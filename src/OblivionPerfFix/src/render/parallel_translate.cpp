// RdgParallelTranslate: in UE 5.3, the render graph (RDG) records its passes on worker threads in "parallel pass
// sets", but only sets flagged ERDGPassFlags::ParallelTranslate (in practice only Nanite's raster
// passes) are also *translated* into D3D12 command lists on workers. Every other set is replayed
// serially on the RHI thread (the "ExecuteCommandLists" lambda in
// FRHICommandListImmediate::QueueAsyncCommandListSubmit). At the Cheydinhal test spots that is
// ~55% of the RHI thread, which is the frame-rate limiter (~94% busy). UE 5.5 translates all such
// command lists in parallel by default (r.RHICmd.ParallelTranslate.Enable).
//
// FRDGBuilder::Execute picks the priority as
//     TranslatePriority = ParallelPassSet.bParallelTranslate ? Normal : Disabled;
// compiled as  cmp byte [r13+32h],0 ; ... ; mov r8d,r15d (r15d = 0) ; setne r8b.
// We replace setne r8b with mov r8b,1 so every RDG parallel pass set takes the parallel translate
// path that 5.3 already uses for the base pass and Nanite. Ray-tracing work (shader bindings,
// acceleration structure builds, ray dispatches) runs in NeverParallel passes on the immediate
// command list, so it stays on the RHI thread in order; this was verified on captures at three
// locations (docs/research.md, "Parallel command translation"). The patch is a single aligned 4-byte write, switched
// live from the ini.
//
// Cost: every parallel-translated set becomes its own D3D12 command-list submission (~55 extra per
// frame, plus one more each because the immediate command list is split around every set), which
// adds ~2.5 ms of GPU time per frame. CPU-bound (720p) that is +22% fps; GPU-bound (4K) it is -5%.
// submit_batch.cpp batches those submissions (RdgSubmitBatch), which removes that GPU cost.
// RHI thread load measured: CPU-bound 82-93% off / 53-59% on; GPU-bound 39-44% off / 21-22% on.
//
// Descriptor heap guard (an early build's start-up crash, "D3D12DescriptorCache.cpp:772 Unable to
// reserve slot"): a D3D12 command context gets its block of the shader-visible view heap
// (FD3D12SubAllocatedOnlineHeap::CurrentBlock) only when it opens its first command list, and
// command lists open lazily. The structured/raw-buffer UAV clear (FD3D12CommandContext::ClearUAV,
// RunOnContext lambda, exe+0x2553860) asks the view heap for a slot *before* anything opens the
// command list: CanReserveSlots(1) -> GetTotalSize is 0 without a block -> 1 > 0 -> fatal error.
// Pooled contexts already own a block, so only a brand-new context whose first command is such a
// clear crashes. With all RDG sets translated on workers the context pool grows (one context per
// concurrent translate job), most of all right after the fix is switched on in menus, where Nanite
// had not created any parallel contexts yet. We wrap the sub-allocated heap's GetTotalSize (vtable
// slot 5): if the heap has no block yet, call its own OpenCommandList (slot 3), which allocates the
// block exactly as opening the command list would have, then return the real size.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace opf {
namespace {

// The last four bytes are the instruction we switch (either form is accepted, so a site that is
// already patched is recognised).
constexpr char kRdgPattern[] =
    "41 80 7D ?? 00 48 8D 95 ?? ?? ?? ?? 48 8B 4B ?? B8 01 00 00 00 89 85 ?? ?? ?? ?? 45 8B C7 41 ?? ?? ??";
constexpr size_t kSiteOffset = 30;
constexpr uint8_t kOriginal[4] = {0x41, 0x0F, 0x95, 0xC0};  // setne r8b
constexpr uint8_t kForced[4] = {0x41, 0xB0, 0x01, 0x90};    // mov r8b,1 ; nop

uint8_t* g_site = nullptr;
std::atomic<int> g_state{-1};  // -1 unknown, 0 original, 1 forced
std::atomic<bool> g_allowed{true};  // false if the descriptor heap guard or the defrag guard is unavailable
bool DefragGuardRequired() { return true; }  // public build: the pause is always part of parallel translate
std::atomic<ULONGLONG> g_offSinceMs{0};
std::atomic<bool> g_useFrameFence{false};   // [Fixes] RdgFrameFence (hidden; used while ViewRenameGuard is on)

bool WriteSite(const uint8_t (&bytes)[4])
{
    DWORD old;
    if (!VirtualProtect(g_site, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint32_t value;
    memcpy(&value, bytes, 4);
    if ((reinterpret_cast<uintptr_t>(g_site) & 3) == 0)
        InterlockedExchange(reinterpret_cast<volatile LONG*>(g_site), static_cast<LONG>(value));  // atomic
    else
        memcpy(g_site, bytes, 4);
    VirtualProtect(g_site, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_site, 4);
    return true;
}

}  // namespace

bool FindRdgParallelTranslateSite()
{
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kRdgPattern));
    if (hits.size() != 1) {
        Log("[rdg] translate-priority site found %zu times (expected 1): fix unavailable", hits.size());
        return false;
    }
    uint8_t* site = hits[0] + kSiteOffset;
    if (memcmp(site, kOriginal, 4) != 0 && memcmp(site, kForced, 4) != 0) {
        Log("[rdg] unexpected instruction at exe+0x%X: fix unavailable", static_cast<unsigned>(site - game.base));
        return false;
    }
    g_site = site;
    g_state = memcmp(site, kForced, 4) == 0 ? 1 : 0;
    Log("[rdg] translate-priority site at exe+0x%X", static_cast<unsigned>(site - game.base));
    return true;
}

