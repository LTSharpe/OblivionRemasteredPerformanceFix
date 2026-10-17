// PsoPrecompileFast self-test, included by selftest.cpp: the sites on the mapped exe (SetBatchMode, the singleton
// it checks, the batch statics, IsPrecompiling's flag), the entry jump and the trampoline (run for real on the mapped exe:
// its singleton is null there, so the moved comparison sends it straight to the function's return), then the hook's and the
// game-thread timer's choices with a fake SetBatchMode, a fake cache object and a fake freeze subsystem: Fast instead of
// Background only while the precompile runs outside play; the game's mode back in play, when the precompile ends and when
// switched off; Fast and Precompile requests unchanged; a request from another thread follows the game thread's last look.
#pragma once

#include <thread>

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
    // the precompile ends in the menu
    Sleep(40);
    running(false);
    tick();
    Check(lastMode == 0 && now().doneMs > 0, "the precompile ends: the game's Background back, the end time noted");
    Check(!now().timerRunning, "... and the timer stops (the next mode request starts it again)");
    hook(0);
    Check(lastMode == 0 && now().timerRunning, "a later request: Background passes, the timer runs again");

    *info.singleton = nullptr;
    PsoPrecompileTestOriginal(reinterpret_cast<void*>(&FakeSetBatchMode));
    FadeHookTestFunctions(nullptr, nullptr, nullptr);
}
}  // namespace pso
