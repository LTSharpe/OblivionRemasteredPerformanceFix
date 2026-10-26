// FrameRateLimitAtStart (worker fpscap, 2026-10-22, public since 1.2.0, default 1; fixes-explained ## 65): the player's saved
// Frame Rate Limit is not in effect after a launch with frame generation on (a game bug, not a speed-up; the game's own
// code also without this DLL).
//
// Build 19115871 (all addresses exe+RVA):
//   - At start-up the game applies the saved options (Save_Settings.sav, UAltarGameUserSetting::SavedSettings, a TMap
//     FString -> FString of console commands) with ApplyAltarCVARSettings(true) (exe+0x47585E0): every entry is run as
//     "<cvar> <value>" through Altar's command setter (exe+0x48F7F10), e.g. Altar.GraphicsOptions.FrameRateLimit 60 and
//     Altar.FSR3.FI.Enabled 1.
//   - Console-variable sinks see frame generation's state change and call Altar's frame-generation switch T (exe+0x495F610,
//     void (void* self, bool on); it does nothing when its state byte already equals "on"): DLSS's (exe+0x495F160 /
//     0x495EF90: Altar.DLSS.Enabled * Altar.DLSS.FG.Enabled, cached state -1 at first, so it calls T(off) once in the
//     first frames on any PC) and FSR's (exe+0x495F380: Altar.FSR3.Enabled && Altar.FSR3.FI.Enabled). With the saved
//     options applied, FSR's sink calls T(on) a few seconds after the launch (live: DLL log 20:48:42 T(off), cap 0 by the
//     next probe sample; the start-up videos are still running). On "on", T stores the current values of Altar.GraphicsOptions.VSync,
//     Altar.GraphicsOptions.FrameRateLimit (and Altar.Reflex.Mode where Reflex exists) and sets them to "0" through the same
//     setter; on "off" it puts the stored values back. The saved options are not changed.
//   - The Frame Rate Limit's sink (exe+0x49586F0) turns the cvar into the engine's cap: UGameUserSettings::SetFrameRateLimit
//     (exe+0x31C03C0) and ApplyNonResolutionSettings (vtable +0x2C0) -> GEngine->SetMaxFPS -> t.MaxFPS.
//   So every launch with frame generation on starts uncapped (t.MaxFPS 0) although the saved option and the settings page
//   say 60. The Graphics settings page, when it is built (every time it is shown), runs each row's saved value again
//   (VModernSettingWidget::ApplyNewValue with bIsInit, exe+0x48DB1F0 -> the same setter, force off): the Frame Rate Limit
//   row is not locked while frame generation is on (V-Sync's is), shows 60 and sets the cvar to 60, and t.MaxFPS becomes 60.
//   Measured live (2026-10-22, runs fpscap-own1 / fpscap-stock1, with and without this DLL): 0 after the launch, in the main
//   menu and in the world; 60 as soon as the Graphics page is shown; FG off keeps 60 (T restores it); FG on again: 0.
//
// The fix does, once, what showing the Graphics page does for the Frame Rate Limit row: right after T's first switch from
// off to on in the process (with frame generation saved on: the start-up apply), if no menu is open (the player's own
// switch in the settings comes with the menu's named freeze; it is left to the game), the saved value of
// Altar.GraphicsOptions.FrameRateLimit is read from SavedSettings (the page's source; the map's own lookup functions,
// nothing added) and run through Altar's setter (same function, same name string, force off: nothing happens when the cvar
// already has that value). The game's own sink then sets the cap. T's stored value (used when frame generation is switched
// off later) is not touched: it is the same saved value. Every later switch of frame generation is left to the game.
// Found by byte pattern: T (its first call's state byte, the Frame Rate Limit name's lazy initialisation with the string
// L"Altar.GraphicsOptions.FrameRateLimit", the setter calls), the Frame Rate Limit sink (the settings getter, the cvar's
// int value) and GetOrAddValueFromSavedSettings (exe+0x4765650: the map's offset, hash and find functions). T gets a
// 5-byte jump at its entry (one atomic 8-byte write); a trampoline runs its first three instructions (7 bytes).
// Installed only while T has not switched frame generation on yet (its state byte is 0). On the version.dll route that is
// DllMain; the OBSE64 route runs our start-up from OBSE64's hook of the CRT's _get_narrow_winmain_command_line, after the
// static initialisers and before WinMain, so before the engine and T's first call too: the same install (T's entry is
// 8-byte aligned, so the jump goes in with the one atomic write; an unaligned entry leaves the fix off there).
// Since 1.2.2 (worker framecap2, 2026-10-26; findings "2026-10-26 (worker framecap2)"): only on the path that was measured.
// Players with NVIDIA DLSS frame generation reported a much lower frame rate with the key on (120 cap -> ~29 fps after a while
// in the world; 90 -> 50-60). From the exe: the DLSS sinks are per-frame console-variable sinks like FSR's, and their only
// condition is that the mode is a valid UStreamlineDLSSGMode value, so T(on) comes at start-up with DLSS frame generation too
// and the stock game leaves the cap at 0 there as well. But T(on) also sets Altar.Reflex.Mode to 2 (On + Boost) when its
// Reflex flag (exe+0x930A86C: hardware GPU scheduling on, an NVIDIA GPU, Reflex available; always 0 on AMD) is set, and with
// Reflex on the engine's limit (t.MaxFPS) is no longer a sleep in the engine: Streamline's MaxTickRateHandler
// (t.Streamline.Reflex.HandleMaxTickRate,
// default 1, exe+0x44C2C00) hands it to Reflex as frameLimitUs, and only on frames that were faster than the cap. How that
// limiter behaves together with DLSS frame generation is NVIDIA's code and was never measured here (AMD PC). So the fix now
// acts only when the switch came from FSR's sink (T's return address is the call in exe+0x495F380) and T's Reflex flag is 0
// after the switch (the cap is then the engine's own sleep, as measured), and only if the limit is still 0 at that moment.
// Everything else (DLSS frame generation, any GPU with Reflex) is left to the game, with a log line.
// Since 1.2.2 also (worker rel122fix, 2026-10-26, from review rel122review): only when the GPU is not NVIDIA's. The Reflex
// flag means "T switches Reflex on" (hardware GPU scheduling on && NVIDIA && the feature), not "Reflex is off": an NVIDIA PC
// with hardware GPU scheduling off and Reflex on from an old saved value or Engine.ini passes it. We cannot test NVIDIA, so on
// NVIDIA the key now leaves everything to the game (the stock game). The vendor is read through the same check T makes for its
// flag (its lazy initialisation block, exe+0x495FC0C, calls exe+0x265B230: cmp GRHIVendorId,0x10DE / sete al / ret).
// On AMD and Intel nothing changes.
// Lab builds: FrameRateLimitAtStart=2 only logs what it would do.
#include "common.h"

