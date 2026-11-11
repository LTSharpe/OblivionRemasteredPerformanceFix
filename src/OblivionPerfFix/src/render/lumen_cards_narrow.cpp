// LumenCardsNarrow (worker lumencards, 2026-11-06; findings "lumenfixa"; public since 1.3.0, port130g). Fix A of findings
// "lumencards": the last loop of UpdateLumenMeshCards (UE 5.3 LumenMeshCards.cpp:458) re-sends the Lumen mapping of ALL
// instances of every queued primitive, although a primitive is usually queued because one instance's mesh cards came or went.
// This sends only those instances; the same values reach the same persistent GPU buffer in the same frame (identical). Nothing
// is held, spread or delayed.
//
// [Fixes] LumenCardsNarrow = 0: not installed (start-up); 1 (default): the fix; then live through the config reload (0 stock,
// 1 fix); any change of value forgets everything the fix knew. Lab builds only: 2 = the fix with proof (below). Start-up only
// install (version.dll). Every site, offset and the FLumenPrimitiveGroup layout are found or checked by byte pattern (no game
// build gate); any mismatch = not installed, with a log line.
//
// What the buffer holds: Buffer[InstanceSceneDataOffset + k] = M(P, k), the mesh cards index of instance k of primitive P,
// M = PrimitiveGroups[LumenPrimitiveGroupIndices[min(k, groups - 1)]].MeshCardsIndex, or -1 with no groups. This loop is the only
// writer (no UAV, shaders only read it); the buffer is external and pooled, so it persists; growing it copies the old contents.
//
// A primitive P may be narrowed only when ALL of these hold (otherwise it is sent whole, as stock):
//   - one group per instance (groups == instances) and at least kMinNarrow instances;
//   - the same pointer, offset and count as at its last whole send (table g_slots);
//   - every entry of its range was last written by P: g_own[dest] holds the id of the last whole send of that dest, so a
//     primitive that took P's range in the meantime (P freed, Q allocated and sent, Q gone, P back at the same offset) is noticed.
// Then only the instances whose group is in PrimitiveGroupIndicesToUpdateInBuffer go. Every change of MeshCardsIndex adds its group
// to that list, in the same step as it queues the primitive (LumenMeshCards.cpp:803 + 810/931, 966 + 969; checked in the binary:
// exe+0x1F1CC20, exe+0x1F1D1D0, exe+0x1F35E48-0x1F35F63), and both lists are emptied together (LumenSceneRendering.cpp:2310).
// A changed group whose primitive is NOT queued in this call: stock sends nothing now but would heal it at the next queue, so
// that primitive's record is dropped and its next send is whole. The queue can be lost: a water-info renderer runs Render on the same
// scene with no Lumen view, so its scene update queues primitives and its UpdateLumenScene empties the lists with no upload. A primitive
// re-created there (same pointer, offset, count) has its new groups listed by the next active renderer with nothing queued, in a call that
// never reaches the loop (stock's je at exe+0x1F42750 and 0x1F42789). So the drop also runs at the lists' reset (exe+0x1FE49D3, every
// UpdateLumenScene, Lumen active or not, one way through): whatever is listed and wasn't handled by Decide in this renderer loses its record.
// Lost queue entries themselves change nothing here: stock writes nothing for them, nor does the fix; a moved primitive fails the offset test later.
//
// Whole call goes to the stock loop (returns false, nothing written): r.LumenScene.UploadEveryFrame on; another LumenSceneData;
// the external buffer replaced or NumIndices changed (forgets everything first); too many instances, queued primitives or changed
// groups for its tables; a negative offset or a range past NumIndices (forgets everything); any group that doesn't fit what
// the source says (forgets everything); upload buffer not as expected; no primitive to narrow in this call.
//
// The patch: the loop's first instruction (exe+0x1F427B5, right after the upload buffer's Init, volatile registers dead) jumps to
// a stub that calls Decide(LumenSceneData, Scene, upload buffer, NumIndices). True = it filled the upload buffer: continue at the
// stock's own exit for an empty list (exe+0x1F428C2, ResourceUploadTo). False = run the displaced instruction and the stock loop.
// The upload buffer keeps stock's full size (Init is before the patch). A call that sends nothing still sends one redundant
// correct entry, so ResourceUploadTo takes its usual path (NumScatters == 0 returns without its Reset()).
//
// Proof (value 2, lab builds only; the public build doesn't patch the upload call): the call of ResourceUploadTo (exe+0x1F428CC) goes through VerifyUpload first, in every call, stock or narrowed:
// it reads the pairs actually in the mapped upload memory into a CPU copy of the GPU buffer, then evaluates stock's value for every
// instance of every queued primitive and compares. A difference: logged (primitive, instance, destination, value there, value
// stock would send) and the fix goes back to stock until the game ends. On a narrowed call it is also repaired in this same upload if it
// fits, so in value 2 the GPU buffer doesn't get a wrong value even on the first difference. On a stock call it is only reported: the real
// loop wrote that entry, so our evaluation is what's suspect, and a second entry for one destination would race in the parallel
// scatter. Word-by-word reads of write-combined memory: value 2's frame times are no performance evidence. What it can't see: the GPU itself. It trusts the scatter
// compute pass, RDG's copy when the buffer grows, and that the external buffer keeps its contents between frames; stock relies
// on the same three. It compares only what stock would have written in that call, which is all a difference can be.
//
// Single render-thread use: Decide, VerifyUpload and ResetHook run on the render thread only, after UpdateLumenScene waited for the setup task.
// Checked at run time: a call from another thread goes to stock and forgets everything.
#include "common.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace opf {
namespace {

// By offset from UpdateLumenMeshCards' start (found by FindMeshCards below, which checks the loops):
constexpr size_t kEveryAt = 0x39;   // cmp [rip+GLumenSceneUploadEveryFrame],esi
constexpr char kEvery[] = "39 35 ?? ?? ?? ??";
constexpr size_t kExtAt = 0xC01;    // lea rdx,[rdi+SceneInstanceIndexToMeshCardsIndexBuffer]
constexpr char kExt[] = "48 8D 97 ?? ?? ?? ??";
constexpr size_t kNumAt = 0xC43;    // mov [rsp+60h],eax (NumIndices)
constexpr char kNum[] = "89 44 24 60";
constexpr size_t kRegionAt = 0xCE6; // test r8d,r8d; je; lea rax,name; mov r9d,r14d; ...; lea rsi,[rdi+upload]; ...; call Init; mov r13,[rdi+list]; ...; je exit
constexpr char kRegion[] =
    "45 85 C0 0F 84 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 45 8B CE 48 89 44 24 28 48 8D B7 ?? ?? ?? ?? 48 8B CE C6 44 24 20 00 49 8B D7 E8 ?? ?? ?? ?? "
    "4C 8B AF ?? ?? ?? ?? 48 63 87 ?? ?? ?? ?? 4C 89 6C 24 30 48 8D 04 85 00 00 00 00 49 03 C5 48 89 45 80 4C 3B E8 0F 84 ?? ?? ?? ??";
constexpr size_t kRegionUpload = 27, kRegionSite = 47, kRegionExitJe = 84;
constexpr size_t kAddAt = 0xDD0;    // the loop's Add: scatter at [rsi], data at [rsi+8], NumScatters [rsi+10h], element size [rsi+18h]
constexpr char kAdd[] = "8B 4E 10 48 8B 06 89 14 88 8B 4E 10 8B 56 18 44 8B C2 8D 41 01 0F AF CA 48 8D 54 24 78 89 46 10 48 03 4E 08 E8";
constexpr char kExit[] = "4C 8B 45 ?? 49 8B D7 48 8B CE E8";  // mov r8,[rbp-70h]; mov rdx,r15; mov rcx,rsi; call ResourceUploadTo
constexpr size_t kExitCall = 10;
constexpr size_t kSiteLen = 7;  // mov r13,[rdi+list]
// The lists' reset at the end of UpdateLumenScene: mov [rbx+..],esi; cmp dword ptr [rbx+queue max],400h; mov [rbx+queue num],esi; je; mov edx,400h; lea rcx,[rbx+queue];
// call; mov [rbx+..],esi; cmp dword ptr [rbx+groups max],400h; mov [rbx+groups num],esi; je. The patched instruction is the first cmp.
constexpr char kReset[] =
    "89 B3 ?? ?? ?? ?? 81 BB ?? ?? ?? ?? 00 04 00 00 89 B3 ?? ?? ?? ?? 74 11 BA 00 04 00 00 48 8D 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 89 B3 ?? ?? ?? ?? "
    "81 BB ?? ?? ?? ?? 00 04 00 00 89 B3 ?? ?? ?? ?? 74";
constexpr size_t kResetSite = 6, kResetLen = 10;
constexpr size_t kResetQMax = 8, kResetQNum = 18, kResetQLea = 32, kResetGMax = 49, kResetGNum = 59;
// FRDGScatterUploadBuffer::Init (exe+0x270B520): mov [rcx+14h],r8d; mov rsi,rcx; mov dword ptr [rcx+10h],0; mov r10,rdx; mov [rcx+18h],r9d: pins the fields
constexpr size_t kInitAt = 0x25;
constexpr char kInit[] = "44 89 41 14 48 8B F1 C7 41 10 00 00 00 00 4C 8B D2 44 89 49 18";
constexpr size_t kRegionInitCall = 42;
// FRDGScatterUploadBase (UnifiedBuffer.h; Init at exe+0x270B520 writes +0x10, +0x14, +0x18)
constexpr uint32_t kUbScatter = 0, kUbUpload = 8, kUbNum = 0x10, kUbMax = 0x14, kUbStride = 0x18;
// FLumenPrimitiveGroup (LumenSceneData.h): Primitives {inline, secondary, num} at +0, +8, +0x10, PrimitiveInstanceIndex +0x18,
// MeshCardsIndex +0x1C, stride 80. Each field is checked in the game's own code (rev130d L-S4): stride and +0x1C by kLoop, the rest by
// kHasMerged (FLumenPrimitiveGroup::HasMergedInstances, exe+0x1F84DB0) and kRemoveCards (RemoveMeshCards' body, exe+0x1F35E48).
constexpr uint32_t kGroupStride = 80, kGPrimInline = 0, kGPrimSecondary = 8, kGPrimNum = 0x10, kGInstance = 0x18, kGMeshCards = 0x1C;
// cmp dword ptr [rcx+18h],0; jge; mov rax,[rcx+8]; xor r10d,r10d; test rax,rax; mov rdx,rcx; cmovne rdx,rax; movsxd rax,[rcx+10h]; lea r11,[rdx+rax*8]
constexpr char kHasMerged[] = "83 79 18 00 7D ?? 48 8B 41 08 45 33 D2 48 85 C0 48 8B D1 48 0F 45 D0 48 63 41 10 4C 8D 1C C2";
// mov edx,[r14+1Ch]; lea rcx,[r13+..]; call; mov dword ptr [r14+1Ch],-1; mov r15,r14; mov rax,[r14+8]; test; cmovne r15,rax; movsxd rax,[r14+10h]; lea rax,[r15+rax*8]
constexpr char kRemoveCards[] =
    "41 8B 56 1C 49 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 49 C7 46 1C FF FF FF FF 4D 8B FE 49 8B 46 08 48 85 C0 4C 0F 45 F8 49 63 46 10 49 8D 04 C7";

// UpdateLumenMeshCards (exe+0x1F41AA0, LumenMeshCards.cpp:296), found through Lumen::UpdateCardSceneBuffer's last call
// (mov rdx,r12; mov [rsp+20h],rsi; mov rcx,r13; call), with its prologue and its loops checked; the list and layout offsets are read
// from those loops (shared with the lab's LumenCardsProbe).
constexpr char kCallSite[] = "49 8B D4 48 89 74 24 20 49 8B CD E8";
constexpr size_t kCallAt = 11;
constexpr char kFnPro[] = "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 90 FE FF FF 48 81 EC 70 02 00 00";
constexpr size_t kGroupListAt = 0x423;  // mov r10,[rdi+list]; movsxd rax,[rdi+list+8]; lea r11,[r10+rax*4]; cmp r10,r11; je
constexpr char kGroupList[] = "4C 8B 97 ?? ?? ?? ?? 48 63 87 ?? ?? ?? ?? 4D 8D 1C 82 4D 3B D3 0F 84";
constexpr size_t kAllocAt = 0x450;      // movsxd r8,[r10]; cmp r8d,[rdi+groupsNum]; jge; cmp r8d,[rdi+bitsNum]; jge; lea r9,[rdi+bits]; mov rax,[r9+10h]; test; cmovne
constexpr char kAlloc[] = "4D 63 02 44 3B 87 ?? ?? ?? ?? 0F 8D ?? ?? ?? ?? 44 3B 87 ?? ?? ?? ?? 7D ?? 4C 8D 8F ?? ?? ?? ?? 49 8B 41 10 48 85 C0 4C 0F 45 C8";
constexpr size_t kCountAt = 0xC9B;      // mov rdx,[rdi+list]; movsxd rax,[rdi+list+8]; lea r10,[rdx+rax*4]; cmp; je; mov r11d,[rbx+sceneNum]
constexpr char kCount[] = "48 8B 97 ?? ?? ?? ?? 48 63 87 ?? ?? ?? ?? 4C 8D 14 82 49 3B D2 0F 84 ?? ?? ?? ?? 44 8B 9B ?? ?? ?? ??";
constexpr size_t kPrimAt = 0xD53;       // mov rax,[rbx+sceneData]; xor ebx,ebx; mov r14,[rax+rcx*8]; mov r15d,[r14+num]; mov r12d,[r14+offset]
constexpr char kPrim[] = "48 8B 83 ?? ?? ?? ?? 33 DB 4C 8B 34 C8 45 8B BE ?? ?? ?? ?? 45 8B A6 ?? ?? ?? ?? 45 85 FF 0F 8E";
constexpr size_t kLoopAt = 0xD80;       // the instance loop up to mov edx,[rax+rcx*8+1Ch] (stride 80: lea rcx,[rax+rax*4]; add rcx,rcx; [rax+rcx*8])
constexpr char kLoop[] =
    "41 8B 86 ?? ?? ?? ?? 85 C0 7E 33 FF C8 49 8D 96 ?? ?? ?? ?? 48 8B 4A 08 3B D8 0F 4C C3 48 85 C9 48 98 48 0F 45 D1 48 63 04 82 "
    "48 8D 0C 80 48 8B 87 ?? ?? ?? ?? 48 03 C9 8B 54 C8 1C";

constexpr int32_t kMinNarrow = 32;           // smaller primitives go whole: nothing to gain
constexpr int32_t kMaxGroupPrims = 4096;     // a group with more primitives than this is not understood
constexpr uint32_t kMaxDest = 1u << 23;      // instances covered by g_own and g_copy (8 M entries)
constexpr int32_t kMaxQueue = 1 << 16;       // queued primitives per call
constexpr int32_t kMaxGroups = 1 << 16;      // changed groups per call: past it the scan costs real time, so stock and forget

struct Offsets {
    uint32_t list, groupList, groupsData, bits, bitsNum, sceneData, primNumInst, primOffset, primIndices, primGroupsNum, extBuffer, upload;
    const int32_t* uploadEvery;
};
Offsets g_n{};


template <class T>
T Rd(const void* p, uint32_t off) { return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(p) + off); }

