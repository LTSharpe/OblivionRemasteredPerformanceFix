// LumenScanCull (worker lumenscan, 2026-11-10; findings "lumenscan", "lumenscan2", "bgload"; review "lumenscanrev"; public since
// 1.4.0, port "lumenpub"). The idea is Epic's: Unreal Engine 5.6.0 culls this scan before it reads the groups (two-level culling
// of the surface cache's primitive update, Epic commit 3e5ff8e28f). Written here as our own hook code for the game's UE 5.3 scan:
// per block of group indices instead of per primitive, with the stock body run unchanged on every block it may matter for.
// Lumen's per-frame CPU scan of every primitive group (UE 5.3 LumenSceneRendering.cpp:357,
// FLumenSurfaceCacheUpdatePrimitivesTask::AnyThreadTask, exe+0x1FBF320 on build 19115871) reads each ~80-byte group every frame:
// with MeshCardsMergeInstances 0 (the UE default) that is one group per instance, ~2 M groups, 8-10 ms of one thread, and it
// streams the whole array through the caches at the moment the old engine's thread runs (findings "bgload"). This makes the scan
// read only the groups whose result can differ from "nothing to do", with the same add and remove requests in the same order.
//
// [Fixes] LumenScanCull = 0: not installed (start-up); 1 (default since 1.4.0): the cull; 2: proof (both scans run,
// compared, stock kept). Then live 0 / 1 / 2. Start-up only install (version.dll route; not on the OBSE64 route). Every site and
// offset is found or checked by byte pattern (the whole scan body is the pattern: a game update that touches it turns the key
// off); any mismatch = not installed, with a log line.
//
// What the stock task emits, per allocated group g of its index range, in index order:
//   pass(g) = D(g) <= MaxDist^2 && R(g) >= (g.bEmissiveLightSource ? 1 : MinCardResolution)   (far field: other constants)
//   pass && g.MeshCardsIndex == -1 && g.bValidMeshCards  -> add {g, D(g)}
//   !pass && g.MeshCardsIndex >= 0                       -> remove {g}
// D = min over view origins of the squared distance to g's box (double, then float), R = TexelDensity * maxExtent /
// sqrt(max(D, 1)) + 0.01 (float). A group with MeshCardsIndex < 0 that is not (== -1 && valid) emits nothing at any distance
// ("inert"); a candidate (== -1 && valid) emits nothing while it fails.
//
// Identity argument:
//  1. Blocks of 64 group indices. A task whose range is not whole blocks runs stock (and everything is marked dirty). A block
//     that is not skipped runs the STOCK body over exactly its index range, on the same task object, so its requests are
//     appended to the same per-task arrays in the same order as a full stock run (runs of scanned blocks share one call).
//  2. A block is skipped only if: not dirty; its last scan saw no group with MeshCardsIndex >= 0, emitted no add, and saw no
//     far-field, merged-component (RayTracingGroupMapElementId valid) or non-finite / inverted box among its non-inert groups;
//     and every candidate provably fails with this frame's parameters (read from the task each time; nothing parameter-
//     dependent is kept): a lower bound of D over the block's box > MaxDist^2 * (1 + 1e-4), or the upper bound of R from
//     that D and the block's largest extent * (1 + 1e-4) < the block's lowest threshold. Every operation of the bound is
//     monotone in the box and the extent, and the 1e-4 margin covers the stock's float roundings and evaluation order.
//  3. A skipped block's summary stays valid while its groups don't change. In 5.3 every write to a group's allocation, box,
//     flags or MeshCardsIndex adds its index to PrimitiveGroupIndicesToUpdateInBuffer (LumenScene.cpp 911, 984, 1042, 1081,
//     1106, 1166; LumenMeshCards.cpp 810, 931, 969), except the merge-components path (LumenScene.cpp 965-978, which only
//     touches merged groups: their blocks are never skipped). Entries leave that list only at its reset at the end of
//     UpdateLumenScene (LumenSceneRendering.cpp:2314) or by r.LumenScene.UploadEveryFrame's refill with every index. The list
//     is read whole at the scan's start (the call into UpdateSurfaceCachePrimitives) and at every reset (LumenCardsNarrow's
//     reset site, or a reset-only patch there when that fix is off): each entry is seen at least once before it can go.
//     Its blocks are marked dirty; a dirty block is scanned and its flag cleared before its groups are read.
//  4. Anything else = everything dirty: another PrimitiveGroups array or another element / allocation-bit storage, any change
//     of the mode, any lists' reset seen while the mode is 0 (its entries are not read then), a misaligned task; a change of
//     the array's size marks the blocks it touches. A scan not entered through our call-site stub (not "armed") runs stock, and
//     so does a view-specific Lumen scene data (bViewSpecific: a scene capture's copy, the only kind CopyInitialData fills
//     without listing); the summaries keep describing the array they were built from. (Review lumenscanrev findings 1-3.)
//
// Proof (value 2): each task runs the cull first, its requests are copied aside, the arrays rewound, then the stock body over
// the whole range; both are compared element by element (index and distance bits). The stock output is what the game gets.
//
// The block summaries (box, largest extent, flags) and the dirty flags are plain arrays indexed by block (BlockSummaries()):
// the same "which blocks can matter this frame" test could later feed a GPU pass that tests only those blocks' groups.
#include "common.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace opf {
namespace {

// ---- The game's code (build 19115871; found by pattern, offsets read or pinned by the patterns) ----

// FLumenSurfaceCacheUpdatePrimitivesTask::AnyThreadTask, whole (rip displacements and call targets wildcarded). Pins the task's
// fields (+0 adds {data,num,max}, +0x10 removes, +0x30 &PrimitiveGroups, +0x38 / +0x68 / +0x70 view origins inline / heap / num,
// +0x78 first, +0x7C count, +0x84 MaxDistanceFromCamera, +0x88 TexelDensityScale, +0x8C MinCardResolution, +0x90 / +0x94 far
// field), the group's (stride 0x50, MeshCardsIndex +0x1C, box min +0x24 / max +0x30 floats, bValidMeshCards +0x44, bFarField
// +0x45, bEmissiveLightSource +0x47) and the sparse array's (data +0, num +8, allocation bits inline +0x10 / heap +0x20 / count +0x28).
constexpr char kBody[] =
    "4C 8B DC 56 41 54 41 56 48 81 EC F0 00 00 00 4C 63 71 78 48 8B F1 48 8B 41 30 8B 51 7C 41 03 D6 44 8B 40 08 41 3B D0 44 0F 4C C2 4D 63 E0 4D 3B F4 0F "
    "8D 7A 03 00 00 F3 0F 10 15 ?? ?? ?? ?? 41 8B CE 49 89 6B 18 4B 8D 2C B6 4D 89 7B D8 41 BF 01 00 00 00 41 0F 29 73 C8 41 0F 29 7B B8 45 0F 29 43 A8 45 "
    "0F 29 53 88 45 0F 29 9B 78 FF FF FF 44 0F 29 64 24 70 44 0F 29 6C 24 60 44 0F 29 74 24 50 44 0F 29 7C 24 40 F3 44 0F 10 3D ?? ?? ?? ?? 49 89 5B 10 41 "
    "D3 C7 48 C1 E5 04 49 89 7B E0 4D 2B E6 45 0F 29 4B 98 0F 1F 84 00 00 00 00 00 48 8B 56 30 44 3B 72 28 0F 8D 73 02 00 00 48 8B 42 20 48 8D 4A 10 48 85 "
    "C0 48 0F 45 C8 41 8B C6 45 85 F6 79 04 41 8D 46 1F C1 F8 05 48 98 44 85 3C 81 0F 84 49 02 00 00 48 8B 46 68 4C 8B 02 48 85 C0 F3 44 0F 10 8E 84 00 00 "
    "00 48 8D 56 38 F3 0F 10 3D ?? ?? ?? ?? 48 0F 45 D0 48 63 46 70 F3 45 0F 59 C9 48 8D 0C 40 4C 8D 0C CA 49 8D 04 28 49 3B D1 0F 84 F3 00 00 00 F3 45 0F "
    "10 44 28 38 F3 45 0F 10 54 28 30 F3 45 0F 10 5C 28 34 F3 44 0F 10 60 24 F3 44 0F 10 68 28 F3 44 0F 10 70 2C 45 0F 5A C0 0F 10 12 F2 0F 10 42 10 0F 57 "
    "DB F2 0F 11 44 24 30 0F 57 ED F3 41 0F 5A DC 0F 57 C0 0F 57 E4 66 0F 2F D3 0F 57 F6 0F 57 C9 0F 11 54 24 20 F3 41 0F 5A C2 F3 41 0F 5A EB F3 41 0F 5A "
    "E5 F3 41 0F 5A F6 73 09 0F 28 CA F2 0F 5C CB EB 0D 66 0F 2F D0 76 0B 0F 28 CA F2 0F 5C C8 F2 0F 59 C9 F2 0F 10 44 24 28 66 0F 2F C4 73 06 F2 0F 5C C4 "
    "EB 0A 66 0F 2F C5 76 0C F2 0F 5C C5 F2 0F 59 C0 F2 0F 58 C8 F2 0F 10 44 24 30 66 0F 2F C6 73 06 F2 0F 5C C6 EB 0C 66 41 0F 2F C0 76 0D F2 41 0F 5C C0 "
    "F2 0F 59 C0 F2 0F 58 C8 0F 5A C7 48 83 C2 18 F2 0F 5D C1 66 0F 5A F8 49 3B D1 0F 85 40 FF FF FF F3 0F 10 15 ?? ?? ?? ?? 41 80 7C 28 45 00 F3 0F 10 58 "
    "34 F3 0F 5C 58 28 F3 0F 10 40 30 F3 0F 5C 40 24 F3 0F 10 48 38 F3 0F 5C 48 2C F3 0F 59 DA F3 0F 59 C2 F3 0F 59 CA F3 0F 5F D8 0F 28 C7 F3 41 0F 5F C7 "
    "F3 0F 5F D9 0F 57 C9 F3 0F 51 C8 0F 28 D3 F3 0F 59 96 88 00 00 00 F3 0F 5E D1 F3 0F 58 15 ?? ?? ?? ?? 74 14 F3 44 0F 10 8E 90 00 00 00 0F 28 D3 F3 0F "
    "59 96 94 00 00 00 41 0F 2F F9 77 69 41 80 7C 28 47 00 74 06 41 0F 28 C7 EB 0B 66 0F 6E 86 8C 00 00 00 0F 5B C0 0F 2F D0 72 4B 41 83 7C 28 1C FF 75 6D "
    "41 80 7C 28 44 00 74 65 48 63 5E 08 F3 0F 11 BC 24 14 01 00 00 44 89 B4 24 10 01 00 00 8D 43 01 89 46 08 3B 46 0C 76 0A 8B D3 48 8B CE E8 ?? ?? ?? ?? "
    "48 8B 0E 48 8B 84 24 10 01 00 00 48 89 04 D9 EB 2A 41 83 7C 28 1C 00 7C 22 48 63 7E 18 8D 47 01 89 46 18 3B 46 1C 76 0B 8B D7 48 8D 4E 10 E8 ?? ?? ?? "
    "?? 48 8B 46 10 44 89 34 B8 F3 0F 10 15 ?? ?? ?? ?? 41 FF C6 41 D1 C7 48 83 C5 50 49 83 EC 01 0F 85 6B FD FF FF 44 0F 28 7C 24 40 44 0F 28 74 24 50 44 "
    "0F 28 6C 24 60 44 0F 28 64 24 70 44 0F 28 9C 24 80 00 00 00 44 0F 28 94 24 90 00 00 00 44 0F 28 8C 24 A0 00 00 00 44 0F 28 84 24 B0 00 00 00 0F 28 BC "
    "24 C0 00 00 00 0F 28 B4 24 D0 00 00 00 4C 8B BC 24 E0 00 00 00 48 8B BC 24 E8 00 00 00 48 8B AC 24 20 01 00 00 48 8B 9C 24 18 01 00 00 48 81 C4 F0 00 "
    "00 00 41 5E 41 5C 5E C3";
// The body's constants (rip-relative loads): insn offset, displacement offset, insn length, expected value.
struct Const { size_t at, disp, len; float value; };
constexpr Const kConsts[] = {
    {0x37, 4, 8, 0.5f}, {0x214, 4, 8, 0.5f}, {0x329, 4, 8, 0.5f},  // GetExtent's 0.5
    {0x88, 5, 9, 1.0f},                                             // max(D, 1), the emissive threshold
    {0xFF, 4, 8, FLT_MAX},                                          // D's start value
    {0x272, 4, 8, 0.01f},                                           // R's + 0.01
};

// ParallelFor's thunk for Tasks[Index].AnyThreadTask() (task size 0x98), which tail-jumps into the body.
constexpr char kThunk[] = "48 63 02 4C 8B C1 48 69 C8 98 00 00 00 49 8B 00 48 03 08 E9 ?? ?? ?? ??";
constexpr size_t kThunkJmp = 19;

// UpdateSurfaceCachePrimitives (LumenSceneRendering.cpp:995, exe+0x1FE4E90) up to the ParallelFor's lambda thunk: LumenSceneData in rcx,
// PrimitiveGroups.Num() at [rcx+num], the tasks built on &LumenSceneData.PrimitiveGroups ([rsi+groups]), task size 0x98.
constexpr char kDispatch[] =
    "4C 8B DC 49 89 5B 20 49 89 4B 08 55 56 41 55 41 56 41 57 49 8D 6B A9 48 81 EC C0 00 00 00 44 8B 05 ?? ?? ?? ?? 41 BF 01 00 00 00 8B 81 ?? ?? ?? ?? "
    "45 3B C7 4C 8B EA 41 0F 29 73 C8 45 0F 4F F8 41 0F 29 7B B8 FF C8 33 DB 41 03 C0 48 89 5D C7 99 48 89 5D CF 41 F7 F8 0F 28 F3 0F 28 FA 44 8B F0 48 8B "
    "F1 85 C0 0F 88 7A 03 00 00 49 89 7B 10 4D 89 63 18 45 85 F6 7E 68 41 8B D6 48 8D 4D C7 E8 ?? ?? ?? ?? 8B FB 66 0F 1F 84 00 00 00 00 00 48 63 5D CF 8D "
    "43 01 89 45 CF 3B 45 D3 76 0B 8B D3 48 8D 4D C7 E8 ?? ?? ?? ?? 44 89 7C 24 30 48 8D 96 ?? ?? ?? ?? 48 69 CB 98 00 00 00 0F 28 DF 89 7C 24 28 4D 8B C5 "
    "48 03 4D C7 F3 0F 11 74 24 20 E8 ?? ?? ?? ?? 41 03 FF 49 83 EE 01 75 B1 33 DB E8 ?? ?? ?? ?? 84 C0 74 0B 83 3D ?? ?? ?? ?? 00 8B C3 75 05 B8 01 00 00 "
    "00 8B 55 CF 48 8D 4D C7 48 89 4D B7 4C 8D 4D F7 48 8D 0D ?? ?? ?? ??";
constexpr size_t kDispatchNum = 0x2D, kDispatchGroups = 0xB2, kDispatchThunkLea = 0x109;

// The setup task's call (LumenSceneRendering.cpp:1473): movaps xmm3,xmm7; mov [rsp+20h],rax; movaps xmm2,xmm8; call UpdateSurfaceCachePrimitives.
// Five arguments (LumenSceneData, origins, detail in xmm2, distance in xmm3, the card renderer on the stack).
constexpr char kCallSite[] = "0F 28 DF 48 89 44 24 20 41 0F 28 D0 E8 ?? ?? ?? ??";
constexpr size_t kCallAt = 12;

// FLumenSceneData::bViewSpecific (RendererScene.cpp:1283-1285, a scene capture's own Lumen scene data): new FLumenSceneData(Default's
// bTrackAllPrimitives); bViewSpecific = true; SurfaceCacheResolution = clamp(..) right after it. Read +0x944 / +0x940 on 19115871.
constexpr char kViewSpec[] =
    "0F B6 92 ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 5D 35 ?? ?? ?? ?? C6 80 ?? ?? ?? ?? 01 48 89 5D ?? 48 89 45 ?? F3 0F 5F 35 ?? ?? ?? ?? F3 0F 11 B0 ?? ?? ?? ??";
constexpr size_t kViewSpecFlag = 22, kViewSpecRes = 47;

// FLumenPrimitiveGroup::RayTracingGroupMapElementId at +0x3C, CardResolutionScale +0x40, PrimitiveInstanceIndex +0x18: the merged-components
// group's creation (LumenScene.cpp:987-989): mov [rsi+3Ch],ebx; mov dword ptr [rsi+18h],-1; mov dword ptr [rsi+40h],1.0f
constexpr char kMergedLayout[] = "89 5E 3C C7 46 18 FF FF FF FF C7 46 40 00 00 80 3F";

// Task, group and array offsets (pinned by kBody / kMergedLayout).
constexpr uint32_t kTAdds = 0x00, kTRemoves = 0x10, kTGroups = 0x30, kTViewsInline = 0x38, kTViewsHeap = 0x68, kTViewsNum = 0x70, kTFirst = 0x78,
                   kTCount = 0x7C, kTMaxDist = 0x84, kTTexel = 0x88, kTMinRes = 0x8C, kTFarMaxSq = 0x90, kTFarTexel = 0x94;
constexpr uint32_t kGStride = 0x50, kGMeshCards = 0x1C, kGMin = 0x24, kGMax = 0x30, kGRtGroup = 0x3C, kGValid = 0x44, kGFar = 0x45, kGEmissive = 0x47;
constexpr uint32_t kAData = 0, kANum = 8, kABitsInline = 0x10, kABitsHeap = 0x20, kABitsNum = 0x28;

// ---- State ----
constexpr int kShift = 6, kBlock = 1 << kShift;   // 64 groups per block
constexpr int32_t kMaxBlocks = 1 << 18;           // 16.7 M groups; more = stock
constexpr int kMaxViews = 8;
constexpr double kMargin = 1e-4;

enum : uint8_t {
    kEmpty = 1,         // no candidate: nothing to emit at any distance
    kEmissive = 2,      // a candidate with bEmissiveLightSource (threshold 1)
    kCards = 4,         // a group with MeshCardsIndex >= 0 (may be removed)
    kMerged = 8,        // a merged-components group (changes without being listed)
    kFar = 16,          // a far-field candidate (its own constants: FarFieldCardMaxDistanceSq, extent * FarFieldCardTexelDensity)
    kBadBox = 32,       // a candidate's box not finite or inverted
    kAdded = 64,        // the last scan emitted an add here (cards may come)
    kNear = 128,        // a candidate that is not far field
    kAlways = kCards | kMerged | kBadBox | kAdded,
};

using BodyFn = void (*)(void* task);
BodyFn g_body = nullptr;
uint32_t g_pgOff = 0, g_listOff = 0;  // LumenSceneData: PrimitiveGroups, PrimitiveGroupIndicesToUpdateInBuffer's index array {data, num}
LumenScanBlock* g_sum = nullptr;
std::atomic<uint8_t>* g_dirty = nullptr;
std::atomic<int> g_mode{0};
std::atomic<bool> g_ready{false};

uint32_t g_viewSpecOff = 0;            // FLumenSceneData::bViewSpecific (a scene capture's own copy): those scans run stock

// Arming (prep / post around the dispatcher; one scan at a time).
std::mutex g_prepLock;
std::atomic<const uint8_t*> g_armed{nullptr};  // the PrimitiveGroups the armed scan is over
thread_local bool t_armedByMe = false;         // Post disarms only what its own Prep armed
// The summaries describe one array: its PrimitiveGroups address, element storage and allocation bits (any change = all dirty).
const uint8_t* g_key = nullptr;
const void* g_keyData = nullptr;
const void* g_keyBits = nullptr;
std::atomic<const uint8_t*> g_keyPublic{nullptr};
int32_t g_lastNum = 0;
// Everything dirty at the next Prep. Set by: start, any key change, any change of the mode (SetLumenScanCull), any lists'
// reset seen while not culling (mode 0: the list entries it empties are not read), a misaligned task, a proof mismatch.
std::atomic<bool> g_needAll{true};
std::atomic<bool> g_prepSinceReset{true};
bool g_testLegacyGap = false;                  // self-test only: the first version's mode handling (finding 1 reproduced)
int g_lastMode = 0;
double g_tscToMs = 0;

// Statistics: per thread, summed by the stats line.
struct Counters {  // 192 bytes: whole cache lines (the array is page-aligned)
    uint64_t tasks, tasksCull, tasksStock, misaligned, badViews, blocksScanned, blocksSkipped, skipEmpty, skipDist, skipRes, groupsScanned, groupsSkipped,
        calls, always[5], dirtyScanned, verified, mismatches, tsc, skipFar, pad[1];
};
static_assert(sizeof(Counters) % 64 == 0);
constexpr int kSlots = 128;
Counters* g_counters = nullptr;
std::atomic<int> g_slotNext{0};
thread_local Counters* t_c = nullptr;
Counters& C()
{
    if (!t_c) {
        const int s = g_slotNext.fetch_add(1, std::memory_order_relaxed);
        t_c = &g_counters[s < kSlots ? s : kSlots - 1];
    }
    return *t_c;
}
struct PrepStats { std::atomic<uint64_t> preps, prepTsc, listed, resetListed, resets, keyChanges, numChanges, allDirty, notArmed, viewSpecific, resetsOff, resetsNoPrep, verifyFallback; };
PrepStats g_prep{};
struct Mismatch { uint32_t task; int32_t first, count, nStock[2], nCull[2], at, kind; };
constexpr int kMaxMismatchLog = 8;
Mismatch g_mm[kMaxMismatchLog];
std::atomic<int> g_mmCount{0};
int g_mmLogged = 0;

template <class T> T Rd(const void* p, uint32_t off) { T v; memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof v); return v; }
template <class T> void Wr(void* p, uint32_t off, T v) { memcpy(static_cast<uint8_t*>(p) + off, &v, sizeof v); }

