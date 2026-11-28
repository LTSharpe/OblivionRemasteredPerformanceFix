// GpuPoolSpareAhead (worker poolahead, 2026-11-23; S-001): a spare D3D12 memory pool is made ahead on our own thread, so a texture
// or buffer allocator that runs out of room takes a ready pool instead of making one with its lock held.
//
// UE 5.3's D3D12 RHI sub-allocates textures and buffers from memory pools (FD3D12PoolAllocator: 64 MB pools for streamed
// read-only textures, 32 MB for buffers). When no pool has room, FRHIPoolAllocator::TryAllocateInternal calls CreateNewPool with
// the allocator's critical section held. CreateNewPool makes the pool's video memory (ID3D12Device::CreateHeap or a committed
// buffer), and Windows makes it resident before the call returns: in play, 34.7-43.4 ms of paging-fence wait on a texture
// streaming worker for each new 64 MB texture pool at Kvatch / Shetcombe (findings play112b), while the RHI thread (its end-of-frame
// clean-up takes the same lock) and then the render thread and the game thread waited behind it: 3 of that play's 5 red drops.
// GpuPoolKeepMB keeps EMPTY pools for the next burst; this is the other half, real growth (a new area's textures).
//
// Epic's own design, adopted here: Unreal Engine ue5-main commit 982b0cbcab (CL 50684724, 2026-02-09, "asynchronous pool
// allocation strategy for D3D12 buffer allocators"; not in any 5.x release up to 5.7.4) keeps one "overflow" pool per allocator,
// made by CreateNewPool with an invalid pool index on a background task outside the lock; on a miss under the lock the ready pool
// gets the index and its free list (SetPoolIndex + InitFreeBlocks) and goes where the new pool would have gone; after each such
// allocation another one is requested. Epic covers upload and default-buffer allocators; the stalls in our data are texture pools,
// so this key covers every allocator whose pools are video memory (the DEFAULT heap: textures and buffers).
//
// How: the allocator class's vtable slot for CreateNewPool (its only caller is TryAllocateInternal, with the lock held) points to
// our hook. Hook: if the allocator's pools are video memory and the game's size for this call is the allocator's default pool size,
// and our spare for (allocator, resource type) is ready, the spare is checked to be untouched (vtable, our sentinel index 0x7FFF in
// the pool, its head block and its one free block, size, free size, resource type, the free block linked to the head, nothing
// allocated, LastUsedFrameFence 0), gets the index the game asked for written into those three fields (the only places
// FRHIMemoryPool::Init puts it) and is returned; otherwise the game's own call runs (timed for the stats line). After either, one
// spare for (allocator, type) is requested if there is none. Our thread (OPF-PoolAhead) makes requested spares with the game's
// own CreateNewPool (index 0x7FFF, minimum size 0 = the default size), outside every allocator lock: the function reads only the
// allocator's constant configuration (checked in the exe: no lock, no pool list), as in Epic's design.
//
// Identical results: the pool the game gets is the one its own CreateNewPool would have made for that call (same allocator and
// configuration, same size, same resource type, same index, a fresh free list, empty, never used), and the game's own code puts it
// into the slot it chose and places the allocation in it, so offsets, pool indices and the pool's later life (defragmentation,
// GpuPoolKeepMB, the 20-frame clean-up) are as in the game. What differs: the pool's memory was made earlier, on our thread (the
// driver's wait no longer holds the lock), and up to one extra pool per growing allocator is held.
// Bound: at most one spare per (allocator, resource type); the spares' bytes stay within half of GpuPoolKeepMB's budget as it is
// now (0 under MemoryRelief or while video memory is tight, as the keep), and the keep's budget is reduced by them, so kept plus
// spare pools stay within GpuPoolKeepMB. A request that does not fit gives back the spare of the allocator that grew least recently.
// Given back (destroyed on our thread, as the game's clean-up does: Destroy, then the deleting destructor): over the cap, off.
// The game's end (fix-up poolfix, review S1): the allocators are deleted only in the D3D12 device's destructor, through the same
// vtable's slot 0 (the deleting destructor; no direct call to it in the exe). That slot points to our second hook: the first such
// delete marks the end, waits (bounded: 2 s, once; review2 F1) until our thread is out of any game call (a spare being made or
// given back: up to ~45 ms in our data, and only if one is in flight), and from then on our thread calls no game code; each deleted
// allocator's spares are
// destroyed on the deleting thread just before the allocator's own pools, as if they were in its list. So no make or give-back
// can run against a deleted allocator or a device being torn down. Quiet wait (review S3, hidden GpuPoolSpareQuietMs, 0 = at
// once): a spare is made only once no video-memory pool was made or taken for that many ms (the burst's paging work first).
//
// [Fixes] GpuPoolSpareAhead = 0: not installed (start-up); 1: installed (needs GpuPoolKeepMB > 0), then live (0 = give back).
// Hidden GpuPoolSpareQuietMs (0..2000, default 0; live).
#include "common.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// FD3D12PoolAllocator::CreateNewPool(int16 PoolIndex, uint32 MinimumAllocationSize, ERHIPoolResourceTypes Type), exe+0x251FAE0 in
// build 19115871: push rbp, r12-r15; sub rsp,30h; mov r15d,[rcx+8] (DefaultPoolSize); mov r12d,r9d; movzx r13d,dx; mov rbp,rcx;
// cmp r8d,r15d; jbe (the default size); mov r15d,[rcx+18h] (MaxAllocationSize: the power-of-two case)
constexpr char kCreate[] = "40 55 41 54 41 55 41 56 41 57 48 83 EC 30 44 8B 79 08 45 8B E1 44 0F B7 EA 48 8B E9 45 3B C7 76 ?? 44 8B 79 18";
// ... its body: mov ecx,0E8h; call (new: the pool's size)
constexpr char kCreateNew[] = "B9 E8 00 00 00 E8";
// ... InitConfig from the allocator (+98h; HeapType its first dword) copied to the pool (+0A0h)
constexpr char kCreateConfig[] = "0F 10 85 98 00 00 00 41 0F 11 86 A0 00 00 00";
// ... the pool's vtable: lea rax,[rip+x]; mov [r14+9Ch],ebx; mov [r14],rax
constexpr char kCreateVtbl[] = "48 8D 05 ?? ?? ?? ?? 41 89 9E 9C 00 00 00 49 89 06";
// ... LastUsedFrameFence (+0D8h) = 0; then Init (vtable +8) and the pool returned
constexpr char kCreateTail[] = "49 89 86 D8 00 00 00 EB 03 4C 8B F0 49 8B 16 49 8B CE FF 52 08 49 8B C6";
constexpr size_t kCreateBody = 0x140;  // all of the above lie within this many bytes of the start (0x12A in build 19115871)
// FRHIMemoryPool::Init: mov rax,[rcx+10h] (PoolSize); lea rdi,[rcx+80h] (AllocationDataPool); mov [rcx+28h],rax (FreeSize)
constexpr char kPoolInit[] = "40 57 41 56 48 83 EC 28 48 8B 41 10 48 8D B9 80 00 00 00 48 89 41 28 4C 8B F1";
// ... the head block: movzx ecx,word [r14+8] (PoolIndex); lea rsi,[r14+40h] (HeadBlock) ... mov word [rsi+0Ch],cx
constexpr char kPoolInitHead[] = "41 0F B7 4E 08 49 8D 76 40";
constexpr char kPoolInitHeadIdx[] = "66 89 4E 0C";
// ... the free block: movzx edx,word [r14+8]; ... mov word [rbx+0Ch],dx; ... FreeBlocks.Add (+70h data, +78h num)
constexpr char kPoolInitFreeIdx[] = "66 89 53 0C";
constexpr char kPoolInitFreeAdd[] = "49 63 7E 78";
constexpr size_t kPoolInitBody = 0x1E0;  // 0x1D7 in build 19115871
// FRHIPoolAllocator::TryAllocateInternal: lea rbx,[rcx+28h] (the CS); mov r15,rcx; mov rcx,rbx; mov r14d,r9d; mov r13d,edx;
// call [EnterCriticalSection]
constexpr char kTryAlloc[] = "48 8D 59 28 4C 8B F9 48 8B CB 45 8B F1 44 8B EA FF 15";
// ... its two CreateNewPool calls through the vtable (+8): new slot at the end, and a free slot
constexpr char kTryAllocCall1[] = "49 8B 07 45 8B CE 0F BF F9 45 8B C5 0F B7 D7 49 8B CF FF 50 08";
constexpr char kTryAllocCall2[] = "49 8B 07 45 8B CE 45 8B C5 0F B7 D7 49 8B CF FF 50 08";
constexpr size_t kTryAllocBody = 0x1B0;  // the calls at +0xA3 / +0x168 from the lea in build 19115871
// FD3D12PoolAllocator's deleting destructor, slot 0 of the same vtable (exe+0x25172A0): mov [rsp+8],rbx; push rdi; sub rsp,20h;
// lea rax,[rip+x] (x = this vtable); mov edi,edx (the flags); mov [rcx],rax; mov rbx,rcx
constexpr char kAllocDtor[] = "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 8B FA 48 89 01 48 8B D9";
// ... then FRHIPoolAllocator::Destroy (its pools: Destroy + delete each), the members, and: test dil,1 (delete this)
constexpr char kAllocDtorDelete[] = "40 F6 C7 01";
constexpr size_t kAllocDtorBody = 0xA0;  // 0x78 in build 19115871
constexpr uint32_t kEndWaitMs = 2000;  // the game's end: longest wait for our thread to leave a game call (a make is ~45 ms)

