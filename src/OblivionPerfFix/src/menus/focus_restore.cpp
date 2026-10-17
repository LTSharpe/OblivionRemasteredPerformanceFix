// RestoreFsrAfterAltTab: FSR and frame generation stay off after an alt-tab (a game bug, not a speed-up).
//
// The game switches FSR 3 off whenever its window loses the focus and on again when it gets the focus back: Altar's
// handler (exe+0x495EE00 in build 19115871, bound to the application-activation event) writes r.FidelityFX.FSR3.Enabled
// only, 0 on loss and 1 on gain, keeping the variable's set-by priority; the FSR 3 plugin then puts back the render scale
// it had saved, and frame generation (the "AMD FSR Presenter/Interpolation" threads) stops and starts with it. On focus
// gain the handler does nothing while VAltarUISubsystem.bIsMenuVisible is set, and nothing switches FSR on later.
// Two ways to end up without FSR (both seen live, 2026-10-06/07, posted WM_ACTIVATEAPP in the hidden bench game):
//   - alt-tab while a menu is open: FSR stays off after the menu closed;
//   - alt-tab in plain play after any player menu (Tab: character, inventory, magic, map, quests; the wait menu) was
//     opened and closed since the last pause menu: the game's menu-visibility function (exe+0x4729660) clears
//     bIsMenuVisible only for the pause menu / main menu states; for these menus it runs its "menu hidden" steps
//     (input mode, freeze) but leaves the flag set. The handler then sees a menu that is not there. This is the case a
//     player reported on 2026-10-07 ("FSR still switched off on alt-tab").
// The player then plays on at native resolution with TAA and without frame generation until the upscaler option is
// applied again in the settings (or, with the stale flag, until the pause menu is closed once and the focus changes).
//
// The fix keeps the game's rule "FSR off while the window is in the background" and restores FSR once the player is
// back in the game: the window has the focus, no menu holds the world, FSR is still the chosen upscaler, and FSR is
// still off because of the focus loss.
//   - The focus handler is detoured: after it ran, a focus loss that switched FSR off (Altar.FSR3.Enabled on, FSR now
//     off) leaves a "pending" mark; a focus gain that switched FSR back on clears it.
//   - "No menu holds the world" = the freeze subsystem's named-freeze stack is empty (UVFreezeInMenuSubsystem, what
//     IsFreezing reads; every menu, dialogue and the main menu push a named freeze, and pop it when they close, which is
//     exactly what the menu flag fails to track). Then the fix writes r.FidelityFX.FSR3.Enabled "1" with the variable's
//     own set-by priority: the handler's gain branch, same string, same call, same priority.
//   - Right after every call of the menu-visibility function, if its flag is now clear, the handler itself is called
//     with "gained" once more (as an earlier version did): it re-reads the flag and Altar.FSR3.Enabled and switches FSR on.
//   - Checked at the focus gain (at once), after every menu-visibility call, and every 100 ms by a timer of the game
//     thread while a restore is pending and the window has the focus (SetTimer without a window: UE's message pump
//     dispatches it on the game thread). Nothing runs while nothing is pending.
// The player's choice wins: an upscaler other than FSR chosen meanwhile clears the mark (Altar.FSR3.Enabled off), and
// FSR switched on by the menu's own apply clears it too. Frame generation follows the player's option through the FSR 3
// plugin, as on any focus gain. Nothing is written to the settings or the save.
// Both functions are found by byte pattern (their code, the menu flag's offset checked against each other, the cvar
// slots and strings of both branches checked); each gets a 5-byte jump at its entry (one atomic 8-byte write) to a
// stub, and a trampoline runs its first instruction.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// Altar's focus handler: void (GraphicsManager*, bool bActive). Build 19115871: exe+0x495EE00.
//   +0x39  48 83 3D <disp32> 00   cmp qword [static IConsoleVariable* r.FidelityFX.FSR3.Enabled], 0
//   +0x85  0F B6 80 <disp32>      movzx eax, byte [uiSubsystem + bIsMenuVisible]
//   +0xA0  48 8B 0D <disp32>      mov rcx, [IConsoleVariable* Altar.FSR3.Enabled]
//   +0xAA  FF 90 <disp32>         call [vtable + GetBool]
//   gain:  +0xB4 mov rbx,[r.FidelityFX.FSR3.Enabled] / +0xC1 call [vtable+GetFlags] / +0xC4 lea rdx,[L"1"] / +0xCB jmp +0xF8
//   loss:  +0xE1 mov rbx,[r.FidelityFX.FSR3.Enabled] / +0xEE call [vtable+GetFlags] / +0xF1 lea rdx,[L"0"]
//   both:  +0xF8 mov r8d,eax / mov rcx,rbx / mov rax,[rbx] / and r8d,0FF000000h / +0x108 call [vtable+Set]
constexpr const char* kHandler =
    "48 89 5C 24 08 57 48 83 EC 20 65 48 8B 04 25 58 00 00 00 48 8B D9 44 8B 05 ?? ?? ?? ?? 0F B6 FA B9 ?? ?? ?? ?? "
    "4E 8B 04 C0 42 8B 04 01 39 05 ?? ?? ?? ?? 0F 8F ?? ?? ?? ?? 48 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? "
    "48 8B 0D ?? ?? ?? ?? 33 D2 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 48 08 F7 C1 00 00 00 60 75 ?? "
    "48 8B C8 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 48 08 F7 C1 00 00 00 60 75 ?? "
    "0F B6 80 ?? ?? ?? ?? 88 83 ?? ?? ?? ?? 40 84 FF 74 ?? 80 BB ?? ?? ?? ?? 00 75 ?? "
    "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 84 C0 74 ?? 48 8B 1D ?? ?? ?? ?? "
    "48 8B CB 48 8B 03 FF 50 ?? 48 8D 15 ?? ?? ?? ?? EB ?? "
    "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 84 C0 74 ?? 48 8B 1D ?? ?? ?? ?? "
    "48 8B CB 48 8B 03 FF 50 ?? 48 8D 15 ?? ?? ?? ?? "
    "44 8B C0 48 8B CB 48 8B 03 41 81 E0 00 00 00 FF FF 90 ?? ?? ?? ??";