bool Allocated(const uint8_t* pg, int32_t g)
{
    if (g >= Rd<int32_t>(pg, kABitsNum)) return false;
    const uint8_t* const heap = Rd<const uint8_t*>(pg, kABitsHeap);
    const uint32_t* const bits = reinterpret_cast<const uint32_t*>(heap ? heap : pg + kABitsInline);
    return (bits[g >> 5] >> (g & 31)) & 1u;
}

void MarkIndex(int32_t g)
{
    if (g >= 0 && g < kMaxBlocks * kBlock) g_dirty[g >> kShift].store(1, std::memory_order_relaxed);
}

// The list's indices, whole: their blocks dirty.
uint64_t MarkList(const uint8_t* lsd)
{
    const int32_t* const data = Rd<const int32_t*>(lsd, g_listOff);
    const int32_t n = Rd<int32_t>(lsd, g_listOff + 8);
    if (!data || n <= 0) return 0;
    for (int32_t i = 0; i < n; ++i) MarkIndex(data[i]);
    return static_cast<uint64_t>(n);
}

void MarkAll(int32_t num)
{
    const int32_t blocks = (num + kBlock - 1) >> kShift;
    for (int32_t b = 0; b < blocks && b < kMaxBlocks; ++b) g_dirty[b].store(1, std::memory_order_relaxed);
}

