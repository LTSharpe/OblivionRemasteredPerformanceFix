// The game module's sections, byte-pattern parsing and search (hex with ?? wildcards), RIP-relative target
// decoding, and the two ways the fixes write to the game's memory (code bytes, and atomic pointer stores).
#include "common.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <emmintrin.h>
#include <intrin.h>

namespace opf {

unsigned UsableLogicalProcessors()
{
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) && processMask) {
        unsigned count = 0;
        for (; processMask; processMask &= processMask - 1) ++count;
        return count;
    }
    return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
}

uint64_t QpcToMicroseconds(uint64_t ticks, uint64_t frequency)
{
    return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
}

void DescribeThread(HANDLE thread, const wchar_t* name)
{
    using Fn = HRESULT (WINAPI*)(HANDLE, PCWSTR);
    static const auto describe = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
    if (describe) describe(thread, name);
}

namespace {
using WaitOnAddressFn = BOOL (WINAPI*)(volatile void*, void*, SIZE_T, DWORD);
using WakeByAddressFn = void (WINAPI*)(void*);
struct AddressWait {
    WaitOnAddressFn wait = nullptr;
    WakeByAddressFn wakeAll = nullptr;
    uint64_t spinTicks = 0;  // about 50 us of QPC ticks
    AddressWait()
    {
        // Windows 8+: in kernelbase (the api set api-ms-win-core-synch-l1-2-0), not exported by kernel32 itself.
        if (HMODULE m = GetModuleHandleW(L"kernelbase.dll")) {
            wait = reinterpret_cast<WaitOnAddressFn>(GetProcAddress(m, "WaitOnAddress"));
            wakeAll = reinterpret_cast<WakeByAddressFn>(GetProcAddress(m, "WakeByAddressAll"));
            if (!wait || !wakeAll) wait = nullptr, wakeAll = nullptr;
        }
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        spinTicks = static_cast<uint64_t>(f.QuadPart) / 20000 + 1;
    }
};
const AddressWait& Aw()
{
    static const AddressWait aw;
    return aw;
}
}  // namespace

bool SpinPhase(int& spin, uint64_t& deadline)
{
    if (deadline == ~0ull) return false;
    if ((spin++ & 63) == 0) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        const uint64_t now = static_cast<uint64_t>(t.QuadPart);
        if (!deadline) deadline = now + Aw().spinTicks;
        else if (now >= deadline) { deadline = ~0ull; return false; }
    }
    YieldProcessor();
    return true;
}

void BlockOnWord(volatile void* word, const void* seen, size_t size)
{
    if (const auto wait = Aw().wait) wait(word, const_cast<void*>(seen), size, INFINITE);
    else Sleep(0);
}

void WakeWordAll(volatile void* word)
{
    if (const auto wake = Aw().wakeAll) wake(const_cast<void*>(word));
}


static GameModule g_game;

const GameModule& Game() { return g_game; }

bool ParseModule(GameModule& out, HMODULE module)
{
    auto* base = reinterpret_cast<uint8_t*>(module ? module : GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;

    out = {};
    out.base = base;
    out.timeDateStamp = nt->FileHeader.TimeDateStamp;
    out.sizeOfImage = nt->OptionalHeader.SizeOfImage;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[9] = {};
        memcpy(name, sec->Name, 8);
        Section s{base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
        if (!strcmp(name, ".text")) out.text = s;
        else if (!strcmp(name, ".rdata")) out.rdata = s;
        else if (!strcmp(name, ".data")) out.data = s;
    }
    return out.text.begin && out.rdata.begin;
}

bool NoteGameBuild(const GameModule& game)
{
    const bool known = (game.timeDateStamp == 0xF19077A4u && game.sizeOfImage == 0x9E1E000u) ||   // Steam 19115871
                       (game.timeDateStamp == 0x94273715u && game.sizeOfImage == 0x94A7000u);     // Game Pass, from players' logs
    if (!known)
        Log("game build not recognised (TimeDateStamp 0x%08X, SizeOfImage 0x%X): this version was made for Steam build 19115871 "
            "(0xF19077A4 / 0x9E1E000); Game Pass 0x94273715 / 0x94A7000 is reported working. Each fix looks for its own code: "
            "any it can't find stays off and says so below, which is safe. Please quote this line in a bug report",
            game.timeDateStamp, game.sizeOfImage);
    return known;
}

bool InitGameModule(GameModule& out, HMODULE module)
{
    if (!ParseModule(out, module)) return false;
    g_game = out;
    return true;
}

Pattern ParsePattern(std::string_view text)
{
    Pattern p;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ') { ++i; continue; }
        if (text[i] == '?') {
            p.bytes.push_back(0);
            p.care.push_back(0);
            i += (i + 1 < text.size() && text[i + 1] == '?') ? 2 : 1;
            continue;
        }
        auto hex = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        p.bytes.push_back(static_cast<uint8_t>(hex(text[i]) << 4 | hex(text[i + 1])));
        p.care.push_back(1);
        i += 2;
    }
    return p;
}

bool MatchesAt(const uint8_t* at, const Pattern& p)
{
    for (size_t i = 0; i < p.size(); ++i)
        if (p.care[i] && at[i] != p.bytes[i]) return false;
    return true;
}