constexpr size_t kFsrCvarCmp = 0x39, kMenuFlagLoad = 0x85, kAltarFsrLoad = 0xA0, kGetBoolCall = 0xAA;
constexpr size_t kGainCvarLoad = 0xB4, kGainGetFlags = 0xC1, kGainOneLea = 0xC4, kGainJmp = 0xCB;
constexpr size_t kLossAltarLoad = 0xCD, kLossCvarLoad = 0xE1, kLossGetFlags = 0xEE, kLossZeroLea = 0xF1;
constexpr size_t kSetSequence = 0xF8, kSetCall = 0x108;

// VAltarUISubsystem's menu-visibility function: (this, bool bVisible, ctx, ctx, uint8 menuType). Build 19115871:
// exe+0x4729660. +0x56: C6 87 <disp32> 01  mov byte [this + bIsMenuVisible], 1
constexpr const char* kMenuSetter =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 49 8B D9 49 8B E8 0F B6 F2 48 8B F9 E8 ?? ?? ?? ?? "
    "4C 8B C5 48 89 44 24 28 48 8D 54 24 28 48 8D 4C 24 30 E8 ?? ?? ?? ?? 4C 8B C3 48 8D 54 24 20 48 8B CF E8 ?? ?? ?? ?? "
    "48 8D 4C 24 20 40 84 F6 74 ?? C6 87 ?? ?? ?? ?? 01";
constexpr size_t kMenuFlagStore = 0x56;

// Both functions start with the same 5-byte instruction, moved to the trampolines: mov [rsp+8], rbx
constexpr uint8_t kFirstInsn[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};

constexpr UINT kTimerMs = 100;      // while a restore is pending and the window has the focus
constexpr int kMaxEventLines = 60;  // per session, then only the periodic statistics

