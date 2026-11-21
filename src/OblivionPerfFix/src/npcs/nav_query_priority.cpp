// NavQueryPriority ([Fixes] NavQueryPriority, default 0 = the game's setting; 1 = the navigation query task runs at normal
// priority; worker navprio 2026-11-18, from worker navprio2's A/B and worker s048gather's thread samples; S-048).
//
// What it fixes: every frame the engine's navigation system hands the characters' queued path searches to ONE task
// (UNavigationSystemV1::TriggerAsyncQueries -> PerformAsyncQueries, a FSimpleDelegateGraphTask) and, in the same frame's
// post-actor tick, the game thread waits for that task (UNavigationSystemV1::PostponeAsyncQueries: sets the abort flag and
// WaitUntilTaskCompletes on the game thread, which cannot take the task for itself in 5.3: a named thread waits by running
// its own queue). The task is dispatched with the priority from Epic's console variable
// TaskGraph.TaskPriorities.NavTriggerAsyncQueries, whose default is "bnn": background thread priority, normal task
// priority (UE 5.3 NavigationSystem.cpp, FAutoConsoleTaskPriority CPrio_TriggerAsyncQueries; the same default in 5.4 and 5.7).
// Background-priority tasks run only on the task graph's Background workers (11 here). While the world is busy those workers
// are all inside ray-tracing pipeline links (one at a time inside the driver, 20-200 ms each): the nav task waits in the
// queue behind them with 3 idle Foreground and 14 idle Reserve workers beside it, and the game thread waits for the rest of
// the link in flight: 99-145 ms waits at the Imperial City Waterfront in 2 of 2 stock runs, 0 of 2 with the setting below
// (a path search itself takes 1-10 ms).
//
// What it does: sets that variable to "nnn" (the engine's own format: [thread priority][task priority][task priority if the
// thread priority is not available], 'n' = normal), so the task is queued for the Foreground / Reserve workers, which are
// idle at those moments. The same as `TaskGraph.TaskPriorities.NavTriggerAsyncQueries=nnn` under [SystemSettings] in
// Engine.ini, which also works and wins over this key. Epic's variable; we only set it.
//
// Why the results are equivalent (not bit for bit): only the worker that runs the task changes. The task is the same
// function over the same queued queries in the same order (one task, one query after another), each query is the same
// code on the same inputs, the game thread still waits for its completion in PostponeAsyncQueries before it uses or
// changes anything the queries read, and the results are handed back by DispatchAsyncQueriesResults as before. One timing
// difference: PostponeAsyncQueries sets bAbortAsyncQueriesRequested before it waits, and PerformAsyncQueries stops after the
// query in hand when it sees it, leaving the rest for the next frame (NavigationSystem.cpp:1741-1755). Stock, a task that
// still sits in the queue starts only after the flag is set and does one query; with the task running at once it gets
// through more queries first, so a query's result arrives on the same frame or an earlier one, never later. Stock varied
// here with thread timing already. Nothing in the engine reads the priority apart from the one Get() at dispatch
// (FAutoConsoleTaskPriority::Get, called at every TriggerAsyncQueries; not cached at start-up), so a change takes effect
// at the next dispatch. The variable is Epic's (UE 5.3 NavigationSystem.cpp:1663; the same default in 5.4-5.7).
//
// How: the console variable is looked up through the game's console manager (as SeparableSkinAbove4K finds it) and set
// through its own IConsoleVariable::Set (vtable +0x80) at the project-setting priority, so the engine's priority rule keeps
// anything the player set (Engine.ini [SystemSettings], console, command line) and the engine's own OnSettingChanged
// callback updates the cached priorities. IConsoleVariable::Set must run on the game thread (FConsoleVariableStringRef
// writes its bound string through OnCVarChange, which does nothing off the game thread in a shipping build), so the call is
// made from the game thread at UWorld::AddToWorld, through the hook StreamRegisterGranularity / StreamInBudget already
// install (installed when any of the three keys is on), which runs at every cell load before any character walks. Set only
// while the variable still has its constructor priority (nobody set it); a value anyone else set is left alone, logged
// once. Switching the key off (live) puts the game's "bnn" back, only if the variable is still ours. The monitor thread
// passes the key (SetNavQueryPriority) and, when it first reads 1, checks the name in the exe (a scan of .rdata and .text,
// kept off the game thread); all reads and writes of the variable are on the game thread.
//
// Clean off: the variable's name must be in the game's .rdata and referenced from code, the console manager must be found
// and know the name, and the variable must be a string variable; otherwise a log line and nothing is set.
#include "common.h"

#include <atomic>
#include <cstring>
#include <cwchar>

