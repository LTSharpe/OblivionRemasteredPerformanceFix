// ClothBuildDefer ([Fixes] ClothBuildDefer=1; a timing change of NpcCreationPacing's kind): a far, unseen NPC's first
// cloth simulation build waits a few frames, and the Blueprint's repeated requests meanwhile become one build.
//
// Found (2026-10-19/20, stack samples at Peryite's Shrine, npcburst): most of a robed NPC's creation unit is
// UChaosClothComponent::RecreateClothSimulationProxy building a whole FClothSimulationProxy (Chaos solver, particles,
// constraints, graph colouring), a few ms per robe, and the character Blueprints ask for it several times: per robe an
// OnRegister build, a script rebuild ~5 ms later and another 40-90 ms later after the proxy ran 1-3 ticks, each inside
// the NPC's creation unit (2026-10-20: 15 builds, 78.5 ms on the npcburst-short flight). Every rebuild starts from
// nothing (RecreateClothSimulationProxy first resets the proxy), so only the last build's result survives.
//
// The rule: a request is deferred when it comes on the game thread inside a NpcCreationPacing unit whose actor may wait
// (far, not rendered recently, a frame that defers) and the component is registered and has no proxy (its first build,
// or all its builds so far deferred). A robe is built inside an equipment unit, whose owner is the character's body
// pairing component: pacing's own walk ends there and runs the unit as near, so this question walks on past it to the
// character (npc_pacing.cpp, ActorLocationGuarded). Later requests from such units merge into the waiting build; a
// request from anywhere else builds at once, as the game would. The waiting build runs, with the game's own function
// and the component's state at that moment, at NpcCreationPacing's frame start:
//   - when no request came for ClothDeferQuietFrames frames (or kForceFrames after the first), inside the pacing budget:
//     as the frame's first unit or when it fits (the budget grows with the wait as pacing's does); it then counts as a
//     unit of that frame, so pacing's later units of the frame wait for the budget too;
//   - at once when the actor is near or rendered, the frame does not defer (switched off, travel, menu, loading
//     screen), or 2 x kForceFrames frames after the first request;
//   - at once before a Blueprint reads the simulation state (IsSimulationSuspended / IsSimulationEnabled: both answer
//     from the proxy), so script logic sees the stock answer.
// Until then the component is as a registered one before its first build (stock state right after Super::OnRegister):
// TickComponent skips the proxy tick (null check), CalcBounds gives empty bounds (as stock gives at registration), the
// render data falls back to plain skinning, OnUnregister resets nothing. OnUnregister (vtable slot) drops a waiting
// build, so a destroyed component is never built. The cloth starts simulating a few frames later, off-screen.
//
// With ClothRegisterLite (npcs/cloth_register.cpp, installed first; this file's hooks then run inside its own): a build
// this file holds leaves no proxy, so ClothRegisterLite has nothing to complete at its EquipBodyPart call's end, and the
// build made later runs outside any such call: a full one. ClothRegisterLite's OnUnregister hook sits in the vtable
// slot: this file's goes in front of it (ClothRegisterUnregisterSlot).
//
// Sites (build 19115871; each found by pattern and checked; anything unexpected: not installed, one log line):
//   RecreateClothSimulationProxy exe+0x53CD090: push rdi; sub rsp,40h; test byte [rcx+8Ch],1 (IsRegistered); ...
//     lea r14,[rcx+940h] (ClothSimulationProxy). Entry detour: the 6 prologue bytes move to a trampoline.
//   OnUnregister exe+0x53CCF80 (vtable slot exe+0x803EF20): call Super::OnUnregister; lea rcx,[rbp+940h]; reset.
//   execIsSimulationSuspended exe+0x53D00B0: P_FINISH; call IsSimulationSuspended exe+0x53CCDB0 (test byte
//     [rbx+918h],8; jne; cmp qword [rbx+940h],0; je ...: suspended or no proxy); that call is redirected.
//   execIsSimulationEnabled exe+0x53CFFC0: P_FINISH; ... test byte [rbx+918h],4; cmp qword [rbx+940h],0 (inline).
//     Entry detour (5 prologue bytes).
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <unordered_map>
#include <vector>

