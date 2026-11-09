// OtFrameTrim (lab fightcost / fightot2 2026-11-02, played by the maintainer since; public since 1.3.0, worker port130j 2026-11-08
// after review rev130e; [Fixes] OtFrameTrim, default 3 = both bits; 0 = off). Installed at start-up when != 0 (version.dll
// route), then live 0..3.
// Two trims of the old engine's frame on the OblivionThread (OT), which the game thread waits for in the next frame's pairing
// session whenever the OT frame is the longer one (big fights: OT frame ~9.7 ms, the game thread waits 6-7 ms per frame;
// findings 2026-11-02 "fightcost", "fightot2"):
//
// bit 1, the path builder's wait wakes early. The process lists poll the old engine's background path builder: while a
// path request is in flight, the poll asks for a 1 ms "deferred sleep" (it adds to an accumulator; this poll is its only
// deferred user), and the next OT frame starts with Sleep(min(accumulated, 20)), so that the path thread gets the CPU
// before the next poll: a single-core-era yield. In a 36-NPC fight it sleeps ~12 times a second, 1-2 ms each, and the game
// thread waits for that. With the bit set, that Sleep becomes a wait on the path thread's own completion semaphore with the
// same timeout: it returns as soon as the path thread has signalled completion (the count is given back at once, so the
// poll consumes it as before), at once when no request is in flight (state not 1/2: nothing can complete), else after the
// full timeout as stock. Equivalent: the poll sees the same done / not-done state as after the stock sleep (a path that
// completes within the window is done either way; one that does not, is not), only the OT frame starts earlier; what other
// threads finish during the skipped rest of the sleep is ordinary thread timing (Sleep(1) itself lasts 1-2 ms, or 15.6 ms
// at the default timer resolution, in the stock game).
//
// bit 2, the node profiler timer off. A QueryPerformanceCounter wrapper (low 32 bits) is called only by 17 scene-graph
// update functions (66 calls: NiNode / NiAVObject update passes and three others), each a profiling scope that stores a
// start / elapsed time per object. With the bit set it returns 0 without the system call (the depth counters and update
// counts run as before; the stored times become 0). Identical as long as nothing reads those times for anything but
// display: no reader was found in the code, and a lab audit with hardware data breakpoints on the stored times in fights
// found only the profiler's own functions (fightot2; not covered: menus, save / load, the console). ~0.9 ms of the OT frame
// per rendered frame in the fight.
//
// How: the frame-start consumer, its only deferred requester, the path builder's poll and its state check (pinning the
// fields the wait reads: state +0x0C, context +0x10, context +0x48 count and +0x50 semaphore), the process-list call
// holding the path builder's object, and the timer are each found once by pattern and cross-checked against each other and
// the Sleep / QueryPerformanceCounter import slots. Two writes at start-up: the consumer's 6-byte call [Sleep] becomes a
// call to the fix; the timer's entry jumps to a small thunk ("flags bit set: return 0; else the stock body"). The thunk's
// code page is read-execute; its flags dword sits on a separate read-write page (no writable and executable memory).
// Any mismatch: not installed, one log line.
#include "common.h"

#include <intrin.h>

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// exe+0x68D4090: sub rsp,28h; mov ecx,[acc]; test ecx,ecx; je; mov eax,14h; cmp ecx,eax; cmova ecx,eax; mov [acc],ecx;
// call [Sleep] (+0x1E); mov dword [acc],0; add rsp,28h; ret
constexpr char kConsumer[] =
    "48 83 EC 28 8B 0D ?? ?? ?? ?? 85 C9 74 20 B8 14 00 00 00 3B C8 0F 47 C8 89 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? C7 05 ?? ?? "
    "?? ?? 00 00 00 00 48 83 C4 28 C3";
constexpr size_t kConsAcc = 4, kConsSleep = 0x1E;
// exe+0x68D4070: test dl,dl; je; jmp [Sleep]; add [acc],ecx; ret
constexpr char kRequest[] = "84 D2 74 07 48 FF 25 ?? ?? ?? ?? 01 0D ?? ?? ?? ?? C3";
// exe+0x68B26F0 (PathBuilder poll): push rdi; sub rsp,20h; mov rdi,rcx; lea rcx,[lock]; call; cmp qword [rdi+78h],0; jne;
// mov rcx,rdi; call next; cmp qword [rdi+78h],0; je; xor edx,edx; lea ecx,[rdx+1]; call request (+0x30); xor edx,edx;
// mov rcx,rdi; call check (+0x3A); test eax,eax
constexpr char kPoll[] =
    "40 57 48 83 EC 20 48 8B F9 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 83 7F 78 00 75 0F 48 8B CF E8 ?? ?? ?? ?? 48 83 7F 78 "
    "00 74 51 33 D2 8D 4A 01 E8 ?? ?? ?? ?? 33 D2 48 8B CF E8 ?? ?? ?? ?? 85 C0";
