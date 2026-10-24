// Water-info captures (WaterCaptureFsrSkip, WaterCaptureNoRhiFlush, WaterCaptureNoOcclusionWait, WaterCaptureFastMeshScan;
// workers bridgelake, waterrdg and waterscan 2026-10-22, public since 1.2.0; all four default 1, installed at start-up when
// any of them is on, then switched live; fixes-explained ## 60, 63, 64).
//
// The stone-bridge and lake hitches of the Imperial City flight are render-thread-bound frames: the UE 5.3 Water plugin
// re-renders a water zone's "water info" texture (UE::WaterInfo::UpdateWaterInfoRendering: three full FSceneRenderers,
// depth of every landscape component in the zone, the water bodies' colour and dilation passes) whenever
// AWaterZone::MarkForRebuild(UpdateWaterInfoTexture) was called, which World Partition streaming does for every level
// added or removed that holds a landscape proxy or a water body touching the zone (AWaterZone::OnLevelChanged). Every
// update has new inputs (a landscape proxy or a body came or went), so the updates themselves are not skipped; the four
// keys remove work inside them that changes no pixel. All four rest on one render-thread scope: the water-info render
// (UpdateWaterInfoRendering_RenderThread exe+0x42EEDA0, UE 5.3 Water plugin; its one caller, the WaterInfoCommand's
// lambda, calls it at exe+0x42B5F2B) is detoured so the hooks below know when the render thread is inside it.
// Lab diagnostic WaterInfoProbe (lab build only): times both halves and fingerprints what each update renders:
//  - game thread (only with WaterInfoProbe != 0): UpdateWaterInfoRendering(FSceneInterface*, const FRenderingContext&)
//    exe+0x42ED870 (its one caller, FWaterViewExtension::SetupViewFamily, calls it at exe+0x42E6955); FRenderingContext = {AWaterZone* +0, UTextureRenderTarget2D* +8,
//    TArray<UWaterBodyComponent*> +0x10, TArray<TWeakObjectPtr<UPrimitiveComponent>> +0x20, float CaptureZ +0x30}
//    (WaterInfoRendering.h). The fingerprint: zone, render target, the sorted body pointers, the sorted ground weak
//    pointers (object index + serial), CaptureZ. Each call logs whether it equals the zone's previous one and how many
//    ground components / bodies came and went. Mode 1 = timing and fingerprints only (nothing changed in the game).
// Sites (after the 2026-10-22 review): each hooked function is found through its single caller (a byte pattern around the
// call, unique in .text, rel32 masked) and its first bytes must match too; the FSR3 slot is the only .rdata pointer to the
// one function in .text that starts with its prologue. Any miss: that part stays off with a log line, nothing written.
// The entry detours are written only while the game starts (version.dll route); on the OBSE64 route the four keys stay off.
//
// WaterCaptureFsrSkip ([Fixes] WaterCaptureFsrSkip=1; needs the render-thread detour only). Every FSceneRenderer calls the FX systems' PostRenderOpaque; AMD's FSR3
// plugin's FX system (FFXFSR3FXSystem, vtable slot exe+0x799AF18 -> exe+0x407DEA0, its only pointer) copies the renderer's scene colour into
// its persistent "FFXFSR3SceneColorPreAlpha" texture and first RE-CREATES that texture whenever its size or format differs
// from the renderer's scene colour (RHICreateTexture, 97 % of the callback's time). The water-info renderers have a
// 32x32 or 256x256 scene colour, so each water-info update re-creates it at that size (in the first of its three
// renderers; all three copy into it) and the next main-view frame re-creates it at the main view's size: two texture
// creations on the render thread per update. The copy is read only by FSR3's upscaler (reactive mask, exe+0x4073060
// reads +0x1C8), which runs only for views FSR3 upscales; the water-info views use AAM_None
// (CreateWaterInfoSceneRenderer) and are never upscaled, and the main view always copies its own scene colour over the
// whole texture in PostRenderOpaque (an RDG copy pass whose Execute, exe+0x4077C20, records an RHI CopyTexture of
// min(scene-colour extent, texture size) = the whole texture) before its upscaler reads it in post-processing. So skipping the callback while the render thread is inside UpdateWaterInfoRendering_RenderThread
// leaves every image identical; only the two texture re-creations and the capture's unused copy pass are gone.
//
// WaterCaptureNoRhiFlush ([Fixes] WaterCaptureNoRhiFlush=1; same detours). A scene renderer made
// on its own (FSceneRenderer::CreateSceneRenderer for one view family, as CreateWaterInfoSceneRenderer does) defaults to
// bIsFirstSceneRenderer = true (SceneRendering.cpp:2498), so each of the three water-info renderers calls
// GraphBuilder.SetFlushResourcesRHI() (DeferredShadingRenderer.cpp:2876) and its FRDGBuilder::Execute ends the barrier
// set-up with EndFlushResourcesRHI (exe+0x26B77C0, reached only by a tail jump from Execute's pre-work lambda at
// exe+0x26A65D7): ImmediateFlush(FlushRHIThreadFlushResources) = the render thread waits until the RHI thread has
// translated everything queued, frees unreferenced pipeline states and enqueues RHIPerFrameRHIFlushComplete. When the
// engine renders several view families in one frame (SceneRendering.cpp:4019) only the first one flushes; the main
// view's renderer flushes later in the same frame. The flush is synchronisation and resource release only: no pass,
// draw or texture content depends on it. Skipping it while the render thread is inside the water-info render gives
// the water-info renderers the flush behaviour of UE's non-first view families; the same flush still happens once per
// frame in the main renderer.
//
// WaterCaptureNoOcclusionWait (worker waterrdg 2026-10-22; [Fixes] WaterCaptureNoOcclusionWait=1; same detours). With the flush skipped, the water-info renderers' render-thread wait moves to
// FSceneRenderer::WaitOcclusionTests (exe+0x22EC8E0, called from FVisibilityTaskData::LaunchVisibilityTasks at the start of
// each renderer's visibility; SceneOcclusion.cpp:1555): with an RHI thread and one buffered occlusion frame (desktop
// WITH_MGPU: GetNumBufferedFrames = 1) it waits on OcclusionSubmittedFence[0] (FRHICommandListExecutor::WaitOnRHIThreadFence)
// and clears it, and every renderer that tests occlusion pushes a new fence there after its occlusion pass
// (FenceOcclusionTests). So water renderer 1 waits until the RHI thread has translated the main view's previous frame up
// to its occlusion pass, renderer 2 waits for renderer 1's, renderer 3 for renderer 2's, and the main view for renderer 3's.
// The fence exists so that a renderer never reads occlusion-query results the RHI thread has not submitted yet; every
// query read (RHIGetRenderQueryResult in SceneVisibility / SceneOcclusion, HZB and occlusion feedback) goes through the
// view's FSceneViewState, and the water-info views have none (WaterInfoRendering.cpp:688, SceneViewStateInterface =
// nullptr), so they read no query results and their wait protects nothing. Skipping it inside the water-info render
// leaves the slot to the renderers' own FenceOcclusionTests: the main view then waits on the latest fence pushed before
// it (the same as stock when the water renderers fence; its own previous-frame fence when they do not, which is what it
// waits on in every frame without a water-info update). Either fence comes after the main view's own previous-frame
// queries in RHI-thread order, so the main view's reads are as protected as in stock; no pass, draw or texture changes.
//
// WaterCaptureFastMeshScan (worker waterscan 2026-10-22; [Fixes] WaterCaptureFastMeshScan=1; lab builds: 2 = also run the
// stock scan's test on every proxy afterwards and count differences; same detours). The colour and dilation
// passes of each water-info update construct FEnableWaterInfoMeshesScoped (exe+0x42B0540, WaterInfoRendering.cpp:307; the
// remaster's code is the 5.3 source): for every proxy in FScene::PrimitiveSceneProxies (FSceneInterface::GetPrimitiveSceneProxies,
// vtable +0x3F8) it reads the proxy's PrimitiveComponentId (+0x2A8) and looks it up in the view's ShowOnlyPrimitives TSet; on a
// hit it sets the proxy enabled (clears bit 0x10 of +0x6E) and appends it to its TArray (the destructor disables exactly
// those). Reading +0x2A8 of every proxy (tens of thousands of heap objects) costs a cache miss each: ~0.5 ms per scan, two
// scans per update. FScene keeps the same ids densely packed in PrimitiveComponentIds (GetScenePrimitiveComponentIds, vtable
// +0x400), index for index with PrimitiveSceneProxies: both arrays are only changed together inside
// FScene::UpdateAllPrimitiveSceneInfos on the render thread (RendererScene.cpp: Add / swap / RemoveAt of both in the same
// loops, PrimitiveSceneInfo.cpp:1544 stores SceneInfo->PrimitiveComponentId at the new index in AddToScene, which runs
// before UpdateAllPrimitiveSceneInfos returns), and SceneInfo and proxy both copy UPrimitiveComponent::ComponentId (set once
// in the component's constructor) when the primitive is added. So PrimitiveComponentIds[i] == PrimitiveSceneProxies[i]->
// GetPrimitiveComponentId() whenever the render thread is outside that update, as here. The hook scans the packed ids with
// the stock TSet lookup (same fields, same hash), behind an exact bitmap of the set's ids, collects the proxies at the
// matching indices in index order, and runs the stock constructor on a stand-in scene whose GetPrimitiveSceneProxies returns
// only those: the stock code then makes its own test, writes and appends for each of them, so the same proxies are enabled
// in the same order and the same TArray results. Anything unexpected (the scene's slots are not the two getters, the arrays
// differ in length, the first or last packed id is not its proxy's own id) runs the stock constructor unchanged.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace opf {
namespace {

// Anchors: the single caller of each hooked function (rel32 masked) and where its call / jmp instruction starts.
constexpr const char* kRtCall = "4C 89 21 48 8B D5 48 8B CB E8 ?? ?? ?? ?? 48 8B 6C 24 40 48 85 DB";  // lambda -> RT fn
constexpr size_t kRtCallAt = 9;
constexpr const char* kGtCall = "48 8D 14 F2 48 8B 88 E0 01 00 00 E8 ?? ?? ?? ??";  // SetupViewFamily -> GT fn
constexpr size_t kGtCallAt = 11;
constexpr const char* kFlushJmp = "48 8B 4F 10 48 8B 5C 24 50 48 83 C4 40 5F E9 ?? ?? ?? ??";  // pre-work lambda -> EndFlush
constexpr size_t kFlushJmpAt = 14;
// mov rax,rsp; mov [rax+18h],rbx; push rbp; push rsi; push rdi; push r12; push r13   (14 bytes, no RIP-relative)
constexpr uint8_t kGtPro[14] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
// mov [rsp+18h],rbx; push rbp; push rsi; push rdi; push r12; push r13   (12 bytes)
constexpr uint8_t kRtPro[12] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
// push rbp; push rsi; push rdi; push r15; lea rbp,[rsp-0E8h]   (14 bytes; unique in .text)
constexpr uint8_t kFsrPro[14] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x18, 0xFF, 0xFF, 0xFF};
// sub rsp,28h; cmp byte ptr [rcx+780h],0; je ...  (the first 11 bytes are moved; the je is jumped back to)
constexpr uint8_t kEndFlushPro[13] = {0x48, 0x83, 0xEC, 0x28, 0x80, 0xB9, 0x80, 0x07, 0x00, 0x00, 0x00, 0x0F, 0x84};
constexpr size_t kEndFlushMoved = 11;
// FSceneRenderer::WaitOcclusionTests: mov rax,rsp; sub rsp,48h; cmp byte ptr [GIsRunningRHIInSeparateThread],0; je;
// cmp byte ptr [rcx+106h],0 (ViewFamily.bIsMultipleViewFamily); ... (two callers, so found by its own unique body)
constexpr const char* kWaitOccBody =
    "48 8B C4 48 83 EC 48 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 80 B9 06 01 00 00 00 48 89 70 F8 4C 89 60 E8 4C 89 70 E0 0F 84";
