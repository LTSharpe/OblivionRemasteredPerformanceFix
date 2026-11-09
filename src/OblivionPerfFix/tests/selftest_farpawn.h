// FarPawnMoveHalfRate / FarHorseMove self-test (public since 1.3.0, worker port130j), included by selftest.cpp: on the mapped exe
// the constructors, vtables, TickComponent, its CharacterOwner / Mesh / UpdatedComponent reads, IsPlayingRootMotion, the vtable's
// IsSwimming / UpdateComponentVelocity, GetCurrentProcedureType and the property-table pins are found (nothing installed). Then
// the cadence (FarPawnDecide) at 40/50/60/90/144 fps, time conservation, the max step, the carried-time flush, root motion
// (never a summed step; carried time kept), the first-phase split (rev130e F-S2), and the hook's path on fake objects:
// distance, procedure, speed, swimming, attached, root motion (F-M1), the input mean (F-S1), live off, horses with riders.
#pragma once

namespace fpm {
struct Call {
    void* comp;
    float dt;
    double inputX;
};
Call g_calls[256];
int g_nCalls = 0;
uint8_t* g_inputPawn = nullptr;  // whose input x the fake tick records
void FakeTick(void* comp, float dt, int, void*)
{
    g_calls[g_nCalls & 255] = {comp, dt, g_inputPawn ? *reinterpret_cast<double*>(g_inputPawn + 0x2E8) : 0.0};
    ++g_nCalls;
    if (g_inputPawn) memset(g_inputPawn + 0x2E8, 0, 24);  // consumed, as the game's tick does
}
uint8_t FakeProc(const void* pawn)  // as GetCurrentProcedureType: 0 without an (AI) controller
{
    const uint8_t* p = static_cast<const uint8_t*>(pawn);
    return *reinterpret_cast<void* const*>(p + 0x2D0) ? p[0x448] : 0;
}
bool FakeRootMotion(const void* pawn) { return static_cast<const uint8_t*>(pawn)[0x11] != 0; }
bool FakePlayer(double* x, double* y, double* z)
{
    *x = *y = *z = 0.0;
    return true;
}

alignas(16) uint8_t g_comp[2][0x300], g_upd[2][0x300], g_pawn[3][0x1000], g_ctrl[0x10], g_rroot[0x300];

void PutPtr(uint8_t* at, const void* p) { memcpy(at, &p, 8); }
void SetLoc(uint8_t* upd, double x)
{
    double l[3] = {x, 0.0, 0.0};
    memcpy(upd + 0x280, l, sizeof l);
}
void Reset()
{
    memset(g_comp, 0, sizeof g_comp);
    memset(g_upd, 0, sizeof g_upd);
    memset(g_pawn, 0, sizeof g_pawn);
    for (int i = 0; i < 2; ++i) {
        PutPtr(g_comp[i] + 0x180, g_pawn[i]);
        PutPtr(g_comp[i] + 0xB8, g_upd[i]);
        PutPtr(g_pawn[i] + 0x2D0, g_ctrl);
        g_pawn[i][0x448] = 2;  // WANDER
        SetLoc(g_upd[i], 20000.0);
    }
    g_nCalls = 0;
    g_inputPawn = nullptr;
}
int Run(int frames, float dt, int comp = 0, bool horse = false)
{
    const int before = g_nCalls;
    for (int i = 0; i < frames; ++i) opf::FarPawnMoveTestTick(g_comp[comp], dt, horse);
    return g_nCalls - before;
}

inline void FarPawnSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    NpcPacingInfoOut pi{};
    const uint32_t root = NpcPacingInfo(&pi) ? pi.rootOffset : 0;
    FarPawnMoveDiscovery d;
    const bool found = FarPawnMoveDiscover(game, root, &d);
    Check(found, "farmove: paired/horse constructors, vtables, TickComponent in slot 127 of both, its CharacterOwner / Mesh / "
                 "UpdatedComponent reads, IsPlayingRootMotion, IsSwimming / UpdateComponentVelocity, GetCurrentProcedureType, "
                 "execGetAttachParent's AttachParent read, property-table pins (ControlInputVector, RootComponent, "
                 "Rider) found");
    if (!found) Log("[selftest] farmove: %s not recognised", d.missing ? d.missing : "?");
    if (found && game.timeDateStamp == 0xF19077A4u) {
        const auto rva = [&](const void* p) { return static_cast<uint64_t>(static_cast<const uint8_t*>(p) - game.base); };
        Check(rva(d.pawnVtable) == 0x7D038F0 && d.horseVtable && rva(d.horseVtable) == 0x7C57B40 && rva(d.tick) == 0x3004340 &&
                  rva(d.getProc) == 0x4856F60 && rva(d.isRootMotion) == 0x2F83490 && rva(d.getAttachParent) == 0x2D90D00,
              "farmove: on 19115871 vtables exe+0x7D038F0 / exe+0x7C57B40, tick exe+0x3004340, getter exe+0x4856F60, "
              "IsPlayingRootMotion exe+0x2F83490, execGetAttachParent exe+0x2D90D00 (AttachParent +0xC8, FP-S1)");
    }

