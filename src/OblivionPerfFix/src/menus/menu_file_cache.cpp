// MenuFileCache (worker menufile 2026-11-25, S-050 / S-051, option A of the dialogue-XML study; public tree, default 0 until a
// release; [Fixes] MenuFileCache, 0 = off, 1 = on, lab 2 = verify). Installed at start-up when != 0, then live 0/1 (lab 0..2).
// Every menu of the old engine (dialogue, pause, journal = stats + inventory + magic + map, container, barter, loading, message,
// ...) is built at each open by Tile::ReadFile (exe+0x66F4CA0), which parses the menu's XML file. The parse gets the file's
// text from a file get (exe+0x66F8F80), and for every <include src=...> (Data\Menus\Prefabs\%s) the same file get again:
// pause_menu.xml includes one button file five times. Its only call of the loader (exe+0x66F8950, at exe+0x66F9024) opens the
// file through the old engine's FileFinder (two _access checks of the loose path, open, read, close: the menus are loose files
// under Content\Dev\ObvData\Data\Menus), then strips comments and whitespace into a new buffer and returns a 16-byte
// {u32 len; char* data} block, both from the game's heap (operator new exe+0x4702CB0); the parse frees them when it is done.
// About 40 % of an open's ReadFile time on the OblivionThread (findings 2026-11-25 "dlgxml"; ~0.9 ms of the dialogue open's
// ~6 ms OT set-up in the bench, more at the journal open, which reads four menus in one frame).
// Here that one call goes to a stub: the loader's output for a path is kept once two successive reads of it gave the same
// bytes (exact path string, Data\Menus\... or Menus\... paths only, len > 0; review revmenufile S3-1: the stock loader
// ignores its read's return value, so a short read (a file locked by a scanner or being written) leaves a truncated or
// garbage text that stock would replace at the next open: two identical reads are kept, a differing pair is not; the first
// copy is held as "pending" until then), and later calls get a fresh copy of those bytes in a fresh
// {len, data} block from the same game allocator, so the parse frees them as it frees the game's own; the stock loader is
// not called for them. A miss, a failed read (the loader returns nothing) or an empty result is asked of the loader every
// time. At most kMaxBytes of text is kept (the 79 menu files are ~1.3 MB raw, ~730 KB stripped); a file that would pass
// it is not kept (one log line), smaller later files still are (review S3-7). Never freed (a few hundred KB).
// Identical: the loader's output depends only on the file's bytes (checked in the disassembly, notes-menufile: the strip
// pass is a byte loop with no other input; the open goes through the FileFinder, whose archives and search folders are set
// at start-up; the loader's second argument (an in-memory text) is always 0 at this call and passed through if not). Same
// bytes -> same parse -> same tiles. What differs: the menu files' existence checks, open and read are skipped after the
// first read (read-only operations), and heap addresses (not deterministic in stock either). A menu XML changed on disk while
// the game runs is seen only after a restart (mod managers place files before the game starts).
// Threads: the OblivionThread at menu opens, the main thread at start-up (HUD); an SRW lock guards the map; entries are never
// changed or removed, so a copy is made outside the lock.
// How: the call site is found by a pattern that must be unique in .text; the loader's entry bytes and the tail that makes
// the {len, data} block (giving operator new and checking that both of its allocations use it) are checked; the rel32 call is
// pointed at a jump stub near the game. Any mismatch: not installed, one log line. Start-up only (the patch is not atomic).
#include "common.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

