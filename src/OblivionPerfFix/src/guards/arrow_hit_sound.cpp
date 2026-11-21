// ArrowHitSoundFix: an NPC's arrow hit is heard at the shooter's bow, often twice or three times (a game bug, GitHub #52).
//
// When an arrow hits a character, two pieces of the game's code post the weapon impact sound (nws_weap_impact):
//  - the arrow's own impact (build 19115871 exe+0x4860BE0, reached from the Unreal side's arrow hit, exe+0x48652E0
//    Arrow_hit_Reverse_Pairing): on the ARROW, with the impact type and material of the hit character's component
//    (exe+0x486F6B0 sets them), i.e. where the hit is;
//  - the attacker's "hit dealt" handler (exe+0x485D970), the code that makes a sword's hit sound, called once for each of
//    the game's two engines that registers the hit (the old engine's own projectile impact and the Unreal side's): on the
//    attacker's WEAPON actor, with the same event and the same impact type and material. For a sword the weapon is at the
//    victim; for a bow it is in the shooter's hands, 10-30 m (or more) from the hit.
// So an NPC arrow hit plays its thud at the shooter once or twice, and at the target once if the Unreal side saw the hit
// (seen orders: arrow + bow; bow only; bow, arrow, bow). A hunter near the player makes a hit on a deer 100 m away sound
// at full volume beside the player (measured: the bow 10-29 m from the arrow).
//
// The fix (one thud per NPC arrow hit, at the hit, like a melee hit; the player's own shots unchanged), for
// shooters whose "Player" sound flag (+0xB20, the flag the game itself uses for the Player switch) is 0. Two call redirects:
//  - the handler's weapon post (exe+0x485DB08) for an arrow hit (the hit record's arrow flag +0x1C, its arrow +0x20 a live
//    arrow): if a thud for this arrow, shooter, hit character and event was already posted on the arrow (within 1 s), the
//    call is not made; else the post is made on the ARROW instead of the weapon, after the game's own switch setter
//    (exe+0x486F6B0) set the hit's impact type and material on the arrow, i.e. exactly what the arrow's own impact does.
//    If that post gives no playing id, the game's post on the weapon is made (a hit is never silent).
//  - the arrow's own post (exe+0x4860CE6): if the handler already posted this hit's thud on the arrow (within 1 s), it is
//    not made again; else the game's call runs unchanged and, when it returns a playing id, the hit is remembered.
// Anything else (melee, spells, the player, an arrow that is gone or not an arrow) gets the game's calls unchanged.
// Each redirect is a 5-byte call to a small stub that hands the hook two of the caller's saved registers in the arguments
// whose values the pattern fixes (r8 = 1, r9 = the game's constant), which the hook passes on as they were.
#include "common.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <windows.h>

