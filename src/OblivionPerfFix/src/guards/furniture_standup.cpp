// FurnitureStandUpFix (worker tpose1123 2026-11-23, S-074; default 0 until a stock repro and the release decide): a
// character who gets up from a chair is no longer left standing in the reference pose ("T-pose"), unable to be
// talked to.
//
// What was seen (findings 2026-11-23 tpose1123, a live full dump of a play session, the Leyawiin stable owner
// Atahba T-posed after a fast travel to Five Riders Stables): the character's state machine (ASM_Character_C) sat in its
// parallel main state (PSM_CharacterMain_C: locomotion in the air + action idle) for the whole 47 s since she was made,
// while her docking component held a chair (BP_LCChair02L_C) with the old engine's sit state 5 ("getting up"), and her
// tags held `Actor.Action.Docked` and a pending `Input.Action.Undock`. Her movement was still the seated one (Flying,
// GravityScale 0), so the in-air state never ended; her seated animation layer had no seated animation any more (the chair
// exit had played), which draws the reference pose; the old engine waited in "getting up" for the whole time.
//
// Why it stays so (the game's blueprints, read from the dump): the main state goes to the seated state
// (AST_CharacterDocked_C) only through COND_IsDocked_C, which asks VPairedCharacter.IsDocked(): the docking component's
// dock actor is valid and its sit state is 4 (sitting) or 9 (sleeping). Only the seated state leads to the getting-up
// state (AST_CharacterUndocking_C, on the pending Undock input), and only that state's exit calls
// SnapToStandingPosition(true, true) (exe+0x4867BE0 in build 19115871), which gives back gravity and walking, frees the
// chair, places the character at its standing point, tells the old engine the stand-up is done ("ReversePairing_DockingState"
// with state 0: the old engine's process sit state goes to 0 and the furniture is released) and removes the docking tags.
// When the seated state was never entered (here: sit state never seen at 4 while the main state checked) or was left
// before the Undock input came, the sit state 5 makes IsDocked() false for ever: the character is stuck as above.
//
// The fix: VPairedCharacter.IsDocked's blueprint function (its exec function; found by its shape and by the two functions
// it reaches, which give the docking component's offset, the dock actor's and the sit state's) calls through our stub.
// The answer stays the game's, except in exactly the stuck state: asked by a COND_IsDocked_C (the blueprint's condition
// object, the calling frame's object), the character's state machine in its parallel main state, the docking component's
// dock actor valid and a VFurniture, the sit state 5, `Input.Action.Undock` among the character's tags and
// `Input.Action.Dock` not among them (review standupfix2 S1: an undock request that comes after DockTo's sit state 2 but
// before the docking state was entered also gives sit state 5 with the main state current; there the stock game still
// goes through its docking state on the pending Dock input and stands up by itself). Then the answer is true once more, so the game's own state machine goes main -> seated -> getting up and finishes the stand-up as it does
// when the race does not happen (the seated state's entry and exit, the getting-up state with its stand-up animation and
// SnapToStandingPosition): every step is the game's own code. With no Dock input pending, no state-machine path of the
// stock game leads out of this state (native callers of the stand-up could end it), so nothing else changes; a normal
// stand-up never passes through it (StartUndock exe+0x4852D70 sets sit state 5 and adds the Undock input in one call,
// while the seated state is current, and the seated state's first transition is the getting-up state). A bed is not
// covered: getting up from a bed is sit state 10. Horses (a horse is not a VFurniture)
// and every other caller of IsDocked keep the game's answer.
//
// Found by: the exec function's shape (a bool getter's exec thunk, 241 in the build) whose call reaches
// `mov rcx,[rcx+disp32]; jmp` and then the docking component's IsDocked shape (weak pointer at +disp32 through the weak
// valid call, sit state byte at +disp32 compared with 4 and 9); exactly one such thunk, else the key stays off with a log
// line. Reflection data (names) give the state machine's offsets and the tags' offset when first needed; FName's
// constructor and GUObjectArray by their patterns at start-up. Start-up install when on, then live (`FurnitureStandUpFix=0`
// later gives the game's answer again).
#include "common.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// VPairedCharacter.IsDocked's exec function (build 19115871 exe+0x46B2860): P_FINISH (Stack.Code += !!Stack.Code), the
// getter's call, *(bool*)Result = al.
constexpr char kThunk[] =
    "40 53 48 83 EC 20 48 8B 42 20 45 33 C9 48 85 C0 49 8B D8 41 0F 95 C1 4C 03 C8 4C 89 4A 20 E8 ?? ?? ?? ?? 88 03 48 83 C4 20 5B C3";
