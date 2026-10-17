// FrameGenAsyncCompute: frame generation's GPU work runs on its own queue, beside the next frame.
//
// The game uses AMD's FSR 3 Unreal plugin with AMD's own D3D12 swap chain (amd_fidelityfx_dx12.dll, FSR 3.1.0). With the
// plugin's console variable r.FidelityFX.FI.AllowAsyncWorkloads at its default 0, which the game keeps, the generated
// frame is computed on the game's graphics queue, in series between one rendered frame and the next. AMD's swap chain
// also lets the game run only about one frame ahead of the GPU with frame generation on: each Present waits until the
// previous generated frame is finished on the GPU. In a town, where the CPU side and the GPU are both close to the
// frame time, the GPU then waits for the CPU part of every frame and the frame generation work comes on top: at 4K, FSR
// performance, ray tracing, Shadows High (RX 9070 XT, Ryzen 7 5800X), the rendered frame rate in three Cheydinhal views
// fell from 58 / 67 / 78 fps without frame generation to 46-47 / 48-49 / 57-60 with it (2026-10-13). With the variable at
// 1 the plugin hands the work to the swap chain's async compute queue, where it overlaps the next frame: 54.6 / 63.5 /
// 74.7 fps in the same views, the same frame pacing (present-to-present spread unchanged), the same images (the same
// passes; only the queue they run on differs).
//
// The variable is read-only at runtime (the console refuses it) and the plugin reads it when it creates its swap chain
// and frame generation context, so it has to be set before then. The fix finds where the game registers it (its name,
// default 0 and flags ECVF_ReadOnly, then the pointer to its value the registration stores) and, from a short-lived
// thread started while the game starts, writes 1 into the value as soon as the registration has run (the game's static
// initialisation, seconds before the renderer starts). Anything that sets it later wins: a line
// r.FidelityFX.FI.AllowAsyncWorkloads=0 under [SystemSettings] in the player's Engine.ini still turns it off. If AMD's
// library is already loaded when the value appears (the plugin started first, e.g. a late plugin load), the fix leaves
// it alone: changing it after the swap chain exists is not what the plugin expects.
//
// Found by shape (build 19115871: the registration at exe+0xAA0690): lea rdx,[name] after xor r8d,r8d and
// mov dword [rsp+20h],4, then the call to RegisterConsoleVariable, the auto-variable's constructor and AsVariableInt
// (vtable +0x58), whose result is stored at auto+0x10. Any mismatch: the fix stays off with a log line.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

constexpr wchar_t kName[] = L"r.FidelityFX.FI.AllowAsyncWorkloads";
// From lea rdx,[name]: call [rax+18h]; mov rdx,rax; lea rcx,[auto]; call ctor; mov rcx,[auto+8]; lea rax,[vtable];
// mov [auto],rax; mov rax,[rcx]; call [rax+58h]; lea rcx,[...]; mov [auto+10h],rax
constexpr char kRegister[] =
    "48 8D 15 ?? ?? ?? ?? FF 50 18 48 8B D0 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? "
    "48 89 05 ?? ?? ?? ?? 48 8B 01 FF 50 58 48 8D 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ??";
constexpr size_t kAutoLea = 13, kCvarLoad = 25, kDataStore = 59;
// Just before it: xor r8d,r8d (default 0); mov dword [rsp+20h],4 (ECVF_ReadOnly)
constexpr char kDefaultAndFlags[] = "45 33 C0 C7 44 24 20 04 00 00 00";
constexpr DWORD kPollMs = 1;
constexpr unsigned kMaxWaitMs = 120000;  // Sleep(1) lasts a timer tick (~1-16 ms): the variable is seen within one tick

int32_t* volatile* g_slot = nullptr;  // where the registration stores TConsoleVariableData<int32>* (two int32 values)
std::atomic<int> g_state{0};          // 0 not set yet, 1 set, 2 left alone

const uint8_t* FindName(const Section& s)
{
    const size_t bytes = sizeof(kName);
    const uint8_t* hit = nullptr;
    for (const uint8_t* p = s.begin; p + bytes <= s.end; p += 2) {
        if (memcmp(p, kName, bytes) != 0) continue;
        if (hit) return nullptr;
        hit = p;
    }
    return hit;
}

bool FrameGenLibraryLoaded() { return GetModuleHandleW(L"amd_fidelityfx_dx12.dll") != nullptr; }

