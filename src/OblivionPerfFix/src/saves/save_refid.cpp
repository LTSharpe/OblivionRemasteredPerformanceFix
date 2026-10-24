// SaveRefIdIndex: the save's formID -> refID lookups through a hash index instead of a linear search (and, in the lab
// build, the old engine's save timer).
//
// When the game saves, the old engine (on the OblivionThread, while the game thread waits for it in the pairing pump)
// writes every form reference of the save as a refID: the index of the formID in the save's formID table
// (TESSaveLoadGame +0xE8, an array: +0x08 data, +0x14 count). The lookup is a linear search over that table, and a
// formID not found yet is appended (exe+0x671CB30) (build 19115871: TESSaveLoadGame::SaveFormIDs exe+0x671C270 inline,
// GetRefID exe+0x6703640; two more inline copies are left alone). Every lookup scans the table, so a save costs
// O(lookups x table size): small in a new game, quadratic in a long one.
//
// The fix (SaveRefIdIndex=1): inside TESSaveLoadGame::SaveGame (exe+0x67198B0, entry detour through a trampoline, which
// also times the old engine's save) the two lookups call a helper that keeps a hash index of the table: first
// occurrence of each formID -> its position, indexed incrementally up to the table's current count at every call, so
// appends made elsewhere are seen too; reset at every SaveGame and whenever the table moves, shrinks or is another one.
// Found: the same position the linear search returns (the first equal entry; checked against the table on each hit).
// Not found: the game's own append, as before. Outside SaveGame, or with the key 0, the helper does the game's linear
// search. The patched pieces: the search-and-append block of each lookup (46 bytes each) becomes a call of the helper
// with (TESSaveLoadGame, formID) and a jump to the code that uses the result. Installed only while the game starts and
// only when the key is on; then live (0 = the linear search).
#include "common.h"

#include <atomic>
#include <cstring>
#include <vector>