constexpr uint8_t kWaitOccPro[9] = {0x48, 0x8B, 0xC4, 0x48, 0x83, 0xEC, 0x48, 0x80, 0x3D};
constexpr size_t kWaitOccMoved = 7;  // mov rax,rsp; sub rsp,48h (no RIP-relative); back to the cmp at entry+7

// FEnableWaterInfoMeshesScoped::FEnableWaterInfoMeshesScoped(FSceneInterface*, const FSceneView&): its whole body up to the
// enable write (two callers, so found by its own unique body); the offsets the replica uses are all in it.
constexpr const char* kEnableBody =
    "40 53 55 48 83 EC 38 33 C0 49 8B E8 48 89 01 4C 8B C2 48 89 41 08 48 8B D9 38 85 08 16 00 00 0F 84 ?? ?? ?? ?? "
    "48 8B 02 49 8B C8 4C 89 74 24 60 48 8D 54 24 20 4C 89 7C 24 30 FF 90 F8 03 00 00 4C 8B 74 24 20 48 63 44 24 28 "
    "4D 8D 3C C6 4D 3B F7 0F 84 ?? ?? ?? ?? 48 89 74 24 50 48 89 7C 24 58 49 8B 3E 48 85 FF 0F 84 ?? ?? ?? ?? "
    "8B 85 C0 15 00 00 44 8B 8F A8 02 00 00 3B 85 EC 15 00 00 74 ?? 8B 8D 00 16 00 00 4C 8D 85 F0 15 00 00 49 8B 50 08 "
    "FF C9 49 23 C9 48 85 D2 4C 0F 45 C2 41 8B 04 88 83 F8 FF 74 ?? 48 8B 95 B8 15 00 00 90 48 63 C8 48 8D 04 49 "
    "44 39 0C 82 74 ?? 8B 44 82 04 83 F8 FF 75 ?? EB ?? 80 67 6E EF";
constexpr uint8_t kEnablePro[9] = {0x40, 0x53, 0x55, 0x48, 0x83, 0xEC, 0x38, 0x33, 0xC0};
constexpr size_t kEnableMoved = 7;  // push rbx; push rbp; sub rsp,38h (no RIP-relative); back to the xor at entry+7
// FSceneView::ShowOnlyPrimitives (TOptional<TSet<FPrimitiveComponentId>>) and the proxy field, as the body above reads them.
constexpr size_t kShowOnlySet = 0x1608, kSetElems = 0x15B8, kSetNum = 0x15C0, kSetFree = 0x15EC, kSetHashInline = 0x15F0,
                 kSetHashHeap = 0x15F8, kSetHashSize = 0x1600, kProxyId = 0x2A8;
constexpr size_t kSlotProxies = 0x3F8, kSlotIds = 0x400;  // FSceneInterface::GetPrimitiveSceneProxies / GetScenePrimitiveComponentIds

using FsrFn = void (*)(void* self, void* graphBuilder, void* views, void* sceneUniforms, uint64_t allowGpuParticles);
using EndFlushFn = void (*)(void* builder);
using GtFn = void (*)(void* scene, const uint8_t* ctx);
using RtFn = void (*)(void* rhiCmdList, const void* params);

struct Sites {
    uint8_t* rt = nullptr;
    uint8_t* endFlush = nullptr;
    uint8_t* waitOcc = nullptr;
    uint8_t* enable = nullptr;
    uint8_t* fsrFn = nullptr;
    void** fsrSlot = nullptr;
};
Sites g_sites;

