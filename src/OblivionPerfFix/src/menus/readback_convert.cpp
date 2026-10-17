// ScreenshotReadbackConvert: the inventory's screenshot is converted to 8-bit colour one pixel at a time on
// the render thread while the game thread waits.
//
// Every time the player menu (inventory, map, magic, ...) opens, the game takes a screenshot of the game view
// (MenuScreenshotFilter is about what the game thread does with it afterwards). UE reads the back buffer back from the GPU in
// FD3D12DynamicRHI::RHIReadSurfaceData (FColor version, exe+0x252B2xx in build 19115871) and converts the raw pixels
// with the static ConvertDXGIToFColor (exe+0x251BBE0), a switch over the DXGI format. The game's back buffer is
// R10G10B10A2_UNORM (r.DefaultBackBufferPixelFormat 4, SDR), whose case runs FColor::MakeRequantizeFrom1010102 for each
// pixel through a byte-by-byte store and a dword reload (a store-forwarding stall per pixel): ~47 ms at 3840x2160 of the
// ~186 ms menu-open frame at 4K (2026-10-15, worker invmenu), proportional to the pixel count.
//
// The conversion per pixel p (the game's code, checked byte for byte at install):
//   B8 = q((p >> 20) & 0x3FF), G8 = q((p >> 10) & 0x3FF), R8 = q(p & 0x3FF), A8 = {0x00, 0x55, 0xAA, 0xFF}[p >> 30]
//   q(v) = (x + (x >> 10)) >> 10 with x = v * 255 + 512;   FColor = B8 | G8 << 8 | R8 << 16 | A8 << 24
// The fix computes the same with SSE2 integer arithmetic, four pixels per step (identical bytes: each output byte is a
// function of one 10-bit (or 2-bit) input field, which the self-test checks for every value against the game's own
// function, plus random images of many sizes and pitches). Only that format is taken over; every other format, a zero
// size, overlapping buffers or a size beyond the game's 32-bit index arithmetic go to the game's function.
// The one call in RHIReadSurfaceData is redirected (start-up install); the ini switches it live: 0 off (the game's loop),
// 1 on, 2 on + verify (also runs the game's loop into a scratch buffer and compares; for testing).
#include "common.h"

