// WaterInfoSpread (worker waterspread, 2026-11-03, findings "waterspread", "waterspread2", "ourbugs1107"; public since 1.3.0,
// port130k; review rev130f). [Fixes] WaterInfoSpread = 0: not installed (start-up); 3 (default): spread + merge; then live 0-3.
// [Fixes] WaterInfoSpreadNearM (default -1): zones within this many metres of the player are never deferred (0 = only the zone the
// player stands in; -1 = no exemption). Installed at start-up when its sites and the water zone's layout are confirmed in the
// game's code (patterns, below); a mismatch leaves it off with a log line.
//
// What it spreads: the UE 5.3 Water plugin re-renders a water zone's water-info texture (three scene renderers, ~5 ms of render
// thread each update at 4K) whenever AWaterZone::bNeedsWaterInfoRebuild is set, which World Partition streaming does for every
// level added or removed that holds a landscape proxy or a water body touching the zone. Several zones (or the same zone again a
// few frames later) often ask in the same frame or within a few frames: 2-4 updates in one or two frames were render-thread-bound
// hitches at the stone bridge, the west lakes and the Imperial City Waterfront (findings "bridgelake", "waterspread").
//
// The flow (WaterSubsystem.cpp:196-216, WaterZoneActor.cpp:226-236, 387-532, WaterViewExtension.cpp:32-102): every frame
// UWaterSubsystem::Tick runs AWaterZone::Update() for each zone, inlined: `if (bNeedsWaterInfoRebuild || ForceNextFrames) { ...;
// if (UpdateWaterInfoTexture()) bNeedsWaterInfoRebuild = false; }`. UpdateWaterInfoTexture gathers the zone's CURRENT bodies and
// the landscape components intersecting it, sets WaterHeightExtents / GroundZMin and queues a rendering context in the view
// extension's map (keyed by zone); the next view set-up of the same frame updates the bodies' material instances and enqueues
// the render (WaterInfoSkip, water_info_skip.cpp, decides there whether that render is a same-input repeat). Nothing else reads
// the queued state; WaterHeightExtents / GroundZMin feed only material parameters (WaterBodyComponent.cpp:1741-1744): render
// only, no gameplay.
//
// The hook: the inlined Update's call to UpdateWaterInfoTexture is redirected to WaterSpreadHook. To defer a zone the hook
// returns false WITHOUT calling it: the flag stays set (the stock code clears it only on true) and the zone asks again next
// frame, so every requested update still happens, only later, and when it happens it gathers the inputs of that moment: the
// texture, the height extents and the material parameters are what stock produces for the same inputs. Requests that arrive
// while a zone waits merge into that one update (the flag is a bool), as two requests in one frame already do in stock.
// EQUIVALENT, not identical: a zone's water may show its water info up to max(4 frames, 250 ms) later after a streaming change.
//
// Mode bits: 1 spread = at most WaterInfoSpreadPerFrame (1) zone updates per frame, waiting zones served oldest first; 2 merge =
// a zone that updated less than WaterInfoSpreadMergeMs (250, max 1000) ago waits until that time is up. The bound, both bits on:
// a request goes at the latest in the first frame by which it has waited both WaterInfoSpreadMaxFrames (4) frames since its
// first ask and until MergeMs after the zone's last update, i.e. within max(MaxFrames frames, MergeMs) of its first ask (plus
// the frame in which that time runs out; until 2026-11-07, ourbugs1107, the two added up: our bug in the lab key). Never
// deferred: a zone's first render (no texture yet), zones within NearM of the player, any zone while the player position is
// unknown, forced updates (r.Water.WaterInfo.ForceUpdateWaterInfoNextFrames with the flag clear), and every zone for 1 s after
// a gap of >= 250 ms between two subsystem ticks or a new subsystem (a loading screen, world change or frozen world: everything
// pending goes at once, as in stock). Lab builds: bit 4 = one log line per request and per held update.
//
// Measured: findings "waterspread" (bridge slice: water-bound excess -28 % with NearM -1, peaks unchanged within noise),
// "waterspread2" (Bravil marks: no clear gain); the maintainer plays with it on (default on by the maintainer's decision, 2026-11-08).
// Side effect: in a frame with a water-info update the Niagara GPU sims run inside the first water renderer (each counts as a
// first view family); spreading puts updates into more frames (once per frame either way).
//
// WaterInfoAfterCrossing (worker waterlate, 2026-11-18, S-003; public tree, default 0 until the release). A cell crossing marks
// the same zone again and again: the removal behind the player, then each level added ahead when its last AddToWorld slice
// broadcasts (UE 5.3 AWaterZone::OnLevelChanged -> MarkForRebuild sets the bool; WaterZoneActor.cpp:180-236). At the IC
// flight's crossings the big zone was redrawn three times ~0.25 s apart (removal, add 1, add 2; rtwater, rtpool) and a small
// zone in the removal's own frame. With this key (value = the bound in ms, 0 = off) a request is held while the world is
// adding or removing a level (a stream step within WaterInfoAfterCrossingQuietMs, 150, i.e. the gap between a crossing's
// adds) and its first ask is less than the bound old; then it goes through the spread as usual (its frame bound counts from
// there, so released zones still go one per frame; the merge hold is skipped: it already merged). So the marks of one
// crossing become one redraw per zone, drawn in the calm frames after the crossing with the ground and water of that moment.
// Bound: the key's ms + MaxFrames frames from the first ask. Signals are the tree's own stream hooks (StreamOutEndPlaySlice's
// RemoveFromWorld unregister hook, StreamRegisterGranularity's AddToWorld hook, a call that did >= 50 us of work), the same
// GcPassAfterBusy uses; without the add hook (either key) nothing is held. Never held: what the spread never defers (first render, flush, unknown
// position, near, forced); anything while Altar's loading mode is on or unknown (it keeps the world ticking behind a door or a
// fast travel; its end flushes for 1 s, as a tick gap does: everything pending goes in that second);
// zones with local-only tessellation (their marks follow the camera, not crossings; the flag's offset +0x2E0 is read from
// GetDynamicWaterMeshCenter's code, else nothing is held). Later UE (5.4-5.7, ue5-main): Update and the subsystem tick are unchanged (one update per zone per
// frame on a set flag, no coalescing or budget). Readers of what is held: only the water bodies' materials (texture parameter
// WaterVelocityAndHeight, scalars WaterZMin/Max, GroundZMin; WaterBodyComponent.cpp:1725-1745): render only, no gameplay.
// EQUIVALENT: intermediate textures of a crossing are not drawn; the final one has the same inputs as stock's last redraw.
#include "common.h"
#include "water_zone.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace opf {
namespace {

// The inlined AWaterZone::Update in UWaterSubsystem::Tick: cmp [rdi+flag],cl; jne; test eax,eax; je; ... (ForceNextFrames
// decrement); mov rcx,rdi; call UpdateWaterInfoTexture; test al,al; je; mov byte [rdi+flag],0
constexpr const char* kCall =
    "38 8F ?? ?? ?? ?? 75 04 85 C0 74 23 85 C0 78 0C 83 E8 01 0F 48 C1 89 05 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 84 C0 74 07 "
    "C6 87 ?? ?? ?? ?? 00";
constexpr size_t kCallAt = 31, kFlagA = 2, kFlagB = 42;
// AWaterZone::UpdateWaterInfoTexture's prologue (not unique on its own: checked at the call's target)
constexpr const char* kUwitPro =
    "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 D8 FD FF FF 48 81 EC 00 03 00 00";
constexpr size_t kUwitBounds = 0x179;  // call GetZoneBounds2D
constexpr size_t kUwitRes = 0x446;     // lea r8,[r13+RenderTargetResolution]
constexpr size_t kUwitTex = 0x44D;     // mov rcx,[r13+WaterInfoTexture]
// Its reset of the extents (searched in its first 0x200 bytes, once): mov [r13+WaterHeightExtents],rax;
// mov dword [r13+GroundZMin],7F7FFFFFh (FLT_MAX)
constexpr const char* kUwitExtents = "49 89 85 ?? ?? ?? ?? 41 C7 85 ?? ?? ?? ?? FF FF 7F 7F";
constexpr size_t kUwitExtentsSpan = 0x200;
// AWaterZone::GetZoneBounds2D(FBox2D* out): sub rsp,38h; mov rax,[rcx+RootComponent]; movaps [rsp+20h],xmm6; test; je; movups
constexpr const char* kBoundsPro = "48 83 EC 38 48 8B 81 ?? ?? ?? ?? 0F 29 74 24 20 48 85 C0 74 1A 0F 10 80";
// UWaterSubsystem::Tick (FTickableGameObject slot): mov r11,rsp; push rbp; push r13; lea rbp,[r11-5Fh]; sub rsp,0E8h; cookie
constexpr const char* kTickPro = "4C 8B DC 55 41 55 49 8D 6B A1 48 81 EC E8 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F7 49";

// AWaterZone::GetDynamicWaterMeshCenter: sub rsp,28h; cmp byte [rcx+bEnableLocalOnlyTessellation],0; je; movups xmm0,
// [rcx+LocalTessellationCenter]; ...; ret; mov rax,[rcx+RootComponent]; ... (the root's location). WaterInfoAfterCrossing reads
// the flag's offset from here (+6) and holds nothing unless it is kWaterZoneLocalTessellation.
constexpr const char* kLocalCenter =
    "48 83 EC 28 80 B9 ?? ?? ?? ?? 00 74 ?? 0F 10 81 10 03 00 00 48 8B C2 F2 0F 10 89 20 03 00 00 0F 11 02 F2 0F 11 4A 10 48 83 C4 "
    "28 C3 48 8B 81 A0 01 00 00 48 85 C0 74 ?? 0F 10 80 80 02 00 00 0F 11 04 24 0F 10 80 90 02 00 00";
constexpr size_t kLocalCenterFlag = 6;

using UwitFn = bool (*)(void* zone);
using BoundsFn = void* (*)(const void* zone, double* box);  // FBox2D {Min.X, Min.Y, Max.X, Max.Y, bool bIsValid}
using TickFn = void (*)(void* self, float dt);

UwitFn g_uwit = nullptr;
BoundsFn g_bounds = nullptr;
TickFn g_tick = nullptr;
uint32_t g_flagOff = 0, g_texOff = 0, g_resOff = 0;
uint8_t* g_site = nullptr;
void** g_tickSlot = nullptr;
bool g_installed = false;

std::atomic<int> g_mode{0}, g_perFrame{1}, g_maxFrames{4}, g_mergeMs{250}, g_nearM{-1};
std::atomic<int> g_afterMs{0}, g_quietMs{150};       // WaterInfoAfterCrossing (0 = off) and its quiet window
std::atomic<double> g_lastStreamMs{-1e30};           // game thread: the last level add / remove step seen by the stream hooks
std::atomic<bool> g_stream{false};                   // a stream hook has reported at least once (for the log line)
std::atomic<int> g_localState{0};                    // bEnableLocalOnlyTessellation's offset confirmed: 0 not checked, 1 yes, -1 no
int (*g_testLoading)() = nullptr;                    // self-test: the loading mode

// Test hooks (self-test only)
double (*g_testNow)() = nullptr;
bool (*g_testPos)(double*, double*) = nullptr;

double NowMs()
{
    if (g_testNow) return g_testNow();
    static const int64_t f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return static_cast<int64_t>(x.QuadPart); }();
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<double>(q.QuadPart / f) * 1000.0 + static_cast<double>(q.QuadPart % f) * 1000.0 / static_cast<double>(f);
}

