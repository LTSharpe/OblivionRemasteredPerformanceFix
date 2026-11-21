// OsPageCache: the allocator's OS page churn.
//
// UE 5.3's FMallocBinned2 gets every 64 KB small-pool page and every large block (up to 16 MB) through one shared cache
// of freed OS blocks (FCachedOSPageAllocator: at most 64 blocks, 128 for threads with TLS caches; exact-size reuse).
// That cache is always full by count, so the game makes ~1,350 VirtualAlloc + VirtualFree pairs a second even at 480p
//, and every block from VirtualAlloc is fresh, demand-zero memory.
//
// This file detours the two OS entry points (FPlatformMemory::BinnedAllocFromOS = VirtualAlloc(MEM_COMMIT|MEM_RESERVE,
// PAGE_READWRITE), BinnedFreeToOS = VirtualFree(MEM_RELEASE); both two-instruction thunks through the exe's imports).
// With the cache off the detours call the same import slots with the same arguments (the game's behaviour).
// OsPageCacheAsyncRelease (live, default off): the releases the cache does not keep are made by one background thread
// instead of the game's thread (the queue below MaxBlock).
//
#include "common.h"

#include <intrin.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#pragma intrinsic(_ReturnAddress)

namespace opf {
namespace {

// BinnedAllocFromOS: mov rdx,rcx; mov r9d,4; xor ecx,ecx; mov r8d,3000h; rex.W jmp [rip+VirtualAlloc slot]
const char kAllocThunk[] = "48 8B D1 41 B9 04 00 00 00 33 C9 41 B8 00 30 00 00 48 FF 25 ?? ?? ?? ??";
// BinnedFreeToOS: xor edx,edx; mov r8d,8000h; rex.W jmp [rip+VirtualFree slot]
const char kFreeThunk[] = "33 D2 41 B8 00 80 00 00 48 FF 25 ?? ?? ?? ??";

using VAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, DWORD);
using VFreeFn = BOOL(WINAPI*)(void*, SIZE_T, DWORD);

void** g_allocSlot = nullptr;  // the exe's import slots the thunks jump through (read at every call, as the thunks do)
void** g_freeSlot = nullptr;
uint8_t* g_allocFn = nullptr;
uint8_t* g_freeFn = nullptr;
bool g_installed = false;

uint8_t* FindOne(const char* pattern, const char* what)
{
    const auto hits = FindAll(Game().text, ParsePattern(pattern));
    if (hits.size() != 1) {
        Log("[oscache] %s not found (%zu matches)", what, hits.size());
        return nullptr;
    }
    return hits[0];
}


// ---- the cache (OsPageCache) ----
// An exact-size cache of the blocks Binned2 releases, handed out again for Binned2's own cache misses.
// - Who gets a recycled block: only AllocateImpl's two cache-miss calls (the sizes FCachedOSPageAllocator caches, <= its
//   byte limit / 4); there the game's own cache already hands out recycled blocks of the same exact size with old
//   contents, so no caller can depend on zeroed pages. Binned2's tables, TLS blocks, the OOM reserve and the OS-sized
//   (> 16 MB) path keep getting fresh blocks from VirtualAlloc.
// - What is kept: releases of at most min(16 MB, cap / 8). The only callers of BinnedFreeToOS are Binned2's (FreeImpl's
//   eviction loops, FreeAll, AllocateImpl's out-of-memory release, the OOM reserve's 32 MB), each passing the size the
//   block was allocated with, the same size the game's cache itself relies on for exact-size reuse.
// - Bounds: cap bytes (oldest block released first), at most cap / 2 of one size, blocks unused for TrimSeconds are
//   released by the monitor thread, and everything is released while MemoryRelief reports RAM or commit short, after a failed
//   VirtualAlloc (then retried), or when the key is switched off.
using OsAllocFn = void* (*)(SIZE_T);
using OsFreeFn = BOOL (*)(void*);
void* RealAlloc(SIZE_T size) { return reinterpret_cast<VAllocFn>(*g_allocSlot)(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
BOOL RealFree(void* p) { return reinterpret_cast<VFreeFn>(*g_freeSlot)(p, 0, MEM_RELEASE); }
uint64_t RealNowMs() { return GetTickCount64(); }
OsAllocFn g_osAlloc = &RealAlloc;
OsFreeFn g_osFree = &RealFree;
uint64_t (*g_nowMs)() = &RealNowMs;

constexpr uint32_t kNodes = 8192, kBuckets = 4096, kNil = 0xFFFFFFFFu, kMaxEvict = 32;
struct Node {
    void* ptr;
    uint64_t bytes, freedMs;
    uint32_t older, newer;      // age list (head = oldest)
    uint32_t sizeOlder, sizeNewer;  // the bucket's list (head = newest)
    uint32_t bucket;
};
struct Bucket {
    uint64_t key;  // block size (0 = unused slot)
    uint64_t bytes;
    uint32_t newest, oldest, count;
};
struct Cache {
    SRWLOCK lock = SRWLOCK_INIT;
    Node nodes[kNodes];
    Bucket buckets[kBuckets];
    uint32_t freeNodes = kNil;
    uint32_t oldest = kNil, newest = kNil;
    uint64_t held = 0, peak = 0;
    uint32_t count = 0;
};
Cache* g_cache = nullptr;  // allocated at install (about 0.5 MB)
std::atomic<bool> g_on{false};             // the key (and no relief)
std::atomic<bool> g_relief{false};
std::atomic<uint64_t> g_cap{64ull << 20}, g_trimMs{5000};
std::atomic<uint64_t> g_hits{0}, g_hitBytes{0}, g_misses{0}, g_missBytes{0}, g_puts{0}, g_passed{0}, g_evictCap{0},
    g_evictSize{0}, g_evictRoom{0}, g_trimmed{0}, g_flushed{0}, g_allocFails{0};
const void* g_cacheRet[2] = {};  // AllocateImpl's two cache-miss calls (return addresses)
int g_mode = 0;

uint64_t MaxBlock(uint64_t cap) { return (std::min)(16ull << 20, cap / 8); }

// ---- OsPageCacheAsyncRelease: the releases the game's threads make, handed to one background thread ----
// BinnedFreeToOS is always VirtualFree(p, 0, MEM_RELEASE) of a whole block the manager has finished with (its callers,
// above, pass the block's own size); the manager has already taken it out of its own accounting before the call, and no
// code of the game touches the range again. With the key on (and the cache on, no MemoryRelief relief), the blocks a
// game thread would release here (the cache's evictions in Put, the blocks it does not keep in FreeCore) go into a
// first-in-first-out queue, and one thread releases them in that order with the same call. Until then Windows keeps the
// range reserved, so it cannot hand the same addresses to anyone else: only the moment the memory goes back changes.
// - Bounds: at most kAsyncBlocks blocks and kAsyncBytes bytes queued (64 MB: a release thread that gets no CPU for a while
//   keeps at most that much extra committed); a release that does not fit is made at once on
//   the calling thread (as the game does), as is one that finds the queue's lock busy for a few microseconds (the lock
//   is only ever held for one entry's copy; a thread ended inside it at the process's exit must not hang the others).
// - Drained (released at once, on the calling thread): when a request to Windows fails (then retried), when MemoryRelief
//   reports RAM or commit short, and when either key is switched off. The thread is started at the first switch-on and
//   never ends: whatever is queued is released by it or by a drain. At the process's end Windows takes the whole address
//   space back, queued ranges included, so nothing waits there. While memory is low (OsPageCacheMemoryLow: MemoryRelief's
//   latest one-second sample has less than twice its RAM or commit threshold free, without its 3-second rule) every
//   release is made at once and the tick drains the queue. After OsPageCacheAsyncPause (a crash being written) the
//   thread releases nothing more and new releases are made at once again.
// - Priority: normal. VirtualFree holds the process's address-space lock, which page faults and VirtualAlloc of the
//   game's threads also take; a lower priority could leave it preempted while holding it. The CPU time is the same the
//   game's thread spent on the call before.
constexpr uint32_t kAsyncBlocks = 4096;
constexpr uint64_t kAsyncBytes = 64ull << 20;  // the cache's own default cap
struct AsyncEntry {
    void* ptr;
    uint64_t bytes;
    uint64_t qpc;  // when it was queued
};
struct AsyncQueue {
    SRWLOCK lock = SRWLOCK_INIT;
    AsyncEntry e[kAsyncBlocks];
    uint32_t head = 0, count = 0;  // head = the oldest entry
    uint64_t bytes = 0;
};
AsyncQueue g_q;
std::atomic<uint32_t> g_qLen{0};  // g_q.count, readable without the lock
std::atomic<bool> g_asyncKey{false};  // the ini key
std::atomic<bool> g_asyncOn{false};   // the key, the cache on, no relief, the thread running, not paused
std::atomic<bool> g_asyncPaused{false};
std::atomic<bool> g_asyncHold{false};  // self-test only: the thread takes nothing
std::atomic<bool> g_asyncLow{false};   // memory low (OsPageCacheMemoryLow): releases at once
std::atomic<uint32_t> g_inFlight{0};   // entries taken off the queue and not yet released (0 or 1: the thread takes one at a time)
HANDLE g_asyncWake = nullptr;
bool g_asyncThread = false;
bool g_asyncEverOn = false;
LARGE_INTEGER g_asyncFreq{};
std::atomic<uint64_t> g_aQueued{0}, g_aQueuedBytes{0}, g_aReleased{0}, g_aDrained{0}, g_aFullBlocks{0}, g_aFullBytes{0},
    g_aBusy{0}, g_aPaused{0}, g_aMaxBlocks{0}, g_aMaxBytes{0}, g_aMaxWaitUs{0}, g_aDrains{0}, g_aDrainBusy{0}, g_aOffWait{0},
    g_aLow{0}, g_aThreadUs{0}, g_aThreadMaxUs{0};

void StoreMax(std::atomic<uint64_t>& m, uint64_t v)
{
    uint64_t c = m.load(std::memory_order_relaxed);
    while (v > c && !m.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}

// The queue's lock for a game thread: a few microseconds of tries, never a wait.
bool TryLockQueue()
{
    for (int i = 0; i < 256; ++i) {
        if (TryAcquireSRWLockExclusive(&g_q.lock)) return true;
        YieldProcessor();
    }
    return false;
}

uint64_t QpcNow()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

// A block the game's thread is done with: queued for the release thread, or released now. Same result as g_osFree for
// every argument (a null pointer is passed on: VirtualFree fails for it as it does for the game).
BOOL OsRelease(void* ptr, uint64_t bytes)
{
    if (!ptr) return g_osFree(ptr);
    if (!g_asyncOn.load(std::memory_order_relaxed)) {
        if (g_asyncPaused.load(std::memory_order_relaxed)) g_aPaused.fetch_add(1, std::memory_order_relaxed);
        return g_osFree(ptr);
    }
    if (g_asyncLow.load(std::memory_order_relaxed)) {
        g_aLow.fetch_add(1, std::memory_order_relaxed);
        return g_osFree(ptr);
    }
    if (!TryLockQueue()) {
        g_aBusy.fetch_add(1, std::memory_order_relaxed);
        return g_osFree(ptr);
    }
    bool wake = false, queued = false;
    if (!g_asyncOn.load(std::memory_order_relaxed)) {
        // switched off (or paused) while waiting for the lock: as the game
        if (!g_asyncPaused.load(std::memory_order_relaxed)) g_aOffWait.fetch_add(1, std::memory_order_relaxed);
    } else if (g_q.count >= kAsyncBlocks) {
        g_aFullBlocks.fetch_add(1, std::memory_order_relaxed);
    } else if (g_q.bytes + bytes > kAsyncBytes) {
        g_aFullBytes.fetch_add(1, std::memory_order_relaxed);
    } else {
        AsyncEntry& e = g_q.e[(g_q.head + g_q.count) % kAsyncBlocks];
        e.ptr = ptr;
        e.bytes = bytes;
        e.qpc = QpcNow();
        wake = g_q.count == 0;
        ++g_q.count;
        g_q.bytes += bytes;
        g_qLen.store(g_q.count, std::memory_order_relaxed);
        StoreMax(g_aMaxBlocks, g_q.count);
        StoreMax(g_aMaxBytes, g_q.bytes);
        queued = true;
    }
    ReleaseSRWLockExclusive(&g_q.lock);
    if (!queued) {
        if (g_asyncPaused.load(std::memory_order_relaxed)) g_aPaused.fetch_add(1, std::memory_order_relaxed);
        return g_osFree(ptr);
    }
    g_aQueued.fetch_add(1, std::memory_order_relaxed);
    g_aQueuedBytes.fetch_add(bytes, std::memory_order_relaxed);
    if (wake && g_asyncWake) SetEvent(g_asyncWake);
    return TRUE;  // VirtualFree(MEM_RELEASE) of a block the manager allocated succeeds; the manager ignores the result
}

// Takes the oldest entry (lock held by the caller).
AsyncEntry PopLocked()
{
    AsyncEntry e = g_q.e[g_q.head];
    g_q.head = (g_q.head + 1) % kAsyncBlocks;
    --g_q.count;
    g_q.bytes -= e.bytes;
    g_qLen.store(g_q.count);
    return e;
}

void NoteWait(const AsyncEntry& e)
{
    const uint64_t now = QpcNow();
    if (g_asyncFreq.QuadPart && now > e.qpc)
        StoreMax(g_aMaxWaitUs, QpcToMicroseconds(now - e.qpc, static_cast<uint64_t>(g_asyncFreq.QuadPart)));
}

// Everything queued, released now on the calling thread, oldest first. failedRequest (a game thread whose request to
// Windows failed): the lock only if free within a few microseconds, and a wait of at most 100 ms for the entry the thread
// is releasing, so that its address space is back too. Otherwise (the 1-second tick) the lock is waited for.
uint64_t DrainQueue(bool failedRequest)
{
    uint64_t n = 0;
    for (;;) {
        AsyncEntry batch[64];
        int k = 0;
        if (!failedRequest) AcquireSRWLockExclusive(&g_q.lock);
        else if (!TryLockQueue()) {
            g_aDrainBusy.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        while (k < 64 && g_q.count) batch[k++] = PopLocked();
        ReleaseSRWLockExclusive(&g_q.lock);
        for (int i = 0; i < k; ++i) {
            NoteWait(batch[i]);
            g_osFree(batch[i].ptr);
        }
        n += k;
        if (k < 64) break;
    }
    if (failedRequest)
        for (int i = 0; i < 100 && g_inFlight.load(std::memory_order_acquire); ++i) Sleep(1);
    if (n) {
        g_aDrained.fetch_add(n, std::memory_order_relaxed);
        g_aDrains.fetch_add(1, std::memory_order_relaxed);
    }
    return n;
}

DWORD WINAPI ReleaseThread(void*)
{
    for (;;) {
        WaitForSingleObject(g_asyncWake, 1000);
        while (!g_asyncPaused.load(std::memory_order_relaxed) && !g_asyncHold.load(std::memory_order_relaxed)) {
            AcquireSRWLockExclusive(&g_q.lock);
            if (!g_q.count) {
                ReleaseSRWLockExclusive(&g_q.lock);
                break;
            }
            g_inFlight.store(1);  // before the queue shows empty (OsPageCacheTestWaitIdle, the failed-request drain)
            const AsyncEntry e = PopLocked();
            ReleaseSRWLockExclusive(&g_q.lock);
            NoteWait(e);
            const uint64_t t0 = QpcNow();
            g_osFree(e.ptr);
            const uint64_t t1 = QpcNow();
            if (g_asyncFreq.QuadPart && t1 > t0) {  // the thread's own time in the call (the test's reading)
                const uint64_t us = QpcToMicroseconds(t1 - t0, static_cast<uint64_t>(g_asyncFreq.QuadPart));
                g_aThreadUs.fetch_add(us, std::memory_order_relaxed);
                StoreMax(g_aThreadMaxUs, us);
            }
            g_aReleased.fetch_add(1, std::memory_order_relaxed);
            g_inFlight.store(0);
        }
    }
}

bool StartReleaseThread()
{
    if (g_asyncThread) return true;
    QueryPerformanceFrequency(&g_asyncFreq);
    if (!g_asyncWake) g_asyncWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_asyncWake) return false;
    HANDLE t = CreateThread(nullptr, 64 * 1024, ReleaseThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return false;
    DescribeThread(t, L"OblivionPerfFix OS release");
    CloseHandle(t);
    g_asyncThread = true;
    return true;
}

// Applies the key with the cache's own state (SetOsPageCache); a switch-off drains on the calling thread (the 1-second tick).
void ApplyAsync(bool cacheOn)
{
    const bool want = g_asyncKey.load() && cacheOn && !g_asyncPaused.load();
    if (want && !g_asyncThread && !StartReleaseThread()) {
        static bool logged = false;
        if (!logged) Log("[oscache] OsPageCacheAsyncRelease: the release thread could not be started, releases stay on the calling thread");
        logged = true;
        return;
    }
    g_asyncOn.store(want);
    if (want) g_asyncEverOn = true;
    if ((!want || g_asyncLow.load()) && !g_asyncPaused.load())
        DrainQueue(false);  // off or memory low: everything queued goes now (also a release that raced the switch)
}

void ResetCache(Cache& c)
{
    c.freeNodes = kNil;
    for (uint32_t i = kNodes; i-- > 0;) {
        c.nodes[i].older = c.freeNodes;
        c.freeNodes = i;
    }
    memset(c.buckets, 0, sizeof c.buckets);
    c.oldest = c.newest = kNil;
    c.held = c.peak = 0;
    c.count = 0;
}

uint32_t FindBucket(Cache& c, uint64_t size, bool create)
{
    uint32_t h = static_cast<uint32_t>(((size >> 12) * 0x9E3779B97F4A7C15ull) >> 52) & (kBuckets - 1);
    for (uint32_t n = 0; n < kBuckets; ++n, h = (h + 1) & (kBuckets - 1)) {
        Bucket& b = c.buckets[h];
        if (b.key == size) return h;
        if (!b.key) {
            if (!create) return kNil;
            b.key = size;
            b.newest = b.oldest = kNil;
            return h;
        }
    }
    return kNil;
}

// unlink node i from both lists (lock held)
void Unlink(Cache& c, uint32_t i)
{
    Node& n = c.nodes[i];
    if (n.older != kNil) c.nodes[n.older].newer = n.newer; else c.oldest = n.newer;
    if (n.newer != kNil) c.nodes[n.newer].older = n.older; else c.newest = n.older;
    Bucket& b = c.buckets[n.bucket];
    if (n.sizeOlder != kNil) c.nodes[n.sizeOlder].sizeNewer = n.sizeNewer; else b.oldest = n.sizeNewer;
    if (n.sizeNewer != kNil) c.nodes[n.sizeNewer].sizeOlder = n.sizeOlder; else b.newest = n.sizeOlder;
    b.bytes -= n.bytes;
    --b.count;
    c.held -= n.bytes;
    --c.count;
    n.older = c.freeNodes;
    c.freeNodes = i;
}

void* Take(uint64_t size)
{
    Cache& c = *g_cache;
    void* p = nullptr;
    AcquireSRWLockExclusive(&c.lock);
    const uint32_t bi = c.count ? FindBucket(c, size, false) : kNil;
    if (bi != kNil && c.buckets[bi].count) {
        const uint32_t i = c.buckets[bi].newest;  // the most recently released block: the warmest
        p = c.nodes[i].ptr;
        Unlink(c, i);
    }
    ReleaseSRWLockExclusive(&c.lock);
    return p;
}

// Keeps a released block; false = release it to the OS as the game does.
bool Put(void* ptr, uint64_t size)
{
    const uint64_t cap = g_cap.load(std::memory_order_relaxed);
    if (!ptr || size < 4096 || (size & 4095) || size > MaxBlock(cap)) return false;
    Cache& c = *g_cache;
    void* evict[kMaxEvict + 2];
    uint64_t evictBytes[kMaxEvict + 2];  // their sizes (OsPageCacheAsyncRelease's byte bound)
    int ne = 0;
    bool kept = true;
    const uint64_t now = g_nowMs();
    AcquireSRWLockExclusive(&c.lock);
    const uint32_t bi = FindBucket(c, size, true);
    if (bi == kNil) {
        kept = false;
    } else {
        if (c.freeNodes == kNil) {  // no node left: the oldest block goes
            evictBytes[ne] = c.nodes[c.oldest].bytes;
            evict[ne++] = c.nodes[c.oldest].ptr;
            Unlink(c, c.oldest);
            g_evictRoom.fetch_add(1, std::memory_order_relaxed);
        }
        Bucket& b = c.buckets[bi];
        const uint32_t i = c.freeNodes;
        Node& n = c.nodes[i];
        c.freeNodes = n.older;
        n.ptr = ptr;
        n.bytes = size;
        n.freedMs = now;
        n.bucket = bi;
        n.older = c.newest;
        n.newer = kNil;
        if (c.newest != kNil) c.nodes[c.newest].newer = i; else c.oldest = i;
        c.newest = i;
        n.sizeOlder = b.newest;
        n.sizeNewer = kNil;
        if (b.newest != kNil) c.nodes[b.newest].sizeNewer = i; else b.oldest = i;
        b.newest = i;
        b.bytes += size;
        ++b.count;
        c.held += size;
        ++c.count;
        // one size holds at most half the cap: its oldest blocks go first
        while (b.bytes > cap / 2 && b.oldest != i && ne < kMaxEvict) {
            evictBytes[ne] = c.nodes[b.oldest].bytes;
            evict[ne++] = c.nodes[b.oldest].ptr;
            Unlink(c, b.oldest);
            g_evictSize.fetch_add(1, std::memory_order_relaxed);
        }
        // the cap: the oldest blocks go
        while (c.held > cap && c.oldest != i && ne < kMaxEvict) {
            evictBytes[ne] = c.nodes[c.oldest].bytes;
            evict[ne++] = c.nodes[c.oldest].ptr;
            Unlink(c, c.oldest);
            g_evictCap.fetch_add(1, std::memory_order_relaxed);
        }
        if (c.held > cap || b.bytes > cap / 2) {  // still over: this block is not kept either
            Unlink(c, i);
            kept = false;
        }
        if (c.held > c.peak) c.peak = c.held;
    }
    ReleaseSRWLockExclusive(&c.lock);
    for (int k = 0; k < ne; ++k) OsRelease(evict[k], evictBytes[k]);
    return kept;
}

// Releases blocks older than maxAgeMs (all with maxAgeMs = 0) to the OS, outside the lock, in batches.
uint64_t Release(uint64_t maxAgeMs)
{
    if (!g_cache) return 0;
    Cache& c = *g_cache;
    uint64_t total = 0;
    for (;;) {
        void* batch[256];
        int n = 0;
        const uint64_t now = g_nowMs();
        AcquireSRWLockExclusive(&c.lock);
        while (n < 256 && c.oldest != kNil && (!maxAgeMs || now - c.nodes[c.oldest].freedMs >= maxAgeMs)) {
            batch[n++] = c.nodes[c.oldest].ptr;
            Unlink(c, c.oldest);
        }
        ReleaseSRWLockExclusive(&c.lock);
        for (int k = 0; k < n; ++k) g_osFree(batch[k]);
        total += n;
        if (n < 256) return total;
    }
}

void* AllocCore(SIZE_T size, bool cachePath)
{
    if (cachePath && g_on.load(std::memory_order_relaxed)) {
        if (void* p = Take(size)) {
            g_hits.fetch_add(1, std::memory_order_relaxed);
            g_hitBytes.fetch_add(size, std::memory_order_relaxed);
            return p;
        }
        g_misses.fetch_add(1, std::memory_order_relaxed);
        g_missBytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = g_osAlloc(size);
    // out of memory or address space: what the cache holds and what waits in the release queue go back, then once more
    if (!p && ((g_cache && g_cache->count) || g_qLen.load(std::memory_order_relaxed) || g_inFlight.load(std::memory_order_relaxed))) {
        g_allocFails.fetch_add(1, std::memory_order_relaxed);
        DrainQueue(true);
        g_flushed.fetch_add(Release(0), std::memory_order_relaxed);
        p = g_osAlloc(size);
    }
    return p;
}

BOOL FreeCore(void* ptr, SIZE_T size)
{
    if (g_on.load(std::memory_order_relaxed)) {
        if (Put(ptr, size)) {
            g_puts.fetch_add(1, std::memory_order_relaxed);
            return TRUE;
        }
        g_passed.fetch_add(1, std::memory_order_relaxed);
        return OsRelease(ptr, size);  // OsPageCacheAsyncRelease (as g_osFree while it is off)
    }
    return g_osFree(ptr);
}

// ---- the detours ----
void* AllocHook(SIZE_T size)
{
    const void* const ret = _ReturnAddress();
    const bool cachePath = ret == g_cacheRet[0] || ret == g_cacheRet[1];
    return AllocCore(size, cachePath);
}

BOOL FreeHook(void* ptr, SIZE_T size)
{
    return FreeCore(ptr, size);
}

// In DllMain no thread allocates, so a plain 5-byte jump at the entry. Before WinMain (OBSE64 route) the allocator is already
// live (GMalloc is made by the first allocation in the exe's static set-up, and any thread allocating through FMemory may run
// the thunks), and a 5-byte jump would cover the first instruction and part of the second (alloc thunk: 3 + 6 bytes, free
// thunk: 2 + 6): a thread paused between them would resume inside the jump. So there the hot-patch form (obseparity review
// S1): the 5-byte jump goes into the 0xCC padding before the thunk (no thread runs it), then one 2-byte short jump back to
// it (EB F9) replaces the start of the first instruction in one atomic store. Without 5 bytes of padding: not patched.
constexpr uint8_t kShortBack[2] = {0xEB, 0xF9};  // jmp at - 5

bool HotPatchFits(const uint8_t* at)
{
    for (int i = 1; i <= 5; ++i)
        if (at[-i] != 0xCC) return false;
    return WriteCodeAtomicFits(at, 2);
}

bool PatchJmp(uint8_t* at, const uint8_t* stub)
{
    const bool hot = CurrentLoadStage() != LoadStage::InDllMain;
    uint8_t* const jmpAt = hot ? at - 5 : at;
    const int64_t rel = stub - (jmpAt + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t code[5] = {0xE9};
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(code + 1, &r32, 4);
    if (!hot) return WriteCode(at, code);
    if (!HotPatchFits(at)) {
        Log("[oscache] exe+0x%llX: no 5 bytes of padding before it, so it cannot be switched in one step while other threads "
            "may allocate: not patched", static_cast<unsigned long long>(at - Game().base));
        return false;
    }
    return WriteCode(jmpAt, code) && WriteCodeAtomic(at, kShortBack);
}

// AllocateImpl's cache-miss calls: "test r14,r14; je; mov rcx,r14; call [EnterCriticalSection]; mov rcx,rbp; call
// BinnedAllocFromOS; mov rcx,r14; mov rdi,rax; call [LeaveCriticalSection]; jmp; mov rcx,rbp; call BinnedAllocFromOS;
// mov rdi,rax; test rdi,rdi" (the mutex-released call and the one without a mutex, UE 5.3 CachedOSPageAllocator.cpp).
const char kAllocateImpl[] =
    "4D 85 F6 74 1F 49 8B CE FF 15 ?? ?? ?? ?? 48 8B CD E8 ?? ?? ?? ?? 49 8B CE 48 8B F8 FF 15 ?? ?? ?? ?? EB 0B 48 8B CD "
    "E8 ?? ?? ?? ?? 48 8B F8 48 85 FF";
constexpr size_t kCall1 = 17, kCall2 = 39;

}  // namespace

// self-test: the thunk patch on a fake thunk as the current stage writes it (review 2, R1: the hot-patch path)
bool OsPageCacheTestPatchThunk(uint8_t* thunk, const uint8_t* stub) { return PatchJmp(thunk, stub); }

bool InstallOsPageCache(bool beforeGameCode, const wchar_t* traceDir, int trace)
{
    if (g_installed) return true;
    if (!beforeGameCode) {
        Log("[oscache] only installed while the game starts (loaded late here), not installed");
        return false;
    }
    uint8_t* const a = FindOne(kAllocThunk, "BinnedAllocFromOS");
    uint8_t* const f = FindOne(kFreeThunk, "BinnedFreeToOS");
    uint8_t* const ai = FindOne(kAllocateImpl, "FCachedOSPageAllocator::AllocateImpl's OS calls");
    if (!a || !f || !ai) return false;
    if (RipTarget(ai + kCall1, 1, 5) != a || RipTarget(ai + kCall2, 1, 5) != a) {
        Log("[oscache] AllocateImpl's calls do not go to BinnedAllocFromOS: not installed");
        return false;
    }
    void** const as = reinterpret_cast<void**>(RipTarget(a + 17, 3, 7));
    void** const fs = reinterpret_cast<void**>(RipTarget(f + 8, 3, 7));
    void** const impA = FindImportSlot(Game().base, "KERNEL32.dll", "VirtualAlloc");
    void** const impF = FindImportSlot(Game().base, "KERNEL32.dll", "VirtualFree");
    if (as != impA || fs != impF || !as || !fs) {
        Log("[oscache] the OS thunks do not jump through the exe's VirtualAlloc / VirtualFree imports: not installed");
        return false;
    }
    g_allocSlot = as;
    g_freeSlot = fs;
    g_cache = static_cast<Cache*>(VirtualAlloc(nullptr, sizeof(Cache), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_cache) {
        Log("[oscache] no memory for the cache's table: not installed");
        return false;
    }
    new (g_cache) Cache();
    ResetCache(*g_cache);
    g_cacheRet[0] = ai + kCall1 + 5;
    g_cacheRet[1] = ai + kCall2 + 5;
    (void)traceDir;
    (void)trace;
    uint8_t* const region = AllocNearGame(64);
    if (!region) {
        Log("[oscache] no memory near the game's code: not installed");
        return false;
    }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const ah = reinterpret_cast<void*>(&AllocHook);
    void* const fh = reinterpret_cast<void*>(&FreeHook);
    memcpy(region, jmpAbs, 6);
    memcpy(region + 6, &ah, 8);
    memcpy(region + 16, jmpAbs, 6);
    memcpy(region + 22, &fh, 8);
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    if (CurrentLoadStage() != LoadStage::InDllMain && (!HotPatchFits(a) || !HotPatchFits(f))) {
        Log("[oscache] an OS thunk has no 5 bytes of padding before it: it cannot be switched in one step while other "
            "threads may allocate (loaded before WinMain): not installed");
        return false;
    }
    if (!PatchJmp(a, region)) {
        Log("[oscache] could not patch the OS thunks: not installed");
        return false;
    }
    if (!PatchJmp(f, region + 16)) {  // put BinnedAllocFromOS back: half a cache (allocs kept, frees not seen) is not wanted
        const uint8_t orig[5] = {0x48, 0x8B, 0xD1, 0x41, 0xB9};
        if (CurrentLoadStage() == LoadStage::InDllMain) WriteCode(a, orig);
        else WriteCodeAtomic(a, std::span<const uint8_t>(orig, 2));  // the short jump out; the padding's jump is never run
        Log("[oscache] could not patch the OS thunks: not installed");
        return false;
    }
    g_allocFn = a;
    g_freeFn = f;
    g_installed = true;
    Log("[oscache] installed: BinnedAllocFromOS exe+0x%llX, BinnedFreeToOS exe+0x%llX, AllocateImpl's cache-miss calls "
        "exe+0x%llX / exe+0x%llX",
        static_cast<unsigned long long>(a - Game().base), static_cast<unsigned long long>(f - Game().base),
        static_cast<unsigned long long>(ai + kCall1 - Game().base), static_cast<unsigned long long>(ai + kCall2 - Game().base));
    return true;
}

// Live (main's 1-second tick): the key, the hidden cap / trim keys, MemoryRelief's RAM/commit relief; trims old blocks.
void SetOsPageCache(int mode, int capMB, int trimSeconds, bool relief)
{
    if (!g_installed) return;
    const uint64_t cap = static_cast<uint64_t>((std::clamp)(capMB, 4, 1024)) << 20;
    g_cap.store(cap, std::memory_order_relaxed);
    g_trimMs.store(static_cast<uint64_t>((std::clamp)(trimSeconds, 1, 600)) * 1000, std::memory_order_relaxed);
    const bool on = mode != 0 && !relief;
    if (relief != g_relief.exchange(relief) && mode)
        Log("[oscache] %s", relief ? "Windows is short of RAM or commit: the kept blocks go back, released blocks go to Windows"
                                   : "memory recovered: released blocks are kept again");
    static int lastCap = -1, lastTrim = -1;
    if (mode != g_mode || (mode && (capMB != lastCap || trimSeconds != lastTrim)))
        Log("[oscache] %s (cap %llu MB, unused blocks released after %llu s)", mode ? "on: released OS blocks are kept for reuse" : "off: as the game",
            static_cast<unsigned long long>(cap >> 20), static_cast<unsigned long long>(g_trimMs.load() / 1000));
    lastCap = capMB;
    lastTrim = trimSeconds;
    g_on.store(on);
    g_mode = mode;
    {  // OsPageCacheAsyncRelease follows the cache: off (and drained) while the cache is off or relieved
        const bool was = g_asyncOn.load();
        ApplyAsync(on);
        if (was != g_asyncOn.load())
            Log("[oscache] OsPageCacheAsyncRelease %s", g_asyncOn.load() ? "on: released blocks go to Windows from a background thread"
                                                                         : "off: released blocks go to Windows at once, as the game");
    }
    if (!on) g_flushed.fetch_add(Release(0), std::memory_order_relaxed);
    else {
        g_trimmed.fetch_add(Release(g_trimMs.load()), std::memory_order_relaxed);
        if (g_cache && g_cache->held > cap) {  // the cap was lowered: down to it, oldest first
            Cache& c = *g_cache;
            while (c.held > cap) {
                AcquireSRWLockExclusive(&c.lock);
                void* p = nullptr;
                if (c.held > cap && c.oldest != kNil) {
                    p = c.nodes[c.oldest].ptr;
                    Unlink(c, c.oldest);
                }
                ReleaseSRWLockExclusive(&c.lock);
                if (!p) break;
                g_osFree(p);
                g_trimmed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void LogOsPageCacheStats()
{
    if (!g_installed || !g_cache) return;
    static uint64_t last = 0;
    const uint64_t sum = g_hits.load() + g_puts.load() + g_misses.load() + g_passed.load();
    if (sum == last) return;
    last = sum;
    Log("[oscache] hits %llu (%.0f MB), misses %llu (%.0f MB), kept %llu, passed %llu; released: over cap %llu, one size over "
        "half %llu, no slot %llu, unused %llu, flushed %llu, failed allocations %llu; holding %u blocks %.1f MB (peak %.1f MB)",
        static_cast<unsigned long long>(g_hits.load()), g_hitBytes.load() / 1048576.0, static_cast<unsigned long long>(g_misses.load()),
        g_missBytes.load() / 1048576.0, static_cast<unsigned long long>(g_puts.load()), static_cast<unsigned long long>(g_passed.load()),
        static_cast<unsigned long long>(g_evictCap.load()), static_cast<unsigned long long>(g_evictSize.load()),
        static_cast<unsigned long long>(g_evictRoom.load()), static_cast<unsigned long long>(g_trimmed.load()),
        static_cast<unsigned long long>(g_flushed.load()), static_cast<unsigned long long>(g_allocFails.load()), g_cache->count,
        g_cache->held / 1048576.0, g_cache->peak / 1048576.0);
    if (g_asyncEverOn)
        Log("[oscache] async release: queued %llu (%.0f MB), released by the thread %llu (in VirtualFree %.1f ms, longest call "
            "%.2f ms), drained %llu (%llu drains), at once: queue full %llu, bytes full %llu, lock busy %llu, memory low %llu, "
            "key off %llu, paused %llu, drain lock busy %llu; queue now %u, largest %llu blocks %.1f MB, longest wait %.2f ms",
            static_cast<unsigned long long>(g_aQueued.load()), g_aQueuedBytes.load() / 1048576.0,
            static_cast<unsigned long long>(g_aReleased.load()), g_aThreadUs.load() / 1000.0, g_aThreadMaxUs.load() / 1000.0,
            static_cast<unsigned long long>(g_aDrained.load()), static_cast<unsigned long long>(g_aDrains.load()),
            static_cast<unsigned long long>(g_aFullBlocks.load()), static_cast<unsigned long long>(g_aFullBytes.load()),
            static_cast<unsigned long long>(g_aBusy.load()), static_cast<unsigned long long>(g_aLow.load()),
            static_cast<unsigned long long>(g_aOffWait.load()), static_cast<unsigned long long>(g_aPaused.load()),
            static_cast<unsigned long long>(g_aDrainBusy.load()), g_qLen.load(), static_cast<unsigned long long>(g_aMaxBlocks.load()),
            g_aMaxBytes.load() / 1048576.0, g_aMaxWaitUs.load() / 1000.0);
}

// [Fixes] OsPageCacheAsyncRelease, live (main's 1-second tick, before SetOsPageCache, which applies it).
void SetOsPageCacheAsyncRelease(int mode)
{
    if (!g_installed) return;
    g_asyncKey.store(mode != 0);
}

// MemoryRelief's monitor, every one-second sample: true while RAM or commit room is under twice its relief thresholds.
void OsPageCacheMemoryLow(bool low) { g_asyncLow.store(low); }

// A crash is being written: the release thread stops (after the one block it may be releasing at that moment) and
// releases are made at once again; the queued ranges stay reserved and untouched, so a dump that reads them sees what
// it saw a moment before. Safe from any thread, no lock, no wait.
void OsPageCacheAsyncPause()
{
    g_asyncPaused.store(true);
    g_asyncOn.store(false);
}

bool OsPageCacheInfo(OsPageCacheInfoOut* out)
{
    if (!g_allocFn) return false;
    out->allocFn = g_allocFn;
    out->freeFn = g_freeFn;
    out->cacheRet[0] = static_cast<const uint8_t*>(g_cacheRet[0]);
    out->cacheRet[1] = static_cast<const uint8_t*>(g_cacheRet[1]);
    return true;
}

// ---- self-test hooks: the cache core with fake OS functions (no game) ----
void OsPageCacheTestInit(void* (*osAlloc)(SIZE_T), BOOL (*osFree)(void*), uint64_t (*nowMs)())
{
    static Cache* testCache = nullptr;
    if (!testCache) testCache = static_cast<Cache*>(VirtualAlloc(nullptr, sizeof(Cache), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    new (testCache) Cache();
    ResetCache(*testCache);
    g_cache = testCache;
    g_osAlloc = osAlloc;
    g_osFree = osFree;
    g_nowMs = nowMs;
    g_installed = true;
    g_on = false;
    g_relief = false;
    g_mode = 0;
    for (auto* s : {&g_hits, &g_hitBytes, &g_misses, &g_missBytes, &g_puts, &g_passed, &g_evictCap, &g_evictSize, &g_evictRoom,
                    &g_trimmed, &g_flushed, &g_allocFails})
        s->store(0);
    // OsPageCacheAsyncRelease: off, an empty queue (a release thread of an earlier init stays, idle)
    g_asyncKey = false;
    g_asyncOn = false;
    g_asyncPaused = false;
    g_asyncHold = false;
    g_asyncLow = false;
    AcquireSRWLockExclusive(&g_q.lock);
    g_q.head = g_q.count = 0;
    g_q.bytes = 0;
    g_qLen = 0;
    ReleaseSRWLockExclusive(&g_q.lock);
    for (auto* s : {&g_aQueued, &g_aQueuedBytes, &g_aReleased, &g_aDrained, &g_aFullBlocks, &g_aFullBytes, &g_aBusy, &g_aPaused,
                    &g_aMaxBlocks, &g_aMaxBytes, &g_aMaxWaitUs, &g_aDrains, &g_aDrainBusy, &g_aOffWait, &g_aLow, &g_aThreadUs,
                    &g_aThreadMaxUs})
        s->store(0);
}
BOOL OsPageCacheTestRelease(void* ptr, size_t bytes) { return OsRelease(ptr, bytes); }
void OsPageCacheTestHold(bool hold)
{
    g_asyncHold = hold;
    if (!hold && g_asyncWake) SetEvent(g_asyncWake);
}
void OsPageCacheTestResume() { g_asyncPaused = false; }  // after OsPageCacheAsyncPause (the next SetOsPageCache applies the key)
bool OsPageCacheTestWaitIdle(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; ++t) {
        if (!g_qLen.load() && !g_inFlight.load()) return true;
        Sleep(1);
    }
    return !g_qLen.load() && !g_inFlight.load();
}
void OsPageCacheTestLockQueue(bool lock)  // a thread that ended inside the queue's lock (the process's exit)
{
    if (lock) AcquireSRWLockExclusive(&g_q.lock);
    else ReleaseSRWLockExclusive(&g_q.lock);
}
bool OsPageCacheTestAsyncOn() { return g_asyncOn.load(); }
void OsPageCacheTestAsyncStats(OsPageCacheAsyncStatsOut* out)
{
    out->queued = g_aQueued.load();
    out->released = g_aReleased.load();
    out->drained = g_aDrained.load();
    out->fullBlocks = g_aFullBlocks.load();
    out->fullBytes = g_aFullBytes.load();
    out->busy = g_aBusy.load();
    out->paused = g_aPaused.load();
    out->drainBusy = g_aDrainBusy.load();
    out->maxBlocks = g_aMaxBlocks.load();
    out->maxBytes = g_aMaxBytes.load();
    out->offWait = g_aOffWait.load();
    out->low = g_aLow.load();
    out->threadUs = g_aThreadUs.load();
    out->queuedNow = g_qLen.load();
}
void* OsPageCacheTestAlloc(size_t size, bool cachePath) { return AllocCore(size, cachePath); }
BOOL OsPageCacheTestFree(void* ptr, size_t size) { return FreeCore(ptr, size); }
void OsPageCacheTestStats(OsPageCacheStatsOut* out)
{
    out->hits = g_hits.load();
    out->misses = g_misses.load();
    out->kept = g_puts.load();
    out->passed = g_passed.load();
    out->evictCap = g_evictCap.load();
    out->evictSize = g_evictSize.load();
    out->trimmed = g_trimmed.load();
    out->flushed = g_flushed.load();
    out->allocFails = g_allocFails.load();
    out->blocks = g_cache ? g_cache->count : 0;
    out->held = g_cache ? g_cache->held : 0;
}


}  // namespace opf
