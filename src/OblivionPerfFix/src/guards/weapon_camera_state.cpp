// WeaponCameraStateFix (S-078, worker horsecamfix 2026-11-24; default off until it has been played): after a save load
// the third-person camera on horseback could sit far too close to the horse, and reloading or remounting did not help.
// Cause from a full dump of a play session of 2026-11-23 (findings tags horsecam, horsecamfix):
//  - the player camera manager (BP_AltarPlayerCameraManager_C) lives for the whole game session, through every save
//    load, and so does its camera state machine (VStateMachineComponent "State Machine", ASM_Camera_C);
//  - the state machine's weapon state (State.Camera.DrawingWeapon, and .DrawingWeapon.Sneaking) leaves only when the
//    player has Actor.Action.Combat.SheathingWeapon (or sprints, sneaks or draws a bow); the horse camera
//    (ASM_OnHorse) is reached only from State.Camera.Standing;
//  - a save load restores the player with the weapon away (Actor.Locomotion.MoveSet.Passive) without a sheathe action,
//    so a camera that was in the weapon state at the load stays there: on foot its preset equals Standing's (nothing
//    to see), on a horse the view keeps the on-foot distance (arm 170 instead of 320-340).
//
// The fix. Every 16th game-thread frame (NpcCreationPacing's frame hook) a few reflected fields are read: the player
// controller (the engine's own GetPlayerController, as NpcCreationPacing reads the player's position), its camera
// manager, the camera state machine's current leaf state (VStateMachine.CurrentState, descending through nested
// machines) with its tag and its own state timer, and the player character's GameplayTags / AnimTags (the controller's
// Pawn; while riding that is the horse, a VPairedCreature, and the player character is its Rider). When the leaf is the
// weapon state while the pawn is passive and has neither Combat.DrawingWeapon nor Combat.SheathingWeapon, and that has
// held at every check for 1 s of the state's own timer (it counts play time only), the state machine component's
// BlueprintCallable UFunction Reset is called once through ProcessEvent: the machine leaves its current states and
// starts again from its initial state (ASM_CameraAction -> Standing), as it does when the game starts and loads that
// save; from Standing the game's own transitions then take the horse camera (or stay on foot), with the state's own
// ease. Nothing is done while the weapon is out, while it is being drawn or put away, in dialogue or the vanity camera,
// or when the reset did not leave the state (then not again until the camera has left it). Everything is found by its
// reflected name (classes, properties, the UFunction, the gameplay tags; UObject::ProcessEvent and the FName
// constructor by their patterns); a name or property not found -> one log line, nothing done. No code is patched.
#include "common.h"

#include <atomic>
#include <cstring>
#include <vector>
#include <windows.h>