constexpr size_t kPollRequest = 0x30, kPollCheck = 0x3A;
// exe+0x674AC70 (process lists): ... mov rcx,[PathBuilder] (+0xE); mov dword [x],6Eh; test rcx,rcx; je; call poll (+0x24)
constexpr char kLists[] =
    "40 53 48 83 EC 30 48 8B D9 0F 29 74 24 20 48 8B 0D ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 6E 00 00 00 48 85 C9 74 05 E8";
constexpr size_t kListsObj = 0xE, kListsPoll = 0x24;
// exe+0x69B4CE0 (task state check): ... mov eax,[rbx+0Ch]; dec eax; cmp eax,1; ja; mov rax,[rbx+10h]; mov ecx,[rax+48h];
// test ecx,ecx; jle; ... mov rdi,[rbx+10h]; ...; mov rcx,[rdi+50h]; mov edx,-1; call [WaitForSingleObject]; cmp eax,102h;
// je; lock dec dword [rdi+48h]
constexpr char kCheck[] =
    "8B 43 0C FF C8 83 F8 01 77 ?? 48 8B 43 10 8B 48 48 85 C9 7E ?? 48 89 7C 24 30 48 8B 7B 10 48 85 FF 74 ?? 48 8B 4F 50 BA "
    "FF FF FF FF FF 15 ?? ?? ?? ?? 3D 02 01 00 00 74 ?? F0 FF 4F 48";
constexpr size_t kCheckAt = 0x40;  // where kCheck starts inside the check function
// exe+0x694E170 (QPC wrapper): sub rsp,28h; lea rcx,[rsp+30h]; call [QueryPerformanceCounter]; mov eax,[rsp+30h];
// add rsp,28h; ret
constexpr char kTimer[] = "48 83 EC 28 48 8D 4C 24 30 FF 15 ?? ?? ?? ?? 8B 44 24 30 48 83 C4 28 C3";

constexpr size_t kPage = 4096;

using SleepFn = void(WINAPI*)(DWORD);
void** g_sleepSlot = nullptr;           // the game's Sleep import slot (the stock call goes through it)
void** g_pathObj = nullptr;             // the PathBuilder task object pointer (or null)
volatile LONG* g_timerFlags = nullptr;  // read by the timer thunk: bit 0 = return 0 (on its own read-write page)
std::atomic<int> g_mode{0};
bool g_installed = false;
uint64_t g_freq = 1;

struct Stats {
    std::atomic<uint64_t> sleeps{}, askedMs{}, ticks{}, early{}, idle{}, full{}, giveBackFailed{};
};
Stats s;

inline uint64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

// Replaces the consumer's call [Sleep] (ecx = min(accumulated, 20) ms, never 0 there). OT only.
void WINAPI SleepHook(DWORD ms)
{
    if (!(g_mode.load(std::memory_order_relaxed) & 1)) {
        reinterpret_cast<SleepFn>(*g_sleepSlot)(ms);
        return;
    }
    const uint64_t t0 = Qpc();
    s.sleeps.fetch_add(1, std::memory_order_relaxed);
    s.askedMs.fetch_add(ms, std::memory_order_relaxed);
    uint8_t* const obj = static_cast<uint8_t*>(*g_pathObj);
    uint8_t* const ctx = obj ? *reinterpret_cast<uint8_t**>(obj + 0x10) : nullptr;
    const int32_t state = obj ? *reinterpret_cast<volatile int32_t*>(obj + 0xC) : 0;
    const HANDLE done = ctx ? *reinterpret_cast<HANDLE*>(ctx + 0x50) : nullptr;
    if (!ctx || !done || (state != 1 && state != 2)) {
        s.idle.fetch_add(1, std::memory_order_relaxed);  // nothing in flight: nothing can complete during the sleep
    } else if (WaitForSingleObject(done, ms) == WAIT_OBJECT_0) {
        // Give the completion back: the poll takes it as in the stock game. The semaphore's maximum is 1 and the path thread
        // signals once per task, and no next task can start before the poll consumes this one, so this cannot fail; a failure
        // would leave the poll's INFINITE wait without its count, so it is counted and logged.
        if (!ReleaseSemaphore(done, 1, nullptr)) s.giveBackFailed.fetch_add(1, std::memory_order_relaxed);
        s.early.fetch_add(1, std::memory_order_relaxed);
    } else {
        s.full.fetch_add(1, std::memory_order_relaxed);
    }
    s.ticks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
}