bool PlayerXY(double* x, double* y)
{
    if (g_testPos) return g_testPos(x, y);
    double z = 0;
    if (NpcPacingPlayerPos(x, y, &z)) return true;
    float p[3], fwd[3];
    if (FrameGenLastCamera(p, fwd, 500)) { *x = p[0]; *y = p[1]; return true; }
    return false;
}

struct Zone {
    const uint8_t* zone = nullptr;
    const void* tex = nullptr;   // the texture of its last update (a new zone at a reused address has another / none)
    double lastGoMs = -1e30;     // its last update (ours)
    double pendMs = 0;           // when the current wait began (first frame it asked)
    uint64_t pendFrame = 0;      // the spread's order (oldest first): frame the wait began, or the merge hold ended
    uint64_t askFrame = 0;       // frame the wait began: the frame bound counts from here
    uint64_t seenFrame = 0;      // last frame it asked
    bool pending = false, everGo = false, merging = false;
    bool crossing = false;       // held by WaterInfoAfterCrossing now
    bool crossDone = false;      // this request was held through a crossing and released (no merge hold, frame bound restarted)
};

// Game thread only (UWaterSubsystem::Tick)
struct State {
    std::vector<Zone> zones;
    uint64_t frame = 0;
    double lastTickMs = 0, flushUntilMs = 0;
    const void* subsystem = nullptr;
    int goThisFrame = 0;
    const Zone* prio = nullptr;  // the oldest waiting zone at the frame's start
    int loading = -1, loadingPrev = -1;  // Altar's loading mode this frame / last frame (WaterInfoAfterCrossing on only)
} s;

struct Stats {
    std::atomic<uint64_t> asks{0}, waits{0}, goNow{0}, goHeld{0}, goBound{0}, heldFrames{0},
        maxFrames{0}, heldUs{0}, maxUs{0}, first{0}, nearGo{0}, noPos{0}, flushGo{0}, flushes{0}, forced{0},
        crossHeld{0}, crossQuiet{0}, crossBound{0}, crossMaxUs{0}, streamNotes{0}, loadEnds{0};
} st;

void Max(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t c = a.load(std::memory_order_relaxed);
    while (v > c && !a.compare_exchange_weak(c, v)) {}
}

Zone& Entry(const uint8_t* z, const void* tex)
{
    for (Zone& e : s.zones)
        if (e.zone == z) {
            if (e.everGo && e.tex != tex) e = Zone{};  // another zone at this address (world change)
            e.zone = z;
            return e;
        }
    if (s.zones.capacity() < 64) s.zones.reserve(64);  // never reallocated (s.prio points into it)
    if (s.zones.size() >= 64) {  // drop the long unseen, else start over (every zone then goes as "first" once: harmless)
        std::erase_if(s.zones, [](const Zone& e) { return s.frame > e.seenFrame + 3600; });
        if (s.zones.size() >= 64) s.zones.clear();
        s.prio = nullptr;
    }
    s.zones.push_back(Zone{});
    s.zones.back().zone = z;
    return s.zones.back();
}