// ---- what the fix knows ----
enum : uint8_t { kStateNone, kStateNarrow, kStateFull };
struct Slot { const void* p; int32_t offset, count; uint32_t id; uint32_t tag; uint32_t state; };
constexpr uint32_t kSlotBits = 16, kSlots = 1u << kSlotBits, kProbe = 32;
Slot* g_slots = nullptr;
uint32_t g_used = 0;
uint32_t* g_own = nullptr;    // kMaxDest: id of the last whole send of this destination; 1 = a primitive without a record; 0 = none
uint8_t* g_queueFull = nullptr;  // kMaxQueue: this call: 1 = this queue entry goes whole
struct Pair { uint32_t dest; int32_t value; };
Pair* g_pairs = nullptr;      // kMaxGroups: this call: the changed instances to send
uint32_t g_nextId = 2;
uint32_t g_call = 0;
int g_lastMode = 0;
const void* g_lastLsd = nullptr;
const void* g_lastBuf = nullptr;
uint32_t g_lastNum = 0;

std::atomic<int> g_mode{0};
std::atomic<bool> g_failed{false};
std::atomic<bool> g_ready{false};  // every patch is in place
DWORD g_thread = 0;                // the render thread, from the first call
bool g_ran = false;                // Decide ran since the last reset, for this LumenSceneData
const void* g_ranLsd = nullptr;
int32_t g_processed = 0;           // how much of the changed list Decide went through
double g_qpcToMs = 0;

// The context of this call for VerifyUpload.
struct Ctx { bool valid; bool narrowed; const void* scene; const void* lsd; uint32_t numIndices; };
Ctx g_ctx{};

// Written only by the render thread (load + store, no lock), read by the monitor thread's stats line.
enum Why { kWNoNarrow, kWBuffer, kWEvery, kWTooBig, kWUpload, kWQueue, kWGroups, kWBad, kWAnomaly, kWCapacity, kWCount };
struct Sum {
    std::atomic<uint64_t> calls{0}, narrow{0}, why[kWCount]{}, wouldSend{0}, sent{0}, primsNarrow{0}, primsFull{0}, ns{0}, maxUs{0}, clears{0}, verified{0}, diffs{0},
        unrepaired{0}, verifyNs{0}, seenIndices{0}, resets{0}, noUploadListed{0}, lostQueue{0}, lostEntries{0}, dropped{0}, otherThread{0};
};
Sum g_sum;
inline void Add(std::atomic<uint64_t>& a, uint64_t v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }
inline void Max(std::atomic<uint64_t>& a, uint64_t v) { if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed); }

void ClearAll()
{
    if (!g_used) return;  // a state that sends every call to stock clears only once
    memset(g_slots, 0, kSlots * sizeof(Slot));
    g_used = 0;
    Add(g_sum.clears, 1);
    // g_own is not cleared: ids are never reused, so old entries match nothing.
}

inline uint32_t HashOf(const void* p)
{
    return static_cast<uint32_t>((reinterpret_cast<uint64_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> (64 - kSlotBits));
}
Slot* FindSlot(const void* p, bool add)
{
    uint32_t h = HashOf(p);
    for (uint32_t i = 0; i < kProbe; ++i, h = (h + 1) & (kSlots - 1)) {
        Slot& s = g_slots[h];
        if (s.p == p) return &s;
        if (!s.p) {
            if (!add) return nullptr;
            memset(&s, 0, sizeof s);
            s.p = p;
            ++g_used;
            return &s;
        }
    }
    return nullptr;
}

inline void FillOwn(int32_t off, int32_t n, uint32_t id)
{
    uint32_t* const o = g_own + off;
    for (int32_t i = 0; i < n; ++i) o[i] = id;
}
inline bool OwnAll(int32_t off, int32_t n, uint32_t id)
{
    const uint32_t* const o = g_own + off;
    uint32_t bad = 0;
    for (int32_t i = 0; i < n; ++i) bad |= o[i] ^ id;
    return bad == 0;
}

bool GroupAllocated(const uint8_t* lsd, int32_t i)
{
    if (i < 0 || i >= Rd<int32_t>(lsd, g_n.bitsNum)) return false;
    const uint32_t* words = Rd<const uint32_t*>(lsd, g_n.bits + 0x10);
    if (!words) words = reinterpret_cast<const uint32_t*>(lsd + g_n.bits);
    return (words[static_cast<uint32_t>(i) >> 5] >> (static_cast<uint32_t>(i) & 31)) & 1;
}

inline const int32_t* GroupIndices(const void* p)
{
    const int32_t* sec = Rd<const int32_t*>(p, g_n.primIndices + 8);
    return sec ? sec : reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(p) + g_n.primIndices);
}

// What stock's loop computes for instance k (FLumenSceneData::GetMeshCardsIndex, read in the binary at exe+0x1F42820-0x1F42858).
inline int32_t StockValue(const void* p, int32_t k, const uint8_t* groupsData)
{
    const int32_t groups = Rd<int32_t>(p, g_n.primGroupsNum);
    if (groups <= 0) return -1;
    const int32_t i = k < groups - 1 ? k : groups - 1;
    return Rd<int32_t>(groupsData + static_cast<int64_t>(GroupIndices(p)[i]) * kGroupStride, kGMeshCards);
}

bool StockCall(Why w)
{
    Add(g_sum.why[w], 1);
    return false;
}

// Pass 1 and 2 decide without touching the upload buffer; pass 3 writes. Every path through pass 1 keeps g_own right for both outcomes: a
// primitive marked whole is sent whole by stock or by pass 3, and a narrowed one owns its range already.
bool DecideInner(const void* scene, const void* lsdv, void* ub, uint32_t numIndices, int mode)
{
    const uint8_t* const lsd = static_cast<const uint8_t*>(lsdv);
    Add(g_sum.calls, 1);
    Max(g_sum.seenIndices, numIndices);
    uint32_t* const uNum = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(ub) + kUbNum);
    const uint32_t uMax = Rd<uint32_t>(ub, kUbMax);
    uint32_t* const scatter = Rd<uint32_t*>(ub, kUbScatter);
    int32_t* const data = Rd<int32_t*>(ub, kUbUpload);
    const bool ubOk = Rd<uint32_t>(ub, kUbStride) == 4 && scatter && data && *uNum == 0;
    (void)mode;
    const bool proof = false;
    g_ctx = Ctx{proof, false, scene, lsdv, numIndices};
    g_ran = true;
    g_ranLsd = lsdv;
    g_processed = 0;
    if (!ubOk) { ClearAll(); return StockCall(kWUpload); }
    if (g_n.uploadEvery && *g_n.uploadEvery != 0) { ClearAll(); return StockCall(kWEvery); }
    if (numIndices > kMaxDest) { ClearAll(); return StockCall(kWTooBig); }

    // Another scene's data, a replaced buffer or a new size: forget everything. This call's sends are whole, so records from here on are good.
    const void* const buf = Rd<const void*>(lsd, g_n.extBuffer);
    bool stockThisCall = false;
    if (lsdv != g_lastLsd || buf != g_lastBuf || numIndices != g_lastNum) {
        ClearAll();
        g_lastLsd = lsdv;
        g_lastBuf = buf;
        g_lastNum = numIndices;
        stockThisCall = true;
    }
    if (g_used > kSlots / 4 * 3) ClearAll();
    const uint32_t call = ++g_call;
    if (g_nextId > 0xFFFE0000u) { ClearAll(); memset(g_own, 0, kMaxDest * sizeof(uint32_t)); g_nextId = 2; }

    const int32_t* const list = Rd<const int32_t*>(lsd, g_n.list);
    const int32_t listNum = Rd<int32_t>(lsd, g_n.list + 8);
    void* const* const prims = Rd<void* const*>(scene, g_n.sceneData);
    const int32_t primNum = Rd<int32_t>(scene, g_n.sceneData + 8);
    if (listNum > kMaxQueue) { ClearAll(); return StockCall(kWQueue); }

    // Pass 1: each queued primitive, whole or narrow.
    uint64_t wouldSend = 0, whole = 0;
    uint32_t primsNarrow = 0, primsFull = 0;
    const void* firstNarrow = nullptr;
    for (int32_t i = 0; i < listNum; ++i) {
        g_queueFull[i] = 0;
        const int32_t idx = list[i];
        if (idx < 0) { ClearAll(); return StockCall(kWBad); }
        if (idx >= primNum) continue;  // the stock loop skips these too
        const void* const p = prims[idx];
        const int32_t n = Rd<int32_t>(p, g_n.primNumInst);
        if (n <= 0) continue;
        const int32_t off = Rd<int32_t>(p, g_n.primOffset);
        if (off < 0 || static_cast<uint64_t>(off) + static_cast<uint64_t>(n) > numIndices) { ClearAll(); return StockCall(kWBad); }
        wouldSend += static_cast<uint64_t>(n);
        const bool eligible = n >= kMinNarrow && Rd<int32_t>(p, g_n.primGroupsNum) == n;
        Slot* const s = eligible ? FindSlot(p, true) : nullptr;
        if (s && s->id && s->offset == off && s->count == n && OwnAll(off, n, s->id)) {
            s->tag = call;
            s->state = kStateNarrow;
            ++primsNarrow;
            if (!firstNarrow) firstNarrow = p;
            continue;
        }
        g_queueFull[i] = 1;
        whole += static_cast<uint64_t>(n);
        ++primsFull;
        if (s) {
            s->offset = off;
            s->count = n;
            s->id = g_nextId++;
            s->tag = call;
            s->state = kStateFull;
            FillOwn(off, n, s->id);
        } else {
            FillOwn(off, n, 1);
        }
    }

    // Pass 2: the changed groups. A primitive that was not queued loses its record, as stock would re-send it whole next time.
    const int32_t* const gl = Rd<const int32_t*>(lsd, g_n.groupList);
    const int32_t glNum = Rd<int32_t>(lsd, g_n.groupList + 8);
    if (glNum > kMaxGroups) { ClearAll(); return StockCall(kWGroups); }
    const uint8_t* const groups = Rd<const uint8_t*>(lsd, g_n.groupsData);
    const int32_t groupsNum = Rd<int32_t>(lsd, g_n.groupsData + 8);
    int32_t pairs = 0;
    for (int32_t i = 0; i < glNum; ++i) {
        const int32_t g = gl[i];
        if (g < 0 || g >= groupsNum || !GroupAllocated(lsd, g)) continue;  // as the stock upload of the groups: a removed group has no primitive left
        const uint8_t* const e = groups + static_cast<int64_t>(g) * kGroupStride;
        const int32_t pn = Rd<int32_t>(e, kGPrimNum);
        if (pn < 0 || pn > kMaxGroupPrims) { ClearAll(); return StockCall(kWAnomaly); }
        const void* const sec = Rd<const void*>(e, kGPrimSecondary);
        const void* const* const pa = sec ? static_cast<const void* const*>(sec) : reinterpret_cast<const void* const*>(e + kGPrimInline);
        for (int32_t j = 0; j < pn; ++j) {
            const void* const p = pa[j];  // only a key unless it is queued in this call
            Slot* const s = FindSlot(p, false);
            if (!s) continue;
            if (s->tag != call) { s->id = 0; continue; }  // changed, not queued
            if (s->state != kStateNarrow) continue;        // goes whole
            const int32_t inst = Rd<int32_t>(e, kGInstance);
            if (pn != 1 || inst < 0 || inst >= s->count || GroupIndices(p)[inst] != g) { ClearAll(); return StockCall(kWAnomaly); }
            g_pairs[pairs++] = Pair{static_cast<uint32_t>(s->offset + inst), Rd<int32_t>(e, kGMeshCards)};
        }
    }
    g_processed = glNum;

    if (stockThisCall) return StockCall(kWBuffer);
    if (!primsNarrow) return StockCall(kWNoNarrow);
    const uint64_t planned = whole + static_cast<uint64_t>(pairs) + (whole + static_cast<uint64_t>(pairs) ? 0 : 1);
    if (planned > uMax) { ClearAll(); return StockCall(kWCapacity); }

    // Pass 3: the writes, as stock's Add does them.
    uint32_t ns = 0;
    for (int32_t i = 0; i < listNum; ++i) {
        if (!g_queueFull[i]) continue;
        const void* const p = prims[list[i]];
        const int32_t n = Rd<int32_t>(p, g_n.primNumInst);
        const int32_t off = Rd<int32_t>(p, g_n.primOffset);
        for (int32_t k = 0; k < n; ++k) {
            scatter[ns] = static_cast<uint32_t>(off + k);
            data[ns] = StockValue(p, k, groups);
            ++ns;
        }
    }
    for (int32_t i = 0; i < pairs; ++i) {
        scatter[ns] = g_pairs[i].dest;
        data[ns] = g_pairs[i].value;
        ++ns;
    }
    if (!ns) {  // nothing changed: one redundant correct entry, so the upload runs its usual path
        scatter[0] = static_cast<uint32_t>(Rd<int32_t>(firstNarrow, g_n.primOffset));
        data[0] = StockValue(firstNarrow, 0, groups);
        ns = 1;
    }
    *uNum = ns;
    g_ctx.narrowed = true;
    Add(g_sum.narrow, 1);
    Add(g_sum.wouldSend, wouldSend);
    Add(g_sum.sent, ns);
    Add(g_sum.primsNarrow, primsNarrow);
    Add(g_sum.primsFull, primsFull);
    return true;
}

