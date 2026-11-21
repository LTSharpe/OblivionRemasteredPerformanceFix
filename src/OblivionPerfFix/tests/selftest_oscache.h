// OsPageCache self-test, included by selftest.cpp: the OS thunks and AllocateImpl's cache-miss calls on the
// mapped exe, then the cache core with fake OS functions: exact-size reuse (newest first), only for the cache-miss
// callers, the cap, the per-size limit, the size limit, the time-based trim, MemoryRelief's relief flush, the off switch, a
// failed allocation (flush and retry), and eight threads allocating and releasing at once. Then OsPageCacheAsyncRelease
// (the releases from a background thread): inert at 0, order and completeness, kept blocks still kept, the cache's
// evictions through the queue, both bounds, the off switch with a full queue, MemoryRelief, a failed request (drain and
// retry), a lock held by an ended thread (the process's exit: no wait anywhere), the crash pause, four threads at once.
#pragma once

#include <map>
#include <set>
#include <thread>

namespace osc {
std::mutex g_m;
std::set<void*> g_os;  // blocks the fake OS has handed out and not taken back
uint64_t g_osAllocs = 0, g_osFrees = 0, g_badFrees = 0;
bool g_failNext = false;
bool g_recordOrder = false;
std::vector<void*> g_freeOrder;  // OsPageCacheAsyncRelease: the order the fake OS got the releases in
std::atomic<void*> g_slowPtr{nullptr};  // its release takes 50 ms (an entry held in flight)
std::atomic<bool> g_slowDone{false};  // that release has returned (the address itself may be handed out again at once)
uint64_t g_now = 1000;

void* FakeAlloc(SIZE_T)
{
    std::lock_guard<std::mutex> l(g_m);
    if (g_failNext) {
        g_failNext = false;
        return nullptr;
    }
    void* p = _aligned_malloc(64, 64);  // identity only: the cache never touches a block's bytes
    g_os.insert(p);
    ++g_osAllocs;
    return p;
}
BOOL FakeFree(void* p)
{
    if (!p) return FALSE;  // VirtualFree(nullptr) fails, as in the game
    if (p == g_slowPtr.load()) Sleep(50);
    std::lock_guard<std::mutex> l(g_m);
    if (!g_os.erase(p)) ++g_badFrees;
    else _aligned_free(p);
    ++g_osFrees;
    if (g_recordOrder) g_freeOrder.push_back(p);
    if (p == g_slowPtr.load()) g_slowDone = true;
    return TRUE;
}
uint64_t FakeNow() { return g_now; }

OsPageCacheStatsOut S()
{
    OsPageCacheStatsOut s{};
    OsPageCacheTestStats(&s);
    return s;
}

static void RunAsync();

static void Run(const GameModule& game)
{
    Check(InstallOsPageCache(true, L".", 0), "os page cache: thunks and AllocateImpl's calls found, thunks detoured");
    OsPageCacheInfoOut info{};
    if (!Installed(OsPageCacheInfo(&info), "OsPageCache (os page cache)")) return;
    Check(info.allocFn == game.base + 0xF41A20 && info.freeFn == game.base + 0xF41A40,
          "BinnedAllocFromOS at exe+0xF41A20, BinnedFreeToOS at exe+0xF41A40");
    Check(info.cacheRet[0] == game.base + 0xDF1B4F && info.cacheRet[1] == game.base + 0xDF1B65,
          "AllocateImpl's cache-miss calls return to exe+0xDF1B4F / exe+0xDF1B65");
    Check(info.allocFn[0] == 0xE9 && info.freeFn[0] == 0xE9, "both thunks start with a jmp to the detours");

    constexpr size_t K64 = 64 << 10, K128 = 128 << 10, K512 = 512 << 10;
    OsPageCacheTestInit(&FakeAlloc, &FakeFree, &FakeNow);
    // off: every call goes to the OS
    SetOsPageCache(0, 64, 5, false);
    void* a = OsPageCacheTestAlloc(K64, true);
    OsPageCacheTestFree(a, K64);
    Check(g_osAllocs == 1 && g_osFrees == 1 && S().blocks == 0, "off: allocation and release go to the OS");
    // on: exact-size reuse, newest first, only for the cache-miss callers
    SetOsPageCache(1, 64, 5, false);
    void* b1 = OsPageCacheTestAlloc(K64, true);
    void* b2 = OsPageCacheTestAlloc(K64, true);
    OsPageCacheTestFree(b1, K64);
    OsPageCacheTestFree(b2, K64);
    Check(g_osFrees == 1 && S().blocks == 2 && S().held == 2 * K64, "on: released blocks are kept");
    Check(OsPageCacheTestAlloc(K64, false) != b2 && S().blocks == 2, "other callers get a fresh block");
    Check(OsPageCacheTestAlloc(K128, true) && S().blocks == 2, "another size: a fresh block, the kept ones stay");
    void* r1 = OsPageCacheTestAlloc(K64, true);
    void* r2 = OsPageCacheTestAlloc(K64, true);
    Check(r1 == b2 && r2 == b1 && S().blocks == 0 && S().hits == 2, "same size: the newest kept block first, then the older");
    OsPageCacheTestFree(r1, K64);
    OsPageCacheTestFree(r2, K64);
    // size limit: blocks over cap / 8 (and over 16 MB) go to the OS
    const uint64_t frees0 = g_osFrees;
    void* big = OsPageCacheTestAlloc(9 << 20, true);
    OsPageCacheTestFree(big, 9 << 20);
    Check(g_osFrees == frees0 + 1, "a block over cap / 8 is released to the OS");
    OsPageCacheTestFree(nullptr, K64);
    Check(S().blocks == 2, "a null release is not kept");
    // the per-size limit (cap / 2) and the cap: a 4 MB cap, 512 KB blocks
    SetOsPageCache(1, 4, 5, false);
    Check(S().blocks == 2, "lowering the cap above what is held keeps the blocks");
    void* p[12];
    for (auto& x : p) x = OsPageCacheTestAlloc(K512, true);
    for (auto& x : p) OsPageCacheTestFree(x, K512);
    Check(S().held <= (4ull << 20) && S().held - 2 * K64 <= (2ull << 20), "512 KB blocks: at most half the cap of one size");
    Check(S().evictSize >= 8, "the oldest of that size went to the OS");
    void* q[8];
    for (auto& x : q) x = OsPageCacheTestAlloc(K128 * 2, true);
    for (auto& x : q) OsPageCacheTestFree(x, K128 * 2);
    Check(S().held <= (4ull << 20) && S().evictCap > 0, "the cap holds: the oldest blocks went to the OS");
    // trim: blocks unused for the trim time go back on the next tick
    g_now += 3000;
    SetOsPageCache(1, 4, 5, false);
    const uint32_t kept3 = S().blocks;
    Check(kept3 > 0, "trim: blocks younger than 5 s stay");
    g_now += 2500;
    SetOsPageCache(1, 4, 5, false);
    Check(S().blocks == 0 && S().held == 0, "trim: blocks unused for 5 s went to the OS");
    // relief: flush, then releases pass through until it ends
    void* c1 = OsPageCacheTestAlloc(K64, true);
    OsPageCacheTestFree(c1, K64);
    Check(S().blocks == 1, "kept again");
    SetOsPageCache(1, 64, 5, true);
    Check(S().blocks == 0, "MemoryRelief relief: everything kept went to the OS");
    void* c2 = OsPageCacheTestAlloc(K64, true);
    OsPageCacheTestFree(c2, K64);
    Check(S().blocks == 0, "MemoryRelief relief: releases go to the OS");
    SetOsPageCache(1, 64, 5, false);
    void* c3 = OsPageCacheTestAlloc(K64, true);
    OsPageCacheTestFree(c3, K64);
    Check(S().blocks == 1, "relief over: kept again");
    // a failed allocation: what is kept goes back, then once more
    g_failNext = true;
    void* c4 = OsPageCacheTestAlloc(K128, true);
    Check(c4 && S().blocks == 0 && S().allocFails == 1, "a failed allocation: the kept blocks went back and the retry worked");
    OsPageCacheTestFree(c4, K128);
    // off switch: flush
    SetOsPageCache(0, 64, 5, false);
    Check(S().blocks == 0 && S().held == 0, "off: everything kept went to the OS");
    Check(g_badFrees == 0, "no block released twice or unknown to the OS");

    // eight threads at once: no block handed out twice, every block back at the end
    SetOsPageCache(1, 8, 5, false);
    const size_t osBefore = g_os.size();  // the two blocks the checks above keep
    std::mutex um;
    std::set<void*> inUse;
    std::atomic<int> dup{0};
    auto worker = [&](int seed) {
        uint32_t s = static_cast<uint32_t>(seed) * 2654435761u + 1;
        std::vector<std::pair<void*, size_t>> mine;
        for (int i = 0; i < 20000; ++i) {
            s = s * 1664525u + 1013904223u;
            if (mine.size() < 16 && (s >> 28) < 9) {
                const size_t sz = static_cast<size_t>(4096) * (1 + (s >> 8) % 48);
                void* x = OsPageCacheTestAlloc(sz, (s & 1) != 0);
                {
                    std::lock_guard<std::mutex> l(um);
                    if (!inUse.insert(x).second) ++dup;
                }
                mine.push_back({x, sz});
            } else if (!mine.empty()) {
                const size_t k = (s >> 4) % mine.size();
                {
                    std::lock_guard<std::mutex> l(um);
                    inUse.erase(mine[k].first);
                }
                OsPageCacheTestFree(mine[k].first, mine[k].second);
                mine[k] = mine.back();
                mine.pop_back();
            }
        }
        for (auto& m : mine) {
            {
                std::lock_guard<std::mutex> l(um);
                inUse.erase(m.first);
            }
            OsPageCacheTestFree(m.first, m.second);
        }
    };
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) ts.emplace_back(worker, t + 1);
    for (auto& t : ts) t.join();
    const OsPageCacheStatsOut st = S();
    Check(dup == 0, "8 threads: no block handed out to two owners");
    Check(st.hits > 1000 && st.held <= (8ull << 20), "8 threads: blocks reused, the cap held");
    SetOsPageCache(0, 64, 5, false);
    Check(g_os.size() == osBefore && g_badFrees == 0, "8 threads: every block back to the OS after the off switch, none twice");
    printf("      (os page cache: 8 threads, %llu hits, %llu misses, %llu kept, %llu over cap, %llu over half)\n",
           static_cast<unsigned long long>(st.hits), static_cast<unsigned long long>(st.misses),
           static_cast<unsigned long long>(st.kept), static_cast<unsigned long long>(st.evictCap),
           static_cast<unsigned long long>(st.evictSize));

