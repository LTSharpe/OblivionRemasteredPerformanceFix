// BowSneakAttackFix: the player's bow sneak attacks land at 1x (and sometimes twice) (game bug).
//
// A player's arrow can reach the old engine's hit function (the shooter's virtual "hit with projectile", vtable
// +0x868; build 19115871 exe+0x65A5160, which calls the hit at +0x860, exe+0x65A5220: damage, the "Sneak attack for
// <n>X" message, hit counters, stagger, the target's reaction) along two paths:
//  - the Unreal side's: the arrow's impact on the Unreal side computes the shooter's sneak multiplier
//    (GetBaseSneakDamageMultiplierForCurrentWeapon: the weapon type's row of WeaponSneakDamageMultiplierDataTable,
//    column by Sneak) and queues it to the old engine (Arrow_hit_Reverse_Pairing); its handler (exe+0x65572E0) calls
//    the hit with that value (exe+0x65574B0, through the thunk exe+0x65A5200);
//  - the old engine's own projectile impact (exe+0x6665690), which still runs for the player's arrows and calls the
//    hit (exe+0x6665A43) with the multiplier fixed at 1.0.
// Whichever engine sees the impact first wins the race: Unreal's gives 2x/3x, the old engine's gives 1x ("Sneak attack
// for 1X damage", the sneak bonus lost), and when both see the same arrow the target is hit twice (1x + 2x = 3x in
// all). Measured with a probe on the target's health, 23 arrows: 10 Unreal only (2x), 9 old engine only (1x),
// 4 both (1x then 2x, about 8 ms apart). docs/research.md, "Bow sneak attacks at 1x".
//
// The fix, both on the OblivionThread (where both calls run), only for hits whose attacker is the player:
//  - one hit per arrow: each hit with an arrow is remembered (arrow and target, 500 ms); a second hit of the same
//    arrow on the same target, from either path, is not made (the first one already dealt the damage, the message,
//    the counters and the reaction);
//  - the old engine's hit uses the multiplier the Unreal side computed for the player's most recent arrow hit (the
//    weapon is a bow on this path, so the value only changes with the Sneak column) instead of 1.0. Until the first
//    Unreal-side arrow hit of a session there is no value, and the old engine's hit stays at 1.0 as in the stock game.
// The multiplier only matters on the hit function's sneak-attack branch; other hits are the same as before. Calls
// whose attacker is not the player go to the game's function unchanged.
#include "common.h"

#include <atomic>
#include <bit>
#include <cstring>
#include <windows.h>

