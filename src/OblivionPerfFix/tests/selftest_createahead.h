// CreateAhead self-test (aheadport 2026-11-16), included by selftest.cpp: on the mapped exe the spawner's SpawnActor /
// FinishSpawning call pair, FinishSpawning's head, K2_DestroyActor and its exec thunk (the vtable slot), the attach's cell walk,
// Spawn_Paired_Actor's reads and OnActorSpawned are found once (nothing installed); then, with fakes for SpawnActor, the class
// lookup, the object array, the clock, the menu freeze and the player: the parameter compare, the scale rule, the cell walk,
// one build per frame only after slack and while pacing holds nothing, adoption (exact class / parameters / scale only), spares
// per base form, the caps, and every drop rule (timeout, menu / loading, fast travel, save, key off, an actor the engine took).
// The fake SpawnActor and destroy call the redirected broadcast hook as UWorld::SpawnActor / DestroyActor do: a build's two
// broadcasts are held back and made once at the adoption, a destroy of ours makes none, a stock spawn passes them on; the
// adopted actor's CreationTime is the world's time at the adoption; a build's fault keeps the key off.
#pragma once

namespace cah {

struct FakeObj {            // enough of a UObject / AActor for the fix's reads
    void** vt;              // +0x00
    int32_t flags, index;   // +0x08, +0x0C
    void* cls;              // +0x10
    float creation;         // +0x18 (the fake CreationTime)
    uint32_t pad0;
    double time;            // +0x20 (the fake world's TimeSeconds)
    uint8_t pad[0x5C - 0x28];
    uint8_t finished, destroying;  // +0x5C, +0x5D
    uint8_t pad2[2];
    uint8_t pad3[0x1A0 - 0x60];
    struct FakeRoot* root;  // +0x1A0 (AActor::RootComponent)
};
struct FakeRoot {
    uint8_t pad[0x280];
    double loc[3];  // +0x280 (the ComponentToWorld translation)
};
static_assert(offsetof(FakeObj, index) == 0x0C && offsetof(FakeObj, cls) == 0x10 && offsetof(FakeObj, finished) == 0x5C &&
                  offsetof(FakeObj, creation) == 0x18 && offsetof(FakeObj, time) == 0x20 && offsetof(FakeObj, root) == 0x1A0 &&
                  offsetof(FakeRoot, loc) == 0x280,
              "fake object");
struct FakeItem { void* obj; int32_t flags, cluster, serial, pad; };
static_assert(sizeof(FakeItem) == 24, "fake item");

inline FakeItem items[256];
inline FakeItem* chunkTable[1] = {items};
inline uint8_t arrayMem[0x40];
inline FakeObj objs[256];
inline FakeRoot roots[256];
inline int nObjs = 0, spawns = 0, destroys = 0, nextSerial = 100;
inline void* vtable[0x80];
inline double clockS = 1000.0;
inline int freeze = 0;
inline double player[3] = {1000.0, 2000.0, 300.0};
inline int clsA = 0, clsB = 0, clsC = 0, clsD = 0;
inline int gcClass = 0;    // 0xB1's class: 0 clsA, 1 collected (the lookup finds nothing), 2 loaded again as clsD
inline int purgeFlag = 0;  // != 0: a GC purge is pending
inline double lastScale = 0;
inline int broadcasts = 0;
inline uint8_t* broadcastActor = nullptr;
inline uintptr_t broadcastOrder = 0;  // the delegates' addresses in call order, two hex digits each
inline double announced[3] = {};       // the actor's root translation when the listeners are told (aheadindex)
inline void FakeBroadcast(void* delegate, uint8_t* actor)
{
    ++broadcasts;
    broadcastActor = actor;
    if (const FakeRoot* const r = reinterpret_cast<FakeObj*>(actor)->root) memcpy(announced, r->loc, sizeof announced);
    broadcastOrder = broadcastOrder * 16 + (reinterpret_cast<uintptr_t>(delegate) & 0xF);
}

inline uint8_t* fakeWorld = nullptr;
inline void FakeDestroy(uint8_t* a)
{
    FakeObj* const o = reinterpret_cast<FakeObj*>(a);
    ++destroys;
    opf::CreateAheadTestBroadcast(fakeWorld + 3, a);  // OnActorDestroyed
    opf::CreateAheadTestBroadcast(fakeWorld + 4, a);  // OnActorRemovedFromWorld
    o->destroying |= 2;
    items[o->index].obj = nullptr;  // as after the purge
    items[o->index].serial = 0;
}
inline FakeObj* NewObj(void* cls)
{
    FakeObj* const o = &objs[nObjs];
    memset(o, 0, sizeof *o);
    o->vt = vtable;
    o->index = nObjs;
    o->cls = cls;
    items[nObjs] = {o, 0, 0, nextSerial++, 0};
    ++nObjs;
    return o;
}
inline uint8_t* FakeSpawn(uint8_t* world, uint8_t* cls, const uint8_t* xf, const uint8_t*)
{
    ++spawns;
    lastScale = reinterpret_cast<const double*>(xf)[8];
    uint8_t* const a = reinterpret_cast<uint8_t*>(NewObj(cls));
    reinterpret_cast<FakeObj*>(a)->creation = 777.0f;   // PostSpawnInitialize: the world's time at the build
    FakeRoot* const root = &roots[reinterpret_cast<FakeObj*>(a)->index];  // PostSpawnInitialize: the root at the spawn transform
    memcpy(root->loc, xf + 4 * 8, sizeof root->loc);
    reinterpret_cast<FakeObj*>(a)->root = root;
    opf::CreateAheadTestBroadcast(world + 1, a);  // OnActorPreSpawnInitialization
    opf::CreateAheadTestBroadcast(world + 2, a);  // OnActorSpawned
    return a;
}
inline void* FakeClassOf(uint32_t base, bool) { return base == 0xB1 ? (gcClass == 0 ? static_cast<void*>(&clsA) : gcClass == 1 ? nullptr : &clsD) : base == 0xB2 ? &clsB : base >= 0xC0 && base < 0xD0 ? &clsC : nullptr; }

// old-engine fakes: a reference (vtable with the scale virtual at +0x200) and a cell with its list
struct FakeRef {
    void** vt;
    uint8_t type; uint8_t pad[7];   // +0x08
    uint32_t id; uint8_t pad2[0x34];  // +0x10
    void* paired;                   // +0x48
    void* base;                     // +0x50
    uint8_t pad3[0x0C];
    float pos[3];                   // +0x64
    float scale;                    // read by the fake virtual
};
static_assert(offsetof(FakeRef, paired) == 0x48 && offsetof(FakeRef, base) == 0x50 && offsetof(FakeRef, pos) == 0x64, "fake reference");
struct FakeForm { void* vt; uint64_t pad; uint32_t id; };
static_assert(offsetof(FakeForm, id) == 0x10, "fake form");
struct FakeNode { FakeRef* ref; FakeNode* next; };
inline void* refVtable[0x80];
inline float FakeScale(const uint8_t* ref) { return reinterpret_cast<const FakeRef*>(ref)->scale; }

inline void Stock(uint8_t* world, uint32_t ref, uint32_t base, void* cls, double scale, const uint8_t* params, uint8_t** out, bool* took)
{
    alignas(16) double xf[12] = {0, 0, 0, 1, 10, 20, 30, 0, scale, scale, scale, 0};
    opf::CreateAheadCreateBegin(ref, base);
    *out = opf::CreateAheadTestSpawnCall(world, static_cast<uint8_t*>(cls), reinterpret_cast<const uint8_t*>(xf), params);
    *took = opf::CreateAheadCreateEnd();
}

inline void CreateAheadSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    CreateAheadSites f{};
    const char* missing = nullptr;
    const bool found = CreateAheadDiscover(game, &f, &missing);
    Check(found, "createahead: the spawner's SpawnActor / FinishSpawning calls, FinishSpawning's head, K2_DestroyActor and its exec thunk, "
                 "the attach's cell walk and Spawn_Paired_Actor's reads found once");
    if (!found) Log("[selftest] createahead: %s not recognised", missing ? missing : "?");
    if (found && game.timeDateStamp == 0xF19077A4u) {
        Check(f.spawnCall == game.base + 0x47A413E && f.spawnFn == game.base + 0x332D0F0 && f.finishFn == game.base + 0x2DBA830 &&
                  f.destroyFn == game.base + 0x2DC21D0 && f.destroyThunk == game.base + 0x2DD4FE0 && f.walk == game.base + 0x66606BF,
              "createahead: the sites are build 19115871's (SpawnActor call, UWorld::SpawnActor, FinishSpawning, K2_DestroyActor, thunk, walk)");
        Check(f.destroySlot == 0x370 && f.spawnedDelegate == 0x398 && f.preSpawnDelegate == 0x3B0,
              "createahead: K2_DestroyActor's vtable slot +0x370, UWorld::OnActorSpawned +0x398, OnActorPreSpawnInitialization +0x3B0");
        Check(f.preSpawnCall == game.base + 0x332E032 && f.spawnedCall == game.base + 0x332E0AB && f.broadcastFn == game.base + 0xDF1F50,
              "createahead: UWorld::SpawnActor's two broadcast calls and their one target are build 19115871's");
        Check(f.destroyedCall == game.base + 0x331DA38 && f.removedCall == game.base + 0x331DCCE && f.destroyedDelegate == 0x3C8 &&
                  f.removedDelegate == 0x410,
              "createahead: UWorld::DestroyActor's two broadcast calls (OnActorDestroyed +0x3C8, OnActorRemovedFromWorld +0x410) are build "
              "19115871's");
        Check(f.creationTimeOff == 0x158 && f.worldTimeOff == 0x6C0,
              "createahead: AActor::CreationTime +0x158 and UWorld::TimeSeconds +0x6C0 (PostSpawnInitialize's store) are build 19115871's");
        Check(f.pawnBox == game.base + 0x4506D7F && f.rootOff == 0x1A0 && f.locOff == 0x280,
              "createahead (aheadindex): the pawn index's read of a pawn's place (exe+0x4506D7F: root +0x1A0, ComponentToWorld "
              "translation +0x280) is build 19115871's");
    }
    {  // the GC purge query on the mapped exe: GcPassAfterBusy's discovery result, not a fake left by another test (review rev-aheadgc SF1)
        uint8_t* const q = FindIncrementalPurgePending(game);
        uint32_t r[7] = {}, off[3] = {};
        const bool disc = GcPassAfterBusyTestDiscover(game, r, off);
        Check(q != nullptr && disc && q == game.base + r[2],
              "createahead: UE's IsIncrementalPurgePending found on the mapped exe (the GC purge query; crash1118)");
        if (game.timeDateStamp == 0xF19077A4u)
            Check(q == game.base + 0x102F9D0, "createahead: IsIncrementalPurgePending is build 19115871's (exe+0x102F9D0)");
    }
    if (found)
        Check(f.destroyedDelegate == f.spawnedDelegate + 0x30 && f.removedDelegate == f.spawnedDelegate + 0x78 && f.creationTimeOff &&
                  f.worldTimeOff,
              "createahead: DestroyActor's delegates follow OnActorSpawned as in UE 5.3's UWorld; the CreationTime store is read");

