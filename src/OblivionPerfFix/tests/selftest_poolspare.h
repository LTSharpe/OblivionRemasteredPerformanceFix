// GpuPoolSpareAhead self-test, included by selftest.cpp: CreateNewPool, its vtable slot, FRHIMemoryPool::Init and
// TryAllocateInternal on the mapped exe; then the hook and the thread's work on fake allocators whose fake CreateNewPool
// builds a pool as FRHIMemoryPool::Init leaves it (index, head block, one free block), with a fake pool vtable; and the game's end
// (the allocator's deleting destructor hook, poolfix) and the quiet wait.
#pragma once

namespace psp {

static int g_destroyCalls = 0, g_deleteCalls = 0, g_createCalls = 0, g_live = 0, g_allocDtorCalls = 0;
static void* g_lastMade = nullptr;      // the last pool FakeCreate built
static void* g_dtorDuringCreate = nullptr;  // FakeCreate deletes this allocator while it makes a pool (the game ending mid-make)
static HANDLE g_createEntered = nullptr, g_createRelease = nullptr;  // FakeCreate signals the first, then waits for the second
static void* FakeAllocDtor(void* allocator, uint32_t) { ++g_allocDtorCalls; return allocator; }

static void FakeDestroy(void*) { ++g_destroyCalls; }
static void* FakeDelete(void* pool, uint32_t flags)
{
    ++g_deleteCalls;
    if (flags & 1) {
        uint8_t* p = static_cast<uint8_t*>(pool);
        uint8_t** blocks = *reinterpret_cast<uint8_t***>(p + 0x70);
        delete[] blocks[0];
        delete[] blocks;
        delete[] p;
        --g_live;
    }
    return pool;
}
static void* g_vtbl[3] = {reinterpret_cast<void*>(&FakeDelete), nullptr, reinterpret_cast<void*>(&FakeDestroy)};

// as the game's CreateNewPool + FRHIMemoryPool::Init: size = default (dword +8) unless the minimum is larger (power of two)
static void* FakeCreate(void* allocator, uint16_t index, uint32_t minSize, uint32_t type)
{
    ++g_createCalls;
    ++g_live;
    uint32_t size = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(allocator) + 8);
    if (minSize > size) {
        uint32_t s = 1;
        while (s < minSize) s <<= 1;
        size = s;
    }
    uint8_t* p = new uint8_t[0xE8]();
    uint8_t* fb = new uint8_t[0x30]();
    uint8_t** blocks = new uint8_t*[4]();
    blocks[0] = fb;
    *reinterpret_cast<void***>(p) = g_vtbl;
    *reinterpret_cast<uint16_t*>(p + 0x08) = index;
    *reinterpret_cast<uint64_t*>(p + 0x10) = size;
    *reinterpret_cast<uint32_t*>(p + 0x1C) = type;
    *reinterpret_cast<uint64_t*>(p + 0x28) = size;
    uint8_t* head = p + 0x40;
    *reinterpret_cast<uint32_t*>(head + 0x0C) = 0xFFE00000u | (3u << 16) | index;  // garbage in the unused bits, as Init leaves them
    *reinterpret_cast<uint8_t**>(head + 0x10) = fb;
    *reinterpret_cast<uint8_t**>(head + 0x18) = fb;
    *reinterpret_cast<uint32_t*>(fb + 0x00) = size;
    *reinterpret_cast<uint32_t*>(fb + 0x04) = 0x10000;
    *reinterpret_cast<uint32_t*>(fb + 0x0C) = (1u << 16) | index;
    *reinterpret_cast<uint8_t**>(fb + 0x10) = head;
    *reinterpret_cast<uint8_t**>(fb + 0x18) = head;
    *reinterpret_cast<uint8_t***>(p + 0x70) = blocks;
    *reinterpret_cast<int32_t*>(p + 0x78) = 1;
    *reinterpret_cast<int32_t*>(p + 0x7C) = 4;
    g_lastMade = p;
    if (g_createRelease) {  // the two-thread end test: our "thread" is inside the game's call until the main thread lets it go
        const HANDLE rel = g_createRelease;
        g_createRelease = nullptr;
        SetEvent(g_createEntered);
        WaitForSingleObject(rel, 5000);
    }
    if (g_dtorDuringCreate) {
        void* const d = g_dtorDuringCreate;
        g_dtorDuringCreate = nullptr;
        opf::GpuPoolSpareTestDtor(d, 1);
    }
    return p;
}

