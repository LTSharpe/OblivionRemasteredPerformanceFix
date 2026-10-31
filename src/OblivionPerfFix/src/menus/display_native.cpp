// MonitorNativeFromWindows and FullscreenAllResolutions (game bug; MonitorNativeFromWindows on, FullscreenAllResolutions off by default): the Graphics
// page's Display Resolution list for monitors whose EDID does not hold their real native resolution, and a fullscreen
// list with every mode Windows offers.
//
// The engine reads each monitor's native resolution from its EDID's first detailed timing (GetMonitorsInfo, exe+0x1172DB0,
// parses the registry EDID). A detailed timing cannot hold a width above 4095, so monitors wider than that put a smaller
// timing there and the real one in an extension block. On an Alienware AW3926QW (5120x2160) the first timing
// is 3440x1440 (registry EDID, read 2026-10-27), and the engine takes 3440x1440 as native. The game then lists, from that
// value (FUN exe+0x31AB020, called by the page's resolution list exe+0x48EFAC0):
// - borderless: only the native value (the window itself still covers the desktop: 5120x2160 measured);
// - fullscreen: the monitor's modes with exactly the native aspect ratio (1e-8 tolerance) and no wider than native,
//   so only 3440x1440.
//
// MonitorNativeFromWindows: GetMonitorsInfo's one call (in FDisplayMetrics::RebuildDisplayMetrics, exe+0x11840B5) goes
// to a stub that runs it and then sets each monitor's native width and height (FMonitorInfo +0x20 / +0x24) to the mode
// Windows calls the monitor's preferred one (QueryDisplayConfig + DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE,
// the monitor matched by its desktop position, FMonitorInfo's DisplayRect at +0x30). Only when that mode is one of the
// monitor's own modes (its resolution list at +0x58) and differs from the EDID value; anything else is left as it is.
// FullscreenAllResolutions: the fullscreen filter's "not fullscreen: skip" branch (exe+0x31AB49E, jnz) becomes a jump,
// so fullscreen lists every mode of the monitor at least the engine's minimum height (720), sorted, as windowed does
// before its own size limit. Other aspect ratios are then shown by the display or the graphics driver's scaling.
//
// What changes: the resolution lists and the borderless value the Graphics page shows (and so what the player can pick).
// Nothing else. Installed at start-up only, on in the shipped ini.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// FDisplayMetrics::RebuildDisplayMetrics, around GetMonitorsInfo's call (exe+0x11840B5):
//   B9 4F 00 00 00 mov ecx,4Fh / 89 47 30 mov [rdi+30h],eax / FF 15 call [GetSystemMetrics] / 03 47 2C add eax,[rdi+2Ch] /
//   48 8D 4F 08 lea rcx,[rdi+8] (&MonitorInfo) / 89 47 34 mov [rdi+34h],eax / E8 call GetMonitorsInfo /
//   0F 57 C0 xorps / 0F 57 C9 xorps / 0F 11 47 40 movups [rdi+40h]
constexpr char kCallSite[] =
    "B9 4F 00 00 00 89 47 30 FF 15 ?? ?? ?? ?? 03 47 2C 48 8D 4F 08 89 47 34 E8 ?? ?? ?? ?? 0F 57 C0 0F 57 C9 0F 11 47 40";
constexpr size_t kCall = 24;
// GetMonitorsInfo's entry: it references the string "EDID" (DisplayModeCache finds it the same way); checked by the
// call target lying in .text and the first bytes being a prologue (48 89 / 40 55 / 48 8B C4).

// The fullscreen filter (exe+0x31AB49C): test eax,eax (the window mode) / jnz end / mov eax,[rsp+native height] /
// test eax,eax / jle end / mov ebp,[rsi+8] / movd xmm6,r12d (native width) / movd xmm0,eax / cvtdq2ps xmm6,xmm6
constexpr char kFilter[] =
    "85 C0 0F 85 ?? ?? ?? ?? 8B 84 24 ?? ?? ?? ?? 85 C0 0F 8E ?? ?? ?? ?? 8B 6E 08 66 41 0F 6E F4 66 0F 6E C0 0F 5B F6";
constexpr size_t kFilterJnz = 2, kFilterJle = 17;

// FMonitorInfo (UE 5.3 plus the game's resolution list), 0x68 bytes
constexpr size_t kInfoStride = 0x68, kNativeW = 0x20, kNativeH = 0x24, kDisplayRect = 0x30, kResolutions = 0x58;

using GetMonitorsInfoFn = void (*)(void* monitors);

struct TArr {
    uint8_t* data;
    int32_t num, max;
};