namespace opf {
namespace {

// The quicksave thunk: xor r9d,r9d; lea r8,["quicksave"]; xor edx,edx; jmp SaveGame
constexpr char kQuickSaveThunk[] = "45 33 C9 4C 8D 05 ?? ?? ?? ?? 33 D2 E9";
constexpr size_t kThunkName = 3, kThunkJmp = 12;
// SaveGame's prologue: the first 16 bytes are moved to the trampoline (no RIP-relative operands).
constexpr char kSaveGameEntry[] = "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00";
constexpr size_t kSaveGameMoved = 16;
// SaveFormIDs' loop: ... mov r9,[rdi+0E8h] (+0x22) ... search ... call append; mov ebx,eax (+0x50).
constexpr char kSaveFormIds[] =
    "80 BF F9 00 00 00 00 41 8B 1E 74 46 48 8B 0D ?? ?? ?? ?? 8B D3 89 5C 24 50 E8 ?? ?? ?? ?? 84 C0 75 30 4C 8B 8F E8 00 00 00 "
    "33 C0 41 8B 51 14 85 D2 74 10 4D 8B 41 08 41 39 1C 80 74 13 FF C0 3B C2 72 F4 4C 8D 44 24 50 49 8B C9 E8 ?? ?? ?? ?? 8B D8";
constexpr size_t kSfiBlock = 0x22, kSfiAppend = 0x4B, kSfiResume = 0x50;
// GetRefID: ... mov r9,[rdi+0E8h] (+0x30) ... search ... call append (+0x59); epilogue (+0x5E).
constexpr char kGetRefId[] =
    "48 89 5C 24 08 89 54 24 10 57 48 83 EC 20 48 8B F9 8B DA 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 0D 8B C3 48 8B 5C 24 "
    "30 48 83 C4 20 5F C3 4C 8B 8F E8 00 00 00 33 C9 41 8B 51 14 85 D2 74 10 4D 8B 41 08 41 39 1C 88 74 1E FF C1 3B CA 72 F4 "
    "4C 8D 44 24 38 49 8B C9 E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 83 C4 20 5F C3 48 8B 5C 24 30 8B C1";
constexpr size_t kGriBlock = 0x30, kGriAppend = 0x59, kGriResume = 0x5E;
constexpr size_t kBlockLen = 0x2E;
constexpr size_t kTableOff = 0xE8, kTableData = 0x08, kTableCount = 0x14;

using AppendFn = uint64_t (*)(void* table, uint32_t at, const uint32_t* id);
using SaveGameFn = void* (*)(void* self, void* a, void* b, void* c);

AppendFn g_append = nullptr;
SaveGameFn g_saveGameTramp = nullptr;
bool g_installed = false;
std::atomic<int> g_mode{0};
std::atomic<bool> g_timers{false};
thread_local int t_inSave = 0;

// FindAll with the search started at a rarer byte of the pattern (anchor): memchr stops less often (a pattern that starts
// with 48 stops ~9 million times in this exe); every hit is checked against the whole pattern.
std::vector<uint8_t*> FindAnchored(const Section& text, const char* pattern, size_t anchor)
{
    const Pattern full = ParsePattern(pattern);
    Pattern tail;
    tail.bytes.assign(full.bytes.begin() + static_cast<std::ptrdiff_t>(anchor), full.bytes.end());
    tail.care.assign(full.care.begin() + static_cast<std::ptrdiff_t>(anchor), full.care.end());
    std::vector<uint8_t*> out;
    for (uint8_t* h : FindAll(text, tail))
        if (h - anchor >= text.begin && MatchesAt(h - anchor, full)) out.push_back(h - anchor);
    return out;
}

// The index: open addressing, keys are formIDs (never 0xFFFFFFFF here: ids above 0xFEFFFFFF skip the lookup).
struct Index {
    const void* table = nullptr;
    const uint32_t* data = nullptr;
    uint32_t indexed = 0;
    std::vector<uint64_t> slots;  // (key << 32) | position, empty = ~0
    uint32_t used = 0;
    void Reset()
    {
        table = nullptr;
        data = nullptr;
        indexed = 0;
        used = 0;
        if (!slots.empty()) std::fill(slots.begin(), slots.end(), ~0ull);
    }
    static uint32_t Hash(uint32_t k) { k ^= k >> 16; k *= 0x7FEB352Du; k ^= k >> 15; k *= 0x846CA68Bu; k ^= k >> 16; return k; }
    void Grow()
    {
        std::vector<uint64_t> old;
        old.swap(slots);
        slots.assign(old.empty() ? 4096 : old.size() * 2, ~0ull);
        used = 0;
        for (uint64_t s : old)
            if (s != ~0ull) Insert(static_cast<uint32_t>(s >> 32), static_cast<uint32_t>(s));
    }
    void Insert(uint32_t key, uint32_t pos)  // keeps the first position of a key
    {
        if ((used + 1) * 2 > slots.size()) Grow();
        const size_t mask = slots.size() - 1;
        for (size_t i = Hash(key) & mask;; i = (i + 1) & mask) {
            if (slots[i] == ~0ull) { slots[i] = (static_cast<uint64_t>(key) << 32) | pos; ++used; return; }
            if (static_cast<uint32_t>(slots[i] >> 32) == key) return;
        }
    }
    bool Find(uint32_t key, uint32_t* pos) const
    {
        if (slots.empty()) return false;
        const size_t mask = slots.size() - 1;
        for (size_t i = Hash(key) & mask;; i = (i + 1) & mask) {
            if (slots[i] == ~0ull) return false;
            if (static_cast<uint32_t>(slots[i] >> 32) == key) { *pos = static_cast<uint32_t>(slots[i]); return true; }
        }
    }
};
Index g_index;  // the OblivionThread's (one save at a time; other threads use the linear search)
std::atomic<uint32_t> g_indexThread{0};

struct SaveStats { uint64_t lookups = 0, hits = 0, appends = 0, scanned = 0, resets = 0, mismatches = 0; };
SaveStats t_stats;  // of the current SaveGame (OblivionThread)
std::atomic<uint64_t> s_saves{0}, s_saveTicks{0}, s_lookups{0}, s_scanned{0}, s_mismatch{0};

uint64_t Ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

double Ms(uint64_t ticks)
{
    static const double perMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1000.0; }();
    return static_cast<double>(ticks) / perMs;
}

// The game's linear search (first equal entry), or count when absent.
uint32_t Linear(const uint8_t* table, uint32_t id, uint64_t* scanned)
{
    const uint32_t count = *reinterpret_cast<const uint32_t*>(table + kTableCount);
    const uint32_t* const data = *reinterpret_cast<const uint32_t* const*>(table + kTableData);
    uint32_t i = 0;
    while (i < count && data[i] != id) ++i;
    if (scanned) *scanned += i < count ? i + 1 : count;
    return i;
}

// Called from the two patched blocks: TESSaveLoadGame (rcx), formID (edx) -> refID (eax), exactly as the block did.
uint64_t RefIdHelper(uint8_t* saveLoad, uint32_t id)
{
    uint8_t* const table = *reinterpret_cast<uint8_t**>(saveLoad + kTableOff);
    uint32_t local = id;
    const int mode = g_mode.load(std::memory_order_relaxed);
    const bool timers = g_timers.load(std::memory_order_relaxed);
    if (t_inSave <= 0 || mode == 0 || GetCurrentThreadId() != g_indexThread.load(std::memory_order_relaxed)) {
        uint64_t scanned = 0;
        const uint32_t i = Linear(table, id, timers ? &scanned : nullptr);
        if (t_inSave > 0) { ++t_stats.lookups; t_stats.scanned += scanned; }
        if (i < *reinterpret_cast<const uint32_t*>(table + kTableCount)) return i;
        if (t_inSave > 0) ++t_stats.appends;
        return g_append(table, *reinterpret_cast<const uint32_t*>(table + kTableCount), &local);
    }
    ++t_stats.lookups;
    const uint32_t count = *reinterpret_cast<const uint32_t*>(table + kTableCount);
    const uint32_t* const data = *reinterpret_cast<const uint32_t* const*>(table + kTableData);
    Index& x = g_index;
    if (x.table != table || x.data != data || x.indexed > count) {
        if (x.table) ++t_stats.resets;
        x.Reset();
        x.table = table;
        x.data = data;
    }
    for (; x.indexed < count; ++x.indexed) x.Insert(data[x.indexed], x.indexed);
    uint32_t pos = count;
    bool found = x.Find(id, &pos) && pos < count && data[pos] == id;
    if (!found && x.Find(id, &pos)) {
        // the table changed in place under the index (not expected): rebuild, then look again
        ++t_stats.resets;
        x.Reset();
        x.table = table;
        x.data = data;
        for (; x.indexed < count; ++x.indexed) x.Insert(data[x.indexed], x.indexed);
        found = x.Find(id, &pos);
    }
    if (found) { ++t_stats.hits; return pos; }
    ++t_stats.appends;
    return g_append(table, count, &local);
}

void* SaveGameHook(void* self, void* a, void* b, void* c)
{
    const bool outer = t_inSave == 0;
    if (outer) {
        t_stats = {};
        g_indexThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        g_index.Reset();
    }
    ++t_inSave;
    const uint64_t t0 = Ticks();
    void* const r = g_saveGameTramp(self, a, b, c);
    const uint64_t dt = Ticks() - t0;
    --t_inSave;
    if (outer) {
        s_saves.fetch_add(1, std::memory_order_relaxed);
        s_saveTicks.fetch_add(dt, std::memory_order_relaxed);
        s_lookups.fetch_add(t_stats.lookups, std::memory_order_relaxed);
        s_scanned.fetch_add(t_stats.scanned, std::memory_order_relaxed);
        g_index.Reset();  // nothing kept between saves
    }
    return r;
}

bool Fail(const char* why)
{
    Log("[saverefid] %s: SaveRefIdIndex unavailable", why);
    return false;
}

// mov rcx,rdi; mov edx,ebx; mov rax,imm64; call rax; jmp resume (rel8)
bool PatchBlock(uint8_t* block, uint8_t* resume)
{
    uint8_t code[kBlockLen];
    memset(code, 0xCC, sizeof code);
    size_t n = 0;
    const uint8_t head[] = {0x48, 0x8B, 0xCF, 0x8B, 0xD3, 0x48, 0xB8};
    memcpy(code, head, sizeof head);
    n = sizeof head;
    const void* const helper = reinterpret_cast<const void*>(&RefIdHelper);
    memcpy(code + n, &helper, 8);
    n += 8;
    code[n++] = 0xFF;
    code[n++] = 0xD0;
    const int64_t rel = resume - (block + n + 2);
    if (rel < -128 || rel > 127) return false;
    code[n++] = 0xEB;
    code[n++] = static_cast<uint8_t>(static_cast<int8_t>(rel));
    return WriteCode(block, std::span<const uint8_t>(code, kBlockLen));
}

}  // namespace

