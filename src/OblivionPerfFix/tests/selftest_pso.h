// PsoPrecompileFast self-test, included by selftest.cpp: the sites on the mapped exe (SetBatchMode, the singleton
// it checks, the batch statics, IsPrecompiling's flag), the entry jump and the trampoline (run for real on the mapped exe:
// its singleton is null there, so the moved comparison sends it straight to the function's return), then the hook's and the
// game-thread timer's choices with a fake SetBatchMode, a fake cache object and a fake freeze subsystem: Fast instead of
// Background only while the precompile runs outside play; the game's mode back in play, when the precompile ends and when
// switched off; Fast and Precompile requests unchanged; a request from another thread follows the game thread's last look.
#pragma once

#include <atomic>
#include <cstring>
#include <thread>

#include <tlhelp32.h>

namespace pso {
int lastMode = -1, sets = 0;
void FakeSetBatchMode(int mode)
{
    lastMode = mode;
    ++sets;
}
uint8_t cache[0x400];       // FShaderPipelineCache: the flag at info.flagOffset
uint8_t freeze[0x200];      // the freeze subsystem: mode / named-freeze count at the fade hook's offsets
bool freezeThere = true;
void* FakeGetFreeze(void*) { return freezeThere ? freeze : nullptr; }

static void Run(const GameModule& game)
{
    LoadLibraryW(L"user32.dll");   // the fix needs user32's timers (the game has user32 loaded)
    Check(InstallPsoPrecompileFast(true), "pso precompile: SetBatchMode, its three cases and IsPrecompiling found, entry detoured");
    PsoPrecompileInfoOut info{};
    if (!Installed(PsoPrecompileInfo(&info), "PsoPrecompileFast (pso precompile)")) return;
    Check(info.setBatchMode == game.base + 0x2702390, "SetBatchMode at exe+0x2702390");
    Check(reinterpret_cast<uint8_t*>(info.singleton) == game.base + 0x921A230, "the FShaderPipelineCache singleton at exe+0x921A230");
    Check(info.flagOffset == 0x140, "the precompile flag (IsPrecompiling) at +0x140");
    Check(reinterpret_cast<uint8_t*>(info.batchSize) == game.base + 0x921A238 && reinterpret_cast<uint8_t*>(info.batchTime) == game.base + 0x921A23C,
          "the batch size / time statics at exe+0x921A238 / +0x921A23C (all three cases store the same size)");
    int32_t rel = 0;
    memcpy(&rel, info.setBatchMode + 1, 4);
    const uint8_t* stub = info.setBatchMode + 5 + rel;
    void* stubTarget = nullptr;
    memcpy(&stubTarget, stub + 6, 8);
    Check(info.setBatchMode[0] == 0xE9 && stub[0] == 0xFF && stub[1] == 0x25 && stubTarget == info.hook,
          "the entry jumps to a stub, the stub to the hook");
    Check(info.setBatchMode[5] == 0x83 && info.setBatchMode[6] == 0x3D, "the entry's comparison after the jump left as it was");
    const uint8_t* t = info.trampoline;
    void* back = nullptr;
    memcpy(&back, t + 12 + 6, 8);
    Check(t[0] == 0x48 && t[1] == 0x83 && t[2] == 0xEC && t[3] == 0x28 && t[4] == 0x48 && t[5] == 0x83 && t[6] == 0x3D && t[11] == 0 &&
              RipTarget(t + 4, 3, 8) == reinterpret_cast<uint8_t*>(info.singleton) && t[12] == 0xFF && t[13] == 0x25 &&
              back == info.setBatchMode + 12,
          "the trampoline: sub rsp,28h; cmp [singleton],0 re-targeted; jmp back to SetBatchMode+12");
    Check(*info.singleton == nullptr, "the mapped exe's singleton is null");
    const uint32_t sizeBefore = *info.batchSize;
    reinterpret_cast<void (*)(int)>(info.trampoline)(1);
    Check(*info.batchSize == sizeBefore, "the trampoline runs on the mapped exe and returns at once (no cache: nothing set)");

    // the decisions, with fakes
    PsoPrecompileTestOriginal(reinterpret_cast<void*>(&FakeSetBatchMode));
    FadeHookSites f{};
    FadeHookInfo(&f);
    FadeHookTestFunctions(nullptr, nullptr, reinterpret_cast<void*>(&FakeGetFreeze));
    const auto state = [&](int32_t named, int32_t mode) {
        *reinterpret_cast<int32_t*>(freeze + f.stackNumOffset) = named;
        *reinterpret_cast<int32_t*>(freeze + f.modeOffset) = mode;
    };
    const auto running = [&](bool on) { cache[info.flagOffset] = on ? 1 : 0; };
    *info.singleton = cache;
    const auto hook = reinterpret_cast<void (*)(int)>(info.hook);
    const auto tick = [&] { reinterpret_cast<TIMERPROC>(info.timerProc)(nullptr, WM_TIMER, 0, 0); };
    const auto now = [&] { PsoPrecompileInfoOut o{}; PsoPrecompileInfo(&o); return o; };
    SetPsoPrecompileFast(true);

    running(false);
    state(1, 0);   // the main menu (a named freeze), no precompile yet
    hook(0);
    Check(lastMode == 0 && now().applied == 0 && now().timerRunning, "no precompile: the game's Background passes; the timer starts");
    running(true);
    tick();
    Check(lastMode == 1 && now().applied == 1 && now().switchesFast == 1, "precompile starts in the main menu: the timer applies Fast");
    const int setsBefore = sets;
    tick();
    Check(sets == setsBefore, "nothing changed: the timer sets nothing");
    hook(0);
    Check(lastMode == 1 && now().replaced == 1, "the game asks for Background again in the menu: Fast instead");
    state(0, 0);   // in play
    tick();
    Check(lastMode == 0 && now().handBacks == 1, "play starts: the game's Background back at the next check");
    hook(0);
    Check(lastMode == 0, "the game's Background in play passes unchanged");
    state(0, 1);   // a loading screen (the transition's freeze)
    tick();
    Check(lastMode == 1, "loading screen: Fast");
    state(2, 0);   // a menu in play
    tick();
    Check(lastMode == 1, "a menu: Fast");
    freezeThere = false;   // start-up: no freeze subsystem yet
    tick();
    Check(lastMode == 1 && now().inPlay == 0, "no freeze subsystem yet (start-up): not in play, Fast");
    freezeThere = true;
    state(0, 0);
    hook(1);
    Check(lastMode == 1 && now().requested == 1, "the game's own Fast request passes");
    hook(2);
    Check(lastMode == 2, "the game's Precompile request passes");
    tick();
    Check(lastMode == 2, "... and stays");
    hook(0);
    Check(lastMode == 0, "Background in play");
    // a request from another thread (the render thread) follows the game thread's last look
    state(1, 0);
    tick();   // the game thread looks: a menu -> Fast
    std::thread([&] { hook(0); }).join();
    Check(lastMode == 1, "another thread's Background request after a menu look: Fast");
    state(0, 0);
    tick();   // in play -> Background
    std::thread([&] { hook(0); }).join();
    Check(lastMode == 0, "... after an in-play look: Background");
    // switched off live, on again
    state(1, 0);
    tick();
    Check(lastMode == 1, "menu again: Fast");
    SetPsoPrecompileFast(false);
    tick();
    Check(lastMode == 0, "switched off: the game's Background at the next check");
    SetPsoPrecompileFast(true);
    tick();
    Check(lastMode == 1, "switched on: Fast again");
    // menus opened in play (PsoPrecompileFastInGameMenus 0 by default): the game's pace once play has lasted long enough
    PsoPrecompileTestPlayMs(50);   // also forgets the menus and play seen so far
    state(0, 0);
    tick();
    Sleep(70);
    tick();
    state(1, 0);
    tick();
    Check(lastMode == 1 && now().inPlay == 0, "no menu freeze for long before the first menu (start-up videos): the main menu stays Fast");
    state(0, 0);
    tick();
    state(1, 0);
    tick();
    Check(lastMode == 1 && now().inPlay == 0, "a moment without a menu (the main menu's arrival) is not play yet: the menu stays Fast");
    state(0, 0);
    tick();
    Sleep(70);
    tick();
    Check(lastMode == 0 && now().inPlay == 1, "play for longer than the threshold: the game's Background");
    state(2, 0);
    tick();
    Check(lastMode == 0 && now().inPlay == 2, "a menu opened in play (inventory): the game's Background stays");
    hook(0);
    Check(lastMode == 0, "... also for the game's own Background request there");
    std::thread([&] { hook(0); }).join();
    Check(lastMode == 0, "... and for a request from another thread");
    state(0, 1);
    tick();
    Check(lastMode == 1, "a loading screen after play: Fast");
    state(2, 0);
    tick();
    Check(lastMode == 0, "the menu in play again: the game's Background");
    SetPsoPrecompileFastInGameMenus(true);
    tick();
    Check(lastMode == 1, "PsoPrecompileFastInGameMenus=1: Fast in the menu opened in play (the 1.1.0 behaviour)");
    SetPsoPrecompileFastInGameMenus(false);
    tick();
    Check(lastMode == 0, "switched back to 0: the game's Background at the next check");
    state(1, 0);
    // the precompile ends in the menu
    Sleep(40);
    running(false);
    tick();
    Check(lastMode == 0 && now().doneMs > 0, "the precompile ends: the game's Background back, the end time noted");
    Check(!now().timerRunning, "... and the timer stops (the next mode request starts it again)");
    hook(0);
    Check(lastMode == 0 && now().timerRunning, "a later request: Background passes, the timer runs again");

    // 1.1.10: the bounded faster pace and the cold-cache guard (fake batch statics and queue counter)
    Check(info.active == reinterpret_cast<int64_t*>(game.base + 0x921A208), "the precompile's queue counter (TotalActiveTasks) at exe+0x921A208");
    uint32_t fakeSize = 50;
    float fakeTime = 16.0f;
    int64_t fakeActive = 0;
    PsoPrecompileTestStatics(&fakeSize, &fakeTime, &fakeActive);
    PsoPrecompileTestOriginal(reinterpret_cast<void*>(&FakeSetBatchMode));
    PsoPrecompileTestStatics(&fakeSize, &fakeTime, &fakeActive);
    state(1, 0);
    running(true);
    hook(0);
    tick();
    Check(lastMode == 1 && fakeTime == 4.0f && fakeSize == 8 && now().bounded, "the faster pace in a menu: 4 ms of render thread per frame, the batch starts at 8 (UE's Fast: 16 ms, 50)");
    fakeSize = 30;
    tick();
    Check(fakeSize == 30, "... the batch then adapts as UE's tick decides (not written again while nothing changes)");
    fakeSize = 50;
    fakeTime = 16.0f;   // what the game's SetBatchMode(Fast) sets (the fake does not)
    state(0, 1);
    sets = 0;
    tick();
    Check(lastMode == 1 && sets == 1 && fakeTime == 16.0f && fakeSize == 50 && !now().bounded,
          "a loading screen: Fast passed again with UE's own values (nobody interacts there)");
    tick();
    Check(sets == 1, "... and not again while it stays");
    state(1, 0);
    tick();
    Check(lastMode == 1 && fakeTime == 4.0f && fakeSize == 8 && now().bounded, "back in a menu: the bounded budget again");
    SetPsoPrecompileFastBudget(16.0f, 1000);
    state(0, 0);
    tick();
    fakeSize = 50;
    fakeTime = 16.0f;
    state(1, 0);
    tick();
    Check(lastMode == 1 && fakeTime == 16.0f && fakeSize == 50, "PsoPrecompileFastBudgetMs=16: UE's Fast values as they are (the pace before 1.1.10)");
    SetPsoPrecompileFastBudget(4.0f, 1000);
    fakeActive = 999;
    tick();
    Check(lastMode == 1 && !now().cold, "999 pipelines waiting: still the faster pace");
    fakeActive = 1500;
    tick();
    Check(lastMode == 0 && now().cold, "1500 waiting (a cold driver cache): the game's Background at the next check");
    fakeActive = 0;
    tick();
    hook(0);
    Check(lastMode == 0 && now().cold, "... for the rest of this precompile, also when the queue drains and the game asks again");
    state(0, 1);
    tick();
    Check(lastMode == 0, "... and at a loading screen");
    running(false);
    tick();
    running(true);
    hook(0);
    tick();
    Check(lastMode == 1 && !now().cold, "a new precompile is judged afresh: the faster pace at the loading screen");
    SetPsoPrecompileFastBudget(4.0f, 0);
    fakeActive = 50000;
    tick();
    Check(lastMode == 1 && !now().cold, "PsoPrecompileColdQueue=0: no queue limit");
    SetPsoPrecompileFastBudget(4.0f, 1000);
    tick();
    Check(lastMode == 0 && now().cold, "the limit back at 1000: the game's pace at the next check");
    // the driver's compile threads busy (the AMD driver compiles on D3D12's background threads)
    running(false);
    tick();
    running(true);
    fakeActive = 0;
    state(1, 0);
    hook(0);
    tick();
    Check(lastMode == 1 && !now().cold, "a new precompile in the menu: the faster pace");
    {
        // the thread list without a Toolhelp snapshot (no NtMapViewOfSection: chrome_elf's start-up hook race), against one
        // (threads of this test process may start or end between the two lists: up to five tries until the counts agree)
        uint32_t first = 0;
        int n = 0, snapCount = 0;
        DWORD snapFirst = 0;
        for (int attempt = 0; attempt < 5; ++attempt) {
            n = PsoPrecompileTestOwnThreads(&first);
            snapCount = 0;
            snapFirst = 0;
            ULONGLONG snapFirstTime = ~0ull;
            const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{};
            te.dwSize = sizeof te;
            for (BOOL ok = snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                ++snapCount;
                if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID)) {
                    FILETIME c, e, k, u;
                    if (GetThreadTimes(h, &c, &e, &k, &u)) {
                        const ULONGLONG made = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
                        if (made < snapFirstTime) snapFirstTime = made, snapFirst = te.th32ThreadID;
                    }
                    CloseHandle(h);
                }
            }
            if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
            if (n > 0 && n == snapCount) break;
            Sleep(20);
        }
        Check(n > 0 && n == snapCount, "pso thread list: this process's threads from NtQuerySystemInformation, the same count as a snapshot");
        Check(first != 0 && first == snapFirst, "pso thread list: the first-created thread (the game thread at install) as the snapshot finds it");
    }
    PsoPrecompileTestDriverBusy(0.6);
    tick();
    Check(lastMode == 1 && !now().cold, "the driver's threads busy for one second: still the faster pace");
    PsoPrecompileTestDriverBusy(0.2);
    PsoPrecompileTestDriverBusy(0.7);
    tick();
    Check(lastMode == 1 && !now().cold, "... not two seconds in a row: still the faster pace");
    PsoPrecompileTestDriverBusy(1.5);
    tick();
    Check(lastMode == 0 && now().cold, "busy two seconds in a row (a cold driver cache): the game's Background at the next check");
    PsoPrecompileWatchDriver();   // the real reading (no D3D background threads in the self-test): nothing breaks
    Check(lastMode == 0 && now().cold, "... and the real reading of this process's threads runs without effect");
    running(false);
    tick();
    PsoPrecompileWatchDriver();

    // a budget change below 16 while the faster pace stays: re-applied at the next check (not only at the next switch)
    const auto fresh = [&] {   // a fresh state, the precompile running in the main menu at the bounded pace
        PsoPrecompileTestOriginal(reinterpret_cast<void*>(&FakeSetBatchMode));
        PsoPrecompileTestStatics(&fakeSize, &fakeTime, &fakeActive);
        fakeActive = 0;
        state(1, 0);
        running(true);
        hook(0);
        tick();
    };
    fresh();
    Check(lastMode == 1 && fakeTime == 4.0f, "fresh: the faster pace at 4 ms");
    SetPsoPrecompileFastBudget(8.0f, 1000);
    fakeTime = 16.0f;
    fakeSize = 50;
    sets = 0;
    tick();
    Check(sets == 1 && lastMode == 1 && fakeTime == 8.0f && fakeSize == 8, "PsoPrecompileFastBudgetMs 4 -> 8 in the menu: the new budget at the next check");
    tick();
    Check(sets == 1, "... applied once");
    SetPsoPrecompileFastBudget(4.0f, 1000);
    tick();
    Check(fakeTime == 4.0f, "... and back to 4");
    // the 60 s limit (shortened to 200 ms), per precompile
    PsoPrecompileTestTiming(200, 10000);
    tick();
    Check(lastMode == 1 && !now().cold, "below the faster pace's time limit: the faster pace");
    Sleep(260);
    tick();
    Check(lastMode == 0 && now().cold && strstr(now().coldWhy, "60 s") == now().coldWhy,
          "the faster pace ran out its time limit without finishing: the game's Background (\"60 s\" guard)");
    running(false);
    tick();
    running(true);
    hook(0);
    tick();
    Check(lastMode == 1 && !now().cold, "a new precompile gets its own time limit: the faster pace");
    Sleep(260);
    tick();
    Check(lastMode == 0 && now().cold, "... until it has run out too");
    // PsoPrecompileColdQueue=0: no cold-cache guard at all (no queue, time or driver-thread limit)
    running(false);
    tick();
    running(true);
    SetPsoPrecompileFastBudget(4.0f, 0);
    hook(0);
    tick();
    Sleep(260);
    fakeActive = 50000;
    tick();
    Check(lastMode == 1 && !now().cold, "PsoPrecompileColdQueue=0: no time limit and no queue limit");
    PsoPrecompileTestDriverBusy(1.0);
    PsoPrecompileTestDriverBusy(1.0);
    tick();
    Check(lastMode == 1 && !now().cold, "... and no driver-thread limit");
    PsoPrecompileWatchDriver();
    Check(now().driverSearches == 0, "... nor a search for the driver's threads");
    fakeActive = 0;
    SetPsoPrecompileFastBudget(4.0f, 1000);
    tick();
    Check(lastMode == 0 && now().cold && strstr(now().coldWhy, "60 s") == now().coldWhy, "the guard back on: the time limit (already run out) at the next check");
    running(false);
    tick();

    // the first precompile keeps a cold verdict the monitor thread made before the game thread's first look
    PsoPrecompileTestOriginal(reinterpret_cast<void*>(&FakeSetBatchMode));
    PsoPrecompileTestStatics(&fakeSize, &fakeTime, &fakeActive);
    state(1, 0);
    running(true);
    PsoPrecompileTestDriverBusy(1.0);
    PsoPrecompileTestDriverBusy(1.0);
    hook(0);
    tick();
    Check(lastMode == 0 && now().cold && strstr(now().coldWhy, "driver threads") == now().coldWhy,
          "the first precompile, judged cold by the monitor before the game thread's first look: the game's Background");
    // ... and so does a later one judged after the earlier one's end, before the game thread saw it start
    fresh();
    running(false);
    tick();
    Check(now().doneMs > 0 && !now().cold, "a precompile at the faster pace ends");
    Sleep(40);
    running(true);
    PsoPrecompileTestDriverBusy(1.0);
    PsoPrecompileTestDriverBusy(1.0);
    hook(0);
    tick();
    Check(lastMode == 0 && now().cold, "the next one, judged cold before the game thread saw it start: the verdict stays");

    // the real driver-thread search: none found (paced, then given up), then a thread named as D3D12's
    fresh();
    PsoPrecompileTestTiming(60000, 100);
    PsoPrecompileWatchDriver();
    Check(now().driverSearches == 1 && now().driverThreads == 0, "no driver compile thread: one search");
    PsoPrecompileWatchDriver();
    Check(now().driverSearches == 1, "... not again within the retry interval");
    Sleep(120);
    PsoPrecompileWatchDriver();
    Sleep(120);
    PsoPrecompileWatchDriver();
    Check(now().driverSearches == 3 && now().driverGaveUp, "three empty searches: the driver-thread guard gives up for this precompile");
    Sleep(120);
    PsoPrecompileWatchDriver();
    Check(now().driverSearches == 3 && lastMode == 1 && !now().cold, "... no more searches, the faster pace stays");
    printf("      one search of the system's threads (Toolhelp snapshot and walk): %.1f ms\n", now().driverSearchMs);
    running(false);
    tick();
    PsoPrecompileWatchDriver();
    Check(now().driverSearches == 0 && !now().driverGaveUp, "the precompile ends: the next one searches afresh");
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    const auto setDesc = reinterpret_cast<SetThreadDescriptionFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
    if (setDesc) {
        std::atomic<int> spin{0};   // 1: named and spinning, 2: stop
        std::thread busy([&] {
            setDesc(GetCurrentThread(), L"D3D Background Thread (test)");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            spin = 1;
            while (spin.load() != 2) {
            }
        });
        while (spin.load() != 1) Sleep(1);
        running(true);
        hook(0);
        tick();
        PsoPrecompileWatchDriver();
        Check(now().driverThreads == 1 && now().driverSearches == 1, "a thread named \"D3D Background Thread (test)\" found in one search");
        Sleep(1050);
        PsoPrecompileWatchDriver();
        tick();
        Check(lastMode == 1 && !now().cold && now().driverSearches == 1, "... busy for one second: still the faster pace, no new search while it lives");
        int readings = 1;   // a busy PC may give the spinning thread less than half a core in some second: up to 8 readings
        while (!now().cold && readings < 8) {
            Sleep(1050);
            PsoPrecompileWatchDriver();
            ++readings;
        }
        tick();
        printf("      the spinning thread tripped the guard at reading %d (2 = no second below half a core)\n", readings);
        Check(lastMode == 0 && now().cold && strstr(now().coldWhy, "driver threads") == now().coldWhy,
              "... busy two seconds in a row: the game's Background (\"driver threads\" guard)");
        spin = 2;
        busy.join();
        PsoPrecompileWatchDriver();
        Check(now().driverThreads == 0, "... and its handle let go");
    } else {
        printf("SKIP  the driver-thread search with a named thread: SetThreadDescription not available\n");
    }
    running(false);
    tick();
    PsoPrecompileWatchDriver();
    PsoPrecompileTestStatics(info.batchSize, info.batchTime, info.active);

    *info.singleton = nullptr;
    PsoPrecompileTestOriginal(reinterpret_cast<void*>(&FakeSetBatchMode));
    FadeHookTestFunctions(nullptr, nullptr, nullptr);
}
}  // namespace pso
