// ShaderScreenAfterDriverChange (game bug, on by default): the game's own "Shaders preloading" screen comes
// back at start-up after the graphics driver or card changed.
//
// The problem. A new graphics driver throws away the driver's compiled shaders. On the very first launch the game waits
// for its pipeline precompile behind a start-up screen ("Shaders preloading: 0-100 %", "Compiling shaders to optimize
// performance during gameplay."), but it decides that only once: at start-up (exe+0x44E1290 in build 19115871) it sets
// the start-up movie player's first-launch flag (+0x89) to "the Save_Settings save does not exist"
// (UGameplayStatics::DoesSaveGameExist, called at exe+0x44E12C4), and the movie player's set-up (exe+0x44E8350) adds the
// ShaderLoadingScreenMovie and its precompile callbacks only while that flag is set. After the first launch the settings
// save exists, so after a driver update the precompile runs in the background into play instead (with a cold driver
// cache ~3 minutes on the test PC, ~35 hitches a minute in the first flight: notes/p7b-shader-cache.md). Players rename
// Save_Settings.sav to get the screen back, which also resets every setting.
//
// The fix. That one call goes through a wrapper. It calls the game's DoesSaveGameExist, then compares the graphics card
// the game runs on (DXGI: vendor, device, the user-mode driver version; the hardware adapter with the most dedicated
// video memory, as in memory_monitor.cpp) with the one recorded in OblivionPerfFix-gpu.txt next to the DLL. If the record
// exists and differs, the wrapper answers "no settings save" to this one caller only, so the game takes its own
// first-launch path: the shader screen, with the game's own pace and timeout. Settings are loaded as always (the game's
// other DoesSaveGameExist callers are untouched). The new card and driver are recorded once the precompile has finished
// (PsoPrecompileFast sees its end; a start-up closed behind the screen shows it again next time). No record yet (the first
// launch with the switch on): it is written and nothing else happens. Delete the file to record again.
//
// What changes. After a driver or card change, start-up waits on the game's shader screen until its precompile is done
// (minutes with a cold driver cache), as on the game's first launch, instead of compiling during play. Nothing else.
#include "common.h"

#include <dxgi1_4.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace opf {
namespace {

using ExistsFn = bool (*)(const void* slotName, int32_t userIndex);  // (const FString&, int32)

// lea rdx,[L"Save_Settings"]; lea rcx,[rsp+20h]; call FString(const TCHAR*); xor edx,edx; lea rcx,[rsp+20h];
// call DoesSaveGameExist; mov rcx,[rsp+20h]; test al,al; sete al; mov [rdi+89h],al
constexpr const char* kCallSite =
    "48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 20 E8 ?? ?? ?? ?? 33 D2 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B 4C 24 20 84 C0 0F 94 C0 "
    "88 87 89 00 00 00";
constexpr size_t kNameLea = 0, kExistsCall = 24;
// DoesSaveGameExist: mov [rsp+8],rbx; push rdi; sub rsp,20h; mov edi,edx; mov rbx,rcx; call IPlatformFeaturesModule::Get;
// mov rcx,rax; mov r8,[rax]; call [r8+40h] (GetSaveGameSystem); test rax,rax; je; cmp dword [rbx+8],0; mov r8,[rax];
// mov r9,[r8+10h] (DoesSaveGameExist)
constexpr const char* kExistsEntry =
    "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 8B C8 4C 8B 00 41 FF 50 40 48 85 C0 74 ?? 83 7B 08 00 "
    "4C 8B 00 4D 8B 48 10";
constexpr wchar_t kSlot[] = L"Save_Settings";

uint8_t* g_call = nullptr;
uint8_t* g_stub = nullptr;
uint8_t* g_target = nullptr;  // the game's DoesSaveGameExist
ExistsFn g_original = nullptr;
std::wstring g_recordPath;
bool (*g_testGpu)(char*, size_t) = nullptr;
int g_decision = -1;  // -1 not asked yet, 0 game's answer, 1 first-launch path taken
char g_pending[96] = "";  // the new card and driver, recorded once the precompile has finished (PsoPrecompileFast sees it)

// "vendor device subsys rev driver" of the hardware adapter with the most dedicated video memory.
bool CurrentGpu(char* out, size_t size)
{
    if (g_testGpu) return g_testGpu(out, size);
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return false;
    auto create = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(IID_PPV_ARGS(&factory))) || !factory) return false;
    bool found = false;
    SIZE_T best = 0;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        LARGE_INTEGER umd{};
        if (SUCCEEDED(a->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.DedicatedVideoMemory > best &&
            SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            best = desc.DedicatedVideoMemory;
            snprintf(out, size, "%04X %04X %08X %02X %u.%u.%u.%u", desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision,
                     static_cast<unsigned>(HIWORD(umd.HighPart)), static_cast<unsigned>(LOWORD(umd.HighPart)),
                     static_cast<unsigned>(HIWORD(umd.LowPart)), static_cast<unsigned>(LOWORD(umd.LowPart)));
            found = true;
        }
        a->Release();
    }
    factory->Release();
    return found;
}

