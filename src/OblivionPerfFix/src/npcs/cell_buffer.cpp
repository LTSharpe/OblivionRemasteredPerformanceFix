// CellBufferPacedEvict (lab prototype 2026-10-17, off by default): the outgoing cells of a cell-row crossing are
// torn down in the OblivionThread frames after the crossing, one per frame, instead of inside the crossing frame.
//
// Why (sessions 41 cellrow/cellstall, 47): every ~4,100 units of travel the OblivionThread (OT) changes its loaded cell
// grid (0x66B2000) and the game thread waits for that OT frame in the pairing session: 17-48 ms OT frames, 13-44 ms GT
// waits. About half of the grid change is the teardown of the outgoing cells (0x662E360: references' 3D, NPC animation,
// scene graph, model release, OS frees; 6 cells at Kvatch's worst crossing). Those cells are not the row that leaves
// the grid: they are the oldest *buffered* (detached) cells. The exterior cell buffer (TES+0x60, `uExterior Cell
// Buffer` slots, 36 for uGridsToLoad 5) is an LRU: the grid's cells at the front, detached cells behind. The insert
// 0x66A9AB0 shifts the entries back until it finds the cell's old slot; when it runs off the end, the entry that fell off
// is torn down (0x662E360(DataHandler, cell); TES+0xA1 = 1). At a crossing the 5 (row) or 9 (diagonal) new cells push the
// 5-9 oldest buffered cells off the end, so their teardown runs inside the crossing frame.
//
// What the fix does:
//  1. Headroom (start-up): the TES constructor clamps `uExterior Cell Buffer` to at least g*(g+2)+1 (g = uGridsToLoad)
//     and allocates exactly that many slots; every user of the buffer reads the setting as its bound. The clamp is
//     redirected to a stub that does the same clamp, remembers the result as `base` and adds H = 2g-1 (9 for g = 5, the
//     most new cells one grid shift can bring) before the allocation: the buffer is base + H slots, as for a player who
//     set a bigger uExterior Cell Buffer in Oblivion.ini (a stock setting, clamped from below only).
//  2. Paced eviction (live, on the OT): after the per-frame grid change call in the OT's TES frame (the two call sites
//     at 0x65D3F20 / 0x65D3F86) returns without a shift, if a slot at index >= base holds a cell, the LAST such cell is
//     evicted exactly as the insert evicts: slot = null, 0x662E360(DataHandler, cell), TES+0xA1 = 1. One cell per OT
//     frame, only in the exterior (TES+0x50 null) and while the load flags the grid change itself checks are clear
//     ([0x9482E80]+0x30 bits 0 and 11). Evicting from the back keeps the game's own order (oldest first).
//     Any other caller of the grid change (door / coc / fast travel / load: 0x66AC810 etc.), and a per-frame call that
//     moved the grid by more than one cell, gets the overflow evicted right after its call returns (same frame as the
//     game).
//  3. Off (ini 0, live, once installed): the overflow is evicted right after every grid-change call returns, so the
//     crossing frame does the teardown again (the game's frame; only its position in the frame differs: after the grid
//     change instead of inside its cell-grid work). The capacity stays base + H; the tail is kept empty.
//  4. The ini file: the game writes Oblivion.ini back as it quits (INISettingCollection::WriteSetting 0x66E0340, from
//     the in-memory values). That function is detoured for this one setting only and given a copy holding `base`, so
//     the player's Oblivion.ini keeps the value it had (otherwise every start would add H again).
//
// Equivalence (fixes-explained ## 50): the game is at every moment in a state the stock game reaches with
// uExterior Cell Buffer = base + H; slots 0..base-1 hold the same cells in the same order as the stock game with `base`
// (an insert rotates the same prefix; only what falls off the prefix goes to the tail instead of being torn down), and
// the cells that stock would tear down are torn down a few OT frames later in the same order, on the same thread, at a
// point of the OT frame where the game tears cells down itself. A cell that comes back before it was trimmed is found
// in the tail (as with a bigger buffer) instead of being torn down and loaded again. Every reader of the buffer loops
// to the setting's value with null checks; no thread, lock or GT/OT hand-off changes.
//
// Found by shape (build 19115871 RVAs in comments); any mismatch: not installed, logged.
#include "common.h"

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace opf {
namespace {

// ---- shapes ----
// TES constructor 0x66A8FCA: mov ecx,[uGridsToLoad]; lea eax,[rcx+2]; imul eax,ecx; inc eax; cmp [uExteriorCellBuffer],eax;
// jae +0E; (same) mov [uExteriorCellBuffer],eax; mov eax,[uInteriorCellBuffer]; cmp eax,1
constexpr char kClamp[] =
    "8B 0D ?? ?? ?? ?? 8D 41 02 0F AF C1 FF C0 39 05 ?? ?? ?? ?? 73 0E 8D 41 02 0F AF C1 FF C0 89 05 ?? ?? ?? ?? "
    "8B 05 ?? ?? ?? ?? 83 F8 01";
constexpr size_t kClampLen = 0x24;  // up to the mov eax,[uInteriorCellBuffer]
// The setting's static initialiser 0xCB4696: mov eax,[uGridsToLoad]; lea ecx,[rax+2]; imul ecx,eax; inc ecx; mov [buf],ecx
constexpr char kDefault[] = "8B 05 ?? ?? ?? ?? 8D 48 02 0F AF C8 FF C1 89 0D ?? ?? ?? ??";
// The buffer insert 0x66A9AB0 (whole): start index (g*g when the grid part is full and the cell is not attached), the
// shift loop bounded by uExterior Cell Buffer, and the eviction of the entry that fell off:
// mov rcx,[DataHandler]; mov rdx,rbx; call teardown; mov byte [rdi+0A1h],1. Its first 5 bytes (mov [rsp+8],rbx) may be
// a jmp of the lab's CellRowDiag: the pattern starts after them (at +5; the pattern search needs a fixed first byte).
constexpr char kInsert[] =
    "57 48 83 EC 20 0F B6 42 7B 4C 8B CA 44 8B 05 ?? ?? ?? ?? 2C 05 3C 01 48 8B F9 8B 05 ?? ?? ?? ?? 49 8B "
    "D9 48 8B 49 60 41 0F 96 C2 0F AF C0 8D 50 FF 48 83 3C D1 00 74 0A 45 84 D2 75 05 41 3B C0 72 0E 33 C0 0F 1F 80 00 00 "
    "00 00 41 3B C0 73 1F 48 8B 4F 60 4C 8B 04 C1 48 89 1C C1 4D 3B C1 74 40 49 8B D8 FF C0 44 8B 05 ?? ?? ?? ?? EB DC 48 "
    "85 DB 74 2D 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 0A 48 8B CB E8 ?? ?? ?? ?? 66 90 48 8B 0D ?? ?? ?? ?? 48 8B D3 E8 ?? "
    "?? ?? ?? C6 87 A1 00 00 00 01";
constexpr size_t kInsBound1 = 0x11, kInsGrids = 0x1F, kInsBound2 = 0x6B, kInsDh = 0x90, kInsTear = 0x9A;
// The OT TES frame's two per-frame grid-change calls (0x65D3F20, 0x65D3F86):
//   cmp al,6; je; mov r8b,1; lea rdx,[rsp+30h]; mov rcx,[TES]; call grid
constexpr char kSite1[] = "3C 06 74 ?? 41 B0 01 48 8D 54 24 30 48 8B 0D ?? ?? ?? ?? E8";
constexpr size_t kSite1Tes = 0x0C, kSite1Call = 0x13;
//   mov rdx,[player]; cmp [rdx+X],r15b; jne; add rdx,64h; mov r8b,1; mov rcx,[TES]; call grid
constexpr char kSite2[] = "48 8B 15 ?? ?? ?? ?? 44 38 BA ?? ?? 00 00 0F 85 ?? ?? ?? ?? 48 83 C2 64 41 B0 01 48 8B 0D ?? ?? ?? ?? E8";
constexpr size_t kSite2Tes = 0x1B, kSite2Call = 0x22;
// The grid change 0x66B2000's prologue (5 bytes: mov [rsp+18h],r8b; CellRowDiag / HitchLedger may have put a jmp there)
// and the rest of its first instructions.
constexpr char kGridEntry[] =
    "44 88 44 24 18 53 56 57 41 56 48 83 EC 58 48 8B F9 45 0F B6 F0 48 8B 89 ?? ?? 00 00 48 8B F2 48 85 C9 74";
constexpr char kGridRest[] = "53 56 57 41 56 48 83 EC 58 48 8B F9 45 0F B6 F0 48 8B 89 ?? ?? 00 00 48 8B F2 48 85 C9 74";
// ... its no-shift branch: mov rcx,[flags object]; mov eax,[rcx+30h]; shr eax,0Bh; test al,1; jne; cmp byte
// [bBackgroundCellLoads],0; je; call; test al,al; jne; cmp qword [rdi+50h],0 (the interior cell)
constexpr char kGridFlags[] =
    "48 8B 0D ?? ?? ?? ?? 8B 41 30 C1 E8 0B A8 01 0F 85 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "84 C0 0F 85 ?? ?? ?? ?? 48 83 7F 50 00";
// INISettingCollection::WriteSetting(collection, setting) 0x66E0340 (virtual; reads the value at [setting] and the
// name at [setting+8]): push rbp; push rbx; push rdi; push r14 (6 bytes, position-independent); ...
constexpr char kWriteSetting[] =
    "40 55 53 57 41 56 48 8D AC 24 38 FF FF FF 48 81 EC C8 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 B0 00 00 00 "
    "32 DB 4C 8B F2 48 83 7A 08 00 48 8B F9 0F 84";
constexpr size_t kWriteSettingPrologue = 6;

constexpr uint32_t kTesInterior = 0x50, kTesBuffer = 0x60, kTesFlag = 0xA1, kTesGridX = 0x38, kTesGridY = 0x3C;
constexpr uint32_t kCellState = 0x7B;
constexpr uint32_t kFlagsOff = 0x30, kFlagsBusy = 0x801;  // bit 0 and bit 11 (the grid change skips its preload for both)

using GridFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);
using TearFn = uint64_t (*)(uint64_t, uint64_t);
using WriteFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);

