// InstanceBufferFloor (worker rtlate, 2026-11-06; findings `rtlate`, `bufferfloor`, reviews `floorreview` x2; public since 1.3.0, workers
// port130a/port130c 2026-11-08) and lab SceneBufferFloorMiB. [Fixes] InstanceBufferFloor, default 1 (any != 0 = on), read at start-up;
// SceneBufferFloorMiB stays lab only (user, 2026-11-06), default 0, read by the lab build from [Fixes] at start-up.
// Public form: no code patch and no pattern of its own: the gate and the size live in GpuSceneKeep (GpuSceneKeepCapacity's reserve,
// stream_churn.cpp, found by that fix's patterns), so without GpuSceneKeepCapacity it does nothing. The lab build also patches the
// SceneBuffer floor and times the instance buffer's creations (below).
//
// The problem. Two render-thread buffers grow in steps as a first ride reaches new places, each step a new committed resource made inside
// the frame while the render thread waits on the driver's paging fence: FGPUScene's InstanceSceneData (96 -> 192 MiB once a session in play,
// wait median 4.9 ms, up to 23.6; the +112.9 s red dip of `ridetrace`) and FRayTracingScene's SceneBuffer (16 MiB steps, 1.7-5 ms each, one
// 11.2 ms step made a red dip in `ride1105`). Neither has a cvar or an engine minimum: InitialBufferSize is a constexpr, the TLAS granularity
// a local constant, r.GPUScene.UseGrowOnlyAllocationPolicy only stops shrinking.
//
// The fix: make the size many sessions reach anyway at the moment the game first makes a large buffer, mostly while a load is ending, instead of in a later play frame.
// - InstanceBufferFloor=1 (public; user, 2026-11-06 and 2026-11-07): when the game first asks for >= 2^21 instance slots the reserve becomes 2^22 slots (192 MiB at
//   48 bytes). When that request comes: of 165 ledgered first crossings 78 (47 %) are in state loading, 145 (88 %) at or within 1 s of a load, 5 over 3 s after, up to 92.9 s. So it
//   isn't always behind a loading screen; a late crossing is one creation where stock would have made two. 644 of 1128 real sessions (57 %) reach 192 MiB anyway. Extra video memory:
//   at most +96 MiB (192 against the 96 GpuSceneKeepCapacity would hold at that need), until the session reaches 192 itself or for as long as it stays at 96 (292 of 1128, but 240 of
//   those span under 60 s, mostly short benchmarks; of 71 spanning 300 s or more, 20 stay at 96 and 50 reach 192); a session that stays at 48 or less pays nothing.
//   Decided in GpuSceneKeep (GpuSceneKeepCapacity, stream_churn.cpp), which already changes the reserve where the game computes it, so the buffer size and the SOA stride come
//   from the one value; every reader takes the stride from InstanceSceneDataSOAStride, 2^22 is a value stock itself reaches, and the same data sits at the same logical slots
//   (the stride was checked in game by GpuSceneTestDouble, findings `gpuscene-vis`). The floor holds down to a need of 2^20, as GpuSceneKeepCapacity would at a capacity of 2^21;
//   below that it shrinks as before.
// - The gate: decided once a session, at that first crossing, from the memory monitor's last sample (no DXGI call here, no thread of its own). On: a budget >= 14000 MiB and
//   >= 1024 MiB free; otherwise the game's sizes for the whole session, so no creation can land in a play frame later because the card looked better then. 14000 limits it to
//   16 GB-class cards, as CmdMemPregrow: the budget reads about 95 % of the card at the start (15419 MiB of 15.81 GiB here; 12 GB about 11560, 8 GB about 7700) but falls within
//   1-2 minutes in play (12.5-12.8 GiB here), so a 12 GB card would pass at the start and be short later. On this 16 GB card 57 of 1791 sessions read under 14000 at the crossing
//   (later crossings, budget tests) and stay stock, the safe side. There is no data on any other card.
// - No give-back: with that line the floor can't be left holding memory on a small card. Under 0.5 GiB free is 22 of 4758 [mem] lines here, mostly cold starts. Giving back would
//   return 96 MiB at the price of a creation near a full card in any frame (the old buffer stays in the RDG pool 30 frames, so used rises first; largest logged waits 174-178 ms) and
//   another if the need rises again. CmdMemPregrow's give-back acts at that line and frees far more. Only SharedMemoryGpu undoes the floor (GpuSceneKeepCapacity off); MemoryRelief
//   and VramRelief leave GPU scene capacity alone.
// - SceneBufferFloorMiB (lab only, not a candidate for the public build; user, 2026-11-06): 0 off, 1 automatic (144 MiB), N >= 2: N MiB rounded
//   up to 16, 32-256. Armed by the same decision, so it also needs GpuSceneKeepCapacity on. When the scene needs more than 16 MiB the rounded-up size is at least the floor. The TLAS
//   is built at the start of the buffer it is given (RtSceneBufferKeep's argument, shipped). How: in FRayTracingScene::Create, cmova rcx,rax; mov rax,[r13] (8 bytes, just before
//   RtSceneBufferKeep's check, no overlap) -> call stub. Known limit (floorreview S2): with RtSceneBufferKeep at 0 or 2 a need going across 16 MiB
//   re-creates the floor's size each time, where stock would keep its buffer.
// - Lab only: InstanceBufferFloor also times the instance buffer's creations: the two call sites of ResizeStructuredBufferSOAIfNeeded -> hook, one log line per size change with size, ms, QPC,
//   fade count and video memory. SceneBuffer's creations are in RtSceneBufferKeep's [rtscene] lines.
// Nothing is held for later, no game logic is touched. Lab: any mismatch patches nothing, with a log line. The log tag is [bufferfloor]. Addresses are build 19115871's.
#include "common.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace opf {
namespace {


constexpr uint32_t kNeedAtLeast = 1u << 21;  // slots the game asks for before the floor applies
constexpr uint32_t kHoldFrom = 1u << 20;     // the need the floor holds down to (what GpuSceneKeepCapacity keeps at a capacity of 2^21)
constexpr uint32_t kFloorSlots = 1u << 22;   // no saved log has more
constexpr double kMinBudgetMB = 14000, kMinFreeMB = 1024;  // MiB

int g_instanceKey = 0, g_sceneKey = 0;
bool g_installed = false;
enum class State { Waiting, On, Off };
State g_state = State::Waiting;  // decided once a session (render thread, under GpuSceneKeep's lock)
double g_testBudget = -1, g_testUsed = 0;  // budget -1: the monitor's sample; 0: not known yet


// The memory monitor's last sample (once a second on its own thread).
bool Sample(double* budgetMB, double* usedMB)
{
    if (g_testBudget >= 0) {
        *budgetMB = g_testBudget;
        *usedMB = g_testUsed;
        return g_testBudget > 0;
    }
    uint64_t used = 0, budget = 0;
    if (!LatestGameVram(&used, &budget) || !budget) return false;
    *budgetMB = static_cast<double>(budget) / 1048576.0;
    *usedMB = static_cast<double>(used) / 1048576.0;
    return true;
}


}  // namespace

