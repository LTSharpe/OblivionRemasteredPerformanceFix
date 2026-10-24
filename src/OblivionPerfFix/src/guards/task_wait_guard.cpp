// TaskWaitDeadlockGuard (1.1.10 lab, default 0): a task worker that waits for other tasks is woken by the reserve worker
// that covers for it, not only by a separate trigger task that may never get a thread (game bug: UE 5.3's task graph).
//
// UE 5.3 (TaskGraph.cpp, FTaskGraphCompatibilityImplementation::WaitUntilTasksComplete, TASKGRAPH_NEW_FRONTEND 0): a
// worker thread that has to wait for tasks wakes a "reserve worker" (FReserveScheduler::DoReserveWorkUntil) to use its
// core meanwhile, and then sleeps on an event (FScopedEvent) that a separate graph task, FTriggerEventGraphTask
// (TriggerEventWhenTasksComplete, AnyHiPriThreadHiPriTask), triggers once the awaited tasks are done. The reserve worker
// runs queued tasks only until the awaited tasks are complete (its condition); then it goes back to sleep. The trigger
// task becomes ready at exactly that moment, so it is normally run by some other worker. If every worker is in such a
// wait, nothing runs it: each worker sleeps until its trigger task runs, the trigger tasks wait for a free worker, and
// all reserve workers sleep because their conditions were met. Nothing wakes any of them again: a deadlock.
//
// Seen (worker coldfreeze, 2026-10-23): with a cold driver shader cache, ray tracing pipeline creation tasks
// (PipelineStateCache's FCompileRayTracingPipelineStateTask -> FD3D12RayTracingPipelineState, which waits for its
// shader collection compiles) occupied all 14 workers; the stock game's minidump after UE's 120 s render-thread
// timeout ("GameThread timed out waiting for RenderThread", RenderingThread.cpp:1272) shows all 14 workers asleep in
// that FScopedEvent and all 14 reserve workers idle: every awaited compile was done, only the trigger tasks were left.
// With 6 cores every cold start froze the same way at the first exterior teleport.
//
// The fix replaces the worker branch of WaitUntilTasksComplete (one vtable slot) with the same steps: wake a reserve
// worker (the game's DoReserveWorkUntil), queue the trigger task (the game's TriggerEventWhenTasksComplete), sleep
// until woken. Only the wake-up changes: the reserve worker's condition, when it finds the awaited tasks complete,
// also wakes the sleeping worker, and so does the trigger task (whichever comes first; the trigger task still runs
// later and only releases its reference). The event the trigger task sets is a small object of ours instead of a
// pooled FEvent, so a late trigger never touches an event that is already reused. Every task runs exactly as before
// (same code, same inputs); the waiting worker wakes when its tasks are done, never later than in the game.
// The awaited tasks are read from a copy of the caller's event list made when the wait starts (the stock lambda copies
// the list the same way): the reserve worker never touches the caller's own array. The copy holds plain pointers; the
// events stay alive because the caller holds them until the wait returns, and the wait cannot return while the reserve
// worker is reading them (state kReading below).
// Not taken over (the game's code as before): named threads (game/render thread), threads that are not task workers,
// a call that names a thread, reserve workers switched off ([TaskGraph] EnableReserveWorkers=False), and the case
// where no reserve worker is free (the game then busy-waits on the worker itself). A wait whose reserve worker was
// not free is tried once more by the game's own function, which may find one by then: that single wait runs entirely
// the game's way (unguarded); it cannot deadlock alone, a deadlock needs every worker in such a wait.
// Mode 2 is for measuring: the waiter is still woken by the trigger task only (the game's timing), but the wait runs
// through the guard's own condition, event and allocation, not the game's code (only mode 0 is the game's code). It
// times how long trigger tasks came after the awaited tasks were done.
// Start-up checks (all must hold, else the guard stays off with a log line): the worker-check pattern found exactly once,
// the function, its calls and its vtable slot as below, and the stock reserve condition (the lambda FReserveScheduler
// calls) reading the event list the way AllComplete does (inline pointers at +0, heap pointer at +0x20, count at +0x28,
// IsComplete = bit 26 of the event's first word).
// PsoCompileThreadPool (pso_pool.cpp) is applied only while this guard is installed in mode 1; the guard keeps mode 1
// for as long as that pool runs (a live switch to 0 or 2 is refused with a log line).
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <new>

