// HavokPickWarningGuard: the old engine's "Havok picks" warning box freezes the game in big fights (game bug).
//
// The old engine counts its Havok "picks" (ray and shape casts for path finding, line of sight and the view caster)
// per frame, in the pick function (build 19115871: exe+0x65E1B60) and caps them at the setting iMaxPicks:HAVOK
// (40; any pick past the cap is dropped with "no hit"). At the end of its physics update (exe+0x65E1030, on the
// OblivionThread) it looks at the count: past the cap it formats "%d Havok picks (%d path, %d LOS, %d ViewCaster)
// used this frame." and calls the engine's warning handler through a pointer in .data (exe+0x8FB7CC8 ->
// exe+0x691B460), which is MessageBoxA(NULL, "WARNING", text, MB_TOPMOST): a modal box on the engine's own thread.
// The old engine stops until someone closes it, the game thread waits for the old engine at its next frame, and the
// whole game stands still, often behind its own full-screen window. A debug warning left in the shipping game; it
// shows in fights with many characters (our battle benchmark lost 2 of 10 runs to it). The cap itself is gameplay
// (it decides which casts the engine skips) and stays as it is.
//
// The fix replaces the handler pointer with a wrapper. A call that comes from the pick warning's call site (the one
// place that formats this text, found by pattern and checked: the counters, the cap, the format strings, "WARNING",
// the handler's own code) is counted and logged instead of shown; any other caller of the handler gets the game's
// function as before. Nothing else changes: the counters are reset by the game's code right after the call, as
// they are after the box, and the cap is applied where it always was.
#include "common.h"

#include <atomic>
#include <cstring>
#include <intrin.h>
#include <windows.h>

