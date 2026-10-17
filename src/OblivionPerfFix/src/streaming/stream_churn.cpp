// LumenKeepArrayCapacity, UploadMemoryKeep and PhysicsTreeTimeSlicing: memory that is given back and taken again every time the world streams in or out, which
// shows up as stutter while travelling (2026-10-08, the Stendarr route north and south of the Blackwood forest).
//
// LumenKeepArrayCapacity. FLumenSceneData::ResetAndConsolidate (UE 5.3 LumenScene.cpp, run once per frame
// inside the Lumen scene update task that the render thread waits for) consolidates its five TSparseSpanArrays
// (PrimitiveGroups, Heightfields, MeshCards, Cards, PageTable). TSparseSpanArray::Consolidate calls
// Elements.SetNum(MaxSize) with shrinking allowed, so whenever streaming frees a tail of entries the array is
// reallocated to exactly fit (TArray::ResizeShrink: realloc + copy of the whole array), and the next AddSpan past the
// end reallocates it again (ResizeGrow, +37.5%). PrimitiveGroups has one 80-byte entry per Lumen primitive group
// (every foliage/tree instance of an unmerged ISM): 1.5-2.1 million entries, up to 166 MB, on the forest route. Each of
// these copies the whole array into freshly committed memory: 25-48 ms on a worker while the render thread waits
// (measured with the fast sampler: exe+0x1F8B990 -> ResizeShrink exe+0xE79B20 / ResizeGrow exe+0xE79870 ->
// FMallocBinned2::Realloc -> memcpy). The fix keeps the capacity: the two RemoveAt calls get bAllowShrinking = false
// and the three ResizeShrink calls are skipped. And PrimitiveGroups' one grow (an inlined AddSpan) gets a larger
// block: at least twice the capacity, or, when a new Lumen scene (each exterior arrival starts an empty one) reaches an
// eighth of the session's peak entry count, the peak plus a quarter (ResizeGrow sizes from Num, so the hook raises Num
// for the call and puts it back). The arrays hold exactly the same elements (only spare capacity differs), so nothing
// the renderer reads changes. Switched live from the ini.
//
// UploadMemoryKeep. The D3D12 RHI hands back upload memory (CPU-written buffers the GPU copies from:
// texture and buffer initial data, distance-field brick uploads, dynamic buffers) soon after a burst: FD3D12Adapter::
// EndFrame cleans the upload heap allocator's three pool allocators with FrameLag = 20 (hard-coded), and the fast
// allocator keeps only d3d12.FastAllocator.MinPagesToRetain = 5 of its 4 MB pages, releasing one more each frame
// once unused for 10 frames. Every streaming burst while travelling (a few seconds apart) then creates them again:
// CreateCommittedResource + Map in the kernel (amdxc64 -> win32u), several to 30 ms each, often while holding the
// allocator's lock that texture streaming, RDG setup tasks and the render thread wait on (measured: the render
// thread 25-41 ms in UpdateDistanceFieldAtlas -> AllocUploadResource -> new pool, or in RDG CreatePassBarriers
// waiting for that lock). The fix keeps empty upload pools for the rest of the session instead of 20 frames
// (UploadMemoryKeepFrames, default 0 = session; a frame count expires them again) and keeps up to UploadPagesToKeep
// (default 24: travel never needed more than 24, and the rest only cost video memory, 2026-10-09)
// fast-allocator pages. Memory is only kept, never taken beyond what the game itself allocated at its
// peak; the pools are reused by the next bursts (measured +0.04-0.46 GB committed over a route and four transitions,
// working set unchanged, no growth across transitions). A time limit was tried first (3600 frames) and dropped: the
// pools of one big burst (a save load) then expire together and the clean-up destroys them all in one frame on the
// RHI thread while holding the big-block lock (~140 ms of driver calls; the render thread waited ~60 ms for it).
// What the GPU receives is identical. Switched live from the ini.
// Switching it off (the ini, or MemoryRelief's RAM level) used to hand every kept pool back at the next clean-up, the
// same ~140 ms bulk release (an earlier relief switch's hitch). Now the frame lag ramps down instead,
// from more than the session's age to the game's 20 over UploadReleaseRampFrames (900) frames: the clean-up releases
// the pools oldest first, those of one streaming burst at a time. Each clean-up call is timed; the ramp's end is logged
// with its longest call.
//
// PhysicsTreeTimeSlicing. Chaos keeps its spatial acceleration structure (AABB trees of all bodies) up to
// date with a background rebuild that is normally time-sliced over several frames, while queries and collision
// detection use the current tree plus a tree of the changed ("dirty") bodies. When more than
// p.Chaos.AccelerationStructureTimeSlicingMaxQueueSizeBeforeForce = 1000 changes are queued (one streamed-in area's
// bodies), the rebuild is done in one go instead, and the game thread waits for it at the end of the physics step:
// 30-34 ms at one spot of the route (FPBDRigidsEvolutionBase::ComputeIntermediateSpatialAcceleration, PBDRigidsEvolution.cpp,
// the only user of the variable). The fix raises the limit to 100000 (the variable itself, found by its registration),
// so big updates are time-sliced like small ones: the same tree is built, only spread over frames, and every query
// still finds every body (unbuilt changes are in the dirty tree). Measured: that spot 48 -> 21 ms of hitch time.
// Bigger slices. Each slice of the rebuild handles p.IterationsPerTimeSlice = 4000 elements, and the finished
// tree is copied to the game thread's structure p.Chaos.AccelerationStructureTimeSlicingMaxBytesCopy = 100,000 bytes per
// frame (BroadPhaseConfig and PBDRigidsEvolution.cpp; both read every time they are used). With the trees of a streamed
// forest that takes 38-131 frames, and every change made meanwhile is queued and applied to both new trees in the one
// frame the rebuild finishes (FlushAsyncAccelerationQueue): 9,000-13,000 changes, 25-41 ms while the game thread waits
// (Echo Mine -> Brotch Camp, 2026-10-13). The fix makes the slices PhysicsSliceIterations / PhysicsSliceCopyKB big, so a
// rebuild finishes within a few frames and its queue stays small (the largest catch-up 25-41 -> 8-12 ms); the same trees
// are built, in fewer frames, on the background task. Both variables are found by their registration like the first.
// The copy slice is set per step by the physics step hook (physics_flush.cpp, PhysicsSliceAdaptive): the game's
// value while quiet, the bigger one while a rebuild has a backlog. A fixed big copy slice made every step a swap while
// the world streamed, and the game thread re-applies its pending changes to each new structure it gets: the walk's mean
// frame 14.6-14.8 -> 16.2-16.3 ms. Without the step hook the slice is fixed.
//
// GpuSceneKeepCapacity. The renderer's GPU scene keeps every primitive's and every instance's data in
// GPU buffers (FGPUScene::UpdateBufferState, UE 5.3 GPUScene.cpp). Each is sized to the next power of two of what is
// in use (ResizeStructuredBufferIfNeeded / ...SOAIfNeeded, UnifiedBuffer.cpp) and resized whenever that size changes,
// in both directions: a new buffer is created and the old contents copied over. Travelling through a forest moves the
// instance count (every tree, bush and grass instance) back and forth across a power of two as areas stream in and out,
// so the 2-4 million instance buffer (48-64 bytes each: 100-250 MB) is created again and again; a new buffer of that
// size is a fresh committed resource from the D3D12 pool allocator, made by the kernel while the render thread waits
// (measured on the Roxey Inn road at night, 2026-10-09: a 118 ms frame, the render thread in GPUScene.Update ->
// UpdateBufferState -> ResizeStructuredBufferSOAIfNeeded -> CreateCommittedResource, the game thread waiting for it; at
// the same spot in every run). The fix keeps a buffer's capacity when the need drops by one power of two and only
// shrinks it when the need falls to a quarter or less (e.g. into an interior): the same hysteresis idea as LumenKeepArrayCapacity. The
// primitive and instance-payload buffers get the same rule. The instance buffer's size is also its SOA stride (the
// arrays of each instance lie that far apart, and every shader reads the stride from the GPU scene parameters): the
// fix therefore changes the reserve where the game computes it (edi, before both the buffer size and the stored stride
// are taken from it), so the buffer and its stride always agree; the data in it is the same, the extra capacity is
// unused. Every size change (the game's and the kept ones) is logged. Switched live from the ini.
//
// Sites are found by code shape and every target is checked before anything is written (otherwise the fix stays
// off with a log line); see the patterns below. Calls are redirected through stubs placed so that only the two high
// bytes of each rel32 change (one atomic write where possible, as ClothConfigCache does).
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace opf {
namespace {

// ---- code patterns (addresses of build 19115871 in the comments) ----

// FLumenSceneData::ResetAndConsolidate (exe+0x1F8B990): each of the five inlined TSparseSpanArray::Consolidate ends
// with its shrinking call, followed by the span allocator's flag test for the bit array:
//   call RemoveAt/ResizeShrink ; cmp byte [rbx+0D0h],r14b ; lea rax,[rbx+4]
// (exe+0x1F8BBA6 PrimitiveGroups, 0x1F8BC1A Heightfields, 0x1F8BC8E MeshCards, 0x1F8BCF4 Cards, 0x1F8BD61 PageTable)
constexpr char kLumenShrinkPattern[] = "E8 ?? ?? ?? ?? 44 38 B3 D0 00 00 00 48 8D 43 04";
constexpr size_t kLumenSpan = 0x200;  // all five lie within this many bytes
// TArray::RemoveAtImpl(Index, Count, bAllowShrinking), called after mov r9b,1: test r8d,r8d; je
constexpr char kRemoveAtEntry[] = "45 85 C0 0F 84";
constexpr char kAllowShrinking[] = "41 B1 01";  // mov r9b,1 right before the call
// TArray::ResizeShrink: push rbx; sub rsp,30h; mov ?,[rcx+0Ch] (ArrayMax); mov rbx,rcx; movsxd ?,[rcx+8] (ArrayNum);
// ... cmp ?,4000h (the 16 KB slack rule)
constexpr char kResizeShrinkEntry[] = "40 53 48 83 EC 30";
constexpr char kMovRbxRcx[] = "48 8B D9";
constexpr size_t kMovRbxSearch = 0x14;       // mov rbx,rcx lies within this
constexpr size_t kShrinkSlackSearch = 0x30;  // the 16 KB slack constant (00 40 00 00) lies within this

// UpdateLumenScenePrimitives: PrimitiveGroups.AddSpan inlined, the only place that array grows (exe+0x1F978CE, call at
// +52 = exe+0x1F97902): movsxd r15,[r12+8] (Num) ... mov [r12+8],eax (Num = new max) ... cmp ebx,eax; jbe;
// mov edx,r15d; mov rcx,r12; call ResizeGrow(OldNum); lea rax,[r15+r15*4]; mov ecx,ebx; shl rax,4 (80-byte elements)
constexpr char kLumenGrowPattern[] =
    "4D 63 7C 24 08 44 39 38 0F 8E ?? ?? ?? ?? 84 C9 74 04 48 83 C3 04 8B 03 8B D8 41 89 44 24 08 41 2B DF "
    "41 8B 44 24 0C 41 2B C7 3B D8 76 0B 41 8B D7 49 8B CC E8 ?? ?? ?? ?? 4B 8D 04 BF 8B CB 48 C1 E0 04";
constexpr size_t kLumenGrowCall = 52;
// TArray<80-byte T>::ResizeGrow(OldNum) (exe+0xE79870): prologue, then movsxd rbx,[rcx+8]; mov esi,edx; mov rdi,rcx;
// cmp ebx,edx ... and the element size in mov r9d,50h before ResizeAllocation.
constexpr char kResizeGrowEntry[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 63 59 08 8B F2 48 8B F9 3B DA";
constexpr char kElementSize80[] = "41 B9 50 00 00 00";
constexpr size_t kResizeGrowSearch = 0xA0;
constexpr int64_t kMaxGroupsCapacity = 16 * 1024 * 1024;  // sanity cap for the grow policy (16M entries, 1.28 GB)

// FD3D12Adapter::EndFrame (exe+0x24FFDA0), FD3D12UploadHeapAllocator::CleanUpAllocations(FrameLag = 20) inlined:
//   mov edx,14h ; lea rcx,[rdi+SmallBlock] ; call FD3D12MultiBuddyAllocator::CleanUpAllocations
//   lea rcx,[rdi+BigBlockCS] ; call [EnterCriticalSection]
//   xor r8d,r8d ; lea rcx,[rdi+BigBlock] ; lea edx,[r8+14h] ; call FD3D12PoolAllocator::CleanUpAllocations
//   lea rcx,[rdi+BigBlockCS] ; call [LeaveCriticalSection]
//   lea rcx,[rdi+FastConstant] ; mov edx,14h ; call FD3D12MultiBuddyAllocator::CleanUpAllocations
constexpr char kEndFramePattern[] =
    "BA 14 00 00 00 48 8D 4F ?? E8 ?? ?? ?? ?? 48 8D 8F ?? ?? ?? ?? FF 15 ?? ?? ?? ?? "
    "45 33 C0 48 8D 8F ?? ?? ?? ?? 41 8D 50 14 E8 ?? ?? ?? ?? 48 8D 8F ?? ?? ?? ?? FF 15 ?? ?? ?? ?? "
    "48 8D 8F ?? ?? ?? ?? BA 14 00 00 00 E8 ?? ?? ?? ??";
constexpr size_t kSmallBlockCall = 9, kEnterCall = 21, kBigBlockCall = 41, kLeaveCall = 53, kFastConstantCall = 71;

// d3d12.FastAllocator.MinPagesToRetain and PhysicsTreeTimeSlicing's Chaos variable: their registration (lea r8,[global] ... lea rdx,[name])
// gives the int.
constexpr char kLeaRdxPattern[] = "48 8D 15 ?? ?? ?? ??";
constexpr char kLeaR8Pattern[] = "4C 8D 05 ?? ?? ?? ??";
constexpr size_t kRegisterLookBack = 32;

// PhysicsTreeTimeSlicing: queued acceleration-structure changes above which Chaos rebuilds in one go instead of time-sliced (game: 1000).
constexpr LONG kChaosQueueBeforeForce = 100000;
// UploadMemoryKeep: frame lag meaning "keep for the session" (the pools' fences never get this far behind).
constexpr uint32_t kKeepForSession = 0xFFFFFFFFu;

// GpuSceneKeepCapacity: FGPUScene::UpdateBufferState, two template instances (exe+0x1DD87BF, exe+0x1DD8D40), rsi = this. The instance
// buffer's reserve RoundUpToPowerOfTwo(Max(MaxSize, 256)) clamped to MAX_INSTANCE_ID in edi, GetDataStrideInFloat4s
// twice (NumBytes = stride * reserve * 16, NumArrays = stride), ResizeStructuredBufferSOAIfNeeded(GraphBuilder,
// InstanceSceneDataBuffer = [rsi+30h], Params, L"GPUScene.InstanceSceneData"), then the reserve stored as the stride:
//   lea rax,[rax*2+1]; bsr rcx,rax; mov eax,1000000h; shl edi,cl; cmp edi,eax; cmova edi,eax; call GetDataStride (+0x18);
//   imul eax,edi; shl eax,4; mov [rbp+?],eax; call GetDataStride (+0x26); lea rdx,[rsi+30h]; mov [rbp+?],eax;
//   lea r9,[name] (+0x32); mov rcx,r13; lea r8,[rbp+?]; call ResizeSOA (+0x40); mov [rsi+338h],rax; mov [rsi+48h],edi;
//   mov [rsi+340h],edi
// The first GetDataStride call goes through a thunk that replaces edi (the caller keeps nothing in volatile registers
// across that call: rdx, r8, r9 are set again before use, and the next real call clobbers the rest).
constexpr char kGpuSceneReservePattern[] =
    "48 8D 04 45 01 00 00 00 48 0F BD C8 B8 00 00 00 01 D3 E7 3B F8 0F 47 F8 E8 ?? ?? ?? ?? 0F AF C7 C1 E0 04 "
    "89 45 ?? E8 ?? ?? ?? ?? 48 8D 56 30 89 45 ?? 4C 8D 0D ?? ?? ?? ?? 49 8B CD 4C 8D 45 ?? E8 ?? ?? ?? ?? "
    "48 89 86 38 03 00 00 89 7E 48 89 BE 40 03 00 00";
constexpr size_t kGpuSceneStrideCall = 0x18, kGpuSceneStrideCall2 = 0x26, kGpuSceneNameLea = 0x32, kGpuSceneSoaCall = 0x40;
constexpr size_t kInstanceBufferOffset = 0x30;  // FGPUScene::InstanceSceneDataBuffer (TRefCountPtr), lea rdx,[rsi+30h]
// FInstanceSceneShaderData::GetDataStrideInFloat4s (exe+0x329BD00): 3 or 4 by the shader platform; uses rax and rcx only.
constexpr char kStrideFn[] = "0F B7 05 ?? ?? ?? ?? 48 6B C8 34 48 8D 05 ?? ?? ?? ?? 8B 04 01 C1 E8 02 24 01 F6 D8 1B C0 83 C0 04 C3";
// ResizeStructuredBufferIfNeeded (exe+0x270CB10): mov rax,rsp; sub rsp,58h; shr r8d,4 (NumBytes / 16-byte elements)
constexpr char kResizeStructuredEntry[] = "48 8B C4 48 83 EC 58 41 C1 E8 04";
// The primitive and payload buffers: lea r9,[name] then, within kNameCallSearch bytes, call ResizeStructuredBufferIfNeeded;
// both lie within kNameLeaSearch bytes of an instance-reserve site (copy 1: exe+0x1DD888D / 0x1DD896E; copy 2:
// exe+0x1DD8E1E / 0x1DD8EF9).
constexpr char kLeaR9Pattern[] = "4C 8D 0D ?? ?? ?? ??";
constexpr size_t kNameCallSearch = 0x40, kNameLeaSearch = 0x100;
constexpr uint32_t kGpuSceneShrinkDivisor = 4;  // shrink only once the need is a quarter of the kept capacity or less
constexpr uint32_t kGpuScenePeakFraction = 8;   // a grow past an eighth of the session's largest size goes straight to it
constexpr double kGpuSceneSmallMB = 4.0;        // buffers this small are never shrunk (and their changes not logged)

using RemoveAtFn = void (*)(void* array, int32_t index, int32_t count, bool allowShrinking);
using ShrinkFn = void (*)(void* array);
using BuddyCleanFn = void (*)(void* allocator, uint64_t frameLag);
using PoolCleanFn = void (*)(void* allocator, uint64_t frameLag, bool forceFree);

constexpr int kLumenSites = 5;

struct State {
    bool lumenFound = false, lumenInstalled = false;
    uint8_t* lumenCall[kLumenSites] = {};
    void* lumenOriginal[kLumenSites] = {};
    bool lumenIsRemoveAt[kLumenSites] = {};
    uint8_t* lumenGrowCall = nullptr;   // the PrimitiveGroups grow (optional part of LumenKeepArrayCapacity)
    void* lumenGrowOriginal = nullptr;
    bool uploadFound = false, uploadInstalled = false;
    uint8_t* uploadCall[3] = {};  // small block, big block, fast constant
    void* buddyClean = nullptr;
    void* poolClean = nullptr;
    volatile LONG* minPages = nullptr;
    LONG minPagesOriginal = 0;
    volatile LONG* chaosForce = nullptr;   // PhysicsTreeTimeSlicing
    LONG chaosForceOriginal = 0;
    volatile LONG* chaosIterations = nullptr;   // PhysicsTreeTimeSlicing: p.IterationsPerTimeSlice
    LONG chaosIterationsOriginal = 0;
    volatile LONG* chaosCopyBytes = nullptr;    // PhysicsTreeTimeSlicing: p.Chaos.AccelerationStructureTimeSlicingMaxBytesCopy
    LONG chaosCopyBytesOriginal = 0;
    bool gpuSceneFound = false, gpuSceneInstalled = false;  // GpuSceneKeepCapacity
    uint8_t* gsStrideCall[2] = {};     // the instance reserve's first GetDataStrideInFloat4s call, per instance
    void* gsStrideFn = nullptr;        // GetDataStrideInFloat4s
    uint8_t* gsResizeCall[4] = {};     // PrimitiveData x2, InstancePayloadData x2
    void* gsResizeFn = nullptr;        // ResizeStructuredBufferIfNeeded
    const wchar_t* gsName[3] = {};     // L"GPUScene.InstanceSceneData", L"GPUScene.PrimitiveData", L"GPUScene.InstancePayloadData"
    uint8_t* gsThunk = nullptr;        // the reserve thunk (in the stub region)
    uint8_t* region = nullptr;     // stubs, within rel32 reach of the game's code
    size_t stubsUsed = 0;
    uintptr_t stubAt[24] = {};
} g;

std::atomic<bool> g_lumenOn{false};
std::atomic<uint64_t> g_lumenCalls{0}, g_lumenKept{0};
std::atomic<int32_t> g_lumenCapacity[kLumenSites];  // largest ArrayMax seen per array (for the log)
std::atomic<int32_t> g_lumenPeakGroups{0};           // largest PrimitiveGroups.Num seen (the grow policy's memory)
std::atomic<uint64_t> g_lumenGrows{0}, g_lumenGrowsEnlarged{0};
const char* const kLumenArrayNames[kLumenSites] = {"PrimitiveGroups", "Heightfields", "MeshCards", "Cards", "PageTable"};

// TArray<T> header after the data pointer: int32 ArrayNum at +8, int32 ArrayMax at +12.
void NoteCapacity(int site, const void* array)
{
    if (!array) return;
    const int32_t max = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(array) + 12);
    if (site == 0) {
        const int32_t num = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(array) + 8);
        int32_t peak = g_lumenPeakGroups.load(std::memory_order_relaxed);
        while (num > peak && !g_lumenPeakGroups.compare_exchange_weak(peak, num, std::memory_order_relaxed)) {}
    }
    int32_t seen = g_lumenCapacity[site].load(std::memory_order_relaxed);
    while (max > seen && !g_lumenCapacity[site].compare_exchange_weak(seen, max, std::memory_order_relaxed)) {}
}
std::atomic<uint32_t> g_uploadLag{0};      // 0 = the game's 20 frames
std::atomic<LONG> g_uploadPages{0};        // 0 = the game's MinPagesToRetain
std::atomic<uint64_t> g_uploadCleanups{0};
std::atomic<uint32_t> g_uploadFrames{0};      // big-block clean-ups so far (one per frame, from FD3D12Adapter::EndFrame)
std::atomic<uint32_t> g_uploadRampFrames{900}; // UploadReleaseRampFrames; 0 = everything at once, as the game
std::atomic<uint32_t> g_uploadRampStart{0};   // frame the release ramp started (0 = none)
std::atomic<uint64_t> g_uploadRampFrom{0};    // frame lag at the start of the ramp
std::atomic<uint64_t> g_uploadCleanMaxUs{0};  // longest clean-up call since the last report
std::atomic<uint64_t> g_uploadRampMaxUs{0};   // longest clean-up call during the current ramp

template <int I>
void LumenRemoveAt(void* array, int32_t index, int32_t count, bool allowShrinking)
{
    g_lumenCalls.fetch_add(1, std::memory_order_relaxed);
    if (allowShrinking && g_lumenOn.load(std::memory_order_relaxed)) {
        allowShrinking = false;
        g_lumenKept.fetch_add(1, std::memory_order_relaxed);
    }
    reinterpret_cast<RemoveAtFn>(g.lumenOriginal[I])(array, index, count, allowShrinking);
    NoteCapacity(I, array);
}

template <int I>
void LumenShrink(void* array)
{
    g_lumenCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_lumenOn.load(std::memory_order_relaxed)) {
        g_lumenKept.fetch_add(1, std::memory_order_relaxed);
        NoteCapacity(I, array);
        return;
    }
    reinterpret_cast<ShrinkFn>(g.lumenOriginal[I])(array);
    NoteCapacity(I, array);
}

