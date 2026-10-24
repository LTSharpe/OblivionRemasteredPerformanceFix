// The game module's sections, byte-pattern parsing and search (hex with ?? wildcards), RIP-relative target
// decoding, and the two ways the fixes write to the game's memory (code bytes, and atomic pointer stores).
#include "common.h"

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

bool WriteCode(void* at, std::span<const uint8_t> bytes)
{
    DWORD old;
    if (!VirtualProtect(at, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes.data(), bytes.size());
    VirtualProtect(at, bytes.size(), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, bytes.size());
    return true;
}

bool WritePointer(void** at, void* value)
{
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
                return static_cast<uint8_t*>(p);
        a = (regionEnd + gran - 1) & ~(gran - 1);
    }
    for (uintptr_t a = (base & ~(gran - 1)) - gran; a > gran && end - a < kReach; a -= gran)
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            return static_cast<uint8_t*>(p);
    return nullptr;
}

uint8_t* AllocNearGame(size_t size) { return AllocNear(Game().base, Game().sizeOfImage, size); }

}  // namespace opf
