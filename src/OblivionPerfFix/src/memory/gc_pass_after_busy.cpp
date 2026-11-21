// GcPassAfterBusy (worker gchold, 2026-11-14): a garbage-collection pass that falls due on the engine's timer while the
// game thread's frame is already busy (a cell being added to or removed from the world, character set-up steps run in the
// frame, a frame already too long for the pass to fit) is put off frame by frame with the engine's own one-frame delay, and runs on the first calm
// frame, or when the bound (GcPassAfterBusy, 500 ms by default) is over, whichever comes first.
//
// The engine side (UE 5.3, UEngine::ConditionalCollectGarbage, exe+0x37D8A20 on Steam 19115871; read from the binary):
// once per frame, after the forced paths, `TimeSinceLastPendingKillPurge += FApp::DeltaTime`, then
// `Interval = GetTimeBetweenGarbageCollectionPasses()` (virtual, the call we take over), then
// `if (bShouldDelayGarbageCollect) bShouldDelayGarbageCollect = false;` (the frame does nothing more for GC)
// `else if (!IsIncrementalPurgePending() && TSL > Interval && Interval > 0) { pass unless async loading blocks it }`
// `else { the purge's time slice }`. UEngine::DelayGarbageCollection() sets that flag; UE 5.4-5.7 keep the same order.
//
// What we do: the 6-byte `call [rax+slot]` of that virtual becomes `call [rip+near slot]` into this file, which calls the
// same virtual through the engine's vtable and returns its value unchanged. In between, only when the pass WOULD run this
// frame (the same tests as the code that follows, in the same order: flag clear, no purge pending, TSL > interval > 0,
// not blocked by async loading), the hold may set the engine's flag. The engine's next instruction then consumes it
// exactly as for its own DelayGarbageCollection(): no pass this frame, and (no purge being pending) no purge slice either,
// since there is none to run. Nothing else of the engine is written; the timer keeps counting.
//
// Never put off: a pass someone forced (ForceGarbageCollection from game or engine code, ShouldForceGarbageCollection,
// World Partition's soft collection at >= 64 pending levels, SetTimeUntilNextGarbageCollection: each writes the timer,
// which our continuity check sees: TSL must equal our last reading plus this frame's DeltaTime in the engine's own
// float/double arithmetic); full purges (another branch: they never reach the call); a frame with a menu, dialogue or
// loading screen up, or whose state is not known; while Windows is short of memory (MemoryRelief's RAM/commit signal);
// when the game itself already set the flag. The purge's per-frame slices are never touched.
//
// The busy signals are the tree's own: AddToWorld calls seen by StreamRegisterGranularity / StreamInBudget's hook, the
// RemoveFromWorld steps seen by StreamOutEndPlaySlice's hook, NpcCreationPacing's steps run in the frame and its frame start
// (the frame's own time so far, see kFrameLimitUs), the menu / loading freeze read through PrepareCollisionBehindLoadingScreen's
// fade hook. A signal whose fix is not installed is simply absent; without the freeze reader nothing is ever delayed.
// The bound (500 ms by default) is about 30 frames at 60 fps; in that time a pass grows by 0.066 ms per ms of cell removal
// (our fit over 56 timer passes of three plays: pass = 6.1 ms + 0.066 x RemoveFromWorld ms since the last pass), well under
// a millisecond even when a whole row of cells leaves meanwhile. The pass resets the timer when it runs, so the next pass
// moves by the same wait (at most the bound); nothing piles up. "Short of memory" is MemoryRelief's RAM/commit state: with
// MemoryRelief=0 there is no memory check (the engine's own low-memory GC timer is off in this game: threshold 0).
#include "common.h"

#include <algorithm>
#include <atomic>

