// HorseFadeCrashGuard: crash after a ride, when the camera goes to third person on foot (game bug, issue #38).
//
// The old engine's third-person camera fades the player, and the horse being ridden, when the camera comes close to
// them (zoomed in, or pushed in by a wall or a tree). The fade function (build 19115871: exe+0x660BDC0) remembers the
// faded horse in one raw global pointer (exe+0x947FB08) so that it can fade the horse back in once the player is on
// foot: it sets the pointer while mounted and fading (exe+0x660C35A), and on foot it calls the horse's alpha update
// (exe+0x65CEC50, through the horse's vtable +0x580) every third-person frame until the horse's fade is back at 1.0,
// then clears the pointer (exe+0x660C3BD). The game also clears it in first person while mounted and in the player's
// destructor, but nothing clears it when the horse itself is destroyed. So: ride with the camera close enough to fade,
// get off and be in first person before the fade is back, then let the horse go from memory (a save loaded, a new
// game) and switch to third person on foot: the camera calls the alpha update through the freed horse and the game
// crashes on the OblivionThread at exe+0x65CEC86 (it also happens without OblivionPerfFix).
//
// The guard takes over the entry of the base reference destructor (TESObjectREFR's, exe+0x65785C0; every reference,
// horses included, ends its destruction there): if the reference being destroyed is the one the camera remembers,
// the pointer is cleared (compare-exchange, so a pointer the camera has just changed is left alone), then the game's
// destructor runs unchanged. The camera then has no horse to fade back, which is exactly the state it reaches when
// the fade has finished; a live horse is never touched, so every fade of a horse that still exists is the game's.
// Found by byte pattern, checked (the camera's three uses of the pointer read the same global and call the same
// function; the destructor stores TESObjectREFR's vtable, checked through RTTI), installed only before game code runs
// (an entry jump), then switched live.
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace opf {
namespace {

// The camera fade's horse branch (build 19115871: exe+0x660C347):
//   F3 0F 10 05 ????????     movss xmm0,[fade]
//   48 8B CB                 mov rcx,rbx                 (rbx = the horse being ridden)
//   F3 0F 11 05 ????????     movss [horse fade],xmm0
//   48 89 1D ????????        mov [faded horse],rbx       (+0x13: set while mounted)
//   E8 ????????              call alpha update           (+0x1A)
//   EB ?? 48 85 DB 75 ??     jmp; test rbx,rbx; jne
//   48 8B 0D ????????        mov rcx,[faded horse]       (+0x26: on foot)
//   48 85 C9 74 ??           test rcx,rcx; je
//   ... fade += rate * dt, capped at 1.0 ...
//   E8 ????????              call alpha update           (+0x62: the crash's call, through the freed horse)
//   F3 0F 10 05 ???????? 0F 2E C7 7A ?? 75 ??   fade == 1.0 ?
//   48 C7 05 ???????? 00000000   mov qword [faded horse],0   (+0x76)
constexpr const char* kFade =
    "F3 0F 10 05 ?? ?? ?? ?? 48 8B CB F3 0F 11 05 ?? ?? ?? ?? 48 89 1D ?? ?? ?? ?? E8 ?? ?? ?? ?? EB ?? "
    "48 85 DB 75 ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? F3 0F 10 0D ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? "
    "F3 41 0F 59 ?? F3 0F 58 C8 0F 2F ?? F3 0F 11 0D ?? ?? ?? ?? 76 ?? C7 05 ?? ?? ?? ?? 00 00 80 3F "
    "E8 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? 0F 2E ?? 7A ?? 75 ?? 48 C7 05 ?? ?? ?? ?? 00 00 00 00";
constexpr size_t kFadeSet = 0x13, kFadeCall1 = 0x1A, kFadeLoad = 0x26, kFadeCall2 = 0x62, kFadeClear = 0x76;

// TESObjectREFR's destructor (build 19115871: exe+0x65785C0), the base every reference's destructor ends in:
//   48 89 5C 24 08           mov [rsp+8],rbx             (the 5 bytes the entry jump replaces)
//   48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8B D9
//   48 8D 05 ????????        lea rax,[TESObjectREFR vtable]   (+0x1F)
//   48 89 01 48 8D 05 ???????? 48 89 41 30 48 8D 05 ???????? 48 89 41 38
constexpr const char* kDtor =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8B D9 "
    "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 41 30 48 8D 05 ?? ?? ?? ?? 48 89 41 38";
constexpr size_t kDtorEntry = 5, kDtorVtableLea = 0x1F;

using DtorFn = void* (*)(void* ref, void* a2, void* a3, void* a4);

void* volatile* g_faded = nullptr;   // the camera's faded-horse pointer (exe+0x947FB08)
DtorFn g_tramp = nullptr;            // the destructor's first 5 bytes, then a jump back
uint8_t* g_dtor = nullptr;
uint8_t* g_fade = nullptr;
uint8_t* g_mem = nullptr;
std::atomic<bool> g_enabled{true};
std::atomic<uint64_t> g_cleared{0};
uint64_t g_clearedLogged = 0;

// ---- RTTI (MSVC x64: image-relative 32-bit offsets) ----
struct Col {  // _RTTICompleteObjectLocator, at vtable[-1]
    uint32_t signature, offset, cdOffset, typeDescriptor, hierarchy, self;
};

const uint8_t* TypeOf(const GameModule& game, const void* const* vtable)
{
    if (!game.rdata.Contains(vtable - 1) || !game.rdata.Contains(vtable)) return nullptr;
    const auto* col = static_cast<const Col*>(vtable[-1]);
    if (!game.rdata.Contains(col) || !game.rdata.Contains(reinterpret_cast<const uint8_t*>(col) + sizeof(Col) - 1)) return nullptr;
    if (col->signature != 1 || col->offset != 0 || game.base + col->self != reinterpret_cast<const uint8_t*>(col)) return nullptr;
    return game.base + col->typeDescriptor;
}

// The type descriptor whose name is `mangled` (".?AVTESObjectREFR@@"): name at +0x10, in .data.
const uint8_t* FindTypeDescriptor(const GameModule& game, const char* mangled)
{
    std::string pattern;
    for (const char* c = mangled;; ++c) {
        char hex[4];
        sprintf_s(hex, "%02X ", static_cast<unsigned char>(*c));
        pattern += hex;
        if (!*c) break;
    }
    const auto hits = FindAll(game.data, ParsePattern(pattern));
    return hits.size() == 1 ? hits[0] - 0x10 : nullptr;
}

void* DtorHook(void* ref, void* a2, void* a3, void* a4)
{
    // A plain read first: almost every destroyed reference is not the faded horse (and the pointer is usually null).
    if (ref && g_enabled.load(std::memory_order_relaxed) && *g_faded == ref &&
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(g_faded), nullptr, ref) == ref)
        g_cleared.fetch_add(1, std::memory_order_relaxed);
    return g_tramp(ref, a2, a3, a4);
}

bool Discover(const GameModule& game, HorseFadeGuardInfo* d, const char** why)
{
    const auto fades = FindAll(game.text, ParsePattern(kFade));
    if (fades.size() != 1) { *why = fades.empty() ? "camera fade code not found" : "camera fade code found more than once"; return false; }
    uint8_t* const f = fades[0];
    uint8_t* const set = RipTarget(f + kFadeSet, 3, 7);
    uint8_t* const load = RipTarget(f + kFadeLoad, 3, 7);
    uint8_t* const clear = RipTarget(f + kFadeClear, 3, 11);
    uint8_t* const call1 = RipTarget(f + kFadeCall1, 1, 5);
    uint8_t* const call2 = RipTarget(f + kFadeCall2, 1, 5);
    if (set != load || set != clear || !game.data.Contains(set) || reinterpret_cast<uintptr_t>(set) % 8 != 0) {
        *why = "the camera's uses of the faded-horse pointer differ";
        return false;
    }
    if (call1 != call2 || !game.text.Contains(call1)) { *why = "the camera's two alpha updates differ"; return false; }

    const auto dtors = FindAll(game.text, ParsePattern(kDtor));
    if (dtors.size() != 1) { *why = dtors.empty() ? "reference destructor not found" : "reference destructor found more than once"; return false; }
    uint8_t* const dtor = dtors[0];
    const auto* const vtable = reinterpret_cast<const void* const*>(RipTarget(dtor + kDtorVtableLea, 3, 7));
    const uint8_t* const refType = FindTypeDescriptor(game, ".?AVTESObjectREFR@@");
    if (!refType || TypeOf(game, vtable) != refType) { *why = "the destructor's vtable is not TESObjectREFR's (RTTI)"; return false; }
    d->fade = f;
    d->faded = reinterpret_cast<void**>(set);
    d->alphaUpdate = call1;
    d->dtor = dtor;
    d->vtable = vtable;
    return true;
}

}  // namespace

