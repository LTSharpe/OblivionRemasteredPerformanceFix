// PsoPrecompileFast (on by default): the game's start-up shader pipeline (PSO) precompile runs at UE's own fast pace
// while the player is not playing (start-up videos, main menu, loading screens, menus), and at the game's pace in play.
//
// After every launch UE 5.3's FShaderPipelineCache replays the game's bundled PSO caches (build 19115871: 15,551 + 2,536
// pipelines): each frame its render-thread tick creates a batch of pipelines, which task-graph workers compile. The batch
// size and its time budget come from the batch mode (FShaderPipelineCache::SetBatchMode: Background, Fast, Precompile).
// The game asks for Fast on a full map load (PreLoadMap, from UEngine::LoadMap) and for Background after every map load
// (PostLoadMapWithWorld, also after seamless travel), with its own Background values (BackgroundBatchSize 5,
// BackgroundBatchTime 1 ms: set by the game itself, an ini cannot change them). The main menu and every new game or save
// load arrive by seamless travel, so the whole precompile runs in Background: from ~10 s to ~110-150 s after launch with
// a warm driver cache, through the main menu, the first load and the first one to two minutes of play, where every door
// and fast-travel loading screen is held 5 s longer (the game's FadeInExtraDurationPSOCompilation while IsPrecompiling:
// measured 5.7-6.7 s instead of 0.7-1.7 s from arrival to the world). In Fast (UE's values: up to 50 per batch, a 16 ms
// budget) the same work finishes in ~8 s (measured 2026-10-16).
//
// The fix detours SetBatchMode: it remembers the mode the game asks for and, while the precompile runs and the player is
// not in play, applies Fast instead of Background. "In play" = the freeze subsystem readable (fade_hook.cpp) with no named
// freeze (every menu, dialogue and the main menu push one) and its mode 0 (no loading freeze); before the subsystem
// exists (start-up) counts as not in play. A game-thread timer (SetTimer without a window, dispatched by UE's message
// pump, as in RestoreFsrAfterAltTab) checks every 250 ms while the precompile runs and hands the game's own mode back as soon as play
// starts. Once play has been seen for 3 s in a row after the first menu (the main menu; the start-up videos before it
// show no menu freeze), a menu opened in play (inventory, map, dialogue, pause) keeps the
// game's pace too (PsoPrecompileFastInGameMenus=1: Fast there as before 1.1.5): the player is still using the game there,
// and after a driver update the precompile runs for minutes, so Fast made those menus lag. Fast and Precompile requests pass unchanged, pausing and resuming are the game's, nothing else is touched. The
// same pipelines are created, only sooner: identical results (timing only). With a cold driver cache (after a driver
// update) the precompile takes longer; it is then fast behind menus and loading screens and at the game's pace in play.
//
// Since 1.1.10 (2026-10-23, worker coldstart) the faster pace is bounded, because with a cold driver shader cache (a new
// install, a driver update) UE's Fast pace made things worse than the stock game: (1) UE's Fast gives the render thread
// 16 ms per frame for dispatching pipelines (its batch size grows while a batch takes less), so main-menu frames ran at
// 30+ ms even with a warm cache (p99 26-32 ms, up to 172 ms; players reported 700 -> 15 fps); now, while a menu is up (the
// main menu, start-up), the render thread gets PsoPrecompileFastBudgetMs (default 4 ms) per frame and the batch starts small
// (measured: p99 6 ms, the warm precompile done ~40 s after launch instead of ~18 s; stock ~120 s). Loading screens, where
// nobody interacts, keep UE's Fast values (as the game itself uses for its own full map loads). (2) With a cold cache the driver compiles
// every pipeline for real, the dispatched work queues up in the driver and the fetched pipelines pile up in UE (its
// TotalActiveTasks: 9,000-12,000 instead of the warm runs' 20-200), and every loading screen waited behind that queue
// (measured: a door 52 s instead of 6 s, a 30 s freeze, the menu with 100+ ms frames). Once the fetched-but-not-dispatched
// queue reaches PsoPrecompileColdQueue (default 1000), or the driver's own compile threads are busy (D3D12's "D3D
// Background Thread"s, which the AMD driver compiles on: 100-400 % of a core with a cold cache, 0 % with a warm one; at
// least half a core for 2 s in a row, read once a second on the monitor thread), or the faster pace has run for 60 s in
// this precompile without finishing, the fix hands the game its own pace for the rest of that precompile: a cold start
// runs as in stock. The driver's threads are searched for once per precompile (a system-wide thread snapshot, ~50 ms of
// the monitor thread on a busy desktop), again only if one of them exits; with none found (likely NVIDIA and Intel) the
// search is retried every 10 s and given up after three, and the log says once which it was. PsoPrecompileColdQueue=0
// turns the whole guard off (no queue, driver-thread or 60 s limit: the pace before 1.1.10, with the budget at 16).
//
// Found by byte pattern: SetBatchMode (its entry, the singleton it checks, its three cases, each storing the batch size
// into the same static) and IsPrecompiling (the same singleton, the flag's offset). The entry's first two instructions
// (sub rsp,28h; cmp qword [singleton],0) move to a trampoline with the comparison re-targeted; a 5-byte jump to a stub
// replaces them (one atomic 8-byte write).
#include "common.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace opf {
namespace {

// FShaderPipelineCache::SetBatchMode(BatchMode) (exe+0x2702390): sub rsp,28h / cmp qword [ShaderPipelineCache],0 / je /
// mov [rsp+30h],rbx / mov [rsp+20h],rdi / test ecx,ecx / je Background / sub ecx,1 / je Fast / cmp ecx,1 /
// jne Background / (Precompile:) mov rdi,[cvar] / ... / mov [BatchSize],eax
constexpr const char* kSetBatchMode =
    "48 83 EC 28 48 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 48 89 5C 24 30 48 89 7C 24 20 85 C9 0F 84 ?? ?? ?? ?? "
    "83 E9 01 74 ?? 83 F9 01 0F 85 ?? ?? ?? ?? 48 8B 3D ?? ?? ?? ?? E8 ?? ?? ?? ?? BB 04 00 00 00 84 C0 75 ?? "
    "E8 ?? ?? ?? ?? 84 C0 75 ?? 8B C3 EB 02 33 C0 8B 04 07 48 8B 3D ?? ?? ?? ?? 89 05 ?? ?? ?? ??";
constexpr size_t kSingletonCmp = 0x04;   // cmp qword [rip+disp32], 0 (8 bytes)
constexpr size_t kEntryLen = 0x0C;       // sub rsp,28h (4) + the cmp (8): moved to the trampoline
constexpr size_t kBackgroundJe = 0x1E;   // je rel32 (mode 0)
constexpr size_t kFastJe = 0x27;         // je rel8 (mode 1, after sub ecx,1)
constexpr size_t kPrecompileJne = 0x2C;  // jne rel32 (anything but 2 -> Background)
constexpr size_t kPrecompileBlock = 0x32;
constexpr size_t kBlockStore = 0x2E;     // each case's mov [BatchSize],eax, from the case's start
constexpr uint8_t kEntry[4] = {0x48, 0x83, 0xEC, 0x28};
// FShaderPipelineCache::IsPrecompiling (exe+0x26FDB30): mov rax,[ShaderPipelineCache] / test rax,rax / je /
// movzx eax,byte [rax+bHasShaderPipelineTask] / nop / ret / ret
constexpr const char* kIsPrecompiling = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 09 0F B6 80 ?? ?? ?? ?? 90 C3 C3";
constexpr size_t kFlagDisp = 0x0F;
// FShaderPipelineCache::NumPrecompilesRemaining (exe+0x26FDE70), its no-MaxPrecompileTime branch at +0xA8:
// mov rcx,[TotalActiveTasks] / nop / mov rax,[TotalWaitingTasks] / nop / test rax,rax / cmovs eax,ebx / test rcx,rcx /
// cmovs ecx,ebx / lea edi,[rcx+rax] / mov rsi,[rsp+40h] / movaps xmm6,[rsp+20h] / call GetNumActivePipelinePrecompileTasks
constexpr const char* kRemaining =
    "48 8B 0D ?? ?? ?? ?? 90 48 8B 05 ?? ?? ?? ?? 90 48 85 C0 0F 48 C3 48 85 C9 0F 48 CB 8D 3C 01 48 8B 74 24 40 0F 28 74 24 20 E8";

enum Mode : int { kBackground = 0, kFast = 1, kPrecompile = 2 };
const char* const kModeName[3] = {"Background", "Fast", "Precompile"};
const char* ModeName(int m) { return m >= 0 && m <= 2 ? kModeName[m] : "?"; }

constexpr UINT kTimerMs = 250;
constexpr ULONGLONG kIdleStopMs = 120000;  // no precompile seen this long after the timer started: stop (restarted by the next call)
constexpr int kMaxEventLines = 40;
constexpr uint32_t kGentleStartBatch = 8;     // the bounded pace's first batch (UE's Fast starts at 50 and grows from there)
constexpr ULONGLONG kFastMaxMs = 60000;       // the faster pace at most this long per precompile (a warm start needs ~8-20 s)

using SetBatchModeFn = void (*)(int mode);

uint8_t* g_fn = nullptr;            // SetBatchMode
SetBatchModeFn g_orig = nullptr;    // its trampoline (a fake in the self-test)
void** g_singleton = nullptr;       // static FShaderPipelineCache*
uint32_t g_flagOffset = 0;          // bHasShaderPipelineTask (what IsPrecompiling reads)
uint32_t* g_batchSize = nullptr;    // statics the mode sets (for the log)
float* g_batchTime = nullptr;
bool g_freezeKnown = false;
DWORD g_gameThread = 0;
uint8_t* g_region = nullptr;
volatile int64_t* g_active = nullptr;   // FShaderPipelineCacheTask::TotalActiveTasks: pipelines fetched, not yet dispatched

std::atomic<bool> g_enabled{true};
std::atomic<int> g_requested{-1};   // the game's last request (-1: none seen yet)
std::atomic<int> g_applied{-1};     // what the fix last passed on (with the call, under g_applyLock)
std::atomic<bool> g_ours{false};    // the applied mode is the fix's Fast in place of the game's Background
SRWLOCK g_applyLock = SRWLOCK_INIT; // the game thread's timer and a render-thread request may apply at the same time
std::atomic<int> g_inPlay{-1};      // the game thread's last look: 1 in play, 0 not, 2 a menu after play started, -1 not known yet
std::atomic<bool> g_inGameMenus{false};  // PsoPrecompileFastInGameMenus: Fast in menus after play started too
std::atomic<float> g_budgetMs{4.0f};    // PsoPrecompileFastBudgetMs: render-thread ms per frame at the faster pace (>= 16: UE's Fast as is)
std::atomic<int> g_coldQueue{1000};     // PsoPrecompileColdQueue: fetched-not-dispatched pipelines that mean "the driver compiles"
                                        // (0: no cold-cache guard at all: no queue, driver-thread or 60 s limit)
std::atomic<bool> g_cold{false};        // this precompile goes slowly (cold driver cache): the game's own pace until it ends
std::atomic<bool> g_screenHold{false};  // ShaderScreenAfterDriverChange took the game's first-launch path: until the
                                        // main menu (nobody can interact behind that screen) UE's Fast values, cold or not
std::atomic<bool> g_loadingNow{false};  // the game thread's last look: a loading screen (the freeze subsystem's loading mode)
std::atomic<bool> g_appliedBounded{false};  // the faster pace was applied with the bounded budget (not UE's Fast values)
std::atomic<float> g_appliedBudget{0.0f};   // the bounded budget last applied (0: none; a change re-applies at the next check)
std::atomic<int64_t> g_maxQueue{0};     // the largest queue seen during this precompile
std::atomic<const char*> g_coldWhy{""}; // which guard tripped (monitor or game thread writes, the stats line reads)
uint64_t g_fastMaxMs = kFastMaxMs;      // the faster pace's limit per precompile (the self-test shortens it)
uint64_t g_fastAtStart = 0;             // game thread: g_fastMs when this precompile started (the limit is per precompile)
// the driver's compile threads (monitor thread only)
constexpr int kMaxDriverThreads = 32;
constexpr int kDriverSearches = 3;      // empty searches per precompile before the driver-thread guard gives up
HANDLE g_drvThreads[kMaxDriverThreads] = {};
int g_drvCount = 0;
ULONGLONG g_drvSearchAt = 0, g_drvLastAt = 0;   // the last search in this precompile (0: none yet), the last reading
uint32_t g_drvRetryMs = 10000;          // searches at most this often (the self-test shortens it)
int g_drvSearches = 0;                  // searches in this precompile
int g_drvEmptySearches = 0;
bool g_drvGaveUp = false;               // none found in kDriverSearches searches: no driver-thread guard for this precompile
bool g_drvLoggedFound = false, g_drvLoggedNone = false;  // each said once per session
double g_drvSearchMs = 0.0;             // the last search's duration
uint64_t g_drvLastCpu = 0;
int g_drvBusySeconds = 0;
std::atomic<int> g_drvPeakPct{0};   // the busiest second seen during this precompile (percent of one core)
std::atomic<bool> g_played{false};  // play has been seen for g_playMs in a row since launch (game thread writes)
ULONGLONG g_playSince = 0;          // game thread: start of the current in-play stretch + 1 (0: not in play)
uint32_t g_playMs = 3000;           // the main menu shows no named freeze for ~0.25 s after its arrival: not play
bool g_menuSeen = false;            // game thread: a named freeze was seen (the main menu is the first): the start-up videos
                                    // before it show none and must not count as play

// game thread (timer)
using SetTimerFn = UINT_PTR(WINAPI*)(HWND, UINT_PTR, UINT, TIMERPROC);
using KillTimerFn = BOOL(WINAPI*)(HWND, UINT_PTR);
SetTimerFn g_setTimer = nullptr;
KillTimerFn g_killTimer = nullptr;
UINT_PTR g_timer = 0;
ULONGLONG g_timerStart = 0;
ULONGLONG g_lastCheck = 0;
bool g_faulted = false;
std::atomic<int> g_eventLines{0};   // game and monitor threads

// timeline (ms since the install)
ULONGLONG g_t0 = 0;
std::atomic<uint64_t> g_firstSeenMs{0}, g_doneMs{0}, g_fastMs{0}, g_gameModeMs{0};
bool g_running = false;  // game thread: the precompile ran at the last check
std::atomic<uint64_t> g_calls{0}, g_replaced{0}, g_switchesFast{0}, g_handBacks{0}, g_timerChecks{0}, g_timerTicks{0}, g_coldMs{0};
uint64_t g_logged = 0;

void Event(const char* fmt, ...)
{
    const int line = g_eventLines.fetch_add(1) + 1;
    if (line > kMaxEventLines) return;
    if (line == kMaxEventLines) {
        Log("[pso] (further events only in the statistics)");
        return;
    }
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    Log("%s", buf);
}

ULONGLONG Now() { return GetTickCount64() - g_t0; }

bool Precompiling()
{
    __try {
        const auto* cache = static_cast<const uint8_t*>(*g_singleton);
        return cache && cache[g_flagOffset] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 1 = in play (no named freeze, no loading freeze); 2 = a menu (named freeze, no loading freeze) after play has been seen
// for g_playMs in a row after the first menu (the main menu): inventory, map, dialogue, pause; 0 = anything else (start-up before the freeze
// subsystem exists, the main menu and its menus before the first play, loading screens). Game thread.
// The main menu after quitting a game counts as a menu opened in play (2: the game's pace, as in stock). Deliberately not
// told apart: by then the precompile has normally ended (a warm one ~40 s after launch) or, with a cold driver cache, the
// cold-cache guard has already handed the game its pace.
int InPlayNow(ULONGLONG now)
{
    int32_t named = -1, mode = -1;
    if (!ReadMenuFreeze(&named, &mode)) {
        g_playSince = 0;
        g_loadingNow = false;
        return 0;
    }
    g_loadingNow = mode != 0;
    if (named > 0) g_menuSeen = true;
    if (named == 0 && mode == 0) {
        if (!g_playSince) g_playSince = now + 1;
        if (g_menuSeen && now + 1 - g_playSince >= g_playMs) g_played = true;
        return 1;
    }
    g_playSince = 0;
    return named > 0 && mode == 0 && g_played.load() ? 2 : 0;
}

// What to pass on for the game's request: Fast instead of Background while the precompile runs and nobody plays. A menu
// opened in play keeps the game's pace unless PsoPrecompileFastInGameMenus: there the player is still interacting, and
// with a cold driver cache (after a driver update) the precompile runs for minutes, so Fast made every inventory or
// map opening lag (a player report after NVIDIA 621.01, 2026-10-22).
// The faster pace with the bounded render-thread budget (menus, start-up), or UE's Fast values (loading screens, or the key at 16).
bool ScreenHold() { return g_screenHold.load() && !g_menuSeen; }
bool Bounded() { return g_budgetMs.load() < 16.0f && !g_loadingNow.load() && !ScreenHold(); }

int Wanted(int requested, int inPlay, bool precompiling)
{
    const bool notPlaying = inPlay == 0 || (inPlay == 2 && g_inGameMenus.load(std::memory_order_relaxed));
    if (g_enabled.load(std::memory_order_relaxed) && !g_faulted && precompiling && requested == kBackground && ScreenHold()) return kFast;
    if (g_enabled.load(std::memory_order_relaxed) && !g_faulted && !g_cold.load() && precompiling && requested == kBackground && notPlaying)
        return kFast;
    return requested;
}

void CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD);
void StartTimer()
{
    if (g_timer || !g_setTimer) return;
    g_timer = g_setTimer(nullptr, 0, kTimerMs, &TimerProc);
    g_timerStart = GetTickCount64();
}
void StopTimer()
{
    if (g_timer && g_killTimer) g_killTimer(nullptr, g_timer);
    g_timer = 0;
}

// Passes a mode on to the game's SetBatchMode (every time: the game's own call goes through, re-reading its cvars).
void Apply(int mode, const char* why)
{
    AcquireSRWLockExclusive(&g_applyLock);
    const bool ours = mode != g_requested.load();
    g_applied = mode;
    const bool wasOurs = g_ours.exchange(ours);
    g_orig(mode);
    // the bounded pace: the game's SetBatchMode has just set UE's Fast values (50 per batch, 16 ms); the render thread's
    // precompile tick adapts the batch size by one per frame towards the time (a race with that tick as in UE itself, whose
    // SetBatchMode also runs on the game thread)
    const float budget = g_budgetMs.load();
    const bool bounded = ours && mode == kFast && Bounded() && g_batchTime && g_batchSize;
    if (bounded) {
        *g_batchTime = budget > 0.25f ? budget : 0.25f;
        if (*g_batchSize > kGentleStartBatch) *g_batchSize = kGentleStartBatch;
    }
    g_appliedBounded = bounded;
    g_appliedBudget = bounded ? budget : 0.0f;
    ReleaseSRWLockExclusive(&g_applyLock);
    if (ours && !wasOurs) {
        g_switchesFast.fetch_add(1, std::memory_order_relaxed);
        Event("[pso] +%.1f s: shader precompile at the faster pace (%s; the game asked for Background)", Now() / 1000.0, why);
    } else if (wasOurs && !ours) {
        g_handBacks.fetch_add(1, std::memory_order_relaxed);
        Event("[pso] +%.1f s: the game's own pace (%s): %s", Now() / 1000.0, why, ModeName(mode));
    }
}

// This precompile is slow (a cold driver cache): the game's own pace until it ends (the game thread's next check applies it).
void MarkCold(ULONGLONG now, const char* why, long long queued)
{
    if (g_cold.exchange(true)) return;
    g_coldWhy = why;
    g_coldMs = now ? now : 1;
    Log("[pso] +%.1f s: cold-cache guard tripped, %s (%lld fetched, not yet dispatched): the game's own pace for the rest of this "
        "precompile", now / 1000.0, why, queued);
}

// One look: the freeze state, the precompile's state and the timeline. Game thread.
void Look(ULONGLONG now)
{
    const bool running = Precompiling();
    g_inPlay = InPlayNow(now);
    // the timeline, from what the last check saw (the pace that was applied up to now)
    if (g_running && g_lastCheck) {
        const uint64_t dt = now - g_lastCheck;
        (g_ours.load() ? g_fastMs : g_gameModeMs).fetch_add(dt, std::memory_order_relaxed);
    }
    if (running && !g_running) {
        if (!g_firstSeenMs) g_firstSeenMs = now ? now : 1;
        g_fastAtStart = g_fastMs.load();   // the 60 s limit counts per precompile
        // A later precompile (an earlier one ended) is judged afresh, unless the monitor thread has already judged this one
        // (its verdict came after the earlier one's end). The first one keeps what the monitor thread may have seen before
        // this thread's first look (the timer starts with the game's first SetBatchMode call, ~10 s after launch).
        if (g_doneMs.load()) {
            if (g_coldMs.load() <= g_doneMs.load()) {
                g_cold = false;
                g_coldWhy = "";
            }
            g_maxQueue = 0;
            g_drvPeakPct = 0;
        }
    }
    if (running) {
        const int64_t queued = g_active ? *g_active : 0;
        if (queued > g_maxQueue.load()) g_maxQueue = queued;
        const int coldQueue = g_coldQueue.load();
        if (!g_cold.load()) {
            const char* why = nullptr;
            if (coldQueue > 0 && queued >= coldQueue) why = "queue: pipelines queue up, the driver compiles them for real (a cold shader cache)";
            else if (coldQueue > 0 && g_ours.load() && g_fastMs.load() - g_fastAtStart >= g_fastMaxMs)
                why = "60 s: the faster pace has run for 60 s in this precompile without finishing";
            if (why) MarkCold(now, why, static_cast<long long>(queued));
        }
    }
    if (!running && g_running) {
        g_doneMs = now;
        Event("[pso] +%.1f s: shader precompile finished (seen from +%.1f s; %.1f s at the Fast pace, %.1f s at the game's)",
              now / 1000.0, g_firstSeenMs.load() / 1000.0, g_fastMs.load() / 1000.0, g_gameModeMs.load() / 1000.0);
        ShaderScreenPrecompileFinished();  // ShaderScreenAfterDriverChange: no-op unless it took the first-launch path
    } else if (!running && g_menuSeen) {
        // ShaderScreenAfterDriverChange (no-op unless a record is pending): the main menu with no precompile running also
        // means it is done (a precompile that ended before this thread's first look is never seen finishing; without this
        // the screen would come back at every start-up)
        ShaderScreenPrecompileFinished();
    }
    g_running = running;
    g_lastCheck = now;
}

// Look under SEH: a fault (the game tearing its objects down while a look is due) leaves the game's own pace for good.
bool LookGuarded()
{
    if (g_faulted) return false;
    __try {
        Look(Now());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted = true;
        g_inPlay = -1;
        return false;
    }
}

const char* Why(bool running, int inPlay)
{
    return !running ? "precompile not running" : ScreenHold() ? "the game's shader screen after a driver change" : g_cold.load() ? "a slow precompile (cold driver cache)" : inPlay == 1 ? "in play" : inPlay == 0 ? "menu, loading screen or start-up"
         : inPlay == 2 ? "a menu opened in play" : "not known";
}

void CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_timerChecks.fetch_add(1, std::memory_order_relaxed);
    const bool ok = LookGuarded();
    const int req = g_requested.load();
    if (req >= 0) {
        const int want = Wanted(req, g_inPlay.load(), g_running);
        // also when the faster pace stays but its budget changes (a menu <-> a loading screen, the key switched)
        const bool rebudget = want == kFast && want != req &&
                              (Bounded() != g_appliedBounded.load() || (Bounded() && g_budgetMs.load() != g_appliedBudget.load()));
        if (want != g_applied.load() || rebudget) Apply(want, ok ? Why(g_running, g_inPlay.load()) : "a look faulted");
    }
    if (!ok) {
        StopTimer();
        Log("[pso] a look at the game's state faulted: the game's own pace from here on");
    } else if (!g_running && (g_firstSeenMs || GetTickCount64() - g_timerStart > kIdleStopMs) && g_applied.load() == req) {
        StopTimer();   // nothing to do until the game sets a mode again (the next map load restarts it)
    }
    QueryPerformanceCounter(&t1);
    g_timerTicks.fetch_add(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
}

void SetBatchModeHook(int mode)
{
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_requested = mode;
    if (GetCurrentThreadId() == g_gameThread && LookGuarded()) StartTimer();
    // on another thread (the render thread's MaxPrecompileTime drop) the game thread's last look decides
    const int want = Wanted(mode, g_inPlay.load(), Precompiling());
    if (want != mode) g_replaced.fetch_add(1, std::memory_order_relaxed);
    Apply(want, Why(Precompiling(), g_inPlay.load()));
}

// The driver-thread decision: cores of CPU its compile threads used in the last second (monitor thread, or the self-test).
void DriverBusy(double cores)
{
    const int pct = static_cast<int>(cores * 100.0 + 0.5);
    if (pct > g_drvPeakPct.load()) g_drvPeakPct = pct;
    g_drvBusySeconds = cores >= 0.5 ? g_drvBusySeconds + 1 : 0;
    if (g_drvBusySeconds >= 2 && g_coldQueue.load() > 0)
        MarkCold(Now(), "driver threads: the graphics driver compiles shaders on its background threads (a cold shader cache)",
                 g_active ? static_cast<long long>(*g_active) : -1);
}

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

void ForgetDriverThreads()
{
    for (int i = 0; i < g_drvCount; ++i) CloseHandle(g_drvThreads[i]);
    g_drvCount = 0;
    g_drvLastCpu = 0;
    g_drvLastAt = 0;
}

// This process's threads (ID, creation time) from one NtQuerySystemInformation(SystemProcessInformation) call; -1 when
// the call fails or the layout does not hold (the calling thread must be among them). Not a Toolhelp snapshot:
// Thread32First/Next map the snapshot with NtMapViewOfSection on every call, one per thread on the PC (thousands per
// search), outside the loader lock. The game's embedded browser (CEF 90's chrome_elf.dll) hooks NtMapViewOfSection while
// it loads, and a call landing before the hook has stored the original jumps to address 0: the monitor thread crashed
// there at start-up (2026-10-27 14:13 and 2026-10-29 00:59; the dump's stack: Thread32Next -> kernel32 ->
// chrome_elf+0x1902 -> chrome_elf+0x14D5 call rax = 0). This call copies the list into our buffer, nothing is mapped.
struct OwnThread {
    DWORD id;
    ULONGLONG created;  // FILETIME units
};

int OwnThreads(OwnThread* out, int max)
{
    using QuerySystemInformationFn = LONG(WINAPI*)(ULONG cls, void* info, ULONG size, ULONG* returned);
    static const auto query = reinterpret_cast<QuerySystemInformationFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) return -1;
    // x64 SYSTEM_PROCESS_INFORMATION: NextEntryOffset +0, NumberOfThreads +4, UniqueProcessId +0x50, then 0x100 bytes in
    // its SYSTEM_THREAD_INFORMATION array (0x50 each: CreateTime +0x10, ClientId.UniqueThread +0x30)
    constexpr ULONG kSystemProcessInformation = 5;
    constexpr LONG kInfoLengthMismatch = static_cast<LONG>(0xC0000004);
    constexpr size_t kProcThreads = 0x100, kProcId = 0x50, kThreadSize = 0x50, kThreadCreate = 0x10, kThreadId = 0x30;
    std::vector<uint8_t> buf(1u << 20);
    ULONG got = 0;
    LONG status = kInfoLengthMismatch;
    for (int tries = 0; tries < 4; ++tries) {
        status = query(kSystemProcessInformation, buf.data(), static_cast<ULONG>(buf.size()), &got);
        if (status != kInfoLengthMismatch) break;
        buf.resize((got > buf.size() ? got : buf.size() * 2) + (64u << 10));   // threads come and go between calls
    }
    if (status < 0 || got > buf.size()) return -1;
    const uintptr_t self = GetCurrentProcessId();
    const DWORD caller = GetCurrentThreadId();
    for (size_t at = 0; at + kProcThreads <= got;) {
        const uint8_t* const p = buf.data() + at;
        ULONG next, count;
        uintptr_t pid;
        memcpy(&next, p, 4);
        memcpy(&count, p + 4, 4);
        memcpy(&pid, p + kProcId, sizeof pid);
        if (pid == self) {
            if (at + kProcThreads + static_cast<size_t>(count) * kThreadSize > got) return -1;
            int n = 0;
            bool callerSeen = false;
            for (ULONG i = 0; i < count; ++i) {
                const uint8_t* const t = p + kProcThreads + i * kThreadSize;
                uintptr_t tid;
                ULONGLONG created;
                memcpy(&tid, t + kThreadId, sizeof tid);
                memcpy(&created, t + kThreadCreate, sizeof created);
                callerSeen = callerSeen || tid == caller;
                if (n < max) out[n++] = {static_cast<DWORD>(tid), created};
            }
            return callerSeen ? n : -1;
        }
        if (!next) break;
        at += next;
    }
    return -1;
}

constexpr int kMaxOwnThreads = 1024;

// D3D12's background-processing threads in this process ("D3D Background Thread N"). The query is system-wide (every
// thread on the PC is copied), so the caller paces it.
void FindDriverThreads(ULONGLONG now)
{
    static const auto getDesc = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    ForgetDriverThreads();
    g_drvSearchAt = now ? now : 1;
    ++g_drvSearches;
    if (!getDesc) return;
    static OwnThread threads[kMaxOwnThreads];   // the monitor thread's only
    const int n = OwnThreads(threads, kMaxOwnThreads);
    for (int i = 0; i < n && g_drvCount < kMaxDriverThreads; ++i) {
        const HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threads[i].id);
        if (!h) continue;
        PWSTR name = nullptr;
        bool keep = false;
        if (SUCCEEDED(getDesc(h, &name)) && name) {
            keep = wcsncmp(name, L"D3D Background Thread", 21) == 0;
            LocalFree(name);
        }
        if (keep) g_drvThreads[g_drvCount++] = h;
        else CloseHandle(h);
    }
}

