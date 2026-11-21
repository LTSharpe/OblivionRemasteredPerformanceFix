// GcPrepAhead self-test (worker gcprepbuild, 2026-11-19, S-041), included by selftest.cpp after selftest_gchold.h (it drives
// GcPassAfterBusy's hook with that file's fake engine). On the mapped exe (Steam 19115871): OnPreGarbageCollect exe+0x33261D0,
// PrepareStreamedOutLevelForGC exe+0x3326890, FWeakObjectPtr::Get exe+0x1126290, RequestUnload's world-tick test exe+0x332AB4B,
// UpdateLevelStreaming's pending-level trigger exe+0x3874138, LevelsPendingUnload.Num exe+0x925F7A0, NumberOfPrepared
// exe+0x925F90C, the continuous count exe+0x8E41690, the low-memory count exe+0x8E41698, GIsLowMemory exe+0x927E7A4, GEngine
// exe+0x927E7D8; UEngine::WorldList +0xF80 / +0xF88, FWorldContext::ThisCurrentWorld +0x2C0, UWorld::bInTick +0x13B bit 2;
// the GC lock (gcprepfix): TryGCLock inlined in TryCollectGarbage at exe+0x106E48B, ReleaseGCLock exe+0x106BC10,
// GGCSingleton exe+0x9109100, the EnterCriticalSection / LeaveCriticalSection import slots exe+0x6D96C50 / exe+0x6D96C48.
// Then the step with a fake pending list, frame by frame through the engine's timer-path order.
#pragma once

