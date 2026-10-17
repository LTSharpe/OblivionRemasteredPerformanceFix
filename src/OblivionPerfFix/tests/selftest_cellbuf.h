// CellBufferPacedEvict (lab prototype) self-test, included by selftest.cpp: the shapes on the mapped exe (the
// TES constructor's clamp, the buffer insert and its eviction, the two per-frame grid-change calls, the no-shift
// branch's flags, the ini writer), the clamp stub run on fake settings, and the LRU: a model of the game's insert
// (0x66A9AB0) driven by a random walk over a grid of cells, once as the stock game (uExterior Cell Buffer = base) and once
// with the spare slots and the fix's hook between the walk's frames. Checked: whenever the fix's tail is empty, the
// buffer is the stock buffer slot for slot; the fix tears cells down in the stock order (a subsequence of the stock
// teardowns: the rest came back from the tail); off = the stock teardowns exactly; any other caller and a jump of the
// grid empty the tail in their own call.
#pragma once

#include <map>
#include <random>
#include <vector>

namespace cbuf {

constexpr uint32_t kG = 5, kBase = 36, kCap = 45;

struct Cell {
    uint32_t id;
    uint8_t pad[0x7B - 4];
    uint8_t state;  // +0x7B
    uint8_t pad2[4];
};
static_assert(offsetof(Cell, state) == 0x7B);

struct Tes {
    uint8_t mem[0x100];
    Cell** buf() { return *reinterpret_cast<Cell***>(mem + 0x60); }
};

struct Model {
    Tes tes{};
    std::vector<Cell*> slots;
    uint32_t bound = 0;
    std::vector<uint32_t> evicted;
    std::map<std::pair<int, int>, Cell*> world;  // each model has its own cell objects (ids by position)
    ~Model()
    {
        for (auto& kv : world) delete kv.second;
    }
    void Init(uint32_t n)
    {
        slots.assign(n, nullptr);
        Cell** p = slots.data();
        memcpy(tes.mem + 0x60, &p, 8);
        bound = n;
    }
};

Model* g_cur = nullptr;  // whose teardown the fake records
uint32_t g_bound = 0;    // the fix's view of the setting (the fix's model)
volatile uint32_t g_base = 0, g_cap = 0;
volatile uint8_t g_done = 0;
uint64_t g_dh = 0x1234;
uint8_t g_flags[0x40];
uint64_t g_flagsSlot = 0;
uint8_t g_site1[8], g_site2[8], g_other[8];

uint64_t FakeTeardown(uint64_t dh, uint64_t cell)
{
    auto* c = reinterpret_cast<Cell*>(cell);
    if (dh == g_dh && g_cur) g_cur->evicted.push_back(c->id);
    c->state = 1;
    return 0;
}

// the game's insert 0x66A9AB0 on a model (bound = the model's setting)
void Insert(Model& m, Cell* c, uint32_t bound)
{
    Cell** const b = m.tes.buf();
    const bool attached = c->state == 5 || c->state == 6;
    uint32_t i = 0;
    if (b[kG * kG - 1] && !attached && kG * kG < bound) i = kG * kG;
    Cell* carry = c;
    for (; i < bound; ++i) {
        Cell* const old = b[i];
        b[i] = carry;
        if (old == c) return;
        carry = old;
    }
    if (carry) {
        g_cur = &m;
        FakeTeardown(g_dh, reinterpret_cast<uint64_t>(carry));
        m.tes.mem[0xA1] = 1;
    }
}

// the world: cells by grid position, created on first use (one object per cell, as the game's cell forms)
Cell* CellAt(Model& m, int x, int y)
{
    Cell*& c = m.world[{x, y}];
    if (!c) {
        c = new Cell{};
        c->id = static_cast<uint32_t>((x + 2000) * 4096 + (y + 2000));
        c->state = 1;
    }
    return c;
}
uint32_t Id(const Cell* c) { return c ? c->id : 0; }

// one grid change to (x, y): cells leaving the grid detach, the grid's cells are touched in row-major order (a cell
// that is not attached is prepared (touched), then attached and touched again, as 0x66ABEC0 does)
struct Walk {
    int x = 0, y = 0;
};
void Shift(Model& m, Walk& w, int nx, int ny, uint32_t bound)
{
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy) {
            const int cx = w.x + dx, cy = w.y + dy;
            if (cx < nx - 2 || cx > nx + 2 || cy < ny - 2 || cy > ny + 2) {
                Cell* c = CellAt(m, cx, cy);
                if (c->state == 5 || c->state == 6) c->state = 4;
            }
        }
    w.x = nx;
    w.y = ny;
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy) {
            Cell* c = CellAt(m, nx + dx, ny + dy);
            if (c->state != 5 && c->state != 6) {
                c->state = 4;
                Insert(m, c, bound);
                c->state = 6;
            }
            Insert(m, c, bound);
        }
    memcpy(m.tes.mem + 0x38, &w.x, 4);
    memcpy(m.tes.mem + 0x3C, &w.y, 4);
}

