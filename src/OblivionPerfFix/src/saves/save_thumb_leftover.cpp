// SaveThumbLeftoverFix (worker shot130q 2026-11-09, findings "shot130q"; a game bug fix, also without the mod; user
// 2026-11-09: ships on): a save's thumbnail binding made on the menus' screenshot service is removed once its callback ran.
//
// The game bug (build 19115871 addresses): UVAltarSaveSystem's save function (exe+0x48CBAE0) binds the save's thumbnail
// callback (exe+0x48AF8A0, a raw delegate whose object is the save system's holder +0x180) either on the engine's
// ScreenshotCapturedDelegate (exe+0x8E50A28; no menu holds a capture) or, while a menu holds a capture (the fast-travel
// autosave, a save from the System page), on Altar's screenshot service's handlers' delegate (+0x370, the AddRaw at
// exe+0x48CBE20). Both store the handle in holder+0x18. The callback, when it has copied the picture, removes holder+0x18
// from the ENGINE delegate (lea +0x55, call +0x5C): right for the first kind, the wrong delegate for the second, so that
// binding stays on the service for the rest of the process, one more per such save, and every later menu screenshot (and
// cached re-request) copies the picture into the finished save's holder once per leftover (33 MB at 4K on the game thread).
//
// The fix: the service branch's AddRaw call is redirected: the game's AddRaw, then the binding's delegate and handle are
// remembered (only the latest: holder+0x18 can only hold the latest save's handle). The callback's Remove call is
// redirected: the game's Remove on the engine delegate as before, then, when the handle is the remembered service binding
// (so this is that binding's own save, its picture just copied) and we are on the thread that bound it (the game thread),
// the game's Remove on the service's delegate too, the removal the callback meant. Both delegates are the same type (the
// save function uses the same AddRaw for both, checked), so the same Remove applies. Inside the service's broadcast (it
// is the one calling the callback) the game's Remove unbinds the entry in place (its instance freed, the slot left empty)
// and does not compact while the broadcast holds the lock (UE 5.3 TMulticastDelegateBase::RemoveDelegateInstance /
// CompactInvocationList); the empty slot goes at the delegate's next compaction (a later Add, Remove or broadcast that
// meets it), and every reader of the list skips empty slots. The same path the menu backdrop's handler takes when it
// unbinds itself from this delegate inside the same broadcast. The raw delegate's ExecuteIfSafe (found once at start-up
// by its whole code) touches nothing of its instance after the call; the entry must use it.
// A binding is only removed from inside its own callback, after its save got the picture: never a pending save's.
// Bindings made before the fix was on (live off/on) stay as in the game (GpuMenuShot's leftover rule handles them).
// What changes: later menu screenshots no longer copy into the finished save's buffer (nothing reads it before the next
// save resets it); gameplay and thumbnails are the same. Start-up only (hooks), then live 0/1.
#include "common.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace opf {
namespace {

// The save function's service branch (exe+0x48CBDFD): cmp byte [rbx+388h],0 (the service holds a capture) / je /
// lea r9,[callback] (+0x0D) / mov r8,r14 (the holder) / lea rdx,[rsp+..] / lea rcx,[rbx+370h] (the service's handlers'
// delegate) / call AddRaw (+0x23) / mov r8,[rax] / mov [rdi+198h],r8 (holder+0x18 = the handle).
constexpr char kServiceBind[] =
    "80 BB 88 03 00 00 00 0F 84 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 4D 8B C6 48 8D 54 24 ?? 48 8D 8B 70 03 00 00 E8 ?? ?? ?? ?? "
    "4C 8B 00 4C 89 87 98 01 00 00";
constexpr size_t kSbCallbackLea = 0x0D, kSbAddRawCall = 0x23;
// The engine branch in the same function: lea r9,[callback] / mov r8,r14 / lea rdx,[rsp+..] / lea rcx,[engine delegate] /
// call AddRaw (+0x14): the same AddRaw for both delegates (same delegate type).
constexpr char kEngineBind[] = "4C 8D 0D ?? ?? ?? ?? 4D 8B C6 48 8D 54 24 ?? 48 8D 0D ?? ?? ?? ?? E8";
constexpr size_t kEbDelegateLea = 0x0F, kEbAddRawCall = 0x16;
constexpr size_t kEngineBindSearch = 0x600;  // from the service branch, within the save function's code
// The callback up to its Remove: holder W/H, the copy under the lock, mov rdx,[rdi+18h] / test / jz / lea rcx,[engine
// delegate] (+0x55) / call Remove (+0x5C).
constexpr char kCallback[] =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 89 54 24 30 48 8B F9 44 89 44 24 34 49 8B D9 48 8B 44 24 30 48 89 01 "
    "48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8D 4F 08 48 8B D3 E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? "
    "48 8B 57 18 48 85 D2 74 0C 48 8D 0D ?? ?? ?? ?? E8";
constexpr size_t kCbDelegateLea = 0x55, kCbRemoveCall = 0x5C;
// A raw method delegate's ExecuteIfSafe, whole: the function [instance+20h] called with the object, then only registers
// restored (the instance is not touched after the call, so the callback may unbind it).
constexpr char kRawExec[] =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B 41 18 8B F2 48 8B E9 48 89 44 24 30 48 8D 54 24 30 "
    "49 8B D9 48 8D 4C 24 30 41 8B F8 E8 ?? ?? ?? ?? 4C 8B CB 44 8B C7 8B D6 48 8B C8 FF 55 20 48 8B 5C 24 38 B0 01 48 8B 6C "
    "24 40 48 8B 74 24 48 48 83 C4 20 5F C3";
constexpr size_t kRawExecAnchor = 0x42;  // its call of the function ([rbp+20h])
constexpr size_t kDelegateExecuteIfSafe = 0x60, kInstanceHandle = 0x10;

using AddRawFn = uint64_t* (*)(void* delegate, uint64_t* outHandle, void* object, void* fn);
using RemoveFn = bool (*)(void* delegate, uint64_t handle);

AddRawFn g_addRaw = nullptr;
RemoveFn g_remove = nullptr;
const uint8_t* g_rawExec = nullptr;  // the raw delegate's ExecuteIfSafe (found once at start-up, kRawExec)
uint8_t* g_removeThunk = nullptr;
uint8_t* g_engineDelegate = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{false};
std::mutex g_lock;
uint8_t* g_bindDelegate = nullptr;  // the latest service binding (delegate, handle, binding thread)
uint64_t g_bindHandle = 0;
DWORD g_bindThread = 0;
std::atomic<uint64_t> s_bound{0}, s_removed{0}, s_notFound{0};

uint8_t* FindOne(const Section& s, const char* pattern, int* count)
{
    const auto hits = FindAll(s, ParsePattern(pattern));
    *count = static_cast<int>(hits.size());
    return hits.size() == 1 ? hits[0] : nullptr;
}

// A jump to target in memory within rel32 reach of the game's code.
uint8_t* JumpThunk(void* target)
{
    uint8_t* const region = AllocNearGame(16);
    if (!region) return nullptr;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &target, sizeof target);
    DWORD old;
    VirtualProtect(region, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 16);
    return region;
}

