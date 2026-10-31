// MonitorNativeFromWindows / FullscreenAllResolutions self-test, included by selftest.cpp: both sites found
// where build 19115871 has them, then installed in the mapped copy and the written bytes checked.
#pragma once

namespace dnat {

inline void Run(const opf::GameModule& game)
{
    using namespace opf;
    DisplayNativeInfoOut d{};
    if (!Installed(FindDisplayNative(&d) && d.callSite && d.filterJump, "MonitorNativeFromWindows / FullscreenAllResolutions")) return;
    Check(d.callSite == game.base + 0x11840B5, "display native: RebuildDisplayMetrics' GetMonitorsInfo call at exe+0x11840B5");
    Check(d.getMonitorsInfo == game.base + 0x1172DB0, "display native: GetMonitorsInfo at exe+0x1172DB0");
    Check(d.filterJump == game.base + 0x31AB49E, "display native: the fullscreen filter's jnz at exe+0x31AB49E");
    uint8_t* const end = RipTarget(d.filterJump, 2, 6);
    Check(InstallMonitorNativeFromWindows(), "display native: GetMonitorsInfo's call redirected");
    Check(d.callSite[0] == 0xE8 && !game.text.Contains(RipTarget(d.callSite, 1, 5)), "display native: the call goes to a stub outside the game's code");
    Check(InstallFullscreenAllResolutions(), "display native: fullscreen filter skipped");
    Check(d.filterJump[0] == 0xE9 && d.filterJump[5] == 0x90 && RipTarget(d.filterJump, 1, 5) == end,
          "display native: the filter's jnz is a jmp to the same end");
}

}  // namespace dnat
