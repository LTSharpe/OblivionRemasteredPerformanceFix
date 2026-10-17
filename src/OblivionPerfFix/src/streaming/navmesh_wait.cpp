// NavmeshBuildNoBusyWait: navmesh tile builds no longer run inside another task's wait.
//
// The game builds its navigation mesh at runtime (Recast) while the world streams in: every tile is an FAsyncTask on
// GThreadPool, which UE 5.3 runs as a low-level task on the worker threads. A worker that waits for another task with
// UE::Tasks BusyWait (LowLevelTasks::BusyWaitUntil) runs other queued tasks while it waits, any task flagged
// AllowBusyWaiting, and the wait cannot end until that task returns. Altar's NPC steering tick, a component tick that
// runs on a worker, busy-waits on a navigation task; when the waiting worker picks up a tile build (20-110 ms), the
// tick cannot finish until the tile is done, and the game thread waits for that tick in the world tick
// (FTickTaskSequencer::ReleaseTickGroup): an 84 ms frame on the Silver Road (2026-10-09), with every other worker idle.
//
// UE has a flag for exactly this, EQueuedWorkFlags::DoNotRunInsideBusyWait (the pool then launches the task without
// AllowBusyWaiting); Recast's tile tasks are started without it (UE 5.3 to 5.5). The fix sets it where the tile task is
// started: the inlined FAsyncTask::Start writes Priority and Flags as one 16-bit constant (Normal = 3, None = 0), and
// the flags byte becomes 1. The tiles are built by the same code from the same data, by a worker that is free; only
// which worker runs them, and when within the frame, changes.
//
// The site is found by its code (the tile generator's checks before the start, the inlined Start, the pool call); the
// task's vtable must return its flags from +0x29 (checked) and the byte must be 0 or 1 (ours).
#include "common.h"

#include <atomic>

