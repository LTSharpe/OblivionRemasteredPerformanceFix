// HdrMenuBackground (TRADE-OFF, off by default): the player menu's background in HDR with the depth it has in
// SDR, not washed out.
//
// The player menu (inventory, map, System, ...) draws the world behind it from a screenshot: Altar's OnScreenshotCaptured
// handler (exe+0x4924EF0 in build 19115871) gets the back buffer's pixels as 8-bit FColor (RHIReadSurfaceData, see
// ScreenshotReadbackConvert), runs them through FImageUtils::ImageResize at the same size (see MenuScreenshotFilter),
// makes a texture of the result and gives it to WBP_InventoryBackgroundWidget's Image "Background", under a
// BackgroundBlur (probe game 2026-10-28: the widget tree). In SDR the back buffer holds sRGB-encoded colour, which the UI
// shows as sRGB: the world as it was. In HDR (r.HDR.Display.OutputDevice 3 or 4, ST 2084 in 10:10:10:2) the bytes the
// handler gets are the PQ value sRGB-encoded (a dump of them against a capture of the same view: byte 112 / 128 / 184 =
// 1.65 / 3.5 / 80 nits = sRGB(PQ(nits)) within 1-2 codes), so the UI shows the PQ value itself as the colour: 3.5 nits
// of world at 22 % of the UI's white, shadows raised many times, highlights pulled down, the whole background a flat
// grey.
//
// The fix redirects the handler's one call of ImageResize (TArray version, exe+0x32548C0) to a wrapper that calls it
// and then, while the switch is on and the output is HDR10, maps each colour byte of the result through a 256-entry
// table: sRGB-decode (the PQ value) -> nits -> divided by r.HDR.UI.Luminance (the nits the UI shows its white at) ->
// sRGB, with a soft shoulder from 80 % up instead of a hard clip. The UI then shows the background at the nits the world had (up to the
// UI's white), like SDR shows it. SDR, or an HDR output that is not ST 2084, leaves the bytes alone. The other two
// callers of ImageResize are not touched. The console variables are read through the game's console manager
// (GConsoleManager, IConsoleManager::FindConsoleVariable at vtable+0xB8, found from the game's own lookup of
// r.HDR.UI.Luminance; IConsoleVariable::GetFloat at vtable+0x98 as that code reads it; AsVariableInt at +0x58 as the
// game's lookups of the two int variables read them).
//
// Changes the picture on purpose (the menu background in HDR only), so off by default. Start-up install, then live.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace opf {
namespace {

struct ColorArray {  // TArray<FColor>
    uint32_t* data;
    int32_t num, max;
};
using ResizeFn = void (*)(int32_t srcW, int32_t srcH, const ColorArray* src, int32_t dstW, int32_t dstH, ColorArray* dst, bool linear,
                          bool forceOpaque);
using FindCvarFn = void* (*)(void* manager, const wchar_t* name, bool trackFrequentCalls);
using AsIntFn = int32_t* (*)(void* cvar);   // TConsoleVariableData<int32>* (game thread's value first)
using GetFloatFn = float (*)(void* cvar);

// In Altar's OnScreenshotCaptured handler (build 19115871: exe+0x4924F3D; the call at exe+0x4924F69):
//   C6 44 24 38 01     mov byte [rsp+38h],1      (bForceOpaqueOutput)
//   48 8D 44 24 40     lea rax,[rsp+40h]         (the destination TArray)
//   44 88 74 24 30     mov [rsp+30h],r14b        (bLinearSpace = false)
//   48 89 44 24 28     mov [rsp+28h],rax
//   44 8B CE / 4C 8B C5 / 89 7C 24 20 / 8B D7      DstWidth, &Bitmap, DstHeight, SrcHeight
//   4C 89 74 24 40 / 8B CE / 4C 89 74 24 48        the destination emptied, SrcWidth
//   E8 rel32           call ImageResize (TArray)
//   48 8B 44 24 40 48 8D 4C 24 50                  the result's data -> the texture's creation parameters
constexpr const char* kCallSite =
    "C6 44 24 38 01 48 8D 44 24 40 44 88 74 24 30 48 89 44 24 28 44 8B CE 4C 8B C5 89 7C 24 20 8B D7 4C 89 74 24 40 8B CE "
    "4C 89 74 24 48 E8 ?? ?? ?? ?? 48 8B 44 24 40 48 8D 4C 24 50";
constexpr size_t kCallOffset = 44;
// ImageResize (TArray): mov rax,rsp; mov [rax+18h],rbx; push rbp, r12-r15; sub rsp,60h; DstHeight; ebx = DstWidth * DstHeight
constexpr const char* kResizeEntry =
    "48 8B C4 48 89 58 18 55 41 54 41 55 41 56 41 57 48 83 EC 60 44 8B B4 24 B0 00 00 00 41 8B D9 41 0F AF DE";
// The game's lookup of r.HDR.UI.Luminance: rcx = GConsoleManager (made first if null), FindConsoleVariable(name, true)
constexpr const char* kLookup =
    "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0C E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8D 15 ?? ?? ?? ?? 41 B0 01 FF 90 B8 00 00 00";
constexpr size_t kLookupLoad1 = 0, kLookupLoad2 = 17, kLookupName = 27;
constexpr uint32_t kFindVar = 0xB8, kAsInt = 0x58, kGetFloat = 0x98;
constexpr wchar_t kUiLum[] = L"r.HDR.UI.Luminance";
constexpr wchar_t kHdrOut[] = L"r.HDR.EnableHDROutput";
constexpr wchar_t kDevice[] = L"r.HDR.Display.OutputDevice";

uint8_t* g_call = nullptr;
uint8_t* g_stub = nullptr;
ResizeFn g_original = nullptr;
void** g_manager = nullptr;  // &GConsoleManager
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_converted{0}, g_left{0};
std::atomic<double> g_ms{0};  // the conversions' time (game thread only)
uint64_t g_convertedLogged = 0, g_leftLogged = 0;
bool g_firstLogged = false;
// self-test: values used instead of the console variables when set
const int32_t* g_testHdr = nullptr;
const int32_t* g_testDevice = nullptr;
const float* g_testUi = nullptr;

void** VTable(void* obj) { return *static_cast<void***>(obj); }

void* FindCvar(const wchar_t* name)
{
    void* const manager = g_manager ? *g_manager : nullptr;
    if (!manager) return nullptr;
    return reinterpret_cast<FindCvarFn>(VTable(manager)[kFindVar / sizeof(void*)])(manager, name, false);
}

// The output state: HDR10 on and the UI's white in nits; false when SDR, another HDR encoding or unknown.
bool Hdr10(float* uiNits, int32_t* deviceOut)
{
    int32_t hdr = 0, device = 0;
    float ui = 0;
    if (g_testHdr) {
        hdr = *g_testHdr;
        device = *g_testDevice;
        ui = *g_testUi;
    } else {
        static void* cvHdr = nullptr;
        static void* cvDevice = nullptr;
        static void* cvUi = nullptr;
        if (!cvHdr) cvHdr = FindCvar(kHdrOut);
        if (!cvDevice) cvDevice = FindCvar(kDevice);
        if (!cvUi) cvUi = FindCvar(kUiLum);
        if (!cvHdr || !cvDevice || !cvUi) return false;
        const int32_t* const h = reinterpret_cast<AsIntFn>(VTable(cvHdr)[kAsInt / sizeof(void*)])(cvHdr);
        const int32_t* const d = reinterpret_cast<AsIntFn>(VTable(cvDevice)[kAsInt / sizeof(void*)])(cvDevice);
        if (!h || !d) return false;
        hdr = h[0];
        device = d[0];
        ui = reinterpret_cast<GetFloatFn>(VTable(cvUi)[kGetFloat / sizeof(void*)])(cvUi);
    }
    *deviceOut = device;
    *uiNits = ui;
    return hdr != 0 && (device == 3 || device == 4) && ui >= 50.f && ui <= 2000.f;
}

double PqToNits(double e)
{
    constexpr double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const double p = std::pow(std::clamp(e, 0.0, 1.0), 1.0 / m2);
    return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

double SrgbEncode(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
}

double SrgbDecode(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

// The console manager, from the game's own lookup of r.HDR.UI.Luminance (the name's one whole copy in .rdata); null when
// not found or when two lookups disagree (*lookups = -1).
void** FindConsoleManager(int* lookups)
{
    const GameModule& game = Game();
    void** manager = nullptr;
    *lookups = 0;
    for (uint8_t* p : FindAll(game.text, ParsePattern(kLookup))) {
        const auto* const name = reinterpret_cast<const wchar_t*>(RipTarget(p + kLookupName, 3, 7));
        if (!game.rdata.Contains(name) || wcscmp(name, kUiLum) != 0) continue;
        uint8_t* const m1 = RipTarget(p + kLookupLoad1, 3, 7);
        uint8_t* const m2 = RipTarget(p + kLookupLoad2, 3, 7);
        if (m1 != m2 || !game.data.Contains(m1) || (reinterpret_cast<uintptr_t>(m1) & 7)) continue;
        if (manager && manager != reinterpret_cast<void**>(m1)) { *lookups = -1; return nullptr; }
        manager = reinterpret_cast<void**>(m1);
        ++*lookups;
    }
    return manager;
}

// The colour bytes of n FColor pixels (B, G, R; alpha kept) through the table.
void ConvertRange(uint8_t* px, int64_t n, const uint8_t* table)
{
    for (int64_t i = 0; i < n; ++i) {
        uint8_t* const p = px + i * 4;
        p[0] = table[p[0]];
        p[1] = table[p[1]];
        p[2] = table[p[2]];
    }
}

// ... in bands of kBandPixels on ScreenshotParallel's helpers when that fix is on and the picture is big enough (else
// all here): every pixel on its own, so the bytes are the same either way (5120x2160 on one thread: ~5.8 ms).
constexpr int64_t kBandPixels = 4096;
void ConvertPixels(uint8_t* px, int64_t n, const uint8_t (&table)[256])
{
    struct Ctx { uint8_t* px; int64_t n; const uint8_t* table; } const ctx{px, n, table};
    const uint32_t bands = static_cast<uint32_t>((n + kBandPixels - 1) / kBandPixels);
    ScreenshotParallelRows(bands, static_cast<size_t>(n) * 4, [](const void* c, uint32_t b0, uint32_t b1) {
        const auto* x = static_cast<const Ctx*>(c);
        const int64_t from = static_cast<int64_t>(b0) * kBandPixels, to = std::min<int64_t>(static_cast<int64_t>(b1) * kBandPixels, x->n);
        ConvertRange(x->px + from * 4, to - from, x->table);
    }, &ctx);
}

// A rel32 call or jump at `site` pointed at `stub`: one atomic qword write when the rel32 fits in one aligned qword,
// otherwise only before the game's code runs.
bool RedirectRel32(uint8_t* site, uint8_t* stub, bool beforeGameCode)
{
    const int64_t rel = stub - (site + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t insn[5] = {site[0]};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(insn + 1, &rel32, sizeof rel32);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(site) + 1, qword = lo & ~uintptr_t{7};
    if (lo + 3 < qword + 8) {
        auto* q = reinterpret_cast<volatile LONG64*>(qword);
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(qword), 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        const LONG64 before = *q;
        LONG64 after = before;
        memcpy(reinterpret_cast<uint8_t*>(&after) + (lo - qword), insn + 1, 4);
        const bool ok = InterlockedCompareExchange64(q, after, before) == before;
        VirtualProtect(reinterpret_cast<void*>(qword), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(qword), 8);
        return ok;
    }
    return beforeGameCode && WriteCode(site, insn);
}

}  // namespace

// &GConsoleManager for HdrMenuRows' HudAspectRow (hdr_menu.cpp): found once from the game's lookup above; null when not
// found. Monitor thread.
void** HdrConsoleManager()
{
    static void** manager = nullptr;
    static bool searched = false;
    if (!searched) {
        int lookups = 0;
        manager = g_manager ? g_manager : FindConsoleManager(&lookups);
        searched = true;
    }
    return manager;
}

// The table: a byte of the screenshot (the PQ value sRGB-encoded) -> the 8-bit sRGB code the UI shows at the same nits
// (relative to its white uiNits), highlights above 80 % of the UI's white rolled off towards it.
void HdrMenuBackgroundTable(float uiNits, uint8_t (&table)[256])
{
    constexpr double knee = 0.8;
    for (int c = 0; c < 256; ++c) {
        double x = PqToNits(SrgbDecode(c / 255.0)) / uiNits;
        if (x > knee) x = knee + (1 - knee) * (1 - std::exp(-(x - knee) / (1 - knee)));
        table[c] = static_cast<uint8_t>(std::lround(255.0 * SrgbEncode(x)));
    }
}

namespace {

void ResizeWrapper(int32_t srcW, int32_t srcH, const ColorArray* src, int32_t dstW, int32_t dstH, ColorArray* dst, bool linear,
                   bool forceOpaque)
{
    g_original(srcW, srcH, src, dstW, dstH, dst, linear, forceOpaque);
    if (!g_on.load(std::memory_order_relaxed)) return;
    float ui = 0;
    int32_t device = 0;
    if (!dst || !dst->data || dstW <= 0 || dstH <= 0 || !Hdr10(&ui, &device)) {
        g_left.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int64_t n = std::min<int64_t>(static_cast<int64_t>(dstW) * dstH, dst->num);
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    uint8_t table[256];
    HdrMenuBackgroundTable(ui, table);
    ConvertPixels(reinterpret_cast<uint8_t*>(dst->data), n, table);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
    g_ms.store(g_ms.load(std::memory_order_relaxed) + ms, std::memory_order_relaxed);
    g_converted.fetch_add(1, std::memory_order_relaxed);
    if (!g_firstLogged) {
        g_firstLogged = true;
        Log("[hdrbg] menu background converted from HDR10 (PQ) to the UI's sRGB: %dx%d, UI white %.0f nits, output device %d, "
            "%.2f ms",
            dstW, dstH, ui, device, ms);
    }
}

}  // namespace

bool ApplyHdrMenuBackground(bool beforeGameCode)
{
    if (g_call) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kCallSite));
    if (hits.size() != 1) {
        Log("[hdrbg] the menu screenshot's ImageResize call not found (%zu matches): HdrMenuBackground not installed", hits.size());
        return false;
    }
    uint8_t* const call = hits[0] + kCallOffset;
    uint8_t* const target = RipTarget(call, 1, 5);
    if (!game.text.Contains(target) || !MatchesAt(target, ParsePattern(kResizeEntry))) {
        Log("[hdrbg] the call at exe+0x%llX does not go to the expected ImageResize: HdrMenuBackground not installed",
            static_cast<unsigned long long>(call - game.base));
        return false;
    }
    int lookups = 0;
    void** const manager = FindConsoleManager(&lookups);
    if (!manager) {
        Log("[hdrbg] the console manager (from the lookup of r.HDR.UI.Luminance) not found (%d): HdrMenuBackground not installed",
            lookups);
        return false;
    }
    constexpr size_t kRegion = 0x1000;
    uint8_t* const region = AllocNearGame(kRegion);
    if (!region) {
        Log("[hdrbg] no free memory near the game's code for the call stub: HdrMenuBackground not installed");
        return false;
    }
    uint8_t* const stub = region;
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&ResizeWrapper);
    memcpy(stub, jmp, sizeof jmp);
    memcpy(stub + sizeof jmp, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, kRegion, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 14);
    const int64_t rel = stub - (call + 5);
    if (rel != static_cast<int32_t>(rel)) {
        VirtualFree(region, 0, MEM_RELEASE);
        Log("[hdrbg] call stub out of reach: HdrMenuBackground not installed");
        return false;
    }
    g_original = reinterpret_cast<ResizeFn>(target);
    g_manager = manager;
    uint8_t insn[5] = {0xE8};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(insn + 1, &rel32, sizeof rel32);
    // the rel32 in one aligned qword: one atomic write; otherwise only before the game's code runs
    const uintptr_t lo = reinterpret_cast<uintptr_t>(call) + 1, qword = lo & ~uintptr_t{7};
    bool ok = false;
    if (lo + 3 < qword + 8) {
        auto* q = reinterpret_cast<volatile LONG64*>(qword);
        if (VirtualProtect(reinterpret_cast<void*>(qword), 8, PAGE_EXECUTE_READWRITE, &old)) {
            const LONG64 before = *q;
            LONG64 after = before;
            memcpy(reinterpret_cast<uint8_t*>(&after) + (lo - qword), insn + 1, 4);
            ok = InterlockedCompareExchange64(q, after, before) == before;
            VirtualProtect(reinterpret_cast<void*>(qword), 8, old, &old);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(qword), 8);
        }
    } else if (beforeGameCode) {
        ok = WriteCode(call, insn);
    } else {
        Log("[hdrbg] the call cannot be switched atomically while the game runs: HdrMenuBackground not installed");
    }
    if (!ok) {
        VirtualFree(region, 0, MEM_RELEASE);
        g_original = nullptr;
        g_manager = nullptr;
        Log("[hdrbg] writing the call at exe+0x%llX failed: HdrMenuBackground not installed",
            static_cast<unsigned long long>(call - game.base));
        return false;
    }
    g_call = call;
    g_stub = stub;
    Log("[hdrbg] the menu screenshot's ImageResize (exe+0x%llX) called at exe+0x%llX through HdrMenuBackground; console manager "
        "at exe+0x%llX",
        static_cast<unsigned long long>(target - game.base), static_cast<unsigned long long>(call - game.base),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(manager) - game.base));
    return true;
}

