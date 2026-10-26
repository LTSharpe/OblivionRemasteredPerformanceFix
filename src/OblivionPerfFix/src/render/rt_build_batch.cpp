// RayTracingBuildBatch ([Fixes] RayTracingBuildBatch=1): the ray-tracing acceleration structures
// (BLAS) of meshes whose render resources the render thread sets up between frames are built together, as one batch at
// the start of the frame's scene render, instead of one at a time as each mesh is set up. The same structures, built in
// the same frame, before anything in that frame reads them.
//
// Found (2026-10-21, a PIX replay of the Imperial City flight's 97.6 s frame): the frame's extra 30-38 ms of GPU time
// is ~145 newly loaded static meshes, each one's BLAS built on its own: its buffers uploaded, one build, then a barrier
// that waits for it. A build costs 65-350 us whatever its size (12 to 7.6k triangles), nothing overlaps. A D3D12 test
// on an RTX 4090 with the same meshes: 71 builds one at a time 16.0 ms, side by side (one barrier) 0.8 ms. With the
// switch (docs/research.md): that frame's GPU busy ~50 -> under 20 ms in every pass.
//
// How the engine decides (build 19115871): FRayTracingGeometry::InitRHI (exe+0x26823C0) creates a mesh's ray-tracing
// geometry through CreateRayTracingGeometry (exe+0x266FBA0) with build priority Normal (Skip for streaming copies),
// which asks the geometry manager (GRayTracingGeometryManager) for a build: RequestBuildAccelerationStructure
// (exe+0x26C47C0). With r.RayTracing.Geometry.MaxBuiltPrimitivesPerFrame <= 0 (the game runs -1) or priority Immediate
// that builds at once on the immediate command list (one build command with its own scratch buffer, then a barrier);
// otherwise it queues the request, and ProcessBuildRequests (exe+0x26C2760) builds the queued ones as one batch at the
// start of the scene's ray-tracing update (exe+0x1D51E60, called by FDeferredShadingSceneRenderer::Render
// exe+0x1D5BC90). That is the engine's own budget mode; with a budget the game hangs the GPU (DEVICE_HUNG in 3 of 3
// runs with r.RayTracing.Geometry.MaxBuiltPrimitivesPerFrame 50000), so the budget stays as it is: only the "build at
// once" moves, to a batch of all requests.
//
// The patch: (1) CreateRayTracingGeometry's call to RequestBuildAccelerationStructure goes to Gate. While the switch is
// on, a Normal build request from InitRHI, on the render thread outside a scene render, with the budget <= 0, for a
// geometry nobody updates in place (not fast-build, no allow-update: the skin cache refits its geometries before the
// engine's batch would run), enters the function past its "build at once" test, on its queue path; everything else goes
// to the game's function unchanged. (2) The scene renderer's Render gets a wrapper (its vtable slot): when requests are
// queued, it first calls ProcessBuildRequests (build all) on the frame's command list, then renders. So the queued
// geometries are built before the frame's ray-tracing gather; no reader ever sees one still queued. (3) A cap: with no
// scene render (a loading screen) the queue would hold a whole load for one call (2,445 builds, 1.23 GB of scratch at
// an Anvil dock exit, measured by LTSharpe on an RX 9070 XT). A request that would take the queued builds' scratch past
// 64 MiB first has the queue built, in the gate, with the game's ProcessBuildRequests on the immediate command list (the
// list the "build at once" path uses there); before queueing, so the new request's index, which Create stores in the
// geometry (+0x9C), is the one that stays queued. The queue is the manager's own (its request array, read as its queue
// path writes it): a request the engine drops (a geometry released while queued) is simply no longer in it.
//
// Why the same picture: the same geometries get the same builds (same inputs, flags and build mode), in the same frame,
// before the frame's ray-tracing scene or anything else uses them. Only their place in the frame's GPU work moves: from
// the set-up commands before the frame to the start of its render, after all of them, side by side. A geometry released
// while its request is queued drops the request (the engine's ReleaseRHI). Geometries made during a render or on other
// threads, and every other build request, build as before.
//
// Sites (each found by pattern and checked; anything unexpected: not installed, one log line):
//   InitRHI: ... cmp dword [rbx+74h],0 (initializer type); mov eax,4; mov r8d,2; cmovne r8d,eax; call Create (+0x32)
//   Create: mov [rsp+10h],rbx; mov [rsp+18h],rbp; mov [rsp+20h],rsi; push rdi; sub rsp,20h (so at its call to the
//     manager its return address is 0x28 above rsp); ... xor r9d,r9d; lea rcx,[manager]; mov r8d,ebp; mov rdx,rbx;
//     call RequestBuild; mov [rbx+9Ch],eax (the request index)
//   RequestBuild: push rsi; sub rsp,40h; cmp dword [budget],0; mov rsi,rcx; jle now; test r8d,r8d; je now; (queue path,
//     +0x1F) call [lock] (the manager's request lock at +0); mov edx,[rsi+5Ch] (free count); ... mov r8,[rsi+28h]
//     (entries, 0x18 bytes each: index, priority, geometry at +8, mode); ... mov edi,[rsi+30h] (count) ...; (+0x120)
//     mov rax,[rbp+10h] (rbp = +0x38, the allocation bits inline; +0x48 their heap pointer when set) ... now: mov
//     rdx,[rdx+90h]; lea rcx,[immediate list]; call BuildAccelerationStructure; mov eax,-1; ret
//   the ray-tracing update: ... cmp byte [rcx+10E0h],bl; jne; mov rdx,[rdx+50h] (the builder's command list); lea
//     rcx,[manager]; xor r8d,r8d; call ProcessBuildRequests
//   Render: push rbp; push rbx; push r12; ... mov r12,rdx; mov rbx,rcx; ... (+0x2FF2) mov rdx,r12; mov rcx,rbx; call
//     the ray-tracing update; its only pointer in .rdata is its vtable slot
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace opf {
namespace {

constexpr char kInitRhi[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 1E 83 7B 74 00 B8 04 00 00 00 41 B8 02 00 00 00 "
    "48 8B D7 44 0F 45 C0 48 8B CB E8 ?? ?? ?? ??";
constexpr size_t kInitRhiCall = 0x32;
constexpr char kCreateHead[] = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B 01 41 8B E8 48 8B F2 48 8B D9";
constexpr char kCreateRequest[] =
    "83 FD 04 74 2D E8 ?? ?? ?? ?? 84 C0 74 1B 45 33 C9 48 8D 0D ?? ?? ?? ?? 44 8B C5 48 8B D3 E8 ?? ?? ?? ?? 89 83 9C 00 00 00";
constexpr size_t kCreateRequestManager = 0x11, kCreateRequestCall = 0x1E;
constexpr size_t kCreateFrame = 0x28;  // Create's rsp below its entry: push rdi + sub rsp,20h
constexpr char kRequest[] =
    "40 56 48 83 EC 40 83 3D ?? ?? ?? ?? 00 48 8B F1 0F 8E ?? ?? ?? ?? 45 85 C0 0F 84 ?? ?? ?? ?? 48 89 5C 24 50";
constexpr size_t kRequestQueuePath = 0x1F, kRequestNow = 0x199, kRequestNowList = 7;  // the now path's lea rcx,[immediate list]
constexpr char kRequestNowCode[] = "48 8B 92 90 00 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? B8 FF FF FF FF 48 83 C4 40 5E C3";
// The queue path's use of the manager's request array (the layout QueueState reads), at +0x87, +0xC5 and +0x120
constexpr char kQueueFree[] = "8B 56 5C 41 BE 01 00 00 00 85 D2 74 ?? 48 63 7E 58 8D 42 FF 4C 8B 46 28 48 8D 0C 7F";
constexpr char kQueueAdd[] = "8B 7E 30 8D 47 01 89 46 30 8B 46 34";
constexpr char kQueueBits[] = "48 8B 45 10 8B CF 48 85 C0 48 0F 45 E8";
constexpr size_t kQueueFreeAt = 0x87, kQueueAddAt = 0xC5, kQueueBitsAt = 0x120;
constexpr size_t kQueueEntries = 0x28, kQueueCount = 0x30, kQueueBitsInline = 0x38, kQueueBitsHeap = 0x48, kQueueFreeCount = 0x5C;
constexpr size_t kQueueEntrySize = 0x18, kQueueEntryGeometry = 8;
constexpr uint64_t kFlushScratch = 64ull << 20;  // a request that would take the queued scratch past this builds the queue first
constexpr char kWorldUpdate[] =
    "40 55 53 41 55 41 57 48 8D AC 24 28 FF FF FF 48 81 EC D8 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 B0 00 00 00 33 DB "
    "4C 89 44 24 60 49 89 18 4C 8B FA 4C 8B E9 48 89 4C 24 68 38 99 ?? ?? 00 00 75 ?? 48 8B 52 50 48 8D 0D ?? ?? ?? ?? 45 33 C0 "
    "E8 ?? ?? ?? ??";
constexpr size_t kWorldUpdateManager = 72, kWorldUpdateProcess = 82;
constexpr char kProcess[] = "44 88 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 57 41 57 48 8D 6C 24 B9 48 81 EC C0 00 00 00 4C 8B F9 FF 15";
constexpr char kRender[] =
    "40 55 53 41 54 48 8D AC 24 90 C6 FF FF B8 70 3A 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 F0 38 00 00 "
    "F7 81 94 00 00 00 00 00 80 00 4C 8B E2 48 8B D9";
constexpr size_t kRenderWorldUpdateCall = 0x2FF2;
constexpr char kRenderCallCode[] = "49 8B D4 48 8B CB E8";
constexpr uint8_t kRequestPrologue[] = {0x40, 0x56, 0x48, 0x83, 0xEC, 0x40};  // push rsi; sub rsp,40h
constexpr uint8_t kMovRsiRcx[] = {0x48, 0x8B, 0xF1};
constexpr size_t kCommandListInBuilder = 0x50;   // FRDGBuilder: its RHI command list (the ray-tracing update reads it)
constexpr size_t kFastBuild = 0x70, kAllowUpdate = 0x71;  // FRayTracingGeometry: Initializer (+0x38) .bFastBuild, .bAllowUpdate
constexpr size_t kPrimitives = 0x48;                       // ... Initializer.TotalPrimitiveCount
constexpr size_t kRhiGeometry = 0x90, kBuildScratch = 0x18;  // ... its RHI geometry; that one's SizeInfo.BuildScratchSize
constexpr int32_t kNormal = 2, kBuild = 0;

using RequestFn = int32_t (*)(void* manager, void* geometry, int32_t priority, int32_t mode);
using ProcessFn = void (*)(void* manager, void* commandList, bool buildAll);
using RenderFn = void (*)(void* renderer, void* graphBuilder);

RequestFn g_request = nullptr;   // the game's RequestBuildAccelerationStructure
RequestFn g_queue = nullptr;     // its queue path (stub in g_region)
ProcessFn g_process = nullptr;
RenderFn g_render = nullptr;     // the game's Render (the slot's old value)
void* g_manager = nullptr;
const volatile int32_t* g_budget = nullptr;
const void* g_initRhiReturn = nullptr;
void* g_immediate = nullptr;     // the immediate RHI command list (the "build at once" path's)
uint8_t* g_region = nullptr;
uint8_t* g_callSite = nullptr;
void** g_slot = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{false};
std::atomic<DWORD> g_renderThread{0};
int g_inRender = 0;              // render thread only
// render thread: requests queued by the gate since the last batch (a flag for the render hook; whether they are still
// queued is the engine's queue), and an upper bound of their scratch (the exact sum is read when it reaches the cap)
uint64_t g_pending = 0, g_pendingScratch = 0;
std::atomic<uint64_t> g_queued{0}, g_triangles{0}, g_batches{0}, g_early{0}, g_largest{0}, g_largestScratch{0}, g_largestUs{0},
    g_atOnceThread{0}, g_atOnceRender{0}, g_atOnceUpdatable{0};

void Max(std::atomic<uint64_t>& m, uint64_t v)
{
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v)) {}
}
std::atomic<bool> g_loggedQueued{false}, g_loggedUpdatable{false};

void LogGeometry(const char* what, const uint8_t* geometry)
{
    char hex[3 * 12 + 1] = {};
    for (int i = 0; i < 12; ++i) sprintf_s(hex + 3 * i, 4, "%02X ", geometry[0x6C + i]);
    Log("[rtbatch] first %s geometry: initializer bytes +0x6C..+0x77 %s(%u primitives)", what, hex,
        *reinterpret_cast<const uint32_t*>(geometry + kPrimitives));
}

// The manager's queued requests (render thread: with the budget <= 0 only the render thread changes the queue): their
// number and the scratch their geometries' builds need. False if the memory could not be read.
bool QueueState(const void* manager, int32_t* count, uint64_t* scratch)
{
    *count = 0;
    *scratch = 0;
    __try {
        const uint8_t* m = static_cast<const uint8_t*>(manager);
        const int32_t num = *reinterpret_cast<const int32_t*>(m + kQueueCount);
        const int32_t numFree = *reinterpret_cast<const int32_t*>(m + kQueueFreeCount);
        if (num <= 0 || num - numFree <= 0) return true;
        const uint8_t* entries = *reinterpret_cast<const uint8_t* const*>(m + kQueueEntries);
        const uint32_t* heap = *reinterpret_cast<const uint32_t* const*>(m + kQueueBitsHeap);
        const uint32_t* bits = heap ? heap : reinterpret_cast<const uint32_t*>(m + kQueueBitsInline);
        for (int32_t i = 0; i < num; ++i) {
            if (!(bits[i >> 5] & (1u << (i & 31)))) continue;  // a free entry
            ++*count;
            const uint8_t* geometry = *reinterpret_cast<const uint8_t* const*>(entries + i * kQueueEntrySize + kQueueEntryGeometry);
            const uint8_t* rhi = geometry ? *reinterpret_cast<const uint8_t* const*>(geometry + kRhiGeometry) : nullptr;
            if (rhi) *scratch += *reinterpret_cast<const uint64_t*>(rhi + kBuildScratch);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Builds what is queued with the game's ProcessBuildRequests (all of it) on the given command list.
void BuildQueued(void* commandList, bool early)
{
    int32_t n = 0;
    uint64_t scratch = 0;
    const bool known = QueueState(g_manager, &n, &scratch);
    g_pending = 0;
    g_pendingScratch = 0;
    if (known && n <= 0) return;  // every queued request was dropped meanwhile
    LARGE_INTEGER a, b, f;
    QueryPerformanceCounter(&a);
    g_process(g_manager, commandList, true);
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    g_batches.fetch_add(1, std::memory_order_relaxed);
    if (early) g_early.fetch_add(1, std::memory_order_relaxed);
    Max(g_largest, static_cast<uint64_t>(n));
    Max(g_largestScratch, scratch);
    Max(g_largestUs, QpcToMicroseconds(static_cast<uint64_t>(b.QuadPart - a.QuadPart), static_cast<uint64_t>(f.QuadPart)));
}

int32_t Gate(void* manager, void* geometry, int32_t priority, int32_t mode)
{
    // Create's return address: Gate was reached by Create's call (the trampoline only jumps), so it sits Create's frame
    // above Gate's own return address.
    void* const* const ret = static_cast<void* const*>(_AddressOfReturnAddress());
    const bool fromInitRhi = ret[1 + kCreateFrame / sizeof(void*)] == g_initRhiReturn;
    if (!fromInitRhi || !g_on.load(std::memory_order_relaxed) || priority != kNormal || mode != kBuild || *g_budget > 0)
        return g_request(manager, geometry, priority, mode);
    uint8_t fastBuild = 1, allowUpdate = 1;
    uint32_t primitives = 0;
    uint64_t scratch = 0;
    __try {
        const uint8_t* g = static_cast<const uint8_t*>(geometry);
        fastBuild = g[kFastBuild];
        allowUpdate = g[kAllowUpdate];
        primitives = *reinterpret_cast<const uint32_t*>(g + kPrimitives);
        if (const uint8_t* rhi = *reinterpret_cast<const uint8_t* const*>(g + kRhiGeometry))
            scratch = *reinterpret_cast<const uint64_t*>(rhi + kBuildScratch);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return g_request(manager, geometry, priority, mode);
    }
    const bool renderThread = GetCurrentThreadId() == g_renderThread.load(std::memory_order_relaxed);
    if (!RayTracingBuildBatchDecide(true, priority, mode, true, renderThread, renderThread && g_inRender > 0, *g_budget, fastBuild,
                                    allowUpdate)) {
        if (fastBuild || allowUpdate) {
            g_atOnceUpdatable.fetch_add(1, std::memory_order_relaxed);
            if (!g_loggedUpdatable.exchange(true)) LogGeometry("updatable (built at once)", static_cast<const uint8_t*>(geometry));
        } else if (!renderThread) {
            g_atOnceThread.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_atOnceRender.fetch_add(1, std::memory_order_relaxed);
        }
        return g_request(manager, geometry, priority, mode);
    }
    if (!g_loggedQueued.exchange(true)) LogGeometry("batched", static_cast<const uint8_t*>(geometry));
    // The cap: past kFlushScratch the queue is built first (before this request is queued). The running sum is an upper
    // bound; the engine's queue gives the exact one.
    if (g_pending && g_pendingScratch + scratch > kFlushScratch) {
        int32_t n = 0;
        uint64_t queued = 0;
        if (!QueueState(g_manager, &n, &queued)) queued = g_pendingScratch;
        if (RayTracingBuildBatchFlushFirst(queued, n, scratch)) BuildQueued(g_immediate, true);
        else g_pendingScratch = queued;
    }
    g_queued.fetch_add(1, std::memory_order_relaxed);
    g_triangles.fetch_add(primitives, std::memory_order_relaxed);
    ++g_pending;
    g_pendingScratch += scratch;
    return g_queue(manager, geometry, priority, mode);
}

void RenderHook(void* renderer, void* graphBuilder)
{
    g_renderThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (g_pending)
        BuildQueued(*reinterpret_cast<void**>(static_cast<uint8_t*>(graphBuilder) + kCommandListInBuilder), false);
    ++g_inRender;
    g_render(renderer, graphBuilder);
    --g_inRender;
}

bool Fail(const char* why)
{
    Log("[rtbatch] %s: RayTracingBuildBatch not installed", why);
    return false;
}

uint8_t* One(const GameModule& game, const char* pattern)
{
    const auto all = FindAll(game.text, ParsePattern(pattern));
    return all.size() == 1 ? all[0] : nullptr;
}

}  // namespace

// The decision for one build request (pure, for the self-test too): true = queue it for the frame's batch.
bool RayTracingBuildBatchDecide(bool on, int32_t priority, int32_t mode, bool fromInitRhi, bool renderThread, bool inRender,
                                int32_t budget, uint8_t fastBuild, uint8_t allowUpdate)
{
    return on && fromInitRhi && priority == kNormal && mode == kBuild && budget <= 0 && renderThread && !inRender && !fastBuild &&
           !allowUpdate;
}

bool InstallRayTracingBuildBatch(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!beforeGameCode) return Fail("only at start-up");
    const auto& game = Game();
    uint8_t* const initRhi = One(game, kInitRhi);
    if (!initRhi) return Fail("FRayTracingGeometry::InitRHI not found exactly once");
    uint8_t* const create = RipTarget(initRhi + kInitRhiCall, 1, 5);
    if (!MatchesAt(create, ParsePattern(kCreateHead))) return Fail("InitRHI's call is not CreateRayTracingGeometry");
    uint8_t* const site = One(game, kCreateRequest);
    if (!site || site < create || site > create + 0x400) return Fail("CreateRayTracingGeometry's build request not found");
    uint8_t* const call = site + kCreateRequestCall;
    uint8_t* const request = RipTarget(call, 1, 5);
    if (!MatchesAt(request, ParsePattern(kRequest))) return Fail("unexpected RequestBuildAccelerationStructure");
    if (RipTarget(request + 0x10, 2, 6) != request + kRequestNow || RipTarget(request + 0x19, 2, 6) != request + kRequestNow ||
        !MatchesAt(request + kRequestNow, ParsePattern(kRequestNowCode)))
        return Fail("RequestBuildAccelerationStructure's \"build at once\" path differs");
    if (memcmp(request, kRequestPrologue, sizeof kRequestPrologue) != 0 || memcmp(request + 0xD, kMovRsiRcx, sizeof kMovRsiRcx) != 0)
        return Fail("unexpected RequestBuildAccelerationStructure prologue");
    if (!MatchesAt(request + kQueueFreeAt, ParsePattern(kQueueFree)) || !MatchesAt(request + kQueueAddAt, ParsePattern(kQueueAdd)) ||
        !MatchesAt(request + kQueueBitsAt, ParsePattern(kQueueBits)))
        return Fail("RequestBuildAccelerationStructure's queue (the manager's request array) differs");
    void* const immediate = RipTarget(request + kRequestNow + kRequestNowList, 3, 7);
    if (!game.data.Contains(static_cast<const uint8_t*>(immediate))) return Fail("the immediate command list is not in the game's data");
    auto* const budget = reinterpret_cast<const volatile int32_t*>(RipTarget(request + 6, 2, 7));
    if (!game.data.Contains(const_cast<const int32_t*>(budget))) return Fail("the build budget is not in the game's data");
    uint8_t* const manager = RipTarget(site + kCreateRequestManager, 3, 7);
    uint8_t* const update = One(game, kWorldUpdate);
    if (!update || RipTarget(update + kWorldUpdateManager, 3, 7) != manager || !game.data.Contains(manager))
        return Fail("the scene's ray-tracing update not found (or another geometry manager)");
    uint8_t* const process = RipTarget(update + kWorldUpdateProcess, 1, 5);
    if (!MatchesAt(process, ParsePattern(kProcess))) return Fail("unexpected ProcessBuildRequests");
    uint8_t* const render = One(game, kRender);
    if (!render || !MatchesAt(render + kRenderWorldUpdateCall, ParsePattern(kRenderCallCode)) ||
        RipTarget(render + kRenderWorldUpdateCall + 6, 1, 5) != update)
        return Fail("the scene renderer's Render not found");
    void** slot = nullptr;
    int slots = 0;
    for (auto** p = reinterpret_cast<void**>(game.rdata.begin); reinterpret_cast<uint8_t*>(p + 1) <= game.rdata.end; ++p)
        if (*p == render) { slot = p; ++slots; }
    if (slots != 1) return Fail("Render's vtable slot not found exactly once");

    g_region = AllocNearGame(64);
    if (!g_region) return Fail("no memory near the game");
    auto failFree = [](const char* why) {   // nothing points at the region yet: give it back
        VirtualFree(g_region, 0, MEM_RELEASE);
        g_region = nullptr;
        return Fail(why);
    };
    // [0] jmp [rip] -> Gate; [16] the queue path: RequestBuild's prologue and mov rsi,rcx, then jmp [rip] -> +0x1F
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const gate = reinterpret_cast<void*>(&Gate);
    uint8_t* const resume = request + kRequestQueuePath;
    memcpy(g_region, jmpAbs, 6);
    memcpy(g_region + 6, &gate, 8);
    memcpy(g_region + 16, kRequestPrologue, sizeof kRequestPrologue);
    memcpy(g_region + 22, kMovRsiRcx, sizeof kMovRsiRcx);
    memcpy(g_region + 25, jmpAbs, 6);
    memcpy(g_region + 31, &resume, 8);
    DWORD old;
    if (!VirtualProtect(g_region, 64, PAGE_EXECUTE_READ, &old)) return failFree("trampoline protection");
    FlushInstructionCache(GetCurrentProcess(), g_region, 64);
    const int64_t rel = g_region - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return failFree("out of reach");

    g_request = reinterpret_cast<RequestFn>(request);
    g_queue = reinterpret_cast<RequestFn>(g_region + 16);
    g_process = reinterpret_cast<ProcessFn>(process);
    g_render = reinterpret_cast<RenderFn>(render);
    g_manager = manager;
    g_budget = budget;
    g_initRhiReturn = initRhi + kInitRhiCall + 5;
    g_immediate = immediate;
    // The flush first, so a queued request always has its batch; then the gate.
    if (!WritePointer(slot, reinterpret_cast<void*>(&RenderHook))) return failFree("could not write Render's vtable slot");
    uint8_t c[5] = {0xE8};
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(c + 1, &r32, 4);
    if (!WriteCode(call, c)) {
        WritePointer(slot, render);
        return failFree("call patch failed");
    }
    g_callSite = call;
    g_slot = slot;
    g_installed = true;
    Log("[rtbatch] InitRHI exe+0x%X, CreateRayTracingGeometry's request exe+0x%X -> RequestBuildAccelerationStructure exe+0x%X "
        "(budget %d), ProcessBuildRequests exe+0x%X, Render exe+0x%X (slot exe+0x%X): installed",
        static_cast<unsigned>(initRhi - game.base), static_cast<unsigned>(call - game.base), static_cast<unsigned>(request - game.base),
        *budget, static_cast<unsigned>(process - game.base), static_cast<unsigned>(render - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(slot) - game.base));
    return true;
}

void SetRayTracingBuildBatch(bool on)
{
    if (!g_installed) return;
    if (g_on.exchange(on) != on)
        Log("[rtbatch] %s", on ? "on: new meshes' ray-tracing builds as one batch at the start of the frame's render"
                               : "off: each new mesh's ray-tracing build at once, as the game does");
}

void LogRayTracingBuildBatchStats()
{
    if (!g_installed) return;
    const uint64_t q = g_queued.exchange(0), t = g_triangles.exchange(0), b = g_batches.exchange(0), e = g_early.exchange(0),
                   l = g_largest.exchange(0),
                   ls = g_largestScratch.exchange(0), lu = g_largestUs.exchange(0), th = g_atOnceThread.exchange(0),
                   r = g_atOnceRender.exchange(0), u = g_atOnceUpdatable.exchange(0);
    if (q || th || r || u)
        Log("[rtbatch] %llu new meshes' builds (%llu triangles) in %llu batches, largest %llu builds / %.1f MB scratch, longest "
            "batch call %.2f ms, %llu of them early (the queue at 64 MiB of scratch); built at once: %llu on other threads, %llu "
            "during a render, %llu updatable",
            static_cast<unsigned long long>(q), static_cast<unsigned long long>(t), static_cast<unsigned long long>(b),
            static_cast<unsigned long long>(l), ls / 1048576.0, lu / 1000.0, static_cast<unsigned long long>(e),
            static_cast<unsigned long long>(th),
            static_cast<unsigned long long>(r), static_cast<unsigned long long>(u));
}

bool RayTracingBuildBatchInfo(RayTracingBuildBatchInfoOut* out)
{
    if (!g_installed) return false;
    out->callSite = g_callSite;
    out->region = g_region;
    out->slot = g_slot;
    out->hook = reinterpret_cast<void*>(&RenderHook);
    out->request = reinterpret_cast<uint8_t*>(g_request);
    out->render = reinterpret_cast<void*>(g_render);
    out->immediate = g_immediate;
    return true;
}

bool RayTracingBuildBatchFlushFirst(uint64_t queuedScratch, int32_t queued, uint64_t scratch)
{
    return queued > 0 && queuedScratch + scratch > kFlushScratch;
}

bool RayTracingBuildBatchQueueForTest(const void* manager, int32_t* count, uint64_t* scratch)
{
    return QueueState(manager, count, scratch);
}

}  // namespace opf
