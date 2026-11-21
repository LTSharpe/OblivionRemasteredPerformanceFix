// GcMarkPriority self-test (worker gc104, 2026-11-15), included by selftest.cpp.
// On the mapped exe (Steam 19115871): ProcessAsync's two callers' `lea rcx, [thunk]` at exe+0x1011687 / 0x10116C7, the
// ProcessSync thunks exe+0x100D9C0 / 0x100D9E0, both sites calling ProcessAsync exe+0x1031540 (nothing patched). Then the
// discovery's refusals and the 4-byte patch on a fake code buffer, and the wrapper with fake thunks on threads of
// different priorities (raised inside, restored after; a thread at or above the level untouched; key 0 untouched).
#pragma once

namespace gcmark_test {
// fake code: thunk A at 0x000, thunk B at 0x040, site 1 at 0x080, site 2 at 0x0C0, "ProcessAsync" at 0x180 (ret)
alignas(4096) uint8_t g_code[0x1000];
const uint8_t kThunk[24] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30,
                            0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
const uint8_t kSite[24] = {0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0xC2, 0x48, 0x8D, 0x0D, 0, 0, 0, 0,
                           0x48, 0x8D, 0x54, 0x24, 0x40, 0xE8, 0, 0, 0, 0};
void Rel32(size_t at, size_t insnEnd, size_t target)
{
    const int32_t d = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(insnEnd));
    memcpy(g_code + at, &d, 4);
}
void PutSite(size_t at, size_t thunk, size_t callee)
{
    memcpy(g_code + at, kSite, sizeof kSite);
    Rel32(at + 10, at + 14, thunk);   // lea rcx, [rip+thunk] (insn at +7, 7 bytes)
    Rel32(at + 20, at + 24, callee);  // call ProcessAsync (insn at +19, 5 bytes)
}
GameModule Fake(int variant)
{
    DWORD old;
    VirtualProtect(g_code, sizeof g_code, PAGE_READWRITE, &old);
    memset(g_code, 0xCC, sizeof g_code);
    memcpy(g_code + 0x000, kThunk, sizeof kThunk);
    memcpy(g_code + 0x040, kThunk, sizeof kThunk);
    g_code[0x180] = 0xC3;
    PutSite(0x080, 0x000, 0x180);
    PutSite(0x0C0, variant == 1 ? 0x000 : 0x040, variant == 2 ? 0x1C0 : 0x180);  // 1: same thunk; 2: another callee
    if (variant == 3) g_code[0x040 + 23] = 0x90;                                   // thunk B of another shape
    if (variant == 4) PutSite(0x100, 0x040, 0x180);                                  // a third site
    if (variant == 5) g_code[0x0C0 + 4] = 0x48;                                      // only one site
    GameModule m;
    m.base = g_code;
    m.text = Section{g_code, g_code + sizeof g_code};
    m.sizeOfImage = sizeof g_code;
    return m;
}

int g_inside[2] = {-99, -99};
int g_thunkCalls = 0;
void* g_seenA = nullptr;
void* g_seenB = nullptr;
void __fastcall FakeThunk0(void* a, void* b) { g_inside[0] = GetThreadPriority(GetCurrentThread()); g_seenA = a; g_seenB = b; ++g_thunkCalls; }
void __fastcall FakeThunk1(void* a, void* b) { g_inside[1] = GetThreadPriority(GetCurrentThread()); g_seenA = a; g_seenB = b; ++g_thunkCalls; }
using Wrapper = void(__fastcall*)(void*, void*);

struct Run { int start, inside, after; };
// one call of Wrap<which> on a new thread that starts at `prio`
Run CallAt(int which, int prio)
{
    struct Arg { int which, prio; Run r; } arg{which, prio, {-99, -99, -99}};
    HANDLE t = CreateThread(nullptr, 0, [](void* p) -> DWORD {
        Arg& a = *static_cast<Arg*>(p);
        SetThreadPriority(GetCurrentThread(), a.prio);
        a.r.start = GetThreadPriority(GetCurrentThread());
        reinterpret_cast<Wrapper>(GcMarkPriorityTestWrapper(a.which))(reinterpret_cast<void*>(0x1234), reinterpret_cast<void*>(0x5678));
        a.r.inside = g_inside[a.which];
        a.r.after = GetThreadPriority(GetCurrentThread());
        return 0;
    }, &arg, 0, nullptr);
    if (t) { WaitForSingleObject(t, 5000); CloseHandle(t); }
    return arg.r;
}

}  // namespace gcmark_test