void SetHdrMenuBackground(bool on)
{
    if (!g_call) return;
    if (g_on.exchange(on) != on) Log("[hdrbg] HdrMenuBackground %s", on ? "on (HDR10 only)" : "off");
}

void LogHdrMenuBackgroundStats()
{
    const uint64_t c = g_converted.load(), l = g_left.load();
    if (!g_call || (c == g_convertedLogged && l == g_leftLogged)) return;
    g_convertedLogged = c;
    g_leftLogged = l;
    Log("[hdrbg] menu backgrounds converted from HDR10: %llu (%.2f ms each on average); left as they were (SDR or not HDR10): %llu",
        static_cast<unsigned long long>(c), c ? g_ms.load() / static_cast<double>(c) : 0.0, static_cast<unsigned long long>(l));
}

bool HdrMenuBackgroundInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook, void*** manager)
{
    if (!g_call) return false;
    *call = g_call;
    *original = reinterpret_cast<uint8_t*>(g_original);
    *stub = g_stub;
    *hook = reinterpret_cast<void*>(&ResizeWrapper);
    *manager = g_manager;
    return true;
}

void HdrMenuBackgroundTestSetup(void* original, const int32_t* hdr, const int32_t* device, const float* uiNits)
{
    g_original = reinterpret_cast<ResizeFn>(original);
    g_testHdr = hdr;
    g_testDevice = device;
    g_testUi = uiNits;
}

