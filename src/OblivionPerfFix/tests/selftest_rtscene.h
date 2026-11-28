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
    // s001load: RtSceneBufferGrowToPeak's size
    Check(RtSceneGrowSize(64 * MiB, 16 * MiB, 112 * MiB, 1) == 112 * MiB, "rtscene grow: a growth in a loading screen goes to the session's peak");
    Check(RtSceneGrowSize(64 * MiB, 16 * MiB, 112 * MiB, 0) == 64 * MiB, "rtscene grow: a growth in play is the game's step (review S2-1)");
    Check(RtSceneGrowSize(64 * MiB, 16 * MiB, 112 * MiB, -1) == 64 * MiB, "rtscene grow: unknown state (no HitchLedger): the game's step");
    Check(RtSceneGrowSize(32 * MiB, 16 * MiB, 112 * MiB, 1) == 64 * MiB, "rtscene grow: at most twice the need (review S2-2)");
    Check(RtSceneGrowSize(16 * MiB, 112 * MiB, 112 * MiB, 1) == 16 * MiB, "rtscene grow: a smaller first need (new scene) is made at the need");
    Check(RtSceneGrowSize(128 * MiB, 112 * MiB, 112 * MiB, 1) == 128 * MiB, "rtscene grow: a need above the peak is made at the need");
    Check(RtSceneGrowSize(32 * MiB, 32 * MiB, 112 * MiB, 1) == 32 * MiB, "rtscene grow: a same-size re-creation (menu) stays at its size");
    Check(RtSceneGrowSize(64 * MiB, 16 * MiB, 0, 1) == 64 * MiB, "rtscene grow: no peak yet, the need");
    Check(RtSceneGrowSize(80 * MiB, 64 * MiB, 112 * MiB, 1) % (16 * MiB) == 0, "rtscene grow: the size stays on the game's 16 MiB granularity");
    // LoadPeakKeepsBack's decision (MB): within the margin of the budget, only in a loading screen
    Check(LoadPeakKeepsDecide(true, 1, 9700, 11200, 12950, 2560), "loadkeeps: a loading screen after play near the budget: keeps back");
    Check(LoadPeakKeepsDecide(true, 1, 12100, 9900, 12950, 2560), "loadkeeps: the load itself climbs near the budget: keeps back");
    Check(!LoadPeakKeepsDecide(true, 1, 8400, 9900, 12950, 2560), "loadkeeps: a low-memory loading screen (interior, plane): keeps stay");
    Check(!LoadPeakKeepsDecide(true, 0, 12100, 12400, 12950, 2560), "loadkeeps: in play: keeps stay");
    Check(!LoadPeakKeepsDecide(true, -1, 12100, 12400, 12950, 2560), "loadkeeps: loading state unknown: keeps stay");
    Check(!LoadPeakKeepsDecide(false, 1, 12100, 12400, 12950, 2560) && !LoadPeakKeepsDecide(true, 1, 12100, 12400, 0, 2560),
          "loadkeeps: key off or no budget yet: keeps stay");
    Check(LoadPeakKeepsDecide(true, 1, 4600, 0, 7000, 2560) && !LoadPeakKeepsDecide(true, 1, 4300, 0, 7000, 2560),
          "loadkeeps: an 8 GB card: the margin is a size (most outdoor loads act)");
    Check(!LoadPeakKeepsDecide(true, 1, 12000, 0, 12950, 100) && LoadPeakKeepsDecide(true, 1, 12500, 0, 12950, 100) &&
              !LoadPeakKeepsDecide(true, 1, 4000, 0, 12950, 99999) && LoadPeakKeepsDecide(true, 1, 4800, 0, 12950, 99999),
          "loadkeeps: the margin is held within 512..8192 MB");
    Check(LoadPeakBudgetFloor(11800, 15419) == 11800, "loadkeeps budget: the lowest of the window above the floor is used");
    Check(LoadPeakBudgetFloor(8000, 15419) == 15419 * 7 / 10, "loadkeeps budget: one low sample is floored at 70 % of the session's highest");
    Check(LoadPeakBudgetFloor(12950, 12950) == 12950, "loadkeeps budget: a steady budget is used as it is");
    // the loading-screen state both S-001 keys read (the hitch ledger's rule)
    Check(LoadingStateOf(0, 1, false, true) == 1, "loading state: the freeze's loading mode, no named freeze: a loading screen");
    Check(LoadingStateOf(-1, -1, true, false) == 1 && LoadingStateOf(2, 1, true, true) == 1, "loading state: a travel pending is a loading screen");
    Check(LoadingStateOf(0, 0, false, true) == 0, "loading state: play");
    Check(LoadingStateOf(1, 1, false, true) == 0 && LoadingStateOf(2, 0, false, true) == 0, "loading state: a menu (named freeze, e.g. the map) is not");
    Check(LoadingStateOf(-1, -1, false, false) == -1, "loading state: freeze state unknown: unknown");
    SetLoadingStateWanted(false);
    LoadingStateFrame(true);
    Check(LoadingScreenNow() == -1, "loading state: nothing read while no reader key is on (-1)");
    SetLoadingStateWanted(true);
    LoadingStateFrame(true);
    Check(LoadingScreenNow() == 1, "loading state: written by the frame hook while wanted");
    // UploadMemoryKeep's hand-back gate (review s001rev S2-5): only in loading frames
    SetUploadMemoryKeep(false, 0, 0, 0, UploadKeepLagFor(0));
    SetLoadPeakHandBackGate(true);
    Check(UploadLagTest(20) == 20, "upload gate: in a loading frame the game's 20-frame lag (hand back)");
    SetLoadingStateWanted(false);
    LoadingStateFrame(false);  // -> -1: not a loading screen
    Check(UploadLagTest(20) == UploadKeepLagFor(0), "upload gate: the screen is over before the hand-back: kept as configured");
    SetLoadPeakHandBackGate(false);
    Check(UploadLagTest(20) == 20, "upload gate: without LoadPeakKeepsBack the off key is the game's 20 frames everywhere");
    SetUploadMemoryKeep(false, 0, 0, 0, 0);
    Check(UploadLagTest(20) == 20, "upload gate: no gate lag: the game's");
    Check(UploadKeepLagFor(10) == 0 && UploadKeepLagFor(60) == 60, "upload: keep frames 1..20 are the game's, more are kept");
    // LoadPeakDrain: the repeat decision and the page-release site
    Check(!LoadPeakDrainRepeatDecide(false, 0, 4), "loaddrain: not draining: the game's one page a frame");
    Check(LoadPeakDrainRepeatDecide(true, 0, 4) && LoadPeakDrainRepeatDecide(true, 2, 4) && !LoadPeakDrainRepeatDecide(true, 3, 4),
          "loaddrain: draining: up to LoadPeakDrainPages releases in one frame (the game's first included)");
    Check(!LoadPeakDrainRepeatDecide(true, 0, 0) && LoadPeakDrainRepeatDecide(true, 14, 99) && !LoadPeakDrainRepeatDecide(true, 15, 99),
          "loaddrain: the page count is held within 1..16");
    SetLoadPeakDrain(true);
    SetLoadingStateWanted(true);
    LoadingStateFrame(true);
    Check(!LoadPeakDrainNow(), "loaddrain: a loading screen without a near-budget tick: not draining");
    SetLoadPeakDrain(false);
    SetLoadingStateWanted(false);
    LoadingStateFrame(false);
    {
        uint32_t callRva = 0, guardRva = 0;
        if (Installed(FindLoadPeakDrain(game, &callRva, &guardRva), "loaddrain: the fast allocator's page release and its guard found")) {
            Check(callRva > guardRva && callRva - guardRva < 0x140, "loaddrain: the guard sits just before the page release");
            Check(!InstallLoadPeakDrain(false), "loaddrain: refuses to install after game code started");
            Check(InstallLoadPeakDrain(true) && LoadPeakDrainTestPatched(), "loaddrain: call operator delete -> call stub (delete, repeat, guard slots)");
        }
    }
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
    // RtSceneBufferGrowToPeak through the hook (review s001rev S2-2 / revgrowpeak S3-8): a fake RHI records the desc it is given
    {
        uint32_t seen[3] = {};
        SetRtSceneKeep(1);
        SetRtSceneGrowPeak(true);
        RtSceneTestGrowReset();
        Check(RtSceneTestGrowCreate(112 * MiB, 0, 0x4321, 0, seen) == 112 * MiB, "rtscene grow hook: a creation in play is made at its need");
        Check(RtSceneTestGrowCreate(16 * MiB, 0, 0x4321, 1, seen) == 16 * MiB, "rtscene grow hook: a smaller first need is made at the need");
        Check(RtSceneTestGrowCreate(64 * MiB, 0, 0x4321, 1, seen) == 112 * MiB, "rtscene grow hook: a growth in a loading screen goes to the peak");
        Check(seen[1] == 0 && seen[2] == 0x4321, "rtscene grow hook: the passed desc is a copy with only Size changed");
        RtSceneTestGrowReset();
        RtSceneTestGrowCreate(112 * MiB, 0, 0x4321, 0, seen);
        RtSceneTestGrowCreate(16 * MiB, 0, 0x4321, 1, seen);
        Check(RtSceneTestGrowCreate(80 * MiB, 0, 0x4321, 1, seen) == 112 * MiB, "rtscene grow hook: a later growth goes to the same peak (min(peak, 2 x need))");
        RtSceneTestGrowCreate(16 * MiB, 0, 0x4321, 1, seen);
        Check(RtSceneTestGrowCreate(64 * MiB, 0, 0x4321, 0, seen) == 64 * MiB, "rtscene grow hook: loading 0: the game's step");
        RtSceneTestGrowCreate(16 * MiB, 0, 0x4321, 1, seen);
        Check(RtSceneTestGrowCreate(64 * MiB, 0, 0x4321, -1, seen) == 64 * MiB, "rtscene grow hook: loading unknown: the game's step");
        RtSceneTestGrowCreate(16 * MiB, 0, 0x4321, 1, seen);
        SetRtSceneKeep(2);
        SetRtSceneGrowPeak(true);
        Check(RtSceneTestGrowCreate(64 * MiB, 0, 0x4321, 1, seen) == 64 * MiB, "rtscene grow hook: the keep not in mode 1: the game's step");
        SetRtSceneKeep(1);
        SetRtSceneGrowPeak(false);
        RtSceneTestGrowCreate(16 * MiB, 0, 0x4321, 1, seen);
        Check(RtSceneTestGrowCreate(64 * MiB, 0, 0x4321, 1, seen) == 64 * MiB, "rtscene grow hook: key off: the game's step");
        RtSceneTestGrowReset();
        SetRtSceneKeep(2);
    }
}

}  // namespace rts
