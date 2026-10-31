// HdrGamma22 (trade-off) self-test, included by selftest.cpp: the tone curve's block and its builder are found
// where build 19115871 has them, and the fit gives the measured quality on a block read from the running game (Mid-Tone
// 15, peak 1300, black floor -4; 2026-10-27).
#pragma once

#include <chrono>
#include <cmath>

namespace hdrg {

inline void Run(const opf::GameModule& game)
{
    using namespace opf;
    HdrGammaInfoOut h{};
    if (!Installed(FindHdrGamma(&h), "HdrGamma22 (HDR tone curve)")) return;
    Check(h.getter == game.base + 0x26C0790, "hdr gamma: the tone curve block's getter at exe+0x26C0790");
    Check(h.block == game.base + 0x8DD9340, "hdr gamma: the block at exe+0x8DD9340");
    Check(h.builder == game.base + 0x26CA6CC, "hdr gamma: the parameter build's mid point (4.8 / 1.55) at exe+0x26CA6CC");
    const float block[20] = {2.51645861e-06f, 0.0001f, 1027.8291f, 1300.f, 0.0824593157f, 4.8f, 1.55f, 1.f, -3.99999976f, -3.99999976f,
                             -3.15737629f, -0.485249758f, -0.376809835f, 1.73929214f, 2.861377f, 3.11394334f, 1.84773231f,
                             3.11394334f, 1.5f, 0.f};
    Check(std::fabs(HdrGammaEval(block, 0.18) - 15.0) < 0.01, "hdr gamma: the curve maps 0.18 to the Mid-Tone (15 nits)");
    double worst = 1;
    float out[20] = {};
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = HdrGammaSelfFit(block, 9.5, &worst, out);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Check(ok && worst < 0.07, "hdr gamma: fit monotonic and within 7 % from 0.1 % of W up");
    Check(ok && out[5] == 4.8f && out[6] == 1.55f && out[1] == block[1] && out[3] == block[3] && out[2] == block[2],
          "hdr gamma: floor, peak, peak input and the mid-point constants kept");
    // the corrected curve: highlights unchanged above W, darks lower
    Check(ok && std::fabs(HdrGammaEval(out, 4.0) / HdrGammaEval(block, 4.0) - 1) < 0.07, "hdr gamma: highlights within 7 % of the game's");
    Check(ok && HdrGammaEval(out, 0.01) < 0.7 * HdrGammaEval(block, 0.01), "hdr gamma: darks lower than the game's");
    Log("[hdrgamma] self-test fit: worst %.1f %%, %.1f ms", 100 * worst, ms);
}

}  // namespace hdrg
