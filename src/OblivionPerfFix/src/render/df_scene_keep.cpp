// Distance-field scene capacity (worker rasavafix, 2026-11-27, S-087; diagnosis: worker rasavastep, findings `rasavastep`;
// DfAddSpread first built by worker dfaddspread 2026-11-19 (S-004 / S-045), reviewed `revdfaddspread`, moved here so that the
// three keys share the one hook at the distance-field update's call; review `rasavafix-review` F1-F9 applied). Three keys,
// each its own switch, default 0, live.
//
// The problem (our measurements, findings tag rasavastep): when a cell's instanced foliage attaches, one render-thread call of
// UE 5.3's FDistanceFieldSceneData::UpdateDistanceFieldObjectBuffers adds ~16 900 distance-field objects (117.6k -> 134.6k at
// Ra'sava Camp) and in that one frame crosses two capacities: the CPU array PrimitiveInstanceMapping (TArray of 208-byte
// entries) re-allocates and copies all of its 117k entries (4.5-7.5 ms in a fresh scene), and the two GPU object buffers go to
// the next power of two (2^17 -> 2^18, a new buffer plus a GPU copy, 2.1-3.0 ms); the per-object work of the 16.9k adds is
// another 2-4 ms on the render thread and 3-6 ms on the RHI thread. UE 5.3's ResizeBufferIfNeeded re-creates a buffer on any
// size change, so the GPU buffers also shrink when the count falls below a power of two and grow again when it comes back.
// UE 5.4 to 5.7.4 and ue5-main keep the same sizing, the same resize-on-any-change and no reserve (checked 2026-11-27).
//
// 1. DfObjectBufferKeep (identical): the two ResizeStructuredBufferIfNeeded calls of the object data / bounds buffers ask for
//    at least the largest power of two this session has needed (per buffer owner; a scene that has never held 16384 objects
//    keeps only its own largest size; an owner whose buffer is not made yet starts afresh), at most 2^19 objects (80 + 24 MB);
//    a world scene's buffers start at 2^18 objects at least (user 2026-11-27 "Yes, pre-size": a fresh session skips the one
//    growth 2^17 -> 2^18 at its first big step, +26 MB until the scene needs that size).
//    Every reader of these buffers is bounded by the object count (NumSceneObjects = NumObjectsInBuffer, CPU and shaders;
//    GlobalDistanceField / Lumen size their own lists from the count, never from the buffer), the entries below the count are
//    written by the same scatter uploads at the same indices, and a larger buffer is a size stock itself reaches; so what is
//    drawn is the same. Cost: video memory up to the session's peak (expected +27 MB), logged at each new peak and kept episode.
// 2. DfMappingReserve (identical, capacity only): behind a loading screen (the game thread's loading state seen on the render
//    thread for at least 750 ms and two calls, so a fade before the screen and the render thread's one-frame lag stay out),
//    the PrimitiveInstanceMapping array gets its capacity raised with the game's own ResizeAllocation (the call its TArray
//    growth makes) to max(session peak + 1/8, the count after the call + 1/4), rounded up to 1024, at most 2^19 entries
//    (109 MB). Before a call that builds a world (>= 256 pending primitives) it is raised to the session's target first, so the
//    load's own 25 growth steps are skipped too. Only the capacity changes; Num, the entries and their order are the game's.
// 3. DfAddSpread (equivalent, not identical): when one update would add more than DfAddSpreadBudget instances (default 4096;
//    a normal cell attach adds ~1 800, the foliage attach 16 900, a far HLOD show 16 000), far primitives past the budget wait
//    in the game's own PendingAddOperations set for the next update (at most 3 updates late, then always through). An update
//    that adds no more than the budget holds nothing. Oldest first: a held-back primitive is never held again behind a newer
//    one that went in in the same update. A primitive whose bounds come within DfAddSpreadNearM metres of the camera (the
//    render thread's own view of this frame), or every primitive when that view cannot be read, is never held; more than 1024
//    pending primitives (a level load): stock. A held primitive's distance-field objects enter the distance-field scene 1-3
//    render frames later; its mesh is drawn as before. When a remove and a held add are the same visible thing (a far view
//    shown for a cell's meshes, a render-state re-creation), that area has neither set for those 1-3 updates (a gap, not only
//    a late arrival). Removes are never reordered against adds of the same primitive: the game's RemovePrimitive takes a held
//    one out of the set (it has no distance-field objects yet), UpdatePrimitive skips it (a pending add), its add uses the
//    newest transform. The set is not empty after the update (5.3 asserts that in checks that are compiled out of this exe).
//    A held primitive whose distance-field asset lost its last other user in that update re-loads it from its lowest mip
//    (counted; Phase B measures it). Equivalence argument: findings tag rasavafix.
// Sites (Steam build 19115871): the one UpdateDistanceFieldObjectBuffers call exe+0x1D96471 (target exe+0x1DACA70; the scene
// renderer in r14 there, its Scene +0x58, Views +0x548), its two ResizeStructuredBufferIfNeeded calls exe+0x1DAD094 /
// exe+0x1DAD0B2 (exe+0x270CB10), the add loop's ProcessPrimitiveUpdate call exe+0x1DACDA5 (exe+0x1D973E0), the mapping's
// growth in ProcessPrimitiveUpdate exe+0x1D97C78 (ResizeGrow exe+0x11D1E70, its ResizeAllocation exe+0xDE6200), AddPrimitive's
// insert exe+0x2262BA6 (TSet::Emplace exe+0xEAFD50), FSceneView::WorldToMetersScale read at exe+0x40987C9's block (the view
// layout, as FrameGenCameraInfo). Each key's part is found on its own; a part not found once is not installed, with a log
// line. Log tag [dfscene].
#include "common.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace opf {

