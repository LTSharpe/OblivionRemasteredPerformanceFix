// GpuMemoryRecord ([General] GpuMemoryRecord, default 0; start-up only; diagnostics for GPU crash reports, worker pubdiagD,
// 2026-11-28, S-082 / issue #54).
//
// Why: #54's GPU crashes are DXGI_ERROR_DEVICE_HUNG with a DRED page fault, always inside one 5.6 MB window of GPU virtual
// addresses, and DRED lists no live and no recently freed allocation there. DRED keeps only a short list of freed objects, so
// an object made early in the session and freed long before the fault is no longer known to it. This record keeps its own
// list of the D3D12 heaps and resources the game (and its upscaler / frame generation libraries) make and free, so that the
// GPU crash report can say what that address belonged to.
//
// What it records (the D3D12 device's own calls; nothing is changed, delayed or added to a call):
//   committed and reserved resources (CreateCommittedResource / 1 / 2 / 3, CreateReservedResource / 1 / 2) and heaps
//   (CreateHeap / 1): kind, size, buffer GPU VA, texture dimensions / format / flags, heap type, the creating call's caller
//   frames (5), the time and engine frame of creation; the last Release marks it freed (time, frame, 2 caller frames); SetName
//   copies the first 39 characters of a name (FidelityFX names its resources; UE does not in shipping builds). Placed
//   resources (hundreds per second: the engine's transient allocator) get no entry: the first buffer placed in a heap teaches
//   that heap's GPU VA range (buffer VA - heap offset), so a fault inside a heap is named by the heap.
// Textures have no GPU VA in D3D12's API: they are kept with their size and creation order, which the report uses for the
// "made between" estimate (Windows hands out a process's GPU VAs mostly in rising order).
//
// Table: 65,536 entries x 136 bytes (8.5 MiB) + an index of 131,072 words (0.5 MiB) + 1,024 names (40 KB), VirtualAlloc'd
// once at start-up. The first 8,192 entries are kept for the whole session (the start-up allocations #54's address belongs
// to); the rest are reused oldest first, never a live one (a full table counts the creations it could not record). One SRW
// lock: exclusive on a recorded creation or free (a few dozen per second outside loading), shared on a placed creation's heap
// look-up and on the final Release of an object that is not in the table.
//
// How it attaches before the device exists: the exe delay-loads d3d12.dll and imports D3D12CreateDevice by ordinal (#101);
// that import slot, and the exe's GetProcAddress import (for callers that ask for "D3D12CreateDevice" by name, such as AMD's
// AGS library linked into the exe), return a wrapper that calls the real function and, on success, patches the new device's
// vtable (the D3D12 runtime's device, heap and resource vtables are shared by every object of their class; the originals are
// kept per vtable). The delay slot's first call goes through the stock delay-load helper unchanged. Fallback: once the
// monitor thread knows the game's adapter, D3D12CreateDevice on it returns the game's device (the late route; the report then
// says how late it attached).
//
// The report (memory_monitor.cpp's DRED part calls GpuMemoryRecordReport with the page-fault VA, so it goes wherever the
// [gpucrash] report goes: the log, and OblivionPerfFix-crash.txt through CrashReport): every recorded range that ever held
// the address (newest first, at most 8; alive or freed), the nearest recorded ranges below and above it, the VA-less objects
// made between those two, the objects freed in the last 10 s, and the record's own counts. Also, with or without the record:
// the GPU and its driver version; with the record on, also the upscaler / frame generation / overlay libraries loaded, with
// their file versions and whether each is in Windows, the game folder or elsewhere (never a path).
//
// Not covered: memory the driver or the runtime allocates for itself (no D3D12 object) and swap-chain buffers (made by DXGI).
#include "common.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace opf {
IDXGIAdapter3* GameAdapter();  // memory_monitor.cpp: the game's GPU once it renders (not AddRef'd)