FsrFn g_fsr = nullptr;
std::atomic<bool> g_fsrSkip{false};
std::atomic<uint64_t> g_fsrSkipped{0};
EndFlushFn g_endFlush = nullptr;
std::atomic<bool> g_noFlush{false};
std::atomic<uint64_t> g_flushSkipped{0};
using WaitOccFn = void (*)(void* renderer, void* rhiCmdList);
WaitOccFn g_waitOcc = nullptr;
std::atomic<bool> g_noOccWait{false};
std::atomic<uint64_t> g_occSkipped{0};
RtFn g_rt = nullptr;
std::atomic<uint64_t> g_updates{0};  // water-info renders since start (render thread)
thread_local bool t_inWaterInfo = false;  // inside the water-info render and skipping FSR3's copy
thread_local bool t_inWater2 = false;      // inside the water-info render (any mode)

// The two flags for the length of one water-info render; cleared however the call ends.
struct WaterScope {
    explicit WaterScope(bool skipFsr) { t_inWaterInfo = skipFsr; t_inWater2 = true; }
    ~WaterScope() { t_inWaterInfo = t_inWater2 = false; }
    WaterScope(const WaterScope&) = delete;
    WaterScope& operator=(const WaterScope&) = delete;
};


void RtHook(void* cmd, const void* params)
{
    g_updates.fetch_add(1, std::memory_order_relaxed);
    const bool skip = g_fsrSkip.load(std::memory_order_relaxed);
        if (!skip && !g_noFlush.load(std::memory_order_relaxed) && !g_noOccWait.load(std::memory_order_relaxed)) return g_rt(cmd, params);
        WaterScope scope(skip);
        g_rt(cmd, params);
        return;
}

void FsrHook(void* self, void* graphBuilder, void* views, void* sceneUniforms, uint64_t allowGpuParticles)
{
    if (t_inWaterInfo) {  // a water-info renderer: FSR3 never upscales it, its pre-alpha copy is never read
        g_fsrSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_fsr(self, graphBuilder, views, sceneUniforms, allowGpuParticles);
}

void EndFlushHook(void* builder)
{
    if (t_inWater2 && g_noFlush.load(std::memory_order_relaxed)) {
        g_flushSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_endFlush(builder);
}

// Guard (review occreview): the skip relies on the single-family fence path, where every renderer waits on slot [0] and
// the main view waits on the latest fence. If any renderer's ViewFamily.bIsMultipleViewFamily (renderer+0x106, the byte
// WaitOcclusionTests tests) is set, the queue logic counts fences per view state instead; stop skipping for the session.
std::atomic<bool> g_occMultiFamily{false};

struct ArrView {  // TConstArrayView<T> as returned through the hidden pointer
    const void* data;
    int32_t num;
};
using EnableFn = void* (*)(void* self, void* scene, const uint8_t* view);
EnableFn g_enable = nullptr;
std::atomic<int> g_fastScan{0};
std::atomic<uint64_t> g_scanStock{0}, g_scanFast{0}, g_scanFallback{0}, g_scanHits{0}, g_scanPrims{0};
std::atomic<const void*> g_sceneVt{nullptr};  // the last scene vtable whose two slots were checked
std::atomic<uint32_t> g_offProxies{0}, g_offIds{0};
thread_local std::vector<const void*> t_cand;

// `mov rax,[rcx+X]; mov [rdx],rax; mov eax,[rcx+X+8]; mov [rdx+8],eax; mov rax,rdx; ret`: returns X, or 0.
uint32_t ArrayGetterOffset(const uint8_t* f)
{
    static const uint8_t a[] = {0x48, 0x8B, 0x81}, b[] = {0x48, 0x89, 0x02, 0x8B, 0x81}, c[] = {0x89, 0x42, 0x08, 0x48, 0x8B, 0xC2, 0xC3};
    if (!f || !Game().text.Contains(const_cast<uint8_t*>(f)) || memcmp(f, a, 3) || memcmp(f + 7, b, 5) || memcmp(f + 16, c, 7)) return 0;
    int32_t x, y;
    memcpy(&x, f + 3, 4);
    memcpy(&y, f + 12, 4);
    return x > 0 && x < 0x100000 && y == x + 8 ? static_cast<uint32_t>(x) : 0;
}

// The scene's packed proxy and component-id arrays (both through the scene's own getters), or false.
bool SceneArrays(const void* scene, ArrView* proxies, ArrView* ids)
{
    const auto* vt = *static_cast<const uint8_t* const* const*>(scene);
    if (vt != g_sceneVt.load(std::memory_order_acquire)) {
        const uint32_t op = ArrayGetterOffset(vt[kSlotProxies / 8]), oi = ArrayGetterOffset(vt[kSlotIds / 8]);
        if (!op || !oi || op == oi) return false;
        g_offProxies = op;
        g_offIds = oi;
        g_sceneVt.store(vt, std::memory_order_release);
        Log("[water] WaterCaptureFastMeshScan: scene vtable exe+0x%llX, proxies +0x%X, component ids +0x%X",
            static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(vt) - Game().base), op, oi);
    }
    const auto* s = static_cast<const uint8_t*>(scene);
    memcpy(proxies, s + g_offProxies.load(std::memory_order_relaxed), sizeof(ArrView));
    memcpy(ids, s + g_offIds.load(std::memory_order_relaxed), sizeof(ArrView));
    if (proxies->num != ids->num || proxies->num < 0) return false;
    if (proxies->num > 0) {  // spot check (two loads): the first and last packed ids are those proxies' own ids
        const auto* pp = static_cast<const uint8_t* const*>(proxies->data);
        const auto* ii = static_cast<const uint32_t*>(ids->data);
        for (const int32_t k : {0, proxies->num - 1})
            if (pp[k] && *reinterpret_cast<const uint32_t*>(pp[k] + kProxyId) != ii[k]) {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true))
                    Log("[water] WaterCaptureFastMeshScan: packed id [%d] %u differs from its proxy's id %u: stock scan for such calls",
                        k, ii[k], *reinterpret_cast<const uint32_t*>(pp[k] + kProxyId));
                return false;
            }
    }
    return true;
}

// The view's ShowOnlyPrimitives set, read exactly as the stock constructor reads it.
struct SetView {
    const uint8_t* elems = nullptr;
    const int32_t* hash = nullptr;
    uint32_t mask = 0;
    bool empty = true;
    bool Contains(uint32_t id) const  // TSet::Contains on a uint32 key: bucket id & (HashSize - 1), then the chain
    {
        for (int32_t i = hash[id & mask]; i != -1; i = *reinterpret_cast<const int32_t*>(elems + int64_t(i) * 12 + 4))
            if (*reinterpret_cast<const uint32_t*>(elems + int64_t(i) * 12) == id) return true;
        return false;
    }
};
SetView ReadSet(const uint8_t* view)
{
    SetView s;
    s.empty = *reinterpret_cast<const int32_t*>(view + kSetNum) == *reinterpret_cast<const int32_t*>(view + kSetFree);
    s.hash = *reinterpret_cast<const int32_t* const*>(view + kSetHashHeap);
    if (!s.hash) s.hash = reinterpret_cast<const int32_t*>(view + kSetHashInline);
    s.mask = static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(view + kSetHashSize)) - 1;
    s.elems = *reinterpret_cast<const uint8_t* const*>(view + kSetElems);
    return s;
}

// The proxies whose packed component id is in the set, in scene order (pure; the self-test checks it on fakes).
void CollectCandidates(const SetView& set, const uint32_t* ids, const void* const* proxies, int32_t n, std::vector<const void*>& out)
{
    out.clear();
    if (set.empty || n <= 0) return;
    uint64_t bits[64] = {};  // 4096-bit filter over id & 4095, built from every element reachable from the buckets
    const bool useBits = set.mask < 0x10000;
    if (useBits) {
        for (uint32_t b = 0; b <= set.mask; ++b)
            for (int32_t i = set.hash[b]; i != -1; i = *reinterpret_cast<const int32_t*>(set.elems + int64_t(i) * 12 + 4)) {
                const uint32_t id = *reinterpret_cast<const uint32_t*>(set.elems + int64_t(i) * 12);
                bits[(id >> 6) & 63] |= 1ull << (id & 63);
            }
    }
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = ids[i];
        if (useBits && !(bits[(id >> 6) & 63] & (1ull << (id & 63)))) continue;
        if (set.Contains(id)) out.push_back(proxies[i]);
    }
}