BufferFloorChoice BufferFloorChoose(double budgetMB, double usedMB, int instanceKey, int sceneKey)
{
    BufferFloorChoice c{};
    if (budgetMB < kMinBudgetMB) { c.why = "the video memory budget is under 14000 MB"; return c; }
    if (budgetMB - usedMB < kMinFreeMB) { c.why = "under 1024 MB free in the video memory budget"; return c; }
    c.on = true;
    c.why = "budget and free video memory are enough";
    c.instanceSlots = instanceKey ? kFloorSlots : 0;
    if (sceneKey == 1) {
        c.sceneBytes = 144ull << 20;
    } else if (sceneKey > 1) {
        const uint64_t mib = (static_cast<uint64_t>(std::clamp(sceneKey, 32, 256)) + 15) / 16 * 16;
        c.sceneBytes = mib << 20;
    }
    return c;
}

uint32_t BufferFloorGrow(int kind, uint32_t use, uint32_t need)
{
    if (kind != 0 || !g_installed) return use;
    switch (g_state) {
    case State::Off:
        return use;
    case State::On:
        return g_instanceKey && need >= kNeedAtLeast ? std::max(use, kFloorSlots) : use;
    case State::Waiting:
        break;
    }
    if (need < kNeedAtLeast) return use;
    // Once a session: an answer of no stays no, so no creation lands in a play frame later on a tight card.
    double budget = 0, used = 0;
    const bool known = Sample(&budget, &used);
    const BufferFloorChoice c = known ? BufferFloorChoose(budget, used, g_instanceKey, g_sceneKey) : BufferFloorChoice{};
    g_state = c.on ? State::On : State::Off;
    Log("[bufferfloor] %s for this session: %s; budget %.0f MB, used %.0f MB; at a need of %u slots: instance floor %u slots, SceneBuffer floor %llu MiB", c.on ? "on" : "off",
        known ? c.why : "video memory not known yet", budget, used, need, c.instanceSlots, static_cast<unsigned long long>(c.sceneBytes >> 20));
    if (!c.on) return use;
    return std::max(use, c.instanceSlots);
}

