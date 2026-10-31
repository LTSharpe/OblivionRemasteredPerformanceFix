// StreamBehindLoadingScreen (on by default): after a door, city gate or fast travel the new area's textures stream in
// while the loading screen is still up, instead of the game holding the fade-in until they are in.
//
// What the game does (build 19115871): when a transition's loading screen starts, the transition object stores the
// texture streaming pool size (r.Streaming.PoolSize) in a TOptional (+0x234 value, +0x238 set) and sets the pool to
// 1 MB, so every streamed mip is dropped while the new area loads. When the heavy loading is over it arms a 0.5 s
// looping gate; the gate's tick waits until the new world has streamed in (world partition streaming completed, no
// pending travel, no async loading) and schedules fade-to-game for the next tick. Fade-to-game's handler sets the stored
// pool size back and calls "stream all resources", which blocks until the new view's textures are in: 150-160 ms after
// an interior entry and 300-350 ms after stepping outside at 5120x2160 (100 / 260 ms at 720p), every time, with the
// last loading-screen image frozen meanwhile.
//
// The change: once the gate is armed and async loading is idle (the gate's own queries), the stored pool size is set
// back (the console-variable call the fade handler makes, same value, same "set by code") and the streaming manager's
// normal update runs once per frame until fade-to-game (non-blocking: it issues requests and returns; it is the update
// stream-all itself runs first). The game's gate, its fade-to-game, its pool-size call and its blocking stream-all run
// as before; the stream-all only finds the textures already in. The freeze, the fade and the moment the player gets
// control are not touched, nothing is waited for here, and the image when the world appears is the one the game's own
// wait produces. Starting earlier (right at arming) competed with the level's last loads and sometimes cost one more
// gate tick (0.5 s of loading screen); waiting for async loading to be idle does not.
//
// Everything runs on the game thread: a pass-through detour of the gate-arming function (its 11-byte prologue moves to
// a trampoline) tells us the transition; NpcCreationPacing's frame hook does the per-frame part.
//
// Found by byte pattern (build 19115871 addresses in the comments); anything unexpected: one log line, not installed.
//  - the gate lambda (exe+0x4878670): from its GetWorld call through its checks; read from it: GetWorld's vtable slot,
//    the travel query (exe+0x3864220) and the global async-loading query pointer (exe+0x8DAA2E8);
//  - the gate-arming function (exe+0x4882730): its prologue, and that it uses the gate timer handle's offset (+0x240,
//    from the fade-to-game hook's search);
//  - the loading-start handler (exe+0x488C4C0): the cached r.Streaming.PoolSize console variable (a function static)
//    and the stored-pool TOptional's offsets, read from its "store the pool size once" code;
//  - the streaming manager: a lazily created singleton (exe+0x924C970, read only, never created here) whose vtable +0x10
//    must be the update that stream-all (exe+0x30D6CB0) calls first.
//
// World drawing behind the loading screen (part of UnfreezeBehindLoadingScreen, below).
// The game hides the world during the loading screen with the game viewport's bDisableWorldRendering (bit 1 of
// viewport +0x6C; UGameplayStatics::SetEnableWorldRendering, exe+0x31D8600, writes exactly that bit) and turns it back on
// right after fade-to-game. When UnfreezeBehindLoadingScreen unfreezes a transition (the same "loading idle" point as
// above) the bit is cleared, so the world is drawn behind the opaque loading screen while it runs; if the unfreeze call
// fails, the bit is set back at once. The fade gate's 0.5 s tick is skipped (its delegate's Execute / ExecuteIfSafe
// vtable slots, pass-through otherwise) until the world has run long enough (below), at most kHoldCapUs; then the
// game's own gate, fade-to-game and fade run as before. Drawing the frozen world without unfreezing it was tried as a
// separate switch; on its own it was not enough (the player still dropped into place in view), so it was removed.
//  - the gate delegate's thunks: add rcx,18h; jmp / call the gate body; their two .rdata slots (vtable exe+0x7D7BD78
//    + 11 / 12 * 8); the transition object is at the delegate instance + 0x18;
//  - UWorld::GetGameViewport (exe+0x385EE90) and the switch's tail (exe+0x31D86CA): call GetGameViewport; ...;
//    and ecx,~2 / xor edx,1 / add edx,edx / or edx,ecx / mov [rax+6Ch],edx.
//
// Prototype UnfreezeBehindLoadingScreen (off by default; a TRADE-OFF: the world runs before it is seen).
// After an interior entry the game unfreezes the world behind the loading screen as soon as the interior has loaded
// (its level-loaded handler, exe+0x48354B0, calls the transition's SetLoadingFreeze(off)), so the player stands in an
// idle pose and the area has rendered by the time the picture fades in. After an exit the freeze lasts until the game's
// unfreeze timer, which runs only once the fade-in has started: the player is seen landing into place. With the switch
// at N ms: when the area has loaded and the transition is still frozen, the game's own SetLoadingFreeze(transition, off)
// (exe+0x4887110) is called, the world is drawn behind the loading screen, and the fade gate's tick is skipped until
// the world has run N ms (cap as above). Only frames with the freeze subsystem actually out of its loading mode count,
// each at most kRunFrameCapUs: the old engine often stalls the game thread for a second or more right after the call,
// and the freeze ends a few frames after it. The game's unfreeze timer at the fade-in then finds nothing to do (it checks
// the same flag). Interiors (already unfrozen) are left alone.
//  - SetLoadingFreeze: its whole body by pattern; the flag it tests and clears (+0x200) and the second flag it clears
//    (+0x1F1) must be the fade hook's, and its "off" path must call the same two functions as the unfreeze timer body.
// UnfreezeBehindLoadingScreenFastTravel (, ms, 0 = UnfreezeBehindLoadingScreen's value): the hold above after a
// fast travel. A fast travel is recognised by the map menu's VMapMenuViewModel::DelayFastTravelRequestUntilFadeEnd(true),
// called once per map fast travel (UE4SS hook in a probe game; doors and teleports do not call it). Its exec thunk (found
// by the registered name, exe+0x468ACF0) reads the bool and calls the function (exe+0x4918830) at +0x6E; that call goes
// to a stub that notes the time when the argument is true and calls the same function. The next transition armed within
// kFastTravelUs counts as that fast travel. Not usable (measured): VLevelChangeData.bIsFastTravelling (stays false),
// bIsSeamlessTravelling (set on doors and teleports too), OnFadeToBlackOverBeforeFastTravel (not called).
// Loading a save gets the same hold. A load reads its save through the engine's save system (ISaveGameSystem's
// LoadGame, vtable +0x38, next to SaveGame at +0x28; this build: FGenericSaveGameSystem, LoadGame exe+0x35876B0), from
// the main menu as from the pause menu's Load, while a door, a gate or a fast travel reads none; the Load list itself
// reads none either (logged in play: two loads, each with one LoadGame of its slot, and doors without). So
// a transition counts as a save load when a save other than the settings' was read since the previous transition
// ended (the read comes before the arming from the main menu and between two armings from the pause menu; the check is
// made when the hold is needed). The save system is the one the settings save's writer uses: that function (found by
// pattern, HdrMenuRows's kSaveWriter) gets the platform features module and its save system (+0x40) and calls its
// SaveGame (+0x28); its LoadGame slot is redirected once, on the first frame after start-up, to a stub that notes the
// time and calls it. Not usable (seen in play): who arms the gate (the timer, the old engine's level change
// and the doors' path each carried both loads and doors), and a menu just before the arming (also before doors).
#include "common.h"