using HandlerFn = uint64_t (*)(void* manager, uint8_t active);
using MenuSetterFn = uint64_t (*)(void* ui, uint8_t visible, void* a3, void* a4, uint64_t menuType);
using GetBoolFn = bool (*)(void* cvar);
using GetFlagsFn = int32_t (*)(void* cvar);
using SetFn = void (*)(void* cvar, const wchar_t* value, int32_t setBy);

uint8_t* g_handler = nullptr;   // the game's functions
uint8_t* g_setter = nullptr;
HandlerFn g_handlerOrig = nullptr;    // trampolines (fakes in the self-test)
MenuSetterFn g_setterOrig = nullptr;
void** g_fsrCvarSlot = nullptr;       // static IConsoleVariable* r.FidelityFX.FSR3.Enabled (filled at the handler's first call)
void** g_altarFsrCvarSlot = nullptr;  // IConsoleVariable* Altar.FSR3.Enabled
uint32_t g_getBool = 0;               // vtable offsets of IConsoleVariable::GetBool, GetFlags, Set
uint32_t g_getFlags = 0;
uint32_t g_set = 0;
const wchar_t* g_one = nullptr;       // the handler's L"1"
uint32_t g_menuFlag = 0;              // VAltarUISubsystem::bIsMenuVisible
bool g_freeze = false;                // the freeze subsystem can be read
uint8_t* g_region = nullptr;

std::atomic<bool> g_enabled{true};
// game thread only
void* g_manager = nullptr;
bool g_appActive = true;
bool g_pending = false;
bool g_faulted = false;   // a check faulted: nothing more is done this session
UINT_PTR g_timer = 0;
ULONGLONG g_gainTick = 0;
int g_eventLines = 0;
std::atomic<uint64_t> g_lost{0}, g_gained{0}, g_gainedLeftOff{0}, g_restored{0}, g_restoreCalls{0}, g_restoredDirect{0},
    g_directCalls{0}, g_timerChecks{0}, g_playerChoice{0}, g_timerTicks{0};
uint64_t g_logged = 0;

bool CvarBool(void** slot)
{
    void* cvar = slot ? *slot : nullptr;
    if (!cvar) return false;
    void** vtable = *static_cast<void***>(cvar);
    return reinterpret_cast<GetBoolFn>(vtable[g_getBool / sizeof(void*)])(cvar);
}
bool FsrOn() { return CvarBool(g_fsrCvarSlot); }
bool AltarFsrOn() { return CvarBool(g_altarFsrCvarSlot); }

// The handler's gain branch: Set(L"1", GetFlags & ECVF_SetByMask) on r.FidelityFX.FSR3.Enabled.
void SetFsrOnAsTheHandlerDoes()
{
    void* cvar = g_fsrCvarSlot ? *g_fsrCvarSlot : nullptr;
    if (!cvar) return;
    void** vtable = *static_cast<void***>(cvar);
    const int32_t flags = reinterpret_cast<GetFlagsFn>(vtable[g_getFlags / sizeof(void*)])(cvar);
    reinterpret_cast<SetFn>(vtable[g_set / sizeof(void*)])(cvar, g_one, static_cast<int32_t>(static_cast<uint32_t>(flags) & 0xFF000000u));
}

void Event(const char* fmt, int a = 0, int b = 0, unsigned c = 0)
{
    if (g_eventLines >= kMaxEventLines) return;
    if (++g_eventLines == kMaxEventLines) {
        Log("[focus] (further events only in the statistics)");
        return;
    }
    Log(fmt, a, b, c);
}

int64_t QpcFrequency()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart ? f.QuadPart : 1;
}

unsigned MsSinceGain() { return static_cast<unsigned>(GetTickCount64() - g_gainTick); }

// user32's SetTimer / KillTimer, resolved at start-up (the game has user32 loaded; the DLL does not link it)
using SetTimerFn = UINT_PTR(WINAPI*)(HWND, UINT_PTR, UINT, TIMERPROC);
using KillTimerFn = BOOL(WINAPI*)(HWND, UINT_PTR);
SetTimerFn g_setTimer = nullptr;
KillTimerFn g_killTimer = nullptr;

void CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD);
void StartTimer()
{
    if (!g_timer && g_setTimer) g_timer = g_setTimer(nullptr, 0, kTimerMs, &TimerProc);
}
void StopTimer()
{
    if (g_timer && g_killTimer) g_killTimer(nullptr, g_timer);
    g_timer = 0;
}

// Restores FSR if the player is back in the game; true when nothing is left pending. ui: the UI subsystem right after
// its menu-visibility function ran (its flag is then current), else nullptr. Game thread.
bool TryRestore(const uint8_t* ui)
{
    if (!g_pending || g_faulted) return true;
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (FsrOn()) {   // switched on again meanwhile (the menu's apply)
        g_pending = false;
        return true;
    }
    if (!AltarFsrOn()) {   // another upscaler chosen meanwhile: the player's choice
        g_pending = false;
        g_playerChoice.fetch_add(1, std::memory_order_relaxed);
        Event("[focus] FSR is no longer the chosen upscaler: left as it is");
        return true;
    }
    if (!g_appActive || !g_manager) return false;
    if (ui && !ui[g_menuFlag]) {
        // the menu flag was cleared (pause menu, main menu): the game's own "focus gained" branch, run again
        g_pending = false;
        g_restoreCalls.fetch_add(1, std::memory_order_relaxed);
        g_handlerOrig(g_manager, 1);
        if (FsrOn()) {
            g_restored.fetch_add(1, std::memory_order_relaxed);
            Event("[focus] FSR switched back on when the menu closed (the game's own focus-gain branch, %d ms after the focus gain)",
                  static_cast<int>(MsSinceGain()));
        }
        return true;
    }
    // No menu holds the world (no named freeze), whatever the menu flag says (the game leaves it set after most menus).
    int32_t freezes = -1, mode = -1;
    if (!g_freeze || !ReadMenuFreeze(&freezes, &mode) || freezes != 0) return false;
    g_pending = false;
    g_directCalls.fetch_add(1, std::memory_order_relaxed);
    SetFsrOnAsTheHandlerDoes();
    if (FsrOn()) {
        g_restoredDirect.fetch_add(1, std::memory_order_relaxed);
        Event("[focus] FSR switched back on: no menu open (freeze mode %d, menu flag %d), %u ms after the focus gain", mode,
              ui ? ui[g_menuFlag] : -1, MsSinceGain());
    }
    return true;
}

// TryRestore under SEH: a fault (the game tearing its objects down while a check is due) stops the checks for good
// instead of taking the game down.
bool TryRestoreGuarded(bool* faulted)
{
    __try {
        return TryRestore(nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return true;
    }
}

void CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_timerChecks.fetch_add(1, std::memory_order_relaxed);
    bool faulted = false;
    const bool done = TryRestoreGuarded(&faulted);
    if (faulted) {
        g_pending = false;
        g_faulted = true;
        Log("[focus] a check faulted: fix switched off for this session");
    }
    if (done || !g_appActive || !g_enabled.load(std::memory_order_relaxed)) StopTimer();
    QueryPerformanceCounter(&t1);
    g_timerTicks.fetch_add(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
}

uint64_t HandlerHook(void* manager, uint8_t active)
{
    const uint64_t r = g_handlerOrig(manager, active);
    g_manager = manager;
    g_appActive = active != 0;
    if (!active) {
        g_lost.fetch_add(1, std::memory_order_relaxed);
        StopTimer();
        if (AltarFsrOn() && !FsrOn()) g_pending = true;   // the game switched FSR off for this focus loss
    } else {
        g_gained.fetch_add(1, std::memory_order_relaxed);
        g_gainTick = GetTickCount64();
        if (FsrOn()) g_pending = false;                    // and back on
        else if (g_pending) {
            g_gainedLeftOff.fetch_add(1, std::memory_order_relaxed);
            if (!TryRestore(nullptr) && g_enabled.load(std::memory_order_relaxed) && g_setTimer) {
                int32_t freezes = -1, mode = -1;
                if (!g_freeze || !ReadMenuFreeze(&freezes, &mode)) freezes = mode = -1;
                Event("[focus] focus regained with FSR left off by the game (a menu holds the world: %d named freezes, mode %d): "
                      "waiting for it to close",
                      freezes, mode);
                StartTimer();
            }
        }
    }
    return r;
}

uint64_t SetterHook(void* ui, uint8_t visible, void* a3, void* a4, uint64_t menuType)
{
    const uint64_t r = g_setterOrig(ui, visible, a3, a4, menuType);
    if (g_pending && g_appActive && ui && TryRestore(static_cast<const uint8_t*>(ui))) StopTimer();
    return r;
}

// A free region within rel32 reach of the game's code.
uint8_t* AllocNearImage(size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const GameModule& game = Game();
    const uintptr_t base = reinterpret_cast<uintptr_t>(game.base);
    const uintptr_t end = base + game.sizeOfImage;
    constexpr uintptr_t kReach = 0x70000000;
    for (uintptr_t a = (end + gran - 1) & ~(gran - 1); a + size < base + kReach;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.State == MEM_FREE && a + size <= regionEnd)
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<uint8_t*>(p);
        a = (regionEnd + gran - 1) & ~(gran - 1);
    }
    for (uintptr_t a = (base & ~(gran - 1)) - gran; a > gran && end - a < kReach; a -= gran)
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            return static_cast<uint8_t*>(p);
    return nullptr;
}