namespace opf {
namespace {

// The Arrow_hit_Reverse_Pairing handler's hit call, build 19115871 exe+0x6557483 (in exe+0x65572E0):
//   48 85 F6 74 ??               test rsi,rsi; jz        rsi = attacker (Actor)
//   48 85 FF 74 ??               test rdi,rdi; jz        rdi = target (Actor)
//   48 85 DB 74 ??               test rbx,rbx; jz        rbx = arrow (ArrowProjectile)
//   C6 44 24 30 00               mov byte [rsp+30h],0
//   F3 0F 11 74 24 28            movss [rsp+28h],xmm6    the queued multiplier
//   C7 44 24 20 03 00 00 00      mov dword [rsp+20h],3
//   4C 8B CF 4C 8B C3 33 D2 48 8B CE
//   E8 [thunk]                   call exe+0x65A5200      <- redirected
//   48 8B D7 48 8B CB E8
constexpr const char* kQueue =
    "48 85 F6 74 ?? 48 85 FF 74 ?? 48 85 DB 74 ?? C6 44 24 30 00 F3 0F 11 74 24 28 C7 44 24 20 03 00 00 00 "
    "4C 8B CF 4C 8B C3 33 D2 48 8B CE E8 ?? ?? ?? ?? 48 8B D7 48 8B CB E8";
constexpr size_t kQueueCall = 45;
// The thunk (exe+0x65A5200): movzx eax,byte [rsp+38h]; mov r10,[rcx]; mov [rsp+38h],al; jmp [r10+868h]
constexpr const char* kThunk = "0F B6 44 24 38 4C 8B 11 88 44 24 38 49 FF A2 68 08 00 00";

// The old engine's projectile impact, build 19115871 exe+0x66659B8 (in exe+0x6665690): rsi = the projectile,
// r14 = the target, [rsi+168h] = the shooter.
//   48 8B 8E 68 01 00 00         mov rcx,[rsi+168h]
//   F3 44 0F 10 15 ????????      movss xmm10,[..]
//   48 85 C9 0F 84 ????????      test rcx,rcx; jz
//   48 3B 0D [player]            cmp rcx,[player]
//   74 ?? 48 8B 01 41 0F B6 D5 FF 90 F0 06 00 00 84 C0 75 ??
//   48 8B 8E 68 01 00 00 4D 8B CE 4C 8B C6 33 D2 48 8B 01 FF 90 70 08 00 00 E9 ????????
//   49 8B 06 33 D2 49 8B CE FF 90 60 03 00 00
//   48 8B 8E 68 01 00 00 4D 8B CE
//   F3 0F 10 05 [one]            movss xmm0,[1.0]        the fixed multiplier
//   4C 8B C6 44 88 64 24 30 33 D2 F3 0F 11 44 24 28 0F B6 D8 4C 8B 11 44 89 64 24 20
//   41 FF 92 68 08 00 00         call [r10+868h]         <- call stub; nop
//   84 DB
constexpr const char* kImpact =
    "48 8B 8E 68 01 00 00 F3 44 0F 10 15 ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 48 3B 0D ?? ?? ?? ?? "
    "74 ?? 48 8B 01 41 0F B6 D5 FF 90 F0 06 00 00 84 C0 75 ?? "
    "48 8B 8E 68 01 00 00 4D 8B CE 4C 8B C6 33 D2 48 8B 01 FF 90 70 08 00 00 E9 ?? ?? ?? ?? "
    "49 8B 06 33 D2 49 8B CE FF 90 60 03 00 00 48 8B 8E 68 01 00 00 4D 8B CE F3 0F 10 05 ?? ?? ?? ?? "
    "4C 8B C6 44 88 64 24 30 33 D2 F3 0F 11 44 24 28 0F B6 D8 4C 8B 11 44 89 64 24 20 41 FF 92 68 08 00 00 84 DB";
constexpr size_t kImpactPlayer = 25, kImpactOne = 104, kImpactCall = 139;
constexpr size_t kHitSlot = 0x868;

// The hit with a projectile (vtable +0x868): rcx attacker, dl, r8 arrow, r9 target, then kind, multiplier, flag.
using HitFn = uint64_t (*)(void* attacker, uint64_t a2, void* arrow, void* target, uint64_t kind, float mult, uint64_t flag);

constexpr uint64_t kSameArrowMs = 500;
constexpr int kRecent = 16;

struct Recent {
    const void* arrow;
    const void* target;
    uint64_t at;
};

uint8_t* g_queueCall = nullptr;   // the handler's call (now to the stub)
uint8_t* g_impactCall = nullptr;  // the impact's call [r10+868h] (now call stub; nop)
uint8_t* g_stub = nullptr;
HitFn g_thunk = nullptr;          // exe+0x65A5200
void* const* g_player = nullptr;  // the old engine's player pointer
std::atomic<bool> g_enabled{true};
std::atomic<uint32_t> g_multBits{0};      // the Unreal side's last multiplier for the player's arrow hit (0: none yet)
std::atomic<uintptr_t> g_arrowVtable{0};  // the vtable of the arrows those hits carried
std::atomic<uint64_t> g_skipped{0}, g_corrected{0}, g_oneOnly{0};
uint64_t g_skippedLogged = 0, g_correctedLogged = 0;
SRWLOCK g_lock = SRWLOCK_INIT;
Recent g_recent[kRecent] = {};
int g_next = 0;
uint64_t (*g_now)() = &GetTickCount64;    // the self-test steps a fake clock
std::atomic<int> g_hitLines{0};

// One log line per player arrow hit (the first kHitLines of a session), so the fix can be checked in play.
constexpr int kHitLines = 200;
void LogHit(const char* path, const void* arrow, const void* target, float in, float used, const char* what)
{
    if (g_hitLines.fetch_add(1, std::memory_order_relaxed) >= kHitLines) return;
    Log("[bowsneak] hit: %s arrow=%p target=%p multiplier %.2f -> %.2f: %s", path, arrow, target, in, used, what);
}

// True if this arrow already hit this target within kSameArrowMs; else remembers it.
bool SeenElseRemember(const void* arrow, const void* target)
{
    const uint64_t now = g_now();
    AcquireSRWLockExclusive(&g_lock);
    for (const Recent& r : g_recent) {
        if (r.arrow == arrow && r.target == target && now - r.at < kSameArrowMs) {
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    }
    g_recent[g_next] = {arrow, target, now};
    g_next = (g_next + 1) % kRecent;
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

uintptr_t Vtable(const void* object) { return *static_cast<const uintptr_t*>(object); }

// The Unreal side's hit (the handler's call through the thunk).
uint64_t QueueHook(void* attacker, uint64_t a2, void* arrow, void* target, uint64_t kind, float mult, uint64_t flag)
{
    if (g_enabled.load(std::memory_order_relaxed) && attacker == *g_player && arrow && target) {
        g_multBits.store(std::bit_cast<uint32_t>(mult), std::memory_order_relaxed);
        g_arrowVtable.store(Vtable(arrow), std::memory_order_relaxed);
        if (SeenElseRemember(arrow, target)) {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            LogHit("Unreal side", arrow, target, mult, mult, "second hit of this arrow, not made");
            return 0;  // the handler ignores the result
        }
        LogHit("Unreal side", arrow, target, mult, mult, "made");
    }
    return g_thunk(attacker, a2, arrow, target, kind, mult, flag);
}

// The old engine's own impact (its call through the attacker's vtable).
uint64_t ImpactHook(void* attacker, uint64_t a2, void* arrow, void* target, uint64_t kind, float mult, uint64_t flag)
{
    const auto hit = reinterpret_cast<HitFn>((*static_cast<void* const* const*>(attacker))[kHitSlot / sizeof(void*)]);
    if (g_enabled.load(std::memory_order_relaxed) && attacker == *g_player && arrow && target) {
        if (SeenElseRemember(arrow, target)) {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            LogHit("old engine", arrow, target, mult, mult, "second hit of this arrow, not made");
            return 0;  // the caller ignores the result
        }
        const float in = mult;
        const uint32_t bits = g_multBits.load(std::memory_order_relaxed);
        if (bits && mult == 1.0f && Vtable(arrow) == g_arrowVtable.load(std::memory_order_relaxed)) {
            mult = std::bit_cast<float>(bits);
            g_corrected.fetch_add(1, std::memory_order_relaxed);
            LogHit("old engine", arrow, target, in, mult, "made with the Unreal side's multiplier");
        } else {
            g_oneOnly.fetch_add(1, std::memory_order_relaxed);
            LogHit("old engine", arrow, target, in, mult, "made unchanged (no Unreal-side value yet, or another projectile)");
        }
    }
    return hit(attacker, a2, arrow, target, kind, mult, flag);
}

bool Fail(const char* why)
{
    Log("[bowsneak] %s: fix not installed", why);
    return false;
}

}  // namespace

bool ApplyBowSneakAttackFix()
{
    if (g_stub) {
        Log("[bowsneak] fix already installed");
        return true;
    }
    const GameModule& game = Game();
    const auto queues = FindAll(game.text, ParsePattern(kQueue));
    const auto impacts = FindAll(game.text, ParsePattern(kImpact));
    if (queues.size() != 1 || impacts.size() != 1) {
        Log("[bowsneak] the arrow hit's code not found (%zu / %zu matches): fix not installed", queues.size(), impacts.size());
        return false;
    }
    uint8_t* queueCall = queues[0] + kQueueCall;
    uint8_t* impactCall = impacts[0] + kImpactCall;
    uint8_t* thunk = RipTarget(queueCall, 1, 5);
    if (!game.text.Contains(thunk) || !MatchesAt(thunk, ParsePattern(kThunk))) return Fail("the handler's call does not reach the hit thunk");
    const uint8_t* player = RipTarget(impacts[0] + kImpactPlayer, 3, 7);
    const uint8_t* one = RipTarget(impacts[0] + kImpactOne, 4, 8);
    float oneValue = 0;
    if (!game.data.Contains(player)) return Fail("the player pointer is not in .data");
    if (!game.rdata.Contains(one)) return Fail("the impact's multiplier constant is not in .rdata");
    memcpy(&oneValue, one, sizeof oneValue);
    if (oneValue != 1.0f) return Fail("the impact's multiplier is not 1.0");
    // Each stub: jmp [rip+0]; dq hook
    uint8_t* stub = AllocNearGame(32);
    if (!stub) return Fail("no memory near the game's code");
    const uint8_t jmp[] = {0xFF, 0x25, 0, 0, 0, 0};
    void* hooks[2] = {reinterpret_cast<void*>(&QueueHook), reinterpret_cast<void*>(&ImpactHook)};
    for (int i = 0; i < 2; ++i) {
        memcpy(stub + 16 * i, jmp, sizeof jmp);
        memcpy(stub + 16 * i + sizeof jmp, &hooks[i], sizeof(void*));
    }
    DWORD old;
    if (!VirtualProtect(stub, 32, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return Fail("stub protection");
    }
    FlushInstructionCache(GetCurrentProcess(), stub, 32);
    const int64_t relQueue = stub - (queueCall + 5), relImpact = stub + 16 - (impactCall + 5);
    if (relQueue != static_cast<int32_t>(relQueue) || relImpact != static_cast<int32_t>(relImpact)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return Fail("stub out of reach");
    }
    g_thunk = reinterpret_cast<HitFn>(thunk);
    g_player = reinterpret_cast<void* const*>(player);
    uint8_t queuePatch[5]{0xE8};
    const int32_t q32 = static_cast<int32_t>(relQueue);
    memcpy(queuePatch + 1, &q32, 4);
    uint8_t impactPatch[7]{0xE8, 0, 0, 0, 0, 0x66, 0x90};  // call stub; 2-byte nop (the original call is 7 bytes)
    const int32_t i32 = static_cast<int32_t>(relImpact);
    memcpy(impactPatch + 1, &i32, 4);
    // The impact's call first: until the handler's call is redirected too, the old engine's hits are only remembered.
    if (!WriteCode(impactCall, impactPatch)) return Fail("could not write the impact's call");
    if (!WriteCode(queueCall, queuePatch)) {
        const uint8_t back[7] = {0x41, 0xFF, 0x92, 0x68, 0x08, 0x00, 0x00};
        WriteCode(impactCall, back);
        return Fail("could not write the handler's call");
    }
    g_queueCall = queueCall;
    g_impactCall = impactCall;
    g_stub = stub;
    Log("[bowsneak] arrow hits: Unreal side's call exe+0x%llX (thunk exe+0x%llX), old engine's impact call exe+0x%llX (fixed 1.0 "
        "at exe+0x%llX), player at exe+0x%llX: one hit per arrow, the old engine's with the Unreal side's multiplier: fix installed",
        static_cast<unsigned long long>(queueCall - game.base), static_cast<unsigned long long>(thunk - game.base),
        static_cast<unsigned long long>(impactCall - game.base), static_cast<unsigned long long>(one - game.base),
        static_cast<unsigned long long>(player - game.base));
    return true;
}

void SetBowSneakAttackFixEnabled(bool enabled) { g_enabled = enabled; }

void LogBowSneakAttackFixStats()
{
    const uint64_t skipped = g_skipped.load(), corrected = g_corrected.load();
    if (!g_stub || (skipped == g_skippedLogged && corrected == g_correctedLogged)) return;
    Log("[bowsneak] player arrow hits: %llu second hits of the same arrow not made, %llu old-engine hits with the Unreal side's "
        "multiplier, %llu at 1.0 (no value yet)",
        static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(corrected),
        static_cast<unsigned long long>(g_oneOnly.load()));
    g_skippedLogged = skipped;
    g_correctedLogged = corrected;
}

bool BowSneakAttackFixInfo(BowSneakInfo* out)
{
    if (!g_stub) return false;
    out->queueCall = g_queueCall;
    out->impactCall = g_impactCall;
    out->stub = g_stub;
    out->thunk = reinterpret_cast<void*>(g_thunk);
    out->player = g_player;
    out->queueHook = reinterpret_cast<void*>(&QueueHook);
    out->impactHook = reinterpret_cast<void*>(&ImpactHook);
    out->skipped = g_skipped.load();
    out->corrected = g_corrected.load();
    out->oneOnly = g_oneOnly.load();
    return true;
}

void BowSneakAttackFixTestSetup(void* fakeThunk, void* const* fakePlayer, uint64_t (*clock)())
{
    if (fakeThunk) g_thunk = reinterpret_cast<HitFn>(fakeThunk);
    if (fakePlayer) g_player = fakePlayer;
    g_now = clock ? clock : &GetTickCount64;
    g_multBits = 0;
    g_arrowVtable = 0;
    AcquireSRWLockExclusive(&g_lock);
    memset(g_recent, 0, sizeof g_recent);
    g_next = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace opf