static bool Indexed(void* pool, uint16_t index)
{
    uint8_t* p = static_cast<uint8_t*>(pool);
    uint8_t* fb = (*reinterpret_cast<uint8_t***>(p + 0x70))[0];
    return *reinterpret_cast<uint16_t*>(p + 0x08) == index && *reinterpret_cast<uint16_t*>(p + 0x4C) == index &&
           *reinterpret_cast<uint16_t*>(fb + 0x0C) == index && ((*reinterpret_cast<uint32_t*>(p + 0x4C) >> 16) & 0xF) == 3 &&
           ((*reinterpret_cast<uint32_t*>(fb + 0x0C) >> 16) & 0xF) == 1;
}
static uint64_t SizeOf(void* pool) { return *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(pool) + 0x10); }

struct FakeAlloc {
    alignas(16) uint8_t b[0x100];
    FakeAlloc(uint32_t defSize, uint32_t heapType)
    {
        memset(b, 0, sizeof b);
        *reinterpret_cast<uint32_t*>(b + 0x08) = defSize;
        *reinterpret_cast<uint32_t*>(b + 0x98) = heapType;
    }
};

static void Drain()
{
    while (opf::GpuPoolSpareTestWork()) {}
}
static DWORD WINAPI DrainThread(void*)
{
    Drain();
    return 0;
}
static DWORD WINAPI ReleaseAfter10ms(void* ev)
{
    Sleep(10);
    SetEvent(static_cast<HANDLE>(ev));
    return 0;
}

