// The log file OblivionPerfFix.log next to the DLL: one time-stamped line per call, flushed at once so the log is
// complete even if the game crashes. Thread-safe; does nothing until LogOpen has been called.
//
// Size: the game opens a new log at every start, and the previous ones are kept as OblivionPerfFix.1.log
// (the last session) and OblivionPerfFix.2.log (the one before), so a crash report survives a relaunch. A session's
// log stops at kMaxBytes (a normal hour writes ~0.3 MB); after that only the GPU crash report ([gpucrash]) is written.
//
// [General] LogFile=0 (public builds): no log in normal operation. Nothing is renamed, written or deleted at start-up;
// the file is created (with the same rotation) only when a GPU crash report ([gpucrash]) is written, so a crash still
// leaves its report. Every other line is dropped before it is even formatted.
#include "common.h"

#include <cstdarg>
#include <io.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace opf {

static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;
static size_t g_logBytes = 0;
static bool g_logFull = false;
static bool g_crashOnly = false;                 // LogFile=0: only [gpucrash] lines, the first one creates the file
static bool g_logCrashOpened = false;
static int g_keepPrevious = 0;
static std::wstring g_logPath;                   // set by LogOpen: the crash path does no string building
static std::vector<std::wstring> g_rotated;      // g_rotated[n] = the name of the n-th previous log
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

// Rotates the previous logs away and creates the log. The caller holds g_logMutex.
static void OpenRotated()
{
    // Oldest first; a file another process still has open stays where it is (and is then overwritten or kept).
    for (int n = g_keepPrevious; n >= 1; --n)
        MoveFileExW(n == 1 ? g_logPath.c_str() : g_rotated[n - 1].c_str(), g_rotated[n].c_str(), MOVEFILE_REPLACE_EXISTING);
    g_logFile = _wfsopen(g_logPath.c_str(), L"w", _SH_DENYWR);
}

void LogOpen(const wchar_t* path, int keepPrevious, bool enabled)
{
    std::lock_guard lock(g_logMutex);
    if (g_logFile || g_crashOnly) return;
    g_logPath = path;
    g_keepPrevious = keepPrevious;
    g_rotated.assign(static_cast<size_t>(keepPrevious) + 1, std::wstring());
    for (int n = 1; n <= keepPrevious; ++n) g_rotated[n] = Numbered(g_logPath, n);
    if (!enabled) {
        g_crashOnly = true;
        return;
    }
    OpenRotated();
}

bool LogEnabled()
{
    return !g_crashOnly;
}

void LogClose()
{
    std::lock_guard lock(g_logMutex);
    if (g_logFile) fclose(g_logFile);
    g_logFile = nullptr;
    g_logBytes = 0;
    g_logFull = false;
    g_crashOnly = false;
    g_logCrashOpened = false;
}

void Log(const char* fmt, ...)
{
    if (g_crashOnly && strncmp(fmt, "[gpucrash]", 10) != 0) return;  // LogFile=0 (set once at start-up)
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard lock(g_logMutex);
    if (!g_logFile && g_crashOnly && !g_logCrashOpened) {
        g_logCrashOpened = true;  // the first crash report: the log is created now (once)
        OpenRotated();
        if (g_logFile)
            fprintf(g_logFile, "%04d-%02d-%02d %02d:%02d:%02d.%03d OblivionPerfFix " OPF_VERSION ": LogFile=0 in "
                    "OblivionPerfFix.ini, so this GPU crash report is the only thing written\n", t.wYear, t.wMonth,
                    t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    }
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
