// OsPageCache self-test, included by selftest.cpp: the OS thunks and AllocateImpl's cache-miss calls on the
// mapped exe, then the cache core with fake OS functions: exact-size reuse (newest first), only for the cache-miss
// callers, the cap, the per-size limit, the size limit, the time-based trim, MemoryRelief's relief flush, the off switch, a
// failed allocation (flush and retry), and eight threads allocating and releasing at once.
#pragma once

#include <map>
#include <set>
#include <thread>

namespace osc {
std::mutex g_m;
std::set<void*> g_os;  // blocks the fake OS has handed out and not taken back
uint64_t g_osAllocs = 0, g_osFrees = 0, g_badFrees = 0;
bool g_failNext = false;
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
    std::lock_guard<std::mutex> l(g_m);
    if (!g_os.erase(p)) ++g_badFrees;
    else _aligned_free(p);
    ++g_osFrees;
    return TRUE;
}
uint64_t FakeNow() { return g_now; }

OsPageCacheStatsOut S()
{
    OsPageCacheStatsOut s{};
    OsPageCacheTestStats(&s);
    return s;
}

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
}
}  // namespace osc