namespace {
// How often each byte value occurs in the game's .text (parts per 65536; Steam build 0xF19077A4). Only the search speed
// depends on it: FindAll looks for a pattern's two rarest fixed bytes first. x64 code keeps this profile across builds.
constexpr uint16_t kTextByteFrequency[256] = {
     5293,  1125,   505,   570,   501,   702,   277,   232,   748,    95,   103,   108,   266,   306,    80,  2475,
      810,   347,    63,    50,   132,   199,    85,    70,   274,    85,    57,    39,    86,    69,    42,   138,
      610,    58,    49,    59,  1746,    69,    33,    46,   645,   172,    41,   108,    65,    46,    54,    61,
      403,    48,    56,   300,    86,    51,    34,    39,   269,   126,    66,   252,    77,    63,    40,    49,
      530,   996,   150,   249,   971,   613,   112,   139,  5259,   606,    73,   100,   926,   360,    67,    81,
      316,    56,    41,    98,   180,   157,    93,   209,   253,   216,    67,   105,   344,   128,   111,   164,
      183,    39,    87,   214,    65,    62,   497,    33,   109,    46,    25,    57,   117,    49,    51,    77,
      173,    40,    64,    79,   674,   362,    86,    60,   147,   109,    44,    82,   193,   164,    95,   109,
      228,   160,    56,   855,   334,   713,    54,    78,   201,  1463,    36,  3127,    87,  1359,    54,    61,
      178,    37,    36,    34,    60,    63,    28,    39,    93,    40,    22,    25,    68,    40,    27,    33,
      112,    30,    24,    29,    47,    32,    22,    33,   100,    27,    30,    28,    55,    32,    27,    44,
      125,    37,    32,    33,    70,    39,   126,    87,   164,    83,   103,    45,    69,    57,    42,    53,
      617,   436,   203,   401,   534,   289,   238,   388,   342,   311,   106,   220,  1803,    71,   123,   144,
      223,   104,   172,    99,    61,    56,    81,   108,   157,    89,    77,   137,    51,    43,    46,    55,
      172,    94,    94,   154,    59,    43,    43,    55,   994,   181,    56,   201,   226,    69,    46,    62,
      259,    76,   338,   276,    43,    45,   167,    82,   256,   132,   117,   155,   180,   201,   223,  1412,
};

std::atomic<uint64_t> g_scanCalls{0}, g_scanTicks{0}, g_scanBytes{0}, g_scanMismatches{0};
std::atomic<uint64_t> g_foreignHooks{0};  // [hook] lines (mem.cpp, other mods' hooks)
std::atomic<uint64_t> g_writeRetries{0};  // WriteCode copies that faulted and were retried (another patcher on the page)

// memcpy that reports an access violation instead of crashing (no C++ objects here: __try needs a plain frame)
bool CopyGuarded(void* to, const void* from, size_t n)
{
    __try {
        memcpy(to, from, n);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
std::atomic<int> g_scanMode{0};  // 0 fast, 1 fast checked against the reference, 2 reference only
ScanObserver g_scanObserver = nullptr;

uint64_t Ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

// The fast search: every start in [begin, end - size] where the pattern matches, in address order (the same set and
// order as FindAllReference). SSE2 compares the pattern's two rarest fixed bytes at 16 starts at once; only starts
// where both match are checked in full with MatchesAt.
std::vector<uint8_t*> FindAllFast(const Section& s, const Pattern& p)
{
    std::vector<uint8_t*> hits;
    if (p.size() == 0 || !p.care[0] || static_cast<size_t>(s.end - s.begin) < p.size()) return hits;
    size_t k1 = 0, k2 = 0;  // offsets of the rarest and the second-rarest fixed byte (k2 = k1 with one fixed byte)
    for (size_t i = 1; i < p.size(); ++i) {
        if (!p.care[i]) continue;
        const uint16_t f = kTextByteFrequency[p.bytes[i]];
        if (f < kTextByteFrequency[p.bytes[k1]]) k2 = k1, k1 = i;
        else if (k2 == k1 || f < kTextByteFrequency[p.bytes[k2]]) k2 = i;
    }
    const __m128i b1 = _mm_set1_epi8(static_cast<char>(p.bytes[k1]));
    const __m128i b2 = _mm_set1_epi8(static_cast<char>(p.bytes[k2]));
    uint8_t* at = s.begin;
    uint8_t* const last = s.end - p.size();  // the last start: the loads below read at most up to last + k + 15
    for (; last - at >= 15; at += 16) {      // (k < size), so never past the section's end
        const __m128i x1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(at + k1));
        const __m128i x2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(at + k2));
        unsigned mask = static_cast<unsigned>(
            _mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(x1, b1), _mm_cmpeq_epi8(x2, b2))));
        while (mask) {
            unsigned long bit;
            _BitScanForward(&bit, mask);
            mask &= mask - 1;
            if (MatchesAt(at + bit, p)) hits.push_back(at + bit);
        }
    }
    for (; at <= last; ++at)
        if (MatchesAt(at, p)) hits.push_back(at);
    return hits;
}
}  // namespace

// The original search (OblivionPerfFix up to 1.1.5): memchr for the pattern's first byte, then MatchesAt. Kept as the
// reference that the self-test and the lab key ScanVerify compare FindAll with.
std::vector<uint8_t*> FindAllReference(const Section& s, const Pattern& p)
{
    std::vector<uint8_t*> hits;
    if (p.size() == 0 || !p.care[0] || static_cast<size_t>(s.end - s.begin) < p.size()) return hits;
    const uint8_t first = p.bytes[0];
    for (uint8_t* at = s.begin; at <= s.end - p.size(); ++at) {
        at = static_cast<uint8_t*>(memchr(at, first, (s.end - p.size() + 1) - at));
        if (!at) break;
        if (MatchesAt(at, p)) hits.push_back(at);
    }
    return hits;
}

