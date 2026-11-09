// WaterInfoSkip self-test (worker waterinfo 2026-11-07; public since 1.3.0, port130g), included by selftest.cpp after the water-capture
// test (which installs the game-thread detour on the mapped exe): every layout read the fingerprint relies on, in
// UpdateWaterInfoRendering's code; the game-thread decision with fakes (first redraw, same inputs skipped, every fingerprint part
// changing, a re-created zone or render target resource, the zone moving, value 0 never skipping, a change of value forgetting
// every zone (rev130d WI-M1), unreadable contexts; lab: value 2 queueing the candidate for the render thread, the read-back
// function on the mapped exe).
#pragma once

static void WaterInfoSkipRun(const GameModule& game)
{
    uint8_t* const fn = WaterInfoGtSite();
    if (fn) {
        const char* bad = WaterInfoSkipTestPins(game, fn);
        Check(bad == nullptr, "WaterInfoSkip: every layout read found in UpdateWaterInfoRendering (and its GetDynamicWaterMeshCenter / GetResource)");
        if (bad) printf("    not found: %s\n", bad);
        if (game.timeDateStamp == 0xF19077A4u)
            Check(fn - game.base == 0x42ED870, "WaterInfoSkip: UpdateWaterInfoRendering is build 19115871's exe+0x42ED870");
    } else {
        Check(false, "WaterInfoSkip: UpdateWaterInfoRendering's game-thread detour is in after the water-capture install");
    }
    const int fail = WaterInfoSkipTestLogic();
    Check(fail == 0, "WaterInfoSkip: same-input decision with fakes (incl. the forget on a change of value and the zone's location)");
    if (fail) printf("    failed checks (bits): 0x%X\n", fail);
}
