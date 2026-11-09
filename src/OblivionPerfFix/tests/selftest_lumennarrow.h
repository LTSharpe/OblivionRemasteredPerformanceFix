// LumenCardsNarrow self-test, included by selftest.cpp: its sites and offsets on the mapped exe (nothing installed; the expected
// values for build 19115871), refusal on any changed byte and the patch (on a private copy), the stubs executed and their unwind
// data, the decision with fakes for each case (render/lumen_cards_narrow.cpp). Lab builds also run the proof (value 2) cases.
#pragma once

static void LumenNarrowRun(const GameModule& game)
{
    uint32_t r[4] = {}, o[3] = {};
    const int ok = LumenNarrowTestDiscover(game, r, o);
    Check(ok == 1, "LumenCardsNarrow: every site, offset and the group layout found by pattern on the mapped exe");
    if (game.timeDateStamp == 0xF19077A4u) {
        const bool want = ok == 1 && r[0] == 0x1F427B5 && r[1] == 0x1F428C2 && r[2] == 0x1F428CC && r[3] == 0x91832C8 && o[0] == 0x6A0 && o[1] == 0x6A8 && o[2] == 0x118;
        Check(want, "LumenCardsNarrow: every site and offset match build 19115871's");
        if (!want) printf("    found %d: site 0x%X exit 0x%X upload call 0x%X every-frame 0x%X; buffer 0x%X upload 0x%X indices 0x%X\n", ok, r[0], r[1], r[2], r[3], o[0], o[1], o[2]);
    }
    if (ok == 1) {
        const int m = LumenNarrowTestMismatch(game);
        Check(m == 0, "LumenCardsNarrow: a changed byte at any checked place refuses; the patch writes only the two sites");
        if (m) printf("    failed checks (bits): 0x%X\n", m);
        uint32_t resetRva = 0;
        const int rs = LumenNarrowTestResetSite(game, &resetRva);
        Check(rs == 0 && (game.timeDateStamp != 0xF19077A4u || resetRva == 0x1FE49D3),
              "LumenCardsNarrow: the lists' reset is found once (exe+0x1FE49D3 on 19115871), refuses a changed byte or other list offsets, patch on a copy");
        if (rs) printf("    failed checks (bits): 0x%X (reset at 0x%X)\n", rs, resetRva);
        const int uw = LumenNarrowTestUnwind(game);
        Check(uw == 0, "LumenCardsNarrow: a stack walk from inside the stub arrives at the game function's caller");
        if (uw) printf("    failed checks (bits): 0x%X\n", uw);
    }
    const int s = LumenNarrowTestStub();
    Check(s == 0, "LumenCardsNarrow: both stubs pass the registers, run the displaced instruction, keep its flags, go to the right exit");
    if (s) printf("    failed checks (bits): 0x%X\n", s);
    const int l = LumenNarrowTestLogic();
    Check(l == 0, "LumenCardsNarrow: the decision through the real stubs against a separate stock model (narrow, moved, range taken, count, not queued, 0 queued, lost queue, listed after upload, buffer, size, scene, every-frame, large list, anomaly, bad range, upload, capacity, toggle, thread, table; lab: proof, proof on a stock call)");
    if (l) printf("    failed checks (bits): 0x%X\n", l);
}