#include <emmintrin.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace opf {
namespace {

using ConvertFn = void (*)(uint32_t format, uint32_t width, uint32_t height, uint8_t* in, uint32_t srcPitch, uint32_t* out,
                           const void* flags);

// In RHIReadSurfaceData (build 19115871, exe+0x252B4B7): SrcPitch = SizeX * BytesPerPixel into the 5th argument slot,
// the DXGI format into ecx, then the call.
//   0F AF CB        imul ecx, ebx
//   89 4C 24 20     mov [rsp+20h], ecx
//   66 0F 7E D9     movd ecx, xmm3
//   E8 ?? ?? ?? ??  call ConvertDXGIToFColor
constexpr char kCallSite[] = "0F 29 45 FF 0F AF CB 89 4C 24 20 66 0F 7E D9 E8 ?? ?? ?? ??";
constexpr size_t kCallOffset = 15;
// ConvertDXGIToFColor's entry up to the switch: frame, Format - 2, InFlags->bLinearToGamma, cmp 59h / ja default,
// lea r9,[image base], movzx eax,[r9+rax+byteTable], mov ecx,[r9+rax*4+jumpTable], add rcx,r9, jmp rcx
constexpr char kTarget[] =
    "48 8B C4 55 57 41 56 41 57 48 8D 68 B9 48 81 EC F8 00 00 00 8B FA 83 C1 FE 48 8B 55 7F 45 8B F0 4D 8B F9 44 0F B6 42 08 "
    "83 F9 59 0F 87 ?? ?? ?? ?? 48 89 58 10 4C 8D 0D ?? ?? ?? ?? 48 89 70 18 4C 89 60 20 4C 89 68 D8 48 63 C1 "
    "41 0F B6 84 01 ?? ?? ?? ?? 41 8B 8C 81 ?? ?? ?? ?? 49 03 C9 FF E1";
constexpr size_t kLeaImageBase = 53;   // 4C 8D 0D disp32 (lea r9, [image base])
constexpr size_t kByteTableDisp = 80;  // disp32 of movzx eax, byte [r9+rax+table]
constexpr size_t kJumpTableDisp = 88;  // disp32 of mov ecx, [r9+rax*4+table]
constexpr uint32_t kFormatR10G10B10A2 = 24;  // DXGI_FORMAT_R10G10B10A2_UNORM
// The switch case for that format: rows, then per pixel the three 10-bit fields times 255 plus 512, folded and shifted,
// stored as bytes with alpha from the dword 0xFFAA5500, and the dword stored to Out.
constexpr char kCase[] =
    "33 DB 45 85 F6 0F 84 ?? ?? ?? ?? 4C 8B 6D 77 44 8B 65 6F 90 44 8B D3 45 0F AF D4 4D 03 D7 85 FF 0F 84 ?? ?? ?? ?? "
    "8B F3 C7 45 4F 00 55 AA FF 0F AF F7 4C 8B DF 48 C1 E6 02 49 2B F2 49 03 F5 0F 1F 44 00 00 "
    "45 8B 0A 4D 8D 52 04 41 8B C1 C1 E8 14 25 FF 03 00 00 69 C8 FF 00 00 00 41 8B C1 C1 E8 0A 25 FF 03 00 00 "
    "69 D0 FF 00 00 00 41 8B C1 25 FF 03 00 00 49 C1 E9 1E 44 69 C0 FF 00 00 00 81 C1 00 02 00 00 8B C1 C1 E8 0A 03 C1 "
    "81 C2 00 02 00 00 C1 E8 0A 88 45 7F 41 81 C0 00 02 00 00 8B C2 C1 E8 0A 03 C2 C1 E8 0A 88 85 80 00 00 00 "
    "41 8B C0 C1 E8 0A 41 03 C0 C1 E8 0A 88 85 81 00 00 00 42 0F B6 44 0D 4F 88 85 82 00 00 00 8B 45 7F 42 89 44 16 FC "
    "49 83 EB 01 0F 85 ?? ?? ?? ?? FF C3 41 3B DE 0F 82";

uint8_t* g_call = nullptr;
uint8_t* g_stub = nullptr;
ConvertFn g_original = nullptr;
std::atomic<int> g_mode{0};
std::atomic<uint64_t> g_fast{0}, g_passed{0}, g_fastTicks{0}, g_fastPixels{0}, g_verified{0}, g_mismatch{0}, g_gameTicks{0};
uint64_t g_fastLogged = 0, g_passedLogged = 0;

uint64_t Ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

inline uint32_t Requantize10(uint32_t v)
{
    const uint32_t x = v * 255u + 512u;
    return (x + (x >> 10)) >> 10;
}

inline __m128i Requantize10x4(__m128i v)  // four 32-bit lanes, each 0..1023
{
    const __m128i x = _mm_add_epi32(_mm_sub_epi32(_mm_slli_epi32(v, 8), v), _mm_set1_epi32(512));
    return _mm_srli_epi32(_mm_add_epi32(x, _mm_srli_epi32(x, 10)), 10);
}

}  // namespace

uint32_t ConvertR10G10B10A2Pixel(uint32_t p)
{
    return Requantize10((p >> 20) & 0x3FF) | Requantize10((p >> 10) & 0x3FF) << 8 | Requantize10(p & 0x3FF) << 16 |
           ((p >> 30) * 0x55u) << 24;
}