namespace {

using DfobFn = void (*)(void* dfs, void* graph, void* queue, void* scene, void* assetAdds, void* assetRemoves);
using PpuFn = void (*)(bool add, void* scene, void* prim, void* xforms, void* indices, void* assetAdds, void* assetRemoves);
using EmplaceFn = void* (*)(void* set, int32_t* outId, void** key, bool* alreadyInSet);
using ResizeFn = void* (*)(void* graph, void* externalBuffer, uint32_t numBytes, const wchar_t* name);
using ResizeAllocFn = void (*)(void* array, int32_t prevNum, int32_t newMax, size_t bytesPerElement, uint32_t alignment);
using LoadingFn = int (*)();
using CameraFn = bool (*)(void* scene, double* x, double* y, double* z);

// PrepareDistanceFieldScene (exe+0x1D96430): mov r9,[r14+58h] (Scene); lea rcx,[rsi+3368h] (Scene->DistanceFieldSceneData) ...
// mov [rsp+58h],0; call UpdateDistanceFieldObjectBuffers; mov rdx,[r14+548h] (Views.GetData())
constexpr char kDfobSite[] =
    "4D 8B 4E ?? 48 8D 8E ?? ?? 00 00 48 89 44 24 60 4D 8B C4 48 89 44 24 50 49 8B D5 48 8D 44 24 50 48 C7 44 24 68 00 00 00 "
    "00 48 89 44 24 28 48 8D 44 24 60 48 89 44 24 20 48 C7 44 24 58 00 00 00 00 E8 ?? ?? ?? ?? 49 8B 96 ?? ?? 00 00";
constexpr size_t kDfobSceneDisp = 3, kDfobDfsDisp = 7, kDfobE8 = 65, kDfobViewsDisp = 73;
// UpdateDistanceFieldObjectBuffers' upload (exe+0x1DAD058): NumDFObjectsRoundedUp = RoundUpToPowerOfTwo(max(NumObjectsInBuffer,1))
// in ebx; r8d = ebx*160 -> ResizeStructuredBufferIfNeeded(GraphBuilder, ObjectBuffers->Data, r8d, L"DistanceFields.DFObjectData");
// r8d = ebx*48 -> the same for ObjectBuffers->Bounds (the strides are part of the pattern)
constexpr char kResizeSites[] =
    "8B 07 4C 8D 0D ?? ?? ?? ?? 48 8B 57 08 85 C0 B9 01 00 00 00 BB 01 00 00 00 0F 45 C8 48 83 C2 08 8D 41 FF 48 8D 0C 45 01 00 "
    "00 00 48 0F BD C9 D3 E3 49 8B CF 44 8D 04 9B 41 C1 E0 05 E8 ?? ?? ?? ?? 48 8B 57 08 44 8D 04 5B 41 C1 E0 04 4C 8D 0D ?? ?? ?? "
    "?? 49 8B CF 4C 8B E0 E8 ?? ?? ?? ??";
constexpr size_t kResDataName = 2, kResDataE8 = 60, kResBoundsName = 77, kResBoundsE8 = 90;
constexpr uint32_t kDataStride = 160, kBoundsStride = 48;  // GDistanceFieldObjectDataStride / BoundsStride x 16 bytes (pattern-fixed)
// the add loop's ProcessPrimitiveUpdate call (exe+0x1DACD7B, as DfAddSpread on its branch)
constexpr char kAddSite[] =
    "4D 8B 00 48 8D 87 ?? ?? ?? ?? 48 03 C9 4C 89 64 24 ?? 4C 89 6C 24 ?? 4C 8D 4C 24 ?? 49 8B D7 48 89 44 24 20 "
    "4D 8B 04 C8 B1 01 E8 ?? ?? ?? ?? 8B 44 24 6C";
constexpr size_t kAddIdxDisp = 6, kAddE8 = 42;
constexpr char kUpdSite[] = "48 8B D6 48 89 44 24 20 4D 8B 04 C8 33 C9 E8 ?? ?? ?? ?? 8B 44 24 6C";
constexpr size_t kUpdE8 = 14;
constexpr char kSetLea[] = "C7 45 EC 01 00 00 00 48 8D B7 ?? ?? 00 00 48 89 44 24 50";
constexpr size_t kSetLeaDisp = 10;
constexpr char kEmplace[] = "48 8D 8F ?? ?? 00 00 45 33 C9 4C 8D 44 24 28 48 8D 54 24 20 E8 ?? ?? ?? ?? 48 8B 4D 40";
constexpr size_t kEmplaceDisp = 3, kEmplaceE8 = 20;
// ProcessPrimitiveUpdate (exe+0x1D97410): mov rbx,[r8+8] (PrimitiveSceneInfo->Proxy); mov r13,r9; mov eax,[r9+0Ch]
constexpr char kProxyLoad[] = "49 8B 58 ?? 4D 8B E9 41 8B 41 0C";
constexpr size_t kProxyDisp = 3;
// ... movsd xmm0,[rbx+250h] (Proxy->GetBounds().SphereRadius); cvtpd2ps; comiss (the huge-bounds test)
constexpr char kRadiusLoad[] = "F2 0F 10 83 ?? ?? 00 00 66 0F 5A C0 0F 2F 05";
constexpr size_t kRadiusDisp = 4;
// FFX frame-generation prepare (exe+0x40987C9; the FViewInfo in r10): the pNext store (wildcarded: FrameGenCameraInfo patches
// it) ... header.type = 20004h; then divss xmm0,[r10+disp32] = 1 / View->WorldToMetersScale. UE 5.3 FSceneView: ViewMatrices
// (7E0h bytes), ViewLocation, ..., WorldToMetersScale 68h after ViewLocation; ViewMatrices.ViewOrigin after 15 matrices + 18h.
constexpr char kCamSite[] =
    "33 C0 0F 11 85 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 0F 11 85 ?? ?? ?? ?? 89 85 ?? ?? ?? ?? 4C 89 65 ?? 44 89 65 ?? "
    "48 C7 85 ?? ?? ?? ?? 04 00 02 00";
constexpr size_t kCamTypeStore = 37, kCamScaleSearch = 0x100;
constexpr char kScaleRead[] = "F3 41 0F 5E 82";
constexpr uint32_t kLocationBeforeScale = 0x68, kMatricesBeforeLocation = 0x7E0, kOriginInMatrices = 15 * 0x80 + 0x18;
// ... the mapping's Add (exe+0x1D97C60): mov [rsi+37C0h],eax (Num); cmp eax,[rsi+37C4h] (Max); jbe; mov edx,r15d;
// lea rcx,[rsi+37B8h] (Scene->DistanceFieldSceneData.PrimitiveInstanceMapping); call ResizeGrow; mov ebx,[rsp+24h]
constexpr char kGrowSite[] = "89 86 ?? ?? 00 00 3B 86 ?? ?? 00 00 76 ?? 41 8B D7 48 8D 8E ?? ?? 00 00 E8 ?? ?? ?? ?? 8B 5C 24 24";
constexpr size_t kGrowNumDisp = 2, kGrowMaxDisp = 8, kGrowLeaDisp = 20, kGrowE8 = 24;
// TArray<FPrimitiveAndInstance>::ResizeGrow (exe+0x11D1E70), checked at its start: the slack (Num + 3/8 Num + 16), QuantizeSize
// (size, alignment), then ResizeAllocation(this, OldNum, NewMax, sizeof, alignment) at +0x8B
constexpr char kResizeGrow[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 63 59 08 8B F2 48 8B F9 3B DA 0F 8C ?? ?? ?? ?? 83 79 0C 00 B8 04 00 00 00 "
    "74 ?? 48 8D 04 5B 48 C1 E8 03 48 83 C0 10 48 03 C3 EB ?? 48 3B D8 48 0F 47 C3 48 69 C8 ?? ?? ?? ?? BA ?? ?? ?? ?? E8 ?? ?? "
    "?? ?? 48 8B C8 C7 44 24 20 ?? ?? ?? ?? 48 B8 ?? ?? ?? ?? ?? ?? ?? ?? 41 B9 ?? ?? ?? ?? 48 F7 E1 B8 FF FF FF 7F 48 8B CF 48 "
    "C1 EA ?? 3B DA 0F 4F D0 89 57 0C 44 8B C2 8B D6 E8";
constexpr size_t kRgSizeImm = 0x46, kRgAlignImm1 = 0x4B, kRgAlignImm2 = 0x5B, kRgSizeImm2 = 0x6B, kRgAllocE8 = 0x8B;
// TSet = TSparseArray: Data.Num at +8, NumFreeIndices at +0x34 (Num() = Data.Num - NumFreeIndices; UE 5.3 layout, not read from code)
constexpr uint32_t kSetDataNum = 0x08, kSetNumFree = 0x34;
constexpr int kMaxPending = 1024;   // DfAddSpread: more pending primitives (a level load): stock
constexpr int kMaxDefers = 3;       // DfAddSpread: a primitive held back this often always goes through
constexpr int kWorldBuild = 256;    // DfMappingReserve: pending primitives of a call that builds a world (load: ~3800)
constexpr int32_t kWorldMin = 16384;          // a scene of this many objects counts as a world (both keeps)
constexpr uint32_t kObjCap = 1u << 19;         // DfObjectBufferKeep: never hold more than 2^19 objects (80 + 24 MB)
constexpr uint32_t kObjFirst = 1u << 18;       // DfObjectBufferKeep: a world scene's buffers start at least this size (52 MB;
                                               // user 2026-11-27 "Yes, pre-size": skips the one growth 2^17 -> 2^18)
constexpr int32_t kMapCap = 1 << 19;           // DfMappingReserve: never reserve more than 2^19 entries (109 MB at 208 B)
constexpr double kLoadSettleMs = 750.0;        // DfMappingReserve: loading state this long before a reserve

struct Found {
    uint8_t* dfobSite = nullptr;  // E8 of the UpdateDistanceFieldObjectBuffers call
    uint8_t* dfob = nullptr;
    uint32_t dfsInScene = 0, sceneOff = 0, viewsOff = 0;
    bool okRes = false, okSpread = false, okMap = false, okCam = false;
    const char* whyRes = "", * whySpread = "", * whyMap = "", * whyCam = "";
    uint8_t* resSite[2] = {};     // E8 of the data / bounds ResizeStructuredBufferIfNeeded calls
    uint8_t* resize = nullptr;
    uint8_t* addSite = nullptr;   // E8 of the add loop's ProcessPrimitiveUpdate call
    uint8_t* ppu = nullptr;
    uint8_t* updSite = nullptr;
    uint8_t* emplace = nullptr;
    uint32_t setOff = 0, idxOff = 0;
    uint32_t proxyOff = 0, radiusOff = 0;
    uint32_t locOff = 0, originOff = 0;  // FSceneView::ViewLocation, ViewMatrices.ViewOrigin
    uint32_t mapOff = 0;          // FDistanceFieldSceneData::PrimitiveInstanceMapping (TArray: data, Num +8, Max +0xC)
    uint8_t* resizeAlloc = nullptr;
    uint32_t elemSize = 0, elemAlign = 0;
};

// the game's functions and our offsets
DfobFn g_dfob = nullptr;
PpuFn g_ppu = nullptr;
EmplaceFn g_emplace = nullptr;
ResizeFn g_resize = nullptr;
ResizeAllocFn g_resizeAlloc = nullptr;
LoadingFn g_loading = &LoadingScreenNow;
uint32_t g_setOff = 0, g_idxOff = 0, g_proxyOff = 0, g_radiusOff = 0, g_mapOff = 0, g_elemSize = 0, g_elemAlign = 0;
uint32_t g_sceneOff = 0, g_viewsOff = 0, g_locOff = 0, g_originOff = 0;
uint8_t* const* g_rendererSlot = nullptr;  // written by the call's stub: the scene renderer (r14) of the running call
bool g_dfobHooked = false, g_addHooked = false, g_resHooked = false, g_found = false;
std::atomic<int> g_keyBuf{0}, g_keyMap{0}, g_keySpread{0};
std::atomic<int> g_budget{4096};
std::atomic<int> g_nearM{60};
double g_qpcMs = 0;
double g_settleMs = kLoadSettleMs;

double NowMs()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * g_qpcMs;
}
int32_t Rd32(const uint8_t* p, uint32_t off) { int32_t v; memcpy(&v, p + off, 4); return v; }
void Wr32(uint8_t* p, uint32_t off, int32_t v) { memcpy(p + off, &v, 4); }
int32_t ArrayNum(const void* tarray) { return tarray ? Rd32(static_cast<const uint8_t*>(tarray), 8) : 0; }  // TArray::Num
int PendingAdds(const uint8_t* dfs) { return Rd32(dfs, g_setOff + kSetDataNum) - Rd32(dfs, g_setOff + kSetNumFree); }
void MaxInto(std::atomic<uint32_t>& a, uint32_t v)
{
    uint32_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

// The camera of the frame being rendered: the scene renderer's first view (FViewInfo), its ViewLocation, checked against
// ViewMatrices.ViewOrigin and against the renderer's Scene being the update's scene. False = not known (nothing is held).
bool RenderCameraPos(void* scene, double* x, double* y, double* z)
{
    if (!g_rendererSlot || !g_locOff || !g_originOff) return false;
    const uint8_t* const r = *g_rendererSlot;
    if (!r || !scene) return false;
    void* rs;
    memcpy(&rs, r + g_sceneOff, sizeof rs);
    if (rs != scene) return false;
    const uint8_t* views;
    memcpy(&views, r + g_viewsOff, sizeof views);
    const int32_t n = Rd32(r, g_viewsOff + 8);
    if (!views || n < 1 || n > 16) return false;
    double loc[3], org[3];
    memcpy(loc, views + g_locOff, sizeof loc);
    memcpy(org, views + g_originOff, sizeof org);
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(loc[i]) || std::fabs(loc[i]) > 1.0e8 || std::fabs(loc[i] - org[i]) > 1.0) return false;
    *x = loc[0];
    *y = loc[1];
    *z = loc[2];
    return true;
}
CameraFn g_camPos = &RenderCameraPos;

// ---- 1. DfObjectBufferKeep (render thread, inside UpdateDistanceFieldObjectBuffers) ----
struct Owner { const void* ext; uint32_t peak; bool holding; };
Owner g_owner[16] = {};
int g_ownerNext = 0;
uint32_t g_objPeak = 0;  // the session's largest object count (power of two) asked for while the key is on
std::mutex g_bufMx;
struct BufStats { std::atomic<uint64_t> calls{0}, held{0}, episodes{0}; std::atomic<uint32_t> heldMB{0}; } sb;

Owner* OwnerOf(const void* ext)
{
    // an owner whose buffer is not made yet (a new FDistanceFieldObjectBuffers, maybe at a dead scene's address) starts afresh
    void* buf = nullptr;
    memcpy(&buf, ext, sizeof buf);
    for (Owner& o : g_owner)
        if (o.ext == ext) {
            if (!buf) o = Owner{ext, 0, false};
            return &o;
        }
    Owner* o = &g_owner[g_ownerNext];
    g_ownerNext = (g_ownerNext + 1) % 16;
    *o = Owner{ext, 0, false};
    return o;
}

// The size one of the two calls gets: the game's, or the session's largest when that is larger (identical: readers stop at
// the object count). site 0 = DFObjectData (160 B an object), 1 = DFObjectBounds (48 B).
uint32_t ObjectBufferBytes(int site, const void* ext, uint32_t bytes)
{
    const uint32_t stride = site ? kBoundsStride : kDataStride;
    if (!g_keyBuf.load(std::memory_order_relaxed) || !ext || bytes % stride) return bytes;
    const uint32_t n = bytes / stride;  // the game's power of two
    std::lock_guard<std::mutex> lk(g_bufMx);
    if (site == 0) sb.calls.fetch_add(1, std::memory_order_relaxed);
    if (n > g_objPeak && n <= kObjCap) {
        if (site == 0)
            Log("[dfscene] DfObjectBufferKeep: new session peak %u distance-field objects (%.1f MB data + bounds; was %u)", n,
                n * double(kDataStride + kBoundsStride) / 1048576.0, g_objPeak);
        g_objPeak = n;
    }
    Owner* const o = OwnerOf(ext);
    if (n > o->peak) o->peak = n;
    const bool world = static_cast<int32_t>(o->peak) >= kWorldMin;  // small / preview scenes keep only their own largest size
    uint32_t hold = world ? (g_objPeak > kObjFirst ? g_objPeak : kObjFirst) : o->peak;
    if (hold > kObjCap) hold = kObjCap;
    if (hold <= n) { o->holding = false; return bytes; }
    if (site == 0) {
        sb.held.fetch_add(1, std::memory_order_relaxed);
        const uint32_t mb = static_cast<uint32_t>((uint64_t(hold - n) * (kDataStride + kBoundsStride)) >> 20);
        MaxInto(sb.heldMB, mb);
        if (!o->holding) {
            sb.episodes.fetch_add(1, std::memory_order_relaxed);
            Log("[dfscene] DfObjectBufferKeep: distance-field object buffers kept at %u objects where the game asks %u%s: "
                "+%.1f MB of video memory held, no re-creation",
                hold, n, hold == kObjFirst && g_objPeak < kObjFirst ? " (the first size of a world scene)" : "",
                (hold - n) * double(kDataStride + kBoundsStride) / 1048576.0);
        }
    }
    o->holding = true;
    return hold * stride;
}

void* ResizeDataHook(void* graph, void* ext, uint32_t bytes, const wchar_t* name)
{
    return g_resize(graph, ext, ObjectBufferBytes(0, ext, bytes), name);
}
void* ResizeBoundsHook(void* graph, void* ext, uint32_t bytes, const wchar_t* name)
{
    return g_resize(graph, ext, ObjectBufferBytes(1, ext, bytes), name);
}

// ---- 2. DfMappingReserve (render thread, around UpdateDistanceFieldObjectBuffers) ----
int32_t g_mapPeak = 0;   // the session's largest mapping Num seen while the key is on
int g_loadCalls = 0;     // calls since the render thread first saw the loading screen up
double g_loadSince = 0;  // when that was (QPC ms)
bool g_loadUnknownLogged = false;
struct MapStats { std::atomic<uint64_t> raises{0}, bytes{0}; } sm;

int32_t MapTarget(int32_t peak, int32_t num)
{
    int64_t t = static_cast<int64_t>(peak) + peak / 8;
    const int64_t h = static_cast<int64_t>(num) + num / 4;
    if (h > t) t = h;
    t = (t + 1023) & ~int64_t(1023);
    return static_cast<int32_t>(t > kMapCap ? kMapCap : t);
}

void MapReserve(uint8_t* dfs, int32_t target, const char* when)
{
    uint8_t* const arr = dfs + g_mapOff;
    const int32_t num = Rd32(arr, 8), max = Rd32(arr, 0xC);
    if (target <= max || num < 0 || num > max) return;
    const double t0 = NowMs();
    g_resizeAlloc(arr, num, target, g_elemSize, g_elemAlign);  // the game's own TArray reallocation (data pointer at +0)
    Wr32(arr, 0xC, target);
    const double ms = NowMs() - t0;
    sm.raises.fetch_add(1, std::memory_order_relaxed);
    sm.bytes.fetch_add(uint64_t(target - max) * g_elemSize, std::memory_order_relaxed);
    Log("[dfscene] DfMappingReserve: loading screen (%s, %.0f ms up): distance-field instance list %d in use, capacity %d -> %d "
        "(%.1f MB, +%.1f MB), %.2f ms; session peak %d",
        when, NowMs() - g_loadSince, num, max, target, double(target) * g_elemSize / 1048576.0,
        double(target - max) * g_elemSize / 1048576.0, ms, g_mapPeak);
}

// ---- 3. DfAddSpread (render thread) ----
struct Held { void* prim; uint8_t age; };
struct Ctx {
    uint8_t* dfs = nullptr;
    int64_t budget = 0;
    int64_t used = 0;           // instances added in this update
    int admitted = 0, forced = 0, freshLoads = 0, nearKept = 0, agedSeen = 0, agedExpected = 0, borrowed = 0;
    bool sawAdd = false, posKnown = false, overtaken = false;
    double px = 0, py = 0, pz = 0, nearUnits = 0;
    std::vector<Held> held;
};
thread_local Ctx* t_ctx = nullptr;
std::mutex g_mx;                              // one update at a time uses the age table (try-lock on the render thread)
struct Age { void* owner; uint8_t age; };
std::unordered_map<void*, Age> g_age;         // primitives held back in their scene's last update: times held back
int g_agedLast = 0;                           // how many were put back in that update
const void* g_agedOwner = nullptr;

struct Stats {
    std::atomic<uint64_t> calls{0}, spreadCalls{0}, held{0}, admitted{0}, inst{0}, forced{0}, overCap{0}, busy{0},
        assetRemoves{0}, freshLoads{0}, reinsertOdd{0}, nearKept{0}, borrowed{0}, camUnknown{0};
    std::atomic<uint32_t> maxAge{0}, maxHeld{0};
};
Stats s;
bool g_camUnknownLogged = false;

// Whether a primitive's bounds come within the near distance of the camera (a primitive with no readable bounds, or any while
// the camera is unknown, counts as near: never held).
bool IsNear(const Ctx* c, const void* prim)
{
    if (!c->posKnown) return true;
    const uint8_t* const proxy = *reinterpret_cast<uint8_t* const*>(static_cast<const uint8_t*>(prim) + g_proxyOff);
    if (!proxy) return true;
    double b[7];  // FBoxSphereBounds: Origin, BoxExtent, SphereRadius (doubles), SphereRadius at radiusOff
    memcpy(b, proxy + g_radiusOff - 48, sizeof b);
    if (!(b[6] >= 0) || !std::isfinite(b[0]) || !std::isfinite(b[1]) || !std::isfinite(b[2])) return true;
    const double dx = b[0] - c->px, dy = b[1] - c->py, dz = b[2] - c->pz;
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz) - b[6];
    return d < c->nearUnits;
}

