// StreamOutEndPlaySlice and ResetLoadersNoFlush self-test (worker epicport, 2026-11-12), included by selftest.cpp.
// On the mapped exe (Steam 19115871): RemoveFromWorld's incremental unregister call exe+0x38698B9 (the game's
// IncrementalUnregisterComponents exe+0x329E1D0 as its target: nothing is patched in the test), the finish block's
// AActor::RouteEndPlay exe+0x2DC7830 with ULevel::Actors at +0x98/+0xA0; ResetLoaders exe+0x1080F90 with
// FlushAsyncLoading exe+0xFE6C60, FLinkerManager::ResetLoaders exe+0x10939E0, GetPackage exe+0x11169A0 and
// FindExistingLinkerForPackage exe+0x107AD80 (UPackage::LinkerLoad +0x70). Then the hooks' logic with fakes.
#pragma once

namespace epicport_test {
// a fake ULevel: InternalIndex at +0x0C, Actors (data, num) at +0x98 / +0xA0 like the game's
struct FakeLevel {
    uint8_t bytes[0x100];
    void** data() { return *reinterpret_cast<void***>(bytes + 0x98); }
    void set(void** d, int32_t n, int32_t objIndex)
    {
        memset(bytes, 0, sizeof bytes);
        *reinterpret_cast<void***>(bytes + 0x98) = d;
        *reinterpret_cast<int32_t*>(bytes + 0xA0) = n;
        *reinterpret_cast<int32_t*>(bytes + 0x0C) = objIndex;
    }
};
int g_unregLeft = 0;      // the game's walk: calls left before it reports "done"
int g_unregCalls = 0, g_unregMax = 0;
std::vector<std::pair<void*, int>> g_routed;  // (actor, reason)
bool Unreg(void*, int32_t n)
{
    ++g_unregCalls;
    if (n == INT32_MAX) { ++g_unregMax; g_unregLeft = 0; return true; }
    if (g_unregLeft > 0) { --g_unregLeft; return false; }
    return true;
}
void Route(void* actor, uint8_t reason) { g_routed.emplace_back(actor, reason); }

// ResetLoaders fakes: an "object" is a FakeObj; its package is ->pkg; a package's linker is ->linker
struct FakeObj { FakeObj* pkg; void* linker; };
int g_gameCalls = 0;
bool g_inFlight = false;
void GameReset(void*) { ++g_gameCalls; }
void* GetPackage(void* o) { return static_cast<FakeObj*>(o)->pkg; }
void* FindLinker(void* p) { return p ? static_cast<FakeObj*>(p)->linker : nullptr; }
bool IsAsync() { return g_inFlight; }
bool (*g_isAsyncPtr)() = &IsAsync;
}  // namespace epicport_test