// One thread only; another one goes to stock and everything is forgotten (it becomes the thread from then on).
bool SameThread()
{
    const DWORD t = GetCurrentThreadId();
    if (t == g_thread) return true;
    if (g_thread) {
        ClearAll();
        Add(g_sum.otherThread, 1);
        Log("[lumennarrow] called on thread %lu, was %lu: stock for this call, everything forgotten", t, g_thread);
        g_thread = t;
        return false;
    }
    g_thread = t;
    return true;
}

bool Decide(const void* lsd, const void* scene, void* ub, uint32_t numIndices)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    g_ctx.valid = false;
    if (!g_ready.load(std::memory_order_relaxed) || !SameThread()) return false;
    if (mode != g_lastMode) {  // nothing known carries over a switch; value 2 needs every narrowed range known in its copy
        ClearAll();
        g_lastMode = mode;
    }
    if (!mode || g_failed.load(std::memory_order_relaxed)) return false;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const bool r = DecideInner(scene, lsd, ub, numIndices, mode);
    QueryPerformanceCounter(&t1);
    const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * g_qpcToMs;
    Add(g_sum.ns, static_cast<uint64_t>(ms * 1e6));
    Max(g_sum.maxUs, static_cast<uint64_t>(ms * 1000.0));
    return r;
}

// Every UpdateLumenScene, right before the lists are emptied, Lumen active or not. Whatever is listed and Decide didn't go through (it wasn't reached, or the group came later)
// wasn't sent: stock would heal it at the primitive's next queue, so that primitive's next send is whole. The counters say how often a call has listed groups with no upload,
// and how often queue entries are lost without an upload: the second is stock's behaviour in water frames, see findings lumenfixa.
std::atomic<void (*)(const void*)> g_resetObserver{nullptr};  // LumenScanCull: sees the lists before they are emptied
std::atomic<bool> g_resetPatched{false};
void ResetOnlyHook(const void* lsdv)  // the reset-only patch's target (LumenCardsNarrow not installed)
{
    if (auto* const f = g_resetObserver.load(std::memory_order_acquire)) f(lsdv);
}

void ResetHook(const void* lsdv)
{
    if (auto* const f = g_resetObserver.load(std::memory_order_acquire)) f(lsdv);
    const uint8_t* const lsd = static_cast<const uint8_t*>(lsdv);
    if (!g_ready.load(std::memory_order_relaxed)) { g_ran = false; return; }
    if (!SameThread()) { g_ran = false; return; }
    // The forget on a value change, as in Decide, which a call with 0 queued instances doesn't reach: nothing is tracked while off, so a record must not outlive an off window.
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode != g_lastMode) {
        ClearAll();
        g_lastMode = mode;
    }
    if (!mode || g_failed.load(std::memory_order_relaxed)) { g_ran = false; return; }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    Add(g_sum.resets, 1);
    const bool ran = g_ran && g_ranLsd == lsdv;
    g_ran = false;
    const int32_t listNum = Rd<int32_t>(lsd, g_n.list + 8);
    const int32_t glNum = Rd<int32_t>(lsd, g_n.groupList + 8);
    if (!ran) {
        if (glNum > 0) Add(g_sum.noUploadListed, 1);
        if (listNum > 0) {
            Add(g_sum.lostQueue, 1);
            Add(g_sum.lostEntries, static_cast<uint64_t>(listNum));
        }
    }
    const int32_t from = ran ? g_processed : 0;
    if (g_used && glNum > from) {
        if (glNum - from > kMaxGroups) {
            ClearAll();
        } else {
            const int32_t* const gl = Rd<const int32_t*>(lsd, g_n.groupList);
            const uint8_t* const groups = Rd<const uint8_t*>(lsd, g_n.groupsData);
            const int32_t groupsNum = Rd<int32_t>(lsd, g_n.groupsData + 8);
            for (int32_t i = from; i < glNum; ++i) {
                const int32_t g = gl[i];
                if (g < 0 || g >= groupsNum || !GroupAllocated(lsd, g)) continue;
                const uint8_t* const e = groups + static_cast<int64_t>(g) * kGroupStride;
                const int32_t pn = Rd<int32_t>(e, kGPrimNum);
                if (pn < 0 || pn > kMaxGroupPrims) { ClearAll(); break; }
                const void* const sec = Rd<const void*>(e, kGPrimSecondary);
                const void* const* const pa = sec ? static_cast<const void* const*>(sec) : reinterpret_cast<const void* const*>(e + kGPrimInline);
                for (int32_t j = 0; j < pn; ++j) {
                    Slot* const s = FindSlot(pa[j], false);
                    if (s && s->id) {
                        s->id = 0;
                        Add(g_sum.dropped, 1);
                    }
                }
            }
        }
    }
    QueryPerformanceCounter(&t1);
    Add(g_sum.ns, static_cast<uint64_t>(static_cast<double>(t1.QuadPart - t0.QuadPart) * g_qpcToMs * 1e6));
}


// 0 stock, 1 the fix; 2 the fix with proof in lab builds (the public build reads 2 as 1); anything else is off.
int KeyValue(int raw)
{
    return raw == 1 || raw == 2 ? 1 : 0;
}

struct Snap { uint64_t calls, narrow, why[kWCount], wouldSend, sent, primsNarrow, primsFull, ns, clears, verified, diffs, unrepaired, verifyNs, resets, noUploadListed, lostQueue, lostEntries, dropped, otherThread; };
Snap g_last{};

Snap Take()
{
    Snap n{};
    const auto L = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    n.calls = L(g_sum.calls); n.narrow = L(g_sum.narrow); n.wouldSend = L(g_sum.wouldSend); n.sent = L(g_sum.sent); n.primsNarrow = L(g_sum.primsNarrow);
    n.primsFull = L(g_sum.primsFull); n.ns = L(g_sum.ns); n.clears = L(g_sum.clears); n.verified = L(g_sum.verified); n.diffs = L(g_sum.diffs);
    n.unrepaired = L(g_sum.unrepaired); n.verifyNs = L(g_sum.verifyNs); n.resets = L(g_sum.resets); n.noUploadListed = L(g_sum.noUploadListed); n.lostQueue = L(g_sum.lostQueue);
    n.lostEntries = L(g_sum.lostEntries); n.dropped = L(g_sum.dropped); n.otherThread = L(g_sum.otherThread);
    for (int i = 0; i < kWCount; ++i) n.why[i] = L(g_sum.why[i]);
    return n;
}

struct Sites {
    uint8_t* fn, *site, *exitTo, *uploadCall, *resetSite;
    Offsets o;
};

// The lists' reset: found by its pattern, with the list offsets the loop's code gave. Exactly one.
bool DiscoverReset(const GameModule& game, uint32_t list, uint32_t groupList, uint8_t** site, const char** why)
{
    const auto U32 = [](const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; };
    uint8_t* found = nullptr;
    for (uint8_t* h : FindAll(game.text, ParsePattern(kReset))) {
        if (U32(h + kResetQMax) != list + 0xC || U32(h + kResetQNum) != list + 8 || U32(h + kResetQLea) != list || U32(h + kResetGMax) != groupList + 0xC || U32(h + kResetGNum) != groupList + 8) continue;
        if (found) { if (why) *why = "the lists' reset found twice"; return false; }
        found = h;
    }
    if (!found) { if (why) *why = "the lists' reset not found"; return false; }
    *site = found + kResetSite;
    return true;
}

// UpdateLumenMeshCards through its caller, with its loops checked, and the offsets its own code reads.
bool FindMeshCards(const GameModule& game, uint8_t** callOut, uint8_t** fnOut, LumenCardsOffsets* o, const char** why)
{
    const auto sites = FindAll(game.text, ParsePattern(kCallSite));
    const Pattern pro = ParsePattern(kFnPro);
    uint8_t* call = nullptr;
    uint8_t* fn = nullptr;
    for (uint8_t* s : sites) {
        uint8_t* const c = s + kCallAt;
        uint8_t* const t = RipTarget(c, 1, 5);
        if (!game.text.Contains(t) || !game.text.Contains(t + 0x1000) || !MatchesAt(t, pro)) continue;
        if (!MatchesAt(t + kCountAt, ParsePattern(kCount))) continue;
        if (call) { *why = "the call to UpdateLumenMeshCards found twice"; return false; }
        call = c;
        fn = t;
    }
    if (!call) { *why = "the call to UpdateLumenMeshCards not found"; return false; }
    if (!MatchesAt(fn + kGroupListAt, ParsePattern(kGroupList)) || !MatchesAt(fn + kAllocAt, ParsePattern(kAlloc)) ||
        !MatchesAt(fn + kPrimAt, ParsePattern(kPrim)) || !MatchesAt(fn + kLoopAt, ParsePattern(kLoop))) {
        *why = "the loops inside UpdateLumenMeshCards do not match";
        return false;
    }
    const auto U32 = [](const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; };
    const uint8_t* c = fn + kCountAt;
    o->list = U32(c + 3);
    const uint32_t sceneNum = U32(c + 30);
    const uint8_t* p = fn + kPrimAt;
    o->sceneData = U32(p + 3);
    o->primNumInst = U32(p + 16);
    o->primOffset = U32(p + 23);
    const uint8_t* l = fn + kLoopAt;
    o->primGroupsNum = U32(l + 3);
    o->primIndices = U32(l + 16);
    o->groupsData = U32(l + 49);
    const uint8_t* g = fn + kGroupListAt;
    o->groupList = U32(g + 3);
    const uint8_t* a = fn + kAllocAt;
    const uint32_t groupsNum = U32(a + 6);
    o->bitsNum = U32(a + 19);
    o->bits = U32(a + 28);
    // Internal consistency: {data, num} pairs, as the stock code reads them.
    if (U32(c + 10) != o->list + 8 || sceneNum != o->sceneData + 8 || U32(g + 10) != o->groupList + 8 || groupsNum != o->groupsData + 8 ||
        o->bitsNum != o->bits + 0x18) {
        *why = "the offsets read from the code are not consistent";
        return false;
    }
    *callOut = call;
    *fnOut = fn;
    return true;
}

// FLumenPrimitiveGroup's fields as the game's own code reads them (kLoop already pinned the stride and MeshCardsIndex): exactly one of each.
bool GroupLayoutChecked(const GameModule& game, const char** why)
{
    if (FindAll(game.text, ParsePattern(kHasMerged)).size() != 1) { *why = "FLumenPrimitiveGroup::HasMergedInstances (the group's primitives and instance index) not found once"; return false; }
    if (FindAll(game.text, ParsePattern(kRemoveCards)).size() != 1) { *why = "RemoveMeshCards (the group's mesh cards index and primitives) not found once"; return false; }
    return true;
}

