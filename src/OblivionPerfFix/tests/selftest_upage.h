// UploadPagePreGrow / UploadPageScreenRelease self-test (s083build, S-083), included by selftest.cpp: the fast allocator's page
// request, its call, the lock and the page return found on the mapped exe; the call redirected; the probe, the pre-grow round and
// the decisions run on a fake page pool laid out as the game's (header, page list, critical section at +0x40).
#pragma once

namespace upg {

struct FakeRes { void* vt; int32_t refs; };
struct FakePage { uint32_t size; uint32_t pad; FakeRes* buf; uint32_t next; uint32_t pad2; void* data; uint64_t fence; };
struct alignas(16) FakePool { uint8_t bytes[0x100]; };
inline FakePool g_pool;
inline void* g_list[64];
inline FakeRes g_res[64];
inline FakePage g_pages[64];
inline int g_made = 0, g_returnedLocked = 0, g_returnedUnlocked = 0, g_privateOk = 0, g_privateBad = 0;
inline bool g_touchList = false;
inline uint8_t g_device[0x100];

template <class T> T& At(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }

inline void* FakeRequest(void* pool)
{
    void** data = At<void**>(pool, 0x28);
    int32_t& num = At<int32_t>(pool, 0x30);
    if (num > 0) {  // the game's scan: the first page (all free here), removed from the list
        void* p = data[0];
        for (int i = 1; i < num; ++i) data[i - 1] = data[i];
        --num;
        return p;
    }
    if (pool != &g_pool) {  // the private copy: the same header, an empty list
        if (memcmp(pool, &g_pool, 0x28) == 0 && !data && At<int32_t>(pool, 0x34) == 0) ++g_privateOk;
        else ++g_privateBad;
        if (g_touchList) num = 1;
    }
    FakePage* pg = &g_pages[g_made % 64];
    FakeRes* r = &g_res[g_made % 64];
    r->refs = 1;
    *pg = {4u << 20, 0, r, 0, 0, nullptr, 0};
    ++g_made;
    return pg;
}

inline void FakeReturn(void* pool, void* page)
{
    auto* cs = reinterpret_cast<CRITICAL_SECTION*>(static_cast<uint8_t*>(pool) + 0x40);
    if (reinterpret_cast<uintptr_t>(cs->OwningThread) == static_cast<uintptr_t>(GetCurrentThreadId())) ++g_returnedLocked;
    else ++g_returnedUnlocked;
    int32_t& num = At<int32_t>(pool, 0x30);
    At<void**>(pool, 0x28)[num++] = page;
}

inline void ResetPool(int pages)
{
    static bool csReady = false;
    memset(&g_pool, 0, 0x40);  // the header and the list; the critical section after it is made once
    if (!csReady) InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(g_pool.bytes + 0x40));
    csReady = true;
    At<void*>(&g_pool, 0) = g_device;
    At<uint32_t>(&g_pool, 0x10) = 4u << 20;
    At<uint32_t>(&g_pool, 0x14) = 2;  // upload heap
    At<void**>(&g_pool, 0x28) = g_list;
    At<int32_t>(&g_pool, 0x34) = 64;
    for (int i = 0; i < pages; ++i) {
        g_res[i].refs = 1;
        g_pages[i] = {4u << 20, 0, &g_res[i], 0, 0, nullptr, 0};
        g_list[i] = &g_pages[i];
    }
    At<int32_t>(&g_pool, 0x30) = pages;
    g_made = pages;
}

