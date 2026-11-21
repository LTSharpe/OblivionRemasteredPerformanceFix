// GcMarkPriority (worker gc104 from worker gcprobe's design, 2026-11-15; [Fixes] GcMarkPriority = 0 off (default until the
// release) | 1 normal | 2 above normal; public: installed at start-up when != 0 (version.dll route), then live; lab: always
// installed, the lab probe GcMarkProbe watches the same wrapper): a task worker that runs a part of a garbage-collection
// pass's mark runs it at least at normal priority, and goes back to its own priority when that part is done.
//
// Why (findings gcprobe): UE 5.3 splits the mark of a GC pass into one context per task worker (UE::GC::ProcessAsync /
// FWorkCoordinator, GarbageCollection.cpp): NumWorkers-1 tasks are launched, the game thread works its own context, steals
// the contexts nobody has started, and then spins (UE::GC::StealWork's KeepSpinning) until every STARTED context is done.
// 5.3 runs its Background Workers at THREAD_PRIORITY_BELOW_NORMAL; when other programs keep the CPU busy, a background
// worker that has started a context can lose its core, and the whole stop-the-world pass waits for it (the slow Bravil pass:
// 16.5 ms where the pile line says 7.2, its last 5-7 ms of the game thread in StealWork). UE 5.4-5.7 keep the same scheme.
//
// How: the two callers of ProcessAsync in this build (exe+0x1011680 / 0x10116C0 in 19115871) pass the ProcessSync function
// with `lea rcx, [rip+thunk]`; the tasks capture that pointer, so every thread that runs a context (game thread and workers)
// calls it. We point each lea's 4-byte displacement at a small jump of ours near the game's code (`jmp [rip+0]; dq Wrap<i>`)
// and Wrap<i> calls the original thunk with the same two registers. Found by byte pattern: the call-site shape
//   48 83 EC 28 4C 8B C2 48 8D 0D ?? ?? ?? ?? 48 8D 54 24 40 E8 ?? ?? ?? ??   (exactly two hits, both calling one function)
// whose lea target has the ProcessSync thunk shape
//   48 83 EC 28 48 89 4C 24 30 48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 83 C4 28 C3   (two different thunks)
// anything else: not installed, one log line. Only the two 4-byte displacements are written, at start-up before the game's
// code runs; the thunks and ProcessAsync are not touched.
//
// The raise: on entry GetThreadPriority; only when it is below the target (THREAD_PRIORITY_NORMAL for 1, ABOVE_NORMAL for 2)
// SetThreadPriority(target); a guard object restores the value read at entry when the context returns. A thread already at
// or above the target is never touched. UE 5.3 on Windows runs the game, render and RHI threads at THREAD_PRIORITY_NORMAL
// (WindowsPlatformAffinity.h is `typedef FGenericPlatformAffinity FPlatformAffinity`, whose GetGameThreadPriority /
// GetRenderingThreadPriority / GetRHIThreadPriority return TPri_Normal; correction 2026-11-15, review rev104 M1; the first
// version of this comment took ABOVE_NORMAL from the 5.7 header; the game may override this, which the lab csv's prio_in of
// the game thread shows) and its Foreground Workers at NORMAL. So at 1 a raised worker is level with the game, render and
// RHI threads (it does not outrank them) and runs at their level, not below it; at 2 the game thread is raised too, for its
// context, to ABOVE_NORMAL, and raised workers then outrank the render and RHI threads: level 2 is not recommended. Raised
// workers that have finished their context spin (YieldProcessor) inside the raise until the pass ends (rev104 S1).
// The mark does not unwind through us in a shipping build
// (no exceptions; a fault ends the process), and the guard restores on every return.
// Equivalence: the same contexts, the same objects, the same order-independent mark; the reachable set and everything after
// it are identical. Only which thread gets a core when changes; stock already lets any worker or the game thread run any
// context. Key 0 (live): the wrapper calls the thunk and does nothing else.
#include "common.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace opf {
namespace {
using SyncFn = void(__fastcall*)(void*, void*);

constexpr char kSitePattern[] = "48 83 EC 28 4C 8B C2 48 8D 0D ?? ?? ?? ?? 48 8D 54 24 40 E8 ?? ?? ?? ??";
constexpr char kThunkPattern[] = "48 83 EC 28 48 89 4C 24 30 48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 83 C4 28 C3";
constexpr size_t kLea = 7, kCall = 19;  // offsets in the site pattern

bool g_installed = false;
std::atomic<int> g_level{0};   // 0 = off, 1 = at least THREAD_PRIORITY_NORMAL, 2 = at least ABOVE_NORMAL (the key, live)
std::atomic<int> g_key{-1};    // the key in effect (for the log line)
SyncFn g_orig[2] = {};
std::atomic<uint64_t> s_contexts{0}, s_raised{0}, s_failed{0};
uint64_t s_totalContexts = 0, s_totalRaised = 0;  // monitor thread only

// THREAD_PRIORITY_NORMAL is 0, so the level (not the priority) says whether the raise is on
int TargetOf(int level) { return level == 2 ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL; }

struct RaiseGuard {
    int prioIn = THREAD_PRIORITY_ERROR_RETURN;
    bool raised = false;
    explicit RaiseGuard(int level)
    {
        if (!level) return;
        const int target = TargetOf(level);
        const HANDLE self = GetCurrentThread();
        prioIn = GetThreadPriority(self);
        if (prioIn == THREAD_PRIORITY_ERROR_RETURN || prioIn >= target) return;
        raised = SetThreadPriority(self, target) != FALSE;
        (raised ? s_raised : s_failed).fetch_add(1, std::memory_order_relaxed);
    }
    void Restore()
    {
        if (raised) SetThreadPriority(GetCurrentThread(), prioIn);
        raised = false;
    }
    ~RaiseGuard() { Restore(); }
    RaiseGuard(const RaiseGuard&) = delete;
    RaiseGuard& operator=(const RaiseGuard&) = delete;
};

template <int I>
void __fastcall Wrap(void* a, void* b)
{
    const int level = g_level.load(std::memory_order_relaxed);
    if (!level) return g_orig[I](a, b);
    s_contexts.fetch_add(1, std::memory_order_relaxed);
    RaiseGuard guard(level);
    g_orig[I](a, b);
}

// Both sites and their thunks, or nullptr in `why`. lea[i] = the lea instruction, thunk[i] its target.
bool Find(const GameModule& game, uint8_t* lea[2], uint8_t* thunk[2], const char** why)
{
    const Pattern site = ParsePattern(kSitePattern);
    const Pattern shape = ParsePattern(kThunkPattern);
    const std::vector<uint8_t*> hits = FindAll(game.text, site);
    if (hits.size() != 2) { *why = hits.empty() ? "ProcessAsync's call sites not found" : "ProcessAsync's call sites not found exactly twice"; return false; }
    uint8_t* callee = nullptr;
    for (int i = 0; i < 2; ++i) {
        uint8_t* const target = RipTarget(hits[i] + kCall, 1, 5);
        if (!game.text.Contains(target)) { *why = "a call site's callee is outside the code"; return false; }
        if (callee && target != callee) { *why = "the two call sites call different functions"; return false; }
        callee = target;
        lea[i] = hits[i] + kLea;
        thunk[i] = RipTarget(lea[i], 3, 7);
        if (!game.text.Contains(thunk[i]) || !game.text.Contains(thunk[i] + shape.size() - 1) || !MatchesAt(thunk[i], shape)) {
            *why = "a call site passes no ProcessSync thunk";
            return false;
        }
    }
    if (thunk[0] == thunk[1]) { *why = "both call sites pass the same thunk"; return false; }
    return true;
}

bool Patch(const GameModule& game, const char** why)
{
    uint8_t* lea[2] = {};
    uint8_t* thunk[2] = {};
    if (!Find(game, lea, thunk, why)) return false;
    uint8_t* const stubs = AllocNear(game.base, game.sizeOfImage, 32);
    if (!stubs) { *why = "no memory near the game's code"; return false; }
    void* const wraps[2] = {reinterpret_cast<void*>(&Wrap<0>), reinterpret_cast<void*>(&Wrap<1>)};
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]; dq target
    for (int i = 0; i < 2; ++i) {
        memcpy(stubs + 16 * i, jmpAbs, sizeof jmpAbs);
        memcpy(stubs + 16 * i + sizeof jmpAbs, &wraps[i], sizeof(void*));
    }
    DWORD old = 0;
    if (!VirtualProtect(stubs, 32, PAGE_EXECUTE_READ, &old)) { FreeOwnCode(stubs); *why = "stub protection"; return false; }
    FlushInstructionCache(GetCurrentProcess(), stubs, 32);
    int32_t disp[2], oldDisp[2];
    for (int i = 0; i < 2; ++i) {
        const int64_t d = (stubs + 16 * i) - (lea[i] + 7);
        if (d != static_cast<int32_t>(d)) { FreeOwnCode(stubs); *why = "out of reach"; return false; }
        disp[i] = static_cast<int32_t>(d);
        memcpy(&oldDisp[i], lea[i] + 3, 4);
    }
    g_orig[0] = reinterpret_cast<SyncFn>(thunk[0]);
    g_orig[1] = reinterpret_cast<SyncFn>(thunk[1]);
    for (int i = 0; i < 2; ++i) {
        uint8_t bytes[4];
        memcpy(bytes, &disp[i], 4);
        if (!WriteCode(lea[i] + 3, bytes) || memcmp(lea[i] + 3, bytes, 4) != 0) {
            if (i == 1) {  // put the first site back
                uint8_t b0[4];
                memcpy(b0, &oldDisp[0], 4);
                WriteCode(lea[0] + 3, b0);
            }
            *why = "the code write failed";
            return false;  // the stubs stay allocated (the first site may have pointed at them for a moment)
        }
    }
    return true;
}
}  // namespace

