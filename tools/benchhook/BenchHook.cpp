// OblvBenchHook.dll: benchmark tooling only, never shipped with the fix.
//
// Keeps an unattended benchmark game out of the PC user's way. tools\BenchLib.cs (GameHider) installs this
// DLL as a thread-specific WH_CBT hook on the game's main thread as soon as the process starts, so Windows
// loads it into that one thread before the game creates its window. Then, while the shared flag says
// "hidden" (the default):
//   - the game's top-level windows are created and kept off-screen (right of the virtual screen): UE
//     auto-centres a new window and re-centres it on every resolution change, and shows it with focus;
//     forcing the position in WM_WINDOWPOSCHANGING means no frame of it is ever drawn on a monitor;
//   - they are tool windows: no taskbar button (nothing to flash orange), not in Alt+Tab; with "capturable" set
//     (Run-Benchmark -Record) the game's main window gets an invisible owner window instead, which keeps it off the
//     taskbar and out of Alt+Tab just the same, because OBS's game capture ignores tool windows when it looks for the
//     game (2026-10-11: a whole take recorded black);
//   - they never become the active window (HCBT_ACTIVATE refused), so the game never takes the keyboard
//     focus and never confines the cursor (UE clips it to the viewport while it has focus); if a refusal leaves
//     no foreground window at all, the PC user's window gets it back on the game thread's next message pump;
//   - they are never minimised (HCBT_MINMAX refused): a game restored from minimised crashes (D3D12
//     CopyCommandList Close E_INVALIDARG, see docs\benchmark.md).
// The window still renders and presents every frame (PresentMon, PrintWindow captures keep working).
// Show-GameWindow (tools\BenchBridge.ps1) sets "show": the window may then be moved on-screen and used.
//
// No crash UI (2026-10-06, after UE's crash reporter stayed on the PC user's screen for a minute), in the game only and
// whatever the "show" flag says, from the first hook call on:
//   - Windows error reporting shows no dialog for the game (SetErrorMode as UE itself does with -unattended,
//     WerSetFlags(WER_FAULT_REPORTING_NO_UI)), and Windows makes no "not responding" ghost window for it;
//   - UE's crash reporter never runs: the exe's import of CreateProcessW goes through this DLL, and a start of
//     CrashReportClient.exe is created suspended and ended before it runs a single instruction. UE has written its
//     crash report folder (Saved\Crashes: crash context, minidump, log) before that point, and it carries on as if the
//     reporter had started (otherwise it would show a "has crashed" message box), so the game ends by itself. The
//     reporter itself would only show the report and offer to upload it (its -Unattended mode uploads without asking,
//     which is why the game is not started with -unattended);
//   - top-level windows that other threads of the game create (a message box from the crash handler, the driver or a
//     mod) are taken over like the main window's (in-context WinEvent hook, EVENT_OBJECT_CREATE, before they are
//     shown), and activation is refused on that thread too. tools\Crash-Guard.ps1 closes such boxes from outside.
// The same DLL hides analysis tools that must not pop up either (e.g. a GPU profiler started by a
// script): in a process other than the game it takes over every top-level window.
//
// Observer mode (2026-10-12, in the game exe only, see "observer mode" below): while the runner sets "observer"
// (spectator mode), NPC AI treats the player as not there: no AI package gets the player as its target, and no NPC can
// start a conversation with the player. Forced greetings do not need the player to be detected, so `tdetect` does not
// stop them, and an open conversation pauses the world for the rest of the scene.
// Watch-mode runs (-Visible) load this DLL too, with "show" set, for this and for the crash UI guard.
//
// Shared state: the file mapping Local\OblvBenchHook (layout below, mirrored in BenchLib.cs). The DLL pins
// itself in the game process, so the window subclass stays valid after the benchmark unhooks.

#include <windows.h>
#include <werapi.h>
#include <intrin.h>
#include <cstring>

