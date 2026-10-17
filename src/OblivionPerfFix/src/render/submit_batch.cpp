// RdgSubmitBatch: fewer GPU submissions when RDG pass sets are translated in parallel (RdgParallelTranslate).
//
// With parallel translate every RDG parallel pass set (~55 per frame at the Cheydinhal views)
// costs two separate submissions: FRHICommandListImmediate::QueueAsyncCommandListSubmit first
// finalizes and submits the immediate (default) context's work so far (ExecuteAndReset), then the
// RHI thread's "SubmitParallelCommandLists" lambda submits the set's own command list. Both go
// through FD3D12DynamicRHI::RHISubmitCommandLists -> SubmitCommands -> SubmitPayloads, and the
// submission thread turns every payload into its own ExecuteCommandLists call plus a fence signal.
// That is ~110 extra ExecuteCommandLists/Signal pairs per frame instead of a handful, and the GPU
// drains its pipeline at each one: +1.6-3 ms of GPU time per frame (+2.35 ms at 4K).
//
// The D3D12 RHI can already merge payloads: SubmitCommands puts all command lists of one call into
// a single payload (one ExecuteCommandLists) when none of them waits, signals a fence, carries
// timing or queries, and all are for the direct queue (r.D3D12.AllowPayloadMerge, default 1). It
// just never gets more than one at a time here. So we hold back the command lists passed to
// RHISubmitCommandLists (vtable slot) for a short while and hand them over together:
//  - a batch is submitted when it holds RdgSubmitBatch command lists, when its oldest list has
//    waited RdgSubmitMaxAgeUs (1 ms; a helper thread checks), when the RHI asks to flush resources (end of
//    frame), and before anything is submitted any other way (a detour on SubmitPayloads: Present,
//    end of frame, GPU waits/readbacks, deferred deletion fences), so the GPU always sees the
//    work in the order it was recorded and nothing can wait on a list that is still held back;
//  - command lists that could not be merged (same test as the engine's, with the field offsets
//    read from the engine's own code) are never held: the batch is submitted, then the list.
// Only active while parallel translate is on (the monitor thread couples the two).
#include "common.h"

#include <atomic>
#include <cstring>
#include <vector>

