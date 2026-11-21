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
// the budget for 10 s in a row (arrival peaks pass). The direct and compute pools keep their peak, as in the game:
// their need returns with every busy scene (trimming them made the engine create ~300 again in 5 minutes on a roomy card,
// ~960 in 6 minutes over budget, where each new one took fresh chunks from a paging driver and the scenes ran slower).
// The one exception is CmdMemPregrow's give-back (cmd_pregrow.cpp): one pass on the direct pool, once a session, of what was grown ahead of need.
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
// The copy floor (AllocatorTrimCopyFloor, live; 0 = the rule above alone; added 2026-11-16; 0 in the shipped ini until the
// release, which sets 64): outside the over-budget mode a pass never gives back copy allocators below the floor, counting
// those in flight (give or take a release racing the pass: inFlight is read once before the pop). It only stops releases
// and never makes allocators, so the pool can sit below it after a session start or an over-budget episode until the game
// grows it again; raising the key live refills nothing. Why: the 20-s rule left 12-20, and a later streaming burst on a
// ride re-made 15-24 at once; their first driver memory queued on the device's paging fence ahead of a new texture pool
// that texture streaming creates with the pool allocator's lock held, and the render thread waited 30-50 ms for that lock
// (both such stalls in one of our sampled plays, 2026-11-15; that the floor removes them is expected, not yet shown).
// Our bug: the stock game never gives any back. In our logs the copy need outside loads sits under 64 in 95 % of the
// one-minute stats intervals (a cluster at 48-63, steady streaming), and 64 leaves 33 of 1062 trim-caused re-makes in our
// plays. About 1 MiB each on our AMD card (444 copy + 42 direct freed 0.50 GB, 2026-10-12): 64 cost about 64 MB, some
// 45-50 MB over the old rule. Once the game has been over its budget for 10 s the floor gives way (3-s window, margin 2).
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
#include <cstddef>
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
constexpr int kCopyFloorDefault = 64;  // AllocatorTrimCopyFloor's default (1.6.0; here, common.h, the ini)
constexpr int kCopyFloorMax = 512;

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
    std::atomic<int64_t> peakEver{0};   // the most in flight this session (CmdMemPregrow's give-back keeps this much)
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
std::atomic<bool> g_failed{false};  // an exception in a trim pass: off until the game restarts (the ini cannot switch it on)
std::atomic<int> g_windowSeconds{20};
std::atomic<int> g_copyFloor{kCopyFloorDefault};  // no copy allocator is given back below this many (in flight + free), except over budget
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
    cur = s.peakEver.load(std::memory_order_relaxed);
    while (now > cur && !s.peakEver.compare_exchange_weak(cur, now, std::memory_order_relaxed)) {
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
            g_deviceSinceMs.store(GetTickCount64());  // first: a tick that sees the new device must not see the old device's age
            g_device.store(device);
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
        if (!CodeSiteProtect(const_cast<LONG64*>(q), 8, &old)) return false;
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

// The copy pool's keep rule: how many free (real) allocators a trim pass leaves in the pool. The window peak plus a
// margin (max(8, peak/8); 2 over budget), and outside the over-budget mode at least the floor in all (in flight + free).
int64_t CopyKeepFree(int64_t peak, int64_t inFlight, bool pressure, int floor)
{
    const int64_t margin = pressure ? 2 : (std::max<int64_t>)(8, peak / 8);
    int64_t keep = peak + margin - inFlight;
    if (!pressure) keep = (std::max<int64_t>)(keep, static_cast<int64_t>(floor) - inFlight);
    return (std::max<int64_t>)(0, keep);
}
// Hysteresis: a pass runs only for a clear surplus (more than 8 over the keep, outside the over-budget mode).
bool CopyHasSurplus(int64_t freeReal, int64_t keepFree, bool pressure) { return freeReal > keepFree + (pressure ? 0 : 8); }

int64_t WindowPeak(const QueueStats& s, int seconds)
{
    const int now = g_bucketIndex.load();
    int64_t peak = s.inFlight.load();
    for (int i = 0; i < seconds && i < kBuckets; ++i) peak = (std::max)(peak, s.bucket[(now - i + kBuckets) % kBuckets].load());
    return peak;
}

// The copy pool's whole decision for one tick, as the tick uses it (and the self-test checks it on a fake QueueStats):
// the window (3 s over budget), the keep rule with the floor, the hysteresis. run = false: no pass this second.
struct CopyPass {
    int64_t peak = 0, real = 0, keepFree = 0;
    bool run = false;
};
CopyPass DecideCopyPass(const QueueStats& s, bool pressure, int windowSeconds, int floor)
{
    CopyPass p;
    p.peak = WindowPeak(s, pressure ? 3 : windowSeconds);
    p.real = s.created.load() - s.trimmed.load();
    const int64_t inFlight = s.inFlight.load();
    p.keepFree = CopyKeepFree(p.peak, inFlight, pressure, floor);
    p.run = CopyHasSurplus(p.real - inFlight, p.keepFree, pressure);
    return p;
}


struct TrimResult {
    int popped = 0, real = 0, swapped = 0;
    double releaseMs = 0, longestOneMs = 0;
};

// Which popped wrappers give up their D3D12 allocator. Most recently used first (the pool is a stack): the first keepReal
// real ones keep theirs, up to maxSwap of the rest get the stand-in. standIn[i] records every wrapper's place in the
// give-back (1: holds the stand-in, from an earlier trim or swapped now) while all of them are still ours: once the first
// is back in the pool the engine may take it again and its constructor rebuilds it (ObtainHook), so a wrapper is never
// read again after it went back (the 1.1.0-beta race, 2026-10-21: the second pass re-tested wrappers the first had
// returned, and reset and returned one the engine was using).
int MarkStandIns(void* const* held, int n, int64_t keepReal, int maxSwap, void** toRelease, uint8_t* standIn, int* real)
{
    int swapped = 0;
    for (int i = 0; i < n; ++i) {
        void*& d3d = D3dOf(held[i]);
        standIn[i] = d3d == &g_stub ? 1 : 0;
        if (standIn[i]) continue;
        if (++*real <= keepReal || swapped >= maxSwap) continue;
        toRelease[swapped++] = d3d;
        d3d = &g_stub;
        standIn[i] = 1;
    }
    return swapped;
}

// Back into the pool, stand-ins first so that the real ones are on top (taken next), by the places decided above. The
// engine's own release (push) resets each: a no-op for the stand-in; the real ones were reset when they came back from
// the GPU.
void GiveBack(void* device, void* const* held, const uint8_t* standIn, int n, ReleaseFn push)
{
    for (int pass = 0; pass < 2; ++pass)
        for (int i = n - 1; i >= 0; --i)
            if (standIn[i] == (pass == 0 ? 1 : 0)) push(device, held[i]);
}

// Monitor thread. SEH-only (no C++ objects with destructors): an exception here must not take the game down.
int TrimQueue(void* device, int q, int64_t keepReal, int maxSwap, void** held, uint8_t* standIn, void** toRelease,
              TrimResult* r)
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
        const int swapped = MarkStandIns(held, n, keepReal, maxSwap, toRelease, standIn, &r->real);
        r->swapped = swapped;
        GiveBack(device, held, standIn, n, g.releaseOrig);
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
uint8_t g_standIn[kMaxPop];
void* g_toRelease[kMaxPop];

// An exception in a trim pass: off until the game restarts. The monitor's once-a-second ini reload calls
// SetAllocatorTrim(1) again; before 1.1.3 that switched the trim back on a second later.
void NoteTrimException(int q)
{
    g_failed = true;
    g_enabled = false;
    Log("[alloc] command allocator trim: exception in the trim pass (queue %d): switched off until the game restarts", q);
}

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
    if (g_failed.load()) enabled = false;  // switched off by an exception: stays off (logged once, then)
    if (g_enabled.exchange(enabled) != enabled)
        Log("[alloc] command allocator trim %s", enabled ? "on" : "off (pools keep their peak, as the game does)");
    g_windowSeconds = (std::max)(1, (std::min)(windowSeconds, kBuckets - 1));
}

