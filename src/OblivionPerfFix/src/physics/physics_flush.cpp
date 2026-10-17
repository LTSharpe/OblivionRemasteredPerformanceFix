// PhysicsFlushParallel: the physics step's catch-up of queued collision changes runs on two threads.
//
// Chaos keeps two copies of its collision lookup structure (AABB trees of every body): one for the physics thread and
// one for the game thread's queries. A background task rebuilds both from scratch every few frames; every change made
// while it works (a body added, moved or removed) is queued, and when the rebuild is done the physics step applies the
// queue to both new copies before it swaps them in (FPBDRigidsEvolutionBase::FlushAsyncAccelerationQueue, from
// ComputeIntermediateSpatialAcceleration, PBDRigidsEvolution.cpp): for each queued change, first the physics-thread copy
// (and the cache the next rebuild starts from), then the game-thread copy. When the world streams in a new area its
// thousands of rocks, trees and terrain pieces arrive within a second or two, the queue holds them all when a rebuild
// finishes, and the step inserts 9,000-13,000 changes one at a time (2.7-3.1 us each): 25-41 ms in one frame while the
// game thread waits for the step (Echo Mine -> Brotch Camp, 2026-10-13).
//
// The two copies are separate objects (their own trees, nothing shared; the game-thread copy is used by nothing else
// until the swap), so their updates can run side by side. With a large queue the fix gives the game-thread copy's
// updates to a helper thread, exactly as the game makes them (same calls, same arguments, same order), while the game's
// own function does the physics-thread copy and the cache: its calls for the game-thread copy go to an inert stand-in
// for the duration, and afterwards the real copy gets the step's timestamp as the function would have set it. The
// step waits for the helper before it continues. Both copies end up as in the stock game; the step takes about the
// time of the larger half.
//
// The same file holds PhysicsTreeTimeSlicing's adaptive copy slice: a detour at the entry of the physics step's
// ComputeIntermediateSpatialAcceleration counts the steps since the last swap and, while PhysicsTreeTimeSlicing is on, sets
// p.Chaos.AccelerationStructureTimeSlicingMaxBytesCopy per step: the game's value while the world is quiet (a rebuild
// cycle then lasts about 10 steps, as in the stock game), PhysicsTreeTimeSlicing's bigger value once a cycle has run 8 steps or 2048
// changes are waiting (a streamed area), so a rebuild finishes within a few frames when it matters and quiet frames
// keep the game's swap rate.
//
// Found by shape (build 19115871 addresses in the comments): the function's entry (queue data/count offsets), the block
// that updates the game-thread copy (copy offset, the dynamic-tree flag, the update/remove vtable slots, the particle
// arrays read for an update, the two filter helpers, the op stride) and the timestamp stores at its end. Any mismatch:
// the fix stays off with a log line.
#include "common.h"

#include <atomic>
#include <cstddef>
#include <cstring>