using GrowFn = void (*)(void* array, int32_t oldNum);

// PrimitiveGroups grows: the game's ResizeGrow sizes the new block from Num (Num + 3/8 Num + 16). We let it size it from
// a larger Num, then put Num back: the array gets more spare capacity, the elements and Num are what the game set.
// Policy: at least double the capacity; and when a new Lumen scene (after a door, a fast travel or a load) already holds
// an eighth of the most this session has needed, go straight to that peak plus a quarter.
void LumenGrow(void* array, int32_t oldNum)
{
    auto* const hdr = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(array) + 8);  // [0] ArrayNum, [1] ArrayMax
    const int32_t num = hdr[0];
    g_lumenGrows.fetch_add(1, std::memory_order_relaxed);
    int64_t want = 0;
    if (g_lumenOn.load(std::memory_order_relaxed)) {
        want = int64_t{hdr[1]} * 2;
        const int64_t peak = g_lumenPeakGroups.load(std::memory_order_relaxed);
        if (peak > 0 && num >= peak / 8 && peak + peak / 4 > want) want = peak + peak / 4;
        if (want > kMaxGroupsCapacity) want = kMaxGroupsCapacity;
    }
    const int64_t basis = want * 8 / 11;  // the game's slack turns this into ~want
    if (basis > num) {
        hdr[0] = static_cast<int32_t>(basis);
        reinterpret_cast<GrowFn>(g.lumenGrowOriginal)(array, oldNum);
        hdr[0] = num;
        g_lumenGrowsEnlarged.fetch_add(1, std::memory_order_relaxed);
    } else {
        reinterpret_cast<GrowFn>(g.lumenGrowOriginal)(array, oldNum);
    }
    int32_t peak = g_lumenPeakGroups.load(std::memory_order_relaxed);
    while (num > peak && !g_lumenPeakGroups.compare_exchange_weak(peak, num, std::memory_order_relaxed)) {}
    NoteCapacity(0, array);
}