namespace opf {
namespace {

// The worker branch's checks (build 19115871: WaitUntilTasksComplete exe+0xDD66B0, this block at exe+0xDD67D3):
//   mov r13d,edi; mov ecx,[TLS index]; lea r15,[FReserveScheduler]; mov rax,gs:[58h]; mov rdx,[rax+rcx*8];
//   mov eax,38h; mov ecx,[rax+rdx] (WorkerType); mov eax,50h; add rax,rdx (ActiveScheduler); lea rdx,[FScheduler];
//   test ecx,ecx; je; cmp [rax],rdx; je; test ecx,ecx; je; cmp [rax],r15; jne; movzx eax,byte [r12+flag] (reserve
//   workers enabled); xor edi,edi; test al,al; je
constexpr char kWorkerCheck[] =
    "44 8B EF 8B 0D ?? ?? ?? ?? 4C 8D 3D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 48 8B 14 C8 B8 38 00 00 00 8B 0C 10 "
    "B8 50 00 00 00 48 03 C2 48 8D 15 ?? ?? ?? ?? 85 C9 0F 84 ?? ?? ?? ?? 48 39 10 74 ?? 85 C9 0F 84 ?? ?? ?? ?? "
    "4C 39 38 0F 85 ?? ?? ?? ?? 41 0F B6 84 24 ?? ?? ?? ?? 33 FF 84 C0 0F 84";
constexpr size_t kTlsLoad = 3, kReserveLea = 9, kSchedulerLea = 45, kFlagOffset = 87;
// The function's prologue (searched back from the check): mov [rsp+20h],rbx; push rbp,rsi,rdi,r12-r15; lea rbp,[rsp-x];
// sub rsp,imm32
constexpr char kPrologue[] = "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC";
// The reserve worker: lea rax,[condition lambda vtable]; lea r8,[rbp+x]; mov [rbp+x],rax; lea rdx; lea rcx;
// call (lambda construct); lea rdx,[rbp+x]; mov rcx,r15; call DoReserveWorkUntil
constexpr char kReserveCall[] =
    "48 8D 05 ?? ?? ?? ?? 4C 8D 45 ?? 48 89 45 ?? 48 8D 55 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 8D 55 ?? 49 8B CF E8";
constexpr size_t kReserveCallAt = 35;
// The stock condition lambda (the one that call constructs): the constructor (kReserveCall + 23) stores the lambda's
// delegate vtable near its start (lea rax,[vtable]; mov [rcx],rax) and slot 1 of that vtable is the lambda's Call (build
// 19115871: ctor exe+0xDAD7F0, vtable exe+0x6E0EBF0, Call exe+0xDBBC00). Call reads [Index(+0), Tasks(+8: inline pointers,
// heap pointer at +0x28, count at +0x30)] exactly as AllComplete does: mov rcx,[rdx]; mov r8d,[rcx]; cmp r8d,[rcx+30h];
// mov rax,[rcx+28h]; lea rdx,[rcx+8]; ... lock cmpxchg [rdx],r10 (atomic read of the event's first word); shr rax,1Ah;
// test al,1 (IsComplete). Each piece must be found once in the first 0x60 bytes.
constexpr size_t kReserveCtorCallAt = 23;
constexpr char kCtorVtable[] = "48 8D 05 ?? ?? ?? ?? 48 89 01";
constexpr size_t kCtorSearch = 0x80, kCondSearch = 0x60;
constexpr char kCondStart[] = "48 8B 0A 44 8B 01";                 // lambda = *data; r8d = Index
constexpr char kCondNum[] = "44 3B 41 30 7D ??";                    // cmp r8d,[lambda+30h]; jge: Num (the loop test repeats it with jl)
constexpr char kCondArray[] = "48 8B 41 28 48 8D 51 08";            // heap pointer [lambda+28h], inline storage lambda+8
constexpr char kCondComplete[] = "F0 4C 0F B1 12 48 C1 E8 1A A8 01";  // locked read of the first word; bit 26
// The non-worker wait: lea rcx,[rbp+x] (FScopedEvent); call ctor; mov rdx,[rbp+x]; mov r9d,r13d; mov r8,rsi;
// mov dword [rsp+20h],TriggerThread; mov rcx,r12; call TriggerEventWhenTasksComplete; lea rcx; call ~FScopedEvent
constexpr char kTriggerCall[] =
    "48 8D 4D ?? E8 ?? ?? ?? ?? 48 8B 55 ?? 45 8B CD 4C 8B C6 C7 44 24 20 ?? ?? ?? ?? 49 8B CC E8 ?? ?? ?? ?? 48 8D 4D ?? E8";
constexpr size_t kTriggerThreadAt = 23, kTriggerCallAt = 30;
constexpr size_t kMaxPrologueBack = 0x400, kMaxFunction = 0x600;

using WaitFn = void (*)(void* self, const void* tasks, uint32_t currentThreadIfKnown);
using ReserveFn = bool (*)(void* reserveScheduler, void* condition);  // FReserveScheduler::DoReserveWorkUntil(FConditional&&)
using TriggerFn = void (*)(void* self, void* event, const void* tasks, uint32_t currentThreadIfKnown, uint32_t triggerThread);

struct Found {
    uint8_t* wait = nullptr;
    void** slot = nullptr;
    uint8_t* doReserve = nullptr;
    uint8_t* trigger = nullptr;
    uint8_t* reserveScheduler = nullptr;
    uint8_t* scheduler = nullptr;
    const uint32_t* tlsIndex = nullptr;
    uint32_t flagOffset = 0;
    uint32_t triggerThread = 0;
};
Found g_found;
bool g_findDone = false, g_findOk = false;
WaitFn g_orig = nullptr;
ReserveFn g_doReserve = nullptr;
TriggerFn g_trigger = nullptr;
std::atomic<int> g_mode{0};
bool g_installed = false;

// ---- statistics
std::atomic<uint64_t> g_waits{0}, g_fallbacks{0}, g_rescues{0}, g_lateTriggers{0}, g_lag100{0}, g_lag1000{0}, g_lagMaxUs{0},
    g_waitMaxUs{0};
// Diagnostics only (the 0 -> 1 and 1 -> 0 transitions of the gauge happen on different threads, so the "since" time and the
// maximum are approximate); the wake-up itself does not depend on them.
std::atomic<int64_t> g_orphans{0};       // waits whose tasks were seen done while their trigger task has not run yet
std::atomic<int64_t> g_orphansMax{0};
std::atomic<uint64_t> g_orphanSinceUs{0};  // when the gauge last went from 0 to 1 (0 = at zero)

uint64_t NowUs()
{
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return QpcToMicroseconds(static_cast<uint64_t>(c.QuadPart), freq);
}
void RaiseMax(std::atomic<uint64_t>& m, uint64_t v)
{
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
void OrphanUp()
{
    const int64_t n = g_orphans.fetch_add(1) + 1;
    if (n == 1) g_orphanSinceUs.store(NowUs());
    int64_t m = g_orphansMax.load();
    while (n > m && !g_orphansMax.compare_exchange_weak(m, n)) {}
}
void OrphanDown()
{
    if (g_orphans.fetch_sub(1) == 1) g_orphanSinceUs.store(0);
}

// ---- one wait. States: 0 waiting, 1 the reserve worker is reading the task list, 2 woken.
constexpr uint32_t kWaiting = 0, kReading = 1, kWoken = 2;
struct WaitCtx;
struct FakeEvent {               // what the trigger task gets as its FEvent*: only Trigger (vtable +0x10) is called
    const void* const* vtable;
    WaitCtx* ctx;
};
std::atomic<int64_t> g_liveCtx{0};  // WaitCtx objects alive (self-test: must return to 0)
constexpr int kInlineEvents = 8;
struct WaitCtx {
    std::atomic<uint32_t> state{kWaiting};
    std::atomic<int> refs{3};    // the waiter, the reserve worker's condition, the trigger
    const void* const* events = nullptr;  // copy of the waiter's event pointers (read only in state kReading)
    int num = 0;
    int index = 0;               // first task not yet seen complete
    int mode = 1;
    uint64_t seenUs = 0;         // when the condition found the tasks complete (0 = not yet)
    FakeEvent event{};
    const void* inlineEvents[kInlineEvents] = {};
    const void** heapEvents = nullptr;
    WaitCtx() { g_liveCtx.fetch_add(1, std::memory_order_relaxed); }
    ~WaitCtx() { delete[] heapEvents; g_liveCtx.fetch_sub(1, std::memory_order_relaxed); }
    WaitCtx(const WaitCtx&) = delete;
    WaitCtx& operator=(const WaitCtx&) = delete;
};

void Release(WaitCtx* c)
{
    if (c->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete c;
}
void Wake(WaitCtx* c)
{
    c->state.store(kWoken, std::memory_order_seq_cst);
    WakeWordAll(&c->state);
}

// FGraphEventArray = TArray<FGraphEventRef, TInlineAllocator<4>>: 4 inline pointers, heap pointer at +0x20, count at
// +0x28 (the layout the stock condition lambda reads, checked at start-up). Copies the event pointers (the stock lambda
// copies the array when the reserve worker is armed). False: not copied (nothing allocated, nothing changed).
constexpr size_t kArrayHeap = 0x20, kArrayNum = 0x28;
bool CopyEvents(WaitCtx* c, const uint8_t* arr)
{
    void* const* heap = *reinterpret_cast<void* const* const*>(arr + kArrayHeap);
    void* const* data = heap ? heap : reinterpret_cast<void* const*>(arr);
    const int num = *reinterpret_cast<const int*>(arr + kArrayNum);
    if (num < 0 || num > (1 << 20)) return false;
    const void** dst = c->inlineEvents;
    if (num > kInlineEvents) {
        c->heapEvents = new (std::nothrow) const void*[static_cast<size_t>(num)];
        if (!c->heapEvents) return false;
        dst = c->heapEvents;
    }
    for (int i = 0; i < num; ++i) dst[i] = data[i];
    c->events = dst;
    c->num = num;
    return true;
}

// FGraphEvent::IsComplete: its subsequent list's head word has bit 26 (closed) set; a null entry counts as done.
bool AllComplete(WaitCtx* c)
{
    for (; c->index < c->num; ++c->index) {
        const void* ev = c->events[c->index];
        if (!ev) continue;
        const uint64_t head = *static_cast<const volatile uint64_t*>(ev);
        if (((head >> 26) & 1) == 0) return false;
    }
    return true;
}

// The reserve worker's condition (called after each task it runs and while it spins).
bool ConditionCall(WaitCtx* c)
{
    uint32_t expected = kWaiting;
    if (!c->state.compare_exchange_strong(expected, kReading, std::memory_order_acq_rel)) return true;  // already woken
    if (!AllComplete(c)) {
        c->state.store(kWaiting, std::memory_order_release);
        return false;
    }
    c->seenUs = NowUs();
    OrphanUp();                  // done, the trigger task has not run yet (it would have woken the waiter)
    if (c->mode == 1) {
        g_rescues.fetch_add(1, std::memory_order_relaxed);
        Wake(c);
    } else {
        c->state.store(kWaiting, std::memory_order_release);
    }
    return true;
}

// The trigger task's Event->Trigger().
void __fastcall FakeTrigger(FakeEvent* ev)
{
    WaitCtx* c = ev->ctx;
    for (int spins = 0;; ++spins) {
        uint32_t expected = kWaiting;
        if (c->state.compare_exchange_strong(expected, kWoken, std::memory_order_acq_rel)) {
            WakeWordAll(&c->state);
            break;
        }
        if (expected == kWoken) break;
        // the reserve worker is reading the task list (a few loads): spin briefly, then give the processor away in case
        // that thread was preempted inside the window (a saturated PC), so this worker does not burn a whole time slice
        if (spins < 64) _mm_pause();
        else SwitchToThread();
    }
    if (c->seenUs) {             // set before the condition gave up its read state (ordered by the CAS above)
        OrphanDown();
        const uint64_t lag = NowUs() - c->seenUs;
        g_lateTriggers.fetch_add(1, std::memory_order_relaxed);
        if (lag >= 100000) g_lag100.fetch_add(1, std::memory_order_relaxed);
        if (lag >= 1000000) g_lag1000.fetch_add(1, std::memory_order_relaxed);
        RaiseMax(g_lagMaxUs, lag);
    }
    Release(c);
}
uint64_t __fastcall FakeEventOther(FakeEvent*)
{
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) Log("[taskwait] unexpected FEvent call on the guard's event (ignored)");
    return 0;
}
const void* const g_eventVtable[16] = {
    reinterpret_cast<const void*>(&FakeEventOther), reinterpret_cast<const void*>(&FakeEventOther),
    reinterpret_cast<const void*>(&FakeTrigger),    reinterpret_cast<const void*>(&FakeEventOther),
    reinterpret_cast<const void*>(&FakeEventOther), reinterpret_cast<const void*>(&FakeEventOther),
    reinterpret_cast<const void*>(&FakeEventOther), reinterpret_cast<const void*>(&FakeEventOther),
    reinterpret_cast<const void*>(&FakeEventOther), reinterpret_cast<const void*>(&FakeEventOther),
    reinterpret_cast<const void*>(&FakeEventOther), reinterpret_cast<const void*>(&FakeEventOther),
    reinterpret_cast<const void*>(&FakeEventOther), reinterpret_cast<const void*>(&FakeEventOther),
    reinterpret_cast<const void*>(&FakeEventOther), reinterpret_cast<const void*>(&FakeEventOther)};

// LowLevelTasks::TTaskDelegate<bool(), 16> (FReserveScheduler::FConditional): 8 bytes of inline storage at +0 (here:
// the WaitCtx pointer), then the type-erased wrapper (a vtable pointer) at +8; the wrapper's virtuals get the wrapper
// as `this` and the inline storage as a parameter: Move, Call, CallAndMove, Destroy, IsHeapAllocated, IsSet, DelegateSize.
extern const void* const g_condVtable[7];
struct Condition {
    WaitCtx* ctx;                // inline storage
    const void* const* vtable;   // CallableWrapper
};
void __fastcall CondMove(void* /*self*/, void* dstWrapper, void* dstData, void* srcData, uint32_t /*dstInlineSize*/)
{
    *static_cast<const void* const**>(dstWrapper) = g_condVtable;
    memcpy(dstData, srcData, sizeof(WaitCtx*));
    memset(srcData, 0, sizeof(WaitCtx*));  // the moved-from delegate holds nothing (its Call answers "done")
}
bool __fastcall CondCall(const void* /*self*/, void* data)
{
    WaitCtx* c;
    memcpy(&c, data, sizeof c);
    if (!c) return true;
    if (!ConditionCall(c)) return false;
    memset(data, 0, sizeof c);   // answered "done" once: give up the reference (the game never asks again)
    Release(c);
    return true;
}
bool __fastcall CondCallAndMove(void* self, uint8_t* destination, void* data, uint32_t destInlineSize)
{
    const bool r = CondCall(self, data);
    CondMove(self, destination + destInlineSize, destination, data, destInlineSize);
    return r;
}
void __fastcall CondDestroy(void*, void* data)
{
    WaitCtx* c;
    memcpy(&c, data, sizeof c);
    if (c) {                     // destroyed without having answered "done" (reserve workers stopping at exit)
        memset(data, 0, sizeof c);
        Release(c);
    }
}
bool __fastcall CondIsHeapAllocated(const void*) { return false; }
// As the stock delegate: a moved-from (or answered) one holds nothing. `self` is the wrapper at +8 of the delegate.
bool __fastcall CondIsSet(const void* self) { return *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(self) - 8) != nullptr; }
uint32_t __fastcall CondDelegateSize(const void*) { return sizeof(WaitCtx*); }
const void* const g_condVtable[7] = {
    reinterpret_cast<const void*>(&CondMove),    reinterpret_cast<const void*>(&CondCall),
    reinterpret_cast<const void*>(&CondCallAndMove), reinterpret_cast<const void*>(&CondDestroy),
    reinterpret_cast<const void*>(&CondIsHeapAllocated), reinterpret_cast<const void*>(&CondIsSet),
    reinterpret_cast<const void*>(&CondDelegateSize)};

// The worker branch with a reserve worker. Returns false when no reserve worker was free (nothing changed: the caller
// runs the game's function, which then busy-waits on this thread).
bool ReserveWait(void* self, const void* tasks, int mode)
{
    WaitCtx* c = new (std::nothrow) WaitCtx;
    if (!c) return false;
    if (!CopyEvents(c, static_cast<const uint8_t*>(tasks))) {  // copied now, as the stock lambda copies the array
        delete c;
        return false;
    }
    c->mode = mode;
    c->event.vtable = g_eventVtable;
    c->event.ctx = c;
    Condition cond{c, g_condVtable};
    if (!g_doReserve(g_found.reserveScheduler, &cond)) {  // no reserve worker free: the condition was not taken
        delete c;
        return false;
    }
    const uint64_t t0 = NowUs();
    g_trigger(self, &c->event, tasks, 0xFF, g_found.triggerThread);
    for (;;) {
        uint32_t v = c->state.load(std::memory_order_acquire);
        if (v == kWoken) break;
        BlockOnWord(&c->state, &v, sizeof v);
    }
    RaiseMax(g_waitMaxUs, NowUs() - t0);
    Release(c);
    return true;
}

bool IsTaskWorker()
{
    const auto* tlsArray = reinterpret_cast<uint8_t* const*>(__readgsqword(0x58));
    const uint8_t* block = tlsArray[*g_found.tlsIndex];
    if (!block) return false;
    const int workerType = *reinterpret_cast<const int*>(block + 0x38);
    const void* active = *reinterpret_cast<const void* const*>(block + 0x50);
    return workerType != 0 && (active == g_found.scheduler || active == g_found.reserveScheduler);
}

void WaitHook(void* self, const void* tasks, uint32_t currentThreadIfKnown)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode != 0 && (currentThreadIfKnown & 0xFF) == 0xFF && static_cast<const uint8_t*>(self)[g_found.flagOffset] != 0 &&
        IsTaskWorker()) {
        g_waits.fetch_add(1, std::memory_order_relaxed);
        if (ReserveWait(self, tasks, mode)) return;
        g_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    g_orig(self, tasks, currentThreadIfKnown);
}

// The stock reserve condition (the lambda the worker branch hands to DoReserveWorkUntil) must read the awaited events the
// way AllComplete does. Follows the lambda's constructor call (reserveCall + 23) to its delegate vtable and checks the
// bytes of slot 1 (the lambda's Call): [Index at +0, event array at +8 (inline pointers, heap pointer +0x28, count
// +0x30)], the event's first word read atomically, IsComplete = bit 26. A game update that changes any of these leaves the
// guard off (logged) instead of waiting on garbage.
uint8_t* g_lastCondCall = nullptr;   // the stock Call last examined (self-test)
bool StockConditionReadsEventsLikeUs(const GameModule& game, const uint8_t* reserveCall)
{
    const uint8_t* ctor = RipTarget(reserveCall + kReserveCtorCallAt, 1, 5);
    const char* why = "its constructor was not found";
    if (game.text.Contains(ctor)) {
        uint8_t* ctorEnd = const_cast<uint8_t*>(ctor) + kCtorSearch;
        if (ctorEnd > game.text.end) ctorEnd = game.text.end;
        const auto vt = FindAll(Section{const_cast<uint8_t*>(ctor), ctorEnd}, ParsePattern(kCtorVtable));
        why = "its delegate vtable was not found";
        if (vt.size() == 1) {
            const uint8_t* table = RipTarget(vt[0], 3, 7);
            why = "its Call was not found";
            if (game.rdata.Contains(table) && table + 16 <= game.rdata.end) {
                uint8_t* call = reinterpret_cast<uint8_t* const*>(table)[1];
                if (game.text.Contains(call)) {
                    g_lastCondCall = call;
                    uint8_t* callEnd = call + kCondSearch;
                    if (callEnd > game.text.end) callEnd = game.text.end;
                    const Section body{call, callEnd};
                    why = "its Call does not read the event list as expected";
                    if (MatchesAt(call, ParsePattern(kCondStart)) && FindAll(body, ParsePattern(kCondNum)).size() == 1 &&
                        FindAll(body, ParsePattern(kCondArray)).size() == 1 && FindAll(body, ParsePattern(kCondComplete)).size() == 1)
                        return true;
                }
            }
        }
    }
    Log("[taskwait] the game's wait condition is not as expected (%s): TaskWaitDeadlockGuard unavailable", why);
    return false;
}

}  // namespace