// privateCopy: the self-test's copy of the function only; Init and the group layout's code aren't in it. The real game always checks both.
bool Discover(const GameModule& game, Sites* f, const char** why, bool privateCopy = false)
{
    uint8_t* call = nullptr;
    uint8_t* fn = nullptr;
    LumenCardsOffsets c{};
    if (!FindMeshCards(game, &call, &fn, &c, why)) return false;
    if (!privateCopy && !GroupLayoutChecked(game, why)) return false;
    if (!MatchesAt(fn + kEveryAt, ParsePattern(kEvery)) || !MatchesAt(fn + kExtAt, ParsePattern(kExt)) || !MatchesAt(fn + kNumAt, ParsePattern(kNum)) ||
        !MatchesAt(fn + kRegionAt, ParsePattern(kRegion)) || !MatchesAt(fn + kAddAt, ParsePattern(kAdd))) {
        *why = "the code around the instance loop does not match";
        return false;
    }
    const auto U32 = [](const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; };
    uint8_t* const region = fn + kRegionAt;
    uint8_t* const site = region + kRegionSite;
    uint8_t* const exitTo = RipTarget(region + kRegionExitJe, 2, 6);
    if (!game.text.Contains(exitTo) || !MatchesAt(exitTo, ParsePattern(kExit))) { *why = "the loop's exit does not match"; return false; }
    uint8_t* const uploadCall = exitTo + kExitCall;
    if (U32(site + 3) != c.list) { *why = "the loop's list offset is not the one read before"; return false; }
    if (U32(fn + kExtAt + 3) + 8 != U32(region + kRegionUpload)) { *why = "the buffer and its upload buffer are not neighbours"; return false; }
    uint8_t* const init = RipTarget(region + kRegionInitCall, 1, 5);
    if (!privateCopy && (!game.text.Contains(init) || !game.text.Contains(init + 0x40) || !MatchesAt(init + kInitAt, ParsePattern(kInit)))) { *why = "the upload buffer's Init does not match"; return false; }
    Offsets o{};
    o.list = c.list;
    o.groupList = c.groupList;
    o.groupsData = c.groupsData;
    o.bits = c.bits;
    o.bitsNum = c.bitsNum;
    o.sceneData = c.sceneData;
    o.primNumInst = c.primNumInst;
    o.primOffset = c.primOffset;
    o.primGroupsNum = c.primGroupsNum;
    o.primIndices = c.primIndices;
    o.extBuffer = U32(fn + kExtAt + 3);
    o.upload = U32(region + kRegionUpload);
    o.uploadEvery = reinterpret_cast<const int32_t*>(RipTarget(fn + kEveryAt, 2, 6));
    f->fn = fn;
    f->site = site;
    f->exitTo = exitTo;
    f->uploadCall = uploadCall;
    f->o = o;
    return true;
}

// The stub the site jumps to. Volatile registers are dead at the site (right after Init returned); rsp is 16-aligned there.
constexpr size_t kStubBytes = 80;
size_t BuildStub(uint8_t* m, const void* decide, const uint8_t* displaced, const uint8_t* stockBack, const uint8_t* handled)
{
    const auto Put = [&m](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *m++ = x; };
    const auto Ptr = [&m](const void* p) { memcpy(m, &p, 8); m += 8; };
    uint8_t* const start = m;
    Put({0x48, 0x8B, 0xCF});                    // mov rcx,rdi   LumenSceneData
    Put({0x48, 0x8B, 0xD3});                    // mov rdx,rbx   Scene
    Put({0x4C, 0x8B, 0xC6});                    // mov r8,rsi    the upload buffer
    Put({0x44, 0x8B, 0x4C, 0x24, 0x60});        // mov r9d,[rsp+60h]  NumIndices
    Put({0x48, 0x83, 0xEC, 0x20});              // sub rsp,20h
    Put({0x48, 0xB8}); Ptr(decide);             // mov rax,Decide
    Put({0xFF, 0xD0});                          // call rax
    Put({0x48, 0x83, 0xC4, 0x20});              // add rsp,20h
    Put({0x84, 0xC0});                          // test al,al
    Put({0x75, static_cast<uint8_t>(kSiteLen + 14)});  // jne handled
    for (size_t i = 0; i < kSiteLen; ++i) *m++ = displaced[i];  // mov r13,[rdi+list]
    Put({0xFF, 0x25, 0, 0, 0, 0}); Ptr(stockBack);     // jmp to the stock loop
    Put({0xFF, 0x25, 0, 0, 0, 0}); Ptr(handled);       // handled: jmp to the stock's exit
    return static_cast<size_t>(m - start);
}

constexpr size_t kStubCallReturn = 3 + 3 + 3 + 5 + 4 + 10 + 2;  // the return address of the stub's call
constexpr size_t kResetCallReturn = 3 + 4 + 10 + 2;              // the same in the reset stub

// At the lists' reset: flags come from the displaced compare, run after the call. Volatile registers are dead there (the code after it sets what it uses).
size_t BuildResetStub(uint8_t* m, const void* target, const uint8_t* displaced, const uint8_t* back)
{
    const auto Put = [&m](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *m++ = x; };
    const auto Ptr = [&m](const void* p) { memcpy(m, &p, 8); m += 8; };
    uint8_t* const start = m;
    Put({0x48, 0x8B, 0xCB});              // mov rcx,rbx   LumenSceneData
    Put({0x48, 0x83, 0xEC, 0x20});        // sub rsp,20h
    Put({0x48, 0xB8}); Ptr(target);       // mov rax,ResetHook
    Put({0xFF, 0xD0});                    // call rax
    Put({0x48, 0x83, 0xC4, 0x20});        // add rsp,20h
    for (size_t i = 0; i < kResetLen; ++i) *m++ = displaced[i];  // cmp dword ptr [rbx+queue max],400h
    Put({0xFF, 0x25, 0, 0, 0, 0}); Ptr(back);                    // jmp back (leaves the flags)
    return static_cast<size_t>(m - start);
}

bool PatchJmp(uint8_t* site, size_t len, uint8_t* stub)
{
    uint8_t jmp[16];
    memset(jmp, 0x90, sizeof jmp);
    const int64_t d = stub - (site + 5);
    const int32_t rel = static_cast<int32_t>(d);
    if (d != rel || len < 5 || len > sizeof jmp) return false;
    jmp[0] = 0xE9;
    memcpy(jmp + 1, &rel, 4);
    return WriteCode(site, std::span<const uint8_t>(jmp, len));
}

bool PatchCall(uint8_t* call, uint8_t* target)
{
    const int64_t d = target - (call + 5);
    const int32_t rel = static_cast<int32_t>(d);
    if (d != rel) return false;
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel), 4));
}

// The game's unwind record of the function holding pc, from the image's exception directory. nullptr if none, or a chained one.
const uint8_t* FindUnwindInfo(const GameModule& game, const uint8_t* pc)
{
    const uint8_t* const b = game.base;
    const uint32_t pe = *reinterpret_cast<const uint32_t*>(b + 0x3C);
    const uint32_t dirRva = *reinterpret_cast<const uint32_t*>(b + pe + 24 + 112 + 3 * 8);
    const uint32_t dirSize = *reinterpret_cast<const uint32_t*>(b + pe + 24 + 112 + 3 * 8 + 4);
    const auto* const rf = reinterpret_cast<const RUNTIME_FUNCTION*>(b + dirRva);
    const uint32_t rva = static_cast<uint32_t>(pc - b);
    size_t lo = 0, hi = dirSize / sizeof(RUNTIME_FUNCTION);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (rva < rf[mid].BeginAddress) hi = mid;
        else if (rva >= rf[mid].EndAddress) lo = mid + 1;
        else {
            const uint8_t* const info = b + rf[mid].UnwindData;
            return (info[0] >> 3) & 4 ? nullptr : info;  // UNW_FLAG_CHAININFO
        }
    }
    return nullptr;
}

// Unwind data for a stub entered by a jump from the middle of a game function: the stub's own 20h, then that function's frame, so a stack walk through the stub arrives at the function's
// caller (the function itself is not shown). No handler. Prolog size 0 and every code's prolog offset 0: the unwinder applies a code only if pc's offset in the stub is at
// least the code's offset, and the copied offsets are the game function's, so they are cleared. Unknown codes refuse.
constexpr size_t kUnwindMax = 4 + 2 * 34;
size_t BuildUnwind(uint8_t* out, const uint8_t* info)
{
    const uint8_t count = info[2];
    if ((info[0] & 7) != 1 || count > 32) return 0;
    out[0] = 0x01;
    out[1] = 0;
    out[2] = static_cast<uint8_t>(count + 1);
    out[3] = info[3];
    out[4] = 0;
    out[5] = static_cast<uint8_t>(2 | ((0x20 / 8 - 1) << 4));  // UWOP_ALLOC_SMALL 20h
    memcpy(out + 6, info + 4, count * 2u);
    for (size_t i = 0; i < count;) {
        const uint8_t op = out[6 + i * 2 + 1] & 15, opInfo = out[6 + i * 2 + 1] >> 4;
        size_t slots = 0;
        switch (op) {
        case 0: case 2: case 3: case 10: slots = 1; break;  // push, small alloc, frame register, machine frame
        case 1: slots = opInfo ? 3 : 2; break;              // large alloc
        case 4: case 8: slots = 2; break;                   // save register, save xmm
        case 5: case 9: slots = 3; break;                   // the far forms
        default: return 0;
        }
        out[6 + i * 2] = 0;
        i += slots;
    }
    return 6 + count * 2u;
}

void WriteJmpAbs(uint8_t* m, const void* target)
{
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(m, jmpAbs, 6);
    memcpy(m + 6, &target, 8);
}

bool AllocTables(bool proof)
{
    const auto A = [](size_t n) { return VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); };
    g_slots = static_cast<Slot*>(A(kSlots * sizeof(Slot)));
    g_own = static_cast<uint32_t*>(A(kMaxDest * sizeof(uint32_t)));
    (void)proof;
    g_queueFull = static_cast<uint8_t*>(A(kMaxQueue));
    g_pairs = static_cast<Pair*>(A(kMaxGroups * sizeof(Pair)));
    return g_slots && g_own && g_queueFull && g_pairs;
}

}  // namespace

// The public wrapper of the discovery, for the lab's LumenCardsProbe (one copy of the patterns).
bool LumenMeshCardsDiscover(const GameModule& game, uint8_t** call, uint8_t** fn, LumenCardsOffsets* o, const char** why)
{
    return FindMeshCards(game, call, fn, o, why);
}

// Monitor thread, every second (config reload): the key, live once installed. The render thread notices a change of value at its next
// call (Decide / ResetHook forget everything then).
void SetLumenCardsNarrow(int value)
{
    if (!g_ready.load(std::memory_order_relaxed)) return;
    static int s_warned = 0;
    if (value != 0 && value != 1 && value != 2 && value != s_warned) {
        s_warned = value;
        Log("[lumennarrow] LumenCardsNarrow=%d is not a value: off (0 stock, 1 fix)", value);
    }
    int v = KeyValue(value);
    if (v != g_mode.load(std::memory_order_relaxed)) {
        g_mode = v;
        Log("[lumennarrow] LumenCardsNarrow=%d", v);
    }
}

// Stats interval: what the fix did since the last line (no line when nothing ran).
void LogLumenCardsNarrowStats()
{
    if (!g_ready.load(std::memory_order_relaxed)) return;
    const Snap n = Take();
    Snap& last = g_last;
    const auto D = [&](uint64_t Snap::*f) { return static_cast<unsigned long long>(n.*f - last.*f); };
    if (n.calls == last.calls && n.resets == last.resets) return;
    Log("[lumennarrow] stats: mode %d, uploads %llu, narrowed %llu, instances stock would send %llu, sent %llu, render-thread ms %.3f, forgets %llu",
        g_mode.load(), D(&Snap::calls), D(&Snap::narrow), D(&Snap::wouldSend), D(&Snap::sent), static_cast<double>(n.ns - last.ns) / 1e6, D(&Snap::clears));
    last = n;
}

