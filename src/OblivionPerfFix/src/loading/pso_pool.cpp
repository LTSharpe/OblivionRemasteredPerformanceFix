// PsoCompileThreadPool (1.1.10 lab, default 0): the start-up PSO precompile and the PSO precache compile on UE's own
// PSO thread pool instead of the task-graph workers (game bug with a cold driver cache: the loading froze behind them).
//
// UE 5.3 (PipelineStateCache.cpp) compiles the pipelines of the bundled PSO caches (FShaderPipelineCache's precompile)
// and of PSO precaching (what newly loaded components will draw with) as task-graph tasks unless r.pso.PrecompileThreadPool*
// asks for its PSO precompile thread pool (FPSOPrecacheThreadPool: a FQueuedThreadPool, "PSOPrecompilePool", threads at
// below-normal priority). The game leaves it off. With a warm driver shader cache a pipeline is created in about a
// millisecond and it does not matter where. With a cold one (after a driver update, a new install) the driver compiles
// every pipeline for real, 0.1 s to seconds each, on the thread that asks: every task worker is then inside the driver for
// long stretches, and the tasks the rest of the game needs (the shader-code preload tasks the loader waits for, render
// and game tasks) wait until a worker comes free. Seen (worker coldfreeze, 2026-10-23, 6 cores / 12 threads, stock game):
// the first teleport after a new game froze the picture for 30 s and more (the benchmark ended the game): the game thread
// in FlushAsyncLoading's loop, all 10 task workers and the driver's 4 compile threads busy compiling pipelines.
//
// With PsoCompileThreadPool = N the fix sets r.pso.PrecompileThreadPoolSize to N while the game starts (the variable is
// read-only on the console; UE reads it each time it creates a precompile or precache pipeline): those compiles then run
// on UE's pool and never occupy a task worker. Pipelines the renderer needs right now (not precompiled, not precached)
// still compile as tasks, as before. The same pipelines are created with the same inputs: identical results, only the
// threads they are compiled on and the order the operating system gives them CPU time change. -1 = logical processors
// minus 2 (at least 1): UE's own task-worker count on a CPU with hyper-threading, one more than it on a CPU without.
//
// START-UP ONLY. UE treats the size as a constant (the variable is ECVF_ReadOnly): whether a precache pipeline compiles as
// a task or on the pool is decided when it starts, and PipelineStateCache.cpp:1048-1054 (BoostPriority ->
// RescheduleTaskToHighPriority) then assumes the pool job exists whenever UsePool() is true, with a check() that is
// compiled out in Shipping: switching 0 -> N while a task-compiled precache pipeline is in flight crashes at the next
// boost, and N -> 0 leaves pool jobs that are never boosted again. So the value is written once, in Init, and every
// later change of the ini key is ignored (logged once: it takes a restart).
//
// NEEDS THE TASK WAIT GUARD. With the PSO compiles off the task workers, more workers are free to take ray-tracing
// pipeline tasks at the same moment, which is exactly what produces the task graph's wait deadlock (worker coldfreeze:
// run c6-p0, the pool alone, hung 125 s; see task_wait_guard.cpp). So the pool is applied only when TaskWaitDeadlockGuard is
// installed and in mode 1 (and the guard keeps mode 1 while the pool runs); otherwise a log line says why it was not
// applied. Two keys, not one: the guard alone is safe and fixes the 8-core deadlock by itself, so it stays usable without
// the pool; the pool is the part that needs the guard.
//
// The pool's threads keep UE's priority for it (below normal, -1: pipelines prepared ahead of use yield to the game's own
// threads, as precache tasks yield to every other task in the task graph). PsoCompileThreadPoolPriority (not in the
// shipped ini) can give them another (0 = normal, the task workers'; measured cold with 6 cores: the loading then
// competes with the pool for the processors, two 21 s stalls instead of none over 4 s; warm: no difference). Below normal
// has only been measured on CPUs with idle processors (every "6-core" run had all 16 logical processors, UE only sized
// itself for 6): on a real 4-6 core CPU with other busy programs the pool may run later than the task workers would have.
//
// An Engine.ini of the player that sets r.pso.PrecompileThreadPoolPercentOfHardwareThreads is applied after our start-up
// write; size and percent together trip UE's ensure (they are mutually exclusive) and our size would win over theirs. For
// the first two minutes (before any pool can exist) the tick therefore puts our size back to 0 when it sees a percent, and
// leaves the player's setting alone; a size that someone else changed is theirs too.
//
// Found by the variable's registration: lea r8,[variable]; lea rdx,[L"r.pso.PrecompileThreadPoolSize"] (build 19115871:
// exe+0x9D2CFB, the variable at exe+0x9206128), the same for r.pso.PrecompileThreadPoolPercentOfHardwareThreads.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

