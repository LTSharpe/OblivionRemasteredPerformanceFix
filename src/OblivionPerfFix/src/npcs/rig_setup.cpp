// RigVmHashMemo and RigLogicMapCache: two identical-result shortcuts in the Control Rig set-up of every NPC's animation
// (docs/research.md).
//
// RigVmHashMemo. Every UControlRig an NPC's anim graph creates (2-3 per humanoid: body, head, hair rigs) hashes its
// RigVM four times: UControlRig::PostInitInstance and URigVMHost::InstantiateVMFromCDO each do
// VM->SetVMHash(VM->ComputeVMHash()) on the instance's copy and ComputeAndUpdateCDOHash(CDO) on the class default
// object's VM (UE 5.3 ControlRig.cpp:2826, RigVMHost.cpp:1024/1108). URigVM::ComputeVMHash (RigVM.cpp:327; exe
// 0x49A95D0 in build 19115871) is a pure function of: the function-name list (FName.ToString hashed), the byte code
// (FRigVMByteCode::GetByteCodeHash: entry names and instruction indices, every instruction's operator and operands,
// branch infos; the instruction walk depends on the bByteCodeIsAligned flag and, when set, on the byte array's address
// modulo the op alignments, all <= 16), the external variables' Name and TypeName, and the literal / work memory
// storage classes' URigVMMemoryStorageGeneratorClass::GetMemoryHash (cached in the class). The memo keys on all of
// these, compared in full (byte arrays memcmp'd, the memory parts computed exactly as the stock code does by calling
// the game's StaticClass / GetMemoryHash), so a hit returns exactly what the stock function would return. Same inputs ->
// same output; the only difference is the work not done (FString allocations, CRCs, the instruction walk).
// Entry detour of ComputeVMHash (prologue + 0x9E3779B9 constant; the whole function's bytes are checked).
//
// RigLogicMapCache. The head rig's FRigUnit_RigLogic (RigLogic plugin, RigUnit_RigLogic.cpp:85) on its first execution
// maps the DNA's joint, control, blend shape and animated-map names to hierarchy indices: per DNA entry an FString from
// the reader, string concatenation / ConstructCurveName, an FName from the string (name-pool lock), then the hierarchy's
// index lookup. ~1.3 ms per head on a worker the game thread waits for (appearance units, measured 2026-10-20).
// The cache (content-keyed) holds, per map, the FNames the stock function created (captured at the
// function's own FName call site, so they are stock's names, not a re-implementation of the string building), the
// reader-side output values, and as its key everything the function read from the DNA reader, in its order: the counts,
// the LOD views and blend-shape mappings (through the IDNAReader getters the stock code calls) and the names (from the
// wrapped dna::Reader, IDNAReader::Unwrap, i.e. the StringViews the stock FString getters convert with ANSI_TO_TCHAR;
// compared as the NUL-terminated bytes that conversion reads). A later head whose reader gives the same reads (any
// context: a new DNA asset load makes a new reader with the same content) replays: the same array operations in the
// same order (the game's own TArray functions), and for every name the hierarchy lookup done live on this rig's
// hierarchy, exactly as the stock code does it (same hash, same probe). Same reads -> the stock code takes the same path
// and builds the same strings; an FName of a given string is fixed once it exists (names are never removed), so the
// outputs are identical. Reader classes: the Unwrap code shape (mov rax,[rcx+d]; ret behind the this-adjusting thunk)
// and the dna getter pointers are checked once per class, and on every capture each dna string must equal the stock
// FString getter's result (else the class is left to the game). The neural-network mask map is left to the game (no
// names in practice). Call sites in FRigUnit_RigLogic_Data::InitializeRigLogic (exe 0x4E443C6 ...), found by the call
// block's shape; each map function's bytes are checked in full.
//
// Keys ([Fixes]; installed at start-up when either is on, lab builds always; then live): RigVmHashMemo, RigLogicMapCache:
// 0 off (pass through), 1 on (default). Lab builds also take 2 = verify (the game's function always runs; the cached result is computed
// beside it and compared, mismatches counted and logged); the public build reads 2 as 1 (config.cpp), and the self-test
// runs the verify path directly.
#include "common.h"

#include <intrin.h>

#include <cstring>
#include <string>
#include <vector>