void* const kRemoveAtHooks[kLumenSites] = {reinterpret_cast<void*>(&LumenRemoveAt<0>), reinterpret_cast<void*>(&LumenRemoveAt<1>),
                                           reinterpret_cast<void*>(&LumenRemoveAt<2>), reinterpret_cast<void*>(&LumenRemoveAt<3>),
                                           reinterpret_cast<void*>(&LumenRemoveAt<4>)};
void* const kShrinkHooks[kLumenSites] = {reinterpret_cast<void*>(&LumenShrink<0>), reinterpret_cast<void*>(&LumenShrink<1>),
                                         reinterpret_cast<void*>(&LumenShrink<2>), reinterpret_cast<void*>(&LumenShrink<3>),
                                         reinterpret_cast<void*>(&LumenShrink<4>)};

// ---- GpuSceneKeepCapacity: the GPU scene buffers' capacity ----

constexpr int kGpuSceneKinds = 3;  // 0 instances (sizes in entries), 1 primitives and 2 payload (sizes in bytes)
const char* const kGpuSceneNames[kGpuSceneKinds] = {"InstanceSceneData", "PrimitiveData", "InstancePayloadData"};
struct KeptBuffer {
    const void* key;     // the buffer member (TRefCountPtr<FRDGPooledBuffer>) inside its FGPUScene
    uint32_t capacity;   // the size the buffer has (what we last let the game use)
    uint32_t need;       // the size the game asked for last time
    uint64_t used;       // g_keptTick at the last call: a scene that is gone stops calling and its slots get reused
};
constexpr int kKeptSlots = 16;  // 3 per FGPUScene; slots of scenes that no longer update are reused (oldest first)
KeptBuffer g_kept[kKeptSlots];
uint64_t g_keptTick = 0;
SRWLOCK g_keptLock = SRWLOCK_INIT;
std::atomic<bool> g_gpuSceneOn{false};
std::atomic<uint64_t> g_gsGrows[kGpuSceneKinds], g_gsShrinks[kGpuSceneKinds], g_gsKept[kGpuSceneKinds];
std::atomic<uint32_t> g_gsLargest[kGpuSceneKinds];
std::atomic<uint64_t> g_gsSlotsReused{0};

using StrideFn = uint32_t (*)();
using ResizeStructuredFn = void* (*)(void* graphBuilder, void** external, uint32_t numBytes, const wchar_t* name);