namespace {

// ID3D12Device vtable slots (d3d12.h order: IUnknown 0-2, ID3D12Object 3-6, ID3D12Device 7-43, Device1 44-46, Device2 47,
// Device3 48-50, Device4 51-56, Device5 57-64, Device6 65, Device7 66-67, Device8 68-72, Device9 73-75, Device10 76-78)
constexpr int kSlotCommitted = 27, kSlotHeap = 28, kSlotPlaced = 29, kSlotReserved = 30, kSlotCommitted1 = 53, kSlotHeap1 = 54,
              kSlotReserved1 = 55, kSlotCommitted2 = 69, kSlotPlaced1 = 70, kSlotCommitted3 = 76, kSlotPlaced2 = 77,
              kSlotReserved2 = 78;
constexpr int kSlotRelease = 2, kSlotSetName = 6;
constexpr int kDevSlots = 79;

constexpr uint32_t kEntries = 65536, kEarly = 8192, kIndexBits = 17, kIndexSize = 1u << kIndexBits, kNames = 1024;
constexpr int kCreateFrames = 5, kFreeFrames = 2;

enum : uint8_t { kEmpty = 0, kLive = 1, kFreed = 2, kFreedUnseen = 3 };
enum : uint8_t { kCommitted = 1, kReserved = 2, kHeap = 3 };

struct Entry {  // 136 bytes
    uint64_t obj;        // the object's interface pointer (the key while alive)
    uint64_t va;         // GPU VA of the range (buffers; heaps once a buffer was placed in them); 0 = not known
    uint64_t size;       // bytes of the range (buffers: width rounded up to 64 KB; textures: GetResourceAllocationInfo)
    uint64_t createQpc, freeQpc;
    uint32_t createFrame, freeFrame;
    uint32_t width, height;
    uint32_t flags;      // resource flags (resources), heap flags (heaps)
    uint32_t seq;        // creation order (1, 2, ...)
    uint16_t depth, format;
    uint16_t name;       // 0 = none, else g_names index + 1
    uint8_t kind, dim, heapType, mips, state, pad;
    void* create[kCreateFrames];
    void* free[kFreeFrames];
};
static_assert(sizeof(Entry) == 136, "entry size");

Entry* g_e = nullptr;
uint32_t* g_index = nullptr;  // entry index + 1 of a live object, open addressing on the pointer; 0 = empty
char (*g_names)[40] = nullptr;
uint32_t g_nameCount = 0;  // under g_lock
SRWLOCK g_lock = SRWLOCK_INIT;
uint32_t g_early = 0, g_cursor = 0;  // under g_lock
std::atomic<uint32_t> g_seq{0};
std::atomic<uint64_t> g_recorded{0}, g_frees{0}, g_reused{0}, g_dropped{0}, g_placed{0}, g_heapVa{0}, g_unseen{0};
std::atomic<uint64_t> g_otherDevices{0};
std::atomic<uint64_t> g_hookTicks{0}, g_hookCalls{0};  // QPC ticks spent in the record's own part of the hooks, and calls
bool g_installed = false;
std::atomic<bool> g_attached{false};
std::atomic<int> g_route{0};  // 1 delay import, 2 GetProcAddress, 3 late (monitor), 4 self-test
uint64_t g_attachQpc = 0;
LARGE_INTEGER g_freq{};
uint64_t g_qpc0 = 0;      // QPC at install
double g_up0 = 0;         // process up seconds at install
const volatile uint64_t* g_gframe = nullptr;

uint64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
// the time of the record's own part of a hook (the stats line's cost)
struct HookTimer {
    uint64_t t0 = Qpc();
    HookTimer() = default;
    HookTimer(const HookTimer&) = delete;
    HookTimer& operator=(const HookTimer&) = delete;
    ~HookTimer()
    {
        g_hookTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
        g_hookCalls.fetch_add(1, std::memory_order_relaxed);
    }
};
double QpcSeconds(int64_t d) { return static_cast<double>(d) / static_cast<double>(g_freq.QuadPart ? g_freq.QuadPart : 1); }
double ProcSeconds(uint64_t qpc) { return g_up0 + QpcSeconds(static_cast<int64_t>(qpc - g_qpc0)); }
uint32_t FrameNow()
{
    if (!g_gframe) return 0;
    __try {
        return static_cast<uint32_t>(*g_gframe);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uintptr_t g_selfLo = 0, g_selfHi = 0;
bool OwnFrame(const void* pc) { return reinterpret_cast<uintptr_t>(pc) >= g_selfLo && reinterpret_cast<uintptr_t>(pc) < g_selfHi; }

uintptr_t g_k32Lo = 0, g_k32Hi = 0, g_ntLo = 0, g_ntHi = 0;  // kernel32 / ntdll (a thread's start frames)
bool ThreadStartFrame(const void* pc)
{
    const auto a = reinterpret_cast<uintptr_t>(pc);
    return (a >= g_k32Lo && a < g_k32Hi) || (a >= g_ntLo && a < g_ntHi);
}

// The caller's frames: our hook's own frames skipped; when only a thread start follows them (an object made by our own code,
// e.g. a lab drill or a keep of ours), our outermost frames instead (they print as OPF+RVA)
void Frames(void** out, int want)
{
    void* raw[24];
    const USHORT n = RtlCaptureStackBackTrace(1, 24, raw, nullptr);
    int k = 0;
    while (k < n && OwnFrame(raw[k])) ++k;
    int from = k;
    if (k > 0 && (k >= n || ThreadStartFrame(raw[k]))) from = k > want ? k - want : 0;
    int m = 0;
    for (int i = from; i < n && m < want; ++i) out[m++] = raw[i];
    for (int i = m; i < want; ++i) out[i] = nullptr;
}

uint32_t Slot(uint64_t obj) { return static_cast<uint32_t>(((obj >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kIndexBits)); }

// under g_lock (shared is enough): the live entry of obj, or UINT32_MAX; *at: its index slot
uint32_t FindLive(uint64_t obj, uint32_t* at = nullptr)
{
    for (uint32_t i = Slot(obj), k = 0; k < kIndexSize; ++k, i = (i + 1) & (kIndexSize - 1)) {
        const uint32_t v = g_index[i];
        if (!v) return UINT32_MAX;
        if (g_e[v - 1].obj == obj) {
            if (at) *at = i;
            return v - 1;
        }
    }
    return UINT32_MAX;
}
// exclusive: remove index slot at (backward shift, no tombstones)
void IndexRemove(uint32_t at)
{
    uint32_t hole = at;
    for (uint32_t i = (at + 1) & (kIndexSize - 1);; i = (i + 1) & (kIndexSize - 1)) {
        const uint32_t v = g_index[i];
        if (!v) break;
        const uint32_t home = Slot(g_e[v - 1].obj);
        // v may stay only if its home lies in (hole, i] (cyclically)
        const bool stays = hole <= i ? (home > hole && home <= i) : (home > hole || home <= i);
        if (!stays) {
            g_index[hole] = v;
            hole = i;
        }
    }
    g_index[hole] = 0;
}
void IndexInsert(uint64_t obj, uint32_t entry)
{
    for (uint32_t i = Slot(obj);; i = (i + 1) & (kIndexSize - 1))
        if (!g_index[i]) {
            g_index[i] = entry + 1;
            return;
        }
}
// exclusive: a free entry (the early part first, then the oldest reusable), or UINT32_MAX
uint32_t TakeEntry()
{
    if (g_early < kEarly) return g_early++;
    for (int k = 0; k < 1024; ++k) {
        const uint32_t i = kEarly + (g_cursor++ % (kEntries - kEarly));
        if (g_e[i].state != kLive) {
            if (g_e[i].state != kEmpty) g_reused.fetch_add(1, std::memory_order_relaxed);
            return i;
        }
    }
    return UINT32_MAX;
}

struct NewObject {
    uint64_t obj = 0, va = 0, size = 0;
    uint32_t width = 0, height = 0, flags = 0;
    uint16_t depth = 0, format = 0;
    uint8_t kind = 0, dim = 0, heapType = 0, mips = 0;
};

void Record(const NewObject& n)
{
    void* frames[kCreateFrames];
    Frames(frames, kCreateFrames);
    const uint64_t now = Qpc();
    const uint32_t frame = FrameNow();
    AcquireSRWLockExclusive(&g_lock);
    uint32_t at = 0;
    const uint32_t old = FindLive(n.obj, &at);
    if (old != UINT32_MAX) {  // the pointer is in use again: the earlier object's last Release was not seen
        g_e[old].state = kFreedUnseen;
        g_e[old].freeQpc = now;
        g_e[old].freeFrame = frame;
        IndexRemove(at);
        g_unseen.fetch_add(1, std::memory_order_relaxed);
    }
    const uint32_t i = TakeEntry();
    if (i == UINT32_MAX) {
        ReleaseSRWLockExclusive(&g_lock);
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Entry& e = g_e[i];
    e = Entry{};
    e.obj = n.obj;
    e.va = n.va;
    e.size = n.size;
    e.createQpc = now;
    e.createFrame = frame;
    e.width = n.width;
    e.height = n.height;
    e.flags = n.flags;
    e.depth = n.depth;
    e.format = n.format;
    e.kind = n.kind;
    e.dim = n.dim;
    e.heapType = n.heapType;
    e.mips = n.mips;
    e.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    memcpy(e.create, frames, sizeof frames);
    e.state = kLive;
    IndexInsert(n.obj, i);
    ReleaseSRWLockExclusive(&g_lock);
    g_recorded.fetch_add(1, std::memory_order_relaxed);
}

// a final Release of obj; seqBefore: the creation counter before that Release (an object made at that address since is newer)
void Freed(uint64_t obj, uint32_t seqBefore)
{
    AcquireSRWLockShared(&g_lock);
    const bool known = FindLive(obj) != UINT32_MAX;
    ReleaseSRWLockShared(&g_lock);
    if (!known) return;
    void* frames[kFreeFrames];
    Frames(frames, kFreeFrames);
    const uint64_t now = Qpc();
    const uint32_t frame = FrameNow();
    AcquireSRWLockExclusive(&g_lock);
    uint32_t at = 0;
    const uint32_t i = FindLive(obj, &at);
    if (i != UINT32_MAX && g_e[i].seq <= seqBefore) {
        Entry& e = g_e[i];
        e.state = kFreed;
        e.freeQpc = now;
        e.freeFrame = frame;
        memcpy(e.free, frames, sizeof frames);
        IndexRemove(at);
        g_frees.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void Named(uint64_t obj, LPCWSTR name)
{
    if (!name || !name[0]) return;
    AcquireSRWLockExclusive(&g_lock);
    const uint32_t i = FindLive(obj);
    if (i != UINT32_MAX) {
        uint32_t n = g_e[i].name;
        if (!n && g_nameCount < kNames) {
            n = ++g_nameCount;
            g_e[i].name = static_cast<uint16_t>(n);
        }
        if (n) {
            int k = 0;
            for (; k < 39 && name[k]; ++k) g_names[n - 1][k] = name[k] >= 32 && name[k] < 127 ? static_cast<char>(name[k]) : '?';
            g_names[n - 1][k] = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// placed buffer: the heap's VA base, the first time
void Placed(uint64_t heap, uint64_t bufferVa, uint64_t offset)
{
    g_placed.fetch_add(1, std::memory_order_relaxed);
    if (!heap || !bufferVa || bufferVa < offset) return;
    AcquireSRWLockShared(&g_lock);
    const uint32_t i = FindLive(heap);
    const bool learn = i != UINT32_MAX && !g_e[i].va;
    ReleaseSRWLockShared(&g_lock);
    if (!learn) return;
    AcquireSRWLockExclusive(&g_lock);
    const uint32_t j = FindLive(heap);
    if (j != UINT32_MAX && !g_e[j].va) {
        g_e[j].va = bufferVa - offset;
        g_heapVa.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// ---------------------------------------------------------------------------------------------------------------- hooks
using CommittedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using HeapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
using PlacedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                             const D3D12_CLEAR_VALUE*, REFIID, void**);
using ReservedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                               REFIID, void**);
using Committed1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                                 D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, void*, REFIID, void**);
using Heap1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, void*, REFIID, void**);
using Reserved1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                                void*, REFIID, void**);
using Committed2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*,
                                                 D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, void*, REFIID, void**);
using Placed1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE*, REFIID, void**);
using Committed3Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*,
                                                 UINT, const D3D12_CLEAR_VALUE*, void*, UINT32, const void*, REFIID, void**);
using Placed2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, UINT, const D3D12_CLEAR_VALUE*,
                                              UINT32, const void*, REFIID, void**);
using Reserved2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_RESOURCE_DESC*, UINT, const D3D12_CLEAR_VALUE*, void*, UINT32,
                                                const void*, REFIID, void**);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
using SetNameFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Object*, LPCWSTR);

// one device vtable (the D3D12 runtime's; a wrapping overlay's device would be a second one)
struct DevVt {
    std::atomic<void**> vt{nullptr};
    void* orig[kDevSlots] = {};
};
DevVt g_dev[2];
// heap / resource vtables (Release and SetName), up to 8 classes
struct ObjVt {
    std::atomic<void**> vt{nullptr};
    ReleaseFn release = nullptr;
    SetNameFn setName = nullptr;
};
ObjVt g_obj[8];
SRWLOCK g_patchLock = SRWLOCK_INIT;

void* DevOrig(void* device, int slot)
{
    void** vt = *static_cast<void***>(device);
    for (DevVt& d : g_dev)
        if (d.vt.load(std::memory_order_acquire) == vt) return d.orig[slot];
    return vt[slot];  // not a patched vtable (cannot happen: the hooks are only in patched ones)
}
const ObjVt* ObjOrig(void* obj)
{
    void** vt = *static_cast<void***>(obj);
    for (const ObjVt& o : g_obj)
        if (o.vt.load(std::memory_order_acquire) == vt) return &o;
    return nullptr;
}

ULONG STDMETHODCALLTYPE ReleaseHook(IUnknown* u)
{
    const ObjVt* o = ObjOrig(u);
    const uint32_t seq = g_seq.load(std::memory_order_relaxed);
    const ULONG r = o->release(u);
    if (r == 0) {
        HookTimer timer;
        Freed(reinterpret_cast<uint64_t>(u), seq);
    }
    return r;
}
HRESULT STDMETHODCALLTYPE SetNameHook(ID3D12Object* obj, LPCWSTR name)
{
    const ObjVt* o = ObjOrig(obj);
    __try {
        Named(reinterpret_cast<uint64_t>(obj), name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return o->setName(obj, name);
}

bool Patch(void** vt, int slot, void* hook)
{
    if (vt[slot] == hook) return true;
    return WritePointer(&vt[slot], hook);
}

// a heap's or resource's class: Release and SetName go through the record (once per vtable)
void LearnObject(void* obj)
{
    if (ObjOrig(obj)) return;
    void** vt = *static_cast<void***>(obj);
    AcquireSRWLockExclusive(&g_patchLock);
    bool known = false;
    for (ObjVt& o : g_obj) known = known || o.vt.load(std::memory_order_relaxed) == vt;
    if (!known) {
        bool placed = false;
        for (ObjVt& o : g_obj) {
            if (o.vt.load(std::memory_order_relaxed)) continue;
            o.release = reinterpret_cast<ReleaseFn>(vt[kSlotRelease]);
            o.setName = reinterpret_cast<SetNameFn>(vt[kSlotSetName]);
            o.vt.store(vt, std::memory_order_release);  // the originals first, then the slots
            if (!Patch(vt, kSlotRelease, reinterpret_cast<void*>(&ReleaseHook)) || !Patch(vt, kSlotSetName, reinterpret_cast<void*>(&SetNameHook)))
                Log("[gpumem] an object class's Release / SetName could not be patched: its frees are not seen");
            placed = true;
            break;
        }
        if (!placed) Log("[gpumem] more than 8 heap / resource classes: the frees of the newest are not seen");
    }
    ReleaseSRWLockExclusive(&g_patchLock);
}

bool IsResourceIid(REFIID r)
{
    return IsEqualIID(r, __uuidof(ID3D12Resource)) || IsEqualIID(r, __uuidof(ID3D12Resource1)) || IsEqualIID(r, __uuidof(ID3D12Resource2));
}
bool IsHeapIid(REFIID r) { return IsEqualIID(r, __uuidof(ID3D12Heap)) || IsEqualIID(r, __uuidof(ID3D12Heap1)); }

uint64_t Align64K(uint64_t v) { return (v + 0xFFFF) & ~0xFFFFull; }

void CreatedGuarded(ID3D12Device* d, uint8_t kind, HRESULT hr, REFIID riid, void** ppv, const D3D12_HEAP_PROPERTIES* props, const D3D12_RESOURCE_DESC* desc)
{
    if (FAILED(hr) || !ppv || !*ppv || !desc || !IsResourceIid(riid)) return;
    __try {
        NewObject n;
        n.obj = reinterpret_cast<uint64_t>(*ppv);
        n.kind = kind;
        n.dim = static_cast<uint8_t>(desc->Dimension);
        n.flags = static_cast<uint32_t>(desc->Flags);
        n.format = static_cast<uint16_t>(desc->Format);
        n.heapType = props ? static_cast<uint8_t>(props->Type) : 0;
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            n.size = Align64K(desc->Width);
            n.va = static_cast<ID3D12Resource*>(*ppv)->GetGPUVirtualAddress();
        } else {
            n.width = static_cast<uint32_t>(desc->Width);
            n.height = desc->Height;
            n.depth = desc->DepthOrArraySize;
            n.mips = static_cast<uint8_t>(desc->MipLevels);
            n.size = d->GetResourceAllocationInfo(0, 1, desc).SizeInBytes;
        }
        LearnObject(*ppv);
        Record(n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void Created(ID3D12Device* d, uint8_t kind, HRESULT hr, REFIID riid, void** ppv, const D3D12_HEAP_PROPERTIES* props, const D3D12_RESOURCE_DESC* desc)
{
    HookTimer timer;  // the record's own part (the stats line's cost)
    CreatedGuarded(d, kind, hr, riid, ppv, props, desc);
}
void CreatedHeapGuarded(HRESULT hr, REFIID riid, void** ppv, const D3D12_HEAP_DESC* desc)
{
    if (FAILED(hr) || !ppv || !*ppv || !desc || !IsHeapIid(riid)) return;
    __try {
        NewObject n;
        n.obj = reinterpret_cast<uint64_t>(*ppv);
        n.kind = kHeap;
        n.size = desc->SizeInBytes;
        n.flags = static_cast<uint32_t>(desc->Flags);
        n.heapType = static_cast<uint8_t>(desc->Properties.Type);
        LearnObject(*ppv);
        Record(n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void CreatedHeap(HRESULT hr, REFIID riid, void** ppv, const D3D12_HEAP_DESC* desc)
{
    HookTimer timer;  // the record's own part (the stats line's cost)
    CreatedHeapGuarded(hr, riid, ppv, desc);
}
void CreatedPlacedGuarded(HRESULT hr, REFIID riid, void** ppv, ID3D12Heap* heap, UINT64 offset, D3D12_RESOURCE_DIMENSION dim)
{
    if (FAILED(hr) || !ppv || !*ppv) return;
    if (dim != D3D12_RESOURCE_DIMENSION_BUFFER || !IsResourceIid(riid)) {
        g_placed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    __try {
        Placed(reinterpret_cast<uint64_t>(heap), static_cast<ID3D12Resource*>(*ppv)->GetGPUVirtualAddress(), offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void CreatedPlaced(HRESULT hr, REFIID riid, void** ppv, ID3D12Heap* heap, UINT64 offset, D3D12_RESOURCE_DIMENSION dim)
{
    HookTimer timer;  // the record's own part (the stats line's cost)
    CreatedPlacedGuarded(hr, riid, ppv, heap, offset, dim);
}
D3D12_RESOURCE_DESC FromDesc1(const D3D12_RESOURCE_DESC1* d1)
{
    D3D12_RESOURCE_DESC d{};
    memcpy(&d, d1, sizeof d);  // DESC1 = DESC + SamplerFeedbackMipRegion at the end
    return d;
}

HRESULT STDMETHODCALLTYPE CommittedHook(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC* desc,
                                        D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<CommittedFn>(DevOrig(d, kSlotCommitted))(d, p, f, desc, s, c, r, ppv);
    Created(d, kCommitted, hr, r, ppv, p, desc);
    return hr;
}
HRESULT STDMETHODCALLTYPE Committed1Hook(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC* desc,
                                         D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, void* ses, REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<Committed1Fn>(DevOrig(d, kSlotCommitted1))(d, p, f, desc, s, c, ses, r, ppv);
    Created(d, kCommitted, hr, r, ppv, p, desc);
    return hr;
}
HRESULT STDMETHODCALLTYPE Committed2Hook(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC1* desc,
                                         D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, void* ses, REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<Committed2Fn>(DevOrig(d, kSlotCommitted2))(d, p, f, desc, s, c, ses, r, ppv);
    if (desc) {
        const D3D12_RESOURCE_DESC d0 = FromDesc1(desc);
        Created(d, kCommitted, hr, r, ppv, p, &d0);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Committed3Hook(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC1* desc,
                                         UINT layout, const D3D12_CLEAR_VALUE* c, void* ses, UINT32 nf, const void* fmts, REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<Committed3Fn>(DevOrig(d, kSlotCommitted3))(d, p, f, desc, layout, c, ses, nf, fmts, r, ppv);
    if (desc) {
        const D3D12_RESOURCE_DESC d0 = FromDesc1(desc);
        Created(d, kCommitted, hr, r, ppv, p, &d0);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE ReservedHook(ID3D12Device* d, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c,
                                       REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<ReservedFn>(DevOrig(d, kSlotReserved))(d, desc, s, c, r, ppv);
    Created(d, kReserved, hr, r, ppv, nullptr, desc);
    return hr;
}
HRESULT STDMETHODCALLTYPE Reserved1Hook(ID3D12Device* d, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c,
                                        void* ses, REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<Reserved1Fn>(DevOrig(d, kSlotReserved1))(d, desc, s, c, ses, r, ppv);
    Created(d, kReserved, hr, r, ppv, nullptr, desc);
    return hr;
}
HRESULT STDMETHODCALLTYPE Reserved2Hook(ID3D12Device* d, const D3D12_RESOURCE_DESC* desc, UINT layout, const D3D12_CLEAR_VALUE* c, void* ses,
                                        UINT32 nf, const void* fmts, REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<Reserved2Fn>(DevOrig(d, kSlotReserved2))(d, desc, layout, c, ses, nf, fmts, r, ppv);
    Created(d, kReserved, hr, r, ppv, nullptr, desc);
    return hr;
}
HRESULT STDMETHODCALLTYPE HeapHook(ID3D12Device* d, const D3D12_HEAP_DESC* desc, REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<HeapFn>(DevOrig(d, kSlotHeap))(d, desc, r, ppv);
    CreatedHeap(hr, r, ppv, desc);
    return hr;
}
HRESULT STDMETHODCALLTYPE Heap1Hook(ID3D12Device* d, const D3D12_HEAP_DESC* desc, void* ses, REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<Heap1Fn>(DevOrig(d, kSlotHeap1))(d, desc, ses, r, ppv);
    CreatedHeap(hr, r, ppv, desc);
    return hr;
}
HRESULT STDMETHODCALLTYPE PlacedHook(ID3D12Device* d, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s,
                                     const D3D12_CLEAR_VALUE* c, REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<PlacedFn>(DevOrig(d, kSlotPlaced))(d, h, off, desc, s, c, r, ppv);
    if (desc) CreatedPlaced(hr, r, ppv, h, off, desc->Dimension);
    return hr;
}
HRESULT STDMETHODCALLTYPE Placed1Hook(ID3D12Device* d, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES s,
                                      const D3D12_CLEAR_VALUE* c, REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<Placed1Fn>(DevOrig(d, kSlotPlaced1))(d, h, off, desc, s, c, r, ppv);
    if (desc) CreatedPlaced(hr, r, ppv, h, off, desc->Dimension);
    return hr;
}
HRESULT STDMETHODCALLTYPE Placed2Hook(ID3D12Device* d, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC1* desc, UINT layout,
                                      const D3D12_CLEAR_VALUE* c, UINT32 nf, const void* fmts, REFIID r, void** ppv)
{
    const HRESULT hr = reinterpret_cast<Placed2Fn>(DevOrig(d, kSlotPlaced2))(d, h, off, desc, layout, c, nf, fmts, r, ppv);
    if (desc) CreatedPlaced(hr, r, ppv, h, off, desc->Dimension);
    return hr;
}

int TopInterface(ID3D12Device* device, void** vt)
{
    int top = 0;
    const IID ids[] = {__uuidof(ID3D12Device4), __uuidof(ID3D12Device8), __uuidof(ID3D12Device10)};
    const int tops[] = {4, 8, 10};
    for (int i = 0; i < 3; ++i) {
        IUnknown* u = nullptr;
        if (SUCCEEDED(device->QueryInterface(ids[i], reinterpret_cast<void**>(&u))) && u) {
            if (*reinterpret_cast<void***>(u) == vt) top = tops[i];
            u->Release();
        }
    }
    return top;
}

const char* RouteName(int r)
{
    return r == 1 ? "attached through the exe's D3D12CreateDevice import" : r == 2 ? "attached through GetProcAddress(\"D3D12CreateDevice\")"
         : r == 3 ? "attached late (the game's adapter, monitor thread)" : r == 4 ? "attached by the self-test" : "not attached";
}

// patches a device's vtable (once per vtable)
void Attach(ID3D12Device* device, int route)
{
    if (!device || !g_installed) return;
    void** vt = *reinterpret_cast<void***>(device);
    for (DevVt& d : g_dev)
        if (d.vt.load(std::memory_order_acquire) == vt) return;
    AcquireSRWLockExclusive(&g_patchLock);
    DevVt* slot = nullptr;
    bool known = false;
    for (DevVt& d : g_dev) {
        known = known || d.vt.load(std::memory_order_relaxed) == vt;
        if (!slot && !d.vt.load(std::memory_order_relaxed)) slot = &d;
    }
    if (known || !slot) {
        if (!known) g_otherDevices.fetch_add(1, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&g_patchLock);
        return;
    }
    const int top = TopInterface(device, vt);
    const int last = top >= 10 ? 78 : top >= 8 ? 72 : top >= 4 ? 56 : 43;
    for (int i = 0; i < kDevSlots; ++i) slot->orig[i] = i <= last ? vt[i] : nullptr;
    slot->vt.store(vt, std::memory_order_release);  // the originals first, then the slots
    bool ok = Patch(vt, kSlotCommitted, reinterpret_cast<void*>(&CommittedHook)) && Patch(vt, kSlotHeap, reinterpret_cast<void*>(&HeapHook)) &&
              Patch(vt, kSlotPlaced, reinterpret_cast<void*>(&PlacedHook)) && Patch(vt, kSlotReserved, reinterpret_cast<void*>(&ReservedHook));
    if (ok && top >= 4)
        ok = Patch(vt, kSlotCommitted1, reinterpret_cast<void*>(&Committed1Hook)) && Patch(vt, kSlotHeap1, reinterpret_cast<void*>(&Heap1Hook)) &&
             Patch(vt, kSlotReserved1, reinterpret_cast<void*>(&Reserved1Hook));
    if (ok && top >= 8)
        ok = Patch(vt, kSlotCommitted2, reinterpret_cast<void*>(&Committed2Hook)) && Patch(vt, kSlotPlaced1, reinterpret_cast<void*>(&Placed1Hook));
    if (ok && top >= 10)
        ok = Patch(vt, kSlotCommitted3, reinterpret_cast<void*>(&Committed3Hook)) && Patch(vt, kSlotPlaced2, reinterpret_cast<void*>(&Placed2Hook)) &&
             Patch(vt, kSlotReserved2, reinterpret_cast<void*>(&Reserved2Hook));
    const bool first = slot == &g_dev[0];
    if (first) {
        g_attachQpc = Qpc();
        g_route.store(route, std::memory_order_relaxed);
        g_attached.store(true, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_patchLock);
    Log("[gpumem] %s: D3D12 device class %d (interfaces up to ID3D12Device%d), %s %.1f s after the game started",
        ok ? "recording" : "PARTLY recording (a slot could not be written)", first ? 1 : 2, top, RouteName(route), ProcSeconds(Qpc()));
}

// the device creation routes
using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
void** g_delaySlot = nullptr;
std::atomic<CreateDeviceFn> g_delayOrig{nullptr};  // the stock delay-load thunk until it ran, then the real function
std::atomic<CreateDeviceFn> g_gpaReal{nullptr};
using GpaFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GpaFn g_realGpa = nullptr;

void AttachResult(HRESULT hr, void** ppv, int route)
{
    if (FAILED(hr) || !ppv || !*ppv) return;
    __try {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppv)->QueryInterface(IID_PPV_ARGS(&d))) && d) {
            Attach(d, route);
            d->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool InD3d12(const void* p)
{
    HMODULE m = GetModuleHandleW(L"d3d12.dll");
    if (!m || !p) return false;
    const auto* b = reinterpret_cast<const uint8_t*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(m)->e_lfanew);
    return p >= static_cast<const void*>(b) && p < static_cast<const void*>(b + nt->OptionalHeader.SizeOfImage);
}

HRESULT WINAPI DelayCreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** ppv)
{
    const CreateDeviceFn orig = g_delayOrig.load(std::memory_order_acquire);
    const HRESULT hr = orig(adapter, fl, riid, ppv);
    // the stock delay-load helper resolved the import and wrote the real function into the slot: keep it, and the hook
    if (g_delaySlot && *g_delaySlot != reinterpret_cast<void*>(&DelayCreateDevice) && InD3d12(*g_delaySlot)) {
        g_delayOrig.store(reinterpret_cast<CreateDeviceFn>(*g_delaySlot), std::memory_order_release);
        WritePointer(g_delaySlot, reinterpret_cast<void*>(&DelayCreateDevice));
    }
    AttachResult(hr, ppv, 1);
    return hr;
}
HRESULT WINAPI GpaCreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** ppv)
{
    const HRESULT hr = g_gpaReal.load(std::memory_order_acquire)(adapter, fl, riid, ppv);
    AttachResult(hr, ppv, 2);
    return hr;
}
FARPROC WINAPI GpaHook(HMODULE m, LPCSTR name)
{
    FARPROC r = g_realGpa(m, name);
    if (r && reinterpret_cast<uintptr_t>(name) > 0xFFFF && strcmp(name, "D3D12CreateDevice") == 0) {
        CreateDeviceFn expected = nullptr;
        const auto real = reinterpret_cast<CreateDeviceFn>(r);
        if (g_gpaReal.compare_exchange_strong(expected, real) || expected == real) return reinterpret_cast<FARPROC>(&GpaCreateDevice);
    }
    return r;
}

// the delay-import slot of dll's ordinal (or name)
void** FindDelaySlot(uint8_t* base, const char* dll, uint16_t ordinal, const char* name)
{
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (!dd.VirtualAddress) return nullptr;
        for (const auto* desc = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dd.VirtualAddress); desc->DllNameRVA; ++desc) {
            if (_stricmp(reinterpret_cast<const char*>(base + desc->DllNameRVA), dll) != 0) continue;
            const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->ImportNameTableRVA);
            for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
                const bool hit = IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)
                                     ? IMAGE_ORDINAL64(names[i].u1.Ordinal) == ordinal
                                     : strcmp(reinterpret_cast<const char*>(
                                                  reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData)->Name),
                                              name) == 0;
                if (hit) return reinterpret_cast<void**>(base + desc->ImportAddressTableRVA) + i;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

constexpr size_t kTableBytes = sizeof(Entry) * kEntries + sizeof(uint32_t) * kIndexSize + 40 * kNames;
bool AllocTables()
{
    if (g_e) return true;
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, kTableBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p) return false;
    g_e = reinterpret_cast<Entry*>(p);
    g_index = reinterpret_cast<uint32_t*>(p + sizeof(Entry) * kEntries);
    g_names = reinterpret_cast<char(*)[40]>(p + sizeof(Entry) * kEntries + sizeof(uint32_t) * kIndexSize);
    return true;
}

void InitClock()
{
    QueryPerformanceFrequency(&g_freq);
    g_qpc0 = Qpc();
    FILETIME created, exited, kernel, user, now;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        GetSystemTimeAsFileTime(&now);
        const uint64_t c = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        const uint64_t n = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        g_up0 = n > c ? static_cast<double>(n - c) / 1e7 : 0.0;
    }
    const auto* self = reinterpret_cast<const uint8_t*>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(self + __ImageBase.e_lfanew);
    g_selfLo = reinterpret_cast<uintptr_t>(self);
    g_selfHi = g_selfLo + nt->OptionalHeader.SizeOfImage;
    const wchar_t* const starts[] = {L"kernel32.dll", L"ntdll.dll"};
    uintptr_t* const ranges[][2] = {{&g_k32Lo, &g_k32Hi}, {&g_ntLo, &g_ntHi}};
    for (int i = 0; i < 2; ++i)
        if (const auto* m = reinterpret_cast<const uint8_t*>(GetModuleHandleW(starts[i]))) {
            const auto* mnt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m + reinterpret_cast<const IMAGE_DOS_HEADER*>(m)->e_lfanew);
            *ranges[i][0] = reinterpret_cast<uintptr_t>(m);
            *ranges[i][1] = *ranges[i][0] + mnt->OptionalHeader.SizeOfImage;
        }
}

// ---------------------------------------------------------------------------------------------------------------- system facts
// The GPU, its driver and the graphics libraries loaded, taken by the monitor thread (the report only prints them).
char g_sys[2][1600];
std::atomic<int> g_sysCur{-1};
DWORD g_sysModules = 0;
char g_gpuLine[320] = "";

void FileVersion(HMODULE m, char* out, size_t size)
{
    __try {
        HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));  // RT_VERSION
        HGLOBAL g = r ? LoadResource(m, r) : nullptr;
        const auto* p = g ? static_cast<const uint8_t*>(LockResource(g)) : nullptr;
        const DWORD n = r ? SizeofResource(m, r) : 0;
        for (DWORD i = 0; p && i + 52 <= n; i += 4) {
            uint32_t sig;
            memcpy(&sig, p + i, 4);
            if (sig != 0xFEEF04BD) continue;
            uint32_t ms, ls;
            memcpy(&ms, p + i + 8, 4);
            memcpy(&ls, p + i + 12, 4);
            sprintf_s(out, size, "%u.%u.%u.%u", ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    strcpy_s(out, size, "no version");
}

// graphics-related libraries: upscalers, frame generation, overlays and wrappers, the D3D12 runtime, the GPU drivers
bool Interesting(const char* lower)
{
    static const char* const keys[] = {"fidelityfx", "ffx", "fsr", "xess", "dlss", "nvngx", "sl.", "streamline", "optiscaler",
                                       "reshade", "dxgi", "d3d12", "amdxc", "amd_ags", "atidxx", "nvwgf2um", "nvldumd", "igd12um",
                                       "igxe", "dstorage", "gameoverlay", "rtss", "nvapi", "amdfrt", "winmm", "nukem", "dxvk",
                                       "vkd3d", "libxell", "overlay", "oblivionperffix", "version.dll", "ue4ss", "obse"};
    for (const char* k : keys)
        if (strstr(lower, k)) return true;
    return false;
}

void SnapshotSystem()
{
    if (!g_installed) return;  // the libraries only with GpuMemoryRecord on (GpuCrashReport alone names no other program)
    static HMODULE mods[768];  // monitor thread only
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return;
    if (needed == g_sysModules && g_sysCur.load() >= 0) return;
    g_sysModules = needed;
    const int next = g_sysCur.load() == 0 ? 1 : 0;
    char* out = g_sys[next];
    size_t len = 0;
    out[0] = 0;
    const DWORD count = needed / sizeof(HMODULE) < 768 ? needed / static_cast<DWORD>(sizeof(HMODULE)) : 768;
    wchar_t winDir[MAX_PATH] = L"", exeDir[MAX_PATH] = L"";
    GetSystemWindowsDirectoryW(winDir, MAX_PATH);
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(exeDir, L'\\')) *slash = 0;
    const size_t wl = wcslen(winDir), el = wcslen(exeDir);
    for (DWORD i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH];
        if (!K32GetModuleFileNameExW(GetCurrentProcess(), mods[i], path, MAX_PATH)) continue;
        const wchar_t* file = wcsrchr(path, L'\\');
        file = file ? file + 1 : path;
        char name[96] = "", lower[96] = "";
        WideCharToMultiByte(CP_UTF8, 0, file, -1, name, sizeof name, nullptr, nullptr);
        name[sizeof name - 1] = 0;
        for (size_t k = 0; k < sizeof lower && name[k]; ++k) lower[k] = static_cast<char>(tolower(static_cast<unsigned char>(name[k])));
        lower[sizeof lower - 1] = 0;
        if (!Interesting(lower)) continue;
        // where it is, never the path (it holds the Windows user name)
        const char* where = wl && _wcsnicmp(path, winDir, wl) == 0 ? "Windows"
                          : el && _wcsnicmp(path, exeDir, el) == 0 ? (wcschr(path + el + 1, L'\\') ? "game subfolder" : "game folder")
                                                                   : "elsewhere";
        char ver[48];
        FileVersion(mods[i], ver, sizeof ver);
        const int n = sprintf_s(out + len, sizeof g_sys[0] - len, "%s%s %s (%s)", len ? ", " : "", name, ver, where);
        if (n <= 0) break;
        len += static_cast<size_t>(n);
        if (len > sizeof g_sys[0] - 140) break;
    }
    g_sysCur.store(next, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------------------------- report
// the libraries line, split at ", " into lines under the log's 1024-character limit
void LibraryLines(const char* all)
{
    if (!all[0]) {
        Log("[gpucrash] graphics libraries loaded: none of note");
        return;
    }
    const size_t total = strlen(all);
    int parts = 0;
    for (size_t at = 0; at < total; ++parts) {
        size_t end = total - at > 860 ? at + 860 : total;
        if (end < total) {
            for (size_t k = end; k > at + 100; --k)
                if (all[k] == ',' && all[k + 1] == ' ') { end = k; break; }
        }
        Log("[gpucrash] graphics libraries loaded%s: %.*s", parts ? " (continued)" : "", static_cast<int>(end - at), all + at);
        at = end < total ? end + 2 : end;
    }
}
const char* HeapTypeName(int t)
{
    switch (t) {
    case D3D12_HEAP_TYPE_DEFAULT: return "default";
    case D3D12_HEAP_TYPE_UPLOAD: return "upload";
    case D3D12_HEAP_TYPE_READBACK: return "readback";
    case D3D12_HEAP_TYPE_CUSTOM: return "custom";
    case 5: return "GPU upload";
    default: return "-";
    }
}

struct ModName {
    uintptr_t lo, hi;
    char name[40];
};
ModName g_mods[24];
int g_modCount = 0;

void FrameText(const void* pc, char* out, size_t size)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(pc);
    const GameModule& g = Game();
    const uintptr_t gb = reinterpret_cast<uintptr_t>(g.base);
    if (gb && a >= gb && a < gb + g.sizeOfImage) {
        sprintf_s(out, size, "exe+0x%llX", static_cast<unsigned long long>(a - gb));
        return;
    }
    if (a >= g_selfLo && a < g_selfHi) {
        sprintf_s(out, size, "OPF+0x%llX", static_cast<unsigned long long>(a - g_selfLo));
        return;
    }
    for (int i = 0; i < g_modCount; ++i)
        if (a >= g_mods[i].lo && a < g_mods[i].hi) {
            sprintf_s(out, size, "%s+0x%llX", g_mods[i].name, static_cast<unsigned long long>(a - g_mods[i].lo));
            return;
        }
    __try {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(pc, &mbi, sizeof mbi) && mbi.Type == MEM_IMAGE && mbi.AllocationBase) {
            const auto* base = static_cast<const uint8_t*>(mbi.AllocationBase);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            wchar_t path[MAX_PATH];
            char name[40] = "module";
            if (K32GetMappedFileNameW(GetCurrentProcess(), mbi.AllocationBase, path, MAX_PATH)) {
                const wchar_t* f = wcsrchr(path, L'\\');
                WideCharToMultiByte(CP_UTF8, 0, f ? f + 1 : path, -1, name, sizeof name, nullptr, nullptr);
                name[sizeof name - 1] = 0;
            }
            if (g_modCount < 24) {
                ModName& m = g_mods[g_modCount++];
                m.lo = reinterpret_cast<uintptr_t>(base);
                m.hi = m.lo + nt->OptionalHeader.SizeOfImage;
                strcpy_s(m.name, name);
            }
            sprintf_s(out, size, "%s+0x%llX", name, static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(base)));
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    sprintf_s(out, size, "0x%llX", static_cast<unsigned long long>(a));
}

void FramesText(void* const* f, int n, char* out, size_t size)
{
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && f[i]; ++i) {
        char one[64];
        FrameText(f[i], one, sizeof one);
        const int k = sprintf_s(out + len, size - len, "%s%s", len ? " < " : "", one);
        if (k <= 0) break;
        len += static_cast<size_t>(k);
    }
    if (!len) strcpy_s(out, size, "(no frames)");
}

double SizeMB(uint64_t b) { return static_cast<double>(b) / 1048576.0; }

// one entry: what it is, when it was made and freed, by whom
void Describe(const Entry& e, uint64_t nowQpc, char* out, size_t size)
{
    char what[200];
    const char* kind = e.kind == kHeap ? "heap" : e.kind == kReserved ? "reserved" : "committed";
    if (e.kind == kHeap)
        sprintf_s(what, "heap %.2f MB (%s, heap flags 0x%X)", SizeMB(e.size), HeapTypeName(e.heapType), e.flags);
    else if (e.dim == D3D12_RESOURCE_DIMENSION_BUFFER)
        sprintf_s(what, "%s buffer %.2f MB (%s heap, flags 0x%X)", kind, SizeMB(e.size), e.kind == kReserved ? "no" : HeapTypeName(e.heapType),
                  e.flags);
    else
        sprintf_s(what, "%s texture%dD %ux%u x%u, %u mips, DXGI format %u, flags 0x%X, %.2f MB%s%s", kind, e.dim >= 2 ? e.dim - 1 : 0, e.width,
                  e.height, static_cast<unsigned>(e.depth), static_cast<unsigned>(e.mips), static_cast<unsigned>(e.format), e.flags, SizeMB(e.size),
                  e.kind == kCommitted ? ", " : "", e.kind == kCommitted ? HeapTypeName(e.heapType) : "");
    char range[64] = "";
    if (e.va) sprintf_s(range, " [0x%llX-0x%llX)", static_cast<unsigned long long>(e.va), static_cast<unsigned long long>(e.va + e.size));
    char name[48] = "";
    if (e.name && e.name <= kNames) sprintf_s(name, " \"%s\"", g_names[e.name - 1]);
    char made[256], gone[300] = "alive";
    FramesText(e.create, kCreateFrames, made, sizeof made);
    if (e.state == kFreed || e.state == kFreedUnseen) {
        char by[180];
        if (e.state == kFreed) FramesText(e.free, kFreeFrames, by, sizeof by);
        else strcpy_s(by, "(its last Release was not seen; the address was used again)");
        sprintf_s(gone, "FREED at %.1f s (frame %u, %.1f s before this report) by %s", ProcSeconds(e.freeQpc), e.freeFrame,
                  QpcSeconds(static_cast<int64_t>(nowQpc - e.freeQpc)), by);
    }
    sprintf_s(out, size, "#%u %s%s%s, made at %.1f s (frame %u) by %s; %s", e.seq, what, range, name, ProcSeconds(e.createQpc), e.createFrame,
              made, gone);
}

// the creator's module of an entry ("exe", "OPF", "name.dll"; "-" without frames)
void CreatorModule(const Entry& e, char* out, size_t cap)
{
    if (!e.create[0]) {
        strcpy_s(out, cap, "-");
        return;
    }
    char f[64];
    FrameText(e.create[0], f, sizeof f);
    char* plus = strchr(f, '+');
    if (plus) *plus = 0;
    strcpy_s(out, cap, f);
}

// The heaps / resources freed in the last 10 s, grouped by their creator's module: the counts per module, then up to 16
// entries, the rarest creators first (one module's per-frame buffers, e.g. an overlay's, last), newest first in each
void FreedRecently(uint64_t now, char* line, size_t lineCap)
{
    const uint64_t since = now - static_cast<uint64_t>(g_freq.QuadPart) * 10;
    struct Group {
        char mod[40];
        int count;
    };
    static Group groups[16];
    static uint32_t recent[256];
    int ng = 0, nr = 0, all = 0;
    for (uint32_t i = 0; i < kEntries; ++i) {
        const Entry& e = g_e[i];
        if ((e.state != kFreed && e.state != kFreedUnseen) || e.freeQpc < since) continue;
        ++all;
        if (nr < 256) recent[nr++] = i;
    }
    for (int a = 0; a < nr; ++a)
        for (int b = a + 1; b < nr; ++b)
            if (g_e[recent[b]].freeQpc > g_e[recent[a]].freeQpc) {
                const uint32_t t = recent[a];
                recent[a] = recent[b];
                recent[b] = t;
            }
    int groupOf[256];
    for (int k = 0; k < nr; ++k) {
        char mod[40];
        CreatorModule(g_e[recent[k]], mod, sizeof mod);
        int g = 0;
        while (g < ng && strcmp(groups[g].mod, mod) != 0) ++g;
        if (g == ng && ng < 16) {
            strcpy_s(groups[ng].mod, mod);
            groups[ng].count = 0;
            ++ng;
        }
        if (g == 16) g = 15;
        ++groups[g].count;
        groupOf[k] = g;
    }
    char counts[400] = "";
    size_t len = 0;
    for (int g = 0; g < ng; ++g) {
        const int n = sprintf_s(counts + len, sizeof counts - len, "%s%s %d", g ? ", " : "", groups[g].mod, groups[g].count);
        if (n <= 0) break;
        len += static_cast<size_t>(n);
    }
    Log("[gpucrash] freed in the last 10 s: %d recorded heaps / resources%s%s%s", all, ng ? " (by creator: " : "", counts, ng ? ")" : "");
    // groups by count, smallest first
    int order[16];
    for (int g = 0; g < ng; ++g) order[g] = g;
    for (int a = 0; a < ng; ++a)
        for (int b = a + 1; b < ng; ++b)
            if (groups[order[b]].count < groups[order[a]].count) {
                const int t = order[a];
                order[a] = order[b];
                order[b] = t;
            }
    int shown = 0;
    for (int o = 0; o < ng && shown < 16; ++o)
        for (int k = 0; k < nr && shown < 16; ++k) {
            if (groupOf[k] != order[o]) continue;
            Describe(g_e[recent[k]], now, line, lineCap);
            Log("[gpucrash]   %s", line);
            ++shown;
        }
    if (shown < all) Log("[gpucrash]   (%d more not shown)", all - shown);
}

bool LockForReport()
{
    const ULONGLONG until = GetTickCount64() + 200;
    while (!TryAcquireSRWLockShared(&g_lock)) {
        if (GetTickCount64() >= until) return false;
        Sleep(1);
    }
    return true;
}

void SortBySeqDesc(uint32_t* v, int n)
{
    for (int a = 0; a < n; ++a)
        for (int b = a + 1; b < n; ++b)
            if (g_e[v[b]].seq > g_e[v[a]].seq) {
                const uint32_t t = v[a];
                v[a] = v[b];
                v[b] = t;
            }
}

void ReportLocked(uint64_t va)
{
    const uint64_t now = Qpc();
    uint32_t used = 0, live = 0, withVa = 0;
    for (uint32_t i = 0; i < kEntries; ++i) {
        const Entry& e = g_e[i];
        if (e.state == kEmpty) continue;
        ++used;
        live += e.state == kLive;
        withVa += e.va != 0;
    }
    const bool att = g_attached.load(std::memory_order_acquire);
    Log("[gpucrash] GPU memory record: %s; %u entries (%u alive, %u with a GPU VA), %llu made, %llu freed, %llu placed resources (%llu heaps' VA "
        "learned), %llu entries reused, %llu not recorded (table full), %llu frees not seen; the first %u entries are kept for the session",
        RouteName(att ? g_route.load() : 0), used, live, withVa, static_cast<unsigned long long>(g_recorded.load()),
        static_cast<unsigned long long>(g_frees.load()), static_cast<unsigned long long>(g_placed.load()),
        static_cast<unsigned long long>(g_heapVa.load()), static_cast<unsigned long long>(g_reused.load()),
        static_cast<unsigned long long>(g_dropped.load()), static_cast<unsigned long long>(g_unseen.load()), kEarly);
    if (att)
        Log("[gpucrash] GPU memory record attached %.1f s after the game started%s", ProcSeconds(g_attachQpc),
            g_route.load() == 3 ? ": allocations made before that are not known" : "");
    char line[1000];
    if (va) {
        // every range that ever held the address, newest first
        uint32_t hits[8];
        int nh = 0, total = 0;
        for (uint32_t i = 0; i < kEntries; ++i) {
            const Entry& e = g_e[i];
            if (e.state == kEmpty || !e.va || va < e.va || va >= e.va + e.size) continue;
            ++total;
            if (nh < 8) hits[nh++] = i;
            else {
                int oldest = 0;
                for (int k = 1; k < 8; ++k)
                    if (g_e[hits[k]].seq < g_e[hits[oldest]].seq) oldest = k;
                if (e.seq > g_e[hits[oldest]].seq) hits[oldest] = i;
            }
        }
        SortBySeqDesc(hits, nh);
        if (total) Log("[gpucrash] VA 0x%llX: %d recorded range(s) held it%s", static_cast<unsigned long long>(va), total, total > nh ? " (the newest 8)" : "");
        for (int k = 0; k < nh; ++k) {
            Describe(g_e[hits[k]], now, line, sizeof line);
            Log("[gpucrash] VA 0x%llX was inside (offset 0x%llX): %s", static_cast<unsigned long long>(va),
                static_cast<unsigned long long>(va - g_e[hits[k]].va), line);
        }
        if (!nh) Log("[gpucrash] VA 0x%llX is in no recorded range", static_cast<unsigned long long>(va));
        // the nearest ranges below and above
        uint32_t below = UINT32_MAX, above = UINT32_MAX;
        for (uint32_t i = 0; i < kEntries; ++i) {
            const Entry& e = g_e[i];
            if (e.state == kEmpty || !e.va) continue;
            if (e.va + e.size <= va &&
                (below == UINT32_MAX || e.va + e.size > g_e[below].va + g_e[below].size ||
                 (e.va + e.size == g_e[below].va + g_e[below].size && e.seq > g_e[below].seq)))
                below = i;
            if (e.va > va && (above == UINT32_MAX || e.va < g_e[above].va || (e.va == g_e[above].va && e.seq > g_e[above].seq))) above = i;
        }
        if (below != UINT32_MAX) {
            Describe(g_e[below], now, line, sizeof line);
            Log("[gpucrash] nearest recorded range below (ends 0x%llX before the VA): %s",
                static_cast<unsigned long long>(va - (g_e[below].va + g_e[below].size)), line);
        }
        if (above != UINT32_MAX) {
            Describe(g_e[above], now, line, sizeof line);
            Log("[gpucrash] nearest recorded range above (starts 0x%llX after the VA): %s", static_cast<unsigned long long>(g_e[above].va - va), line);
        }
        if (!nh && below != UINT32_MAX && above != UINT32_MAX) {
            const uint32_t s0 = g_e[below].seq < g_e[above].seq ? g_e[below].seq : g_e[above].seq;
            const uint32_t s1 = g_e[below].seq < g_e[above].seq ? g_e[above].seq : g_e[below].seq;
            const uint64_t gap = g_e[above].va - (g_e[below].va + g_e[below].size);
            Log("[gpucrash] the gap there is 0x%llX bytes (%.2f MB); recorded objects without a known VA made between #%u and #%u that fit in it "
                "(likely owners if the VAs were handed out in rising order; freed ones first):",
                static_cast<unsigned long long>(gap), SizeMB(gap), s0, s1);
            int shown = 0;
            for (int pass = 0; pass < 2 && shown < 8; ++pass)
                for (uint32_t i = 0; i < kEntries && shown < 8; ++i) {
                    const Entry& e = g_e[i];
                    if (e.state == kEmpty || e.va || e.seq <= s0 || e.seq >= s1 || e.size > gap) continue;
                    if ((pass == 0) == (e.state == kLive)) continue;
                    Describe(e, now, line, sizeof line);
                    Log("[gpucrash]   %s", line);
                    ++shown;
                }
            if (!shown) Log("[gpucrash]   none (the owner is not a recorded heap or resource, or its entry was reused)");
        }
    }
    FreedRecently(now, line, sizeof line);
}


}  // namespace

// ---------------------------------------------------------------------------------------------------------------- public
bool InstallGpuMemoryRecord(const wchar_t* iniPath)
{
    if (g_installed) return true;
    InitClock();
    if (!AllocTables()) {
        Log("[gpumem] GpuMemoryRecord: no memory for the table (%.1f MB): off", static_cast<double>(kTableBytes) / 1048576.0);
        return false;
    }
    g_gframe = FindGFrameCounter();
    g_installed = true;
    const GameModule& game = Game();
    int routes = 0;
    if (game.base) {
        if (void** slot = FindDelaySlot(game.base, "d3d12.dll", 101, "D3D12CreateDevice")) {
            g_delaySlot = slot;
            g_delayOrig.store(reinterpret_cast<CreateDeviceFn>(*slot));
            if (WritePointer(slot, reinterpret_cast<void*>(&DelayCreateDevice))) ++routes;
            else g_delaySlot = nullptr;
        }
        if (void** slot = FindImportSlot(game.base, "KERNEL32.dll", "GetProcAddress")) {
            g_realGpa = reinterpret_cast<GpaFn>(*slot);
            if (g_realGpa && WritePointer(slot, reinterpret_cast<void*>(&GpaHook))) ++routes;
        }
    }
    Log("[gpumem] GpuMemoryRecord on: %s; table of %u entries (%.1f MB), the first %u kept for the session",
        routes == 2 ? "both device-creation routes set (the d3d12.dll import #101 and GetProcAddress)"
        : routes   ? "one device-creation route set"
                   : "no device-creation route found: it attaches late (the game's adapter)",
        kEntries, static_cast<double>(kTableBytes) / 1048576.0, kEarly);
    (void)iniPath;
    return true;
}

void GpuMemoryRecordTick()
{
    static unsigned ticks = 0;
    ++ticks;
    if (ticks % 30 == 1) SnapshotSystem();
    if (!g_installed) return;
    if (!g_attached.load(std::memory_order_acquire) && ticks % 5 == 0) {
        if (IDXGIAdapter3* adapter = GameAdapter()) {
            HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
            auto create = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
            ID3D12Device* d = nullptr;
            if (create && SUCCEEDED(create(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d))) && d) {
                const ULONG refs = d->AddRef();
                d->Release();
                if (refs > 2) Attach(d, 3);  // the game's device (else a new one: not used)
                d->Release();
            }
        }
    }
    if (ticks % 60 == 0) {
        static uint64_t lastTicks = 0, lastCalls = 0, lastMade = 0, lastFreed = 0, lastPlaced = 0;
        const uint64_t t = g_hookTicks.load(), c = g_hookCalls.load(), m = g_recorded.load(), f = g_frees.load(), pl = g_placed.load();
        uint32_t live = 0, used = 0;
        AcquireSRWLockShared(&g_lock);
        for (uint32_t i = 0; i < kEntries; ++i) {
            used += g_e[i].state != kEmpty;
            live += g_e[i].state == kLive;
        }
        ReleaseSRWLockShared(&g_lock);
        Log("[gpumem] stats (60 s): %llu made, %llu freed, %llu placed; own time %.2f ms in %llu calls (%.2f us each); table %u used, %u alive, "
            "%llu reused, %llu not recorded, %llu frees not seen; %s",
            static_cast<unsigned long long>(m - lastMade), static_cast<unsigned long long>(f - lastFreed),
            static_cast<unsigned long long>(pl - lastPlaced), QpcSeconds(static_cast<int64_t>(t - lastTicks)) * 1000.0,
            static_cast<unsigned long long>(c - lastCalls), c > lastCalls ? QpcSeconds(static_cast<int64_t>(t - lastTicks)) * 1e6 / static_cast<double>(c - lastCalls) : 0.0,
            used, live, static_cast<unsigned long long>(g_reused.load()), static_cast<unsigned long long>(g_dropped.load()),
            static_cast<unsigned long long>(g_unseen.load()), RouteName(g_attached.load() ? g_route.load() : 0));
        lastTicks = t;
        lastCalls = c;
        lastMade = m;
        lastFreed = f;
        lastPlaced = pl;
    }
}

void GpuMemoryRecordReport(uint64_t faultVa)
{
    __try {
        const int cur = g_sysCur.load(std::memory_order_acquire);
        if (g_installed && cur >= 0) LibraryLines(g_sys[cur]);
        if (!g_installed) {
            Log("[gpucrash] GPU memory record: off (GpuMemoryRecord=0)%s",
                faultVa ? "; with GpuMemoryRecord=1 the next report names what was at the fault address" : "");
            return;
        }
        const bool locked = LockForReport();
        if (!locked) Log("[gpucrash] GPU memory record: busy (another thread holds it); reading it anyway");
        ReportLocked(faultVa);
        if (locked) ReleaseSRWLockShared(&g_lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[gpucrash] GPU memory record: reading it faulted");
    }
}

// What was at a fault VA, in one line for CrashReport's summary: the newest recorded range that held it (alive, or freed N s
// before), else the nearest recorded ranges below / above. False: the record is off (out says how to switch it on).
bool GpuMemoryRecordOwner(uint64_t va, char* out, size_t cap)
{
    out[0] = 0;
    if (!g_installed) {
        strcpy_s(out, cap, "not known: GpuMemoryRecord is off (set GpuMemoryRecord=1 and the next report names it)");
        return false;
    }
    __try {
        const bool locked = LockForReport();
        const uint64_t now = Qpc();
        uint32_t best = UINT32_MAX, below = UINT32_MAX, above = UINT32_MAX;
        int held = 0;
        for (uint32_t i = 0; i < kEntries; ++i) {
            const Entry& e = g_e[i];
            if (e.state == kEmpty || !e.va) continue;
            if (va >= e.va && va < e.va + e.size) {
                ++held;
                if (best == UINT32_MAX || e.seq > g_e[best].seq) best = i;
            } else if (e.va + e.size <= va) {
                if (below == UINT32_MAX || e.va + e.size > g_e[below].va + g_e[below].size) below = i;
            } else if (e.va > va && (above == UINT32_MAX || e.va < g_e[above].va)) above = i;
        }
        auto brief = [&](const Entry& e, char* o, size_t oc) {
            const char* kind = e.kind == kHeap ? "heap" : e.kind == kReserved ? "reserved" : "committed";
            const char* what = e.kind == kHeap ? "" : e.dim == D3D12_RESOURCE_DIMENSION_BUFFER ? " buffer" : " texture";
            char by[64] = "?";
            if (e.create[0]) FrameText(e.create[0], by, sizeof by);
            char name[48] = "";
            if (e.name && e.name <= kNames) sprintf_s(name, " \"%s\"", g_names[e.name - 1]);
            sprintf_s(o, oc, "%.2f MB %s%s%s (%s heap) made at %.1f s (frame %u) by %s", SizeMB(e.size), kind, what, name,
                      e.kind == kReserved ? "no" : HeapTypeName(e.heapType), ProcSeconds(e.createQpc), e.createFrame, by);
        };
        char b[240];
        if (best != UINT32_MAX) {
            const Entry& e = g_e[best];
            brief(e, b, sizeof b);
            if (e.state == kLive)
                sprintf_s(out, cap, "alive: %s; VA at offset 0x%llX (record #%u)%s", b, static_cast<unsigned long long>(va - e.va), e.seq,
                          held > 1 ? "; older ranges held it before" : "");
            else
                sprintf_s(out, cap, "freed %.1f s before (at %.1f s, frame %u): %s; VA at offset 0x%llX (record #%u)%s",
                          QpcSeconds(static_cast<int64_t>(now - e.freeQpc)), ProcSeconds(e.freeQpc), e.freeFrame, b,
                          static_cast<unsigned long long>(va - e.va), e.seq, held > 1 ? "; older ranges held it before" : "");
        } else {
            char lo[240] = "none", hi[240] = "none";
            if (below != UINT32_MAX) brief(g_e[below], lo, sizeof lo);
            if (above != UINT32_MAX) brief(g_e[above], hi, sizeof hi);
            sprintf_s(out, cap, "in no recorded range (driver memory, or made before the record attached); nearest below: %s; above: %s", lo, hi);
        }
        if (locked) ReleaseSRWLockShared(&g_lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(out, cap, "not known (reading the record faulted)");
    }
    return true;
}

// ---- self-test hooks ----
bool GpuMemoryRecordTestInstall()
{
    if (g_installed) return true;
    InitClock();
    if (!AllocTables()) return false;
    g_installed = true;
    return true;
}
void* GpuMemoryRecordTestWrap(void* realCreate)
{
    g_gpaReal.store(reinterpret_cast<CreateDeviceFn>(realCreate));
    return reinterpret_cast<void*>(&GpaCreateDevice);
}
bool GpuMemoryRecordTestRoutes(uint8_t* exeBase, bool* delay, bool* gpa)
{
    *delay = FindDelaySlot(exeBase, "d3d12.dll", 101, "D3D12CreateDevice") != nullptr;
    *gpa = FindImportSlot(exeBase, "KERNEL32.dll", "GetProcAddress") != nullptr;
    return *delay && *gpa;
}
int GpuMemoryRecordTestFind(const void* obj, uint64_t* va, uint64_t* size, char* name, size_t nameSize)
{
    int best = -1;
    for (uint32_t i = 0; i < kEntries; ++i)
        if (g_e[i].state != kEmpty && g_e[i].obj == reinterpret_cast<uint64_t>(obj) && (best < 0 || g_e[i].seq > g_e[best].seq))
            best = static_cast<int>(i);
    if (best < 0) return 0;
    const Entry& e = g_e[best];
    if (va) *va = e.va;
    if (size) *size = e.size;
    if (name) strcpy_s(name, nameSize, e.name ? g_names[e.name - 1] : "");
    return e.state == kLive ? 1 : 2;
}
bool GpuMemoryRecordTestAttached() { return g_attached.load(); }
void GpuMemoryRecordTestAdd(const void* obj, uint64_t va, uint64_t size, bool buffer, bool freeNow)
{
    NewObject n;
    n.obj = reinterpret_cast<uint64_t>(obj);
    n.kind = kCommitted;
    n.va = va;
    n.size = size;
    n.dim = static_cast<uint8_t>(buffer ? D3D12_RESOURCE_DIMENSION_BUFFER : D3D12_RESOURCE_DIMENSION_TEXTURE2D);
    n.width = buffer ? 0 : 1024;
    n.height = buffer ? 0 : 1024;
    n.depth = buffer ? 0 : 1;
    n.mips = buffer ? 0 : 1;
    n.format = buffer ? 0 : 28;
    n.heapType = D3D12_HEAP_TYPE_DEFAULT;
    Record(n);
    if (freeNow) Freed(n.obj, g_seq.load());
}
void GpuMemoryRecordTestBench(int n, double* createNs, double* releaseMissNs, double* releaseHitNs)
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    const double scale = 1e9 / static_cast<double>(f.QuadPart) / n;
    const uint64_t fake = 0x7FF000000000ull;  // never dereferenced
    QueryPerformanceCounter(&a);
    for (int i = 0; i < n; ++i) GpuMemoryRecordTestAdd(reinterpret_cast<const void*>(fake + static_cast<uint64_t>(i) * 16), 0x7000000000ull + static_cast<uint64_t>(i) * 65536, 65536, true, false);
    QueryPerformanceCounter(&b);
    *createNs = static_cast<double>(b.QuadPart - a.QuadPart) * scale;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < n; ++i) Freed(fake + static_cast<uint64_t>(n + i) * 16, g_seq.load(std::memory_order_relaxed));
    QueryPerformanceCounter(&b);
    *releaseMissNs = static_cast<double>(b.QuadPart - a.QuadPart) * scale;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < n; ++i) Freed(fake + static_cast<uint64_t>(i) * 16, g_seq.load(std::memory_order_relaxed));
    QueryPerformanceCounter(&b);
    *releaseHitNs = static_cast<double>(b.QuadPart - a.QuadPart) * scale;
}
void GpuMemoryRecordTestReset()
{
    for (DevVt& d : g_dev) {
        void** vt = d.vt.load();
        if (!vt) continue;
        const int slots[] = {kSlotCommitted, kSlotHeap, kSlotPlaced, kSlotReserved, kSlotCommitted1, kSlotHeap1, kSlotReserved1,
                             kSlotCommitted2, kSlotPlaced1, kSlotCommitted3, kSlotPlaced2, kSlotReserved2};
        for (int s : slots)
            if (d.orig[s]) WritePointer(&vt[s], d.orig[s]);
        d.vt.store(nullptr);
    }
    for (ObjVt& o : g_obj) {
        void** vt = o.vt.load();
        if (!vt) continue;
        WritePointer(&vt[kSlotRelease], reinterpret_cast<void*>(o.release));
        WritePointer(&vt[kSlotSetName], reinterpret_cast<void*>(o.setName));
        o.vt.store(nullptr);
    }
    g_attached.store(false);
    if (g_e) memset(g_e, 0, kTableBytes);
    g_early = g_cursor = 0;
    g_seq = 0;
    g_nameCount = 0;
    g_recorded = 0;
    g_frees = 0;
    g_reused = 0;
    g_dropped = 0;
    g_placed = 0;
    g_heapVa = 0;
    g_unseen = 0;
}

}  // namespace opf
