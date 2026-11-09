// PairReleaseOnce self-test (public; lab PairRaceProbe parts in the lab build), included by selftest.cpp: discovery on the mapped exe,
// the fix's writes and counting stubs on the mapped image; lab: the probe's logic with a fake pool (the early release is the game's
// own exe+0x4596FB0).
#pragma once

static void PairRaceRun(const GameModule& game)
{
    PairRaceDiscovery d{};
    const char* missing = nullptr;
    const bool found = PairRaceDiscover(game, &d, &missing);
    Check(found, "PairRace: discovery on the mapped exe");
    if (!found) {
        printf("  PairRace: first mismatch: %s\n", missing ? missing : "?");
        return;
    }
    if (game.timeDateStamp == 0xF19077A4u) {
        Check(d.first - game.base == 0x4596FB0 && d.dtor - game.base == 0x458EF50 && d.drain - game.base == 0x4596CE0 && d.erase - game.base == 0x4596C30,
              "PairRace: early release, destructor, drain, erase (build 19115871)");
        Check(d.runCall - game.base == 0x45910F0, "PairRace: RunSession's early release at exe+0x45910F0 (build 19115871)");
    }
    const int fix = PairReleaseOnceTestInstall();
    if (fix) printf("  PairReleaseOnce: check %d failed\n", fix);
    Check(fix == 0, "PairReleaseOnce: both early releases go to counting stubs, the destructor's kept, the stubs count only a session "
                    "with a ring, a patched image does not match again");
}