struct State {
    bool found = false, installed = false;
    uint8_t* clamp = nullptr;
    uint32_t* grids = nullptr;   // uGridsToLoad's value
    uint32_t* bound = nullptr;   // uExterior Cell Buffer's value (the {value, name*} pair's first field)
    uint8_t* insert = nullptr;
    uint64_t* dataHandler = nullptr;
    TearFn teardown = nullptr;
    uint8_t* grid = nullptr;
    GridFn gridOrig = nullptr;
    uint8_t* site1Ret = nullptr;
    uint8_t* site2Ret = nullptr;
    uint64_t* tesSlot = nullptr;
    uint64_t* flagsSlot = nullptr;
    uint8_t* writeSetting = nullptr;
    WriteFn writeOrig = nullptr;
    // the stub's data (written by the TES constructor's clamp)
    volatile uint32_t* base = nullptr;
    volatile uint32_t* cap = nullptr;
    volatile uint8_t* done = nullptr;
    std::atomic<int> mode{0};
} s;

// OT-only counters (the OT is the only thread that runs the hooks' trims) + atomics for the monitor's log
std::atomic<uint64_t> g_trimmed{0}, g_paced{0}, g_immediate{0}, g_blocked{0}, g_crossings{0}, g_full{0}, g_trimUs{0},
    g_maxTrimUs{0}, g_maxOverflow{0}, g_returned{0}, g_iniWrites{0};