bool Redirect(uint8_t* site, uint8_t opcode, const uint8_t* to, size_t len)
{
    const int64_t d = to - (site + 5);
    if (d != static_cast<int32_t>(d)) return false;
    uint8_t b[6] = {opcode, 0, 0, 0, 0, 0x90};
    const int32_t r = static_cast<int32_t>(d);
    memcpy(b + 1, &r, 4);
    return WriteCode(site, std::span<const uint8_t>(b, len));
}

// Everything checked, nothing written. Returns false with a reason.
bool Discover(const GameModule& game, OtFrameTrimDiscovery* d, const char** why)
{
    const auto cons = FindAll(game.text, ParsePattern(kConsumer));
    const auto req = FindAll(game.text, ParsePattern(kRequest));
    const auto poll = FindAll(game.text, ParsePattern(kPoll));
    const auto lists = FindAll(game.text, ParsePattern(kLists));
    const auto timer = FindAll(game.text, ParsePattern(kTimer));
    d->consumers = static_cast<int>(cons.size());
    d->requests = static_cast<int>(req.size());
    d->polls = static_cast<int>(poll.size());
    d->lists = static_cast<int>(lists.size());
    d->timers = static_cast<int>(timer.size());
    if (cons.size() != 1 || req.size() != 1 || poll.size() != 1 || lists.size() != 1 || timer.size() != 1) {
        *why = "code not recognised (pattern counts)";
        return false;
    }
    void** const sleepSlot = FindImportSlot(game.base, "KERNEL32.dll", "Sleep");
    void** const qpcSlot = FindImportSlot(game.base, "KERNEL32.dll", "QueryPerformanceCounter");
    uint8_t* const check = RipTarget(poll[0] + kPollCheck, 1, 5);
    if (!sleepSlot || !qpcSlot || RipTarget(cons[0] + kConsSleep, 2, 6) != reinterpret_cast<uint8_t*>(sleepSlot) ||
        RipTarget(req[0] + 4, 3, 7) != reinterpret_cast<uint8_t*>(sleepSlot) ||
        RipTarget(timer[0] + 9, 2, 6) != reinterpret_cast<uint8_t*>(qpcSlot)) {
        *why = "the Sleep / QueryPerformanceCounter imports are not where expected";
        return false;
    }
    if (RipTarget(req[0] + 0xB, 2, 6) != RipTarget(cons[0] + kConsAcc, 2, 6) || RipTarget(poll[0] + kPollRequest, 1, 5) != req[0] ||
        RipTarget(lists[0] + kListsPoll, 1, 5) != poll[0] || !game.text.Contains(check) ||
        !MatchesAt(check + kCheckAt, ParsePattern(kCheck))) {
        *why = "the sleep request / poll / state check do not belong together";
        return false;
    }
    d->consumer = cons[0];
    d->timer = timer[0];
    d->sleepSlot = sleepSlot;
    d->pathObj = reinterpret_cast<void**>(RipTarget(lists[0] + kListsObj, 3, 7));
    d->accumulator = reinterpret_cast<int32_t*>(RipTarget(cons[0] + kConsAcc, 2, 6));
    return true;
}

