// MapPopupSpread self-test (worker mapspread 2026-11-02; public since 1.3.0, port130k), included by selftest.cpp: every site the
// key needs is found by pattern on the mapped exe (the three classes' native tables by name, the map's functions through their
// exec thunks, the hover finder, FMemory::Free, the copy constructor, the rehash, ProcessEvent's slot, FName's constructor; no
// writes) and, on build 19115871, at the addresses the lab key used (rev130f's derivation table; the icon filter's and icon's
// native tables, which the lab key had wrong); and the keep / drop / flush / reuse state machine with fakes for the game's
// functions (map_spread.cpp's MapSpreadTestLogic: a kept list is dropped by a clear, runs with its size pass in stock order and
// with the zoom of then at the pairing end or before a reader, is never kept outside the pairing run or in mode 0, a second list
// first runs the first; reuse writes nothing unless every copy matches, reuses through the hooks, never across map kinds
// (M-S1), not after a non-quiet event; public values).
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
    }
    const int fail = MapSpreadTestLogic();
    Check(fail == 0, "MapPopupSpread: keep / drop / flush / reuse logic with fakes (incl. M-S1 map kind; MS-S1: the filter's displayed-icon count is no guard)");
    if (fail) printf("    failed checks (bits): 0x%X\n", fail);
}