bool BufferFloorHold(int kind, uint32_t need, uint32_t capacity)
{
    // Down to the size stock's own hysteresis would keep at the threshold (a capacity of 2^21 keeps a need of 2^20): below that it shrinks as stock would, so a
    // need going up and down around the threshold makes no more re-creations than stock's, only each at the floor's size.
    return kind == 0 && g_state == State::On && g_instanceKey && need >= kHoldFrom && capacity <= kFloorSlots;
}


// Start-up: the key (public: any != 0 = on). Public: nothing to find or patch (GpuSceneKeep calls BufferFloorGrow / Hold);
// the lab build also reads SceneBufferFloorMiB and patches its sites (SceneBuffer floor, instance-buffer creation timing).
bool InstallBufferFloor(bool beforeGameCode, int instanceKey, bool gpuSceneKeep, const wchar_t* iniPath)
{
    (void)beforeGameCode;
    (void)iniPath;
    g_instanceKey = instanceKey ? 1 : 0;
    if (!g_instanceKey && !g_sceneKey) {
        Log("[bufferfloor] InstanceBufferFloor off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (g_installed) return true;
    if (!gpuSceneKeep) {
        Log("[bufferfloor] InstanceBufferFloor: needs GpuSceneKeepCapacity (its reserve decides the size), not installed");
        return false;
    }
    g_installed = true;
    Log("[bufferfloor] InstanceBufferFloor installed: at the session's first need of %u GPU scene instance slots the reserve becomes %u "
        "(192 MiB) when the video memory budget is >= %.0f MiB with >= %.0f MiB free (decided once a session; held down to a need of %u; "
        "in GpuSceneKeepCapacity's reserve)", kNeedAtLeast, kFloorSlots, kMinBudgetMB, kMinFreeMB, kHoldFrom);
    return true;
}

// ---- self-test only ----
void BufferFloorTestKeys(int instanceKey, int sceneKey, double budgetMB, double usedMB)
{
    g_instanceKey = instanceKey;
    g_sceneKey = sceneKey;
    g_testBudget = budgetMB;
    g_testUsed = usedMB;
    g_installed = instanceKey || sceneKey;
    g_state = State::Waiting;
}

void BufferFloorTestVram(double budgetMB, double usedMB)
{
    g_testBudget = budgetMB;
    g_testUsed = usedMB;
}


}  // namespace opf