// allocator (FRHIPoolAllocator + FD3D12PoolAllocator) and pool (FRHIMemoryPool + FD3D12MemoryPool) fields, build 19115871
constexpr uint32_t kDefaultPoolSize = 0x08, kHeapType = 0x98;
constexpr uint32_t kPoolIndex = 0x08, kPoolSize = 0x10, kPoolTypes = 0x1C, kFreeSize = 0x28, kWaste = 0x30, kAllocated = 0x38,
                   kHead = 0x40, kFreeBlocks = 0x70, kFreeBlocksNum = 0x78, kLastUsed = 0xD8;
constexpr uint32_t kBlkSize = 0x00, kBlkOffset = 0x08, kBlkBits = 0x0C, kBlkPrev = 0x10, kBlkNext = 0x18;
constexpr uint32_t kHeapDefault = 1;  // D3D12_HEAP_TYPE_DEFAULT: video memory
constexpr uint16_t kSpareIndex = 0x7FFF;  // our "not placed yet" index (the game's indices are Pools.Num() at most)
constexpr int kMaxSpares = 64;

using CreateFn = void* (*)(void* allocator, uint16_t index, uint32_t minSize, uint32_t type);
using DestroyFn = void (*)(void* pool);
using DeleteFn = void* (*)(void* pool, uint32_t flags);

