// StringTableMemo (worker ftextmemo, 2026-11-25; S-051 menu opens, S-050 dialogue, S-071 Settings pages): the string
// table asset path of a table id, remembered instead of asked from the package store on every text.
//
// Every text a menu shows from a string table (item names, labels, the Settings pages' rows) is made by
// FText::FromStringTable (exe+0xE630F0, called by Altar's text helper exe+0x4883D70 > 0x4883EA0, which tries the game's
// string tables one after the other for a key). UE 5.3 builds a new FTextHistory_StringTableEntry per call, and its
// FStringTableReferenceData::Initialize asks the engine bridge twice for the table's asset (RedirectTableIdAndKey ->
// RedirectStringTableAssetImpl exe+0x32A8F30, then ConditionalBeginAssetLoad -> LoadStringTableAssetImpl exe+0x32A2100).
// Both start with FStringTableEngineBridge::GetAssetReference(FName TableId) (exe+0x329AEC0, StringTable.cpp, a static
// function): TableId to a string, cut at the first '.', FPackageName::IsValidLongPackageName, FPackageName::
// DoesPackageExist (a package store look-up with path conversions), then FSoftObjectPath::SetPath. Sampled in the
// user's play (play114 gtwd, 29 of 32 samples under 0xE630F0) this look-up is ~90 % of the text cost of a menu edge:
// inventory ~7 ms, pause ~9, map ~21, magic ~3, a Settings tab 5-10 ms (menupair / menuedge), every open.
//
// The fix: an entry detour of GetAssetReference. A table id whose answer was a valid asset path with no sub-path
// (PackageName != None, SubPathString empty) is remembered (FName -> the two FNames); the next call for that id writes the
// same 32 bytes and returns. Everything after it (ResolveObject, the load or the callback, the registry's FindStringTable,
// FindEntry, the display string, the new text history) runs as in the game, so every FText is built exactly as before:
// same history type, table id, key, loading phase and display string, a new history per call as stock (FText identity is
// unchanged, so no IdenticalTo or EqualTo caller can see a difference). Culture changes and string table reloads do not
// touch the answer (it is the asset's path and whether its package exists, not the table's content). Only "exists"
// answers are kept: a package that exists stays existing while the game runs (the game mounts its paks at start-up and
// never unmounts them); "not an asset" answers are always asked again (a later mount could change them). Insert-only
// table of 1,024 slots (3/4 used at most), lock-free reads, inserts under a lock: safe from any thread (the async loading
// thread also creates string table texts).
// Since UE 5.3 Epic has not changed GetAssetReference (5.4.4, 5.5.4, 5.6.1, 5.7.4 and ue5-main read 2026-11-25).
//
// Ini [Fixes] StringTableMemo: 0 = off (not installed), 1 = on; lab 2 = verify (every remembered answer is also asked
// the game's way and compared; the game's answer is used; mismatches counted). Installed at start-up, then live.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// FStringTableEngineBridge::GetAssetReference (build 19115871: exe+0x329AEC0), from its entry through the two package-name
// checks: FName::ToString, the '.' search, LeftInline, the result's four zeroed qwords (FSoftObjectPath: PackageName,
// AssetName, SubPathString data, num/max), IsValidLongPackageName(.., true), DoesPackageExist(.., nullptr, true).
constexpr char kPattern[] =
    "48 89 5C 24 08 48 89 74 24 18 48 89 54 24 10 55 57 41 56 48 8B EC 48 83 EC 50 48 8B D9 48 8D 55 E0 48 8D 4D 28 "
    "E8 ?? ?? ?? ?? 48 8B 7D E8 45 33 F6 48 8B 75 E0 45 8B C6 4C 89 75 D0 89 7D D8 85 FF 75 06 44 89 75 DC EB 24 8B D7 "
    "48 8D 4D D0 E8 ?? ?? ?? ?? 48 8B 4D D0 48 8B D6 4C 63 C7 4D 03 C0 E8 ?? ?? ?? ?? 4C 8B 45 D0 8B 7D D8 48 63 CF 49 "
    "8B C0 49 8D 14 48 4C 3B C2 74 52 0F 1F 40 00 66 83 38 2E 74 0B 48 83 C0 02 48 3B C2 75 F1 EB 3D 49 2B C0 48 D1 F8 "
    "83 F8 FF 74 32 44 8D 47 FF 85 FF 75 03 45 8B C6 41 3B C0 41 8B C8 41 8B D6 0F 4C C8 85 C9 0F 4F D1 45 33 C9 44 2B "
    "C2 48 8D 4D D0 E8 ?? ?? ?? ?? 4C 8B 45 D0 8B 7D D8 4C 89 33 8D 47 FF 4C 89 73 08 4C 89 73 10 4C 89 73 18 4C 89 45 "
    "F0 85 FF 75 03 41 8B C6 89 45 F8 48 8D 4D F0 0F 28 45 F0 45 33 C0 B2 01 66 0F 7F 45 F0 E8 ?? ?? ?? ?? 84 C0 74 42 "
    "41 B0 01 48 8D 4D D0 33 D2 E8 ?? ?? ?? ?? 84 C0 74 30";
