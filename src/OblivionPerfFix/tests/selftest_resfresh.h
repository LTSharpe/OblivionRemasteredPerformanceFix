// ResidencyFreshNew self-test, included by selftest.cpp: BeginTrackingObject and FD3D12Heap::BeginTrackingResidency on the mapped
// exe; then the replacement's work and the stamp on a fake residency manager (critical section at +118h, LRU at +0E8h) with fake
// ManagedObjects (status +0, pUnderlying +8, Size +10h, LastUsedTimestamp +20h, list entry +428h), and the pool path
// (pool +0D0h -> heap; heap +10h -> device; device +100h = the manager; heap +70h = the object).
#pragma once

namespace rfn {

static int g_evicts = 0;
static HRESULT STDMETHODCALLTYPE FakeEvict(void*, UINT n, void* const* objs) { g_evicts += (n == 1 && objs && *objs) ? 1 : 100; return S_OK; }

template <class T> static T& At(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }
static LIST_ENTRY* Head(void* mgr, uint32_t off) { return &At<LIST_ENTRY>(mgr, 0xE8 + off); }
static LIST_ENTRY* Entry(void* obj) { return &At<LIST_ENTRY>(obj, 0x428); }
static void* Obj(LIST_ENTRY* e) { return reinterpret_cast<uint8_t*>(e) - 0x428; }
static void InitMgr(void* mgr)
{
    InitializeCriticalSection(&At<CRITICAL_SECTION>(mgr, 0x118));
    for (uint32_t off : {0u, 0x10u}) Head(mgr, off)->Flink = Head(mgr, off)->Blink = Head(mgr, off);
}
// the resident list from the head, as object pointers
static int Order(void* mgr, void** out, int max)
{
    int n = 0;
    for (LIST_ENTRY* e = Head(mgr, 0)->Flink; e != Head(mgr, 0) && n < max; e = e->Flink) out[n++] = Obj(e);
    return n;
}

inline void ResidencyFreshSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    uint32_t rva[2] = {};
    const bool found = ResidencyFreshTestFind(game, rva);
    Check(found, "resfresh: BeginTrackingObject (whole body, field offsets) and FD3D12Heap::BeginTrackingResidency (its tail jump), with "
                 "FD3D12MemoryPool::Init's call for its BackingHeap, found once");
    if (game.timeDateStamp == 0xF19077A4u)
        Check(found && rva[0] == 0x253E9C0 && rva[1] == 0x253EA90, "resfresh: at exe+0x253E9C0 / exe+0x253EA90 in build 19115871");
    if (found) printf("      BeginTrackingObject exe+0x%X, BeginTrackingResidency exe+0x%X\n", rva[0], rva[1]);

