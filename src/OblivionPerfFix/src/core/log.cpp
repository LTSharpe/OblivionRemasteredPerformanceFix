// The log file OblivionPerfFix.log next to the DLL: one time-stamped line per call. Thread-safe; does nothing until
// LogOpen has been called.
//
// Writing (2026-11-07, ourbugs1107): a line is formatted and time-stamped by the thread that logs it and copied into a
// queue; a background thread ("OPF log writer") writes the queue to the file, in order. Until 1.2.4 every line was
// written and flushed by the thread that logged it, so a slow disk write (Windows' write path can take tens of ms)
// stopped that thread, often the game thread (our bug: a 40 ms game-thread stall in the maintainer's ride play1107d). The
// file gets the same bytes as before (CRLF line ends, same lines, order and time stamps); only when they reach the disk
// changes, by microseconds normally.
//   Nothing is lost: at a crash (LogCrashMode, called by the crash handlers; and every "[crash]" / "[gpucrash]" line),
//   the queue is written by the calling thread at once, and from then on every line is written by its caller. At the
//   game's end: its TerminateProcess import (how UE ends the game) writes the queue first, and DLL_PROCESS_DETACH
//   (ExitProcess) writes what is left, also the part of a write the ended writer thread was in the middle of.
//   The queue is bounded (kQueueBytes). If it is ever full (the disk stopped for minutes), the caller does not wait: the
//   line is dropped and counted, and the next line that fits is preceded by one "[log] queue full: N line(s) dropped" line.
//   Start-up (LogOpen until LogGoAsync at the end of Init, before the game's code runs) writes synchronously as before.
//
// Size: the game opens a new log at every start, and the previous ones are kept as OblivionPerfFix.1.log
// (the last session) and OblivionPerfFix.2.log (the one before), so a crash report survives a relaunch. A session's
// log stops at kMaxBytes (a normal hour writes ~0.3 MB); after that only the GPU crash report ([gpucrash]) is written.
//
// [General] LogFile=0 (public builds): no log in normal operation. Nothing is renamed, written or deleted at start-up;
// the file is created (with the same rotation) only when a GPU crash report ([gpucrash]) is written, so a crash still
// leaves its report. Every other line is dropped before it is even formatted. No writer thread in this mode.
//
// If the log can't be opened (a game process that did not fully close still holds it, a folder this game may not write
// to) the session's log goes to OblivionPerfFix.<process id>.log next to it, or else to the same name in %TEMP%; its
// first lines say which file failed and why. Each of those folders keeps at most three such logs: the new one and the
// two newest others (the rest are deleted when a new one is created). A normal start touches none of them.
#include "common.h"

#include <cstdarg>
#include <io.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