    // the parameter compare
    alignas(16) uint8_t tmpl[0x80] = {}, p[0x80] = {};
    tmpl[0x30] = 1; tmpl[0x31] = 1; tmpl[0x32] = 0x04; tmpl[0x33] = 0; tmpl[0x34] = 0x08;
    memcpy(p, tmpl, sizeof p);
    bool ok = CreateAheadTestParams(tmpl, p) == 0;
    p[0x32] = 0xE4;  // the padding bits above the five flags are not compared
    ok = ok && CreateAheadTestParams(tmpl, p) == 0;
    static const int kFields[] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x31, 0x33, 0x34, 0x38, 0x40};
    for (int o : kFields) {
        memcpy(p, tmpl, sizeof p);
        p[o] ^= 2;
        ok = ok && CreateAheadTestParams(tmpl, p) != 0;
    }
    for (int bit = 0; bit < 5; ++bit) {
        memcpy(p, tmpl, sizeof p);
        p[0x32] ^= static_cast<uint8_t>(1 << bit);
        ok = ok && CreateAheadTestParams(tmpl, p) == 0x33;
    }
    Check(ok, "createahead: parameters compare field by field (name, template, owner, instigator, level, parent, collision, scale "
              "method, the five flags, name mode, object flags, no pre-spawn function: its callable and its storage)");
    memcpy(p, tmpl, sizeof p);
    p[0x10] = 1;  // an owner
    alignas(16) uint8_t notDeferred[0x80] = {};
    notDeferred[0x30] = 1;
    Check(CreateAheadTestParams(p, p) == -1 && CreateAheadTestParams(notDeferred, notDeferred) == -1,
          "createahead: a parameter set with an owner, or not deferred, is never copied");

    // the scale rule
    const double s103 = static_cast<double>(1.03f), s09 = static_cast<double>(0.9f);
    const double exact1[3] = {1.0, 1.0, 1.0}, exact103[3] = {s103, s103, s103}, ulp[3] = {s103, std::nextafter(s103, 2.0), s103},
                 nonUni[3] = {1.0, 1.0, 1.03}, r09[3] = {s09, s09, s09};
    Check(CreateAheadTestScale(exact1, 1.0) && CreateAheadTestScale(exact103, s103), "createahead: a requested scale equal to the build scale is adoptable");
    Check(!CreateAheadTestScale(ulp, s103) && !CreateAheadTestScale(nonUni, 1.0) && !CreateAheadTestScale(exact1, s103),
          "createahead: one unit in the last place off, or a non-uniform scale, is not");
    Check(s09 * (1.0 / s09) != 1.0 && !CreateAheadTestScale(r09, s09),
          "createahead: a scale whose reciprocal does not multiply back to exactly 1 is not adopted (UE's re-place would not be exact)");

    // the flow with fakes
    memset(arrayMem, 0, sizeof arrayMem);
    void* const chunks = chunkTable;
    memcpy(arrayMem + 0x10, &chunks, 8);
    const int32_t num = 256;
    memcpy(arrayMem + 0x24, &num, 4);
    for (void*& v : vtable) v = nullptr;
    vtable[5] = reinterpret_cast<void*>(&FakeDestroy);
    for (void*& v : refVtable) v = nullptr;
    refVtable[0x200 / 8] = reinterpret_cast<void*>(&FakeScale);
    nObjs = spawns = destroys = 0;
    clockS = 1000.0;
    freeze = 0;
    broadcasts = 0;
    broadcastOrder = 0;
    gcClass = purgeFlag = 0;
    const CreateAheadFakes fakes{&FakeSpawn, &FakeClassOf, arrayMem, 5, &clockS, &freeze, player, &FakeBroadcast, 0x18, 0x20, &purgeFlag, 0x1A0, 0x280};
    CreateAheadTestSetup(&fakes);
    uint8_t* const world = reinterpret_cast<uint8_t*>(NewObj(nullptr));
    fakeWorld = world;
    reinterpret_cast<FakeObj*>(world)->time = 1234.5;
    uint8_t* got = nullptr;
    bool took = false;

    // nothing is built before a stock character create gave the parameters and the world
    static FakeForm formB1{nullptr, 0, 0xB1}, formItem{nullptr, 0, 0x77};
    static FakeRef r1{}, r2{}, r3{}, r4{}, r5{};
    r1 = {}; r2 = {}; r3 = {}; r4 = {}; r5 = {};
    r1.vt = r2.vt = r3.vt = r4.vt = r5.vt = refVtable;
    r1.type = 0x32; r1.id = 0x1001; r1.base = &formB1; r1.pos[0] = 4096; r1.pos[1] = -8192; r1.pos[2] = 100; r1.scale = 1.0f;
    r2.type = 0x33; r2.id = 0x1002; r2.base = &formB1; r2.scale = 1.03f;
    r3.type = 0x20; r3.id = 0x1003; r3.base = &formItem; r3.scale = 1.0f;          // an item: never noted
    r4.type = 0x32; r4.id = 0x1004; r4.base = &formB1; r4.scale = 1.0f; r4.paired = &r4;  // has its actor already
    r5.type = 0x32; r5.id = 0xFF000009; r5.base = &formB1; r5.scale = 1.0f;        // a run-time reference: spares only
    static FakeNode n5{&r5, nullptr}, n4{&r4, &n5}, n3{&r3, &n4}, n2{&r2, &n3};
    alignas(8) static uint8_t cell[0x100];
    memset(cell, 0, sizeof cell);
    FakeNode head{&r1, &n2};
    memcpy(cell + 0xB8, &head, sizeof head);
    CreateAheadCell(cell);
    CreateAheadFrame(8000, false, false);
    auto c = CreateAheadTestRead();
    Check(c.queued == 2 && c.built == 0 && spawns == 0, "createahead: the cell walk notes character references without an actor only; "
                                                          "nothing is built before a stock create gave the spawner's parameters");
    Stock(world, 0x900, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    Check(spawns == 1 && !took && got, "createahead: a create with nothing held runs the game's SpawnActor");
    Check(broadcasts == 2 && broadcastActor == got && CreateAheadTestRead().heldBack == 0,
          "createahead: a stock SpawnActor's broadcasts pass the hook unchanged");

    // one build per frame, only after slack, only while pacing holds nothing
    CreateAheadFrame(1000, false, false);
    const int b0 = CreateAheadTestRead().built;
    CreateAheadFrame(8000, true, false);
    const int b1 = CreateAheadTestRead().built;
    CreateAheadFrame(8000, false, false);
    const int b2 = CreateAheadTestRead().built;
    const double sc1 = lastScale;
    CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(b0 == 0 && b1 == 0 && b2 == 1 && c.built == 2 && c.held == 2 && spawns == 3,
          "createahead: no build after a frame without slack or while pacing holds a unit; then one per frame");
    Check(sc1 == 1.0 && lastScale == s103, "createahead: each is built at the scale its reference gives (the create command's own)");
    Check(broadcasts == 2 && c.heldBack == 4, "createahead: a build's two SpawnActor broadcasts are held back (nobody is told of it)");
    broadcasts = 0;
    broadcastOrder = 0;

    // adoption: exact class, parameters and scale only
    const int sp0 = spawns;
    Stock(world, 0x1001, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    Check(took && spawns == sp0 && got == reinterpret_cast<uint8_t*>(&objs[2]) && destroys == 0,
          "createahead: the create of a reference with a held actor takes it (no SpawnActor)");
    Check(broadcasts == 2 && broadcastActor == got &&
              broadcastOrder == (((reinterpret_cast<uintptr_t>(world) + 1) & 0xF) * 16 + ((reinterpret_cast<uintptr_t>(world) + 2) & 0xF)),
          "createahead: at the adoption the two spawn broadcasts held back at the build are made for the actor, pre-spawn first; none before");
    Check(reinterpret_cast<FakeObj*>(got)->creation == 1234.5f,
          "createahead: the adopted actor's CreationTime is the world's time at the adoption (its age as stock), not the build's");
    {
        constexpr double k = 100.0 / 70.0;
        const double* const at = reinterpret_cast<FakeObj*>(got)->root->loc;
        Check(announced[0] == 10.0 && announced[1] == 20.0 && announced[2] == 30.0,
              "createahead (aheadindex): at the adoption's broadcasts the actor stands at the requested translation, as the stock "
              "SpawnActor's would (the pawn index files it there)");
        Check(at[0] == 4096.0 * k && at[1] == 8192.0 * k && at[2] == 100.0 * k - 50000.0,
              "createahead (aheadindex): after them its root is back at the park translation (FinishSpawning's re-place reads it)");
    }
    const double offScale = std::nextafter(s103, 2.0);
    Stock(world, 0x1002, 0xB1, &clsA, offScale, tmpl, &got, &took);
    c = CreateAheadTestRead();
    Check(!took && spawns == sp0 + 1 && destroys == 1 && c.rejScale == 1 && c.held == 0,
          "createahead: a requested scale one unit in the last place off: the held actor is destroyed, the game's SpawnActor runs");

    // spares for run-time references: up to 3 per base form in use, built one per frame, adopted by class + scale
    Stock(world, 0xFF000001, 0xB2, &clsB, 1.0, tmpl, &got, &took);
    for (int i = 0; i < 6; ++i) CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    const int sp1 = spawns;
    Check(c.held == 3 && c.built == 5, "createahead: three spares for a base form the game created, no more");
    Stock(world, 0xFF000002, 0xB2, &clsB, 1.0, tmpl, &got, &took);
    c = CreateAheadTestRead();
    Check(took && spawns == sp1 && c.spareAdopted == 1 && c.held == 2, "createahead: a run-time reference of that base form takes a spare");
    Stock(world, 0xFF000003, 0xB2, &clsA, 1.0, tmpl, &got, &took);  // another class asked for the same base form
    Check(!took, "createahead: a spare of another class is not taken");
    // the cap: five more base forms in use would want 15 spares
    for (uint32_t b = 0xC0; b < 0xC5; ++b) Stock(world, 0xFF000010 + b, b, &clsC, 1.0, tmpl, &got, &took);
    for (int i = 0; i < 30; ++i) CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(c.held == 8, "createahead: spares are only built while fewer than 8 actors are held (room for references; 12 at most)");

    // a menu / loading screen: everything held is destroyed with the game's own destroy
    const int d0 = destroys;
    freeze = 1;
    CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(c.held == 0 && destroys == d0 + 8, "createahead: a menu or loading screen destroys every held actor");
    freeze = 0;
    CreateAheadFrame(8000, false, false);
    Check(CreateAheadTestRead().built == c.built, "createahead: after it nothing is built until the next stock create gave the world again");

    // an actor the engine took away is forgotten, never touched; a finished one is not ours
    Stock(world, 0x901, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    static FakeRef r6{}, r7{}, r8{};
    r6 = {}; r7 = {}; r8 = {};
    r6.vt = r7.vt = r8.vt = refVtable;
    r6.type = r7.type = r8.type = 0x32;
    r6.id = 0x1006; r7.id = 0x1007; r8.id = 0x1008;
    r6.base = r7.base = r8.base = &formB1;
    r6.scale = r7.scale = r8.scale = 1.0f;
    static FakeNode m8{&r8, nullptr}, m7{&r7, &m8};
    FakeNode head2{&r6, &m7};
    memcpy(cell + 0xB8, &head2, sizeof head2);
    CreateAheadCell(cell);
    // (the spares of base forms still in use come back too: let every build happen)
    for (int i = 0; i < 20; ++i) CreateAheadFrame(8000, false, false);
    int i6 = -1, i7 = -1;
    {
        // the three references were built first (oldest noted first): the first three spawns after the stock create
        const int first = nObjs - CreateAheadTestRead().held;
        i6 = first; i7 = first + 1;
    }
    const int d1 = destroys;
    items[i6].obj = nullptr;                 // purged by the engine
    items[i7].serial += 1;                   // its slot reused by another object
    const int sp2 = spawns;
    Stock(world, 0x1006, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    const bool t6 = took;
    Stock(world, 0x1007, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    const bool t7 = took;
    objs[i7 + 1].finished |= 1;              // somebody finished the third
    Stock(world, 0x1008, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    c = CreateAheadTestRead();
    Check(!t6 && !t7 && !took && spawns == sp2 + 3 && destroys == d1 && c.gone == 3,
          "createahead: a held actor whose slot is empty, reused or that somebody finished is not taken and not destroyed (weak handle)");

    // no create came: destroyed after the hold time and not noted again at once
    static FakeRef r9{};
    r9 = {};
    r9.vt = refVtable; r9.type = 0x32; r9.id = 0x1009; r9.base = &formB1; r9.scale = 1.0f;
    FakeNode head3{&r9, nullptr};
    memcpy(cell + 0xB8, &head3, sizeof head3);
    // make room: the spares' base forms are forgotten after 90 s
    clockS += 91.0;
    const int hs0 = CreateAheadTestRead().held, ds0 = destroys;
    CreateAheadFrame(1000, false, false);  // no slack: no timeout destroy
    const int ds1 = destroys;
    CreateAheadFrame(8000, true, false);   // pacing busy: none either
    const int ds2 = destroys;
    CreateAheadFrame(8000, false, false);  // slack: exactly one
    const int ds3 = destroys;
    for (int i = 0; i < 16; ++i) CreateAheadFrame(8000, false, false);
    Check(hs0 > 1 && ds1 == ds0 && ds2 == ds0 && ds3 == ds0 + 1 && destroys == ds0 + hs0,
          "createahead: timed-out actors are destroyed one per frame, only after a frame with slack while pacing holds nothing");
    Stock(world, 0x902, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    Check(CreateAheadTestRead().held == 0, "createahead: spares of a base form not created for 90 s are destroyed");
    CreateAheadCell(cell);
    CreateAheadFrame(8000, false, false);
    const int h1 = CreateAheadTestRead().held;
    clockS += 21.0;
    const int d2 = destroys;
    CreateAheadFrame(8000, false, false);
    const int q1 = CreateAheadTestRead().queued;
    CreateAheadCell(cell);
    CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(h1 == 1 && destroys == d2 + 1 && c.held == 0 && c.queued == q1,
          "createahead: an actor whose create has not come after 20 s is destroyed; its reference is not noted again for two minutes");

    // a save, a pending fast travel, the key off
    static FakeRef ra{}, rb{}, rc{};
    ra = {}; rb = {}; rc = {};
    ra.vt = rb.vt = rc.vt = refVtable;
    ra.type = rb.type = rc.type = 0x32;
    ra.id = 0x100A; rb.id = 0x100B; rc.id = 0x100C;
    ra.base = rb.base = rc.base = &formB1;
    ra.scale = rb.scale = rc.scale = 1.0f;
    const auto one = [&](FakeRef* r) {
        FakeNode h{r, nullptr};
        memcpy(cell + 0xB8, &h, sizeof h);
        CreateAheadCell(cell);
        CreateAheadFrame(8000, false, false);
        return CreateAheadTestRead().held;
    };
    const int ha = one(&ra);
    const int d3 = destroys;
    CreateAheadDropAll(0);
    const bool saveOk = ha == 1 && destroys == d3 + 1 && CreateAheadTestRead().held == 0;
    const int hb = one(&rb);
    CreateAheadFrame(8000, false, true);
    const bool travelOk = hb == 1 && destroys == d3 + 2 && CreateAheadTestRead().held == 0;
    Stock(world, 0x903, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    const int hc = one(&rc);
    SetCreateAhead(0, 4);
    CreateAheadFrame(8000, false, false);
    const bool offOk = hc == 1 && destroys == d3 + 3 && CreateAheadTestRead().held == 0;
    Check(saveOk && travelOk && offOk, "createahead: a save, a pending fast travel and switching the key off each destroy every held actor");
    c = CreateAheadTestRead();
    Check(c.destroyQuiet == 2 * destroys && destroys > 0,
          "createahead: destroying an actor built ahead and never adopted makes neither OnActorDestroyed nor OnActorRemovedFromWorld");
    SetCreateAhead(1, 4);
    const bool onAgain = CreateAheadTestRead().mode == 1;
    CreateAheadTestFault();
    SetCreateAhead(1, 4);
    Check(onAgain && CreateAheadTestRead().mode == 0, "createahead: after a build's fault the ini's re-read cannot switch the key back on");

    // CreateAheadPool (aheadwide): a larger pool, and idle spares of other base forms make room for a base form in use
    CreateAheadTestSetup(&fakes);
    SetCreateAheadPool(24);
    clockS = 5000.0;
    for (uint32_t b = 0xC0; b < 0xC7; ++b) Stock(world, 0xFF000100 + b, b, &clsC, 1.0, tmpl, &got, &took);  // 7 forms want 21
    for (int i = 0; i < 40; ++i) CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(c.held == 20 && c.idleEvicted == 0, "createahead pool 24: spares are built while fewer than 20 actors are held");
    clockS += 10.0;
    Stock(world, 0xFF000200, 0xC8, &clsC, 1.0, tmpl, &got, &took);  // a new form; the others idle 10 s: nothing makes room
    for (int i = 0; i < 10; ++i) CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    const bool notYet = c.held == 20 && c.idleEvicted == 0;
    clockS += 15.0;                                                  // the first 7 forms idle 25 s, 0xC8 15 s
    Stock(world, 0xFF000201, 0xC7, &clsC, 1.0, tmpl, &got, &took);   // another new form
    const int dE0 = destroys, sE0 = spawns;
    CreateAheadFrame(1000, false, false);                            // no slack: nothing
    CreateAheadFrame(8000, true, false);                             // pacing busy: nothing
    const bool quiet = destroys == dE0 && spawns == sE0;
    CreateAheadFrame(8000, false, false);                            // slack: one destroy, no build in the same frame
    const bool oneWork = destroys == dE0 + 1 && spawns == sE0;
    CreateAheadFrame(8000, false, false);                            // the next slack frame: the spare for 0xC7
    const bool thenBuild = destroys == dE0 + 1 && spawns == sE0 + 1;
    for (int i = 0; i < 30; ++i) CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(notYet && quiet && oneWork && thenBuild && c.held == 20 && c.idleEvicted == 6,
          "createahead pool 24: a spare of a form idle 20 s (and longer than the one that wants room) makes room, one destroy per slack "
          "frame, the build in a later frame; forms idle less than 20 s keep theirs");
    // 0xC7 and 0xC8 now hold three each; the 0xC7 spares go to 0xFF references of 0xC7
    Stock(world, 0xFF000202, 0xC7, &clsC, 1.0, tmpl, &got, &took);
    Check(took, "createahead pool 24: a run-time reference takes the spare built after the eviction");
    // a base form whose class is not resident: its spare cannot be built, so nothing is destroyed for it (review rev-pool SF3)
    for (int i = 0; i < 5; ++i) CreateAheadFrame(8000, false, false);  // the adopted spare's form gets its spare back
    const int dS0 = destroys, eS0 = CreateAheadTestRead().idleEvicted, hS0 = CreateAheadTestRead().held;
    Stock(world, 0xFF000204, 0xD0, &clsC, 1.0, tmpl, &got, &took);
    for (int i = 0; i < 10; ++i) CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(hS0 == 20 && destroys == dS0 && c.idleEvicted == eS0 && c.held == 20,
          "createahead pool 24: no spare is destroyed for a base form whose class is not resident");
    // back to 0 (live): the pool shrinks to 12 one spare per slack frame, no idle eviction any more
    SetCreateAheadPool(0);
    for (int i = 0; i < 20; ++i) CreateAheadFrame(8000, false, false);
    const int e0 = CreateAheadTestRead().idleEvicted;
    clockS += 30.0;
    Stock(world, 0xFF000203, 0xC9, &clsC, 1.0, tmpl, &got, &took);
    for (int i = 0; i < 10; ++i) CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(c.held == 12 && c.idleEvicted == e0, "createahead pool 0 (live): back to 12 held, idle spares are not evicted");
    SetCreateAheadPool(0);

    // CreateAheadLevelled (rollahead): the game's levelled spawns already made (run-time references, ids FF......) found in a
    // cell ahead are built as references and taken by their own create; another base form, or a create before the build,
    // lets the entry go and the spares apply as before; with 0 they are not noted
    CreateAheadTestSetup(&fakes);
    clockS = 9000.0;
    Stock(world, 0x910, 0xB1, &clsA, 1.0, tmpl, &got, &took);  // a stock create gives the world and the parameters
    static FakeForm formC1{nullptr, 0, 0xC1};
    static FakeRef lv1{}, lv2{}, lv3{}, lv4{};
    lv1 = {}; lv2 = {}; lv3 = {}; lv4 = {};
    lv1.vt = lv2.vt = lv3.vt = lv4.vt = refVtable;
    lv1.type = lv3.type = lv4.type = 0x32; lv2.type = 0x33;
    lv1.id = 0xFF000301; lv2.id = 0xFF000302; lv3.id = 0xFF000303; lv4.id = 0xFF000304;
    lv1.base = lv3.base = lv4.base = &formB1; lv2.base = &formC1;
    lv1.scale = lv2.scale = lv3.scale = lv4.scale = 1.0f;
    static FakeNode lk3{&lv3, nullptr}, lk2{&lv2, &lk3};
    FakeNode headL{&lv1, &lk2};
    memcpy(cell + 0xB8, &headL, sizeof headL);
    CreateAheadCell(cell);
    CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    const bool offNone = c.queued == 0 && c.runtimeNoted == 0 && c.held == 0;
    SetCreateAheadLevelled(1);
    CreateAheadCell(cell);
    for (int i = 0; i < 4; ++i) CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(offNone && c.queued == 3 && c.runtimeNoted == 3 && c.built == 3 && c.held == 3,
          "createahead levelled: with 0 a cell's run-time references are not noted; with 1 they are noted and built one per frame");
    const int spL0 = spawns;
    Stock(world, 0xFF000301, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    c = CreateAheadTestRead();
    Check(took && spawns == spL0 && c.runtimeTook == 1 && c.spareAdopted == 0 && c.held == 2,
          "createahead levelled: a run-time reference's create takes the actor built for that very reference");
    const int dL0 = destroys;
    Stock(world, 0xFF000302, 0xC2, &clsC, 1.0, tmpl, &got, &took);  // the same id and class, another base form
    c = CreateAheadTestRead();
    Check(!took && spawns == spL0 + 1 && destroys == dL0 + 1 && c.rejBase == 1 && c.held == 1,
          "createahead levelled: a create of another base form under that id does not take it (destroyed, the game's SpawnActor runs)");
    // spares of 0xC2 and 0xB1 (run-time references of both were created): three each in the next six slack frames
    for (int i = 0; i < 6; ++i) CreateAheadFrame(8000, false, false);
    const int spareHeld = CreateAheadTestRead().held;
    FakeNode headL2{&lv4, nullptr};
    memcpy(cell + 0xB8, &headL2, sizeof headL2);
    CreateAheadCell(cell);
    CreateAheadFrame(1000, false, false);  // noted, no slack: not built
    const int spL1 = spawns;
    Stock(world, 0xFF000304, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    c = CreateAheadTestRead();
    Check(spareHeld == 7 && took && spawns == spL1 && c.runtimeLate == 1 && c.spareAdopted == 1 && c.runtimeTook == 1,
          "createahead levelled: a create before its reference's build lets the entry go and takes a spare of its base form as before");
    SetCreateAheadLevelled(0);
    const int nL = CreateAheadTestRead().runtimeNoted;
    CreateAheadCell(cell);
    CreateAheadFrame(8000, false, false);
    Check(CreateAheadTestRead().runtimeNoted == nL, "createahead levelled 0 (live): run-time references are no longer noted");

    // CreateAheadLevelled review fixes (rev-levelled S1 S2 S3 N6): each case from a fresh state, three spares of 0xB1 held
    static FakeForm fC3{nullptr, 0, 0xC3}, fC5{nullptr, 0, 0xC5}, fC6{nullptr, 0, 0xC6}, fC7{nullptr, 0, 0xC7}, fC8{nullptr, 0, 0xC8},
        fC9{nullptr, 0, 0xC9}, fCB{nullptr, 0, 0xCB}, fCC{nullptr, 0, 0xCC};
    static FakeRef rf{};
    const auto fresh = [&] {
        CreateAheadTestSetup(&fakes);
        clockS = 9500.0;
        Stock(world, 0x920, 0xB1, &clsA, 1.0, tmpl, &got, &took);  // the world, the parameters, 0xB1 in use
        SetCreateAheadLevelled(1);
        for (int i = 0; i < 6; ++i) CreateAheadFrame(8000, false, false);  // its three spares
    };
    const auto note = [&](uint32_t id, FakeForm& form, float scale) {
        rf = {};
        rf.vt = refVtable;
        rf.type = 0x32;
        rf.id = id;
        rf.base = &form;
        rf.scale = scale;
        FakeNode h{&rf, nullptr};
        memcpy(cell + 0xB8, &h, sizeof h);
        CreateAheadCell(cell);
    };
    // S2: a held run-time entry that fails the scale check does not block the spare of the create's scale
    fresh();
    Stock(world, 0xFF000500, 0xC3, &clsC, 1.25, tmpl, &got, &took);  // 0xC3 in use at scale 1.25: its spares are built at 1.25
    for (int i = 0; i < 6; ++i) CreateAheadFrame(8000, false, false);
    note(0xFF000401, fC3, 1.0f);                                      // the reference as the walk saw it: 1.0
    for (int i = 0; i < 3; ++i) CreateAheadFrame(8000, false, false);
    const int sS2 = spawns, dS2 = destroys, sa0 = CreateAheadTestRead().spareAdopted;
    Stock(world, 0xFF000401, 0xC3, &clsC, 1.25, tmpl, &got, &took);  // the create asks for 1.25
    c = CreateAheadTestRead();
    Check(took && spawns == sS2 && destroys == dS2 + 1 && c.spareAdopted == sa0 + 1 && c.runtimeTook == 0,
          "createahead levelled: a run-time reference built at another scale than its create asks for is destroyed and the create takes a "
          "spare of its base form and scale, as with the key at 0");
    // S1 (a): switched off live before the build: an entry noted, not built, is not built after the switch
    fresh();
    note(0xFF000402, fC5, 1.0f);
    CreateAheadFrame(1000, false, false);  // noted, no slack
    SetCreateAheadLevelled(0);
    const int sS1 = spawns, hS1 = CreateAheadTestRead().held;
    for (int i = 0; i < 4; ++i) CreateAheadFrame(8000, false, false);
    Check(spawns == sS1 && CreateAheadTestRead().held == hS1,
          "createahead levelled 1 -> 0 (live): a noted run-time reference that was not built yet is not built any more");
    // S1 (b) + S3: switched off live after the build: not adopted, destroyed in a slack frame, counted under key off
    fresh();
    const int hB0 = CreateAheadTestRead().held;
    note(0xFF000403, fC6, 1.0f);
    for (int i = 0; i < 3; ++i) CreateAheadFrame(8000, false, false);
    const bool built6 = CreateAheadTestRead().held == hB0 + 1;
    SetCreateAheadLevelled(0);
    const int sB = spawns, dB = destroys;
    Stock(world, 0xFF000403, 0xC6, &clsC, 1.0, tmpl, &got, &took);
    c = CreateAheadTestRead();
    const bool notTaken = !took && spawns == sB + 1 && destroys == dB && c.runtimeTook == 0;
    CreateAheadFrame(1000, false, false);  // no slack: nothing destroyed
    const bool waits = destroys == dB;
    CreateAheadFrame(8000, false, false);  // slack: its one destroy
    c = CreateAheadTestRead();
    Check(built6 && notTaken && waits && destroys == dB + 1 && c.dropOff == 1,
          "createahead levelled 1 -> 0 (live): a built run-time reference is not adopted, and is destroyed in a slack frame");
    // S3: noted references whose create never came: counted apart (all, and of them the built)
    fresh();
    note(0xFF000404, fC7, 1.0f);
    CreateAheadFrame(8000, false, false);  // builds it
    note(0xFF000405, fC8, 1.0f);
    CreateAheadFrame(1000, false, false);  // noted, no slack: not built
    clockS += 30.0;
    for (int i = 0; i < 4; ++i) CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(c.runtimeNever == 2 && c.runtimeNeverBuilt == 1,
          "createahead levelled: run-time references noted ahead whose create never came are counted apart from placed references' timeouts");
    // N6: a held run-time actor that is gone at its create: the spares apply
    fresh();
    note(0xFF000406, fC9, 1.0f);
    for (int i = 0; i < 3; ++i) CreateAheadFrame(8000, false, false);
    items[nObjs - 1].obj = nullptr;  // purged by the engine
    const int sG = spawns, g0 = CreateAheadTestRead().gone;
    Stock(world, 0xFF000406, 0xC9, &clsC, 1.0, tmpl, &got, &took);
    c = CreateAheadTestRead();
    Check(!took && spawns == sG + 1 && c.gone == g0 + 1 && c.runtimeTook == 0,
          "createahead levelled: a held run-time actor that is gone at its create is forgotten and the game's SpawnActor runs");
    // N6: another base form under the run-time id: that entry is destroyed and a held spare of the create's base form is taken
    fresh();
    Stock(world, 0xFF000501, 0xCB, &clsC, 1.0, tmpl, &got, &took);  // 0xCB in use
    for (int i = 0; i < 6; ++i) CreateAheadFrame(8000, false, false);
    note(0xFF000407, fCC, 1.0f);
    for (int i = 0; i < 3; ++i) CreateAheadFrame(8000, false, false);
    const int sM = spawns, dM = destroys, saM = CreateAheadTestRead().spareAdopted, rbM = CreateAheadTestRead().rejBase;
    Stock(world, 0xFF000407, 0xCB, &clsC, 1.0, tmpl, &got, &took);
    c = CreateAheadTestRead();
    Check(took && spawns == sM && destroys == dM + 1 && c.rejBase == rbM + 1 && c.spareAdopted == saM + 1,
          "createahead levelled: a run-time id built for another base form is destroyed and the create takes a held spare of its own base form");

    // crash1118: never built from a class the garbage collector is taking away
    // (a) a purge pending: no spare and no reference is built, nothing is destroyed for room; after it, as before
    CreateAheadTestSetup(&fakes);
    clockS = 9600.0;
    gcClass = purgeFlag = 0;
    Stock(world, 0x930, 0xB1, &clsA, 1.0, tmpl, &got, &took);  // the world, the parameters, 0xB1 in use
    purgeFlag = 1;
    const int sQ0 = spawns, dQ0 = destroys;
    for (int i = 0; i < 4; ++i) CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    const bool noSpare = spawns == sQ0 && destroys == dQ0 && c.purgeWait == 4 && c.held == 0;
    note(0x1301, formB1, 1.0f);
    CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(noSpare && spawns == sQ0 && c.purgeWait == 5, "createahead: while a GC purge is pending nothing is built (spares, references)");
    purgeFlag = 0;
    CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(spawns == sQ0 + 1 && c.held == 1 && objs[nObjs - 1].cls == &clsA && c.classRenewed == 0 && c.classLost == 0,
          "createahead: after the purge the reference is built with its class (the build frame's lookup gives the same one)");
    // (b) the class noted with a reference is collected before its build: the build frame's lookup finds nothing, nothing is built,
    // the reference asks for its class again and is built with the class loaded anew
    note(0x1302, formB1, 1.0f);
    CreateAheadFrame(1000, false, false);  // noted, its class looked up (clsA), no slack: not built
    gcClass = 1;                           // collected: the lookup finds nothing (as ResolveObject skips unreachable objects)
    const int sQ1 = spawns;
    CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(spawns == sQ1 && c.classLost == 1, "createahead: a reference whose class is no longer found in the build frame is not built");
    gcClass = 2;                           // loaded again, another object
    CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    const bool builtD = spawns == sQ1 + 1 && objs[nObjs - 1].cls == &clsD;
    Stock(world, 0x1302, 0xB1, &clsD, 1.0, tmpl, &got, &took);
    Check(builtD && took && got == reinterpret_cast<uint8_t*>(&objs[nObjs - 1]),
          "createahead: it asks for its class again and is built with the class the lookup gives now; its create takes it");
    // (c) the class changed between note and build (collected and loaded again): built with the current one, never the noted one
    gcClass = 0;
    note(0x1303, formB1, 1.0f);
    CreateAheadFrame(1000, false, false);  // noted with clsA, not built
    gcClass = 2;
    const int sQ2 = spawns;
    CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(spawns == sQ2 + 1 && objs[nObjs - 1].cls == &clsD && c.classRenewed == 1,
          "createahead: a reference whose class is another object in the build frame is built with that one, not the one noted");
    // (d) the pool full and a purge pending: the destroy that makes room for a reference waits too (review rev-aheadgc N4)
    CreateAheadTestSetup(&fakes);
    clockS = 9700.0;
    gcClass = purgeFlag = 0;
    Stock(world, 0xFF000940, 0xB1, &clsA, 1.0, tmpl, &got, &took);  // three base forms in use (run-time creates): spares fill 8 of 12
    Stock(world, 0xFF000941, 0xB2, &clsB, 1.0, tmpl, &got, &took);
    Stock(world, 0xFF000942, 0xC0, &clsC, 1.0, tmpl, &got, &took);
    for (int i = 0; i < 12; ++i) CreateAheadFrame(8000, false, false);
    const int spF = CreateAheadTestRead().held;
    for (uint32_t id = 0x1311; id < 0x1315; ++id) {              // four references built: the pool holds 12
        note(id, formB1, 1.0f);
        CreateAheadFrame(8000, false, false);
    }
    const int hF = CreateAheadTestRead().held;
    note(0x1315, formB1, 1.0f);                                  // a fifth reference wants room
    purgeFlag = 1;
    const int sF = spawns, dF = destroys, wF = CreateAheadTestRead().purgeWait;
    for (int i = 0; i < 3; ++i) CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    const bool held12 = spF == 8 && hF == 12 && spawns == sF && destroys == dF && c.held == 12 && c.purgeWait == wF + 3;
    const int dbgHeld = c.held, dbgWait = c.purgeWait - wF, dbgS = spawns - sF, dbgD = destroys - dF;
    purgeFlag = 0;
    CreateAheadFrame(8000, false, false);                        // after the purge: the room destroy (one piece of work)
    const bool room = destroys == dF + 1 && spawns == sF;
    CreateAheadFrame(8000, false, false);                        // then the build
    c = CreateAheadTestRead();
    Check(held12 && room && spawns == sF + 1 && c.held == 12 && nObjs < 256,
          "createahead: with the pool full and a GC purge pending nothing is destroyed for room either; after it, the oldest spare makes "
          "room and the reference is built");
    if (!(held12 && room)) printf("    pool-full case: spares %d, then %d held; pending: held %d, waits %d, spawns %d, destroys %d; after: spawns %d, destroys %d, held %d, objects %d\n",
                                  spF, hF, dbgHeld, dbgWait, dbgS, dbgD, spawns - sF, destroys - dF, c.held, nObjs);

    // aheadindex: a held actor whose root is no longer exactly at its build translation is not adopted (the requested
    // translation shown to the listeners would not be the stock one)
    CreateAheadTestSetup(&fakes);
    clockS = 9800.0;
    gcClass = purgeFlag = 0;
    Stock(world, 0x950, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    note(0x1401, formB1, 1.0f);
    CreateAheadFrame(8000, false, false);
    FakeObj* const moved = &objs[nObjs - 1];
    const bool heldOne = CreateAheadTestRead().held == 1 && moved->root;
    if (heldOne) moved->root->loc[2] += 1.0;
    const int sP = spawns, dP = destroys;
    Stock(world, 0x1401, 0xB1, &clsA, 1.0, tmpl, &got, &took);
    c = CreateAheadTestRead();
    Check(heldOne && !took && spawns == sP + 1 && destroys == dP + 1 && c.rejPlace == 1,
          "createahead (aheadindex): a held actor no longer at its build place is not adopted: destroyed, the game's SpawnActor runs");
    {
        const FakeRoot* const r = reinterpret_cast<FakeObj*>(got)->root;
        Check(r && r->loc[0] == 10.0 && r->loc[1] == 20.0 && r->loc[2] == 30.0 && announced[2] == 30.0,
              "createahead (aheadindex): the stock SpawnActor's actor is announced at its spawn place, untouched");
    }
    // SF2: the class is remembered: another reference of the same base form is not built ahead (no spawn, no destroy later)
    const int sK = spawns, dK = destroys;
    note(0x1402, formB1, 1.0f);
    CreateAheadFrame(8000, false, false);  // class asked for
    CreateAheadFrame(8000, false, false);  // the build would come here
    CreateAheadFrame(8000, false, false);
    c = CreateAheadTestRead();
    Check(spawns == sK && destroys == dK && c.held == 0 && c.offsetSkipped >= 1,
          "createahead (aheadindex): a class whose root had an offset of its own is not built ahead any more (SF2)");
    gcClass = purgeFlag = 0;
    CreateAheadTestSetup(nullptr);
}

}  // namespace cah
