// NavQueryPriority self-test (worker navprio 2026-11-18), included by selftest.cpp: on the mapped exe the name of Epic's
// TaskGraph.TaskPriorities.NavTriggerAsyncQueries (once in .rdata, referenced from code); then the game-thread rules with a
// fake console variable (IConsoleVariable vtable: GetFlags +0x18, IsVariableString +0x48, Set +0x80; Set keeps the engine's
// priority rule): "nnn" through Set at the project priority while nobody set the variable, nothing while the key is 0, live
// off puts "bnn" back, live on again, a value of the player's (Engine.ini, console) or anyone's is left alone.
#pragma once

namespace navprio_test {
struct NpCvar {
    void** vt;
    wchar_t v[8];
    uint32_t flags;
    int sets;
};
uint32_t GetFlags(void* c) { return static_cast<NpCvar*>(c)->flags; }
bool IsString(void*) { return true; }
void Set(void* c, const wchar_t* v, uint32_t by)
{
    NpCvar* const f = static_cast<NpCvar*>(c);
    if ((by & 0xFF000000u) < (f->flags & 0xFF000000u)) return;  // the engine's CanChange
    wcsncpy_s(f->v, v, 7);
    f->flags = by & 0xFF000000u;
    ++f->sets;
}
void* g_vt[32];
}  // namespace navprio_test

static void NavPrioRun()
{
    using navprio_test::NpCvar; using navprio_test::g_vt;
    Check(NavQueryPriorityNameInGame(), "navprio: TaskGraph.TaskPriorities.NavTriggerAsyncQueries found once in .rdata and referenced from code");
    g_vt[0x18 / 8] = reinterpret_cast<void*>(&navprio_test::GetFlags);
    g_vt[0x48 / 8] = reinterpret_cast<void*>(&navprio_test::IsString);
    g_vt[0x80 / 8] = reinterpret_cast<void*>(&navprio_test::Set);
    auto same = [](const NpCvar& c, const wchar_t* v) { return wcscmp(c.v, v) == 0; };
    {
        NpCvar c{g_vt, L"bnn", 0, 0};  // the constructor's value and priority
        NavQueryPriorityTestSetup(&c);
        NavQueryPriorityOnGameThread();
        Check(same(c, L"bnn") && c.sets == 0, "navprio: key 0 (default) leaves the game's \"bnn\" (no Set)");
        Check(NavQueryPriorityNameState() == 0, "navprio: the game-thread call never checks the name in the exe (the monitor thread does)");
        SetNavQueryPriority(1);
        Check(NavQueryPriorityNameState() == 1, "navprio: the monitor thread's key 1 checks the name first");
        NavQueryPriorityOnGameThread();
        Check(same(c, L"nnn") && c.sets == 1 && c.flags == 0x03000000u, "navprio: key 1 sets \"nnn\" at the project priority");
        NavQueryPriorityOnGameThread();
        NavQueryPriorityOnGameThread();
        Check(c.sets == 1, "navprio: settled: no further Set");
        SetNavQueryPriority(0);
        NavQueryPriorityOnGameThread();
        Check(same(c, L"bnn") && c.sets == 2, "navprio: live key 0 puts the game's \"bnn\" back");
        NavQueryPriorityOnGameThread();
        Check(c.sets == 2, "navprio: key 0 stays off (no further Set)");
        SetNavQueryPriority(1);
        NavQueryPriorityOnGameThread();
        Check(same(c, L"nnn") && c.sets == 3, "navprio: live key 1 again sets \"nnn\" (the variable is at our priority)");
        navprio_test::Set(&c, L"hhh", 0x09000000u);  // the player's console
        SetNavQueryPriority(0);
        NavQueryPriorityOnGameThread();
        Check(same(c, L"hhh") && c.sets == 4, "navprio: the player's later value is not put back over");
        SetNavQueryPriority(1);
        NavQueryPriorityOnGameThread();
        Check(same(c, L"hhh") && c.sets == 4, "navprio: key 1 over the player's value: left alone");
    }
    {
        NpCvar c{g_vt, L"nbn", 0x04000000u, 0};  // Engine.ini [SystemSettings] value
        NavQueryPriorityTestSetup(&c);
        SetNavQueryPriority(1);
        NavQueryPriorityOnGameThread();
        Check(same(c, L"nbn") && c.sets == 0, "navprio: an Engine.ini value is left alone");
        SetNavQueryPriority(0);
        NavQueryPriorityOnGameThread();
        Check(same(c, L"nbn") && c.sets == 0, "navprio: ... also when the key goes off");
    }
    {
        NpCvar c{g_vt, L"bnn", 0, 0};
        NavQueryPriorityTestSetup(&c);
        SetNavQueryPriority(1);
        NavQueryPriorityOnGameThread();
        navprio_test::Set(&c, L"nbn", 0x04000000u);  // set by someone after ours
        NavQueryPriorityOnGameThread();
        SetNavQueryPriority(0);
        NavQueryPriorityOnGameThread();
        Check(same(c, L"nbn") && c.sets == 2, "navprio: a value set after ours (higher priority) is not overwritten when the key goes off");
        SetNavQueryPriority(7);  // out of range = off
        NavQueryPriorityOnGameThread();
        Check(c.sets == 2, "navprio: out-of-range key = off");
    }
    NavQueryPriorityTestSetup(nullptr);
}
