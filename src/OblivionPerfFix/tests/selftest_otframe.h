// OtFrameTrim self-test (public since 1.3.0, worker port130j; lab fightcost before), included by selftest.cpp: the discovery on the
// mapped exe (the frame-start deferred sleep, its only deferred requester, the PathBuilder poll and state check, the QPC
// wrapper, the two imports), the install (the consumer's Sleep call and the wrapper's entry now go to the fix; the thunk's
// code page read-execute, its flags on another page), then the game's own consumer and wrapper run in the mapped image with
// fakes in the import slots and a fake PathBuilder: mode 0 sleeps as stock and times as stock; mode 1 returns at once with
// nothing in flight, at once with a completion signalled (and gives the count back), after the full timeout without one,
// early when the path thread signals during the wait; mode 2 returns 0 from the timer.
#pragma once
namespace otf {
int g_sleeps = 0;
DWORD g_sleptMs = 0;
void WINAPI FakeSleep(DWORD ms)
{
    ++g_sleeps;
    g_sleptMs = ms;
}
BOOL WINAPI FakeQpc(LARGE_INTEGER* out)
{
    out->QuadPart = 0x1234567890ABCDEFll;
    return TRUE;
}
alignas(16) uint8_t g_obj[0x100] = {};
alignas(16) uint8_t g_ctx[0x80] = {};
HANDLE g_done = nullptr;
double NowMs()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return t.QuadPart * 1000.0 / f.QuadPart;
}
DWORD WINAPI Signaller(void*)  // 2 ms of spinning (Sleep(n) can last a whole 15.6 ms timer tick in a test process)
{
    const double t = NowMs();
    while (NowMs() - t < 2) YieldProcessor();
    ReleaseSemaphore(g_done, 1, nullptr);
    return 0;
}

}  // namespace otf