namespace opf {
namespace {

// The end of the old engine's physics update, build 19115871 exe+0x65E105F (inside exe+0x65E1030):
//   40 38 35 [flag]              cmp byte ptr [counting off],sil      sil = 0
//   0F 85 ????????               jnz skip
//   44 8B 05 [picks]             mov r8d,[picks this frame]
//   44 3B 05 [max]               cmp r8d,[iMaxPicks]
//   76 ??                        jbe reset
//   8B 05 [other] 48 8D 4C 24 ?? 44 8B 0D [path] 85 C0 74 ??
//   89 44 24 ?? 48 8D 15 [fmtOther] 8B 05 ???????? 89 44 24 ?? 8B 05 ???????? 89 44 24 ?? E8 [sprintf] EB ??
//   8B 05 ???????? 48 8D 15 [fmt] 89 44 24 ?? 8B 05 ???????? 89 44 24 ?? E8 [sprintf]
//   48 8B 05 [handler]           mov rax,[warning handler]
//   48 85 C0 74 ??               test rax,rax; jz reset
//   45 33 C0                     xor r8d,r8d
//   48 8D 54 24 ??               lea rdx,[the text]
//   48 8D 0D [warning]           lea rcx,["WARNING"]
//   FF D0                        call rax                             <- the only call site held back
//   89 2D [picks]                mov [picks this frame],ebp           ebp = 0 (the reset, as before)
constexpr const char* kWarn =
    "40 38 35 ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ?? 44 3B 05 ?? ?? ?? ?? 76 ?? "
    "8B 05 ?? ?? ?? ?? 48 8D 4C 24 ?? 44 8B 0D ?? ?? ?? ?? 85 C0 74 ?? "
    "89 44 24 ?? 48 8D 15 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 89 44 24 ?? 8B 05 ?? ?? ?? ?? 89 44 24 ?? E8 ?? ?? ?? ?? EB ?? "
    "8B 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 89 44 24 ?? 8B 05 ?? ?? ?? ?? 89 44 24 ?? E8 ?? ?? ?? ?? "
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 45 33 C0 48 8D 54 24 ?? 48 8D 0D ?? ?? ?? ?? FF D0 89 2D";
constexpr size_t kWarnFlag = 0, kWarnPicks = 13, kWarnMax = 20, kWarnFmtOther = 55, kWarnSprintf1 = 82,
                 kWarnFmt = 95, kWarnSprintf2 = 116, kWarnHandler = 121, kWarnCaption = 141, kWarnCall = 148,
                 kWarnReset = 150;

// The engine's warning handler (build 19115871 exe+0x691B460): MessageBoxA(NULL, rcx, rdx, MB_TOPMOST) (rcx the box's text,
// rdx its caption), returns 0.
//   48 83 EC 28 4C 8B C2 41 B9 00 00 04 00 48 8B D1 33 C9 FF 15 [MessageBoxA] 33 C0 48 83 C4 28 C3
constexpr const char* kHandler = "48 83 EC 28 4C 8B C2 41 B9 00 00 04 00 48 8B D1 33 C9 FF 15 ?? ?? ?? ?? 33 C0 48 83 C4 28 C3";
constexpr size_t kHandlerImport = 18;

using HandlerFn = uint64_t (*)(const char* boxText, const char* boxCaption, uint64_t a3);

constexpr size_t kTextKeep = 160;

void** g_slot = nullptr;              // the handler pointer in .data (now the wrapper)
HandlerFn g_original = nullptr;       // the game's handler
const uint8_t* g_return = nullptr;    // the pick warning's return address (after `call rax`)
const uint32_t* g_picks = nullptr;    // the per-frame pick counter
const uint32_t* g_max = nullptr;      // iMaxPicks
const void* g_testReturn = nullptr;   // self-test: stands in for _ReturnAddress()
std::atomic<bool> g_enabled{true};
std::atomic<uint64_t> g_heldBack{0}, g_forwarded{0};
uint64_t g_heldBackLogged = 0;
int g_textLogs = 0;
char g_lastText[kTextKeep];           // written by the OblivionThread (Keep), read by the monitor thread for the stats line:
                                      // an unsynchronised read can at worst print a mix of two texts; nothing acts on it

void Keep(const char* text)
{
    if (!text) text = "";
    strncpy_s(g_lastText, text, _TRUNCATE);
}

// Same signature as the game's handler (MessageBoxA's order): rcx = the box's text (the call site passes "WARNING"),
// rdx = the box's caption (the call site passes the formatted warning), r8 = a third value the handler ignores.
uint64_t Hook(const char* boxText, const char* boxCaption, uint64_t a3)
{
    const void* ret = g_testReturn ? g_testReturn : _ReturnAddress();
    if (ret == g_return && g_enabled.load(std::memory_order_relaxed)) {
        g_heldBack.fetch_add(1, std::memory_order_relaxed);
        Keep(boxCaption);
        if (g_textLogs < 3) {
            ++g_textLogs;
            Log("[picks] the old engine's warning box held back (the game would have stopped until it was closed): \"%s\" "
                "(iMaxPicks %u: the game's pick cap still applies)", g_lastText, g_max ? *g_max : 0u);
        }
        return 0;  // what the game's handler returns
    }
    g_forwarded.fetch_add(1, std::memory_order_relaxed);
    return g_original(boxText, boxCaption, a3);
}

// True if `slot` is the exe's import-table entry for user32's MessageBoxA: the import directory names it (that holds for a
// mapped copy too, where the imports are not resolved), and the value is either still the file's (the RVA of the name
// entry: the self-test's mapped copy, or a slot the loader has not filled in yet) or the function user32 exports.
bool ImportIsMessageBoxA(const GameModule& game, const uint8_t* slot)
{
    if (slot < game.base || slot + sizeof(void*) > game.base + game.sizeOfImage) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(game.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.VirtualAddress + dir.Size > game.sizeOfImage) return false;
    bool named = false;
    uint64_t nameRva = 0;
    for (const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(game.base + dir.VirtualAddress);
         desc->Name && !named; ++desc) {
        if (desc->Name >= game.sizeOfImage || _stricmp(reinterpret_cast<const char*>(game.base + desc->Name), "USER32.dll") != 0) continue;
        const DWORD namesRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        if (namesRva >= game.sizeOfImage || desc->FirstThunk >= game.sizeOfImage) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(game.base + namesRva);
        const auto* entries = reinterpret_cast<const IMAGE_THUNK_DATA64*>(game.base + desc->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal) || names[i].u1.AddressOfData >= game.sizeOfImage) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(game.base + names[i].u1.AddressOfData);
            if (strcmp(byName->Name, "MessageBoxA") == 0) {
                named = reinterpret_cast<const uint8_t*>(entries + i) == slot;
                nameRva = names[i].u1.AddressOfData;
                break;
            }
        }
    }
    if (!named) return false;
    // The slot's value: unresolved (the file's value, the name entry's RVA: a mapped copy, whose imports the loader does not
    // resolve, holds that whether or not this process has user32 loaded) or the function user32 exports.
    const uint64_t value = *reinterpret_cast<const uint64_t*>(slot);
    if (value == nameRva) return true;
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 && value == reinterpret_cast<uint64_t>(GetProcAddress(user32, "MessageBoxA"));
}

bool StartsWith(const GameModule& game, const uint8_t* s, const char* prefix)
{
    const size_t n = strlen(prefix);
    return game.rdata.Contains(s) && game.rdata.Contains(s + n) && memcmp(s, prefix, n) == 0;
}

}  // namespace