GetMonitorsInfoFn g_getMonitorsInfo = nullptr;
uint8_t* g_callSite = nullptr;
uint8_t* g_filter = nullptr;
bool g_nativeInstalled = false, g_allInstalled = false;
std::atomic<long> g_corrected{0};  // game thread writes, the monitor thread logs
long g_correctedLogged = 0;
int g_loggedW = 0, g_loggedH = 0;

bool Fail(const char* what, const char* why)
{
    Log("[display] %s: %s not installed", why, what);
    return false;
}

// Windows' preferred mode of the monitor whose desktop area starts at (x, y); false if none found.
bool PreferredMode(int32_t x, int32_t y, uint32_t* w, uint32_t* h)
{
    using SizesFn = LONG(WINAPI*)(UINT32, UINT32*, UINT32*);
    using QueryFn = LONG(WINAPI*)(UINT32, UINT32*, DISPLAYCONFIG_PATH_INFO*, UINT32*, DISPLAYCONFIG_MODE_INFO*, DISPLAYCONFIG_TOPOLOGY_ID*);
    using InfoFn = LONG(WINAPI*)(DISPLAYCONFIG_DEVICE_INFO_HEADER*);
    static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static const auto sizes = user32 ? reinterpret_cast<SizesFn>(GetProcAddress(user32, "GetDisplayConfigBufferSizes")) : nullptr;
    static const auto query = user32 ? reinterpret_cast<QueryFn>(GetProcAddress(user32, "QueryDisplayConfig")) : nullptr;
    static const auto info = user32 ? reinterpret_cast<InfoFn>(GetProcAddress(user32, "DisplayConfigGetDeviceInfo")) : nullptr;
    if (!sizes || !query || !info) return false;
    UINT32 np = 0, nm = 0;
    if (sizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np || np > 64 || nm > 128) return false;
    DISPLAYCONFIG_PATH_INFO paths[64];
    DISPLAYCONFIG_MODE_INFO modes[128];
    if (query(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, nullptr) != ERROR_SUCCESS) return false;
    for (UINT32 i = 0; i < np; ++i) {
        const UINT32 sm = paths[i].sourceInfo.modeInfoIdx;
        if (sm >= nm || modes[sm].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        const POINTL pos = modes[sm].sourceMode.position;
        if (pos.x != x || pos.y != y) continue;
        DISPLAYCONFIG_TARGET_PREFERRED_MODE pref{};
        pref.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;
        pref.header.size = sizeof pref;
        pref.header.adapterId = paths[i].targetInfo.adapterId;
        pref.header.id = paths[i].targetInfo.id;
        if (info(&pref.header) != ERROR_SUCCESS || !pref.width || !pref.height) return false;
        *w = pref.width;
        *h = pref.height;
        return true;
    }
    return false;
}

// The stub's target: GetMonitorsInfo as the game calls it, then each monitor's native size from Windows.
void GetMonitorsInfoHook(void* monitors)
{
    g_getMonitorsInfo(monitors);
    TArr a;
    memcpy(&a, monitors, sizeof a);
    if (!a.data || a.num <= 0 || a.num > 16) return;
    for (int32_t m = 0; m < a.num; ++m) {
        uint8_t* const mi = a.data + static_cast<size_t>(m) * kInfoStride;
        int32_t nw, nh, left, top;
        memcpy(&nw, mi + kNativeW, 4);
        memcpy(&nh, mi + kNativeH, 4);
        memcpy(&left, mi + kDisplayRect, 4);
        memcpy(&top, mi + kDisplayRect + 4, 4);
        uint32_t pw = 0, ph = 0;
        if (!PreferredMode(left, top, &pw, &ph) || (static_cast<int32_t>(pw) == nw && static_cast<int32_t>(ph) == nh)) continue;
        // the preferred mode must be one of the monitor's own modes (its resolution list: {width, height} pairs)
        TArr res;
        memcpy(&res, mi + kResolutions, sizeof res);
        bool listed = false;
        for (int32_t r = 0; res.data && r < res.num && r < 4096 && !listed; ++r) {
            int32_t rw, rh;
            memcpy(&rw, res.data + static_cast<size_t>(r) * 8, 4);
            memcpy(&rh, res.data + static_cast<size_t>(r) * 8 + 4, 4);
            listed = rw == static_cast<int32_t>(pw) && rh == static_cast<int32_t>(ph);
        }
        if (!listed) continue;
        const int32_t w = static_cast<int32_t>(pw), h = static_cast<int32_t>(ph);
        memcpy(mi + kNativeW, &w, 4);
        memcpy(mi + kNativeH, &h, 4);
        ++g_corrected;
        if (g_loggedW != w || g_loggedH != h) {
            g_loggedW = w;
            g_loggedH = h;
            Log("[display] monitor %d at %d,%d: native %dx%d from its EDID, %dx%d from Windows' preferred mode: Windows' used", m,
                left, top, nw, nh, w, h);
        }
    }
}

uint8_t* JumpStub(const void* target)
{
    uint8_t* const stub = AllocNearGame(32);
    if (!stub) return nullptr;
    memset(stub, 0xCC, 32);
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]; dq target
    memcpy(stub, jmp, sizeof jmp);
    memcpy(stub + 6, &target, 8);
    DWORD old;
    if (!VirtualProtect(stub, 32, PAGE_EXECUTE_READ, &old)) return nullptr;
    FlushInstructionCache(GetCurrentProcess(), stub, 32);
    return stub;
}

}  // namespace