#include <cstring>
#include <intrin.h>

namespace opf {
namespace {

// T, exe+0x495F610: push rbp / push rbx / push rdi / mov rbp,rsp / sub rsp,50h / gs:[58h] / movzx edi,dl / TLS guard checks
// of its six function-static FStrings (VSync, FrameRateLimit, Reflex names; three stored values) / cmp byte [state],dil.
//   +0x3F  0F 8F <rel32>      jg -> lazy init of the FrameRateLimit name: +0x19 lea rdx,[L"Altar.GraphicsOptions.FrameRateLimit"],
//                             +0x20 lea rcx,[name FString]
//   +0x53  40 38 3D <disp32>  cmp byte [frame generation state], dil
//   +0x8C  4C 8D 3D <disp32>  lea r15,[stored FrameRateLimit value]
//   +0x12C 48 8D 15 <disp32>  lea rdx,[name FString]  (the stash of the FrameRateLimit value)
//   +0x1CB, +0x23F  E8 <rel32> call setter (VSync, FrameRateLimit)
constexpr const char* kSwitch =
    "40 55 53 57 48 8B EC 48 83 EC 50 65 48 8B 04 25 58 00 00 00 0F B6 FA 8B 0D ?? ?? ?? ?? BA ?? ?? 00 00 48 8B 1C C8 "
    "48 03 DA 8B 03 39 05 ?? ?? ?? ?? 0F 8F ?? ?? ?? ?? 8B 03 39 05 ?? ?? ?? ?? 0F 8F ?? ?? ?? ?? 8B 03 39 05 ?? ?? ?? ?? "
    "0F 8F ?? ?? ?? ?? 40 38 3D ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 8B 03 39 05 ?? ?? ?? ?? 4C 89 64 24 78 4C 89 B4 24 80 00 00 00 "
    "4C 8D 35 ?? ?? ?? ?? 4C 89 BC 24 88 00 00 00 0F 8F ?? ?? ?? ?? 8B 03 4C 8D 3D ?? ?? ?? ?? 39 05";
constexpr size_t kInitJg = 0x3F, kStateCmp = 0x53, kStashLea = 0x8C, kNameLea = 0x12C, kSetCallA = 0x1CB, kSetCallB = 0x23F;
constexpr size_t kInitStrLea = 0x19, kInitNameLea = 0x20;
constexpr uint8_t kFirstInsns[7] = {0x40, 0x55, 0x53, 0x57, 0x48, 0x8B, 0xEC};

// The Frame Rate Limit sink, exe+0x49586F0: call getter / null and RF_BeginDestroyed|RF_FinishDestroyed checks / mov rcx,[int*]
// / cmp with its cached value / GEngine flag / call SetFrameRateLimit / jmp [vtable+ApplyNonResolutionSettings].
// T's Reflex flag (function static, set on its first state change: hardware GPU scheduling on (D3DKMT WDDM 2.7 caps),
// GRHIVendorId 0x10DE, a MaxTickRateHandler / latency-marker feature available): two reads of it.
//   +0x26E 40 38 35 <disp32>     cmp byte [flag], sil   (the "on" branch)
//   +0x3C6 80 3D <disp32> 00     cmp byte [flag], 0     (the "off" branch)
constexpr size_t kReflexCmpA = 0x26E, kReflexCmpB = 0x3C6;

// T's lazy initialisation of the Reflex flag, exe+0x495FC0C (reached from T's TLS checks): lea rcx,[guard] / call
// _Init_thread_header / cmp [guard],-1 / jnz back into T / call the HAGS check / call the vendor check / call the feature
// check / al = all three / jmp (twice) to "lea rcx,[guard]; mov [flag],al; call _Init_thread_footer" in T.
constexpr const char* kReflexInit =
    "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? FF 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 19 E8 ?? ?? ?? ?? 84 C0 74 10 "
    "E8 ?? ?? ?? ?? 84 C0 74 07 B0 01 E9 ?? ?? ?? ?? 32 C0 E9";
constexpr size_t kInitBack = 0x13, kInitVendorCall = 0x22, kInitStoreJmpA = 0x36, kInitStoreJmpB = 0x3D;
// The vendor check, exe+0x265B230: cmp dword [GRHIVendorId], 0x10DE / sete al / ret.
constexpr uint8_t kVendorFn[14] = {0x81, 0x3D, 0, 0, 0, 0, 0xDE, 0x10, 0x00, 0x00, 0x0F, 0x94, 0xC0, 0xC3};
constexpr uint32_t kNvidia = 0x10DE;

// FSR frame generation's sink, exe+0x495F380: Altar.FSR3.Enabled && Altar.FSR3.FI.Enabled / cached state / r.FidelityFX.FI
// set / call T(self, on) at +0x93.
constexpr const char* kFsrSink =
    "48 89 74 24 10 57 48 83 EC 20 48 8B F1 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 88 00 00 00 84 C0 74 19 48 8B 0D ?? ?? ?? ?? "
    "48 8B 01 FF 90 88 00 00 00 84 C0 74 05 40 B7 01 EB 03 40 32 FF 40 38 3D ?? ?? ?? ?? 74 ?? 48 89 5C 24 30 48 8B 1D ?? ?? ?? ?? "
    "48 8B CB 40 88 3D ?? ?? ?? ?? 48 8B 03 FF 50 18 4C 8B 0B 48 8D 0D ?? ?? ?? ?? 25 00 00 00 FF 48 8D 15 ?? ?? ?? ?? 40 84 FF "
    "44 8B C0 48 0F 45 D1 48 8B CB 41 FF 91 80 00 00 00 40 0F B6 D7 48 8B CE E8";
constexpr size_t kFsrCallT = 0x93;

constexpr const char* kSink =
    "40 53 48 83 EC 20 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 8B 48 08 F7 C1 00 00 00 60 75 ?? 48 8B 0D ?? ?? ?? ?? 44 8B 01 "
    "44 3B 05 ?? ?? ?? ?? 74 ?? 48 8B 15 ?? ?? ?? ?? 33 C9 B8 10 00 00 00 66 41 0F 6E C8 45 85 C0 44 89 05 ?? ?? ?? ?? "
    "0F 5B C9 0F 44 C8 8B 82 ?? ?? ?? ?? 83 E0 EF 0B C8 89 8A ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 03 B2 01 48 8B CB "
    "48 83 C4 20 5B 48 FF A0 ?? ?? ?? ??";
constexpr size_t kSinkGetter = 6, kSinkValue = 0x1E;

// GetOrAddValueFromSavedSettings, exe+0x4765650: lea rdi,[rcx+SavedSettings] / hash(len, chars) / find(map, hash, key) /
// element = data + index * 0x28, value at +0x10 / not found: FString of the default value, add.
constexpr const char* kLookup =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 8B 42 08 48 8D B9 ?? ?? ?? ?? 48 8D 35 ?? ?? ?? ?? 48 8B DA "
    "85 C0 74 05 48 8B 12 EB 03 48 8B D6 33 ED 8D 48 FF 85 C0 0F 44 CD E8 ?? ?? ?? ?? 4C 8B C3 8B D0 48 8B CF E8 ?? ?? ?? ?? "
    "83 F8 FF 74 ?? 48 98 48 8D 0C 80 48 8B 07 48 8D 14 C8 48 85 D2 48 8D 42 10 48 0F 44 C5 48 85 C0 75 ?? "
    "48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 30 E8";  // (two other GetOrAdd functions of the same shape go on differently here)
constexpr size_t kLookupMap = 0x1A, kLookupHash = 0x3E, kLookupFind = 0x4B;
constexpr size_t kElementSize = 0x28, kValueOffset = 0x10;

struct FStr {  // FString: TArray<TCHAR> (data, num incl. the terminator, max)
    const wchar_t* data;
    int32_t num, max;
};
struct CmdPair {  // what the setter reads: the cvar name and the value
    FStr name, value;
};
using SwitchFn = void (*)(void* self, bool on);
using GetterFn = void* (*)();
using HashFn = uint32_t (*)(uint32_t len, const wchar_t* chars);
using FindFn = int32_t (*)(void* map, uint32_t hash, const FStr* key);
using SetFn = bool (*)(const CmdPair* pair, bool force);

uint8_t* g_switch = nullptr;
SwitchFn g_orig = nullptr;
GetterFn g_getter = nullptr;
HashFn g_hash = nullptr;
FindFn g_find = nullptr;
SetFn g_set = nullptr;
const FStr* g_name = nullptr;        // T's function-static FString "Altar.GraphicsOptions.FrameRateLimit" (filled by T)
const FStr* g_stash = nullptr;       // T's stored FrameRateLimit value
const uint8_t* g_state = nullptr;    // T's frame generation state byte
const uint8_t* g_reflex = nullptr;   // T's Reflex flag (1: T(on) also switched Reflex on)
const uint8_t* g_fsrRet = nullptr;   // the return address of FSR's sink's call of T
const uint32_t* g_vendor = nullptr;  // GRHIVendorId (the variable T's vendor check reads)
int32_t* const* g_valueSlot = nullptr;  // the cvar's int value (IConsoleVariable::GetIntRef), for the log
uint32_t g_mapOffset = 0;
bool g_freeze = false;
int g_mode = 0;                      // 0 off, 1 on (lab: 2 log only)
bool g_seen = false;                 // T has been called (game thread)
int g_testMenu = -1;                 // self-test: -1 = read the freeze subsystem, 0/1 = no menu / a menu
int g_testCaller = -1;               // self-test: -1 = T's real caller, 0/1 = another sink / FSR's sink
wchar_t g_value[64];

void Narrow(const FStr* s, char* out, size_t size)
{
    size_t i = 0;
    if (s && s->data && s->num > 0)
        for (; i + 1 < size && i + 1 < static_cast<size_t>(s->num) && s->data[i]; ++i) out[i] = static_cast<char>(s->data[i] & 0x7F);
    out[i] = 0;
}

int32_t CvarValue()
{
    __try {
        return g_valueSlot && *g_valueSlot ? **g_valueSlot : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

uint32_t VendorId()
{
    __try {
        return g_vendor ? *g_vendor : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool MenuOpen()
{
    if (g_testMenu >= 0) return g_testMenu != 0;
    int32_t named = 0, mode = 0;
    return g_freeze && ReadMenuFreeze(&named, &mode) && named != 0;
}

// The saved value of the name from SavedSettings (no entry added); false if there is none or it does not fit.
bool SavedValue(void* settings, const FStr* name, FStr* out)
{
    __try {
        if (!settings || (*reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(settings) + 8) & 0x60000000) != 0) return false;
        uint8_t* const map = static_cast<uint8_t*>(settings) + g_mapOffset;
        const uint32_t hash = g_hash(name->num ? static_cast<uint32_t>(name->num - 1) : 0u, name->num ? name->data : L"");
        const int32_t index = g_find(map, hash, name);
        if (index < 0) return false;
        const uint8_t* const elements = *reinterpret_cast<uint8_t* const*>(map);
        const FStr* const v = reinterpret_cast<const FStr*>(elements + static_cast<size_t>(index) * kElementSize + kValueOffset);
        if (!v->data || v->num < 2 || v->num > static_cast<int32_t>(sizeof g_value / sizeof g_value[0])) return false;
        memcpy(g_value, v->data, static_cast<size_t>(v->num) * sizeof(wchar_t));
        g_value[v->num - 1] = 0;
        out->data = g_value;
        out->num = out->max = v->num;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SwitchHook(void* self, bool on)
{
    // reached by jumps from T's entry: the return address is the one T's caller pushed
    const bool fromFsr = g_testCaller >= 0 ? g_testCaller == 1 : _ReturnAddress() == g_fsrRet;
    const bool wasOn = *g_state != 0;
    g_orig(self, on);
    // only frame generation's first switch from off to on (T itself does nothing when the state does not change): with the
    // saved options that is the start-up apply; every later switch is left to the game
    if (g_seen || !on || wasOn) return;
    g_seen = true;
    if (!fromFsr) {
        Log("[fpscap] frame generation's first switch on did not come from FSR frame generation's sink (DLSS frame generation's sinks "
            "or another caller; Frame Rate Limit now %d): left to the game (only FSR frame generation on AMD and Intel GPUs is verified)",
            CvarValue());
        return;
    }
    const uint32_t vendor = VendorId();
    if (vendor == kNvidia || vendor == 0) {
        Log("[fpscap] frame generation's first switch on with GPU vendor 0x%04X (%s; Frame Rate Limit now %d): left to the game "
            "(only FSR frame generation on AMD and Intel GPUs is verified)",
            vendor, vendor ? "NVIDIA" : "not known", CvarValue());
        return;
    }
    if (g_reflex && *g_reflex) {
        Log("[fpscap] frame generation's first switch on also switched NVIDIA Reflex on (Frame Rate Limit now %d): left to the game "
            "(with Reflex the limit goes to Reflex's own limiter, not verified)",
            CvarValue());
        return;
    }
    if (MenuOpen()) {
        Log("[fpscap] frame generation's first switch came with a menu open: left to the game");
        return;
    }
    if (CvarValue() > 0) {
        Log("[fpscap] frame generation on at start-up left the Frame Rate Limit at %d: nothing to do", CvarValue());
        return;
    }
    char stash[64], saved[64];
    Narrow(g_stash, stash, sizeof stash);
    FStr value{};
    if (!g_name || !g_name->data || g_name->num < 2 || !SavedValue(g_getter(), g_name, &value)) {
        Log("[fpscap] frame generation on at start-up (Frame Rate Limit now %d, was \"%s\"); no saved Frame Rate Limit found: left to the game",
            CvarValue(), stash);
        return;
    }
    Narrow(&value, saved, sizeof saved);
    const int32_t before = CvarValue();
    const CmdPair pair{*g_name, value};
    g_set(&pair, false);
    Log("[fpscap] frame generation on at start-up set the Frame Rate Limit to %d (stored \"%s\"); the saved \"%s\" applied again as the "
        "Graphics page does: now %d%s",
        before, stash, saved, CvarValue(), strcmp(stash, saved) != 0 ? " (stored and saved values differ)" : "");
}

// jmp [rip+0]; dq target
void AbsJump(uint8_t* at, const void* target)
{
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, jmp, sizeof jmp);
    memcpy(at + sizeof jmp, &target, sizeof target);
}

bool PatchEntry(uint8_t* fn, uint8_t* stub, bool beforeGameCode)
{
    const int64_t rel = stub - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t insn[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(insn + 1, &rel32, sizeof rel32);
    if ((reinterpret_cast<uintptr_t>(fn) & 7) == 0) {
        auto* q = reinterpret_cast<volatile LONG64*>(fn);
        DWORD old;
        if (!VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        const LONG64 before = *q;
        LONG64 after = before;
        memcpy(&after, insn, sizeof insn);
        const bool ok = InterlockedCompareExchange64(q, after, before) == before;
        VirtualProtect(fn, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, 8);
        return ok;
    }
    return beforeGameCode && WriteCode(fn, insn);
}

}  // namespace

// Locates the three functions and checks them against each other (no write). Logs and returns false on any mismatch.
bool FindFrameCapStart()
{
    if (g_switch) return true;
    const GameModule& game = Game();
    const auto sw = FindAll(game.text, ParsePattern(kSwitch));
    const auto sink = FindAll(game.text, ParsePattern(kSink));
    const auto look = FindAll(game.text, ParsePattern(kLookup));
    const auto fsr = FindAll(game.text, ParsePattern(kFsrSink));
    const auto rinit = FindAll(game.text, ParsePattern(kReflexInit));
    if (sw.size() != 1 || sink.size() != 1 || look.size() != 1 || fsr.size() != 1 || rinit.size() != 1) {
        Log("[fpscap] frame generation switch / Frame Rate Limit sink / saved-settings lookup / FSR frame generation sink / Reflex "
            "flag's initialisation not found (%zu / %zu / %zu / %zu / %zu matches): fix not installed",
            sw.size(), sink.size(), look.size(), fsr.size(), rinit.size());
        return false;
    }
    uint8_t* const t = sw[0];
    const auto inData = [&](const void* p, uintptr_t align) {
        return (game.data.Contains(p) || game.rdata.Contains(p)) && (reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0;
    };
    uint8_t* const init = RipTarget(t + kInitJg, 2, 6);
    const bool initOk = game.text.Contains(init) && init[kInitStrLea] == 0x48 && init[kInitStrLea + 1] == 0x8D &&
                        init[kInitStrLea + 2] == 0x15 && init[kInitNameLea] == 0x48 && init[kInitNameLea + 1] == 0x8D &&
                        init[kInitNameLea + 2] == 0x0D;
    const auto* const str = initOk ? reinterpret_cast<const wchar_t*>(RipTarget(init + kInitStrLea, 3, 7)) : nullptr;
    const auto* const name = reinterpret_cast<const FStr*>(RipTarget(t + kNameLea, 3, 7));
    const bool nameAgrees = initOk && RipTarget(init + kInitNameLea, 3, 7) == reinterpret_cast<const uint8_t*>(name);
    const auto* const stash = reinterpret_cast<const FStr*>(RipTarget(t + kStashLea, 3, 7));
    const uint8_t* const state = RipTarget(t + kStateCmp, 3, 7);
    const bool reflexOk = t[kReflexCmpA] == 0x40 && t[kReflexCmpA + 1] == 0x38 && t[kReflexCmpA + 2] == 0x35 && t[kReflexCmpB] == 0x80 &&
                          t[kReflexCmpB + 1] == 0x3D && t[kReflexCmpB + 6] == 0x00 &&
                          RipTarget(t + kReflexCmpA, 3, 7) == RipTarget(t + kReflexCmpB, 2, 7);
    const uint8_t* const reflex = reflexOk ? RipTarget(t + kReflexCmpB, 2, 7) : nullptr;
    const bool fsrOk = RipTarget(fsr[0] + kFsrCallT, 1, 5) == t;
    // the Reflex flag's initialisation: belongs to T (jumps back into it), stores into the flag T reads, calls the vendor check
    const uint8_t* const ri = rinit[0];
    const uint8_t* const back = RipTarget(ri + kInitBack, 2, 6);
    const uint8_t* const store = RipTarget(ri + kInitStoreJmpA, 1, 5);
    const uint8_t* const vfn = RipTarget(ri + kInitVendorCall, 1, 5);
    const bool vfnOk = game.text.Contains(vfn) && memcmp(vfn, kVendorFn, 2) == 0 &&
                       memcmp(vfn + 6, kVendorFn + 6, sizeof kVendorFn - 6) == 0;
    const auto* const vendor = vfnOk ? reinterpret_cast<const uint32_t*>(RipTarget(vfn, 2, 10)) : nullptr;
    const bool initOwn = back > t && back < t + 0x500 && store > t && store < t + 0x500 &&
                         RipTarget(ri + kInitStoreJmpB, 1, 5) == store && store[0] == 0x48 && store[1] == 0x8D && store[2] == 0x0D &&
                         RipTarget(store, 3, 7) == RipTarget(ri, 3, 7) && store[7] == 0x88 && store[8] == 0x05 && reflex &&
                         RipTarget(store + 7, 2, 6) == reflex;
    const bool vendorOk = initOwn && vendor && inData(vendor, 4);
    uint8_t* const setA = t[kSetCallA] == 0xE8 ? RipTarget(t + kSetCallA, 1, 5) : nullptr;
    uint8_t* const setB = t[kSetCallB] == 0xE8 ? RipTarget(t + kSetCallB, 1, 5) : nullptr;
    const bool strOk = str && inData(str, 2) && wcscmp(str, L"Altar.GraphicsOptions.FrameRateLimit") == 0;
    uint32_t mapOffset = 0;
    memcpy(&mapOffset, look[0] + kLookupMap, 4);
    uint8_t* const getter = RipTarget(sink[0] + kSinkGetter, 1, 5);
    auto* const valueSlot = reinterpret_cast<int32_t* const*>(RipTarget(sink[0] + kSinkValue, 3, 7));
    uint8_t* const hash = RipTarget(look[0] + kLookupHash, 1, 5);
    uint8_t* const find = RipTarget(look[0] + kLookupFind, 1, 5);
    if (!strOk || !nameAgrees || !inData(name, 8) || !inData(stash, 8) || !inData(state, 1) || !setA || setA != setB ||
        !game.text.Contains(setA) || !game.text.Contains(getter) || !game.text.Contains(hash) || !game.text.Contains(find) ||
        !inData(valueSlot, 8) || mapOffset == 0 || mapOffset > 0x1000 || (mapOffset & 7) || memcmp(t, kFirstInsns, sizeof kFirstInsns) != 0 ||
        !reflex || !inData(reflex, 1) || !fsrOk || !vendorOk) {
        Log("[fpscap] unexpected code (name string %s, name %s, setter calls %s, map +0x%X, Reflex flag %s, FSR sink %s, Reflex flag's "
            "initialisation %s, vendor check %s): fix not installed",
            strOk ? "ok" : "differs", nameAgrees ? "agrees" : "differs", setA && setA == setB ? "agree" : "differ", mapOffset,
            reflex ? "ok" : "differs", fsrOk ? "calls the switch" : "differs", initOwn ? "ok" : "differs", vfnOk ? "ok" : "differs");
        return false;
    }
    g_name = name;
    g_stash = stash;
    g_state = state;
    g_reflex = reflex;
    g_vendor = vendor;
    g_fsrRet = fsr[0] + kFsrCallT + 5;
    g_set = reinterpret_cast<SetFn>(setA);
    g_getter = reinterpret_cast<GetterFn>(getter);
    g_hash = reinterpret_cast<HashFn>(hash);
    g_find = reinterpret_cast<FindFn>(find);
    g_valueSlot = valueSlot;
    g_mapOffset = mapOffset;
    g_switch = t;
    return true;
}

bool InstallFrameCapStart(bool beforeGameCode, int mode)
{
    if (!FindFrameCapStart()) return false;
    if (g_orig) return true;
    if (*g_state != 0) {
        Log("[fpscap] the game has already switched frame generation on%s: fix not installed",
            beforeGameCode ? "" : " (OBSE64 route)");
        return false;
    }
    uint8_t* const region = AllocNearGame(0x40);
    if (!region) {
        Log("[fpscap] no free memory near the game's code: fix not installed");
        return false;
    }
    AbsJump(region, reinterpret_cast<const void*>(&SwitchHook));
    memcpy(region + 0x20, kFirstInsns, sizeof kFirstInsns);
    AbsJump(region + 0x20 + sizeof kFirstInsns, g_switch + sizeof kFirstInsns);
    DWORD old;
    VirtualProtect(region, 0x40, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 0x40);
    g_orig = reinterpret_cast<SwitchFn>(region + 0x20);
    g_freeze = FindFreezeSubsystem();
    g_mode = mode;
    if (!PatchEntry(g_switch, region, beforeGameCode)) {
        g_orig = nullptr;
        Log("[fpscap] could not patch the frame generation switch at exe+0x%llX: fix not installed",
            static_cast<unsigned long long>(g_switch - Game().base));
        return false;
    }
    Log("[fpscap] frame generation switch exe+0x%llX detoured (setter exe+0x%llX, SavedSettings +0x%X%s%s): %s",
        static_cast<unsigned long long>(g_switch - Game().base), static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g_set) - Game().base),
        g_mapOffset, g_freeze ? "" : ", no freeze subsystem: menus not checked", beforeGameCode ? "" : ", OBSE64 route",
        mode == 1 ? "the saved Frame Rate Limit is applied after FSR frame generation's start-up switch on AMD and Intel GPUs"
                  : "log only");
    return true;
}

// Self-test: the GRHIVendorId variable found through T's vendor check (nullptr until FindFrameCapStart succeeded).
const uint32_t* FrameCapStartTestVendor() { return g_vendor; }

// Self-test: T's entry is the 5-byte jump to the hook's stub and the trampoline holds its first three instructions.
bool FrameCapStartTestPatched()
{
    if (!g_switch || !g_orig || g_switch[0] != 0xE9) return false;
    const uint8_t* const stub = RipTarget(g_switch, 1, 5);
    const uint8_t* const tramp = reinterpret_cast<const uint8_t*>(g_orig);
    return stub + 0x20 == tramp && stub[0] == 0xFF && stub[1] == 0x25 && memcmp(tramp, kFirstInsns, sizeof kFirstInsns) == 0 &&
           tramp[sizeof kFirstInsns] == 0xFF && memcmp(g_switch + 5, kFirstInsns + 5, 2) == 0;
}

bool FrameCapStartTestRun(void* fakeOrig, void* fakeGetter, void* fakeHash, void* fakeFind, void* fakeSet, const void* name,
                          const uint8_t* state, int menu, int mode, bool on, bool reset, int caller, const uint8_t* reflex,
                          int32_t* const* valueSlot, const uint32_t* vendor)
{
    if (reset) g_seen = false;
    g_state = state;
    g_reflex = reflex;
    g_vendor = vendor;
    g_testCaller = caller;
    g_orig = reinterpret_cast<SwitchFn>(fakeOrig);
    g_getter = reinterpret_cast<GetterFn>(fakeGetter);
    g_hash = reinterpret_cast<HashFn>(fakeHash);
    g_find = reinterpret_cast<FindFn>(fakeFind);
    g_set = reinterpret_cast<SetFn>(fakeSet);
    g_name = static_cast<const FStr*>(name);
    g_stash = nullptr;
    g_valueSlot = valueSlot;
    g_mapOffset = 0x1C8;
    g_testMenu = menu;
    g_mode = mode;
    SwitchHook(nullptr, on);
    return g_seen;
}

}  // namespace opf
