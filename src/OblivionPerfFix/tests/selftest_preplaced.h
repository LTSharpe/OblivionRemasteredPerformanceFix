// PrePlacedRecordGuard self-test (worker crash072b 2026-11-21, record recreation recguard 2026-11-23, S-072), included by
// selftest.cpp: the discovery on the mapped exe (the adoption's level check, GUID look-up and record checks, the exit's
// restores, the spawn branch, the game's Send_State handler and the flow command's string copy / cancel test; it must still
// be found after FlowOverlapsOnce patched the adoption's entry), the decision with fake game functions (record made ->
// continue; cancelled pairing, no record afterwards, mode 2 -> exit), the install (both jz go to stubs that call our
// functions and continue at the record checks or the exit's restores; read-execute pages), a second install finds it in place.
#pragma once
namespace preplaced_test {
struct FakePair {  // one TSet element of a level's record map: GUID key, value, hash links
    uint32_t key[4];
    void* value;
    int32_t next, bucket;
};
FakePair g_pairs[2];
FakePair* g_map = g_pairs;  // the map's first field: its element data
bool g_present = false;     // whether the fake look-up finds the GUID (element 1)
bool g_makes = true;        // whether the fake Send_State makes the record
int g_sendCalls = 0;
uint64_t g_sendClaim = 0;
uint32_t g_sendGuid[4] = {};
char g_sendName[32] = {};
uint8_t g_record[0x48] = {};
struct Str {
    uint8_t buf[16];
    uint64_t size, cap;
};
uint32_t FakeHash(const void*, uint32_t size) { return size == 16 ? 0x1234u : 0u; }
int32_t FakeFind(void* map, uint32_t hash, const void* key)
{
    if (map != &g_map || hash != 0x1234u || !g_present) return -1;
    return memcmp(key, g_pairs[1].key, 16) == 0 ? 1 : -1;
}
Str* FakeCopy(Str* dst, const Str* src)
{
    *dst = *src;  // inline only (the test's names are short)
    return dst;
}
void FakeSend(void*, const uint32_t* guid, Str* s, uint64_t claimed)
{
    ++g_sendCalls;
    g_sendClaim = claimed;
    memcpy(g_sendGuid, guid, 16);
    memcpy(g_sendName, s->buf, 16);
    s->size = 0;  // the handler destroys its argument
    s->buf[0] = 0;
    if (g_makes) {
        memcpy(g_pairs[1].key, guid, 16);
        g_pairs[1].value = g_record;
        g_present = true;
    }
}
}  // namespace preplaced_test

