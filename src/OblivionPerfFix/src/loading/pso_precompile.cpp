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
// starts. Fast and Precompile requests pass unchanged, pausing and resuming are the game's, nothing else is touched. The
// same pipelines are created, only sooner: identical results (timing only). With a cold driver cache (after a driver
// update) the precompile takes longer; it is then fast behind menus and loading screens and at the game's pace in play.
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
#include <tlhelp32.h>

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

enum Mode : int { kBackground = 0, kFast = 1, kPrecompile = 2 };
const char* const kModeName[3] = {"Background", "Fast", "Precompile"};
const char* ModeName(int m) { return m >= 0 && m <= 2 ? kModeName[m] : "?"; }

constexpr UINT kTimerMs = 250;
constexpr ULONGLONG kIdleStopMs = 120000;  // no precompile seen this long after the timer started: stop (restarted by the next call)
constexpr int kMaxEventLines = 40;

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

std::atomic<bool> g_enabled{true};
std::atomic<int> g_requested{-1};   // the game's last request (-1: none seen yet)
std::atomic<int> g_applied{-1};     // what the fix last passed on (with the call, under g_applyLock)
std::atomic<bool> g_ours{false};    // the applied mode is the fix's Fast in place of the game's Background
SRWLOCK g_applyLock = SRWLOCK_INIT; // the game thread's timer and a render-thread request may apply at the same time
std::atomic<int> g_inPlay{-1};      // the game thread's last look: 1 in play, 0 not, -1 not known yet

// game thread (timer)
using SetTimerFn = UINT_PTR(WINAPI*)(HWND, UINT_PTR, UINT, TIMERPROC);
using KillTimerFn = BOOL(WINAPI*)(HWND, UINT_PTR);
SetTimerFn g_setTimer = nullptr;
KillTimerFn g_killTimer = nullptr;
UINT_PTR g_timer = 0;
ULONGLONG g_timerStart = 0;
ULONGLONG g_lastCheck = 0;
bool g_faulted = false;
int g_eventLines = 0;

// timeline (ms since the install)
ULONGLONG g_t0 = 0;
std::atomic<uint64_t> g_firstSeenMs{0}, g_doneMs{0}, g_fastMs{0}, g_gameModeMs{0};
bool g_running = false;  // game thread: the precompile ran at the last check
std::atomic<uint64_t> g_calls{0}, g_replaced{0}, g_switchesFast{0}, g_handBacks{0}, g_timerChecks{0}, g_timerTicks{0};
uint64_t g_logged = 0;