// ---- The decision ----
struct Frame {
    double view[kMaxViews][3];
    int nv;
    float c2;       // MaxDist * MaxDist (float, as the stock)
    float tds;      // TexelDensityScale
    float minRes;   // (float)MinCardResolution
    float ffMaxSq;  // FarFieldCardMaxDistanceSq
    float ffTd;     // FarFieldCardTexelDensity
    float one, cut; // the body's 1.0 and 0.01
};
float g_one = 1.0f, g_cut = 0.01f;

// True when every candidate of the block provably fails (no request from the block). The block has no kAlways flag, is not empty.
// Near candidates: D > MaxDist^2, or R from the D bound and the largest extent < threshold. Far-field ones (the stock's other branch):
// D > FarFieldCardMaxDistanceSq, or largest extent * FarFieldCardTexelDensity < threshold (no distance in it).
bool Fails(const LumenScanBlock& s, const Frame& f, int* why)
{
    double dlo = static_cast<double>(FLT_MAX);
    for (int v = 0; v < f.nv; ++v) {
        double d = 0.0;
        for (int a = 0; a < 3; ++a) {
            const double p = f.view[v][a], lo = s.lo[a], hi = s.hi[a];
            double t = 0.0;
            if (p < lo) t = p - lo;
            else if (p > hi) t = p - hi;
            d += t * t;
        }
        if (d < dlo) dlo = d;
    }
    const double thr = (s.flags & kEmissive) && f.one < f.minRes ? f.one : f.minRes;
    int nearWhy = 0;
    if (s.flags & kNear) {
        const double c2 = static_cast<double>(f.c2);
        if (dlo > c2 * (1.0 + kMargin)) nearWhy = 1;
        else {
            const double r = static_cast<double>(f.tds) * static_cast<double>(s.maxExt) / std::sqrt(dlo > 1.0 ? dlo : 1.0) + static_cast<double>(f.cut);
            if (r * (1.0 + kMargin) < thr) nearWhy = 2;
        }
        if (!nearWhy) return false;
    }
    if (s.flags & kFar) {
        const bool farFails = dlo > static_cast<double>(f.ffMaxSq) * (1.0 + kMargin) ||
                              static_cast<double>(s.maxExtFar) * static_cast<double>(f.ffTd) * (1.0 + kMargin) < thr;
        if (!farFails) return false;
        if (!nearWhy) nearWhy = 3;
    }
    *why = nearWhy;
    return true;
}