// a stock update of this scene (or a key change) ends the held-back episode's bookkeeping
void ForgetAged(const void* d)
{
    if (!d || g_agedOwner == d) {
        g_agedLast = 0;
        g_agedOwner = nullptr;
    }
}

}  // namespace

// The add loop's hook (render thread, inside UpdateDistanceFieldObjectBuffers). Public for the self-test.
void DfAddSpreadAddHook(bool add, void* scene, void* prim, void* xforms, void* indices, void* assetAdds, void* assetRemoves)
{
    Ctx* const c = t_ctx;
    if (!c || !add || indices != c->dfs + g_idxOff) {
        g_ppu(add, scene, prim, xforms, indices, assetAdds, assetRemoves);
        return;
    }
    c->sawAdd = true;
    uint8_t age = 0;
    if (const auto it = g_age.find(prim); it != g_age.end() && it->second.owner == c->dfs) age = it->second.age;
    if (age) ++c->agedSeen;
    if (IsNear(c, prim)) {
        ++c->nearKept;
    } else if (c->admitted > 0 && c->used >= c->budget) {
        // past the budget: held, unless held 3 times already, or older than a newer far primitive that already went in
        // (a new add can sit in a free slot before the held-back ones; the older one then goes in over the budget)
        if (age > 0 && c->overtaken) {
            ++c->borrowed;
        } else if (age < kMaxDefers) {
            c->held.push_back({prim, static_cast<uint8_t>(age + 1)});
            return;
        } else {
            ++c->forced;
        }
    }
    if (age == 0 && c->agedSeen < c->agedExpected && !IsNear(c, prim)) c->overtaken = true;
    const int32_t n0 = Rd32(c->dfs, 0), a0 = ArrayNum(assetAdds);
    g_ppu(add, scene, prim, xforms, indices, assetAdds, assetRemoves);
    const int32_t n1 = Rd32(c->dfs, 0);
    if (n1 > n0) c->used += n1 - n0;
    if (age && ArrayNum(assetAdds) > a0) ++c->freshLoads;  // a held-back primitive's asset loaded anew (dropped-and-reloaded <= this)
    ++c->admitted;
}