namespace opf {
namespace {

struct View {  // TArrayView<T*>: data pointer, int32 count
    void** data;
    int32_t num;
};
using SubmitListsFn = void (*)(void* rhi, const View* lists, bool flushResources);
using SubmitPayloadsFn = void (*)(void* rhi, const View* payloads);

// FD3D12DynamicRHI::RHISubmitCommandLists (exe+0x254EDF0):
//     SubmitCommands(CommandLists); if (bFlushResources) ProcessDeferredDeletionQueue;
constexpr char kSubmitListsPattern[] =
    "40 53 55 48 81 EC ?? ?? ?? ?? 48 8B 02 41 0F B6 D8 48 89 44 24 ?? 48 8B E9 8B 42 08 48 8D 54 24 ?? "
    "89 44 24 ?? 0F 28 44 24 ?? 66 0F 7F 44 24 ?? E8 ?? ?? ?? ?? 84 DB 0F 84";
constexpr size_t kSubmitListsCall = 48;
// FD3D12DynamicRHI::SubmitCommands (exe+0x2551A40) starts with
//     if (Commands.Num > 1 && GD3D12AsyncPayloadMerge)
constexpr char kMergeGatePattern[] = "41 83 FB 01 0F 8E ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 0F 84";
constexpr size_t kMergeGateSearch = 0x40;
// ... followed by the per-payload test: for (Payload : *CmdList) { Payload->Queue.QueueType == Direct && ... }
constexpr char kMergeLoopPattern[] = "49 8B 01 48 8B 10 48 63 40 08 4C 8D 04 C2 49 3B D0 74 ?? 48 8B 0A 48 8B 41 ??";
constexpr size_t kMergeLoopSearch = 0x100;
constexpr size_t kMergeLoopQueueDisp = 25;  // mov rax,[rcx+disp8]: Payload->Queue
constexpr size_t kMergeLoopChecks = 26;     // the field compares start here
constexpr int kMaxChecks = 16;
// FD3D12DynamicRHI::SubmitPayloads (exe+0x25520F0): the 13 position-independent prologue bytes we
// move to the trampoline (push rbx; push rsi; sub rsp,28h; mov rbx,[rdx]; mov rsi,rcx).
constexpr char kSubmitPayloadsPrologue[] = "40 53 56 48 83 EC 28 48 8B 1A 48 8B F1";
constexpr size_t kPrologueSize = 13;
constexpr size_t kSubmitCommandsScan = 0x800;
// The RHI thread's SubmitParallelCommandLists lambda (exe+0x265232A):
//     GDynamicRHI->RHISubmitCommandLists(AllCmdLists, false)   (gives GDynamicRHI and the slot offset)
constexpr char kDynamicRhiCallPattern[] =
    "45 33 C0 48 8B 0D ?? ?? ?? ?? 48 89 45 ?? 0F 28 45 ?? 66 0F 7F 45 ?? 48 8B 01 FF 90 ?? ?? ?? ??";
constexpr size_t kDynamicRhiMov = 3, kDynamicRhiSlot = 28;

struct FieldCheck {
    bool onQueue;
    uint8_t width;
    uint32_t offset;
};

struct State {
    // found in the exe
    uint8_t* submitLists = nullptr;
    uint8_t* submitCommands = nullptr;
    uint8_t* submitPayloads = nullptr;
    void** vtableSlot = nullptr;
    void** gDynamicRhi = nullptr;
    void** rhiVtable = nullptr;  // FD3D12DynamicRHI vtable: vtableSlot minus the call's slot offset
    uint32_t queueOffset = 0;
    FieldCheck checks[kMaxChecks] = {};
    int numChecks = 0;
    // runtime
    SubmitListsFn submit = nullptr;       // original RHISubmitCommandLists (a fake in the self-test)
    SubmitPayloadsFn payloads = nullptr;  // trampoline to the original SubmitPayloads (a fake in the self-test)
    CRITICAL_SECTION cs;
    std::vector<void*> pending;  // finalized command lists held back, in submission order
    std::atomic<int> pendingCount{0};
    void* rhi = nullptr;
    LONGLONG since = 0;  // QPC time the oldest pending list arrived
    LONGLONG qpcPerSecond = 1;
    std::atomic<LONGLONG> maxAge{0};  // QPC ticks a list may be held back
    std::atomic<int> maxAgeUs{0};
    std::atomic<int> maxLists{0};  // 0 = pass everything through
    HANDLE wake = nullptr;
    bool installed = false;
} g;

thread_local int t_inSubmit = 0;  // > 0 while this thread runs a submit call we made

struct Stats {
    std::atomic<uint64_t> lists{0}, batches{0}, batchedLists{0}, alone{0}, passed{0}, payloadCalls{0};
    std::atomic<uint64_t> byCount{0}, byAge{0}, byDirect{0}, byFlush{0}, byUnmergeable{0}, byOff{0};
} g_stats;

uint32_t ReadU32(const uint8_t* p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }

// Decodes the engine's merge test: a run of "cmp <size> [rcx|rax + disp], 0 ; jne <same label>".
bool ParseMergeChecks(const uint8_t* q)
{
    const uint8_t* label = nullptr;
    int n = 0;
    for (;;) {
        FieldCheck c{};
        size_t len = 0;
        if (q[0] == 0x83 && (q[1] == 0x78 || q[1] == 0x79) && q[3] == 0) { c = {q[1] == 0x78, 4, q[2]}; len = 4; }
        else if (q[0] == 0x80 && q[1] == 0x79 && q[3] == 0) { c = {false, 1, q[2]}; len = 4; }
        else if (q[0] == 0x48 && q[1] == 0x83 && q[2] == 0x79 && q[4] == 0) { c = {false, 8, q[3]}; len = 5; }
        else if (q[0] == 0x83 && q[1] == 0xB9 && q[6] == 0) { c = {false, 4, ReadU32(q + 2)}; len = 7; }
        else if (q[0] == 0x80 && q[1] == 0xB9 && q[6] == 0) { c = {false, 1, ReadU32(q + 2)}; len = 7; }
        else if (q[0] == 0x48 && q[1] == 0x83 && q[2] == 0xB9 && q[7] == 0) { c = {false, 8, ReadU32(q + 3)}; len = 8; }
        else break;
        q += len;
        if (q[0] != 0x75) return false;
        const uint8_t* target = q + 2 + static_cast<int8_t>(q[1]);
        if (label && target != label) return false;
        label = target;
        q += 2;
        if (n == kMaxChecks || c.offset > 0x1000) return false;
        g.checks[n++] = c;
    }
    int onQueue = 0;
    for (int i = 0; i < n; ++i) onQueue += g.checks[i].onQueue;
    if (n < 8 || onQueue != 1) return false;
    g.numChecks = n;
    return true;
}

// The engine's own test (D3D12Submission.cpp, SubmitCommands): direct queue, no waits, no fence
// signals, no timing, no completion fence, no submission event or time, no queries.
bool Mergeable(const void* cmdList)
{
    auto* fc = static_cast<const uint8_t*>(cmdList);  // FD3D12FinalizedCommands: TArray<FD3D12Payload*> at +0
    auto* const* payloads = *reinterpret_cast<uint8_t* const* const*>(fc);
    const int32_t num = *reinterpret_cast<const int32_t*>(fc + 8);
    if (!payloads || num <= 0 || num > 256) return false;
    for (int32_t i = 0; i < num; ++i) {
        const uint8_t* p = payloads[i];
        if (!p) return false;
        const uint8_t* queue = *reinterpret_cast<const uint8_t* const*>(p + g.queueOffset);
        if (!queue) return false;
        for (int c = 0; c < g.numChecks; ++c) {
            const uint8_t* at = (g.checks[c].onQueue ? queue : p) + g.checks[c].offset;
            uint64_t v = 0;
            memcpy(&v, at, g.checks[c].width);
            if (v) return false;
        }
    }
    return true;
}

void CallSubmit(const View* lists, bool flushResources)
{
    ++t_inSubmit;
    g.submit(g.rhi, lists, flushResources);
    --t_inSubmit;
}

// Caller holds g.cs.
void FlushLocked(bool flushResources, std::atomic<uint64_t>& reason)
{
    if (g.pending.empty() && !flushResources) return;
    View v{g.pending.empty() ? nullptr : g.pending.data(), static_cast<int32_t>(g.pending.size())};
    if (v.num) {
        g_stats.batches.fetch_add(1, std::memory_order_relaxed);
        g_stats.batchedLists.fetch_add(v.num, std::memory_order_relaxed);
        reason.fetch_add(1, std::memory_order_relaxed);
    }
    CallSubmit(&v, flushResources);
    g.pending.clear();
    g.pendingCount.store(0);
}

void SubmitListsHook(void* rhi, const View* lists, bool flushResources)
{
    if (t_inSubmit || (g.maxLists.load(std::memory_order_relaxed) == 0 && g.pendingCount.load() == 0)) {
        if (!t_inSubmit) g_stats.passed.fetch_add(lists->num > 0 ? 1u : 0u, std::memory_order_relaxed);
        g.submit(rhi, lists, flushResources);
        return;
    }
    EnterCriticalSection(&g.cs);
    g.rhi = rhi;
    const int maxLists = g.maxLists.load();
    if (maxLists == 0) {
        FlushLocked(false, g_stats.byOff);
        g_stats.passed.fetch_add(lists->num > 0 ? 1u : 0u, std::memory_order_relaxed);
        CallSubmit(lists, flushResources);
        LeaveCriticalSection(&g.cs);
        return;
    }
    for (int32_t i = 0; i < lists->num; ++i) {
        void* item = lists->data[i];
        if (!item) continue;
        g_stats.lists.fetch_add(1, std::memory_order_relaxed);
        if (!Mergeable(item)) {
            FlushLocked(false, g_stats.byUnmergeable);
            View one{&item, 1};
            CallSubmit(&one, false);
            g_stats.alone.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (g.pending.empty()) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            g.since = now.QuadPart;
            SetEvent(g.wake);
        }
        g.pending.push_back(item);
        g.pendingCount.store(static_cast<int>(g.pending.size()));
        if (static_cast<int>(g.pending.size()) >= maxLists) FlushLocked(false, g_stats.byCount);
    }
    if (flushResources) FlushLocked(true, g_stats.byFlush);
    LeaveCriticalSection(&g.cs);
}

// Everything that reaches the GPU without RHISubmitCommandLists goes through here: submit the
// held-back lists first so the GPU executes work in the order it was recorded.
void SubmitPayloadsHook(void* rhi, const View* payloads)
{
    if (!t_inSubmit) {
        g_stats.payloadCalls.fetch_add(1, std::memory_order_relaxed);
        if (g.pendingCount.load() > 0) {
            EnterCriticalSection(&g.cs);
            FlushLocked(false, g_stats.byDirect);
            LeaveCriticalSection(&g.cs);
        }
    }
    g.payloads(rhi, payloads);
}

// Submits a batch whose oldest list has waited the maximum age, so nothing is held back for long
// even when no further submission comes (e.g. a thread blocking on a GPU result in a held-back list).
DWORD WINAPI AgeThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);  // it only ever runs for microseconds
    if (auto setName = reinterpret_cast<HRESULT(WINAPI*)(HANDLE, PCWSTR)>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
        setName(GetCurrentThread(), L"OblivionPerfFix submit");
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
        WaitForSingleObject(g.wake, INFINITE);
        for (;;) {
            EnterCriticalSection(&g.cs);
            LONGLONG wait = -1;
            if (!g.pending.empty()) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                const LONGLONG age = now.QuadPart - g.since, maxAge = g.maxAge.load();
                if (age >= maxAge) FlushLocked(false, g_stats.byAge);
                else wait = maxAge - age;
            }
            LeaveCriticalSection(&g.cs);
            if (wait < 0) break;  // nothing left: sleep until the next list is held back
            const LONGLONG us = static_cast<LONGLONG>(QpcToMicroseconds(static_cast<uint64_t>(wait), static_cast<uint64_t>(g.qpcPerSecond))) + 20;
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -us * 10;  // relative, 100 ns units
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
                else Sleep(1);
            } else {
                Sleep(1);
            }
        }
    }
}

