// NpcBodiesOneLock: a character's physics bodies and constraints made under one hold of the scene lock.
//
// When an NPC's skeletal mesh registers, USkeletalMeshComponent::OnCreatePhysicsState calls InitArticulated(PhysScene)
// (exe+0x36E3100 in build 19115871; UE 5.3 SkeletalMeshComponentPhysics.cpp:665), which, past its early returns (no scene,
// no physics asset, no mesh, ShouldCreatePhysicsState false, bodies already made, no root body), calls
// InstantiatePhysicsAsset_Internal (exe+0x36E8080, SkeletalMeshComponentPhysics.cpp:903): the physics asset's bodies one
// after the other (InstantiatePhysicsAssetBodies_Internal exe+0x36E7A80: per body FBodyInstance::InitBody), the aggregate's
// scene add, then the constraints (FConstraintInstance::InitConstraint exe+0x34EB210). Every body and constraint takes the
// scene's write lock on its own (FPhysicsCommand::ExecuteWrite, exe+0x35303B0 -> the lock scope exe+0x353D2E0): ~37 bodies
// per humanoid, ~550 calls of the lock scope per humanoid create (measured 2026-10-20), of which every outermost one acquires the
// SRW lock of the solver's external data (solver = scene+0x38, lock object = [solver+0x200], an SRW lock whose recursion
// counts live in a TLS slot whose index is at lock+8: read count in the low, write count in the high 32 bits). At the
// Imperial City the game thread waits 0.2-0.3 ms per NPC in those acquires: the readers are world scene queries (async
// traces) on other threads.
//
// The fix takes the same lock once, the game's way (write count +1 in the TLS slot; AcquireSRWLockExclusive when it is the
// thread's first hold), around InitArticulated's call of InstantiatePhysicsAsset_Internal and releases it after; every
// ExecuteWrite inside is then a nested hold (the game's own code just counts it in the TLS slot). InitArticulated's early
// returns and its work after the bodies (SetName/SetId per body, SetRootBodyIndex, InitCollisionRelationships, the bone
// matrix, the teleport thresholds) run as in the game, outside the hold. What the game thread does is unchanged: the same
// bodies, shapes, constraints and aggregate, made by the game's code in the same order with the same arguments; the physics
// world ends in the same state. What changes is only when other threads can get in: a reader on another thread now sees the
// scene either before this NPC's first body or after its last constraint, never between two of them. Both are states the
// game itself can show a reader (lock acquisition order between threads is not fixed; a reader that arrives just before the
// first body or just after the last sees exactly that), so the set of outcomes is a subset of the stock ones ("equivalent":
// as fixes 28/34, which also add a whole batch inside one lock). Nothing inside InstantiatePhysicsAsset_Internal waits for
// another thread (5.3 source: body setup copy, GetBoneTransform through the TFunctionRef, InitBody: shape and actor
// creation, BodySetup::CreatePhysicsMeshes from cooked data, the scene add; AddAggregateToScene; InitConstraint), so holding
// the lock cannot deadlock where the stock per-body holds could not. The cost moves to readers: the hold now also covers
// shape/actor creation that the game did outside the lock (~0.25 ms per humanoid at 480p); the stats line logs the longest hold.
//
// Pinned to build 19115871: InitArticulated (found by its prologue), InstantiatePhysicsAsset_Internal (the call at
// InitArticulated+0x25F), InstantiatePhysicsAssetBodies_Internal (its call at +0x120) and InitConstraint (its call at +0x492)
// must each equal this build's bytes (FNV-1a over the whole function, which includes the rel32 of every call they make, so a
// relink of their callees changes it too); the lock scope's code is checked by pattern and the solver and lock offsets read
// from ExecuteWrite and the lock scope. Off with a log line if anything differs (a game update, the Game Pass exe).
// Key: [Fixes] NpcBodiesOneLock 0 off / 1 on (default): the call is redirected at start-up when it is on (lab builds:
// always), then the key is live.
#include "common.h"

#include <atomic>
#include <cstring>
#include <cwchar>

