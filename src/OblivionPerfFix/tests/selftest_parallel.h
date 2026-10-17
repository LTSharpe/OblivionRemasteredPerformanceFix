// ScreenshotParallel self-test, included by selftest.cpp: the sites on the mapped exe, the two machine-code
// stubs run in small harnesses (the row loop's replacement, the zero-fill skip), the big-copy hook, and MenuScreenshotFilter and ScreenshotReadbackConvert
// on row bands against their single-thread result, with 3840x2160 timings.
#pragma once

namespace spar {

static uint8_t* g_code = nullptr;  // harness code (RWX, self-test only)

static uint8_t* Emit(std::initializer_list<uint8_t> bytes, uint8_t*& at)
{
    uint8_t* start = at;
    for (uint8_t b : bytes) *at++ = b;
    return start;
}

static void Put(uint8_t* at, const void* p) { memcpy(at, &p, 8); }

static void Run(const GameModule& game)
{
    Check(ApplyScreenshotParallelFix(true), "screenshot parallel: copy calls, row loop and zero fill found and patched");
    ScreenshotParallelSites s{};
    if (!Installed(ScreenshotParallelInfo(&s), "ScreenshotParallel (screenshot parallel)")) return;
    Check(s.copySites == 7, "7 copy calls (CopyImage, GetCopy x5, the delegate's array copy)");
    Check(s.helpers >= 1, "helper threads started");
    Check(s.rowSite == game.base + 0x252D0F5, "the readback's row loop at exe+0x252D0F5");
    Check(s.memsetSite == game.base + 0x325494A, "ImageResize's zero fill at exe+0x325494A");
    {
        int32_t rel = 0;
        memcpy(&rel, s.rowSite + 1, 4);
        Check(s.rowSite[0] == 0xE9 && s.rowSite + 5 + rel == s.rowStub, "the row loop jumps to its stub");
        memcpy(&rel, s.memsetSite + 1, 4);
        Check(s.memsetSite[0] == 0xE8 && s.memsetSite + 5 + rel == s.memsetStub, "the zero fill calls its stub");
        const uint8_t* copyCall = game.base + 0x1293002;  // CopyImage
        memcpy(&rel, copyCall + 1, 4);
        const uint8_t* stub = copyCall + 5 + rel;
        void* to = nullptr;
        memcpy(&to, stub + 6, 8);
        Check(copyCall[0] == 0xE8 && stub[0] == 0xFF && stub[1] == 0x25 && to == reinterpret_cast<void*>(&BigMemcpyHook),
              "CopyImage's memcpy (exe+0x1293002) goes to the big-copy hook");
        const uint32_t others[] = {0xFE7E51, 0xFE7F4E, 0xFE7FE6, 0xFE8016, 0xFE802B, 0xF9DF0D};
        bool all = true;
        for (uint32_t r : others) {
            memcpy(&rel, game.base + r + 1, 4);
            all = all && game.base[r] == 0xE8 && game.base + r + 5 + rel == stub;
        }
        Check(all, "GetCopy's five memcpy calls and the delegate's array copy (exe+0xF9DF0D) too");
    }
    Check(ApplyScreenshotParallelFix(true), "second application: already installed");

    g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    uint32_t seed = 777;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    auto ms = [&](LARGE_INTEGER a, LARGE_INTEGER b) { return 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart); };

