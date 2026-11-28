// RiderBindGuard self-test (worker crash1121r 2026-11-22, S-073; review revs073 S1/S2 cases by fixs073b), included by
// selftest.cpp: the discovery on the mapped exe (StoreTargets' rider step, StoreTargets and its calls, both vtables by RTTI,
// both destructor shapes, the controller's reset and its call), the install (two vtable slots, the calls retargeted to two
// stubs), then the hooks' logic on fakes: a horse sequence whose rider items point into a rider controller's blend array is
// noted after StoreTargets; that controller's death nulls exactly those items (not the horse's own items); during a later
// StoreTargets the nulled items hold the sentinel (non-null as in stock) and are null again afterwards unless the game bound
// them; a never-bound item stays null; off the OblivionThread nothing is nulled (counted); the controller's reset nulls as its
// death does; a sequence is dropped at its destruction; the live switch stops the nulling only.
#pragma once
namespace rbind {

struct FakeMtt {
    void* vt;
    uint8_t pad[0x48];
    uint8_t* blend;        // +0x50
    void* targets;         // +0x58
    uint16_t count;        // +0x60
    uint8_t pad2[0x6];
};
static_assert(offsetof(FakeMtt, blend) == 0x50 && offsetof(FakeMtt, count) == 0x60, "fake controller layout");
struct FakeItem { void* interp; void* ctl; uint8_t* blend; uint8_t idx, prio, pad[6]; };
static_assert(sizeof(FakeItem) == 0x20, "item size");
struct FakeSeq {
    void* vt;
    uint8_t pad[0x10];
    uint32_t count;        // +0x18
    uint32_t pad2;
    FakeItem* items;       // +0x20
};
static_assert(offsetof(FakeSeq, count) == 0x18 && offsetof(FakeSeq, items) == 0x20, "fake sequence layout");

static int g_stores = 0, g_mttDtors = 0, g_seqDtors = 0, g_resets = 0;
static void (*g_onStore)(uint8_t* seq) = nullptr;  // runs inside the fake StoreTargets (sees the items as the game would)
static uint64_t FakeStore(uint8_t* seq, void*)
{
    ++g_stores;
    if (g_onStore) g_onStore(seq);
    return 1;
}
static void* FakeMttDtor(uint8_t* self, uint32_t) { ++g_mttDtors; return self; }
static void* FakeSeqDtor(uint8_t* self, uint32_t) { ++g_seqDtors; return self; }
static void FakeReset(uint8_t*) { ++g_resets; }

// a blend array of n elements with its count at [-8], as the game's array new makes it
static uint8_t* NewBlendArray(std::vector<uint8_t>& mem, size_t n)
{
    mem.assign(8 + n * 0x2028, 0);
    *reinterpret_cast<uint64_t*>(mem.data()) = n;
    return mem.data() + 8;
}

// what the fake StoreTargets saw / does
static const void* g_seen[6] = {};
static uint8_t* g_rebind2 = nullptr;  // non-null: item 2 is bound to this inside the call (as the first pass / rider step would)
static void Inspect(uint8_t* seq)
{
    auto* const items = reinterpret_cast<FakeItem*>(*reinterpret_cast<uint8_t**>(seq + 0x20));
    for (int k = 0; k < 6; ++k) g_seen[k] = items[k].blend;
    if (g_rebind2) items[2].blend = g_rebind2;
}

static void Run(const GameModule& game)
{
    RiderBindGuardInfo d{};
    Check(RiderBindGuardDiscover(game, &d), "RiderBindGuard: discovery on the mapped exe");
    Check(d.matches == 1, "RiderBindGuard: StoreTargets' rider step found once");
    Check(d.reset && d.resetSite, "RiderBindGuard: the controller's reset and its one direct call found");
    if (game.timeDateStamp == 0xF19077A4u) {
        Check(d.store - game.base == 0x67DB9C0 && d.riderStore - game.base == 0x67DC2BF,
              "RiderBindGuard: StoreTargets exe+0x67DB9C0, rider store exe+0x67DC2BF (build 19115871)");
        Check(d.nSites == 4 && d.sites[0] - game.base == 0x67F4531 && d.sites[1] - game.base == 0x68517FC &&
                  d.sites[2] - game.base == 0x6852283 && d.sites[3] - game.base == 0x6852302,
              "RiderBindGuard: StoreTargets' four calls (exe+0x67F4531, 0x68517FC, 0x6852283, 0x6852302)");
        Check(reinterpret_cast<uint8_t*>(d.mttVt) - game.base == 0x86452D8 && reinterpret_cast<uint8_t*>(d.seqVt) - game.base == 0x86478C0 &&
                  d.mttDtor - game.base == 0x67D7300 && d.seqDtor - game.base == 0x67EE5D0,
              "RiderBindGuard: controller vtable exe+0x86452D8 (destructor 0x67D7300), BSAnimGroupSequence exe+0x86478C0 (0x67EE5D0)");
        Check(d.reset && d.reset - game.base == 0x6853790 && d.resetSite && d.resetSite - game.base == 0x67F0D17,
              "RiderBindGuard: the controller's reset exe+0x6853790, its call exe+0x67F0D17");
    }
    if (!Installed(InstallRiderBindGuard(true, true), "RiderBindGuard (installs on the mapped exe)")) return;
    RiderBindGuardInfo st{};
    Check(RiderBindGuardState(&st) && st.stub && st.nSites == d.nSites, "RiderBindGuard: installed state");
    bool callsOk = st.nSites > 0;
    for (size_t i = 0; i < st.nSites; ++i)
        callsOk = callsOk && st.sites[i][0] == 0xE8 && RipTarget(st.sites[i], 1, 5) == st.stub;
    Check(callsOk, "RiderBindGuard: every StoreTargets call goes to the stub");
    Check(st.resetSite && st.resetSite[0] == 0xE8 && RipTarget(st.resetSite, 1, 5) == st.stub + 16 && st.reset == d.reset,
          "RiderBindGuard: the reset's call goes to the second stub");
    Check(st.stub[0] == 0xFF && st.stub[1] == 0x25 && *reinterpret_cast<const uint32_t*>(st.stub + 2) == 0 &&
              st.stub[16] == 0xFF && st.stub[17] == 0x25 && *reinterpret_cast<const uint32_t*>(st.stub + 18) == 0,
          "RiderBindGuard: both stubs are absolute jumps");
    MEMORY_BASIC_INFORMATION mi{};
    Check(VirtualQuery(st.stub, &mi, sizeof mi) && mi.Protect == PAGE_EXECUTE_READ, "RiderBindGuard: the stub's page is read-execute");
    Check(d.mttVt[0] != d.mttDtor && d.seqVt[0] != d.seqDtor, "RiderBindGuard: both vtable slots replaced");
    Check(RiderBindGuardGroupSeqDtor(d.seqVt) == d.seqDtor, "RiderBindGuard: BSAnimGroupSequence's original destructor is given back");
    Check(InstallRiderBindGuard(true, true), "RiderBindGuard: a second install finds it in place");

    // ---- the logic on fakes ----
    void* mttVt[1] = {nullptr};
    void* seqVt[1] = {nullptr};
    void* otherVt[1] = {nullptr};
    RiderBindGuardTestSetup(mttVt, seqVt, reinterpret_cast<void*>(&FakeStore), reinterpret_cast<void*>(&FakeMttDtor),
                            reinterpret_cast<void*>(&FakeSeqDtor), reinterpret_cast<void*>(&FakeReset));
    const void* const sentinel = RiderBindGuardTestSentinel();
    std::vector<uint8_t> horseMem, riderMem, rider2Mem;
    FakeMtt horse{mttVt, {}, NewBlendArray(horseMem, 6), nullptr, 6, {}};
    FakeMtt rider{mttVt, {}, NewBlendArray(riderMem, 4), nullptr, 4, {}};
    FakeMtt rider2{mttVt, {}, NewBlendArray(rider2Mem, 4), nullptr, 4, {}};
    // horse sequence A (active idle): items 0-1 horse bones, 2-4 rider bones, 5 never bound (stock null)
    FakeItem a[6] = {};
    a[0] = {&a[0], &horse, horse.blend + 0 * 0x2028, 0, 0, {}};
    a[1] = {&a[1], &horse, horse.blend + 1 * 0x2028, 0, 0, {}};
    a[2] = {&a[2], &horse, rider.blend + 0 * 0x2028, 0, 0, {}};
    a[3] = {&a[3], &horse, rider.blend + 2 * 0x2028, 0, 0, {}};
    a[4] = {&a[4], &horse, rider.blend + 3 * 0x2028, 0, 0, {}};
    a[5] = {&a[5], &horse, nullptr, 0xFF, 0, {}};
    FakeSeq seqA{seqVt, {}, 6, 0, a};
    // horse sequence B: only horse bones (never noted); sequence C of another class with a rider item (never noted)
    FakeItem b[2] = {{b, &horse, horse.blend, 0, 0, {}}, {b + 1, &horse, horse.blend + 0x2028 * 5, 0, 0, {}}};
    FakeSeq seqB{seqVt, {}, 2, 0, b};
    FakeItem c[1] = {{c, &horse, rider.blend + 0x2028, 0, 0, {}}};
    FakeSeq seqC{otherVt, {}, 1, 0, c};
    g_stores = g_mttDtors = g_seqDtors = g_resets = 0;
    g_onStore = nullptr;
    g_rebind2 = nullptr;
    RiderBindGuardTestStore(&seqA, nullptr);
    RiderBindGuardTestStore(&seqB, nullptr);
    RiderBindGuardTestStore(&seqC, nullptr);
    Check(g_stores == 3 && RiderBindGuardTestNoted() == 1, "RiderBindGuard: StoreTargets runs; only the sequence with rider items is noted");
    // another rider's controller dies: nothing changes
    RiderBindGuardTestMttDtor(&rider2, 1);
    Check(g_mttDtors == 1 && a[2].blend && a[3].blend && a[4].blend && RiderBindGuardTestNoted() == 1,
          "RiderBindGuard: a controller the sequence does not point into: items kept, the game's destructor runs");
    // the rider's controller dies off the OblivionThread: left as stock leaves them, counted (review S2)
    RiderBindGuardTestOnOt(false);
    RiderBindGuardTestMttDtor(&rider, 1);
    Check(g_mttDtors == 2 && a[2].blend == rider.blend && a[3].blend && a[4].blend && RiderBindGuardTestOffOt() == 3,
          "RiderBindGuard: off the OblivionThread the items are left as the game has them (3 counted)");
    RiderBindGuardTestOnOt(true);
    // on the OT: its three items null, the horse's own kept, an unnoted class's kept
    RiderBindGuardTestMttDtor(&rider, 1);
    Check(g_mttDtors == 3 && !a[2].blend && !a[3].blend && !a[4].blend && a[0].blend == horse.blend &&
              a[1].blend == horse.blend + 0x2028 && c[0].blend == rider.blend + 0x2028,
          "RiderBindGuard: the dying rider's items null; the horse's own and an unnoted class's left as they were");
    Check(RiderBindGuardTestNoted() == 1, "RiderBindGuard: a sequence with nulled items stays noted (for the sentinel)");
    // a later StoreTargets (review S1): inside it the nulled items are non-null (the sentinel), the never-bound one is null;
    // the game binds item 2 to a new rider; afterwards items 3 and 4 are null again, item 2 keeps the game's binding
    g_onStore = &Inspect;
    g_rebind2 = rider2.blend;
    RiderBindGuardTestStore(&seqA, nullptr);
    Check(g_seen[2] == sentinel && g_seen[3] == sentinel && g_seen[4] == sentinel && g_seen[5] == nullptr &&
              g_seen[0] == horse.blend,
          "RiderBindGuard: during StoreTargets the cleared items hold the sentinel, a never-bound item stays null");
    Check(a[2].blend == rider2.blend && !a[3].blend && !a[4].blend && !a[5].blend && a[0].blend == horse.blend,
          "RiderBindGuard: after StoreTargets the items the game bound keep it, the others are null again");
    // the next StoreTargets: only items 3 and 4 get the sentinel (item 2 is the game's again)
    g_rebind2 = nullptr;
    RiderBindGuardTestStore(&seqA, nullptr);
    Check(g_seen[2] == rider2.blend && g_seen[3] == sentinel && g_seen[4] == sentinel && !a[3].blend && !a[4].blend &&
              a[2].blend == rider2.blend,
          "RiderBindGuard: a rebound item is forgotten, the still-cleared ones keep the sentinel during the call");
    g_onStore = nullptr;
    // the new rider's controller is reset (its array freed and reallocated): item 2 null, the game's reset runs
    RiderBindGuardTestReset(&rider2);
    Check(g_resets == 1 && !a[2].blend && RiderBindGuardTestNoted() == 1,
          "RiderBindGuard: the controller's reset clears the items into its array first, then runs");
    // the sequence is destroyed before its rider: dropped, the game's destructor runs, the rider's death touches nothing
    RiderBindGuardTestSeqDtor(&seqA, 1);
    Check(g_seqDtors == 1 && RiderBindGuardTestNoted() == 0, "RiderBindGuard: a destroyed sequence is dropped first");
    a[2].blend = rider2.blend;
    a[3].blend = rider2.blend + 0x2028;
    RiderBindGuardTestMttDtor(&rider2, 1);
    Check(a[2].blend == rider2.blend && a[3].blend == rider2.blend + 0x2028, "RiderBindGuard: a dropped sequence is never written");
    // live off: noted, but not nulled
    RiderBindGuardTestStore(&seqA, nullptr);
    SetRiderBindGuard(false);
    RiderBindGuardTestMttDtor(&rider2, 1);
    Check(a[2].blend == rider2.blend && RiderBindGuardTestNoted() == 1, "RiderBindGuard: switched off live, items left as the game has them");
    SetRiderBindGuard(true);
    RiderBindGuardTestMttDtor(&rider2, 1);
    Check(!a[2].blend && !a[3].blend && RiderBindGuardTestNoted() == 1, "RiderBindGuard: switched on again, the next death nulls");
    RiderBindGuardTestSeqDtor(&seqA, 1);
    LogRiderBindGuardStats();
}

}  // namespace rbind