void Event(const char* fmt, ...)
{
    if (g_eventLines >= kMaxEventLines) return;
    if (++g_eventLines == kMaxEventLines) {
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

// 1 = in play (no named freeze, no loading freeze), 0 = not (or the freeze subsystem does not exist yet). Game thread.
int InPlayNow()
{
    int32_t named = -1, mode = -1;
    if (!ReadMenuFreeze(&named, &mode)) return 0;
    return named == 0 && mode == 0 ? 1 : 0;
}

// What to pass on for the game's request: Fast instead of Background while the precompile runs and nobody plays.
int Wanted(int requested, int inPlay, bool precompiling)
{
    if (g_enabled.load(std::memory_order_relaxed) && !g_faulted && precompiling && requested == kBackground && inPlay == 0)
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
    ReleaseSRWLockExclusive(&g_applyLock);
    if (ours && !wasOurs) {
        g_switchesFast.fetch_add(1, std::memory_order_relaxed);
        Event("[pso] +%.1f s: shader precompile at UE's Fast pace (%s; the game asked for Background)", Now() / 1000.0, why);
    } else if (wasOurs && !ours) {
        g_handBacks.fetch_add(1, std::memory_order_relaxed);
        Event("[pso] +%.1f s: the game's own pace (%s): %s", Now() / 1000.0, why, ModeName(mode));
    }
}

// One look: the freeze state, the precompile's state and the timeline. Game thread.
void Look(ULONGLONG now)
{
    const bool running = Precompiling();
    g_inPlay = InPlayNow();
    // the timeline, from what the last check saw (the pace that was applied up to now)
    if (g_running && g_lastCheck) {
        const uint64_t dt = now - g_lastCheck;
        (g_ours.load() ? g_fastMs : g_gameModeMs).fetch_add(dt, std::memory_order_relaxed);
    }
    if (running && !g_running && !g_firstSeenMs) g_firstSeenMs = now ? now : 1;
    if (!running && g_running) {
        g_doneMs = now;
        Event("[pso] +%.1f s: shader precompile finished (seen from +%.1f s; %.1f s at the Fast pace, %.1f s at the game's)",
              now / 1000.0, g_firstSeenMs.load() / 1000.0, g_fastMs.load() / 1000.0, g_gameModeMs.load() / 1000.0);
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
    return !running ? "precompile not running" : inPlay == 1 ? "in play" : inPlay == 0 ? "menu, loading screen or start-up" : "not known";
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
        if (want != g_applied.load()) Apply(want, ok ? Why(g_running, g_inPlay.load()) : "a look faulted");
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

// The process's first thread (the game thread): the one created first.
DWORD MainThreadId()
{
    DWORD best = 0;
    ULONGLONG bestTime = ~0ull;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID)) {
            FILETIME c, e, k, u;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                const ULONGLONG t = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
                if (t < bestTime) bestTime = t, best = te.th32ThreadID;
            }
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
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
    g_t0 = GetTickCount64();
    if (!PatchEntry(fn, stub, beforeGameCode)) {
        Log("[pso] could not patch SetBatchMode at exe+0x%llX: fix not installed", static_cast<unsigned long long>(fn - game.base));
        return false;
    }
    g_fn = fn;
    Log("[pso] SetBatchMode exe+0x%llX detoured (precompile flag +0x%X, batch size/time exe+0x%llX/+0x%llX): the start-up shader "
        "precompile runs at UE's Fast pace outside play",
        static_cast<unsigned long long>(fn - game.base), flagOffset, static_cast<unsigned long long>(batchSize - game.base),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(batchTime) - game.base));
    return true;
}

void SetPsoPrecompileFast(bool enabled)
{
    g_enabled = enabled;   // the timer (if running) applies the change at its next check
}

void LogPsoPrecompileStats()
{
    if (!g_fn) return;
    const uint64_t calls = g_calls.load(), replaced = g_replaced.load(), fast = g_switchesFast.load(), back = g_handBacks.load(),
                   checks = g_timerChecks.load();
    const uint64_t sum = calls + replaced + fast + back + checks + g_doneMs.load();
    if (sum == g_logged) return;
    g_logged = sum;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    Log("[pso] precompile seen from +%.1f s, finished %s%.1f s; %.1f s at the Fast pace, %.1f s at the game's; game's mode requests %llu "
        "(%llu at Fast instead of Background), switches to Fast %llu, back to the game's %llu; timer checks %llu (%.1f us each); "
        "batch now %u per frame, %.1f ms",
        g_firstSeenMs.load() / 1000.0, g_doneMs.load() ? "at +" : "(not yet) ", g_doneMs.load() / 1000.0, g_fastMs.load() / 1000.0,
        g_gameModeMs.load() / 1000.0, static_cast<unsigned long long>(calls), static_cast<unsigned long long>(replaced),
        static_cast<unsigned long long>(fast), static_cast<unsigned long long>(back), static_cast<unsigned long long>(checks),
        checks && f.QuadPart ? 1e6 * static_cast<double>(g_timerTicks.load()) / static_cast<double>(f.QuadPart) / static_cast<double>(checks) : 0.0,
        g_batchSize ? *g_batchSize : 0u, g_batchTime ? *g_batchTime : 0.0f);
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
}

}  // namespace opf