// The block's summary from its groups as they are now (right after the stock read them).
void Rebuild(LumenScanBlock& s, const uint8_t* pg, int32_t b0, int32_t b1, bool added, Counters& c)
{
    const uint8_t* const data = Rd<const uint8_t*>(pg, kAData);
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX}, ext = 0.0f, extFar = 0.0f;
    uint8_t flags = added ? kAdded : 0;
    int cand = 0;
    for (int32_t g = b0; g < b1; ++g) {
        if (!Allocated(pg, g)) continue;
        const uint8_t* const e = data + static_cast<int64_t>(g) * kGStride;
        if (Rd<int32_t>(e, kGRtGroup) != -1) { flags |= kMerged; continue; }
        const int32_t mci = Rd<int32_t>(e, kGMeshCards);
        if (mci >= 0) { flags |= kCards; continue; }
        if (mci != -1 || Rd<uint8_t>(e, kGValid) == 0) continue;  // inert
        float mn[3], mx[3];
        memcpy(mn, e + kGMin, 12);
        memcpy(mx, e + kGMax, 12);
        bool ok = true;
        for (int a = 0; a < 3; ++a) ok = ok && std::isfinite(mn[a]) && std::isfinite(mx[a]) && mn[a] <= mx[a];
        if (!ok) { flags |= kBadBox; continue; }
        for (int a = 0; a < 3; ++a) {
            if (mn[a] < lo[a]) lo[a] = mn[a];
            if (mx[a] > hi[a]) hi[a] = mx[a];
        }
        // GetExtent().GetMax() as the stock computes it (float; y, x, z).
        float x = (mx[1] - mn[1]) * 0.5f;
        const float ex = (mx[0] - mn[0]) * 0.5f, ez = (mx[2] - mn[2]) * 0.5f;
        if (ex > x) x = ex;
        if (ez > x) x = ez;
        if (Rd<uint8_t>(e, kGFar) != 0) {
            flags |= kFar;
            if (x > extFar) extFar = x;
        } else {
            flags |= kNear;
            if (x > ext) ext = x;
        }
        if (Rd<uint8_t>(e, kGEmissive) != 0) flags |= kEmissive;
        ++cand;
    }
    if (!cand && !(flags & kAlways)) flags |= kEmpty;
    memcpy(s.lo, lo, sizeof lo);
    memcpy(s.hi, hi, sizeof hi);
    s.maxExt = ext;
    s.maxExtFar = extFar;
    s.flags = flags;
    if (flags & kCards) ++c.always[0];
    if (flags & kMerged) ++c.always[1];
    if (flags & kFar) ++c.always[2];
    if (flags & kBadBox) ++c.always[3];
    if (flags & kAdded) ++c.always[4];
}

// One stock call over [from, to) of the task (the task's own range fields narrowed for the call).
void StockRange(uint8_t* task, int32_t from, int32_t to)
{
    Wr<int32_t>(task, kTFirst, from);
    Wr<int32_t>(task, kTCount, to - from);
    g_body(task);
}

