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
// Re-look at the grid change (CellObjectPreloadRelook, bookrelook 2026-11-23, S-008; [Fixes] CellObjectPreloadRelook, default 0): a
// package this key loaded ahead is referenced by nothing, so a garbage collection between the look-up and the create can free
// it (play114d: pass 33 came 0.6 s before the grid change; a book then loaded inside a 219 ms flush; booktest 2026-11-23: a
// forced pass between the look-up and the create freed the book's package 4 of 4 times). With the sub-option on, the cells
// this walked ahead are remembered with the base forms that walk found (at most 24 cells); at the grid change (old-engine
// thread, after the shift) the stored base forms of every remembered cell that is now in the grid are queued again and looked
// up at the game thread's next frame ends, without the 20 s wait, so what was freed since is requested again when the row is
// switched on: from a few frames (one sighting: 4 frames, 0.08 s) to a few tenths of a second before the object is made, at
// priority -1, i.e. behind the game's own loads of the new row (AsyncLoading2 orders event nodes and IO by priority), until the
// create raises it to its own. It can only shorten a create's in-frame load by that head start, not remove a congested loader's
// wait (PacingFlowWaitFrames is the lever for that). In the booktest the re-requested book package was resident again 78-94 ms
// later and no in-frame load was reproduced in any arm, so the gain is not yet shown. A base form whose form asset request this
// key still follows is skipped; other files in flight (a class package, a form asset the game itself asked for, one given up
// after 3 s) get a second request at priority -1, which UE 5.3's FindOrInsertPackage merges into the load in flight (it adds
// the request id and raises the priority only if the new one is higher). At most 256 base forms per grid change (then the
// usual 48 look-ups and 16 requests per frame); the shift reads no cell memory (stored ids; measured with the walk it
// replaced: 25 us mean, 54 us max over 32 grid changes on the Dasek Moor ride). It asks for packages earlier; nothing else
// changes. The re-look's look-ups and requests are included in the [cellpreload] line's main counters too.
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
constexpr int kAhead = 24, kRelookBases = 256;
constexpr uint64_t kGiveUpMs = 3000, kAgainMs = 20000;

using StepFn = int (*)(uint32_t form, uint32_t ask, uint32_t* requested);  // ask bit 0 form asset, bit 1 class
using NowFn = uint64_t (*)();
uint64_t TickMs() { return GetTickCount64(); }

struct Pending {
    uint32_t base;
    uint64_t t0;
    uint64_t next;  // frame of the next look
    uint32_t cell;  // x,y of the cell that noted it (packed, see Pack)
};

// One base form to look up: the cell that noted it (log only), the tick it was noted at (log only), relook = noted again at the
// grid change (skipped while its own request is pending).
struct Item {
    uint32_t base;
    uint32_t cell;
    uint32_t t;
    uint32_t relook;
};
uint32_t Pack(int32_t x, int32_t y) { return (static_cast<uint32_t>(x) & 0xFFFFu) << 16 | (static_cast<uint32_t>(y) & 0xFFFFu); }
// A cell walked ahead (CellObjectPreloadRelook): the cell, its grid x, y and the base forms that walk found (each once), so the
// grid change reads no cell memory (review revbookrelook S1).
struct Ahead {
    const uint8_t* cell;
    int32_t x, y;
    int nb;
    uint32_t bases[kCellBases];
};