// A call's rel32 pointed at thunk (start-up, before game code runs).
bool RedirectSite(uint8_t* call, uint8_t* thunk)
{
    const int64_t rel = thunk - (call + 5);
    if (call[0] != 0xE8 || rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}

uint64_t* AddRawHook(void* delegate, uint64_t* outHandle, void* object, void* fn)
{
    uint64_t* const r = g_addRaw(delegate, outHandle, object, fn);
    if (g_on.load(std::memory_order_relaxed) && r && *r) {
        std::lock_guard<std::mutex> lk(g_lock);
        g_bindDelegate = static_cast<uint8_t*>(delegate);
        g_bindHandle = *r;
        g_bindThread = GetCurrentThreadId();
        s_bound.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

}  // namespace

// The entry of a delegate's invocation list bound with this handle (nullptr: none): instance pointer.
uint8_t* SaveThumbLeftoverFind(const uint8_t* delegate, uint64_t handle)
{
    if (!delegate || handle == 0) return nullptr;
    uint8_t* const* const list = *reinterpret_cast<uint8_t* const* const*>(delegate);
    const int32_t num = *reinterpret_cast<const int32_t*>(delegate + 8);
    for (int32_t i = 0; i < num; ++i) {
        const uint8_t* const e = reinterpret_cast<const uint8_t*>(list) + 16ll * i;
        uint8_t* const inst = *reinterpret_cast<uint8_t* const*>(e);
        if (*reinterpret_cast<const int32_t*>(e + 8) != 0 && inst && *reinterpret_cast<const uint64_t*>(inst + kInstanceHandle) == handle)
            return inst;
    }
    return nullptr;
}

// The decision, apart from the game's memory (self-test): remove from the remembered service delegate?
bool SaveThumbLeftoverShouldRemove(bool on, uint64_t handle, uint64_t boundHandle, DWORD boundThread, DWORD thread, bool foundRawExec)
{
    return on && handle != 0 && handle == boundHandle && boundThread == thread && foundRawExec;
}

namespace {
bool RemoveHook(void* delegate, uint64_t handle)
{
    const bool r = g_remove(delegate, handle);  // the game's Remove on the engine delegate, as the callback does
    if (!g_on.load(std::memory_order_relaxed) || handle == 0) return r;
    uint8_t* svc = nullptr;
    uint64_t bh = 0;
    DWORD bt = 0;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        svc = g_bindDelegate;
        bh = g_bindHandle;
        bt = g_bindThread;
    }
    if (handle != bh || bt != GetCurrentThreadId() || !svc) return r;
    uint8_t* const inst = SaveThumbLeftoverFind(svc, handle);
    const bool raw = inst && g_rawExec &&
                     *reinterpret_cast<uint8_t* const*>(*reinterpret_cast<uint8_t* const*>(inst) + kDelegateExecuteIfSafe) == g_rawExec;
    if (!SaveThumbLeftoverShouldRemove(true, handle, bh, bt, GetCurrentThreadId(), raw)) {
        s_notFound.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_bindHandle == handle) { g_bindDelegate = nullptr; g_bindHandle = 0; g_bindThread = 0; }
    }
    if (g_remove(svc, handle)) s_removed.fetch_add(1, std::memory_order_relaxed);
    return r;
}
}  // namespace

