// RowPreload (worker rowpreload 2026-11-12; [Fixes] RowPreload, default 0 until a release, the lab ini switches it on).
// Installed at start-up when RowPreload != 0 (needs CellBufferPacedEvict: it runs from its grid-change hook and parks the cells in
// its spare slots; and NpcCreationPacing's frame hook: the game thread tells it when a menu or loading screen is up), then live 0/1.
//
// What it restores (findings "cellxing", "rowpreload"; the GAME's behaviour, not ours): the 2006 game loaded the next row of
// exterior cells ahead. Its grid update (2006 exe 0x444FB0) asked, once the player was past the edge of the centre cell, for
// every cell of the row or column the next grid shift would bring in (0x4BE090), and each request became an
// ExteriorCellLoaderTask on a background thread that created the cell, loaded its record and temporary references and queued
// their 3D; at the shift the finished cells went into the exterior cell buffer (0x4BDDF0 -> 0x442740), so the shift only had
// to attach them. The remaster still makes the request (its grid change exe+0x66B2000 calls exe+0x6926CA0 for the same cells
// under the same rule, bBackgroundCellLoads=1), but the request only moves an already buffered cell to the front of the
// buffer and looks up a task that is never created. So the whole load of the new row lands in the crossing's old-engine
// frame (temporary-data load 3.1 of 7.1 ms on the bench, more in play), and the game thread waits for it.
//
// What it does (stage 1: the cells' data):
//  1. The grid change's three request calls are redirected to a stub that calls the game's request unchanged and then notes
//     (worldspace, x, y): the game's own list of the cells the next shift will need (5 per side, 11 at a corner).
//  2. In a later old-engine (OT) frame that changes nothing else (after the per-frame grid-change call at the same point as
//     CellBufferPacedEvict's paced unload, not in a frame that shifted the grid or unloaded a cell, not within 8 OT frames
//     after a shift, not while a menu or loading screen is up, not in an interior, not while the grid change's own load flags
//     are set), ONE noted cell is taken: dropped if it is no longer just outside the loaded grid of the same worldspace
//     (the player turned or jumped), skipped if it is loaded and in the buffer already; otherwise the game's cell load
//     exe+0x67227D0 (the one the shift's preparation exe+0x66AD780 calls for the same worldspace, x and y) loads it and the
//     game's exterior buffer insert exe+0x66A9AB0 parks it in the buffer, exactly the pair the game's own load-every-cell
//     loop (exe+0x66B12BA) runs. A cell is parked only while CellBufferPacedEvict's spare slots have room and the buffer's
//     last slot is empty (the insert rotates every slot and unloads whatever falls off the end, gaps included; rev94c
//     finding 2), so whatever the insert pushes past the stock buffer size is unloaded by the paced unload (one cell per OT
//     frame) and never inside the preload's frame.
//  3. At the shift the preparation finds the cell loaded (exe+0x67227D0 returns at once for a loaded cell) and only attaches
//     it, as the 2006 game did. Any grid change clears the notes (the next row is asked for again).
// Equivalence: the same cells (the game's own requests, the same worldspace and coordinates), loaded by the same function on
// the same thread at the same point of an OT frame, end up in the state that load-every-cell loop leaves them in (data loaded,
// not attached, no 3D root yet: state 0, where a detached buffered cell is state 3 with a node; every buffer reader treats 0
// like the detached states, rev94c finding 1; five readers of the buffer were not audited: exe+0x66B09D6, 0x66AF7D4, 0x66AF850,
// 0x6D75A50, 0x66AAC40, and 0x66AD960 beyond its switch, rev94c item 21) in the same buffer the game keeps; they are attached by the shift exactly as
// a buffered cell is. A cell asked for and
// never entered stays in the buffer and is unloaded by the game in LRU order like any cell left behind (the 2006 game also
// buffered unused preloads). Only the buffer's LRU order and the moment the new row's data is read differ. Nothing is drawn
// earlier: attaching (what makes a cell's references live) stays in the shift.
// Patterns (any mismatch: not installed, one log line): the request stub (its lookup, interior test, exterior-buffer test and
// the TES pointer it reads), the shift preparation's call of the cell load, the game's load-then-insert loop (load = the
// preparation's, insert = CellBufferPacedEvict's), exactly three request calls in the grid change, the grid change's no-shift
// branch (load flags, bBackgroundCellLoads, its loading test).
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// The request stub exe+0x6926CA0 (manager, worldspace, x, y): test rdx,rdx; je; ...; call lookup(ws, x, y); ...;
// mov rdi,[TES]; mov rcx,rax; call IsInterior; ...; call InInteriorBuffer; jmp; call InExteriorBuffer; ...; mov rcx,[TES]; call touch
constexpr char kStub[] =
    "48 85 D2 0F 84 ?? ?? ?? ?? 48 89 5C 24 08 48 89 6C 24 18 56 57 41 56 48 83 EC 20 41 8B F1 41 8B E8 48 8B C2 4C 8B F1 45 8B C1 "
    "8B D5 48 8B C8 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 48 8B 3D ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B D3 48 8B CF 84 C0 74 07 "
    "E8 ?? ?? ?? ?? EB 05 E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B D3 48 8B 0D ?? ?? ?? ?? E8";