// The R10G10B10A2_UNORM case of UE's ConvertDXGIToFColor: rows of srcPitch bytes in, rows of width FColors out.
void ConvertR10G10B10A2ToColor(uint32_t width, uint32_t height, const uint8_t* in, uint32_t srcPitch, uint32_t* out)
{
    const __m128i mask = _mm_set1_epi32(0x3FF);
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(in + static_cast<size_t>(y) * srcPitch);
        uint32_t* dst = out + static_cast<size_t>(y) * width;
        uint32_t x = 0;
        for (; x + 4 <= width; x += 4) {
            const __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x));
            const __m128i b = Requantize10x4(_mm_and_si128(_mm_srli_epi32(p, 20), mask));
            const __m128i g = Requantize10x4(_mm_and_si128(_mm_srli_epi32(p, 10), mask));
            const __m128i r = Requantize10x4(_mm_and_si128(p, mask));
            const __m128i a2 = _mm_srli_epi32(p, 30);  // 0..3; * 0x55 = a + a<<2 + a<<4 + a<<6
            const __m128i a = _mm_or_si128(_mm_or_si128(a2, _mm_slli_epi32(a2, 2)), _mm_or_si128(_mm_slli_epi32(a2, 4), _mm_slli_epi32(a2, 6)));
            const __m128i c = _mm_or_si128(_mm_or_si128(b, _mm_slli_epi32(g, 8)), _mm_or_si128(_mm_slli_epi32(r, 16), _mm_slli_epi32(a, 24)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x), c);
        }
        for (; x < width; ++x) dst[x] = ConvertR10G10B10A2Pixel(src[x]);
    }
}

namespace {

void ConvertHook(uint32_t format, uint32_t width, uint32_t height, uint8_t* in, uint32_t srcPitch, uint32_t* out, const void* flags)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode != 0 && format == kFormatR10G10B10A2 && width > 0 && height > 0 && in && out && srcPitch >= width * 4ull) {
        // the game's loop indexes rows with 32-bit products (row * pitch, row * width): stay inside that range
        const uint64_t inBytes = static_cast<uint64_t>(height - 1) * srcPitch + width * 4ull;
        const uint64_t outBytes = static_cast<uint64_t>(height) * width * 4ull;
        const uintptr_t i0 = reinterpret_cast<uintptr_t>(in), o0 = reinterpret_cast<uintptr_t>(out);
        if (static_cast<uint64_t>(height) * srcPitch < (1ull << 32) && static_cast<uint64_t>(height) * width < (1ull << 32) &&
            (i0 + inBytes <= o0 || o0 + outBytes <= i0)) {
            const uint64_t t0 = Ticks();
            // ScreenshotParallel on: row bands on its helper threads (same bytes); else all rows here
            struct Ctx {
                uint32_t width;
                const uint8_t* in;
                uint32_t pitch;
                uint32_t* out;
            } const ctx{width, in, srcPitch, out};
            ScreenshotParallelRows(height, static_cast<size_t>(outBytes), [](const void* c, uint32_t y0, uint32_t y1) {
                const auto* x = static_cast<const Ctx*>(c);
                ConvertR10G10B10A2ToColor(x->width, y1 - y0, x->in + static_cast<size_t>(y0) * x->pitch, x->pitch,
                                          x->out + static_cast<size_t>(y0) * x->width);
            }, &ctx);
            const uint64_t t1 = Ticks();
            g_fastTicks.fetch_add(t1 - t0, std::memory_order_relaxed);
            g_fastPixels.fetch_add(static_cast<uint64_t>(width) * height, std::memory_order_relaxed);
            g_fast.fetch_add(1, std::memory_order_relaxed);
            if (mode == 2) {
                std::vector<uint32_t> ref(static_cast<size_t>(width) * height);
                const uint64_t t2 = Ticks();
                g_original(format, width, height, in, srcPitch, ref.data(), flags);
                g_gameTicks.fetch_add(Ticks() - t2, std::memory_order_relaxed);
                g_verified.fetch_add(1, std::memory_order_relaxed);
                if (memcmp(ref.data(), out, outBytes) != 0) {
                    g_mismatch.fetch_add(1, std::memory_order_relaxed);
                    size_t i = 0;
                    while (i < ref.size() && ref[i] == out[i]) ++i;
                    Log("[readback] VERIFY MISMATCH %ux%u pitch %u at pixel %zu: game %08X, fix %08X (the game's result is kept)",
                        width, height, srcPitch, i, ref[i], out[i]);
                    memcpy(out, ref.data(), outBytes);
                }
            }
            return;
        }
    }
    if (mode != 0) g_passed.fetch_add(1, std::memory_order_relaxed);  // switched off: not counted
    g_original(format, width, height, in, srcPitch, out, flags);
}

bool Fail(const char* why)
{
    Log("[readback] %s: fix not installed", why);
    return false;
}

}  // namespace

