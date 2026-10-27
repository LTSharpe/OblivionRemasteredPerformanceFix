// Diagnostic TickSplit ([Fixes] HitchLedgerTickSplit, default 0 = not installed; needs HitchLedger=1 at start-up; public
// since 1.2.3, lab since 2026-10-16): splits the game thread's tick work by tick-function kind and ticked class, so the
// ledger's single "TG_PrePhysics" number can be ranked by cause (tools/tick_split.py reads the csv). Start-up only,
// version.dll route (the OBSE64 route logs it as not installed).
//
// What is timed (game thread only; no behaviour change, the hooks only read the clock around the game's own calls):
//  - every FTickFunction::ExecuteTick run by the tick task (TGraphTask<FTickFunctionTask>::ExecuteTask exe+0x37C18C0,
//    its `call r10` at exe+0x37C1957 redirected): keyed by the kind (vtable[1] = the kind's ExecuteTick), the class of
//    the ticked object (FTickFunction + 0x28 = Target for FActorTickFunction, FActorComponentTickFunction and the
//    engine's other UObject-target tick functions; checked once per kind), the tick group being released
//    (FTickTaskSequencer::ReleaseTickGroup, from the ledger's hook) and the function's actual start group.
//    Exclusive time: a tick run inside another tick (the game thread helping while it waits) is subtracted from the
//    outer one. Ticks on other threads (bRunOnAnyThread) are counted per kind only, separately;
//  - every FTickableGameObject::Tick run by FTickableGameObject::TickObjects (its `call [rax]` at exe+0x37CD3F4),
//    keyed by the object's vtable (group 16 = "tickable");
//  - the actor phase's other work in UWorld::Tick, as buckets: SetupPhysicsTickFunctions, StartFrame (the gap from it to
//    TG_PrePhysics: queueing every tick function), EnsureCollisionTreeIsBuilt, ProcessLatentActions, the timer manager,
//    TickObjects (all tickables), the camera update (the gap from TickObjects to the streaming update or
//    TG_PostUpdateWork), InternalUpdateStreamingState, and the start of the actor phase (OnWorldPreActorTick ->
//    SetupPhysicsTickFunctions: the old-engine pairing session and the level list).
// Output: OblivionPerfFix-ticksplit.csv next to the log (previous session kept as .1.csv), written by the monitor thread:
// every 2 s window one W row (frames, the ledger's tick-group and actor-phase totals over all frames, the
// buckets) plus one E row per (kind, class, group) and one O row per off-thread kind; one T row per slow ledger frame
// (its top 3 entries by game-thread time). Class names come from the FName pool (found by a scan of .data on the
// monitor thread); kinds are written as the RVA of their ExecuteTick (named by tools/tick_split.py).
// Game thread: a fixed table per window (two, swapped at the summary), no locks, no allocation, no file I/O.
// Cost: off (the default) nothing is installed. On: one clock read pair per tick function, tickable and timer on the
// game thread (~45-50 ns each in the self-test; +0.44 ms per frame, within the noise, in a held crowd of ~1,300 timed calls
// per frame, 2026-10-16); the csv grows ~0.3 MB per minute and stops at 64 MB per session.
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace opf {
namespace {

// ---- patterns (build 19115871) ----
// TGraphTask<FTickFunctionTask>::ExecuteTask: movss [rdx+24h],xmm1; lea rdi,[rbx+80h]; movaps xmm1,xmm2;
// mov [rsp+20h],rdi; mov r9d,esi; call r10 (= Target->ExecuteTick); jmp
constexpr char kExecSite[] = "F3 0F 11 4A 24 48 8D BB ?? ?? ?? ?? 0F 28 CA 48 89 7C 24 20 44 8B CE 41 FF D2 EB";
constexpr size_t kExecAt = 0x14;  // mov r9d,esi; call r10 (6 bytes)
// FTickableGameObject::TickObjects: ... call [rax+20h] (GetStatId); mov rax,[rbx]; movaps xmm1,xmm6; mov rcx,rbx;
// call [rax] (= Tick); cmp qword [rdi],0
constexpr char kTickableSite[] = "48 8B 03 48 8D 55 ?? 48 8B CB FF 50 20 48 8B 03 0F 28 CE 48 8B CB FF 10 48 83 3F 00";
constexpr size_t kTickableAt = 0x0D;  // 11 bytes
// UWorld::Tick's direct calls (exe+0x332EFF0)
constexpr char kSetupSite[] = "0F 28 CE 48 8B CF E8 ?? ?? ?? ?? 44 88 A7 ?? ?? 00 00 E8";  // SetupPhysicsTickFunctions
constexpr size_t kSetupAt = 0x06;
constexpr char kCollSite[] = "80 A7 ?? ?? 00 00 FB E8 ?? ?? ?? ?? 80 8F ?? ?? 00 00 04";  // EnsureCollisionTreeIsBuilt
constexpr size_t kCollAt = 0x07;
constexpr char kLatentSite[] = "0F 28 D6 33 D2 48 8B CE E8 ?? ?? ?? ?? 8B 5C 24";  // ProcessLatentActions(nullptr, dt)
constexpr size_t kLatentAt = 0x08;
// GetTimerManager; FTimerManager::Tick(dt); FTickableGameObject::TickObjects(world, type, paused, dt); GetPlayerControllerIterator
constexpr char kTimerSite[] =
    "48 8B CF E8 ?? ?? ?? ?? 0F 28 CE 48 8B C8 E8 ?? ?? ?? ?? 0F 28 DE 45 0F B6 C5 8B D3 48 8B CF E8 ?? ?? ?? ?? 48 8D 55";
constexpr size_t kTimerAt = 0x0E, kTickObjAt = 0x1F;
// if (!paused && IsGameWorld) InternalUpdateStreamingState
constexpr char kStreamSite[] = "45 84 ED 75 ?? 48 8B CF E8 ?? ?? ?? ?? 84 C0 74 08 48 8B CF E8";
constexpr size_t kStreamAt = 0x14;
// FTimerManager::Tick (exe+0x37FACA0), its inlined FTimerUnifiedDelegate::Execute (rbx = the FTimerData, 0x90 bytes;
// +0x78 = TimerIndicesByObjectKey, the object the timer was set for):
//   native delegate: cmp dword [rbx+18h],0; je; mov rcx,[rbx+10h]; mov rax,[rcx]; call [rax+58h] (Execute); jmp
constexpr char kTimerNative[] = "83 7B 18 00 74 ?? 48 8B 4B 10 48 8B 01 FF 50 58 E9";
constexpr size_t kTimerNativeAt = 0x06;  // 10 bytes
//   dynamic delegate: mov r9,[rbx]; xor r8d,r8d; mov rdx,rax; mov rcx,rbx; call [r9+ProcessEvent]; mov edi,r14d
constexpr char kTimerDynamic[] = "4C 8B 0B 45 33 C0 48 8B D0 48 8B CB 41 FF 91 ?? ?? 00 00 41 8B FE";
constexpr size_t kTimerDynamicAt = 0x0C;  // 7 bytes
//   TFunction: mov rax,[rcx]; call [rax+8] (the callable); mov rcx,rax; call rsi (its invoker); lea rsi,[r12+8]
constexpr char kTimerCallback[] = "48 8B 01 FF 50 08 48 8B C8 FF D6 49 8D 74 24 08";
constexpr size_t kTimerCallbackAt = 0x06;  // 5 bytes
constexpr size_t kTimerObjectOffset = 0x78;

constexpr size_t kTargetOffset = 0x28;   // sizeof(FTickFunction) in 5.3: the first member of the derived tick functions
constexpr size_t kInternalOffset = 0x20; // FTickFunction::InternalData (TUniquePtr); +1 = ActualStartTickGroup
constexpr size_t kSlots = 4096;          // entries per window (power of two); full at 3/4 -> "overflow"
constexpr size_t kTouched = 2048;        // entries remembered per frame for the slow frame's top 3
constexpr int kDepth = 32;
constexpr int kOffKinds = 64;
constexpr size_t kTRing = 512;
constexpr uint32_t kGroupOutside = 15, kGroupTickable = 16, kGroupTimerNative = 17, kGroupTimerDynamic = 18,
                   kGroupTimerCallback = 19;
constexpr uint64_t kWindowUs = 2000000;  // window length (handed to the monitor thread at the first frame end after it)

enum Bucket { kActorStart, kSetup, kStartFrame, kCollTree, kLatent, kTimers, kTickables, kCamera, kStreaming, kBuckets };
const char* const kBucketName[kBuckets] = {"actorstart", "setup", "startframe", "colltree", "latent",
                                           "timers",     "tickables", "camera", "streaming"};

using ExecFn = void (*)(void* tf, float dt, int type, int thread, const void* ev);
using TickableFn = void (*)(void* obj, float dt);
using WorldFloatFn = void (*)(void* world, float dt);
using WorldFn = void (*)(void* world);
using LatentFn = void (*)(void* manager, void* object, float dt);
using TickObjectsFn = void (*)(void* world, int type, bool paused, float dt);

struct Entry {
    const void* fn;   // the kind's ExecuteTick (tickables: their Tick)
    const void* cls;  // the ticked object's UClass (tickables: their vtable; 0 = not a UObject kind)
    uint32_t grp;     // released group | actual start group << 8 (0xFF = unknown)
    uint32_t calls;
    uint64_t ticks, maxTicks;
    uint64_t frameTicks, frameStamp;
};

// Perception (worker percsplit): only the timer's delegate establishes an update scope. The redirected calls in
// AVPairedPawn's update/LOS then accumulate on the GT, never in unrelated callers of the shared UE functions.
// Inclusive parents: whole, los. Disjoint partition: list + ignore + trace1/2/3 + los_rest + commands + remainder.
enum PercStage { pWhole, pList, pLos, pIgnore, pTrace1, pTrace2, pTrace3, pLosRest, pCommands, pRemainder, pStages };
const char* const kPercNames[pStages] = {"whole", "list", "los", "ignore", "trace1", "trace2", "trace3",
                                        "los_rest", "commands", "remainder"};
struct PercStat { uint64_t calls, ticks, maxTicks; };
struct PercAcc {
    PercStat stage[pStages];
    uint64_t frames, checksByTraces[4], visible, invisible;
    uint64_t hit[3], blocking[3], penetrating[3];
};
struct PercContext { PercAcc acc{}; };
struct PercLosContext { uint64_t ignore = 0, trace = 0; unsigned traces = 0; };
PercAcc g_percFrame{};
std::atomic<bool> g_percOn{false};  // a finer split of one timer's work (off)

struct Window {
    Entry e[kSlots];
    uint32_t used;
    uint64_t overflowCalls, overflowTicks;
    uint64_t frames, slowFrames, startUs, endUs;
    uint64_t tgUs[8], actorUs, pairUs, periodUs;
    uint64_t bucket[kBuckets];
    uint64_t execCalls, execTicks;  // game-thread totals (all entries + overflow)
    PercAcc perc;
};

struct OffKind {
    std::atomic<const void*> fn{nullptr};
    std::atomic<uint64_t> calls{0}, ticks{0};
};

struct Top {
    const void* fn;
    const void* cls;
    uint32_t grp;
    uint32_t us;
};
struct TRec {
    uint64_t frame, endUs;
    uint32_t periodUs;
    Top top[3];
    uint32_t bucketUs[kBuckets];
    uint32_t execUs, execCalls;
};

struct Sites {
    uint8_t *exec, *tickable, *setup, *coll, *latent, *timer, *tickObj, *stream, *tNative, *tDynamic, *tCallback;
    uint32_t nativeSlot, processEventSlot;
    uint8_t* region;
};

Sites st{};
bool g_installed = false;
std::atomic<bool> g_on{false};
LARGE_INTEGER g_freq{};
uint64_t (*g_testClock)() = nullptr;
WorldFloatFn g_setupOrig = nullptr;
WorldFn g_collOrig = nullptr, g_streamOrig = nullptr;
uint32_t g_nativeSlot = 0x58, g_peSlot = 0x268;
LatentFn g_latentOrig = nullptr;
WorldFloatFn g_timerOrig = nullptr;  // FTimerManager::Tick(this, dt): same shape
TickObjectsFn g_tickObjOrig = nullptr;
std::wstring g_path;

// game thread
Window g_win[2];
int g_active = 0;
uint64_t g_frameNo = 1;
Entry* g_touched[kTouched];
uint32_t g_nTouched = 0;
uint64_t g_child[kDepth];
int g_depth = 0;
uint32_t g_curGroup = kGroupOutside;
uint64_t g_preTs = 0, g_setupEnd = 0, g_camTs = 0;
uint64_t g_frameBucket[kBuckets];
uint64_t g_frameExecTicks = 0, g_frameExecCalls = 0;
struct KindState { const void* fn; int state; };  // state: 1 = UObject target, 2 = not, 0 = unknown
KindState g_kinds[256];
Entry g_overflowDummy;

// shared
std::atomic<int> g_pending{-1};  // the window the monitor thread may read (GT hands it over at the summary)
OffKind g_off[kOffKinds];
std::atomic<uint64_t> g_offOverflowCalls{0}, g_offOverflowTicks{0};
TRec g_tring[kTRing];
std::atomic<uint64_t> g_thead{0}, g_ttail{0};
std::atomic<uint32_t> g_tdropped{0};

// monitor thread
FILE* g_file = nullptr;
bool g_fileFailed = false;
long g_fileBytes = 0;
constexpr long kMaxFileBytes = 64L << 20;
uint64_t g_offLastCalls[kOffKinds], g_offLastTicks[kOffKinds];
const uint8_t* g_pool = nullptr;
bool g_poolSearched = false;
std::unordered_map<const void*, std::string> g_names;

inline uint64_t Now()
{
    if (g_testClock) return g_testClock();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
double TicksMs(uint64_t t) { return static_cast<double>(t) * 1000.0 / static_cast<double>(g_freq.QuadPart); }
uint32_t TicksUs(uint64_t t) { const double us = TicksMs(t) * 1000.0; return us > 4e9 ? 0xFFFFFFFFu : static_cast<uint32_t>(us); }

inline bool Active() { return g_on.load(std::memory_order_relaxed) && LedgerIsGameThread(); }

// A profiler zone around a timed call (nothing in this build unless the lab defines it).
#define OPF_TICK_ZONE(loc) ((void)0)



void PercMerge(PercAcc& a, const PercAcc& b)
{
    for (int s = 0; s < pStages; ++s) {
        a.stage[s].calls += b.stage[s].calls;
        a.stage[s].ticks += b.stage[s].ticks;
        if (b.stage[s].maxTicks > a.stage[s].maxTicks) a.stage[s].maxTicks = b.stage[s].maxTicks;
    }
    a.frames += b.frames;
    a.visible += b.visible;
    a.invisible += b.invisible;
    for (int i = 0; i < 4; ++i) a.checksByTraces[i] += b.checksByTraces[i];
    for (int i = 0; i < 3; ++i) {
        a.hit[i] += b.hit[i];
        a.blocking[i] += b.blocking[i];
        a.penetrating[i] += b.penetrating[i];
    }
}


// Validates once per kind that FTickFunction + 0x28 points to a UObject: its class's class is the UClass metaclass
// (whose class is itself). Kinds with another target (e.g. a manager struct) get no class.
int ProbeKind(void* tf)
{
    __try {
        const uint8_t* const obj = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(tf) + kTargetOffset);
        if (!obj) return 0;
        // the object's class, its class (UClass, or UBlueprintGeneratedClass for a Blueprint class), ...: within three
        // steps a class whose class is itself (UClass)
        const uint8_t* c = *reinterpret_cast<uint8_t* const*>(obj + 0x10);
        for (int i = 0; i < 4 && c; ++i) {
            const uint8_t* const next = *reinterpret_cast<uint8_t* const*>(c + 0x10);
            if (next == c) return 1;
            c = next;
        }
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

const void* ClassOf(void* tf, const void* fn)
{
    size_t h = (reinterpret_cast<uintptr_t>(fn) >> 4) & 255;
    for (int i = 0; i < 256; ++i, h = (h + 1) & 255) {
        KindState& k = g_kinds[h];
        if (k.fn == fn || !k.fn) {
            if (!k.fn) k.fn = fn;
            if (!k.state) k.state = ProbeKind(tf);
            if (k.state != 1) return nullptr;
            const uint8_t* const obj = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(tf) + kTargetOffset);
            return obj ? *reinterpret_cast<const void* const*>(obj + 0x10) : nullptr;
        }
    }
    return nullptr;
}

Entry* Slot(const void* fn, const void* cls, uint32_t grp)
{
    Window& w = g_win[g_active];
    size_t h = ((reinterpret_cast<uintptr_t>(fn) >> 3) * 0x9E3779B97F4A7C15ull ^ (reinterpret_cast<uintptr_t>(cls) >> 3) * 0xC2B2AE3D27D4EB4Full ^ grp) >> 52;
    h &= kSlots - 1;
    for (int probe = 0; probe < 64; ++probe, h = (h + 1) & (kSlots - 1)) {
        Entry& e = w.e[h];
        if (e.fn == fn && e.cls == cls && e.grp == grp) return &e;
        if (!e.fn) {
            if (w.used >= kSlots * 3 / 4) break;
            e.fn = fn;
            e.cls = cls;
            e.grp = grp;
            ++w.used;
            return &e;
        }
    }
    return nullptr;
}

void Add(const void* fn, const void* cls, uint32_t grp, uint64_t dt)
{
    Window& w = g_win[g_active];
    w.execCalls++;
    w.execTicks += dt;
    g_frameExecTicks += dt;
    g_frameExecCalls++;
    Entry* const e = Slot(fn, cls, grp);
    if (!e) {
        w.overflowCalls++;
        w.overflowTicks += dt;
        return;
    }
    e->calls++;
    e->ticks += dt;
    if (dt > e->maxTicks) e->maxTicks = dt;
    if (e->frameStamp != g_frameNo) {
        e->frameStamp = g_frameNo;
        e->frameTicks = 0;
        if (g_nTouched < kTouched) g_touched[g_nTouched++] = e;
    }
    e->frameTicks += dt;
}

void OffAdd(const void* fn, uint64_t dt)
{
    size_t h = (reinterpret_cast<uintptr_t>(fn) >> 4) & (kOffKinds - 1);
    for (int i = 0; i < kOffKinds; ++i, h = (h + 1) & (kOffKinds - 1)) {
        OffKind& k = g_off[h];
        const void* cur = k.fn.load(std::memory_order_acquire);
        if (!cur) {
            const void* expected = nullptr;
            if (k.fn.compare_exchange_strong(expected, fn, std::memory_order_acq_rel)) cur = fn;
            else cur = expected;
        }
        if (cur == fn) {
            k.calls.fetch_add(1, std::memory_order_relaxed);
            k.ticks.fetch_add(dt, std::memory_order_relaxed);
            return;
        }
    }
    g_offOverflowCalls.fetch_add(1, std::memory_order_relaxed);
    g_offOverflowTicks.fetch_add(dt, std::memory_order_relaxed);
}

// ---- hooks ----
void ExecHook(void* tf, float dt, int type, int thread, const void* ev)
{
    const ExecFn fn = (*reinterpret_cast<ExecFn* const*>(tf))[1];
    if (!g_on.load(std::memory_order_relaxed)) {
        return fn(tf, dt, type, thread, ev);
    }
    if (!LedgerIsGameThread()) {
        const uint64_t t0 = Now();
        fn(tf, dt, type, thread, ev);
        OffAdd(reinterpret_cast<const void*>(fn), Now() - t0);
        return;
    }
    const void* const cls = ClassOf(tf, reinterpret_cast<const void*>(fn));
    OPF_TICK_ZONE(TickZones() ? TickLocation(reinterpret_cast<const void*>(fn), cls, false) : nullptr);
    uint32_t actual = 0xFF;
    if (const uint8_t* const internal = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(tf) + kInternalOffset))
        actual = internal[1];
    const uint32_t grp = g_curGroup | actual << 8;
    if (g_depth >= kDepth) return fn(tf, dt, type, thread, ev);
    const int d = g_depth++;
    g_child[d] = 0;
    const uint64_t t0 = Now();
    fn(tf, dt, type, thread, ev);
    const uint64_t incl = Now() - t0;
    g_depth = d;
    const uint64_t child = g_child[d];
    if (d > 0) g_child[d - 1] += incl;
    Add(reinterpret_cast<const void*>(fn), cls, grp, incl > child ? incl - child : 0);
}

void TickableHook(void* obj, float dt)
{
    const TickableFn fn = (*reinterpret_cast<TickableFn* const*>(obj))[0];
    OPF_TICK_ZONE(TickZones() && LedgerIsGameThread()
                      ? TickLocation(reinterpret_cast<const void*>(fn), *reinterpret_cast<void* const*>(obj), true)
                      : nullptr);
    if (!Active() || g_depth >= kDepth) return fn(obj, dt);
    const int d = g_depth++;
    g_child[d] = 0;
    const uint64_t t0 = Now();
    fn(obj, dt);
    const uint64_t incl = Now() - t0;
    g_depth = d;
    const uint64_t child = g_child[d];
    if (d > 0) g_child[d - 1] += incl;
    Add(reinterpret_cast<const void*>(fn), *reinterpret_cast<void* const*>(obj), kGroupTickable | 0xFF00u,
        incl > child ? incl - child : 0);
}


void SetupHook(void* world, float dt)
{
    OPF_TICK_ZONE(BucketZone(kSetup));
    if (!Active()) return g_setupOrig(world, dt);
    const uint64_t t0 = Now();
    if (g_preTs) { g_frameBucket[kActorStart] += t0 - g_preTs; g_preTs = 0; }
    g_setupOrig(world, dt);
    const uint64_t t1 = Now();
    g_frameBucket[kSetup] += t1 - t0;
    g_setupEnd = t1;
}

void CollHook(void* world)
{
    OPF_TICK_ZONE(BucketZone(kCollTree));
    if (!Active()) return g_collOrig(world);
    const uint64_t t0 = Now();
    g_collOrig(world);
    g_frameBucket[kCollTree] += Now() - t0;
}

void LatentHook(void* manager, void* object, float dt)
{
    OPF_TICK_ZONE(BucketZone(kLatent));
    if (!Active()) return g_latentOrig(manager, object, dt);
    const uint64_t t0 = Now();
    g_latentOrig(manager, object, dt);
    g_frameBucket[kLatent] += Now() - t0;
}

void TimerHook(void* manager, float dt)
{
    OPF_TICK_ZONE(BucketZone(kTimers));
    if (!Active()) return g_timerOrig(manager, dt);
    const uint64_t t0 = Now();
    g_timerOrig(manager, dt);
    g_frameBucket[kTimers] += Now() - t0;
}

void TickObjectsHook(void* world, int type, bool paused, float dt)
{
    OPF_TICK_ZONE(BucketZone(kTickables));
    if (!Active()) return g_tickObjOrig(world, type, paused, dt);
    const uint64_t t0 = Now();
    g_tickObjOrig(world, type, paused, dt);
    const uint64_t t1 = Now();
    g_frameBucket[kTickables] += t1 - t0;
    g_camTs = t1;
}

// Times one call on the game thread as an entry (exclusive of ticks/timers nested in it).
template <typename F>
void Timed(const void* fn, const void* cls, uint32_t grp, F&& call)
{
    if (!Active() || g_depth >= kDepth) return call();
    const int d = g_depth++;
    g_child[d] = 0;
    const uint64_t t0 = Now();
    call();
    const uint64_t incl = Now() - t0;
    g_depth = d;
    const uint64_t child = g_child[d];
    if (d > 0) g_child[d - 1] += incl;
    Add(fn, cls, grp, incl > child ? incl - child : 0);
}

const void* SafeClass(const void* obj)
{
    if (!obj || (reinterpret_cast<uintptr_t>(obj) & 7) || reinterpret_cast<uintptr_t>(obj) < 0x10000 ||
        reinterpret_cast<uintptr_t>(obj) >> 47)
        return nullptr;
    __try {
        return *reinterpret_cast<const void* const*>(static_cast<const uint8_t*>(obj) + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

const void* TimerClass(const void* timerData)
{
    if (!Active()) return nullptr;
    return SafeClass(*reinterpret_cast<const void* const*>(static_cast<const uint8_t*>(timerData) + kTimerObjectOffset));
}

using DelegateExecFn = void (*)(void* instance);
using ProcessEventFn = void (*)(void* obj, void* func, void* parms);
using InvokerFn = void (*)(void* callable);

// The bound method of a UObject method delegate instance (TBaseUObjectMethodDelegateInstance: +0x18 the weak object,
// +0x20 the method, +0x28 the this-adjustment; its Execute exe+0x1BB6B30 reads them so), if it points into the game's
// code; else nullptr (other instance kinds are keyed by their vtable).
const void* BoundMethod(const void* instance)
{
    __try {
        const void* const m = *reinterpret_cast<const void* const*>(static_cast<const uint8_t*>(instance) + 0x20);
        return Game().text.Contains(m) ? m : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void TimerNativeHook(void* instance, void*, void* timerData)
{
    const DelegateExecFn fn = *reinterpret_cast<DelegateExecFn*>(*static_cast<uint8_t**>(instance) + g_nativeSlot);
    const void* key = *static_cast<void**>(instance);
    if (Active())
        if (const void* m = BoundMethod(instance)) key = m;
    Timed(key, TimerClass(timerData), kGroupTimerNative | 0xFF00u, [&] {
            fn(instance);
    });
}

void TimerDynamicHook(void* obj, void* func, void* parms)
{
    const ProcessEventFn fn = *reinterpret_cast<ProcessEventFn*>(*static_cast<uint8_t**>(obj) + g_peSlot);
    Timed(func, Active() ? SafeClass(obj) : nullptr, kGroupTimerDynamic | 0xFF00u, [&] { fn(obj, func, parms); });
}

void TimerCallbackHook(void* callable, void* invoker, void* timerData)
{
    Timed(invoker, TimerClass(timerData), kGroupTimerCallback | 0xFF00u, [&] { reinterpret_cast<InvokerFn>(invoker)(callable); });
}

void StreamHook(void* world)
{
    OPF_TICK_ZONE(BucketZone(kStreaming));
    if (!Active()) return g_streamOrig(world);
    const uint64_t t0 = Now();
    if (g_camTs) { g_frameBucket[kCamera] += t0 - g_camTs; g_camTs = 0; }
    g_streamOrig(world);
    g_frameBucket[kStreaming] += Now() - t0;
}

// ---- install helpers ----
uint8_t* FindOne(const GameModule& game, const char* pattern, const char* what)
{
    const auto hits = FindAll(game.text, ParsePattern(pattern));
    if (hits.size() == 1) return hits[0];
    Log("[ticksplit] %s %s: not timed", what, hits.empty() ? "not found" : "ambiguous");
    return nullptr;
}

// A stub at slot: jmp [rip+0] -> hook (14 bytes)
void JmpStub(uint8_t* slot, void* hook)
{
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(slot, jmpAbs, 6);
    memcpy(slot + 6, &hook, 8);
}

// Redirects the rel32 call at `call` to a stub at slot (-> hook); returns the original target (nullptr on failure).
uint8_t* PrepareCall(const GameModule& game, uint8_t* call, uint8_t* slot, void* hook)
{
    if (!call || call[0] != 0xE8) return nullptr;
    uint8_t* const target = RipTarget(call, 1, 5);
    if (!game.text.Contains(target)) return nullptr;
    JmpStub(slot, hook);
    return target;
}

bool RedirectCall(uint8_t* call, uint8_t* slot)
{
    const int64_t rel = slot - (call + 5);
    const int32_t rel32 = static_cast<int32_t>(rel);
    return rel == rel32 && WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}

// ---- names (monitor thread) ----
// FNamePool: FNameEntryAllocator {FRWLock (8), uint32 CurrentBlock, uint32 CurrentByteCursor, uint8* Blocks[8192]};
// block 0 starts with the entry "None" (header: bit 0 wide, bits 6.. length).
bool PoolAt(const uint8_t* p)  // p = the qword {CurrentBlock, CurrentByteCursor}
{
    __try {
        const uint64_t q = *reinterpret_cast<const uint64_t*>(p);
        const uint32_t lo = static_cast<uint32_t>(q), hi = static_cast<uint32_t>(q >> 32);
        if (lo < 1 || lo >= 8192 || hi == 0 || hi >= (1u << 18) || (hi & 1)) return false;
        const uint64_t* const blocks = reinterpret_cast<const uint64_t*>(p + 8);
        if (blocks[lo + 1] != 0) return false;
        for (uint32_t i = 0; i <= lo; ++i)
            if (blocks[i] <= 0x10000) return false;
        const uint8_t* const b0 = reinterpret_cast<const uint8_t*>(blocks[0]);
        return (*reinterpret_cast<const uint16_t*>(b0) >> 6) == 4 && memcmp(b0 + 2, "None", 4) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const uint8_t* FindNamePool()
{
    const GameModule& game = Game();
    for (const uint8_t* r = game.data.begin; r < game.data.end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(r, &mbi, sizeof mbi)) break;
        const uint8_t* const end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                              (prot == PAGE_READWRITE || prot == PAGE_READONLY || prot == PAGE_WRITECOPY);
        if (readable)
            for (const uint8_t* p = r < game.data.begin + 8 ? game.data.begin + 8 : r; p + 16 <= end && p + 16 <= game.data.end; p += 8) {
                const uint64_t q = *reinterpret_cast<const uint64_t*>(p);
                const uint32_t lo = static_cast<uint32_t>(q);
                if (lo >= 1 && lo < 8192 && (q >> 32) && PoolAt(p)) return p - 8;
            }
        r = end;
    }
    return nullptr;
}

bool ReadNameIn(const uint8_t* pool, uint32_t index, uint32_t number, char* out, size_t size)
{
    if (!pool || size < 8) return false;
    __try {
        const uint8_t* const block = *reinterpret_cast<const uint8_t* const*>(pool + 16 + (index >> 16) * 8);
        if (!block) return false;
        const uint8_t* const e = block + (index & 0xFFFF) * 2;
        const uint16_t hdr = *reinterpret_cast<const uint16_t*>(e);
        size_t n = hdr >> 6;
        if (n == 0 || n >= size - 12) return false;
        if (hdr & 1) {
            for (size_t i = 0; i < n; ++i) {
                const wchar_t c = reinterpret_cast<const wchar_t*>(e + 2)[i];
                out[i] = c < 128 && c >= 32 ? static_cast<char>(c) : '?';
            }
        } else {
            for (size_t i = 0; i < n; ++i) {
                const char c = reinterpret_cast<const char*>(e + 2)[i];
                out[i] = c >= 32 && c != ',' ? c : '?';
            }
        }
        out[n] = 0;
        if (number) snprintf(out + n, size - n, "_%u", number - 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadName(uint32_t index, uint32_t number, char* out, size_t size) { return ReadNameIn(g_pool, index, number, out, size); }

bool ReadClassIndex(const void* cls, uint32_t* index, uint32_t* number)
{
    __try {
        *index = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(cls) + 0x18);
        *number = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(cls) + 0x1C);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const std::string& NameOf(const void* cls, bool isVtable)
{
    auto it = g_names.find(cls);
    if (it != g_names.end()) return it->second;
    char buf[160];
    const GameModule& game = Game();
    if (!cls) snprintf(buf, sizeof buf, "-");
    else if (isVtable) {
        const uint8_t* const p = static_cast<const uint8_t*>(cls);
        if (p >= game.base && p < game.base + game.sizeOfImage) snprintf(buf, sizeof buf, "vt:exe+0x%X", static_cast<unsigned>(p - game.base));
        else snprintf(buf, sizeof buf, "vt:%p", cls);
    } else {
        if (!g_poolSearched) {
            g_poolSearched = true;
            g_pool = FindNamePool();
            if (g_pool) Log("[ticksplit] FName pool at exe+0x%X", static_cast<unsigned>(g_pool - game.base));
            else Log("[ticksplit] FName pool not found: classes written as addresses");
        }
        uint32_t index = 0, number = 0;
        if (!(ReadClassIndex(cls, &index, &number) && ReadName(index, number, buf, sizeof buf)))
            snprintf(buf, sizeof buf, "cls:%p", cls);
    }
    return g_names.emplace(cls, buf).first->second;
}

uint32_t Rva(const void* p)
{
    const GameModule& game = Game();
    const uint8_t* const b = static_cast<const uint8_t*>(p);
    return b >= game.base && b < game.base + game.sizeOfImage ? static_cast<uint32_t>(b - game.base) : 0;
}

// ---- csv ----
bool g_capLogged = false;
void Write(const char* s, int n)
{
    if (!g_file || n <= 0) return;
    if (g_fileBytes + n > kMaxFileBytes) {
        if (!g_capLogged) {
            g_capLogged = true;
            Log("[ticksplit] the csv reached its %ld MB cap: no more rows this session", kMaxFileBytes >> 20);
        }
        return;
    }
    fwrite(s, 1, static_cast<size_t>(n), g_file);
    g_fileBytes += n;
}

bool OpenFile()
{
    if (g_file) return true;
    if (g_fileFailed || g_path.empty()) return false;
    const std::wstring prev = g_path.substr(0, g_path.size() - 4) + L".1.csv";
    MoveFileExW(g_path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfsopen(g_path.c_str(), L"w", _SH_DENYWR);
    if (!g_file) {
        g_fileFailed = true;
        Log("[ticksplit] could not create %ls", g_path.c_str());
        return false;
    }
    char head[3072];
    const int n = snprintf(
        head, sizeof head,
        "# OblivionPerfFix-ticksplit v1, OblivionPerfFix " OPF_VERSION ", pid=%lu, qpc_freq=%lld; read with tools/tick_split.py; "
        "2 s windows; groups: 0-7 TG_PrePhysics..TG_NewlySpawned (the group being released), 15 outside a release, 16 tickable, "
        "17-19 timer delegates (17 native: fn = the bound method, else the delegate vtable; 18 dynamic: class::function; 19 TFunction: fn = invoker)\n"
        "#W,end_ms,seconds,frames,slow_frames,period_ms,actor_ms,pair_ms,tg0_ms,tg1_ms,tg2_ms,tg3_ms,tg4_ms,tg5_ms,tg6_ms,tg7_ms,"
        "exec_calls,exec_ms,overflow_calls,overflow_ms,entries,b_actorstart_ms,b_setup_ms,b_startframe_ms,b_colltree_ms,"
        "b_latent_ms,b_timers_ms,b_tickables_ms,b_camera_ms,b_streaming_ms\n"
        "#E,end_ms,group,actual_group,fn_rva,class,calls,ms,max_ms\n"
        "#O,end_ms,fn_rva,calls,ms\n"
        "#T,frame,end_ms,period_ms,exec_calls,exec_ms,{group,fn_rva,class,ms}x3,b_actorstart_ms..b_streaming_ms\n"
        "#P,end_ms,enabled_frames,stage,calls,ms,max_ms (whole/los inclusive; rest derived per call; ignore=9 gather/add calls)\n"
        "#Q,end_ms,enabled_frames,checks_0traces,checks_1trace,checks_2traces,checks_3traces,visible,invisible,"
        "trace1_hits,trace1_blocking,trace1_penetrating,trace2_hits,trace2_blocking,trace2_penetrating,"
        "trace3_hits,trace3_blocking,trace3_penetrating\n",
        GetCurrentProcessId(), static_cast<long long>(g_freq.QuadPart));
    Write(head, n);
    fflush(g_file);
    Log("[ticksplit] writing the tick split to %ls", g_path.c_str());
    return true;
}

void WriteWindow(Window& w)
{
    char line[1024];
    const double endMs = static_cast<double>(w.endUs) / 1000.0;
    int n = snprintf(line, sizeof line, "W,%.3f,%.3f,%llu,%llu,%.3f,%.3f,%.3f", endMs, (w.endUs - w.startUs) / 1e6,
                     static_cast<unsigned long long>(w.frames), static_cast<unsigned long long>(w.slowFrames), w.periodUs / 1000.0,
                     w.actorUs / 1000.0, w.pairUs / 1000.0);
    for (int i = 0; i < 8; ++i) n += snprintf(line + n, sizeof line - n, ",%.3f", w.tgUs[i] / 1000.0);
    n += snprintf(line + n, sizeof line - n, ",%llu,%.3f,%llu,%.3f,%u", static_cast<unsigned long long>(w.execCalls),
                  TicksMs(w.execTicks), static_cast<unsigned long long>(w.overflowCalls), TicksMs(w.overflowTicks), w.used);
    for (int b = 0; b < kBuckets; ++b) n += snprintf(line + n, sizeof line - n, ",%.3f", TicksMs(w.bucket[b]));
    n += snprintf(line + n, sizeof line - n, "\n");
    Write(line, n);
    if (w.perc.frames) {
        for (int s = 0; s < pStages; ++s) {
            const PercStat& v = w.perc.stage[s];
            n = snprintf(line, sizeof line, "P,%.3f,%llu,%s,%llu,%.6f,%.6f\n", endMs,
                         static_cast<unsigned long long>(w.perc.frames), kPercNames[s],
                         static_cast<unsigned long long>(v.calls), TicksMs(v.ticks), TicksMs(v.maxTicks));
            Write(line, n);
        }
        const PercAcc& a = w.perc;
        n = snprintf(line, sizeof line, "Q,%.3f,%llu", endMs, static_cast<unsigned long long>(a.frames));
        for (int i = 0; i < 4; ++i)
            n += snprintf(line + n, sizeof line - n, ",%llu", static_cast<unsigned long long>(a.checksByTraces[i]));
        n += snprintf(line + n, sizeof line - n, ",%llu,%llu", static_cast<unsigned long long>(a.visible),
                      static_cast<unsigned long long>(a.invisible));
        for (int i = 0; i < 3; ++i)
            n += snprintf(line + n, sizeof line - n, ",%llu,%llu,%llu", static_cast<unsigned long long>(a.hit[i]),
                          static_cast<unsigned long long>(a.blocking[i]), static_cast<unsigned long long>(a.penetrating[i]));
        n += snprintf(line + n, sizeof line - n, "\n");
        Write(line, n);
    }
    for (size_t i = 0; i < kSlots; ++i) {
        const Entry& e = w.e[i];
        if (!e.fn || !e.calls) continue;
        const uint32_t g = e.grp & 0xFF, a = (e.grp >> 8) & 0xFF;
        std::string name = NameOf(e.cls, g == kGroupTickable);
        if (g == kGroupTimerDynamic) name += "::" + NameOf(e.fn, false);
        n = snprintf(line, sizeof line, "E,%.3f,%u,%d,0x%X,%s,%u,%.4f,%.4f\n", endMs, g, a == 0xFF ? -1 : static_cast<int>(a),
                     Rva(e.fn), name.c_str(), e.calls, TicksMs(e.ticks), TicksMs(e.maxTicks));
        Write(line, n);
    }
    for (int k = 0; k < kOffKinds; ++k) {
        const void* const fn = g_off[k].fn.load(std::memory_order_acquire);
        if (!fn) continue;
        const uint64_t c = g_off[k].calls.load(std::memory_order_relaxed), t = g_off[k].ticks.load(std::memory_order_relaxed);
        if (c == g_offLastCalls[k]) continue;
        n = snprintf(line, sizeof line, "O,%.3f,0x%X,%llu,%.4f\n", endMs, Rva(fn), static_cast<unsigned long long>(c - g_offLastCalls[k]),
                     TicksMs(t - g_offLastTicks[k]));
        g_offLastCalls[k] = c;
        g_offLastTicks[k] = t;
        Write(line, n);
    }
}

void WriteTop(const TRec& r)
{
    char line[1024];
    int n = snprintf(line, sizeof line, "T,%llu,%.3f,%.3f,%u,%.3f", static_cast<unsigned long long>(r.frame), r.endUs / 1000.0,
                     r.periodUs / 1000.0, r.execCalls, r.execUs / 1000.0);
    for (const Top& t : r.top) {
        if (t.fn) {
            const uint32_t g = t.grp & 0xFF;
            n += snprintf(line + n, sizeof line - n, ",%u,0x%X,%s,%.3f", g, Rva(t.fn), NameOf(t.cls, g == kGroupTickable).c_str(),
                          t.us / 1000.0);
        } else n += snprintf(line + n, sizeof line - n, ",,,,");
    }
    for (int b = 0; b < kBuckets; ++b) n += snprintf(line + n, sizeof line - n, ",%.3f", r.bucketUs[b] / 1000.0);
    n += snprintf(line + n, sizeof line - n, "\n");
    Write(line, n);
}

}  // namespace

// ---- start-up ----
bool InstallTickSplit(bool beforeGameCode, const wchar_t* dir)
{
    if (g_installed) return true;
    QueryPerformanceFrequency(&g_freq);
    if (dir) g_path = std::wstring(dir) + L"\\OblivionPerfFix-ticksplit.csv";
    if (!beforeGameCode) {
        Log("[ticksplit] HitchLedgerTickSplit: can only be installed at start-up (version.dll), not installed");
        return false;
    }
    if (!LedgerClockUs() && !g_testClock) {
        Log("[ticksplit] HitchLedgerTickSplit needs HitchLedger=1 at start-up: not installed");
        return false;
    }
    const GameModule& game = Game();
    st.region = AllocNearGame(256);
    if (!st.region) {
        Log("[ticksplit] no memory near the game's code: not installed");
        return false;
    }
    uint8_t* const r = st.region;
    if (uint8_t* p = FindOne(game, kExecSite, "the tick task's ExecuteTick call")) {
        st.exec = p + kExecAt;
        // stub at +0: mov r9d,esi; jmp [rip] -> ExecHook
        const uint8_t mov[3] = {0x44, 0x8B, 0xCE};
        memcpy(r, mov, 3);
        JmpStub(r + 3, reinterpret_cast<void*>(&ExecHook));
    }
    if (uint8_t* p = FindOne(game, kTickableSite, "TickObjects' Tick call")) {
        st.tickable = p + kTickableAt;
        // stub at +32: mov rcx,rbx; movaps xmm1,xmm6; jmp [rip] -> TickableHook
        const uint8_t pre[6] = {0x48, 0x8B, 0xCB, 0x0F, 0x28, 0xCE};
        memcpy(r + 32, pre, 6);
        JmpStub(r + 38, reinterpret_cast<void*>(&TickableHook));
    }
    // the timers' three execute calls: stubs at +160 (native), +192 (dynamic), +224 (callback)
    if (uint8_t* p = FindOne(game, kTimerNative, "the timer manager's delegate call")) {
        st.tNative = p + kTimerNativeAt;
        g_nativeSlot = st.tNative[9];
        const uint8_t pre[7] = {0x48, 0x8B, 0x4B, 0x10, 0x4C, 0x8B, 0xC3};  // mov rcx,[rbx+10h]; mov r8,rbx
        memcpy(r + 160, pre, 7);
        JmpStub(r + 167, reinterpret_cast<void*>(&TimerNativeHook));
    }
    if (uint8_t* p = FindOne(game, kTimerDynamic, "the timer manager's ProcessEvent call")) {
        st.tDynamic = p + kTimerDynamicAt;
        memcpy(&g_peSlot, st.tDynamic + 3, 4);
        if (g_peSlot > 0x1000 || (g_peSlot & 7)) st.tDynamic = nullptr;
        else JmpStub(r + 192, reinterpret_cast<void*>(&TimerDynamicHook));
    }
    if (uint8_t* p = FindOne(game, kTimerCallback, "the timer manager's TFunction call")) {
        st.tCallback = p + kTimerCallbackAt;
        const uint8_t pre[9] = {0x48, 0x8B, 0xC8, 0x48, 0x8B, 0xD6, 0x4C, 0x8B, 0xC3};  // mov rcx,rax; mov rdx,rsi; mov r8,rbx
        memcpy(r + 224, pre, 9);
        JmpStub(r + 233, reinterpret_cast<void*>(&TimerCallbackHook));
    }
    struct CallSite { const char* pattern; size_t at; const char* what; uint8_t** site; void* hook; void** orig; size_t slot; };
    uint8_t* timerBase = FindOne(game, kTimerSite, "UWorld::Tick's timer and tickables calls");
    const CallSite calls[] = {
        {kSetupSite, kSetupAt, "UWorld::Tick's SetupPhysicsTickFunctions call", &st.setup, reinterpret_cast<void*>(&SetupHook),
         reinterpret_cast<void**>(&g_setupOrig), 64},
        {kCollSite, kCollAt, "UWorld::Tick's EnsureCollisionTreeIsBuilt call", &st.coll, reinterpret_cast<void*>(&CollHook),
         reinterpret_cast<void**>(&g_collOrig), 80},
        {kLatentSite, kLatentAt, "UWorld::Tick's ProcessLatentActions call", &st.latent, reinterpret_cast<void*>(&LatentHook),
         reinterpret_cast<void**>(&g_latentOrig), 96},
        {nullptr, kTimerAt, "timer", &st.timer, reinterpret_cast<void*>(&TimerHook), reinterpret_cast<void**>(&g_timerOrig), 112},
        {nullptr, kTickObjAt, "tickobjects", &st.tickObj, reinterpret_cast<void*>(&TickObjectsHook),
         reinterpret_cast<void**>(&g_tickObjOrig), 128},
        {kStreamSite, kStreamAt, "UWorld::Tick's InternalUpdateStreamingState call", &st.stream, reinterpret_cast<void*>(&StreamHook),
         reinterpret_cast<void**>(&g_streamOrig), 144},
    };
    for (const CallSite& c : calls) {
        uint8_t* const base = c.pattern ? FindOne(game, c.pattern, c.what) : timerBase;
        if (!base) continue;
        uint8_t* const target = PrepareCall(game, base + c.at, r + c.slot, c.hook);
        if (!target) {
            Log("[ticksplit] %s: not a call to the game's code: not timed", c.what);
            continue;
        }
        *c.orig = target;
        *c.site = base + c.at;
    }
    DWORD old;
    VirtualProtect(r, 256, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), r, 256);
    // place the patches
    if (st.exec) {
        uint8_t call[6] = {0xE8, 0, 0, 0, 0, 0x90};
        const int64_t rel = r - (st.exec + 5);
        const int32_t rel32 = static_cast<int32_t>(rel);
        memcpy(call + 1, &rel32, 4);
        if (rel != rel32 || !WriteCode(st.exec, std::span<const uint8_t>(call, 6))) {
            Log("[ticksplit] could not redirect the ExecuteTick call: tick functions not timed");
            st.exec = nullptr;
        }
    }
    if (st.tickable) {
        uint8_t call[11] = {0xE8, 0, 0, 0, 0, 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
        const int64_t rel = (r + 32) - (st.tickable + 5);
        const int32_t rel32 = static_cast<int32_t>(rel);
        memcpy(call + 1, &rel32, 4);
        if (rel != rel32 || !WriteCode(st.tickable, std::span<const uint8_t>(call, 11))) {
            Log("[ticksplit] could not redirect the tickables' Tick call: tickables not split");
            st.tickable = nullptr;
        }
    }
    for (const CallSite& c : calls)
        if (*c.site && !RedirectCall(*c.site, r + c.slot)) {
            Log("[ticksplit] could not redirect %s", c.what);
            *c.site = nullptr;
        }
    auto place = [&](uint8_t*& site, uint8_t* stub, size_t n, const char* what) {
        if (!site) return;
        uint8_t code[16] = {0xE8};
        const int64_t rel = stub - (site + 5);
        const int32_t rel32 = static_cast<int32_t>(rel);
        memcpy(code + 1, &rel32, 4);
        for (size_t i = 5; i < n; ++i) code[i] = 0x90;
        if (rel != rel32 || !WriteCode(site, std::span<const uint8_t>(code, n))) {
            Log("[ticksplit] could not redirect %s", what);
            site = nullptr;
        }
    };
    place(st.tNative, r + 160, 10, "the timers' delegate call");
    place(st.tDynamic, r + 192, 7, "the timers' ProcessEvent call");
    place(st.tCallback, r + 224, 5, "the timers' TFunction call");
    g_installed = st.exec != nullptr;
    Log("[ticksplit] %s: tick functions %s, tickables %s, buckets setup %s, colltree %s, latent %s, timers %s, tickobjects %s, "
        "streaming %s, timers' calls %s/%s/%s",
        g_installed ? "installed" : "NOT installed (the ExecuteTick call was not found)", st.exec ? "timed" : "-",
        st.tickable ? "timed" : "-", st.setup ? "timed" : "-", st.coll ? "timed" : "-", st.latent ? "timed" : "-",
        st.timer ? "timed" : "-", st.tickObj ? "timed" : "-", st.stream ? "timed" : "-",
        st.tNative ? "timed" : "-", st.tDynamic ? "timed" : "-", st.tCallback ? "timed" : "-");
    return g_installed;
}

void SetTickSplit(bool on)
{
    if (!g_installed) {
        static bool logged = false;
        if (on && !logged) {
            logged = true;
            Log("[ticksplit] HitchLedgerTickSplit=1 but the split was not installed at start-up (needs HitchLedger=1 too): restart");
        }
        return;
    }
    if (g_on.exchange(on) != on) Log("[ticksplit] %s", on ? "splitting tick time" : "stopped");
}



// ---- game thread (from the ledger's hooks) ----
void TickSplitGroup(int group, bool entering)
{
    if (!g_installed || !g_on.load(std::memory_order_relaxed)) return;
    if (!entering) {
        g_curGroup = kGroupOutside;
        return;
    }
    g_curGroup = group >= 0 && group < 8 ? static_cast<uint32_t>(group) : kGroupOutside;
    const uint64_t now = Now();
    if (group == 0 && g_setupEnd) { g_frameBucket[kStartFrame] += now - g_setupEnd; g_setupEnd = 0; }
    if (group == 5 && g_camTs) { g_frameBucket[kCamera] += now - g_camTs; g_camTs = 0; }
}

void TickSplitPreActor()
{
    if (g_installed && g_on.load(std::memory_order_relaxed)) g_preTs = Now();
}

void TickSplitFrameEnd(const TickSplitFrame& fr)
{
    if (!g_installed) return;
    Window& w = g_win[g_active];
    const bool on = g_on.load(std::memory_order_relaxed);
    if (on && fr.recording) {
        if (g_percOn.load(std::memory_order_relaxed)) {
            g_percFrame.frames = 1;
            PercMerge(w.perc, g_percFrame);
        }
        if (!w.startUs) w.startUs = fr.endUs - fr.periodUs;
        w.endUs = fr.endUs;
        w.frames++;
        w.periodUs += fr.periodUs;
        w.actorUs += fr.actorUs;
        w.pairUs += fr.pairUs;
        for (int i = 0; i < 8; ++i) w.tgUs[i] += fr.tgUs[i];
        for (int b = 0; b < kBuckets; ++b) w.bucket[b] += g_frameBucket[b];
        if (fr.slow) {
            w.slowFrames++;
            TRec r{};
            r.frame = fr.frame;
            r.endUs = fr.endUs;
            r.periodUs = fr.periodUs;
            r.execUs = TicksUs(g_frameExecTicks);
            r.execCalls = static_cast<uint32_t>(g_frameExecCalls);
            for (uint32_t i = 0; i < g_nTouched; ++i) {
                const Entry* const e = g_touched[i];
                if (e->frameStamp != g_frameNo) continue;
                for (int k = 0; k < 3; ++k) {
                    if (!r.top[k].fn || TicksUs(e->frameTicks) > r.top[k].us) {
                        for (int m = 2; m > k; --m) r.top[m] = r.top[m - 1];
                        r.top[k] = Top{e->fn, e->cls, e->grp, TicksUs(e->frameTicks)};
                        break;
                    }
                }
            }
            for (int b = 0; b < kBuckets; ++b) r.bucketUs[b] = TicksUs(g_frameBucket[b]);
            const uint64_t h = g_thead.load(std::memory_order_relaxed);
            if (h - g_ttail.load(std::memory_order_acquire) < kTRing) {
                g_tring[h % kTRing] = r;
                g_thead.store(h + 1, std::memory_order_release);
            } else g_tdropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (on && fr.recording && w.endUs - w.startUs >= kWindowUs) TickSplitWindowEnd();
    ++g_frameNo;
    g_nTouched = 0;
    g_frameExecTicks = g_frameExecCalls = 0;
    memset(g_frameBucket, 0, sizeof g_frameBucket);
    g_percFrame = PercAcc{};
    g_preTs = g_setupEnd = g_camTs = 0;
    g_depth = 0;
    g_curGroup = kGroupOutside;
}

// Hands the window to the monitor thread if it took the last one (else this window grows).
void TickSplitWindowEnd()
{
    if (!g_installed || !g_on.load(std::memory_order_relaxed)) return;
    if (g_pending.load(std::memory_order_acquire) != -1) return;
    const int done = g_active;
    g_active ^= 1;  // the monitor thread cleared it after writing
    g_pending.store(done, std::memory_order_release);
}

// ---- monitor thread (from HitchLedgerDrain) ----
void TickSplitDrain()
{
    if (!g_installed) return;
    const int p = g_pending.load(std::memory_order_acquire);
    const bool rows = p >= 0 || g_thead.load(std::memory_order_acquire) != g_ttail.load(std::memory_order_relaxed);
    if (!rows) return;
    if (!OpenFile()) {
        g_ttail.store(g_thead.load());
        if (p >= 0) {
            memset(&g_win[p], 0, sizeof(Window));
            g_pending.store(-1, std::memory_order_release);
        }
        return;
    }
    for (uint64_t t = g_ttail.load(std::memory_order_relaxed); t != g_thead.load(std::memory_order_acquire);) {
        const TRec r = g_tring[t % kTRing];
        g_ttail.store(++t, std::memory_order_release);
        WriteTop(r);
    }
    if (p >= 0) {
        WriteWindow(g_win[p]);
        memset(&g_win[p], 0, sizeof(Window));
        g_pending.store(-1, std::memory_order_release);
    }
    fflush(g_file);
}

bool TickSplitInfo(TickSplitInfoOut* out)
{
    if (!out) return false;
    out->installed = g_installed;
    out->exec = st.exec;
    out->tickable = st.tickable;
    out->setup = st.setup;
    out->coll = st.coll;
    out->latent = st.latent;
    out->timer = st.timer;
    out->tickObj = st.tickObj;
    out->stream = st.stream;
    out->region = st.region;
    out->tNative = st.tNative;
    out->tDynamic = st.tDynamic;
    out->tCallback = st.tCallback;
    out->execHook = reinterpret_cast<void*>(&ExecHook);
    out->tickableHook = reinterpret_cast<void*>(&TickableHook);
    return true;
}

void TickSplitTestSetup(uint64_t (*clock)(), const wchar_t* path)
{
    g_testClock = clock;
    if (path) {
        if (g_file) { fclose(g_file); g_file = nullptr; }
        g_path = path;
        g_fileBytes = 0;
        g_fileFailed = false;
    }
}

void TickSplitTestClose()
{
    if (g_file) { fclose(g_file); g_file = nullptr; }
}


}  // namespace opf
