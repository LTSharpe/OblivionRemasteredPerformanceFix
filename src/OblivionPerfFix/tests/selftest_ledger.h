// Diagnostic HitchLedger self-test, included by selftest.cpp: its sites on the mapped exe (tick groups, the frame-end
// sync call, the OT's grid transition, the precompile counters, GGPUFrameTime), then the record path with a fake clock
// and fake originals: per-frame accumulators -> record -> ring -> csv (header, columns, a slow frame's stage times, the
// 10-second summary's denominators), the threshold, and the cost per frame with the real clock.
#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace hl {
uint64_t g_now = 0;
uint64_t g_tgAdvance = 0, g_syncAdvance = 0;
uint64_t Clock() { return g_now; }
void FakeTg(void*, int, uint8_t) { g_now += g_tgAdvance; }
void FakeSync(void*, uint8_t) { g_now += g_syncAdvance; }

std::vector<std::string> Split(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) out.push_back(item);
    return out;
}

// One frame: pre-actor tick after pre us, tick group 3 for tg us, post-actor tick, sync for sync us, the cap's sleep.
void Frame(LedgerPacing& p, const HitchLedgerInfoOut& info, uint64_t pre, uint64_t tg, uint64_t actorRest, uint64_t sync,
           uint64_t post, uint64_t sleep)
{
    g_now += pre;
    LedgerPreActorTick();
    g_tgAdvance = tg;
    reinterpret_cast<void (*)(void*, int, uint8_t)>(info.tgHook)(nullptr, 3, 1);
    g_now += actorRest;
    LedgerPostActorTick();
    g_syncAdvance = sync;
    reinterpret_cast<void (*)(void*, uint8_t)>(info.syncHook)(nullptr, 1);
    g_now += post;
    LedgerSleepBegin();
    g_now += sleep;
    LedgerFrameEnd(p);
}