static void PrePlacedRecordGuardRun(const GameModule& game)
{
    using namespace preplaced_test;
    PrePlacedRecordGuardInfo d{};
    Check(PrePlacedRecordGuardDiscover(game, &d), "PrePlacedRecordGuard: discovery on the mapped exe");
    Check(d.matches == 1, "PrePlacedRecordGuard: one match of the adoption's record lookup");
    Check(d.sendStateMatches == 1 && d.sendState, "PrePlacedRecordGuard: one Send_State handler, its calls match the adoption's");
    Check(d.flowCmdMatches >= 1 && d.strCopy && d.cancelOff == 0x12,
          "PrePlacedRecordGuard: the flow command's string copy and cancel byte +0x12");
    if (game.timeDateStamp == 0xF19077A4u) {
        Check(d.adopt - game.base == 0x472EFB0 && d.site - game.base == 0x472F0EC && d.restore - game.base == 0x472F4B0 &&
                  d.spawn - game.base == 0x472F1E8,
              "PrePlacedRecordGuard: adoption exe+0x472EFB0, GUID jz exe+0x472F0EC, exit restores exe+0x472F4B0, spawn exe+0x472F1E8 "
              "(build 19115871)");
        Check(d.nullSite - game.base == 0x472F112 && d.recheck - game.base == 0x472F10C && d.hash - game.base == 0x0E1D570 &&
                  d.findId - game.base == 0x1070E90,
              "PrePlacedRecordGuard: null-value jz exe+0x472F112, record checks exe+0x472F10C, hash exe+0xE1D570, look-up "
              "exe+0x1070E90 (build 19115871)");
        Check(d.sendState && d.sendState - game.base == 0x4732370 && d.strCopy && d.strCopy - game.base == 0x3F1D240,
              "PrePlacedRecordGuard: Send_State exe+0x4732370, string copy exe+0x3F1D240 (build 19115871)");
    }
    if (!d.site) return;
    const uint8_t* const spawnBefore = d.site + 6 + *reinterpret_cast<const int32_t*>(d.site + 2);
    const uint8_t* const nullBefore = d.nullSite + 6 + *reinterpret_cast<const int32_t*>(d.nullSite + 2);
    Check(spawnBefore == d.spawn && nullBefore == d.spawn + 3,
          "PrePlacedRecordGuard: before the install the jz go to the spawn branch (missing) and past its xor (null value)");

    // The decision with fakes
    PrePlacedRecordGuardFakes f{reinterpret_cast<void*>(&FakeSend), reinterpret_cast<void*>(&FakeCopy),
                                reinterpret_cast<void*>(&FakeHash), reinterpret_cast<void*>(&FakeFind), 1};
    uint8_t entry[0x20] = {};
    Str name{};
    memcpy(name.buf, "0.014.0-9.00.0", 15);
    name.size = 14;
    name.cap = 15;
    const uint32_t guid[4] = {0x48103D46u, 0x1DE1D641u, 0x980C5EA3u, 0x4DB9F26Au};
    void* const map = &g_map;
    g_present = false; g_makes = true; g_sendCalls = 0;
    void** slot = PrePlacedRecordGuardTestRecreate(&f, entry, &name, reinterpret_cast<const uint8_t*>(guid), map, false);
    Check(slot == &g_pairs[1].value && *slot == g_record && g_sendCalls == 1 && g_sendClaim == 1 &&
              memcmp(g_sendGuid, guid, 16) == 0 && strcmp(g_sendName, "0.014.0-9.00.0") == 0 && name.size == 14 &&
              strcmp(reinterpret_cast<char*>(name.buf), "0.014.0-9.00.0") == 0,
          "PrePlacedRecordGuard: a missing record is made by Send_State(GUID, a copy of the name, claimed) and its slot returned "
          "(the adoption's own string untouched)");
    g_present = false; g_sendCalls = 0;
    slot = PrePlacedRecordGuardTestRecreate(&f, entry, &name, reinterpret_cast<const uint8_t*>(guid), map, true);
    Check(slot == &g_pairs[1].value && g_sendCalls == 1, "PrePlacedRecordGuard: a record without value is made the same way");
    entry[0x12] = 1; g_present = false; g_sendCalls = 0;
    slot = PrePlacedRecordGuardTestRecreate(&f, entry, &name, reinterpret_cast<const uint8_t*>(guid), map, false);
    Check(!slot && g_sendCalls == 0, "PrePlacedRecordGuard: a cancelled pairing makes no record and takes the exit");
    entry[0x12] = 0; g_present = false; g_makes = false; g_sendCalls = 0;
    slot = PrePlacedRecordGuardTestRecreate(&f, entry, &name, reinterpret_cast<const uint8_t*>(guid), map, false);
    Check(!slot && g_sendCalls == 1, "PrePlacedRecordGuard: no record after Send_State takes the exit");
    g_makes = true; g_sendCalls = 0; f.mode = 2;
    slot = PrePlacedRecordGuardTestRecreate(&f, entry, &name, reinterpret_cast<const uint8_t*>(guid), map, false);
    Check(!slot && g_sendCalls == 0, "PrePlacedRecordGuard: mode 2 takes the exit without making a record");

    // The install
    Check(InstallPrePlacedRecordGuard(true, 1, 0), "PrePlacedRecordGuard: installs on the mapped exe");
    PrePlacedRecordGuardInfo s{};
    Check(PrePlacedRecordGuardState(&s) && s.site == d.site && s.stub && s.nullStub && s.mode == 1,
          "PrePlacedRecordGuard: installed state (mode 1, both stubs)");
    if (!s.stub || !s.nullStub) return;
    const uint8_t* const to = d.site + 6 + *reinterpret_cast<const int32_t*>(d.site + 2);
    const uint8_t* const toNull = d.nullSite + 6 + *reinterpret_cast<const int32_t*>(d.nullSite + 2);
    Check(d.site[0] == 0x0F && d.site[1] == 0x84 && to == s.stub && d.nullSite[0] == 0x0F && d.nullSite[1] == 0x84 &&
              toNull == s.nullStub,
          "PrePlacedRecordGuard: the GUID jz goes to the first stub, the null-value jz to the second");
    // stub: 48 83 EC 20; 4C 89 E1; 4C 89 F2; 4C 8D 45 AF; 49 89 D9; 48 B8 fn; FF D0; 48 83 C4 20; 48 85 C0; 74 11; 49 89 C7;
    // FF 25 0 recheck; FF 25 0 exit
    for (const uint8_t* st : {s.stub, s.nullStub}) {
        Check(memcmp(st, "\x48\x83\xEC\x20\x4C\x89\xE1\x4C\x89\xF2\x4C\x8D\x45\xAF\x49\x89\xD9\x48\xB8", 19) == 0 &&
                  memcmp(st + 27, "\xFF\xD0\x48\x83\xC4\x20\x48\x85\xC0\x74\x11\x49\x89\xC7\xFF\x25\x00\x00\x00\x00", 20) == 0 &&
                  *reinterpret_cast<const uint8_t* const*>(st + 47) == d.recheck &&
                  memcmp(st + 55, "\xFF\x25\x00\x00\x00\x00", 6) == 0 && *reinterpret_cast<const uint8_t* const*>(st + 61) == d.restore &&
                  *reinterpret_cast<void* const*>(st + 19) != nullptr,
              "PrePlacedRecordGuard: a stub passes entry / name / GUID / map, continues at the record checks with r15 = the "
              "slot, or jumps to the exit's restores");
    }
    Check(*reinterpret_cast<void* const*>(s.stub + 19) != *reinterpret_cast<void* const*>(s.nullStub + 19),
          "PrePlacedRecordGuard: the two stubs call their own functions (missing / null value)");
    MEMORY_BASIC_INFORMATION mi{};
    Check(VirtualQuery(s.stub, &mi, sizeof mi) && mi.Protect == PAGE_EXECUTE_READ, "PrePlacedRecordGuard: the stubs' page is read-execute");
    Check(memcmp(d.restore, "\x4C\x8B\xBC\x24\xB8\x00\x00\x00\x4C\x8B\xAC\x24\xC0\x00\x00\x00", 16) == 0,
          "PrePlacedRecordGuard: the exit restores r15 and r13 from the adoption's own saves");
    Check(memcmp(d.recheck, "\x49\x8B\x07\x48\x85\xC0\x0F\x84", 8) == 0,
          "PrePlacedRecordGuard: the record checks start with mov rax,[r15]; test rax,rax; jz");
    Check(InstallPrePlacedRecordGuard(true, 1, 0), "PrePlacedRecordGuard: a second install finds it in place");
    PrePlacedRecordGuardInfo again{};
    Check(!PrePlacedRecordGuardDiscover(game, &again), "PrePlacedRecordGuard: discovery refuses an already patched jz");
}
