// WaterInfoSkip (worker waterinfo, 2026-11-07, findings "waterinfo"; public since 1.3.0, port130g; review rev130d).
// [Fixes] WaterInfoSkip = 0: not installed (start-up); 1 (default): on; then live 0/1 (lab builds: 2 = proof, below). Installed
// at start-up with the water-info render's game-thread detour (water_info.cpp) when the layout below is confirmed in the game's
// code (rev130d WI-M2); any change of the value forgets every zone's last inputs (WI-M1: nothing is recorded while off).
//
// What it skips: a water zone's water-info redraw whose inputs are exactly those of the zone's last redraw. The UE 5.3 Water
// plugin redraws a zone's whole top-down water-info texture (three FSceneRenderers: ground depth, water colour, dilation;
// WaterInfoRendering.cpp) whenever AWaterZone::bNeedsWaterInfoRebuild is set. Most redraws have new inputs (a landscape piece
// or a water body came or went: findings "bridgelake", "play1107why"), but in play the game also marks every zone at once
// with nothing changed (8 zones in one frame, 23-43 ms of render thread, when a menu opens or a save is made: play1107c).
//
// The texture is a function of what the three renderers draw and of the parameters the merge and finalize passes get
// (WaterInfoMerge.usf, WaterInfoFinalize.usf: per pixel, from the depth / colour / dilation captures, CaptureZ, the water
// height extents, GroundZMin and the velocity blur radius). The render function (UE::WaterInfo::UpdateWaterInfoRendering,
// exe+0x42ED870, game thread; its context FRenderingContext = {zone +0, render target +8, bodies +0x10, ground weak pointers
// +0x20, CaptureZ +0x30}) builds all of it from:
//   - the context: the zone, the render target and its resource (UTexture +0x110, GetResource), the water bodies, the
//     landscape components, CaptureZ;
//   - the zone: ZoneExtent +0x2C0, VelocityBlurRadius +0x2D8, bEnableLocalOnlyTessellation +0x2E0 / LocalTessellationExtent
//     +0x2E8 / LocalTessellationCenter +0x310 or, without local tessellation, the zone's location (root component +0x1A0,
//     its world location +0x280) (capture extent and centre: GetDynamicWaterMeshCenter), WaterHeightExtents +0x304,
//     GroundZMin +0x30C;
//   - each body: its Bounds (USceneComponent +0x108, the capture-bounds culling) and its owner's (+0xA8) two water-info mesh
//     components (+0x2C8, +0x2D0) with their ComponentIds (+0x36C), which are what the colour and dilation passes draw;
//   - each landscape component: its Bounds (+0x108; position and size).
// Every one of these offsets is checked in that function's own code (or its GetDynamicWaterMeshCenter / GetResource) at
// install (Pins below); a mismatch = not installed, with a log line. The fingerprint is all of the above. The fix skips the
// call (no renderers made, nothing queued) when the fingerprint equals the one of the zone's last redraw (same zone object,
// same render target resource).
//
// EQUIVALENT, not identical (rev130d WI-S1): what the fingerprint cannot see is the meshes' and materials' contents, texture
// streaming state (e.g. landscape heightmap mips) and material parameters. Stock redraws a zone whenever something marks it
// (a menu opened, a save), so whether stock refreshes such content depends on when a menu opens; the fix keeps the texture
// of the zone's last real redraw until its inputs change. Lab proof (value 2): every redraw is made as in stock, its texture
// read back and compared bit for bit with the zone's previous one; 36 of 36 same-input redraws (32 menu / save, 4 sea zone)
// were bit-identical (findings "waterinfo").
// Side effect of a skip: the frame runs as a frame without a water-info update (Niagara GPU sims in the main view, no
// water-view VT feedback / DF streaming requests; findings "waterdepth"); the texture and the water materials' parameters
// (set by UpdateWaterInfoTexture before this call, unchanged) are as in stock.
//
// Lab value 2 reads the texture back (render thread, the engine's own FDynamicRHI::RHIReadSurfaceFloatData_RenderThread, slot
// 0x4A8 of GDynamicRHI: flushes the RHI thread, waits for the GPU; for tests only); it also logs every redraw's changed
// pixels per channel and the landscape components that came or went. The public build has none of it (WI-S2).
#include "common.h"
#include "water_zone.h"  // WaterHeightExtents / GroundZMin: the layout WaterInfoSpread also reads (rev130f WS-S3)

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace opf {
namespace {


// Context, zone, body, component, texture offsets (pinned by Pins in UpdateWaterInfoRendering's code).
constexpr size_t kCtxZone = 0, kCtxRt = 8, kCtxBodies = 0x10, kCtxGround = 0x20, kCtxCapZ = 0x30;
constexpr size_t kTexResource = 0x110;
constexpr size_t kBounds = 0x108, kBoundsSize = 7 * 8;  // FBoxSphereBounds (doubles): Origin, BoxExtent, SphereRadius
constexpr size_t kBodyOwner = 0xA8, kOwnerInfoMesh = 0x2C8, kOwnerDilatedMesh = 0x2D0, kComponentId = 0x36C;
constexpr size_t kZoneRoot = 0x1A0, kRootLocation = 0x280;  // GetDynamicWaterMeshCenter without local tessellation
struct ZoneField { size_t at, size; };
constexpr ZoneField kZoneFields[] = {{0x2C0, 16}, {0x2D8, 4}, {0x2E0, 1}, {0x2E8, 24}, {kWaterZoneHeightExtents, 8}, {kWaterZoneGroundZMin, 4}, {0x310, 24}};
static_assert(kWaterZoneHeightExtents == 0x304 && kWaterZoneGroundZMin == 0x30C, "the pins below read these offsets");
// GUObjectArray (FUObjectArray, UE 5.3; as object_replace_guard.cpp)
constexpr size_t kArrayChunks = 0x10, kArrayNum = 0x24;

// The layout's reads in UpdateWaterInfoRendering (searched in its first kFnSpan bytes; each must be there):
constexpr size_t kFnSpan = 0x1600;
struct Pin { const char* what; const char* bytes; };
constexpr Pin kPins[] = {
    {"context +8 (render target)", "49 8B 46 08 48 85 C0 0F 84 ?? ?? ?? ?? 8B 40 08 A9 00 00 00 60"},
    {"context +0 (zone)", "4D 85 ED 0F 84 ?? ?? ?? ?? 49 8B 0E"},
    {"context +0x30 (CaptureZ)", "F3 45 0F 10 5E 30"},
    {"context +0x10 (bodies)", "49 8B 76 10 49 63 46 18"},
    {"context +0x20 (ground)", "49 8B 5E 20 49 63 46 28"},
    {"zone +0x2E0", "44 38 B9 E0 02 00 00"},
    {"zone +0x2E8", "44 0F 10 99 E8 02 00 00 F2 44 0F 10 B1 F8 02 00 00"},
    {"zone +0x2C0", "0F 10 B9 C0 02 00 00"},
    {"zone +0x304", "48 8B 81 04 03 00 00"},
    {"zone +0x30C", "F3 0F 10 B9 0C 03 00 00"},
    {"zone +0x2D8", "8B 81 D8 02 00 00"},
    {"body Bounds +0x108", "F2 0F 10 8B 08 01 00 00"},
    {"body Bounds +0x110", "F2 0F 10 9B 10 01 00 00"},
    {"body owner +0xA8", "48 8B BB A8 00 00 00"},
    {"owner +0x2C8", "4C 8B 87 C8 02 00 00"},
    {"owner +0x2D0", "4C 8B 83 D0 02 00 00"},
    {"ComponentId +0x36C", "4C 8D 80 6C 03 00 00"},
};
// GetDynamicWaterMeshCenter (exe+0x42D19D0), called right before the CaptureZ read: +0x2E0, +0x310 (local) or root +0x1A0, +0x280.
constexpr char kCenterCall[] = "E8 ?? ?? ?? ?? F2 44 0F 10 55 ?? F2 44 0F 10 4D ?? 41 0F 28 C2 F3 45 0F 10 5E 30";
constexpr char kCenter[] =
    "48 83 EC 28 80 B9 E0 02 00 00 00 74 ?? 0F 10 81 10 03 00 00 48 8B C2 F2 0F 10 89 20 03 00 00 0F 11 02 F2 0F 11 4A 10 48 83 C4 "
    "28 C3 48 8B 81 A0 01 00 00 48 85 C0 74 ?? 0F 10 80 80 02 00 00 0F 11 04 24 0F 10 80 90 02 00 00";
// UTexture::GetResource (exe+0x37ADA40 through a jmp thunk), the render target's: ... mov rax,[rbx+110h]; add rsp,20h; pop rbx; ret.
constexpr char kResCall[] = "49 8B 4E 08 48 8B D8 E8";
constexpr char kResRet[] = "48 8B 83 10 01 00 00 48 83 C4 20 5B C3";

uint8_t* g_objArray = nullptr;
bool g_installed = false;
std::atomic<int> g_mode{0};
int g_lastMode = 0;  // game thread: the value the zones' records were made under
// A redraw is skipped only when the zone's inputs last changed at least this long ago (port130g, lab run p130g3-lab: a redraw
// 0.15 s after a teleport's input change had the same inputs but 138 different pixels of velocity / water height: a water
// body's render data that arrived a frame later, which the inputs can't show). Menu and save bursts come seconds later.
uint64_t g_settleMs = 2000;

struct Fingerprint {
    std::vector<uint8_t> bytes;  // everything listed above, in a fixed order
    std::vector<uint64_t> ground;  // sorted weak pointers (for the +/- log)
};
struct ZoneState {                // game thread
    const void* zone = nullptr;
    int32_t zoneIndex = -1, zoneSerial = 0;
    Fingerprint last;             // the zone's last redraw (made or skipped)
    std::vector<std::pair<uint64_t, std::array<double, 4>>> groundXY;  // weak ptr -> XY min/max at the last redraw
    bool have = false;
    uint64_t changedMs = 0;       // GetTickCount64 of the last redraw whose inputs differed from the one before (or the first)
    uint64_t seenMs = 0;          // GetTickCount64 of the zone's last redraw request (for the bound below)
};
// One entry per zone address seen, at most kMaxZones (rev130m WI-N1): a new zone past the bound replaces the entry asked
// least recently; a dropped entry only loses its record, so that zone's next redraw is made (stock), never skipped.
constexpr size_t kMaxZones = 64;
std::vector<ZoneState> g_zones;
std::atomic<uint64_t> g_calls{0}, g_skipped{0}, g_candidates{0}, g_forgets{0};

template <class T> T Rd(const uint8_t* p) { T v; memcpy(&v, p, sizeof v); return v; }

bool ReadMem(void* dst, const void* src, size_t n)  // SEH only here: the callers hold C++ objects
{
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> bool Get(const uint8_t* p, T* v) { return p && ReadMem(v, p, sizeof(T)); }

const uint8_t* Resolve(uint64_t weak)  // TWeakObjectPtr {ObjectIndex, SerialNumber} -> object, nullptr if stale
{
    if (!g_objArray) return nullptr;
    const int32_t index = static_cast<int32_t>(weak & 0xFFFFFFFF), serial = static_cast<int32_t>(weak >> 32);
    int32_t num = 0;
    uint8_t* const* chunks = nullptr;
    if (!Get(g_objArray + kArrayNum, &num) || !Get(g_objArray + kArrayChunks, &chunks)) return nullptr;
    if (index < 0 || index >= num || !chunks) return nullptr;
    uint8_t* chunk = nullptr;
    if (!Get(reinterpret_cast<const uint8_t*>(chunks + (index >> 16)), &chunk) || !chunk) return nullptr;
    const uint8_t* item = chunk + 24 * (index & 0xFFFF);  // FUObjectItem {Object, Flags, ClusterRootIndex, SerialNumber}
    int32_t s = 0;
    const uint8_t* obj = nullptr;
    if (!Get(item + 16, &s) || s != serial || !Get(item, &obj)) return nullptr;
    return obj;
}

bool Put(std::vector<uint8_t>& out, const uint8_t* p, size_t n)  // false = not readable
{
    uint8_t buf[64];
    if (n > sizeof buf || !p || !ReadMem(buf, p, n)) return false;
    out.insert(out.end(), buf, buf + n);
    return true;
}
void PutValue(std::vector<uint8_t>& out, const void* v, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(v);
    out.insert(out.end(), b, b + n);
}

// The fingerprint of one context; false = something not readable (then never skipped).
bool MakeFingerprint(const uint8_t* ctx, Fingerprint* fp, std::vector<std::pair<uint64_t, std::array<double, 4>>>* groundXY)
{
    fp->bytes.clear();
    fp->ground.clear();
    groundXY->clear();
    const uint8_t *zone = nullptr, *tex = nullptr;
    if (!Get(ctx + kCtxZone, &zone) || !Get(ctx + kCtxRt, &tex) || !zone || !tex) return false;
    if (!Put(fp->bytes, ctx + kCtxZone, 8) || !Put(fp->bytes, ctx + kCtxRt, 8) || !Put(fp->bytes, tex + kTexResource, 8) ||
        !Put(fp->bytes, ctx + kCtxCapZ, 4))
        return false;
    for (const ZoneField& f : kZoneFields)
        if (!Put(fp->bytes, zone + f.at, f.size)) return false;
    // the capture centre without local tessellation: the zone's location (GetDynamicWaterMeshCenter)
    const uint8_t* root = nullptr;
    if (!Get(zone + kZoneRoot, &root)) return false;
    PutValue(fp->bytes, &root, 8);
    if (root && !Put(fp->bytes, root + kRootLocation, 24)) return false;
    // bodies, sorted by pointer
    const uint8_t* const* bodies = nullptr;
    int32_t nb = 0;
    if (!Get(ctx + kCtxBodies, &bodies) || !Get(ctx + kCtxBodies + 8, &nb) || nb < 0 || nb > 10000 || (nb && !bodies)) return false;
    std::vector<const uint8_t*> bs(static_cast<size_t>(nb));
    if (nb && !ReadMem(bs.data(), bodies, sizeof(void*) * nb)) return false;
    std::sort(bs.begin(), bs.end());
    for (const uint8_t* b : bs) {
        PutValue(fp->bytes, &b, 8);
        if (!b) continue;
        const uint8_t* owner = nullptr;
        if (!Put(fp->bytes, b + kBounds, kBoundsSize) || !Get(b + kBodyOwner, &owner)) return false;
        PutValue(fp->bytes, &owner, 8);
        if (!owner) continue;
        for (size_t at : {kOwnerInfoMesh, kOwnerDilatedMesh}) {
            const uint8_t* mesh = nullptr;
            if (!Get(owner + at, &mesh)) return false;
            PutValue(fp->bytes, &mesh, 8);
            if (mesh && !Put(fp->bytes, mesh + kComponentId, 4)) return false;
        }
    }
    // ground components, sorted by weak pointer
    const uint64_t* ground = nullptr;
    int32_t ng = 0;
    if (!Get(ctx + kCtxGround, &ground) || !Get(ctx + kCtxGround + 8, &ng) || ng < 0 || ng > 100000 || (ng && !ground)) return false;
    fp->ground.resize(static_cast<size_t>(ng));
    if (ng && !ReadMem(fp->ground.data(), ground, 8 * static_cast<size_t>(ng))) return false;
    std::sort(fp->ground.begin(), fp->ground.end());
    for (uint64_t g : fp->ground) {
        PutValue(fp->bytes, &g, 8);
        const uint8_t* c = Resolve(g);
        double bnd[7];
        if (!c || !ReadMem(bnd, c + kBounds, sizeof bnd)) {
            const uint64_t none = ~0ull;
            PutValue(fp->bytes, &none, 8);
            continue;
        }
        PutValue(fp->bytes, bnd, sizeof bnd);
        groundXY->push_back({g, {bnd[0] - bnd[3], bnd[1] - bnd[4], bnd[0] + bnd[3], bnd[1] + bnd[4]}});
    }
    return true;
}


// Every layout read the fingerprint relies on, in UpdateWaterInfoRendering's own code (fn = its entry; the detour's bytes there are
// not part of any pin).
bool CheckPins(const GameModule& game, uint8_t* fn, const char** why)
{
    if (!fn || !game.text.Contains(fn)) { *why = "UpdateWaterInfoRendering not found"; return false; }
    uint8_t* const end = game.text.Contains(fn + kFnSpan) ? fn + kFnSpan : game.text.end;
    const Section body{fn, end};
    for (const Pin& p : kPins)
        if (FindAll(body, ParsePattern(p.bytes)).empty()) { *why = p.what; return false; }
    const auto cc = FindAll(body, ParsePattern(kCenterCall));
    uint8_t* const center = cc.size() == 1 ? RipTarget(cc[0], 1, 5) : nullptr;
    if (!center || !game.text.Contains(center) || !MatchesAt(center, ParsePattern(kCenter))) { *why = "GetDynamicWaterMeshCenter (+0x310 / root +0x1A0 +0x280)"; return false; }
    const auto rc = FindAll(body, ParsePattern(kResCall));
    uint8_t* res = rc.size() == 1 ? RipTarget(rc[0] + 7, 1, 5) : nullptr;
    if (res && game.text.Contains(res) && res[0] == 0xE9) res = RipTarget(res, 1, 5);  // a jmp thunk
    bool resOk = false;
    if (res && game.text.Contains(res) && game.text.Contains(res + 0x80))
        resOk = !FindAll(Section{res, res + 0x80}, ParsePattern(kResRet)).empty();
    if (!resOk) { *why = "UTexture::GetResource (+0x110)"; return false; }
    return true;
}

}  // namespace


// Start-up: key = WaterInfoSkip at start-up (0 = not installed); gtFn = UpdateWaterInfoRendering when its game-thread detour is in.
bool InstallWaterInfoSkip(bool beforeGameCode, int key, uint8_t* gtFn, const wchar_t* iniPath)
{
    if (!key) { Log("[waterinfo] WaterInfoSkip off in OblivionPerfFix.ini (takes a restart to switch on)"); return false; }
    if (!beforeGameCode) { Log("[waterinfo] WaterInfoSkip: start-up only (version.dll), not installed"); return false; }
    if (!gtFn) { Log("[waterinfo] WaterInfoSkip: the water-info render's game-thread detour is not in: not installed"); return false; }
    const char* why = "";
    if (!CheckPins(Game(), gtFn, &why)) { Log("[waterinfo] WaterInfoSkip: %s does not match in UpdateWaterInfoRendering: not installed", why); return false; }
    g_objArray = FindObjectArrayFromDtor();
    if (!g_objArray) { Log("[waterinfo] WaterInfoSkip: GUObjectArray not found: not installed"); return false; }
    (void)iniPath;
    g_installed = true;
    SetWaterInfoSkip(key);
    Log("[waterinfo] WaterInfoSkip installed: UpdateWaterInfoRendering exe+0x%llX, %zu layout reads checked; mode %d (live)",
        static_cast<unsigned long long>(gtFn - Game().base), sizeof kPins / sizeof kPins[0] + 2, g_mode.load());
    return true;
}

bool WaterInfoSkipInstalled() { return g_installed; }

// Monitor thread, every second (live once installed). The game thread notices a change at its next call and forgets every zone.
void SetWaterInfoSkip(int mode)
{
    if (!g_installed) return;
    mode = mode != 0 ? 1 : 0;
    if (g_mode.exchange(mode) != mode) Log("[waterinfo] WaterInfoSkip=%d", mode);
}

// Game thread, before UpdateWaterInfoRendering(scene, ctx): true = do not call it (mode 1, same inputs as the last redraw).
bool WaterInfoSkipGt(const uint8_t* ctx)
{
    if (!g_installed) return false;
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode != g_lastMode) {  // WI-M1: nothing is recorded while off, so no record may outlive a change of value
        for (auto& z : g_zones) z.have = false;
        g_lastMode = mode;
        g_forgets.fetch_add(1, std::memory_order_relaxed);
    }
    if (!mode || !ctx) return false;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    Fingerprint fp;
    std::vector<std::pair<uint64_t, std::array<double, 4>>> xy;
    const bool ok = MakeFingerprint(ctx, &fp, &xy);
    const void* zone = Rd<const void*>(ctx + kCtxZone);
    int32_t zi = -1, zs = 0;
    if (!Get(static_cast<const uint8_t*>(zone) + 0xC, &zi)) zi = -1;  // UObjectBase::InternalIndex
    if (zi >= 0 && g_objArray) {
        uint8_t* const* chunks = nullptr;
        uint8_t* chunk = nullptr;
        int32_t num = 0;
        if (Get(g_objArray + kArrayChunks, &chunks) && Get(g_objArray + kArrayNum, &num) && chunks && zi < num &&
            Get(reinterpret_cast<const uint8_t*>(chunks + (zi >> 16)), &chunk) && chunk)
            Get(chunk + 24 * (zi & 0xFFFF) + 16, &zs);
    }
    ZoneState* z = nullptr;
    for (auto& s : g_zones) if (s.zone == zone) z = &s;
    if (!z) {
        if (g_zones.size() >= kMaxZones) {
            auto oldest = g_zones.begin();
            for (auto it = g_zones.begin(); it != g_zones.end(); ++it) if (it->seenMs < oldest->seenMs) oldest = it;
            g_zones.erase(oldest);
        }
        g_zones.push_back({});
        z = &g_zones.back();
        z->zone = zone;
    }
    if (z->zoneIndex != zi || z->zoneSerial != zs) { z->have = false; z->zoneIndex = zi; z->zoneSerial = zs; }  // a new zone object
    const uint64_t now = GetTickCount64();
    z->seenMs = now;
    const bool same = ok && z->have && fp.bytes == z->last.bytes;
    if (!same) z->changedMs = now;
    const bool candidate = same && now - z->changedMs >= g_settleMs;
    if (candidate) g_candidates.fetch_add(1, std::memory_order_relaxed);
    if (mode == 1 && candidate) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (ok) {
        z->last = std::move(fp);
        z->groundXY = std::move(xy);
        z->have = true;
    } else {
        z->have = false;
    }
    return false;
}


// Self-test: the game-thread decision with fakes (zone, render target, bodies, owner, meshes, landscape component, GUObjectArray).
int WaterInfoSkipTestLogic()
{
    uint64_t fail = 0;  // one bit per check (more than 32)
    const bool wasInstalled = g_installed;
    const int wasLast = g_lastMode;
    uint8_t* const wasArray = g_objArray;
    const int wasMode = g_mode.load();
    std::vector<ZoneState> wasZones;
    wasZones.swap(g_zones);
    alignas(16) static uint8_t zone[0x400], tex[0x200], res[0x100], body[0x200], owner[0x300], mesh1[0x400], mesh2[0x400],
        comp[0x200], ctx[0x40], array[0x40];
    alignas(16) static uint8_t items[24 * 4];
    static uint8_t* chunk0[1];
    memset(zone, 0, sizeof zone); memset(tex, 0, sizeof tex); memset(body, 0, sizeof body); memset(owner, 0, sizeof owner);
    memset(mesh1, 0, sizeof mesh1); memset(mesh2, 0, sizeof mesh2); memset(comp, 0, sizeof comp); memset(ctx, 0, sizeof ctx);
    memset(array, 0, sizeof array); memset(items, 0, sizeof items);
    chunk0[0] = items;
    uint8_t** chunks = chunk0;
    memcpy(array + kArrayChunks, &chunks, 8);
    const int32_t num = 4;
    memcpy(array + kArrayNum, &num, 4);
    auto setItem = [&](int i, void* obj, int32_t serial) { memcpy(items + 24 * i, &obj, 8); memcpy(items + 24 * i + 16, &serial, 4); };
    setItem(1, zone, 7);
    setItem(2, comp, 9);
    const int32_t zoneIndex = 1;
    memcpy(zone + 0xC, &zoneIndex, 4);
    const float groundZMin = -500.0f;
    memcpy(zone + 0x30C, &groundZMin, 4);
    void* resp = res;
    memcpy(tex + kTexResource, &resp, 8);
    void* ownerp = owner;
    memcpy(body + kBodyOwner, &ownerp, 8);
    void *m1 = mesh1, *m2 = mesh2;
    memcpy(owner + kOwnerInfoMesh, &m1, 8);
    memcpy(owner + kOwnerDilatedMesh, &m2, 8);
    const double bb[7] = {100, 200, 0, 50, 60, 10, 80};
    memcpy(body + kBounds, bb, sizeof bb);
    const double cb[7] = {1000, 2000, 0, 500, 500, 100, 800};
    memcpy(comp + kBounds, cb, sizeof cb);
    static void* bodyList[1];
    bodyList[0] = body;
    static uint64_t groundList[2];
    groundList[0] = 2ull | (9ull << 32);
    groundList[1] = 3ull | (5ull << 32);  // index 3: no object (stale)
    void *zp = zone, *tp = tex, *bl = bodyList, *gl = groundList;
    memcpy(ctx + kCtxZone, &zp, 8);
    memcpy(ctx + kCtxRt, &tp, 8);
    memcpy(ctx + kCtxBodies, &bl, 8);
    const int32_t one = 1, two = 2, bad = -1;
    memcpy(ctx + kCtxBodies + 8, &one, 4);
    memcpy(ctx + kCtxGround, &gl, 8);
    memcpy(ctx + kCtxGround + 8, &one, 4);
    const float capz = 1234.0f, capz2 = 1235.0f, gz2 = -501.0f;
    memcpy(ctx + kCtxCapZ, &capz, 4);
    g_objArray = array;
    g_installed = true;
    g_mode = 1;
    const uint64_t wasSettle = g_settleMs;
    g_settleMs = 0;  // the cases below run within microseconds; the settle time has its own case at the end
    uint64_t bit = 1;
    auto expect = [&](bool skipped) { if (WaterInfoSkipGt(ctx) != skipped) fail |= bit; bit <<= 1; };
    expect(false);  // 0x1: first redraw of the zone
    expect(true);   // 0x2: same inputs
    expect(true);   // 0x4: again
    memcpy(zone + 0x30C, &gz2, 4);
    expect(false);  // 0x8: GroundZMin changed
    expect(true);   // 0x10: then the same again
    memcpy(ctx + kCtxCapZ, &capz2, 4);
    expect(false);  // 0x20: CaptureZ changed
    double bb2[7];
    memcpy(bb2, bb, sizeof bb);
    bb2[0] += 1;
    memcpy(body + kBounds, bb2, sizeof bb2);
    expect(false);  // 0x40: a body's bounds changed
    const int32_t id = 77;
    memcpy(mesh2 + kComponentId, &id, 4);
    expect(false);  // 0x80: the dilated mesh's component id changed
    memcpy(ctx + kCtxGround + 8, &two, 4);
    expect(false);  // 0x100: a ground component added (a stale weak pointer)
    expect(true);   // 0x200: same again
    double cb2[7];
    memcpy(cb2, cb, sizeof cb);
    cb2[4] += 1;
    memcpy(comp + kBounds, cb2, sizeof cb2);
    expect(false);  // 0x400: a ground component's bounds changed
    setItem(1, zone, 8);  // the zone object re-created at the same address (new serial)
    expect(false);  // 0x800
    void* res2 = owner;
    memcpy(tex + kTexResource, &res2, 8);
    expect(false);  // 0x1000: the render target's resource re-created
    bit <<= 3;
    g_mode = 0;
    expect(false);  // 0x10000: mode 0
    // WI-M1: value 1 records inputs A; at 0 the zone redraws with B (nothing recorded); back at 1 the zone redraws with A: stock draws A
    // over B, so this redraw must not be skipped. Then A again: skipped.
    g_mode = 1;
    expect(false);  // 0x20000: first redraw after the switch (A recorded)
    expect(true);   // 0x40000: A again
    g_mode = 0;
    memcpy(ctx + kCtxCapZ, &capz, 4);  // B
    expect(false);  // 0x80000
    memcpy(ctx + kCtxCapZ, &capz2, 4);  // A
    g_mode = 1;
    expect(false);  // 0x100000: must not be skipped (the texture holds B)
    expect(true);   // 0x200000
    // The zone's location (no local tessellation): its root component moved.
    static uint8_t root[0x300];
    memset(root, 0, sizeof root);
    void* rootp = root;
    memcpy(zone + kZoneRoot, &rootp, 8);
    expect(false);  // 0x400000: a root component appeared
    expect(true);   // 0x800000
    const double loc = 512.0;
    memcpy(root + kRootLocation + 8, &loc, 8);
    expect(false);  // 0x1000000: the zone moved
    g_mode = 1;
    void* nz = nullptr;
    memcpy(ctx + kCtxZone, &nz, 8);
    expect(false);  // 0x2000000: unreadable context
    expect(false);  // 0x4000000
    memcpy(ctx + kCtxZone, &zp, 8);
    memcpy(ctx + kCtxBodies + 8, &bad, 4);
    expect(false);  // 0x8000000: bad body count
    expect(false);  // 0x10000000
    void* noRoot = nullptr;
    memcpy(zone + kZoneRoot, &noRoot, 8);
    // The settle time: inputs that changed less than g_settleMs ago are not skipped even when the same again.
    memcpy(ctx + kCtxBodies + 8, &one, 4);
    g_settleMs = 3600 * 1000;
    expect(false);  // 0x20000000: changed (recorded)
    expect(false);  // 0x40000000: same inputs, but changed just now: made
    g_settleMs = 0;
    expect(true);   // 0x80000000: the same once settled
    g_settleMs = wasSettle;
    g_mode = wasMode;
    g_lastMode = wasLast;
    g_installed = wasInstalled;
    g_objArray = wasArray;
    g_zones.swap(wasZones);
    return fail ? static_cast<int>((fail | (fail >> 32)) & 0x7FFFFFFF) | 1 : 0;  // nonzero = failed (bits folded)
}

// Self-test: every layout read on the mapped exe (fn = UpdateWaterInfoRendering, e.g. WaterInfoGtSite() after the water-capture test's
// install). Returns nullptr or what does not match.
const char* WaterInfoSkipTestPins(const GameModule& game, uint8_t* fn)
{
    const char* why = nullptr;
    return CheckPins(game, fn, &why) ? nullptr : why;
}

void LogWaterInfoSkipStats()
{
    if (!g_installed) return;
    static uint64_t logged = ~0ull;
    const uint64_t calls = g_calls.load();
    if (calls == logged) return;
    logged = calls;
    Log("[waterinfo] WaterInfoSkip mode %d (since start): redraws asked %llu, same inputs %llu, skipped %llu",
        g_mode.load(), static_cast<unsigned long long>(calls), static_cast<unsigned long long>(g_candidates.load()),
        static_cast<unsigned long long>(g_skipped.load()));
}

}  // namespace opf
