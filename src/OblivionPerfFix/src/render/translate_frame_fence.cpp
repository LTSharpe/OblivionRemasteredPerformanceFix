// Guard for RdgParallelTranslate (parallel translate): every frame's start is ordered before that frame's parallel translates,
// as in Unreal Engine 5.5, so the D3D12 buffer-pool defragmentation can run while parallel translate is on.
//
// Why (translate_defrag_guard.cpp): the RHI thread defragments buffer pools at the start of every frame
// (FD3D12CommandContext::RHIBeginFrame -> FD3D12DefaultBufferAllocator::BeginFrame, up to 32 MB of live buffers moved
// per frame). With parallel translate a worker can translate a pass set of frame N before the RHI thread has run frame
// N's RHIBeginFrame, so the pass could use a buffer's old location, which the defragmentation copies away from (writes
// lost) and frees a frame later. The defrag guard therefore pauses buffer defragmentation while parallel translate is on, which
// costs ~0.37 GB of video memory (partly used pools are no longer compacted; measured 2026-10-07).
//
// UE 5.5 queues the frame's BeginFrame work with an RHI thread fence (FD3D12DynamicRHI::RHIEndFrame_RenderThread:
// EnqueueLambdaMultiPipe(..., EThreadFence::Enabled, "D3D12 BeginFrame")), so parallel translates queued after it
// start only once the RHI thread has run it. UE 5.3 has the same mechanism: FRHICommandListBase::RHIThreadFence(true)
// queues a fence command on the immediate command list and makes its event the RHIThreadBufferLockFence that every
// later parallel translate task gets as a prerequisite (FRHICommandListImmediate::QueueAsyncCommandListSubmit chains
// fence candidates to it, so they complete no earlier); 5.3's own FRHICommandListImmediate::EndFrame calls it when
// fence candidates are queued. This guard calls it right after every FRHICommandListImmediate::BeginFrame (a detour at
// its entry; it runs on the render thread, like EndFrame):
//  - a translate task queued after BeginFrame N starts only after the RHI thread has executed BeginFrame N, and so
//    after frame N's defragmentation;
//  - the RHI thread reaches BeginFrame N+1 only after the submit command of every translate task queued before it,
//    and that command waits for its task: no translate task runs while a frame's defragmentation does.
// Once the first fence after switching on has completed (its event is complete), every BeginFrame the RHI thread runs
// from then on is followed by a fence, and the defrag guard lets buffer defragmentation run with parallel translate on.
// What remains is the engine's own rare race between defragmentation and view creation (UE-217692, fixed by Epic in 5.5
// by creating views on the RHI thread), which the stock game, with defragmentation always on, has as well. ViewRenameGuard
// (view_rename_guard.cpp) closes it, and the fence is used only while ViewRenameGuard is installed and on.
//
// Measured: 2026-10-09 (cpu profile, six scenes, three fresh launches per arm): it works (buffer moves 245-289/s with
// it, 0/s with the pause), costs nothing measurable (RHI/render thread and GPU ms within noise); 8940 vs 8990 MB mean.
// 2026-10-12, interleaved fresh launches: -0.24 GB at 720p (8.18-8.35 vs 8.42-8.54 GB, 3 + 3), -0.21 GB at 1440p at a
// 12 GB card's budget (2 + 2), -0.0 to -0.2 GB at 4K with frame generation, the same frame rate and images. On by default
// ([Fixes] RdgFrameFence=0 goes back to the pause; the detour is only placed when it is used).
//
// Found by shape (build 19115871 addresses in the comments):
//  - BeginFrameRenderThread's render command (exe+0x39095C0): call LatchBypass; inc GFrameNumberRenderThread;
//    mov rcx,rdi; call FRHICommandListImmediate::BeginFrame; lea rcx,[OnBeginFrameRT]; call Broadcast; cmp byte
//    [rdi+X],0 (Bypass) - exactly one match;
//  - BeginFrame (exe+0x2652B70): prologue whose first instruction is the 5-byte mov [rsp+8],rbx (the detour's
//    place), then the ALLOC_COMMAND of a 16-byte command and its vtable (FRHICommandBeginFrame, exe+0x7286F28), whose
//    executor calls ProcessStats and tail-jumps through the command context's vtable (RHIBeginFrame);
//  - FRHICommandListImmediate::EndFrame (exe+0x2655730): if (QueuedFenceCandidates.Num) RHIThreadFence(true) and the
//    returned FGraphEventRef released (lock xadd [ev+RefCount],-1; last -> recycle), then the ALLOC_COMMAND of
//    FRHICommandEndFrame, whose vtable must be BeginFrame's + 0x10 (the two commands are declared together);
//  - FGraphEvent::IsComplete's "closed" bit from its inlined copies (all must agree).
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