bool FindTaskWaitGuard()
{
    if (g_findDone) return g_findOk;
    g_findDone = true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kWorkerCheck));
    if (hits.size() != 1) {
        Log("[taskwait] the task graph's worker wait was found %zu times (expected once): TaskWaitDeadlockGuard unavailable", hits.size());
        return false;
    }
    uint8_t* check = hits[0];
    const Pattern prologue = ParsePattern(kPrologue);
    uint8_t* fn = nullptr;
    for (uint8_t* p = check; p > check - kMaxPrologueBack && p > game.text.begin; --p)
        if (MatchesAt(p, prologue)) { fn = p; break; }
    Found f;
    f.tlsIndex = reinterpret_cast<const uint32_t*>(RipTarget(check + kTlsLoad, 2, 6));
    f.reserveScheduler = RipTarget(check + kReserveLea, 3, 7);
    f.scheduler = RipTarget(check + kSchedulerLea, 3, 7);
    memcpy(&f.flagOffset, check + kFlagOffset, 4);
    if (!fn || !game.data.Contains(f.tlsIndex) || !game.data.Contains(f.reserveScheduler) || !game.data.Contains(f.scheduler) ||
        f.flagOffset == 0 || f.flagOffset > 0x10000) {
        Log("[taskwait] the worker wait at exe+0x%X is not as expected: TaskWaitDeadlockGuard unavailable",
            static_cast<unsigned>(check - game.base));
        return false;
    }
    Section body{fn, fn + kMaxFunction};
    if (body.end > game.text.end) body.end = game.text.end;
    const auto rc = FindAll(body, ParsePattern(kReserveCall));
    const auto tc = FindAll(body, ParsePattern(kTriggerCall));
    if (rc.size() != 1 || tc.size() != 1) {
        Log("[taskwait] reserve/trigger calls found %zu/%zu times in the wait at exe+0x%X: TaskWaitDeadlockGuard unavailable",
            rc.size(), tc.size(), static_cast<unsigned>(fn - game.base));
        return false;
    }
    if (!StockConditionReadsEventsLikeUs(game, rc[0])) return false;
    f.wait = fn;
    f.doReserve = RipTarget(rc[0] + kReserveCallAt, 1, 5);
    f.trigger = RipTarget(tc[0] + kTriggerCallAt, 1, 5);
    memcpy(&f.triggerThread, tc[0] + kTriggerThreadAt, 4);
    // the vtable slot: exactly one pointer to the function in .rdata, and the next slot is TriggerEventWhenTasksComplete
    void** slot = nullptr;
    size_t slots = 0;
    {
        char hex[8 * 3 + 1] = {};
        const auto v = reinterpret_cast<uintptr_t>(fn);
        for (int i = 0; i < 8; ++i) snprintf(hex + i * 3, 4, i < 7 ? "%02X " : "%02X", static_cast<unsigned>((v >> (8 * i)) & 0xFF));
        for (uint8_t* p : FindAll(game.rdata, ParsePattern(hex)))
            if ((reinterpret_cast<uintptr_t>(p) & 7) == 0 && p + 16 <= game.rdata.end) { slot = reinterpret_cast<void**>(p); ++slots; }
    }
    if (slots != 1 || slot[1] != f.trigger || !game.text.Contains(f.doReserve) || !game.text.Contains(f.trigger) ||
        (f.triggerThread & 0xFF) != 0xFF) {
        Log("[taskwait] the wait's vtable slot (%zu found) or its calls are not as expected: TaskWaitDeadlockGuard unavailable", slots);
        return false;
    }
    f.slot = slot;
    g_found = f;
    g_findOk = true;
    Log("[taskwait] worker wait exe+0x%X (vtable slot exe+0x%X), DoReserveWorkUntil exe+0x%X, TriggerEventWhenTasksComplete exe+0x%X",
        static_cast<unsigned>(fn - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(slot) - game.base),
        static_cast<unsigned>(f.doReserve - game.base), static_cast<unsigned>(f.trigger - game.base));
    return true;
}