// Start-up only (version.dll): the loop and the lists' reset (the lab's proof also redirects the upload call). key = LumenCardsNarrow at start-up;
// 0 = not installed.
bool InstallLumenCardsNarrow(bool beforeGameCode, int key)
{
    if (!key) { Log("[lumennarrow] LumenCardsNarrow off in OblivionPerfFix.ini (takes a restart to switch on)"); return false; }
    if (!beforeGameCode) { Log("[lumennarrow] LumenCardsNarrow: start-up only (version.dll), not installed"); return false; }
    const GameModule& game = Game();
    Sites f{};
    const char* why = "";
    if (!Discover(game, &f, &why)) { Log("[lumennarrow] LumenCardsNarrow: %s: not installed", why); return false; }
    if (!DiscoverReset(game, f.o.list, f.o.groupList, &f.resetSite, &why)) { Log("[lumennarrow] LumenCardsNarrow: %s: not installed", why); return false; }
    if (!game.data.Contains(f.o.uploadEvery)) { Log("[lumennarrow] LumenCardsNarrow: r.LumenScene.UploadEveryFrame not in .data: not installed"); return false; }
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    g_qpcToMs = 1000.0 / static_cast<double>(fq.QuadPart);
    const int startMode = KeyValue(key);
    if (!AllocTables(startMode == 2)) { Log("[lumennarrow] LumenCardsNarrow: no memory: not installed"); return false; }
    // The block: loop stub, reset stub, (lab) upload jump, their unwind data, the function table.
    constexpr size_t kOffReset = kStubBytes, kOffUpload = kOffReset + 64, kOffU1 = kOffUpload + 16, kOffU2 = kOffU1 + kUnwindMax, kOffTable = kOffU2 + kUnwindMax, kBlock = kOffTable + 2 * sizeof(RUNTIME_FUNCTION);
    uint8_t* const mem = AllocNearGame(kBlock);
    if (!mem) { Log("[lumennarrow] LumenCardsNarrow: no memory near the game's code: not installed"); return false; }
    const size_t len = BuildStub(mem, reinterpret_cast<const void*>(&Decide), f.site, f.site + kSiteLen, f.exitTo);
    const size_t lenReset = BuildResetStub(mem + kOffReset, reinterpret_cast<const void*>(&ResetHook), f.resetSite, f.resetSite + kResetLen);
    if (len > kStubBytes || lenReset > kOffUpload - kOffReset) { Log("[lumennarrow] LumenCardsNarrow: stub too long: not installed"); return false; }
    const uint8_t* const infoLoop = FindUnwindInfo(game, f.site);
    const uint8_t* const infoReset = FindUnwindInfo(game, f.resetSite);
    const bool unwind = infoLoop && infoReset && BuildUnwind(mem + kOffU1, infoLoop) && BuildUnwind(mem + kOffU2, infoReset);
    auto* const table = reinterpret_cast<RUNTIME_FUNCTION*>(mem + kOffTable);
    table[0].BeginAddress = 0;
    table[0].EndAddress = static_cast<DWORD>(len);
    table[0].UnwindData = static_cast<DWORD>(kOffU1);
    table[1].BeginAddress = static_cast<DWORD>(kOffReset);
    table[1].EndAddress = static_cast<DWORD>(kOffReset + lenReset);
    table[1].UnwindData = static_cast<DWORD>(kOffU2);
    DWORD old;
    VirtualProtect(mem, kBlock, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, kBlock);
    if (!unwind || !RtlAddFunctionTable(table, 2, reinterpret_cast<DWORD64>(mem))) Log("[lumennarrow] no unwind data for the stubs: a stack in a dump won't walk past them");
    g_n = f.o;
    g_mode = startMode;
    g_lastMode = g_mode.load();
    // Decide stays off (g_ready) until every patch is in. The loop goes last: a failure before it leaves the game's loop running as stock.
    if (!PatchJmp(f.resetSite, kResetLen, mem + kOffReset)) { Log("[lumennarrow] LumenCardsNarrow: could not patch the lists' reset: stays stock"); return false; }
    g_resetPatched = true;
    if (!PatchJmp(f.site, kSiteLen, mem)) { Log("[lumennarrow] LumenCardsNarrow: could not patch the loop: stays stock (the reset hook already in does nothing)"); return false; }
    g_ready = true;
    const char* const modes = "0 stock, 1 fix";
    Log("[lumennarrow] LumenCardsNarrow installed: loop exe+0x%X (exit exe+0x%X), lists' reset exe+0x%X; list +0x%X, changed groups +0x%X, groups +0x%X, buffer +0x%X, upload buffer +0x%X; "
        "group layout checked; mode %d (%s; live)",
        static_cast<unsigned>(f.site - game.base), static_cast<unsigned>(f.exitTo - game.base), static_cast<unsigned>(f.resetSite - game.base), g_n.list, g_n.groupList, g_n.groupsData, g_n.extBuffer,
        g_n.upload, g_mode.load(), modes);
    return true;
}

// LumenScanCull (public since 1.4.0): an observer at the lists' reset. Through LumenCardsNarrow's reset stub when that is in (ResetHook calls it first),
// else a reset-only stub at the same site (start-up only, after InstallLumenCardsNarrow).
bool LumenListsResetObserve(void (*observer)(const void*), const char** why)
{
    if (!g_resetPatched.load()) {
        const GameModule& game = Game();
        Sites f{};
        if (!Discover(game, &f, why)) return false;
        if (!DiscoverReset(game, f.o.list, f.o.groupList, &f.resetSite, why)) return false;
        constexpr size_t kOffU = 64, kOffTable = kOffU + kUnwindMax, kBlock = kOffTable + sizeof(RUNTIME_FUNCTION);
        uint8_t* const mem = AllocNearGame(kBlock);
        if (!mem) { *why = "no memory near the game's code"; return false; }
        const size_t len = BuildResetStub(mem, reinterpret_cast<const void*>(&ResetOnlyHook), f.resetSite, f.resetSite + kResetLen);
        if (len > kOffU) { *why = "stub too long"; return false; }
        const uint8_t* const info = FindUnwindInfo(game, f.resetSite);
        auto* const table = reinterpret_cast<RUNTIME_FUNCTION*>(mem + kOffTable);
        table[0].BeginAddress = 0;
        table[0].EndAddress = static_cast<DWORD>(len);
        table[0].UnwindData = static_cast<DWORD>(kOffU);
        const bool unwind = info && BuildUnwind(mem + kOffU, info);
        DWORD old;
        VirtualProtect(mem, kBlock, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), mem, kBlock);
        if (!unwind || !RtlAddFunctionTable(table, 1, reinterpret_cast<DWORD64>(mem))) Log("[lumennarrow] no unwind data for the reset-only stub");
        g_resetObserver = observer;  // before the patch: the stub calls it from the first reset on
        if (!PatchJmp(f.resetSite, kResetLen, mem)) { g_resetObserver = nullptr; *why = "could not patch the lists' reset"; return false; }
        g_resetPatched = true;
        Log("[lumennarrow] the lists' reset (exe+0x%X) patched for an observer only (LumenCardsNarrow not installed)", static_cast<unsigned>(f.resetSite - game.base));
        return true;
    }
    g_resetObserver = observer;
    return true;
}

// ---- self-test ----
int LumenNarrowTestDiscover(const GameModule& game, uint32_t* rvas, uint32_t* offs)
{
    Sites f{};
    const char* why = "";
    if (!Discover(game, &f, &why)) return 0;
    rvas[0] = static_cast<uint32_t>(f.site - game.base);
    rvas[1] = static_cast<uint32_t>(f.exitTo - game.base);
    rvas[2] = static_cast<uint32_t>(f.uploadCall - game.base);
    rvas[3] = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(f.o.uploadEvery) - game.base);
    offs[0] = f.o.extBuffer;
    offs[1] = f.o.upload;
    offs[2] = f.o.primIndices;
    return 1;
}

// Discovery on a private copy of the function, with one byte of each checked place changed: each must refuse. Also the patch on the copy.
int LumenNarrowTestMismatch(const GameModule& game)
{
    // The copy: the pages from UpdateLumenMeshCards' start, 0x3000 bytes (its caller and its exit are in them; checked).
    uint8_t* realCall = nullptr;
    uint8_t* realFn = nullptr;
    LumenCardsOffsets ro{};
    const char* rwhy = "";
    if (!FindMeshCards(game, &realCall, &realFn, &ro, &rwhy)) return 4096;
    const uint32_t kFrom = static_cast<uint32_t>(realFn - game.base) & ~0xFFFu;
    constexpr uint32_t kSize = 0x3000;
    if (realCall < game.base + kFrom || realCall >= game.base + kFrom + kSize) return 8192;
    uint8_t* const block = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!block) return 128;
    int fail = 0;
    const auto Fresh = [&]() {
        memcpy(block, game.base + kFrom, kSize);
        GameModule g{};
        g.base = block - kFrom;
        g.text = Section{block, block + kSize};
        g.timeDateStamp = game.timeDateStamp;
        return g;
    };
    Sites f{};
    const char* why = "";
    GameModule g = Fresh();
    if (Discover(g, &f, &why)) fail |= 1;  // without privateCopy: Init is outside this copy: must refuse
    if (!Discover(g, &f, &why, true)) fail |= 1;
    const uint8_t* const fn = f.fn;
    const size_t places[] = {kEveryAt, kExtAt, kNumAt, kRegionAt + kRegionSite, kAddAt, static_cast<size_t>(f.exitTo - fn) + 3 + 1};
    int bit = 2;
    for (const size_t at : places) {
        g = Fresh();
        block[(fn - block) + at] ^= 0x5A;
        Sites t{};
        if (Discover(g, &t, &why, true)) fail |= bit;
        bit = bit < 64 ? bit << 1 : bit;
    }
    // The patch: the site jumps to the stub, the upload call's target moves, nothing else in the function changes.
    g = Fresh();
    if (Discover(g, &f, &why, true)) {
        uint8_t* const stub = block + kSize + 0x100;
        uint8_t* const up = block + kSize + 0x200;
        if (!PatchJmp(f.site, kSiteLen, stub) || !PatchCall(f.uploadCall, up)) fail |= 256;
        int32_t rel = 0;
        memcpy(&rel, f.site + 1, 4);
        if (f.site[0] != 0xE9 || f.site + 5 + rel != stub || f.site[5] != 0x90 || f.site[6] != 0x90) fail |= 512;
        memcpy(&rel, f.uploadCall + 1, 4);
        if (f.uploadCall[0] != 0xE8 || f.uploadCall + 5 + rel != up) fail |= 1024;
        int changed = 0;
        for (uint32_t i = 0; i < kSize; ++i) changed += block[i] != (game.base + kFrom)[i];
        if (changed > 11) fail |= 2048;
    } else {
        fail |= 256;
    }
    // block stays allocated: the self-test's pattern-search report repeats every recorded search at the end, this copy's too.
    return fail;
}

namespace {
struct StubSeen { const void* a; const void* b; const void* c; uint32_t d; int calls; bool ret; bool misaligned; };
StubSeen g_stubSeen;
bool StubTestTarget(const void* a, const void* b, void* c, uint32_t d)
{
    if (reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) % 16 != 8) g_stubSeen.misaligned = true;
    g_stubSeen.a = a;
    g_stubSeen.b = b;
    g_stubSeen.c = c;
    g_stubSeen.d = d;
    ++g_stubSeen.calls;
    return g_stubSeen.ret;
}
void ResetTestTarget(const void* a)
{
    if (reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) % 16 != 8) g_stubSeen.misaligned = true;
    g_stubSeen.a = a;
    ++g_stubSeen.calls;
}

// Machine code that builds the register and stack state of each patched place, jumps to the real stub, and lands where the game's code would go on.
using LoopThunk = int64_t (*)(const void* rdi, const void* rbx, void* rsi, uint32_t numIndices);  // 1 = the exit; else r13 = the stock loop, after the displaced instruction
using ResetThunk = int64_t (*)(const void* rbx);                                                   // 1 = ZF set by the displaced cmp
struct Exec { uint8_t* mem; LoopThunk loop; ResetThunk reset; };
Exec BuildExec(const void* decide, const void* resetFn)
{
    Exec x{};
    uint8_t* const mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem) return x;
    uint8_t* m = mem;
    const auto Put = [&m](std::initializer_list<uint8_t> b) { for (uint8_t v : b) *m++ = v; };
    uint8_t* const loop = m;
    Put({0x53, 0x56, 0x57, 0x41, 0x55});            // push rbx, rsi, rdi, r13
    Put({0x48, 0x83, 0xEC, 0x78});                  // sub rsp,78h (8 at entry + 4 pushes + 78h = 16-aligned, as at the site)
    Put({0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xDA, 0x49, 0x8B, 0xF0});  // mov rdi,rcx; mov rbx,rdx; mov rsi,r8
    Put({0x44, 0x89, 0x4C, 0x24, 0x60});            // mov [rsp+60h],r9d
    uint8_t* const jmpLoop = m;
    m += 14;
    uint8_t* const landStock = m;
    Put({0x4C, 0x89, 0xE8});                        // mov rax,r13
    uint8_t* const exitJ = m;
    Put({0xEB, 0x00});                              // jmp out
    uint8_t* const landHandled = m;
    Put({0xB8, 0x01, 0x00, 0x00, 0x00});            // mov eax,1
    exitJ[1] = static_cast<uint8_t>(m - (exitJ + 2));
    Put({0x48, 0x83, 0xC4, 0x78, 0x41, 0x5D, 0x5F, 0x5E, 0x5B, 0xC3});  // add rsp,78h; pop r13, rdi, rsi, rbx; ret
    uint8_t* const reset = m;
    Put({0x53, 0x48, 0x83, 0xEC, 0x20});            // push rbx; sub rsp,20h (16-aligned)
    Put({0x48, 0x8B, 0xD9});                        // mov rbx,rcx
    uint8_t* const jmpReset = m;
    m += 14;
    uint8_t* const landReset = m;
    Put({0x0F, 0x94, 0xC0, 0x0F, 0xB6, 0xC0});      // sete al; movzx eax,al
    Put({0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3});      // add rsp,20h; pop rbx; ret
    uint8_t* const stubLoop = mem + 512;
    uint8_t* const stubReset = mem + 1024;
    static const uint8_t kDisplacedLoop[kSiteLen] = {0x4C, 0x8B, 0xAF, 0x70, 0x06, 0x00, 0x00};  // mov r13,[rdi+670h]
    static const uint8_t kDisplacedReset[kResetLen] = {0x81, 0xBB, 0x7C, 0x06, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00};  // cmp dword ptr [rbx+67Ch],400h
    BuildStub(stubLoop, decide, kDisplacedLoop, landStock, landHandled);
    BuildResetStub(stubReset, resetFn, kDisplacedReset, landReset);
    WriteJmpAbs(jmpLoop, stubLoop);
    WriteJmpAbs(jmpReset, stubReset);
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    x.mem = mem;
    x.loop = reinterpret_cast<LoopThunk>(loop);
    x.reset = reinterpret_cast<ResetThunk>(reset);
    return x;
}
}  // namespace

