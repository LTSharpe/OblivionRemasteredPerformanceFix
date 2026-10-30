// VramCounterFix (game bug) self-test, included by selftest.cpp: the query and its two calls on the mapped
// exe, and the wrapper with a fake query and a fake DXGI sample (0/0 filled, the game's own numbers passed through, off
// passes through, no sample yet leaves 0/0).
#pragma once

namespace vramc {

inline int32_t g_fakeBudget = 0, g_fakeUsed = 0;
inline void FakeQuery(int32_t* budget, int32_t* used)
{
    *budget = g_fakeBudget;
    *used = g_fakeUsed;
}
inline bool g_haveSample = true;
inline bool FakeLatest(uint64_t* used, uint64_t* budget)
{
    *used = g_haveSample ? 5321ull << 20 : 0;
    *budget = g_haveSample ? 30976ull << 20 : 0;
    return g_haveSample;
}

inline void Run(const opf::GameModule& game)
{
    using namespace opf;
    Check(ApplyVramCounterFix(true), "vram counter: query and its two calls found, calls redirected");
    VramCounterInfo vi{};
    if (!Installed(VramCounterFixInfo(&vi), "VramCounterFix (Show VRAM counter)")) return;
    Check(vi.query == game.base + 0x47B6AF0, "vram counter: the query at exe+0x47B6AF0");
    Check(vi.calls[0] == game.base + 0x48C2511, "vram counter: the widget's call at exe+0x48C2511");
    Check(vi.calls[1] == game.base + 0x46019EB, "vram counter: GetUsedVRAMMB's call at exe+0x46019EB");
    Check(RipTarget(vi.query + 22, 3, 7) == game.base + 0x9206108, "vram counter: the query reads GDynamicRHI (exe+0x9206108)");
    for (int i = 0; i < 2; ++i)
        Check(vi.calls[i][0] == 0xE8 && RipTarget(vi.calls[i], 1, 5) == vi.stub, "vram counter: a call goes to the stub");
    Check(*reinterpret_cast<void* const*>(vi.stub + 6) == vi.hook, "vram counter: the stub jumps to the wrapper");
    Check(ApplyVramCounterFix(true), "vram counter: second installation finds the fix in place");

    using QueryFn = void (*)(int32_t*, int32_t*);
    const auto hook = reinterpret_cast<QueryFn>(vi.hook);
    VramCounterFixTestSetup(reinterpret_cast<void*>(&FakeQuery), &FakeLatest);
    SetVramCounterFix(true);
    int32_t budget = -1, used = -1;
    g_fakeBudget = g_fakeUsed = 0;
    hook(&budget, &used);
    Check(budget == 30976 && used == 5321, "vram counter: the game's 0/0 filled from DXGI (used 5321 / budget 30976 MB)");
    g_fakeBudget = 24000;
    g_fakeUsed = 7000;
    hook(&budget, &used);
    Check(budget == 24000 && used == 7000, "vram counter: the game's own numbers passed through");
    g_fakeBudget = g_fakeUsed = 0;
    g_haveSample = false;
    hook(&budget, &used);
    Check(budget == 0 && used == 0, "vram counter: no DXGI sample yet: 0/0 as the game");
    g_haveSample = true;
    SetVramCounterFix(false);
    hook(&budget, &used);
    Check(budget == 0 && used == 0, "vram counter: switched off: 0/0 as the game");
    SetVramCounterFix(true);
    VramCounterFixInfo(&vi);
    Check(vi.filled == 1 && vi.gameValues == 1 && vi.unknown == 1, "vram counter: filled, passed-through and early readings counted");
    LogVramCounterFixStats();
    VramCounterFixTestSetup(nullptr, nullptr);
}

}  // namespace vramc
