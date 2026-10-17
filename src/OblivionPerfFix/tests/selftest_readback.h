// ScreenshotReadbackConvert self-test, included by selftest.cpp: the call site on the mapped exe, then the game's
// own ConvertDXGIToFColor (run from the mapped exe: a leaf loop, no imports) against the fast conversion and the hook,
// for every value of every field, random images of many sizes and row pitches, other formats, the live switch, and the
// speed at 3840x2160.
#pragma once

namespace rbc {
using ConvertFn = void (*)(uint32_t format, uint32_t width, uint32_t height, uint8_t* in, uint32_t srcPitch, uint32_t* out,
                           const void* flags);

static void Run(const GameModule& game)
{
    Check(ApplyReadbackConvertFix(true), "readback convert: call site, switch case and loop found, call redirected");
    uint8_t *call = nullptr, *orig = nullptr, *stub = nullptr;
    void* hook = nullptr;
    if (!Installed(ReadbackConvertFixInfo(&call, &orig, &stub, &hook), "ScreenshotReadbackConvert (readback convert)")) return;
    Check(call == game.base + 0x252B4C2, "RHIReadSurfaceData calls ConvertDXGIToFColor at exe+0x252B4C2");
    Check(orig == game.base + 0x251BBE0, "ConvertDXGIToFColor at exe+0x251BBE0");
    int32_t rel = 0;
    memcpy(&rel, call + 1, 4);
    Check(call[0] == 0xE8 && call + 5 + rel == stub, "the call goes to the stub");
    const auto game24 = reinterpret_cast<ConvertFn>(orig);
    const auto hooked = reinterpret_cast<ConvertFn>(hook);
    alignas(16) uint8_t flags[64] = {};  // FReadSurfaceDataFlags: the game reads bLinearToGamma (+8) before its switch
    SetReadbackConvertMode(1);

    uint32_t seed = 4242;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
    // every value of each field (the other fields random), all four alpha values
    {
        std::vector<uint32_t> in;
        for (int field = 0; field < 3; ++field)
            for (uint32_t v = 0; v < 1024; ++v)
                for (int k = 0; k < 8; ++k) {
                    uint32_t p = rnd();
                    p = (p & ~(0x3FFu << (10 * field))) | (v << (10 * field));
                    in.push_back(p);
                }
        for (uint32_t a = 0; a < 4; ++a)
            for (int k = 0; k < 64; ++k) in.push_back((rnd() & 0x3FFFFFFFu) | (a << 30));
        for (uint32_t v = 0; v < 1024; ++v) in.push_back(v | v << 10 | v << 20 | 3u << 30);  // greys
        const uint32_t n = static_cast<uint32_t>(in.size());
        std::vector<uint32_t> a(n, 0xDEADBEEFu), b(n, 0x12345678u), c(n, 0x0BADF00Du);
        game24(24, n, 1, reinterpret_cast<uint8_t*>(in.data()), n * 4, a.data(), flags);
        ConvertR10G10B10A2ToColor(n, 1, reinterpret_cast<const uint8_t*>(in.data()), n * 4, b.data());
        hooked(24, n, 1, reinterpret_cast<uint8_t*>(in.data()), n * 4, c.data(), flags);
        bool pix = true;
        for (uint32_t i = 0; i < n; ++i) pix = pix && ConvertR10G10B10A2Pixel(in[i]) == a[i];
        char what[200];
        sprintf_s(what, "every value of R, G, B (x8 random others) and A: the fast conversion gives the game's bytes (%u pixels)", n);
        Check(a == b, what);
        Check(a == c, "... and so does the hook");
        Check(pix, "... and the scalar formula used for row ends");
    }
    // random images: sizes, row pitches with padding, odd widths for the scalar tail
    {
        const uint32_t sizes[][2] = {{1, 1}, {2, 1}, {3, 2}, {4, 4}, {5, 3}, {7, 7}, {8, 2}, {9, 5}, {17, 3}, {33, 9},
                                     {64, 48}, {127, 65}, {640, 360}, {1279, 7}};
        bool same = true, hookSame = true;
        int images = 0;
        for (const auto& sz : sizes)
            for (const uint32_t pad : {0u, 4u, 12u, 64u}) {
                const uint32_t w = sz[0], h = sz[1], pitch = w * 4 + pad;
                std::vector<uint8_t> in(static_cast<size_t>(pitch) * h);
                for (auto& x : in) x = static_cast<uint8_t>(rnd() >> 24);
                std::vector<uint32_t> a(static_cast<size_t>(w) * h, 1), b(a.size(), 2), c(a.size(), 3);
                game24(24, w, h, in.data(), pitch, a.data(), flags);
                ConvertR10G10B10A2ToColor(w, h, in.data(), pitch, b.data());
                hooked(24, w, h, in.data(), pitch, c.data(), flags);
                same = same && a == b;
                hookSame = hookSame && a == c;
                ++images;
            }
        char what[160];
        sprintf_s(what, "random images (%d: 14 sizes 1x1..1279x7, row padding 0/4/12/64 bytes): the game's bytes", images);
        Check(same, what);
        Check(hookSame, "... and through the hook");
    }
    // another format goes to the game's code (R8G8B8A8_UNORM = 28); zero sizes too; mode 0 = the game's loop
    {
        std::vector<uint8_t> in(64 * 4 * 8);
        for (auto& x : in) x = static_cast<uint8_t>(rnd() >> 24);
        std::vector<uint32_t> a(64 * 8, 1), b(64 * 8, 2);
        game24(28, 64, 8, in.data(), 256, a.data(), flags);
        hooked(28, 64, 8, in.data(), 256, b.data(), flags);
        Check(a == b, "R8G8B8A8 through the hook: the game's code, same bytes");
        std::vector<uint32_t> z(4, 7);
        hooked(24, 0, 4, in.data(), 0, z.data(), flags);
        Check(z == std::vector<uint32_t>(4, 7), "a zero width writes nothing");
        SetReadbackConvertMode(0);
        std::vector<uint32_t> c(64 * 8, 3);
        hooked(24, 64, 8, in.data(), 256, c.data(), flags);
        game24(24, 64, 8, in.data(), 256, a.data(), flags);
        Check(a == c, "switched off: the game's loop, same bytes");
        SetReadbackConvertMode(2);
        hooked(24, 64, 8, in.data(), 256, c.data(), flags);
        Check(a == c, "verify mode: same bytes");
        SetReadbackConvertMode(1);
    }
    // 3840x2160 (informational timing)
    {
        const uint32_t w = 3840, h = 2160;
        std::vector<uint32_t> in(static_cast<size_t>(w) * h), a(in.size()), b(in.size());
        for (auto& x : in) x = rnd();
        LARGE_INTEGER f, t0, t1, t2;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        game24(24, w, h, reinterpret_cast<uint8_t*>(in.data()), w * 4, a.data(), flags);
        QueryPerformanceCounter(&t1);
        hooked(24, w, h, reinterpret_cast<uint8_t*>(in.data()), w * 4, b.data(), flags);
        QueryPerformanceCounter(&t2);
        char msg[160];
        sprintf_s(msg, "3840x2160: same bytes; the game's loop %.1f ms, the fix %.1f ms",
                  1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart),
                  1000.0 * static_cast<double>(t2.QuadPart - t1.QuadPart) / static_cast<double>(f.QuadPart));
        Check(a == b, msg);
    }
    LogReadbackConvertStats();
}
}  // namespace rbc