// SaveThumbnailAsync's check of the callback's Remove: the game's Remove when p is our redirect, else p.
uint8_t* SaveThumbLeftoverResolve(uint8_t* p)
{
    return (g_removeThunk && p == g_removeThunk) ? reinterpret_cast<uint8_t*>(g_remove) : p;
}

// Sites on the running game or a mapped exe (self-test). nullptr = all found.
const char* SaveThumbLeftoverDiscover(const GameModule& game, SaveThumbLeftoverSites* out)
{
    int n = 0;
    uint8_t* const site = FindOne(game.text, kServiceBind, &n);
    if (!site) return "the save function's service binding";
    uint8_t* const callback = RipTarget(site + kSbCallbackLea, 3, 7);
    uint8_t* const addRawCall = site + kSbAddRawCall;
    uint8_t* const addRaw = RipTarget(addRawCall, 1, 5);
    if (!game.text.Contains(callback) || !MatchesAt(callback, ParsePattern(kCallback))) return "the save's thumbnail callback";
    if (addRawCall[0] != 0xE8 || !game.text.Contains(addRaw)) return "the service binding's AddRaw call";
    uint8_t* const engine = RipTarget(callback + kCbDelegateLea, 3, 7);
    uint8_t* const removeCall = callback + kCbRemoveCall;
    uint8_t* const remove = SaveThumbLeftoverResolve(RipTarget(removeCall, 1, 5));
    if (!game.data.Contains(engine) || removeCall[0] != 0xE8 || !game.text.Contains(remove)) return "the callback's Remove call";
    // The engine branch binds the same callback with the same AddRaw on the engine delegate: one delegate type.
    bool same = false;
    const Pattern eb = ParsePattern(kEngineBind);
    for (uint8_t* q = site; q < site + kEngineBindSearch && q + 0x1B <= game.text.end && !same; ++q)
        if (MatchesAt(q, eb) && RipTarget(q, 3, 7) == callback && RipTarget(q + kEbDelegateLea, 3, 7) == engine &&
            RipTarget(q + kEbAddRawCall, 1, 5) == addRaw)
            same = true;
    if (!same) return "the engine branch's binding with the same AddRaw";
    // Searched from its call of the function (anchor; a search started at its 48 prologue stops millions of times).
    uint8_t* rawExec = nullptr;
    {
        const Pattern full = ParsePattern(kRawExec);
        Pattern tail;
        tail.bytes.assign(full.bytes.begin() + kRawExecAnchor, full.bytes.end());
        tail.care.assign(full.care.begin() + kRawExecAnchor, full.care.end());
        n = 0;
        for (uint8_t* h : FindAll(game.text, tail))
            if (h - kRawExecAnchor >= game.text.begin && MatchesAt(h - kRawExecAnchor, full)) { rawExec = h - kRawExecAnchor; ++n; }
        if (n != 1) rawExec = nullptr;
    }
    if (!rawExec) return "the raw delegate's ExecuteIfSafe";
    if (out) {
        out->rawExec = rawExec;
        out->serviceBind = site;
        out->addRawCall = addRawCall;
        out->callback = callback;
        out->removeCall = removeCall;
        out->engineDelegate = engine;
    }
    return nullptr;
}

