// StartupBarFinish (worker loader106, 2026-11-16; S-058 / S-032): the old engine's loading-bar "finish" loop ends at its
// first step for the first loading menu of the process (the black screen at start-up), with the loop's own final state.
//
// What the game does (game behaviour, also without OblivionPerfFix; our reading of build 19115871): when the old
// engine closes a loading menu (LoadingMenu's destructor, exe+0x6770C20) or finishes a load
// (exe+0x6772340, from the end of the load function exe+0x66AD960), it sets the menu data's "finishing" flag (+0xB0 = 1)
// and its goal (+0x64 = 100) and then runs `while (bar(+0xA0) < 100) step(0)`. The step (exe+0x6770FD0) raises the bar's
// target (+0xA4) by 1 once more than iMoveBarWaitingMilliseconds (10) passed since the bar last moved, copies it to the
// bar (+0xA0), writes the percent to the menu, and for every value below 100 pauses all the loader threads (exe+0x67E1490,
// it waits until each one is idle), updates the menus, draws the interface scene and resumes the loaders (exe+0x67E1D80).
// The load is already finished when the loop starts: the loop only animates the bar to 100. The time it measures comes
// from GetTickCount (a 15.6 ms clock), so each percent waits for one clock tick, the step called in a busy spin until then
// (about 100 million calls): 100 x 15.4 ms = 1.5 s in every one of our start-ups (of a 2.8 s stop before the first logo in
// our hidden 4K start-ups), while the screen stays black and the game thread waits for the old
// engine's frame. Every world load ends with the same 1.5 s in the load's finish loop.
//
// What this does: the two loops' `call step` sites go to a small stub that passes the loop's data object (rbx in both
// loops, the object the loop tests) and the site to our hook. When the loop starts and the key applies to it (1: the
// destructor's loop of the first LoadingMenu of the process, the start-up black screen; 2: every loop), the hook sets the
// target to one below the goal (+0xA4 = +0x64 - 1 = 99, only while the finishing flag is set and the goal is 100) before
// the game's step runs. The step's own +1 then takes it to exactly 100 by either of its rules (more than
// iMaxMilliseconds (2500) since the last move: +1 without a goal test; more than 10 ms with the finishing flag: +1 below
// the goal), so the step does what its last step does in the stock loop: bar = 100, the time it moved = now, the percent
// 100 written to the menu, and (the value is not below 100) no loader pause and no interface draw; the loop ends. When
// the bar moved less than 10 ms before (both start-ups of our check) the step first shows 99 (one stock step: pause,
// update, draw, resume) and the loop ends at the next tick (4-12 ms). Skipped: the 98-99 steps in between (loader pauses,
// menu updates and interface draws of a bar nobody sees at start-up). The load's own work is untouched (it was done before
// the loop), and so is every other caller of the step (the bar during a load).
// Equivalent: the loop's final state is the stock final state; the menu's loading-tip countdown (+0x68) is lowered by one
// step's time instead of the loop's time (it only times the old engine's tip text), and the timer update the step makes on
// each call (globals previous / current tick) runs once instead of ~100 million times: the state the stock game has when
// it skips the loop (bar already 100). Key 2 would also shorten the end of every loading screen: a visible change that also
// shortens the loading screen's idle time other keys use, so it is the player's choice, off by default.
// Key 1 is spent once the first LoadingMenu has been destroyed (the deleting destructor's call of the destructor goes
// through a second stub), whether its loop ran, was skipped (bar already 100, the game's skip flags) or never started:
// key 1 never acts on a loading screen in play.
//
// Key StartupBarFinish: 0 = the game's way; 1 = the first LoadingMenu of the process (start-up); 2 = every loading menu.
// Installed at start-up when != 0 (lab: always, to time the loops with 0; version.dll route only: on the OBSE64 route the
// start-up loop has already run), then live (decided when a loop starts). Each loop is logged (site, process age, bar at
// the start and the end, step calls, ms) up to 40 lines.
#include "common.h"

#include <atomic>