namespace opf {
namespace {

// FPBDRigidsEvolutionBase::FlushAsyncAccelerationQueue (exe+0x1931660): mov rax,rsp; push rbp; push r13; lea rbp,
// [rax-5Fh]; sub rsp,0C8h; ...; mov rbx,[rcx+Data]; mov r13,rcx; ...; movsxd rax,[rcx+Num]; mov [rbp+77h],rbx;
// lea rdx,[rax+rax*8]; lea rax,[rbx+rdx*8] (0x48-byte ops); ...; lea r12,[rcx+...]; mov edi,0FFFDh; lea r15,[rbx+38h]
constexpr char kEntry[] =
    "48 8B C4 55 41 55 48 8D 68 A1 48 81 EC C8 00 00 00 48 89 58 E8 45 33 D2 48 8B 99 ?? ?? 00 00 4C 8B E9 "
    "48 89 70 E0 48 89 78 D8 4C 89 60 D0 4C 89 70 C8 4C 89 78 C0 48 63 81 ?? ?? 00 00 48 89 5D 77 48 8D 14 C0 "
    "48 8D 04 D3 48 89 45 C7 48 3B D8 0F 84 ?? ?? ?? ?? 4C 8D A1 ?? ?? 00 00 BF FD FF 00 00 4C 8D 7B 38";
constexpr size_t kEntryData = 0x1B, kEntryNum = 0x39;
constexpr uint8_t kEntryPrologue[6] = {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x55};  // mov rax,rsp; push rbp; push r13
// The game-thread copy's update for one op (exe+0x1931B14, r15 = op + 0x38, rbx = op, r13 = evolution), through the
// loop's step to the next op (add rbx,48h; add r15,48h).
constexpr char kExternalBlock[] =
    "41 80 7F 08 00 49 8B 8D ?? ?? 00 00 75 32 83 3D ?? ?? ?? ?? 00 74 14 41 0F B7 07 66 C1 E8 03 66 FF C8 66 85 C7 "
    "0F 84 ?? ?? 00 00 48 8B 01 48 8B D3 45 0F B7 07 FF 90 ?? ?? 00 00 E9 ?? ?? 00 00 83 3D ?? ?? ?? ?? 00 4D 8B 4F D0 "
    "74 14 41 0F B7 07 66 C1 E8 03 66 FF C8 66 85 C7 0F 84 ?? ?? 00 00 49 8B 11 48 8B 01 4D 63 41 08 41 0F B7 1F "
    "4C 8B B0 ?? ?? 00 00 48 8B 82 ?? ?? 00 00 4B 8D 34 40 48 C1 E6 04 48 03 B2 ?? ?? 00 00 41 0F B6 3C 00 "
    "48 8B 82 ?? ?? 00 00 4A 8B 0C C0 48 89 4D DF 49 8B C9 4C 89 4D E7 48 8B 42 ?? 42 8B 14 80 33 C0 89 55 EF "
    "48 8D 55 F3 48 89 45 FB 48 89 45 F3 48 89 45 0B 48 89 45 03 88 45 13 E8 ?? ?? ?? ?? 49 8B 4F D0 48 8D 55 03 "
    "E8 ?? ?? ?? ?? 49 8B 8D ?? ?? 00 00 48 8D 55 DF 44 0F B6 CF C6 45 13 01 4C 8B C6 66 89 5C 24 20 41 FF D6 "
    "48 8B 5D 77 48 83 C3 48 49 83 C7 48";
constexpr size_t kBlkCopy = 0x08, kBlkFlag = 0x0E, kBlkFlag2 = 0x40, kBlkRemoveSlot = 0x37, kBlkUpdateSlot = 0x72,
                 kBlkHasBounds = 0x79, kBlkBounds = 0x88, kBlkGtParticle = 0x94, kBlkUnique = 0xAA, kBlkQuery = 0xCB,
                 kBlkSim = 0xD8, kBlkCopy2 = 0xE0;
// The end (exe+0x1931D1A): both copies get the evolution's timestamp: mov rcx,[r13+Internal]; mov eax,[r13+Stamp];
// mov [rcx+0Ch],eax; mov rcx,[r13+External]; mov eax,[r13+Stamp]; mov [rcx+0Ch],eax; add rsp,0C8h; pop r13; pop rbp; ret
constexpr char kTail[] =
    "49 8B 8D ?? ?? 00 00 41 8B 85 ?? ?? 00 00 89 41 0C 49 8B 8D ?? ?? 00 00 41 8B 85 ?? ?? 00 00 89 41 0C "
    "48 81 C4 C8 00 00 00 41 5D 5D C3";
constexpr size_t kTailInternal = 0x03, kTailStamp = 0x0A, kTailExternal = 0x14, kTailStamp2 = 0x1B;
constexpr uint32_t kCopyStampOffset = 0x0C;  // the collection's sync timestamp (mov [rcx+0Ch],eax)
constexpr size_t kTailSearch = 0x800;        // the tail is within this many bytes after the block
// FPBDRigidsEvolutionBase::ComputeIntermediateSpatialAcceleration (exe+0x192CC60): push rbx; push rbp; push rsi; push rdi;
// push r14; push r15; sub rsp,78h; lea rax,[stat]; movzx ebx,dl; mov [rsp+48h],rax; mov rdi,rcx; call ...
constexpr char kCisaEntry[] = "40 53 55 56 57 41 56 41 57 48 83 EC 78 48 8D 05 ?? ?? ?? ?? 0F B6 DA 48 89 44 24 48 48 8B F9 E8";
constexpr uint8_t kCisaPrologue[5] = {0x40, 0x53, 0x55, 0x56, 0x57};  // push rbx; push rbp; push rsi; push rdi
// ... its swap: mov rcx,rdi; mov bpl,1; call FlushAsyncAccelerationQueue (exe+0x192CDDE)
constexpr char kCisaSwapCall[] = "48 8B CF 40 B5 01 E8";
constexpr size_t kCisaSearch = 0x400;
constexpr int kBoostSteps = 8;          // PhysicsTreeTimeSlicing's copy slice from this many steps into a rebuild cycle ...
constexpr int kBoostChanges = 2048;     // ... or with this many changes waiting
// Op layout (FPendingSpatialData, 0x48 bytes): +0x00 the acceleration handle (the remove call's argument), +0x08 the
// particle handle (r15-30h), +0x38 the spatial index (u16, [r15]), +0x40 the operation (u8, [r15+8]; 0 = delete).
constexpr size_t kOpSize = 0x48, kOpParticle = 0x08, kOpIndex = 0x38, kOpOperation = 0x40;

// The handle the update passes ([rbp-21h] .. [rbp+13h]): game-thread particle, particle handle, unique index, query
// filter union (16), sim filter union (16), "can pre-filter" = 1.
struct AccelHandle {
    void* gtParticle;
    void* particle;
    uint32_t uniqueIdx;
    uint8_t query[16];
    uint8_t sim[16];
    uint8_t canPreFilter;
};
static_assert(offsetof(AccelHandle, uniqueIdx) == 0x10 && offsetof(AccelHandle, query) == 0x14 &&
              offsetof(AccelHandle, sim) == 0x24 && offsetof(AccelHandle, canPreFilter) == 0x34, "handle layout");

using FlushFn = void (*)(uint8_t* evolution);
using RemoveFn = bool (*)(void* collection, const void* handle, uint16_t index);
using UpdateFn = bool (*)(void* collection, const AccelHandle* handle, const void* bounds, uint8_t hasBounds, uint16_t index);
using FilterFn = void (*)(void* particle, void* out);

struct Sites {
    uint8_t* flush = nullptr;
    uint32_t data = 0, num = 0, internalCopy = 0, externalCopy = 0, stamp = 0;
    uint32_t removeSlot = 0, updateSlot = 0, hasBounds = 0, bounds = 0, gtParticle = 0, unique = 0;
    const volatile int32_t* dynamicTreeFlag = nullptr;
    FilterFn query = nullptr, sim = nullptr;
};
Sites g;

FlushFn g_original = nullptr;  // trampoline (or a fake in the self-test)
uint8_t* g_region = nullptr;
bool g_installed = false;

// PhysicsTreeTimeSlicing's adaptive copy slice (the step hook)
using CisaFn = void (*)(uint8_t* evolution, uint8_t block);
uint8_t* g_cisa = nullptr;
CisaFn g_cisaOriginal = nullptr;  // trampoline (or a fake in the self-test)
bool g_stepHooked = false;
std::atomic<int> g_steps{0};                 // steps since the last swap
std::atomic<volatile LONG*> g_copyVar{nullptr};
std::atomic<LONG> g_copyNormal{0}, g_copyBoost{0};
std::atomic<bool> g_adaptOn{false};
std::atomic<uint64_t> s_boostSteps{0}, s_steps{0};
std::atomic<bool> g_on{false};
std::atomic<int> g_minOps{1024};
// The stand-in for the game-thread copy while the game's function runs: a vtable of no-ops (only the remove and update
// slots are called; every slot is filled) and room for the timestamp it writes.
constexpr int kSinkSlots = 64;
uint8_t SinkNoop() { return 0; }
void* g_sinkVtable[kSinkSlots];
struct alignas(16) Sink {
    void** vtable;
    uint8_t rest[0x38];
} g_sink;

// The helper threads and their one job each (the caller holds g_jobLock for the whole flush): helper 0 = the game-thread
// copy (PhysicsFlushParallel), helper 1 = the physics-thread copy (PhysicsFlushThreeWay).
SRWLOCK g_jobLock = SRWLOCK_INIT;
HANDLE g_go = nullptr, g_done = nullptr, g_thread = nullptr;
struct Job {
    void* copy;
    uint8_t* ops;
    int count;
    int priority;
    uint64_t us;
} g_job;

std::atomic<uint64_t> s_split{0}, s_ops{0}, s_passThrough{0}, s_maxOps{0}, s_maxUs{0}, s_maxHelperUs{0}, s_sumUs{0};

LARGE_INTEGER Freq()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    return f;
}
uint64_t NowUs()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return QpcToMicroseconds(static_cast<uint64_t>(c.QuadPart), static_cast<uint64_t>(Freq().QuadPart));
}

