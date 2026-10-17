// Diagnostic HitchLedger ([Fixes] HitchLedger=0 default, HitchLedgerMs=25): for every slow game-thread frame
// (period >= HitchLedgerMs, the frame-rate cap's sleep included) a record of where its time went, so a play session can
// be ranked by cause afterwards (tools/stutter_ledger.py, design in docs/stutter-ledger.md). No behaviour change: the
// hooks only read the clock around the game's own calls.
//
// The frame is the game thread's time from one exit of UEngine::UpdateTimeAndHandleMaxTickRate (where the cap sleeps,
// early in FEngineLoop::Tick) to the next (UE 5.3 LaunchEngineLoop / GameEngine.cpp / LevelTick.cpp):
//   pre    frame start -> FWorldDelegates::OnWorldPreActorTick (NpcCreationPacing's FrameRun): the rest of the previous
//          FEngineLoop::Tick after the cap, UGameEngine::Tick's StaticTick (async loading), TickWorldTravel, the start
//          of UWorld::Tick (level streaming visibility, network);
//   actor  -> OnWorldPostActorTick (NpcCreationPacing's Handler_End hook): the pairing session with the old engine (the GT waits
//          for the OblivionThread's frame: "pair"), the tick groups TG_PrePhysics .. TG_LastDemotable
//          (FTickTaskSequencer::ReleaseTickGroup, game-thread time per group: tick work plus waits, TG_EndPhysics =
//          waiting for the physics step), latent actions, timers, tickables, cameras, streaming state;
//   post   -> the cap's entry: the end of UWorld::Tick (FX, garbage collection), UGameEngine::Tick's level streaming and
//          map change, Slate, the viewport draw, FFrameEndSync::Sync ("sync": the wait for the render thread, which
//          waits for the RHI thread and the GPU), the next FEngineLoop::Tick's start;
//   sleep  the cap's sleep.
// Labelled work inside these: NPC units (NpcCreationPacing), old-engine level flows, InitStaticBodies / the scene add of new static
// bodies (ParallelStaticBodies, PipelinedSceneAdd and PipelinedBodyCreation, cumulative game-thread time), the fade-to-game call (PrepareCollisionBehindLoadingScreen's collision hand-over runs
// in it), the OblivionThread's cell-grid change (a cell-row crossing; its own thread, counted in the GT frame in which
// it ended), the GPU frame time (GGPUFrameTime), the shader pipeline cache's precompile (FShaderPipelineCache: PSOs
// dispatched and render-thread time since the last frame, and whether it is running).
//
// Game thread: per-frame accumulators only; a slow frame (and every 10 s a summary) becomes a fixed record pushed into a
// single-producer ring. The monitor thread drains it once a second into OblivionPerfFix-ledger.csv (own file, size
// capped; the game thread never does file I/O or takes the log's mutex for ledger data).
//
// Sites (build 19115871; each found by pattern, a missing one turns its column to -1 with a log line):
//   FTickTaskSequencer::ReleaseTickGroup exe+0x37C8160 (entry detour, start-up)
//   FFrameEndSync::Sync exe+0x37FA780, called once from FEngineLoop::Tick at exe+0x3908517 (that call redirected)
//   the OT's cell-grid transition exe+0x66B2000 (entry detour, start-up)
//   FShaderPipelineCache: TotalCompleteTasks / TotalPrecompileTime (exe+0x921A218 / 0x921A220, from their lock inc /
//   lock add in the precompile task at exe+0x26FEF99), the singleton (exe+0x921A230) and its precompiling flag (+0x140,
//   from the getter exe+0x26FDB30); read only
//   GGPUFrameTime (r.D3D12.GPUTimeFromTimestamps' reader, ProcessTimestamps), read only
// Garbage collection has no cheap anchor in this build (no log strings; the interval getter is virtual): gc_us = -1.
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace opf {
namespace {

// ---- patterns ----
// FTickTaskSequencer::ReleaseTickGroup(this, ETickingGroup group, bool bBlockTillComplete)
constexpr char kTgEntry[] =
    "40 53 55 56 57 41 54 41 55 41 56 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 90 00 00 00 41 0F B6 E8 "
    "4C 63 E2 4C 8B E9 E8";
constexpr size_t kTgPrologue = 5;  // push rbx (REX); push rbp; push rsi; push rdi
// The OT's grid transition: mov [rsp+18h],r8b; push rbx; push rsi; push rdi; push r14; sub rsp,58h; mov rdi,rcx; ...
constexpr char kGridEntry[] =
    "44 88 44 24 18 53 56 57 41 56 48 83 EC 58 48 8B F9 45 0F B6 F0 48 8B 89 ?? ?? 00 00 48 8B F2 48 85 C9 74";
constexpr size_t kGridPrologue = 5;
// FEngineLoop::Tick: FrameEndSync.Sync(CVarAllowOneFrameThreadLag->GetValueOnGameThread != 0):
//   mov rax,[cvar data]; lea rcx,[FrameEndSync]; cmp dword [rax],0; setne dl; call Sync
constexpr char kSyncCall[] = "48 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 83 38 00 0F 95 C2 E8";
constexpr size_t kSyncCallAt = 0x14;
// FFrameEndSync::Sync: rbx = this, dil = bAllowOneFrameThreadLag, Fence[EventIndex (+20h)].BeginFence(true)
constexpr char kSyncFn[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 0F B6 FA 48 63 49 20 B2 01 48 C1 E1 04 48 03 CB E8";
// The precompile task's end: lea rcx,[rsp+x]; call [QueryPerformanceCounter]; lock inc [TotalCompleteTasks];
// mov rcx,[rsp+x]; sub rcx,[rbp+y]; lock add [TotalPrecompileTime],rcx
constexpr char kPsoCounters[] =
    "48 8D 4C 24 ?? FF 15 ?? ?? ?? ?? F0 48 FF 05 ?? ?? ?? ?? 48 8B 4C 24 ?? 48 2B 4D ?? F0 48 01 0D";
constexpr size_t kPsoTasksInsn = 0x0B, kPsoTimeInsn = 0x1C;
// The precompiling getter: mov rax,[singleton]; test rax,rax; je; movzx eax,byte [rax+X]; nop; ret; ret
constexpr char kPsoGetter[] = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 09 0F B6 80 ?? ?? 00 00 90 C3 C3";
constexpr size_t kPsoGetterFlag = 0x0F;

constexpr int kGroups = 8;          // TG_PrePhysics .. TG_NewlySpawned
constexpr size_t kRing = 1024;      // records (a second of slow frames at any rate, plus summaries)
constexpr uint64_t kSummaryUs = 10000000;
constexpr long kMaxFileBytes = 48L << 20;
constexpr uint32_t kSat16 = 65535;

using TgFn = void (*)(void* sequencer, int group, uint8_t block);
using SyncFn = void (*)(void* self, uint8_t lag);
using GridFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);

// State classes (also the summary's columns): play = no freeze, no named freeze, no travel; menu = a named freeze
// (inventory, dialogue, map, pause, the main menu); loading = travel pending or the transition's own freeze (mode 1,
// no named freeze); other = anything else (freeze state unreadable, other modes).
enum StateClass { kPlay, kMenu, kLoading, kOther, kStates };
const char* const kStateName[kStates] = {"play", "menu", "loading", "other"};
constexpr int kBins = 6;  // period < 16.7, < 25, < 33.3, < 50, < 100, >= 100 ms
const uint32_t kBinEdgeUs[kBins - 1] = {16667, 25000, 33333, 50000, 100000};

enum Flag : uint32_t {
    kHasPre = 1, kHasPost = 2, kFade = 4, kGrid = 8, kUnfocused = 16, kTravel = 32, kPsoActive = 64, kPairing = 128,
    kFirst = 256,  // the first frame after the ledger was switched on (its start is not known exactly)
};

struct Summary {
    uint32_t n[kStates], hist[kStates][kBins], slowN[kStates];
    uint64_t us[kStates], slowExcessUs[kStates];
    uint64_t unfocusedPlayUs, frames;  // play-state time without the keyboard focus
};

// One record: kind 1 = slow frame, 2 = summary (the denominators of the last ~10 s).
struct Record {
    uint32_t kind, flags;
    uint64_t frame, endUs;
    // frame
    uint32_t periodUs, sleepUs, preUs, actorUs, postUs, syncUs;
    uint32_t tgUs[kGroups];
    uint32_t pairUs, flowUs, unitUs, units, gridUs, gridN, bodiesUs, fadeUs;
    int32_t gpuUs, gcUs, psoTasks, psoUs;
    int32_t named, mode;
    uint32_t sinceChange, sinceFade, capUs, state;
    float px, py, pz;
    // summary
    Summary sum;
    uint32_t dropped;
};

struct FrameAcc {
    uint64_t startUs = 0, preTs = 0, postTs = 0, sleepTs = 0;
    uint32_t tg[kGroups] = {};
    uint64_t syncUs = 0, fadeUs = 0;
    bool fade = false;
};

struct Sites {
    uint8_t* tg = nullptr;
    uint8_t* syncCall = nullptr;
    uint8_t* sync = nullptr;
    uint8_t* grid = nullptr;
    volatile int64_t* psoTasks = nullptr;
    volatile int64_t* psoTime = nullptr;
    uint8_t* const* psoSingleton = nullptr;
    uint32_t psoFlag = 0;
    const volatile uint32_t* gpu = nullptr;
    uint8_t* region = nullptr;  // trampolines and stubs near the game
};

Sites st;
TgFn g_tgOrig = nullptr;
SyncFn g_syncOrig = nullptr;
GridFn g_gridOrig = nullptr;
bool g_installed = false, g_pacing = false, g_gpuSearched = false;
std::atomic<bool> g_on{false};
std::atomic<uint32_t> g_thresholdUs{25000};
std::atomic<bool> g_focused{true};
std::atomic<DWORD> g_gtId{0};
std::atomic<uint64_t> g_gridUs{0}, g_gridN{0};  // the OT's grid changes, taken by the game thread at each frame end
double g_cycleUs = 0;                           // FPlatformTime cycles (QPC ticks) -> us
LARGE_INTEGER g_freq{};
std::wstring g_path;
uint64_t (*g_testClock)() = nullptr;

// game thread
FrameAcc f;
int g_tgDepth = 0;
bool g_wasOn = false;
uint64_t g_frames = 0;
LedgerPacing g_lastPacing{};
uint64_t g_lastBodies = 0;
int64_t g_lastPsoTasks = 0, g_lastPsoTime = 0;
int g_lastState = -1;
uint32_t g_sinceChange = kSat16, g_sinceFade = kSat16;
uint64_t g_summaryStart = 0;
Summary g_sum{};

// ring (game thread produces, monitor thread consumes)
Record g_ring[kRing];
std::atomic<uint64_t> g_head{0}, g_tail{0};
std::atomic<uint32_t> g_dropped{0};

// monitor thread
FILE* g_file = nullptr;
long g_fileBytes = 0;
bool g_fileFull = false, g_fileFailed = false, g_noFramesLogged = false;
uint64_t g_written = 0;

uint64_t Us()
{
    if (g_testClock) return g_testClock();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return QpcToMicroseconds(static_cast<uint64_t>(t.QuadPart), static_cast<uint64_t>(g_freq.QuadPart));
}

bool OnGt() { return GetCurrentThreadId() == g_gtId.load(std::memory_order_relaxed); }

uint32_t U32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v); }