    // --- the row loop's stub: harness(dst, src, bytesPerLine, pitch, rows) sets rsi, rdi, r15, r14d, r13d and jumps in;
    //     the stub's jump back is pointed at the harness's epilogue for the test
    {
        uint8_t* at = g_code;
        uint8_t* h = Emit({0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,  // push rsi, rdi, r13, r14, r15
                           0x48, 0x89, 0xCE, 0x48, 0x89, 0xD7, 0x4D, 0x89, 0xC7, 0x45, 0x89, 0xCE,  // rsi, rdi, r15, r14d
                           0x44, 0x8B, 0x6C, 0x24, 0x50,                                    // mov r13d,[rsp+50h]
                           0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0}, at);            // mov rax,stub; jmp rax
        Put(h + 27, s.rowStub);
        uint8_t* back = Emit({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x5F, 0x5E, 0xC3}, at);
        DWORD old;
        uint8_t* backSlot = s.rowStub + 46;
        void* gameBack = nullptr;
        memcpy(&gameBack, backSlot, 8);
        Check(gameBack == s.rowSite + 0x25, "the row stub returns after the loop (exe+0x252D11A)");
        VirtualProtect(backSlot, 8, PAGE_EXECUTE_READWRITE, &old);
        Put(backSlot, back);
        using RowFn = void (*)(uint8_t*, const uint8_t*, uint64_t, uint64_t, uint64_t);
        const auto rowFn = reinterpret_cast<RowFn>(h);
        bool same = true;
        const uint32_t cases[][3] = {{16, 1, 0}, {16, 0, 256}, {60, 7, 196}, {15360, 2160, 15360}, {7680, 1080, 7680}, {4000, 1500, 4096}};
        for (int mode = 0; mode < 2; ++mode) {
            SetScreenshotParallel(mode);
            for (const auto& c : cases) {
                const uint64_t bpl = c[0], rows = c[1], pitch = c[2] ? c[2] : bpl;
                std::vector<uint8_t> src(static_cast<size_t>(pitch * (rows ? rows : 1)) + 64), a(static_cast<size_t>(bpl * rows) + 64, 0xAB),
                    b(a.size(), 0xAB);
                for (auto& x : src) x = static_cast<uint8_t>(rnd() >> 24);
                for (uint64_t y = 0; y < rows; ++y) memcpy(a.data() + y * bpl, src.data() + y * pitch, bpl);
                rowFn(b.data(), src.data(), bpl, pitch, rows);
                same = same && a == b;
            }
        }
        Check(same, "row stub: off and on, 6 cases (0 / 1 / 7 rows, 4K, padded pitch): the game loop's bytes, nothing past the end");
        // 3840x2160 fresh destination: off vs on (informational)
        double t[2] = {};
        for (int mode = 0; mode < 2; ++mode) {
            SetScreenshotParallel(mode);
            std::vector<uint8_t> src(15360ull * 2160);
            memset(src.data(), 5, src.size());
            uint8_t* dst = static_cast<uint8_t*>(VirtualAlloc(nullptr, src.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            rowFn(dst, src.data(), 15360, 15360, 2160);
            QueryPerformanceCounter(&t1);
            t[mode] = ms(t0, t1);
            VirtualFree(dst, 0, MEM_RELEASE);
        }
        char msg[160];
        sprintf_s(msg, "3840x2160 readback row copy into fresh memory: one thread %.1f ms, helpers %.1f ms", t[0], t[1]);
        Check(true, msg);
        Put(backSlot, gameBack);
        VirtualProtect(backSlot, 8, old, &old);
    }

    // --- the zero-fill stub: harness(dstData, dstArray, srcArray, bytes) sets rdi / r15 and calls it as the game does;
    //     its memset is pointed at the CRT's for the test (the mapped exe's imports are not bound)
    {
        uint8_t* at = g_code + 512;
        uint8_t* h = Emit({0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x28,  // push rdi; push r15; sub rsp,28h
                           0x48, 0x89, 0xD7, 0x4D, 0x89, 0xC7, 0x4D, 0x89, 0xC8, 0x31, 0xD2,  // rdi, r15, r8 = bytes, edx = 0
                           0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0,      // mov rax,stub; call rax
                           0x48, 0x83, 0xC4, 0x28, 0x41, 0x5F, 0x5F, 0xC3}, at);
        Put(h + 20, s.memsetStub);
        uint8_t* fillSlot = s.memsetStub + 41;
        void* gameFill = nullptr;
        memcpy(&gameFill, fillSlot, 8);
        Check(gameFill == game.base + 0x6BFC464, "the zero-fill stub falls back to the game's memset");
        DWORD old;
        VirtualProtect(fillSlot, 8, PAGE_EXECUTE_READWRITE, &old);
        Put(fillSlot, reinterpret_cast<void*>(static_cast<void* (*)(void*, int, size_t)>(&memset)));
        using FillFn = void* (*)(void*, void*, void*, size_t);
        const auto fill = reinterpret_cast<FillFn>(h);
        struct Arr { uint32_t* data; int32_t num, max; };
        std::vector<uint32_t> d(1000, 0x11111111u), sdata(1000, 7);
        Arr dst{d.data(), 1000, 1000}, src{sdata.data(), 1000, 1000};
        SetScreenshotParallel(1);
        const uint64_t skips0 = *s.zeroSkips;
        void* r = fill(d.data(), &dst, &src, 4000);
        Check(r == d.data() && d[0] == 0x11111111u && d[999] == 0x11111111u && *s.zeroSkips == skips0 + 1,
              "on, two arrays, fill at the data: skipped (returns the destination)");
        r = fill(d.data(), &dst, &dst, 4000);
        Check(r == d.data() && d[0] == 0 && d[999] == 0, "same array as source and destination: filled");
        std::fill(d.begin(), d.end(), 0x11111111u);
        fill(d.data() + 1, &dst, &src, 3996);
        Check(d[0] == 0x11111111u && d[1] == 0 && d[999] == 0, "a fill that does not start at the data: filled");
        std::fill(d.begin(), d.end(), 0x11111111u);
        SetScreenshotParallel(0);
        fill(d.data(), &dst, &src, 4000);
        Check(d[0] == 0 && d[999] == 0 && *s.zeroSkips == skips0 + 1, "off: filled");
        Put(fillSlot, gameFill);
        VirtualProtect(fillSlot, 8, old, &old);
    }

    // --- the big-copy hook
    {
        SetScreenshotParallel(1);
        bool ok = true;
        for (const size_t n : {size_t{0}, size_t{1}, size_t{4095}, size_t{(4u << 20) - 1}, size_t{4u << 20}, size_t{(9u << 20) + 13},
                               size_t{33177600}}) {
            std::vector<uint8_t> src(n + 1), a(n + 1, 0xCD);
            for (auto& x : src) x = static_cast<uint8_t>(rnd() >> 24);
            void* r = BigMemcpyHook(a.data(), src.data(), n);
            ok = ok && r == a.data() && memcmp(a.data(), src.data(), n) == 0 && a[n] == 0xCD;
        }
        Check(ok, "big-copy hook: 0 B .. 33 MB, the same bytes, nothing past the end, returns the destination");
        std::vector<uint8_t> o(10u << 20), ref;
        for (auto& x : o) x = static_cast<uint8_t>(rnd() >> 24);
        ref = o;
        memmove(ref.data() + 4096, ref.data(), 8u << 20);
        BigMemcpyHook(o.data() + 4096, o.data(), 8u << 20);
        Check(o == ref, "overlapping 8 MB copy: memmove's result (the game's memcpy is a memmove)");
        double t[2] = {};
        std::vector<uint8_t> src(33177600, 9);
        for (int mode = 0; mode < 2; ++mode) {
            SetScreenshotParallel(mode);
            uint8_t* dst = static_cast<uint8_t*>(VirtualAlloc(nullptr, src.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            BigMemcpyHook(dst, src.data(), src.size());
            QueryPerformanceCounter(&t1);
            t[mode] = ms(t0, t1);
            VirtualFree(dst, 0, MEM_RELEASE);
        }
        char msg[160];
        sprintf_s(msg, "33 MB copy into fresh memory: one thread %.1f ms, helpers %.1f ms", t[0], t[1]);
        Check(true, msg);
    }

    // --- MenuScreenshotFilter and ScreenshotReadbackConvert on row bands: the same bytes as on one thread (3840x2160, odd sizes)
    {
        uint8_t *call = nullptr, *orig = nullptr, *stub = nullptr;
        void* hook = nullptr;
        const uint32_t sizes[][2] = {{3840, 2160}, {2561, 1441}, {1000, 3}};
        if (ReadbackConvertFixInfo(&call, &orig, &stub, &hook)) {
            using ConvertFn = void (*)(uint32_t, uint32_t, uint32_t, uint8_t*, uint32_t, uint32_t*, const void*);
            const auto hooked = reinterpret_cast<ConvertFn>(hook);
            alignas(16) uint8_t flags[64] = {};
            SetReadbackConvertMode(1);
            bool same = true;
            double t[2] = {};
            for (const auto& sz : sizes) {
                const uint32_t w = sz[0], hgt = sz[1], pitch = w * 4 + 64;
                std::vector<uint8_t> in(static_cast<size_t>(pitch) * hgt);
                for (auto& x : in) x = static_cast<uint8_t>(rnd() >> 24);
                std::vector<uint32_t> a(static_cast<size_t>(w) * hgt), b(a.size(), 1);
                ConvertR10G10B10A2ToColor(w, hgt, in.data(), pitch, a.data());
                for (int mode = 0; mode < 2; ++mode) {
                    SetScreenshotParallel(mode);
                    std::fill(b.begin(), b.end(), 1u);
                    LARGE_INTEGER t0, t1;
                    QueryPerformanceCounter(&t0);
                    hooked(24, w, hgt, in.data(), pitch, b.data(), flags);
                    QueryPerformanceCounter(&t1);
                    if (w == 3840) t[mode] = ms(t0, t1);
                    same = same && a == b;
                }
            }
            char msg[200];
            sprintf_s(msg, "ScreenshotReadbackConvert on row bands: the single-thread bytes (3 sizes, padded rows); 4K warm: one thread %.1f ms, helpers %.1f ms",
                      t[0], t[1]);
            Check(same, msg);
        }
        if (ImageResizeFixInfo(&call, &orig, &stub, &hook)) {
            struct View { uint32_t* data; int32_t num; };
            using ViewResize = void (*)(int32_t, int32_t, const View*, int32_t, int32_t, const View*, bool, bool);
            const auto hooked = reinterpret_cast<ViewResize>(hook);
            SetImageResizeFixEnabled(true);
            bool same = true;
            double t[2] = {};
            for (const auto& sz : sizes)
                for (const bool opaque : {false, true}) {
                    const int32_t w = static_cast<int32_t>(sz[0]), hgt = static_cast<int32_t>(sz[1]);
                    std::vector<uint32_t> in(static_cast<size_t>(w) * hgt), a(in.size()), b(in.size(), 1);
                    for (auto& x : in) x = rnd();
                    BoxFilter2x2(w, hgt, in.data(), a.data(), opaque);
                    const View sv{in.data(), static_cast<int32_t>(in.size())};
                    const View dv{b.data(), static_cast<int32_t>(b.size())};
                    for (int mode = 0; mode < 2; ++mode) {
                        SetScreenshotParallel(mode);
                        std::fill(b.begin(), b.end(), 1u);
                        LARGE_INTEGER t0, t1;
                        QueryPerformanceCounter(&t0);
                        hooked(w, hgt, &sv, w, hgt, &dv, false, opaque);
                        QueryPerformanceCounter(&t1);
                        if (w == 3840 && opaque) t[mode] = ms(t0, t1);
                        same = same && a == b;
                    }
                }
            char msg[200];
            sprintf_s(msg, "MenuScreenshotFilter on row bands: the single-thread bytes (3 sizes, opaque or not); 4K warm: one thread %.1f ms, helpers %.1f ms",
                      t[0], t[1]);
            Check(same, msg);
        }
    }
    SetScreenshotParallel(1);
    LogScreenshotParallelStats();
    SetScreenshotParallel(0);
    VirtualFree(g_code, 0, MEM_RELEASE);
}
}  // namespace spar
