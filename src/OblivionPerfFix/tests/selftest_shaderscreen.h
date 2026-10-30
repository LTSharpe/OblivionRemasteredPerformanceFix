// ShaderScreenAfterDriverChange (game bug) self-test, included by selftest.cpp: the start-up's Save_Settings
// check on the mapped exe, and the wrapper with a fake DoesSaveGameExist, a fake graphics card and a record file in the
// temp folder (first record, unchanged, changed once, no settings save).
#pragma once

#include <string>

namespace shs {

inline bool g_exists = true;
inline bool FakeExists(const void*, int32_t) { return g_exists; }
inline const char* g_gpu = "10DE 2B85 00000000 A1 32.0.15.8180";
inline bool FakeGpu(char* out, size_t size)
{
    snprintf(out, size, "%s", g_gpu);
    return true;
}

inline std::string ReadAll(const std::wstring& path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return "";
    char buf[128] = "";
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return std::string(buf, n);
}

inline void Run(const opf::GameModule& game)
{
    using namespace opf;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir(tmp);
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    Check(ApplyShaderScreenAfterDriverChange(true, dir.c_str()), "shader screen: the start-up's Save_Settings check found, call redirected");
    uint8_t *call = nullptr, *orig = nullptr, *stub = nullptr;
    void* hook = nullptr;
    if (!Installed(ShaderScreenInfo(&call, &orig, &stub, &hook), "ShaderScreenAfterDriverChange (shader screen after a driver change)")) return;
    Check(call == game.base + 0x44E12C4, "shader screen: the start-up calls DoesSaveGameExist at exe+0x44E12C4");
    Check(orig == game.base + 0x31A4F30, "shader screen: DoesSaveGameExist at exe+0x31A4F30");
    Check(call[0] == 0xE8 && RipTarget(call, 1, 5) == stub && *reinterpret_cast<void* const*>(stub + 6) == hook,
          "shader screen: the call goes to the stub, the stub to the wrapper");
    Check(call[15] == 0x88 && call[16] == 0x87 && call[17] == 0x89, "shader screen: the answer goes to the flag at +0x89");

    using ExistsFn = bool (*)(const void*, int32_t);
    const auto wrap = reinterpret_cast<ExistsFn>(hook);
    const std::wstring record = dir + L"\\OblivionPerfFix-gpu-selftest.txt";
    DeleteFileW(record.c_str());
    g_exists = true;
    ShaderScreenTestSetup(reinterpret_cast<void*>(&FakeExists), &FakeGpu, record.c_str());
    Check(wrap(nullptr, 0) && ReadAll(record).rfind("10DE 2B85", 0) == 0, "shader screen: no record yet: recorded, the game's answer");
    ShaderScreenTestSetup(reinterpret_cast<void*>(&FakeExists), &FakeGpu, record.c_str());
    Check(wrap(nullptr, 0), "shader screen: same card and driver: the game's answer (save exists)");
    g_gpu = "10DE 2B85 00000000 A1 32.0.15.8215";
    ShaderScreenTestSetup(reinterpret_cast<void*>(&FakeExists), &FakeGpu, record.c_str());
    Check(!wrap(nullptr, 0), "shader screen: driver changed: \"no settings save\" (the first-launch path)");
    PsoPrecompileInfoOut pso{};
    if (PsoPrecompileInfo(&pso)) Check(PsoPrecompileScreenHold(), "shader screen: PsoPrecompileFast told to run the screen at the Fast pace");
    Check(!wrap(nullptr, 0), "shader screen: the same start-up asks again: the same answer");
    Check(ReadAll(record).find("32.0.15.8180") != std::string::npos, "shader screen: the old record kept until the precompile has finished");
    ShaderScreenPrecompileFinished();
    Check(ReadAll(record).find("32.0.15.8215") != std::string::npos, "shader screen: the new driver recorded once it has finished");
    ShaderScreenTestSetup(reinterpret_cast<void*>(&FakeExists), &FakeGpu, record.c_str());
    Check(wrap(nullptr, 0), "shader screen: the next start-up with that driver: the game's answer");
    g_exists = false;
    ShaderScreenTestSetup(reinterpret_cast<void*>(&FakeExists), &FakeGpu, record.c_str());
    Check(!wrap(nullptr, 0), "shader screen: no settings save at all: the game's answer (its own first launch)");
    DeleteFileW(record.c_str());
    SetPsoPrecompileScreenHold(false);
    ShaderScreenTestSetup(nullptr, nullptr, nullptr);
}

}  // namespace shs