uint64_t g_logged = ~0ull;
bool g_site1Shift = false;
int g_drainLogs = 0;
struct Drain { uint32_t cells, frames, overflow; uint64_t us, maxUs; } g_drain{};

uint64_t Us()
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return QpcToMicroseconds(static_cast<uint64_t>(t.QuadPart), static_cast<uint64_t>(f.QuadPart));
}

bool Fail(const char* why)
{
    Log("[cellbuf] CellBufferPacedEvict not available: %s", why);
    return false;
}

uint8_t* CallTarget(const uint8_t* at) { return RipTarget(at, 1, 5); }

uint32_t Bound()
{
    const uint32_t b = *s.bound, c = *s.cap;
    return b < c ? b : c;
}

uint32_t Overflow(uint8_t* tes)
{
    uint8_t** const buf = *reinterpret_cast<uint8_t***>(tes + kTesBuffer);
    if (!buf) return 0;
    uint32_t n = 0;
    for (uint32_t i = *s.base; i < Bound(); ++i) n += buf[i] != nullptr;
    return n;
}

void Max(std::atomic<uint64_t>& m, uint64_t v)
{
    if (v > m.load(std::memory_order_relaxed)) m.store(v, std::memory_order_relaxed);
}

// Evict up to maxCells cells from the tail (index >= base), last one first, as the insert evicts the entry that fell
// off. A cell that is attached (state 5/6: never seen in the tail) stops it.
uint32_t Trim(uint8_t* tes, uint32_t maxCells, bool paced)
{
    uint8_t** const buf = *reinterpret_cast<uint8_t***>(tes + kTesBuffer);
    if (!buf) return 0;
    const uint32_t base = *s.base;
    uint32_t n = 0;
    for (uint32_t i = Bound(); i-- > base && n < maxCells;) {
        uint8_t* const cell = buf[i];
        if (!cell) continue;
        const uint8_t st = cell[kCellState];
        if (st == 5 || st == 6) {
            g_blocked.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        const uint64_t dh = *s.dataHandler;
        if (!dh) break;
        buf[i] = nullptr;
        const uint64_t t0 = Us();
        s.teardown(dh, reinterpret_cast<uint64_t>(cell));
        const uint64_t us = Us() - t0;
        tes[kTesFlag] = 1;
        ++n;
        g_trimmed.fetch_add(1, std::memory_order_relaxed);
        (paced ? g_paced : g_immediate).fetch_add(1, std::memory_order_relaxed);
        g_trimUs.fetch_add(us, std::memory_order_relaxed);
        Max(g_maxTrimUs, us);
        if (paced) {
            ++g_drain.cells;
            ++g_drain.frames;
            g_drain.us += us;
            if (us > g_drain.maxUs) g_drain.maxUs = us;
        }
    }
    return n;
}

bool SafeForPaced(uint8_t* tes)
{
    if (*reinterpret_cast<uint64_t*>(tes + kTesInterior)) return false;
    const uint64_t flags = *s.flagsSlot;
    if (!flags || (*reinterpret_cast<uint32_t*>(flags + kFlagsOff) & kFlagsBusy)) return false;
    return true;
}

uint64_t GridHookAt(const uint8_t* ret, uint64_t tesArg, uint64_t pos, uint64_t flag, uint64_t r9)
{
    uint8_t* const tes = reinterpret_cast<uint8_t*>(tesArg);
    if (!tes || !*s.done) return s.gridOrig(tesArg, pos, flag, r9);
    const int32_t x0 = *reinterpret_cast<int32_t*>(tes + kTesGridX), y0 = *reinterpret_cast<int32_t*>(tes + kTesGridY);
    const uint32_t before = Overflow(tes);
    const uint64_t r = s.gridOrig(tesArg, pos, flag, r9);
    const int32_t x1 = *reinterpret_cast<int32_t*>(tes + kTesGridX), y1 = *reinterpret_cast<int32_t*>(tes + kTesGridY);
    const bool shifted = static_cast<uint8_t>(r) != 0;
    const bool perFrame = ret == s.site1Ret || ret == s.site2Ret;
    const bool jump = x0 == 0x7FFFFFFF || y0 == 0x7FFFFFFF || x1 - x0 > 1 || x0 - x1 > 1 || y1 - y0 > 1 || y0 - y1 > 1;
    const int mode = s.mode.load(std::memory_order_relaxed);
    if (mode == 0 || !perFrame || (shifted && jump)) {
        if (Trim(tes, 0xFFFFFFFF, false)) g_drain = {};
        if (ret == s.site1Ret) g_site1Shift = false;
        return r;
    }
    if (shifted) {
        const uint32_t ov = Overflow(tes);
        g_crossings.fetch_add(1, std::memory_order_relaxed);
        Max(g_maxOverflow, ov);
        if (ov >= *s.cap - *s.base) g_full.fetch_add(1, std::memory_order_relaxed);
        if (ov < before) g_returned.fetch_add(before - ov, std::memory_order_relaxed);  // found again in the tail
        if (!g_drain.overflow) g_drain = {0, 0, ov, 0, 0};
        else g_drain.overflow = ov;
        if (ret == s.site1Ret) g_site1Shift = true;
        return r;
    }
    if (ret == s.site2Ret) {
        const bool skip = g_site1Shift;
        g_site1Shift = false;
        if (!skip && SafeForPaced(tes) && Trim(tes, 1, true) && !Overflow(tes) && g_drain.cells) {
            if (g_drainLogs < 2000) {
                ++g_drainLogs;
                Log("[cellbuf] tail drained: %u cell(s) over %u OT frame(s), %.2f ms (max %.2f ms per cell)", g_drain.cells,
                    g_drain.frames, g_drain.us / 1000.0, g_drain.maxUs / 1000.0);
            }
            g_drain = {};
        }
    }
    return r;
}

uint64_t GridHook(uint64_t tes, uint64_t pos, uint64_t flag, uint64_t r9)
{
    return GridHookAt(static_cast<const uint8_t*>(_ReturnAddress()), tes, pos, flag, r9);
}

// The setting is written with the value the player's ini gave (base), not with the headroom.
uint64_t WriteSettingHook(uint64_t coll, uint64_t setting, uint64_t r8, uint64_t r9)
{
    if (setting == reinterpret_cast<uint64_t>(s.bound) && s.done && *s.done) {
        uint64_t copy[2] = {*s.base, reinterpret_cast<const uint64_t*>(s.bound)[1]};
        if (g_iniWrites.fetch_add(1, std::memory_order_relaxed) == 0)
            Log("[cellbuf] the ini is written with uExterior Cell Buffer=%u (the player's value; the buffer has %u slots)", *s.base, *s.cap);
        return s.writeOrig(coll, reinterpret_cast<uint64_t>(copy), r8, r9);
    }
    return s.writeOrig(coll, setting, r8, r9);
}

// ---- the clamp stub ----
struct Emit {
    uint8_t* p;
    void B(std::initializer_list<uint8_t> b) { for (uint8_t x : b) *p++ = x; }
    // a RIP-relative instruction: prefix bytes, disp32 to target, then imm bytes (the disp counts from the end)
    bool Rip(std::initializer_list<uint8_t> pre, const void* target, std::initializer_list<uint8_t> imm = {})
    {
        B(pre);
        const int64_t d = static_cast<const uint8_t*>(target) - (p + 4 + imm.size());
        if (d != static_cast<int32_t>(d)) return false;
        const int32_t d32 = static_cast<int32_t>(d);
        memcpy(p, &d32, 4);
        p += 4;
        B(imm);
        return true;
    }
};

}  // namespace