void Push(const Record& r)
{
    const uint64_t h = g_head.load(std::memory_order_relaxed);
    if (h - g_tail.load(std::memory_order_acquire) >= kRing) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_ring[h % kRing] = r;
    g_head.store(h + 1, std::memory_order_release);
}

// ---- hooks ----
void TgHook(void* sequencer, int group, uint8_t block)
{
    if (!g_on.load(std::memory_order_relaxed) || g_tgDepth || !OnGt()) return g_tgOrig(sequencer, group, block);
    ++g_tgDepth;
    const uint64_t t0 = Us();
    g_tgOrig(sequencer, group, block);
    const uint64_t dt = Us() - t0;
    --g_tgDepth;
    if (g_on.load(std::memory_order_relaxed) && g_tgDepth == 0 && group >= 0 && group < kGroups) f.tg[group] += U32(dt);
}

void SyncHook(void* self, uint8_t lag)
{
    if (!g_on.load(std::memory_order_relaxed) || !OnGt()) return g_syncOrig(self, lag);
    const uint64_t t0 = Us();
    g_syncOrig(self, lag);
    f.syncUs += Us() - t0;
}

uint64_t GridHook(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    if (!g_on.load(std::memory_order_relaxed)) return g_gridOrig(a, b, c, d);
    const uint64_t t0 = Us();
    const uint64_t r = g_gridOrig(a, b, c, d);
    g_gridUs.fetch_add(Us() - t0, std::memory_order_relaxed);
    g_gridN.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// ---- install helpers ----
// Copies n whole, position-independent instructions of fn to a trampoline (slot) that continues at fn + n, and puts a jmp
// to hook at slot + 32; returns the trampoline.
uint8_t* Trampoline(uint8_t* fn, size_t n, void* hook, uint8_t* slot)
{
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + n;
    memcpy(slot, fn, n);
    memcpy(slot + n, jmpAbs, 6);
    memcpy(slot + n + 6, &resume, 8);
    memcpy(slot + 32, jmpAbs, 6);
    memcpy(slot + 32 + 6, &hook, 8);
    return slot;
}

bool PatchJmp(uint8_t* fn, size_t n, uint8_t* target)
{
    const int64_t rel = target - (fn + 5);
    if (rel != static_cast<int32_t>(rel) || n < 5 || n > 8) return false;
    uint8_t jmp[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(jmp + 1, &rel32, 4);
    return WriteCode(fn, std::span<const uint8_t>(jmp, n));
}

uint8_t* FindOne(const GameModule& game, const char* pattern, const char* what)
{
    const auto hits = FindAll(game.text, ParsePattern(pattern));
    if (hits.size() == 1) return hits[0];
    Log("[ledger] %s %s: its column is -1", what, hits.empty() ? "not found" : "ambiguous");
    return nullptr;
}

void FindPso(const GameModule& game)
{
    uint8_t* const c = FindOne(game, kPsoCounters, "the shader pipeline cache's precompile counters");
    uint8_t* const g = FindOne(game, kPsoGetter, "the shader pipeline cache's precompiling flag");
    if (c) {
        uint8_t* const tasks = RipTarget(c + kPsoTasksInsn, 4, 8);
        uint8_t* const time = RipTarget(c + kPsoTimeInsn, 4, 8);
        if (game.data.Contains(tasks) && game.data.Contains(time) && (reinterpret_cast<uintptr_t>(tasks) & 7) == 0 &&
            (reinterpret_cast<uintptr_t>(time) & 7) == 0) {
            st.psoTasks = reinterpret_cast<volatile int64_t*>(tasks);
            st.psoTime = reinterpret_cast<volatile int64_t*>(time);
        } else Log("[ledger] the precompile counters are not in the game's data: pso columns -1");
    }
    if (g) {
        uint8_t* const single = RipTarget(g, 3, 7);
        uint32_t flag;
        memcpy(&flag, g + kPsoGetterFlag, 4);
        if (game.data.Contains(single) && flag < 0x1000) {
            st.psoSingleton = reinterpret_cast<uint8_t* const*>(single);
            st.psoFlag = flag;
        }
    }
}

int32_t PsoActive()
{
    if (!st.psoSingleton) return -1;
    __try {
        const uint8_t* const s = *st.psoSingleton;
        return s ? (s[st.psoFlag] ? 1 : 0) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int StateOf(int32_t named, int32_t mode, bool travel, bool known)
{
    if (travel) return kLoading;
    if (!known) return kOther;
    if (named > 0) return kMenu;
    if (mode == 1) return kLoading;
    if (mode == 0) return kPlay;
    return kOther;
}

void FlushSummary(uint64_t now)
{
    Record r{};
    r.kind = 2;
    r.frame = g_frames;
    r.endUs = now;
    r.sum = g_sum;
    r.dropped = g_dropped.exchange(0, std::memory_order_relaxed);
    Push(r);
    g_sum = Summary{};
    g_summaryStart = now;
}

// ---- the csv ----
const char kFrameColumns[] =
    "F,frame,end_ms,period_ms,sleep_ms,pre_ms,actor_ms,post_ms,sync_ms,tg0_ms,tg1_ms,tg2_ms,tg3_ms,tg4_ms,tg5_ms,tg6_ms,tg7_ms,"
    "pair_ms,flow_ms,unit_ms,units,grid_ms,grid_n,bodies_ms,fade_ms,gpu_ms,gc_ms,pso_tasks,pso_ms,named,mode,state,"
    "since_change,since_fade,cap_ms,flags,x,y,z";
const char kSummaryColumns[] =
    "S,frame,end_ms,frames,unfocused_play_s,dropped,"
    "play_n,play_s,play_slow,play_excess_ms,play_h0,play_h1,play_h2,play_h3,play_h4,play_h5,"
    "menu_n,menu_s,menu_slow,menu_excess_ms,menu_h0,menu_h1,menu_h2,menu_h3,menu_h4,menu_h5,"
    "loading_n,loading_s,loading_slow,loading_excess_ms,loading_h0,loading_h1,loading_h2,loading_h3,loading_h4,loading_h5,"
    "other_n,other_s,other_slow,other_excess_ms,other_h0,other_h1,other_h2,other_h3,other_h4,other_h5";

double Ms(uint64_t us) { return static_cast<double>(us) / 1000.0; }
double MsI(int32_t us) { return us < 0 ? -1.0 : us / 1000.0; }

int FormatRecord(const Record& r, char* buf, size_t size)
{
    if (r.kind == 1) {
        return snprintf(
            buf, size,
            "F,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%.3f,%u,%.3f,"
            "%.3f,%.3f,%.3f,%d,%.3f,%d,%d,%s,%u,%u,%.3f,%u,%.0f,%.0f,%.0f\n",
            static_cast<unsigned long long>(r.frame), Ms(r.endUs), Ms(r.periodUs), Ms(r.sleepUs), Ms(r.preUs), Ms(r.actorUs),
            Ms(r.postUs), Ms(r.syncUs), Ms(r.tgUs[0]), Ms(r.tgUs[1]), Ms(r.tgUs[2]), Ms(r.tgUs[3]), Ms(r.tgUs[4]),
            Ms(r.tgUs[5]), Ms(r.tgUs[6]), Ms(r.tgUs[7]), Ms(r.pairUs), Ms(r.flowUs), Ms(r.unitUs), r.units, Ms(r.gridUs),
            r.gridN, Ms(r.bodiesUs), Ms(r.fadeUs), MsI(r.gpuUs), MsI(r.gcUs), r.psoTasks, MsI(r.psoUs), r.named, r.mode,
            r.state < kStates ? kStateName[r.state] : "?", r.sinceChange, r.sinceFade, Ms(r.capUs), r.flags, r.px, r.py,
            r.pz);
    }
    int k = snprintf(buf, size, "S,%llu,%.3f,%llu,%.3f,%u", static_cast<unsigned long long>(r.frame), Ms(r.endUs),
                     static_cast<unsigned long long>(r.sum.frames), r.sum.unfocusedPlayUs / 1e6, r.dropped);
    for (int s = 0; s < kStates && k > 0 && static_cast<size_t>(k) < size; ++s) {
        k += snprintf(buf + k, size - k, ",%u,%.3f,%u,%.3f", r.sum.n[s], r.sum.us[s] / 1e6, r.sum.slowN[s], Ms(r.sum.slowExcessUs[s]));
        for (int b = 0; b < kBins && static_cast<size_t>(k) < size; ++b) k += snprintf(buf + k, size - k, ",%u", r.sum.hist[s][b]);
    }
    if (k > 0 && static_cast<size_t>(k) + 1 < size) {
        buf[k++] = '\n';
        buf[k] = 0;
    }
    return k;
}

void WriteLine(const char* line, int n)
{
    if (!g_file || g_fileFull || n <= 0) return;
    if (g_fileBytes + n > kMaxFileBytes) {
        g_fileFull = true;
        fputs("# size limit reached: no more rows\n", g_file);
        fflush(g_file);
        Log("[ledger] OblivionPerfFix-ledger.csv reached %ld MB: no more rows this session", kMaxFileBytes >> 20);
        return;
    }
    fwrite(line, 1, static_cast<size_t>(n), g_file);
    g_fileBytes += n;
}

bool OpenFile()
{
    if (g_file) return true;
    if (g_fileFailed || g_path.empty()) return false;
    // the previous session's file is kept as OblivionPerfFix-ledger.1.csv
    const std::wstring prev = g_path.substr(0, g_path.size() - 4) + L".1.csv";
    MoveFileExW(g_path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfsopen(g_path.c_str(), L"w", _SH_DENYWR);
    if (!g_file) {
        g_fileFailed = true;
        Log("[ledger] could not create %ls: no ledger this session", g_path.c_str());
        return false;
    }
    char head[1024];
    const int n = snprintf(head, sizeof head,
                           "# OblivionPerfFix-ledger v1, OblivionPerfFix " OPF_VERSION ", qpc_freq=%lld, pid=%lu, threshold_ms=%.1f, "
                           "signals=tg:%d,sync:%d,grid:%d,pso:%d,gpu:%d,pacing:%d,gc:0\n"
                           "# times: end_ms = QueryPerformanceCounter / frequency in ms (the lab log's qpc base); see "
                           "docs/stutter-ledger.md\n#%s\n#%s\n",
                           static_cast<long long>(g_freq.QuadPart), GetCurrentProcessId(),
                           g_thresholdUs.load() / 1000.0, st.tg != nullptr, st.sync != nullptr, st.grid != nullptr,
                           st.psoTasks != nullptr, st.gpu != nullptr, g_pacing, kFrameColumns, kSummaryColumns);
    WriteLine(head, n);
    fflush(g_file);
    Log("[ledger] writing slow frames (>= %.0f ms) to %ls", g_thresholdUs.load() / 1000.0, g_path.c_str());
    return true;
}

}  // namespace

// ---- GGPUFrameTime (shared with fix_npc_pacing's lab line) ----
// UE's GGPUFrameTime (uint32 cycles of FPlatformTime), written by the D3D12 RHI's ProcessTimestamps every frame (in
// shipping too). Found from the cvar r.D3D12.GPUTimeFromTimestamps: its registration stores the TConsoleVariableData
// pointer (after call [rax+58h]); ProcessTimestamps is that pointer's one reader, and its first `mov [rip+x], eax` after
// the read is the store of GGPUFrameTime. nullptr if not found (logged once).
const volatile uint32_t* FindGpuFrameTime()
{
    static const volatile uint32_t* found = nullptr;
    static bool searched = false;
    if (searched) return found;
    searched = true;
    const GameModule& game = Game();
    static const wchar_t kCvar[] = L"r.D3D12.GPUTimeFromTimestamps";
    const uint8_t* name = nullptr;
    int names = 0;
    for (const uint8_t* q = game.rdata.begin; q + sizeof(kCvar) <= game.rdata.end; q += 2)
        if (memcmp(q, kCvar, sizeof(kCvar)) == 0) { name = q; ++names; }
    if (names != 1) { Log("[gputime] cvar name found %d times: GPU time unavailable", names); return nullptr; }
    const uint8_t* site = nullptr;
    int sites = 0;
    for (const uint8_t* q = game.text.begin; q + 7 <= game.text.end; ++q)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x15 && RipTarget(q, 3, 7) == name) { site = q; ++sites; }
    if (sites != 1) { Log("[gputime] registration found %d times: GPU time unavailable", sites); return nullptr; }
    const uint8_t* slot = nullptr;
    for (const uint8_t* q = site; q < site + 0x100 && !slot; ++q)
        if (q[0] == 0xFF && q[1] == 0x50 && q[2] == 0x58)
            for (const uint8_t* r = q + 3; r < q + 24; ++r)
                if (r[0] == 0x48 && r[1] == 0x89 && r[2] == 0x05) { slot = RipTarget(r, 3, 7); break; }
    if (!slot || !game.data.Contains(slot)) { Log("[gputime] value pointer not found: GPU time unavailable"); return nullptr; }
    const uint8_t* reader = nullptr;
    int readers = 0;
    for (const uint8_t* q = game.text.begin; q + 7 <= game.text.end; ++q)
        if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8B && (q[2] & 0xC7) == 0x05 && RipTarget(q, 3, 7) == slot) { reader = q; ++readers; }
    if (readers != 1) { Log("[gputime] value pointer read %d times (expected once): GPU time unavailable", readers); return nullptr; }
    for (const uint8_t* q = reader; q < reader + 0x80; ++q)
        if (q[0] == 0x89 && q[1] == 0x05) {
            uint8_t* const t = RipTarget(q, 2, 6);
            if (!game.data.Contains(t)) break;
            found = reinterpret_cast<volatile uint32_t*>(t);
            Log("[gputime] GGPUFrameTime at exe+0x%X (ProcessTimestamps reads the cvar at exe+0x%X)",
                static_cast<unsigned>(t - game.base), static_cast<unsigned>(reader - game.base));
            return found;
        }
    Log("[gputime] no store after the cvar read: GPU time unavailable");
    return nullptr;
}

// ---- start-up ----
bool InstallHitchLedger(bool beforeGameCode, const wchar_t* dir, bool pacingInstalled)
{
    if (g_installed) return true;
    QueryPerformanceFrequency(&g_freq);
    g_cycleUs = 1e6 / static_cast<double>(g_freq.QuadPart);
    g_pacing = pacingInstalled;
    if (dir) g_path = std::wstring(dir) + L"\\OblivionPerfFix-ledger.csv";
    if (!beforeGameCode) {
        Log("[ledger] HitchLedger: can only be installed at start-up (version.dll), not installed");
        return false;
    }
    const GameModule& game = Game();
    st.region = AllocNearGame(256);
    if (!st.region) {
        Log("[ledger] no memory near the game's code: not installed");
        return false;
    }
    // tick groups: entry detour (trampoline at +0, hook jump at +32)
    if (uint8_t* tg = FindOne(game, kTgEntry, "FTickTaskSequencer::ReleaseTickGroup")) {
        g_tgOrig = reinterpret_cast<TgFn>(Trampoline(tg, kTgPrologue, reinterpret_cast<void*>(&TgHook), st.region));
        st.tg = tg;
    }
    // the frame-end sync: its call redirected through a stub at +64
    if (uint8_t* call = FindOne(game, kSyncCall, "FEngineLoop::Tick's FFrameEndSync::Sync call")) {
        call += kSyncCallAt;
        uint8_t* const target = RipTarget(call, 1, 5);
        if (game.text.Contains(target) && MatchesAt(target, ParsePattern(kSyncFn))) {
            const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
            void* const hook = reinterpret_cast<void*>(&SyncHook);
            memcpy(st.region + 64, jmpAbs, 6);
            memcpy(st.region + 70, &hook, 8);
            g_syncOrig = reinterpret_cast<SyncFn>(target);
            st.syncCall = call;
            st.sync = target;
        } else Log("[ledger] the frame-end sync call's target is not FFrameEndSync::Sync as expected: sync column -1");
    }
    FindPso(game);
    st.gpu = FindGpuFrameTime();
    DWORD old;
    VirtualProtect(st.region, 256, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), st.region, 256);
    if (st.tg && !PatchJmp(st.tg, kTgPrologue, st.region + 32)) {
        Log("[ledger] could not place the tick-group detour: tg columns -1");
        st.tg = nullptr;
    }
    if (st.syncCall) {
        const int64_t rel = (st.region + 64) - (st.syncCall + 5);
        const int32_t rel32 = static_cast<int32_t>(rel);
        if (rel != rel32 || !WriteCode(st.syncCall + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4))) {
            Log("[ledger] could not redirect the frame-end sync call: sync column -1");
            st.syncCall = st.sync = nullptr;
        }
    }
    g_installed = true;
    Log("[ledger] HitchLedger installed: tick groups %s, frame-end sync %s, precompile counters %s, GPU time %s, frames %s "
        "(the OblivionThread's grid change is added last)",
        st.tg ? "timed" : "-", st.sync ? "timed" : "-", st.psoTasks ? (st.psoSingleton ? "read" : "read (no flag)") : "-",
        st.gpu ? "read" : "-", g_pacing ? "from NpcCreationPacing's frame hooks" : "UNAVAILABLE (needs NpcCreationPacing=1 at start-up)");
    return true;
}

