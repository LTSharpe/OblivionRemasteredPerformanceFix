// ResidencyFreshNew (worker s089fix, 2026-11-28; S-089 / S-001): video memory the game has just made counts as just used in
// Unreal's D3D12 residency manager, so it is not evicted the moment it is made and paged back in at its first use.
//
// UE 5.3's D3D12 RHI keeps its video memory resident with Microsoft's residency library (D3DX12Residency.h, with Epic's changes;
// the same policy in UE 5.4.4, 5.5.4, 5.6.1 and 5.7.4). Each heap or committed resource it tracks is a ManagedObject in an LRU list
// (head = least recently used). In every ExecuteCommandLists' paging work, while the game's video memory is above 70 % of its DXGI
// budget, TrimAgedAllocations walks the list from the head and evicts every object whose LastUsedTimestamp is older than a grace
// period (1 s at the full budget up to 60 s at 70 %; ~17 s at the 0.92 seen on the Kvatch road). A newly tracked object is put at
// the HEAD with LastUsedTimestamp 0 (BeginTrackingObject; ManagedObject's constructor), so it counts as unused since the PC
// started: unless the very next submission references it, it is evicted at once. Its first real use then makes it resident again
// (EnqueueMakeResident) and the submitting queue waits on the GPU for that paging; near the budget that paging sits behind other
// paging work (a new 64 MB pool's own residency, 55-60 ms measured on the dev PC) and the whole frame stalls: the RHI thread spins
// in the frame-generation swap chain's Present waiting for the GPU (80-96 ms frames on the Kvatch road; every such second shows
// Evict 64 MB + EnqueueMakeResident 64 MB next to the 64 MB pool that was made). GpuPoolSpareAhead's spare pools, which wait unused
// until the game takes one, were always evicted this way.
//
// Fix: BeginTrackingObject is replaced by the same work under the same lock (the manager's critical section, cStartEvicted honoured,
// the same counters), except that a RESIDENT object gets LastUsedTimestamp = now - 10 ms (the QueryPerformanceCounter clock the library
// uses) and goes to the TAIL of the LRU list: being made counts as a use, as a reference by a command list does (ObjectReferenced
// moves an object to the tail; ProcessPagingWork stamps it with the same clock). GpuPoolSpareAhead's held spares are stamped
// the same way (under that lock) while they wait and when the game takes one, so a spare is resident when it is used.
// Only with room (review F1): the library has two evictors. TrimAgedAllocations (usage above 70 % of the local budget) walks the LRU
// from the head by age; this is the one the fix is for. MakeResident's over-budget loop (TrimToSyncPointInclusive: local + non-local
// usage plus the batch over local + non-local budget) also walks from the head and evicts what the GPU has finished with; there a
// new object or held spare at the tail could not go first, so older data would go instead. So the new order is used only while a
// fresh DXGI sample (at most 100 ms old) shows that the object still fits under the local budget AND the total room is at least
// 256 MB more than the object: then TrimAgedAllocations, not the over-budget loop, is the evictor. At or over that line a new object
// takes the game's own place (head, timestamp 0) and held spares are not stamped, so in the over-budget loop they go first, as in
// the stock game. Earlier stamps fade by themselves: every object the game uses moves behind them at its next reference (the LRU),
// so within a few frames they are near the head again. No sample yet: the game's own order. A spare the game takes is stamped
// whatever the room (the take is a use: the game allocates from it at once).
// What does not change: what is drawn and when (residency only decides where memory lives and whether a queue waits for paging);
// the trim itself (pressure, grace periods, order of everything else) and the over-budget loop are the library's. An object nobody
// uses is still evicted once it is older than the grace period.
// Bound: memory the game made stays resident at most one grace period longer (1-60 s); spares are capped by GpuPoolSpareAhead.
//
// Found: BeginTrackingObject by its whole body (byte pattern with the field offsets literal; both critical-section calls through
// the import table of EnterCriticalSection / LeaveCriticalSection), and FD3D12Heap::BeginTrackingResidency, which tail-jumps to it
// (it gives the ManagedObject inside FD3D12Heap at +0x70, the device at +0x10 and the manager at device +0x100). A missing pattern
// or a different target: one log line, nothing installed.
//
// [Fixes] ResidencyFreshNew = 0: not installed (start-up); 1: installed, then live (0 = the game's own insert at the head).
#include "common.h"