// The first instruction (mov [rsp+8],rbx), copied into the trampoline: exactly 5 bytes, no rip-relative operand.
constexpr uint8_t kPrologue[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};

// rcx = the FSoftObjectPath to construct (32 bytes), rdx = the FName by value; returns rcx.
using GetRefFn = void* (*)(void* out, uint64_t tableId);

constexpr uint32_t kSlots = 1024;            // power of two
constexpr uint32_t kMaxUsed = kSlots / 4 * 3;
struct Slot {
    std::atomic<uint64_t> key;  // the FName (never None: None is never an asset); 0 = empty; published last (release)
    uint64_t package, asset;    // the answer's PackageName and AssetName
};
Slot g_slots[kSlots];
SRWLOCK g_insertLock = SRWLOCK_INIT;
uint32_t g_used = 0;  // under g_insertLock

GetRefFn g_original = nullptr;  // the trampoline (or a fake in the self-test)
uint8_t* g_fn = nullptr;
bool g_installed = false, g_searched = false;
std::atomic<int> g_mode{0};

std::atomic<uint64_t> g_hits{0}, g_stock{0}, g_stockTicks{0}, g_posStock{0}, g_posTicks{0}, g_notKept{0}, g_full{0};
std::atomic<uint64_t> g_verified{0}, g_mismatch{0};
uint64_t g_logged = 0;

uint32_t Home(uint64_t key) { return static_cast<uint32_t>((key * 0x9E3779B97F4A7C15ull) >> 54) & (kSlots - 1); }

bool Lookup(uint64_t key, uint64_t* package, uint64_t* asset)
{
    for (uint32_t i = 0, s = Home(key); i < kSlots; ++i, s = (s + 1) & (kSlots - 1)) {
        const uint64_t k = g_slots[s].key.load(std::memory_order_acquire);
        if (k == key) {
            *package = g_slots[s].package;
            *asset = g_slots[s].asset;
            return true;
        }
        if (k == 0) return false;
    }
    return false;
}

void Insert(uint64_t key, uint64_t package, uint64_t asset)
{
    AcquireSRWLockExclusive(&g_insertLock);
    uint32_t s = Home(key);
    bool present = false;
    for (uint32_t i = 0; i < kSlots; ++i, s = (s + 1) & (kSlots - 1)) {
        const uint64_t k = g_slots[s].key.load(std::memory_order_relaxed);
        if (k == key) { present = true; break; }
        if (k == 0) break;
    }
    if (!present) {
        if (g_used >= kMaxUsed) {
            ++g_full;
        } else {
            g_slots[s].package = package;
            g_slots[s].asset = asset;
            g_slots[s].key.store(key, std::memory_order_release);
            ++g_used;
        }
    }
    ReleaseSRWLockExclusive(&g_insertLock);
}

uint64_t Ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

// The detour's target.
void* Hook(void* out, uint64_t tableId)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    uint64_t package = 0, asset = 0;
    if (mode && tableId && Lookup(tableId, &package, &asset)) {
        const uint64_t q[4] = {package, asset, 0, 0};
        memcpy(out, q, sizeof q);
        g_hits.fetch_add(1, std::memory_order_relaxed);
        return out;
    }
    const uint64_t t0 = Ticks();
    g_original(out, tableId);
    const uint64_t dt = Ticks() - t0;
    g_stock.fetch_add(1, std::memory_order_relaxed);
    g_stockTicks.fetch_add(dt, std::memory_order_relaxed);
    if (!mode || !tableId) return out;
    uint64_t q[4];
    memcpy(q, out, sizeof q);
    if (q[0] && !q[2] && !q[3]) {  // an asset path with no sub-path: kept
        g_posStock.fetch_add(1, std::memory_order_relaxed);
        g_posTicks.fetch_add(dt, std::memory_order_relaxed);
        Insert(tableId, q[0], q[1]);
    } else {
        g_notKept.fetch_add(1, std::memory_order_relaxed);
    }
    return out;
}