// The two stubs, executed.
int LumenNarrowTestStub()
{
    int fail = 0;
    const Exec x = BuildExec(reinterpret_cast<const void*>(&StubTestTarget), reinterpret_cast<const void*>(&ResetTestTarget));
    if (!x.mem) return 128;
    static uint8_t fakeLsd[0x700];
    const int64_t want = 0x1122334455667788;
    memcpy(fakeLsd + 0x670, &want, 8);
    uint8_t fakeScene[8], fakeUb[8];
    g_stubSeen = StubSeen{};
    g_stubSeen.ret = true;
    if (x.loop(fakeLsd, fakeScene, fakeUb, 4096) != 1) fail |= 2;
    if (g_stubSeen.calls != 1 || g_stubSeen.a != fakeLsd || g_stubSeen.b != fakeScene || g_stubSeen.c != fakeUb || g_stubSeen.d != 4096) fail |= 4;
    g_stubSeen = StubSeen{};
    g_stubSeen.ret = false;
    if (x.loop(fakeLsd, fakeScene, fakeUb, 8192) != want) fail |= 8;
    if (g_stubSeen.calls != 1 || g_stubSeen.d != 8192 || g_stubSeen.misaligned) fail |= 16;
    // The reset stub: the target gets rbx, and the flags it leaves are those of the displaced compare, for the game's je after it.
    g_stubSeen = StubSeen{};
    const int32_t maxEq = 0x400, maxNe = 0x500;
    memcpy(fakeLsd + 0x67C, &maxEq, 4);
    if (x.reset(fakeLsd) != 1 || g_stubSeen.calls != 1 || g_stubSeen.a != fakeLsd || g_stubSeen.misaligned) fail |= 32;
    memcpy(fakeLsd + 0x67C, &maxNe, 4);
    if (x.reset(fakeLsd) != 0) fail |= 64;
    VirtualFree(x.mem, 0, MEM_RELEASE);
    return fail;
}

// The unwind data, on the real functions' records, for both stubs: a walker at the return address inside a stub must arrive at the game function's caller.
int LumenNarrowTestUnwind(const GameModule& game)
{
    int fail = 0;
    uint8_t* const mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem) return 128;
    static const uint8_t kDisp[kSiteLen] = {0x4C, 0x8B, 0xAF, 0x70, 0x06, 0x00, 0x00};
    static const uint8_t kDispReset[kResetLen] = {0x81, 0xBB, 0x7C, 0x06, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00};
    const size_t lenLoop = BuildStub(mem, reinterpret_cast<const void*>(&StubTestTarget), kDisp, mem + 4096, mem + 4096);
    const size_t lenReset = BuildResetStub(mem + 512, reinterpret_cast<const void*>(&ResetTestTarget), kDispReset, mem + 4096);
    Sites f{};
    const char* why = "";
    if (!Discover(game, &f, &why) || !DiscoverReset(game, f.o.list, f.o.groupList, &f.resetSite, &why)) { VirtualFree(mem, 0, MEM_RELEASE); return 1; }
    const uint8_t* const infoLoop = FindUnwindInfo(game, f.site);
    const uint8_t* const infoReset = FindUnwindInfo(game, f.resetSite);
    if (!infoLoop || !infoReset) { VirtualFree(mem, 0, MEM_RELEASE); return 1; }
    if (!BuildUnwind(mem + 1024, infoLoop) || !BuildUnwind(mem + 1152, infoReset)) fail |= 2;
    auto* const table = reinterpret_cast<RUNTIME_FUNCTION*>(mem + 2048);
    table[0].BeginAddress = 0;
    table[0].EndAddress = static_cast<DWORD>(lenLoop);
    table[0].UnwindData = 1024;
    table[1].BeginAddress = 512;
    table[1].EndAddress = static_cast<DWORD>(512 + lenReset);
    table[1].UnwindData = 1152;
    if (!RtlAddFunctionTable(table, 2, reinterpret_cast<DWORD64>(mem))) fail |= 4;
    // The stack at the call inside a stub: its 20h, the function's own allocation (read by hand from .pdata: UpdateLumenMeshCards 270h, UpdateLumenScene 8F0h), 7 pushes, the return address.
    static uint64_t stack[1024];
    const uint64_t marker = 0x0000123412341234;
    const auto Walk = [&](uint8_t* pc, size_t frame, int bit) {
        memset(stack, 0, sizeof stack);
        const size_t atRet = (0x20 + frame + 7 * 8) / 8;
        stack[atRet] = marker;
        DWORD64 base = 0;
        RUNTIME_FUNCTION* const rf = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(pc), &base, nullptr);
        if (!rf || base != reinterpret_cast<DWORD64>(mem)) { fail |= bit; return; }
        CONTEXT ctx{};
        ctx.Rip = reinterpret_cast<DWORD64>(pc);
        ctx.Rsp = reinterpret_cast<DWORD64>(stack);
        void* handler = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &handler, &establisher, nullptr);
        if (ctx.Rip != marker || ctx.Rsp != reinterpret_cast<DWORD64>(&stack[atRet + 1])) fail |= bit;
    };
    Walk(mem + kStubCallReturn, 0x270, 8);
    Walk(mem + 512 + kResetCallReturn, 0x8F0, 16);
    RtlDeleteFunctionTable(table);
    VirtualFree(mem, 0, MEM_RELEASE);
    return fail;
}

// The reset site on the mapped exe, and refusal on a private copy.
int LumenNarrowTestResetSite(const GameModule& game, uint32_t* foundRva)
{
    int fail = 0;
    // The list offsets as the loop's code gives them (0x670 / 0x180 on build 19115871, checked by the caller's expected values).
    Sites f{};
    const char* why = "";
    if (!Discover(game, &f, &why)) return 256;
    const uint32_t list = f.o.list, groupList = f.o.groupList;
    uint8_t* site = nullptr;
    if (!DiscoverReset(game, list, groupList, &site, nullptr)) fail |= 1;
    if (!site) return fail;
    const uint32_t siteRva = static_cast<uint32_t>(site - game.base);
    const uint32_t kFrom = siteRva & ~0xFFFu;
    constexpr uint32_t kSize = 0x1000;
    if (foundRva) *foundRva = siteRva;
    if (siteRva < kFrom + kResetSite || siteRva + 0x40 > kFrom + kSize) return fail | 512;  // the pattern must sit inside the copied page
    uint8_t* const block = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!block) return fail | 128;
    GameModule g{};
    g.base = block - kFrom;
    g.text = Section{block, block + kSize};
    memcpy(block, game.base + kFrom, kSize);
    uint8_t* s2 = nullptr;
    if (!DiscoverReset(g, list, groupList, &s2, nullptr) || static_cast<uint32_t>(s2 - g.base) != siteRva) fail |= 2;
    if (DiscoverReset(g, list + 8, groupList, &s2, nullptr)) fail |= 4;  // not the list's offsets
    block[siteRva - kFrom + 2] ^= 0x5A;                                  // the compare's offset
    if (DiscoverReset(g, list, groupList, &s2, nullptr)) fail |= 8;
    memcpy(block, game.base + kFrom, kSize);
    uint8_t* s3 = nullptr;
    DiscoverReset(g, list, groupList, &s3, nullptr);
    if (s3) {
        uint8_t* const stub = block + kSize + 0x100;
        if (!PatchJmp(s3, kResetLen, stub)) fail |= 16;
        int32_t rel = 0;
        memcpy(&rel, s3 + 1, 4);
        if (s3[0] != 0xE9 || s3 + 5 + rel != stub) fail |= 32;
        for (size_t i = 5; i < kResetLen; ++i) if (s3[i] != 0x90) fail |= 64;
    }
    // block stays allocated: the self-test's pattern-search report repeats every recorded search at the end.
    return fail;
}

namespace {
// The fakes: game structures at offsets as read from the binary. The test's stock model is its own: model[] holds each primitive's values, kept apart from the group
// array, so it can catch a wrong StockValue. Two buffers: stockGpu gets what stock would write, fixGpu what the upload actually carried.
constexpr int kFP = 8;      // primitives
constexpr int kFG = 2000;   // groups
constexpr int kFBuf = 1 << 14;
struct Fake {
    uint8_t lsd[0x1000]{};
    uint8_t scene[0x3000]{};
    uint8_t prim[kFP][0x400]{};
    void* prims[kFP]{};
    int32_t list[kFP + 2]{};
    int32_t changed[kFG]{};
    uint8_t groups[kFG * kGroupStride]{};
    uint32_t bits[kFG / 32 + 1]{};
    std::vector<int32_t> indices[kFP];
    std::vector<int32_t> model[kFP];
    std::vector<uint32_t> scatter;
    std::vector<int32_t> data;
    std::vector<int32_t> stockGpu, fixGpu;
    uint8_t ub[0x40]{};
    void* extBuf = reinterpret_cast<void*>(0x1000);
    int nextGroup = 1;
    const Exec* x = nullptr;

