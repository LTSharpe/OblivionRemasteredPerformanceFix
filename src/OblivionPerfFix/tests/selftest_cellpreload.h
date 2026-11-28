// CellObjectPreload self-test (worker preload 2026-11-18, S-008), included by selftest.cpp: on the mapped exe the cell reference
// walk (the attach's walk, Spawn_Paired_Actor's reads) and StaticLoadObject's still-loading flag test are found (nothing is
// installed; the key has no patch of its own); then with a fake look-up and a fake clock: the old-engine walk of fake cells
// (references without an actor, each base form once, run-time forms and static objects skipped, a base form again only after
// 20 s or after a freeze dropped it, a fault in the walk), the game thread's frame end (the form asset requested once, its
// class once it has loaded, at most 16 requests per frame, given up after 3 s), and the queues emptied at a freeze / travel
// and when the key is switched off. The fake look-up answers as the real one does: a package not resident is requested again
// every time the caller allows it (the real look-up cannot see a package in flight), so the "requested once" rule is the
// caller's (review rev-preload S1). CellObjectPreloadRelook (bookrelook 2026-11-23): the cells walked ahead are remembered with
// their base forms; at the grid change those of the cells in the grid are queued again past the 20 s wait (no cell memory read
// then); a base form whose form asset request is followed is skipped, a class in flight is asked again (the loader merges it);
// switching either key off between the remember and the grid change queues nothing; at most 256 base forms per grid change,
// then the usual 16 requests per frame; the grid change's time is measured.
#pragma once

namespace cop {

struct FakeForm {
    uint32_t id;
    int state;  // what the fake look-up answers: 0 nothing, 1 form asset missing, 2 class missing, 3 loaded
};
FakeForm g_forms[400];
int g_nForms = 0;
uint64_t g_now = 1000000;
uint64_t Now() { return g_now; }
FakeForm* Find(uint32_t id)
{
    for (int i = 0; i < g_nForms; ++i)
        if (g_forms[i].id == id) return &g_forms[i];
    return nullptr;
}
int Step(uint32_t form, uint32_t ask, uint32_t* requested)
{
    *requested = 0;
    FakeForm* f = Find(form);
    if (!f) return 0;
    if (f->state == 1 && (ask & 1)) *requested = 1;
    if (f->state == 2 && (ask & 2)) *requested = 2;
    return f->state;
}
void AddForm(uint32_t id, int state) { g_forms[g_nForms++] = {id, state}; }

// a fake reference: type +0x08, id +0x10, paired actor +0x48, base form +0x50; a fake base form: type +0x08, id +0x10
struct alignas(16) Obj {
    uint8_t b[0x60];
};
struct Node {
    const void* ref;
    const Node* next;
};

inline void CellObjectPreloadSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    const char* missing = nullptr;
    const bool found = CellObjectPreloadDiscover(game, &missing);
    Check(found, "cellpreload: the cell reference walk (the attach's walk, Spawn_Paired_Actor's reads) and StaticLoadObject's "
                 "still-loading flag test found");
    if (!found) Log("[selftest] cellpreload: %s not recognised", missing ? missing : "?");

