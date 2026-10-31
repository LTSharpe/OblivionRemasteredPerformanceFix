// AutosaveFollowsMenu: autosaves with every autosave option in the menu off (game bug).
//
// The Gameplay menu's four autosave switches (on rest, on wait, on travel, on entering or leaving an interior) set the old
// engine's bSaveOnRest, bSaveOnWait, bSaveOnTravel and bSaveOnInteriorExteriorSwitch (:GamePlay; the menu's handler,
// VGameplayMenuHandlersATO::SetGameplaySettings, build 19115871 exe+0x6571340). Two more autosaves of the old engine
// have no switch in the menu and stay on whatever the player picks:
//  - after a level-up (the level-up's end, exe+0x65FEF80, checks bSaveOnLevelUp:Gameplay, default 1, and calls the
//    autosave at exe+0x65FF287);
//  - the scripts' Autosave command (exe+0x6A07670, checks bAllowScriptedAutosave:General, default 1, calls it at
//    exe+0x6A07684): quests save at set points.
// So with all four switches off, the game still autosaves after each level-up and at the quests' save points.
//
// The fix sends those two calls of the autosave (exe+0x6703BC0) through a wrapper: when all four of the menu's
// switches are off (read at the moment of the call), the autosave is not made; when any one is on, it is made as before
// (its own switch, bSaveOnLevelUp or bAllowScriptedAutosave, still checked first by the game's code). The other
// autosave calls (rest, wait, travel, doors) are unchanged. Only the saving changes; the level-up and the script go on
// as before (the scripted command's caller still gets its result, 1).
#include "common.h"

#include <atomic>
#include <cstring>
#include <windows.h>

