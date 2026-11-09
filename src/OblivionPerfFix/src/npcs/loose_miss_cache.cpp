// LooseMissCache (lab fightot2 2026-11-02, played by the maintainer since; public since 1.3.0, worker port130j 2026-11-08 after
// review rev130e; [Fixes] LooseMissCache, default 1; 0 = off). Installed at start-up when != 0 (version.dll route), then live
// 0/1 (0 = every check goes to the real _access, as stock).
// The old engine's file finder (the FileFinder's find, vtable +0x20) checks a path that is not in its archives on disk with
// the CRT's _access(path, 0): first the path itself, then the path under each extra search folder. Combat barks build a voice
// path per line (Data\Sound\Voice\<plugin>\<race>\<sex>\...mp3) and check it this way on the OblivionThread; the loose
// Data\Sound\Voice folder does not exist (the remaster ships its voices in archives only), so every bark costs two failing
// attribute queries (~0.2 ms per frame in a 36-NPC fight at the maintainer's settings; findings 2026-11-01 "otio", 2026-11-02
// "fightot2").
// Both _access calls are redirected here: a voice path (Sound\Voice\... or ...\Data\Sound\Voice\..., the latter also under
// the finder's extra folder ../../../OblivionRemastered/Content/Dev/ObvData/; case-insensitive, either slash) whose real
// _access failed with ENOENT (read from the game's CRT on the calling thread) fails again without the system call (exact
// string, mode 0 only; at most 65536 paths, never freed: a few MB at worst). Everything else, and every first check, goes
// to the real _access.
// Identical while no loose voice file appears under Data\Sound\Voice during a session (the game never writes there; a mod
// manager's virtual file makes the first real _access succeed, so it is never cached); the caller reads only the -1 / not -1
// result (errno is not read after these calls, so the cached answer does not set it).
// How: the find is found by pattern (once) with its second check at +0x110; both calls must go through the _access import
// slot; each 6-byte call [_access] becomes a call to a read-execute jump stub near the game. Any mismatch: not installed,
// one log line.
#include "common.h"

#include <errno.h>  // ENOENT

#include <atomic>
#include <cstring>
#include <string>
#include <unordered_set>