bool InstallTaskWaitGuard(int mode)
{
    if (g_installed) { SetTaskWaitGuardMode(mode); return true; }
    if (!FindTaskWaitGuard()) return false;
    g_orig = reinterpret_cast<WaitFn>(g_found.wait);
    g_doReserve = reinterpret_cast<ReserveFn>(g_found.doReserve);
    g_trigger = reinterpret_cast<TriggerFn>(g_found.trigger);
    g_mode = mode;
    if (!WritePointer(g_found.slot, reinterpret_cast<void*>(&WaitHook))) {
        Log("[taskwait] VirtualProtect failed: TaskWaitDeadlockGuard not installed");
        return false;
    }
    g_installed = true;
    Log("[taskwait] installed, mode %d (%s)", mode, mode == 1 ? "a waiting worker is also woken by its reserve worker" : "measure only");
    return true;
}

int TaskWaitGuardMode() { return g_installed ? g_mode.load() : -1; }

void SetTaskWaitGuardMode(int mode)
{
    if (!g_installed || g_mode.load() == mode) return;
    if (mode != 1 && PsoCompileThreadPoolApplied()) {  // the pool alone runs into the deadlock this guard prevents
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            Log("[taskwait] mode %d ignored: PsoCompileThreadPool is running and needs mode 1 (a restart changes both)", mode);
        return;
    }
    g_mode = mode;
    Log("[taskwait] mode %d (%s)", mode,
        mode == 1 ? "a waiting worker is also woken by its reserve worker" : mode == 2 ? "measure only" : "the game's wait");
}

