// MenuScreenshotFilter: opening the in-game menu filters a full-resolution screenshot on the game thread, one pixel at a time.
//
// Every time the player menu opens, the game takes a screenshot of the game view (the engine's OnScreenshotCaptured
// delegate; Altar's handler exe+0x4924EF0 in build 19115871, presumably for the save-game picture) and passes the
// bitmap through FImageUtils::ImageResize with the destination as large as the source (TArray version exe+0x32548C0
// -> TArrayView version exe+0x32549E0), before it makes a texture of the result. With equal sizes UE's ImageResize is
// a 2x2 box filter: each output pixel is the average of the input pixel and its right, lower and lower-right
// neighbours (only those that exist at the right and bottom edges), channel by channel, truncated, and alpha set to
// 255 when asked for. UE computes it pixel by pixel in double precision: ~0.17 s at 3840x2160 (about a third of the
// menu-open frame at 4K), proportional to the pixel count.
//
// The fix computes exactly the same bytes with integer SSE2 arithmetic. For 1, 2 or 4 pixels the double-precision
// average is exact before it is truncated (channel sums <= 1020, divisors 1, 2 or 4), so the integer sum shifted right
// by 0, 1 or 2 gives the same value. It handles only that case (same size, not "linear space", the arrays as large as
// the sizes say, source and destination not overlapping); every other call runs the game's code. The call is
// redirected at its one call site in the TArray version; the self-test compares the result with the game's own
// function, byte for byte, on many images.
#include "common.h"

#include <emmintrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace opf {
namespace {

struct ColorView {  // TArrayView<FColor>: data, num
    uint32_t* data;
    int32_t num;
};
using ViewResizeFn = void (*)(int32_t srcW, int32_t srcH, const ColorView* src, int32_t dstW, int32_t dstH, const ColorView* dst,
                              bool linear, bool forceOpaque);

// In the TArray version: the arguments for the TArrayView version, then the call (build 19115871: exe+0x325497C)
//   0F B6 84 24 C8 00 00 00   movzx eax, byte [rsp+0C8h]   (bForceOpaqueOutput)
//   88 44 24 38               mov [rsp+38h], al
//   0F B6 84 24 C0 00 00 00   movzx eax, byte [rsp+0C0h]   (bLinearSpace)
//   88 44 24 30               mov [rsp+30h], al
//   48 8D 44 24 40            lea rax, [rsp+40h]           (&DstView)
//   48 89 44 24 28            mov [rsp+28h], rax
//   44 89 74 24 20            mov [rsp+20h], r14d          (DstHeight)
//   E8 ?? ?? ?? ??            call ImageResize(TArrayView)
constexpr const char* kCallSite =
    "0F B6 84 24 C8 00 00 00 88 44 24 38 0F B6 84 24 C0 00 00 00 88 44 24 30 48 8D 44 24 40 48 89 44 24 28 "
    "44 89 74 24 20 E8 ?? ?? ?? ??";
constexpr size_t kCallOffset = 39;
// The TArrayView version's entry: spills its four register arguments, 0x158 bytes of frame.
constexpr const char* kTarget = "48 8B C4 44 89 48 20 4C 89 40 18 89 50 10 89 48 08 55 56 48 8D 68 98 48 81 EC 58 01 00 00";

uint8_t* g_call = nullptr;
uint8_t* g_stub = nullptr;
ViewResizeFn g_original = nullptr;
std::atomic<bool> g_enabled{true};
std::atomic<uint64_t> g_fast{0}, g_passed{0}, g_fastTicks{0}, g_fastPixels{0};
uint64_t g_fastLogged = 0, g_passedLogged = 0;

uint64_t Ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

}  // namespace

// The 2x2 box filter of UE's ImageResize for equal sizes (FColor = 4 bytes, each lane averaged on its own), output rows
// [y0, y1) (each output row depends only on input rows y and y + 1, so bands can run on different threads).
void BoxFilter2x2Rows(int32_t w, int32_t h, const uint32_t* src, uint32_t* dst, bool forceOpaque, int32_t y0, int32_t y1);
void BoxFilter2x2(int32_t w, int32_t h, const uint32_t* src, uint32_t* dst, bool forceOpaque)
{
    BoxFilter2x2Rows(w, h, src, dst, forceOpaque, 0, h);
}

