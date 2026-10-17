// The game module's sections, byte-pattern parsing and search (hex with ?? wildcards), RIP-relative target
// decoding, and the two ways the fixes write to the game's memory (code bytes, and atomic pointer stores).
#include "common.h"

#include <cstring>

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

std::vector<uint8_t*> FindAll(const Section& s, const Pattern& p)
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