double GpuSceneMB(int kind, uint32_t size)
{
    const double bytes = kind == 0 ? double(size) * 16.0 * reinterpret_cast<StrideFn>(g.gsStrideFn)() : double(size);
    return bytes / (1024.0 * 1024.0);
}

// The size the game gets for this buffer (sizes are the game's powers of two):
//  - grows: what it asks for, or, once the need is past 1/kGpuScenePeakFraction of the largest this buffer kind had
//    in the session, that largest size at once (after a fast travel or a door the scene fills up again in several
//    doublings, each a new buffer; LumenKeepArrayCapacity's grow policy does the same for Lumen's array);
//  - drops: the kept capacity while the need is above 1/kGpuSceneShrinkDivisor of it, and always for buffers under
//    kGpuSceneSmallMB (the payload buffer flips between two small sizes every frame: the per-view dynamic primitives);
//    below that, what the game asks for.
// `fresh`: the buffer does not exist yet; the game creates it at the size returned. Off: always what the game asks for.
uint32_t GpuSceneKeep(int kind, const void* key, uint32_t need, bool fresh)
{
    AcquireSRWLockExclusive(&g_keptLock);
    KeptBuffer* slot = nullptr;
    for (KeptBuffer& k : g_kept)
        if (k.key == key) { slot = &k; break; }
    if (!slot) {
        KeptBuffer* oldest = &g_kept[0];
        for (KeptBuffer& k : g_kept)
            if (k.used < oldest->used) oldest = &k;
        if (oldest->key) g_gsSlotsReused.fetch_add(1, std::memory_order_relaxed);
        *oldest = {key, 0, 0, 0};
        slot = oldest;
    }
    slot->used = ++g_keptTick;
    uint32_t use = need;
    {
        const bool on = g_gpuSceneOn.load(std::memory_order_relaxed);
        const uint32_t cap = slot->capacity;
        const bool changed = need != slot->need;
        slot->need = need;
        const uint32_t peak = g_gsLargest[kind].load(std::memory_order_relaxed);
        const bool toPeak = on && peak > need && need > peak / kGpuScenePeakFraction;
        const char* const unit = kind == 0 ? " instances" : " bytes";
        if (fresh || cap == 0 || need > cap) {
            use = toPeak ? peak : need;
            if (cap && !fresh) {
                g_gsGrows[kind].fetch_add(1, std::memory_order_relaxed);
                if (GpuSceneMB(kind, use) >= kGpuSceneSmallMB)
                    Log("[gpuscene] %s %u -> %u%s (%.0f -> %.0f MB): %s", kGpuSceneNames[kind], cap, use, unit, GpuSceneMB(kind, cap),
                        GpuSceneMB(kind, use), use == need ? "the game grows it" : "grown straight to the session's largest");
            }
            slot->capacity = use;
        } else if (need < cap) {
            if (on && (need > cap / kGpuSceneShrinkDivisor || GpuSceneMB(kind, cap) < kGpuSceneSmallMB)) {
                use = cap;
                if (changed) {
                    g_gsKept[kind].fetch_add(1, std::memory_order_relaxed);
                    if (GpuSceneMB(kind, cap) >= kGpuSceneSmallMB)
                        Log("[gpuscene] %s needs %u%s (%.0f MB): kept at %u (%.0f MB) instead of shrinking", kGpuSceneNames[kind],
                            need, unit, GpuSceneMB(kind, need), cap, GpuSceneMB(kind, cap));
                }
            } else {
                slot->capacity = need;
                g_gsShrinks[kind].fetch_add(1, std::memory_order_relaxed);
                if (GpuSceneMB(kind, cap) >= kGpuSceneSmallMB)
                    Log("[gpuscene] %s %u -> %u%s (%.0f -> %.0f MB): shrunk", kGpuSceneNames[kind], cap, need, unit,
                        GpuSceneMB(kind, cap), GpuSceneMB(kind, need));
            }
        }
        uint32_t largest = g_gsLargest[kind].load(std::memory_order_relaxed);
        while (slot->capacity > largest &&
               !g_gsLargest[kind].compare_exchange_weak(largest, slot->capacity, std::memory_order_relaxed)) {}
    }
    ReleaseSRWLockExclusive(&g_keptLock);
    return use;
}

// Called by the thunk with the game's reserve (edi) and the FGPUScene (rsi); the thunk puts the result in edi.
uint32_t InstanceReserveHook(uint32_t need, const uint8_t* gpuScene)
{
    const void* const buffer = gpuScene + kInstanceBufferOffset;
    return GpuSceneKeep(0, buffer, need, *static_cast<void* const*>(buffer) == nullptr);
}

void* GpuSceneResizeHook(void* graphBuilder, void** external, uint32_t numBytes, const wchar_t* name)
{
    const int kind = name == g.gsName[1] ? 1 : name == g.gsName[2] ? 2 : 0;
    if (kind) numBytes = GpuSceneKeep(kind, external, numBytes, *external == nullptr);
    return reinterpret_cast<ResizeStructuredFn>(g.gsResizeFn)(graphBuilder, external, numBytes, name);
}

// The thunk the first GetDataStrideInFloat4s call goes to (through a stub): edi = InstanceReserveHook(edi, rsi), then on
// to GetDataStrideInFloat4s with the caller's return address. Entered with rsp = 8 mod 16; 28h keeps the call aligned.
constexpr uint8_t kThunkCode[] = {
    0x48, 0x83, 0xEC, 0x28,              // sub rsp,28h
    0x89, 0xF9,                          // mov ecx,edi
    0x48, 0x89, 0xF2,                    // mov rdx,rsi
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax,InstanceReserveHook
    0xFF, 0xD0,                          // call rax
    0x89, 0xC7,                          // mov edi,eax
    0x48, 0x83, 0xC4, 0x28,              // add rsp,28h
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax,GetDataStrideInFloat4s
    0xFF, 0xE0,                          // jmp rax
};
constexpr size_t kThunkHookImm = 11, kThunkStrideImm = 29, kThunkSize = 64;

// The frame lag for this frame's clean-up (RHI thread): kept, ramping down after the fix was switched off, or the game's.
uint64_t UploadLag(uint64_t gameLag)
{
    const uint32_t keep = g_uploadLag.load(std::memory_order_relaxed);
    if (keep) return keep;
    const uint32_t start = g_uploadRampStart.load(std::memory_order_relaxed);
    const uint32_t frames = g_uploadRampFrames.load(std::memory_order_relaxed);
    if (!start) return gameLag;
    const uint32_t elapsed = g_uploadFrames.load(std::memory_order_relaxed) - start;
    if (elapsed >= frames) {
        uint32_t expected = start;
        if (g_uploadRampStart.compare_exchange_strong(expected, 0))
            Log("[upload] kept upload memory handed back over %u frames, oldest first (longest clean-up %.1f ms)", elapsed,
                g_uploadRampMaxUs.exchange(0) / 1000.0);
        return gameLag;
    }
    const uint64_t from = g_uploadRampFrom.load(std::memory_order_relaxed);
    const uint64_t lag = from - from * elapsed / frames;
    return lag > gameLag ? lag : gameLag;
}

uint64_t UploadMicroseconds()
{
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return QpcToMicroseconds(static_cast<uint64_t>(c.QuadPart), freq);
}

void UploadTimed(uint64_t us)
{
    for (std::atomic<uint64_t>* m : {&g_uploadCleanMaxUs, &g_uploadRampMaxUs}) {
        if (m == &g_uploadRampMaxUs && !g_uploadRampStart.load(std::memory_order_relaxed)) continue;
        uint64_t old = m->load(std::memory_order_relaxed);
        while (us > old && !m->compare_exchange_weak(old, us)) {}
    }
}

void UploadBuddyClean(void* allocator, uint64_t frameLag)
{
    g_uploadCleanups.fetch_add(1, std::memory_order_relaxed);
    const uint64_t t0 = UploadMicroseconds();
    reinterpret_cast<BuddyCleanFn>(g.buddyClean)(allocator, UploadLag(frameLag));
    UploadTimed(UploadMicroseconds() - t0);
}

void UploadPoolClean(void* allocator, uint64_t frameLag, bool forceFree)
{
    if (forceFree) {
        reinterpret_cast<PoolCleanFn>(g.poolClean)(allocator, frameLag, forceFree);
        return;
    }
    g_uploadFrames.fetch_add(1, std::memory_order_relaxed);
    const uint64_t t0 = UploadMicroseconds();
    reinterpret_cast<PoolCleanFn>(g.poolClean)(allocator, UploadLag(frameLag), false);
    UploadTimed(UploadMicroseconds() - t0);
}

uint32_t Rva(const void* p) { return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - Game().base); }

bool InText(const uint8_t* p) { return Game().text.Contains(p) && Game().text.Contains(p + 16); }

const uint8_t* FindWideString(const Section& s, const wchar_t* text)
{
    const size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    const uint8_t* hit = nullptr;
    for (const uint8_t* p = s.begin; p + bytes <= s.end; p += 2) {
        if (memcmp(p, text, bytes) != 0) continue;
        if (hit) return nullptr;
        hit = p;
    }
    return hit;
}