void SetRdgParallelTranslateEnabled(bool enabled)
{
    if (enabled && !g_allowed.load()) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            Log("[rdg] parallel translate stays off: a required guard is not installed (see above)");
        enabled = false;
    }
    if (!g_site) return;
    const ULONGLONG now = GetTickCount64();
    const bool translating = g_state.load() == 1;
    // translate_frame_fence.cpp: with an RHI thread fence after every BeginFrame, no pass set is translated while a
    // frame's defragmentation runs, so buffers may move again. Fenced while translating, and for a second afterwards.
    const bool fenceWanted = g_useFrameFence.load() && FrameFenceAvailable() &&
                             (enabled || translating || now - g_offSinceMs.load() < 1000);
    if (fenceWanted) SetFrameFence(true);
    const bool fenced = fenceWanted && FrameFenceReady();
    if (enabled && DefragGuardRequired() && !fenced) {
        // Buffers must have stopped moving before any pass set is translated on a worker (translate_defrag_guard.cpp).
        // A defragmentation pass that already started this frame finishes first: wait a moment before switching
        // on (the monitor thread calls this every second).
        SetPoolDefragPaused(true);
        if (!PoolDefragPausedFor(250)) return;
    } else if (enabled) {
        SetPoolDefragPaused(false);  // fenced, or lab A-B: parallel translate without the pause
    }
    if (g_state.load() != static_cast<int>(enabled)) {
        if (!WriteSite(enabled ? kForced : kOriginal)) {
            Log("[rdg] write failed");
            return;
        }
        g_state = enabled;
        if (!enabled) g_offSinceMs = now;
        Log("[rdg] parallel translate of all RDG pass sets %s", enabled ? "enabled" : "disabled (original behaviour)");
    }
    // Defragmentation comes back only when no pass set can still be translating on a worker: a second after
    // parallel translate went off (or straight away if the guard is not wanted).
    if (!enabled && (!DefragGuardRequired() || now - g_offSinceMs.load() >= 1000)) SetPoolDefragPaused(false);
    // The fence may only go while defragmentation cannot run alongside a translate: parallel translate off for a
    // second, or the pause in effect (every BeginFrame the RHI thread runs from then on reads it).
    if (!fenceWanted && (g_state.load() != 1 || PoolDefragPausedFor(250))) SetFrameFence(false);
}

void SetRdgUseFrameFence(bool use)
{
    if (g_useFrameFence.exchange(use) != use)
        Log("[rdg] frame fence %s by RdgFrameFence", use ? "allowed" : "not used (buffer defragmentation paused while translating)");
}


uint8_t* RdgParallelTranslateSite() { return g_site; }

bool RdgParallelTranslateActive() { return g_state.load() == 1; }

void SetRdgParallelTranslateAllowed(bool allowed)
{
    g_allowed = allowed;
    if (!allowed) SetRdgParallelTranslateEnabled(false);
}