namespace opf {
namespace {

// exe+0x68D0270: push rbx; push rdi; push r14; sub rsp,150h; mov rax,[cookie]; xor rax,rsp; mov [rsp+130h],rax;
// mov edi,r8d; mov rbx,rdx; mov r14,rcx; test r8b,1; jne; mov edx,r9d; mov rcx,rbx; call archive; test al,al; je;
// mov eax,2; jmp; mov [rsp+148h],rbp; mov [rsp+140h],rsi; test dil,2; jne; xor edx,edx; mov rcx,rbx;
// call [_access] (+0x61); cmp eax,-1
constexpr char kFind[] =
    "40 53 57 41 56 48 81 EC 50 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 30 01 00 00 41 8B F8 48 8B DA 4C 8B F1 "
    "41 F6 C0 01 75 19 41 8B D1 48 8B CB E8 ?? ?? ?? ?? 84 C0 74 0A B8 02 00 00 00 E9 ?? ?? ?? ?? 48 89 AC 24 48 01 00 00 "
    "48 89 B4 24 40 01 00 00 40 F6 C7 02 75 14 33 D2 48 8B CB FF 15 ?? ?? ?? ?? 83 F8 FF";
constexpr size_t kSite1 = 0x61;
// +0x110: xor edx,edx; lea rcx,[rsp+20h]; call [_access] (+0x117); cmp eax,-1
constexpr char kSecond[] = "33 D2 48 8D 4C 24 20 FF 15 ?? ?? ?? ?? 83 F8 FF";
constexpr size_t kSecondAt = 0x110, kSite2 = 0x117;

using AccessFn = int(__cdecl*)(const char*, int);
using ErrnoFn = int*(__cdecl*)();
void** g_slot = nullptr;
uint8_t* g_find = nullptr;
ErrnoFn g_gameErrno = nullptr;  // ucrtbase's _errno: the game's CRT (this DLL links its own static CRT)
std::atomic<int> g_mode{0};
bool g_installed = false;
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_set<std::string>* g_miss = nullptr;
constexpr size_t kMaxEntries = 65536;
std::atomic<uint64_t> g_cached{0}, g_stored{0};

// "Sound\Voice\" after an optional ".\" and "Data\", case-insensitive, backslash or slash.
bool VoicePath(const char* p)
{
    auto sep = [](char c) { return c == 0x5C || c == '/'; };  // backslash or slash
    auto word = [&](const char* q, const char* w) -> const char* {  // q starts with w + a separator -> the rest
        for (; *w; ++q, ++w)
            if ((*q | 0x20) != *w) return nullptr;
        return sep(*q) ? q + 1 : nullptr;
    };
    if (p[0] == '.' && sep(p[1])) p += 2;
    auto soundVoice = [&](const char* at) { const char* q = word(at, "sound"); return q && word(q, "voice") != nullptr; };
    if (soundVoice(p)) return true;  // Sound\Voice\... (relative to Data)
    // ...\Data\Sound\Voice\... : the path itself or under one of the finder's extra folders (the second check of each bark is
    // "../../../OblivionRemastered/Content/Dev/ObvData/Data/Sound/Voice/...")
    for (const char* q = p; *q; ++q)
        if (q == p || sep(q[-1]))
            if (const char* r = word(q, "data"); r && soundVoice(r)) return true;
    return false;
}

bool Cached(const char* path)
{
    AcquireSRWLockShared(&g_lock);
    const bool hit = g_miss->count(path) != 0;
    ReleaseSRWLockShared(&g_lock);
    return hit;
}

int __cdecl AccessHook(const char* path, int mode)
{
    const auto real = reinterpret_cast<AccessFn>(*g_slot);
    if (g_mode.load(std::memory_order_relaxed) != 1 || !path || mode != 0 || !VoicePath(path)) return real(path, mode);
    if (Cached(path)) {
        g_cached.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    const int r = real(path, mode);
    if (r != -1) return r;
    if (!g_gameErrno)  // resolved on first use (the game's CRT may not be initialised at install time)
        if (const HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll")) g_gameErrno = reinterpret_cast<ErrnoFn>(GetProcAddress(ucrt, "_errno"));
    if (!g_gameErrno || *g_gameErrno() != ENOENT) return r;  // only "does not exist" is remembered
    AcquireSRWLockExclusive(&g_lock);
    if (g_miss->size() < kMaxEntries && g_miss->insert(path).second) g_stored.fetch_add(1, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

bool Discover(const GameModule& game, LooseMissDiscovery* d, const char** why)
{
    const auto f = FindAll(game.text, ParsePattern(kFind));
    d->finds = static_cast<int>(f.size());
    if (f.size() != 1) { *why = "code not recognised (pattern count)"; return false; }
    if (!MatchesAt(f[0] + kSecondAt, ParsePattern(kSecond))) { *why = "the second check is not where expected"; return false; }
    void** const slot = FindImportSlot(game.base, "api-ms-win-crt-filesystem-l1-1-0.dll", "_access");
    if (!slot || RipTarget(f[0] + kSite1, 2, 6) != reinterpret_cast<uint8_t*>(slot) ||
        RipTarget(f[0] + kSite2, 2, 6) != reinterpret_cast<uint8_t*>(slot)) {
        *why = "the two calls do not go through the _access import";
        return false;
    }
    d->find = f[0];
    d->slot = slot;
    return true;
}

bool Install(const GameModule& game, const char** why)
{
    LooseMissDiscovery d{};
    if (!Discover(game, &d, why)) return false;
    uint8_t* const mem = AllocNearGame(64);
    if (!mem) { *why = "no memory near the game's code"; return false; }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip]; AccessHook
    const void* const hook = reinterpret_cast<void*>(&AccessHook);
    memcpy(mem, jmpAbs, 6);
    memcpy(mem + 6, &hook, 8);
    DWORD old;
    if (!VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old)) { *why = "could not make the stub executable"; return false; }
    FlushInstructionCache(GetCurrentProcess(), mem, 64);
    g_slot = d.slot;
    g_find = d.find;
    if (!g_miss) g_miss = new std::unordered_set<std::string>();
    for (const size_t site : {kSite1, kSite2}) {
        uint8_t b[6] = {0xE8, 0, 0, 0, 0, 0x90};
        const int64_t rel = mem - (d.find + site + 5);
        if (rel != static_cast<int32_t>(rel)) { *why = "out of reach"; return false; }
        const int32_t r = static_cast<int32_t>(rel);
        memcpy(b + 1, &r, 4);
        if (!WriteCode(d.find + site, std::span<const uint8_t>(b, 6))) { *why = "write failed"; return false; }
    }
    g_installed = true;
    return true;
}

}  // namespace

bool LooseMissDiscover(const GameModule& game, LooseMissDiscovery* out)
{
    const char* why = "";
    return Discover(game, out, &why);
}

bool LooseMissTestInstall()
{
    const char* why = "";
    return Install(Game(), &why);
}

bool LooseMissTestVoicePath(const char* path) { return VoicePath(path); }

void LooseMissTestStats(uint64_t* cached, uint64_t* stored)
{
    *cached = g_cached.load();
    *stored = g_stored.load();
}

void SetLooseMissCache(int mode) { g_mode.store(mode ? 1 : 0, std::memory_order_relaxed); }

// Start-up only (version.dll route).
bool InstallLooseMissCache(bool beforeGameCode, int mode)
{
    if (g_installed) return true;
    if (!beforeGameCode) { Log("[loosemiss] LooseMissCache: start-up only (version.dll route), not installed"); return false; }
    const char* why = "";
    if (!Install(Game(), &why)) {
        Log("[loosemiss] LooseMissCache: %s, not installed", why);
        return false;
    }
    SetLooseMissCache(mode);
    Log("[loosemiss] LooseMissCache installed (file finder exe+0x%X, both _access calls): LooseMissCache=%d",
        static_cast<unsigned>(g_find - Game().base), g_mode.load());
    return true;
}

void LogLooseMissCacheStats()
{
    if (!g_installed) return;
    static uint64_t last = 0;
    const uint64_t c = g_cached.load();
    if (c == last) return;
    Log("[loosemiss] missing loose voice files answered from memory: %llu (total %llu, %llu paths remembered)",
        static_cast<unsigned long long>(c - last), static_cast<unsigned long long>(c),
        static_cast<unsigned long long>(g_stored.load()));
    last = c;
}

}  // namespace opf