bool InstallGcMarkPriority(bool beforeGameCode, int key)
{
    if (g_installed) return true;
    if (!key) {
        Log("[gcmark] GcMarkPriority off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[gcmark] GcMarkPriority: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    const GameModule& game = Game();
    const char* why = "";
    if (!Patch(game, &why)) {
        Log("[gcmark] GcMarkPriority not installed (the game's way): %s", why);
        return false;
    }
    g_installed = true;
    SetGcMarkPriority(key);
    Log("[gcmark] GcMarkPriority installed: the garbage collection's mark parts run through our wrapper (ProcessAsync's two "
        "callers, 4-byte addresses changed); live 0 / 1 / 2");
    return true;
}

void SetGcMarkPriority(int key)
{
    if (!g_installed) return;
    if (key < 0 || key > 2) key = key > 2 ? 2 : 0;
    g_level.store(key, std::memory_order_relaxed);
    if (g_key.exchange(key) == key) return;
    Log("[gcmark] %s", key == 2 ? "task workers run their part of a garbage collection's mark at above normal priority at least"
                       : key == 1 ? "task workers run their part of a garbage collection's mark at normal priority at least"
                                  : "garbage collection's mark parts run at the workers' own priority, as the game does");
}

bool GcMarkPriorityInstalled() { return g_installed; }

void LogGcMarkPriorityStats()
{
    if (!g_installed) return;
    const uint64_t c = s_contexts.exchange(0), r = s_raised.exchange(0), f = s_failed.exchange(0);
    if (!c) return;
    s_totalContexts += c;
    s_totalRaised += r;
    Log("[gcmark] since last report: %llu mark parts run, %llu of them by a thread below the target raised for it (%llu raises "
        "failed); %llu / %llu since start",
        static_cast<unsigned long long>(c), static_cast<unsigned long long>(r), static_cast<unsigned long long>(f),
        static_cast<unsigned long long>(s_totalRaised), static_cast<unsigned long long>(s_totalContexts));
}


// ---- self-test ----
bool GcMarkPriorityTestFind(const GameModule& game, uint32_t rva[5])
{
    uint8_t* lea[2] = {};
    uint8_t* thunk[2] = {};
    const char* why = "";
    memset(rva, 0, 5 * sizeof(uint32_t));
    if (!Find(game, lea, thunk, &why)) return false;
    const auto r = [&](const uint8_t* p) { return static_cast<uint32_t>(p - game.base); };
    rva[0] = r(lea[0]); rva[1] = r(lea[1]); rva[2] = r(thunk[0]); rva[3] = r(thunk[1]);
    rva[4] = r(RipTarget(lea[0] - kLea + kCall, 1, 5));
    return true;
}

const char* GcMarkPriorityTestPatch(const GameModule& game)
{
    const char* why = nullptr;
    const bool wasInstalled = g_installed;
    if (!Patch(game, &why)) return why ? why : "failed";
    g_installed = wasInstalled;
    return nullptr;
}

void* GcMarkPriorityTestWrapper(int i) { return i ? reinterpret_cast<void*>(&Wrap<1>) : reinterpret_cast<void*>(&Wrap<0>); }

void GcMarkPriorityTestSetup(void* orig0, void* orig1, int key)
{
    g_orig[0] = reinterpret_cast<SyncFn>(orig0);
    g_orig[1] = reinterpret_cast<SyncFn>(orig1);
    g_level.store(key, std::memory_order_relaxed);
    s_contexts = 0;
    s_raised = 0;
    s_failed = 0;
}

void GcMarkPriorityTestCounts(uint64_t out[3])
{
    out[0] = s_contexts.load();
    out[1] = s_raised.load();
    out[2] = s_failed.load();
}
}  // namespace opf
