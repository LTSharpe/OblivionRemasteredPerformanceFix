// ClothBuildDefer self-test, included by selftest.cpp and run after ClothRegisterLite's (main.cpp installs its hooks first
// and this file's then run inside them): the sites in the mapped exe (build 19115871), then the rule with forced "far
// unit" answers and fakes for RecreateClothSimulationProxy, OnUnregister and IsSimulationSuspended.
#pragma once

#include <thread>

namespace cdf {

inline void ClothDeferSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    Check(!InstallClothBuildDefer(false), "clothdefer: refuses late installation");
    // The refused path: with RecreateClothSimulationProxy's first byte changed in this mapped copy the installation stops
    // at its checks, writes nothing (the entry, both Blueprint readers and OnUnregister's slot as before), one log line
    {
        uint8_t* const entry = game.base + 0x53CD090;
        uint8_t* const susp = game.base + 0x53D00CE;
        uint8_t* const enabled = game.base + 0x53CFFC0;
        void** const slot = reinterpret_cast<void**>(game.base + 0x803EF20);
        uint8_t entryBefore[6], suspBefore[5], enabledBefore[5];
        memcpy(entryBefore, entry, sizeof entryBefore);
        memcpy(suspBefore, susp, sizeof suspBefore);
        memcpy(enabledBefore, enabled, sizeof enabledBefore);
        void* const slotBefore = *slot;
        const uint8_t first[1] = {entry[0]}, changed[1] = {static_cast<uint8_t>(entry[0] ^ 0x01)};
        const bool setUp = WriteCode(entry, changed);
        Check(setUp, "clothdefer: test set-up, RecreateClothSimulationProxy's first byte changed");
        if (setUp) {
            const bool refused = !InstallClothBuildDefer(true);
            WriteCode(entry, first);
            Check(refused && memcmp(entry, entryBefore, sizeof entryBefore) == 0 && memcmp(susp, suspBefore, sizeof suspBefore) == 0 &&
                      memcmp(enabled, enabledBefore, sizeof enabledBefore) == 0 && *slot == slotBefore &&
                      LogContains("[clothdefer] RecreateClothSimulationProxy not found exactly once: ClothBuildDefer not installed"),
                  "clothdefer: a changed RecreateClothSimulationProxy: not installed, nothing written, one log line");
        }
    }
    if (!Installed(InstallClothBuildDefer(true), "ClothBuildDefer (RecreateClothSimulationProxy, OnUnregister, the Blueprint readers)"))
        return;
    Check(ClothDeferTestRecreate() == game.base + 0x53CD090, "clothdefer: RecreateClothSimulationProxy exe+0x53CD090");
    void** unregSlot = nullptr;
    void *suspCall = nullptr, *enabledThunk = nullptr;
    Check(ClothDeferTestSites(&unregSlot, &suspCall, &enabledThunk), "clothdefer: OnUnregister slot and both Blueprint readers found");
    Check(reinterpret_cast<uint8_t*>(unregSlot) == game.base + 0x803EF20, "clothdefer: OnUnregister's vtable slot exe+0x803EF20");
    ClothRegisterInfoOut cri{};
    if (ClothRegisterInfo(&cri) && cri.addRulesCall)
        Check(*unregSlot != cri.onUnregisterHook && ClothDeferTestUnregisterNext() == cri.onUnregisterHook,
              "clothdefer: OnUnregister's hook goes in front of ClothRegisterLite's (installed before it, as in the game)");
    Check(suspCall == game.base + 0x53D00CE && enabledThunk == game.base + 0x53CFFC0,
          "clothdefer: execIsSimulationSuspended's call exe+0x53D00CE, execIsSimulationEnabled exe+0x53CFFC0");

