// HeadCancelNoWait self-test (worker s057fix 2026-11-23, S-057), included by selftest.cpp: the discovery on the mapped exe
// (the child loop and its call of the task cancel, the composite cancel, the worker's run step, the QueuedHead vtable,
// the composite vtables, QueuedPlayer left out), the decision and the drain with fake tasks (also the review revheadcancel
// gates: the player's form, a set face cache, a thread that is not the OblivionThread) (no game code runs: the task cancel is a fake that
// finishes a state-5 task like the game's), and the install (the loop's call goes to a stub that passes rsi).
#pragma once
namespace headcancel_test {
struct FakeTask {
    void* vt;
    volatile long refs;
    volatile int32_t state;
    uint8_t pad[0x28 - 0x10];
    const uint8_t* npc;  // +0x28
    uint8_t rest[0x20];
};
static_assert(offsetof(FakeTask, npc) == 0x28, "fake task layout");
int g_deleted = 0, g_cancels = 0, g_finishes = 0;
const void* g_lastCancelled = nullptr;
void* FakeDtor(uint8_t*, uint32_t flags)
{
    if (flags & 1) ++g_deleted;
    return nullptr;
}
void* g_headVt[12] = {reinterpret_cast<void*>(&FakeDtor)};
void* g_otherVt[12] = {reinterpret_cast<void*>(&FakeDtor)};
void* g_compVt[12] = {reinterpret_cast<void*>(&FakeDtor)};
uint64_t FakeCancel(uint64_t, uint64_t task)
{
    auto* t = reinterpret_cast<FakeTask*>(task);
    ++g_cancels;
    g_lastCancelled = t;
    if (t->state == 5) {
        t->state = 6;
        ++g_finishes;
    }
    return 7;
}
DWORD WINAPI DrainElsewhere(LPVOID p) { return static_cast<DWORD>(HeadCancelTestDrain(static_cast<HeadCancelTest*>(p))); }
}  // namespace headcancel_test

