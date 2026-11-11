// BodyRemovalSpread (worker bodyspread, 2026-11-07; review fixes worker spreadfix; public since 1.3.0, worker port130a,
// 2026-11-07). [Fixes] BodyRemovalSpread, default 1: installed at start-up when != 0 and NpcCreationPacing's frame hooks
// are in (it drains at their post-actor tick), then switched live (0 = the game's way). Every site is found by byte
// pattern and checked; any mismatch installs nothing, with a log line. User decision 2026-11-07: equivalent, not
// identical, is accepted here (a level the player has left keeps its far bodies a few frames longer); spread the work,
// never just hold it. Not in the shipped ini: BodyRemovalSpreadPerFrame (500), BodyRemovalSpreadMaxFrames (30; public
// 2..120), BodyRemovalSpreadMin (1), BodyRemovalSpreadNearM (50).
//
// The problem (findings 2026-11-07 "physjump", "play1107b"; 2026-10-22 "physwait"): when World Partition removes a streamed
// level from the world (UWorld::RemoveFromWorld, incremental: Level->IncrementalUnregisterComponents(n) in a loop until
// s.LevelStreamingUnregisterComponentsTimeLimit, World.cpp), each instanced-mesh component's unregistration runs
// UInstancedStaticMeshComponent::OnDestroyPhysicsState -> ClearAllInstanceBodies (for every instance body TermBody(false)
// and the body's deleting destructor, then InstanceBodies.Empty()). One component with thousands of bodies is one unit for
// the time limit, so 1,000-2,500 bodies go per frame for 6-10 frames (7-16k per place on the Bravil road). Each body costs
// the game thread its TermBody (component maps, the game thread's acceleration structure, UnregisterObject) and the
// physics step ~5.5 us (the push's unregister command and the rest of the push 3.85 us, DestroyPendingProxies 1.68 us
// with PhysicsDestroyBatch=3): TG_EndPhysics 17-24 ms in 4 frames.
//
// What it changes (only for components whose bodies are cleared inside the INCREMENTAL level removal, on the game thread,
// with at least BodyRemovalSpreadMin bodies (default 1), at least BodyRemovalSpreadNearM metres from the player (bounds
// box; unknown position or bounds = stock), while the per-frame hooks run (NpcCreationPacing's)):
//  - At the original moment: for every body with a physics actor, FPhysScene_Chaos::RemoveFromComponentMaps(scene, actor)
//    (the call TermBody itself makes first, with the scene TermBody computes: the component's world's PhysicsScene). Then
//    the component's InstanceBodies entries are moved into a queue (the array slots set to null) and the game's own
//    ClearAllInstanceBodies runs on the component: it skips the null slots and empties the array as stock.
//  - Each frame at the post-actor tick (game thread, after TG_EndPhysics; NpcCreationPacing's OnWorldPostActorTick call):
//    the queued bodies are terminated exactly as ClearAllInstanceBodies does it (TermBody(body, false), then the body's
//    deleting destructor; the body's OwnerComponent weak pointer cleared first, see TermQueued), batches whose component
//    was garbage-collected first, then the oldest, at most max(BodyRemovalSpreadPerFrame, ceil(pending / (MaxFrames - age
//    of the oldest batch in FRAMES))) bodies per frame, so no body waits more than BodyRemovalSpreadMaxFrames frames
//    (spread, never held). Nothing is terminated inside the level removal's own time-limited loop (it ran in the
//    async-trace window and used the removal's 1 ms). TermBody's own RemoveFromComponentMaps call then has nothing to do,
//    the rest of TermBody (ReleaseActor: the game thread's acceleration structure, UnregisterObject -> the next push's
//    command and that step's destroy) runs as stock, only in a later frame. A frame without a post-actor tick (world
//    paused: UE 5.3 LevelTick.cpp broadcasts it only with actor ticks) drains its quota at the next frame start, so the
//    bound holds. (Lab mode 64: always at the frame start; ~4x the game-thread cost per body, run r5.)
//    Why the frame start is ~4x dearer (offline, UE 5.3 LevelTick.cpp / WorldCollisionAsync.cpp; not measured): the
//    world's async traces are dispatched by FinishAsyncTrace right after OnWorldPostActorTick and run on workers into the
//    next frame until its ResetAsyncTrace, which comes after our frame-start hook; each query holds the scene's read lock
//    (SceneQuery.cpp FScopedSceneReadLock) and TermBody's acceleration-structure removal needs its write lock.
//  - Removals behind a loading screen (doors, fast travel, loads) stay stock: the frame hook is not "alive" there (no
//    frame start for 500 ms), so nothing is spread where only loading time is spent.
// Measured (findings 2026-11-07 "bodyspread"): at the Bravil road's places ~85 % of the removed bodies are in ~1,500
// small instanced components of ~10 bodies each, inside ONE actor's UnregisterAllComponents per frame (the unit no stock
// cvar splits); the rest in a few components of 300-960.
// Why it is equivalent (UE 5.3 BodyInstance.cpp TermBody, InstancedStaticMesh.cpp ClearAllInstanceBodies, PhysScene_Chaos.cpp,
// ChaosEngineInterface.cpp ReleaseActor, PBDRigidsSolver.cpp UnregisterObject): a queued body stays a complete, consistent
// static body of the physics scene (both acceleration structures, the solver) until it is terminated: physics-thread
// collisions far from the player can still meet it for those frames (that is the accepted difference). World scene
// queries cannot from the next release on (after 1.3.0-beta): it is hidden from them, on game-thread and on physics-thread
// data, from the moment it is queued (see "Query hiding" below). In 1.3.0-beta game-thread queries could meet it, and an
// overlap that met a body whose component had been garbage-collected crashed the game's VPhysicsController code (our
// bug, findings "eastcrash"). Its FBodyInstance
// is owned by the queue (the component no longer points to it). What TermBody needs from the component is only the
// component-map removal (done at the original moment, in stock order) and the collision-event unregistration (components
// with a notifying body are left to stock). So the component may be unregistered, re-registered (new bodies; the queued
// old ones go on their own) or garbage-collected while its bodies wait: TermBody's weak OwnerComponent then reads null and
// skips the maps (already done); TermBody reads nothing else of the component (BodySetup / ExternalCollisionProfileBodySetup
// are only cleared; ISM instance bodies have no DOFConstraint); the shapes' geometry is refcounted by the particle. No
// world query can hit such a body (query hiding; 1.3.0-beta argued "a hit reports Component = null, as any hit's weak
// Component can", which missed consumers that do not check it: the crash). The physics scene: the
// FPhysScene_Chaos deleting destructor's vtable slot (UWorld::FinishDestroy's ReleasePhysicsScene, after
// OnPreWorldFinishDestroy and WaitForAllAsyncTraceTasks: no trace runs, the scene is whole) is wrapped: that scene's queued
// bodies are terminated there first (PhysicsSolverBase.cpp DestroySolver then flushes the pending unregister commands, its
// designed path). As a fallback (slot not found / another scene class) FPhysicsSolverBase::DestroySolver's one call (from
// ~FChaosScene) is wrapped too: queued bodies of that solver are then dropped untouched (their memory leaks; never
// released into a destroyed scene) and logged. Only the deletes' order and frame differ otherwise.
// The weak-pointer clear (TermQueued) is one 8-byte store outside the scene lock; the body is still hidden from queries
// then (shown again only after TermBody has removed it), so no trace sees the cleared owner.
// The near rule reads the component's bounds (USceneComponent::Bounds, UE 5.3's class layout, kBoundsOff); values that
// don't look like a box (not finite, negative or under 1 cm, origin out of the world) count as unknown = stock.
//
// Mode (live): 1 spread, 0 the game's way (installed). The public build reads any value != 0 as 1. Lab bits: 2 log (one
// line per cleared component >= Min inside a level removal: bodies, ms; per-frame drain lines); 4 installed but off; 8
// measure split (stock behaviour: the component maps removed first, timed, then the game's clear, timed; bit-identical to
// stock); 16 fast map order (the maps' removals in the order e0, eN-1, ..., e1: each proxy then sits at the front of the
// component's proxy array, O(N) instead of RemoveSingleSwap's O(N^2/2) scan; same end state, other internal order of the
// proxy->component TMap's free list); 32 frame summary (one line per frame with clears inside a level removal or drains:
// components / bodies cleared, queued, drained, ms, pending, oldest age, the longest single termination and how many took
// > 0.05 ms, the nearest queued component's distance and kind); 64 drain the frame's quota at the frame start instead of
// the post-actor tick (the first design; slower: run r5); 128 one line per queued or near-excluded component: its distance
// from the player, bounds, class, owner and level package (World Partition cell / HLOD / data layer); 256 the 1.3.0-beta
// behaviour (queued bodies NOT hidden from queries: for the crash repro only); 512 proof counts (PreFilter calls on each
// data path; shapes of queued bodies rejected, in queries of every kind, and how many of those bodies had their owner
// component gone already, the first 20 logged; only an overlap among them would have been the 1.3.0-beta crash; the hook
// cannot tell the query kind). Lab only also:
// BodyRemovalSpreadFile (a text file "<mode> [perFrame]" read every second that overrides the two keys), the 5 s stats and
// MaxFrames up to 20000 (S2 tests: bodies still queued at a world change).
// Defaults: BodyRemovalSpreadMin 1 (run r2 found ~85 % of a place's removed bodies in ~1,500 small instanced components of
// ~10 bodies each, the rest in 4-6 of 300-960); BodyRemovalSpreadNearM kNearDefaultM (components whose bounds box comes
// nearer the player are left to stock).
// The log tag is [bodyspread].
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace opf {
namespace {

// UInstancedStaticMeshComponent::ClearAllInstanceBodies: lea rdi,[rcx+InstanceBodies]; ...; cmp [rcx+Num],esi; jle; loop:
// mov rcx,[rbx+rax]; test; je; xor edx,edx; call TermBody; ...
constexpr const char* kClearPro =
    "40 57 48 83 EC 20 48 89 6C 24 38 48 8D B9 ?? ?? ?? ?? 48 89 74 24 40 48 8B E9 33 F6 39 B1 ?? ?? ?? ?? 7E ?? 48 89 5C 24 "
    "30 33 DB 0F 1F 44 00 00 48 8B 07 48 8B 0C 03 48 85 C9 74 ?? 33 D2 E8";
constexpr size_t kClearArr = 14, kClearNum = 30, kClearTerm = 62;
// ISM OnDestroyPhysicsState (exe+0x3257CF0): mov rbx,rcx; call USceneComponent::OnDestroyPhysicsState; mov rcx,rbx;
// call ClearAllInstanceBodies; mov rax,[rbx]; mov rcx,rbx; call [rax+4C0h] (IsNavigationRelevant); shl al,5
constexpr const char* kDestroySite = "48 8B D9 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 90 C0 04 00 00 C0 E0 05";
constexpr size_t kDestroyCall = 11;
// FBodyInstance::TermBody(bool bNeverDeferRelease) (exe+0x34FC870)
constexpr const char* kTermPro =
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 48 8B F9 0F B6 EA 48 81 C1 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D8 48 "
    "85 C0 74 ?? 48 8B 80 ?? ?? ?? ?? 48 85 C0 75 08 48 8B CB E8 ?? ?? ?? ?? 48 8B B0 ?? ?? ?? ?? 48 85 F6 74 ?? 48 8B 97 ?? "
    "?? ?? ?? 48 85 D2 74 08 48 8B CE E8 ?? ?? ?? ?? F6 47 ?? 04 74 0B 48 8B D3 48 8B CE E8 ?? ?? ?? ?? 48 83 BF ?? ?? ?? ?? "
    "00 74 1F 48 8D 8F ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 48 8D 8F ?? ?? ?? ?? 44 0F B6 C5 E8";
constexpr size_t kTermWeak = 27, kTermWeakGet = 31, kTermWorld = 47, kTermGetWorld = 59, kTermScene = 67, kTermHandle = 79,
                 kTermMaps = 91, kTermNotify = 98, kTermCurScene = 130;
// FChaosEngineInterface::GetCurrentScene(const FPhysicsActorHandle&): [handle] -> +Solver -> +PhysSceneHack
constexpr const char* kCurScenePro = "48 8B 01 48 85 C0 74 ?? 48 8B 40 ?? 48 85 C0 74 ?? 48 8B 80 ?? ?? ?? ?? C3";
constexpr size_t kCurSolver = 11, kCurHack = 20;
// The query filter. TermBody's last call (kTermPro's final E8) is FChaosEngineInterface::ReleaseActor(handle,
// scene, neverDefer) (exe+0x1AA3A40): if the handle and the scene are set, FChaosScene::RemoveActorFromAccelerationStructure
// (scene, handle->GetParticle_LowLevel()) = `mov rdx,[rcx+particle]; mov rcx,rdi; call`.
constexpr size_t kTermRelease = 149;
constexpr const char* kReleasePro =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 09 48 85 C9 74 ?? 48 85 D2 74 ?? 48 8B 51 ?? 48 8B CF E8";
constexpr size_t kReleaseParticle = 32, kReleaseRemove = 36;
// RemoveActorFromAccelerationStructure (exe+0x1AA3BF0): if (scene's spatial acceleration && particle->UniqueIdx valid)
constexpr const char* kRemoveAccelPro =
    "48 8B C4 48 89 48 08 55 41 55 48 8B EC 48 83 EC 78 48 83 79 ?? 00 4C 8B EA 0F 84 ?? ?? ?? ?? 83 7A ?? FF";
// FCollisionQueryFilterCallback's constructor (exe+0x2F6B510; the only writer of its vtable): lea rax,[vtable]; ...;
// mov [rcx],rax; mov rbp,rdx; cmp byte [rdx+bIgnoreComponentsSorted?],0
constexpr const char* kFilterCtor =
    "48 89 5C 24 18 48 89 6C 24 20 56 57 41 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 45 0F B6 F8 48 89 01 48 8B EA 80 7A ?? 00 48 8B D9";
constexpr size_t kFilterCtorVt = 18;
// FCollisionQueryFilterCallback::PreFilterBaseImp (exe+0x34F2040; GT and PT overloads folded): Shape.GetQueryEnabled()...
constexpr const char* kPreFilterPro =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 41 F6 40 08 03 49 8B D8 48 8B F2 48 8B F9 75 07 41 0F B6 40 ?? EB 05 41 0F "
    "B6 40 ?? D0 E8 A8 01";
// A scene-query visitor's PreFilter call (SQVisitor.h): if (flags & ePREFILTER) QueryCallback.PreFilter(filter, shape,
// particle or handle) = test byte [reg+10h],4; je; ...; mov r9,...; call [reg+slot*8] (the first `call [reg+disp8]`
// within 48 bytes, after a write of r9: the fourth argument).
// Every such call must go through slot 3 or 4 (both wrapped; another slot of the callback's vtable = not installed), and
// both must be used (this exe: 84 calls through slot 4, 41 through slot 3).
constexpr const char* kVisitTest = "F6 ?? 10 04";
constexpr size_t kVisitWindow = 48;
constexpr size_t kPreFilterSlot = 4;    // vtable index: [0] dtor, [1] [2] PostFilter, [3] PreFilter (PT handle), [4] (GT particle)
constexpr size_t kPreFilterSlotPT = 3;  // (MSVC groups the overloads in reverse order; both are wrapped, so the order is moot)
// FPhysicsSolverBase::bSolverHasFrozenGameThreadCallbacks: the inlined CreateAndRegisterSimCallbackObject_External sets it
// for a callback with RunOnFrozenGameThread: test byte [cb+41h],1; (mov rdi,[rsp+30h];) je +7; mov byte [reg+off],1.
// 5 sites in this exe, all at solver+0x1D0. Optional: only the diagnostic log line depends on it.
constexpr const char* kFrozenSetA = "F6 ?? 41 01 74 07 C6 ?? ?? ?? 00 00 01";
constexpr const char* kFrozenSetB = "F6 ?? 41 01 48 8B ?? 24 ?? 74 07 C6 ?? ?? ?? 00 00 01";
constexpr size_t kFrozenModA = 7, kFrozenModB = 12;  // the mov's ModRM byte; the offset follows it
// UWorld::RemoveFromWorld's incremental loop: mov edx,ebx; mov rcx,r13; call Level->IncrementalUnregisterComponents;
// test al,al; jne; lea rcx,[rsp+28h]; call [QueryPerformanceCounter]
constexpr const char* kUnregSite = "8B D3 49 8B CD E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8D 4C 24 28 FF 15";
constexpr size_t kUnregCall = 5;
// ... and its blocking loop (the same callee with MAX_int32): checked to name the callee
constexpr const char* kUnregBlock = "BA FF FF FF 7F 49 8B CD E8 ?? ?? ?? ?? 84 C0 74";
constexpr size_t kUnregBlockCall = 8;
// FChaosSolversModule::DestroySolver: ...; test r10,r10; je; mov rcx,r10; call FPhysicsSolverBase::DestroySolver; call
constexpr const char* kDsSite = "4C 8B 54 24 38 4D 85 D2 74 08 49 8B CA E8 ?? ?? ?? ?? E8";
constexpr size_t kDsCall = 13;
// FPhysicsSolverBase::DestroySolver(FPhysicsSolverBase&) (exe+0x1886640)
constexpr const char* kDsPro = "40 55 41 57 48 8D 6C 24 B1 48 81 EC 88 00 00 00 4C 89 64 24 70 4C 8B F9 4C 89 6C 24 68 E8";
// A scalar deleting destructor: mov [rsp+8],rbx; push rdi; sub rsp,20h; mov ebx,edx; mov rdi,rcx; call ~T; test bl,1; je;
// mov edx,sizeof(T); mov rcx,rdi; call operator delete. FPhysScene_Chaos' (exe+0x351A8D0, slot 0 of its vtable
// exe+0x775AF00, which is the live scene's vptr: run brv-frozen3) is the one whose ~T (exe+0x3514130) tail-jumps to
// ~FChaosScene (exe+0x1A9BC40), which calls the function holding kDsSite.
constexpr const char* kSceneDelPro =
    "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? F6 C3 01 74 0D BA ?? ?? ?? ?? 48 8B CF E8";
constexpr size_t kSceneDelDtor = 15;
// USceneComponent::Bounds (FBoxSphereBounds, doubles: Origin, BoxExtent, SphereRadius): after AttachParent (+0xC8,
// findings), AttachSocketName, AttachChildren, ClientAttachedChildren, NetOldAttachSocketName, NetOldAttachParent (UE 5.3
// SceneComponent.h order); only read for the near check / log (validated per component: bad values = stock).
constexpr uint32_t kBoundsOff = 0x108;
constexpr int kNearDefaultM = 50;  // run sf-r1: nearest removed component 96 m (bounds box), all World Partition foliage cells

using ClearFn = void (*)(uint8_t* comp);
using TermFn = void (*)(uint8_t* body, bool neverDefer);
using WeakGetFn = uint8_t* (*)(const void* weak);
using GetWorldFn = uint8_t* (*)(uint8_t* comp);
using MapsFn = void (*)(uint8_t* scene, void* handle);
using UnregFn = bool (*)(void* level, int32_t n);
using SolverFn = void (*)(void* solver);
using DelFn = void* (*)(void* obj, uint32_t flags);
using PosFn = bool (*)(double* x, double* y, double* z);

struct Found {
    uint8_t *clear = nullptr, *term = nullptr, *weakGet = nullptr, *getWorld = nullptr, *maps = nullptr, *unreg = nullptr,
            *destroySolver = nullptr, *sceneDel = nullptr;
    uint8_t *destroySite = nullptr, *unregSite = nullptr, *dsSite = nullptr;
    void** sceneSlot = nullptr;  // FPhysScene_Chaos' deleting destructor's vtable slot (nullptr: fallback only)
    uint32_t arr = 0, weak = 0, world = 0, scene = 0, handle = 0, notify = 0, solver = 0, hack = 0;
    // the query filter (required)
    uint8_t *filterCtor = nullptr, *preFilter = nullptr, *preFilterPT = nullptr, *release = nullptr;
    void** filterSlot = nullptr;    // FCollisionQueryFilterCallback's vtable slot 4 (game-thread PreFilter)
    void** filterSlotPT = nullptr;  // ... slot 3 (physics-thread PreFilter)
    uint32_t particle = 0;          // proxy -> game-thread particle (its physics-thread handle: + 8, checked at run time)
    size_t visits4 = 0, visits3 = 0;  // visitor PreFilter calls found through slot 4 / 3
    // the frozen-game-thread flag (optional: 0 = not found)
    uint32_t frozen = 0;
    size_t frozenSites = 0;
};

ClearFn g_clear = nullptr;
TermFn g_term = nullptr;
WeakGetFn g_weakGet = nullptr;
GetWorldFn g_getWorld = nullptr;
MapsFn g_maps = nullptr;
UnregFn g_unreg = nullptr;
SolverFn g_destroySolver = nullptr;
DelFn g_sceneDel = nullptr;
PosFn g_playerPos = &NpcPacingPlayerPos;
uint32_t g_arrOff = 0, g_weakOff = 0, g_worldOff = 0, g_sceneOff = 0, g_handleOff = 0, g_notifyOff = 0, g_solverOff = 0,
         g_hackOff = 0;
void** g_sceneSlot = nullptr;
int g_sceneVtOk = 0;  // 0 not checked, 1 the scene's vtable holds our slot, -1 it does not (fallback: dropped at DestroySolver)
bool g_installed = false;
Found g_found;

std::atomic<int> g_mode{0};
std::atomic<int> g_perFrame{500}, g_maxFrames{30}, g_min{1}, g_nearM{kNearDefaultM};
double g_qpcMs = 0;

thread_local int t_inRemoval = 0;

struct Batch {
    uint8_t* comp = nullptr;
    void* solver = nullptr;
    uint8_t* scene = nullptr;
    std::vector<uint8_t*> bodies;
    size_t next = 0;
    uint64_t frame = 0;  // g_frame when queued
    double box[6] = {};  // the component's bounds box at queue time (min xyz, max xyz; UE units)
    bool hasBox = false;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::deque<Batch> g_q;  // under g_lock
size_t g_pending = 0;   // under g_lock
uint64_t g_frame = 0;   // game thread: frame starts (a batch's age is counted in these)
uint64_t g_drains = 0;  // game thread: drain points
uint64_t g_sincePost = 0;  // game thread: frame starts since the last post-actor tick
uint64_t g_fComps = 0, g_fBodies = 0, g_fQueued = 0, g_fNear = 0;  // game thread: this frame's clears inside level removals
uint64_t g_fDrained = 0, g_fSlow = 0, g_fForced = 0;  // game thread: this frame's terminations (calls > 0.05 ms), dead-first
double g_fDrainMs = 0, g_fMaxCallMs = 0;
double g_fMinDistM = -1;  // game thread: the nearest queued component this frame (m; -1 none)
const uint8_t* g_fNearComp = nullptr;  // ... and that component (named at the next frame start: no GC in between)
std::atomic<DWORD> g_gtId{0};
std::atomic<int64_t> g_lastFrameQpc{0};

struct Stat {
    std::atomic<uint64_t> v{0};
    void add(uint64_t x) { v.fetch_add(x, std::memory_order_relaxed); }
    void max(uint64_t x)
    {
        uint64_t c = v.load(std::memory_order_relaxed);
        while (x > c && !v.compare_exchange_weak(c, x, std::memory_order_relaxed)) {}
    }
    uint64_t take() { return v.exchange(0, std::memory_order_relaxed); }
};
struct MinStat {  // a minimum in decimetres; UINT64_MAX = none
    std::atomic<uint64_t> v{UINT64_MAX};
    void min(double m)
    {
        const uint64_t x = static_cast<uint64_t>(std::max(0.0, m) * 10.0);
        uint64_t c = v.load(std::memory_order_relaxed);
        while (x < c && !v.compare_exchange_weak(c, x, std::memory_order_relaxed)) {}
    }
    double takeM() { const uint64_t x = v.exchange(UINT64_MAX, std::memory_order_relaxed); return x == UINT64_MAX ? -1.0 : x / 10.0; }
};
struct Stats {
    Stat comps, bodies, stockComps, stockBodies, ineligible, drained, drainFrames, drainUs, maxDrainUs, maxFrameN, mapsUs,
        maxMapsUs, forced, dropped, maxPending, maxAge, clearUs, maxClearUs, measured, smallComps, smallBodies, outComps,
        outBodies, nearComps, nearBodies, noPos, sceneTerm, startDrains, hidden, hideFull, hiddenGone, preCalls, maxMarked,
        hiddenPT, preCallsPT, hideNoKey, tabSwitch, tabClear, flagChanges;
    MinStat minQueueM, minDrainM;
};
Stats s;
double g_minEverM = -1;  // game thread: the nearest queued component since start

double NowMs()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * g_qpcMs;
}
int64_t NowQpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// ---- S3: where the removed components are ----
// The component's bounds box (UE units) if its bounds look valid.
bool BoxOf(const uint8_t* comp, double box[6])
{
    double v[7];
    __try {
        memcpy(v, comp + kBoundsOff, sizeof v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (double d : v)
        if (!std::isfinite(d)) return false;
    const double ext = std::sqrt(v[3] * v[3] + v[4] * v[4] + v[5] * v[5]);
    // under 1 cm: not a real component's box (also what pointers or small integers read as doubles look like)
    if (v[3] < 0 || v[4] < 0 || v[5] < 0 || ext < 1.0 || ext > 1e7 || v[6] < 1.0 || v[6] > ext * 1.01 + 1 || std::fabs(v[0]) > 1e8 ||
        std::fabs(v[1]) > 1e8 || std::fabs(v[2]) > 1e8)
        return false;
    for (int i = 0; i < 3; ++i) {
        box[i] = v[i] - v[i + 3];
        box[i + 3] = v[i] + v[i + 3];
    }
    return true;
}
// Distance (m) from the player to the box, -1 if the player's position is unknown.
double DistM(const double box[6])
{
    double p[3];
    if (!g_playerPos || !g_playerPos(&p[0], &p[1], &p[2])) return -1;
    double d2 = 0;
    for (int i = 0; i < 3; ++i) {
        const double d = p[i] < box[i] ? box[i] - p[i] : (p[i] > box[i + 3] ? p[i] - box[i + 3] : 0);
        d2 += d * d;
    }
    return std::sqrt(d2) / 100.0;
}

// ---- Query hiding (fix for OUR crash in 1.3.0-beta, exe+0x487CBE1; design worker bodyfix 2026-11-10, wired by worker
// bodyfix2 2026-11-11, physics-thread data path and two-table set worker bodyfix3 2026-11-11; ships in the next release) ----
// A queued body stays in the physics scene's query structures until its deferred TermBody, while its owner component
// may be garbage-collected: an overlap then returns an element with a null component (UE 5.3 CollisionConversions.cpp
// ConvertQueryOverlap / ConvertOverlapResultsImp add it anyway) and the game's VPhysicsController overlap tracker
// dereferences it. Stock never meets such a body (TermBody removes it before its component can die).
// What is covered: every world query built by UE 5.3 SceneQuery.cpp (UWorld line traces, sweeps and overlaps, sync and
// async; in this exe the callback's one constructor is called only from there, and no other query-callback class is
// found: no RTTI name, no visitor built elsewhere) asks the callback's
// PreFilter about each candidate shape (SQVisitor.h; SceneQuery.cpp always sets the pre-filter flag), on either data
// path: game-thread data (the normal case; visitor payload +0, the game-thread particle, vtable slot 4) or physics-thread
// data (SceneQuery.cpp GetThreadQueryContext: only for a query made on the game thread while it is frozen inside the
// physics step, i.e. from a frozen-game-thread sim callback such as the engine's async physics tick; payload +8, the
// physics-thread handle, slot 3; its results are converted back to game objects, CollisionConversions.cpp GetGTActor).
// Both slots are wrapped with the same check, and every queued body is in the hidden set (g_tab) under both keys (its game-thread
// particle and its physics-thread handle) from the moment it is queued until TermBody has run: PreFilter answers None,
// so no hit is formed, a ray is not shortened, the real body behind it is found, and the query returns what stock
// returns once the body is gone. The overload order (which slot gets which key) does not matter for this.
// Not covered (the accepted difference, unchanged): the physics simulation's own collision detection can still meet a
// queued (static, far) body until its TermBody; notifying bodies are never queued, so no collision event reaches game code.
// What the exe shows: the callback class has one out-of-line constructor (the only reference to its vtable); vtable
// slots [1]=[2] PostFilter, [3]=[4] PreFilter (GT and PT bodies are one folded function); the 125 visitor PreFilter
// calls go through [vtable+0x20] (84, payload +0) or [vtable+0x18] (41, payload +8), a real virtual call.
// Keys: the game-thread particle is the one FChaosScene::RemoveActorFromAccelerationStructure removes (proxy +
// g_particleOff, read from ReleaseActor's code); the physics-thread handle is FSingleParticlePhysicsProxy::Handle, read
// at the offset the proxy's own GetHandleUnsafe() uses (IPhysicsProxyBase's first virtual, `mov rax,[rcx+off]; ret`,
// checked once per proxy vtable; it must be the member right after the particle, as in UE 5.3). A body whose handle is
// still null (registered on the physics thread this frame) or whose proxy class does not check out sends its component
// to stock: a run-time, per-component fallback, not part of the all-or-nothing install below.
// All or nothing (at start-up): the spread is not installed without the filter (that spread was the 1.3.0-beta bug).
// The physics-thread path (slot 3) has never run in the game (every solver's frozen flag 0 so far): the self-test checks
// its logic on a fake layout only; the handle's place rests on UE 5.3 and on GetHandleUnsafe's bytes read at run time.
// The set: two open-addressing tables (linear probing). Slot values: 0 empty (ends a probe), 1 tombstone, else a key.
// Writers (queue, drain, scene delete, solver drop) are serialized by g_lock; readers (any query thread) take no lock.
// Readers may see an insert or erase a moment early or late: the same race a query in flight has with stock's removal
// under the scene lock (a query that starts while a component is being queued may still see its bodies, whose component
// is alive then). Entries never move; a table is only cleared when it holds no live key (a reader probing it meanwhile
// can miss nothing). Inserts go to one table; once it has kSwitchAt used slots (live + tombstones) they go to the other
// table if that one holds no live key (it is cleared first), so a continuous unload stream never fills a table with
// tombstones: the old table empties as its bodies are terminated (within BodyRemovalSpreadMaxFrames frames) and is
// cleared when the inserts come back to it (the insert table also when it empties with kTombClear used). A table is full
// at g_hiddenLimit used slots (load 50 %): the component then goes to stock (counted, logged in the stats line).
// HiddenTable::body (lab, same index) holds each key's body, read only by the lab's proof counter (bit 512).
// Capacity: each table takes inserts up to g_hiddenLimit = 65,536 used slots (half of kHiddenCap), so at most 2 x 65,536
// keys at once (two keys per body); beyond that (the insert table full while the other still holds live keys) components
// go to stock. Seen at most: 30,586 keys (bf3-lab1); a worst case of 2.5k bodies a frame for 30 frames (150k keys) would
// pass it, and the rest of that stream would go to stock. Cost: readers ~1-4 ns per PreFilter call; writers insert /
// erase under g_lock, and a ClearTable (131,072 stores) takes 29-50 us (bodyrev3), at a table switch or when the insert
// table empties with kTombClear used.
constexpr size_t kHiddenCap = size_t(1) << 17;  // slots per table: 1 MB of keys
constexpr size_t kTombClear = 4096;             // the insert table is cleared when it empties with at least this many used
struct HiddenTable {
    std::atomic<uintptr_t> key[kHiddenCap];
    size_t used = 0;  // live + tombstones (writers)
};
HiddenTable g_tab[2];
std::atomic<size_t> g_marked[2];  // live keys per table (together: one cache line for the readers' first check)
int g_ins = 0;                    // the table inserts go to (writers)
size_t g_switchAt = kHiddenCap / 4;     // inserts move to the other table at this many used (lab tests lower it)
size_t g_hiddenLimit = kHiddenCap / 2;  // a table takes no insert beyond this many used (lab tests lower it)
bool g_filterOn = false;          // both PreFilter wrappers are installed and g_particleOff is known
uint32_t g_particleOff = 0;       // FSingleParticlePhysicsProxy -> its game-thread particle (TUniquePtr)
uint32_t g_ptHandleOff = 0;       // ... -> its physics-thread handle (checked per proxy vtable)
constexpr int kProxyVts = 4;
const void* g_proxyVtOk[kProxyVts] = {};   // proxy vtables whose GetHandleUnsafe was checked (writers)
const void* g_proxyVtBad[kProxyVts] = {};  // ... and ones that did not check out (logged once each)
using PreFilterFn = uint8_t (*)(void* self, const void* filter, const void* shape, const void* particle);
PreFilterFn g_origPreFilter = nullptr;    // slot 4 (game-thread particle)
PreFilterFn g_origPreFilterPT = nullptr;  // slot 3 (physics-thread handle)
std::atomic<int> g_proofLogged{0};  // lab bit 512: rejections logged so far (first 20)
constexpr size_t kNotHidden = 2 * kHiddenCap;

size_t HashSlot(uintptr_t p) { return static_cast<size_t>((p >> 4) * 0x9E3779B97F4A7C15ull >> 47) & (kHiddenCap - 1); }

size_t FindIn(int t, uintptr_t p)
{
    for (size_t i = HashSlot(p), n = 0; n < kHiddenCap; i = (i + 1) & (kHiddenCap - 1), ++n) {
        const uintptr_t v = g_tab[t].key[i].load(std::memory_order_acquire);
        if (v == p) return i;
        if (v == 0) return kHiddenCap;
    }
    return kHiddenCap;
}
// t * kHiddenCap + slot of this key, or kNotHidden.
size_t FindHidden(const void* key)
{
    const uintptr_t p = reinterpret_cast<uintptr_t>(key);
    for (int t = 0; t < 2; ++t)
        if (g_marked[t].load(std::memory_order_acquire)) {
            const size_t i = FindIn(t, p);
            if (i != kHiddenCap) return static_cast<size_t>(t) * kHiddenCap + i;
        }
    return kNotHidden;
}
bool Hidden(const void* key) { return FindHidden(key) != kNotHidden; }
// The table that can take n more keys (switching to the other one when this one has kSwitchAt used and the other is
// empty), or -1. Writers.
// A table without live keys loses its tombstones (a reader probing it meanwhile can miss nothing). Writers.
void ClearTable(int t)
{
    for (auto& k : g_tab[t].key) k.store(0, std::memory_order_relaxed);
    g_tab[t].used = 0;
    s.tabClear.add(1);
}
int TableFor(size_t n)
{
    int t = g_ins;
    if (g_tab[t].used + n > g_switchAt && g_marked[1 - t].load(std::memory_order_relaxed) == 0) {
        g_ins = t = 1 - t;  // the other table is empty: cleared, then it takes the inserts
        if (g_tab[t].used) ClearTable(t);
        s.tabSwitch.add(1);
    }
    return g_tab[t].used + n <= g_hiddenLimit ? t : -1;
}
void Insert(int t, uintptr_t p, uint8_t* body)  // writers; the table has room (TableFor)
{
    HiddenTable& h = g_tab[t];
    for (size_t i = HashSlot(p);; i = (i + 1) & (kHiddenCap - 1)) {
        const uintptr_t v = h.key[i].load(std::memory_order_relaxed);
        if (v > 1) continue;
        if (v == 0) ++h.used;
        (void)body;
        h.key[i].store(p, std::memory_order_release);
        const size_t m = g_marked[t].fetch_add(1, std::memory_order_release) + 1;
        s.maxMarked.max(m + g_marked[1 - t].load(std::memory_order_relaxed));
        return;
    }
}
void Unhide(const void* key)  // writers
{
    const uintptr_t p = reinterpret_cast<uintptr_t>(key);
    if (p <= 1) return;
    const size_t at = FindHidden(key);
    if (at == kNotHidden) return;
    const int t = static_cast<int>(at / kHiddenCap);
    HiddenTable& h = g_tab[t];
    h.key[at % kHiddenCap].store(1, std::memory_order_release);
    // the insert table empty with many tombstones: cleared now (the other table is cleared when inserts move to it)
    if (g_marked[t].fetch_sub(1, std::memory_order_acq_rel) == 1 && t == g_ins && h.used >= kTombClear) ClearTable(t);
}
// The proxy's vtable and the bytes of its slot 0 (GetHandleUnsafe), or false if unreadable.
bool ProxyVt(const uint8_t* proxy, const void** vt, uint8_t code[5])
{
    __try {
        *vt = *reinterpret_cast<const void* const*>(proxy);
        memcpy(code, *static_cast<const uint8_t* const*>(*vt), 5);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Whether this proxy's class keeps its physics-thread handle at g_ptHandleOff (its GetHandleUnsafe is `mov rax,
// [rcx+g_particleOff+8]; ret`, UE 5.3 SingleParticlePhysicsProxy.h). Writers (cache).
bool ProxyClassOk(const uint8_t* proxy)
{
    const void* const vt = *reinterpret_cast<const void* const*>(proxy);
    for (const void* k : g_proxyVtOk)
        if (k == vt) return true;
    for (const void* k : g_proxyVtBad)
        if (k == vt) return false;
    const void* v = nullptr;
    uint8_t c[5] = {};
    const bool ok = ProxyVt(proxy, &v, c) && c[0] == 0x48 && c[1] == 0x8B && c[2] == 0x41 && c[3] == g_ptHandleOff &&
                    c[4] == 0xC3;
    const void** const list = ok ? g_proxyVtOk : g_proxyVtBad;
    for (int i = 0; i < kProxyVts; ++i)
        if (!list[i]) {
            list[i] = vt;
            const auto rva = [](const void* p) { return static_cast<long long>(static_cast<const uint8_t*>(p) - Game().base); };
            Log("[bodyspread] physics proxy vtable exe+0x%llX: GetHandleUnsafe bytes %02X %02X %02X %02X %02X: %s", rva(vt),
                c[0], c[1], c[2], c[3], c[4],
                ok ? "physics-thread handle read there" : "does not match: its components' removals stay stock");
            break;
        }
    return ok;
}
// The body's two query keys; false if it has a physics actor and a key is unknown.
bool KeysOf(const uint8_t* body, const void** gt, const void** pt)
{
    *gt = *pt = nullptr;
    const uint8_t* const proxy = *reinterpret_cast<const uint8_t* const*>(body + g_handleOff);
    if (!proxy) return true;
    if (!ProxyClassOk(proxy)) return false;
    *gt = *reinterpret_cast<const void* const*>(proxy + g_particleOff);
    *pt = *reinterpret_cast<const void* const*>(proxy + g_ptHandleOff);  // set once on the physics thread at registration
    return *gt && *pt;
}
// The keys a queued body was hidden under (read before its TermBody, which clears the body's actor; the proxy's two
// fields do not change between registration and the physics thread's removal, which comes after TermBody).
void KeysForTerm(const uint8_t* body, const void** gt, const void** pt)
{
    *gt = *pt = nullptr;
    const uint8_t* const proxy = *reinterpret_cast<const uint8_t* const*>(body + g_handleOff);
    if (!proxy) return;
    *gt = *reinterpret_cast<const void* const*>(proxy + g_particleOff);
    if (g_ptHandleOff) *pt = *reinterpret_cast<const void* const*>(proxy + g_ptHandleOff);
}
// Both keys shown again. Writers.
void UnhideBody(const void* gt, const void* pt)
{
    if (gt) Unhide(gt);
    if (pt) Unhide(pt);
}
// Hides every body of a component about to be queued (both keys); false (nothing hidden) if one cannot be: the component
// then goes the game's way. Caller holds g_lock.
bool HideAll(uint8_t** data, int32_t num)
{
    std::vector<const void*> keys;
    keys.reserve(static_cast<size_t>(num) * 2);
    std::vector<uint8_t*> owners;
    owners.reserve(static_cast<size_t>(num) * 2);
    for (int32_t i = 0; i < num; ++i) {
        if (!data[i]) continue;
        const void *gt, *pt;
        if (!KeysOf(data[i], &gt, &pt)) { s.hideNoKey.add(1); return false; }
        if (!gt) continue;  // no physics actor: nothing to hide
        if (Hidden(gt) || Hidden(pt)) { s.hideNoKey.add(1); return false; }  // never expected (a key queued twice)
        keys.push_back(gt);
        keys.push_back(pt);
        owners.push_back(data[i]);
        owners.push_back(data[i]);
    }
    const int t = TableFor(keys.size());
    if (t < 0) { s.hideFull.add(1); return false; }
    for (size_t i = 0; i < keys.size(); ++i) Insert(t, reinterpret_cast<uintptr_t>(keys[i]), owners[i]);
    return true;
}
// The wrapped PreFilters (ECollisionQueryHitType: None 0, Touch 1, Block 2). Nothing hidden: two loads (one cache line)
// and the game's call.
uint8_t PreFilterHook(void* self, const void* filter, const void* shape, const void* particle)  // slot 4
{
    if (g_marked[0].load(std::memory_order_acquire) | g_marked[1].load(std::memory_order_acquire)) {
        const size_t at = FindHidden(particle);
        if (at != kNotHidden) {
            s.hidden.add(1);
            return 0;
        }
    }
    return g_origPreFilter(self, filter, shape, particle);
}
uint8_t PreFilterHookPT(void* self, const void* filter, const void* shape, const void* handle)  // slot 3
{
    if (g_marked[0].load(std::memory_order_acquire) | g_marked[1].load(std::memory_order_acquire)) {
        const size_t at = FindHidden(handle);
        if (at != kNotHidden) {
            s.hiddenPT.add(1);
            return 0;
        }
    }
    return g_origPreFilterPT(self, filter, shape, handle);
}

// ---- The frozen-game-thread flag (diagnostics for the physics-thread data path; bodyfix3) ----
// FPhysicsSolverBase::bSolverHasFrozenGameThreadCallbacks (solver + g_frozenOff; set, never cleared, when a sim
// callback that runs on the frozen game thread is registered: in this exe only the engine's async physics tick). While
// it is 0 no world query takes the physics-thread data path. Logged the first time the fix meets a solver (its first
// instanced-mesh clear) and whenever it changes (checked at each frame start); nothing depends on it.
uint32_t g_frozenOff = 0;  // 0: not found (nothing logged)
struct Watched { void* solver; int flag; };
constexpr int kWatch = 8;
Watched g_watch[kWatch] = {};  // under g_lock
void* g_lastSolver = nullptr;  // game thread: the last solver checked by WatchSolver
int ReadFlag(const void* solver)
{
    __try {
        return *(static_cast<const volatile uint8_t*>(solver) + g_frozenOff);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
void WatchSolver(void* solver)  // game thread
{
    if (!g_frozenOff || !solver || solver == g_lastSolver) return;
    g_lastSolver = solver;
    AcquireSRWLockExclusive(&g_lock);
    int free = -1;
    bool known = false;
    for (int i = 0; i < kWatch && !known; ++i) {
        if (g_watch[i].solver == solver) known = true;
        else if (!g_watch[i].solver && free < 0) free = i;
    }
    int flag = -2;
    if (!known) {
        flag = ReadFlag(solver);
        if (free < 0) {  // full: the oldest entry makes room
            memmove(&g_watch[0], &g_watch[1], sizeof(Watched) * (kWatch - 1));
            free = kWatch - 1;
        }
        g_watch[free] = {solver, flag};
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (flag != -2)
        Log("[bodyspread] physics solver %p: frozen-game-thread callbacks flag (solver+0x%X) = %d (%s)", solver, g_frozenOff,
            flag, flag == 0 ? "no async physics tick: world queries use game-thread data only"
                            : "async physics tick registered: queries from it use physics-thread data, queued bodies are hidden there too");
}
void CheckWatched()  // game thread, frame start
{
    if (!g_frozenOff) return;
    AcquireSRWLockExclusive(&g_lock);
    for (Watched& w : g_watch) {
        if (!w.solver) continue;
        const int f = ReadFlag(w.solver);
        if (f == w.flag) continue;
        Log("[bodyspread] physics solver %p: frozen-game-thread callbacks flag changed %d -> %d, frame %llu", w.solver, w.flag,
            f, static_cast<unsigned long long>(g_frame));
        s.flagChanges.add(1);
        w.flag = f;
    }
    ReleaseSRWLockExclusive(&g_lock);
}
void UnwatchSolverLocked(void* solver)  // caller holds g_lock
{
    for (Watched& w : g_watch)
        if (w.solver == solver) w = {};
    if (g_lastSolver == solver) g_lastSolver = nullptr;
}

// A queued body terminated as ClearAllInstanceBodies' loop does (TermBody(false), then its deleting destructor), with its
// OwnerComponent weak pointer cleared first. TermBody's component part is only the component-map removal (done when the
// body was queued, in stock order) and the collision-event unregistration (notifying components are never queued); and it
// reads the component's world with no null check (exe+0x34FC8B0: mov rsi,[rax+228h]), which a component of a level that
// has left the world no longer has (crash in run r1, 2026-11-07). So the weak pointer is set to null (index 0, serial 0:
// what the game's Get treats as null and what TermBody itself stores at its end; one aligned 8-byte store) and TermBody
// goes straight to ReleaseActor, which finds the scene through the actor's own solver.
// With the query filter on, the body's two keys are shown again right after TermBody (taken before: ReleaseActor clears the
// body's handle) and before the body is deleted (the addresses can be reused only after that). TermBody has then removed
// the particle from the game thread's structure and queued its physics-thread removal for the next push, which the
// physics step processes before that step's frozen-game-thread callbacks run: the body is in exactly the state stock's
// TermBody leaves it in. Caller holds g_lock. A body queued unhidden (lab bit 256) finds nothing to show (one probe each).
void TermQueued(uint8_t* body)
{
    const void *gt = nullptr, *pt = nullptr;
    if (g_filterOn) KeysForTerm(body, &gt, &pt);
    *reinterpret_cast<volatile uint64_t*>(body + g_weakOff) = 0;
    g_term(body, false);
    UnhideBody(gt, pt);
    (*reinterpret_cast<DelFn* const*>(body))[0](body, 1);
}

// A fully drained batch: the distance at that moment (S3), from the box kept at queue time.
void BatchDone(const Batch& b)
{
    if (!b.hasBox) return;
    const double d = DistM(b.box);
    if (d >= 0) s.minDrainM.min(d);
}

// Terminates up to n queued bodies, front first. Caller holds g_lock.
size_t DrainLocked(size_t n)
{
    size_t done = 0;
    int64_t slow = 0, maxCall = 0;
    const int64_t t0 = NowQpc();
    int64_t last = t0;
    while (done < n && !g_q.empty()) {
        Batch& b = g_q.front();
        while (done < n && b.next < b.bodies.size()) {
            TermQueued(b.bodies[b.next++]);
            ++done;
            const int64_t now = NowQpc();
            maxCall = std::max(maxCall, now - last);
            if ((now - last) * g_qpcMs > 0.05) ++slow;
            last = now;
        }
        if (b.next >= b.bodies.size()) {
            BatchDone(b);
            g_q.pop_front();
        }
    }
    g_pending -= done;
    g_fDrained += done;
    g_fDrainMs += (last - t0) * g_qpcMs;
    g_fMaxCallMs = std::max(g_fMaxCallMs, maxCall * g_qpcMs);
    g_fSlow += static_cast<uint64_t>(slow);
    return done;
}

// The scene TermBody would use for this component (null if none).
uint8_t* SceneOf(uint8_t* comp)
{
    uint8_t* w = *reinterpret_cast<uint8_t**>(comp + g_worldOff);
    if (!w) w = g_getWorld(comp);
    return w ? *reinterpret_cast<uint8_t**>(w + g_sceneOff) : nullptr;
}

// Decides whether this component's bodies may be queued; on yes returns the scene and the first actor's solver.
bool Eligible(uint8_t* comp, uint8_t** data, int32_t num, uint8_t** scene, void** solver)
{
    *scene = SceneOf(comp);
    if (!*scene) return false;
    void* first = nullptr;
    for (int32_t i = 0; i < num; ++i) {
        uint8_t* const b = data[i];
        if (!b) continue;
        if (b[g_notifyOff] & 4) return false;                       // collision notifications: stock
        if (g_weakGet(b + g_weakOff) != comp) return false;         // not (or no longer) this component's body
        void* const h = *reinterpret_cast<void**>(b + g_handleOff);
        if (!h) continue;
        void* const sv = *reinterpret_cast<void**>(static_cast<uint8_t*>(h) + g_solverOff);
        if (!sv) return false;
        if (!first) first = sv;
        else if (sv != first) return false;
    }
    if (!first) return false;
    if (*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(first) + g_hackOff) != *scene) return false;
    *solver = first;
    return true;
}

// The first queued scene: its vtable must hold the deleting-destructor slot we wrapped (else the fallback drop applies).
void CheckSceneVtable(const uint8_t* scene)
{
    if (g_sceneVtOk != 0) return;
    void** const vt = *reinterpret_cast<void** const*>(scene);
    g_sceneVtOk = (g_sceneSlot && g_sceneSlot >= vt && g_sceneSlot < vt + 64) ? 1 : -1;
    if (g_sceneVtOk < 0)
        Log("[bodyspread] the physics scene's vtable %p does not hold the wrapped destructor slot %p: a dying scene's queued "
            "bodies would be dropped (fallback)", static_cast<void*>(vt), static_cast<void*>(g_sceneSlot));
}

void RemoveMaps(uint8_t* scene, uint8_t** data, int32_t num, bool fast)
{
    if (!fast) {
        for (int32_t i = 0; i < num; ++i)
            if (data[i])
                if (void* h = *reinterpret_cast<void**>(data[i] + g_handleOff)) g_maps(scene, h);
        return;
    }
    std::vector<void*> hs;
    hs.reserve(static_cast<size_t>(num));
    for (int32_t i = 0; i < num; ++i)
        if (data[i])
            if (void* h = *reinterpret_cast<void**>(data[i] + g_handleOff)) hs.push_back(h);
    if (hs.empty()) return;
    g_maps(scene, hs[0]);
    for (size_t i = hs.size() - 1; i >= 1; --i) g_maps(scene, hs[i]);
}

void ClearHook(uint8_t* comp)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    const int32_t num = *reinterpret_cast<int32_t*>(comp + g_arrOff + 8);
    uint8_t** const data = *reinterpret_cast<uint8_t***>(comp + g_arrOff);
    if (t_inRemoval > 0) {
        ++g_fComps;
        g_fBodies += static_cast<uint64_t>(std::max(num, 0));
    }
    if (g_frozenOff && GetCurrentThreadId() == g_gtId.load(std::memory_order_relaxed))  // the flag's log line (diagnostics)
        for (int32_t i = 0; i < num; ++i)
            if (data[i])
                if (const uint8_t* const h = *reinterpret_cast<uint8_t* const*>(data[i] + g_handleOff)) {
                    WatchSolver(*reinterpret_cast<void* const*>(h + g_solverOff));
                    break;
                }
    const bool big = t_inRemoval > 0 && num >= g_min.load(std::memory_order_relaxed);
    if (!big || (mode & 4) || !(mode & (1 | 2 | 8))) {
        if (mode & (2 | 8)) {  // counted for the picture: small clears in a removal, big ones outside it
            if (t_inRemoval > 0) { s.smallComps.add(1); s.smallBodies.add(static_cast<uint64_t>(std::max(num, 0))); }
            else if (num >= g_min.load(std::memory_order_relaxed)) {
                s.outComps.add(1);
                s.outBodies.add(static_cast<uint64_t>(num));
                if (mode & 2)
                    Log("[bodyspread] outside comp %p %d bodies (not in an incremental level removal), frame %llu", comp, num,
                        static_cast<unsigned long long>(g_frame));
            }
        }
        g_clear(comp);
        return;
    }
    const bool alive = GetCurrentThreadId() == g_gtId.load(std::memory_order_relaxed) &&
                       (NowQpc() - g_lastFrameQpc.load(std::memory_order_relaxed)) * g_qpcMs < 500.0;
    uint8_t* scene = nullptr;
    void* solver = nullptr;
    bool eligible = (mode & 1) && alive && Eligible(comp, data, num, &scene, &solver);
    double box[6] = {};
    bool hasBox = false;
    double dist = -1;
    if (eligible) {
        // S3: only components at least BodyRemovalSpreadNearM from the player (bounds box); unknown = stock
        hasBox = BoxOf(comp, box);
        dist = hasBox ? DistM(box) : -1;
        const bool isNear = dist < 0 || dist < static_cast<double>(g_nearM.load(std::memory_order_relaxed));
        if (dist < 0) s.noPos.add(1);
        else if (isNear) { s.nearComps.add(1); s.nearBodies.add(static_cast<uint64_t>(num)); ++g_fNear; }
        bool hid = true;
        if (!isNear && g_filterOn && !(mode & 256)) {  // hidden from queries first, before anything else changes
            AcquireSRWLockExclusive(&g_lock);
            hid = HideAll(data, num);  // counts why not (set full / a key unknown)
            ReleaseSRWLockExclusive(&g_lock);
        }
        if (isNear || !hid) eligible = false;
        else if (g_fMinDistM < 0 || dist < g_fMinDistM) {
            g_fMinDistM = dist;
            g_fNearComp = comp;
        }
    }
    if (eligible) {
        CheckSceneVtable(scene);
        const double t0 = NowMs();
        RemoveMaps(scene, data, num, (mode & 16) != 0);
        const double t1 = NowMs();
        Batch b;
        b.comp = comp;
        b.solver = solver;
        b.scene = scene;
        b.frame = g_frame;
        b.hasBox = hasBox;
        memcpy(b.box, box, sizeof box);
        b.bodies.reserve(static_cast<size_t>(num));
        for (int32_t i = 0; i < num; ++i)
            if (data[i]) {
                b.bodies.push_back(data[i]);
                data[i] = nullptr;
            }
        const size_t n = b.bodies.size();
        g_clear(comp);  // the null slots are skipped; the array is emptied as stock
        const double t2 = NowMs();
        size_t pending;
        AcquireSRWLockExclusive(&g_lock);
        if (n) g_q.push_back(std::move(b));
        g_pending += n;
        pending = g_pending;
        ReleaseSRWLockExclusive(&g_lock);
        s.comps.add(1);
        s.bodies.add(n);
        s.minQueueM.min(dist);
        if (g_minEverM < 0 || dist < g_minEverM) g_minEverM = dist;
        g_fQueued += n;
        s.mapsUs.add(static_cast<uint64_t>((t1 - t0) * 1000));
        s.maxMapsUs.max(static_cast<uint64_t>((t1 - t0) * 1000));
        s.maxPending.max(pending);
        if (mode & 2)
            Log("[bodyspread] queued comp %p %zu bodies (maps %.2f ms, clear %.2f ms), %.0f m away, pending %zu, frame %llu",
                comp, n, t1 - t0, t2 - t1, dist, pending, static_cast<unsigned long long>(g_frame));
        return;
    }
    // stock (measured when asked)
    if ((mode & 1) && alive) s.ineligible.add(1);
    double t0 = NowMs(), t1 = t0;
    if ((mode & 8) && GetCurrentThreadId() == g_gtId.load(std::memory_order_relaxed)) {
        if (uint8_t* sc = SceneOf(comp)) {
            bool notify = false;
            for (int32_t i = 0; i < num && !notify; ++i) notify = data[i] && (data[i][g_notifyOff] & 4);
            if (!notify) RemoveMaps(sc, data, num, false);  // the same calls TermBody makes first, in its order
        }
        t1 = NowMs();
    }
    g_clear(comp);
    const double t2 = NowMs();
    s.stockComps.add(1);
    s.stockBodies.add(static_cast<uint64_t>(num));
    s.measured.add(1);
    s.clearUs.add(static_cast<uint64_t>((t2 - t0) * 1000));
    s.maxClearUs.max(static_cast<uint64_t>((t2 - t0) * 1000));
    if (mode & 2)
        Log("[bodyspread] stock comp %p %d bodies: maps %.2f ms, clear %.2f ms, frame %llu", comp, num, t1 - t0, t2 - t1,
            static_cast<unsigned long long>(g_frame));
}

bool UnregHook(void* level, int32_t n)
{
    ++t_inRemoval;
    const bool r = g_unreg(level, n);
    --t_inRemoval;
    return r;
}

// Fallback only: a solver destroyed with queued bodies (its scene's deleting destructor was not wrapped or not reached).
void DestroySolverHook(void* solver)
{
    size_t dropped = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (auto it = g_q.begin(); it != g_q.end();) {
        if (it->solver == solver) {
            dropped += it->bodies.size() - it->next;
            if (g_filterOn)  // the scene is going: no query can run on it; the addresses must not stay hidden
                for (size_t i = it->next; i < it->bodies.size(); ++i) {
                    const void *gt, *pt;
                    KeysForTerm(it->bodies[i], &gt, &pt);
                    UnhideBody(gt, pt);
                }
            it = g_q.erase(it);
        } else {
            ++it;
        }
    }
    g_pending -= dropped;
    UnwatchSolverLocked(solver);
    ReleaseSRWLockExclusive(&g_lock);
    if (dropped) {
        s.dropped.add(dropped);
        Log("[bodyspread] solver %p destroyed with %zu queued bodies: dropped (fallback; not released into a dying scene), "
            "thread %s", solver, dropped, GetCurrentThreadId() == g_gtId.load(std::memory_order_relaxed) ? "game" : "other");
    }
    g_destroySolver(solver);
}

// FPhysScene_Chaos' deleting destructor (UWorld::FinishDestroy -> ReleasePhysicsScene; the scene is still whole): its
// queued bodies are terminated first, as stock would have done before.
void* SceneDeleteHook(void* scene, uint32_t flags)
{
    size_t n = 0;
    double ms = 0;
    AcquireSRWLockExclusive(&g_lock);
    const double t0 = NowMs();
    for (auto it = g_q.begin(); it != g_q.end();) {
        if (it->scene == scene) {
            while (it->next < it->bodies.size()) {
                TermQueued(it->bodies[it->next++]);
                ++n;
            }
            it = g_q.erase(it);
        } else {
            ++it;
        }
    }
    g_pending -= n;
    ms = NowMs() - t0;
    ReleaseSRWLockExclusive(&g_lock);
    if (n) {
        s.sceneTerm.add(n);
        Log("[bodyspread] physics scene %p deleted with %zu queued bodies: terminated first (%.2f ms, thread %s)", scene, n, ms,
            GetCurrentThreadId() == g_gtId.load(std::memory_order_relaxed) ? "game" : "other");
    }
    return g_sceneDel(scene, flags);
}

void LogStats(const char* why)
{
    const uint64_t comps = s.comps.take(), bodies = s.bodies.take(), sc = s.stockComps.take(), sb = s.stockBodies.take(),
                   inel = s.ineligible.take(), dr = s.drained.take(), df = s.drainFrames.take(), dus = s.drainUs.take(),
                   mdus = s.maxDrainUs.take(), mfn = s.maxFrameN.take(), mus = s.mapsUs.take(), mmus = s.maxMapsUs.take(),
                   fo = s.forced.take(), dp = s.dropped.take(), mp = s.maxPending.take(), ma = s.maxAge.take(),
                   cus = s.clearUs.take(), mcus = s.maxClearUs.take();
    s.measured.take();
    const uint64_t smc = s.smallComps.take(), smb = s.smallBodies.take(), oc = s.outComps.take(), ob = s.outBodies.take();
    const uint64_t nc = s.nearComps.take(), nb = s.nearBodies.take(), np = s.noPos.take(), st = s.sceneTerm.take(),
                   sd = s.startDrains.take();
    const double mq = s.minQueueM.takeM(), md = s.minDrainM.takeM();
    const uint64_t hid = s.hidden.take(), hf = s.hideFull.take(), hg = s.hiddenGone.take(), pc = s.preCalls.take(),
                   mm = s.maxMarked.take(), hpt = s.hiddenPT.take(), pcpt = s.preCallsPT.take(), hk = s.hideNoKey.take(),
                   tsw = s.tabSwitch.take(), tcl = s.tabClear.take(), fch = s.flagChanges.take();
    // "shapes rejected": candidate shapes of queued bodies the PreFilter answered None for, in queries of every kind
    // (traces, sweeps, overlaps); an overlap among them would have returned the body in 1.3.0-beta
    (void)hg;
    (void)pc;
    (void)pcpt;
    if (hid || hpt || hf || hk || fch)
        Log("[bodyspread] %s: query filter: %llu shapes of queued bodies rejected on game-thread data, %llu on physics-thread "
            "data, %llu components to stock (%llu set full, %llu a key unknown), max %llu keys hidden (2 per body), %llu table "
            "switches, %llu table clears, %llu frozen-flag changes",
            why, static_cast<unsigned long long>(hid), static_cast<unsigned long long>(hpt), static_cast<unsigned long long>(hf + hk),
            static_cast<unsigned long long>(hf), static_cast<unsigned long long>(hk), static_cast<unsigned long long>(mm),
            static_cast<unsigned long long>(tsw), static_cast<unsigned long long>(tcl), static_cast<unsigned long long>(fch));
    if (!comps && !sc && !dr && !dp && !smc && !oc && !nc && !np && !st) return;
    if (smc || oc)
        Log("[bodyspread] %s: also small clears in level removals %llu comps / %llu bodies, big clears outside them %llu comps / "
            "%llu bodies", why, static_cast<unsigned long long>(smc), static_cast<unsigned long long>(smb),
            static_cast<unsigned long long>(oc), static_cast<unsigned long long>(ob));
    Log("[bodyspread] %s (mode %d, per frame %d, max %d frames, min %d, near %d m): queued %llu comps / %llu bodies (maps %.1f "
        "ms, max %.2f), stock %llu comps / %llu bodies (%.1f ms, max %.2f; %llu ineligible, %llu comps / %llu bodies near, %llu "
        "position or bounds unknown); drained %llu in %llu frames (%.1f ms, max %.2f ms / %llu bodies a frame; %llu frame-start drains), "
        "dead-first %llu, scene-delete %llu, dropped %llu, max pending %llu, max age %llu frames; nearest queued %.1f m (ever "
        "%.1f), nearest at drain %.1f m",
        why, g_mode.load(), g_perFrame.load(), g_maxFrames.load(), g_min.load(), g_nearM.load(),
        static_cast<unsigned long long>(comps), static_cast<unsigned long long>(bodies), mus / 1000.0, mmus / 1000.0,
        static_cast<unsigned long long>(sc), static_cast<unsigned long long>(sb), cus / 1000.0, mcus / 1000.0,
        static_cast<unsigned long long>(inel), static_cast<unsigned long long>(nc), static_cast<unsigned long long>(nb),
        static_cast<unsigned long long>(np), static_cast<unsigned long long>(dr), static_cast<unsigned long long>(df),
        dus / 1000.0, mdus / 1000.0, static_cast<unsigned long long>(mfn), static_cast<unsigned long long>(sd),
        static_cast<unsigned long long>(fo), static_cast<unsigned long long>(st), static_cast<unsigned long long>(dp),
        static_cast<unsigned long long>(mp), static_cast<unsigned long long>(ma), mq, g_minEverM, md);
}


// The key's value as this build reads it: the public build knows on (1) and off (0) only.
int ModeOf(int key)
{
    return key ? 1 : 0;
}

}  // namespace

// Monitor thread, once a second (the ini is re-read each second): the keys, live.
void SetBodyRemovalSpread(int key, int perFrame, int maxFrames, int min, int nearM)
{
    if (!g_installed) return;
    int mode = ModeOf(key);
    constexpr int kMaxFramesCap = 120;    // 2 s at 60 fps: spread, never held
    const int p = g_perFrame, mf = g_maxFrames, mn = g_min, nm = g_nearM;
    g_perFrame = std::clamp(perFrame, 16, 100000);
    g_maxFrames = std::clamp(maxFrames, 2, kMaxFramesCap);
    g_min = std::max(min, 1);
    g_nearM = std::clamp(nearM, 0, 100000);
    if (mode != g_mode.load() || p != g_perFrame || mf != g_maxFrames || mn != g_min || nm != g_nearM) {
        LogStats("before switch");
        g_mode = mode;
        Log("[bodyspread] BodyRemovalSpread=%d (%s), per frame %d, max %d frames, min %d, near %d m", mode,
            mode ? "spread" : "the game's way", g_perFrame.load(), g_maxFrames.load(), g_min.load(), g_nearM.load());
    }
}

// Stats interval (monitor thread): what was spread since the last line (nothing logged when nothing happened). The lab
// logs every 5 s from SetBodyRemovalSpread instead.
void LogBodyRemovalSpreadStats()
{
    if (g_installed) LogStats("stats");
}

namespace {

uint8_t* CallTarget(uint8_t* call) { return call[0] == 0xE8 ? RipTarget(call, 1, 5) : nullptr; }

// Whether this destructor tail-jumps (within 0x500 bytes) to a function that calls (within 0x200 bytes) the function
// holding the DestroySolver call site (that site within 0x200 bytes of its start): ~FPhysScene_Chaos -> ~FChaosScene ->
// FChaosSolversModule::DestroySolver.
bool ReachesDestroySolver(const GameModule& game, uint8_t* dtor, const uint8_t* dsSite)
{
    for (size_t i = 0; i < 0x500 && game.text.Contains(dtor + i + 5); ++i) {
        if (dtor[i] != 0xE9) continue;
        uint8_t* const t = RipTarget(dtor + i, 1, 5);
        if (!game.text.Contains(t) || !game.text.Contains(t + 0x200)) continue;
        for (size_t j = 0; j < 0x200; ++j) {
            if (t[j] != 0xE8) continue;
            const uint8_t* const f = RipTarget(t + j, 1, 5);
            if (f <= dsSite && dsSite < f + 0x200) return true;
        }
    }
    return false;
}

// The only qword in .rdata that holds this code address (a vtable slot), or nullptr.
void** FindSlot(const GameModule& game, const uint8_t* target)
{
    void** found = nullptr;
    int hits = 0;
    const uintptr_t want = reinterpret_cast<uintptr_t>(target);
    for (uint8_t* p = game.rdata.begin; p + 8 <= game.rdata.end; p += 8)
        if (*reinterpret_cast<const uintptr_t*>(p) == want) { found = reinterpret_cast<void**>(p); ++hits; }
    return hits == 1 ? found : nullptr;
}

bool Discover(const GameModule& game, Found* f, const char** why)
{
    const auto clears = FindAll(game.text, ParsePattern(kClearPro));
    if (clears.size() != 1) { *why = "ClearAllInstanceBodies not found once"; return false; }
    uint8_t* const clear = clears[0];
    uint32_t arr, num;
    memcpy(&arr, clear + kClearArr, 4);
    memcpy(&num, clear + kClearNum, 4);
    if (num != arr + 8 || arr > 0x2000) { *why = "the instance-body array's offsets do not match"; return false; }
    uint8_t* const term = CallTarget(clear + kClearTerm);
    if (!term || !game.text.Contains(term) || !MatchesAt(term, ParsePattern(kTermPro))) { *why = "TermBody's bytes do not match"; return false; }
    const auto sites = FindAll(game.text, ParsePattern(kDestroySite));
    if (sites.size() != 1 || CallTarget(sites[0] + kDestroyCall) != clear) {
        *why = "the instanced-mesh OnDestroyPhysicsState call not found once";
        return false;
    }
    uint32_t weak, world, scene, handle;
    memcpy(&weak, term + kTermWeak, 4);
    memcpy(&world, term + kTermWorld, 4);
    memcpy(&scene, term + kTermScene, 4);
    memcpy(&handle, term + kTermHandle, 4);
    const uint32_t notify = term[kTermNotify];
    uint8_t* const weakGet = CallTarget(term + kTermWeakGet);
    uint8_t* const getWorld = CallTarget(term + kTermGetWorld);
    uint8_t* const maps = CallTarget(term + kTermMaps);
    uint8_t* const cur = CallTarget(term + kTermCurScene);
    if (!weakGet || !getWorld || !maps || !cur || !game.text.Contains(weakGet) || !game.text.Contains(getWorld) ||
        !game.text.Contains(maps) || !game.text.Contains(cur) || weak > 0x400 || world > 0x400 || scene > 0x1000 ||
        handle > 0x400 || handle + 8 != weak) {
        *why = "TermBody's callees or offsets do not match";
        return false;
    }
    if (!MatchesAt(cur, ParsePattern(kCurScenePro))) { *why = "GetCurrentScene's bytes do not match"; return false; }
    uint32_t hack;
    memcpy(&hack, cur + kCurHack, 4);
    const uint32_t solver = cur[kCurSolver];
    const auto us = FindAll(game.text, ParsePattern(kUnregSite));
    const auto ub = FindAll(game.text, ParsePattern(kUnregBlock));
    if (us.size() != 1 || ub.size() != 1) { *why = "RemoveFromWorld's unregister loops not found once"; return false; }
    uint8_t* const unreg = CallTarget(us[0] + kUnregCall);
    if (!unreg || unreg != CallTarget(ub[0] + kUnregBlockCall) || us[0] > ub[0] || ub[0] - us[0] > 0x200) {
        *why = "RemoveFromWorld's two loops do not call the same function";
        return false;
    }
    const auto ds = FindAll(game.text, ParsePattern(kDsSite));
    if (ds.size() != 1) { *why = "DestroySolver's call not found once"; return false; }
    uint8_t* const dsf = CallTarget(ds[0] + kDsCall);
    if (!dsf || !game.text.Contains(dsf) || !MatchesAt(dsf, ParsePattern(kDsPro))) { *why = "DestroySolver's bytes do not match"; return false; }
    // the physics scene's deleting destructor (optional: without it a dying scene's queued bodies are dropped)
    uint8_t* sceneDel = nullptr;
    int delHits = 0;
    for (uint8_t* c : FindAll(game.text, ParsePattern(kSceneDelPro))) {
        uint8_t* const dtor = CallTarget(c + kSceneDelDtor);
        if (dtor && game.text.Contains(dtor) && ReachesDestroySolver(game, dtor, ds[0])) { sceneDel = c; ++delHits; }
    }
    void** const slot = delHits == 1 ? FindSlot(game, sceneDel) : nullptr;
    // the query filter: required (no spread without it)
    uint8_t* const release = CallTarget(term + kTermRelease);
    if (!release || !game.text.Contains(release) || !MatchesAt(release, ParsePattern(kReleasePro))) {
        *why = "query filter: ReleaseActor's bytes do not match";
        return false;
    }
    const uint32_t particle = release[kReleaseParticle];
    uint8_t* const removeAccel = CallTarget(release + kReleaseRemove);
    if (!removeAccel || !game.text.Contains(removeAccel) || !MatchesAt(removeAccel, ParsePattern(kRemoveAccelPro)) ||
        particle < 0x10 || particle > 0x100 || (particle & 7)) {
        *why = "query filter: RemoveActorFromAccelerationStructure or the particle offset does not match";
        return false;
    }
    const auto ctors = FindAll(game.text, ParsePattern(kFilterCtor));
    if (ctors.size() != 1) { *why = "query filter: FCollisionQueryFilterCallback's constructor not found once"; return false; }
    void** const vt = reinterpret_cast<void**>(RipTarget(ctors[0] + kFilterCtorVt, 3, 7));
    if (!game.rdata.Contains(vt) || !game.rdata.Contains(vt + kPreFilterSlot + 1)) {
        *why = "query filter: the callback's vtable is not in .rdata";
        return false;
    }
    // slots 3 and 4 lead (directly or through one jmp) to PreFilterBaseImp, slots 1 and 2 do not
    const Pattern prePat = ParsePattern(kPreFilterPro);
    auto leadsToPre = [&](void* p) {
        uint8_t* c = static_cast<uint8_t*>(p);
        if (!game.text.Contains(c)) return static_cast<uint8_t*>(nullptr);
        if (c[0] == 0xE9) c = RipTarget(c, 1, 5);
        return game.text.Contains(c) && MatchesAt(c, prePat) ? c : nullptr;
    };
    uint8_t* const pre = leadsToPre(vt[kPreFilterSlot]);
    if (!pre || !leadsToPre(vt[kPreFilterSlotPT]) || leadsToPre(vt[1]) || leadsToPre(vt[2])) {
        *why = "query filter: the callback's vtable slots do not match (PostFilter 1-2, PreFilter 3-4)";
        return false;
    }
    // the visitors' PreFilter calls go through slot 3 or 4 (none through another slot of this vtable; both used)
    size_t v3 = 0, v4 = 0;
    for (uint8_t* v : FindAll(game.text, ParsePattern(kVisitTest))) {
        if (v[1] < 0x40 || v[1] > 0x47 || v[1] == 0x44 || !game.text.Contains(v + 4 + kVisitWindow + 3)) continue;  // [reg+disp8]
        bool r9 = false;  // the fourth argument (the particle or handle) written before the call: mov/lea r9,...
        for (size_t i = 4; i < 4 + kVisitWindow; ++i) {
            if ((v[i] == 0x4C || v[i] == 0x4D) && (v[i + 1] == 0x8B || v[i + 1] == 0x8D) && (v[i + 2] & 0x38) == 0x08) r9 = true;
            if ((v[i] == 0x49 || v[i] == 0x4D) && v[i + 1] == 0x89 && (v[i + 2] & 0xC7) == 0xC1) r9 = true;
            if (v[i] != 0xFF || v[i + 1] < 0x50 || v[i + 1] > 0x57 || v[i + 1] == 0x54) continue;  // call [reg+disp8]
            if (!r9) break;  // not a four-argument call (exe+0x442B1F3: another flag test, call [rax+18h] with two)
            const uint8_t d = v[i + 2];
            if (d == kPreFilterSlot * 8) ++v4;
            else if (d == kPreFilterSlotPT * 8) ++v3;
            else if (d >= 8 && d <= 0x30 && !(d & 7)) { *why = "query filter: a visitor's PreFilter call uses another vtable slot"; return false; }
            break;
        }
    }
    if (!v3 || !v4) { *why = "query filter: no visitor PreFilter call through slot 3 or slot 4 found"; return false; }
    // the frozen-game-thread flag's offset (optional, diagnostics): every site must agree
    uint32_t frozen = 0;
    size_t frozenSites = 0;
    bool frozenOk = true;
    const auto frozenAt = [&](const char* pat, size_t mod) {
        for (uint8_t* c : FindAll(game.text, ParsePattern(pat))) {
            if (c[1] < 0x40 || c[1] > 0x47 || c[1] == 0x44 || c[mod] < 0x80 || c[mod] > 0x87 || c[mod] == 0x84) continue;
            uint32_t o;
            memcpy(&o, c + mod + 1, 4);
            if (o == 0 || o > 0x1000 || (frozen && o != frozen)) frozenOk = false;
            frozen = o;
            ++frozenSites;
        }
    };
    frozenAt(kFrozenSetA, kFrozenModA);
    frozenAt(kFrozenSetB, kFrozenModB);
    if (!frozenOk) frozen = 0;
    f->frozen = frozen;
    f->frozenSites = frozenSites;
    f->filterCtor = ctors[0];
    f->filterSlot = vt + kPreFilterSlot;
    f->preFilter = static_cast<uint8_t*>(vt[kPreFilterSlot]);
    f->filterSlotPT = vt + kPreFilterSlotPT;
    f->preFilterPT = static_cast<uint8_t*>(vt[kPreFilterSlotPT]);
    f->release = release;
    f->particle = particle;
    f->visits4 = v4;
    f->visits3 = v3;
    f->clear = clear;
    f->term = term;
    f->weakGet = weakGet;
    f->getWorld = getWorld;
    f->maps = maps;
    f->unreg = unreg;
    f->destroySolver = dsf;
    f->sceneDel = slot ? sceneDel : nullptr;
    f->sceneSlot = slot;
    f->destroySite = sites[0] + kDestroyCall;
    f->unregSite = us[0] + kUnregCall;
    f->dsSite = ds[0] + kDsCall;
    f->arr = arr;
    f->weak = weak;
    f->world = world;
    f->scene = scene;
    f->handle = handle;
    f->notify = notify;
    f->solver = solver;
    f->hack = hack;
    return true;
}

void UseFound(const Found& f)
{
    g_clear = reinterpret_cast<ClearFn>(f.clear);
    g_term = reinterpret_cast<TermFn>(f.term);
    g_weakGet = reinterpret_cast<WeakGetFn>(f.weakGet);
    g_getWorld = reinterpret_cast<GetWorldFn>(f.getWorld);
    g_maps = reinterpret_cast<MapsFn>(f.maps);
    g_unreg = reinterpret_cast<UnregFn>(f.unreg);
    g_destroySolver = reinterpret_cast<SolverFn>(f.destroySolver);
    g_sceneDel = reinterpret_cast<DelFn>(f.sceneDel);
    g_sceneSlot = f.sceneSlot;
    g_arrOff = f.arr;
    g_weakOff = f.weak;
    g_worldOff = f.world;
    g_sceneOff = f.scene;
    g_handleOff = f.handle;
    g_notifyOff = f.notify;
    g_solverOff = f.solver;
    g_hackOff = f.hack;
    g_particleOff = f.particle;
    g_ptHandleOff = f.particle ? f.particle + 8 : 0;
    g_origPreFilter = reinterpret_cast<PreFilterFn>(f.preFilter);
    g_origPreFilterPT = reinterpret_cast<PreFilterFn>(f.preFilterPT);
    g_frozenOff = f.frozen;
}

bool Install(const GameModule& game, const char** why)
{
    Found f;
    if (!Discover(game, &f, why)) return false;
    constexpr size_t kStub = 16;
    uint8_t* const mem = AllocNearGame(3 * kStub);
    if (!mem) { *why = "no memory near the game's code"; return false; }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    const void* const hooks[3] = {reinterpret_cast<void*>(&ClearHook), reinterpret_cast<void*>(&UnregHook),
                                  reinterpret_cast<void*>(&DestroySolverHook)};
    uint8_t* const sites[3] = {f.destroySite, f.unregSite, f.dsSite};
    int32_t rel[3];
    for (int i = 0; i < 3; ++i) {
        memcpy(mem + i * kStub, jmpAbs, 6);
        memcpy(mem + i * kStub + 6, &hooks[i], 8);
        const int64_t d = (mem + i * kStub) - (sites[i] + 5);
        if (d != static_cast<int32_t>(d)) { *why = "stub out of reach"; return false; }
        rel[i] = static_cast<int32_t>(d);
    }
    DWORD old;
    VirtualProtect(mem, 3 * kStub, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, 3 * kStub);
    UseFound(f);
    g_found = f;
    // the query filter first: without it nothing else is patched (the 1.3.0-beta bug was the spread without it)
    if (!WritePointer(f.filterSlot, reinterpret_cast<void*>(&PreFilterHook)) ||
        !WritePointer(f.filterSlotPT, reinterpret_cast<void*>(&PreFilterHookPT))) {  // a wrapper already written stays: nothing is hidden
        *why = "could not wrap the query filter's PreFilter slots";
        return false;
    }
    g_filterOn = true;
    // order: the scene destructor and solver wrappers and the removal flag first, the clear hook (which queues) last
    if (f.sceneSlot && !WritePointer(f.sceneSlot, reinterpret_cast<void*>(&SceneDeleteHook))) {
        g_sceneSlot = nullptr;
        g_sceneDel = nullptr;
        g_found.sceneSlot = nullptr;
        Log("[bodyspread] could not wrap the physics scene's destructor slot: a dying scene's queued bodies are dropped (fallback)");
    }
    if (!WriteCode(sites[2] + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel[2]), 4))) { *why = "could not patch DestroySolver's call"; return false; }
    if (!WriteCode(sites[1] + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel[1]), 4))) { *why = "could not patch the unregister call"; return false; }
    if (!WriteCode(sites[0] + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel[0]), 4))) { *why = "could not patch the clear call"; return false; }
    return true;
}

}  // namespace