std::vector<uint8_t*> FindAll(const Section& s, const Pattern& p)
{
    const int mode = g_scanMode.load(std::memory_order_relaxed);
    const uint64_t t0 = Ticks();
    std::vector<uint8_t*> hits = mode == 2 ? FindAllReference(s, p) : FindAllFast(s, p);
    const uint64_t ticks = Ticks() - t0;
    g_scanCalls.fetch_add(1, std::memory_order_relaxed);
    g_scanTicks.fetch_add(ticks, std::memory_order_relaxed);
    g_scanBytes.fetch_add(static_cast<uint64_t>(s.end - s.begin), std::memory_order_relaxed);
    if (mode == 1) {
        std::vector<uint8_t*> ref = FindAllReference(s, p);
        if (ref != hits) {
            const uint64_t n = g_scanMismatches.fetch_add(1) + 1;
            if (n <= 20)
                Log("[scan] verify: MISMATCH %zu-byte pattern (first byte %02X) over %zu bytes: fast %zu hit(s), reference %zu",
                    p.size(), p.bytes.empty() ? 0 : p.bytes[0], static_cast<size_t>(s.end - s.begin), hits.size(), ref.size());
            hits.swap(ref);  // the reference result is used
        }
    }
    if (const ScanObserver observe = g_scanObserver) observe(s, p, hits, ticks);
    return hits;
}

void SetScanMode(int mode) { g_scanMode.store(mode); }
void SetScanObserver(ScanObserver observe) { g_scanObserver = observe; }
ScanStats GetScanStats()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return {g_scanCalls.load(), QpcToMicroseconds(g_scanTicks.load(), static_cast<uint64_t>(f.QuadPart)), g_scanBytes.load(),
            g_scanMismatches.load()};
}

uint8_t* RipTarget(const uint8_t* insn, size_t dispOffset, size_t insnLen)
{
    int32_t disp;
    memcpy(&disp, insn + dispOffset, sizeof disp);
    return const_cast<uint8_t*>(insn) + insnLen + disp;
}

// Our own code memory (AllocNear's regions and the few stubs allocated directly), by allocation base: a jump into it is
// ours, a jump into other anonymous executable memory is another mod's (obseparity review S2).
namespace {
constexpr int kOwnMax = 1024;
SRWLOCK g_ownLock = SRWLOCK_INIT;
const void* g_own[kOwnMax];
int g_ownCount = 0;
bool g_ownFullLogged = false;

// The code ranges we have written (WriteCode, WriteCodeAtomic, CodeSiteProtect): writing there again (a rollback, a live
// switch, a re-install) puts back or changes our own bytes and is never refused (obseparity review 2, R3).
constexpr int kWrittenMax = 4096;
SRWLOCK g_writtenLock = SRWLOCK_INIT;
struct Range { const uint8_t* begin; const uint8_t* end; };
Range g_written[kWrittenMax];
int g_writtenCount = 0;
bool g_writtenFullLogged = false;
}  // namespace

void* RegisterOwnCode(void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return p;
    AcquireSRWLockExclusive(&g_ownLock);
    bool known = false, full = false;
    for (int i = 0; i < g_ownCount; ++i) known = known || g_own[i] == mbi.AllocationBase;
    if (!known && g_ownCount < kOwnMax) g_own[g_ownCount++] = mbi.AllocationBase;
    else if (!known && !g_ownFullLogged) full = g_ownFullLogged = true;
    ReleaseSRWLockExclusive(&g_ownLock);
    if (full)
        Log("[hook] more than %d code regions of ours: later ones count as not ours (a write next to a jump into one is refused)",
            kOwnMax);
    return p;
}

void UnregisterOwnCode(const void* base)
{
    AcquireSRWLockExclusive(&g_ownLock);
    for (int i = 0; i < g_ownCount; ++i)
        if (g_own[i] == base) {
            g_own[i] = g_own[--g_ownCount];
            break;
        }
    ReleaseSRWLockExclusive(&g_ownLock);
}

BOOL FreeOwnCode(void* base)
{
    if (base) UnregisterOwnCode(base);  // before the free: the address may be handed to someone else at once
    return VirtualFree(base, 0, MEM_RELEASE);
}

void RecordOwnWrite(const void* at, size_t size)
{
    const auto* const b = static_cast<const uint8_t*>(at);
    bool full = false;
    AcquireSRWLockExclusive(&g_writtenLock);
    bool known = false;
    for (int i = 0; i < g_writtenCount && !known; ++i)
        if (g_written[i].begin <= b && b + size <= g_written[i].end) known = true;
    if (!known && g_writtenCount < kWrittenMax) g_written[g_writtenCount++] = {b, b + size};
    else if (!known && !g_writtenFullLogged) full = g_writtenFullLogged = true;
    ReleaseSRWLockExclusive(&g_writtenLock);
    if (full) Log("[hook] more than %d code sites written: later ones are checked again when written back", kWrittenMax);
}

void ForgetOwnWritesForTest()
{
    AcquireSRWLockExclusive(&g_writtenLock);
    g_writtenCount = 0;
    ReleaseSRWLockExclusive(&g_writtenLock);
}

bool OverlapsOwnWrite(const void* at, size_t size)
{
    const auto* const b = static_cast<const uint8_t*>(at);
    AcquireSRWLockShared(&g_writtenLock);
    bool hit = false;
    for (int i = 0; i < g_writtenCount && !hit; ++i) hit = g_written[i].begin < b + size && b < g_written[i].end;
    ReleaseSRWLockShared(&g_writtenLock);
    return hit;
}

