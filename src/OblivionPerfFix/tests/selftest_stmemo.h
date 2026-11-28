// StringTableMemo self-test, included by selftest.cpp: FStringTableEngineBridge::GetAssetReference is found by its pattern at
// the known place on the mapped exe (build 19115871), its first instruction is the 5-byte relocatable mov, all its direct
// callers are the string table bridge's, and the remember / not-an-asset / sub-path / full-table / verify logic works with a
// fake GetAssetReference. Nothing is installed.
#pragma once

static void StringTableMemoRun(const GameModule& game)
{
    const uint8_t* const fn = StringTableMemoFind(game);
    if (game.timeDateStamp == 0xF19077A4u) {
        Check(fn != nullptr && fn == game.base + 0x329AEC0, "StringTableMemo: GetAssetReference is found once, at exe+0x329AEC0");
        static const uint8_t kFirst[] = {0x48, 0x89, 0x5C, 0x24, 0x08};  // mov [rsp+8],rbx: copied into the trampoline
        Check(fn && memcmp(fn, kFirst, sizeof kFirst) == 0, "StringTableMemo: the 5 bytes moved into the trampoline are mov [rsp+8],rbx");
        Check(fn && (reinterpret_cast<uintptr_t>(fn) & 7) == 0, "StringTableMemo: the entry jump fits one aligned qword (atomic write)");
        // its direct calls on this build: the five string table bridge functions (Collect, FullyLoad, IsFromAsset, Load, Redirect)
        int calls = 0;
        for (uint8_t* p = game.text.begin; fn && p + 5 <= game.text.end; ++p) {
            if (*p != 0xE8) continue;
            int32_t rel;
            memcpy(&rel, p + 1, 4);
            if (p + 5 + rel == fn) ++calls;
        }
        Check(calls == 5, "StringTableMemo: GetAssetReference has its 5 known direct callers");
    } else {
        Check(true, "StringTableMemo: unknown build, pattern search only");
    }
    const int fail = StringTableMemoTestLogic();
    Check(fail == 0, "StringTableMemo: remember / not-an-asset / sub-path / None / full table / verify logic with a fake GetAssetReference");
    if (fail) printf("    failed checks (bits): 0x%X\n", fail);
}
