// RtSceneBufferKeep self-test, included by selftest.cpp: FRayTracingScene::Create's TLAS size
// check and its three buffer creations on the mapped exe, the patch, and the stub's decisions run through a thunk that
// does the game's "cmp need,size" first: the game's way in mode 2, no re-creation for a smaller need in mode 1.
#pragma once

namespace rts {

inline void RtSceneSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    (void)game;
    constexpr uint32_t MiB = 1u << 20;
    Check(!RtSceneLogCreate(0, 32 * MiB, 32 * MiB, 400), "rtscene log: a quick same-size TLAS re-creation (menu, loading) is only counted");
    Check(RtSceneLogCreate(0, 48 * MiB, 32 * MiB, 400), "rtscene log: a TLAS size change is logged");
    Check(RtSceneLogCreate(0, 32 * MiB, 32 * MiB, 1000), "rtscene log: a TLAS creation >= 1 ms is logged");
    Check(!RtSceneLogCreate(1, 8 * MiB, 4 * MiB, 4999), "rtscene log: an upload buffer under 5 ms is only counted");
    Check(RtSceneLogCreate(2, 8 * MiB, 8 * MiB, 5000), "rtscene log: any creation >= 5 ms is logged");
    if (!Installed(FindRtSceneKeep(), "rtscene: FRayTracingScene::Create's TLAS size check and buffer creations found")) return;
    Check(!InstallRtSceneKeep(false), "rtscene: refuses to install after game code started");
    Check(InstallRtSceneKeep(true), "rtscene: installs before game code");
    Check(RtSceneKeepTestPatched(), "rtscene: check -> call stub + nop, jae kept; the three creations -> call [rip+slot]");
    constexpr uint64_t M = 1ull << 20;
    SetRtSceneKeep(2);  // the game's way
    Check(RtSceneKeepTestDecide(64 * M, 32 * M) == 0, "rtscene (2): need > size re-creates");
    Check(RtSceneKeepTestDecide(32 * M, 32 * M) == 1, "rtscene (2): need == size keeps");
    Check(RtSceneKeepTestDecide(16 * M, 32 * M) == 1, "rtscene (2): need == size / 2 keeps");
    Check(RtSceneKeepTestDecide(16 * M, 48 * M) == 0, "rtscene (2): need < size / 2 re-creates");
    SetRtSceneKeep(1);  // keep
    Check(RtSceneKeepTestDecide(64 * M, 32 * M) == 0, "rtscene (1): need > size still re-creates");
    Check(RtSceneKeepTestDecide(16 * M, 48 * M) == 1, "rtscene (1): need < size / 2 keeps the buffer");
    Check(RtSceneKeepTestDecide(16 * M, 256 * M) == 1, "rtscene (1): a much smaller need keeps the buffer");
    Check(RtSceneKeepTestDecide(32 * M, 32 * M) == 1, "rtscene (1): need == size keeps");
    SetRtSceneKeep(2);
    Check(RtSceneKeepTestDecide(16 * M, 256 * M) == 0, "rtscene: back to 2, the game's way again");
}

}  // namespace rts