namespace {

struct Shared {
    LONG version;             // 1
    LONG show;                // 0 = hidden (default), 1 = the PC user wants to see and use the game
    LONG offX, offY;          // where hidden windows go; 0,0 = just right of the virtual screen
    LONG64 lastHwnd;          // the last game window seen (UnrealWindow)
    LONG created;             // windows taken over
    LONG moved;               // positions forced off-screen
    LONG activationsBlocked;  // activations refused
    LONG minimiseBlocked;     // minimise requests refused
    LONG pid;                 // process the hook runs in
    // since 2026-10-06 (no crash UI); zero in a game that still has an older build of this DLL
    LONG guard;               // what is in place in the game: 1 no WER dialogs, 2 crash reporter blocked, 4 windows of all threads
    LONG crashReportsBlocked; // crash reporter starts refused (the game crashed)
    LONG otherWindows;        // top-level windows of other game threads taken over (message boxes)
    LONG blockedPid;          // the game process whose crash reporter was refused last
    LONG testFlags;           // 1 = let the crash reporter start (to test tools\Crash-Guard.ps1 on its own)
    LONG focusReturned;       // the foreground handed back to the PC user's window after a refused activation left none
    LONG capturable;          // since 2026-10-11: 1 = the main window is owned instead of a tool window (see above)
    WCHAR crashArgs[128];     // the end of the refused crash reporter's command line (crash folder, CrashGUID)
    // since 2026-10-12 (observer mode); zero in a game that still has an older build of this DLL
    LONG observer;            // set by the runner: 1 = NPC AI treats the player as not there (observer mode)
    LONG talkGuard;           // in the game talkGuardPid: 1 = installed; < 0 = not: -1 RTTI, -2 command, -3 slot,
                              // -4 write, -5 Activate slot, -6 SetPackageTarget slot
    LONG talkGuardPid;        // the game process talkGuard is about
    LONG talksRefused;        // conversation starts refused (all games since the mapping was created)
    LONG lastRefusedForm;     // form ID of the NPC refused last
    LONG talkSlot;            // Character's StartConversation slot that was wrapped (+0x670 in build 19115871)
    LONG activateSlot;        // TESNPC's Activate slot that was wrapped (+0x198 in build 19115871)
    LONG lastRefusedCaller;   // exe RVA the refused call came from (where in the game's AI it was tried)
    LONG targetSlot;          // the AI processes' SetPackageTarget slot that was wrapped (+0x1C8 in build 19115871)
    LONG targetsRefused;      // package targets refused (the player as an AI package's target)
    LONG targetCallers[16];   // where in the game's AI the refused targets came from: 8 pairs of { exe RVA, count }
};
static_assert(sizeof(Shared) == 432, "layout is mirrored in tools\\BenchLib.cs");

const wchar_t kMapping[] = L"Local\\OblvBenchHook";
// A hidden tool GUI (RGP, RMV, the Radeon Developer Panel) uses its own mapping, so it never overwrites the game's
// pid and counters while both run, and Show-GameWindow does not show the tool (2026-10-07, worker vramtrace).
const wchar_t kToolMapping[] = L"Local\\OblvBenchHook.Tools";
const wchar_t kProp[] = L"OblvBenchHook.OrigProc";

Shared* g_shared = nullptr;
HMODULE g_module = nullptr;

bool Hidden() { return !g_shared || g_shared->show == 0; }

POINT OffScreen() {
    if (g_shared && (g_shared->offX || g_shared->offY)) return { g_shared->offX, g_shared->offY };
    return { GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64, GetSystemMetrics(SM_YVIRTUALSCREEN) };
}

void Count(LONG Shared::*field) { if (g_shared) InterlockedIncrement(&(g_shared->*field)); }

bool g_gameHost = true;   // set in DllMain: the game, or another tool hidden the same way (Radeon GPU Profiler)
bool g_gameExe = false;   // set in DllMain: the game itself (not Steam's launcher stub, which is hooked as well)

// The foreground window of another process, seen at the last hook call: the PC user's. When the game had the right to
// take the foreground (Steam can pass it on at a launch), refusing its activation leaves no foreground window at all
// (2026-10-06: the PC user's window lost the keyboard focus at a launch until it was clicked). This thread then owns the
// foreground, so it can hand it back: on its next message pump (a thread timer), if still no window has it.
HWND g_userForeground = nullptr;

void NoteForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) g_userForeground = fg;
}

void CALLBACK ReturnForeground(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    HWND to = g_userForeground;
    if (!GetForegroundWindow() && to && IsWindow(to) && SetForegroundWindow(to)) Count(&Shared::focusReturned);
}

// In the game: its main window, and message boxes it may show on errors (the runner closes those itself).
// In a tool (e.g. a GPU profiler run hidden): every top-level window, menus and popups included (Qt clamps
// a popup onto the nearest screen), except the system's IME windows.
bool IsTarget(HWND h) {
    wchar_t cls[64] = {};
    if (!GetClassNameW(h, cls, 64)) return false;
    if (!g_gameHost) return lstrcmpW(cls, L"IME") != 0 && lstrcmpW(cls, L"MSCTFIME UI") != 0;
    return lstrcmpW(cls, L"UnrealWindow") == 0 || lstrcmpW(cls, L"#32770") == 0;
}

void MakeToolWindow(HWND h) {
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    LONG_PTR want = (ex | WS_EX_TOOLWINDOW) & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    if (want != ex) SetWindowLongPtrW(h, GWL_EXSTYLE, want);
}

bool IsUnreal(HWND h) {
    wchar_t cls[64] = {};
    return GetClassNameW(h, cls, 64) && lstrcmpW(cls, L"UnrealWindow") == 0;
}

// "capturable" applies to the game's own window only (message boxes stay tool windows)
bool OwnedMode(HWND h) { return g_gameHost && g_shared && g_shared->capturable && IsUnreal(h); }