void AtomicMax(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

bool SkipDynamic(uint16_t index)
{
    // As the game: with dynamic trees on, the dynamic-tree buckets are not updated here (copied after the swap).
    return *g.dynamicTreeFlag != 0 && static_cast<uint16_t>(static_cast<uint16_t>((index >> 3) - 1) & 0xFFFD) == 0;
}

// The game-thread copy's part of the flush, op by op, as the game's own loop does it.
void ExternalPass(void* copy, uint8_t* ops, int count)
{
    void* const* vtable = *static_cast<void* const* const*>(copy);
    const auto remove = reinterpret_cast<RemoveFn>(vtable[g.removeSlot / 8]);
    const auto update = reinterpret_cast<UpdateFn>(vtable[g.updateSlot / 8]);
    for (int i = 0; i < count; ++i) {
        uint8_t* const op = ops + static_cast<size_t>(i) * kOpSize;
        const uint16_t index = *reinterpret_cast<const uint16_t*>(op + kOpIndex);
        if (SkipDynamic(index)) continue;
        if (op[kOpOperation] == 0) {
            remove(copy, op, index);
            continue;
        }
        void* const particle = *reinterpret_cast<void* const*>(op + kOpParticle);
        const uint8_t* const container = *static_cast<uint8_t* const*>(particle);
        const int64_t k = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(particle) + 8);
        const uint8_t hasBounds = (*reinterpret_cast<const uint8_t* const*>(container + g.hasBounds))[k];
        const uint8_t* const bounds = *reinterpret_cast<const uint8_t* const*>(container + g.bounds) + k * 0x30;
        AccelHandle h{};
        h.gtParticle = (*reinterpret_cast<void* const* const*>(container + g.gtParticle))[k];
        h.particle = particle;
        h.uniqueIdx = (*reinterpret_cast<const uint32_t* const*>(container + g.unique))[k];
        g.query(particle, h.query);
        g.sim(particle, h.sim);
        h.canPreFilter = 1;
        update(copy, &h, bounds, hasBounds, index);
    }
}

