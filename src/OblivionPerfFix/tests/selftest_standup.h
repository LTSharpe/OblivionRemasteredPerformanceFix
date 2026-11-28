// FurnitureStandUpFix self-test (worker tpose1123 2026-11-23, S-074), included by selftest.cpp: the discovery on the mapped
// exe (IsDocked's exec function among the bool getters' thunks, the pawn's getter, the docking component's getter and the
// three offsets), the install (the thunk's call goes to an absolute-jump stub), then the decision on fakes: the game's true
// answer is kept; false becomes true only for a character whose docking component holds a furniture dock actor at sit
// state 5, asked by a COND_IsDocked_C, with its state machine in a parallel (main) state, `Input.Action.Undock` among
// its tags and `Input.Action.Dock` not (review S1: an undock request before the docking state was entered); each single
// condition missing keeps the game's false; a stale weak pointer, a horse, the key off and an unreadable docking component
// keep it too; another caller is answered before any name or layout lookup.
#pragma once
namespace stup {

// ---- fake names: FName(const TCHAR*, FNAME_Find) over a small table ----
static const wchar_t* const kNames[] = {L"", L"COND_IsDocked_C", L"VParallelStateMachine", L"VFurniture", L"Input.Action.Undock",
                                        L"StateMachineComponent", L"StateMachineInstance", L"CurrentState", L"GameplayTags",
                                        L"BP_Pawn", L"BP_SmComp", L"ASM_Root", L"PSM_Main", L"AST_Docked", L"BP_Chair", L"BP_Horse",
                                        L"VPawn", L"COND_Other", L"Input.Action.Other", L"ObjectProperty", L"StructProperty",
                                        L"Input.Action.Dock"};
struct Nm { uint32_t index, number; };
static Nm* FakeFName(Nm* out, const wchar_t* text, int)
{
    *out = {0, 0};
    for (uint32_t i = 1; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        if (wcscmp(kNames[i], text) == 0) *out = {i, 0};
    return out;
}
static Nm N(const wchar_t* t) { Nm n; return *FakeFName(&n, t, 0); }

struct Blk { alignas(16) uint8_t b[0x300]; };  // a fake UObject / UClass / FField (zeroed)
static void PutName(Blk& o, size_t off, const wchar_t* t) { const Nm n = N(t); memcpy(o.b + off, &n, 8); }
static void PutPtr(Blk& o, size_t off, const void* p) { memcpy(o.b + off, &p, 8); }

static uint8_t g_answer = 0;
static uint8_t FakeGetter(uint8_t*) { return g_answer; }

static void Run(const GameModule& game)
{
    FurnitureStandUpInfo d{};
    Check(FurnitureStandUpDiscover(game, &d), "FurnitureStandUpFix: discovery on the mapped exe");
    Check(d.thunks > 1 && d.thunk && d.site == d.thunk + 0x1E && d.site[0] == 0xE8,
          "FurnitureStandUpFix: IsDocked's exec function found once among the bool getters' thunks");
    if (game.timeDateStamp == 0xF19077A4u) {
        Check(d.thunk - game.base == 0x46B2860 && d.getter - game.base == 0x485A020 && d.dockGetter - game.base == 0x48C07E0,
              "FurnitureStandUpFix: exec exe+0x46B2860, pawn getter exe+0x485A020, docking getter exe+0x48C07E0 (build 19115871)");
        Check(d.dockOff == 0xF48 && d.weakOff == 0x1B4 && d.stateOff == 0x1B0,
              "FurnitureStandUpFix: docking component +0xF48, dock actor +0x1B4, sit state +0x1B0 (build 19115871)");
    }
    if (!Installed(InstallFurnitureStandUpFix(true, true), "FurnitureStandUpFix (installs on the mapped exe)")) return;
    const uint8_t* const stub = RipTarget(d.site, 1, 5);
    Check(d.site[0] == 0xE8 && stub && stub[0] == 0xFF && stub[1] == 0x25 && *reinterpret_cast<const uint32_t*>(stub + 2) == 0,
          "FurnitureStandUpFix: the exec function's call goes to an absolute-jump stub");
    MEMORY_BASIC_INFORMATION mi{};
    Check(VirtualQuery(stub, &mi, sizeof mi) && mi.Protect == PAGE_EXECUTE_READ, "FurnitureStandUpFix: the stub's page is read-execute");
    Check(InstallFurnitureStandUpFix(true, true), "FurnitureStandUpFix: a second install finds it in place");

    // ---- fakes: classes, properties, objects, the object list ----
    static Blk clsPawn, clsPawnSuper, clsSm, clsRoot, clsPsm, clsParallel, clsDocked, clsChair, clsFurniture, clsHorse, clsVPawn,
        clsCond, clsCondOther, fSm, fTags, fInst, fCur, fieldClass;
    for (Blk* b : {&clsPawn, &clsPawnSuper, &clsSm, &clsRoot, &clsPsm, &clsParallel, &clsDocked, &clsChair, &clsFurniture, &clsHorse,
                   &clsVPawn, &clsCond, &clsCondOther, &fSm, &fTags, &fInst, &fCur, &fieldClass})
        memset(b->b, 0, sizeof b->b);
    auto field = [](Blk& f, const wchar_t* name, int32_t size, int32_t off, const void* next) {
        PutPtr(f, 0x08, &fieldClass);
        PutPtr(f, 0x18, next);
        PutName(f, 0x20, name);
        memcpy(f.b + 0x34, &size, 4);
        memcpy(f.b + 0x44, &off, 4);
    };
    PutName(clsPawn, 0x18, L"BP_Pawn");
    PutPtr(clsPawn, 0x40, &clsPawnSuper);
    field(fTags, L"GameplayTags", 0x20, 0x120, nullptr);
    field(fSm, L"StateMachineComponent", 8, 0x100, &fTags);
    PutPtr(clsPawnSuper, 0x50, &fSm);
    PutName(clsPawnSuper, 0x18, L"VPawn");
    PutName(clsSm, 0x18, L"BP_SmComp");
    field(fInst, L"StateMachineInstance", 8, 0x40, nullptr);
    PutPtr(clsSm, 0x50, &fInst);
    PutName(clsRoot, 0x18, L"ASM_Root");
    field(fCur, L"CurrentState", 8, 0x48, nullptr);
    PutPtr(clsRoot, 0x50, &fCur);
    PutName(clsPsm, 0x18, L"PSM_Main");
    PutPtr(clsPsm, 0x40, &clsParallel);
    PutName(clsParallel, 0x18, L"VParallelStateMachine");
    PutName(clsDocked, 0x18, L"AST_Docked");
    PutName(clsChair, 0x18, L"BP_Chair");
    PutPtr(clsChair, 0x40, &clsFurniture);
    PutName(clsFurniture, 0x18, L"VFurniture");
    PutName(clsHorse, 0x18, L"BP_Horse");
    PutPtr(clsHorse, 0x40, &clsVPawn);
    PutName(clsVPawn, 0x18, L"VPawn");
    PutName(clsCond, 0x18, L"COND_IsDocked_C");
    PutName(clsCondOther, 0x18, L"COND_Other");

    static Blk pawn, dock, smComp, root, psm, docked, chair, horse, cond, condOther, frame, frameOther;
    for (Blk* b : {&pawn, &dock, &smComp, &root, &psm, &docked, &chair, &horse, &cond, &condOther, &frame, &frameOther})
        memset(b->b, 0, sizeof b->b);
    constexpr uint32_t kDockOff = 0x200, kWeakOff = 0x1B4, kStateOff = 0x1B0;
    PutPtr(pawn, 0x10, &clsPawn);
    PutPtr(pawn, 0x100, &smComp);
    PutPtr(pawn, kDockOff, &dock);
    Nm tags[3] = {N(L"Input.Action.Other"), N(L"Input.Action.Undock"), N(L"Input.Action.Dock")};  // count 2: no Dock
    PutPtr(pawn, 0x120, tags);
    int32_t two = 2;
    memcpy(pawn.b + 0x128, &two, 4);
    PutPtr(smComp, 0x10, &clsSm);
    PutPtr(smComp, 0x40, &root);
    PutPtr(root, 0x10, &clsRoot);
    PutPtr(root, 0x48, &psm);
    PutPtr(psm, 0x10, &clsPsm);
    PutPtr(docked, 0x10, &clsDocked);
    PutPtr(chair, 0x10, &clsChair);
    PutPtr(horse, 0x10, &clsHorse);
    PutPtr(cond, 0x10, &clsCond);
    PutPtr(condOther, 0x10, &clsCondOther);
    PutPtr(frame, 0x18, &cond);
    PutPtr(frameOther, 0x18, &condOther);
    dock.b[kStateOff] = 5;
    // the object list: index 3 = the chair (serial 7), index 4 = the horse (serial 9)
    static uint8_t items[8 * 24];
    memset(items, 0, sizeof items);
    auto item = [](int i, const void* obj, int32_t serial) {
        memcpy(items + 24 * i, &obj, 8);
        memcpy(items + 24 * i + 16, &serial, 4);
    };
    item(3, &chair, 7);
    item(4, &horse, 9);
    static uint8_t* chunks[1] = {items};
    static Blk array;
    memset(array.b, 0, sizeof array.b);
    PutPtr(array, 0x10, chunks);
    int32_t num = 8;
    memcpy(array.b + 0x24, &num, 4);
    auto weak = [&](int32_t idx, int32_t serial) {
        memcpy(dock.b + kWeakOff, &idx, 4);
        memcpy(dock.b + kWeakOff + 4, &serial, 4);
    };
    weak(3, 7);

    FurnitureStandUpTestSetup(reinterpret_cast<void*>(&FakeGetter), reinterpret_cast<void*>(&FakeFName), array.b, kDockOff, kWeakOff,
                              kStateOff);
    auto ask = [&](const void* f) { return FurnitureStandUpTestAsk(pawn.b, const_cast<void*>(f)); };
    g_answer = 1;
    Check(ask(frame.b) == 1 && FurnitureStandUpTestApplied() == 0, "FurnitureStandUpFix: the game's true answer is kept");
    g_answer = 0;
    Check(ask(frameOther.b) == 0 && FurnitureStandUpTestApplied() == 0 && !FurnitureStandUpTestResolved(),
          "FurnitureStandUpFix: another caller keeps the game's false before any layout lookup");
    Check(ask(frame.b) == 1 && FurnitureStandUpTestApplied() == 1 && FurnitureStandUpTestResolved(),
          "FurnitureStandUpFix: the stuck state (chair, sit state 5, main state, Undock pending, asked by COND_IsDocked_C) answers true");
    Check(ask(frameOther.b) == 0, "FurnitureStandUpFix: another caller keeps the game's false");
    dock.b[kStateOff] = 4;
    Check(ask(frame.b) == 0, "FurnitureStandUpFix: sit state 4 keeps the game's false");
    dock.b[kStateOff] = 0;
    Check(ask(frame.b) == 0, "FurnitureStandUpFix: sit state 0 keeps the game's false");
    dock.b[kStateOff] = 5;
    PutPtr(root, 0x48, &docked);
    Check(ask(frame.b) == 0, "FurnitureStandUpFix: the seated state current keeps the game's false (a normal stand-up)");
    PutPtr(root, 0x48, &psm);
    weak(4, 9);
    Check(ask(frame.b) == 0, "FurnitureStandUpFix: a horse as the dock actor keeps the game's false");
    weak(3, 8);
    Check(ask(frame.b) == 0, "FurnitureStandUpFix: a stale weak pointer (serial) keeps the game's false");
    weak(3, 7);
    memcpy(pawn.b + 0x128, &(num = 1), 4);  // only Input.Action.Other
    Check(ask(frame.b) == 0, "FurnitureStandUpFix: no pending Undock keeps the game's false");
    memcpy(pawn.b + 0x128, &(num = 3), 4);  // Undock and Dock pending: an undock request before the docking state
    Check(ask(frame.b) == 0, "FurnitureStandUpFix: a pending Dock input keeps the game's false (stock leaves through Docking)");
    tags[1] = N(L"Input.Action.Dock");  // Dock before Undock in the list
    tags[2] = N(L"Input.Action.Undock");
    Check(ask(frame.b) == 0, "FurnitureStandUpFix: a pending Dock input listed before Undock keeps the game's false");
    tags[1] = N(L"Input.Action.Undock");
    tags[2] = N(L"Input.Action.Dock");
    num = 2;
    memcpy(pawn.b + 0x128, &num, 4);
    Check(ask(frame.b) == 1 && FurnitureStandUpTestApplied() == 2, "FurnitureStandUpFix: the Dock input gone answers true again");
    SetFurnitureStandUpFix(false);
    Check(ask(frame.b) == 0, "FurnitureStandUpFix: off (live) keeps the game's false");
    SetFurnitureStandUpFix(true);
    Check(ask(frame.b) == 1 && FurnitureStandUpTestApplied() == 3, "FurnitureStandUpFix: on again answers true");
    PutPtr(pawn, kDockOff, reinterpret_cast<void*>(0x10));  // unreadable docking component
    Check(ask(frame.b) == 0, "FurnitureStandUpFix: an unreadable docking component keeps the game's false (no fault)");
    PutPtr(pawn, kDockOff, &dock);
}

}  // namespace stup