void TaskWaitGuardTick()
{
    static uint64_t lastLogUs = 0;
    const uint64_t since = g_orphanSinceUs.load();
    if (!g_installed || !since) return;
    const uint64_t now = NowUs();
    if (now - since >= 1000000 && now - lastLogUs >= 5000000) {
        lastLogUs = now;
        Log("[taskwait] %lld worker wait(s) whose tasks are done have had no trigger task run for %.1f s (about; mode %d: %s)",
            static_cast<long long>(g_orphans.load()), (now - since) / 1e6, g_mode.load(),
            g_mode.load() == 1 ? "woken by their reserve workers" : "still asleep: the game's deadlock");
    }
}

void LogTaskWaitGuardStats()
{
    if (!g_installed) return;
    const uint64_t w = g_waits.exchange(0), fb = g_fallbacks.exchange(0), r = g_rescues.exchange(0), lt = g_lateTriggers.exchange(0),
                   l1 = g_lag100.exchange(0), l2 = g_lag1000.exchange(0), lm = g_lagMaxUs.exchange(0), wm = g_waitMaxUs.exchange(0);
    const int64_t om = g_orphansMax.exchange(g_orphans.load());
    if (w || lt)
        Log("[taskwait] worker waits %llu (no reserve free %llu), woken by the reserve worker first %llu; trigger tasks after the "
            "tasks were done %llu (>= 100 ms %llu, >= 1 s %llu, longest %.1f ms), most at once %lld, longest wait %.1f ms",
            static_cast<unsigned long long>(w), static_cast<unsigned long long>(fb), static_cast<unsigned long long>(r),
            static_cast<unsigned long long>(lt), static_cast<unsigned long long>(l1), static_cast<unsigned long long>(l2), lm / 1000.0,
            static_cast<long long>(om), wm / 1000.0);
}