namespace opf {
namespace {

// UE 5.3 layouts (as StuckEffectLookFix and the HDR menu use them): UObjectBase ClassPrivate +0x10, NamePrivate +0x18;
// UStruct SuperStruct +0x40, Children +0x48, ChildProperties +0x50; UField::Next +0x28; FField Next +0x18, NamePrivate
// +0x20; FProperty ElementSize +0x34, Offset_Internal +0x44; UFunction FunctionFlags +0xB0, NumParms +0xB4, ParmsSize +0xB6.
constexpr size_t kObjClass = 0x10, kObjName = 0x18;
constexpr size_t kStructSuper = 0x40, kStructChildren = 0x48, kStructProps = 0x50, kFieldNextObj = 0x28;
constexpr size_t kFieldNext = 0x18, kFieldName = 0x20, kPropElementSize = 0x34, kPropOffset = 0x44;
constexpr size_t kFnFlags = 0xB0, kFnNumParms = 0xB4, kFnParmsSize = 0xB6;
constexpr uint32_t kFuncNative = 0x400, kFuncBlueprintCallable = 0x04000000;
constexpr uint32_t kCheckEveryFrames = 16;  // ~0.27 s at 60 fps
constexpr float kStuckSeconds = 1.0f;       // of the weapon state's own timer (play time), held at every check
constexpr int kMaxDepth = 6;                // nested state machines followed
constexpr int32_t kMaxTags = 256;           // a tag array longer than this is not read
constexpr uint64_t kResetCapMs = 10000;     // at most one reset per 10 s of wall time (a bound on any loop nobody foresaw)
constexpr uint32_t kNameLogAfter = 450;     // ~2 min of checks with a player controller: then a still missing name is logged

struct NameId {
    uint32_t index = 0, number = 0;
    bool operator==(const NameId& o) const { return index == o.index && number == o.number; }
    bool valid() const { return index != 0; }
};
using FNameFn = NameId* (*)(NameId* out, const wchar_t* name, int findType);
using PeFn = void (*)(void* obj, void* fn, void* parms);
using PcFn = void* (*)();

struct TagArray {
    const NameId* data;
    int32_t num, max;
};

// The names (FNAME_Find: a name that does not exist yet stays 0 and is asked for again at the next check)
enum NameIx {
    kNPlayerCameraManager, kNPawn, kNAltarCameraManager, kNStateMachineComponent, kNCameraTags, kNSmComponentClass,
    kNStateMachineInstance, kNStateMachineClass, kNCurrentState, kNStateBaseClass, kNStateBaseTag, kNThisStateTimer,
    kNPairedPawn, kNGameplayTags, kNAnimTags, kNReset, kNTagWeapon, kNTagWeaponSneaking, kNTagPassive, kNTagDrawing,
    kNTagSheathing, kNPairedCreature, kNRider, kNCount
};
constexpr const wchar_t* kNames[kNCount] = {
    L"PlayerCameraManager", L"Pawn", L"VAltarPlayerCameraManager", L"StateMachineComponent", L"CameraTags",
    L"VStateMachineComponent", L"StateMachineInstance", L"VStateMachine", L"CurrentState", L"VStateBase", L"StateBaseTag",
    L"ThisStateTimer", L"VPairedPawn", L"GameplayTags", L"AnimTags", L"Reset", L"State.Camera.DrawingWeapon",
    L"State.Camera.DrawingWeapon.Sneaking", L"Actor.Locomotion.MoveSet.Passive", L"Actor.Action.Combat.DrawingWeapon",
    L"Actor.Action.Combat.SheathingWeapon", L"VPairedCreature", L"Rider",
};

enum OptIx { kOStanding, kOHorseIdle, kOHorseWalking, kOHorseTrot, kOHorseGallop, kOCurrentSetting, kODesiredArm, kOCount };
constexpr const wchar_t* kOptNames[kOCount] = {
    L"State.Camera.Standing", L"State.Camera.Horse.Idle", L"State.Camera.Horse.Walking", L"State.Camera.Horse.Trot",
    L"State.Camera.Horse.Gallop", L"CurrentCameraSettingData", L"DesiredArmLength",
};
constexpr uint32_t kAfterResetChecks = 4;  // the result of a reset is judged 4 checks (~1.1 s at 60 fps) later

struct Layout {
    uint32_t pcm = 0, pawn = 0, smc = 0, camTags = 0, inst = 0, cur = 0, baseTag = 0, timer = 0, gtags = 0, atags = 0;
    uint32_t rider = 0;                 // VPairedCreature.Rider (resolved at the first ridden creature; 0 = not yet)
    bool riderFailed = false;
    const uint8_t* cmClass = nullptr;   // VAltarPlayerCameraManager
    const uint8_t* smcClass = nullptr;  // VStateMachineComponent
    const uint8_t* smClass = nullptr;   // VStateMachine
    const uint8_t* pawnClass = nullptr; // VPairedPawn
    uint8_t* reset = nullptr;           // VStateMachineComponent.Reset
    uint32_t arm = 0;                   // CurrentCameraSettingData.DesiredArmLength (log only; 0 = not found)
};

// What one check saw (the decision's input)
struct Seen {
    const uint8_t* leaf = nullptr;  // the camera's current leaf state (null: nothing to judge)
    bool weaponState = false;       // the leaf is State.Camera.DrawingWeapon(.Sneaking)
    bool passive = false;           // the pawn's GameplayTags hold MoveSet.Passive
    bool action = false;            // Combat.DrawingWeapon or .SheathingWeapon in GameplayTags or AnimTags
    float timer = 0;                // the leaf's ThisStateTimer
    bool mounted = false;           // the controller's pawn is a ridden creature (only for the log)
};

// The decision's memory
struct Track {
    const uint8_t* armLeaf = nullptr;  // first seen stuck at this leaf...
    float armTimer = 0;                // ...with this state time
    const uint8_t* doneLeaf = nullptr; // reset called at this leaf...
    float doneTimer = 0;               // ...at this state time (the same leaf with a later time = no effect)
    bool noEffectCounted = false;
};

enum class Act { None, Arm, Reset, NoEffect };

struct State {
    bool installed = false;
    FNameFn fname = nullptr;
    PeFn pe = nullptr;
    PcFn pc = nullptr;
    NameId n[kNCount];
    bool namesReady = false;
    Layout l;
    bool layoutReady = false, layoutFailed = false;
    Track t;
    uint32_t frames = 0;
    NameId opt[kOCount];               // optional names (log and the after-reset check)
    uint32_t afterReset = 0;           // checks left until the reset's result is judged (0 = none pending)
    uint32_t nameMisses = 0;       // checks with a player controller while a name was still missing (one log line at kNameLogAfter)
    uint64_t capMs = kResetCapMs;  // at most one reset per this many ms (0 in the self-test's own cases)
    uint64_t lastResetMs = 0;
    uint64_t (*clock)() = &GetTickCount64;
};
State g;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_checks{0}, g_armed{0}, g_resets{0}, g_mounted{0}, g_noEffect{0}, g_horse{0}, g_capped{0};
uint64_t g_logged = ~0ull;

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
NameId NameOf(const uint8_t* obj)
{
    NameId n;
    Rd(obj + kObjName, &n);
    return n;
}
// The class (or one of its parents) named `name`, or null
const uint8_t* ClassNamed(const uint8_t* cls, const NameId& name)
{
    for (int depth = 0; cls && depth < 24; ++depth) {
        if (NameOf(cls) == name) return cls;
        cls = Ptr(cls, kStructSuper);
    }
    return nullptr;
}
bool IsA(const uint8_t* obj, const uint8_t* cls)
{
    const uint8_t* c = Ptr(obj, kObjClass);
    for (int depth = 0; c && cls && depth < 24; ++depth) {
        if (c == cls) return true;
        c = Ptr(c, kStructSuper);
    }
    return false;
}
// A property of the struct (or a parent) by name and element size: its offset, or 0
uint32_t PropOffset(const uint8_t* st, const NameId& name, int32_t size)
{
    for (int depth = 0; st && depth < 24; ++depth) {
        const uint8_t* f = Ptr(st, kStructProps);
        for (int k = 0; f && k < 1024; ++k) {
            NameId fn;
            if (Rd(f + kFieldName, &fn) && fn == name) {
                int32_t off = -1, es = -1;
                if (!Rd(f + kPropOffset, &off) || !Rd(f + kPropElementSize, &es) || es != size) return 0;
                return off > 0 && off < 0x10000 ? static_cast<uint32_t>(off) : 0;
            }
            f = Ptr(f, kFieldNext);
        }
        st = Ptr(st, kStructSuper);
    }
    return 0;
}
// The class's (or a parent's) UFunction `name`: native, BlueprintCallable, no parameters; else null
uint8_t* NoParmFunction(const uint8_t* cls, const NameId& name)
{
    for (int depth = 0; cls && depth < 24; ++depth, cls = Ptr(cls, kStructSuper))
        for (uint8_t* f = Ptr(cls, kStructChildren); f; f = Ptr(f, kFieldNextObj)) {
            if (!(NameOf(f) == name)) continue;
            uint32_t flags = 0;
            uint8_t parms = 0xFF;
            uint16_t size = 0xFFFF;
            if (!Rd(f + kFnFlags, &flags) || !Rd(f + kFnNumParms, &parms) || !Rd(f + kFnParmsSize, &size)) return nullptr;
            if ((flags & (kFuncNative | kFuncBlueprintCallable)) != (kFuncNative | kFuncBlueprintCallable) || parms || size) return nullptr;
            return f;
        }
    return nullptr;
}
bool HasTag(const uint8_t* obj, uint32_t off, const NameId& tag)
{
    TagArray a{};
    if (!tag.valid() || !Rd(obj + off, &a) || !a.data || a.num <= 0 || a.num > kMaxTags) return false;
    for (int32_t i = 0; i < a.num; ++i) {
        NameId t;
        if (Rd(a.data + i, &t) && t == tag) return true;
    }
    return false;
}

bool MakeNames()
{
    if (g.namesReady) return true;
    if (!g.fname) return false;
    for (int i = 0; i < kNCount; ++i) {
        if (g.n[i].valid()) continue;
        g.fname(&g.n[i], kNames[i], 0);  // FNAME_Find
        if (!g.n[i].valid()) {  // not made yet (tags and blueprints load after start-up): next check
            if (++g.nameMisses == kNameLogAfter)
                Log("[weapcam] the name \"%ls\" does not exist yet after ~2 min in the game (renamed by a game update?): "
                    "WeaponCameraStateFix does nothing until it does", kNames[i]);
            return false;
        }
    }
    for (int i = 0; i < kOCount; ++i) g.fname(&g.opt[i], kOptNames[i], 0);  // optional: 0 when missing
    g.namesReady = true;
    return true;
}

// The offsets and the Reset function from the live objects' classes (once; game thread)
bool Resolve(const uint8_t* pc)
{
    if (g.layoutReady) return true;
    if (g.layoutFailed || !MakeNames()) return false;
    Layout l;
    const uint8_t* const pcClass = Ptr(pc, kObjClass);
    l.pcm = PropOffset(pcClass, g.n[kNPlayerCameraManager], 8);
    l.pawn = PropOffset(pcClass, g.n[kNPawn], 8);
    const uint8_t* const cm = l.pcm ? Ptr(pc, l.pcm) : nullptr;
    const uint8_t* const pawn = l.pawn ? Ptr(pc, l.pawn) : nullptr;
    if (!cm || !pawn) return false;  // not in the game yet (the main menu has neither or another camera)
    l.cmClass = ClassNamed(Ptr(cm, kObjClass), g.n[kNAltarCameraManager]);
    l.pawnClass = ClassNamed(Ptr(pawn, kObjClass), g.n[kNPairedPawn]);
    if (!l.cmClass || !l.pawnClass) return false;  // another camera manager or pawn (a menu level): next check
    l.smc = PropOffset(l.cmClass, g.n[kNStateMachineComponent], 8);
    l.camTags = PropOffset(l.cmClass, g.n[kNCameraTags], 8);
    l.gtags = PropOffset(l.pawnClass, g.n[kNGameplayTags], 0x20);
    l.atags = PropOffset(l.pawnClass, g.n[kNAnimTags], 0x20);
    const uint8_t* const smc = l.smc ? Ptr(cm, l.smc) : nullptr;
    if (l.smc && !smc) return false;  // the component is not made yet (or is being torn down): next check
    l.smcClass = smc ? ClassNamed(Ptr(smc, kObjClass), g.n[kNSmComponentClass]) : nullptr;
    l.inst = l.smcClass ? PropOffset(l.smcClass, g.n[kNStateMachineInstance], 8) : 0;
    l.reset = l.smcClass ? NoParmFunction(l.smcClass, g.n[kNReset]) : nullptr;
    const uint8_t* const inst = l.inst ? Ptr(smc, l.inst) : nullptr;
    if (smc && l.smcClass && l.inst && !inst) return false;  // the machine is not made yet: next check
    l.smClass = inst ? ClassNamed(Ptr(inst, kObjClass), g.n[kNStateMachineClass]) : nullptr;
    const uint8_t* const baseClass = l.smClass ? ClassNamed(l.smClass, g.n[kNStateBaseClass]) : nullptr;
    l.cur = l.smClass ? PropOffset(l.smClass, g.n[kNCurrentState], 8) : 0;
    l.baseTag = baseClass ? PropOffset(baseClass, g.n[kNStateBaseTag], 8) : 0;
    l.timer = baseClass ? PropOffset(baseClass, g.n[kNThisStateTimer], 4) : 0;
    if (!l.smc || !l.camTags || !l.gtags || !l.atags || !l.smcClass || !l.inst || !l.reset || !l.smClass || !l.cur ||
        !l.baseTag || !l.timer) {
        g.layoutFailed = true;
        Log("[weapcam] the camera state machine is not as in build 19115871 (state machine +0x%X, CameraTags +0x%X, "
            "GameplayTags +0x%X, AnimTags +0x%X, its component class %d, instance +0x%X, Reset %d, VStateMachine %d, "
            "CurrentState +0x%X, StateBaseTag +0x%X, ThisStateTimer +0x%X): WeaponCameraStateFix does nothing",
            l.smc, l.camTags, l.gtags, l.atags, l.smcClass != nullptr, l.inst, l.reset != nullptr, l.smClass != nullptr, l.cur,
            l.baseTag, l.timer);
        return false;
    }
    for (const uint8_t* st = l.cmClass; st && !l.arm; st = Ptr(st, kStructSuper))
        for (const uint8_t* f = Ptr(st, kStructProps); f; f = Ptr(f, kFieldNext)) {
            NameId fn;
            if (!Rd(f + kFieldName, &fn) || !(fn == g.opt[kOCurrentSetting]) || !fn.valid()) continue;
            int32_t outer = -1;
            const uint8_t* const inner = Ptr(f, 0x70);  // FStructProperty::Struct
            const uint32_t a = inner ? PropOffset(inner, g.opt[kODesiredArm], 4) : 0;
            if (Rd(f + kPropOffset, &outer) && outer > 0 && a) l.arm = static_cast<uint32_t>(outer) + a;
            break;
        }
    g.l = l;
    g.layoutReady = true;
    Log("[weapcam] camera state machine found: PlayerCameraManager +0x%X, Pawn +0x%X, StateMachineComponent +0x%X, "
        "CameraTags +0x%X, StateMachineInstance +0x%X, CurrentState +0x%X, StateBaseTag +0x%X, ThisStateTimer +0x%X, "
        "GameplayTags +0x%X, AnimTags +0x%X",
        l.pcm, l.pawn, l.smc, l.camTags, l.inst, l.cur, l.baseTag, l.timer, l.gtags, l.atags);
    return true;
}

// The player character: the controller's pawn, or while riding (the pawn is a VPairedCreature) its Rider; null when
// neither is a VPairedPawn that is no creature.
const uint8_t* PlayerCharacter(const uint8_t* pawn, bool* mounted)
{
    *mounted = false;
    if (!pawn) return nullptr;
    const uint8_t* const creature = ClassNamed(Ptr(pawn, kObjClass), g.n[kNPairedCreature]);
    if (creature) {
        Layout& l = g.l;
        if (!l.rider && !l.riderFailed) {
            l.rider = PropOffset(creature, g.n[kNRider], 8);
            if (!l.rider) {
                l.riderFailed = true;
                Log("[weapcam] VPairedCreature's Rider not found: WeaponCameraStateFix does nothing while riding");
            } else Log("[weapcam] VPairedCreature.Rider +0x%X (the player while riding)", l.rider);
        }
        if (!l.rider) return nullptr;
        pawn = Ptr(pawn, l.rider);
        *mounted = true;
        if (!pawn || ClassNamed(Ptr(pawn, kObjClass), g.n[kNPairedCreature])) return nullptr;
    }
    return IsA(pawn, g.l.pawnClass) ? pawn : nullptr;
}

// The decision (pure; self-tested): Reset once the weapon state has held for kStuckSeconds of its own timer at every
// check; not again at the same leaf while its timer goes on (the reset had no effect).
Act Decide(Track& t, const Seen& s, bool capped)
{
    if (!s.leaf || !s.weaponState || !s.passive || s.action) {
        t.armLeaf = nullptr;
        t.doneLeaf = nullptr;
        t.noEffectCounted = false;
        return Act::None;
    }
    if (t.doneLeaf) {
        if (s.leaf == t.doneLeaf && s.timer >= t.doneTimer) {
            if (t.noEffectCounted) return Act::None;
            t.noEffectCounted = true;
            return Act::NoEffect;
        }
        t.doneLeaf = nullptr;  // entered again since (its timer started over): judged afresh
        t.noEffectCounted = false;
    }
    if (t.armLeaf != s.leaf || s.timer < t.armTimer) {
        t.armLeaf = s.leaf;
        t.armTimer = s.timer;
        return Act::Arm;
    }
    if (s.timer - t.armTimer < kStuckSeconds) return Act::None;
    if (capped) return Act::None;  // a reset less than kResetCapMs ago: stays armed, fires once the cap has passed
    t.armLeaf = nullptr;
    t.doneLeaf = s.leaf;
    t.doneTimer = s.timer;
    return Act::Reset;
}

void Check()
{
    g_checks.fetch_add(1, std::memory_order_relaxed);
    uint8_t* const pc = static_cast<uint8_t*>(g.pc ? g.pc() : nullptr);
    if (!pc || !Resolve(pc)) {
        Decide(g.t, Seen{}, false);
        g.afterReset = 0;
        return;
    }
    const Layout& l = g.l;
    Seen s;
    uint8_t* const cm = Ptr(pc, l.pcm);
    const uint8_t* const pawn = PlayerCharacter(Ptr(pc, l.pawn), &s.mounted);
    uint8_t* const smc = IsA(cm, l.cmClass) ? Ptr(cm, l.smc) : nullptr;
    const uint8_t* const inst = IsA(smc, l.smcClass) ? Ptr(smc, l.inst) : nullptr;
    if (inst && IsA(pawn, l.pawnClass) && IsA(inst, l.smClass)) {
        const uint8_t* st = inst;
        for (int depth = 0; depth < kMaxDepth; ++depth) {
            const uint8_t* const c = Ptr(st, l.cur);
            if (!c) break;
            if (!IsA(c, l.smClass)) {
                s.leaf = c;
                break;
            }
            st = c;
        }
        NameId tag;
        if (s.leaf && Rd(s.leaf + l.baseTag, &tag) && Rd(s.leaf + l.timer, &s.timer)) {
            s.weaponState = tag == g.n[kNTagWeapon] || tag == g.n[kNTagWeaponSneaking];
            if (s.weaponState) {
                s.passive = HasTag(pawn, l.gtags, g.n[kNTagPassive]);
                s.action = HasTag(pawn, l.gtags, g.n[kNTagDrawing]) || HasTag(pawn, l.gtags, g.n[kNTagSheathing]) ||
                           HasTag(pawn, l.atags, g.n[kNTagDrawing]) || HasTag(pawn, l.atags, g.n[kNTagSheathing]);
            }
        } else s.leaf = nullptr;
    }
    if (g.afterReset && --g.afterReset == 0) {
        // the reset's result: on a horse it must have reached the horse camera (the review's check, revs078 finding 1)
        NameId tag;
        float arm = -1;
        if (l.arm) Rd(cm + l.arm, &arm);
        const bool known = s.leaf && Rd(s.leaf + l.baseTag, &tag);
        const bool horse = known && tag.valid() &&
                           (tag == g.opt[kOHorseIdle] || tag == g.opt[kOHorseWalking] || tag == g.opt[kOHorseTrot] ||
                            tag == g.opt[kOHorseGallop]);
        const bool standing = known && tag.valid() && tag == g.opt[kOStanding];
        const char* const what = !known ? "unknown" : horse ? "the horse camera" : standing ? "Standing"
                                 : s.weaponState ? "still the weapon state" : "another state";
        Log("[weapcam] after the reset: %s, arm %.0f (mounted %d)", what, arm, s.mounted ? 1 : 0);
        if (horse) g_horse.fetch_add(1, std::memory_order_relaxed);
        else if (s.mounted && !s.weaponState) {
            g_noEffect.fetch_add(1, std::memory_order_relaxed);
            Log("[weapcam] the reset did not bring back the horse camera while riding: no effect");
        }
    }
    const bool capped = g.capMs && g.lastResetMs && g.clock() - g.lastResetMs < g.capMs;
    const Act act = Decide(g.t, s, capped);
    if (capped && g.t.armLeaf && s.weaponState && s.timer - g.t.armTimer >= kStuckSeconds) g_capped.fetch_add(1, std::memory_order_relaxed);
    switch (act) {
    case Act::Arm:
        g_armed.fetch_add(1, std::memory_order_relaxed);
        break;
    case Act::Reset: {
        const bool mounted = s.mounted;
        NameId leafTag, camTag;
        Rd(s.leaf + l.baseTag, &leafTag);
        Rd(cm + l.camTags, &camTag);
        Log("[weapcam] reset: the camera's state machine stayed in its weapon state (%s, %.1f s; CameraTags %s) while the "
            "player's weapon is away (mounted %d): restarted from its initial state",
            leafTag == g.n[kNTagWeaponSneaking] ? "DrawingWeapon.Sneaking" : "DrawingWeapon", s.timer,
            camTag == leafTag ? "the same" : "another", mounted ? 1 : 0);
        g_resets.fetch_add(1, std::memory_order_relaxed);
        if (mounted) g_mounted.fetch_add(1, std::memory_order_relaxed);
        uint8_t parms[16] = {};
        g.lastResetMs = g.clock();
        if (!g.lastResetMs) g.lastResetMs = 1;
        g.afterReset = kAfterResetChecks;
        g.pe(smc, l.reset, parms);
        break;
    }
    case Act::NoEffect:
        g_noEffect.fetch_add(1, std::memory_order_relaxed);
        Log("[weapcam] the reset did not leave the weapon state (%.1f s): not again until the camera leaves it", s.timer);
        break;
    case Act::None:
        break;
    }
}

}  // namespace

bool InstallWeaponCameraStateFix()
{
    if (g.installed) return true;
    uint8_t* const pe = FindProcessEvent();
    uint8_t* const fname = FindFNameCtor();
    if (!pe || !fname) {
        Log("[weapcam] UObject::ProcessEvent %s, FName's constructor %s by pattern: WeaponCameraStateFix not installed",
            pe ? "found" : "not found", fname ? "found" : "not found");
        return false;
    }
    g.pe = reinterpret_cast<PeFn>(pe);
    g.fname = reinterpret_cast<FNameFn>(fname);
    g.pc = &NpcPacingPlayerController;
    g.installed = true;
    Log("[weapcam] installed: ProcessEvent exe+0x%llX, FName exe+0x%llX; the camera state machine is checked every %u "
        "game-thread frames (no code patched)",
        static_cast<unsigned long long>(pe - Game().base), static_cast<unsigned long long>(fname - Game().base), kCheckEveryFrames);
    return true;
}

void SetWeaponCameraStateFix(bool on)
{
    g_on.store(on && g.installed, std::memory_order_relaxed);
}

void WeaponCameraStateFrameEnd()
{
    if (!g.installed || !g_on.load(std::memory_order_relaxed)) return;
    if (++g.frames % kCheckEveryFrames) return;
    Check();
}

void LogWeaponCameraStateStats()
{
    if (!g.installed) return;
    const uint64_t r = g_resets.load(), ne = g_noEffect.load();
    const uint64_t key = r * 1000003ull + ne;
    if (key == g_logged || (r == 0 && ne == 0)) return;
    g_logged = key;
    Log("[weapcam] stats: %llu camera resets (%llu mounted), %llu reached the horse camera, %llu without effect, %llu "
        "checks held back by the one-per-10-s cap; %llu checks",
        static_cast<unsigned long long>(r), static_cast<unsigned long long>(g_mounted.load()),
        static_cast<unsigned long long>(g_horse.load()), static_cast<unsigned long long>(ne),
        static_cast<unsigned long long>(g_capped.load()), static_cast<unsigned long long>(g_checks.load()));
}

// ---- self-test: discovery by name and the decision on fake objects (classes, properties, a UFunction, tags) ----
namespace {
struct Fake {
    std::vector<uint8_t> b;
    explicit Fake(size_t n = 0x2600) : b(n, 0) {}
    uint8_t* p() { return b.data(); }
    template <class T> void put(size_t off, const T& v) { memcpy(b.data() + off, &v, sizeof v); }
};
// a fake name table: index = position + 1 in this list (0 = not found)
const wchar_t* const kTestNames[] = {
    L"PlayerCameraManager", L"Pawn", L"VAltarPlayerCameraManager", L"StateMachineComponent", L"CameraTags",
    L"VStateMachineComponent", L"StateMachineInstance", L"VStateMachine", L"CurrentState", L"VStateBase", L"StateBaseTag",
    L"ThisStateTimer", L"VPairedPawn", L"GameplayTags", L"AnimTags", L"Reset", L"State.Camera.DrawingWeapon",
    L"State.Camera.DrawingWeapon.Sneaking", L"Actor.Locomotion.MoveSet.Passive", L"Actor.Action.Combat.DrawingWeapon",
    L"Actor.Action.Combat.SheathingWeapon", L"Actor.Action.Docked.Horse", L"State.Camera.Standing", L"State.Camera.Dialogue",
    L"BP_PC", L"BP_CM", L"BP_Pawn", L"ASM_Camera_C", L"ASM_CameraAction_C", L"AST_C", L"VPairedCreature", L"Rider",
    L"BP_Horse", L"State.Camera.Horse.Idle",
};
const wchar_t* g_testMissing = nullptr;  // a name the fake table does not know
NameId* TestFName(NameId* out, const wchar_t* name, int)
{
    *out = NameId{};
    if (g_testMissing && !wcscmp(name, g_testMissing)) return out;
    for (size_t i = 0; i < sizeof kTestNames / sizeof kTestNames[0]; ++i)
        if (!wcscmp(name, kTestNames[i])) out->index = static_cast<uint32_t>(i + 1);
    return out;
}
NameId TN(const wchar_t* name)
{
    NameId n;
    TestFName(&n, name, 0);
    return n;
}
uint8_t* g_testPc = nullptr;
void* TestPc() { return g_testPc; }
int g_testPeCalls = 0;
uint8_t *g_testPeObj = nullptr, *g_testPeFn = nullptr;
uint8_t *g_testRoot = nullptr, *g_testAction = nullptr, *g_testStanding = nullptr;  // what a working Reset does
bool g_testPeWorks = true;
uint8_t* g_testAfter = nullptr;  // where a working Reset lands (null: Standing; the game's own transition is not modelled)
uint64_t g_testNow = 0;
uint64_t TestClock() { return g_testNow; }
void TestPe(void* obj, void* fn, void*)
{
    ++g_testPeCalls;
    g_testPeObj = static_cast<uint8_t*>(obj);
    g_testPeFn = static_cast<uint8_t*>(fn);
    if (!g_testPeWorks) return;
    memcpy(g_testAction + 0x98, g_testAfter ? &g_testAfter : &g_testStanding, 8);  // ASM_CameraAction -> Standing (timer 0)
    const float zero = 0;
    memcpy(g_testStanding + 0x44, &zero, 4);
}
}  // namespace

int WeaponCameraStateTestLogic()
{
    const State saved = g;  // the real state (installed or not) comes back afterwards
    const bool savedOn = g_on.load();
    int failed = 0;
    auto check = [&](bool ok, int id) {
        if (!ok && !failed) failed = id;
    };
    // classes: name +0x18, super +0x40, Children +0x48, ChildProperties +0x50
    Fake objCls(0x100), cmNative(0x100), cmBp(0x100), pcCls(0x100), smcCls(0x100), baseCls(0x100), smBase(0x100), smCls(0x100),
        asmCam(0x100), asmAct(0x100), stateCls(0x100), astCls(0x100), pawnNative(0x100), pawnBp(0x100), creatureCls(0x100),
        horseCls(0x100);
    auto cls = [&](Fake& c, const wchar_t* name, Fake* super) {
        c.put(kObjName, TN(name));
        c.put(kStructSuper, super ? super->p() : static_cast<uint8_t*>(nullptr));
    };
    cls(objCls, L"Object", nullptr);  // not in the table: index 0 (never matched)
    cls(pcCls, L"BP_PC", &objCls);
    cls(cmNative, L"VAltarPlayerCameraManager", &objCls);
    cls(cmBp, L"BP_CM", &cmNative);
    cls(smcCls, L"VStateMachineComponent", &objCls);
    cls(baseCls, L"VStateBase", &objCls);
    cls(smBase, L"VStateMachineBase", &baseCls);
    cls(smCls, L"VStateMachine", &smBase);
    cls(asmCam, L"ASM_Camera_C", &smCls);
    cls(asmAct, L"ASM_CameraAction_C", &smCls);
    cls(stateCls, L"VState", &baseCls);
    cls(astCls, L"AST_C", &stateCls);
    cls(pawnNative, L"VPairedPawn", &objCls);
    cls(pawnBp, L"BP_Pawn", &pawnNative);
    cls(creatureCls, L"VPairedCreature", &pawnNative);
    cls(horseCls, L"BP_Horse", &creatureCls);
    // properties: FField Next +0x18, name +0x20, ElementSize +0x34, Offset +0x44
    std::vector<Fake*> fields;
    auto prop = [&](Fake& owner, const wchar_t* name, int32_t size, int32_t off) {
        Fake* f = new Fake(0x80);
        f->put(kFieldName, TN(name));
        f->put(kPropElementSize, size);
        f->put(kPropOffset, off);
        f->put(kFieldNext, Ptr(owner.p(), kStructProps));
        owner.put(kStructProps, f->p());
        fields.push_back(f);
    };
    prop(pcCls, L"Pawn", 8, 0x2D8);
    prop(pcCls, L"PlayerCameraManager", 8, 0x350);
    prop(cmNative, L"StateMachineComponent", 8, 0x24E0);
    prop(cmNative, L"CameraTags", 8, 0x24E8);
    prop(smcCls, L"StateMachineInstance", 8, 0x188);
    prop(baseCls, L"StateBaseTag", 8, 0x28);
    prop(baseCls, L"ThisStateTimer", 4, 0x44);
    prop(smCls, L"CurrentState", 8, 0x98);
    prop(pawnNative, L"GameplayTags", 0x20, 0xBA0);
    prop(pawnNative, L"AnimTags", 0x20, 0xBD0);
    prop(creatureCls, L"Rider", 8, 0xE30);
    // UFunction Reset: name +0x18, Next +0x28, flags +0xB0, NumParms +0xB4, ParmsSize +0xB6
    Fake fnOther(0x100), fnReset(0x100);
    fnOther.put(kObjName, TN(L"CurrentState"));
    fnOther.put(kFieldNextObj, fnReset.p());
    fnReset.put(kObjName, TN(L"Reset"));
    fnReset.put(kFnFlags, 0x04020401u);
    smcCls.put(kStructChildren, fnOther.p());
    // objects
    Fake pc, cm, smc, root(0x200), action(0x200), weapon(0x200), standing(0x200), dialogue(0x200), pawn(0x1000), horse(0x1000),
        horseIdle(0x200);
    pc.put(kObjClass, pcCls.p());
    cm.put(kObjClass, cmBp.p());
    smc.put(kObjClass, smcCls.p());
    root.put(kObjClass, asmCam.p());
    action.put(kObjClass, asmAct.p());
    weapon.put(kObjClass, astCls.p());
    standing.put(kObjClass, astCls.p());
    dialogue.put(kObjClass, astCls.p());
    pawn.put(kObjClass, pawnBp.p());
    horse.put(kObjClass, horseCls.p());
    horse.put(0xE30, pawn.p());  // its Rider
    pc.put(0x2D8, horse.p());    // riding: the controller's pawn is the horse
    pc.put(0x350, cm.p());
    cm.put(0x24E0, smc.p());
    cm.put(0x24E8, TN(L"State.Camera.DrawingWeapon"));
    smc.put(0x188, root.p());
    root.put(0x98, action.p());
    action.put(0x98, weapon.p());
    weapon.put(0x28, TN(L"State.Camera.DrawingWeapon"));
    standing.put(0x28, TN(L"State.Camera.Standing"));
    dialogue.put(0x28, TN(L"State.Camera.Dialogue"));
    NameId tagsG[4], tagsA[4];
    NameId horseTags[2] = {TN(L"Actor.Locomotion.MoveSet.Passive")};
    horse.put(0xBA0, TagArray{horseTags, 1, 2});  // the horse itself is passive (as in the dump)
    auto setTags = [&](size_t off, NameId* store, std::initializer_list<const wchar_t*> names) {
        int n = 0;
        for (const wchar_t* t : names) store[n++] = TN(t);
        const TagArray a{store, n, 4};
        pawn.put(off, a);
    };
    auto timer = [&](Fake& st, float t) { st.put(0x44, t); };
    g = State{};
    g.capMs = 0;
    g.installed = true;
    g.fname = &TestFName;
    g.pe = &TestPe;
    g.pc = &TestPc;
    g_testPc = pc.p();
    g_testRoot = root.p();
    g_testAction = action.p();
    g_testStanding = standing.p();
    g_testPeWorks = true;
    g_testPeCalls = 0;
    g_testMissing = nullptr;
    g_on.store(true);
    const uint64_t resets0 = g_resets.load(), mounted0 = g_mounted.load(), noEffect0 = g_noEffect.load();

    // 1. stuck after a load, mounted: armed at the first check, reset once 1 s of the state's time later
    setTags(0xBA0, tagsG, {L"Actor.Locomotion.MoveSet.Passive", L"Actor.Action.Docked.Horse"});
    setTags(0xBD0, tagsA, {L"Actor.Locomotion.MoveSet.Passive"});
    timer(weapon, 559.6f);
    Check();
    check(g.layoutReady && g.l.reset == fnReset.p() && g.l.cur == 0x98 && g.l.timer == 0x44 && g.l.gtags == 0xBA0, 1);
    check(g_testPeCalls == 0, 2);
    timer(weapon, 560.3f);
    Check();
    check(g_testPeCalls == 0, 3);
    timer(weapon, 560.7f);
    Check();
    check(g_testPeCalls == 1 && g_testPeObj == smc.p() && g_testPeFn == fnReset.p(), 4);
    check(g_resets.load() == resets0 + 1 && g_mounted.load() == mounted0 + 1, 5);
    // 2. after the reset the camera is in Standing: nothing more
    timer(standing, 2.0f);
    Check();
    Check();
    check(g_testPeCalls == 1 && !g.t.doneLeaf && !g.t.armLeaf, 6);
    // 2b. riding: the rider's tags count, not the horse's (rider not passive, horse passive: never); a horse without a
    // rider: nothing
    action.put(0x98, weapon.p());
    setTags(0xBA0, tagsG, {L"Actor.Action.Docked.Horse"});
    for (int i = 0; i < 6; ++i) {
        timer(weapon, 3.f + i);
        Check();
    }
    setTags(0xBA0, tagsG, {L"Actor.Locomotion.MoveSet.Passive"});
    horse.put(0xE30, static_cast<uint8_t*>(nullptr));
    for (int i = 0; i < 6; ++i) {
        timer(weapon, 10.f + i);
        Check();
    }
    check(g_testPeCalls == 1 && g.l.rider == 0xE30, 24);
    horse.put(0xE30, pawn.p());
    pc.put(0x2D8, pawn.p());  // on foot from here

    // 3. the weapon really out (not passive): never
    g_testPeCalls = 0;
    action.put(0x98, weapon.p());
    setTags(0xBA0, tagsG, {L"Actor.Locomotion.MoveSet.Passive"});
    setTags(0xBA0, tagsG, {L"Actor.Action.Docked.Horse"});
    for (int i = 0; i < 8; ++i) {
        timer(weapon, 10.f + i);
        Check();
    }
    check(g_testPeCalls == 0, 7);
    // 4. putting it away (SheathingWeapon in AnimTags) or drawing it (DrawingWeapon in GameplayTags): never
    setTags(0xBA0, tagsG, {L"Actor.Locomotion.MoveSet.Passive"});
    setTags(0xBD0, tagsA, {L"Actor.Action.Combat.SheathingWeapon"});
    for (int i = 0; i < 8; ++i) {
        timer(weapon, 20.f + i);
        Check();
    }
    setTags(0xBD0, tagsA, {});
    setTags(0xBA0, tagsG, {L"Actor.Locomotion.MoveSet.Passive", L"Actor.Action.Combat.DrawingWeapon"});
    for (int i = 0; i < 8; ++i) {
        timer(weapon, 30.f + i);
        Check();
    }
    check(g_testPeCalls == 0, 8);
    // 5. the timer does not move (a menu, the loading screen): not reset
    setTags(0xBA0, tagsG, {L"Actor.Locomotion.MoveSet.Passive"});
    timer(weapon, 40.f);
    for (int i = 0; i < 8; ++i) Check();
    check(g_testPeCalls == 0, 9);
    // 6. a break in the condition re-arms: passive, not passive at +0.5, passive at +1.0 -> nothing; +2.0 -> reset
    setTags(0xBA0, tagsG, {});
    Check();  // (ends step 5's arming)
    setTags(0xBA0, tagsG, {L"Actor.Locomotion.MoveSet.Passive"});
    timer(weapon, 50.f);
    Check();
    setTags(0xBA0, tagsG, {});
    timer(weapon, 50.5f);
    Check();
    setTags(0xBA0, tagsG, {L"Actor.Locomotion.MoveSet.Passive"});
    timer(weapon, 51.0f);
    Check();
    check(g_testPeCalls == 0, 10);
    timer(weapon, 52.0f);
    Check();
    check(g_testPeCalls == 1 && g_mounted.load() == mounted0 + 1, 11);  // not mounted this time
    // 7. the sneaking weapon state is reset as well; dialogue never
    action.put(0x98, weapon.p());
    weapon.put(0x28, TN(L"State.Camera.DrawingWeapon.Sneaking"));
    timer(weapon, 5.f);
    Check();
    timer(weapon, 6.5f);
    Check();
    check(g_testPeCalls == 2, 12);
    weapon.put(0x28, TN(L"State.Camera.DrawingWeapon"));
    action.put(0x98, dialogue.p());
    for (int i = 0; i < 8; ++i) {
        timer(dialogue, 1.f + i);
        Check();
    }
    check(g_testPeCalls == 2, 13);
    // 8. a reset without effect: called once, one "no effect", not again while the state's time goes on; again once the
    // state was entered anew (its timer started over)
    g_testPeWorks = false;
    action.put(0x98, weapon.p());
    timer(weapon, 100.f);
    Check();
    timer(weapon, 101.5f);
    Check();
    check(g_testPeCalls == 3, 14);
    for (int i = 0; i < 6; ++i) {
        timer(weapon, 102.f + i);
        Check();
    }
    check(g_testPeCalls == 3 && g_noEffect.load() == noEffect0 + 1, 15);
    timer(weapon, 0.5f);
    Check();
    timer(weapon, 1.6f);
    Check();
    check(g_testPeCalls == 4, 16);
    g_testPeWorks = true;
    // 9. off: nothing (the frame-end entry)
    action.put(0x98, weapon.p());
    g_on.store(false);
    for (uint32_t i = 0; i < 8 * kCheckEveryFrames; ++i) {
        timer(weapon, 200.f + i * 0.02f);
        WeaponCameraStateFrameEnd();
    }
    check(g_testPeCalls == 4, 17);
    // 10. on through the frame-end entry: a check every kCheckEveryFrames frames (the state entered anew)
    g_on.store(true);
    g.t = Track{};
    for (uint32_t i = 0; i < 8 * kCheckEveryFrames; ++i) {
        timer(weapon, 300.f + i * 0.02f);
        WeaponCameraStateFrameEnd();
    }
    check(g_testPeCalls == 5, 18);
    // 11. no player controller, or a camera manager of another class: nothing
    g_testPc = nullptr;
    action.put(0x98, weapon.p());
    for (int i = 0; i < 4; ++i) {
        timer(weapon, 400.f + i);
        Check();
    }
    g_testPc = pc.p();
    cm.put(kObjClass, pcCls.p());
    for (int i = 0; i < 4; ++i) {
        timer(weapon, 500.f + i);
        Check();
    }
    cm.put(kObjClass, cmBp.p());
    check(g_testPeCalls == 5, 19);
    // 12. a name not there yet: nothing, then found once it exists
    g = State{};
    g.capMs = 0;
    g.installed = true;
    g.fname = &TestFName;
    g.pe = &TestPe;
    g.pc = &TestPc;
    g_testMissing = L"State.Camera.DrawingWeapon";
    for (int i = 0; i < 4; ++i) {
        timer(weapon, 600.f + i);
        Check();
    }
    check(g_testPeCalls == 5 && !g.namesReady && !g.layoutFailed, 20);
    g_testMissing = nullptr;
    timer(weapon, 700.f);
    Check();
    timer(weapon, 701.5f);
    Check();
    check(g_testPeCalls == 6 && g.layoutReady, 21);
    // 13. a property gone (CurrentState renamed): logged once, nothing done
    g = State{};
    g.capMs = 0;
    g.installed = true;
    g.fname = &TestFName;
    g.pe = &TestPe;
    g.pc = &TestPc;
    fields[7]->put(kFieldName, TN(L"AnimTags"));  // CurrentState's field now carries another name
    for (int i = 0; i < 4; ++i) {
        timer(weapon, 800.f + i);
        Check();
    }
    check(g_testPeCalls == 6 && g.layoutFailed && !g.layoutReady, 22);
    // 14. Reset with a parameter is not taken
    fields[7]->put(kFieldName, TN(L"CurrentState"));
    fnReset.put(kFnNumParms, static_cast<uint8_t>(1));
    g = State{};
    g.capMs = 0;
    g.installed = true;
    g.fname = &TestFName;
    g.pe = &TestPe;
    g.pc = &TestPc;
    for (int i = 0; i < 4; ++i) {
        timer(weapon, 900.f + i);
        Check();
    }
    check(g_testPeCalls == 6 && g.layoutFailed, 23);

    // 15. the reset's result while riding: Standing = no effect (counted, logged), the horse camera = success
    fnReset.put(kFnNumParms, static_cast<uint8_t>(0));
    g = State{};
    g.capMs = 0;
    g.installed = true;
    g.fname = &TestFName;
    g.pe = &TestPe;
    g.pc = &TestPc;
    horseIdle.put(kObjClass, astCls.p());
    horseIdle.put(0x28, TN(L"State.Camera.Horse.Idle"));
    pc.put(0x2D8, horse.p());
    horse.put(0xE30, pawn.p());
    setTags(0xBA0, tagsG, {L"Actor.Locomotion.MoveSet.Passive", L"Actor.Action.Docked.Horse"});
    setTags(0xBD0, tagsA, {});
    {
        const uint64_t ne0 = g_noEffect.load(), h0 = g_horse.load();
        const int calls0 = g_testPeCalls;
        g_testAfter = nullptr;  // lands in Standing and stays there
        action.put(0x98, weapon.p());
        timer(weapon, 10.f);
        Check();
        timer(weapon, 11.5f);
        Check();
        for (int i = 0; i < 4; ++i) Check();
        check(g_testPeCalls == calls0 + 1 && g_noEffect.load() == ne0 + 1 && g_horse.load() == h0, 25);
        g_testAfter = horseIdle.p();  // lands in the horse camera
        action.put(0x98, weapon.p());
        timer(weapon, 20.f);
        Check();
        timer(weapon, 21.5f);
        Check();
        for (int i = 0; i < 4; ++i) Check();
        check(g_testPeCalls == calls0 + 2 && g_noEffect.load() == ne0 + 1 && g_horse.load() == h0 + 1, 26);
        // 16. the cap: a second stuck episode within 10 s waits until 10 s after the first reset
        g.capMs = 10000;
        g.clock = &TestClock;
        g.lastResetMs = 0;
        g_testNow = 100000;
        action.put(0x98, weapon.p());
        timer(weapon, 30.f);
        Check();
        timer(weapon, 31.5f);
        Check();  // reset (no earlier one)
        action.put(0x98, weapon.p());
        g_testNow = 104000;
        timer(weapon, 1.f);  // the weapon state entered anew (its timer started over)
        Check();
        timer(weapon, 2.5f);
        Check();  // stuck again 4 s later: held back
        check(g_testPeCalls == calls0 + 3, 27);
        g_testNow = 110500;
        timer(weapon, 3.f);
        Check();  // 10.5 s after the first: now
        check(g_testPeCalls == calls0 + 4, 28);
        g_testAfter = nullptr;
    }
    for (Fake* f : fields) delete f;
    g = saved;
    g_on.store(savedOn);
    g_testPc = nullptr;
    return failed;
}

}  // namespace opf