namespace opf {

static std::mutex g_logMutex;                    // the queue and the log's state (a caller holds it only to copy its line in)
static std::mutex g_fileMutex;                   // writing to the file: the writer thread, or a caller that flushes
static FILE* g_logFile = nullptr;
static HANDLE g_handle = INVALID_HANDLE_VALUE;   // g_logFile's OS handle: every line is written with WriteFile on it
static size_t g_logBytes = 0;
static bool g_logFull = false;
static bool g_crashOnly = false;                 // LogFile=0: only [gpucrash] lines, the first one creates the file
static bool g_logCrashOpened = false;
static int g_keepPrevious = 0;
static std::wstring g_logPath;                   // set by LogOpen: the crash path does no string building
static std::vector<std::wstring> g_rotated;      // g_rotated[n] = the name of the n-th previous log
static std::wstring g_fallbackHere;              // OblivionPerfFix.<pid>.log next to the log (see OpenRotated)
static std::wstring g_fallbackTemp;              // the same name in %TEMP% (empty: Windows gives no temp folder)
static constexpr int kOtherFallbackLogsKept = 2; // per folder, besides the new one
static constexpr size_t kMaxBytes = 8u << 20;
static constexpr size_t kQueueBytes = 512u << 10;  // public builds write a few lines a minute
static constexpr DWORD kCrashWaitMs = 5000;      // a crash-path flush waits this long for a write in progress, then writes anyway

// The queue: two buffers; callers append to g_buf[g_cur] (under g_logMutex), the flusher swaps g_cur and writes the full
// one (under g_fileMutex). Static: no allocation, nothing to fail at start-up.
struct LogBuffer {
    char data[kQueueBytes];
    std::atomic<size_t> len{0};
};
static LogBuffer g_buf[2];
static std::atomic<int> g_cur{0};
static size_t g_capacity = kQueueBytes;          // the self-test lowers it
static std::atomic<bool> g_async{false};         // the writer thread runs: callers only queue their lines
static std::atomic<bool> g_sync{false};          // crash mode: every line is written by its caller at once
static std::atomic<bool> g_stop{false}, g_testHold{false};
static HANDLE g_wake = nullptr;                  // auto-reset: a line arrived in an empty queue
static HANDLE g_writer = nullptr;
static std::atomic<DWORD> g_fileOwner{0};        // thread id holding g_fileMutex (a crash inside a write must not re-lock)
// The write in progress, for DLL_PROCESS_DETACH after ExitProcess has ended the thread doing it.
static std::atomic<const char*> g_inflight{nullptr};
static std::atomic<size_t> g_inflightLen{0};
static std::atomic<long long> g_inflightAt{0};   // file position where it starts
static long long g_filePos = 0;                  // the handle's file position after our writes (under g_fileMutex)
// queue-full drops (under g_logMutex)
static uint64_t g_dropped = 0;
static SYSTEMTIME g_dropFirst{}, g_dropLast{};
// statistics (relaxed)
static std::atomic<uint64_t> g_statLines{0}, g_statBytes{0}, g_statWrites{0}, g_statSlow{0}, g_statMaxWriteUs{0},
    g_statMaxQueue{0}, g_statDropped{0};

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

// Deletes the oldest fallback logs in current's folder until kOtherFallbackLogsKept are left besides current. Only names
// "<stem>.<digits><tail>" with a number above the rotated logs' (.1, .2) count; a file that can't be deleted (another
// game still writes it) ends the clean-up. Stack buffers only: this can run on the crash path.
static void PruneFallbackLogs(const wchar_t* current)
{
    wchar_t path[1024];
    if (wcscpy_s(path, current)) return;
    wchar_t* name = path;
    for (wchar_t* c = path; *c; ++c)
        if (*c == L'\\' || *c == L'/') name = c + 1;
    wchar_t* tailDot = wcsrchr(name, L'.');
    if (!tailDot || tailDot == name) return;
    wchar_t* numberDot = tailDot - 1;
    while (numberDot > name && *numberDot != L'.') --numberDot;
    if (*numberDot != L'.') return;
    wchar_t own[MAX_PATH], tail[16];
    if (wcscpy_s(own, name) || wcscpy_s(tail, tailDot)) return;
    const size_t stemLen = static_cast<size_t>(numberDot - name) + 1;  // with the dot
    const size_t tailLen = wcslen(tail);
    const size_t nameCapacity = _countof(path) - static_cast<size_t>(name - path);

    for (int round = 0; round < 64; ++round) {
        if (stemLen + 1 >= nameCapacity) return;
        name[stemLen] = L'*';
        if (wcscpy_s(name + stemLen + 1, nameCapacity - stemLen - 1, tail)) return;
        WIN32_FIND_DATAW found;
        HANDLE find = FindFirstFileW(path, &found);
        if (find == INVALID_HANDLE_VALUE) return;
        int others = 0;
        wchar_t oldest[MAX_PATH] = L"";
        FILETIME oldestTime{};
        do {
            const wchar_t* candidate = found.cFileName;
            const size_t len = wcslen(candidate);
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || len <= stemLen + tailLen) continue;
            const size_t digits = len - stemLen - tailLen;
            if (digits > 10 || _wcsnicmp(candidate, own, stemLen) || _wcsicmp(candidate + len - tailLen, tail) || !_wcsicmp(candidate, own))
                continue;
            unsigned long long number = 0;
            bool numeric = true;
            for (size_t i = 0; i < digits; ++i) {
                const wchar_t d = candidate[stemLen + i];
                numeric = numeric && d >= L'0' && d <= L'9';
                number = number * 10 + static_cast<unsigned>(d - L'0');
            }
            if (!numeric || number <= static_cast<unsigned long long>(g_keepPrevious)) continue;
            if (!others++ || CompareFileTime(&found.ftLastWriteTime, &oldestTime) < 0) {
                oldestTime = found.ftLastWriteTime;
                wcscpy_s(oldest, candidate);
            }
        } while (FindNextFileW(find, &found));
        FindClose(find);
        if (others <= kOtherFallbackLogsKept) return;
        if (wcscpy_s(name, nameCapacity, oldest) || !DeleteFileW(path)) return;
    }
}