bool IsOwnCode(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    AcquireSRWLockShared(&g_ownLock);
    bool known = false;
    for (int i = 0; i < g_ownCount; ++i) known = known || g_own[i] == mbi.AllocationBase;
    ReleaseSRWLockShared(&g_ownLock);
    return known;
}

BOOL CodeSiteProtect(void* at, size_t size, DWORD* old)
{
    char why[320];
    if (ForeignHookOverlap(at, size, why, sizeof why)) {
        g_foreignHooks.fetch_add(1, std::memory_order_relaxed);
        Log("[hook] not patched: %s; the fix that needed this site reports itself off or incomplete below", why);
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const BOOL ok = VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, old);
    if (ok) RecordOwnWrite(at, size);
    else Log("[code] could not make %zu code bytes at %p writable (error %lu)", size, at, GetLastError());
    return ok;
}

bool WriteCode(void* at, std::span<const uint8_t> bytes)
{
    // Never over another mod's hook (obseparity): a jump of theirs that covers the site is left alone, the fix stays off.
    char why[320];
    if (ForeignHookOverlap(at, bytes.size(), why, sizeof why)) {
        g_foreignHooks.fetch_add(1, std::memory_order_relaxed);
        Log("[hook] not patched: %s; the fix that needed this site reports itself off or incomplete below", why);
        return false;
    }
    // Before WinMain another DLL's thread may patch code on the same page at the same moment and put its protection back
    // under our copy: the copy is retried with the page made writable again (obseaudit D-1).
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD old;
        if (!VirtualProtect(at, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) {
            Log("[code] could not make %zu code bytes at %p writable (error %lu): not written", bytes.size(), at, GetLastError());
            return false;
        }
        const bool copied = CopyGuarded(at, bytes.data(), bytes.size());
        VirtualProtect(at, bytes.size(), old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, bytes.size());
        if (copied) {
            RecordOwnWrite(at, bytes.size());
            return true;
        }
        g_writeRetries.fetch_add(1, std::memory_order_relaxed);
    }
    Log("[hook] could not write %zu code bytes at %p: the page's protection kept changing under the write (another mod "
        "patching the same code page)", bytes.size(), at);
    return false;
}

bool WriteCodeAtomicFits(const void* at, size_t size)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(at);
    return size > 0 && size <= 8 && (a & ~uintptr_t{7}) == ((a + size - 1) & ~uintptr_t{7});
}

bool WriteCodeAtomic(void* at, std::span<const uint8_t> bytes)
{
    if (!WriteCodeAtomicFits(at, bytes.size())) return false;
    char why[320];
    if (ForeignHookOverlap(at, bytes.size(), why, sizeof why)) {
        g_foreignHooks.fetch_add(1, std::memory_order_relaxed);
        Log("[hook] not patched: %s; the fix that needed this site reports itself off or incomplete below", why);
        return false;
    }
    auto* const q = reinterpret_cast<volatile LONG64*>(reinterpret_cast<uintptr_t>(at) & ~uintptr_t{7});
    const size_t off = reinterpret_cast<uintptr_t>(at) & 7;
    DWORD old;
    if (!VirtualProtect(const_cast<LONG64*>(q), 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; ++attempt) {  // another writer of the same qword: read it again
        const LONG64 before = *q;
        LONG64 after = before;
        memcpy(reinterpret_cast<uint8_t*>(&after) + off, bytes.data(), bytes.size());
        ok = InterlockedCompareExchange64(q, after, before) == before;
    }
    VirtualProtect(const_cast<LONG64*>(q), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(q), 8);
    if (ok) RecordOwnWrite(at, bytes.size());
    return ok;
}

uint64_t CodeWriteRetries() { return g_writeRetries.load(std::memory_order_relaxed); }

bool WritePointer(void** at, void* value)
{
    // A slot of the game's own tables (vtables, native tables; not its import table) that another mod has already pointed
    // at its own code is left alone (obseparity review M1). Slots outside the game's image are not checked.
    char why[320];
    if (ForeignPointerSlot(at, why, sizeof why)) {
        g_foreignHooks.fetch_add(1, std::memory_order_relaxed);
        Log("[hook] not patched: %s; the fix that needed this slot reports itself off or incomplete below", why);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(at, sizeof(void*), PAGE_READWRITE, &old)) return false;
    // An aligned 8-byte store is atomic, so concurrent virtual calls see either the old or the new pointer.
    InterlockedExchangePointer(at, value);
    VirtualProtect(at, sizeof(void*), old, &old);
    return true;
}

// A module's import slot for dll!function (found through the import name table, so it works on the mapped image of the
// self-test too, whose imports are not resolved). Used by DisplayModeCache and the GPU crash report.
void** FindImportSlot(uint8_t* base, const char* dll, const char* function)
{
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0)
                return reinterpret_cast<void**>(&slots[i].u1.Function);
        }
    }
    return nullptr;
}

// A free 64 KB-aligned, committed read-write region within rel32 reach of a module's code (call stubs, trampolines).
uint8_t* AllocNear(const uint8_t* module, size_t sizeOfImage, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    const uintptr_t end = base + sizeOfImage;
    constexpr uintptr_t kReach = 0x70000000;
    for (uintptr_t a = (end + gran - 1) & ~(gran - 1); a + size < base + kReach;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.State == MEM_FREE && a + size <= regionEnd)
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<uint8_t*>(RegisterOwnCode(p));
        a = (regionEnd + gran - 1) & ~(gran - 1);
    }
    for (uintptr_t a = (base & ~(gran - 1)) - gran; a > gran && end - a < kReach; a -= gran)
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            return static_cast<uint8_t*>(RegisterOwnCode(p));
    return nullptr;
}

