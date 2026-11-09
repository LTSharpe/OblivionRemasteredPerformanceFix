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
    limit.f = 10000.0f;  // Altar's loading mode
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
    StreamGranTestSetup(nullptr, nullptr);
}