void SetAllocatorTrimCopyFloor(int floor)
{
    floor = (std::max)(0, (std::min)(floor, kCopyFloorMax));
    static bool logged = false;
    if (g_copyFloor.exchange(floor) != floor || !logged) {
        logged = true;
        if (!g_enabled.load())
            Log("[alloc] command allocator trim off: AllocatorTrimCopyFloor %d has nothing to do", floor);
        else if (floor > 0)
            Log("[alloc] command allocator trim: no copy allocator given back below %d, counting those in use "
                "(AllocatorTrimCopyFloor; it makes none; not once video memory has been over budget for 10 s)", floor);
        else
            Log("[alloc] command allocator trim: copy pool floor off (AllocatorTrimCopyFloor=0: the 20-s peak and margin only)");
    }
}

namespace {
// The self-test's fake pool: the push stands for the engine's release (reset + push), and after the first push the
// "engine" takes the top wrapper out again (another thread's obtain), as in the 19:42 crash of 2026-10-21.
struct FakeWrapper {
    void* device;
    int64_t queueType;
    void* d3d;  // +0x10, as in FD3D12CommandAllocator
};
static_assert(offsetof(FakeWrapper, queueType) == kAllocType && offsetof(FakeWrapper, d3d) == kAllocD3d,
              "the fake wrapper has the engine's layout");
struct FakePool {
    void* items[32] = {};
    int n = 0;
    bool obtainAfterFirst = false;
    void* refillTo = nullptr;  // what the obtained wrapper's +0x10 holds when the trim could look again (nullptr: the
                               // constructor has cleared it and not yet made the new allocator)
    void* inUse = nullptr;     // the wrapper the engine took out
    int pushes = 0;
    const char* fail = nullptr;
};
FakePool g_fakePool;
int g_fakeAllocators[16];  // stand for ID3D12CommandAllocators: only their addresses are used

void FakePush(void*, void* allocator)
{
    FakePool& p = g_fakePool;
    const auto* w = static_cast<FakeWrapper*>(allocator);
    if (!p.fail && !w->d3d) p.fail = "trim: a wrapper without an allocator was reset (the access violation of 2026-10-21)";
    if (!p.fail && allocator == p.inUse) p.fail = "trim: a wrapper the engine is using went back into the pool";
    for (int i = 0; i < p.n && !p.fail; ++i)
        if (p.items[i] == allocator) p.fail = "trim: a wrapper went back into the pool twice";
    if (p.n < 32) p.items[p.n++] = allocator;
    if (++p.pushes == 1 && p.obtainAfterFirst && p.n > 0) {
        // ObtainHook on another thread: pops the top wrapper, sees the stand-in and runs the engine's constructor on it.
        void* const top = p.items[--p.n];
        p.inUse = top;
        static_cast<FakeWrapper*>(top)->d3d = p.refillTo;
    }
}
}  // namespace