// Last, after the lab's CellRowDiag (which times the same entry in lab builds; its detour wins there).
bool InstallHitchLedgerGrid(bool beforeGameCode)
{
    if (!g_installed || !beforeGameCode || st.grid) return st.grid != nullptr;
    const GameModule& game = Game();
    uint8_t* const fn = FindOne(game, kGridEntry, "the OblivionThread's cell-grid transition");
    if (!fn) return false;
    uint8_t* const slot = AllocNearGame(64);
    if (!slot) return false;
    g_gridOrig = reinterpret_cast<GridFn>(Trampoline(fn, kGridPrologue, reinterpret_cast<void*>(&GridHook), slot));
    DWORD old;
    VirtualProtect(slot, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, 64);
    if (!PatchJmp(fn, kGridPrologue, slot + 32)) {
        Log("[ledger] could not place the grid-transition detour: grid columns 0");
        return false;
    }
    st.grid = fn;
    Log("[ledger] the OblivionThread's cell-grid transition timed (exe+0x%X)", static_cast<unsigned>(fn - game.base));
    return true;
}

// ---- monitor thread ----
void SetHitchLedger(bool on, int thresholdMs)
{
    const int ms = thresholdMs < 5 ? 5 : thresholdMs > 1000 ? 1000 : thresholdMs;
    g_thresholdUs.store(static_cast<uint32_t>(ms) * 1000, std::memory_order_relaxed);
    if (!g_installed) {
        static bool logged = false;
        if (on && !logged) {
            logged = true;
            Log("[ledger] HitchLedger=1 but the ledger was not installed at start-up: restart the game to record");
        }
        return;
    }
    if (g_on.exchange(on) != on) Log("[ledger] %s", on ? "recording slow frames" : "stopped");
}