double DistM(const uint8_t* z, double px, double py)
{
    alignas(16) double box[6] = {};
    g_bounds(z, box);
    if (!reinterpret_cast<const uint8_t*>(box)[32]) return 0;  // no valid box: treat as near
    const double dx = std::max({box[0] - px, 0.0, px - box[2]});
    const double dy = std::max({box[1] - py, 0.0, py - box[3]});
    return std::sqrt(dx * dx + dy * dy) / 100.0;
}

enum class Why { kGo, kFirst, kFlush, kNoPos, kNear, kBound, kMergeHold, kSlot, kOlder, kCrossing };

Why Decide(Zone& e, const uint8_t* z, int mode, double now, double* distOut)
{
    *distOut = -1;
    if (!*reinterpret_cast<void* const*>(z + g_texOff)) return Why::kFirst;
    if (now < s.flushUntilMs) return Why::kFlush;
    const int after = std::min(1000, std::max(0, g_afterMs.load(std::memory_order_relaxed)));
    if (!(mode & 3) && !after) return Why::kGo;
    double px = 0, py = 0;
    const bool pos = PlayerXY(&px, &py);
    const double d = pos ? DistM(z, px, py) : -1;
    *distOut = d;
    if (!pos) return Why::kNoPos;
    const int nearM = g_nearM.load(std::memory_order_relaxed);
    if (nearM >= 0 && d <= nearM) return Why::kNear;
    // WaterInfoAfterCrossing: held while levels are being added or removed, at most `after` ms from the first ask
    if (after && !e.crossDone) {
        const double quiet = std::min(500, std::max(16, g_quietMs.load(std::memory_order_relaxed)));
        const bool streaming = now - g_lastStreamMs.load(std::memory_order_relaxed) < quiet;
        const bool may = s.loading == 0 && g_localState.load(std::memory_order_relaxed) == 1 && !z[kWaterZoneLocalTessellation];
        if (may && streaming && now - e.pendMs < after) {
            if (!e.crossing) st.crossHeld.fetch_add(1, std::memory_order_relaxed);
            e.crossing = true;
            return Why::kCrossing;
        }
        if (e.crossing) {  // released: by the quiet frames, or by the bound while the crossing goes on
            e.crossing = false;
            e.crossDone = true;
            e.merging = false;     // a merge hold before the crossing is over too
            e.askFrame = s.frame;  // the spread's frame bound counts from here: released zones still go one per frame
            (streaming ? st.crossBound : st.crossQuiet).fetch_add(1, std::memory_order_relaxed);
            Max(st.crossMaxUs, static_cast<uint64_t>((now - e.pendMs) * 1000.0));
        }
    }
    if (!after && e.crossing) {  // switched off live while held: the spread's rules (or at once), its frame bound restarted
        e.crossing = false;
        e.crossDone = true;
        e.askFrame = s.frame;
    }
    if (!(mode & 3)) return Why::kGo;
    if ((mode & 2) && e.everGo && !e.crossDone) {
        const double merge = std::min(1000, std::max(0, g_mergeMs.load(std::memory_order_relaxed)));
        if (now - e.lastGoMs < merge) {
            e.merging = true;
            e.pendFrame = s.frame;  // in the spread's order the zone queues from the end of its hold
            return Why::kMergeHold;
        }
        e.merging = false;
    }
    if (mode & 1) {
        const uint64_t maxF = static_cast<uint64_t>(std::max(0, g_maxFrames.load(std::memory_order_relaxed)));
        // The bound counts from the request's first ask (askFrame), not from the end of a merge hold (ourbugs1107: counted from
        // pendFrame, which the merge hold moves, the hold and the spread added up: 14-16 frames / 245-274 ms in play1107d).
        if (s.frame - e.askFrame >= maxF) return Why::kBound;
        if (s.goThisFrame >= std::max(1, g_perFrame.load(std::memory_order_relaxed))) return Why::kSlot;
        if (s.prio && s.prio != &e && s.prio->pending && !s.prio->crossing && s.prio->seenFrame + 1 >= s.frame) return Why::kOlder;
    }
    return Why::kGo;
}


bool WaterSpreadHook(void* zone)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (!mode && g_afterMs.load(std::memory_order_relaxed) <= 0) return g_uwit(zone);
    const uint8_t* z = static_cast<const uint8_t*>(zone);
    if (!z[g_flagOff]) {  // forced by the console variable only
        st.forced.fetch_add(1, std::memory_order_relaxed);
        return g_uwit(zone);
    }
    const void* tex = *reinterpret_cast<void* const*>(z + g_texOff);
    Zone& e = Entry(z, tex);
    const double now = NowMs();
    if (!e.pending) {
        e.pending = true;
        e.pendMs = now;
        e.pendFrame = e.askFrame = s.frame;
        st.asks.fetch_add(1, std::memory_order_relaxed);
    }
    e.seenFrame = s.frame;
    double dist = -1;
    const Why w = Decide(e, z, mode, now, &dist);
    if (w == Why::kMergeHold || w == Why::kSlot || w == Why::kOlder || w == Why::kCrossing) {
        st.waits.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Go: the stock call, with the inputs of this frame.
    const bool r = g_uwit(zone);
    ++s.goThisFrame;
    const double heldMs = now - e.pendMs;
    const uint64_t frames = s.frame - e.askFrame;
    switch (w) {
    case Why::kFirst: st.first.fetch_add(1, std::memory_order_relaxed); break;
    case Why::kFlush: st.flushGo.fetch_add(1, std::memory_order_relaxed); break;
    case Why::kNoPos: st.noPos.fetch_add(1, std::memory_order_relaxed); break;
    case Why::kNear: st.nearGo.fetch_add(1, std::memory_order_relaxed); break;
    case Why::kBound: st.goBound.fetch_add(1, std::memory_order_relaxed); break;
    default: break;
    }
    if (frames > 0) {
        st.goHeld.fetch_add(1, std::memory_order_relaxed);
        st.heldFrames.fetch_add(frames, std::memory_order_relaxed);
        Max(st.maxFrames, frames);
        const uint64_t us = static_cast<uint64_t>(heldMs * 1000.0);
        st.heldUs.fetch_add(us, std::memory_order_relaxed);
        Max(st.maxUs, us);
    } else {
        st.goNow.fetch_add(1, std::memory_order_relaxed);
    }
    if (r) {  // a failed update keeps the request (and its first ask): it goes again next frame by the bound, as stock retries
        e.pending = false;
        e.merging = false;
        e.crossing = false;
        e.crossDone = false;
        e.everGo = true;
        e.lastGoMs = now;
        e.tex = *reinterpret_cast<void* const*>(z + g_texOff);
    }
    return r;
}