bool AnyDriverThreadExited()
{
    for (int i = 0; i < g_drvCount; ++i) {
        DWORD code = 0;
        if (!GetExitCodeThread(g_drvThreads[i], &code) || code != STILL_ACTIVE) return true;
    }
    return false;
}

void ResetDriverSearch()   // a new precompile searches afresh
{
    g_drvSearchAt = 0;
    g_drvSearches = 0;
    g_drvEmptySearches = 0;
    g_drvGaveUp = false;
}

uint64_t DriverThreadsCpu()
{
    uint64_t sum = 0;
    for (int i = 0; i < g_drvCount; ++i) {
        FILETIME c, e, k, u;
        if (GetThreadTimes(g_drvThreads[i], &c, &e, &k, &u))
            sum += ((static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
                   ((static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
    }
    return sum;   // 100 ns units
}

// jmp [rip+0]; dq target
void WriteAbsJump(uint8_t* at, const void* target)
{
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, jmp, sizeof jmp);
    memcpy(at + sizeof jmp, &target, sizeof target);
}

// The function's first 5 bytes become "jmp rel32 stub" in one atomic 8-byte write (the function is 16-byte aligned).
bool PatchEntry(uint8_t* fn, uint8_t* stub, bool beforeGameCode)
{
    const int64_t rel = stub - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t insn[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(insn + 1, &rel32, sizeof rel32);
    if ((reinterpret_cast<uintptr_t>(fn) & 7) == 0) {
        auto* q = reinterpret_cast<volatile LONG64*>(fn);
        DWORD old;
        if (!VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        const LONG64 before = *q;
        LONG64 after = before;
        memcpy(&after, insn, sizeof insn);
        const bool ok = InterlockedCompareExchange64(q, after, before) == before;
        VirtualProtect(fn, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, 8);
        return ok;
    }
    return beforeGameCode && WriteCode(fn, insn);
}

// The process's first thread (the game thread): the one created first (from OwnThreads, not a Toolhelp snapshot).
DWORD MainThreadId()
{
    static OwnThread threads[kMaxOwnThreads];   // start-up only
    const int n = OwnThreads(threads, kMaxOwnThreads);
    DWORD best = 0;
    ULONGLONG bestTime = ~0ull;
    for (int i = 0; i < n; ++i)
        if (threads[i].created < bestTime) bestTime = threads[i].created, best = threads[i].id;
    return best;
}

uint8_t* Rel32Target(const uint8_t* insn, size_t opLen) // jcc/jmp rel32: target = next instruction + rel32
{
    int32_t rel;
    memcpy(&rel, insn + opLen, 4);
    return const_cast<uint8_t*>(insn) + opLen + 4 + rel;
}

}  // namespace

bool InstallPsoPrecompileFast(bool beforeGameCode)
{
    if (g_fn) return true;
    const GameModule& game = Game();
    const auto fns = FindAll(game.text, ParsePattern(kSetBatchMode));
    if (fns.size() != 1) {
        Log("[pso] SetBatchMode not found (%zu matches): fix not installed", fns.size());
        return false;
    }
    uint8_t* const fn = fns[0];
    void** const singleton = reinterpret_cast<void**>(RipTarget(fn + kSingletonCmp, 3, 8));
    // the three cases: mode 0 and anything but 1 and 2 -> Background, 1 -> Fast, 2 -> Precompile; each stores the batch
    // size into the same static (at the case's +0x2E)
    uint8_t* const background = Rel32Target(fn + kBackgroundJe, 2);
    uint8_t* const fast = fn + kFastJe + 2 + static_cast<int8_t>(fn[kFastJe + 1]);
    uint8_t* const otherwise = Rel32Target(fn + kPrecompileJne, 2);
    uint8_t* const precompile = fn + kPrecompileBlock;
    const auto caseOk = [&](const uint8_t* c) {
        return c > fn && c < fn + 0x200 && c[0] == 0x48 && c[1] == 0x8B && c[2] == 0x3D && c[kBlockStore] == 0x89 &&
               c[kBlockStore + 1] == 0x05;
    };
    uint8_t* const batchSize = caseOk(precompile) ? RipTarget(precompile + kBlockStore, 2, 6) : nullptr;
    const bool casesOk = caseOk(background) && caseOk(fast) && otherwise == background && batchSize &&
                         RipTarget(fast + kBlockStore, 2, 6) == batchSize && RipTarget(background + kBlockStore, 2, 6) == batchSize;
    // the batch time: the shared tail's movss [BatchTime],xmm0 (F3 0F 11 05 disp32), 0x29 bytes after the Background store
    uint8_t* const timeStore = background + kBlockStore + 6 + 0x23;
    const bool timeOk = casesOk && timeStore[0] == 0xF3 && timeStore[1] == 0x0F && timeStore[2] == 0x11 && timeStore[3] == 0x05;
    const auto inData = [&](const void* p) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(p), b = reinterpret_cast<uintptr_t>(game.base);
        return a > b && a + 8 < b + game.sizeOfImage && !game.text.Contains(p) && (a & 3) == 0;
    };
    if (!casesOk || !timeOk || !inData(singleton) || !inData(batchSize) || (reinterpret_cast<uintptr_t>(singleton) & 7)) {
        Log("[pso] SetBatchMode at exe+0x%llX does not look as expected (cases %s, batch time %s): fix not installed",
            static_cast<unsigned long long>(fn - game.base), casesOk ? "ok" : "differ", timeOk ? "ok" : "differ");
        return false;
    }
    float* const batchTime = reinterpret_cast<float*>(RipTarget(timeStore, 4, 8));
    // IsPrecompiling: the getter of the same singleton gives the flag's offset
    uint32_t flagOffset = 0;
    int getters = 0;
    for (uint8_t* p : FindAll(game.text, ParsePattern(kIsPrecompiling)))
        if (RipTarget(p, 3, 7) == reinterpret_cast<uint8_t*>(singleton)) {
            memcpy(&flagOffset, p + kFlagDisp, 4);
            ++getters;
        }
    if (getters != 1 || flagOffset == 0 || flagOffset > 0x1000) {
        Log("[pso] IsPrecompiling not found (%d matches, flag +0x%X): fix not installed", getters, flagOffset);
        return false;
    }
    // TotalActiveTasks (the cold-cache guard): from NumPrecompilesRemaining; without it only the 60 s limit guards
    volatile int64_t* active = nullptr;
    {
        const auto rem = FindAll(game.text, ParsePattern(kRemaining));
        if (rem.size() == 1) {
            auto* a = reinterpret_cast<int64_t*>(RipTarget(rem[0], 3, 7));
            auto* w = reinterpret_cast<int64_t*>(RipTarget(rem[0] + 8, 3, 7));
            if (inData(a) && inData(w) && w == a + 1 && (reinterpret_cast<uintptr_t>(a) & 7) == 0) active = a;
        }
        if (!active)
            Log("[pso] the precompile's queue counter not found (%zu matches): the cold-cache guard has only the driver-thread "
                "check and the %llu s limit per precompile", rem.size(), static_cast<unsigned long long>(kFastMaxMs / 1000));
    }
    if (memcmp(fn, kEntry, sizeof kEntry) != 0) {
        Log("[pso] SetBatchMode does not start as expected (already patched?): fix not installed");
        return false;
    }
    // "in play" needs the freeze subsystem (found with the loading-screen hook's search, nothing installed)
    g_freezeKnown = FindFreezeSubsystem();
    if (!g_freezeKnown) {
        Log("[pso] freeze subsystem not found (no way to tell play from menus): fix not installed");
        return false;
    }
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        g_setTimer = reinterpret_cast<SetTimerFn>(GetProcAddress(user32, "SetTimer"));
        g_killTimer = reinterpret_cast<KillTimerFn>(GetProcAddress(user32, "KillTimer"));
    }
    if (!g_setTimer || !g_killTimer) {
        Log("[pso] user32 timers not available (the game's pace could not be handed back when play starts): fix not installed");
        return false;
    }
    g_gameThread = beforeGameCode ? GetCurrentThreadId() : MainThreadId();
    if (!g_gameThread) {
        Log("[pso] game thread not known: fix not installed");
        return false;
    }
    // stub to the hook and the trampoline: the two moved instructions (the comparison re-targeted), then jmp back
    constexpr size_t kRegion = 0x1000;
    uint8_t* const region = AllocNearGame(kRegion);
    if (!region) {
        Log("[pso] no free memory near the game's code: fix not installed");
        return false;
    }
    uint8_t* const stub = region, *const tramp = region + 0x20;
    WriteAbsJump(stub, reinterpret_cast<const void*>(&SetBatchModeHook));
    memcpy(tramp, fn, kEntryLen);
    const int64_t disp = reinterpret_cast<uint8_t*>(singleton) - (tramp + kSingletonCmp + 8);
    if (disp != static_cast<int32_t>(disp)) {
        VirtualFree(region, 0, MEM_RELEASE);
        Log("[pso] trampoline out of reach of the singleton: fix not installed");
        return false;
    }
    const int32_t disp32 = static_cast<int32_t>(disp);
    memcpy(tramp + kSingletonCmp + 3, &disp32, 4);
    WriteAbsJump(tramp + kEntryLen, fn + kEntryLen);   // jmp [rip] leaves the comparison's flags for the je that follows
    DWORD old;
    VirtualProtect(region, kRegion, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, kRegion);
    g_orig = reinterpret_cast<SetBatchModeFn>(tramp);
    g_singleton = singleton;
    g_flagOffset = flagOffset;
    g_batchSize = reinterpret_cast<uint32_t*>(batchSize);
    g_batchTime = batchTime;
    g_region = region;
    g_active = active;
    g_t0 = GetTickCount64();
    if (!PatchEntry(fn, stub, beforeGameCode)) {
        Log("[pso] could not patch SetBatchMode at exe+0x%llX: fix not installed", static_cast<unsigned long long>(fn - game.base));
        return false;
    }
    g_fn = fn;
    Log("[pso] SetBatchMode exe+0x%llX detoured (precompile flag +0x%X, batch size/time exe+0x%llX/+0x%llX): the start-up shader "
        "precompile runs faster outside play (%.1f ms of render thread per frame; %s%d%s)",
        static_cast<unsigned long long>(fn - game.base), flagOffset, static_cast<unsigned long long>(batchSize - game.base),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(batchTime) - game.base), g_budgetMs.load(),
        g_coldQueue.load() > 0 ? "cold-cache guard: the game's pace once pipelines queue up to " : "PsoPrecompileColdQueue=",
        g_coldQueue.load(),
        g_coldQueue.load() <= 0 ? ": no cold-cache guard" : active ? ", the driver's threads are busy or after 60 s at the faster pace"
                                                                  : " (queue counter not found), the driver's threads are busy or after 60 s");
    return true;
}

void SetPsoPrecompileFast(bool enabled)
{
    g_enabled = enabled;   // the timer (if running) applies the change at its next check
}

void SetPsoPrecompileScreenHold(bool on)
{
    if (g_screenHold.exchange(on) != on && on)
        Log("[pso] the game's shader screen is up: UE's Fast pace, the cold-cache guard aside, until the main menu");
}

bool PsoPrecompileScreenHold() { return g_screenHold.load(); }

void SetPsoPrecompileFastInGameMenus(bool enabled)
{
    g_inGameMenus = enabled;   // as above: the timer's next check
}

// Once a second from the monitor thread: while the precompile runs and is not judged slow yet, how busy the driver's
// compile threads were since the last call.
// The threads are searched for once per precompile, again only when one of them has exited; with none found, at most every
// 10 s and three times.
void PsoPrecompileWatchDriver()
{
    if (!g_fn) return;
    const bool precompiling = Precompiling();
    if (!precompiling || g_cold.load() || g_coldQueue.load() <= 0) {
        if (g_drvCount) ForgetDriverThreads();
        g_drvBusySeconds = 0;
        if (!precompiling) ResetDriverSearch();
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const bool due = !g_drvSearchAt || now - g_drvSearchAt >= g_drvRetryMs;
    if (due && (g_drvCount ? AnyDriverThreadExited() : !g_drvGaveUp)) {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        FindDriverThreads(now);
        QueryPerformanceCounter(&t1);
        g_drvSearchAt = GetTickCount64();   // the interval counts from the search's end
        g_drvSearchMs = f.QuadPart ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart) : 0.0;
        if (g_drvCount) {
            g_drvEmptySearches = 0;
            if (!g_drvLoggedFound) {
                g_drvLoggedFound = true;
                Log("[pso] +%.1f s: %d driver compile thread(s) found (\"D3D Background Thread\"; one search %.1f ms): the "
                      "driver-thread guard is active", Now() / 1000.0, g_drvCount, g_drvSearchMs);
            }
        } else if (++g_drvEmptySearches >= kDriverSearches) {
            g_drvGaveUp = true;
            if (!g_drvLoggedNone) {
                g_drvLoggedNone = true;
                Log("[pso] +%.1f s: no driver compile threads found (%d searches, %.1f ms each): the driver-thread guard is off "
                      "for this precompile; the queue and 60 s limits still apply", Now() / 1000.0, g_drvSearches, g_drvSearchMs);
            }
        }
    }
    if (!g_drvCount) return;
    const uint64_t cpu = DriverThreadsCpu();
    if (g_drvLastAt && now > g_drvLastAt && cpu >= g_drvLastCpu)
        DriverBusy(static_cast<double>(cpu - g_drvLastCpu) / 1e4 / static_cast<double>(now - g_drvLastAt));
    g_drvLastCpu = cpu;
    g_drvLastAt = now;
}

void PsoPrecompileTestDriverBusy(double cores) { DriverBusy(cores); }

int PsoPrecompileTestOwnThreads(uint32_t* mainThread)
{
    static OwnThread threads[kMaxOwnThreads];
    *mainThread = MainThreadId();
    return OwnThreads(threads, kMaxOwnThreads);
}

void SetPsoPrecompileFastBudget(float ms, int coldQueue)
{
    g_budgetMs = ms;            // the timer's next check (re-applies the faster pace when the budget differs)
    g_coldQueue = coldQueue;    // the timer's next look
}

void PsoPrecompileTestTiming(uint32_t fastMaxMs, uint32_t driverRetryMs)
{
    g_fastMaxMs = fastMaxMs;
    g_drvRetryMs = driverRetryMs;
}

void PsoPrecompileTestStatics(uint32_t* batchSize, float* batchTime, int64_t* active)
{
    g_batchSize = batchSize;
    g_batchTime = batchTime;
    g_active = active;
}

void PsoPrecompileTestPlayMs(uint32_t ms)
{
    g_playMs = ms;
    g_played = false;
    g_playSince = 0;
    g_menuSeen = false;
}

void LogPsoPrecompileStats()
{
    if (!g_fn) return;
    const uint64_t calls = g_calls.load(), replaced = g_replaced.load(), fast = g_switchesFast.load(), back = g_handBacks.load(),
                   checks = g_timerChecks.load();
    const uint64_t sum = calls + replaced + fast + back + checks + g_doneMs.load() + g_coldMs.load();
    if (sum == g_logged) return;
    g_logged = sum;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    Log("[pso] precompile seen from +%.1f s, finished %s%.1f s; %.1f s at the Fast pace, %.1f s at the game's; game's mode requests %llu "
        "(%llu at Fast instead of Background), switches to Fast %llu, back to the game's %llu; timer checks %llu (%.1f us each); "
        "batch now %u per frame, %.1f ms; queue max %lld, driver threads up to %d %% of a core%s%s",
        g_firstSeenMs.load() / 1000.0, g_doneMs.load() ? "at +" : "(not yet) ", g_doneMs.load() / 1000.0, g_fastMs.load() / 1000.0,
        g_gameModeMs.load() / 1000.0, static_cast<unsigned long long>(calls), static_cast<unsigned long long>(replaced),
        static_cast<unsigned long long>(fast), static_cast<unsigned long long>(back), static_cast<unsigned long long>(checks),
        checks && f.QuadPart ? 1e6 * static_cast<double>(g_timerTicks.load()) / static_cast<double>(f.QuadPart) / static_cast<double>(checks) : 0.0,
        g_batchSize ? *g_batchSize : 0u, g_batchTime ? *g_batchTime : 0.0f, static_cast<long long>(g_maxQueue.load()), g_drvPeakPct.load(),
        g_cold.load() ? "; the game's pace since the cold-cache guard tripped, " : "", g_cold.load() ? g_coldWhy.load() : "");
}

bool PsoPrecompileInfo(PsoPrecompileInfoOut* out)
{
    if (!g_fn || !out) return false;
    out->setBatchMode = g_fn;
    out->trampoline = reinterpret_cast<uint8_t*>(g_orig);
    out->singleton = g_singleton;
    out->flagOffset = g_flagOffset;
    out->batchSize = g_batchSize;
    out->batchTime = g_batchTime;
    out->hook = reinterpret_cast<void*>(&SetBatchModeHook);
    out->timerProc = reinterpret_cast<void*>(&TimerProc);
    out->requested = g_requested.load();
    out->applied = g_applied.load();
    out->inPlay = g_inPlay.load();
    out->timerRunning = g_timer != 0;
    out->switchesFast = g_switchesFast.load();
    out->handBacks = g_handBacks.load();
    out->replaced = g_replaced.load();
    out->fastMs = g_fastMs.load();
    out->doneMs = g_doneMs.load();
    out->cold = g_cold.load();
    out->bounded = g_appliedBounded.load();
    out->active = const_cast<int64_t*>(g_active);
    out->coldWhy = g_coldWhy.load();
    out->driverThreads = g_drvCount;
    out->driverSearches = g_drvSearches;
    out->driverGaveUp = g_drvGaveUp;
    out->driverSearchMs = g_drvSearchMs;
    return true;
}

void PsoPrecompileTestOriginal(void* fakeSetBatchMode)
{
    StopTimer();
    g_orig = reinterpret_cast<SetBatchModeFn>(fakeSetBatchMode);
    g_requested = -1;
    g_applied = -1;
    g_ours = false;
    g_inPlay = -1;
    g_running = false;
    g_lastCheck = 0;
    g_faulted = false;
    g_played = false;
    g_playSince = 0;
    g_playMs = 3000;
    g_menuSeen = false;
    g_inGameMenus = false;
    g_cold = false;
    g_drvBusySeconds = 0;
    g_drvPeakPct = 0;
    g_loadingNow = false;
    g_appliedBounded = false;
    g_maxQueue = 0;
    g_coldMs = 0;
    g_coldWhy = "";
    g_fastMs = 0;
    g_fastAtStart = 0;
    g_doneMs = 0;
    g_appliedBudget = 0.0f;
    g_budgetMs = 4.0f;
    g_coldQueue = 1000;
    g_fastMaxMs = kFastMaxMs;
    g_drvRetryMs = 10000;
    ForgetDriverThreads();
    ResetDriverSearch();
}

}  // namespace opf