void HitchLedgerDrain()
{
    if (!g_installed) return;
    // focus (1 s resolution): the game's window has the keyboard focus (user32 resolved at run time: the DLL does not
    // link it; the game has it loaded)
    using ForegroundFn = HWND(WINAPI*)();
    using WindowPidFn = DWORD(WINAPI*)(HWND, LPDWORD);
    static ForegroundFn foreground = nullptr;
    static WindowPidFn windowPid = nullptr;
    if (!foreground)
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            windowPid = reinterpret_cast<WindowPidFn>(GetProcAddress(user32, "GetWindowThreadProcessId"));
            foreground = reinterpret_cast<ForegroundFn>(GetProcAddress(user32, "GetForegroundWindow"));
        }
    DWORD pid = 0;
    if (foreground && windowPid)
        if (HWND fg = foreground()) windowPid(fg, &pid);
    g_focused.store(!foreground || pid == GetCurrentProcessId(), std::memory_order_relaxed);
    if (!g_on.load(std::memory_order_relaxed) && g_head.load() == g_tail.load(std::memory_order_relaxed)) {
        if (g_file) fflush(g_file);
        return;
    }
    static unsigned idle = 0;
    if (!g_pacing && !g_noFramesLogged && ++idle == 30) {
        g_noFramesLogged = true;
        Log("[ledger] no frames: the ledger needs NpcCreationPacing's frame hooks (NpcCreationPacing=1 at start-up)");
    }
    if (!OpenFile()) {
        g_tail.store(g_head.load(std::memory_order_acquire), std::memory_order_release);
        return;
    }
    char line[1600];
    for (uint64_t t = g_tail.load(std::memory_order_relaxed); t != g_head.load(std::memory_order_acquire);) {
        const Record r = g_ring[t % kRing];
        g_tail.store(++t, std::memory_order_release);
        WriteLine(line, FormatRecord(r, line, sizeof line));
        ++g_written;
    }
    fflush(g_file);
}