// jmp [rip+0]; dq target
void WriteAbsJump(uint8_t* at, const void* target)
{
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, jmp, sizeof jmp);
    memcpy(at + sizeof jmp, &target, sizeof target);
}

// The function's first 5 bytes become "jmp rel32 stub" in one atomic 8-byte write (functions start 16-byte aligned).
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

bool ApplyFocusRestoreFix(bool beforeGameCode)
{
    if (g_handler) return true;
    const GameModule& game = Game();
    const auto handlers = FindAll(game.text, ParsePattern(kHandler));
    const auto setters = FindAll(game.text, ParsePattern(kMenuSetter));
    if (handlers.size() != 1 || setters.size() != 1) {
        Log("[focus] focus handler / menu-visibility function not found (%zu / %zu matches): fix not installed", handlers.size(),
            setters.size());
        return false;
    }
    uint8_t* const handler = handlers[0];
    uint8_t* const setter = setters[0];
    uint32_t menuFlag = 0, menuFlagSet = 0, getBool = 0, getBoolLoss = 0, set = 0;
    memcpy(&menuFlag, handler + kMenuFlagLoad + 3, 4);
    memcpy(&menuFlagSet, setter + kMenuFlagStore + 2, 4);
    memcpy(&getBool, handler + kGetBoolCall + 2, 4);
    memcpy(&getBoolLoss, handler + kLossAltarLoad + 12, 4);
    memcpy(&set, handler + kSetCall + 2, 4);
    const uint32_t getFlags = handler[kGainGetFlags + 2];
    void** const fsrSlot = reinterpret_cast<void**>(RipTarget(handler + kFsrCvarCmp, 3, 8));
    void** const altarSlot = reinterpret_cast<void**>(RipTarget(handler + kAltarFsrLoad, 3, 7));
    const auto* const one = reinterpret_cast<const wchar_t*>(RipTarget(handler + kGainOneLea, 3, 7));
    const auto* const zero = reinterpret_cast<const wchar_t*>(RipTarget(handler + kLossZeroLea, 3, 7));
    const auto inData = [&](const void* p, uintptr_t align) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(p), b = reinterpret_cast<uintptr_t>(game.base);
        return a > b && a + 8 < b + game.sizeOfImage && !game.text.Contains(p) && (a & (align - 1)) == 0;
    };
    const bool slotsAgree = RipTarget(handler + kGainCvarLoad, 3, 7) == reinterpret_cast<uint8_t*>(fsrSlot) &&
                            RipTarget(handler + kLossCvarLoad, 3, 7) == reinterpret_cast<uint8_t*>(fsrSlot) &&
                            RipTarget(handler + kLossAltarLoad, 3, 7) == reinterpret_cast<uint8_t*>(altarSlot);
    const bool branchesAgree = handler[kLossGetFlags + 2] == getFlags && getBoolLoss == getBool &&
                               handler + kGainJmp + 2 + static_cast<int8_t>(handler[kGainJmp + 1]) == handler + kSetSequence;
    if (menuFlag != menuFlagSet || menuFlag == 0 || menuFlag > 0x1000 || getBool == 0 || getBool > 0x400 || (getBool & 7) ||
        getFlags == 0 || (getFlags & 7) || set == 0 || set > 0x400 || (set & 7) || !inData(fsrSlot, 8) || !inData(altarSlot, 8) ||
        !inData(one, 2) || !inData(zero, 2) || !slotsAgree || !branchesAgree) {
        Log("[focus] unexpected values (menu flag +0x%X / +0x%X, GetBool +0x%X, GetFlags +0x%X, Set +0x%X, cvar slots %s, branches %s): "
            "fix not installed",
            menuFlag, menuFlagSet, getBool, getFlags, set, slotsAgree ? "agree" : "differ", branchesAgree ? "agree" : "differ");
        return false;
    }
    if (one[0] != L'1' || one[1] != 0 || zero[0] != L'0' || zero[1] != 0) {
        Log("[focus] the handler's values are not \"1\" / \"0\": fix not installed");
        return false;
    }
    if (memcmp(handler, kFirstInsn, 5) != 0 || memcmp(setter, kFirstInsn, 5) != 0) {
        Log("[focus] the functions do not start as expected (already patched?): fix not installed");
        return false;
    }
    // "no menu holds the world": the freeze subsystem (found with the loading-screen hook's search); without it the fix
    // only covers menus that clear the menu flag (pause menu, main menu), as an earlier version did
    const bool freeze = FindFreezeSubsystem();
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        g_setTimer = reinterpret_cast<SetTimerFn>(GetProcAddress(user32, "SetTimer"));
        g_killTimer = reinterpret_cast<KillTimerFn>(GetProcAddress(user32, "KillTimer"));
    }
    if (!g_setTimer || !g_killTimer) {
        g_setTimer = nullptr;
        g_killTimer = nullptr;
        Log("[focus] user32 timers not available: FSR comes back at the focus gain or when a menu's visibility changes");
    }
    if (!freeze) Log("[focus] freeze subsystem not found: FSR comes back only after the pause menu or the main menu");
    // stubs to the hooks (within rel32 reach), trampolines (first instruction, jmp back)
    constexpr size_t kRegion = 0x1000;
    uint8_t* const region = AllocNearImage(kRegion);
    if (!region) {
        Log("[focus] no free memory near the game's code: fix not installed");
        return false;
    }
    uint8_t* const stubH = region, *const stubS = region + 0x20, *const trampH = region + 0x40, *const trampS = region + 0x60;
    WriteAbsJump(stubH, reinterpret_cast<const void*>(&HandlerHook));
    WriteAbsJump(stubS, reinterpret_cast<const void*>(&SetterHook));
    memcpy(trampH, kFirstInsn, 5);
    WriteAbsJump(trampH + 5, handler + 5);
    memcpy(trampS, kFirstInsn, 5);
    WriteAbsJump(trampS + 5, setter + 5);
    DWORD old;
    VirtualProtect(region, kRegion, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, kRegion);
    g_handlerOrig = reinterpret_cast<HandlerFn>(trampH);
    g_setterOrig = reinterpret_cast<MenuSetterFn>(trampS);
    g_fsrCvarSlot = fsrSlot;
    g_altarFsrCvarSlot = altarSlot;
    g_getBool = getBool;
    g_getFlags = getFlags;
    g_set = set;
    g_one = one;
    g_menuFlag = menuFlag;
    g_freeze = freeze;
    g_region = region;
    // the setter first: until the handler is detoured nothing is pending, so a half-installed fix does nothing
    if (!PatchEntry(setter, stubS, beforeGameCode)) {
        Log("[focus] could not patch the menu-visibility function at exe+0x%llX: fix not installed",
            static_cast<unsigned long long>(setter - game.base));
        return false;
    }
    if (!PatchEntry(handler, stubH, beforeGameCode)) {
        Log("[focus] could not patch the focus handler at exe+0x%llX: fix not active", static_cast<unsigned long long>(handler - game.base));
        return false;
    }
    g_handler = handler;
    g_setter = setter;
    Log("[focus] focus handler exe+0x%llX and menu-visibility function exe+0x%llX detoured (menu flag +0x%X, cvar Set +0x%X): FSR "
        "comes back once no menu is open after a focus gain",
        static_cast<unsigned long long>(handler - game.base), static_cast<unsigned long long>(setter - game.base), menuFlag, set);
    return true;
}