    // fakes
    static Obj bases[6], refs[8];
    static uint8_t cell[0x100];
    static Node nodes[8];
    memset(bases, 0, sizeof bases);
    memset(refs, 0, sizeof refs);
    memset(cell, 0, sizeof cell);
    const uint32_t ids[6] = {0x100, 0x200, 0x300, 0xFF000123u, 0x400, 0x500};
    for (int i = 0; i < 6; ++i) {
        memcpy(bases[i].b + 0x10, &ids[i], 4);
        bases[i].b[0x08] = 0x1B;  // MISC
    }
    bases[5].b[0x08] = 0x1C;  // STAT
    // refs: 0 -> 0x100, 1 -> 0x100 again, 2 -> 0x200 with an actor, 3 -> no base, 4 -> run-time FF form, 5 -> 0x300, 6 -> 0x400,
    // 7 -> 0x500 (a static object)
    const int baseOf[8] = {0, 0, 1, -1, 3, 2, 4, 5};
    for (int i = 0; i < 8; ++i) {
        const void* bp = baseOf[i] >= 0 ? static_cast<const void*>(&bases[baseOf[i]]) : nullptr;
        memcpy(refs[i].b + 0x50, &bp, 8);
        refs[i].b[0x08] = 0x31;
        const uint32_t rid = 0x9000 + static_cast<uint32_t>(i);
        memcpy(refs[i].b + 0x10, &rid, 4);
    }
    const uint64_t actor = 0x12345678;
    memcpy(refs[2].b + 0x48, &actor, 8);
    // the first node lives in the cell (+0xB8), the rest are chained
    for (int i = 1; i < 8; ++i) nodes[i] = {&refs[i], i + 1 < 8 ? &nodes[i + 1] : nullptr};
    Node first{&refs[0], &nodes[1]};
    memcpy(cell + 0xB8, &first, sizeof first);

    g_nForms = 0;
    AddForm(0x100, 1);  // form asset missing
    AddForm(0x300, 3);  // loaded
    AddForm(0x400, 2);  // class missing
    AddForm(0x500, 1);  // a static object (form asset missing): must not be noted
    g_now = 1000000;
    CellObjectPreloadTestSetup(&Step, &Now);
    CellObjectPreloadCell(cell);  // key off: nothing
    Check(CellObjectPreloadTestRead().queued == 0, "cellpreload: with the key off a cell is not walked");
    SetCellObjectPreload(1);
    CellObjectPreloadCell(cell);
    auto c = CellObjectPreloadTestRead();
    Check(c.queued == 3 && c.otQueue == 3 && c.statSkip == 1,
          "cellpreload: a cell's references without an actor noted once per base form (0x100 twice -> once; with an actor, no base "
          "form, run-time form, static object skipped)");
    CellObjectPreloadCell(cell);
    Check(CellObjectPreloadTestRead().queued == 3, "cellpreload: the same base forms are not noted again within 20 s");
    CellObjectPreloadFrame(false);
    c = CellObjectPreloadTestRead();
    Check(c.formReqs == 1 && c.classReqs == 1 && c.resident == 1 && c.pending == 1 && c.todo == 0,
          "cellpreload: frame end: the missing form asset and the missing class requested, the loaded one left alone, the form "
          "asset in flight followed");
    CellObjectPreloadFrame(false);  // looked at again only after 4 frames
    CellObjectPreloadFrame(false);
    CellObjectPreloadFrame(false);
    Check(CellObjectPreloadTestRead().looks == 3, "cellpreload: a followed form asset is looked at again only every 4 frames");
    CellObjectPreloadFrame(false);  // still loading (the real look-up sees it as not resident): looked at, not requested again
    c = CellObjectPreloadTestRead();
    Check(c.looks == 4 && c.formReqs == 1 && c.pending == 1, "cellpreload: a form asset in flight is not requested twice");
    Find(0x100)->state = 2;  // its form asset has loaded: now its class is missing
    for (int i = 0; i < 4; ++i) CellObjectPreloadFrame(false);
    c = CellObjectPreloadTestRead();
    Check(c.classReqs == 2 && c.pending == 0 && c.formReqs == 1,
          "cellpreload: once the form asset has loaded its class is requested, then it is let go");
    g_now += 20001;
    CellObjectPreloadCell(cell);
    Check(CellObjectPreloadTestRead().queued == 6, "cellpreload: after 20 s the same base forms are noted again");
    CellObjectPreloadFrame(true);  // a freeze / pending travel: the queues go
    c = CellObjectPreloadTestRead();
    Check(c.todo == 0 && c.otQueue == 0 && c.pending == 0 && c.looks == 5, "cellpreload: at a freeze or pending travel nothing is "
                                                                            "looked up and the queues are emptied");
    CellObjectPreloadCell(cell);  // the bases the freeze dropped may be noted again at once
    Check(CellObjectPreloadTestRead().queued == 9, "cellpreload: base forms a freeze dropped are noted again by the next cell");
    CellObjectPreloadFrame(true);