// One line at a fallback log's top: a file that could not be opened and why, in words a player can act on.
static void WriteOpenFailure(const wchar_t* path, unsigned long error, const SYSTEMTIME& t)
{
    char utf8[1024];  // not %ls: the CRT's conversion stops at the first character outside the C locale (a Cyrillic user name)
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof utf8, nullptr, nullptr)) strcpy_s(utf8, "(path too long)");
    const char* why = error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION
                          ? "another program has it open, usually a game process that did not fully close: look in Task Manager"
                      : error == ERROR_ACCESS_DENIED ? "access denied: a read-only file, or a folder this game may not write to"
                      : error == ERROR_PATH_NOT_FOUND ? "the folder does not exist"
                                                      : "unexpected";
    fprintf(g_logFile, "%04d-%02d-%02d %02d:%02d:%02d.%03d could not open %s (Windows error %lu: %s)\n", t.wYear, t.wMonth, t.wDay,
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, utf8, error, why);
}

// Rotates the previous logs away and creates the log; if that fails, a fallback log (see the top of this file). All paths
// were built by LogOpen: this also runs on the crash path. The caller holds g_logMutex.
static void OpenRotated()
{
    // Oldest first; a file another process still has open stays where it is (and is then overwritten or kept).
    for (int n = g_keepPrevious; n >= 1; --n)
        MoveFileExW(n == 1 ? g_logPath.c_str() : g_rotated[n - 1].c_str(), g_rotated[n].c_str(), MOVEFILE_REPLACE_EXISTING);
    _doserrno = 0;
    g_logFile = _wfsopen(g_logPath.c_str(), L"w", _SH_DENYWR);
    if (g_logFile) return;

    const wchar_t* const paths[] = {g_logPath.c_str(), g_fallbackHere.c_str(), g_fallbackTemp.c_str()};
    unsigned long errors[] = {_doserrno, 0, 0};
    for (int i = 1; i < 3 && !g_logFile; ++i) {
        if (!paths[i][0]) continue;
        _doserrno = 0;
        g_logFile = _wfsopen(paths[i], L"w", _SH_DENYWR);
        if (!g_logFile) {
            errors[i] = _doserrno;
            continue;
        }
        PruneFallbackLogs(paths[i]);
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_logFile, "%04d-%02d-%02d %02d:%02d:%02d.%03d OblivionPerfFix " OPF_VERSION ": this session's log is in this file "
                "because the normal log could not be opened. Only the log is affected, not the fixes\n", t.wYear, t.wMonth, t.wDay,
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        for (int j = 0; j < i; ++j)
            if (paths[j][0]) WriteOpenFailure(paths[j], errors[j], t);
        fflush(g_logFile);
    }
}

// "2026-11-07 19:42:07.331 " (24 characters)
static size_t Stamp(char* out, const SYSTEMTIME& t)
{
    const int n = snprintf(out, 32, "%04d-%02d-%02d %02d:%02d:%02d.%03d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                           t.wSecond, t.wMilliseconds);
    return n > 0 ? static_cast<size_t>(n) : 0;
}

// The file was opened (or not): its OS handle and position, for WriteFile. The caller holds g_logMutex.
static void AfterOpen()
{
    g_handle = INVALID_HANDLE_VALUE;
    g_filePos = 0;
    if (!g_logFile) return;
    fflush(g_logFile);  // a fallback log's top lines (fprintf)
    g_handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(g_logFile)));
    LARGE_INTEGER zero{}, pos{};
    if (g_handle != INVALID_HANDLE_VALUE && SetFilePointerEx(g_handle, zero, &pos, FILE_CURRENT)) g_filePos = pos.QuadPart;
}

