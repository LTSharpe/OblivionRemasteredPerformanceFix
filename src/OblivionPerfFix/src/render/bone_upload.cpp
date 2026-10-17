// BoneUploadStores: GPU-skinning bone upload (ISPC UpdateBoneData_CopyBones) writes each 3x4 bone
// matrix as a 32-byte store plus a *masked* 32-byte store whose mask is the constant
// {-1,-1,-1,-1,0,0,0,0}. The destination is a write-combined upload buffer, where masked
// stores defeat write combining and are extremely slow (up to a third of the render thread).
// A masked store with that constant mask writes exactly the low 16 bytes, so we replace it
// with a plain 16-byte store: bit-identical output.
//
// Each site is found by its loop body (the store itself is a wildcard so we can recognise
// an already-patched site), and the mask is verified before patching:
//  - VEX vmaskmovps: the mask register's load is found just before the loop and the
//    32-byte constant it loads must be {-1 x4, 0 x4};
//  - EVEX vmovups {k1}: k1 comes from r10d, which must be set by `mov r10b, 0Fh` (4 lanes).
#include "common.h"

#include <array>
#include <cstring>

namespace opf {
namespace {

enum class MaskKind { RipConstant, KmaskR10b0F };

struct StoreSite {
    const char* name;
    const char* body;         // loop body; the masked store is the first ?? run
    const char* maskSetup;    // searched for within kMaskSearch bytes before the body
    size_t maskDispOffset;    // RipConstant: offset of disp32 in the mask-setup instruction
    size_t maskInsnLen;       // RipConstant: length of the mask-setup instruction
    MaskKind kind;
    const char* from;         // original masked store
    const char* to;           // plain 16-byte store (+ nop padding)
};

constexpr size_t kMaskSearch = 40;

// Bytes from game build 19115871 (exe+0x5C7310..0x5C78A7); see docs/research.md, "Bone upload".
const StoreSite kSites[] = {
    // AVX2 uniform (exe+0x5C75CB) and the all-lanes-on path of the AVX-512-family varying variant
    // (exe+0x5C74CB): vmaskmovps [rcx], ymm3, ymm4 -> vmovups [rcx], xmm4
    {"avx2 32+16", "43 0F B7 04 50 48 C1 E0 06 C5 FC 10 64 02 20 C4 E2 7D 16 2C 02 C5 F8 10 34 02 C5 C8 15 74 02 10 "
                   "C4 E2 75 16 FC C4 E3 55 0C EF CC C5 FC 11 69 E0 C4 E2 6D 16 E4 C4 E3 59 0C E6 03 ?? ?? ?? ?? ?? "
                   "49 FF C2 48 83 C1 30 4D 39 D1 75",
     "C5 FC 28 1D", 4, 8, MaskKind::RipConstant,  // vmovaps ymm3, [rip+disp32]
     "C4 E2 65 2E 21", "C5 F8 11 21 90"},
    // AVX uniform (exe+0x5C7896) and the all-lanes-on path of the AVX2 varying variant
    // (exe+0x5C77C6): vmaskmovps [rcx], ymm0, ymm1 -> vmovups [rcx], xmm1
    {"avx 16+16+16", "43 0F B7 04 50 48 C1 E0 06 C5 F8 10 0C 02 C5 F8 10 54 02 10 C5 F8 10 5C 02 20 C5 F8 10 64 02 30 "
                     "C4 E3 75 18 EA 01 C4 E3 65 18 F4 01 C4 E3 5D 18 FB 01 C4 E3 7D 04 FF 14 C5 FF 12 F6 C4 E3 4D 0C F7 48 "
                     "C4 E3 6D 18 F9 01 C4 E3 7D 04 FF E1 C4 E3 55 0C EF 12 C4 E3 55 0C EE CC C4 E3 7D 19 69 F0 01 "
                     "C5 F8 11 69 E0 C5 F0 15 CA C4 E3 61 21 D4 B3 C4 E3 69 0C C9 03 ?? ?? ?? ?? ?? "
                     "49 FF C2 48 83 C1 30 4D 39 D1 0F 85",
     "C5 FC 28 05", 4, 8, MaskKind::RipConstant,  // vmovaps ymm0, [rip+disp32]
     "C4 E2 7D 2E 09", "C5 F8 11 09 90"},
    // AVX-512 uniform (exe+0x5C736E): vmovups [rcx]{k1}, ymm2 with k1 = 0x0F -> vmovups [rcx], xmm2
    {"avx512 k-mask", "43 0F B7 04 58 48 C1 E0 06 C5 FC 10 14 02 C5 FC 10 5C 02 20 C5 FC 28 E2 62 F2 7D 28 7F E3 "
                      "C5 FC 11 61 E0 62 F2 75 28 7F D3 C4 C1 7B 92 CA ?? ?? ?? ?? ?? ?? 49 FF C3 48 83 C1 30 4D 39 D9 75",
     "41 B2 0F", 0, 0, MaskKind::KmaskR10b0F,  // mov r10b, 0Fh
     "62 F1 7C 29 11 11", "C5 F8 11 11 66 90"},
};

bool MaskIsLowHalf(const uint8_t* constant)
{
    for (int i = 0; i < 16; ++i)
        if (constant[i] != 0xFF || constant[16 + i] != 0x00) return false;
    return true;
}

// Finds the mask setup instruction in the bytes just before the loop body.
const uint8_t* FindMaskSetup(const uint8_t* body, const Pattern& setup)
{
    for (const uint8_t* at = body - setup.size(); at >= body - kMaskSearch; --at)
        if (MatchesAt(at, setup)) return at;
    return nullptr;
}

}  // namespace

bool ApplyBoneUploadFix()
{
    const GameModule& game = Game();
    int patched = 0, already = 0, rejected = 0;
    for (const StoreSite& site : kSites) {
        const Pattern body = ParsePattern(site.body);
        const Pattern setup = ParsePattern(site.maskSetup);
        const Pattern from = ParsePattern(site.from);
        const Pattern to = ParsePattern(site.to);
        size_t storeOffset = 0;
        while (body.care[storeOffset]) ++storeOffset;

        const auto hits = FindAll(game.text, body);
        Log("[bone] %-14s: %zu loop(s) found", site.name, hits.size());
        for (uint8_t* loop : hits) {
            uint8_t* store = loop + storeOffset;
            const auto rva = static_cast<unsigned>(store - game.base);
            if (MatchesAt(store, to)) { ++already; Log("[bone]   exe+0x%X already patched", rva); continue; }
            if (!MatchesAt(store, from)) { ++rejected; Log("[bone]   exe+0x%X: unexpected store bytes, skipped", rva); continue; }

            const uint8_t* mask = FindMaskSetup(loop, setup);
            bool maskOk = mask != nullptr;
            if (maskOk && site.kind == MaskKind::RipConstant) {
                const uint8_t* constant = RipTarget(mask, site.maskDispOffset, site.maskInsnLen);
                maskOk = game.rdata.Contains(constant) && MaskIsLowHalf(constant);
            }
            if (!maskOk) { ++rejected; Log("[bone]   exe+0x%X: mask not verified, skipped", rva); continue; }

            if (WriteCode(store, to.bytes)) { ++patched; Log("[bone]   exe+0x%X patched", rva); }
            else { ++rejected; Log("[bone]   exe+0x%X: VirtualProtect failed", rva); }
        }
    }
    Log("[bone] done: %d patched, %d already patched, %d skipped", patched, already, rejected);
    return patched + already > 0;
}

}  // namespace opf
