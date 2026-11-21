// CellObjectPreload self-test (worker preload 2026-11-18, S-008), included by selftest.cpp: on the mapped exe the cell reference
// walk (the attach's walk, Spawn_Paired_Actor's reads) and StaticLoadObject's still-loading flag test are found (nothing is
// installed; the key has no patch of its own); then with a fake look-up and a fake clock: the old-engine walk of fake cells
// (references without an actor, each base form once, run-time forms and static objects skipped, a base form again only after
// 20 s or after a freeze dropped it, a fault in the walk), the game thread's frame end (the form asset requested once, its
// class once it has loaded, at most 16 requests per frame, given up after 3 s), and the queues emptied at a freeze / travel
// and when the key is switched off. The fake look-up answers as the real one does: a package not resident is requested again
// every time the caller allows it (the real look-up cannot see a package in flight), so the "requested once" rule is the
// caller's (review rev-preload S1).
#pragma once

namespace cop {

struct FakeForm {
    uint32_t id;
    int state;  // what the fake look-up answers: 0 nothing, 1 form asset missing, 2 class missing, 3 loaded
};
FakeForm g_forms[160];
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
    SetCellObjectPreload(0);
    CellObjectPreloadTestSetup(nullptr, nullptr);
}

}  // namespace cop