// Start-up (version.dll route): the hooks, installed when the key is != 0 (lab: always) and NpcCreationPacing's frame
// hooks are in (frameHooks); then SetBodyRemovalSpread switches it live.
bool InstallBodyRemovalSpread(bool beforeGameCode, int key, int perFrame, int maxFrames, int min, int nearM, bool frameHooks,
                              const wchar_t* iniPath)
{
    (void)iniPath;
    const bool install = key != 0;
    if (!install) { Log("[bodyspread] BodyRemovalSpread off in OblivionPerfFix.ini (takes a restart to switch on)"); return false; }
    if (!beforeGameCode) { Log("[bodyspread] BodyRemovalSpread: start-up only (version.dll), not installed"); return false; }
    if (!frameHooks) {
        Log("[bodyspread] BodyRemovalSpread: needs NpcCreationPacing's frame hooks (NpcCreationPacing=1 at start-up), not installed");
        return false;
    }
    const GameModule& game = Game();
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    g_qpcMs = 1000.0 / static_cast<double>(fq.QuadPart);
    const char* why = "";
    if (!Install(game, &why)) {
        Log("[bodyspread] BodyRemovalSpread: %s: off", why);
        return false;
    }
    g_installed = true;
    g_mode = ModeOf(key);  // as SetBodyRemovalSpread would set it (no switch line for the start value)
    SetBodyRemovalSpread(key, perFrame, maxFrames, min, nearM);
    const auto rva = [&](const void* p) { return p ? static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base) : 0u; };
    Log("[bodyspread] BodyRemovalSpread installed: clear call exe+0x%X -> exe+0x%X, unregister call exe+0x%X -> exe+0x%X, "
        "DestroySolver call exe+0x%X -> exe+0x%X, scene deleting destructor exe+0x%X (slot exe+0x%X%s); TermBody exe+0x%X, "
        "maps exe+0x%X; bodies +0x%X, actor +0x%X, owner +0x%X, notify +0x%X, world +0x%X, scene +0x%X, solver +0x%X / +0x%X; "
        "mode %d, per frame %d, max %d frames, min %d, near %d m (live; drains at NpcCreationPacing's post-actor tick); "
        "query filter: queued bodies hidden from world queries on game-thread and physics-thread data (PreFilter slots "
        "exe+0x%X -> exe+0x%X and exe+0x%X -> exe+0x%X, callback constructor exe+0x%X, visitor calls %zu through slot 4 and "
        "%zu through slot 3; ReleaseActor exe+0x%X, particle +0x%X, handle +0x%X); frozen-game-thread flag solver+0x%X "
        "(%zu sites%s)",
        rva(g_found.destroySite), rva(g_found.clear), rva(g_found.unregSite), rva(g_found.unreg), rva(g_found.dsSite),
        rva(g_found.destroySolver), rva(g_found.sceneDel), rva(g_found.sceneSlot),
        g_found.sceneSlot ? "" : ": not found, a dying scene's queued bodies are dropped", rva(g_found.term),
        rva(g_found.maps), g_arrOff, g_handleOff, g_weakOff, g_notifyOff, g_worldOff, g_sceneOff, g_solverOff, g_hackOff,
        g_mode.load(), g_perFrame.load(), g_maxFrames.load(), g_min.load(), g_nearM.load(), rva(g_found.filterSlot),
        rva(g_found.preFilter), rva(g_found.filterSlotPT), rva(g_found.preFilterPT), rva(g_found.filterCtor),
        g_found.visits4, g_found.visits3, rva(g_found.release), g_particleOff, g_ptHandleOff, g_frozenOff,
        g_found.frozenSites, g_frozenOff ? "" : ": not found or not agreeing, not logged");
    return true;
}

