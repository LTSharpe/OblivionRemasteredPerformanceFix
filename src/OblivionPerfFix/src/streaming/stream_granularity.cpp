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

uint64_t AddHook(uint64_t world, uint64_t level, uint64_t xform, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
{
    StreamGranBeforeAdd((a4 & 0xFF) != 0);
    return g_add(world, level, xform, a4, a5, a6, a7, a8);
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
    if (!beforeGameCode) { Log("[streamgran] StreamRegisterGranularity: start-up only (version.dll), not installed"); return false; }
    uint8_t* const fn = FindAddToWorld(probeAdd == nullptr);
    if (!fn) return false;
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

// Game thread, at every UWorld::AddToWorld call (before the call reads the variable).
void StreamGranBeforeAdd(bool considerTimeLimit)
{
    if (!g_active || !considerTimeLimit) return;  // without the time limit the call registers everything (granularity 0)
    const int key = g_key.load(std::memory_order_relaxed);
    if (key == 0 && g_written == 0) return;      // off and nothing of ours in place
    if (!FindCvars()) return;
    __try {
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

void StreamGranLogStats()
{
    if (!g_active) return;
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
