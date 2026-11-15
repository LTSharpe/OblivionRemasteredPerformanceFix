// RowPreload self-test (worker rowpreload 2026-11-12), included by selftest.cpp after the cell buffer's test (it reuses
// CellBufferPacedEvict's sites): on the mapped exe the request stub (lookup, interior test, exterior-buffer test, TES pointer),
// the shift preparation's cell load, the game's load-then-insert loop, three request calls in the grid change and its no-shift
// branch are found once (nothing installed); then the logic with fakes: requests noted once each, a cell taken only in a calm
// frame (not busy, not right after a shift, not frozen, buffer room, safe flags), a cell outside the next ring or of another
// worldspace dropped, a buffered cell skipped, a loaded cell parked with the game's insert, one load per frame, a shift clears
// the notes and counts the parked cells it attached, interior / load flags clear the notes, the key off forwards only.
#pragma once

namespace rpl {

constexpr int kG = 5;
struct FCell {
    uint8_t pad[0x7B];
    uint8_t state;  // +0x7B
    uint8_t pad2[4];
    int32_t x, y;
    bool buffered;
};
FCell cells[16];
int nCells = 0;
uint64_t world = 0x5000, otherWorld = 0x6000;
uint8_t tesMem[0x100];
uint8_t* buf[64];
uint64_t tesSlot = 0;
uint8_t flagsObj[0x40];
uint64_t flagsSlot = 0;
uint8_t background = 1;
uint32_t grids = kG;
int stubCalls = 0, loads = 0, inserts = 0;
bool loadingNow = false;
bool loadMakesNothing = false;

FCell* Find(int32_t x, int32_t y)
{
    for (int i = 0; i < nCells; ++i)
        if (cells[i].x == x && cells[i].y == y) return &cells[i];
    return nullptr;
}
void Stub(uint64_t, uint64_t, int32_t, int32_t) { ++stubCalls; }
uint8_t* Lookup(uint64_t w, int32_t x, int32_t y) { return w == world ? reinterpret_cast<uint8_t*>(Find(x, y)) : nullptr; }
uint8_t IsInterior(const uint8_t*) { return 0; }
uint8_t InExt(uint64_t, uint64_t c) { return reinterpret_cast<FCell*>(c)->buffered ? 1 : 0; }
uint8_t* Load(uint64_t w, int32_t x, int32_t y)
{
    ++loads;
    if (loadMakesNothing || w != world) return nullptr;
    FCell* c = Find(x, y);
    if (!c && nCells < 16) {
        c = &cells[nCells++];
        memset(c, 0, sizeof *c);
        c->x = x;
        c->y = y;
    }
    if (c) c->state = 0;  // the game's load leaves a cell data-loaded, not attached, no 3D root yet: state 0 (rev94c finding 1)
    return reinterpret_cast<uint8_t*>(c);
}
void Insert(uint64_t, uint64_t c)
{
    ++inserts;
    reinterpret_cast<FCell*>(c)->buffered = true;
}
uint8_t Loading(uint64_t) { return loadingNow ? 1 : 0; }

void Grid(int32_t cx, int32_t cy)
{
    memcpy(tesMem + 0x38, &cx, 4);
    memcpy(tesMem + 0x3C, &cy, 4);
}

inline void RowPreloadSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    RowPreloadSites s{};
    const char* missing = nullptr;
    const bool found = FindCellBufferPacedEvict() && RowPreloadDiscover(game, &s, &missing);
    Check(found, "rowpreload: the request stub, the shift preparation's cell load, the load-then-insert loop, three request calls "
                 "in the grid change and its no-shift branch found once");
    if (!found) Log("[selftest] rowpreload: %s not recognised", missing ? missing : "?");
    if (found && game.timeDateStamp == 0xF19077A4u) {
        const auto rva = [&](const uint8_t* p) { return static_cast<uint32_t>(p - game.base); };
        Check(rva(s.stub) == 0x6926CA0 && rva(s.calls[0]) == 0x66B21D8 && rva(s.calls[1]) == 0x66B2228 && rva(s.calls[2]) == 0x66B225B,
              "rowpreload: request stub exe+0x6926CA0, calls at exe+0x66B21D8 / 0x66B2228 / 0x66B225B (build 19115871)");
        Check(rva(s.load) == 0x67227D0 && rva(s.insert) == 0x66A9AB0 && rva(s.lookup) == 0x6720A00 && rva(s.isInterior) == 0x665B760 &&
                  rva(s.inExtBuffer) == 0x66AC7A0 && rva(s.loadingTest) == 0x670CE30,
              "rowpreload: cell load exe+0x67227D0, insert exe+0x66A9AB0, lookup exe+0x6720A00, interior test exe+0x665B760, "
              "exterior-buffer test exe+0x66AC7A0, loading test exe+0x670CE30");
    }