// The fake grid change seen by the fix's hook: does the scheduled shift (if any) on the fix's model.
Model* g_fixModel = nullptr;
Walk* g_fixWalk = nullptr;
bool g_doShift = false;
int g_toX = 0, g_toY = 0;
uint64_t FakeGrid(uint64_t, uint64_t, uint64_t, uint64_t)
{
    if (!g_doShift) return 0;
    Shift(*g_fixModel, *g_fixWalk, g_toX, g_toY, g_bound);
    return 1;
}

bool IsSubsequence(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)  // a in b, in order
{
    size_t j = 0;
    for (uint32_t v : b)
        if (j < a.size() && a[j] == v) ++j;
    return j == a.size();
}

// One walk: n crossings, frames between them from the rng; mode 1 paced, 0 off. Returns failures counted in *bad.
struct Result { int compared, mismatched; size_t stockEvicted, fixEvicted; bool subseq, exact, tailEmptyAfterOther; uint32_t maxTail; };
Result RunWalk(int mode, uint32_t seed, int crossings, int maxGap, bool straight = false)
{
    Model stock, fix;
    stock.Init(kBase);
    fix.Init(kCap);
    Walk ws, wf;
    g_bound = kCap;
    g_base = kBase;
    g_cap = kCap;
    g_done = 1;
    memset(g_flags, 0, sizeof g_flags);
    g_flagsSlot = reinterpret_cast<uint64_t>(g_flags);
    CellBufferTest t{};
    t.grid = reinterpret_cast<void*>(&FakeGrid);
    t.teardown = reinterpret_cast<void*>(&FakeTeardown);
    t.dataHandler = &g_dh;
    t.bound = &g_bound;
    t.base = &g_base;
    t.cap = &g_cap;
    t.done = &g_done;
    t.flagsSlot = &g_flagsSlot;
    t.site1Ret = g_site1;
    t.site2Ret = g_site2;
    CellBufferTestSetup(t);
    SetCellBufferPacedEvict(mode);
    g_fixModel = &fix;
    g_fixWalk = &wf;
    std::mt19937 rng(seed);
    Result r{};
    r.tailEmptyAfterOther = true;
    // first load at (0,0): a jump (the grid was unset), by another caller
    auto fixCall = [&](const void* caller, bool shift, int nx, int ny) {
        g_doShift = shift;
        g_toX = nx;
        g_toY = ny;
        g_cur = &fix;
        CellBufferTestGridHook(caller, reinterpret_cast<uint64_t>(fix.tes.mem), 0, 1);
        g_doShift = false;
    };
    auto tail = [&]() {
        uint32_t n = 0;
        for (uint32_t i = kBase; i < kCap; ++i) n += fix.slots[i] != nullptr;
        return n;
    };
    // both start with the grid at (0, 0)
    int x = 0, y = 0;
    {
        const int init = 0x7FFFFFFF;
        memcpy(fix.tes.mem + 0x38, &init, 4);
        memcpy(fix.tes.mem + 0x3C, &init, 4);
        ws.x = wf.x = -100;
        ws.y = wf.y = -100;
        g_cur = &stock;
        Shift(stock, ws, 0, 0, kBase);
        fixCall(g_other, true, 0, 0);
    }
    for (int k = 0; k < crossings; ++k) {
        // a step: row, column or diagonal; now and then a jump of a few cells by another caller (a door / coc)
        const int kind = straight ? 1 : static_cast<int>(rng() % 40);
        int nx = x, ny = y;
        const void* caller = g_site2;
        if (kind == 0) {
            nx += static_cast<int>(rng() % 7) - 3;
            ny += 4 + static_cast<int>(rng() % 3);
            caller = g_other;
        } else {
            const int d = straight ? 4 * (k % 2) : static_cast<int>(rng() % 8);  // straight: row / diagonal steps
            static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
            nx += dx[d];
            ny += dy[d];
            if (!straight && rng() % 9 == 0) caller = g_site1;
        }
        x = nx;
        y = ny;
        g_cur = &stock;
        Shift(stock, ws, nx, ny, kBase);
        fixCall(caller, true, nx, ny);
        if (caller == g_other && tail() != 0) r.tailEmptyAfterOther = false;
        if (tail() > r.maxTail) r.maxTail = tail();
        const int gap = static_cast<int>(rng() % (maxGap + 1));
        for (int f = 0; f < gap; ++f) {
            fixCall(rng() % 7 == 0 ? static_cast<const void*>(g_site1) : static_cast<const void*>(g_site2), false, x, y);
            if (tail() == 0) {
                ++r.compared;
                bool same = true;
                for (uint32_t i = 0; i < kBase; ++i) same &= Id(fix.slots[i]) == Id(stock.slots[i]);
                if (!same) ++r.mismatched;
            }
        }
    }
    for (int f = 0; f < 64 && tail(); ++f) fixCall(g_site2, false, x, y);  // the last crossing's tail
    r.stockEvicted = stock.evicted.size();
    r.fixEvicted = fix.evicted.size();
    r.subseq = IsSubsequence(fix.evicted, stock.evicted);
    r.exact = fix.evicted == stock.evicted;
    return r;
}

