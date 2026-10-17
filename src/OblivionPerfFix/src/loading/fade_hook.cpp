// The hook PrepareCollisionBehindLoadingScreen (collision_prepare.cpp) runs from: the moment a door, city gate or fast-travel loading screen ends.
//
// During a transition the game keeps the world frozen (VFreezeInMenuSubsystem in its loading mode, entered by
// VLevelChangeData::OnFadeToBlackEndEventReceived). Its fade gate (a 0.5 s looping timer) waits until the world has
// streamed in, then calls the fade-to-game function on the next tick: that removes the loading screen and starts the
// fade-in, and arms a timer that unfreezes the world FadeInDurationUntilUnfreeze (0.5 s) into the fade-in.
//
// We wrap the fade-to-game delegate: right before the game's own fade-to-game runs, PrepareCollisionBehindLoadingScreen hands the new area to the
// physics engine while the world is still frozen, and finishes that work right after. The game's fade-to-game always
// runs, unchanged and at the same moment; the freeze, the fade and the unfreeze are not touched.
//
// Only the transition's own freeze counts as "frozen behind the loading screen": the loading-mode flag the game's
// unfreeze timer clears is set, the fade gate timer exists, the freeze subsystem is in its loading mode and no menu
// holds a named freeze. The game's unfreeze timer body is located only to read that flag's offset and the freeze
// subsystem's getter from its code; it is never called.
//
// Hook: the two call slots (Execute, ExecuteIfSafe) of the fade-to-game delegate's vtable (pointer writes, atomic,
// safe while the game runs). Everything is found by byte patterns and the offsets are read from the game's code.
#include "common.h"