namespace opf {
namespace {

// FRecastNavMeshGenerator::ProcessTileTasksAsync (exe+0x41FD360), after `new FRecastTileGeneratorTask(TileGenerator)`
// in rdi (build 19115871, exe+0x41FD4F1): the generator's "has data to build" checks, then the inlined
// StartBackgroundTask(GThreadPool): RequiredMemory = -1, DebugName = 0, counter++, QueuedPool = GThreadPool,
// word [rdi+28h] = 0x0003 (Priority Normal, Flags None) at exe+0x41FD57E, then QueuedPool->AddQueuedWork(this, 3).
constexpr char kTileStart[] =
    "48 8B 47 48 83 B8 ?? ?? 00 00 00 75 ?? 83 B8 ?? ?? 00 00 00 75 ?? 83 B8 ?? ?? 00 00 00 75 ?? 83 B8 ?? ?? 00 00 00 75 ?? "
    "83 B8 ?? ?? 00 00 00 74 ?? 83 B8 ?? ?? 00 00 00 7F ?? 80 BC 24 ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 4D 8B C4 49 8B D5 "
    "49 8B CF E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 45 33 ED 48 89 7C 24 ?? 44 39 2D ?? ?? ?? ?? 41 8B DD 75 ?? 48 8B 05 ?? ?? ?? ?? "
    "48 C7 47 30 FF FF FF FF 4C 89 6F 38 0F AE F8 F0 FF 47 10 48 89 47 20 66 C7 47 28 03 ?? 48 85 C0 74 ?? 48 39 5F 18 "
    "75 ?? B1 01 E8 ?? ?? ?? ?? 48 89 47 18 48 8B 4F 18 48 8B 01 FF 50 18 48 8B 4F 20 41 B0 03 48 8B D7 48 8B 01 FF 50 10";
constexpr size_t kFlagsByte = 0x92;  // the high byte of the 16-bit constant (Flags)
// Before it: the task's construction, "mov rcx,rax (new); call ctor; mov rdi,rax", within this many bytes.
constexpr char kCtorCall[] = "48 8B D3 48 8B C8 E8 ?? ?? ?? ?? 48 8B F8";
constexpr size_t kCtorSearch = 0x80;
// The constructor stores the IQueuedWork base vtable, then the task's own: "lea rax,[vtable]; mov [rdi+8],rcx; mov [rdi],rax"
constexpr char kCtorVtable[] = "48 8D 05 ?? ?? ?? ?? 48 89 4F 08 48 89 07";
// IQueuedWork::GetQueuedWorkFlags (vtable +0x10) of FAsyncTaskBase: movzx eax, byte [rcx+29h]; ret
constexpr char kFlagsGetter[] = "0F B6 41 29 C3";
constexpr uint8_t kDoNotRunInsideBusyWait = 1;

uint8_t* g_site = nullptr;           // the Flags byte
std::atomic<bool> g_on{false};

}  // namespace

bool FindNavmeshBuildNoBusyWait()
{
    if (g_site) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kTileStart));
    if (hits.size() != 1) {
        Log("[navmesh] tile task start not recognised (%zu matches): NavmeshBuildNoBusyWait unavailable", hits.size());
        return false;
    }
    uint8_t* start = hits[0];
    // the task object started there is the one constructed just before, whose flags getter reads +0x29
    const Pattern call = ParsePattern(kCtorCall);
    uint8_t* ctorCall = nullptr;
    for (uint8_t* p = start - call.size(); p >= start - kCtorSearch; --p)
        if (MatchesAt(p, call)) { ctorCall = p; break; }
    uint8_t* ctor = ctorCall ? RipTarget(ctorCall + 6, 1, 5) : nullptr;
    uint8_t* lea = nullptr;
    if (ctor && game.text.Contains(ctor)) {
        const Pattern vt = ParsePattern(kCtorVtable);
        for (size_t i = 0; i < 0x40 && !lea; ++i)
            if (MatchesAt(ctor + i, vt)) lea = ctor + i;
    }
    void* const* vtable = lea ? reinterpret_cast<void* const*>(RipTarget(lea, 3, 7)) : nullptr;
    const uint8_t* getter = vtable && game.rdata.Contains(vtable) ? static_cast<const uint8_t*>(vtable[2]) : nullptr;
    if (!getter || !game.text.Contains(getter) || !MatchesAt(getter, ParsePattern(kFlagsGetter))) {
        Log("[navmesh] tile task at exe+0x%X: its flags are not where expected: NavmeshBuildNoBusyWait unavailable",
            static_cast<unsigned>(start - game.base));
        return false;
    }
    uint8_t* site = start + kFlagsByte;
    if (*site > kDoNotRunInsideBusyWait) {
        Log("[navmesh] tile task flags byte exe+0x%X is %u (expected 0): NavmeshBuildNoBusyWait unavailable",
            static_cast<unsigned>(site - game.base), *site);
        return false;
    }
    g_site = site;
    Log("[navmesh] tile task start exe+0x%X (flags byte exe+0x%X, task vtable exe+0x%X)",
        static_cast<unsigned>(start - game.base), static_cast<unsigned>(site - game.base),
        static_cast<unsigned>(reinterpret_cast<const uint8_t*>(vtable) - game.base));
    return true;
}

void SetNavmeshBuildNoBusyWait(bool enabled)
{
    if (!g_site || g_on.load() == enabled) return;
    // One byte of an immediate, written only by this function (atomic for the code that runs it on the game thread).
    const uint8_t v = enabled ? kDoNotRunInsideBusyWait : 0;
    if (!WriteCode(g_site, std::span<const uint8_t>(&v, 1))) {
        Log("[navmesh] VirtualProtect failed: NavmeshBuildNoBusyWait not switched");
        return;
    }
    g_on = enabled;
    Log("[navmesh] navmesh tile builds %s", enabled ? "never run inside another task's busy wait" : "as before (may run inside busy waits)");
}

uint8_t* NavmeshBuildNoBusyWaitSite() { return g_site; }

}  // namespace opf