// Writes n bytes at the handle's position, recording the write as in progress for the exit path.
static void WriteAll(const char* p, size_t n)
{
    if (!n || g_handle == INVALID_HANDLE_VALUE) return;
    g_inflightAt.store(g_filePos);
    g_inflight.store(p);
    g_inflightLen.store(n);
    size_t done = 0;
    while (done < n) {
        DWORD w = 0;
        const DWORD part = static_cast<DWORD>(n - done < (1u << 30) ? n - done : (1u << 30));
        if (!WriteFile(g_handle, p + done, part, &w, nullptr) || !w) break;  // a failing disk: the lines are lost, as before
        done += w;
    }
    g_filePos += static_cast<long long>(done);
    g_inflightLen.store(0);
}

// g_fileMutex with a time limit (crash paths: a write in progress is waited for, a stuck holder is not).
struct FileLock {
    bool owned = false;  // false: re-entered on this thread (a crash inside a write) or timed out: write without the spare buffer
    explicit FileLock(DWORD timeoutMs)
    {
        const DWORD self = GetCurrentThreadId();
        if (g_fileOwner.load() == self) return;
        if (timeoutMs == INFINITE) {
            g_fileMutex.lock();
            owned = true;
        } else {
            const ULONGLONG until = GetTickCount64() + timeoutMs;
            while (!(owned = g_fileMutex.try_lock()) && GetTickCount64() < until) Sleep(1);
            if (!owned) return;
        }
        g_fileOwner.store(self);
    }
    ~FileLock()
    {
        if (!owned) return;
        g_fileOwner.store(0);
        g_fileMutex.unlock();
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
};

// Writes the queue. With the file lock: swaps the buffers (callers go on appending to the empty one) and writes the full
// one without holding g_logMutex. Without it: writes the queue in place under g_logMutex. Returns the bytes written.
static size_t FlushQueue(const FileLock& lock)
{
    if (!lock.owned) {
        std::lock_guard q(g_logMutex);
        LogBuffer& b = g_buf[g_cur.load()];
        const size_t n = b.len.load();
        WriteAll(b.data, n);
        b.len.store(0);
        return n;
    }
    LogBuffer* full;
    size_t n;
    {
        std::lock_guard q(g_logMutex);
        const int cur = g_cur.load();
        full = &g_buf[cur];
        n = full->len.load();
        if (!n) return 0;
        // the batch is described as in progress before the swap (the exit path tells the two apart by g_cur)
        g_inflightAt.store(g_filePos);
        g_inflight.store(full->data);
        g_inflightLen.store(n);
        g_cur.store(cur ^ 1);  // the other buffer is empty: only a flusher empties a buffer, and it holds the file lock
    }
    WriteAll(full->data, n);
    full->len.store(0);
    return n;
}

static void Flush(DWORD timeoutMs)
{
    FileLock lock(timeoutMs);
    FlushQueue(lock);
}

static void LogWriterStats(ULONGLONG ms);

static DWORD WINAPI WriterThread(void*)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    ULONGLONG lastStats = GetTickCount64();
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        const bool stop = g_stop.load();
        while (stop || !g_testHold.load()) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            size_t n;
            {
                FileLock lock(INFINITE);
                n = FlushQueue(lock);
            }
            if (!n) break;
            QueryPerformanceCounter(&t1);
            const uint64_t us = QpcToMicroseconds(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), static_cast<uint64_t>(f.QuadPart));
            g_statWrites.fetch_add(1, std::memory_order_relaxed);
            if (us >= 10000) g_statSlow.fetch_add(1, std::memory_order_relaxed);
            uint64_t m = g_statMaxWriteUs.load(std::memory_order_relaxed);
            while (us > m && !g_statMaxWriteUs.compare_exchange_weak(m, us)) {}
        }
        if (stop) return 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastStats >= 60000) {
            LogWriterStats(now - lastStats);
            lastStats = now;
        }
    }
}