#include <windows.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstring>

namespace opf {
IDXGIAdapter3* GameAdapter();  // memory_monitor.cpp: the game's GPU once it renders (not AddRef'd)
namespace {

// ResidencyManagerInternal::BeginTrackingObject(ManagedObject*), exe+0x253E9C0 in build 19115871: lea rbx,[rcx+118h] (Mutex);
// EnterCriticalSection; if (obj) { if cStartEvicted (+168h): status = 1, Device (+0C8h)->Evict(1, &obj->pUnderlying);
// LRU (+0E8h).Insert: resident -> InsertHeadList(Resident head +0E8h), NumResident (+108h)++, ResidentSize (+110h) += Size;
// evicted -> InsertHeadList(Evicted head +0F8h), NumEvicted (+10Ch)++ } LeaveCriticalSection.
constexpr char kTrack[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 99 18 01 00 00 48 8B FA 48 8B F1 48 85 DB 74 09 48 8B CB FF 15 ?? ?? ?? ?? "
    "48 85 FF 74 77 80 BE 68 01 00 00 00 74 1F C7 07 01 00 00 00 4C 8D 47 08 48 8B 8E C8 00 00 00 BA 01 00 00 00 48 8B 01 "
    "FF 90 18 01 00 00 83 3F 00 48 8D 96 E8 00 00 00 4C 8D 87 28 04 00 00 75 21 49 89 50 08 48 8B 02 49 89 00 48 8B 02 4C 89 40 08 "
    "FF 42 20 4C 89 02 48 8B 47 10 48 01 42 28 EB 1B 48 8D 4A 10 49 89 48 08 48 8B 01 49 89 00 48 8B 01 4C 89 40 08 FF 42 24 "
    "4C 89 01 48 85 DB 74 09 48 8B CB FF 15 ?? ?? ?? ??";
constexpr size_t kTrackEnterCall = 0x24, kTrackLeaveCall = 0xAE;  // the two FF 15 calls
// FD3D12Heap::BeginTrackingResidency(Size), exe+0x253EA90: if GEnableResidencyManagement { handle = this+70h; handle.pUnderlying
// (+78h) = Heap (+20h); Size (+80h); LastGPUSyncPoint (+88h) = 0; then jmp BeginTrackingObject(Parent (+10h) + 100h, handle) }
constexpr char kHeapTrack[] =
    "80 3D ?? ?? ?? ?? 00 4C 8D 41 70 74 ?? 48 8B 41 20 48 89 41 78 48 89 91 80 00 00 00 48 C7 81 88 00 00 00 00 00 00 00 "
    "80 3D ?? ?? ?? ?? 00 74 ?? 48 8B 49 10 49 8B D0 48 81 C1 00 01 00 00 E9";
constexpr size_t kHeapTrackJmp = 0x3E;
// FD3D12MemoryPool::Init: mov rcx,[rbx+0D0h] (BackingHeap); call BeginTrackingResidency (the pool's heap, after SetHeap)
constexpr char kPoolHeapTrack[] = "48 8B 55 FF 48 8B 8B D0 00 00 00 E8";
constexpr size_t kPoolHeapTrackCall = 0x0B;

// ResidencyManagerInternal / ManagedObject / FD3D12Heap / FD3D12MemoryPool fields (build 19115871; the patterns pin them)
constexpr uint32_t kMgrMutex = 0x118, kMgrStartEvicted = 0x168, kMgrDevice = 0xC8, kLru = 0xE8;
constexpr uint32_t kLruResident = 0x00, kLruEvicted = 0x10, kLruNumResident = 0x20, kLruNumEvicted = 0x24, kLruResidentSize = 0x28;
constexpr uint32_t kObjStatus = 0x00, kObjUnderlying = 0x08, kObjSize = 0x10, kObjLastUsed = 0x20, kObjEntry = 0x428;
constexpr uint32_t kHeapParent = 0x10, kHeapHandle = 0x70, kDeviceResidency = 0x100, kPoolBackingHeap = 0xD0;
constexpr int32_t kResident = 0, kEvicted = 1;

using EvictFn = HRESULT(STDMETHODCALLTYPE*)(void* device, UINT n, void* const* objects);  // ID3D12Device::Evict, vtable +118h

std::atomic<bool> g_on{false};
bool g_installed = false;
std::atomic<uint64_t> g_fresh{0}, g_stock{0}, g_near{0}, g_touched{0}, g_touchEvicted{0}, g_touchNear{0}, g_touchBusy{0};

// ---- the room rule (review F1) ----
constexpr uint64_t kTotalRoom = 256ull << 20;  // room the over-budget loop must still have beyond the object
constexpr ULONGLONG kSampleMs = 100;           // DXGI sample age at most
struct Vram {
    uint64_t lu = 0, lb = 0, nu = 0, nb = 0;  // local used / budget, non-local used / budget (bytes)
};
SRWLOCK g_vlock = SRWLOCK_INIT;  // a leaf lock (never held while the residency manager's lock is taken)
Vram g_v;
bool g_vOk = false;
ULONGLONG g_vAt = 0;
std::atomic<int> g_testRoom{-1};  // self-test: -1 the real sample, 0 no room, 1 room

// True when `size` more bytes leave TrimAgedAllocations as the library's evictor (not the over-budget loop): the object fits
// under the local budget, and local + non-local room exceeds it by kTotalRoom (MakeResident's AvailableSpace test).
bool RoomFor(const Vram& v, uint64_t size)
{
    if (!v.lb || v.lu + size >= v.lb) return false;
    return v.lu + v.nu + size + kTotalRoom <= v.lb + v.nb;
}
// Called outside the residency manager's lock: a DXGI query at most every 100 ms (the library queries the same per submission).
bool Roomy(uint64_t size)
{
    const int t = g_testRoom.load(std::memory_order_relaxed);
    if (t >= 0) return t == 1;
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_vlock);
    const ULONGLONG at = g_vAt;
    ReleaseSRWLockShared(&g_vlock);
    if (now - at >= kSampleMs) {
        Vram v;
        bool ok = false;
        if (IDXGIAdapter3* const ad = GameAdapter()) {
            DXGI_QUERY_VIDEO_MEMORY_INFO l{}, n{};
            if (SUCCEEDED(ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l)) &&
                SUCCEEDED(ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &n))) {
                v.lu = l.CurrentUsage, v.lb = l.Budget, v.nu = n.CurrentUsage, v.nb = n.Budget;
                ok = true;
            }
        }
        AcquireSRWLockExclusive(&g_vlock);
        g_v = v;
        g_vOk = ok;
        g_vAt = now;
        ReleaseSRWLockExclusive(&g_vlock);
    }
    // Every object admitted counts against the cached sample until the next one (review revs089b (a)): a burst of creates within
    // one 100 ms sample (a loading screen) cannot overshoot the rule by its own size. Counted twice at most for 100 ms if the sample
    // already held it (conservative).
    AcquireSRWLockExclusive(&g_vlock);
    const bool room = g_vOk && RoomFor(g_v, size);
    if (room) g_v.lu += size;
    ReleaseSRWLockExclusive(&g_vlock);
    return room;
}