// Self-test of the trim's give-back and its switch-off, with fake wrappers and a fake pool (no game code runs).
// Returns the first failure or nullptr.
const char* AllocatorTrimSelfTest()
{
    const char* fail = nullptr;
    auto expect = [&](bool ok, const char* what) { if (!ok && !fail) fail = what; };
    // 0: nothing happens between the passes; 1: the engine takes the first stand-in back out and its constructor is
    // still running (+0x10 null); 2: the constructor has finished (a new allocator, the wrapper is recording).
    for (int mode = 0; mode < 3 && !fail; ++mode) {
        constexpr int n = 10;
        FakeWrapper w[n];
        void* held[n];
        uint8_t standIn[n];
        void* toRelease[n] = {};
        for (int i = 0; i < n; ++i) {
            // 2 and 5 hold the stand-in from an earlier trim
            w[i] = {nullptr, kCopyQueue, (i == 2 || i == 5) ? static_cast<void*>(&g_stub) : &g_fakeAllocators[i]};
            held[i] = &w[i];
        }
        g_fakePool = FakePool{};
        g_fakePool.obtainAfterFirst = mode > 0;
        g_fakePool.refillTo = mode == 2 ? &g_fakeAllocators[15] : nullptr;
        int real = 0;
        const int swapped = MarkStandIns(held, n, 3, 4, toRelease, standIn, &real);
        expect(swapped == 4 && real == 8, "trim: 4 of 8 real allocators released (3 kept, at most 4 a pass)");
        expect(toRelease[0] == &g_fakeAllocators[4] && toRelease[1] == &g_fakeAllocators[6] &&
                   toRelease[2] == &g_fakeAllocators[7] && toRelease[3] == &g_fakeAllocators[8],
               "trim: the released ones are the next less recently used after the kept ones");
        static const uint8_t kStandIn[n] = {0, 0, 1, 0, 1, 1, 1, 1, 1, 0};
        expect(memcmp(standIn, kStandIn, n) == 0, "trim: every wrapper's place decided before the give-back");
        GiveBack(nullptr, held, standIn, n, &FakePush);
        expect(g_fakePool.fail == nullptr, g_fakePool.fail);
        // The pool from bottom to top: the stand-ins 8 7 6 5 4 2, then the real ones 9 3 1 0 (the obtain took 8).
        static const int kOrder[n] = {8, 7, 6, 5, 4, 2, 9, 3, 1, 0};
        const int* const order = mode ? kOrder + 1 : kOrder;
        const int count = mode ? n - 1 : n;
        bool same = g_fakePool.n == count;
        for (int i = 0; i < count && same; ++i) same = g_fakePool.items[i] == held[order[i]];
        expect(same, mode ? "trim: an obtain between the passes: every other wrapper back once, stand-ins below"
                          : "trim: every wrapper back once, stand-ins at the bottom, real ones on top");
    }
    // CmdMemPregrow's give-back: a pool of 30, keep 10. The engine gets the kept ones first, then stand-ins that it refills one by one, as it
    // would make new ones: the books (created - trimmed = real) stay right and nothing goes back twice. The latch against a second pass: CmdPregrowSelfTest.
    if (!fail) {
        constexpr int n = 30;
        static FakeWrapper w[n];
        void* held[n];
        uint8_t standIn[n];
        static void* toRelease[n];
        static int d3d[n];
        for (int i = 0; i < n; ++i) {
            w[i] = {nullptr, 0, &d3d[i]};
            held[i] = &w[i];
        }
        g_fakePool = FakePool{};
        int real = 0;
        const int swapped = MarkStandIns(held, n, 10, 20, toRelease, standIn, &real);
        expect(swapped == 20 && real == 30, "give-back: 20 of 30 released, 10 kept");
        GiveBack(nullptr, held, standIn, n, &FakePush);
        expect(g_fakePool.fail == nullptr, g_fakePool.fail);
        expect(g_fakePool.n == n, "give-back: every wrapper back");
        int64_t created = 30;
        const int64_t trimmed = swapped;
        // The next obtains: only the kept ones, the most recently used first.
        for (int i = 0; i < 10 && !fail; ++i) {
            auto* top = static_cast<FakeWrapper*>(g_fakePool.items[g_fakePool.n - 1 - i]);
            expect(top->d3d != &g_stub, "give-back: a stand-in above a kept allocator");
            expect(top == &w[i], "give-back: the kept ones are the most recently used");
        }
        // A later busy scene takes everything: the rest are stand-ins, refilled on the way out.
        for (int i = 10; i < n && !fail; ++i) {
            auto* next = static_cast<FakeWrapper*>(g_fakePool.items[g_fakePool.n - 1 - i]);
            expect(next->d3d == &g_stub, "give-back: a real allocator below the stand-ins");
            next->d3d = &d3d[0];  // what ObtainHook's constructor call does
            ++created;
        }
        expect(created - trimmed == 30, "give-back: the books after the regrowth");
        int stillStub = 0;
        for (int i = 0; i < n; ++i) stillStub += w[i].d3d == &g_stub ? 1 : 0;
        expect(stillStub == 0, "give-back: stand-ins left after the regrowth");
    }
    // The copy floor (AllocatorTrimCopyFloor 64). A play of 2026-11-15, 21:07:33: 48 free, window peak 11, none
    // in flight: the old rule kept 19 (released 29); with the floor nothing goes.
    expect(CopyKeepFree(11, 0, false, 0) == 19 && CopyHasSurplus(48, 19, false), "floor: off, the old rule (peak 11 keeps 19)");
    expect(CopyKeepFree(11, 0, false, 64) == 64 && !CopyHasSurplus(48, 64, false), "floor: 48 free, peak 11: nothing released");
    expect(CopyKeepFree(4, 0, false, 64) == 64 && CopyHasSurplus(100, 64, false), "floor: 100 free, peak 4: trimmed to 64");
    expect(!CopyHasSurplus(72, 64, false) && CopyHasSurplus(73, 64, false), "floor: the hysteresis of 8 above it");
    expect(CopyKeepFree(63, 0, false, 64) == 71, "floor: a peak over it keeps peak + margin (63 + 8)");
    expect(CopyKeepFree(30, 30, false, 64) == 34, "floor: counts the ones in flight (30 + 34 = 64)");
    expect(CopyKeepFree(80, 70, false, 64) == 20, "floor: more in flight than the floor: peak + margin");
    expect(CopyKeepFree(4, 0, true, 64) == 6 && CopyHasSurplus(7, 6, true), "floor: over budget it gives way (peak 4 + 2)");
    expect(CopyKeepFree(0, 5, true, 64) == 0, "floor: never negative");
    {
        // After a load: 878 free, the window peak 63 then 4 (play e 21:16): passes of at most 32 a second end at 64-72.
        int64_t freeReal = 878;
        for (int t = 0; t < 200; ++t) {
            const int64_t peak = t < 10 ? 63 : 4;
            const int64_t keep = CopyKeepFree(peak, 0, false, 64);
            if (CopyHasSurplus(freeReal, keep, false)) freeReal -= (std::min<int64_t>)(kMaxTrimPerTick, freeReal - keep);
        }
        expect(freeReal >= 64 && freeReal <= 72, "floor: a pool after a load ends at 64-72");
        freeReal = 878;
        for (int t = 0; t < 200; ++t) {
            const int64_t keep = CopyKeepFree(4, 0, true, 64);
            if (CopyHasSurplus(freeReal, keep, true)) freeReal -= (std::min<int64_t>)(kMaxTrimPerTick, freeReal - keep);
        }
        expect(freeReal == 6, "floor: over budget the pool goes to peak + 2 as before");
    }
    {
        // The tick's own decision (DecideCopyPass) on a fake copy queue, and its keep handed to the pass's marking on a pool
        // of 48 (then 100) real wrappers: what the tick would release, not a re-implementation of it.
        static QueueStats fs;
        static FakeWrapper fw[100];
        static int fd3d[100];
        static void* fheld[100];
        static void* frel[100];
        static uint8_t fstand[100];
        auto setStats = [&](int64_t created, int64_t inFlight, int64_t peak) {
            fs.created = created;
            fs.trimmed = 0;
            fs.inFlight = inFlight;
            for (auto& b : fs.bucket) b.store(0);
            fs.bucket[g_bucketIndex.load() % kBuckets].store(peak);
        };
        auto released = [&](int n, int64_t keep) {
            for (int i = 0; i < n; ++i) {
                fw[i] = {nullptr, kCopyQueue, &fd3d[i]};
                fheld[i] = &fw[i];
            }
            int real = 0;
            return MarkStandIns(fheld, n, keep, kMaxTrimPerTick, frel, fstand, &real);
        };
        setStats(48, 0, 11);  // play e 21:07:33: 48 free, window peak 11
        CopyPass p = DecideCopyPass(fs, false, 20, 64);
        expect(!p.run && p.keepFree == 64 && p.real == 48, "floor (tick): 48 free, peak 11, floor 64: no pass");
        p = DecideCopyPass(fs, false, 20, 0);
        expect(p.run && p.keepFree == 19 && released(48, p.keepFree) == 29, "floor (tick): floor 0: the old rule releases 29");
        p = DecideCopyPass(fs, true, 20, 64);
        expect(p.run && p.keepFree == 13 && released(48, p.keepFree) == 32, "floor (tick): over budget the floor gives way (peak 11 + 2)");
        setStats(100, 0, 4);
        p = DecideCopyPass(fs, false, 20, 64);
        expect(p.run && p.keepFree == 64 && released(100, p.keepFree) == 32, "floor (tick): 100 free: 32 released, the first 64 kept");
        setStats(80, 0, 4);
        p = DecideCopyPass(fs, false, 20, 64);
        expect(p.run && released(80, p.keepFree) == 16, "floor (tick): 80 free: down to 64, not below");
        setStats(60, 20, 20);
        p = DecideCopyPass(fs, false, 20, 64);
        expect(!p.run && p.keepFree == 44, "floor (tick): 20 in flight + 40 free = 60 under the floor: no pass");
        const int wasFloor = g_copyFloor.load();
        SetAllocatorTrimCopyFloor(-5);
        expect(g_copyFloor.load() == 0, "floor: a negative value is off");
        SetAllocatorTrimCopyFloor(100000);
        expect(g_copyFloor.load() == kCopyFloorMax, "floor: clamped to 512");
        g_copyFloor = wasFloor;
    }
    // An exception in a trim pass switches the trim off until the game restarts: the per-second ini reload must not
    // switch it back on.
    const bool wasEnabled = g_enabled.load(), wasFailed = g_failed.load();
    const int wasWindow = g_windowSeconds.load();
    g_failed = false;
    SetAllocatorTrim(true, 20);
    expect(g_enabled.load(), "trim: switched on from the ini");
    NoteTrimException(kCopyQueue);
    expect(!g_enabled.load(), "trim: an exception switches it off");
    SetAllocatorTrim(true, 20);
    expect(!g_enabled.load(), "trim: after an exception the ini reload does not switch it back on");
    g_failed = wasFailed;
    g_enabled = wasEnabled;
    g_windowSeconds = wasWindow;
    return fail;
}


