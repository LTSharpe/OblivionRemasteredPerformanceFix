// ImpactPreload self-test (worker impactload 2026-11-12), included by selftest.cpp: on the mapped exe the impact table's row
// lookup (its settings getter, the settings object +0x110, the table path +0x240, the row list and its count, GUObjectArray, the
// root bit), the getter's static-class cache, the pawn impact timer's class load (+0x80) and the hit handler's row / blood reads
// are found once (nothing installed); then the state machine with fakes: nothing before the settings exist, the table requested
// and waited for, the game's row-list set-up called once, classes deduplicated by package, an empty slot skipped, a class already
// loaded rooted at once, a requested one rooted when it is loaded, done after that; the key off does nothing.
#pragma once

namespace ipl {

struct Fake {
    uint8_t* resident[16];
    int nRes = 0;
    const void* requests[16];
    int nReq = 0;
    int inits = 0;
} f;

alignas(16) uint8_t cls[0x200];
alignas(16) uint8_t cdo[0x300];
alignas(16) uint8_t rows[4][0x100];
uint8_t* rowPtrs[4];
struct List { uint8_t** data; int32_t num, max; } list;
alignas(16) uint8_t objs[4][0x40];  // fake UObjects (InternalIndex at +0xC)
alignas(16) uint8_t items[4][0x18];  // fake GUObjectArray items (object, flags at +8)
uint8_t* chunk0;
uint8_t** chunks;
uint8_t* classGlobal;
int32_t objNum = 4;

// path -> object: the object whose "path" (the first 8 bytes = package FName) matches a resident path
const void* Resolve(const void* sp)
{
    for (int i = 0; i < f.nRes; ++i)
        if (f.resident[i] == sp) return objs[i];
    return nullptr;
}
void Request(const void* sp) { if (f.nReq < 16) f.requests[f.nReq++] = sp; }
void Init(void*)
{
    ++f.inits;
    list.data = rowPtrs;
    list.num = 4;
}
void MakeResident(uint8_t* sp, int obj)
{
    f.resident[obj] = sp;
    if (f.nRes <= obj) f.nRes = obj + 1;
}
void Frames(int n) { for (int i = 0; i < n; ++i) opf::ImpactPreloadFrame(); }
bool Rooted(int obj) { return (*reinterpret_cast<uint32_t*>(items[obj] + 8) & (1u << 30)) != 0; }

inline void ImpactPreloadSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    ImpactPreloadSites s{};
    const char* missing = nullptr;
    const bool found = ImpactPreloadDiscover(game, &s, &missing);
    Check(found, "impactpreload: the impact table's row lookup (getter, settings +0x110, table +0x240, row list, GUObjectArray, "
                 "root bit), the settings class cache, the pawn impact class load and the hit handler's reads found once");
    if (!found) Log("[selftest] impactpreload: %s not recognised", missing ? missing : "?");
    if (found && game.timeDateStamp == 0xF19077A4u) {
        Check(s.rowLookup == game.base + 0x47F5430 && s.rowList == game.base + 0x9304000 && s.classGetter == game.base + 0x46AD100,
              "impactpreload: row lookup exe+0x47F5430, row list exe+0x9304000, settings getter exe+0x46AD100 (build 19115871)");
        Check(reinterpret_cast<uint8_t*>(s.objNum) == game.base + 0x9112A84 &&
                  reinterpret_cast<uint8_t*>(s.objChunks) == game.base + 0x9112A70,
              "impactpreload: GUObjectArray NumElements exe+0x9112A84, chunk table exe+0x9112A70");
        Check(s.cdo == 0x110 && s.table == 0x240 && s.rowClass == 0x80 && s.blood == 0x78,
              "impactpreload: offsets settings +0x110, table +0x240, row class +0x80, blood class +0x78");
    }