namespace {

// DfAddSpread's part around the game's call; returns false when it did not run the call
bool SpreadCall(uint8_t* d, void* graph, void* queue, void* scene, void* assetAdds, void* assetRemoves)
{
    if (!g_addHooked || t_ctx || !d) return false;
    std::unique_lock<std::mutex> lk(g_mx, std::try_to_lock);
    if (!lk.owns_lock()) { s.busy.fetch_add(1, std::memory_order_relaxed); ForgetAged(d); return false; }
    const int pending = g_keySpread.load(std::memory_order_relaxed) ? PendingAdds(d) : 0;
    if (pending <= 0 || pending > kMaxPending) {
        if (pending > kMaxPending) s.overCap.fetch_add(1, std::memory_order_relaxed);
        if (PendingAdds(d) > 0) ForgetAged(d);  // the game adds them all now: any held-back episode of this scene ends
        return false;
    }
    s.calls.fetch_add(1, std::memory_order_relaxed);
    Ctx c;
    c.dfs = d;
    c.budget = g_budget.load(std::memory_order_relaxed);
    c.nearUnits = g_nearM.load(std::memory_order_relaxed) * 100.0;  // UE units are centimetres
    c.posKnown = g_camPos && g_camPos(scene, &c.px, &c.py, &c.pz);
    if (!c.posKnown) {
        s.camUnknown.fetch_add(1, std::memory_order_relaxed);
        if (!g_camUnknownLogged) {
            g_camUnknownLogged = true;
            Log("[dfscene] DfAddSpread: the camera of this frame could not be read: nothing held in such updates");
        }
    }
    c.agedExpected = g_agedOwner == d ? g_agedLast : 0;
    c.held.reserve(64);
    const double t0 = NowMs();
    t_ctx = &c;
    g_dfob(d, graph, queue, scene, assetAdds, assetRemoves);
    t_ctx = nullptr;
    const double ms = NowMs() - t0;
    if (!c.sawAdd) return true;  // the add loop did not run (the set is kept as it was): ages kept
    // back into the game's set, with the game's own insert (the set was reset by the game's loop), in their order
    const int after = PendingAdds(d);
    if (after != 0 && !c.held.empty()) s.reinsertOdd.fetch_add(1, std::memory_order_relaxed);
    for (Held& h : c.held) {
        int32_t id = -1;
        void* key = h.prim;
        g_emplace(d + g_setOff, &id, &key, nullptr);
    }
    // the age table: this scene's entries are now exactly the primitives held back in this update (others' only while few)
    for (auto it = g_age.begin(); it != g_age.end();)
        it = (it->second.owner == d || g_age.size() > 4096) ? g_age.erase(it) : std::next(it);
    uint8_t maxAge = 0;
    for (const Held& h : c.held) {
        g_age[h.prim] = {d, h.age};
        if (h.age > maxAge) maxAge = h.age;
    }
    g_agedOwner = d;
    g_agedLast = static_cast<int>(c.held.size());
    s.nearKept.fetch_add(static_cast<uint64_t>(c.nearKept), std::memory_order_relaxed);
    s.borrowed.fetch_add(static_cast<uint64_t>(c.borrowed), std::memory_order_relaxed);
    if (c.held.empty() && !c.forced) return true;
    const int32_t removes = ArrayNum(assetRemoves);
    s.spreadCalls.fetch_add(1, std::memory_order_relaxed);
    s.held.fetch_add(c.held.size(), std::memory_order_relaxed);
    s.admitted.fetch_add(static_cast<uint64_t>(c.admitted), std::memory_order_relaxed);
    s.inst.fetch_add(static_cast<uint64_t>(c.used), std::memory_order_relaxed);
    s.forced.fetch_add(static_cast<uint64_t>(c.forced), std::memory_order_relaxed);
    s.assetRemoves.fetch_add(removes > 0 ? static_cast<uint64_t>(removes) : 0, std::memory_order_relaxed);
    s.freshLoads.fetch_add(static_cast<uint64_t>(c.freshLoads), std::memory_order_relaxed);
    MaxInto(s.maxAge, c.forced ? kMaxDefers : maxAge);
    MaxInto(s.maxHeld, static_cast<uint32_t>(c.held.size()));
    (void)ms;
    return true;
}

}  // namespace