void SetFocusRestoreEnabled(bool enabled) { g_enabled = enabled; }

void LogFocusRestoreStats()
{
    const uint64_t lost = g_lost.load(), gained = g_gained.load(), leftOff = g_gainedLeftOff.load(), calls = g_restoreCalls.load(),
                   restored = g_restored.load(), direct = g_restoredDirect.load(), directCalls = g_directCalls.load(),
                   checks = g_timerChecks.load(), choice = g_playerChoice.load();
    const uint64_t sum = lost + gained + leftOff + calls + restored + direct + directCalls + checks + choice;
    if (!g_handler || sum == g_logged) return;
    g_logged = sum;
    Log("[focus] focus lost %llu, regained %llu (FSR left off by the game: %llu); FSR switched back on: %llu with no menu open "
        "(of %llu), %llu by the game's own branch after the pause/main menu (of %llu); another upscaler chosen meanwhile %llu; "
        "timer checks %llu (%.1f us each on average)",
        static_cast<unsigned long long>(lost), static_cast<unsigned long long>(gained), static_cast<unsigned long long>(leftOff),
        static_cast<unsigned long long>(direct), static_cast<unsigned long long>(directCalls), static_cast<unsigned long long>(restored),
        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(choice), static_cast<unsigned long long>(checks),
        checks ? 1e6 * static_cast<double>(g_timerTicks.load()) / static_cast<double>(QpcFrequency()) / static_cast<double>(checks) : 0.0);
}