static void DrainQuota(bool atStart);

// Game thread, once per frame at the frame start (NpcCreationPacing's MaxTickRate hook, before the world ticks): the
// summary of the frame that ended and, with mode 64 or when the last frame had no post-actor tick, the quota's drain.
void BodySpreadFrame()
{
    if (!g_installed) return;
    g_gtId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_lastFrameQpc.store(NowQpc(), std::memory_order_relaxed);
    ++g_frame;
    const int mode = g_mode.load(std::memory_order_relaxed);
    CheckWatched();  // the frozen-game-thread flag of the solvers met so far: a change is logged
    // the frame that just ended
    if (g_fDrained) {
        s.drained.add(g_fDrained);
        s.forced.add(g_fForced);
        s.drainFrames.add(1);
        s.drainUs.add(static_cast<uint64_t>(g_fDrainMs * 1000));
        s.maxDrainUs.max(static_cast<uint64_t>(g_fDrainMs * 1000));
        s.maxFrameN.max(g_fDrained);
    }
    g_fComps = g_fBodies = g_fQueued = g_fNear = g_fDrained = g_fSlow = g_fForced = 0;
    g_fDrainMs = g_fMaxCallMs = 0;
    g_fMinDistM = -1;
    g_fNearComp = nullptr;
    // the frame's drain: at the post-actor tick (default) or here (mode 64, or the last frame had no post-actor tick)
    if ((mode & 64) || ++g_sincePost >= 2) {
        if (!(mode & 64)) s.startDrains.add(1);
        DrainQuota(true);
    }
}