DWORD WINAPI HelperThread(LPVOID which)
{
    (void)which;
    const HANDLE go = g_go, done = g_done;
    Job& job = g_job;
    int priority = THREAD_PRIORITY_NORMAL;
    for (;;) {
        if (WaitForSingleObject(go, INFINITE) != WAIT_OBJECT_0) return 0;
        if (job.priority != priority) SetThreadPriority(GetCurrentThread(), priority = job.priority);
        HelperThreadCache();  // the game allocator's per-thread cache (HelperThreadCaches, static_bodies.cpp)
        const uint64_t t0 = NowUs();
        BulkBatchBegin(job.count);  // BulkIndexInsert: this pass's dirty-tree inserts batched (if on)
        ExternalPass(job.copy, job.ops, job.count);
        BulkBatchEnd();
        job.us = NowUs() - t0;
        SetEvent(done);
    }
}

// In place of ComputeIntermediateSpatialAcceleration (the physics step's thread): PhysicsTreeTimeSlicing's copy slice for the task this
// step starts (it reads the variable when it copies), then the game's function.
void CisaHook(uint8_t* evolution, uint8_t block)
{
    if (g_adaptOn.load(std::memory_order_relaxed)) {
        if (volatile LONG* var = g_copyVar.load(std::memory_order_relaxed)) {
            const int count = *reinterpret_cast<const int32_t*>(evolution + g.num);
            const bool boost = g_steps.load(std::memory_order_relaxed) >= kBoostSteps || count >= kBoostChanges;
            const LONG want = boost ? g_copyBoost.load(std::memory_order_relaxed) : g_copyNormal.load(std::memory_order_relaxed);
            if (want > 0 && *var != want) InterlockedExchange(var, want);
            s_steps.fetch_add(1, std::memory_order_relaxed);
            if (boost) s_boostSteps.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_steps.fetch_add(1, std::memory_order_relaxed);
    g_cisaOriginal(evolution, block);
}

// In place of FlushAsyncAccelerationQueue (the physics step's thread).
void FlushSplit(uint8_t* evolution, int count);
void FlushHook(uint8_t* evolution)
{
    g_steps.store(0, std::memory_order_relaxed);  // a swap: the next rebuild cycle starts
    const int count = *reinterpret_cast<const int32_t*>(evolution + g.num);
    BulkFlushAsyncBegin(evolution);  // PhysicsInternalFlushParallel: the step's FlushInternal on a helper meanwhile (if on)
    if (!g_on.load(std::memory_order_relaxed) || count < g_minOps.load(std::memory_order_relaxed) || !g_thread ||
        !TryAcquireSRWLockExclusive(&g_jobLock)) {  // (else another scene's flush has the helper)
        if (count > 0) s_passThrough.fetch_add(1, std::memory_order_relaxed);
        BulkBatchBegin(count);  // BulkIndexInsert: this thread's dirty-tree inserts batched (if on)
        g_original(evolution);
        BulkBatchEnd();
    } else {
        FlushSplit(evolution, count);
    }
    BulkFlushAsyncEnd();
}

// PhysicsFlushParallel's split: the game-thread copy's updates on the helper, the rest in the game's function here.
void FlushSplit(uint8_t* evolution, int count)
{
    const uint64_t t0 = NowUs();
    void** const copySlot = reinterpret_cast<void**>(evolution + g.externalCopy);
    void* const copy = *copySlot;
    g_job.copy = copy;
    g_job.ops = *reinterpret_cast<uint8_t**>(evolution + g.data);
    g_job.count = count;
    g_job.priority = GetThreadPriority(GetCurrentThread());
    if (g_job.priority == THREAD_PRIORITY_ERROR_RETURN) g_job.priority = THREAD_PRIORITY_NORMAL;
    SetEvent(g_go);
    // The game's function does the physics-thread copy (unless 38b has it) and the cache; its calls for the copies the
    // helpers have go to the stand-ins. The queue's storage stays allocated when the function empties it (Reset keeps
    // the allocation), and nothing adds to it until the step continues after our wait.
    *copySlot = &g_sink;
    BulkBatchBegin(count);  // BulkIndexInsert: this thread's dirty-tree inserts batched (if on)
    g_original(evolution);
    BulkBatchEnd();
    *copySlot = copy;
    WaitForSingleObject(g_done, INFINITE);
    const int32_t stamp = *reinterpret_cast<const int32_t*>(evolution + g.stamp);
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(copy) + kCopyStampOffset) = stamp;
    const uint64_t us = NowUs() - t0;
    ReleaseSRWLockExclusive(&g_jobLock);
    s_split.fetch_add(1, std::memory_order_relaxed);
    s_ops.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
    s_sumUs.fetch_add(us, std::memory_order_relaxed);
    AtomicMax(s_maxOps, static_cast<uint64_t>(count));
    AtomicMax(s_maxUs, us);
    AtomicMax(s_maxHelperUs, g_job.us);
}

bool Fail(const char* why)
{
    Log("[chaos] parallel flush: %s: PhysicsFlushParallel unavailable", why);
    return false;
}

uint32_t Disp32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

}  // namespace

