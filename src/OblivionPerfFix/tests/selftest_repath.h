// RepathParallel self-test, included by selftest.cpp: every function, call target, vtable and layout pin found by pattern on the
// mapped exe (nothing installed; build 19115871's RVAs checked when it is that build), the input key on fake queries, the
// nav-edit counter and the link marker with fake originals (npcs/repath_parallel.cpp).
#pragma once

static void RepathRun(const GameModule& game)
{
    uint32_t r[21] = {};
    const char* why = "";
    const int ok = RepathParallelTestDiscover(game, r, &why);
    Check(ok == 1, "RepathParallel: every function, call target, vtable and layout pin found by pattern on the mapped exe");
    if (ok != 1) printf("    %s\n", why);
    if (ok == 1 && game.timeDateStamp == 0xF19077A4u) {
        static const uint32_t want[21] = {0x41B8970, 0x41CA0C0, 0x41D6490, 0x4175E90, 0x1126290, 0x416B770, 0x416B900, 0x417CED0, 0x41D6240, 0x41976C0,
                                          0xE26500,  0xE32300,  0x2DC0350, 0x41919F0, 0x2D65460, 0x1115A40, 0x41CA4A0, 0x79E7EE0, 0x4186270, 0x4186340, 0x79E4198};
        int bad = 0;
        for (int i = 0; i < 21; ++i)
            if (r[i] != want[i]) { ++bad; printf("    target %d: 0x%X, build 19115871 has 0x%X\n", i, r[i], want[i]); }
        Check(bad == 0, "RepathParallel: the 21 targets are build 19115871's (rev130d's table, the link filter and query filter vtables, the nav-link updates)");
    }
    Check(RepathParallelTestKey(), "RepathParallel: the input key compares every findPath input; the helpers' link filter keys as stock; a filter of another class is never keyed");
    Check(RepathParallelTestHooks(), "RepathParallel: nav-link updates bump the nav counter and call the original; the helpers' isLinkAllowed passes "
                                     "the stock answer and records each link once with its answer; a changing answer or too many links is not servable");
    // repath130l: a search that asked custom links is served only when every link it asked answers the same at serve time.
    Check(RepathParallelTestLinkServe(0) == 1, "RepathParallel: a search whose custom links answer the same at serve time is served");
    Check(RepathParallelTestLinkServe(1) == 1, "RepathParallel: a search one of whose custom links flipped between precompute and serve is declined (stock search)");
    Check(RepathParallelTestLinkServe(2) == 1, "RepathParallel: a flip of a custom link the search never asked does not decline it");
    Check(RepathParallelTestLinkServe(3) == 1, "RepathParallel: a search that asked no custom link is served whatever the links do");
    Check(RepathParallelTestLinkServe(4) == 1, "RepathParallel: a navmesh edit between precompute and serve declines it");
}