namespace opf {
namespace {

// The menu's handler, build 19115871 exe+0x657134F (in exe+0x6571340): r9b, r8b, dl and a stack byte are the four
// switches, each stored in its setting and saved through the settings collection:
//   41 0F B6 F1 / 41 0F B6 F8 / 0F B6 DA        movzx esi,r9b; movzx edi,r8b; movzx ebx,dl
//   48 8D 0D ????????  E8 ????????  90
//   88 1D [interior/exterior]                    mov [bSaveOnInteriorExteriorSwitch],bl     (+0x18)
//   48 8D 15 [same]  48 8D 0D ????????  48 8B 05 ????????  FF 50 20
//   40 88 3D [travel]                            mov [bSaveOnTravel],dil                    (+0x36)
//   ... FF 50 20
//   40 88 35 [rest]                              mov [bSaveOnRest],sil                      (+0x55)
//   ... FF 50 20  0F B6 44 24 60
//   88 05 [wait]                                 mov [bSaveOnWait],al                       (+0x79)
//   48 8D 15 [same]
constexpr const char* kMenu =
    "41 0F B6 F1 41 0F B6 F8 0F B6 DA 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 88 1D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? "
    "48 8D 0D ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? FF 50 20 40 88 3D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? "
    "48 8B 05 ?? ?? ?? ?? FF 50 20 40 88 35 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
    "FF 50 20 0F B6 44 24 60 88 05 ?? ?? ?? ?? 48 8D 15";
// Each store (offset, its disp's offset, its length); the lea rdx of the same setting follows it.
struct Store { size_t at, disp, len; };
constexpr Store kStores[4] = {{0x18, 2, 6}, {0x36, 3, 7}, {0x55, 3, 7}, {0x79, 2, 6}};

// The level-up's autosave, build 19115871 exe+0x65FF277:
//   44 38 35 [bSaveOnLevelUp]   cmp [bSaveOnLevelUp],r14b (r14 = 0)
//   74 0C                       je  past the call
//   48 8B 0D [manager]          mov rcx,[the save manager]
//   E8 [autosave]               call exe+0x6703BC0      <- redirected (+0x10)
//   48 8B 5C 24 68 48 8B 6C 24 70 48 83 C4 40 41 5E 5F 5E C3
constexpr const char* kLevelUp =
    "44 38 35 ?? ?? ?? ?? 74 0C 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 5C 24 68 48 8B 6C 24 70 48 83 C4 40 41 5E 5F 5E C3";
constexpr size_t kLevelUpCall = 0x10;
// The scripts' Autosave command, build 19115871 exe+0x6A07670:
//   48 83 EC 28                 sub rsp,28h
//   80 3D [bAllowScripted] 00   cmp [bAllowScriptedAutosave],0
//   74 0C 48 8B 0D [manager]
//   E8 [autosave]               call exe+0x6703BC0      <- redirected (+0x14)
//   B0 01 48 83 C4 28 C3        mov al,1; ...; ret
constexpr const char* kScripted =
    "48 83 EC 28 80 3D ?? ?? ?? ?? 00 74 0C 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? B0 01 48 83 C4 28 C3";
constexpr size_t kScriptedCall = 0x14;

using AutosaveFn = void (*)(void* manager);

uint8_t* g_levelUpCall = nullptr;
uint8_t* g_scriptedCall = nullptr;
uint8_t* g_stub = nullptr;
AutosaveFn g_autosave = nullptr;          // exe+0x6703BC0
const uint8_t* g_switches[4] = {};        // the menu's four settings' values
std::atomic<bool> g_enabled{true};
std::atomic<uint64_t> g_skippedLevelUp{0}, g_skippedScripted{0}, g_made{0};
uint64_t g_skippedLogged = 0;

bool AllMenuSwitchesOff()
{
    for (const uint8_t* s : g_switches)
        if (*s) return false;
    return true;
}

void Gate(void* manager, const char* what, std::atomic<uint64_t>& skipped)
{
    if (g_enabled.load(std::memory_order_relaxed) && AllMenuSwitchesOff()) {
        skipped.fetch_add(1, std::memory_order_relaxed);
        Log("[autosave] %s autosave not made: every autosave option in the menu is off", what);
        return;
    }
    g_made.fetch_add(1, std::memory_order_relaxed);
    g_autosave(manager);
}

void LevelUpHook(void* manager) { Gate(manager, "level-up", g_skippedLevelUp); }
void ScriptedHook(void* manager) { Gate(manager, "scripted", g_skippedScripted); }

bool Fail(const char* why)
{
    Log("[autosave] %s: fix not installed", why);
    return false;
}

}  // namespace

bool ApplyAutosaveFollowsMenu()
{
    if (g_stub) {
        Log("[autosave] fix already installed");
        return true;
    }
    const GameModule& game = Game();
    const auto menus = FindAll(game.text, ParsePattern(kMenu));
    const auto levelUps = FindAll(game.text, ParsePattern(kLevelUp));
    const auto scripteds = FindAll(game.text, ParsePattern(kScripted));
    if (menus.size() != 1 || levelUps.size() != 1 || scripteds.size() != 1) {
        Log("[autosave] the autosave code not found (%zu / %zu / %zu matches): fix not installed", menus.size(), levelUps.size(),
            scripteds.size());
        return false;
    }
    const uint8_t* switches[4];
    for (int i = 0; i < 4; ++i) {
        const Store& s = kStores[i];
        switches[i] = RipTarget(menus[0] + s.at, s.disp, s.len);
        if (!game.data.Contains(switches[i]) || RipTarget(menus[0] + s.at + s.len, 3, 7) != switches[i])
            return Fail("a menu switch's setting is not where the handler stores and saves it");
        for (int j = 0; j < i; ++j)
            if (switches[j] == switches[i]) return Fail("two menu switches share a setting");
    }
    uint8_t* levelUpCall = levelUps[0] + kLevelUpCall;
    uint8_t* scriptedCall = scripteds[0] + kScriptedCall;
    uint8_t* autosave = RipTarget(levelUpCall, 1, 5);
    if (!game.text.Contains(autosave) || RipTarget(scriptedCall, 1, 5) != autosave)
        return Fail("the two calls do not reach the same autosave function");
    if (RipTarget(levelUps[0] + 9, 3, 7) != RipTarget(scripteds[0] + 13, 3, 7))
        return Fail("the two calls do not pass the same save manager");
    // bSaveOnLevelUp sits among the menu's four settings (they are next to each other in .data).
    const uint8_t* levelUpSetting = RipTarget(levelUps[0], 3, 7);
    const uint8_t* lo = switches[0];
    const uint8_t* hi = switches[0];
    for (const uint8_t* s : switches) {
        if (s < lo) lo = s;
        if (s > hi) hi = s;
    }
    if (!game.data.Contains(levelUpSetting) || levelUpSetting < lo - 0x100 || levelUpSetting > hi + 0x100)
        return Fail("the level-up's setting is not among the menu's autosave settings");

    // Each stub: jmp [rip+0]; dq hook
    uint8_t* stub = AllocNearGame(32);
    if (!stub) return Fail("no memory near the game's code");
    const uint8_t jmp[] = {0xFF, 0x25, 0, 0, 0, 0};
    void* hooks[2] = {reinterpret_cast<void*>(&LevelUpHook), reinterpret_cast<void*>(&ScriptedHook)};
    for (int i = 0; i < 2; ++i) {
        memcpy(stub + 16 * i, jmp, sizeof jmp);
        memcpy(stub + 16 * i + sizeof jmp, &hooks[i], sizeof(void*));
    }
    DWORD old;
    if (!VirtualProtect(stub, 32, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return Fail("stub protection");
    }
    FlushInstructionCache(GetCurrentProcess(), stub, 32);
    const int64_t relLevelUp = stub - (levelUpCall + 5), relScripted = stub + 16 - (scriptedCall + 5);
    if (relLevelUp != static_cast<int32_t>(relLevelUp) || relScripted != static_cast<int32_t>(relScripted)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return Fail("stub out of reach");
    }
    g_autosave = reinterpret_cast<AutosaveFn>(autosave);
    memcpy(g_switches, switches, sizeof switches);
    uint8_t levelUpPatch[5]{0xE8}, scriptedPatch[5]{0xE8}, levelUpBack[5];
    const int32_t l32 = static_cast<int32_t>(relLevelUp), s32 = static_cast<int32_t>(relScripted);
    memcpy(levelUpPatch + 1, &l32, 4);
    memcpy(scriptedPatch + 1, &s32, 4);
    memcpy(levelUpBack, levelUpCall, 5);
    if (!WriteCode(levelUpCall, levelUpPatch)) return Fail("could not write the level-up's call");
    if (!WriteCode(scriptedCall, scriptedPatch)) {
        WriteCode(levelUpCall, levelUpBack);
        return Fail("could not write the scripted autosave's call");
    }
    g_levelUpCall = levelUpCall;
    g_scriptedCall = scriptedCall;
    g_stub = stub;
    Log("[autosave] level-up (exe+0x%llX) and scripted (exe+0x%llX) autosaves (exe+0x%llX) follow the menu's four autosave "
        "options (exe+0x%llX, 0x%llX, 0x%llX, 0x%llX): fix installed",
        static_cast<unsigned long long>(levelUpCall - game.base), static_cast<unsigned long long>(scriptedCall - game.base),
        static_cast<unsigned long long>(autosave - game.base), static_cast<unsigned long long>(switches[0] - game.base),
        static_cast<unsigned long long>(switches[1] - game.base), static_cast<unsigned long long>(switches[2] - game.base),
        static_cast<unsigned long long>(switches[3] - game.base));
    return true;
}

void SetAutosaveFollowsMenuEnabled(bool enabled) { g_enabled = enabled; }

void LogAutosaveFollowsMenuStats()
{
    const uint64_t skipped = g_skippedLevelUp.load() + g_skippedScripted.load();
    if (!g_stub || skipped == g_skippedLogged) return;
    Log("[autosave] autosaves not made with every menu option off: %llu after a level-up, %llu scripted (%llu made)",
        static_cast<unsigned long long>(g_skippedLevelUp.load()), static_cast<unsigned long long>(g_skippedScripted.load()),
        static_cast<unsigned long long>(g_made.load()));
    g_skippedLogged = skipped;
}

bool AutosaveFollowsMenuInfo(AutosaveMenuInfo* out)
{
    if (!g_stub) return false;
    out->levelUpCall = g_levelUpCall;
    out->scriptedCall = g_scriptedCall;
    out->stub = g_stub;
    out->autosave = reinterpret_cast<void*>(g_autosave);
    memcpy(out->switches, g_switches, sizeof g_switches);
    out->levelUpHook = reinterpret_cast<void*>(&LevelUpHook);
    out->scriptedHook = reinterpret_cast<void*>(&ScriptedHook);
    out->skippedLevelUp = g_skippedLevelUp.load();
    out->skippedScripted = g_skippedScripted.load();
    out->made = g_made.load();
    return true;
}

void AutosaveFollowsMenuTestSetup(void* fakeAutosave, const uint8_t* const* fakeSwitches)
{
    static AutosaveFn realAutosave = nullptr;
    static const uint8_t* realSwitches[4] = {};
    if (fakeAutosave) {
        realAutosave = g_autosave;
        memcpy(realSwitches, g_switches, sizeof g_switches);
        g_autosave = reinterpret_cast<AutosaveFn>(fakeAutosave);
        memcpy(g_switches, fakeSwitches, sizeof g_switches);
    } else {
        g_autosave = realAutosave;
        memcpy(g_switches, realSwitches, sizeof g_switches);
    }
}

}  // namespace opf