bool TaskWaitGuardInfo(TaskWaitGuardInfoOut* out)
{
    if (!g_findOk) return false;
    out->wait = g_found.wait;
    out->slot = g_found.slot;
    out->doReserve = g_found.doReserve;
    out->trigger = g_found.trigger;
    out->reserveScheduler = g_found.reserveScheduler;
    out->scheduler = g_found.scheduler;
    out->tlsIndex = g_found.tlsIndex;
    out->flagOffset = g_found.flagOffset;
    out->triggerThread = g_found.triggerThread;
    return true;
}

void TaskWaitGuardTestSet(void* doReserve, void* trigger, uint8_t* reserveScheduler, uint32_t triggerThread)
{
    g_doReserve = reinterpret_cast<ReserveFn>(doReserve);
    g_trigger = reinterpret_cast<TriggerFn>(trigger);
    g_found.reserveScheduler = reserveScheduler;
    g_found.triggerThread = triggerThread;
}

bool TaskWaitGuardTestWait(void* self, const void* tasks, int mode) { return ReserveWait(self, tasks, mode); }

bool TaskWaitGuardTestConditionCheck(uint8_t** call)
{
    if (!g_findOk) return false;
    const auto hits = FindAll(Game().text, ParsePattern(kWorkerCheck));
    uint8_t* fn = g_found.wait;
    const auto rc = FindAll(Section{fn, fn + kMaxFunction}, ParsePattern(kReserveCall));
    if (hits.size() != 1 || rc.size() != 1) return false;
    const bool ok = StockConditionReadsEventsLikeUs(Game(), rc[0]);
    if (call) *call = g_lastCondCall;
    return ok;
}

int64_t TaskWaitGuardTestLiveWaits() { return g_liveCtx.load(); }

TaskWaitGuardCounters TaskWaitGuardTestCounters()
{
    TaskWaitGuardCounters c;
    c.rescues = g_rescues.exchange(0);
    c.lateTriggers = g_lateTriggers.exchange(0);
    c.orphans = g_orphans.load();
    return c;
}

}  // namespace opf