// The cull. False = the task was run stock (whole).
bool Culled(uint8_t* task, Counters& c)
{
    const uint8_t* const pg = Rd<const uint8_t*>(task, kTGroups);
    const int32_t first = Rd<int32_t>(task, kTFirst), count = Rd<int32_t>(task, kTCount);
    if (count <= 0 || first < 0 || (first & (kBlock - 1)) || (count & (kBlock - 1))) {
        ++c.misaligned;
        g_needAll.store(true, std::memory_order_relaxed);
        g_body(task);
        return false;
    }
    const int32_t num = Rd<int32_t>(pg, kANum);
    const int64_t lastWide = static_cast<int64_t>(first) + count;
    const int32_t last = static_cast<int32_t>(lastWide < num ? lastWide : num);
    Frame f;
    f.nv = Rd<int32_t>(task, kTViewsNum);
    bool ok = f.nv >= 1 && f.nv <= kMaxViews && last <= kMaxBlocks * kBlock;
    if (ok) {
        const uint8_t* const heap = Rd<const uint8_t*>(task, kTViewsHeap);
        const double* const v = reinterpret_cast<const double*>(heap ? heap : task + kTViewsInline);
        for (int i = 0; i < f.nv; ++i)
            for (int a = 0; a < 3; ++a) {
                f.view[i][a] = v[i * 3 + a];
                ok = ok && std::isfinite(f.view[i][a]);
            }
    }
    if (!ok) {
        ++c.badViews;
        g_body(task);  // nothing summarised, nothing cleared: the blocks keep their state
        return false;
    }
    const float maxDist = Rd<float>(task, kTMaxDist);
    f.c2 = maxDist * maxDist;
    f.tds = Rd<float>(task, kTTexel);
    f.minRes = static_cast<float>(Rd<int32_t>(task, kTMinRes));
    f.ffMaxSq = Rd<float>(task, kTFarMaxSq);
    f.ffTd = Rd<float>(task, kTFarTexel);
    f.one = g_one;
    f.cut = g_cut;
    ++c.tasksCull;
    const int32_t* addData = nullptr;
    int32_t runFrom = -1;  // first block of the current run of scanned blocks
    const auto flush = [&](int32_t runTo /* block, exclusive */) {
        if (runFrom < 0) return;
        const int32_t from = runFrom << kShift;
        const int64_t toWide = static_cast<int64_t>(runTo) << kShift;
        const int32_t to = static_cast<int32_t>(toWide < last ? toWide : last);
        const int32_t a0 = Rd<int32_t>(task, kTAdds + 8);
        StockRange(task, from, to);
        ++c.calls;
        const int32_t a1 = Rd<int32_t>(task, kTAdds + 8);
        addData = Rd<const int32_t*>(task, kTAdds);  // {index, distance} pairs; the data may have moved
        int32_t k = a0;
        for (int32_t b = runFrom; b < runTo; ++b) {
            const int32_t b0 = b << kShift, b1 = b0 + kBlock < to ? b0 + kBlock : to;
            bool added = false;
            while (k < a1 && addData[k * 2] < b1) { added = added || addData[k * 2] >= b0; ++k; }
            Rebuild(g_sum[b], pg, b0, b1, added, c);
            c.groupsScanned += static_cast<uint64_t>(b1 - b0);
        }
        runFrom = -1;
    };
    const int32_t bFirst = first >> kShift;
    const int32_t bEnd = static_cast<int32_t>((static_cast<int64_t>(last) + kBlock - 1) >> kShift);
    for (int32_t b = bFirst; b < bEnd; ++b) {
        const LumenScanBlock& s = g_sum[b];
        bool skip = false;
        int why = 0;
        if (!g_dirty[b].load(std::memory_order_relaxed) && !(s.flags & kAlways)) {
            if (s.flags & kEmpty) { skip = true; why = 0; }
            else skip = Fails(s, f, &why);
        }
        if (skip) {
            flush(b);
            ++c.blocksSkipped;
            const int32_t b0 = b << kShift, b1 = b0 + kBlock < last ? b0 + kBlock : last;
            c.groupsSkipped += static_cast<uint64_t>(b1 - b0);
            if (why == 0) ++c.skipEmpty;
            else if (why == 1) ++c.skipDist;
            else if (why == 2) ++c.skipRes;
            else ++c.skipFar;
            continue;
        }
        if (g_dirty[b].exchange(0, std::memory_order_acq_rel)) ++c.dirtyScanned;  // cleared before the groups are read
        ++c.blocksScanned;
        if (runFrom < 0) runFrom = b;
    }
    flush(bEnd);
    Wr<int32_t>(task, kTFirst, first);
    Wr<int32_t>(task, kTCount, count);
    return true;
}

thread_local std::vector<uint64_t> t_adds;
thread_local std::vector<int32_t> t_removes;

void Verify(uint8_t* task, Counters& c)
{
    const int32_t a0 = Rd<int32_t>(task, kTAdds + 8), r0 = Rd<int32_t>(task, kTRemoves + 8);
    const int32_t first = Rd<int32_t>(task, kTFirst), count = Rd<int32_t>(task, kTCount);
    if (!Culled(task, c)) return;  // ran stock already
    const int32_t a1 = Rd<int32_t>(task, kTAdds + 8), r1 = Rd<int32_t>(task, kTRemoves + 8);
    const uint64_t* ad = Rd<const uint64_t*>(task, kTAdds);
    const int32_t* rd = Rd<const int32_t*>(task, kTRemoves);
    bool copied = true;
    try {
        t_adds.assign(ad + a0, ad + a1);
        t_removes.assign(rd + r0, rd + r1);
    } catch (...) {  // no memory for the copy: this task is not compared, the game still gets the stock output
        copied = false;
    }
    Wr<int32_t>(task, kTAdds + 8, a0);
    Wr<int32_t>(task, kTRemoves + 8, r0);
    g_body(task);  // stock, whole range: its output is what the game gets
    if (!copied) {
        g_prep.verifyFallback.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int32_t sa = Rd<int32_t>(task, kTAdds + 8), sr = Rd<int32_t>(task, kTRemoves + 8);
    ad = Rd<const uint64_t*>(task, kTAdds);
    rd = Rd<const int32_t*>(task, kTRemoves);
    ++c.verified;
    int at = -1, kind = 0;
    if (sa - a0 != static_cast<int32_t>(t_adds.size())) kind = 1;
    else if (sr - r0 != static_cast<int32_t>(t_removes.size())) kind = 2;
    else {
        for (size_t i = 0; i < t_adds.size() && at < 0; ++i) if (ad[a0 + i] != t_adds[i]) { at = static_cast<int>(i); kind = 3; }
        for (size_t i = 0; i < t_removes.size() && at < 0; ++i) if (rd[r0 + i] != t_removes[i]) { at = static_cast<int>(i); kind = 4; }
    }
    if (kind) {
        ++c.mismatches;
        const int n = g_mmCount.fetch_add(1, std::memory_order_relaxed);
        if (n < kMaxMismatchLog) {
            Mismatch& m = g_mm[n];
            m.task = static_cast<uint32_t>(first / (count ? count : 1));
            m.first = first;
            m.count = count;
            m.nStock[0] = sa - a0;
            m.nStock[1] = sr - r0;
            m.nCull[0] = static_cast<int32_t>(t_adds.size());
            m.nCull[1] = static_cast<int32_t>(t_removes.size());
            m.at = at;
            m.kind = kind;
        }
        // The summaries of this range may be wrong: start over.
        g_needAll.store(true, std::memory_order_relaxed);
    }
}

}  // namespace

// ---- Hooks (called from the game) ----

// The thunk's jump lands here (any worker thread; rcx = the task).
void LumenScanCullTask(void* taskv)
{
    uint8_t* const task = static_cast<uint8_t*>(taskv);
    Counters& c = C();
    const uint64_t t0 = __rdtsc();
    ++c.tasks;
    const int mode = g_mode.load(std::memory_order_relaxed);
    const uint8_t* const armed = g_armed.load(std::memory_order_acquire);
    if (mode == 0 || !armed || Rd<const uint8_t*>(task, kTGroups) != armed) {
        ++c.tasksStock;
        g_body(task);
    } else if (mode == 2) {
        Verify(task, c);
    } else {
        Culled(task, c);
    }
    c.tsc += __rdtsc() - t0;
}