bool InstallSaveRefIdIndex(bool beforeGameCode)
{
    if (g_installed) return true;
    const uint64_t install0 = Ticks();
    const GameModule& game = Game();
    const auto sfi = FindAnchored(game.text, kSaveFormIds, 9);
    const auto gri = FindAnchored(game.text, kGetRefId, 16);
    uint8_t* saveGame = nullptr;
    for (uint8_t* t : FindAnchored(game.text, kQuickSaveThunk, 2)) {
        const char* const name = reinterpret_cast<const char*>(RipTarget(t + kThunkName, 3, 7));
        if (reinterpret_cast<const uint8_t*>(name) >= game.rdata.begin && reinterpret_cast<const uint8_t*>(name) + 10 <= game.rdata.end &&
            memcmp(name, "quicksave", 10) == 0) {
            saveGame = RipTarget(t + kThunkJmp, 1, 5);
            break;
        }
    }
    if (sfi.size() != 1 || gri.size() != 1 || !saveGame) {
        Log("[saverefid] SaveFormIDs found %zu times, GetRefID %zu, SaveGame %s: unavailable", sfi.size(), gri.size(), saveGame ? "found" : "not found");
        return false;
    }
    if (!beforeGameCode) return Fail("only installed while the game starts");
    if (!game.text.Contains(saveGame) || !MatchesAt(saveGame, ParsePattern(kSaveGameEntry))) return Fail("SaveGame's entry is not as expected");
    uint8_t* const append = RipTarget(sfi[0] + kSfiAppend, 1, 5);
    if (append != RipTarget(gri[0] + kGriAppend, 1, 5) || !game.text.Contains(append)) return Fail("the two lookups append differently");
    // Trampoline: SaveGame's first 16 bytes, then jmp [rip] back.
    uint8_t* const tramp = AllocNearGame(64);
    if (!tramp) return Fail("no free memory near the game's code");
    memcpy(tramp, saveGame, kSaveGameMoved);
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const back = saveGame + kSaveGameMoved;
    memcpy(tramp + kSaveGameMoved, jmpAbs, 6);
    memcpy(tramp + kSaveGameMoved + 6, &back, 8);
    DWORD old;
    VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    g_append = reinterpret_cast<AppendFn>(append);
    g_saveGameTramp = reinterpret_cast<SaveGameFn>(tramp);
    if (!PatchBlock(sfi[0] + kSfiBlock, sfi[0] + kSfiResume) || !PatchBlock(gri[0] + kGriBlock, gri[0] + kGriResume))
        return Fail("a lookup block could not be patched");
    uint8_t entry[kSaveGameMoved];
    memset(entry, 0x90, sizeof entry);
    memcpy(entry, jmpAbs, 6);
    void* const hook = reinterpret_cast<void*>(&SaveGameHook);
    memcpy(entry + 6, &hook, 8);
    if (!WriteCode(saveGame, std::span<const uint8_t>(entry, sizeof entry))) return Fail("SaveGame's entry could not be written");
    g_installed = true;
    Log("[saverefid] SaveGame exe+0x%X (timed), SaveFormIDs lookup exe+0x%X, GetRefID lookup exe+0x%X, append exe+0x%X: installed (%.1f ms)",
        static_cast<unsigned>(saveGame - game.base), static_cast<unsigned>(sfi[0] + kSfiBlock - game.base),
        static_cast<unsigned>(gri[0] + kGriBlock - game.base), static_cast<unsigned>(append - game.base), Ms(Ticks() - install0));
    return true;
}