// mem: two pages near the globals (code in the first, data in the second: base +0, cap +4, done +8).
bool BuildCellBufferClampStub(uint8_t* mem, const void* grids, const void* bound, const void* back)
{
    uint8_t* const data = mem + 4096;
    Emit e{mem};
    bool ok = e.Rip({0x8B, 0x0D}, grids);                       // mov ecx,[g]
    e.B({0x8D, 0x41, 0x02, 0x0F, 0xAF, 0xC1, 0xFF, 0xC0});     // lea eax,[rcx+2]; imul eax,ecx; inc eax
    ok &= e.Rip({0x39, 0x05}, bound);                           // cmp [buf],eax
    e.B({0x73, 0x06});                                          // jae +6
    ok &= e.Rip({0x89, 0x05}, bound);                           // mov [buf],eax
    ok &= e.Rip({0x80, 0x3D}, data + 8, {0x00});                // cmp byte [done],0
    uint8_t* const j1 = e.p;
    e.B({0x75, 0x00});                                          // jne end
    e.B({0x83, 0xF9, 0x01});                                    // cmp ecx,1
    uint8_t* const j2 = e.p;
    e.B({0x72, 0x00});                                          // jb end
    e.B({0x83, 0xF9, 0x0F});                                    // cmp ecx,15
    uint8_t* const j3 = e.p;
    e.B({0x77, 0x00});                                          // ja end
    ok &= e.Rip({0x8B, 0x05}, bound);                           // mov eax,[buf]
    ok &= e.Rip({0x89, 0x05}, data + 0);                        // mov [base],eax
    e.B({0x8D, 0x4C, 0x09, 0xFF, 0x01, 0xC8});                  // lea ecx,[rcx+rcx-1]; add eax,ecx
    ok &= e.Rip({0x89, 0x05}, bound);                           // mov [buf],eax
    ok &= e.Rip({0x89, 0x05}, data + 4);                        // mov [cap],eax
    ok &= e.Rip({0xC6, 0x05}, data + 8, {0x01});                // mov byte [done],1
    uint8_t* const end = e.p;
    e.B({0xFF, 0x25, 0, 0, 0, 0});                              // jmp [rip]
    memcpy(e.p, &back, 8);
    e.p += 8;
    for (uint8_t* j : {j1, j2, j3}) j[1] = static_cast<uint8_t>(end - (j + 2));
    return ok;
}