void TickStart(void* self)
{
    const double now = NowMs();
    ++s.frame;
    s.goThisFrame = 0;
    if (self != s.subsystem || (s.lastTickMs > 0 && now - s.lastTickMs >= 250.0)) {
        if (s.subsystem) {
            s.flushUntilMs = now + 1000.0;
            st.flushes.fetch_add(1, std::memory_order_relaxed);
        }
        if (self != s.subsystem) s.zones.clear();
        s.subsystem = self;
    }
    s.lastTickMs = now;
    // WaterInfoAfterCrossing: Altar's loading mode, read once a frame; its end flushes as a tick gap does
    if (g_afterMs.load(std::memory_order_relaxed) > 0) {
        s.loading = g_testLoading ? g_testLoading() : StreamLoadingModeNow();
        if (s.loadingPrev == 1 && s.loading != 1) {
            s.flushUntilMs = now + 1000.0;
            st.flushes.fetch_add(1, std::memory_order_relaxed);
            st.loadEnds.fetch_add(1, std::memory_order_relaxed);
        }
        s.loadingPrev = s.loading;
        // Diagnosis (once): steps are seen (the removal hook) but the loading mode can't be read for a while: nothing is held
        static int unknownTicks = 0;
        if (s.loading == -1 && g_stream.load(std::memory_order_relaxed) && unknownTicks >= 0 && ++unknownTicks >= 600 &&
            !g_testLoading) {
            unknownTicks = -1;
            Log("[waterspread] WaterInfoAfterCrossing: Altar's loading mode can't be read (no AddToWorld hook: StreamRegisterGranularity "
                "and StreamInBudget off, or its console variable not found): nothing is held");
        }
    } else {
        s.loading = s.loadingPrev = -1;
    }
    // The oldest zone that waited last frame (merge holds excluded) is served first this frame.
    s.prio = nullptr;
    for (const Zone& e : s.zones)
        if (e.pending && !e.merging && !e.crossing && e.seenFrame + 1 == s.frame && (!s.prio || e.pendFrame < s.prio->pendFrame)) s.prio = &e;
}

void TickHook(void* self, float dt)
{
    TickStart(self);
    g_tick(self, dt);
}

int Clean(int mode)
{
    return mode < 0 ? 0 : (mode & 3);
}

struct Found {
    uint8_t* site = nullptr;
    uint8_t* uwit = nullptr;
    uint8_t* bounds = nullptr;
    uint8_t* tick = nullptr;
    void** slot = nullptr;
    uint32_t flag = 0, tex = 0, res = 0, extents = 0, groundZ = 0;
};

bool Discover(const GameModule& game, Found* f, const char** why)
{
    const auto calls = FindAll(game.text, ParsePattern(kCall));
    if (calls.size() != 1) { *why = "the zone update's call not found once"; return false; }
    uint8_t* const c = calls[0];
    uint32_t fa, fb;
    memcpy(&fa, c + kFlagA, 4);
    memcpy(&fb, c + kFlagB, 4);
    if (fa != fb || fa > 0x1000) { *why = "the rebuild flag's offset does not match"; return false; }
    uint8_t* const uwit = RipTarget(c + kCallAt, 1, 5);
    if (!game.text.Contains(uwit) || !game.text.Contains(uwit + kUwitTex + 8) || !MatchesAt(uwit, ParsePattern(kUwitPro)) ||
        uwit[kUwitBounds] != 0xE8 || !MatchesAt(uwit + kUwitRes, ParsePattern("4D 8D 85")) ||
        !MatchesAt(uwit + kUwitTex, ParsePattern("49 8B 8D"))) {
        *why = "UpdateWaterInfoTexture's bytes do not match";
        return false;
    }
    uint8_t* const bounds = RipTarget(uwit + kUwitBounds, 1, 5);
    if (!game.text.Contains(bounds) || !MatchesAt(bounds, ParsePattern(kBoundsPro))) {
        *why = "GetZoneBounds2D's bytes do not match";
        return false;
    }
    uint32_t tex, res;
    memcpy(&res, uwit + kUwitRes + 3, 4);
    memcpy(&tex, uwit + kUwitTex + 3, 4);
    // The zone's layout, read from the code, must be the one WaterInfoSkip checks in UpdateWaterInfoRendering (water_zone.h).
    const auto ext = FindAll(Section{uwit, uwit + kUwitExtentsSpan}, ParsePattern(kUwitExtents));
    if (ext.size() != 1) { *why = "UpdateWaterInfoTexture's extents reset not found once"; return false; }
    uint32_t he, gz;
    memcpy(&he, ext[0] + 3, 4);
    memcpy(&gz, ext[0] + 10, 4);
    if (fa != kWaterZoneRebuildFlag || he != kWaterZoneHeightExtents || gz != kWaterZoneGroundZMin || tex != kWaterZoneTexture ||
        res != kWaterZoneResolution) {
        *why = "the water zone's layout differs from the one both water fixes expect";
        return false;
    }
    const auto ticks = FindAll(game.text, ParsePattern(kTickPro));
    if (ticks.size() != 1 || c < ticks[0] || c > ticks[0] + 0x400) { *why = "UWaterSubsystem::Tick not found once before the call"; return false; }
    int n = 0;
    void** slot = nullptr;
    for (uint8_t* p = game.rdata.begin; p + 8 <= game.rdata.end; p += 8)
        if (*reinterpret_cast<uint8_t**>(p) == ticks[0]) { slot = reinterpret_cast<void**>(p); ++n; }
    if (n != 1) { *why = "UWaterSubsystem::Tick has not exactly one vtable slot"; return false; }
    f->site = c + kCallAt;
    f->uwit = uwit;
    f->bounds = bounds;
    f->tick = ticks[0];
    f->slot = slot;
    f->flag = fa;
    f->tex = tex;
    f->res = res;
    f->extents = he;
    f->groundZ = gz;
    return true;
}

bool Install(const GameModule& game, const char** why)
{
    Found f;
    if (!Discover(game, &f, why)) return false;
    uint8_t* const mem = AllocNearGame(64);
    if (!mem) { *why = "no memory near the game's code"; return false; }
    const int64_t d = mem - (f.site + 5);
    if (d != static_cast<int32_t>(d)) { *why = "stub out of reach"; return false; }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]; the hook's address follows
    const void* const hook = reinterpret_cast<void*>(&WaterSpreadHook);
    memcpy(mem, jmpAbs, 6);
    memcpy(mem + 6, &hook, 8);
    DWORD old;
    VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, 64);
    g_uwit = reinterpret_cast<UwitFn>(f.uwit);
    g_bounds = reinterpret_cast<BoundsFn>(f.bounds);
    g_tick = reinterpret_cast<TickFn>(*f.slot);
    if (reinterpret_cast<uint8_t*>(g_tick) != f.tick) { *why = "the tick slot holds another function (another mod?)"; return false; }
    g_flagOff = f.flag;
    g_texOff = f.tex;
    g_resOff = f.res;
    const int32_t rel = static_cast<int32_t>(d);
    if (!WritePointer(f.slot, reinterpret_cast<void*>(&TickHook))) { *why = "could not write the tick slot"; return false; }
    if (!WriteCode(f.site + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel), 4))) {
        WritePointer(f.slot, reinterpret_cast<void*>(g_tick));
        *why = "could not patch the call";
        return false;
    }
    g_site = f.site;
    g_tickSlot = f.slot;
    return true;
}

}  // namespace

