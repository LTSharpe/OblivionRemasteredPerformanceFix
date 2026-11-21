// StreamOutEndPlaySlice (worker epicport, 2026-11-12): a streamed-out level's actors get their EndPlay in small batches
// inside RemoveFromWorld's own time limit, over the next frames, instead of all at once in the call that finishes the
// level. The idea and the design are Epic's: Unreal Engine 5.6.0, commit dfeb9ecce7 (CL 41233703),
// `s.LevelStreamingRouteActorEndPlayForRemoveFromWorldGranularity` and `ULevel::RouteActorEndPlayForRemoveFromWorld`
// (Level.cpp / World.cpp). This file is our hook code for the game's UE 5.3 binary; no engine source text is copied.
//
// The problem (our measurements of 2026-11-12): when World Partition streams a cell out
// (UWorld::RemoveFromWorld, incremental), UE 5.3 unregisters the level's components over several frames within
// s.UnregisterComponentsTimeLimit (1 ms), but the call that unregisters the last components then runs, in one go and
// without any limit, AActor::RouteEndPlay(RemovedFromWorld) for EVERY actor of the level, then a scan of the world's pawns,
// ReleaseRenderingResources, ClearLevelComponents and the broadcasts. In this game each actor's EndPlay reaches a game
// component's EndPlay (exe+0x4880A80 on Steam 19115871) that removes the component's bindings from multicast delegates of
// the game mode (AAltarGameModeBase +0x348 / +0x360) and the game instance; each removal scans the whole invocation list
// and compacts it again (TMulticastDelegateBase::RemoveDelegateInstance, exe+0xE01FC0: O(bindings in the world)). In the
// user's 15-minute ride that finishing call was one chunk of 4-14 ms per streamed-out cell (16 slow frames, 203 ms over
// 16.7 ms, 3 of the ride's 9 frames >= 33 ms); 50 of the 50 watchdog samples inside the chunk were under RouteEndPlay.
//
// What it changes (Epic's 5.6 order, made with one call-site hook): RemoveFromWorld's incremental loop
// `do { if (Level->IncrementalUnregisterComponents(n)) { finish; break; } } while (!time limit exceeded)` calls us instead
// of IncrementalUnregisterComponents. While the game's walk still has components to unregister we only pass the call on.
// When it reports "done" we do not report done yet: from then on each call routes RouteEndPlay(RemovedFromWorld) for the
// next StreamOutEndPlaySlice actors of Level->Actors, in array order (Epic's granularity), and reports done only after the
// last one. The game's loop checks its own time limit between our calls (as 5.6 checks it between batches), so the EndPlay
// work takes the level's 1 ms per frame and continues next frame; the level stays CurrentLevelPendingInvisibility and
// bIsBeingRemoved meanwhile, exactly as during the stock unregister frames. When we report done, the game's own finish
// block runs: its RouteEndPlay loop finds every actor already routed (bActorInitialized false: only UninitializeComponents
// runs again, with nothing left to uninitialise; the engine relies on the same idempotence when a level is removed twice),
// then the pawn scan, rendering release, ClearLevelComponents and broadcasts, in the stock order (5.6 also runs them only
// after the last EndPlay batch).
// Unchanged: the order of all calls; which actors get EndPlay and with which reason; no extra work, nothing held back to
// land later (each frame does at most the level's time limit plus one batch). The level is hidden from rendering at the
// first removal call (World Partition levels require full visibility: UE 5.3 BeginRemoval) and its actors stop ticking
// then (the tick manager skips invisible levels), so nothing drawn changes. What differs (the accepted part of Epic's
// design, opt-in in 5.6): the actors of a level that has left the player's range get their EndPlay over a few frames
// instead of in one, and the level is removed from the world's list that many frames later; the next removal waits for
// it (one level is made invisible at a time, as stock). Loading screens: the game's loading mode sets the removal budget
// to 10 s (Altar.Loading.RemoveFromWorldBudget), so the whole routing still finishes in one call there: no extra frames
// behind a loading screen. Blocking removals (FlushLevelStreaming, MAX_int32 calls) go the stock way: a pending batch is
// handed to the game's finish block, which routes the rest.
//
// Key StreamOutEndPlaySlice: 0 = stock; N (1..1000) = actors per batch (Epic's granularity). Installed at start-up when
// != 0 (lab: always), then live. Chained after BodyRemovalSpread's stub on the same call (both in the lab and the public
// build); any other target there, or a site / loop that does not match, installs nothing (log line).
#include "common.h"