void SetImmediateAllocatorLists(int lists)
{
    lists = (std::max)(0, (std::min)(lists, 1024));
    if (g_immediateLists.exchange(lists) != lists && g.finalizeInstalled)
        Log("[alloc] immediate allocator reuse: %s", lists > 1 ? "on" : "off (a new allocator for every list, as the game does)");
}

// CmdMemPregrow (cmd_pregrow.cpp): the direct pool, past the counting hooks.
void* CmdMemPopDirect(void* device) { return PopFree(PoolOf(device, 0)); }
void* CmdMemObtainDirect(void* device) { return g.obtainOrig(device, 0); }
void CmdMemReleaseDirect(void* device, void* wrapper) { g.releaseOrig(device, wrapper); }
void* CmdMemD3dOf(void* wrapper) { return D3dOf(wrapper); }
int64_t CmdMemDirectCreated() { return g_q[0].created.load(); }
uint64_t CmdMemLastObtainMs() { return g_lastObtainMs.load(std::memory_order_relaxed); }
uint64_t CmdMemDeviceAgeMs() { return GetTickCount64() - g_deviceSinceMs.load(); }
bool AllocatorTrimFailed() { return g_failed.load(); }
void CmdMemDirectStats(int64_t* real, int64_t* inFlight, int64_t* peakEver)
{
    const QueueStats& s = g_q[0];
    *real = s.created.load() - s.trimmed.load();
    *inFlight = s.inFlight.load();
    *peakEver = s.peakEver.load();
}
// The give-back's one pass on the direct pool: the trim's own (monitor thread, the only one that runs it). 1 done, -1 an exception (then off, as after any trim's).
int CmdMemTrimDirect(void* device, int64_t keepFree, int maxSwap, CmdMemTrimOut* out)
{
    TrimResult r;
    const int ok = TrimQueue(device, 0, keepFree, maxSwap > kMaxPop ? kMaxPop : maxSwap, g_held, g_standIn, g_toRelease, &r);
    g_trimPasses.fetch_add(1);
    if (ok < 0) {
        NoteTrimException(0);
        return ok;
    }
    g_q[0].trimmed.fetch_add(r.swapped);
    if (r.releaseMs > g_longestReleaseMs) g_longestReleaseMs = r.releaseMs;
    out->swapped = r.swapped;
    out->freeReal = r.real;
    out->releaseMs = r.releaseMs;
    out->longestOneMs = r.longestOneMs;
    return ok;
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
    CmdMemPregrowTick(g_device.load(), GetTickCount64() - g_deviceSinceMs.load());
    const int next = g_bucketIndex.load() + 1;
    for (auto& s : g_q) s.bucket[next % kBuckets].store(0);
    g_bucketIndex.store(next);
    CmdMemGiveBackTick(g_device.load());  // its own key: not CommandAllocatorTrim's
    if (!g_enabled.load()) return;
    void* const device = g_device.load();
    // Only while the RHI is active (an obtain in the last 250 ms) and its device has been the same for 30 seconds: never
    // near device creation or shutdown. A quit from a world deletes the device 0.84-1.22 s after the last obtain, so this
    // must stay well under that. Obtains come every frame; a miss (a stall over 250 ms) just retries next second.
    const ULONGLONG now = GetTickCount64();
    if (!device || now - g_lastObtainMs.load() > 250 || now - g_deviceSinceMs.load() < 30000) return;
    const bool pressure = g_pressure.load() != 0;
    for (int q = 0; q < kQueues; ++q) {
        // The direct and compute pools are always kept at their peak, as the game does: their need comes back with every
        // busy scene (trimming them on a roomy card made the engine create ~300 again in 5 minutes for ~60 MB; over budget
        // ~960 in 6 minutes, each new one taking fresh chunks from a driver that was already paging, and the scenes ran
        // slower than the stock game's). The copy pool's peak is a loading burst, needed again only at the next load.
        if (q != kCopyQueue) continue;
        if (!pressure && !g_tight.load()) continue;
        QueueStats& s = g_q[q];
        const int floor = g_copyFloor.load();
        const CopyPass p = DecideCopyPass(s, pressure, g_windowSeconds.load(), floor);
        if (!p.run) continue;  // hysteresis: trim only a clear surplus
        const int64_t peak = p.peak, real = p.real;
        TrimResult r;
        const int ok = TrimQueue(device, q, p.keepFree, kMaxTrimPerTick, g_held, g_standIn, g_toRelease, &r);
        g_trimPasses.fetch_add(1);
        if (ok < 0) {
            NoteTrimException(q);
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
                "in flight, %lld left in all, floor %d%s; qpc %.1f ms)", names[q], r.swapped, r.real, r.releaseMs, r.longestOneMs,
                static_cast<long long>(peak), static_cast<long long>(real - r.swapped), pressure ? 0 : floor,
                pressure ? ", over budget: no floor" : "", c.QuadPart * 1000.0 / f.QuadPart);
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
