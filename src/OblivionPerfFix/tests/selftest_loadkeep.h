// MenuLoadKeep self-test, included by selftest.cpp: FSoftObjectPath::TryLoad is found by its pattern at the known place on the
// mapped exe (build 19115871), its prologue is relocatable, and the class names / root / cluster / cap / unroot logic works on a
// fake GUObjectArray. Nothing is installed.
#pragma once

static void LoadKeepRun(const GameModule& game)
{
    if (game.timeDateStamp == 0xF19077A4u) {
        const uint8_t* const fn = MenuLoadKeepFindTryLoad(game);
        Check(fn != nullptr && fn == game.base + 0x10F8A90, "MenuLoadKeep: FSoftObjectPath::TryLoad is found once, at exe+0x10F8A90");
        // the relocated prologue holds no rip-relative instruction: mov [rsp+10h],rbx; push rdi
        static const uint8_t kPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57};
        Check(fn && memcmp(fn, kPrologue, sizeof kPrologue) == 0, "MenuLoadKeep: the six bytes copied into the detour slot are the plain prologue");
        const uint8_t* const call = MenuLoadKeepFindLoopCall(game);
        Check(call == game.base + 0x491EE04 && call[0] == 0xE8,
              "MenuLoadKeep: the input-context loop's LoadSynchronous call is found once, at exe+0x491EE04 (only its loads keep IMCs)");
    }
    const int fail = MenuLoadKeepTestLogic();
    Check(fail == 0, "MenuLoadKeep: class names, root / cluster root / skip / cap / unroot logic with a fake GUObjectArray");
    if (fail) printf("    failed checks (bits): 0x%X\n", fail);
}