#include <atomic>

namespace opf {
const uint8_t* BodyRemovalSpreadUnregStub();  // physics/body_spread.cpp

namespace {

using UnregFn = bool (*)(void* level, int32_t n);
using RouteFn = void (*)(void* actor, uint8_t reason);

// UWorld::RemoveFromWorld's incremental loop (exe+0x38698B4): mov edx,ebx; mov rcx,r13; call
// Level->IncrementalUnregisterComponents; test al,al; jne; lea rcx,[rsp+28h]; call [QueryPerformanceCounter]
// (the same site BodyRemovalSpread patches; the pattern does not include the call's target).
constexpr char kUnregSite[] = "8B D3 49 8B CD E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8D 4C 24 28 FF 15";
constexpr size_t kUnregCall = 5;
// ... its blocking loop (MAX_int32): the same callee, after the incremental site
constexpr char kUnregBlock[] = "BA FF FF FF 7F 49 8B CD E8 ?? ?? ?? ?? 84 C0 74";
constexpr size_t kUnregBlockCall = 8;
// ... and the finish block's EndPlay loop (exe+0x38699A0): mov rax,[r13+Actors.Data]; mov rcx,[rsi+rax]; test rcx,rcx;
// je; mov edx,3 (EEndPlayReason::RemovedFromWorld); call AActor::RouteEndPlay; inc ebx; add rsi,8;
// cmp ebx,[r13+Actors.Num]; jl
constexpr char kEndPlayLoop[] =
    "49 8B 85 ?? ?? 00 00 48 8B 0C 06 48 85 C9 74 ?? BA 03 00 00 00 E8 ?? ?? ?? ?? FF C3 48 83 C6 08 41 3B 9D ?? ?? 00 00 7C";
constexpr size_t kLoopData = 3, kLoopCall = 21, kLoopNum = 35;
constexpr uint8_t kRemovedFromWorld = 3;
constexpr uint32_t kObjIndexOff = 0x0C;  // UObjectBase::InternalIndex (only to tell a new level at the same address)
constexpr double kStaleMs = 500.0;       // a pending level not called for this long: its state is dropped (harmless, see Hook)

struct Found {
    uint8_t* site = nullptr;     // the E8 of the incremental call
    uint8_t* unreg = nullptr;    // ULevel::IncrementalUnregisterComponents
    uint8_t* current = nullptr;  // the call's target now (the game's function or BodyRemovalSpread's stub)
    uint8_t* route = nullptr;    // AActor::RouteEndPlay
    uint32_t dataOff = 0, numOff = 0;
};

UnregFn g_next = nullptr;
RouteFn g_route = nullptr;
uint32_t g_dataOff = 0, g_numOff = 0;
bool g_installed = false;
std::atomic<int> g_gran{0};
double g_qpcMs = 0;

// game thread only (RemoveFromWorld runs there)
struct Phase {
    uint8_t* level = nullptr;
    int32_t index = 0;
    int32_t objIndex = 0;
    int32_t routed = 0;
    int32_t calls = 0;
    double lastMs = 0, startMs = 0, totalMs = 0, maxCallMs = 0, maxActorMs = 0;
} g_ph;

struct Stats {
    std::atomic<uint64_t> levels{0}, actors{0}, calls{0}, totalUs{0}, maxCallUs{0}, maxActorUs{0}, maxLevelUs{0},
        handed{0}, dropped{0}, maxCalls{0};
};
Stats s;

void MaxInto(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

double NowMs()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * g_qpcMs;
}

int32_t Num(const uint8_t* level) { return *reinterpret_cast<const int32_t*>(level + g_numOff); }
void* ActorAt(const uint8_t* level, int32_t i) { return (*reinterpret_cast<void* const* const*>(level + g_dataOff))[i]; }
int32_t ObjIndex(const uint8_t* level) { return *reinterpret_cast<const int32_t*>(level + kObjIndexOff); }

void EndPhase(bool handed)
{
    s.levels.fetch_add(1, std::memory_order_relaxed);
    s.actors.fetch_add(static_cast<uint64_t>(g_ph.routed), std::memory_order_relaxed);
    s.calls.fetch_add(static_cast<uint64_t>(g_ph.calls), std::memory_order_relaxed);
    s.totalUs.fetch_add(static_cast<uint64_t>(g_ph.totalMs * 1000), std::memory_order_relaxed);
    MaxInto(s.maxCallUs, static_cast<uint64_t>(g_ph.maxCallMs * 1000));
    MaxInto(s.maxActorUs, static_cast<uint64_t>(g_ph.maxActorMs * 1000));
    MaxInto(s.maxLevelUs, static_cast<uint64_t>(g_ph.totalMs * 1000));
    MaxInto(s.maxCalls, static_cast<uint64_t>(g_ph.calls));
    if (handed) s.handed.fetch_add(1, std::memory_order_relaxed);
    g_ph = Phase{};
}

// One batch: RouteEndPlay(RemovedFromWorld) for the next `gran` slots of Level->Actors (null slots count, as in Epic's
// loop); true once the index has reached the array's end. Data and count are read for every actor (as the game's loop
// does: an EndPlay may change the array).
bool RouteBatch(uint8_t* level, int gran)
{
    const double t0 = NowMs();
    double last = t0;
    for (int k = 0; k < gran && g_ph.index < Num(level); ++k) {
        void* const actor = ActorAt(level, g_ph.index);
        ++g_ph.index;
        if (!actor) continue;
        g_route(actor, kRemovedFromWorld);
        ++g_ph.routed;
        const double t = NowMs();
        if (t - last > g_ph.maxActorMs) g_ph.maxActorMs = t - last;
        last = t;
    }
    const double dt = last - t0;
    ++g_ph.calls;
    g_ph.totalMs += dt;
    if (dt > g_ph.maxCallMs) g_ph.maxCallMs = dt;
    g_ph.lastMs = last;
    return g_ph.index >= Num(level);
}

}  // namespace

// The hook (RemoveFromWorld's two IncrementalUnregisterComponents loops call it only through the patched incremental
// site; game thread). Public for the self-test.
bool StreamOutEndPlaySliceHook(void* levelArg, int32_t n)
{
    uint8_t* const level = static_cast<uint8_t*>(levelArg);
    GcPassAfterBusyNoteStream(1);  // GcPassAfterBusy: a cell is being removed from the world this frame (no-op unless installed)
    if (WaterCrossingWatching()) WaterCrossingNoteStream();  // WaterInfoAfterCrossing: the same signal
    const int gran = g_gran.load(std::memory_order_relaxed);
    const bool blocking = n == INT32_MAX || n <= 0;
    if (g_ph.level) {
        const double now = NowMs();
        if (g_ph.level != level || ObjIndex(level) != g_ph.objIndex || now - g_ph.lastMs > kStaleMs) {
            // Another level (or the same address reused, or a removal not continued): forget it. Harmless either way: the
            // game's walk below re-reports "done" for a level whose components are gone, and RouteEndPlay of an actor
            // already routed does nothing more (bActorInitialized false); the game's finish routes whatever is left.
            s.dropped.fetch_add(1, std::memory_order_relaxed);
            g_ph = Phase{};
        } else if (blocking || gran <= 0) {
            // A blocking finish, or the key switched off: the game's finish block routes the rest (stock order).
            EndPhase(true);
            return blocking ? g_next(levelArg, n) : true;
        } else {
            if (RouteBatch(level, gran)) {
                EndPhase(false);
                return true;
            }
            return false;
        }
    }
    if (gran <= 0 || blocking) return g_next(levelArg, n);
    if (!g_next(levelArg, n)) return false;
    // The game's walk has unregistered the level's last components (and reset its own index). Stock would now route
    // EndPlay of every actor at once; report "not done" so the game's loop checks its time limit, and route from the
    // next call on.
    if (Num(level) <= 0) return true;
    g_ph.level = level;
    g_ph.objIndex = ObjIndex(level);
    g_ph.startMs = g_ph.lastMs = NowMs();
    return false;
}

namespace {

uint8_t* CallTarget(uint8_t* call) { return call[0] == 0xE8 ? RipTarget(call, 1, 5) : nullptr; }

bool Discover(const GameModule& game, Found* f, const char** why)
{
    const auto us = FindAll(game.text, ParsePattern(kUnregSite));
    const auto ub = FindAll(game.text, ParsePattern(kUnregBlock));
    if (us.size() != 1 || ub.size() != 1) { *why = "RemoveFromWorld's unregister loops not found once"; return false; }
    if (us[0] > ub[0] || ub[0] - us[0] > 0x200) { *why = "RemoveFromWorld's two unregister loops are not where expected"; return false; }
    uint8_t* const site = us[0] + kUnregCall;
    uint8_t* const unreg = CallTarget(ub[0] + kUnregBlockCall);
    uint8_t* const cur = CallTarget(site);
    if (!unreg || !game.text.Contains(unreg)) { *why = "IncrementalUnregisterComponents not found"; return false; }
    if (cur != unreg && (!cur || cur != BodyRemovalSpreadUnregStub())) {
        *why = "the incremental unregister call already goes somewhere else";
        return false;
    }
    const auto lp = FindAll(game.text, ParsePattern(kEndPlayLoop));
    if (lp.size() != 1) { *why = "the finish block's EndPlay loop not found once"; return false; }
    if (lp[0] < ub[0] || lp[0] - ub[0] > 0x100) { *why = "the EndPlay loop is not right after the unregister loops"; return false; }
    uint32_t data, num;
    memcpy(&data, lp[0] + kLoopData, 4);
    memcpy(&num, lp[0] + kLoopNum, 4);
    if (num != data + 8 || data < 0x28 || data > 0x400) { *why = "ULevel::Actors' offsets do not look like a TArray"; return false; }
    uint8_t* const route = CallTarget(lp[0] + kLoopCall);
    if (!route || !game.text.Contains(route) || PrimaryFunction(game, route) != route) {
        *why = "AActor::RouteEndPlay is not a function start";
        return false;
    }
    f->site = site;
    f->unreg = unreg;
    f->current = cur;
    f->route = route;
    f->dataOff = data;
    f->numOff = num;
    return true;
}

}  // namespace

bool InstallStreamOutEndPlaySlice(bool beforeGameCode, int key)
{
    if (g_installed) return true;
    LARGE_INTEGER fr;
    QueryPerformanceFrequency(&fr);
    g_qpcMs = 1000.0 / static_cast<double>(fr.QuadPart);
    if (key == 0) {
        Log("[endplayslice] StreamOutEndPlaySlice off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[endplayslice] StreamOutEndPlaySlice: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    const GameModule& game = Game();
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) {
        Log("[endplayslice] StreamOutEndPlaySlice not installed: %s", why);
        return false;
    }
    uint8_t* const mem = AllocNearGame(16);
    if (!mem) { Log("[endplayslice] StreamOutEndPlaySlice not installed: no memory near the game's code"); return false; }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&StreamOutEndPlaySliceHook);
    memcpy(mem, jmpAbs, sizeof jmpAbs);
    memcpy(mem + sizeof jmpAbs, &hook, sizeof hook);
    const int64_t d = mem - (f.site + 5);
    if (d != static_cast<int32_t>(d)) { Log("[endplayslice] StreamOutEndPlaySlice not installed: stub out of reach"); return false; }
    DWORD old;
    VirtualProtect(mem, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, 16);
    g_next = reinterpret_cast<UnregFn>(f.current);
    g_route = reinterpret_cast<RouteFn>(f.route);
    g_dataOff = f.dataOff;
    g_numOff = f.numOff;
    g_gran = key;
    const int32_t rel = static_cast<int32_t>(d);
    if (!WriteCode(f.site + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel), 4))) {
        Log("[endplayslice] StreamOutEndPlaySlice not installed: could not patch the call");
        return false;
    }
    g_installed = true;
    Log("[endplayslice] RemoveFromWorld's unregister call exe+0x%X (next: %s), AActor::RouteEndPlay exe+0x%X, ULevel::Actors "
        "+0x%X/+0x%X: installed, %d actors per batch (0 = the game's way)",
        static_cast<unsigned>(f.site - game.base), f.current == f.unreg ? "the game's IncrementalUnregisterComponents" : "BodyRemovalSpread",
        static_cast<unsigned>(f.route - game.base), f.dataOff, f.numOff, key);
    return true;
}