template <class T> T& F(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }

// The stamp: QPC now minus 10 ms. ProcessPagingWork reads its CurrentTime BEFORE it takes the manager's lock, so a submit that
// waited for our lock may trim with a CurrentTime a little older than our stamp; its unsigned "CurrentTime - LastUsedTimestamp"
// would then wrap and count the object as aged. 10 ms earlier keeps the difference positive (grace periods are 1-60 s).
uint64_t Stamp()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    const uint64_t back = static_cast<uint64_t>(f.QuadPart) / 100;
    const uint64_t now = static_cast<uint64_t>(t.QuadPart);
    return now > back ? now - back : 1;
}
void InsertHead(LIST_ENTRY* head, LIST_ENTRY* e)
{
    e->Flink = head->Flink;
    e->Blink = head;
    head->Flink->Blink = e;
    head->Flink = e;
}
void InsertTail(LIST_ENTRY* head, LIST_ENTRY* e)
{
    e->Flink = head;
    e->Blink = head->Blink;
    head->Blink->Flink = e;
    head->Blink = e;
}

// The replacement: BeginTrackingObject's work, the same lock; fresh = stamp + tail for a resident object.
void Track(void* mgr, void* obj, bool fresh)
{
    CRITICAL_SECTION* const cs = reinterpret_cast<CRITICAL_SECTION*>(static_cast<uint8_t*>(mgr) + kMgrMutex);
    EnterCriticalSection(cs);
    if (obj) {
        if (F<uint8_t>(mgr, kMgrStartEvicted)) {  // cStartEvicted (false in UE): as the library does
            F<int32_t>(obj, kObjStatus) = kEvicted;
            void* const device = F<void*>(mgr, kMgrDevice);
            reinterpret_cast<EvictFn>((*static_cast<void***>(device))[0x118 / 8])(device, 1, &F<void*>(obj, kObjUnderlying));
        }
        uint8_t* const lru = static_cast<uint8_t*>(mgr) + kLru;
        LIST_ENTRY* const e = &F<LIST_ENTRY>(obj, kObjEntry);
        if (F<int32_t>(obj, kObjStatus) == kResident) {
            if (fresh) {
                F<uint64_t>(obj, kObjLastUsed) = Stamp();
                InsertTail(reinterpret_cast<LIST_ENTRY*>(lru + kLruResident), e);
            } else {
                InsertHead(reinterpret_cast<LIST_ENTRY*>(lru + kLruResident), e);
            }
            ++F<uint32_t>(lru, kLruNumResident);
            F<uint64_t>(lru, kLruResidentSize) += F<uint64_t>(obj, kObjSize);
        } else {
            InsertHead(reinterpret_cast<LIST_ENTRY*>(lru + kLruEvicted), e);
            ++F<uint32_t>(lru, kLruNumEvicted);
        }
    }
    LeaveCriticalSection(cs);
}