static void EpicPortRun(const GameModule& game)
{
    using namespace epicport_test;
    if (game.timeDateStamp == 0xF19077A4u) {
        uint32_t rva[4] = {}, dataOff = 0, numOff = 0;
        const bool ok = StreamOutEndPlaySliceTestDiscover(game, rva, &dataOff, &numOff);
        Check(ok && rva[0] == 0x38698B9 && rva[1] == 0x329E1D0 && rva[2] == 0x2DC7830 && rva[3] == 0x329E1D0 &&
                  dataOff == 0x98 && numOff == 0xA0,
              "StreamOutEndPlaySlice: RemoveFromWorld's unregister call, IncrementalUnregisterComponents, RouteEndPlay and "
              "ULevel::Actors found on the mapped exe");
        if (!ok || rva[2] != 0x2DC7830)
            printf("    found %d: site %X unreg %X route %X current %X, Actors +%X/+%X\n", ok, rva[0], rva[1], rva[2], rva[3], dataOff, numOff);
        uint32_t r[5] = {}, linkerOff = 0;
        const bool ok2 = ResetLoadersNoFlushTestDiscover(game, r, &linkerOff);
        Check(ok2 && r[0] == 0x1080F90 && r[1] == 0xFE6C60 && r[2] == 0x10939E0 && r[3] == 0x11169A0 && r[4] == 0x107AD80 &&
                  linkerOff == 0x70,
              "ResetLoadersNoFlush: ResetLoaders, FlushAsyncLoading, FLinkerManager::ResetLoaders, GetPackage and the "
              "linker lookup (UPackage +0x70) found on the mapped exe");
        if (!ok2 || r[0] != 0x1080F90)
            printf("    found %d: %X %X %X %X %X, linker +%X\n", ok2, r[0], r[1], r[2], r[3], r[4], linkerOff);
    }

    // ---- StreamOutEndPlaySlice logic ----
    int a[7];
    void* actors[7] = {&a[0], &a[1], nullptr, &a[3], &a[4], &a[5], &a[6]};
    FakeLevel lv;
    lv.set(actors, 7, 42);
    void* const unreg = reinterpret_cast<void*>(&Unreg);
    void* const route = reinterpret_cast<void*>(&Route);
    // key 0: pure pass-through
    StreamOutEndPlaySliceTestSetup(unreg, route, 0x98, 0xA0, 0);
    g_unregLeft = 2; g_unregCalls = 0; g_routed.clear();
    bool r1 = StreamOutEndPlaySliceHook(&lv, 5), r2 = StreamOutEndPlaySliceHook(&lv, 5), r3 = StreamOutEndPlaySliceHook(&lv, 5);
    Check(!r1 && !r2 && r3 && g_unregCalls == 3 && g_routed.empty(), "StreamOutEndPlaySlice: key 0 passes every call to the game's walk");
    // key 2: walk first, then EndPlay two slots per call in array order (null slot skipped but counted), done after the last
    StreamOutEndPlaySliceTestSetup(unreg, route, 0x98, 0xA0, 2);
    g_unregLeft = 1; g_unregCalls = 0; g_routed.clear();
    std::vector<bool> res;
    for (int i = 0; i < 6; ++i) res.push_back(StreamOutEndPlaySliceHook(&lv, 5));
    const int walks6 = g_unregCalls;
    const size_t routed6 = g_routed.size();
    // calls: 1 walk false, 2 walk done -> phase (false), 3 [0,1], 4 [2(null),3], 5 [4,5], 6 [6] -> true
    const bool order = g_routed.size() >= 6 && g_routed[0].first == &a[0] && g_routed[1].first == &a[1] && g_routed[2].first == &a[3] &&
                       g_routed[3].first == &a[4] && g_routed[4].first == &a[5] && g_routed[5].first == &a[6];
    bool reasons = true;
    for (auto& p : g_routed) reasons = reasons && p.second == 3;
    Check(!res[0] && !res[1] && !res[2] && !res[3] && !res[4] && res[5] && walks6 == 2 && routed6 == 6 && order && reasons,
          "StreamOutEndPlaySlice: the walk first, then RouteEndPlay(RemovedFromWorld) per batch in array order, done after the last");
    // after done: a new removal of the same level starts with the game's walk again (then routes from slot 0)
    for (int i = 0; i < 2; ++i) res.push_back(StreamOutEndPlaySliceHook(&lv, 5));
    Check(!res[6] && !res[7] && g_unregCalls == 3 && g_routed.size() == 8 && g_routed[6].first == &a[0],
          "StreamOutEndPlaySlice: after the level is done the next call is the game's walk again (no state left)");
    // a blocking call in the middle: rest handed to the game (walk with MAX_int32), nothing routed by us
    StreamOutEndPlaySliceTestSetup(unreg, route, 0x98, 0xA0, 3);
    g_unregLeft = 0; g_unregCalls = 0; g_unregMax = 0; g_routed.clear();
    const bool b1 = StreamOutEndPlaySliceHook(&lv, 5);  // walk done -> phase
    const bool b2 = StreamOutEndPlaySliceHook(&lv, 5);  // [0,1,2]
    const bool b3 = StreamOutEndPlaySliceHook(&lv, INT32_MAX);
    Check(!b1 && !b2 && b3 && g_unregMax == 1 && g_routed.size() == 2,
          "StreamOutEndPlaySlice: a blocking call mid-way goes to the game's walk; the game's finish routes the rest");
    // key 1: one actor per batch; then the key switched off mid-way: done at once (the game's finish routes the rest),
    // no further walk, nothing routed by us
    StreamOutEndPlaySliceTestSetup(unreg, route, 0x98, 0xA0, 1);
    g_unregLeft = 0; g_unregCalls = 0; g_routed.clear();
    const bool d1 = StreamOutEndPlaySliceHook(&lv, 5);
    const bool d2 = StreamOutEndPlaySliceHook(&lv, 5);
    Check(!d1 && !d2 && g_routed.size() == 1, "StreamOutEndPlaySlice: one actor per batch with key 1");
    SetStreamOutEndPlaySlice(0);  // live switch (not installed in the test: only the key changes)
    const bool d3 = StreamOutEndPlaySliceHook(&lv, 5);
    Check(d3 && g_routed.size() == 1 && g_unregCalls == 1,
          "StreamOutEndPlaySlice: key switched off mid-way = done at once, the game's finish routes the rest");
    SetStreamOutEndPlaySlice(2);
    g_unregCalls = 0;
    const bool d4 = StreamOutEndPlaySliceHook(&lv, 5);  // a fresh removal: walk (done) -> phase
    Check(!d4 && g_unregCalls == 1, "StreamOutEndPlaySlice: after the switch a new removal starts with the walk");
    // another level while one is pending: the pending one is dropped, the new one starts with the game's walk
    FakeLevel lv2;
    void* actors2[2] = {&a[5], &a[6]};
    lv2.set(actors2, 2, 43);
    g_unregCalls = 0;
    const bool e1 = StreamOutEndPlaySliceHook(&lv2, 5);
    Check(!e1 && g_unregCalls == 1, "StreamOutEndPlaySlice: another level's call drops the pending state and goes to the walk");
    // the same address with another object index (a new level): dropped as well
    g_unregCalls = 0;
    lv2.set(actors2, 2, 44);
    const bool f1 = StreamOutEndPlaySliceHook(&lv2, 5);
    Check(!f1 && g_unregCalls == 1, "StreamOutEndPlaySlice: a new object at the same address is not taken for the pending level");
    // stale (not called for > 500 ms): dropped, walk again
    StreamOutEndPlaySliceTestAge(1000);
    g_unregCalls = 0;
    const bool g1 = StreamOutEndPlaySliceHook(&lv2, 5);
    Check(!g1 && g_unregCalls == 1, "StreamOutEndPlaySlice: a pending level not continued for 500 ms is dropped (the walk again)");
    // a level without actors: done when the walk is done
    FakeLevel lv0;
    lv0.set(actors, 0, 50);
    StreamOutEndPlaySliceTestSetup(unreg, route, 0x98, 0xA0, 4);
    g_unregLeft = 0; g_routed.clear();
    Check(StreamOutEndPlaySliceHook(&lv0, 5) && g_routed.empty(), "StreamOutEndPlaySlice: a level without actors is done with the walk");

    // ---- ResetLoadersNoFlush logic ----
    FakeObj pkgNo{nullptr, nullptr}, pkgLinker{nullptr, reinterpret_cast<void*>(0x1234)};
    FakeObj objNo{&pkgNo, nullptr}, objLinker{&pkgLinker, nullptr}, objNoPkg{nullptr, nullptr};
    uint64_t calls, skipped, linker;
    ResetLoadersNoFlushTestSetup(reinterpret_cast<void*>(&GameReset), reinterpret_cast<void*>(&GetPackage),
                                 reinterpret_cast<void*>(&FindLinker), reinterpret_cast<void*>(&g_isAsyncPtr), true);
    ResetLoadersNoFlushTestCounts(&calls, &skipped, &linker);
    g_gameCalls = 0; g_inFlight = true;
    ResetLoadersNoFlushHook(&objNo);
    ResetLoadersNoFlushHook(&objLinker);
    ResetLoadersNoFlushHook(&objNoPkg);
    ResetLoadersNoFlushHook(nullptr);
    ResetLoadersNoFlushTestCounts(&calls, &skipped, &linker);
    Check(g_gameCalls == 3 && calls == 4 && skipped == 1 && linker == 1,
          "ResetLoadersNoFlush: only a package without a linker skips the game's ResetLoaders (linker, no package, null = the game's)");
    ResetLoadersNoFlushTestSetup(reinterpret_cast<void*>(&GameReset), reinterpret_cast<void*>(&GetPackage),
                                 reinterpret_cast<void*>(&FindLinker), reinterpret_cast<void*>(&g_isAsyncPtr), false);
    g_gameCalls = 0;
    ResetLoadersNoFlushHook(&objNo);
    ResetLoadersNoFlushTestCounts(&calls, &skipped, &linker);
    Check(g_gameCalls == 1 && skipped == 0, "ResetLoadersNoFlush: key 0 runs the game's ResetLoaders for every call");
}
