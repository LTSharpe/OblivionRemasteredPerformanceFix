// StreamRegisterGranularity self-test (worker bodyadd; review fixes worker granfix; public since 1.3.0), included by
// selftest.cpp: on the mapped exe the cvar's bound int (registration + its only read, the game's 10) and UWorld::AddToWorld
// (the read's function, its first bytes; exe+0x3853B80 on Steam 19115871), nothing installed; then the per-call rules with
// fake console variables (IConsoleVariable vtable: GetFlags +0x18, Set +0x80, GetInt +0x90, GetFloat +0x98; Set keeps
// the engine's priority rule): the key's value through Set at the project priority, the game's 10 in loading mode and
// with key 0, re-applied after the engine's 10, a live key change, values of a higher priority or a project-priority
// value other than 10 left alone, calls without a time limit untouched.
#pragma once

namespace streamgran_test {
struct SgCvar {
    void** vt;
    int32_t i;
    float f;
    uint32_t flags;
    int sets;
};
uint32_t GetFlags(void* c) { return static_cast<SgCvar*>(c)->flags; }
void Set(void* c, const wchar_t* v, uint32_t by)
{
    SgCvar* const f = static_cast<SgCvar*>(c);
    if ((by & 0xFF000000u) < (f->flags & 0xFF000000u)) return;  // the engine's CanChange
    f->i = _wtoi(v);
    f->f = static_cast<float>(_wtof(v));
    f->flags = by & 0xFF000000u;
    ++f->sets;
}
int32_t GetInt(void* c) { return static_cast<SgCvar*>(c)->i; }
float GetFloat(void* c) { return static_cast<SgCvar*>(c)->f; }
void* g_sgVt[32];
}  // namespace streamgran_test

