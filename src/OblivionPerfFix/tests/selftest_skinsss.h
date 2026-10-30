// SeparableSkinAbove4K self-test, included by selftest.cpp: GConsoleManager found on the mapped exe, then the switch
// logic against a fake r.SSS.Burley.Quality (its two copies), a fake r.ScreenPercentage and a fake window width (the
// console variables and the game window do not exist offline).
#pragma once

namespace skinsss {
void Run(const GameModule& game)
{
    Check(reinterpret_cast<uint8_t*>(SeparableSkinConsoleManager()) == game.base + 0x9007F28,
          "SeparableSkinAbove4K: GConsoleManager at exe+0x9007F28 (from the game's r.HDR.UI.Luminance lookups)");
    int32_t v[2] = {1, 1};
    float pct = 100.f;
    int win = 5120;
    SeparableSkinTestSetup(v, &pct, &win);
    SetSeparableSkinAbove4K(true);
    Check(v[0] == 0 && v[1] == 0, "SeparableSkinAbove4K: 5120 wide at 100 % (5120): both copies of r.SSS.Burley.Quality become 0");
    pct = 66.7f;
    SetSeparableSkinAbove4K(true);
    Check(v[0] == 1 && v[1] == 1, "SeparableSkinAbove4K: 5120 at 66.7 % (3415): the replaced value (1) is back");
    pct = 85.f;
    SetSeparableSkinAbove4K(true);
    Check(v[0] == 0, "SeparableSkinAbove4K: 5120 at 85 % (4352): separable");
    win = 3840;
    pct = 100.f;
    SetSeparableSkinAbove4K(true);
    Check(v[0] == 1, "SeparableSkinAbove4K: a 4K screen at 100 % (3840): Burley");
    win = 5120;
    SetSeparableSkinAbove4K(true);
    SetSeparableSkinAbove4K(false);
    Check(v[0] == 1 && v[1] == 1, "SeparableSkinAbove4K: switched off while wide: the replaced value is back");
    SetSeparableSkinAbove4K(false);
    Check(v[0] == 1, "SeparableSkinAbove4K: off: nothing written");
    int32_t w[2] = {0, 0};  // a 0 from elsewhere (an Engine.ini line)
    SeparableSkinTestSetup(w, &pct, &win);
    SetSeparableSkinAbove4K(true);
    pct = 50.f;
    SetSeparableSkinAbove4K(true);
    Check(w[0] == 0 && w[1] == 0, "SeparableSkinAbove4K: a 0 it did not write is left alone when narrow");
    int32_t z[2] = {2, 2};
    pct = 100.f;
    SeparableSkinTestSetup(z, &pct, &win);
    SetSeparableSkinAbove4K(true);
    pct = 70.f;
    SetSeparableSkinAbove4K(true);
    Check(z[0] == 2 && z[1] == 2, "SeparableSkinAbove4K: another value (2) is put back as it was");
    SeparableSkinTestSetup(nullptr, nullptr, nullptr);
}
}  // namespace skinsss
