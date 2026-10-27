// RigTraceReuse ([Fixes] RigTraceReuse, default 1; worker corral2 2026-10-27, public since 1.2.3).
//
// RigTraceReuse (1 = on, 2 = verify, 0 = off; installed at start-up when on, then live; version.dll route only): inside one
// UControlRig::Execute call (one VM run of a rig event, on one thread), a sphere trace with bit-identical inputs to an earlier
// one of the same run (same context, owner component, owner actor, world, Start, End, radius, channel and rig-to-world
// transform) gets that earlier trace's outputs (bHit, HitLocation, HitNormal) instead of a new SweepSingleByChannel.
// Measured at the Brindle Home corral: each horse's rig runs 106 sphere traces per frame, of which 91.5 % repeat an earlier
// trace of the same frame with identical inputs, on the same thread, 20-2000 us later, and all 116,400 such repeats returned
// the identical result (the rig re-runs the pure trace node per consumer: the RigVM lazy-branch re-execution). Equivalent:
// the repeated query asks the same scene the same question microseconds later on the same thread; a different answer is
// only possible if another thread changed the scene in between, a race the game does not order (the rig's read and the
// game thread's body writes interleave by timing), so the reused answer is one stock can give for that run. Nothing is kept
// across runs (a per-thread generation bumped at Execute's entry and exit), nor outside a rig run. RigTraceReuse=2 (verify):
// such repeats still run the game's trace and the result is compared with the one that would have been reused (counted as
// "verified ... with a different result" in the periodic "[rigtrace]" line; 0 of ~2.6 million at the corral, also while
// riding and with a fight next to the horses).
//
// Where: the rig unit FRigUnit_SphereTraceByTraceChannel / SphereTraceWorld's execute (UE 5.3 RigUnit_WorldCollision.cpp:62;
// this build exe+0x4B83E90, found by pattern, its world compare checked) and UControlRig::Execute (exe+0x4AB6250, found by
// pattern). Entry detours with trampolines (8- and 12-byte prologues); Execute is patched first.
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace opf {
namespace {

constexpr char kUnit[] =
    "4C 8B DC 55 53 56 41 54 41 55 41 56 41 57 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
    "48 89 85 ?? ?? ?? ?? 48 8B B5 ?? ?? ?? ?? 4D 8B E0 4C 8B B5 ?? ?? ?? ?? 4C 8B EA 4C 8B BD ?? ?? ?? ?? 48 8B D9 41 C6 07 00";
constexpr size_t kPrologue = 8;
constexpr uint32_t kXform = 0x70, kXformBytes = 0x60, kComp = 0xD0;
// in the function: cmp qword [rcx+0E0h],0 (the world) within the first 0x90 bytes
constexpr uint8_t kWorldCmp[] = {0x48, 0x83, 0xB9, 0xE0, 0x00, 0x00, 0x00, 0x00};


// UControlRig::Execute(const FName& EventName) (UE 5.3 ControlRig.cpp; this build exe+0x4AB6250): one VM run of a rig event
constexpr char kExecute[] =
    "40 55 56 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 33 C0 48 89 54 24 38 "
    "89 44 24 70 48 8B F2 89 44 24 58 48 8B F9 48 8B 01 48 89 4C 24 48 FF 90 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 80 BF ?? ?? ?? ?? 00";
constexpr size_t kExecPrologue = 12;  // push rbp; push rsi; push rdi; lea rbp,[rsp-160h]

using UnitFn = void (*)(uint8_t* ctx, const double* start, const double* end, const uint8_t* channel, float radius,
                        uint8_t* hit, double* loc, double* nrm);
UnitFn g_unit = nullptr;
bool g_installed = false;
int64_t g_freq = 1;

struct In {
    double s[3], e[3];
    float r;
    uint8_t ch;
    uint8_t unit;  // 0 the sphere-trace unit, 1 the second world-trace unit (exe+0x54DCDE0)
    uint8_t x[kXformBytes];
};
struct Out {
    uint8_t hit;
    double loc[3], nrm[3];
};
bool SameIn(const In& a, const In& b)
{
    return memcmp(a.s, b.s, sizeof a.s) == 0 && memcmp(a.e, b.e, sizeof a.e) == 0 && memcmp(&a.r, &b.r, 4) == 0 &&
           a.ch == b.ch && a.unit == b.unit && memcmp(a.x, b.x, kXformBytes) == 0;
}
bool SameOut(const Out& a, const Out& b)
{
    return a.hit == b.hit && memcmp(a.loc, b.loc, sizeof a.loc) == 0 && memcmp(a.nrm, b.nrm, sizeof a.nrm) == 0;
}

// ---- the reuse (RigTraceReuse) ----
// Per thread: the generation of the innermost UControlRig::Execute running on it (bumped at its entry and exit) and the
// last 32 sweeps made inside it. A sweep is reused only inside the same Execute call (same generation) of the same context
// with the same owner component, owner actor and world pointers and bit-identical inputs (Start, End, radius, channel and
// the rig-to-world transform).
struct Entry {
    uint64_t gen, ctx, comp, actor, world;
    In in;
    Out out;
};
struct Tl {
    uint64_t gen = 1;
    int depth = 0, head = 0;
    Entry e[32];
};
thread_local Tl t_tl;
using ExecFn = uint64_t (*)(void*, void*, void*, void*);
ExecFn g_exec = nullptr;
std::atomic<int> g_reuse{0};   // unit 0: 0 off, 1 reuse, 2 verify (run the trace anyway and compare)
std::atomic<uint64_t> s_entries{0}, s_reused{0}, s_outside{0}, s_execs{0}, s_verified{0}, s_verifyDiff{0};
// per unit (0, 1): calls, calls that found an identical earlier trace of the same run (counted in every mode), reused,
// verified and verified-with-a-different-result
std::atomic<uint64_t> s_uCalls[2]{}, s_uRepeats[2]{}, s_uReused[2]{}, s_uVerified[2]{}, s_uDiff[2]{};

uint64_t ExecHook(void* self, void* name, void* c, void* d)
{
    Tl& t = t_tl;
    ++t.depth;
    ++t.gen;
    s_execs.fetch_add(1, std::memory_order_relaxed);
    const uint64_t r = g_exec(self, name, c, d);
    ++t.gen;
    --t.depth;
    return r;
}


template <class Call>
void TraceCore(int u, uint8_t* ctx, const double* start, const double* end, uint8_t ch, float radius, uint8_t* hit, double* loc,
               double* nrm, Call call)
{
    if (u == 0) s_entries.fetch_add(1, std::memory_order_relaxed);
    s_uCalls[u].fetch_add(1, std::memory_order_relaxed);
    Tl& t = t_tl;
    const bool inExec = t.depth > 0;
    In in;
    memset(&in, 0, sizeof in);
    memcpy(in.s, start, sizeof in.s);
    memcpy(in.e, end, sizeof in.e);
    in.r = radius;
    in.ch = ch;
    in.unit = static_cast<uint8_t>(u);
    memcpy(in.x, ctx + kXform, kXformBytes);
    const uint64_t comp = *reinterpret_cast<uint64_t*>(ctx + kComp), actor = *reinterpret_cast<uint64_t*>(ctx + kComp + 8),
                   world = *reinterpret_cast<uint64_t*>(ctx + kComp + 16);
    const int mode = g_reuse.load(std::memory_order_relaxed);
    if (inExec) {
        for (const Entry& e : t.e)
            if (e.gen == t.gen && e.ctx == reinterpret_cast<uint64_t>(ctx) && e.comp == comp && e.actor == actor &&
                e.world == world && SameIn(e.in, in)) {
                s_uRepeats[u].fetch_add(1, std::memory_order_relaxed);
                if (mode == 0) break;
                if (mode == 2) {  // verify: the game's trace runs and its result is compared with the one that would be reused
                    call();
                    Out now;
                    memset(&now, 0, sizeof now);
                    now.hit = *hit;
                    memcpy(now.loc, loc, sizeof now.loc);
                    memcpy(now.nrm, nrm, sizeof now.nrm);
                    if (u == 0) s_verified.fetch_add(1, std::memory_order_relaxed);
                    s_uVerified[u].fetch_add(1, std::memory_order_relaxed);
                    if (!SameOut(now, e.out)) {
                        if (u == 0) s_verifyDiff.fetch_add(1, std::memory_order_relaxed);
                        s_uDiff[u].fetch_add(1, std::memory_order_relaxed);
                    }
                    return;
                }
                *hit = e.out.hit;
                memcpy(loc, e.out.loc, sizeof e.out.loc);
                memcpy(nrm, e.out.nrm, sizeof e.out.nrm);
                if (u == 0) s_reused.fetch_add(1, std::memory_order_relaxed);
                s_uReused[u].fetch_add(1, std::memory_order_relaxed);
                return;
            }
    }
    call();
    Out out;
    memset(&out, 0, sizeof out);
    out.hit = *hit;
    memcpy(out.loc, loc, sizeof out.loc);
    memcpy(out.nrm, nrm, sizeof out.nrm);
    if (inExec) {
        Entry& e = t.e[t.head];
        t.head = (t.head + 1) & 31;
        e.gen = t.gen;
        e.ctx = reinterpret_cast<uint64_t>(ctx);
        e.comp = comp;
        e.actor = actor;
        e.world = world;
        e.in = in;
        e.out = out;
    } else if (u == 0) s_outside.fetch_add(1, std::memory_order_relaxed);
}

void UnitHook(uint8_t* ctx, const double* start, const double* end, const uint8_t* channel, float radius, uint8_t* hit,
              double* loc, double* nrm)
{
    TraceCore(0, ctx, start, end, *channel, radius, hit, loc, nrm,
              [&] { g_unit(ctx, start, end, channel, radius, hit, loc, nrm); });
}


uint8_t* Discover(const GameModule& game)
{
    const auto hits = FindAll(game.text, ParsePattern(kUnit));
    if (hits.size() != 1) {
        Log("[rigtrace] the sphere-trace rig unit found %zu times (expected 1): RigTraceReuse not installed", hits.size());
        return nullptr;
    }
    uint8_t* const site = hits[0];
    bool world = false;
    for (size_t i = 0; i < 0x90 && !world; ++i) world = memcmp(site + i, kWorldCmp, sizeof kWorldCmp) == 0;
    if (!world) { Log("[rigtrace] no world check where expected: RigTraceReuse not installed"); return nullptr; }
    return site;
}

uint32_t DiscoverRva(const GameModule& game)
{
    uint8_t* const p = Discover(game);
    return p ? static_cast<uint32_t>(p - game.base) : 0;
}

// one entry hook: region[0,16) jmp [hook]; region[16,48) trampoline = the prologue + jmp back; returns the trampoline
uint8_t* Detour(uint8_t* site, size_t prologue, void* hook, const char* what)
{
    uint8_t* const region = AllocNearGame(64);
    if (!region) { Log("[rigtrace] %s: no memory near the game's code", what); return nullptr; }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &hook, sizeof hook);
    uint8_t* const tramp = region + 16;
    memcpy(tramp, site, prologue);
    memcpy(tramp + prologue, jmpAbs, sizeof jmpAbs);
    uint8_t* const back = site + prologue;
    memcpy(tramp + prologue + sizeof jmpAbs, &back, sizeof back);
    DWORD old;
    if (!VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(region, 0, MEM_RELEASE);
        Log("[rigtrace] %s: could not make the trampoline executable", what);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    const int64_t rel = region - (site + 5);
    if (rel != static_cast<int32_t>(rel)) { VirtualFree(region, 0, MEM_RELEASE); Log("[rigtrace] %s: out of reach", what); return nullptr; }
    return tramp;
}
bool Patch(uint8_t* site, uint8_t* region)
{
    uint8_t bytes[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(region - (site + 5));
    memcpy(bytes + 1, &rel32, 4);
    return WriteCode(site, bytes);
}

uint64_t g_statFrame = 0;  // the periodic stats line's window
const char* ModeText(int mode)
{
    return mode == 1 ? "a Control Rig sphere trace repeated with identical inputs inside the same rig execution reuses its result"
         : mode == 2 ? "verify: such repeats run as in the game and their results are compared with the ones that would be reused"
                     : "every Control Rig sphere trace runs, as in the game";
}

}  // namespace

uint32_t RigTraceUnitDiscover(const GameModule& game) { return DiscoverRva(game); }
uint32_t RigTraceExecuteDiscover(const GameModule& game)
{
    const auto hits = FindAll(game.text, ParsePattern(kExecute));
    return hits.size() == 1 ? static_cast<uint32_t>(hits[0] - game.base) : 0;
}

// Start-up only (the version.dll route: both entries are replaced by jumps before the game's code runs); the key is live
// afterwards (SetRigTraceReuse).
bool InstallRigTraceReuse(bool beforeGameCode, const wchar_t* iniPath)
{
    if (g_installed) return true;
    (void)iniPath;
    if (!beforeGameCode) {
        Log("[rigtrace] RigTraceReuse: only installed when the game starts (version.dll route; not on the OBSE64 route): not installed");
        return false;
    }
    const GameModule& game = Game();
    uint8_t* const site = Discover(game);
    if (!site) return false;
    const auto ex = FindAll(game.text, ParsePattern(kExecute));
    if (ex.size() != 1) {
        Log("[rigtrace] UControlRig::Execute found %zu times (expected 1): RigTraceReuse not installed", ex.size());
        return false;
    }
    uint8_t* const exec = ex[0];
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    uint8_t* const unitTramp = Detour(site, kPrologue, reinterpret_cast<void*>(&UnitHook), "the sphere-trace unit");
    uint8_t* const execTramp = Detour(exec, kExecPrologue, reinterpret_cast<void*>(&ExecHook), "UControlRig::Execute");
    if (!unitTramp || !execTramp) {
        if (unitTramp) VirtualFree(unitTramp - 16, 0, MEM_RELEASE);
        if (execTramp) VirtualFree(execTramp - 16, 0, MEM_RELEASE);
        Log("[rigtrace] RigTraceReuse not installed");
        return false;
    }
    g_unit = reinterpret_cast<UnitFn>(unitTramp);
    g_exec = reinterpret_cast<ExecFn>(execTramp);
    // Execute first, so a unit call made later always runs inside a counted execution (Execute's hook alone only counts
    // generations and calls the game's function)
    if (!Patch(exec, execTramp - 16)) { Log("[rigtrace] could not patch UControlRig::Execute: RigTraceReuse not installed"); return false; }
    if (!Patch(site, unitTramp - 16)) { Log("[rigtrace] could not patch the unit: RigTraceReuse not installed"); return false; }
    g_installed = true;
    Log("[rigtrace] sphere-trace rig unit exe+0x%X and UControlRig::Execute exe+0x%X hooked (RigTraceReuse installed, live)",
        static_cast<unsigned>(site - game.base), static_cast<unsigned>(exec - game.base));
    g_statFrame = NpcPacingFrame();
    return true;
}

// Live (main.cpp's ini re-read): 0 off, 1 reuse, 2 verify; no-op until installed.
void SetRigTraceReuse(int mode)
{
    mode = mode == 1 ? 1 : mode == 2 ? 2 : 0;
    if (!g_installed || g_reuse.load() == mode) return;
    g_reuse = mode;
    Log("[rigtrace] RigTraceReuse=%d: %s", mode, ModeText(mode));
}

// Periodic stats (StatsIntervalSeconds): sphere-trace calls per frame, how many repeated an earlier one of the same rig run
// with identical inputs, how many were reused, and (mode 2) verified and different.
void LogRigTraceReuseStats()
{
    if (!g_installed) return;
    const uint64_t frame = NpcPacingFrame();
    const double frames = static_cast<double>(frame - g_statFrame);
    g_statFrame = frame;
    const uint64_t ex = s_execs.exchange(0), outside = s_outside.exchange(0);
    s_entries.exchange(0);
    uint64_t uc[2], ur[2], uu[2], uv[2], ud[2];
    for (int k = 0; k < 2; ++k) {
        uc[k] = s_uCalls[k].exchange(0); ur[k] = s_uRepeats[k].exchange(0); uu[k] = s_uReused[k].exchange(0);
        uv[k] = s_uVerified[k].exchange(0); ud[k] = s_uDiff[k].exchange(0);
    }
    if (!uc[0] && !uc[1]) return;
    const double fr = frames > 0 ? frames : 1.0;
    Log("[rigtrace] since last report (%.0f frames): %llu Control Rig sphere traces (%.1f/frame), %.1f%% repeats inside the same "
        "rig execution, %llu reused (%.1f%%), %llu verified (%llu with a different result), %llu outside a rig execution; "
        "%llu rig executions (RigTraceReuse=%d)",
        frames, (unsigned long long)uc[0], uc[0] / fr, uc[0] ? 100.0 * ur[0] / uc[0] : 0.0, (unsigned long long)uu[0],
        uc[0] ? 100.0 * uu[0] / uc[0] : 0.0, (unsigned long long)uv[0], (unsigned long long)ud[0], (unsigned long long)outside,
        (unsigned long long)ex, g_reuse.load());
}

// ---- self-test ----
void RigTraceTestCall(uint8_t* ctx, const double* start, const double* end, const uint8_t* channel, float radius, uint8_t* hit,
                      double* loc, double* nrm, void* fakeUnit)
{
    g_unit = reinterpret_cast<UnitFn>(fakeUnit);
    UnitHook(ctx, start, end, channel, radius, hit, loc, nrm);
}
uint64_t RigTraceTestExecute(void* fakeExec, void* self)
{
    g_exec = reinterpret_cast<ExecFn>(fakeExec);
    return ExecHook(self, nullptr, nullptr, nullptr);
}
void RigTraceTestSetReuse(bool on) { g_reuse = on ? 1 : 0; }
void RigTraceTestSetMode(int mode) { g_reuse = mode; }
uint64_t RigTraceTestVerified(uint64_t* diff)
{
    if (diff) *diff = s_verifyDiff.load();
    return s_verified.load();
}
uint64_t RigTraceTestReused() { return s_reused.load(); }
}  // namespace opf