bool FindCellBufferPacedEvict()
{
    if (s.found) return true;
    const GameModule& game = Game();
    auto one = [&](const char* pat, const char* what) -> uint8_t* {
        const auto hits = FindAll(game.text, ParsePattern(pat));
        if (hits.size() != 1) {
            Log("[cellbuf] %s %s", what, hits.empty() ? "not found" : "ambiguous");
            return nullptr;
        }
        return hits[0];
    };
    uint8_t* const clamp = one(kClamp, "the TES constructor's cell buffer clamp");
    uint8_t* const dflt = one(kDefault, "uExterior Cell Buffer's default");
    uint8_t* const insHit = one(kInsert, "the exterior cell buffer's insert");
    uint8_t* const ins = insHit ? insHit - 5 : nullptr;
    uint8_t* const site1 = one(kSite1, "the OT frame's first grid-change call");
    uint8_t* const site2 = one(kSite2, "the OT frame's second grid-change call");
    uint8_t* const ws = one(kWriteSetting, "the ini collection's WriteSetting");
    if (!clamp || !dflt || !ins || !site1 || !site2 || !ws) return Fail("a shape was not found");
    if (ins[0] != 0xE9 && !MatchesAt(ins, ParsePattern("48 89 5C 24 08"))) return Fail("the insert's entry is not as expected");
    auto* const grids = reinterpret_cast<uint32_t*>(RipTarget(clamp, 2, 6));
    auto* const bound = reinterpret_cast<uint32_t*>(RipTarget(clamp + 0x0E, 2, 6));
    if (RipTarget(clamp + 0x1E, 2, 6) != reinterpret_cast<uint8_t*>(bound) ||
        RipTarget(dflt, 2, 6) != reinterpret_cast<uint8_t*>(grids) || RipTarget(dflt + 0x0E, 2, 6) != reinterpret_cast<uint8_t*>(bound) ||
        RipTarget(ins + kInsBound1, 3, 7) != reinterpret_cast<uint8_t*>(bound) ||
        RipTarget(ins + kInsBound2, 3, 7) != reinterpret_cast<uint8_t*>(bound) ||
        RipTarget(ins + kInsGrids, 2, 6) != reinterpret_cast<uint8_t*>(grids) || !game.data.Contains(bound) || !game.data.Contains(grids))
        return Fail("the constructor, the default and the insert do not use the same settings");
    uint8_t* const grid = CallTarget(site2 + kSite2Call);
    if (CallTarget(site1 + kSite1Call) != grid || RipTarget(site1 + kSite1Tes, 3, 7) != RipTarget(site2 + kSite2Tes, 3, 7))
        return Fail("the two OT frame calls differ");
    const bool stockEntry = MatchesAt(grid, ParsePattern(kGridEntry));
    const bool chained = grid[0] == 0xE9 && MatchesAt(grid + 5, ParsePattern(kGridRest));
    if (!stockEntry && !chained) return Fail("the grid change's entry is not as expected");
    uint8_t* flags = nullptr;
    for (uint8_t* p = grid; p < grid + 0x400 && !flags; ++p)
        if (MatchesAt(p, ParsePattern(kGridFlags))) flags = p;
    if (!flags) return Fail("the grid change's no-shift branch is not as expected");
    s.clamp = clamp;
    s.grids = grids;
    s.bound = bound;
    s.insert = ins;
    s.dataHandler = reinterpret_cast<uint64_t*>(RipTarget(ins + kInsDh, 3, 7));
    s.teardown = reinterpret_cast<TearFn>(CallTarget(ins + kInsTear));
    s.grid = grid;
    s.site1Ret = site1 + kSite1Call + 5;
    s.site2Ret = site2 + kSite2Call + 5;
    s.tesSlot = reinterpret_cast<uint64_t*>(RipTarget(site2 + kSite2Tes, 3, 7));
    s.flagsSlot = reinterpret_cast<uint64_t*>(RipTarget(flags, 3, 7));
    s.writeSetting = ws;
    s.found = true;
    Log("[cellbuf] found: clamp exe+0x%X (uGridsToLoad exe+0x%X, uExterior Cell Buffer exe+0x%X), insert exe+0x%X "
        "(teardown exe+0x%X, DataHandler exe+0x%X), grid change exe+0x%X%s (calls exe+0x%X / exe+0x%X), TES exe+0x%X, "
        "flags exe+0x%X, WriteSetting exe+0x%X",
        static_cast<unsigned>(clamp - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(grids) - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(bound) - game.base), static_cast<unsigned>(ins - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(s.teardown) - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(s.dataHandler) - game.base), static_cast<unsigned>(grid - game.base),
        chained ? " (after another detour)" : "", static_cast<unsigned>(site1 + kSite1Call - game.base),
        static_cast<unsigned>(site2 + kSite2Call - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(s.tesSlot) - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(s.flagsSlot) - game.base), static_cast<unsigned>(ws - game.base));
    return true;
}