// Lab: one line a minute on the queue (public builds stay quiet; they log only drops).
static void LogWriterStats(ULONGLONG ms)
{
    (void)ms;
}

void LogOpen(const wchar_t* path, int keepPrevious, bool enabled)
{
    std::lock_guard lock(g_logMutex);
    if (g_logFile || g_crashOnly) return;
    g_logPath = path;
    g_keepPrevious = keepPrevious;
    g_rotated.assign(static_cast<size_t>(keepPrevious) + 1, std::wstring());
    for (int n = 1; n <= keepPrevious; ++n) g_rotated[n] = Numbered(g_logPath, n);
    const int pid = static_cast<int>(GetCurrentProcessId());
    g_fallbackHere = Numbered(g_logPath, pid);
    g_fallbackTemp.clear();
    wchar_t temp[MAX_PATH + 1];
    const DWORD tempLen = GetTempPathW(MAX_PATH + 1, temp);
    if (tempLen > 0 && tempLen <= MAX_PATH) {
        const size_t slash = g_logPath.find_last_of(L"\\/");
        g_fallbackTemp = Numbered(temp + (slash == std::wstring::npos ? g_logPath : g_logPath.substr(slash + 1)), pid);
    }
    if (!enabled) {
        g_crashOnly = true;
        return;
    }
    OpenRotated();
    AfterOpen();
}

// The writer thread's TerminateProcess guard: UE ends the game (a forced exit, the end of a crash) with
// TerminateProcess(GetCurrentProcess()), which ends every thread at once; the queue is written first.
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
static TerminateProcessFn g_realTerminate = nullptr;

void LogBeforeExit()
{
    g_sync.store(true);
    Flush(kCrashWaitMs);
}

static BOOL WINAPI TerminateGuard(HANDLE proc, UINT code)
{
    if (proc == GetCurrentProcess() || GetProcessId(proc) == GetCurrentProcessId()) LogBeforeExit();
    return g_realTerminate(proc, code);
}

void LogGoAsync(uint8_t* exeBase)
{
    if (g_async.load() || !g_logFile || g_crashOnly) return;
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stop.store(false);
    g_writer = g_wake ? CreateThread(nullptr, 64 * 1024, &WriterThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr) : nullptr;
    if (!g_writer) {
        Log("[log] no writer thread (error %lu): each line is written by the thread that logs it, as in 1.2.4", GetLastError());
        return;
    }
    DescribeThread(g_writer, L"OPF log writer");
    const char* guard = "";
    if (exeBase) {
        void** slot = FindImportSlot(exeBase, "KERNEL32.dll", "TerminateProcess");
        HMODULE owner = nullptr;
        // chain to what the slot holds (another mod may have hooked it first); only once the loader has filled it
        const bool bound = slot && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                      static_cast<LPCWSTR>(*slot), &owner) && owner;
        if (bound) g_realTerminate = reinterpret_cast<TerminateProcessFn>(*slot);
        guard = !slot ? "; the exe's TerminateProcess import was not found (the game's own end may cut its last lines)"
                : !bound || !WritePointer(slot, reinterpret_cast<void*>(&TerminateGuard))
                    ? "; the exe's TerminateProcess import could not be redirected (the game's own end may cut its last lines)"
                    : "";
    }
    g_async.store(true);
    Log("[log] lines are written by a background thread from here on (queue %zu KB)%s", g_capacity >> 10, guard);
}

bool LogEnabled()
{
    return !g_crashOnly;
}