// ---- game thread: called from NpcCreationPacing's frame hooks (npc_pacing.cpp) and the fade hook ----
void LedgerPreActorTick()
{
    if (g_on.load(std::memory_order_relaxed) && !f.preTs && OnGt()) {
        f.preTs = Us();
    }
}

void LedgerPostActorTick()
{
    if (g_on.load(std::memory_order_relaxed) && f.preTs && !f.postTs && OnGt()) f.postTs = Us();
}

void LedgerSleepBegin()
{
    if (g_on.load(std::memory_order_relaxed) && OnGt()) f.sleepTs = Us();
}

void LedgerFadeToGame(uint64_t us)
{
    if (!g_on.load(std::memory_order_relaxed) || !OnGt()) return;
    f.fade = true;
    f.fadeUs += us;
}

uint64_t LedgerClockUs() { return g_installed ? Us() : 0; }

bool LedgerIsGameThread() { return g_installed && OnGt(); }

void LedgerFrameEnd(const LedgerPacing& p)
{
    if (!g_installed) return;
    if (!g_gtId.load(std::memory_order_relaxed)) g_gtId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (!OnGt()) return;
    const bool on = g_on.load(std::memory_order_relaxed);
    const uint64_t now = Us();
    const uint64_t bodies = StaticBodiesGtUs();
    if (!on) {
        g_wasOn = false;
        f = FrameAcc{};
        f.startUs = now;
        g_lastPacing = p;
        g_lastBodies = bodies;
        g_gridUs.store(0, std::memory_order_relaxed);
        g_gridN.store(0, std::memory_order_relaxed);
        if (st.psoTasks) { g_lastPsoTasks = *st.psoTasks; g_lastPsoTime = *st.psoTime; }
        return;
    }
    const bool first = !g_wasOn;
    if (first) {
        g_wasOn = true;
        g_summaryStart = now;
        g_sum = Summary{};
        if (!f.startUs) f.startUs = now;
    }
    ++g_frames;
    const uint64_t period = now - f.startUs;
    const uint64_t sleepTs = f.sleepTs && f.sleepTs >= f.startUs ? f.sleepTs : now;
    // state
    int32_t named = -1, mode = -1;
    const bool known = ReadMenuFreeze(&named, &mode);
    const int state = StateOf(named, mode, p.travel, known);
    if (state != g_lastState) g_sinceChange = 0;
    else if (g_sinceChange < kSat16) ++g_sinceChange;
    g_lastState = state;
    if (f.fade) g_sinceFade = 0;
    else if (g_sinceFade < kSat16) ++g_sinceFade;
    const bool focused = g_focused.load(std::memory_order_relaxed);
    // the summary's denominators
    g_sum.frames++;
    g_sum.n[state]++;
    g_sum.us[state] += period;
    if (!focused && state == kPlay) g_sum.unfocusedPlayUs += period;
    int bin = 0;
    while (bin < kBins - 1 && period >= kBinEdgeUs[bin]) ++bin;
    g_sum.hist[state][bin]++;
    const uint64_t capUs = p.capPeriodUs > 1000 ? static_cast<uint64_t>(p.capPeriodUs) : 16667;
    const uint32_t threshold = g_thresholdUs.load(std::memory_order_relaxed);
    const uint64_t gridUs = g_gridUs.exchange(0, std::memory_order_relaxed);
    const uint64_t gridN = g_gridN.exchange(0, std::memory_order_relaxed);
    int64_t psoTasks = -1, psoTime = -1;
    if (st.psoTasks) {
        const int64_t t = *st.psoTasks, c = *st.psoTime;
        psoTasks = t - g_lastPsoTasks;
        psoTime = c - g_lastPsoTime;
        g_lastPsoTasks = t;
        g_lastPsoTime = c;
    }
    if (period >= threshold && !first) {
        g_sum.slowN[state]++;
        g_sum.slowExcessUs[state] += period > capUs ? period - capUs : 0;
        Record r{};
        r.kind = 1;
        r.frame = g_frames;
        r.endUs = now;
        r.periodUs = U32(period);
        r.sleepUs = U32(now - sleepTs);
        const bool hasPre = f.preTs >= f.startUs && f.preTs && f.preTs <= sleepTs;
        const bool hasPost = hasPre && f.postTs >= f.preTs && f.postTs <= sleepTs;
        r.preUs = U32((hasPre ? f.preTs : sleepTs) - f.startUs);
        r.actorUs = hasPost ? U32(f.postTs - f.preTs) : hasPre ? U32(sleepTs - f.preTs) : 0;
        r.postUs = hasPost ? U32(sleepTs - f.postTs) : 0;
        r.syncUs = st.sync ? U32(f.syncUs) : 0xFFFFFFFFu;
        for (int i = 0; i < kGroups; ++i) r.tgUs[i] = f.tg[i];
        r.pairUs = U32(p.pairCumUs - g_lastPacing.pairCumUs);
        r.flowUs = U32(p.flowCumUs - g_lastPacing.flowCumUs);
        r.unitUs = U32(p.unitCumUs - g_lastPacing.unitCumUs);
        r.units = U32(p.unitCumN - g_lastPacing.unitCumN);
        r.gridUs = U32(gridUs);
        r.gridN = U32(gridN);
        r.bodiesUs = U32(bodies - g_lastBodies);
        r.fadeUs = U32(f.fadeUs);
        r.gpuUs = st.gpu ? static_cast<int32_t>(*st.gpu * g_cycleUs) : -1;
        r.gcUs = -1;
        r.psoTasks = static_cast<int32_t>(psoTasks);
        r.psoUs = psoTime < 0 ? -1 : static_cast<int32_t>(static_cast<double>(psoTime) * g_cycleUs);
        r.named = named;
        r.mode = mode;
        r.state = static_cast<uint32_t>(state);
        r.sinceChange = g_sinceChange;
        r.sinceFade = g_sinceFade;
        r.capUs = U32(p.capPeriodUs > 0 ? static_cast<uint64_t>(p.capPeriodUs) : 0);
        const int32_t pso = PsoActive();
        r.flags = (hasPre ? kHasPre : 0) | (hasPost ? kHasPost : 0) | (f.fade ? kFade : 0) | (gridUs >= 1000 ? kGrid : 0) |
                  (focused ? 0 : kUnfocused) | (p.travel ? kTravel : 0) | (pso > 0 ? kPsoActive : 0);
        double x = 0, y = 0, z = 0;
        if (NpcPacingPlayerPos(&x, &y, &z)) {
            r.px = static_cast<float>(x);
            r.py = static_cast<float>(y);
            r.pz = static_cast<float>(z);
        }
        Push(r);
    }
    if (now - g_summaryStart >= kSummaryUs) FlushSummary(now);
    g_lastPacing = p;
    g_lastBodies = bodies;
    f = FrameAcc{};
    f.startUs = now;
}