namespace opf {
namespace {

constexpr wchar_t kName[] = L"TaskGraph.TaskPriorities.NavTriggerAsyncQueries";
constexpr wchar_t kOn[] = L"nnn";    // normal thread priority, normal task priority, normal if forced to normal threads
constexpr wchar_t kGame[] = L"bnn";  // the game's (Epic's default): background threads at normal task priority
constexpr uint32_t kSetByMask = 0xFF000000u, kSetByConstructor = 0, kSetByProjectSetting = 0x03000000u;
constexpr size_t kGetFlags = 0x18, kIsVariableString = 0x48, kSet = 0x80, kFindVar = 0xB8;
constexpr int kMaxFindTries = 4000;  // game-thread calls (AddToWorld) before giving up on finding the variable

using GetFlagsFn = uint32_t (*)(void*);
using BoolFn = bool (*)(void*);
using SetFn = void (*)(void*, const wchar_t*, uint32_t);
using FindCvarFn = void* (*)(void* manager, const wchar_t* name, bool trackFrequentCalls);

std::atomic<int> g_key{0};  // monitor thread -> game thread
bool g_nameChecked = false, g_nameOk = false;  // NameInGame: monitor thread (and the self-test) only
// The name check's result for the game thread: 0 not checked yet, 1 found, 2 not found. The check (a scan of .rdata and
// .text, about 0.1-0.2 s) runs on the monitor thread when the key first reads 1, never on the game thread (review S2).
std::atomic<int> g_nameState{0};

// game thread only
void* g_cvar = nullptr;     // IConsoleVariable* TaskGraph.TaskPriorities.NavTriggerAsyncQueries
bool g_gaveUp = false;
int g_findTries = 0;
bool g_ours = false;        // the variable holds our "nnn"
bool g_touched = false;     // we set it at least once (its priority is the project's since)
bool g_leftAloneLogged = false;
int g_decidedKey = -1;      // the key the last call settled (-1: nothing yet); a settled key costs two loads per call
unsigned g_sets = 0;

void** VTable(void* obj) { return *static_cast<void***>(obj); }
uint32_t CvarFlags(void* c) { return reinterpret_cast<GetFlagsFn>(VTable(c)[kGetFlags / 8])(c); }
bool CvarIsString(void* c) { return reinterpret_cast<BoolFn>(VTable(c)[kIsVariableString / 8])(c); }
void CvarSet(void* c, const wchar_t* v) { reinterpret_cast<SetFn>(VTable(c)[kSet / 8])(c, v, kSetByProjectSetting); }

// The name once in .rdata (a whole UTF-16 string) and a `lea reg,[rip+name]` in the code that registers it.
bool NameInGame()
{
    if (g_nameChecked) return g_nameOk;
    g_nameChecked = true;
    const GameModule& game = Game();
    const size_t n = sizeof(kName);
    const uint8_t* name = nullptr;
    int copies = 0;
    for (const uint8_t* p = game.rdata.begin; p + n <= game.rdata.end; p += 2)
        if (memcmp(p, kName, n) == 0) { name = p; ++copies; }
    if (copies != 1) {
        Log("[navprio] the name %ls found %d times in the game (expected once): NavQueryPriority unavailable", kName, copies);
        return false;
    }
    int refs = 0;
    for (const uint8_t* p = game.text.begin; p + 7 <= game.text.end; ++p) {  // lea r64,[rip+disp32]: REX.W 8D /r mod 00 rm 101
        if ((p[0] & 0xFB) != 0x48 || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
        if (RipTarget(p, 3, 7) == name) ++refs;
    }
    if (refs < 1) {
        Log("[navprio] no code references the name %ls: NavQueryPriority unavailable", kName);
        return false;
    }
    g_nameOk = true;
    return true;
}

// The variable through the console manager (game thread; SEH-guarded; retried for a while, the manager and the registration
// exist long before the first cell loads).
bool FindCvar()
{
    if (g_cvar) return true;
    if (g_gaveUp) return false;
    __try {
        void** const slot = SeparableSkinConsoleManager();
        void* const manager = slot ? *slot : nullptr;
        if (manager) {
            void* const c = reinterpret_cast<FindCvarFn>(VTable(manager)[kFindVar / 8])(manager, kName, false);
            if (c) {
                if (!CvarIsString(c)) {
                    Log("[navprio] %ls is not a string variable here: NavQueryPriority off", kName);
                    g_gaveUp = true;
                    return false;
                }
                g_cvar = c;
                Log("[navprio] %ls found through the console manager (priority %u)", kName, (CvarFlags(c) & kSetByMask) >> 24);
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[navprio] fault while looking up the console variable: NavQueryPriority off");
        g_gaveUp = true;
        return false;
    }
    if (++g_findTries >= kMaxFindTries) {
        Log("[navprio] the console variable not found: NavQueryPriority off");
        g_gaveUp = true;
    }
    return false;
}

}  // namespace

// Game thread, at every UWorld::AddToWorld call (StreamGranBeforeAdd), before the call does its work.
void NavQueryPriorityOnGameThread()
{
    const int key = g_key.load(std::memory_order_relaxed);
    if (key == g_decidedKey) return;
    if (key == 0 && !g_ours) { g_decidedKey = 0; return; }  // off and nothing of ours in place
    if (g_gaveUp) { g_decidedKey = key; return; }
    const int nameState = g_nameState.load(std::memory_order_acquire);
    if (nameState == 0) return;                             // the monitor thread has not checked the name yet: next call
    if (nameState != 1) { g_gaveUp = true; g_decidedKey = key; return; }
    if (!FindCvar()) return;                                // retried at the next call
    __try {
        const uint32_t pri = CvarFlags(g_cvar) & kSetByMask;
        if (key == 1) {
            if (g_ours) {
                if (pri != kSetByProjectSetting) {          // set by someone else since: theirs
                    g_ours = false;
                    Log("[navprio] %ls was set again (priority %u) after the fix: left alone", kName, pri >> 24);
                }
            } else if (pri == kSetByConstructor || (g_touched && pri == kSetByProjectSetting)) {
                CvarSet(g_cvar, kOn);
                const uint32_t after = CvarFlags(g_cvar) & kSetByMask;
                if (after == kSetByProjectSetting) {
                    g_ours = g_touched = true;
                    ++g_sets;
                    Log("[navprio] NavQueryPriority=1: %ls \"%ls\" -> \"%ls\" (the navigation query task runs on the Foreground and "
                        "Reserve workers instead of waiting for a Background one; set #%u)", kName, kGame, kOn, g_sets);
                } else {
                    Log("[navprio] %ls: Set had no effect (priority %u): left as it is", kName, after >> 24);
                }
            } else if (!g_leftAloneLogged) {
                g_leftAloneLogged = true;
                Log("[navprio] %ls was set at priority %u (Engine.ini, the console or the game's own setting): left alone", kName, pri >> 24);
            }
        } else if (g_ours) {                                 // key switched off live
            g_ours = false;
            if (pri == kSetByProjectSetting) {
                CvarSet(g_cvar, kGame);
                ++g_sets;
                Log("[navprio] NavQueryPriority=0: %ls \"%ls\" put back (set #%u)", kName, kGame, g_sets);
            } else {
                Log("[navprio] NavQueryPriority=0: %ls was changed by someone else (priority %u): left alone", kName, pri >> 24);
            }
        }
        if (key == 0) g_leftAloneLogged = false;
        g_decidedKey = key;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[navprio] fault in the console variable calls: NavQueryPriority off");
        g_gaveUp = true;
    }
}

// Monitor thread, once per second: the key (live); applied by the game thread at the next AddToWorld call.
void SetNavQueryPriority(int value)
{
    if (value != 1) value = 0;
    // The name check here, once, before the game thread may act on the key (it waits for g_nameState).
    if (value == 1 && g_nameState.load(std::memory_order_relaxed) == 0)
        g_nameState.store(NameInGame() ? 1 : 2, std::memory_order_release);
    const int before = g_key.exchange(value, std::memory_order_relaxed);
    static bool logged = false;  // monitor thread only
    if (value == 1 && before != 1 && !logged) {
        logged = true;
        if (StreamGranHookActive())
            Log("[navprio] NavQueryPriority=1: %ls is set to \"%ls\" on the game thread at the next cell load", kName, kOn);
        else
            Log("[navprio] NavQueryPriority=1: not active (needs the AddToWorld hook, installed at start-up on the version.dll route with "
                "this or a streaming key on: see the [streamgran] lines)");
    }
}

// Self-test: the name checks on the mapped exe, and a fake console variable (vtable GetFlags +0x18, IsVariableString +0x48,
// Set +0x80) instead of the console manager's.
bool NavQueryPriorityNameInGame() { return NameInGame(); }

void NavQueryPriorityTestSetup(void* fakeCvar)
{
    g_cvar = fakeCvar;
    g_nameState.store(0);
    g_gaveUp = false;
    g_findTries = 0;
    g_ours = g_touched = g_leftAloneLogged = false;
    g_decidedKey = -1;
    g_sets = 0;
    g_key.store(0);
}

unsigned NavQueryPrioritySets() { return g_sets; }
int NavQueryPriorityNameState() { return g_nameState.load(); }

}  // namespace opf