void LogClose()
{
    if (g_writer) {
        g_stop.store(true);
        g_testHold.store(false);
        SetEvent(g_wake);
        WaitForSingleObject(g_writer, INFINITE);
        CloseHandle(g_writer);
        g_writer = nullptr;
    }
    g_async.store(false);
    Flush(INFINITE);
    std::lock_guard lock(g_logMutex);
    if (g_logFile) fclose(g_logFile);
    g_logFile = nullptr;
    g_handle = INVALID_HANDLE_VALUE;
    g_logBytes = 0;
    g_logFull = false;
    g_crashOnly = false;
    g_logCrashOpened = false;
    g_sync.store(false);
    g_dropped = 0;
    g_capacity = kQueueBytes;
    g_buf[0].len.store(0);
    g_buf[1].len.store(0);
    g_inflightLen.store(0);
}

// Appends a finished line to the queue. The caller holds g_logMutex.
static bool Append(const char* text, size_t n)
{
    LogBuffer& b = g_buf[g_cur.load()];
    const size_t len = b.len.load();
    if (len + n > g_capacity) return false;
    memcpy(b.data + len, text, n);
    b.len.store(len + n);  // after the copy: the exit path writes only whole lines
    uint64_t m = g_statMaxQueue.load(std::memory_order_relaxed);
    while (len + n > m && !g_statMaxQueue.compare_exchange_weak(m, len + n)) {}
    return true;
}

static void Dropped(const SYSTEMTIME& t)
{
    if (!g_dropped++) g_dropFirst = t;
    g_dropLast = t;
    g_statDropped.fetch_add(1, std::memory_order_relaxed);
}

// "<time> [log] queue full: ..." for the lines dropped since the last line that fitted. The caller holds g_logMutex.
static size_t DropNote(char* out, size_t cap, const SYSTEMTIME& t)
{
    size_t n = Stamp(out, t);
    const int k = snprintf(out + n, cap - n, "[log] queue full: %llu line(s) dropped from %02d:%02d:%02d.%03d to %02d:%02d:%02d.%03d "
                           "(the disk did not take the log's lines in time; nothing else is affected)\r\n",
                           static_cast<unsigned long long>(g_dropped), g_dropFirst.wHour, g_dropFirst.wMinute, g_dropFirst.wSecond,
                           g_dropFirst.wMilliseconds, g_dropLast.wHour, g_dropLast.wMinute, g_dropLast.wSecond, g_dropLast.wMilliseconds);
    return k > 0 && static_cast<size_t>(k) < cap - n ? n + static_cast<size_t>(k) : 0;
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
    const bool gpuCrash = strncmp(msg, "[gpucrash]", 10) == 0;
    const bool crashLine = gpuCrash || strncmp(msg, "[crash]", 7) == 0;  // crash-path lines: written at once, with all before them
    // The line as the C runtime's text mode wrote it until 1.2.4: "\n" -> "\r\n"
    char line[32 + 2 * sizeof msg + 2];
    size_t n = Stamp(line, t);
    const size_t counted = n + strlen(msg) + 1;  // what fprintf returned (before the text-mode translation): the size limit
    for (const char* p = msg; *p; ++p) {
        if (*p == '\n') line[n++] = '\r';
        line[n++] = *p;
    }
    line[n++] = '\r';
    line[n++] = '\n';

    bool wake = false;
    {
        std::lock_guard lock(g_logMutex);
        if (!g_logFile && g_crashOnly && !g_logCrashOpened) {
            g_logCrashOpened = true;  // the first crash report: the log is created now (once)
            OpenRotated();
            AfterOpen();
            if (g_logFile) {
                char note[256];
                size_t k = Stamp(note, t);
                const int w = snprintf(note + k, sizeof note - k, "OblivionPerfFix " OPF_VERSION ": LogFile=0 in "
                                       "OblivionPerfFix.ini, so this GPU crash report is the only thing written\r\n");
                if (w > 0) Append(note, k + static_cast<size_t>(w));
            }
        }
        if (!g_logFile) return;
        if (g_logFull && !gpuCrash) return;
        wake = g_buf[g_cur.load()].len.load() == 0;
        if (g_dropped) {
            char note[256];
            const size_t k = DropNote(note, sizeof note, t);
            if (g_buf[g_cur.load()].len.load() + k + n > g_capacity) {
                Dropped(t);
                return;
            }
            Append(note, k);
            g_dropped = 0;
        }
        if (!Append(line, n)) {
            Dropped(t);
            return;
        }
        g_statLines.fetch_add(1, std::memory_order_relaxed);
        g_statBytes.fetch_add(n, std::memory_order_relaxed);
        g_logBytes += counted;
        if (!g_logFull && g_logBytes >= kMaxBytes) {
            g_logFull = true;
            char note[256];
            size_t k = Stamp(note, t);
            const int w = snprintf(note + k, sizeof note - k, "log size limit (%zu MB) reached: only a GPU crash report "
                                   "is written from here on\r\n", kMaxBytes >> 20);
            if (w > 0 && !Append(note, k + static_cast<size_t>(w))) Dropped(t);
        }
    }
    if (crashLine || g_sync.load() || !g_async.load()) Flush(kCrashWaitMs);
    else if (wake) SetEvent(g_wake);
}