    // the per-frame request cap and the give-up
    CellObjectPreloadTestSetup(&Step, &Now);
    g_nForms = 0;
    static Obj many[100], manyRefs[100];
    static Node manyNodes[100];
    static uint8_t cell2[0x100];
    memset(many, 0, sizeof many);
    memset(manyRefs, 0, sizeof manyRefs);
    memset(cell2, 0, sizeof cell2);
    for (int i = 0; i < 100; ++i) {
        const uint32_t id = 0x1000 + static_cast<uint32_t>(i);
        memcpy(many[i].b + 0x10, &id, 4);
        const void* bp = &many[i];
        memcpy(manyRefs[i].b + 0x50, &bp, 8);
        AddForm(id, 1);
    }
    for (int i = 1; i < 100; ++i) manyNodes[i] = {&manyRefs[i], i + 1 < 100 ? &manyNodes[i + 1] : nullptr};
    Node first2{&manyRefs[0], &manyNodes[1]};
    memcpy(cell2 + 0xB8, &first2, sizeof first2);
    SetCellObjectPreload(1);
    CellObjectPreloadCell(cell2);
    CellObjectPreloadFrame(false);
    c = CellObjectPreloadTestRead();
    Check(c.queued == 100 && c.formReqs == 16 && c.todo == 84 && c.pending == 16, "cellpreload: at most 16 requests in one frame, the "
                                                                                  "rest wait in order");
    for (int i = 0; i < 8; ++i) CellObjectPreloadFrame(false);
    c = CellObjectPreloadTestRead();
    Check(c.formReqs == 100 && c.todo == 0, "cellpreload: the rest requested over the next frames");
    g_now += 3001;  // none of the form assets loads: given up after 3 s
    for (int i = 0; i < 12; ++i) CellObjectPreloadFrame(false);
    c = CellObjectPreloadTestRead();
    Check(c.gaveUp == 100 && c.pending == 0 && c.formReqs == 100, "cellpreload: a form asset still loading after 3 s is let go, "
                                                                   "never requested again");
    // key off empties the queues
    g_now += 20001;
    CellObjectPreloadCell(cell2);
    SetCellObjectPreload(0);
    CellObjectPreloadFrame(false);
    c = CellObjectPreloadTestRead();
    Check(c.todo == 0 && c.otQueue == 0 && c.pending == 0, "cellpreload: switched off, the queues are emptied");
    // a fault in the walk
    static uint8_t bad[0x100];
    memset(bad, 0, sizeof bad);
    const uintptr_t wild = 0x10;  // an unreadable reference
    memcpy(bad + 0xB8, &wild, 8);
    SetCellObjectPreload(1);
    const int q0 = CellObjectPreloadTestRead().queued;
    CellObjectPreloadCell(bad);
    c = CellObjectPreloadTestRead();
    Check(c.walkFaults == 1 && c.queued == q0, "cellpreload: a fault in a cell's walk is caught and notes nothing");