#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>

namespace opf {
namespace {

// gate body, from the GetWorld call (body + 0x30) to the async-loading check
constexpr char kGate[] =
    "FF 90 ?? ?? 00 00 48 8B C8 48 8B F8 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 ED 48 8B D8 48 85 C0 74 ?? E8 ?? ?? ?? ?? "
    "48 85 C0 74 ?? 48 8D 50 30 48 63 40 38 3B 43 38 7F ?? 48 8B C8 48 8B 43 30 48 39 14 C8 74 ?? 48 8B DD 48 8B D3 "
    "48 8D 8F ?? ?? 00 00 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B CF "
    "E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 84 C0 0F 85";
constexpr size_t kGateFromBody = 0x30;
constexpr char kGatePrologue[] = "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 81 EC A0 00 00 00";
// the gate-arming function: mov r11,rsp / push rbp,rsi,r14 / lea rbp,[r11-28h] / sub rsp,110h / cookie / ... /
// lock cmpxchg [rcx+disp32],r14d (once per transition)
constexpr char kArm[] =
    "4C 8B DC 55 56 41 56 49 8D 6B D8 48 81 EC 10 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 E0 45 33 F6 48 8B F1 "
    "33 C0 F0 44 0F B1 B1 ?? ?? 00 00";
constexpr size_t kArmPrologue = 11;  // mov r11,rsp / push rbp / push rsi / push r14 / lea rbp,[r11-28h]: nothing rip-relative
// loading-start handler: read the cached cvar's int, test the TOptional's flag, store value + flag
constexpr char kStore[] =
    "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? 00 00 00 80 BE ?? ?? 00 00 00 8B E8 75 ?? 48 8D 8E ?? ?? 00 00";
// ... then the streaming manager getter: call get-singleton / mov rcx,rax / call get-member / shl ebp,14h
constexpr char kManager[] = "E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? C1 E5 14 48 8B C8 48 63 D5 48 8B D8";
constexpr char kSingletonGetter[] = "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 75";
constexpr char kStreamAll[] =
    "40 53 48 83 EC 40 F3 0F 10 05 ?? ?? ?? ?? 48 8B D9 0F 29 74 24 30 0F 28 F1 0F 29 7C 24 20 F3 0F 10 3D";
constexpr char kUpdate[] = "48 89 5C 24 20 41 56 48 83 EC 30 0F 29 74 24 20 45 0F B6 F0 0F 28 F1 48 8B D9 E8";
constexpr int32_t kSetByCode = 0x0A000000;  // the flags the fade handler passes
constexpr size_t kCvarGetInt = 0x90, kCvarSet = 0x80, kMgrUpdate = 0x10;
constexpr uint64_t kGiveUpUs = 30000000;    // a transition never armed/cleared within 30 s is dropped
// the world drawing behind the loading screen (UnfreezeBehindLoadingScreen)
constexpr char kExec[] = "48 83 C1 18 E9 ?? ?? ?? ??";
constexpr char kExecIfSafe[] = "48 83 EC 28 48 83 C1 18 E8 ?? ?? ?? ?? B0 01 48 83 C4 28 C3";
constexpr size_t kGateCapture = 0x18;  // the transition object in the gate delegate instance
constexpr char kGetViewport[] =
    "48 83 EC 28 48 8B D1 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 74 0C 48 8B 80 ?? ?? 00 00 48 83 C4 28 C3";
constexpr char kEnableWorldRendering[] =
    "E8 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 48 ?? 40 0F B6 D6 83 E1 FD 83 F2 01 03 D2 0B D1 89 50";
constexpr uint32_t kDrawOffBit = 2;           // bDisableWorldRendering
constexpr uint64_t kHoldCapUs = 3000000;      // the gate is never held longer than this per transition
constexpr uint64_t kRunFrameCapUs = 50000;    // UnfreezeBehindLoadingScreen: one frame counts at most this much run time
// UnfreezeBehindLoadingScreen: VLevelChangeData::SetLoadingFreeze(bool) (exe+0x4887110), whole body
constexpr char kSetLoadingFreeze[] =
    "40 53 48 83 EC 20 0F B6 81 ?? ?? 00 00 48 8B D9 84 D2 74 ?? 84 C0 75 ?? 33 C9 E8 ?? ?? ?? ?? BA 01 00 00 00 48 8B C8 "
    "E8 ?? ?? ?? ?? C6 83 ?? ?? 00 00 01 48 83 C4 20 5B C3 84 C0 74 ?? 33 C9 E8 ?? ?? ?? ?? 33 D2 48 8B C8 E8 ?? ?? ?? ?? "
    "C6 83 ?? ?? 00 00 00 C6 83 ?? ?? 00 00 00";
constexpr size_t kSlfFlag = 9, kSlfGetSubsystem = 63, kSlfSetMode = 73, kSlfClearFlag = 80, kSlfClearFlag2 = 87;
// the unfreeze timer body's two calls (fade_hook.cpp's kUnfreeze): GetFreezeSubsystem at +21, RequestMode at +31
constexpr size_t kUnfreezeGetSubsystem = 21, kUnfreezeSetMode = 31;
using SetLoadingFreezeFn = void (*)(void* transition, bool freeze);
// The settings save's writer (as HdrMenuRows finds it): +0x1D a call, +0x22 the call to IPlatformFeaturesModule::Get,
// then mov rcx,rax / mov rdx,[rax] / call [rdx+40h] (GetSaveGameSystem); at +0x111 the call of the save system's
// SaveGame: ... 48 8B 07 mov rax,[rdi] ... FF 50 28 call [rax+28h]
constexpr char kSaveWriter[] =
    "48 89 5C 24 18 48 89 7C 24 20 55 48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00 48 8B D9 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 8B C8 48 8B 10 FF 52 40 48 8B F8 48 85 C0 0F 84";
constexpr char kSaveWriterSlot[] =
    "39 35 ?? ?? ?? ?? 48 8D 4C 24 30 48 8B 07 4C 8D 05 ?? ?? ?? ?? 4C 0F 45 05 ?? ?? ?? ?? 45 33 C9 48 89 4C 24 20 33 D2 "
    "48 8B CF FF 50 28";
constexpr size_t kSaveWriterGet = 0x22, kSaveWriterSlotAt = 0x111;
constexpr size_t kSaveSystemGetter = 0x40 / 8, kSaveGameSlot = 0x28 / 8, kLoadGameSlot = 0x38 / 8;  // vtable slots
using PlatformFeaturesGetFn = void* (*)();
using GetSaveSystemFn = void* (*)(void* module);
using LoadGameFn = bool (*)(void* self, bool attemptUi, const wchar_t* name, int32_t user, void* data);
// UnfreezeBehindLoadingScreenFastTravel: DelayFastTravelRequestUntilFadeEnd's exec thunk (one bool parameter read from
// the stack frame, Stack.Code advanced, then call the function with (this, the bool))
constexpr char kFastTravelExec[] =
    "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 33 FF 48 8B DA 89 7C 24 38 48 8B F1 E8 ?? ?? ?? ?? 48 8B CB 48 39 7B 20 "
    "74 10 48 8B 53 18 4C 8D 44 24 38 E8 ?? ?? ?? ?? EB 1C 4C 8B 83 88 00 00 00 48 8D 54 24 38 49 8B 40 18 48 89 83 88 00 00 "
    "00 E8 ?? ?? ?? ?? 48 8B 43 20 48 8B CE 8B 54 24 38 48 85 C0 40 0F 95 C7 48 03 F8 48 89 7B 20 E8 ?? ?? ?? ?? 48 8B 5C 24 30";
constexpr size_t kFastTravelCall = 0x6E;
constexpr uint64_t kFastTravelUs = 60000000;  // a fast-travel request counts for a transition armed within 60 s
using FastTravelDelayFn = void (*)(void* mapMenu, uint32_t delay);

using ExecFn = void (*)(void* delegateInstance);
using ExecIfSafeFn = bool (*)(void* delegateInstance);
using GameViewportFn = void* (*)(void* world);
using BoolOfFn = bool (*)(void*);
using BoolFn = bool (*)();
using GetIntFn = int32_t (*)(void* cvar);
using SetFn = void (*)(void* cvar, const wchar_t* value, int32_t flags);
using UpdateFn = void (*)(void* manager, float delta, bool processEverything);
using ArmFn = uint64_t (*)(void* transition);

struct State {
    bool found = false, installed = false;
    uint8_t* gateBody = nullptr;
    uint8_t* arm = nullptr;
    ArmFn origArm = nullptr;
    uint32_t getWorldSlot = 0, poolOffset = 0, poolSetOffset = 0, timerOffset = 0;
    BoolOfFn travelPending = nullptr;
    BoolFn* asyncLoading = nullptr;  // the global function pointer the gate calls
    void** cvarStatic = nullptr;     // the loading-start handler's cached r.Streaming.PoolSize
    void** singleton = nullptr;      // the streaming manager (collection)
    uint8_t* streamAll = nullptr;
    uint8_t* update = nullptr;
    std::atomic<bool> on{false};
    // the current transition (game thread)
    uint8_t* lcd = nullptr;
    bool started = false, seenArmed = false;
    uint64_t armUs = 0, startUs = 0, updateUs = 0;
    uint32_t updates = 0;
    // totals
    std::atomic<uint32_t> transitions{0}, streamed{0};
    std::atomic<uint64_t> leadUs{0};
    // the world drawing for UnfreezeBehindLoadingScreen (found / hooked optionally; the rest works without it)
    bool drawFound = false, drawHooked = false;
    void** execSlot = nullptr;
    void** execIfSafeSlot = nullptr;
    ExecFn origExec = nullptr;
    ExecIfSafeFn origExecIfSafe = nullptr;
    GameViewportFn gameViewport = nullptr;
    uint32_t drawFlagOffset = 0;
    // per transition (game thread)
    bool drawing = false;
    uint64_t drawStartUs = 0, holdStartUs = 0;
    uint32_t heldTicks = 0, reHidden = 0;
    std::atomic<uint32_t> drawnTransitions{0};
    // UnfreezeBehindLoadingScreen
    SetLoadingFreezeFn setLoadingFreeze = nullptr;
    uint32_t loadingFlagOffset = 0;
    std::atomic<int> unfreezeMs{0};
    bool unfrozen = false;    // per transition: we unfroze it (game thread)
    uint64_t unfreezeUs = 0;
    uint64_t runUs = 0, runTickUs = 0;  // frames run with the freeze off since then, and the last one's time
    uint64_t firstRunUs = 0;            // when the freeze was first seen off
    std::atomic<uint32_t> unfrozenTransitions{0};
    // UnfreezeBehindLoadingScreenFastTravel
    FastTravelDelayFn ftHandler = nullptr;  // the function the thunk calls (nullptr: fast travel not recognised)
    std::atomic<int> unfreezeFtMs{0};
    std::atomic<uint64_t> ftFadeUs{0};  // the last fast-travel request
    bool fastTravel = false;            // per transition (game thread)
    // save loads: the last save read (LoadGame of any slot but the settings') and the last transition's end
    PlatformFeaturesGetFn platformGet = nullptr;  // IPlatformFeaturesModule::Get (nullptr: save loads keep the door hold)
    LoadGameFn origLoadGame = nullptr;
    bool loadHookTried = false;
    std::atomic<uint64_t> saveReadUs{0};
    uint64_t doneUs = 0;
    std::atomic<uint32_t> saveLoads{0};
    std::atomic<uint32_t> fastTravels{0};
};
State g;

uint64_t NowUs()
{
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return QpcToMicroseconds(static_cast<uint64_t>(c.QuadPart), freq);
}

unsigned Rva(const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - Game().base); }

uint32_t Disp32(const uint8_t* at) { return *reinterpret_cast<const uint32_t*>(at); }

// An exec function (UFunction thunk) by its registered name. The engine's native function tables are {const char* name,
// function} pairs: the name's one copy in .rdata (a whole string), the pairs that point to it and to code (.rdata or
// .data, 8-byte aligned), and of their functions the one that matches `shape`. nullptr unless each is exactly one.
uint8_t* ExecByName(const char* name, const char* shape)
{
    const Pattern pattern = ParsePattern(shape);
    const GameModule& game = Game();
    const size_t len = strlen(name) + 1;
    const uint8_t* str = nullptr;
    int copies = 0;
    for (const uint8_t* p = game.rdata.begin + 1; p + len <= game.rdata.end; ++p) {
        p = static_cast<const uint8_t*>(memchr(p, name[0], static_cast<size_t>(game.rdata.end - p)));
        if (!p || p + len > game.rdata.end) break;
        if (p[-1] == 0 && memcmp(p, name, len) == 0) {
            str = p;
            ++copies;
        }
    }
    if (copies != 1) return nullptr;
    uint8_t* fn = nullptr;
    int pairs = 0;
    for (const Section* sec : {&game.rdata, &game.data}) {
        for (const uint8_t* q = sec->begin; q + 16 <= sec->end; q += 8) {
            uint8_t* const code = *reinterpret_cast<uint8_t* const*>(q + 8);
            if (*reinterpret_cast<const uint8_t* const*>(q) != str || !game.text.Contains(code) || !MatchesAt(code, pattern)) continue;
            fn = code;
            ++pairs;
        }
    }
    return pairs == 1 ? fn : nullptr;
}

bool Find(const GameModule& game)
{
    const auto gates = FindAll(game.text, ParsePattern(kGate));
    if (gates.size() != 1) { Log("[streamload] fade gate not found (%zu matches): not installed", gates.size()); return false; }
    uint8_t* const m = gates[0];
    uint8_t* const body = m - kGateFromBody;
    if (!MatchesAt(body, ParsePattern(kGatePrologue))) { Log("[streamload] fade gate prologue differs: not installed"); return false; }
    g.gateBody = body;
    g.getWorldSlot = Disp32(m + 2);
    g.travelPending = reinterpret_cast<BoolOfFn>(RipTarget(m + 110, 1, 5));
    g.asyncLoading = reinterpret_cast<BoolFn*>(RipTarget(m + 123, 2, 6));
    if (g.getWorldSlot == 0 || g.getWorldSlot > 0x800 || g.getWorldSlot % 8 || !game.text.Contains(reinterpret_cast<uint8_t*>(g.travelPending))) {
        Log("[streamload] fade gate layout differs (GetWorld +0x%X): not installed", g.getWorldSlot);
        return false;
    }
    FadeHookSites fade{};
    if (!FadeHookInfo(&fade)) { Log("[streamload] the fade-to-game hook's layout is not available: not installed"); return false; }
    g.timerOffset = fade.timerOffset;
    const auto arms = FindAll(game.text, ParsePattern(kArm));
    g.arm = arms.size() == 1 ? arms[0] : nullptr;
    bool armsGate = false;  // it uses the gate timer handle's offset
    for (uint8_t* p = g.arm; p && p < g.arm + 0x500; ++p)
        if (Disp32(p) == g.timerOffset) { armsGate = true; break; }
    if (!g.arm || !armsGate) { Log("[streamload] gate-arming function not found (%zu matches): not installed", arms.size()); return false; }
    // the save system's getter, from the settings save's writer (its SaveGame call checked)
    g.platformGet = nullptr;
    for (uint8_t* w : FindAll(game.text, ParsePattern(kSaveWriter)))
        if (MatchesAt(w + kSaveWriterSlotAt, ParsePattern(kSaveWriterSlot))) {
            uint8_t* const get = RipTarget(w + kSaveWriterGet, 1, 5);
            if (g.platformGet || !game.text.Contains(get)) { g.platformGet = nullptr; break; }  // two, or not code
            g.platformGet = reinterpret_cast<PlatformFeaturesGetFn>(get);
        }
    if (!g.platformGet) Log("[streamload] the settings save's writer not found: save loads keep the door hold");
    const auto stores = FindAll(game.text, ParsePattern(kStore));
    if (stores.size() != 1) { Log("[streamload] loading-start pool store not found (%zu matches): not installed", stores.size()); return false; }
    uint8_t* const s = stores[0];
    g.cvarStatic = reinterpret_cast<void**>(RipTarget(s, 3, 7));
    g.poolSetOffset = Disp32(s + 18);
    g.poolOffset = Disp32(s + 30);
    if (s[12] != kCvarGetInt || g.poolSetOffset != g.poolOffset + 4 || g.poolOffset > 0x1000) {
        Log("[streamload] loading-start pool store layout differs (value +0x%X, set +0x%X): not installed", g.poolOffset, g.poolSetOffset);
        return false;
    }
    uint8_t* mgr = nullptr;
    for (uint8_t* p : FindAll(game.text, ParsePattern(kManager))) if (p > s && p < s + 0x100) { mgr = p; break; }
    uint8_t* const getS = mgr ? RipTarget(mgr, 1, 5) : nullptr;
    if (!getS || !MatchesAt(getS, ParsePattern(kSingletonGetter))) { Log("[streamload] streaming manager getter not recognised: not installed"); return false; }
    g.singleton = reinterpret_cast<void**>(RipTarget(getS + 4, 3, 7));
    const auto streamAlls = FindAll(game.text, ParsePattern(kStreamAll));
    const auto updates = FindAll(game.text, ParsePattern(kUpdate));
    g.streamAll = streamAlls.size() == 1 ? streamAlls[0] : nullptr;
    g.update = updates.size() == 1 ? updates[0] : nullptr;
    bool calls = false;  // stream-all calls the update through vtable +0x10 (call [rax+10h]) near its start
    for (uint8_t* p = g.streamAll; p && p < g.streamAll + 0x60; ++p)
        if (p[0] == 0xFF && p[1] == 0x50 && p[2] == kMgrUpdate) { calls = true; break; }
    if (!g.streamAll || !g.update || !calls) { Log("[streamload] stream-all / its update not recognised: not installed"); return false; }
    // the world drawing (optional): the gate delegate's two slots, the viewport getter and the switch's bit
    {
        uint8_t* exec = nullptr;
        uint8_t* execIfSafe = nullptr;
        for (uint8_t* t : FindAll(game.text, ParsePattern(kExec))) if (RipTarget(t, 5, 9) == body) { exec = t; break; }
        for (uint8_t* t : FindAll(game.text, ParsePattern(kExecIfSafe))) if (RipTarget(t, 9, 13) == body) { execIfSafe = t; break; }
        auto slotOf = [&](const uint8_t* fn) -> void** {
            void** found = nullptr;
            int hits = 0;
            for (uint8_t* p = game.rdata.begin; fn && p + 8 <= game.rdata.end; p += 8)
                if (*reinterpret_cast<const uint8_t* const*>(p) == fn) { found = reinterpret_cast<void**>(p); ++hits; }
            return hits == 1 ? found : nullptr;
        };
        void** const es = slotOf(exec);
        void** const ess = slotOf(execIfSafe);
        const auto vps = FindAll(game.text, ParsePattern(kGetViewport));
        const auto sws = FindAll(game.text, ParsePattern(kEnableWorldRendering));
        uint8_t* const vp = vps.size() == 1 ? vps[0] : nullptr;
        uint8_t* const sw = sws.size() == 1 ? sws[0] : nullptr;
        if (es && ess && ess == es + 1 && vp && sw && RipTarget(sw, 1, 5) == vp && sw[0x1D] == sw[0x0C] && sw[0x0C] < 0x400) {
            g.execSlot = es;
            g.execIfSafeSlot = ess;
            g.origExec = reinterpret_cast<ExecFn>(exec);
            g.origExecIfSafe = reinterpret_cast<ExecIfSafeFn>(execIfSafe);
            g.gameViewport = reinterpret_cast<GameViewportFn>(vp);
            g.drawFlagOffset = sw[0x0C];
            g.drawFound = true;
            Log("[streamload] world drawing behind the loading screen: gate slots exe+0x%X/+0x%X, viewport getter exe+0x%X, world drawing switch "
                "bit 1 of viewport+0x%X (exe+0x%X)", Rva(es), Rva(ess), Rva(vp), g.drawFlagOffset, Rva(sw));
        } else {
            Log("[streamload] world drawing behind the loading screen: gate slots %p/%p, viewport getter %p, switch %p (or "
                "checks failed): UnfreezeBehindLoadingScreen not available", static_cast<void*>(es), static_cast<void*>(ess), vp, sw);
        }
    }
    // UnfreezeBehindLoadingScreen (optional; needs the draw/hold part): the game's SetLoadingFreeze, checked against the
    // fade hook's flags and the unfreeze timer's two calls
    if (g.drawFound) {
        const auto slfs = FindAll(game.text, ParsePattern(kSetLoadingFreeze));
        uint8_t* const slf = slfs.size() == 1 ? slfs[0] : nullptr;
        const uint8_t* const u = fade.unfreeze;
        if (slf && u && Disp32(slf + kSlfFlag) == fade.flagOffset && Disp32(slf + kSlfClearFlag) == fade.flagOffset &&
            Disp32(slf + kSlfClearFlag2) == fade.flag2Offset && u[kUnfreezeGetSubsystem] == 0xE8 && u[kUnfreezeSetMode] == 0xE8 &&
            RipTarget(slf + kSlfGetSubsystem, 1, 5) == RipTarget(const_cast<uint8_t*>(u) + kUnfreezeGetSubsystem, 1, 5) &&
            RipTarget(slf + kSlfSetMode, 1, 5) == RipTarget(const_cast<uint8_t*>(u) + kUnfreezeSetMode, 1, 5)) {
            g.setLoadingFreeze = reinterpret_cast<SetLoadingFreezeFn>(slf);
            g.loadingFlagOffset = fade.flagOffset;
            Log("[streamload] UnfreezeBehindLoadingScreen: SetLoadingFreeze exe+0x%X (loading flag +0x%X, same calls as the "
                "unfreeze timer exe+0x%X)", Rva(slf), g.loadingFlagOffset, Rva(u));
        } else {
            Log("[streamload] UnfreezeBehindLoadingScreen: SetLoadingFreeze %p (%zu matches) or its checks failed: not available",
                slf, slfs.size());
        }
    }
    g.found = true;
    Log("[streamload] gate-arming exe+0x%X, fade gate exe+0x%X (travel exe+0x%X, async-loading pointer exe+0x%X), pool TOptional "
        "+0x%X, cvar static exe+0x%X, streaming manager exe+0x%X (update exe+0x%X)", Rva(g.arm), Rva(body),
        Rva(reinterpret_cast<void*>(g.travelPending)), Rva(g.asyncLoading), g.poolOffset, Rva(g.cvarStatic), Rva(g.singleton),
        Rva(g.update));
    return true;
}

// The gate is armed (fade-to-game clears its timer handle). SEH-guarded.
bool Armed(const uint8_t* lcd)
{
    __try {
        return *reinterpret_cast<const uint64_t*>(lcd + g.timerOffset) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The gate's own async-loading and travel checks (pure queries). SEH-guarded.
bool LoadingIdle(uint8_t* lcd)
{
    __try {
        void** vt = *reinterpret_cast<void***>(lcd);
        void* const world = reinterpret_cast<void* (*)(void*)>(vt[g.getWorldSlot / 8])(lcd);
        return world && !g.travelPending(world) && !(*g.asyncLoading)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* Manager()
{
    __try {
        void* const mgr = *g.singleton;
        if (!mgr || (*reinterpret_cast<void***>(mgr))[kMgrUpdate / 8] != g.update) return nullptr;
        return mgr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Sets the stored pool size back (the fade handler's call); false if the game stored none. SEH-guarded.
bool RestorePool(uint8_t* lcd, int32_t* from, int32_t* to)
{
    __try {
        if (!lcd[g.poolSetOffset]) return false;
        void* const cvar = *g.cvarStatic;
        if (!cvar) return false;
        void** vt = *reinterpret_cast<void***>(cvar);
        *from = reinterpret_cast<GetIntFn>(vt[kCvarGetInt / 8])(cvar);
        *to = *reinterpret_cast<const int32_t*>(lcd + g.poolOffset);
        if (*to <= 0) return false;
        if (*to == *from) return true;
        wchar_t value[16];
        swprintf(value, 16, L"%d", *to);
        reinterpret_cast<SetFn>(vt[kCvarSet / 8])(cvar, value, kSetByCode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Update(void* mgr)
{
    __try {
        reinterpret_cast<UpdateFn>((*reinterpret_cast<void***>(mgr))[kMgrUpdate / 8])(mgr, 0.0f, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The transition world's game viewport field holding bDisableWorldRendering, or nullptr. SEH-guarded.
uint32_t* DrawFlag(uint8_t* lcd)
{
    __try {
        void** vt = *reinterpret_cast<void***>(lcd);
        void* const world = reinterpret_cast<void* (*)(void*)>(vt[g.getWorldSlot / 8])(lcd);
        if (!world) return nullptr;
        uint8_t* const vp = static_cast<uint8_t*>(g.gameViewport(world));
        return vp ? reinterpret_cast<uint32_t*>(vp + g.drawFlagOffset) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Shows (hide=false) or hides the world under the loading screen; returns whether the bit had to change. SEH-guarded.
bool SetWorldHidden(uint8_t* lcd, bool hide)
{
    uint32_t* const f = DrawFlag(lcd);
    if (!f) return false;
    __try {
        const uint32_t v = *f;
        const uint32_t want = hide ? (v | kDrawOffBit) : (v & ~kDrawOffBit);
        if (v == want) return false;
        *f = want;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The transition is still in its loading freeze (the flag SetLoadingFreeze and the unfreeze timer test). SEH-guarded.
bool LoadingFlag(const uint8_t* lcd)
{
    __try {
        return lcd[g.loadingFlagOffset] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game's SetLoadingFreeze(transition, off): what its level-loaded handler does for interiors. Game thread.
bool CallSetLoadingFreezeOff(uint8_t* lcd)
{
    __try {
        g.setLoadingFreeze(lcd, false);
        return lcd[g.loadingFlagOffset] == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The current transition is a save load: a save was read since the previous transition ended (game thread)
bool SaveLoad()
{
    return !g.fastTravel && g.saveReadUs.load(std::memory_order_relaxed) > g.doneUs;
}

// UnfreezeBehindLoadingScreen's hold for the current transition (ms): its fast-travel value after a fast travel or a
// save load
int UnfreezeMs()
{
    const int ums = g.unfreezeMs.load(std::memory_order_relaxed);
    const int fms = g.unfreezeFtMs.load(std::memory_order_relaxed);
    return ums > 0 && (g.fastTravel || SaveLoad()) && fms > 0 ? fms : ums;
}

// The save system's LoadGame: note a save's read, then the game's function
bool LoadGameHook(void* self, bool attemptUi, const wchar_t* name, int32_t user, void* data)
{
    if (name && wcscmp(name, L"Save_Settings") != 0) g.saveReadUs.store(NowUs(), std::memory_order_relaxed);
    return g.origLoadGame(self, attemptUi, name, user, data);
}

// Once, on the first frame after start-up (the module exists by then): the save system's LoadGame slot redirected
void InstallLoadGameHook()
{
    g.loadHookTried = true;
    if (!g.platformGet) return;
    void* const module = g.platformGet();
    void* const sys = module ? (*static_cast<GetSaveSystemFn**>(module))[kSaveSystemGetter](module) : nullptr;
    void** const vt = sys ? *static_cast<void***>(sys) : nullptr;
    const GameModule& game = Game();
    if (!vt || !game.rdata.Contains(vt) || !game.text.Contains(vt[kSaveGameSlot]) ||
        PrimaryFunction(game, static_cast<uint8_t*>(vt[kLoadGameSlot])) != vt[kLoadGameSlot]) {
        Log("[streamload] the save system is not as expected: save loads keep the door hold");
        return;
    }
    g.origLoadGame = reinterpret_cast<LoadGameFn>(vt[kLoadGameSlot]);
    if (!WritePointer(&vt[kLoadGameSlot], reinterpret_cast<void*>(&LoadGameHook))) {
        g.origLoadGame = nullptr;
        Log("[streamload] the save system's LoadGame slot could not be written: save loads keep the door hold");
        return;
    }
    Log("[streamload] save loads recognised by their save read: the save system's LoadGame exe+0x%X (vtable exe+0x%X)",
        Rva(reinterpret_cast<void*>(g.origLoadGame)), Rva(vt));
}

// The fast-travel thunk's call lands here (game thread): note the request, then the game's function as before.
void FastTravelFadeHook(void* mapMenu, uint32_t delay)
{
    if (delay & 0xFF) g.ftFadeUs.store(NowUs(), std::memory_order_relaxed);
    g.ftHandler(mapMenu, delay);
}

// The gate's tick, game thread: true = skip it this time (the world has not run long enough behind the loading
// screen). The gate is a looping timer: a skipped tick simply comes again 0.5 s later.
bool HoldGate(void* delegateInstance)
{
    if (!g.drawing || !g.lcd) return false;
    if (*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(delegateInstance) + kGateCapture) != g.lcd) return false;
    const int ums = UnfreezeMs();
    const uint64_t now = NowUs();
    const bool runWait = g.unfrozen && ums > 0 && g.runUs < static_cast<uint64_t>(ums) * 1000;
    if (!runWait) return false;
    if (!g.holdStartUs) g.holdStartUs = now;
    if (now - g.holdStartUs > kHoldCapUs) return false;
    ++g.heldTicks;
    return true;
}

void ExecHook(void* delegateInstance)
{
    if (HoldGate(delegateInstance)) return;
    g.origExec(delegateInstance);
}

bool ExecIfSafeHook(void* delegateInstance)
{
    if (HoldGate(delegateInstance)) return true;  // bound and safe; the tick just did nothing
    return g.origExecIfSafe(delegateInstance);
}

// jmp [rip+0]; dq target
void WriteAbsJump(uint8_t* at, const void* target)
{
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, jmp, sizeof jmp);
    memcpy(at + sizeof jmp, &target, sizeof target);
}

// The function's first 5 bytes become "jmp rel32 stub" in one atomic 8-byte write.
bool PatchEntry(uint8_t* fn, uint8_t* stub, bool beforeGameCode)
{
    const int64_t rel = stub - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t insn[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(insn + 1, &rel32, sizeof rel32);
    if ((reinterpret_cast<uintptr_t>(fn) & 7) == 0) {
        auto* q = reinterpret_cast<volatile LONG64*>(fn);
        DWORD old;
        if (!VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        const LONG64 before = *q;
        LONG64 after = before;
        memcpy(&after, insn, sizeof insn);
        const bool ok = InterlockedCompareExchange64(q, after, before) == before;
        VirtualProtect(fn, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, 8);
        return ok;
    }
    return beforeGameCode && WriteCode(fn, insn);
}

// The gate-arming function: the game's own work first, then remember the transition (game thread).
uint64_t ArmHook(void* transition)
{
    const uint64_t result = g.origArm(transition);
    if (g.on.load(std::memory_order_relaxed) && transition && transition != g.lcd) {
        g.lcd = static_cast<uint8_t*>(transition);
        g.started = g.seenArmed = false;
        g.updates = 0;
        g.updateUs = 0;
        g.drawing = false;
        g.drawStartUs = g.holdStartUs = 0;
        g.heldTicks = g.reHidden = 0;
        g.unfrozen = false;
        g.unfreezeUs = g.runUs = g.runTickUs = g.firstRunUs = 0;
        g.armUs = NowUs();
        const uint64_t ft = g.ftFadeUs.exchange(0, std::memory_order_relaxed);
        g.fastTravel = ft && g.armUs - ft < kFastTravelUs;
        if (g.fastTravel) g.fastTravels.fetch_add(1, std::memory_order_relaxed);
        g.transitions.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

}  // namespace

bool FindStreamBehindLoading(StreamBehindLoadingInfo* out)
{
    if (!g.found && !Find(Game())) return false;
    if (out) {
        out->gateBody = g.gateBody;
        out->arm = g.arm;
        out->platformGet = reinterpret_cast<void*>(g.platformGet);
        out->cvarStatic = g.cvarStatic;
        out->singleton = g.singleton;
        out->asyncLoading = reinterpret_cast<void**>(g.asyncLoading);
        out->travelPending = reinterpret_cast<uint8_t*>(g.travelPending);
        out->update = g.update;
        out->poolOffset = g.poolOffset;
        out->getWorldSlot = g.getWorldSlot;
        out->armHook = reinterpret_cast<void*>(&ArmHook);
        out->execSlot = g.execSlot;
        out->execIfSafeSlot = g.execIfSafeSlot;
        out->gameViewport = reinterpret_cast<uint8_t*>(g.gameViewport);
        out->drawFlagOffset = g.drawFlagOffset;
        out->setLoadingFreeze = reinterpret_cast<uint8_t*>(g.setLoadingFreeze);
    }
    return true;
}

bool InstallStreamBehindLoading(bool beforeGameCode, bool tradeOffs)
{
    if (g.installed) return true;
    if (!FindStreamBehindLoading(nullptr)) return false;
    constexpr size_t kRegion = 0x1000;
    uint8_t* const region = AllocNearGame(kRegion);
    if (!region) {
        Log("[streamload] no free memory near the game's code: not installed");
        return false;
    }
    WriteAbsJump(region, reinterpret_cast<const void*>(&ArmHook));
    memcpy(region + 0x20, g.arm, kArmPrologue);
    WriteAbsJump(region + 0x20 + kArmPrologue, g.arm + kArmPrologue);
    constexpr size_t kFastTravelStub = 0x80;
    WriteAbsJump(region + kFastTravelStub, reinterpret_cast<const void*>(&FastTravelFadeHook));
    DWORD old;
    VirtualProtect(region, kRegion, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, kRegion);
    g.origArm = reinterpret_cast<ArmFn>(region + 0x20);
    if (!PatchEntry(g.arm, region, beforeGameCode)) {
        Log("[streamload] could not detour the gate-arming function at exe+0x%X: not installed", Rva(g.arm));
        return false;
    }
    // The trade-off's redirections (the gate delegate's two slots, the fast-travel thunk's call, and on the first frame
    // the save system's LoadGame slot) only when UnfreezeBehindLoadingScreen is on at start-up: with the default the
    // only change to the game's code is the gate-arming detour above.
    g.loadHookTried = !tradeOffs;
    if (!tradeOffs) {
        g.installed = true;
        Log("[streamload] installed: textures of a new area stream behind the loading screen once its loading is done "
            "(UnfreezeBehindLoadingScreen 0 at start-up: its hooks not installed; takes a restart to switch on)");
        return true;
    }
    if (g.drawFound) {  // pass-through until UnfreezeBehindLoadingScreen holds a tick
        if (WritePointer(g.execSlot, reinterpret_cast<void*>(&ExecHook)) &&
            WritePointer(g.execIfSafeSlot, reinterpret_cast<void*>(&ExecIfSafeHook))) {
            g.drawHooked = true;
        } else {
            WritePointer(g.execSlot, reinterpret_cast<void*>(g.origExec));
            Log("[streamload] UnfreezeBehindLoadingScreen: writing the gate delegate's slots failed: not available");
        }
    }
    // UnfreezeBehindLoadingScreenFastTravel: the map's fast-travel request thunk, its call to the function via the stub
    uint8_t* const ft = FindExecByName("DelayFastTravelRequestUntilFadeEnd", kFastTravelExec);
    uint8_t* const ftTarget = ft ? RipTarget(ft + kFastTravelCall, 1, 5) : nullptr;
    const int64_t ftRel = ft ? (region + kFastTravelStub) - (ft + kFastTravelCall + 5) : 0;
    if (ftTarget && Game().text.Contains(ftTarget) && ftRel == static_cast<int32_t>(ftRel)) {
        g.ftHandler = reinterpret_cast<FastTravelDelayFn>(ftTarget);
        uint8_t call[5] = {0xE8};
        const int32_t rel32 = static_cast<int32_t>(ftRel);
        memcpy(call + 1, &rel32, sizeof rel32);
        if (WriteCode(ft + kFastTravelCall, call)) {
            Log("[streamload] fast travel recognised by the map's request thunk exe+0x%X (function exe+0x%X)", Rva(ft), Rva(ftTarget));
        } else {
            g.ftHandler = nullptr;
            Log("[streamload] the fast-travel thunk could not be redirected: UnfreezeBehindLoadingScreenFastTravel not available");
        }
    } else {
        Log("[streamload] the map's fast-travel request thunk not found: UnfreezeBehindLoadingScreenFastTravel not available");
    }
    g.installed = true;
    Log("[streamload] installed: textures of a new area stream behind the loading screen once its loading is done");
    return true;
}

void SetStreamBehindLoading(bool on)
{
    if (g.on.exchange(on) != on && g.installed)
        Log("[streamload] StreamBehindLoadingScreen %s", on ? "on" : "off (the game waits for the textures at the fade-in, as stock)");
}

void SetUnfreezeBehindLoading(int ms)
{
    ms = ms < 0 ? 0 : (ms > 3000 ? 3000 : ms);
    if (g.unfreezeMs.exchange(ms) != ms && g.installed)
        Log(g.drawHooked && g.setLoadingFreeze ? "[streamload] UnfreezeBehindLoadingScreen %d ms%s"
                                               : "[streamload] UnfreezeBehindLoadingScreen %d ms%s (not available)",
            ms, ms ? " (trade-off: after an exit the world runs behind the loading screen this long, as after an interior entry)" : " (off)");
}

void SetUnfreezeBehindLoadingFastTravel(int ms)
{
    ms = ms < 0 ? 0 : (ms > 3000 ? 3000 : ms);
    if (g.unfreezeFtMs.exchange(ms) != ms && g.installed)
        Log(g.ftHandler ? "[streamload] UnfreezeBehindLoadingScreenFastTravel %d ms%s"
                        : "[streamload] UnfreezeBehindLoadingScreenFastTravel %d ms%s (not available)",
            ms, ms ? " (after a fast travel or a save load, instead of UnfreezeBehindLoadingScreen's value)" : " (UnfreezeBehindLoadingScreen's value)");
}

void StreamBehindLoadingFrameEnd()
{
    if (g.installed && !g.loadHookTried) InstallLoadGameHook();
    if (!g.installed || !g.lcd) return;
    if (Armed(g.lcd)) {
        g.seenArmed = true;
    } else if (g.seenArmed || NowUs() - g.armUs > kGiveUpUs) {  // fade-to-game ran (it clears the gate): done
        const bool saveLoad = SaveLoad();
        if (saveLoad) g.saveLoads.fetch_add(1, std::memory_order_relaxed);
        if (g.started) {
            const uint64_t lead = NowUs() - g.startUs;
            g.leadUs.fetch_add(lead, std::memory_order_relaxed);
            Log("[streamload] fade-in %.0f ms after streaming started (%u updates, %.2f ms each)", lead / 1000.0, g.updates,
                g.updates ? g.updateUs / 1000.0 / g.updates : 0.0);
        }
        if (g.drawing) {
            Log("[streamload] UnfreezeBehindLoadingScreen: world drawn %.0f ms behind the loading screen before the fade-in; "
                "gate held %u tick(s) (%.0f ms); hidden again by the game %u time(s)", (NowUs() - g.drawStartUs) / 1000.0,
                g.heldTicks, g.holdStartUs ? (NowUs() - g.holdStartUs) / 1000.0 : 0.0, g.reHidden);
            if (g.unfrozen)
                Log("[streamload] UnfreezeBehindLoadingScreen: unfrozen %.0f ms before the fade-in, freeze seen off %.0f ms "
                    "later, world ran %.0f ms of frames behind the loading screen (%s, hold %d ms)", (NowUs() - g.unfreezeUs) / 1000.0,
                    g.firstRunUs ? (g.firstRunUs - g.unfreezeUs) / 1000.0 : -1.0, g.runUs / 1000.0,
                    g.fastTravel ? "fast travel" : saveLoad ? "save load" : "door or gate", UnfreezeMs());
            g.drawing = false;
        }
        g.unfrozen = false;
        g.lcd = nullptr;
        g.doneUs = NowUs();
        return;
    } else {
        return;  // armed a moment later
    }
    // UnfreezeBehindLoadingScreen: keep the running world drawn behind the loading screen (it starts below)
    if (g.drawing) {
        if (SetWorldHidden(g.lcd, false)) ++g.reHidden;
        if (g.unfrozen) {  // count the frames the world really runs (see the comment at the top)
            int32_t named = -1, mode = -1;
            const uint64_t now = NowUs();
            if (ReadMenuFreeze(&named, &mode) && !(mode == 1 && named == 0)) {
                if (!g.firstRunUs) g.firstRunUs = now;
                if (g.runTickUs) g.runUs += (std::min)(now - g.runTickUs, kRunFrameCapUs);
                g.runTickUs = now;
            } else {
                g.runTickUs = 0;
            }
        }
    }
    if (!g.on.load(std::memory_order_relaxed)) return;
    if (!g.started) {
        if (!LoadingIdle(g.lcd)) return;
        void* const mgr = Manager();
        int32_t from = 0, to = 0;
        if (!mgr || !RestorePool(g.lcd, &from, &to)) {
            Log("[streamload] %s: this transition left to the game", mgr ? "no stored pool size" : "streaming manager not recognised");
            g.lcd = nullptr;
            return;
        }
        g.started = true;
        g.startUs = NowUs();
        g.streamed.fetch_add(1, std::memory_order_relaxed);
        const bool unfreeze = g.setLoadingFreeze && UnfreezeMs() > 0 && LoadingFlag(g.lcd);
        if (g.drawHooked && unfreeze && DrawFlag(g.lcd)) {
            SetWorldHidden(g.lcd, false);
            // like an interior entry: the game's own SetLoadingFreeze(off), behind the loading screen
            if (CallSetLoadingFreezeOff(g.lcd)) {
                g.drawing = true;
                g.drawStartUs = g.startUs;
                g.drawnTransitions.fetch_add(1, std::memory_order_relaxed);
                g.unfrozen = true;
                g.unfreezeUs = NowUs();
                g.unfrozenTransitions.fetch_add(1, std::memory_order_relaxed);
            } else {
                SetWorldHidden(g.lcd, true);  // not unfrozen: hidden again, nothing held
                Log("[streamload] UnfreezeBehindLoadingScreen: the transition could not be unfrozen: left to the game");
            }
        }
    }
    void* const mgr = Manager();
    if (!mgr) return;
    const uint64_t t0 = NowUs();
    if (Update(mgr)) {
        g.updateUs += NowUs() - t0;
        ++g.updates;
    }
}

void LogStreamBehindLoadingStats()
{
    if (!g.installed) return;
    const uint32_t n = g.streamed.load();
    Log("[streamload] %s: %u transitions (%u fast travels, %u save loads), streamed early in %u (%.0f ms before the fade-in "
        "on average)", g.on.load() ? "on" : "off", g.transitions.load(), g.fastTravels.load(), g.saveLoads.load(), n,
        n ? g.leadUs.load() / 1000.0 / n : 0.0);
}

uint8_t* FindExecByName(const char* name, const char* shape) { return ExecByName(name, shape); }

}  // namespace opf