void SetStreamOutEndPlaySlice(int key)
{
    if (key < 0) key = 0;
    if (key > 1000) key = 1000;
    if (!g_installed) { g_gran = key; return; }  // not patched: nothing calls the hook (the self-test does)
    const int was = g_gran.exchange(key);
    if (was != key)
        Log("[endplayslice] %s", key ? "a streamed-out level's actors get EndPlay in batches within the removal's time limit"
                                     : "a streamed-out level's actors get EndPlay all at once, as before");
}

void LogStreamOutEndPlaySliceStats()
{
    if (!g_installed) return;
    const uint64_t levels = s.levels.exchange(0), dropped = s.dropped.exchange(0);
    if (!levels && !dropped) return;
    const uint64_t actors = s.actors.exchange(0), calls = s.calls.exchange(0), us = s.totalUs.exchange(0),
                   mc = s.maxCallUs.exchange(0), ma = s.maxActorUs.exchange(0), ml = s.maxLevelUs.exchange(0),
                   handed = s.handed.exchange(0), mcalls = s.maxCalls.exchange(0);
    Log("[endplayslice] since last report: %llu levels, %llu actors' EndPlay in %llu batches (%.1f ms; per level at most "
        "%.1f ms over %llu batches), longest batch %.2f ms, longest single actor %.2f ms; %llu handed to a blocking finish, "
        "%llu dropped",
        static_cast<unsigned long long>(levels), static_cast<unsigned long long>(actors), static_cast<unsigned long long>(calls),
        us / 1000.0, ml / 1000.0, static_cast<unsigned long long>(mcalls), mc / 1000.0, ma / 1000.0,
        static_cast<unsigned long long>(handed), static_cast<unsigned long long>(dropped));
}