// The int an FAutoConsoleVariableRef registers under this name, or nullptr: lea rdx,[name] with the nearest
// lea r8,[global] at most kRegisterLookBack bytes before it (D3D12: adjacent; Chaos: a mov [rsp+20h],0 in between).
volatile LONG* RegisteredIntCvar(const wchar_t* text)
{
    const GameModule& game = Game();
    const uint8_t* name = FindWideString(game.rdata, text);
    if (!name) return nullptr;
    const Pattern leaR8 = ParsePattern(kLeaR8Pattern);
    uint8_t* global = nullptr;
    int n = 0;
    for (uint8_t* r : FindAll(game.text, ParsePattern(kLeaRdxPattern))) {
        if (RipTarget(r, 3, 7) != name) continue;
        uint8_t* found = nullptr;
        for (uint8_t* at = r - leaR8.size(); at >= r - kRegisterLookBack && !found; --at)
            if (MatchesAt(at, leaR8)) found = RipTarget(at, 3, 7);
        if (!found) continue;
        global = found;
        ++n;
    }
    if (n != 1 || global < game.base || global + 4 > game.base + game.sizeOfImage || game.text.Contains(global) ||
        game.rdata.Contains(global) || (reinterpret_cast<uintptr_t>(global) & 3) != 0)
        return nullptr;
    return reinterpret_cast<volatile LONG*>(global);
}

bool IsResizeShrink(const uint8_t* f)
{
    if (!InText(f) || !MatchesAt(f, ParsePattern(kResizeShrinkEntry))) return false;
    bool movRbx = false;
    for (size_t i = 6; i + 3 <= kMovRbxSearch && !movRbx; ++i) movRbx = MatchesAt(f + i, ParsePattern(kMovRbxRcx));
    if (!movRbx) return false;
    for (size_t i = 0; i + 4 <= kShrinkSlackSearch; ++i)
        if (f[i] == 0x00 && f[i + 1] == 0x40 && f[i + 2] == 0x00 && f[i + 3] == 0x00) return true;
    return false;
}

bool FindLumen()
{
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kLumenShrinkPattern));
    if (hits.size() != kLumenSites || hits.back() - hits.front() > static_cast<ptrdiff_t>(kLumenSpan)) {
        Log("[lumen] ResetAndConsolidate shrink calls found %zu times (expected %d close together): LumenKeepArrayCapacity unavailable",
            hits.size(), kLumenSites);
        return false;
    }
    int removeAts = 0, shrinks = 0;
    for (int i = 0; i < kLumenSites; ++i) {
        uint8_t* const call = hits[i];
        uint8_t* const target = RipTarget(call, 1, 5);
        const bool removeAt = MatchesAt(call - 3, ParsePattern(kAllowShrinking)) && InText(target) &&
                              MatchesAt(target, ParsePattern(kRemoveAtEntry));
        const bool shrink = !removeAt && IsResizeShrink(target);
        if (!removeAt && !shrink) {
            Log("[lumen] call at exe+0x%X (-> exe+0x%X) is neither RemoveAt(.., true) nor ResizeShrink: LumenKeepArrayCapacity unavailable",
                Rva(call), Rva(target));
            return false;
        }
        g.lumenCall[i] = call;
        g.lumenOriginal[i] = target;
        g.lumenIsRemoveAt[i] = removeAt;
        (removeAt ? removeAts : shrinks)++;
    }
    if (removeAts != 2 || shrinks != 3) {
        Log("[lumen] %d RemoveAt and %d ResizeShrink calls (expected 2 and 3): LumenKeepArrayCapacity unavailable", removeAts, shrinks);
        return false;
    }
    g.lumenFound = true;
    Log("[lumen] ResetAndConsolidate shrink calls at exe+0x%X, 0x%X, 0x%X, 0x%X, 0x%X (RemoveAt exe+0x%X/0x%X, ResizeShrink x3)",
        Rva(hits[0]), Rva(hits[1]), Rva(hits[2]), Rva(hits[3]), Rva(hits[4]),
        Rva(g.lumenOriginal[g.lumenIsRemoveAt[0] ? 0 : 3]), Rva(g.lumenOriginal[g.lumenIsRemoveAt[3] ? 3 : 0]));

    // Optional: the PrimitiveGroups grow (without it only shrinking is avoided).
    const auto grows = FindAll(game.text, ParsePattern(kLumenGrowPattern));
    if (grows.size() == 1) {
        uint8_t* const call = grows[0] + kLumenGrowCall;
        uint8_t* const target = RipTarget(call, 1, 5);
        bool size80 = false;
        if (InText(target) && MatchesAt(target, ParsePattern(kResizeGrowEntry)))
            for (size_t i = 0; i + 6 <= kResizeGrowSearch && !size80; ++i) size80 = MatchesAt(target + i, ParsePattern(kElementSize80));
        if (size80) {
            g.lumenGrowCall = call;
            g.lumenGrowOriginal = target;
            Log("[lumen] PrimitiveGroups grow at exe+0x%X (ResizeGrow exe+0x%X, 80-byte elements)", Rva(call), Rva(target));
        } else {
            Log("[lumen] PrimitiveGroups grow call at exe+0x%X does not go to the expected ResizeGrow: grows stay as they are", Rva(call));
        }
    } else {
        Log("[lumen] PrimitiveGroups grow found %zu times (expected 1): grows stay as they are", grows.size());
    }
    return true;
}

bool FindUpload()
{
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kEndFramePattern));
    if (hits.size() != 1) {
        Log("[upload] upload heap clean-up in FD3D12Adapter::EndFrame found %zu times (expected 1): UploadMemoryKeep unavailable",
            hits.size());
        return false;
    }
    uint8_t* const at = hits[0];
    void** const enter = FindImportSlot(game.base, "KERNEL32.dll", "EnterCriticalSection");
    void** const leave = FindImportSlot(game.base, "KERNEL32.dll", "LeaveCriticalSection");
    if (!enter || !leave || reinterpret_cast<void**>(RipTarget(at + kEnterCall, 2, 6)) != enter ||
        reinterpret_cast<void**>(RipTarget(at + kLeaveCall, 2, 6)) != leave) {
        Log("[upload] the clean-up at exe+0x%X is not locked by Enter/LeaveCriticalSection: UploadMemoryKeep unavailable", Rva(at));
        return false;
    }
    uint8_t* const buddy = RipTarget(at + kSmallBlockCall, 1, 5);
    uint8_t* const pool = RipTarget(at + kBigBlockCall, 1, 5);
    if (RipTarget(at + kFastConstantCall, 1, 5) != buddy || pool == buddy || !InText(buddy) || !InText(pool)) {
        Log("[upload] clean-up calls at exe+0x%X do not have the expected targets: UploadMemoryKeep unavailable", Rva(at));
        return false;
    }
    g.uploadCall[0] = at + kSmallBlockCall;
    g.uploadCall[1] = at + kBigBlockCall;
    g.uploadCall[2] = at + kFastConstantCall;
    g.buddyClean = buddy;
    g.poolClean = pool;

    // d3d12.FastAllocator.MinPagesToRetain (optional: without it only the pool lag is kept)
    if ((g.minPages = RegisteredIntCvar(L"d3d12.FastAllocator.MinPagesToRetain")) != nullptr) g.minPagesOriginal = *g.minPages;
    if (!g.minPages) Log("[upload] d3d12.FastAllocator.MinPagesToRetain not found: fast-allocator pages are released as before");
    g.uploadFound = true;
    Log("[upload] EndFrame clean-up at exe+0x%X (pool allocator exe+0x%X, buddy allocators exe+0x%X); "
        "d3d12.FastAllocator.MinPagesToRetain %s exe+0x%X (%ld)",
        Rva(at), Rva(pool), Rva(buddy), g.minPages ? "at" : "missing", g.minPages ? Rva(const_cast<LONG*>(g.minPages)) : 0,
        g.minPages ? g.minPagesOriginal : 0L);
    return true;
}

constexpr size_t kRegion = 0x20000, kStubSize = 14;

// A stub "jmp [rip+0]; dq hook" at the original target's address modulo 64 KB (in either half of the region), so the
// redirected rel32 keeps its low 16 bits. Returns nullptr if both places are taken.
uint8_t* MakeStub(const uint8_t* original, void* hook)
{
    const uintptr_t low = reinterpret_cast<uintptr_t>(original) & 0xFFFF;
    for (uintptr_t half = 0; half < kRegion; half += 0x10000) {
        const uintptr_t at = reinterpret_cast<uintptr_t>(g.region) + half + low;
        if (at + kStubSize > reinterpret_cast<uintptr_t>(g.region) + kRegion) continue;
        bool taken = false;
        for (size_t i = 0; i < g.stubsUsed; ++i)
            if (at < g.stubAt[i] + kStubSize && g.stubAt[i] < at + kStubSize) taken = true;
        const uintptr_t thunk = reinterpret_cast<uintptr_t>(g.gsThunk);
        if (thunk && at < thunk + kThunkSize && thunk < at + kStubSize) taken = true;
        if (taken) continue;
        auto* stub = reinterpret_cast<uint8_t*>(at);
        const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
        DWORD old;
        VirtualProtect(stub, kStubSize, PAGE_READWRITE, &old);
        memcpy(stub, jmp, sizeof jmp);
        memcpy(stub + sizeof jmp, &hook, sizeof hook);
        VirtualProtect(stub, kStubSize, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), stub, kStubSize);
        if (g.stubsUsed < sizeof g.stubAt / sizeof g.stubAt[0]) g.stubAt[g.stubsUsed++] = at;
        return stub;
    }
    return nullptr;
}