void TrackHook(void* mgr, void* obj)
{
    const bool on = g_on.load(std::memory_order_relaxed);
    const bool fresh = on && obj && Roomy(F<uint64_t>(obj, kObjSize));  // the caller set Size before the call
    Track(mgr, obj, fresh);
    (fresh ? g_fresh : on ? g_near : g_stock).fetch_add(1, std::memory_order_relaxed);
}

// A tracked object counts as used now: stamped and moved to the tail (ObjectReferenced's move), under the manager's lock.
// False when it is not tracked (no underlying object, not in a list) or evicted (then left as it is). tryOnly (review F5): the
// lock is not waited for (a submission's paging work can hold it through a GPU wait); *busy = it was held, nothing done.
bool Touch(void* mgr, void* obj, bool tryOnly = false, bool* busy = nullptr)
{
    CRITICAL_SECTION* const cs = reinterpret_cast<CRITICAL_SECTION*>(static_cast<uint8_t*>(mgr) + kMgrMutex);
    if (busy) *busy = false;
    if (!tryOnly) {
        EnterCriticalSection(cs);
    } else if (!TryEnterCriticalSection(cs)) {
        if (busy) *busy = true;
        return false;
    }
    LIST_ENTRY* const e = &F<LIST_ENTRY>(obj, kObjEntry);
    bool done = false;
    if (F<void*>(obj, kObjUnderlying) && e->Flink && e->Blink && F<int32_t>(obj, kObjStatus) == kResident) {
        F<uint64_t>(obj, kObjLastUsed) = Stamp();
        e->Blink->Flink = e->Flink;  // RemoveEntryList
        e->Flink->Blink = e->Blink;
        InsertTail(reinterpret_cast<LIST_ENTRY*>(static_cast<uint8_t*>(mgr) + kLru + kLruResident), e);
        done = true;
    }
    LeaveCriticalSection(cs);
    return done;
}

// The two FF 15 calls go through import slots holding EnterCriticalSection / LeaveCriticalSection (no C++ objects: __try).
bool LockCallsOk(const uint8_t* track, const char** why)
{
    void* const* const enter = reinterpret_cast<void* const*>(RipTarget(track + kTrackEnterCall, 2, 6));
    void* const* const leave = reinterpret_cast<void* const*>(RipTarget(track + kTrackLeaveCall, 2, 6));
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    void* const e = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "EnterCriticalSection")) : nullptr;
    void* const l = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "LeaveCriticalSection")) : nullptr;
    __try {
        if (e && l && FollowJumps(static_cast<const uint8_t*>(*enter)) == FollowJumps(static_cast<const uint8_t*>(e)) &&
            FollowJumps(static_cast<const uint8_t*>(*leave)) == FollowJumps(static_cast<const uint8_t*>(l)))
            return true;
        *why = "BeginTrackingObject's lock calls are not EnterCriticalSection / LeaveCriticalSection";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "BeginTrackingObject's import slots are not readable";
    }
    return false;
}