bool HitchLedgerInfo(HitchLedgerInfoOut* out)
{
    if (!out) return false;
    out->installed = g_installed;
    out->tg = st.tg;
    out->syncCall = st.syncCall;
    out->sync = st.sync;
    out->grid = st.grid;
    out->psoTasks = const_cast<int64_t*>(st.psoTasks);
    out->psoTime = const_cast<int64_t*>(st.psoTime);
    out->psoSingleton = const_cast<uint8_t**>(st.psoSingleton);
    out->psoFlag = st.psoFlag;
    out->gpu = const_cast<uint32_t*>(st.gpu);
    out->tgHook = reinterpret_cast<void*>(&TgHook);
    out->syncHook = reinterpret_cast<void*>(&SyncHook);
    out->gridHook = reinterpret_cast<void*>(&GridHook);
    out->recordSize = sizeof(Record);
    return true;
}

void HitchLedgerTestSetup(uint64_t (*clock)(), void* fakeTg, void* fakeSync, const wchar_t* path)
{
    g_testClock = clock;
    if (fakeTg) g_tgOrig = reinterpret_cast<TgFn>(fakeTg);
    if (fakeSync) g_syncOrig = reinterpret_cast<SyncFn>(fakeSync);
    if (path) {
        if (g_file) { fclose(g_file); g_file = nullptr; }
        g_path = path;
        g_fileBytes = 0;
        g_fileFull = g_fileFailed = false;
    }
    g_gtId.store(GetCurrentThreadId());
}

void HitchLedgerTestGridAdd(uint64_t us)
{
    g_gridUs.fetch_add(us);
    g_gridN.fetch_add(1);
}

void HitchLedgerTestClose()
{
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

}  // namespace opf