    static uint8_t proxy[0x40]{};
    static int builds = 0, unregs = 0, suspends = 0;
    auto fakeRecreate = +[](uint8_t* c) {   // what a build writes into the component: the proxy pointer
        ++builds;
        *reinterpret_cast<uint8_t**>(c + 0x940) = proxy;
    };
    auto fakeUnreg = +[](uint8_t*) { ++unregs; };
    auto fakeSusp = +[](uint8_t* c) -> bool { ++suspends; return *reinterpret_cast<uint8_t**>(c + 0x940) == nullptr; };
    ClothDeferTestSetFakes(reinterpret_cast<void*>(+fakeRecreate), reinterpret_cast<void*>(+fakeUnreg), reinterpret_cast<void*>(+fakeSusp));
    static uint8_t c3[0x950]{};
    auto proxyOf = [] { return *reinterpret_cast<uint8_t**>(c3 + 0x940); };
    auto clear = [] { *reinterpret_cast<uint8_t**>(c3 + 0x940) = nullptr; };
    c3[0x8C] = 1;
    SetClothBuildDefer(true, 3);
    ClothDeferTestSetDistant(1, 100);
    ClothDeferTestCallRecreate(c3);
    Check(builds == 0 && ClothDeferTestPending() == 1, "clothdefer: far unit, no proxy yet: the build waits");
    ClothDeferTestSetDistant(1, 101);
    ClothDeferTestCallRecreate(c3);
    Check(builds == 0 && ClothDeferTestPending() == 1, "clothdefer: a second far request merges into it");
    Check(ClothDeferFrame(102, false, 0, 4000, false) == 0 && builds == 0, "clothdefer: 1 frame after the last request: still waits");
    ClothDeferFrame(104, false, 0, 4000, false);
    Check(builds == 1 && ClothDeferTestPending() == 0 && proxyOf() == proxy, "clothdefer: 3 quiet frames: built once");
    ClothDeferTestSetDistant(1, 105);
    ClothDeferTestCallRecreate(c3);
    Check(builds == 2 && ClothDeferTestPending() == 0, "clothdefer: a component with a proxy builds at once");
    clear();
    ClothDeferTestCallRecreate(c3);
    ClothDeferTestSetDistant(0, 106);
    ClothDeferTestCallRecreate(c3);
    Check(builds == 3 && ClothDeferTestPending() == 0, "clothdefer: asked for again outside a far unit: built then");
    clear();
    ClothDeferTestSetDistant(1, 107);
    ClothDeferTestCallRecreate(c3);
    ClothDeferTestCallUnregister(c3);
    ClothDeferFrame(120, false, 0, 4000, false);
    Check(unregs == 1 && ClothDeferTestPending() == 0 && builds == 3, "clothdefer: OnUnregister drops the waiting build");
    ClothDeferTestSetDistant(1, 130);
    ClothDeferTestCallRecreate(c3);
    ClothDeferFrame(140, false, 5000, 4000, true);
    Check(builds == 3 && ClothDeferTestPending() == 1, "clothdefer: quiet, but the frame's budget is spent: waits");
    ClothDeferTestSetDistant(0, 141);
    ClothDeferFrame(141, false, 5000, 4000, true);
    Check(builds == 4 && ClothDeferTestPending() == 0, "clothdefer: the actor near or seen: built whatever the budget");
    clear();
    ClothDeferTestSetDistant(1, 150);
    ClothDeferTestCallRecreate(c3);
    ClothDeferTestSetDistant(1, 179);
    ClothDeferTestCallRecreate(c3);   // keeps it from being quiet
    ClothDeferFrame(179, false, 9000, 4000, true);
    Check(builds == 4, "clothdefer: 29 frames, not quiet: waits");
    int built = -1;
    ClothDeferFrame(180, false, 9000, 4000, true, &built);
    Check(builds == 5 && ClothDeferTestPending() == 0 && built == 1,
          "clothdefer: 30 frames after the first request: due, built in the grown budget, counted as a unit");
    clear();
    ClothDeferTestSetDistant(1, 400);
    ClothDeferTestCallRecreate(c3);
    ClothDeferTestSetDistant(1, 459);
    ClothDeferTestCallRecreate(c3);
    ClothDeferFrame(459, false, 100000, 4000, true);
    Check(builds == 5, "clothdefer: 59 frames, a frame over any budget: waits");
    ClothDeferFrame(460, false, 100000, 4000, true, &built);
    Check(builds == 6 && ClothDeferTestPending() == 0 && built == 1, "clothdefer: 60 frames: built whatever the budget");
    clear();
    ClothDeferTestSetDistant(1, 200);
    ClothDeferTestCallRecreate(c3);
    const bool suspended = ClothDeferTestCallSuspended(c3);
    Check(builds == 7 && suspends == 1 && !suspended && ClothDeferTestPending() == 0,
          "clothdefer: a Blueprint's IsSimulationSuspended: the build is made first, the stock answer");
    clear();
    c3[0x8C] = 0;
    ClothDeferTestCallRecreate(c3);
    Check(builds == 8 && ClothDeferTestPending() == 0, "clothdefer: an unregistered component is not deferred");
    c3[0x8C] = 1;
    clear();
    ClothDeferTestCallRecreate(c3);
    ClothDeferFrame(201, true, 0, 4000, false);
    Check(builds == 9 && ClothDeferTestPending() == 0, "clothdefer: a frame that does not defer builds everything waiting");
    clear();
    ClothDeferTestCallRecreate(c3);
    c3[0] ^= 1;   // another object at that address
    ClothDeferFrame(300, true, 0, 4000, false);
    Check(builds == 9 && ClothDeferTestPending() == 0, "clothdefer: a component that is no longer the same object is not built");
    c3[0] ^= 1;
    std::thread other([] { ClothDeferTestCallRecreate(c3); });
    other.join();
    Check(builds == 10 && ClothDeferTestPending() == 0, "clothdefer: requests from other threads are never deferred");
    clear();
    ClothDeferTestCallRecreate(c3);
    SetClothBuildDefer(false, 3);
    ClothDeferFrame(310, false, 0, 4000, true);
    Check(builds == 11 && ClothDeferTestPending() == 0, "clothdefer: switched off: what waits is built at the next frame");
    ClothDeferTestCallRecreate(c3);
    Check(builds == 12 && ClothDeferTestPending() == 0, "clothdefer: switched off: every request builds at once");