    // ---- the logic with fakes ----
    memset(tesMem, 0, sizeof tesMem);
    memset(flagsObj, 0, sizeof flagsObj);
    uint8_t** b = buf;
    memcpy(tesMem + 0x60, &b, 8);
    memcpy(tesMem + 0xB0, &world, 8);
    tesSlot = reinterpret_cast<uint64_t>(tesMem);
    flagsSlot = reinterpret_cast<uint64_t>(flagsObj);
    RowPreloadFakes f{reinterpret_cast<void*>(&Stub), reinterpret_cast<void*>(&Lookup), reinterpret_cast<void*>(&IsInterior),
                      reinterpret_cast<void*>(&InExt), reinterpret_cast<void*>(&Load), reinterpret_cast<void*>(&Insert),
                      reinterpret_cast<void*>(&Loading), &background, &tesSlot, &flagsSlot, &grids};
    RowPreloadTestSetup(&f);
    nCells = 0;
    stubCalls = loads = inserts = 0;
    SetRowPreload(1);
    Grid(10, 20);
    uint8_t* const tes = tesMem;
    // the next row east: x = 13, y = 18..22; plus one repeat and one far (stale) request
    for (int y = 18; y <= 22; ++y) RowPreloadTestRequest(1, world, 13, y);
    RowPreloadTestRequest(1, world, 13, 20);
    RowPreloadTestRequest(1, world, 15, 20);       // 5 from the centre: never the next ring
    RowPreloadTestRequest(1, otherWorld, 13, 20);  // another worldspace
    auto c = RowPreloadTestRead();
    Check(stubCalls == 8 && c.requests == 8 && c.dupes == 1 && c.pending == 7,
          "rowpreload: every request forwarded to the game's stub; noted once each");
    RowPreloadOtStep(tes, true, 9);
    c = RowPreloadTestRead();
    Check(loads == 0 && c.skipBusy == 1, "rowpreload: nothing in a frame that shifted or unloaded a cell");
    RowPreloadTestGt(2);
    RowPreloadOtStep(tes, false, 9);
    RowPreloadTestGt(1);
    RowPreloadOtStep(tes, false, 0);
    c = RowPreloadTestRead();
    Check(loads == 0 && c.skipFrozen == 1 && c.skipTail == 1, "rowpreload: nothing while a menu / loading screen is up or the spare slots are full");
    RowPreloadOtStep(tes, false, 9);
    c = RowPreloadTestRead();
    Check(loads == 1 && inserts == 1 && c.loaded == 1 && c.pending == 6, "rowpreload: one cell loaded and parked in one calm frame");
    // a cell already loaded and buffered is skipped without a load; then the rest one per frame
    FCell* const pre = Find(13, 18);
    Check(pre && pre->state == 0 && pre->buffered, "rowpreload: the parked cell is data-loaded (state 0), not attached, in the buffer");
    {
        FCell& k = cells[nCells++];
        memset(&k, 0, sizeof k);
        k.x = 13;
        k.y = 19;
        k.state = 0;
        k.buffered = true;
    }
    for (int i = 0; i < 4; ++i) RowPreloadOtStep(tes, false, 9);
    c = RowPreloadTestRead();
    Check(loads == 4 && inserts == 4 && c.already == 1 && c.loaded == 4, "rowpreload: a buffered cell skipped; one load per frame");
    for (int i = 0; i < 4; ++i) RowPreloadOtStep(tes, false, 9);
    c = RowPreloadTestRead();
    Check(loads == 4 && c.stale == 2 && c.pending == 0, "rowpreload: a far request and another worldspace's dropped without a load");
    // the shift east: the grid part holds three of the parked cells
    Grid(11, 20);
    memset(buf, 0, sizeof buf);
    buf[0] = reinterpret_cast<uint8_t*>(Find(13, 18));
    buf[3] = reinterpret_cast<uint8_t*>(Find(13, 20));
    buf[24] = reinterpret_cast<uint8_t*>(Find(13, 21));
    buf[30] = reinterpret_cast<uint8_t*>(Find(13, 22));  // behind the grid part: not attached
    RowPreloadTestRequest(1, world, 14, 20);
    RowPreloadGridCall(tes, true);
    c = RowPreloadTestRead();
    Check(c.hits == 3 && c.unused == 1 && c.pending == 0 && c.cleared == 1,
          "rowpreload: a shift counts the parked cells it attached and clears the notes");
    RowPreloadTestRequest(1, world, 14, 20);
    for (int i = 0; i < 8; ++i) RowPreloadOtStep(tes, false, 9);
    c = RowPreloadTestRead();
    Check(loads == 4 && c.skipSettle == 8 && c.pending == 1, "rowpreload: nothing in the 8 OT frames after a shift");
    *reinterpret_cast<uint32_t*>(flagsObj + 0x30) = 0x800;
    RowPreloadOtStep(tes, false, 9);
    c = RowPreloadTestRead();
    Check(loads == 4 && c.skipUnsafe == 1 && c.pending == 0, "rowpreload: the grid change's load flag set: no load, the notes cleared");
    *reinterpret_cast<uint32_t*>(flagsObj + 0x30) = 0;
    RowPreloadTestRequest(1, world, 14, 21);
    loadingNow = true;
    RowPreloadOtStep(tes, false, 9);
    loadingNow = false;
    RowPreloadTestRequest(1, world, 14, 21);
    uint64_t interior = 1;
    memcpy(tesMem + 0x50, &interior, 8);
    RowPreloadOtStep(tes, false, 9);
    interior = 0;
    memcpy(tesMem + 0x50, &interior, 8);
    c = RowPreloadTestRead();
    Check(loads == 4 && c.skipUnsafe == 3, "rowpreload: no load while the game loads or in an interior");
    // an attached cell is never touched; a request with no record loads nothing and is counted
    {
        FCell& k = cells[nCells++];
        memset(&k, 0, sizeof k);
        k.x = 14;
        k.y = 22;
        k.state = 6;
    }
    RowPreloadTestRequest(1, world, 14, 22);
    RowPreloadOtStep(tes, false, 9);
    loadMakesNothing = true;
    RowPreloadTestRequest(1, world, 14, 18);
    RowPreloadOtStep(tes, false, 9);
    loadMakesNothing = false;
    c = RowPreloadTestRead();
    Check(loads == 5 && inserts == 4 && c.attached == 1 && c.noCell == 1, "rowpreload: an attached cell skipped; no record = nothing parked");
    // off: requests forwarded, nothing noted, nothing loaded
    SetRowPreload(0);
    RowPreloadTestRequest(1, world, 14, 19);
    RowPreloadOtStep(tes, false, 9);
    c = RowPreloadTestRead();
    Check(stubCalls == 15 && c.pending == 0 && loads == 5, "rowpreload: off = the game's request only");
    RowPreloadTestSetup(nullptr);
    Check(Config().rowPreload == 1, "rowpreload: RowPreload defaults to 1 (on since 1.5.0)");
}

}  // namespace rpl