uint8_t* AllocNearGame(size_t size) { return AllocNear(Game().base, Game().sizeOfImage, size); }

// ---- other mods' hooks (obseparity, 2026-11-21) ----
// On the OBSE64 route our searches run after other injected code (UE4SS, OBSE64, other plugins) may have detoured a
// function's first bytes. A function we only call can still be found by the rest of its code and called through the other
// mod's hook (the entry is the function's address either way); a site we would write over is refused instead.
namespace {
// [p, p + n) committed and readable (not a guard page), within one region.
bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto* const end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return static_cast<const uint8_t*>(p) + n <= end;
}

HMODULE ModuleOf(const void* p)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(p), &m))
        return nullptr;
    return m;
}

// The image range of the module holding p (the game's mapped copy in the self-test is a module too).
bool ModuleRange(const void* p, const uint8_t** begin, const uint8_t** end)
{
    const HMODULE m = ModuleOf(p);
    if (!m) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    *begin = base;
    *end = base + nt->OptionalHeader.SizeOfImage;
    return true;
}

HMODULE SelfModule() { return ModuleOf(reinterpret_cast<const void*>(&SelfModule)); }

// The section of the module holding `at` that holds `slot`: true when it is not executable (data: import / delay-load
// tables, jump tables).
bool SlotInModuleData(const void* at, const void* slot)
{
    const uint8_t* b;
    const uint8_t* e;
    if (!ModuleRange(at, &b, &e) || slot < b || slot >= e) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    const size_t rva = static_cast<const uint8_t*>(slot) - b;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (rva >= sec->VirtualAddress && rva < sec->VirtualAddress + (std::max)(sec->Misc.VirtualSize, sec->SizeOfRawData))
            return !(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE);
    return true;  // headers
}

bool Executable(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    return p && VirtualQuery(p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

// Where a jump chain ends: another mod's code (a module that is not `home` and not ours, or anonymous executable memory
// that is not ours)?
// A function begins at p in its module (the module's .pdata, RUNTIME_FUNCTION.BeginAddress; binary search).
bool IsFunctionStart(const uint8_t* moduleBase, const uint8_t* p)
{
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dir.VirtualAddress || dir.Size < sizeof(RUNTIME_FUNCTION)) return false;
    const auto* const fns = reinterpret_cast<const RUNTIME_FUNCTION*>(moduleBase + dir.VirtualAddress);
    size_t lo = 0, hi = dir.Size / sizeof(RUNTIME_FUNCTION);
    const DWORD rva = static_cast<DWORD>(p - moduleBase);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (fns[mid].BeginAddress < rva) lo = mid + 1;
        else hi = mid;
    }
    return lo < dir.Size / sizeof(RUNTIME_FUNCTION) && fns[lo].BeginAddress == rva;
}

bool ForeignEnd(const uint8_t* end, const uint8_t* homeBegin, const uint8_t* homeEnd)
{
    if (end >= homeBegin && end < homeEnd) return false;  // the module's own code
    if (!Executable(end)) return false;  // data or unmapped: a misread, not a hook
    if (const HMODULE owner = ModuleOf(end)) return owner != SelfModule();
    return !IsOwnCode(end);
}

// One [hook] line per hooked function (several fixes look up the same one).
bool Seen(const void* at)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::vector<const void*> seen;
    AcquireSRWLockExclusive(&lock);
    bool found = false;
    for (const void* p : seen) found = found || p == at;
    if (!found && seen.size() < 256) seen.push_back(at);
    ReleaseSRWLockExclusive(&lock);
    return found;
}

void DescribeSite(const void* at, char* out, size_t size)
{
    const auto* const p = static_cast<const uint8_t*>(at);
    if (Game().base && p >= Game().base && p < Game().base + Game().sizeOfImage)
        snprintf(out, size, "exe+0x%llX", static_cast<unsigned long long>(p - Game().base));
    else DescribeAddress(at, out, size);
}
}  // namespace