// The frame's quota: batches whose component was garbage-collected first (their bodies' weak owner reads null: no reason
// to keep them), then the oldest; at least BodyRemovalSpreadPerFrame, more when needed so that no batch waits longer than
// BodyRemovalSpreadMaxFrames frames.
static void DrainQuota(bool atStart)
{
    (void)atStart;
    ++g_drains;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_q.empty()) {
        uint64_t oldest = g_frame;
        size_t dead = 0, deadBodies = 0;
        for (size_t i = 0; i < g_q.size(); ++i) {
            Batch& b = g_q[i];
            oldest = std::min(oldest, b.frame);
            if (b.next >= b.bodies.size() || g_weakGet(b.bodies[b.next] + g_weakOff) == b.comp) continue;
            if (i != dead) {  // move it in front of the live ones (keeping both groups' order)
                Batch moved = std::move(g_q[i]);
                g_q.erase(g_q.begin() + static_cast<std::ptrdiff_t>(i));
                g_q.insert(g_q.begin() + static_cast<std::ptrdiff_t>(dead), std::move(moved));
            }
            deadBodies += g_q[dead].bodies.size() - g_q[dead].next;
            ++dead;
        }
        const uint64_t age = g_frame - oldest;
        s.maxAge.max(age);
        const int64_t left = std::max<int64_t>(1, static_cast<int64_t>(g_maxFrames.load(std::memory_order_relaxed)) - static_cast<int64_t>(age));
        const int64_t q = std::max<int64_t>(g_perFrame.load(std::memory_order_relaxed), (static_cast<int64_t>(g_pending) + left - 1) / left);
        g_fForced += std::min(DrainLocked(static_cast<size_t>(q)), deadBodies);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// Game thread, once per frame after the actors' tick (NpcCreationPacing's OnWorldPostActorTick call), after
// TG_EndPhysics: the default drain point (the terminations reach the next frame's push). Measured (run r5): ~1.1 us of game
// thread per body here, as in the stock clear, against ~4.3 us at the frame start, where single terminations waited up
// to 1.6 ms (most likely the async traces in flight there; see the header).
void BodySpreadPostTick()
{
    if (!g_installed) return;
    g_sincePost = 0;
    if (!(g_mode.load(std::memory_order_relaxed) & 64)) DrainQuota(false);
}

// ---- self-test ----
int BodySpreadTestDiscover(const GameModule& game, uint32_t* arr, uint32_t* handle, uint32_t* weak)
{
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) {
        printf("    BodyRemovalSpread discovery: %s\n", why);
        return 0;
    }
    *arr = f.arr;
    *handle = f.handle;
    *weak = f.weak;
    if (!f.sceneSlot) {
        printf("    BodyRemovalSpread: the physics scene's deleting destructor / its vtable slot not found once\n");
        return 2;
    }
    if (static_cast<uint32_t>(f.sceneDel - game.base) != 0x351A8D0u ||
        static_cast<uint32_t>(reinterpret_cast<uint8_t*>(f.sceneSlot) - game.base) != 0x775AF00u) {
        printf("    BodyRemovalSpread: scene destructor exe+0x%X, slot exe+0x%X (expected exe+0x351A8D0, exe+0x775AF00)\n",
               static_cast<unsigned>(f.sceneDel - game.base),
               static_cast<unsigned>(reinterpret_cast<uint8_t*>(f.sceneSlot) - game.base));
        return 3;
    }
    const auto rv = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
    if (rv(f.filterSlot) != 0x75B84F0u + 0x20u || rv(f.preFilter) != 0x2F8AA20u || rv(f.filterSlotPT) != 0x75B84F0u + 0x18u ||
        rv(f.preFilterPT) != 0x2F8AA20u || rv(f.filterCtor) != 0x2F6B510u || rv(f.release) != 0x1AA3A40u || f.particle != 0x48 ||
        f.visits4 != 84 || f.visits3 != 41) {
        printf("    BodyRemovalSpread query filter: slots exe+0x%X -> exe+0x%X, exe+0x%X -> exe+0x%X, ctor exe+0x%X, ReleaseActor "
               "exe+0x%X, particle +0x%X, visitor calls %zu / %zu (expected exe+0x75B8510 / exe+0x75B8508 -> exe+0x2F8AA20, "
               "exe+0x2F6B510, exe+0x1AA3A40, +0x48, 84 / 41)\n",
               rv(f.filterSlot), rv(f.preFilter), rv(f.filterSlotPT), rv(f.preFilterPT), rv(f.filterCtor), rv(f.release),
               f.particle, f.visits4, f.visits3);
        return 4;
    }
    if (f.frozen != 0x1D0 || f.frozenSites != 5) {
        printf("    BodyRemovalSpread: frozen-game-thread flag solver+0x%X at %zu sites (expected +0x1D0 at 5)\n", f.frozen,
               f.frozenSites);
        return 5;
    }
    printf("    BodyRemovalSpread query filter: PreFilter slots exe+0x%X / exe+0x%X, visitor calls %zu / %zu through them; "
           "frozen flag solver+0x%X (%zu sites)\n",
           rv(f.filterSlot), rv(f.filterSlotPT), f.visits4, f.visits3, f.frozen, f.frozenSites);
    return 1;
}