// The setup task, right before UpdateSurfaceCachePrimitives (rcx = LumenSceneData), then after it.
void LumenScanCullPrep(void* lsdv)
{
    const uint8_t* const lsd = static_cast<const uint8_t*>(lsdv);
    const uint64_t t0 = __rdtsc();
    std::lock_guard<std::mutex> lock(g_prepLock);
    g_prep.preps.fetch_add(1, std::memory_order_relaxed);
    g_prepSinceReset.store(true, std::memory_order_relaxed);
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode != g_lastMode) {
        g_lastMode = mode;
        g_needAll.store(true, std::memory_order_relaxed);
    }
    if (mode == 0 || g_armed.load(std::memory_order_relaxed)) {  // off, or another scan is armed (it keeps its key)
        if (mode) g_prep.notArmed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // A view-specific copy (CopyInitialData copies the groups without listing them): stock, the summaries keep their array.
    if (g_viewSpecOff && Rd<uint8_t>(lsd, g_viewSpecOff) != 0) {
        g_prep.viewSpecific.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint8_t* const pg = lsd + g_pgOff;
    const int32_t num = Rd<int32_t>(pg, kANum);
    if (num < 0 || num > kMaxBlocks * kBlock) {
        g_prep.notArmed.fetch_add(1, std::memory_order_relaxed);
        g_needAll.store(true, std::memory_order_relaxed);
        return;
    }
    const void* const data = Rd<const void*>(pg, kAData);
    const void* const heapBits = Rd<const void*>(pg, kABitsHeap);
    const void* const bits = heapBits ? heapBits : static_cast<const void*>(pg + kABitsInline);
    if (pg != g_key || data != g_keyData || bits != g_keyBits) {
        g_key = pg;
        g_keyData = data;
        g_keyBits = bits;
        g_keyPublic.store(pg, std::memory_order_relaxed);
        g_prep.keyChanges.fetch_add(1, std::memory_order_relaxed);
        g_needAll.store(true, std::memory_order_relaxed);
    }
    if (num != g_lastNum) {
        const int32_t lo = (num < g_lastNum ? num : g_lastNum) >> kShift;
        const int32_t hi = static_cast<int32_t>(((static_cast<int64_t>(num > g_lastNum ? num : g_lastNum)) + kBlock - 1) >> kShift);
        for (int32_t b = lo; b < hi && b < kMaxBlocks; ++b) g_dirty[b].store(1, std::memory_order_relaxed);
        g_lastNum = num;
        g_prep.numChanges.fetch_add(1, std::memory_order_relaxed);
    }
    g_prep.listed.fetch_add(MarkList(lsd), std::memory_order_relaxed);
    if (g_needAll.exchange(false, std::memory_order_relaxed)) {
        MarkAll(num);
        g_prep.allDirty.fetch_add(1, std::memory_order_relaxed);
    }
    g_armed.store(pg, std::memory_order_release);
    t_armedByMe = true;
    g_prep.prepTsc.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
}

void LumenScanCullPost()
{
    if (!t_armedByMe) return;  // another scan armed (or nothing did): not ours to disarm
    t_armedByMe = false;
    g_armed.store(nullptr, std::memory_order_release);  // ParallelFor has returned: every task of this scan is done
}

// At the lists' reset (render thread), before they are emptied.
void LumenScanCullListsReset(const void* lsdv)
{
    if (!g_ready.load(std::memory_order_relaxed)) return;
    g_prep.resets.fetch_add(1, std::memory_order_relaxed);
    if (!g_prepSinceReset.exchange(false, std::memory_order_relaxed)) g_prep.resetsNoPrep.fetch_add(1, std::memory_order_relaxed);
    if (!g_mode.load(std::memory_order_relaxed)) {
        // Not culling: what this reset empties is not read, so nothing the summaries say can be trusted any more.
        g_prep.resetsOff.fetch_add(1, std::memory_order_relaxed);
        if (!g_testLegacyGap) g_needAll.store(true, std::memory_order_relaxed);
        return;
    }
    const uint8_t* const lsd = static_cast<const uint8_t*>(lsdv);
    if (lsd + g_pgOff != g_keyPublic.load(std::memory_order_relaxed)) return;  // not the array we describe
    g_prep.resetListed.fetch_add(MarkList(lsd), std::memory_order_relaxed);
}

LumenScanBlock* LumenScanBlocks(int32_t* count)
{
    if (count) *count = g_lastNum >> kShift;
    return g_sum;
}

namespace {

struct ScanSites {
    uint8_t* body, *thunkJmp, *dispatch, *call;
    uint32_t pgOff, numOff, viewSpecOff;
    float consts[6];
};

bool Discover(const GameModule& game, ScanSites* s, const char** why)
{
    const auto bodies = FindAll(game.text, ParsePattern(kBody));
    if (bodies.size() != 1) { *why = "the scan task's body not found once"; return false; }
    s->body = bodies[0];
    for (size_t i = 0; i < sizeof kConsts / sizeof kConsts[0]; ++i) {
        const uint8_t* const p = RipTarget(s->body + kConsts[i].at, kConsts[i].disp, kConsts[i].len);
        if (!game.rdata.Contains(p) && !game.data.Contains(p) && !game.text.Contains(p)) { *why = "a constant of the body is outside the image"; return false; }
        memcpy(&s->consts[i], p, 4);
        if (memcmp(&s->consts[i], &kConsts[i].value, 4) != 0) { *why = "a constant of the body is not the one expected"; return false; }
    }
    uint8_t* thunk = nullptr;
    for (uint8_t* h : FindAll(game.text, ParsePattern(kThunk))) {
        if (RipTarget(h + kThunkJmp, 1, 5) != s->body) continue;
        if (thunk) { *why = "the task thunk found twice"; return false; }
        thunk = h;
    }
    if (!thunk) { *why = "the task thunk not found"; return false; }
    s->thunkJmp = thunk + kThunkJmp;
    uint8_t* disp = nullptr;
    for (uint8_t* h : FindAll(game.text, ParsePattern(kDispatch))) {
        if (RipTarget(h + kDispatchThunkLea, 3, 7) != thunk) continue;
        if (disp) { *why = "UpdateSurfaceCachePrimitives found twice"; return false; }
        disp = h;
    }
    if (!disp) { *why = "UpdateSurfaceCachePrimitives not found"; return false; }
    s->dispatch = disp;
    memcpy(&s->numOff, disp + kDispatchNum, 4);
    memcpy(&s->pgOff, disp + kDispatchGroups, 4);
    if (s->numOff != s->pgOff + 8) { *why = "PrimitiveGroups' data and count are not neighbours"; return false; }
    uint8_t* call = nullptr;
    int foreign = 0;  // the call's code is there but goes elsewhere: another patch owns it (another mod; in lab builds a probe)
    for (uint8_t* h : FindAll(game.text, ParsePattern(kCallSite))) {
        const uint8_t* const t = RipTarget(h + kCallAt, 1, 5);
        if (t != disp) { if (!game.text.Contains(t)) ++foreign; continue; }
        if (call) { *why = "the call to UpdateSurfaceCachePrimitives found twice"; return false; }
        call = h + kCallAt;
    }
    if (!call) {
        *why = foreign ? "the call to UpdateSurfaceCachePrimitives is already redirected by another patch (another mod?)"
                       : "the call to UpdateSurfaceCachePrimitives not found";
        return false;
    }
    s->call = call;
    if (FindAll(game.text, ParsePattern(kMergedLayout)).size() != 1) { *why = "the merged group's layout (+0x3C) not found once"; return false; }
    const auto vs = FindAll(game.text, ParsePattern(kViewSpec));
    if (vs.size() != 1) { *why = "FLumenSceneData::bViewSpecific's set-up not found once"; return false; }
    uint32_t flag = 0, res = 0;
    memcpy(&flag, vs[0] + kViewSpecFlag, 4);
    memcpy(&res, vs[0] + kViewSpecRes, 4);
    if (flag != res + 4 || flag < 0x200 || flag > 0x4000) { *why = "FLumenSceneData::bViewSpecific's offset is not the expected neighbour"; return false; }
    s->viewSpecOff = flag;
    return true;
}

// The call-site stub: keeps the five arguments, Prep(LumenSceneData), the original, Post. Entered by a call (rsp = 8 mod 16).
constexpr size_t kPrepStubMax = 128;
size_t BuildPrepStub(uint8_t* m, const void* prep, const void* original, const void* post)
{
    const auto Put = [&m](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *m++ = x; };
    const auto Ptr = [&m](const void* p) { memcpy(m, &p, 8); m += 8; };
    uint8_t* const start = m;
    Put({0x48, 0x83, 0xEC, 0x58});                    // sub rsp,58h
    Put({0x48, 0x89, 0x4C, 0x24, 0x30});              // mov [rsp+30h],rcx
    Put({0x48, 0x89, 0x54, 0x24, 0x38});              // mov [rsp+38h],rdx
    Put({0x4C, 0x89, 0x44, 0x24, 0x40});              // mov [rsp+40h],r8
    Put({0x4C, 0x89, 0x4C, 0x24, 0x48});              // mov [rsp+48h],r9
    Put({0xF2, 0x0F, 0x11, 0x54, 0x24, 0x50});        // movsd [rsp+50h],xmm2
    Put({0xF2, 0x0F, 0x11, 0x5C, 0x24, 0x28});        // movsd [rsp+28h],xmm3 (above the callee's 20h home area)
    Put({0x48, 0xB8}); Ptr(prep);                     // mov rax,Prep
    Put({0xFF, 0xD0});                                // call rax (rcx = LumenSceneData)
    Put({0x48, 0x8B, 0x4C, 0x24, 0x30});              // mov rcx,[rsp+30h]
    Put({0x48, 0x8B, 0x54, 0x24, 0x38});              // mov rdx,[rsp+38h]
    Put({0x4C, 0x8B, 0x44, 0x24, 0x40});              // mov r8,[rsp+40h]
    Put({0x4C, 0x8B, 0x4C, 0x24, 0x48});              // mov r9,[rsp+48h]
    Put({0xF2, 0x0F, 0x10, 0x54, 0x24, 0x50});        // movsd xmm2,[rsp+50h]
    Put({0xF2, 0x0F, 0x10, 0x5C, 0x24, 0x28});        // movsd xmm3,[rsp+28h]
    Put({0x48, 0x8B, 0x84, 0x24, 0x80, 0, 0, 0});     // mov rax,[rsp+80h]  the caller's fifth argument
    Put({0x48, 0x89, 0x44, 0x24, 0x20});              // mov [rsp+20h],rax
    Put({0x48, 0xB8}); Ptr(original);                 // mov rax,UpdateSurfaceCachePrimitives
    Put({0xFF, 0xD0});                                // call rax
    Put({0x48, 0xB8}); Ptr(post);                     // mov rax,Post
    Put({0xFF, 0xD0});                                // call rax
    Put({0x48, 0x83, 0xC4, 0x58});                    // add rsp,58h
    Put({0xC3});                                      // ret
    return static_cast<size_t>(m - start);
}
// Its unwind data: version 1, prolog 4 bytes, one code: at 4, UWOP_ALLOC_SMALL (58h - 8) / 8 = 10.
constexpr uint8_t kPrepUnwind[8] = {0x01, 0x04, 0x01, 0x00, 0x04, 0xA2, 0x00, 0x00};

void WriteJmpAbs(uint8_t* m, const void* target)
{
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(m, jmpAbs, 6);
    memcpy(m + 6, &target, 8);
}

bool PatchRel32(uint8_t* insn, uint8_t* target)  // E8 / E9 rel32
{
    const int64_t d = target - (insn + 5);
    const int32_t rel = static_cast<int32_t>(d);
    if (d != rel) return false;
    return WriteCode(insn + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel), 4));
}

