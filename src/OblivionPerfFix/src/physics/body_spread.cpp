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
// static body of the physics scene (both acceleration structures, the solver) until it is terminated: scene queries and
// collisions far from the player can still meet it for those frames (that is the accepted difference). Its FBodyInstance
// is owned by the queue (the component no longer points to it). What TermBody needs from the component is only the
// component-map removal (done at the original moment, in stock order) and the collision-event unregistration (components
// with a notifying body are left to stock). So the component may be unregistered, re-registered (new bodies; the queued
// old ones go on their own) or garbage-collected while its bodies wait: TermBody's weak OwnerComponent then reads null and
// skips the maps (already done); TermBody reads nothing else of the component (BodySetup / ExternalCollisionProfileBodySetup
// are only cleared; ISM instance bodies have no DOFConstraint); the shapes' geometry is refcounted by the particle. A hit
// on such a body reports Component = null, as FHitResult's weak Component can for any hit. The physics scene: the
// FPhysScene_Chaos deleting destructor's vtable slot (UWorld::FinishDestroy's ReleasePhysicsScene, after
// OnPreWorldFinishDestroy and WaitForAllAsyncTraceTasks: no trace runs, the scene is whole) is wrapped: that scene's queued
// bodies are terminated there first (PhysicsSolverBase.cpp DestroySolver then flushes the pending unregister commands, its
// designed path). As a fallback (slot not found / another scene class) FPhysicsSolverBase::DestroySolver's one call (from
// ~FChaosScene) is wrapped too: queued bodies of that solver are then dropped untouched (their memory leaks; never
// released into a destroyed scene) and logged. Only the deletes' order and frame differ otherwise.
// The weak-pointer clear (TermQueued) is one 8-byte store outside the scene lock: an async trace dispatched mid-frame that
// meets the body in the ~1 us before TermBody's removal reports Component = null (as above); the post-actor tick is the
// quietest point for that (the frame's last trace batch is dispatched after it).
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
// from the player, bounds, class, owner and level package (World Partition cell / HLOD / data layer). Lab only also:
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
// mov edx,sizeof(T); mov rcx,rdi; call operator delete. FPhysScene_Chaos' (exe+0x351A8D0, vtable exe+0x775AEE0 slot 4) is
// the one whose ~T (exe+0x3514130) tail-jumps to ~FChaosScene (exe+0x1A9BC40), which calls the function holding kDsSite.
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
        outBodies, nearComps, nearBodies, noPos, sceneTerm, startDrains;
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

// A queued body terminated as ClearAllInstanceBodies' loop does (TermBody(false), then its deleting destructor), with its
// OwnerComponent weak pointer cleared first. TermBody's component part is only the component-map removal (done when the
// body was queued, in stock order) and the collision-event unregistration (notifying components are never queued); and it
// reads the component's world with no null check (exe+0x34FC8B0: mov rsi,[rax+228h]), which a component of a level that
// has left the world no longer has (crash in run r1, 2026-11-07). So the weak pointer is set to null (index 0, serial 0:
// what the game's Get treats as null and what TermBody itself stores at its end; one aligned 8-byte store) and TermBody
// goes straight to ReleaseActor, which finds the scene through the actor's own solver.
void TermQueued(uint8_t* body)
{
    *reinterpret_cast<volatile uint64_t*>(body + g_weakOff) = 0;
    g_term(body, false);
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
        if (isNear) eligible = false;
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
            it = g_q.erase(it);
        } else {
            ++it;
        }
    }
    g_pending -= dropped;
    ReleaseSRWLockExclusive(&g_lock);
    if (dropped) {
        s.dropped.add(dropped);
        Log("[bodyspread] solver %p destroyed with %zu queued bodies: dropped (fallback; not released into a dying scene)",
            solver, dropped);
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
        "mode %d, per frame %d, max %d frames, min %d, near %d m (live; drains at NpcCreationPacing's post-actor tick)",
        rva(g_found.destroySite), rva(g_found.clear), rva(g_found.unregSite), rva(g_found.unreg), rva(g_found.dsSite),
        rva(g_found.destroySolver), rva(g_found.sceneDel), rva(g_found.sceneSlot),
        g_found.sceneSlot ? "" : ": not found, a dying scene's queued bodies are dropped", rva(g_found.term),
        rva(g_found.maps), g_arrOff, g_handleOff, g_weakOff, g_notifyOff, g_worldOff, g_sceneOff, g_solverOff, g_hackOff,
        g_mode.load(), g_perFrame.load(), g_maxFrames.load(), g_min.load(), g_nearM.load());
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
        static_cast<uint32_t>(reinterpret_cast<uint8_t*>(f.sceneSlot) - game.base) != 0x775AEE0u + 0x20u) {
        printf("    BodyRemovalSpread: scene destructor exe+0x%X, slot exe+0x%X (expected exe+0x351A8D0, exe+0x775AF00)\n",
               static_cast<unsigned>(f.sceneDel - game.base),
               static_cast<unsigned>(reinterpret_cast<uint8_t*>(f.sceneSlot) - game.base));
        return 3;
    }
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
void TestTerm(uint8_t* body, bool)
{
    if (t_queuedPhase && *reinterpret_cast<uint8_t**>(body + 0x160)) ++t_ownerSeen;
    t_log->terms.push_back(body);
    *reinterpret_cast<uint8_t**>(body + 0x160) = nullptr;
}
void* TestDel(void* obj, uint32_t)
{
    t_log->dels.push_back(obj);
    return obj;
}
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