// An owned window has no taskbar button and is not in Alt+Tab. The owner is a never-shown popup of the same thread
// (an owner on another thread would join the two threads' input queues).
void MakeOwnedWindow(HWND h) {
    thread_local HWND owner = nullptr;
    if (!owner || !IsWindow(owner))
        owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"OblvBenchHook owner", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_module, nullptr);
    if (owner && GetWindow(h, GW_OWNER) != owner) SetWindowLongPtrW(h, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    LONG_PTR want = ex & ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW | WS_EX_APPWINDOW);
    if (want != ex) SetWindowLongPtrW(h, GWL_EXSTYLE, want);
}

void HideFromTaskbar(HWND h) { if (OwnedMode(h)) MakeOwnedWindow(h); else MakeToolWindow(h); }

LRESULT CALLBACK SubclassProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    WNDPROC orig = reinterpret_cast<WNDPROC>(GetPropW(h, kProp));
    switch (msg) {
    case WM_CREATE: {
        LRESULT r = CallWindowProcW(orig, h, msg, w, l);
        if (Hidden()) {
            HideFromTaskbar(h);
            POINT p = OffScreen();
            SetWindowPos(h, nullptr, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return r;
    }
    case WM_WINDOWPOSCHANGING:
        if (Hidden()) {
            auto* wp = reinterpret_cast<WINDOWPOS*>(l);
            if (!(wp->flags & SWP_NOMOVE)) {
                POINT p = OffScreen();
                if (wp->x != p.x || wp->y != p.y) { wp->x = p.x; wp->y = p.y; Count(&Shared::moved); }
            }
        }
        break;
    case WM_STYLECHANGING:
        if (Hidden() && w == static_cast<WPARAM>(GWL_EXSTYLE)) {
            auto* ss = reinterpret_cast<STYLESTRUCT*>(l);
            if (OwnedMode(h)) ss->styleNew &= ~static_cast<DWORD>(WS_EX_TOOLWINDOW | WS_EX_APPWINDOW);
            else ss->styleNew = (ss->styleNew | WS_EX_TOOLWINDOW) & ~static_cast<DWORD>(WS_EX_APPWINDOW);
        }
        break;
    case WM_NCDESTROY:
        SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
        RemovePropW(h, kProp);
        break;
    }
    return CallWindowProcW(orig, h, msg, w, l);
}

bool TakeOver(HWND h, CREATESTRUCTW* cs) {
    if (GetPropW(h, kProp)) return false;
    LONG_PTR orig = GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (!orig) return false;
    SetPropW(h, kProp, reinterpret_cast<HANDLE>(orig));
    SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassProc));
    if (Hidden() && cs) { POINT p = OffScreen(); cs->x = p.x; cs->y = p.y; }
    Count(&Shared::created);
    if (g_shared) InterlockedExchange(&g_shared->pid, static_cast<LONG>(GetCurrentProcessId()));
    wchar_t cls[64] = {};
    if (g_shared && GetClassNameW(h, cls, 64) && lstrcmpW(cls, L"UnrealWindow") == 0)
        InterlockedExchange64(&g_shared->lastHwnd, reinterpret_cast<LONG64>(h));
    return true;
}

// ---------------------------------------------------------------- no crash UI (game only)

using CreateProcessWFn = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                       LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
CreateProcessWFn g_createProcess = nullptr;

bool ContainsNoCase(const wchar_t* s, const wchar_t* what) {
    if (!s) return false;
    const int n = lstrlenW(what);
    for (; *s; ++s)
        if (CompareStringOrdinal(s, n, what, n, TRUE) == CSTR_EQUAL) return true;
    return false;
}

