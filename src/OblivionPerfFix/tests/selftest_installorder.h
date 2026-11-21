// Start-up install order self-test, included by selftest.cpp (worker repathfix 2026-11-20, from repathdefertest's finding: the
// CapSleepSkip install ran before GcPassAfterBusy's and found its readings missing, so the key was never installed). Init in
// core/main.cpp is not part of the self-test build, so its source text is read: every key whose Install* depends on another fix's
// install state must be installed after it (both calls in Init, in source order; Init runs top to bottom). The source folder
// comes from OPF_SELFTEST_SRC (build.ps1 sets it to the project folder); without it the test says so and is skipped.
#pragma once

#include <cstdlib>
#include <string>

static void InstallOrderRun()
{
    wchar_t dir[1024] = {};
    if (!GetEnvironmentVariableW(L"OPF_SELFTEST_SRC", dir, 1024)) {
        printf("  (start-up install order: OPF_SELFTEST_SRC not set, skipped)\n");
        return;
    }
    std::wstring path = std::wstring(dir) + L"\\src\\core\\main.cpp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) {
        Check(false, "start-up install order: src\\core\\main.cpp readable");
        return;
    }
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    // {prerequisite's install call, dependent's install call, what}: the dependent's Install* reads the prerequisite's state
    struct Pair { const char* pre; const char* dep; const char* what; };
    static const Pair pairs[] = {
        {"InstallGcPassAfterBusy(", "InstallCapSleepSkip(", "CapSleepSkip after GcPassAfterBusy (its GC timer and FApp::DeltaTime readings)"},
        {"g_pacingHooked = InstallNpcPacing(", "InstallCapSleepSkip(", "CapSleepSkip after NpcCreationPacing (its frame-rate cap hook)"},
        {"g_pacingHooked = InstallNpcPacing(", "InstallBodyRemovalSpread(", "BodyRemovalSpread after NpcCreationPacing (its frame hooks)"},
        {"g_npcBodies = InstallNpcBodiesOneLock(", "InstallSceneLockWaitProbe(", "SceneLockWaitProbe after NpcBodiesOneLock (the scene lock)"},
    };
    const size_t init = text.find("void Init(");
    for (const Pair& p : pairs) {
        const size_t dep = text.find(p.dep, init == std::string::npos ? 0 : init);
        if (dep == std::string::npos) {  // that key is not in this tree (a lab-only key in the public export)
            continue;
        }
        const size_t pre = text.find(p.pre, init == std::string::npos ? 0 : init);
        char what[256];
        snprintf(what, sizeof what, "start-up install order: %s", p.what);
        Check(init != std::string::npos && pre != std::string::npos && pre < dep, what);
    }
}