bool InstallCellBufferPacedEvict(bool beforeGameCode)
{
    if (s.installed) return true;
    if (!beforeGameCode) return Fail("only installed while the game starts (before its cell buffer is made)");
    if (!FindCellBufferPacedEvict()) return false;
    if (*s.tesSlot) return Fail("the cell buffer already exists");
    // [0, 4096): clamp stub; [4096, 8192): its data; then the grid change's and WriteSetting's stubs and trampolines
    uint8_t* const mem = AllocNearGame(3 * 4096);
    if (!mem) return Fail("no free memory near the game's code");
    if (!BuildCellBufferClampStub(mem, s.grids, s.bound, s.clamp + kClampLen)) return Fail("the clamp stub is out of reach");
    s.base = reinterpret_cast<uint32_t*>(mem + 4096);
    s.cap = reinterpret_cast<uint32_t*>(mem + 4096 + 4);
    s.done = mem + 4096 + 8;
    uint8_t* const t = mem + 2 * 4096;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    // grid change: trampoline at t (its 5-byte prologue + jmp back, or the earlier detour's target), stub at t + 64
    if (s.grid[0] == 0xE9) {
        uint8_t* const prev = CallTarget(s.grid);
        memcpy(t, jmpAbs, 6);
        memcpy(t + 6, &prev, 8);
    } else {
        uint8_t* const resume = s.grid + 5;
        memcpy(t, s.grid, 5);
        memcpy(t + 5, jmpAbs, 6);
        memcpy(t + 11, &resume, 8);
    }
    void* const gh = reinterpret_cast<void*>(&GridHook);
    memcpy(t + 64, jmpAbs, 6);
    memcpy(t + 70, &gh, 8);
    // WriteSetting: trampoline at t + 128 (6-byte prologue + jmp back), stub at t + 192
    {
        uint8_t* const resume = s.writeSetting + kWriteSettingPrologue;
        memcpy(t + 128, s.writeSetting, kWriteSettingPrologue);
        memcpy(t + 128 + kWriteSettingPrologue, jmpAbs, 6);
        memcpy(t + 128 + kWriteSettingPrologue + 6, &resume, 8);
        void* const wh = reinterpret_cast<void*>(&WriteSettingHook);
        memcpy(t + 192, jmpAbs, 6);
        memcpy(t + 198, &wh, 8);
    }
    DWORD old;
    VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
    VirtualProtect(t, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, 3 * 4096);
    s.gridOrig = reinterpret_cast<GridFn>(t);
    s.writeOrig = reinterpret_cast<WriteFn>(t + 128);
    auto jmpTo = [](uint8_t* at, size_t n, const uint8_t* to) {
        const int64_t rel = to - (at + 5);
        if (rel != static_cast<int32_t>(rel) || n < 5 || n > 64) return false;
        uint8_t code[64];
        memset(code, 0x90, sizeof code);
        code[0] = 0xE9;
        const int32_t r32 = static_cast<int32_t>(rel);
        memcpy(code + 1, &r32, 4);
        return WriteCode(at, std::span<const uint8_t>(code, n));
    };
    // order: the ini guard and the grid hook first (both inert until the constructor ran), the clamp last
    if (!jmpTo(s.writeSetting, kWriteSettingPrologue, t + 192)) return Fail("could not patch WriteSetting");
    if (!jmpTo(s.grid, 5, t + 64)) return Fail("could not patch the grid change");
    if (!jmpTo(s.clamp, kClampLen, mem)) return Fail("could not patch the clamp");
    s.installed = true;
    Log("[cellbuf] installed: the cell buffer gets 2 x uGridsToLoad - 1 spare slots when the game makes it; grid change and "
        "WriteSetting detoured");
    return true;
}