namespace opf {
namespace {

using ExecFn = void (*)(void* delegateInstance);
using ExecIfSafeFn = bool (*)(void* delegateInstance);

// Bytes from game build 19115871.
// The game's unfreeze timer body (TBaseFunctorDelegateInstance::Execute of its lambda), exe+0x4881640:
//   push rbx / sub rsp,20h / mov rbx,[rcx+18h] / cmp byte [rbx+200h],0 / je / xor ecx,ecx / call GetFreezeSubsystem /
//   xor edx,edx / mov rcx,rax / call RequestMode / mov byte [rbx+200h],0 / mov byte [rbx+1F1h],0 / add rsp,20h / pop rbx / ret
const char* const kUnfreeze =
    "40 53 48 83 EC 20 48 8B 59 18 80 BB ?? ?? 00 00 00 74 ?? 33 C9 E8 ?? ?? ?? ?? 33 D2 48 8B C8 E8 ?? ?? ?? ?? "
    "C6 83 ?? ?? 00 00 00 C6 83 ?? ?? 00 00 00 48 83 C4 20 5B C3";
// The fade-to-game function (lambda body), exe+0x4877E80, up to "clear the fade gate timer handle":
//   ... mov r14,rcx / mov rcx,[rcx] / mov rax,[rcx] / call [rax+188h] (GetWorld) / ... / mov rbx,[r14] / ...
//   lea rdx,[rbx+240h] / call IsTimerActive / test / je / mov rdx,[rbx+240h] / mov rcx,rdi / call ClearTimer /
//   xor esi,esi / mov [rbx+240h],rsi
const char* const kFadeToGame =
    "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 18 FF FF FF 48 81 EC C0 01 00 00 "
    "0F 29 70 C8 0F 29 78 B8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 90 00 00 00 4C 8B F1 48 8B 09 48 8B 01 FF 90 88 01 00 00 "
    "48 8B C8 E8 ?? ?? ?? ?? 49 8B 1E 48 8B C8 48 8B F8 48 8D 93 ?? ?? 00 00 E8 ?? ?? ?? ?? 48 85 C0 74 ?? "
    "48 8B 93 ?? ?? 00 00 48 8B CF E8 ?? ?? ?? ?? 33 F6 48 89 B3 ?? ?? 00 00";
constexpr size_t kFadeTimerLea = 0x5F;    // lea rdx,[rbx+disp32]: disp at +3
constexpr size_t kFadeTimerMov = 0x70;    // mov rdx,[rbx+disp32]: disp at +3
constexpr size_t kFadeTimerClear = 0x81;  // mov [rbx+disp32],rsi: disp at +3
constexpr size_t kFadeGetWorld = 0x48;    // call [rax+disp32] = UObject::GetWorld on the VLevelChangeData
// The fade-to-game delegate's Execute / ExecuteIfSafe: add rcx,18h then jmp / call the fade-to-game function.
const char* const kExec = "48 83 C1 ?? E9 ?? ?? ?? ??";
const char* const kExecIfSafe = "48 83 EC 28 48 83 C1 ?? E8 ?? ?? ?? ?? B0 01 48 83 C4 28 C3";

struct State {
    bool found = false, installed = false, tried = false;
    uint8_t* unfreeze = nullptr;     // the game's unfreeze timer body (read, never called)
    uint8_t* fadeToGame = nullptr;
    uint8_t* exec = nullptr;         // delegate thunks (the original slot values)
    uint8_t* execIfSafe = nullptr;
    void** execSlot = nullptr;       // their vtable slots
    void** execIfSafeSlot = nullptr;
    uint32_t fadeCapture = 0;        // where the fade-to-game lambda sits in its delegate instance (0x18)
    uint32_t flagOffset = 0;         // VLevelChangeData: "the transition's loading mode is on" (0x200)
    uint32_t flag2Offset = 0;        // VLevelChangeData: cleared by the unfreeze as well (0x1F1)
    uint32_t timerOffset = 0;        // VLevelChangeData: the fade gate timer handle (0x240)
    uint8_t* getFreeze = nullptr;    // UVFreezeInMenuSubsystem getter called by the unfreeze timer (exe+0x4959880)
    uint32_t modeOffset = 0;         // UVFreezeInMenuSubsystem: current mode (0x3C), from RequestMode's code
    uint32_t stackNumOffset = 0;     // UVFreezeInMenuSubsystem: FreezeStack.Num (0xB0), from IsFreezing's code
    uint32_t getWorldSlot = 0;       // UObject::GetWorld vtable offset (0x188), from the fade-to-game's own call
    ExecFn origExec = nullptr;
    ExecIfSafeFn origExecIfSafe = nullptr;
};
State g;

uint32_t Disp32(const uint8_t* at) { return *reinterpret_cast<const uint32_t*>(at); }

// UVFreezeInMenuSubsystem::IsFreezing is "FreezeStack.Num != 0": cmp dword [rcx+disp32],0 / setne al / ret.
// It is found through the engine's native function table, which pairs the name "IsFreezing" with its exec thunk
// (that thunk calls the implementation). Returns the Num offset, or 0 if not exactly one candidate.
uint32_t FindFreezeStackNumOffset(const GameModule& game)
{
    static const char kName[] = "IsFreezing";
    const Pattern impl = ParsePattern("83 B9 ?? ?? 00 00 00 0F 95 C0 C3");
    uint32_t found = 0;
    int distinct = 0;
    for (uint8_t* p = game.rdata.begin + 1; p + sizeof kName <= game.rdata.end; ++p) {
        if (p[-1] != 0 || memcmp(p, kName, sizeof kName) != 0) continue;
        const uintptr_t name = reinterpret_cast<uintptr_t>(p);
        for (uint8_t* q = game.rdata.begin; q + 16 <= game.rdata.end; q += 8) {
            if (*reinterpret_cast<const uintptr_t*>(q) != name) continue;
            uint8_t* thunk = *reinterpret_cast<uint8_t**>(q + 8);
            if (!game.text.Contains(thunk) || !game.text.Contains(thunk + 0x40)) continue;
            for (int k = 0; k < 0x3B; ++k) {
                if (thunk[k] != 0xE8) continue;
                uint8_t* target = RipTarget(thunk + k, 1, 5);
                if (!game.text.Contains(target) || !MatchesAt(target, impl)) continue;
                const uint32_t off = Disp32(target + 2);
                if (off != found) { found = off; ++distinct; }
            }
        }
    }
    return distinct == 1 ? found : 0;
}

// The only qword in .rdata that holds this code address (a vtable slot), or nullptr.
void** FindSlot(const GameModule& game, const uint8_t* target)
{
    void** found = nullptr;
    int hits = 0;
    const uintptr_t want = reinterpret_cast<uintptr_t>(target);
    for (uint8_t* p = game.rdata.begin; p + 8 <= game.rdata.end; p += 8)
        if (*reinterpret_cast<const uintptr_t*>(p) == want) { found = reinterpret_cast<void**>(p); ++hits; }
    return hits == 1 ? found : nullptr;
}

bool Find(const GameModule& game)
{
    auto rva = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
    const auto unfreezeHits = FindAll(game.text, ParsePattern(kUnfreeze));
    const auto fadeHits = FindAll(game.text, ParsePattern(kFadeToGame));
    if (unfreezeHits.size() != 1 || fadeHits.size() != 1) {
        Log("[fade] game code not recognised (unfreeze timer %zu, fade-to-game %zu matches): hook not installed",
            unfreezeHits.size(), fadeHits.size());
        return false;
    }
    uint8_t* const u = unfreezeHits[0];
    uint8_t* const f = fadeHits[0];
    // unfreeze timer: mov rbx,[rcx+18h]; cmp byte [rbx+flag],0; ...; mov byte [rbx+flag],0; mov byte [rbx+flag2],0
    g.flagOffset = Disp32(u + 12);
    g.flag2Offset = Disp32(u + 45);
    if (Disp32(u + 38) != g.flagOffset || g.flagOffset > 0x1000 || g.flag2Offset > 0x1000 || u[9] != 0x18) {
        Log("[fade] unfreeze timer at exe+0x%X does not clear its own flag (+0x%X / +0x%X): hook not installed", rva(u),
            g.flagOffset, Disp32(u + 38));
        return false;
    }
    // fade-to-game: the same gate timer handle is tested, cleared and zeroed
    g.timerOffset = Disp32(f + kFadeTimerLea + 3);
    g.getWorldSlot = (f[kFadeGetWorld] == 0xFF && f[kFadeGetWorld + 1] == 0x90) ? Disp32(f + kFadeGetWorld + 2) : 0;
    if (g.getWorldSlot == 0 || g.getWorldSlot > 0x800 || g.getWorldSlot % 8) {
        Log("[fade] fade-to-game at exe+0x%X: GetWorld call not recognised: hook not installed", rva(f));
        return false;
    }
    if (Disp32(f + kFadeTimerMov + 3) != g.timerOffset || Disp32(f + kFadeTimerClear + 3) != g.timerOffset ||
        g.timerOffset > 0x1000) {
        Log("[fade] fade-to-game at exe+0x%X: gate timer handle offsets differ: hook not installed", rva(f));
        return false;
    }
    // the delegate thunks that call it, and their vtable slots
    for (uint8_t* t : FindAll(game.text, ParsePattern(kExec)))
        if (RipTarget(t, 5, 9) == f) { g.exec = t; break; }
    for (uint8_t* t : FindAll(game.text, ParsePattern(kExecIfSafe)))
        if (RipTarget(t, 9, 13) == f) { g.execIfSafe = t; break; }
    if (!g.exec || !g.execIfSafe || g.exec[3] != g.execIfSafe[7]) {
        Log("[fade] fade-to-game delegate thunks not found: hook not installed");
        return false;
    }
    g.fadeCapture = g.exec[3];
    g.execSlot = FindSlot(game, g.exec);
    g.execIfSafeSlot = FindSlot(game, g.execIfSafe);
    if (!g.execSlot || !g.execIfSafeSlot || g.execIfSafeSlot != g.execSlot + 1) {
        Log("[fade] fade-to-game delegate vtable not found: hook not installed");
        return false;
    }
    g.unfreeze = u;
    g.fadeToGame = f;
    g.getFreeze = RipTarget(u + 0x15, 1, 5);
    // RequestMode (the unfreeze timer's second call): cmp [rcx+mode],edx / je +3 / mov [rcx+requested],edx / ret
    const uint8_t* requestMode = RipTarget(u + 0x1F, 1, 5);
    g.stackNumOffset = FindFreezeStackNumOffset(game);
    if (!game.text.Contains(requestMode) || !MatchesAt(requestMode, ParsePattern("39 51 ?? 74 03 89 51 ?? C3")) || !g.stackNumOffset ||
        g.stackNumOffset > 0x1000) {
        Log("[fade] freeze subsystem layout not recognised (RequestMode exe+0x%X, freeze stack +0x%X): hook not installed",
            rva(requestMode), g.stackNumOffset);
        return false;
    }
    g.modeOffset = requestMode[2];
    g.origExec = reinterpret_cast<ExecFn>(g.exec);
    g.origExecIfSafe = reinterpret_cast<ExecIfSafeFn>(g.execIfSafe);
    g.found = true;
    Log("[fade] fade-to-game exe+0x%X (gate timer handle +0x%X), unfreeze timer exe+0x%X (flags +0x%X, +0x%X), "
        "delegate slots exe+0x%X/+0x%X, freeze subsystem mode +0x%X, freeze stack +0x%X",
        rva(f), g.timerOffset, rva(u), g.flagOffset, g.flag2Offset, rva(g.execSlot), rva(g.execIfSafeSlot), g.modeOffset,
        g.stackNumOffset);
    return true;
}

// The freeze subsystem as the game's own unfreeze timer finds it; nullptr if not available.
const uint8_t* FreezeSubsystem()
{
    using GetFreezeFn = void* (*)(void*);
    return g.getFreeze ? static_cast<const uint8_t*>(reinterpret_cast<GetFreezeFn>(g.getFreeze)(nullptr)) : nullptr;
}

// Reads the freeze subsystem's mode and named-freeze count; false if it is not available.
bool ReadFreezeState(int32_t* mode, int32_t* stack)
{
    __try {
        const uint8_t* sub = FreezeSubsystem();
        if (!sub) return false;
        *mode = *reinterpret_cast<const int32_t*>(sub + g.modeOffset);
        *stack = *reinterpret_cast<const int32_t*>(sub + g.stackNumOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The transition's own freeze is on: loading-mode flag, gate timer, freeze mode 1, no named freeze (a menu such as the
// main menu or dialogue keeps one on the stack; then everything is left to the game).
bool TransitionFrozen(const uint8_t* lcd)
{
    if (lcd[g.flagOffset] == 0 || *reinterpret_cast<const uint64_t*>(lcd + g.timerOffset) == 0) return false;
    int32_t mode = -1, stack = -1;
    return ReadFreezeState(&mode, &stack) && mode == 1 && stack == 0;
}

// lcd->GetWorld, as the fade-to-game itself calls it; nullptr if it faults.
void* WorldOf(uint8_t* lcd)
{
    using GetWorldFn = void* (*)(void*);
    __try {
        void** vtable = *reinterpret_cast<void***>(lcd);
        return reinterpret_cast<GetWorldFn>(vtable[g.getWorldSlot / sizeof(void*)])(lcd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// PrepareCollisionBehindLoadingScreen: hand the new area to physics before the game's fade-to-game runs; true = remember to finish afterwards.
bool PrepareFirst(void* delegateInstance)
{
    if (!CollisionPrepareEnabled()) return false;
    uint8_t* const lcd = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(delegateInstance) + g.fadeCapture);
    if (!lcd || !TransitionFrozen(lcd)) return false;
    void* const world = WorldOf(lcd);
    return world && PrepareCollisionBegin(world);
}

void ExecHook(void* delegateInstance)
{
    const uint64_t t0 = LedgerClockUs();  // HitchLedger: the fade-to-game frame and its time (PrepareCollisionBehindLoadingScreen included)
    const bool prepared = PrepareFirst(delegateInstance);
    g.origExec(delegateInstance);
    if (prepared) PrepareCollisionEnd();
    if (t0) LedgerFadeToGame(LedgerClockUs() - t0);
}

bool ExecIfSafeHook(void* delegateInstance)
{
    const uint64_t t0 = LedgerClockUs();
    const bool prepared = PrepareFirst(delegateInstance);
    const bool result = g.origExecIfSafe(delegateInstance);
    if (prepared) PrepareCollisionEnd();
    if (t0) LedgerFadeToGame(LedgerClockUs() - t0);
    return result;
}

}  // namespace

bool InstallFadeHook()
{
    if (g.installed) return true;
    const GameModule& game = Game();
    g.tried = true;
    if (!g.found && !Find(game)) return false;
    if (!WritePointer(g.execSlot, reinterpret_cast<void*>(&ExecHook)) ||
        !WritePointer(g.execIfSafeSlot, reinterpret_cast<void*>(&ExecIfSafeHook))) {
        WritePointer(g.execSlot, g.exec);
        Log("[fade] writing the delegate vtable failed: hook not installed");
        return false;
    }
    g.installed = true;
    Log("[fade] fade-to-game hook installed");
    return true;
}

// RestoreFsrAfterAltTab reads the freeze subsystem too (menus, dialogue and the main menu hold a named freeze). Locates the game code
// once (the same search as the hook's, nothing installed); call at start-up.
bool FindFreezeSubsystem()
{
    if (!g.found && !g.tried) {
        g.tried = true;
        Find(Game());
    }
    return g.found;
}

// The freeze subsystem's named-freeze count (IsFreezing = count != 0) and mode; false if not available. Game thread.
bool ReadMenuFreeze(int32_t* namedFreezes, int32_t* mode)
{
    return g.found && ReadFreezeState(mode, namedFreezes);
}

bool FadeHookInfo(FadeHookSites* out)
{
    if (!g.found || !out) return false;
    out->fadeToGame = g.fadeToGame;
    out->unfreeze = g.unfreeze;
    out->exec = g.exec;
    out->execIfSafe = g.execIfSafe;
    out->execSlot = g.execSlot;
    out->execIfSafeSlot = g.execIfSafeSlot;
    out->flagOffset = g.flagOffset;
    out->flag2Offset = g.flag2Offset;
    out->timerOffset = g.timerOffset;
    out->modeOffset = g.modeOffset;
    out->stackNumOffset = g.stackNumOffset;
    out->captureOffset = g.fadeCapture;
    out->getWorldSlot = g.getWorldSlot;
    out->execHook = reinterpret_cast<void*>(&ExecHook);
    out->execIfSafeHook = reinterpret_cast<void*>(&ExecIfSafeHook);
    return true;
}

void FadeHookTestFunctions(void* fakeExec, void* fakeExecIfSafe, void* fakeGetFreeze)
{
    g.getFreeze = static_cast<uint8_t*>(fakeGetFreeze);  // never run game code in the offline test
    if (fakeExec) g.origExec = reinterpret_cast<ExecFn>(fakeExec);
    if (fakeExecIfSafe) g.origExecIfSafe = reinterpret_cast<ExecIfSafeFn>(fakeExecIfSafe);
}

}  // namespace opf