BOOL WINAPI CreateProcessHook(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                              DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    const bool reporter = ContainsNoCase(app, L"CrashReportClient") || ContainsNoCase(cmd, L"CrashReportClient");
    if (!reporter || (g_shared && (g_shared->testFlags & 1))) return g_createProcess(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
    // Created suspended and ended at once: no window, no upload; UE sees a started reporter and lets the game end.
    BOOL ok = g_createProcess(app, cmd, pa, ta, inherit, flags | CREATE_SUSPENDED, env, dir, si, pi);
    if (ok && pi) {
        TerminateProcess(pi->hProcess, 0xC0DE);
        WaitForSingleObject(pi->hProcess, 2000);
    }
    if (g_shared) {
        const wchar_t* text = cmd ? cmd : app;
        const int len = text ? lstrlenW(text) : 0;
        const int keep = static_cast<int>(sizeof(g_shared->crashArgs) / sizeof(WCHAR)) - 1;
        const wchar_t* tail = text ? text + (len > keep ? len - keep : 0) : L"";
        lstrcpynW(g_shared->crashArgs, tail, keep + 1);
        InterlockedExchange(&g_shared->blockedPid, static_cast<LONG>(GetCurrentProcessId()));
        InterlockedIncrement(&g_shared->crashReportsBlocked);
    }
    return ok;
}

// Points the exe's import slot dll!fn at hook; returns the original target or nullptr.
void* PatchImport(const char* dll, const char* fn, void* hook) {
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dirEntry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dirEntry.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dirEntry.VirtualAddress); d->Name; ++d) {
        if (lstrcmpiA(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (lstrcmpA(reinterpret_cast<const char*>(ibn->Name), fn) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
            void* orig = InterlockedExchangePointer(reinterpret_cast<PVOID*>(&slots->u1.Function), hook);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return orig;
        }
    }
    return nullptr;
}

DWORD g_hookedThreads[64];
LONG g_numHookedThreads = 0;

extern "C" LRESULT CALLBACK CbtProcImpl(int code, WPARAM w, LPARAM l);

// Activation and minimising refused on this thread too (the runner hooks only the main thread).
void HookThisThread() {
    const DWORD tid = GetCurrentThreadId();
    const LONG n = g_numHookedThreads;
    for (LONG i = 0; i < n && i < 64; ++i)
        if (g_hookedThreads[i] == tid) return;
    if (n >= 64) return;
    if (SetWindowsHookExW(WH_CBT, &CbtProcImpl, g_module, tid)) {
        g_hookedThreads[n] = tid;
        InterlockedIncrement(&g_numHookedThreads);
    }
}

// In-context: runs on the thread that creates the window, right after CreateWindowEx made it (before it is shown).
void CALLBACK OnWindowCreated(HWINEVENTHOOK, DWORD, HWND h, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!h || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !Hidden()) return;
    if (GetAncestor(h, GA_ROOT) != h || !IsTarget(h)) return;
    if (!TakeOver(h, nullptr)) return;   // the main thread's windows: done in HCBT_CREATEWND already
    MakeToolWindow(h);
    POINT p = OffScreen();
    SetWindowPos(h, nullptr, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    Count(&Shared::otherWindows);
    HookThisThread();
}

void GuardAgainstCrashUi() {
    static LONG once = 0;
    if (InterlockedExchange(&once, 1)) return;
    LONG guard = 0;
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (SUCCEEDED(WerSetFlags(WER_FAULT_REPORTING_NO_UI))) guard |= 1;
    DisableProcessWindowsGhosting();
    if (void* orig = PatchImport("kernel32.dll", "CreateProcessW", reinterpret_cast<void*>(&CreateProcessHook))) {
        g_createProcess = reinterpret_cast<CreateProcessWFn>(orig);
        guard |= 2;
    }
    // (the main thread's windows are taken over in HCBT_CREATEWND while the runner's hook is in place; a window it
    // creates after the runner has ended comes through here as well)
    if (SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, g_module, &OnWindowCreated, GetCurrentProcessId(), 0,
                        WINEVENT_INCONTEXT))
        guard |= 4;
    if (g_shared) InterlockedExchange(&g_shared->guard, guard);
}

// ---------------------------------------------------------------- observer mode (game exe only)
//
// Spectator mode makes the player an invisible observer: `tdetect` keeps NPCs from noticing the player, but an NPC can
// start a conversation without detecting anyone, through an AI package (Ilend Vonius' Ambush package at the Kvatch
// gate's plane, MS48IlendTalktoPlayer: target the player, while MS48 is at stage 10 or lower, so in every new game) or a
// script's StartConversation. The NPC walks up, the dialogue opens, and the world pauses for the rest of the scene.
//
// While "observer" is set, three old-engine vtable slots treat the player as not there:
//   - The AI processes' SetPackageTarget(process, target) (High/MiddleHigh share one function, MiddleLow/Low another;
//     it stores the target at process+0x48): the player is never a package's target. A package aimed at the player
//     (Ambush, Find, Travel, Follow... with the player as target, like Ilend's) finds no target, as when its target is
//     not loaded, and the NPC does not set off. Without this the NPC still ran up to the player and tried to talk to
//     it ~40 times a second (2026-10-12, `observer-kvatch2`).
//   - Character's StartConversation(speaker, target, flag, topic) (Actor, Creature and PlayerCharacter have a stub
//     there that refuses): scripts and some AI. It gives the speaker a dialogue package that walks it to the target. It
//     first checks the target (in high process, a normal sit/sleep state) and returns false if the target cannot talk
//     now (e.g. the player sits), which every caller handles; the wrapper gives that answer for the player.
//   - TESNPC's Activate(base, target, activator, ...): the dialogue menu itself opens only here (its one caller of the
//     menu's start), when the player activates an NPC or when an NPC that walked up to the player under a Find, Ambush
//     or dialogue package "activates" the player (a forced greeting). Activations of the player by anyone else are
//     refused (false: nothing activated).
// NPCs among themselves are untouched: their packages, conversations and their cost are part of every scene.
//
// The slots are found, not hard-coded. StartConversation: the script command StartConversation (its command table
// entry: name pointer first, execute function at +0x30) calls it; of the vtable offsets its execute function calls,
// exactly one must be a slot that Character overrides while Actor and PlayerCharacter share their base version (RTTI
// names the vtables). Activate: exactly one slot of TESNPC's vtable must hold a function (its .pdata range) that both
// checks for the dialogue menu (`cmp eax, 3F1h`, menu type 1009) and calls StartConversation's slot. SetPackageTarget:
// exactly one slot of HighProcess's vtable must start with `mov [rcx+48h], rdx` and a call with `dl = 1` (marking the
// target); MiddleHighProcess has the same function there, and MiddleLow/LowProcess share one that stores the target at
// +0x48 too (`mov [rdi+48h], rbx`) and calls the same marker. Anything else: not installed (talkGuard < 0), the game
// runs as without it.