    void Init(const Exec* e)
    {
        x = e;
        void* p = prims;
        for (int i = 0; i < kFP; ++i) prims[i] = prim[i];
        memcpy(scene + g_n.sceneData, &p, 8);
        const int32_t num = kFP;
        memcpy(scene + g_n.sceneData + 8, &num, 4);
        p = list;
        memcpy(lsd + g_n.list, &p, 8);
        p = changed;
        memcpy(lsd + g_n.groupList, &p, 8);
        p = groups;
        memcpy(lsd + g_n.groupsData, &p, 8);
        const int32_t gn = kFG;
        memcpy(lsd + g_n.groupsData + 8, &gn, 4);
        p = bits;
        memcpy(lsd + g_n.bits + 0x10, &p, 8);
        memcpy(lsd + g_n.bitsNum, &gn, 4);
        memcpy(lsd + g_n.extBuffer, &extBuf, 8);
        const int32_t maxQ = 0x400;
        memcpy(lsd + g_n.list + 0xC, &maxQ, 4);
        scatter.assign(kFBuf, 0);
        data.assign(kFBuf, 0);
        stockGpu.assign(kFBuf, -7);
        fixGpu.assign(kFBuf, -7);
    }
    void SetLists(std::initializer_list<int> q, std::initializer_list<int> c)
    {
        int n = 0;
        for (int v : q) list[n++] = v;
        memcpy(lsd + g_n.list + 8, &n, 4);
        n = 0;
        for (int v : c) changed[n++] = v;
        memcpy(lsd + g_n.groupList + 8, &n, 4);
    }
    int32_t& GroupField(int g, uint32_t off) { return *reinterpret_cast<int32_t*>(groups + g * kGroupStride + off); }
    void SetGroup(int g, const void* primitive, int inst, int cards, int prims_ = 1)
    {
        memcpy(groups + g * kGroupStride + kGPrimInline, &primitive, 8);
        void* none = nullptr;
        memcpy(groups + g * kGroupStride + kGPrimSecondary, &none, 8);
        GroupField(g, kGPrimNum) = prims_;
        GroupField(g, kGInstance) = inst;
        GroupField(g, kGMeshCards) = cards;
        bits[g >> 5] |= 1u << (g & 31);
    }
    void Prim(int i, int n, int off, int groupsCount)
    {
        *reinterpret_cast<int32_t*>(prim[i] + g_n.primNumInst) = n;
        *reinterpret_cast<int32_t*>(prim[i] + g_n.primOffset) = off;
        *reinterpret_cast<int32_t*>(prim[i] + g_n.primGroupsNum) = groupsCount;
    }
    // One group per instance, values = base + k (or all `base` when flat). Re-creating keeps the pointer; the groups are new.
    void PerInstance(int i, int n, int off, int base, bool flat = false)
    {
        Prim(i, n, off, n);
        indices[i].resize(static_cast<size_t>(n));
        model[i].resize(static_cast<size_t>(n));
        void* p = indices[i].data();
        memcpy(prim[i] + g_n.primIndices + 8, &p, 8);
        for (int k = 0; k < n; ++k) {
            const int g = nextGroup++;
            const int v = flat ? base : base + k;
            indices[i][static_cast<size_t>(k)] = g;
            model[i][static_cast<size_t>(k)] = v;
            SetGroup(g, prim[i], k, v);
        }
    }
    int G(int i, int k) const { return indices[i][static_cast<size_t>(k)]; }
    void Cards(int i, int k, int v)  // what AddMeshCards or RemoveMeshCards does to one instance's value
    {
        GroupField(G(i, k), kGMeshCards) = v;
        model[i][static_cast<size_t>(k)] = v;
    }
    int32_t Model(int i, int k) const
    {
        if (model[i].empty()) return -1;
        const int top = static_cast<int>(model[i].size()) - 1;
        return model[i][static_cast<size_t>(k < top ? k : top)];
    }
    void BeginUpload(uint32_t maxN)
    {
        void* p = scatter.data();
        memcpy(ub + kUbScatter, &p, 8);
        p = data.data();
        memcpy(ub + kUbUpload, &p, 8);
        const uint32_t zero = 0, stride = 4;
        memcpy(ub + kUbNum, &zero, 4);
        memcpy(ub + kUbMax, &maxN, 4);
        memcpy(ub + kUbStride, &stride, 4);
    }
    uint32_t Sent() const { return Rd<uint32_t>(ub, kUbNum); }
    int QueuedInstances() const
    {
        const int32_t ln = Rd<int32_t>(lsd, g_n.list + 8);
        int sum = 0;
        for (int i = 0; i < ln; ++i)
            if (list[i] >= 0 && list[i] < kFP) sum += Rd<int32_t>(prim[list[i]], g_n.primNumInst);
        return sum;
    }
    // One UpdateLumenMeshCards + the lists' reset, as the game runs them: stock's je when no instance is queued (no Decide, no upload), the real stubs, the stock model when
    // Decide declines, the proof before the upload. reset = false skips the reset hook only, to show what it prevents. Returns whether the fix narrowed.
    bool Run(uint32_t numIndices, bool reset = true)
    {
        bool handled = false;
        const int32_t ln = Rd<int32_t>(lsd, g_n.list + 8);
        BeginUpload(static_cast<uint32_t>(scatter.size()));
        if (QueuedInstances() > 0) {
            handled = x->loop(lsd, scene, ub, numIndices) == 1;
            if (!handled) {  // the stock loop, from the model
                uint32_t ns = 0;
                for (int i = 0; i < ln; ++i) {
                    if (list[i] < 0 || list[i] >= kFP) continue;
                    const int32_t n = Rd<int32_t>(prim[list[i]], g_n.primNumInst), off = Rd<int32_t>(prim[list[i]], g_n.primOffset);
                    for (int k = 0; k < n; ++k)
                        if (off + k < static_cast<int32_t>(numIndices)) {
                            scatter[ns] = static_cast<uint32_t>(off + k);
                            data[ns] = Model(list[i], k);
                            ++ns;
                        }
                }
                memcpy(ub + kUbNum, &ns, 4);
            }
            const uint32_t sent = Sent();
            for (uint32_t i = 0; i < sent; ++i) fixGpu[scatter[i]] = data[i];
            for (int i = 0; i < ln; ++i) {  // what stock would write, by the model only
                if (list[i] < 0 || list[i] >= kFP) continue;
                const int32_t n = Rd<int32_t>(prim[list[i]], g_n.primNumInst), off = Rd<int32_t>(prim[list[i]], g_n.primOffset);
                for (int k = 0; k < n; ++k)
                    if (off + k < static_cast<int32_t>(numIndices)) stockGpu[static_cast<size_t>(off + k)] = Model(list[i], k);
            }
        }
        if (reset && x->reset(lsd) != 1) resetFlagBad = true;
        SetLists({}, {});
        return handled;
    }
    bool Go(std::initializer_list<int> q, std::initializer_list<int> c, uint32_t numIndices, bool reset = true)
    {
        SetLists(q, c);
        return Run(numIndices, reset);
    }
    // A water-info renderer: UpdateLumenMeshCards not called, the lists reset all the same.
    void WaterFrame(std::initializer_list<int> q)
    {
        SetLists(q, {});
        if (x->reset(lsd) != 1) resetFlagBad = true;
        SetLists({}, {});
    }
    int Diff() const
    {
        int d = 0;
        for (size_t i = 0; i < stockGpu.size(); ++i) d += stockGpu[i] != fixGpu[i];
        return d;
    }
    bool resetFlagBad = false;
};

const Offsets kFakeOffsets = {0x670, 0x180, 0x1B0, 0x1C0, 0x1D8, 0x20E0, 0x23C, 0x238, 0x118, 0x128, 0x6A0, 0x6A8, nullptr};

DWORD WINAPI OtherThreadDecide(void* f)
{
    Fake& h = *static_cast<Fake*>(f);
    h.BeginUpload(4096);
    return Decide(h.lsd, h.scene, h.ub, 8192) ? 1 : 0;
}
}  // namespace

