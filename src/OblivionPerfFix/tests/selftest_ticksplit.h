// Diagnostic TickSplit self-test (public since 1.2.3; the perception split's part is lab only), included by selftest.cpp after the ledger's test: its sites on the
// mapped exe, then the record path with fake tick functions, a fake ReleaseTickGroup and a fake clock: per-entry calls
// and exclusive time (a nested tick subtracted from its outer one), the group key, the window's W row against the ledger's
// tick-group time, a slow frame's top entries, and the hook's cost per call with the real clock.
#pragma once

namespace tsp {
uint64_t g_now = 0;  // us (the ledger's fake clock)
int64_t g_freq = 10000000;
uint64_t LedgerClock() { return g_now; }
uint64_t SplitClock() { return g_now * static_cast<uint64_t>(g_freq) / 1000000; }

// fake UObjects: obj -> cls -> meta -> meta
struct FakeObj { void* vt; uint32_t flags, index; FakeObj* cls; uint32_t name, number; void* outer; };
FakeObj g_meta{}, g_bpgc{}, g_clsA{}, g_clsB{}, g_objA{}, g_objB{};
struct FakeTf { void** vt; uint8_t tickGroup, endGroup, bits, state; float interval; uint8_t prereq[16]; uint8_t* internal; void* target; };
static_assert(offsetof(FakeTf, internal) == 0x20 && offsetof(FakeTf, target) == 0x28, "FTickFunction layout");
uint8_t g_internal[0x30];
using ExecFn = void (*)(void*, float, int, int, const void*);
ExecFn g_hook = nullptr;
FakeTf g_tfA{}, g_tfB{}, g_tfOuter{};
uint64_t g_runA = 300, g_runB = 700;  // us per call
void ExecA(void*, float, int, int, const void*) { g_now += g_runA; }
void ExecB(void*, float, int, int, const void*) { g_now += g_runB; }
void ExecOuter(void*, float, int, int, const void*)
{
    g_now += 100;
    g_hook(&g_tfA, 0.016f, 0, 0, nullptr);  // a nested tick (the game thread helping while it waits)
    g_now += 100;
}
void* g_vtA[2] = {nullptr, reinterpret_cast<void*>(&ExecA)};
void* g_vtB[2] = {nullptr, reinterpret_cast<void*>(&ExecB)};
void* g_vtOuter[2] = {nullptr, reinterpret_cast<void*>(&ExecOuter)};
int g_ticksPerRelease = 0;
void FakeTg(void*, int group, uint8_t)
{
    if (group == 0)
        for (int i = 0; i < g_ticksPerRelease; ++i) g_hook(&g_tfA, 0.016f, 0, 0, nullptr);
    if (group == 2) g_hook(&g_tfB, 0.016f, 0, 0, nullptr);
    if (group == 3) g_hook(&g_tfOuter, 0.016f, 0, 0, nullptr);
    g_now += 50;  // the release's own time
}
void Trivial(void*, float, int, int, const void*) {}
void* g_vtTrivial[2] = {nullptr, reinterpret_cast<void*>(&Trivial)};


std::vector<std::string> Split(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) out.push_back(item);
    return out;
}