// Points the call at `stub`; one atomic write when the changed bytes share a qword (only then while the game runs).
bool RedirectCall(uint8_t* call, uint8_t* stub, bool beforeGameCode)
{
    const int64_t rel = stub - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t insn[5] = {0xE8};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(insn + 1, &rel32, sizeof rel32);
    size_t first = 5, last = 0;
    for (size_t i = 0; i < 5; ++i)
        if (insn[i] != call[i]) {
            if (first == 5) first = i;
            last = i;
        }
    if (first == 5) return true;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(call) + first, hi = reinterpret_cast<uintptr_t>(call) + last;
    const uintptr_t qword = lo & ~uintptr_t{7};
    if (hi < qword + 8) {
        auto* q = reinterpret_cast<volatile LONG64*>(qword);
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(qword), 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        const LONG64 before = *q;
        LONG64 after = before;
        for (size_t i = first; i <= last; ++i)
            reinterpret_cast<uint8_t*>(&after)[reinterpret_cast<uintptr_t>(call) + i - qword] = insn[i];
        const bool ok = InterlockedCompareExchange64(q, after, before) == before;
        VirtualProtect(reinterpret_cast<void*>(qword), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(qword), 8);
        return ok;
    }
    return beforeGameCode && WriteCode(call, insn);
}

bool EnsureRegion(const char* tag)
{
    if (g.region) return true;
    g.region = AllocNearGame(kRegion);
    if (!g.region) Log("[%s] no free memory near the game's code for the call stubs: fix unavailable", tag);
    return g.region != nullptr;
}

// The first call target within `search` bytes after `from` that is `fn` (an E8 byte inside another instruction gives
// some other target and is skipped).
uint8_t* CallTo(uint8_t* from, size_t search, const void* fn)
{
    for (uint8_t* p = from; p < from + search; ++p)
        if (*p == 0xE8 && RipTarget(p, 1, 5) == fn) return p;
    return nullptr;
}

bool FindGpuScene()
{
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kGpuSceneReservePattern));
    if (hits.size() != 2) {
        Log("[gpuscene] UpdateBufferState's instance reserve found %zu times (expected 2): GpuSceneKeepCapacity unavailable", hits.size());
        return false;
    }
    const wchar_t* const names[kGpuSceneKinds] = {L"GPUScene.InstanceSceneData", L"GPUScene.PrimitiveData",
                                                   L"GPUScene.InstancePayloadData"};
    for (int k = 0; k < kGpuSceneKinds; ++k) g.gsName[k] = reinterpret_cast<const wchar_t*>(FindWideString(game.rdata, names[k]));
    uint8_t* stride = RipTarget(hits[0] + kGpuSceneStrideCall, 1, 5);
    uint8_t* const soa = RipTarget(hits[0] + kGpuSceneSoaCall, 1, 5);
    bool ok = g.gsName[0] && g.gsName[1] && g.gsName[2] && InText(stride) && MatchesAt(stride, ParsePattern(kStrideFn)) && InText(soa);
    for (int i = 0; i < 2 && ok; ++i) {
        ok = RipTarget(hits[i] + kGpuSceneStrideCall, 1, 5) == stride && RipTarget(hits[i] + kGpuSceneStrideCall2, 1, 5) == stride &&
             RipTarget(hits[i] + kGpuSceneNameLea, 3, 7) == reinterpret_cast<const uint8_t*>(g.gsName[0]) &&
             RipTarget(hits[i] + kGpuSceneSoaCall, 1, 5) == soa;
        g.gsStrideCall[i] = hits[i] + kGpuSceneStrideCall;
    }
    if (!ok) {
        Log("[gpuscene] instance reserve at exe+0x%X / exe+0x%X: calls or name not as expected: GpuSceneKeepCapacity unavailable",
            Rva(hits[0]), Rva(hits[1]));
        return false;
    }
    // the primitive and payload buffers: lea r9,[name] near each reserve site, then the call to the resize function
    const Pattern lea = ParsePattern(kLeaR9Pattern);
    uint8_t* resize = nullptr;
    int found = 0;
    for (int k = 1; k < kGpuSceneKinds && ok; ++k) {
        int n = 0;
        for (int i = 0; i < 2 && ok; ++i) {
            for (uint8_t* p = hits[i] - kNameLeaSearch; p < hits[i] + kNameLeaSearch; ++p) {
                if (!MatchesAt(p, lea) || RipTarget(p, 3, 7) != reinterpret_cast<const uint8_t*>(g.gsName[k])) continue;
                if (!resize)
                    for (uint8_t* q = p + lea.size(); q < p + lea.size() + kNameCallSearch && !resize; ++q)
                        if (*q == 0xE8 && InText(RipTarget(q, 1, 5)) && MatchesAt(RipTarget(q, 1, 5), ParsePattern(kResizeStructuredEntry)))
                            resize = RipTarget(q, 1, 5);
                uint8_t* const call = resize ? CallTo(p + lea.size(), kNameCallSearch, resize) : nullptr;
                if (!call || n >= 2 || found >= 4) { ok = false; break; }
                g.gsResizeCall[found++] = call;
                ++n;
            }
        }
        ok = ok && n == 2;
    }
    if (!ok || found != 4) {
        Log("[gpuscene] the primitive and payload buffer resizes were not found where expected: GpuSceneKeepCapacity unavailable");
        return false;
    }
    g.gsStrideFn = stride;
    g.gsResizeFn = resize;
    g.gpuSceneFound = true;
    Log("[gpuscene] UpdateBufferState: instance reserve calls exe+0x%X / exe+0x%X (GetDataStrideInFloat4s exe+0x%X, SOA resize "
        "exe+0x%X, buffer +0x%zX), primitive/payload resizes exe+0x%X / 0x%X / 0x%X / 0x%X (exe+0x%X)",
        Rva(g.gsStrideCall[0]), Rva(g.gsStrideCall[1]), Rva(stride), Rva(soa), kInstanceBufferOffset, Rva(g.gsResizeCall[0]),
        Rva(g.gsResizeCall[1]), Rva(g.gsResizeCall[2]), Rva(g.gsResizeCall[3]), Rva(resize));
    return true;
}

// Writes the thunk at the end of the stub region (no stub there; MakeStub keeps clear of it afterwards).
uint8_t* MakeThunk()
{
    auto* const at = g.region + kRegion - kThunkSize;
    for (size_t i = 0; i < g.stubsUsed; ++i)
        if (reinterpret_cast<uintptr_t>(at) < g.stubAt[i] + kStubSize && g.stubAt[i] < reinterpret_cast<uintptr_t>(at) + kThunkSize)
            return nullptr;
    uint8_t code[sizeof kThunkCode];
    memcpy(code, kThunkCode, sizeof code);
    const void* const hook = reinterpret_cast<const void*>(&InstanceReserveHook);
    memcpy(code + kThunkHookImm, &hook, sizeof hook);
    memcpy(code + kThunkStrideImm, &g.gsStrideFn, sizeof g.gsStrideFn);
    DWORD old;
    if (!VirtualProtect(at, kThunkSize, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    memcpy(at, code, sizeof code);
    VirtualProtect(at, kThunkSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), at, kThunkSize);
    return at;
}

}  // namespace

bool ApplyGpuSceneKeepCapacityFix(bool beforeGameCode)
{
    if (g.gpuSceneInstalled) return true;
    if (!g.gpuSceneFound && !FindGpuScene()) return false;
    if (!EnsureRegion("gpuscene")) return false;
    if (!g.gsThunk) g.gsThunk = MakeThunk();
    uint8_t* const strideStub = g.gsThunk ? MakeStub(static_cast<uint8_t*>(g.gsStrideFn), g.gsThunk) : nullptr;
    uint8_t* const resizeStub = MakeStub(static_cast<uint8_t*>(g.gsResizeFn), reinterpret_cast<void*>(&GpuSceneResizeHook));
    if (!strideStub || !resizeStub) {
        Log("[gpuscene] no room for the thunk or the call stubs: GpuSceneKeepCapacity unavailable");
        return false;
    }
    // All six or none: the two template instances must agree on the sizes, or each would resize what the other kept.
    uint8_t* const calls[6] = {g.gsStrideCall[0], g.gsStrideCall[1], g.gsResizeCall[0], g.gsResizeCall[1], g.gsResizeCall[2], g.gsResizeCall[3]};
    uint8_t* const originals[6] = {static_cast<uint8_t*>(g.gsStrideFn), static_cast<uint8_t*>(g.gsStrideFn), static_cast<uint8_t*>(g.gsResizeFn),
                                   static_cast<uint8_t*>(g.gsResizeFn), static_cast<uint8_t*>(g.gsResizeFn), static_cast<uint8_t*>(g.gsResizeFn)};
    for (int i = 0; i < 6; ++i) {
        if (!RedirectCall(calls[i], i < 2 ? strideStub : resizeStub, beforeGameCode)) {
            for (int j = 0; j < i; ++j) RedirectCall(calls[j], originals[j], beforeGameCode);
            Log("[gpuscene] could not switch the call at exe+0x%X: GpuSceneKeepCapacity unavailable", Rva(calls[i]));
            return false;
        }
    }
    g.gpuSceneInstalled = true;
    Log("[gpuscene] GpuSceneKeepCapacity installed (2 reserve calls through the thunk at %p, 4 resize calls redirected)", static_cast<void*>(g.gsThunk));
    return true;
}

void SetGpuSceneKeepCapacityEnabled(bool enabled)
{
    if (g_gpuSceneOn.exchange(enabled) != enabled && g.gpuSceneInstalled)
        Log("[gpuscene] keep buffer capacity %s", enabled ? "enabled (shrink only at a quarter of the need)" : "disabled (buffers resize as before)");
}