constexpr size_t kStubLookup = 0x2F, kStubTes = 0x3C, kStubInterior = 0x46, kStubExtBuf = 0x5C, kStubTes2 = 0x68;
// The shift's preparation exe+0x66AD780: mov rcx,[rcx+0B0h] (the worldspace); ...; call load(ws, x, y)
constexpr char kPrep[] =
    "48 8B 89 B0 00 00 00 41 8B D9 45 8B F0 8B EA 48 85 C9 0F 84 ?? ?? ?? ?? 45 8B C7 48 89 BC 24 ?? ?? ?? ?? 8B D3 E8";
constexpr size_t kPrepLoad = 0x25;
// The game's load-every-cell loop exe+0x66B12BA: mov rcx,[r15+0B0h]; call load; mov r14,rax; test rax,rax; je; mov rdx,rax;
// mov rcx,r15; call insert
constexpr char kLoadInsert[] = "49 8B 8F B0 00 00 00 E8 ?? ?? ?? ?? 4C 8B F0 48 85 C0 74 0B 48 8B D0 49 8B CF E8";
constexpr size_t kLiLoad = 0x07, kLiInsert = 0x1A;
// The grid change's no-shift branch (as cell_buffer.cpp): mov rcx,[flags]; mov eax,[rcx+30h]; shr eax,0Bh; test al,1; jne;
// cmp byte [bBackgroundCellLoads],0; je; call loading test; test al,al; jne; cmp qword [rdi+50h],0
constexpr char kGridFlags[] =
    "48 8B 0D ?? ?? ?? ?? 8B 41 30 C1 E8 0B A8 01 0F 85 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "84 C0 0F 85 ?? ?? ?? ?? 48 83 7F 50 00";
constexpr size_t kGfBackground = 0x15, kGfLoading = 0x22;
constexpr size_t kGridSpan = 0x400;

constexpr uint32_t kTesGridX = 0x38, kTesGridY = 0x3C, kTesInterior = 0x50, kTesWorld = 0xB0;
constexpr uint32_t kCellState = 0x7B;
constexpr uint32_t kFlagsOff = 0x30, kFlagsBusy = 0x801;  // bit 0 and bit 11 (the grid change skips its requests for bit 11)
constexpr uint32_t kSettleFrames = 8;   // OT frames after a grid shift without a preload (the shift's queued 3D completes then)
constexpr uint32_t kMaxReq = 16;        // 11 at a corner (5 + 5 + 1 for uGridsToLoad 5)
constexpr uint32_t kMaxPre = 32;

using StubFn = void (*)(uint64_t, uint64_t, int32_t, int32_t);
using LookupFn = uint8_t* (*)(uint64_t, int32_t, int32_t);
using TestFn = uint8_t (*)(uint64_t, uint64_t);   // returns al
using CellFn = uint8_t (*)(const uint8_t*);       // IsInterior(cell)
using LoadFn = uint8_t* (*)(uint64_t, int32_t, int32_t);
using InsertFn = void (*)(uint64_t, uint64_t);
using FlagFn = uint8_t (*)(uint64_t);              // the loading test (flags object)