namespace opf {
namespace {

// exe+0x66F901F (file get +0x9F): xor edx,edx; mov rcx,rsi; call loader (+5); mov rbx,rax; test bpl,bpl; je
constexpr char kSite[] = "33 D2 48 8B CE E8 ?? ?? ?? ?? 48 8B D8 40 84 ED 74";
constexpr size_t kCallAt = 5;
// exe+0x66F8950 loader entry: mov [rsp+8],rbx; mov [rsp+10h],rdx; push rbp; push rsi; push rdi; push r12..r15; sub rsp,30h
constexpr char kLoaderHead[] = "48 89 5C 24 08 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 30";
// loader+0xA9: mov ecx,esi; call operator new (the OUTPUT buffer for the stripped text, raw size; review S3-4 name)
constexpr char kOutAlloc[] = "8B CE E8";
constexpr size_t kOutAllocAt = 0xA9;
// loader+0x25C: mov r12,[rsp+78h]; cmp byte [rsp+80h],0; mov rdi,[rsp+88h]; movsxd rax,ebx; mov byte [rax+rdi],0; je;
// mov rcx,r12; call free (+0x21); mov ecx,10h; call operator new (+0x2B); mov [rsp+78h],rax; test rax,rax; je;
// mov [rax],ebx; mov [rax+8],rdi
constexpr char kLoaderTail[] =
    "4C 8B 64 24 78 80 BC 24 80 00 00 00 00 48 8B BC 24 88 00 00 00 48 63 C3 C6 04 38 00 74 08 49 8B CC E8 ?? ?? ?? ?? "
    "B9 10 00 00 00 E8 ?? ?? ?? ?? 48 89 44 24 78 48 85 C0 74 06 89 18 48 89 78 08";
constexpr size_t kTailAt = 0x25C, kTailAllocAt = 0x2B;
// exe+0x4702CB0 operator new: test rcx,rcx; mov eax,1; mov edx,10h; cmovne rax,rcx; mov rcx,rax; jmp FMemory::Malloc
constexpr char kNew[] = "48 85 C9 B8 01 00 00 00 BA 10 00 00 00 48 0F 45 C1 48 8B C8 E9";

constexpr size_t kMaxBytes = 8u << 20;  // kept text, all paths together

struct FileBuf {  // the loader's result (16 bytes, operator new)
    uint32_t len;
    uint32_t pad;
    char* data;
};
using LoaderFn = FileBuf* (*)(const char* path, const char* text);
using NewFn = void* (*)(size_t size);

struct Entry {
    const char* bytes;  // len bytes, ours (never freed)
    uint32_t len;
};

LoaderFn g_loader = nullptr;  // the stock loader (the call's original target)
NewFn g_new = nullptr;        // the game's operator new
uint8_t* g_site = nullptr;    // the call instruction
std::atomic<int> g_mode{0};
bool g_installed = false;
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<std::string, Entry>* g_map = nullptr;
std::unordered_map<std::string, Entry>* g_pend = nullptr;  // first reads waiting for an identical second (owned, delete[])
size_t g_bytes = 0, g_pendBytes = 0;  // under g_lock
bool g_full = false;  // a file was left out for the size limit (later smaller files are still kept)
std::atomic<bool> g_fullLogged{false};
std::atomic<uint64_t> g_hits{0}, g_misses{0}, g_stored{0}, g_passed{0}, g_compared{0}, g_different{0};
std::atomic<uint64_t> g_diskTicks{0}, g_copyTicks{0}, g_diskCalls{0};

uint64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

// A path with a "Data\Menus\" component pair anywhere (play1125 game check c1: all 503 calls were rejected by the earlier
// start-anchored test, so the game passes the loader a longer path, e.g. with its data folder in front), or one that starts
// with "Menus\" (after an optional ".\" or separator); any case, either slash.
bool MenuPath(const char* p)
{
    auto sep = [](char c) { return c == 0x5C || c == '/'; };
    auto word = [&](const char* q, const char* w) -> const char* {
        for (; *w; ++q, ++w)
            if ((*q | 0x20) != *w) return nullptr;
        return sep(*q) ? q + 1 : nullptr;
    };
    if (p[0] == '.' && sep(p[1])) p += 2;
    if (sep(p[0])) ++p;
    if (word(p, "menus")) return true;
    for (const char* q = p; *q; ++q) {
        if (q != p && !sep(q[-1])) continue;  // a component start
        if (const char* m = word(q, "data"))
            if (word(m, "menus")) return true;
    }
    return false;
}

// The first few paths the test refused, logged once each (to see the game's path forms; no effect on the result).
std::atomic<int> g_refusedLogged{0};
void LogRefused(const char* path, const char* text)
{
    if (g_refusedLogged.load(std::memory_order_relaxed) >= 6 || g_refusedLogged.fetch_add(1) >= 6) return;
    char buf[160] = "";
    if (path) {
        size_t i = 0;
        for (; i + 1 < sizeof buf && path[i]; ++i) buf[i] = (path[i] >= 32 && path[i] < 127) ? path[i] : '?';
        buf[i] = 0;
    }
    Log("[menufile] not kept (%s): \"%s\"", text ? "in-memory text" : !path ? "no path" : "not a Data\\Menus path", buf);
}

bool Lookup(const char* path, Entry* out)
{
    AcquireSRWLockShared(&g_lock);
    const auto it = g_map->find(path);
    const bool hit = it != g_map->end();
    if (hit) *out = it->second;
    ReleaseSRWLockShared(&g_lock);
    return hit;
}

void Store(const char* path, const FileBuf* r)
{
    if (!r || !r->data || r->len == 0 || r->len > 0x7FFFFFFFu) return;  // a failed or empty read is asked again next time
    AcquireSRWLockExclusive(&g_lock);
    bool logFull = false;
    if (g_map->find(path) == g_map->end()) {
        if (g_bytes + r->len > kMaxBytes) {  // this file is not kept; smaller later ones still can be (S3-7)
            g_full = true;
            logFull = true;
        } else {
            const auto pend = g_pend->find(path);
            if (pend != g_pend->end() && pend->second.len == r->len && memcmp(pend->second.bytes, r->data, r->len) == 0) {
                // the second identical read: the pending copy becomes the kept one (S3-1)
                g_map->emplace(path, pend->second);
                g_bytes += r->len;
                g_pendBytes -= r->len;
                g_pend->erase(pend);
                g_stored.fetch_add(1, std::memory_order_relaxed);
            } else if (g_pendBytes + r->len <= kMaxBytes) {
                // the first read (or one that differs from the pending one): held until a read gives the same bytes again
                char* const copy = new char[r->len];
                memcpy(copy, r->data, r->len);
                if (pend != g_pend->end()) {
                    g_pendBytes -= pend->second.len;
                    delete[] pend->second.bytes;
                    pend->second = Entry{copy, r->len};
                } else {
                    g_pend->emplace(path, Entry{copy, r->len});
                }
                g_pendBytes += r->len;
            }
        }
    }
    const size_t files = g_map->size(), bytes = g_bytes;
    ReleaseSRWLockExclusive(&g_lock);
    if (logFull && !g_fullLogged.exchange(true))
        Log("[menufile] MenuFileCache: %zu files (%zu KB) kept, a file of %u KB would pass the limit of %zu KB: that one (and any "
            "other that would) is read as stock", files, bytes >> 10, r->len >> 10, kMaxBytes >> 10);
}

FileBuf* RealLoad(const char* path, const char* text)
{
    const uint64_t t0 = Now();
    FileBuf* const r = g_loader(path, text);
    g_diskTicks.fetch_add(Now() - t0, std::memory_order_relaxed);
    g_diskCalls.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// A fresh {len, data} block from the game's operator new, as the loader makes it (data NUL-terminated at [len]).
FileBuf* Copy(const Entry& e)
{
    char* const data = static_cast<char*>(g_new(static_cast<size_t>(e.len) + 1));
    if (!data) return nullptr;
    FileBuf* const b = static_cast<FileBuf*>(g_new(sizeof(FileBuf)));
    if (!b) return nullptr;  // leaks data; operator new does not return null in this engine (it stops the game)
    memcpy(data, e.bytes, e.len);
    data[e.len] = 0;
    b->len = e.len;
    b->pad = 0;
    b->data = data;
    return b;
}

FileBuf* LoaderHook(const char* path, const char* text)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == 0 || text || !path || !MenuPath(path)) {
        if (mode != 0) {
            g_passed.fetch_add(1, std::memory_order_relaxed);
            LogRefused(path, text);
        }
        return mode == 0 ? g_loader(path, text) : RealLoad(path, text);
    }
    Entry e{};
    if (Lookup(path, &e)) {
        if (mode == 2) {  // verify: the game gets the stock loader's bytes; ours are compared with them
            FileBuf* const r = RealLoad(path, text);
            g_compared.fetch_add(1, std::memory_order_relaxed);
            if (!r || !r->data || r->len != e.len || memcmp(r->data, e.bytes, e.len) != 0) {
                const uint64_t n = g_different.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 8) Log("[menufile] verify: %s differs from the kept copy (%u bytes kept, %u read)", path, e.len,
                                r ? r->len : 0u);
            }
            return r;
        }
        const uint64_t t0 = Now();
        FileBuf* const b = Copy(e);
        if (b) {
            g_copyTicks.fetch_add(Now() - t0, std::memory_order_relaxed);
            g_hits.fetch_add(1, std::memory_order_relaxed);
            return b;
        }
    }
    g_misses.fetch_add(1, std::memory_order_relaxed);
    FileBuf* const r = RealLoad(path, text);
    Store(path, r);
    return r;
}