// ---------------------------------------------------------------- HdrSaveThumbnail
//
// A save's thumbnail is the same screenshot (the same broadcast, the same bytes: the PQ value sRGB-encoded in HDR10), so
// in HDR the load menu shows it as washed out as the menu background. Every save's picture passes through one copy: the
// save's screenshot callback (exe+0x48AF8A0 in build 19115871) copies the pixels into its holder (TArray<FColor>
// copy-assign, called at +0x3A) under the save's lock, and the JPEG task reads that holder. The callback runs from the
// screenshot broadcast (or from SaveThumbnailAsync's helper, which calls it with the same arguments) and, for a save made
// in a menu, directly with the save system's cached screenshot (the cache, +0x1B4 / +0x1B8 / +0x1C0 of UVAltarSaveSystem,
// keeps the raw bytes; the save function hands it to the callback). That call is redirected to a wrapper that does the
// copy and then maps the holder's colour bytes through HdrMenuBackground's table, while the switch is on and the output
// is HDR10. The cache, the menu background's own copy and SDR are left alone, so no picture is converted twice. Pictures
// already in saves keep how they were made.
namespace {

// The save's screenshot callback: holder W/H, the copy under the lock (rcx = holder+8, rdx = the pixels), Remove(...)
constexpr const char* kSaveCallback =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 89 54 24 30 48 8B F9 44 89 44 24 34 49 8B D9 48 8B 44 24 30 48 89 01 "
    "48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8D 4F 08 48 8B D3 E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ??";
constexpr size_t kThumbCopyCall = 0x3A;

using CopyFn = void* (*)(void* dst, const void* src);
uint8_t* g_thumbCall = nullptr;
uint8_t* g_thumbStub = nullptr;
CopyFn g_copy = nullptr;
std::atomic<bool> g_thumbOn{false};
std::atomic<uint64_t> g_thumbConverted{0}, g_thumbLeft{0};
std::atomic<double> g_thumbMs{0};
uint64_t g_thumbConvertedLogged = 0, g_thumbLeftLogged = 0;
std::atomic<bool> g_thumbFirstLogged{false};

void* CopyWrapper(void* dst, const void* src)
{
    void* const result = g_copy(dst, src);
    if (!g_thumbOn.load(std::memory_order_relaxed)) return result;
    auto* const arr = static_cast<ColorArray*>(dst);
    float ui = 0;
    int32_t device = 0;
    if (!arr || !arr->data || arr->num <= 0 || !Hdr10(&ui, &device)) {
        g_thumbLeft.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    uint8_t table[256];
    HdrMenuBackgroundTable(ui, table);
    ConvertPixels(reinterpret_cast<uint8_t*>(arr->data), arr->num, table);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
    g_thumbMs.store(g_thumbMs.load(std::memory_order_relaxed) + ms, std::memory_order_relaxed);
    g_thumbConverted.fetch_add(1, std::memory_order_relaxed);
    if (!g_thumbFirstLogged.exchange(true))
        Log("[hdrthumb] save thumbnail converted from HDR10 (PQ) to sRGB: %d pixels, UI white %.0f nits, output device %d, %.2f ms",
            arr->num, ui, device, ms);
    return result;
}

}  // namespace

bool ApplyHdrSaveThumbnail(bool beforeGameCode)
{
    if (g_thumbCall) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kSaveCallback));
    if (hits.size() != 1) {
        Log("[hdrthumb] the save's screenshot callback not found (%zu matches): HdrSaveThumbnail not installed", hits.size());
        return false;
    }
    uint8_t* const call = hits[0] + kThumbCopyCall;
    uint8_t* const target = RipTarget(call, 1, 5);
    if (call[0] != 0xE8 || !game.text.Contains(target)) {
        Log("[hdrthumb] the callback's copy at exe+0x%llX is not a call into the game's code: HdrSaveThumbnail not installed",
            static_cast<unsigned long long>(call - game.base));
        return false;
    }
    int lookups = 0;
    void** const manager = g_manager ? g_manager : FindConsoleManager(&lookups);
    if (!manager) {
        Log("[hdrthumb] the console manager (from the lookup of r.HDR.UI.Luminance) not found (%d): HdrSaveThumbnail not installed",
            lookups);
        return false;
    }
    constexpr size_t kRegion = 0x1000;
    uint8_t* const region = AllocNearGame(kRegion);
    if (!region) {
        Log("[hdrthumb] no free memory near the game's code for the call stub: HdrSaveThumbnail not installed");
        return false;
    }
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&CopyWrapper);
    memcpy(region, jmp, sizeof jmp);
    memcpy(region + sizeof jmp, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, kRegion, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 14);
    g_copy = reinterpret_cast<CopyFn>(target);
    if (!g_manager) g_manager = manager;
    if (!RedirectRel32(call, region, beforeGameCode)) {
        VirtualFree(region, 0, MEM_RELEASE);
        g_copy = nullptr;
        Log("[hdrthumb] writing the call at exe+0x%llX failed: HdrSaveThumbnail not installed",
            static_cast<unsigned long long>(call - game.base));
        return false;
    }
    g_thumbCall = call;
    g_thumbStub = region;
    Log("[hdrthumb] the save thumbnail's copy (exe+0x%llX) in the save's screenshot callback (exe+0x%llX) called at exe+0x%llX "
        "through HdrSaveThumbnail",
        static_cast<unsigned long long>(target - game.base), static_cast<unsigned long long>(hits[0] - game.base),
        static_cast<unsigned long long>(call - game.base));
    return true;
}