bool GpuSceneInfo(GpuSceneFixInfo* out)
{
    if (!g.gpuSceneFound) return false;
    *out = {};
    for (int i = 0; i < 2; ++i) out->strideCall[i] = g.gsStrideCall[i];
    for (int i = 0; i < 4; ++i) out->resizeCall[i] = g.gsResizeCall[i];
    out->strideFn = static_cast<uint8_t*>(g.gsStrideFn);
    out->resizeFn = static_cast<uint8_t*>(g.gsResizeFn);
    out->thunk = g.gsThunk;
    out->resizeHook = reinterpret_cast<void*>(&GpuSceneResizeHook);
    for (int k = 0; k < 3; ++k) out->name[k] = g.gsName[k];
    return true;
}

void GpuSceneTestOriginals(void* stride, void* resize)
{
    if (stride) {
        g.gsStrideFn = stride;
        if (g.gsThunk) {  // the thunk's jump target as well
            DWORD old;
            VirtualProtect(g.gsThunk, kThunkSize, PAGE_EXECUTE_READWRITE, &old);
            memcpy(g.gsThunk + kThunkStrideImm, &stride, sizeof stride);
            VirtualProtect(g.gsThunk, kThunkSize, PAGE_EXECUTE_READ, &old);
            FlushInstructionCache(GetCurrentProcess(), g.gsThunk, kThunkSize);
        }
    }
    if (resize) g.gsResizeFn = resize;
}

bool ApplyLumenKeepCapacityFix(bool beforeGameCode)
{
    if (g.lumenInstalled) return true;
    if (!g.lumenFound && !FindLumen()) return false;
    if (!EnsureRegion("lumen")) return false;
    uint8_t* stubs[kLumenSites];
    for (int i = 0; i < kLumenSites; ++i) {
        stubs[i] = MakeStub(static_cast<uint8_t*>(g.lumenOriginal[i]), g.lumenIsRemoveAt[i] ? kRemoveAtHooks[i] : kShrinkHooks[i]);
        if (!stubs[i]) {
            Log("[lumen] no room for call stub %d: LumenKeepArrayCapacity unavailable", i);
            return false;
        }
    }
    for (int i = 0; i < kLumenSites; ++i) {
        if (!RedirectCall(g.lumenCall[i], stubs[i], beforeGameCode)) {
            // put back the ones already switched (each is a single write of the original bytes' rel32)
            for (int j = 0; j < i; ++j) RedirectCall(g.lumenCall[j], static_cast<uint8_t*>(g.lumenOriginal[j]), beforeGameCode);
            Log("[lumen] could not switch the call at exe+0x%X: LumenKeepArrayCapacity unavailable", Rva(g.lumenCall[i]));
            return false;
        }
    }
    g.lumenInstalled = true;
    Log("[lumen] LumenKeepArrayCapacity installed (5 calls redirected)");
    if (g.lumenGrowCall) {
        uint8_t* const stub = MakeStub(static_cast<uint8_t*>(g.lumenGrowOriginal), reinterpret_cast<void*>(&LumenGrow));
        if (stub && RedirectCall(g.lumenGrowCall, stub, beforeGameCode)) Log("[lumen] PrimitiveGroups grow policy installed");
        else {
            g.lumenGrowCall = nullptr;
            Log("[lumen] could not switch the PrimitiveGroups grow call: grows stay as they are");
        }
    }
    return true;
}

void SetLumenKeepCapacityEnabled(bool enabled)
{
    if (g_lumenOn.exchange(enabled) != enabled && g.lumenInstalled)
        Log("[lumen] keep array capacity %s", enabled ? "enabled" : "disabled (arrays shrink as before)");
}

bool ApplyUploadMemoryKeepFix(bool beforeGameCode)
{
    if (g.uploadInstalled) return true;
    if (!g.uploadFound && !FindUpload()) return false;
    if (!EnsureRegion("upload")) return false;
    uint8_t* const buddyStub = MakeStub(static_cast<uint8_t*>(g.buddyClean), reinterpret_cast<void*>(&UploadBuddyClean));
    uint8_t* const poolStub = MakeStub(static_cast<uint8_t*>(g.poolClean), reinterpret_cast<void*>(&UploadPoolClean));
    if (!buddyStub || !poolStub) {
        Log("[upload] no room for the call stubs: UploadMemoryKeep unavailable");
        return false;
    }
    uint8_t* const stubs[3] = {buddyStub, poolStub, buddyStub};
    uint8_t* const originals[3] = {static_cast<uint8_t*>(g.buddyClean), static_cast<uint8_t*>(g.poolClean), static_cast<uint8_t*>(g.buddyClean)};
    for (int i = 0; i < 3; ++i) {
        if (!RedirectCall(g.uploadCall[i], stubs[i], beforeGameCode)) {
            for (int j = 0; j < i; ++j) RedirectCall(g.uploadCall[j], originals[j], beforeGameCode);
            Log("[upload] could not switch the call at exe+0x%X: UploadMemoryKeep unavailable", Rva(g.uploadCall[i]));
            return false;
        }
    }
    g.uploadInstalled = true;
    Log("[upload] UploadMemoryKeep installed (3 clean-up calls redirected)");
    return true;
}

void SetUploadMemoryKeep(bool enabled, int keepFrames, int keepPages, int releaseRampFrames)
{
    // keepFrames <= 0: for the rest of the session (a frame lag no fence count reaches); 1..20: the game's 20.
    const uint32_t lag = !enabled ? 0 : keepFrames <= 0 ? kKeepForSession : keepFrames > 20 ? static_cast<uint32_t>(keepFrames) : 0;
    const LONG pages = enabled && g.minPages && keepPages > g.minPagesOriginal ? keepPages : 0;
    g_uploadRampFrames = releaseRampFrames > 0 ? static_cast<uint32_t>(releaseRampFrames) : 0;
    const uint32_t oldLag = g_uploadLag.load();
    if (lag) g_uploadRampStart = 0;
    else if (oldLag && g.uploadInstalled && g_uploadRampFrames.load()) {
        // Pools kept so far are at most as old as the clean-ups counted (plus what came before the fix was installed,
        // when it was installed late): start above that, so the first frames release nothing and the oldest go first.
        const uint32_t frames = g_uploadFrames.load();
        const uint64_t from = oldLag == kKeepForSession ? 2ull * frames + 3600 : oldLag;
        g_uploadRampFrom = from;
        g_uploadRampMaxUs = 0;
        g_uploadRampStart = frames ? frames : 1;
        Log("[upload] UploadMemoryKeep off: kept upload memory is handed back over %u frames, oldest first (game: at the next frame)",
            g_uploadRampFrames.load());
    }
    const bool changed = (g_uploadLag.exchange(lag) != lag) | (g_uploadPages.exchange(pages) != pages);
    // The page count is the engine's own console variable: set it while the fix is on (again if Engine.ini or the
    // console changed it back), and when switched off put the game's value back only if it is still ours.
    static LONG written = 0;
    if (g.minPages) {
        if (pages) {
            if (*g.minPages != pages) InterlockedExchange(g.minPages, pages);
            written = pages;
        } else if (written) {
            if (*g.minPages == written) InterlockedExchange(g.minPages, g.minPagesOriginal);
            written = 0;
        }
    }
    if (changed && g.uploadInstalled) {
        if (lag == kKeepForSession)
            Log("[upload] keeping empty upload pools for the rest of the session (game: 20 frames) and up to %ld fast-allocator pages (game: %ld)",
                pages ? pages : g.minPagesOriginal, g.minPagesOriginal);
        else if (lag) Log("[upload] keeping empty upload pools for %u frames (game: 20) and up to %ld fast-allocator pages (game: %ld)",
                     lag, pages ? pages : g.minPagesOriginal, g.minPagesOriginal);
        else if (g_uploadRampStart.load())
            Log("[upload] fast-allocator pages back to the game's %ld; pools follow the game's 20 frames once handed back",
                g.minPagesOriginal);
        else Log("[upload] upload memory released as before (20 frames, %ld pages)", g.minPagesOriginal);
    }
}

// PhysicsTreeTimeSlicing: p.Chaos.AccelerationStructureTimeSlicingMaxQueueSizeBeforeForce (the int behind it, found by its registration).
bool FindChaosTimeSlicing()
{
    if (g.chaosForce) return true;
    g.chaosForce = RegisteredIntCvar(L"p.Chaos.AccelerationStructureTimeSlicingMaxQueueSizeBeforeForce");
    if (!g.chaosForce) {
        Log("[chaos] p.Chaos.AccelerationStructureTimeSlicingMaxQueueSizeBeforeForce not found: PhysicsTreeTimeSlicing unavailable");
        return false;
    }
    g.chaosForceOriginal = *g.chaosForce;
    Log("[chaos] p.Chaos.AccelerationStructureTimeSlicingMaxQueueSizeBeforeForce at exe+0x%X (%ld)",
        Rva(const_cast<LONG*>(g.chaosForce)), g.chaosForceOriginal);
    // The slice sizes (optional: without them PhysicsTreeTimeSlicing is the limit alone). p.IterationsPerTimeSlice is set by a
    // constructor (FBroadPhaseConfig), which may not have run yet when the DLL starts: the game's values are read when
    // the fix first sets them.
    g.chaosIterations = RegisteredIntCvar(L"p.IterationsPerTimeSlice");
    g.chaosCopyBytes = RegisteredIntCvar(L"p.Chaos.AccelerationStructureTimeSlicingMaxBytesCopy");
    if (g.chaosIterations && g.chaosCopyBytes)
        Log("[chaos] p.IterationsPerTimeSlice at exe+0x%X, p.Chaos.AccelerationStructureTimeSlicingMaxBytesCopy at exe+0x%X",
            Rva(const_cast<LONG*>(g.chaosIterations)), Rva(const_cast<LONG*>(g.chaosCopyBytes)));
    else {
        g.chaosIterations = g.chaosCopyBytes = nullptr;
        Log("[chaos] time-slice size variables not found: PhysicsTreeTimeSlicing keeps the game's slice sizes");
    }
    return true;
}