static void HeadCancelNoWaitRun(const GameModule& game)
{
    using namespace headcancel_test;
    HeadCancelNoWaitInfo d{};
    Check(HeadCancelNoWaitDiscover(game, &d), "HeadCancelNoWait: discovery on the mapped exe");
    Check(d.matches == 111, "HeadCancelNoWait: one child loop, one composite cancel, one worker run step");
    Check(d.composites >= 1 && d.composites <= 8, "HeadCancelNoWait: composite vtables found");
    Check(d.playerSkipped == 1, "HeadCancelNoWait: QueuedPlayer's vtable found once and left out (review M1)");
    if (game.timeDateStamp == 0xF19077A4u) {
        Check(d.loop - game.base == 0x67E6610 && d.site - game.base == 0x67E6658 && d.cancel - game.base == 0x67E0E00 &&
                  d.composite - game.base == 0x67E66E0 && d.workerRun - game.base == 0x67E0F20 &&
                  static_cast<const uint8_t*>(d.headVt) - game.base == 0x8646C30,
              "HeadCancelNoWait: loop exe+0x67E6610, call exe+0x67E6658, cancel exe+0x67E0E00, composite exe+0x67E66E0, "
              "worker run exe+0x67E0F20, QueuedHead vtable exe+0x8646C30 (build 19115871)");
        bool all = d.composites == 4;
        for (uint32_t rva : {0x8646D38u, 0x8646DB8u, 0x8646E38u, 0x8646EB8u}) {
            bool hit = false;
            for (int i = 0; i < d.composites; ++i) hit |= static_cast<const uint8_t*>(d.compositeVt[i]) - game.base == rva;
            all &= hit;
        }
        for (int i = 0; i < d.composites; ++i) all &= static_cast<const uint8_t*>(d.compositeVt[i]) - game.base != 0x8646F38;
        Check(all, "HeadCancelNoWait: the four composites QueuedReference / Tree / Character / Creature, not QueuedPlayer "
                   "exe+0x8646F38 (build 19115871)");
    }

    // ---- the decision and the drain, with fakes ----
    HeadCancelCtx api{};
    api.cancel = &FakeCancel;
    api.headVt = g_headVt;
    api.compositeVt[0] = g_compVt;
    api.composites = 1;
    api.mode = 1;
    HeadCancelTest* const t = HeadCancelTestNew(&api);
    HeadCancelTestOnOt(t, 1);
    static uint8_t npc[0x310] = {};
    npc[8] = 0x23;
    *reinterpret_cast<uint32_t*>(npc + 0x10) = 0x0000A08F;
    static uint8_t dynNpc[0x310] = {};
    dynNpc[8] = 0x23;
    *reinterpret_cast<uint32_t*>(dynNpc + 0x10) = 0xFF000801;
    static uint8_t notNpc[0x310] = {};
    notNpc[8] = 0x24;
    static uint8_t playerNpc[0x310] = {};
    playerNpc[8] = 0x23;
    *reinterpret_cast<uint32_t*>(playerNpc + 0x10) = 0x7;
    static uint8_t cachedNpc[0x310] = {}, cachedNpc2[0x310] = {};
    cachedNpc[8] = cachedNpc2[8] = 0x23;
    *reinterpret_cast<uint32_t*>(cachedNpc + 0x10) = *reinterpret_cast<uint32_t*>(cachedNpc2 + 0x10) = 0x0000A08F;
    *reinterpret_cast<uint64_t*>(cachedNpc + 0x2F0) = 0x1234;
    *reinterpret_cast<uint64_t*>(cachedNpc2 + 0x2F8) = 0x1234;
    FakeTask parent{g_compVt, 2, 6};
    FakeTask child{g_headVt, 2, 3};
    child.npc = npc;
    // running head of a cancelled composite: held, not cancelled, one ref each
    g_cancels = 0;
    Check(HeadCancelTestStep(t, 9, reinterpret_cast<uint8_t*>(&child), reinterpret_cast<uint8_t*>(&parent)) == 0 &&
              g_cancels == 0 && HeadCancelTestHeld(t) == 1 && child.refs == 3 && parent.refs == 3,
          "HeadCancelNoWait: a running head (state 3) of a cancelled composite is held (one ref each), not waited for");
    // still running: a drain leaves it
    Check(HeadCancelTestDrain(t) == 0 && HeadCancelTestHeld(t) == 1 && g_cancels == 0,
          "HeadCancelNoWait: a drain leaves a head that is still running");
    child.state = 4;
    Check(HeadCancelTestDrain(t) == 0 && HeadCancelTestHeld(t) == 1, "HeadCancelNoWait: ... also in state 4");
    // done on the worker: another thread's drain does not touch it
    child.state = 5;
    HANDLE th = CreateThread(nullptr, 0, &DrainElsewhere, t, 0, nullptr);
    DWORD code = 99;
    if (th) {
        WaitForSingleObject(th, 10000);
        GetExitCodeThread(th, &code);
        CloseHandle(th);
    }
    Check(code == 0 && HeadCancelTestHeld(t) == 1 && child.state == 5, "HeadCancelNoWait: another thread does not finish it");
    // this thread: the stock finish (5 -> 6) once, then both refs released
    g_finishes = 0;
    Check(HeadCancelTestDrain(t) == 1 && HeadCancelTestHeld(t) == 0 && g_finishes == 1 && g_lastCancelled == &child &&
              child.state == 6 && child.refs == 2 && parent.refs == 2,
          "HeadCancelNoWait: done -> the game's finish call on the same thread, then both refs released");
    // the last refs: our release deletes (the game's release: slot 0 with 1)
    {
        FakeTask p2{g_compVt, 0, 6}, c2{g_headVt, 0, 3};
        c2.npc = npc;
        g_deleted = 0;
        HeadCancelTestStep(t, 9, reinterpret_cast<uint8_t*>(&c2), reinterpret_cast<uint8_t*>(&p2));
        c2.state = 5;
        Check(HeadCancelTestDrain(t) == 1 && g_deleted == 2 && c2.refs == 0 && p2.refs == 0,
              "HeadCancelNoWait: holding the last refs, the drain deletes child and composite through slot 0");
    }
    // the hook's next call drains first
    {
        FakeTask c3{g_headVt, 1, 3};
        c3.npc = npc;
        HeadCancelTestStep(t, 9, reinterpret_cast<uint8_t*>(&c3), reinterpret_cast<uint8_t*>(&parent));
        c3.state = 5;
        FakeTask other{g_otherVt, 1, 1};
        g_cancels = 0;
        g_finishes = 0;
        HeadCancelTestStep(t, 9, reinterpret_cast<uint8_t*>(&other), reinterpret_cast<uint8_t*>(&parent));
        Check(HeadCancelTestHeld(t) == 0 && g_finishes == 1 && g_cancels == 2 && g_lastCancelled == &other && c3.state == 6,
              "HeadCancelNoWait: the next call drains the finished head, then cancels its own task the game's way");
    }
    // not held: the game's cancel is called (with the same arguments and result)
    struct Case {
        void* childVt;
        int32_t childState;
        int32_t parentState;
        void* parentVt;
        const uint8_t* npc;
        const char* what;
    } cases[] = {
        {g_headVt, 1, 6, g_compVt, npc, "a queued head (state 1)"},
        {g_headVt, 5, 6, g_compVt, npc, "a finished head (state 5)"},
        {g_headVt, 3, 5, g_compVt, npc, "a composite not in state 6"},
        {g_otherVt, 3, 6, g_compVt, npc, "another child class"},
        {g_headVt, 3, 6, g_otherVt, npc, "another parent class"},
        {g_headVt, 3, 6, g_compVt, dynNpc, "a dynamic (FF) NPC form"},
        {g_headVt, 3, 6, g_compVt, notNpc, "a form that is not an NPC"},
        {g_headVt, 3, 6, g_compVt, nullptr, "no NPC"},
        {g_headVt, 3, 6, g_compVt, playerNpc, "the player's NPC form 7 (review M1)"},
        {g_headVt, 3, 6, g_compVt, cachedNpc, "the NPC's face cache +0x2F0 set (review S1)"},
        {g_headVt, 3, 6, g_compVt, cachedNpc2, "the NPC's face cache +0x2F8 set (review S1)"},
    };
    for (const Case& k : cases) {
        FakeTask p{k.parentVt, 1, k.parentState}, c{k.childVt, 1, k.childState};
        c.npc = k.npc;
        g_cancels = 0;
        const uint64_t r = HeadCancelTestStep(t, 9, reinterpret_cast<uint8_t*>(&c), reinterpret_cast<uint8_t*>(&p));
        char what[160];
        sprintf_s(what, "HeadCancelNoWait: not held, the game's cancel: %s", k.what);
        Check(r == 7 && g_cancels == 1 && g_lastCancelled == &c && HeadCancelTestHeld(t) == 0 && c.refs == 1 && p.refs == 1, what);
    }
    Check(HeadCancelTestFaceCacheSet(t) == 2, "HeadCancelNoWait: the two face-cache refusals counted");
    // not on the OblivionThread: the game's wait, counted (review S2)
    HeadCancelTestOnOt(t, 0);
    {
        FakeTask p{g_compVt, 1, 6}, c{g_headVt, 1, 3};
        c.npc = npc;
        g_cancels = 0;
        HeadCancelTestStep(t, 9, reinterpret_cast<uint8_t*>(&c), reinterpret_cast<uint8_t*>(&p));
        Check(g_cancels == 1 && g_lastCancelled == &c && HeadCancelTestHeld(t) == 0 && HeadCancelTestOffOt(t) == 1 && c.refs == 1,
              "HeadCancelNoWait: not on the OblivionThread = the game's wait, counted (review S2)");
    }
    HeadCancelTestOnOt(t, 1);
    // mode 0: never held
    api.mode = 0;
    {
        FakeTask p{g_compVt, 1, 6}, c{g_headVt, 1, 3};
        c.npc = npc;
        g_cancels = 0;
        HeadCancelTestStep(t, 9, reinterpret_cast<uint8_t*>(&c), reinterpret_cast<uint8_t*>(&p));
        Check(g_cancels == 1 && HeadCancelTestHeld(t) == 0, "HeadCancelNoWait: mode 0 = the game's wait");
    }
    api.mode = 1;
    // the bound: kHeadCancelMax held, the next one waits the game's way
    {
        static FakeTask kids[kHeadCancelMax + 1];
        FakeTask p{g_compVt, 1, 6};
        for (FakeTask& k : kids) {
            k = FakeTask{g_headVt, 1, 3};
            k.npc = npc;
        }
        g_cancels = 0;
        for (FakeTask& k : kids) HeadCancelTestStep(t, 9, reinterpret_cast<uint8_t*>(&k), reinterpret_cast<uint8_t*>(&p));
        Check(HeadCancelTestHeld(t) == kHeadCancelMax && g_cancels == 1 && HeadCancelTestFull(t) == 1 &&
                  g_lastCancelled == &kids[kHeadCancelMax],
              "HeadCancelNoWait: at most 16 held, then the game's wait");
        for (FakeTask& k : kids) k.state = 5;
        Check(HeadCancelTestDrain(t) == kHeadCancelMax && HeadCancelTestHeld(t) == 0 && p.refs == 1,
              "HeadCancelNoWait: all 16 finished and released");
    }
    HeadCancelTestFree(t);

    // ---- the install on the mapped exe ----
    if (!d.site) return;
    Check(InstallHeadCancelNoWait(true, 1), "HeadCancelNoWait: installs on the mapped exe");
    HeadCancelNoWaitInfo s{};
    Check(HeadCancelNoWaitState(&s) && s.site == d.site && s.stub, "HeadCancelNoWait: installed state");
    if (!s.stub) return;
    Check(d.site[0] == 0xE8 && d.site + 5 + *reinterpret_cast<const int32_t*>(d.site + 1) == s.stub,
          "HeadCancelNoWait: the child loop's call now goes to the stub");
    Check(memcmp(s.stub, "\x4C\x8B\xC6\xFF\x25\x00\x00\x00\x00", 9) == 0 && *reinterpret_cast<void* const*>(s.stub + 9) != nullptr,
          "HeadCancelNoWait: the stub passes rsi (the composite) in r8 and jumps to the hook");
    MEMORY_BASIC_INFORMATION mi{};
    Check(VirtualQuery(s.stub, &mi, sizeof mi) && mi.Protect == PAGE_EXECUTE_READ, "HeadCancelNoWait: the stub's page is read-execute");
    Check(InstallHeadCancelNoWait(true, 1), "HeadCancelNoWait: a second install finds it in place");
    HeadCancelNoWaitInfo again{};
    Check(!HeadCancelNoWaitDiscover(game, &again), "HeadCancelNoWait: discovery refuses an already patched call");
}