namespace opf {
namespace {

// USkeletalMeshComponent::InitArticulated (exe+0x36E3100): prologue, security cookie, PhysScene kept in rdi, this in rbx,
// GetPhysicsAsset.
constexpr char kInitArticulated[] =
    "40 53 57 41 55 48 81 EC ?? ?? 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? 00 00 48 8B FA 48 89 54 24 ?? 48 8B D9 E8";
uint64_t Fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
struct Pinned {
    const char* name;
    uint32_t size;
    uint64_t fnv;
};
// build 19115871 (whole functions up to their int3 padding)
constexpr Pinned kIa = {"InitArticulated", 0x38B, 0x113BE8D723F05B6Aull};
constexpr Pinned kIpa = {"InstantiatePhysicsAsset_Internal", 0x642, 0x661ADBE1D446E9C0ull};
constexpr Pinned kBodies = {"InstantiatePhysicsAssetBodies_Internal", 0x3A9, 0xB3C79B05F9A5BD52ull};
constexpr Pinned kConstraint = {"FConstraintInstance::InitConstraint", 0x25A, 0xE02B38F67CB3F05Dull};
constexpr size_t kIaCallIpa = 0x25F, kIpaCallBodies = 0x120, kIpaCallConstraint = 0x492;
// FPhysicsCommand::ExecuteWrite(FPhysScene*, TFunctionRef) (exe+0x35303B0): scope {locked=0, solver=[scene+solverOff],
// type=1 (write)}; call the lock scope at +0x2F.
constexpr char kExecWrite[] =
    "40 53 48 83 EC 40 48 8B DA 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 41 ?? 48 8D 4C 24 20 48 89 44 24 28 48 89 74 24 50 C6 44 24 20 00 "
    "C6 44 24 30 01 E8";
constexpr size_t kExecWriteSolver = 0x15, kExecWriteCall = 0x2F;
// The lock scope (exe+0x353D2E0): solver null -> nothing; type 1: lock = [solver+lockOff]; v = TlsGetValue([lock+8]);
// write count (high half) + 1; TlsSetValue; if (write + read == 1) AcquireSRWLockExclusive(lock); scope.locked = 1.
constexpr char kLockScope[] =
    "40 56 48 83 EC 20 48 8B F1 48 8B 49 08 48 85 C9 0F 84 ?? ?? ?? ?? 0F B6 56 10 48 89 6C 24 38 85 D2 74 ?? 83 FA 01 75 ?? "
    "48 8B A9 ?? ?? 00 00 48 89 5C 24 30 48 89 7C 24 40 8B 4D 08 FF 15 ?? ?? ?? ?? 8B 4D 08 8B F8 48 C1 E8 20 8D 58 01 8B D3 48 C1 E2 20 "
    "48 0B D7 FF 15 ?? ?? ?? ?? 8D 04 3B 48 8B 7C 24 40 48 8B 5C 24 30 83 F8 01 75 ?? 48 8B CD FF 15";
constexpr size_t kLockScopeLockOff = 0x2B, kLockScopeTlsSet = 0x57, kLockScopeAcquire = 0x72;
// The recursive lock's read acquire (exe+0x3542D00): TlsGetValue; read count + 1; TlsSetValue; if 1: AcquireSRWLockShared.
constexpr char kReadLock[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 8B 49 08 FF 15 ?? ?? ?? ?? 8B 4E 08 48 8B D0 48 C1 EA 20 48 8B FA "
    "48 C1 E2 20 8D 58 01 8B C3 48 0B D0 FF 15 ?? ?? ?? ?? 8D 04 3B 83 F8 01 75 ?? 48 8B CE FF 15";
constexpr size_t kReadLockAcquire = 0x45;

// InstantiatePhysicsAsset_Internal(this, PhysAsset&, Scale3D&, OutBodies&, OutConstraints&, BoneTransformGetter (TFunctionRef,
// by address), PhysScene*, OwningComponent*, UseRootBodyIndex, UseAggregate&): ten register/stack slots, forwarded as they are.
using IpaFn = void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint8_t*, uintptr_t, uintptr_t, uintptr_t);
IpaFn g_orig = nullptr;
uint8_t* g_site = nullptr;
uint32_t g_solverOff = 0, g_lockOff = 0;
bool g_installed = false;
std::atomic<int> g_mode{0};
std::atomic<uint64_t> s_calls{0}, s_wrapped{0}, s_nested{0}, s_waitTicks{0}, s_innerTicks{0};
std::atomic<uint64_t> s_maxHold{0}, s_holds60{0}, s_holdTicks60{0};

template <typename T> T Read(const uint8_t* p, size_t off) { T v; memcpy(&v, p + off, sizeof v); return v; }

int64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
void AtomicMax(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

// The game's recursive write hold (as its lock scope does it). Returns true when this was the thread's first hold.
bool LockWrite(uint8_t* lock)
{
    const DWORD idx = *reinterpret_cast<const uint32_t*>(lock + 8);
    const uint64_t v = reinterpret_cast<uint64_t>(TlsGetValue(idx));
    const uint32_t w = static_cast<uint32_t>(v >> 32) + 1;
    TlsSetValue(idx, reinterpret_cast<void*>((static_cast<uint64_t>(w) << 32) | (v & 0xFFFFFFFFull)));
    if (w + static_cast<uint32_t>(v) != 1) return false;
    AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(lock));
    return true;
}
void UnlockWrite(uint8_t* lock)
{
    const DWORD idx = *reinterpret_cast<const uint32_t*>(lock + 8);
    const uint64_t v = reinterpret_cast<uint64_t>(TlsGetValue(idx));
    const uint32_t w = static_cast<uint32_t>(v >> 32) - 1;
    TlsSetValue(idx, reinterpret_cast<void*>((static_cast<uint64_t>(w) << 32) | (v & 0xFFFFFFFFull)));
    if (w + static_cast<uint32_t>(v) == 0) ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(lock));
}

void IpaHook(uintptr_t self, uintptr_t asset, uintptr_t scale, uintptr_t bodies, uintptr_t constraints, uintptr_t boneTm,
             uint8_t* scene, uintptr_t owner, uintptr_t root, uintptr_t aggregate)
{
    uint8_t* const solver = scene ? *reinterpret_cast<uint8_t**>(scene + g_solverOff) : nullptr;
    uint8_t* const lock = solver ? *reinterpret_cast<uint8_t**>(solver + g_lockOff) : nullptr;
    if (g_mode.load(std::memory_order_relaxed) == 0 || !lock) {  // the game's lock scope does nothing without a solver either
        g_orig(self, asset, scale, bodies, constraints, boneTm, scene, owner, root, aggregate);
        return;
    }
    s_calls.fetch_add(1, std::memory_order_relaxed);
    const int64_t t0 = Qpc();
    const bool first = LockWrite(lock);
    const int64_t t1 = Qpc();
    g_orig(self, asset, scale, bodies, constraints, boneTm, scene, owner, root, aggregate);
    const int64_t t2 = Qpc();
    UnlockWrite(lock);
    (first ? s_wrapped : s_nested).fetch_add(1, std::memory_order_relaxed);
    s_waitTicks.fetch_add(static_cast<uint64_t>(t1 - t0), std::memory_order_relaxed);
    s_innerTicks.fetch_add(static_cast<uint64_t>(t2 - t1), std::memory_order_relaxed);
    if (first) {
        AtomicMax(s_maxHold, static_cast<uint64_t>(t2 - t1));
        s_holds60.fetch_add(1, std::memory_order_relaxed);
        s_holdTicks60.fetch_add(static_cast<uint64_t>(t2 - t1), std::memory_order_relaxed);
    }
}

bool Fail(const char* what)
{
    Log("[npcbodies] %s: NpcBodiesOneLock unavailable", what);
    return false;
}

// The function a call rel32 at `site` reaches, if it is in .text and equals this build's bytes.
uint8_t* PinnedCallee(const GameModule& game, uint8_t* site, const Pinned& want)
{
    if (site[0] != 0xE8) return nullptr;
    uint8_t* const f = RipTarget(site, 1, 5);
    if (!game.text.Contains(f) || !game.text.Contains(f + want.size - 1) || Fnv(f, want.size) != want.fnv) return nullptr;
    return f;
}

bool Find(uint8_t** site, IpaFn* fn, uint32_t* solverOff, uint32_t* lockOff)
{
    const GameModule& game = Game();
    const std::vector<uint8_t*> fns = FindAll(game.text, ParsePattern(kInitArticulated));
    if (fns.size() != 1) return Fail("InitArticulated not found");
    uint8_t* const ia = fns[0];
    if (!game.text.Contains(ia + kIa.size - 1) || Fnv(ia, kIa.size) != kIa.fnv) return Fail("InitArticulated differs from build 19115871's");
    uint8_t* const ipa = PinnedCallee(game, ia + kIaCallIpa, kIpa);
    if (!ipa) return Fail("InstantiatePhysicsAsset_Internal (called by InitArticulated) differs from build 19115871's");
    if (!PinnedCallee(game, ipa + kIpaCallBodies, kBodies)) return Fail("InstantiatePhysicsAssetBodies_Internal differs from build 19115871's");
    if (!PinnedCallee(game, ipa + kIpaCallConstraint, kConstraint)) return Fail("InitConstraint differs from build 19115871's");
    uint32_t sOff = 0, lOff = 0;
    int ew = 0;
    for (uint8_t* m : FindAll(game.text, ParsePattern(kExecWrite))) {
        const uint8_t* scope = RipTarget(m + kExecWriteCall, 1, 5);
        if (!game.text.Contains(scope) || !MatchesAt(scope, ParsePattern(kLockScope))) continue;
        const uint32_t s = m[kExecWriteSolver], l = Read<uint32_t>(scope, kLockScopeLockOff);
        if (ew++ && (s != sOff || l != lOff)) return Fail("ExecuteWrite variants disagree on the lock's place");
        sOff = s;
        lOff = l;
    }
    if (ew == 0 || sOff == 0 || lOff == 0 || lOff > 0x2000) return Fail("ExecuteWrite / the scene lock scope not as expected");
    *site = ia + kIaCallIpa;
    *fn = reinterpret_cast<IpaFn>(ipa);
    *solverOff = sOff;
    *lockOff = lOff;
    return true;
}


}  // namespace