bool Find(const GameModule& game)
{
    const auto lists = FindAll(game.text, ParsePattern(kSubmitListsPattern));
    if (lists.size() != 1) {
        Log("[rdg] submit batching: RHISubmitCommandLists found %zu times (expected 1): unavailable", lists.size());
        return false;
    }
    g.submitLists = lists[0];
    g.submitCommands = RipTarget(g.submitLists + kSubmitListsCall, 1, 5);
    if (!game.text.Contains(g.submitCommands)) return false;

    const Pattern gate = ParsePattern(kMergeGatePattern);
    bool gated = false;
    for (const uint8_t* p = g.submitCommands; p < g.submitCommands + kMergeGateSearch && !gated; ++p) gated = MatchesAt(p, gate);
    const Pattern loop = ParsePattern(kMergeLoopPattern);
    const uint8_t* loopAt = nullptr;
    for (const uint8_t* p = g.submitCommands; p < g.submitCommands + kMergeLoopSearch && !loopAt; ++p)
        if (MatchesAt(p, loop)) loopAt = p;
    if (!gated || !loopAt || !ParseMergeChecks(loopAt + kMergeLoopChecks)) {
        Log("[rdg] submit batching: SubmitCommands at exe+0x%X does not have the expected payload merge: unavailable",
            static_cast<unsigned>(g.submitCommands - game.base));
        return false;
    }
    g.queueOffset = loopAt[kMergeLoopQueueDisp];

    // SubmitPayloads: the function SubmitCommands calls (at least twice) with the expected prologue.
    const Pattern prologue = ParsePattern(kSubmitPayloadsPrologue);
    uint8_t* candidates[16] = {};
    int counts[16] = {}, numCandidates = 0;
    for (uint8_t* p = g.submitCommands; p < g.submitCommands + kSubmitCommandsScan; ++p) {
        if (*p != 0xE8) continue;
        uint8_t* t = RipTarget(p, 1, 5);
        if (!game.text.Contains(t) || !MatchesAt(t, prologue)) continue;
        int i = 0;
        while (i < numCandidates && candidates[i] != t) ++i;
        if (i == numCandidates) {
            if (numCandidates == 16) break;
            candidates[numCandidates++] = t;
        }
        ++counts[i];
    }
    g.submitPayloads = nullptr;
    for (int i = 0; i < numCandidates; ++i)
        if (counts[i] >= 2) {
            if (g.submitPayloads) { g.submitPayloads = nullptr; break; }  // ambiguous
            g.submitPayloads = candidates[i];
        }
    if (!g.submitPayloads) {
        Log("[rdg] submit batching: SubmitPayloads not found: unavailable");
        return false;
    }

    // The vtable slot holding RHISubmitCommandLists.
    int slots = 0;
    for (auto** slot = reinterpret_cast<void**>(game.rdata.begin); slot + 1 <= reinterpret_cast<void**>(game.rdata.end); ++slot)
        if (*slot == g.submitLists) { g.vtableSlot = slot; ++slots; }
    if (slots != 1) {
        Log("[rdg] submit batching: RHISubmitCommandLists is in %d vtable slots (expected 1): unavailable", slots);
        return false;
    }

    // GDynamicRHI and the slot offset, from the RHI thread's call
    // GDynamicRHI->RHISubmitCommandLists(AllCmdLists, false). Optional (used for checks only): every
    // match must name the same variable and slot.
    g.gDynamicRhi = nullptr;
    g.rhiVtable = nullptr;
    bool consistent = true;
    for (const uint8_t* hit : FindAll(game.text, ParsePattern(kDynamicRhiCallPattern))) {
        const uint32_t slotOffset = ReadU32(hit + kDynamicRhiSlot);
        auto** variable = reinterpret_cast<void**>(RipTarget(hit + kDynamicRhiMov, 3, 7));
        auto** vtable = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g.vtableSlot) - slotOffset);
        if (slotOffset % 8 || slotOffset > 0x2000 || !game.rdata.Contains(vtable)) { consistent = false; continue; }
        if (g.gDynamicRhi && (g.gDynamicRhi != variable || g.rhiVtable != vtable)) consistent = false;
        g.gDynamicRhi = variable;
        g.rhiVtable = vtable;
    }
    if (!consistent) {
        g.gDynamicRhi = nullptr;
        g.rhiVtable = nullptr;
    }
    return true;
}

}  // namespace