bool FindDisplayNative(DisplayNativeInfoOut* out)
{
    const GameModule& game = Game();
    const auto sites = FindAll(game.text, ParsePattern(kCallSite));
    const auto filters = FindAll(game.text, ParsePattern(kFilter));
    if (out) *out = DisplayNativeInfoOut{};
    if (sites.size() == 1) {
        uint8_t* const fn = RipTarget(sites[0] + kCall, 1, 5);
        if (game.text.Contains(fn) && out) {
            out->callSite = sites[0] + kCall;
            out->getMonitorsInfo = fn;
        }
    }
    if (filters.size() == 1 && RipTarget(filters[0] + kFilterJnz, 2, 6) == RipTarget(filters[0] + kFilterJle, 2, 6) && out)
        out->filterJump = filters[0] + kFilterJnz;
    return sites.size() == 1 || filters.size() == 1;
}

bool InstallMonitorNativeFromWindows()
{
    if (g_nativeInstalled) return true;
    DisplayNativeInfoOut i{};
    FindDisplayNative(&i);
    if (!i.callSite) return Fail("MonitorNativeFromWindows", "RebuildDisplayMetrics' GetMonitorsInfo call not found");
    uint8_t* const stub = JumpStub(reinterpret_cast<const void*>(&GetMonitorsInfoHook));
    if (!stub) return Fail("MonitorNativeFromWindows", "no memory near the game's code");
    const int64_t rel = stub - (i.callSite + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("MonitorNativeFromWindows", "the stub is out of reach");
    g_getMonitorsInfo = reinterpret_cast<GetMonitorsInfoFn>(i.getMonitorsInfo);
    uint8_t patch[5] = {0xE8, 0, 0, 0, 0};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(patch + 1, &rel32, 4);
    if (!WriteCode(i.callSite, patch)) return Fail("MonitorNativeFromWindows", "the call could not be redirected");
    g_callSite = i.callSite;
    g_nativeInstalled = true;
    Log("[display] MonitorNativeFromWindows: GetMonitorsInfo's call exe+0x%llX redirected (GetMonitorsInfo exe+0x%llX): monitors' "
        "native resolution from Windows' preferred mode",
        static_cast<unsigned long long>(i.callSite - Game().base), static_cast<unsigned long long>(i.getMonitorsInfo - Game().base));
    return true;
}

bool InstallFullscreenAllResolutions()
{
    if (g_allInstalled) return true;
    DisplayNativeInfoOut i{};
    FindDisplayNative(&i);
    if (!i.filterJump) return Fail("FullscreenAllResolutions", "the fullscreen resolution filter not found");
    // jnz rel32 (0F 85, 6 bytes) -> jmp rel32 (E9, 5 bytes) to the same end + nop
    uint8_t* const end = RipTarget(i.filterJump, 2, 6);
    const int64_t rel = end - (i.filterJump + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("FullscreenAllResolutions", "unexpected jump distance");
    uint8_t patch[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(patch + 1, &rel32, 4);
    if (!WriteCode(i.filterJump, patch)) return Fail("FullscreenAllResolutions", "the filter could not be patched");
    g_filter = i.filterJump;
    g_allInstalled = true;
    Log("[display] FullscreenAllResolutions: the fullscreen resolution filter at exe+0x%llX skipped: every mode of the monitor is "
        "listed", static_cast<unsigned long long>(i.filterJump - Game().base));
    return true;
}

void LogDisplayNativeStats()
{
    const long n = g_corrected.load(std::memory_order_relaxed);
    if (!g_nativeInstalled || n == g_correctedLogged) return;  // only when it changed (not once a minute for the session)
    g_correctedLogged = n;
    Log("[display] native resolutions corrected: %ld time(s)", n);
}

}  // namespace opf
