// PluginPrefetch (worker gapfix, 2026-11-16; S-058): at start-up a low-priority helper thread reads the game's active
// plugin files (Oblivion.esm, the DLC and Altar plugins listed in Data\Plugins.txt) once, start to end, so that they are
// in Windows' file cache when the old engine loads them.
//
// What the game does (game behaviour, also without OblivionPerfFix; our reading of build 19115871): about 12-14 s after
// the process starts, the old engine's thread loads every active plugin (exe+0x6625980 -> "Loading Files" loop
// exe+0x6626BC0; Oblivion.esm is 277 MB, AltarESPMain.esp 54 MB) with the C runtime's fopen_s("rb") / fread, record by
// record, while the start-up screen is black and the game thread waits for it. When the files are not in the file cache
// the reads would add up to about 2 s in our start-ups (gapprobe: the same work took 1.4 s in one launch and 3.4-4.1 s
// in two others, the difference in fread). That the slow launches had cold files is our hypothesis (three launches, no
// cache state recorded); it is not proven, and the key is a guess at a cure for it.
//
// What this does: right after our start-up (the version.dll route: about 5 s after the process starts, 7-9 s before
// the old engine's thread needs the files) one thread at the lowest priority, with a low I/O priority hint on each file, reads
// Data\Plugins.txt and then each listed plugin file sequentially in 1 MB pieces into one reused 1 MB buffer, then exits.
// Nothing is kept: Windows keeps the pages in its file cache (standby memory it gives back whenever anything needs it).
// The paths are the game's own: the project folder above Binaries\Win64 (as UE derives it), its Content folder, and the
// game's literal "Dev/ObvData/Data/Plugins.txt" (found in the exe; not found = not started). Names in the list are taken
// only as plain .esm / .esp / .esl file names (no folders, no ".."); missing files are skipped; a failed open or read only
// ends that file. The files are opened for reading with every sharing mode allowed (the game's fopen_s("rb") asks for
// read sharing only, which a read handle never blocks). Caps: 512 names, 2 GB per file, 4 GB in all.
// Identical: only reads; what the game reads is the same bytes, from the cache instead of the disk.
// Tested on an NVMe disk only: on a hard disk the sequential read could compete with the game's own start-up reads; leave
// the key off there.
//
// Key PluginPrefetch: 0 = off; 1 = read the plugin files once at start-up (both routes).
#include "common.h"

#include <string>
#include <vector>

namespace opf {
namespace {

constexpr wchar_t kListLiteral[] = L"Dev/ObvData/Data/Plugins.txt";
constexpr size_t kMaxNames = 512;
constexpr size_t kBuffer = 1 << 20;
constexpr uint64_t kMaxFile = 2ull << 30, kMaxTotal = 4ull << 30;
constexpr DWORD kMaxListBytes = 64 * 1024;

std::wstring g_dataDir;  // ...\Content\Dev\ObvData\Data\ (with the trailing separator)

bool EndsWithI(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

double QpcMs()
{
    static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return 1000.0 / static_cast<double>(q.QuadPart); }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * f;
}

double ProcessAgeS()
{
    FILETIME c, e, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    GetSystemTimeAsFileTime(&now);
    const uint64_t a = (static_cast<uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    const uint64_t b = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return b > a ? static_cast<double>(b - a) / 1e7 : 0;
}

// One file read start to end; returns the bytes read.
uint64_t ReadOnce(const std::wstring& path, uint8_t* buf, uint64_t budget, bool* opened)
{
    *opened = false;
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (GetFileType(h) != FILE_TYPE_DISK) {  // a device name in the list (CON, AUX, ...): not a plugin file, not read
        CloseHandle(h);
        return 0;
    }
    *opened = true;
    FILE_IO_PRIORITY_HINT_INFO hint{IoPriorityHintLow};
    SetFileInformationByHandle(h, FileIoPriorityHintInfo, &hint, sizeof hint);  // best effort
    uint64_t total = 0;
    const uint64_t cap = budget < kMaxFile ? budget : kMaxFile;
    while (total < cap) {
        DWORD got = 0;
        if (!ReadFile(h, buf, static_cast<DWORD>(kBuffer), &got, nullptr) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    return total;
}

DWORD WINAPI PrefetchThread(void*)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    const double t0 = QpcMs(), age0 = ProcessAgeS();
    std::string text;
    {
        const HANDLE h = CreateFileW((g_dataDir + L"Plugins.txt").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            Log("[prefetch] PluginPrefetch: no Data\\Plugins.txt (error %lu), nothing read", GetLastError());
            return 0;
        }
        text.resize(kMaxListBytes);
        DWORD got = 0;
        if (!ReadFile(h, text.data(), kMaxListBytes, &got, nullptr)) got = 0;
        CloseHandle(h);
        text.resize(got);
    }
    std::vector<std::wstring> names;
    PluginPrefetchParse(text.data(), text.size(), &names);
    uint8_t* const buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, kBuffer, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) {
        Log("[prefetch] PluginPrefetch: no read buffer, nothing read");
        return 0;
    }
    uint64_t total = 0;
    int files = 0, missing = 0;
    for (const std::wstring& n : names) {
        if (total >= kMaxTotal) break;
        const double f0 = QpcMs();
        bool opened = false;
        const uint64_t got = ReadOnce(g_dataDir + n, buf, kMaxTotal - total, &opened);
        if (!opened) { ++missing; continue; }
        ++files;
        total += got;
        const double ms = QpcMs() - f0;
        if (got >= (1u << 20))  // the big ones: their read speed says whether they were cached already
            Log("[prefetch] %ls: %.1f MB in %.0f ms (%.0f MB/s)", n.c_str(), got / 1048576.0, ms,
                ms > 0 ? got / 1048576.0 / (ms / 1000.0) : 0.0);
    }
    FreeOwnCode(buf);
    Log("[prefetch] PluginPrefetch: %d plugin files read (%.1f MB) in %.0f ms (process age %.1f -> %.1f s), %d listed but missing",
        files, total / 1048576.0, QpcMs() - t0, age0, ProcessAgeS(), missing);
    return 0;
}

}  // namespace

// Data\Plugins.txt's lines -> plain plugin file names (public for the self-test). Lines: trimmed; empty, '#' and ';'
// lines skipped; only names ending in .esm / .esp / .esl with no folder part, no ':' and no "..".
void PluginPrefetchParse(const char* text, size_t len, std::vector<std::wstring>* out)
{
    out->clear();
    size_t i = 0;
    if (len >= 3 && static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB && static_cast<uint8_t>(text[2]) == 0xBF) i = 3;
    while (i < len && out->size() < kMaxNames) {
        size_t e = i;
        while (e < len && text[e] != '\n' && text[e] != '\r') ++e;
        size_t a = i, b = e;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) --b;
        i = e + 1;
        if (a == b || text[a] == '#' || text[a] == ';' || b - a > 255) continue;
        std::wstring name;
        bool ok = true;
        for (size_t k = a; k < b && ok; ++k) {
            const char c = text[k];
            if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
                static_cast<uint8_t>(c) < 0x20)
                ok = false;
            name.push_back(static_cast<wchar_t>(static_cast<uint8_t>(c)));
        }
        if (!ok || name.find(L"..") != std::wstring::npos) continue;
        if (!EndsWithI(name, L".esm") && !EndsWithI(name, L".esp") && !EndsWithI(name, L".esl")) continue;
        out->push_back(name);
    }
}