    RunAsync();
}

OsPageCacheAsyncStatsOut A()
{
    OsPageCacheAsyncStatsOut s{};
    OsPageCacheTestAsyncStats(&s);
    return s;
}

// OsPageCacheAsyncRelease with the fake OS functions and the real release thread
static void RunAsync()
{
    constexpr size_t K64 = 64 << 10, K512 = 512 << 10, M9 = 9 << 20, M16 = 16ull << 20;
    OsPageCacheTestInit(&FakeAlloc, &FakeFree, &FakeNow);
    const size_t osBase = g_os.size();
    // inert at 0: a block the cache does not keep goes to the OS at once
    SetOsPageCacheAsyncRelease(0);
    SetOsPageCache(1, 64, 5, false);
    const uint64_t f0 = g_osFrees;
    OsPageCacheTestFree(OsPageCacheTestAlloc(M9, true), M9);
    Check(!OsPageCacheTestAsyncOn() && g_osFrees == f0 + 1 && A().queued == 0, "async release 0: released at once, nothing queued");
    // on: queued in order, released once each by the thread
    SetOsPageCacheAsyncRelease(1);
    SetOsPageCache(1, 64, 5, false);
    Check(OsPageCacheTestAsyncOn(), "async release 1 with the cache on: on");
    OsPageCacheTestHold(true);
    std::vector<void*> pushed;
    for (int i = 0; i < 6; ++i) pushed.push_back(OsPageCacheTestAlloc(M9, true));
    const uint64_t f1 = g_osFrees;
    {
        std::lock_guard<std::mutex> l(g_m);
        g_freeOrder.clear();
        g_recordOrder = true;
    }
    for (void* x : pushed) OsPageCacheTestFree(x, M9);
    Check(g_osFrees == f1 && A().queued == 6 && A().queuedNow == 6, "6 blocks over the keep size: queued, none released yet");
    OsPageCacheTestHold(false);
    Check(OsPageCacheTestWaitIdle(3000), "the thread emptied the queue");
    {
        std::lock_guard<std::mutex> l(g_m);
        g_recordOrder = false;
        Check(g_freeOrder == pushed && g_osFrees == f1 + 6 && A().released == 6, "released by the thread, each once, in the order queued");
    }
    // a block the cache keeps is still kept (not queued)
    const uint32_t kept0 = S().blocks;
    OsPageCacheTestFree(OsPageCacheTestAlloc(K64, true), K64);
    Check(S().blocks == kept0 + 1 && A().queued == 6, "a block the cache keeps is kept, not queued");
    // the cache's evictions go through the queue
    SetOsPageCache(1, 4, 5, false);
    OsPageCacheTestHold(true);
    void* p[12];
    for (auto& x : p) x = OsPageCacheTestAlloc(K512, true);
    const uint64_t f2 = g_osFrees;
    for (auto& x : p) OsPageCacheTestFree(x, K512);
    Check(S().evictSize >= 8 && g_osFrees == f2 && A().queuedNow >= 8, "evictions (one size over half the cap): queued, not released on the caller");
    OsPageCacheTestHold(false);
    Check(OsPageCacheTestWaitIdle(3000) && g_osFrees >= f2 + 8, "evictions released by the thread");
    SetOsPageCache(1, 64, 5, false);
    // the count bound: 4,096 queued, the next one released at once; then the off switch with the full queue drains it
    OsPageCacheTestHold(true);
    std::vector<void*> many;
    for (int i = 0; i < 4097; ++i) many.push_back(OsPageCacheTestAlloc(4096, false));
    const uint64_t f3 = g_osFrees;
    for (void* x : many) OsPageCacheTestRelease(x, 4096);
    Check(A().queuedNow == 4096 && A().fullBlocks == 1 && g_osFrees == f3 + 1, "count bound: 4,096 queued, the 4,097th released at once");
    SetOsPageCacheAsyncRelease(0);
    SetOsPageCache(1, 64, 5, false);
    Check(!OsPageCacheTestAsyncOn() && A().queuedNow == 0 && g_osFrees == f3 + 4097 && A().drained >= 4096,
          "key off with a full queue: all of it released at once by the switch");
    OsPageCacheTestHold(false);
    SetOsPageCacheAsyncRelease(1);
    SetOsPageCache(1, 64, 5, false);
    // the byte bound: 4 x 16 MB queued, the fifth released at once; MemoryRelief drains and turns it off
    OsPageCacheTestHold(true);
    void* m[5];
    for (auto& x : m) x = OsPageCacheTestAlloc(4096, false);
    const uint64_t f4 = g_osFrees;
    for (auto& x : m) OsPageCacheTestRelease(x, M16);
    Check(A().queuedNow == 4 && A().fullBytes == 1 && g_osFrees == f4 + 1 && A().maxBytes == 4 * M16,
          "byte bound: 64 MB queued, the next block released at once");
    const uint64_t dr4 = A().drained;
    SetOsPageCache(1, 64, 5, true);
    Check(!OsPageCacheTestAsyncOn() && A().queuedNow == 0 && A().drained == dr4 + 4, "MemoryRelief: queue drained, async off");
    void* r = OsPageCacheTestAlloc(4096, false);
    const uint64_t f4b = g_osFrees;
    OsPageCacheTestRelease(r, 4096);
    Check(A().queuedNow == 0 && g_osFrees == f4b + 1, "MemoryRelief: releases at once");
    SetOsPageCache(1, 64, 5, false);
    Check(OsPageCacheTestAsyncOn(), "relief over: on again");
    // the cache's off switch turns it off too
    SetOsPageCache(0, 64, 5, false);
    Check(!OsPageCacheTestAsyncOn(), "OsPageCache off: async off");
    SetOsPageCache(1, 64, 5, false);
    // a failed request: the queue is drained before the retry
    void* d[3];
    for (auto& x : d) x = OsPageCacheTestAlloc(4096, false);
    const uint64_t f5 = g_osFrees, dr5 = A().drained;
    for (auto& x : d) OsPageCacheTestRelease(x, 4096);
    g_failNext = true;
    void* got = OsPageCacheTestAlloc(K64, false);
    Check(got && A().queuedNow == 0 && A().drained == dr5 + 3 && g_osFrees >= f5 + 3, "a failed request: queue drained, the retry worked");
    OsPageCacheTestFree(got, K64);
    // the process's exit: a thread ended inside the queue's lock must not make anyone wait
    void* e[3];
    for (auto& x : e) x = OsPageCacheTestAlloc(4096, false);
    OsPageCacheTestRelease(e[0], 4096);
    OsPageCacheTestRelease(e[1], 4096);
    OsPageCacheTestLockQueue(true);
    const uint64_t f6 = g_osFrees, busy6 = A().busy, db6 = A().drainBusy;
    OsPageCacheTestRelease(e[2], 4096);
    Check(A().busy == busy6 + 1 && g_osFrees == f6 + 1, "queue lock held by an ended thread: the release is made at once, no wait");
    g_failNext = true;
    void* got2 = OsPageCacheTestAlloc(K64, false);
    Check(got2 && A().drainBusy == db6 + 1, "queue lock held: a failed request skips the drain, no wait, the retry worked");
    OsPageCacheTestLockQueue(false);
    OsPageCacheTestFree(got2, K64);
    OsPageCacheTestHold(false);
    Check(OsPageCacheTestWaitIdle(3000) && A().queuedNow == 0, "lock free again: the two queued blocks released by the thread");
    // memory low (MemoryRelief's latest sample): releases at once, the tick drains what is queued
    OsPageCacheTestHold(true);
    void* lw[3];
    for (auto& x : lw) x = OsPageCacheTestAlloc(4096, false);
    OsPageCacheTestRelease(lw[0], 4096);
    OsPageCacheTestRelease(lw[1], 4096);
    OsPageCacheMemoryLow(true);
    const uint64_t f8 = g_osFrees, low8 = A().low;
    OsPageCacheTestRelease(lw[2], 4096);
    Check(A().low == low8 + 1 && g_osFrees == f8 + 1 && A().queuedNow == 2, "memory low: a release is made at once");
    SetOsPageCache(1, 64, 5, false);
    Check(A().queuedNow == 0 && g_osFrees == f8 + 3, "memory low: the tick drained the queue");
    OsPageCacheMemoryLow(false);
    OsPageCacheTestHold(false);
    // a failed request waits for the entry the thread is releasing (its address space back before the retry)
    void* slow = OsPageCacheTestAlloc(4096, false);
    g_slowDone = false;
    g_slowPtr = slow;
    OsPageCacheTestRelease(slow, 4096);
    for (int i = 0; i < 1000 && A().queuedNow; ++i) Sleep(1);  // the thread has taken it (50 ms in the fake call)
    g_failNext = true;
    void* got3 = OsPageCacheTestAlloc(K64, false);
    // (not "slow is no longer handed out": the fake's next allocation may get the same address back)
    Check(got3 && g_slowDone.load(), "a failed request: waited for the entry in flight, then retried");
    g_slowPtr = nullptr;
    OsPageCacheTestFree(got3, K64);
    // the crash pause with entries queued: they stay queued and untouched, nothing more queued
    OsPageCacheTestHold(true);
    void* pq[2];
    for (auto& x : pq) x = OsPageCacheTestAlloc(4096, false);
    for (auto& x : pq) OsPageCacheTestRelease(x, 4096);
    const uint64_t f9 = g_osFrees;
    OsPageCacheAsyncPause();
    OsPageCacheTestHold(false);
    Sleep(20);
    SetOsPageCache(1, 64, 5, false);
    Check(A().queuedNow == 2 && g_osFrees == f9, "crash pause: queued entries stay queued, the tick does not drain them");
    // the crash pause: nothing more queued or released by the thread, releases at once; the tick does not turn it back on
    void* c = OsPageCacheTestAlloc(4096, false);
    const uint64_t f7 = g_osFrees;
    OsPageCacheTestRelease(c, 4096);
    SetOsPageCache(1, 64, 5, false);
    Check(!OsPageCacheTestAsyncOn() && g_osFrees == f7 + 1, "crash pause: releases at once, stays off");
    Check(!OsPageCacheTestAsyncOn() && g_osFrees == f7 + 1 && A().queuedNow == 2 && A().paused >= 1, "crash pause: counted as paused");
    OsPageCacheTestResume();
    SetOsPageCache(1, 64, 5, false);
    Check(OsPageCacheTestAsyncOn(), "after the pause (self-test only): on again");
    Check(OsPageCacheTestWaitIdle(3000), "after the pause: the two kept entries released by the thread");
    // four threads at once, 10,000 releases each: every block released exactly once
    const OsPageCacheAsyncStatsOut a0 = A();
    const uint64_t bad0 = g_badFrees;
    auto worker = [&](int) {
        for (int i = 0; i < 10000; ++i) OsPageCacheTestRelease(OsPageCacheTestAlloc(4096, false), 4096);
    };
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back(worker, t);
    for (auto& t : ts) t.join();
    Check(OsPageCacheTestWaitIdle(10000), "4 threads: the queue emptied");
    const OsPageCacheAsyncStatsOut a1 = A();
    const uint64_t queued = a1.queued - a0.queued, atOnce = (a1.fullBlocks - a0.fullBlocks) + (a1.fullBytes - a0.fullBytes) + (a1.busy - a0.busy) +
                           (a1.offWait - a0.offWait);
    Check(queued + atOnce == 40000 && (a1.released - a0.released) + (a1.drained - a0.drained) == queued,
          "4 threads: 40,000 releases, each queued and released by the thread, or released at once");
    Check(queued > 0 && a1.released > a0.released && a1.threadUs >= a0.threadUs, "4 threads: blocks were queued and released by the thread");
    // the key switched off and on live while four threads release: nothing lost, nothing twice
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts2;
    for (int t = 0; t < 4; ++t)
        ts2.emplace_back([&] {
            for (int i = 0; i < 5000; ++i) OsPageCacheTestRelease(OsPageCacheTestAlloc(4096, false), 4096);
        });
    for (int k = 0; k < 200 && !stop; ++k) {
        SetOsPageCacheAsyncRelease(k & 1);
        SetOsPageCache(1, 64, 5, false);
    }
    for (auto& t : ts2) t.join();
    SetOsPageCacheAsyncRelease(1);
    SetOsPageCache(1, 64, 5, false);
    Check(OsPageCacheTestWaitIdle(10000) && g_badFrees == bad0, "live switch under load: the queue emptied, no block released twice");
    SetOsPageCacheAsyncRelease(0);
    SetOsPageCache(0, 64, 5, false);
    Check(g_os.size() == osBase && g_badFrees == bad0 && g_badFrees == 0, "async release: every block back to the OS, none twice");
    printf("      (async release: 4 threads, %llu queued, %llu at once (lock busy %llu), largest queue %llu)\n",
           static_cast<unsigned long long>(queued), static_cast<unsigned long long>(atOnce),
           static_cast<unsigned long long>(a1.busy - a0.busy), static_cast<unsigned long long>(a1.maxBlocks));
}
}  // namespace osc