void SetCellBufferPacedEvict(int mode)
{
    if (!s.installed) return;
    const int m = mode ? 1 : 0;
    if (s.mode.exchange(m) != m)
        Log("[cellbuf] %s", m ? "outgoing cells torn down one per OblivionThread frame after a crossing"
                              : "outgoing cells torn down in the crossing frame (as the game does)");
}

void LogCellBufferPacedEvictStats()
{
    if (!s.installed) return;
    static bool sized = false;
    if (!sized && *s.done) {
        sized = true;
        const char* name = reinterpret_cast<const char* const*>(s.bound)[1];
        char nm[40] = "?";
        __try {
            if (name) strncpy_s(nm, name, _TRUNCATE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        Log("[cellbuf] cell buffer made with %u slots (\"%s\" = %u from the ini / minimum, + %u spare; uGridsToLoad %u)",
            *s.cap, nm, *s.base, *s.cap - *s.base, *s.grids);
    } else if (!sized) {
        return;
    }
    const uint64_t n = g_trimmed.load() + g_crossings.load() * 1000 + g_iniWrites.load();
    if (n == g_logged) return;
    g_logged = n;
    Log("[cellbuf] crossings %llu (max tail %llu, tail full %llu, cells back from the tail %llu); cells torn down from the "
        "tail %llu (paced %llu, at once %llu), %.1f ms (max %.2f ms); attached tail cells left %llu; ini writes with the "
        "player's value %llu",
        static_cast<unsigned long long>(g_crossings.load()), static_cast<unsigned long long>(g_maxOverflow.load()),
        static_cast<unsigned long long>(g_full.load()), static_cast<unsigned long long>(g_returned.load()),
        static_cast<unsigned long long>(g_trimmed.load()), static_cast<unsigned long long>(g_paced.load()),
        static_cast<unsigned long long>(g_immediate.load()), g_trimUs.load() / 1000.0, g_maxTrimUs.load() / 1000.0,
        static_cast<unsigned long long>(g_blocked.load()), static_cast<unsigned long long>(g_iniWrites.load()));
}

// ---- self-test ----
bool CellBufferTestInfo(CellBufferInfoOut* out)
{
    if (!s.found) return false;
    out->clamp = s.clamp;
    out->grids = reinterpret_cast<uint8_t*>(s.grids);
    out->bound = reinterpret_cast<uint8_t*>(s.bound);
    out->insert = s.insert;
    out->teardown = reinterpret_cast<uint8_t*>(s.teardown);
    out->dataHandler = reinterpret_cast<uint8_t*>(s.dataHandler);
    out->grid = s.grid;
    out->site1Ret = s.site1Ret;
    out->site2Ret = s.site2Ret;
    out->tesSlot = reinterpret_cast<uint8_t*>(s.tesSlot);
    out->flagsSlot = reinterpret_cast<uint8_t*>(s.flagsSlot);
    out->writeSetting = s.writeSetting;
    return true;
}

// Fakes for the hook: the grid change, the teardown, the globals; site1/site2 = the return addresses to treat as the
// per-frame calls.
void CellBufferTestSetup(const CellBufferTest& t)
{
    s.gridOrig = reinterpret_cast<GridFn>(t.grid);
    s.teardown = reinterpret_cast<TearFn>(t.teardown);
    s.dataHandler = t.dataHandler;
    s.bound = t.bound;
    s.base = t.base;
    s.cap = t.cap;
    s.done = t.done;
    s.flagsSlot = t.flagsSlot;
    s.site1Ret = static_cast<uint8_t*>(t.site1Ret);
    s.site2Ret = static_cast<uint8_t*>(t.site2Ret);
    s.installed = true;
    for (auto* c : {&g_trimmed, &g_paced, &g_immediate, &g_blocked, &g_crossings, &g_full, &g_trimUs, &g_maxTrimUs,
                    &g_maxOverflow, &g_returned, &g_iniWrites})
        c->store(0);
    g_site1Shift = false;
    g_drain = {};
}

uint64_t CellBufferTestGridHook(const void* caller, uint64_t tes, uint64_t pos, uint64_t flag)
{
    return GridHookAt(static_cast<const uint8_t*>(caller), tes, pos, flag, 0);
}

CellBufferCounts CellBufferTestCounts()
{
    return {g_trimmed.load(), g_paced.load(), g_immediate.load(), g_blocked.load(), g_crossings.load(), g_maxOverflow.load()};
}

}  // namespace opf