JumpInfo DecodeJump(const uint8_t* at)
{
    JumpInfo j;
    if (!Readable(at, 2)) return j;
    int32_t d;
    if (at[0] == 0xE9 && Readable(at, 5)) {
        memcpy(&d, at + 1, 4);
        j = {5, at + 5 + d, false};
    } else if (at[0] == 0xEB) {
        j = {2, at + 2 + static_cast<int8_t>(at[1]), false};
    } else if (at[0] == 0xFF && at[1] == 0x25 && Readable(at, 6)) {
        memcpy(&d, at + 2, 4);
        const uint8_t* const slot = at + 6 + d;
        if (!Readable(slot, 8)) return j;
        const uint8_t* target;
        memcpy(&target, slot, 8);
        // the module's own jump through a pointer in its data (import table, delay-load table, jump table); a slot in an
        // executable section (a code cave) is followed like any other hook
        const bool thunk = d != 0 && SlotInModuleData(at, slot);
        j = {d == 0 ? size_t{14} : size_t{6}, target, thunk};
    } else if ((at[0] == 0x48 || at[0] == 0x49) && (at[1] & 0xF8) == 0xB8 && Readable(at, 13)) {
        // mov r64, imm64; jmp r64 (48 B8+r .. FF E0+r; 49 B8+r .. 41 FF E0+r)
        const uint8_t r = at[1] & 7;
        const uint8_t* target;
        memcpy(&target, at + 2, 8);
        if (at[0] == 0x48 && at[10] == 0xFF && at[11] == (0xE0 | r)) j = {12, target, false};
        else if (at[0] == 0x49 && at[10] == 0x41 && at[11] == 0xFF && at[12] == (0xE0 | r)) j = {13, target, false};
    } else if (at[0] == 0x50 && Readable(at, 16) && at[1] == 0x48 && at[2] == 0xB8 && at[11] == 0x48 && at[12] == 0x87 &&
               at[13] == 0x04 && at[14] == 0x24 && at[15] == 0xC3) {
        // push rax; mov rax, imm64; xchg [rsp], rax; ret (PolyHook2's in-place form)
        const uint8_t* target;
        memcpy(&target, at + 3, 8);
        j = {16, target, false};
    } else if (at[0] == 0x68 && Readable(at, 14)) {
        uint32_t lo, hi;
        memcpy(&lo, at + 1, 4);
        if (at[5] == 0xC3) {
            j = {6, reinterpret_cast<const uint8_t*>(static_cast<intptr_t>(static_cast<int32_t>(lo))), false};
        } else if (at[5] == 0xC7 && at[6] == 0x44 && at[7] == 0x24 && at[8] == 0x04 && at[13] == 0xC3) {
            memcpy(&hi, at + 9, 4);
            j = {14, reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(hi) << 32 | lo), false};
        }
    }
    return j;
}

const uint8_t* FollowJumps(const uint8_t* at)
{
    const uint8_t* p = at;
    for (int hop = 0; hop < 6; ++hop) {
        const JumpInfo j = DecodeJump(p);
        if (!j.length || j.importThunk || !Readable(j.target, 1)) {
            if (j.length && !j.importThunk) p = j.target;  // ends in unreadable memory: report that address
            break;
        }
        p = j.target;
    }
    return p;
}