struct Found {
    uint8_t* track = nullptr;
    uint8_t* heapTrack = nullptr;
};

bool Find(const GameModule& game, Found* f, bool checkImports, const char** why)
{
    const auto t = FindAll(game.text, ParsePattern(kTrack));
    if (t.size() != 1) { *why = t.empty() ? "BeginTrackingObject not found" : "BeginTrackingObject found more than once"; return false; }
    if (checkImports && !LockCallsOk(t[0], why)) return false;  // the running game: its import slots hold the two functions
    const auto h = FindAll(game.text, ParsePattern(kHeapTrack));
    if (h.size() != 1 || RipTarget(h[0] + kHeapTrackJmp, 1, 5) != t[0]) {
        *why = "FD3D12Heap::BeginTrackingResidency (its jump to BeginTrackingObject) not found";
        return false;
    }
    bool poolCall = false;  // FD3D12MemoryPool::Init tracks its BackingHeap (+0D0h)
    for (uint8_t* p : FindAll(game.text, ParsePattern(kPoolHeapTrack)))
        if (RipTarget(p + kPoolHeapTrackCall, 1, 5) == h[0]) poolCall = true;
    if (!poolCall) { *why = "FD3D12MemoryPool::Init's BeginTrackingResidency call (BackingHeap) not found"; return false; }
    f->track = t[0];
    f->heapTrack = h[0];
    return true;
}

bool Patch(uint8_t* at, const char** why)
{
    uint8_t* const stub = AllocNearGame(16);
    if (!stub) { *why = "no memory near the game's code"; return false; }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]; dq TrackHook
    void* const target = reinterpret_cast<void*>(&TrackHook);
    memcpy(stub, jmpAbs, sizeof jmpAbs);
    memcpy(stub + sizeof jmpAbs, &target, sizeof target);
    DWORD old = 0;
    if (!VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &old)) { FreeOwnCode(stub); *why = "stub protection"; return false; }
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const int64_t d = stub - (at + 5);
    if (d != static_cast<int32_t>(d)) { FreeOwnCode(stub); *why = "out of reach"; return false; }
    uint8_t jmp[5] = {0xE9};
    const int32_t d32 = static_cast<int32_t>(d);
    memcpy(jmp + 1, &d32, 4);
    if (!WriteCode(at, jmp) || memcmp(at, jmp, 5) != 0) { *why = "the code write failed"; return false; }  // the stub stays
    return true;
}

}  // namespace

