// EquipRefreshOneSpawn self-test (povkeep, 2026-11-25; S-050), included by selftest.cpp after ClothRegisterLite's (whose
// ClothRegisterLiteRefresh install has redirected the refresh's two callers by then: both keys on). On the mapped exe (build
// 19115871): the refresh, its SetChildActorClass call, the CreateChildActor vtable slot after it and inside SetChildActorClass,
// the component offsets; the real install redirects only that call and keeps ClothRegisterLiteRefresh's caller redirects;
// then the hook against a fake SetChildActorClass on fake objects (the skip guard, the key off).
// EquipRefreshKeepActor (clothkeep, 2026-11-26): installed in the same call (keep 1): SwitchPOV exe+0x4899380 found by its
// body, its first 10 bytes relocated to the trampoline and a jmp written; the refresh's `call [rax+0x608]` at exe+0x48301FE
// replaced by a call to a stub + nop; the decision with fakes inside / outside the view-switch scope and the CreateChildActor
// hook against a fake vtable (skipped only once, only right after a keep, only for that component).
#pragma once

namespace ers {
int g_calls = 0, g_creates = 0;
void FakeSetClass(uint8_t*, void*, void*) { ++g_calls; }
void FakeCreate(uint8_t*, void*) { ++g_creates; }
int g_switchCalls = 0;
bool g_switchScope = false;
uint64_t FakeSwitch(uint8_t*, uint64_t, uint64_t, uint64_t) { ++g_switchCalls; g_switchScope = EquipRefreshKeepInViewSwitch(); return 7; }

void SelfTest(const GameModule& game)
{
    if (game.timeDateStamp != 0xF19077A4u) { Check(true, "one spawn: unknown build, not checked"); return; }
    auto at = [&](const void* p, uint32_t rva) { return static_cast<const uint8_t*>(p) == game.base + rva; };
    EquipOneSpawnSites s{};
    const char* why = "";
    const bool found = EquipRefreshOneSpawnLocate(game, &s, &why);
    if (!found) printf("    one spawn locate: %s\n", why);
    Check(found && at(s.refresh, 0x4830170) && at(s.call, 0x48301DC) && at(s.setClass, 0x3043D90),
          "one spawn: refresh exe+0x4830170, its SetChildActorClass call exe+0x48301DC -> exe+0x3043D90");
    Check(found && s.vslot == 0x608 && s.classOff == 0x2C0 && s.childOff == 0x2C8 && s.templateOff == 0x2D0,
          "one spawn: CreateChildActor slot +0x608 after the call and inside SetChildActorClass; class +0x2C0, child +0x2C8, template +0x2D0");
    ClothRegisterInfoOut ci{};
    ClothRegisterInfo(&ci);
    const bool clothBefore = ci.numRefreshCalls == 2;
    const uint8_t* const swAt = game.base + 0x4899380;
    uint8_t swBefore[10];
    memcpy(swBefore, swAt, 10);
    const bool vcallBefore = memcmp(game.base + 0x48301FE, "\xFF\x90\x08\x06\x00\x00", 6) == 0;
    const bool inst = InstallEquipRefreshOneSpawn(true, 1, 1, false);
    const uint8_t* const st = RipTarget(game.base + 0x48301DC, 1, 5);
    void* t = nullptr;
    int32_t z = -1;
    memcpy(&z, st + 2, 4);
    if (st[0] == 0xFF && st[1] == 0x25 && z == 0) memcpy(&t, st + 6, 8);
    Check(inst && t == EquipRefreshOneSpawnTestHook() && at(EquipRefreshOneSpawnSetClass(), 0x3043D90),
          "one spawn: installed, the call goes through a stub to the hook, the original target kept");
    ClothRegisterInfo(&ci);
    Check(clothBefore && ci.numRefreshCalls == 2 && RipTarget(ci.refreshCalls[0], 1, 5)[0] == 0xFF && RipTarget(ci.refreshCalls[1], 1, 5)[0] == 0xFF,
          "one spawn with ClothRegisterLiteRefresh on: the refresh's 2 callers stay redirected to the cloth window");
    Check(!EquipRefreshOneSpawnLocate(game, &s, &why), "one spawn: a second locate refuses the redirected call (no double install)");
    // EquipRefreshKeepActor's two patches
    const uint8_t* const tr = EquipRefreshKeepTestSwitchTrampoline();
    Check(EquipRefreshPovHooked() && swAt[0] == 0xE9 && tr && !memcmp(tr, swBefore, 10) && tr[10] == 0xFF && tr[11] == 0x25,
          "keep: SwitchPOV exe+0x4899380 jumps to the hook, its first 10 bytes relocated to the trampoline (+ jmp back)");
    void* back = nullptr;
    if (tr) memcpy(&back, tr + 16, 8);
    Check(back == swAt + 10, "keep: the trampoline jumps back to SwitchPOV+0x0A");
    Check(EquipRefreshKeepTestViewOff() == 0x97A && EquipRefreshKeepTestSkipOff() != 0 && EquipRefreshKeepTestSkipOff() < 0x2000,
          "keep: SwitchPOV's view byte +0x97A and skip flag read from its code");
    {  // review S1-1: the scope is entered only on a real view change (SwitchHook against a fake trampoline)
        using SwFn = uint64_t (*)(uint8_t*, uint64_t, uint64_t, uint64_t);
        const SwFn sh = reinterpret_cast<SwFn>(EquipRefreshKeepTestSwitchHook());
        void* const prevSw = EquipRefreshKeepTestSwitchOrig(reinterpret_cast<void*>(&FakeSwitch));
        alignas(16) static uint8_t ctrl[0x1000];
        memset(ctrl, 0, sizeof ctrl);
        const uint32_t vo = EquipRefreshKeepTestViewOff(), so = EquipRefreshKeepTestSkipOff();
        auto sw = [&](uint8_t cur, uint8_t skip, uint64_t pov, uint64_t force) {
            ctrl[vo] = cur; ctrl[so] = skip; g_switchCalls = 0; g_switchScope = false;
            const uint64_t r = sh(ctrl, pov, force, 0);
            return r == 7 && g_switchCalls == 1;
        };
        Check(sw(1, 0, 0, 0) && g_switchScope && !EquipRefreshKeepInViewSwitch(),
              "keep: SwitchPOV 1 -> 0 (a conversation start in third person) is in the keep's scope; the scope ends with the call");
        Check(sw(0, 0, 1, 0) && g_switchScope, "keep: SwitchPOV 0 -> 1 (the conversation's end, the toggle) is in scope");
        Check(sw(0, 0, 0, 1) && !g_switchScope, "keep: a forced same-view SwitchPOV (pairing executor, scene set-up) stays outside");
        Check(sw(1, 1, 0, 0) && !g_switchScope, "keep: SwitchPOV with its skip flag set stays outside");
        EquipRefreshKeepTestSwitchOrig(prevSw);
    }
    const uint8_t* const vc = game.base + 0x48301FE;
    const uint8_t* const vst = vc[0] == 0xE8 ? RipTarget(vc, 1, 5) : nullptr;
    void* vt = nullptr;
    if (vst && vst[0] == 0xFF && vst[1] == 0x25) memcpy(&vt, vst + 6, 8);
    Check(vcallBefore && vc[5] == 0x90 && vt == EquipRefreshKeepTestVcallHook(),
          "keep: the refresh's call [rax+0x608] at exe+0x48301FE -> call stub + nop -> the CreateChildActor hook");
    // the hook against a fake SetChildActorClass on fake objects (flags +8, class +0x10; class +0x2C0, child +0x2C8, template +0x2D0)
    void* const orig = EquipRefreshOneSpawnTestSetup(reinterpret_cast<void*>(&FakeSetClass), 0x2C0, 0x2C8, 0x2D0);
    using HookFn = void (*)(uint8_t*, void*, void*);
    const HookFn hook = reinterpret_cast<HookFn>(EquipRefreshOneSpawnTestHook());
    alignas(16) static uint8_t clsA[0x60], clsB[0x60], child[0x40], tpl[0x40], cac[0x300];
    auto setup = [&](void* childCls, uint32_t flags, void* tplCls, bool hasChild) {
        memset(child, 0, sizeof child); memset(tpl, 0, sizeof tpl); memset(cac, 0, sizeof cac);
        memcpy(child + 8, &flags, 4); memcpy(child + 0x10, &childCls, 8); memcpy(tpl + 0x10, &tplCls, 8);
        void* k = clsA; memcpy(cac + 0x2C0, &k, 8);
        void* c = hasChild ? static_cast<void*>(child) : nullptr; memcpy(cac + 0x2C8, &c, 8);
        void* tp = tplCls ? static_cast<void*>(tpl) : nullptr; memcpy(cac + 0x2D0, &tp, 8);
    };
    auto calls = [&](void* cls, void* tmplArg) { g_calls = 0; hook(cac, cls, tmplArg); return g_calls; };
    setup(clsA, 0, nullptr, true);
    Check(calls(clsA, nullptr) == 0, "one spawn: same class, live child of that class -> the redundant call skipped");
    Check(calls(clsB, nullptr) == 1, "one spawn: another class -> the game's call");
    Check(calls(clsA, tpl) == 1, "one spawn: a template argument -> the game's call");
    setup(clsA, 0, clsA, true);
    Check(calls(clsA, nullptr) == 0, "one spawn: the component's template of that class -> skipped");
    setup(clsA, 0, clsB, true);
    Check(calls(clsA, nullptr) == 1, "one spawn: the component's template of another class -> the game's call (it clears it)");
    setup(clsA, 0x20000000u, nullptr, true);
    Check(calls(clsA, nullptr) == 1, "one spawn: dying child -> the game's call");
    setup(clsB, 0, nullptr, true);
    Check(calls(clsA, nullptr) == 1, "one spawn: child of another class -> the game's call");
    setup(clsA, 0, nullptr, false);
    Check(calls(clsA, nullptr) == 1, "one spawn: no child -> the game's call");
    setup(clsA, 0, nullptr, true);
    SetEquipRefreshOneSpawn(0);
    Check(calls(clsA, nullptr) == 1, "one spawn off (live): the game's call");
    // EquipRefreshKeepActor: decision + the CreateChildActor hook (fake vtable: slot +0x608 -> FakeCreate)
    static void* vtbl[0x700 / 8] = {};
    vtbl[0x608 / 8] = reinterpret_cast<void*>(&FakeCreate);
    using VcFn = void (*)(uint8_t*, void*);
    const VcFn vch = reinterpret_cast<VcFn>(EquipRefreshKeepTestVcallHook());
    alignas(16) static uint8_t other[0x300];
    void* const vp = vtbl;
    auto refresh = [&](void* cls, uint8_t* vcTarget) {  // the refresh's two calls in order: SetChildActorClass, CreateChildActor
        g_calls = 0; g_creates = 0;
        memcpy(cac, &vp, 8); memcpy(other, &vp, 8);
        hook(cac, cls, nullptr);
        vch(vcTarget, nullptr);
        return g_calls * 10 + g_creates;
    };
    Check(refresh(clsA, cac) == 11, "keep: outside a view switch (one spawn off) -> the game's two calls");
    EquipRefreshKeepTestScope(true);
    Check(refresh(clsA, cac) == 0, "keep: inside a view switch, same class, live child -> both calls skipped (the actor stays)");
    Check(refresh(clsB, cac) == 11, "keep: inside a view switch, another class -> the game's two calls");
    Check(refresh(clsA, other) == 1, "keep: the skip is only for that component (another component's CreateChildActor runs)");
    g_creates = 0;
    vch(cac, nullptr);
    Check(g_creates == 1, "keep: the skip is armed for one call only");
    setup(clsA, 0x20000000u, nullptr, true);
    Check(refresh(clsA, cac) == 11, "keep: inside a view switch, dying child -> the game's two calls");
    setup(clsA, 0, nullptr, true);
    SetEquipRefreshKeepActor(0);
    Check(refresh(clsA, cac) == 11, "keep off (live), one spawn off: the game's two calls");
    SetEquipRefreshOneSpawn(1);
    Check(refresh(clsA, cac) == 1, "keep off (live), one spawn on: CreateChildActor only (one build)");
    SetEquipRefreshKeepActor(1);
    Check(refresh(clsA, cac) == 0, "keep on again (live): both skipped");
    EquipRefreshKeepTestScope(false);
    Check(refresh(clsA, cac) == 1, "keep on, outside a view switch, one spawn on: CreateChildActor only");
    SetEquipRefreshOneSpawn(1);
    EquipRefreshOneSpawnTestSetup(orig, 0x2C0, 0x2C8, 0x2D0);
}
}  // namespace ers
