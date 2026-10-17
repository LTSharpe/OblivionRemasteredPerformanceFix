// CommandAllocatorTrim: the D3D12 RHI's command allocators stop keeping their peak for the session.
//
// Why (2026-10-12): every D3D12 command list is recorded into a command allocator, and the driver keeps the memory it
// gave an allocator (on AMD at least one 1 MiB command chunk, often several, plus a 256 KiB embedded-data chunk, all
// video memory) until the allocator is destroyed; ID3D12CommandAllocator::Reset only makes it reusable. UE 5.3 takes an
// allocator from a per-queue pool for every command list (FD3D12Device::ObtainCommandAllocator), hands it back to the
// pool once the GPU has finished with it (ReleaseCommandAllocator, from the payload's destructor on the RHI interrupt
// thread) and never destroys any. So each queue's pool is as large as the most command lists that were ever in flight
// at once. Loading an area creates hundreds of them (every buffer and texture created with initial data gets its own copy
// context, allocator and submission: D3D12Buffer.cpp / D3D12Texture.cpp FD3D12CopyScope), and parallel translate (RdgParallelTranslate)
// adds two direct-queue lists per render-graph pass set. Measured on the cpu profile: 147 allocators after the first
// load, 607 after three teleports (about 450 of them copy allocators from the loads), none ever given back. Each holds about
// 1 MiB of video memory: trimming 444 copy and 42 direct allocators freed 0.50 GB (per-second budget log, 2026-10-12).
//
// The fix gives spare copy allocators back when video memory gets tight. The obtain and release functions are detoured
// to count the allocators in flight per queue and their peak over the last AllocatorTrimWindowSeconds (20); the
// constructor to count the real ones. Once a second the monitor thread checks the copy pool when the game is within
// AllocatorTrimHeadroomMB (2048; 0 = always) of its video memory budget, and with a 3-second window once it has been over
// the budget for 10 s in a row (arrival peaks pass). The direct and compute pools always keep their peak, as in the game:
// their need returns with every busy scene (trimming them made the engine create ~300 again in 5 minutes on a roomy card,
// ~960 in 6 minutes over budget, where each new one took fresh chunks from a paging driver and the scenes ran slower).
// A copy pool with a clear surplus over its window peak (plus max(8, peak/8)) is emptied with the engine's own lock-free
// pop, the surplus wrappers (at most 32 a second) get an inert stand-in in place of their D3D12 allocator, every wrapper goes back
// (stand-ins at the bottom), and the real allocators are released four at a time with 25 ms pauses, which frees their
// memory in the driver (one run of 418 releases held up the game's submissions: frames of 174/79/158 ms at 1440p; four at
// a time: frames inside the release windows at most 37 ms, median 16, and the scenes' worst frames 19-73 ms instead of
// 31-224 ms). A wrapper that comes out of the pool holding the stand-in is given a new D3D12 allocator by the engine's own
// constructor before the engine sees it, exactly what the engine does when its pool is
// empty (measured: ~0.03 ms each). Nothing that is recorded or submitted changes; only when allocators are created and
// destroyed. Switched live from the ini.
//
// Part 2 (ImmediateAllocatorLists, hidden, 0 = off): the immediate context, which the RHI thread finalizes before every
// parallel-translated pass set (FRHICommandListImmediate::QueueAsyncCommandListSubmit -> ExecuteAndReset ->
// RHIFinalizeContext), keeps its command allocator for that many command lists instead of taking a new one each time
// (FD3D12ContextCommon::Finalize hands it to the payload; OpenCommandList takes a new one when it has none). D3D12 allows
// recording a new list into an allocator whose earlier lists are still executing; it only must not be reset before they
// are done. The immediate context's lists are finalized and submitted in order on one queue, so the allocator, handed to
// the payload of its last list, is reset only after all of them have finished. Parallel-translate contexts are not
// touched: their lists are submitted in the render graph's order, not in the order they were recorded.
//
// Found by shape (build 19115871 addresses in the comments):
//  - ObtainCommandAllocator (exe+0x2527730): queues array (inline or heap), stride 0x300, pool at +0x240, the lock-free
//    pop (exe+0xDE3320), link blocks (exe+0x8FFF980, 24-byte links, payload at +8), link free (exe+0xDE12C0), then
//    FMemory::Malloc(0x18) and a tail jump to the constructor;
//  - the constructor (exe+0x24ED9D0): [this] = device, [this+8] = queue type, [this+0x10] = the ID3D12CommandAllocator;
//  - RHIFinalizeContext's single-GPU branch (exe+0x254DBC2): add rbx,-0xE8 (to FD3D12CommandContext), call
//    FD3D12ContextCommon::Finalize (exe+0x24F8570), cmp byte [rbx+0x14],0 (the default-context flag); in Finalize the
//    allocator member (+0xB0) handed to the Signal payload, and the payload's device/queue type (+8 / +0x10);
//  - ReleaseCommandAllocator (exe+0x252E2E0): the Reset helper (exe+0x2505EC0: [this+0x10]->Reset, vtable +0x40), then
//    the same queue/pool arithmetic and the push.
#include "common.h"