bool InstallResidencyFresh(bool beforeGameCode, int key)
{
    if (g_installed) return true;
    if (!key) {
        Log("[resfresh] ResidencyFreshNew=0 in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {  // the D3D12 device and its residency manager may already be in use
        Log("[resfresh] ResidencyFreshNew: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    const GameModule& game = Game();
    Found f;
    const char* why = "";
    if (!Find(game, &f, true, &why) || !Patch(f.track, &why)) {
        Log("[resfresh] ResidencyFreshNew not installed (the game's way): %s", why);
        return false;
    }
    g_installed = true;
    g_on.store(true, std::memory_order_relaxed);
    Log("[resfresh] ResidencyFreshNew installed: BeginTrackingObject exe+0x%X, FD3D12Heap::BeginTrackingResidency exe+0x%X; video memory "
        "the game makes counts as just used in the residency manager (no eviction the moment it is made)",
        static_cast<unsigned>(f.track - game.base), static_cast<unsigned>(f.heapTrack - game.base));
    return true;
}

void SetResidencyFresh(int key)
{
    const bool on = key != 0;
    const bool was = g_on.exchange(on && g_installed);
    if (g_installed && was != on)
        Log(on ? "[resfresh] ResidencyFreshNew on: new video memory counts as just used" : "[resfresh] ResidencyFreshNew off: the game's own order");
}

bool ResidencyFreshActive() { return g_installed && g_on.load(std::memory_order_relaxed); }

// GpuPoolSpareAhead's wake, before it takes its lock: may the held spares be stamped now? Not near the budget (the room rule): in
// the over-budget loop they then go first, as in the stock game.
bool ResidencyFreshHeldRoom()
{
    if (!ResidencyFreshActive()) return false;
    if (Roomy(0)) return true;
    g_touchNear.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// held: a spare GpuPoolSpareAhead holds (its wake; the lock is only tried), else one the game has just taken (waited for, as the
// game's own order allocator lock -> residency lock).
// No __try here (review F2): a fault inside would leave the manager's lock held and hang the next submission without a dump. The
// pool and its heap are live game objects (held by GpuPoolSpareAhead under its lock, or just taken by the game in the allocator's lock).
bool ResidencyFreshTouchPool(void* pool, bool held)
{
    if (!ResidencyFreshActive() || !pool) return false;
    void* const heap = F<void*>(pool, kPoolBackingHeap);
    if (!heap) return false;  // a committed-buffer pool: tracked through its resource, not touched here
    void* const device = F<void*>(heap, kHeapParent);
    if (!device) return false;
    void* const obj = static_cast<uint8_t*>(heap) + kHeapHandle;
    bool busy = false;
    const bool done = Touch(static_cast<uint8_t*>(device) + kDeviceResidency, obj, held, &busy);
    if (done) g_touched.fetch_add(1, std::memory_order_relaxed);
    else if (busy) g_touchBusy.fetch_add(1, std::memory_order_relaxed);
    else if (F<void*>(obj, kObjUnderlying) && F<int32_t>(obj, kObjStatus) == kEvicted) g_touchEvicted.fetch_add(1, std::memory_order_relaxed);
    return done;
}

void LogResidencyFreshStats()
{
    if (!g_installed) return;
    const uint64_t fresh = g_fresh.exchange(0), stock = g_stock.exchange(0), nearB = g_near.exchange(0), touched = g_touched.exchange(0),
                   ev = g_touchEvicted.exchange(0), tnear = g_touchNear.exchange(0), busy = g_touchBusy.exchange(0);
    if (!fresh && !stock && !nearB && !ev && !tnear) return;
    Log("[resfresh] %llu new tracked objects counted as just used, %llu in the game's order near the budget (%llu while off); "
        "%llu spare-pool stamps, %llu spares found evicted, %llu spare-stamp rounds skipped near the budget, %llu stamps skipped with the lock busy",
        static_cast<unsigned long long>(fresh), static_cast<unsigned long long>(nearB), static_cast<unsigned long long>(stock),
        static_cast<unsigned long long>(touched), static_cast<unsigned long long>(ev), static_cast<unsigned long long>(tnear),
        static_cast<unsigned long long>(busy));
}

// ---- self-test ----
bool ResidencyFreshTestFind(const GameModule& game, uint32_t rva[2])
{
    Found f;
    const char* why = "";
    if (!Find(game, &f, false, &why)) return false;
    rva[0] = static_cast<uint32_t>(f.track - game.base);
    rva[1] = static_cast<uint32_t>(f.heapTrack - game.base);
    return true;
}
void ResidencyFreshTestTrack(void* mgr, void* obj, bool fresh) { Track(mgr, obj, fresh); }
bool ResidencyFreshTestTouch(void* mgr, void* obj, bool tryOnly) { return Touch(mgr, obj, tryOnly); }
void ResidencyFreshTestSetRoom(int room) { g_testRoom.store(room, std::memory_order_relaxed); }
bool ResidencyFreshTestRoomFor(uint64_t lu, uint64_t lb, uint64_t nu, uint64_t nb, uint64_t size)
{
    Vram v;
    v.lu = lu, v.lb = lb, v.nu = nu, v.nb = nb;
    return RoomFor(v, size);
}
void ResidencyFreshTestHook(void* mgr, void* obj) { TrackHook(mgr, obj); }
int ResidencyFreshTestBurst(uint64_t lu, uint64_t lb, uint64_t nu, uint64_t nb, uint64_t size, int maxN)
{
    AcquireSRWLockExclusive(&g_vlock);
    g_v.lu = lu, g_v.lb = lb, g_v.nu = nu, g_v.nb = nb;
    g_vOk = true;
    g_vAt = GetTickCount64();  // a fresh sample: no query within the burst
    ReleaseSRWLockExclusive(&g_vlock);
    int n = 0;
    while (n < maxN && Roomy(size)) ++n;
    AcquireSRWLockExclusive(&g_vlock);
    g_v = Vram();
    g_vOk = false;
    g_vAt = 0;
    ReleaseSRWLockExclusive(&g_vlock);
    return n;
}
void ResidencyFreshTestSetOn(bool on)
{
    g_installed = on;
    g_on.store(on, std::memory_order_relaxed);
}

}  // namespace opf