bool ReadRecord(char* out, size_t size)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_recordPath.c_str(), L"rb") != 0 || !f) return false;
    const size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = 0;
    for (size_t i = n; i > 0 && (out[i - 1] == '\r' || out[i - 1] == '\n' || out[i - 1] == ' '); --i) out[i - 1] = 0;
    return out[0] != 0;
}

bool WriteRecord(const char* gpu)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_recordPath.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = fprintf(f, "%s\r\n", gpu) > 0;
    fclose(f);
    return ok;
}

bool ExistsWrapper(const void* slotName, int32_t userIndex)
{
    const bool exists = g_original(slotName, userIndex);
    if (g_decision != -1) return g_decision == 1 ? false : exists;
    g_decision = 0;
    char now[96] = "", before[96] = "";
    if (!exists) {
        Log("[shaderscreen] no settings save yet: the game's first launch shows its shader screen anyway");
        if (CurrentGpu(now, sizeof now)) WriteRecord(now);
        return exists;
    }
    if (!CurrentGpu(now, sizeof now)) {
        Log("[shaderscreen] the graphics card could not be read (DXGI): start-up as the game decides");
        return exists;
    }
    if (!ReadRecord(before, sizeof before)) {
        const bool ok = WriteRecord(now);
        Log("[shaderscreen] graphics card and driver recorded (%s)%s; the shader screen returns after the next change", now,
            ok ? "" : " - writing OblivionPerfFix-gpu.txt failed");
        return exists;
    }
    if (strcmp(before, now) == 0) {
        Log("[shaderscreen] graphics card and driver unchanged (%s): start-up as usual", now);
        return exists;
    }
    g_decision = 1;
    // Recorded once the precompile has finished, so a start-up closed behind the screen shows it again next time; without
    // PsoPrecompileFast (which sees the end) it is recorded now.
    PsoPrecompileInfoOut pso{};
    const bool deferred = PsoPrecompileInfo(&pso);
    const bool ok = deferred || WriteRecord(now);
    if (deferred) snprintf(g_pending, sizeof g_pending, "%s", now);
    // The game itself asks only for UE's Background pace behind that screen (8 pipelines per frame here; a warm precompile
    // still unfinished after 160 s, 2026-10-28): PsoPrecompileFast runs it at UE's Fast pace until the main menu.
    if (deferred) SetPsoPrecompileScreenHold(true);
    Log("[shaderscreen] graphics card or driver changed (%s -> %s): the game's shader screen is shown at this start-up%s", before,
        now, deferred ? "; recorded once the precompile has finished"
                      : ok ? "" : " (writing OblivionPerfFix-gpu.txt failed: it will show again next time)");
    return false;
}

}  // namespace

