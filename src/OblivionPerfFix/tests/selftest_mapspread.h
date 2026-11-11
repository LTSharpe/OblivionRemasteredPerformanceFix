// MapPopupSpread self-test (worker mapspread 2026-11-02; public since 1.3.0, port130k), included by selftest.cpp: every site the
// key needs is found by pattern on the mapped exe (the three classes' native tables by name, the map's functions through their
// exec thunks, the hover finder, FMemory::Free, the copy constructor, the rehash, ProcessEvent's slot, FName's constructor; no
// writes) and, on build 19115871, at the addresses the lab key used (rev130f's derivation table; the icon filter's and icon's
// native tables, which the lab key had wrong); and the keep / drop / flush / reuse state machine with fakes for the game's
// functions (map_spread.cpp's MapSpreadTestLogic: a kept list is dropped by a clear, runs with its size pass in stock order and
// with the zoom of then at the pairing end or before a reader, is never kept outside the pairing run or in mode 0, a second list
// first runs the first; reuse writes nothing unless every copy matches, reuses through the hooks, never across map kinds
// (M-S1), not after a non-quiet event; public values); the wrapped native set (mapshrink: all but 37 audited map widget
// natives on the audited table, every entry on any other); the code sites (mapfix: 11, none on UpdateOrCreateMapIcons' exec
// thunk).
#pragma once