bool InstallNpcBodiesOneLock(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!beforeGameCode) return Fail("only installed while the game starts (version.dll)");
    uint8_t* site = nullptr;
    IpaFn fn = nullptr;
    if (!Find(&site, &fn, &g_solverOff, &g_lockOff)) return false;
    uint8_t* const stub = AllocNearGame(16);
    if (!stub) return Fail("no stub memory near the game");
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* hook = reinterpret_cast<void*>(&IpaHook);
    memcpy(stub, jmpAbs, sizeof jmpAbs);
    memcpy(stub + sizeof jmpAbs, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const int64_t rel = stub - (site + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("stub out of reach");
    const int32_t rel32 = static_cast<int32_t>(rel);
    g_orig = fn;
    if (!WriteCode(site + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4))) return Fail("could not redirect the call");
    g_site = site;
    g_installed = true;
    Log("[npcbodies] installed: InitArticulated's InstantiatePhysicsAsset_Internal call exe+0x%X -> exe+0x%X (call tree pinned to "
        "build 19115871); scene lock = [[scene+0x%X]+0x%X]",
        static_cast<unsigned>(site - Game().base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(fn) - Game().base), g_solverOff,
        g_lockOff);
    return true;
}


void SetNpcBodiesOneLock(int mode)
{
    if (!g_installed) return;
    const int m = mode ? 1 : 0;
    if (g_mode.exchange(m) == m) return;
    Log("[npcbodies] %s", m ? "on: a character's physics bodies and constraints made under one hold of the scene lock"
                            : "off: every body and constraint takes the scene lock on its own, as in the game");
}