bool InstallRdgSubmitBatching(bool rhiMayBeRunning)
{
    if (g.installed) return true;
    const GameModule& game = Game();
    if (!Find(game)) return false;
    if (g.gDynamicRhi ? *g.gDynamicRhi != nullptr : rhiMayBeRunning) {
        Log("[rdg] submit batching: the renderer may already be running, so SubmitPayloads cannot be detoured safely: not installed");
        return false;
    }

    // Trampoline: the original prologue, then jmp [rip+0] back to the rest of SubmitPayloads.
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, g.submitPayloads, kPrologueSize);
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(tramp + kPrologueSize, jmpAbs, sizeof jmpAbs);
    const uint64_t back = reinterpret_cast<uint64_t>(g.submitPayloads + kPrologueSize);
    memcpy(tramp + kPrologueSize + sizeof jmpAbs, &back, sizeof back);
    DWORD old;
    VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);

    InitializeCriticalSection(&g.cs);
    g.pending.reserve(256);
    g.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g.qpcPerSecond = freq.QuadPart;
    g.submit = reinterpret_cast<SubmitListsFn>(g.submitLists);
    g.payloads = reinterpret_cast<SubmitPayloadsFn>(tramp);
    if (!g.wake) return false;
    if (HANDLE t = CreateThread(nullptr, 0, AgeThread, nullptr, 0, nullptr)) CloseHandle(t);
    else return false;

    // Detour: mov rax, SubmitPayloadsHook ; jmp rax ; nop   (13 bytes, replaces the prologue)
    uint8_t detour[kPrologueSize] = {0x48, 0xB8};
    const uint64_t hook = reinterpret_cast<uint64_t>(&SubmitPayloadsHook);
    memcpy(detour + 2, &hook, sizeof hook);
    detour[10] = 0xFF;
    detour[11] = 0xE0;
    detour[12] = 0x90;
    if (!WriteCode(g.submitPayloads, detour)) return false;
    if (!WritePointer(g.vtableSlot, reinterpret_cast<void*>(&SubmitListsHook))) {
        WriteCode(g.submitPayloads, std::span<const uint8_t>(tramp, kPrologueSize));  // undo
        return false;
    }
    g.installed = true;
    Log("[rdg] submit batching installed: RHISubmitCommandLists exe+0x%X (vtable slot exe+0x%X), SubmitCommands exe+0x%X, "
        "SubmitPayloads exe+0x%X, %d merge checks, Queue at +0x%X",
        static_cast<unsigned>(g.submitLists - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(g.vtableSlot) - game.base),
        static_cast<unsigned>(g.submitCommands - game.base), static_cast<unsigned>(g.submitPayloads - game.base), g.numChecks, g.queueOffset);
    return true;
}