static void OtFrameTrimRun(const GameModule& game)
{
    using namespace otf;
    OtFrameTrimDiscovery d{};
    Check(OtFrameTrimDiscover(game, &d), "OtFrameTrim: discovery on the mapped exe");
    Check(d.consumers == 1 && d.requests == 1 && d.polls == 1 && d.lists == 1 && d.timers == 1,
          "OtFrameTrim: one frame-start sleep, one sleep request, one PathBuilder poll, one process-list call, one node timer");
    if (game.timeDateStamp == 0xF19077A4u)
        Check(d.consumer - game.base == 0x68D4090 && d.timer - game.base == 0x694E170 &&
                  reinterpret_cast<uint8_t*>(d.pathObj) - game.base == 0x948C200 &&
                  reinterpret_cast<uint8_t*>(d.accumulator) - game.base == 0x948D0A4,
              "OtFrameTrim: exe+0x68D4090 / 0x694E170, PathBuilder exe+0x948C200, accumulator exe+0x948D0A4 (build 19115871)");
    if (!d.consumer) return;
    Check(OtFrameTrimTestInstall(), "OtFrameTrim: installs on the mapped exe");
    Check(d.consumer[0x1E] == 0xE8 && d.consumer[0x23] == 0x90 && d.timer[0] == 0xE9,
          "OtFrameTrim: the consumer's call [Sleep] and the timer's entry are redirected");
    MEMORY_BASIC_INFORMATION mi{};
    const uint8_t* const thunk = d.timer + 5 + *reinterpret_cast<const int32_t*>(d.timer + 1);
    Check(VirtualQuery(thunk, &mi, sizeof mi) && mi.Protect == PAGE_EXECUTE_READ,
          "OtFrameTrim: the timer thunk's page is read-execute (not writable)");
    void** const qpcSlot = FindImportSlot(game.base, "KERNEL32.dll", "QueryPerformanceCounter");
    Check(WritePointer(d.sleepSlot, reinterpret_cast<void*>(&FakeSleep)) && qpcSlot &&
              WritePointer(qpcSlot, reinterpret_cast<void*>(&FakeQpc)),
          "OtFrameTrim: fakes in the mapped exe's Sleep / QueryPerformanceCounter slots");
    using Fn = uint32_t (*)();
    const auto consumer = reinterpret_cast<void (*)()>(d.consumer);
    const auto timer = reinterpret_cast<Fn>(d.timer);
    g_done = CreateSemaphoreW(nullptr, 0, 16, nullptr);
    *reinterpret_cast<uint8_t**>(g_obj + 0x10) = g_ctx;
    *reinterpret_cast<HANDLE*>(g_ctx + 0x50) = g_done;
    *reinterpret_cast<int32_t*>(g_obj + 0xC) = 1;
    DWORD old;
    VirtualProtect(d.pathObj, 8, PAGE_READWRITE, &old);
    VirtualProtect(d.accumulator, 4, PAGE_READWRITE, &old);
    *d.pathObj = g_obj;

    // mode 0: stock sleep through the slot, clamped to 20, accumulator cleared; the timer returns the counter's low 32 bits
    SetOtFrameTrim(0);
    *d.accumulator = 37;
    consumer();
    Check(g_sleeps == 1 && g_sleptMs == 20 && *d.accumulator == 0, "OtFrameTrim mode 0: the stock Sleep(min(acc, 20)), acc cleared");
    *d.accumulator = 0;
    consumer();
    Check(g_sleeps == 1, "OtFrameTrim mode 0: no sleep with nothing accumulated");
    Check(timer() == 0x90ABCDEFu, "OtFrameTrim mode 0: the node timer returns the counter's low 32 bits as stock");

    // mode 1: no Sleep call; the wait follows the PathBuilder
    SetOtFrameTrim(1);
    uint64_t s0, e0, i0, f0, s1, e1, i1, f1;
    OtFrameTrimTestStats(&s0, &e0, &i0, &f0);
    *reinterpret_cast<int32_t*>(g_obj + 0xC) = 0;  // nothing in flight
    *d.accumulator = 5;
    double t = NowMs();
    consumer();
    OtFrameTrimTestStats(&s1, &e1, &i1, &f1);
    Check(g_sleeps == 1 && i1 == i0 + 1 && NowMs() - t < 4 && *d.accumulator == 0,
          "OtFrameTrim mode 1: nothing in flight -> no wait, accumulator cleared");
    *reinterpret_cast<int32_t*>(g_obj + 0xC) = 1;
    ReleaseSemaphore(g_done, 1, nullptr);  // the path thread has finished
    *d.accumulator = 5;
    t = NowMs();
    consumer();
    OtFrameTrimTestStats(&s0, &e0, &i0, &f0);
    Check(e0 == e1 + 1 && NowMs() - t < 4 && WaitForSingleObject(g_done, 0) == WAIT_OBJECT_0 &&
              WaitForSingleObject(g_done, 0) == WAIT_TIMEOUT,
          "OtFrameTrim mode 1: completion already signalled -> no wait, the count given back (exactly one)");
    *d.accumulator = 5;
    t = NowMs();
    consumer();
    OtFrameTrimTestStats(&s1, &e1, &i1, &f1);
    Check(f1 == f0 + 1 && NowMs() - t >= 2 && WaitForSingleObject(g_done, 0) == WAIT_TIMEOUT,
          "OtFrameTrim mode 1: in flight, no completion -> the full timeout, no count made up");
    *d.accumulator = 20;
    HANDLE th = CreateThread(nullptr, 0, Signaller, nullptr, 0, nullptr);
    t = NowMs();
    consumer();
    const double waited = NowMs() - t;
    OtFrameTrimTestStats(&s0, &e0, &i0, &f0);
    if (th) { WaitForSingleObject(th, INFINITE); CloseHandle(th); }
    Check(e0 == e1 + 1 && waited < 12 && WaitForSingleObject(g_done, 0) == WAIT_OBJECT_0,
          "OtFrameTrim mode 1: completion during the wait -> wakes early, the count given back");
    Check(g_sleeps == 1 && timer() == 0x90ABCDEFu, "OtFrameTrim mode 1: Sleep never called; the node timer stock");

    // mode 2 (and 3): the timer returns 0; mode 2 sleeps as stock
    SetOtFrameTrim(2);
    Check(timer() == 0, "OtFrameTrim mode 2: the node timer returns 0");
    *d.accumulator = 3;
    consumer();
    Check(g_sleeps == 2 && g_sleptMs == 3, "OtFrameTrim mode 2: the frame-start sleep stock");
    SetOtFrameTrim(3);
    Check(timer() == 0, "OtFrameTrim mode 3: the node timer returns 0");
    SetOtFrameTrim(0);
    Check(timer() == 0x90ABCDEFu, "OtFrameTrim back to mode 0: the node timer stock again");
    *d.pathObj = nullptr;
    CloseHandle(g_done);

}