    alignas(16) static uint8_t mgrDev[0x100 + 0x200];  // a fake device whose +100h is the manager
    void* const mgr = mgrDev + 0x100;
    memset(mgrDev, 0, sizeof mgrDev);
    InitMgr(mgr);
    alignas(16) static uint8_t objs[5][0x440];
    memset(objs, 0, sizeof objs);
    void* const a = objs[0];
    void* const b = objs[1];
    void* const c = objs[2];
    void* const ev = objs[3];
    for (int i = 0; i < 4; ++i) {
        At<void*>(objs[i], 0x08) = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + i));
        At<uint64_t>(objs[i], 0x10) = (i + 1) * 0x100000ull;
    }
    At<int32_t>(ev, 0) = 1;  // evicted
    ResidencyFreshTestTrack(mgr, a, false);  // the game's order: head, timestamp left at 0
    ResidencyFreshTestTrack(mgr, b, true);   // fresh: tail, stamped
    ResidencyFreshTestTrack(mgr, c, false);
    ResidencyFreshTestTrack(mgr, ev, true);  // evicted: the evicted list's head either way
    ResidencyFreshTestTrack(mgr, nullptr, true);
    void* o[8];
    const int n = Order(mgr, o, 8);
    Check(n == 3 && o[0] == c && o[1] == a && o[2] == b, "resfresh: the game's order inserts at the LRU head; fresh at the tail");
    Check(At<uint64_t>(a, 0x20) == 0 && At<uint64_t>(c, 0x20) == 0 && At<uint64_t>(b, 0x20) != 0, "resfresh: only a fresh insert is stamped");
    Check(At<uint32_t>(mgr, 0xE8 + 0x20) == 3 && At<uint64_t>(mgr, 0xE8 + 0x28) == 0x600000ull && At<uint32_t>(mgr, 0xE8 + 0x24) == 1 &&
              Head(mgr, 0x10)->Flink == Entry(ev) && Entry(ev)->Flink == Head(mgr, 0x10),
          "resfresh: NumResident, ResidentSize, NumEvicted and the evicted list as the game's Insert keeps them");
    Check(Entry(c)->Blink == Head(mgr, 0) && Head(mgr, 0)->Blink == Entry(b) && Entry(b)->Blink == Entry(a), "resfresh: the list's back links");
    // the stamp: an object to the tail (ObjectReferenced's move); an evicted or untracked one is left as it is
    const bool t1 = ResidencyFreshTestTouch(mgr, c, false);
    const int n2 = Order(mgr, o, 8);
    Check(t1 && n2 == 3 && o[0] == a && o[1] == b && o[2] == c && At<uint64_t>(c, 0x20) != 0, "resfresh: a touch stamps and moves to the tail");
    Check(!ResidencyFreshTestTouch(mgr, ev, false) && Head(mgr, 0x10)->Flink == Entry(ev), "resfresh: an evicted object is not touched");
    Check(!ResidencyFreshTestTouch(mgr, objs[4], false), "resfresh: an untracked object (no underlying heap) is not touched");
    // cStartEvicted (false in UE): the library's path, Device->Evict(1, &pUnderlying)
    void* vt[40] = {};
    vt[0x118 / 8] = reinterpret_cast<void*>(&FakeEvict);
    void** fakeDevice = vt;
    At<void*>(mgr, 0xC8) = &fakeDevice;
    At<uint8_t>(mgr, 0x168) = 1;
    void* const d = objs[4];
    At<void*>(d, 0x08) = reinterpret_cast<void*>(0x2000);
    At<uint64_t>(d, 0x10) = 0x10000;
    ResidencyFreshTestTrack(mgr, d, true);
    Check(g_evicts == 1 && At<int32_t>(d, 0) == 1 && At<uint32_t>(mgr, 0xE8 + 0x24) == 2 && Order(mgr, o, 8) == 3,
          "resfresh: cStartEvicted evicts through the device and inserts in the evicted list");
    At<uint8_t>(mgr, 0x168) = 0;
    // the pool path: pool +0D0h -> heap (+70h the object, +10h the device)
    alignas(16) static uint8_t heap[0x4B8];
    alignas(16) static uint8_t pool[0xE8];
    memset(heap, 0, sizeof heap);
    memset(pool, 0, sizeof pool);
    At<void*>(pool, 0xD0) = heap;
    At<void*>(heap, 0x10) = mgrDev;
    void* const ho = heap + 0x70;
    At<void*>(ho, 0x08) = reinterpret_cast<void*>(0x3000);
    At<uint64_t>(ho, 0x10) = 64ull << 20;
    ResidencyFreshTestTrack(mgr, ho, false);  // at the head, as the game would insert a spare
    ResidencyFreshTestSetOn(false);
    Check(!ResidencyFreshTouchPool(pool, false) && Order(mgr, o, 8) == 4 && o[0] == ho, "resfresh: off (or not installed): a spare is not touched");
    ResidencyFreshTestSetOn(true);
    const bool tp = ResidencyFreshTouchPool(pool, false);
    const int n3 = Order(mgr, o, 8);
    Check(tp && n3 == 4 && o[3] == ho && At<uint64_t>(ho, 0x20) != 0, "resfresh: a held spare's heap is stamped and moved to the tail");
    At<void*>(pool, 0xD0) = nullptr;
    Check(!ResidencyFreshTouchPool(pool, false), "resfresh: a pool without a heap (a committed buffer) is not touched");
    Check(!ResidencyFreshTouchPool(nullptr, false), "resfresh: no pool, nothing touched");
    // the room rule (review F1): the new order only while the object fits under the local budget and the total room exceeds it by 256 MB
    constexpr uint64_t G = 1ull << 30, M = 1ull << 20;
    Check(ResidencyFreshTestRoomFor(10 * G, 12 * G, 1 * G, 16 * G, 64 * M), "resfresh: room at 0.83 of the local budget");
    Check(!ResidencyFreshTestRoomFor(12 * G - 32 * M, 12 * G, 1 * G, 16 * G, 64 * M), "resfresh: no room once the object would reach the local budget");
    Check(!ResidencyFreshTestRoomFor(11 * G, 12 * G, 2 * G + 900 * M, 2 * G, 64 * M) && ResidencyFreshTestRoomFor(11 * G, 12 * G, 2 * G + 500 * M, 2 * G, 64 * M),
          "resfresh: no room within 256 MB of the total (local + non-local) budget, the over-budget loop's test");
    Check(!ResidencyFreshTestRoomFor(0, 0, 0, 0, 0), "resfresh: no budget known, no room");
    Check(ResidencyFreshTestBurst(11 * G, 12 * G, 0, 16 * G, 256 * M, 20) == 3,
          "resfresh: a burst within one sample counts each admitted object (11 GB of 12: three 256 MB objects, not twenty)");
    // the hook's choice: on + room -> tail and stamped; on, no room -> the game's head insert, timestamp 0
    void* o2[8];
    alignas(16) static uint8_t hobj[2][0x440];
    memset(hobj, 0, sizeof hobj);
    for (int i = 0; i < 2; ++i) {
        At<void*>(hobj[i], 0x08) = reinterpret_cast<void*>(static_cast<uintptr_t>(0x4000 + i));
        At<uint64_t>(hobj[i], 0x10) = 64ull << 20;
    }
    ResidencyFreshTestSetOn(true);
    ResidencyFreshTestSetRoom(0);
    ResidencyFreshTestHook(mgr, hobj[0]);
    const int n4 = Order(mgr, o2, 8);
    Check(n4 >= 1 && o2[0] == hobj[0] && At<uint64_t>(hobj[0], 0x20) == 0 && !ResidencyFreshHeldRoom(),
          "resfresh: near the budget the hook keeps the game's order (head, not stamped) and held spares are not stamped");
    ResidencyFreshTestSetRoom(1);
    ResidencyFreshTestHook(mgr, hobj[1]);
    const int n5 = Order(mgr, o2, 8);
    Check(n5 == n4 + 1 && o2[n5 - 1] == hobj[1] && At<uint64_t>(hobj[1], 0x20) != 0 && ResidencyFreshHeldRoom(),
          "resfresh: with room the hook puts a new object at the tail, stamped; held spares may be stamped");
    // a held spare's stamp only tries the lock (F5): busy -> nothing done, no wait
    {
        CRITICAL_SECTION* const mcs = &At<CRITICAL_SECTION>(mgr, 0x118);
        HANDLE in = CreateEventW(nullptr, TRUE, FALSE, nullptr), out = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        struct Arg { CRITICAL_SECTION* cs; HANDLE in, out; } arg{mcs, in, out};
        HANDLE th = CreateThread(nullptr, 0, [](void* p) -> DWORD {
            Arg* a = static_cast<Arg*>(p);
            EnterCriticalSection(a->cs);
            SetEvent(a->in);
            WaitForSingleObject(a->out, 5000);
            LeaveCriticalSection(a->cs);
            return 0;
        }, &arg, 0, nullptr);
        const bool held = th && WaitForSingleObject(in, 5000) == WAIT_OBJECT_0;
        At<void*>(pool, 0xD0) = heap;
        const uint64_t before = At<uint64_t>(ho, 0x20);
        const bool busyTouch = ResidencyFreshTouchPool(pool, true);
        Check(held && !busyTouch && At<uint64_t>(ho, 0x20) == before, "resfresh: a held spare's stamp skips a busy residency lock (no wait)");
        SetEvent(out);
        if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
        CloseHandle(in);
        CloseHandle(out);
        Check(ResidencyFreshTouchPool(pool, true), "resfresh: a held spare's stamp takes a free residency lock");
    }
    ResidencyFreshTestSetRoom(-1);
    ResidencyFreshTestSetOn(false);
    DeleteCriticalSection(&At<CRITICAL_SECTION>(mgr, 0x118));
}

}  // namespace rfn