namespace opf {
bool NpcPacingFrameState(uint64_t* elapsedUs, uint64_t* unitUs);  // npcs/npc_pacing.cpp

namespace {

using IntervalFn = float (*)(void* engine, bool playersConnected);
using BoolFn = bool (*)();

// ConditionalCollectGarbage's start: mov r11,rsp; push rsi; sub rsp,imm32 (kEntry, 11 bytes: another hook's entry jump may
// stand there, e.g. the lab's SpikeStages, so it is not part of the search), then from byte 11 (kFunc):
// mov rax,[rcx+LastGCFrame]; mov rsi,rcx; cmp [GFrameCounter],rax; je end; mov rax,[rcx]; (3 saves);
// call [rax+ShouldForceGarbageCollection]; lea r14,[rsi+bFullPurgeTriggered]; mov ebx,eax; lea r15,[rsi+TSL]
constexpr char kEntry[] = "4C 8B DC 56 48 81 EC ?? ?? ?? ??";
constexpr size_t kEntryLen = 11;
constexpr char kFunc[] =
    "48 8B 81 ?? ?? ?? ?? 48 8B F1 48 39 05 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 48 8B 01 "
    "49 89 5B 10 4D 89 73 F0 4D 89 7B E8 FF 90 ?? ?? ?? ?? 4C 8D B6 ?? ?? ?? ?? 8B D8 4C 8D BE ?? ?? ?? ??";
constexpr size_t kFuncFrameCounter = 13, kFuncFull = 47, kFuncTsl = 56;  // offsets from byte 11
// The timer path: TSL += FApp::DeltaTime (double); call [rax+GetTimeBetweenGarbageCollectionPasses];
// cmp byte [rsi+bShouldDelayGarbageCollect],0; movaps xmm6,xmm0; je; mov byte [rsi+flag],0; jmp end;
// call IsIncrementalPurgePending; xorps; test al; jne slice; comiss xmm6,[r15]; jae slice; comiss xmm6,xmm1; jbe slice;
// cmp [GPerformGCWhileAsyncLoading],0; jne; call [IsAsyncLoading]; test al; jne end
constexpr char kSite[] =
    "F3 41 0F 10 07 33 D2 48 8B 06 48 8B CE 0F 5A C0 0F 29 B4 24 ?? ?? ?? ?? F2 0F 58 05 ?? ?? ?? ?? 66 0F 5A C0 "
    "F3 41 0F 11 07 FF 90 ?? ?? ?? ?? 80 BE ?? ?? ?? ?? 00 0F 28 F0 74 ?? C6 86 ?? ?? ?? ?? 00 E9 ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? 0F 57 C9 84 C0 0F 85 ?? ?? ?? ?? 41 0F 2F 37 0F 83 ?? ?? ?? ?? 0F 2F F1 0F 86 ?? ?? ?? ?? "
    "83 3D ?? ?? ?? ?? 00 75 ?? FF 15 ?? ?? ?? ?? 84 C0 0F 85";
constexpr size_t kSiteDelta = 24, kSiteCall = 41, kSiteFlagCmp = 47, kSiteFlagClear = 59, kSitePurge = 71, kSiteAsyncVar = 106,
                 kSiteAsyncFn = 115;
// IsIncrementalPurgePending: two byte flags, nothing else (GObjIncrementalPurgeIsInProgress || GObjPurgeIsRequired)
constexpr char kPurge[] = "80 3D ?? ?? ?? ?? 00 75 ?? 80 3D ?? ?? ?? ?? 00 75 ?? 32 C0 C3 B0 01 C3";

struct Found {
    uint8_t* func;
    uint8_t* site;  // the 6-byte virtual call
    uint8_t* purge;
    uint8_t* deltaTime;
    uint8_t* frameCounter;
    uint8_t* asyncVar;
    uint8_t* asyncFnPtr;
    uint32_t slot, tslOff, flagOff;
};

// What the hook reads (the game's, or the self-test's fakes)
struct Env {
    uint32_t slot = 0, tslOff = 0, flagOff = 0;
    BoolFn purgePending = nullptr;
    const double* deltaTime = nullptr;
    const uint64_t* frameCounter = nullptr;
    const int32_t* asyncVar = nullptr;
    BoolFn* asyncFn = nullptr;
    uint64_t (*now)() = nullptr;                                 // microseconds
    bool (*frame)(uint64_t*, uint64_t*) = nullptr;               // NpcPacingFrameState
    bool (*menu)(int32_t*, int32_t*) = nullptr;                  // ReadMenuFreeze
};
Env g_env;

enum Why { kCalm, kBound, kForced, kMenu, kMemory, kUnknown, kGame, kOff, kWhyN };
const char* const kWhyName[kWhyN] = {"calm frame", "bound", "forced", "menu/loading", "memory short", "frame state unknown",
                                     "game's own delay", "key off"};
enum Busy { kAdd = 1, kRemove = 2, kUnits = 4, kTime = 8 };
// The frame-time rule: the frame is busy when the time it has already taken at the GC call, plus a median timer pass
// (10 ms, our three plays) and the work that still follows the call in the frame (about 2 ms), would reach 25 ms, the level
// at which a capped 60 fps frame shows as a sudden drop: 25 - 10 - 2 = 13 ms. A probe of the time at the call against the
// final frame time can tune it.
constexpr uint64_t kFrameLimitUs = 13000;
// Character steps: busy when the pacing ran at least 1 ms of them in this frame (held ones still waiting do not count).
constexpr uint64_t kUnitWorkUs = 1000;
// A level's AddToWorld call counts only when it did work (a step takes its time slice, 1-5 ms; a call that returns at once,
// e.g. while another level is still being made visible, takes microseconds).
constexpr uint64_t kAddWorkUs = 50;

std::atomic<int> g_boundMs{0};       // 0 = off
std::atomic<bool> g_memShort{false};
bool g_installed = false;
// game thread state
bool g_haveLast = false;
float g_lastTsl = 0.0f;
bool g_holding = false, g_released = false;
uint64_t g_holdStartUs = 0, g_holdFrames = 0;
int g_holdBusy = 0;
std::atomic<uint64_t> g_lastAddFrame{~0ull}, g_lastRemoveFrame{~0ull};

struct Stats {
    std::atomic<uint64_t> due{}, atOnce{}, delayed{}, frames{}, us{}, maxFrames{}, maxUs{};
    std::atomic<uint64_t> why[kWhyN]{};    // a delayed pass: why it ran; else why it was not delayed
    std::atomic<uint64_t> busy[4]{};       // delayed frames by signal (add, remove, units, time)
} s;
int g_passLogs = 0;
constexpr int kMaxPassLogs = 400;

uint64_t QpcUs()
{
    static const double perUs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1e6; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(static_cast<double>(t.QuadPart) / perUs);
}

void AtomicMax(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t c = a.load(std::memory_order_relaxed);
    while (v > c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}

int BusyNow(uint64_t frame)
{
    int b = 0;
    const uint64_t add = g_lastAddFrame.load(std::memory_order_relaxed), rem = g_lastRemoveFrame.load(std::memory_order_relaxed);
    // UE 5.3: UGameEngine::Tick runs UpdateLevelStreaming (AddToWorld / RemoveFromWorld steps) after UWorld::Tick, whose
    // end calls ConditionalCollectGarbage; so a step in the previous frame means a level is being added or removed now and
    // its next step lands in this frame, after the pass (this frame's own value: a blocking flush from inside the tick)
    if (add != ~0ull && add + 1 >= frame) b |= kAdd;
    if (rem != ~0ull && rem + 1 >= frame) b |= kRemove;
    uint64_t elapsed = 0, unitUs = 0;
    if (g_env.frame && g_env.frame(&elapsed, &unitUs)) {
        if (unitUs >= kUnitWorkUs) b |= kUnits;  // character set-up steps that ran in this frame (not the ones still queued)
        if (elapsed >= kFrameLimitUs) b |= kTime;
    }
    return b;
}

void EndHold(Why why, uint64_t nowUs)
{
    if (!g_holding) return;
    const uint64_t us = nowUs > g_holdStartUs ? nowUs - g_holdStartUs : 0;
    s.frames.fetch_add(g_holdFrames, std::memory_order_relaxed);
    s.us.fetch_add(us, std::memory_order_relaxed);
    AtomicMax(s.maxFrames, g_holdFrames);
    AtomicMax(s.maxUs, us);
    s.why[why].fetch_add(1, std::memory_order_relaxed);
    if (g_passLogs < kMaxPassLogs) {
        ++g_passLogs;
        Log("[gcafterbusy] timer pass put off %llu frames (%.1f ms) while busy (%s%s%s%s), then run: %s",
            static_cast<unsigned long long>(g_holdFrames), us / 1000.0, (g_holdBusy & kAdd) ? "cell added " : "",
            (g_holdBusy & kRemove) ? "cell removed " : "", (g_holdBusy & kUnits) ? "character steps " : "",
            (g_holdBusy & kTime) ? "frame already 13 ms at the call" : "", kWhyName[why]);
    }
    g_holding = false;
    g_released = true;  // until the pass has run (the timer starts again), no second hold
}

// After the virtual call: decide. Runs on the game thread inside ConditionalCollectGarbage.
uint8_t* g_lastEngine = nullptr;  // CapSleepSkip's prediction reads the timer through these (game thread)
float g_lastInterval = 0.0f;

void Decide(uint8_t* engine, float interval)
{
    g_lastEngine = engine;
    g_lastInterval = interval;
    const int boundMs = g_boundMs.load(std::memory_order_relaxed);
    const float tsl = *reinterpret_cast<const float*>(engine + g_env.tslOff);
    const double dt = *g_env.deltaTime;
    const bool cont = g_haveLast && tsl == static_cast<float>(static_cast<double>(g_lastTsl) + dt);
    g_lastTsl = tsl;
    g_haveLast = true;
    if (boundMs <= 0) {  // off: only the reading above; nothing is called or written
        if (g_holding) EndHold(kOff, g_env.now());
        g_released = false;
        return;
    }
    uint8_t* const flag = engine + g_env.flagOff;
    // the engine's own tests, in its order (the flag first: a set flag skips everything else)
    const bool flagged = *flag != 0;
    const bool due = !flagged && !g_env.purgePending() && !(interval >= tsl) && interval > 0.0f;
    const uint64_t nowUs = g_env.now();
    if (!due) {
        if (flagged && g_holding) EndHold(kGame, nowUs);  // not ours (we never leave it set): the game delays it now
        else if (!flagged) {
            if (g_holding) EndHold(kForced, nowUs);  // the pass ran another way (a full purge) or the timer was reset
            g_released = false;
        }
        return;
    }
    if (*g_env.asyncVar == 0 && (*g_env.asyncFn)()) return;  // the engine skips the pass while async loading (not ours)
    if (g_released) return;  // put off once already: runs now (TryCollectGarbage may also fail and retry: let it)
    const bool first = !g_holding;
    if (first) s.due.fetch_add(1, std::memory_order_relaxed);
    Why stop = kCalm;
    bool hold = false;
    int32_t named = 0, mode = 0;
    if (!cont) stop = kForced;
    else if (g_memShort.load(std::memory_order_relaxed)) stop = kMemory;
    else if (!g_env.menu || !g_env.menu(&named, &mode)) stop = kUnknown;
    else if (named != 0 || mode != 0) stop = kMenu;
    else {
        const int busy = BusyNow(*g_env.frameCounter);
        if (!busy) stop = kCalm;
        else if (!first && nowUs - g_holdStartUs >= static_cast<uint64_t>(boundMs) * 1000) stop = kBound;
        else {
            hold = true;
            if (first) {
                g_holding = true;
                g_holdStartUs = nowUs;
                g_holdFrames = 0;
                g_holdBusy = 0;
                s.delayed.fetch_add(1, std::memory_order_relaxed);
            }
            ++g_holdFrames;
            g_holdBusy |= busy;
            for (int i = 0; i < 4; ++i)
                if (busy & (1 << i)) s.busy[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (hold) {
        *flag = 1;  // the engine's DelayGarbageCollection(): consumed by its next instruction
        return;
    }
    if (g_holding) EndHold(stop, nowUs);
    else {
        if (stop == kCalm) s.atOnce.fetch_add(1, std::memory_order_relaxed);
        else if (stop != kOff) s.why[stop].fetch_add(1, std::memory_order_relaxed);
        g_released = true;  // decided for this pass: it runs now
    }
}

}  // namespace

// The hook: replaces `call [rax+slot]` (GetTimeBetweenGarbageCollectionPasses) on ConditionalCollectGarbage's timer path.
float GcPassAfterBusyInterval(void* engine, bool playersConnected)
{
    void** const vtable = *static_cast<void***>(engine);
    const float interval = reinterpret_cast<IntervalFn>(vtable[g_env.slot / sizeof(void*)])(engine, playersConnected);
    Decide(static_cast<uint8_t*>(engine), interval);
    // GcPrepAhead (gc_prep_ahead.cpp) shares this hook (two keys cannot both patch the call): its once-per-frame step, after
    // the hold's decision and before the engine's pass test (a no-op unless installed and on)
    if (GcPrepAheadActive()) {
        GcTimerFrame f;
        f.tsl = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(engine) + g_env.tslOff);
        f.interval = interval;
        f.purgePending = g_env.purgePending();
        GcPrepAheadFrame(f);
    }
    return interval;
}

// GcPrepAhead's reads of the timer path's state (the game's, or the self-test's fakes)
// (both fail closed for GcPrepAhead's gates: unknown = loading / busy; review N1)
bool GcTimerAsyncLoading() { return !g_env.asyncFn || !*g_env.asyncFn || (*g_env.asyncFn)(); }
int GcTimerBusy() { return g_env.frameCounter ? BusyNow(*g_env.frameCounter) : 15; }
bool GcPassAfterBusyInstalled() { return g_installed; }

// CapSleepSkip (npc_pacing.cpp), game thread, at the end of a frame before the engine's cap wait: which units the coming frame
// is known to carry. GFrameCounter moves at the end of FEngineLoop::Tick, so it already holds the coming frame's number.
// Bit 2: a RemoveFromWorld unregister step ran in the frame that just ended (the level's removal goes on in the coming
// frame's UpdateLevelStreaming). Bit 1: the timer GC pass runs in the coming frame's ConditionalCollectGarbage: the engine's
// own tests (delay flag clear, no purge pending, timer + at least `elapsedUs` of delta over the last interval, not blocked by
// async loading) and none of our hold's stream signals (a level step in the frame that ended holds it). Our hold's other
// signals (character steps, the frame's time) are not known yet: a pass held by them is counted as a miss by CapSleepSkip.
// FApp::DeltaTime (found by the timer path's `addsd xmm0,[FApp::DeltaTime]`; a plain static double), for CapSleepSkip, which
// writes it once right after UpdateTimeAndHandleMaxTickRate wrote it (game thread). Null when not installed.
double* GcPassAfterBusyDeltaTime() { return g_installed ? const_cast<double*>(g_env.deltaTime) : nullptr; }

int GcPassAfterBusyNextUnits(uint64_t elapsedUs)
{
    if (!g_installed || !g_lastEngine || !g_env.frameCounter) return 0;
    const uint64_t frame = *g_env.frameCounter;
    const uint64_t add = g_lastAddFrame.load(std::memory_order_relaxed), rem = g_lastRemoveFrame.load(std::memory_order_relaxed);
    int u = 0;
    if (rem != ~0ull && rem + 1 >= frame) u |= 2;
    const bool stream = u != 0 || (add != ~0ull && add + 1 >= frame);
    if (stream || !(g_lastInterval > 0.0f)) return u;
    const float tsl = *reinterpret_cast<const float*>(g_lastEngine + g_env.tslOff);
    if (*(g_lastEngine + g_env.flagOff) != 0 || g_env.purgePending()) return u;
    if (!(static_cast<double>(tsl) + static_cast<double>(elapsedUs) / 1e6 > static_cast<double>(g_lastInterval))) return u;
    if (*g_env.asyncVar == 0 && (*g_env.asyncFn)()) return u;
    return u | 1;
}

// Game thread: StreamOutEndPlaySlice's hook in RemoveFromWorld's unregister loop (kind 1; reached only while a level's
// components are being unregistered, i.e. with work) and, through GcPassAfterBusyNoteAdd, an AddToWorld call that did work
void GcPassAfterBusyNoteStream(int kind)
{
    if (!g_env.frameCounter) return;
    (kind ? g_lastRemoveFrame : g_lastAddFrame).store(*g_env.frameCounter, std::memory_order_relaxed);
}
bool GcPassAfterBusyWatching() { return g_env.frameCounter != nullptr; }
// StreamRegisterGranularity's AddToWorld hook: the call's own time in QPC ticks
void GcPassAfterBusyNoteAdd(int64_t qpcTicks)
{
    static const double perUs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1e6; }();
    if (qpcTicks > 0 && static_cast<double>(qpcTicks) / perUs >= static_cast<double>(kAddWorkUs)) GcPassAfterBusyNoteStream(0);
}

namespace {

// The function's first 11 bytes: the game's prologue, or a 5-byte entry jump of another hook (the lab's SpikeStages chains
// its thunk there; its trampoline keeps the original bytes, and we never touch the entry).
bool EntryOk(const uint8_t* func) { return func[0] == 0xE9 || MatchesAt(func, ParsePattern(kEntry)); }

// The UE 5.3 layout of UEngine's GC fields and the virtual's slot, as read from the code (nullptr = as expected)
const char* LayoutWhy(uint32_t full, uint32_t tsl, uint32_t slot, uint32_t flagCmp, uint32_t flagClear)
{
    if (flagCmp != flagClear || flagCmp != full + 1 || tsl + 4 != full || tsl < 0x100 || tsl > 0x4000)
        return "UEngine's GC fields (timer, full purge, delay flag) are not laid out as in UE 5.3";
    if (slot < 0x100 || slot > 0x1000 || slot % 8) return "GetTimeBetweenGarbageCollectionPasses' vtable slot looks wrong";
    return nullptr;
}

// The 6 bytes written at the site: call [rip+rel32] with rel32 = slot - (site + 6); false = out of reach
bool MakeCall(const uint8_t* site, const uint8_t* slot, uint8_t out[6])
{
    const int64_t d = slot - (site + 6);
    if (d != static_cast<int32_t>(d)) return false;
    const int32_t rel = static_cast<int32_t>(d);
    out[0] = 0xFF;
    out[1] = 0x15;
    memcpy(out + 2, &rel, 4);
    return true;
}

bool Discover(const GameModule& game, Found* f, const char** why)
{
    const auto fs = FindAll(game.text, ParsePattern(kFunc));
    const auto ss = FindAll(game.text, ParsePattern(kSite));
    if (fs.size() != 1 || ss.size() != 1) { *why = "ConditionalCollectGarbage's start or timer path not found once"; return false; }
    uint8_t* const func = fs[0] - kEntryLen;  // the body from byte 11; the entry is checked below
    uint8_t* const site = ss[0];
    if (!game.text.Contains(func) || site < func || site - func > 0x400 || PrimaryFunction(game, site) != func) {
        *why = "the timer path is not inside ConditionalCollectGarbage";
        return false;
    }
    if (!EntryOk(func)) { *why = "ConditionalCollectGarbage's entry is neither the game's nor a hook's jump"; return false; }
    uint32_t full, tsl, slot, flagCmp, flagClear;
    memcpy(&full, func + kEntryLen + kFuncFull, 4);
    memcpy(&tsl, func + kEntryLen + kFuncTsl, 4);
    memcpy(&slot, site + kSiteCall + 2, 4);
    memcpy(&flagCmp, site + kSiteFlagCmp + 2, 4);
    memcpy(&flagClear, site + kSiteFlagClear + 2, 4);
    if (const char* bad = LayoutWhy(full, tsl, slot, flagCmp, flagClear)) { *why = bad; return false; }
    uint8_t* const purge = RipTarget(site + kSitePurge, 1, 5);
    if (!game.text.Contains(purge) || !MatchesAt(purge, ParsePattern(kPurge))) {
        *why = "IsIncrementalPurgePending is not the two-flag query";
        return false;
    }
    uint8_t* const delta = RipTarget(site + kSiteDelta, 4, 8);
    uint8_t* const frame = RipTarget(func + kEntryLen + kFuncFrameCounter - 3, 3, 7);
    uint8_t* const asyncVar = RipTarget(site + kSiteAsyncVar, 2, 7);
    uint8_t* const asyncFn = RipTarget(site + kSiteAsyncFn, 2, 6);
    for (uint8_t* p : {delta, frame, asyncVar, asyncFn})
        if (!game.data.Contains(p)) { *why = "a global of the timer path (DeltaTime, frame counter, async loading) is not in .data"; return false; }
    f->func = func;
    f->site = site + kSiteCall;
    f->purge = purge;
    f->deltaTime = delta;
    f->frameCounter = frame;
    f->asyncVar = asyncVar;
    f->asyncFnPtr = asyncFn;
    f->slot = slot;
    f->tslOff = tsl;
    f->flagOff = flagCmp;
    return true;
}

}  // namespace

// sharedUse: GcPrepAhead is on and runs from this hook, so it is installed even with GcPassAfterBusy=0 (then it only reads)
bool InstallGcPassAfterBusy(bool beforeGameCode, int key, bool sharedUse)
{
    if (g_installed) return true;
    if (key == 0 && !sharedUse) {
        Log("[gcafterbusy] GcPassAfterBusy off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[gcafterbusy] GcPassAfterBusy: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    const GameModule& game = Game();
    Found f{};
    const char* why = "";
    if (!Discover(game, &f, &why)) {
        Log("[gcafterbusy] GcPassAfterBusy NOT installed (the game's way): %s", why);
        return false;
    }
    uint8_t* const mem = AllocNearGame(16);
    if (!mem) { Log("[gcafterbusy] GcPassAfterBusy NOT installed (the game's way): no memory near the game's code"); return false; }
    void* const hook = reinterpret_cast<void*>(&GcPassAfterBusyInterval);
    memcpy(mem, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(mem, 16, PAGE_READONLY, &old);
    uint8_t insn[6];
    if (!MakeCall(f.site, mem, insn)) { Log("[gcafterbusy] GcPassAfterBusy NOT installed (the game's way): slot out of reach"); return false; }
    g_env.slot = f.slot;
    g_env.tslOff = f.tslOff;
    g_env.flagOff = f.flagOff;
    g_env.purgePending = reinterpret_cast<BoolFn>(f.purge);
    g_env.deltaTime = reinterpret_cast<const double*>(f.deltaTime);
    g_env.frameCounter = reinterpret_cast<const uint64_t*>(f.frameCounter);
    g_env.asyncVar = reinterpret_cast<const int32_t*>(f.asyncVar);
    g_env.asyncFn = reinterpret_cast<BoolFn*>(f.asyncFnPtr);
    g_env.now = &QpcUs;
    g_env.frame = &NpcPacingFrameState;
    g_env.menu = &ReadMenuFreeze;
    g_boundMs = key == 1 ? 500 : key;
    if (!WriteCode(f.site, std::span<const uint8_t>(insn, 6))) {
        Log("[gcafterbusy] GcPassAfterBusy NOT installed (the game's way): could not patch the call");
        return false;
    }
    if (memcmp(f.site, insn, 6) != 0 || *reinterpret_cast<void* const*>(mem) != hook) {
        Log("[gcafterbusy] GcPassAfterBusy: the patched call does not read back as written (left as is, counted as installed)");
    }
    g_installed = true;
    Log("[gcafterbusy] ConditionalCollectGarbage exe+0x%X (entry: %s), timer call exe+0x%X (vtable +0x%X), UEngine timer +0x%X, "
        "delay flag +0x%X: installed, %s",
        static_cast<unsigned>(f.func - game.base), f.func[0] == 0xE9 ? "another hook's jump" : "the game's",
        static_cast<unsigned>(f.site - game.base), f.slot, f.tslOff, f.flagOff,
        key ? "a due timer pass waits for a calm frame while the game is busy" : "off (GcPassAfterBusy=0)");
    return true;
}

// Monitor thread, every second: the key (0 off, 1 = 500 ms, 100-1000 = the bound in ms) and MemoryRelief's RAM/commit signal
void SetGcPassAfterBusy(int key, bool memoryShort)
{
    const int ms = key <= 0 ? 0 : key == 1 ? 500 : std::clamp(key, 100, 1000);
    g_memShort.store(memoryShort, std::memory_order_relaxed);
    const int was = g_boundMs.exchange(ms);
    if (g_installed && was != ms)
        Log(ms ? "[gcafterbusy] a due timer garbage-collection pass waits up to %d ms for a calm frame while the game is busy"
               : "[gcafterbusy] timer garbage-collection passes run when due, as the game does",
            ms);
}

void LogGcPassAfterBusyStats()
{
    if (!g_installed) return;
    const uint64_t due = s.due.exchange(0);
    if (!due) return;
    uint64_t w[kWhyN], b[4];
    for (int i = 0; i < kWhyN; ++i) w[i] = s.why[i].exchange(0);
    for (int i = 0; i < 4; ++i) b[i] = s.busy[i].exchange(0);
    const uint64_t once = s.atOnce.exchange(0), delayed = s.delayed.exchange(0), frames = s.frames.exchange(0),
                   us = s.us.exchange(0), mf = s.maxFrames.exchange(0), mu = s.maxUs.exchange(0);
    auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    Log("[gcafterbusy] since last report: %llu timer passes due, %llu ran at once on a calm frame, %llu put off (%llu frames, %.1f ms; "
        "longest %llu frames, %.1f ms; busy frames: cell added %llu, removed %llu, character steps %llu, frame >= 13 ms at the call %llu), "
        "released by a calm frame %llu, by the bound %llu; never put off: forced %llu, menu/loading %llu, memory short %llu, "
        "state unknown %llu, game's own delay %llu",
        u(due), u(once), u(delayed), u(frames), us / 1000.0, u(mf), mu / 1000.0, u(b[0]), u(b[1]), u(b[2]), u(b[3]), u(w[kCalm]),
        u(w[kBound]), u(w[kForced]), u(w[kMenu]), u(w[kMemory]), u(w[kUnknown]), u(w[kGame]));
}

// UE's IsIncrementalPurgePending (GObjIncrementalPurgeIsInProgress || GObjPurgeIsRequired: true from a pass's reachability
// analysis until its purge has destroyed and freed every object it found unreachable), for CreateAhead (crash1118). Found as
// GcPassAfterBusy finds it (the call on ConditionalCollectGarbage's timer path, then its two-flag body); once GcPassAfterBusy has
// patched that path, the one it found. Nothing is written. Null: not found.
uint8_t* FindIncrementalPurgePending(const GameModule& game)
{
    if (g_installed) return reinterpret_cast<uint8_t*>(g_env.purgePending);
    Found f{};
    const char* why = "";
    return Discover(game, &f, &why) ? f.purge : nullptr;
}

// ---- self-test ----
bool GcPassAfterBusyTestDiscover(const GameModule& game, uint32_t rva[7], uint32_t off[3])
{
    Found f{};
    const char* why = "";
    if (!Discover(game, &f, &why)) return false;
    uint8_t* const p[7] = {f.func, f.site, f.purge, f.deltaTime, f.frameCounter, f.asyncVar, f.asyncFnPtr};
    for (int i = 0; i < 7; ++i) rva[i] = static_cast<uint32_t>(p[i] - game.base);
    off[0] = f.slot;
    off[1] = f.tslOff;
    off[2] = f.flagOff;
    return true;
}

bool GcPassAfterBusyTestFindOn(const GameModule& game) { Found f{}; const char* why = ""; return Discover(game, &f, &why); }
bool GcPassAfterBusyTestEntry(const uint8_t* entry) { return EntryOk(entry); }
const char* GcPassAfterBusyTestLayout(uint32_t full, uint32_t tsl, uint32_t slot, uint32_t flagCmp, uint32_t flagClear)
{
    return LayoutWhy(full, tsl, slot, flagCmp, flagClear);
}
bool GcPassAfterBusyTestCall(const uint8_t* site, const uint8_t* slot, uint8_t out[6]) { return MakeCall(site, slot, out); }

void GcPassAfterBusyTestSetup(const GcPassAfterBusyFakes& k)
{
    g_env = Env{};
    g_env.slot = k.slot;
    g_env.tslOff = k.tslOff;
    g_env.flagOff = k.flagOff;
    g_env.purgePending = k.purgePending;
    g_env.deltaTime = k.deltaTime;
    g_env.frameCounter = k.frameCounter;
    g_env.asyncVar = k.asyncVar;
    g_env.asyncFn = k.asyncFn;
    g_env.now = k.now;
    g_env.frame = k.frame;
    g_env.menu = k.menu;
    g_haveLast = g_holding = g_released = false;
    g_lastAddFrame = g_lastRemoveFrame = ~0ull;
    g_passLogs = 0;
    g_installed = true;  // the stats line is written (nothing is patched)
    s.due = s.atOnce = s.delayed = s.frames = s.us = s.maxFrames = s.maxUs = 0;
    for (auto& w : s.why) w = 0;
    for (auto& b : s.busy) b = 0;
}

// self-test: back to the state before GcPassAfterBusyTestSetup (nothing installed, no fakes), so that later tests reach the
// mapped exe (FindIncrementalPurgePending; review rev-aheadgc SF1)
void GcPassAfterBusyTestTeardown()
{
    g_env = Env{};
    g_installed = false;
    g_boundMs.store(0);
    g_memShort.store(false);
    g_haveLast = g_holding = g_released = false;
    g_lastAddFrame = g_lastRemoveFrame = ~0ull;
    g_passLogs = 0;
}

void GcPassAfterBusyTestCounts(uint64_t out[16])
{
    out[0] = s.due; out[1] = s.atOnce; out[2] = s.delayed; out[3] = s.frames; out[4] = s.maxFrames;
    for (int i = 0; i < kWhyN; ++i) out[5 + i] = s.why[i];
    out[13] = s.busy[0]; out[14] = s.busy[1]; out[15] = s.busy[3];
}

}  // namespace opf