void LogNpcBodiesOneLockStats()
{
    if (!g_installed) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 / static_cast<double>(f.QuadPart);
    static uint64_t last[5] = {};
    const uint64_t now[5] = {s_calls.load(), s_wrapped.load(), s_nested.load(), s_waitTicks.load(), s_innerTicks.load()};
    uint64_t d[5];
    for (int i = 0; i < 5; ++i) { d[i] = now[i] - last[i]; last[i] = now[i]; }
    if (d[0])
        Log("[npcbodies] since last report: %llu InstantiatePhysicsAsset calls under one lock (%llu first holds, %llu already held); "
            "outer acquire %.3f ms each, inside %.3f ms each",
            d[0], d[1], d[2], d[3] * ms / static_cast<double>(d[0]), d[4] * ms / static_cast<double>(d[0]));
    static int64_t last60 = 0;
    const int64_t t = Qpc();
    if (!last60) last60 = t;
    if ((t - last60) * ms < 59500.0) return;
    last60 = t;
    const uint64_t holds = s_holds60.exchange(0), holdTicks = s_holdTicks60.exchange(0), mx = s_maxHold.exchange(0);
    if (holds)
        Log("[npcbodies] 60 s: %llu holds of the scene lock, mean %.3f ms, longest %.3f ms", holds, holdTicks * ms / static_cast<double>(holds),
            mx * ms);
}

// self-test: the hook with a fake InstantiatePhysicsAsset_Internal and offsets
void NpcBodiesTestSetup(void* fakeIpa, uint32_t solverOff, uint32_t lockOff)
{
    g_orig = reinterpret_cast<IpaFn>(fakeIpa);
    g_solverOff = solverOff;
    g_lockOff = lockOff;
    g_installed = true;
}
void* NpcBodiesTestHook() { return reinterpret_cast<void*>(&IpaHook); }
bool NpcBodiesTestFind(void** site, void** fn, uint32_t* solverOff, uint32_t* lockOff)
{
    uint8_t* s = nullptr;
    IpaFn f = nullptr;
    if (!Find(&s, &f, solverOff, lockOff)) return false;
    *site = s;
    *fn = reinterpret_cast<void*>(f);
    return true;
}
}  // namespace opf