bool Fail(const char* why)
{
    Log("[stmemo] StringTableMemo not installed: %s", why);
    return false;
}

// Entry detour as stuck_effect_look.cpp: region+0 = trampoline (the 5-byte first instruction, jmp [rip] back to fn+5),
// region+32 = jmp [rip] to the hook; fn's first 5 bytes become a jmp to region+32 (one aligned qword, compare-exchange).
bool Detour(uint8_t* fn, void* hook, uint8_t* region, bool beforeGameCode)
{
    if (memcmp(fn, kPrologue, sizeof kPrologue) != 0) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + sizeof kPrologue;
    memcpy(region, kPrologue, sizeof kPrologue);
    memcpy(region + 5, jmpAbs, sizeof jmpAbs);
    memcpy(region + 11, &resume, sizeof resume);
    memcpy(region + 32, jmpAbs, sizeof jmpAbs);
    memcpy(region + 38, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    const int64_t rel = (region + 32) - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t jmp[5] = {0xE9, 0, 0, 0, 0};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(jmp + 1, &rel32, sizeof rel32);
    const uintptr_t at = reinterpret_cast<uintptr_t>(fn);
    if ((at & 7) + sizeof jmp <= 8) {
        auto* q = reinterpret_cast<volatile LONG64*>(at & ~uintptr_t{7});
        if (!CodeSiteProtect(const_cast<LONG64*>(q), 8, &old)) return false;
        const LONG64 was = *q;
        LONG64 now = was;
        memcpy(reinterpret_cast<uint8_t*>(&now) + (at & 7), jmp, sizeof jmp);
        const bool ok = InterlockedCompareExchange64(q, now, was) == was;
        VirtualProtect(const_cast<LONG64*>(q), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(q), 8);
        if (ok) RecordOwnWrite(fn, sizeof jmp);
        return ok;
    }
    return beforeGameCode && WriteCode(fn, std::span<const uint8_t>(jmp, sizeof jmp));
}

}  // namespace

uint8_t* StringTableMemoFind(const GameModule& game)
{
    const auto hits = FindAll(game.text, ParsePattern(kPattern));
    return hits.size() == 1 ? hits[0] : nullptr;
}

bool InstallStringTableMemo(bool beforeGameCode)
{
    if (g_installed) return true;
    if (g_searched) return false;
    g_searched = true;
    const GameModule& game = Game();
    uint8_t* const fn = StringTableMemoFind(game);
    if (!fn) return Fail("FStringTableEngineBridge::GetAssetReference not found (byte pattern; another game version?)");
    uint8_t* const region = AllocNearGame(64);
    if (!region) return Fail("no free memory near the game's code");
    g_original = reinterpret_cast<GetRefFn>(region);
    if (!Detour(fn, reinterpret_cast<void*>(&Hook), region, beforeGameCode))
        return Fail("GetAssetReference's first instruction changed, another mod hooks it, or the jump could not be placed");
    g_fn = fn;
    g_installed = true;
    Log("[stmemo] installed: FStringTableEngineBridge::GetAssetReference exe+0x%llX detoured (string table asset paths remembered)",
        static_cast<unsigned long long>(fn - game.base));
    return true;
}

void SetStringTableMemo(int mode)
{
    if (!g_installed) return;
    if (mode > 1) mode = 1;
    if (mode < 0 || mode > 2) mode = 1;
    if (g_mode.exchange(mode) != mode) Log("[stmemo] StringTableMemo %d", mode);
}