struct Range {
    const BYTE* begin = nullptr;
    const BYTE* end = nullptr;
    bool Contains(const void* p, size_t n = 1) const {
        const BYTE* b = static_cast<const BYTE*>(p);
        return b >= begin && b <= end && static_cast<size_t>(end - b) >= n;
    }
};
struct Image { const BYTE* base = nullptr; Range text, rdata, data; };

Image GameImage() {
    Image img;
    img.base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(img.base + reinterpret_cast<const IMAGE_DOS_HEADER*>(img.base)->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        char name[9] = {};
        memcpy(name, s->Name, 8);
        const Range r{ img.base + s->VirtualAddress, img.base + s->VirtualAddress + s->Misc.VirtualSize };
        if (!strcmp(name, ".text")) img.text = r;
        else if (!strcmp(name, ".rdata")) img.rdata = r;
        else if (!strcmp(name, ".data")) img.data = r;
    }
    return img;
}

// Calls f(p) for every place p in r where the n bytes of `what` start; f returns false to stop.
template <typename F> void ForEachMatch(const Range& r, const void* what, size_t n, F f) {
    const BYTE first = *static_cast<const BYTE*>(what);
    for (const BYTE* p = r.begin; r.Contains(p, n); ++p) {
        p = static_cast<const BYTE*>(memchr(p, first, static_cast<size_t>(r.end - p)));
        if (!p || !r.Contains(p, n)) return;
        if (!memcmp(p, what, n) && !f(p)) return;
    }
}

// The RTTI type descriptor named `mangled` (".?AVCharacter@@"): its name, NUL included, at +0x10, in .data. Unique or null.
const BYTE* FindTypeDescriptor(const Image& img, const char* mangled) {
    const BYTE* found = nullptr;
    int hits = 0;
    ForEachMatch(img.data, mangled, strlen(mangled) + 1, [&](const BYTE* p) { found = p - 0x10; return ++hits < 2; });
    return hits == 1 && img.data.Contains(found) ? found : nullptr;
}

struct Col { DWORD signature, offset, cdOffset, typeDescriptor, hierarchy, self; };   // _RTTICompleteObjectLocator (x64)

// The complete object's vtable of the class with this type descriptor: the .rdata qword after the one that points at
// its complete object locator (signature 1, offset 0, image-relative self pointer). Unique or null.
void* const* FindVtable(const Image& img, const BYTE* type) {
    if (!type) return nullptr;
    const DWORD typeRva = static_cast<DWORD>(type - img.base);
    const Col* col = nullptr;
    for (const BYTE* p = img.rdata.begin; img.rdata.Contains(p, sizeof(Col)); p += 4) {
        const auto* c = reinterpret_cast<const Col*>(p);
        if (c->typeDescriptor == typeRva && c->signature == 1 && c->offset == 0 && img.base + c->self == p) {
            if (col) return nullptr;
            col = c;
        }
    }
    if (!col) return nullptr;
    void* const* vtable = nullptr;
    for (auto* q = reinterpret_cast<void* const*>(img.rdata.begin); img.rdata.Contains(q, 2 * sizeof(void*)); ++q)
        if (*q == col) {
            if (vtable) return nullptr;
            vtable = q + 1;
        }
    return vtable;
}

// The execute function of the script command named `name`: its command table entry (in .data) points at the name (in
// .rdata) and holds the execute function at +0x30. Unique or null.
const BYTE* FindCommandHandler(const Image& img, const char* name) {
    const BYTE* handler = nullptr;
    bool ambiguous = false;
    ForEachMatch(img.rdata, name, strlen(name) + 1, [&](const BYTE* s) {
        if (s > img.rdata.begin && s[-1] != 0) return true;   // the end of a longer name
        for (auto* q = reinterpret_cast<const BYTE* const*>(img.data.begin); img.data.Contains(q, 7 * sizeof(void*)); ++q) {
            if (q[0] != s || !img.text.Contains(q[6])) continue;
            if (handler && handler != q[6]) ambiguous = true;
            handler = q[6];
        }
        return true;
    });
    return ambiguous ? nullptr : handler;
}