struct Sites {
    uint8_t* stub = nullptr;
    uint8_t* calls[3]{};
    LookupFn lookup = nullptr;
    CellFn isInterior = nullptr;
    TestFn inExtBuffer = nullptr;
    LoadFn load = nullptr;
    InsertFn insert = nullptr;
    FlagFn loadingTest = nullptr;
    const uint8_t* background = nullptr;  // bBackgroundCellLoads
    uint64_t* tesSlot = nullptr;
    uint64_t* flagsSlot = nullptr;
    const uint32_t* grids = nullptr;      // uGridsToLoad
};

struct Req {
    uint64_t world;
    int32_t x, y;
};

struct State {
    bool found = false, installed = false;
    Sites s{};
    StubFn stubOrig = nullptr;
    std::atomic<int> mode{0};
    std::atomic<int> gt{0};  // game thread's last frame: 1 calm (no menu / loading freeze), 2 frozen, 0 unknown
    // OT-only
    Req req[kMaxReq]{};
    uint32_t nReq = 0;
    uint8_t* pre[kMaxPre]{};  // cells parked since the last shift (compared as pointers only)
    uint32_t nPre = 0;
    uint32_t sinceShift = 0xFFFF;
    bool testFakes = false;
} g;

std::atomic<uint64_t> c_requests{0}, c_dupes{0}, c_full{0}, c_loaded{0}, c_already{0}, c_stale{0}, c_noCell{0}, c_attached{0},
    c_shifts{0}, c_hits{0}, c_unused{0}, c_skipBusy{0}, c_skipSettle{0}, c_skipFrozen{0}, c_skipTail{0}, c_skipUnsafe{0},
    c_us{0}, c_maxUs{0}, c_over1{0}, c_over2{0}, c_over4{0}, c_cleared{0};
uint64_t g_logged = ~0ull;

uint64_t Us()
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return QpcToMicroseconds(static_cast<uint64_t>(t.QuadPart), static_cast<uint64_t>(f.QuadPart));
}

void Max(std::atomic<uint64_t>& m, uint64_t v)
{
    if (v > m.load(std::memory_order_relaxed)) m.store(v, std::memory_order_relaxed);
}

uint8_t* CallTarget(const uint8_t* at) { return RipTarget(at, 1, 5); }

