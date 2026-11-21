// StartupBarFinish self-test (worker loader106, 2026-11-16, S-058; review rev106e S1/S2), included by selftest.cpp.
// On the mapped exe (Steam 19115871): the two finish loops' step calls exe+0x6770CD2 (LoadingMenu's destructor) and
// exe+0x67723BA (the load's finish), both calling the bar step exe+0x6770FD0; the destructor exe+0x6770C20 and the
// deleting destructor's call of it exe+0x6770F9F (nothing is patched in the test). Then the hooks with a fake step that
// moves the bar by the game's rules (exe+0x6771059..0x6771084: +1 when more than 2500 ms passed since the last move, with
// no goal test; else +1 below the goal when more than 100 ms, or more than 10 ms with the finishing flag; a change sets the
// last-move time) on a fake clock, run in the game's own loop `while (bar < 100) step(0)`.
#pragma once

namespace barfinish_test {
struct FakeData {
    uint8_t bytes[0xC0];
    int32_t& goal() { return *reinterpret_cast<int32_t*>(bytes + 0x64); }
    int32_t& bar() { return *reinterpret_cast<int32_t*>(bytes + 0xA0); }
    int32_t& target() { return *reinterpret_cast<int32_t*>(bytes + 0xA4); }
    int32_t& lastMove() { return *reinterpret_cast<int32_t*>(bytes + 0xA8); }
    uint8_t& finishing() { return bytes[0xB0]; }
    void reset(int startBar, uint8_t flag, int goal_, int lastMoveMs)
    {
        memset(bytes, 0, sizeof bytes);
        bar() = startBar;
        target() = startBar;
        finishing() = flag;
        goal() = goal_;
        lastMove() = lastMoveMs;
    }
};
FakeData g_data;
int g_stepCalls = 0, g_now = 0, g_firstAdvance = 16, g_advance = 16;
uint64_t g_lastArg = 99;
void FakeStep(uint64_t arg)
{
    g_now += g_stepCalls == 0 ? g_firstAdvance : g_advance;  // the clock moves between calls
    ++g_stepCalls;
    g_lastArg = arg;
    const int elapsed = g_now - g_data.lastMove();
    int t = g_data.target();
    if (elapsed > 2500) ++t;
    else if (t < g_data.goal() && (elapsed > 100 || (g_data.finishing() && elapsed > 10))) ++t;
    g_data.target() = t;
    if (g_data.bar() != t) g_data.bar() = t, g_data.lastMove() = g_now;
}
int RunLoop(int site, int firstAdvance = 16, int advance = 16)
{
    g_stepCalls = 0;
    g_firstAdvance = firstAdvance;
    g_advance = advance;
    int guard = 0;
    while (g_data.bar() < 100 && ++guard < 100000) StartupBarFinishStep(0, g_data.bytes, static_cast<uint64_t>(site));
    return g_stepCalls;
}
int g_dtorCalls = 0;
bool g_firstClosedInDtor = true;
uint64_t FakeDtor(uint64_t menu, uint64_t flags)
{
    ++g_dtorCalls;
    g_firstClosedInDtor = StartupBarFinishTestFirstClosed();  // the key is spent only after the game's destructor
    return menu + flags;
}
}  // namespace barfinish_test

