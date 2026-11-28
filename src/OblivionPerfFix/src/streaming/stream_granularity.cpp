// StreamRegisterGranularity (worker bodyadd 2026-11-07, review fixes worker granfix 2026-11-07; public since 1.3.0, workers
// port130a/port130c 2026-11-08; [Fixes] StreamRegisterGranularity, default 1 (user 2026-11-07: streamed-in cells may show
// ~0.1-0.3 s later); 0 = the game's 10; start-up hook when != 0, then live 0..9): the engine's own console variable
// s.LevelStreamingComponentsRegistrationGranularity set to the key's value (1..9) outside loading screens, so a
// streamed-in level's components are registered in smaller steps between AddToWorld's time checks.
//
// What it fixes: World Partition levels with many instanced meshes (wooded slopes, rock fields: Kvatch C, the Kvatch road
// north of the city, play1107 +74) are made visible by UWorld::AddToWorld, which is time-sliced (AdaptiveAddToWorld,
// a few ms per frame) but checks the time only between calls of ULevel::IncrementalUpdateComponents. Each call registers
// up to s.LevelStreamingComponentsRegistrationGranularity (10) components of ONE actor (UE 5.3 Level.cpp
// IncrementalRegisterComponents: "return to outer loop after each processed actor"; Actor.cpp
// AActor::IncrementalRegisterComponents: up to NumComponentsToRegister). A foliage actor holds one instanced mesh per
// foliage type, each with ~1000 collision bodies (CreateAllInstanceBodies, ~2 ms each with fixes 28/34), so one call
// registers 4-5 of them: 9-14 ms of game thread in one frame, and the next physics step then takes ~5k new static
// bodies at once (push ~6-10 ms, collision-index inserts ~11 ms): the 40-55 ms frames of play1107b marks 2 and 4.
// With 1, the time check comes after every component, so a frame stops after the first component past its slice.
//
// Why it is equivalent (not identical): the same components are registered in the same order by the same engine code;
// only the frame in which each one is registered changes (stock already spreads a level over several frames; this
// spreads it in smaller steps). Nothing is skipped or held: a level being added keeps being worked on every frame
// (AddToWorld's slice, at least one component per call). A World Partition cell is drawn only once its AddToWorld is
// complete (bRequireFullVisibilityToRender) and cells are made visible one at a time, so a streamed-in cell (and its
// HLOD swap) appears ~0.2 s later in the measured arrivals (445-545 -> 694-738 ms at 60 fps): what stock gives at ~40 fps,
// where the same per-frame slice also covers fewer components per second.
//
// Loading screens are exempt: Altar's loading mode (a door, fast travel, a load) sets s.LevelStreamingActorsUpdateTimeLimit
// to its Altar.Loading.AddToWorldBudget (10 s) while it lasts and puts the old value back after. AddToWorld is still
// called with its time limit on there (the game's block-until-loaded loops call it again and again within a frame), so
// a smaller step would only add per-call overhead (each IncrementalUpdateComponents call also processes the deferred
// physics state): bodyadd measured the door's loading-state hitch 147 -> 298-360 ms with 1. While that time limit is
// 1000 ms or more, the game's 10 is put back; when it drops, the key's value again.
//
// How: one entry detour on UWorld::AddToWorld (game thread; found as the function holding the variable's only read in
// the code, the read found from the variable's registration: lea r8,[value] directly before lea rdx,[name]; its first 7
// bytes checked). Per call with the time limit on, the value the call should see (the game's 10 in loading mode or with
// key 0, else the key) is set through the variable's own IConsoleVariable::Set (vtable +0x80; the console manager's
// FindConsoleVariable as SeparableSkinAbove4K finds it) at SetByProjectSetting, the priority the game's own value has
// (UStreamingSettings), only when it differs (a few calls per loading screen). So the console's / the bench's `get`
// shows the value in use, and anything the player sets at a higher priority (Engine.ini [SystemSettings], the console,
// the bench's -Cvars) is refused by the engine's own priority rule and left alone by us (checked with GetFlags first, no
// warning). A value at the project priority other than 10 and ours (e.g. an Engine.ini [/Script/Engine.StreamingSettings]
// line) is left alone too; an explicit 10 there cannot be told from the game's and is replaced (lab note). The value
// takes effect at the next AddToWorld call: a live key change shows in `get` once a level streams in. (Lab: when the
// AddToWorld detour is installed by BodyAddProbe (both on), its hook calls StreamGranBeforeAdd instead.)
// The monitor thread only passes the key (SetStreamRegisterGranularity); all reads and writes are on the game thread.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace opf {
namespace {

constexpr int32_t kGame = 10;
constexpr uint32_t kSetByMask = 0xFF000000u, kSetByProjectSetting = 0x03000000u;
constexpr size_t kGetFlags = 0x18, kSet = 0x80, kGetInt = 0x90, kGetFloat = 0x98, kFindVar = 0xB8;
constexpr float kLoadingLimitMs = 1000.0f;  // Altar's loading budget is 10000 ms; the game's own slice a few ms
constexpr int kMaxSwitchLogs = 200;
constexpr const char* kAddPrologue = "48 8B C4 48 89 58 10";  // mov rax,rsp; mov [rax+10h],rbx (position-independent)

using GetFlagsFn = uint32_t (*)(void*);
using SetFn = void (*)(void*, const wchar_t*, uint32_t);
using GetIntFn = int32_t (*)(void*);
using GetFloatFn = float (*)(void*);
using FindCvarFn = void* (*)(void* manager, const wchar_t* name, bool trackFrequentCalls);
using Fn8 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

int32_t* g_var = nullptr;
const uint8_t* g_read = nullptr;  // the variable's (only) read in the code
bool g_searched = false;
std::atomic<int> g_key{0};        // monitor thread -> game thread
bool g_active = false;            // the AddToWorld detour is in place (ours or BodyAddProbe's)
bool g_installTried = false;
Fn8 g_add = nullptr;

// game thread only
void* g_gran = nullptr;   // IConsoleVariable* s.LevelStreamingComponentsRegistrationGranularity
void* g_limit = nullptr;  // IConsoleVariable* s.LevelStreamingActorsUpdateTimeLimit
int g_findTries = 0;
bool g_gaveUp = false;
int32_t g_written = 0;    // the value we set last (0: none, or the game's 10 put back)
int g_loggedKey = 0;
bool g_loading = false;
int32_t g_leftAloneLogged = 0;
uint32_t g_leftAlonePriLogged = 0;
int g_switchLogs = 0;
unsigned g_addsLoading = 0, g_addsPlay = 0, g_sets = 0, g_loadings = 0;

// StreamInBudget (worker gatefix 2026-11-12, from worker streambudget's measurements): the two stock per-frame budgets of
// streaming in, s.AdaptiveAddToWorld.AddToWorldTimeSliceMax (the game's 4 ms) and s.AsyncLoadingTimeLimit (the game's
// 5 ms), set lower while the player plays (key 1: 1 / 3 ms, key 2: 2 / 3 ms), the game's values in Altar's loading mode
// and with key 0. Same rules as the granularity: set through IConsoleVariable::Set, only when the variable holds the game's
// value or ours; a value set at the command line or the console, or any other value, is the player's and left alone.
constexpr float kGameSlice = 4.0f, kGameAsync = 5.0f;
constexpr uint32_t kSetByHotfix = 0x08000000u;  // at or above this (hotfix, command line, code, console): not ours to change
std::atomic<int> g_budgetKey{0};
void* g_slice = nullptr;  // IConsoleVariable* s.AdaptiveAddToWorld.AddToWorldTimeSliceMax
void* g_async = nullptr;  // IConsoleVariable* s.AsyncLoadingTimeLimit
bool g_budgetGaveUp = false;
int g_budgetFindTries = 0;
float g_sliceWritten = 0.0f, g_asyncWritten = 0.0f;  // the values we set last (0: none, or the game's put back)
int g_budgetLoggedKey = 0;
unsigned g_budgetSets = 0;
int g_budgetLeftAloneLogs = 0;

void** VTable(void* obj) { return *static_cast<void***>(obj); }
uint32_t CvarFlags(void* c) { return reinterpret_cast<GetFlagsFn>(VTable(c)[kGetFlags / 8])(c); }
int32_t CvarInt(void* c) { return reinterpret_cast<GetIntFn>(VTable(c)[kGetInt / 8])(c); }
float CvarFloat(void* c) { return reinterpret_cast<GetFloatFn>(VTable(c)[kGetFloat / 8])(c); }
void CvarSet(void* c, int32_t v)
{
    wchar_t text[16];
    swprintf(text, 16, L"%d", v);
    reinterpret_cast<SetFn>(VTable(c)[kSet / 8])(c, text, kSetByProjectSetting);
}

int FindWideAll(const Section& s, const wchar_t* text, const uint8_t** out, int max)
{
    const size_t n = (wcslen(text) + 1) * sizeof(wchar_t);
    int found = 0;
    for (const uint8_t* p = s.begin; p + n <= s.end && found < max; p += 2)
        if (memcmp(p, text, n) == 0) out[found++] = p;
    return found;
}

// The two console variables through the console manager (game thread; SEH-guarded; retried for a while).
bool FindCvars()
{
    if (g_gran && g_limit) return true;
    if (g_gaveUp) return false;
    __try {
        void** const slot = SeparableSkinConsoleManager();
        void* const manager = slot ? *slot : nullptr;
        if (manager) {
            const auto find = reinterpret_cast<FindCvarFn>(VTable(manager)[kFindVar / 8]);
            void* const gran = find(manager, L"s.LevelStreamingComponentsRegistrationGranularity", false);
            void* const limit = find(manager, L"s.LevelStreamingActorsUpdateTimeLimit", false);
            if (gran && limit) {
                const int32_t v = CvarInt(gran);
                if (g_var && v != *g_var) {
                    Log("[streamgran] the console variable reads %d, its bound int %d: not the same variable, off", v, *g_var);
                    g_gaveUp = true;
                    return false;
                }
                g_gran = gran;
                g_limit = limit;
                Log("[streamgran] console variables found (granularity %d, priority %u; s.LevelStreamingActorsUpdateTimeLimit %.1f ms)",
                    v, (CvarFlags(gran) & kSetByMask) >> 24, CvarFloat(limit));
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[streamgran] fault while looking up the console variables: off");
        g_gaveUp = true;
        return false;
    }
    if (++g_findTries >= 2000) {
        Log("[streamgran] the console variables not found: off");
        g_gaveUp = true;
    }
    return false;
}

// StreamInBudget: its two console variables (game thread; SEH-guarded; retried for a while)
bool FindBudgetCvars()
{
    if (g_slice && g_async) return true;
    if (g_budgetGaveUp) return false;
    __try {
        void** const slot = SeparableSkinConsoleManager();
        void* const manager = slot ? *slot : nullptr;
        if (manager) {
            const auto find = reinterpret_cast<FindCvarFn>(VTable(manager)[kFindVar / 8]);
            void* const slice = find(manager, L"s.AdaptiveAddToWorld.AddToWorldTimeSliceMax", false);
            void* const async = find(manager, L"s.AsyncLoadingTimeLimit", false);
            if (slice && async) {
                g_slice = slice;
                g_async = async;
                Log("[streambudget] console variables found: s.AdaptiveAddToWorld.AddToWorldTimeSliceMax %.2f ms (priority %u), "
                    "s.AsyncLoadingTimeLimit %.2f ms (priority %u)", CvarFloat(slice), (CvarFlags(slice) & kSetByMask) >> 24,
                    CvarFloat(async), (CvarFlags(async) & kSetByMask) >> 24);
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[streambudget] fault while looking up the console variables: off");
        g_budgetGaveUp = true;
        return false;
    }
    if (++g_budgetFindTries >= 2000) {
        Log("[streambudget] the console variables not found: off");
        g_budgetGaveUp = true;
    }
    return false;
}

bool Near(float a, float b) { return a - b < 0.001f && b - a < 0.001f; }

// One variable: the value the game should see now (want), set only over the game's value or ours, at the variable's own
// priority (at least the project's) unless that is the player's (hotfix, command line, code, console)
void BudgetVar(void* c, float game, float want, float& written, const char* name)
{
    const float now = CvarFloat(c);
    if (Near(now, want)) return;
    const uint32_t pri = CvarFlags(c) & kSetByMask;
    const bool ours = written != 0.0f && Near(now, written);
    if (pri >= kSetByHotfix || (!ours && !Near(now, game))) {
        if (g_budgetLeftAloneLogs < 20) {
            ++g_budgetLeftAloneLogs;
            Log("[streambudget] %s is %.2f (priority %u; not the game's %.1f, not ours): left alone", name, now, pri >> 24, game);
        }
        written = 0.0f;
        return;
    }
    wchar_t text[24];
    swprintf(text, 24, L"%.3f", want);
    reinterpret_cast<SetFn>(VTable(c)[kSet / 8])(c, text, pri > kSetByProjectSetting ? pri : kSetByProjectSetting);
    ++g_budgetSets;
    written = Near(want, game) ? 0.0f : want;
    if (g_budgetSets <= 20 || (g_budgetSets % 100) == 0)
        Log("[streambudget] %s %.2f -> %.2f ms (set #%u)", name, now, CvarFloat(c), g_budgetSets);
}

void BudgetApply(int key, bool loading)
{
    if (!FindBudgetCvars()) return;
    if (key != g_budgetLoggedKey) {
        g_budgetLoggedKey = key;
        Log("[streambudget] StreamInBudget=%d (AddToWorld slice max %.2f ms, async loading limit %.2f ms now)", key,
            CvarFloat(g_slice), CvarFloat(g_async));
    }
    const bool game = key == 0 || loading;
    BudgetVar(g_slice, kGameSlice, game ? kGameSlice : key == 1 ? 1.0f : 2.0f, g_sliceWritten,
              "s.AdaptiveAddToWorld.AddToWorldTimeSliceMax");
    BudgetVar(g_async, kGameAsync, game ? kGameAsync : 3.0f, g_asyncWritten, "s.AsyncLoadingTimeLimit");
}

uint64_t AddHook(uint64_t world, uint64_t level, uint64_t xform, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
{
    StreamGranBeforeAdd((a4 & 0xFF) != 0);
    // StreamInHeavySkipMs: the frame is already long; the level being made visible goes on next frame (AddToWorld returns void)
    if (StreamHeavySkipBeforeAdd(reinterpret_cast<const void*>(world), reinterpret_cast<const void*>(level), (a4 & 0xFF) != 0))
        return 0;
    GcPrepAheadNoteAdd(reinterpret_cast<const void*>(level));  // GcPrepAhead's "wanted again" counter (no-op unless it prepared levels)
    const bool gc = GcPassAfterBusyWatching(), water = WaterCrossingWatching();
    if (!gc && !water) return g_add(world, level, xform, a4, a5, a6, a7, a8);
    // GcPassAfterBusy / WaterInfoAfterCrossing: a cell is being added to the world when this call did work (its own time)
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const uint64_t r = g_add(world, level, xform, a4, a5, a6, a7, a8);
    QueryPerformanceCounter(&t1);
    if (gc) GcPassAfterBusyNoteAdd(t1.QuadPart - t0.QuadPart);
    if (water) WaterCrossingNoteAdd(t1.QuadPart - t0.QuadPart);
    return r;
}

}  // namespace

int32_t* FindStreamRegisterGranularityVar()
{
    if (g_searched) return g_var;
    g_searched = true;
    const GameModule& game = Game();
    const uint8_t* names[4] = {};
    const int nNames = FindWideAll(game.rdata, L"s.LevelStreamingComponentsRegistrationGranularity", names, 4);
    if (!nNames) { Log("[streamgran] the name s.LevelStreamingComponentsRegistrationGranularity not found: off"); return nullptr; }
    // lea r8,[value]; mov [rsp+20h],flags; lea rdx,[name]  or (build 19115871) lea r8,[value]; lea rdx,[name]
    const Pattern reg = ParsePattern("4C 8D 05 ?? ?? ?? ?? C7 44 24 20 ?? ?? ?? ?? 48 8D 15");
    const Pattern regNear = ParsePattern("4C 8D 05 ?? ?? ?? ??");
    int32_t* var = nullptr;
    int regs = 0;
    for (uint8_t* p = game.text.begin + 15; p + 7 <= game.text.end; ++p) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15) continue;  // lea rdx,[name]
        const uint8_t* const t = RipTarget(p, 3, 7);
        bool isName = false;
        for (int i = 0; i < nNames; ++i) isName = isName || t == names[i];
        if (!isName) continue;
        ++regs;
        if (MatchesAt(p - 15, reg)) var = reinterpret_cast<int32_t*>(RipTarget(p - 15, 3, 7));
        else if (MatchesAt(p - 7, regNear)) var = reinterpret_cast<int32_t*>(RipTarget(p - 7, 3, 7));  // this build
    }
    if (regs != 1 || !var || !game.data.Contains(reinterpret_cast<uint8_t*>(var))) {
        Log("[streamgran] the variable's registration not as expected (%d): off", regs);
        return nullptr;
    }
    // a read of the value in the code: mov r32,[rip+value] (optional REX 40-47 except W, 8B /r, mod 00 rm 101)
    int reads = 0;
    const uint8_t* read = nullptr;
    for (uint8_t* p = game.text.begin; p + 7 <= game.text.end; ++p) {
        const uint8_t* q = p;
        if ((*q & 0xF8) == 0x40) ++q;  // REX without W
        if (q[0] != 0x8B || (q[1] & 0xC7) != 0x05) continue;
        int32_t rel;
        memcpy(&rel, q + 2, 4);
        if (q + 6 + rel == reinterpret_cast<uint8_t*>(var)) { ++reads; read = p; }
    }
    if (reads != 1) { Log("[streamgran] %d reads of the variable in the code (expected 1, UWorld::AddToWorld): off", reads); return nullptr; }
    g_var = var;
    g_read = read;
    Log("[streamgran] s.LevelStreamingComponentsRegistrationGranularity at exe+0x%X (now %d; its read at exe+0x%X)",
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(var) - game.base), *var, static_cast<unsigned>(read - game.base));
    return var;
}

// UWorld::AddToWorld: the function holding the variable's only read, its first bytes checked unless already hooked
// (also on the mapped exe).
uint8_t* FindAddToWorld(bool checkPrologue)
{
    if (!FindStreamRegisterGranularityVar() || !g_read) return nullptr;
    const GameModule& game = Game();
    uint8_t* const fn = PrimaryFunction(game, g_read);
    if (!fn || (checkPrologue && !MatchesAt(fn, ParsePattern(kAddPrologue)))) {
        Log("[streamgran] UWorld::AddToWorld (the function of the read) not as expected: off");
        return nullptr;
    }
    return fn;
}

// Start-up (version.dll route) when the key is on: the AddToWorld entry detour, or BodyAddProbe's hook calls us.
bool InstallStreamRegisterGranularity(bool beforeGameCode, const uint8_t* probeAdd)
{
    g_installTried = true;
    if (!beforeGameCode) { Log("[streamgran] StreamRegisterGranularity: start-up only (loaded late here), not installed"); return false; }
    uint8_t* const fn = FindAddToWorld(probeAdd == nullptr);
    if (!fn) return false;
    StreamHeavySkipInstallFields(fn);  // StreamInHeavySkipMs: UWorld's fields from AddToWorld's code (before the entry is patched)
    if (probeAdd) {
        if (probeAdd != fn) { Log("[streamgran] BodyAddProbe hooked another function than AddToWorld: not installed"); return false; }
        g_active = true;
        Log("[streamgran] StreamRegisterGranularity: through BodyAddProbe's AddToWorld hook (exe+0x%X)",
            static_cast<unsigned>(fn - Game().base));
        return true;
    }
    const Pattern pro = ParsePattern(kAddPrologue);
    const size_t n = pro.bytes.size();
    uint8_t* const slot = AllocNearGame(96);
    if (!slot) { Log("[streamgran] no memory near the game's code: not installed"); return false; }
    // trampoline: the first n bytes, then a jump back; at +64 the jump to the hook
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + n;
    void* const hook = reinterpret_cast<void*>(&AddHook);
    memcpy(slot, fn, n);
    memcpy(slot + n, jmpAbs, 6);
    memcpy(slot + n + 6, &resume, 8);
    memcpy(slot + 64, jmpAbs, 6);
    memcpy(slot + 64 + 6, &hook, 8);
    DWORD old;
    VirtualProtect(slot, 96, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, 96);
    g_add = reinterpret_cast<Fn8>(slot);
    const int64_t rel = (slot + 64) - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) { Log("[streamgran] the hook is out of reach: not installed"); return false; }
    uint8_t code[8];
    memset(code, 0x90, sizeof code);
    code[0] = 0xE9;
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(code + 1, &rel32, 4);
    if (!WriteCode(fn, std::span<const uint8_t>(code, n))) { Log("[streamgran] patch failed: not installed"); return false; }
    g_active = true;
    Log("[streamgran] StreamRegisterGranularity: AddToWorld hooked (exe+0x%X); the game's 10 while the loading mode's time "
        "limit (>= %.0f ms) is set", static_cast<unsigned>(fn - Game().base), kLoadingLimitMs);
    return true;
}

// Game thread: Altar's loading mode (its loading budget in s.LevelStreamingActorsUpdateTimeLimit, as StreamGranBeforeAdd reads
// it): 1 on, 0 off, -1 unknown (no AddToWorld hook of ours, or the console variable not found yet / given up). WaterInfoAfterCrossing.
// It never looks the variable up itself (that would spend FindCvars' tries every frame): it reads what the AddToWorld hook found.
int StreamLoadingModeNow()
{
    if (!g_active || !g_limit) return -1;
    __try {
        return CvarFloat(g_limit) >= kLoadingLimitMs ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Game thread, at every UWorld::AddToWorld call (before the call reads the variable).
void StreamGranBeforeAdd(bool considerTimeLimit)
{
    if (!g_active) return;
    NavQueryPriorityOnGameThread();  // NavQueryPriority (npcs/nav_query_priority.cpp): two loads per call once settled
    PhysicsSubstepCapOnGameThread(); // PhysicsSubstepCap (physics/physics_substep_cap.cpp): two loads per call once settled
    if (!considerTimeLimit) return;  // without the time limit the call registers everything (granularity 0)
    const int key = g_key.load(std::memory_order_relaxed);
    const int budgetKey = g_budgetKey.load(std::memory_order_relaxed);
    const bool granWork = !(key == 0 && g_written == 0);  // off and nothing of ours in place
    const bool budgetWork = !(budgetKey == 0 && g_sliceWritten == 0.0f && g_asyncWritten == 0.0f);
    if (!granWork && !budgetWork) return;
    if (!FindCvars()) return;
    __try {
        if (budgetWork) BudgetApply(budgetKey, CvarFloat(g_limit) >= kLoadingLimitMs);  // StreamInBudget
        if (!granWork) return;
        const bool loading = CvarFloat(g_limit) >= kLoadingLimitMs;
        if (loading != g_loading) {
            g_loading = loading;
            if (loading) ++g_loadings;
            if (g_switchLogs < kMaxSwitchLogs) {
                ++g_switchLogs;
                Log("[streamgran] loading mode %s (s.LevelStreamingActorsUpdateTimeLimit %.0f ms; adds so far: %u in play, %u in "
                    "loading mode)", loading ? "on: the game's 10" : "off: the key's value again", CvarFloat(g_limit), g_addsPlay,
                    g_addsLoading);
            }
        }
        ++(loading ? g_addsLoading : g_addsPlay);
        const int32_t want = (key == 0 || loading) ? kGame : key;
        const int32_t now = CvarInt(g_gran);
        if (key != g_loggedKey) {
            g_loggedKey = key;
            Log("[streamgran] StreamRegisterGranularity=%d (s.LevelStreamingComponentsRegistrationGranularity now %d)", key, now);
        }
        if (now == want) return;
        const uint32_t pri = CvarFlags(g_gran) & kSetByMask;
        if (pri > kSetByProjectSetting) {  // the player's (Engine.ini [SystemSettings], console, -Cvars): theirs
            if (now != g_leftAloneLogged || pri != g_leftAlonePriLogged) {
                g_leftAloneLogged = now;
                g_leftAlonePriLogged = pri;
                Log("[streamgran] s.LevelStreamingComponentsRegistrationGranularity is %d, set at priority %u (the player's): left alone",
                    now, pri >> 24);
            }
            g_written = 0;
            return;
        }
        if (now != kGame && now != g_written) {  // a project-priority value that is neither the game's nor ours
            if (now != g_leftAloneLogged || pri != g_leftAlonePriLogged) {
                g_leftAloneLogged = now;
                g_leftAlonePriLogged = pri;
                Log("[streamgran] s.LevelStreamingComponentsRegistrationGranularity is %d (not the game's 10, not ours): left alone", now);
            }
            return;
        }
        CvarSet(g_gran, want);
        ++g_sets;
        g_written = (want == kGame) ? 0 : want;
        if (g_sets <= 20 || (g_sets % 100) == 0)
            Log("[streamgran] s.LevelStreamingComponentsRegistrationGranularity %d -> %d (%s; set #%u)", now, CvarInt(g_gran),
                loading ? "loading mode" : key ? "the key" : "key 0: the game's", g_sets);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[streamgran] fault in the console variable calls: off");
        g_active = false;
    }
}

// NavQueryPriority (npcs/nav_query_priority.cpp): is the AddToWorld hook in place (monitor thread; for its log line)
bool StreamGranHookActive() { return g_active; }

// Monitor thread, once per second: the key (live); applied by the game thread at the next AddToWorld call.
void SetStreamRegisterGranularity(int value)
{
    if (value < 0 || value > 9) value = 0;
    const int before = g_key.exchange(value, std::memory_order_relaxed);
    if (value != 0 && !g_active && before != value) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("[streamgran] StreamRegisterGranularity=%d: not active (the AddToWorld hook is installed at start-up with the "
                "key on; %s)", value, g_installTried ? "it was not: see the lines above" : "restart the game");
        }
    }
}

// Monitor thread, once per second: StreamInBudget (live; 0 off, 1 = slice 1 / async 3 ms, 2 = slice 2 / async 3 ms); applied
// by the game thread at the next AddToWorld call (the AddToWorld hook is installed at start-up when either key is on).
void SetStreamInBudget(int value)
{
    if (value < 0 || value > 2) value = 0;
    const int before = g_budgetKey.exchange(value, std::memory_order_relaxed);
    if (value != 0 && !g_active && before != value) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("[streambudget] StreamInBudget=%d: not active (needs the AddToWorld hook, installed at start-up with this key or "
                "StreamRegisterGranularity on; %s)", value, g_installTried ? "it was not: see the [streamgran] lines" : "restart the game");
        }
    }
}