constexpr char kRenderThreadCall[] =
    "E8 ?? ?? ?? ?? FF 05 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 BF ?? ?? ?? ?? 00";
constexpr size_t kRenderThreadCallBeginFrame = 14;
constexpr char kBeginFrame[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 91 ?? ?? ?? ?? 48 8B F9 48 83 C2 07 48 83 E2 F8 48 8D 42 10 "
    "48 3B 81 ?? ?? ?? ?? 76 ?? BA 18 00 00 00";
constexpr char kCommandVtableStore[] = "48 8D 05 ?? ?? ?? ?? 48 89 02";  // lea rax,[vtable]; mov [rdx],rax
// ... then: cmp [GIsRunningRHIInSeparateThread],bl; jne (no flush) - read by the self-test only
constexpr char kRhiThreadCheck[] = "38 1D ?? ?? ?? ?? 75";
constexpr char kEndFrame[] =
    "48 89 5C 24 18 57 48 83 EC 20 83 B9 ?? ?? ?? ?? 00 48 8B D9 74 ?? 41 B0 01 48 8D 54 24 ?? E8 ?? ?? ?? ?? "
    "48 8B 44 24 ?? 48 85 C0 74 ?? BA FF FF FF FF F0 0F C1 50 ?? 83 FA 01 75 ?? 48 8B 4C 24 ?? E8 ?? ?? ?? ??";
constexpr size_t kEndFrameFenceCall = 0x1E, kEndFrameRefCount = 0x36, kEndFrameRecycleCall = 0x41;
constexpr char kEndCommandVtableStore[] = "48 8D 05 ?? ?? ?? ?? 48 89 01";  // lea rax,[vtable]; mov [rcx],rax
// FRHICommandBeginFrame's executor: push rbx; sub rsp,20h; mov rcx,rdx; mov rbx,rdx; call ProcessStats;
// mov rcx,[rbx+GraphicsContext]; mov rax,[rcx]; add rsp,20h; pop rbx; jmp [rax+RHIBeginFrame]
constexpr char kExecutor[] = "40 53 48 83 EC 20 48 8B CA 48 8B DA E8 ?? ?? ?? ?? 48 8B 4B ?? 48 8B 01 48 83 C4 20 5B 48 FF A0";
// FGraphEvent::IsComplete inlined: atomic read of the subsequents list head, then the "closed" bit
constexpr char kIsComplete[] = "48 85 C9 74 ?? 33 C0 F0 4C 0F B1 29 48 C1 E8 ?? A8 01";
constexpr uint8_t kPrologue[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};  // mov [rsp+8],rbx

using BeginFrameFn = void (*)(void* cmdList);
using ThreadFenceFn = void** (*)(void* cmdList, void** outEvent, bool setLockFence);
using RecycleFn = void (*)(void* event);

struct State {
    uint8_t* beginFrame = nullptr;
    uint8_t* threadFence = nullptr;
    uint8_t* recycle = nullptr;
    uint8_t* commandVtable = nullptr;
    uint8_t* rhiThreadFlag = nullptr;
    uint32_t refCount = 0;
    uint8_t completeShift = 0;
    bool found = false, installed = false;
    BeginFrameFn original = nullptr;  // the trampoline: the replaced instruction, then the rest of BeginFrame
    uint8_t* region = nullptr;
};
State g;

// The hand-over of the first fence after switching on is under g_lock (render thread and monitor thread), so an event
// from before an off/on can never count: "ready" means every BeginFrame since the kept one is fenced.
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<bool> g_on{false};           // fences wanted (set by the translate switch)
void* g_first = nullptr;                 // the first fence's event since switching on, held until it has run (g_lock)
std::atomic<bool> g_needFirst{false};
std::atomic<bool> g_ready{false};        // the first fence has run: every BeginFrame since is fenced
std::atomic<uint64_t> g_fences{0}, g_noFence{0};

void ReleaseEvent(void* ev)
{
    if (ev && InterlockedDecrement(reinterpret_cast<volatile LONG*>(static_cast<uint8_t*>(ev) + g.refCount)) == 0)
        reinterpret_cast<RecycleFn>(g.recycle)(ev);
}

bool EventComplete(void* ev)
{
    const uint64_t head = static_cast<uint64_t>(InterlockedCompareExchange64(static_cast<volatile LONG64*>(ev), 0, 0));
    return ((head >> g.completeShift) & 1) != 0;
}

// Render thread, in place of FRHICommandListImmediate::BeginFrame.
void BeginFrameHook(void* cmdList)
{
    g.original(cmdList);
    if (!g_on.load(std::memory_order_acquire)) return;
    void* ev = nullptr;
    reinterpret_cast<ThreadFenceFn>(g.threadFence)(cmdList, &ev, true);
    if (!ev) {  // no separate RHI thread: nothing to order (and the guard keeps defragmentation paused)
        g_noFence.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_fences.fetch_add(1, std::memory_order_relaxed);
    if (g_needFirst.load(std::memory_order_acquire)) {
        AcquireSRWLockExclusive(&g_lock);
        const bool keep = g_needFirst.load() && g_on.load() && !g_first;
        if (keep) {
            g_first = ev;  // keep this reference until it has run
            g_needFirst = false;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (keep) return;
    }
    ReleaseEvent(ev);
}

bool Fail(const char* what)
{
    Log("[rdg] frame fence: %s: unavailable (buffer defragmentation stays paused while parallel translate is on)", what);
    return false;
}

uint8_t* FindIn(uint8_t* from, size_t len, const char* pattern)
{
    const auto hits = FindAll(Section{from, from + len}, ParsePattern(pattern));
    return hits.size() == 1 ? hits[0] : nullptr;
}

}  // namespace

bool FindFrameFence()
{
    if (g.found) return true;
    const GameModule& game = Game();
    auto inText = [&](const uint8_t* p) { return game.text.Contains(p); };

    uint8_t* beginFrame = nullptr;
    for (uint8_t* at : FindAll(game.text, ParsePattern(kRenderThreadCall))) {
        uint8_t* const target = RipTarget(at + kRenderThreadCallBeginFrame, 1, 5);
        if (beginFrame && target != beginFrame) return Fail("BeginFrameRenderThread found more than once");
        beginFrame = target;
    }
    if (!beginFrame || !inText(beginFrame) || !MatchesAt(beginFrame, ParsePattern(kBeginFrame)))
        return Fail("FRHICommandListImmediate::BeginFrame not recognised");
    uint8_t* const vtStore = FindIn(beginFrame, 0x80, kCommandVtableStore);
    uint8_t* const vtable = vtStore ? RipTarget(vtStore, 3, 7) : nullptr;
    uint8_t* const executor = vtable && game.rdata.Contains(vtable) ? *reinterpret_cast<uint8_t**>(vtable) : nullptr;
    if (!executor || !inText(executor) || !MatchesAt(executor, ParsePattern(kExecutor)))
        return Fail("the BeginFrame command is not the expected one");
    uint8_t* const rhiCheck = FindIn(vtStore, 0x20, kRhiThreadCheck);

    const auto ends = FindAll(game.text, ParsePattern(kEndFrame));
    if (ends.size() != 1) return Fail("FRHICommandListImmediate::EndFrame not found exactly once");
    uint8_t* const endFrame = ends[0];
    uint8_t* const endStore = FindIn(endFrame, 0xC0, kEndCommandVtableStore);
    if (!endStore || RipTarget(endStore, 3, 7) != vtable + 0x10) return Fail("the EndFrame command does not follow BeginFrame's");
    uint8_t* const threadFence = RipTarget(endFrame + kEndFrameFenceCall, 1, 5);
    uint8_t* const recycle = RipTarget(endFrame + kEndFrameRecycleCall, 1, 5);
    const uint32_t refCount = endFrame[kEndFrameRefCount];
    if (!inText(threadFence) || !inText(recycle) || refCount == 0 || refCount > 0x100)
        return Fail("RHIThreadFence or the event release not as expected");

    uint8_t shift = 0;
    size_t sites = 0;
    for (uint8_t* at : FindAll(game.text, ParsePattern(kIsComplete))) {
        if (sites && at[15] != shift) return Fail("FGraphEvent::IsComplete copies disagree");
        shift = at[15];
        ++sites;
    }
    if (sites < 2 || shift >= 64) return Fail("FGraphEvent::IsComplete not found");

    g.beginFrame = beginFrame;
    g.threadFence = threadFence;
    g.recycle = recycle;
    g.commandVtable = vtable;
    g.rhiThreadFlag = rhiCheck ? RipTarget(rhiCheck, 2, 6) : nullptr;
    g.refCount = refCount;
    g.completeShift = shift;
    g.found = true;
    Log("[rdg] frame fence: BeginFrame exe+0x%X (command vtable exe+0x%X), RHIThreadFence exe+0x%X, event release +0x%X -> "
        "exe+0x%X, complete bit %u (%zu copies)",
        static_cast<unsigned>(beginFrame - game.base), static_cast<unsigned>(vtable - game.base),
        static_cast<unsigned>(threadFence - game.base), refCount, static_cast<unsigned>(recycle - game.base), shift, sites);
    return true;
}

bool InstallFrameFence(bool beforeGameCode)
{
    if (g.installed) return true;
    if (!g.found && !FindFrameFence()) return false;
    if (memcmp(g.beginFrame, kPrologue, sizeof kPrologue) != 0) return Fail("BeginFrame's first instruction changed");
    if (!g.region) g.region = AllocNearGame(0x10000);
    if (!g.region) return Fail("no free memory near the game's code");
    // region: [0] trampoline: the replaced instruction, jmp [rip+0] -> BeginFrame + 5; [32] jmp [rip+0] -> hook
    uint8_t* const trampoline = g.region;
    uint8_t* const stub = g.region + 32;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = g.beginFrame + sizeof kPrologue;
    void* const hook = reinterpret_cast<void*>(&BeginFrameHook);
    memcpy(trampoline, kPrologue, sizeof kPrologue);
    memcpy(trampoline + sizeof kPrologue, jmpAbs, sizeof jmpAbs);
    memcpy(trampoline + sizeof kPrologue + sizeof jmpAbs, &resume, sizeof resume);
    memcpy(stub, jmpAbs, sizeof jmpAbs);
    memcpy(stub + sizeof jmpAbs, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(g.region, 0x10000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), g.region, 64);
    g.original = reinterpret_cast<BeginFrameFn>(trampoline);

    const int64_t rel = stub - (g.beginFrame + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("the hook is out of reach");
    uint8_t jmp[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(jmp + 1, &rel32, sizeof rel32);
    // The 5 bytes are one instruction inside one aligned qword: a thread is either before it or past it, so the
    // switch is a single atomic write even while the render thread runs.
    const uintptr_t at = reinterpret_cast<uintptr_t>(g.beginFrame);
    bool ok = false;
    if ((at & 7) <= 3) {
        auto* q = reinterpret_cast<volatile LONG64*>(at & ~uintptr_t{7});
        if (VirtualProtect(const_cast<LONG64*>(q), 8, PAGE_EXECUTE_READWRITE, &old)) {
            const LONG64 before = *q;
            LONG64 after = before;
            memcpy(reinterpret_cast<uint8_t*>(&after) + (at & 7), jmp, sizeof jmp);
            ok = InterlockedCompareExchange64(q, after, before) == before;
            VirtualProtect(const_cast<LONG64*>(q), 8, old, &old);
            FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(q), 8);
        }
    } else if (beforeGameCode) {
        ok = WriteCode(g.beginFrame, jmp);
    }
    if (!ok) return Fail("could not place the detour");
    g.installed = true;
    Log("[rdg] frame fence installed (BeginFrame detoured)");
    return true;
}

bool FrameFenceAvailable() { return g.installed; }

// Monitor thread. On: fences start with the next BeginFrame; ready once the first of them has run on the RHI thread.
void SetFrameFence(bool on)
{
    if (!g.installed) return;
    void* release = nullptr;
    const char* message = nullptr;
    AcquireSRWLockExclusive(&g_lock);
    if (on && !g_on.load()) {
        release = g_first;
        g_first = nullptr;
        g_ready = false;
        g_needFirst = true;
        g_on = true;
        message = "[rdg] frame fence on: an RHI thread fence after every BeginFrame";
    } else if (on && !g_ready.load() && g_first && EventComplete(g_first)) {
        release = g_first;
        g_first = nullptr;
        g_ready = true;
        message = "[rdg] frame fence in effect: every frame's start now precedes its parallel translates";
    } else if (!on && g_on.load()) {
        g_on = false;
        g_needFirst = false;
        g_ready = false;
        release = g_first;
        g_first = nullptr;
        message = "[rdg] frame fence off";
    }
    ReleaseSRWLockExclusive(&g_lock);
    ReleaseEvent(release);
    if (message) Log("%s", message);
}

bool FrameFenceReady() { return g.installed && g_on.load() && g_ready.load(); }

void LogFrameFenceStats()
{
    static uint64_t lastFences = 0, lastNone = 0;
    const uint64_t fences = g_fences.load(), none = g_noFence.load();
    if (g.installed && (fences != lastFences || none != lastNone))
        Log("[rdg] frame fence since last report: %llu fences after BeginFrame%s", fences - lastFences,
            none != lastNone ? " (some without an RHI thread: no fence)" : "");
    lastFences = fences;
    lastNone = none;
}

bool FrameFenceInfo(FrameFenceInfoOut* out)
{
    if (!g.found) return false;
    out->beginFrame = g.beginFrame;
    out->threadFence = g.threadFence;
    out->recycle = g.recycle;
    out->commandVtable = g.commandVtable;
    out->rhiThreadFlag = g.rhiThreadFlag;
    out->refCount = g.refCount;
    out->completeShift = g.completeShift;
    return true;
}

void FrameFenceTestFakes(void* fakeThreadFence, void* fakeRecycle)
{
    g.threadFence = static_cast<uint8_t*>(fakeThreadFence);
    g.recycle = static_cast<uint8_t*>(fakeRecycle);
}

}  // namespace opf