#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace opf {
namespace {

constexpr char kObtain[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 81 ?? ?? ?? ?? 4C 8D 89 ?? ?? ?? ?? 48 8B F1 48 63 FA 48 85 C0 "
    "4C 0F 45 C8 48 8D 0C 7F 48 C1 E1 08 48 81 C1 ?? ?? ?? ?? 49 03 C9 E8 ?? ?? ?? ?? 33 DB 85 C0 74 ?? 8B D0 "
    "4C 8D 05 ?? ?? ?? ?? 8B C8 81 E2 FF 3F 00 00 48 C1 E9 0E 48 8D 14 52 49 8B 0C C8 48 8B 5C D1 08 8B C8 E8 ?? ?? ?? ?? "
    "48 85 DB 75 ?? 8D 4B 18 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 44 8B C7 48 8B D6 48 8B C8 48 8B 5C 24 30 48 8B 74 24 38 "
    "48 83 C4 20 5F E9";
constexpr size_t kObtainQueuesData = 18, kObtainQueuesInline = 25, kObtainPool = 53, kObtainPop = 60, kObtainLinks = 73,
                 kObtainFreeLink = 107, kObtainCtor = 154;
constexpr char kRelease[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B CA E8 ?? ?? ?? ?? 48 8B 83 ?? ?? ?? ?? 48 81 C3 ?? ?? ?? ?? "
    "48 85 C0 48 0F 45 D8 48 63 47 08 48 8D 0C 40 48 C1 E1 08 48 03 D9 E8 ?? ?? ?? ?? 44 8B C0 85 C0 75 ?? 33 C9 EB ?? "
    "49 8B D0 48 8D 0D ?? ?? ?? ?? 49 8B C0 48 C1 E8 0E 81 E2 FF 3F 00 00 48 8B 04 C1 48 8D 14 52 48 8D 0C D0 "
    "48 89 79 08 41 8B D0 48 8D 8B ?? ?? ?? ?? 48 8B 5C 24 30 48 83 C4 20 5F E9";
constexpr size_t kReleaseReset = 19, kReleaseQueuesData = 27, kReleaseQueuesInline = 34, kReleaseLinks = 79,
                 kReleasePool = 121;
// The Reset helper: sub rsp,48h; mov rcx,[rcx+10h]; mov rax,[rcx]; call [rax+40h]; test eax,eax
constexpr char kReset[] = "48 83 EC 48 48 8B 49 10 48 8B 01 FF 50 40 85 C0";
// The constructor: ... mov ebx,r8d; mov [rcx],rdx; mov [rcx+8],ebx; mov rdi,rcx; xor r15d,r15d; mov [rcx+10h],r15
constexpr char kCtor[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 41 8B D8 48 89 11 89 59 08 48 8B F9 "
    "45 33 FF 4C 89 79 10";
constexpr char kFinalizeCall[] =
    "48 81 C3 ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B CB E8 ?? ?? ?? ?? 44 38 73 ?? 75 ?? BA 01 00 00 00 48 8B CB E8";
constexpr size_t kFinalizeCallAt = 15, kDefaultFlagAt = 23;
constexpr char kFinalize[] = "40 53 56 41 57 48 83 EC 20 48 83 B9 ?? ?? ?? ?? 00 4C 8B FA 48 89 7C 24 ?? 48 8B F9";
// cmp qword [rdi+Allocator],0; je; mov edx,2 (EPhase::Signal); mov rcx,rdi; call GetPayload
constexpr char kFinalizeAllocator[] = "48 83 BF ?? ?? ?? ?? 00 74 ?? BA 02 00 00 00 48 8B CF E8";
// new FD3D12Payload(Device, QueueType): mov r8d,[rdi+QueueType]; mov rcx,rax; mov rdx,[rdi+Device]
constexpr char kFinalizeQueueType[] = "44 8B 47 ?? 48 8B C8 48 8B 57 ??";
constexpr uint8_t kFinalizePrologue[5] = {0x40, 0x53, 0x56, 0x41, 0x57};  // push rbx; push rsi; push r15
constexpr uint8_t kPrologue[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};  // mov [rsp+8],rbx (all three functions)
constexpr int kQueues = 3;            // ED3D12QueueType: Direct, Copy, Async
constexpr size_t kAllocD3d = 0x10;    // FD3D12CommandAllocator: Device, QueueType, TRefCountPtr<ID3D12CommandAllocator>
constexpr size_t kAllocType = 0x08;
constexpr int kMaxPop = 4096;
constexpr int kCopyQueue = 1;
constexpr int kMaxTrimPerTick = 32;  // ~1 MiB and ~1 ms of driver time each (2026-10-12): spread a big pool over seconds
constexpr int kReleaseGroup = 4;      // released together, then a pause
constexpr int kReleasePauseMs = 25;
constexpr int kPressureSeconds = 10;  // over (or back under) the budget this long in a row before the mode changes
constexpr int kBuckets = 64;          // one-second maxima of the allocators in flight

using ObtainFn = void* (*)(void* device, int queueType);
using ReleaseFn = void (*)(void* device, void* allocator);
using CtorFn = void* (*)(void* self, void* device, int queueType);
using PopFn = uint32_t (*)(void* root);
using FinalizeFn = void (*)(void* context, void* payloads);
using FreeLinkFn = void (*)(uint32_t link);

// ---- the stand-in for a released D3D12 allocator: every method a no-op, Reset succeeds ----
HRESULT STDMETHODCALLTYPE StubQueryInterface(void*, REFIID, void** out)
{
    if (out) *out = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE StubRefCount(void*) { return 1; }
HRESULT STDMETHODCALLTYPE StubNotImpl(void*) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE StubReset(void*) { return S_OK; }
void* g_stubVtable[16] = {
    reinterpret_cast<void*>(&StubQueryInterface), reinterpret_cast<void*>(&StubRefCount),
    reinterpret_cast<void*>(&StubRefCount),       reinterpret_cast<void*>(&StubNotImpl),  // GetPrivateData
    reinterpret_cast<void*>(&StubNotImpl),        reinterpret_cast<void*>(&StubNotImpl),  // SetPrivateData(Interface)
    reinterpret_cast<void*>(&StubNotImpl),        reinterpret_cast<void*>(&StubNotImpl),  // SetName, GetDevice
    reinterpret_cast<void*>(&StubReset),          reinterpret_cast<void*>(&StubNotImpl),
    reinterpret_cast<void*>(&StubNotImpl),        reinterpret_cast<void*>(&StubNotImpl),
    reinterpret_cast<void*>(&StubNotImpl),        reinterpret_cast<void*>(&StubNotImpl),
    reinterpret_cast<void*>(&StubNotImpl),        reinterpret_cast<void*>(&StubNotImpl)};
struct Stub {
    void** vtable = g_stubVtable;
};
Stub g_stub;

struct QueueStats {
    std::atomic<int64_t> inFlight{0};
    std::atomic<int64_t> created{0};    // real D3D12 allocators made by the constructor (the game's and refills)
    std::atomic<int64_t> trimmed{0};    // real allocators swapped for the stand-in and released
    std::atomic<int64_t> refilled{0};   // stand-ins given a new allocator on their way out of the pool
    std::atomic<int64_t> bucket[kBuckets];
    int64_t lastLogCreated = 0, lastLogTrimmed = 0, lastLogRefilled = 0;
};

struct State {
    uint8_t *obtain = nullptr, *release = nullptr, *ctor = nullptr;
    PopFn pop = nullptr;
    FreeLinkFn freeLink = nullptr;
    uint8_t** linkBlocks = nullptr;
    uint32_t queuesData = 0, queuesInline = 0, pool = 0;
    ObtainFn obtainOrig = nullptr;
    ReleaseFn releaseOrig = nullptr;
    CtorFn ctorOrig = nullptr;
    uint8_t* finalize = nullptr;
    FinalizeFn finalizeOrig = nullptr;
    uint32_t allocOff = 0, queueTypeOff = 0, defaultFlagOff = 0;
    bool finalizeFound = false, finalizeInstalled = false;
    uint8_t* region = nullptr;
    bool found = false, installed = false;
};
State g;
QueueStats g_q[kQueues];
std::atomic<void*> g_device{nullptr};
std::atomic<ULONGLONG> g_deviceSinceMs{0};
std::atomic<int> g_bucketIndex{0};
std::atomic<bool> g_enabled{false};
std::atomic<int> g_windowSeconds{20};
std::atomic<int> g_pressure{0};   // 1: VRAM over budget (MemoryRelief): short window, small margin
std::atomic<bool> g_tight{false};   // video memory within AllocatorTrimHeadroomMB of the budget (always true when that is 0)
std::atomic<ULONGLONG> g_lastObtainMs{0};
std::atomic<int64_t> g_trimPasses{0};
double g_longestReleaseMs = 0, g_trimmedMB = 0;
std::atomic<int> g_immediateLists{0};
int g_immediateUses = 0;                 // the thread that finalizes the immediate context (the RHI thread)
std::atomic<int64_t> g_immediateKept{0}, g_immediateHanded{0};

int QueueOf(void* allocator)
{
    const int q = *reinterpret_cast<int*>(static_cast<uint8_t*>(allocator) + kAllocType);
    return q >= 0 && q < kQueues ? q : -1;
}
void*& D3dOf(void* allocator) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(allocator) + kAllocD3d); }

void NoteInFlight(QueueStats& s, int64_t now)
{
    auto& b = s.bucket[g_bucketIndex.load(std::memory_order_relaxed) % kBuckets];
    int64_t cur = b.load(std::memory_order_relaxed);
    while (now > cur && !b.compare_exchange_weak(cur, now, std::memory_order_relaxed)) {
    }
}

std::atomic<int64_t> g_createUs{0}, g_createMaxUs{0};  // time spent creating allocators (the engine's and refills)


void* CtorHook(void* self, void* device, int queueType)
{
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceCounter(&t0);
    void* r = g.ctorOrig(self, device, queueType);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const int64_t us = static_cast<int64_t>(QpcToMicroseconds(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), static_cast<uint64_t>(f.QuadPart)));
    g_createUs.fetch_add(us, std::memory_order_relaxed);
    int64_t cur = g_createMaxUs.load(std::memory_order_relaxed);
    while (us > cur && !g_createMaxUs.compare_exchange_weak(cur, us, std::memory_order_relaxed)) {
    }
    if (queueType >= 0 && queueType < kQueues) g_q[queueType].created.fetch_add(1, std::memory_order_relaxed);
    return r;
}

void* ObtainHook(void* device, int queueType)
{
    void* a = g.obtainOrig(device, queueType);
    if (!a) return a;
    if (D3dOf(a) == &g_stub) {
        // A wrapper whose allocator was trimmed: give it a new one the way the engine makes one (its constructor creates
        // the D3D12 allocator, with the engine's own error handling; it drops the stand-in through TRefCountPtr).
        CtorHook(a, device, queueType);
        if (queueType >= 0 && queueType < kQueues) g_q[queueType].refilled.fetch_add(1, std::memory_order_relaxed);
    }
    if (queueType >= 0 && queueType < kQueues) {
        if (g_device.load(std::memory_order_relaxed) != device) {
            g_device.store(device);
            g_deviceSinceMs.store(GetTickCount64());
        }
        auto& s = g_q[queueType];
        const int64_t inFlight = s.inFlight.fetch_add(1, std::memory_order_relaxed) + 1;
        NoteInFlight(s, inFlight);
    }
    g_lastObtainMs.store(GetTickCount64(), std::memory_order_relaxed);
    return a;
}

void ReleaseHook(void* device, void* allocator)
{
    const int q = QueueOf(allocator);
    if (q >= 0) g_q[q].inFlight.fetch_sub(1, std::memory_order_relaxed);
    g.releaseOrig(device, allocator);
}

// FD3D12ContextCommon::Finalize. The immediate (default) direct-queue context keeps its allocator for up to
// ImmediateAllocatorLists lists: hidden from Finalize (which then hands nothing over), put back afterwards so that the
// next OpenCommandList records into it.
void FinalizeHook(void* context, void* payloads)
{
    auto* c = static_cast<uint8_t*>(context);
    const int lists = g_immediateLists.load(std::memory_order_relaxed);
    if (lists > 1 && c[g.defaultFlagOff] && *reinterpret_cast<int32_t*>(c + g.queueTypeOff) == 0) {
        void*& allocator = *reinterpret_cast<void**>(c + g.allocOff);
        if (allocator && ++g_immediateUses < lists) {
            void* const keep = allocator;
            allocator = nullptr;
            g.finalizeOrig(context, payloads);
            allocator = keep;
            g_immediateKept.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (allocator) g_immediateHanded.fetch_add(1, std::memory_order_relaxed);
        g_immediateUses = 0;
    }
    g.finalizeOrig(context, payloads);
}

bool Fail(const char* what)
{
    Log("[alloc] command allocator trim: %s: unavailable", what);
    return false;
}

// A 5-byte entry detour (the first instruction is the 5-byte mov [rsp+8],rbx): trampoline at region+slot*64, stub at
// region+slot*64+32; the jump is written as one atomic qword store when the 5 bytes share an aligned qword.
bool DetourWith(uint8_t* fn, const uint8_t (&prologue)[5], void* hook, int slot, void** trampolineOut, bool beforeGameCode)
{
    if (memcmp(fn, prologue, sizeof prologue) != 0) return false;
    uint8_t* const trampoline = g.region + slot * 64;
    uint8_t* const stub = trampoline + 32;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + sizeof prologue;
    DWORD old;
    VirtualProtect(g.region, 0x1000, PAGE_READWRITE, &old);
    memcpy(trampoline, prologue, sizeof prologue);
    memcpy(trampoline + sizeof prologue, jmpAbs, sizeof jmpAbs);
    memcpy(trampoline + sizeof prologue + sizeof jmpAbs, &resume, sizeof resume);
    memcpy(stub, jmpAbs, sizeof jmpAbs);
    memcpy(stub + sizeof jmpAbs, &hook, sizeof hook);
    VirtualProtect(g.region, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), g.region, 0x1000);
    *trampolineOut = trampoline;
    const int64_t rel = stub - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t jmp[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(jmp + 1, &rel32, sizeof rel32);
    const uintptr_t at = reinterpret_cast<uintptr_t>(fn);
    if ((at & 7) <= 3) {
        auto* q = reinterpret_cast<volatile LONG64*>(at & ~uintptr_t{7});
        if (!VirtualProtect(const_cast<LONG64*>(q), 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        const LONG64 before = *q;
        LONG64 after = before;
        memcpy(reinterpret_cast<uint8_t*>(&after) + (at & 7), jmp, sizeof jmp);
        const bool ok = InterlockedCompareExchange64(q, after, before) == before;
        VirtualProtect(const_cast<LONG64*>(q), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(q), 8);
        return ok;
    }
    return beforeGameCode && WriteCode(fn, jmp);
}

bool Detour(uint8_t* fn, void* hook, int slot, void** trampolineOut, bool beforeGameCode)
{
    return DetourWith(fn, kPrologue, hook, slot, trampolineOut, beforeGameCode);
}

uint8_t* PoolOf(void* device, int q)
{
    auto* d = static_cast<uint8_t*>(device);
    uint8_t* queues = *reinterpret_cast<uint8_t**>(d + g.queuesData);
    if (!queues) queues = d + g.queuesInline;
    return queues + static_cast<size_t>(q) * 0x300 + g.pool;
}

void* PopFree(uint8_t* pool)
{
    const uint32_t link = g.pop(pool);
    if (!link) return nullptr;
    uint8_t* const block = g.linkBlocks[link >> 14];
    void* const item = *reinterpret_cast<void**>(block + static_cast<size_t>(link & 0x3FFF) * 24 + 8);
    g.freeLink(link);
    return item;
}

int64_t WindowPeak(const QueueStats& s, int seconds)
{
    const int now = g_bucketIndex.load();
    int64_t peak = s.inFlight.load();
    for (int i = 0; i < seconds && i < kBuckets; ++i) peak = (std::max)(peak, s.bucket[(now - i + kBuckets) % kBuckets].load());
    return peak;
}

struct TrimResult {
    int popped = 0, real = 0, swapped = 0;
    double releaseMs = 0, longestOneMs = 0;
};

// Monitor thread. SEH-only (no C++ objects with destructors): an exception here must not take the game down.
int TrimQueue(void* device, int q, int64_t keepReal, int maxSwap, void** held, void** toRelease, TrimResult* r)
{
    __try {
        uint8_t* const pool = PoolOf(device, q);
        int n = 0;
        while (n < kMaxPop) {
            void* a = PopFree(pool);
            if (!a) break;
            held[n++] = a;
        }
        r->popped = n;
        // Most recently used first (the pool is a stack): keep the first keepReal real ones.
        int swapped = 0;
        for (int i = 0; i < n; ++i) {
            void*& d3d = D3dOf(held[i]);
            if (d3d == &g_stub) continue;
            if (++r->real <= keepReal || swapped >= maxSwap) continue;
            toRelease[swapped++] = d3d;
            d3d = &g_stub;
        }
        r->swapped = swapped;
        // Back into the pool, stand-ins first so that the real ones are on top (taken next). The engine's own release
        // resets each (a no-op for the stand-in; the real ones were reset when they came back from the GPU).
        for (int pass = 0; pass < 2; ++pass)
            for (int i = n - 1; i >= 0; --i)
                if ((D3dOf(held[i]) == &g_stub) == (pass == 0)) g.releaseOrig(device, held[i]);
        // Released a few at a time with pauses in between: the driver frees an allocator's chunks in the kernel (about
        // 1 ms each, 20 ms for a large direct one), and a long run of frees held up the game's own GPU submissions (418 in
        // one go: frames of 174, 79 and 158 ms at 1440p, 2026-10-12).
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        for (int i = 0; i < swapped; ++i) {
            if (i && i % kReleaseGroup == 0) Sleep(kReleasePauseMs);
            QueryPerformanceCounter(&t0);
            static_cast<IUnknown*>(toRelease[i])->Release();
            QueryPerformanceCounter(&t1);
            const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
            r->releaseMs += ms;
            if (ms > r->longestOneMs) r->longestOneMs = ms;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void* g_held[kMaxPop];
void* g_toRelease[kMaxPop];

}  // namespace

bool FindAllocatorTrim()
{
    if (g.found) return true;
    const GameModule& game = Game();
    const auto obtains = FindAll(game.text, ParsePattern(kObtain));
    if (obtains.size() != 1) return Fail("ObtainCommandAllocator not found exactly once");
    const auto releases = FindAll(game.text, ParsePattern(kRelease));
    if (releases.size() != 1) return Fail("ReleaseCommandAllocator not found exactly once");
    uint8_t* const o = obtains[0];
    uint8_t* const r = releases[0];
    auto u32 = [](const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; };
    g.queuesData = u32(o + kObtainQueuesData);
    g.queuesInline = u32(o + kObtainQueuesInline);
    g.pool = u32(o + kObtainPool);
    if (u32(r + kReleaseQueuesData) != g.queuesData || u32(r + kReleaseQueuesInline) != g.queuesInline ||
        u32(r + kReleasePool) != g.pool)
        return Fail("the two functions disagree on the queue layout");
    g.pop = reinterpret_cast<PopFn>(RipTarget(o + kObtainPop, 1, 5));
    g.freeLink = reinterpret_cast<FreeLinkFn>(RipTarget(o + kObtainFreeLink, 1, 5));
    g.linkBlocks = reinterpret_cast<uint8_t**>(RipTarget(o + kObtainLinks, 3, 7));
    if (reinterpret_cast<uint8_t*>(g.linkBlocks) != RipTarget(r + kReleaseLinks, 3, 7))
        return Fail("the two functions disagree on the lock-free link table");
    uint8_t* const reset = RipTarget(r + kReleaseReset, 1, 5);
    if (!game.text.Contains(reset) || !MatchesAt(reset, ParsePattern(kReset))) return Fail("the reset helper is not the expected one");
    uint8_t* const ctor = RipTarget(o + kObtainCtor, 1, 5);
    if (!game.text.Contains(ctor) || !MatchesAt(ctor, ParsePattern(kCtor))) return Fail("the constructor is not the expected one");
    if (!game.text.Contains(reinterpret_cast<uint8_t*>(g.pop)) || !game.text.Contains(reinterpret_cast<uint8_t*>(g.freeLink)))
        return Fail("the lock-free list functions are outside the code");
    g.obtain = o;
    g.release = r;
    g.ctor = ctor;
    g.found = true;
    // Part 2: Finalize, found through RHIFinalizeContext (optional: without it only the trim works).
    const auto calls = FindAll(game.text, ParsePattern(kFinalizeCall));
    uint8_t* const fin = calls.size() == 1 ? RipTarget(calls[0] + kFinalizeCallAt, 1, 5) : nullptr;
    if (fin && game.text.Contains(fin) && MatchesAt(fin, ParsePattern(kFinalize)) &&
        memcmp(fin, kFinalizePrologue, sizeof kFinalizePrologue) == 0) {
        const Section body{fin, fin + 0x200};
        const auto allocs = FindAll(body, ParsePattern(kFinalizeAllocator));
        const auto qt = FindAll(body, ParsePattern(kFinalizeQueueType));
        if (allocs.size() == 1 && qt.size() == 1) {
            const uint32_t allocOff = u32(allocs[0] + 3);
            // The hand-over must also clear the same member: mov qword [rdi+Allocator],0
            char clear[64];
            snprintf(clear, sizeof clear, "48 C7 87 %02X %02X %02X %02X 00 00 00 00", allocOff & 0xFF, (allocOff >> 8) & 0xFF,
                     (allocOff >> 16) & 0xFF, allocOff >> 24);
            const bool clears = FindAll(Section{allocs[0], allocs[0] + 0x60}, ParsePattern(clear)).size() == 1;
            g.allocOff = allocOff;
            g.queueTypeOff = qt[0][3];
            g.defaultFlagOff = calls[0][kDefaultFlagAt];
            g.finalize = fin;
            g.finalizeFound = clears && allocOff >= 0x20 && allocOff < 0x400 && g.defaultFlagOff < 0x40;
        }
    }
    if (!g.finalizeFound) Log("[alloc] immediate allocator reuse: Finalize not recognised: unavailable (the trim still works)");
    Log("[alloc] command allocator pools: obtain exe+0x%X, release exe+0x%X, constructor exe+0x%X, queues +0x%X/+0x%X, "
        "pool +0x%X",
        static_cast<unsigned>(o - game.base), static_cast<unsigned>(r - game.base), static_cast<unsigned>(ctor - game.base),
        g.queuesData, g.queuesInline, g.pool);
    return true;
}

bool InstallAllocatorTrim(bool beforeGameCode)
{
    const GameModule& game = Game();
    if (g.installed) return true;
    if (!g.found && !FindAllocatorTrim()) return false;
    if (!g.region) g.region = AllocNearGame(0x1000);
    if (!g.region) return Fail("no free memory near the game's code");
    void *t0 = nullptr, *t1 = nullptr, *t2 = nullptr;
    // Constructor first (it only counts), then release, then obtain (which may call the constructor's trampoline).
    if (!Detour(g.ctor, reinterpret_cast<void*>(&CtorHook), 0, &t0, beforeGameCode)) return Fail("constructor detour");
    g.ctorOrig = reinterpret_cast<CtorFn>(t0);
    if (!Detour(g.release, reinterpret_cast<void*>(&ReleaseHook), 1, &t1, beforeGameCode)) return Fail("release detour");
    g.releaseOrig = reinterpret_cast<ReleaseFn>(t1);
    if (!Detour(g.obtain, reinterpret_cast<void*>(&ObtainHook), 2, &t2, beforeGameCode)) return Fail("obtain detour");
    g.obtainOrig = reinterpret_cast<ObtainFn>(t2);
    g.installed = true;
    if (g.finalizeFound) {
        // Finalize's first 5 bytes are three pushes (push rbx; push rsi; push r15): relocatable like the others.
        void* t3 = nullptr;
        if (DetourWith(g.finalize, kFinalizePrologue, reinterpret_cast<void*>(&FinalizeHook), 3, &t3, beforeGameCode)) {
            g.finalizeOrig = reinterpret_cast<FinalizeFn>(t3);
            g.finalizeInstalled = true;
            Log("[alloc] immediate allocator reuse available: Finalize exe+0x%X (allocator +0x%X, queue type +0x%X, default "
                "flag +0x%X)", static_cast<unsigned>(g.finalize - game.base), g.allocOff, g.queueTypeOff, g.defaultFlagOff);
        } else {
            Log("[alloc] immediate allocator reuse: Finalize detour failed: unavailable");
        }
    }
    Log("[alloc] command allocator trim installed%s", beforeGameCode ? " (before the game's code)" : "");
    return true;
}

void SetAllocatorTrim(bool enabled, int windowSeconds)
{
    if (g_enabled.exchange(enabled) != enabled)
        Log("[alloc] command allocator trim %s", enabled ? "on" : "off (pools keep their peak, as the game does)");
    g_windowSeconds = (std::max)(1, (std::min)(windowSeconds, kBuckets - 1));
}


void SetImmediateAllocatorLists(int lists)
{
    lists = (std::max)(0, (std::min)(lists, 1024));
    if (g_immediateLists.exchange(lists) != lists && g.finalizeInstalled)
        Log("[alloc] immediate allocator reuse: %s", lists > 1 ? "on" : "off (a new allocator for every list, as the game does)");
}

// Called once a second (MemoryRelief's sampler). The over-budget mode needs 10 seconds over in a row and ends after 10 seconds
// under: an arrival after a door or fast travel goes over for a few seconds while Windows is busy moving memory, and a
// release then can take the driver 200+ ms (4K, 2026-10-12), so those peaks are left alone.
void SetAllocatorTrimPressure(bool over, bool tight)
{
    if (g_tight.exchange(tight) != tight)
        Log("[alloc] command allocator trim: copy pool %s", tight ? "trimmed after loads (video memory is getting tight)"
                                                                  : "kept (enough video memory to spare)");
    static int overSeconds = 0, underSeconds = 0;
    overSeconds = over ? (overSeconds < kPressureSeconds ? overSeconds + 1 : overSeconds) : 0;
    underSeconds = over ? 0 : (underSeconds < kPressureSeconds ? underSeconds + 1 : underSeconds);
    const bool was = g_pressure.load() != 0;
    const bool pressure = was ? underSeconds < kPressureSeconds : overSeconds >= kPressureSeconds;
    if (pressure != was) {
        g_pressure = pressure ? 1 : 0;
        Log("[alloc] command allocator trim: %s", pressure ? "video memory over budget for 10 s: copy pool, short window"
                                                           : "video memory back under budget for 10 s: normal window");
    }
}

// Monitor thread, once a second.
void AllocatorTrimTick()
{
    if (!g.installed) return;
    const int next = g_bucketIndex.load() + 1;
    for (auto& s : g_q) s.bucket[next % kBuckets].store(0);
    g_bucketIndex.store(next);
    if (!g_enabled.load()) return;
    void* const device = g_device.load();
    // Only while the RHI is active (obtains in the last two seconds) and its device has been the same for 30 seconds:
    // never near device creation or shutdown.
    const ULONGLONG now = GetTickCount64();
    if (!device || now - g_lastObtainMs.load() > 2000 || now - g_deviceSinceMs.load() < 30000) return;
    const bool pressure = g_pressure.load() != 0;
    const int window = pressure ? 3 : g_windowSeconds.load();
    for (int q = 0; q < kQueues; ++q) {
        // The direct and compute pools are always kept at their peak, as the game does: their need comes back with every
        // busy scene (trimming them on a roomy card made the engine create ~300 again in 5 minutes for ~60 MB; over budget
        // ~960 in 6 minutes, each new one taking fresh chunks from a driver that was already paging, and the scenes ran
        // slower than the stock game's). The copy pool's peak is a loading burst, needed again only at the next load.
        if (q != kCopyQueue) continue;
        if (!pressure && !g_tight.load()) continue;
        QueueStats& s = g_q[q];
        const int64_t peak = WindowPeak(s, window);
        const int64_t margin = pressure ? 2 : (std::max<int64_t>)(8, peak / 8);
        const int64_t real = s.created.load() - s.trimmed.load();
        const int64_t freeReal = real - s.inFlight.load();
        const int64_t keepFree = (std::max<int64_t>)(0, peak + margin - s.inFlight.load());
        if (freeReal <= keepFree + (pressure ? 0 : 8)) continue;  // hysteresis: trim only a clear surplus
        TrimResult r;
        const int ok = TrimQueue(device, q, keepFree, kMaxTrimPerTick, g_held, g_toRelease, &r);
        g_trimPasses.fetch_add(1);
        if (ok < 0) {
            Log("[alloc] command allocator trim: exception in the trim pass (queue %d): switched off", q);
            g_enabled = false;
            return;
        }
        s.trimmed.fetch_add(r.swapped);
        if (r.releaseMs > g_longestReleaseMs) g_longestReleaseMs = r.releaseMs;
        static const char* const names[kQueues] = {"direct", "copy", "compute"};
        if (r.swapped >= 8 || r.releaseMs >= 2.0) {
            LARGE_INTEGER f, c;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&c);
            Log("[alloc] trim: %s queue: %d of %d free allocators released, %.1f ms in the driver (longest %.1f ms; window peak %lld "
                "in flight%s; qpc %.1f ms)", names[q], r.swapped, r.real, r.releaseMs, r.longestOneMs, static_cast<long long>(peak),
                pressure ? ", over budget" : "", c.QuadPart * 1000.0 / f.QuadPart);
        }
    }
}

void LogAllocatorTrimStats()
{
    if (!g.installed) return;
    static const char* const names[kQueues] = {"direct", "copy", "compute"};
    char line[512];
    int len = 0;
    bool any = false;
    for (int q = 0; q < kQueues; ++q) {
        QueueStats& s = g_q[q];
        const int64_t created = s.created.load(), trimmed = s.trimmed.load(), refilled = s.refilled.load();
        len += snprintf(line + len, sizeof line - len, " %s %lld real (%lld in flight, peak %lld; +%lld made, -%lld trimmed, %lld refilled)",
                        names[q], static_cast<long long>(created - trimmed), static_cast<long long>(s.inFlight.load()),
                        static_cast<long long>(WindowPeak(s, kBuckets - 1)), static_cast<long long>(created - s.lastLogCreated),
                        static_cast<long long>(trimmed - s.lastLogTrimmed), static_cast<long long>(refilled - s.lastLogRefilled));
        any |= created != s.lastLogCreated || trimmed != s.lastLogTrimmed;
        s.lastLogCreated = created;
        s.lastLogTrimmed = trimmed;
        s.lastLogRefilled = refilled;
    }
    static int64_t lastKept = 0, lastHanded = 0;
    const int64_t kept = g_immediateKept.load(), handed = g_immediateHanded.load();
    const int64_t createUs = g_createUs.exchange(0), createMaxUs = g_createMaxUs.exchange(0);
    if (any || g_longestReleaseMs > 0 || kept != lastKept)
        Log("[alloc] command allocators:%s; creating them took %.1f ms (longest %.2f ms); longest release %.1f ms; immediate lists "
            "on a kept allocator %lld (handed over %lld)", line, createUs / 1000.0, createMaxUs / 1000.0, g_longestReleaseMs,
            static_cast<long long>(kept - lastKept), static_cast<long long>(handed - lastHanded));
    lastKept = kept;
    lastHanded = handed;
    g_longestReleaseMs = 0;
}

void* AllocatorTrimTestStub() { return &g_stub; }

bool AllocatorTrimInfo(AllocatorTrimInfoOut* out)
{
    if (!g.found) return false;
    out->obtain = g.obtain;
    out->release = g.release;
    out->ctor = g.ctor;
    out->pop = reinterpret_cast<uint8_t*>(g.pop);
    out->freeLink = reinterpret_cast<uint8_t*>(g.freeLink);
    out->linkBlocks = reinterpret_cast<uint8_t*>(g.linkBlocks);
    out->queuesData = g.queuesData;
    out->queuesInline = g.queuesInline;
    out->pool = g.pool;
    out->finalize = g.finalize;
    out->allocOff = g.allocOff;
    out->queueTypeOff = g.queueTypeOff;
    out->defaultFlagOff = g.defaultFlagOff;
    return true;
}

}  // namespace opf