bool Install(const GameModule& game, OtFrameTrimDiscovery& d, const char** why)
{
    if (!Discover(game, &d, why)) return false;
    // Two pages near the game: page 0 code (jmp [rip] to SleepHook at +0, the timer thunk at +32), made read-execute; page 1
    // the thunk's flags dword (read-write).
    uint8_t* const mem = AllocNearGame(2 * kPage);
    if (!mem) { *why = "no memory near the game's code"; return false; }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    const void* const hook = reinterpret_cast<void*>(&SleepHook);
    memcpy(mem, jmpAbs, 6);
    memcpy(mem + 6, &hook, 8);
    uint8_t* const th = mem + 32;
    auto* const flags = reinterpret_cast<volatile LONG*>(mem + kPage);
    *flags = 0;
    size_t k = 0;
    auto b = [&](std::initializer_list<uint8_t> v) { for (const uint8_t x : v) th[k++] = x; };
    auto rip = [&](const volatile void* target, size_t tail) {  // disp32 at k; the instruction ends 4 + tail bytes later
        const int32_t r = static_cast<int32_t>(reinterpret_cast<const volatile uint8_t*>(target) - (th + k + 4 + tail));
        memcpy(th + k, &r, 4);
        k += 4;
    };
    b({0xF6, 0x05}); rip(flags, 1); b({0x01});                    // test byte [flags],1 (timer off)
    b({0x74, 0x03});                                              // jz stock
    b({0x31, 0xC0, 0xC3});                                        // xor eax,eax; ret
    b({0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xFF, 0x15});  // stock: sub rsp,28h; lea rcx,[rsp+30h]; call [QPC]
    rip(RipTarget(d.timer + 9, 2, 6), 0);
    b({0x8B, 0x44, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x28, 0xC3});    // mov eax,[rsp+30h]; add rsp,28h; ret
    DWORD old;
    if (!VirtualProtect(mem, kPage, PAGE_EXECUTE_READ, &old)) { *why = "could not make the thunk executable"; return false; }
    FlushInstructionCache(GetCurrentProcess(), mem, kPage);
    g_sleepSlot = d.sleepSlot;
    g_pathObj = d.pathObj;
    g_timerFlags = flags;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = static_cast<uint64_t>(f.QuadPart);
    if (!Redirect(d.consumer + kConsSleep, 0xE8, mem, 6) || !Redirect(d.timer, 0xE9, th, 5)) {
        *why = "could not patch (out of reach or write failed)";
        return false;
    }
    g_installed = true;
    return true;
}

}  // namespace

bool OtFrameTrimDiscover(const GameModule& game, OtFrameTrimDiscovery* out)
{
    const char* why = "";
    return Discover(game, out, &why);
}

bool OtFrameTrimTestInstall()
{
    const char* why = "";
    OtFrameTrimDiscovery d{};
    return Install(Game(), d, &why);
}

void OtFrameTrimTestStats(uint64_t* sleeps, uint64_t* early, uint64_t* idle, uint64_t* full)
{
    *sleeps = s.sleeps.load();
    *early = s.early.load();
    *idle = s.idle.load();
    *full = s.full.load();
}

void SetOtFrameTrim(int mode)
{
    if (!g_installed) return;
    const int m = mode & 3;
    g_mode.store(m, std::memory_order_relaxed);
    _InterlockedExchange(g_timerFlags, (m & 2) ? 1 : 0);
}

// Start-up only (version.dll route).
bool InstallOtFrameTrim(bool beforeGameCode, int mode)
{
    if (g_installed) return true;
    if (!beforeGameCode) { Log("[otframe] OtFrameTrim: start-up only (version.dll route), not installed"); return false; }
    const char* why = "";
    OtFrameTrimDiscovery d{};
    if (!Install(Game(), d, &why)) {
        Log("[otframe] OtFrameTrim: %s, not installed", why);
        return false;
    }
    SetOtFrameTrim(mode);
    const GameModule& game = Game();
    Log("[otframe] OtFrameTrim installed (frame-start sleep exe+0x%X, node timer exe+0x%X): OtFrameTrim=%d",
        static_cast<unsigned>(d.consumer - game.base), static_cast<unsigned>(d.timer - game.base), g_mode.load());
    return true;
}

void LogOtFrameTrimStats()
{
    if (!g_installed) return;
    static uint64_t ls, la, lt, le, li, lf;
    const uint64_t sl = s.sleeps.load(), a = s.askedMs.load(), t = s.ticks.load(), e = s.early.load(), i = s.idle.load(),
                   f = s.full.load();
    if (sl == ls) return;
    Log("[otframe] OtFrameTrim=%d: %llu frame-start sleeps, asked %llu ms, waited %.1f ms (early wake %llu, nothing in flight %llu, "
        "full %llu)%s",
        g_mode.load(), static_cast<unsigned long long>(sl - ls), static_cast<unsigned long long>(a - la), (t - lt) * 1000.0 / g_freq,
        static_cast<unsigned long long>(e - le), static_cast<unsigned long long>(i - li), static_cast<unsigned long long>(f - lf),
        s.giveBackFailed.load() ? "; a completion count could not be given back (please report)" : "");
    ls = sl; la = a; lt = t; le = e; li = i; lf = f;
}

}  // namespace opf