inline void PoolSpareSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    constexpr uint32_t M = 1u << 20;
    if (game.timeDateStamp == 0xF19077A4u) {
        uint32_t c = 0, s = 0, v = 0, pi = 0, ta = 0, dt = 0;
        const bool ok = GpuPoolSpareTestDiscover(game, &c, &s, &v, &pi, &ta, &dt);
        const bool want = ok && c == 0x251FAE0 && s == 0x725D268 && v == 0x725D248 && pi == 0x1CBFF90 && ta == 0x1CC1271 && dt == 0x25172A0;
        Check(want, "poolspare: CreateNewPool, its one vtable slot, the pool vtable, FRHIMemoryPool::Init, TryAllocateInternal and the "
                    "allocator's deleting destructor (slot 0) match");
        if (!want) printf("    found %d: create 0x%X slot 0x%X pool vtbl 0x%X init 0x%X try 0x%X dtor 0x%X\n", ok, c, s, v, pi, ta, dt);
    }
    uint64_t taken, made, under, back, noRoom;
    bool broken;
    FakeAlloc a(32 * M, 1), b(32 * M, 1), up(4 * M, 2);
    void* got[24] = {};
    int n = 0;
    GpuPoolSpareTestSetup(reinterpret_cast<void*>(&FakeCreate), g_vtbl, reinterpret_cast<void*>(&FakeAllocDtor));
    SetGpuPoolSpare(true, 256, 0);  // cap 128 MB

    // 1. first growth: no spare yet -> the game's call; one spare requested, made by the thread's work
    got[n++] = GpuPoolSpareTestCall(a.b, 3, 4096, 1);
    GpuPoolSpareTestCounts(&taken, &made, &under, &back, &noRoom, &broken);
    Check(got[0] && Indexed(got[0], 3) && taken == 0 && under == 1 && GpuPoolSpareTestReady(a.b) == 0 && GpuPoolSpareBytes() == 32 * M,
          "poolspare: the first new pool is the game's own call; one spare (32 MB) requested");
    Drain();
    GpuPoolSpareTestCounts(&taken, &made, &under, &back, &noRoom, &broken);
    Check(made == 1 && GpuPoolSpareTestReady(a.b) == 1, "poolspare: the thread makes the spare with the game's CreateNewPool");

    // 2. next growth: taken from the spare, the game's index where Init puts it
    const int createsBefore = g_createCalls;
    got[n++] = GpuPoolSpareTestCall(a.b, 7, 32 * M, 1);
    GpuPoolSpareTestCounts(&taken, &made, &under, &back, &noRoom, &broken);
    Check(got[1] && got[1] != got[0] && g_createCalls == createsBefore && taken == 1 && under == 1 && Indexed(got[1], 7) &&
              SizeOf(got[1]) == 32 * M,
          "poolspare: the next new pool comes from the spare (no create under the lock), index 7 in pool, head and free block");
    Check(GpuPoolSpareTestReady(a.b) == 0 && GpuPoolSpareBytes() == 32 * M, "poolspare: a new spare is requested after the take");
    Drain();
    Check(GpuPoolSpareTestReady(a.b) == 1, "poolspare: ... and made");

    // 3. a minimum above the default size: the game's call (power-of-two pool), the spare stays
    got[n++] = GpuPoolSpareTestCall(a.b, 8, 40 * M, 1);
    GpuPoolSpareTestCounts(&taken, &made, &under, &back, &noRoom, &broken);
    Check(got[2] && taken == 1 && under == 2 && SizeOf(got[2]) == 64 * M && GpuPoolSpareTestReady(a.b) == 1,
          "poolspare: a pool larger than the default size is the game's call; the spare stays");

    // 4. another resource type: not served by the first type's spare; its own spare
    got[n++] = GpuPoolSpareTestCall(a.b, 9, 4096, 4);
    Drain();
    GpuPoolSpareTestCounts(&taken, &made, &under, &back, &noRoom, &broken);
    Check(got[3] && taken == 1 && GpuPoolSpareTestReady(a.b) == 2 && GpuPoolSpareBytes() == 64 * M,
          "poolspare: another resource type gets its own spare (one per allocator and type)");

    // 5. not video memory: the game's call, no spare
    got[n++] = GpuPoolSpareTestCall(up.b, 1, 4096, 1);
    Drain();
    Check(got[4] && GpuPoolSpareTestReady(up.b) == 0, "poolspare: an upload-heap allocator gets no spare");

    // 6. a lower cap (96 MB budget -> 48 MB of spares): the least recently grown spare goes first
    const int destroyBefore = g_destroyCalls, deleteBefore = g_deleteCalls;
    SetGpuPoolSpare(true, 96, 0);
    Drain();
    GpuPoolSpareTestCounts(&taken, &made, &under, &back, &noRoom, &broken);
    Check(GpuPoolSpareBytes() == 32 * M && GpuPoolSpareTestReady(nullptr) == 1 && back == 1 &&
              g_destroyCalls == destroyBefore + 1 && g_deleteCalls == deleteBefore + 1,
          "poolspare: over a lower cap the oldest spare is given back (Destroy, then the deleting destructor)");
    // 7. b grows: no room -> a's spare (grew longer ago) is given back for b's
    got[n++] = GpuPoolSpareTestCall(b.b, 0, 4096, 1);
    Drain();
    Check(got[5] && GpuPoolSpareTestReady(b.b) == 1 && GpuPoolSpareTestReady(a.b) == 0 && GpuPoolSpareBytes() == 32 * M,
          "poolspare: a request without room gives back the spare of the allocator that grew least recently");
    got[n++] = GpuPoolSpareTestCall(b.b, 2, 4096, 1);
    Check(got[6] && Indexed(got[6], 2), "poolspare: b's next pool comes from its spare");
    Drain();

    // 8. what counts as untouched
    {
        void* t = FakeCreate(a.b, 0x7FFF, 0, 1);
        Check(GpuPoolSpareTestUntouched(t, 32 * M, 1), "poolspare: a fresh pool with the sentinel index is untouched");
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(t) + 0x38) = 1;
        Check(!GpuPoolSpareTestUntouched(t, 32 * M, 1), "poolspare: a pool with an allocated block is not taken");
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(t) + 0x38) = 0;
        Check(!GpuPoolSpareTestUntouched(t, 64 * M, 1) && !GpuPoolSpareTestUntouched(t, 32 * M, 2),
              "poolspare: another size or resource type is not taken");
        *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(t) + 0x28) = 16 * M;
        Check(!GpuPoolSpareTestUntouched(t, 32 * M, 1), "poolspare: a pool with less free space than its size is not taken");
        *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(t) + 0x28) = 32 * M;
        *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(t) + 0x4C) = 5;
        Check(!GpuPoolSpareTestUntouched(t, 32 * M, 1), "poolspare: a head block with another index is not taken");
        *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(t) + 0x4C) = 0x7FFF;
        *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(t) + 0xD8) = 12;
        Check(!GpuPoolSpareTestUntouched(t, 32 * M, 1), "poolspare: a pool with a last-used fence is not taken");
        *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(t) + 0xD8) = 0;
        void** vt = *reinterpret_cast<void***>(t);
        *reinterpret_cast<void***>(t) = nullptr;
        Check(!GpuPoolSpareTestUntouched(t, 32 * M, 1), "poolspare: a pool with another vtable is not taken");
        *reinterpret_cast<void***>(t) = vt;
        FakeDelete(t, 1);
    }

    // 9. off: every spare given back, then the game's call with nothing requested
    SetGpuPoolSpare(false, 96, 0);
    Drain();
    GpuPoolSpareTestCounts(&taken, &made, &under, &back, &noRoom, &broken);
    Check(GpuPoolSpareTestReady(nullptr) == 0 && GpuPoolSpareBytes() == 0 && !broken, "poolspare: off gives every spare back");
    got[n++] = GpuPoolSpareTestCall(a.b, 4, 4096, 1);
    Drain();
    Check(got[7] && GpuPoolSpareTestReady(nullptr) == 0 && GpuPoolSpareBytes() == 0, "poolspare: off = the game's call, nothing requested");

    // 10. cap 0 (MemoryRelief / video memory tight): nothing requested
    SetGpuPoolSpare(true, 0, 0);
    got[n++] = GpuPoolSpareTestCall(a.b, 5, 4096, 1);
    Drain();
    GpuPoolSpareTestCounts(&taken, &made, &under, &back, &noRoom, &broken);
    Check(got[8] && GpuPoolSpareTestReady(nullptr) == 0 && GpuPoolSpareBytes() == 0 && noRoom >= 1,
          "poolspare: with no budget (relief / tight) no spare is made");

    // 11. quiet wait (GpuPoolSpareQuietMs 40): no spare is made within 40 ms of a growth, then it is
    SetGpuPoolSpare(true, 256, 40);
    got[n++] = GpuPoolSpareTestCall(a.b, 6, 4096, 1);
    uint32_t retry = 0;
    const bool moreQuiet = GpuPoolSpareTestWork(&retry);
    Check(got[9] && !moreQuiet && retry >= 1 && retry <= 41 && GpuPoolSpareTestReady(a.b) == 0 && GpuPoolSpareBytes() == 32 * M,
          "poolspare: within the quiet time after a growth the spare waits (requested, not made; the thread wakes after the rest)");
    Sleep(50);
    Drain();
    Check(GpuPoolSpareTestReady(a.b) == 1, "poolspare: ... and is made once no pool was made for the quiet time");
    SetGpuPoolSpare(true, 256, 0);

    // 12. a ready spare that no longer looks as Init left it (review N2): the game's own call, key off, the pool left alone
    {
        void* const spare = g_lastMade;
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(spare) + 0x38) = 1;  // an allocated block
        const int createsBefore2 = g_createCalls;
        uint64_t under0 = 0;
        GpuPoolSpareTestCounts(&taken, &made, &under0, &back, &noRoom, &broken);
        got[n++] = GpuPoolSpareTestCall(a.b, 10, 4096, 1);
        GpuPoolSpareTestCounts(&taken, &made, &under, &back, &noRoom, &broken);
        Check(got[10] && got[10] != spare && g_createCalls == createsBefore2 + 1 && under == under0 + 1 && broken &&
                  GpuPoolSpareTestReady(nullptr) == 0 && GpuPoolSpareBytes() == 0,
              "poolspare: a touched spare is not handed out: the game's call, the key off, the slot and its bytes cleared");
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(spare) + 0x38) = 0;
        FakeDelete(spare, 1);  // in the game it is leaked (bounded: one)
    }
    Drain();

    // 13. the game's end (review S1): the first allocator deletion stops all game calls of ours; spares die with their allocator
    GpuPoolSpareTestSetup(reinterpret_cast<void*>(&FakeCreate), g_vtbl, reinterpret_cast<void*>(&FakeAllocDtor));
    SetGpuPoolSpare(true, 256, 0);
    FakeAlloc c(32 * M, 1), d(32 * M, 1);
    got[n++] = GpuPoolSpareTestCall(a.b, 11, 4096, 1);
    got[n++] = GpuPoolSpareTestCall(b.b, 12, 4096, 1);
    Drain();
    {
        const int destroyBefore2 = g_destroyCalls, dtorBefore = g_allocDtorCalls, createsBefore3 = g_createCalls;
        GpuPoolSpareTestBusy(true);  // as if our thread were making a spare (the wait is 50 ms in the self-test)
        const uint64_t t0 = GetTickCount64();
        void* const r = GpuPoolSpareTestDtor(a.b, 1);
        const uint64_t waitedMs = GetTickCount64() - t0;
        Check(r == a.b && g_allocDtorCalls == dtorBefore + 1 && GpuPoolSpareTestEnding() && g_destroyCalls == destroyBefore2 + 1 &&
                  GpuPoolSpareTestReady(a.b) == 0 && GpuPoolSpareTestReady(b.b) == 1 && GpuPoolSpareBytes() == 32 * M && waitedMs >= 30 &&
                  GpuPoolSpareTestEndTimedOut(),
              "poolspare: deleting an allocator waits (bounded) for our thread, destroys its spare, then calls the game's destructor");
        got[n++] = GpuPoolSpareTestCall(b.b, 13, 4096, 1);
        Check(got[n - 1] && g_createCalls == createsBefore3 + 1 && GpuPoolSpareTestReady(b.b) == 1 && !GpuPoolSpareTestWork(),
              "poolspare: after the end every new pool is the game's call, nothing is requested, our thread does nothing");
        const uint64_t t1 = GetTickCount64();
        GpuPoolSpareTestDtor(b.b, 1);  // our thread still "stuck": the run-out wait is latched (review2 F1)
        Check(GetTickCount64() - t1 < 30 && GpuPoolSpareTestReady(nullptr) == 0 && GpuPoolSpareBytes() == 0 &&
                  g_destroyCalls == destroyBefore2 + 2 && g_allocDtorCalls == dtorBefore + 2,
              "poolspare: after one run-out wait the next deletions do not wait again; their spares go with them");
        GpuPoolSpareTestBusy(false);
    }
    // 14. the end while our thread makes a spare for another allocator: the spare is parked, and destroyed with its allocator
    GpuPoolSpareTestSetup(reinterpret_cast<void*>(&FakeCreate), g_vtbl, reinterpret_cast<void*>(&FakeAllocDtor));
    SetGpuPoolSpare(true, 256, 0);
    got[n++] = GpuPoolSpareTestCall(c.b, 0, 4096, 1);
    {
        const int destroyBefore3 = g_destroyCalls;
        g_dtorDuringCreate = d.b;  // d is deleted while c's spare is being made (same thread here: the wait runs out after 50 ms)
        Drain();
        Check(GpuPoolSpareTestEnding() && GpuPoolSpareTestReady(c.b) == 1 && g_destroyCalls == destroyBefore3,
              "poolspare: a spare finished after the end is parked (our thread calls no game code to destroy it)");
        GpuPoolSpareTestDtor(c.b, 1);
        Check(GpuPoolSpareTestReady(nullptr) == 0 && GpuPoolSpareBytes() == 0 && g_destroyCalls == destroyBefore3 + 1,
              "poolspare: ... and destroyed when its allocator is deleted");
    }
    // 15. the normal end (review2 F4), two threads: our thread is inside the game's call for a spare of e; e is deleted on the main
    // thread; our thread leaves its call during the wait (10 ms), parks the spare; the deletion then destroys it, without a time-out
    GpuPoolSpareTestSetup(reinterpret_cast<void*>(&FakeCreate), g_vtbl, reinterpret_cast<void*>(&FakeAllocDtor));
    GpuPoolSpareTestEndWait(2000);
    SetGpuPoolSpare(true, 256, 0);
    FakeAlloc e(32 * M, 1);
    got[n++] = GpuPoolSpareTestCall(e.b, 0, 4096, 1);
    {
        const int destroyBefore4 = g_destroyCalls, dtorBefore4 = g_allocDtorCalls;
        g_createEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_createRelease = release;
        HANDLE worker = CreateThread(nullptr, 0, &DrainThread, nullptr, 0, nullptr);
        const bool entered = worker && WaitForSingleObject(g_createEntered, 2000) == WAIT_OBJECT_0;
        HANDLE releaser = entered ? CreateThread(nullptr, 0, &ReleaseAfter10ms, release, 0, nullptr) : nullptr;
        const uint64_t t0 = GetTickCount64();
        void* const r = entered && releaser ? GpuPoolSpareTestDtor(e.b, 1) : nullptr;
        const uint64_t waitedMs = GetTickCount64() - t0;
        if (!releaser) SetEvent(release);
        if (worker) WaitForSingleObject(worker, 5000);
        if (releaser) WaitForSingleObject(releaser, 5000);
        Check(entered && r == e.b && waitedMs >= 5 && waitedMs < 1000 && !GpuPoolSpareTestEndTimedOut() &&
                  g_destroyCalls == destroyBefore4 + 1 && g_allocDtorCalls == dtorBefore4 + 1 && GpuPoolSpareTestReady(nullptr) == 0 &&
                  GpuPoolSpareBytes() == 0 && GpuPoolSpareTestEnding(),
              "poolspare: the end while our thread makes a spare: the deletion waits until it leaves the call (no time-out), then "
              "destroys the parked spare with its allocator");
        if (worker) CloseHandle(worker);
        if (releaser) CloseHandle(releaser);
        CloseHandle(release);
        CloseHandle(g_createEntered);
        g_createEntered = nullptr;
        g_createRelease = nullptr;
    }

    GpuPoolSpareTestEnd();
    for (int i = 0; i < n; ++i)
        if (got[i]) FakeDelete(got[i], 1);
    Check(g_live == 0, "poolspare: every fake pool made was handed out or destroyed (no spare leaked)");
}

}  // namespace psp