// The UpdateDistanceFieldObjectBuffers call's hook (render thread). Public for the self-test.
void DfAddSpreadDfobHook(void* dfs, void* graph, void* queue, void* scene, void* assetAdds, void* assetRemoves)
{
    uint8_t* const d = static_cast<uint8_t*>(dfs);
    const bool mapOn = d && g_mapOff && g_resizeAlloc && g_keyMap.load(std::memory_order_relaxed);
    bool settled = false;
    if (mapOn) {
        const int ls = g_loading ? g_loading() : -1;
        if (ls < 0 && !g_loadUnknownLogged) {
            g_loadUnknownLogged = true;
            Log("[dfscene] DfMappingReserve: the loading-screen state is not known (needs NpcCreationPacing's frame hook): nothing reserved");
        }
        if (ls == 1) {
            if (g_loadCalls++ == 0) g_loadSince = NowMs();
        } else {
            g_loadCalls = 0;
        }
        // the screen up for a while (not the fade before it, nor the render thread's lagging frame) and seen twice
        settled = g_loadCalls >= 2 && NowMs() - g_loadSince >= g_settleMs;
        // a world build behind the screen: its capacity first (skips the build's own growth steps)
        if (settled && g_mapPeak >= kWorldMin && PendingAdds(d) >= kWorldBuild) MapReserve(d, MapTarget(g_mapPeak, 0), "before a build");
    }
    if (!SpreadCall(d, graph, queue, scene, assetAdds, assetRemoves)) g_dfob(dfs, graph, queue, scene, assetAdds, assetRemoves);
    if (mapOn) {
        const int32_t num = Rd32(d + g_mapOff, 8);
        if (num > g_mapPeak) g_mapPeak = num;
        if (settled && num >= kWorldMin) MapReserve(d, MapTarget(g_mapPeak, num), "after an update");
    }
}