DWORD WINAPI SetWhenRegistered(LPVOID)
{
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const auto waited = static_cast<unsigned>(GetTickCount64() - start);
        if (waited >= kMaxWaitMs) break;
        int32_t* const value = *g_slot;
        if (value) {
            if (FrameGenLibraryLoaded()) {
                g_state = 2;
                Log("[fgasync] r.FidelityFX.FI.AllowAsyncWorkloads registered after AMD's FidelityFX library was loaded: "
                    "left as it is (%d)", value[0]);
                return 0;
            }
            // TConsoleVariableData<int32>: the game thread's value and the render thread's copy
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&value[0]), 1);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&value[1]), 1);
            g_state = 1;
            Log("[fgasync] r.FidelityFX.FI.AllowAsyncWorkloads set to 1 at its registration (%u ms after this DLL started): "
                "frame generation's GPU work runs on its own queue", waited);
            return 0;
        }
        Sleep(kPollMs);
    }
    Log("[fgasync] r.FidelityFX.FI.AllowAsyncWorkloads was not registered within %u s: not changed", kMaxWaitMs / 1000);
    return 0;
}

}  // namespace

bool FindFrameGenAsyncCompute()
{
    if (g_slot) return true;
    const GameModule& game = Game();
    const uint8_t* const name = FindName(game.rdata);
    if (!name) {
        Log("[fgasync] the name r.FidelityFX.FI.AllowAsyncWorkloads not found (once) in the game: FrameGenAsyncCompute unavailable");
        return false;
    }
    const Pattern reg = ParsePattern(kRegister), pre = ParsePattern(kDefaultAndFlags);
    uint8_t* site = nullptr;
    int n = 0;
    for (uint8_t* p : FindAll(game.text, reg)) {
        if (RipTarget(p, 3, 7) != name) continue;
        site = p;
        ++n;
    }
    if (n != 1) {
        Log("[fgasync] its registration found %d times (expected once): FrameGenAsyncCompute unavailable", n);
        return false;
    }
    if (!MatchesAt(site - pre.size(), pre)) {
        Log("[fgasync] its registration does not have default 0 and the read-only flag: FrameGenAsyncCompute unavailable");
        return false;
    }
    uint8_t* const autoVar = RipTarget(site + kAutoLea, 3, 7);
    uint8_t* const cvarSlot = RipTarget(site + kCvarLoad, 3, 7);
    uint8_t* const dataSlot = RipTarget(site + kDataStore, 3, 7);
    if (cvarSlot != autoVar + 8 || dataSlot != autoVar + 0x10 || !game.data.Contains(dataSlot) ||
        (reinterpret_cast<uintptr_t>(dataSlot) & 7) != 0) {
        Log("[fgasync] its value pointer is not where expected: FrameGenAsyncCompute unavailable");
        return false;
    }
    g_slot = reinterpret_cast<int32_t* volatile*>(dataSlot);
    Log("[fgasync] r.FidelityFX.FI.AllowAsyncWorkloads registered at exe+0x%X, value pointer at exe+0x%X",
        static_cast<unsigned>(site - game.base), static_cast<unsigned>(dataSlot - game.base));
    return true;
}

bool ApplyFrameGenAsyncCompute(bool beforeGameCode)
{
    if (!FindFrameGenAsyncCompute()) return false;
    if (int32_t* const value = *g_slot) {
        // registered already (a late load): only before AMD's library is there
        if (!beforeGameCode || FrameGenLibraryLoaded()) {
            g_state = 2;
            Log("[fgasync] loaded after the game's start-up: r.FidelityFX.FI.AllowAsyncWorkloads left as it is (%d)", value[0]);
            return false;
        }
    }
    if (HANDLE t = CreateThread(nullptr, 0, SetWhenRegistered, nullptr, 0, nullptr)) {
        CloseHandle(t);
        return true;
    }
    Log("[fgasync] no thread: FrameGenAsyncCompute not applied");
    return false;
}

void LogFrameGenAsyncComputeState()
{
    if (!g_slot || g_state.load() != 1) return;
    static bool reported = false;
    if (reported) return;
    if (int32_t* const value = *g_slot) {
        reported = true;
        if (value[0] != 1) Log("[fgasync] r.FidelityFX.FI.AllowAsyncWorkloads is %d now: set by the game's or the player's config after the fix", value[0]);
    }
}

int32_t* FrameGenAsyncComputeValueForTest()
{
    return g_slot ? *g_slot : nullptr;
}

}  // namespace opf