constexpr size_t kThunkCall = 0x1E;
// VPairedCharacter::IsDocked (exe+0x485A020): mov rcx,[rcx+0F48h]; jmp VDockingPairingComponent::IsDocked
constexpr char kPawnGetter[] = "48 8B 89 ?? ?? ?? ?? E9 ?? ?? ?? ??";
constexpr size_t kPawnGetterDisp = 3, kPawnGetterJmp = 7;
// VDockingPairingComponent::IsDocked (exe+0x48C07E0): weak dock actor at +0x1B4 valid and sit state +0x1B0 in {4, 9}
constexpr char kDockGetter[] =
    "40 53 48 83 EC 20 48 8B D9 48 81 C1 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F B6 8B ?? ?? ?? ?? 80 F9 04 74 09 80 F9 09 74 04 32 C9 "
    "EB 02 B1 01 84 C0 74 0C 84 C9 74 08 B0 01 48 83 C4 20 5B C3";
constexpr size_t kDockWeakDisp = 12, kDockStateDisp = 24;
constexpr uint8_t kGettingUp = 5;  // the old engine's sit state "wants to stand / getting up" (GetSitting 4)
constexpr size_t kStubSize = 16;

// UE 5.3 layouts (the same as PhysicsSubstepCap / StuckEffectLookFix use): UObjectBase ClassPrivate +0x10, NamePrivate
// +0x18; UStruct SuperStruct +0x40, ChildProperties +0x50; FField ClassPrivate +0x08, Next +0x18, NamePrivate +0x20;
// FProperty ElementSize +0x34, Offset_Internal +0x44; FFrame Object +0x18; GUObjectArray chunks +0x10, NumElements +0x24,
// FUObjectItem 24 bytes {Object, Flags, ClusterRootIndex, SerialNumber}; FGameplayTagContainer = TArray<FGameplayTag>
// first (FGameplayTag = its FName).
constexpr size_t kObjClass = 0x10, kObjName = 0x18, kStructSuper = 0x40, kStructProps = 0x50;
constexpr size_t kFieldClass = 0x08, kFieldNext = 0x18, kFieldName = 0x20, kPropElementSize = 0x34, kPropOffset = 0x44;
constexpr size_t kFrameObject = 0x18;
constexpr size_t kArrayChunks = 0x10, kArrayNum = 0x24, kItemSize = 24, kItemFlags = 8, kItemSerial = 16;
constexpr uint32_t kChunkItems = 64 * 1024;
constexpr int32_t kItemDead = (1 << 21) | (1 << 28) | (1 << 29);  // EInternalObjectFlags Garbage, Unreachable, PendingKill
constexpr uint32_t kMaxPropOffset = 0x4000;
constexpr int32_t kMaxTags = 256;

struct NameId {
    uint32_t index = 0, number = 0;
    bool operator==(const NameId& o) const { return index == o.index && number == o.number; }
    bool valid() const { return index != 0; }
};
using FNameFn = NameId* (*)(NameId* out, const wchar_t* name, int findType);
using GetterFn = uint8_t (*)(uint8_t* pawn);

struct Layout {  // resolved on the first candidate (game thread); 0 = not yet
    NameId condName, parallelName, furnitureName, undockName, dockName;
    uint32_t smComp = 0, smInstance = 0, current = 0, tags = 0;
    bool resolved = false, failed = false;
};

struct State {
    bool installed = false;
    std::atomic<bool> on{false};
    GetterFn original = nullptr;
    uint32_t dockOff = 0, weakOff = 0, stateOff = 0;
    uint8_t* site = nullptr;
    uint8_t* stub = nullptr;
    FNameFn fname = nullptr;
    uint8_t* objArray = nullptr;
    Layout lay;
};
State s;
std::atomic<uint64_t> g_candidates{0}, g_applied{0}, g_faults{0}, g_logged{0};
int g_named = 0;  // game thread: characters named in the log so far

bool Fail(const char* why)
{
    Log("[standup] %s: FurnitureStandUpFix not installed", why);
    return false;
}