namespace {
// bEnableLocalOnlyTessellation's offset as GetDynamicWaterMeshCenter reads it (0 = not found once)
uint32_t FindLocalTessOffset(const GameModule& game)
{
    const auto m = FindAll(game.text, ParsePattern(kLocalCenter));
    if (m.size() != 1) return 0;
    uint32_t off;
    memcpy(&off, m[0] + kLocalCenterFlag, 4);
    return off;
}
// Once, when the hold is first wanted (start-up or a live switch-on): confirms the offset Decide reads in every zone
void CheckLocalTess()
{
    int expected = 0;
    if (!g_localState.compare_exchange_strong(expected, 2)) return;  // checked (or being checked)
    const uint32_t off = FindLocalTessOffset(Game());
    const bool ok = off == kWaterZoneLocalTessellation;
    g_localState = ok ? 1 : -1;
    if (!ok)
        Log("[waterspread] WaterInfoAfterCrossing: the water zone's local-tessellation flag is not where expected (%s): nothing is held",
            off ? "another offset" : "GetDynamicWaterMeshCenter not found once");
}
}  // namespace

// Start-up only (version.dll route: the call is patched before the game's code runs). mode 0 = not installed.
bool InstallWaterInfoSpread(bool beforeGameCode, int mode, int nearM, int perFrame, int maxFrames, int mergeMs)
{
    // Installed when either key wants the call site: WaterInfoSpread, or WaterInfoAfterCrossing (set before this call)
    if (!mode && g_afterMs.load() <= 0) {
        Log("[waterspread] WaterInfoSpread and WaterInfoAfterCrossing off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) { Log("[waterspread] WaterInfoSpread: start-up only (loaded late here), not installed"); return false; }
    const GameModule& game = Game();
    const char* why = "";
    if (!Install(game, &why)) {
        Log("[waterspread] WaterInfoSpread: %s: not installed", why);
        return false;
    }
    g_installed = true;
    if (g_afterMs.load() > 0) CheckLocalTess();
    SetWaterInfoSpread(mode, nearM, perFrame, maxFrames, mergeMs);
    Log("[waterspread] WaterInfoSpread installed: call exe+0x%X -> UpdateWaterInfoTexture exe+0x%X, tick slot exe+0x%X (exe+0x%X), "
        "zone flag +0x%X, texture +0x%X (layout shared with WaterInfoSkip); mode %d, near %d m, after crossing %d ms (live)",
        static_cast<unsigned>(g_site - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(g_uwit) - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(g_tickSlot) - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(g_tick) - game.base), g_flagOff, g_texOff, g_mode.load(), g_nearM.load(),
        g_afterMs.load());
    return true;
}


// WaterInfoAfterCrossing: the bound in ms (0 = off, max 1000) and the quiet window (16-500 ms). Monitor thread, and once before
// InstallWaterInfoSpread (it decides whether the call site is installed when WaterInfoSpread is 0). Live once installed; a
// request held when it is switched off goes through the spread (or at once) at its next ask.
void SetWaterInfoAfterCrossing(int afterMs, int quietMs)
{
    afterMs = std::clamp(afterMs, 0, 1000);
    g_quietMs = std::clamp(quietMs, 16, 500);
    if (afterMs > 0 && g_installed) CheckLocalTess();
    if (g_afterMs.exchange(afterMs) != afterMs && g_installed)
        Log("[waterspread] WaterInfoAfterCrossing=%d ms (quiet %d ms)%s", afterMs, g_quietMs.load(),
            g_stream.load() ? "" : ": no stream hook in place (StreamOutEndPlaySlice / StreamRegisterGranularity off), nothing is held");
}

// Game thread, from the stream hooks: a level is being added to or removed from the world in this frame.
void WaterCrossingNoteStream()
{
    g_stream.store(true, std::memory_order_relaxed);
    g_lastStreamMs.store(NowMs(), std::memory_order_relaxed);
    st.streamNotes.fetch_add(1, std::memory_order_relaxed);
}
// The AddToWorld hook times its calls for us while the hold is on (a call that did >= 50 us of work is a step)
bool WaterCrossingWatching() { return g_installed && g_afterMs.load(std::memory_order_relaxed) > 0; }
void WaterCrossingNoteAdd(int64_t qpcTicks)
{
    static const double perUs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1e6; }();
    if (qpcTicks > 0 && static_cast<double>(qpcTicks) / perUs >= 50.0) WaterCrossingNoteStream();
}

// Monitor thread (live once installed). A change of value takes effect at the zone's next ask; requests left waiting when it is
// switched off go at once by the stock path (mode 0 calls straight through).
void SetWaterInfoSpread(int mode, int nearM, int perFrame, int maxFrames, int mergeMs)
{
    if (!g_installed) return;
    mode = Clean(mode);
    g_perFrame = std::clamp(perFrame, 1, 8);
    g_maxFrames = std::clamp(maxFrames, 0, 60);
    g_mergeMs = std::clamp(mergeMs, 0, 1000);
    const int oldNear = g_nearM.exchange(nearM);
    if (g_mode.exchange(mode) != mode || oldNear != nearM)
        Log("[waterspread] WaterInfoSpread=%d (spread %s, merge %s), near %d m", mode, (mode & 1) ? "on" : "off",
            (mode & 2) ? "on" : "off", nearM);
}

// Stats interval (no line unless something was asked since the last one).
void LogWaterInfoSpreadStats()
{
    if (!g_installed) return;
    static uint64_t logged = ~0ull;
    const uint64_t asks = st.asks.load();
    if (asks == logged) return;
    logged = asks;
    const uint64_t held = st.goHeld.load(), heldUs = st.heldUs.load();
    Log("[waterspread] WaterInfoSpread mode %d (since start): requests %llu, waits %llu; updates at once %llu, after a wait %llu "
        "(mean %.2f frames %.1f ms, max %llu frames %.1f ms; by the frame bound %llu); not deferred: first %llu, near %llu, "
        "no position %llu, flush %llu (%llu flushes), forced %llu",
        g_mode.load(), static_cast<unsigned long long>(asks), static_cast<unsigned long long>(st.waits.load()),
        static_cast<unsigned long long>(st.goNow.load()), static_cast<unsigned long long>(held),
        held ? double(st.heldFrames.load()) / double(held) : 0.0, held ? double(heldUs) / 1000.0 / double(held) : 0.0,
        static_cast<unsigned long long>(st.maxFrames.load()), double(st.maxUs.load()) / 1000.0,
        static_cast<unsigned long long>(st.goBound.load()), static_cast<unsigned long long>(st.first.load()),
        static_cast<unsigned long long>(st.nearGo.load()), static_cast<unsigned long long>(st.noPos.load()),
        static_cast<unsigned long long>(st.flushGo.load()), static_cast<unsigned long long>(st.flushes.load()),
        static_cast<unsigned long long>(st.forced.load()));
    if (g_afterMs.load() > 0 || st.crossHeld.load())
        Log("[waterspread] WaterInfoAfterCrossing %d ms: requests held through a crossing %llu, released after the quiet frames %llu, "
            "by the bound %llu, max %.1f ms from the first ask; stream steps seen %llu; loading-mode ends flushed %llu",
            g_afterMs.load(), static_cast<unsigned long long>(st.crossHeld.load()),
            static_cast<unsigned long long>(st.crossQuiet.load()), static_cast<unsigned long long>(st.crossBound.load()),
            double(st.crossMaxUs.load()) / 1000.0, static_cast<unsigned long long>(st.streamNotes.load()),
            static_cast<unsigned long long>(st.loadEnds.load()));
}

// ---- self-test ----
int WaterSpreadTestLocalTess(const GameModule& game) { return static_cast<int>(FindLocalTessOffset(game)); }
int WaterSpreadTestDiscover(const GameModule& game, uint32_t* flag, uint32_t* tex, uint32_t* extents, uint32_t* groundZ)
{
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) return 0;
    *flag = f.flag;
    *tex = f.tex;
    *extents = f.extents;
    *groundZ = f.groundZ;
    return 1;
}

