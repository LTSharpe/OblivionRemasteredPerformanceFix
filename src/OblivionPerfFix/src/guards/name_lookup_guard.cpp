// NameLookupCrashGuard: crash in the old engine's name table (OblivionThread, _stricmp reading freed memory).
//
// The old engine keeps one global name -> form table (a NiTStringMap, 4001 buckets, exe+0x8FAB580 in build
// 19115871) for the names the console and a few game systems look up: cells, worldspaces, quests, dialogue
// topics, globals and sounds (~9,500 entries). Its keys are not copied: each entry points at the form's own
// editor-ID string. Interior cells keep that string in an extra-data object that is created again whenever the
// cell is loaded again (plugin overrides at start-up, then every new or loaded game), and the table is never told:
// it has no remove or update path. Measured live: ~1,550 cell entries point at freed memory at the main menu after
// start-up, ~3,400 of 3,483 after the first new game. A lookup compares the wanted name with each entry of its
// bucket in turn (_stricmp), so about a third of all lookups read freed memory: "Tamriel" (console positionworld)
// passes two dead entries, as do collision sounds such as "CStoneMedium", "CWoodLarge", "CClothMedium" and
// opening a body ("DRSBodyOpen"). Usually that memory still holds other data and the compare just fails, which is
// what the game's lookups return then; once the allocator has given the page back to Windows the read faults and
// the game crashes (crash #7, 3 times on 2026-10-05, each after several new games in one session).
//
// The fix replaces the table's key compare (vtable slot 2, the 26-byte `return _stricmp(a, b) == 0`) with the
// same call through the game's own _stricmp import inside an SEH guard. When reading the stored key faults (and
// only then), the entry does not match, as it does whenever the freed memory is still mapped, and the search goes
// on. Every other compare returns exactly what it returned before. A fault at any other address (for example a
// bad name passed in by the caller) is not handled and crashes as before.
#include "common.h"

#include <atomic>