void LogFlush()
{
    Flush(kCrashWaitMs);
}

void LogCrashMode()
{
    g_sync.store(true);
    Flush(kCrashWaitMs);
}

void LogProcessExit(bool otherThreadsEnded)
{
    if (!otherThreadsEnded) {  // FreeLibrary (never for this DLL): the writer may still run
        LogBeforeExit();
        return;
    }
    // ExitProcess has ended every other thread, maybe one inside a write or holding a lock: no locks, nothing waits.
    g_sync.store(true);
    if (!g_logFile || g_handle == INVALID_HANDLE_VALUE) return;
    LogBuffer& pending = g_buf[g_cur.load()];
    const char* inflight = g_inflight.load();
    const size_t inflightLen = g_inflightLen.load();
    bool completed = false;  // a cut write was completed
    if (inflightLen && inflight != pending.data) {
        // the rest of the write in progress: the file position says how much of it reached the file
        LARGE_INTEGER zero{}, pos{};
        const long long at = g_inflightAt.load();
        long long done = SetFilePointerEx(g_handle, zero, &pos, FILE_CURRENT) ? pos.QuadPart - at : 0;
        if (done < 0) done = 0;
        g_filePos = at + done;
        if (static_cast<size_t>(done) < inflightLen) {
            WriteAll(inflight + done, inflightLen - static_cast<size_t>(done));
            completed = true;
        }
    }
    WriteAll(pending.data, pending.len.load());
    pending.len.store(0);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char note[256];
    if (g_dropped) {
        WriteAll(note, DropNote(note, sizeof note, t));
        g_dropped = 0;
    }
}

// ---- self-test hooks ----
void LogTestHoldWriter(bool hold)
{
    g_testHold.store(hold);
    if (!hold && g_wake) SetEvent(g_wake);
}

void LogTestSetCapacity(size_t bytes)
{
    std::lock_guard lock(g_logMutex);
    g_capacity = bytes && bytes < kQueueBytes ? bytes : kQueueBytes;
}

bool LogTestAsync()
{
    return g_async.load() && !g_sync.load();
}

// Stands in for a writer thread that ExitProcess ended in the middle of a write: the queue becomes the write in
// progress (as FlushQueue does), its first `written` bytes reach the file, the rest does not. False if the queue is empty.
bool LogTestEndedMidWrite(size_t written)
{
    FileLock lock(INFINITE);
    std::lock_guard q(g_logMutex);
    const int cur = g_cur.load();
    LogBuffer& full = g_buf[cur];
    const size_t n = full.len.load();
    if (!n || written > n || g_handle == INVALID_HANDLE_VALUE) return false;
    g_inflightAt.store(g_filePos);
    g_inflight.store(full.data);
    g_inflightLen.store(n);
    g_cur.store(cur ^ 1);
    DWORD w = 0;
    if (written) WriteFile(g_handle, full.data, static_cast<DWORD>(written), &w, nullptr);
    (void)w;  // g_filePos stays where it was, as when the thread is ended inside WriteFile: the exit path reads the real position
    full.len.store(0);  // the buffer itself stays as it is (the exit path writes from g_inflight)
    return true;
}


}  // namespace opf