bool Discover(const GameModule& game, MenuFileDiscovery* d, const char** why)
{
    const auto f = FindAll(game.text, ParsePattern(kSite));
    d->sites = static_cast<int>(f.size());
    if (f.size() != 1) { *why = "code not recognised (call site pattern count)"; return false; }
    uint8_t* const call = f[0] + kCallAt;
    uint8_t* const loader = RipTarget(call, 1, 5);
    if (!game.text.Contains(loader) || !game.text.Contains(loader + kTailAt + ParsePattern(kLoaderTail).size()) || !MatchesAt(loader, ParsePattern(kLoaderHead))) {
        *why = "the loader's entry is not as expected";
        return false;
    }
    if (!MatchesAt(loader + kOutAllocAt, ParsePattern(kOutAlloc)) || !MatchesAt(loader + kTailAt, ParsePattern(kLoaderTail))) {
        *why = "the loader's allocations are not as expected";
        return false;
    }
    uint8_t* const opNew = RipTarget(loader + kTailAt + kTailAllocAt, 1, 5);
    if (RipTarget(loader + kOutAllocAt + 2, 1, 5) != opNew || !game.text.Contains(opNew) || !MatchesAt(opNew, ParsePattern(kNew))) {
        *why = "the loader's two allocations do not use the game's operator new";
        return false;
    }
    d->call = call;
    d->loader = loader;
    d->opNew = opNew;
    return true;
}