static void StreamGranRun()
{
    using streamgran_test::SgCvar; using streamgran_test::g_sgVt; using streamgran_test::Set;
    int32_t* const v = FindStreamRegisterGranularityVar();
    Check(v && *v == 10, "streamgran: s.LevelStreamingComponentsRegistrationGranularity's int found (registration + its only read), the game's 10");
    uint8_t* const add = FindAddToWorld(true);
    Check(add != nullptr, "streamgran: UWorld::AddToWorld found as the function of the read, its first bytes as expected");
    if (add && Game().timeDateStamp == 0xF19077A4u)
        Check(add - Game().base == 0x3853B80, "streamgran: AddToWorld at exe+0x3853B80 on Steam 19115871 (BodyAddProbe's address)");

    g_sgVt[0x18 / 8] = reinterpret_cast<void*>(&streamgran_test::GetFlags);
    g_sgVt[0x80 / 8] = reinterpret_cast<void*>(&streamgran_test::Set);
    g_sgVt[0x90 / 8] = reinterpret_cast<void*>(&streamgran_test::GetInt);
    g_sgVt[0x98 / 8] = reinterpret_cast<void*>(&streamgran_test::GetFloat);
    SgCvar gran{g_sgVt, 10, 10.0f, 0x03000000u, 0};   // the game's 10 from UStreamingSettings (project priority)
    SgCvar limit{g_sgVt, 4, 4.0f, 0x03000000u, 0};    // s.LevelStreamingActorsUpdateTimeLimit, a few ms
    StreamGranTestSetup(&gran, &limit);

    SetStreamRegisterGranularity(0);
    StreamGranBeforeAdd(true);
    Check(gran.i == 10 && gran.sets == 0, "streamgran: key 0 leaves the game's 10 (no Set)");
    SetStreamRegisterGranularity(1);
    StreamGranBeforeAdd(false);
    Check(gran.i == 10 && gran.sets == 0, "streamgran: a call without a time limit is not touched");
    StreamGranBeforeAdd(true);
    Check(gran.i == 1 && gran.flags == 0x03000000u && gran.sets == 1, "streamgran: key 1 sets 1 through Set at the project priority");
    StreamGranBeforeAdd(true);
    Check(gran.sets == 1, "streamgran: no Set while the value is already right");
    Check(StreamLoadingModeNow() == 0, "streamgran: StreamLoadingModeNow reads 0 in play (WaterInfoAfterCrossing)");
    limit.f = 10000.0f;  // Altar's loading mode
    Check(StreamLoadingModeNow() == 1, "streamgran: StreamLoadingModeNow reads 1 in Altar's loading mode");
    StreamGranBeforeAdd(true);
    Check(gran.i == 10, "streamgran: loading mode (time limit 10 s): the game's 10");
    StreamGranBeforeAdd(true);
    Check(gran.i == 10 && gran.sets == 2, "streamgran: loading mode: stays 10, no further Set");
    limit.f = 4.0f;
    StreamGranBeforeAdd(true);
    Check(gran.i == 1, "streamgran: loading mode over: the key's 1 again");
    SetStreamRegisterGranularity(2);
    StreamGranBeforeAdd(true);
    Check(gran.i == 2, "streamgran: a live change of the key (1 -> 2) is applied at the next add");
    Set(&gran, L"10", 0x03000000u);  // the engine's settings export writes the game's 10 again
    StreamGranBeforeAdd(true);
    Check(gran.i == 2, "streamgran: re-applied after the engine wrote its 10 again");
    SetStreamRegisterGranularity(0);
    StreamGranBeforeAdd(true);
    Check(gran.i == 10 && gran.flags == 0x03000000u, "streamgran: key 0 puts the game's 10 back (project priority)");
    StreamGranBeforeAdd(true);
    const int setsOff = gran.sets;
    Check(gran.i == 10, "streamgran: key 0 stays off");

    SetStreamRegisterGranularity(1);
    Set(&gran, L"10", 0x0B000000u);  // the player types 10 in the console (or the bench's -Cvars)
    StreamGranBeforeAdd(true);
    Check(gran.i == 10 && gran.sets == setsOff + 1, "streamgran: a console 10 (higher priority) is left alone");
    Set(&gran, L"3", 0x04000000u);   // would be refused: console beats SystemSettingsIni
    Check(gran.i == 10, "streamgran: fake keeps the engine's priority rule");
    limit.f = 10000.0f;
    StreamGranBeforeAdd(true);
    limit.f = 4.0f;
    StreamGranBeforeAdd(true);
    Check(gran.i == 10 && gran.flags == 0x0B000000u, "streamgran: a console value is not touched across a loading screen either");

    SgCvar gran2{g_sgVt, 5, 5.0f, 0x03000000u, 0};  // a project-priority 5 (Engine.ini [/Script/Engine.StreamingSettings])
    StreamGranTestSetup(&gran2, &limit);
    SetStreamRegisterGranularity(1);
    StreamGranBeforeAdd(true);
    Check(gran2.i == 5 && gran2.sets == 0, "streamgran: a project-priority value other than 10 is left alone");
    SgCvar gran3{g_sgVt, 2, 2.0f, 0x04000000u, 0};  // Engine.ini [SystemSettings] 2
    StreamGranTestSetup(&gran3, &limit);
    SetStreamRegisterGranularity(1);
    StreamGranBeforeAdd(true);
    Check(gran3.i == 2 && gran3.sets == 0, "streamgran: an Engine.ini [SystemSettings] value is left alone");
    SetStreamRegisterGranularity(10);  // out of range = off
    StreamGranBeforeAdd(true);
    Check(gran3.i == 2, "streamgran: out-of-range key = off");
    SetStreamRegisterGranularity(0);

    // StreamInBudget (gatefix 2026-11-12): the AddToWorld slice max and the async loading limit, through the same hook
    {
        SgCvar g4{g_sgVt, 10, 10.0f, 0x03000000u, 0};
        SgCvar lim{g_sgVt, 4, 4.0f, 0x03000000u, 0};
        SgCvar slice{g_sgVt, 4, 4.0f, 0x04000000u, 0};  // the game's 4 ms (an ini priority, above the project's)
        SgCvar async{g_sgVt, 5, 5.0f, 0x03000000u, 0};  // the game's 5 ms (project setting)
        StreamGranTestSetup(&g4, &lim);
        StreamBudgetTestSetup(&slice, &async);
        StreamGranBeforeAdd(true);
        Check(slice.f == 4.0f && async.f == 5.0f && slice.sets == 0 && async.sets == 0 && g4.sets == 0,
              "streambudget: key 0 leaves the game's 4 / 5 ms (no Set), granularity key 0 untouched");
        SetStreamInBudget(1);
        StreamGranBeforeAdd(false);
        Check(slice.sets == 0 && async.sets == 0, "streambudget: a call without a time limit is not touched");
        StreamGranBeforeAdd(true);
        Check(slice.f == 1.0f && async.f == 3.0f && slice.flags == 0x04000000u && async.flags == 0x03000000u && g4.sets == 0,
              "streambudget: key 1 sets slice 1 / async 3 ms at each variable's own priority; the granularity untouched");
        StreamGranBeforeAdd(true);
        Check(slice.sets == 1 && async.sets == 1, "streambudget: no Set while the values are right");
        lim.f = 10000.0f;  // Altar's loading mode
        StreamGranBeforeAdd(true);
        Check(slice.f == 4.0f && async.f == 5.0f, "streambudget: loading mode: the game's 4 / 5 ms");
        lim.f = 4.0f;
        SetStreamInBudget(2);
        StreamGranBeforeAdd(true);
        Check(slice.f == 2.0f && async.f == 3.0f, "streambudget: loading over, live key 2: slice 2 / async 3 ms");
        SetStreamInBudget(0);
        StreamGranBeforeAdd(true);
        Check(slice.f == 4.0f && async.f == 5.0f, "streambudget: live key 0 puts the game's 4 / 5 ms back");
        const int budgetSetsOff = slice.sets + async.sets;
        StreamGranBeforeAdd(true);
        Check(slice.sets + async.sets == budgetSetsOff, "streambudget: key 0 stays off (no further Set)");
        SetStreamInBudget(1);
        Set(&async, L"5", 0x0B000000u);  // the player's console 5
        Set(&slice, L"6", 0x04000000u);  // the player's Engine.ini [SystemSettings] 6
        StreamGranBeforeAdd(true);
        Check(async.f == 5.0f && slice.f == 6.0f, "streambudget: a console value and another ini value are left alone");
        SetStreamInBudget(7);  // out of range = off
        StreamGranBeforeAdd(true);
        Check(async.f == 5.0f && slice.f == 6.0f, "streambudget: out-of-range key = off");
        SetStreamInBudget(0);
        StreamBudgetTestSetup(nullptr, nullptr);
    }
    StreamGranTestSetup(nullptr, nullptr);
    Check(StreamLoadingModeNow() == -1, "streamgran: StreamLoadingModeNow reads -1 (unknown) without the hook / variables");
}