void ShaderScreenPrecompileFinished()
{
    if (!g_pending[0]) return;
    const bool ok = WriteRecord(g_pending);
    Log("[shaderscreen] the precompile has finished: %s recorded%s", g_pending, ok ? "" : " - writing OblivionPerfFix-gpu.txt failed");
    g_pending[0] = 0;
}

bool ApplyShaderScreenAfterDriverChange(bool beforeGameCode, const wchar_t* dir)
{
    if (g_call) return true;
    if (!beforeGameCode) {
        Log("[shaderscreen] can only be installed at start-up (version.dll), not installed");
        return false;
    }
    if (!dir) return false;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kCallSite));
    uint8_t* site = nullptr;
    int named = 0;
    for (uint8_t* p : hits) {
        const auto* const name = reinterpret_cast<const wchar_t*>(RipTarget(p + kNameLea, 3, 7));
        if (!game.rdata.Contains(name) || wcscmp(name, kSlot) != 0) continue;
        site = p;
        ++named;
    }
    if (named != 1) {
        Log("[shaderscreen] the start-up's Save_Settings check not found (%zu matches, %d with the name): "
            "ShaderScreenAfterDriverChange not installed", hits.size(), named);
        return false;
    }
    uint8_t* const call = site + kExistsCall;
    uint8_t* const target = RipTarget(call, 1, 5);
    if (call[0] != 0xE8 || !game.text.Contains(target) || !MatchesAt(target, ParsePattern(kExistsEntry))) {
        Log("[shaderscreen] the call at exe+0x%llX does not go to the expected DoesSaveGameExist: ShaderScreenAfterDriverChange not "
            "installed", static_cast<unsigned long long>(call - game.base));
        return false;
    }
    uint8_t* const region = AllocNearGame(0x1000);
    if (!region) {
        Log("[shaderscreen] no free memory near the game's code for the call stub: ShaderScreenAfterDriverChange not installed");
        return false;
    }
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&ExistsWrapper);
    memcpy(region, jmp, sizeof jmp);
    memcpy(region + sizeof jmp, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 14);
    const int64_t rel = region - (call + 5);
    if (rel != static_cast<int32_t>(rel)) {
        VirtualFree(region, 0, MEM_RELEASE);
        Log("[shaderscreen] call stub out of reach: ShaderScreenAfterDriverChange not installed");
        return false;
    }
    g_original = reinterpret_cast<ExistsFn>(target);
    g_target = target;
    g_recordPath = std::wstring(dir) + L"\\OblivionPerfFix-gpu.txt";
    const int32_t rel32 = static_cast<int32_t>(rel);
    if (!WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4))) {
        VirtualFree(region, 0, MEM_RELEASE);
        g_original = nullptr;
        Log("[shaderscreen] writing the call at exe+0x%llX failed: ShaderScreenAfterDriverChange not installed",
            static_cast<unsigned long long>(call - game.base));
        return false;
    }
    g_call = call;
    g_stub = region;
    Log("[shaderscreen] the start-up's Save_Settings check (DoesSaveGameExist exe+0x%llX, called at exe+0x%llX) goes through "
        "ShaderScreenAfterDriverChange",
        static_cast<unsigned long long>(target - game.base), static_cast<unsigned long long>(call - game.base));
    return true;
}

bool ShaderScreenInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook)
{
    if (!g_call) return false;
    *call = g_call;
    *original = reinterpret_cast<uint8_t*>(g_original);
    *stub = g_stub;
    *hook = reinterpret_cast<void*>(&ExistsWrapper);
    return true;
}

void ShaderScreenTestSetup(void* original, bool (*gpu)(char*, size_t), const wchar_t* recordPath)
{
    g_original = reinterpret_cast<ExistsFn>(original ? original : g_target);
    g_testGpu = gpu;
    if (recordPath) g_recordPath = recordPath;
    g_decision = -1;
    g_pending[0] = 0;
}

}  // namespace opf