CreateFn g_orig = nullptr;
CreateFn g_testOrig = nullptr;  // self-test only
DeleteFn g_origDtor = nullptr;
DeleteFn g_testOrigDtor = nullptr;  // self-test only
uint32_t g_endWaitMs = kEndWaitMs;  // self-test: shorter
const void* g_poolVtbl = nullptr;
bool g_installed = false;      // the CreateNewPool slot is ours (GpuPoolSpareAhead)
bool g_hostInstalled = false;  // the OPF-PoolAhead thread and the allocator destructor's end hook (also UploadPagePreGrow's host)
std::atomic<PoolAheadWorkFn> g_extraWork{nullptr};  // UploadPagePreGrow's work item (s083build), run on the same thread after the spares
bool g_testNoThread = false;  // self-test: requests are made by GpuPoolSpareTestWork, not by the thread
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_cap{0};  // bytes the spares may hold now (half of GpuPoolKeepMB's budget now)
std::atomic<uint64_t> g_bytes{0};  // bytes held or being made
std::atomic<bool> g_broken{false};  // a spare did not look as Init leaves it: off until the game restarts
std::atomic<bool> g_ending{false};  // an allocator was deleted (the device's teardown): our thread calls no game code any more
std::atomic<uint32_t> g_quietMs{0};  // GpuPoolSpareQuietMs: make a spare only once no pool was made or taken for this long
std::atomic<uint64_t> g_lastGrowth{0};  // QPC of the last video-memory CreateNewPool call (taken or made)

enum State : uint8_t { kFree, kPending, kMaking, kReady };
struct Spare {
    void* alloc;
    uint32_t type, size;
    void* pool;
    uint64_t grew;   // order of the allocator's last growth (for the give-back when a request does not fit)
    State state;
    bool cancel;     // given back while being made: destroyed when it is done
};
SRWLOCK g_lock = SRWLOCK_INIT;  // taken inside an allocator's CS by the hook; under it only the residency manager's lock
                                // (ResidencyFreshNew's stamp of held spares), which the game itself takes inside an allocator's CS
Spare g_sp[kMaxSpares];
uint64_t g_growth = 0;
bool g_busy = false;  // under g_lock: our thread is in a game call (making or giving back a spare), outside the lock
bool g_endTimedOut = false;  // under g_lock: the end's wait for our thread ran out once: later deletions do not wait again
HANDLE g_wake = nullptr;

std::atomic<uint64_t> g_taken{0}, g_made{0}, g_madeUs{0}, g_madeMaxUs{0}, g_under{0}, g_underUs{0}, g_underMaxUs{0}, g_big{0},
    g_givenBack{0}, g_noRoom{0}, g_bad{0};
uint64_t g_qpf = 0;
bool g_brokenLogged = false;