void SetSaveRefIdIndex(int mode, bool timers)
{
    g_timers = timers;
    if (!g_installed) return;
    mode = mode != 0;
    timers = false;
    g_timers = false;
    if (g_mode.exchange(mode) != mode)
        Log("[saverefid] %s", mode == 2 ? "SaveRefIdIndex 2: the save's refID lookups through the index, each compared with the linear search"
                              : mode ? "SaveRefIdIndex on: the save's refID lookups through a hash index"
                                     : "SaveRefIdIndex off: the save's refID lookups search linearly, as the game does");
}

// Self-test: the helper with a fake append, inside or outside a "SaveGame".
void SaveRefIdTestSetup(void* append, int mode)
{
    g_append = reinterpret_cast<AppendFn>(append);
    g_mode = mode;
}

uint64_t SaveRefIdTestCall(void* saveLoad, uint32_t id, bool inSave)
{
    if (inSave && t_inSave == 0) {
        g_indexThread.store(GetCurrentThreadId());
    }
    t_inSave += inSave ? 1 : 0;
    const uint64_t r = RefIdHelper(static_cast<uint8_t*>(saveLoad), id);
    t_inSave -= inSave ? 1 : 0;
    return r;
}

void SaveRefIdTestNewSave() { g_index.Reset(); }

void LogSaveRefIdStats()
{
    const uint64_t n = s_saves.exchange(0), t = s_saveTicks.exchange(0), l = s_lookups.exchange(0), sc = s_scanned.exchange(0),
                   m = s_mismatch.exchange(0);
    if (n)
        Log("[saverefid] since last report: %llu old engine saves (%.1f ms), %llu refID lookups, %llu linear entries scanned, %llu mismatches",
            static_cast<unsigned long long>(n), Ms(t), static_cast<unsigned long long>(l), static_cast<unsigned long long>(sc),
            static_cast<unsigned long long>(m));
}

}  // namespace opf