static void BarFinishRun(const GameModule& game)
{
    using namespace barfinish_test;
    if (game.timeDateStamp == 0xF19077A4u) {
        uint32_t rva[5] = {};
        const bool ok = StartupBarFinishTestDiscover(game, rva);
        Check(ok && rva[0] == 0x6770CD2 && rva[1] == 0x67723BA && rva[2] == 0x6770FD0 && rva[3] == 0x6770C20 && rva[4] == 0x6770F9F,
              "StartupBarFinish: the two loading-bar finish loops' step calls, the bar step, LoadingMenu's destructor and the "
              "deleting destructor's call of it found on the mapped exe");
        if (!ok || rva[0] != 0x6770CD2) printf("    found %d: %X %X step %X dtor %X call %X\n", ok, rva[0], rva[1], rva[2], rva[3], rva[4]);
    }
    void* const step = reinterpret_cast<void*>(&FakeStep);
    void* const dtor = reinterpret_cast<void*>(&FakeDtor);
    auto same = [](const FakeData& a, FakeData& b) {
        return a.bytes[0xB0] == b.finishing() && *reinterpret_cast<const int32_t*>(a.bytes + 0xA0) == b.bar() &&
               *reinterpret_cast<const int32_t*>(a.bytes + 0xA4) == b.target() &&
               *reinterpret_cast<const int32_t*>(a.bytes + 0x64) == b.goal();
    };

    // key 0: the game's steps, one value per tick from bar 1 (as at start-up)
    StartupBarFinishTestSetup(step, dtor, 0, false);
    g_now = 1000;
    g_data.reset(1, 1, 100, 1000);
    Check(RunLoop(0) == 99 && g_data.bar() == 100 && g_data.target() == 100 && g_lastArg == 0,
          "StartupBarFinish 0: the finish loop runs the game's 99 values from 1 to 100");
    const FakeData stock = g_data;
    Check(!StartupBarFinishTestFirstClosed(), "StartupBarFinish: a loop alone does not spend key 1 (the destructor does)");

    // key 1, the first LoadingMenu's loop: one call, the stock final state (bar and target exactly 100)
    StartupBarFinishTestSetup(step, dtor, 1, false);
    g_now = 1000;
    g_data.reset(1, 1, 100, 1000);
    Check(RunLoop(0) == 1 && same(stock, g_data), "StartupBarFinish 1: the bar moved 16 ms ago: one call, bar and target 100");
    StartupBarFinishTestSetup(step, dtor, 1, false);
    g_now = 10000;
    g_data.reset(1, 1, 100, 1000);  // the last move 9 s ago: the 2500 ms rule (+1 with no goal test) applies
    Check(RunLoop(0) == 1 && same(stock, g_data), "StartupBarFinish 1 (rev106e S1): last move > 2.5 s ago: one call, exactly 100 (not 101)");
    StartupBarFinishTestSetup(step, dtor, 1, false);
    g_now = 1000;
    g_data.reset(1, 1, 100, 1000);  // moved 5 ms before the first call: 99 shown first, then 100 at the next tick
    const int nFresh = RunLoop(0, 5, 16);
    Check(nFresh == 2 && same(stock, g_data), "StartupBarFinish 1: the bar moved < 10 ms before: 99, then 100 one tick later");

    // key 1 only for the destructor's loop: a load-finish loop runs the game's steps even before the first close
    StartupBarFinishTestSetup(step, dtor, 1, false);
    g_now = 1000;
    g_data.reset(40, 1, 100, 1000);
    Check(RunLoop(1) == 60 && g_data.bar() == 100, "StartupBarFinish 1 (rev106e S2): a load-finish loop runs the game's steps");

    // the destructor hook: the game's destructor first (its loop applied), then key 1 is spent, also when no loop ran
    StartupBarFinishTestSetup(step, dtor, 1, false);
    g_dtorCalls = 0;
    const uint64_t r = StartupBarFinishDtor(0x1000, 1);
    Check(g_dtorCalls == 1 && r == 0x1001 && !g_firstClosedInDtor && StartupBarFinishTestFirstClosed(),
          "StartupBarFinish (rev106e S2): the destructor call runs the game's destructor, then spends key 1 (no loop needed)");
    g_now = 1000;
    g_data.reset(0, 1, 100, 1000);
    Check(RunLoop(0) == 100, "StartupBarFinish 1: after the first destructor, a later closing loop runs the game's steps");
    g_data.reset(40, 1, 100, 1000);
    Check(RunLoop(1) == 60, "StartupBarFinish 1: after the first destructor, a load-finish loop runs the game's steps");

    // the guards: no finishing flag or another goal = the game's steps
    StartupBarFinishTestSetup(step, dtor, 2, false);
    g_now = 1000;
    g_data.reset(1, 0, 100, 1000);
    Check(RunLoop(0, 101, 101) == 99, "StartupBarFinish: without the finishing flag the target is not touched");
    StartupBarFinishTestSetup(step, dtor, 2, false);
    g_now = 1000;
    g_data.reset(1, 1, 50, 1000);
    g_stepCalls = 0;
    g_firstAdvance = 16;
    StartupBarFinishStep(0, g_data.bytes, 0);
    Check(g_stepCalls == 1 && g_data.target() == 2, "StartupBarFinish: a goal other than 100 leaves the target to the game");

    // key 2: every loop ends at once, both sites, exactly 100
    StartupBarFinishTestSetup(step, dtor, 2, true);
    g_now = 1000;
    g_data.reset(1, 1, 100, 1000);
    Check(RunLoop(0) == 1 && same(stock, g_data), "StartupBarFinish 2: every closing loop ends at once at 100");
    g_now = 10000;
    g_data.reset(7, 1, 100, 1000);
    Check(RunLoop(1) == 1 && g_data.bar() == 100, "StartupBarFinish 2: every load-finish loop ends at once at 100 (also after 2.5 s)");
    StartupBarFinishTestSetup(step, dtor, 0, false);
}