constexpr wchar_t kSize[] = L"r.pso.PrecompileThreadPoolSize";
constexpr wchar_t kPercent[] = L"r.pso.PrecompileThreadPoolPercentOfHardwareThreads";
constexpr wchar_t kPriority[] = L"r.pso.PrecompileThreadPoolThreadPriority";
constexpr int32_t kUeDefaultPriority = 2;   // TPri_BelowNormal (EThreadPriority: Normal 0, AboveNormal 1, BelowNormal 2, ...)
constexpr int kWatchTicks = 120;            // the Engine.ini check runs for the first two minutes (ticks are one second apart)

int32_t* g_size = nullptr;
int32_t* g_percent = nullptr;
int32_t* g_priority = nullptr;
int g_priorityTicks = 0;         // the pool's priority is written within the first 60 calls (a minute) only
bool g_priorityDone = false;
bool g_findDone = false, g_findOk = false;
bool g_started = false;          // the start-up decision was taken (once)
int g_startupThreads = 0;        // the key's value at start-up
bool g_restartLogged = false;
int g_watchTicks = 0;
std::atomic<int> g_applied{0};   // the value we wrote (0 = none / given back)
bool g_leftAlone = false;

const uint8_t* FindWide(const Section& s, const wchar_t* name, size_t bytes)
{
    const uint8_t* hit = nullptr;
    for (const uint8_t* p = s.begin; p + bytes <= s.end; p += 2) {
        if (memcmp(p, name, bytes) != 0) continue;
        if (hit) return nullptr;
        hit = p;
    }
    return hit;
}

// lea r8,[var] (4C 8D 05 rel32) immediately followed by lea rdx,[name] (48 8D 15 rel32): returns the variable
int32_t* FindRegistration(const GameModule& game, const std::vector<uint8_t*>& hits, const uint8_t* name)
{
    int32_t* found = nullptr;
    for (uint8_t* q : hits) {
        if (RipTarget(q + 7, 3, 7) != name) continue;
        uint8_t* var = RipTarget(q, 3, 7);
        if (!game.data.Contains(var) || (found && found != reinterpret_cast<int32_t*>(var))) return nullptr;
        found = reinterpret_cast<int32_t*>(var);
    }
    return found;
}

}  // namespace

bool FindPsoCompileThreadPool()
{
    if (g_findDone) return g_findOk;
    g_findDone = true;
    const GameModule& game = Game();
    const uint8_t* sizeName = FindWide(game.rdata, kSize, sizeof(kSize));
    const uint8_t* percentName = FindWide(game.rdata, kPercent, sizeof(kPercent));
    if (!sizeName || !percentName) {
        Log("[psopool] the PSO thread pool's console variables were not found: PsoCompileThreadPool unavailable");
        return false;
    }
    const auto hits = FindAll(game.text, ParsePattern("4C 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ??"));
    g_size = FindRegistration(game, hits, sizeName);
    g_percent = FindRegistration(game, hits, percentName);
    if (const uint8_t* priorityName = FindWide(game.rdata, kPriority, sizeof(kPriority))) g_priority = FindRegistration(game, hits, priorityName);
    if (!g_size || !g_percent || g_size == g_percent) {
        Log("[psopool] the PSO thread pool's variables were not found where they are registered: PsoCompileThreadPool unavailable");
        return false;
    }
    g_findOk = true;
    Log("[psopool] r.pso.PrecompileThreadPoolSize at exe+0x%X, ...PercentOfHardwareThreads at exe+0x%X",
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(g_size) - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(g_percent) - game.base));
    return true;
}