namespace opf {
namespace {

// ---------------------------------------------------------------- shared

int64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
int64_t g_freq = 1;
double Ms(int64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_freq); }

uint64_t Fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

const uint8_t kJmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
// stub: jmp [rip+0] <abs64>; 14 bytes
void WriteJumpStub(uint8_t* stub, void* target)
{
    memcpy(stub, kJmpAbs, 6);
    memcpy(stub + 6, &target, 8);
}
// Redirect a call rel32 at `site` to `stub`; returns the old target.
uint8_t* CallTarget(uint8_t* site) { return site[0] == 0xE8 ? RipTarget(site, 1, 5) : nullptr; }
bool PatchCall(uint8_t* site, uint8_t* stub)
{
    const int64_t rel = stub - (site + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t code[5] = {0xE8};
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(code + 1, &r32, 4);
    return WriteCode(site, std::span<const uint8_t>(code, 5));
}

// ---------------------------------------------------------------- RigVmHashMemo

// ComputeVMHash's prologue (mov [rsp+8..20h],rbx/rbp/rsi/rdi; push r13-r15; sub rsp,40h), mov rax,[rcx+FunctionNames],
// xor edi,edi; mov r13,rcx; mov esi,edi; mov ebp,9E3779B9h (HashCombine's constant).
constexpr char kComputeVmHash[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 55 41 56 41 57 48 83 EC 40 48 8B 81 ?? ?? ?? ?? "
    "33 FF 4C 8B E9 8B F7 BD B9 79 37 9E";
constexpr size_t kCvhSize = 0x456;
constexpr uint64_t kCvhFnv = 0x64A8F80FC5FB217Cull;  // build 19115871
// fixed by the checked bytes:
constexpr size_t kVmFunctionNames = 0x130, kVmByteCode = 0x100, kVmExternals = 0x1D8, kVmLiteral = 0x30, kVmWork = 0x28;
constexpr size_t kCallStaticClass = 0x2FC, kCallMemoryHash = 0x322;
// FRigVMByteCode::GetByteCodeHash (exe+0x49B41A0, called at ComputeVMHash+0x10F): reads the byte code fields below; pinned to
// this build's bytes too (its rel32 calls of the instruction walk / operand / branch hashes included).
constexpr size_t kCallByteCodeHash = 0x10F, kBchSize = 0x300;
constexpr uint64_t kBchFnv = 0xCEA7DCD1B35893C3ull;  // build 19115871
constexpr size_t kExtStride = 0x28, kExtName = 0x00, kExtType = 0x10;
// FRigVMByteCode (fields GetByteCodeHash and its callees read)
constexpr size_t kBcData = 0x00, kBcNum = 0x08, kBcEntries = 0x18, kBcEntriesNum = 0x20, kBcBranches = 0x28, kBcBranchesNum = 0x30,
                 kBcAligned = 0x98;
constexpr size_t kEntryStride = 0x0C, kBranchStride = 0x18;

using HashFn = uint32_t (*)(const uint8_t* vm);
using StaticClassFn = uint8_t* (*)();
using MemoryHashFn = uint32_t (*)(uint8_t* cls);

struct VmKey {  // owned copy of everything ComputeVMHash reads
    std::vector<uint8_t> names, code, entries, branches, ext;
    uint32_t codeNum = 0, misalign = 0, litHash = 0, workHash = 0;
    uint8_t aligned = 0, lit = 0, work = 0;
    uint64_t quick = 0;
};
struct VmLive {  // the same, in place in the game's VM
    const uint8_t *names, *code, *entries, *branches, *ext;
    size_t namesBytes, entriesBytes, branchesBytes;
    int32_t extNum;
    uint32_t codeNum, misalign, litHash, workHash;
    uint8_t aligned, lit, work;
    uint64_t quick;
};
struct VmEntry {
    VmKey key;
    uint32_t hash = 0;
};

struct VmState {
    uint8_t* fn = nullptr;
    HashFn orig = nullptr;
    StaticClassFn staticClass = nullptr;
    MemoryHashFn memoryHash = nullptr;
    std::atomic<int> mode{0};
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<VmEntry*> entries;
    size_t bytes = 0;
    std::atomic<uint64_t> calls{0}, hits{0}, misses{0}, verified{0}, mismatches{0}, full{0};
    std::atomic<int64_t> origTicks{0}, hitTicks{0};
    std::atomic<uint64_t> origCalls{0};
    std::atomic<int> logged{0};
} g_vm;

constexpr size_t kVmMaxEntries = 512, kVmMaxBytes = 48u << 20;

uint32_t MemoryPart(uint8_t* storage)
{
    uint8_t* const cls = *reinterpret_cast<uint8_t**>(storage + 0x10);
    if (!cls) return 0;
    uint8_t* const gen = g_vm.staticClass();
    uint8_t* const meta = *reinterpret_cast<uint8_t**>(cls + 0x10);
    const int32_t depth = *reinterpret_cast<int32_t*>(gen + 0x38);
    if (depth > *reinterpret_cast<int32_t*>(meta + 0x38)) return 0;
    if ((*reinterpret_cast<uint8_t***>(meta + 0x30))[depth] != gen + 0x30) return 0;
    return g_vm.memoryHash(cls);
}

size_t Bytes(int32_t num, size_t stride) { return num > 0 ? static_cast<size_t>(num) * stride : 0; }

void ReadLive(const uint8_t* vm, VmLive& k)
{
    const uint8_t* names = *reinterpret_cast<uint8_t* const*>(vm + kVmFunctionNames);
    k.names = *reinterpret_cast<uint8_t* const*>(names);
    k.namesBytes = Bytes(*reinterpret_cast<const int32_t*>(names + 8), 8);
    const uint8_t* bc = *reinterpret_cast<uint8_t* const*>(vm + kVmByteCode);
    k.code = *reinterpret_cast<uint8_t* const*>(bc + kBcData);
    const int32_t codeNum = *reinterpret_cast<const int32_t*>(bc + kBcNum);
    k.codeNum = static_cast<uint32_t>(codeNum > 0 ? codeNum : 0);
    k.misalign = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(k.code) & 15);
    k.aligned = bc[kBcAligned];
    k.entries = *reinterpret_cast<uint8_t* const*>(bc + kBcEntries);
    k.entriesBytes = Bytes(*reinterpret_cast<const int32_t*>(bc + kBcEntriesNum), kEntryStride);
    k.branches = *reinterpret_cast<uint8_t* const*>(bc + kBcBranches);
    k.branchesBytes = Bytes(*reinterpret_cast<const int32_t*>(bc + kBcBranchesNum), kBranchStride);
    k.ext = *reinterpret_cast<uint8_t* const*>(vm + kVmExternals);
    const int32_t xn = *reinterpret_cast<const int32_t*>(vm + kVmExternals + 8);
    k.extNum = xn > 0 ? xn : 0;
    uint8_t* const lit = *reinterpret_cast<uint8_t* const*>(vm + kVmLiteral);
    uint8_t* const work = *reinterpret_cast<uint8_t* const*>(vm + kVmWork);
    k.lit = lit != nullptr;
    k.work = work != nullptr;
    k.litHash = lit ? MemoryPart(lit) : 0;
    k.workHash = work ? MemoryPart(work) : 0;
    uint64_t q = Fnv(k.code, k.codeNum < 256 ? k.codeNum : 256);
    q ^= (static_cast<uint64_t>(k.codeNum) << 32) ^ (static_cast<uint64_t>(k.entriesBytes) << 16) ^ k.branchesBytes ^
         (static_cast<uint64_t>(k.namesBytes) << 40) ^ (static_cast<uint64_t>(k.extNum) << 56) ^
         (static_cast<uint64_t>(k.litHash) * 0x9E3779B97F4A7C15ull) ^ k.workHash;
    k.quick = q;
}

bool SameExt(const std::vector<uint8_t>& owned, const uint8_t* ext, int32_t n)
{
    if (owned.size() != static_cast<size_t>(n) * 16) return false;
    for (int32_t i = 0; i < n; ++i)
        if (memcmp(owned.data() + i * 16, ext + i * kExtStride + kExtName, 8) != 0 ||
            memcmp(owned.data() + i * 16 + 8, ext + i * kExtStride + kExtType, 8) != 0)
            return false;
    return true;
}
bool SameBytes(const std::vector<uint8_t>& owned, const uint8_t* p, size_t n)
{
    return owned.size() == n && (n == 0 || memcmp(owned.data(), p, n) == 0);
}

bool SameKey(const VmKey& a, const VmLive& b)
{
    return a.quick == b.quick && a.codeNum == b.codeNum && a.misalign == b.misalign && a.aligned == b.aligned && a.lit == b.lit &&
           a.work == b.work && a.litHash == b.litHash && a.workHash == b.workHash && SameBytes(a.names, b.names, b.namesBytes) &&
           SameBytes(a.entries, b.entries, b.entriesBytes) && SameBytes(a.branches, b.branches, b.branchesBytes) &&
           SameExt(a.ext, b.ext, b.extNum) && SameBytes(a.code, b.code, b.codeNum);
}

void Own(const VmLive& l, VmKey& k)
{
    k.names.assign(l.names, l.names + l.namesBytes);
    k.code.assign(l.code, l.code + l.codeNum);
    k.entries.assign(l.entries, l.entries + l.entriesBytes);
    k.branches.assign(l.branches, l.branches + l.branchesBytes);
    k.ext.resize(static_cast<size_t>(l.extNum) * 16);
    for (int32_t i = 0; i < l.extNum; ++i) {
        memcpy(k.ext.data() + i * 16, l.ext + i * kExtStride + kExtName, 8);
        memcpy(k.ext.data() + i * 16 + 8, l.ext + i * kExtStride + kExtType, 8);
    }
    k.codeNum = l.codeNum;
    k.misalign = l.misalign;
    k.litHash = l.litHash;
    k.workHash = l.workHash;
    k.aligned = l.aligned;
    k.lit = l.lit;
    k.work = l.work;
    k.quick = l.quick;
}

const VmEntry* FindVm(const VmLive& k)
{
    AcquireSRWLockShared(&g_vm.lock);
    const VmEntry* hit = nullptr;
    for (const VmEntry* e : g_vm.entries)
        if (SameKey(e->key, k)) {
            hit = e;
            break;
        }
    ReleaseSRWLockShared(&g_vm.lock);
    return hit;
}

void AddVm(const VmLive& l, uint32_t hash)
{
    const size_t bytes = l.namesBytes + l.codeNum + l.entriesBytes + l.branchesBytes + static_cast<size_t>(l.extNum) * 16;
    if (FindVm(l)) return;
    {  // full: count it and skip the key copy (checked again under the lock below)
        AcquireSRWLockShared(&g_vm.lock);
        const bool full = g_vm.entries.size() >= kVmMaxEntries || g_vm.bytes + bytes > kVmMaxBytes;
        ReleaseSRWLockShared(&g_vm.lock);
        if (full) {
            g_vm.full.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    auto* e = new VmEntry;
    Own(l, e->key);
    e->hash = hash;
    AcquireSRWLockExclusive(&g_vm.lock);
    bool dup = false;
    for (const VmEntry* x : g_vm.entries) dup |= SameKey(x->key, l);
    if (!dup && g_vm.entries.size() < kVmMaxEntries && g_vm.bytes + bytes <= kVmMaxBytes) {
        g_vm.entries.push_back(e);
        g_vm.bytes += bytes;
        e = nullptr;
    } else if (!dup) g_vm.full.fetch_add(1, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_vm.lock);
    delete e;
}

uint32_t TimedOrig(const uint8_t* vm)
{
    const int64_t t0 = Qpc();
    const uint32_t h = g_vm.orig(vm);
    g_vm.origTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
    g_vm.origCalls.fetch_add(1, std::memory_order_relaxed);
    return h;
}

uint32_t ComputeVmHashHook(const uint8_t* vm)
{
    const int mode = g_vm.mode.load(std::memory_order_relaxed);
    if (mode == 0) return g_vm.orig(vm);
    g_vm.calls.fetch_add(1, std::memory_order_relaxed);
    const int64_t t0 = Qpc();
    VmLive k;
    ReadLive(vm, k);
    const VmEntry* e = FindVm(k);
    if (e && mode == 1) {
        g_vm.hits.fetch_add(1, std::memory_order_relaxed);
        g_vm.hitTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
        return e->hash;
    }
    const uint32_t h = TimedOrig(vm);
    if (e) {
        g_vm.verified.fetch_add(1, std::memory_order_relaxed);
        if (e->hash != h) {
            g_vm.mismatches.fetch_add(1, std::memory_order_relaxed);
            if (g_vm.logged.fetch_add(1) < 10)
                Log("[rigsetup] RigVmHashMemo verify MISMATCH: cached 0x%08X, game 0x%08X (code %u bytes, %zu names)", e->hash, h,
                    k.codeNum, k.namesBytes / 8);
        }
    } else {
        g_vm.misses.fetch_add(1, std::memory_order_relaxed);
        AddVm(k, h);
    }
    return h;
}

// ---------------------------------------------------------------- RigLogicMapCache

struct I32Arr {
    int32_t* data;
    int32_t num, max;
};
struct NestArr {  // TArray<TNestedArray<int32>>, 16-byte elements
    I32Arr* data;
    int32_t num, max;
};
static_assert(sizeof(I32Arr) == 16 && sizeof(NestArr) == 16, "TArray layout");

// FRigUnit_RigLogic_Data (fixed by the checked function bytes)
constexpr size_t kDContext = 0x08, kDController = 0x10, kDInput = 0x20, kDJoints = 0x40, kDMorph = 0x50, kDBlend = 0x60, kDMaskCurve = 0x70,
                 kDMaskIndex = 0x80;
// URigHierarchy::IndexLookup (TMap<FRigElementKey, int32>) as the map functions read it inline
constexpr size_t kHElems = 0xB8, kHNum = 0xC0, kHNumFree = 0xEC, kHInlineHash = 0xF0, kHHash = 0xF8, kHHashSize = 0x100;
constexpr uint8_t kTypeBone = 1, kTypeCurve = 8;
// reader interface vtable slots (as called by the checked functions)
constexpr size_t kVLodCount = 0x78, kVRawControlCount = 0xA8, kVJointCount = 0xB8, kVAnimMapIndices = 0x118, kVBlendMapIndices = 0x150;

enum Kind { kJoints, kInput, kMorph, kMask, kKinds };
const char* const kKindNames[kKinds] = {"joints", "controls", "morph targets", "mask multipliers"};

struct MapFunc {
    uint32_t size;
    uint64_t fnv;
    uint32_t nameCall;  // offset of the FName(const TCHAR*, FNAME_Add) call
    uint32_t nameHash;  // offset of the GetTypeHash(FName) call
};
// build 19115871: MapJoints 0x4E44DD0, MapInputCurveIndices 0x4E44B80, MapMorphTargets 0x4E45A90, MapMaskMultipliers 0x4E45250
const MapFunc kMapFuncs[kKinds] = {
    {0x1B5, 0xE058A7D313715E57ull, 0xAE, 0xC1},
    {0x247, 0x220B62D6A06D193Aull, 0x10B, 0x12A},
    {0x499, 0x604F218E4400B223ull, 0x305, 0x317},
    {0x4CE, 0x934342E67D2BBED7ull, 0x30C, 0x31F},
};
// morph function: helper calls (offsets of call rel32)
constexpr size_t kMorphFree = 0x6C, kMorphEmpty16 = 0xAF, kMorphGrow16 = 0xCE, kMorphResizeTo = 0x20E, kMorphGrow32 = 0x3B6;
// InitializeRigLogic's five calls: mov rdx,r12; mov rcx,rbp; (mov [rbp+90h],eax first) call Map...
constexpr char kInitCalls[] =
    "0F B7 C0 49 8B D4 48 8B CD 89 85 90 00 00 00 E8 ?? ?? ?? ?? 49 8B D4 48 8B CD E8 ?? ?? ?? ?? 49 8B D4 48 8B CD E8 ?? ?? ?? ?? "
    "49 8B D4 48 8B CD E8 ?? ?? ?? ?? 49 8B D4 48 8B CD E8 ?? ?? ?? ??";
constexpr size_t kInitCallOff[5] = {0x0F, 0x1A, 0x25, 0x30, 0x3B};  // joints, input, neural (left alone), morph, mask

using MapFn = void (*)(uint8_t* data, uint8_t* hierarchy);
using FNameCtorFn = uint64_t* (*)(uint64_t* out, const wchar_t* s, int find);
using NameHashFn = uint32_t (*)(uint32_t comparisonIndex);
using ResizeFn = void (*)(void* arr, int32_t n);
using FreeFn = void (*)(void* p);
using CountFn = uint16_t (*)(void* iface);
struct View {
    const uint16_t* data;
    int32_t num;
    int32_t pad;
};
using ViewFn = View* (*)(void* iface, View* out, uint32_t lod);

// The content key (rigcache2): everything the map function reads from the DNA reader, in the order it reads it. Counts,
// LOD views and blend-shape mappings come from the IDNAReader getters the stock code calls (no allocation); the names
// from the wrapped dna::Reader (IDNAReader::Unwrap, the same StringView the FString getters convert with ANSI_TO_TCHAR),
// compared as the NUL-terminated byte strings the stock conversion reads. Equal reads -> the stock function takes the
// same path and builds the same strings, hence the same FNames (an FName of a given string is fixed once it exists).
// dna::DefinitionReader slots = IDNAReader's minus 0x10 (no Unwrap / Unload); checked per reader class against the
// FString getters on every capture.
constexpr size_t kVUnwrap = 0x08, kVRawName = 0xB0, kVJointName = 0xC0, kVBsName = 0xE8, kVAnimName = 0x108, kVMeshName = 0x128,
                 kVMapping = 0x148;
constexpr size_t kNRaw = 0xA0, kNJoint = 0xB0, kNBs = 0xD8, kNAnim = 0xF8, kNMesh = 0x118;
enum StrSlot : uint8_t { kSRaw, kSJoint, kSBs, kSAnim, kSMesh, kSlots };
const size_t kDnaSlot[kSlots] = {kNRaw, kNJoint, kNBs, kNAnim, kNMesh};
const size_t kIfSlot[kSlots] = {kVRawName, kVJointName, kVBsName, kVAnimName, kVMeshName};

struct MapEntry {
    int kind = 0;
    uint32_t n = 0;                    // joints / controls: the count Reset() reserves; morph / mask: LOD count
    std::vector<uint64_t> names;       // in stock order
    std::vector<uint32_t> lodReserve;  // morph / mask: per LOD, the count reserved (the reader's view size)
    std::vector<uint32_t> lodCount;    // morph / mask: per LOD, entries added (mask: an early return stops short)
    std::vector<int32_t> values;       // morph: blend shape channel index per entry; mask: animated map index per entry
    // content key
    void* ifaceVt = nullptr;           // IDNAReader (definition sub-object) vtable
    void* dnaVt = nullptr;             // dna::DefinitionReader sub-object vtable
    std::vector<uint16_t> views;       // morph / mask: the LOD views read (lodReserve[lod] values per LOD read)
    std::vector<uint64_t> maps;        // morph: mesh | blend shape channel << 32 (both int32), per entry
    std::vector<uint8_t> strSlot;      // strings read: getter, index, bytes (blob[strOff[i] .. strOff[i+1]))
    std::vector<uint16_t> strIdx;
    std::vector<uint32_t> strOff;
    std::string blob;
    size_t Bytes() const
    {
        return sizeof(*this) + (names.size() + maps.size()) * 8 + (lodReserve.size() + lodCount.size() + values.size() + strOff.size()) * 4 +
               views.size() * 2 + strIdx.size() * 3 + blob.size();
    }
};

struct MapState {
    MapFn orig[kKinds] = {};
    FNameCtorFn fnameCtor = nullptr;
    NameHashFn nameHash = nullptr;
    ResizeFn resizeTo = nullptr, grow32 = nullptr, grow16 = nullptr, empty16 = nullptr;
    FreeFn free = nullptr;
    std::atomic<int> mode{0};
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<MapEntry*> entries;
    size_t bytes = 0;
    std::atomic<uint64_t> calls[kKinds] = {}, replays[kKinds] = {}, captures[kKinds] = {}, verified[kKinds] = {}, mismatches[kKinds] = {},
                          rejected[kKinds] = {}, stockCalls[kKinds] = {}, unkeyed[kKinds] = {}, full[kKinds] = {};
    std::atomic<int64_t> keyTicks[kKinds] = {};
    std::atomic<uint64_t> keyCalls[kKinds] = {};
    // reader classes (IDNAReader vtable, dna::DefinitionReader vtable) checked usable / refused
    struct ReaderClass {
        void* ifaceVt;
        void* dnaVt;
        int state;  // 1 usable (strings matched the FString getters), -1 refused
    };
    ReaderClass classes[16] = {};
    int numClasses = 0;
    std::atomic<int64_t> stockTicks[kKinds] = {}, replayTicks[kKinds] = {};
    std::atomic<int> logged{0};
} g_map;

thread_local std::vector<uint64_t>* tl_capture = nullptr;

uint64_t* NameCaptureHook(uint64_t* out, const wchar_t* s, int find)
{
    uint64_t* const r = g_map.fnameCtor(out, s, find);
    if (tl_capture) tl_capture->push_back(*out);
    return r;
}

void* Iface(uint8_t* data)
{
    uint8_t* const context = *reinterpret_cast<uint8_t**>(data + kDContext);
    uint8_t* const reader = *reinterpret_cast<uint8_t**>(context);
    uint8_t* const vb = *reinterpret_cast<uint8_t**>(reader + 8);
    return reader + 8 + *reinterpret_cast<int32_t*>(vb + 4);
}
template <class F> F Slot(void* iface, size_t off) { return *reinterpret_cast<F*>(*reinterpret_cast<uint8_t**>(iface) + off); }
uint32_t Count(void* iface, size_t off) { return Slot<CountFn>(iface, off)(iface); }
int32_t ViewNum(void* iface, size_t off, uint32_t lod)
{
    View v{};
    Slot<ViewFn>(iface, off)(iface, &v, lod);
    return v.num;
}

// ---- content key: reader access
struct StrView {
    const char* data;
    uint64_t size;
};
struct FStr {  // FString = TArray<TCHAR>; Num counts the terminator
    wchar_t* data;
    int32_t num, max;
};
struct Mapping {  // FMeshBlendShapeChannelMapping (int32 MeshIndex, BlendShapeChannelIndex; the getters take them as uint16)
    int32_t mesh, bs;
};
using DnaStrFn = StrView* (*)(void* def, StrView* out, uint32_t idx);
using IfStrFn = FStr* (*)(void* iface, FStr* out, uint32_t idx);
using UnwrapFn = uint8_t* (*)(void* iface);
using MappingFn = Mapping* (*)(void* iface, Mapping* out, uint32_t idx);

// A dna string getter under SEH (only reached for reader classes whose vtables were checked; the guard covers a wrong
// slot assumption on a first use). False on a fault.
bool SafeDnaStr(void* def, size_t slot, uint32_t idx, const char** out)
{
    __try {
        StrView v{nullptr, 0};
        Slot<DnaStrFn>(def, slot)(def, &v, idx);
        *out = v.data;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The narrow string the stock ANSI_TO_TCHAR read, against the FString it made: n chars including the terminator.
// 1 same, 0 different, 2 not ASCII (the conversion is then not a plain widening; the entry is not cached), -1 fault.
int SafeSameW(const char* narrow, const wchar_t* wide, int32_t n)
{
    __try {
        for (int32_t i = 0; i < n; ++i) {
            const uint8_t c = static_cast<uint8_t>(narrow[i]);
            if (c >= 0x80) return 2;
            if (static_cast<uint32_t>(wide[i]) != c) return 0;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// dna::Reader* -> its DefinitionReader sub-object (virtual base, vbptr at +8 as in IDNAReader), under SEH.
uint8_t* SafeDefOf(uint8_t* r)
{
    if (!r) return nullptr;
    __try {
        uint8_t* const vb = *reinterpret_cast<uint8_t**>(r + 8);
        uint8_t* const def = r + 8 + *reinterpret_cast<int32_t*>(vb + 4);
        volatile void* vt = *reinterpret_cast<void**>(def);
        return vt ? def : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
bool IsExec(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
// IDNAReader::Unwrap as FDNAReader implements it: `return ReaderPtr.Get()` = mov rax,[rcx+d]; ret, reached through
// the virtual base's this-adjusting thunk (sub/add rcx; jmp). Anything else (another IDNAReader class) is not used.
bool UnwrapShapeOk(const uint8_t* p)
{
    for (int hop = 0; hop < 4 && p; ++hop) {
        // vtordisp adjustor (build 19115871: movsxd rax,[rcx-4]; sub rcx,rax; jmp -> mov rax,[rcx-10h]; ret)
        if (p[0] == 0x48 && p[1] == 0x63 && p[2] == 0x41 && p[4] == 0x48 && p[5] == 0x2B && p[6] == 0xC8) p += 7;
        else if (p[0] == 0x48 && p[1] == 0x83 && (p[2] == 0xE9 || p[2] == 0xC1) && p[4] == 0xE9) p = RipTarget(p + 4, 1, 5);
        else if (p[0] == 0x48 && p[1] == 0x81 && (p[2] == 0xE9 || p[2] == 0xC1) && p[7] == 0xE9) p = RipTarget(p + 7, 1, 5);
        else if (p[0] == 0xE9) p = RipTarget(p, 1, 5);
        else break;
    }
    if (!p) return false;
    return (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x41 && p[4] == 0xC3) || (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x81 && p[7] == 0xC3);
}

// Reader class state: 1 usable, 0 not yet checked against the FString getters, -1 refused; nullptr = no usable key.
int ClassState(void* ivt, void* dvt)
{
    for (int i = 0; i < g_map.numClasses; ++i)
        if (g_map.classes[i].ifaceVt == ivt && g_map.classes[i].dnaVt == dvt) return g_map.classes[i].state;
    return 2;  // unknown
}
void SetClass(void* ivt, void* dvt, int state)
{
    AcquireSRWLockExclusive(&g_map.lock);
    bool found = false;
    for (int i = 0; i < g_map.numClasses; ++i)
        if (g_map.classes[i].ifaceVt == ivt && g_map.classes[i].dnaVt == dvt) {
            if (g_map.classes[i].state >= 0) g_map.classes[i].state = state;  // a refusal is final
            found = true;
        }
    if (!found && g_map.numClasses < 16) g_map.classes[g_map.numClasses++] = {ivt, dvt, state};
    ReleaseSRWLockExclusive(&g_map.lock);
}
uint8_t* DnaDef(void* iface, int& state)
{
    void* const ivt = *reinterpret_cast<void**>(iface);
    AcquireSRWLockShared(&g_map.lock);
    int unwrap = ClassState(ivt, nullptr);
    ReleaseSRWLockShared(&g_map.lock);
    if (unwrap == 2) {
        const uint8_t* const fn = Slot<const uint8_t*>(iface, kVUnwrap);
        const bool ok = IsExec(fn) && UnwrapShapeOk(fn);
        if (!ok) {
            const GameModule& game = Game();
            Log("[rigsetup] RigLogicMapCache: DNA reader class (vtable exe+0x%llX) has an unknown Unwrap (%02X %02X %02X %02X %02X %02X %02X "
                "%02X): its heads are left to the game",
                static_cast<unsigned long long>(static_cast<uint8_t*>(ivt) - game.base), fn ? fn[0] : 0, fn ? fn[1] : 0, fn ? fn[2] : 0,
                fn ? fn[3] : 0, fn ? fn[4] : 0, fn ? fn[5] : 0, fn ? fn[6] : 0, fn ? fn[7] : 0);
        }
        SetClass(ivt, nullptr, ok ? 1 : -1);
        unwrap = ok ? 1 : -1;
    }
    if (unwrap != 1) return nullptr;
    uint8_t* const def = SafeDefOf(Slot<UnwrapFn>(iface, kVUnwrap)(iface));
    if (!def) return nullptr;
    void* const dvt = *reinterpret_cast<void**>(def);
    AcquireSRWLockShared(&g_map.lock);
    state = ClassState(ivt, dvt);
    ReleaseSRWLockShared(&g_map.lock);
    if (state == 2) {
        bool ok = true;
        for (size_t s : kDnaSlot) ok &= IsExec(Slot<const void*>(def, s));
        SetClass(ivt, dvt, ok ? 0 : -1);
        state = ok ? 0 : -1;
    }
    return state < 0 ? nullptr : def;
}
// The live string at (slot, idx) equals the stored bytes (NUL-terminated read, as the stock conversion reads it).
bool StrEq(uint8_t* def, int slot, uint32_t idx, const char* s, size_t n)
{
    StrView v{nullptr, 0};
    Slot<DnaStrFn>(def, kDnaSlot[slot])(def, &v, idx);
    return v.data && strncmp(v.data, s, n) == 0 && v.data[n] == 0;
}

// The hierarchy lookup exactly as the map functions do it inline.
int32_t Find(const uint8_t* h, uint8_t type, uint64_t name)
{
    const uint32_t hash = g_map.nameHash(static_cast<uint32_t>(name));
    if (*reinterpret_cast<const int32_t*>(h + kHNum) == *reinterpret_cast<const int32_t*>(h + kHNumFree)) return -1;
    const int32_t* buckets = *reinterpret_cast<int32_t* const*>(h + kHHash);
    if (!buckets) buckets = reinterpret_cast<const int32_t*>(h + kHInlineHash);
    const uint32_t mask = static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(h + kHHashSize)) - 1u;
    int32_t id = buckets[mask & ((hash + static_cast<uint32_t>(name >> 32)) * 10u + type)];
    const uint8_t* const elems = *reinterpret_cast<uint8_t* const*>(h + kHElems);
    while (id != -1) {
        const uint8_t* const e = elems + static_cast<int64_t>(id) * 0x18;
        if (*reinterpret_cast<const uint64_t*>(e + 4) == name && e[0] == type) return *reinterpret_cast<const int32_t*>(e + 0xC);
        id = *reinterpret_cast<const int32_t*>(e + 0x10);
    }
    return -1;
}

// TArray<int32>::Add as stock (grow when full; never needed after the reserve, kept for exactness)
void Add32(I32Arr* a, int32_t v)
{
    const int32_t i = a->num;
    a->num = i + 1;
    if (static_cast<uint32_t>(a->max) < static_cast<uint32_t>(i + 1)) g_map.grow32(a, i);
    a->data[i] = v;
}
// TArray<TNestedArray<int32>>::Reset() then AddDefaulted(n), as stock
void ResetNested(NestArr* a, uint32_t n)
{
    I32Arr* p = a->data;
    int32_t c = a->num;
    const bool emptyPath = a->max < 0;
    for (; c != 0; --c, ++p)
        if (p->data) g_map.free(p->data);
    a->num = 0;
    if (emptyPath && a->max != 0) g_map.empty16(a, 0);
    const int32_t old = a->num;
    a->num = old + static_cast<int32_t>(n);
    if (static_cast<uint32_t>(a->max - old) < n) g_map.grow16(a, old);
    memset(a->data + old, 0, static_cast<size_t>(n) * 16);
}

// Replay into the game's arrays (mode 1).
void Replay(const MapEntry& e, uint8_t* d, uint8_t* h)
{
    switch (e.kind) {
    case kJoints: {
        I32Arr* a = reinterpret_cast<I32Arr*>(d + kDJoints);
        const int32_t n = static_cast<int32_t>(e.n);
        a->num = 0;
        if (n != a->max && a->max <= n) g_map.resizeTo(a, n);
        for (uint64_t nm : e.names) Add32(a, Find(h, kTypeBone, nm));
        break;
    }
    case kInput: {
        I32Arr* a = reinterpret_cast<I32Arr*>(d + kDInput);
        const int32_t n = static_cast<int32_t>(e.n);
        a->num = 0;
        if (a->max < n) g_map.resizeTo(a, n);
        for (uint64_t nm : e.names) Add32(a, h ? Find(h, kTypeCurve, nm) : -1);
        break;
    }
    case kMorph:
    case kMask: {
        NestArr* a = reinterpret_cast<NestArr*>(d + (e.kind == kMorph ? kDMorph : kDMaskCurve));
        NestArr* b = reinterpret_cast<NestArr*>(d + (e.kind == kMorph ? kDBlend : kDMaskIndex));
        ResetNested(a, e.n);
        ResetNested(b, e.n);
        size_t k = 0;
        for (uint32_t lod = 0; lod < e.n && lod < e.lodCount.size(); ++lod) {
            const int32_t r = static_cast<int32_t>(e.lodReserve[lod]);
            I32Arr* ia = a->data + lod;
            if (ia->max < r) g_map.resizeTo(ia, r);
            I32Arr* ib = b->data + lod;
            if (ib->max < r) g_map.resizeTo(ib, r);
            for (uint32_t j = 0; j < e.lodCount[lod]; ++j, ++k) {
                Add32(ia, Find(h, kTypeCurve, e.names[k]));
                Add32(ib, e.values[k]);
            }
            if (e.lodCount[lod] < e.lodReserve[lod]) break;  // the stock early return (mask only)
        }
        break;
    }
    }
}

// What the replay would produce, without touching the game's arrays (verify mode).
bool ReplayMatches(const MapEntry& e, uint8_t* d, uint8_t* h, const std::vector<uint64_t>& names)
{
    if (names != e.names) return false;
    auto same = [](const I32Arr* a, const std::vector<int32_t>& v) {
        return a->num == static_cast<int32_t>(v.size()) && (v.empty() || memcmp(a->data, v.data(), v.size() * 4) == 0);
    };
    std::vector<int32_t> v;
    switch (e.kind) {
    case kJoints:
        for (uint64_t nm : e.names) v.push_back(Find(h, kTypeBone, nm));
        return same(reinterpret_cast<I32Arr*>(d + kDJoints), v);
    case kInput:
        for (uint64_t nm : e.names) v.push_back(h ? Find(h, kTypeCurve, nm) : -1);
        return same(reinterpret_cast<I32Arr*>(d + kDInput), v);
    default: {
        const NestArr* a = reinterpret_cast<NestArr*>(d + (e.kind == kMorph ? kDMorph : kDMaskCurve));
        const NestArr* b = reinterpret_cast<NestArr*>(d + (e.kind == kMorph ? kDBlend : kDMaskIndex));
        if (a->num != static_cast<int32_t>(e.n) || b->num != static_cast<int32_t>(e.n)) return false;
        size_t k = 0;
        for (uint32_t lod = 0; lod < e.n; ++lod) {
            std::vector<int32_t> va, vb;
            const uint32_t c = lod < e.lodCount.size() ? e.lodCount[lod] : 0;
            for (uint32_t j = 0; j < c; ++j, ++k) {
                va.push_back(Find(h, kTypeCurve, e.names[k]));
                vb.push_back(e.values[k]);
            }
            if (!same(a->data + lod, va) || !same(b->data + lod, vb)) return false;
        }
        return true;
    }
    }
}

// Build a cache entry from a stock call's captured names and outputs plus the reader's counts; false if anything does
// not add up (then the context is simply not cached).
bool BuildEntry(MapEntry& e, uint8_t* d, const std::vector<uint64_t>& names)
{
    void* const iface = Iface(d);
    e.names = names;
    switch (e.kind) {
    case kJoints: {
        e.n = Count(iface, kVJointCount);
        const I32Arr* a = reinterpret_cast<I32Arr*>(d + kDJoints);
        return names.size() == e.n && a->num == static_cast<int32_t>(e.n);
    }
    case kInput: {
        e.n = Count(iface, kVRawControlCount);
        const I32Arr* a = reinterpret_cast<I32Arr*>(d + kDInput);
        return names.size() <= e.n && a->num == static_cast<int32_t>(names.size());
    }
    default: {
        e.n = Count(iface, kVLodCount);
        const NestArr* a = reinterpret_cast<NestArr*>(d + (e.kind == kMorph ? kDMorph : kDMaskCurve));
        const NestArr* b = reinterpret_cast<NestArr*>(d + (e.kind == kMorph ? kDBlend : kDMaskIndex));
        if (a->num != static_cast<int32_t>(e.n) || b->num != static_cast<int32_t>(e.n)) return false;
        size_t total = 0;
        bool stopped = false;
        for (uint32_t lod = 0; lod < e.n; ++lod) {
            const int32_t r = ViewNum(iface, e.kind == kMorph ? kVBlendMapIndices : kVAnimMapIndices, lod);
            const int32_t c = a->data[lod].num;
            if (r < 0 || c < 0 || c > r || b->data[lod].num != c) return false;
            if (stopped) {  // LODs after an early return: nothing added, nothing reserved
                if (c != 0) return false;
                continue;
            }
            e.lodReserve.push_back(static_cast<uint32_t>(r));
            e.lodCount.push_back(static_cast<uint32_t>(c));
            for (int32_t j = 0; j < c; ++j) e.values.push_back(b->data[lod].data[j]);
            total += static_cast<size_t>(c);
            if (c < r) {
                if (e.kind == kMorph) return false;  // morph has no early return
                stopped = true;
            }
        }
        return total == names.size();
    }
    }
}

// The content key of a captured entry (BuildEntry passed): read what the stock function read, in its order, the
// strings both from the dna::Reader and through the stock FString getter, which must agree. False: not cached
// (refuse: the reader class's dna slots disagree with the FString getters, never use it again).
bool BuildKey(MapEntry& e, void* iface, uint8_t* def, const std::vector<uint64_t>& names, bool& refuse)
{
    e.ifaceVt = *reinterpret_cast<void**>(iface);
    e.dnaVt = *reinterpret_cast<void**>(def);
    auto addStr = [&](int slot, uint32_t idx) -> bool {
        const char* p = nullptr;
        if (!SafeDnaStr(def, kDnaSlot[slot], idx, &p) || !p) {
            refuse = true;
            return false;
        }
        FStr f{nullptr, 0, 0};
        Slot<IfStrFn>(iface, kIfSlot[slot])(iface, &f, idx);
        const int32_t n = f.num > 0 ? f.num : 1;
        const int same = SafeSameW(p, f.num > 0 && f.data ? f.data : L"", n);
        if (f.data) g_map.free(f.data);
        if (same != 1) {
            if (same != 2) refuse = true;
            return false;
        }
        e.strSlot.push_back(static_cast<uint8_t>(slot));
        e.strIdx.push_back(static_cast<uint16_t>(idx));
        e.strOff.push_back(static_cast<uint32_t>(e.blob.size()));
        e.blob.append(p, static_cast<size_t>(n - 1));
        return true;
    };
    switch (e.kind) {
    case kJoints:
        for (uint32_t i = 0; i < e.n; ++i)
            if (!addStr(kSJoint, i)) return false;
        break;
    case kInput: {
        // the names added, plus the one that ended the loop early (its curve name was empty)
        const size_t r = names.size() + 1 < e.n ? names.size() + 1 : e.n;
        for (uint32_t i = 0; i < r; ++i)
            if (!addStr(kSRaw, i)) return false;
        break;
    }
    case kMorph: {
        std::vector<uint8_t> seenBs(65536), seenMesh(65536);
        size_t k = 0;
        for (uint32_t lod = 0; lod < e.n; ++lod) {
            View v{};
            Slot<ViewFn>(iface, kVBlendMapIndices)(iface, &v, lod);
            if (v.num != static_cast<int32_t>(e.lodReserve[lod]) || (v.num && !v.data)) return false;
            for (int32_t j = 0; j < v.num; ++j, ++k) {
                const uint16_t mi = v.data[j];
                e.views.push_back(mi);
                Mapping m{};
                Slot<MappingFn>(iface, kVMapping)(iface, &m, mi);
                if (k >= e.values.size() || e.values[k] != m.bs) return false;  // the stock output must agree
                e.maps.push_back(static_cast<uint32_t>(m.mesh) | static_cast<uint64_t>(static_cast<uint32_t>(m.bs)) << 32);
                const uint16_t bs = static_cast<uint16_t>(m.bs), mesh = static_cast<uint16_t>(m.mesh);  // as the stock code passes them
                if (!seenBs[bs]) {
                    seenBs[bs] = 1;
                    if (!addStr(kSBs, bs)) return false;
                }
                if (!seenMesh[mesh]) {
                    seenMesh[mesh] = 1;
                    if (!addStr(kSMesh, mesh)) return false;
                }
            }
        }
        if (k != e.values.size()) return false;
        break;
    }
    case kMask: {
        std::vector<uint8_t> seen(65536);
        size_t k = 0;
        for (uint32_t lod = 0; lod < e.lodCount.size(); ++lod) {
            View v{};
            Slot<ViewFn>(iface, kVAnimMapIndices)(iface, &v, lod);
            if (v.num != static_cast<int32_t>(e.lodReserve[lod]) || (v.num && !v.data)) return false;
            for (int32_t j = 0; j < v.num; ++j) e.views.push_back(v.data[j]);
            const uint32_t c = e.lodCount[lod];
            const uint32_t read = c < e.lodReserve[lod] ? c + 1 : c;  // the early return read one more name
            for (uint32_t j = 0; j < read; ++j) {
                const uint16_t idx = v.data[j];
                if (j < c && (k >= e.values.size() || e.values[k++] != idx)) return false;
                if (!seen[idx]) {
                    seen[idx] = 1;
                    if (!addStr(kSAnim, idx)) return false;
                }
            }
        }
        if (k != e.values.size()) return false;
        break;
    }
    }
    e.strOff.push_back(static_cast<uint32_t>(e.blob.size()));
    return true;
}

// The live reader gives the entry's reads (counts, views, mappings, strings): then the stock function on it would read
// exactly the same values, take the same path and produce the entry's names and values.
bool Match(const MapEntry& e, void* iface, uint8_t* def)
{
    switch (e.kind) {
    case kJoints:
        if (Count(iface, kVJointCount) != e.n) return false;
        break;
    case kInput:
        if (Count(iface, kVRawControlCount) != e.n) return false;
        break;
    default: {
        if (Count(iface, kVLodCount) != e.n) return false;
        size_t k = 0, m = 0;
        const uint32_t lods = e.kind == kMorph ? e.n : static_cast<uint32_t>(e.lodCount.size());
        for (uint32_t lod = 0; lod < lods; ++lod) {
            View v{};
            Slot<ViewFn>(iface, e.kind == kMorph ? kVBlendMapIndices : kVAnimMapIndices)(iface, &v, lod);
            const uint32_t num = e.lodReserve[lod];
            if (v.num != static_cast<int32_t>(num) || k + num > e.views.size()) return false;
            if (num && (!v.data || memcmp(v.data, e.views.data() + k, num * 2) != 0)) return false;
            k += num;
            if (e.kind == kMorph)
                for (uint32_t j = 0; j < num; ++j) {
                    Mapping mm{};
                    Slot<MappingFn>(iface, kVMapping)(iface, &mm, v.data[j]);
                    if ((static_cast<uint32_t>(mm.mesh) | static_cast<uint64_t>(static_cast<uint32_t>(mm.bs)) << 32) != e.maps[m++]) return false;
                }
        }
        if (k != e.views.size()) return false;
        break;
    }
    }
    for (size_t i = 0; i < e.strIdx.size(); ++i)
        if (!StrEq(def, e.strSlot[i], e.strIdx[i], e.blob.data() + e.strOff[i], e.strOff[i + 1] - e.strOff[i])) return false;
    return true;
}

bool SameKey(const MapEntry& a, const MapEntry& b)
{
    return a.kind == b.kind && a.ifaceVt == b.ifaceVt && a.dnaVt == b.dnaVt && a.n == b.n && a.lodReserve == b.lodReserve &&
           a.lodCount == b.lodCount && a.views == b.views && a.maps == b.maps && a.strSlot == b.strSlot && a.strIdx == b.strIdx &&
           a.strOff == b.strOff && a.blob == b.blob;
}

// Entries are never removed while the game runs (names are permanent); the shared lock is held while matching.
const MapEntry* FindMap(int kind, void* iface, uint8_t* def)
{
    void* const ivt = *reinterpret_cast<void**>(iface);
    void* const dvt = *reinterpret_cast<void**>(def);
    AcquireSRWLockShared(&g_map.lock);
    const MapEntry* hit = nullptr;
    for (const MapEntry* e : g_map.entries)
        if (e->kind == kind && e->ifaceVt == ivt && e->dnaVt == dvt && Match(*e, iface, def)) {
            hit = e;
            break;
        }
    ReleaseSRWLockShared(&g_map.lock);
    return hit;
}
constexpr size_t kMapMaxEntries = 1024, kMapMaxBytes = 32u << 20;
void AddMap(MapEntry* e)
{
    const size_t bytes = e->Bytes();
    AcquireSRWLockExclusive(&g_map.lock);
    bool dup = false;
    for (const MapEntry* x : g_map.entries) dup |= SameKey(*x, *e);
    if (!dup && g_map.entries.size() < kMapMaxEntries && g_map.bytes + bytes <= kMapMaxBytes) {
        g_map.entries.push_back(e);
        g_map.bytes += bytes;
        e = nullptr;
    } else if (!dup)
        g_map.full[e->kind].fetch_add(1, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_map.lock);
    delete e;
}

template <int K> void MapHook(uint8_t* d, uint8_t* h)
{
    const int mode = g_map.mode.load(std::memory_order_relaxed);
    uint8_t* const context = *reinterpret_cast<uint8_t**>(d + kDContext);
    if (mode == 0 || !context || !*reinterpret_cast<uint8_t**>(context) || (K == kJoints && !h)) {
        g_map.orig[K](d, h);
        return;
    }
    g_map.calls[K].fetch_add(1, std::memory_order_relaxed);
    void* const iface = Iface(d);
    int state = 0;
    uint8_t* const def = DnaDef(iface, state);
    const MapEntry* e = nullptr;
    if (!def) g_map.unkeyed[K].fetch_add(1, std::memory_order_relaxed);
    else if (state == 1) {
        const int64_t t0 = Qpc();
        e = FindMap(K, iface, def);
        g_map.keyTicks[K].fetch_add(Qpc() - t0, std::memory_order_relaxed);
        g_map.keyCalls[K].fetch_add(1, std::memory_order_relaxed);
    }
    if (e && mode == 1) {
        const int64_t t0 = Qpc();
        Replay(*e, d, h);
        g_map.replayTicks[K].fetch_add(Qpc() - t0, std::memory_order_relaxed);
        g_map.replays[K].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::vector<uint64_t> names;
    tl_capture = &names;
    const int64_t t0 = Qpc();
    g_map.orig[K](d, h);
    g_map.stockTicks[K].fetch_add(Qpc() - t0, std::memory_order_relaxed);
    g_map.stockCalls[K].fetch_add(1, std::memory_order_relaxed);
    tl_capture = nullptr;
    if (e) {
        g_map.verified[K].fetch_add(1, std::memory_order_relaxed);
        if (!ReplayMatches(*e, d, h, names)) {
            g_map.mismatches[K].fetch_add(1, std::memory_order_relaxed);
            if (g_map.logged.fetch_add(1) < 10)
                Log("[rigsetup] RigLogicMapCache verify MISMATCH (%s): %zu names captured, %zu cached", kKindNames[K], names.size(),
                    e->names.size());
        }
        return;
    }
    if (!def) return;
    auto* ne = new MapEntry;
    ne->kind = K;
    bool refuse = false;
    if (BuildEntry(*ne, d, names) && BuildKey(*ne, iface, def, names, refuse)) {
        SetClass(ne->ifaceVt, ne->dnaVt, 1);
        const uint64_t nth = g_map.captures[K].fetch_add(1, std::memory_order_relaxed);
        if (nth < 3)
            Log("[rigsetup] RigLogicMapCache %s: captured %zu names, key %zu strings / %zu bytes, %zu view values", kKindNames[K],
                ne->names.size(), ne->strIdx.size(), ne->blob.size(), ne->views.size());
        AddMap(ne);
    } else {
        if (refuse) {
            SetClass(*reinterpret_cast<void**>(iface), *reinterpret_cast<void**>(def), -1);
            Log("[rigsetup] RigLogicMapCache %s: the dna::Reader strings differ from the FString getters: this reader class is left to "
                "the game", kKindNames[K]);
        }
        g_map.rejected[K].fetch_add(1, std::memory_order_relaxed);
        delete ne;
    }
}

const MapFn kMapHooks[kKinds] = {&MapHook<kJoints>, &MapHook<kInput>, &MapHook<kMorph>, &MapHook<kMask>};

bool g_vmInstalled = false, g_mapInstalled = false;

bool InstallVmHashMemo(const GameModule& game, uint8_t* region)
{
    const auto hits = FindAll(game.text, ParsePattern(kComputeVmHash));
    if (hits.size() != 1) {
        Log("[rigsetup] RigVmHashMemo: URigVM::ComputeVMHash found %zu times (expected 1), off", hits.size());
        return false;
    }
    uint8_t* const fn = hits[0];
    if (Fnv(fn, kCvhSize) != kCvhFnv) {
        Log("[rigsetup] RigVmHashMemo: ComputeVMHash at exe+0x%llX differs from build 19115871's, off",
            static_cast<unsigned long long>(fn - game.base));
        return false;
    }
    uint8_t* const bch = CallTarget(fn + kCallByteCodeHash);
    if (!bch || !game.text.Contains(bch) || !game.text.Contains(bch + kBchSize - 1) || Fnv(bch, kBchSize) != kBchFnv) {
        Log("[rigsetup] RigVmHashMemo: FRigVMByteCode::GetByteCodeHash differs from build 19115871's, off");
        return false;
    }
    uint8_t* const sc = CallTarget(fn + kCallStaticClass);
    uint8_t* const mh = CallTarget(fn + kCallMemoryHash);
    if (!sc || !mh || !game.text.Contains(sc) || !game.text.Contains(mh)) {
        Log("[rigsetup] RigVmHashMemo: helper calls not found, off");
        return false;
    }
    // trampoline: mov [rsp+8],rbx (5 bytes) then jmp back; at +32 the stub to the hook
    memcpy(region, fn, 5);
    WriteJumpStub(region + 5, fn + 5);
    WriteJumpStub(region + 32, reinterpret_cast<void*>(&ComputeVmHashHook));
    g_vm.fn = fn;
    g_vm.orig = reinterpret_cast<HashFn>(region);
    g_vm.staticClass = reinterpret_cast<StaticClassFn>(sc);
    g_vm.memoryHash = reinterpret_cast<MemoryHashFn>(mh);
    return true;
}

bool InstallMapCache(const GameModule& game, uint8_t* region, uint8_t** callSites, uint8_t** nameSites)
{
    const auto hits = FindAll(game.text, ParsePattern(kInitCalls));
    if (hits.size() != 1) {
        Log("[rigsetup] RigLogicMapCache: InitializeRigLogic's map calls found %zu times (expected 1), off", hits.size());
        return false;
    }
    const int sitesFor[kKinds] = {0, 1, 3, 4};
    uint8_t* fns[kKinds] = {};
    for (int k = 0; k < kKinds; ++k) {
        uint8_t* const site = hits[0] + kInitCallOff[sitesFor[k]];
        uint8_t* const f = CallTarget(site);
        if (!f || !game.text.Contains(f) || Fnv(f, kMapFuncs[k].size) != kMapFuncs[k].fnv) {
            Log("[rigsetup] RigLogicMapCache: %s map function differs from build 19115871's, off", kKindNames[k]);
            return false;
        }
        fns[k] = f;
        callSites[k] = site;
        nameSites[k] = f + kMapFuncs[k].nameCall;
        if (CallTarget(nameSites[k]) != CallTarget(fns[0] + kMapFuncs[0].nameCall) ||
            CallTarget(f + kMapFuncs[k].nameHash) != CallTarget(fns[0] + kMapFuncs[0].nameHash)) {
            Log("[rigsetup] RigLogicMapCache: %s map's name calls differ, off", kKindNames[k]);
            return false;
        }
    }
    uint8_t* const m = fns[kMorph];
    g_map.fnameCtor = reinterpret_cast<FNameCtorFn>(CallTarget(nameSites[0]));
    g_map.nameHash = reinterpret_cast<NameHashFn>(CallTarget(fns[0] + kMapFuncs[0].nameHash));
    g_map.free = reinterpret_cast<FreeFn>(CallTarget(m + kMorphFree));
    g_map.empty16 = reinterpret_cast<ResizeFn>(CallTarget(m + kMorphEmpty16));
    g_map.grow16 = reinterpret_cast<ResizeFn>(CallTarget(m + kMorphGrow16));
    g_map.resizeTo = reinterpret_cast<ResizeFn>(CallTarget(m + kMorphResizeTo));
    g_map.grow32 = reinterpret_cast<ResizeFn>(CallTarget(m + kMorphGrow32));
    for (int k = 0; k < kKinds; ++k) {
        g_map.orig[k] = reinterpret_cast<MapFn>(fns[k]);
        WriteJumpStub(region + k * 16, reinterpret_cast<void*>(kMapHooks[k]));
    }
    WriteJumpStub(region + kKinds * 16, reinterpret_cast<void*>(&NameCaptureHook));
    return true;
}

}  // namespace

bool InstallRigSetup(bool beforeGameCode)
{
    if (g_vmInstalled || g_mapInstalled) return true;
    if (!g_freq || g_freq == 1) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        g_freq = q.QuadPart ? q.QuadPart : 1;
    }
    if (!beforeGameCode) {
        Log("[rigsetup] RigVmHashMemo / RigLogicMapCache: installed only while the game starts, off");
        return false;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart ? f.QuadPart : 1;
    const GameModule& game = Game();
    uint8_t* const region = AllocNearGame(256);
    if (!region) {
        Log("[rigsetup] no memory near the game's code, off");
        return false;
    }
    uint8_t* callSites[kKinds] = {};
    uint8_t* nameSites[kKinds] = {};
    const bool vm = InstallVmHashMemo(game, region);
    const bool map = InstallMapCache(game, region + 64, callSites, nameSites);
    DWORD old;
    VirtualProtect(region, 256, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 256);
    if (vm) {
        uint8_t code[5] = {0xE9};
        const int64_t rel = (region + 32) - (g_vm.fn + 5);
        const int32_t r32 = static_cast<int32_t>(rel);
        memcpy(code + 1, &r32, 4);
        g_vmInstalled = rel == r32 && WriteCode(g_vm.fn, std::span<const uint8_t>(code, 5));
        if (!g_vmInstalled) Log("[rigsetup] RigVmHashMemo: could not patch ComputeVMHash, off");
    }
    if (map) {
        bool ok = true;
        for (int k = 0; k < kKinds && ok; ++k) ok = PatchCall(nameSites[k], region + 64 + kKinds * 16);
        for (int k = 0; k < kKinds && ok; ++k) ok = PatchCall(callSites[k], region + 64 + k * 16);
        g_mapInstalled = ok;
        if (!ok) Log("[rigsetup] RigLogicMapCache: could not patch the call sites, off");
    }
    Log("[rigsetup] RigVmHashMemo %s, RigLogicMapCache %s (keys live; 0 off, 1 on)", g_vmInstalled ? "installed" : "unavailable",
        g_mapInstalled ? "installed" : "unavailable");
    return g_vmInstalled || g_mapInstalled;
}

void SetRigSetup(int vmHashMemo, int mapCache)
{
    auto clamp = [](int v) { return v < 0 ? 0 : v > 2 ? 2 : v; };
    if (g_vmInstalled) {
        const int m = clamp(vmHashMemo);
        if (g_vm.mode.exchange(m) != m) Log("[rigsetup] RigVmHashMemo=%d", m);
    }
    if (g_mapInstalled) {
        const int m = clamp(mapCache);
        if (g_map.mode.exchange(m) != m) Log("[rigsetup] RigLogicMapCache=%d", m);
    }
}

void LogRigSetupStats()
{
    if (g_vmInstalled && g_vm.calls.load()) {
        const uint64_t oc = g_vm.origCalls.load(), hits = g_vm.hits.load();
        Log("[rigsetup] RigVmHashMemo: %llu calls, %llu hits, %llu misses, %llu verified (%llu MISMATCHES), %zu entries %.1f MB%s; game "
            "%.3f ms/call (%llu), hit %.3f ms/call",
            static_cast<unsigned long long>(g_vm.calls.load()), static_cast<unsigned long long>(hits),
            static_cast<unsigned long long>(g_vm.misses.load()), static_cast<unsigned long long>(g_vm.verified.load()),
            static_cast<unsigned long long>(g_vm.mismatches.load()), g_vm.entries.size(), g_vm.bytes / 1048576.0,
            g_vm.full.load() ? " (full)" : "", oc ? Ms(g_vm.origTicks.load()) / static_cast<double>(oc) : 0.0,
            static_cast<unsigned long long>(oc), hits ? Ms(g_vm.hitTicks.load()) / static_cast<double>(hits) : 0.0);
    }
    if (g_mapInstalled) {
        for (int k = 0; k < kKinds; ++k) {
            if (!g_map.calls[k].load()) continue;
            const uint64_t sc = g_map.stockCalls[k].load(), rp = g_map.replays[k].load();
            const uint64_t kc = g_map.keyCalls[k].load();
            Log("[rigsetup] RigLogicMapCache %s: %llu calls, %llu replays, %llu captures (%llu rejected, %llu unkeyed, %llu full), %llu verified "
                "(%llu MISMATCHES); game %.3f ms/call (%llu), key match %.3f ms/call (%llu), replay %.3f ms/call; %zu entries %.2f MB",
                kKindNames[k], static_cast<unsigned long long>(g_map.calls[k].load()), static_cast<unsigned long long>(rp),
                static_cast<unsigned long long>(g_map.captures[k].load()), static_cast<unsigned long long>(g_map.rejected[k].load()),
                static_cast<unsigned long long>(g_map.unkeyed[k].load()), static_cast<unsigned long long>(g_map.full[k].load()),
                static_cast<unsigned long long>(g_map.verified[k].load()), static_cast<unsigned long long>(g_map.mismatches[k].load()),
                sc ? Ms(g_map.stockTicks[k].load()) / static_cast<double>(sc) : 0.0, static_cast<unsigned long long>(sc),
                kc ? Ms(g_map.keyTicks[k].load()) / static_cast<double>(kc) : 0.0, static_cast<unsigned long long>(kc),
                rp ? Ms(g_map.replayTicks[k].load()) / static_cast<double>(rp) : 0.0, g_map.entries.size(), g_map.bytes / 1048576.0);
        }
    }
}

// ---- self-test hooks (fakes instead of the game's functions) ----
uint8_t* RigSetupTestSites(int which)
{
    if (which == 0) return g_vm.fn;
    return nullptr;
}
void RigSetupTestSetup(const RigSetupTestFns& f)
{
    g_vm.orig = reinterpret_cast<HashFn>(f.vmOrig);
    g_vm.staticClass = reinterpret_cast<StaticClassFn>(f.staticClass);
    g_vm.memoryHash = reinterpret_cast<MemoryHashFn>(f.memoryHash);
    g_map.fnameCtor = reinterpret_cast<FNameCtorFn>(f.fnameCtor);
    g_map.nameHash = reinterpret_cast<NameHashFn>(f.nameHash);
    g_map.resizeTo = reinterpret_cast<ResizeFn>(f.resizeTo);
    g_map.grow32 = reinterpret_cast<ResizeFn>(f.grow32);
    g_map.grow16 = reinterpret_cast<ResizeFn>(f.grow16);
    g_map.empty16 = reinterpret_cast<ResizeFn>(f.empty16);
    g_map.free = reinterpret_cast<FreeFn>(f.free);
    for (int k = 0; k < kKinds; ++k) g_map.orig[k] = reinterpret_cast<MapFn>(f.mapOrig[k]);
    g_vmInstalled = g_mapInstalled = true;
    AcquireSRWLockExclusive(&g_vm.lock);
    for (VmEntry* e : g_vm.entries) delete e;
    g_vm.entries.clear();
    g_vm.bytes = 0;
    ReleaseSRWLockExclusive(&g_vm.lock);
    AcquireSRWLockExclusive(&g_map.lock);
    for (MapEntry* e : g_map.entries) delete e;
    g_map.entries.clear();
    g_map.bytes = 0;
    g_map.numClasses = 0;
    ReleaseSRWLockExclusive(&g_map.lock);
}
void* RigSetupTestNameHook() { return reinterpret_cast<void*>(&NameCaptureHook); }
uint32_t RigSetupTestVm(const void* vm) { return ComputeVmHashHook(static_cast<const uint8_t*>(vm)); }
void RigSetupTestMap(int kind, void* data, void* hierarchy) { kMapHooks[kind](static_cast<uint8_t*>(data), static_cast<uint8_t*>(hierarchy)); }
bool RigSetupTestUnwrapShape(const uint8_t* fn) { return UnwrapShapeOk(fn); }
int RigSetupTestReaderClasses(int state)  // self-test: reader classes in a state (1 usable, 0 unchecked, -1 refused)
{
    int n = 0;
    for (int i = 0; i < g_map.numClasses; ++i) n += g_map.classes[i].state == state && g_map.classes[i].dnaVt != nullptr;
    return n;
}
void RigSetupTestCounts(uint64_t* out)  // vm hits, vm misses, vm mismatches, per map kind: replays, captures, mismatches, rejected; then verified per kind
{
    out[0] = g_vm.hits.load();
    out[1] = g_vm.misses.load();
    out[2] = g_vm.mismatches.load();
    for (int k = 0; k < kKinds; ++k) {
        out[3 + k * 4] = g_map.replays[k].load();
        out[4 + k * 4] = g_map.captures[k].load();
        out[5 + k * 4] = g_map.mismatches[k].load();
        out[6 + k * 4] = g_map.rejected[k].load();
        out[19 + k] = g_map.verified[k].load();
    }
}

}  // namespace opf
