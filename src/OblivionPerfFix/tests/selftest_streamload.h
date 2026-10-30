// StreamBehindLoadingScreen self-test, included by selftest.cpp: its sites on the mapped exe (locate only: the detour's
// work calls game code on the game thread, so nothing runs offline). Checks named "at exe+0x..." pin build 19115871.
#pragma once

namespace sbl {
void Run(const GameModule& game)
{
    StreamBehindLoadingInfo i{};
    Check(FindStreamBehindLoading(&i), "StreamBehindLoadingScreen: fade gate, gate-arming function, pool store, cvar and manager found");
    if (!i.gateBody) return;
    auto rva = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
    Check(rva(i.gateBody) == 0x4878670, "StreamBehindLoadingScreen: fade gate at exe+0x4878670");
    Check(i.arm && rva(i.arm) == 0x4882730, "StreamBehindLoadingScreen: gate-arming function at exe+0x4882730");
    Check(i.platformGet != nullptr, "StreamBehindLoadingScreen: the save system's getter found in the settings save's writer (save loads recognised by their save read)");
    Check(rva(i.travelPending) == 0x3864220 && rva(i.asyncLoading) == 0x8DAA2E8 && i.getWorldSlot == 0x188,
          "StreamBehindLoadingScreen: travel query exe+0x3864220, async-loading pointer exe+0x8DAA2E8, GetWorld +0x188");
    Check(rva(i.cvarStatic) == 0x9306EA8 && i.poolOffset == 0x234,
          "StreamBehindLoadingScreen: cached r.Streaming.PoolSize at exe+0x9306EA8, stored pool size at transition+0x234");
    Check(rva(i.singleton) == 0x924C970 && rva(i.update) == 0x30D9650,
          "StreamBehindLoadingScreen: streaming manager at exe+0x924C970, its update at exe+0x30D9650");
    // the world drawing for UnfreezeBehindLoadingScreen
    Check(i.execSlot && i.execIfSafeSlot == i.execSlot + 1 && i.gameViewport && i.drawFlagOffset,
          "UnfreezeBehindLoadingScreen: gate delegate slots, viewport getter and world drawing switch found");
    if (!i.execSlot) return;
    Check(rva(i.execSlot) == 0x7D7BDD0 && rva(i.gameViewport) == 0x385EE90 && i.drawFlagOffset == 0x6C,
          "UnfreezeBehindLoadingScreen: gate Execute slot at exe+0x7D7BDD0, GetGameViewport exe+0x385EE90, bDisableWorldRendering at "
          "viewport+0x6C");
    Check(i.setLoadingFreeze && rva(i.setLoadingFreeze) == 0x4887110,
          "UnfreezeBehindLoadingScreen: SetLoadingFreeze found (flags and calls as the unfreeze timer) at exe+0x4887110");
    // UnfreezeBehindLoadingScreenFastTravel: the map's fast-travel request thunk, by its registered name
    uint8_t* const ft = FindExecByName("DelayFastTravelRequestUntilFadeEnd",
        "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 33 FF 48 8B DA 89 7C 24 38 48 8B F1 E8 ?? ?? ?? ?? 48 8B CB 48 39 7B 20 "
        "74 10 48 8B 53 18 4C 8D 44 24 38 E8 ?? ?? ?? ?? EB 1C 4C 8B 83 88 00 00 00 48 8D 54 24 38 49 8B 40 18 48 89 83 88 00 00 "
        "00 E8 ?? ?? ?? ?? 48 8B 43 20 48 8B CE 8B 54 24 38 48 85 C0 40 0F 95 C7 48 03 F8 48 89 7B 20 E8 ?? ?? ?? ?? 48 8B 5C 24 30");
    Check(ft && rva(ft) == 0x468ACF0 && rva(RipTarget(ft + 0x6E, 1, 5)) == 0x4918830,
          "UnfreezeBehindLoadingScreenFastTravel: DelayFastTravelRequestUntilFadeEnd's thunk at exe+0x468ACF0, function exe+0x4918830");
}
}  // namespace sbl
