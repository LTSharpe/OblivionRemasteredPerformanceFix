// RigImportTemplate ([Fixes] RigImportTemplate, default 1; worker rigimport 2026-10-30, reviews tmplrev / rev130e, guard design
// rig130h; public since 1.3.0, worker rig130i 2026-11-08). Installed at start-up when on (version.dll route), then live 0/1.
//
// What it does. When a character, creature or horse appears, Altar's Control Rig anim node (FAnimNode_ControlRig::
// CacheBones_AnyThread with Altar's prefix: GetController(hierarchy, create) + URigHierarchyController::ImportBones(the mesh's
// reference skeleton, replace existing, remove obsolete)) builds the rig's bone hierarchy from the skeleton, one element at a
// time (0.75-1.5 ms per rig). The first such import of a new rig - Altar's call (recognised by its return address and by the
// controller being the rig hierarchy's), namespace None, replace 1, remove obsolete 1, select 0, on a rig before InitializeVM
// whose hierarchy is in its constructed state (0 elements, topology and metadata version 0, no selection, no previous names) -
// is replaced by URigHierarchy::CopyHierarchy from a stored hierarchy (0.18-0.22 ms): the result of an earlier real import for
// the same rig class and the same reference skeleton content (bone names, parents and reference pose, hashed), copied on the
// game thread right after that import into a rooted URigHierarchy (NewObject with RF_Transient | RF_MarkAsRootSet). The copy is
// used only while the hierarchy's listeners are the ones seen when the template was made (else the game's import runs).
// Equivalent: the lab's verify mode compared the copy with the game's own import over the whole element memory and the
// hierarchy's counters and caches: 33 of 33 rigs element-identical; only allocation bookkeeping differs (below). Measured at
// the Imperial City Waterfront (findings `rigimport`): NPC create units -1.0 to -1.1 ms (-14 %), ABBA.
//
// The guard (whose bug: ours to prevent). CopyHierarchy allocates each element type's elements in ONE block owned by the first
// element (OwnedInstances = count; UE 5.3 RigHierarchy.h NewElement); the game's import allocates one element at a time.
// URigHierarchy::DestroyElement of a block owner frees the whole block, so removing the owner from the middle of a copied
// hierarchy (URigHierarchyController::RemoveElement, RigHierarchyController.cpp:1731-1843, the only middle-of-hierarchy free;
// reached from ImportBones' remove-obsolete loop, RemoveElement(Key), the RigUnit and the script thunks) would free the
// elements still in the hierarchy: a use-after-free that, on a hierarchy we copied, would be our bug. Stock UE has the same
// latent risk on the hierarchies it copies itself (copy-before-construction rigs); this fix does not change those, it only
// counts them ("stock batch-owner removes"). Never seen: 0 owners released in 4,125 lab stats lines (up to 3,752 later imports
// on copied hierarchies per session). The guard redirects RemoveElement's one DestroyElement call: on a hierarchy made by a
// template copy (a set of hierarchy addresses, marked before the copy; a copy is made only when its mark is stored), an element
// with OwnedInstances > 1 gets OwnedInstances 0 first, so DestroyElement returns before the destructors and the free: the block
// is kept (a bounded leak: one block per copied hierarchy, the template's imported bones, ~0.3 MB for a 385-bone humanoid
// with its members' names and caches), counted and logged. After 8 kept blocks template copies switch off for the session.
// The guard stays active when the key is switched off live (the marks outlive the switch). Each mark also records the block
// owners the copy made (elements with OwnedInstances > 1 right after it), and only those are kept: a stale mark (the copied
// hierarchy collected, its address reused by a hierarchy that never passes Altar's empty first import, e.g. a stock
// copy-before-construction rig) leaves that hierarchy's own blocks to the game and off the 8-block count (rev130m RT-N1).
//
// Every address comes from a byte pattern found exactly once, every member offset this code reads is read from the game's own
// code and compared, and the call targets are cross-checked (the guarded call's target is the DestroyElement that Reset_Impl's
// loop calls; the call lies in the RemoveElement that ImportBones calls; RemoveElement's and Reset_Impl's Notify agree). Any
// mismatch installs nothing (the guard first: no copy without it), with one log line. Log tag [rigtmpl]; addresses below are
// build 19115871's (exe+0x...).
#include "common.h"

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace opf {
namespace {

// ---- patterns ----
// Altar's CacheBones exe+0x4AA96B0: +0x15 rig member disp32, +0x24 hierarchy member disp32, +0x28 GetController call, +0x7F the
// ImportBones call (return +0x84).
constexpr char kAltarCbPat[] =
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 50 48 8B 81 ?? ?? ?? ?? 4C 8B F2 48 8B D9 B2 01 48 8B 88 ?? ?? ?? ?? E8 ?? ?? "
    "?? ?? 33 F6 48 8B F8 48 85 C0 74 ?? 4D 8B 06 49 8B 88 ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 89 ?? ?? ?? ?? 48 8B 11 FF 92 ?? ?? ?? ?? "
    "40 88 74 24 38 4C 8D 4C 24 70 40 88 74 24 30 48 8D 54 24 40 C6 44 24 28 01 4C 8B C0 48 8B CF C6 44 24 20 01 48 89 74 24 70 E8";
// UControlRig::Execute's construction branch: +0x27 CopyHierarchy (exe+0x4AB1950), +0x33 Reset_Impl(true) (exe+0x4ACEC00)
constexpr char kCopySitePat[] =
    "48 8D 8E ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 1E 48 8D 8E ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 74 0D 48 8B D0 48 8B CE E8 ?? ?? ?? "
    "?? EB 0A B2 01 48 8B CE E8";
// URigHierarchy::GetController's NewObject: +0x0D FStaticConstructObjectParameters(const UClass*), +0x38
// StaticConstructObject_Internal; the flags store (params + 0x18, RF_Transient) is part of the pattern
constexpr char kNewObjPat[] =
    "E8 ?? ?? ?? ?? 48 8B D0 48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 8D 4C 24 30 48 89 74 24 38 48 89 5C 24 40 C7 44 24 48 40 00 00 00 48 89 "
    "7C 24 58 40 88 7C 24 50 48 89 7C 24 60 E8";
constexpr char kImportPrologue[] = "40 55 53 56 41 54";  // ImportBones exe+0x4B4B190: push rbp/rbx/rsi/r12 (6)
// RemoveElement(FRigBaseElement*) exe+0x4B5BF10, its tail at exe+0x4B5C87B: +0x03 controller bSuspendAllNotifications disp8,
// +0x17 Notify(HierarchyReset), +0x2C THE GUARDED CALL: Hierarchy->DestroyElement(InElement) (rcx = hierarchy, rdx = &element)
constexpr char kRemoveTailPat[] =
    "41 80 7F ?? 00 75 15 49 8B CC E8 ?? ?? ?? ?? 45 33 C0 B2 06 48 8B C8 E8 ?? ?? ?? ?? 49 8B CC E8 ?? ?? ?? ?? 48 8D 54 24 20 48 8B "
    "C8 E8 ?? ?? ?? ?? 49 8B CC E8";
// RemoveElement's entry (null element -> false; the last byte: the controller's weak Hierarchy disp8)
constexpr char kRemoveHeadPat[] =
    "40 55 41 57 48 8D 6C 24 E8 48 81 EC 18 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 E8 48 89 4C 24 70 4C 8B F9 48 89 54 24 20 "
    "48 85 D2 75 07 32 C0 E9 ?? ?? ?? ?? 48 89 9C 24 40 01 00 00 48 89 B4 24 10 01 00 00 48 89 BC 24 08 01 00 00 4C 89 A4 24 00 01 00 "
    "00 4C 8D 61 ??";
// ImportBones' remove-obsolete loop (ImportBones+0xE81): +0x06 Elements num disp8, +0x0F Elements disp8, +0x1C RemoveElement call
constexpr char kImportRemovePat[] = "85 C0 78 1F 3B 43 ?? 7D 1A 48 63 C8 48 8B 43 ?? 48 8B 14 C8 48 85 D2 74 0A 49 8B CD E8";
// DestroyElement exe+0x4AB3EF0: +0x14 OwnedInstances disp32, +0x24 element type disp8
constexpr char kDestroyHeadPat[] =
    "48 89 5C 24 20 41 57 48 83 EC 20 48 8B 1A 4C 8B FA 48 63 83 ?? ?? ?? ?? 85 C0 0F 84 ?? ?? ?? ?? 44 0F B6 43 ?? 41 8D 48 FF 83 F9 "
    "1F 0F 87";
// Reset_Impl+0x1B: +0x07 TopologyVersion (u32 store: TopologyVersion u16 + MetadataVersion u16) disp8, +0x15 Elements num disp8,
// +0x21 Elements disp8, +0x2F the reverse loop's DestroyElement call
constexpr char kResetHeadPat[] =
    "C6 41 ?? 01 44 89 79 ?? 48 8B F1 84 D2 0F 84 ?? ?? ?? ?? 8B 5E ?? 83 EB 01 78 24 48 63 FB 4C 8D 76 ?? 48 C1 E7 03 49 8B 16 48 8B CE "
    "48 03 D7 E8";
// Reset_Impl+0x1D2: +0x03 OrderedSelection disp32 (num at +8), +0x2A Notify(HierarchyReset)
constexpr char kResetSelPat[] =
    "48 8D 8E ?? ?? ?? ?? 8B 41 0C 44 89 79 08 85 C0 79 07 33 D2 E8 ?? ?? ?? ?? 44 38 3D ?? ?? ?? ?? 75 0D 45 33 C0 B2 06 48 8B CE E8";
// GetController+0x1D: +0x13 the hierarchy's controller member disp32
constexpr char kGetCtrlPat[] = "80 B9 ?? ?? ?? ?? 00 48 8B F1 0F 84 ?? ?? ?? ?? 48 8B 81 ?? ?? ?? ?? 48 85 C0 0F 85";
// CopyHierarchy+0x84E: PreviousNameMap +0x1E0 (its element array's num at +0x1E8); literal (the wildcard form is not unique)
constexpr char kCopyPrevPat[] = "49 8D 97 E0 01 00 00 49 8D 8E E0 01 00 00 E8";
// UControlRig::Execute+0x41: +0x10 bRequiresInitExecution disp32
constexpr char kExecInitPat[] = "FF 90 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 80 BF ?? ?? ?? ?? 00 C6 44 24 30 00 74";
// URigHierarchy::Notify+0x108: +0x03 ModifiedEvent's invocation list disp8 (num at +8), +0x11 ModifiedEventDynamic num disp8
constexpr char kNotifyEvtPat[] = "48 8D 4F ?? 4C 8B CE 4C 8B C7 E8 ?? ?? ?? ?? 83 7F ?? 00 7E";
// IncrementTopologyVersion exe+0x4AC42F0: inc word [rcx+TopologyVersion]
constexpr char kIncTopoPat[] = "66 FF 41 ?? 48 81 C1 ?? ?? ?? ?? E9";
// ImportBones+0x15B / +0x180 / +0x758: FReferenceSkeleton FinalRefBoneInfo num, data (FMeshBoneInfo stride 12), FinalRefBonePose
// data (FTransform stride 0x60)
constexpr char kImportSkelPat[] = "45 39 7E ?? 0F 8E ?? ?? ?? ?? 4D 8B EF 4C 89 7D 20 4C 89 7D D8 49 8B F7 49 8B FF";
constexpr char kImportInfoPat[] = "49 8B 5E ?? 4A 8D 04 6D 00 00 00 00 48 8B 4D 88 49 03 C5";
constexpr char kImportPosePat[] = "49 03 CD 49 8B 46 ?? 48 C1 E1 05 0F 28 04 01";

// ---- member offsets (each compared with the game's code at start-up) ----
constexpr size_t kNodeRig = 0x238;        // FAnimNode_ControlRig::ControlRig
constexpr size_t kRigHier = 0x398;        // UControlRig::DynamicHierarchy
constexpr size_t kRigNeedsInit = 0x2F8;   // URigVMHost::bRequiresInitExecution
constexpr size_t kObjClass = 0x10;        // UObjectBase::ClassPrivate
constexpr size_t kHierListeners = 0x28;   // ModifiedEvent invocation list (num +0x30, entries 0x10 with the instance first)
constexpr size_t kHierDynNum = 0x48;      // ModifiedEventDynamic num
constexpr size_t kHierTopo = 0x68;        // TopologyVersion u16, MetadataVersion u16 at +0x6A
constexpr size_t kHierElems = 0x70;       // TArray<FRigBaseElement*> Elements (num +0x78)
constexpr size_t kHierNum = 0x78;
constexpr size_t kHierSelNum = 0x178;     // OrderedSelection num
constexpr size_t kHierCtrl = 0x180;       // the hierarchy's controller
constexpr size_t kHierPrevNames = 0x1E8;  // PreviousNameMap's element array num
constexpr size_t kCtrlWeakHier = 0x2C;    // URigHierarchyController::Hierarchy (weak)
constexpr size_t kCtrlListenNum = 0x40;   // its ModifiedEvent num: the member between Hierarchy (+0x2C) and +0x50 (0x38..0x50)
constexpr size_t kCtrlSuspend = 0x50;     // bSuspendAllNotifications
constexpr size_t kElemType = 0x08;
constexpr size_t kElemOwned = 0xBC;       // OwnedInstances
constexpr size_t kSkelInfo = 0x20, kSkelInfoNum = 0x28, kSkelPose = 0x30, kSkelPoseNum = 0x38;

constexpr int kKeepCap = 8;        // kept blocks before template copies switch off for the session
constexpr size_t kMaxTemplates = 64;

using CacheBonesFn = void (*)(uint8_t* node, uint8_t** ctx);
using ImportFn = uint64_t* (*)(uint8_t* ctrl, uint64_t* ret, const uint8_t* skel, const uint64_t* ns, int replace, int removeObsolete,
                               int select, int undo);
using CopyHierFn = void (*)(uint8_t* dst, uint8_t* src);
using ResetImplFn = void (*)(uint8_t* hier, int resetElements);
using ParamsCtorFn = void* (*)(void* params, const void* cls);
using ConstructFn = uint8_t* (*)(const void* params);
using DestroyFn = void (*)(uint8_t* hier, uint8_t** pElem);
using GetCtrlFn = uint8_t* (*)(uint8_t* hier, bool create);
using RemoveFn = bool (*)(uint8_t* ctrl, uint8_t* elem);

CacheBonesFn g_origCb = nullptr;
ImportFn g_origImport = nullptr;
CopyHierFn g_copyHier = nullptr;
ResetImplFn g_resetImpl = nullptr;
ParamsCtorFn g_paramsCtor = nullptr;
ConstructFn g_construct = nullptr;
DestroyFn g_destroy = nullptr;
GetCtrlFn g_getCtrl = nullptr;
RemoveFn g_remove = nullptr;
const uint8_t* g_importRet = nullptr;  // the return address of Altar's ImportBones call
std::atomic<int> g_tmplMode{0};
std::atomic<bool> g_capHit{false};
bool g_installed = false;
bool g_guardOn = false;
bool g_testing = false;  // self-test: Set acts without the hooks
DWORD g_gameThread = 0;
uint64_t g_freq = 1;
thread_local int t_cbDepth = 0;
thread_local uint8_t* t_cbRig = nullptr;

struct Listeners {  // who would hear the hierarchy's notifications
    int native = -1, dynamic = -1, ctrl = -1;
    uint64_t vt[4] = {};  // the native delegates' instance vtables
    bool operator==(const Listeners& o) const
    {
        return native == o.native && dynamic == o.dynamic && ctrl == o.ctrl && !memcmp(vt, o.vt, sizeof vt);
    }
};
struct Template {
    const void* cls;
    uint64_t skelHash;
    uint8_t* hier;  // rooted URigHierarchy
    int bones;
    Listeners lis;
};
SRWLOCK g_tmplLock = SRWLOCK_INIT;
std::vector<Template> g_templates;
// Hierarchies made by a template copy (batch-allocated), each with the block owners its copy made. A mark is dropped when
// Altar's first import finds that address empty again (a new hierarchy there); a stale mark acts only on an element that is
// one of the recorded owners (the copy's own blocks), so another hierarchy's blocks at a reused address stay stock.
SRWLOCK g_markLock = SRWLOCK_INIT;
std::unordered_map<const uint8_t*, std::vector<const uint8_t*>> g_marks;
std::atomic<size_t> g_markCount{0};
size_t g_maxMarks = 65536;

std::atomic<long long> g_copies{0}, g_copyTicks{0}, g_imports{0}, g_importTicks{0};
std::atomic<long long> g_removesOnCopied{0}, g_blocksKept{0}, g_elemsKept{0}, g_stockOwnerRemoves{0};
std::atomic<long long> g_listenerMismatch{0}, g_notClean{0}, g_notAltar{0}, g_marksFull{0};

uint64_t Now()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

bool IsMarked(const uint8_t* hier)
{
    if (!hier || g_markCount.load(std::memory_order_acquire) == 0) return false;
    AcquireSRWLockShared(&g_markLock);
    const bool m = g_marks.count(hier) != 0;
    ReleaseSRWLockShared(&g_markLock);
    return m;
}
// Whether elem is one of the block owners recorded for the marked hierarchy (the guard's test).
bool IsCopiedOwner(const uint8_t* hier, const uint8_t* elem)
{
    AcquireSRWLockShared(&g_markLock);
    const auto it = g_marks.find(hier);
    bool m = false;
    if (it != g_marks.end())
        for (const uint8_t* o : it->second) m = m || o == elem;
    ReleaseSRWLockShared(&g_markLock);
    return m;
}
bool Mark(const uint8_t* hier)  // false: the table is full
{
    AcquireSRWLockExclusive(&g_markLock);
    bool ok = g_marks.count(hier) != 0;
    if (ok) g_marks[hier].clear();  // a new copy at a marked address: its owners are recorded after it
    if (!ok && g_marks.size() < g_maxMarks) {
        g_marks.emplace(hier, std::vector<const uint8_t*>{});
        ok = true;
    }
    g_markCount.store(g_marks.size(), std::memory_order_release);
    ReleaseSRWLockExclusive(&g_markLock);
    return ok;
}
// After the copy: the elements that own a block (OwnedInstances > 1; one per element type the template holds).
constexpr int kMaxOwners = 32;
int ReadOwners(const uint8_t* hier, const uint8_t** out)
{
    int k = 0;
    __try {
        const uint8_t* const* elems = *reinterpret_cast<const uint8_t* const* const*>(hier + kHierElems);
        const int n = *reinterpret_cast<const int*>(hier + kHierNum);
        for (int i = 0; elems && i < n && i < 65536 && k < kMaxOwners; ++i)
            if (elems[i] && *reinterpret_cast<const int*>(elems[i] + kElemOwned) > 1) out[k++] = elems[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return k;
}
void RecordOwners(const uint8_t* hier)
{
    const uint8_t* buf[kMaxOwners];
    const int k = ReadOwners(hier, buf);
    std::vector<const uint8_t*> owners(buf, buf + k);
    AcquireSRWLockExclusive(&g_markLock);
    const auto it = g_marks.find(hier);
    if (it != g_marks.end()) it->second = std::move(owners);
    ReleaseSRWLockExclusive(&g_markLock);
}
void Unmark(const uint8_t* hier)
{
    if (!IsMarked(hier)) return;
    AcquireSRWLockExclusive(&g_markLock);
    g_marks.erase(hier);
    g_markCount.store(g_marks.size(), std::memory_order_release);
    ReleaseSRWLockExclusive(&g_markLock);
}

// The guard: RemoveElement's call of URigHierarchy::DestroyElement(FRigBaseElement*& InElement) (rcx = hierarchy, rdx = &element).
void RemoveDestroyHook(uint8_t* hier, uint8_t** pElem)
{
    uint8_t* const e = pElem ? *pElem : nullptr;
    if (e) {
        int* const owned = reinterpret_cast<int*>(e + kElemOwned);
        if (IsMarked(hier)) {
            ++g_removesOnCopied;
            const int n = *owned;
            if (n > 1 && !IsCopiedOwner(hier, e)) {
                ++g_stockOwnerRemoves;  // not a block our copy made (a stale mark): stock behaviour, counted only
            } else if (n > 1) {  // a batch owner: its free would take the block from under the elements still in the hierarchy
                *owned = 0;  // DestroyElement then returns before the destructors and the free
                const long long k = ++g_blocksKept;
                g_elemsKept += n;
                if (k <= 3 || (k & (k - 1)) == 0)
                    Log("[rigtmpl] a batch owner was removed from a copied hierarchy: its block of %d elements is kept (not freed); "
                        "%lld block(s) kept so far", n, k);
                if (k >= kKeepCap && !g_capHit.exchange(true)) {
                    g_tmplMode.store(0);
                    Log("[rigtmpl] %lld blocks kept: template copies off for this session (the game's import runs; the guard stays)", k);
                }
            }
        } else if (*owned > 1) {
            ++g_stockOwnerRemoves;  // a block the game itself copied: stock behaviour, counted only
        }
    }
    g_destroy(hier, pElem);
}

// FReferenceSkeleton (UE 5.3, no editor data): FinalRefBoneInfo TArray<FMeshBoneInfo{FName, int32 parent}>, FinalRefBonePose
// TArray<FTransform> (what ImportBones reads). 0 = not usable (no copy).
uint64_t SkelHash(const uint8_t* skel, int* bones)
{
    uint64_t h = 1469598103934665603ull;
    __try {
        const uint8_t* info = *reinterpret_cast<const uint8_t* const*>(skel + kSkelInfo);
        const int ni = *reinterpret_cast<const int*>(skel + kSkelInfoNum);
        const uint8_t* pose = *reinterpret_cast<const uint8_t* const*>(skel + kSkelPose);
        const int np = *reinterpret_cast<const int*>(skel + kSkelPoseNum);
        if (ni <= 0 || ni > 4096 || np != ni || !info || !pose) return 0;
        for (int i = 0; i < ni * 12; ++i) h = (h ^ info[i]) * 1099511628211ull;
        for (int i = 0; i < np; ++i) {
            const uint8_t* t = pose + static_cast<size_t>(i) * 0x60;
            for (int b = 0; b < 32; ++b) h = (h ^ t[b]) * 1099511628211ull;   // rotation
            for (int b = 32; b < 56; ++b) h = (h ^ t[b]) * 1099511628211ull;  // translation xyz
            for (int b = 64; b < 88; ++b) h = (h ^ t[b]) * 1099511628211ull;  // scale xyz
        }
        *bones = ni;
        return h ^ static_cast<uint64_t>(ni);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// A rooted URigHierarchy: the game's own NewObject path (GetController's), Outer = the transient package (the params
// constructor's default), Name None (a unique one is made), RF_Transient | RF_MarkAsRootSet. Game thread only.
uint8_t* NewRootedHierarchy(const void* hierClass)
{
    if (!hierClass || GetCurrentThreadId() != g_gameThread || !g_paramsCtor || !g_construct) return nullptr;
    alignas(16) uint8_t params[0x100] = {};
    g_paramsCtor(params, hierClass);
    *reinterpret_cast<uint32_t*>(params + 0x18) = 0x40 | 0x80;
    return g_construct(params);
}
// The constructed state of a hierarchy: what the import and a copy both start from in the verified cases.
bool CleanStart(const uint8_t* hier)
{
    return *reinterpret_cast<const int*>(hier + kHierNum) == 0 && *reinterpret_cast<const uint16_t*>(hier + kHierTopo) == 0 &&
           *reinterpret_cast<const uint16_t*>(hier + kHierTopo + 2) == 0 && *reinterpret_cast<const int*>(hier + kHierSelNum) == 0 &&
           *reinterpret_cast<const int*>(hier + kHierPrevNames) == 0;
}
Listeners ReadListeners(const uint8_t* hier)
{
    Listeners l;
    l.native = *reinterpret_cast<const int*>(hier + kHierListeners + 8);
    l.dynamic = *reinterpret_cast<const int*>(hier + kHierDynNum);
    const uint8_t* ctrl = *reinterpret_cast<uint8_t* const*>(hier + kHierCtrl);
    l.ctrl = ctrl ? *reinterpret_cast<const int*>(ctrl + kCtrlListenNum) : -1;
    const uint8_t* list = *reinterpret_cast<uint8_t* const*>(hier + kHierListeners);
    for (int i = 0; i < l.native && i < 4 && list; ++i) {
        const uint8_t* inst = *reinterpret_cast<uint8_t* const*>(list + i * 0x10);
        l.vt[i] = inst ? *reinterpret_cast<const uint64_t*>(inst) : 0;
    }
    return l;
}
// The copy, only with its mark stored (the guard needs it); a full table -> false (the game's import runs).
bool TryCopy(uint8_t* hier, uint8_t* tmpl)
{
    if (!Mark(hier)) {
        if (g_marksFull.fetch_add(1) == 0)
            Log("[rigtmpl] the copied-hierarchy table is full (%zu): new rigs get the game's import from now on", g_maxMarks);
        return false;
    }
    const uint64_t t0 = Now();
    g_copyHier(hier, tmpl);
    RecordOwners(hier);
    g_copyTicks += static_cast<long long>(Now() - t0);
    ++g_copies;
    return true;
}

void CacheBonesHook(uint8_t* node, uint8_t** ctx)
{
    uint8_t* const rig = node ? *reinterpret_cast<uint8_t* const*>(node + kNodeRig) : nullptr;
    uint8_t* const prevRig = t_cbRig;
    ++t_cbDepth;
    t_cbRig = rig;
    g_origCb(node, ctx);
    t_cbRig = prevRig;
    --t_cbDepth;
}

uint64_t* ImportHook(uint8_t* ctrl, uint64_t* ret, const uint8_t* skel, const uint64_t* ns, int replace, int removeObsolete, int select,
                     int undo)
{
    uint8_t* const rig = t_cbRig;
    if (t_cbDepth <= 0 || !rig || !skel || !ret || !ctrl || (ns && *ns) || !(replace & 0xFF) || !(removeObsolete & 0xFF) || (select & 0xFF) ||
        !rig[kRigNeedsInit])
        return g_origImport(ctrl, ret, skel, ns, replace, removeObsolete, select, undo);
    uint8_t* const hier = *reinterpret_cast<uint8_t* const*>(rig + kRigHier);
    // exactly Altar's call: its return address, and the controller is the rig hierarchy's
    if (static_cast<const uint8_t*>(_ReturnAddress()) != g_importRet || !hier || *reinterpret_cast<uint8_t* const*>(hier + kHierCtrl) != ctrl) {
        ++g_notAltar;
        return g_origImport(ctrl, ret, skel, ns, replace, removeObsolete, select, undo);
    }
    if (*reinterpret_cast<const int*>(hier + kHierNum) != 0) return g_origImport(ctrl, ret, skel, ns, replace, removeObsolete, select, undo);
    // an empty hierarchy holds no batch block any more: a mark at this address belonged to an earlier hierarchy
    Unmark(hier);
    const int mode = g_tmplMode.load(std::memory_order_relaxed);
    if (!mode) return g_origImport(ctrl, ret, skel, ns, replace, removeObsolete, select, undo);
    if (!CleanStart(hier)) {
        ++g_notClean;
        return g_origImport(ctrl, ret, skel, ns, replace, removeObsolete, select, undo);
    }
    int bones = 0;
    const uint64_t sh = SkelHash(skel, &bones);
    const void* cls = *reinterpret_cast<const void* const*>(rig + kObjClass);
    const bool gt = GetCurrentThreadId() == g_gameThread;
    uint8_t* tmpl = nullptr;
    Listeners tlis;
    AcquireSRWLockShared(&g_tmplLock);
    for (const Template& t : g_templates)
        if (sh && t.cls == cls && t.skelHash == sh) {
            tmpl = t.hier;
            tlis = t.lis;
            break;
        }
    const bool full = g_templates.size() >= kMaxTemplates;
    ReleaseSRWLockShared(&g_tmplLock);
    const Listeners lis = ReadListeners(hier);
    const double toMs = 1000.0 / static_cast<double>(g_freq);
    if (mode == 1 && tmpl) {
        if (!(lis == tlis)) {
            ++g_listenerMismatch;
        } else {
            [[maybe_unused]] const long long c0 = g_copyTicks.load();
            if (TryCopy(hier, tmpl)) {
                ret[0] = 0;  // Altar's caller only frees the AddedBones array: return it empty
                ret[1] = 0;
                return ret;
            }
        }
    }
    const uint64_t t0 = Now();
    uint64_t* const r = g_origImport(ctrl, ret, skel, ns, replace, removeObsolete, select, undo);
    const uint64_t t1 = Now();
    ++g_imports;
    g_importTicks += static_cast<long long>(t1 - t0);
    [[maybe_unused]] const double importMs = static_cast<double>(t1 - t0) * toMs;
    if (!gt || !sh) return r;
    if (!tmpl) {
        if (full) return r;
        uint8_t* const t = NewRootedHierarchy(*reinterpret_cast<const void* const*>(hier + kObjClass));
        if (!t || !CleanStart(t)) {
            Log("[rigtmpl] a template was not made (new hierarchy %p not empty or not created)", static_cast<void*>(t));
            return r;
        }
        g_copyHier(t, hier);
        AcquireSRWLockExclusive(&g_tmplLock);
        g_templates.push_back({cls, sh, t, bones, lis});
        ReleaseSRWLockExclusive(&g_tmplLock);
        return r;
    }
    return r;
}

// One entry hook: region[0,16) jmp [hook]; region[16,64) trampoline = the prologue (or a jump to an earlier detour) + jmp back.
uint8_t* Detour(uint8_t* site, size_t prologue, void* hook, const char* what)
{
    uint8_t* const region = AllocNearGame(64);
    if (!region) {
        Log("[rigtmpl] %s: no memory near the game's code", what);
        return nullptr;
    }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &hook, sizeof hook);
    uint8_t* const tramp = region + 16;
    if (site[0] == 0xE9) {  // already detoured by another module: chain to it
        uint8_t* const target = RipTarget(site, 1, 5);
        memcpy(tramp, jmpAbs, sizeof jmpAbs);
        memcpy(tramp + sizeof jmpAbs, &target, sizeof target);
    } else {
        memcpy(tramp, site, prologue);
        memcpy(tramp + prologue, jmpAbs, sizeof jmpAbs);
        uint8_t* const back = site + prologue;
        memcpy(tramp + prologue + sizeof jmpAbs, &back, sizeof back);
    }
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    const int64_t rel = region - (site + 5);
    if (rel != static_cast<int32_t>(rel)) {
        Log("[rigtmpl] %s: out of reach", what);
        return nullptr;
    }
    return tramp;
}
bool PatchJump(uint8_t* site, uint8_t* region)
{
    uint8_t bytes[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(region - (site + 5));
    memcpy(bytes + 1, &rel32, 4);
    return WriteCode(site, bytes);
}
// A rel32 call redirected to hook through a "jmp [rip+0]" stub near the game's code (start-up only).
bool RedirectCall(uint8_t* call, void* hook)
{
    uint8_t* const stub = AllocNearGame(16);
    if (!stub) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(stub, jmpAbs, sizeof jmpAbs);
    memcpy(stub + sizeof jmpAbs, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const int64_t rel = stub - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}

// ---- discovery (also the self-test's) ----
struct Sites {
    uint8_t* cb = nullptr;
    uint8_t* import = nullptr;
    uint8_t* copyHier = nullptr;
    uint8_t* resetImpl = nullptr;
    uint8_t* paramsCtor = nullptr;
    uint8_t* construct = nullptr;
    uint8_t* getCtrl = nullptr;
    uint8_t* remove = nullptr;
    uint8_t* destroySite = nullptr;  // RemoveElement's DestroyElement call (the guard)
    uint8_t* destroy = nullptr;
    const uint8_t* importRet = nullptr;
    const char* missing = nullptr;
    char detail[160] = {};
};
uint8_t* CallTarget(uint8_t* call) { return call && call[0] == 0xE8 ? RipTarget(call, 1, 5) : nullptr; }
uint32_t D8(const uint8_t* p) { return *p; }
uint32_t D32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
bool Inside(const uint8_t* at, const uint8_t* fn, size_t len) { return at && fn && at > fn && at < fn + len; }

Sites Discover(const GameModule& game)
{
    Sites s;
    auto one = [&](const char* pat) -> uint8_t* {
        const auto v = FindAll(game.text, ParsePattern(pat));
        return v.size() == 1 ? v[0] : nullptr;
    };
    uint8_t* const cb = one(kAltarCbPat);
    uint8_t* const cs = one(kCopySitePat);
    uint8_t* const no = one(kNewObjPat);
    uint8_t* const tail = one(kRemoveTailPat);
    uint8_t* const head = one(kRemoveHeadPat);
    uint8_t* const imRem = one(kImportRemovePat);
    uint8_t* const dHead = one(kDestroyHeadPat);
    uint8_t* const rHead = one(kResetHeadPat);
    uint8_t* const rSel = one(kResetSelPat);
    uint8_t* const gCtl = one(kGetCtrlPat);
    uint8_t* const cPrev = one(kCopyPrevPat);
    uint8_t* const exIn = one(kExecInitPat);
    uint8_t* const nEvt = one(kNotifyEvtPat);
    uint8_t* const incT = one(kIncTopoPat);
    uint8_t* const iSkel = one(kImportSkelPat);
    uint8_t* const iInfo = one(kImportInfoPat);
    uint8_t* const iPose = one(kImportPosePat);
    const struct {
        uint8_t* at;
        const char* what;
    } need[] = {{cb, "Altar's Control Rig CacheBones"},
                {cs, "UControlRig::Execute's construction copy"},
                {no, "URigHierarchy::GetController's NewObject"},
                {tail, "RemoveElement's DestroyElement call"},
                {head, "URigHierarchyController::RemoveElement"},
                {imRem, "ImportBones' remove-obsolete loop"},
                {dHead, "URigHierarchy::DestroyElement"},
                {rHead, "URigHierarchy::Reset_Impl's element loop"},
                {rSel, "URigHierarchy::Reset_Impl's selection reset"},
                {gCtl, "URigHierarchy::GetController's member read"},
                {cPrev, "CopyHierarchy's previous-name map"},
                {exIn, "UControlRig::Execute's init check"},
                {nEvt, "URigHierarchy::Notify's listeners"},
                {incT, "IncrementTopologyVersion"},
                {iSkel, "ImportBones' skeleton read"},
                {iInfo, "ImportBones' bone info read"},
                {iPose, "ImportBones' reference pose read"}};
    for (const auto& n : need)
        if (!n.at) {
            s.missing = n.what;
            return s;
        }
    s.cb = cb;
    s.import = CallTarget(cb + 0x7F);
    s.importRet = cb + 0x84;
    s.getCtrl = CallTarget(cb + 0x28);
    s.copyHier = CallTarget(cs + 0x27);
    s.resetImpl = CallTarget(cs + 0x33);
    s.paramsCtor = CallTarget(no + 0x0D);
    s.construct = CallTarget(no + 0x38);
    s.remove = head;
    s.destroySite = tail + 0x2C;
    s.destroy = CallTarget(s.destroySite);
    if (!s.import || !(MatchesAt(s.import, ParsePattern(kImportPrologue)) || s.import[0] == 0xE9)) {
        s.missing = "URigHierarchyController::ImportBones";
        return s;
    }
    if (!s.copyHier || !s.resetImpl || !s.paramsCtor || !s.construct || !s.getCtrl || !s.destroy) {
        s.missing = "a call target";
        return s;
    }
    // where each piece must be (cross-checks)
    const struct {
        bool ok;
        const char* what;
    } where[] = {
        {s.destroy == dHead && CallTarget(rHead + 0x2F) == dHead, "the guarded call's target is not Reset_Impl's DestroyElement"},
        {CallTarget(imRem + 0x1C) == head && Inside(s.destroySite, head, 0x1000), "the guarded call is not in ImportBones' RemoveElement"},
        {CallTarget(tail + 0x17) && CallTarget(tail + 0x17) == CallTarget(rSel + 0x2A), "RemoveElement's and Reset_Impl's Notify differ"},
        {Inside(nEvt, CallTarget(tail + 0x17), 0x400), "the listener read is not in Notify"},
        {Inside(rHead, s.resetImpl, 0x400) && Inside(rSel, s.resetImpl, 0x400), "the reset reads are not in Reset_Impl"},
        {Inside(gCtl, s.getCtrl, 0x100), "the controller read is not in GetController"},
        {Inside(cPrev, s.copyHier, 0x1000), "the previous-name read is not in CopyHierarchy"},
        {Inside(cs, exIn, 0x1000), "the init check is not in UControlRig::Execute"},
        {Inside(imRem, s.import, 0x2000) && Inside(iSkel, s.import, 0x2000) && Inside(iInfo, s.import, 0x2000) &&
             Inside(iPose, s.import, 0x2000),
         "the skeleton reads are not in ImportBones"}};
    for (const auto& w : where)
        if (!w.ok) {
            s.missing = "a cross-check";
            snprintf(s.detail, sizeof s.detail, "%s", w.what);
            return s;
        }
    // every member offset this code reads, from the game's code
    const struct {
        uint32_t got, want;
        const char* what;
    } pins[] = {{D32(cb + 0x15), kNodeRig, "anim node: ControlRig"},
                {D32(cb + 0x24), kRigHier, "rig: DynamicHierarchy"},
                {D32(exIn + 0x10), kRigNeedsInit, "rig: bRequiresInitExecution"},
                {D8(rHead + 0x07), kHierTopo, "hierarchy: TopologyVersion (Reset_Impl)"},
                {D8(incT + 0x03), kHierTopo, "hierarchy: TopologyVersion (IncrementTopologyVersion)"},
                {D8(rHead + 0x21), kHierElems, "hierarchy: Elements (Reset_Impl)"},
                {D8(rHead + 0x15), kHierNum, "hierarchy: Elements num (Reset_Impl)"},
                {D8(imRem + 0x0F), kHierElems, "hierarchy: Elements (ImportBones)"},
                {D8(imRem + 0x06), kHierNum, "hierarchy: Elements num (ImportBones)"},
                {D32(rSel + 0x03) + 8, kHierSelNum, "hierarchy: OrderedSelection num"},
                {D32(gCtl + 0x13), kHierCtrl, "hierarchy: controller"},
                {D32(cPrev + 0x03) + 8, kHierPrevNames, "hierarchy: PreviousNameMap num"},
                {D8(nEvt + 0x03), kHierListeners, "hierarchy: ModifiedEvent"},
                {D8(nEvt + 0x11), kHierDynNum, "hierarchy: ModifiedEventDynamic num"},
                {D8(head + ParsePattern(kRemoveHeadPat).size() - 1), kCtrlWeakHier, "controller: Hierarchy"},
                {D8(tail + 0x03), kCtrlSuspend, "controller: bSuspendAllNotifications"},
                {D32(dHead + 0x14), kElemOwned, "element: OwnedInstances"},
                {D8(dHead + 0x24), kElemType, "element: type"},
                {D8(iSkel + 0x03), kSkelInfoNum, "skeleton: FinalRefBoneInfo num"},
                {D8(iInfo + 0x03), kSkelInfo, "skeleton: FinalRefBoneInfo"},
                {D8(iPose + 0x06), kSkelPose, "skeleton: FinalRefBonePose"}};
    for (const auto& p : pins)
        if (p.got != p.want) {
            s.missing = "the member offsets this code reads";
            snprintf(s.detail, sizeof s.detail, "%s at +0x%X, expected +0x%X", p.what, p.got, p.want);
            return s;
        }
    return s;
}

}  // namespace

void SetRigImportTemplate(int mode)
{
    const int want = mode ? 1 : 0;
    if (!g_installed && !g_testing) return;
    if (want && g_capHit.load()) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            Log("[rigtmpl] RigImportTemplate %d: stays off for this session (the kept-block limit was reached)", want);
        }
        return;
    }
    const int old = g_tmplMode.exchange(want);
    if (old != want && !g_testing)
        Log("[rigtmpl] RigImportTemplate %d (%s)", want,
            want == 1 ? "the first bone import of a new rig is a copy of a stored, identical import"
            : want == 2 ? "verify: the game's import runs, a template copy is compared with it"
                        : "the game's import runs; the guard stays");
}

bool RigImportTemplateDiscover(const GameModule& game, const char** missing, const char** detail)
{
    static Sites s;  // the detail text outlives the call
    s = Discover(game);
    if (missing) *missing = s.missing;
    if (detail) *detail = s.detail;
    return !s.missing;
}

bool InstallRigImportTemplate(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!beforeGameCode) {
        Log("[rigtmpl] RigImportTemplate: only installed while the game starts (version.dll route), not installed");
        return false;
    }
    g_gameThread = GetCurrentThreadId();  // DllMain of the statically imported version.dll runs on the main (= game) thread
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart ? static_cast<uint64_t>(f.QuadPart) : 1;
    const GameModule& game = Game();
    const Sites s = Discover(game);
    if (s.missing) {
        Log("[rigtmpl] RigImportTemplate: %s not found as expected in this game build%s%s: off", s.missing, s.detail[0] ? " - " : "",
            s.detail);
        return false;
    }
    g_copyHier = reinterpret_cast<CopyHierFn>(s.copyHier);
    g_resetImpl = reinterpret_cast<ResetImplFn>(s.resetImpl);
    g_paramsCtor = reinterpret_cast<ParamsCtorFn>(s.paramsCtor);
    g_construct = reinterpret_cast<ConstructFn>(s.construct);
    g_getCtrl = reinterpret_cast<GetCtrlFn>(s.getCtrl);
    g_remove = reinterpret_cast<RemoveFn>(s.remove);
    g_importRet = s.importRet;
    // the guard first: no copy without it
    g_destroy = reinterpret_cast<DestroyFn>(s.destroy);
    if (!RedirectCall(s.destroySite, reinterpret_cast<void*>(&RemoveDestroyHook))) {
        Log("[rigtmpl] RigImportTemplate: RemoveElement's DestroyElement call could not be guarded: off");
        return false;
    }
    g_guardOn = true;
    uint8_t* const cbTramp = Detour(s.cb, 5, reinterpret_cast<void*>(&CacheBonesHook), "the Control Rig node's CacheBones");
    uint8_t* const imTramp = Detour(s.import, 6, reinterpret_cast<void*>(&ImportHook), "ImportBones");
    if (!cbTramp || !imTramp) return false;
    g_origCb = reinterpret_cast<CacheBonesFn>(cbTramp);
    g_origImport = reinterpret_cast<ImportFn>(imTramp);
    const bool chained = s.import[0] == 0xE9;
    if (!PatchJump(s.import, imTramp - 16) || !PatchJump(s.cb, cbTramp - 16)) {
        Log("[rigtmpl] RigImportTemplate: could not patch");
        return false;
    }
    g_installed = true;
    auto rva = [&](const uint8_t* p) { return static_cast<unsigned>(p - game.base); };
    Log("[rigtmpl] installed: node CacheBones exe+0x%X, ImportBones exe+0x%X%s, CopyHierarchy exe+0x%X; guard: RemoveElement "
        "exe+0x%X's DestroyElement call exe+0x%X -> exe+0x%X (offsets checked)",
        rva(s.cb), rva(s.import), chained ? " (chained)" : "", rva(s.copyHier), rva(s.remove), rva(s.destroySite), rva(s.destroy));
    return true;
}

void LogRigImportTemplateStats()
{
    if (!g_guardOn) return;
    static long long s_last[11] = {-1};
    size_t templates;
    AcquireSRWLockShared(&g_tmplLock);
    templates = g_templates.size();
    ReleaseSRWLockShared(&g_tmplLock);
    const long long now[11] = {g_copies.load(), g_imports.load(), static_cast<long long>(templates), static_cast<long long>(g_markCount.load()),
                               g_removesOnCopied.load(), g_blocksKept.load(), g_stockOwnerRemoves.load(), g_listenerMismatch.load(),
                               g_notClean.load(), g_notAltar.load(), g_marksFull.load()};
    if (!memcmp(now, s_last, sizeof now)) return;
    memcpy(s_last, now, sizeof now);
    const double toMs = 1000.0 / static_cast<double>(g_freq);
    const long long c = now[0];
    Log("[rigtmpl] copies %lld (%.3f ms each), game imports %lld, templates %lld, copied hierarchies %lld | removes on copied hierarchies "
        "%lld (expect 0), batch blocks kept %lld (%lld elements), stock batch-owner removes %lld | listener mismatches %lld, not a clean "
        "start %lld, not Altar's call %lld, table full %lld%s",
        c, c ? static_cast<double>(g_copyTicks.load()) * toMs / static_cast<double>(c) : 0.0, now[1], now[2], now[3], now[4], now[5],
        g_elemsKept.load(), now[6], now[7], now[8], now[9], now[10], g_capHit.load() ? " (copies off: kept-block limit)" : "");
}

// ---- self-test ----
void RigTmplTestBegin(void* fakeDestroy, void* fakeCopy, size_t maxMarks)
{
    g_testing = true;
    AcquireSRWLockExclusive(&g_markLock);
    g_marks.clear();
    g_markCount.store(0);
    ReleaseSRWLockExclusive(&g_markLock);
    for (auto* a : {&g_copies, &g_copyTicks, &g_imports, &g_importTicks, &g_removesOnCopied, &g_blocksKept, &g_elemsKept,
                    &g_stockOwnerRemoves, &g_listenerMismatch, &g_notClean, &g_notAltar, &g_marksFull})
        a->store(0);
    g_capHit.store(false);
    g_destroy = reinterpret_cast<DestroyFn>(fakeDestroy);
    g_copyHier = reinterpret_cast<CopyHierFn>(fakeCopy);
    g_maxMarks = maxMarks;
    g_tmplMode.store(1);
}
void RigTmplTestEnd()
{
    RigTmplTestBegin(nullptr, nullptr, 65536);
    g_tmplMode.store(0);
    g_testing = false;
}
void RigTmplTestDestroy(uint8_t* hier, uint8_t** pElem) { RemoveDestroyHook(hier, pElem); }
bool RigTmplTestCopy(uint8_t* hier, uint8_t* tmpl) { return TryCopy(hier, tmpl); }
void RigTmplTestNewImport(const uint8_t* hier) { Unmark(hier); }
RigTmplTestCounts RigTmplTestGet()
{
    RigTmplTestCounts c;
    c.removesOnCopied = g_removesOnCopied.load();
    c.blocksKept = g_blocksKept.load();
    c.elemsKept = g_elemsKept.load();
    c.stockOwnerRemoves = g_stockOwnerRemoves.load();
    c.copies = g_copies.load();
    c.marksFull = g_marksFull.load();
    c.mode = g_tmplMode.load();
    c.marks = g_markCount.load();
    return c;
}


}  // namespace opf