bool FocusRestoreInfo(FocusRestoreInfoOut* out)
{
    if (!g_handler) return false;
    out->handler = g_handler;
    out->setter = g_setter;
    out->fsrCvarSlot = g_fsrCvarSlot;
    out->altarFsrCvarSlot = g_altarFsrCvarSlot;
    out->getBoolOffset = g_getBool;
    out->getFlagsOffset = g_getFlags;
    out->setOffset = g_set;
    out->one = g_one;
    out->menuFlagOffset = g_menuFlag;
    out->freezeAvailable = g_freeze;
    out->handlerHook = reinterpret_cast<void*>(&HandlerHook);
    out->setterHook = reinterpret_cast<void*>(&SetterHook);
    out->timerProc = reinterpret_cast<void*>(&TimerProc);
    out->handlerTrampoline = reinterpret_cast<uint8_t*>(g_handlerOrig);
    out->setterTrampoline = reinterpret_cast<uint8_t*>(g_setterOrig);
    out->restored = g_restored.load();
    out->restoreCalls = g_restoreCalls.load();
    out->restoredDirect = g_restoredDirect.load();
    out->timerChecks = g_timerChecks.load();
    out->pending = g_pending;
    out->timerRunning = g_timer != 0;
    return true;
}

void FocusRestoreTestOriginals(void* fakeHandler, void* fakeSetter)
{
    StopTimer();
    g_handlerOrig = reinterpret_cast<HandlerFn>(fakeHandler);
    g_setterOrig = reinterpret_cast<MenuSetterFn>(fakeSetter);
    g_manager = nullptr;
    g_appActive = true;
    g_pending = false;
}

}  // namespace opf
