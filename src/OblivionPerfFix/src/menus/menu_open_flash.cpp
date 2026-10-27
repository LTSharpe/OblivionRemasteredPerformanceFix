// MenuOpenFlashFix (GitHub issue #5, a game bug: also without this DLL): the bright wrong frame when the player menu
// (Esc, the pause menu's System page; the same widget for inventory, map, ...) opens.
//
// What the game does (build 19115871, measured 2026-10-26, worker escfix; findings "2026-10-26 14:40-17:30 (worker escfix)"):
//   - The player menu's background is not a 2D picture: the view target switches to an "inventory preview box"
//     (BP_Box_Preview_Inventory, a Blueprint actor far outside the playable world, about 336 km from Cheydinhal) whose
//     camera looks at a translucent plane (ScreenshotDisplay) showing the menu's screenshot of the game view, inside a
//     5000-unit unlit cube (Cube, M_InventoryPreview_SceneBackground) that hides the world around the box.
//   - Frame N (the open): the menu widget (VBackgroundWidgetBase, native RequestCapture exe+0x492A120) asks for the
//     screenshot of this frame; the box's components are made visible; the camera is still the game's.
//     Frame N+1: the view target is the box.
//   - When the box's location lies inside the game camera's view on frame N (it is far away, but UE has no far plane), the
//     renderer's occlusion culling processes the box's just-shown primitives from the old camera (where they are hidden),
//     and on frame N+1, inside the box, it culls the cube and the plane: that second frame of the open shows the world
//     outside the box (bright sky and a cliff at the map's edge) under the menu: the "flash". It needs a gap of more than
//     ~8 s since the last menu (UE's PrimitiveProbablyVisibleTime): the box's primitives then have no recent "proven
//     visible" time.
//   - Measured (hidden game, 8 / 5 fps cap so every rendered frame is captured): camera facing the box 2/2 and 4/4 wrong
//     frames, 90 degrees away 0/2; r.AllowOcclusionQueries 0 for frame N only (on again for N+1): 0/2, and N+1 shows the
//     menu's normal background; occlusion off for N+1 only: N+1 right, N+2 wrong. A camera cut on N+1 does not help.
//
// The fix: occlusion culling (r.AllowOcclusionQueries, UE's GOcclusionCullEnabled) is switched off for frame N only. The
// RequestCapture call (game thread, frame N, before the frame's scene is sent to the render thread) sets the console
// variable to 0 with its own set-by priority. UGameViewportClient::ProcessScreenShots, which FViewport::Draw calls on the
// game thread every frame right after the frame's scene was sent to the render thread (the menu's screenshot is read
// back inside it), sets it back to 1 on its way out, so frame N+1 renders with occlusion as before. The variable is
// render-thread-safe: UE applies each change on the render thread in command order, so exactly frame N's rendering sees
// 0. Without occlusion culling a frame draws a superset of what it would draw: everything the culling would have removed
// correctly lies behind other opaque geometry and is depth-tested away, so frame N looks the same; the only visible change
// is on frame N+1, where the box's cube and the menu's background plane are no longer culled. Not bit-for-bit the same
// frame in two ways (escrev review): primitives that occlusion would have culled get that frame's LastRenderTime (for
// one frame, while the game is entering a paused menu), and the frame costs more GPU time (one longer frame at menu
// open). Custom depth is unaffected (UE never occlusion-culls proxies that render custom depth). UE's own high-resolution
// screenshot switches occlusion culling off for its frame the same way.
// The OBSE64 route loads the DLL after the game's code started: there the fix is not installed (a log line says so).
// Safety: it never switches occlusion off unless the restore point is live (ProcessScreenShots ran on the game thread
// within the last second) and the console variable's interface checked out on the variable itself (GetFlags has
// ECVF_RenderThreadSafe, GetBool / GetInt / GetFloat at their UE 5.3 slots agree with the bool the registration binds, and
// after the first Set the variable reads 0); otherwise it stays off with a log line. Nothing is done when the variable is
// already 0 (set by the player or an ini). A later RequestCapture restores a pending switch first.
//
// Found by byte pattern: RequestCapture's body (its screenshot-delegate binding: this+0x500 handle, subsystem+0x370
// delegate), the one jmp to RequestCapture (its UFunction exec thunk), ProcessScreenShots (the .pdata function around its
// read-back call, its prologue; its vtable slots in .rdata) and the static registration of r.AllowOcclusionQueries (the
// bool reference variable with ECVF_RenderThreadSafe; the IConsoleVariable* slot it stores, its GetFlags call at +0x18).
// IConsoleVariable::Set / GetBool offsets: the ones RestoreFsrAfterAltTab finds by pattern when it is installed, else
// UE 5.3's layout (+0x80 / +0x88); either way checked on the live variable before the first use. Start-up only (the
// thunk's rel32 is rewritten before the game's code runs; the vtable slots are single 8-byte writes), then switched live.
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace opf {
namespace {

// RequestCapture (exe+0x492A120): get the screenshot subsystem, unbind this+0x500, bind (this, callback) to
// subsystem+0x370, store the handle, tail-jump to the request.
constexpr const char* kRequestCapture =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B D9 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 93 00 05 00 00 "
    "48 8B F0 48 85 D2 74 0C 48 8D 88 70 03 00 00 E8 ?? ?? ?? ?? C7 44 24 28 00 00 00 00 48 8D 05 ?? ?? ?? ?? 48 89 44 24 20";
// RequestCapture's exec thunk (exe+0x4620C40): mov rax,[rdx+20h]; xor r8d,r8d; test rax,rax; setne r8b; add r8,rax;
// mov [rdx+20h],r8; jmp RequestCapture
constexpr const char* kThunk = "48 8B 42 20 45 33 C0 48 85 C0 41 0F 95 C0 4C 03 C0 4C 89 42 20 E9 ?? ?? ?? ??";
constexpr size_t kThunkJmp = 21;
// In UGameViewportClient::ProcessScreenShots (exe+0x3222CE0): the read-back call (save_bytes.cpp's kReadBackCall)
constexpr const char* kReadBack = "48 8D 55 A7 89 45 EF 8B 45 9B 89 45 F3 E8 ?? ?? ?? ?? 0F B6 F0 45 32 F6";
constexpr const char* kProcessShotsEntry = "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC C0 00 00 00";
// The static registration of a bool reference cvar with ECVF_RenderThreadSafe (0x20): lea r8,[ref]; lea rdx,[name];
// ...; call [rax+40h] (RegisterConsoleVariableRef); lea rcx,[...]; mov [slot],rax; mov [...],rcx; mov rcx,rax;
// mov rdx,[rax]; call [rdx+18h] (GetFlags)
constexpr const char* kCvarRegistration =
    "4C 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 89 7C 24 30 C7 44 24 20 20 00 00 00 FF 50 40 48 8D 0D ?? ?? ?? ?? "
    "48 89 05 ?? ?? ?? ?? 48 89 0D ?? ?? ?? ?? 48 8B C8 48 8B 10 FF 52 18";
constexpr size_t kRefLea = 0, kNameLea = 7, kSlotMov = 37;
constexpr wchar_t kCvarName[] = L"r.AllowOcclusionQueries";
constexpr uint32_t kOffGetFlags = 0x18, kOffGetInt = 0x90, kOffGetFloat = 0x98;
constexpr uint32_t kRenderThreadSafe = 0x20;  // ECVF_RenderThreadSafe

using RequestCaptureFn = void (*)(void* self);
using ProcessShotsFn = bool (*)(void* self, void* viewport);
using GetBoolFn = bool (*)(void* cvar);
using GetIntFn = int32_t (*)(void* cvar);
using GetFloatFn = float (*)(void* cvar);
using GetFlagsFn = int32_t (*)(void* cvar);
using SetFn = void (*)(void* cvar, const wchar_t* value, int32_t setBy);

RequestCaptureFn g_requestOrig = nullptr;
ProcessShotsFn g_shotsOrig = nullptr;
void** g_cvarSlot = nullptr;
const volatile bool* g_cvarRef = nullptr;  // the bool r.AllowOcclusionQueries binds (GOcclusionCullEnabled)
uint32_t g_offSet = 0x80, g_offGetBool = 0x88;
bool g_offsetsFromFocus = false;
uint8_t* g_jmpSite = nullptr;  // the thunk's jmp to RequestCapture
void** g_shotSlots[4] = {};
int g_numShotSlots = 0;
uint8_t* g_region = nullptr;
bool g_installed = false;
bool g_testRanges = true;           // self-test: fakes live outside the game's sections
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_broken{false};     // the variable's interface did not check out (or a Set failed 3 times): stays off
std::atomic<bool> g_armed{false};
std::atomic<int> g_verified{0};        // 0 not yet, 1 checked out, -1 failed
std::atomic<uint32_t> g_gtId{0};       // the game thread (ProcessScreenShots' caller)
std::atomic<int64_t> g_lastShotsQpc{0};
int32_t g_armedSetBy = 0;
int g_restoreFails = 0;
int64_t g_qpcPerSecond = 0;
std::atomic<uint64_t> g_arms{0}, g_restoredByFrame{0}, g_restoredByNext{0}, g_alreadyOff{0}, g_noCvar{0}, g_notLive{0},
    g_offThread{0}, g_shotsCalls{0};
uint64_t g_logged = 0;

int64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
bool OnGameThread() { const uint32_t gt = g_gtId.load(std::memory_order_relaxed); return gt && GetCurrentThreadId() == gt; }
void* Cvar() { return g_cvarSlot ? *g_cvarSlot : nullptr; }
template <class Fn> Fn Slot(void* cvar, uint32_t off) { return reinterpret_cast<Fn>((*static_cast<void***>(cvar))[off / sizeof(void*)]); }

// The value r.AllowOcclusionQueries is left at, for Broken()'s log line: GetBool once the interface checked out, else the
// bool the registration binds.
const char* ValueText()
{
    int v = -1;
    __try {
        void* const cv = Cvar();
        if (cv && g_verified.load(std::memory_order_relaxed) > 0) v = Slot<GetBoolFn>(cv, g_offGetBool)(cv) ? 1 : 0;
        else if (g_cvarRef) v = *g_cvarRef ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = -1;
    }
    return v == 1   ? "1 (occlusion culling on, as the game has it)"
           : v == 0 ? "0 (occlusion culling stays off until the game restarts or the console sets it to 1: same image, more GPU work)"
                    : "an unknown value (it could not be read)";
}

char g_brokenLog[320] = {};

void Broken(const char* why)
{
    if (g_broken.exchange(true)) return;
    snprintf(g_brokenLog, sizeof g_brokenLog, "%s: fix off for this session; r.AllowOcclusionQueries is left at %s", why, ValueText());
    Log("[menuflash] %s", g_brokenLog);
}

// The console variable's interface, checked once on the live variable (game thread, before the first Set): the vtable in
// .rdata, the slots in .text, ECVF_RenderThreadSafe in its flags, GetBool / GetInt / GetFloat equal to the bound bool.
// If a change of the variable is in flight at that moment (set on the game thread, the bound bool not yet updated by the
// render thread), the values disagree and the check fails although the interface is right: the fix then stays off for
// the session. That is the safe direction (the game's behaviour, nothing switched); it is not retried.
bool Verify(void* cv)
{
    const int v = g_verified.load(std::memory_order_relaxed);
    if (v) return v > 0;
    const GameModule& game = Game();
    bool ok = false;
    __try {
        void** const vt = *static_cast<void***>(cv);
        ok = !g_testRanges || (game.rdata.Contains(vt) && game.rdata.Contains(vt + 0x9F / sizeof(void*)));
        for (uint32_t off : {kOffGetFlags, g_offSet, g_offGetBool, kOffGetInt, kOffGetFloat})
            ok = ok && (!g_testRanges || game.text.Contains(vt[off / sizeof(void*)]));
        if (ok) {
            const bool ref = *g_cvarRef;
            const uint32_t flags = static_cast<uint32_t>(Slot<GetFlagsFn>(cv, kOffGetFlags)(cv));
            const bool b = Slot<GetBoolFn>(cv, g_offGetBool)(cv);
            const int32_t i = Slot<GetIntFn>(cv, kOffGetInt)(cv);
            const float f = Slot<GetFloatFn>(cv, kOffGetFloat)(cv);
            ok = (flags & kRenderThreadSafe) && b == ref && i == (ref ? 1 : 0) && f == (ref ? 1.0f : 0.0f);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    g_verified.store(ok ? 1 : -1);
    if (!ok) Broken("r.AllowOcclusionQueries' IConsoleVariable interface is not the expected one");
    return ok;
}

// Sets the variable back to 1 with the priority it had (game thread). Disarmed only once that worked.
void Restore(std::atomic<uint64_t>& counter)
{
    if (!g_armed.load(std::memory_order_relaxed)) return;
    void* const cv = Cvar();
    bool ok = false;
    if (cv) {
        __try {
            if (!Slot<GetBoolFn>(cv, g_offGetBool)(cv)) Slot<SetFn>(cv, g_offSet)(cv, L"1", g_armedSetBy);
            ok = Slot<GetBoolFn>(cv, g_offGetBool)(cv);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    if (ok) {
        g_armed.store(false);
        g_restoreFails = 0;
        counter.fetch_add(1, std::memory_order_relaxed);
    } else if (++g_restoreFails >= 3) {
        g_armed.store(false);
        Broken("r.AllowOcclusionQueries could not be set back to 1 (left as it is)");
    }
}

void Arm()
{
    if (!OnGameThread()) {
        g_offThread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_armed.load(std::memory_order_relaxed)) Restore(g_restoredByNext);  // never two in a row
    if (g_armed.load(std::memory_order_relaxed) || g_broken.load(std::memory_order_relaxed)) return;
    // the restore point must be live: ProcessScreenShots ran within the last second
    if (Qpc() - g_lastShotsQpc.load(std::memory_order_relaxed) > g_qpcPerSecond) {
        g_notLive.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    void* const cv = Cvar();
    if (!cv) {
        g_noCvar.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!Verify(cv)) return;
    bool set = false;
    __try {
        if (!Slot<GetBoolFn>(cv, g_offGetBool)(cv)) {
            g_alreadyOff.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_armedSetBy = static_cast<int32_t>(static_cast<uint32_t>(Slot<GetFlagsFn>(cv, kOffGetFlags)(cv)) & 0xFF000000u);
        Slot<SetFn>(cv, g_offSet)(cv, L"0", g_armedSetBy);
        set = !Slot<GetBoolFn>(cv, g_offGetBool)(cv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        set = false;
    }
    if (!set) {
        Broken("r.AllowOcclusionQueries did not switch to 0");
        return;
    }
    g_armed.store(true);
    g_arms.fetch_add(1, std::memory_order_relaxed);
}

void RequestCaptureHook(void* self)
{
    if (g_enabled.load(std::memory_order_relaxed)) Arm();
    g_requestOrig(self);
}

bool ProcessShotsHook(void* self, void* viewport)
{
    const bool r = g_shotsOrig(self, viewport);
    if (!g_gtId.load(std::memory_order_relaxed)) g_gtId.store(GetCurrentThreadId());
    if (OnGameThread()) {
        g_lastShotsQpc.store(Qpc(), std::memory_order_relaxed);
        g_shotsCalls.fetch_add(1, std::memory_order_relaxed);
        Restore(g_restoredByFrame);  // after this frame's scene went to the render thread
    }
    return r;
}

// A stub "jmp [rip+0]; dq target" at `at`.
void WriteStub(uint8_t* at, const void* target)
{
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, jmp, sizeof jmp);
    memcpy(at + sizeof jmp, &target, sizeof target);
}

}  // namespace

bool InstallMenuOpenFlashFix(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!beforeGameCode) {
        Log("[menuflash] this route loads the DLL after the game's code started (the exec thunk's jmp is rewritten only before "
            "that): fix not installed; the version.dll route installs it");
        return false;
    }
    const GameModule& game = Game();
    const auto req = FindAll(game.text, ParsePattern(kRequestCapture));
    if (req.size() != 1) {
        Log("[menuflash] RequestCapture not found (%zu matches): fix not installed", req.size());
        return false;
    }
    uint8_t* const request = req[0];
    // the one jmp to RequestCapture: its UFunction exec thunk (P_FINISH: Stack.Code += !!Stack.Code, then jmp)
    uint8_t* jmp = nullptr;
    int jmps = 0;
    for (uint8_t* hit : FindAll(game.text, ParsePattern(kThunk))) {
        if (RipTarget(hit + kThunkJmp, 1, 5) != request) continue;
        jmp = hit + kThunkJmp;
        ++jmps;
    }
    if (jmps != 1) {
        Log("[menuflash] %d exec thunks jump to RequestCapture (expected one): fix not installed", jmps);
        return false;
    }
    // ProcessScreenShots: the function around the read-back call; its vtable slots
    const auto rb = FindAll(game.text, ParsePattern(kReadBack));
    uint8_t* const shots = rb.size() == 1 ? PrimaryFunction(game, rb[0]) : nullptr;
    if (!shots || !MatchesAt(shots, ParsePattern(kProcessShotsEntry))) {
        Log("[menuflash] UGameViewportClient::ProcessScreenShots not found (%zu read-back calls): fix not installed", rb.size());
        return false;
    }
    int nslots = 0;
    for (uint8_t* p = game.rdata.begin; p + sizeof(void*) <= game.rdata.end; p += sizeof(void*)) {
        void* v;
        memcpy(&v, p, sizeof v);
        if (v != shots) continue;
        if (nslots < 4) g_shotSlots[nslots] = reinterpret_cast<void**>(p);
        ++nslots;
    }
    if (nslots < 1 || nslots > 4) {
        Log("[menuflash] %d vtable slots hold ProcessScreenShots (expected 1-4): fix not installed", nslots);
        return false;
    }
    // r.AllowOcclusionQueries' IConsoleVariable* slot and the bool it binds
    void** slot = nullptr;
    const volatile bool* ref = nullptr;
    int slots = 0;
    for (uint8_t* hit : FindAll(game.text, ParsePattern(kCvarRegistration))) {
        const uint8_t* const name = RipTarget(hit + kNameLea, 3, 7);
        if (!game.rdata.Contains(name) || !game.rdata.Contains(name + sizeof kCvarName - 1)) continue;
        if (memcmp(name, kCvarName, sizeof kCvarName) != 0) continue;
        slot = reinterpret_cast<void**>(RipTarget(hit + kSlotMov, 3, 7));
        ref = reinterpret_cast<const volatile bool*>(RipTarget(hit + kRefLea, 3, 7));
        ++slots;
    }
    if (slots != 1 || !game.data.Contains(slot) || !game.data.Contains(const_cast<const bool*>(ref))) {
        Log("[menuflash] the r.AllowOcclusionQueries registration not found (%d matches): fix not installed", slots);
        return false;
    }
    FocusRestoreInfoOut fr{};
    g_offsetsFromFocus = FocusRestoreInfo(&fr);
    if (g_offsetsFromFocus) {
        if (fr.getFlagsOffset != kOffGetFlags) {
            Log("[menuflash] IConsoleVariable::GetFlags at +0x%X, not +0x18 as the registration calls it: fix not installed", fr.getFlagsOffset);
            return false;
        }
        g_offSet = fr.setOffset;
        g_offGetBool = fr.getBoolOffset;
    }
    uint8_t* const region = AllocNearGame(0x20);
    if (!region) {
        Log("[menuflash] no free memory near the game's code: fix not installed");
        return false;
    }
    WriteStub(region, reinterpret_cast<const void*>(&RequestCaptureHook));
    DWORD old;
    VirtualProtect(region, 0x20, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 0x20);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcPerSecond = f.QuadPart;
    g_requestOrig = reinterpret_cast<RequestCaptureFn>(request);
    g_shotsOrig = reinterpret_cast<ProcessShotsFn>(shots);
    g_cvarSlot = slot;
    g_cvarRef = ref;
    g_numShotSlots = nslots;
    // the restore point first (harmless on its own), then the request
    bool ok = true;
    for (int i = 0; i < nslots; ++i) ok &= WritePointer(g_shotSlots[i], reinterpret_cast<void*>(&ProcessShotsHook));
    const int64_t rel = region - (jmp + 5);
    if (ok && rel == static_cast<int32_t>(rel)) {
        const int32_t rel32 = static_cast<int32_t>(rel);
        uint8_t bytes[4];
        memcpy(bytes, &rel32, sizeof bytes);
        ok = WriteCode(jmp + 1, bytes);
    } else ok = false;
    if (!ok) {
        for (int i = 0; i < nslots; ++i) WritePointer(g_shotSlots[i], shots);  // the hook only passes through meanwhile
        VirtualFree(region, 0, MEM_RELEASE);
        Log("[menuflash] could not redirect RequestCapture's jmp or ProcessScreenShots' slots: fix not installed");
        return false;
    }
    g_jmpSite = jmp;
    g_region = region;
    g_installed = true;
    Log("[menuflash] RequestCapture exe+0x%llX (jmp exe+0x%llX) and ProcessScreenShots exe+0x%llX (%d vtable slots) hooked, "
        "r.AllowOcclusionQueries slot exe+0x%llX, bool exe+0x%llX (Set +0x%X, GetBool +0x%X %s, checked on the variable before "
        "the first use): occlusion culling off for the menu-open frame",
        static_cast<unsigned long long>(request - game.base), static_cast<unsigned long long>(jmp - game.base),
        static_cast<unsigned long long>(shots - game.base), nslots,
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(slot) - game.base),
        static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(const_cast<const bool*>(ref)) - game.base), g_offSet,
        g_offGetBool, g_offsetsFromFocus ? "from RestoreFsrAfterAltTab's search" : "UE 5.3 layout");
    return true;
}

void SetMenuOpenFlashFix(bool on)
{
    if (!g_installed) return;
    if (g_enabled.exchange(on) != on) Log("[menuflash] %s", on ? "on" : "off (the game's behaviour)");
}

void LogMenuOpenFlashStats()
{
    const uint64_t arms = g_arms.load(), fr = g_restoredByFrame.load(), nx = g_restoredByNext.load(), off = g_alreadyOff.load(),
                   nocv = g_noCvar.load(), nl = g_notLive.load(), ot = g_offThread.load();
    const uint64_t sum = arms + fr + nx + off + nocv + nl + ot;
    if (!g_installed || sum == g_logged) return;
    g_logged = sum;
    Log("[menuflash] menu-open frames without occlusion culling: %llu (back on at that frame's ProcessScreenShots %llu, by the next "
        "request %llu); not switched: occlusion already off %llu, variable not registered %llu, restore point not live %llu, "
        "not on the game thread %llu%s",
        static_cast<unsigned long long>(arms), static_cast<unsigned long long>(fr), static_cast<unsigned long long>(nx),
        static_cast<unsigned long long>(off), static_cast<unsigned long long>(nocv), static_cast<unsigned long long>(nl),
        static_cast<unsigned long long>(ot), g_broken.load() ? "; OFF for this session (see above)" : "");
}

bool MenuOpenFlashInfo(MenuOpenFlashInfoOut* out)
{
    if (!g_installed) return false;
    out->jmpSite = g_jmpSite;
    out->request = reinterpret_cast<void*>(g_requestOrig);
    out->processShots = reinterpret_cast<void*>(g_shotsOrig);
    for (int i = 0; i < 4; ++i) out->shotSlots[i] = g_shotSlots[i];
    out->numShotSlots = g_numShotSlots;
    out->cvarSlot = g_cvarSlot;
    out->cvarRef = const_cast<const bool*>(g_cvarRef);
    out->requestHook = reinterpret_cast<void*>(&RequestCaptureHook);
    out->shotsHook = reinterpret_cast<void*>(&ProcessShotsHook);
    out->armed = g_armed.load();
    out->broken = g_broken.load();
    return true;
}

// Self-test: the arm/restore logic against a fake console variable and fake originals (nothing of the game runs). The
// calling thread becomes the "game thread"; resets the state (verified, broken, armed, liveness).
void MenuOpenFlashTestSetup(void** fakeCvarSlot, const bool* fakeRef, void* fakeRequest, void* fakeShots, uint32_t setOffset,
                            uint32_t getBoolOffset)
{
    g_cvarSlot = fakeCvarSlot;
    g_cvarRef = fakeRef;
    g_requestOrig = reinterpret_cast<RequestCaptureFn>(fakeRequest);
    g_shotsOrig = reinterpret_cast<ProcessShotsFn>(fakeShots);
    g_offSet = setOffset;
    g_offGetBool = getBoolOffset;
    g_installed = true;
    g_testRanges = false;
    g_armed = false;
    g_broken = false;
    g_verified = 0;
    g_restoreFails = 0;
    g_gtId = GetCurrentThreadId();
    g_lastShotsQpc = 0;
    g_brokenLog[0] = 0;
    if (!g_qpcPerSecond) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcPerSecond = f.QuadPart;
    }
}

void MenuOpenFlashTestAgeRestorePoint(int64_t ms) { g_lastShotsQpc.fetch_sub(ms * g_qpcPerSecond / 1000); }

const char* MenuOpenFlashLastBrokenLog() { return g_brokenLog; }

}  // namespace opf