static void Run(const GameModule& game)
{
    Check(InstallTickSplit(true, L"."), "tick split: installed on the mapped exe (after the ledger)");
    TickSplitInfoOut info{};
    if (!Installed(TickSplitInfo(&info) && info.installed, "tick split")) return;
    Check(info.exec == game.base + 0x37C1954 && info.exec[0] == 0xE8 && info.exec[5] == 0x90,
          "tick split: the tick task's ExecuteTick call at exe+0x37C1954, now call stub + nop");
    Check(info.tickable == game.base + 0x37CD3EB && info.tickable[0] == 0xE8, "tick split: TickObjects' Tick call at exe+0x37CD3EB");
    Check(info.tNative == game.base + 0x37FAFA8 && info.tDynamic == game.base + 0x37FB019 && info.tCallback == game.base + 0x37FB046 &&
              info.tNative[0] == 0xE8 && info.tNative[9] == 0x90 && info.tDynamic[0] == 0xE8 && info.tDynamic[6] == 0x90 &&
              info.tCallback[0] == 0xE8,
          "tick split: the timer manager's three delegate calls (native, ProcessEvent, TFunction), now calls to stubs");
    if (info.tNative) Check(RipTarget(info.tNative, 1, 5) == info.region + 160 && memcmp(info.region + 160, "\x48\x8B\x4B\x10\x4C\x8B\xC3", 7) == 0,
                            "tick split: the native timer stub reloads rcx = [rbx+10h], r8 = rbx");
    Check(info.setup == game.base + 0x332F7CF && info.coll == game.base + 0x332F829 && info.latent == game.base + 0x332F917 &&
              info.timer == game.base + 0x332F937 && info.tickObj == game.base + 0x332F948 && info.stream == game.base + 0x332F9F2,
          "tick split: UWorld::Tick's six calls (setup, collision tree, latent, timers, tickables, streaming)");
    if (info.setup) Check(RipTarget(info.setup, 1, 5) == info.region + 64, "tick split: the setup call goes to its stub");
    const uint8_t execStub[3] = {0x44, 0x8B, 0xCE};
    Check(memcmp(info.region, execStub, 3) == 0, "tick split: the exec stub starts with mov r9d,esi");
    g_hook = reinterpret_cast<ExecFn>(info.execHook);

    // fake objects
    g_meta.cls = &g_meta;
    g_clsA.cls = &g_meta;
    g_bpgc.cls = &g_meta;
    g_clsB.cls = &g_bpgc;  // a Blueprint class: its class is UBlueprintGeneratedClass, whose class is UClass
    g_objA.cls = &g_clsA;
    g_objB.cls = &g_clsB;
    g_internal[1] = 0;  // actual start group TG_PrePhysics
    g_tfA = FakeTf{g_vtA, 0, 0, 0, 1, 0, {}, g_internal, &g_objA};
    g_tfB = FakeTf{g_vtB, 0, 0, 0, 1, 0, {}, g_internal, &g_objB};
    g_tfOuter = FakeTf{g_vtOuter, 0, 0, 0, 1, 0, {}, g_internal, &g_objB};

    const wchar_t* const path = L"selftest-ticksplit.csv";
    DeleteFileW(path);
    HitchLedgerTestSetup(&LedgerClock, reinterpret_cast<void*>(&FakeTg), nullptr, L"selftest-ticksplit-ledger.csv");
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    g_freq = fq.QuadPart;
    TickSplitTestSetup(&SplitClock, path);
    g_now = 5000000;
    LedgerPacing p{};
    SetHitchLedger(false, 25);
    LedgerFrameEnd(p);  // off: the ledger's frame state restarts on the fake clock
    SetHitchLedger(true, 25);
    SetTickSplit(true);
    LedgerFrameEnd(p);  // first frame after switching on: not recorded
    HitchLedgerInfoOut li{};
    HitchLedgerInfo(&li);
    auto frame = [&](int ticks, uint64_t rest) {
        g_ticksPerRelease = ticks;
        g_now += 1000;
        LedgerPreActorTick();
        for (int grp : {0, 2, 3}) reinterpret_cast<void (*)(void*, int, uint8_t)>(li.tgHook)(nullptr, grp, 1);
        g_now += rest;
        LedgerPostActorTick();
        g_now += 1000;
        LedgerSleepBegin();
        g_now += 500;
        LedgerFrameEnd(p);
    };
    // ~8.9 ms frames: 10 x A (3 ms) in TG_PrePhysics, B (0.7) in group 2, outer (0.2 + nested A 0.3) in group 3
    for (int i = 0; i < 1000; ++i) {
        frame(10, 2000);
        if (i % 50 == 0) HitchLedgerDrain();  // the monitor thread, about twice a second
    }
    frame(70, 2000);  // one slow frame (26.9 ms): 70 x A = 21 ms
    for (int i = 0; i < 300; ++i) {
        frame(10, 2000);
        if (i % 50 == 0) HitchLedgerDrain();
    }
    HitchLedgerDrain();
    TickSplitTestClose();
    HitchLedgerTestClose();

    std::ifstream in("selftest-ticksplit.csv");
    std::string line;
    std::vector<std::vector<std::string>> w, e, t;
    while (std::getline(in, line)) {
        if (line.rfind("W,", 0) == 0) w.push_back(Split(line));
        else if (line.rfind("E,", 0) == 0) e.push_back(Split(line));
        else if (line.rfind("T,", 0) == 0) t.push_back(Split(line));
    }
    uint64_t frames = 0, calls = 0;
    double tg0 = 0, exec = 0, seconds = 0;
    bool cols = true;
    for (const auto& r : w) {
        cols = cols && r.size() == 30;
        if (r.size() != 30) continue;
        frames += std::stoull(r[3]);
        seconds = std::max(seconds, std::stod(r[2]));
        tg0 += std::stod(r[8]);
        exec += std::stod(r[17]);
        calls += std::stoull(r[16]);
    }
    Check(w.size() >= 4 && cols && seconds < 2.5, "tick split csv: 2-second window rows (W, 30 columns)");
    {
        const double expectExec = frames * (10 * 0.3 + 0.7 + 0.2 + 0.3) + 60 * 0.3;  // the slow frame has 60 more A
        char msg[200];
        sprintf_s(msg, "tick split W: frames %llu, exec %.1f ms (expected %.1f), calls %llu (expected %llu)",
                  static_cast<unsigned long long>(frames), exec, expectExec, static_cast<unsigned long long>(calls),
                  static_cast<unsigned long long>(frames * 13 + 60));
        Check(frames >= 1001 && std::abs(exec - expectExec) < 0.5 && calls == frames * 13 + 60, msg);
        sprintf_s(msg, "tick split W: the ledger's TG_PrePhysics %.1f ms = A's ticks + the releases' own 0.05 ms", tg0);
        Check(std::abs(tg0 - (frames * 3.05 + 18.0)) < 0.5, msg);
    }
    // entries: A in group 0 (calls 10/frame), B in group 2, outer in group 3 with exclusive 0.2, A nested in group 3
    double aG0 = 0, aG3 = 0, outerG3 = 0, bG2 = 0;
    bool bClassed = false;
    for (const auto& r : e) {
        if (r.size() < 9) continue;
        const int g = std::stoi(r[2]);
        const double ms = std::stod(r[7]);
        const uint64_t c = std::stoull(r[6]);
        if (g == 0 && c > 1000) aG0 = ms / c;
        if (g == 2) {
            bG2 = ms / c;
            bClassed = r[5] != "-";
        }
        if (g == 3 && std::abs(ms / c - 0.3) < 0.01) aG3 = ms / c;
        if (g == 3 && std::abs(ms / c - 0.2) < 0.01) outerG3 = ms / c;
    }
    Check(std::abs(aG0 - 0.3) < 0.01 && std::abs(bG2 - 0.7) < 0.01, "tick split E: per call A 0.3 ms (group 0), B 0.7 ms (group 2)");
    Check(bClassed, "tick split E: a Blueprint-class target (class -> generated class -> UClass) gets its class");
    Check(aG3 > 0 && outerG3 > 0, "tick split E: the outer tick's own 0.2 ms, the nested A's 0.3 ms as its own entry (group 3)");
    Check(t.size() == 1 && t[0].size() > 8 && t[0][5 + 1] == "0" && std::abs(std::stod(t[0][5 + 4]) - 21.0) < 0.1,
          "tick split T: one slow frame, its top entry group 0 with 21 ms");

    // ---- cost per call (real clock): the hook around a trivial tick vs the direct call ----
    TickSplitTestSetup(nullptr, L"selftest-ticksplit-cost.csv");
    HitchLedgerTestSetup(nullptr, nullptr, nullptr, nullptr);
    FakeTf trivial{g_vtTrivial, 0, 0, 0, 1, 0, {}, g_internal, &g_objA};
    LARGE_INTEGER q0, q1;
    constexpr int kN = 1000000;
    QueryPerformanceCounter(&q0);
    for (int i = 0; i < kN; ++i) g_hook(&trivial, 0.016f, 0, 0, nullptr);
    QueryPerformanceCounter(&q1);
    const double ns = 1e9 * static_cast<double>(q1.QuadPart - q0.QuadPart) / static_cast<double>(fq.QuadPart) / kN;
    char msg[160];
    sprintf_s(msg, "tick split cost per timed tick call: %.0f ns (target < 300 ns)", ns);
    Check(ns < 300, msg);
    SetTickSplit(false);
    SetHitchLedger(false, 25);
    LedgerFrameEnd(p);
    HitchLedgerDrain();
    TickSplitTestClose();
    HitchLedgerTestClose();
}
}  // namespace tsp
