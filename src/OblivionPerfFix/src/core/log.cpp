// The log file OblivionPerfFix.log next to the DLL: one time-stamped line per call, flushed at once so the log is
// complete even if the game crashes. Thread-safe; does nothing until LogOpen has been called.
//
// Size: the game opens a new log at every start, and the previous ones are kept as OblivionPerfFix.1.log
// (the last session) and OblivionPerfFix.2.log (the one before), so a crash report survives a relaunch. A session's
// log stops at kMaxBytes (a normal hour writes ~0.3 MB); after that only the GPU crash report ([gpucrash]) is written.
#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace opf {

static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;
static size_t g_logBytes = 0;
static bool g_logFull = false;
static constexpr size_t kMaxBytes = 8u << 20;

// "x\OblivionPerfFix.log", 2 -> "x\OblivionPerfFix.2.log"
static std::wstring Numbered(const std::wstring& path, int n)
{
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    const bool ext = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
    const std::wstring stem = ext ? path.substr(0, dot) : path;
    const std::wstring tail = ext ? path.substr(dot) : L"";
    return stem + L"." + std::to_wstring(n) + tail;
}

void LogOpen(const wchar_t* path, int keepPrevious)
{
    std::lock_guard lock(g_logMutex);
    if (g_logFile) return;
    const std::wstring p(path);
    // Oldest first; a file another process still has open stays where it is (and is then overwritten or kept).
    for (int n = keepPrevious; n >= 1; --n)
        MoveFileExW(n == 1 ? p.c_str() : Numbered(p, n - 1).c_str(), Numbered(p, n).c_str(), MOVEFILE_REPLACE_EXISTING);
    g_logFile = _wfsopen(path, L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...)
{
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard lock(g_logMutex);
    if (!g_logFile) return;
    if (g_logFull && strncmp(msg, "[gpucrash]", 10) != 0) return;
    const int n = fprintf(g_logFile, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                          t.wMinute, t.wSecond, t.wMilliseconds, msg);
    g_logBytes += n > 0 ? static_cast<size_t>(n) : 0;
    if (!g_logFull && g_logBytes >= kMaxBytes) {
        g_logFull = true;
        fprintf(g_logFile, "%04d-%02d-%02d %02d:%02d:%02d.%03d log size limit (%zu MB) reached: only a GPU crash report "
                "is written from here on\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                kMaxBytes >> 20);
    }
    fflush(g_logFile);
}

}  // namespace opf