namespace {

uint8_t* CallTarget(uint8_t* call) { return call[0] == 0xE8 ? RipTarget(call, 1, 5) : nullptr; }
bool WideIs(const GameModule& game, const uint8_t* p, const wchar_t* text)
{
    if (!p || !(game.rdata.Contains(p) || game.data.Contains(p))) return false;
    const size_t n = wcslen(text) + 1;
    return memcmp(p, text, n * sizeof(wchar_t)) == 0;
}

// key 1's part: the two resize calls
void DiscoverRes(const GameModule& game, Found* f)
{
    const auto rs = FindAll(game.text, ParsePattern(kResizeSites));
    if (rs.size() != 1 || PrimaryFunction(game, rs[0]) != f->dfob) { f->whyRes = "the object buffers' resize calls not found once in it"; return; }
    uint8_t* const r0 = rs[0] + kResDataE8;
    uint8_t* const r1 = rs[0] + kResBoundsE8;
    uint8_t* const resize = CallTarget(r0);
    if (!resize || CallTarget(r1) != resize || PrimaryFunction(game, resize) != resize) {
        f->whyRes = "the two object buffers' resize calls do not call the same function";
        return;
    }
    if (!WideIs(game, RipTarget(rs[0] + kResDataName, 3, 7), L"DistanceFields.DFObjectData") ||
        !WideIs(game, RipTarget(rs[0] + kResBoundsName, 3, 7), L"DistanceFields.DFObjectBounds")) {
        f->whyRes = "the resize calls do not name DistanceFields.DFObjectData / DFObjectBounds";
        return;
    }
    f->resSite[0] = r0;
    f->resSite[1] = r1;
    f->resize = resize;
    f->okRes = true;
}

// the view layout (DfAddSpread's camera)
void DiscoverCam(const GameModule& game, Found* f)
{
    const auto hits = FindAll(game.text, ParsePattern(kCamSite));
    if (hits.size() != 1) { f->whyCam = "the view layout's code not found once"; return; }
    const Pattern scaleRead = ParsePattern(kScaleRead);
    uint32_t scale = 0;
    for (uint8_t* p = hits[0] + kCamTypeStore; p < hits[0] + kCamTypeStore + kCamScaleSearch && p + 9 <= game.text.end; ++p) {
        if (!MatchesAt(p, scaleRead)) continue;
        memcpy(&scale, p + 5, 4);
        break;
    }
    if (scale < kMatricesBeforeLocation + kLocationBeforeScale || scale > 0x4000 || scale % 4 != 0) {
        f->whyCam = "View->WorldToMetersScale not read where expected";
        return;
    }
    f->locOff = scale - kLocationBeforeScale;
    f->originOff = f->locOff - kMatricesBeforeLocation + kOriginInMatrices;
    f->okCam = true;
}

// key 3's part: the add / update loops, the set, the insert, the proxy's bounds (needs the camera)
void DiscoverSpread(const GameModule& game, Found* f)
{
    const auto as = FindAll(game.text, ParsePattern(kAddSite));
    const auto us = FindAll(game.text, ParsePattern(kUpdSite));
    const auto ls = FindAll(game.text, ParsePattern(kSetLea));
    const auto es = FindAll(game.text, ParsePattern(kEmplace));
    if (as.size() != 1 || us.size() != 1 || ls.size() != 1 || es.size() != 1) {
        f->whySpread = "the add loop's call, the update loop's call, the pending set or AddPrimitive's insert not found once";
        return;
    }
    uint8_t* const addSite = as[0] + kAddE8;
    uint8_t* const ppu = CallTarget(addSite);
    uint8_t* const updSite = us[0] + kUpdE8;
    uint8_t* const emplace = CallTarget(es[0] + kEmplaceE8);
    if (!ppu || !game.text.Contains(ppu) || CallTarget(updSite) != ppu || PrimaryFunction(game, ppu) != ppu) {
        f->whySpread = "the add and update loops do not call the same function (or it is not a function start)";
        return;
    }
    if (PrimaryFunction(game, addSite) != f->dfob || PrimaryFunction(game, updSite) != f->dfob || PrimaryFunction(game, ls[0]) != f->dfob ||
        !(addSite < updSite) || !(ls[0] < addSite)) {
        f->whySpread = "the add loop, the update loop and the pending set are not in UpdateDistanceFieldObjectBuffers in order";
        return;
    }
    if (!emplace || !game.text.Contains(emplace) || PrimaryFunction(game, emplace) != emplace) {
        f->whySpread = "AddPrimitive's insert has no function start as its target";
        return;
    }
    uint32_t setOff, setOff2, idxOff;
    memcpy(&setOff, ls[0] + kSetLeaDisp, 4);
    memcpy(&setOff2, es[0] + kEmplaceDisp, 4);
    memcpy(&idxOff, as[0] + kAddIdxDisp, 4);
    if (setOff != setOff2 || setOff < 0x40 || setOff > 0x2000 || idxOff < 0x08 || idxOff >= setOff) {
        f->whySpread = "the add loop's set and AddPrimitive's set are not the same field";
        return;
    }
    const auto px = FindAll(game.text, ParsePattern(kProxyLoad));
    const auto rr = FindAll(game.text, ParsePattern(kRadiusLoad));
    if (px.size() != 1 || rr.size() != 1 || PrimaryFunction(game, px[0]) != ppu || PrimaryFunction(game, rr[0]) != ppu) {
        f->whySpread = "the proxy or its bounds not found once in ProcessPrimitiveUpdate";
        return;
    }
    const uint32_t proxyOff = px[0][kProxyDisp];
    uint32_t radiusOff;
    memcpy(&radiusOff, rr[0] + kRadiusDisp, 4);
    if (proxyOff > 0x100 || radiusOff < 0x80 || radiusOff > 0x1000) { f->whySpread = "the proxy / bounds offsets are not as expected"; return; }
    if (!f->okCam) { f->whySpread = f->whyCam; return; }
    f->addSite = addSite;
    f->ppu = ppu;
    f->updSite = updSite;
    f->emplace = emplace;
    f->setOff = setOff;
    f->idxOff = idxOff;
    f->proxyOff = proxyOff;
    f->radiusOff = radiusOff;
    f->okSpread = true;
}

// key 2's part: the mapping's growth in ProcessPrimitiveUpdate (found through the update loop's call)
void DiscoverMap(const GameModule& game, Found* f)
{
    const auto us = FindAll(game.text, ParsePattern(kUpdSite));
    uint8_t* const ppu = us.size() == 1 ? CallTarget(us[0] + kUpdE8) : nullptr;
    if (!ppu || PrimaryFunction(game, us[0]) != f->dfob || PrimaryFunction(game, ppu) != ppu) {
        f->whyMap = "ProcessPrimitiveUpdate not found through the update loop's call";
        return;
    }
    const auto gs = FindAll(game.text, ParsePattern(kGrowSite));
    if (gs.size() != 1 || PrimaryFunction(game, gs[0]) != ppu) { f->whyMap = "the instance list's growth not found once in ProcessPrimitiveUpdate"; return; }
    uint32_t growNum, growMax, growLea;
    memcpy(&growNum, gs[0] + kGrowNumDisp, 4);
    memcpy(&growMax, gs[0] + kGrowMaxDisp, 4);
    memcpy(&growLea, gs[0] + kGrowLeaDisp, 4);
    if (growNum != growLea + 8 || growMax != growLea + 0xC || growLea <= f->dfsInScene || growLea - f->dfsInScene > 0x2000) {
        f->whyMap = "the instance list's offsets are not as expected";
        return;
    }
    uint8_t* const rg = CallTarget(gs[0] + kGrowE8);
    if (!rg || !game.text.Contains(rg) || !MatchesAt(rg, ParsePattern(kResizeGrow))) {
        f->whyMap = "the instance list's growth function is not the TArray growth we know";
        return;
    }
    uint32_t sz, sz2, al1, al2;
    memcpy(&sz, rg + kRgSizeImm, 4);
    memcpy(&sz2, rg + kRgSizeImm2, 4);
    memcpy(&al1, rg + kRgAlignImm1, 4);
    memcpy(&al2, rg + kRgAlignImm2, 4);
    uint8_t* const ra = CallTarget(rg + kRgAllocE8);
    if (sz != sz2 || sz < 0x40 || sz > 0x400 || al1 != al2 || (al1 != 8 && al1 != 16) || !ra || PrimaryFunction(game, ra) != ra) {
        f->whyMap = "the instance list's element size, alignment or reallocation call not as expected";
        return;
    }
    f->mapOff = growLea - f->dfsInScene;
    f->resizeAlloc = ra;
    f->elemSize = sz;
    f->elemAlign = al1;
    f->okMap = true;
}

// The call site (needed by all three); then each key's part on its own.
bool Discover(const GameModule& game, Found* f, const char** why)
{
    const auto ds = FindAll(game.text, ParsePattern(kDfobSite));
    if (ds.size() != 1) { *why = "the UpdateDistanceFieldObjectBuffers call not found once"; return false; }
    uint8_t* const dfobSite = ds[0] + kDfobE8;
    uint8_t* const dfob = CallTarget(dfobSite);
    if (!dfob || !game.text.Contains(dfob) || PrimaryFunction(game, dfob) != dfob) {
        *why = "the UpdateDistanceFieldObjectBuffers call has no function start as its target";
        return false;
    }
    uint32_t viewsOff;
    memcpy(&f->dfsInScene, ds[0] + kDfobDfsDisp, 4);
    memcpy(&viewsOff, ds[0] + kDfobViewsDisp, 4);
    f->sceneOff = ds[0][kDfobSceneDisp];
    f->viewsOff = viewsOff;
    if (f->sceneOff < 0x08 || viewsOff < 0x80 || viewsOff > 0x4000 || f->dfsInScene < 0x100 || f->dfsInScene > 0x10000) {
        *why = "the renderer's Scene / Views or the scene's distance-field data offsets are not as expected";
        return false;
    }
    f->dfobSite = dfobSite;
    f->dfob = dfob;
    DiscoverRes(game, f);
    DiscoverCam(game, f);
    DiscoverSpread(game, f);
    DiscoverMap(game, f);
    return true;
}

bool Retarget(uint8_t* site, uint8_t* stub)
{
    const int64_t d = stub - (site + 5);
    if (d != static_cast<int32_t>(d)) return false;
    const int32_t rel = static_cast<int32_t>(d);
    return WriteCode(site + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel), 4));
}

// a jump stub [jmp qword [rip+0]; target] at p
void JumpStub(uint8_t* p, void* target)
{
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(p, jmpAbs, sizeof jmpAbs);
    memcpy(p + sizeof jmpAbs, &target, sizeof(void*));
}

}  // namespace

