// TaskWaitDeadlockGuard self-test (worker coldfreeze), included by selftest.cpp: the task graph's worker wait on the mapped
// exe (the function, its vtable slot, DoReserveWorkUntil, TriggerEventWhenTasksComplete, the singletons, the TLS index,
// the reserve-workers flag), then the guard's worker branch against fakes: a fake reserve worker thread (takes the
// condition through its Move, calls it until it answers "done", like FReserveScheduler's BusyWaitUntil), a fake trigger
// task thread (calls the event's Trigger once the tasks are done, after a delay), fake graph events (bit 26 = complete).
#pragma once

#include <intrin.h>
#include <thread>
#include <vector>

namespace tw {

struct FakeArray {                  // FGraphEventArray: 4 inline pointers, heap pointer at +0x20, count at +0x28
    void* inl[4];
    void** heap;
    int32_t num, max;
};
static_assert(sizeof(FakeArray) == 0x30, "FGraphEventArray layout");

alignas(8) uint64_t g_ev[6];        // fake FGraphEvents: the subsequent list head word
std::atomic<bool> g_reserveFree{true};
std::atomic<int> g_triggerDelayMs{0};
std::atomic<bool> g_triggerImmediate{false};
std::thread g_reserveThread, g_triggerThread;
std::atomic<int> g_condCalls{0};

void CompleteAll(int n) { for (int i = 0; i < n; ++i) reinterpret_cast<std::atomic<uint64_t>*>(&g_ev[i])->fetch_or(1ull << 26); }
bool AllDone(const FakeArray* a)
{
    void* const* d = a->heap ? a->heap : a->inl;
    for (int i = 0; i < a->num; ++i)
        if (d[i] && ((reinterpret_cast<std::atomic<uint64_t>*>(d[i])->load() >> 26) & 1) == 0) return false;
    return true;
}

using MoveFn = void (*)(void*, void*, void*, void*, uint32_t);
using CallFn = bool (*)(const void*, void*);
using DestroyFn = void (*)(void*, void*);

// FReserveScheduler::DoReserveWorkUntil: moves the condition into the reserve worker's storage, wakes it
bool FakeDoReserve(void* /*reserve*/, void* condition)
{
    if (!g_reserveFree.load()) return false;
    auto* src = static_cast<uint8_t*>(condition);
    const void* const* vt = *reinterpret_cast<const void* const* const*>(src + 8);
    struct alignas(8) Stored { uint8_t b[16]; };
    auto* dst = new Stored{};
    reinterpret_cast<MoveFn>(vt[0])(src + 8, dst->b + 8, dst->b, src, 8);
    g_reserveThread = std::thread([dst] {
        const void* const* v = *reinterpret_cast<const void* const* const*>(dst->b + 8);
        while (!reinterpret_cast<CallFn>(v[1])(dst->b + 8, dst->b)) { g_condCalls++; Sleep(1); }
        reinterpret_cast<DestroyFn>(v[3])(dst->b + 8, dst->b);
        delete dst;
    });
    return true;
}
// TriggerEventWhenTasksComplete: a task that calls Event->Trigger() (vtable +0x10) once the tasks are done
void FakeTrigger(void* /*self*/, void* event, const void* tasks, uint32_t cur, uint32_t triggerThread)
{
    (void)cur; (void)triggerThread;
    using TrigFn = void (*)(void*);
    auto trig = reinterpret_cast<TrigFn>((*reinterpret_cast<void* const* const*>(event))[2]);
    if (g_triggerImmediate.load()) { trig(event); return; }   // all done already: the game triggers at once
    const auto* a = static_cast<const FakeArray*>(tasks);
    g_triggerThread = std::thread([event, a, trig] {
        while (!AllDone(a)) Sleep(1);
        Sleep(static_cast<DWORD>(g_triggerDelayMs.load()));
        trig(event);
    });
}
void Join()
{
    if (g_reserveThread.joinable()) g_reserveThread.join();
    if (g_triggerThread.joinable()) g_triggerThread.join();
}

// Runs the worker branch with tasks that complete after completeMs; returns how long the wait took (ms), -1 if it was
// not taken (no reserve worker).
double RunWait(int mode, int tasks, int completeMs, bool* taken)
{
    memset(g_ev, 0, sizeof g_ev);
    FakeArray arr{};
    for (int i = 0; i < tasks && i < 4; ++i) arr.inl[i] = &g_ev[i];
    void* heap[6] = {};
    if (tasks > 4) { for (int i = 0; i < tasks; ++i) heap[i] = &g_ev[i]; arr.heap = heap; }
    arr.num = tasks; arr.max = tasks;
    std::thread completer([tasks, completeMs] { Sleep(static_cast<DWORD>(completeMs)); CompleteAll(tasks); });
    if (completeMs == 0) completer.join();
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    *taken = opf::TaskWaitGuardTestWait(reinterpret_cast<void*>(0x1234), &arr, mode);
    QueryPerformanceCounter(&t1);
    if (completer.joinable()) completer.join();
    Join();
    return (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}

// ---- stress: the wake race, many threads, many iterations. Per lane: a waiter thread (the "worker"), a reserve thread that
// calls the condition back to back (the busy-wait of FReserveScheduler, no sleeps), and a trigger/completion thread that
// completes the events (random jitter) and fires the event's Trigger right at the completion instant or later. Each
// iteration is random: 1-10 events (null entries, inline/heap array, > 8 = heap copy), mode 1 or 2, how the reserve
// thread takes the condition (Call, CallAndMove, or destroyed without a call), how the trigger comes (synchronously
// because the tasks were done already, at once, after jitter, late). After each wait: every event must be complete (never
// woken early); the caller's array is poisoned at the start of the wait by the trigger thread (the stock lambda has its
// own copy) and again, with the events' memory made inaccessible, as soon as the wait returns (the reserve thread must not
// read either any more; a read would crash the test). At the end no wait object may be alive.
struct Lane {
    std::atomic<uint32_t> goR{0}, goT{0}, doneR{0}, doneT{0};   // iteration numbers; kQuit ends the thread
    alignas(8) uint8_t cond[16]{};     // the delegate the fake DoReserveWorkUntil moved the condition into
    void* event = nullptr;
    void* evPtr[10] = {};
    FakeArray* arr = nullptr;
    uint8_t* page = nullptr;
    uint32_t variant = 0, tstyle = 0;
    int k = 0;
    uint32_t rng = 1;
    std::thread R, T;
    uint32_t Next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }   // the waiter thread only
};
constexpr uint32_t kQuit = 0xFFFFFFFFu;
inline thread_local Lane* t_lane = nullptr;
inline thread_local uint32_t t_iter = 0;
std::atomic<uint64_t> g_stressProgress{0}, g_stressBad{0};

inline void Post(std::atomic<uint32_t>& a, uint32_t v) { a.store(v, std::memory_order_release); opf::WakeWordAll(&a); }
// waits until `a` differs from `seen`; spins briefly, then blocks
inline uint32_t Await(std::atomic<uint32_t>& a, uint32_t seen)
{
    for (int s = 0;; ++s) {
        const uint32_t v = a.load(std::memory_order_acquire);
        if (v != seen) return v;
        if (s < 400) _mm_pause();
        else opf::BlockOnWord(&a, &seen, sizeof seen);
    }
}
inline void AwaitEq(std::atomic<uint32_t>& a, uint32_t want)
{
    for (uint32_t v = a.load(std::memory_order_acquire); v != want; v = a.load(std::memory_order_acquire)) Await(a, v);
}
inline uint32_t Rnd()    // the helper threads' own generator
{
    static thread_local uint32_t s = GetCurrentThreadId() * 2654435761u | 1u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}
inline void Jitter(uint32_t r)
{
    for (uint32_t i = r % 64; i; --i) _mm_pause();
    if ((r >> 8) % 16 == 0) SwitchToThread();
}

bool StressDoReserve(void*, void* condition)
{
    Lane* l = t_lane;
    auto* src = static_cast<uint8_t*>(condition);
    const void* const* vt = *reinterpret_cast<const void* const* const*>(src + 8);
    reinterpret_cast<MoveFn>(vt[0])(src + 8, l->cond + 8, l->cond, src, 8);
    Post(l->goR, t_iter);
    return true;
}
void StressTrigger(void*, void* event, const void* tasks, uint32_t, uint32_t)
{
    Lane* l = t_lane;
    using TrigFn = void (*)(void*);
    auto trig = reinterpret_cast<TrigFn>((*reinterpret_cast<void* const* const*>(event))[2]);
    if (l->tstyle == 0) {            // done already: the game triggers inside TriggerEventWhenTasksComplete
        (void)tasks;
        trig(event);
        Post(l->doneT, t_iter);
        return;
    }
    l->event = event;
    Post(l->goT, t_iter);
}

inline void LaneReserveThread(Lane* l)
{
    uint32_t seen = 0;
    for (;;) {
        seen = Await(l->goR, seen);
        if (seen == kQuit) return;
        const void* const* v = *reinterpret_cast<const void* const* const*>(l->cond + 8);
        if (l->variant == 7) {       // the reserve worker stops: the condition is destroyed without ever being called
            Jitter(Rnd());
            reinterpret_cast<DestroyFn>(v[3])(l->cond + 8, l->cond);
        } else if (l->variant >= 5) {   // CallAndMove into a second delegate, then Call that one
            alignas(8) uint8_t dst[16] = {};
            using CamFn = bool (*)(void*, uint8_t*, void*, uint32_t);
            bool r = reinterpret_cast<CamFn>(v[2])(l->cond + 8, dst, l->cond, 8);
            const void* const* v2 = *reinterpret_cast<const void* const* const*>(dst + 8);
            for (int s = 0; !r; ++s) {
                r = reinterpret_cast<CallFn>(v2[1])(dst + 8, dst);
                if (s > 2000) SwitchToThread();
            }
            reinterpret_cast<DestroyFn>(v2[3])(dst + 8, dst);
            reinterpret_cast<DestroyFn>(v[3])(l->cond + 8, l->cond);   // the moved-from delegate: nothing left to release
        } else {
            for (int s = 0; !reinterpret_cast<CallFn>(v[1])(l->cond + 8, l->cond); ++s)
                if (s > 2000) SwitchToThread();
            reinterpret_cast<DestroyFn>(v[3])(l->cond + 8, l->cond);
        }
        Post(l->doneR, seen);
    }
}
inline void LaneTriggerThread(Lane* l)
{
    uint32_t seen = 0;
    for (;;) {
        seen = Await(l->goT, seen);
        if (seen == kQuit) return;
        if (l->tstyle >= 2 && (Rnd() & 1))                 // the caller's array changes during the wait (legal in stock)
            memset(static_cast<void*>(l->arr), 0xCD, sizeof(FakeArray));
        for (int i = 0; i < l->k; ++i) {
            if (!l->evPtr[i]) continue;
            if (l->tstyle >= 2) Jitter(Rnd());
            reinterpret_cast<std::atomic<uint64_t>*>(l->evPtr[i])->fetch_or(1ull << 26);
        }
        if (l->tstyle == 3) { Jitter(Rnd()); SwitchToThread(); }
        else if (l->tstyle == 2) Jitter(Rnd());
        using TrigFn = void (*)(void*);
        reinterpret_cast<TrigFn>((*reinterpret_cast<void* const* const*>(l->event))[2])(l->event);
        Post(l->doneT, seen);
    }
}
inline void LaneWaiter(Lane* l, int iterations, uint32_t seed)
{
    l->rng = seed;
    l->page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    l->R = std::thread(LaneReserveThread, l);
    l->T = std::thread(LaneTriggerThread, l);
    t_lane = l;
    for (uint32_t it = 1; it <= static_cast<uint32_t>(iterations); ++it) {
        t_iter = it;
        const uint32_t r = l->Next();
        l->k = 1 + static_cast<int>(r % 10);
        l->variant = (r >> 8) % 8;
        l->tstyle = (r >> 12) % 4;
        const int mode = (r >> 16) % 5 == 0 ? 2 : 1;
        DWORD old;
        VirtualProtect(l->page, 4096, PAGE_READWRITE, &old);
        memset(l->page, 0, 4096);
        FakeArray arr{};
        void** heap = nullptr;
        for (int i = 0; i < l->k; ++i) l->evPtr[i] = ((r >> (20 + i)) & 7) == 0 ? nullptr : l->page + i * 64;
        if (l->k > 4) { heap = new void*[l->k]; arr.heap = heap; }
        for (int i = 0; i < l->k; ++i) (heap ? heap : arr.inl)[i] = l->evPtr[i];
        arr.num = arr.max = l->k;
        l->arr = &arr;
        if (l->tstyle == 0)
            for (int i = 0; i < l->k; ++i) if (l->evPtr[i]) reinterpret_cast<std::atomic<uint64_t>*>(l->evPtr[i])->fetch_or(1ull << 26);
        const bool taken = opf::TaskWaitGuardTestWait(reinterpret_cast<void*>(0x1234), &arr, mode);
        for (int i = 0; i < l->k; ++i)       // woken only when every awaited event is complete
            if (l->evPtr[i] && ((*reinterpret_cast<std::atomic<uint64_t>*>(l->evPtr[i]) >> 26) & 1) == 0) g_stressBad++;
        if (!taken) g_stressBad++;
        memset(static_cast<void*>(&arr), 0xCD, sizeof arr);     // the caller is done with its array and its events
        if (heap) { memset(heap, 0xCD, l->k * sizeof(void*)); delete[] heap; }
        VirtualProtect(l->page, 4096, PAGE_NOACCESS, &old);
        AwaitEq(l->doneR, it);
        if (l->tstyle != 0) AwaitEq(l->doneT, it);
        g_stressProgress++;
    }
    Post(l->goR, kQuit);
    Post(l->goT, kQuit);
    l->R.join();
    l->T.join();
    VirtualFree(l->page, 0, MEM_RELEASE);
}

inline void StressTest()
{
    using namespace opf;
    TaskWaitGuardTestSet(reinterpret_cast<void*>(&StressDoReserve), reinterpret_cast<void*>(&StressTrigger), reinterpret_cast<uint8_t*>(0x5678), 0x6FF);
    TaskWaitGuardTestCounters();
    constexpr int kLanes = 6, kIterations = 30000;
    static Lane lanes[kLanes];
    std::vector<std::thread> waiters;
    const ULONGLONG t0 = GetTickCount64();
    for (int i = 0; i < kLanes; ++i) {
        waiters.emplace_back(LaneWaiter, &lanes[i], kIterations, 0x9E3779B9u * (i + 1));
        SetThreadPriority(waiters.back().native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
    }
    uint64_t last = 0;
    ULONGLONG lastChange = GetTickCount64();
    while (g_stressProgress.load() < static_cast<uint64_t>(kLanes) * kIterations) {
        Sleep(50);
        const uint64_t p = g_stressProgress.load();
        if (p != last) { last = p; lastChange = GetTickCount64(); }
        else if (GetTickCount64() - lastChange > 30000) {
            printf("FAIL  taskwait stress: no progress for 30 s after %llu waits (a wait never woke up)\n", static_cast<unsigned long long>(p));
            fflush(stdout);
            ExitProcess(3);
        }
    }
    for (auto& w : waiters) w.join();
    const ULONGLONG ms = GetTickCount64() - t0;
    const TaskWaitGuardCounters c = TaskWaitGuardTestCounters();
    char msg[240];
    snprintf(msg, sizeof msg, "taskwait stress: %d waits on %d lanes in %llu ms, none woken early, none lost (woken first by the reserve worker %llu, "
             "trigger after the tasks were done %llu), no wait object left", kLanes * kIterations, kLanes, static_cast<unsigned long long>(ms),
             static_cast<unsigned long long>(c.rescues), static_cast<unsigned long long>(c.lateTriggers));
    Check(g_stressBad.load() == 0 && TaskWaitGuardTestLiveWaits() == 0 && c.orphans == 0 && c.rescues > 0 && c.lateTriggers > 0, msg);
}

// PsoCompileThreadPool against the mapped exe: start-up only, only with the guard in mode 1, Engine.ini percent given back.
inline void PoolSelfTest(int32_t* size, int32_t* percent)
{
    using namespace opf;
    *size = 0; *percent = 0;
    PsoCompileThreadPoolTestReset();
    SetPsoCompileThreadPool(-1, -1);   // the guard is not installed yet (the test did not install it): not applied
    Check(*size == 0 && !PsoCompileThreadPoolApplied(), "psopool: guard not installed: the pool is not applied");
    PsoCompileThreadPoolTestReset();
    Check(InstallTaskWaitGuard(2), "taskwait: guard installed on the mapped exe (mode 2, for the pool tests)");
    SetPsoCompileThreadPool(3, -1);
    Check(*size == 0 && !PsoCompileThreadPoolApplied(), "psopool: guard in mode 2: the pool is not applied");
    SetTaskWaitGuardMode(1);
    PsoCompileThreadPoolTestReset();
    SetPsoCompileThreadPool(3, -1);
    Check(*size == 3 && PsoCompileThreadPoolApplied(), "psopool: guard in mode 1: the pool size is written at start-up");
    PsoCompileThreadPoolTick(5, -1);   // the ini changed while running
    PsoCompileThreadPoolTick(0, -1);
    Check(*size == 3 && PsoCompileThreadPoolApplied(), "psopool: later ini changes (5, 0) are ignored: restart needed");
    SetTaskWaitGuardMode(0);
    SetTaskWaitGuardMode(2);
    Check(TaskWaitGuardMode() == 1, "taskwait: the guard stays in mode 1 while the pool runs (a switch to 0 or 2 is refused)");
    *percent = 25;                     // the player's Engine.ini sets the percent after our write
    PsoCompileThreadPoolTick(3, -1);
    Check(*size == 0 && *percent == 25 && !PsoCompileThreadPoolApplied(), "psopool: Engine.ini percent set: our size is given back, theirs stays");
    SetTaskWaitGuardMode(2);
    Check(TaskWaitGuardMode() == 2, "taskwait: with the pool given back the mode can be switched again");
    SetTaskWaitGuardMode(1);
    *percent = 0; *size = 0;
    PsoCompileThreadPoolTestReset();
    SetPsoCompileThreadPool(-1, -1);
    const unsigned logical = UsableLogicalProcessors();
    Check(*size == static_cast<int32_t>(logical > 3 ? logical - 2 : 1), "psopool: -1 = logical processors - 2 (at least 1)");
    *size = 9;                         // the player's Engine.ini sets the size
    PsoCompileThreadPoolTick(-1, -1);
    Check(*size == 9 && !PsoCompileThreadPoolApplied(), "psopool: a size set by someone else is left alone");
    *size = 0;
    PsoCompileThreadPoolTestReset();
}

inline void TaskWaitSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    if (!Installed(FindTaskWaitGuard(), "TaskWaitDeadlockGuard: the task graph's worker wait found")) return;
    TaskWaitGuardInfoOut info{};
    Check(TaskWaitGuardInfo(&info), "taskwait: info");
    Check(info.wait == game.base + 0xDD66B0 && info.slot == reinterpret_cast<void**>(game.base + 0x6E0E188),
          "taskwait: WaitUntilTasksComplete exe+0xDD66B0, vtable slot exe+0x6E0E188");
    Check(info.doReserve == game.base + 0xDC08F0 && info.trigger == game.base + 0xDD4490,
          "taskwait: DoReserveWorkUntil exe+0xDC08F0, TriggerEventWhenTasksComplete exe+0xDD4490");
    Check(info.reserveScheduler == game.base + 0x8DA8D08 && info.scheduler == game.base + 0x8DA3180 &&
              info.tlsIndex == reinterpret_cast<const uint32_t*>(game.base + 0x94E3088),
          "taskwait: FReserveScheduler exe+0x8DA8D08, FScheduler exe+0x8DA3180, TLS index exe+0x94E3088");
    Check(info.flagOffset == 0x1280 && info.triggerThread == 0x6FF, "taskwait: reserve-workers flag at +0x1280, trigger thread 0x6FF");

    // PsoCompileThreadPool: the two console variables (found by their registration)
    int32_t *poolSize = nullptr, *poolPercent = nullptr, *poolPriority = nullptr;
    if (Installed(FindPsoCompileThreadPool() && PsoCompileThreadPoolInfo(&poolSize, &poolPercent, &poolPriority),
                  "PsoCompileThreadPool: r.pso.PrecompileThreadPool* variables found"))
        Check(poolSize == reinterpret_cast<int32_t*>(game.base + 0x9206128) && poolPercent == reinterpret_cast<int32_t*>(game.base + 0x920612C),
              "psopool: r.pso.PrecompileThreadPoolSize exe+0x9206128, PercentOfHardwareThreads exe+0x920612C");
    Check(poolPriority == reinterpret_cast<int32_t*>(game.base + 0x8DD7B8C), "psopool: r.pso.PrecompileThreadPoolThreadPriority exe+0x8DD7B8C");
    // the worker branch against the fakes
    TaskWaitGuardTestSet(reinterpret_cast<void*>(&FakeDoReserve), reinterpret_cast<void*>(&FakeTrigger),
                         reinterpret_cast<uint8_t*>(0x5678), 0x6FF);
    TaskWaitGuardTestCounters();
    bool taken = false;
    // the usual case: the trigger task comes at once
    g_triggerDelayMs = 0;
    double ms = RunWait(1, 3, 30, &taken);
    TaskWaitGuardCounters c = TaskWaitGuardTestCounters();
    Check(taken && ms >= 25 && ms < 1000 && c.orphans == 0, "taskwait: mode 1, trigger task on time: woken after the tasks, nothing left over");
    // the game's deadlock: the trigger task comes 600 ms late; the reserve worker wakes the waiter
    g_triggerDelayMs = 600;
    ms = RunWait(1, 6, 30, &taken);
    c = TaskWaitGuardTestCounters();
    Check(taken && ms >= 25 && ms < 400 && c.rescues == 1 && c.lateTriggers == 1 && c.orphans == 0,
          "taskwait: mode 1, trigger task 600 ms late (6 tasks, heap array): woken by the reserve worker, the late trigger only releases");
    // measure only: the waiter waits for the trigger task as in the game
    g_triggerDelayMs = 300;
    ms = RunWait(2, 2, 20, &taken);
    c = TaskWaitGuardTestCounters();
    Check(taken && ms >= 300 && c.rescues == 0 && c.lateTriggers == 1 && c.orphans == 0,
          "taskwait: mode 2, trigger task 300 ms late: woken only by the trigger task (the game's way), the delay counted");
    // tasks already complete: the game triggers at once inside TriggerEventWhenTasksComplete
    g_triggerImmediate = true;
    ms = RunWait(1, 1, 0, &taken);
    g_triggerImmediate = false;
    c = TaskWaitGuardTestCounters();
    Check(taken && ms < 1000 && c.orphans == 0, "taskwait: tasks already done, immediate trigger: returns, nothing left over");
    // no reserve worker free: not taken (the game's function busy-waits instead)
    g_reserveFree = false;
    ms = RunWait(1, 2, 0, &taken);
    g_reserveFree = true;
    Check(!taken, "taskwait: no reserve worker free: the wait is left to the game's function");
    // many waits in a row with random timings (lifetime and races)
    bool all = true;
    for (int i = 0; i < 40; ++i) {
        g_triggerDelayMs = (i * 7) % 5;
        ms = RunWait(i % 3 == 0 ? 2 : 1, 1 + i % 6, i % 4, &taken);
        all = all && taken && ms < 2000;
    }
    c = TaskWaitGuardTestCounters();
    Check(all && c.orphans == 0, "taskwait: 40 waits with mixed timings and modes: all woken, nothing left over");
    // the layout check of the stock condition (also done by FindTaskWaitGuard): passes as is, fails when its count offset changes
    {
        uint8_t* call = nullptr;
        bool good = TaskWaitGuardTestConditionCheck(&call), tampered = true;
        if (call) {
            DWORD old;
            if (VirtualProtect(call, 16, PAGE_EXECUTE_READWRITE, &old)) {
                const uint8_t keep = call[9];
                call[9] = 0x38;                    // cmp r8d,[rcx+38h] instead of +30h: another event-list layout
                tampered = TaskWaitGuardTestConditionCheck(nullptr);
                call[9] = keep;
                VirtualProtect(call, 16, old, &old);
            }
        }
        Check(good && !tampered && TaskWaitGuardTestConditionCheck(nullptr),
              "taskwait: the stock condition reads the event list as the guard does (+0 index, +0x28 heap, +0x30 count, bit 26); a changed layout is refused");
    }
    // the wake race at scale:
    StressTest();
    if (poolSize && poolPercent) PoolSelfTest(poolSize, poolPercent);   // last: installs the guard on the mapped exe
}

}  // namespace tw
