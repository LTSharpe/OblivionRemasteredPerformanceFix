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
    int ne = 0;
    bool kept = true;
    const uint64_t now = g_nowMs();
    AcquireSRWLockExclusive(&c.lock);
    const uint32_t bi = FindBucket(c, size, true);
    if (bi == kNil) {
        kept = false;
    } else {
        if (c.freeNodes == kNil) {  // no node left: the oldest block goes
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
            evict[ne++] = c.nodes[b.oldest].ptr;
            Unlink(c, b.oldest);
            g_evictSize.fetch_add(1, std::memory_order_relaxed);
        }
        // the cap: the oldest blocks go
        while (c.held > cap && c.oldest != i && ne < kMaxEvict) {
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
    for (int k = 0; k < ne; ++k) g_osFree(evict[k]);
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
    if (!p && g_cache && g_cache->count) {  // out of memory or address space: what the cache holds goes back, once more
        g_allocFails.fetch_add(1, std::memory_order_relaxed);
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

bool PatchJmp(uint8_t* at, const uint8_t* stub)
{
    const int64_t rel = stub - (at + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t code[5] = {0xE9};
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(code + 1, &r32, 4);
    return WriteCode(at, code);
}

// AllocateImpl's cache-miss calls: "test r14,r14; je; mov rcx,r14; call [EnterCriticalSection]; mov rcx,rbp; call
// BinnedAllocFromOS; mov rcx,r14; mov rdi,rax; call [LeaveCriticalSection]; jmp; mov rcx,rbp; call BinnedAllocFromOS;
// mov rdi,rax; test rdi,rdi" (the mutex-released call and the one without a mutex, UE 5.3 CachedOSPageAllocator.cpp).
const char kAllocateImpl[] =
    "4D 85 F6 74 1F 49 8B CE FF 15 ?? ?? ?? ?? 48 8B CD E8 ?? ?? ?? ?? 49 8B CE 48 8B F8 FF 15 ?? ?? ?? ?? EB 0B 48 8B CD "
    "E8 ?? ?? ?? ?? 48 8B F8 48 85 FF";
constexpr size_t kCall1 = 17, kCall2 = 39;

}  // namespace

bool InstallOsPageCache(bool beforeGameCode, const wchar_t* traceDir, int trace)
{
    if (g_installed) return true;
    if (!beforeGameCode) {
        Log("[oscache] only installed while the game starts (version.dll), not installed");
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
    if (!PatchJmp(a, region) || !PatchJmp(f, region + 16)) {
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