bool FindPhysicsFlush()
{
    if (g.flush) return true;
    const GameModule& game = Game();
    const auto entries = FindAll(game.text, ParsePattern(kEntry));
    if (entries.size() != 1) {
        Log("[chaos] parallel flush: FlushAsyncAccelerationQueue found %zu times (expected 1): PhysicsFlushParallel unavailable", entries.size());
        return false;
    }
    uint8_t* const fn = entries[0];
    // The game-thread copy's block and the tail, in this function (before the next function's padding).
    const Pattern block = ParsePattern(kExternalBlock), tail = ParsePattern(kTail);
    uint8_t* blk = nullptr;
    for (uint8_t* p = fn + 0x80; p < fn + 0x800 && !blk; ++p)
        if (MatchesAt(p, block)) blk = p;
    if (!blk) return Fail("the game-thread copy's update was not recognised");
    uint8_t* end = nullptr;
    for (uint8_t* p = blk + block.size(); p < blk + kTailSearch && !end; ++p)
        if (MatchesAt(p, tail)) end = p;
    if (!end) return Fail("the end of the function was not recognised");

    Sites s;
    s.flush = fn;
    s.data = Disp32(fn + kEntryData);
    s.num = Disp32(fn + kEntryNum);
    s.externalCopy = Disp32(blk + kBlkCopy);
    s.internalCopy = Disp32(end + kTailInternal);
    s.stamp = Disp32(end + kTailStamp);
    s.removeSlot = Disp32(blk + kBlkRemoveSlot);
    s.updateSlot = Disp32(blk + kBlkUpdateSlot);
    s.hasBounds = Disp32(blk + kBlkHasBounds);
    s.bounds = Disp32(blk + kBlkBounds);
    s.gtParticle = Disp32(blk + kBlkGtParticle);
    s.unique = blk[kBlkUnique];
    const uint8_t* const flag = RipTarget(blk + kBlkFlag, 2, 7);
    s.query = reinterpret_cast<FilterFn>(RipTarget(blk + kBlkQuery, 1, 5));
    s.sim = reinterpret_cast<FilterFn>(RipTarget(blk + kBlkSim, 1, 5));
    if (s.num != s.data + 8) return Fail("queue count not next to its data");
    if (Disp32(blk + kBlkCopy2) != s.externalCopy || Disp32(end + kTailExternal) != s.externalCopy ||
        Disp32(end + kTailStamp2) != s.stamp || s.internalCopy + 8 != s.externalCopy)
        return Fail("copy offsets disagree");
    if (RipTarget(blk + kBlkFlag2, 2, 7) != flag || !game.data.Contains(flag)) return Fail("dynamic-tree flag not found");
    if (s.removeSlot % 8 || s.updateSlot % 8 || s.removeSlot >= kSinkSlots * 8 || s.updateSlot >= kSinkSlots * 8)
        return Fail("unexpected vtable slots");
    if (!game.text.Contains(reinterpret_cast<const void*>(s.query)) || !game.text.Contains(reinterpret_cast<const void*>(s.sim)))
        return Fail("filter helpers not in the game's code");
    s.dynamicTreeFlag = reinterpret_cast<const volatile int32_t*>(flag);
    g = s;
    // The step that calls it (for PhysicsTreeTimeSlicing's adaptive copy slice): its entry, and its swap's call to this function.
    g_cisa = nullptr;
    const auto steps = FindAll(game.text, ParsePattern(kCisaEntry));
    if (steps.size() == 1) {
        const Pattern call = ParsePattern(kCisaSwapCall);
        for (uint8_t* p = steps[0]; p < steps[0] + kCisaSearch && !g_cisa; ++p)
            if (MatchesAt(p, call) && RipTarget(p + 6, 1, 5) == fn) g_cisa = steps[0];
    }
    if (!g_cisa) Log("[chaos] ComputeIntermediateSpatialAcceleration not recognised (%zu matches): PhysicsTreeTimeSlicing's copy slice stays fixed",
                     steps.size());
    Log("[chaos] parallel flush: FlushAsyncAccelerationQueue exe+0x%X (queue +0x%X, copies +0x%X/+0x%X, timestamp +0x%X, "
        "slots +0x%X/+0x%X, dynamic-tree flag exe+0x%X)",
        static_cast<unsigned>(fn - game.base), g.data, g.internalCopy, g.externalCopy, g.stamp, g.removeSlot, g.updateSlot,
        static_cast<unsigned>(flag - game.base));
    return true;
}