// StreamInHeavySkipMs self-test (worker heavyskip 2026-11-23, review fixes the same day; S-028): (0) UWorld's three fields
// from AddToWorld's code on the mapped exe (each pattern once, cross-checked; refused on a changed copy); (1) the decision
// table on the rule itself (key off, no time limit, loading, nothing pending, another level pending, below the key, below the
// median + 4, only the pending level's call of a heavy frame, its second call in the same frame (a flush loop), the frame
// after a skip, the per-level cap, a fresh count once another level or none is pending); (2) a 41-frame train with the
// game-thread times at the AddToWorld calls estimated from play114d f 58650-58690 (4 calls a frame, one level pending): at
// most the pending level's call skipped, never two skip frames in a row, at most the cap; (3) no frame clock: never a skip,
// one log line; (4) the entry's world read (pending, bMatchStarted, WorldType) and its clean off paths.
static void StreamHeavySkipRun()
{
    // (0) the fields on the mapped exe
    {
        const uint8_t* const add = FindAddToWorld(true);
        StreamHeavySkipFields f;
        const bool ok = add && StreamHeavySkipFindFields(add, 0x300, &f);
        Check(ok, "heavyskip: UWorld's pending-visibility field, WorldType and bMatchStarted found in AddToWorld's code (each once, cross-checked)");
        if (ok && Game().timeDateStamp == 0xF19077A4u)
            Check(f.pending == 0xE0 && f.worldType == 0x13A && f.matchStarted == 0x13D && f.matchMask == 2,
                  "heavyskip: CurrentLevelPendingVisibility +0xE0, WorldType +0x13A, bMatchStarted +0x13D bit 2 on Steam 19115871");
        if (add) {
            static uint8_t copy[0x300];
            memcpy(copy, add, sizeof copy);
            StreamHeavySkipFields g;
            Check(StreamHeavySkipFindFields(copy, sizeof copy, &g) && g.pending == f.pending, "heavyskip: (a copy of the code is found the same)");
            // the begin block's write (mov [r13+pending],r14) changed: refused
            const uint8_t st[7] = {0x4D, 0x89, 0xB5, static_cast<uint8_t>(f.pending), static_cast<uint8_t>(f.pending >> 8),
                                   static_cast<uint8_t>(f.pending >> 16), static_cast<uint8_t>(f.pending >> 24)};
            uint8_t* const store = std::search(copy, copy + sizeof copy, st, st + 7);
            bool refusedStore = false, refusedTwice = false;
            if (store != copy + sizeof copy) {
                store[1] = 0x8B;
                refusedStore = !StreamHeavySkipFindFields(copy, sizeof copy, &g) && !g.ok;
            }
            // the pending pattern (56 bytes from mov rcx,[r13+pending]) a second time near the end of the window: refused
            memcpy(copy, add, sizeof copy);
            const uint8_t ld[7] = {0x49, 0x8B, 0x8D, st[3], st[4], st[5], st[6]};
            uint8_t* const load = std::search(copy, copy + 0x200, ld, ld + 7);
            if (load != copy + 0x200) {
                memcpy(copy + 0x2C0, load, 56);
                refusedTwice = !StreamHeavySkipFindFields(copy, sizeof copy, &g);
            }
            Check(refusedStore && refusedTwice, "heavyskip: code not as expected (the begin block's write changed / a pattern twice): refused");
            uint8_t zero[0x300] = {};
            Check(!StreamHeavySkipFindFields(zero, sizeof zero, &g) && !StreamHeavySkipFindFields(nullptr, 0x300, &g),
                  "heavyskip: other bytes / no function: refused");
        }
    }

    // (1) the rule
    int lv[40];
    const void* A = &lv[0];
    const void* B = &lv[1];
    uint64_t f = 1000;
    StreamHeavySkipTestReset(0, 8);
    Check(!StreamHeavySkipTestCall(A, A, true, false, true, f, 40.0), "heavyskip: key 0: never a skip");
    StreamHeavySkipTestReset(17, 8);
    Check(!StreamHeavySkipTestCall(A, A, false, false, true, f, 40.0),
          "heavyskip: a call whose time limit stock turns off (blocking, match not started, not a game world) is never skipped");
    Check(!StreamHeavySkipTestCall(A, A, true, true, true, f, 40.0), "heavyskip: never in the loading mode");
    Check(!StreamHeavySkipTestCall(A, nullptr, true, false, true, ++f, 40.0),
          "heavyskip: nothing pending (the call may begin making a level visible): never skipped");
    Check(!StreamHeavySkipTestCall(A, B, true, false, true, ++f, 40.0),
          "heavyskip: another level pending (a no-op in stock): never skipped");
    Check(StreamHeavySkipTestRead().skipFrames == 0 && StreamHeavySkipTestRead().declinedInRow == 0,
          "heavyskip: (those calls made no decision)");
    StreamHeavySkipTestReset(17, 8);
    Check(!StreamHeavySkipTestCall(A, A, true, false, true, ++f, 16.0), "heavyskip: below the key (16 < 17 ms): the call goes on");
    Check(!StreamHeavySkipTestCall(A, A, true, false, true, ++f, 18.0), "heavyskip: 18 ms but below the median + 4 (16 + 4): goes on");
    ++f;
    const bool s1 = StreamHeavySkipTestCall(B, A, true, false, true, f, 25.0);  // another level's call first: not ours
    const bool s2 = StreamHeavySkipTestCall(A, A, true, false, true, f, 25.4);
    const bool s3 = StreamHeavySkipTestCall(B, A, true, false, true, f, 25.5);
    Check(!s1 && s2 && !s3, "heavyskip: 25 ms >= max(17, median 18 + 4): only the pending level's call is skipped");
    Check(!StreamHeavySkipTestCall(A, A, true, false, true, f, 26.0) && StreamHeavySkipTestRead().declinedRepeat == 1,
          "heavyskip: the pending level called again in the same frame (a flush / block loop's next pass) goes on");
    Check(!StreamHeavySkipTestCall(A, A, true, false, true, f, 26.5) && StreamHeavySkipTestRead().declinedRepeat == 1,
          "heavyskip: (a third pass goes on, counted once)");
    Check(!StreamHeavySkipTestCall(A, A, true, false, true, ++f, 30.0) && StreamHeavySkipTestRead().declinedInRow == 1,
          "heavyskip: never two skip frames in a row (also after a flush loop's repeat)");
    Check(StreamHeavySkipTestCall(A, A, true, false, true, ++f, 30.0), "heavyskip: the frame after the one after a skip may skip again");
    {
        const StreamHeavySkipTestStats st = StreamHeavySkipTestRead();
        Check(st.skipFrames == 2 && st.fromMedian == 2 && st.maxPerLevel == 2 && st.pendingSkips == 2,
              "heavyskip: counted 2 skip frames, both thresholds from the median + 4, the pending level delayed 2 frames");
    }
    // per-level cap 2 (one heavy frame in three, so the median stays at the quiet frames' 5 ms)
    StreamHeavySkipTestReset(17, 2);
    f = 5000;
    int skips = 0;
    for (int i = 0; i < 12; ++i) skips += StreamHeavySkipTestCall(A, A, true, false, true, ++f, i % 3 == 0 ? 40.0 : 5.0) ? 1 : 0;
    Check(skips == 2 && StreamHeavySkipTestRead().declinedCap == 2, "heavyskip: at most StreamInHeavySkipMax (2) skip frames for a pending level");
    // the count starts afresh once another level (or none) is seen pending
    StreamHeavySkipTestCall(A, nullptr, true, false, true, ++f, 5.0);  // A finished; the next call begins a level
    StreamHeavySkipTestCall(A, A, true, false, true, ++f, 5.0);
    Check(StreamHeavySkipTestRead().pendingSkips == 0 && StreamHeavySkipTestCall(A, A, true, false, true, ++f, 40.0),
          "heavyskip: a level pending again after nothing was pending starts afresh");
    StreamHeavySkipTestCall(B, B, false, false, true, ++f, 5.0);  // B pending (seen at a blocking call too)
    Check(StreamHeavySkipTestRead().pendingSkips == 0, "heavyskip: another level pending: a fresh count");

    // (2) the play114d train, 4 levels a frame, lv[0] pending
    static const float kEl[41] = {15.7f, 22.4f, 14.2f, 13.6f, 15.7f, 17.3f, 15.6f, 20.3f, 15.0f, 13.4f, 13.0f, 13.6f, 13.2f, 13.0f,
                                  14.4f, 13.2f, 15.0f, 19.3f, 22.8f, 18.5f, 16.0f, 23.5f, 16.5f, 14.5f, 16.1f, 14.8f, 15.3f, 15.5f,
                                  15.8f, 23.3f, 15.8f, 13.7f, 13.8f, 13.2f, 14.9f, 15.0f, 15.0f, 17.9f, 17.2f, 14.2f, 19.4f};
    StreamHeavySkipTestReset(17, 8);
    f = 58650;
    bool prev = false, inRow = false, other = false;
    int frames = 0;
    for (int i = 0; i < 41; ++i) {
        ++f;
        int n = 0;
        for (int k = 0; k < 4; ++k) {
            const bool s = StreamHeavySkipTestCall(&lv[k], &lv[0], true, false, true, f, kEl[i] + 0.05 * k);
            n += s ? 1 : 0;
            if (s && k != 0) other = true;
        }
        if (n && prev) inRow = true;
        prev = n != 0;
        frames += n ? 1 : 0;
    }
    {
        const StreamHeavySkipTestStats st = StreamHeavySkipTestRead();
        Check(frames >= 3 && !inRow && !other && st.pendingSkips <= 8 && st.skipFrames == static_cast<unsigned>(frames),
              "heavyskip: play114d train: some frames skipped, never two in a row, only the pending level's call, <= 8");
    }

    // (3) no frame clock: never a skip, one log line
    StreamHeavySkipTestReset(17, 8);
    bool any = false;
    for (int i = 0; i < 3; ++i) any = StreamHeavySkipTestCall(A, A, true, false, false, 0, 40.0) || any;
    Check(!any && StreamHeavySkipTestRead().noClockLogs == 1, "heavyskip: no frame clock: never a skip, logged once");

    // (4) the entry: its world read, and clean off
    {
        alignas(8) static uint8_t world[0x200];
        memset(world, 0, sizeof world);
        const StreamHeavySkipFields fields{0xE0, 0x13A, 0x13D, 2, true};
        StreamHeavySkipTestFields(&fields);
        const void* pend = nullptr;
        bool tl = false;
        memcpy(world + 0xE0, &A, sizeof A);
        world[0x13A] = 1;     // EWorldType::Game
        world[0x13D] = 0x02;  // bMatchStarted
        Check(StreamHeavySkipTestReadWorld(world, &pend, &tl) && pend == A && tl, "heavyskip: entry reads the pending level, a started game world");
        world[0x13D] = 0xFD;  // every other bit, not bMatchStarted
        Check(StreamHeavySkipTestReadWorld(world, &pend, &tl) && !tl, "heavyskip: entry: match not started = time limit off (as stock)");
        world[0x13D] = 0x02;
        world[0x13A] = 2;     // EWorldType::Editor
        Check(StreamHeavySkipTestReadWorld(world, &pend, &tl) && !tl, "heavyskip: entry: not a game world = time limit off (as stock)");
        world[0x13A] = 3;     // PIE
        bool okPie = StreamHeavySkipTestReadWorld(world, &pend, &tl) && tl;
        world[0x13A] = 7;     // Inactive
        Check(okPie && StreamHeavySkipTestReadWorld(world, &pend, &tl) && !tl, "heavyskip: entry: PIE is a game world, Inactive is not");
        world[0x13A] = 1;

        using streamgran_test::SgCvar; using streamgran_test::g_sgVt;
        SgCvar gran{g_sgVt, 10, 10.0f, 0x03000000u, 0};
        SgCvar limit{g_sgVt, 4, 4.0f, 0x03000000u, 0};
        StreamGranTestSetup(&gran, &limit);
        StreamHeavySkipTestReset(0, 8);
        SetStreamInHeavySkip(5, 8);  // out of range = off
        Check(!StreamHeavySkipBeforeAdd(world, A, true), "heavyskip: key out of range (5): off");
        SetStreamInBudget(0);
        SetStreamInHeavySkip(17, 8);
        Check(!StreamHeavySkipBeforeAdd(world, A, true), "heavyskip: StreamInBudget 0: off (logged)");
        SetStreamInBudget(1);
        limit.f = 10000.0f;  // Altar's loading mode
        Check(!StreamHeavySkipBeforeAdd(world, A, true), "heavyskip: the real entry in Altar's loading mode: no skip");
        limit.f = 4.0f;
        Check(!StreamHeavySkipBeforeAdd(world, A, false), "heavyskip: the real entry, a call without the time limit: no skip");
        StreamHeavySkipTestFields(nullptr);
        Check(!StreamHeavySkipBeforeAdd(world, A, true), "heavyskip: the real entry without the fields (pattern not found): no skip");
        SetStreamInHeavySkip(0, 8);
        SetStreamInBudget(0);
        StreamGranTestSetup(nullptr, nullptr);
        StreamHeavySkipTestReset(0, 8);
    }
}