static void Run(const GameModule& game)
{
    Check(InstallHitchLedger(true, L".", true), "hitch ledger: installed on the mapped exe");
    HitchLedgerInfoOut info{};
    if (!Installed(HitchLedgerInfo(&info) && info.installed, "hitch ledger")) return;
    Check(info.tg == game.base + 0x37C8160, "ReleaseTickGroup at exe+0x37C8160");
    Check(info.tg && info.tg[0] == 0xE9, "ReleaseTickGroup starts with the jmp to the timer");
    Check(info.sync == game.base + 0x37FA780 && info.syncCall == game.base + 0x3908517,
          "FFrameEndSync::Sync at exe+0x37FA780, its call in FEngineLoop::Tick at exe+0x3908517");
    if (info.syncCall) Check(RipTarget(info.syncCall, 1, 5) != info.sync, "the sync call goes to the timer's stub");
    Check(info.psoTasks == reinterpret_cast<int64_t*>(game.base + 0x921A218) &&
              info.psoTime == reinterpret_cast<int64_t*>(game.base + 0x921A220),
          "precompile counters TotalCompleteTasks exe+0x921A218, TotalPrecompileTime exe+0x921A220");
    Check(info.psoSingleton == reinterpret_cast<uint8_t**>(game.base + 0x921A230) && info.psoFlag == 0x140,
          "shader pipeline cache singleton exe+0x921A230, precompiling flag +0x140");
    Check(info.gpu != nullptr, "GGPUFrameTime found");
    Check(InstallHitchLedgerGrid(true), "hitch ledger: the OT's grid transition detoured");
    HitchLedgerInfo(&info);
    Check(info.grid == game.base + 0x66B2000 && info.grid[0] == 0xE9, "grid transition at exe+0x66B2000, now a jmp");
    Check(info.recordSize <= 512, "ledger record <= 512 bytes");

    // ---- the record path with a fake clock ----
    const wchar_t* const path = L"selftest-ledger.csv";
    DeleteFileW(path);
    HitchLedgerTestSetup(&Clock, reinterpret_cast<void*>(&FakeTg), reinterpret_cast<void*>(&FakeSync), path);
    SetHitchLedger(true, 25);
    g_now = 1000000;
    LedgerPacing p{};
    LedgerFrameEnd(p);  // the first frame end after switching on: its start is not known, never a record
    // 600 frames of 16 ms (9.6 s), then one slow frame of 40 ms with known parts, then fast frames past 10 s
    for (int i = 0; i < 600; ++i) Frame(p, info, 2000, 6000, 1000, 3000, 1000, 3000);
    // slow frame: pre 3, tg3 12 (of which pairing is counted by NpcCreationPacing: 6), actor rest 4, sync 8, post 2, sleep 1 = 30 +
    // the fixed parts below
    p.pairCumUs += 6000;
    p.unitCumUs += 4000;
    p.unitCumN += 2;
    p.flowCumUs += 1500;
    HitchLedgerTestGridAdd(5000);
    Frame(p, info, 3000, 12000, 14000, 8000, 2000, 1000);  // 40 ms
    for (int i = 0; i < 40; ++i) Frame(p, info, 2000, 6000, 1000, 3000, 1000, 3000);
    // a frame of 24 ms: under the threshold
    Frame(p, info, 2000, 6000, 9000, 3000, 1000, 3000);
    HitchLedgerDrain();
    HitchLedgerTestClose();

    std::ifstream in("selftest-ledger.csv");
    std::string line, frameHeader, summaryHeader;
    std::vector<std::vector<std::string>> fRows, sRows;
    bool head = false;
    while (std::getline(in, line)) {
        if (line.rfind("# OblivionPerfFix-ledger v1", 0) == 0) head = line.find("qpc_freq=") != std::string::npos;
        else if (line.rfind("#F,", 0) == 0) frameHeader = line.substr(1);
        else if (line.rfind("#S,", 0) == 0) summaryHeader = line.substr(1);
        else if (line.rfind("F,", 0) == 0) fRows.push_back(Split(line));
        else if (line.rfind("S,", 0) == 0) sRows.push_back(Split(line));
    }
    Check(head, "ledger csv: header line with the version and the QPC frequency");
    const auto fh = Split(frameHeader), sh = Split(summaryHeader);
    Check(fh.size() == 39 && sh.size() == 46, "ledger csv: 39 frame columns, 46 summary columns");
    Check(fRows.size() == 1, "ledger csv: exactly one slow frame (the 40 ms one; 16 ms and 24 ms frames are not rows)");
    if (fRows.size() == 1 && fRows[0].size() == fh.size()) {
        auto col = [&](const char* name) {
            for (size_t i = 0; i < fh.size(); ++i)
                if (fh[i] == name) return std::stod(fRows[0][i]);
            return -999.0;
        };
        Check(col("period_ms") == 40.0 && col("sleep_ms") == 1.0, "slow frame: period 40 ms, sleep 1 ms");
        Check(col("pre_ms") == 3.0 && col("actor_ms") == 26.0 && col("post_ms") == 10.0,
              "slow frame: phases pre 3, actor 26 (tick group 12 + 14), post 10 (sync 8 + 2) ms");
        Check(col("tg3_ms") == 12.0 && col("tg0_ms") == 0.0 && col("sync_ms") == 8.0, "slow frame: TG_EndPhysics 12 ms, sync 8 ms");
        Check(col("pair_ms") == 6.0 && col("unit_ms") == 4.0 && col("units") == 2 && col("flow_ms") == 1.5,
              "slow frame: pairing 6 ms, units 4 ms (2), flows 1.5 ms (deltas of NpcCreationPacing's counters)");
        Check(col("grid_ms") == 5.0 && col("grid_n") == 1, "slow frame: the OT's grid change 5 ms, once");
        Check(col("gc_ms") == -1.0, "slow frame: GC unknown (-1)");
        Check((static_cast<int>(col("flags")) & 3) == 3, "slow frame: pre- and post-actor-tick marks seen");
    }
    Check(!sRows.empty(), "ledger csv: a 10-second summary row");
    if (!sRows.empty() && sRows[0].size() == sh.size()) {
        uint64_t frames = std::stoull(sRows[0][3]);
        uint64_t sumN = 0, slow = 0;
        for (int s = 0; s < 4; ++s) {
            sumN += std::stoull(sRows[0][6 + s * 10]);
            slow += std::stoull(sRows[0][8 + s * 10]);
        }
        Check(frames >= 600 && frames == sumN, "summary: frames counted, equal to the sum over states");
        Check(slow == 1, "summary: one slow frame");
    }

    // ---- cost per frame (real clock, real QPC; the freeze read is a fake-less no-op here) ----
    HitchLedgerTestSetup(nullptr, nullptr, nullptr, L"selftest-ledger-cost.csv");  // the fake-clock csv stays as it is
    SetHitchLedger(false, 25);
    LedgerFrameEnd(p);  // off: the frame state restarts on the real clock
    SetHitchLedger(true, 25);
    LARGE_INTEGER q0, q1, fq;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&q0);
    constexpr int kN = 200000;
    for (int i = 0; i < kN; ++i) {
        LedgerPreActorTick();
        LedgerPostActorTick();
        LedgerSleepBegin();
        LedgerFrameEnd(p);
    }
    QueryPerformanceCounter(&q1);
    const double ns = 1e9 * static_cast<double>(q1.QuadPart - q0.QuadPart) / static_cast<double>(fq.QuadPart) / kN;
    char msg[160];
    sprintf_s(msg, "ledger cost per frame (marks + frame end, no tick groups): %.0f ns (target < 50000 ns)", ns);
    Check(ns < 50000, msg);
    SetHitchLedger(false, 25);
    HitchLedgerDrain();  // drop the cost loop's records (fast frames: none expected)
    HitchLedgerTestClose();
}
}  // namespace hl