namespace opf {
namespace {

// Static initialiser of the table (build 19115871: exe+0xCAECD0): allocates 4001 bucket pointers, clears them,
// stores the vtable and bCopy = 0 (keys are not copied), registers the destructor.
//   B9 08 7D 00 00           mov ecx, 7D08h              (4001 * 8)
//   E8 ?? ?? ?? ??           call allocate
//   44 8B 05 [nb]            mov r8d, [map+8]            (bucket count)
//   33 D2 49 C1 E0 03 48 8B C8
//   48 89 05 [buckets]       mov [map+10h], rax
//   E8 ?? ?? ?? ??           call memset
//   48 8D 05 [vtable]        lea rax, [vtable]
//   C6 05 [bcopy] 00         mov byte [map+20h], 0
//   48 8D 0D ?? ?? ?? ??     lea rcx, [destructor]
//   48 89 05 [map]           mov [map], rax
constexpr const char* kInit =
    "B9 08 7D 00 00 E8 ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ?? 33 D2 49 C1 E0 03 48 8B C8 48 89 05 ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 48 8D 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ??";
// Instruction offsets in kInit (each a RIP-relative operand).
constexpr size_t kInitBucketCount = 10, kInitBuckets = 26, kInitVtable = 38, kInitBcopy = 45, kInitMap = 59;

// The key compare (build 19115871: exe+0x660FDE0): sub rsp,28h; mov rcx,rdx; mov rdx,r8; call [_stricmp];
// test eax,eax; sete al; add rsp,28h; ret
constexpr const char* kCompare = "48 83 EC 28 48 8B CA 49 8B D0 FF 15 ?? ?? ?? ?? 85 C0 0F 94 C0 48 83 C4 28 C3";

using CompareFn = bool (*)(void* map, const char* a, const char* b);
using StricmpFn = int(__cdecl*)(const char*, const char*);

void** g_slot = nullptr;           // vtable slot 2 of the table
uint8_t* g_original = nullptr;     // the game's compare
void* const* g_stricmp = nullptr;  // the game's import slot for _stricmp (what the original calls)
uint8_t* g_map = nullptr;
std::atomic<bool> g_enabled{true};
std::atomic<uint64_t> g_faults{0};
std::atomic<uintptr_t> g_lastFaultKey{0};
uint64_t g_faultsLogged = 0;

bool __cdecl GuardedCompare(void* map, const char* a, const char* b);

}  // namespace

// Exception filter of the guarded compare: handle only a read fault inside the stored key (b), i.e. the dead
// entry's string; everything else is left to the game's own handling as before.
int EditorIdGuardFilter(const EXCEPTION_RECORD* rec, const char* key)
{
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    if (rec->ExceptionInformation[0] != 0) return EXCEPTION_CONTINUE_SEARCH;  // not a read
    const uintptr_t at = rec->ExceptionInformation[1], k = reinterpret_cast<uintptr_t>(key);
    // _stricmp reads the key byte by byte (or in aligned words) up to its terminator: the fault lies at or after
    // the key's start, within the longest editor ID plus a page.
    if (at < (k & ~static_cast<uintptr_t>(15)) || at - k >= 0x2000) return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_EXECUTE_HANDLER;
}

namespace {

int Compare(const char* a, const char* b)
{
    __try {
        return reinterpret_cast<StricmpFn>(*g_stricmp)(a, b);
    } __except (EditorIdGuardFilter(GetExceptionInformation()->ExceptionRecord, b)) {
        g_lastFaultKey = reinterpret_cast<uintptr_t>(b);
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return 1;  // no match: the dead entry is skipped, as when its memory is still mapped
    }
}

bool __cdecl GuardedCompare(void* map, const char* a, const char* b)
{
    if (!g_enabled.load(std::memory_order_relaxed)) return reinterpret_cast<CompareFn>(g_original)(map, a, b);
    return Compare(a, b) == 0;
}

}  // namespace

bool ApplyEditorIdLookupGuard()
{
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kInit));
    if (hits.size() != 1) {
        Log("[edid] name table initialiser not found (%zu matches): crash guard not installed", hits.size());
        return false;
    }
    uint8_t* const at = hits[0];
    uint8_t* const map = RipTarget(at + kInitMap, 3, 7);
    auto* const vtable = reinterpret_cast<void**>(RipTarget(at + kInitVtable, 3, 7));
    // The layout the lookups rely on: bucket count +8, bucket array +0x10, bCopy +0x20 (0 = keys not copied).
    if (RipTarget(at + kInitBucketCount, 3, 7) != map + 8 || RipTarget(at + kInitBuckets, 3, 7) != map + 0x10 ||
        RipTarget(at + kInitBcopy, 2, 7) != map + 0x20 || !game.rdata.Contains(vtable) || !game.rdata.Contains(vtable + 2)) {
        Log("[edid] name table layout differs from build 19115871: crash guard not installed");
        return false;
    }
    void** const slot = vtable + 2;
    if (*slot == reinterpret_cast<void*>(&GuardedCompare)) {
        Log("[edid] crash guard already installed");
        return true;
    }
    auto* const original = static_cast<uint8_t*>(*slot);
    if (!game.text.Contains(original) || !MatchesAt(original, ParsePattern(kCompare))) {
        Log("[edid] name table compare at exe+0x%llX is not the expected _stricmp compare: crash guard not installed",
            static_cast<unsigned long long>(original - game.base));
        return false;
    }
    auto* const import = reinterpret_cast<void* const*>(RipTarget(original + 10, 2, 6));
    if (import < reinterpret_cast<void* const*>(game.base) || import >= reinterpret_cast<void* const*>(game.base + game.sizeOfImage)) {
        Log("[edid] _stricmp import slot outside the exe: crash guard not installed");
        return false;
    }
    g_map = map;
    g_original = original;
    g_stricmp = import;
    g_slot = slot;
    if (!WritePointer(slot, reinterpret_cast<void*>(&GuardedCompare))) {
        Log("[edid] could not write the vtable slot: crash guard not installed");
        g_slot = nullptr;
        return false;
    }
    Log("[edid] name table exe+0x%llX (vtable exe+0x%llX, compare exe+0x%llX, _stricmp import exe+0x%llX): crash guard installed",
        static_cast<unsigned long long>(map - game.base), static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(vtable) - game.base),
        static_cast<unsigned long long>(original - game.base),
        static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(import) - game.base));
    return true;
}

void SetEditorIdLookupGuardEnabled(bool enabled) { g_enabled = enabled; }

void LogEditorIdLookupGuardStats()
{
    const uint64_t n = g_faults.load();
    if (!g_slot || n == g_faultsLogged) return;
    Log("[edid] name lookups that reached a freed table entry and were kept from crashing: %llu so far (last key at %p)",
        static_cast<unsigned long long>(n), reinterpret_cast<void*>(g_lastFaultKey.load()));
    g_faultsLogged = n;
}

bool EditorIdLookupGuardInfo(EditorIdGuardInfo* out)
{
    if (!g_slot) return false;
    out->map = g_map;
    out->vtableSlot = g_slot;
    out->original = g_original;
    out->stricmpImport = g_stricmp;
    out->hook = reinterpret_cast<void*>(&GuardedCompare);
    out->faults = g_faults.load();
    return true;
}

void EditorIdLookupGuardTestImport(void* const* fakeImport) { g_stricmp = fakeImport; }

}  // namespace opf
