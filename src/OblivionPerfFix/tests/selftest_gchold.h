// GcPassAfterBusy self-test (worker gchold, 2026-11-14), included by selftest.cpp.
// On the mapped exe (Steam 19115871): UEngine::ConditionalCollectGarbage exe+0x37D8A20, its timer path's virtual call
// (GetTimeBetweenGarbageCollectionPasses, vtable +0x3A0) at exe+0x37D8B70, IsIncrementalPurgePending exe+0x102F9D0,
// FApp::DeltaTime exe+0x8DAA340, GFrameCounter exe+0x903C610, GPerformGCWhileAsyncLoading exe+0x927E840, the IsAsyncLoading
// pointer exe+0x8DAA2E8; UEngine TimeSinceLastPendingKillPurge +0xD58, bShouldDelayGarbageCollect +0xD5D (nothing patched).
// Then the hook with a fake engine driven through the engine's own timer-path order, frame by frame.
#pragma once

namespace gchold_test {
float g_interval = 61.1f;
bool g_purge = false, g_async = false, g_frameKnown = true, g_menuKnown = true;
int32_t g_asyncVar = 0, g_menuMode = 0;
double g_dt = 1.0 / 60.0;
uint64_t g_frameNo = 1000, g_nowUs = 1000000, g_elapsedUs = 9000;
uint64_t g_unitUs = 0, g_stepUs = 16667;
bool g_tryFail = false;
int g_vcalls = 0;
alignas(16) uint8_t g_engine[0x1000];
void* g_vtable[0x200];
float GhInterval(void*, bool) { ++g_vcalls; return g_interval; }
bool GhPurge() { return g_purge; }
bool GhAsync() { return g_async; }
bool (*g_asyncPtr)() = &GhAsync;
uint64_t GhNow() { return g_nowUs; }
bool GhFrame(uint64_t* e, uint64_t* u) { if (!g_frameKnown) return false; *e = g_elapsedUs; *u = g_unitUs; return true; }
bool GhMenu(int32_t* n, int32_t* m) { if (!g_menuKnown) return false; *n = 0; *m = g_menuMode; return true; }
float& GhTsl() { return *reinterpret_cast<float*>(g_engine + 0xD58); }
uint8_t& GhFlag() { return g_engine[0xD5D]; }

void GhReset(int key, bool memShort = false)
{
    memset(g_engine, 0, sizeof g_engine);
    for (auto& v : g_vtable) v = nullptr;
    g_vtable[0x3A0 / 8] = reinterpret_cast<void*>(&GhInterval);
    *reinterpret_cast<void***>(g_engine) = g_vtable;
    g_interval = 61.1f; g_purge = g_async = false; g_frameKnown = g_menuKnown = true; g_asyncVar = 0; g_menuMode = 0;
    g_dt = 1.0 / 60.0; g_frameNo = 1000; g_nowUs = 1000000; g_elapsedUs = 9000; g_unitUs = 0; g_stepUs = 16667; g_tryFail = false; g_vcalls = 0;
    GcPassAfterBusyFakes k{};
    k.slot = 0x3A0; k.tslOff = 0xD58; k.flagOff = 0xD5D;
    k.purgePending = &GhPurge; k.deltaTime = &g_dt; k.frameCounter = &g_frameNo; k.asyncVar = &g_asyncVar; k.asyncFn = &g_asyncPtr;
    k.now = &GhNow; k.frame = &GhFrame; k.menu = &GhMenu;
    GcPassAfterBusyTestSetup(k);
    SetGcPassAfterBusy(key, memShort);
}

// One frame of ConditionalCollectGarbage's timer path (UE 5.3 order, as in the binary): 'D' delayed, 'P' pass, 'B' blocked by
// async loading, 'S' purge slice / nothing due. The frame counter and the clock advance afterwards.
char GhEngineFrame()
{
    GhTsl() = static_cast<float>(static_cast<double>(GhTsl()) + g_dt);
    const float interval = GcPassAfterBusyInterval(g_engine, false);
    char r;
    if (GhFlag()) { GhFlag() = 0; r = 'D'; }
    else if (!g_purge && GhTsl() > interval && interval > 0.0f) {
        if (g_asyncVar == 0 && g_async) r = 'B';
        else if (g_tryFail) r = 'F';  // TryCollectGarbage could not run (GC lock held): the timer stays, retried next frame
        else { GhTsl() = 0.0f; r = 'P'; }
    } else r = 'S';
    ++g_frameNo;
    g_nowUs += g_stepUs;
    return r;
}
// run frames until the pass ran (or n frames); returns the frames it was delayed ('D')
int GhUntilPass(int n, int* frames = nullptr)
{
    int d = 0, i = 0;
    for (; i < n; ++i) {
        const char r = GhEngineFrame();
        if (r == 'D') ++d;
        if (r == 'P') break;
    }
    if (frames) *frames = i;
    return i < n ? d : -1;
}
void GhToDue() { GhTsl() = g_interval - 0.02f; GhEngineFrame(); }  // one ordinary frame just before the pass falls due (continuity)
}  // namespace gchold_test

