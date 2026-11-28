// UnfreezeClothSkip (worker dlgedge, 2026-11-26; S-050 dialogue close, also every other menu close and the end of a loading
// screen; evidence findings dlgdiag1126 / dlgedge / dlgedge-p1). A stock game inefficiency (also
// without the mod).
//
// Altar's game-freeze subsystem (the freeze behind a conversation, the in-game menus and the loading screens; UFunctions
// Freeze_Original_Game / Unfreeze_Original_Game) sets or clears the world's bShouldSimulatePhysics in its restore function
// (exe+0x4958950 on Steam build 19115871: world+0x13C bit 2). Right after an unfreeze it walks EVERY actor of EVERY level of
// the world (level+0x98 / +0xA0), makes a weak pointer to it and resolves it (exe+0x1125C20 / exe+0x1126290: a garbage actor
// is passed over), collects its USkeletalMeshComponents (AActor::GetComponents with bIncludeFromChildActors = true:
// exe+0x4946450, recursive through UChildActorComponent::ChildActor), and calls USkeletalMeshComponent::RecreateClothingActors
// (exe+0x36EB420) on each one, so that a mesh registered while physics was off (UE 5.3 creates legacy clothing actors only when
// World->bShouldSimulatePhysics) gets its legacy (skeletal-mesh) clothing simulation. Bench (dlgedge-p1): Cheydinhal street
// (4.3k actors, ~260 skeletal mesh components) 2.9-4.1 ms per walk of which 0.6-0.9 ms are the 2 real recreates; canal / inn
// (9.2-9.8k actors) 5.8-7.3 ms with 13 real recreates (3.5-5.0 ms): ~0.5 us per actor of pure traversal.
//
// UE 5.3 RecreateClothingActors (SkeletalMeshComponentPhysics.cpp) on a component whose ClothingSimulation is null and whose
// skeletal mesh has no clothing assets (GetMeshClothingAssets().Num() == 0) does nothing: ReleaseAllClothingResources only
// acts on a simulation; then either an early return (no mesh / not registered / cloth actors not allowed) or the "no clothing
// assets" branch, which only destroys an existing simulation. The exe's function has that shape (simulation +0xCA0, the mesh's
// asset count +0x450). A simulation is only created inside RecreateClothingActors, for a mesh with clothing assets.
//
// The fix (design B, registry): RecreateClothingActors' entry is hooked from start-up (every caller). Each call whose component
// holds a simulation (before or after) or whose mesh has clothing assets puts the component into a registry (a weak pointer,
// game's own FWeakObjectPtr functions). OnRegister and SetSkeletalMesh both end in RecreateClothingActors, so a registered
// component that can make the walk do something has been through the hook with that mesh. At an unfreeze the walk is replaced
// by the registry's components the walk would reach: each live entry's owner actor (its Outer, holding it in OwnedComponents,
// live by the game's weak pointer test) must be in an actor array of one of the world's levels; then RecreateClothingActors is
// called on it (through the hook). Every other component of the walk is the no-op above. Anything unexpected (an owner that is
// not the Outer, not in a level, a garbage owner, a full registry) runs the game's walk instead (counted).
// Why the order and the number of calls do not matter: each component has its own clothing simulation (the factory's
// CreateSimulation per component, its own solver, context and interactor); a call releases and rebuilds only that component's
// cloth actors, from the component's own state; the stock walk reaches a child actor's component twice (as a level actor and
// through its parent's ChildActorComponent) and the second call rebuilds the same actors from the same state in the same frame.
// Not covered (a lab verify is the check): a mesh with clothing assets set on a registered component by the raw setter
// USkinnedMeshComponent::SetSkinnedAsset without SetSkeletalMesh (engine code uses SetSkeletalMesh; deserialization comes
// before registration).
// Keys ([Fixes]): UnfreezeClothSkip 0 = the game's way, 1 = on (start-up install when != 0, then live). Lab: 2 = verify (the
// game's walk runs; the registry's set is computed beside it and every component the walk really changes must be in it: a
// missing one is logged with its own, its owner's and its mesh's names), UnfreezeWalkProbe = 1 logs a line per restore call.
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace opf {
namespace {
// the restore function's walk gate: FUN(byte), then world = this->vfunc(+0x188); test byte [world+0x13C],4; je skip; the walk's
// level array (vfunc again, GetLevels) and its first loop loads (level+0x98 actors, +0xA0 count)
constexpr char kGate[] =
    "0F B6 4C 24 20 E8 ?? ?? ?? ?? 49 8B 45 00 49 8B CD FF 90 ?? ?? 00 00 F6 80 ?? ?? 00 00 ?? 0F 84 ?? ?? ?? ?? "
    "49 8B 45 00 49 8B CD FF 90 ?? ?? 00 00 48 8B C8 E8 ?? ?? ?? ?? 4C 8B 20 48 63 40 08 4D 8D 2C C4 4D 3B E5 0F 84 ?? ?? ?? ?? "
    "0F 1F 84 00 00 00 00 00 49 8B 04 24 48 8B B0 ?? ?? 00 00 48 63 80 ?? ?? 00 00";
constexpr size_t kGateTest = 0x17;      // F6 80 disp32 imm8 (7 bytes)
constexpr size_t kGateLevels = 0x34;    // E8 GetLevels
constexpr size_t kGateActors = 0x59;    // 48 8B B0 disp32 (level actors)
constexpr size_t kGateActorNum = 0x60;  // 48 63 80 disp32 (count)
// the walk body's weak pointer: mov rdx,[rsi]; lea rcx,[rsp+28h]; mov qword [rsp+28h],0; call MakeWeak (pattern + 0x77 .. 0x88);
// mov rax,[rsp+28h]; lea rcx,[rsp+60h]; mov [rsp+60h],rax; call Get (+0x9C)
constexpr char kWeakMake[] = "48 8B 16 48 8D 4C 24 28 48 C7 44 24 28 00 00 00 00 E8";
constexpr size_t kWeakMakeAt = 0x77, kWeakMakeCall = 0x88;
constexpr char kWeakGet[] = "48 8B 44 24 28 48 8D 4C 24 60 48 89 44 24 60 E8";
constexpr size_t kWeakGetAt = 0x8D, kWeakGetCall = 0x9C;
// the walk's call of RecreateClothingActors (pattern + 0x13D): mov rcx,[rbx]; call
constexpr size_t kWalkCall = 0x13D;
// GetComponents (exe+0x4946450): lea r12,[rcx+Owned]; ...; lea r13,[r12+10h] (allocation bits); mov r14d,[r13+18h] (bit count)
constexpr char kOwned[] = "4C 8D A1 ?? ?? 00 00 48 89 45 30 4D 8D 6C 24 10 89 45 38 45 8B 75 18";
// RecreateClothingActors: push rbx; push r14; sub rsp,48h; cmp qword [rcx+Sim],0; mov rbx,rcx; je; call (handle parallel);
// ... the two release blocks ... mov rcx,rbx; call GetSkeletalMeshAsset; test byte [rbx+flags],2 (bAllowClothActors)
constexpr char kRecreate[] =
    "40 53 41 56 48 83 EC 48 48 83 B9 ?? ?? 00 00 00 48 8B D9 74 ?? E8 ?? ?? ?? ?? 48 8B 8B ?? ?? 00 00 48 8B 01 FF 50 78 "
    "48 83 BB ?? ?? 00 00 00 74 ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 8B ?? ?? 00 00 48 85 C9 74 ?? E8 ?? ?? ?? ?? "
    "48 8B 8B ?? ?? 00 00 48 8B 01 FF 50 50 48 8B CB E8 ?? ?? ?? ?? F6 83 ?? ?? 00 00 02 4C 8B F0 0F 84";
constexpr uint8_t kRecreatePrologue[8] = {0x40, 0x53, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x48};
constexpr size_t kRecreateSim = 11, kRecreateSim2 = 42, kRecreateGetMesh = 0x5A;  // the sim's two compares (review S3-2: equal)
constexpr char kAssetNum[] = "41 83 BE ?? ?? 00 00 00 0F 8E";  // cmp dword [r14+Num],0; jle (after the mesh's async-property wait)
constexpr size_t kOuterOff = 0x20;  // UObjectBase::OuterPrivate

using RecreateFn = void (*)(uint8_t* comp);
using GetMeshFn = uint8_t* (*)(uint8_t* comp);
using MakeWeakFn = void (*)(uint64_t* weak, const void* obj);  // FWeakObjectPtr = UObject*
using GetWeakFn = uint8_t* (*)(const uint64_t* weak);          // FWeakObjectPtr::Get
using GetLevelsFn = uint8_t* (*)(uint8_t* world);              // TArray<ULevel*>*
using WeakExistsFn = bool (*)(const uint64_t* weak);           // the object still exists (garbage-flagged or not)
RecreateFn g_recreateOrig = nullptr;  // trampoline: the 8 relocated bytes, then back
GetMeshFn g_getMesh = nullptr;
MakeWeakFn g_makeWeak = nullptr;
GetWeakFn g_getWeak = nullptr;
GetLevelsFn g_getLevels = nullptr;
WeakExistsFn g_weakExists = nullptr;
uint8_t* g_objArray = nullptr;  // GUObjectArray (FindObjectArrayFromDtor)
// FUObjectArray (UE 5.3): chunk table +0x10, NumElements +0x24; FUObjectItem {Object, Flags, ClusterRootIndex, SerialNumber} = 24
// bytes, 65536 per chunk; FWeakObjectPtr = {ObjectIndex, ObjectSerialNumber}. As menu_load_keep.cpp / object_replace_guard.cpp.
bool WeakExistsInArray(const uint64_t* weak)
{
    const int32_t index = static_cast<int32_t>(*weak & 0xFFFFFFFFu), serial = static_cast<int32_t>(*weak >> 32);
    if (!g_objArray || index < 0 || serial == 0) return false;
    __try {
        if (index >= *reinterpret_cast<const int32_t*>(g_objArray + 0x24)) return false;
        const uint8_t* const* const chunks = *reinterpret_cast<const uint8_t* const* const*>(g_objArray + 0x10);
        const uint8_t* const chunk = chunks ? chunks[index >> 16] : nullptr;
        if (!chunk) return false;
        const uint8_t* const item = chunk + static_cast<size_t>(index & 0xFFFF) * 24;
        return *reinterpret_cast<void* const*>(item) != nullptr && *reinterpret_cast<const int32_t*>(item + 0x10) == serial;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;  // unreadable: treated as existing (the game's walk)
    }
}
uint32_t g_simOff = 0, g_numOff = 0, g_bitOff = 0, g_actorsOff = 0, g_actorNumOff = 0, g_ownedOff = 0;
uint8_t g_bitMask = 0;
uint8_t* g_gate = nullptr;      // patched test (7 bytes)
uint8_t* g_recreate = nullptr;  // patched entry
std::atomic<int> g_mode{0};
std::atomic<bool> g_seen{false};  // the registry was ever non-empty
std::atomic<uint64_t> s_skipped{0}, s_walks{0}, s_calls{0}, s_regWalks{0}, s_fallbacks{0}, s_regCalls{0}, s_dropped{0}, s_garbage{0};
const char* g_lastFallback = "";

// ---- the registry (weak pointers to the components that held / could make a legacy clothing simulation) ----
constexpr int kMaxReg = 2048;
uint64_t g_reg[kMaxReg];
int g_numReg = 0;
bool g_regFull = false;
SRWLOCK g_regLock = SRWLOCK_INIT;

template <typename T> T ReadAt(const uint8_t* p, size_t off)
{
    T v{};
    __try { memcpy(&v, p + off, sizeof v); } __except (EXCEPTION_EXECUTE_HANDLER) { v = T{}; }
    return v;
}

// true when this component holds or would make a legacy clothing simulation (not the no-op)
bool LegacyCloth(uint8_t* comp)
{
    if (ReadAt<void*>(comp, g_simOff)) return true;
    uint8_t* const mesh = g_getMesh ? g_getMesh(comp) : nullptr;
    return mesh && ReadAt<int32_t>(mesh, g_numOff) > 0;
}

void RegAdd(uint8_t* comp)
{
    uint64_t w = 0;
    g_makeWeak(&w, comp);
    if (!w) return;
    AcquireSRWLockExclusive(&g_regLock);
    bool have = false;
    for (int i = 0; i < g_numReg && !have; ++i) have = g_reg[i] == w;
    if (!have && g_numReg == kMaxReg && !g_regFull)  // review S3-1: make room by dropping destroyed entries first
        for (int i = 0; i < g_numReg;)
            if (!g_getWeak(&g_reg[i]) && g_weakExists && !g_weakExists(&g_reg[i])) {
                g_reg[i] = g_reg[--g_numReg];
                s_dropped.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++i;
            }
    if (!have) {
        if (g_numReg < kMaxReg) g_reg[g_numReg++] = w;
        else g_regFull = true;  // an add lost: the game's walk for the rest of the session
    }
    ReleaseSRWLockExclusive(&g_regLock);
    if (!have && !g_seen.exchange(true))
        Log("[unfreeze] first legacy clothing component (%p): the restore's walk now visits the registry's components", static_cast<void*>(comp));
}

// RecreateClothingActors' entry (every caller)
void RecreateHook(uint8_t* comp)
{
    s_calls.fetch_add(1, std::memory_order_relaxed);
    const bool before = LegacyCloth(comp);
    g_recreateOrig(comp);
    if (before || ReadAt<void*>(comp, g_simOff)) RegAdd(comp);
}

// the owner's OwnedComponents (a TSet: sparse array data +0, allocation bits inline +0x10 / heap +0x20, bit count +0x28) holds comp
bool OwnsComponent(const uint8_t* actor, const uint8_t* comp)
{
    __try {
        const uint8_t* const set = actor + g_ownedOff;
        const uint8_t* const data = *reinterpret_cast<const uint8_t* const*>(set);
        const uint32_t* bits = reinterpret_cast<const uint32_t*>(set + 0x10);
        if (const uint32_t* heap = *reinterpret_cast<const uint32_t* const*>(set + 0x20)) bits = heap;
        const int32_t n = *reinterpret_cast<const int32_t*>(set + 0x28);
        if (n < 0 || n > 100000 || !data) return false;
        for (int32_t i = 0; i < n; ++i)
            if ((bits[i >> 5] >> (i & 31)) & 1)
                if (*reinterpret_cast<const uint8_t* const*>(data + static_cast<size_t>(i) * 0x10) == comp) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// the registry's components the walk would reach; false = run the game's walk (reason in g_lastFallback)
struct Target { uint8_t* comp; const uint8_t* actor; bool found; };
constexpr int kHash = 4096;  // open-addressed actor -> first target index (+1)
Target g_targets[kMaxReg];
int g_numTargets = 0;
int32_t g_hash[kHash];

bool Fallback(const char* why)
{
    g_lastFallback = why;
    s_fallbacks.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool CollectTargets(uint8_t* world)
{
    g_numTargets = 0;
    AcquireSRWLockExclusive(&g_regLock);
    const bool full = g_regFull;
    bool garbage = false;
    for (int i = 0; i < g_numReg;) {
        uint8_t* const c = g_getWeak(&g_reg[i]);
        if (!c) {
            // Review S2-2: the weak test is also null for a garbage-flagged object that still exists, and the walk does not test
            // components (GetComponents: OwnedComponents with IsA only), so such a component of a live actor still gets its call
            // in the game. Kept (dropped once destroyed) and this restore runs the game's walk. Destroyed: no actor owns it any
            // more (UnregisterComponent / OnComponentDestroyed take it out of OwnedComponents), the walk cannot reach it: dropped.
            if (!g_weakExists || g_weakExists(&g_reg[i])) {
                garbage = true;
                ++i;
                continue;
            }
            g_reg[i] = g_reg[--g_numReg];
            s_dropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        g_targets[g_numTargets++] = Target{c, nullptr, false};
        ++i;
    }
    ReleaseSRWLockExclusive(&g_regLock);
    if (full) return Fallback("registry full");
    if (garbage) {
        s_garbage.fetch_add(1, std::memory_order_relaxed);
        return Fallback("a registered component is garbage but not yet destroyed");
    }
    if (g_numTargets == 0) return true;
    for (int32_t& h : g_hash) h = 0;
    for (int i = 0; i < g_numTargets; ++i) {
        Target& t = g_targets[i];
        const uint8_t* const a = ReadAt<const uint8_t*>(t.comp, kOuterOff);
        if (!a || !OwnsComponent(a, t.comp)) return Fallback("a registered component's Outer does not own it");
        uint64_t wa = 0;
        g_makeWeak(&wa, a);
        if (!g_getWeak(&wa)) return Fallback("a registered component's owner is garbage");
        t.actor = a;
        uint32_t h = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(a) >> 4) * 2654435761u) & (kHash - 1);
        while (g_hash[h] && g_targets[g_hash[h] - 1].actor != a) h = (h + 1) & (kHash - 1);
        if (!g_hash[h]) g_hash[h] = i + 1;
    }
    // every owner must be an actor of one of the world's levels (the walk's outer loops)
    int found = 0;
    __try {
        const uint8_t* const arr = g_getLevels(world);
        const uint8_t* const* const lv = *reinterpret_cast<const uint8_t* const* const*>(arr);
        const int32_t nl = *reinterpret_cast<const int32_t*>(arr + 8);
        for (int32_t l = 0; l < nl; ++l) {
            const uint8_t* const level = lv[l];
            if (!level) continue;
            const uint8_t* const* const actors = *reinterpret_cast<const uint8_t* const* const*>(level + g_actorsOff);
            const int32_t na = *reinterpret_cast<const int32_t*>(level + g_actorNumOff);
            for (int32_t k = 0; k < na; ++k) {
                const uint8_t* const a = actors[k];
                if (!a) continue;
                uint32_t h = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(a) >> 4) * 2654435761u) & (kHash - 1);
                while (g_hash[h]) {
                    Target& t = g_targets[g_hash[h] - 1];
                    if (t.actor == a) {
                        for (int i = g_hash[h] - 1; i < g_numTargets; ++i)  // every target of this actor
                            if (g_targets[i].actor == a && !g_targets[i].found) { g_targets[i].found = true; ++found; }
                        break;
                    }
                    h = (h + 1) & (kHash - 1);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Fallback("the world's level arrays could not be read");
    }
    if (found != g_numTargets) return Fallback("a registered component's owner is in none of the world's levels");
    return true;
}


// the walk gate (rcx = the world): true = the game's walk, false = left out (the registry's components done here instead)
bool GateDecide(uint8_t* world)
{
    const bool bit = (ReadAt<uint8_t>(world, g_bitOff) & g_bitMask) != 0;
    if (!bit) return false;
    s_walks.fetch_add(1, std::memory_order_relaxed);
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode != 1 || !g_recreate) return true;
    if (!g_seen.load(std::memory_order_relaxed)) {
        s_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!CollectTargets(world)) return true;  // the game's walk (counted)
    s_regWalks.fetch_add(1, std::memory_order_relaxed);
    s_skipped.fetch_add(1, std::memory_order_relaxed);
    const int n = g_numTargets;
    for (int i = 0; i < n; ++i) {
        s_regCalls.fetch_add(1, std::memory_order_relaxed);
        RecreateHook(g_targets[i].comp);  // as the walk's call (through the entry hook: the registry stays current)
    }
    return false;
}


bool Fail(const char* why)
{
    Log("[unfreeze] %s: UnfreezeClothSkip off", why);
    return false;
}
}  // namespace

bool UnfreezeClothLocate(const GameModule& game, UnfreezeClothSites* out, const char** why)
{
    *out = {};
    const auto gates = FindAll(game.text, ParsePattern(kGate));
    if (gates.size() != 1) { *why = "the restore's cloth walk gate not found once"; return false; }
    uint8_t* const g = gates[0];
    const auto recs = FindAll(game.text, ParsePattern(kRecreate));
    if (recs.size() != 1) { *why = "RecreateClothingActors not found once"; return false; }
    uint8_t* const r = recs[0];
    uint8_t* const wc = g + kWalkCall;
    if (wc[0] != 0x48 || wc[1] != 0x8B || wc[2] != 0x0B || wc[3] != 0xE8 || RipTarget(wc + 3, 1, 5) != r) {
        *why = "the walk's call is not RecreateClothingActors";
        return false;
    }
    if (!MatchesAt(g + kWeakMakeAt, ParsePattern(kWeakMake)) || !MatchesAt(g + kWeakGetAt, ParsePattern(kWeakGet))) {
        *why = "the walk's weak pointer calls not as expected";
        return false;
    }
    const uint8_t* num = nullptr;
    int nums = 0;
    const Pattern pn = ParsePattern(kAssetNum);
    for (size_t i = 0x60; i < 0x140; ++i)
        if (MatchesAt(r + i, pn)) { if (!num) num = r + i; ++nums; }
    if (nums != 1) { *why = "RecreateClothingActors' clothing asset count not found once"; return false; }
    // GetComponents: the call between the walk's class checks (E8 at pattern + 0x113 or + 0x10C: the non-ActorComponent branch)
    uint8_t* getComps = nullptr;
    for (size_t i = 0xF0; i < 0x130 && !getComps; ++i)
        if (g[i] == 0xEB && g[i + 1] == 0x05 && g[i + 2] == 0xE8) getComps = RipTarget(g + i + 2, 1, 5);
    if (!getComps || !game.text.Contains(getComps)) { *why = "the walk's GetComponents call not found"; return false; }
    const Pattern po = ParsePattern(kOwned);
    const uint8_t* owned = nullptr;
    for (size_t i = 0; i < 0x60 && !owned; ++i)
        if (MatchesAt(getComps + i, po)) owned = getComps + i;
    if (!owned) { *why = "GetComponents' OwnedComponents not recognised"; return false; }
    out->gate = g + kGateTest;
    out->walkCall = wc + 3;
    out->recreate = r;
    out->getMesh = RipTarget(r + kRecreateGetMesh, 1, 5);
    out->getLevels = RipTarget(g + kGateLevels, 1, 5);
    out->makeWeak = RipTarget(g + kWeakMakeCall, 1, 5);
    out->getWeak = RipTarget(g + kWeakGetCall, 1, 5);
    out->getComps = getComps;
    memcpy(&out->bitOff, g + kGateTest + 2, 4);
    out->bitMask = g[kGateTest + 6];
    memcpy(&out->simOff, r + kRecreateSim, 4);
    uint32_t sim2 = 0;
    memcpy(&sim2, r + kRecreateSim2, 4);
    if (sim2 != out->simOff) { *why = "RecreateClothingActors' two simulation compares differ"; return false; }
    memcpy(&out->numOff, num + 3, 4);
    memcpy(&out->actorsOff, g + kGateActors + 3, 4);
    memcpy(&out->actorNumOff, g + kGateActorNum + 3, 4);
    memcpy(&out->ownedOff, owned + 3, 4);
    out->restore = PrimaryFunction(game, g);
    for (const uint8_t* f : {out->getMesh, out->getLevels, out->makeWeak, out->getWeak})
        if (!game.text.Contains(f)) { *why = "call targets outside the code"; return false; }
    if (out->bitMask == 0 || (out->bitMask & (out->bitMask - 1)) || out->bitOff > 0x1000 || out->simOff > 0x4000 || out->numOff > 0x4000 ||
        out->ownedOff > 0x1000 || out->actorsOff > 0x1000 || out->actorNumOff != out->actorsOff + 8) {
        *why = "offsets out of range";
        return false;
    }
    return true;
}

bool InstallUnfreezeClothSkip(bool beforeGameCode, int mode, bool probe)
{
    if (g_gate) return true;
    probe = false;
    if (mode == 2) mode = 1;
    if (mode == 0 && !probe) {
        Log("[unfreeze] UnfreezeClothSkip off in OblivionPerfFix.ini (take a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) return Fail("installation requires game startup");
    const GameModule& game = Game();
    UnfreezeClothSites s;
    const char* why = "";
    if (!UnfreezeClothLocate(game, &s, &why)) return Fail(why);
    if (memcmp(s.recreate, kRecreatePrologue, sizeof kRecreatePrologue) != 0) return Fail("RecreateClothingActors' first bytes changed");
    g_objArray = FindObjectArrayFromDtor();
    if (!g_objArray) return Fail("GUObjectArray not found");
    uint8_t* const region = AllocNearGame(96);
    if (!region) return Fail("no memory near the game's code");
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    // region+0: the 8 relocated bytes + jmp back; +32: jmp RecreateHook; +48: mov rcx,rax + jmp GateDecide
    memcpy(region, s.recreate, 8);
    memcpy(region + 8, jmpAbs, 6);
    uint8_t* const back = s.recreate + 8;
    memcpy(region + 14, &back, 8);
    uint8_t* const recStub = region + 32;
    memcpy(recStub, jmpAbs, 6);
    void* const rh = reinterpret_cast<void*>(&RecreateHook);
    memcpy(recStub + 6, &rh, 8);
    uint8_t* const gateStub = region + 48;
    gateStub[0] = 0x48; gateStub[1] = 0x8B; gateStub[2] = 0xC8;  // mov rcx,rax (the world)
    memcpy(gateStub + 3, jmpAbs, 6);
    void* const gh = reinterpret_cast<void*>(&GateDecide);
    memcpy(gateStub + 9, &gh, 8);
    DWORD old;
    if (!VirtualProtect(region, 96, PAGE_EXECUTE_READ, &old)) return Fail("stub protection");
    FlushInstructionCache(GetCurrentProcess(), region, 96);
    const int64_t relR = recStub - (s.recreate + 5), relG = gateStub - (s.gate + 5);
    if (relR != static_cast<int32_t>(relR) || relG != static_cast<int32_t>(relG)) return Fail("stub out of reach");
    g_recreateOrig = reinterpret_cast<RecreateFn>(region);
    g_getMesh = reinterpret_cast<GetMeshFn>(s.getMesh);
    g_makeWeak = reinterpret_cast<MakeWeakFn>(s.makeWeak);
    g_getWeak = reinterpret_cast<GetWeakFn>(s.getWeak);
    g_getLevels = reinterpret_cast<GetLevelsFn>(s.getLevels);
    g_weakExists = &WeakExistsInArray;
    g_simOff = s.simOff;
    g_numOff = s.numOff;
    g_bitOff = s.bitOff;
    g_bitMask = s.bitMask;
    g_actorsOff = s.actorsOff;
    g_actorNumOff = s.actorNumOff;
    g_ownedOff = s.ownedOff;
    g_mode.store(mode);
    uint8_t jr[5] = {0xE9};
    const int32_t r32 = static_cast<int32_t>(relR);
    memcpy(jr + 1, &r32, 4);
    if (!WriteCode(s.recreate, jr)) return Fail("RecreateClothingActors patch");
    g_recreate = s.recreate;
    uint8_t gc[7] = {0xE8, 0, 0, 0, 0, 0x84, 0xC0};  // call stub; test al,al (the game's je follows)
    const int32_t g32 = static_cast<int32_t>(relG);
    memcpy(gc + 1, &g32, 4);
    if (!WriteCode(s.gate, gc)) return Fail("walk gate patch");  // the entry hook alone only keeps the registry
    g_gate = s.gate;
    Log("[unfreeze] installed: walk gate exe+0x%X (world +0x%X & 0x%X), RecreateClothingActors exe+0x%X (simulation +0x%X, mesh "
        "clothing assets +0x%X), weak pointer exe+0x%X / exe+0x%X, OwnedComponents +0x%X; UnfreezeClothSkip %d",
        static_cast<unsigned>(s.gate - game.base), s.bitOff, s.bitMask, static_cast<unsigned>(s.recreate - game.base), s.simOff,
        s.numOff, static_cast<unsigned>(s.makeWeak - game.base), static_cast<unsigned>(s.getWeak - game.base), s.ownedOff, mode);
    return true;
}

void SetUnfreezeClothSkip(int mode, bool probe)
{
    if (!g_gate) return;
    (void)probe;
    if (mode == 2) mode = 1;
    const int m = mode == 1 || mode == 2 ? mode : 0;
    if (g_mode.exchange(m) != m) Log("[unfreeze] UnfreezeClothSkip %d", m);
}

void LogUnfreezeClothStats()
{
    static uint64_t last[8] = {};
    const uint64_t now[8] = {s_walks.load(), s_skipped.load(), s_calls.load(), s_regWalks.load(), s_fallbacks.load(), s_regCalls.load(),
                             s_dropped.load(), s_garbage.load()};
    if (now[0] != last[0]) {
        Log("[unfreeze] since last report: %llu restore walks, %llu replaced (%llu by the registry: %llu calls), %llu fell back to the "
            "game's walk (%llu for a garbage component not yet destroyed; last: %s), %llu RecreateClothingActors calls in all, %llu "
            "dead entries dropped, registry %d%s",
            now[0] - last[0], now[1] - last[1], now[3] - last[3], now[5] - last[5], now[4] - last[4], now[7] - last[7], g_lastFallback,
            now[2] - last[2], now[6] - last[6], g_numReg, g_regFull ? " (full)" : "");
    }
    memcpy(last, now, sizeof now);
}

// ---- self-test ----
void UnfreezeClothTestSetup(const UnfreezeClothTestFns& f)
{
    g_recreateOrig = reinterpret_cast<RecreateFn>(f.recreate);
    g_getMesh = reinterpret_cast<GetMeshFn>(f.getMesh);
    g_makeWeak = reinterpret_cast<MakeWeakFn>(f.makeWeak);
    g_getWeak = reinterpret_cast<GetWeakFn>(f.getWeak);
    g_getLevels = reinterpret_cast<GetLevelsFn>(f.getLevels);
    g_weakExists = reinterpret_cast<WeakExistsFn>(f.weakExists);
    g_simOff = f.simOff;
    g_numOff = f.numOff;
    g_bitOff = f.bitOff;
    g_bitMask = f.mask;
    g_actorsOff = f.actorsOff;
    g_actorNumOff = f.actorsOff + 8;
    g_ownedOff = f.ownedOff;
    g_recreate = reinterpret_cast<uint8_t*>(f.recreate);  // non-null: "installed" for the decision
    g_seen.store(false);
    g_numReg = 0;
    g_regFull = false;
}
void UnfreezeClothTestReset()
{
    g_recreate = nullptr; g_recreateOrig = nullptr; g_getMesh = nullptr; g_weakExists = nullptr;
    g_seen.store(false); g_mode.store(0); g_numReg = 0; g_regFull = false;
}
void UnfreezeClothTestMode(int mode) { g_mode.store(mode); }
bool UnfreezeClothTestGate(uint8_t* world) { return GateDecide(world); }
void UnfreezeClothTestRecreate(uint8_t* comp) { RecreateHook(comp); }
bool UnfreezeClothTestSeen() { return g_seen.load(); }
int UnfreezeClothTestRegistry() { return g_numReg; }
uint64_t UnfreezeClothTestFallbacks() { return s_fallbacks.load(); }
}  // namespace opf