// The data folder from the exe's path and the game's literal; empty when either does not fit (public for the self-test).
std::wstring PluginPrefetchDataDir(const std::wstring& exePath, const wchar_t* literal)
{
    const size_t s1 = exePath.find_last_of(L"\\/");
    if (s1 == std::wstring::npos) return {};
    const std::wstring binDir = exePath.substr(0, s1);  // ...\OblivionRemastered\Binaries\Win64
    const wchar_t* const suffix = EndsWithI(binDir, L"\\Binaries\\Win64")     ? L"\\Binaries\\Win64"
                                  : EndsWithI(binDir, L"\\Binaries\\WinGDK") ? L"\\Binaries\\WinGDK"
                                                                             : nullptr;
    if (!suffix) return {};
    const std::wstring project = binDir.substr(0, binDir.size() - wcslen(suffix));
    std::wstring rel(literal);
    for (wchar_t& c : rel)
        if (c == L'/') c = L'\\';
    if (!EndsWithI(rel, L"\\Plugins.txt")) return {};
    rel.resize(rel.size() - wcslen(L"Plugins.txt"));
    return project + L"\\Content\\" + rel;
}

// Whether the exe holds the game's own literal (UTF-16, in .rdata).
bool PluginPrefetchFindLiteral(const GameModule& game)
{
    std::string pat;
    for (const wchar_t* p = kListLiteral; *p; ++p) {
        char b[8];
        sprintf_s(b, "%02X 00 ", static_cast<unsigned>(*p));
        pat += b;
    }
    pat += "00 00";
    return !FindAll(game.rdata, ParsePattern(pat)).empty();
}

void StartPluginPrefetch(int key)
{
    if (key != 1) {
        Log("[prefetch] PluginPrefetch off in OblivionPerfFix.ini");
        return;
    }
    if (!PluginPrefetchFindLiteral(Game())) {
        Log("[prefetch] PluginPrefetch not started: the game's plugin list path was not found in the exe");
        return;
    }
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dataDir = n && n < MAX_PATH ? PluginPrefetchDataDir(std::wstring(exe, n), kListLiteral) : std::wstring();
    if (g_dataDir.empty()) {
        Log("[prefetch] PluginPrefetch not started: the exe is not in <project>\\Binaries\\Win64");
        return;
    }
    if (HANDLE t = CreateThread(nullptr, 0, PrefetchThread, nullptr, 0, nullptr)) {
        CloseHandle(t);
        Log("[prefetch] PluginPrefetch: reading the active plugin files of %ls once (low priority thread)", g_dataDir.c_str());
    } else {
        Log("[prefetch] PluginPrefetch not started: no thread (error %lu)", GetLastError());
    }
}

}  // namespace opf