namespace {
// An entry detour in two parts. Prepare: the first n bytes of fn (whole, position-independent instructions) go to a
// trampoline at slot that jumps back to fn + n, and a stub at slot + 32 jumps to hook (the region is still writable).
// Patch (after the region is made executable): fn starts with a jmp to the stub. The n bytes must lie in one aligned
// qword (one atomic write, safe while the game runs); before the game's code runs a plain write is fine too.
bool PrepareDetour(uint8_t* fn, const uint8_t* prologue, size_t n, void* hook, uint8_t* slot)
{
    if (memcmp(fn, prologue, n) != 0) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + n;
    memcpy(slot, prologue, n);
    memcpy(slot + n, jmpAbs, sizeof jmpAbs);
    memcpy(slot + n + sizeof jmpAbs, &resume, sizeof resume);
    memcpy(slot + 32, jmpAbs, sizeof jmpAbs);
    memcpy(slot + 32 + sizeof jmpAbs, &hook, sizeof hook);
    return true;
}

bool PatchDetour(uint8_t* fn, size_t n, uint8_t* slot, bool beforeGameCode)
{
    const int64_t rel = (slot + 32) - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t jmp[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(jmp + 1, &rel32, sizeof rel32);
    const uintptr_t at = reinterpret_cast<uintptr_t>(fn);
    DWORD old;
    if ((at & 7) + n <= 8) {
        auto* q = reinterpret_cast<volatile LONG64*>(at & ~uintptr_t{7});
        if (!VirtualProtect(const_cast<LONG64*>(q), 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        const LONG64 before = *q;
        LONG64 after = before;
        memcpy(reinterpret_cast<uint8_t*>(&after) + (at & 7), jmp, n);
        const bool ok = InterlockedCompareExchange64(q, after, before) == before;
        VirtualProtect(const_cast<LONG64*>(q), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(q), 8);
        return ok;
    }
    return beforeGameCode && WriteCode(fn, std::span<const uint8_t>(jmp, n));
}
}  // namespace

// Both detours: FlushAsyncAccelerationQueue (PhysicsFlushParallel; also marks each swap for PhysicsTreeTimeSlicing's copy slice) and the step
// (PhysicsTreeTimeSlicing's adaptive copy slice). Each is a pass-through while its fix is off. Installed once.
bool InstallPhysicsFlush(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!g.flush && !FindPhysicsFlush()) return false;
    for (void*& slot : g_sinkVtable) slot = reinterpret_cast<void*>(&SinkNoop);
    g_sink.vtable = g_sinkVtable;
    if (!g_go) g_go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_done) g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_go || !g_done) return Fail("no events");
    if (!g_thread) g_thread = CreateThread(nullptr, 0, HelperThread, nullptr, 0, nullptr);
    if (!g_thread) return Fail("no helper thread");
    if (g_region) return Fail("already tried");
    g_region = AllocNearGame(0x10000);
    if (!g_region) return Fail("no free memory near the game's code");
    // region: [0] FlushAsyncAccelerationQueue's trampoline and stub, [64] the step's (written before it is executable)
    uint8_t* const flushSlot = g_region;
    uint8_t* const stepSlot = g_region + 64;
    if (!PrepareDetour(g.flush, kEntryPrologue, sizeof kEntryPrologue, reinterpret_cast<void*>(&FlushHook), flushSlot))
        return Fail("the function's first instructions changed");
    const bool step = g_cisa && PrepareDetour(g_cisa, kCisaPrologue, sizeof kCisaPrologue, reinterpret_cast<void*>(&CisaHook),
                                              stepSlot);
    DWORD old;
    VirtualProtect(g_region, 0x10000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), g_region, 128);
    g_original = reinterpret_cast<FlushFn>(flushSlot);
    g_cisaOriginal = reinterpret_cast<CisaFn>(stepSlot);
    if (!PatchDetour(g.flush, sizeof kEntryPrologue, flushSlot, beforeGameCode)) return Fail("could not place the detour");
    g_installed = true;
    Log("[chaos] parallel flush installed (FlushAsyncAccelerationQueue detoured)");
    if (step && PatchDetour(g_cisa, sizeof kCisaPrologue, stepSlot, beforeGameCode)) {
        g_stepHooked = true;
        Log("[chaos] physics step hooked (ComputeIntermediateSpatialAcceleration exe+0x%X): PhysicsTreeTimeSlicing's copy slice adapts per step",
            static_cast<unsigned>(g_cisa - Game().base));
    } else {
        Log("[chaos] physics step not hooked: PhysicsTreeTimeSlicing's copy slice is a fixed size");
    }
    return true;
}

