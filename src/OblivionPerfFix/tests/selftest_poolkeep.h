// GpuPoolKeepMB self-test, included by selftest.cpp: FD3D12PoolAllocator::CleanUpAllocations and its 20-frame call sites
// on the mapped exe, the keep decision, and the hook run on fake allocators (the allocator's lock, its pools, the adapter's
// frame fence) with a fake clean-up that destroys as the game's does for the frame lag it is given.
#pragma once

namespace pkp {

struct FakePool {
    alignas(16) uint8_t b[0xE0];
};
static uint8_t* g_fakeAllocs[4];
static uint64_t g_lastLag = 0;
static bool g_lastForce = false;
static int g_destroyed = 0;
static const uint64_t* g_completedPtr = nullptr;

static void SetPool(FakePool* p, uint64_t size, uint64_t free, uint64_t lastUsed)
{
    memset(p->b, 0, sizeof p->b);
    *reinterpret_cast<uint64_t*>(p->b + 0x10) = size;
    *reinterpret_cast<uint64_t*>(p->b + 0x28) = free;
    *reinterpret_cast<uint64_t*>(p->b + 0xD8) = lastUsed;
}

// the game's rule for the frame lag it is given: empty and LastUsed + lag <= completed (or forced) -> destroyed (slot nulled)
static void FakeClean(void* allocator, uint64_t lag, bool force)
{
    g_lastLag = lag;
    g_lastForce = force;
    uint8_t* const a = static_cast<uint8_t*>(allocator);
    const uint64_t completed = *g_completedPtr;  // the shared fake fence (a test allocator may carry a broken device chain)
    uint8_t** pools = *reinterpret_cast<uint8_t***>(a + 0x50);
    const int32_t n = *reinterpret_cast<int32_t*>(a + 0x58);
    for (int32_t i = 0; i < n; ++i) {
        uint8_t* p = pools[i];
        if (!p) continue;
        const bool empty = *reinterpret_cast<uint64_t*>(p + 0x10) == *reinterpret_cast<uint64_t*>(p + 0x28);
        if (empty && (force || *reinterpret_cast<uint64_t*>(p + 0xD8) + lag <= completed)) {
            pools[i] = nullptr;
            ++g_destroyed;
        }
    }
}

inline void PoolKeepSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    constexpr uint64_t M = 1ull << 20;
    if (game.timeDateStamp == 0xF19077A4u) {
        uint32_t fn = 0, s[8] = {};
        const int n = GpuPoolKeepTestDiscover(game, &fn, s, 8);
        const bool want = n == 3 && fn == 0x251B2A0 && s[0] == 0x24F3FFA && s[1] == 0x24FFF5A && s[2] == 0x24FFF9F;
        Check(want, "poolkeep: CleanUpAllocations and its three 20-frame call sites (texture x2, buffers; not the upload heap's) match");
        if (!want) printf("    found %d: fn 0x%X sites 0x%X 0x%X 0x%X 0x%X\n", n, fn, s[0], s[1], s[2], s[3]);
        Check(GpuPoolKeepTestLockImport(game, 0x251B2A0), "poolkeep: CleanUpAllocations' lock is KERNEL32 EnterCriticalSection (recursive)");
        uint32_t de = 0, ds = 0;
        Check(GpuPoolKeepTestDefrag(game, &de, &ds) && de == 0x1CBEAFB && ds == 0x1CBEF12,
              "poolkeep: Defrag's switch (+24h) and cursor step (+60h, Pools.Num +58h) match");
    }
    // the decision: ages are frames since last use (all >= 20), most recently used kept first within the budget
    {
        const uint64_t ages[3] = {50, 30, 40}, sizes[3] = {64 * M, 64 * M, 64 * M};
        uint64_t kb = 0;
        int kc = 0;
        uint64_t lag = GpuPoolKeepTestDecide(ages, sizes, 3, 256 * M, &kb, &kc);
        Check(lag >= (1ull << 40) && kb == 192 * M && kc == 3, "poolkeep decide: all fit the budget -> none destroyed");
        lag = GpuPoolKeepTestDecide(ages, sizes, 3, 128 * M, &kb, &kc);
        Check(lag == 50 && kb == 128 * M && kc == 2, "poolkeep decide: over the budget -> only the oldest (50 frames) goes");
        lag = GpuPoolKeepTestDecide(ages, sizes, 3, 0, &kb, &kc);
        Check(lag == 50 && kb == 128 * M && kc == 2, "poolkeep decide: budget 0 -> still one per call (the oldest), the rest later");
        const uint64_t same[3] = {20, 20, 20};
        lag = GpuPoolKeepTestDecide(same, sizes, 3, 0, &kb, &kc);
        Check(lag == 20 && kb == 0 && kc == 0, "poolkeep decide: budget 0, all just 20 frames old -> the game's lag (all go, as the game)");
        const uint64_t mixed[2] = {21, 25}, ms[2] = {32 * M, 64 * M};
        lag = GpuPoolKeepTestDecide(mixed, ms, 2, 48 * M, &kb, &kc);
        Check(lag == 25 && kb == 32 * M && kc == 1, "poolkeep decide: a pool that does not fit is not skipped for an older one");
    }
    // the hook on fake allocators
    alignas(16) static uint8_t fence[0x40], adapter[0x560], device[0x20];
    memset(fence, 0, sizeof fence);
    memset(adapter, 0, sizeof adapter);
    memset(device, 0, sizeof device);
    *reinterpret_cast<uint8_t**>(device + 0x10) = adapter;
    *reinterpret_cast<uint8_t**>(adapter + 0x548) = fence;
    uint64_t& completed = *reinterpret_cast<uint64_t*>(fence + 0x20);
    completed = 1000;
    g_completedPtr = &completed;
    alignas(16) static uint8_t allocA[0x100], allocB[0x100], allocC[0x100], allocD[0x100];
    static FakePool pa[5], pb[1], pc[1];
    static uint8_t* poolsA[5];
    static uint8_t* poolsB[1];
    static uint8_t* poolsC[1];
    const auto makeAlloc = [&](uint8_t* a, uint8_t** pools, int n, bool defrag) {
        memset(a, 0, 0x100);
        a[0x24] = defrag ? 1 : 0;
        InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(a + 0x28));
        *reinterpret_cast<uint8_t***>(a + 0x50) = pools;
        *reinterpret_cast<int32_t*>(a + 0x58) = n;
        *reinterpret_cast<uint8_t**>(a + 0x88) = device;
    };
    const auto freeAlloc = [](uint8_t* a) { DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(a + 0x28)); };
    int32_t& cursorA = *reinterpret_cast<int32_t*>(allocA + 0x60);
    makeAlloc(allocA, poolsA, 5, true);
    makeAlloc(allocB, poolsB, 1, true);
    void* const fake = reinterpret_cast<void*>(&FakeClean);
    GpuPoolKeepTestReset();
    SetGpuPoolKeep(0);
    SetPool(&pa[0], 64 * M, 64 * M, 900);  // empty, 100 frames
    for (int i = 0; i < 5; ++i) poolsA[i] = i == 0 ? pa[0].b : nullptr;
    g_destroyed = 0;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(g_lastLag == 20 && !g_lastForce && g_destroyed == 1, "poolkeep hook: budget 0, nothing kept -> the game's call (lag 20)");
    SetGpuPoolKeep(128);
    SetPool(&pa[0], 64 * M, 64 * M, 950);  // empty 50
    SetPool(&pa[1], 64 * M, 64 * M, 970);  // empty 30
    SetPool(&pa[2], 64 * M, 64 * M, 960);  // empty 40
    SetPool(&pa[3], 64 * M, 10 * M, 999);  // in use
    SetPool(&pa[4], 64 * M, 64 * M, 995);  // empty 5 (the game keeps it too)
    for (int i = 0; i < 5; ++i) poolsA[i] = pa[i].b;
    g_destroyed = 0;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(g_lastLag == 50 && g_destroyed == 1 && !poolsA[0] && poolsA[1] && poolsA[2] && poolsA[4],
          "poolkeep hook: budget 128 MB, three empty 64 MB pools past 20 frames -> the oldest goes, two stay");
    Check(GpuPoolKeepTestKept() == 128 * M, "poolkeep hook: 128 MB counted as kept");
    Check(cursorA == 0 && GpuPoolKeepTestCursorMoves() == 0, "poolkeep defrag: cursor on the pool destroyed now -> left (the game's walk skips the slot)");
    completed = 1001;
    g_destroyed = 0;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(g_lastLag >= (1ull << 40) && g_destroyed == 0, "poolkeep hook: next frame, within the budget -> nothing destroyed");
    SetPool(&pa[1], 64 * M, 0, 1001);  // the 30-frame pool is used again
    completed = 1002;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(GpuPoolKeepTestReused() == 1 && GpuPoolKeepTestKept() == 64 * M, "poolkeep hook: a kept pool used again counts as reused");
    SetPool(&pb[0], 64 * M, 64 * M, 940);  // allocator B: one empty pool, 62 frames
    poolsB[0] = pb[0].b;
    g_destroyed = 0;
    GpuPoolKeepTestCall(allocB, 20, false, fake);
    Check(g_destroyed == 0 && GpuPoolKeepTestKept() == 128 * M, "poolkeep hook: a second allocator shares the budget (64 + 64 MB)");
    // S1: ranking by last use across allocators: A's newer pool stays (A keeps both), B's older one goes at B's next call
    SetPool(&pa[4], 64 * M, 64 * M, 970);  // A: another pool now 32 frames old -> over the shared budget
    g_destroyed = 0;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(g_destroyed == 0 && poolsA[2] && poolsA[4], "poolkeep S1: A's two pools are the most recently used -> A keeps both");
    GpuPoolKeepTestCall(allocB, 20, false, fake);
    Check(g_destroyed == 1 && !poolsB[0] && g_lastLag == 62 && GpuPoolKeepTestKept() == 128 * M,
          "poolkeep S1: B's pool is the least recently used of all -> B gives it back at its own call; 128 MB kept");
    SetGpuPoolKeep(0);
    completed = 1003;
    g_destroyed = 0;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    GpuPoolKeepTestCall(allocB, 20, false, fake);
    Check(g_destroyed == 1 && !poolsA[2] && poolsA[4] && GpuPoolKeepTestKept() == 64 * M,
          "poolkeep hook: budget 0 -> A gives back its oldest only (one per allocator per call)");
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(g_destroyed == 2 && !poolsA[4] && GpuPoolKeepTestKept() == 0, "poolkeep hook: ... and its last at the next call");
    SetPool(&pa[0], 64 * M, 64 * M, 900);
    poolsA[0] = pa[0].b;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(g_lastLag == 20, "poolkeep hook: all given back -> the game's call again");
    GpuPoolKeepTestCall(allocA, 0, true, fake);
    Check(g_lastLag == 0 && g_lastForce, "poolkeep hook: a forced clean-up passes unchanged");
    SetGpuPoolKeep(128);
    GpuPoolKeepTestCall(allocA, 7, false, fake);
    Check(g_lastLag == 7 && !g_lastForce, "poolkeep hook: another frame lag passes unchanged");
    // S3: an allocator without defragmentation (pools in index order) is left as the game has it
    makeAlloc(allocC, poolsC, 1, false);
    SetPool(&pc[0], 4 * M, 4 * M, 903);
    poolsC[0] = pc[0].b;
    g_destroyed = 0;
    GpuPoolKeepTestCall(allocC, 20, false, fake);
    Check(g_lastLag == 20 && g_destroyed == 1 && GpuPoolKeepTestKept() == 0, "poolkeep S3: allocator without defragmentation -> the game's call");
    freeAlloc(allocC);
    // B2: Defrag's cursor (+60h) moves past kept pools only
    GpuPoolKeepTestReset();
    SetGpuPoolKeep(256);
    completed = 1000;
    SetPool(&pa[0], 64 * M, 10 * M, 999);  // in use
    SetPool(&pa[1], 64 * M, 64 * M, 970);  // empty 30
    SetPool(&pa[2], 64 * M, 64 * M, 960);  // empty 40
    SetPool(&pa[3], 64 * M, 64 * M, 990);  // empty 10 (young: the game keeps it, its walk parks there as in the game)
    SetPool(&pa[4], 64 * M, 10 * M, 999);  // in use
    for (int i = 0; i < 5; ++i) poolsA[i] = pa[i].b;
    cursorA = 1;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(cursorA == 3 && GpuPoolKeepTestCursorMoves() == 1 && poolsA[1] && poolsA[2],
          "poolkeep B2: cursor on a kept drained pool -> past both kept pools (to the young empty one, as the game's walk)");
    cursorA = 0;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(cursorA == 0 && GpuPoolKeepTestCursorMoves() == 1, "poolkeep B2: cursor on a pool in use -> left");
    cursorA = 5;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(cursorA == 5, "poolkeep B2: cursor at the end, pool 0 in use -> left (Defrag starts over at 0 itself)");
    SetPool(&pa[0], 64 * M, 64 * M, 975);  // pool 0 now kept too
    cursorA = 5;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(cursorA == 3, "poolkeep B2: cursor at the end -> starts over at 0 and moves past the kept pools 0-2");
    // SF1: empty slots between the cursor and a kept pool are passed with it; trailing empty slots are left to the game's walk
    poolsA[1] = nullptr;
    cursorA = 1;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(cursorA == 3 && poolsA[2], "poolkeep SF1: cursor on an empty slot, a kept pool behind it -> past both");
    SetPool(&pa[2], 64 * M, 10 * M, 999);  // in use
    cursorA = 1;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(cursorA == 1, "poolkeep SF1: cursor on an empty slot, a pool in use behind it -> left");
    SetPool(&pa[2], 64 * M, 64 * M, 960);  // empty 40 again
    poolsA[1] = pa[1].b;
    SetGpuPoolKeep(64);
    cursorA = 2;
    g_destroyed = 0;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(g_destroyed == 1 && !poolsA[2] && cursorA == 2, "poolkeep B2: cursor on the pool given back now -> left (its slot is empty)");
    // B1: more empty pools than fit, in one allocator: one destroyed per call (the oldest), never all at once
    static FakePool many[600];
    static uint8_t* poolsD[600];
    for (int listLen : {70, 600}) {
        GpuPoolKeepTestReset();
        SetGpuPoolKeep(128);
        completed = 2000;
        for (int i = 0; i < listLen; ++i) {
            SetPool(&many[i], 4 * M, 4 * M, 2000 - (21 + i));  // ages 21 .. 20 + listLen
            poolsD[i] = many[i].b;
        }
        makeAlloc(allocD, poolsD, listLen, true);
        g_destroyed = 0;
        GpuPoolKeepTestCall(allocD, 20, false, fake);
        const bool one = g_destroyed == 1 && !poolsD[listLen - 1] && g_lastLag == static_cast<uint64_t>(20 + listLen);
        GpuPoolKeepTestCall(allocD, 20, false, fake);
        Check(one && g_destroyed == 2 && !poolsD[listLen - 2],
              listLen == 70 ? "poolkeep B1: 70 old empty 4 MB pools, 32 fit -> one destroyed per call (the oldest)"
                            : "poolkeep B1: 600 old empty pools (more than listed) -> one destroyed per call (the oldest)");
        freeAlloc(allocD);
    }
    // N1: the frame fence cannot be read -> the key turns off; kept pools given back one per call from the last good reading
    GpuPoolKeepTestReset();
    SetGpuPoolKeep(256);
    completed = 1000;
    SetPool(&pa[0], 64 * M, 64 * M, 970);  // empty 30
    SetPool(&pa[1], 64 * M, 64 * M, 960);  // empty 40
    for (int i = 0; i < 5; ++i) poolsA[i] = i < 2 ? pa[i].b : nullptr;
    g_destroyed = 0;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    alignas(16) static uint8_t badDevice[0x20];
    memset(badDevice, 0, sizeof badDevice);  // adapter pointer null: the fence read faults
    *reinterpret_cast<uint8_t**>(allocA + 0x88) = badDevice;
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(g_destroyed == 1 && !poolsA[1] && poolsA[0] && GpuPoolKeepTestFailed(),
          "poolkeep N1: fence read fails -> off; the oldest kept pool goes, not all at once");
    GpuPoolKeepTestCall(allocA, 20, false, fake);
    Check(g_destroyed == 2 && !poolsA[0] && GpuPoolKeepTestKept() == 0, "poolkeep N1: ... the next one at the next call");
    *reinterpret_cast<uint8_t**>(allocA + 0x88) = device;
    // S2: video memory + the budget over the DXGI budget gives back (512 MB hysteresis); MemoryRelief too; budget unknown: kept
    {
        GpuPoolKeepTestReset();
        const double G = 1073741824.0;
        const auto b = [&](double gb) { return static_cast<uint64_t>(gb * G); };
        bool s2 = GpuPoolKeepBudgetStep(256, false, b(10.6), b(12.48)) == 256;
        s2 = s2 && GpuPoolKeepBudgetStep(256, false, b(12.3), b(12.48)) == 0;
        s2 = s2 && GpuPoolKeepBudgetStep(256, false, b(11.9), b(12.48)) == 0;
        s2 = s2 && GpuPoolKeepBudgetStep(256, false, b(11.5), b(12.48)) == 256;
        s2 = s2 && GpuPoolKeepBudgetStep(256, true, b(10.0), b(12.48)) == 0;
        s2 = s2 && GpuPoolKeepBudgetStep(256, false, b(10.0), 0) == 256;
        Check(s2, "poolkeep S2: given back while video memory + 256 MB is over the budget (back 512 MB below), under MemoryRelief");
    }
    SetGpuPoolKeep(0);
    GpuPoolKeepTestReset();
    freeAlloc(allocA);
    freeAlloc(allocB);
    // the install on the mapped exe: before game code only; then the sites call the hook
    if (game.timeDateStamp == 0xF19077A4u) {
        Check(!InstallGpuPoolKeep(false), "poolkeep: refuses to install after game code started");
        if (Installed(InstallGpuPoolKeep(true), "poolkeep: installs before game code")) {
            uint32_t fn = 0, s[8] = {};
            Check(GpuPoolKeepTestDiscover(game, &fn, s, 8) == 0, "poolkeep: after the install no 20-frame call goes to the game's clean-up directly");
            const uint8_t* const site = game.base + 0x24FFF5A;
            const uint8_t* const stub = site + 5 + *reinterpret_cast<const int32_t*>(site + 1);
            const bool jmp = stub[0] == 0xFF && stub[1] == 0x25 && *reinterpret_cast<const int32_t*>(stub + 2) == 0;
            Check(jmp && GpuPoolKeepTestInstalled(), "poolkeep: the texture loop's call goes to the stub (jmp [rip]) -> hook");
        }
    }
}

}  // namespace pkp
