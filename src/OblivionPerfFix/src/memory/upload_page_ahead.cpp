// UploadPagePreGrow (worker s083build, 2026-11-26; S-083; public tree, default 0 until a release): the D3D12 fast allocator keeps
// enough 4 MB upload pages for a streaming burst, and a page that is missing is made on our own thread instead of inside the
// burst with the allocator's lock held.
//
// Why (findings fortemp / s083spec, our play-session kmt data from 2026-11-15 to 2026-11-25): after a cell crossing on the road, texture streaming
// creates its new textures' upload data through the device's one FD3D12FastAllocator (4 MB pages of upload memory; UE 5.3
// D3D12Allocation.cpp; RHIAsyncCreateTexture2D, texture locks, buffer initial data). A page is reused once the GPU has finished
// the frame it was last used in and its texture copy has dropped its reference; when no page is free, RequestFastAllocatorPage
// makes a new one (CreateBuffer + Map, 3.2-4.2 ms in the driver) with the allocator's critical section held, so a burst of
// 6-18 new pages runs in series (36-83 ms) and the render thread's own driver calls queue behind it: a 43-98 ms frame at 5 of
// 5 such events in the plays with the probe (Fort Empire road from Aleswell among them). The pool is not empty then: it sits AT
// its floor (UploadMemoryKeep's UploadPagesToKeep, 24 pages; the game's d3d12.FastAllocator.MinPagesToRetain is 5), and the
// burst needs 30-36 pages at once (the RHI thread released exactly as many pages as were created, one a frame, 0.2 s after
// each event). The pool only grows on demand, inside the burst, and is trimmed back to the floor within 0.2-0.4 s.
// Epic: RequestFastAllocatorPage is unchanged in UE 5.4-5.7; from 5.5 RHIAsyncCreateTexture2D takes its upload memory from the
// pooled upload-heap allocator instead (a backport would hook a large function; not done). The make-outside-the-lock idea is
// Epic's for buffer pools (ue5-main 982b0cbcab, CL 50684724; GpuPoolSpareAhead adopted it), applied here to these pages.
//
// With the key on:
//  - floor: MinPagesToRetain is UploadPagePreGrowTarget (hidden, 40 = 160 MB; 24..64) instead of UploadPagesToKeep's 24, so the
//    game's clean-up no longer trims the pool below what a burst needs. While LoadPeakKeepsBack gives our keeps back behind a
//    loading screen near the video memory budget, and while video memory is tight (the game's use within 1 GB of the DXGI
//    budget; room again 1.5 GB below it), the floor stays UploadPagesToKeep; under MemoryRelief the game's 5 (UploadMemoryKeep's own rule).
//  - pre-grow: when the pool holds fewer pages than the floor (a fresh session, after MemoryRelief or a tight spell, after a load
//    near the budget), OPF-PoolAhead (GpuPoolSpareAhead's thread; installed for this key alone when that key is off) makes the
//    missing pages one at a time, at most one per 50 ms, never behind a loading screen, never within UploadPagePreGrowQuietMs
//    (hidden, 300) of a page the game made itself (the burst's own work goes first), never while tight. Each page is made by
//    the game's own RequestFastAllocatorPage called on a private copy of the pool's header with an empty page list: the function
//    scans the empty list, then makes the page exactly as it would (new page object, Adapter->CreateBuffer with the pool's heap
//    properties, DoNotDeferDelete, Map) and returns it, touching no shared allocator state (checked in the exe: it reads the
//    header's parent device, GPU mask, page size and heap properties, and the list only to scan it). Then the allocator's own
//    critical section is taken (a few microseconds) and the game's ReturnFastAllocatorPage puts the page at the end of the list,
//    as the game does with every page it has finished with (fence = the next frame's: usable once that frame completes).
//  - only pages are added; the game's clean-up still decides every release (one a frame above the floor, unused for 10 frames).
// Identical results: the upload data the game writes and copies is the same; which page serves an allocation can differ (the
// scan order is the game's; a page made earlier is simply there), and nothing drawn or simulated depends on that. Memory only: up
// to (40 - 24) x 4 MB = 64 MB more upload memory than UploadMemoryKeep's floor (video memory on cards with resizable BAR), none
// near the budget, behind a near-budget loading screen or under MemoryRelief.
// The game's end: OPF-PoolAhead calls game code only between PoolAheadEnterGame / PoolAheadLeaveGame (the GpuPoolSpareAhead end
// protocol: the D3D12 device's destructor deletes the buffer pool allocators before it destroys the fast allocator; the first
// deletion waits, bounded, until our thread is out of its call, and from then on it calls nothing), so a page we made is always
// in the list before the device's teardown deletes the list's pages.
//
// Probe (same hook; the [upage] lines): the one call of RequestFastAllocatorPage (in FD3D12FastAllocator::Allocate's slow path,
// the allocator's lock held) goes through us: a page is "created" when the pool's page count did not drop (served pages are
// removed from it). Counts and times per report; with UploadPageStats (hidden, 0) also the free pages and one line per burst
// (>= 3 pages created within 200 ms). Also the time of the last creation, which UploadPageScreenRelease reads.
//
// Found by byte pattern (build 19115871 addresses in the comments): Allocate's slow path (Return / Request calls, the page pool's
// and current page's offsets), its lock (lea of the critical section, EnterCriticalSection import), RequestFastAllocatorPage's
// start, scan and new-page code (the pool list and page-size offsets; the frame fence's cached completed value), and
// ReturnFastAllocatorPage's start. Any mismatch: one log line, nothing written.
// [Fixes] UploadPagePreGrow = 0 (default until the release): off; 1: on (needs UploadMemoryKeep). Start-up install, then live.
// Hidden: UploadPagePreGrowTarget (40, 24..64), UploadPagePreGrowQuietMs (300, 0..2000), UploadPageStats (0/1).
#include "common.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// FD3D12FastAllocator::Allocate's slow path (exe+0x24F2E04): lea r12,[rdi+10h] (PagePool); mov rdx,r8; mov rcx,r12;
// call ReturnFastAllocatorPage; jmp +4; lea r12,[rdi+10h]; mov rcx,r12; >> call RequestFastAllocatorPage <<; mov r8,rax;
// mov [rdi+48h],rax (CurrentAllocatorPage)
constexpr char kSlow[] = "4C 8D 67 ?? 49 8B D0 49 8B CC E8 ?? ?? ?? ?? EB 04 4C 8D 67 ?? 49 8B CC E8 ?? ?? ?? ?? 4C 8B C0 48 89 47 ??";
constexpr size_t kSlowPool1 = 3, kSlowReturn = 10, kSlowPool2 = 20, kSlowRequest = 24, kSlowCurrent = 35;
// ... its lock (exe+0x24F2DBC, 0x48 bytes before): lea rbx,[rcx+50h] (the CS); mov [rsp+88h],r12; mov rcx,rbx; mov [rsp+90h],r13;
// call [EnterCriticalSection]
constexpr char kLock[] = "48 8D 59 ?? 4C 89 A4 24 ?? ?? ?? ?? 48 8B CB 4C 89 AC 24 ?? ?? ?? ?? FF 15";
constexpr size_t kLockCs = 3, kLockCall = 23, kLockMin = 0x30, kLockMax = 0x80;
// ... Allocate's start (exe+0x24F2C90): mov [rsp+20h],rbx; push rbp,rsi,rdi,r14,r15; sub rsp,50h; mov r14,r9; mov ebp,r8d;
// mov rdi,rcx (the allocator); mov r15d,edx; cmp edx,[rcx+20h] (PagePool.PageSize); jbe >> the lock << : so the lock's rcx and
// the slow path's rdi are the same allocator (review S2-1)
constexpr char kAllocStart[] = "48 89 5C 24 20 55 56 57 41 56 41 57 48 83 EC 50 4D 8B F1 41 8B E8 48 8B F9 44 8B FA 3B 51 20 0F 86";
constexpr size_t kAllocJbe = 31, kAllocBack = 0x200;
constexpr uint32_t kFenceOffExpected = 0x548;  // FD3D12Adapter's frame fence (build 19115871; review S2-1)
// RequestFastAllocatorPage (exe+0x2505AC0): push rbx,rsi,rdi,r12,r15; sub rsp,60h; mov rax,[rcx] (parent device); mov rdi,rcx;
// mov dl,1; mov r15,[rax+10h] (adapter); mov rcx,[r15+548h] (frame fence); call GetCompletedFenceValue
constexpr char kRequest[] = "40 53 56 57 41 54 41 57 48 83 EC 60 48 8B 01 48 8B F9 B2 01 4C 8B 78 10 49 8B 8F ?? ?? ?? ?? E8";
constexpr size_t kRequestFence = 27, kRequestFenceCall = 31;
// ... the scan: cmp [rdi+30h],r12d (Pool.Num); jle
constexpr char kRequestNum[] = "44 39 67 30 7E";
// ... mov rcx,[rdi+28h] (Pool.Data); xor eax,eax; mov rbx,[rdx+rcx]; mov rcx,[rbx+8] (the page's buffer); lock cmpxchg [rcx+8],r12d
// (its reference count); cmp eax,1; jne; cmp [rbx+20h],r8 (the page's frame fence)
constexpr char kRequestScan[] = "48 8B 4F 28 33 C0 48 8B 1C 0A 48 8B 4B 08 F0 44 0F B1 61 08 83 F8 01 75 ?? 4C 39 43 20";
// ... the new page: mov ecx,28h; mov [rsp+0A0h],r14; call new; mov rbx,rax; test rax,rax; je; mov eax,[rdi+10h] (PageSize); mov [rbx],eax
constexpr char kRequestNew[] = "B9 28 00 00 00 4C 89 B4 24 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 8B 47 10 89 03";
constexpr size_t kRequestBody = 0xC0;  // the three above within this many bytes of the start (0x74 in build 19115871)
// GetCompletedFenceValue's cached branch: mov rax,[rcx+20h]; add rsp,20h; pop rbp; ret
constexpr char kFenceCached[] = "48 8B 41 20 48 83 C4 20 5D C3";
constexpr size_t kFenceBody = 0x80;
// ReturnFastAllocatorPage (exe+0x2506760): mov [rsp+8],rbx; mov [rsp+10h],rsi; push rdi; sub rsp,20h; mov rax,[rdx+8]; mov rsi,rdx
constexpr char kReturn[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 42 08 48 8B F2";
constexpr char kReturnPool[] = "48 8D 59 28";  // lea rbx,[rcx+28h] (Pool), within 0x40 bytes
constexpr size_t kReturnBody = 0x40;

// The page pool's layout (FD3D12FastAllocatorPagePool, build 19115871), checked against the patterns above
constexpr uint32_t kHeader = 0x28;    // parent device, GPU / visibility masks, PageSize (+10h), HeapProperties (+14h..+28h)
constexpr uint32_t kPoolData = 0x28, kPoolNum = 0x30, kPoolEnd = 0x38;
constexpr uint32_t kPageSizeOff = 0x10, kHeapTypeOff = 0x14;
constexpr uint32_t kPageBuffer = 0x08, kPageFence = 0x20, kBufferRefs = 0x08;
constexpr uint32_t kHeapUpload = 2;   // D3D12_HEAP_TYPE_UPLOAD
constexpr uint32_t kPageSize = 4u << 20;
constexpr uint64_t kBurstGapMs = 200;
constexpr uint32_t kMinGapMs = 50;    // at most one page made per this many ms

using RequestFn = void* (*)(void* pool);
using ReturnFn = void (*)(void* pool, void* page);

struct Sites {
    uint8_t* call = nullptr;     // the call of RequestFastAllocatorPage in Allocate
    uint8_t* request = nullptr;
    uint8_t* ret = nullptr;      // ReturnFastAllocatorPage
    uint32_t csOff = 0;          // the allocator's critical section, from the page pool
    uint32_t fenceOff = 0;       // the frame fence in the adapter (+548h)
    uint32_t fenceCachedOff = 0; // its cached completed value (+20h)
};
Sites g;
bool g_found = false, g_installed = false, g_workInstalled = false;
RequestFn g_request = nullptr, g_testRequest = nullptr;
ReturnFn g_return = nullptr, g_testReturn = nullptr;
bool g_testMode = false;
int g_testKept = 0, g_testMin = 0;  // self-test: UploadMemoryKeep's floor as if set
uint8_t* g_stub = nullptr;

std::atomic<bool> g_on{false}, g_stats{false}, g_tight{false}, g_broken{false};
std::atomic<int> g_target{40};
std::atomic<uint32_t> g_quietMs{300};
std::atomic<void*> g_pool{nullptr};       // the device's page pool (the first one seen; one device)
std::atomic<uint64_t> g_lastCreate{0};    // QPC of the last page the game made on demand
uint64_t g_lastMade = 0;                  // OPF-PoolAhead only: QPC of our last page
uint64_t g_qpf = 10000000;
alignas(16) uint8_t g_private[0x100];     // OPF-PoolAhead only: the header copy with an empty page list (never freed)

std::atomic<uint64_t> s_served{0}, s_created{0}, s_createdUs{0}, s_createdMaxUs{0}, s_other{0};
std::atomic<uint64_t> s_made{0}, s_madeUs{0}, s_madeMaxUs{0}, s_lockMaxUs{0}, s_skipLoading{0}, s_skipTight{0}, s_skipQuiet{0};
std::atomic<int> s_freeMin{-1}, s_freeMax{-1};

// bursts (written under the fast allocator's lock; handed to the logger under a leaf lock)
struct Burst {
    uint64_t start, last;
    uint32_t n, us, poolBefore;
    int freeBefore;
    int minPages;
};
SRWLOCK g_burstLock = SRWLOCK_INIT;
Burst g_cur = {};
constexpr int kRing = 16;
Burst g_ring[kRing];
int g_ringN = 0;
std::atomic<uint64_t> s_burstsDropped{0};

uint64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
uint64_t Us(uint64_t ticks) { return ticks / g_qpf * 1000000 + ticks % g_qpf * 1000000 / g_qpf; }
void Max(std::atomic<uint64_t>& m, uint64_t v)
{
    uint64_t c = m.load(std::memory_order_relaxed);
    while (v > c && !m.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}
template <class T> T& F(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }

int Num(void* pool) { return reinterpret_cast<volatile int32_t*>(static_cast<uint8_t*>(pool) + kPoolNum)[0]; }

// Outside the allocator's lock (our thread, the monitor's stats): -1 once the game's end began or if unreadable (review S2-3)
int SafeNum(void* pool)
{
    if (!pool || PoolAheadEnding()) return -1;
    __try {
        return Num(pool);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

RequestFn Request() { return g_testRequest ? g_testRequest : g_request; }
ReturnFn Return() { return g_testReturn ? g_testReturn : g_return; }

// Under the allocator's lock: pages in the list that the game's scan would serve now (by the fence's cached completed value: it
// may lag the real one by a frame, so this is a lower bound). -1 if unreadable.
int FreeNow(void* pool)
{
    __try {
        void* const dev = F<void*>(pool, 0);
        void* const adapter = dev ? F<void*>(dev, 0x10) : nullptr;
        void* const fence = adapter ? F<void*>(adapter, g.fenceOff) : nullptr;
        if (!fence && !g_testMode) return -1;
        const uint64_t done = fence ? F<uint64_t>(fence, g.fenceCachedOff) : ~0ull;
        void** const data = F<void**>(pool, kPoolData);
        const int n = Num(pool);
        int free = 0;
        for (int i = 0; i < n && i < 4096; ++i) {
            void* const page = data[i];
            void* const buf = F<void*>(page, kPageBuffer);
            if (buf && F<int32_t>(buf, kBufferRefs) == 1 && F<uint64_t>(page, kPageFence) <= done) ++free;
        }
        return free;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool PoolLooksRight(void* pool)
{
    __try {
        return F<void*>(pool, 0) != nullptr && F<uint32_t>(pool, kPageSizeOff) == kPageSize && F<uint32_t>(pool, kHeapTypeOff) == kHeapUpload &&
               Num(pool) >= 0 && Num(pool) < 100000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void NoteFree(int free)
{
    if (free < 0) return;
    int mn = s_freeMin.load(std::memory_order_relaxed);
    while ((mn < 0 || free < mn) && !s_freeMin.compare_exchange_weak(mn, free, std::memory_order_relaxed)) {}
    int mx = s_freeMax.load(std::memory_order_relaxed);
    while (free > mx && !s_freeMax.compare_exchange_weak(mx, free, std::memory_order_relaxed)) {}
}

void PushBurst(const Burst& b)  // under g_burstLock
{
    if (b.n < 3) return;
    if (g_ringN < kRing) g_ring[g_ringN++] = b;
    else s_burstsDropped.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

// The probe: the game's call of RequestFastAllocatorPage (Allocate's slow path, the fast allocator's lock held).
extern "C" void* UploadPageRequestHook(void* pool)
{
    void* known = g_pool.load(std::memory_order_relaxed);
    if (!known && PoolLooksRight(pool)) {
        void* expected = nullptr;
        if (g_pool.compare_exchange_strong(expected, pool)) known = pool;
        else known = expected;
    }
    if (pool != known) {  // another allocator (a second device): the game's call, nothing of ours
        s_other.fetch_add(1, std::memory_order_relaxed);
        return Request()(pool);
    }
    const bool stats = g_stats.load(std::memory_order_relaxed);
    const int before = Num(pool);
    const int freeBefore = stats ? FreeNow(pool) : -1;
    const uint64_t t0 = Now();
    void* const page = Request()(pool);
    const uint64_t t1 = Now();
    if (Num(pool) != before) {  // served from the list (the scan removed it)
        s_served.fetch_add(1, std::memory_order_relaxed);
        if (stats) NoteFree(freeBefore);
        return page;
    }
    const uint64_t us = Us(t1 - t0);
    g_lastCreate.store(t1, std::memory_order_relaxed);
    s_created.fetch_add(1, std::memory_order_relaxed);
    s_createdUs.fetch_add(us, std::memory_order_relaxed);
    Max(s_createdMaxUs, us);
    if (stats) {
        NoteFree(freeBefore);
        AcquireSRWLockExclusive(&g_burstLock);
        if (!g_cur.n || Us(t0 - g_cur.last) / 1000 > kBurstGapMs) {
            PushBurst(g_cur);
            g_cur = {t0, t1, 0, 0, static_cast<uint32_t>(before), freeBefore, UploadMinPagesNow()};
        }
        g_cur.n += 1;
        g_cur.us += static_cast<uint32_t>(us);
        g_cur.last = t1;
        ReleaseSRWLockExclusive(&g_burstLock);
    }
    return page;
}

// The pre-grow decision alone (self-test): make one page now? *retryMs = when to look again.
bool UploadPagePreGrowDecide(bool on, int keptPages, int minPages, int target, int poolNum, int loading, bool tight, uint64_t sinceCreateMs,
                             uint64_t sinceMadeMs, uint32_t quietMs, uint32_t* retryMs, int* why)
{
    *retryMs = 1000;
    *why = 0;
    if (!on || keptPages <= 0 || minPages <= 0) return false;  // the key off, UploadMemoryKeep's floor not ours (off, MemoryRelief)
    const int want = target < minPages ? target : minPages;   // never above the floor in force (pages above it would be released)
    if (poolNum >= want) return false;
    if (loading == 1) { *retryMs = 250; *why = 1; return false; }  // behind a loading screen the load makes them
    if (tight) { *why = 2; return false; }
    if (sinceCreateMs < quietMs) {  // the game is making pages itself: its burst first
        *retryMs = static_cast<uint32_t>(quietMs - sinceCreateMs) + 1;
        *why = 3;
        return false;
    }
    if (sinceMadeMs < kMinGapMs) { *retryMs = static_cast<uint32_t>(kMinGapMs - sinceMadeMs) + 1; return false; }
    *retryMs = kMinGapMs;
    return true;
}

// The floor UploadMemoryKeep keeps (self-test): the pre-grow target while on and not tight, else UploadPagesToKeep.
int UploadPagePreGrowFloor(bool on, bool tight, int keepPages, int target)
{
    if (!on || tight) return keepPages;
    const int t = target < 24 ? 24 : target > 64 ? 64 : target;
    return t > keepPages ? t : keepPages;
}

namespace {
// One round on OPF-PoolAhead: at most one page. Returns the ms until it wants to look again.
uint32_t Work()
{
    void* const pool = g_pool.load(std::memory_order_relaxed);
    if (!pool || g_broken.load(std::memory_order_relaxed) || PoolAheadEnding()) return 1000;
    const int num = SafeNum(pool);
    if (num < 0) return 1000;
    const uint64_t now = Now();
    const uint64_t lastCreate = g_lastCreate.load(std::memory_order_relaxed);
    const uint64_t sinceCreate = now > lastCreate ? Us(now - lastCreate) / 1000 : 0;
    const uint64_t sinceMade = g_lastMade && now > g_lastMade ? Us(now - g_lastMade) / 1000 : ~0ull;
    uint32_t retry = 1000;
    int why = 0;
    const bool make = UploadPagePreGrowDecide(g_on.load(std::memory_order_relaxed), g_testMode ? g_testKept : UploadPagesKeptNow(),
                                              g_testMode ? g_testMin : UploadMinPagesNow(),
                                              g_target.load(std::memory_order_relaxed), num, LoadingScreenNow(),
                                              g_tight.load(std::memory_order_relaxed), lastCreate ? sinceCreate : ~0ull, sinceMade,
                                              g_quietMs.load(std::memory_order_relaxed), &retry, &why);
    if (why == 1) s_skipLoading.fetch_add(1, std::memory_order_relaxed);
    else if (why == 2) s_skipTight.fetch_add(1, std::memory_order_relaxed);
    else if (why == 3) s_skipQuiet.fetch_add(1, std::memory_order_relaxed);
    if (!make) return retry;
    if (!PoolAheadEnterGame()) return 1000;  // the game's end began: no game call any more
    // the header as it is (constant after the pool's construction), the page list empty: the game's scan finds nothing
    memset(g_private, 0, sizeof g_private);
    memcpy(g_private, pool, kHeader);
    const uint64_t t0 = Now();
    void* const page = Request()(g_private);
    const uint64_t t1 = Now();
    bool ok = page != nullptr && F<int32_t>(g_private, kPoolNum) == 0 && F<void*>(g_private, kPoolData) == nullptr;
    if (ok && PoolAheadEnding()) {  // the device's teardown began while we were in the request: the page is left (the process ends)
        PoolAheadLeaveGame();
        return 1000;
    }
    if (ok) {
        auto* const cs = reinterpret_cast<CRITICAL_SECTION*>(static_cast<uint8_t*>(pool) + g.csOff);
        EnterCriticalSection(cs);
        const uint64_t t2 = Now();
        Return()(pool, page);  // the game's own: fence = the next frame's, appended to the list
        LeaveCriticalSection(cs);
        Max(s_lockMaxUs, Us(t2 - t1));
    }
    PoolAheadLeaveGame();
    g_lastMade = t1;
    if (!ok) {  // never seen: the private list was touched or no page came back: off until the game restarts (the page, if any, leaks)
        g_broken.store(true, std::memory_order_relaxed);
        Log("[upage] UploadPagePreGrow: the game's page request did not behave as expected on the private pool copy: off until the game "
            "restarts");
        return 1000;
    }
    const uint64_t us = Us(t1 - t0);
    s_made.fetch_add(1, std::memory_order_relaxed);
    s_madeUs.fetch_add(us, std::memory_order_relaxed);
    Max(s_madeMaxUs, us);
    return retry;
}
}  // namespace

bool FindUploadPageAhead(const GameModule& game, uint32_t* callRva, uint32_t* requestRva, uint32_t* returnRva, uint32_t* csOff)
{
    if (!g_found) {
        const auto hits = FindAll(game.text, ParsePattern(kSlow));
        if (hits.size() != 1) {
            Log("[upage] FD3D12FastAllocator::Allocate's page switch found %zu times (expected 1): UploadPagePreGrow unavailable", hits.size());
            return false;
        }
        uint8_t* const at = hits[0];
        Sites s;
        s.call = at + kSlowRequest;
        s.request = RipTarget(s.call, 1, 5);
        s.ret = RipTarget(at + kSlowReturn, 1, 5);
        const uint8_t poolOff = at[kSlowPool1];
        if (at[kSlowPool2] != poolOff || poolOff != 0x10 || at[kSlowCurrent] != 0x48) {
            Log("[upage] Allocate's page pool / current page offsets differ (0x%X / 0x%X): UploadPagePreGrow unavailable", poolOff, at[kSlowCurrent]);
            return false;
        }
        // the lock taken before: lea rbx,[rcx+CS] ... call [EnterCriticalSection]
        void** const enter = FindImportSlot(game.base, "KERNEL32.dll", "EnterCriticalSection");
        const Pattern lock = ParsePattern(kLock);
        uint8_t* lk = nullptr;
        for (size_t back = kLockMin; back <= kLockMax && !lk; ++back)
            if (MatchesAt(at - back, lock)) lk = at - back;
        if (!lk || !enter || reinterpret_cast<void**>(RipTarget(lk + kLockCall, 2, 6)) != enter || lk[kLockCs] <= poolOff) {
            Log("[upage] Allocate's lock (EnterCriticalSection before the page switch) not found: UploadPagePreGrow unavailable");
            return false;
        }
        s.csOff = static_cast<uint32_t>(lk[kLockCs] - poolOff);  // 0x50 - 0x10 = 0x40
        {
            const Pattern ps = ParsePattern(kAllocStart);
            bool tied = false;
            for (size_t back = 0; back <= kAllocBack && !tied; ++back)
                tied = MatchesAt(lk - back, ps) && RipTarget(lk - back + kAllocJbe, 2, 6) == lk;
            if (!tied) {
                Log("[upage] Allocate's start (mov rdi,rcx; jbe to the lock) not found: UploadPagePreGrow unavailable");
                return false;
            }
        }
        if (s.csOff < kPoolEnd) { Log("[upage] the allocator's lock lies inside the page pool: UploadPagePreGrow unavailable"); return false; }
        // RequestFastAllocatorPage: start, scan of the list (+28h/+30h, page +8/+20h), new page (+10h PageSize)
        uint8_t* q = nullptr;
        if (!game.text.Contains(s.request) || !MatchesAt(s.request, ParsePattern(kRequest))) {
            Log("[upage] RequestFastAllocatorPage's start does not match: UploadPagePreGrow unavailable");
            return false;
        }
        bool num = false, scan = false, nw = false;
        const Pattern pn = ParsePattern(kRequestNum), ps = ParsePattern(kRequestScan), pw = ParsePattern(kRequestNew);
        for (q = s.request; q < s.request + kRequestBody; ++q) {
            num = num || MatchesAt(q, pn);
            scan = scan || MatchesAt(q, ps);
            nw = nw || MatchesAt(q, pw);
        }
        if (!num || !scan || !nw) {
            Log("[upage] RequestFastAllocatorPage's list scan / new page do not match: UploadPagePreGrow unavailable");
            return false;
        }
        s.fenceOff = *reinterpret_cast<const uint32_t*>(s.request + kRequestFence);
        uint8_t* const fenceFn = RipTarget(s.request + kRequestFenceCall, 1, 5);
        bool cached = false;
        if (game.text.Contains(fenceFn)) {
            const Pattern pc = ParsePattern(kFenceCached);
            for (q = fenceFn; q < fenceFn + kFenceBody && !cached; ++q) cached = MatchesAt(q, pc);
        }
        if (!cached || s.fenceOff != kFenceOffExpected) {
            Log("[upage] the frame fence's cached value not found: UploadPagePreGrow unavailable");
            return false;
        }
        s.fenceCachedOff = 0x20;
        bool retPool = false;
        if (game.text.Contains(s.ret) && MatchesAt(s.ret, ParsePattern(kReturn))) {
            const Pattern pr = ParsePattern(kReturnPool);
            for (q = s.ret; q < s.ret + kReturnBody && !retPool; ++q) retPool = MatchesAt(q, pr);
        }
        if (!retPool) {
            Log("[upage] ReturnFastAllocatorPage does not match: UploadPagePreGrow unavailable");
            return false;
        }
        g = s;
        g_found = true;
    }
    if (callRva) *callRva = static_cast<uint32_t>(g.call - game.base);
    if (requestRva) *requestRva = static_cast<uint32_t>(g.request - game.base);
    if (returnRva) *returnRva = static_cast<uint32_t>(g.ret - game.base);
    if (csOff) *csOff = g.csOff;
    return true;
}

// Start-up: the probe (the Request call) and, with pre-grow wanted, the OPF-PoolAhead host and our work item.
bool InstallUploadPageAhead(bool beforeGameCode, bool preGrow)
{
    if (g_installed) return true;
    const GameModule& game = Game();
    if (!FindUploadPageAhead(game, nullptr, nullptr, nullptr, nullptr)) return false;
    if (!beforeGameCode) { Log("[upage] UploadPagePreGrow: only installed while the game starts"); return false; }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = static_cast<uint64_t>(f.QuadPart);
    if (preGrow && !InstallPoolAheadHost(true, "UploadPagePreGrow")) {
        Log("[upage] UploadPagePreGrow: no OPF-PoolAhead thread: pages are not made ahead (the floor and the probe still act)");
        preGrow = false;
    }
    uint8_t* const stub = AllocNearGame(16);
    if (!stub) { Log("[upage] UploadPagePreGrow: no memory near the game's code: not installed"); return false; }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    const void* hook = reinterpret_cast<const void*>(&UploadPageRequestHook);
    memcpy(stub, jmpAbs, 6);
    memcpy(stub + 6, &hook, 8);
    DWORD old;
    if (!VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &old)) { Log("[upage] UploadPagePreGrow: stub not executable: not installed"); return false; }
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const int64_t rel = stub - (g.call + 5);
    if (rel != static_cast<int32_t>(rel)) { Log("[upage] UploadPagePreGrow: the stub is out of reach: not installed"); return false; }
    g_request = reinterpret_cast<RequestFn>(g.request);
    g_return = reinterpret_cast<ReturnFn>(g.ret);
    uint8_t patch[5] = {0xE8, 0, 0, 0, 0};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(patch + 1, &rel32, 4);
    if (!WriteCode(g.call, patch)) { Log("[upage] UploadPagePreGrow: could not patch the call: not installed"); return false; }
    g_stub = stub;
    g_installed = true;
    if (preGrow) {
        SetPoolAheadWork(&Work);
        g_workInstalled = true;
    }
    Log("[upage] UploadPagePreGrow installed: RequestFastAllocatorPage exe+0x%X (its call exe+0x%X), ReturnFastAllocatorPage exe+0x%X, "
        "the allocator's lock at page pool +0x%X; %s",
        static_cast<unsigned>(g.request - game.base), static_cast<unsigned>(g.call - game.base), static_cast<unsigned>(g.ret - game.base),
        g.csOff, preGrow ? "missing pages made on OPF-PoolAhead" : "probe and floor only");
    return true;
}

bool UploadPageAheadInstalled() { return g_installed; }
bool UploadPagePreGrowReady() { return g_installed && g_workInstalled; }  // the work item too (review S3-1)
uint32_t UploadPageAheadTestFenceOff() { return g.fenceOff; }

void SetUploadPagePreGrow(bool on, int target, int quietMs, bool stats, bool tight)
{
    const int t = target < 24 ? 24 : target > 64 ? 64 : target;
    g_target.store(t, std::memory_order_relaxed);
    g_quietMs.store(quietMs < 0 ? 0u : quietMs > 2000 ? 2000u : static_cast<uint32_t>(quietMs), std::memory_order_relaxed);
    g_stats.store(stats, std::memory_order_relaxed);
    const bool wasTight = g_tight.exchange(tight);
    const bool was = g_on.exchange(on);
    if (!g_installed) return;
    if (was != on) {
        if (on) Log("[upage] UploadPagePreGrow on: up to %d fast-allocator pages kept and made ahead", t);
        else Log("[upage] UploadPagePreGrow off: the floor is UploadPagesToKeep again, no pages made ahead");
    }
    if (on && wasTight != tight)
        Log(tight ? "[upage] video memory tight: the fast allocator's floor back to UploadPagesToKeep, no pages made ahead"
                  : "[upage] video memory has room again: floor %d pages", t);
    if (on) PoolAheadWake();
}

uint64_t UploadPageLastCreateQpc() { return g_lastCreate.load(std::memory_order_relaxed); }

void LogUploadPageStats()
{
    if (!g_installed) return;
    const uint64_t served = s_served.exchange(0), created = s_created.exchange(0), cUs = s_createdUs.exchange(0), cMax = s_createdMaxUs.exchange(0),
                   made = s_made.exchange(0), mUs = s_madeUs.exchange(0), mMax = s_madeMaxUs.exchange(0), lockMax = s_lockMaxUs.exchange(0),
                   skL = s_skipLoading.exchange(0), skT = s_skipTight.exchange(0), skQ = s_skipQuiet.exchange(0), other = s_other.exchange(0);
    const int fMin = s_freeMin.exchange(-1), fMax = s_freeMax.exchange(-1);
    void* const pool = g_pool.load(std::memory_order_relaxed);
    const int num = SafeNum(pool);
    if (created || made || other || (g_stats.load(std::memory_order_relaxed) && served))
        Log("[upage] since last report: %llu pages created on demand under the allocator's lock (%.1f ms, longest %.1f), %llu served from "
            "the pool, %llu made ahead on OPF-PoolAhead (%.1f ms, longest %.1f; lock held %.2f ms at most); pool %d pages (free %d..%d), "
            "floor %d; ahead skipped: %llu loading, %llu tight, %llu quiet waits%s",
            static_cast<unsigned long long>(created), cUs / 1000.0, cMax / 1000.0, static_cast<unsigned long long>(served),
            static_cast<unsigned long long>(made), mUs / 1000.0, mMax / 1000.0, lockMax / 1000.0, num, fMin, fMax, UploadMinPagesNow(),
            static_cast<unsigned long long>(skL), static_cast<unsigned long long>(skT), static_cast<unsigned long long>(skQ),
            other ? "; another device's allocator seen (left alone)" : "");
    // bursts: the finished ones, and the current one once 200 ms have passed
    Burst out[kRing + 1];
    int n = 0;
    AcquireSRWLockExclusive(&g_burstLock);
    for (int i = 0; i < g_ringN; ++i) out[n++] = g_ring[i];
    g_ringN = 0;
    if (g_cur.n && Us(Now() - g_cur.last) / 1000 > kBurstGapMs) {
        if (g_cur.n >= 3) out[n++] = g_cur;
        g_cur = {};
    }
    ReleaseSRWLockExclusive(&g_burstLock);
    for (int i = 0; i < n; ++i)
        Log("[upage] burst: %u pages created on demand in %.0f ms (%.1f ms each) at qpc_ms %.1f, pool before %u free %d, floor %d",
            out[i].n, Us(out[i].last - out[i].start) / 1000.0, out[i].us / 1000.0 / out[i].n, out[i].start * 1000.0 / g_qpf,
            out[i].poolBefore, out[i].freeBefore, out[i].minPages);
    if (const uint64_t d = s_burstsDropped.exchange(0)) Log("[upage] %llu more bursts not listed", static_cast<unsigned long long>(d));
}

// ---- self-test ----
bool UploadPageAheadTestPatched()
{
    return g_installed && g.call[0] == 0xE8 && RipTarget(g.call, 1, 5) == g_stub &&
           *reinterpret_cast<void* const*>(g_stub + 6) == reinterpret_cast<void*>(&UploadPageRequestHook);
}
void UploadPageAheadTestSetup(void* request, void* ret, uint32_t csOff)
{
    g_testRequest = reinterpret_cast<RequestFn>(request);
    g_testReturn = reinterpret_cast<ReturnFn>(ret);
    g_testMode = request != nullptr;
    if (csOff) g.csOff = csOff;
    g_pool = nullptr;
    g_lastCreate = 0;
    g_lastMade = 0;
    g_broken = false;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = static_cast<uint64_t>(f.QuadPart);
    AcquireSRWLockExclusive(&g_burstLock);
    g_cur = {};
    g_ringN = 0;
    ReleaseSRWLockExclusive(&g_burstLock);
    s_served = 0; s_created = 0; s_made = 0;
}
uint32_t UploadPageAheadTestWork() { return Work(); }
void UploadPageAheadTestFloor(int kept, int minPages) { g_testKept = kept; g_testMin = minPages; }
void UploadPageAheadTestCounts(uint64_t* served, uint64_t* created, uint64_t* made, int* bursts, bool* broken)
{
    *served = s_served.load(); *created = s_created.load(); *made = s_made.load(); *broken = g_broken.load();
    AcquireSRWLockShared(&g_burstLock);
    *bursts = g_ringN + (g_cur.n >= 3 ? 1 : 0);
    ReleaseSRWLockShared(&g_burstLock);
}
void UploadPageAheadTestAge(uint64_t ms)  // as if the last creation / our last page were `ms` ago
{
    const uint64_t back = ms * g_qpf / 1000, now = Now();
    if (g_lastCreate.load()) g_lastCreate = now > back ? now - back : 1;
    if (g_lastMade) g_lastMade = now > back ? now - back : 1;
}

}  // namespace opf