bool PhysicsStepHooked() { return g_stepHooked; }

// PhysicsTreeTimeSlicing (stream_churn.cpp): on = the hook owns the variable (normal / boost per step); off = the game's value back
// if the variable still holds one of ours.
void SetPhysicsCopyAdaptive(volatile LONG* var, LONG normal, LONG boost, bool on)
{
    if (!g_stepHooked || !var) return;
    if (on) {
        g_copyNormal = normal;
        g_copyBoost = boost > normal ? boost : normal;
        g_copyVar = var;
        if (!g_adaptOn.exchange(true))
            Log("[chaos] physics tree copy slice: %ld bytes per frame while a rebuild runs %d+ frames or %d+ changes wait, "
                "else the game's %ld", static_cast<long>(g_copyBoost.load()), kBoostSteps, kBoostChanges, normal);
    } else if (g_adaptOn.exchange(false)) {
        const LONG cur = *var;
        if (cur == g_copyBoost.load() || cur == g_copyNormal.load()) InterlockedExchange(var, normal);
        Log("[chaos] physics tree copy slice: fixed again");
    }
}

void SetPhysicsFlushParallel(bool enabled, int minOps)
{
    g_minOps = minOps < 1 ? 1 : minOps;
    if (!g_installed || g_on.load() == enabled) return;
    g_on = enabled;
    Log("[chaos] %s", enabled ? "collision changes queued during a rebuild: the game-thread copy's updates run on a helper thread "
                                "(queues of at least the threshold)"
                              : "collision changes queued during a rebuild applied on one thread, as before");
}