struct State {
    std::atomic<bool> installed{false};
    std::atomic<int> mode{0};
    std::atomic<int> relook{0};  // CellObjectPreloadRelook
    std::atomic<uint32_t> gen{0};  // +1 at each switch on: the old-engine thread forgets its notes
    StepFn step = nullptr;
    NowFn now = &TickMs;
    // old-engine thread
    uint32_t otGen = 0;
    uint32_t seenId[kSeenTab] = {};  // base form id + 1 (0 = free)
    uint64_t seenAt[kSeenTab] = {};
    // old-engine thread -> game thread
    std::mutex mu;
    Item q[kOtQueue];
    int nq = 0;
    // game thread
    Item todo[kTodo];
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
    // old-engine thread: the cells walked ahead since the last grid change (CellObjectPreloadRelook)
    Ahead ahead[kAhead];
    int nAhead = 0;
    std::atomic<uint64_t> relCells{0}, relQueued{0}, relDrop{0}, relPendSkip{0}, relLooks{0}, relReqs{0}, relResident{0}, relUsSum{0},
        relUsMax{0}, relShifts{0};
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
bool NoteAgain(uint32_t id, uint64_t now, int64_t* ageMs)
{
    *ageMs = -1;
    uint32_t h = (id * 2654435761u) >> 19;
    for (int i = 0; i < 32; ++i, h = (h + 1) & (kSeenTab - 1)) {
        if (g.seenId[h] == id + 1) {
            *ageMs = static_cast<int64_t>(now - g.seenAt[h]);
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

// Is this cell one of the grid's cells? (SEH: the grid buffer is the game's; no C++ objects here)
bool InGrid(uint8_t* const* grid, uint32_t n, const uint8_t* cell)
{
    __try {
        for (uint32_t i = 0; i < n; ++i)
            if (grid[i] == cell) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// Game thread: is a request for this base form's form asset still being followed?
bool FormAssetPending(uint32_t base)
{
    for (int i = 0; i < g.nPend; ++i)
        if (g.pend[i].base == base) return true;
    return false;
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

// CellObjectPreloadRelook (live): 1 = the base forms of the cells walked ahead are looked up again at the grid change.
void SetCellObjectPreloadRelook(int mode)
{
    const int m = mode ? 1 : 0;
    const int was = g.relook.exchange(m);
    if (m && !was && g.installed.load(std::memory_order_relaxed) && !g.mode.load(std::memory_order_relaxed))
        Log("[cellpreload] CellObjectPreloadRelook 1 has no effect while CellObjectPreload is 0 (the re-look needs it)");
    if (was != m && g.installed.load())
        Log("[cellpreload] re-look at the grid change %s (CellObjectPreloadRelook %d)",
            m ? "on: the base forms of the cells walked ahead are looked up again when the grid changes" : "off", m);
}

// streaming/row_preload.cpp, old-engine thread: a cell loaded ahead (or found buffered) for the next grid shift. x, y: its
// grid coordinates (the log, and the re-look's list of cells).
void CellObjectPreloadCell(const uint8_t* cell, int32_t x, int32_t y)
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
    if (g.relook.load(std::memory_order_relaxed)) {  // remember it and its base forms for the grid change (a cell once, the oldest dropped)
        int k = 0;
        while (k < g.nAhead && g.ahead[k].cell != cell) ++k;
        if (k == g.nAhead) {
            if (g.nAhead == kAhead) {
                memmove(g.ahead, g.ahead + 1, sizeof(Ahead) * (kAhead - 1));
                --g.nAhead;
                k = g.nAhead;
            }
            ++g.nAhead;
        }
        Ahead& ah = g.ahead[k];
        ah.cell = cell;
        ah.x = x;
        ah.y = y;
        ah.nb = n;
        memcpy(ah.bases, bases, sizeof(uint32_t) * static_cast<size_t>(n));
    }
    g.cells.fetch_add(1, std::memory_order_relaxed);
    g.refs.fetch_add(refs, std::memory_order_relaxed);
    g.statSkip.fetch_add(stat, std::memory_order_relaxed);
    const uint64_t now = g.now();
    const uint32_t tag = Pack(x, y);
    uint32_t fresh[kCellBases];
    int nf = 0;
    for (int i = 0; i < n; ++i) {
        int64_t age = -1;
        const bool again = NoteAgain(bases[i], now, &age);
        if (again) fresh[nf++] = bases[i];
    }
    if (!nf) return;
    std::lock_guard<std::mutex> lock(g.mu);
    for (int i = 0; i < nf; ++i) {
        if (g.nq >= kOtQueue) {
            g.otDrop.fetch_add(static_cast<uint64_t>(nf - i), std::memory_order_relaxed);
            break;
        }
        g.q[g.nq++] = {fresh[i], tag, static_cast<uint32_t>(now), 0};
        g.queued.fetch_add(1, std::memory_order_relaxed);
    }
}

// streaming/row_preload.cpp, old-engine thread, after every grid change that shifted the grid (the cells it attached are in the
// grid buffer now): CellObjectPreloadRelook. For every cell walked ahead since the last grid change that is one of the grid's n
// cells, the base forms its walk ahead found are queued for a look-up again, past the 20 s wait; the game thread skips one only
// while its own form asset request is still followed. At most kRelookBases per grid change (each base form once, a hash set).
// This runs inside the crossing's old-engine frame, which the game thread waits for, so it reads no cell memory: only the grid
// buffer's n pointers (compare) and the stored ids; the walk itself stays in the calm frame ahead (review revbookrelook S1).
void CellObjectPreloadShift(uint8_t* const* grid, uint32_t n)
{
    const int na = g.nAhead;
    g.nAhead = 0;
    if (!na || !grid || !g.installed.load(std::memory_order_relaxed) || !g.mode.load(std::memory_order_relaxed) ||
        !g.relook.load(std::memory_order_relaxed))
        return;
    LARGE_INTEGER a, b, f;
    QueryPerformanceCounter(&a);
    static Item items[kRelookBases];      // old-engine thread only
    static uint32_t seen[kRelookBases * 2];  // ids + 1 of the items (open addressing; at most half full)
    memset(seen, 0, sizeof seen);
    int ni = 0, cells = 0;
    uint64_t dropped = 0;
    const uint32_t tick = static_cast<uint32_t>(g.now());
    for (int c = 0; c < na; ++c) {
        const Ahead& ah = g.ahead[c];
        if (!InGrid(grid, n, ah.cell)) continue;
        ++cells;
        for (int i = 0; i < ah.nb; ++i) {
            const uint32_t id = ah.bases[i];
            uint32_t h = (id * 2654435761u) >> 23;  // 9 bits: 0..511
            while (seen[h] && seen[h] != id + 1) h = (h + 1) & (kRelookBases * 2 - 1);
            if (seen[h]) continue;  // queued already at this grid change
            if (ni >= kRelookBases) {
                ++dropped;
                continue;
            }
            seen[h] = id + 1;
            items[ni++] = {id, Pack(ah.x, ah.y), tick, 1};
        }
    }
    int queuedNow = 0;
    if (ni) {
        std::lock_guard<std::mutex> lock(g.mu);
        for (int i = 0; i < ni; ++i) {
            if (g.nq >= kOtQueue) {
                dropped += static_cast<uint64_t>(ni - i);
                break;
            }
            g.q[g.nq++] = items[i];
            ++queuedNow;
        }
    }
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    const uint64_t us = static_cast<uint64_t>((b.QuadPart - a.QuadPart) * 1000000 / (f.QuadPart ? f.QuadPart : 1));
    g.relShifts.fetch_add(1, std::memory_order_relaxed);
    g.relCells.fetch_add(static_cast<uint64_t>(cells), std::memory_order_relaxed);
    g.relQueued.fetch_add(static_cast<uint64_t>(queuedNow), std::memory_order_relaxed);
    g.relDrop.fetch_add(dropped, std::memory_order_relaxed);
    g.relUsSum.fetch_add(us, std::memory_order_relaxed);
    MaxTo(g.relUsMax, us);
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
        const Item it = g.todo[g.todoHead];
        const uint32_t base = it.base;
        g.todoHead = (g.todoHead + 1) % kTodo;
        --g.todoN;
        if (it.relook && FormAssetPending(base)) {  // its form asset's request is still in flight: not asked for twice
            g.relPendSkip.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        ++looks;
        uint32_t req = 0;
        const int st = Look(base, 3, &req);
        reqs += (req & 1) + ((req >> 1) & 1);
        if (it.relook) {
            g.relLooks.fetch_add(1, std::memory_order_relaxed);
            g.relReqs.fetch_add((req & 1) + ((req >> 1) & 1), std::memory_order_relaxed);
            if (st == 3) g.relResident.fetch_add(1, std::memory_order_relaxed);
        }
        if (st < 0) g.notReady.fetch_add(1, std::memory_order_relaxed);
        else if (st == 0) g.nothing.fetch_add(1, std::memory_order_relaxed);
        else if (st == 3) g.resident.fetch_add(1, std::memory_order_relaxed);
        else if (st == 1) g.pend[g.nPend++] = {base, now, g.frame + kLookFrames, it.cell};
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
    const uint64_t sum = cells + looks + fr + cr + g.cleared.load() + g.relShifts.load();
    if (sum == g.lastSum) return;
    g.lastSum = sum;
    const uint64_t frames = g.frames.load(), workUs = g.workUs.load();
    Log("[cellpreload] cells %llu (references %llu, static %llu), base forms noted %llu (dropped %llu), looked up %llu: loaded %llu, nothing to "
        "load %llu, not ready %llu; requests form asset %llu, class %llu; gave up %llu; followed at most %llu; frames with work "
        "%llu, %.3f ms mean, %.3f ms max; cleared %llu, walk faults %llu; re-look at grid changes %llu: cells %llu, base forms "
        "%llu (dropped %llu, skipped while pending %llu), looked up %llu (loaded %llu), requests %llu (all three in the totals above too), "
        "%.3f ms mean, %.3f ms max",
        static_cast<unsigned long long>(cells), static_cast<unsigned long long>(g.refs.load()),
        static_cast<unsigned long long>(g.statSkip.load()), static_cast<unsigned long long>(g.queued.load()), static_cast<unsigned long long>(g.otDrop.load()),
        static_cast<unsigned long long>(looks), static_cast<unsigned long long>(g.resident.load()),
        static_cast<unsigned long long>(g.nothing.load()), static_cast<unsigned long long>(g.notReady.load()),
        static_cast<unsigned long long>(fr), static_cast<unsigned long long>(cr), static_cast<unsigned long long>(g.gaveUp.load()),
        static_cast<unsigned long long>(g.pendMax.load()), static_cast<unsigned long long>(frames),
        frames ? static_cast<double>(workUs) / 1000.0 / static_cast<double>(frames) : 0.0, g.workMaxUs.load() / 1000.0,
        static_cast<unsigned long long>(g.cleared.load()), static_cast<unsigned long long>(g.walkFaults.load()),
        static_cast<unsigned long long>(g.relShifts.load()), static_cast<unsigned long long>(g.relCells.load()),
        static_cast<unsigned long long>(g.relQueued.load()), static_cast<unsigned long long>(g.relDrop.load()),
        static_cast<unsigned long long>(g.relPendSkip.load()), static_cast<unsigned long long>(g.relLooks.load()),
        static_cast<unsigned long long>(g.relResident.load()), static_cast<unsigned long long>(g.relReqs.load()),
        g.relShifts.load() ? static_cast<double>(g.relUsSum.load()) / 1000.0 / static_cast<double>(g.relShifts.load()) : 0.0,
        g.relUsMax.load() / 1000.0);
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
    g.nAhead = 0;
    g.relook.store(0);
    g.frame = 0;
    memset(g.seenId, 0, sizeof g.seenId);
    for (auto* c : {&g.cells, &g.refs, &g.statSkip, &g.queued, &g.otDrop, &g.walkFaults, &g.looks, &g.resident, &g.nothing, &g.notReady, &g.formReqs,
                    &g.classReqs, &g.gaveUp, &g.cleared, &g.frames, &g.workUs, &g.workMaxUs, &g.pendMax, &g.relCells, &g.relQueued,
                    &g.relDrop, &g.relPendSkip, &g.relLooks, &g.relReqs, &g.relResident, &g.relUsSum, &g.relUsMax, &g.relShifts})
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
    c.relookCells = static_cast<int>(g.relCells.load());
    c.relookQueued = static_cast<int>(g.relQueued.load());
    c.relookDropped = static_cast<int>(g.relDrop.load());
    c.relookSkipped = static_cast<int>(g.relPendSkip.load());
    c.relookLooks = static_cast<int>(g.relLooks.load());
    c.relookReqs = static_cast<int>(g.relReqs.load());
    c.relookUsMax = static_cast<int>(g.relUsMax.load());
    c.ahead = g.nAhead;
    c.cells = static_cast<int>(g.cells.load());
    return c;
}

}  // namespace opf