void* FakeProxiesGetter(const void*, ArrView* out)  // the stand-in scene's GetPrimitiveSceneProxies
{
    out->data = t_cand.data();
    out->num = static_cast<int32_t>(t_cand.size());
    return out;
}
void* g_fakeVt[kSlotProxies / 8 + 1] = {};  // only the one slot the stock constructor calls

// Self-test only: the stock constructor's steps in C++ (the real one's TArray growth needs the game's allocator, which the
// mapped exe of the self-test does not have): proxies from the scene's slot +0x3F8, null skipped, the set test on the
// proxy's own id, bit 0x10 of +0x6E cleared, appended in order.
const void* g_testOut[256];
void* TestStockCtor(void* self, void* scene, const uint8_t* view)
{
    auto* arr = static_cast<uint8_t*>(self);
    memset(arr, 0, 16);
    if (!view[kShowOnlySet]) return self;
    ArrView v{};
    using Getter = void* (*)(const void*, ArrView*);
    reinterpret_cast<Getter>((*static_cast<void* const* const*>(scene))[kSlotProxies / 8])(scene, &v);
    const SetView set = ReadSet(view);
    int32_t n = 0;
    for (int32_t i = 0; i < v.num && n < 256; ++i) {
        auto* p = static_cast<uint8_t* const*>(const_cast<void*>(v.data))[i];
        if (!p || set.empty || !set.Contains(*reinterpret_cast<const uint32_t*>(p + kProxyId))) continue;
        p[0x6E] &= 0xEF;
        g_testOut[n++] = p;
    }
    const void* d = g_testOut;
    memcpy(arr, &d, 8);
    memcpy(arr + 8, &n, 4);
    memcpy(arr + 12, &n, 4);
    return self;
}

void* EnableHook(void* self, void* scene, const uint8_t* view)
{
    const int mode = g_fastScan.load(std::memory_order_relaxed);
    ArrView proxies{}, ids{};
    if (!mode || !scene || !view || !view[kShowOnlySet] || !SceneArrays(scene, &proxies, &ids)) {
        if (mode && scene && view && view[kShowOnlySet]) g_scanFallback.fetch_add(1, std::memory_order_relaxed);
        void* r = g_enable(self, scene, view);
        g_scanStock.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    const SetView set = ReadSet(view);
    CollectCandidates(set, static_cast<const uint32_t*>(ids.data), static_cast<const void* const*>(proxies.data), ids.num, t_cand);
    void* fake[1] = {g_fakeVt};
    void* r = g_enable(self, fake, view);
    g_scanFast.fetch_add(1, std::memory_order_relaxed);
    g_scanHits.fetch_add(t_cand.size(), std::memory_order_relaxed);
    g_scanPrims.fetch_add(static_cast<uint64_t>(ids.num), std::memory_order_relaxed);
    return r;
}

void WaitOccHook(void* renderer, void* rhiCmdList)
{
    if (renderer && !g_occMultiFamily.load(std::memory_order_relaxed) && static_cast<const uint8_t*>(renderer)[0x106] != 0) {
        if (!g_occMultiFamily.exchange(true))
            Log("[water] WaterCaptureNoOcclusionWait: a renderer with several view families (+0x106 set) seen; occlusion waits no longer skipped this session");
    }
    if (t_inWater2 && g_noOccWait.load(std::memory_order_relaxed) && !g_occMultiFamily.load(std::memory_order_relaxed)) {
        // a water-info renderer: reads no query results. ~4.6 skips per update were counted (3 renderers): the extra calls
        // come from the second call site (exe+0x2315E70) or extra renderers; all are inside the water-info render (flag).
        g_occSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_waitOcc(renderer, rhiCmdList);
}

// The target of the one call / jmp the pattern finds (unique in .text), if its first bytes are `pro`.
uint8_t* FindByCaller(const char* pattern, size_t insnAt, const uint8_t* pro, size_t proLen, const char* what)
{
    const auto hits = FindAll(Game().text, ParsePattern(pattern));
    if (hits.size() != 1) {
        Log("[water] %s: its caller pattern matched %zu times (want 1)", what, hits.size());
        return nullptr;
    }
    uint8_t* target = RipTarget(hits[0] + insnAt, 1, 5);
    if (!Game().text.Contains(target) || memcmp(target, pro, proLen) != 0) {
        Log("[water] %s: the call at exe+0x%llX goes to exe+0x%llX, whose first bytes differ", what,
            static_cast<unsigned long long>(hits[0] + insnAt - Game().base), static_cast<unsigned long long>(target - Game().base));
        return nullptr;
    }
    return target;
}

Sites FindSites()
{
    Sites s;
    s.rt = FindByCaller(kRtCall, kRtCallAt, kRtPro, sizeof kRtPro, "UpdateWaterInfoRendering_RenderThread");
    s.endFlush = FindByCaller(kFlushJmp, kFlushJmpAt, kEndFlushPro, sizeof kEndFlushPro, "FRDGBuilder::EndFlushResourcesRHI");
    const auto occ = FindAll(Game().text, ParsePattern(kWaitOccBody));
    const auto en = FindAll(Game().text, ParsePattern(kEnableBody));
    if (en.size() == 1) s.enable = en[0];
    else Log("[water] FEnableWaterInfoMeshesScoped: its body matched %zu times (want 1)", en.size());
    if (occ.size() == 1) s.waitOcc = occ[0];
    else Log("[water] FSceneRenderer::WaitOcclusionTests: its body matched %zu times (want 1)", occ.size());
    Pattern fp;
    fp.bytes.assign(kFsrPro, kFsrPro + sizeof kFsrPro);
    fp.care.assign(sizeof kFsrPro, 1);
    const auto fns = FindAll(Game().text, fp);
    if (fns.size() == 1) {
        int n = 0;
        void** slot = nullptr;
        for (uint8_t* p = Game().rdata.begin; p + 8 <= Game().rdata.end; p += 8)
            if (*reinterpret_cast<uint8_t**>(p) == fns[0]) { slot = reinterpret_cast<void**>(p); ++n; }
        if (n == 1) { s.fsrFn = fns[0]; s.fsrSlot = slot; }
        else Log("[water] FFXFSR3FXSystem::PostRenderOpaque: %d .rdata pointers to it (want 1)", n);
    } else {
        Log("[water] FFXFSR3FXSystem::PostRenderOpaque: its first bytes matched %zu times (want 1)", fns.size());
    }
    return s;
}

// 12-byte `mov rax, hook; jmp rax` over the first n bytes (whole instructions, no RIP-relative); the trampoline runs
// them and jumps back. Nothing is written unless everything is ready.
bool Detour(uint8_t* at, const uint8_t* pro, size_t n, void* hook, void** orig, const char* what)
{
    if (!at || n < 12 || n > 16 || memcmp(at, pro, n) != 0) { Log("[water] %s: entry not found or its bytes differ, not hooked", what); return false; }
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!tramp) { Log("[water] %s: no memory for the trampoline (error %lu), not hooked", what, GetLastError()); return false; }
    memcpy(tramp, pro, n);
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(tramp + n, jmpAbs, sizeof jmpAbs);
    const uint64_t back = reinterpret_cast<uint64_t>(at + n);
    memcpy(tramp + n + sizeof jmpAbs, &back, sizeof back);
    DWORD old;
    VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    uint8_t detour[16] = {0x48, 0xB8};  // mov rax, hook ; jmp rax ; int3 padding
    const uint64_t h = reinterpret_cast<uint64_t>(hook);
    memcpy(detour + 2, &h, sizeof h);
    detour[10] = 0xFF;
    detour[11] = 0xE0;
    for (size_t i = 12; i < n; ++i) detour[i] = 0xCC;
    void* const prev = *orig;
    *orig = tramp;  // before the write: the hook may run as soon as the bytes are in
    if (!WriteCode(at, std::span<const uint8_t>(detour, n))) {
        *orig = prev;
        VirtualFree(tramp, 0, MEM_RELEASE);
        Log("[water] %s: could not write the entry detour, not hooked", what);
        return false;
    }
    return true;
}

// 5-byte jmp at EndFlushResourcesRHI's entry to a near stub (jmp [rip] -> EndFlushHook); the trampoline runs the two
// moved instructions (sub, cmp) and jumps back to the je at entry+11.
bool InstallEndFlushDetour(uint8_t* at)
{
    if (!at || memcmp(at, kEndFlushPro, sizeof kEndFlushPro) != 0) {
        Log("[water] WaterCaptureNoRhiFlush: EndFlushResourcesRHI not found or its bytes differ");
        return false;
    }
    uint8_t* stub = AllocNearGame(32);
    if (!stub) { Log("[water] WaterCaptureNoRhiFlush: no memory within reach of the game for the stub"); return false; }
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!tramp) {
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[water] WaterCaptureNoRhiFlush: no memory for the trampoline (error %lu)", GetLastError());
        return false;
    }
    memcpy(tramp, kEndFlushPro, kEndFlushMoved);
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(tramp + kEndFlushMoved, jmpAbs, 6);
    const uint64_t back = reinterpret_cast<uint64_t>(at + kEndFlushMoved);
    memcpy(tramp + kEndFlushMoved + 6, &back, 8);
    DWORD old;
    VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    memcpy(stub, jmpAbs, 6);
    const uint64_t hook = reinterpret_cast<uint64_t>(&EndFlushHook);
    memcpy(stub + 6, &hook, 8);
    VirtualProtect(stub, 32, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 32);
    uint8_t patch[kEndFlushMoved] = {0xE9};
    const int32_t rel = static_cast<int32_t>(stub - (at + 5));
    memcpy(patch + 1, &rel, 4);
    for (size_t i = 5; i < kEndFlushMoved; ++i) patch[i] = 0xCC;
    g_endFlush = reinterpret_cast<EndFlushFn>(tramp);
    if (!WriteCode(at, std::span<const uint8_t>(patch, kEndFlushMoved))) {
        g_endFlush = nullptr;
        VirtualFree(tramp, 0, MEM_RELEASE);
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[water] WaterCaptureNoRhiFlush: could not write the entry jump");
        return false;
    }
    return true;
}

