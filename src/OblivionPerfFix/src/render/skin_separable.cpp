// SeparableSkinAbove4K (game bug fix, on by default since 1.2.4: user 2026-10-31): skin scattering by the separable method while the game
// renders wider than a 4K screen (more than 3840 pixels across: an ultrawide at DLSS DLAA or FSR Native AA), the
// game's Burley method otherwise.
//
// The problem (a game bug): with Effects Quality Ultra (skin scattering at full resolution)
// and a 5120-wide render (5120x2160 here; 5120x1440 in a 2025 Steam report), sunlit characters show glowing magenta
// specks on skin and cloth edges (a skirt's metal studs, a hand), in the stock game too and with FSR as well as DLSS.
// Bisected in play with Engine.ini lines: only r.SSS.HalfRes=1 (flicker) or r.SSS.Burley.Quality=0 (the separable
// method instead of Burley) remove them; Burley's sample count, filters, profile cache and mip shortcut do not. A
// 3413-wide render (DLSS Quality at 5120x2160) never showed them, nor a 4K 16:9 screen at DLAA (3840x2160, in play,
// by eye): something in the render's width, not 100 %. The width rule (over 3840) is a choice, not a measured edge.
//
// What it does: once per second from the DLL's monitor, the render width (the game window's client width x
// r.ScreenPercentage / 100) is compared with 3840; above it r.SSS.Burley.Quality is set to 0, at or below it the value
// it replaced is put back. The variable is read and written through the game's console manager (GConsoleManager,
// found from the game's own lookups of r.HDR.UI.Luminance: FindConsoleVariable at vtable+0xB8, AsVariableInt at +0x58
// for the int's two copies, game thread's and render thread's, both written; GetFloat at +0x98 for r.ScreenPercentage). The
// window is the process's visible UnrealWindow (user32 functions looked up at run time: the DLL links kernel32 only).
// r.SSS.Burley.Quality is not part of any graphics preset, so nothing in the game writes it back. A 0 that was already
// there (an Engine.ini line) is left alone. A change of the upscaler mode or window size is followed within a second.
//
// Changes the picture on purpose (skin shaded by the separable method: measured at a save in play, the skin looked the
// same at play distance, about 0.2 ms less GPU time at 5120x2160; dialogue close-ups not compared). On by default (user 2026-10-31);
// live.
#include "common.h"