namespace {
double t_now = 0;
double TestNow() { return t_now; }
double t_px = 0, t_py = 0;
bool t_posOk = true;
bool TestPos(double* x, double* y) { *x = t_px; *y = t_py; return t_posOk; }
int t_calls = 0;
const void* t_last = nullptr;
bool t_fail = false;  // the next update fails (returns false, as UpdateWaterInfoTexture can)
int t_loading = 0;    // Altar's loading mode as the hold sees it
int TestLoading() { return t_loading; }
int t_crossBits = 0;  // failed checks of cases 21-26 (WaterInfoAfterCrossing review S1-S3, N1)
bool TestUwit(void* zone)
{
    ++t_calls;
    t_last = zone;
    if (t_fail) return false;
    static int texture;
    *reinterpret_cast<void**>(static_cast<uint8_t*>(zone) + g_texOff) = &texture;
    return true;
}
// fake zone: box from 4 doubles at +0x100
void* TestBounds(const void* zone, double* box)
{
    memcpy(box, static_cast<const uint8_t*>(zone) + 0x100, 32);
    reinterpret_cast<uint8_t*>(box)[32] = 1;
    return box;
}
void TestTick(void*, float) {}
}  // namespace

// Runs the hook as the inlined Update does (flag cleared when it returns true). 0 = all checks passed, else failed bits.
int WaterSpreadTestLogic()
{
    int fail = 0;
    const UwitFn su = g_uwit;
    const BoundsFn sb = g_bounds;
    const TickFn stk = g_tick;
    const uint32_t sf = g_flagOff, stx = g_texOff, sr = g_resOff;
    const int sMode = g_mode, sPer = g_perFrame, sMax = g_maxFrames, sMerge = g_mergeMs, sNear = g_nearM;
    const int sAfter = g_afterMs, sQuiet = g_quietMs;
    const double sLastStream = g_lastStreamMs;
    const int sLocal = g_localState;
    g_afterMs = 0;
    g_localState = 1;  // the offset as confirmed in the game's code (checked on the mapped exe by the wrapper)
    g_testLoading = TestLoading;
    t_loading = 0;
    t_crossBits = 0;
    g_uwit = TestUwit;
    g_bounds = TestBounds;
    g_tick = TestTick;
    g_flagOff = kWaterZoneRebuildFlag;
    g_texOff = kWaterZoneTexture;
    g_resOff = kWaterZoneResolution;
    g_testNow = TestNow;
    g_testPos = TestPos;
    s = State{};
    constexpr uint32_t F = kWaterZoneRebuildFlag, T = kWaterZoneTexture;
    alignas(16) static uint8_t zones[4][0x400];
    memset(zones, 0, sizeof zones);
    static int oldTex;
    for (int i = 0; i < 4; ++i) {
        double* b = reinterpret_cast<double*>(zones[i] + 0x100);
        b[0] = 100000.0 * (i + 1); b[1] = 0; b[2] = b[0] + 50000; b[3] = 50000;  // far from (0,0) except zone 3 below
    }
    {
        double* b = reinterpret_cast<double*>(zones[3] + 0x100);
        b[0] = -1000; b[1] = -1000; b[2] = 1000; b[3] = 1000;  // the player stands in zone 3
    }
    auto frame = [&](int n, const bool ask[4]) {  // one subsystem tick at 16.7 ms steps; returns zones updated (bits)
        int went = 0;
        for (int k = 0; k < n; ++k) {
            t_now += 16.7;
            TickHook(&zones, 0.0167f);
            for (int i = 0; i < 4; ++i) {
                if (!ask[i] && !zones[i][F]) continue;
                if (ask[i]) zones[i][F] = 1;
                const int c = t_calls;
                if (WaterSpreadHook(zones[i])) zones[i][F] = 0;
                if (t_calls != c) went |= 1 << i;
            }
        }
        return went;
    };
    const bool none[4] = {false, false, false, false};
    auto settle = [&] { t_now += 2000; frame(70, none); };  // past the 1 s flush a time jump opens
    const bool all[4] = {true, true, true, true};
    t_now = 1000;
    g_mode = 1; g_perFrame = 1; g_maxFrames = 4; g_mergeMs = 250; g_nearM = 0;
    // 1. first renders (no texture) are never deferred
    if (frame(1, all) != 0xF) fail |= 1;
    // give them old textures and a past update so they are not "first" any more
    for (int i = 0; i < 4; ++i) *reinterpret_cast<void**>(zones[i] + T) = &oldTex;
    s.zones.clear();
    settle();
    // 2. spread: three far zones and the near one ask in one frame -> near + one far now, then one per frame, oldest first
    int w1 = frame(1, all);
    if (w1 != (0x8 | 0x1)) fail |= 2;
    int w2 = frame(1, none), w3 = frame(1, none), w4 = frame(1, none);
    if (w2 != 0x2 || w3 != 0x4 || w4 != 0) fail |= 4;
    for (int i = 0; i < 4; ++i) if (zones[i][F]) fail |= 8;
    // 3. the frame bound: per frame 1, max 1 frame -> a backlog of three goes 1, then 2 at once
    g_maxFrames = 1;
    settle();
    const bool far3[4] = {true, true, true, false};
    int b1 = frame(1, far3), b2 = frame(1, none);
    if (b1 != 0x1 || b2 != 0x6) fail |= 0x10;
    g_maxFrames = 4;
    // 4. merge: a zone asks again 50 ms after its update -> held until 250 ms after it, then goes
    g_mode = 3;
    settle();
    const bool z0[4] = {true, false, false, false};
    if (frame(1, z0) != 0x1) fail |= 0x20;
    t_now += 30;
    int m = frame(1, z0);  // ~47 ms after
    for (int k = 0; k < 10 && !m; ++k) m = frame(1, none);
    if (m) fail |= 0x40;  // 11 frames = ~230 ms: still held
    int k2 = 0;
    for (; k2 < 5 && !m; ++k2) m = frame(1, none);
    if (m != 0x1 || zones[0][F]) fail |= 0x80;
    // 5. near: with NearM -1 the near zone waits too; with 0 it goes at once
    g_mode = 1; g_nearM = -1;
    settle();
    const bool z03[4] = {true, false, false, true};
    if (frame(1, z03) != 0x1) fail |= 0x100;  // zone 0 first (order), 3 waits
    if (frame(1, none) != 0x8) fail |= 0x200;
    g_nearM = 0;
    // 6. unknown position: nothing deferred
    t_posOk = false;
    settle();
    if (frame(1, far3) != 0x7) fail |= 0x400;
    t_posOk = true;
    // 7. a tick gap of >= 250 ms flushes for 1 s; a new subsystem too
    settle();
    frame(1, none);
    t_now += 400;
    if (frame(1, far3) != 0x7) fail |= 0x800;
    // 8. mode 0: stock (every ask goes)
    g_mode = 0;
    settle();
    if (frame(1, far3) != 0x7) fail |= 0x1000;
    // 9. forced (flag clear): goes
    g_mode = 1;
    settle();
    {
        const int c = t_calls;
        zones[1][F] = 0;
        WaterSpreadHook(zones[1]);
        if (t_calls != c + 1) fail |= 0x2000;
    }
    // 10. merge + spread (mode 3) together never hold a request longer than max(MaxFrames frames, MergeMs) from its first
    // ask (ourbugs1107: the frame bound started only when the merge hold ended). Zone 0 updates, asks again 3 frames later
    // (merge hold), and while it is held five fresh zones ask at once and keep the one slot busy for 4 frames.
    {
        alignas(16) static uint8_t zb[6][0x400];
        memset(zb, 0, sizeof zb);
        for (int i = 0; i < 6; ++i) {
            double* b = reinterpret_cast<double*>(zb[i] + 0x100);
            b[0] = 100000.0 * (i + 1); b[1] = 0; b[2] = b[0] + 50000; b[3] = 50000;  // all far
            *reinterpret_cast<void**>(zb[i] + T) = &oldTex;
        }
        auto frameB = [&](double stepMs, int askMask) {  // one tick; returns the zones updated (bits)
            int went = 0;
            t_now += stepMs;
            TickHook(&zones, static_cast<float>(stepMs / 1000.0));
            for (int i = 0; i < 6; ++i) {
                const bool ask = (askMask >> i) & 1;
                if (!ask && !zb[i][F]) continue;
                if (ask) zb[i][F] = 1;
                const int c = t_calls;
                if (WaterSpreadHook(zb[i])) zb[i][F] = 0;
                if (t_calls != c) went |= 1 << i;
            }
            return went;
        };
        g_mode = 3; g_perFrame = 1; g_maxFrames = 4; g_mergeMs = 250; g_nearM = -1;
        for (const double step : {16.7, 100.0}) {
            s.zones.clear();
            t_now += 2000;
            for (int k = 0; k < 70; ++k) frameB(16.7, 0);  // past the flush a time jump opens
            if (frameB(step, 0x1) != 0x1) fail |= 0x4000;  // zone 0's update (frame 0; now it can merge)
            const double goneAt = t_now;
            const int askAt = step < 50 ? 3 : 1;  // its new request: 50 ms / 100 ms after the update (a merge hold)
            int mergeEnd = 1;                     // the first frame at least 250 ms after the update
            while (mergeEnd * step < 250.0) ++mergeEnd;
            const int freshAt = std::max(askAt, mergeEnd - 2);  // five fresh zones ask: their wait is older than zone 0's
            double askMs = 0;
            int w = 0, k = 1;
            for (; k <= 60 && !(w & 1); ++k) {
                w = frameB(step, (k == askAt ? 0x1 : 0) | (k == freshAt ? 0x3E : 0));
                if (k == askAt) {
                    askMs = t_now;
                    if (w & 1) fail |= 0x8000;  // held, not at once
                    w = 0;
                }
            }
            const double heldMs = t_now - askMs;
            const double bound = std::max(4 * step, goneAt + 250.0 - askMs + step);  // + the frame in which the 250 ms run out
            if (!(w & 1) || heldMs > bound + 0.01) fail |= step < 50 ? 0x10000 : 0x20000;
            for (int r = 0; r < 10; ++r) frameB(step, 0);  // let the fresh zones finish
            for (int i = 0; i < 6; ++i) if (zb[i][F]) fail |= 0x40000;
        }
        g_nearM = 0;
    }
    // 11. a failed update (UpdateWaterInfoTexture returns false) keeps the flag and is asked again next frame, as stock retries
    g_mode = 3; g_maxFrames = 4; g_nearM = -1;
    settle();
    {
        const bool z1[4] = {false, true, false, false};
        t_fail = true;
        const int a = frame(1, z1);         // goes (slot free), fails: flag stays
        const int b = frame(1, none);       // asked again: the bound counts from the first ask, so it goes within 4 frames
        int c = b;
        for (int k = 0; k < 4 && !(c & 2); ++k) c = frame(1, none);
        t_fail = false;
        if (!(a & 2) || !zones[1][F] || !(c & 2)) fail |= 0x80000;
        for (int k = 0; k < 6 && zones[1][F]; ++k) frame(1, none);
        if (zones[1][F]) fail |= 0x80000;
    }
    // 13-20. WaterInfoAfterCrossing (waterlate): a request is held while stream steps come (QuietMs 150), at most AfterMs (500)
    // from its first ask; released zones go one per frame (spread), within AfterMs + MaxFrames frames.
    {
        const bool c01[4] = {true, true, false, false};
        const bool c0[4] = {true, false, false, false};
        const bool c012[4] = {true, true, true, false};
        auto firstGo = [&](int notesFrames, int maxFrames, const bool ask[4], int* goAt) {  // frame index of each zone's go
            for (int i = 0; i < 4; ++i) goAt[i] = -1;
            for (int k = 0; k < maxFrames; ++k) {
                if (k < notesFrames) WaterCrossingNoteStream();
                const int w = frame(1, k == 0 ? ask : none);
                for (int i = 0; i < 4; ++i) if ((w >> i & 1) && goAt[i] < 0) goAt[i] = k;
            }
        };
        int at[4];
        g_mode = 3; g_perFrame = 1; g_maxFrames = 4; g_mergeMs = 250; g_nearM = -1; g_afterMs = 500; g_quietMs = 150;
        // 13. the crossing ends after 6 frames of steps: both held through them, then zone 0 once the 150 ms are quiet, zone 1 next
        settle();
        g_lastStreamMs = -1e30;
        firstGo(6, 40, c01, at);
        // last note before frame 5 (t5 - 16.7); quiet when now - last >= 150: frame 5 + 9 - 1 = 13 at 16.7 ms steps
        if (at[0] < 13 || at[0] > 14 || at[1] != at[0] + 1 || zones[0][F] || zones[1][F]) fail |= 0x200000;
        // 14. steps never stop: held until the bound (500 ms = frame 30 from the first ask), then it goes
        settle();
        firstGo(60, 60, c0, at);
        if (at[0] < 29 || at[0] > 31 || zones[0][F]) fail |= 0x400000;
        // 15. three zones at the bound: one per frame, all within AfterMs + MaxFrames frames
        settle();
        firstGo(60, 60, c012, at);
        if (at[0] < 29 || at[1] != at[0] + 1 || at[2] != at[1] + 1 || at[2] > 30 + 4 || zones[2][F]) fail |= 0x800000;
        // 16. off (0): steps do not hold anything
        g_afterMs = 0;
        settle();
        firstGo(10, 3, c0, at);
        if (at[0] != 0) fail |= 0x1000000;
        g_afterMs = 500;
        // 17. never held: the zone the player stands in with NearM 0; a first render (no texture yet)
        g_nearM = 0;
        settle();
        {
            const bool c3[4] = {false, false, false, true};
            firstGo(10, 3, c3, at);
            if (at[3] != 0) fail |= 0x2000000;
            void* const keep = *reinterpret_cast<void**>(zones[2] + T);
            *reinterpret_cast<void**>(zones[2] + T) = nullptr;
            const bool c2[4] = {false, false, true, false};
            firstGo(10, 3, c2, at);
            if (at[2] != 0) fail |= 0x2000000;
            *reinterpret_cast<void**>(zones[2] + T) = keep ? keep : &oldTex;
        }
        g_nearM = -1;
        // 18. WaterInfoSpread 0 with the hold on: held through the steps, then both go together (no spread), as stock would
        g_mode = 0;
        settle();
        firstGo(6, 40, c01, at);
        if (at[0] < 13 || at[0] > 14 || at[1] != at[0] || zones[0][F] || zones[1][F]) fail |= 0x4000000;
        g_mode = 3;
        // 19. no merge hold after a crossing hold: zone 0 updates, steps begin, it asks again 2 frames later; it goes as soon as
        // the steps have been quiet 150 ms (about 200 ms after its update), not at the 250 ms merge time
        settle();
        g_lastStreamMs = -1e30;
        if (frame(1, c0) != 0x1) fail |= 0x8000000;
        WaterCrossingNoteStream();
        frame(1, none);
        WaterCrossingNoteStream();
        int went = frame(1, c0), k = 2;  // its new ask (held)
        for (; k < 40 && !went; ++k) went = frame(1, none);
        // last step stamped at frame 1 (one frame before the ask); quiet from frame 10 = ~167 ms after the update (merge: 250 ms)
        if (went != 0x1 || k > 12) fail |= 0x8000000;
        // 20. no stream step ever seen: nothing is held
        settle();
        g_lastStreamMs = -1e30;
        firstGo(0, 3, c0, at);
        if (at[0] != 0) fail |= 0x10000000;
        // 21. (R1) switched off live while held: goes at its next ask, within MaxFrames frames
        settle();
        {
            int goAt = -1;
            for (int q = 0; q < 20; ++q) {
                WaterCrossingNoteStream();
                if (q == 5) g_afterMs = 0;
                const int w = frame(1, q == 0 ? c0 : none);
                if ((w & 1) && goAt < 0) goAt = q;
            }
            if (goAt < 5 || goAt > 9 || zones[0][F]) t_crossBits |= 0x1;
            g_afterMs = 500;
        }
        // 22. (R1) the update fails at the release: asked again next frames and not held again, although steps come back
        settle();
        {
            int firstCall = -1, doneAt = -1;
            for (int q = 0; q < 40 && doneAt < 0; ++q) {
                if (q < 6 || q >= 14) WaterCrossingNoteStream();
                t_fail = q >= 6 && q < 15;
                const int c = t_calls;
                frame(1, q == 0 ? c0 : none);
                if (t_calls != c && firstCall < 0) firstCall = q;
                if (firstCall >= 0 && !zones[0][F]) doneAt = q;
            }
            t_fail = false;
            if (firstCall < 13 || firstCall > 14 || doneAt < 0 || doneAt > firstCall + 5) t_crossBits |= 0x2;
        }
        // 23. (R1) a tick gap while held flushes: it goes at once although steps go on; a new subsystem too
        settle();
        {
            int goAt = -1;
            for (int q = 0; q < 12; ++q) {
                WaterCrossingNoteStream();
                if (q == 5) t_now += 400;
                const int w = frame(1, q == 0 ? c0 : none);
                if ((w & 1) && goAt < 0) goAt = q;
            }
            if (goAt != 5) t_crossBits |= 0x4;
            settle();
            for (int q = 0; q < 3; ++q) { WaterCrossingNoteStream(); frame(1, q == 0 ? c0 : none); }
            if (!zones[0][F]) t_crossBits |= 0x4;  // held
            static int other;
            WaterCrossingNoteStream();
            t_now += 16.7;
            TickHook(&other, 0.0167f);
            const int c = t_calls;
            if (WaterSpreadHook(zones[0])) zones[0][F] = 0;
            if (t_calls != c + 1 || zones[0][F]) t_crossBits |= 0x4;
        }
        // 24. (S2) Altar's loading mode on or unknown: nothing held; its end flushes (a zone asking then goes at once)
        settle();
        t_loading = 1;
        firstGo(10, 3, c0, at);
        if (at[0] != 0) t_crossBits |= 0x8;
        t_loading = -1;
        settle();
        firstGo(10, 3, c0, at);
        if (at[0] != 0) t_crossBits |= 0x8;
        t_loading = 1;
        settle();
        {
            int goAt = -1;
            for (int q = 0; q < 12; ++q) {
                WaterCrossingNoteStream();
                if (q == 4) t_loading = 0;
                const int w = frame(1, q == 4 ? c0 : none);
                if ((w & 1) && goAt < 0) goAt = q;
            }
            if (goAt != 4) t_crossBits |= 0x8;
        }
        t_loading = 0;
        // 25. (S3) a zone with local-only tessellation is never held; the flag's offset not confirmed: nothing is held
        settle();
        zones[1][kWaterZoneLocalTessellation] = 1;
        {
            const bool c1[4] = {false, true, false, false};
            firstGo(10, 3, c1, at);
            if (at[1] != 0) t_crossBits |= 0x10;
        }
        zones[1][kWaterZoneLocalTessellation] = 0;
        g_localState = -1;
        settle();
        firstGo(10, 3, c0, at);
        if (at[0] != 0) t_crossBits |= 0x10;
        g_localState = 1;
        // 26. (N1) the spread's "older waits" does not wait for a zone that is itself held through a crossing (it became held
        // after the frame's start picked it): a zone already released goes; with that zone not held, it waits as before
        {
            const State keep = s;
            s = State{};
            s.frame = 100;
            s.zones.reserve(64);
            s.zones.push_back(Zone{});
            s.zones.push_back(Zone{});
            Zone& za = s.zones[0];
            Zone& zb = s.zones[1];
            za.zone = zones[0]; za.pending = true; za.crossing = true; za.seenFrame = 100; za.pendFrame = 1;
            zb.zone = zones[2]; zb.pending = true; zb.crossDone = true; zb.askFrame = 100; zb.pendFrame = 2; zb.seenFrame = 100;
            s.prio = &za;
            double dist = 0;
            if (Decide(zb, zones[2], 1, t_now, &dist) != Why::kGo) t_crossBits |= 0x20;
            za.crossing = false;
            if (Decide(zb, zones[2], 1, t_now, &dist) != Why::kOlder) t_crossBits |= 0x20;
            s = keep;
            s.prio = nullptr;  // pointed into the replaced table; the next tick picks it again
        }
        settle();
        if (t_crossBits) fail |= 0x40000000;
        g_afterMs = 0;
    }
    // 12. public values: only the spread and merge bits are kept (lab: also bit 4, the per-request log)
    if (Clean(3) != 3 || Clean(-1) != 0 || (Clean(7) & 3) != 3 || Clean(0) != 0) fail |= 0x100000;
    g_uwit = su; g_bounds = sb; g_tick = stk;
    g_flagOff = sf; g_texOff = stx; g_resOff = sr;
    g_testNow = nullptr; g_testPos = nullptr;
    g_mode = sMode; g_perFrame = sPer; g_maxFrames = sMax; g_mergeMs = sMerge; g_nearM = sNear;
    g_afterMs = sAfter; g_quietMs = sQuiet; g_lastStreamMs = sLastStream;
    g_localState = sLocal;
    g_testLoading = nullptr;
    s = State{};
    return fail;
}

int WaterSpreadTestCrossingBits() { return t_crossBits; }

}  // namespace opf