// 5-byte jmp at a function's entry to a near stub (jmp [rip] -> hook); the trampoline runs the first `moved` bytes (whole
// instructions, no RIP-relative) and jumps back to entry+moved. The first `proLen` bytes must equal `pro`. Nothing is
// written unless everything is ready; *orig is set before the write.
bool InstallNearDetour(uint8_t* at, const uint8_t* pro, size_t proLen, size_t moved, void* hook, void** orig, const char* what)
{
    if (!at || moved < 5 || moved > proLen || memcmp(at, pro, proLen) != 0) {
        Log("[water] %s: entry not found or its bytes differ, not hooked", what);
        return false;
    }
    uint8_t* stub = AllocNearGame(32);
    if (!stub) { Log("[water] %s: no memory within reach of the game for the stub", what); return false; }
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!tramp) {
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[water] %s: no memory for the trampoline (error %lu)", what, GetLastError());
        return false;
    }
    memcpy(tramp, pro, moved);
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(tramp + moved, jmpAbs, 6);
    const uint64_t back = reinterpret_cast<uint64_t>(at + moved);
    memcpy(tramp + moved + 6, &back, 8);
    DWORD old;
    VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    memcpy(stub, jmpAbs, 6);
    const uint64_t h = reinterpret_cast<uint64_t>(hook);
    memcpy(stub + 6, &h, 8);
    VirtualProtect(stub, 32, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 32);
    uint8_t patch[16] = {0xE9};
    const int32_t rel = static_cast<int32_t>(stub - (at + 5));
    memcpy(patch + 1, &rel, 4);
    for (size_t i = 5; i < moved; ++i) patch[i] = 0xCC;
    void* const prev = *orig;
    *orig = tramp;
    if (!WriteCode(at, std::span<const uint8_t>(patch, moved))) {
        *orig = prev;
        VirtualFree(tramp, 0, MEM_RELEASE);
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[water] %s: could not write the entry jump", what);
        return false;
    }
    return true;
}

// ---- self-test fakes ----
std::atomic<int> g_fakeFsr{0}, g_fakeFlush{0}, g_fakeOcc{0};
bool g_fakeInsideOk = true;
void FakeFsr(void*, void*, void*, void*, uint64_t) { g_fakeFsr.fetch_add(1); }
void FakeFlush(void*) { g_fakeFlush.fetch_add(1); }
void FakeOcc(void*, void*) { g_fakeOcc.fetch_add(1); }
void FakeRt(void*, const void*)  // what the water-info render does: FSR3's PostRenderOpaque, a graph's flush, the occlusion wait
{
    g_fakeInsideOk = g_fakeInsideOk && t_inWater2;
    FsrHook(nullptr, nullptr, nullptr, nullptr, 0);
    EndFlushHook(nullptr);
    WaitOccHook(nullptr, nullptr);
}

}  // namespace

bool InstallWaterCapture(bool beforeGameCode, bool probe)
{
    if (!beforeGameCode) {
        Log("[water] WaterCapture*: can only be installed at start-up (version.dll), not installed");
        return false;
    }
    g_sites = FindSites();
    void* rt = nullptr;
    if (!Detour(g_sites.rt, kRtPro, sizeof kRtPro, reinterpret_cast<void*>(&RtHook), &rt, "UpdateWaterInfoRendering_RenderThread")) {
        Log("[water] the water-info render was not found: WaterCaptureFsrSkip, WaterCaptureNoRhiFlush, WaterCaptureNoOcclusionWait and "
            "WaterCaptureFastMeshScan off");
        return false;
    }
    g_rt = reinterpret_cast<RtFn>(rt);
    Log("[water] UpdateWaterInfoRendering_RenderThread exe+0x%llX detoured (game TimeDateStamp 0x%08X)",
        static_cast<unsigned long long>(g_sites.rt - Game().base), Game().timeDateStamp);
    (void)probe;
    if (g_sites.fsrSlot) {
        g_fsr = reinterpret_cast<FsrFn>(g_sites.fsrFn);
        if (WritePointer(g_sites.fsrSlot, reinterpret_cast<void*>(&FsrHook)))
            Log("[water] WaterCaptureFsrSkip: FFXFSR3FXSystem::PostRenderOpaque slot exe+0x%llX hooked",
                static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g_sites.fsrSlot) - Game().base));
        else { g_fsr = nullptr; Log("[water] WaterCaptureFsrSkip: could not write the slot, unavailable"); }
    } else {
        Log("[water] WaterCaptureFsrSkip: unavailable");
    }
    if (InstallEndFlushDetour(g_sites.endFlush))
        Log("[water] WaterCaptureNoRhiFlush: FRDGBuilder::EndFlushResourcesRHI exe+0x%llX detoured",
            static_cast<unsigned long long>(g_sites.endFlush - Game().base));
    else
        Log("[water] WaterCaptureNoRhiFlush: unavailable");
    void* occ = nullptr;
    if (InstallNearDetour(g_sites.waitOcc, kWaitOccPro, sizeof kWaitOccPro, kWaitOccMoved, reinterpret_cast<void*>(&WaitOccHook), &occ,
                          "WaterCaptureNoOcclusionWait")) {
        g_waitOcc = reinterpret_cast<WaitOccFn>(occ);
        Log("[water] WaterCaptureNoOcclusionWait: FSceneRenderer::WaitOcclusionTests exe+0x%llX detoured",
            static_cast<unsigned long long>(g_sites.waitOcc - Game().base));
    } else {
        Log("[water] WaterCaptureNoOcclusionWait: unavailable");
    }
    g_fakeVt[kSlotProxies / 8] = reinterpret_cast<void*>(&FakeProxiesGetter);
    void* en = nullptr;
    if (InstallNearDetour(g_sites.enable, kEnablePro, sizeof kEnablePro, kEnableMoved, reinterpret_cast<void*>(&EnableHook), &en,
                          "WaterCaptureFastMeshScan")) {
        g_enable = reinterpret_cast<EnableFn>(en);
        Log("[water] WaterCaptureFastMeshScan: FEnableWaterInfoMeshesScoped exe+0x%llX detoured",
            static_cast<unsigned long long>(g_sites.enable - Game().base));
    } else {
        Log("[water] WaterCaptureFastMeshScan: unavailable");
    }
    return true;
}

