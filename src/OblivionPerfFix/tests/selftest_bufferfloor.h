// InstanceBufferFloor self-test (public; lab SceneBufferFloorMiB parts in the lab build), included by selftest.cpp: the gate and tiers, and
// the reserve through GpuSceneKeepCapacity's thunk (needs that fix's test to have run); lab: sites on the mapped exe, the stub through a
// thunk, the timing hook with a fake, the patches and their undoing, the SceneBuffer floor's arming.
#pragma once

static void BufferFloorRun(const GameModule& game)
{
    (void)game;  // the public build reads no sites

    constexpr uint64_t M = 1ull << 20;
    // Budgets in MiB as read at the start of a session: 16 GB here 15419; 12 GB about 11560; 8 GB about 7700
    auto c = BufferFloorChoose(15419, 5000, 1, 1);
    Check(c.on && c.instanceSlots == 1u << 22 && c.sceneBytes == 144 * M, "bufferfloor gate: 16 GB card: on, 192 MiB instances, 144 MiB SceneBuffer");
    Check(!BufferFloorChoose(11560, 3000, 1, 1).on, "bufferfloor gate: 12 GB card: off");
    Check(!BufferFloorChoose(7700, 1000, 1, 1).on, "bufferfloor gate: 8 GB card: off");
    Check(!BufferFloorChoose(13999, 1000, 1, 1).on && BufferFloorChoose(14000, 1000, 1, 1).on, "bufferfloor gate: the line is 14000 MiB");
    Check(BufferFloorChoose(15419, 14395, 1, 1).on && !BufferFloorChoose(15419, 14396, 1, 1).on, "bufferfloor gate: and 1024 MiB free");
    c = BufferFloorChoose(15419, 5000, 0, 100);
    Check(c.on && c.instanceSlots == 0 && c.sceneBytes == 112 * M, "bufferfloor gate: an explicit size rounds up to 16 MiB; instance key off");
    Check(BufferFloorChoose(15419, 5000, 1, 0).sceneBytes == 0, "bufferfloor gate: SceneBuffer key off");
    Check(BufferFloorChoose(15419, 5000, 0, 1000).sceneBytes == 256 * M, "bufferfloor gate: an explicit size is capped");


    // Keys off: nothing changes
    BufferFloorTestKeys(0, 0, 15419, 5000);
    Check(BufferFloorGrow(0, 1u << 20, 1u << 20) == 1u << 20 && !BufferFloorHold(0, 1u << 20, 1u << 22), "bufferfloor: keys off: the game's size");

    // The reserve through GpuSceneKeepCapacity's thunk, as UpdateBufferState calls it
    GpuSceneFixInfo gs{};
    const bool haveInfo = GpuSceneInfo(&gs);
    Check(haveInfo && gs.strideCall[0], "bufferfloor: gpuscene info");
    if (!haveInfo || !gs.strideCall[0]) return;
    SetGpuSceneKeepCapacityEnabled(true);
    static const uint8_t kCaller[] = {0x57, 0x56, 0x89, 0xCF, 0x48, 0x89, 0xD6, 0x48, 0x83, 0xEC, 0x28, 0x41, 0xFF, 0xD0, 0x89, 0xF8, 0x48, 0x83, 0xC4, 0x28, 0x5E, 0x5F, 0xC3};
    void* const mem = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    memcpy(mem, kCaller, sizeof kCaller);
    using CallerFn = uint32_t (*)(uint32_t need, uint8_t* gpuScene, void* target);
    const auto reserve = [&](uint32_t need, uint8_t* scene) { return reinterpret_cast<CallerFn>(mem)(need, scene, RipTarget(gs.strideCall[0], 1, 5)); };
    // These blocks run in this order on purpose: the policy's session largest (GpuSceneKeep) carries over from one to the next.
    constexpr uint32_t S18 = 1u << 18, S19 = 1u << 19, S20 = 1u << 20, S21 = 1u << 21, S22 = 1u << 22, S23 = 1u << 23;
    const auto start = [&](uint8_t* scene) {  // a small scene whose buffer exists: under every threshold
        Check(reserve(1u << 10, scene) == 1u << 10, "bufferfloor: a small scene keeps the game's size");
        *reinterpret_cast<void**>(scene + 0x30) = scene;
    };
    {   // A 12 GB card at the crossing: no for the whole session, also when the card looks better later
        alignas(16) static uint8_t scene[0x400] = {};
        BufferFloorTestKeys(1, 1, 11560, 3000);
        start(scene);
        Check(reserve(S21, scene) == S21, "bufferfloor: 12 GB card: the game's size at the crossing");
        BufferFloorTestVram(15419, 5000);
        Check(reserve(S18, scene) == S18, "bufferfloor: 12 GB card: it shrinks as before");
        Check(reserve(S21, scene) == S21, "bufferfloor: decided once: a better look later changes nothing");
    }
    {   // Too little free at the crossing
        alignas(16) static uint8_t scene[0x400] = {};
        BufferFloorTestKeys(1, 0, 15419, 14900);
        start(scene);
        Check(reserve(S21, scene) == S21, "bufferfloor: under 1024 MiB free: the game's size");
    }
    {   // The memory figure not known at the crossing: off, and that decision stands too
        alignas(16) static uint8_t scene[0x400] = {};
        BufferFloorTestKeys(1, 0, 0, 0);
        start(scene);
        Check(reserve(S21, scene) == S21, "bufferfloor: budget not known: the game's size");
        BufferFloorTestVram(15419, 5000);
        Check(reserve(S18, scene) == S18 && reserve(S21, scene) == S21, "bufferfloor: budget not known at the crossing: off for the session");
    }
    {   // On: the floor, the hold, the shrink, the way back
        alignas(16) static uint8_t scene[0x400] = {};
        BufferFloorTestKeys(1, 1, 15419, 5000);
        start(scene);
        Check(reserve(S21, scene) == S22, "bufferfloor: from 2^21 slots the reserve is 2^22");
        Check(reserve(S21, scene) == S22, "bufferfloor: the same need: kept");
        BufferFloorTestVram(15419, 15400);
        Check(reserve(S21, scene) == S22, "bufferfloor: a full card later: no give-back");
        Check(reserve(S20, scene) == S22, "bufferfloor: need at a quarter: held (GpuSceneKeepCapacity alone would shrink here)");
        Check(reserve(S19, scene) == S19, "bufferfloor: under that (an interior): shrinks as before");
        Check(reserve(S20, scene) == S22, "bufferfloor: and back: straight to the floor's size");
    }
    {   // Above the floor: the game's way
        alignas(16) static uint8_t scene[0x400] = {};
        BufferFloorTestKeys(1, 0, 15419, 5000);
        start(scene);
        Check(reserve(S22, scene) == S22, "bufferfloor: a first need of 2^22: as asked");
        Check(reserve(S23, scene) == S23, "bufferfloor: a need above the floor grows the game's way");
    }
    BufferFloorTestKeys(0, 0, -1, 0);
    VirtualFree(mem, 0, MEM_RELEASE);
}