namespace {
// Fakes: a component (bodies at +0x6E8, world at +0xB0 -> scene at +0x228, bounds at +0x108), bodies (vtable +0, notify
// +0x5B, actor +0x158, owner +0x160 = a pointer here, read by the fake weak Get), actors (+8 solver), solvers (+0x240 scene).
struct TestLog {
    std::vector<void*> maps, terms, dels;
    int clears = 0;
    std::vector<uint8_t*> dead;
};
TestLog* t_log = nullptr;
uint8_t* TestWeakGet(const void* weak)
{
    uint8_t* const c = *static_cast<uint8_t* const*>(weak);
    for (uint8_t* d : t_log->dead)
        if (d == c) return nullptr;
    return c;
}
uint8_t* TestGetWorld(uint8_t*) { return nullptr; }
void TestMaps(uint8_t*, void* h) { t_log->maps.push_back(h); }
int t_ownerSeen = 0;  // TermBody calls that would have read a component (owner not null) while queued
bool t_queuedPhase = false;
// Query filter tests: TermBody as the game's ReleaseActor ends it (the body's handle cleared); a queued body's keys must
// still be hidden when its TermBody runs (removed from the structures there) and shown before its delete.
int t_shownAtTerm = 0;  // queued (hidden) bodies with a key already shown when TermBody ran
struct TermKeys { const void* body; const void* gt; const void* pt; };
std::vector<TermKeys> t_termPart;  // body -> its keys, taken at TermBody
void TestTerm(uint8_t* body, bool)
{
    if (t_queuedPhase && *reinterpret_cast<uint8_t**>(body + 0x160)) ++t_ownerSeen;
    t_log->terms.push_back(body);
    *reinterpret_cast<uint8_t**>(body + 0x160) = nullptr;
    if (g_filterOn) {
        const void *gt, *pt;
        KeysForTerm(body, &gt, &pt);
        if (t_queuedPhase && !(g_mode.load() & 256) && gt && (!Hidden(gt) || !pt || !Hidden(pt))) ++t_shownAtTerm;
        t_termPart.push_back({body, gt, pt});
        *reinterpret_cast<uint8_t**>(body + 0x158) = nullptr;  // ReleaseActor: the handle is gone
    }
}
int t_hiddenAtDel = 0;  // bodies deleted while a key was still hidden from queries (query filter tests)
void* TestDel(void* obj, uint32_t)
{
    t_log->dels.push_back(obj);
    if (g_filterOn)
        for (auto it = t_termPart.rbegin(); it != t_termPart.rend(); ++it)  // the body's latest TermBody
            if (it->body == obj) {
                if ((it->gt && Hidden(it->gt)) || (it->pt && Hidden(it->pt))) ++t_hiddenAtDel;
                break;
            }
    return obj;
}
// The query filter tests: the game's PreFilters as fakes that block everything, and a tiny scene-query model of UE 5.3
// SQVisitor.h's per-shape loop (PreFilter None = the shape is skipped), on game-thread data (slot 4, particles) or
// physics-thread data (slot 3, handles: the frozen game thread's path).
// Two separate fakes (each counts its calls), so a wrapper that called the other slot's original fails test G.
int t_origCalls = 0, t_origCallsPT = 0;
uint8_t TestOrigPreFilter(void*, const void*, const void*, const void*) { ++t_origCalls; return 2; }    // slot 4's original
uint8_t TestOrigPreFilterPT(void*, const void*, const void*, const void*) { ++t_origCallsPT; return 2; } // slot 3's original
struct TestCand { const void* particle; double dist; };
uint8_t TestPre(const void* key, bool pt) { return pt ? PreFilterHookPT(nullptr, nullptr, nullptr, key) : PreFilterHook(nullptr, nullptr, nullptr, key); }
std::vector<const void*> TestOverlap(const std::vector<TestCand>& cands, bool pt = false)  // every shape not filtered out
{
    std::vector<const void*> r;
    for (const auto& c : cands)
        if (TestPre(c.particle, pt) != 0) r.push_back(c.particle);
    return r;
}
const void* TestRay(const std::vector<TestCand>& cands, bool pt = false)  // raycast / sweep single: the nearest blocking shape
{
    const void* best = nullptr;
    double bd = 1e30;
    for (const auto& c : cands)
        if (c.dist < bd && TestPre(c.particle, pt) == 2) { best = c.particle; bd = c.dist; }
    return best;
}
// A fake physics proxy class: its GetHandleUnsafe's bytes (mov rax,[rcx+18h]; ret: the handle right after the particle at
// +0x10, as the tests' g_particleOff) and one whose bytes do not match.
const uint8_t t_getHandleOk[5] = {0x48, 0x8B, 0x41, 0x18, 0xC3};
const uint8_t t_getHandleBad[5] = {0x48, 0x8B, 0x41, 0x20, 0xC3};
const void* t_proxyVtOk[2] = {t_getHandleOk, nullptr};
const void* t_proxyVtBad[2] = {t_getHandleBad, nullptr};
DelFn t_vtbl[1] = {TestDel};
void TestClear(uint8_t* comp)  // as the game's: TermBody + delete for non-null slots, then Num = 0
{
    ++t_log->clears;
    const bool q = t_queuedPhase;
    t_queuedPhase = false;  // the game's own clear terminates the component's (still owned) bodies
    struct Restore { bool v; ~Restore() { t_queuedPhase = v; } } restore{q};
    uint8_t** data = *reinterpret_cast<uint8_t***>(comp + 0x6E8);
    int32_t& num = *reinterpret_cast<int32_t*>(comp + 0x6F0);
    for (int32_t i = 0; i < num; ++i)
        if (data[i]) {
            TestTerm(data[i], false);
            TestDel(data[i], 1);
        }
    num = 0;
}
bool TestUnreg(void*, int32_t) { return true; }
void* t_destroyed = nullptr;
void TestSolver(void* sv) { t_destroyed = sv; }
void* t_sceneDeleted = nullptr;
size_t t_termsAtSceneDelete = 0;
void* TestSceneDel(void* scene, uint32_t)
{
    t_sceneDeleted = scene;
    t_termsAtSceneDelete = t_log->terms.size();
    return scene;
}
double t_px = 1e6, t_py = 0, t_pz = 0;  // the fake player: 10 km from the fake components' bounds (a 2 m box at 0,0,0)
bool t_posKnown = true;
bool TestPos(double* x, double* y, double* z)
{
    *x = t_px;
    *y = t_py;
    *z = t_pz;
    return t_posKnown;
}
}  // namespace