bool WaterInfoTestPatched()  // self-test: the sites carry our patches (lab: the game-thread one only when the probe asked)
{
    auto movJmp = [](const uint8_t* b) { return b && b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0; };
    const bool rt = movJmp(g_sites.rt) && g_rt;
    const bool gt = true;
    const bool fsr = g_sites.fsrSlot && *g_sites.fsrSlot == reinterpret_cast<void*>(&FsrHook);
    const bool fl = g_sites.endFlush && g_sites.endFlush[0] == 0xE9 && g_endFlush &&
                    memcmp(reinterpret_cast<const void*>(g_endFlush), kEndFlushPro, kEndFlushMoved) == 0;
    const bool occ = g_sites.waitOcc && g_sites.waitOcc[0] == 0xE9 && g_sites.waitOcc[kWaitOccMoved] == 0x80 && g_waitOcc &&
                     memcmp(reinterpret_cast<const void*>(g_waitOcc), kWaitOccPro, kWaitOccMoved) == 0;
    const bool en = g_sites.enable && g_sites.enable[0] == 0xE9 && g_sites.enable[kEnableMoved] == 0x33 && g_enable &&
                    memcmp(reinterpret_cast<const void*>(g_enable), kEnablePro, kEnableMoved) == 0;
    return gt && rt && fsr && fl && occ && en;
}

// Self-test (after the install): hook decisions with fakes. Returns a failure text or nullptr.
const char* WaterInfoTestHooks()
{
    const RtFn rt = g_rt;
    const FsrFn fsr = g_fsr;
    const EndFlushFn fl = g_endFlush;
    const WaitOccFn oc = g_waitOcc;
    const bool skip0 = g_fsrSkip.load(), flush0 = g_noFlush.load(), occ0 = g_noOccWait.load();
    constexpr int kProbeModes = 1;
    const char* fail = nullptr;
    g_rt = &FakeRt;
    g_fsr = &FakeFsr;
    g_endFlush = &FakeFlush;
    g_waitOcc = &FakeOcc;
    for (int probe = 0; probe < kProbeModes && !fail; ++probe) {
        for (int keys = 0; keys < 4 && !fail; ++keys) {  // 0 off, 1 all on, 2 only the occlusion wait, 3 all but it
            const bool fsrFlush = keys == 1 || keys == 3, occ = keys == 1 || keys == 2;
            g_fsrSkip = fsrFlush;
            g_noFlush = fsrFlush;
            g_noOccWait = occ;
            g_fakeFsr = 0;
            g_fakeFlush = 0;
            g_fakeOcc = 0;
            g_fakeInsideOk = true;
            FsrHook(nullptr, nullptr, nullptr, nullptr, 0);  // a main view: always forwarded
            EndFlushHook(nullptr);
            WaitOccHook(nullptr, nullptr);
            if (g_fakeFsr != 1 || g_fakeFlush != 1 || g_fakeOcc != 1) fail = "outside the water-info render the hooks must forward";
            RtHook(nullptr, nullptr);  // inside: skipped only with its key on
            const int want = fsrFlush ? 1 : 2, wantOcc = occ ? 1 : 2;
            if (!fail && (g_fakeFsr != want || g_fakeFlush != want))
                fail = fsrFlush ? "inside the water-info render with the keys on both calls must be skipped"
                                : "with the keys off the water-info render must forward both calls";
            if (!fail && g_fakeOcc != wantOcc)
                fail = occ ? "inside the water-info render with WaterCaptureNoOcclusionWait on the occlusion wait must be skipped"
                           : "with WaterCaptureNoOcclusionWait off the water-info render must forward the occlusion wait";
            if (!fail && keys && !g_fakeInsideOk) fail = "the render-thread hook must mark its scope";
            if (!fail && (t_inWaterInfo || t_inWater2)) fail = "the scope flags must be clear after the water-info render";
            if (!fail) {  // after it, outside again: forwarded
                FsrHook(nullptr, nullptr, nullptr, nullptr, 0);
                EndFlushHook(nullptr);
                WaitOccHook(nullptr, nullptr);
                if (g_fakeFsr != want + 1 || g_fakeFlush != want + 1 || g_fakeOcc != wantOcc + 1)
                    fail = "after the water-info render the hooks must forward again";
            }
        }
    }
    if (!fail) {  // the multi-family guard: once a renderer with +0x106 set is seen, the wait is forwarded inside too
        alignas(16) static uint8_t multi[0x200] = {};
        multi[0x106] = 1;
        g_noOccWait = true;
        g_fakeOcc = 0;
        {
            WaterScope scope(false);
            WaitOccHook(multi, nullptr);
            WaitOccHook(nullptr, nullptr);
        }
        if (g_fakeOcc != 2) fail = "after a renderer with several view families the occlusion wait must no longer be skipped";
        g_occMultiFamily = false;
    }
    g_rt = rt;
    g_fsr = fsr;
    g_endFlush = fl;
    g_waitOcc = oc;
    g_fsrSkip = skip0;
    g_noFlush = flush0;
    g_noOccWait = occ0;
    return fail;
}

// Self-test: EndFlushResourcesRHI's trampoline on a fake builder whose bFlushResourcesRHI (+0x780) is 0: the moved
// sub/cmp, the jump back to the je, and the function's epilogue (nothing else runs).
bool WaterInfoTestTrampoline()
{
    if (!g_endFlush) return false;
    alignas(16) static uint8_t builder[0x800] = {};
    g_endFlush(builder);
    return true;
}

// Self-test: WaitOcclusionTests' trampoline (mov rax,rsp; sub rsp,48h, back to the cmp of GIsRunningRHIInSeparateThread,
// which is false in a process that never started the RHI thread: the function returns at once).
bool WaterInfoTestOccTrampoline()
{
    if (!g_waitOcc || !g_sites.waitOcc) return false;
    const uint8_t* cmp = g_sites.waitOcc + kWaitOccMoved;  // 80 3D rel32 00
    const uint8_t* flag = cmp + 7 + *reinterpret_cast<const int32_t*>(cmp + 2);
    if (*flag != 0) return true;  // the RHI thread runs here: do not call the real function from the self-test
    alignas(16) static uint8_t renderer[0x800] = {};
    g_waitOcc(renderer, nullptr);
    return true;
}