bool InstallSaveThumbLeftoverFix(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!beforeGameCode) { Log("[thumbleft] can only be installed at start-up (version.dll), not installed"); return false; }
    const GameModule& game = Game();
    SaveThumbLeftoverSites s{};
    if (const char* why = SaveThumbLeftoverDiscover(game, &s)) {
        Log("[thumbleft] %s not found: SaveThumbLeftoverFix not installed (the game's leftover bindings stay)", why);
        return false;
    }
    g_addRaw = reinterpret_cast<AddRawFn>(RipTarget(s.addRawCall, 1, 5));
    g_remove = reinterpret_cast<RemoveFn>(SaveThumbLeftoverResolve(RipTarget(s.removeCall, 1, 5)));
    g_engineDelegate = s.engineDelegate;
    g_rawExec = s.rawExec;
    uint8_t* const addThunk = JumpThunk(reinterpret_cast<void*>(&AddRawHook));
    uint8_t* const removeThunk = JumpThunk(reinterpret_cast<void*>(&RemoveHook));
    if (!addThunk || !removeThunk) { Log("[thumbleft] no memory near the game's code: SaveThumbLeftoverFix not installed"); return false; }
    // The Remove redirect first: with only it in place, nothing is remembered, so it passes through.
    g_removeThunk = removeThunk;
    if (!RedirectSite(s.removeCall, removeThunk)) {
        g_removeThunk = nullptr;
        Log("[thumbleft] the callback's Remove call could not be redirected: SaveThumbLeftoverFix not installed");
        return false;
    }
    if (!RedirectSite(s.addRawCall, addThunk)) {
        Log("[thumbleft] the service binding's AddRaw call could not be redirected: SaveThumbLeftoverFix not installed (the "
            "Remove redirect passes through)");
        return false;
    }
    g_installed = true;
    Log("[thumbleft] save function's service binding exe+0x%X (AddRaw call exe+0x%X), callback exe+0x%X (Remove call exe+0x%X), "
        "engine delegate exe+0x%X: installed", static_cast<unsigned>(s.serviceBind - game.base),
        static_cast<unsigned>(s.addRawCall - game.base), static_cast<unsigned>(s.callback - game.base),
        static_cast<unsigned>(s.removeCall - game.base), static_cast<unsigned>(s.engineDelegate - game.base));
    return true;
}

void SetSaveThumbLeftoverFix(bool on)
{
    if (!g_installed) return;
    if (g_on.exchange(on) != on)
        Log("[thumbleft] SaveThumbLeftoverFix %s", on ? "on: a save's thumbnail binding on the menus' screenshot service is removed once its callback ran"
                                                       : "off: those bindings stay for the session, as in the game");
}

void LogSaveThumbLeftoverStats()
{
    if (!g_installed) return;
    const uint64_t b = s_bound.exchange(0), r = s_removed.exchange(0), x = s_notFound.exchange(0);
    if (b || r || x)
        Log("[thumbleft] service bindings: %llu made, %llu removed after their callback, %llu not found where expected",
            static_cast<unsigned long long>(b), static_cast<unsigned long long>(r), static_cast<unsigned long long>(x));
}

// Self-test: the two hooks' sequence with fake game functions and delegates (nothing installed). setup: the game's
// AddRaw / Remove stand-ins, the raw ExecuteIfSafe address and the switch; clear: back to the not-installed state.
void SaveThumbLeftoverTestSetup(void* addRaw, void* remove, const uint8_t* rawExec, bool on)
{
    std::lock_guard<std::mutex> lk(g_lock);
    g_addRaw = reinterpret_cast<AddRawFn>(addRaw);
    g_remove = reinterpret_cast<RemoveFn>(remove);
    g_rawExec = rawExec;
    g_on.store(on);
    g_bindDelegate = nullptr;
    g_bindHandle = 0;
    g_bindThread = 0;
}
uint64_t* SaveThumbLeftoverTestAddRaw(void* delegate, uint64_t* out, void* object, void* fn) { return AddRawHook(delegate, out, object, fn); }
bool SaveThumbLeftoverTestRemove(void* delegate, uint64_t handle) { return RemoveHook(delegate, handle); }
uint64_t SaveThumbLeftoverTestNoted()
{
    std::lock_guard<std::mutex> lk(g_lock);
    return g_bindHandle;
}

}  // namespace opf