bool AllocState()
{
    g_sum = static_cast<LumenScanBlock*>(VirtualAlloc(nullptr, sizeof(LumenScanBlock) * kMaxBlocks, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_dirty = static_cast<std::atomic<uint8_t>*>(VirtualAlloc(nullptr, kMaxBlocks, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_counters = static_cast<Counters*>(VirtualAlloc(nullptr, sizeof(Counters) * kSlots, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    return g_sum && g_dirty && g_counters;
}

void CalibrateTsc()
{
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    const uint64_t t0 = __rdtsc();
    do QueryPerformanceCounter(&q1); while (q1.QuadPart - q0.QuadPart < f.QuadPart / 50);
    const uint64_t t1 = __rdtsc();
    const double ms = static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
    g_tscToMs = ms / static_cast<double>(t1 - t0);
}

struct Snap { uint64_t v[48]; };
Snap g_last[2]{};          // [0] the release line, [1] the lab's detailed lines
LARGE_INTEGER g_lastQ[2]{};

// Every change of the mode leaves every block dirty: nothing is tracked while off, and a value change is the only moment the
// cull can't account for. needAll first, then the mode (a Prep that sees the new mode also sees needAll).
void ChangeMode(int v)
{
    if (!g_testLegacyGap) g_needAll.store(true, std::memory_order_seq_cst);
    g_mode.store(v, std::memory_order_seq_cst);
}

}  // namespace

void SetLumenScanCull(int value)
{
    if (!g_ready.load(std::memory_order_relaxed)) return;
    const int v = value == 1 || value == 2 ? value : 0;
    if (v != g_mode.load(std::memory_order_relaxed)) {
        ChangeMode(v);
        Log("[lumenscan] LumenScanCull=%d", v);
    }
}

// One short line per stats interval (no line when nothing ran); MISMATCH lines (proof mode) always. Lab builds: detailed = the
// 10 s lines of LumenScanCullStats=1.
void LogLumenScanCullStats(bool detailed)
{
    if (!g_ready.load(std::memory_order_relaxed)) return;
    const int li = detailed ? 1 : 0;
    Snap n{};
    for (int i = 0; i < kSlots; ++i) {
        const Counters& c = g_counters[i];
        const uint64_t f[] = {c.tasks, c.tasksCull, c.tasksStock, c.misaligned, c.badViews, c.blocksScanned, c.blocksSkipped, c.skipEmpty, c.skipDist, c.skipRes,
                              c.groupsScanned, c.groupsSkipped, c.calls, c.always[0], c.always[1], c.always[2], c.always[3], c.always[4], c.dirtyScanned,
                              c.verified, c.mismatches, c.tsc};
        for (size_t k = 0; k < sizeof f / sizeof f[0]; ++k) n.v[k] += f[k];
    }
    const auto L = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    const uint64_t p[] = {L(g_prep.preps), L(g_prep.prepTsc), L(g_prep.listed), L(g_prep.resetListed), L(g_prep.resets), L(g_prep.keyChanges), L(g_prep.numChanges),
                          L(g_prep.allDirty), L(g_prep.notArmed)};
    for (size_t k = 0; k < sizeof p / sizeof p[0]; ++k) n.v[22 + k] = p[k];
    for (int i = 0; i < kSlots; ++i) n.v[31] += g_counters[i].skipFar;
    n.v[32] = L(g_prep.viewSpecific);
    n.v[33] = L(g_prep.resetsOff);
    n.v[34] = L(g_prep.resetsNoPrep);
    n.v[35] = L(g_prep.verifyFallback);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    const auto D = [&](int k) { return static_cast<unsigned long long>(n.v[k] - g_last[li].v[k]); };
    if (D(0) == 0 && D(22) == 0) { g_last[li] = n; g_lastQ[li] = q; return; }
    const double frames = D(22) ? static_cast<double>(D(22)) : 1.0;
    const double groups = static_cast<double>(D(10) + D(11));
    const int mm = g_mmCount.load(std::memory_order_relaxed);
    for (; g_mmLogged < mm && g_mmLogged < kMaxMismatchLog; ++g_mmLogged) {
        const Mismatch& m = g_mm[g_mmLogged];
        Log("[lumenscan] MISMATCH %d: task range %d+%d, kind %d (1 adds count, 2 removes count, 3 add differs, 4 remove differs) at %d; stock adds %d removes %d, cull adds %d removes %d",
            g_mmLogged + 1, m.first, m.count, m.kind, m.at, m.nStock[0], m.nStock[1], m.nCull[0], m.nCull[1]);
    }
    if (!detailed) {
        Log("[lumenscan] stats: mode %d, scans %llu, groups per scan %.0f, read %.1f %%, scan CPU %.2f ms per scan; compared %llu, mismatches %llu",
            g_mode.load(), D(22), groups / frames, groups > 0 ? 100.0 * static_cast<double>(D(10)) / groups : 0.0,
            static_cast<double>(D(21)) * g_tscToMs / frames, D(19), D(20));
        g_last[li] = n;
        g_lastQ[li] = q;
        return;
    }
    g_last[li] = n;
    g_lastQ[li] = q;
}

// Start-up only (version.dll). key = LumenScanCull at start-up; 0 = not installed.
bool InstallLumenScanCull(bool beforeGameCode, int key)
{
    if (!key) { Log("[lumenscan] LumenScanCull off in OblivionPerfFix.ini (takes a restart to switch on)"); return false; }
    if (!beforeGameCode) { Log("[lumenscan] LumenScanCull: start-up only (version.dll), not installed"); return false; }
    const GameModule& game = Game();
    ScanSites s{};
    const char* why = "";
    if (!Discover(game, &s, &why)) { Log("[lumenscan] LumenScanCull: %s: not installed", why); return false; }
    uint8_t* mcCall = nullptr;
    uint8_t* mcFn = nullptr;
    LumenCardsOffsets o{};
    if (!LumenMeshCardsDiscover(game, &mcCall, &mcFn, &o, &why)) { Log("[lumenscan] LumenScanCull: %s: not installed", why); return false; }
    if (o.groupsData != s.pgOff) { Log("[lumenscan] LumenScanCull: PrimitiveGroups at +0x%X here, +0x%X in UpdateLumenMeshCards: not installed", s.pgOff, o.groupsData); return false; }
    if (!AllocState()) { Log("[lumenscan] LumenScanCull: no memory: not installed"); return false; }
    CalibrateTsc();
    g_body = reinterpret_cast<BodyFn>(s.body);
    g_pgOff = s.pgOff;
    g_listOff = o.groupList;
    g_viewSpecOff = s.viewSpecOff;
    g_one = s.consts[3];
    g_cut = s.consts[5];
    // The block: task trampoline (14), prep stub, its unwind info, the function table.
    constexpr size_t kOffPrep = 16, kOffUnwind = kOffPrep + kPrepStubMax, kOffTable = kOffUnwind + 16, kSize = kOffTable + sizeof(RUNTIME_FUNCTION);
    uint8_t* const mem = AllocNearGame(kSize);
    if (!mem) { Log("[lumenscan] LumenScanCull: no memory near the game's code: not installed"); return false; }
    WriteJmpAbs(mem, reinterpret_cast<const void*>(&LumenScanCullTask));
    const size_t len = BuildPrepStub(mem + kOffPrep, reinterpret_cast<const void*>(&LumenScanCullPrep), s.dispatch, reinterpret_cast<const void*>(&LumenScanCullPost));
    memcpy(mem + kOffUnwind, kPrepUnwind, sizeof kPrepUnwind);
    auto* const table = reinterpret_cast<RUNTIME_FUNCTION*>(mem + kOffTable);
    table[0].BeginAddress = static_cast<DWORD>(kOffPrep);
    table[0].EndAddress = static_cast<DWORD>(kOffPrep + len);
    table[0].UnwindData = static_cast<DWORD>(kOffUnwind);
    DWORD old;
    VirtualProtect(mem, kSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, kSize);
    if (!RtlAddFunctionTable(table, 1, reinterpret_cast<DWORD64>(mem))) Log("[lumenscan] no unwind data for the stub: a stack in a dump won't walk past it");
    // Dirty capture at the lists' reset first: without it nothing may be skipped.
    if (!LumenListsResetObserve(&LumenScanCullListsReset, &why)) { Log("[lumenscan] LumenScanCull: the lists' reset: %s: not installed", why); return false; }
    g_mode = key == 1 || key == 2 ? key : 0;
    g_lastMode = g_mode.load();
    g_ready = true;
    // The thunk first (unarmed = stock), the call site last (arms).
    if (!PatchRel32(s.thunkJmp, mem)) { Log("[lumenscan] LumenScanCull: could not patch the task thunk: stays stock"); g_ready = false; return false; }
    if (!PatchRel32(s.call, mem + kOffPrep)) { Log("[lumenscan] LumenScanCull: could not patch the call: stays stock (tasks run stock)"); return false; }
    Log("[lumenscan] LumenScanCull installed: body exe+0x%X, thunk jump exe+0x%X, UpdateSurfaceCachePrimitives exe+0x%X (call exe+0x%X); PrimitiveGroups +0x%X, "
        "changed groups +0x%X; blocks of %d; mode %d (0 stock, 1 cull, 2 cull with proof; live)",
        static_cast<unsigned>(s.body - game.base), static_cast<unsigned>(s.thunkJmp - game.base), static_cast<unsigned>(s.dispatch - game.base),
        static_cast<unsigned>(s.call - game.base), g_pgOff, g_listOff, kBlock, g_mode.load());
    return true;
}

// ---- Self-test entry points ----

int LumenScanCullTestDiscover(const GameModule& game, uint32_t rva[4], uint32_t* pgOff, uint32_t* viewSpecOff)
{
    ScanSites s{};
    const char* why = "";
    if (!Discover(game, &s, &why)) { printf("    %s\n", why); return 0; }
    rva[0] = static_cast<uint32_t>(s.body - game.base);
    rva[1] = static_cast<uint32_t>(s.thunkJmp - game.base);
    rva[2] = static_cast<uint32_t>(s.dispatch - game.base);
    rva[3] = static_cast<uint32_t>(s.call - game.base);
    *pgOff = s.pgOff;
    *viewSpecOff = s.viewSpecOff;
    return 1;
}

// Sets the state up with the given body (the mapped exe's, in the self-test) and offsets; no patching.
bool LumenScanCullTestInit(void* body, uint32_t pgOff, uint32_t listOff, uint32_t viewSpecOff)
{
    if (!g_sum && !AllocState()) return false;
    memset(g_sum, 0, sizeof(LumenScanBlock) * kMaxBlocks);
    for (int32_t b = 0; b < kMaxBlocks; ++b) g_dirty[b].store(0, std::memory_order_relaxed);
    memset(g_counters, 0, sizeof(Counters) * kSlots);
    g_body = reinterpret_cast<BodyFn>(body);
    g_pgOff = pgOff;
    g_listOff = listOff;
    g_viewSpecOff = viewSpecOff;
    g_key = nullptr;
    g_keyData = nullptr;
    g_keyBits = nullptr;
    g_testLegacyGap = false;
    t_armedByMe = false;
    g_prepSinceReset = true;
    g_keyPublic = nullptr;
    g_lastNum = 0;
    g_needAll = true;
    g_armed = nullptr;
    g_mmCount = 0;
    g_mode = 1;
    g_lastMode = 1;
    g_ready = true;
    return true;
}
void LumenScanCullTestMode(int mode) { if (mode != g_mode.load()) ChangeMode(mode); }
void LumenScanCullTestLegacyGap(bool on) { g_testLegacyGap = on; }
void LumenScanCullTestCounts(uint64_t* scanned, uint64_t* skipped, uint64_t* verified, uint64_t* mismatches)
{
    uint64_t a = 0, b = 0, v = 0, m = 0;
    for (int i = 0; i < kSlots; ++i) { a += g_counters[i].groupsScanned; b += g_counters[i].groupsSkipped; v += g_counters[i].verified; m += g_counters[i].mismatches; }
    *scanned = a; *skipped = b; *verified = v; *mismatches = m;
}
void LumenScanCullTestDone() { g_ready = false; g_mode = 0; }
// The prep stub with test targets, in executable memory near nothing in particular (the caller frees it).
uint8_t* LumenScanCullTestStub(const void* prep, const void* original, const void* post)
{
    uint8_t* const mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!mem) return nullptr;
    const size_t len = BuildPrepStub(mem, prep, original, post);
    memcpy(mem + 256, kPrepUnwind, sizeof kPrepUnwind);
    auto* const table = reinterpret_cast<RUNTIME_FUNCTION*>(mem + 512);
    table[0].BeginAddress = 0;
    table[0].EndAddress = static_cast<DWORD>(len);
    table[0].UnwindData = 256;
    DWORD old;
    VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    RtlAddFunctionTable(table, 1, reinterpret_cast<DWORD64>(mem));
    return mem;
}
void LumenScanCullTestFreeStub(uint8_t* mem)
{
    RtlDeleteFunctionTable(reinterpret_cast<RUNTIME_FUNCTION*>(mem + 512));
    VirtualFree(mem, 0, MEM_RELEASE);
}

}  // namespace opf