// The decision and the proof with fakes, through the real stubs. After every step fixGpu must equal stockGpu on every destination.
int LumenNarrowTestLogic()
{
    const Offsets savedN = g_n;
    g_n = kFakeOffsets;
    const auto A = [](size_t n) { return VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); };
    g_slots = static_cast<Slot*>(A(kSlots * sizeof(Slot)));
    g_own = static_cast<uint32_t*>(A(kMaxDest * sizeof(uint32_t)));
    g_queueFull = static_cast<uint8_t*>(A(kMaxQueue));
    g_pairs = static_cast<Pair*>(A(kMaxGroups * sizeof(Pair)));
    if (!g_slots || !g_own || !g_queueFull || !g_pairs) return -1;
    const Exec x = BuildExec(reinterpret_cast<const void*>(&Decide), reinterpret_cast<const void*>(&ResetHook));
    if (!x.mem) return -1;
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    g_qpcToMs = 1000.0 / static_cast<double>(fq.QuadPart);
    g_ready = true;
    g_failed = false;
    g_mode = 1;
    g_lastMode = 1;
    g_lastLsd = nullptr;
    g_lastBuf = nullptr;
    g_lastNum = 0;
    g_used = 0;
    g_thread = 0;
    g_ran = false;

    auto fp = std::make_unique<Fake>();
    Fake& f = *fp;
    f.Init(&x);
    constexpr uint32_t kIdx = 4096;
    uint32_t fail = 0;
    const auto Check1 = [&](bool ok, int bit) { if (!ok) fail |= 1u << bit; };
    const auto Same = [&](int bit) { if (f.Diff()) fail |= 1u << bit; };
    const auto Reason = [&](Why w) { return g_sum.why[w].load(); };

    // A primitive of 100 instances at 100, one of 50 at 300, merged (10 instances, 1 group), no groups (5).
    f.PerInstance(0, 100, 100, 1000);
    f.PerInstance(1, 50, 300, 2000);
    f.Prim(2, 10, 400, 1);
    {
        const int g = f.nextGroup++;
        void* in = nullptr;
        memcpy(f.prim[2] + g_n.primIndices + 8, &in, 8);
        *reinterpret_cast<int32_t*>(f.prim[2] + g_n.primIndices) = g;
        f.SetGroup(g, f.prim[2], -1, 3000);
        f.model[2] = {3000};
    }
    f.Prim(3, 5, 450, 0);
    // StockValue against the model: this is where a wrong evaluation shows.
    for (int p = 0; p < 4; ++p)
        for (int k = 0; k < Rd<int32_t>(f.prim[p], g_n.primNumInst); ++k)
            Check1(StockValue(f.prim[p], k, f.groups) == f.Model(p, k), 0);
    // First sight (and a new buffer): stock.
    Check1(!f.Go({0, 1, 2, 3}, {}, kIdx) && f.Sent() == 165, 0);
    Same(0);
    // One instance's cards changed: only that one goes; merged and no-groups primitives in the same call go whole.
    f.Cards(0, 7, 7777);
    Check1(f.Go({0, 2, 3}, {f.G(0, 7)}, kIdx) && f.Sent() == 1 + 10 + 5 && f.fixGpu[107] == 7777, 1);
    Same(1);
    // Mesh cards removed (-1): same path.
    f.Cards(0, 8, -1);
    Check1(f.Go({0}, {f.G(0, 8)}, kIdx) && f.Sent() == 1 && f.fixGpu[108] == -1, 2);
    Same(2);
    // Queued with nothing changed: one redundant correct entry.
    Check1(f.Go({0}, {}, kIdx) && f.Sent() == 1 && f.scatter[0] == 100, 3);
    Same(3);
    // The offset moved: whole at the new place.
    f.Prim(0, 100, 600, 100);
    Check1(!f.Go({0}, {}, kIdx) && f.Sent() == 100, 4);
    Same(4);
    // Another primitive takes that range, then the first comes back to the same offset: its entries are not its own any more: whole.
    f.Prim(0, 0, -1, 100);
    f.PerInstance(4, 40, 600, 4000);
    Check1(!f.Go({4}, {}, kIdx), 5);
    f.Prim(4, 0, -1, 40);
    f.Prim(0, 100, 600, 100);
    Check1(!f.Go({0}, {}, kIdx) && f.Sent() == 100, 5);
    Same(5);
    // The instance count changed (same offset): whole.
    {
        const int g = f.nextGroup++;
        f.indices[0].push_back(g);
        f.model[0].push_back(1500);
        void* p = f.indices[0].data();
        memcpy(f.prim[0] + g_n.primIndices + 8, &p, 8);
        f.SetGroup(g, f.prim[0], 100, 1500);
        f.Prim(0, 101, 600, 101);
        Check1(!f.Go({0}, {}, kIdx) && f.Sent() == 101, 6);
        Same(6);
    }
    // A group changed while its primitive is not queued, in a call that runs (others queued): the next send is whole. Stock heals it then.
    f.GroupField(f.G(1, 3), kGMeshCards) = 2222;
    f.model[1][3] = 2222;
    Check1(f.Go({0}, {f.G(1, 3)}, kIdx), 7);
    Check1(!f.Go({1}, {}, kIdx) && f.Sent() == 50 && f.fixGpu[303] == 2222, 7);
    Same(7);
    // M1, shown without the hook: a water frame re-creates the primitive at the same place; the next active renderer lists its new groups with 0 queued instances, so the loop
    // isn't reached. Without the reset hook the record survives, the next card change narrows, and the buffer differs from stock. This also proves the comparison can fail.
    const auto Recreate = [&](int i, int n, int off) {
        f.PerInstance(i, n, off, -1, true);
        std::vector<int> g;
        for (int k = 0; k < n; ++k) g.push_back(f.G(i, k));
        return g;
    };
    const auto ListOnly = [&](const std::vector<int>& g, bool reset) {
        f.SetLists({}, {});
        int n = 0;
        for (int v : g) f.changed[n++] = v;
        memcpy(f.lsd + g_n.groupList + 8, &n, 4);
        return f.Run(kIdx, reset);
    };
    Check1(f.Go({1}, {}, kIdx), 26);  // narrow state
    {
        f.WaterFrame({1});  // the queue entry from LumenAddPrimitive is lost
        const std::vector<int> g = Recreate(1, 50, 300);
        ListOnly(g, false);
        f.Cards(1, 5, 555);
        Check1(f.Go({1}, {f.G(1, 5)}, kIdx) && f.Sent() == 1, 26);
        Check1(f.Diff() != 0, 26);  // wrong on purpose: 49 entries where stock sent -1
        ClearAll();
        Check1(!f.Go({1}, {}, kIdx), 26);
        Same(26);
    }
    // M1 with the hook: the same chain through the same code: the record is dropped at the reset, the next send is whole, the buffers stay equal.
    {
        Check1(f.Go({1}, {}, kIdx), 27);
        f.WaterFrame({1});
        const std::vector<int> g = Recreate(1, 50, 300);
        const uint64_t zero0 = g_sum.noUploadListed.load(), drop0 = g_sum.dropped.load(), lost0 = g_sum.lostQueue.load();
        ListOnly(g, true);
        Check1(g_sum.noUploadListed.load() == zero0 + 1 && g_sum.dropped.load() == drop0 + 1 && lost0 >= 1, 27);
        f.Cards(1, 5, 555);
        Check1(!f.Go({1}, {f.G(1, 5)}, kIdx) && f.Sent() == 50, 27);
        Same(27);
    }
    // Lost queue entries (water frames): stock writes nothing, the fix writes nothing; moved and lost, then queued: whole at the new place; moved away and back, both lost: as stock, same contents.
    {
        const uint64_t lost0 = g_sum.lostQueue.load();
        f.Prim(1, 50, 900, 50);
        f.WaterFrame({1});
        Check1(g_sum.lostQueue.load() == lost0 + 1, 28);
        Same(28);
        Check1(!f.Go({1}, {}, kIdx) && f.Sent() == 50, 28);
        Same(28);
        f.Prim(1, 50, 950, 50);
        f.WaterFrame({1});
        f.Prim(1, 50, 900, 50);
        f.WaterFrame({1});
        Check1(f.Go({1}, {}, kIdx) && f.Sent() == 1, 28);
        Same(28);
        f.Prim(1, 50, 300, 50);  // back for the later cases
        Check1(!f.Go({1}, {}, kIdx), 28);
    }
    // A group listed after the upload in the same call: not sent, so its record drops at the reset.
    {
        Check1(f.Go({1}, {}, kIdx), 29);
        f.SetLists({1}, {});
        f.BeginUpload(static_cast<uint32_t>(f.scatter.size()));
        Check1(x.loop(f.lsd, f.scene, f.ub, kIdx) == 1, 29);
        f.fixGpu[300] = f.Model(1, 0);
        f.GroupField(f.G(1, 6), kGMeshCards) = 6666;
        f.model[1][6] = 6666;
        const uint64_t drop0 = g_sum.dropped.load();
        f.changed[0] = f.G(1, 6);
        const int32_t one = 1;
        memcpy(f.lsd + g_n.groupList + 8, &one, 4);
        x.reset(f.lsd);
        Check1(g_sum.dropped.load() == drop0 + 1, 29);
        f.SetLists({}, {});
        Check1(!f.Go({1}, {}, kIdx) && f.Sent() == 50 && f.fixGpu[306] == 6666, 29);
        Same(29);
    }
    // The buffer replaced: stock, and everything forgotten.
    f.extBuf = reinterpret_cast<void*>(0x2000);
    memcpy(f.lsd + g_n.extBuffer, &f.extBuf, 8);
    Check1(!f.Go({0}, {}, kIdx) && f.Sent() == 101, 8);
    Check1(!f.Go({1}, {}, kIdx) && f.Sent() == 50, 8);  // not sent since: whole
    Same(8);
    // NumIndices changed: the same.
    Check1(!f.Go({0}, {}, kIdx * 2), 9);
    Check1(!f.Go({1}, {}, kIdx * 2) && f.Sent() == 50, 9);
    Same(9);
    // Another LumenSceneData: the same.
    {
        auto g2 = std::make_unique<Fake>();
        Fake& h = *g2;
        h.Init(&x);
        h.PerInstance(0, 100, 100, 1000);
        Check1(!h.Go({0}, {}, kIdx * 2) && h.Sent() == 100 && h.Diff() == 0, 10);
    }
    // Back on the first: forgotten, then narrow works again.
    Check1(!f.Go({0, 1}, {}, kIdx * 2), 11);
    Check1(f.Go({0, 1}, {}, kIdx * 2) && f.Sent() == 1, 11);
    Same(11);
    // r.LumenScene.UploadEveryFrame: stock, forgotten.
    {
        int32_t every = 1;
        g_n.uploadEvery = &every;
        Check1(!f.Go({0}, {}, kIdx * 2) && f.Sent() == 101, 12);
        every = 0;
        Check1(!f.Go({1}, {}, kIdx * 2) && f.Sent() == 50, 12);
        g_n.uploadEvery = nullptr;
        Same(12);
    }
    f.Go({0}, {}, kIdx * 2);
    // Too many changed groups: stock for the call, forgotten.
    {
        std::vector<int32_t> big(static_cast<size_t>(kMaxGroups) + 1, 0);
        void* p = big.data();
        memcpy(f.lsd + g_n.groupList, &p, 8);
        const int32_t n = kMaxGroups + 1;
        f.SetLists({0}, {});
        memcpy(f.lsd + g_n.groupList + 8, &n, 4);
        const uint64_t before = Reason(kWGroups);
        Check1(!f.Run(kIdx * 2) && f.Sent() == 101 && Reason(kWGroups) == before + 1, 13);
        p = f.changed;
        memcpy(f.lsd + g_n.groupList, &p, 8);
        Check1(!f.Go({0}, {}, kIdx * 2) && f.Sent() == 101, 13);  // forgotten: whole again
        Same(13);
    }
    // A group of a narrowed primitive that says another instance than the primitive's table, or names 2 primitives: stock for the call, forgotten.
    {
        Check1(f.Go({0}, {}, kIdx * 2), 14);
        const int g = f.G(0, 9);
        f.GroupField(g, kGInstance) = 10;
        const uint64_t before = Reason(kWAnomaly);
        Check1(!f.Go({0}, {g}, kIdx * 2) && f.Sent() == 101 && Reason(kWAnomaly) == before + 1, 14);
        f.GroupField(g, kGInstance) = 9;
        f.Go({0}, {}, kIdx * 2);
        Check1(f.Go({0}, {}, kIdx * 2), 14);
        f.GroupField(g, kGPrimNum) = 2;
        void* sec[2] = {f.prim[0], f.prim[1]};
        void* secp = sec;
        memcpy(f.groups + g * kGroupStride + kGPrimSecondary, &secp, 8);
        Check1(!f.Go({0}, {g}, kIdx * 2), 14);
        f.GroupField(g, kGPrimNum) = 1;
        secp = nullptr;
        memcpy(f.groups + g * kGroupStride + kGPrimSecondary, &secp, 8);
        Same(14);
    }
    // A removed group in the list (bit cleared): skipped, the primitive stays narrow.
    {
        f.Go({0}, {}, kIdx * 2);
        const int g = f.G(0, 11);
        const uint32_t saved = f.bits[g >> 5];
        f.bits[g >> 5] &= ~(1u << (g & 31));
        Check1(f.Go({0}, {g}, kIdx * 2) && f.Sent() == 1, 15);
        f.bits[g >> 5] = saved;
        Same(15);
    }
    // A negative offset and a range past NumIndices: stock, forgotten.
    {
        f.Prim(5, 40, -1, 40);
        f.SetLists({5}, {});
        f.BeginUpload(4096);
        const uint64_t before = Reason(kWBad);
        Check1(!Decide(f.lsd, f.scene, f.ub, kIdx * 2) && f.Sent() == 0 && Reason(kWBad) == before + 1, 16);
        f.Prim(5, 40, kIdx * 2 - 10, 40);
        Check1(!Decide(f.lsd, f.scene, f.ub, kIdx * 2) && Reason(kWBad) == before + 2, 16);
        f.Prim(5, 0, -1, 0);
        x.reset(f.lsd);
        f.SetLists({}, {});
        f.Go({0}, {}, kIdx * 2);
        Same(16);
    }
    // The upload buffer not as expected: stock, nothing written.
    {
        const uint32_t badStride = 16;
        f.SetLists({0}, {});
        f.BeginUpload(4096);
        memcpy(f.ub + kUbStride, &badStride, 4);
        Check1(!Decide(f.lsd, f.scene, f.ub, kIdx * 2) && f.Sent() == 0, 17);
        x.reset(f.lsd);
        f.SetLists({}, {});
    }
    // Capacity respected: a narrow call that wouldn't fit declines before writing.
    {
        f.Go({0}, {}, kIdx * 2);
        f.SetLists({0}, {f.G(0, 1), f.G(0, 2)});
        f.BeginUpload(1);
        Check1(!Decide(f.lsd, f.scene, f.ub, kIdx * 2) && f.Sent() == 0, 18);
        x.reset(f.lsd);
        f.SetLists({}, {});
    }
    // Value 0 and a value change: stock, forgotten. Values past 2 read as 0. Another thread: stock, forgotten.
    {
        Check1(KeyValue(3) == 0 && KeyValue(-1) == 0 && KeyValue(1) == 1 && KeyValue(2) == 1 && KeyValue(0) == 0, 19);
        f.Go({0}, {}, kIdx * 2);
        g_mode = 0;
        Check1(!f.Go({0}, {}, kIdx * 2) && f.Sent() == 101, 19);
        g_mode = 1;
        Check1(!f.Go({0}, {}, kIdx * 2) && f.Sent() == 101, 19);
        Check1(f.Go({0}, {}, kIdx * 2) && f.Sent() == 1, 19);
        Same(19);
        f.SetLists({0}, {});
        HANDLE t = CreateThread(nullptr, 0, OtherThreadDecide, &f, 0, nullptr);
        DWORD code = 9;
        if (t) { WaitForSingleObject(t, 5000); GetExitCodeThread(t, &code); CloseHandle(t); }
        Check1(code == 0, 19);
        f.SetLists({}, {});
        g_thread = GetCurrentThreadId();
        Check1(!f.Go({0}, {}, kIdx * 2) && f.Sent() == 101, 19);  // forgotten by the other thread's call
        Same(19);
    }
    // Key off, then on, with no queued instance in the off window: the forget must not depend on a call reaching Decide. A water frame re-creates the primitive while off, its groups are
    // listed with nothing queued, the key comes back: the next send is whole. With the forget only in Decide this narrows, and the buffers differ.
    {
        f.Go({1}, {}, kIdx * 2);
        Check1(f.Go({1}, {}, kIdx * 2), 19);
        g_mode = 0;
        f.WaterFrame({1});
        const std::vector<int> g = Recreate(1, 50, 300);
        ListOnly(g, true);
        g_mode = 1;
        f.Cards(1, 5, 777);
        Check1(!f.Go({1}, {f.G(1, 5)}, kIdx * 2) && f.Sent() == 50, 19);
        Same(19);
    }
    // Queue too long, too many instances, crowded table.
    {
        std::vector<int32_t> many(static_cast<size_t>(kMaxQueue) + 1, 0);
        void* p = many.data();
        memcpy(f.lsd + g_n.list, &p, 8);
        const int32_t n = kMaxQueue + 1;
        memcpy(f.lsd + g_n.list + 8, &n, 4);
        f.BeginUpload(4096);
        const uint64_t q0 = Reason(kWQueue);
        Check1(!Decide(f.lsd, f.scene, f.ub, kIdx * 2) && Reason(kWQueue) == q0 + 1 && f.Sent() == 0, 30);
        p = f.list;
        memcpy(f.lsd + g_n.list, &p, 8);
        x.reset(f.lsd);
        f.SetLists({0}, {});
        f.BeginUpload(4096);
        const uint64_t b0 = Reason(kWTooBig);
        Check1(!Decide(f.lsd, f.scene, f.ub, kMaxDest * 2) && Reason(kWTooBig) == b0 + 1, 30);
        x.reset(f.lsd);
        // Every probe position of primitive 0 taken by another key: no record possible: whole, still right.
        f.SetLists({}, {});
        f.Go({0}, {}, kIdx * 2);
        ClearAll();
        const uint32_t h0 = HashOf(f.prim[0]);
        for (uint32_t i = 0; i < kProbe; ++i) g_slots[(h0 + i) & (kSlots - 1)].p = reinterpret_cast<const void*>(uintptr_t{16});
        g_used = kProbe;
        Check1(!f.Go({0}, {}, kIdx * 2) && f.Sent() == 101, 30);
        Check1(!f.Go({0}, {}, kIdx * 2) && f.Sent() == 101, 30);
        Same(30);
        // A table past 3/4 starts over.
        const uint64_t c0 = g_sum.clears.load();
        g_used = kSlots;
        f.Go({0}, {}, kIdx * 2);
        Check1(g_sum.clears.load() == c0 + 1, 30);
        Same(30);
    }
    // The public build has no value 2: the next case's switch to 1 is no switch, so go through 0 for the same forget.
    g_mode = 0;
    f.Go({1}, {}, kIdx * 2);
    // A primitive removed and re-added: same pointer, offset and count, every group new: all are in the list, so all go, with their new values.
    {
        g_mode = 1;
        f.Go({1}, {}, kIdx * 2);  // forgets at the switch
        f.Go({1}, {}, kIdx * 2);
        const std::vector<int> g = Recreate(1, 50, 300);
        f.SetLists({1}, {});
        int n = 0;
        for (int v : g) f.changed[n++] = v;
        memcpy(f.lsd + g_n.groupList + 8, &n, 4);
        Check1(f.Run(kIdx * 2) && f.Sent() == 50 && f.fixGpu[300] == -1, 23);
        Same(23);
    }
    // Groups created after a first send without groups: the no-groups send leaves no record: whole. A primitive under the size limit goes whole every time.
    {
        f.Prim(5, 40, 720, 0);
        f.model[5].clear();
        f.Go({5}, {}, kIdx * 2);
        f.PerInstance(5, 40, 720, 8000);
        Check1(!f.Go({5}, {}, kIdx * 2) && f.Sent() == 40, 24);
        Same(24);
        f.PerInstance(6, kMinNarrow - 1, 800, 9000);
        f.Go({6}, {}, kIdx * 2);
        Check1(!f.Go({6}, {}, kIdx * 2) && f.Sent() == static_cast<uint32_t>(kMinNarrow - 1), 24);
        Same(24);
    }
    // A neighbour sent whole right next to a narrowed primitive doesn't touch its ownership.
    {
        f.Prim(6, 8, 760, 0);
        f.model[6].clear();
        f.Go({5, 6}, {}, kIdx * 2);
        Check1(f.Go({5, 6}, {}, kIdx * 2) && f.Sent() == 8, 25);
        Same(25);
    }
    if (f.resetFlagBad) fail |= 1u << 31;
    g_mode = 0;
    g_lastMode = 0;
    g_ready = false;
    VirtualFree(x.mem, 0, MEM_RELEASE);
    VirtualFree(g_slots, 0, MEM_RELEASE);
    VirtualFree(g_own, 0, MEM_RELEASE);
    VirtualFree(g_queueFull, 0, MEM_RELEASE);
    VirtualFree(g_pairs, 0, MEM_RELEASE);
    g_slots = nullptr;
    g_own = nullptr;
    g_queueFull = nullptr;
    g_pairs = nullptr;
    g_n = savedN;
    g_lastLsd = g_lastBuf = nullptr;
    g_lastNum = 0;
    g_thread = 0;
    return static_cast<int>(fail);
}

}  // namespace opf