void BoxFilter2x2Rows(int32_t w, int32_t h, const uint32_t* src, uint32_t* dst, bool forceOpaque, int32_t y0, int32_t y1)
{
    const uint32_t alpha = forceOpaque ? 0xFF000000u : 0u;
    const __m128i zero = _mm_setzero_si128();
    const __m128i alphaMask = _mm_set1_epi32(static_cast<int>(alpha));
    auto lanes = [](uint32_t c, uint32_t (&s)[4]) {
        s[0] += c & 0xFF;
        s[1] += (c >> 8) & 0xFF;
        s[2] += (c >> 16) & 0xFF;
        s[3] += c >> 24;
    };
    auto pack = [&](const uint32_t (&s)[4], int shift) {
        return (s[0] >> shift) | (s[1] >> shift) << 8 | (s[2] >> shift) << 16 | (s[3] >> shift) << 24 | alpha;
    };
    for (int32_t y = y0; y < y1; ++y) {
        const uint32_t* r0 = src + static_cast<size_t>(y) * w;
        uint32_t* out = dst + static_cast<size_t>(y) * w;
        if (y + 1 < h) {
            const uint32_t* r1 = r0 + w;
            int32_t x = 0;
            for (; x + 4 < w; x += 4) {  // pixels x..x+3, their right neighbours up to x+4
                const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r0 + x));
                const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r0 + x + 1));
                const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r1 + x));
                const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r1 + x + 1));
                __m128i lo = _mm_add_epi16(_mm_add_epi16(_mm_unpacklo_epi8(a, zero), _mm_unpacklo_epi8(b, zero)),
                                           _mm_add_epi16(_mm_unpacklo_epi8(c, zero), _mm_unpacklo_epi8(d, zero)));
                __m128i hi = _mm_add_epi16(_mm_add_epi16(_mm_unpackhi_epi8(a, zero), _mm_unpackhi_epi8(b, zero)),
                                           _mm_add_epi16(_mm_unpackhi_epi8(c, zero), _mm_unpackhi_epi8(d, zero)));
                const __m128i r = _mm_packus_epi16(_mm_srli_epi16(lo, 2), _mm_srli_epi16(hi, 2));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x), _mm_or_si128(r, alphaMask));
            }
            for (; x + 1 < w; ++x) {
                uint32_t s[4] = {};
                lanes(r0[x], s);
                lanes(r0[x + 1], s);
                lanes(r1[x], s);
                lanes(r1[x + 1], s);
                out[x] = pack(s, 2);
            }
            uint32_t s[4] = {};  // the last column: this pixel and the one below
            lanes(r0[x], s);
            lanes(r1[x], s);
            out[x] = pack(s, 1);
        } else {
            int32_t x = 0;
            for (; x + 1 < w; ++x) {  // the last row: this pixel and the one to the right
                uint32_t s[4] = {};
                lanes(r0[x], s);
                lanes(r0[x + 1], s);
                out[x] = pack(s, 1);
            }
            uint32_t s[4] = {};  // the corner: the pixel alone
            lanes(r0[x], s);
            out[x] = pack(s, 0);
        }
    }
}

