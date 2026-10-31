// HdrMenuBackground (trade-off) self-test, included by selftest.cpp: the handler's call site and the console
// manager on the mapped exe, the PQ -> sRGB table, and the wrapper with a fake ImageResize and fake console variables
// (HDR10 converts, SDR / scRGB / off pass through).
#pragma once

#include <cmath>
#include <vector>

namespace hmbg {

inline std::vector<uint32_t> g_fakeOut;
inline void FakeResize(int32_t, int32_t, const void*, int32_t, int32_t, void* dst, bool, bool)
{
    struct Arr { uint32_t* data; int32_t num, max; };
    auto* a = static_cast<Arr*>(dst);
    a->data = g_fakeOut.data();
    a->num = a->max = static_cast<int32_t>(g_fakeOut.size());
}

inline void Run(const opf::GameModule& game)
{
    using namespace opf;
    Check(ApplyHdrMenuBackground(true), "hdr menu background: call site, ImageResize and console manager found, call redirected");
    uint8_t *call = nullptr, *orig = nullptr, *stub = nullptr;
    void* hook = nullptr;
    void** manager = nullptr;
    if (!Installed(HdrMenuBackgroundInfo(&call, &orig, &stub, &hook, &manager), "HdrMenuBackground (menu background in HDR)")) return;
    Check(call == game.base + 0x4924F69, "hdr menu background: the handler calls ImageResize at exe+0x4924F69");
    Check(orig == game.base + 0x32548C0, "hdr menu background: ImageResize (TArray) at exe+0x32548C0");
    Check(reinterpret_cast<uint8_t*>(manager) == game.base + 0x9007F28, "hdr menu background: GConsoleManager at exe+0x9007F28");
    int32_t rel = 0;
    memcpy(&rel, call + 1, 4);
    Check(call[0] == 0xE8 && call + 5 + rel == stub, "hdr menu background: the call goes to the stub");

    // the table: black stays black, monotonic, 100 nits at a 250-nit UI white -> sRGB of 0.4 (170), the top rolled off
    uint8_t t[256];
    HdrMenuBackgroundTable(250.f, t);
    bool mono = true;
    for (int i = 1; i < 256; ++i) mono = mono && t[i] >= t[i - 1];
    const int c100 = static_cast<int>(std::lround(0.738752 * 255));  // the screenshot's byte for 100 nits: sRGB(PQ 0.508078)
    Check(t[0] == 0 && mono && t[255] <= 255 && t[255] >= 250, "hdr menu background: table monotonic from 0, top near white");
    // (byte 188 decodes to PQ 0.5025 = 96 nits: 8-bit rounding of the screenshot)
    Check(std::abs(t[c100] - 170) <= 5, "hdr menu background: ~100 nits at a 250-nit UI white -> sRGB ~170 (+-5)");

    // the wrapper: the fake resize's output converted in HDR10 only
    using Fn = void (*)(int32_t, int32_t, const void*, int32_t, int32_t, void*, bool, bool);
    struct Arr { uint32_t* data; int32_t num, max; } src{}, dst{};
    int32_t hdr = 1, device = 4;
    float ui = 250.f;
    HdrMenuBackgroundTestSetup(reinterpret_cast<void*>(&FakeResize), &hdr, &device, &ui);
    auto runOnce = [&](uint32_t pixel) {
        g_fakeOut.assign(6, pixel);
        reinterpret_cast<Fn>(hook)(3, 2, &src, 3, 2, &dst, false, true);
        return g_fakeOut[5];
    };
    const uint32_t in = 0xFF000000u | static_cast<uint32_t>(c100) << 16 | static_cast<uint32_t>(c100) << 8 | static_cast<uint32_t>(c100);
    const uint32_t want = 0xFF000000u | static_cast<uint32_t>(t[c100]) * 0x010101u;
    SetHdrMenuBackground(false);
    Check(runOnce(in) == in, "hdr menu background: off -> the game's bytes");
    SetHdrMenuBackground(true);
    Check(runOnce(in) == want, "hdr menu background: on, HDR10 (device 4) -> converted, alpha kept");
    device = 3;
    Check(runOnce(in) == want, "hdr menu background: on, HDR10 (device 3) -> converted");
    {
        // a full 5120x2160 picture (in bands on ScreenshotParallel's helpers when they run): every pixel as the table says
        constexpr int32_t w = 5120, h2 = 2160;
        uint32_t seed = 12345;
        g_fakeOut.resize(static_cast<size_t>(w) * h2);
        for (auto& px : g_fakeOut) { seed = seed * 1664525u + 1013904223u; px = seed; }
        std::vector<uint32_t> ref(g_fakeOut.size());
        for (size_t i = 0; i < ref.size(); ++i) {
            const uint32_t p = g_fakeOut[i];
            ref[i] = (p & 0xFF000000u) | static_cast<uint32_t>(t[(p >> 16) & 0xFF]) << 16 | static_cast<uint32_t>(t[(p >> 8) & 0xFF]) << 8 | t[p & 0xFF];
        }
        reinterpret_cast<Fn>(hook)(w, h2, &src, w, h2, &dst, false, true);
        Check(g_fakeOut == ref, "hdr menu background: a 5120x2160 picture converted pixel for pixel as the table says (banded on the helpers)");
    }
    hdr = 0;
    Check(runOnce(in) == in, "hdr menu background: on, SDR -> the game's bytes");
    hdr = 1;
    device = 5;
    Check(runOnce(in) == in, "hdr menu background: on, scRGB output -> the game's bytes");
    SetHdrMenuBackground(false);
    HdrMenuBackgroundTestSetup(orig, nullptr, nullptr, nullptr);
}

}  // namespace hmbg
