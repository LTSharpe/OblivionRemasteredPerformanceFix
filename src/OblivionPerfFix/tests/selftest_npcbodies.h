// NpcBodiesOneLock self-test, included by selftest.cpp: InitArticulated's call of InstantiatePhysicsAsset_Internal, the pinned functions and the scene lock's
// place on the mapped exe; then the hook against a fake InstantiatePhysicsAsset_Internal (ten argument slots, all forwarded) and
// a fake scene / solver / lock with a real TLS slot:
// off = the game's call alone; on = called inside one write hold (count 1 in the TLS slot's high half, the SRW lock held) and the
// hold released after; nested inside an existing hold = no second SRW acquire; no solver = passed through.
#pragma once

namespace nbl {

alignas(16) uint8_t g_scene[0x100], g_solver[0x400];
alignas(16) uint8_t g_lock[0x20];  // SRWLOCK at +0, TLS index at +8
int g_calls = 0;
HANDLE g_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
uint32_t g_seenWrite = 0;
bool g_seenHeld = false;
uint8_t* g_seenScene = nullptr;
uintptr_t g_seenArgs[10] = {};
void FakeIpa(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uint8_t* scene, uintptr_t a8,
             uintptr_t a9, uintptr_t a10)
{
    ++g_calls;
    const uintptr_t seen[10] = {a1, a2, a3, a4, a5, a6, reinterpret_cast<uintptr_t>(scene), a8, a9, a10};
    memcpy(g_seenArgs, seen, sizeof seen);
    g_seenScene = scene;
    const DWORD idx = *reinterpret_cast<const uint32_t*>(g_lock + 8);
    g_seenWrite = static_cast<uint32_t>(reinterpret_cast<uint64_t>(TlsGetValue(idx)) >> 32);
    const BOOLEAN got = TryAcquireSRWLockShared(reinterpret_cast<PSRWLOCK>(g_lock));
    if (got) ReleaseSRWLockShared(reinterpret_cast<PSRWLOCK>(g_lock));
    g_seenHeld = !got;
}

inline void NpcBodiesSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    void* site = nullptr;
    void* fn = nullptr;
    uint32_t so = 0, lo = 0;
    const bool found = NpcBodiesTestFind(&site, &fn, &so, &lo);
    Check(found && static_cast<uint8_t*>(site) - game.base == 0x36E335F && static_cast<uint8_t*>(fn) - game.base == 0x36E8080 && so == 0x38 &&
              lo == 0x200,
          "npc bodies: InitArticulated's InstantiatePhysicsAsset_Internal call exe+0x36E335F -> exe+0x36E8080 (call tree pinned), lock "
          "[[scene+0x38]+0x200]");
    const DWORD idx = TlsAlloc();
    InitializeSRWLock(reinterpret_cast<PSRWLOCK>(g_lock));
    memcpy(g_lock + 8, &idx, 4);
    uint8_t* const pSolver = g_solver;
    uint8_t* const pLock = g_lock;
    memcpy(g_scene + 0x38, &pSolver, 8);
    memcpy(g_solver + 0x200, &pLock, 8);
    NpcBodiesTestSetup(reinterpret_cast<void*>(&FakeIpa), 0x38, 0x200);
    using H = void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint8_t*, uintptr_t, uintptr_t, uintptr_t);
    const H h = reinterpret_cast<H>(NpcBodiesTestHook());
    auto hook = [&](uint8_t* scene) { h(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, scene, 0x88, 0xFFFFFFFF00000099ull, 0xAA); };
    SetNpcBodiesOneLock(0);
    hook(g_scene);
    const uintptr_t want[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, reinterpret_cast<uintptr_t>(g_scene), 0x88, 0xFFFFFFFF00000099ull, 0xAA};
    Check(g_calls == 1 && g_seenScene == g_scene && g_seenWrite == 0 && !g_seenHeld && !memcmp(g_seenArgs, want, sizeof want),
          "npc bodies off: the game's InstantiatePhysicsAsset_Internal alone, all ten argument slots as passed");
    SetNpcBodiesOneLock(1);
    hook(g_scene);
    const bool freed = TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(g_lock)) != 0;
    if (freed) ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(g_lock));
    Check(g_calls == 2 && g_seenWrite == 1 && g_seenHeld && freed && TlsGetValue(idx) == nullptr,
          "npc bodies on: InstantiatePhysicsAsset_Internal inside one write hold (TLS write count 1, SRW held), released after");
    TlsSetValue(idx, reinterpret_cast<void*>(1ull << 32));  // already holding one write (the game's scope around the call)
    AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(g_lock));
    hook(g_scene);
    const uint64_t after = reinterpret_cast<uint64_t>(TlsGetValue(idx));
    ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(g_lock));
    TlsSetValue(idx, nullptr);
    Check(g_calls == 3 && g_seenWrite == 2 && after == (1ull << 32), "npc bodies on, inside a hold: nested (count 2), back to 1 after");
    memset(g_solver + 0x200, 0, 8);
    hook(g_scene);
    Check(g_calls == 4 && g_seenWrite == 0, "npc bodies on, no lock: passed through");
    hook(nullptr);
    Check(g_calls == 5 && g_seenScene == nullptr, "npc bodies on, no scene: passed through");
    SetNpcBodiesOneLock(0);
    TlsFree(idx);
}

}  // namespace nbl