bool ApplyReadbackConvertFix(bool beforeGameCode)
{
    if (g_call) return true;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kCallSite));
    if (hits.size() != 1) {
        Log("[readback] the screenshot readback's conversion call was not found (%zu matches): fix not installed", hits.size());
        return false;
    }
    uint8_t* const call = hits[0] + kCallOffset;
    uint8_t* const target = RipTarget(call, 1, 5);
    if (!game.text.Contains(target) || !MatchesAt(target, ParsePattern(kTarget)))
        return Fail("the call does not go to the expected ConvertDXGIToFColor");
    // the switch: lea r9,[image base]; byte table [format - 2] -> case index; jump table [case] -> image-relative target
    if (RipTarget(target + kLeaImageBase, 3, 7) != game.base) return Fail("unexpected switch base");
    uint32_t byteTable = 0, jumpTable = 0;
    memcpy(&byteTable, target + kByteTableDisp, 4);
    memcpy(&jumpTable, target + kJumpTableDisp, 4);
    const uint8_t* const bt = game.base + byteTable;
    if (!game.text.Contains(bt + kFormatR10G10B10A2 - 2)) return Fail("unexpected switch table");
    const uint8_t caseIndex = bt[kFormatR10G10B10A2 - 2];
    const uint8_t* const jt = game.base + jumpTable + 4ull * caseIndex;
    if (!game.text.Contains(jt)) return Fail("unexpected jump table");
    uint32_t caseRva = 0;
    memcpy(&caseRva, jt, 4);
    uint8_t* const caseCode = game.base + caseRva;
    if (!game.text.Contains(caseCode) || !MatchesAt(caseCode, ParsePattern(kCase)))
        return Fail("the R10G10B10A2 case is not the expected requantize loop");
    uint8_t* const region = AllocNearGame(64);
    if (!region) return Fail("no free memory near the game's code");
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&ConvertHook);
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    const int64_t rel = region - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("the hook is out of reach");
    g_original = reinterpret_cast<ConvertFn>(target);
    const int32_t rel32 = static_cast<int32_t>(rel);
    if (!WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4)))
        return Fail("could not patch the call");
    g_call = call;
    g_stub = region;
    Log("[readback] ConvertDXGIToFColor (exe+0x%llX) called at exe+0x%llX; R10G10B10A2 case exe+0x%llX: installed",
        static_cast<unsigned long long>(target - game.base), static_cast<unsigned long long>(call - game.base),
        static_cast<unsigned long long>(caseCode - game.base));
    return true;
}

void SetReadbackConvertMode(int mode)
{
    if (!g_call) return;
    if (mode < 0 || mode > 2) mode = 0;
    if (g_mode.exchange(mode) == mode) return;
    Log("[readback] %s", mode == 0   ? "screenshot readback converted by the game's loop"
                         : mode == 1 ? "screenshot readback converted the fast way"
                                     : "screenshot readback converted the fast way, verified against the game's loop");
}

void LogReadbackConvertStats()
{
    const uint64_t fast = g_fast.load(), passed = g_passed.load();
    if (!g_call || (fast == g_fastLogged && passed == g_passedLogged)) return;
    g_fastLogged = fast;
    g_passedLogged = passed;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 / static_cast<double>(f.QuadPart);
    const uint64_t ver = g_verified.load();
    Log("[readback] screenshots converted the fast way: %llu (%.1f Mpixel, %.2f ms each on average); verified %llu (game's loop "
        "%.2f ms each), mismatches %llu; other formats left to the game: %llu",
        static_cast<unsigned long long>(fast), static_cast<double>(g_fastPixels.load()) / 1e6,
        fast ? ms * static_cast<double>(g_fastTicks.load()) / static_cast<double>(fast) : 0.0, static_cast<unsigned long long>(ver),
        ver ? ms * static_cast<double>(g_gameTicks.load()) / static_cast<double>(ver) : 0.0,
        static_cast<unsigned long long>(g_mismatch.load()), static_cast<unsigned long long>(passed));
}

bool ReadbackConvertFixInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook)
{
    if (!g_call) return false;
    *call = g_call;
    *original = reinterpret_cast<uint8_t*>(g_original);
    *stub = g_stub;
    *hook = reinterpret_cast<void*>(&ConvertHook);
    return true;
}

}  // namespace opf
