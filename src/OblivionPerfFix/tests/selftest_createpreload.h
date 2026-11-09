// CreatePreload self-test (public since 1.3.0, worker port130j; lab bigcreate before), included by selftest.cpp: on the mapped exe
// the form-asset lookup (both map readers), SetPath (its call), ResolveObject, ToString, FMemory::Free, LoadPackageAsync, the
// create command's form id inside NpcCreationPacing's create Execute and Altar's create's class-path rule are found once
// (nothing installed); the class path getters' offsets read from their code; then the readers of the lookup's two maps on a
// fake map holder (hits, chained collisions, misses, empty maps, a path longer than the copy buffer) and the deferred
// command's frame count keyed by the command (rev130e C-S1: another command offered in between does not inherit it).
#pragma once

namespace cpl {

inline void CreatePreloadSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    NpcPacingInfoOut pi{};
    const uint8_t* const createExec = NpcPacingInfo(&pi) ? pi.createExec : nullptr;
    const char* missing = nullptr;
    const bool found = CreatePreloadDiscover(game, createExec, &missing);
    Check(found, "createpreload: the form-asset lookup and its maps, SetPath, ResolveObject, ToString, FMemory::Free, LoadPackageAsync, "
                 "the create command's form id, Altar's class path rule found once");
    if (!found) Log("[selftest] createpreload: %s not recognised", missing ? missing : "?");
    if (game.timeDateStamp == 0xF19077A4u) {
        Check(CreatePreloadTestGetter(game.base + 0x45E20F0) == 0x80 && CreatePreloadTestGetter(game.base + 0x45E20B0) == 0x220,
              "createpreload: the class path getters' offsets read from their code (+0x80, TESAmmo +0x220)");
        Check(CreatePreloadTestGetter(game.base + 0x47B2910) == -1, "createpreload: another function is not taken for a getter");
    }
    alignas(16) static uint8_t h[0x200];
    memset(h, 0, sizeof h);
    // first map (formID -> loaded asset): entries stride 0x18 {key, value @8, next @0x10}; hash size 4
    struct E1 { uint32_t key, pad; const void* value; int32_t next, pad2; };
    static_assert(sizeof(E1) == 0x18, "entry 1");
    static E1 e1[3];
    static int32_t b1[4];
    static int dummyA = 0, dummyB = 0, dummyC = 0;
    e1[0] = {5, 0, &dummyA, -1, 0};
    e1[1] = {9, 0, &dummyB, 0, 0};  // 9 & 3 == 5 & 3: chained 9 -> 5
    e1[2] = {6, 0, &dummyC, -1, 0};
    b1[0] = -1; b1[1] = 1; b1[2] = 2; b1[3] = -1;
    *reinterpret_cast<E1**>(h + 0x118) = e1;
    *reinterpret_cast<int32_t*>(h + 0x120) = 3;
    *reinterpret_cast<int32_t*>(h + 0x14C) = 0;
    *reinterpret_cast<int32_t**>(h + 0x158) = b1;
    *reinterpret_cast<int32_t*>(h + 0x160) = 4;
    Check(CreatePreloadTestCached(h, 5) == &dummyA && CreatePreloadTestCached(h, 9) == &dummyB && CreatePreloadTestCached(h, 6) == &dummyC,
          "createpreload: cached form assets found (chained bucket too)");
    Check(!CreatePreloadTestCached(h, 13) && !CreatePreloadTestCached(h, 4), "createpreload: absent form ids give nothing");
    // second map (formID -> asset path FString): entries stride 0x20 {key, FString @8 (data, num), next @0x18}
    struct E2 { uint32_t key, pad; const wchar_t* data; int32_t num, max; int32_t next, pad2; };
    static_assert(sizeof(E2) == 0x20, "entry 2");
    static E2 e2[2];
    static int32_t b2[2];
    static const wchar_t kPath[] = L"/Game/Forms/items/book/SKLxHandtoHand1.SKLxHandtoHand1";
    const int32_t n = static_cast<int32_t>(wcslen(kPath)) + 1;
    e2[0] = {7, 0, kPath, n, n, -1, 0};
    e2[1] = {3, 0, nullptr, 0, 0, 0, 0};  // empty string -> nothing; chained 3 -> 7 (both odd)
    b2[0] = -1; b2[1] = 1;
    *reinterpret_cast<E2**>(h + 0x28) = e2;
    *reinterpret_cast<int32_t*>(h + 0x30) = 2;
    *reinterpret_cast<int32_t*>(h + 0x5C) = 0;
    *reinterpret_cast<int32_t**>(h + 0x68) = b2;
    *reinterpret_cast<int32_t*>(h + 0x70) = 2;
    wchar_t buf[512];
    const int len = CreatePreloadTestPath(h, 7, buf, 512);
    Check(len == n - 1 && wcscmp(buf, kPath) == 0, "createpreload: a form's asset path string copied from behind a chained entry");
    Check(!CreatePreloadTestPath(h, 3, buf, 512) && !CreatePreloadTestPath(h, 5, buf, 512), "createpreload: empty path / absent id give nothing");
    Check(!CreatePreloadTestPath(h, 7, buf, 16), "createpreload: a path longer than the copy buffer gives nothing (no prediction)");
    *reinterpret_cast<int32_t*>(h + 0x5C) = 2;  // num == free count: empty map
    Check(!CreatePreloadTestPath(h, 7, buf, 512), "createpreload: an empty map gives nothing");

    // the deferred command's frames waited, keyed by the command
    int a = 0, b = 0;
    CreatePreloadTestHeads(nullptr, 0, false);
    const int w0 = CreatePreloadTestHeads(&a, 100, true);  // A deferred (0 before)
    const int w1 = CreatePreloadTestHeads(&b, 100, true);  // another session's B offered in between: its own count
    const int w2 = CreatePreloadTestHeads(&a, 101, true);  // A again: 1
    const int w3 = CreatePreloadTestHeads(&a, 104, false); // A again after pacing's budget deferrals: 2, then runs
    const int w4 = CreatePreloadTestHeads(&a, 105, false); // a new command at A's address: 0
    const int w5 = CreatePreloadTestHeads(&b, 300, false); // B offered again long after: stale, 0
    Check(w0 == 0 && w1 == 0 && w2 == 1 && w3 == 2 && w4 == 0 && w5 == 0,
          "createpreload: frames waited are kept per command (another create in between does not inherit them; stale entries reset)");
    CreatePreloadTestHeads(nullptr, 0, false);
    static int many[9];
    bool all8 = true;
    for (int i = 0; i < 8; ++i) all8 = all8 && CreatePreloadTestHeads(&many[i], 200, true) == 0;
    const int ninth = CreatePreloadTestHeads(&many[8], 200, true);   // no free entry: not deferred
    const int first = CreatePreloadTestHeads(&many[0], 201, true);   // its count survived the ninth
    Check(all8 && ninth == -1 && first == 1,
          "createpreload: with every head entry in use a new create is not deferred and no other command's count is lost (CP-N1)");
    CreatePreloadTestHeads(nullptr, 0, false);
}

}  // namespace cpl