bool Install(const GameModule& game, const char** why)
{
    MenuFileDiscovery d{};
    if (!Discover(game, &d, why)) return false;
    uint8_t* const mem = AllocNearGame(64);
    if (!mem) { *why = "no memory near the game's code"; return false; }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip]; LoaderHook
    const void* const hook = reinterpret_cast<void*>(&LoaderHook);
    memcpy(mem, jmpAbs, 6);
    memcpy(mem + 6, &hook, 8);
    DWORD old;
    if (!VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old)) { *why = "could not make the stub executable"; return false; }
    FlushInstructionCache(GetCurrentProcess(), mem, 64);
    const int64_t rel = mem - (d.call + 5);
    if (rel != static_cast<int32_t>(rel)) { *why = "out of reach"; return false; }
    g_loader = reinterpret_cast<LoaderFn>(d.loader);
    g_new = reinterpret_cast<NewFn>(d.opNew);
    g_site = d.call;
    if (!g_map) g_map = new std::unordered_map<std::string, Entry>();
    if (!g_pend) g_pend = new std::unordered_map<std::string, Entry>();
    uint8_t b[5] = {0xE8, 0, 0, 0, 0};
    const int32_t r = static_cast<int32_t>(rel);
    memcpy(b + 1, &r, 4);
    if (!WriteCode(d.call, std::span<const uint8_t>(b, 5))) { *why = "write failed"; return false; }
    g_installed = true;
    return true;
}

}  // namespace