    // Cadence (pure decision): steady state, time conservation.
    const float p30 = 1.f / 30.f;
    struct Rate { float fps; int every; };
    const Rate rates[] = {{40.f, 1}, {50.f, 2}, {60.f, 2}, {90.f, 3}, {144.f, 5}};
    bool cadence = true, conserved = true;
    for (const Rate& r : rates) {
        FarPawnStep s;
        const float dt = 1.f / r.fps;
        int ticks = 0, lastTick = -1, gapsOk = 0, gaps = 0;
        double sum = 0.0;
        for (int f = 0; f < 600; ++f) {
            float step = 0.f;
            if (FarPawnDecide(s, dt, p30, 0.4f, true, false, 0.3f, &step)) {
                ++ticks;
                sum += step;
                if (lastTick >= 0 && f > 20) { ++gaps; gapsOk += (f - lastTick) == r.every; }
                lastTick = f;
            }
        }
        cadence = cadence && gaps > 0 && gapsOk == gaps;
        conserved = conserved && std::fabs(sum + s.acc - 600.0 * dt) < 1e-3;
    }
    Check(cadence, "farmove: 30 Hz ticks every frame at 40 fps, every 2nd at 50 / 60 fps, every 3rd at 90, every 5th at 144");
    Check(conserved, "farmove: summed steps plus the carried time equal the frames' time (nothing lost)");
    {
        FarPawnStep s;  // max step: 2.5 Hz with 0.1 s frames ticks before carrying more than 0.4 s; a long frame ticks at once
        float step = 0.f;
        int skips = 0;
        while (!FarPawnDecide(s, 0.1f, 0.4f, 0.4f, true, false, 0.99f, &step) && skips < 10) ++skips;
        const bool bound = step <= 0.4001f && skips <= 3;
        FarPawnStep t;
        const bool longFrame = FarPawnDecide(t, 0.5f, p30, 0.4f, true, false, 0.9f, &step) && std::fabs(step - 0.5f) < 1e-6f;
        Check(bound && longFrame, "farmove: no carried step over FarPawnMoveMaxStepMs; a frame longer than it ticks at once");
    }
    {
        FarPawnStep s;
        float step = 0.f;
        const float dt = 1.f / 60.f;
        const bool skip1 = !FarPawnDecide(s, dt, p30, 0.4f, true, false, 0.9f, &step) && s.n == 1;
        // root motion after a skip: its own frame's time only, the carried time stays
        const bool rm = FarPawnDecide(s, dt, p30, 0.4f, false, true, 0.9f, &step) && std::fabs(step - dt) < 1e-6f &&
                        std::fabs(s.acc - dt) < 1e-6f && s.n == 0;
        // then near (not qualifying, no root motion): the carried time is flushed
        const bool flush = FarPawnDecide(s, dt, p30, 0.4f, false, false, 0.9f, &step) && std::fabs(step - 2 * dt) < 1e-6f && s.acc == 0.f;
        Check(skip1 && rm && flush, "farmove: root motion ticks with its own frame's time (carried time kept); the next ordinary "
                                    "tick gets it");
    }
    {
        int skipFirst = 0;  // F-S2: pawns starting in the same frame at 30 Hz / 60 fps split about 50/50
        for (int i = 0; i < 1000; ++i) {
            FarPawnStep s;
            float step = 0.f;
            skipFirst += !FarPawnDecide(s, 1.f / 60.f, p30, 0.4f, true, false, (i + 0.5f) / 1000.f, &step);
        }
        Check(skipFirst > 450 && skipFirst < 550, "farmove: pawns that start qualifying together split 50/50 over the two frames");
    }

