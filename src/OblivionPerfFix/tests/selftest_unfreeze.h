// UnfreezeClothSkip self-test (dlgedge, 2026-11-26; S-050), included by selftest.cpp. On the mapped exe (build 19115871): the
// freeze restore exe+0x4958950's walk gate (test byte [world+0x13C],4 at exe+0x4958D72), the walk's RecreateClothingActors call
// exe+0x4958E9B -> exe+0x36EB420, its simulation (+0xCA0) and the mesh's clothing asset count (+0x450); the install writes the
// entry jump and the gate call; then the decision and the watch with fakes.
#pragma once

namespace ufz {
int g_orig = 0;
alignas(16) uint8_t g_comp[2][0x1000];
alignas(16) uint8_t g_mesh[0x600];
alignas(16) uint8_t g_world[0x200];
alignas(16) uint8_t g_actor[2][0x300];
alignas(16) uint8_t g_level[0x100];
uint8_t* g_levelArr[1];
uint8_t* g_actors[4];
alignas(16) uint8_t g_owned[2][4][16];  // OwnedComponents elements {component, hash}
struct Arr { void* data; int32_t num, max; } g_levels{};
bool g_makeSim = false;
const void* g_dead = nullptr;
const void* g_garbage = nullptr;  // weak test null (g_dead) but the object still exists
bool FakeWeakExists(const uint64_t* w) { return g_garbage && reinterpret_cast<const void*>(*w) == g_garbage; }
alignas(16) uint8_t g_many[2100 * 8 + 0x1000];  // components many + 8 * i (their +0xCA0 inside the buffer)
void FakeRecreate(uint8_t* c)
{
    ++g_orig;
    if (g_makeSim) *reinterpret_cast<void**>(c + 0xCA0) = reinterpret_cast<void*>(0x1234);
}
uint8_t* g_meshOut = nullptr;
uint8_t* FakeGetMesh(uint8_t*) { return g_meshOut; }
void FakeMakeWeak(uint64_t* w, const void* o) { *w = reinterpret_cast<uint64_t>(o); }
uint8_t* FakeGetWeak(const uint64_t* w) { return reinterpret_cast<const void*>(*w) == g_dead ? nullptr : reinterpret_cast<uint8_t*>(*w); }
uint8_t* FakeGetLevels(uint8_t*) { return reinterpret_cast<uint8_t*>(&g_levels); }
// actor a owns component c (OwnedComponents at +0x218: data, bits inline +0x228, heap +0x238, bit count +0x240)
void Own(int a, int c, bool owns)
{
    uint8_t* const act = g_actor[a];
    memset(g_owned[a], 0, sizeof g_owned[a]);
    *reinterpret_cast<uint8_t**>(g_owned[a][1]) = owns ? g_comp[c] : reinterpret_cast<uint8_t*>(0x77);
    *reinterpret_cast<void**>(act + 0x218) = g_owned[a];
    *reinterpret_cast<uint32_t*>(act + 0x228) = 0x3;  // elements 0 and 1 allocated
    *reinterpret_cast<void**>(act + 0x238) = nullptr;
    *reinterpret_cast<int32_t*>(act + 0x240) = 2;
    *reinterpret_cast<uint8_t**>(g_comp[c] + 0x20) = act;  // Outer
}
void Own2(int a, int c0, int c1)  // actor a owns components c0 and c1
{
    Own(a, c1, true);
    *reinterpret_cast<uint8_t**>(g_owned[a][0]) = g_comp[c0];
    *reinterpret_cast<uint8_t**>(g_comp[c0] + 0x20) = g_actor[a];
}
void SetLevelActors(int n)  // the level's actor array: n of {actor 1, actor 0}
{
    g_actors[0] = g_actor[1];
    g_actors[1] = g_actor[0];
    *reinterpret_cast<uint8_t***>(g_level + 0x98) = g_actors;
    *reinterpret_cast<int32_t*>(g_level + 0xA0) = n;
    g_levelArr[0] = g_level;
    g_levels = Arr{g_levelArr, 1, 1};
}

void SelfTest(const GameModule& game)
{
    if (game.timeDateStamp != 0xF19077A4u) { Check(true, "unfreeze: unknown build, not checked"); return; }
    auto at = [&](const void* p, uint32_t rva) { return static_cast<const uint8_t*>(p) == game.base + rva; };
    UnfreezeClothSites s{};
    const char* why = "";
    const bool found = UnfreezeClothLocate(game, &s, &why);
    if (!found) printf("    unfreeze locate: %s\n", why);
    Check(found && at(s.gate, 0x4958D72) && at(s.walkCall, 0x4958E9B) && at(s.recreate, 0x36EB420) && at(s.getMesh, 0x3078E60) &&
              at(s.getLevels, 0xF7CE60) && at(s.restore, 0x4958950),
          "unfreeze: gate exe+0x4958D72, walk call exe+0x4958E9B -> RecreateClothingActors exe+0x36EB420, mesh getter, levels, restore");
    Check(found && at(s.makeWeak, 0x1125C20) && at(s.getWeak, 0x1126290) && at(s.getComps, 0x4946450) && s.ownedOff == 0x218,
          "unfreeze: the walk's weak pointer exe+0x1125C20 / Get exe+0x1126290, GetComponents exe+0x4946450, OwnedComponents +0x218");
    Check(found && s.bitOff == 0x13C && s.bitMask == 4 && s.simOff == 0xCA0 && s.numOff == 0x450 && s.actorsOff == 0x98 && s.actorNumOff == 0xA0,
          "unfreeze: world +0x13C & 4, simulation +0xCA0, clothing assets +0x450, level actors +0x98 / +0xA0");
    const bool inst = InstallUnfreezeClothSkip(true, 1, false);
    const uint8_t* const g = game.base + 0x4958D72;
    const uint8_t* const r = game.base + 0x36EB420;
    Check(inst && g[0] == 0xE8 && g[5] == 0x84 && g[6] == 0xC0 && g[7] == 0x0F && g[8] == 0x84 && r[0] == 0xE9,
          "unfreeze: installed (gate: call stub + test al,al before the game's je; entry: jmp)");
    const uint8_t* const gs = RipTarget(g, 1, 5);
    Check(inst && gs[0] == 0x48 && gs[1] == 0x8B && gs[2] == 0xC8 && gs[3] == 0xFF && gs[4] == 0x25,
          "unfreeze: gate stub = mov rcx,rax + jmp to the decision");
    const uint8_t* const rt = RipTarget(r, 1, 5);
    void* rh = nullptr;
    memcpy(&rh, rt + 6, 8);
    Check(inst && rt[0] == 0xFF && rt[1] == 0x25 && rh != nullptr, "unfreeze: entry stub jumps to the watch");
    Check(!UnfreezeClothLocate(game, &s, &why), "unfreeze: a second locate refuses the patched code");
    {   // MenuLoadPreload (dlgedge): the input-context function and its free, from the loop's pattern
        uint8_t *fn = nullptr, *fr = nullptr;
        Check(MenuLoadPreloadLocate(game, &fn, &fr) && at(fn, 0x491ED70) && at(fr, 0xE26500) && FindFNameCtor() != nullptr,
              "menu preload: input-context function exe+0x491ED70, FMemory::Free exe+0xE26500, FName's constructor found");
        const int steps = MenuLoadPreloadTestSteps();
        if (steps) printf("    menu preload steps: fail mask 0x%X\n", steps);
        Check(steps == 0, "menu preload: steps 0..3 from a loading screen's 30th frame, once per session; restart per screen; "
                          "classes wait for the FName pool; off / MenuLoadKeep not 1: nothing");
    }
    // decision, registry and the registry walk with fakes
    UnfreezeClothTestFns f{reinterpret_cast<void*>(&FakeRecreate), reinterpret_cast<void*>(&FakeGetMesh), reinterpret_cast<void*>(&FakeMakeWeak),
                           reinterpret_cast<void*>(&FakeGetWeak), reinterpret_cast<void*>(&FakeGetLevels), 0xCA0, 0x450, 0x13C, 0x98, 0x218, 4};
    f.weakExists = reinterpret_cast<void*>(&FakeWeakExists);
    UnfreezeClothTestSetup(f);
    memset(g_world, 0, sizeof g_world);
    memset(g_comp, 0, sizeof g_comp);
    memset(g_mesh, 0, sizeof g_mesh);
    Own(0, 0, true);
    Own(1, 1, true);
    SetLevelActors(2);
    UnfreezeClothTestMode(1);
    Check(!UnfreezeClothTestGate(g_world), "unfreeze: physics bit clear: no walk (the game's je)");
    g_world[0x13C] = 4;
    int o0 = g_orig;
    Check(!UnfreezeClothTestGate(g_world) && g_orig == o0, "unfreeze on, registry empty: walk left out, no call");
    UnfreezeClothTestMode(0);
    Check(UnfreezeClothTestGate(g_world), "unfreeze off: walk as in the game");
    UnfreezeClothTestMode(1);
    g_meshOut = nullptr;
    UnfreezeClothTestRecreate(g_comp[0]);
    g_meshOut = g_mesh;
    UnfreezeClothTestRecreate(g_comp[0]);
    Check(UnfreezeClothTestRegistry() == 0 && !UnfreezeClothTestSeen(), "unfreeze: calls with no mesh / a mesh without clothing assets: not registered");
    *reinterpret_cast<int32_t*>(g_mesh + 0x450) = 1;
    UnfreezeClothTestRecreate(g_comp[0]);
    UnfreezeClothTestRecreate(g_comp[0]);
    Check(UnfreezeClothTestRegistry() == 1 && UnfreezeClothTestSeen(), "unfreeze: a mesh with clothing assets: registered once");
    *reinterpret_cast<int32_t*>(g_mesh + 0x450) = 0;
    g_makeSim = true;
    UnfreezeClothTestRecreate(g_comp[1]);
    g_makeSim = false;
    Check(UnfreezeClothTestRegistry() == 2, "unfreeze: a simulation made by a call: registered");
    o0 = g_orig;
    uint64_t fb = UnfreezeClothTestFallbacks();
    Check(!UnfreezeClothTestGate(g_world) && g_orig == o0 + 2 && UnfreezeClothTestFallbacks() == fb,
          "unfreeze: both owners in the world's level: walk replaced by the 2 registered components' calls");
    SetLevelActors(1);  // actor 0 no longer in a level
    o0 = g_orig;
    Check(UnfreezeClothTestGate(g_world) && g_orig == o0 && UnfreezeClothTestFallbacks() == fb + 1,
          "unfreeze: an owner in none of the world's levels: the game's walk (fallback), no call of ours");
    SetLevelActors(2);
    Own(1, 1, false);
    Check(UnfreezeClothTestGate(g_world) && UnfreezeClothTestFallbacks() == fb + 2, "unfreeze: the Outer does not own the component: the game's walk");
    Own(1, 1, true);
    g_dead = g_actor[1];
    Check(UnfreezeClothTestGate(g_world) && UnfreezeClothTestFallbacks() == fb + 3, "unfreeze: a garbage owner: the game's walk");
    g_dead = g_comp[1];
    g_garbage = g_comp[1];  // review S2-2: garbage-flagged, not yet destroyed (the walk may still reach it)
    o0 = g_orig;
    Check(UnfreezeClothTestGate(g_world) && g_orig == o0 && UnfreezeClothTestFallbacks() == fb + 4 && UnfreezeClothTestRegistry() == 2,
          "unfreeze: a garbage component not yet destroyed: the game's walk, entry kept");
    g_garbage = nullptr;
    o0 = g_orig;
    Check(!UnfreezeClothTestGate(g_world) && g_orig == o0 + 1 && UnfreezeClothTestRegistry() == 1,
          "unfreeze: a dead component dropped from the registry, the live one called");
    g_dead = nullptr;
    // review S3-6: two registered components of one actor (the hash's same-actor scan); a full registry
    Own2(1, 0, 1);
    UnfreezeClothTestRecreate(g_comp[1]);  // it still holds its simulation: registered again
    SetLevelActors(1);                     // only actor 1 in the level
    o0 = g_orig;
    fb = UnfreezeClothTestFallbacks();
    Check(UnfreezeClothTestRegistry() == 2 && !UnfreezeClothTestGate(g_world) && g_orig == o0 + 2 && UnfreezeClothTestFallbacks() == fb,
          "unfreeze: two registered components of one actor: both called, walk left out");
    *reinterpret_cast<int32_t*>(g_mesh + 0x450) = 1;
    g_meshOut = g_mesh;
    for (int i = 0; i < 2100; ++i) UnfreezeClothTestRecreate(g_many + 8 * i);
    o0 = g_orig;
    Check(UnfreezeClothTestRegistry() == 2048 && UnfreezeClothTestGate(g_world) && g_orig == o0 && UnfreezeClothTestFallbacks() == fb + 1,
          "unfreeze: registry full (an add lost): the game's walk");
    *reinterpret_cast<int32_t*>(g_mesh + 0x450) = 0;
    UnfreezeClothTestReset();
    Check(UnfreezeClothTestGate(g_world), "unfreeze: not installed: walk as in the game");
}
}  // namespace ufz