static void MapSpreadRun(const GameModule& game)
{
    MapSpreadFound f{};
    const char* const missing = MapSpreadTestDiscover(game, &f);
    Check(missing == nullptr, "MapPopupSpread: every site found by pattern on the mapped exe");
    if (missing) printf("    not found: %s\n", missing);
    if (!missing && game.timeDateStamp == 0xF19077A4u) {
        const auto rva = [&](const uint8_t* p) { return static_cast<uint32_t>(p - game.base); };
        Check(rva(f.loop) == 0x493F910 && rva(f.clear) == 0x49136A0 && rva(f.size) == 0x493E820 && rva(f.hover) == 0x491CE60 &&
                  rva(f.copy) == 0x490C1B0 && rva(f.rehash) == 0x4929730 && rva(f.free) == 0xE26500 &&
                  rva(f.thunkReturn) == 0x46A79B7 && (f.peSlot == 0x268 || f.peSlot == 0),
              "MapPopupSpread: build 19115871's functions (loop exe+0x493F910, clear exe+0x49136A0, ProcessEvent slot +0x268)");
        Check(f.tableCounts[0] == 65 && f.tableCounts[1] == 10 && f.tableCounts[2] == 8 && rva(f.filterTable) == 0x7CD2B90 &&
                  rva(f.iconTable) == 0x7CCDB50,
              "MapPopupSpread: native tables 65 + 10 + 8 entries (filter exe+0x7CD2B90, icon exe+0x7CCDB50: not the link info)");
        printf("      native tables %d + %d + %d\n", f.tableCounts[0], f.tableCounts[1], f.tableCounts[2]);
        Check(f.mapAudited, "MapPopupSpread: build 19115871's map widget table is the audited one (mapshrink)");
    }
    if (!missing) {
        // mapshrink (2026-11-11): the wrapped set, pinned by its own copy of the audit here, so that an edit of the list in
        // map_spread.cpp cannot widen or narrow it unnoticed. Left as the game has them: these 37 map widget natives
        // (getters, the local map's loads / unloads, pure coordinate helpers); every other entry of the three tables wrapped.
        static const char* const kLeft[] = {
            "AbsoluteSpaceToMapSpace", "ClampZoomValue", "GetBaseColorRenderTarget", "GetCurrentZoomValue", "GetDPIScaling",
            "GetFogMaterial", "GetFogRenderTarget", "GetIsPaintSpaceGeometryValid", "GetLocalMapMaterial",
            "GetLocalMapSobelMaterial", "GetOriginalMapSize", "GetResolution", "GetSceneDepthRenderTarget",
            "GetSecondPassRenderTarget", "GetSmallestPossibleZoomValue", "GetViewSize", "GetZoomedMapSize",
            "IsAskingForPlayerMarker", "LoadBaseColorRenderTarget", "LoadFogMaterial", "LoadFogRenderTarget",
            "LoadLocalMapMaterial", "LoadLocalMapSobelMaterial", "LoadSceneDepthRenderTarget", "LoadSecondPassRenderTarget",
            "LocalMapToOblivionWorld", "OTAMapCoord", "RemapCircleToSquare", "UnloadBaseColorRenderTarget", "UnloadFogMaterial",
            "UnloadFogRenderTarget", "UnloadLocalMapMaterial", "UnloadLocalMapSobelMaterial", "UnloadSceneDepthRenderTarget",
            "UnloadSecondPassRenderTarget", "ViewSpaceToAbsoluteSpace", "ViewSpaceToMapSpace"};
        constexpr int kNumLeft = sizeof kLeft / sizeof kLeft[0];
        int leftFound = 0, mismatch = 0, wrappedN[3] = {};
        for (int t = 0; t < 3; ++t)
            for (int i = 0; i < f.tableCounts[t]; ++i) wrappedN[t] += f.wrapped[t][i];
        if (f.mapAudited) {
            for (int i = 0; i < f.tableCounts[0]; ++i) {
                const char* const name = *reinterpret_cast<const char* const*>(f.mapTable + i * 16);
                bool left = false;
                for (const char* l : kLeft) left |= strcmp(l, name) == 0;
                leftFound += left;
                if (f.wrapped[0][i] == left) {
                    ++mismatch;
                    printf("    %s: %s\n", name, left ? "wrapped, expected left as is" : "left as is, expected wrapped");
                }
            }
            Check(kNumLeft == 37 && leftFound == kNumLeft && mismatch == 0 && wrappedN[0] == f.tableCounts[0] - kNumLeft,
                  "MapPopupSpread: map widget natives wrapped = all but the 37 audited ones (mapshrink)");
        } else {
            Check(wrappedN[0] == f.tableCounts[0], "MapPopupSpread: a map widget table other than the audited one wraps every entry");
        }
        Check(wrappedN[1] == f.tableCounts[1] && wrappedN[2] == f.tableCounts[2],
              "MapPopupSpread: every icon filter and icon widget native wrapped");
        printf("      wrapped %d + %d + %d of %d + %d + %d\n", wrappedN[0], wrappedN[1], wrappedN[2], f.tableCounts[0],
               f.tableCounts[1], f.tableCounts[2]);
        // mapfix (2026-11-11): 9 inline E9 patches (the 3 detours + 6 readers) and 2 call sites; none on UpdateOrCreateMapIcons'
        // exec thunk (its table stub is always in front of it), and no two patches at one place
        bool thunkFree = f.iconsThunk != nullptr, distinct = true;
        for (int i = 0; i < f.inlinePatches; ++i) {
            thunkFree &= f.inlineAt[i] != nullptr && f.inlineAt[i] != f.iconsThunk;
            for (int j = 0; j < i; ++j) distinct &= f.inlineAt[i] != f.inlineAt[j];
        }
        Check(f.inlinePatches == 9 && f.codeSites == 11 && thunkFree && distinct,
              "MapPopupSpread: 11 code sites (9 inline E9 + 2 calls), none on UpdateOrCreateMapIcons' exec thunk (mapfix)");
        printf("      code sites %d (inline %d)\n", f.codeSites, f.inlinePatches);
    }
    const int fail = MapSpreadTestLogic();
    Check(fail == 0, "MapPopupSpread: keep / drop / flush / reuse logic with fakes (incl. M-S1 map kind; MS-S1: the filter's displayed-icon count is no guard)");
    if (fail) printf("    failed checks (bits): 0x%X\n", fail);
}