// self-test: discovery on the mapped exe (nothing patched) and the hook's logic with fakes
bool StreamOutEndPlaySliceTestDiscover(const GameModule& game, uint32_t rva[4], uint32_t* dataOff, uint32_t* numOff)
{
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) return false;
    rva[0] = static_cast<uint32_t>(f.site - game.base);
    rva[1] = static_cast<uint32_t>(f.unreg - game.base);
    rva[2] = static_cast<uint32_t>(f.route - game.base);
    rva[3] = static_cast<uint32_t>(f.current - game.base);
    *dataOff = f.dataOff;
    *numOff = f.numOff;
    return true;
}

void StreamOutEndPlaySliceTestSetup(void* next, void* route, uint32_t dataOff, uint32_t numOff, int gran)
{
    LARGE_INTEGER fr;
    QueryPerformanceFrequency(&fr);
    g_qpcMs = 1000.0 / static_cast<double>(fr.QuadPart);
    g_next = reinterpret_cast<UnregFn>(next);
    g_route = reinterpret_cast<RouteFn>(route);
    g_dataOff = dataOff;
    g_numOff = numOff;
    g_gran = gran;
    g_ph = Phase{};
}

void StreamOutEndPlaySliceTestAge(double ms) { if (g_ph.level) g_ph.lastMs -= ms; }

}  // namespace opf