inline void UploadPageSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    // the decisions
    uint32_t retry = 0;
    int why = 0;
    Check(UploadPagePreGrowDecide(true, 40, 40, 40, 24, 0, false, 5000, 5000, 300, &retry, &why) && retry == 50,
          "upage: pool under the floor, quiet, room: one page now, next look in 50 ms");
    Check(!UploadPagePreGrowDecide(true, 40, 40, 40, 40, 0, false, 5000, 5000, 300, &retry, &why), "upage: pool at the floor: nothing made");
    Check(!UploadPagePreGrowDecide(true, 24, 24, 40, 24, 0, false, 5000, 5000, 300, &retry, &why),
          "upage: floor 24 (LoadPeakKeepsBack's episode, tight): never above the floor in force");
    Check(!UploadPagePreGrowDecide(true, 0, 5, 40, 3, 0, false, 5000, 5000, 300, &retry, &why), "upage: MemoryRelief / keep off (game's 5): nothing made");
    Check(!UploadPagePreGrowDecide(false, 40, 40, 40, 10, 0, false, 5000, 5000, 300, &retry, &why), "upage: key off: nothing made");
    Check(!UploadPagePreGrowDecide(true, 40, 40, 40, 10, 1, false, 5000, 5000, 300, &retry, &why) && why == 1 && retry == 250,
          "upage: behind a loading screen: nothing made (the load makes pages)");
    Check(UploadPagePreGrowDecide(true, 40, 40, 40, 10, -1, false, 5000, 5000, 300, &retry, &why), "upage: loading state unknown: made");
    Check(!UploadPagePreGrowDecide(true, 40, 40, 40, 10, 0, true, 5000, 5000, 300, &retry, &why) && why == 2, "upage: video memory tight: nothing made");
    Check(!UploadPagePreGrowDecide(true, 40, 40, 40, 10, 0, false, 120, 5000, 300, &retry, &why) && why == 3 && retry == 181,
          "upage: the game made a page 120 ms ago: wait for the quiet time (its burst first)");
    Check(!UploadPagePreGrowDecide(true, 40, 40, 40, 10, 0, false, 5000, 20, 300, &retry, &why) && retry == 31,
          "upage: our last page 20 ms ago: at most one per 50 ms");
    Check(UploadPagePreGrowFloor(true, false, 24, 40) == 40 && UploadPagePreGrowFloor(true, true, 24, 40) == 24 &&
              UploadPagePreGrowFloor(false, false, 24, 40) == 24,
          "upage floor: 40 while on and not tight, else UploadPagesToKeep");
    Check(UploadPagePreGrowFloor(true, false, 24, 99) == 64 && UploadPagePreGrowFloor(true, false, 24, 1) == 24 &&
              UploadPagePreGrowFloor(true, false, 48, 40) == 48,
          "upage floor: the target held within 24..64, never under UploadPagesToKeep");
    Check(UploadPageScreenReleaseDecide(true, 1, 500, 0, 7000, 24000) && UploadPageScreenReleaseDecide(true, 1, 500, 16000, 7000, 24000) &&
              !UploadPageScreenReleaseDecide(true, 1, 500, 18000, 7000, 24000),
          "screenrel: behind the screen after the burst: releases while the frame's extra time stays within the cap");
    Check(!UploadPageScreenReleaseDecide(true, 1, 40, 0, 7000, 24000), "screenrel: a page made 40 ms ago (the burst): none");
    Check(!UploadPageScreenReleaseDecide(true, 0, 500, 0, 7000, 24000) && !UploadPageScreenReleaseDecide(true, -1, 500, 0, 7000, 24000) &&
              !UploadPageScreenReleaseDecide(false, 1, 500, 0, 7000, 24000),
          "screenrel: in play, state unknown or key off: the game's one a frame");
    // the sites on the exe
    uint32_t callRva = 0, requestRva = 0, returnRva = 0, csOff = 0;
    if (!Installed(FindUploadPageAhead(game, &callRva, &requestRva, &returnRva, &csOff),
                   "UploadPagePreGrow (Allocate's page request, its lock and the page return)"))
        return;
    Check(csOff == 0x40, "upage: the allocator's lock 0x40 after its page pool (Allocate: lea rbx,[rcx+50h], pool at +10h)");
    Check(UploadPageAheadTestFenceOff() == 0x548, "upage: the adapter's frame fence at +0x548 (the free count's cached value)");
    Check(RipTarget(game.base + callRva, 1, 5) == game.base + requestRva && returnRva != requestRva,
          "upage: the redirected call goes to RequestFastAllocatorPage");
    Check(!InstallUploadPageAhead(false, false), "upage: refuses to install after game code started");
    Check(InstallUploadPageAhead(true, false) && UploadPageAheadTestPatched(), "upage: call RequestFastAllocatorPage -> call stub -> probe");
    // the probe on a fake pool
    UploadPageAheadTestSetup(reinterpret_cast<void*>(&FakeRequest), reinterpret_cast<void*>(&FakeReturn), 0x40);
    SetUploadPagePreGrow(true, 40, 300, true, false);
    ResetPool(2);
    At<uint32_t>(&g_pool, 0x10) = 1u << 20;  // not the device's 4 MB page pool: left alone
    UploadPageRequestHook(&g_pool);
    uint64_t served = 0, created = 0, made = 0;
    int bursts = 0;
    bool broken = false;
    UploadPageAheadTestCounts(&served, &created, &made, &bursts, &broken);
    Check(served == 0 && created == 0 && At<int32_t>(&g_pool, 0x30) == 1, "upage probe: another pool layout: the game's call only, not counted");
    ResetPool(2);
    UploadPageRequestHook(&g_pool);
    UploadPageRequestHook(&g_pool);
    UploadPageAheadTestCounts(&served, &created, &made, &bursts, &broken);
    Check(served == 2 && created == 0, "upage probe: pages taken from the list count as served");
    for (int i = 0; i < 3; ++i) UploadPageRequestHook(&g_pool);
    UploadPageAheadTestCounts(&served, &created, &made, &bursts, &broken);
    Check(created == 3 && bursts == 1, "upage probe: an empty list: created on demand; 3 within 200 ms = one burst");
    // the pre-grow round
    ResetPool(30);
    UploadPageAheadTestFloor(40, 40);
    SetLoadingStateWanted(false);
    LoadingStateFrame(false);  // -1: unknown
    UploadPageAheadTestAge(1000);
    const int before = g_made;
    UploadPageAheadTestWork();
    UploadPageAheadTestCounts(&served, &created, &made, &bursts, &broken);
    Check(made == 1 && g_made == before + 1 && At<int32_t>(&g_pool, 0x30) == 31, "upage ahead: one page made and put at the end of the list");
    Check(g_privateOk == 1 && g_privateBad == 0, "upage ahead: the game's request ran on a header copy with an empty list");
    Check(g_returnedLocked == 1 && g_returnedUnlocked == 0, "upage ahead: the game's return ran with the allocator's lock held");
    UploadPageAheadTestWork();
    UploadPageAheadTestCounts(&served, &created, &made, &bursts, &broken);
    Check(made == 1, "upage ahead: the next page waits 50 ms");
    UploadPageAheadTestAge(1000);
    UploadPageAheadTestFloor(24, 24);
    UploadPageAheadTestWork();
    UploadPageAheadTestCounts(&served, &created, &made, &bursts, &broken);
    Check(made == 1, "upage ahead: the floor in force at 24 (episode / tight): nothing above it");
    UploadPageAheadTestFloor(40, 40);
    UploadPageRequestHook(&g_pool);  // served (31 in the list)
    ResetPool(0);
    UploadPageRequestHook(&g_pool);  // created now
    UploadPageAheadTestWork();
    UploadPageAheadTestCounts(&served, &created, &made, &bursts, &broken);
    Check(made == 1, "upage ahead: right after a page the game made: waits for the quiet time");
    UploadPageAheadTestAge(1000);
    g_touchList = true;
    UploadPageAheadTestWork();
    g_touchList = false;
    UploadPageAheadTestCounts(&served, &created, &made, &bursts, &broken);
    Check(broken && made == 1, "upage ahead: the private list touched: off until the game restarts, nothing returned");
    SetUploadPagePreGrow(false, 40, 300, false, false);
    UploadPageAheadTestFloor(0, 0);
    UploadPageAheadTestSetup(nullptr, nullptr, 0);
}

}  // namespace upg