static void Run(const GameModule& game)
{
    // ---- the mapped exe ----
    Check(FindCellBufferPacedEvict(), "cell buffer: clamp, insert, per-frame grid-change calls, flags and ini writer found");
    CellBufferInfoOut i{};
    if (Installed(CellBufferTestInfo(&i), "CellBufferPacedEvict (cell buffer paced eviction)")) {
        const auto rva = [&](const uint8_t* p) { return static_cast<uint32_t>(p - game.base); };
        Check(rva(i.clamp) == 0x66A8FCA && rva(i.grids) == 0x8FAED80 && rva(i.bound) == 0x8FABEC0,
              "cell buffer: TES constructor clamp at exe+0x66A8FCA on uGridsToLoad exe+0x8FAED80, uExterior Cell Buffer exe+0x8FABEC0");
        Check(rva(i.insert) == 0x66A9AB0 && rva(i.teardown) == 0x662E360 && rva(i.dataHandler) == 0x9480018,
              "cell buffer: insert exe+0x66A9AB0 evicts with teardown exe+0x662E360 on DataHandler exe+0x9480018");
        Check(rva(i.grid) == 0x66B2000 && rva(i.site1Ret) == 0x65D3F25 && rva(i.site2Ret) == 0x65D3F8B && rva(i.tesSlot) == 0x941F748,
              "cell buffer: grid change exe+0x66B2000 called by the OT frame at exe+0x65D3F20 / exe+0x65D3F86, TES exe+0x941F748");
        Check(rva(i.flagsSlot) == 0x9482E80 && rva(i.writeSetting) == 0x66E0340,
              "cell buffer: the no-shift branch's flags object exe+0x9482E80, WriteSetting exe+0x66E0340");
    }
    // ---- the clamp stub on fake settings ----
    {
        uint8_t* const mem = AllocNearGame(3 * 4096);
        if (Installed(mem != nullptr, "cell buffer: memory for the clamp stub")) {
            uint8_t* const ret = mem + 0x800;
            *ret = 0xC3;
            auto* const g = reinterpret_cast<uint32_t*>(mem + 2 * 4096);
            auto* const b = g + 1;
            const bool built = BuildCellBufferClampStub(mem, g, b, ret);
            DWORD old;
            VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
            FlushInstructionCache(GetCurrentProcess(), mem, 4096);
            auto* const base = reinterpret_cast<volatile uint32_t*>(mem + 4096);
            auto* const cap = base + 1;
            auto* const done = mem + 4096 + 8;
            auto run = [&](uint32_t gv, uint32_t bv) {
                *g = gv;
                *b = bv;
                *base = 0;
                *cap = 0;
                *done = 0;
                reinterpret_cast<void (*)()>(mem)();
            };
            Check(built, "cell buffer: clamp stub built");
            run(5, 0);
            Check(*b == 45 && *base == 36 && *cap == 45 && *done == 1, "cell buffer: g 5, ini 0 -> minimum 36 + 9 spare = 45");
            run(5, 36);
            Check(*b == 45 && *base == 36 && *cap == 45, "cell buffer: g 5, ini 36 -> 45");
            run(5, 50);
            Check(*b == 59 && *base == 50 && *cap == 59, "cell buffer: g 5, ini 50 -> 50 + 9");
            run(3, 4);
            Check(*b == 21 && *base == 16 && *cap == 21, "cell buffer: g 3, ini 4 -> minimum 16 + 5");
            run(5, 36);
            reinterpret_cast<void (*)()>(mem)();  // the constructor again: only the clamp
            Check(*b == 45 && *base == 36 && *cap == 45, "cell buffer: a second construction adds nothing");
            run(0, 0);
            Check(*b == 1 && *done == 0, "cell buffer: g 0 -> the game's clamp only, no spare slots");
            run(17, 300);
            Check(*b == 324 && *done == 0, "cell buffer: g 17 (out of range) -> the game's clamp only (324), no spare slots");
            VirtualFree(mem, 0, MEM_RELEASE);
        }
    }
    // ---- the LRU with the hook ----
    {
        const Result p = RunWalk(1, 50, 3000, 12);
        Check(p.compared > 1000 && p.mismatched == 0, "cell buffer: paced, every drained state = the stock buffer slot for slot");
        Check(p.subseq && p.fixEvicted > 1000 && p.fixEvicted <= p.stockEvicted,
              "cell buffer: paced, cells torn down in the stock order (subsequence of the stock teardowns)");
        Check(p.tailEmptyAfterOther, "cell buffer: paced, another caller's grid change empties the tail in its own call");
        Check(p.maxTail >= 9, "cell buffer: paced, a diagonal crossing leaves 9 cells for later frames");
        printf("      paced: %d drained states compared, stock teardowns %zu, fix %zu (rest came back from the tail), max tail %u\n",
               p.compared, p.stockEvicted, p.fixEvicted, p.maxTail);
        const Result q = RunWalk(1, 51, 3000, 2);  // crossings faster than the tail drains: the insert's own fall-off
        Check(q.mismatched == 0 && q.subseq, "cell buffer: paced, back-to-back crossings (tail full) keep the stock buffer and order");
        const Result o = RunWalk(0, 52, 2000, 6);
        // off: the tail is empty after every call and the buffer is the stock one; the teardowns are the stock ones in
        // the stock order except a cell pushed into the tail and touched again in the same grid change (the stock game
        // tears it down and loads it again within that call)
        Check(o.subseq && o.mismatched == 0 && o.maxTail == 0 && o.stockEvicted - o.fixEvicted < o.stockEvicted / 10,
              "cell buffer: off, the stock buffer after every call, the stock teardowns in the stock order");
        printf("      off: stock teardowns %zu, fix %zu (the rest were touched again in the same grid change)\n", o.stockEvicted,
               o.fixEvicted);
        CellBufferCounts c = CellBufferTestCounts();
        // a walk on which no cell comes back (rows and diagonals, always forward): exactly the stock teardowns
        const Result so = RunWalk(0, 53, 1500, 6, true);
        Check(so.exact && so.mismatched == 0 && so.stockEvicted > 1000, "cell buffer: off, forward walk: exactly the stock teardowns");
        const Result sp = RunWalk(1, 54, 1500, 12, true);
        Check(sp.exact && sp.mismatched == 0 && sp.compared > 500, "cell buffer: paced, forward walk: exactly the stock teardowns, in order");
        const Result sq = RunWalk(1, 55, 1500, 1, true);
        Check(sq.exact && sq.mismatched == 0, "cell buffer: paced, forward walk with back-to-back crossings: exactly the stock teardowns");
        Check(c.paced == 0 && c.trimmed > 0 && c.trimmed <= o.fixEvicted, "cell buffer: off, every overflow torn down in its own call");
    }
    // ---- an attached cell in the tail stops the trim ----
    {
        Model m;
        m.Init(kCap);
        g_bound = kCap;
        g_base = kBase;
        g_cap = kCap;
        g_done = 1;
        g_flagsSlot = reinterpret_cast<uint64_t>(g_flags);
        CellBufferTest t{};
        t.grid = reinterpret_cast<void*>(&FakeGrid);
        t.teardown = reinterpret_cast<void*>(&FakeTeardown);
        t.dataHandler = &g_dh;
        t.bound = &g_bound;
        t.base = &g_base;
        t.cap = &g_cap;
        t.done = &g_done;
        t.flagsSlot = &g_flagsSlot;
        t.site1Ret = g_site1;
        t.site2Ret = g_site2;
        CellBufferTestSetup(t);
        SetCellBufferPacedEvict(1);
        Cell a{}, b2{};
        a.id = 1;
        a.state = 4;
        b2.id = 2;
        b2.state = 6;
        m.slots[kBase] = &a;
        m.slots[kBase + 1] = &b2;
        g_doShift = false;
        g_cur = &m;
        const int gx = 0;
        memcpy(m.tes.mem + 0x38, &gx, 4);
        memcpy(m.tes.mem + 0x3C, &gx, 4);
        CellBufferTestGridHook(g_site2, reinterpret_cast<uint64_t>(m.tes.mem), 0, 1);
        Check(m.evicted.empty() && m.slots[kBase + 1] == &b2 && CellBufferTestCounts().blocked == 1,
              "cell buffer: an attached cell at the tail's end is left alone");
        b2.state = 4;
        g_flags[0x30] = 1;  // loading: no paced trim
        CellBufferTestGridHook(g_site2, reinterpret_cast<uint64_t>(m.tes.mem), 0, 1);
        Check(m.evicted.empty(), "cell buffer: no paced trim while the load flags are set");
        g_flags[0x30] = 0;
        uint64_t interior = 1;
        memcpy(m.tes.mem + 0x50, &interior, 8);
        CellBufferTestGridHook(g_site2, reinterpret_cast<uint64_t>(m.tes.mem), 0, 1);
        Check(m.evicted.empty(), "cell buffer: no paced trim in an interior");
        interior = 0;
        memcpy(m.tes.mem + 0x50, &interior, 8);
        CellBufferTestGridHook(g_site2, reinterpret_cast<uint64_t>(m.tes.mem), 0, 1);
        CellBufferTestGridHook(g_site2, reinterpret_cast<uint64_t>(m.tes.mem), 0, 1);
        Check(m.evicted.size() == 2 && m.evicted[0] == 2 && m.evicted[1] == 1 && m.tes.mem[0xA1] == 1,
              "cell buffer: then one cell per frame, the last first, flag set");
        SetCellBufferPacedEvict(0);
    }
}

}  // namespace cbuf
