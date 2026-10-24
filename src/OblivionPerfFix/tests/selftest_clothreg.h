// ClothRegisterLite self-test, included by selftest.cpp: the cloth component's sites on the mapped exe (native table slots,
// vtable slots, EquipBodyPart, FLODData::Add's AddRules call, the completion sites), the install, then lite builds and their
// identical completion against fakes. Lab builds also run the ClothRegisterSkip and ClothRegisterVerify checks.
#pragma once

// ClothRegisterSkip's fakes: cloth components (vtable at +0, class +0x10, name +0x18, outer +0x20, registered flag and proxy at the
// offsets the fix found), owner actors and their classes; a fake EquipBodyPart plays what the game does inside the call.
namespace clr {
alignas(16) uint8_t g_comp[16][0x1000];
alignas(16) uint8_t g_actor[8][0x40];
alignas(16) uint8_t g_class[8][0x40];
uint32_t g_proxyOff = 0, g_regOff = 0;
alignas(16) void* g_vt[8];
alignas(16) void* g_otherVt[8];
int g_builds = 0;
void* const kProxy = reinterpret_cast<void*>(0x1234);
void*& Proxy(uint8_t* c) { return *reinterpret_cast<void**>(c + g_proxyOff); }
// ClothRegisterLite: a build runs the constraint rules through the fix's AddRules hook and makes a new proxy object.
ClothRegisterInfoOut g_info{};
bool g_callRules = false;
int g_rules = 0;
int g_extraRebuilds = 0;
uintptr_t g_serial = 0;
using F8 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
// Identical completion: a fake FClothConstraints (0x1F8 bytes, evolution +0x1B0, offset +0x1D8, count +0x1DC, rule offsets
// +0x1E0.., counts +0x1EC..) per build, one fake evolution (X +0x48, InvM +0xB0, particle ranges +0xE0: the LOD is active).
// A build sets the start pose (X = build pose), "adds rules" (AddRules: a constraint object in slot +0x90, init range 0, one
// init), runs the constructor's two Update calls, then moves X to the second pose (the constructor's Update(0)).
alignas(16) uint8_t g_cc[16][0x200];
alignas(16) uint8_t g_evo[0x600];
alignas(16) float g_x[16 * 3];
alignas(16) float g_invm[16];
alignas(16) float g_pinvm[16 * 4];
int32_t g_ranges[1] = {16};
alignas(16) uint8_t g_fakeObj[0x100];  // a spring constraint object (slot +0x90) with empty arrays
int g_ccNext = 0, g_enables = 0, g_updates = 0, g_diffNext = 0;
float g_rulesSawX = 0, g_buildPose = 0;
bool g_spoilAfterBuild = false;
bool g_flipOffAfterA = false;  // review finding 1: the key switched off between the registration build and the Blueprint rebuild
int g_lods = 1, g_updPerBuild = 2;  // review finding 3: more LODs / Update calls than the old fixed records held (8 / 32)
uint8_t* g_lastCc = nullptr;
template <typename T> void Put(uint8_t* p, size_t off, T v) { memcpy(p + off, &v, sizeof v); }
void SetupEvo()
{
    Put(g_evo, 0x48, g_x); Put(g_evo, 0x50, int32_t{16}); Put(g_evo, 0x54, int32_t{16});
    Put(g_evo, 0xB0, g_invm); Put(g_evo, 0xB8, int32_t{16}); Put(g_evo, 0xBC, int32_t{16});
    Put(g_evo, 0xC8, g_pinvm); Put(g_evo, 0xD0, int32_t{16}); Put(g_evo, 0xD4, int32_t{16});
    Put(g_evo, 0xE0, g_ranges); Put(g_evo, 0xE8, int32_t{1}); Put(g_evo, 0xEC, int32_t{1});
    for (float& v : g_invm) v = 1.0f;
}
uint64_t FakeAddRules(uint64_t a1, uint64_t a2, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    ++g_rules;
    if (a1 > 0x10000 && a2 == 2) {  // a fake constraints object (not the bare pass-through test)
        auto* cc = reinterpret_cast<uint8_t*>(a1);
        g_rulesSawX = g_x[0];
        Put(cc, 0x90, static_cast<void*>(g_fakeObj));
        Put(cc, 0x1E0, int32_t{0});
        Put(cc, 0x1EC, int32_t{g_diffNext ? 2 : 1});
        g_diffNext = 0;
    }
    return 0;
}
void FakeCcEnable(uint8_t*, bool on) { if (on) ++g_enables; }
void FakeCcUpdate(uint8_t*, const void*, const void*, float, float) { ++g_updates; }
void FakeRecreate(uint8_t* c)
{
    ++g_builds;
    if (!g_callRules) {
        Proxy(c) = kProxy;
        return;
    }
    g_buildPose = static_cast<float>(1 + g_serial);
    for (float& v : g_x) v = g_buildPose;
    uint8_t* cc = nullptr;
    for (int lod = 0; lod < g_lods; ++lod) {
        cc = g_cc[g_ccNext++ & 15];
        memset(cc, 0, 0x200);
        Put(cc, 0x1B0, static_cast<void*>(g_evo));
        Put(cc, 0x1D8, int32_t{0});
        Put(cc, 0x1DC, int32_t{16});
        Put(cc, 0x1E0, int32_t{-1}); Put(cc, 0x1E4, int32_t{-1}); Put(cc, 0x1E8, int32_t{-1});
        reinterpret_cast<F8>(g_info.addRulesHook)(reinterpret_cast<uint64_t>(cc), 2, 3, 4, 5, 6, 7, 0);
    }
    g_lastCc = cc;
    using U = void (*)(uint8_t*, const void*, const void*, float, float);
    for (int u = 0; u < g_updPerBuild; ++u) reinterpret_cast<U>(g_info.ccUpdateHook)(cc, g_evo, g_evo, 1.0f, 1.0f);
    for (float& v : g_x) v = 1000.0f;  // the constructor's Update(0): the second start pose
    if (g_spoilAfterBuild) Put(cc, 0x1EC, int32_t{5});
    Proxy(c) = reinterpret_cast<void*>(0x50000 + 16 * ++g_serial);
}
void FakeExecRecreate(uint8_t* c, void*, void*) { FakeRecreate(c); }
void FakeExecSuspended(uint8_t* c, void*, void* r) { *static_cast<bool*>(r) = Proxy(c) == nullptr; }
void FakeExecEnabled(uint8_t* c, void*, void* r) { *static_cast<bool*>(r) = Proxy(c) != nullptr; }
bool FakeRequiresSync(uint8_t* c) { return Proxy(c) != nullptr; }
void* FakeCalcBounds(uint8_t*, void* out, const void*) { return out; }
struct Script { uint8_t* comp; bool rebuild, read, bounds, unregister; } g_s;
bool g_sync = false, g_susp = true, g_en = false;
void* g_proxyAfterReads = nullptr;
void FakeEquip(void*, void*, void*, void*)
{
    using Exec = void (*)(uint8_t*, void*, void*);
    Proxy(g_s.comp) = nullptr;
    g_s.comp[g_regOff] |= 1;
    reinterpret_cast<void (*)(uint8_t*)>(g_info.onRegisterHook)(g_s.comp);  // (a), inside the child actor's registration
    if (g_flipOffAfterA) SetClothRegisterLite(0);  // the ini monitor switching the key off in the middle of the call
    if (g_s.read) {
        g_sync = reinterpret_cast<bool (*)(uint8_t*)>(g_info.requiresSyncHook)(g_s.comp);
        reinterpret_cast<Exec>(g_info.execSuspendedHook)(g_s.comp, nullptr, &g_susp);
        reinterpret_cast<Exec>(g_info.execEnabledHook)(g_s.comp, nullptr, &g_en);
        g_proxyAfterReads = Proxy(g_s.comp);
    }
    if (g_s.bounds) {
        alignas(16) uint8_t out[64];
        reinterpret_cast<void* (*)(uint8_t*, void*, const void*)>(g_info.calcBoundsHook)(g_s.comp, out, nullptr);
    }
    if (g_s.unregister) {
        g_s.comp[g_regOff] &= ~1;
        Proxy(g_s.comp) = nullptr;
    }
    if (g_s.rebuild) reinterpret_cast<Exec>(g_info.execRecreateHook)(g_s.comp, nullptr, nullptr);  // the robe Blueprint's (b)
    for (int i = 0; g_s.rebuild && i < g_extraRebuilds; ++i)
        reinterpret_cast<void (*)(uint8_t*)>(g_info.setAssetHook)(g_s.comp);  // a further rebuild in the same call (SetSkinnedAsset)
}
// One EquipBodyPart call through the fix's hook; returns the builds it made.
int Dress(int comp, int cls, bool rebuild, bool read = false, bool bounds = false, bool unregister = false)
{
    uint8_t* c = g_comp[comp];
    *reinterpret_cast<void**>(c) = g_vt;
    *reinterpret_cast<uint8_t**>(c + 0x20) = g_actor[cls];
    *reinterpret_cast<uint64_t*>(c + 0x18) = 77;  // the component's FName
    *reinterpret_cast<uint8_t**>(g_actor[cls] + 0x10) = g_class[cls];
    *reinterpret_cast<int32_t*>(g_class[cls] + 0x0C) = 1000 + cls;
    g_s = Script{c, rebuild, read, bounds, unregister};
    g_proxyAfterReads = nullptr;
    const int before = g_builds;
    reinterpret_cast<void (*)(void*, void*, void*, void*)>(g_info.equipHook)(nullptr, nullptr, nullptr, nullptr);
    return g_builds - before;
}

inline void ClothRegSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    if (!Installed(FindClothRegisterSkip(), "cloth register: sites found")) return;
    ClothRegisterInfoOut& ci = g_info;
    ClothRegisterInfo(&ci);
    auto at = [&](const void* p, uint32_t rva) { return p == game.base + rva; };
    Check(at(ci.recreate, 0x53CD090) && at(ci.onRegisterCall, 0x53CCF61) && at(ci.setAssetCall, 0x53CE71E),
          "cloth register: Recreate exe+0x53CD090, OnRegister's call exe+0x53CCF61, SetSkinnedAsset's call exe+0x53CE71E");
    Check(at(ci.execRecreateSlot, 0x803F888) && at(ci.execSuspendedSlot, 0x803F878) && at(ci.execEnabledSlot, 0x803F868),
          "cloth register: native table slots exe+0x803F888 / 0x803F878 / 0x803F868");
    Check(at(ci.clothVtable, 0x803EBD0) && at(ci.requiresSyncSlot, 0x803EBD0 + 0x448) && at(ci.calcBoundsSlot, 0x803EBD0 + 0x568) &&
              at(*ci.requiresSyncSlot, 0x53CD370) && at(*ci.calcBoundsSlot, 0x53C7500),
          "cloth register: vtable exe+0x803EBD0, RequiresPreEndOfFrameSync exe+0x53CD370, CalcBounds exe+0x53C7500");
    Check(ci.proxyOff == 0x940 && ci.registeredOff == 0x8C, "cloth register: proxy +0x940, registered flag +0x8C");
    Check(at(ci.equip, 0x484CCD0) && ci.numEquipSlots == 2 && at(ci.equipCall, 0x4823DDD) &&
              ((at(ci.equipSlots[0], 0x7CDEF80 + 0xAD0) && at(ci.equipSlots[1], 0x7CED258 + 0xAD0)) ||
               (at(ci.equipSlots[1], 0x7CDEF80 + 0xAD0) && at(ci.equipSlots[0], 0x7CED258 + 0xAD0))),
          "cloth register: EquipBodyPart exe+0x484CCD0, 2 vtable slots, direct call exe+0x4823DDD");
    Check(InstallClothRegisterSkip(true), "cloth register: installed on the mapped exe");
    ClothRegisterInfo(&ci);
    Check(*ci.execRecreateSlot == ci.execRecreateHook && *ci.requiresSyncSlot == ci.requiresSyncHook &&
              *ci.calcBoundsSlot == ci.calcBoundsHook && *ci.equipSlots[0] == ci.equipHook && *ci.equipSlots[1] == ci.equipHook &&
              RipTarget(ci.onRegisterCall, 1, 5)[0] == 0xFF && RipTarget(ci.setAssetCall, 1, 5)[0] == 0xFF &&
              RipTarget(ci.equipCall, 1, 5)[0] == 0xFF,
          "cloth register: slots and calls point at the hooks");
    g_proxyOff = ci.proxyOff;
    g_regOff = ci.registeredOff;
    // ClothRegisterLite: FLODData::Add's AddRules call (build 19115871), then lite builds against the fakes.
    Check(at(ci.addRulesCall, 0x53A09EC) && RipTarget(ci.addRulesCall, 1, 5)[0] == 0xFF,
          "cloth lite: AddRules call exe+0x53A09EC found and redirected");
    Check(at(ci.ccEnable, 0x53ABC00) && at(ci.ccUpdateCall, 0x53C0651) && RipTarget(ci.ccUpdateCall, 1, 5)[0] == 0xFF,
          "cloth lite: completion sites: FClothConstraints::Enable exe+0x53ABC00, FLODData::Update's Update call exe+0x53C0651 redirected");
    Check(at(ci.onUnregisterSlot, 0x803EBD0 + 0x350) && *ci.onUnregisterSlot == ci.onUnregisterHook,
          "cloth lite: OnUnregister's vtable slot exe+0x803EF20 points at the hook (proxies dropped inside the call)");
    ClothRegisterTestSetup(reinterpret_cast<void*>(&FakeRecreate), reinterpret_cast<void*>(&FakeExecRecreate),
                           reinterpret_cast<void*>(&FakeExecSuspended), reinterpret_cast<void*>(&FakeExecEnabled),
                           reinterpret_cast<void*>(&FakeRequiresSync), reinterpret_cast<void*>(&FakeCalcBounds),
                           reinterpret_cast<void*>(&FakeEquip), g_vt, g_proxyOff, g_regOff);
    ClothRegisterLiteTestSetup(reinterpret_cast<void*>(&FakeAddRules), reinterpret_cast<void*>(&FakeCcEnable),
                               reinterpret_cast<void*>(&FakeCcUpdate));
    SetupEvo();
    g_callRules = true;
    SetClothRegisterLite(true);
    const ClothRegisterStats l0 = GetClothRegisterStats();
    auto dressRules = [&](int comp, int cls, bool rebuild, bool bounds = false, bool unregister = false, int* rules = nullptr) {
        const int r0 = g_rules;
        const int b = Dress(comp, cls, rebuild, false, bounds, unregister);
        if (rules) *rules = g_rules - r0;
        return b;
    };
    int r = 0;
    Check(dressRules(0, 0, true, false, false, &r) == 2 && r == 2 && dressRules(14, 0, true, false, false, &r) == 2 && r == 2,
          "cloth lite: first two robes of a class built in full (learning)");
    Check(dressRules(1, 0, true, false, false, &r) == 2 && r == 1 && Proxy(g_comp[1]) != nullptr,
          "cloth lite: next robe of that class: (a) built without its rules, the Blueprint's (b) in full");
    ClothRegisterStats ls = GetClothRegisterStats();
    Check(ls.liteBuilds - l0.liteBuilds == 1 && ls.rulesLeftOut - l0.rulesLeftOut == 1 && ls.liteReplaced - l0.liteReplaced == 1 &&
              ls.liteLearning - l0.liteLearning == 2 && ls.lateBuilds == l0.lateBuilds,
          "cloth lite: counted (1 lite build, 1 rule set left out, replaced, 2 learning)");
    Check(dressRules(2, 0, true, true, false, &r) == 2 && r == 1 && GetClothRegisterStats().lateBuilds == l0.lateBuilds,
          "cloth lite: bounds read in between: no extra build (the lite proxy has the same bounds)");
    {
        const int e0 = g_enables, u0 = g_updates;
        const uint64_t c0 = GetClothRegisterStats().completed;
        const bool built = dressRules(3, 0, false, false, false, &r) == 1;
        const float pose = g_buildPose;
        Check(built && r == 1 && GetClothRegisterStats().liteFallbacks == l0.liteFallbacks &&
                  GetClothRegisterStats().completed == c0 + 1 && g_rulesSawX == pose && g_x[0] == 1000.0f && g_x[47] == 1000.0f,
              "cloth lite: not rebuilt in the call: completed in place (rules made once, from the build-time X, current X put back)");
        Check(g_enables - e0 == 1 && g_updates - u0 == 4 && [] { int32_t v; memcpy(&v, g_lastCc + 0x1EC, 4); return v; }() == 1,
              "cloth lite: completion enables the active LOD's constraints and replays the constructor's two Update calls (2 at the build on the empty constraints + 2 replayed)");
    }
    Check(dressRules(4, 0, true, false, false, &r) == 2 && r == 1, "cloth lite: after a completion the class stays lite");
    Check(dressRules(5, 1, false, false, false, &r) == 1 && r == 1 && dressRules(6, 1, true, false, false, &r) == 2 && r == 2,
          "cloth lite: not rebuilt while learning: the class stays in full");
    Check(dressRules(7, 2, true) == 2 && dressRules(15, 2, true) == 2 && dressRules(8, 2, false, false, true, &r) == 1 && r == 0 &&
              GetClothRegisterStats().liteFallbacks == l0.liteFallbacks,
          "cloth lite: unregistered inside the call: nothing rebuilt");
    const int r9 = g_rules;
    reinterpret_cast<F8>(ci.addRulesHook)(1, 2, 3, 4, 5, 6, 7, 0);
    Check(g_rules - r9 == 1, "cloth lite: AddRules outside a lite build passed through");
    g_extraRebuilds = 1;
    Check(dressRules(10, 3, true, false, false, &r) == 3 && r == 3 && dressRules(9, 3, true, false, false, &r) == 3 && r == 3 &&
              dressRules(11, 3, true, false, false, &r) == 3 && r == 1,
          "cloth lite: three builds in a call: the first two lite, the last in full");
    g_extraRebuilds = 0;
    const uint64_t fb = GetClothRegisterStats().liteFallbacks;
    Check(dressRules(12, 3, true, false, false, &r) == 2 && r == 1 && GetClothRegisterStats().liteFallbacks == fb,
          "cloth lite: fewer builds than learned: the last (lite) one completed in place at the call's end");
    g_spoilAfterBuild = true;
    Check(dressRules(13, 3, true, false, false, &r) == 3 && r == 1 && GetClothRegisterStats().liteFallbacks == fb + 1,
          "cloth lite: constraints no longer empty at the call's end: not completed, rebuilt in full (lite fallback)");
    g_spoilAfterBuild = false;
    g_extraRebuilds = 1;
    const bool a4 = dressRules(2, 4, true, false, false, &r) == 3 && r == 3;
    g_extraRebuilds = 0;
    const bool b4 = dressRules(4, 4, true, false, false, &r) == 2 && r == 2;
    g_extraRebuilds = 1;
    const uint64_t fb4 = GetClothRegisterStats().liteFallbacks;
    Check(a4 && b4 && dressRules(6, 4, true, false, false, &r) == 3 && r == 2 && GetClothRegisterStats().liteFallbacks == fb4,
          "cloth lite: learned with 3 then 2 builds: only the first build of a call lite (the fewer count), no fallback");
    g_extraRebuilds = 0;
    g_extraRebuilds = 1;
    SetClothRegisterLite(2);
    const bool l2 = dressRules(3, 2, true, false, false, &r) == 3 && r == 2;
    g_extraRebuilds = 0;
    Check(l2, "cloth lite mode 2: only the registration build lite (3 builds, 2 rule sets)");
    SetClothRegisterLite(3);
    g_extraRebuilds = 1;
    {
        const uint64_t c0 = GetClothRegisterStats().completed;
        Check(dressRules(5, 5, true, false, false, &r) == 3 && r == 1 && GetClothRegisterStats().completed == c0 + 1 &&
                  g_rulesSawX == g_buildPose,
              "cloth lite mode 3: every build lite, the last completed (one rule set per call, no learning)");
    }
    g_extraRebuilds = 0;
    Check(dressRules(7, 6, false, false, false, &r) == 1 && r == 1, "cloth lite mode 3: registration only: completed");
    {   // review finding 1: key off between (a) and (b): the entry that started lite stays lite, its last build is completed
        const ClothRegisterStats f0 = GetClothRegisterStats();
        g_flipOffAfterA = true;
        const bool b = dressRules(10, 6, true, false, false, &r) == 2 && r == 1;
        g_flipOffAfterA = false;
        const ClothRegisterStats f1 = GetClothRegisterStats();
        Check(b && f1.liteBuilds - f0.liteBuilds == 2 && f1.completed == f0.completed + 1 && f1.liteFallbacks == f0.liteFallbacks &&
                  f1.completeFail == f0.completeFail && Proxy(g_comp[10]) != nullptr,
              "cloth lite: key off between the registration build and the Blueprint rebuild: 2 lite builds, 1 rule set, completed, no fallback");
        Check(dressRules(11, 6, true, false, false, &r) == 2 && r == 2 && GetClothRegisterStats().liteBuilds == f1.liteBuilds,
              "cloth lite: after the switch-off the next call is built in full as in the game");
        SetClothRegisterLite(3);
    }
    {   // review finding 3: 12 LODs and 40 Update calls per build (beyond the old fixed 8 / 32): completed, no fallback
        const ClothRegisterStats m0 = GetClothRegisterStats();
        const int e0 = g_enables, u0 = g_updates;
        g_lods = 12;
        g_updPerBuild = 40;
        const bool b = dressRules(12, 6, true, false, false, &r) == 2 && r == 12;
        g_lods = 1;
        g_updPerBuild = 2;
        const ClothRegisterStats m1 = GetClothRegisterStats();
        Check(b && m1.completed == m0.completed + 1 && m1.liteFallbacks == m0.liteFallbacks && g_enables - e0 == 12 &&
                  g_updates - u0 == 3 * 40,
              "cloth lite: 12 LODs / 40 Update calls in a build: one rule set per LOD, all LODs enabled, every Update replayed, no fallback");
    }
    // The shipped mode (3) on the paths the learned mode's checks above cover.
    {
        const ClothRegisterStats b0 = GetClothRegisterStats();
        Check(dressRules(13, 7, true, true, false, &r) == 2 && r == 1 && GetClothRegisterStats().lateBuilds == b0.lateBuilds &&
                  GetClothRegisterStats().completed == b0.completed + 1,
              "cloth lite mode 3: bounds read between the builds: no extra build, the last build completed");
        g_spoilAfterBuild = true;
        const uint64_t fb3 = GetClothRegisterStats().liteFallbacks;
        const bool spoiled = dressRules(14, 7, false, false, false, &r) == 2 && r == 1;
        g_spoilAfterBuild = false;
        Check(spoiled && GetClothRegisterStats().liteFallbacks == fb3 + 1,
              "cloth lite mode 3: constraints no longer empty at the call's end: not completed, rebuilt in full (lite fallback)");
        const ClothRegisterStats u0 = GetClothRegisterStats();
        Check(dressRules(15, 7, false, false, true, &r) == 1 && r == 0 && GetClothRegisterStats().liteFallbacks == u0.liteFallbacks &&
                  GetClothRegisterStats().completed == u0.completed,
              "cloth lite mode 3: unregistered inside the call: nothing completed or rebuilt (as the game's OnUnregister leaves it)");
        const int r3 = g_rules;
        reinterpret_cast<F8>(ci.addRulesHook)(1, 2, 3, 4, 5, 6, 7, 0);
        Check(g_rules - r3 == 1, "cloth lite mode 3: AddRules outside a lite build passed through");
    }
    SetClothRegisterLite(1);
    SetClothRegisterLite(0);
    Check(dressRules(9, 2, true, false, false, &r) == 2 && r == 2, "cloth lite off: built in full as in the game");
    g_callRules = false;
}

}  // namespace clr