static void GcMarkRun(const GameModule& game)
{
    using namespace gcmark_test;
    if (game.timeDateStamp == 0xF19077A4u) {
        uint32_t r[5] = {};
        const bool ok = GcMarkPriorityTestFind(game, r);
        Check(ok && r[0] == 0x1011687 && r[1] == 0x10116C7 && r[2] == 0x100D9C0 && r[3] == 0x100D9E0 && r[4] == 0x1031540,
              "GcMarkPriority: ProcessAsync's two call sites, their ProcessSync thunks and ProcessAsync found on the mapped exe");
        if (!ok || r[0] != 0x1011687) printf("    found %d: lea %X %X thunk %X %X callee %X\n", ok, r[0], r[1], r[2], r[3], r[4]);
    }
    {
        uint32_t r[5] = {};
        GameModule m = Fake(0);
        Check(GcMarkPriorityTestFind(m, r) && r[0] == 0x087 && r[1] == 0x0C7 && r[2] == 0x000 && r[3] == 0x040 && r[4] == 0x180,
              "GcMarkPriority: discovery on a fake code buffer (two sites, two thunks, one callee)");
        const char* const names[] = {"", "both sites pass one thunk", "the sites call different functions", "a thunk of another shape",
                                     "three sites", "one site"};
        bool refused = true;
        for (int v = 1; v <= 5; ++v) {
            GameModule mv = Fake(v);
            if (GcMarkPriorityTestFind(mv, r)) { refused = false; printf("    not refused: %s\n", names[v]); }
        }
        Check(refused, "GcMarkPriority: refused when both sites pass one thunk, call different functions, a thunk has another "
                       "shape, or there are three sites or one");
        m = Fake(0);
        uint8_t before[0x100];
        memcpy(before, g_code, sizeof before);
        const char* why = GcMarkPriorityTestPatch(m);
        // only bytes 0x08A-0x08D and 0x0CA-0x0CD (the two displacements) changed; each lea now points at a jmp [rip+0] to Wrap<i>
        bool onlyDisp = why == nullptr;
        for (size_t i = 0; i < sizeof before && onlyDisp; ++i) {
            const bool disp = (i >= 0x08A && i < 0x08E) || (i >= 0x0CA && i < 0x0CE);
            if (!disp && g_code[i] != before[i]) onlyDisp = false;
        }
        bool stubs = onlyDisp;
        for (int i = 0; i < 2 && stubs; ++i) {
            const uint8_t* stub = RipTarget(g_code + (i ? 0x0C7 : 0x087), 3, 7);
            void* target = nullptr;
            memcpy(&target, stub + 6, sizeof target);
            stubs = stub[0] == 0xFF && stub[1] == 0x25 && stub[2] == 0 && stub[3] == 0 && stub[4] == 0 && stub[5] == 0 &&
                    target == GcMarkPriorityTestWrapper(i);
        }
        Check(stubs, "GcMarkPriority: the patch writes only the two lea displacements, each pointing at a jump to its wrapper");
        if (!stubs) printf("    why %s, only displacements %d\n", why ? why : "-", onlyDisp ? 1 : 0);
    }
    {
        uint64_t c[3];
        GcMarkPriorityTestSetup(reinterpret_cast<void*>(&FakeThunk0), reinterpret_cast<void*>(&FakeThunk1), 1);
        const Run bl = CallAt(0, THREAD_PRIORITY_BELOW_NORMAL);
        const Run lo = CallAt(1, THREAD_PRIORITY_LOWEST);
        const Run nm = CallAt(0, THREAD_PRIORITY_NORMAL);
        const Run an = CallAt(1, THREAD_PRIORITY_ABOVE_NORMAL);
        GcMarkPriorityTestCounts(c);
        Check(bl.start == THREAD_PRIORITY_BELOW_NORMAL && bl.inside == THREAD_PRIORITY_NORMAL && bl.after == THREAD_PRIORITY_BELOW_NORMAL &&
                  lo.start == THREAD_PRIORITY_LOWEST && lo.inside == THREAD_PRIORITY_NORMAL && lo.after == THREAD_PRIORITY_LOWEST,
              "GcMarkPriority 1: a below-normal / lowest worker runs its mark part at normal and goes back to its own priority after");
        Check(nm.inside == THREAD_PRIORITY_NORMAL && nm.after == THREAD_PRIORITY_NORMAL && an.inside == THREAD_PRIORITY_ABOVE_NORMAL &&
                  an.after == THREAD_PRIORITY_ABOVE_NORMAL && c[0] == 4 && c[1] == 2 && c[2] == 0,
              "GcMarkPriority 1: a normal and an above-normal caller untouched; 4 parts counted, 2 raised");
        Check(g_seenA == reinterpret_cast<void*>(0x1234) && g_seenB == reinterpret_cast<void*>(0x5678),
              "GcMarkPriority: the wrapper passes both registers to the game's thunk unchanged");
        GcMarkPriorityTestSetup(reinterpret_cast<void*>(&FakeThunk0), reinterpret_cast<void*>(&FakeThunk1), 2);
        const Run b2 = CallAt(0, THREAD_PRIORITY_BELOW_NORMAL);
        const Run n2 = CallAt(1, THREAD_PRIORITY_NORMAL);
        const Run h2 = CallAt(0, THREAD_PRIORITY_HIGHEST);
        GcMarkPriorityTestCounts(c);
        Check(b2.inside == THREAD_PRIORITY_ABOVE_NORMAL && b2.after == THREAD_PRIORITY_BELOW_NORMAL && n2.inside == THREAD_PRIORITY_ABOVE_NORMAL &&
                  n2.after == THREAD_PRIORITY_NORMAL && h2.inside == THREAD_PRIORITY_HIGHEST && h2.after == THREAD_PRIORITY_HIGHEST && c[1] == 2,
              "GcMarkPriority 2: below-normal and normal callers run at above normal and go back; a highest caller untouched");
        GcMarkPriorityTestSetup(reinterpret_cast<void*>(&FakeThunk0), reinterpret_cast<void*>(&FakeThunk1), 0);
        g_thunkCalls = 0;
        const Run b0 = CallAt(0, THREAD_PRIORITY_BELOW_NORMAL);
        GcMarkPriorityTestCounts(c);
        Check(b0.inside == THREAD_PRIORITY_BELOW_NORMAL && b0.after == THREAD_PRIORITY_BELOW_NORMAL && g_thunkCalls == 1 && c[0] == 0,
              "GcMarkPriority 0: the wrapper only calls the game's thunk (priority untouched, nothing counted)");
        GcMarkPriorityTestSetup(nullptr, nullptr, 0);
    }
}