    // Bounds after a held build (pr31fix): the registration cached an empty box without a proxy; right after the held
    // build is made the cache flags are cleared, the game's CalcBounds runs with the identity transform (GetLocalBounds:
    // fills the cache from the new proxy) and then MarkRenderTransformDirty (the renderer takes them at the frame's end)
    void *calcSite = nullptr, *markSite = nullptr, *vtable = nullptr;
    Check(ClothDeferTestBoundsSites(&calcSite, &markSite, &vtable) && calcSite == game.base + 0x53C7500 &&
              markSite == game.base + 0x2FE7B70 && vtable == game.base + 0x803EF20 - 0x350,
          "clothdefer: bounds sites: CalcBounds exe+0x53C7500 (behind ClothRegisterLite's hook), MarkRenderTransformDirty "
          "exe+0x2FE7B70, the class vtable exe+0x803EBD0");
    static int calcs = 0, marks = 0;
    static bool calcOk = true, markAfterCalc = true;
    static bool noProxy = false;
    auto fakeRecreate2 = +[](uint8_t* c) {
        ++builds;
        *reinterpret_cast<uint8_t**>(c + 0x940) = noProxy ? nullptr : proxy;
    };
    auto fakeCalc = +[](uint8_t* c, void* out, const void* l2w) -> void* {
        static const double id[12] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0};
        ++calcs;
        // as the game's CalcBounds finds it: both cache flags clear (so it reads the proxy), a proxy, the identity
        calcOk = calcOk && !(c[0x800] & 0x80) && !(c[0x801] & 1) && *reinterpret_cast<uint8_t**>(c + 0x940) == proxy &&
                 memcmp(l2w, id, sizeof id) == 0 && marks == calcs - 1;
        c[0x800] |= 0x80;   // cached in local space
        const double box[7] = {0, 0, 10, 40, 40, 90, 105};
        memcpy(c + 0x840, box, sizeof box);
        return out;
    };
    auto fakeMark = +[](uint8_t*) {
        ++marks;
        markAfterCalc = markAfterCalc && calcs == marks;
    };
    ClothDeferTestSetFakes(reinterpret_cast<void*>(+fakeRecreate2), reinterpret_cast<void*>(+fakeUnreg), reinterpret_cast<void*>(+fakeSusp));
    ClothDeferTestSetBoundsFakes(reinterpret_cast<void*>(+fakeCalc), reinterpret_cast<void*>(+fakeMark));
    auto registration = [&] {   // what the registration leaves without a proxy: an empty box, cached
        clear();
        memset(c3 + 0x840, 0, 56);
        c3[0x800] |= 0x80;
    };
    auto extent = [] { double e[3]; memcpy(e, c3 + 0x858, sizeof e); return e[0] == 40 && e[1] == 40 && e[2] == 90; };
    memcpy(c3, &vtable, sizeof vtable);   // a component of the cloth class
    SetClothBuildDefer(true, 3);
    registration();
    ClothDeferTestSetDistant(1, 500);
    ClothDeferTestCallRecreate(c3);
    Check(builds == 12 && calcs == 0 && marks == 0, "clothdefer: bounds: nothing done while the build waits");
    ClothDeferFrame(503, false, 0, 4000, false);
    Check(builds == 13 && calcs == 1 && marks == 1 && calcOk && markAfterCalc && extent() && (c3[0x800] & 0x80),
          "clothdefer: bounds: after the held build: flags cleared, CalcBounds(identity) from the new proxy, then "
          "MarkRenderTransformDirty; the cache holds the proxy's box");
    ClothDeferTestSetDistant(1, 504);
    ClothDeferTestCallRecreate(c3);
    Check(builds == 14 && calcs == 1 && marks == 1, "clothdefer: bounds: a later build of a component with a proxy: left to the game");
    registration();
    ClothDeferTestSetDistant(1, 510);
    ClothDeferTestCallRecreate(c3);
    ClothDeferTestSetDistant(0, 511);
    ClothDeferTestCallRecreate(c3);
    Check(builds == 15 && calcs == 2 && marks == 2 && calcOk && markAfterCalc && extent(),
          "clothdefer: bounds: a held build asked for elsewhere: built, then the same refresh");
    registration();
    ClothDeferTestCallRecreate(c3);
    Check(builds == 16 && calcs == 2 && marks == 2, "clothdefer: bounds: a first build that was never held: left to the game");
    registration();
    noProxy = true;
    ClothDeferTestSetDistant(1, 520);
    ClothDeferTestCallRecreate(c3);
    ClothDeferFrame(523, false, 0, 4000, false);
    Check(builds == 17 && calcs == 2 && marks == 2 && (c3[0x800] & 0x80),
          "clothdefer: bounds: a held build that makes no proxy: nothing to read, the cache as the game left it");
    noProxy = false;
    registration();
    const uint8_t otherVt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    memcpy(c3, otherVt, sizeof otherVt);
    ClothDeferTestSetDistant(1, 530);
    ClothDeferTestCallRecreate(c3);
    ClothDeferFrame(533, false, 0, 4000, false);
    Check(builds == 18 && calcs == 2 && marks == 2, "clothdefer: bounds: a component of another class: left as it is");
    memcpy(c3, &vtable, sizeof vtable);
    memset(c3, 0, 8);
    SetClothBuildDefer(false, 3);
    ClothDeferTestSetDistant(-1, 0);
}

}  // namespace cdf