bool MenuFileDiscover(const GameModule& game, MenuFileDiscovery* out)
{
    const char* why = "";
    return Discover(game, out, &why);
}

bool MenuFileTestInstall()
{
    const char* why = "";
    return Install(Game(), &why);
}

bool MenuFileTestPath(const char* path) { return MenuPath(path); }

void* MenuFileTestHook() { return reinterpret_cast<void*>(&LoaderHook); }

void MenuFileTestSetFns(void* loader, void* opNew)
{
    g_loader = reinterpret_cast<LoaderFn>(loader);
    g_new = reinterpret_cast<NewFn>(opNew);
    if (!g_map) g_map = new std::unordered_map<std::string, Entry>();
    if (!g_pend) g_pend = new std::unordered_map<std::string, Entry>();
}

void MenuFileTestStats(MenuFileStats* s)
{
    s->hits = g_hits.load();
    s->misses = g_misses.load();
    s->stored = g_stored.load();
    s->passed = g_passed.load();
    s->compared = g_compared.load();
    s->different = g_different.load();
    AcquireSRWLockShared(&g_lock);
    s->bytes = g_bytes;
    s->full = g_full;
    ReleaseSRWLockShared(&g_lock);
}

void SetMenuFileCache(int mode)
{
    g_mode.store(mode ? 1 : 0, std::memory_order_relaxed);
}

// Start-up only (version.dll route or OBSE64 before WinMain).
bool InstallMenuFileCache(bool beforeGameCode, int mode)
{
    if (g_installed) return true;
    if (!beforeGameCode) { Log("[menufile] MenuFileCache: start-up only (loaded late here), not installed"); return false; }
    const char* why = "";
    if (!Install(Game(), &why)) {
        Log("[menufile] MenuFileCache: %s, not installed", why);
        return false;
    }
    SetMenuFileCache(mode);
    Log("[menufile] MenuFileCache installed (menu file loader exe+0x%X, its call at exe+0x%X): MenuFileCache=%d",
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(g_loader) - Game().base), static_cast<unsigned>(g_site - Game().base),
        g_mode.load());
    return true;
}

void LogMenuFileCacheStats()
{
    if (!g_installed) return;
    static uint64_t lastHits = 0, lastDisk = 0, lastCompared = 0;
    const uint64_t hits = g_hits.load(), disk = g_diskCalls.load(), compared = g_compared.load();
    if (hits == lastHits && disk == lastDisk && compared == lastCompared) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const uint64_t freq = f.QuadPart ? static_cast<uint64_t>(f.QuadPart) : 1;
    MenuFileStats s{};
    MenuFileTestStats(&s);
    const uint64_t diskUs = QpcToMicroseconds(g_diskTicks.load(), freq), copyUs = QpcToMicroseconds(g_copyTicks.load(), freq);
    Log("[menufile] menu files from memory: %llu (total %llu, %.2f ms copying); read by the game: %llu (total %llu, %.2f ms, "
        "%llu not cached kinds); kept %llu files, %zu KB%s; verify: %llu compared, %llu different",
        static_cast<unsigned long long>(hits - lastHits), static_cast<unsigned long long>(hits), copyUs / 1000.0,
        static_cast<unsigned long long>(disk - lastDisk), static_cast<unsigned long long>(disk), diskUs / 1000.0,
        static_cast<unsigned long long>(s.passed), static_cast<unsigned long long>(s.stored), s.bytes >> 10,
        s.full ? " (full)" : "", static_cast<unsigned long long>(s.compared), static_cast<unsigned long long>(s.different));
    lastHits = hits;
    lastDisk = disk;
    lastCompared = compared;
}

}  // namespace opf