bool ApplyHavokPickWarningGuard()
{
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kWarn));
    if (hits.size() != 1) {
        Log("[picks] the pick warning's code not found (%zu matches): guard not installed", hits.size());
        return false;
    }
    uint8_t* const at = hits[0];
    const uint8_t* flag = RipTarget(at + kWarnFlag, 3, 7);
    const uint8_t* picks = RipTarget(at + kWarnPicks, 3, 7);
    const uint8_t* max = RipTarget(at + kWarnMax, 3, 7);
    const uint8_t* reset = RipTarget(at + kWarnReset, 2, 6);
    const uint8_t* fmtOther = RipTarget(at + kWarnFmtOther, 3, 7);
    const uint8_t* fmt = RipTarget(at + kWarnFmt, 3, 7);
    const uint8_t* sprintf1 = RipTarget(at + kWarnSprintf1, 1, 5);
    const uint8_t* sprintf2 = RipTarget(at + kWarnSprintf2, 1, 5);
    auto** slot = reinterpret_cast<void**>(RipTarget(at + kWarnHandler, 3, 7));
    const uint8_t* caption = RipTarget(at + kWarnCaption, 3, 7);
    const bool dataOk = game.data.Contains(flag) && game.data.Contains(picks) && game.data.Contains(max) && picks == reset &&
                        game.data.Contains(slot) && game.data.Contains(reinterpret_cast<uint8_t*>(slot + 1) - 1);
    const bool textOk = StartsWith(game, fmt, "%d Havok picks (") && StartsWith(game, fmtOther, "%d Havok picks (") &&
                        StartsWith(game, caption, "WARNING") && caption[7] == 0;
    const bool callsOk = sprintf1 == sprintf2 && game.text.Contains(sprintf1);
    if (!dataOk || !textOk || !callsOk) {
        Log("[picks] the pick warning's code differs (data %d, strings %d, calls %d): guard not installed", dataOk, textOk, callsOk);
        return false;
    }
    void* const current = *slot;
    if (current == reinterpret_cast<void*>(&Hook)) {
        Log("[picks] guard already installed");
        return true;
    }
    const auto* handler = static_cast<const uint8_t*>(current);
    if (!game.text.Contains(handler) || !MatchesAt(handler, ParsePattern(kHandler))) {
        Log("[picks] the warning handler at %p is not the message box the fix expects: guard not installed", current);
        return false;
    }
    const uint8_t* import = RipTarget(handler + kHandlerImport, 2, 6);
    if (!ImportIsMessageBoxA(game, import)) {
        Log("[picks] the warning handler's import is not user32's MessageBoxA: guard not installed");
        return false;
    }
    g_original = reinterpret_cast<HandlerFn>(current);
    g_return = at + kWarnCall + 2;
    g_picks = reinterpret_cast<const uint32_t*>(picks);
    g_max = reinterpret_cast<const uint32_t*>(max);
    if (!WritePointer(slot, reinterpret_cast<void*>(&Hook))) {
        Log("[picks] could not write the handler pointer: guard not installed");
        g_original = nullptr;
        g_return = nullptr;
        return false;
    }
    g_slot = slot;
    Log("[picks] pick warning at exe+0x%llX (handler exe+0x%llX through exe+0x%llX, iMaxPicks %u): the warning box is held back, "
        "the pick cap stays: guard installed",
        static_cast<unsigned long long>(g_return - game.base), static_cast<unsigned long long>(handler - game.base),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(slot) - game.base), *g_max);
    return true;
}

void SetHavokPickWarningGuardEnabled(bool enabled) { g_enabled = enabled; }

void LogHavokPickWarningGuardStats()
{
    const uint64_t n = g_heldBack.load();
    if (!g_slot || n == g_heldBackLogged) return;
    Log("[picks] warning boxes held back: %llu so far (last: \"%s\")", static_cast<unsigned long long>(n), g_lastText);
    g_heldBackLogged = n;
}

bool HavokPickWarningGuardInfo(HavokPickGuardInfo* out)
{
    if (!g_slot) return false;
    out->slot = g_slot;
    out->original = reinterpret_cast<void*>(g_original);
    out->hook = reinterpret_cast<void*>(&Hook);
    out->callReturn = g_return;
    out->picks = g_picks;
    out->max = g_max;
    out->heldBack = g_heldBack.load();
    out->forwarded = g_forwarded.load();
    return true;
}

void HavokPickWarningGuardTestFunctions(void* fakeHandler, const void* returnAddress)
{
    if (fakeHandler) g_original = reinterpret_cast<HandlerFn>(fakeHandler);
    g_testReturn = returnAddress;
}

}  // namespace opf