// The vtable offsets a function calls through: `call qword ptr [reg+disp32]` (FF /2, mod 10, not rsp), REX.B or not.
template <typename F> void ForEachVirtualCall(const BYTE* begin, const BYTE* end, F f) {
    for (const BYTE* p = begin; p + 6 <= end; ++p)
        if (p[0] == 0xFF && (p[1] & 0xF8) == 0x90 && p[1] != 0x94) f(*reinterpret_cast<const DWORD*>(p + 2));
}

const void* g_playerVtable = nullptr;
const BYTE* g_imageBase = nullptr;

bool IsPlayer(const void* ref) { return ref && *static_cast<const void* const*>(ref) == g_playerVtable; }
bool Observing() { return g_shared && g_shared->observer; }

void NoteRefusal(const void* npc, const void* returnAddress) {
    InterlockedIncrement(&g_shared->talksRefused);
    // TESForm: form ID at +0x10
    InterlockedExchange(&g_shared->lastRefusedForm, *reinterpret_cast<const LONG*>(static_cast<const BYTE*>(npc) + 0x10));
    InterlockedExchange(&g_shared->lastRefusedCaller, static_cast<LONG>(static_cast<const BYTE*>(returnAddress) - g_imageBase));
}

using StartConversationFn = UINT_PTR (*)(void* speaker, void* target, UINT_PTR flag, void* topic);
StartConversationFn g_startConversation = nullptr;

UINT_PTR ObserverStartConversation(void* speaker, void* target, UINT_PTR flag, void* topic) {
    if (Observing() && IsPlayer(target)) {
        NoteRefusal(speaker, _ReturnAddress());
        return 0;
    }
    return g_startConversation(speaker, target, flag, topic);
}

// Six arguments are known (base, target, activator, flag, item, count); two more stack slots are passed on unchanged
// in case there are more.
using ActivateFn = UINT_PTR (*)(void* base, void* target, void* activator, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR);
ActivateFn g_activate = nullptr;

UINT_PTR ObserverActivate(void* base, void* target, void* activator, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    if (Observing() && activator && IsPlayer(target) && !IsPlayer(activator)) {
        NoteRefusal(activator, _ReturnAddress());
        return 0;
    }
    return g_activate(base, target, activator, a4, a5, a6, a7, a8);
}

using SetPackageTargetFn = UINT_PTR (*)(void* process, void* target);
SetPackageTargetFn g_setTargetHigh = nullptr;   // HighProcess, MiddleHighProcess
SetPackageTargetFn g_setTargetLow = nullptr;    // MiddleLowProcess, LowProcess

void* ObservedTarget(void* target, const void* returnAddress) {
    if (!Observing() || !IsPlayer(target)) return target;
    InterlockedIncrement(&g_shared->targetsRefused);
    const LONG rva = static_cast<LONG>(static_cast<const BYTE*>(returnAddress) - g_imageBase);
    for (int i = 0; i < 16; i += 2) {
        LONG* slot = &g_shared->targetCallers[i];
        const LONG was = InterlockedCompareExchange(slot, rva, 0);
        if (was == 0 || was == rva) {
            InterlockedIncrement(slot + 1);
            break;
        }
    }
    return nullptr;
}
UINT_PTR ObserverSetTargetHigh(void* process, void* target) { return g_setTargetHigh(process, ObservedTarget(target, _ReturnAddress())); }
UINT_PTR ObserverSetTargetLow(void* process, void* target) { return g_setTargetLow(process, ObservedTarget(target, _ReturnAddress())); }

// The target of the first `call rel32` within 6 bytes after a `mov dl, 1` in [p, end), or null.
const BYTE* CallAfterDl1(const BYTE* p, const BYTE* end) {
    for (; p + 2 <= end; ++p) {
        if (p[0] != 0xB2 || p[1] != 0x01) continue;
        for (const BYTE* c = p + 2; c < p + 8 && c + 5 <= end; ++c)
            if (*c == 0xE8) return c + 5 + *reinterpret_cast<const INT32*>(c + 1);
    }
    return nullptr;
}

bool HasBytes(const BYTE* begin, size_t n, const BYTE* what, size_t m) {
    bool found = false;
    ForEachMatch(Range{ begin, begin + n }, what, m, [&](const BYTE*) { found = true; return false; });
    return found;
}

// Points a vtable slot in .rdata at `hook`. 1 done, 0 not the game's own function there (an earlier build of this DLL
// in the game wrapped it already: nothing to do), -4 not writable.
LONG WrapSlot(const Image& img, void* const* slotIn, void** original, void* hook) {
    void** slot = const_cast<void**>(slotIn);
    if (!img.text.Contains(*slot)) return 0;
    *original = *slot;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return -4;
    InterlockedExchangePointer(slot, hook);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return 1;
}