bool HorseFadeGuardDiscover(const GameModule& game, HorseFadeGuardInfo* out)
{
    const char* why = "";
    return Discover(game, out, &why);
}

bool ApplyHorseFadeGuard()
{
    const GameModule& game = Game();
    if (g_tramp) { Log("[horsefade] crash guard already installed"); return true; }
    HorseFadeGuardInfo d{};
    const char* why = "";
    if (!Discover(game, &d, &why)) { Log("[horsefade] %s: crash guard not installed", why); return false; }
    if (ForeignJumpAt(d.dtor)) { Log("[horsefade] another program has hooked the reference destructor: crash guard not installed"); return false; }
    uint8_t* const mem = AllocNearGame(64);
    if (!mem) { Log("[horsefade] no memory near the game's code: crash guard not installed"); return false; }
    // mem+0: trampoline (the destructor's first 5 bytes, jmp [rip] back); +32: jmp [rip] to DtorHook
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = d.dtor + kDtorEntry;
    const void* const hook = reinterpret_cast<void*>(&DtorHook);
    memcpy(mem, d.dtor, kDtorEntry);
    memcpy(mem + kDtorEntry, jmpAbs, 6);
    memcpy(mem + kDtorEntry + 6, &resume, 8);
    memcpy(mem + 32, jmpAbs, 6);
    memcpy(mem + 32 + 6, &hook, 8);
    DWORD old;
    if (!VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old)) { Log("[horsefade] could not make the stub executable: crash guard not installed"); return false; }
    FlushInstructionCache(GetCurrentProcess(), mem, 64);
    const int64_t rel = (mem + 32) - (d.dtor + 5);
    if (rel != static_cast<int32_t>(rel)) { Log("[horsefade] stub out of reach: crash guard not installed"); return false; }
    g_faded = reinterpret_cast<void* volatile*>(d.faded);
    g_tramp = reinterpret_cast<DtorFn>(mem);
    g_mem = mem;
    g_fade = d.fade;
    uint8_t jmp[kDtorEntry] = {0xE9};
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(jmp + 1, &r32, 4);
    if (!WriteCode(d.dtor, std::span<const uint8_t>(jmp, kDtorEntry))) {
        g_tramp = nullptr;
        Log("[horsefade] could not patch the reference destructor: crash guard not installed");
        return false;
    }
    g_dtor = d.dtor;
    Log("[horsefade] camera fade exe+0x%llX (faded-horse pointer exe+0x%llX, alpha update exe+0x%llX), reference destructor "
        "exe+0x%llX (TESObjectREFR vtable exe+0x%llX): crash guard installed",
        static_cast<unsigned long long>(d.fade - game.base),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(d.faded) - game.base),
        static_cast<unsigned long long>(d.alphaUpdate - game.base),
        static_cast<unsigned long long>(d.dtor - game.base),
        static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(d.vtable) - game.base));
    return true;
}

void SetHorseFadeGuardEnabled(bool enabled) { g_enabled = enabled; }

void LogHorseFadeGuardStats()
{
    const uint64_t n = g_cleared.load();
    if (!g_tramp || n == g_clearedLogged) return;
    Log("[horsefade] the camera's faded horse was destroyed before its fade-in: pointer cleared %llu time(s) so far",
        static_cast<unsigned long long>(n));
    g_clearedLogged = n;
}

bool HorseFadeGuardState(HorseFadeGuardInfo* out, uint64_t* cleared)
{
    if (!g_dtor) return false;
    out->fade = g_fade;
    out->faded = const_cast<void**>(g_faded);
    out->dtor = g_dtor;
    out->hook = reinterpret_cast<void*>(&DtorHook);
    out->trampoline = g_mem;
    if (cleared) *cleared = g_cleared.load();
    return true;
}

void HorseFadeGuardTestRedirect(void** faded, void* fakeDtor)
{
    if (faded) g_faded = reinterpret_cast<void* volatile*>(faded);
    if (fakeDtor) g_tramp = reinterpret_cast<DtorFn>(fakeDtor);
}

}  // namespace opf