void Note(uint64_t world, int32_t x, int32_t y)
{
    c_requests.fetch_add(1, std::memory_order_relaxed);
    for (uint32_t i = 0; i < g.nReq; ++i)
        if (g.req[i].world == world && g.req[i].x == x && g.req[i].y == y) {
            c_dupes.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    if (g.nReq == kMaxReq) {  // never seen (11 at most per side change); drop the oldest
        memmove(g.req, g.req + 1, sizeof(Req) * (kMaxReq - 1));
        --g.nReq;
        c_full.fetch_add(1, std::memory_order_relaxed);
    }
    g.req[g.nReq++] = {world, x, y};
}

// The grid change's request calls land here (OT, inside the grid change's no-shift branch).
void RequestHook(uint64_t manager, uint64_t world, int32_t x, int32_t y)
{
    g.stubOrig(manager, world, x, y);
    if (world && g.mode.load(std::memory_order_relaxed)) Note(world, x, y);
}

bool Safe(const uint8_t* tes)
{
    __try {
        if (*reinterpret_cast<const uint64_t*>(tes + kTesInterior)) return false;
        const uint64_t flags = *g.s.flagsSlot;
        if (!flags || (*reinterpret_cast<const uint32_t*>(flags + kFlagsOff) & kFlagsBusy)) return false;
        if (!*g.s.background) return false;
        if (g.s.loadingTest(flags)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ClearAll(bool count)
{
    if (count && g.nReq) c_cleared.fetch_add(g.nReq, std::memory_order_relaxed);
    g.nReq = 0;
    g.nPre = 0;
}

int32_t Abs(int32_t v) { return v < 0 ? -v : v; }

// One noted cell, if one is due. Returns after at most one load.
void Step(uint8_t* tes)
{
    const uint64_t world = *reinterpret_cast<const uint64_t*>(tes + kTesWorld);
    const int32_t cx = *reinterpret_cast<const int32_t*>(tes + kTesGridX), cy = *reinterpret_cast<const int32_t*>(tes + kTesGridY);
    const int32_t ring = static_cast<int32_t>(*g.s.grids >> 1) + 1;
    while (g.nReq) {
        const Req r = g.req[0];
        memmove(g.req, g.req + 1, sizeof(Req) * (g.nReq - 1));
        --g.nReq;
        const int32_t dx = Abs(r.x - cx), dy = Abs(r.y - cy);
        if (!world || r.world != world || cx == 0x7FFFFFFF || cy == 0x7FFFFFFF || (dx > dy ? dx : dy) != ring) {
            c_stale.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        uint8_t* cell = g.s.lookup(world, r.x, r.y);
        if (cell) {
            const uint8_t st = cell[kCellState];
            if (st == 5 || st == 6) {  // attaching / attached: not ours to touch
                c_attached.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (!g.s.isInterior(cell) && g.s.inExtBuffer(*g.s.tesSlot, reinterpret_cast<uint64_t>(cell))) {
                c_already.fetch_add(1, std::memory_order_relaxed);
                if (!g.testFakes) CreateAheadCell(cell);  // CreateAhead: its character references (no-op unless on)
                if (!g.testFakes) CellObjectPreloadCell(cell);  // CellObjectPreload: its objects' files (no-op unless on)
                continue;
            }
        }
        const uint64_t t0 = Us();
        cell = g.s.load(world, r.x, r.y);
        if (cell) {
            const uint8_t st = cell[kCellState];
            if (st == 5 || st == 6) {
                c_attached.fetch_add(1, std::memory_order_relaxed);
                cell = nullptr;
            } else if (!g.s.inExtBuffer(*g.s.tesSlot, reinterpret_cast<uint64_t>(cell))) {
                g.s.insert(reinterpret_cast<uint64_t>(tes), reinterpret_cast<uint64_t>(cell));
            }
        }
        const uint64_t us = Us() - t0;
        if (!cell) {
            c_noCell.fetch_add(1, std::memory_order_relaxed);
            return;  // the load ran (or looked): one per frame either way
        }
        c_loaded.fetch_add(1, std::memory_order_relaxed);
        c_us.fetch_add(us, std::memory_order_relaxed);
        Max(c_maxUs, us);
        if (us > 1000) c_over1.fetch_add(1, std::memory_order_relaxed);
        if (us > 2000) c_over2.fetch_add(1, std::memory_order_relaxed);
        if (us > 4000) c_over4.fetch_add(1, std::memory_order_relaxed);
        if (g.nPre < kMaxPre) g.pre[g.nPre++] = cell;
        if (!g.testFakes) CreateAheadCell(cell);  // CreateAhead: its character references (no-op unless on)
        if (!g.testFakes) CellObjectPreloadCell(cell);  // CellObjectPreload: its objects' files (no-op unless on)
        return;
    }
}

}  // namespace

// cell_buffer.cpp's grid hook, after every grid-change call (OT): a shift (any caller) clears the notes and counts how many
// parked cells the shift attached (pointer compare with the grid part of the buffer, no reads of the cells).
void RowPreloadGridCall(uint8_t* tes, bool shifted)
{
    if (!g.installed || !shifted || !tes) return;
    c_shifts.fetch_add(1, std::memory_order_relaxed);
    if (g.nPre) {
        uint32_t hit = 0;
        __try {
            uint8_t** const buf = *reinterpret_cast<uint8_t***>(tes + 0x60);
            const uint32_t n = *g.s.grids * *g.s.grids;
            for (uint32_t k = 0; buf && k < g.nPre; ++k)
                for (uint32_t i = 0; i < n; ++i)
                    if (buf[i] == g.pre[k]) {
                        ++hit;
                        break;
                    }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        c_hits.fetch_add(hit, std::memory_order_relaxed);
        c_unused.fetch_add(g.nPre - hit, std::memory_order_relaxed);
    }
    ClearAll(true);
    g.sinceShift = 0;
}

// cell_buffer.cpp's grid hook, after the per-frame grid-change call that runs the paced unload (OT, no shift in this frame).
// busy: this frame shifted the grid at the other call or unloaded a cell; tailFree: CellBufferPacedEvict's free spare slots.
void RowPreloadOtStep(uint8_t* tes, bool busy, uint32_t tailFree)
{
    if (!g.installed || !tes) return;
    if (g.sinceShift < 0xFFFF) ++g.sinceShift;
    if (!g.mode.load(std::memory_order_relaxed)) {
        if (g.nReq) ClearAll(true);
        return;
    }
    if (!g.nReq) return;
    if (busy) {
        c_skipBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g.sinceShift <= kSettleFrames) {
        c_skipSettle.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g.gt.load(std::memory_order_relaxed) != 1) {
        c_skipFrozen.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!tailFree) {
        c_skipTail.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!Safe(tes)) {
        c_skipUnsafe.fetch_add(1, std::memory_order_relaxed);
        ClearAll(true);  // interior, load in progress, background loads off: the notes are not for this state
        return;
    }
    Step(tes);
}

// Game thread, every frame end (NpcCreationPacing's hook): whether a menu, dialogue or loading screen holds the game frozen.
void RowPreloadGtFrame()
{
    if (!g.installed) return;
    int32_t named = 0, mode = 0;
    const bool known = ReadMenuFreeze(&named, &mode);
    g.gt.store(known ? (named == 0 && mode == 0 ? 1 : 2) : 0, std::memory_order_relaxed);
}

bool RowPreloadDiscover(const GameModule& game, RowPreloadSites* out, const char** missing)
{
    *missing = nullptr;
    auto one = [&](const char* pat, const char* what) -> uint8_t* {
        const auto hits = FindAll(game.text, ParsePattern(pat));
        if (hits.size() != 1) {
            *missing = what;
            return nullptr;
        }
        return hits[0];
    };
    uint8_t* const stub = one(kStub, "the cell request stub");
    if (!stub) return false;
    uint8_t* const prep = one(kPrep, "the shift preparation's cell load");
    if (!prep) return false;
    uint8_t* const li = one(kLoadInsert, "the game's load-then-insert loop");
    if (!li) return false;
    uint8_t* const load = CallTarget(prep + kPrepLoad);
    if (CallTarget(li + kLiLoad) != load) {
        *missing = "the same cell load in the preparation and the loop";
        return false;
    }
    uint8_t* const tes1 = RipTarget(stub + kStubTes, 3, 7);
    if (RipTarget(stub + kStubTes2, 3, 7) != tes1 || !game.data.Contains(tes1)) {
        *missing = "the request stub's TES pointer";
        return false;
    }
    // the grid change: the function that calls the stub; exactly three calls in its first kGridSpan bytes
    CellBufferInfoOut cb{};
    if (!CellBufferTestInfo(&cb) || !cb.grid) {
        *missing = "CellBufferPacedEvict's sites";
        return false;
    }
    if (CallTarget(li + kLiInsert) != cb.insert || RipTarget(stub + kStubTes, 3, 7) != cb.tesSlot) {
        *missing = "the same buffer insert and TES pointer as CellBufferPacedEvict";
        return false;
    }
    uint8_t* calls[3]{};
    uint32_t n = 0;
    uint8_t* flags = nullptr;
    for (uint8_t* p = cb.grid; p < cb.grid + kGridSpan; ++p) {
        if (*p == 0xE8 && CallTarget(p) == stub) {
            if (n < 3) calls[n] = p;
            ++n;
        }
        if (!flags && MatchesAt(p, ParsePattern(kGridFlags))) flags = p;
    }
    if (n != 3) {
        *missing = "three request calls in the grid change";
        return false;
    }
    if (!flags || RipTarget(flags, 3, 7) != cb.flagsSlot) {
        *missing = "the grid change's no-shift branch";
        return false;
    }
    out->stub = stub;
    for (int i = 0; i < 3; ++i) out->calls[i] = calls[i];
    out->lookup = CallTarget(stub + kStubLookup);
    out->isInterior = CallTarget(stub + kStubInterior);
    out->inExtBuffer = CallTarget(stub + kStubExtBuf);
    out->load = load;
    out->insert = cb.insert;
    out->loadingTest = CallTarget(flags + kGfLoading);
    out->background = RipTarget(flags + kGfBackground, 2, 7);
    out->tesSlot = cb.tesSlot;
    out->flagsSlot = cb.flagsSlot;
    out->grids = cb.grids;
    return true;
}

bool InstallRowPreload(bool beforeGameCode)
{
    if (g.installed) return true;
    if (!beforeGameCode) {
        Log("[rowpreload] RowPreload: only installed while the game starts, not installed");
        return false;
    }
    RowPreloadSites rs{};
    const char* missing = nullptr;
    if (!RowPreloadDiscover(Game(), &rs, &missing)) {
        Log("[rowpreload] RowPreload: %s not recognised, not installed", missing ? missing : "?");
        return false;
    }
    const GameModule& game = Game();
    if (!game.data.Contains(rs.background) || !game.data.Contains(rs.grids)) {
        Log("[rowpreload] RowPreload: settings outside the game's data, not installed");
        return false;
    }
    uint8_t* const mem = AllocNearGame(4096);
    if (!mem) {
        Log("[rowpreload] RowPreload: no free memory near the game's code, not installed");
        return false;
    }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&RequestHook);
    memcpy(mem, jmpAbs, 6);
    memcpy(mem + 6, &hook, 8);
    DWORD old;
    VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    g.s.stub = rs.stub;
    g.s.lookup = reinterpret_cast<LookupFn>(rs.lookup);
    g.s.isInterior = reinterpret_cast<CellFn>(rs.isInterior);
    g.s.inExtBuffer = reinterpret_cast<TestFn>(rs.inExtBuffer);
    g.s.load = reinterpret_cast<LoadFn>(rs.load);
    g.s.insert = reinterpret_cast<InsertFn>(rs.insert);
    g.s.loadingTest = reinterpret_cast<FlagFn>(rs.loadingTest);
    g.s.background = rs.background;
    g.s.tesSlot = reinterpret_cast<uint64_t*>(rs.tesSlot);
    g.s.flagsSlot = reinterpret_cast<uint64_t*>(rs.flagsSlot);
    g.s.grids = reinterpret_cast<const uint32_t*>(rs.grids);
    g.stubOrig = reinterpret_cast<StubFn>(rs.stub);
    for (int i = 0; i < 3; ++i) {
        const int64_t rel = mem - (rs.calls[i] + 5);
        if (rel != static_cast<int32_t>(rel)) {
            Log("[rowpreload] RowPreload: the stub is out of reach, not installed");
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        uint8_t code[5] = {0xE8};
        const int32_t r32 = static_cast<int32_t>(mem - (rs.calls[i] + 5));
        memcpy(code + 1, &r32, 4);
        if (!WriteCode(rs.calls[i], std::span<const uint8_t>(code, 5))) {
            // undo what was written (the stub forwards to the game's request, so a partial state is still stock)
            Log("[rowpreload] RowPreload: could not patch request call %d (the stub forwards: behaviour unchanged)", i);
            return false;
        }
        g.s.calls[i] = rs.calls[i];
    }
    g.installed = true;
    Log("[rowpreload] RowPreload installed (request stub exe+0x%llX, calls exe+0x%llX / 0x%llX / 0x%llX, cell load exe+0x%llX, "
        "buffer insert exe+0x%llX)",
        static_cast<unsigned long long>(rs.stub - game.base), static_cast<unsigned long long>(rs.calls[0] - game.base),
        static_cast<unsigned long long>(rs.calls[1] - game.base), static_cast<unsigned long long>(rs.calls[2] - game.base),
        static_cast<unsigned long long>(rs.load - game.base), static_cast<unsigned long long>(rs.insert - game.base));
    return true;
}

bool RowPreloadInstalled() { return g.installed; }  // CellObjectPreload's install check (fixmerge, rev-preload N1)

void SetRowPreload(int mode)
{
    const int m = mode ? 1 : 0;
    if (g.mode.exchange(m) != m && g.installed)
        Log("[rowpreload] RowPreload %d: %s", m,
            m ? "the next row's cells are loaded ahead, one per calm old-engine frame (as the 2006 game did)"
              : "the next row's cells are loaded at the crossing (the remaster's way)");
}

void LogRowPreloadStats()
{
    if (!g.installed) return;
    const uint64_t n = c_requests.load() + c_loaded.load() * 1000 + c_shifts.load() * 1000000;
    if (n == g_logged) return;
    g_logged = n;
    const uint64_t loaded = c_loaded.load();
    Log("[rowpreload] requests %llu (repeats %llu), cells loaded ahead %llu (%.1f ms, mean %.2f, max %.2f; > 1 ms %llu, > 2 ms %llu, "
        "> 4 ms %llu), already loaded %llu, no record %llu, attached %llu, stale %llu, cleared %llu; grid shifts %llu: loaded-ahead "
        "cells attached %llu, not used %llu; waits: busy %llu, after a shift %llu, menu/loading %llu, buffer full %llu, unsafe %llu",
        static_cast<unsigned long long>(c_requests.load()), static_cast<unsigned long long>(c_dupes.load()),
        static_cast<unsigned long long>(loaded), c_us.load() / 1000.0, loaded ? c_us.load() / 1000.0 / static_cast<double>(loaded) : 0.0,
        c_maxUs.load() / 1000.0, static_cast<unsigned long long>(c_over1.load()), static_cast<unsigned long long>(c_over2.load()),
        static_cast<unsigned long long>(c_over4.load()), static_cast<unsigned long long>(c_already.load()),
        static_cast<unsigned long long>(c_noCell.load()), static_cast<unsigned long long>(c_attached.load()),
        static_cast<unsigned long long>(c_stale.load()), static_cast<unsigned long long>(c_cleared.load()),
        static_cast<unsigned long long>(c_shifts.load()), static_cast<unsigned long long>(c_hits.load()),
        static_cast<unsigned long long>(c_unused.load()), static_cast<unsigned long long>(c_skipBusy.load()),
        static_cast<unsigned long long>(c_skipSettle.load()), static_cast<unsigned long long>(c_skipFrozen.load()),
        static_cast<unsigned long long>(c_skipTail.load()), static_cast<unsigned long long>(c_skipUnsafe.load()));
}

// ---- self-test ----
void RowPreloadTestSetup(const RowPreloadFakes* f)
{
    for (auto* c : {&c_requests, &c_dupes, &c_full, &c_loaded, &c_already, &c_stale, &c_noCell, &c_attached, &c_shifts, &c_hits,
                    &c_unused, &c_skipBusy, &c_skipSettle, &c_skipFrozen, &c_skipTail, &c_skipUnsafe, &c_us, &c_maxUs, &c_over1,
                    &c_over2, &c_over4, &c_cleared})
        c->store(0);
    g.nReq = g.nPre = 0;
    g.sinceShift = 0xFFFF;
    if (!f) {
        g.installed = false;
        g.testFakes = false;
        g.s = {};
        return;
    }
    g.s.lookup = reinterpret_cast<LookupFn>(f->lookup);
    g.s.isInterior = reinterpret_cast<CellFn>(f->isInterior);
    g.s.inExtBuffer = reinterpret_cast<TestFn>(f->inExtBuffer);
    g.s.load = reinterpret_cast<LoadFn>(f->load);
    g.s.insert = reinterpret_cast<InsertFn>(f->insert);
    g.s.loadingTest = reinterpret_cast<FlagFn>(f->loadingTest);
    g.s.background = f->background;
    g.s.tesSlot = f->tesSlot;
    g.s.flagsSlot = f->flagsSlot;
    g.s.grids = f->grids;
    g.stubOrig = reinterpret_cast<StubFn>(f->stub);
    g.installed = true;
    g.testFakes = true;
    g.gt.store(1);
}

void RowPreloadTestGt(int state) { g.gt.store(state); }
void RowPreloadTestRequest(uint64_t manager, uint64_t world, int32_t x, int32_t y) { RequestHook(manager, world, x, y); }

RowPreloadTestCounts RowPreloadTestRead()
{
    return {c_requests.load(), c_dupes.load(), c_loaded.load(), c_already.load(), c_stale.load(), c_noCell.load(), c_attached.load(),
            c_hits.load(), c_unused.load(), c_skipBusy.load(), c_skipSettle.load(), c_skipFrozen.load(), c_skipTail.load(),
            c_skipUnsafe.load(), c_cleared.load(), g.nReq};
}

}  // namespace opf