    // The hook's path on fakes.
    FarPawnMoveTestFakes f{0x1A0, 0x280, reinterpret_cast<void*>(&FakeProc), reinterpret_cast<void*>(&FakeRootMotion), &FakePlayer,
                           reinterpret_cast<void*>(&FakeTick)};
    FarPawnMoveTestSetup(f);
    SetFarPawnMove(1, 30, 2);
    const float dt = 1.f / 60.f;
    Reset();
    const int farCalm = Run(20, dt);
    double total = 0.0;
    for (int i = 0; i < g_nCalls; ++i) total += g_calls[i].dt;
    Check(farCalm >= 10 && farCalm <= 11 && total <= 20 * dt + 1e-4 && total >= 19 * dt - 1e-4,
          "farmove: a far calm pawn at walking pace ticks every 2nd frame at 60 fps with the summed step");
    Reset();
    SetLoc(g_upd[0], 5000.0);
    const int nearOne = Run(10, dt);
    Reset();
    g_pawn[0][0x448] = 10;  // COMBAT
    const int busy = Run(10, dt);
    Reset();
    g_comp[0][0x221] = 4;  // swimming
    const int swim = Run(10, dt);
    Reset();
    PutPtr(g_upd[0] + 0xC8, g_rroot);  // attached
    const int attached = Run(10, dt);
    Reset();
    memset(g_pawn[0] + 0x2D0, 0, 8);  // no AI controller (the player)
    const int noCtrl = Run(10, dt);
    Reset();
    const double fast[3] = {400.0, 0.0, 0.0};
    memcpy(g_comp[0] + 0xD0, fast, sizeof fast);
    const int fast1 = Run(10, dt);
    SetFarPawnMove(2, 30, 2);
    const int fast2 = Run(10, dt);
    SetFarPawnMove(1, 30, 2);
    Check(nearOne == 10 && busy == 10 && swim == 10 && attached == 10 && noCtrl == 10 && fast1 == 10 && fast2 <= 6,
          "farmove: near, combat, swimming, attached, no AI controller and (at 1) running pawns tick every frame; 2 = any speed");
    Reset();
    g_pawn[0][0x11] = 1;  // root motion all along
    const int rmAll = Run(10, dt);
    bool rmSteps = true;
    for (int i = 0; i < g_nCalls; ++i) rmSteps = rmSteps && std::fabs(g_calls[i].dt - dt) < 1e-6f;
    Check(rmAll == 10 && rmSteps, "farmove: a pawn playing root motion ticks every frame with its own frame's step (F-M1)");
    Reset();
    int k = 0;  // find a frame that skipped, then start root motion
    while (k < 4 && Run(1, dt) == 1) ++k;
    g_pawn[0][0x11] = 1;
    const int n0 = g_nCalls;
    Run(1, dt);
    const bool rmNoSum = g_nCalls == n0 + 1 && std::fabs(g_calls[n0].dt - dt) < 1e-6f;
    g_pawn[0][0x11] = 0;
    SetLoc(g_upd[0], 5000.0);
    Run(1, dt);
    const bool rmThenFlush = g_nCalls == n0 + 2 && std::fabs(g_calls[n0 + 1].dt - 2 * dt) < 1e-6f;
    Check(k < 4 && rmNoSum && rmThenFlush, "farmove: root motion starting on a skipped frame: no summed step; the carried time "
                                           "goes to the next ordinary tick");
    Reset();
    g_inputPawn = g_pawn[0];
    int skipped = 0;
    for (int i = 0; i < 6; ++i) {  // each frame adds input 0.5; a tick after one skip sees the mean 0.5, not 1.0
        double in = *reinterpret_cast<double*>(g_pawn[0] + 0x2E8) + 0.5;
        memcpy(g_pawn[0] + 0x2E8, &in, 8);
        skipped += Run(1, dt) == 0;
    }
    bool mean = skipped >= 2;
    for (int i = 0; i < g_nCalls; ++i) mean = mean && std::fabs(g_calls[i].inputX - 0.5) < 1e-9;
    Check(mean, "farmove: inputs added on skipped frames reach the tick as their mean (F-S1)");
    Reset();
    Run(1, dt);
    Run(1, dt);
    SetFarPawnMove(0, 30, 0);
    const int off = Run(10, dt);
    SetFarPawnMove(1, 30, 2);
    Check(off == 10, "farmove: live 0: every frame");

    // Horses (comp 1 = a horse's movement component; pawn 1 = the horse, pawn 2 = its rider).
    Reset();
    memset(g_pawn[1] + 0x2D0, 0, 8);  // a riderless horse without a controller: not throttled
    const int hNone = Run(10, dt, 1, true);
    Reset();
    const int hCalm = Run(10, dt, 1, true);  // a calm controller
    Reset();
    memset(g_pawn[1] + 0x2D0, 0, 8);
    PutPtr(g_pawn[1] + 0xE30, g_pawn[2]);  // an NPC rider
    PutPtr(g_pawn[2] + 0x1A0, g_rroot);
    memset(g_rroot, 0, sizeof g_rroot);
    SetLoc(g_rroot, 20100.0);  // on the horse
    PutPtr(g_pawn[2] + 0x2D0, g_ctrl);
    g_pawn[2][0x448] = 1;  // TRAVEL
    const int hRidden = Run(10, dt, 1, true);
    SetFarPawnMove(1, 30, 1);
    const int hRidden1 = Run(10, dt, 1, true);
    SetFarPawnMove(1, 30, 2);
    g_pawn[2][0x448] = 7;  // FOLLOW
    const int hFollow = Run(10, dt, 1, true);
    g_pawn[2][0x448] = 1;
    SetLoc(g_rroot, 21000.0);  // dismounting
    const int hOff = Run(10, dt, 1, true);
    Check(hNone == 10 && hCalm <= 6 && hRidden <= 6 && hRidden1 == 10 && hFollow == 10 && hOff == 10,
          "farmove: horses: calm riderless throttled, no controller and no rider not; at 2 a calm NPC rider sitting on it "
          "throttled, at 1 not; a rider following someone or not on the horse: every frame");
    FarPawnMoveTestSetup(FarPawnMoveTestFakes{0, 0, nullptr, nullptr, nullptr, nullptr});
    SetFarPawnMove(0, 30, 0);
}
}  // namespace fpm