// ---- descriptor heap guard ----
namespace {

// FD3D12SubAllocatedOnlineHeap virtuals (exe+0x7259348 in build 19115871). The disp8 is
// CurrentBlock; all three must agree on it.
//   slot 1 RollOver:        AllocateBlock; return CurrentBlock == nullptr;
//   slot 3 OpenCommandList: if (!CurrentBlock) AllocateBlock;  (tail jump)
//   slot 5 GetTotalSize:    return CurrentBlock ? CurrentBlock->Size : 0;
constexpr char kGetTotalSizePattern[] = "48 8B 41 ?? 48 85 C0 74 04 8B 40 ?? C3 C3";
constexpr char kOpenPattern[] = "48 83 79 ?? 00 0F 84 ?? ?? ?? ?? C3";
constexpr char kRollOverPattern[] = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 7B ?? 00 0F 94 C0 48 83 C4 20 5B C3";
constexpr int kSlotRollOver = 1, kSlotOpen = 3, kSlotGetTotalSize = 5;

using HeapSizeFn = uint32_t (*)(void* heap);
using HeapVoidFn = void (*)(void* heap);

HeapSizeFn g_origGetTotalSize = nullptr;
size_t g_blockOffset = 0;
void** g_heapVtable = nullptr;
std::atomic<uint64_t> g_guardHits{0};

uint32_t GuardedGetTotalSize(void* heap)
{
    if (!*reinterpret_cast<void**>(static_cast<uint8_t*>(heap) + g_blockOffset)) {
        // This context has never opened a command list, so it has no descriptor block yet.
        // Allocate it now, the way OpenCommandList would have (slot 3 of the object's own vtable).
        reinterpret_cast<HeapVoidFn>((*static_cast<void***>(heap))[kSlotOpen])(heap);
        if (g_guardHits.fetch_add(1, std::memory_order_relaxed) == 0)
            Log("[rdg] descriptor heap guard: first fresh command context got its view-heap block early (this used to be the 'Unable to reserve slot' crash)");
    }
    return g_origGetTotalSize(heap);
}

}  // namespace

bool InstallDescriptorHeapGuard()
{
    if (g_heapVtable && g_heapVtable[kSlotGetTotalSize] == reinterpret_cast<void*>(&GuardedGetTotalSize)) return true;
    const GameModule& game = Game();
    const Pattern sizeP = ParsePattern(kGetTotalSizePattern), openP = ParsePattern(kOpenPattern),
                  rollP = ParsePattern(kRollOverPattern);
    const auto candidates = FindAll(game.text, sizeP);
    void** found = nullptr;
    int count = 0;
    for (auto** slot = reinterpret_cast<void**>(game.rdata.begin) + kSlotGetTotalSize;
         slot + 1 <= reinterpret_cast<void**>(game.rdata.end); ++slot) {
        auto* fn = static_cast<uint8_t*>(*slot);
        if (!game.text.Contains(fn) || std::find(candidates.begin(), candidates.end(), fn) == candidates.end()) continue;
        void** vt = slot - kSlotGetTotalSize;
        auto* roll = static_cast<uint8_t*>(vt[kSlotRollOver]);
        auto* open = static_cast<uint8_t*>(vt[kSlotOpen]);
        if (!game.text.Contains(roll) || !game.text.Contains(open)) continue;
        if (!MatchesAt(roll, rollP) || !MatchesAt(open, openP)) continue;
        // Same CurrentBlock offset everywhere, and OpenCommandList's tail call is RollOver's AllocateBlock.
        if (fn[3] != open[3] || fn[3] != roll[17] || RipTarget(open + 5, 2, 6) != RipTarget(roll + 9, 1, 5)) continue;
        found = vt;
        ++count;
    }
    if (count != 1) {
        Log("[rdg] descriptor heap guard: sub-allocated view heap vtable found %d times (expected 1): guard unavailable", count);
        return false;
    }
    auto* origSize = static_cast<uint8_t*>(found[kSlotGetTotalSize]);
    g_blockOffset = origSize[3];
    g_origGetTotalSize = reinterpret_cast<HeapSizeFn>(origSize);
    if (!WritePointer(&found[kSlotGetTotalSize], reinterpret_cast<void*>(&GuardedGetTotalSize))) {
        Log("[rdg] descriptor heap guard: vtable write failed");
        return false;
    }
    g_heapVtable = found;
    Log("[rdg] descriptor heap guard installed: sub-allocated view heap vtable exe+0x%X, CurrentBlock at +0x%zX",
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(found) - game.base), g_blockOffset);
    return true;
}

bool DescriptorHeapGuardInfo(void*** vtable, size_t* blockOffset, void** hook)
{
    if (!g_heapVtable) return false;
    *vtable = g_heapVtable;
    *blockOffset = g_blockOffset;
    *hook = reinterpret_cast<void*>(&GuardedGetTotalSize);
    return true;
}

uint64_t DescriptorHeapGuardHits() { return g_guardHits.load(); }

}  // namespace opf