template <class T> bool Rd(const void* p, T* v)
{
    if (!p) return false;
    __try {
        memcpy(v, p, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint8_t* Ptr(const uint8_t* p, size_t off)
{
    uint8_t* v = nullptr;
    return p && Rd(p + off, &v) ? v : nullptr;
}
NameId NameAt(const uint8_t* p)
{
    NameId n;
    if (p) Rd(p, &n);
    return n;
}
NameId Lookup(const wchar_t* text)
{
    NameId n;
    if (s.fname) s.fname(&n, text, 0);  // FNAME_Find: none until the name exists
    return n;
}

// The object's class or one of its parents has this name.
bool IsA(const uint8_t* obj, const NameId& cls)
{
    const uint8_t* c = Ptr(obj, kObjClass);
    for (int depth = 0; c && depth < 32; ++depth, c = Ptr(c, kStructSuper))
        if (NameAt(c + kObjName) == cls) return true;
    return false;
}

// An object property's offset (8 bytes) or a struct's of the given size, by name, in the class or a parent.
bool PropOffset(const uint8_t* cls, const NameId& name, int32_t size, uint32_t* out)
{
    for (int depth = 0; cls && depth < 32; ++depth, cls = Ptr(cls, kStructSuper)) {
        const uint8_t* f = Ptr(cls, kStructProps);
        for (int n = 0; f && n < 1024; ++n, f = Ptr(f, kFieldNext)) {
            if (!(NameAt(f + kFieldName) == name)) continue;
            int32_t elem = 0, off = -1;
            if (!Ptr(f, kFieldClass) || !Rd(f + kPropElementSize, &elem) || !Rd(f + kPropOffset, &off) || elem != size || off < 0x28 ||
                static_cast<uint32_t>(off) > kMaxPropOffset || off % 8)
                return false;
            *out = static_cast<uint32_t>(off);
            return true;
        }
    }
    return false;
}

// TWeakObjectPtr {ObjectIndex, SerialNumber} -> the object if still the same live one.
uint8_t* ResolveWeak(const uint8_t* weak)
{
    int32_t idx = -1, serial = 0, num = 0;
    if (!Rd(weak, &idx) || !Rd(weak + 4, &serial) || idx < 0 || serial == 0) return nullptr;
    uint8_t* const* chunks = nullptr;
    if (!Rd(s.objArray + kArrayNum, &num) || idx >= num || !Rd(s.objArray + kArrayChunks, &chunks) || !chunks) return nullptr;
    uint8_t* chunk = nullptr;
    if (!Rd(chunks + idx / kChunkItems, &chunk) || !chunk) return nullptr;
    const uint8_t* const item = chunk + kItemSize * (idx % kChunkItems);
    uint8_t* obj = nullptr;
    int32_t flags = 0, itemSerial = 0;
    if (!Rd(item, &obj) || !obj || !Rd(item + kItemFlags, &flags) || (flags & kItemDead) || !Rd(item + kItemSerial, &itemSerial) ||
        itemSerial != serial)
        return nullptr;
    return obj;
}

// Names and offsets, from the first candidate character asked by a COND_IsDocked_C (game thread). Names are made by the
// engine as their objects load: a missing one is looked up again on the next candidate; an offset that is not found turns
// the key off for the session.
bool Resolve(const uint8_t* pawn)
{
    Layout& L = s.lay;
    if (L.resolved) return true;
    if (L.failed) return false;
    if (!L.parallelName.valid()) L.parallelName = Lookup(L"VParallelStateMachine");
    if (!L.furnitureName.valid()) L.furnitureName = Lookup(L"VFurniture");
    if (!L.undockName.valid()) L.undockName = Lookup(L"Input.Action.Undock");
    if (!L.dockName.valid()) L.dockName = Lookup(L"Input.Action.Dock");
    if (!L.parallelName.valid() || !L.furnitureName.valid() || !L.undockName.valid() || !L.dockName.valid()) return false;
    const NameId nSm = Lookup(L"StateMachineComponent"), nInst = Lookup(L"StateMachineInstance"),
                 nCur = Lookup(L"CurrentState"), nTags = Lookup(L"GameplayTags");
    const uint8_t* const pawnCls = Ptr(pawn, kObjClass);
    uint32_t smComp = 0, smInstance = 0, current = 0, tags = 0;
    const uint8_t* comp = nullptr;
    const uint8_t* inst = nullptr;
    bool ok = nSm.valid() && nInst.valid() && nCur.valid() && nTags.valid() && PropOffset(pawnCls, nSm, 8, &smComp) &&
              PropOffset(pawnCls, nTags, 0x20, &tags) && (comp = Ptr(pawn, smComp)) != nullptr &&
              PropOffset(Ptr(comp, kObjClass), nInst, 8, &smInstance) && (inst = Ptr(comp, smInstance)) != nullptr &&
              PropOffset(Ptr(inst, kObjClass), nCur, 8, &current);
    if (!ok) {
        L.failed = true;
        Log("[standup] the state machine's or the tags' reflection data not found as expected: FurnitureStandUpFix off");
        return false;
    }
    L.smComp = smComp;
    L.smInstance = smInstance;
    L.current = current;
    L.tags = tags;
    L.resolved = true;
    Log("[standup] layout: StateMachineComponent +0x%X, StateMachineInstance +0x%X, CurrentState +0x%X, GameplayTags +0x%X", smComp,
        smInstance, current, tags);
    return true;
}

// Input.Action.Undock among the character's tags and Input.Action.Dock not (an unreadable or oversized tag list: false).
bool UndockOnly(const uint8_t* pawn)
{
    const uint8_t* data = Ptr(pawn, s.lay.tags);
    int32_t num = 0;
    if (!data || !Rd(pawn + s.lay.tags + 8, &num) || num <= 0 || num > kMaxTags) return false;
    bool undock = false;
    for (int32_t i = 0; i < num; ++i) {
        const NameId n = NameAt(data + 8 * i);
        if (n == s.lay.dockName) return false;
        if (n == s.lay.undockName) undock = true;
    }
    return undock;
}

// The stuck state (see the top): true when the game's false answer should be true once more.
bool Stuck(uint8_t* pawn, const uint8_t* frame)
{
    const uint8_t* const dock = Ptr(pawn, s.dockOff);
    uint8_t sit = 0;
    if (!dock || !Rd(dock + s.stateOff, &sit) || sit != kGettingUp) return false;  // the common case ends here
    g_candidates.fetch_add(1, std::memory_order_relaxed);
    // the caller first: only the blueprint condition's question can get the other answer (and only it resolves the layout)
    Layout& L = s.lay;
    if (!L.condName.valid()) L.condName = Lookup(L"COND_IsDocked_C");
    const uint8_t* const caller = Ptr(frame, kFrameObject);
    if (!L.condName.valid() || !caller || !(NameAt(Ptr(caller, kObjClass) + kObjName) == L.condName)) return false;
    if (!Resolve(pawn)) return false;
    const uint8_t* const actor = ResolveWeak(dock + s.weakOff);
    if (!actor || !IsA(actor, L.furnitureName)) return false;
    const uint8_t* const comp = Ptr(pawn, L.smComp);
    const uint8_t* const inst = Ptr(comp, L.smInstance);
    const uint8_t* const cur = Ptr(inst, L.current);
    if (!cur || !IsA(cur, L.parallelName)) return false;
    return UndockOnly(pawn);
}

void LogApplied(const uint8_t* pawn)
{
    if (g_named >= 8) return;
    ++g_named;
    const uint8_t* const cls = Ptr(pawn, kObjClass);
    const NameId n = NameAt(pawn + kObjName), c = NameAt(cls + kObjName);
    Log("[standup] a character left seated with a pending stand-up (sit state 5, state machine in its main state, no Dock "
        "input) is led through the seated state to its stand-up (pawn %p, name id %u_%u, class name id %u)",
        static_cast<const void*>(pawn), n.index, n.number, c.index);
}

// The exec function's call (rcx = the character, rdx = the calling FFrame, untouched by the thunk before its call).
uint8_t Hook(uint8_t* pawn, uint8_t* frame)
{
    const uint8_t r = s.original(pawn);
    if (r || !s.on.load(std::memory_order_relaxed)) return r;
    bool stuck = false;
    __try {
        stuck = Stuck(pawn, frame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        stuck = false;
    }
    if (!stuck) return r;
    g_applied.fetch_add(1, std::memory_order_relaxed);
    LogApplied(pawn);
    return 1;
}

bool Discover(const GameModule& game, FurnitureStandUpInfo* out)
{
    *out = {};
    const Pattern pawnGetter = ParsePattern(kPawnGetter), dockGetter = ParsePattern(kDockGetter);
    for (uint8_t* t : FindAll(game.text, ParsePattern(kThunk))) {
        ++out->thunks;
        uint8_t* const c = RipTarget(t + kThunkCall, 1, 5);
        if (!game.text.Contains(c) || !MatchesAt(c, pawnGetter)) continue;
        uint8_t* const g = RipTarget(c + kPawnGetterJmp, 1, 5);
        if (!game.text.Contains(g) || !MatchesAt(g, dockGetter)) continue;
        if (out->thunk) {
            out->thunk = nullptr;
            return Fail("IsDocked's exec function found more than once");
        }
        out->thunk = t;
        out->site = t + kThunkCall;
        out->getter = c;
        out->dockGetter = g;
        int32_t d = 0;
        memcpy(&d, c + kPawnGetterDisp, 4);
        out->dockOff = static_cast<uint32_t>(d);
        memcpy(&d, g + kDockWeakDisp, 4);
        out->weakOff = static_cast<uint32_t>(d);
        memcpy(&d, g + kDockStateDisp, 4);
        out->stateOff = static_cast<uint32_t>(d);
    }
    if (!out->thunk) return Fail("IsDocked's exec function not found");
    if (out->dockOff > kMaxPropOffset || out->weakOff > 0x1000 || out->stateOff > 0x1000) return Fail("unexpected offsets in IsDocked");
    return true;
}

}  // namespace

bool FurnitureStandUpDiscover(const GameModule& game, FurnitureStandUpInfo* out) { return Discover(game, out); }

bool InstallFurnitureStandUpFix(bool beforeGameCode, bool enabled)
{
    if (s.installed) return true;
    if (!enabled) return false;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    const GameModule& game = Game();
    FurnitureStandUpInfo d{};
    if (!Discover(game, &d)) return false;
    s.fname = reinterpret_cast<FNameFn>(FindFNameCtor());
    s.objArray = FindObjectArrayFromDtor();
    if (!s.fname || !s.objArray) return Fail(!s.fname ? "FName's constructor not found" : "GUObjectArray not found");
    uint8_t* const stub = AllocNearGame(kStubSize);
    if (!stub) return Fail("no free memory near the game's code");
    void* const hook = reinterpret_cast<void*>(&Hook);
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(stub, jmpAbs, 6);
    memcpy(stub + 6, &hook, 8);
    DWORD old;
    VirtualProtect(stub, kStubSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, kStubSize);
    const int64_t rel = stub - (d.site + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("the stub is out of reach");
    s.original = reinterpret_cast<GetterFn>(d.getter);
    s.dockOff = d.dockOff;
    s.weakOff = d.weakOff;
    s.stateOff = d.stateOff;
    s.stub = stub;
    const int32_t rel32 = static_cast<int32_t>(rel);
    if (!WriteCode(d.site + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4)))
        return Fail("could not patch IsDocked's call");
    s.site = d.site;
    s.installed = true;
    s.on = true;
    Log("[standup] installed: IsDocked's exec function exe+0x%llX (call exe+0x%llX -> exe+0x%llX), docking component +0x%X, dock "
        "actor +0x%X, sit state +0x%X",
        static_cast<unsigned long long>(d.thunk - game.base), static_cast<unsigned long long>(d.site - game.base),
        static_cast<unsigned long long>(d.getter - game.base), d.dockOff, d.weakOff, d.stateOff);
    return true;
}

void SetFurnitureStandUpFix(bool enabled)
{
    if (!s.installed) return;
    if (s.on.exchange(enabled) != enabled)
        Log("[standup] %s", enabled ? "a character stuck before its stand-up is led through it" : "IsDocked answers as the game");
}

void LogFurnitureStandUpStats()
{
    if (!s.installed) return;
    const uint64_t a = g_applied.load(), c = g_candidates.load(), f = g_faults.load();
    if (a + f + c == g_logged.load()) return;
    g_logged = a + f + c;
    Log("[standup] stand-ups led through the seated state %llu (IsDocked questions with sit state 5: %llu; read faults %llu)",
        static_cast<unsigned long long>(a), static_cast<unsigned long long>(c), static_cast<unsigned long long>(f));
}

// ---- self-test: the decision on fakes (after the install on the mapped exe; the original getter replaced by a fake) ----
void FurnitureStandUpTestSetup(void* fakeGetter, void* fakeFname, uint8_t* fakeArray, uint32_t dockOff, uint32_t weakOff,
                               uint32_t stateOff)
{
    s.original = reinterpret_cast<GetterFn>(fakeGetter);
    s.fname = reinterpret_cast<FNameFn>(fakeFname);
    s.objArray = fakeArray;
    s.dockOff = dockOff;
    s.weakOff = weakOff;
    s.stateOff = stateOff;
    s.lay = Layout{};
    g_applied = 0;
    g_candidates = 0;
    g_faults = 0;
    g_named = 8;  // no log lines from the fakes
    s.installed = true;
    s.on = true;
}
uint8_t FurnitureStandUpTestAsk(void* pawn, void* frame) { return Hook(static_cast<uint8_t*>(pawn), static_cast<uint8_t*>(frame)); }
uint64_t FurnitureStandUpTestApplied() { return g_applied.load(); }
bool FurnitureStandUpTestResolved() { return s.lay.resolved; }

}  // namespace opf