void DescribeAddress(const void* p, char* out, size_t size)
{
    if (const HMODULE m = ModuleOf(p)) {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        snprintf(out, size, "%ls+0x%llX", name,
                 static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(m)));
        return;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (p && VirtualQuery(p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
        snprintf(out, size, "%s memory at %p",
                 (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
                     ? "anonymous executable" : "anonymous", p);
    else snprintf(out, size, "unmapped memory at %p", p);
}

bool ForeignJumpAt(const uint8_t* at, const uint8_t** finalTarget)
{
    const JumpInfo j = DecodeJump(at);
    if (!j.length || j.importThunk) return false;
    const uint8_t* const end = FollowJumps(at);
    if (finalTarget) *finalTarget = end;
    const uint8_t* b;
    const uint8_t* e;
    if (!ModuleRange(at, &b, &e)) return true;  // code outside any module (a stub) that jumps away: not a module's own code
    return end < b || end >= e;
}

namespace {
// The intact offset of a hooked candidate: the smallest j in [jump length, jump length + 15] from which the pattern's bytes
// up to `checkedTo` match (bytes from checkedTo on are already known to match); SIZE_MAX if none.
size_t IntactFrom(const uint8_t* at, const Pattern& p, size_t jumpLength, size_t checkedTo)
{
    for (size_t j = jumpLength; j <= jumpLength + 15 && j <= checkedTo; ++j) {
        bool ok = true;
        for (size_t i = j; i < checkedTo && ok; ++i) ok = !p.care[i] || at[i] == p.bytes[i];
        if (ok) return j;
    }
    return SIZE_MAX;
}

void LogCallableHook(const char* tag, const uint8_t* at, const uint8_t* end)
{
    if (Seen(at)) return;
    g_foreignHooks.fetch_add(1, std::memory_order_relaxed);
    char site[96], owner[200];
    DescribeSite(at, site, sizeof site);
    DescribeAddress(end, owner, sizeof owner);
    Log("[hook] %s: %s starts with another mod's jump (to %s): found by the rest of its code; our calls go through that "
        "mod's hook, as the game's own calls do", tag, site, owner);
}
}  // namespace

bool MatchesAtCallable(const uint8_t* at, const Pattern& p, const char* tag, size_t readFrom)
{
    if (MatchesAt(at, p)) return true;
    const uint8_t* end = nullptr;
    if (!ForeignJumpAt(at, &end)) return false;
    const size_t len = DecodeJump(at).length;
    if (len >= p.size()) return false;
    const size_t j = IntactFrom(at, p, len, p.size());
    if (j == SIZE_MAX || j > readFrom) return false;
    size_t fixed = 0;
    for (size_t i = j; i < p.size(); ++i) fixed += p.care[i];
    if (fixed < 8) return false;
    LogCallableHook(tag, at, end);
    return true;
}

std::vector<uint8_t*> FindAllCallable(const Section& s, const Pattern& p, const char* tag, size_t readFrom)
{
    std::vector<uint8_t*> hits = FindAll(s, p);
    if (!hits.empty() || p.size() <= 24) return hits;
    // The suffix from offset m (the longest jump form, 14 bytes, plus up to 10 bytes to the next instruction boundary)
    // must start with a fixed byte (FindAll) and keep at least 12 fixed bytes.
    size_t m = 24;
    while (m > 5 && !p.care[m]) --m;
    Pattern q;
    q.bytes.assign(p.bytes.begin() + static_cast<ptrdiff_t>(m), p.bytes.end());
    q.care.assign(p.care.begin() + static_cast<ptrdiff_t>(m), p.care.end());
    size_t fixed = 0;
    for (uint8_t c : q.care) fixed += c;
    if (!q.care[0] || fixed < 12) return hits;
    std::vector<uint8_t*> hooked;
    for (uint8_t* c : FindAll(s, q)) {
        if (c - s.begin < static_cast<ptrdiff_t>(m)) continue;
        uint8_t* const at = c - m;
        if (!ForeignJumpAt(at)) continue;
        const size_t j = IntactFrom(at, p, DecodeJump(at).length, m);
        if (j == SIZE_MAX || j > readFrom) continue;
        hooked.push_back(at);
    }
    if (hooked.size() == 1) {
        LogCallableHook(tag, hooked[0], FollowJumps(hooked[0]));
    } else if (hooked.size() > 1) {
        Log("[hook] %s: %zu functions with another mod's jump at the entry match the rest of the pattern: none used", tag,
            hooked.size());
        hooked.clear();
    }
    return hooked;
}

const char* ForeignHookOverlap(const void* atv, size_t size, char* why, size_t whySize)
{
    // A jump of another mod that starts up to 32 bytes before the site or inside it: it covers the site, or its trampoline
    // resumes there (a detour moves up to the jump's length plus one instruction), or it is a hook right next to it. A jump
    // counts only when its chain leaves the site's module and ends in another module or in executable memory that is not
    // ours, so the game's own jumps never refuse a write (obseparity review S2).
    const auto* const at = static_cast<const uint8_t*>(atv);
    const uint8_t* b;
    const uint8_t* e;
    if (!ModuleRange(at, &b, &e)) return nullptr;  // our stubs and other anonymous memory: not checked
    if (ModuleOf(at) == SelfModule()) return nullptr;
    if (OverlapsOwnWrite(at, size)) return nullptr;  // our own bytes written back (rollback, live switch): review 2, R3
    for (const uint8_t* start = (at - b >= 32 ? at - 32 : b); start < at + size; ++start) {
        const JumpInfo jmp = DecodeJump(start);
        if (!jmp.length || jmp.importThunk) continue;
        // a jump that starts before the site and does not reach it counts only at a function's start, where a detour sits
        // and its trampoline resumes after the moved bytes (review 2, R2: not a misread inside another instruction or the
        // previous function's tail jump)
        if (start < at && start + jmp.length <= at && !IsFunctionStart(b, start)) continue;
        const uint8_t* const end = FollowJumps(start);
        if (!ForeignEnd(end, b, e)) continue;
        char site[96], dest[200];
        DescribeSite(start, site, sizeof site);
        DescribeAddress(end, dest, sizeof dest);
        snprintf(why, whySize, "%s holds another mod's jump (to %s)", site, dest);
        return why;
    }
    return nullptr;
}

const char* ForeignPointerSlot(void* const* slot, char* why, size_t whySize)
{
    const GameModule& g = Game();
    const auto* const s = reinterpret_cast<const uint8_t*>(slot);
    if (!g.base || s < g.base || s >= g.base + g.sizeOfImage) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g.base + reinterpret_cast<const IMAGE_DOS_HEADER*>(g.base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& iat = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    if (iat.VirtualAddress && s >= g.base + iat.VirtualAddress && s < g.base + iat.VirtualAddress + iat.Size) return nullptr;
    const auto* const value = static_cast<const uint8_t*>(*slot);
    if (!ForeignEnd(value, g.base, g.base + g.sizeOfImage)) return nullptr;
    char site[96], dest[200];
    DescribeSite(slot, site, sizeof site);
    DescribeAddress(value, dest, sizeof dest);
    snprintf(why, whySize, "the slot at %s already points to another mod's code (%s)", site, dest);
    return why;
}

uint64_t ForeignHooksMet() { return g_foreignHooks.load(std::memory_order_relaxed); }

// ---- load stage (obseparity, 2026-11-21) ----
namespace {
LoadStage g_loadStage = LoadStage::Late;  // Init sets it (N2: the least permissive until then)

const uint8_t* ThreadStart(HANDLE thread)
{
    using QueryFn = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    void* start = nullptr;
    if (!query || query(thread, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, nullptr) != 0) return nullptr;
    return static_cast<const uint8_t*>(start);
}
}  // namespace

LoadStage CurrentLoadStage() { return g_loadStage; }
void SetLoadStage(LoadStage stage) { g_loadStage = stage; }

const char* LoadStageName(LoadStage stage)
{
    return stage == LoadStage::InDllMain       ? "in DllMain (version.dll: before any game code)"
           : stage == LoadStage::BeforeWinMain ? "before the engine starts its threads (no thread started in the exe yet; OBSE64 calls plugins before WinMain)"
                                               : "late (the game's own threads already run)";
}

bool IsCrashReportingThreadStart(const uint8_t* exeBase, const uint8_t* start)
{
    void** const wait = FindImportSlot(const_cast<uint8_t*>(exeBase), "KERNEL32.dll", "WaitForMultipleObjects");
    if (!wait || !start) return false;
    const uint8_t* const fn = FollowJumps(start);  // the thread's start is an incremental-link style jump thunk
    if (!Readable(fn, 0x60)) return false;
    for (const uint8_t* p = fn; p + 6 <= fn + 0x40; ++p) {
        if (p[0] != 0x41 || p[1] != 0xB9 || p[2] != 0xF4 || p[3] != 0x01 || p[4] != 0 || p[5] != 0) continue;  // mov r9d, 500
        for (const uint8_t* q = p + 6; q + 6 <= p + 6 + 20; ++q)
            if (q[0] == 0xFF && q[1] == 0x15 && reinterpret_cast<void**>(RipTarget(q, 2, 6)) == wait) return true;
    }
    return false;
}

LoadStage ClassifyLoadStage(bool onMainThread, int exeThreads)
{
    return onMainThread && exeThreads == 0 ? LoadStage::BeforeWinMain : LoadStage::Late;
}

namespace {
// This process's thread IDs from one NtQuerySystemInformation(SystemProcessInformation) call (not a Toolhelp snapshot:
// Thread32Next maps memory once per thread on the PC, which the game's embedded browser hooks while it loads; see
// loading/pso_precompile.cpp OwnThreads, the same layout). Empty when the call fails or the layout does not hold.
std::vector<DWORD> ProcessThreadIds()
{
    using QuerySystemInformationFn = LONG(WINAPI*)(ULONG cls, void* info, ULONG size, ULONG* returned);
    static const auto query = reinterpret_cast<QuerySystemInformationFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    std::vector<DWORD> ids;
    if (!query) return ids;
    // x64 SYSTEM_PROCESS_INFORMATION: NextEntryOffset +0, NumberOfThreads +4, UniqueProcessId +0x50, its
    // SYSTEM_THREAD_INFORMATION array at +0x100 (0x50 each: ClientId.UniqueThread +0x30)
    constexpr ULONG kSystemProcessInformation = 5;
    constexpr LONG kInfoLengthMismatch = static_cast<LONG>(0xC0000004);
    constexpr size_t kProcThreads = 0x100, kProcId = 0x50, kThreadSize = 0x50, kThreadId = 0x30;
    std::vector<uint8_t> buf(1u << 20);
    ULONG got = 0;
    LONG status = kInfoLengthMismatch;
    for (int tries = 0; tries < 4; ++tries) {
        status = query(kSystemProcessInformation, buf.data(), static_cast<ULONG>(buf.size()), &got);
        if (status != kInfoLengthMismatch) break;
        buf.resize((got > buf.size() ? got : buf.size() * 2) + (64u << 10));
    }
    if (status < 0 || got > buf.size()) return ids;
    const uintptr_t self = GetCurrentProcessId();
    for (size_t at = 0; at + kProcThreads <= got;) {
        const uint8_t* const p = buf.data() + at;
        ULONG next, count;
        uintptr_t pid;
        memcpy(&next, p, 4);
        memcpy(&count, p + 4, 4);
        memcpy(&pid, p + kProcId, sizeof pid);
        if (pid == self) {
            if (at + kProcThreads + static_cast<size_t>(count) * kThreadSize > got) return ids;
            bool callerSeen = false;
            for (ULONG i = 0; i < count; ++i) {
                uintptr_t tid;
                memcpy(&tid, p + kProcThreads + i * kThreadSize + kThreadId, sizeof tid);
                ids.push_back(static_cast<DWORD>(tid));
                callerSeen = callerSeen || tid == GetCurrentThreadId();
            }
            if (!callerSeen) ids.clear();
            return ids;
        }
        if (!next) break;
        at += next;
    }
    return ids;
}
}  // namespace

ThreadCensus TakeThreadCensus(const uint8_t* exeBase, size_t exeSize, const uint8_t* entry)
{
    ThreadCensus c;
    const DWORD self = GetCurrentThreadId();
    const std::vector<DWORD> ids = ProcessThreadIds();
    if (ids.empty()) return c;
    c.ok = true;
    for (const DWORD id : ids) {
        ++c.threads;
        const HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, id);
        const uint8_t* const start = h ? ThreadStart(h) : nullptr;
        int64_t createdMs = -1;
        FILETIME tc, te2, tk, tu, pc, pe, pk, pu;
        if (h && GetThreadTimes(h, &tc, &te2, &tk, &tu) && GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu)) {
            const auto u64 = [](const FILETIME& f) { return static_cast<int64_t>(static_cast<uint64_t>(f.dwHighDateTime) << 32 | f.dwLowDateTime); };
            createdMs = (u64(tc) - u64(pc)) / 10000;
        }
        if (h) CloseHandle(h);
        if (id == self) c.currentIsEntry = start && start == entry;
        if (!start) {  // cannot tell where it started: counted as an engine thread (a wrong "before WinMain" is the risk)
            ++c.exeThreads;
            if (c.exeStarts.size() < 8) c.exeStarts.push_back({0, createdMs, false});
            continue;
        }
        if (start && start >= exeBase && start < exeBase + exeSize) {
            if (start != entry) {
                const bool crash = IsCrashReportingThreadStart(exeBase, start);
                if (crash) ++c.crashThreads;
                else ++c.exeThreads;
                if (c.exeStarts.size() < 8) c.exeStarts.push_back({static_cast<uint32_t>(start - exeBase), createdMs, crash});
            }
            continue;
        }
        // other threads by the module they started in (UE4SS, OBSE64, the OS's thread pool, ...)
        char name[64] = "unknown";
        if (start) {
            HMODULE m = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(start), &m)) {
                wchar_t path[MAX_PATH] = L"";
                GetModuleFileNameW(m, path, MAX_PATH);
                const wchar_t* n = wcsrchr(path, L'\\');
                snprintf(name, sizeof name, "%ls", n ? n + 1 : path);
            } else snprintf(name, sizeof name, "anonymous memory");
        }
        bool counted = false;
        for (auto& o : c.others)
            if (o.first == name) { ++o.second; counted = true; }
        if (!counted && c.others.size() < 16) c.others.emplace_back(name, 1);
    }
    return c;
}

}  // namespace opf