    // ---- CellObjectPreloadRelook ----
    CellObjectPreloadTestSetup(&Step, &Now);
    g_nForms = 0;
    AddForm(0x100, 3);  // loaded now
    AddForm(0x300, 3);
    AddForm(0x400, 3);
    AddForm(0x500, 1);
    static uint8_t* grid[25];
    for (auto& e : grid) e = nullptr;
    static uint8_t other[0x100];
    grid[7] = cell;
    grid[8] = other;
    SetCellObjectPreload(1);
    CellObjectPreloadCell(cell, -37, -9);  // relook off: the cell is not remembered
    c = CellObjectPreloadTestRead();
    Check(c.ahead == 0 && c.queued == 3, "cellpreload relook: with the option off no cell is remembered");
    CellObjectPreloadShift(grid, 25);
    CellObjectPreloadFrame(false);
    c = CellObjectPreloadTestRead();
    Check(c.relookCells == 0 && c.relookQueued == 0 && c.looks == 3, "cellpreload relook: with the option off a grid change does nothing");
    g_now += 3000;
    SetCellObjectPreloadRelook(1);
    CellObjectPreloadCell(cell, -37, -9);  // within 20 s of the first note: not noted again, but remembered
    CellObjectPreloadCell(cell, -37, -9);
    c = CellObjectPreloadTestRead();
    Check(c.ahead == 1 && c.queued == 3, "cellpreload relook: a cell walked ahead is remembered once, its bases not noted again within 20 s");
    // a package freed by a garbage collection between the look-up and the create (variant A): all three gone
    Find(0x100)->state = 1;
    Find(0x300)->state = 1;
    Find(0x400)->state = 2;
    const int cells0 = CellObjectPreloadTestRead().cells;
    CellObjectPreloadShift(grid, 25);
    c = CellObjectPreloadTestRead();
    Check(c.relookCells == 1 && c.relookQueued == 3 && c.ahead == 0 && c.otQueue == 3 && c.cells == cells0,
          "cellpreload relook: at the grid change the stored base forms of the remembered cell in the grid are queued again, past the "
          "20 s wait (its 3 base forms; the cell is not walked again)");
    CellObjectPreloadFrame(false);
    c = CellObjectPreloadTestRead();
    Check(c.relookLooks == 3 && c.formReqs == 2 && c.classReqs == 1 && c.pending == 2 && c.relookReqs == 3,
          "cellpreload relook: what was freed is requested again (2 form assets, 1 class) at the next frame end");
    // the same grid change again, while the two form assets' requests are pending: those two are not asked for twice
    CellObjectPreloadCell(cell, -37, -9);
    CellObjectPreloadShift(grid, 25);
    CellObjectPreloadFrame(false);
    c = CellObjectPreloadTestRead();
    Check(c.relookSkipped == 2 && c.relookLooks == 4 && c.formReqs == 2 && c.classReqs == 2,
          "cellpreload relook: a base form whose form asset request is still followed is skipped, not asked for twice; a class "
          "package in flight is asked for again (1 -> 2 class requests; the loader merges it into the load in flight)");
    // a remembered cell that is not in the grid is not walked
    for (int i = 0; i < 4; ++i) CellObjectPreloadFrame(false);
    CellObjectPreloadCell(cell, -37, -9);
    grid[7] = nullptr;
    CellObjectPreloadShift(grid, 25);
    c = CellObjectPreloadTestRead();
    Check(c.relookCells == 2 && c.ahead == 0 && c.otQueue == 0, "cellpreload relook: a remembered cell that the grid change did not attach queues nothing");
    grid[7] = cell;
    // either key switched off between the remember and the grid change: nothing is queued, and the list is emptied
    CellObjectPreloadCell(cell, -37, -9);
    SetCellObjectPreloadRelook(0);
    CellObjectPreloadShift(grid, 25);
    c = CellObjectPreloadTestRead();
    Check(c.relookCells == 2 && c.ahead == 0 && c.otQueue == 0, "cellpreload relook: switched off before the grid change: nothing queued");
    SetCellObjectPreloadRelook(1);
    CellObjectPreloadCell(cell, -37, -9);
    SetCellObjectPreload(0);
    CellObjectPreloadShift(grid, 25);
    c = CellObjectPreloadTestRead();
    Check(c.relookCells == 2 && c.ahead == 0 && c.otQueue == 0, "cellpreload relook: CellObjectPreload off before the grid change: nothing queued");
    SetCellObjectPreload(1);
    // a freeze drops the queue: nothing is looked up
    CellObjectPreloadCell(cell, -37, -9);
    CellObjectPreloadShift(grid, 25);
    CellObjectPreloadFrame(true);
    c = CellObjectPreloadTestRead();
    Check(c.todo == 0 && c.otQueue == 0 && c.pending == 0, "cellpreload relook: at a freeze the re-look's queue is emptied too");