bool InstallDfSceneKeep(bool beforeGameCode, int bufKey, int mapKey, int spreadKey, int budget, int nearM)
{
    if (g_found) return true;
    LARGE_INTEGER fr;
    QueryPerformanceFrequency(&fr);
    g_qpcMs = 1000.0 / static_cast<double>(fr.QuadPart);
    bool wantBuf = bufKey != 0, wantMap = mapKey != 0, wantSpread = spreadKey != 0;
    if (!wantBuf && !wantMap && !wantSpread) {
        Log("[dfscene] DfObjectBufferKeep, DfMappingReserve and DfAddSpread off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[dfscene] only installed when the game starts (version.dll route), not installed");
        return false;
    }
    const GameModule& game = Game();
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) {
        Log("[dfscene] DfObjectBufferKeep / DfMappingReserve / DfAddSpread not installed: %s", why);
        return false;
    }
    if (wantBuf && !f.okRes) Log("[dfscene] DfObjectBufferKeep not installed: %s", f.whyRes);
    if (wantSpread && !f.okSpread) Log("[dfscene] DfAddSpread not installed: %s", f.whySpread);
    if (wantMap && !f.okMap) Log("[dfscene] DfMappingReserve not installed: %s", f.whyMap);
    wantBuf = wantBuf && f.okRes;
    wantSpread = wantSpread && f.okSpread;
    wantMap = wantMap && f.okMap;
    if (!wantBuf && !wantSpread && !wantMap) return false;
    // page 0: code stubs (read + execute); page 1: the renderer slot the call's stub writes (read + write)
    uint8_t* const mem = AllocNearGame(8192);
    if (!mem) { Log("[dfscene] not installed: no memory near the game's code"); return false; }
    uint8_t* const slot = mem + 4096;
    // stub 0 (the update call): mov [rip+slot],r14 (the scene renderer); jmp DfAddSpreadDfobHook
    {
        const int64_t rel = slot - (mem + 7);
        const int32_t r32 = static_cast<int32_t>(rel);
        const uint8_t mov[3] = {0x4C, 0x89, 0x35};
        memcpy(mem, mov, 3);
        memcpy(mem + 3, &r32, 4);
        JumpStub(mem + 7, reinterpret_cast<void*>(&DfAddSpreadDfobHook));
    }
    JumpStub(mem + 32, reinterpret_cast<void*>(&DfAddSpreadAddHook));
    JumpStub(mem + 64, reinterpret_cast<void*>(&ResizeDataHook));
    JumpStub(mem + 96, reinterpret_cast<void*>(&ResizeBoundsHook));
    DWORD old;
    VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    g_rendererSlot = reinterpret_cast<uint8_t* const*>(slot);
    g_dfob = reinterpret_cast<DfobFn>(f.dfob);
    g_ppu = reinterpret_cast<PpuFn>(f.ppu);
    g_emplace = reinterpret_cast<EmplaceFn>(f.emplace);
    g_resize = reinterpret_cast<ResizeFn>(f.resize);
    g_resizeAlloc = wantMap ? reinterpret_cast<ResizeAllocFn>(f.resizeAlloc) : nullptr;
    g_setOff = f.setOff;
    g_idxOff = f.idxOff;
    g_proxyOff = f.proxyOff;
    g_radiusOff = f.radiusOff;
    g_mapOff = wantMap ? f.mapOff : 0;
    g_elemSize = f.elemSize;
    g_elemAlign = f.elemAlign;
    g_sceneOff = f.sceneOff;
    g_viewsOff = f.viewsOff;
    g_locOff = f.locOff;
    g_originOff = f.originOff;
    g_keyBuf = bufKey ? 1 : 0;
    g_keyMap = mapKey ? 1 : 0;
    g_keySpread = spreadKey ? 1 : 0;
    g_budget = budget;
    g_nearM = nearM;
    g_found = true;
    // 1: the two resize calls (each passes the game's size on while the key is 0)
    if (wantBuf) {
        if (Retarget(f.resSite[0], mem + 64) && Retarget(f.resSite[1], mem + 96)) g_resHooked = true;
        else {
            const int32_t back = static_cast<int32_t>(f.resize - (f.resSite[0] + 5));
            WriteCode(f.resSite[0] + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&back), 4));
            Log("[dfscene] DfObjectBufferKeep not installed: could not patch the resize calls");
        }
    }
    // 3 before the outer call: until the outer call goes through us, the add hook only passes calls on
    if (wantSpread) {
        if (Retarget(f.addSite, mem + 32)) g_addHooked = true;
        else Log("[dfscene] DfAddSpread not installed: could not patch the add loop's call");
    }
    if (wantMap || g_addHooked) {
        if (Retarget(f.dfobSite, mem)) g_dfobHooked = true;
        else {
            if (g_addHooked) {
                const int32_t back = static_cast<int32_t>(f.ppu - (f.addSite + 5));
                WriteCode(f.addSite + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&back), 4));
                g_addHooked = false;
            }
            g_mapOff = 0;
            Log("[dfscene] DfMappingReserve / DfAddSpread not installed: could not patch the distance-field update's call");
        }
    }
    Log("[dfscene] UpdateDistanceFieldObjectBuffers call exe+0x%X (exe+0x%X; renderer Scene +0x%X, Views +0x%X; scene +0x%X), "
        "object-buffer resize calls exe+0x%X / exe+0x%X (exe+0x%X), add loop exe+0x%X (ProcessPrimitiveUpdate exe+0x%X; proxy +0x%X, "
        "radius +0x%X), pending set +0x%X, insert exe+0x%X, view location +0x%X / origin +0x%X, instance list +0x%X (%u B, align %u, "
        "realloc exe+0x%X)",
        static_cast<unsigned>(f.dfobSite - game.base), static_cast<unsigned>(f.dfob - game.base), f.sceneOff, f.viewsOff, f.dfsInScene,
        f.okRes ? static_cast<unsigned>(f.resSite[0] - game.base) : 0u, f.okRes ? static_cast<unsigned>(f.resSite[1] - game.base) : 0u,
        f.okRes ? static_cast<unsigned>(f.resize - game.base) : 0u, f.okSpread ? static_cast<unsigned>(f.addSite - game.base) : 0u,
        f.okSpread ? static_cast<unsigned>(f.ppu - game.base) : 0u, f.proxyOff, f.radiusOff, f.setOff,
        f.okSpread ? static_cast<unsigned>(f.emplace - game.base) : 0u, f.locOff, f.originOff, f.mapOff, f.elemSize, f.elemAlign,
        f.okMap ? static_cast<unsigned>(f.resizeAlloc - game.base) : 0u);
    Log("[dfscene] DfObjectBufferKeep %s, DfMappingReserve %s, DfAddSpread %s (%d instances per update, near %d m)",
        !g_resHooked ? "not installed" : bufKey ? "on (object buffers kept at the session's largest size)" : "off (the game's way)",
        !(g_dfobHooked && g_mapOff) ? "not installed" : mapKey ? "on (instance list reserved behind loading screens)" : "off (the game's way)",
        !g_addHooked || !g_dfobHooked ? "not installed" : spreadKey ? "on (a big burst of distance-field adds spread over up to 4 updates)"
                                                                     : "off (the game's way)",
        budget, nearM);
    return true;
}

void SetDfSceneKeep(int bufKey, int mapKey, int spreadKey, int budget, int nearM)
{
    bufKey = bufKey ? 1 : 0;
    mapKey = mapKey ? 1 : 0;
    spreadKey = spreadKey ? 1 : 0;
    g_budget = budget;
    g_nearM = nearM;
    if (!g_found) { g_keyBuf = bufKey; g_keyMap = mapKey; g_keySpread = spreadKey; return; }
    if (g_keyBuf.exchange(bufKey) != bufKey && g_resHooked) {
        if (!bufKey) { std::lock_guard<std::mutex> lk(g_bufMx); g_objPeak = 0; for (Owner& o : g_owner) o = Owner{}; }
        Log("[dfscene] DfObjectBufferKeep %s", bufKey ? "on: distance-field object buffers kept at the session's largest size"
                                                      : "off: the game's sizes (the buffers shrink at the next change)");
    }
    if (g_keyMap.exchange(mapKey) != mapKey && g_dfobHooked && g_mapOff)
        Log("[dfscene] DfMappingReserve %s", mapKey ? "on: the instance list reserved behind loading screens" : "off: the game's growth");
    if (g_keySpread.exchange(spreadKey) != spreadKey && g_addHooked) {
        {
            std::lock_guard<std::mutex> lk(g_mx);  // the render thread only try-locks: it never waits for this
            g_age.clear();
            ForgetAged(nullptr);
        }
        Log("[dfscene] DfAddSpread %s", spreadKey ? "on: a big burst of distance-field adds spread over up to 4 updates"
                                                  : "off: every pending distance-field add in one update, as before");
    }
}