// 1 installed, 0 nothing to do (another build of this DLL in the game wrapped the slots already), < 0 not installed.
LONG WrapConversationStarts(LONG* talkSlot, LONG* activateSlot, LONG* targetSlot) {
    const Image img = GameImage();
    void* const* actor = FindVtable(img, FindTypeDescriptor(img, ".?AVActor@@"));
    void* const* character = FindVtable(img, FindTypeDescriptor(img, ".?AVCharacter@@"));
    void* const* player = FindVtable(img, FindTypeDescriptor(img, ".?AVPlayerCharacter@@"));
    void* const* npcBase = FindVtable(img, FindTypeDescriptor(img, ".?AVTESNPC@@"));
    void* const* high = FindVtable(img, FindTypeDescriptor(img, ".?AVHighProcess@@"));
    void* const* middleHigh = FindVtable(img, FindTypeDescriptor(img, ".?AVMiddleHighProcess@@"));
    void* const* middleLow = FindVtable(img, FindTypeDescriptor(img, ".?AVMiddleLowProcess@@"));
    void* const* low = FindVtable(img, FindTypeDescriptor(img, ".?AVLowProcess@@"));
    if (!actor || !character || !player || !npcBase || !high || !middleHigh || !middleLow || !low) return -1;

    // StartConversation: the one slot the script command calls that Character overrides and Actor/PlayerCharacter share
    const BYTE* handler = FindCommandHandler(img, "StartConversation");
    if (!handler || !img.text.Contains(handler, 0x200)) return -2;
    LONG talk = -1;
    int candidates = 0;
    ForEachVirtualCall(handler, handler + 0x200, [&](DWORD disp) {
        if (disp % sizeof(void*) || disp >= 0x2000 || static_cast<LONG>(disp) == talk) return;
        const size_t i = disp / sizeof(void*);
        if (!img.rdata.Contains(actor + i, sizeof(void*)) || !img.rdata.Contains(character + i, sizeof(void*)) ||
            !img.rdata.Contains(player + i, sizeof(void*)))
            return;
        if (actor[i] == player[i] && img.text.Contains(actor[i]) && character[i] != actor[i] && character[i]) {
            talk = static_cast<LONG>(disp);
            ++candidates;
        }
    });
    if (candidates != 1) return -3;

    // Activate: the one TESNPC slot whose function checks for the dialogue menu and calls StartConversation
    LONG activate = -1;
    candidates = 0;
    static const BYTE kCmpDialogMenu[] = { 0x3D, 0xF1, 0x03, 0x00, 0x00 };   // cmp eax, 3F1h
    for (size_t i = 0; i < 512 && img.rdata.Contains(npcBase + i, sizeof(void*)) && img.text.Contains(npcBase[i]); ++i) {
        DWORD64 imageBase = 0;
        const auto fn = reinterpret_cast<DWORD64>(npcBase[i]);
        const RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(fn, &imageBase, nullptr);
        if (!rf || imageBase + rf->BeginAddress != fn) continue;
        const Range body{ img.base + rf->BeginAddress, img.base + rf->EndAddress };
        bool menu = false, calls = false;
        ForEachMatch(body, kCmpDialogMenu, sizeof(kCmpDialogMenu), [&](const BYTE*) { menu = true; return false; });
        ForEachVirtualCall(body.begin, body.end, [&](DWORD disp) { calls |= static_cast<LONG>(disp) == talk; });
        if (menu && calls) {
            activate = static_cast<LONG>(i * sizeof(void*));
            ++candidates;
        }
    }
    if (candidates != 1) return -5;

    // SetPackageTarget: the one HighProcess slot that stores its argument at +0x48 and marks it (see above)
    static const BYTE kStoreHigh[] = { 0x48, 0x89, 0x51, 0x48 };   // mov [rcx+48h], rdx
    static const BYTE kStoreLow[] = { 0x48, 0x89, 0x5F, 0x48 };    // mov [rdi+48h], rbx
    LONG target = -1;
    const BYTE* marker = nullptr;
    candidates = 0;
    for (size_t i = 0; i < 1024 && img.rdata.Contains(high + i, sizeof(void*)) && img.text.Contains(high[i]); ++i) {
        const BYTE* f = static_cast<const BYTE*>(high[i]);
        if (!img.text.Contains(f, 0x30) || !HasBytes(f, 0x24, kStoreHigh, sizeof(kStoreHigh))) continue;
        const BYTE* m = CallAfterDl1(f, f + 0x24);
        if (!m || !img.text.Contains(m)) continue;
        target = static_cast<LONG>(i * sizeof(void*));
        marker = m;
        ++candidates;
    }
    if (candidates != 1) return -6;
    const size_t t = target / sizeof(void*);
    if (!img.rdata.Contains(middleHigh + t, sizeof(void*)) || !img.rdata.Contains(middleLow + t, sizeof(void*)) ||
        !img.rdata.Contains(low + t, sizeof(void*)) || middleHigh[t] != high[t] || low[t] != middleLow[t])
        return -6;
    const BYTE* lowFn = static_cast<const BYTE*>(low[t]);
    if (!img.text.Contains(lowFn, 0x30) || !HasBytes(lowFn, 0x30, kStoreLow, sizeof(kStoreLow)) ||
        CallAfterDl1(lowFn, lowFn + 0x30) != marker)
        return -6;

    *talkSlot = talk;
    *activateSlot = activate;
    *targetSlot = target;
    g_playerVtable = player;
    g_imageBase = img.base;
    const LONG wrapped[] = {
        WrapSlot(img, character + talk / sizeof(void*), reinterpret_cast<void**>(&g_startConversation),
                 reinterpret_cast<void*>(&ObserverStartConversation)),
        WrapSlot(img, npcBase + activate / sizeof(void*), reinterpret_cast<void**>(&g_activate),
                 reinterpret_cast<void*>(&ObserverActivate)),
        WrapSlot(img, high + t, reinterpret_cast<void**>(&g_setTargetHigh), reinterpret_cast<void*>(&ObserverSetTargetHigh)),
        WrapSlot(img, middleHigh + t, reinterpret_cast<void**>(&g_setTargetHigh), reinterpret_cast<void*>(&ObserverSetTargetHigh)),
        WrapSlot(img, middleLow + t, reinterpret_cast<void**>(&g_setTargetLow), reinterpret_cast<void*>(&ObserverSetTargetLow)),
        WrapSlot(img, low + t, reinterpret_cast<void**>(&g_setTargetLow), reinterpret_cast<void*>(&ObserverSetTargetLow)),
    };
    LONG status = 1;
    for (LONG w : wrapped) {
        if (w < 0) return w;
        if (w == 0) status = 0;
    }
    return status;
}