static void GcHoldRun(const GameModule& game)
{
    using namespace gchold_test;
    if (game.timeDateStamp == 0xF19077A4u) {
        uint32_t r[7] = {}, off[3] = {};
        const bool ok = GcPassAfterBusyTestDiscover(game, r, off);
        Check(ok && r[0] == 0x37D8A20 && r[1] == 0x37D8B70 && r[2] == 0x102F9D0 && r[3] == 0x8DAA340 && r[4] == 0x903C610 &&
                  r[5] == 0x927E840 && r[6] == 0x8DAA2E8 && off[0] == 0x3A0 && off[1] == 0xD58 && off[2] == 0xD5D,
              "GcPassAfterBusy: ConditionalCollectGarbage, its timer call, IsIncrementalPurgePending, DeltaTime, GFrameCounter, "
              "the async-loading gate and UEngine's timer / delay flag found on the mapped exe");
        if (!ok || r[1] != 0x37D8B70)
            printf("    found %d: %X %X %X %X %X %X %X, slot +%X tsl +%X flag +%X\n", ok, r[0], r[1], r[2], r[3], r[4], r[5], r[6],
                   off[0], off[1], off[2]);
        const uint8_t* call = game.base + 0x37D8B70;
        Check(call[0] == 0xFF && call[1] == 0x90, "GcPassAfterBusy: the mapped exe's timer call is still the game's (nothing patched)");
        // coexistence (rev98c M1): another hook's entry jump on ConditionalCollectGarbage (the lab's SpikeStages) must not stop
        // the discovery; written on the mapped copy (copy-on-write page) and put back
        uint8_t* const entry = game.base + 0x37D8A20;
        uint8_t saved[5];
        memcpy(saved, entry, 5);
        DWORD old = 0;
        if (VirtualProtect(entry, 5, PAGE_EXECUTE_WRITECOPY, &old) || VirtualProtect(entry, 5, PAGE_EXECUTE_READWRITE, &old)) {
            const uint8_t jmp[5] = {0xE9, 0x10, 0x20, 0x30, 0x00};
            memcpy(entry, jmp, 5);
            const bool chained = GcPassAfterBusyTestFindOn(game);
            entry[0] = 0xCC;  // neither the prologue nor a jump
            const bool odd = GcPassAfterBusyTestFindOn(game);
            memcpy(entry, saved, 5);
            DWORD o2;
            VirtualProtect(entry, 5, old, &o2);
            Check(chained && !odd && GcPassAfterBusyTestFindOn(game),
                  "GcPassAfterBusy: found with another hook's entry jump on ConditionalCollectGarbage (SpikeStages), refused with "
                  "an unknown entry, found again once restored");
        } else {
            Check(false, "GcPassAfterBusy: could not write the mapped copy for the coexistence test");
        }
    }
    {  // a build without the code: not found, so nothing is installed
        static uint8_t zero[0x4000];
        GameModule fake = game;
        fake.text = Section{zero, zero + sizeof zero};
        Check(!GcPassAfterBusyTestFindOn(fake), "GcPassAfterBusy: an exe without ConditionalCollectGarbage's timer path is refused");
    }
    {  // the entry check and the layout refusals (the values the code holds on 19115871, then each one altered)
        const uint8_t prologue[11] = {0x4C, 0x8B, 0xDC, 0x56, 0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00};
        const uint8_t jmp[11] = {0xE9, 1, 2, 3, 4, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        const uint8_t other[11] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x90};
        Check(GcPassAfterBusyTestEntry(prologue) && GcPassAfterBusyTestEntry(jmp) && !GcPassAfterBusyTestEntry(other),
              "GcPassAfterBusy: the entry is accepted as the game's prologue or a hook's jump, refused otherwise");
        Check(!GcPassAfterBusyTestLayout(0xD5C, 0xD58, 0x3A0, 0xD5D, 0xD5D) && GcPassAfterBusyTestLayout(0xD5C, 0xD58, 0x3A0, 0xD5E, 0xD5E) &&
                  GcPassAfterBusyTestLayout(0xD5C, 0xD58, 0x3A0, 0xD5D, 0xD5C) && GcPassAfterBusyTestLayout(0xD5C, 0xD54, 0x3A0, 0xD5D, 0xD5D) &&
                  GcPassAfterBusyTestLayout(0x60, 0x5C, 0x3A0, 0x61, 0x61) && GcPassAfterBusyTestLayout(0xD5C, 0xD58, 0x3A4, 0xD5D, 0xD5D) &&
                  GcPassAfterBusyTestLayout(0xD5C, 0xD58, 0x2000, 0xD5D, 0xD5D),
              "GcPassAfterBusy: the UE 5.3 layout is accepted; a moved delay flag, a flag compare and clear that differ, a timer not "
              "next to the full flag, offsets out of range and an odd or far vtable slot are refused");
        // the 6 bytes written: call [rip+rel32] reaching the slot; a slot out of rel32 reach is refused
        static uint8_t buf[64];
        uint8_t out[6] = {};
        const bool made = GcPassAfterBusyTestCall(buf + 8, buf + 40, out);
        int32_t rel = 0;
        memcpy(&rel, out + 2, 4);
        Check(made && out[0] == 0xFF && out[1] == 0x15 && buf + 8 + 6 + rel == buf + 40,
              "GcPassAfterBusy: the patch is FF 15 rel32 (call [rip+rel32]) and reaches the slot holding the hook");
        const uint8_t* const farSlot = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(buf) + 0x100000000ull);
        Check(!GcPassAfterBusyTestCall(buf, farSlot, out), "GcPassAfterBusy: a slot out of rel32 reach is refused");
    }
    uint64_t c[16];
    // key 0: inert (the flag is never written, the pass runs on the frame it falls due, busy or not)
    GhReset(0);
    GcPassAfterBusyNoteStream(0);
    GhToDue();
    g_elapsedUs = 30000; g_unitUs = 5000; GcPassAfterBusyNoteStream(1);
    int d = GhUntilPass(5);
    Check(d == 0 && g_vcalls >= 2, "GcPassAfterBusy: key 0 never delays a pass (the virtual is still called and its value returned)");
    // busy (a cell added this frame): delayed while busy, runs on the first calm frame
    GhReset(1);
    GhToDue();
    int frames = 0;
    for (int i = 0; i < 4; ++i) {
        GcPassAfterBusyNoteStream(0);
        const char r = GhEngineFrame();
        if (r == 'D') ++frames;
    }
    // the frame after the last note still counts as busy (this or the previous frame); then calm
    d = GhUntilPass(10);
    GcPassAfterBusyTestCounts(c);
    Check(frames == 4 && d == 1 && c[0] == 1 && c[2] == 1 && c[3] == 5 && c[5] == 1 && c[13] == 5,
          "GcPassAfterBusy: a due pass waits through busy frames (cell added) and runs on the first calm frame");
    // the pass ran: the timer starts again and the next due pass may wait again
    GhTsl() = g_interval - 0.02f; GhEngineFrame();
    g_unitUs = 3000;  // 3 ms of character steps ran in this frame
    char r1 = GhEngineFrame();
    g_unitUs = 400;   // a small step only: not busy
    char r2 = GhEngineFrame();
    GcPassAfterBusyTestCounts(c);
    Check(r1 == 'D' && r2 == 'P' && c[2] == 2,
          "GcPassAfterBusy: character steps run in the frame (>= 1 ms) count as busy, a small one does not; the next pass waits again");
    // an AddToWorld call counts only when it did work (its own time >= 50 us)
    GhReset(1);
    GhToDue();
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    GcPassAfterBusyNoteAdd(qf.QuadPart / 1000000 * 5);  // 5 us: a call that returned at once
    char a1 = GhEngineFrame();
    GhTsl() = g_interval - 0.02f; GhEngineFrame();
    GcPassAfterBusyNoteAdd(qf.QuadPart / 1000);  // 1 ms: a step
    char a2 = GhEngineFrame();
    Check(a1 == 'P' && a2 == 'D', "GcPassAfterBusy: an AddToWorld call without work does not make the frame busy, a step does");
    // TryCollectGarbage fails on the release frame (GC lock held): the engine retries next frame; no second hold
    GhReset(1);
    GhToDue();
    GcPassAfterBusyNoteStream(0);
    char f1 = GhEngineFrame();
    char f1b = GhEngineFrame();  // the previous frame's step still counts: held once more
    g_tryFail = true;
    char f2 = GhEngineFrame();
    GcPassAfterBusyNoteStream(0);  // busy again: still not held a second time
    g_tryFail = false;
    char f3 = GhEngineFrame();
    GcPassAfterBusyTestCounts(c);
    Check(f1 == 'D' && f1b == 'D' && f2 == 'F' && f3 == 'P' && c[2] == 1, "GcPassAfterBusy: a failed TryCollectGarbage is retried by the engine, not held again");
    // key switched 1 -> 0 mid-hold: the pass runs at once
    GhReset(1);
    GhToDue();
    g_elapsedUs = 20000;
    GhEngineFrame(); GhEngineFrame();
    SetGcPassAfterBusy(0, false);
    char k1 = GhEngineFrame();
    GcPassAfterBusyTestCounts(c);
    Check(k1 == 'P' && c[12] == 1, "GcPassAfterBusy: switched off during a hold, the pass runs on the next frame");
    // one long frame inside a hold: the wall-time bound releases on the next frame
    GhReset(1);
    GhToDue();
    g_elapsedUs = 20000;
    g_stepUs = 600000;
    char l1 = GhEngineFrame(), l2 = GhEngineFrame();
    GcPassAfterBusyTestCounts(c);
    Check(l1 == 'D' && l2 == 'P' && c[6] == 1, "GcPassAfterBusy: a frame longer than the bound inside a hold releases the pass on the next frame");
    // async loading starts in the middle of a hold: the engine holds the pass; it runs once loading ends and the frame is calm
    GhReset(1);
    GhToDue();
    GcPassAfterBusyNoteStream(1);
    char x1 = GhEngineFrame();
    g_async = true;
    char x2 = GhEngineFrame();
    g_async = false;
    char x3 = GhEngineFrame();
    Check(x1 == 'D' && x2 == 'B' && x3 == 'P', "GcPassAfterBusy: async loading during a hold is the engine's wait; the pass runs after it");
    // the bound: busy for ever (frame over the calm limit) -> runs once 500 ms are over
    GhReset(1);
    GhToDue();
    g_elapsedUs = 20000;
    int at = 0;
    d = GhUntilPass(200, &at);
    GcPassAfterBusyTestCounts(c);
    Check(d >= 29 && d <= 31 && c[6] == 1 && c[15] == static_cast<uint64_t>(d),
          "GcPassAfterBusy: a pass busy for ever runs when the 500 ms bound is over (about 30 frames at 60 fps)");
    GhReset(200);
    GhToDue();
    g_elapsedUs = 20000;
    d = GhUntilPass(200);
    Check(d >= 11 && d <= 13, "GcPassAfterBusy: GcPassAfterBusy=200 bounds the wait to 200 ms");
    // never delayed: forced (the timer written by ForceGarbageCollection), also in the middle of a hold
    GhReset(1);
    GhToDue();
    g_elapsedUs = 20000;
    GhTsl() = 1.0f + g_interval;  // ForceGarbageCollection(false)
    d = GhUntilPass(3);
    GcPassAfterBusyTestCounts(c);
    Check(d == 0 && c[7] == 1, "GcPassAfterBusy: a forced pass is never delayed");
    GhReset(1);
    GhToDue();
    g_elapsedUs = 20000;
    GhEngineFrame(); GhEngineFrame();
    GhTsl() = 1.0f + g_interval;
    d = GhUntilPass(3);
    GcPassAfterBusyTestCounts(c);
    Check(d == 0 && c[7] == 1 && c[2] == 1, "GcPassAfterBusy: a pass forced during a hold runs on that frame");
    // never delayed: menu or loading screen up, its state unknown, memory short
    GhReset(1); GhToDue(); g_elapsedUs = 20000; g_menuMode = 1;
    d = GhUntilPass(3); GcPassAfterBusyTestCounts(c);
    Check(d == 0 && c[8] == 1, "GcPassAfterBusy: never delayed while a menu or loading screen is up");
    GhReset(1); GhToDue(); g_elapsedUs = 20000; g_menuKnown = false;
    d = GhUntilPass(3); GcPassAfterBusyTestCounts(c);
    Check(d == 0 && c[10] == 1, "GcPassAfterBusy: never delayed when the menu / loading state is not known");
    GhReset(1, true); GhToDue(); g_elapsedUs = 20000;
    d = GhUntilPass(3); GcPassAfterBusyTestCounts(c);
    Check(d == 0 && c[9] == 1, "GcPassAfterBusy: never delayed while Windows is short of memory");
    // the purge's slices are never touched; async loading's own wait is not ours
    GhReset(1); GhToDue(); g_elapsedUs = 20000; g_purge = true;
    char s1 = GhEngineFrame(), s2 = GhEngineFrame();
    GcPassAfterBusyTestCounts(c);
    Check(s1 == 'S' && s2 == 'S' && c[0] == 0, "GcPassAfterBusy: while a purge is pending the flag is never set (its slices run)");
    GhReset(1); GhToDue(); g_elapsedUs = 20000; g_async = true;
    char b1 = GhEngineFrame();
    g_async = false; g_elapsedUs = 9000;
    char b2 = GhEngineFrame();
    GcPassAfterBusyTestCounts(c);
    Check(b1 == 'B' && b2 == 'P' && c[1] == 1, "GcPassAfterBusy: a pass held by the engine for async loading is left to the engine");
    // the game's own delay: left alone, not counted as ours
    GhReset(1); GhToDue(); g_elapsedUs = 20000;
    GhFlag() = 1;
    char g1 = GhEngineFrame();
    GcPassAfterBusyTestCounts(c);
    Check(g1 == 'D' && c[0] == 0 && c[2] == 0, "GcPassAfterBusy: the game's own DelayGarbageCollection is left alone");
    // not busy: runs on the frame it falls due
    GhReset(1); GhToDue();
    d = GhUntilPass(3, &at); GcPassAfterBusyTestCounts(c);
    Check(d == 0 && at == 0 && c[1] == 1, "GcPassAfterBusy: a due pass on a calm frame runs at once");
    LogGcPassAfterBusyStats();
    GhReset(0);
    GcPassAfterBusyTestTeardown();  // later tests see the mapped exe, not these fakes (review rev-aheadgc SF1)
}