namespace opf {
namespace {

// The handler's switch setup and weapon post, build 19115871 exe+0x485DAC2 (in exe+0x485D970; rbx = the attacker, rdi = the
// hit record, rsi = the target's hit component):
//   33 D2 48 8B CB E8 [4871410]          xor edx,edx; mov rcx,rbx; call (enchantment switch)
//   45 33 C9 45 33 C0 48 8B D6 48 8B CB E8 [486F6B0]   (attacker, component, 0, 0): impact type and material
//   33 D2 48 8B CB E8 [4856A30]          xor edx,edx; mov rcx,rbx; call (the attacker's weapon actor)
//   48 8B 8B 38 0D 00 00                 mov rcx,[rbx+0D38h]       the attacker's impact event
//   C6 44 24 28 01 4C 8D 0D [const]      mov byte [rsp+28h],1; lea r9,[const]
//   41 B8 01 00 00 00 44 88 74 24 20     mov r8d,1; mov [rsp+20h],r14b
//   48 8B D0 E8 [post]                   mov rdx,rax; call the post helper  <- redirected
//   44 38 77 2D                          cmp [rdi+2Dh],r14b
constexpr const char* kBowPost =
    "33 D2 48 8B CB E8 ?? ?? ?? ?? 45 33 C9 45 33 C0 48 8B D6 48 8B CB E8 ?? ?? ?? ?? "
    "33 D2 48 8B CB E8 ?? ?? ?? ?? 48 8B 8B 38 0D 00 00 C6 44 24 28 01 4C 8D 0D ?? ?? ?? ?? 41 B8 01 00 00 00 "
    "44 88 74 24 20 48 8B D0 E8 ?? ?? ?? ?? 44 38 77 2D";
constexpr size_t kBowSwitchCall = 22, kBowConst = 49, kBowCall = 70;
// Earlier in the same handler (exe+0x485D9E4): the spell flag +0x1D and the target +0x08 of the record in rdi
//   44 38 77 1D 0F 85 ????????  48 8B 77 08 48 85 F6
constexpr const char* kTarget = "44 38 77 1D 0F 85 ?? ?? ?? ?? 48 8B 77 08 48 85 F6";
// Then (exe+0x485DA81) the target's hit component: mov rsi,[rsi+790h]
constexpr const char* kComp = "48 8B B6 ?? ?? ?? ?? 4C 89 75 D0";
constexpr size_t kCompOff = 3;
// Later in the same handler (exe+0x485DBC9): the arrow flag +0x1C, the arrow's weak pointer +0x20, FWeakObjectPtr::Get and
// the arrow class (StaticClass, then the IsA test: class +0x30 / depth +0x38 against the object's class +0x10)
//   44 38 77 1C 0F 84 ????????  48 8D 4F 20 E8 [weakGet]  48 8B D8 48 85 C0 0F 84 ????????  E8 [arrowClass]
//   48 8B 4B 10 48 83 C0 30 48 63 50 08 3B 51 38
constexpr const char* kArrowBranch =
    "44 38 77 1C 0F 84 ?? ?? ?? ?? 48 8D 4F 20 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 8B 4B 10 48 83 C0 30 48 63 50 08 3B 51 38";
constexpr size_t kWeakGetCall = 14, kArrowClassCall = 31;
// The shooter's "Player" sound switch in the weapon's sound setup (exe+0x486DADA): cmp byte [rbp+0B20h],0 picks PC or NPC
//   80 BD [off32] 00 48 8B CF 4C 8B 05 ???????? 4C 0F 45 05 ???????? 48 8B 15 ???????? E8
constexpr const char* kPlayerFlag =
    "80 BD ?? ?? ?? ?? 00 48 8B CF 4C 8B 05 ?? ?? ?? ?? 4C 0F 45 05 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? E8";
constexpr size_t kPlayerFlagOff = 2;
// The arrow's own impact post: the whole function exe+0x4860BE0 up to its post (rcx = shooter -> rdi, rdx = the hit
// component -> rbp, r9 = the arrow -> rbx; rbx falls back to the shooter's weapon when r9 is not a live arrow; a tagged
// shooter posts another event on another actor, with rdx != rbx). It calls the same switch setter (shooter, component,
// index, arrow) before its post.
constexpr const char* kArrowPost =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B D9 41 8B F0 48 8B EA 48 8B F9 4D 85 C9 74 ?? "
    "E8 ?? ?? ?? ?? 4C 8B 4B 10 4C 8D 50 30 48 63 40 38 41 3B 41 38 7F ?? 48 8B C8 49 8B 41 30 4C 39 14 C8 75 ?? "
    "8B 43 08 A9 00 00 00 60 75 ?? 83 BB ?? ?? ?? ?? 00 0F 8F ?? ?? ?? ?? 80 BB ?? ?? ?? ?? 00 75 ?? B2 01 48 8B CB "
    "E8 ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ?? 33 C0 48 89 44 24 58 4C 3B C0 74 ?? 48 8B 8F A0 0B 00 00 48 63 87 A8 0B 00 00 "
    "48 8D 14 C1 48 3B CA 74 ?? 0F 1F 00 4C 39 01 74 ?? 48 83 C1 08 48 3B CA 75 ?? 33 D2 48 8B CF E8 ?? ?? ?? ?? "
    "4C 8B CB 44 8B C6 48 8B D5 48 8B CF E8 ?? ?? ?? ?? 48 85 DB 74 ?? 8B 43 08 A9 00 00 00 60 74 ?? 33 D2 48 8B CF "
    "E8 ?? ?? ?? ?? 48 8B D8 48 8B 8F 38 0D 00 00 48 8B D3 C6 44 24 28 01 4C 8D 0D ?? ?? ?? ?? 41 B8 01 00 00 00 "
    "C6 44 24 20 00 E8 ?? ?? ?? ?? 48 8B 5C 24 40";
constexpr size_t kArrowPostClassCall = 37, kArrowPostSwitchCall = 0xC4, kArrowPostConst = 0xF4, kArrowPostCall = 0x106;
// The post helper both calls reach (exe+0x480DE20): mov [rsp+10h],rbp; mov [rsp+18h],rsi; mov [rsp+20h],rdi
constexpr const char* kPostHelper = "48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20";

constexpr uint32_t kRecArrowFlag = 0x1C, kRecSpellFlag = 0x1D, kRecArrow = 0x20, kRecTarget = 0x08;
constexpr uint32_t kObjFlags = 0x08, kObjClass = 0x10, kClassBases = 0x30, kClassDepth = 0x38;
// The switch setter's read of the hit component's owner (exe+0x486F707): mov rdi,[rdx+0A8h]; jmp
constexpr const char* kOwnerRead = "48 8B BA ?? ?? ?? ?? EB";
constexpr size_t kOwnerReadOff = 3, kOwnerReadWithin = 0x100;
constexpr uint32_t kDying = 0x60000000;  // RF_BeginDestroyed | RF_FinishDestroyed (the game's own "not usable" test)
constexpr uint64_t kWindowMs = 1000;      // one arrow's posts come within 5-300 ms (measured); 1 s covers hitches
constexpr int kRecent = 32;

using PostFn = uint64_t (*)(void* event, void* actor, uint64_t one, void* constant, uint64_t a5, uint64_t a6);
using SwitchFn = void (*)(void* shooter, void* comp, uint64_t index, void* actor);
using WeakGetFn = void* (*)(const void* weakPtr);
using ClassFn = const uint8_t* (*)();

struct Posted {
    const void* arrow;
    const void* shooter;
    const void* target;
    const void* event;
    uint64_t at;
    bool byHandler;  // posted on the arrow by the handler's redirected post (else by the arrow's own impact)
};

uint8_t* g_bowCall = nullptr;    // exe+0x485DB08 (now to the stub)
uint8_t* g_arrowCall = nullptr;  // exe+0x4860CE6 (now to the stub)
uint8_t* g_stub = nullptr;
PostFn g_post = nullptr;         // the post helper (exe+0x480DE20)
SwitchFn g_setSwitches = nullptr;  // impact type and material from a hit component onto an actor (exe+0x486F6B0)
void* g_bowConst = nullptr;      // the r9 constant of each call (the same object)
void* g_arrowConst = nullptr;
WeakGetFn g_weakGet = nullptr;   // FWeakObjectPtr::Get
ClassFn g_arrowClass = nullptr;  // the arrow's StaticClass
uint32_t g_playerFlag = 0;       // pawn +0xB20
uint32_t g_compOff = 0;          // pawn +0x790: the component the game takes impact type and material from
uint32_t g_compOwner = 0;        // component +0xA8: its owner (UActorComponent::OwnerPrivate, as the switch setter reads it)
std::atomic<bool> g_enabled{true};
std::atomic<uint64_t> g_skipped{0}, g_moved{0}, g_kept{0}, g_player{0}, g_arrowSkipped{0};
uint64_t g_skippedLogged = 0, g_movedLogged = 0, g_keptLogged = 0, g_playerLogged = 0, g_arrowSkippedLogged = 0;
SRWLOCK g_lock = SRWLOCK_INIT;
Posted g_recent[kRecent] = {};
int g_next = 0;
uint64_t (*g_now)() = &GetTickCount64;
std::atomic<int> g_lines{0};
constexpr int kLines = 20;

template <class T>
T Field(const void* p, uint32_t off)
{
    T v;
    memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof v);
    return v;
}

bool Usable(const void* obj) { return obj && (Field<uint32_t>(obj, kObjFlags) & kDying) == 0; }

// The game's IsA test: the object's class chain at the arrow class's depth is the arrow class.
bool IsArrow(const void* obj)
{
    if (!Usable(obj) || !g_arrowClass) return false;
    const uint8_t* cls = g_arrowClass();
    const uint8_t* objClass = Field<const uint8_t*>(obj, kObjClass);
    if (!cls || !objClass) return false;
    const int32_t depth = Field<int32_t>(cls, kClassDepth);
    if (depth < 0 || depth > Field<int32_t>(objClass, kClassDepth)) return false;
    const uint8_t* const* bases = Field<const uint8_t* const*>(objClass, kClassBases);
    return bases && bases[depth] == cls + kClassBases;
}

void Remember(const void* arrow, const void* shooter, const void* target, const void* event, bool byHandler)
{
    const uint64_t now = g_now();
    AcquireSRWLockExclusive(&g_lock);
    g_recent[g_next] = {arrow, shooter, target, event, now, byHandler};
    g_next = (g_next + 1) % kRecent;
    ReleaseSRWLockExclusive(&g_lock);
}

// True if this hit's thud (arrow, shooter, hit character, event) was posted on the arrow within kWindowMs (with
// handlerOnly: by the handler's redirected post).
bool AlreadyPosted(const void* arrow, const void* shooter, const void* target, const void* event, bool handlerOnly)
{
    const uint64_t now = g_now();
    bool found = false;
    AcquireSRWLockShared(&g_lock);
    for (const Posted& p : g_recent) {
        if (p.arrow == arrow && p.shooter == shooter && p.target == target && p.event == event && now - p.at < kWindowMs &&
            (!handlerOnly || p.byHandler)) {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}


// Lab: each NPC arrow hit's thud (moved to the arrow, or the arrow's own) is followed by its Wwise playing id until it ends
// (AK::SoundEngine::Query::GetEventIDFromPlayingID returns 0 once the event has stopped): one line per thud with its length,
// and a marker when a moved thud is not followed within 1 s by the arrow's own post (left out as already played), the case in
// which the arrow's own code never posted (the old engine alone saw the hit, or the arrow's +0x470 guard).
void LabTrack(uint32_t, const void*, bool) {}
void LabFollowed(const void*) {}

void Line(const char* what, const void* shooter, const void* weapon, const void* arrow, const void* target)
{
    if (g_lines.fetch_add(1, std::memory_order_relaxed) >= kLines) return;
    Log("[arrowhit] %s", what);
    (void)shooter; (void)weapon; (void)arrow; (void)target;
}

// The arrow's own post (stub: r8 = the hit component, r9 = the shooter).
uint64_t ArrowPostHook(void* event, void* actor, void* comp, void* shooter, uint64_t a5, uint64_t a6)
{
    if (!g_enabled.load(std::memory_order_relaxed) || !IsArrow(actor)) return g_post(event, actor, 1, g_arrowConst, a5, a6);
    const void* target = comp ? Field<const void*>(comp, g_compOwner) : nullptr;
    if (AlreadyPosted(actor, shooter, target, event, true)) {
        g_arrowSkipped.fetch_add(1, std::memory_order_relaxed);
        LabFollowed(actor);
        Line("arrow hit: the arrow's own impact sound not posted again (the hit's sound is already on the arrow)", shooter, nullptr,
             actor, target);
        return 0;  // the caller returns right after the post
    }
    const uint64_t r = g_post(event, actor, 1, g_arrowConst, a5, a6);
    if (static_cast<uint32_t>(r) != 0) {  // a playing id (32-bit)
        Remember(actor, shooter, target, event, false);
        LabTrack(static_cast<uint32_t>(r), actor, false);
    }
    return r;
}

// The handler's weapon post (stub: r8 = the hit record, r9 = the attacker).
uint64_t BowPostHook(void* event, void* actor, void* record, void* attacker, uint64_t a5, uint64_t a6)
{
    if (g_enabled.load(std::memory_order_relaxed) && record && attacker && Field<uint8_t>(record, kRecArrowFlag) != 0 &&
        Field<uint8_t>(record, kRecSpellFlag) == 0) {
        void* arrow = g_weakGet(static_cast<uint8_t*>(record) + kRecArrow);
        void* target = Field<void*>(record, kRecTarget);
        if (Field<uint8_t>(attacker, g_playerFlag) != 0) {
            g_player.fetch_add(1, std::memory_order_relaxed);
        } else if (IsArrow(arrow) && Usable(target)) {
            if (AlreadyPosted(arrow, attacker, target, event, false)) {
                g_skipped.fetch_add(1, std::memory_order_relaxed);
                Line("NPC arrow hit: the shooter's weapon's copy of the impact sound not posted (the hit's is on the arrow)",
                     attacker, actor, arrow, target);
                return 0;  // the handler ignores the result
            }
            void* comp = Field<void*>(target, g_compOff);
            if (comp) {
                g_setSwitches(attacker, comp, 0, arrow);  // as the arrow's own impact does before its post
                const uint64_t r = g_post(event, arrow, 1, g_bowConst, a5, a6);
                if (static_cast<uint32_t>(r) != 0) {  // a playing id (32-bit)
                    LabTrack(static_cast<uint32_t>(r), arrow, true);
                    Remember(arrow, attacker, target, event, true);
                    g_moved.fetch_add(1, std::memory_order_relaxed);
                    Line("NPC arrow hit: the impact sound posted on the arrow instead of the shooter's weapon", attacker, actor,
                         arrow, target);
                    return r;
                }
            }
        }
        if (Field<uint8_t>(attacker, g_playerFlag) == 0) {
            g_kept.fetch_add(1, std::memory_order_relaxed);
            Line("NPC arrow hit: the impact sound posted on the shooter's weapon as in the stock game (arrow gone or post failed)",
                 attacker, actor, arrow, target);
        }
    }
    return g_post(event, actor, 1, g_bowConst, a5, a6);
}

bool Fail(const char* why)
{
    Log("[arrowhit] %s: ArrowHitSoundFix not installed", why);
    return false;
}

uint8_t* Unique(const Section& s, const char* pattern, const char* what)
{
    const auto all = FindAll(s, ParsePattern(pattern));
    if (all.size() != 1) {
        Log("[arrowhit] %s: %zu matches", what, all.size());
        return nullptr;
    }
    return all[0];
}

}  // namespace

bool ApplyArrowHitSoundFix()
{
    if (g_stub) {
        Log("[arrowhit] fix already installed");
        return true;
    }
    const GameModule& game = Game();
    uint8_t* bow = Unique(game.text, kBowPost, "the hit handler's weapon post");
    uint8_t* arrowPost = Unique(game.text, kArrowPost, "the arrow's own impact post");
    uint8_t* flag = Unique(game.text, kPlayerFlag, "the shooter's Player switch flag");
    if (!bow || !arrowPost || !flag) return Fail("the arrow hit's code not found");
    uint8_t* bowCall = bow + kBowCall;
    uint8_t* arrowCall = arrowPost + kArrowPostCall;
    // the handler's other pieces, within the same function around the post
    uint8_t* target = nullptr;
    uint8_t* compRead = nullptr;
    uint8_t* branch = nullptr;
    for (uint8_t* p : FindAll(game.text, ParsePattern(kTarget)))
        if (p < bow && bow - p < 0x400) target = p;
    for (uint8_t* p : FindAll(game.text, ParsePattern(kComp)))
        if (target && p > target && p < bow) compRead = p;
    for (uint8_t* p : FindAll(game.text, ParsePattern(kArrowBranch)))
        if (p > bowCall && p - bowCall < 0x400) branch = p;
    if (!target || !compRead || !branch) return Fail("the hit record's fields not found next to the weapon post");
    uint8_t* post = RipTarget(bowCall, 1, 5);
    if (RipTarget(arrowCall, 1, 5) != post) return Fail("the two posts call different functions");
    if (!game.text.Contains(post) || !MatchesAt(post, ParsePattern(kPostHelper))) return Fail("the post helper starts differently");
    uint8_t* setSwitches = RipTarget(bow + kBowSwitchCall, 1, 5);
    if (RipTarget(arrowPost + kArrowPostSwitchCall, 1, 5) != setSwitches || !game.text.Contains(setSwitches))
        return Fail("the two posts set the impact switches with different functions");
    void* bowConst = RipTarget(bow + kBowConst, 3, 7);
    void* arrowConst = RipTarget(arrowPost + kArrowPostConst, 3, 7);
    if (bowConst != arrowConst || !game.data.Contains(static_cast<uint8_t*>(bowConst)))
        return Fail("the posts' constant argument differs or is not in .data");
    uint8_t* weakGet = RipTarget(branch + kWeakGetCall, 1, 5);
    uint8_t* arrowClass = RipTarget(branch + kArrowClassCall, 1, 5);
    if (!game.text.Contains(weakGet) || !game.text.Contains(arrowClass)) return Fail("FWeakObjectPtr::Get or the arrow class outside the code");
    if (RipTarget(arrowPost + kArrowPostClassCall, 1, 5) != arrowClass) return Fail("the two pieces test different arrow classes");
    uint32_t ownerOff = 0;
    {
        int found = 0;
        for (uint8_t* p : FindAll(game.text, ParsePattern(kOwnerRead)))
            if (p >= setSwitches && p < setSwitches + kOwnerReadWithin) {
                memcpy(&ownerOff, p + kOwnerReadOff, sizeof ownerOff);
                ++found;
            }
        if (found != 1 || ownerOff < 0x40 || ownerOff > 0x200 || (ownerOff & 7))
            return Fail("the switch setter's read of the component's owner not found");
    }
    uint32_t playerFlag = 0, compOff = 0;
    memcpy(&playerFlag, flag + kPlayerFlagOff, sizeof playerFlag);
    memcpy(&compOff, compRead + kCompOff, sizeof compOff);
    if (playerFlag < 0x100 || playerFlag > 0x4000 || compOff < 0x100 || compOff > 0x4000 || (compOff & 7))
        return Fail("the Player flag's or the hit component's offset is implausible");
    // Each stub: two movs, then jmp [rip+0]; dq hook
    //   bow:   mov r8,rdi (the record); mov r9,rbx (the attacker)
    //   arrow: mov r8,rbp (the hit component); mov r9,rdi (the shooter)
    uint8_t* stub = AllocNearGame(64);
    if (!stub) return Fail("no memory near the game's code");
    const uint8_t bowMov[6] = {0x49, 0x89, 0xF8, 0x49, 0x89, 0xD9};
    const uint8_t arrowMov[6] = {0x49, 0x89, 0xE8, 0x49, 0x89, 0xF9};
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* hooks[2] = {reinterpret_cast<void*>(&BowPostHook), reinterpret_cast<void*>(&ArrowPostHook)};
    const uint8_t* movs[2] = {bowMov, arrowMov};
    for (int i = 0; i < 2; ++i) {
        memcpy(stub + 32 * i, movs[i], 6);
        memcpy(stub + 32 * i + 6, jmp, sizeof jmp);
        memcpy(stub + 32 * i + 12, &hooks[i], sizeof(void*));
    }
    DWORD old;
    if (!VirtualProtect(stub, 64, PAGE_EXECUTE_READ, &old)) {
        FreeOwnCode(stub);
        return Fail("stub protection");
    }
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    const int64_t relBow = stub - (bowCall + 5), relArrow = stub + 32 - (arrowCall + 5);
    if (relBow != static_cast<int32_t>(relBow) || relArrow != static_cast<int32_t>(relArrow)) {
        FreeOwnCode(stub);
        return Fail("stub out of reach");
    }
    g_post = reinterpret_cast<PostFn>(post);
    g_setSwitches = reinterpret_cast<SwitchFn>(setSwitches);
    g_bowConst = g_arrowConst = bowConst;
    g_weakGet = reinterpret_cast<WeakGetFn>(weakGet);
    g_arrowClass = reinterpret_cast<ClassFn>(arrowClass);
    g_playerFlag = playerFlag;
    g_compOff = compOff;
    g_compOwner = ownerOff;
    uint8_t bowPatch[5]{0xE8}, arrowPatch[5]{0xE8};
    const int32_t b32 = static_cast<int32_t>(relBow), a32 = static_cast<int32_t>(relArrow);
    memcpy(bowPatch + 1, &b32, 4);
    memcpy(arrowPatch + 1, &a32, 4);
    // The arrow's call first: until the handler's call is redirected too, arrow posts are only remembered.
    if (!WriteCode(arrowCall, arrowPatch)) return Fail("could not write the arrow's call");
    if (!WriteCode(bowCall, bowPatch)) {
        uint8_t back[5]{0xE8};
        const int32_t o32 = static_cast<int32_t>(post - (arrowCall + 5));
        memcpy(back + 1, &o32, 4);
        WriteCode(arrowCall, back);
        return Fail("could not write the handler's call");
    }
    g_bowCall = bowCall;
    g_arrowCall = arrowCall;
    g_stub = stub;
    Log("[arrowhit] arrow hits: the hit handler's weapon post exe+0x%llX, the arrow's own post exe+0x%llX (helper exe+0x%llX, "
        "switch setter exe+0x%llX), weak get exe+0x%llX, arrow class exe+0x%llX, Player flag +0x%X, hit component +0x%X: NPC "
        "arrow hits heard once, at the hit: fix installed",
        static_cast<unsigned long long>(bowCall - game.base), static_cast<unsigned long long>(arrowCall - game.base),
        static_cast<unsigned long long>(post - game.base), static_cast<unsigned long long>(setSwitches - game.base),
        static_cast<unsigned long long>(weakGet - game.base), static_cast<unsigned long long>(arrowClass - game.base), playerFlag,
        compOff);
    return true;
}

void SetArrowHitSoundFixEnabled(bool enabled)
{
    if (!g_stub) return;
    if (g_enabled.exchange(enabled) != enabled) Log("[arrowhit] ArrowHitSoundFix %s", enabled ? "on" : "off");
}

void LogArrowHitSoundFixStats()
{
    const uint64_t skipped = g_skipped.load(), moved = g_moved.load(), kept = g_kept.load(), player = g_player.load(),
                   arrowSkipped = g_arrowSkipped.load();
    if (!g_stub || (skipped == g_skippedLogged && moved == g_movedLogged && kept == g_keptLogged && player == g_playerLogged &&
                    arrowSkipped == g_arrowSkippedLogged))
        return;
    Log("[arrowhit] NPC arrow hits: %llu weapon posts left out (the hit's sound already on the arrow), %llu moved to the arrow, "
        "%llu arrow posts not repeated, %llu left on the weapon (arrow gone or post failed); %llu of the player's unchanged",
        static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(moved),
        static_cast<unsigned long long>(arrowSkipped), static_cast<unsigned long long>(kept),
        static_cast<unsigned long long>(player));
    g_skippedLogged = skipped;
    g_movedLogged = moved;
    g_keptLogged = kept;
    g_playerLogged = player;
    g_arrowSkippedLogged = arrowSkipped;
}

bool ArrowHitSoundFixInfo(ArrowHitSoundInfo* out)
{
    if (!g_stub) return false;
    out->bowCall = g_bowCall;
    out->arrowCall = g_arrowCall;
    out->stub = g_stub;
    out->post = reinterpret_cast<void*>(g_post);
    out->setSwitches = reinterpret_cast<void*>(g_setSwitches);
    out->constant = g_bowConst;
    out->weakGet = reinterpret_cast<void*>(g_weakGet);
    out->arrowClass = reinterpret_cast<void*>(g_arrowClass);
    out->playerFlag = g_playerFlag;
    out->compOffset = g_compOff;
    out->ownerOffset = g_compOwner;
    out->bowHook = reinterpret_cast<void*>(&BowPostHook);
    out->arrowHook = reinterpret_cast<void*>(&ArrowPostHook);
    out->skipped = g_skipped.load();
    out->moved = g_moved.load();
    out->kept = g_kept.load();
    out->player = g_player.load();
    out->arrowSkipped = g_arrowSkipped.load();
    return true;
}

void ArrowHitSoundFixTestSetup(void* fakePost, void* fakeSwitches, void* fakeWeakGet, void* fakeClass, uint64_t (*clock)())
{
    static PostFn realPost = nullptr;
    static SwitchFn realSwitches = nullptr;
    static WeakGetFn realWeak = nullptr;
    static ClassFn realClass = nullptr;
    if (fakePost) {
        realPost = g_post;
        realSwitches = g_setSwitches;
        realWeak = g_weakGet;
        realClass = g_arrowClass;
        g_post = reinterpret_cast<PostFn>(fakePost);
        g_setSwitches = reinterpret_cast<SwitchFn>(fakeSwitches);
        g_weakGet = reinterpret_cast<WeakGetFn>(fakeWeakGet);
        g_arrowClass = reinterpret_cast<ClassFn>(fakeClass);
    } else if (realPost) {
        g_post = realPost;
        g_setSwitches = realSwitches;
        g_weakGet = realWeak;
        g_arrowClass = realClass;
    }
    g_now = clock ? clock : &GetTickCount64;
    AcquireSRWLockExclusive(&g_lock);
    memset(g_recent, 0, sizeof g_recent);
    g_next = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace opf
