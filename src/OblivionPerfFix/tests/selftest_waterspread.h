// WaterInfoSpread self-test (worker waterspread 2026-11-03; public since 1.3.0, port130k), included by selftest.cpp: its sites match
// on the mapped exe (the inlined AWaterZone::Update's call to UpdateWaterInfoTexture, GetZoneBounds2D, UWaterSubsystem::Tick's one
// vtable slot) and the water zone's layout read from UpdateWaterInfoTexture's code equals the one WaterInfoSkip checks (water_zone.h:
// flag +0x300, WaterHeightExtents +0x304, GroundZMin +0x30C, texture +0x298); and the decision logic with fakes for the game's
// functions (water_spread.cpp's WaterSpreadTestLogic: first renders, near zones, unknown position, forced updates, a tick gap and
// mode 0 never wait; one zone per frame oldest first; the frame bound; the merge hold ends after its time and the update then
// happens; merge and spread together hold no request longer than max(MaxFrames frames, MergeMs) from its first ask, at 60 and at
// 10 fps; a failed update is asked again within the bound; public values keep only the spread and merge bits).
#pragma once

static void WaterSpreadRun(const GameModule& game)
{
    uint32_t flag = 0, tex = 0, ext = 0, gz = 0;
    const int ok = WaterSpreadTestDiscover(game, &flag, &tex, &ext, &gz);
    Check(ok == 1, "WaterInfoSpread: every site and the shared water-zone layout match on the mapped exe");
    if (ok != 1 || flag != 0x300 || tex != 0x298 || ext != 0x304 || gz != 0x30C)
        printf("    found %d, flag +0x%X, texture +0x%X, extents +0x%X, GroundZMin +0x%X\n", ok, flag, tex, ext, gz);
    const int fail = WaterSpreadTestLogic();
    Check(fail == 0, "WaterInfoSpread: defer / merge / flush logic with fakes; merge + spread within max(MaxFrames, MergeMs)");
    if (fail) printf("    failed checks (bits): 0x%X\n", fail);
}