    // ---- the state machine with fakes ----
    memset(cls, 0, sizeof cls);
    memset(cdo, 0, sizeof cdo);
    memset(rows, 0, sizeof rows);
    memset(items, 0, sizeof items);
    f = Fake{};
    for (int i = 0; i < 4; ++i) {
        memset(objs[i], 0, sizeof objs[i]);
        *reinterpret_cast<int32_t*>(objs[i] + 0xC) = i;
        *reinterpret_cast<void**>(items[i]) = objs[i];
    }
    chunk0 = &items[0][0];
    chunks = &chunk0;
    for (int i = 0; i < 4; ++i) rowPtrs[i] = rows[i];
    list = {nullptr, 0, 0};
    classGlobal = nullptr;
    *reinterpret_cast<uint8_t**>(cls + 0x110) = cdo;
    // table path (cdo+0x240), blood class path (cdo+0x78+8), row classes (row+0x80+8): package FName in the first 8 bytes
    *reinterpret_cast<uint64_t*>(cdo + 0x240) = 0x100;
    *reinterpret_cast<uint64_t*>(cdo + 0x80) = 0x200;
    *reinterpret_cast<uint64_t*>(rows[0] + 0x88) = 0x300;
    *reinterpret_cast<uint64_t*>(rows[1] + 0x88) = 0x300;  // the same class in two rows: requested once
    // rows[2] + 0x88 = 0: an empty slot
    *reinterpret_cast<uint64_t*>(rows[3] + 0x88) = 0x400;  // a second class: requested after the first one is in
    ImpactPreloadFakes fk{};
    fk.resolve = &Resolve;
    fk.request = &Request;
    fk.initRows = &Init;
    fk.classGlobal = &classGlobal;
    fk.rowList = reinterpret_cast<uint8_t*>(&list);
    fk.objNum = &objNum;
    fk.objChunks = &chunks;
    ImpactPreloadTestSetup(&fk);
    MakeResident(cdo + 0x80, 1);  // the blood class is loaded already

    Frames(60);
    ImpactPreloadTestState st = ImpactPreloadTestRead();
    Check(st.state == 0 && f.nReq == 0 && f.inits == 0, "impactpreload: nothing before the settings object exists");
    classGlobal = cls;
    Frames(15);
    st = ImpactPreloadTestRead();
    Check(st.state == 1 && f.nReq == 1 && f.requests[0] == cdo + 0x240 && f.inits == 0,
          "impactpreload: the impact table is requested when it is not loaded (no row set-up yet)");
    Frames(45);
    Check(ImpactPreloadTestRead().state == 1 && f.nReq == 1, "impactpreload: the table is requested once and waited for");
    MakeResident(cdo + 0x240, 0);
    Frames(15);
    st = ImpactPreloadTestRead();
    Check(f.inits == 1 && st.rows == 4, "impactpreload: the game's row-list set-up is called once the table is loaded");
    Check(st.state == 2 && f.nReq == 2 && f.requests[1] == rows[0] + 0x88 && st.requested == 2,
          "impactpreload: the first row class not loaded is requested, one request at a time (two rows with the same class, "
          "one empty slot)");
    Check(st.resident == 1 && st.rooted == 1 && Rooted(1) && !Rooted(2), "impactpreload: a class already loaded is kept (rooted) at once");
    Frames(30);
    Check(ImpactPreloadTestRead().state == 2 && !Rooted(2), "impactpreload: a requested class is waited for");
    MakeResident(rows[0] + 0x88, 2);
    Frames(15);
    st = ImpactPreloadTestRead();
    Check(st.state == 2 && st.rooted == 2 && Rooted(2) && f.nReq == 3 && f.requests[2] == rows[3] + 0x88,
          "impactpreload: the loaded class is kept (rooted), then the next one is requested");
    MakeResident(rows[3] + 0x88, 3);
    Frames(15);
    st = ImpactPreloadTestRead();
    Check(st.state == 3 && st.rooted == 3 && Rooted(3) && st.failed == 0 && f.inits == 1 && f.nReq == 3,
          "impactpreload: the last class kept, then done; the row set-up ran once");
    Check(!Rooted(0), "impactpreload: the table itself is left to the game's set-up (not rooted here)");
    Frames(60);
    Check(f.nReq == 3 && f.inits == 1, "impactpreload: nothing more after done");

    // the key off: nothing happens
    ImpactPreloadTestSetup(&fk);
    f = Fake{};
    list = {nullptr, 0, 0};
    SetImpactPreload(0);
    Frames(60);
    Check(f.nReq == 0 && f.inits == 0 && ImpactPreloadTestRead().state == 0, "impactpreload: with the key off nothing is requested");
    ImpactPreloadTestSetup(nullptr);
}

}  // namespace ipl
