// BusyWaitFlagCheck (0.27.x, BusyWaitFlagCheck): a busy-waiting thread never runs a task that asked not to run inside busy waits.
//
// UE 5.3's task scheduler lets a worker that waits for a task run other queued tasks meanwhile (LowLevelTasks
// BusyWaitUntil). A task launched without ETaskFlags::AllowBusyWaiting must not be picked up that way: the waiting
// thread cannot return to its own work until the picked-up task ends. NavmeshBuildNoBusyWait gives the navigation mesh's tile builds
// that flag (EQueuedWorkFlags::DoNotRunInsideBusyWait). But FScheduler::TryExecuteTaskFrom<..., bIsBusyWaiting = true>
// (Scheduler.cpp, 5.3) checks only the FIRST task it dequeues: when that one is not allowed it puts it back and dequeues
// another, and runs the second one without checking it. So a tile build still lands inside a busy wait whenever two
// disallowed tasks are next to each other in the queue (a streaming burst queues dozens of tiles).
//
// Seen: Altar's NPC steering tick (a TG_PrePhysics component tick on a
// worker) waits for a navigation task with BusyWaitUntil, runs further steering ticks inside that wait, and one of
// them picks up a tile build through the unchecked second dequeue: the game thread then waits in tick group 0 until
// the tile is built: 2,572 ms and 247 ms in two of four flights (every other worker idle), and the earlier 75-130 ms
// tick-group-0 waits of the IC flight look the same.
//
// The fix: after the put-back, the function returns "nothing executed" (as when the queue is empty) instead of
// dequeuing and running the next task unchecked; the caller's busy-wait loop then checks its condition and tries again
// (the next attempt dequeues the following task and checks it). In the four busy-wait instantiations (local queue,
// global queue, two steal paths) the put-back is followed by "test dil,dil (anything executed?); jne <return dil>";
// the jne becomes jno (one byte, 0F 85 -> 0F 81; TEST clears OF, so jno always jumps to the same return).
// What changes: a task that asked not to run inside busy waits runs on a free worker instead (UE's documented rule);
// tasks allowed in busy waits still run there on the next attempt. Every task runs, with the same code and inputs; only
// which thread runs it, and when, changes, which stock already varies with thread timing. Navigation tiles finish on
// another worker; the game thread no longer waits for them in the world tick.
#include "common.h"

#include <atomic>
#include <cstring>
#include <intrin.h>

namespace opf {
namespace {

// Up to the put-back (QueueRegistry.Enqueue, exe+0xDD8000 in build 19115871), then the AnyExecuted check:
//   mov rbx,[rbp+38h]; lea rax,[r13|r14+80h]; and ecx,7; mov rdx,rbp; lea rcx,[rcx+rcx*8]; shl rcx,9; add rcx,rax;
//   call Enqueue; mov eax,[r+5A94h]; shr rbx,3Bh; and bl,7; cmp bl,2; jae +8; nop; mov eax,[r+5A90h]; nop;
//   test dil,dil; jne <movzx eax,dil; epilogue>
constexpr char kRequeue[] =
    "48 8B 5D 38 49 8D ?? 80 00 00 00 83 E1 07 48 8B D5 48 8D 0C C9 48 C1 E1 09 48 03 C8 E8 ?? ?? ?? ?? "
    "41 8B ?? 94 5A 00 00 48 C1 EB 3B 80 E3 07 80 FB 02 73 08 90 41 8B ?? 90 5A 00 00 90 40 84 FF 0F 85 ?? ?? ?? ??";
// Just before it, the check of the dequeued task: "AllowBusyWaiting flag (bit 62) or canceled/running state", then
// "if (!allowed || AnyExecuted)": test al,1; jne; mov rax,[rbp+38h]; test al,5; je; test dil,dil; je rel32 | je rel8
constexpr char kCheck[] = "A8 01 75 08 48 8B 45 38 A8 05 74 ?? 40 84 FF";
constexpr size_t kJneOffset = 0x40;     // the jne's 0F 85 within kRequeue
constexpr size_t kCallOffset = 0x1C;    // the call to Enqueue
constexpr uint8_t kReturnPath[] = {0x40, 0x0F, 0xB6, 0xC7};  // movzx eax,dil at the jne's target
constexpr size_t kMaxSites = 8;

uint8_t* g_sites[kMaxSites] = {};       // the jne's second opcode byte (0x85 stock, 0x81 on)
size_t g_count = 0;
std::atomic<bool> g_on{false};

}  // namespace

bool FindBusyWaitFlagCheck()
{
    if (g_count) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kRequeue));
    const Pattern check = ParsePattern(kCheck);
    uint8_t* enqueue = nullptr;
    size_t n = 0;
    for (uint8_t* h : hits) {
        // the flag check right before: "je rel32" (0F 84 xx xx xx xx) or "je rel8" (74 xx) after it
        const bool rel32 = h[-6] == 0x0F && h[-5] == 0x84 && MatchesAt(h - 6 - check.size(), check);
        const bool rel8 = h[-2] == 0x74 && MatchesAt(h - 2 - check.size(), check);
        uint8_t* call = h + kCallOffset;
        uint8_t* target = RipTarget(call, 1, 5);
        uint8_t* jne = h + kJneOffset;
        uint8_t* ret = RipTarget(jne, 2, 6);
        if (!(rel32 || rel8) || !game.text.Contains(target) || (enqueue && target != enqueue) || !game.text.Contains(ret) ||
            memcmp(ret, kReturnPath, sizeof(kReturnPath)) != 0 || jne[1] != 0x85 || n >= kMaxSites) {
            Log("[busywait] put-back at exe+0x%X not as expected: BusyWaitFlagCheck unavailable", static_cast<unsigned>(h - game.base));
            return false;
        }
        enqueue = target;
        g_sites[n++] = jne + 1;
    }
    if (n == 0) {
        Log("[busywait] the busy-wait put-back was not found: BusyWaitFlagCheck unavailable");
        return false;
    }
    g_count = n;
    Log("[busywait] %zu busy-wait put-backs found (first exe+0x%X, put-back exe+0x%X)", n,
        static_cast<unsigned>(g_sites[0] - 1 - game.base), static_cast<unsigned>(enqueue - game.base));
    return true;
}

void SetBusyWaitFlagCheck(bool enabled)
{
    if (!g_count || g_on.load() == enabled) return;
    const uint8_t v = enabled ? 0x81 : 0x85;  // jno (always taken after TEST) / jne
    for (size_t i = 0; i < g_count; ++i) {
        if (!WriteCode(g_sites[i], std::span<const uint8_t>(&v, 1))) {
            Log("[busywait] VirtualProtect failed at site %zu: BusyWaitFlagCheck left as it was", i);
            // put back what was written so all sites agree
            const uint8_t old = enabled ? 0x85 : 0x81;
            for (size_t j = 0; j < i; ++j) WriteCode(g_sites[j], std::span<const uint8_t>(&old, 1));
            return;
        }
    }
    g_on = enabled;
    Log("[busywait] %s", enabled ? "tasks that must not run inside busy waits are never run there (the second dequeue is checked)"
                                 : "as before (a second dequeued task runs unchecked)");
}

void LogBusyWaitStats() {}

size_t BusyWaitFlagCheckSites(uint8_t** out, size_t max)
{
    for (size_t i = 0; i < g_count && i < max; ++i) out[i] = g_sites[i];
    return g_count;
}

}  // namespace opf