    // the cap: 3 cells of 100 base forms each in the grid: 256 queued, 44 dropped, then 16 requests per frame, the walk is timed
    CellObjectPreloadTestSetup(&Step, &Now);
    g_nForms = 0;
    static Obj bb[300], bbRefs[300];
    static Node bbNodes[300];
    static uint8_t bigCells[3][0x100];
    memset(bb, 0, sizeof bb);
    memset(bbRefs, 0, sizeof bbRefs);
    memset(bigCells, 0, sizeof bigCells);
    for (int i = 0; i < 300; ++i) {
        const uint32_t id = 0x2000 + static_cast<uint32_t>(i);
        memcpy(bb[i].b + 0x10, &id, 4);
        const void* bp = &bb[i];
        memcpy(bbRefs[i].b + 0x50, &bp, 8);
        AddForm(id, 1);
    }
    for (int k = 0; k < 3; ++k) {
        for (int i = 1; i < 100; ++i) bbNodes[k * 100 + i] = {&bbRefs[k * 100 + i], i + 1 < 100 ? &bbNodes[k * 100 + i + 1] : nullptr};
        Node first3{&bbRefs[k * 100], &bbNodes[k * 100 + 1]};
        memcpy(bigCells[k] + 0xB8, &first3, sizeof first3);
        grid[k] = bigCells[k];
    }
    SetCellObjectPreload(1);
    SetCellObjectPreloadRelook(1);
    for (int k = 0; k < 3; ++k) CellObjectPreloadCell(bigCells[k], k, 0);
    c = CellObjectPreloadTestRead();
    Check(c.ahead == 3 && c.queued == 300, "cellpreload relook: three cells walked ahead are remembered");
    for (int i = 0; i < 24; ++i) CellObjectPreloadFrame(false);  // 256 requested and followed (16 per frame), 44 wait for room
    g_now += 3001;
    for (int i = 0; i < 40; ++i) CellObjectPreloadFrame(false);  // those given up (nothing loads); the 44 requested
    g_now += 3001;
    for (int i = 0; i < 40; ++i) CellObjectPreloadFrame(false);  // the 44 given up
    c = CellObjectPreloadTestRead();
    Check(c.pending == 0 && c.todo == 0 && c.gaveUp == 300, "cellpreload relook: the 300 first look-ups are all given up (set-up of the cap test)");
    g_now += 100;
    const int looks0 = CellObjectPreloadTestRead().looks;
    CellObjectPreloadShift(grid, 25);
    c = CellObjectPreloadTestRead();
    Check(c.relookCells == 3 && c.relookQueued == 256 && c.relookDropped == 44 && c.otQueue == 256,
          "cellpreload relook: at most 256 base forms per grid change (3 x 100 -> 256 queued, 44 dropped)");
    Check(c.relookUsMax < 1000, "cellpreload relook: the grid change's queueing of 3 cells' stored base forms (300) took under 1 ms");
    Log("[selftest] cellpreload relook: the grid change's queueing of 3 cells / 300 stored base forms took %d us", c.relookUsMax);
    CellObjectPreloadFrame(false);
    c = CellObjectPreloadTestRead();
    Check(c.looks - looks0 == 16 && c.pending == 16, "cellpreload relook: the next frame end makes at most 16 requests (16 look-ups here), the rest wait");
    for (int i = 0; i < 40; ++i) CellObjectPreloadFrame(false);
    c = CellObjectPreloadTestRead();
    Check(c.todo == 0 && c.relookLooks == 256, "cellpreload relook: the 256 are all looked up over the next frames");

    SetCellObjectPreloadRelook(0);
    SetCellObjectPreload(0);
    CellObjectPreloadTestSetup(nullptr, nullptr);
}

}  // namespace cop