void SetRdgSubmitBatch(int maxLists, int maxAgeUs)
{
    if (!g.installed) return;
    if (maxLists < 0) maxLists = 0;
    if (maxLists > 256) maxLists = 256;
    if (maxAgeUs < 50) maxAgeUs = 50;
    if (maxAgeUs > 20000) maxAgeUs = 20000;
    const int previousAge = g.maxAgeUs.exchange(maxAgeUs);
    g.maxAge.store(g.qpcPerSecond * maxAgeUs / 1000000);
    const int previous = g.maxLists.exchange(maxLists);
    if (previous == maxLists && (previousAge == maxAgeUs || maxLists == 0)) return;
    if (maxLists == 0) {
        EnterCriticalSection(&g.cs);
        FlushLocked(false, g_stats.byOff);
        LeaveCriticalSection(&g.cs);
        if (previous != 0) Log("[rdg] submit batching off (one submission per command list, original behaviour)");
    } else {
        Log("[rdg] submit batching on: up to %d command lists per submission, held back at most %d us", maxLists, maxAgeUs);
    }
}

void CheckRdgSubmitBatching()
{
    // Once the RHI exists, confirm it uses the vtable we patched.
    static bool checked = false;
    if (!g.installed || checked || !g.gDynamicRhi) return;
    void* rhi = *g.gDynamicRhi;
    if (!rhi) return;
    checked = true;
    if (*static_cast<void***>(rhi) == g.rhiVtable) Log("[rdg] submit batching: the D3D12 RHI uses the patched vtable");
    else Log("[rdg] submit batching: warning: the RHI object does not use the patched vtable (batching has no effect)");
}