namespace opf {
namespace {

using StepFn = void (*)(uint64_t arg);

// LoadingMenu destructor's loop (exe+0x6770CB4): mov byte [rbx+0B0h],1; mov dword [rbx+64h],64h; cmp dword [rbx+0A0h],64h;
// jge; nop5; xor ecx,ecx; call step; cmp dword [rbx+0A0h],64h; jl; mov byte [rbx+0B0h],0
constexpr char kLoop1[] =
    "C6 83 B0 00 00 00 01 C7 43 64 64 00 00 00 83 BB A0 00 00 00 64 7D ?? 0F 1F 44 00 00 33 C9 E8 ?? ?? ?? ?? 83 BB A0 00 00 00 "
    "64 7C F0 C6 83 B0 00 00 00 00";
constexpr size_t kLoop1Call = 30;
// the load finish's loop (exe+0x67723A1): cmp dword [rbx+0A0h],64h; mov byte [rbx+0B0h],1; mov dword [rbx+64h],64h; jge;
// xor ecx,ecx; call step; cmp dword [rbx+0A0h],64h; jl
constexpr char kLoop2[] =
    "83 BB A0 00 00 00 64 C6 83 B0 00 00 00 01 C7 43 64 64 00 00 00 7D ?? 33 C9 E8 ?? ?? ?? ?? 83 BB A0 00 00 00 64 7C F0";
constexpr size_t kLoop2Call = 25;
// the step (exe+0x6770FD0): mov [rsp+20h],rbp; push rsi; sub rsp,30h; movzx ebp,cl; mov ecx,3EFh (LoadingMenu); call
constexpr char kStep[] = "48 89 6C 24 20 56 48 83 EC 30 0F B6 E9 B9 EF 03 00 00 E8";
// inside the step: mov eax,[rdi+0A4h] (target); mov r14d,[r9+10h] (now); mov edx,r14d; sub edx,[rdi+0A8h] (last move)
constexpr char kStepTarget[] = "8B 87 A4 00 00 00 45 8B 71 10 41 8B D6 2B 97 A8 00 00 00";
constexpr size_t kStepTargetSearch = 0x100;
// LoadingMenu's destructor (exe+0x6770C20; the first loop lies inside it): three home-slot stores, push rdi/r14/r15,
// sub rsp,30h; mov r14,rcx; lea rax,[vtable]; mov [rcx],rax; mov dword [rcx+64h],64h
constexpr char kDtor[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 4C 8B F1 48 8D 05 ?? ?? ?? ?? 48 89 01 C7 41 64 64 00 00 00";
constexpr size_t kDtorLoopMax = 0x100;
// its deleting destructor (exe+0x6770F90): mov [rsp+8],rbx; push rdi; sub rsp,20h; mov ebx,edx; mov rdi,rcx; call dtor;
// test bl,1 (a common shape: the one hit whose call goes to the destructor above)
constexpr char kDeleting[] = "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? F6 C3 01";
constexpr size_t kDeletingCall = 15;

constexpr uint32_t kBar = 0xA0, kTarget = 0xA4, kGoal = 0x64, kFinishing = 0xB0;
constexpr int kSites = 2;
constexpr uint64_t kMaxLines = 40;

using DtorFn = uint64_t (*)(uint64_t menu, uint64_t flags);

StepFn g_step = nullptr;
DtorFn g_dtor = nullptr;
bool g_installed = false;
std::atomic<int> g_mode{0};
std::atomic<bool> g_firstClosed{false};  // the first LoadingMenu has been destroyed (key 1 is spent)
std::atomic<uint64_t> s_loops{0}, s_ended{0}, s_steps{0}, s_lines{0};
std::atomic<uint64_t> s_loopUs{0};

// One per site; the loops run on the old engine's thread, one at a time. A loop of the same site nested inside a step
// (not seen: the step's menu update would have to destroy a LoadingMenu) would share it: only the decision for the inner
// loop and the log lines would be off, never more than that (the preset needs that loop's own flag and goal).
struct Loop {
    bool active = false, applied = false;
    int startBar = 0;
    uint64_t steps = 0;  // step calls of this loop (plain: one thread), added to the 5 s total at the loop's end
    double t0 = 0;
};
Loop g_loop[kSites];

double QpcMs()
{
    static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return 1000.0 / static_cast<double>(q.QuadPart); }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * f;
}

double ProcessAgeS()
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const uint64_t a = (static_cast<uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    const uint64_t b = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return b > a ? static_cast<double>(b - a) / 1e7 : 0;
}

const char* SiteName(int site) { return site == 0 ? "a loading menu closes" : "a load finishes"; }

}  // namespace

// The hook (public for the self-test): arg = the loop's ecx (0), data = the loop's rbx, site 0 = destructor, 1 = finish.
void StartupBarFinishStep(uint64_t arg, uint8_t* data, uint64_t site)
{
    Loop& l = g_loop[site < kSites ? site : 0];
    if (!l.active) {
        l.active = true;
        l.steps = 0;
        l.t0 = QpcMs();
        l.startBar = data ? *reinterpret_cast<const int32_t*>(data + kBar) : -1;
        const int mode = g_mode.load(std::memory_order_relaxed);
        // 1: only the destructor's loop of the first LoadingMenu (the start-up one); 2: every loop
        l.applied = mode == 2 || (mode == 1 && site == 0 && !g_firstClosed.load(std::memory_order_relaxed));
        s_loops.fetch_add(1, std::memory_order_relaxed);
    }
    // one below the goal, only inside the finish loop (flag set) with the goal it set: the step's own +1 makes it 100
    if (l.applied && data && data[kFinishing] == 1 && *reinterpret_cast<const int32_t*>(data + kGoal) == 100 &&
        *reinterpret_cast<const int32_t*>(data + kTarget) < 99)
        *reinterpret_cast<int32_t*>(data + kTarget) = 99;
    g_step(arg);
    ++l.steps;
    if (!data || *reinterpret_cast<const int32_t*>(data + kBar) >= 100) {  // the loop's own end test
        const double ms = QpcMs() - l.t0;
        const int endBar = data ? *reinterpret_cast<const int32_t*>(data + kBar) : -1;
        l.active = false;
        if (l.applied) s_ended.fetch_add(1, std::memory_order_relaxed);
        s_steps.fetch_add(l.steps, std::memory_order_relaxed);
        s_loopUs.fetch_add(static_cast<uint64_t>(ms * 1000), std::memory_order_relaxed);
        if (s_lines.fetch_add(1, std::memory_order_relaxed) < kMaxLines)
            Log("[barfinish] loading-bar finish loop (%s) at +%.1f s (qpc %.1f to %.1f ms): bar %d -> %d, %llu step calls, %.1f ms%s",
                SiteName(static_cast<int>(site)), ProcessAgeS(), l.t0, l.t0 + ms, l.startBar, endBar,
                static_cast<unsigned long long>(l.steps), ms, l.applied ? " (ended at once: StartupBarFinish)" : " (the game's way)");
    }
}

// The deleting destructor's call of LoadingMenu's destructor (public for the self-test): the game's destructor (its loop
// included), then key 1 is spent: the first LoadingMenu of the process is gone, whatever its loop did.
uint64_t StartupBarFinishDtor(uint64_t menu, uint64_t flags)
{
    const uint64_t r = g_dtor(menu, flags);
    if (!g_firstClosed.exchange(true))
        Log("[barfinish] the first loading menu was closed at +%.1f s: StartupBarFinish=1 has nothing more to do", ProcessAgeS());
    return r;
}

namespace {

struct Found {
    uint8_t* call[kSites] = {};
    uint8_t* step = nullptr;
    uint8_t* dtor = nullptr;      // LoadingMenu's destructor (holds the first loop)
    uint8_t* dtorCall = nullptr;  // the deleting destructor's call of it
};

uint8_t* One(const GameModule& game, const char* pat)
{
    const auto hits = FindAll(game.text, ParsePattern(pat));
    return hits.size() == 1 ? hits[0] : nullptr;
}

bool Discover(const GameModule& game, Found* f, const char** why)
{
    uint8_t* const l1 = One(game, kLoop1);
    uint8_t* const l2 = One(game, kLoop2);
    if (!l1 || !l2) { *why = "the two finish loops were not found once each"; return false; }
    uint8_t* const c1 = l1 + kLoop1Call;
    uint8_t* const c2 = l2 + kLoop2Call;
    uint8_t* const s1 = RipTarget(c1, 1, 5);
    uint8_t* const s2 = RipTarget(c2, 1, 5);
    if (s1 != s2 || !game.text.Contains(s1) || !MatchesAt(s1, ParsePattern(kStep))) {
        *why = "the loops do not call the same bar step";
        return false;
    }
    const Pattern target = ParsePattern(kStepTarget);
    bool targetFound = false;
    for (size_t i = 0; i < kStepTargetSearch && !targetFound; ++i) targetFound = MatchesAt(s1 + i, target);
    if (!targetFound) { *why = "the bar step's target field (+0xA4) was not found"; return false; }
    uint8_t* const dtor = One(game, kDtor);
    if (!dtor || l1 < dtor || l1 > dtor + kDtorLoopMax) { *why = "LoadingMenu's destructor (around the first loop) not found"; return false; }
    uint8_t* dtorCall = nullptr;
    int calls = 0;
    for (uint8_t* h : FindAll(game.text, ParsePattern(kDeleting)))
        if (RipTarget(h + kDeletingCall, 1, 5) == dtor) dtorCall = h + kDeletingCall, ++calls;
    if (calls != 1) { *why = "LoadingMenu's deleting destructor not found once"; return false; }
    f->dtor = dtor;
    f->dtorCall = dtorCall;
    f->call[0] = c1;
    f->call[1] = c2;
    f->step = s1;
    return true;
}

// stub per site: mov rdx,rbx; mov r8d,site; jmp [rip+0] -> hook; the third stub (destructor call): jmp [rip+0] -> hook
constexpr size_t kStub = 32;
constexpr int kStubs = kSites + 1;
void BuildStub(uint8_t* at, int site, void* hook)
{
    const uint8_t code[] = {0x48, 0x8B, 0xD3, 0x41, 0xB8, static_cast<uint8_t>(site), 0, 0, 0, 0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, code, sizeof code);
    memcpy(at + sizeof code, &hook, sizeof hook);
}

}  // namespace

bool InstallStartupBarFinish(bool beforeGameCode, int key)
{
    if (g_installed) return true;
    if (key == 0) {
        Log("[barfinish] StartupBarFinish off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[barfinish] StartupBarFinish: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    const GameModule& game = Game();
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) {
        Log("[barfinish] StartupBarFinish not installed: %s", why);
        return false;
    }
    uint8_t* const region = AllocNearGame(kStubs * kStub);
    if (!region) { Log("[barfinish] StartupBarFinish not installed: no memory near the game's code"); return false; }
    // the three call sites and what each calls in the game: the two loops' step calls, the deleting destructor's call
    uint8_t* const site[kStubs] = {f.call[0], f.call[1], f.dtorCall};
    uint8_t* const orig[kStubs] = {f.step, f.step, f.dtor};
    for (int i = 0; i < kStubs; ++i) {
        const int64_t rel = (region + i * kStub) - (site[i] + 5);
        if (rel != static_cast<int32_t>(rel)) {
            FreeOwnCode(region);
            Log("[barfinish] StartupBarFinish not installed: out of reach");
            return false;
        }
    }
    BuildStub(region, 0, reinterpret_cast<void*>(&StartupBarFinishStep));
    BuildStub(region + kStub, 1, reinterpret_cast<void*>(&StartupBarFinishStep));
    {
        const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
        void* const hook = reinterpret_cast<void*>(&StartupBarFinishDtor);
        memcpy(region + 2 * kStub, jmpAbs, sizeof jmpAbs);
        memcpy(region + 2 * kStub + sizeof jmpAbs, &hook, sizeof hook);
    }
    DWORD old;
    if (!VirtualProtect(region, kStubs * kStub, PAGE_EXECUTE_READ, &old)) {
        FreeOwnCode(region);
        Log("[barfinish] StartupBarFinish not installed: could not make the stubs executable");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), region, kStubs * kStub);
    g_step = reinterpret_cast<StepFn>(f.step);
    g_dtor = reinterpret_cast<DtorFn>(f.dtor);
    g_mode = key == 1 || key == 2 ? key : 0;
    int patched = 0;
    for (int i = 0; i < kStubs; ++i) {
        uint8_t bytes[5] = {0xE8};
        const int32_t rel32 = static_cast<int32_t>((region + i * kStub) - (site[i] + 5));
        memcpy(bytes + 1, &rel32, 4);
        if (WriteCode(site[i], bytes)) ++patched;
    }
    if (patched != kStubs) {
        // every site goes back to its direct call of the game's function
        for (int i = 0; i < kStubs; ++i) {
            uint8_t bytes[5] = {0xE8};
            const int32_t rel32 = static_cast<int32_t>(orig[i] - (site[i] + 5));
            memcpy(bytes + 1, &rel32, 4);
            WriteCode(site[i], bytes);
        }
        Log("[barfinish] StartupBarFinish not installed: could not patch the loops");
        return false;
    }
    g_installed = true;
    Log("[barfinish] loading-bar finish loops exe+0x%X / exe+0x%X (bar step exe+0x%X), LoadingMenu destructor exe+0x%X "
        "(called at exe+0x%X): installed, %s",
        static_cast<unsigned>(f.call[0] - game.base), static_cast<unsigned>(f.call[1] - game.base),
        static_cast<unsigned>(f.step - game.base), static_cast<unsigned>(f.dtor - game.base),
        static_cast<unsigned>(f.dtorCall - game.base),
        g_mode == 1 ? "the first loading menu's bar ends at once" : g_mode == 2 ? "every loading menu's bar ends at once"
                                                                             : "off (timing the loops only)");
    return true;
}

void SetStartupBarFinish(int key)
{
    const int mode = key == 1 || key == 2 ? key : 0;
    if (!g_installed || g_mode.load() == mode) return;
    g_mode = mode;
    Log("[barfinish] StartupBarFinish now %d (from the next finish loop)", mode);
}

void LogStartupBarFinishStats()
{
    if (!g_installed) return;
    const uint64_t loops = s_loops.exchange(0);
    if (!loops) return;
    const uint64_t ended = s_ended.exchange(0), steps = s_steps.exchange(0), us = s_loopUs.exchange(0);
    Log("[barfinish] since last report: %llu loading-bar finish loops (%llu ended at once), %llu step calls, %.1f ms in the loops",
        static_cast<unsigned long long>(loops), static_cast<unsigned long long>(ended), static_cast<unsigned long long>(steps),
        us / 1000.0);
}

// self-test: discovery on the mapped exe (nothing patched); the hook with a fake step
bool StartupBarFinishTestDiscover(const GameModule& game, uint32_t rva[5])
{
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) return false;
    rva[0] = static_cast<uint32_t>(f.call[0] - game.base);
    rva[1] = static_cast<uint32_t>(f.call[1] - game.base);
    rva[2] = static_cast<uint32_t>(f.step - game.base);
    rva[3] = static_cast<uint32_t>(f.dtor - game.base);
    rva[4] = static_cast<uint32_t>(f.dtorCall - game.base);
    return true;
}

void StartupBarFinishTestSetup(void* step, void* dtor, int mode, bool firstClosed)
{
    g_step = reinterpret_cast<StepFn>(step);
    g_dtor = reinterpret_cast<DtorFn>(dtor);
    g_mode = mode;
    g_firstClosed = firstClosed;
    for (Loop& l : g_loop) l = Loop{};
    s_loops = 0; s_ended = 0; s_steps = 0; s_loopUs = 0;
}

bool StartupBarFinishTestFirstClosed() { return g_firstClosed.load(); }

}  // namespace opf