static void TestFrame() { BodySpreadFrame(); BodySpreadPostTick(); }

// 0 = all checks passed, else failed bits.
int BodySpreadTestLogic()
{
    int fail = 0;
    TestLog log;
    t_log = &log;
    const Found saved = g_found;
    const PosFn savedPos = g_playerPos;
    const int savedVt = g_sceneVtOk;
    Found f;
    f.clear = reinterpret_cast<uint8_t*>(&TestClear);
    f.term = reinterpret_cast<uint8_t*>(&TestTerm);
    f.weakGet = reinterpret_cast<uint8_t*>(&TestWeakGet);
    f.getWorld = reinterpret_cast<uint8_t*>(&TestGetWorld);
    f.maps = reinterpret_cast<uint8_t*>(&TestMaps);
    f.unreg = reinterpret_cast<uint8_t*>(&TestUnreg);
    f.destroySolver = reinterpret_cast<uint8_t*>(&TestSolver);
    f.sceneDel = reinterpret_cast<uint8_t*>(&TestSceneDel);
    f.arr = 0x6E8; f.weak = 0x160; f.world = 0xB0; f.scene = 0x228; f.handle = 0x158; f.notify = 0x5B; f.solver = 8; f.hack = 0x240;
    f.frozen = 0x1D0;  // the fake solver's frozen-game-thread flag (test 18)
    static uint8_t scene[0x300], solver[0x300], world[0x300];
    static void* sceneVt[8];
    *reinterpret_cast<void***>(scene) = sceneVt;
    f.sceneSlot = &sceneVt[4];
    UseFound(f);
    g_playerPos = &TestPos;
    g_sceneVtOk = 0;
    t_px = 1e6;
    t_posKnown = true;
    const bool wasInstalled = g_installed;
    g_installed = true;
    if (g_qpcMs == 0) {
        LARGE_INTEGER fq;
        QueryPerformanceFrequency(&fq);
        g_qpcMs = 1000.0 / static_cast<double>(fq.QuadPart);
    }
    const int sm = g_mode, sp = g_perFrame, smf = g_maxFrames, smn = g_min, snm = g_nearM;
    g_mode = 1;
    g_perFrame = 100;
    g_maxFrames = 10;
    g_min = 50;
    g_nearM = 60;
    g_q.clear();
    g_pending = 0;
    g_frame = 0;
    g_drains = 0;
    g_sincePost = 0;

    *reinterpret_cast<uint8_t**>(world + 0x228) = scene;
    *reinterpret_cast<uint8_t**>(solver + 0x240) = scene;
    constexpr int kN = 300;
    static uint8_t comps[3][0x800];
    static uint8_t bodies[3][kN][0x200];
    static uint8_t actors[3][kN][0x20];
    static uint8_t* arrays[3][kN];
    auto setup = [&](int c, int n) {
        memset(comps[c], 0, sizeof comps[c]);
        // bounds at +0x108: a 2 m box around 0,0,0 (origin, extent 100, radius 173; BoxOf rejects boxes under 1 cm)
        const double bounds[7] = {0, 0, 0, 100, 100, 100, 173};
        memcpy(comps[c] + kBoundsOff, bounds, sizeof bounds);
        *reinterpret_cast<uint8_t**>(comps[c] + 0xB0) = world;
        *reinterpret_cast<uint8_t***>(comps[c] + 0x6E8) = arrays[c];
        *reinterpret_cast<int32_t*>(comps[c] + 0x6F0) = n;
        for (int i = 0; i < n; ++i) {
            memset(bodies[c][i], 0, 0x200);
            *reinterpret_cast<DelFn**>(bodies[c][i]) = t_vtbl;
            *reinterpret_cast<uint8_t**>(bodies[c][i] + 0x158) = actors[c][i];
            *reinterpret_cast<uint8_t**>(bodies[c][i] + 0x160) = comps[c];
            *reinterpret_cast<uint8_t**>(actors[c][i] + 8) = solver;
            arrays[c][i] = (i == 7) ? nullptr : bodies[c][i];  // one null slot (zero-scale instance)
        }
    };
    auto removal = [&](int c) {
        ++t_inRemoval;
        ClearHook(comps[c]);
        --t_inRemoval;
    };
    g_gtId = GetCurrentThreadId();
    t_ownerSeen = 0;
    t_queuedPhase = true;
    TestFrame();  // frame 1
    // 1. outside a level removal: stock
    setup(0, kN);
    ClearHook(comps[0]);
    if (log.clears != 1 || log.terms.size() != kN - 1 || !log.maps.empty() || g_pending) fail |= 1;
    log = TestLog{};
    // 2. inside: queued, maps removed at once in stock order, the clear empties, nothing terminated inside the removal
    setup(0, kN);
    removal(0);
    if (log.clears != 1 || *reinterpret_cast<int32_t*>(comps[0] + 0x6F0) != 0 || log.maps.size() != kN - 1 ||
        log.maps[0] != actors[0][0] || log.maps[7] != actors[0][8] || !log.terms.empty() || !log.dels.empty() ||
        g_pending != kN - 1 || g_sceneVtOk != 1)
        fail |= 2;
    // 3. the next frames drain 100 each at the post-actor tick, oldest first; pending reaches 0
    BodySpreadFrame();
    if (!log.terms.empty()) fail |= 4;  // nothing at the frame start
    BodySpreadPostTick();
    if (log.terms.size() != 100 || log.terms[0] != bodies[0][0]) fail |= 4;
    TestFrame();
    if (log.terms.size() != 200 || log.terms[199] != bodies[0][200]) fail |= 4;
    TestFrame();
    if (g_pending != 0 || log.terms.size() != kN - 1 || !g_q.empty()) fail |= 8;
    // 4. small component (below min) inside a removal: stock
    log = TestLog{};
    setup(1, 20);
    removal(1);
    if (log.terms.size() != 19 || !log.maps.empty()) fail |= 16;
    // 5. a notifying body: stock
    log = TestLog{};
    setup(1, 60);
    bodies[1][3][0x5B] |= 4;
    removal(1);
    if (log.terms.size() != 59 || !log.maps.empty()) fail |= 32;
    // 6. age bound in frames: perFrame 10, maxFrames 10, 300 bodies -> done within 10 frames of queueing
    log = TestLog{};
    g_perFrame = 10;
    TestFrame();
    setup(0, kN);
    removal(0);
    int frames = 0;
    while (g_pending && frames < 20) { TestFrame(); ++frames; }
    if (g_pending || frames > 10 || log.terms.size() != kN - 1) fail |= 64;
    // 7. dead component: its batch goes first, within the frame's quota (no unbounded drain)
    log = TestLog{};
    g_perFrame = 100;
    g_maxFrames = 30;
    TestFrame();
    setup(0, kN);
    removal(0);  // live, older
    setup(1, 60);
    removal(1);  // will die
    log.dead.push_back(comps[1]);
    TestFrame();
    if (log.terms.size() != 100 || log.terms[0] != bodies[1][0] || log.terms[58] != bodies[1][59] ||
        log.terms[59] != bodies[0][0])
        fail |= 128;
    for (int k = 0; k < 40 && g_pending; ++k) TestFrame();
    if (g_pending) fail |= 128;
    // 8. scene deleted with queued bodies: terminated first, then the scene's own delete runs; nothing left for the solver
    log = TestLog{};
    TestFrame();
    setup(2, kN);
    removal(2);
    t_sceneDeleted = nullptr;
    SceneDeleteHook(scene, 1);
    if (g_pending || !g_q.empty() || log.terms.size() != kN - 1 || t_sceneDeleted != scene ||
        t_termsAtSceneDelete != kN - 1)
        fail |= 256;
    // 8b. fallback: solver destroyed with queued bodies: dropped untouched, then the solver's destroy runs
    log = TestLog{};
    TestFrame();
    setup(2, kN);
    removal(2);
    t_destroyed = nullptr;
    DestroySolverHook(solver);
    if (g_pending || !g_q.empty() || !log.terms.empty() || t_destroyed != solver) fail |= 256;
    // 9. fast map order: e0, eN-1, ..., e1 (null slot skipped)
    log = TestLog{};
    g_mode = 1 | 16;
    TestFrame();
    setup(0, kN);
    removal(0);
    if (log.maps.size() != kN - 1 || log.maps[0] != actors[0][0] || log.maps[1] != actors[0][kN - 1] ||
        log.maps.back() != actors[0][1])
        fail |= 512;
    for (int k = 0; k < 20 && g_pending; ++k) TestFrame();
    // 10. a body owned by another component: stock
    log = TestLog{};
    g_mode = 1;
    setup(1, 60);
    *reinterpret_cast<uint8_t**>(bodies[1][5] + 0x160) = comps[0];
    removal(1);
    if (!log.maps.empty() || log.terms.size() != 59) fail |= 1024;
    // 11. mode 8 (measure split, no spread): maps first in stock order, then the stock clear; nothing queued
    log = TestLog{};
    g_mode = 8;
    setup(1, 60);
    removal(1);
    if (log.maps.size() != 59 || log.terms.size() != 59 || g_pending) fail |= 2048;
    // 13. mode 64: the frame start drains the quota, the post-actor tick does not
    log = TestLog{};
    g_mode = 1 | 64;
    g_perFrame = 100;
    g_maxFrames = 30;
    TestFrame();
    setup(0, kN);
    removal(0);
    BodySpreadFrame();
    const size_t afterStart = log.terms.size();
    BodySpreadPostTick();
    if (afterStart != 100 || log.terms.size() != 100) fail |= 8192;
    for (int k = 0; k < 40 && g_pending; ++k) TestFrame();
    if (g_pending) fail |= 8192;
    // 14. default mode, no post-actor tick (world paused): the second frame start drains, the bound holds in frames
    log = TestLog{};
    g_mode = 1;
    g_maxFrames = 10;
    TestFrame();
    setup(0, kN);
    removal(0);
    BodySpreadFrame();  // the frame after the removal: a post-actor tick may still come
    const size_t first = log.terms.size();
    int paused = 1;
    while (g_pending && paused < 20) { BodySpreadFrame(); ++paused; }
    if (first != 0 || g_pending || paused > 11) fail |= 16384;
    // 15. near the player (S3): stock; unknown position: stock; far: queued
    log = TestLog{};
    g_maxFrames = 30;
    t_px = 50 * 100.0;  // 49 m from the box
    TestFrame();
    setup(1, 60);
    removal(1);
    if (!log.maps.empty() || log.terms.size() != 59 || g_pending) fail |= 32768;
    log = TestLog{};
    t_posKnown = false;
    setup(1, 60);
    removal(1);
    if (!log.maps.empty() || log.terms.size() != 59 || g_pending) fail |= 32768;
    log = TestLog{};
    t_posKnown = true;
    t_px = 70 * 100.0;  // 69 m from the box
    setup(1, 60);
    removal(1);
    if (log.maps.size() != 59 || !log.terms.empty() || g_pending != 59) fail |= 32768;
    for (int k = 0; k < 40 && g_pending; ++k) TestFrame();
    // 16. bounds box: the distance is to the box, not its centre
    {
        double box[6];
        uint8_t c[0x200] = {};
        const double v[7] = {1000, 0, 0, 500, 500, 500, 866};
        memcpy(c + kBoundsOff, v, sizeof v);
        t_px = 3000;
        if (!BoxOf(c, box) || std::fabs(DistM(box) - 15.0) > 0.01) fail |= 65536;
        const double bad[7] = {0, 0, 0, -1, 0, 0, 0};
        memcpy(c + kBoundsOff, bad, sizeof bad);
        if (BoxOf(c, box)) fail |= 65536;
    }
    // 17. query filter (fix for the 1.3.0-beta crash): queued bodies are hidden from every query, on game-thread data
    // (slot 4, particles) and on physics-thread data (slot 3, handles: S1, the frozen game thread's path), until TermBody
    // has removed them, shown again before their delete; a full set, an unknown key or an unknown proxy class sends the
    // component to stock; the two tables keep a continuous stream hidden (N1)
    {
        static uint8_t parts[3][kN][16], pts[3][kN][16];
        static uint8_t realPart[16], realPt[16];
        auto withParticles = [&](int c, int n) {
            setup(c, n);
            for (int i = 0; i < n; ++i) {
                *reinterpret_cast<const void***>(actors[c][i]) = t_proxyVtOk;
                *reinterpret_cast<uint8_t**>(actors[c][i] + 0x10) = parts[c][i];
                *reinterpret_cast<uint8_t**>(actors[c][i] + 0x18) = pts[c][i];
            }
        };
        const void* savedOk[kProxyVts];
        const void* savedBad[kProxyVts];
        memcpy(savedOk, g_proxyVtOk, sizeof savedOk);
        memcpy(savedBad, g_proxyVtBad, sizeof savedBad);
        memset(g_proxyVtOk, 0, sizeof g_proxyVtOk);
        memset(g_proxyVtBad, 0, sizeof g_proxyVtBad);
        g_filterOn = true;
        g_particleOff = 0x10;
        g_ptHandleOff = 0x18;
        g_origPreFilter = &TestOrigPreFilter;
        g_origPreFilterPT = &TestOrigPreFilterPT;
        t_hiddenAtDel = 0;
        t_shownAtTerm = 0;
        t_termPart.clear();
        s.hideFull.take();
        s.hideNoKey.take();
        s.tabSwitch.take();
        s.tabClear.take();
        g_mode = 1;
        g_perFrame = 100;
        g_maxFrames = 10;
        t_px = 1e6;
        t_posKnown = true;
        log = TestLog{};
        TestFrame();
        // G: nothing hidden: the game's PreFilters answer, each slot's wrapper calling its own original
        t_origCalls = t_origCallsPT = 0;
        if (PreFilterHook(nullptr, nullptr, nullptr, parts[0][3]) != 2 || t_origCalls != 1 || t_origCallsPT != 0)
            fail |= 1 << 22;
        if (PreFilterHookPT(nullptr, nullptr, nullptr, pts[0][3]) != 2 || t_origCalls != 1 || t_origCallsPT != 1)
            fail |= 1 << 22;
        // A: queued -> every body's two keys hidden
        withParticles(0, kN);
        removal(0);
        bool allHidden = g_pending == kN - 1 && g_marked[0].load() + g_marked[1].load() == 2 * (kN - 1);
        for (int i = 0; i < kN; ++i)
            if (i != 7 && (!Hidden(parts[0][i]) || !Hidden(pts[0][i]))) allHidden = false;
        if (!allHidden || Hidden(parts[0][7]) || Hidden(pts[0][7]) || Hidden(realPart) || Hidden(realPt)) fail |= 1 << 17;
        // B: overlap, sweep and raycast on game-thread data never return a queued body; a real body behind one is found
        const std::vector<TestCand> cands = {{parts[0][3], 1.0}, {realPart, 5.0}, {parts[0][4], 2.0}};
        const auto ov = TestOverlap(cands);
        if (ov.size() != 1 || ov[0] != realPart || TestRay(cands) != realPart) fail |= 1 << 18;
        // I (S1): the same on physics-thread data (slot 3, handles), and either slot rejects either key (order moot)
        const std::vector<TestCand> candsPT = {{pts[0][3], 1.0}, {realPt, 5.0}, {pts[0][4], 2.0}};
        const auto ovPT = TestOverlap(candsPT, true);
        if (ovPT.size() != 1 || ovPT[0] != realPt || TestRay(candsPT, true) != realPt ||
            PreFilterHookPT(nullptr, nullptr, nullptr, parts[0][5]) != 0 || PreFilterHook(nullptr, nullptr, nullptr, pts[0][5]) != 0)
            fail |= 1 << 25;
        // C: drained -> shown again (both keys), each before its delete
        for (int k = 0; k < 20 && g_pending; ++k) TestFrame();
        if (g_pending || g_marked[0].load() + g_marked[1].load() != 0 || t_hiddenAtDel || t_shownAtTerm || Hidden(parts[0][3]) ||
            Hidden(pts[0][3]) || log.dels.size() != kN - 1 || TestOverlap(cands).size() != 3 || TestRay(cands) != parts[0][3] ||
            TestOverlap(candsPT, true).size() != 3 || TestRay(candsPT, true) != pts[0][3])
            fail |= 1 << 19;
        // F: lab bit 256 (the 1.3.0-beta behaviour): queued but not hidden; the queries still meet the queued bodies
        log = TestLog{};
        g_mode = 1 | 256;
        TestFrame();
        withParticles(0, kN);
        removal(0);
        const bool oldQueued = g_pending == kN - 1 && g_marked[0].load() + g_marked[1].load() == 0 && TestOverlap(cands).size() == 3;
        for (int k = 0; k < 20 && g_pending; ++k) TestFrame();
        if (!oldQueued || g_pending || log.dels.size() != kN - 1) fail |= 1 << 23;
        g_mode = 1;
        // H: a query never sees an actor or a body as hidden (address compare only), and the set survives many queue / drain
        // rounds (tombstones reused or cleared)
        log = TestLog{};
        bool rounds = true;
        for (int r = 0; r < 40 && rounds; ++r) {
            TestFrame();
            withParticles(0, kN);
            removal(0);
            rounds = g_marked[0].load() + g_marked[1].load() == 2 * (kN - 1) && !Hidden(actors[0][3]) && !Hidden(bodies[0][3]);
            for (int k = 0; k < 20 && g_pending; ++k) TestFrame();
            rounds = rounds && g_marked[0].load() + g_marked[1].load() == 0 && g_tab[0].used + g_tab[1].used < 2 * (kTombClear + 2 * kN);
        }
        if (!rounds || t_hiddenAtDel || t_shownAtTerm) fail |= 1 << 24;
        // D: the set cannot take the component: stock, nothing left hidden
        log = TestLog{};
        g_hiddenLimit = 100;
        withParticles(0, kN);
        removal(0);
        if (g_pending || g_marked[0].load() + g_marked[1].load() != 0 || log.terms.size() != kN - 1 || Hidden(parts[0][0]) ||
            s.hideFull.take() != 1)
            fail |= 1 << 20;
        g_hiddenLimit = kHiddenCap / 2;
        // J: a proxy class whose GetHandleUnsafe does not read the handle after the particle: stock, nothing hidden
        log = TestLog{};
        withParticles(0, kN);
        *reinterpret_cast<const void***>(actors[0][150]) = t_proxyVtBad;
        removal(0);
        const bool badClass = !g_pending && g_marked[0].load() + g_marked[1].load() == 0 && log.terms.size() == kN - 1 &&
                              !Hidden(parts[0][0]) && s.hideNoKey.take() == 1;
        // K: a body whose physics-thread handle is not set yet: stock, nothing hidden
        log = TestLog{};
        withParticles(0, kN);
        *reinterpret_cast<uint8_t**>(actors[0][20] + 0x18) = nullptr;
        removal(0);
        const bool noHandle = !g_pending && g_marked[0].load() + g_marked[1].load() == 0 && log.terms.size() == kN - 1 &&
                              s.hideNoKey.take() == 1;
        if (!badClass || !noHandle) fail |= 1 << 26;
        // E: scene deleted / solver dropped with queued bodies: nothing stays hidden
        log = TestLog{};
        TestFrame();
        withParticles(2, kN);
        removal(2);
        SceneDeleteHook(scene, 1);
        const bool sceneOk = g_marked[0].load() + g_marked[1].load() == 0 && !t_hiddenAtDel;
        TestFrame();
        withParticles(2, kN);
        removal(2);
        const bool queuedOk = g_marked[0].load() + g_marked[1].load() == 2 * (kN - 1);
        DestroySolverHook(solver);
        if (!sceneOk || !queuedOk || g_marked[0].load() + g_marked[1].load() != 0 || g_pending || Hidden(parts[2][0]) ||
            Hidden(pts[2][0]))
            fail |= 1 << 21;
        // L (N1): a continuous unload stream (the set never empties) stays hidden without filling a table with tombstones:
        // inserts move to the other table once it is empty; the old one empties as its bodies are terminated, is cleared
        {
            constexpr int kC = 12, kB = 100;
            static uint8_t sc[kC][0x800], sb[kC][kB][0x200], sa[kC][kB][0x20], spp[kC][kB][16], spt[kC][kB][16];
            static uint8_t* sarr[kC][kB];
            log = TestLog{};
            g_switchAt = 600;
            g_hiddenLimit = 2400;
            g_perFrame = 90;  // 100 bodies in, 90 out per frame: the backlog grows, the set never empties
            g_maxFrames = 30;
            bool ok = true, everEmpty = false;
            for (int fr = 0; fr < 40 && ok; ++fr) {
                TestFrame();
                if (fr > 1 && g_marked[0].load() + g_marked[1].load() == 0) everEmpty = true;  // the stream must be continuous
                const int c = fr % kC;
                for (const Batch& b : g_q)
                    if (b.comp == sc[c]) ok = false;  // still queued: the bound did not hold
                memset(sc[c], 0, sizeof sc[c]);
                const double bounds[7] = {0, 0, 0, 100, 100, 100, 173};
                memcpy(sc[c] + kBoundsOff, bounds, sizeof bounds);
                *reinterpret_cast<uint8_t**>(sc[c] + 0xB0) = world;
                *reinterpret_cast<uint8_t***>(sc[c] + 0x6E8) = sarr[c];
                *reinterpret_cast<int32_t*>(sc[c] + 0x6F0) = kB;
                for (int i = 0; i < kB; ++i) {
                    memset(sb[c][i], 0, 0x200);
                    *reinterpret_cast<DelFn**>(sb[c][i]) = t_vtbl;
                    *reinterpret_cast<uint8_t**>(sb[c][i] + 0x158) = sa[c][i];
                    *reinterpret_cast<uint8_t**>(sb[c][i] + 0x160) = sc[c];
                    *reinterpret_cast<const void***>(sa[c][i]) = t_proxyVtOk;
                    *reinterpret_cast<uint8_t**>(sa[c][i] + 8) = solver;
                    *reinterpret_cast<uint8_t**>(sa[c][i] + 0x10) = spp[c][i];
                    *reinterpret_cast<uint8_t**>(sa[c][i] + 0x18) = spt[c][i];
                    sarr[c][i] = sb[c][i];
                }
                ++t_inRemoval;
                ClearHook(sc[c]);
                --t_inRemoval;
                if (g_pending == 0) ok = false;  // not queued
                for (const Batch& b : g_q)
                    for (size_t i = b.next; i < b.bodies.size(); ++i) {
                        const void *gt, *pt;
                        KeysForTerm(b.bodies[i], &gt, &pt);
                        if (!Hidden(gt) || !Hidden(pt)) ok = false;
                    }
            }
            for (int k = 0; k < 20 && g_pending; ++k) TestFrame();
            const uint64_t full = s.hideFull.take(), sw = s.tabSwitch.take(), cl = s.tabClear.take();
            if (!ok || everEmpty || full || sw < 4 || cl < 3 || g_pending || g_marked[0].load() + g_marked[1].load() != 0 ||
                t_hiddenAtDel || t_shownAtTerm) {
                fail |= 1 << 27;
                printf("    BodyRemovalSpread stream test: ok %d, ever empty %d, set full %llu, switches %llu, clears %llu, pending "
                       "%zu, marked %zu / %zu, used %zu / %zu\n", ok, everEmpty, static_cast<unsigned long long>(full),
                       static_cast<unsigned long long>(sw), static_cast<unsigned long long>(cl), g_pending, g_marked[0].load(),
                       g_marked[1].load(), g_tab[0].used, g_tab[1].used);
            }
            g_switchAt = kHiddenCap / 4;
            g_hiddenLimit = kHiddenCap / 2;
            g_perFrame = 100;
            g_maxFrames = 10;
        }
        g_filterOn = false;
        g_particleOff = 0;
        g_ptHandleOff = 0;
        g_origPreFilter = nullptr;
        g_origPreFilterPT = nullptr;
        t_termPart.clear();
        memcpy(g_proxyVtOk, savedOk, sizeof savedOk);
        memcpy(g_proxyVtBad, savedBad, sizeof savedBad);
        s.hidden.take();
        s.hiddenPT.take();
        s.hideFull.take();
        s.hideNoKey.take();
        s.hiddenGone.take();
        s.preCalls.take();
        s.preCallsPT.take();
        s.maxMarked.take();
        s.tabSwitch.take();
        s.tabClear.take();
    }
    // 18. the frozen-game-thread flag (S1 diagnostics): watched from the solver's first clear, a change counted at the
    // next frame start, unwatched when the solver is destroyed
    {
        s.flagChanges.take();
        solver[0x1D0] = 0;
        g_lastSolver = nullptr;
        for (Watched& w : g_watch) w = {};
        log = TestLog{};
        setup(1, 20);  // below the minimum: stock, but the solver is met
        removal(1);
        bool seen = false;
        for (const Watched& w : g_watch) seen = seen || (w.solver == solver && w.flag == 0);
        TestFrame();
        const uint64_t before = s.flagChanges.take();
        solver[0x1D0] = 1;
        TestFrame();
        const uint64_t changed = s.flagChanges.take();
        TestFrame();
        const uint64_t again = s.flagChanges.take();
        DestroySolverHook(solver);
        bool gone = true;
        for (const Watched& w : g_watch) gone = gone && w.solver != solver;
        solver[0x1D0] = 0;
        TestFrame();
        if (!seen || before || changed != 1 || again || !gone || s.flagChanges.take()) fail |= 1 << 28;
        for (Watched& w : g_watch) w = {};
        g_lastSolver = nullptr;
    }
    // 12. no queued body's TermBody saw its owner (it would read the component's world, crash in run r1)
    if (t_ownerSeen) fail |= 4096;
    t_queuedPhase = false;
    g_q.clear();
    g_pending = 0;
    g_mode = sm;
    g_perFrame = sp;
    g_maxFrames = smf;
    g_min = smn;
    g_nearM = snm;
    g_installed = wasInstalled;
    g_playerPos = savedPos;
    g_sceneVtOk = savedVt;
    UseFound(saved);
    g_found = saved;
    t_log = nullptr;
    return fail;
}

}  // namespace opf