// The pool's thread priority (UE: below normal; read when UE creates the pool, which happens when the first pipeline needs
// it: a priority written after that is not used): the task workers' priority (normal) unless PsoCompileThreadPoolPriority
// says otherwise. The variable may be set by the exe's static initialisation, so it is written once it holds UE's default,
// within the first minute, and only then; any other value (the game's or an ini's) is kept.
static void SetPoolPriority(int priority)
{
    if (!g_priority || g_priorityDone || priority < 0 || ++g_priorityTicks > 60) return;
    volatile int32_t* p = g_priority;
    if (*p == priority) { g_priorityDone = true; return; }
    if (*p != kUeDefaultPriority) return;
    *p = priority;
    g_priorityDone = true;
    Log("[psopool] PSO thread pool priority %d (EThreadPriority; UE's default %d = below normal), for a pool created from now on",
        priority, kUeDefaultPriority);
}

// Start-up, once (Init, before the game's code runs): threads 0 = the game's, N = N threads, -1 = logical processors - 2.
// Applied only with TaskWaitDeadlockGuard installed in mode 1 (so Init installs the guard first).
void SetPsoCompileThreadPool(int threads, int priority)
{
    if (!g_findOk || g_started) return;
    g_started = true;
    g_startupThreads = threads;
    if (threads == 0) {
        Log("[psopool] PsoCompileThreadPool off (takes a restart to switch on)");
        return;
    }
    const int guard = TaskWaitGuardMode();
    if (guard != 1) {
        Log("[psopool] PsoCompileThreadPool needs TaskWaitDeadlockGuard=1 installed at start-up (the pool alone can run into the "
            "task graph's wait deadlock); the guard is %s: not applied",
            guard < 0 ? "not installed" : "in mode 2 (measure only)");
        return;
    }
    const unsigned logical = UsableLogicalProcessors();
    const int want = threads < 0 ? static_cast<int>(logical > 3 ? logical - 2 : 1) : threads;
    volatile int32_t* size = g_size;
    volatile int32_t* percent = g_percent;
    if (*size != 0 || *percent != 0) {
        g_leftAlone = true;
        Log("[psopool] the game already sets the PSO thread pool (size %d, %d %% of threads): left alone", *size, *percent);
        return;
    }
    *size = want;
    g_applied = want;
    Log("[psopool] PSO precompile and precache compile on UE's PSO thread pool: %d thread(s) (r.pso.PrecompileThreadPoolSize; "
        "start-up only)", want);
    SetPoolPriority(priority);
}

// Once a second (monitor thread). No change is made to the pool size after start-up, except giving it back to the player's
// own Engine.ini setting in the first two minutes (see above).
void PsoCompileThreadPoolTick(int threads, int priority)
{
    if (!g_findOk || !g_started) return;
    if (threads != g_startupThreads && !g_restartLogged) {
        g_restartLogged = true;
        Log("[psopool] PsoCompileThreadPool changed in the ini (%d, at start-up %d): takes a restart, ignored while the game runs",
            threads, g_startupThreads);
    }
    const int applied = g_applied.load();
    if (applied == 0 || g_leftAlone) return;
    volatile int32_t* size = g_size;
    volatile int32_t* percent = g_percent;
    if (g_watchTicks < kWatchTicks) {
        ++g_watchTicks;
        if (*size != applied) {      // the player's Engine.ini (or the game) set the size after our write: theirs
            g_leftAlone = true;
            g_applied = 0;
            Log("[psopool] r.pso.PrecompileThreadPoolSize is %d, not our %d (set by Engine.ini or the game): left alone", *size, applied);
            return;
        }
        if (*percent != 0) {         // size and percent together trip UE's ensure: keep the player's percent, give ours back
            *size = 0;
            g_applied = 0;
            g_leftAlone = true;
            Log("[psopool] the player's Engine.ini sets r.pso.PrecompileThreadPoolPercentOfHardwareThreads %d: PsoCompileThreadPool "
                "given back (size set to 0 again, their setting stays)", *percent);
            return;
        }
    }
    SetPoolPriority(priority);
}

bool PsoCompileThreadPoolApplied() { return g_applied.load() > 0; }

void PsoCompileThreadPoolTestReset()   // self-test only: the start-up state again
{
    g_started = false;
    g_startupThreads = 0;
    g_restartLogged = false;
    g_watchTicks = 0;
    g_priorityTicks = 0;
    g_priorityDone = false;
    g_applied = 0;
    g_leftAlone = false;
}

bool PsoCompileThreadPoolInfo(int32_t** size, int32_t** percent, int32_t** priority)
{
    if (!g_findOk) return false;
    *size = g_size;
    *percent = g_percent;
    if (priority) *priority = g_priority;
    return true;
}

}  // namespace opf