void InstallTalkGuard() {
    static LONG once = 0;
    if (InterlockedExchange(&once, 1)) return;
    // Once per game process, also with two builds of this DLL in it (-Session reuse): the first one's wrappers read the
    // same flag. A named event lives exactly as long as the process (a process ID alone can be reused). Kept open.
    wchar_t name[64];
    wsprintfW(name, L"Local\\OblvBenchHook.Observer.%lu", GetCurrentProcessId());
    const HANDLE first = CreateEventW(nullptr, TRUE, FALSE, name);
    if (first && GetLastError() == ERROR_ALREADY_EXISTS) return;
    LONG talk = 0, activate = 0, target = 0;
    const LONG status = WrapConversationStarts(&talk, &activate, &target);
    if (!g_shared || status == 0) return;
    InterlockedExchange(&g_shared->talkGuardPid, static_cast<LONG>(GetCurrentProcessId()));
    InterlockedExchange(&g_shared->talkSlot, talk);
    InterlockedExchange(&g_shared->activateSlot, activate);
    InterlockedExchange(&g_shared->targetSlot, target);
    InterlockedExchange(&g_shared->talkGuard, status);
}

}  // namespace

extern "C" LRESULT CALLBACK CbtProcImpl(int code, WPARAM w, LPARAM l) {
    if (g_gameHost) GuardAgainstCrashUi();
    if (g_gameExe) InstallTalkGuard();
    if (code >= 0 && code != HCBT_ACTIVATE) NoteForeground();
    if (code >= 0) {
        HWND h = reinterpret_cast<HWND>(w);
        switch (code) {
        case HCBT_CREATEWND: {
            auto* cw = reinterpret_cast<CBT_CREATEWNDW*>(l);
            CREATESTRUCTW* cs = cw ? cw->lpcs : nullptr;
            if (cs && !(cs->style & WS_CHILD) && cs->hwndParent != HWND_MESSAGE && IsTarget(h)) TakeOver(h, cs);
            break;
        }
        case HCBT_ACTIVATE:
            if (Hidden()) {
                Count(&Shared::activationsBlocked);
                SetTimer(nullptr, 0, 0, &ReturnForeground);   // the PC user's window gets the foreground back if none has it
                return 1;
            }
            break;
        case HCBT_MINMAX: {
            int cmd = LOWORD(l);
            if (Hidden() && (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE)) {
                Count(&Shared::minimiseBlocked);
                return 1;
            }
            break;
        }
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

// The runner's hook entry (the name BenchLib.cs asks for).
extern "C" __declspec(dllexport) LRESULT CALLBACK CbtProc(int code, WPARAM w, LPARAM l) { return CbtProcImpl(code, w, l); }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        CharLowerW(exe);
        g_gameHost = wcsstr(exe, L"oblivionremastered") != nullptr;
        g_gameExe = wcsstr(exe, L"oblivionremastered-win64-shipping") != nullptr;
        // Stay loaded for the life of the process: the window subclass lives in this DLL, and Windows
        // unloads hook DLLs from the target lazily once the benchmark removes the hook.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&CbtProc), &pinned);
        HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, g_gameHost ? kMapping : kToolMapping);
        if (map) {
            g_shared = static_cast<Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
            if (g_shared) { InterlockedCompareExchange(&g_shared->version, 1, 0); }
            // the handle stays open: the mapping must outlive the benchmark process that created it
        }
    }
    return TRUE;
}