void StreamBudgetTestSetup(void* slice, void* async)
{
    g_slice = slice;
    g_async = async;
    g_sliceWritten = g_asyncWritten = 0.0f;
    g_budgetKey.store(0);
    g_budgetLoggedKey = 0;
    g_budgetGaveUp = false;
    g_budgetFindTries = 0;
}

// StreamInHeavySkipMs (worker heavyskip 2026-11-23, design: option B of the ismdesign study; review fixes S1/S2/N1-N8
// the same day; S-028; [Fixes] StreamInHeavySkipMs, public default 0, live; hidden
// StreamInHeavySkipMax 8). A streaming cell is made visible by UWorld::AddToWorld, called again every frame while the level is
// in MakingVisible (UE 5.3 LevelStreaming.cpp UpdateStreamingState), each call doing one time slice (StreamInBudget: 1 or 2
// ms). The slice does not know how long the frame already is: after a big physics step (the acceleration-structure swap, the
// push of the last call's statics) the next slice (a foliage component's ~2000 bodies, a nav chunk attach, render proxies)
// lands on a frame already past 17 ms.
// The rule: only the call for the level that is ALREADY being made visible (UWorld::CurrentLevelPendingVisibility, set by an
// earlier call: "begin making visible", World.cpp 3150-3166) can be left for the next frame, and only its first call in a
// frame, when the game thread is already >= max(key, median of recent such frames' values + 4) ms into the frame. A call
// while no level is pending (it may begin one) and a call for any other level (a no-op in stock while one is pending,
// World.cpp 3107) go to the game unchanged. Limits: never two such frames in a row; at most StreamInHeavySkipMax skipped
// frames for one pending level (the count starts afresh whenever another level, or none, is seen pending at a call); never
// a call whose time limit stock turns off (bConsiderTimeLimit false, or !bMatchStarted, or not a game world: World.cpp
// 3121, read as AddToWorld itself reads them), never in Altar's loading mode (s.LevelStreamingActorsUpdateTimeLimit >=
// 1000 ms, as above); a second call for the pending level in the same frame (the engine's FlushLevelStreaming /
// BlockTillLevelStreamingCompleted loops call UpdateLevelStreaming again within one frame) goes on.
// Why the blocking loops are unchanged: a level stays pending while it is skipped, so IsVisibilityRequestPending() stays
// true (World.cpp 4606) and every loop that tests it runs its next pass, in which the pending level's call is the repeat
// and goes to the game. BlockTillLevelStreamingCompleted (World.cpp 4155-4171, also the path of World Partition's
// wp.Runtime.BlockOnSlowStreaming via bRequestedBlockOnAsyncLoading, GameEngine.cpp 1808): its first pass may be the
// skipped call; bWorkToDo is then true (a level pending), so it enters FlushLevelStreaming(Full), as stock does whenever the
// pending level needs more than that one call or anything is async-loading; FlushAsyncLoading there has nothing to wait for
// if nothing is loading, and its visibility loop gives the pending level its calls until nothing is pending: the block's
// end state (nothing pending, check at World.cpp 4593) is stock's. FlushLevelStreaming(Visibility) (travel, teardown): its
// first TickLevelStreaming may be the skip, its while(IsVisibilityRequestPending) loop then runs the repeat. A block at
// match start (UWorldPartition::OnWorldMatchStarting) runs with bMatchStarted false: never skipped.
// Why equivalent (not identical): stock never gives the pending level a call without progress (each step runs before its
// time check, World.cpp 3176-3186); a skip is a one-frame pause of that level's add. Everything else in the skip frame is
// what stock does while that level is pending (other levels wait, incremental removals wait: World.cpp 3529); the same
// components are created by the game's own code in the game's order, the level finishes 1..Max frames later (more with
// StreamInBudget 2: FAdaptiveAddToWorld restarts its slice from TimeSliceMin after a frame without registered work,
// World.cpp 297-300); the next level is picked by the game when this one finishes (WP re-sorts every frame, so it may be
// another one than without the pause, as with a slower slice in stock). UWorld's three fields (the pending level, bMatchStarted's
// bit, WorldType) are read from AddToWorld's own instructions (byte patterns, each found exactly once; else the skip is not
// available, one log line). Frame clock: NpcCreationPacing's frame edge (NpcPacingFrameClock); without it, off (one log
// line). Only while StreamInBudget is 1 or 2 (one log line otherwise).
namespace {
constexpr int kHsRing = 64;
constexpr size_t kHsSearch = 0x300;  // AddToWorld's entry block (the three reads are at +0x69 .. +0x12C on Steam 19115871)
// The step runs only when the pending level is this level or none (World.cpp 3107): the
// field's load (mov rcx,[r13+disp32]), StartTime's conversion, then cmp rcx,r14 / je / test rcx,rcx / je / xor bl,bl / mov bl,1
constexpr const char* kHsPending = "49 8B 8D ?? ?? ?? ?? 45 0F 57 C0 F2 44 0F 10 15 ?? ?? ?? ?? F2 4C 0F 2A 45 ?? F2 44 0F 59 05 "
                                   "?? ?? ?? ?? F2 45 0F 58 C2 49 3B CE 74 ?? 48 85 C9 74 ?? 32 DB EB 02 B3 01";
// bIsGameWorld = IsGameWorld(): movzx eax,byte [r13+WorldType]; cmp al,6; ja; mov edx,0x6A (Game, PIE, GamePreview, GameRPC); bt
constexpr const char* kHsType = "41 0F B6 85 ?? ?? ?? ?? 3C 06 77 ?? BA 6A 00 00 00 0F A3 C2 73 ?? 40 B6 01";
// bConsiderTimeLimit &= bMatchStarted && bIsGameWorld (World.cpp 3121): test byte [r13+x],bit; je; test sil,sil; jne; xor r15b,r15b
constexpr const char* kHsMatch = "41 F6 85 ?? ?? ?? ?? ?? 74 ?? 40 84 F6 75 ?? 45 32 FF";

struct HsState {
    int keyMs = 0, maxPerLevel = 8;
    const void* pending = nullptr;  // the level pending visibility as last seen at a call's entry
    int pendingSkips = 0;           // frames this pending level's add was paused (= frames of delay)
    uint64_t frameStart = 0;        // the frame of the last decision (its start, us)
    bool frameDecided = false, frameSkipped = false, frameRepeatNoted = false, prevSkipped = false;
    float ring[kHsRing] = {};
    int ringN = 0, ringNext = 0;
    bool noClockLogged = false, noBudgetLogged = false, noLimitLogged = false;
    unsigned noClockLogs = 0;
};
HsState hs;
// counters: written on the game thread, read by the stats line on the monitor thread (atomics, relaxed)
struct HsStats {
    std::atomic<unsigned> skipFrames{0}, declinedInRow{0}, declinedCap{0}, declinedRepeat{0}, fromMedian{0}, levels{0};
    std::atomic<int> maxPerLevel{0};          // since the last report (the monitor exchanges it to 0)
    std::atomic<uint64_t> elapsedSumUs{0};    // since the last report: elapsed at the skips, for the mean
    std::atomic<unsigned> elapsedN{0};
};
HsStats hst;
std::atomic<int> g_hsKey{0}, g_hsMax{8};
StreamHeavySkipFields g_hsf;  // found at install (InstallStreamRegisterGranularity)

void HsStatsReset()
{
    for (std::atomic<unsigned>* a : {&hst.skipFrames, &hst.declinedInRow, &hst.declinedCap, &hst.declinedRepeat, &hst.fromMedian,
                                     &hst.levels, &hst.elapsedN})
        a->store(0, std::memory_order_relaxed);
    hst.maxPerLevel.store(0, std::memory_order_relaxed);
    hst.elapsedSumUs.store(0, std::memory_order_relaxed);
}

void HsAdd(std::atomic<unsigned>& a) { a.store(a.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }  // one writer

float HsMedian()
{
    if (hs.ringN == 0) return 0.0f;
    float v[kHsRing];
    memcpy(v, hs.ring, sizeof(float) * hs.ringN);
    std::nth_element(v, v + hs.ringN / 2, v + hs.ringN);
    return v[hs.ringN / 2];
}

// The rule (game thread; the self-test drives it directly). pending = UWorld::CurrentLevelPendingVisibility at the call's
// entry; timeLimit = the call's bConsiderTimeLimit after stock's own "&= bMatchStarted && bIsGameWorld".
bool HsDecide(const void* level, const void* pending, bool timeLimit, bool loading, bool clockOk, uint64_t frameStartUs,
              double elapsedMs)
{
    if (pending != hs.pending) {  // another level (or none) in progress: a fresh count (pointer reuse can only inherit
        hs.pending = pending;     // skips when the observing calls were filtered: then it skips less)
        hs.pendingSkips = 0;
    }
    if (hs.keyMs <= 0 || !timeLimit || loading) return false;
    if (!pending || pending != level) return false;  // S1: may begin a level / a no-op in stock: the game's way
    if (!clockOk) {
        if (!hs.noClockLogged) {
            hs.noClockLogged = true;
            ++hs.noClockLogs;
            Log("[streambudget] heavy-frame skip off: no frame clock (needs NpcCreationPacing)");
        }
        return false;
    }
    if (frameStartUs != hs.frameStart) {  // a new frame: the last one's decision becomes "the previous frame"
        hs.prevSkipped = hs.frameSkipped;
        hs.frameStart = frameStartUs;
        hs.frameDecided = hs.frameSkipped = hs.frameRepeatNoted = false;
    } else if (hs.frameDecided) {  // the pending level called again in this frame (a flush / block loop's next pass): goes on
        if (hs.frameSkipped && !hs.frameRepeatNoted) {
            hs.frameRepeatNoted = true;
            HsAdd(hst.declinedRepeat);
        }
        return false;
    }
    hs.frameDecided = true;
    const float median = HsMedian();
    const float threshold = std::max(static_cast<float>(hs.keyMs), hs.ringN ? median + 4.0f : 0.0f);
    hs.ring[hs.ringNext] = static_cast<float>(elapsedMs);
    hs.ringNext = (hs.ringNext + 1) % kHsRing;
    if (hs.ringN < kHsRing) ++hs.ringN;
    if (elapsedMs < threshold) return false;
    if (hs.prevSkipped) { HsAdd(hst.declinedInRow); return false; }
    if (hs.pendingSkips >= hs.maxPerLevel) { HsAdd(hst.declinedCap); return false; }
    hs.frameSkipped = true;
    if (hs.pendingSkips++ == 0) HsAdd(hst.levels);
    if (hs.pendingSkips > hst.maxPerLevel.load(std::memory_order_relaxed))
        hst.maxPerLevel.store(hs.pendingSkips, std::memory_order_relaxed);
    HsAdd(hst.skipFrames);
    if (threshold > static_cast<float>(hs.keyMs)) HsAdd(hst.fromMedian);
    hst.elapsedSumUs.fetch_add(static_cast<uint64_t>(elapsedMs * 1000.0), std::memory_order_relaxed);
    HsAdd(hst.elapsedN);
    return true;
}

// Stats interval (monitor thread): the heavy-frame skip's line, only when something changed
void StreamHeavySkipLogStats()
{
    static unsigned lastFrames = 0, lastInRow = 0, lastCap = 0, lastRepeat = 0, lastLevels = 0, lastMedian = 0;
    const unsigned frames = hst.skipFrames.load(std::memory_order_relaxed), inRow = hst.declinedInRow.load(std::memory_order_relaxed),
                   cap = hst.declinedCap.load(std::memory_order_relaxed), repeat = hst.declinedRepeat.load(std::memory_order_relaxed),
                   levels = hst.levels.load(std::memory_order_relaxed), median = hst.fromMedian.load(std::memory_order_relaxed);
    if (frames == lastFrames && inRow == lastInRow && cap == lastCap && repeat == lastRepeat) return;
    const int maxPer = hst.maxPerLevel.exchange(0, std::memory_order_relaxed);
    const uint64_t sumUs = hst.elapsedSumUs.exchange(0, std::memory_order_relaxed);
    const unsigned n = hst.elapsedN.exchange(0, std::memory_order_relaxed);
    Log("[streambudget] heavy-frame skips since last report: %u frames (one call each, the level being made visible; levels %u, "
        "most skips of one level %d; elapsed at skip mean %.1f ms; threshold from the median + 4 in %u); heavy but not skipped: "
        "%u after a skip, %u at the per-level cap; %u blocking-loop passes went on after a skip", frames - lastFrames,
        levels - lastLevels, maxPer, n ? sumUs / 1000.0 / n : 0.0, median - lastMedian, inRow - lastInRow, cap - lastCap,
        repeat - lastRepeat);
    lastFrames = frames;
    lastInRow = inRow;
    lastCap = cap;
    lastRepeat = repeat;
    lastLevels = levels;
    lastMedian = median;
}

uint32_t Disp32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

// The call's world as AddToWorld reads it at its entry: the pending level, and bConsiderTimeLimit's "&= bMatchStarted &&
// bIsGameWorld" (World.cpp 3121; IsGameWorld = WorldType Game, PIE, GamePreview or GameRPC, AddToWorld's own bit mask 0x6A).
// false = the read faulted (no skip).
bool HsReadWorld(const void* world, const void** pending, bool* matchAndGame)
{
    __try {
        const uint8_t* const w = static_cast<const uint8_t*>(world);
        *pending = *reinterpret_cast<const void* const*>(w + g_hsf.pending);
        const uint8_t type = w[g_hsf.worldType];
        *matchAndGame = type <= 6 && ((0x6Au >> type) & 1u) && (w[g_hsf.matchStarted] & g_hsf.matchMask) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

// UWorld's three fields from AddToWorld's own instructions (install; the self-test on the mapped exe). Each pattern must be
// found exactly once in the function's first kHsSearch bytes, and the pending field is cross-checked against the "begin
// making visible" block (cmp [r13+pending],0 / cmp [r13+pending+8],r14 = CurrentLevelPendingInvisibility / mov [r13+pending],r14).
bool StreamHeavySkipFindFields(const uint8_t* addToWorld, size_t searchBytes, StreamHeavySkipFields* out)
{
    *out = StreamHeavySkipFields{};
    if (!addToWorld) return false;
    const size_t n = std::min(searchBytes, kHsSearch);
    auto once = [&](const char* text, const uint8_t** at) {
        const Pattern p = ParsePattern(text);
        int hits = 0;
        for (size_t i = 0; i + p.size() <= n; ++i)
            if (MatchesAt(addToWorld + i, p)) { *at = addToWorld + i; ++hits; }
        return hits == 1;
    };
    const uint8_t *pend = nullptr, *type = nullptr, *match = nullptr;
    if (!once(kHsPending, &pend) || !once(kHsType, &type) || !once(kHsMatch, &match)) return false;
    const uint32_t pending = Disp32(pend + 3), worldType = Disp32(type + 4), matchByte = Disp32(match + 3);
    const uint8_t mask = match[7];
    if (pending < 0x28 || pending > 0x1000 || worldType > 0x2000 || matchByte > 0x2000 || !mask || (mask & (mask - 1))) return false;
    // the begin block's reads and write of the same field
    uint8_t cmp0[8] = {0x49, 0x83, 0xBD, 0, 0, 0, 0, 0x00}, cmpInv[7] = {0x4D, 0x39, 0xB5}, store[7] = {0x4D, 0x89, 0xB5};
    memcpy(cmp0 + 3, &pending, 4);
    const uint32_t inv = pending + 8;
    memcpy(cmpInv + 3, &inv, 4);
    memcpy(store + 3, &pending, 4);
    bool c1 = false, c2 = false, c3 = false;
    for (size_t i = 0; i + 8 <= n; ++i) {
        c1 = c1 || memcmp(addToWorld + i, cmp0, 8) == 0;
        c2 = c2 || memcmp(addToWorld + i, cmpInv, 7) == 0;
        c3 = c3 || memcmp(addToWorld + i, store, 7) == 0;
    }
    if (!c1 || !c2 || !c3) return false;
    out->pending = pending;
    out->worldType = worldType;
    out->matchStarted = matchByte;
    out->matchMask = mask;
    out->ok = true;
    return true;
}

// Install (game thread not running yet): find the fields once; StreamInHeavySkipMs is not available without them.
void StreamHeavySkipInstallFields(const uint8_t* addToWorld)
{
    if (StreamHeavySkipFindFields(addToWorld, kHsSearch, &g_hsf))
        Log("[streambudget] StreamInHeavySkipMs: UWorld fields from AddToWorld's code: CurrentLevelPendingVisibility +0x%X, "
            "WorldType +0x%X, bMatchStarted +0x%X bit 0x%X", g_hsf.pending, g_hsf.worldType, g_hsf.matchStarted, g_hsf.matchMask);
    else
        Log("[streambudget] StreamInHeavySkipMs: UWorld's pending-visibility field not found in AddToWorld (code not as "
            "expected): the heavy-frame skip is not available (the key does nothing)");
}

// Monitor thread, once per second: the key (live). 0 off, 10..100 ms; out of range = off. Max 1..30 (else 8).
void SetStreamInHeavySkip(int ms, int maxPerLevel)
{
    if (ms != 0 && (ms < 10 || ms > 100)) ms = 0;
    if (maxPerLevel < 1 || maxPerLevel > 30) maxPerLevel = 8;
    const int before = g_hsKey.exchange(ms, std::memory_order_relaxed);
    g_hsMax.store(maxPerLevel, std::memory_order_relaxed);
    if (ms != before)
        Log("[streambudget] StreamInHeavySkipMs=%d (max %d skipped frames per level)%s", ms, maxPerLevel,
            ms && !g_active ? ": not active (needs the AddToWorld hook, installed at start-up with StreamInBudget on)"
            : ms && !g_hsf.ok ? ": not active (UWorld's pending-visibility field was not found)" : "");
}

// Game thread, every AddToWorld call (after StreamGranBeforeAdd, which applies StreamInBudget and finds the loading mode's
// variable): true = return without calling the game. world = the call's UWorld (this).
bool StreamHeavySkipBeforeAdd(const void* world, const void* level, bool considerTimeLimit)
{
    const int key = g_hsKey.load(std::memory_order_relaxed);
    if (key != hs.keyMs) {
        hs.keyMs = key;
        hs.noBudgetLogged = hs.noLimitLogged = false;
    }
    hs.maxPerLevel = g_hsMax.load(std::memory_order_relaxed);
    if (key == 0 || !g_active || !g_hsf.ok || !considerTimeLimit || !world) return false;
    const int budget = g_budgetKey.load(std::memory_order_relaxed);
    if (budget != 1 && budget != 2) {
        if (!hs.noBudgetLogged) {
            hs.noBudgetLogged = true;
            Log("[streambudget] heavy-frame skip off: needs StreamInBudget 1 or 2 (it is %d)", budget);
        }
        return false;
    }
    if (!g_limit) {  // the loading mode cannot be read (yet): never skip
        if (!hs.noLimitLogged) {
            hs.noLimitLogged = true;
            Log("[streambudget] heavy-frame skip: s.LevelStreamingActorsUpdateTimeLimit not found (yet): no skip until it is");
        }
        return false;
    }
    bool loading = true, timeLimit = false;
    const void* pending = nullptr;
    __try {
        loading = CvarFloat(g_limit) >= kLoadingLimitMs;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!HsReadWorld(world, &pending, &timeLimit)) return false;
    uint64_t startUs = 0, elapsedUs = 0;
    const bool clock = NpcPacingFrameClock(&startUs, &elapsedUs);
    const bool skip = HsDecide(level, pending, timeLimit, loading, clock, startUs, elapsedUs / 1000.0);
    if (!skip) return false;
    // a paused add is still a cell being added (GcPassAfterBusy and WaterInfoAfterCrossing count it as streaming)
    if (GcPassAfterBusyWatching()) GcPassAfterBusyNoteStream(0);
    if (WaterCrossingWatching()) WaterCrossingNoteStream();
    return true;
}

void StreamHeavySkipTestReset(int ms, int maxPerLevel)
{
    hs = HsState{};
    hs.keyMs = ms;
    hs.maxPerLevel = maxPerLevel;
    HsStatsReset();
}

bool StreamHeavySkipTestCall(const void* level, const void* pending, bool timeLimit, bool loading, bool clockOk,
                             uint64_t frameStartUs, double elapsedMs)
{
    return HsDecide(level, pending, timeLimit, loading, clockOk, frameStartUs, elapsedMs);
}

// self-test: set (or clear, nullptr) the fields as if found at install; the entry's world read
void StreamHeavySkipTestFields(const StreamHeavySkipFields* f) { g_hsf = f ? *f : StreamHeavySkipFields{}; }
bool StreamHeavySkipTestReadWorld(const void* world, const void** pending, bool* timeLimit)
{
    return HsReadWorld(world, pending, timeLimit);
}

StreamHeavySkipTestStats StreamHeavySkipTestRead()
{
    return {hst.skipFrames.load(), hst.declinedInRow.load(), hst.declinedCap.load(), hst.declinedRepeat.load(), hs.noClockLogs,
            hst.fromMedian.load(), static_cast<unsigned>(hst.maxPerLevel.load()), static_cast<unsigned>(hs.pendingSkips)};
}

void StreamGranLogStats()
{
    if (!g_active) return;
    StreamHeavySkipLogStats();  // StreamInHeavySkipMs: only when something changed
    {   // StreamInBudget: only when its sets changed
        static unsigned lastBudgetSets = 0;
        if (g_budgetSets != lastBudgetSets) {
            lastBudgetSets = g_budgetSets;
            Log("[streambudget] stats: %u sets, key %d", g_budgetSets, g_budgetKey.load(std::memory_order_relaxed));
        }
    }
    // only when something changed since the last line (rev130m SG-N1); the counters are the game thread's, read unlocked
    static unsigned lastLoadings = ~0u, lastPlay = ~0u, lastLoading = ~0u, lastSets = ~0u;
    static int lastKey = -1;
    const int key = g_key.load(std::memory_order_relaxed);
    if (g_loadings == lastLoadings && g_addsPlay == lastPlay && g_addsLoading == lastLoading && g_sets == lastSets && key == lastKey)
        return;
    lastLoadings = g_loadings;
    lastPlay = g_addsPlay;
    lastLoading = g_addsLoading;
    lastSets = g_sets;
    lastKey = key;
    Log("[streamgran] stats: %u loading screens, adds %u in play / %u in loading mode, %u sets, key %d", g_loadings, g_addsPlay,
        g_addsLoading, g_sets, g_key.load(std::memory_order_relaxed));
}

// Self-test: fake console variables instead of the console manager's, the hook marked active.
void StreamGranTestSetup(void* gran, void* limit)
{
    g_gran = gran;
    g_limit = limit;
    g_active = gran != nullptr;
    g_written = 0;
    g_loading = false;
    g_key.store(0);
    g_loggedKey = 0;
}

}  // namespace opf