namespace opf {
namespace {

constexpr wchar_t kBurley[] = L"r.SSS.Burley.Quality";
constexpr wchar_t kPercent[] = L"r.ScreenPercentage";
constexpr uint32_t kFindVar = 0xB8, kAsInt = 0x58, kGetFloat = 0x98;
constexpr double kMaxWidth = 3840.0;  // a 4K screen's width: wider renders get the separable method
constexpr int kTries = 300;           // seconds to wait for the two variables to be registered
constexpr int kSwitchLogLines = 10;   // switches logged one by one, then only counted

// The game's lookup of r.HDR.UI.Luminance: rcx = GConsoleManager (made first if null), FindConsoleVariable(name, true)
constexpr const char* kLookup =
    "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0C E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8D 15 ?? ?? ?? ?? 41 B0 01 FF 90 B8 00 00 00";
constexpr size_t kLookupLoad1 = 0, kLookupLoad2 = 17, kLookupName = 27;
constexpr wchar_t kUiLum[] = L"r.HDR.UI.Luminance";

using FindCvarFn = void* (*)(void* manager, const wchar_t* name, bool trackFrequentCalls);
using AsIntFn = int32_t* (*)(void* cvar);
using GetFloatFn = float (*)(void* cvar);
using EnumWindowsFn = BOOL(WINAPI*)(WNDENUMPROC, LPARAM);
using GetWindowThreadProcessIdFn = DWORD(WINAPI*)(HWND, LPDWORD);
using GetClassNameWFn = int(WINAPI*)(HWND, LPWSTR, int);
using IsWindowVisibleFn = BOOL(WINAPI*)(HWND);
using GetClientRectFn = BOOL(WINAPI*)(HWND, LPRECT);

int32_t* g_burley = nullptr;  // TConsoleVariableData<int32>: [0] game thread, [1] render thread
void* g_percent = nullptr;
const float* g_testPercent = nullptr;  // self-test: read instead of r.ScreenPercentage
const int* g_testWindow = nullptr;     // self-test: read instead of the window's width
int g_tries = 0;
bool g_gaveUp = false;
bool g_ours = false;    // the value 0 was written by this fix
int32_t g_replaced = 1; // the value it replaced
long g_switches = 0;

struct User32 {
    EnumWindowsFn enumWindows = nullptr;
    GetWindowThreadProcessIdFn windowPid = nullptr;
    GetClassNameWFn className = nullptr;
    IsWindowVisibleFn visible = nullptr;
    GetClientRectFn clientRect = nullptr;
} g_user32;
HWND g_window = nullptr;

void** VTable(void* obj) { return *static_cast<void***>(obj); }

bool LoadUser32()
{
    if (g_user32.clientRect) return true;
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return false;
    g_user32.enumWindows = reinterpret_cast<EnumWindowsFn>(GetProcAddress(u, "EnumWindows"));
    g_user32.windowPid = reinterpret_cast<GetWindowThreadProcessIdFn>(GetProcAddress(u, "GetWindowThreadProcessId"));
    g_user32.className = reinterpret_cast<GetClassNameWFn>(GetProcAddress(u, "GetClassNameW"));
    g_user32.visible = reinterpret_cast<IsWindowVisibleFn>(GetProcAddress(u, "IsWindowVisible"));
    g_user32.clientRect = reinterpret_cast<GetClientRectFn>(GetProcAddress(u, "GetClientRect"));
    return g_user32.enumWindows && g_user32.windowPid && g_user32.className && g_user32.visible && g_user32.clientRect;
}

BOOL CALLBACK PickWindow(HWND w, LPARAM best)
{
    DWORD pid = 0;
    g_user32.windowPid(w, &pid);
    if (pid != GetCurrentProcessId() || !g_user32.visible(w)) return TRUE;
    wchar_t cls[32];
    if (g_user32.className(w, cls, 32) <= 0 || wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
    RECT r{}, rb{};
    HWND* const out = reinterpret_cast<HWND*>(best);
    if (!g_user32.clientRect(w, &r)) return TRUE;
    if (!*out || (g_user32.clientRect(*out, &rb) && (r.right - r.left) * (r.bottom - r.top) > (rb.right - rb.left) * (rb.bottom - rb.top)))
        *out = w;
    return TRUE;
}

// The game window's client width, 0 when unknown.
int WindowWidth()
{
    if (g_testWindow) return *g_testWindow;
    if (!LoadUser32()) return 0;
    RECT r{};
    if (!g_window || !g_user32.clientRect(g_window, &r) || r.right <= r.left) {
        HWND best = nullptr;
        g_user32.enumWindows(PickWindow, reinterpret_cast<LPARAM>(&best));
        g_window = best;
        if (!g_window || !g_user32.clientRect(g_window, &r)) return 0;
    }
    return r.right - r.left;
}

// &GConsoleManager, from the game's own lookups of r.HDR.UI.Luminance (the name's one whole copy in .rdata, both loads
// of the pointer the same 8-byte aligned .data slot); null when not found or when two lookups disagree. Searched once.
void** GameConsoleManager()
{
    static void** manager = nullptr;
    static bool searched = false;
    if (searched) return manager;
    searched = true;
    const GameModule& game = Game();
    void** found = nullptr;
    for (uint8_t* p : FindAll(game.text, ParsePattern(kLookup))) {
        const auto* const name = reinterpret_cast<const wchar_t*>(RipTarget(p + kLookupName, 3, 7));
        if (!game.rdata.Contains(name) || wcscmp(name, kUiLum) != 0) continue;
        uint8_t* const m1 = RipTarget(p + kLookupLoad1, 3, 7);
        uint8_t* const m2 = RipTarget(p + kLookupLoad2, 3, 7);
        if (m1 != m2 || !game.data.Contains(m1) || (reinterpret_cast<uintptr_t>(m1) & 7)) continue;
        if (found && found != reinterpret_cast<void**>(m1)) return nullptr;
        found = reinterpret_cast<void**>(m1);
    }
    manager = found;
    return manager;
}

bool Find()
{
    if (g_burley && (g_percent || g_testPercent)) return true;
    if (g_gaveUp) return false;
    void** const managerSlot = GameConsoleManager();
    void* const manager = managerSlot ? *managerSlot : nullptr;
    if (manager) {
        const auto find = reinterpret_cast<FindCvarFn>(VTable(manager)[kFindVar / sizeof(void*)]);
        void* const burley = find(manager, kBurley, false);
        void* const percent = find(manager, kPercent, false);
        if (burley && percent) {
            int32_t* const data = reinterpret_cast<AsIntFn>(VTable(burley)[kAsInt / sizeof(void*)])(burley);
            if (!data) {
                Log("[skinsss] r.SSS.Burley.Quality is not an int variable here: SeparableSkinAbove4K unavailable");
                g_gaveUp = true;
                return false;
            }
            g_burley = data;
            g_percent = percent;
            Log("[skinsss] r.SSS.Burley.Quality (now %d) and r.ScreenPercentage found through the console manager", data[0]);
            return true;
        }
    }
    if (++g_tries >= kTries) {
        Log("[skinsss] %s: SeparableSkinAbove4K unavailable",
            manager ? "r.SSS.Burley.Quality or r.ScreenPercentage not registered after 5 min" : "the console manager not found");
        g_gaveUp = true;
    }
    return false;
}

void Write(int32_t v)
{
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_burley[0]), v);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_burley[1]), v);
}