void SetHdrSaveThumbnail(bool on)
{
    if (!g_thumbCall) return;
    if (g_thumbOn.exchange(on) != on) Log("[hdrthumb] HdrSaveThumbnail %s", on ? "on (HDR10 only)" : "off");
}

void LogHdrSaveThumbnailStats()
{
    const uint64_t c = g_thumbConverted.load(), l = g_thumbLeft.load();
    if (!g_thumbCall || (c == g_thumbConvertedLogged && l == g_thumbLeftLogged)) return;
    g_thumbConvertedLogged = c;
    g_thumbLeftLogged = l;
    Log("[hdrthumb] save thumbnails converted from HDR10: %llu (%.2f ms each on average); left as they were (SDR or not HDR10): %llu",
        static_cast<unsigned long long>(c), c ? g_thumbMs.load() / static_cast<double>(c) : 0.0, static_cast<unsigned long long>(l));
}

uint8_t* HdrSaveThumbnailResolve(uint8_t* p)
{
    return (g_thumbStub && p == g_thumbStub) ? reinterpret_cast<uint8_t*>(g_copy) : p;
}

bool HdrSaveThumbnailInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook)
{
    if (!g_thumbCall) return false;
    *call = g_thumbCall;
    *original = reinterpret_cast<uint8_t*>(g_copy);
    *stub = g_thumbStub;
    *hook = reinterpret_cast<void*>(&CopyWrapper);
    return true;
}

void HdrSaveThumbnailTestSetup(void* original)
{
    g_copy = reinterpret_cast<CopyFn>(original);
}

}  // namespace opf
