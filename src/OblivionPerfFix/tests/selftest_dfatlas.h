// DfAtlasKeep self-test, included by selftest.cpp: FDistanceFieldSceneData::ResizeBrickAtlasIfNeeded is found by its pattern
// at the known place on the mapped exe (build 19115871), with the globals it reads in the data section and its own
// RegisterExternalTexture call; the search fails closed on a changed copy; the game's depth arithmetic; the keep decision; the
// hook's paths with fakes. Nothing is installed.
#pragma once

static void DfAtlasRun(const GameModule& game)
{
    DfAtlasSites s;
    const bool found = DfAtlasKeepFind(game, &s);
    if (game.timeDateStamp == 0xF19077A4u) {
        Check(found && s.fn == game.base + 0x1DA0EF0, "DfAtlasKeep: ResizeBrickAtlasIfNeeded found once, at exe+0x1DA0EF0");
        Check(found && s.registerTexture == game.base + 0x26C30E0, "DfAtlasKeep: its no-resize call is RegisterExternalTexture exe+0x26C30E0");
        static const uint8_t kFirst[] = {0x48, 0x89, 0x5C, 0x24, 0x20};  // mov [rsp+20h],rbx: copied into the trampoline
        Check(found && memcmp(s.fn, kFirst, sizeof kFirst) == 0, "DfAtlasKeep: the 5 bytes moved into the trampoline are mov [rsp+20h],rbx");
        Check(found && (reinterpret_cast<uintptr_t>(s.fn) & 7) == 0, "DfAtlasKeep: the entry jump fits one aligned qword (atomic write)");
        int calls = 0;
        for (uint8_t* p = game.text.begin; found && p + 5 <= game.text.end; ++p) {
            if (*p != 0xE8) continue;
            int32_t rel;
            memcpy(&rel, p + 1, 4);
            if (p + 5 + rel == s.fn) ++calls;
        }
        Check(calls == 1, "DfAtlasKeep: ResizeBrickAtlasIfNeeded has its one known direct caller (the atlas update)");
    } else {
        Check(true, "DfAtlasKeep: unknown build, pattern search only");
    }
    // fail closed: a copy of the function in a fake module is not taken (its globals are not in a data section), nor a copy with
    // one changed byte (the allocator's MaxSize offset 0x3D0 -> 0x3D4)
    if (found) {
        static uint8_t copy[0x800];
        memcpy(copy, s.fn, sizeof copy);
        GameModule fake{};
        fake.base = copy;
        fake.text = Section{copy, copy + sizeof copy};
        fake.data = Section{nullptr, nullptr};
        DfAtlasSites t;
        Check(!DfAtlasKeepFind(fake, &t), "DfAtlasKeep: a copy whose globals are outside the data section is refused");
        copy[0x5C] = 0xD4;
        fake.data = game.data;
        Check(!DfAtlasKeepFind(fake, &t), "DfAtlasKeep: a copy with a changed field offset is not found (fails closed)");
        DfAtlasKeepFind(game, &t);  // the real sites back for the logic test's save / restore
    }
    // the game's depth: ceil(MaxSize x 16 / 128^2) bricks, power of two up to 32, then multiples of 4
    Check(DfAtlasGameZ(0, 16, 128, 32) == 1 && DfAtlasGameZ(1, 16, 128, 32) == 1 && DfAtlasGameZ(1024, 16, 128, 32) == 1,
          "DfAtlasKeep: the game's depth for an empty / tiny allocator is 1 brick");
    Check(DfAtlasGameZ(1025, 16, 128, 32) == 2 && DfAtlasGameZ(3 * 1024, 16, 128, 32) == 4 && DfAtlasGameZ(5 * 1024, 16, 128, 32) == 8,
          "DfAtlasKeep: the game's depth rounds up to a power of two");
    Check(DfAtlasGameZ(32 * 1024, 16, 128, 32) == 32 && DfAtlasGameZ(33 * 1024, 16, 128, 32) == 36 && DfAtlasGameZ(37 * 1024, 16, 128, 32) == 40,
          "DfAtlasKeep: above BrickAtlasMaxSizeZ the depth is a multiple of 4");
    // the decision
    const int32_t e = 128 * 8;
    Check(DfAtlasKeepDecide(8, 16, 128, 128, 128, 32, e, e, 128, 0), "DfAtlasKeep: atlas 16, the game wants 8: kept");
    Check(!DfAtlasKeepDecide(16, 16, 128, 128, 128, 32, e, e, 128, 0) && !DfAtlasKeepDecide(32, 16, 128, 128, 128, 32, e, e, 128, 0),
          "DfAtlasKeep: same size or larger: the game's way");
    Check(!DfAtlasKeepDecide(8, 64, 128, 128, 128, 32, e, e, 512, 0) && !DfAtlasKeepDecide(8, 36, 128, 128, 128, 64, e, e, 288, 0),
          "DfAtlasKeep: above BrickAtlasMaxSizeZ or not a power of two: shrinks as in the game");
    Check(!DfAtlasKeepDecide(8, 16, 128, 128, 128, 32, e, e, 64, 0) && !DfAtlasKeepDecide(8, 16, 64, 128, 128, 32, e, e, 128, 0) &&
              !DfAtlasKeepDecide(8, 16, 128, 128, 128, 32, e, 512, 128, 0),
          "DfAtlasKeep: texture size not matching the recorded dimensions: the game's way");
    Check(!DfAtlasKeepDecide(8, 16, 128, 128, 128, 32, e, e, 128, 1), "DfAtlasKeep: r.DistanceFields.ResizeAtlasEveryFrame on: the game's way");
    const int fail = DfAtlasKeepTestLogic();
    Check(fail == 0, "DfAtlasKeep: the hook keeps / grows / passes through with a fake atlas, function and RegisterExternalTexture");
    if (fail) printf("    failed checks (bits): 0x%X\n", fail);
}