namespace {

void ResizeHook(int32_t srcW, int32_t srcH, const ColorView* src, int32_t dstW, int32_t dstH, const ColorView* dst, bool linear,
                bool forceOpaque)
{
    if (g_enabled.load(std::memory_order_relaxed) && !linear && srcW == dstW && srcH == dstH && srcW > 0 && srcH > 0 && src &&
        dst && src->data && dst->data) {
        const int64_t n = static_cast<int64_t>(srcW) * srcH;
        const uintptr_t s0 = reinterpret_cast<uintptr_t>(src->data), d0 = reinterpret_cast<uintptr_t>(dst->data);
        const uintptr_t bytes = static_cast<uintptr_t>(n) * 4;
        if (n <= src->num && n <= dst->num && (s0 + bytes <= d0 || d0 + bytes <= s0)) {
            const uint64_t t0 = Ticks();
            // ScreenshotParallel on: row bands on its helper threads (same bytes); else all rows here
            struct Ctx {
                int32_t w, h;
                const uint32_t* src;
                uint32_t* dst;
                bool forceOpaque;
            } const ctx{srcW, srcH, src->data, dst->data, forceOpaque};
            ScreenshotParallelRows(static_cast<uint32_t>(srcH), static_cast<size_t>(bytes), [](const void* c, uint32_t y0, uint32_t y1) {
                const auto* x = static_cast<const Ctx*>(c);
                BoxFilter2x2Rows(x->w, x->h, x->src, x->dst, x->forceOpaque, static_cast<int32_t>(y0), static_cast<int32_t>(y1));
            }, &ctx);
            g_fastTicks.fetch_add(Ticks() - t0, std::memory_order_relaxed);
            g_fastPixels.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
            g_fast.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_passed.fetch_add(1, std::memory_order_relaxed);
    g_original(srcW, srcH, src, dstW, dstH, dst, linear, forceOpaque);
}

// A free region within rel32 reach of the game's code.
uint8_t* AllocNearImage(size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const GameModule& game = Game();
    const uintptr_t base = reinterpret_cast<uintptr_t>(game.base);
    const uintptr_t end = base + game.sizeOfImage;
    constexpr uintptr_t kReach = 0x70000000;
    for (uintptr_t a = (end + gran - 1) & ~(gran - 1); a + size < base + kReach;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.State == MEM_FREE && a + size <= regionEnd)
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<uint8_t*>(p);
        a = (regionEnd + gran - 1) & ~(gran - 1);
    }
    for (uintptr_t a = (base & ~(gran - 1)) - gran; a > gran && end - a < kReach; a -= gran)
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            return static_cast<uint8_t*>(p);
    return nullptr;
}

}  // namespace

bool ApplyImageResizeFix(bool beforeGameCode)
{
    if (g_call) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kCallSite));
    if (hits.size() != 1) {
        Log("[resize] ImageResize call not found (%zu matches): fix not installed", hits.size());
        return false;
    }
    uint8_t* const call = hits[0] + kCallOffset;
    uint8_t* const target = RipTarget(call, 1, 5);
    if (!game.text.Contains(target) || !MatchesAt(target, ParsePattern(kTarget))) {
        Log("[resize] the call at exe+0x%llX does not go to the expected ImageResize: fix not installed",
            static_cast<unsigned long long>(call - game.base));
        return false;
    }
    // A stub "jmp [rip+0]; dq hook" at the target's address modulo 64 KB, so the call's new rel32 keeps its low 16 bits
    // and only two bytes of the instruction change: one atomic write when they lie in one aligned qword.
    constexpr size_t kRegion = 0x20000;
    uint8_t* const region = AllocNearImage(kRegion);
    if (!region) {
        Log("[resize] no free memory near the game's code for the call stub: fix not installed");
        return false;
    }
    uint8_t* const stub = region + (reinterpret_cast<uintptr_t>(target) & 0xFFFF);
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&ResizeHook);
    memcpy(stub, jmp, sizeof jmp);
    memcpy(stub + sizeof jmp, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, kRegion, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 14);
    const int64_t rel = stub - (call + 5);
    if (rel != static_cast<int32_t>(rel)) {
        VirtualFree(region, 0, MEM_RELEASE);
        Log("[resize] call stub out of reach: fix not installed");
        return false;
    }
    g_original = reinterpret_cast<ViewResizeFn>(target);
    uint8_t insn[5] = {0xE8};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(insn + 1, &rel32, sizeof rel32);
    size_t first = 5, last = 0;
    for (size_t i = 0; i < 5; ++i)
        if (insn[i] != call[i]) { first = std::min(first, i); last = i; }
    const uintptr_t lo = reinterpret_cast<uintptr_t>(call) + first, hi = reinterpret_cast<uintptr_t>(call) + last;
    const uintptr_t qword = lo & ~uintptr_t{7};
    bool ok = false;
    if (first == 5) {
        ok = true;
    } else if (hi < qword + 8) {
        auto* q = reinterpret_cast<volatile LONG64*>(qword);
        if (VirtualProtect(reinterpret_cast<void*>(qword), 8, PAGE_EXECUTE_READWRITE, &old)) {
            const LONG64 before = *q;
            LONG64 after = before;
            for (size_t i = first; i <= last; ++i)
                reinterpret_cast<uint8_t*>(&after)[reinterpret_cast<uintptr_t>(call) + i - qword] = insn[i];
            ok = InterlockedCompareExchange64(q, after, before) == before;
            VirtualProtect(reinterpret_cast<void*>(qword), 8, old, &old);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(qword), 8);
        }
    } else if (beforeGameCode) {
        ok = WriteCode(call, insn);
    } else {
        Log("[resize] the call cannot be switched atomically while the game runs: fix not installed");
    }
    if (!ok) {
        VirtualFree(region, 0, MEM_RELEASE);
        Log("[resize] writing the call at exe+0x%llX failed: fix not installed", static_cast<unsigned long long>(call - game.base));
        return false;
    }
    g_call = call;
    g_stub = stub;
    Log("[resize] ImageResize (exe+0x%llX) called at exe+0x%llX: same-size calls use the fast box filter",
        static_cast<unsigned long long>(target - game.base), static_cast<unsigned long long>(call - game.base));
    return true;
}

void SetImageResizeFixEnabled(bool enabled) { g_enabled = enabled; }

void LogImageResizeStats()
{
    const uint64_t fast = g_fast.load(), passed = g_passed.load();
    if (!g_call || (fast == g_fastLogged && passed == g_passedLogged)) return;
    g_fastLogged = fast;
    g_passedLogged = passed;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const uint64_t px = g_fastPixels.load();
    Log("[resize] same-size image filters done the fast way: %llu (%.1f Mpixel, %.2f ms each on average); other ImageResize "
        "calls left to the game: %llu",
        static_cast<unsigned long long>(fast), static_cast<double>(px) / 1e6,
        fast ? 1000.0 * static_cast<double>(g_fastTicks.load()) / static_cast<double>(f.QuadPart) / static_cast<double>(fast) : 0.0,
        static_cast<unsigned long long>(passed));
}

bool ImageResizeFixInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook)
{
    if (!g_call) return false;
    *call = g_call;
    *original = reinterpret_cast<uint8_t*>(g_original);
    *stub = g_stub;
    *hook = reinterpret_cast<void*>(&ResizeHook);
    return true;
}

}  // namespace opf