void LogSwitch(const char* what, double width)
{
    if (++g_switches <= kSwitchLogLines)
        Log("[skinsss] %s (render width %.0f)%s", what, width, g_switches == kSwitchLogLines ? "; further switches only counted" : "");
}

}  // namespace

void SetSeparableSkinAbove4K(bool on)
{
    if (!on && !g_ours) return;
    if (!Find()) return;
    const int window = WindowWidth();
    if (on && window <= 0) return;  // no window yet: nothing decided
    const float percent = g_testPercent ? *g_testPercent
                                        : reinterpret_cast<GetFloatFn>(VTable(g_percent)[kGetFloat / sizeof(void*)])(g_percent);
    const double width = window * static_cast<double>(percent) / 100.0;
    if (on && width > kMaxWidth + 0.5) {
        if (g_burley[0] != 0) {
            g_replaced = g_burley[0];
            Write(0);
            g_ours = true;
            LogSwitch("render wider than 4K: skin scattering by the separable method (r.SSS.Burley.Quality 0)", width);
        }
        return;
    }
    if (g_ours) {
        if (g_burley[0] == 0) Write(g_replaced);
        g_ours = false;
        LogSwitch(on ? "render 4K wide or less: Burley skin scattering back" : "switched off: Burley skin scattering back", width);
    }
}

void LogSeparableSkinAbove4KStats()
{
    static long logged = 0;  // monitor thread only (as SetSeparableSkinAbove4K)
    if (g_switches == logged) return;  // only when it changed
    logged = g_switches;
    Log("[skinsss] switches so far %ld; now %s", g_switches, g_ours ? "separable (render wider than 4K)" : "the game's value");
}

void SeparableSkinTestSetup(int32_t* data, const float* percent, const int* windowWidth)
{
    g_burley = data;
    g_testPercent = percent;
    g_testWindow = windowWidth;
    g_percent = nullptr;
    g_ours = false;
    g_replaced = 1;
    g_switches = 0;
    g_gaveUp = false;
}

void** SeparableSkinConsoleManager() { return GameConsoleManager(); }

}  // namespace opf