namespace {
// Same rule as the page count: set while on (again if someone changed it), put back when off only if still ours.
// Returns true when it wrote a new value.
bool OwnInt(volatile LONG* var, LONG original, LONG want, LONG& written)
{
    if (!var) return false;
    if (want) {
        const bool change = *var != want;
        if (change) InterlockedExchange(var, want);
        const bool fresh = written != want;
        written = want;
        return change && fresh;
    }
    if (written) {
        if (*var == written) InterlockedExchange(var, original);
        written = 0;
        return true;
    }
    return false;
}

// A slice size: never below the game's own value, which is taken when the fix first sets it (0 = not set up yet: wait).
bool OwnSlice(volatile LONG* var, LONG& original, int64_t configured, bool enabled, LONG& written)
{
    if (!var) return false;
    if (enabled && !written) {
        const LONG current = *var;
        if (current <= 0) return false;
        original = current;
    }
    const LONG want = enabled ? static_cast<LONG>(std::clamp<int64_t>(configured, original, 1000000000)) : 0;
    return OwnInt(var, original, want, written);
}
}  // namespace

void SetChaosTimeSlicing(bool enabled, int sliceIterations, int sliceCopyKB, bool adaptiveCopy)
{
    static LONG writtenForce = 0, writtenIterations = 0, writtenCopy = 0;
    static bool adaptive = false;     // the copy slice is handed to the physics step hook (physics_flush.cpp)
    if (!g.chaosForce) return;
    const LONG force = enabled && kChaosQueueBeforeForce > g.chaosForceOriginal ? kChaosQueueBeforeForce : 0;
    if (OwnInt(g.chaosForce, g.chaosForceOriginal, force, writtenForce)) {
        if (force) Log("[chaos] physics tree rebuilds time-sliced up to %ld queued changes (game: %ld)", force, g.chaosForceOriginal);
        else Log("[chaos] physics tree rebuilds as before (full rebuild above %ld queued changes)", g.chaosForceOriginal);
    }
    const bool a = OwnSlice(g.chaosIterations, g.chaosIterationsOriginal, sliceIterations, enabled, writtenIterations);
    const int64_t copyWanted = static_cast<int64_t>(sliceCopyKB) * 1000;
    bool b = false;
    if (adaptiveCopy && PhysicsStepHooked() && g.chaosCopyBytes) {
        // The step hook sets the variable each step: the game's value while quiet, ours while a rebuild has a backlog.
        if (writtenCopy) b = OwnSlice(g.chaosCopyBytes, g.chaosCopyBytesOriginal, 0, false, writtenCopy);  // fixed -> adaptive
        if (!adaptive && g.chaosCopyBytesOriginal <= 0 && *g.chaosCopyBytes > 0) g.chaosCopyBytesOriginal = *g.chaosCopyBytes;
        if (g.chaosCopyBytesOriginal > 0) {
            const LONG boost = static_cast<LONG>(std::clamp<int64_t>(copyWanted, g.chaosCopyBytesOriginal, 1000000000));
            SetPhysicsCopyAdaptive(g.chaosCopyBytes, g.chaosCopyBytesOriginal, boost, enabled);
            adaptive = enabled;
        }
    } else {
        if (adaptive) {  // adaptive -> fixed
            SetPhysicsCopyAdaptive(g.chaosCopyBytes, g.chaosCopyBytesOriginal, 0, false);
            adaptive = false;
        }
        b = OwnSlice(g.chaosCopyBytes, g.chaosCopyBytesOriginal, copyWanted, enabled, writtenCopy);
    }
    if ((a || b) && g.chaosIterations && g.chaosCopyBytes)
        Log("[chaos] physics tree rebuild slices: %ld elements and %ld bytes of copy per frame (game: %ld and %ld)",
            *g.chaosIterations, *g.chaosCopyBytes, g.chaosIterationsOriginal, g.chaosCopyBytesOriginal);
}

void LogStreamChurnStats()
{
    static uint64_t lastCalls = 0, lastKept = 0;
    const uint64_t calls = g_lumenCalls.load(), kept = g_lumenKept.load();
    if (g.lumenInstalled && calls != lastCalls)
        Log("[lumen] consolidate since last report: %llu shrinking calls, %llu kept the capacity; largest capacity so far: "
            "%s %d, %s %d, %s %d, %s %d, %s %d entries", calls - lastCalls, kept - lastKept,
            kLumenArrayNames[0], g_lumenCapacity[0].load(), kLumenArrayNames[1], g_lumenCapacity[1].load(),
            kLumenArrayNames[2], g_lumenCapacity[2].load(), kLumenArrayNames[3], g_lumenCapacity[3].load(),
            kLumenArrayNames[4], g_lumenCapacity[4].load());
    static uint64_t lastGrows = 0, lastEnlarged = 0;
    const uint64_t grows = g_lumenGrows.load(), enlarged = g_lumenGrowsEnlarged.load();
    if (g.lumenGrowCall && grows != lastGrows)
        Log("[lumen] PrimitiveGroups grows since last report: %llu (%llu given extra room); most entries so far %d",
            grows - lastGrows, enlarged - lastEnlarged, g_lumenPeakGroups.load());
    lastGrows = grows;
    lastEnlarged = enlarged;
    const uint64_t cleanMaxUs = g_uploadCleanMaxUs.exchange(0);
    if (g.uploadInstalled && cleanMaxUs >= 2000)
        Log("[upload] longest upload clean-up since last report: %.1f ms", cleanMaxUs / 1000.0);
    lastCalls = calls;
    lastKept = kept;
    static uint64_t lastGs[kGpuSceneKinds][3] = {};
    bool any = false;
    uint64_t now[kGpuSceneKinds][3];
    for (int k = 0; k < kGpuSceneKinds; ++k) {
        now[k][0] = g_gsGrows[k].load(), now[k][1] = g_gsShrinks[k].load(), now[k][2] = g_gsKept[k].load();
        for (int j = 0; j < 3; ++j) any |= now[k][j] != lastGs[k][j];
    }
    if (g.gpuSceneInstalled && any)
        Log("[gpuscene] since last report (grown / shrunk / kept instead of shrinking): instances %llu/%llu/%llu, primitives "
            "%llu/%llu/%llu, payload %llu/%llu/%llu; largest so far %u instances (%.0f MB), %.0f MB primitives, %.0f MB payload; "
            "slots reused %llu",
            now[0][0] - lastGs[0][0], now[0][1] - lastGs[0][1], now[0][2] - lastGs[0][2], now[1][0] - lastGs[1][0],
            now[1][1] - lastGs[1][1], now[1][2] - lastGs[1][2], now[2][0] - lastGs[2][0], now[2][1] - lastGs[2][1],
            now[2][2] - lastGs[2][2], g_gsLargest[0].load(), GpuSceneMB(0, g_gsLargest[0].load()), GpuSceneMB(1, g_gsLargest[1].load()),
            GpuSceneMB(2, g_gsLargest[2].load()), g_gsSlotsReused.load());
    memcpy(lastGs, now, sizeof now);
}

bool StreamChurnInfo(StreamChurnFixInfo* out)
{
    if (!g.lumenFound && !g.uploadFound) return false;
    *out = {};
    for (int i = 0; i < kLumenSites; ++i) {
        out->lumenCall[i] = g.lumenCall[i];
        out->lumenOriginal[i] = static_cast<uint8_t*>(g.lumenOriginal[i]);
        out->lumenIsRemoveAt[i] = g.lumenIsRemoveAt[i];
    }
    for (int i = 0; i < 3; ++i) out->uploadCall[i] = g.uploadCall[i];
    out->buddyClean = static_cast<uint8_t*>(g.buddyClean);
    out->poolClean = static_cast<uint8_t*>(g.poolClean);
    out->minPages = g.minPages;
    out->minPagesOriginal = g.minPagesOriginal;
    out->chaosForce = g.chaosForce;
    out->chaosForceOriginal = g.chaosForceOriginal;
    out->chaosIterations = g.chaosIterations;
    out->chaosIterationsOriginal = g.chaosIterationsOriginal;
    out->chaosCopyBytes = g.chaosCopyBytes;
    out->chaosCopyBytesOriginal = g.chaosCopyBytesOriginal;
    out->removeAtHook0 = kRemoveAtHooks[0];
    out->lumenGrowCall = g.lumenGrowCall;
    out->lumenGrowOriginal = static_cast<uint8_t*>(g.lumenGrowOriginal);
    out->growHook = reinterpret_cast<void*>(&LumenGrow);
    out->shrinkHook1 = kShrinkHooks[1];
    out->buddyHook = reinterpret_cast<void*>(&UploadBuddyClean);
    out->poolHook = reinterpret_cast<void*>(&UploadPoolClean);
    return true;
}

void StreamChurnTestOriginals(void* removeAt0, void* shrink1, void* buddy, void* pool, void* grow)
{
    if (grow) g.lumenGrowOriginal = grow;
    if (removeAt0) g.lumenOriginal[0] = removeAt0;
    if (shrink1) g.lumenOriginal[1] = shrink1;
    if (buddy) g.buddyClean = buddy;
    if (pool) g.poolClean = pool;
}

}  // namespace opf
