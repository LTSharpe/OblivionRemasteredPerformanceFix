// CellObjectPreload (worker preload 2026-11-18, S-008; [Fixes] CellObjectPreload, default 0 until a release flips it):
// the files of the objects placed in a cell that RowPreload has loaded ahead are asked for in the background, so the cell's
// creates find them loaded. Installed at start-up when != 0 (version.dll route), then live 0/1.
//
// What it fixes (findings "loadlead", "play1117"): when a cell is switched on, the old engine's create command makes a UE
// actor for each placed item (books, food, quills, alchemy apparatus, clutter); Altar's create looks up the base form's
// UE-side form asset and then the actor class (CreatePreload's file comment has the two steps), and when either is not
// resident it loads it synchronously: LoadPackage = LoadPackageAsync at INT32_MAX plus a flush, so the game thread waits for
// the loader (1.4-3.7 ms each; at the bend before Aleswell 11.9 ms of a 27.1 ms frame were such loads). CreatePreload covers
// only creates the NPC pacing may defer; a cell's placed items are created inside the level flow's drain, at once.
//
// The fix: RowPreload (streaming/row_preload.cpp) loads the next row of cells on the old-engine thread a few seconds before
// the crossing switches them on. At that point this walks each such cell's reference list (cell +0xB8, as the attach walks
// it) and notes the base form of every reference that has no UE actor yet (characters too: their form asset and class are
// the same two packages), except static world objects (base type STAT 0x1C: rocks, walls, houses), which the cell's level
// loads in the background by itself. At the game thread's frame end each new base form is looked up with CreatePreload's
// readers (create_preload.cpp CreatePreloadAheadStep): if its form asset is not resident it is requested once
// (LoadPackageAsync, no delegate, at a priority below the game's own background loads; a create that needs it first raises
// it to its own); once that has loaded, its actor class is requested the same way. Nothing waits for anything and no create
// is moved: the creates run when and as the game runs them, and find their packages loaded (or, if a load is still in
// flight, the create's own flush waits only for the rest).
//
// Bounds: at most 16 requests and 48 look-ups per frame (16 of them kept for new base forms); at most 256 form assets
// followed while they load (looked at again every 4 frames, never requested again, given up after 3 s); a base form is noted
// again only 20 s after its last note; the queues are emptied at a menu or loading-screen freeze, a pending fast travel and
// when the key is switched off (and the bases they held may then be noted again). Nothing is held: a package
// loaded here is referenced by nothing until the create uses it, so the next garbage collection frees it if the cell is
// never switched on (the player turned away), as it frees anything else no longer used.
// Equivalent: the same packages with the same contents through the same loader, earlier; which actors are made, when and
// how does not change. The engine's timer garbage collection does not start while any package is loading
// (UEngine::PerformGarbageCollectionAndCleanupActors, UE 5.3, gc.PerformGCWhileAsyncLoading 0), so a pass can move by the
// tens of milliseconds a batch of these loads takes, as it does for any of the game's own loads.
//
// Found by pattern: the cell reference walk (create_ahead.cpp CellRefWalkFound: the attach's walk and Spawn_Paired_Actor's
// reads), StaticLoadObject's still-loading flag test (create_preload.cpp CreatePreloadAheadDiscover); CreatePreload's
// lookup readers, ResolveObject, ToString and LoadPackageAsync (it must be installed and on), RowPreload installed. Any
// mismatch: not installed, one log line.
#include "common.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace opf {
namespace {

constexpr size_t kCellRefs = 0xB8, kFormType = 0x08, kFormId = 0x10, kRefPaired = 0x48, kRefBase = 0x50;
constexpr uint8_t kTypeStat = 0x1C;  // TESForm type byte +0x08 (the byte the pinned kWalkType reads for references)
constexpr int kMaxWalk = 4096, kCellBases = 256;
constexpr int kOtQueue = 2048, kTodo = 4096, kMaxPending = 256, kSeenTab = 1 << 13;
constexpr int kReqCap = 16, kLookCap = 48, kLookNew = 16, kLookFrames = 4;
constexpr uint64_t kGiveUpMs = 3000, kAgainMs = 20000;

using StepFn = int (*)(uint32_t form, uint32_t ask, uint32_t* requested);  // ask bit 0 form asset, bit 1 class
using NowFn = uint64_t (*)();
uint64_t TickMs() { return GetTickCount64(); }

struct Pending {
    uint32_t base;
    uint64_t t0;
    uint64_t next;  // frame of the next look
};

struct State {
    std::atomic<bool> installed{false};
    std::atomic<int> mode{0};
    std::atomic<uint32_t> gen{0};  // +1 at each switch on: the old-engine thread forgets its notes
    StepFn step = nullptr;
    NowFn now = &TickMs;
    // old-engine thread
    uint32_t otGen = 0;
    uint32_t seenId[kSeenTab] = {};  // base form id + 1 (0 = free)
    uint64_t seenAt[kSeenTab] = {};
    // old-engine thread -> game thread
    std::mutex mu;
    uint32_t q[kOtQueue];
    int nq = 0;
    // game thread
    uint32_t todo[kTodo];
    int todoHead = 0, todoN = 0;
    Pending pend[kMaxPending];
    int nPend = 0;
    uint64_t frame = 0;
    bool test = false;
    // stats
    std::atomic<uint64_t> cells{0}, refs{0}, queued{0}, otDrop{0}, walkFaults{0}, statSkip{0};
    std::atomic<uint64_t> looks{0}, resident{0}, nothing{0}, notReady{0}, formReqs{0}, classReqs{0}, gaveUp{0}, cleared{0};
    std::atomic<uint64_t> frames{0}, workUs{0}, workMaxUs{0}, pendMax{0};
    uint64_t lastSum = 0;
} g;

void MaxTo(std::atomic<uint64_t>& m, uint64_t v)
{
    uint64_t c = m.load(std::memory_order_relaxed);
    while (v > c && !m.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}

// The cell's references without a UE actor yet: their base form ids, each once (SEH: no C++ objects here).
int Walk(const uint8_t* cell, uint32_t* bases, int cap, uint32_t* refsOut, uint32_t* statOut)
{
    int n = 0;
    uint32_t refs = 0, stat = 0;
    __try {
        const uint8_t* node = cell + kCellRefs;
        for (int i = 0; node && i < kMaxWalk; ++i) {
            const uint8_t* const ref = *reinterpret_cast<const uint8_t* const*>(node);
            if (!ref) break;
            ++refs;
            if (*reinterpret_cast<const uint64_t*>(ref + kRefPaired) == 0) {
                const uint8_t* const base = *reinterpret_cast<const uint8_t* const*>(ref + kRefBase);
                const uint32_t id = base ? *reinterpret_cast<const uint32_t*>(base + kFormId) : 0;
                if (id && base[kFormType] == kTypeStat) ++stat;  // the cell's level loads static objects in the background
                else if (id && id < 0xFF000000u) {  // run-time forms (FF......) have no placed asset of their own
                    int k = 0;
                    while (k < n && bases[k] != id) ++k;
                    if (k == n && n < cap) bases[n++] = id;
                }
            }
            node = *reinterpret_cast<const uint8_t* const*>(node + 8);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g.walkFaults.fetch_add(1, std::memory_order_relaxed);
        n = 0;
    }
    *refsOut = refs;
    *statOut = stat;
    return n;
}

// Old-engine thread: true = note this base form now (not noted in the last kAgainMs).
bool NoteAgain(uint32_t id, uint64_t now)
{
    uint32_t h = (id * 2654435761u) >> 19;
    for (int i = 0; i < 32; ++i, h = (h + 1) & (kSeenTab - 1)) {
        if (g.seenId[h] == id + 1) {
            if (now - g.seenAt[h] < kAgainMs) return false;
            g.seenAt[h] = now;
            return true;
        }
        if (!g.seenId[h]) {
            g.seenId[h] = id + 1;
            g.seenAt[h] = now;
            return true;
        }
    }
    return true;  // no room in the run: noted (the game thread's look-up is cheap for a resident one)
}

void ClearGt()
{
    bool dropped = g.todoN || g.nPend;
    g.todoHead = g.todoN = 0;
    g.nPend = 0;
    {
        std::lock_guard<std::mutex> lock(g.mu);
        dropped = dropped || g.nq;
        g.nq = 0;
    }
    if (dropped) {
        g.cleared.fetch_add(1, std::memory_order_relaxed);
        g.gen.fetch_add(1, std::memory_order_relaxed);  // the old-engine thread forgets its notes: those bases may come again
    }
}

// One look-up: the state CreatePreloadAheadStep returns; *req = what it requested (ask: what it may request).
int Look(uint32_t base, uint32_t ask, uint32_t* req)
{
    g.looks.fetch_add(1, std::memory_order_relaxed);
    const int st = g.step(base, ask, req);
    if (*req & 1) g.formReqs.fetch_add(1, std::memory_order_relaxed);
    if (*req & 2) g.classReqs.fetch_add(1, std::memory_order_relaxed);
    return st;
}

}  // namespace

bool CellObjectPreloadDiscover(const GameModule& game, const char** missing)
{
    if (!CellRefWalkFound(game, missing)) return false;
    return CreatePreloadAheadDiscover(game, missing);
}

bool InstallCellObjectPreload(bool beforeGameCode, bool createPreloadOn, bool rowPreloadOn)
{
    if (g.installed.load()) return true;
    if (!beforeGameCode) {
        Log("[cellpreload] CellObjectPreload: start-up only (loaded late here), not installed");
        return false;
    }
    if (!createPreloadOn) {
        Log("[cellpreload] CellObjectPreload: needs CreatePreload (installed and on), not installed");
        return false;
    }
    if (!rowPreloadOn) {
        Log("[cellpreload] CellObjectPreload: needs RowPreload (installed), not installed");
        return false;
    }
    const char* missing = nullptr;
    if (!CellObjectPreloadDiscover(Game(), &missing)) {
        Log("[cellpreload] CellObjectPreload: %s not recognised, not installed", missing ? missing : "?");
        return false;
    }
    g.step = &CreatePreloadAheadStep;
    g.now = &TickMs;
    g.installed.store(true);
    Log("[cellpreload] CellObjectPreload installed: the files of objects in cells RowPreload loads ahead are requested in the "
        "background (static objects left to the level; at most %d requests per frame, %d form assets followed, a base form "
        "again after %llu s)",
        kReqCap, kMaxPending, static_cast<unsigned long long>(kAgainMs / 1000));
    return true;
}

void SetCellObjectPreload(int mode)
{
    const int m = mode ? 1 : 0;
    const int was = g.mode.exchange(m);
    if (was != m && m) g.gen.fetch_add(1, std::memory_order_relaxed);
    if (was != m && g.installed.load())
        Log("[cellpreload] objects' files of cells loaded ahead are %s (CellObjectPreload %d)",
            m ? "requested in the background" : "not requested: each create loads its own, as the game does", m);
}

// streaming/row_preload.cpp, old-engine thread: a cell loaded ahead (or found buffered) for the next grid shift.
void CellObjectPreloadCell(const uint8_t* cell)
{
    if (!g.installed.load(std::memory_order_relaxed) || !g.mode.load(std::memory_order_relaxed) || !cell) return;
    const uint32_t gen = g.gen.load(std::memory_order_relaxed);
    if (gen != g.otGen) {
        memset(g.seenId, 0, sizeof g.seenId);
        g.otGen = gen;
    }
    uint32_t bases[kCellBases];
    uint32_t refs = 0, stat = 0;
    const int n = Walk(cell, bases, kCellBases, &refs, &stat);
    g.cells.fetch_add(1, std::memory_order_relaxed);
    g.refs.fetch_add(refs, std::memory_order_relaxed);
    g.statSkip.fetch_add(stat, std::memory_order_relaxed);
    const uint64_t now = g.now();
    uint32_t fresh[kCellBases];
    int nf = 0;
    for (int i = 0; i < n; ++i)
        if (NoteAgain(bases[i], now)) fresh[nf++] = bases[i];
    if (!nf) return;
    std::lock_guard<std::mutex> lock(g.mu);
    for (int i = 0; i < nf; ++i) {
        if (g.nq >= kOtQueue) {
            g.otDrop.fetch_add(static_cast<uint64_t>(nf - i), std::memory_order_relaxed);
            break;
        }
        g.q[g.nq++] = fresh[i];
        g.queued.fetch_add(1, std::memory_order_relaxed);
    }
}

// npcs/npc_pacing.cpp's frame-end hook, game thread. A menu or loading screen holding the game, or a pending fast travel
// (travel; the cells ahead are not where the player is going): nothing is requested and the queues are emptied.
void CellObjectPreloadFrame(bool travel)
{
    if (!g.installed.load(std::memory_order_relaxed)) return;
    if (!g.test && !NpcPacingOnGameThread()) return;
    bool frozen = travel;
    if (!g.test) {
        int32_t named = 0, fmode = 0;
        if (ReadMenuFreeze(&named, &fmode) && (named != 0 || fmode != 0)) frozen = true;
    }
    if (!g.mode.load(std::memory_order_relaxed) || frozen) {
        ClearGt();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g.mu);
        for (int i = 0; i < g.nq; ++i) {
            if (g.todoN >= kTodo) {
                g.otDrop.fetch_add(static_cast<uint64_t>(g.nq - i), std::memory_order_relaxed);
                break;
            }
            g.todo[(g.todoHead + g.todoN++) % kTodo] = g.q[i];
        }
        g.nq = 0;
    }
    ++g.frame;
    if (!g.todoN && !g.nPend) return;
    LARGE_INTEGER a, f;
    QueryPerformanceCounter(&a);
    const uint64_t now = g.now();
    int reqs = 0, looks = 0;
    // form assets in flight (requested once, never again): their class once they have loaded; kLookNew look-ups stay for
    // the new base forms below
    for (int i = 0; i < g.nPend && looks < kLookCap - kLookNew && reqs < kReqCap;) {
        Pending& p = g.pend[i];
        if (g.frame < p.next) {
            ++i;
            continue;
        }
        ++looks;
        uint32_t req = 0;
        const int st = Look(p.base, 2, &req);
        reqs += (req & 1) + ((req >> 1) & 1);
        if (st == 1 && now - p.t0 < kGiveUpMs) {
            p.next = g.frame + kLookFrames;
            ++i;
            continue;
        }
        if (st == 1) g.gaveUp.fetch_add(1, std::memory_order_relaxed);
        else if (st == 3) g.resident.fetch_add(1, std::memory_order_relaxed);
        g.pend[i] = g.pend[--g.nPend];
    }
    // new base forms
    while (g.todoN && looks < kLookCap && reqs < kReqCap && g.nPend < kMaxPending) {
        const uint32_t base = g.todo[g.todoHead];
        g.todoHead = (g.todoHead + 1) % kTodo;
        --g.todoN;
        ++looks;
        uint32_t req = 0;
        const int st = Look(base, 3, &req);
        reqs += (req & 1) + ((req >> 1) & 1);
        if (st < 0) g.notReady.fetch_add(1, std::memory_order_relaxed);
        else if (st == 0) g.nothing.fetch_add(1, std::memory_order_relaxed);
        else if (st == 3) g.resident.fetch_add(1, std::memory_order_relaxed);
        else if (st == 1) g.pend[g.nPend++] = {base, now, g.frame + kLookFrames};
        // st == 2: the class is requested (or already loading); nothing more to do
    }
    MaxTo(g.pendMax, static_cast<uint64_t>(g.nPend));
    LARGE_INTEGER b;
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    const uint64_t us = static_cast<uint64_t>((b.QuadPart - a.QuadPart) * 1000000 / (f.QuadPart ? f.QuadPart : 1));
    g.frames.fetch_add(1, std::memory_order_relaxed);
    g.workUs.fetch_add(us, std::memory_order_relaxed);
    MaxTo(g.workMaxUs, us);
}

void LogCellObjectPreloadStats()
{
    if (!g.installed.load(std::memory_order_relaxed)) return;
    const uint64_t cells = g.cells.load(), looks = g.looks.load(), fr = g.formReqs.load(), cr = g.classReqs.load();
    const uint64_t sum = cells + looks + fr + cr + g.cleared.load();
    if (sum == g.lastSum) return;
    g.lastSum = sum;
    const uint64_t frames = g.frames.load(), workUs = g.workUs.load();
    Log("[cellpreload] cells %llu (references %llu, static %llu), base forms noted %llu (dropped %llu), looked up %llu: loaded %llu, nothing to "
        "load %llu, not ready %llu; requests form asset %llu, class %llu; gave up %llu; followed at most %llu; frames with work "
        "%llu, %.3f ms mean, %.3f ms max; cleared %llu, walk faults %llu",
        static_cast<unsigned long long>(cells), static_cast<unsigned long long>(g.refs.load()),
        static_cast<unsigned long long>(g.statSkip.load()), static_cast<unsigned long long>(g.queued.load()), static_cast<unsigned long long>(g.otDrop.load()),
        static_cast<unsigned long long>(looks), static_cast<unsigned long long>(g.resident.load()),
        static_cast<unsigned long long>(g.nothing.load()), static_cast<unsigned long long>(g.notReady.load()),
        static_cast<unsigned long long>(fr), static_cast<unsigned long long>(cr), static_cast<unsigned long long>(g.gaveUp.load()),
        static_cast<unsigned long long>(g.pendMax.load()), static_cast<unsigned long long>(frames),
        frames ? static_cast<double>(workUs) / 1000.0 / static_cast<double>(frames) : 0.0, g.workMaxUs.load() / 1000.0,
        static_cast<unsigned long long>(g.cleared.load()), static_cast<unsigned long long>(g.walkFaults.load()));
}

// ---- self-test ----
void CellObjectPreloadTestSetup(int (*step)(uint32_t, uint32_t, uint32_t*), uint64_t (*now)())
{
    g.mode.store(0);
    g.test = step != nullptr;
    g.step = step;
    g.now = now ? now : &TickMs;
    g.installed.store(step != nullptr);
    g.todoHead = g.todoN = g.nPend = 0;
    g.nq = 0;
    g.frame = 0;
    memset(g.seenId, 0, sizeof g.seenId);
    for (auto* c : {&g.cells, &g.refs, &g.statSkip, &g.queued, &g.otDrop, &g.walkFaults, &g.looks, &g.resident, &g.nothing, &g.notReady, &g.formReqs,
                    &g.classReqs, &g.gaveUp, &g.cleared, &g.frames, &g.workUs, &g.workMaxUs, &g.pendMax})
        c->store(0);
}
CellObjectPreloadTestCounts CellObjectPreloadTestRead()
{
    CellObjectPreloadTestCounts c{};
    c.queued = static_cast<int>(g.queued.load());
    c.looks = static_cast<int>(g.looks.load());
    c.formReqs = static_cast<int>(g.formReqs.load());
    c.classReqs = static_cast<int>(g.classReqs.load());
    c.gaveUp = static_cast<int>(g.gaveUp.load());
    c.resident = static_cast<int>(g.resident.load());
    c.pending = g.nPend;
    c.todo = g.todoN;
    c.otQueue = g.nq;
    c.walkFaults = static_cast<int>(g.walkFaults.load());
    c.statSkip = static_cast<int>(g.statSkip.load());
    return c;
}

}  // namespace opf