void LogStringTableMemoStats()
{
    if (!g_installed) return;
    const uint64_t hits = g_hits.load(), stock = g_stock.load();
    const uint64_t sum = hits + stock + g_verified.load();
    if (sum == g_logged) return;
    g_logged = sum;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double us = 1e6 / static_cast<double>(f.QuadPart);
    const uint64_t pos = g_posStock.load();
    const double posUs = pos ? us * static_cast<double>(g_posTicks.load()) / static_cast<double>(pos) : 0.0;
    const double stockUs = stock ? us * static_cast<double>(g_stockTicks.load()) / static_cast<double>(stock) : 0.0;
    AcquireSRWLockShared(&g_insertLock);
    const uint32_t used = g_used;
    ReleaseSRWLockShared(&g_insertLock);
    Log("[stmemo] stats: %llu remembered answers used, %llu asked the game's way (%.1f us each; %llu kept, %.1f us each; "
        "%llu not an asset path), %u table ids kept, %llu not kept (table full); about %.1f ms saved so far; verify %llu "
        "compared, %llu different",
        static_cast<unsigned long long>(hits), static_cast<unsigned long long>(stock), stockUs, static_cast<unsigned long long>(pos),
        posUs, static_cast<unsigned long long>(g_notKept.load()), used, static_cast<unsigned long long>(g_full.load()),
        posUs * static_cast<double>(hits) / 1000.0, static_cast<unsigned long long>(g_verified.load()),
        static_cast<unsigned long long>(g_mismatch.load()));
}

// ---- self-test: the table and the hook with a fake GetAssetReference (nothing installed) ----
namespace {
int g_fakeCalls = 0;
uint64_t g_fakeAnswerShift = 0;  // changes the fake's answer (verify-mode mismatch)
void* FakeGetRef(void* out, uint64_t id)
{
    ++g_fakeCalls;
    uint64_t q[4] = {0, 0, 0, 0};
    if (id >= 0x1000 && id < 0x100000) { q[0] = id * 3 + g_fakeAnswerShift; q[1] = id * 5; }  // assets
    else if (id == 0x300) { q[0] = 0x33; q[1] = 0x44; q[2] = 0x1234; q[3] = 1; }           // a sub-path: not kept
    memcpy(out, q, sizeof q);
    return out;
}
void ResetForTest()
{
    for (Slot& s : g_slots) { s.key.store(0); s.package = s.asset = 0; }
    g_used = 0;
    g_hits = g_stock = g_stockTicks = g_posStock = g_posTicks = g_notKept = g_full = g_verified = g_mismatch = 0;
    g_fakeCalls = 0;
    g_fakeAnswerShift = 0;
}
}  // namespace

int StringTableMemoTestLogic()
{
    if (g_installed) return 0x80000000;  // never on a live detour
    int fail = 0;
    ResetForTest();
    g_original = &FakeGetRef;
    uint64_t q[4];
    auto call = [&](uint64_t id) { memset(q, 0xAB, sizeof q); return Hook(q, id) == q; };
    // off: every call asked, nothing kept
    g_mode = 0;
    if (!call(0x2000) || !call(0x2000) || g_fakeCalls != 2 || g_used != 0) fail |= 1;
    // on: asked once, then remembered, the same 32 bytes
    g_mode = 1;
    g_fakeCalls = 0;
    call(0x2000);
    const uint64_t first[4] = {q[0], q[1], q[2], q[3]};
    call(0x2000);
    if (g_fakeCalls != 1 || g_hits.load() != 1 || memcmp(first, q, sizeof q) != 0 || q[0] != 0x6000 || q[1] != 0xA000 || q[2] || q[3])
        fail |= 2;
    // not an asset (all zero), a sub-path, None: always asked
    g_fakeCalls = 0;
    call(0x200); call(0x200); call(0x300); call(0x300); call(0); call(0);
    if (g_fakeCalls != 6 || g_used != 1 || q[0] || q[1]) fail |= 4;
    // many ids: kept up to 3/4 of the table, then "full"; every one answers right
    for (uint64_t id = 0x1000; id < 0x1000 + kSlots; ++id) {
        call(id);
        if (q[0] != id * 3 || q[1] != id * 5) fail |= 8;
    }
    // 0x2000 (kept above) lies outside 0x1000..0x13FF, so 767 of these are kept and 257 find the table full
    if (g_used != kMaxUsed || g_full.load() != kSlots - (kMaxUsed - 1)) fail |= 16;
    for (uint64_t id = 0x1000; id < 0x1000 + kSlots; ++id) {  // second pass: kept ones hit, the rest asked again
        call(id);
        if (q[0] != id * 3 || q[1] != id * 5) fail |= 32;
    }
    g_mode = 0;
    ResetForTest();
    g_original = nullptr;
    return fail;
}

}  // namespace opf
