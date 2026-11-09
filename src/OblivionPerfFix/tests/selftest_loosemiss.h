// LooseMissCache self-test (public since 1.3.0, worker port130j; lab fightot2 before), included by selftest.cpp: the discovery on
// the mapped exe (the file finder's find and its two _access calls through the import), the install (both calls redirected),
// then the game's own find runs in the mapped image with a fake _access in the import slot and a finder without extra folders
// (archive check skipped by flag 1): mode 0 always asks _access; mode 1 asks once per missing voice path, then answers -1
// itself; an existing path is never remembered; other path families and a failure other than ENOENT are asked every time.
#pragma once

namespace lmc {
int g_calls = 0;
int g_result = -1;
int g_errnoValue = 2;  // ENOENT
int* (__cdecl* g_errno)() = nullptr;
int __cdecl FakeAccess(const char*, int)
{
    ++g_calls;
    if (g_result == -1 && g_errno) *g_errno() = g_errnoValue;  // in the game's CRT
    return g_result;
}
alignas(16) uint8_t g_finder[0x40] = {};
}  // namespace lmc

static void LooseMissRun(const GameModule& game)
{
    LooseMissDiscovery d{};
    Check(LooseMissDiscover(game, &d), "LooseMissCache: discovery on the mapped exe");
    Check(d.finds == 1, "LooseMissCache: one file finder find");
    if (game.timeDateStamp == 0xF19077A4u)
        Check(d.find - game.base == 0x68D0270, "LooseMissCache: the find at exe+0x68D0270 (build 19115871)");
    if (!d.find) return;
    if (const HMODULE u = LoadLibraryW(L"ucrtbase.dll"))
        lmc::g_errno = reinterpret_cast<int* (__cdecl*)()>(GetProcAddress(u, "_errno"));
    Check(lmc::g_errno != nullptr, "LooseMissCache: ucrtbase's _errno for the fake");
    Check(LooseMissTestInstall(), "LooseMissCache: installs on the mapped exe");
    Check(d.find[0x61] == 0xE8 && d.find[0x66] == 0x90 && d.find[0x117] == 0xE8 && d.find[0x11C] == 0x90,
          "LooseMissCache: both _access calls redirected");
    Check(WritePointer(d.slot, reinterpret_cast<void*>(&lmc::FakeAccess)), "LooseMissCache: fake in the mapped exe's _access slot");
    using Find = int64_t (*)(void* finder, const char* path, uint32_t flags, int32_t arg);
    const auto find = reinterpret_cast<Find>(d.find);
    char path[260] = "Data\\Sound\\Voice\\Oblivion.esm\\Imperial\\M\\fo2_selftest.mp3";
    uint64_t k0, s0, k1, s1;

    SetLooseMissCache(0);
    LooseMissTestStats(&k0, &s0);
    lmc::g_calls = 0;
    find(lmc::g_finder, path, 1, -1);
    find(lmc::g_finder, path, 1, -1);
    LooseMissTestStats(&k1, &s1);
    Check(lmc::g_calls == 2 && k1 == k0 && s1 == s0, "LooseMissCache 0: every check asks _access, nothing remembered");

    SetLooseMissCache(1);
    lmc::g_calls = 0;
    find(lmc::g_finder, path, 1, -1);
    find(lmc::g_finder, path, 1, -1);
    find(lmc::g_finder, path, 1, -1);
    LooseMissTestStats(&k0, &s0);
    Check(lmc::g_calls == 1 && k0 == k1 + 2 && s0 == s1 + 1, "LooseMissCache 1: a missing voice path is asked once, then answered from memory");
    char other[260] = "Data\\Sound\\Voice\\Oblivion.esm\\Imperial\\M\\fo2_selftest2.mp3";
    find(lmc::g_finder, other, 1, -1);
    Check(lmc::g_calls == 2, "LooseMissCache 1: another path is asked");
    SetLooseMissCache(0);
    find(lmc::g_finder, path, 1, -1);
    Check(lmc::g_calls == 3, "LooseMissCache live 0: a remembered path is asked again");
    SetLooseMissCache(1);
    char exists[260] = "Data\\Sound\\Voice\\Oblivion.esm\\Imperial\\M\\fo2_selftest3.mp3";
    lmc::g_result = 0;  // the file exists
    lmc::g_calls = 0;
    find(lmc::g_finder, exists, 1, -1);
    find(lmc::g_finder, exists, 1, -1);
    Check(lmc::g_calls == 2, "LooseMissCache 1: an existing voice file is asked every time (never remembered)");
    lmc::g_result = -1;
    lmc::g_errnoValue = 13;  // EACCES: not "does not exist"
    char denied[260] = "Data\\Sound\\Voice\\Oblivion.esm\\Imperial\\M\\fo2_selftest4.mp3";
    lmc::g_calls = 0;
    find(lmc::g_finder, denied, 1, -1);
    find(lmc::g_finder, denied, 1, -1);
    Check(lmc::g_calls == 2, "LooseMissCache 1: a failure other than ENOENT is not remembered");
    lmc::g_errnoValue = 2;
    lmc::g_result = -1;

    // review fix: only the voice family is cached
    Check(LooseMissTestVoicePath("Data\\Sound\\Voice\\a.mp3") && LooseMissTestVoicePath("data/sound/voice/a.mp3") &&
              LooseMissTestVoicePath("SOUND\\VOICE\\x\\y.lip") && LooseMissTestVoicePath(".\\Data\\Sound\\Voice\\a.mp3") &&
              !LooseMissTestVoicePath("Data\\Sound\\fx\\a.wav") && !LooseMissTestVoicePath("Data\\Meshes\\Sound\\Voice\\a.nif") &&
              !LooseMissTestVoicePath("Data\\Sound\\Voices\\a.mp3") && !LooseMissTestVoicePath("Data\\SoundVoice\\a.mp3") &&
              !LooseMissTestVoicePath("Sound") &&
              LooseMissTestVoicePath("../../../OblivionRemastered/Content/Dev/ObvData/Data/Sound/Voice/x/y.mp3") &&
              !LooseMissTestVoicePath("../../../OblivionRemastered/Content/Dev/ObvData/Data/Menus/strings.xml") &&
              !LooseMissTestVoicePath("../MyData/Sound/Voice/a.mp3"),
          "LooseMissCache: the voice-path family (Data\\ optional, either slash, any case) and nothing else");
    char mesh[260] = "Data\\Meshes\\fo2_selftest.nif";
    SetLooseMissCache(1);
    lmc::g_calls = 0;
    find(lmc::g_finder, mesh, 1, -1);
    find(lmc::g_finder, mesh, 1, -1);
    Check(lmc::g_calls == 2, "LooseMissCache 1: a missing non-voice path is asked every time");
    SetLooseMissCache(0);
}