namespace opf {
namespace {

constexpr char kRecreate[] = "40 57 48 83 EC 40 F6 81 8C 00 00 00 01 48 8B F9 0F 84";
constexpr uint8_t kRecreatePrologue[6] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x40};
constexpr char kRecreateProxyField[] = "4C 8D B1 40 09 00 00";
constexpr char kUnregister[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B E9 E8 ?? ?? ?? ?? 48 8D 8D 40 09 00 00 0F 57 C0";
// a UFUNCTION thunk returning a bool member call: P_FINISH, then the member call's result stored to the result slot
constexpr char kBoolThunk[] =
    "40 53 48 83 EC 20 48 8B 42 20 45 33 C9 48 85 C0 49 8B D8 41 0F 95 C1 4C 03 C8 4C 89 4A 20 E8 ?? ?? ?? ?? 88 03 48 83 C4 20 5B C3";
constexpr size_t kBoolThunkCall = 30;
constexpr char kSuspendedBody[] = "F6 83 18 09 00 00 08 75 ?? 48 83 BB 40 09 00 00 00 74";   // bSuspendSimulation; proxy
constexpr char kEnabledThunk[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 42 20 45 33 C9 48 85 C0 48 8B D9 B9 ?? ?? ?? ?? 49 8B F8 41 0F 95 C1 4C 03 C8 "
    "65 48 8B 04 25 58 00 00 00 4C 89 4A 20 8B 15 ?? ?? ?? ?? 48 8B 14 D0 8B 04 11 39 05 ?? ?? ?? ?? 7F ?? "
    "F6 83 18 09 00 00 04 74 ?? 48 83 BB 40 09 00 00 00 74";
constexpr uint8_t kEnabledPrologue[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr size_t kProxyField = 0x940;
constexpr size_t kRegisteredByte = 0x8C;   // bRegistered (bit 0), RecreateClothSimulationProxy's own first check
constexpr uint64_t kForceFrames = 30;      // a waiting build is due this many frames after its first request (budget
                                           // permitting), and runs whatever the budget after twice as many
constexpr uint64_t kScaleFrom = 8;         // from this wait on the budget grows (x2 at 12 frames, x3 at 16, ...)

using RecreateFn = void (*)(uint8_t* component);
using UnregisterFn = void (*)(uint8_t* component);
using SuspendedFn = bool (*)(uint8_t* component);
using ThunkFn = void (*)(uint8_t* context, void* stack, void* result);

RecreateFn g_recreateOrig = nullptr;
UnregisterFn g_unregOrig = nullptr;
SuspendedFn g_suspendedFn = nullptr;
ThunkFn g_enabledOrig = nullptr;
uint8_t* g_region = nullptr;
bool g_installed = false;
uint8_t* g_recreateSite = nullptr;   // the detoured entry (self-test)
void** g_unregSlot = nullptr;
uint8_t* g_suspendedCall = nullptr;
uint8_t* g_enabledThunk = nullptr;
std::atomic<bool> g_on{false};
std::atomic<int> g_quietFrames{6};
uint64_t g_freq = 0;
DWORD g_gtId = 0;
int g_testDistant = -1;   // self-test: -1 = NpcCreationPacing decides, 0/1 forced
uint64_t g_testFrame = 0;

// The builds that wait (game thread; OnUnregister may come from elsewhere, hence the lock)
struct Pend {
    uint64_t owner = 0;             // the pacing unit's owner (weak object pointer)
    uint64_t first = 0, last = 0;   // pacing frames of the first and the latest request
    uint32_t requests = 0;
    uint8_t head[16] = {};          // vtable, flags, object index at the first request (vtable and index checked before building)
};
SRWLOCK g_pendLock = SRWLOCK_INIT;
std::unordered_map<const uint8_t*, Pend> g_pend;
std::atomic<size_t> g_pendCount{0};
enum Why { kQuiet, kSeen, kLag, kFlush, kReader, kNumWhy };
std::atomic<uint64_t> g_deferred{0}, g_merged{0}, g_cancelled{0}, g_asked{0}, g_odd{0}, g_builtUs{0}, g_maxFrameUs{0};
std::atomic<uint64_t> g_by[kNumWhy]{};
double g_estUs = 6000;   // running average of a deferred build's time

uint64_t NowUs()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart) * 1000000ull / g_freq;
}

bool Copy(const uint8_t* from, uint8_t* to, size_t n)
{
    __try { memcpy(to, from, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint8_t* ReadProxy(const uint8_t* component)
{
    __try { return *reinterpret_cast<uint8_t* const*>(component + kProxyField); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool Registered(const uint8_t* component)
{
    __try { return (component[kRegisteredByte] & 1) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The game thread is the one Unreal names "GameThread" (remembered at its first call here).
bool OnGameThread()
{
    if (g_gtId) return GetCurrentThreadId() == g_gtId;
    PWSTR name = nullptr;
    bool gt = false;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &name)) && name) {
        gt = wcscmp(name, L"GameThread") == 0;
        LocalFree(name);
    }
    if (gt) g_gtId = GetCurrentThreadId();
    return gt;
}

bool DistantUnit(uint64_t* owner, uint64_t* frame)
{
    if (g_testDistant >= 0) {
        *owner = 1;
        *frame = g_testFrame;
        return g_testDistant != 0;
    }
    return NpcPacingDistantUnit(owner, frame);
}

bool StillDistant(uint64_t owner) { return g_testDistant >= 0 ? g_testDistant != 0 : NpcPacingStillDistant(owner); }

bool IsPending(const uint8_t* component)
{
    if (!g_pendCount.load(std::memory_order_relaxed)) return false;
    AcquireSRWLockShared(&g_pendLock);
    const bool p = g_pend.count(component) != 0;
    ReleaseSRWLockShared(&g_pendLock);
    return p;
}

// Removes a waiting build; true if there was one (*out: what it was).
bool Take(const uint8_t* component, Pend* out)
{
    if (!g_pendCount.load(std::memory_order_relaxed)) return false;
    AcquireSRWLockExclusive(&g_pendLock);
    auto it = g_pend.find(component);
    const bool found = it != g_pend.end();
    if (found) {
        if (out) *out = it->second;
        g_pend.erase(it);
        g_pendCount = g_pend.size();
    }
    ReleaseSRWLockExclusive(&g_pendLock);
    return found;
}

void Defer(const uint8_t* component, uint64_t owner, uint64_t frame)
{
    Pend p;
    p.owner = owner;
    p.first = p.last = frame;
    p.requests = 1;
    Copy(component, p.head, sizeof p.head);
    AcquireSRWLockExclusive(&g_pendLock);
    g_pend[component] = p;
    g_pendCount = g_pend.size();
    ReleaseSRWLockExclusive(&g_pendLock);
}

void Merge(const uint8_t* component, uint64_t frame)
{
    AcquireSRWLockExclusive(&g_pendLock);
    auto it = g_pend.find(component);
    if (it != g_pend.end()) {
        it->second.last = frame;
        ++it->second.requests;
    }
    ReleaseSRWLockExclusive(&g_pendLock);
}

// Whether a taken waiting build may run: the component is still the registered object it was (same vtable and object
// index; its flags may change), with no proxy. Otherwise the game's state already differs: nothing to build for it.
bool StillAsDeferred(const uint8_t* component, const Pend& p)
{
    uint8_t head[16];
    if (!Copy(component, head, sizeof head) || memcmp(head, p.head, 8) != 0 || memcmp(head + 12, p.head + 12, 4) != 0 ||
        !Registered(component) || ReadProxy(component)) {
        ++g_odd;
        return false;
    }
    return true;
}

// The game's build of a taken waiting build; returns its time in us.
uint64_t BuildTaken(uint8_t* component, int why)
{
    const uint64_t t0 = NowUs();
    g_recreateOrig(component);
    const uint64_t us = NowUs() - t0;
    ++g_by[why];
    g_builtUs += us;
    if (why == kQuiet) g_estUs = g_estUs * 0.75 + static_cast<double>(us) * 0.25;
    return us;
}

// A Blueprint asking whether the simulation runs gets the stock answer: a waiting build is made first.
void BuildNowIfPending(uint8_t* component)
{
    if (!g_pendCount.load(std::memory_order_relaxed) || !OnGameThread()) return;
    Pend p;
    if (!Take(component, &p) || !StillAsDeferred(component, p)) return;
    BuildTaken(component, kReader);
}

void UnregisterHook(uint8_t* component)
{
    if (g_pendCount.load(std::memory_order_relaxed) && Take(component, nullptr)) ++g_cancelled;
    g_unregOrig(component);
}

bool SuspendedHook(uint8_t* component)
{
    BuildNowIfPending(component);
    return g_suspendedFn(component);
}

void EnabledHook(uint8_t* context, void* stack, void* result)
{
    BuildNowIfPending(context);
    g_enabledOrig(context, stack, result);
}

void RecreateHook(uint8_t* component)
{
    const bool on = g_on.load(std::memory_order_relaxed);
    if ((!on && !g_pendCount.load(std::memory_order_relaxed)) || !OnGameThread()) { g_recreateOrig(component); return; }
    uint64_t owner = 0, frame = 0;
    const bool farUnit = on && DistantUnit(&owner, &frame);
    if (IsPending(component)) {
        if (farUnit) {   // one more request from a far unit: merges into the waiting build
            Merge(component, frame);
            ++g_merged;
            return;
        }
        Take(component, nullptr);   // asked for from anywhere else: built now, as the game would
        ++g_asked;
    } else if (farUnit && Registered(component) && !ReadProxy(component)) {
        Defer(component, owner, frame);
        ++g_deferred;
        return;
    }
    g_recreateOrig(component);
}

// The only 8-byte-aligned .rdata slot holding this address (a vtable slot), or nullptr.
void** OnlySlot(const GameModule& game, const void* target)
{
    void** found = nullptr;
    uint8_t* p = game.rdata.begin + ((8 - reinterpret_cast<uintptr_t>(game.rdata.begin) % 8) % 8);
    for (; p + 8 <= game.rdata.end; p += 8) {
        if (*reinterpret_cast<void**>(p) != target) continue;
        if (found) return nullptr;
        found = reinterpret_cast<void**>(p);
    }
    return found;
}

bool Fail(const char* why)
{
    Log("[clothdefer] %s: ClothBuildDefer not installed", why);
    return false;
}

}  // namespace

bool InstallClothBuildDefer(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!beforeGameCode) return Fail("only at start-up");
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = static_cast<uint64_t>(f.QuadPart);
    const auto& game = Game();

    const auto rec = FindAll(game.text, ParsePattern(kRecreate));
    if (rec.size() != 1) return Fail("RecreateClothSimulationProxy not found exactly once");
    uint8_t* recreate = rec[0];
    if (memcmp(recreate, kRecreatePrologue, sizeof kRecreatePrologue) != 0) return Fail("unexpected prologue");
    if (FindAll({recreate, recreate + 0x40}, ParsePattern(kRecreateProxyField)).size() != 1) return Fail("proxy field +0x940 unconfirmed");

    const auto unreg = FindAll(game.text, ParsePattern(kUnregister));
    if (unreg.size() != 1) return Fail("OnUnregister not found exactly once");
    g_unregSlot = OnlySlot(game, unreg[0]);
    if (!g_unregSlot) g_unregSlot = ClothRegisterUnregisterSlot();   // ClothRegisterLite's hook holds it: ours goes in front
    if (!g_unregSlot) return Fail("OnUnregister's vtable slot not found exactly once");
    for (uint8_t* t : FindAll(game.text, ParsePattern(kBoolThunk))) {
        uint8_t* call = t + kBoolThunkCall;
        uint8_t* fn = RipTarget(call, 1, 5);
        if (!game.text.Contains(fn) || FindAll({fn, fn + 0x40}, ParsePattern(kSuspendedBody)).size() != 1) continue;
        if (g_suspendedCall) return Fail("execIsSimulationSuspended found twice");
        g_suspendedCall = call;
        g_suspendedFn = reinterpret_cast<SuspendedFn>(fn);
    }
    if (!g_suspendedCall) return Fail("execIsSimulationSuspended not found");
    const auto en = FindAll(game.text, ParsePattern(kEnabledThunk));
    if (en.size() != 1 || memcmp(en[0], kEnabledPrologue, sizeof kEnabledPrologue) != 0)
        return Fail("execIsSimulationEnabled not found exactly once");
    g_enabledThunk = en[0];

    g_region = AllocNearGame(256);
    if (!g_region) return Fail("no memory near the game for the trampolines");
    // From here on a failure puts back what was written and gives the region back (nothing points at it then)
    uint8_t suspBefore[5], enabledBefore[5];
    memcpy(suspBefore, g_suspendedCall, sizeof suspBefore);
    memcpy(enabledBefore, g_enabledThunk, sizeof enabledBefore);
    void* const slotBefore = *g_unregSlot;
    int written = 0;   // 1 = the suspended call, 2 = and the enabled thunk, 3 = and OnUnregister's slot
    auto undo = [&](const char* why) {
        if (written >= 3) WritePointer(g_unregSlot, slotBefore);
        if (written >= 2) WriteCode(g_enabledThunk, enabledBefore);
        if (written >= 1) WriteCode(g_suspendedCall, suspBefore);
        VirtualFree(g_region, 0, MEM_RELEASE);
        g_region = nullptr;
        return Fail(why);
    };
    // [0] trampoline: the 6 prologue bytes + jmp [rip] -> recreate+6; [32] jmp [rip] -> RecreateHook; [96] -> SuspendedHook;
    // [128] trampoline: execIsSimulationEnabled's 5 prologue bytes + jmp [rip] -> +5; [160] -> EnabledHook
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* resume = recreate + sizeof kRecreatePrologue;
    uint8_t* enabledResume = g_enabledThunk + sizeof kEnabledPrologue;
    void* const hooks[3] = {reinterpret_cast<void*>(&RecreateHook), reinterpret_cast<void*>(&SuspendedHook),
                            reinterpret_cast<void*>(&EnabledHook)};
    memcpy(g_region, kRecreatePrologue, sizeof kRecreatePrologue);
    memcpy(g_region + 6, jmpAbs, 6);
    memcpy(g_region + 12, &resume, 8);
    memcpy(g_region + 128, kEnabledPrologue, sizeof kEnabledPrologue);
    memcpy(g_region + 128 + 5, jmpAbs, 6);
    memcpy(g_region + 128 + 11, &enabledResume, 8);
    const size_t hookAt[3] = {32, 96, 160};
    for (int i = 0; i < 3; ++i) {
        memcpy(g_region + hookAt[i], jmpAbs, 6);
        memcpy(g_region + hookAt[i] + 6, &hooks[i], 8);
    }
    DWORD old;
    if (!VirtualProtect(g_region, 256, PAGE_EXECUTE_READ, &old)) return undo("trampoline protection");
    FlushInstructionCache(GetCurrentProcess(), g_region, 256);
    auto rel = [](const uint8_t* to, const uint8_t* insn) { return static_cast<int64_t>(to - (insn + 5)); };
    const int64_t relEntry = rel(g_region + 32, recreate), relSusp = rel(g_region + 96, g_suspendedCall),
                  relEn = rel(g_region + 160, g_enabledThunk);
    if (relEntry != static_cast<int32_t>(relEntry) || relSusp != static_cast<int32_t>(relSusp) ||
        relEn != static_cast<int32_t>(relEn))
        return undo("trampolines out of reach");
    g_recreateOrig = reinterpret_cast<RecreateFn>(g_region);
    g_enabledOrig = reinterpret_cast<ThunkFn>(g_region + 128);
    g_unregOrig = reinterpret_cast<UnregisterFn>(*g_unregSlot);   // the game's OnUnregister, or ClothRegisterLite's hook of it
    auto rel32 = [](uint8_t op, int64_t r) {
        std::vector<uint8_t> b(5, op);
        const int32_t r32 = static_cast<int32_t>(r);
        memcpy(b.data() + 1, &r32, 4);
        return b;
    };
    // the readers and OnUnregister first: a waiting build can only exist once the entry detour is in
    if (!WriteCode(g_suspendedCall, rel32(0xE8, relSusp))) return undo("execIsSimulationSuspended's call patch failed");
    written = 1;
    if (!WriteCode(g_enabledThunk, rel32(0xE9, relEn))) return undo("execIsSimulationEnabled patch failed");
    written = 2;
    if (!WritePointer(g_unregSlot, reinterpret_cast<void*>(&UnregisterHook))) return undo("OnUnregister slot patch failed");
    written = 3;
    if (!WriteCode(recreate, rel32(0xE9, relEntry))) return undo("entry patch failed");
    g_recreateSite = recreate;
    g_installed = true;
    Log("[clothdefer] RecreateClothSimulationProxy exe+0x%X, OnUnregister slot exe+0x%X%s, execIsSimulationSuspended's call "
        "exe+0x%X, execIsSimulationEnabled exe+0x%X: installed",
        static_cast<unsigned>(recreate - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(g_unregSlot) - game.base),
        g_unregOrig == reinterpret_cast<UnregisterFn>(unreg[0]) ? "" : " (in front of ClothRegisterLite's hook)",
        static_cast<unsigned>(g_suspendedCall - game.base), static_cast<unsigned>(g_enabledThunk - game.base));
    return true;
}

void SetClothBuildDefer(bool on, int quietFrames)
{
    if (!g_installed) return;
    const int quiet = std::clamp(quietFrames, 1, static_cast<int>(kForceFrames));
    const bool changed = g_on.exchange(on) != on;
    const bool quietChanged = g_quietFrames.exchange(quiet) != quiet;
    if (changed || quietChanged) Log("[clothdefer] %s (%d quiet frames)", on ? "on" : "off", quiet);
}

uint64_t ClothDeferFrame(uint64_t frame, bool flushAll, uint64_t spentUs, uint64_t budgetUs, bool unitsRan, int* built)
{
    if (built) *built = 0;
    if (!g_installed || !g_pendCount.load(std::memory_order_relaxed) || !OnGameThread()) return 0;
    flushAll = flushAll || !g_on.load(std::memory_order_relaxed);
    std::vector<std::pair<uint64_t, const uint8_t*>> order;   // oldest request first
    AcquireSRWLockShared(&g_pendLock);
    for (const auto& [c, p] : g_pend) order.emplace_back(p.first, c);
    ReleaseSRWLockShared(&g_pendLock);
    std::sort(order.begin(), order.end());
    const uint64_t quiet = static_cast<uint64_t>(g_quietFrames.load(std::memory_order_relaxed));
    uint64_t used = 0;
    int ran = 0;
    for (const auto& [first, c] : order) {
        uint64_t owner = 0, last = 0;
        AcquireSRWLockShared(&g_pendLock);
        auto it = g_pend.find(c);
        const bool there = it != g_pend.end();
        if (there) {
            owner = it->second.owner;
            last = it->second.last;
        }
        ReleaseSRWLockShared(&g_pendLock);
        if (!there) continue;
        const uint64_t age = frame - first;
        int why = -1;
        if (flushAll) why = kFlush;
        else if (!StillDistant(owner)) why = kSeen;
        else if (age >= 2 * kForceFrames) why = kLag;   // the hard bound: whatever the frame holds
        else if (frame - last >= quiet || age >= kForceFrames) {
            // due: as the frame's first unit, or inside the budget (which grows with the wait, as pacing's does), one
            // build after another, so builds that came due together still spread over frames
            const uint64_t scale = 1 + (age > kScaleFrom ? (age - kScaleFrom + 3) / 4 : 0);
            const uint64_t spent = spentUs + used;
            if ((!unitsRan && ran == 0) || spent + static_cast<uint64_t>(g_estUs) <= budgetUs * scale)
                why = frame - last >= quiet ? kQuiet : kLag;
        }
        if (why < 0) continue;
        uint8_t* const comp = const_cast<uint8_t*>(c);
        Pend p;
        if (!Take(comp, &p) || !StillAsDeferred(comp, p)) continue;
        used += BuildTaken(comp, why);
        ++ran;
    }
    if (used > g_maxFrameUs.load(std::memory_order_relaxed)) g_maxFrameUs = used;
    if (built) *built = ran;
    return used;
}

// self-test only: the detoured entry, fakes for the game's functions, direct calls
void* ClothDeferTestRecreate() { return g_recreateSite; }
void ClothDeferTestSetFakes(void* recreate, void* unregister, void* suspended)
{
    g_recreateOrig = reinterpret_cast<RecreateFn>(recreate);
    g_unregOrig = reinterpret_cast<UnregisterFn>(unregister);
    g_suspendedFn = reinterpret_cast<SuspendedFn>(suspended);
    g_gtId = GetCurrentThreadId();   // the test's thread plays the game thread
}
void ClothDeferTestSetDistant(int mode, uint64_t frame)
{
    g_testDistant = mode;
    g_testFrame = frame;
}
bool ClothDeferTestSites(void*** unregisterSlot, void** suspendedCall, void** enabledThunk)
{
    *unregisterSlot = g_unregSlot;
    *suspendedCall = g_suspendedCall;
    *enabledThunk = g_enabledThunk;
    return g_installed;
}
void* ClothDeferTestUnregisterNext() { return reinterpret_cast<void*>(g_unregOrig); }
void ClothDeferTestCallRecreate(uint8_t* component) { RecreateHook(component); }
void ClothDeferTestCallUnregister(uint8_t* component) { UnregisterHook(component); }
bool ClothDeferTestCallSuspended(uint8_t* component) { return SuspendedHook(component); }
size_t ClothDeferTestPending() { return g_pendCount.load(); }

void LogClothBuildDeferStats()
{
    const uint64_t d = g_deferred.exchange(0), m = g_merged.exchange(0), c = g_cancelled.exchange(0), a = g_asked.exchange(0),
                   o = g_odd.exchange(0), us = g_builtUs.exchange(0), mx = g_maxFrameUs.exchange(0);
    uint64_t by[kNumWhy];
    for (int i = 0; i < kNumWhy; ++i) by[i] = g_by[i].exchange(0);
    if (d || m || c || a || o || by[kQuiet] || by[kSeen] || by[kLag] || by[kFlush] || by[kReader])
        Log("[clothdefer] %llu builds deferred, %llu requests merged into them, %llu dropped at unregister, %llu asked for "
            "again outside a far unit (built then), %llu changed while waiting (left to the game); built later: %llu quiet, "
            "%llu near/seen, %llu lag bound, %llu not deferring, %llu before a Blueprint read, %.1f ms (most in one frame "
            "%.1f ms); %zu waiting",
            static_cast<unsigned long long>(d), static_cast<unsigned long long>(m), static_cast<unsigned long long>(c),
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(o), static_cast<unsigned long long>(by[kQuiet]),
            static_cast<unsigned long long>(by[kSeen]), static_cast<unsigned long long>(by[kLag]),
            static_cast<unsigned long long>(by[kFlush]), static_cast<unsigned long long>(by[kReader]), us / 1000.0, mx / 1000.0,
            g_pendCount.load());
}

}  // namespace opf
