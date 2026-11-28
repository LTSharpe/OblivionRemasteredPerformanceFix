// WaterRebuildSkip self-test, included by selftest.cpp: the six byte patterns are found once each at their known places on the
// mapped exe (build 19115871), the two detoured entries start with the instructions the trampolines copy and take an aligned
// 5-byte jump, the water tick's Update calls the RebuildWaterMesh found, and the decision logic (taken back / kept for each
// rule) works on fake objects. Nothing is installed.
#pragma once

static void WaterRebuildSkipRun(const GameModule& game)
{
    WaterRebuildSkipSites s;
    const bool found = WaterRebuildSkipFind(game, &s);
    if (game.timeDateStamp == 0xF19077A4u) {
        Check(found, "WaterRebuildSkip: all water patterns found once (notify, UCV, rebuild, tick, add, weak get, lambda, ShouldRender, river MIDs, view extension)");
        if (!found && s.why) printf("    %s\n", s.why);
        Check(s.notify == game.base + 0x42DBC70, "WaterRebuildSkip: NotifyWaterVisibilityChangedInternal at exe+0x42DBC70");
        Check(s.ucv == game.base + 0x42EBC80, "WaterRebuildSkip: UpdateComponentVisibility at exe+0x42EBC80");
        Check(s.rebuild == game.base + 0x42E1BD0, "WaterRebuildSkip: RebuildWaterMesh at exe+0x42E1BD0");
        Check(s.tickPin == game.base + 0x42E814B && s.tickCallsRebuild, "WaterRebuildSkip: the tick's Update (exe+0x42E814B) calls that RebuildWaterMesh");
        Check(s.addPin == game.base + 0x42BC5F0, "WaterRebuildSkip: AddWaterBodyComponent at exe+0x42BC5F0 (zone body list +0x2A0)");
        Check(s.weakGet == game.base + 0x1126290, "WaterRebuildSkip: FWeakObjectPtr::Get at exe+0x1126290");
        Check(s.lambda == game.base + 0x42B4D87 && s.shouldRender == game.base + 0x308C6C0 && s.lambdaCallsShouldRender,
              "WaterRebuildSkip: the rebuild's per-body lambda (WaterMID +0xDC8, spline bounds) calls ShouldRender (owner hidden +0x58)");
        Check(s.viewExtPin == game.base + 0x42E667D && s.sceneProxyOff == 0x578, "WaterRebuildSkip: SceneProxy +0x578 found by the scan after WaterViewExtension's anchor exe+0x42E667D");
        Check(s.createProxy == game.base + 0x42C4670 && (reinterpret_cast<uintptr_t>(s.createProxy) & 7) == 0, "WaterRebuildSkip: CreateSceneProxy at exe+0x42C4670 (aligned entry)");
        Check(s.riverMids == game.base + 0x42C4030, "WaterRebuildSkip: the river transition MIDs (+0x1588, +0x1598) at exe+0x42C4030");
        Check(s.notify && (reinterpret_cast<uintptr_t>(s.notify) & 7) == 0 && s.rebuild && (reinterpret_cast<uintptr_t>(s.rebuild) & 7) == 0,
              "WaterRebuildSkip: both entry jumps fit one aligned qword (atomic write)");
        // the loop is reached only through its delegate: one lea of it (UWaterSubsystem::Initialize), no direct call
        int leas = 0, calls = 0;
        for (uint8_t* p = game.text.begin; s.notify && p + 7 <= game.text.end; ++p) {
            if (p[0] == 0xE8) {
                int32_t rel;
                memcpy(&rel, p + 1, 4);
                if (p + 5 + rel == s.notify) ++calls;
            }
            if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05) {
                int32_t rel;
                memcpy(&rel, p + 3, 4);
                if (p + 7 + rel == s.notify) ++leas;
            }
        }
        Check(leas == 1 && calls == 0, "WaterRebuildSkip: the visibility loop is bound once as a delegate (1 lea, no direct call)");
    } else {
        Check(true, "WaterRebuildSkip: unknown build, pattern search only");
    }
    const int fail = WaterRebuildSkipTestLogic();
    Check(fail == 0, "WaterRebuildSkip: taken back / kept (other writer, inputs changed, tree changed, disabled with / without proxy, unreadable, proxy lost -> mark given back, lost-proxy count) on fakes");
    if (fail) printf("    failed checks (bits): 0x%X\n", fail);
}
