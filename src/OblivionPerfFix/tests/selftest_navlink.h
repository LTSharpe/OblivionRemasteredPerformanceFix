// NavLinkFast self-test (worker navstream), included by selftest.cpp: findConnectingPolys' call in connectExtLinks
// (exe+0x417129C) and addTile's 4 connectExtLinks calls redirected on the mapped exe; the fast search against the game's
// own search on random fake tiles, entry for entry; the skip rule against a brute-force run of every search the stock
// call would make (lab builds also: mode 2's own comparison).
#pragma once

namespace nlf {

inline void NavLinkSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    Check(!InstallNavLinkFast(false, 3), "navlink: refuses to install after game code started");
    if (Installed(InstallNavLinkFast(true, 3), "navlink: findConnectingPolys / connectExtLinks / addTile found")) {
        Check(NavLinkFastTestSite(-1) == game.base + 0x417129C && game.base[0x417129C] == 0xE8 &&
                  RipTarget(game.base + 0x417129C, 1, 5) != game.base + 0x4174C30,
              "navlink: connectExtLinks exe+0x4171090 now calls the stub instead of findConnectingPolys exe+0x4174C30");
        Check(NavLinkFastTestSite(0) == game.base + 0x416ED1F && NavLinkFastTestSite(1) == game.base + 0x416ED39 &&
                  NavLinkFastTestSite(2) == game.base + 0x416EEB3 && NavLinkFastTestSite(3) == game.base + 0x416EEC9,
              "navlink: addTile exe+0x416E640's 4 connectExtLinks calls redirected");
        // a fake dtNavMesh: walkableClimb +0x10, m_tiles +0xE8, m_tileBits +0xF4, m_polyBits +0xF8; tiles of 0xA8 bytes
        struct TileData {
            std::vector<uint8_t> header = std::vector<uint8_t>(0x58), polys, detail, tris;
            std::vector<double> verts;
        };
        uint32_t rng = 12345;
        auto rnd = [&](uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; };
        auto make = [&](TileData& t, uint8_t* tile, uint32_t salt, int polyCount, uint32_t sideMask = 0xFF) {
            const int vertCount = 40;
            t.verts.assign(vertCount * 3, 0.0);
            for (int v = 0; v < vertCount; ++v) {
                const double xs[4] = {10.0, 10.0, 10.004, 9.0};
                t.verts[v * 3 + 0] = rnd(3) ? xs[rnd(4)] : static_cast<double>(rnd(12));
                t.verts[v * 3 + 1] = static_cast<double>(rnd(4)) * 0.4;
                t.verts[v * 3 + 2] = rnd(3) ? xs[rnd(4)] : static_cast<double>(rnd(12)) * 0.7;
            }
            t.polys.assign(static_cast<size_t>(polyCount) * 0x20, 0);
            t.detail.assign(static_cast<size_t>(polyCount) * 6, 0);
            t.tris.assign(static_cast<size_t>(polyCount) * 4, 0);
            for (int i = 0; i < polyCount; ++i) {
                uint8_t* p = &t.polys[static_cast<size_t>(i) * 0x20];
                const int nv = 3 + static_cast<int>(rnd(4));
                for (int j = 0; j < nv; ++j) {
                    const uint16_t vi = static_cast<uint16_t>(rnd(vertCount));
                    const uint32_t k = rnd(10);
                    uint32_t sd = rnd(8);
                    while (sideMask && !(sideMask & (1u << sd))) sd = (sd + 1) & 7;
                    const uint16_t nei = static_cast<uint16_t>(k < 5 && sideMask ? 0x8000u | sd : k < 7 ? 0 : 1 + rnd(polyCount));
                    memcpy(p + 4 + 2 * j, &vi, 2);
                    memcpy(p + 0x10 + 2 * j, &nei, 2);
                }
                p[0x1E] = static_cast<uint8_t>(nv);
                const uint16_t triBase = static_cast<uint16_t>(i);
                memcpy(&t.detail[static_cast<size_t>(i) * 6 + 2], &triBase, 2);
                t.detail[static_cast<size_t>(i) * 6 + 5] = 1;  // one detail triangle: the polygon's vertices 0, 1, 2
                t.tris[static_cast<size_t>(i) * 4 + 1] = 1;
                t.tris[static_cast<size_t>(i) * 4 + 2] = 2;
            }
            const uint16_t pc = static_cast<uint16_t>(polyCount), vc = vertCount, dmc = static_cast<uint16_t>(polyCount - 2);
            memcpy(&t.header[4], &pc, 2);
            memcpy(&t.header[6], &vc, 2);
            memcpy(&t.header[0x12], &dmc, 2);  // the last two polygons without a detail mesh (as off-mesh polygons)
            memset(tile, 0, 0xA8);
            memcpy(tile, &salt, 4);
            void* ptrs[7] = {t.header.data(), t.polys.data(), t.verts.data(), nullptr, t.detail.data(), t.verts.data(), t.tris.data()};
            memcpy(tile + 8, ptrs, sizeof ptrs);
        };
        alignas(16) uint8_t nav[0x100] = {};
        alignas(16) uint8_t tiles[3 * 0xA8] = {};
        const double climb = 0.45;
        memcpy(nav + 0x10, &climb, 8);
        uint8_t* tp = tiles;
        memcpy(nav + 0xE8, &tp, 8);
        const uint32_t tileBits = 10, polyBits = 12;
        memcpy(nav + 0xF4, &tileBits, 4);
        memcpy(nav + 0xF8, &polyBits, 4);
        struct ConsT { uint8_t* data; int32_t size; int32_t cap; };
        std::vector<uint8_t> bufA(4096 * 0x18), bufB(4096 * 0x18);
        int cases = 0, entries = 0, diff = 0, odd = 0;
        for (int round = 0; round < 40; ++round) {
            TileData a, b;
            make(a, tiles + 0 * 0xA8, 7, 24);
            make(b, tiles + 2 * 0xA8, 3 + round, 60 + round * 7);
            for (int c = 0; c < 200; ++c) {
                double va[3], vb[3];
                const double xs[4] = {10.0, 10.0, 10.004, 9.0};
                for (double* v : {va, vb}) {
                    v[0] = rnd(4) ? xs[rnd(4)] : static_cast<double>(rnd(12));
                    v[1] = static_cast<double>(rnd(4)) * 0.4;
                    v[2] = rnd(4) ? xs[rnd(4)] : static_cast<double>(rnd(12)) * 0.7;
                }
                const int side = static_cast<int>(rnd(8));
                const int from = static_cast<int>(rnd(24));
                ConsT ca{bufA.data(), static_cast<int32_t>(rnd(3)), 4096}, cb = ca;
                cb.data = bufB.data();
                memcpy(bufB.data(), bufA.data(), 3 * 0x18);
                const int r0 = NavLinkFastTestOriginal(nav, va, vb, tiles, from, tiles + 2 * 0xA8, side, &ca);
                const int r1 = NavLinkFastTestFind(1, nav, va, vb, tiles, from, tiles + 2 * 0xA8, side, &cb);
                ++cases;
                if (side & 1) { ++odd; continue; }  // odd sides: the game's path (its slab bounds are uninitialised there)
                entries += ca.size;
                if (r0 != r1 || ca.size != cb.size || memcmp(bufA.data(), bufB.data(), static_cast<size_t>(ca.size) * 0x18) != 0) ++diff;
            }
        }
        printf("      navlink: %d searches (%d odd sides), %d entries found by the game's search\n", cases, odd, entries);
        Check(diff == 0 && entries > 500, "navlink: the fast search finds the same entries as the game's (same order, bytes, return value)");
        // the skip rule: a connectExtLinks(tile, target, side) left out must be one whose every search finds nothing
        // (brute force: every portal edge of the tile the stock call would look at, searched with the game's code)
        int skipped = 0, made = 0, wrong = 0, productive = 0;
        for (int round = 0; round < 300; ++round) {
            TileData ta, tb;
            const uint32_t ma = round % 3 == 0 ? 0xFF : (1u << rnd(8)) | (rnd(2) ? 1u << rnd(8) : 0u);
            const uint32_t mb = round % 4 == 0 ? 0xFF : round % 5 == 0 ? 0 : (1u << rnd(8)) | (1u << rnd(8));
            make(ta, tiles, 7, 4 + static_cast<int>(rnd(20)), ma);
            make(tb, tiles + 2 * 0xA8, 9, 4 + static_cast<int>(rnd(40)), mb);
            for (int side = -1; side < 8; ++side) {
                int found = 0;
                for (int i = 0; i < static_cast<int>(ta.polys.size() / 0x20); ++i) {
                    const uint8_t* p = &ta.polys[static_cast<size_t>(i) * 0x20];
                    const int nv = p[0x1E];
                    for (int j = 0; j < nv; ++j) {
                        uint16_t nei, v0, v1;
                        memcpy(&nei, p + 0x10 + 2 * j, 2);
                        if (!(nei & 0x8000)) continue;
                        const int dir = nei & 0xFF;
                        if (side != -1 && dir != side) continue;
                        memcpy(&v0, p + 4 + 2 * j, 2);
                        memcpy(&v1, p + 4 + 2 * ((j + 1) % nv), 2);
                        ConsT cs{bufA.data(), 0, 4096};
                        NavLinkFastTestOriginal(nav, &ta.verts[v0 * 3], &ta.verts[v1 * 3], tiles, i, tiles + 2 * 0xA8, (dir - 4) & 7, &cs);
                        found += cs.size;
                    }
                }
                const int k = NavLinkFastTestSkip(tiles, tiles + 2 * 0xA8, side);
                if (k) { ++skipped; if (found) ++wrong; }
                else { ++made; if (found) ++productive; }
            }
        }
        printf("      navlink: skip rule: %d calls left out, %d made (%d of them make entries)\n", skipped, made, productive);
        Check(wrong == 0 && skipped > 300 && productive > 100, "navlink: every connectExtLinks call the rule leaves out makes no entry in the game's search");
    }
}

}  // namespace nlf