namespace gcprep_test {
// a fake UObject chain: level -> world -> package (UObjectBase: NamePrivate +0x18, OuterPrivate +0x20)
struct alignas(8) Obj {
    uint8_t b[0x28];
};
constexpr int kLevels = 96;
Obj g_lv[kLevels], g_world[kLevels], g_pkg[kLevels];
Obj g_newLv, g_newWorld, g_newPkg;  // a level built anew for package 5
uint64_t g_store[128];
uint64_t* g_data = g_store;
int32_t g_num = 0, g_prepared = 0, g_soft = 64;
uint8_t g_lowMem = 0;
std::vector<uint64_t> g_order;   // keys prepared, in order
bool g_stale[kLevels] = {};
int g_mutate = 0;                // 1: the next Prepare removes entry key 40 and appends key 41
int g_spinUs = 0;                // each Prepare takes this long
bool g_lockBusy = false;         // another thread holds an FGCScopeGuard: TryGCLock refuses
int g_lockDepth = 0, g_unlockedPreps = 0, g_lockTakes = 0, g_lockLimit = 1 << 30;  // refused once taken g_lockLimit times
bool FakeTryLock() { if (g_lockBusy || g_lockDepth || g_lockTakes >= g_lockLimit) return false; ++g_lockDepth; ++g_lockTakes; return true; }
void FakeUnlock() { --g_lockDepth; }
// a fake engine with one world context
alignas(8) uint8_t g_eng[0x1000], g_ctx[0x300], g_uworld[0x200];
uint8_t* g_ctxList[1] = {g_ctx};
void* g_engPtr = g_eng;

uint64_t Key(int i) { return (static_cast<uint64_t>(1000 + i) << 32) | static_cast<uint32_t>(i); }  // SerialNumber : ObjectIndex
int IndexOf(uint64_t key) { return static_cast<int>(key & 0xFFFFFFFF); }
void SetObj(Obj& o, const void* outer, uint64_t name)
{
    memset(o.b, 0, sizeof o.b);
    memcpy(o.b + 0x18, &name, 8);
    memcpy(o.b + 0x20, &outer, 8);
}
void* FakeGet(const void* weak)
{
    uint64_t k;
    memcpy(&k, weak, 8);
    const int i = IndexOf(k);
    if (i < 0 || i >= kLevels || g_stale[i] || k != Key(i)) return nullptr;
    return &g_lv[i];
}
void FakePrep(void* level)
{
    if (g_lockDepth != 1) ++g_unlockedPreps;  // every early preparation runs under the GC lock
    ++g_prepared;
    const int i = static_cast<int>(static_cast<Obj*>(level) - g_lv);
    g_order.push_back(Key(i));
    if (g_spinUs) {
        LARGE_INTEGER f, t0, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        do QueryPerformanceCounter(&t); while ((t.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart < g_spinUs);
    }
    if (g_mutate == 1) {  // the unloaded event's listeners change the list: one entry taken back, one added
        g_mutate = 0;
        for (int32_t j = 0; j < g_num; ++j)
            if (g_data[j] == Key(40)) { memmove(g_data + j, g_data + j + 1, (g_num - j - 1) * 8); --g_num; break; }
        g_data[g_num++] = Key(41);
    }
}
void Push(int i) { g_data[g_num++] = Key(i); }
bool Has(int i) { for (int32_t j = 0; j < g_num; ++j) if (g_data[j] == Key(i)) return true; return false; }
int Prepped(int i) { int n = 0; for (uint64_t k : g_order) n += k == Key(i); return n; }

void Setup(int key, int maxPerFrame = 2, int budgetUs = 500)
{
    using namespace gchold_test;
    GhReset(0);  // GcPassAfterBusy installed (fakes), its key off: only the shared hook
    for (int i = 0; i < kLevels; ++i) {
        SetObj(g_pkg[i], nullptr, 0x5000 + static_cast<uint64_t>(i));
        SetObj(g_world[i], &g_pkg[i], 0x7000 + static_cast<uint64_t>(i));
        SetObj(g_lv[i], &g_world[i], 0x9000 + static_cast<uint64_t>(i));
        g_stale[i] = false;
    }
    SetObj(g_newPkg, nullptr, 0x5000 + 5);
    SetObj(g_newWorld, &g_newPkg, 0x7777);
    SetObj(g_newLv, &g_newWorld, 0x9999);
    g_num = g_prepared = 0;
    g_soft = 64;
    g_lowMem = 0;
    g_order.clear();
    g_mutate = g_spinUs = 0;
    g_lockBusy = false;
    g_lockDepth = g_unlockedPreps = g_lockTakes = 0;
    g_lockLimit = 1 << 30;
    memset(g_eng, 0, sizeof g_eng);
    memset(g_ctx, 0, sizeof g_ctx);
    memset(g_uworld, 0, sizeof g_uworld);
    uint8_t** list = g_ctxList;
    memcpy(g_eng + 0xF80, &list, 8);
    const int32_t one = 1;
    memcpy(g_eng + 0xF88, &one, 4);
    uint8_t* w = g_uworld;
    memcpy(g_ctx + 0x2C0, &w, 8);
    GcPrepAheadFakes k{};
    k.num = &g_num; k.data = &g_data; k.prepared = &g_prepared; k.get = &FakeGet; k.prep = &FakePrep;
    k.softCount = &g_soft; k.lowMemory = &g_lowMem; k.engine = &g_engPtr;
    k.worldList = 0xF80; k.worldNum = 0xF88; k.ctxWorld = 0x2C0; k.inTickOff = 0x13B; k.inTickMask = 4;
    k.now = &GhNow; k.frame = &GhFrame; k.menu = &GhMenu;
    k.tryLock = &FakeTryLock; k.unlock = &FakeUnlock;
    GcPrepAheadTestSetup(k);
    SetGcPrepAhead(key, 3000, maxPerFrame, budgetUs, 1000);
}
// One frame: the engine's timer path (our hook inside), then the pass's own preparation when it ran ('P')
char Frame()
{
    const char r = gchold_test::GhEngineFrame();
    if (r == 'P') {  // OnPreGarbageCollect: prepare the rest, Empty, NumberOfPrepared = 0
        for (int32_t j = 0; j < g_num; ++j) g_order.push_back(g_data[j]);
        g_num = 0;
        g_prepared = 0;
    }
    return r;
}
void Frames(int n) { for (int i = 0; i < n; ++i) Frame(); }
// TimeSinceLastPendingKillPurge set to `before` seconds before the pass falls due
void Before(double s) { gchold_test::GhTsl() = static_cast<float>(gchold_test::g_interval - s); }
// a lock object laid out as UE 5.3's FGCCSyncObject: AsyncCounter, GCCounter, GCWantsToRunCounter, Critical, GCUnlockedEvent
struct FakeSync {
    volatile LONG async, gc, wants, pad;
    CRITICAL_SECTION cs;
    void* event;
};
static_assert(offsetof(FakeSync, cs) == 0x10 && offsetof(FakeSync, event) == 0x38, "FGCCSyncObject layout");
FakeSync g_sync;
int g_resets = 0, g_triggers = 0;
void FakeEvTrigger(void*) { ++g_triggers; }
void FakeEvReset(void*) { ++g_resets; }
void* g_evVt[6] = {nullptr, nullptr, reinterpret_cast<void*>(&FakeEvTrigger), reinterpret_cast<void*>(&FakeEvReset), nullptr, nullptr};
void* g_ev[1] = {g_evVt};
void* g_syncPtr = &g_sync;
void FakeRelease() { ++g_triggers; InterlockedDecrement(&g_sync.gc); }  // ReleaseGCLock: Trigger, GCCounter--
}  // namespace gcprep_test

static void GcPrepRun(const GameModule& game)
{
    using namespace gcprep_test;
    using gchold_test::g_nowUs;
    if (game.timeDateStamp == 0xF19077A4u) {
        uint32_t r[16] = {}, off[5] = {};
        const char* why = "";
        const bool ok = GcPrepAheadTestDiscover(game, r, off, &why);
        Check(ok && r[0] == 0x33261D0 && r[1] == 0x3326890 && r[2] == 0x1126290 && r[3] == 0x332AB4B && r[4] == 0x3874138 &&
                  r[5] == 0x925F7A0 && r[6] == 0x925F90C && r[7] == 0x8E41690 && r[8] == 0x8E41698 && r[9] == 0x927E7A4 &&
                  r[10] == 0x927E7D8 && off[0] == 0xF80 && off[1] == 0xF88 && off[2] == 0x2C0 && off[3] == 0x13B && off[4] == 4,
              "GcPrepAhead: OnPreGarbageCollect's loop, PrepareStreamedOutLevelForGC, Get, RequestUnload's world-tick test, the "
              "pending-level trigger, the list, NumberOfPrepared, the counts, GIsLowMemory, GEngine and the world list found");
        Check(ok && r[11] == 0x106E48B && r[12] == 0x106BC10 && r[13] == 0x9109100 && r[14] == 0x6D96C50 && r[15] == 0x6D96C48,
              "GcPrepAhead: the engine's GC lock found (TryGCLock in TryCollectGarbage, ReleaseGCLock, GGCSingleton, the "
              "critical-section imports)");
        if (!ok || r[1] != 0x3326890 || r[13] != 0x9109100)
            printf("    found %d (%s): %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X; %X %X %X %X %X\n", ok, why, r[0], r[1], r[2], r[3],
                   r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15], off[0], off[1], off[2], off[3], off[4]);
        // GetNumLevelsPendingPurge = Num + Prepared, read from the same two globals (exe+0x3322470)
        const uint8_t* gn = game.base + 0x3322470;
        Check(RipTarget(gn, 2, 6) == game.base + 0x925F7A0 && RipTarget(gn + 6, 2, 6) == game.base + 0x925F90C,
              "GcPrepAhead: GetNumLevelsPendingPurge sums the list's Num and NumberOfPrepared that the step keeps constant");
        // refusals on altered bytes of the mapped copy (copy-on-write), each put back: PrepareStreamedOutLevelForGC's
        // NumberOfPrepared increment pointing elsewhere; RequestUnload's tail jump to another function
        auto alter = [&](uint8_t* at, int32_t delta) {
            DWORD old = 0;
            if (!VirtualProtect(at, 4, PAGE_EXECUTE_WRITECOPY, &old) && !VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return -1;
            int32_t v;
            memcpy(&v, at, 4);
            const int32_t w = v + delta;
            memcpy(at, &w, 4);
            const bool found = GcPrepAheadTestFindOn(game);
            memcpy(at, &v, 4);
            DWORD o2;
            VirtualProtect(at, 4, old, &o2);
            return found ? 1 : 0;
        };
        const int a1 = alter(game.base + 0x3326890 + 0x2A, 4);
        const size_t itLen = 106;  // kInTick's length; its last byte is the E9
        const int a2 = alter(game.base + 0x332AB4B + itLen, 0x10);
        const int a3 = alter(game.base + 0x3874138 + 0x0C, 4);  // the trigger's count: still in .data, so found
        const int a4 = alter(game.base + 0x106BC10 + 0x09, 8);  // ReleaseGCLock reading another lock object
        Check(a1 == 0 && a2 == 0 && a3 == 1 && a4 == 0 && GcPrepAheadTestFindOn(game),
              "GcPrepAhead: refused when PrepareStreamedOutLevelForGC counts another global, RequestUnload's branch leads "
              "elsewhere or ReleaseGCLock unlocks another lock object; found again once restored");
    }
    {
        static uint8_t zero[0x4000];
        GameModule fake = game;
        fake.text = Section{zero, zero + sizeof zero};
        Check(!GcPrepAheadTestFindOn(fake), "GcPrepAhead: an exe without the GC pass's level preparation is refused");
    }
    uint64_t c[32];
    // remove-by-value on the fake list: order kept, Num decremented, absent refused
    Setup(1);
    for (int i = 0; i < 5; ++i) Push(i);
    const bool r1 = GcPrepAheadTestRemove(Key(2)), r2 = GcPrepAheadTestRemove(Key(4)), r3 = GcPrepAheadTestRemove(Key(9)),
               r4 = GcPrepAheadTestRemove(Key(0));
    Check(r1 && r2 && !r3 && r4 && g_num == 2 && g_data[0] == Key(1) && g_data[1] == Key(3),
          "GcPrepAhead: removal by value keeps the order (middle, last, first) and refuses an entry not in the list");
    // off: nothing prepared across a whole window and pass
    Setup(0);
    for (int i = 0; i < 10; ++i) Push(i);
    g_nowUs += 10000000;
    Before(0.5);
    Frames(60);
    Check(g_order.size() == 10 && g_prepared == 0 && g_num == 0, "GcPrepAhead: key 0 prepares nothing; the pass prepares all");
    // on: outside the window nothing; inside, two a frame, oldest first, only those pending >= 3 s
    Setup(1);
    for (int i = 0; i < 10; ++i) Push(i);
    Before(20.0);
    Frame();                 // first seen now
    g_nowUs += 4000000;      // they are 4 s old
    for (int i = 10; i < 13; ++i) Push(i);  // young ones
    Before(5.0);
    Frames(3);
    const size_t outside = g_order.size();
    Before(0.9);
    Frame();
    const bool firstTwo = g_order.size() == 2 && g_order[0] == Key(0) && g_order[1] == Key(1);
    const bool sumKept = g_num + g_prepared == 13 && g_prepared == 2;
    Frames(4);
    Check(outside == 0 && firstTwo && sumKept && g_order.size() == 10 && !Has(0) && !Has(9) && Has(10) && Has(12) && g_num == 3,
          "GcPrepAhead: nothing outside the window; inside, two levels a frame, oldest first, only those pending >= 3 s; "
          "Num + NumberOfPrepared unchanged");
    // the pass runs when due: it prepares the young rest; the next frame sees it ran
    int guard = 0;
    while (Frame() != 'P' && ++guard < 200) {}
    Frame();
    GcPrepAheadTestCounts(c);
    Check(guard < 200 && g_order.size() == 13 && Prepped(10) == 1 && Prepped(0) == 1 && c[0] == 10 && c[3] == 1 && c[4] == 1 && c[5] == 3,
          "GcPrepAhead: the pass prepares only the rest (3 young levels), each level is prepared once; the pass and the levels "
          "left for it are counted");
    Check(g_lockTakes == 10 && g_unlockedPreps == 0 && g_lockDepth == 0 && c[6] == 0 && c[26] == 0,
          "GcPrepAhead: each early preparation runs holding the engine's GC lock, released after it; no late pass");
    LogGcPrepAheadStats();
    // a due frame is never used (the pass runs then); a held pass (game's own delay) likewise
    Setup(1);
    for (int i = 0; i < 4; ++i) Push(i);
    Before(20.0); Frame();
    g_nowUs += 4000000;
    gchold_test::GhTsl() = static_cast<float>(gchold_test::g_interval + 0.5);
    gchold_test::GhFlag() = 1;  // the game delays it this frame
    Frame();
    GcPrepAheadTestCounts(c);
    Check(g_order.empty() && c[14 + 0] == 1, "GcPrepAhead: never in a frame where the pass is due (also when held a frame)");
    // the gates: purge pending, menu, low memory, async loading, a world in tick, frame state unknown, long frame, busy
    auto gateRun = [&](int which) {
        Setup(1);
        for (int i = 0; i < 4; ++i) Push(i);
        Before(20.0); Frame();
        g_nowUs += 4000000;
        Before(0.5);
        switch (which) {
        case 1: gchold_test::g_purge = true; break;
        case 2: gchold_test::g_menuMode = 1; break;
        case 3: g_lowMem = 1; break;
        case 4: gchold_test::g_async = true; break;
        case 5: g_uworld[0x13B] = 4; break;
        case 6: gchold_test::g_frameKnown = false; break;
        case 7: gchold_test::g_elapsedUs = 12500; break;
        case 8: GcPassAfterBusyNoteStream(1); break;
        case 10: g_lockBusy = true; break;
        }
        Frame();
        GcPrepAheadTestCounts(c);
        const bool refused = g_order.empty() && c[14 + which] == 1;
        gchold_test::g_purge = gchold_test::g_async = false; gchold_test::g_menuMode = 0; gchold_test::g_frameKnown = true;
        gchold_test::g_elapsedUs = 9000; g_lowMem = 0; g_uworld[0x13B] = 0; g_lockBusy = false;
        Frames(3);  // then it goes ahead
        return refused && g_order.size() == 4;
    };
    bool gates = true;
    for (int w = 1; w <= 10; ++w) {
        if (w == 9) continue;  // "none old enough" has its own test
        const bool ok = gateRun(w);
        if (!ok) printf("    gate %d failed\n", w);
        gates = gates && ok;
    }
    Check(gates, "GcPrepAhead: refused while a purge is pending, a menu or loading screen is up, memory is low, files are "
                 "loading, a world is in its tick, the frame state is unknown, the frame has taken 12 ms, a cell was just "
                 "removed, or another thread holds the GC lock; it goes ahead once the frame is calm");
    // the GC lock refused after the first level of a frame: that frame stops, the next goes on
    Setup(1);
    for (int i = 0; i < 4; ++i) Push(i);
    Before(20.0); Frame();
    g_nowUs += 4000000;
    Before(0.5);
    g_lockLimit = 1;
    Frame();
    GcPrepAheadTestCounts(c);
    const bool stopped = g_order.size() == 1 && c[28] == 1 && g_lockDepth == 0;
    g_lockLimit = 1 << 30;
    Frame();
    Check(stopped && g_order.size() == 3 && g_unlockedPreps == 0,
          "GcPrepAhead: a GC lock refused after a level stops the frame (counted); the next frame goes on");
    // a stale entry is left for the pass; the next one is prepared
    Setup(1);
    for (int i = 0; i < 4; ++i) Push(i);
    g_stale[0] = true;
    Before(20.0); Frame();
    g_nowUs += 4000000;
    Before(0.5);
    Frame();
    GcPrepAheadTestCounts(c);
    Check(Has(0) && Prepped(1) == 1 && Prepped(2) == 1 && c[8] == 1, "GcPrepAhead: a stale weak pointer is left for the pass, the next ones are prepared");
    // the list changes during the call: removal by value still right, the new entry is young, the taken-back one counted
    Setup(1);
    for (int i = 0; i < 4; ++i) Push(i);
    Push(40);
    Before(20.0); Frame();
    g_nowUs += 4000000;
    Before(0.5);
    g_mutate = 1;
    Frame();
    GcPrepAheadTestCounts(c);
    Check(!Has(0) && !Has(1) && Has(2) && Has(3) && !Has(40) && Has(41) && g_num == 3 && g_prepared == 2 && c[10] == 1 && c[9] == 0,
          "GcPrepAhead: a list changed during the call (one taken back, one added) is handled by value; the taken-back entry "
          "is counted as the game's reuse");
    // the game takes back an old entry between frames (CancelUnloadRequest): counted, never prepared
    Setup(1);
    for (int i = 0; i < 4; ++i) Push(i);
    Before(20.0); Frame();
    g_nowUs += 4000000;
    GcPrepAheadTestRemove(Key(1));
    Before(0.5);
    Frames(2);
    GcPrepAheadTestCounts(c);
    Check(Prepped(1) == 0 && c[10] == 1 && g_order.size() == 3, "GcPrepAhead: an entry the game took back is never prepared (counted as reuse)");
    // budget: with 8 a frame allowed and 300 us a level, the frame stops after 500 us (two levels)
    Setup(1, 8, 500);
    for (int i = 0; i < 6; ++i) Push(i);
    Before(20.0); Frame();
    g_nowUs += 4000000;
    Before(0.5);
    g_spinUs = 300;
    Frame();
    g_spinUs = 0;
    GcPrepAheadTestCounts(c);
    Check(g_order.size() == 2 && c[7] == 1, "GcPrepAhead: a frame stops once its preparations took the budget (500 us)");
    // World Partition's soft pass: open only when at most two levels (maxPerFrame) are still needed and those came within
    // the window; closed with more needed, when they came slowly, and without a soft trigger
    Setup(1);
    g_soft = 64;
    for (int i = 0; i < 56; ++i) Push(i);
    Before(30.0); Frame();
    g_nowUs += 4000000;
    size_t softFar = 0;
    for (int i = 56; i < 62; ++i) {  // 6 levels in 0.6 s; 8, 7, ... 3 still needed: closed; then 2: open
        if (i == 61) softFar = g_order.size();
        Push(i); Frame(); g_nowUs += 100000;
    }
    const size_t softFast = g_order.size();
    GcPrepAheadTestCounts(c);
    const uint64_t softWin = c[25];
    Setup(1);
    for (int i = 0; i < 56; ++i) Push(i);
    Before(30.0); Frame();
    g_nowUs += 4000000;
    for (int i = 56; i < 62; ++i) { Push(i); Frame(); g_nowUs += 2000000; }  // 6 levels in 12 s
    Frame();
    const size_t softSlow = g_order.size();
    g_soft = 0;  // no soft trigger in this world: the timer rule only
    Frames(3);
    const size_t softNone = g_order.size();
    Check(softFar == 0 && softFast == 2 && softWin == 1 && softSlow == 0 && softNone == 0 && g_num + g_prepared == 62,
          "GcPrepAhead: the soft trigger's window opens only when the pass is at most two levels away and those came within a "
          "second; not further away, not when they came slowly, not without a soft trigger");
    // a soft window whose levels then stopped coming: the timer pass comes > 2 s later = a late pass; the next cycle opens
    // no soft window
    Setup(1);
    g_soft = 64;
    for (int i = 0; i < 56; ++i) Push(i);
    Before(30.0); Frame();
    g_nowUs += 4000000;
    for (int i = 56; i < 62; ++i) { Push(i); Frame(); g_nowUs += 100000; }  // soft window: 2 prepared
    const size_t lateFirst = g_order.size();
    g_nowUs += 3000000;  // no more levels; 3 s later the timer pass
    gchold_test::GhTsl() = static_cast<float>(gchold_test::g_interval + 0.5);
    guard = 0;
    while (Frame() != 'P' && ++guard < 50) {}
    Frame();  // the pass is seen
    GcPrepAheadTestCounts(c);
    const bool lateSeen = c[26] == 1 && c[6] == 0 && c[27] == 1;
    const size_t afterPass = g_order.size();
    // next cycle: the same 62 levels again, the last two needed arrive fast: paused, no soft window
    for (int i = 0; i < 56; ++i) Push(i);
    Before(30.0); Frame();
    g_nowUs += 4000000;
    for (int i = 56; i < 62; ++i) { Push(i); Frame(); g_nowUs += 100000; }
    const size_t paused = g_order.size() - afterPass;
    Check(lateFirst == 2 && guard < 50 && afterPass == 62 && lateSeen && paused == 0,
          "GcPrepAhead: a pass > 2 s after its soft-window preparations is counted late and pauses the soft window for the "
          "next cycle");
    // "wanted again": a level prepared early whose package is added to the world again before the pass, then after it
    Setup(1);
    for (int i = 0; i < 8; ++i) Push(i);
    Before(20.0); Frame();
    g_nowUs += 4000000;
    Before(0.9);
    Frames(4);
    GcPrepAheadNoteAdd(&g_newLv);  // package of level 5 built anew
    GcPrepAheadNoteAdd(&g_newLv);  // the same call again (time-sliced AddToWorld): counted once
    GcPrepAheadNoteAdd(&g_lv[20]); // never prepared
    GcPrepAheadTestCounts(c);
    const bool before = c[12] == 1 && Prepped(5) == 1;
    while (Frame() != 'P' && ++guard < 400) {}
    Frame();
    static Obj other;
    SetObj(other, &g_world[6], 0x9997);
    GcPrepAheadNoteAdd(&other);  // package of level 6, prepared early, added within 2 s after the pass
    GcPrepAheadTestCounts(c);
    Check(before && c[13] == 1, "GcPrepAhead: a level prepared early and built again is counted before the pass and within 2 s after it");
    // the real TryGCLock (as TryCollectGarbage inlines it) on a fake lock object: free -> taken (Reset, GCCounter 1);
    // again -> refused (not recursive); released by ReleaseGCLock; another thread inside FGCScopeGuard -> refused, untouched
    {
        memset(&g_sync, 0, sizeof g_sync);
        InitializeCriticalSection(&g_sync.cs);
        g_sync.event = g_ev;
        g_resets = g_triggers = 0;
        void* const* sp = &g_syncPtr;
        const bool t1 = GcPrepAheadTestTryLock(sp, &FakeRelease, false);
        const bool held = g_sync.gc == 1 && g_resets == 1;
        const bool t2 = GcPrepAheadTestTryLock(sp, &FakeRelease, false);
        FakeRelease();
        const bool freed = g_sync.gc == 0 && g_triggers == 1;
        g_sync.async = 1;
        const bool t3 = GcPrepAheadTestTryLock(sp, &FakeRelease, true);
        const bool untouched = g_sync.gc == 0 && g_resets == 1 && g_triggers == 1;
        g_sync.async = 0;
        const bool t4 = GcPrepAheadTestTryLock(sp, &FakeRelease, true);
        const bool cycle = g_sync.gc == 0 && g_resets == 2 && g_triggers == 2;
        void* nul = nullptr;
        const bool t5 = GcPrepAheadTestTryLock(&nul, &FakeRelease, true);
        DeleteCriticalSection(&g_sync.cs);
        Check(t1 && held && !t2 && freed && !t3 && untouched && t4 && cycle && !t5,
              "GcPrepAhead: the engine's TryGCLock as rebuilt takes a free GC lock (event reset, GCCounter 1), refuses it while "
              "held or while another thread is inside an FGCScopeGuard, and the release frees it; no lock object = refused");
    }
    // live off in the middle of a window: no further preparation
    Setup(1);
    for (int i = 0; i < 8; ++i) Push(i);
    Before(20.0); Frame();
    g_nowUs += 4000000;
    Before(0.9);
    Frame();
    SetGcPrepAhead(0, 3000, 2, 500, 1000);
    Frames(3);
    Check(g_order.size() == 2 && g_prepared == 2 && g_num == 6, "GcPrepAhead: switched off live, no further level is prepared (those prepared wait for the pass)");
    GcPrepAheadTestTeardown();
    gchold_test::GhReset(0);
    GcPassAfterBusyTestTeardown();
}