void LogPhysicsFlushStats()
{
    const uint64_t split = s_split.exchange(0), ops = s_ops.exchange(0), sum = s_sumUs.exchange(0);
    const uint64_t maxOps = s_maxOps.exchange(0), maxUs = s_maxUs.exchange(0), maxHelper = s_maxHelperUs.exchange(0);
    const uint64_t steps = s_steps.exchange(0), boosted = s_boostSteps.exchange(0);
    s_passThrough.exchange(0);
    if (split)
        Log("[chaos] parallel flush since last report: %llu split (%llu changes, %.1f ms in all); largest %llu changes, "
            "longest %.1f ms (helper %.1f ms)",
            split, ops, sum / 1000.0, maxOps, maxUs / 1000.0, maxHelper / 1000.0);
    if (boosted) Log("[chaos] physics tree copy slice: the bigger slice in %llu of %llu steps", boosted, steps);
}

bool PhysicsFlushInfo(PhysicsFlushInfoOut* out)
{
    if (!g.flush) return false;
    out->flush = g.flush;
    out->data = g.data;
    out->num = g.num;
    out->internalCopy = g.internalCopy;
    out->externalCopy = g.externalCopy;
    out->stamp = g.stamp;
    out->removeSlot = g.removeSlot;
    out->updateSlot = g.updateSlot;
    out->hasBounds = g.hasBounds;
    out->bounds = g.bounds;
    out->gtParticle = g.gtParticle;
    out->unique = g.unique;
    out->dynamicTreeFlag = reinterpret_cast<const uint8_t*>(const_cast<const int32_t*>(g.dynamicTreeFlag));
    out->query = reinterpret_cast<const uint8_t*>(g.query);
    out->sim = reinterpret_cast<const uint8_t*>(g.sim);
    out->step = g_cisa;
    return true;
}

// Self-test: run the hook against fakes (a fake "original", fake filter helpers and a fake dynamic-tree flag).
void PhysicsFlushTestSetup(void* fakeOriginal, void* fakeQuery, void* fakeSim, const int32_t* fakeFlag)
{
    g_original = reinterpret_cast<FlushFn>(fakeOriginal);
    g.query = reinterpret_cast<FilterFn>(fakeQuery);
    g.sim = reinterpret_cast<FilterFn>(fakeSim);
    g.dynamicTreeFlag = fakeFlag;
    for (void*& slot : g_sinkVtable) slot = reinterpret_cast<void*>(&SinkNoop);
    g_sink.vtable = g_sinkVtable;
    if (!g_go) g_go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_done) g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_thread) g_thread = CreateThread(nullptr, 0, HelperThread, nullptr, 0, nullptr);
    g_installed = true;
}

void PhysicsFlushTestCall(uint8_t* evolution) { FlushHook(evolution); }

void PhysicsStepTestSetup(void* fakeStep)
{
    g_cisaOriginal = reinterpret_cast<CisaFn>(fakeStep);
    g_stepHooked = true;
}

void PhysicsStepTestCall(uint8_t* evolution, uint8_t block) { CisaHook(evolution, block); }

}  // namespace opf