// Self-test: a mismatch writes nothing and turns that part off with a log line.
bool WaterInfoTestMismatch()
{
    alignas(16) uint8_t code[32] = {};
    memcpy(code, kRtPro, sizeof kRtPro);
    memcpy(code + 16, kEndFlushPro, 8);
    code[3] ^= 0xFF;  // the entry no longer has the expected bytes
    code[16 + 3] ^= 0xFF;
    uint8_t before[32];
    memcpy(before, code, sizeof code);
    void* orig = reinterpret_cast<void*>(1);
    const bool hooked = Detour(code, kRtPro, sizeof kRtPro, reinterpret_cast<void*>(&RtHook), &orig, "self-test mismatch");
    const bool nullSite = !Detour(nullptr, kRtPro, sizeof kRtPro, reinterpret_cast<void*>(&RtHook), &orig, "self-test missing site");
    const EndFlushFn fl = g_endFlush;
    const bool noFlush = !InstallEndFlushDetour(code + 16);
    const bool flushKept = g_endFlush == fl;
    alignas(16) uint8_t occCode[16] = {};
    memcpy(occCode, kWaitOccPro, sizeof kWaitOccPro);
    occCode[4] ^= 0xFF;
    void* occOrig = reinterpret_cast<void*>(1);
    const bool noOcc = !InstallNearDetour(occCode, kWaitOccPro, sizeof kWaitOccPro, kWaitOccMoved, reinterpret_cast<void*>(&WaitOccHook),
                                          &occOrig, "self-test occlusion mismatch") &&
                       occOrig == reinterpret_cast<void*>(1) && occCode[0] == kWaitOccPro[0] && occCode[4] == (kWaitOccPro[4] ^ 0xFF);
    const bool none = FindByCaller("CC CC 11 22 33 44 55 66 77 88 99 E8 ?? ?? ?? ??", 11, kRtPro, sizeof kRtPro, "self-test pattern") == nullptr;
    return !hooked && nullSite && noFlush && flushKept && noOcc && none && memcmp(code, before, sizeof code) == 0 && orig == reinterpret_cast<void*>(1);
}

void SetWaterCaptureNoRhiFlush(bool on)
{
    if (!g_endFlush) return;
    if (g_noFlush.exchange(on) != on) Log("[water] WaterCaptureNoRhiFlush=%d", on ? 1 : 0);
}

void SetWaterCaptureNoOcclusionWait(bool on)
{
    if (!g_waitOcc) return;
    if (g_noOccWait.exchange(on) != on) Log("[water] WaterCaptureNoOcclusionWait=%d", on ? 1 : 0);
}