// Submission rates since the last report. "Submissions" are calls that hand work to the GPU queue:
// RHISubmitCommandLists calls that went straight through, batches, lists submitted alone, and
// SubmitPayloads calls from elsewhere in the RHI (Present, end of frame, GPU waits).
void LogRdgSubmitStats()
{
    static uint64_t last[11] = {};
    static ULONGLONG lastTick = 0;
    if (!g.installed) return;
    const uint64_t now[11] = {g_stats.lists.load(), g_stats.batches.load(), g_stats.batchedLists.load(), g_stats.alone.load(),
                              g_stats.passed.load(), g_stats.payloadCalls.load(), g_stats.byCount.load(), g_stats.byAge.load(),
                              g_stats.byDirect.load(), g_stats.byFlush.load(), g_stats.byUnmergeable.load()};
    const ULONGLONG tick = GetTickCount64();
    const double seconds = lastTick ? (tick - lastTick) / 1000.0 : 0.0;
    double d[11];
    bool any = false;
    for (int i = 0; i < 11; ++i) {
        d[i] = seconds > 0 ? static_cast<double>(now[i] - last[i]) / seconds : 0.0;
        any = any || now[i] != last[i];
        last[i] = now[i];
    }
    lastTick = tick;
    if (!any || seconds <= 0) return;
    Log("[rdg] submissions per second over %.0f s: %.0f (batching %s): %.0f batches of %.1f command lists, %.0f lists alone, "
        "%.0f straight through, %.0f from elsewhere in the RHI; batches closed by size %.0f, age %.0f, other submission %.0f, "
        "flush %.0f, unmergeable list %.0f; descriptor heap guard hits so far %llu",
        seconds, d[1] + d[3] + d[4] + d[5], g.maxLists.load() ? "on" : "off", d[1], d[1] > 0 ? d[2] / d[1] : 0.0, d[3], d[4], d[5],
        d[6], d[7], d[8], d[9], d[10], DescriptorHeapGuardHits());
}

bool RdgSubmitBatchingInfo(RdgSubmitInfo* out)
{
    if (!g.submitLists) return false;
    out->submitLists = g.submitLists;
    out->submitCommands = g.submitCommands;
    out->submitPayloads = g.submitPayloads;
    out->vtableSlot = g.vtableSlot;
    out->rhiVtable = g.rhiVtable;
    out->gDynamicRhi = g.gDynamicRhi;
    out->queueOffset = g.queueOffset;
    out->numChecks = g.numChecks;
    for (int i = 0; i < g.numChecks && i < 16; ++i) {
        out->checkOffset[i] = g.checks[i].offset;
        out->checkWidth[i] = g.checks[i].width;
        out->checkOnQueue[i] = g.checks[i].onQueue;
    }
    return true;
}

void RdgSubmitBatchingTestHooks(void* fakeSubmitLists, void* fakeSubmitPayloads, void** hookLists, void** hookPayloads)
{
    g.submit = reinterpret_cast<SubmitListsFn>(fakeSubmitLists);
    g.payloads = reinterpret_cast<SubmitPayloadsFn>(fakeSubmitPayloads);
    *hookLists = reinterpret_cast<void*>(&SubmitListsHook);
    *hookPayloads = reinterpret_cast<void*>(&SubmitPayloadsHook);
}

}  // namespace opf