uint64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
uint64_t Us(uint64_t ticks) { return g_qpf ? ticks / g_qpf * 1000000 + ticks % g_qpf * 1000000 / g_qpf : 0; }  // no overflow
void Max(std::atomic<uint64_t>& m, uint64_t v)
{
    uint64_t c = m.load(std::memory_order_relaxed);
    while (v > c && !m.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}

template <class T> T& F(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }

// The spare as FRHIMemoryPool::Init left it (our index everywhere it puts one, one free block covering the pool, nothing used).
bool Untouched(void* pool, uint32_t size, uint32_t type)
{
    __try {
        if (*static_cast<const void* const*>(pool) != g_poolVtbl) return false;
        uint8_t* const head = static_cast<uint8_t*>(pool) + kHead;
        if (F<uint16_t>(pool, kPoolIndex) != kSpareIndex || F<uint64_t>(pool, kPoolSize) != size || F<uint64_t>(pool, kFreeSize) != size ||
            F<uint32_t>(pool, kPoolTypes) != type || F<uint64_t>(pool, kWaste) != 0 || F<uint32_t>(pool, kAllocated) != 0 ||
            F<uint64_t>(pool, kLastUsed) != 0 || F<int32_t>(pool, kFreeBlocksNum) != 1)
            return false;
        const uint32_t hb = F<uint32_t>(head, kBlkBits);
        if ((hb & 0xFFFF) != kSpareIndex || ((hb >> 16) & 0xF) != 3) return false;  // the head block (type Head)
        uint8_t* const fb = *F<uint8_t**>(pool, kFreeBlocks);
        if (!fb) return false;
        const uint32_t fbits = F<uint32_t>(fb, kBlkBits);
        if ((fbits & 0xFFFF) != kSpareIndex || ((fbits >> 16) & 0xF) != 1 || F<uint32_t>(fb, kBlkSize) != size || F<uint32_t>(fb, kBlkOffset) != 0)
            return false;  // the free block (type Free) covering the pool from offset 0
        return F<uint8_t*>(head, kBlkNext) == fb && F<uint8_t*>(head, kBlkPrev) == fb && F<uint8_t*>(fb, kBlkNext) == head &&
               F<uint8_t*>(fb, kBlkPrev) == head;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Epic's SetPoolIndex + InitFreeBlocks for a pool made with our index: the game's index where Init put ours.
void Place(void* pool, uint16_t index)
{
    uint8_t* const head = static_cast<uint8_t*>(pool) + kHead;
    uint8_t* const fb = *F<uint8_t**>(pool, kFreeBlocks);
    F<uint16_t>(pool, kPoolIndex) = index;
    F<uint16_t>(head, kBlkBits) = index;
    F<uint16_t>(fb, kBlkBits) = index;
}

void DestroyPool(void* pool)
{
    void* const* const vt = *static_cast<void* const* const*>(pool);
    reinterpret_cast<DestroyFn>(vt[2])(pool);      // FD3D12MemoryPool::Destroy
    reinterpret_cast<DeleteFn>(vt[0])(pool, 1);    // the deleting destructor (the game's clean-up does the same two calls)
}

CreateFn Orig() { return g_testOrig ? g_testOrig : g_orig; }
DeleteFn OrigDtor() { return g_testOrigDtor ? g_testOrigDtor : g_origDtor; }

// Under g_lock: the slot of (alloc, type), or -1.
int FindSlot(const void* alloc, uint32_t type)
{
    for (int i = 0; i < kMaxSpares; ++i)
        if (g_sp[i].state != kFree && g_sp[i].alloc == alloc && g_sp[i].type == type) return i;
    return -1;
}

// Under g_lock: room for `size` more bytes, giving back (cancel) ready spares of allocators that grew longest ago.
bool MakeRoom(uint64_t size, const void* except)
{
    const uint64_t cap = g_cap.load(std::memory_order_relaxed);
    if (size > cap) return false;
    while (g_bytes.load(std::memory_order_relaxed) + size > cap) {
        int old = -1;
        for (int i = 0; i < kMaxSpares; ++i)
            if (g_sp[i].state == kReady && !g_sp[i].cancel && g_sp[i].alloc != except && (old < 0 || g_sp[i].grew < g_sp[old].grew)) old = i;
        if (old < 0) return false;
        g_sp[old].cancel = true;  // our thread destroys it and frees the bytes
        g_bytes.fetch_sub(g_sp[old].size, std::memory_order_relaxed);
        g_sp[old].size = 0;
    }
    return true;
}

void Request(void* alloc, uint32_t type, uint32_t size)
{
    bool wake = false;
    AcquireSRWLockExclusive(&g_lock);
    if (g_ending.load(std::memory_order_relaxed)) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    const uint64_t order = ++g_growth;
    const int at = FindSlot(alloc, type);
    if (at >= 0) g_sp[at].grew = order;
    else if (MakeRoom(size, alloc)) {
        for (int i = 0; i < kMaxSpares; ++i) {
            if (g_sp[i].state != kFree) continue;
            g_sp[i] = {alloc, type, size, nullptr, order, kPending, false};
            g_bytes.fetch_add(size, std::memory_order_relaxed);
            wake = true;
            break;
        }
        if (!wake) g_noRoom.fetch_add(1, std::memory_order_relaxed);
    } else g_noRoom.fetch_add(1, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_lock);
    if (wake && g_wake) SetEvent(g_wake);
}

// The hook: the game's CreateNewPool call (TryAllocateInternal, the allocator's lock held).
void* Hook(void* self, uint16_t index, uint32_t minSize, uint32_t type)
{
    const uint32_t size = F<uint32_t>(self, kDefaultPoolSize);
    const bool video = F<uint32_t>(self, kHeapType) == kHeapDefault;
    const bool on = g_on.load(std::memory_order_relaxed) && !g_broken.load(std::memory_order_relaxed) &&
                    !g_ending.load(std::memory_order_relaxed);
    void* pool = nullptr;
    if (on && video) g_lastGrowth.store(Now(), std::memory_order_relaxed);  // the quiet wait counts from here
    if (on && video && minSize <= size) {
        AcquireSRWLockExclusive(&g_lock);
        const int at = FindSlot(self, type);
        if (at >= 0 && g_sp[at].state == kReady && !g_sp[at].cancel && g_sp[at].size == size) {
            pool = g_sp[at].pool;
            g_sp[at] = {};
            g_bytes.fetch_sub(size, std::memory_order_relaxed);
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (pool && !Untouched(pool, size, type)) {
            // never seen: a layout we do not know. The pool is left alone (not destroyed: we no longer trust it), the key goes off.
            g_bad.fetch_add(1, std::memory_order_relaxed);
            g_broken.store(true, std::memory_order_relaxed);
            pool = nullptr;
        }
    }
    if (pool) {
        Place(pool, index);
        ResidencyFreshTouchPool(pool, false);  // ResidencyFreshNew: the take is a use (resident for its first use)
        g_taken.fetch_add(1, std::memory_order_relaxed);
    } else {
        const uint64_t t0 = Now();
        pool = Orig()(self, index, minSize, type);
        if (on && video) g_lastGrowth.store(Now(), std::memory_order_relaxed);  // the quiet time counts from its end too (F3)
        if (video) {
            const uint64_t us = Us(Now() - t0);
            g_under.fetch_add(1, std::memory_order_relaxed);
            g_underUs.fetch_add(us, std::memory_order_relaxed);
            Max(g_underMaxUs, us);
            if (minSize > size) g_big.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (on && video && !g_broken.load(std::memory_order_relaxed)) Request(self, type, size);  // Epic: after each new pool, one ahead
    return pool;
}

// One round of our thread's work: give back what is cancelled or over the cap, then make one requested spare. True: more to do.
// *retryMs: a spare waits for the quiet time; wake again after this many ms (else unchanged).
bool Work(DWORD* retryMs)
{
    void* toDestroy = nullptr;
    void* alloc = nullptr;
    uint32_t type = 0, size = 0;
    int slot = -1;
    AcquireSRWLockExclusive(&g_lock);
    if (g_ending.load(std::memory_order_relaxed)) {  // the device's teardown: no game call any more
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const bool on = g_on.load(std::memory_order_relaxed) && !g_broken.load(std::memory_order_relaxed);
    const uint64_t cap = on ? g_cap.load(std::memory_order_relaxed) : 0;
    for (int i = 0; i < kMaxSpares && !toDestroy; ++i) {  // off or over the cap: the least recently grown first
        Spare& s = g_sp[i];
        if (s.state == kPending && (s.cancel || !on)) {
            if (!s.cancel) g_bytes.fetch_sub(s.size, std::memory_order_relaxed);
            s = {};
        } else if (s.state == kReady && s.cancel) {
            toDestroy = s.pool;
            s = {};
        }
    }
    while (!toDestroy && g_bytes.load(std::memory_order_relaxed) > cap) {
        int old = -1;
        for (int i = 0; i < kMaxSpares; ++i)
            if ((g_sp[i].state == kReady || g_sp[i].state == kPending) && !g_sp[i].cancel && (old < 0 || g_sp[i].grew < g_sp[old].grew)) old = i;
        if (old < 0) break;  // only spares being made: they are cancelled below when they are done
        g_bytes.fetch_sub(g_sp[old].size, std::memory_order_relaxed);
        if (g_sp[old].state == kReady) toDestroy = g_sp[old].pool;
        g_sp[old] = {};
    }
    bool quiet = true;
    if (!toDestroy) {
        const uint32_t q = g_quietMs.load(std::memory_order_relaxed);
        if (q) {  // GpuPoolSpareQuietMs: not while pools are still being made (the burst's own paging work goes first)
            const uint64_t last = g_lastGrowth.load(std::memory_order_relaxed), now = Now();
            const uint64_t since = now > last ? Us(now - last) / 1000 : 0;  // a newer stamp than now: not quiet
            if (since < q) {
                quiet = false;
                if (retryMs) *retryMs = static_cast<DWORD>(q - since + 1);
            }
        }
    }
    if (!toDestroy && quiet && LoadPeakDrainNow()) {  // LoadPeakDrain: no spare made behind a loading screen near the budget
        quiet = false;
        if (retryMs) *retryMs = 250;
    }
    if (!toDestroy && quiet)
        for (int i = 0; i < kMaxSpares; ++i)
            if (g_sp[i].state == kPending && !g_sp[i].cancel) {
                g_sp[i].state = kMaking;
                slot = i;
                alloc = g_sp[i].alloc;
                type = g_sp[i].type;
                size = g_sp[i].size;
                break;
            }
    if (toDestroy || slot >= 0) g_busy = true;  // the game's end waits until we are out of the game call
    ReleaseSRWLockExclusive(&g_lock);
    if (toDestroy) {
        DestroyPool(toDestroy);
        g_givenBack.fetch_add(1, std::memory_order_relaxed);
        AcquireSRWLockExclusive(&g_lock);
        g_busy = false;
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    if (slot < 0) return false;
    // the game's own CreateNewPool, outside every allocator lock (it reads only the allocator's constant configuration)
    const uint64_t t0 = Now();
    void* const pool = Orig()(alloc, kSpareIndex, 0, type);
    const uint64_t us = Us(Now() - t0);
    g_made.fetch_add(1, std::memory_order_relaxed);
    g_madeUs.fetch_add(us, std::memory_order_relaxed);
    Max(g_madeMaxUs, us);
    const bool fresh = pool && Untouched(pool, size, type);
    bool keep = false;
    AcquireSRWLockExclusive(&g_lock);
    Spare& s = g_sp[slot];
    const bool ending = g_ending.load(std::memory_order_relaxed);
    if (s.state == kMaking && s.alloc == alloc && s.type == type) {
        // at the end the pool is parked as ready whatever else holds: its allocator's deletion destroys it (we call no game code)
        keep = fresh && (ending || (!s.cancel && g_on.load(std::memory_order_relaxed)));
        if (keep) {
            s.pool = pool;
            s.state = kReady;
        } else {
            if (!s.cancel) g_bytes.fetch_sub(s.size, std::memory_order_relaxed);
            s = {};
        }
    }
    // (at the end a pool that lost its slot is left alone: bounded, one, and the process is ending)
    const bool destroy = pool && fresh && !keep && !ending;
    if (!destroy) g_busy = false;  // else only once that game call is done (review2 F2): the end waits for it
    ReleaseSRWLockExclusive(&g_lock);
    if (pool && !fresh) {  // never seen: a layout we do not know: off, and the pool is not touched again (leaked, bounded: one)
        g_bad.fetch_add(1, std::memory_order_relaxed);
        g_broken.store(true, std::memory_order_relaxed);
    } else if (destroy) {
        DestroyPool(pool);
        g_givenBack.fetch_add(1, std::memory_order_relaxed);
        AcquireSRWLockExclusive(&g_lock);
        g_busy = false;
        ReleaseSRWLockExclusive(&g_lock);
    }
    return true;
}

// The allocator's deleting destructor (vtable slot 0): only the D3D12 device's destructor deletes allocators (and the game's end).
void* DtorHook(void* self, uint32_t flags)
{
    // the host is not installed (the destructor slot was not written; never here then) or only half: pass through (review2 N-a).
    // With the host alone (UploadPagePreGrow without GpuPoolSpareAhead) no spare exists: only the end protocol below acts.
    if (!g_hostInstalled && !g_testNoThread) return OrigDtor()(self, flags);
    const uint64_t t0 = Now();
    bool waited = false, timedOut = false;
    AcquireSRWLockExclusive(&g_lock);
    const bool first = !g_ending.exchange(true);
    while (g_busy && !g_endTimedOut) {  // our thread is in a game call: bounded wait until it is out (it then calls none)
        ReleaseSRWLockExclusive(&g_lock);
        waited = true;
        if (Us(Now() - t0) >= static_cast<uint64_t>(g_endWaitMs) * 1000) {
            timedOut = true;
            AcquireSRWLockExclusive(&g_lock);
            g_endTimedOut = true;  // once (review2 F1): the next deletions do not wait 2 s each
            break;
        }
        Sleep(1);
        AcquireSRWLockExclusive(&g_lock);
    }
    const bool stuck = g_busy;  // still in its call (after a time-out): its slot is left as it is
    void* mine[kMaxSpares];
    int n = 0;
    for (Spare& s : g_sp) {
        if (s.state == kFree || s.alloc != self || (stuck && s.state == kMaking)) continue;
        if (s.state == kReady && s.pool) mine[n++] = s.pool;
        g_bytes.fetch_sub(s.size, std::memory_order_relaxed);  // 0 for a cancelled one (its bytes went at the cancel)
        s = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (int i = 0; i < n; ++i) DestroyPool(mine[i]);  // as the allocator's own pools, which its destructor destroys next
    g_givenBack.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    if (first || timedOut)
        Log("[poolspare] the game is ending (a pool allocator is deleted): no more spares; %s",
            timedOut ? "our thread did not leave the game's call within 2 s (left as it is)"
                     : waited ? "waited for our thread to finish a spare" : "our thread was idle");
    return OrigDtor()(self, flags);
}

// ResidencyFreshNew (s089fix): the ready spares count as used now, so the residency manager's trim (grace 1 s at the full budget)
// never evicts one while it waits for the game; under g_lock, so none is taken or destroyed meanwhile. Only with room (near the
// budget not stamped: they may go first in the over-budget loop, as in stock); the residency lock is only tried (a submission's
// paging work may hold it through a GPU wait: then the next wake, 0.5 s later).
void TouchReady()
{
    if (!ResidencyFreshHeldRoom()) return;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_ending.load(std::memory_order_relaxed))
        for (const Spare& s : g_sp)
            if (s.state == kReady && !s.cancel && s.pool) ResidencyFreshTouchPool(s.pool, true);
    ReleaseSRWLockExclusive(&g_lock);
}

DWORD WINAPI Thread(void*)
{
    DWORD wait = 1000;
    for (;;) {
        WaitForSingleObject(g_wake, wait);  // a request, the quiet time, or once a second for the cap (twice with ResidencyFreshNew)
        TouchReady();
        wait = ResidencyFreshActive() ? 500 : 1000;
        while (Work(&wait)) {}
        if (PoolAheadWorkFn extra = g_extraWork.load(std::memory_order_acquire)) {  // UploadPagePreGrow: at most one page per wake, it says when to wake again
            const DWORD w = static_cast<DWORD>(extra());
            if (w < wait) wait = w;
        }
    }
}

struct Found {
    uint8_t* create = nullptr;
    void** slot = nullptr;
    const void* poolVtbl = nullptr;
    uint8_t* poolInit = nullptr;
    uint8_t* tryAlloc = nullptr;
    uint8_t* dtor = nullptr;
};

bool Within(uint8_t* fn, size_t span, const char* pattern, uint8_t** at = nullptr)
{
    const Pattern p = ParsePattern(pattern);
    for (uint8_t* q = fn; q + p.size() <= fn + span; ++q)
        if (MatchesAt(q, p)) {
            if (at) *at = q;
            return true;
        }
    return false;
}

bool Discover(const GameModule& game, Found* f, const char** why)
{
    const auto c = FindAll(game.text, ParsePattern(kCreate));
    if (c.size() != 1) { *why = "CreateNewPool's start not found once"; return false; }
    uint8_t* const fn = c[0];
    uint8_t* vt = nullptr;
    if (!Within(fn, kCreateBody, kCreateNew) || !Within(fn, kCreateBody, kCreateConfig) || !Within(fn, kCreateBody, kCreateVtbl, &vt) ||
        !Within(fn, kCreateBody, kCreateTail)) {
        *why = "CreateNewPool's body (pool size, heap config, vtable, Init call) does not match";
        return false;
    }
    const auto* poolVtbl = reinterpret_cast<uint8_t* const*>(RipTarget(vt, 3, 7));
    if (!game.rdata.Contains(poolVtbl) || !game.rdata.Contains(reinterpret_cast<const uint8_t*>(poolVtbl + 3) - 1) ||
        !game.text.Contains(poolVtbl[0]) || !game.text.Contains(poolVtbl[1]) || !game.text.Contains(poolVtbl[2])) {
        *why = "the memory pool's vtable is not where CreateNewPool puts it";
        return false;
    }
    // FD3D12MemoryPool::Init (vtable +8) ends in FRHIMemoryPool::Init, which puts the pool index where Place writes it
    const auto pi = FindAll(game.text, ParsePattern(kPoolInit));
    if (pi.size() != 1 || !Within(pi[0], kPoolInitBody, kPoolInitHead) || !Within(pi[0], kPoolInitBody, kPoolInitHeadIdx) ||
        !Within(pi[0], kPoolInitBody, kPoolInitFreeIdx) || !Within(pi[0], kPoolInitBody, kPoolInitFreeAdd)) {
        *why = "FRHIMemoryPool::Init (the pool index's places) does not match";
        return false;
    }
    bool calls = false;
    for (uint8_t* q = poolVtbl[1]; q < poolVtbl[1] + 0x500 && !calls; ++q)
        calls = q[0] == 0xE8 && RipTarget(q, 1, 5) == pi[0];
    if (!calls) { *why = "the pool's Init does not call FRHIMemoryPool::Init"; return false; }
    // the allocator class's vtable slot that holds CreateNewPool: exactly one qword in the exe's read-only data
    void** slot = nullptr;
    int n = 0;
    for (uint8_t* q = game.rdata.begin; q + 8 <= game.rdata.end; q += 8)
        if (*reinterpret_cast<uint8_t**>(q) == fn) slot = reinterpret_cast<void**>(q), ++n;
    if (n != 1) { *why = "CreateNewPool's vtable slot not found once"; return false; }
    if (!game.text.Contains(static_cast<uint8_t*>(slot[-1])) || !game.text.Contains(static_cast<uint8_t*>(slot[1]))) {
        *why = "CreateNewPool's vtable neighbours are not code";
        return false;
    }
    // slot 0 of the same vtable: the allocator's deleting destructor (it stores this vtable, then deletes when flags & 1)
    uint8_t* const dtor = static_cast<uint8_t*>(slot[-1]);
    if (!MatchesAt(dtor, ParsePattern(kAllocDtor)) || RipTarget(dtor + 10, 3, 7) != reinterpret_cast<uint8_t*>(slot - 1) ||
        !Within(dtor, kAllocDtorBody, kAllocDtorDelete)) {
        *why = "the pool allocator's destructor (vtable slot 0) does not match";
        return false;
    }
    // its caller: TryAllocateInternal, the allocator's lock (+28h) taken, two calls through vtable +8 with the index in dx
    const auto ta = FindAll(game.text, ParsePattern(kTryAlloc));
    if (ta.size() != 1 || !Within(ta[0], kTryAllocBody, kTryAllocCall1) || !Within(ta[0], kTryAllocBody, kTryAllocCall2)) {
        *why = "TryAllocateInternal (lock, CreateNewPool calls) does not match";
        return false;
    }
    f->create = fn;
    f->slot = slot;
    f->poolVtbl = poolVtbl;
    f->poolInit = pi[0];
    f->tryAlloc = ta[0];
    f->dtor = dtor;
    return true;
}

}  // namespace

namespace {
Found g_found;  // the host's discovery (InstallPoolAheadHost), reused by InstallGpuPoolSpare
}  // namespace

// The OPF-PoolAhead thread and the pool allocator destructor's end hook (the game's end protocol below): GpuPoolSpareAhead's host,
// and UploadPagePreGrow's (s083build) when GpuPoolSpareAhead is off. Start-up only.
bool InstallPoolAheadHost(bool beforeGameCode, const char* who)
{
    if (g_hostInstalled) return true;
    if (!beforeGameCode) { Log("[poolspare] %s: only installed while the game starts", who); return false; }
    const GameModule& game = Game();
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) { Log("[poolspare] %s: %s: not installed", who, why); return false; }
    if (*f.slot != f.create) { Log("[poolspare] %s: CreateNewPool's vtable slot changed: not installed", who); return false; }
    LARGE_INTEGER q;
    QueryPerformanceFrequency(&q);
    g_qpf = static_cast<uint64_t>(q.QuadPart);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake) { Log("[poolspare] %s: no event: not installed", who); return false; }
    g_orig = reinterpret_cast<CreateFn>(f.create);
    g_origDtor = reinterpret_cast<DeleteFn>(f.dtor);
    g_poolVtbl = f.poolVtbl;
    HANDLE th = CreateThread(nullptr, 0, &Thread, nullptr, 0, nullptr);
    if (!th) { Log("[poolspare] %s: no thread: not installed", who); return false; }
    DescribeThread(th, L"OPF-PoolAhead");
    CloseHandle(th);
    // the destructor first: once CreateNewPool is ours, every allocator that can hold a spare is deleted through our hook
    if (!WritePointer(f.slot - 1, reinterpret_cast<void*>(&DtorHook))) {
        Log("[poolspare] %s: could not write the allocator destructor's vtable slot: not installed (our thread stays idle)", who);
        return false;  // the thread waits for requests that never come
    }
    g_found = f;
    g_hostInstalled = true;
    return true;
}

void SetPoolAheadWork(PoolAheadWorkFn work)
{
    g_extraWork.store(work, std::memory_order_release);
    if (g_wake && !g_testNoThread) SetEvent(g_wake);
}

void PoolAheadWake()
{
    if (g_wake && !g_testNoThread) SetEvent(g_wake);
}

// Our thread is about to call game code (outside every lock): false = the game's end has begun, call nothing.
bool PoolAheadEnterGame()
{
    AcquireSRWLockExclusive(&g_lock);
    const bool ok = !g_ending.load(std::memory_order_relaxed);
    if (ok) g_busy = true;  // the end's first allocator deletion waits (bounded) until PoolAheadLeaveGame
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

void PoolAheadLeaveGame()
{
    AcquireSRWLockExclusive(&g_lock);
    g_busy = false;
    ReleaseSRWLockExclusive(&g_lock);
}

bool PoolAheadEnding() { return g_ending.load(std::memory_order_relaxed); }

bool InstallGpuPoolSpare(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!InstallPoolAheadHost(beforeGameCode, "GpuPoolSpareAhead")) return false;
    const GameModule& game = Game();
    const Found& f = g_found;
    if (!WritePointer(f.slot, reinterpret_cast<void*>(&Hook))) {
        Log("[poolspare] GpuPoolSpareAhead: could not write CreateNewPool's vtable slot: not installed (our thread stays idle)");
        return false;  // the destructor hook only runs the end protocol (nothing is ever requested)
    }
    g_installed = true;
    Log("[poolspare] GpuPoolSpareAhead installed: CreateNewPool exe+0x%X (vtable slot exe+0x%X), the allocator's destructor exe+0x%X, "
        "FRHIMemoryPool::Init exe+0x%X, TryAllocateInternal exe+0x%X; spare pools made ahead on OPF-PoolAhead",
        static_cast<unsigned>(f.create - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(f.slot) - game.base),
        static_cast<unsigned>(f.dtor - game.base), static_cast<unsigned>(f.poolInit - game.base),
        static_cast<unsigned>(f.tryAlloc - game.base));
    return true;
}

void SetGpuPoolSpare(bool on, int keepBudgetMB, int quietMs)
{
    const uint64_t cap = keepBudgetMB > 0 ? (static_cast<uint64_t>(keepBudgetMB) << 20) / 2 : 0;
    g_quietMs.store(quietMs > 0 ? static_cast<uint32_t>(quietMs) : 0, std::memory_order_relaxed);
    const bool was = g_on.exchange(on);
    const uint64_t oldCap = g_cap.exchange(cap);
    if (!g_installed) return;
    if (was != on) Log(on ? "[poolspare] spare pools made ahead (up to half of GpuPoolKeepMB's budget)" : "[poolspare] spare pools given back");
    if ((cap < oldCap || !on) && g_wake && !g_testNoThread) SetEvent(g_wake);
}

uint64_t GpuPoolSpareBytes() { return g_bytes.load(std::memory_order_relaxed); }

void LogGpuPoolSpareStats()
{
    if (!g_installed) return;
    if (g_broken.load() && !g_brokenLogged) {
        g_brokenLogged = true;
        Log("[poolspare] GpuPoolSpareAhead: a spare pool did not look as the game's Init leaves it (%llu): off until the game restarts",
            static_cast<unsigned long long>(g_bad.load()));
    }
    const uint64_t taken = g_taken.exchange(0), made = g_made.exchange(0), madeUs = g_madeUs.exchange(0), madeMax = g_madeMaxUs.exchange(0),
                   under = g_under.exchange(0), underUs = g_underUs.exchange(0), underMax = g_underMaxUs.exchange(0), big = g_big.exchange(0),
                   back = g_givenBack.exchange(0), noRoom = g_noRoom.exchange(0);
    if (!taken && !made && !under && !back && !noRoom) return;
    int ready = 0;
    AcquireSRWLockShared(&g_lock);
    for (const Spare& s : g_sp) ready += s.state == kReady && !s.cancel;
    ReleaseSRWLockShared(&g_lock);
    Log("[poolspare] %llu new pools taken from a spare (no wait under the lock); %llu made under the lock as the game does, "
        "%.1f ms waited there (longest %.1f; %llu larger than the default size); %llu spares made ahead, %.1f ms on our thread "
        "(longest %.1f); %d ready, %.0f MB (cap %.0f MB); %llu given back; %llu requests without room",
        static_cast<unsigned long long>(taken), static_cast<unsigned long long>(under), underUs / 1000.0, underMax / 1000.0,
        static_cast<unsigned long long>(big), static_cast<unsigned long long>(made), madeUs / 1000.0, madeMax / 1000.0, ready,
        g_bytes.load() / 1048576.0, g_cap.load() / 1048576.0, static_cast<unsigned long long>(back), static_cast<unsigned long long>(noRoom));
}

// ---- self-test ----
bool GpuPoolSpareTestDiscover(const GameModule& game, uint32_t* create, uint32_t* slot, uint32_t* poolVtbl, uint32_t* poolInit,
                              uint32_t* tryAlloc, uint32_t* dtor)
{
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) { printf("    poolspare discovery: %s\n", why); return false; }
    *create = static_cast<uint32_t>(f.create - game.base);
    *slot = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(f.slot) - game.base);
    *poolVtbl = static_cast<uint32_t>(static_cast<const uint8_t*>(f.poolVtbl) - game.base);
    *poolInit = static_cast<uint32_t>(f.poolInit - game.base);
    *tryAlloc = static_cast<uint32_t>(f.tryAlloc - game.base);
    *dtor = static_cast<uint32_t>(f.dtor - game.base);
    return true;
}

// Fake allocators and pools: `orig` builds a pool as FRHIMemoryPool::Init does; `vtbl` is the fake pool vtable.
void GpuPoolSpareTestSetup(void* orig, const void* vtbl, void* origDtor)
{
    AcquireSRWLockExclusive(&g_lock);
    for (Spare& s : g_sp) s = {};
    g_growth = 0;
    g_busy = false;
    ReleaseSRWLockExclusive(&g_lock);
    g_endTimedOut = false;
    g_ending = false;
    g_quietMs = 0;
    g_lastGrowth = 0;
    g_endWaitMs = 50;
    g_testOrigDtor = reinterpret_cast<DeleteFn>(origDtor);
    g_testOrig = reinterpret_cast<CreateFn>(orig);
    g_poolVtbl = vtbl;
    g_testNoThread = true;
    g_qpf = 10000000;
    g_bytes = 0;
    g_broken = false;
    g_taken = 0; g_made = 0; g_under = 0; g_big = 0; g_givenBack = 0; g_noRoom = 0; g_bad = 0;
}
void* GpuPoolSpareTestCall(void* allocator, uint16_t index, uint32_t minSize, uint32_t type) { return Hook(allocator, index, minSize, type); }
bool GpuPoolSpareTestWork(uint32_t* retryMs)
{
    DWORD r = 0;
    const bool more = Work(&r);
    if (retryMs) *retryMs = r;
    return more;
}
void* GpuPoolSpareTestDtor(void* allocator, uint32_t flags) { return DtorHook(allocator, flags); }
void GpuPoolSpareTestBusy(bool busy)
{
    AcquireSRWLockExclusive(&g_lock);
    g_busy = busy;
    ReleaseSRWLockExclusive(&g_lock);
}
bool GpuPoolSpareTestEnding() { return g_ending.load(); }
bool GpuPoolSpareTestEndTimedOut()
{
    AcquireSRWLockShared(&g_lock);
    const bool t = g_endTimedOut;
    ReleaseSRWLockShared(&g_lock);
    return t;
}
void GpuPoolSpareTestEndWait(uint32_t ms) { g_endWaitMs = ms; }
bool GpuPoolSpareTestUntouched(void* pool, uint32_t size, uint32_t type) { return Untouched(pool, size, type); }
void GpuPoolSpareTestCounts(uint64_t* taken, uint64_t* made, uint64_t* under, uint64_t* back, uint64_t* noRoom, bool* broken)
{
    *taken = g_taken.load(); *made = g_made.load(); *under = g_under.load(); *back = g_givenBack.load(); *noRoom = g_noRoom.load();
    *broken = g_broken.load();
}
int GpuPoolSpareTestReady(const void* allocator)
{
    int n = 0;
    AcquireSRWLockShared(&g_lock);
    for (const Spare& s : g_sp) n += s.state == kReady && !s.cancel && (!allocator || s.alloc == allocator);
    ReleaseSRWLockShared(&g_lock);
    return n;
}
void GpuPoolSpareTestEnd()
{
    while (Work(nullptr)) {}
    g_testOrig = nullptr;
    g_testOrigDtor = nullptr;
    g_ending = false;
    g_quietMs = 0;
    g_endWaitMs = kEndWaitMs;
    g_testNoThread = false;
    g_on = false;
    g_cap = 0;
}

}  // namespace opf