void LogDfSceneKeepStats()
{
    if (!g_found) return;
    const uint64_t bc = sb.calls.exchange(0), bh = sb.held.exchange(0), be = sb.episodes.exchange(0);
    const uint32_t bmb = sb.heldMB.exchange(0);
    if (bh)
        Log("[dfscene] DfObjectBufferKeep since last report: %llu of %llu object-buffer updates kept a larger size (%llu new "
            "episodes, at most +%u MB held), session peak %u objects",
            static_cast<unsigned long long>(bh), static_cast<unsigned long long>(bc), static_cast<unsigned long long>(be), bmb,
            g_objPeak);
    const uint64_t mr = sm.raises.exchange(0), mbytes = sm.bytes.exchange(0);
    if (mr)
        Log("[dfscene] DfMappingReserve since last report: %llu raises behind loading screens (+%.1f MB), session peak %d entries",
            static_cast<unsigned long long>(mr), double(mbytes) / 1048576.0, g_mapPeak);
    const uint64_t spread = s.spreadCalls.exchange(0), over = s.overCap.exchange(0), busy = s.busy.exchange(0);
    const uint64_t calls = s.calls.exchange(0);
    const uint64_t nearK = s.nearKept.exchange(0), camU = s.camUnknown.exchange(0);
    if (!spread && !busy) return;
    const uint64_t held = s.held.exchange(0), adm = s.admitted.exchange(0), inst = s.inst.exchange(0),
                   forced = s.forced.exchange(0), rem = s.assetRemoves.exchange(0), fresh = s.freshLoads.exchange(0),
                   odd = s.reinsertOdd.exchange(0), borrowed = s.borrowed.exchange(0);
    const uint32_t maxAge = s.maxAge.exchange(0), maxHeld = s.maxHeld.exchange(0);
    Log("[dfscene] DfAddSpread since last report: %llu of %llu distance-field updates spread (%llu primitives / %llu instances "
        "added in them, %llu held back, most in one update %u, most holds of one primitive %u, %llu through after %d holds, "
        "%llu older ones over the budget, %llu near the camera never held, %llu updates with the camera unknown); held-back "
        "primitives loading their asset anew %llu (assets released in those updates %llu); %llu updates with more than %d "
        "pending (as before)%s",
        static_cast<unsigned long long>(spread), static_cast<unsigned long long>(calls), static_cast<unsigned long long>(adm),
        static_cast<unsigned long long>(inst), static_cast<unsigned long long>(held), maxHeld, maxAge,
        static_cast<unsigned long long>(forced), kMaxDefers, static_cast<unsigned long long>(borrowed),
        static_cast<unsigned long long>(nearK), static_cast<unsigned long long>(camU), static_cast<unsigned long long>(fresh),
        static_cast<unsigned long long>(rem), static_cast<unsigned long long>(over), kMaxPending,
        busy || odd ? " (unexpected: another update at the same time or a set not empty after the game's loop)" : "");
}

// ---- self-test ----
bool DfSceneKeepTestDiscover(const GameModule& game, uint32_t rva[9], uint32_t off[12])
{
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) { printf("    DfSceneKeep: %s\n", why); return false; }
    if (!f.okRes || !f.okSpread || !f.okMap || !f.okCam) {
        printf("    DfSceneKeep parts: res %d (%s) spread %d (%s) map %d (%s) cam %d (%s)\n", f.okRes, f.whyRes, f.okSpread, f.whySpread,
               f.okMap, f.whyMap, f.okCam, f.whyCam);
        return false;
    }
    rva[0] = static_cast<uint32_t>(f.dfobSite - game.base);
    rva[1] = static_cast<uint32_t>(f.dfob - game.base);
    rva[2] = static_cast<uint32_t>(f.addSite - game.base);
    rva[3] = static_cast<uint32_t>(f.ppu - game.base);
    rva[4] = static_cast<uint32_t>(f.updSite - game.base);
    rva[5] = static_cast<uint32_t>(f.emplace - game.base);
    rva[6] = static_cast<uint32_t>(f.resSite[0] - game.base);
    rva[7] = static_cast<uint32_t>(f.resSite[1] - game.base);
    rva[8] = static_cast<uint32_t>(f.resizeAlloc - game.base);
    off[0] = f.setOff;
    off[1] = f.idxOff;
    off[2] = f.dfsInScene;
    off[3] = f.mapOff;
    off[4] = f.proxyOff;
    off[5] = f.radiusOff;
    off[6] = f.elemSize;
    off[7] = f.elemAlign;
    off[8] = f.sceneOff;
    off[9] = f.viewsOff;
    off[10] = f.locOff;
    off[11] = f.originOff;
    return true;
}

void DfSceneKeepTestSetup(const DfSceneKeepTestFakes& t)
{
    LARGE_INTEGER fr;
    QueryPerformanceFrequency(&fr);
    g_qpcMs = 1000.0 / static_cast<double>(fr.QuadPart);
    g_dfob = reinterpret_cast<DfobFn>(t.dfob);
    g_ppu = reinterpret_cast<PpuFn>(t.ppu);
    g_emplace = reinterpret_cast<EmplaceFn>(t.emplace);
    g_resize = reinterpret_cast<ResizeFn>(t.resize);
    g_resizeAlloc = reinterpret_cast<ResizeAllocFn>(t.resizeAlloc);
    g_loading = reinterpret_cast<LoadingFn>(t.loading);
    g_camPos = t.camPos ? reinterpret_cast<CameraFn>(t.camPos) : &RenderCameraPos;
    g_rendererSlot = reinterpret_cast<uint8_t* const*>(t.rendererSlot);
    g_sceneOff = t.sceneOff;
    g_viewsOff = t.viewsOff;
    g_locOff = t.locOff;
    g_originOff = t.originOff;
    g_setOff = t.setOff;
    g_idxOff = t.idxOff;
    g_proxyOff = t.proxyOff;
    g_radiusOff = t.radiusOff;
    g_mapOff = t.mapOff;
    g_elemSize = t.elemSize;
    g_elemAlign = 16;
    g_settleMs = t.settleMs;
    g_keyBuf = t.bufKey;
    g_keyMap = t.mapKey;
    g_keySpread = t.spreadKey;
    g_budget = t.budget;
    g_nearM = t.nearM;
    g_addHooked = g_dfobHooked = g_resHooked = true;
    g_age.clear();
    g_agedLast = 0;
    g_agedOwner = nullptr;
    g_objPeak = 0;
    for (Owner& o : g_owner) o = Owner{};
    g_mapPeak = 0;
    g_loadCalls = 0;
}

void DfSceneKeepTestKeys(int bufKey, int mapKey, int spreadKey) { SetDfSceneKeep(bufKey, mapKey, spreadKey, g_budget, g_nearM); }

void* DfSceneKeepTestResize(int site, void* graph, void* ext, uint32_t bytes, const wchar_t* name)
{
    return site ? ResizeBoundsHook(graph, ext, bytes, name) : ResizeDataHook(graph, ext, bytes, name);
}

bool DfSceneKeepTestCamera(void* scene, double* x, double* y, double* z) { return RenderCameraPos(scene, x, y, z); }

void DfSceneKeepTestCounts(uint64_t c[8])
{
    c[0] = s.spreadCalls.load();
    c[1] = s.held.load();
    c[2] = s.forced.load();
    c[3] = s.overCap.load();
    c[4] = s.maxAge.load();
    c[5] = s.nearKept.load();
    c[6] = s.borrowed.load();
    c[7] = sm.raises.load();
}

}  // namespace opf