// Self-test of WaterCaptureFastMeshScan (after the install). (1) CollectCandidates equals the stock per-proxy test on fake
// sets (one bucket, many buckets, heap and inline hash, empty set, chains, ids sharing their low 12 bits, null proxies).
// (2) The game's FScene vtable is the one .rdata pair of adjacent array getters at +0x3F8 / +0x400. (3) The hook on a fake
// scene that uses those real getters: with no proxy in the set every mode gives an empty TArray, the fast modes take the
// packed-id path, mode 2 finds no difference; a scene whose slots are not getters, or whose first or last packed id differs
// from its proxy's own id, runs the stock code (a fallback); three matching proxies through the hook in all modes (with a
// C++ copy of the stock constructor's steps, since the real one's TArray growth needs the game's allocator) give the same
// enabled proxies and the same TArray.
// Returns a failure text or nullptr.
const char* WaterInfoTestScan()
{
    if (!g_enable) return "fast mesh scan: not installed";
    // (1)
    uint32_t rng = 12345;
    auto rnd = [&rng] { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
    for (int round = 0; round < 40; ++round) {
        const uint32_t hashSize = round % 4 == 0 ? 1u : (round % 4 == 1 ? 8u : (round % 4 == 2 ? 64u : 4u));
        const int k = round % 10 == 9 ? 0 : 1 + int(rnd() % 40);
        std::vector<uint8_t> view(0x1700, 0);
        std::vector<uint8_t> elems(size_t(k + 1) * 12, 0);
        std::vector<int32_t> heapHash(hashSize, -1);
        int32_t inlineHash[1] = {-1};
        std::vector<uint32_t> setIds;
        int32_t* hash = round % 3 == 0 && hashSize == 1 ? inlineHash : heapHash.data();
        for (int i = 0; i < k; ++i) {
            const uint32_t id = (round & 1) ? (rnd() % 64) * 4096 + 7 : 100000 + rnd() % 100000;  // odd rounds: same low 12 bits
            bool dup = false;
            for (uint32_t x : setIds) dup = dup || x == id;
            if (dup) continue;
            const int idx = int(setIds.size());
            setIds.push_back(id);
            memcpy(&elems[idx * 12], &id, 4);
            const uint32_t b = id & (hashSize - 1);
            memcpy(&elems[idx * 12 + 4], &hash[b], 4);  // push-front into the chain
            hash[b] = idx;
        }
        const int32_t num = int32_t(setIds.size()), freeN = 0;
        const uint8_t* ep = elems.data();
        memcpy(&view[kSetElems], &ep, 8);
        memcpy(&view[kSetNum], &num, 4);
        memcpy(&view[kSetFree], k == 0 ? &num : &freeN, 4);
        if (hash != inlineHash) { const int32_t* hp = hash; memcpy(&view[kSetHashHeap], &hp, 8); }
        else memcpy(&view[kSetHashInline], inlineHash, 4);
        const int32_t hs = int32_t(hashSize);
        memcpy(&view[kSetHashSize], &hs, 4);
        view[kShowOnlySet] = 1;
        const int n = 3000;
        std::vector<uint8_t> proxyMem(size_t(n) * 0x300, 0);
        std::vector<const void*> proxies(n);
        std::vector<uint32_t> ids(n);
        for (int i = 0; i < n; ++i) {
            uint32_t id = !setIds.empty() && rnd() % 50 == 0 ? setIds[rnd() % setIds.size()] : ((round & 1) ? (rnd() % 64) * 4096 + 7 + (rnd() % 3) : rnd() % 300000);
            ids[i] = id;
            memcpy(&proxyMem[size_t(i) * 0x300 + kProxyId], &id, 4);
            proxies[i] = rnd() % 97 == 0 ? nullptr : &proxyMem[size_t(i) * 0x300];
        }
        const SetView set = ReadSet(view.data());
        std::vector<const void*> fast, ref;
        CollectCandidates(set, ids.data(), proxies.data(), n, fast);
        fast.erase(std::remove(fast.begin(), fast.end(), nullptr), fast.end());  // the stock constructor skips null proxies
        for (int i = 0; i < n; ++i) {
            const auto* pr = static_cast<const uint8_t*>(proxies[i]);
            if (!pr || set.empty) continue;
            // the stock test, independently: walk the chain of the proxy's own id
            uint32_t id;
            memcpy(&id, pr + kProxyId, 4);
            bool in = false;
            for (uint32_t x : setIds) in = in || x == id;
            if (in) ref.push_back(pr);
        }
        if (fast != ref) return "fast mesh scan: the packed-id candidates differ from the per-proxy test on a fake set";
    }
    // (2)
    const uint8_t* vtSlot = nullptr;
    int pairs = 0;
    for (uint8_t* q = Game().rdata.begin; q + 16 <= Game().rdata.end; q += 8) {
        const auto* a = *reinterpret_cast<const uint8_t* const*>(q);
        if (!Game().text.Contains(const_cast<uint8_t*>(a)) || !ArrayGetterOffset(a)) continue;
        if (ArrayGetterOffset(*reinterpret_cast<const uint8_t* const*>(q + 8))) { vtSlot = q; ++pairs; }
    }
    if (pairs != 1) return "fast mesh scan: want exactly one .rdata pair of adjacent array getters (FScene +0x3F8 / +0x400)";
    const auto* vt = reinterpret_cast<const void* const*>(vtSlot - kSlotProxies);
    const uint32_t op = ArrayGetterOffset(static_cast<const uint8_t*>(vt[kSlotProxies / 8]));
    const uint32_t oi = ArrayGetterOffset(static_cast<const uint8_t*>(vt[kSlotIds / 8]));
    // (3)
    std::vector<uint8_t> scene(std::max(op, oi) + 0x20, 0);
    const void* vtp = vt;
    memcpy(scene.data(), &vtp, 8);
    std::vector<uint8_t> proxyMem(64 * 0x300, 0);
    std::vector<const void*> proxies(64);
    std::vector<uint32_t> ids(64);
    for (int i = 0; i < 64; ++i) {
        ids[i] = 5000 + i;
        memcpy(&proxyMem[size_t(i) * 0x300 + kProxyId], &ids[i], 4);
        proxyMem[size_t(i) * 0x300 + 0x6E] = 0x10;
        proxies[i] = &proxyMem[size_t(i) * 0x300];
    }
    const void* pd = proxies.data();
    const void* idd = ids.data();
    const int32_t n64 = 64;
    memcpy(&scene[op], &pd, 8);
    memcpy(&scene[op + 8], &n64, 4);
    memcpy(&scene[oi], &idd, 8);
    memcpy(&scene[oi + 8], &n64, 4);
    std::vector<uint8_t> view(0x1700, 0);
    uint8_t elems[24] = {};
    const uint32_t notThere[2] = {77, 4096 + 5000};  // the second shares its low 12 bits with a scene id
    int32_t hash[2] = {-1, -1};
    for (int i = 0; i < 2; ++i) {
        memcpy(elems + i * 12, &notThere[i], 4);
        const uint32_t b = notThere[i] & 1;
        memcpy(elems + i * 12 + 4, &hash[b], 4);
        hash[b] = i;
    }
    const uint8_t* ep = elems;
    const int32_t two = 2, zero = 0, hs = 2;
    const int32_t* hp = hash;
    memcpy(&view[kSetElems], &ep, 8);
    memcpy(&view[kSetNum], &two, 4);
    memcpy(&view[kSetFree], &zero, 4);
    memcpy(&view[kSetHashHeap], &hp, 8);
    memcpy(&view[kSetHashSize], &hs, 4);
    view[kShowOnlySet] = 1;
    const int mode0 = g_fastScan.load();
    const uint64_t fast0 = g_scanFast.load(), fb0 = g_scanFallback.load();
    constexpr int kModes = 2;  // 0 stock, 1 fast
    const char* fail = nullptr;
    for (int mode = 0; mode < kModes && !fail; ++mode) {
        g_fastScan = mode;
        uint64_t arr[2] = {0xDEAD, 0xDEADDEAD};
        void* r = EnableHook(arr, scene.data(), view.data());
        if (r != arr || arr[0] != 0 || arr[1] != 0) fail = "fast mesh scan: a set with no scene proxy must give an empty TArray in every mode";
        for (int i = 0; i < 64 && !fail; ++i)
            if (proxyMem[size_t(i) * 0x300 + 0x6E] != 0x10) fail = "fast mesh scan: no proxy outside the set may be touched";
    }
    if (!fail && g_scanFast.load() - fast0 != static_cast<uint64_t>(kModes - 1))
        fail = "fast mesh scan: the fast modes must take the packed-id path on the game's scene getters";
    if (!fail) {  // a scene whose slot +0x400 is not a getter: the stock code runs (it calls the proxies getter itself)
        std::vector<const void*> fakeVt(kSlotIds / 8 + 1, nullptr);
        fakeVt[kSlotProxies / 8] = vt[kSlotProxies / 8];
        const void* fv = fakeVt.data();
        std::vector<uint8_t> scene2(scene);
        memcpy(scene2.data(), &fv, 8);
        g_sceneVt = nullptr;
        g_fastScan = 1;
        uint64_t arr[2] = {1, 1};
        EnableHook(arr, scene2.data(), view.data());
        if (arr[0] != 0 || arr[1] != 0 || g_scanFallback.load() - fb0 != 1) fail = "fast mesh scan: a scene without the two getters must run the stock code";
    }
    if (!fail) {  // packed ids that do not match their proxies (first or last): the stock code runs
        for (const int k : {0, 63}) {
            ids[k] += 100;
            g_sceneVt = nullptr;
            g_fastScan = 1;
            const uint64_t fb1 = g_scanFallback.load();
            uint64_t arr[2] = {1, 1};
            EnableHook(arr, scene.data(), view.data());
            ids[k] -= 100;
            if (arr[0] != 0 || arr[1] != 0 || g_scanFallback.load() - fb1 != 1) fail = "fast mesh scan: packed ids that differ from their proxies' ids must run the stock code";
        }
    }
    if (!fail) {  // matching proxies through the hook (with TestStockCtor as the constructor): the same proxies, in order
        const EnableFn real = g_enable;
        g_enable = &TestStockCtor;
        const uint32_t in[3] = {5003, 5040, 5063};  // three scene ids in the set, plus the two absent ones
        uint8_t elems5[60] = {};
        int32_t hash4[4] = {-1, -1, -1, -1};
        const uint32_t all[5] = {in[0], in[1], in[2], notThere[0], notThere[1]};
        for (int i = 0; i < 5; ++i) {
            memcpy(elems5 + i * 12, &all[i], 4);
            const uint32_t b = all[i] & 3;
            memcpy(elems5 + i * 12 + 4, &hash4[b], 4);
            hash4[b] = i;
        }
        std::vector<uint8_t> view2(view);
        const uint8_t* ep5 = elems5;
        const int32_t five = 5, four = 4;
        const int32_t* hp4 = hash4;
        memcpy(&view2[kSetElems], &ep5, 8);
        memcpy(&view2[kSetNum], &five, 4);
        memcpy(&view2[kSetHashHeap], &hp4, 8);
        memcpy(&view2[kSetHashSize], &four, 4);
        std::vector<const void*> want, got[3];
        for (const uint32_t id : in) want.push_back(proxies[id - 5000]);
        for (int mode = 0; mode < kModes && !fail; ++mode) {
            for (int i = 0; i < 64; ++i) proxyMem[size_t(i) * 0x300 + 0x6E] = 0x10;
            g_sceneVt = nullptr;
            g_fastScan = mode;
            uint64_t arr[2] = {1, 1};
            EnableHook(arr, scene.data(), view2.data());
            const auto* out = reinterpret_cast<const void* const*>(arr[0]);
            const int32_t num = static_cast<int32_t>(arr[1] & 0xFFFFFFFF);
            for (int32_t i = 0; i < num; ++i) got[mode].push_back(out[i]);
            for (int i = 0; i < 64 && !fail; ++i) {
                const bool on = i == 3 || i == 40 || i == 63;
                if (proxyMem[size_t(i) * 0x300 + 0x6E] != (on ? 0 : 0x10)) fail = "fast mesh scan: exactly the proxies in the set must be enabled";
            }
            if (!fail && got[mode] != want) fail = "fast mesh scan: the TArray must hold the matching proxies in scene order";
        }
        g_enable = real;
    }
    g_sceneVt = nullptr;
    g_fastScan = mode0;
    return fail;
}

void SetWaterCaptureFastMeshScan(int mode)
{
    if (!g_enable) return;
    if (g_fastScan.exchange(mode) != mode) Log("[water] WaterCaptureFastMeshScan=%d", mode);
}

void SetWaterCaptureFsrSkip(bool on)
{
    if (!g_fsr) return;
    if (g_fsrSkip.exchange(on) != on) Log("[water] WaterCaptureFsrSkip=%d", on ? 1 : 0);
}


// One line per stats interval in which water-info updates happened.
void LogWaterCaptureStats()
{
    if (!g_rt) return;
    static uint64_t logged = 0;
    const uint64_t updates = g_updates.load();
    if (updates == logged) return;
    logged = updates;
    Log("[water] water-info updates %llu; in them: FSR3 pre-alpha copies skipped %llu, RHI-thread flushes skipped %llu, occlusion-fence "
        "waits skipped %llu; mesh scans fast %llu (%llu primitives, %llu enabled), stock %llu (fallbacks %llu)",
        static_cast<unsigned long long>(updates), static_cast<unsigned long long>(g_fsrSkipped.load()),
        static_cast<unsigned long long>(g_flushSkipped.load()), static_cast<unsigned long long>(g_occSkipped.load()),
        static_cast<unsigned long long>(g_scanFast.load()), static_cast<unsigned long long>(g_scanPrims.load()),
        static_cast<unsigned long long>(g_scanHits.load()), static_cast<unsigned long long>(g_scanStock.load()),
        static_cast<unsigned long long>(g_scanFallback.load()));
}

}  // namespace opf
