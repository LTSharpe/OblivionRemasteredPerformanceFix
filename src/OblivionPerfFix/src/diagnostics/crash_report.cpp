// CrashReport and HangReport (worker pubdiagbuild, 2026-11-28).
// When the game crashes or its main thread stops finishing frames, OblivionPerfFix-crash.txt (next to the DLL) says where:
// the faulting module and offset, the thread, its call stack, which of our keys' code is on it, and what happened in the last
// minute (the event ring). Plain text, one fact per line; module names only, never paths (paths hold the Windows user name).
//
// The report file: opened at start-up and at once marked for deletion (FileDispositionInfo), so a session without a record
// leaves no file however it ends; the first record clears the mark. A previous non-empty report is kept as .1.txt.
//
// Crash record ([General] CrashReport=1, start-up only):
//  - A vectored exception handler, first in the list, ALWAYS returns EXCEPTION_CONTINUE_SEARCH: it never handles an exception
//    and never changes a register or the thread's last error. For the fatal-type codes (access violation, in-page error,
//    illegal instruction, division by zero, stack overflow, heap corruption, UE's fatal error 0x4000 and GPU crash 0x8000, ...)
//    it copies the code, address, five registers and a 32-frame stack walk (RtlLookupFunctionEntry / RtlVirtualUnwind, every
//    read guarded) into one of 16 fixed slots. No heap, no lock, no formatting there (~5 us per such exception; ~20 ns for
//    every other code). C++ throws only note their address per thread.
//  - A first-chance exception is not yet a crash. A slot becomes a record only once it is shown to be fatal:
//    (1) the game ends through the exe's TerminateProcess import with an exit code other than 0 (UE's crash path ends with 3
//        after its own crash report, UE 5.3 LaunchWindows.cpp / WindowsPlatformMisc.cpp): the calling thread's slot, else the
//        newest one <= 120 s old;
//    (2) the watchdog thread finds the thread still inside that exception's dispatch 3 s after it (UE's ReportCrash waiting
//        for its minidump, or WER): the thread is stopped for a few microseconds (stack copy), and its walked stack still holds
//        KiUserExceptionDispatcher or RtlRaiseException around the exception's context record. Otherwise the exception was
//        handled: the slot is dropped and counted ("first-chance fatal-type exceptions handled" in the stats line);
//    (3) DLL_PROCESS_DETACH (ExitProcess) with a slot <= 120 s old, unless the game had asked to exit (PostQuitMessage).
//  - The record is written by the watchdog thread (a thread of ours whose C runtime is set up); TerminateProcess waits for it
//    up to 10 s, then writes itself. The log gets a 3-line summary through try-locks only.
//
// Hang record ([General] HangReport=1, HangReportSeconds=10; start-up install, seconds live):
//  - The clock: the engine's GFrameCounter (found by ConditionalCollectGarbage's code, gc_pass_after_busy.cpp; works on the
//    Game Pass exe too), else NpcCreationPacing's frame count. The watchdog wakes every 500 ms; a frame's open time counts only
//    its own ticks, each capped at 1 s (a PC sleep or a suspended process is no hang). Armed after the first frame; off for the
//    rest of the session once the game asks to exit (the exe's PostQuitMessage import on the game thread: UE spends ~10 s
//    without frames in its exit) and while a debugger is attached.
//  - A frame open HangReportSeconds: one record with every thread's stack (each stopped one at a time for GetThreadContext and
//    a <= 128 KB stack copy, walked after it runs again), the render thread's frames, the event ring, the memory history and
//    the D3D12 device's removed reason (+ DRED when removed; asked by a helper thread, waited for 3 s at most). 2 x the time:
//    the stacks again. A frame that ends later gets one "ended after" line. At most 3 records per session. Nothing is killed,
//    resumed, skipped or changed.
//
// The event ring: 512 x 64 B (static), written lock-free by the producers (CrashEvent), printed by every record.
//
// No dbghelp, no heap use in the exception handler, no loader lock (VirtualQuery + K32GetMappedFileNameW for modules), only
// try-locks with a 200 ms limit; nothing in the network.
#include "common.h"

#include <intrin.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace opf {
bool ReadMenuFreeze(int32_t* namedFreezes, int32_t* mode);  // loading/fade_hook.cpp
const volatile uint64_t* FindGFrameCounter();                // memory/gc_pass_after_busy.cpp (locate only)
bool NpcPacingLastPlayerPos(double* x, double* y, double* z);

namespace {
constexpr int kSlots = 16;
constexpr int kFrames = 32;          // the crashing thread's stack
constexpr int kDepth = 24;           // other threads' stacks
constexpr int kMaxThreads = 640;
constexpr size_t kCopy = 128u << 10; // a stopped thread's stack copy
constexpr DWORD kTickMs = 500;
constexpr double kTickCapMs = 1000;
constexpr int kRing = 512;
constexpr double kSlotMaxAgeS = 120;
constexpr int kMaxHangRecords = 3;
constexpr DWORD kStackOverflow = 0xC00000FD, kCppThrow = 0xE06D7363, kUeAssert = 0x4000, kUeGpu = 0x8000;

int g_lateCheckMs = 3000;  // rule 2's age (the self-test lowers it)

inline uint64_t Qpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return static_cast<uint64_t>(q.QuadPart); }
int64_t g_freq = 1;
inline double QSec(int64_t q) { return static_cast<double>(q) / static_cast<double>(g_freq); }

// ---------------------------------------------------------------------------------------------------------------- state
std::atomic<bool> g_crashOn{false}, g_hangOn{false}, g_ringOn{false};
// (the handler flag is thread_local t_own, below)
HANDLE g_file = INVALID_HANDLE_VALUE;
std::atomic<bool> g_fileKept{false};
SRWLOCK g_writeLock = SRWLOCK_INIT;
wchar_t g_path[MAX_PATH + 32] = L"";
uint64_t g_startQpc = 0;
DWORD g_mainTid = 0;
std::atomic<DWORD> g_gtTid{0};
std::atomic<DWORD> g_watchdogTid{0};
HANDLE g_wake = nullptr;      // auto-reset: a TerminateProcess request
HANDLE g_reqDone = nullptr;   // auto-reset: the watchdog finished it
std::atomic<int> g_reqSlot{-1};
std::atomic<DWORD> g_reqTid{0};
std::atomic<UINT> g_reqCode{0};
std::atomic<uint64_t> g_quitAnyQpc{0};  // first PostQuitMessage from any thread (a hang record written later says so)
std::atomic<bool> g_quit{false};       // the game asked to exit (PostQuitMessage on the game thread)
std::atomic<bool> g_terminating{false};
uint64_t g_kiDispatcher = 0, g_kiEnd = 0, g_raise = 0, g_raiseEnd = 0;
// header facts (start-up)
char g_exeName[64] = "?";
uint32_t g_exeStamp = 0, g_exeSize = 0;
char g_pdb[80] = "?";
const uint8_t* g_selfBase = nullptr;
size_t g_selfSize = 0;

// counters
std::atomic<uint64_t> g_fatalSeen{0}, g_handled{0}, g_slotsFull{0}, g_records{0}, g_throwsSeen{0};

using NtQitFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
NtQitFn g_qit = nullptr;

// ---------------------------------------------------------------------------------------------------------------- ring
struct alignas(64) RingEntry {
    std::atomic<uint64_t> seq;  // index + 1 once complete; 0 while being written
    uint64_t qpc;
    uint16_t kind, pad;
    uint32_t tid;
    uint64_t a, b, c;
    uint32_t frame;  // the game frame number (low 32 bits) when it happened
    char text[12];
};
static_assert(sizeof(RingEntry) == 64, "ring entry size");
RingEntry g_ring[kRing];
std::atomic<uint64_t> g_ringHead{0};

// ---------------------------------------------------------------------------------------------------------------- slots
enum SlotState : int { kFree = 0, kBusy, kPending, kTaken, kWritten, kDropped };
constexpr int kWalk = 64;   // frames walked (ancestry checks); the record prints kFrames of them
constexpr int kScan = 16;   // return-address candidates of a raw stack scan where the unwinder stopped
constexpr int kRegs = 16;   // rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8-r15
struct alignas(16) Slot {
    CONTEXT ctx;  // the walk's working copy (first: 16-byte aligned)
    std::atomic<int> state;
    DWORD tid;
    uint64_t qpc;
    uint32_t code, flags, nParams;
    uint64_t addr, params[3];
    uint64_t rip, rsp, ctxAddr, erAddr;
    uint64_t gpr[kRegs];
    uint8_t regMem[kRegs][64];  // 64 bytes at each register that pointed to readable memory (regMemOk bit set)
    uint32_t regMemOk;
    uint64_t pc[kWalk], sp[kWalk];
    int depth;
    uint64_t scan[kScan];
    int nScan;
    bool sawLive;     // the late check found it still in dispatch at least once
    bool byLate;      // written by the late check (rule 2): re-checked afterwards ("handled after N s")
    bool lateNoted;   // that re-check's line was written
    char msg[304];
};
Slot g_slots[kSlots];
struct ThrowNote { std::atomic<DWORD> tid; uint64_t ip, qpc; };
ThrowNote g_throws[kSlots];
std::atomic<unsigned> g_throwNext{0};
// per thread: inside our handler, or one of our own threads / a record being written (their faults are never slots)
thread_local int t_own = 0;

bool Taken(DWORD code)
{
    switch (code) {
    case 0xC0000005: case 0xC0000006: case 0xC000001D: case 0xC0000096: case 0xC0000094: case 0xC0000095:
    case 0xC000008C: case 0xC000008D: case 0xC000008E: case 0xC000008F: case 0xC0000090: case 0xC0000091:
    case 0xC0000092: case 0xC0000093: case kStackOverflow: case 0xC0000374: case 0xC0000409: case 0xC0000017:
    case 0x80000003: case 0x80000002: case 0xC00002B4: case 0xC00002B5: case 0xC0000420: case 0xC0000025: case 0xC0000026:
    case 0xC000041D: case kUeAssert: case kUeGpu: case kCppThrow:
        return true;
    default:
        return false;
    }
}

const char* CodeName(DWORD code)
{
    switch (code) {
    case 0xC0000005: return "ACCESS_VIOLATION";
    case 0xC0000006: return "IN_PAGE_ERROR (a file or disk read failed)";
    case 0xC000001D: return "ILLEGAL_INSTRUCTION";
    case 0xC0000096: return "PRIVILEGED_INSTRUCTION";
    case 0xC0000094: return "INTEGER_DIVIDE_BY_ZERO";
    case 0xC0000095: return "INTEGER_OVERFLOW";
    case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
    case 0xC000008D: case 0xC000008E: case 0xC000008F: case 0xC0000090: case 0xC0000091: case 0xC0000092: case 0xC0000093:
        return "FLOAT_EXCEPTION";
    case 0xC00002B4: case 0xC00002B5: return "FLOAT_MULTIPLE_FAULTS / TRAPS (SSE)";
    case kStackOverflow: return "STACK_OVERFLOW";
    case 0xC0000374: return "HEAP_CORRUPTION";
    case 0xC0000409: return "STACK_BUFFER_OVERRUN / FAST_FAIL";
    case 0xC0000017: return "NO_MEMORY";
    case 0x80000003: return "BREAKPOINT";
    case 0x80000002: return "DATATYPE_MISALIGNMENT";
    case 0xC0000420: return "ASSERTION_FAILURE";
    case 0xC0000025: return "NONCONTINUABLE_EXCEPTION";
    case 0xC0000026: return "INVALID_DISPOSITION";
    case 0xC000041D: return "FATAL_USER_CALLBACK_EXCEPTION";
    case kUeAssert: return "UE fatal error / assert (0x4000)";
    case kUeGpu: return "UE GPU crash (0x8000)";
    case kCppThrow: return "C++ exception";
    default: return "exception";
    }
}

// ---------------------------------------------------------------------------------------------------------------- walk
// The current thread's own stack from a context (the handler: the faulting frames lie below it on this stack). sp[] gets
// each frame's stack pointer; *stop the stack pointer where the walk stopped (for the raw scan).
int WalkLive(CONTEXT* c, uint64_t* out, uint64_t* sp, int max, uint64_t* stop)
{
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uint64_t lo = reinterpret_cast<uint64_t>(tib->StackLimit), hi = reinterpret_cast<uint64_t>(tib->StackBase);
    int k = 0, pops = 0;
    uint64_t lastSp = 0;
    *stop = 0;
    __try {
        while (k < max && c->Rip) {
            out[k] = c->Rip;
            sp[k] = c->Rsp;
            ++k;
            if (c->Rsp < lo || c->Rsp + 8 > hi || (k > 1 && c->Rsp <= lastSp)) break;
            lastSp = c->Rsp;
            *stop = c->Rsp;
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c->Rip, &image, nullptr);
            if (fe) {
                pops = 0;
                void* hd = nullptr;
                DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c->Rip, fe, c, &hd, &est, nullptr);
            } else {
                if (++pops > 2) break;  // no unwind data (JIT code, a stub): the return address at [rsp], twice at most
                c->Rip = *reinterpret_cast<const DWORD64*>(c->Rsp);
                c->Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return k;
}

// Where the unwinder stopped: words on the stack that are return addresses into code with unwind data (the frames of other
// mods' stubs and JIT code hide the rest of the stack from the unwinder), up to kScan of them in the next 2 KB
int RawScan(uint64_t from, uint64_t* out)
{
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uint64_t hi = reinterpret_cast<uint64_t>(tib->StackBase);
    int n = 0;
    __try {
        for (uint64_t p = from & ~7ull; p + 8 <= hi && p < from + 2048 && n < kScan; p += 8) {
            const uint64_t v = *reinterpret_cast<const uint64_t*>(p);
            if (v < 0x10000 || v > 0x7FFFFFFFFFFFull) continue;
            DWORD64 image = 0;
            if (RtlLookupFunctionEntry(v, &image, nullptr)) out[n++] = v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

inline void Rebase(DWORD64* r, uint64_t lo, uint64_t hi, uint64_t delta) { if (*r >= lo && *r < hi) *r += delta; }

// A copied stack (another thread's): registers that point into the original stack are moved into the copy before each step.
// sp[] gets each frame's stack pointer in the ORIGINAL stack's addresses.
int WalkCopy(CONTEXT* c, const uint8_t* copy, size_t n, uint64_t* out, uint64_t* sp, int max)
{
    const uint64_t lo = c->Rsp, hi = lo + n, delta = reinterpret_cast<uint64_t>(copy) - lo;
    const uint64_t copyLo = reinterpret_cast<uint64_t>(copy), copyEnd = copyLo + n;
    int k = 0, pops = 0;
    uint64_t lastSp = 0;
    __try {
        DWORD64* const regs[] = {&c->Rsp, &c->Rbp, &c->Rbx, &c->Rsi, &c->Rdi, &c->R12, &c->R13, &c->R14, &c->R15};
        for (DWORD64* r : regs) Rebase(r, lo, hi, delta);
        while (k < max && c->Rip) {
            const DWORD64 rip = c->Rip;
            out[k] = rip;
            if (sp) sp[k] = c->Rsp >= copyLo && c->Rsp < copyEnd ? c->Rsp - delta : c->Rsp;
            ++k;
            if (c->Rsp < copyLo || c->Rsp + 8 > copyEnd || (k > 1 && c->Rsp <= lastSp)) break;
            lastSp = c->Rsp;
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(rip, &image, nullptr);
            if (fe) {
                pops = 0;
                void* hd = nullptr;
                DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, rip, fe, c, &hd, &est, nullptr);
            } else {
                if (++pops > 2) break;
                c->Rip = *reinterpret_cast<const DWORD64*>(c->Rsp);
                c->Rsp += 8;
            }
            for (DWORD64* r : regs) Rebase(r, lo, hi, delta);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return k;
}

// ---------------------------------------------------------------------------------------------------------------- liveness
// A slot's exception is still being dispatched only while (a) its EXCEPTION_RECORD and CONTEXT still hold that exception
// (code, address, RIP, RSP; read guarded) and (b) a walk of its thread passes through the dispatcher frame that owns that
// CONTEXT: KiUserExceptionDispatcher's frame starts exactly at it (hardware faults), RtlRaiseException's frame holds it as a
// local (software exceptions: UE's fatal error, C++). Anything else (unwound, the storage reused) = it was handled.
bool RecordIntact(const Slot& s)
{
    __try {
        const auto* er = reinterpret_cast<const EXCEPTION_RECORD*>(s.erAddr);
        const auto* cr = reinterpret_cast<const CONTEXT*>(s.ctxAddr);
        return er->ExceptionCode == s.code && reinterpret_cast<uint64_t>(er->ExceptionAddress) == s.addr && cr->Rip == s.rip &&
               cr->Rsp == s.rsp;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool AncestorIn(const Slot& s, const uint64_t* pc, const uint64_t* sp, int depth)
{
    for (int k = 0; k < depth; ++k) {
        const bool ki = pc[k] >= g_kiDispatcher && pc[k] < g_kiEnd;
        const bool raise = pc[k] >= g_raise && pc[k] < g_raiseEnd;
        if (ki && sp[k] == s.ctxAddr) return true;
        if (raise && s.ctxAddr >= sp[k] && (k + 1 >= depth || s.ctxAddr < sp[k + 1])) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------- handler
// The places of first-chance fatal-type exceptions (the first 16 distinct addresses, with counts and how many were found
// handled) for the stats line, the ring's coalescing and the exit rules (a place handled again and again is a probe)
struct Site { std::atomic<uint64_t> addr; std::atomic<uint32_t> count, handled; uint32_t code; };
Site g_sites[16];
Site* NoteSite(DWORD code, uint64_t addr)
{
    for (Site& s : g_sites) {
        uint64_t a = s.addr.load(std::memory_order_relaxed);
        if (a == addr) { s.count.fetch_add(1, std::memory_order_relaxed); return &s; }
        if (!a) {
            if (s.addr.compare_exchange_strong(a, addr)) { s.code = code; s.count.fetch_add(1, std::memory_order_relaxed); return &s; }
            if (a == addr) { s.count.fetch_add(1, std::memory_order_relaxed); return &s; }
        }
    }
    return nullptr;
}
Site* SiteOf(uint64_t addr)
{
    for (Site& s : g_sites)
        if (s.addr.load(std::memory_order_relaxed) == addr) return &s;
    return nullptr;
}
void NoteHandled(const Slot& s)
{
    g_handled.fetch_add(1, std::memory_order_relaxed);
    if (Site* site = SiteOf(s.addr)) site->handled.fetch_add(1, std::memory_order_relaxed);
}

// A free or judged slot, else this thread's pending slot whose record is no longer intact (handled), else the oldest
// pending one older than 120 s
Slot* TakeSlot(DWORD tid)
{
    for (Slot& s : g_slots) {
        int st = kFree;
        if (s.state.compare_exchange_strong(st, kBusy)) return &s;
    }
    for (Slot& s : g_slots) {
        int st = s.state.load(std::memory_order_acquire);
        if (st == kPending && s.tid == tid && !RecordIntact(s) && s.state.compare_exchange_strong(st, kBusy)) {
            NoteHandled(s);
            return &s;
        }
    }
    const uint64_t now = Qpc();
    Slot* best = nullptr;
    for (Slot& s : g_slots) {
        const int st = s.state.load(std::memory_order_acquire);
        const bool judged = (st == kWritten && !s.byLate) || st == kDropped;
        const bool stale = st == kPending && QSec(static_cast<int64_t>(now - s.qpc)) > kSlotMaxAgeS;
        if ((judged || stale) && (!best || s.qpc < best->qpc)) best = &s;
    }
    if (best) {
        int st = best->state.load(std::memory_order_acquire);
        if ((st == kWritten || st == kDropped || st == kPending) && best->state.compare_exchange_strong(st, kBusy)) return best;
    }
    return nullptr;
}

void CopyAssertMessage(Slot* s, ULONG_PTR info)
{
    // UE's FAssertInfo { const TCHAR* ErrorMessage; void* ProgramCounter; } (WindowsPlatformCrashContext.cpp, UE 5.3)
    __try {
        const wchar_t* m = *reinterpret_cast<const wchar_t* const*>(info);
        size_t i = 0;
        for (; m && m[i] && i < 300; ++i) s->msg[i] = m[i] >= 32 && m[i] < 127 ? static_cast<char>(m[i]) : (m[i] == L'\n' ? ' ' : '?');
        s->msg[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s->msg[0] = 0;
    }
}

void NoteThrow(DWORD tid, const void* ip)
{
    g_throwsSeen.fetch_add(1, std::memory_order_relaxed);
    for (ThrowNote& t : g_throws)
        if (t.tid.load(std::memory_order_relaxed) == tid) { t.ip = reinterpret_cast<uint64_t>(ip); t.qpc = Qpc(); return; }
    ThrowNote& t = g_throws[g_throwNext.fetch_add(1, std::memory_order_relaxed) % kSlots];
    t.tid.store(0, std::memory_order_relaxed);
    t.ip = reinterpret_cast<uint64_t>(ip);
    t.qpc = Qpc();
    t.tid.store(tid, std::memory_order_release);
}

bool Readable64(uint64_t p)
{
    if (p < 0x10000 || p > 0x7FFFFFFFFFC0ull) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    const DWORD pr = mbi.Protect & 0xFF;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || pr == PAGE_NOACCESS || pr == PAGE_EXECUTE) return false;
    return p + 64 <= reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
}

void Fill(Slot* s, const EXCEPTION_RECORD* er, const CONTEXT* cr, DWORD tid, DWORD code, uint64_t rip);

__declspec(noinline) void Snapshot(EXCEPTION_POINTERS* info, DWORD code)
{
    const EXCEPTION_RECORD* er = info->ExceptionRecord;
    const CONTEXT* cr = info->ContextRecord;
    const DWORD tid = GetCurrentThreadId();
    if (code == kCppThrow) { NoteThrow(tid, er->ExceptionAddress); return; }
    g_fatalSeen.fetch_add(1, std::memory_order_relaxed);
    Site* site = NoteSite(code, reinterpret_cast<uint64_t>(er->ExceptionAddress));
    const uint64_t rip = cr->Rip;  // read before a slot is taken: a bad context faults here, with no slot held
    Slot* s = TakeSlot(tid);
    if (!s) { g_slotsFull.fetch_add(1, std::memory_order_relaxed); return; }
    __try {
        Fill(s, er, cr, tid, code, rip);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s->state.store(kFree, std::memory_order_release);
        return;
    }
    s->state.store(kPending, std::memory_order_release);
    // this thread's other pending slots: still dispatching only if this walk passes through their dispatcher frame
    for (Slot& o : g_slots) {
        if (&o == s || o.tid != tid) continue;
        int st = kPending;
        if (o.state.load(std::memory_order_acquire) != kPending) continue;
        if (AncestorIn(o, s->pc, s->sp, s->depth) && RecordIntact(o)) continue;
        if (o.state.compare_exchange_strong(st, kDropped)) NoteHandled(o);
    }
    // the ring: a place's 1st, 2nd, 4th, 8th ... time only (a probe that faults all the time does not flood it); c = count
    const uint32_t n = site ? site->count.load(std::memory_order_relaxed) : 1;
    if ((n & (n - 1)) == 0) CrashEvent(kEvException, code, s->addr, n);
}

__declspec(noinline) void Fill(Slot* s, const EXCEPTION_RECORD* er, const CONTEXT* cr, DWORD tid, DWORD code, uint64_t rip)
{
    s->tid = tid;
    s->qpc = Qpc();
    s->code = code;
    s->flags = er->ExceptionFlags;
    s->addr = reinterpret_cast<uint64_t>(er->ExceptionAddress);
    s->nParams = er->NumberParameters < 3 ? er->NumberParameters : 3;
    for (uint32_t i = 0; i < 3; ++i) s->params[i] = i < s->nParams ? er->ExceptionInformation[i] : 0;
    s->ctxAddr = reinterpret_cast<uint64_t>(cr);
    s->erAddr = reinterpret_cast<uint64_t>(er);
    s->rip = rip;
    s->rsp = cr->Rsp;
    const DWORD64 r[kRegs] = {cr->Rax, cr->Rcx, cr->Rdx, cr->Rbx, cr->Rsp, cr->Rbp, cr->Rsi, cr->Rdi,
                              cr->R8,  cr->R9,  cr->R10, cr->R11, cr->R12, cr->R13, cr->R14, cr->R15};
    for (int i = 0; i < kRegs; ++i) s->gpr[i] = r[i];
    s->regMemOk = 0;
    s->depth = 0;
    s->nScan = 0;
    s->sawLive = s->byLate = s->lateNoted = false;
    s->msg[0] = 0;
    // a stack overflow, or a stack nearly used up (< 32 KB left): code, registers and RIP only, so that our handler cannot
    // take the last of the stack the game's own crash handler needs
    const auto* teb = reinterpret_cast<const uint8_t*>(NtCurrentTeb());
    const uint64_t deallocation = *reinterpret_cast<const uint64_t*>(teb + 0x1478);  // TEB.DeallocationStack (x64)
    const bool lowStack = deallocation && reinterpret_cast<uint64_t>(_AddressOfReturnAddress()) - deallocation < (32u << 10);
    if (code == kStackOverflow || lowStack) return;
    if (code == kUeAssert && er->NumberParameters >= 1) CopyAssertMessage(s, er->ExceptionInformation[0]);
    for (int i = 0; i < kRegs; ++i)
        if (i != 4 && Readable64(s->gpr[i])) {  // not rsp: the stack follows in the walk
            __try {
                memcpy(s->regMem[i], reinterpret_cast<const void*>(s->gpr[i]), 64);
                s->regMemOk |= 1u << i;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    memcpy(&s->ctx, cr, sizeof(CONTEXT));
    uint64_t stop = 0;
    s->depth = WalkLive(&s->ctx, s->pc, s->sp, kWalk, &stop);
    if (s->depth < kWalk && stop) s->nScan = RawScan(stop, s->scan);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!Taken(code)) return EXCEPTION_CONTINUE_SEARCH;
    if (t_own) return EXCEPTION_CONTINUE_SEARCH;  // our own thread, a record being written, or a fault inside this handler
    const DWORD lastError = GetLastError();
    t_own = 1;
    __try {
        Snapshot(info, code);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    t_own = 0;
    SetLastError(lastError);
    return EXCEPTION_CONTINUE_SEARCH;  // never handled here, never changed
}

// ---------------------------------------------------------------------------------------------------------------- modules
struct Mod { uint64_t base, size; char name[48]; };
Mod g_mods[256];
int g_nMods = 0;

const Mod* ModOf(uint64_t a)
{
    for (int i = 0; i < g_nMods; ++i)
        if (a >= g_mods[i].base && a < g_mods[i].base + g_mods[i].size) return &g_mods[i];
    MEMORY_BASIC_INFORMATION mbi;
    if (g_nMods >= 256 || !VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE || !mbi.AllocationBase)
        return nullptr;
    const uint8_t* const b = static_cast<const uint8_t*>(mbi.AllocationBase);
    uint64_t size = 0;
    __try {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
        size = nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (a >= reinterpret_cast<uint64_t>(b) + size) return nullptr;
    Mod& m = g_mods[g_nMods];
    m.base = reinterpret_cast<uint64_t>(b);
    m.size = size;
    wchar_t path[MAX_PATH] = L"";
    K32GetMappedFileNameW(GetCurrentProcess(), const_cast<uint8_t*>(b), path, MAX_PATH);
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\') name = p + 1;
    size_t i = 0;
    for (; name[i] && i < sizeof m.name - 1; ++i) m.name[i] = name[i] < 127 ? static_cast<char>(name[i]) : '?';
    m.name[i] = 0;
    if (!i) strcpy_s(m.name, "module");
    ++g_nMods;
    return &m;
}

// ---------------------------------------------------------------------------------------------------------------- output
constexpr size_t kOut = 64u << 10;
char g_out[kOut];
size_t g_outLen = 0;
bool g_outNoLock = false;  // the exit path writes without the lock (every other thread has ended)

void OutFlush()
{
    if (g_file != INVALID_HANDLE_VALUE && g_outLen) {
        DWORD w = 0;
        WriteFile(g_file, g_out, static_cast<DWORD>(g_outLen), &w, nullptr);
    }
    g_outLen = 0;
}

void Out(const char* fmt, ...)
{
    if (g_outLen > kOut - 2048) OutFlush();
    const size_t room = kOut - g_outLen;  // vsnprintf writes at most room - 1 characters and a terminator
    va_list a;
    va_start(a, fmt);
    const int n = vsnprintf(g_out + g_outLen, room, fmt, a);
    va_end(a);
    if (n > 0) g_outLen += static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room - 1;
}

// A block of text as it is (the GPU helper's lines: up to 16 KB): the buffer first, then the block, straight to the file
void OutRaw(const char* p, size_t n)
{
    OutFlush();
    if (g_file != INVALID_HANDLE_VALUE && n) {
        DWORD w = 0;
        WriteFile(g_file, p, static_cast<DWORD>(n), &w, nullptr);
    }
}

// The log's sink: memory_monitor's lines while a record prints them ("[gpucrash] " dropped)
void SinkLine(const char* line)
{
    if (strncmp(line, "[gpucrash] ", 11) == 0) line += 11;
    Out("  %s\n", line);
}

bool LockWriter()
{
    if (g_outNoLock) return true;
    const ULONGLONG until = GetTickCount64() + 200;
    while (!TryAcquireSRWLockExclusive(&g_writeLock)) {
        if (GetTickCount64() >= until) return false;
        Sleep(1);
    }
    return true;
}
void UnlockWriter()
{
    if (!g_outNoLock) ReleaseSRWLockExclusive(&g_writeLock);
}

void KeepFile()
{
    if (g_fileKept.exchange(true) || g_file == INVALID_HANDLE_VALUE) return;
    FILE_DISPOSITION_INFO d{};
    d.DeleteFile = FALSE;
    if (!SetFileInformationByHandle(g_file, FileDispositionInfo, &d, sizeof d))
        LogCrashTry("[crash] the report file could not be kept (Windows refused to clear its delete mark): it may be gone after the game ends");
}

void ThreadName(DWORD tid, char* out, size_t cap)
{
    out[0] = 0;
    if (!g_qit) return;
    HANDLE h = tid == GetCurrentThreadId() ? GetCurrentThread() : OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h) return;
    struct { USHORT len, max; ULONG pad; PWSTR buf; wchar_t text[96]; } nm{};
    ULONG got = 0;
    if (g_qit(h, 38 /* ThreadNameInformation */, &nm, sizeof nm, &got) >= 0 && nm.buf && nm.len) {
        size_t n = nm.len / 2 < cap - 1 ? nm.len / 2 : cap - 1;
        for (size_t i = 0; i < n; ++i) out[i] = nm.buf[i] >= 32 && nm.buf[i] < 127 ? static_cast<char>(nm.buf[i]) : '_';
        out[n] = 0;
    }
    if (h != GetCurrentThread()) CloseHandle(h);
}

const char* ThreadLabel(DWORD tid)
{
    const DWORD gt = g_gtTid.load(std::memory_order_relaxed);
    if (tid == (gt ? gt : g_mainTid)) return " (game thread)";
    if (tid == g_watchdogTid.load(std::memory_order_relaxed)) return " (OPF watchdog)";
    return "";
}

// A module's file version from its mapped resource section (RT_VERSION's VS_FIXEDFILEINFO; no version.dll call)
bool FileVersion(const uint8_t* b, uint32_t* ms, uint32_t* ls)
{
    __try {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
        const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        if (!d.VirtualAddress) return false;
        const uint8_t* root = b + d.VirtualAddress;
        auto entries = [](const uint8_t* dir) {
            return reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY_ENTRY*>(dir + sizeof(IMAGE_RESOURCE_DIRECTORY));
        };
        auto count = [](const uint8_t* dir) {
            const auto* r = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY*>(dir);
            return static_cast<int>(r->NumberOfNamedEntries) + r->NumberOfIdEntries;
        };
        const uint8_t* level = nullptr;
        for (int i = 0; i < count(root); ++i) {
            const auto& e = entries(root)[i];
            if (!e.NameIsString && e.Id == 16 && e.DataIsDirectory) level = root + e.OffsetToDirectory;  // RT_VERSION
        }
        for (int depth = 0; level && depth < 2; ++depth) {  // name, then language: the first of each
            if (!count(level)) return false;
            const auto& e = entries(level)[0];
            if (depth == 0 && !e.DataIsDirectory) return false;
            if (depth == 0) { level = root + e.OffsetToDirectory; continue; }
            if (e.DataIsDirectory) return false;
            const auto* de = reinterpret_cast<const IMAGE_RESOURCE_DATA_ENTRY*>(root + e.OffsetToData);
            const uint8_t* p = b + de->OffsetToData;
            for (uint32_t k = 0; k + 52 <= de->Size && k < 512; k += 4) {
                uint32_t sig;
                memcpy(&sig, p + k, 4);
                if (sig != 0xFEEF04BD) continue;
                memcpy(ms, p + k + 8, 4);
                memcpy(ls, p + k + 12, 4);
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// The version resource's strings (StringFileInfo, the first table): ProductName, FileDescription, FileVersion. A plain walk of
// the VS_VERSIONINFO blocks in the mapped image (no version.dll call: this DLL is the game's version.dll).
const uint8_t* VersionBlob(const uint8_t* b, uint32_t* size)
{
    __try {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
        const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        if (!d.VirtualAddress) return nullptr;
        const uint8_t* root = b + d.VirtualAddress;
        auto entries = [](const uint8_t* dir) { return reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY_ENTRY*>(dir + sizeof(IMAGE_RESOURCE_DIRECTORY)); };
        auto count = [](const uint8_t* dir) {
            const auto* r = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY*>(dir);
            return static_cast<int>(r->NumberOfNamedEntries) + r->NumberOfIdEntries;
        };
        const uint8_t* level = nullptr;
        for (int i = 0; i < count(root) && i < 64; ++i)
            if (!entries(root)[i].NameIsString && entries(root)[i].Id == 16 && entries(root)[i].DataIsDirectory)
                level = root + entries(root)[i].OffsetToDirectory;
        if (!level || !count(level) || !entries(level)[0].DataIsDirectory) return nullptr;
        level = root + entries(level)[0].OffsetToDirectory;
        if (!count(level) || entries(level)[0].DataIsDirectory) return nullptr;
        const auto* de = reinterpret_cast<const IMAGE_RESOURCE_DATA_ENTRY*>(root + entries(level)[0].OffsetToData);
        *size = de->Size;
        return b + de->OffsetToData;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

inline const uint8_t* Align4(const uint8_t* p, const uint8_t* base) { return base + ((p - base + 3) & ~static_cast<ptrdiff_t>(3)); }

void Narrow(const wchar_t* w, size_t maxChars, char* out, size_t cap)
{
    size_t n = 0;
    for (; n < maxChars && w[n] && n < cap - 1; ++n) out[n] = w[n] >= 32 && w[n] < 127 ? static_cast<char>(w[n]) : '?';
    out[n] = 0;
}

struct VerStrings { char product[64], desc[80], fileVer[40]; };

void VersionStrings(const uint8_t* b, VerStrings* v)
{
    v->product[0] = v->desc[0] = v->fileVer[0] = 0;
    uint32_t size = 0;
    const uint8_t* blob = VersionBlob(b, &size);
    if (!blob || size < 64) return;
    __try {
        const uint8_t* const end = blob + size;
        // the root block: header (len, value len, type), key "VS_VERSION_INFO", the fixed info, then the children
        auto keyOf = [](const uint8_t* blk) { return reinterpret_cast<const wchar_t*>(blk + 6); };
        auto childrenOf = [&](const uint8_t* blk) {
            const wchar_t* key = keyOf(blk);
            size_t kl = 0;
            while (key[kl] && kl < 64) ++kl;
            const uint8_t* val = Align4(reinterpret_cast<const uint8_t*>(key + kl + 1), blob);
            const uint16_t vlen = *reinterpret_cast<const uint16_t*>(blk + 2), type = *reinterpret_cast<const uint16_t*>(blk + 4);
            return Align4(val + (type == 1 ? vlen * 2u : vlen), blob);
        };
        const uint16_t rootLen = *reinterpret_cast<const uint16_t*>(blob);
        const uint8_t* rootEnd = blob + (rootLen < size ? rootLen : size);
        for (const uint8_t* c = childrenOf(blob); c + 6 < rootEnd && c < end;) {
            const uint16_t clen = *reinterpret_cast<const uint16_t*>(c);
            if (!clen) break;
            if (wcsncmp(keyOf(c), L"StringFileInfo", 14) == 0) {
                const uint8_t* table = childrenOf(c);  // the first StringTable
                const uint16_t tlen = *reinterpret_cast<const uint16_t*>(table);
                const uint8_t* tend = table + tlen;
                for (const uint8_t* s = childrenOf(table); s + 6 < tend && s < end;) {
                    const uint16_t slen = *reinterpret_cast<const uint16_t*>(s);
                    if (!slen) break;
                    const wchar_t* key = keyOf(s);
                    size_t kl = 0;
                    while (key[kl] && kl < 64) ++kl;
                    const wchar_t* val = reinterpret_cast<const wchar_t*>(Align4(reinterpret_cast<const uint8_t*>(key + kl + 1), blob));
                    const size_t vchars = *reinterpret_cast<const uint16_t*>(s + 2);
                    if (wcscmp(key, L"ProductName") == 0) Narrow(val, vchars, v->product, sizeof v->product);
                    else if (wcscmp(key, L"FileDescription") == 0) Narrow(val, vchars, v->desc, sizeof v->desc);
                    else if (wcscmp(key, L"FileVersion") == 0) Narrow(val, vchars, v->fileVer, sizeof v->fileVer);
                    s = Align4(s + slen, blob);
                }
                return;
            }
            c = Align4(c + clen, blob);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Where a module was loaded from, without the Windows user name: "game" (relative to the game's folder), "System32",
// "SysWOW64", "driver store", "WinSxS", "Windows", "Program Files" (relative to it), or "other" (the path with the
// volume dropped and the profile folder's name blanked)
wchar_t g_gameRoot[MAX_PATH] = L"";  // the exe's mapped path up to "\OblivionRemastered\Binaries\", device form
size_t g_gameRootLen = 0;

void PathClass(const uint8_t* b, char* cls, size_t clsCap, char* rel, size_t relCap)
{
    wchar_t path[MAX_PATH] = L"";
    K32GetMappedFileNameW(GetCurrentProcess(), const_cast<uint8_t*>(b), path, MAX_PATH);
    wchar_t low[MAX_PATH];
    size_t n = 0;
    for (; path[n] && n < MAX_PATH - 1; ++n) low[n] = path[n] >= L'A' && path[n] <= L'Z' ? path[n] | 0x20 : path[n];
    low[n] = 0;
    const wchar_t* after = nullptr;
    const char* c = "other";
    if (g_gameRootLen && _wcsnicmp(path, g_gameRoot, g_gameRootLen) == 0) { c = "game"; after = path + g_gameRootLen; }
    else if (const wchar_t* p = wcsstr(low, L"\\windows\\system32\\driverstore\\")) { c = "driver store"; after = path + (p - low) + 30; }
    else if (const wchar_t* q = wcsstr(low, L"\\windows\\system32\\")) { c = "System32"; after = path + (q - low) + 18; }
    else if (const wchar_t* r = wcsstr(low, L"\\windows\\syswow64\\")) { c = "SysWOW64"; after = path + (r - low) + 18; }
    else if (const wchar_t* s = wcsstr(low, L"\\windows\\winsxs\\")) { c = "WinSxS"; after = path + (s - low) + 16; }
    else if (const wchar_t* t = wcsstr(low, L"\\windows\\")) { c = "Windows"; after = path + (t - low) + 9; }
    else if (const wchar_t* u = wcsstr(low, L"\\program files")) {
        c = "Program Files";
        const wchar_t* slash = wcschr(path + (u - low) + 1, L'\\');
        after = slash ? slash + 1 : path + (u - low);
    }
    snprintf(cls, clsCap, "%s", c);
    if (!after) {  // other: drop "\Device\HarddiskVolumeN", blank the profile folder's name
        const wchar_t* p = path;
        if (_wcsnicmp(p, L"\\Device\\", 8) == 0) { p = wcschr(p + 8, L'\\'); p = p ? p : path; }
        char tmp[MAX_PATH];
        Narrow(p, MAX_PATH, tmp, sizeof tmp);
        char* users = nullptr;
        for (char* x = tmp; *x && !users; ++x)
            if (_strnicmp(x, "\\users\\", 7) == 0) users = x;
        if (users) {
            char* nameEnd = strchr(users + 7, '\\');
            snprintf(rel, relCap, "%.*s\\<user>%s", static_cast<int>(users + 6 - tmp), tmp, nameEnd ? nameEnd : "");
        } else {
            snprintf(rel, relCap, "%s", tmp);
        }
        return;
    }
    Narrow(after, MAX_PATH, rel, relCap);
}

struct ModInfo {
    const uint8_t* base;
    uint32_t size, stamp, ms, ls;
    bool ver, self, exe;
    char name[48], cls[16], rel[200];
    VerStrings vs;
};

// Calls fn for every image module (VirtualQuery walk: no loader lock); stops after max
template <class Fn> void ForEachModule(int max, Fn&& fn)
{
    uint64_t a = 0x10000, lastBase = 0;
    int n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (n < max && VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof mbi)) {
        const uint64_t next = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= a) break;
        const uint64_t base = reinterpret_cast<uint64_t>(mbi.AllocationBase);
        if (mbi.Type == MEM_IMAGE && base && base != lastBase) {
            lastBase = base;
            ModInfo mi;
            mi = ModInfo{};
            mi.base = reinterpret_cast<const uint8_t*>(base);
            bool pe = false;
            __try {
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(mi.base + reinterpret_cast<const IMAGE_DOS_HEADER*>(mi.base)->e_lfanew);
                pe = nt->Signature == IMAGE_NT_SIGNATURE;
                mi.stamp = nt->FileHeader.TimeDateStamp;
                mi.size = nt->OptionalHeader.SizeOfImage;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                pe = false;
            }
            if (pe) {
                mi.ver = FileVersion(mi.base, &mi.ms, &mi.ls);
                VersionStrings(mi.base, &mi.vs);
                PathClass(mi.base, mi.cls, sizeof mi.cls, mi.rel, sizeof mi.rel);
                const char* nm = mi.rel;
                for (const char* p = mi.rel; *p; ++p)
                    if (*p == '\\') nm = p + 1;
                snprintf(mi.name, sizeof mi.name, "%s", nm);
                mi.self = mi.base == g_selfBase;
                mi.exe = mi.base == Game().base;
                fn(mi);
                ++n;
            }
        }
        a = next;
    }
}

void ModuleLine(const ModInfo& m, char* line, size_t cap)
{
    snprintf(line, cap, "%s%s [%s: %s] 0x%llX size 0x%X stamp 0x%08X version %u.%u.%u.%u | %s | %s | %s", m.name,
             m.exe ? " (exe)" : m.self ? " (OPF)" : "", m.cls, m.rel, static_cast<unsigned long long>(reinterpret_cast<uint64_t>(m.base)),
             m.size, m.stamp, m.ver ? m.ms >> 16 : 0, m.ver ? m.ms & 0xFFFF : 0, m.ver ? m.ls >> 16 : 0, m.ver ? m.ls & 0xFFFF : 0,
             m.vs.product[0] ? m.vs.product : "-", m.vs.desc[0] ? m.vs.desc : "-", m.vs.fileVer[0] ? m.vs.fileVer : "-");
}

// Every module of the process: name, where it was loaded from, base, size, PE time stamp, version and its product / file
// description strings. Says exactly which exe build (Steam or Game Pass), driver, upscaler, frame generation and other
// injected DLLs (OptiScaler, ReShade, overlays) were loaded.
void Modules()
{
    Out("modules (name [where: relative path] base size stamp version | product | description | file version string):\n");
    ForEachModule(400, [](const ModInfo& m) {
        char line[640];
        ModuleLine(m, line, sizeof line);
        Out("  %s\n", line);
    });
}

// A DLL that is not Windows' own: loaded from the game's folder, Program Files or elsewhere (OptiScaler, ReShade, Special K,
// UE4SS, OBSE64, nvngx / FidelityFX replacements, overlays), besides the exe and this DLL
bool Foreign(const ModInfo& m)
{
    return !m.self && !m.exe && strcmp(m.cls, "System32") != 0 && strcmp(m.cls, "SysWOW64") != 0 && strcmp(m.cls, "WinSxS") != 0 &&
           strcmp(m.cls, "Windows") != 0 && strcmp(m.cls, "driver store") != 0;
}

// The proxy names a mod loader or OptiScaler uses in the game's folder
bool ProxyName(const char* name)
{
    static const char* const names[] = {"version.dll", "dxgi.dll", "winmm.dll", "d3d12.dll", "d3d11.dll", "dinput8.dll", "xinput1_3.dll",
                                        "xinput1_4.dll", "dsound.dll", "winhttp.dll", "wininet.dll", "nvngx.dll", "dbghelp.dll"};
    for (const char* n : names)
        if (_stricmp(n, name) == 0) return true;
    return false;
}

// The install result of every key main.cpp installs (OPF_INSTALL): 1 installed, 0 not (off in the ini, or its code not found
// in this exe: the log's line says which), - no result
struct InstallNote { const char* name; int result; };
InstallNote g_installs[320];
std::atomic<int> g_nInstalls{0};
uint32_t g_iniCrc = 0, g_iniSize = 0;
const char* g_clockName = "none";

void Installs()
{
    const int n = g_nInstalls.load() < 320 ? g_nInstalls.load() : 320;
    Out("installs (%d; 1 installed, 0 not: off or not found, - no result):", n);
    for (int i = 0; i < n; ++i) Out("%s %s=%s", i % 8 ? "" : "\n ", g_installs[i].name, g_installs[i].result > 0 ? "1" : g_installs[i].result == 0 ? "0" : "-");
    Out("\n");
}

// ---- the summary block that starts every record: plain text a player can paste as is (Nexus takes no files); fixed markers
char g_gpuLine[256] = "GPU: not known (the game's graphics adapter had not been opened yet)";
char g_sysLine[256] = "system: not read";

void ReadSystemLine()
{
    // Windows: KUSER_SHARED_DATA (fixed address, no registry): NtBuildNumber +0x260, NtMajorVersion +0x26C, NtMinorVersion +0x270
    uint32_t build = 0, major = 0, minor = 0;
    __try {
        const auto* k = reinterpret_cast<const volatile uint8_t*>(static_cast<uintptr_t>(0x7FFE0000));
        build = *reinterpret_cast<const volatile uint32_t*>(k + 0x260) & 0xFFFF;
        major = *reinterpret_cast<const volatile uint32_t*>(k + 0x26C);
        minor = *reinterpret_cast<const volatile uint32_t*>(k + 0x270);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    int r[4] = {};
    char brand[49] = {};
    __cpuid(r, static_cast<int>(0x80000000));
    if (static_cast<unsigned>(r[0]) >= 0x80000004u) {
        for (int i = 0; i < 3; ++i) {
            __cpuid(r, static_cast<int>(0x80000002u + i));
            memcpy(brand + 16 * i, r, 16);
        }
    }
    const char* b = brand;
    while (*b == ' ') ++b;
    for (size_t e = strlen(brand); e > 0 && brand[e - 1] == ' '; --e) brand[e - 1] = 0;
    MEMORYSTATUSEX ms{sizeof ms};
    const double ramGB = GlobalMemoryStatusEx(&ms) ? static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0) : 0;
    const char* const win = major == 10 && build >= 22000 ? "11" : major == 10 ? "10" : "?";
    snprintf(g_sysLine, sizeof g_sysLine, "system: Windows %s (%u.%u build %u), CPU %s, %u logical processors, RAM %.1f GB", win, major, minor, build,
             *b ? b : "?", UsableLogicalProcessors(), ramGB);
}

// The upscaler / frame generation / overlay libraries among the modules, with their file versions
void UpscalerModules()
{
    static const char* const kinds[] = {"ffx", "fidelityfx", "fsr", "nvngx", "dlss", "sl.", "xess", "optiscaler", "reshade", "nvapi"};
    uint64_t a = 0x10000, lastBase = 0;
    int n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    Out("upscaler / frame generation:");
    while (n < 6 && VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof mbi)) {
        const uint64_t next = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= a) break;
        const uint64_t base = reinterpret_cast<uint64_t>(mbi.AllocationBase);
        if (mbi.Type == MEM_IMAGE && base && base != lastBase) {
            lastBase = base;
            const Mod* m = ModOf(base);
            char low[48] = "";
            for (size_t i = 0; m && m->name[i] && i < sizeof low - 1; ++i) low[i] = static_cast<char>(m->name[i] | (m->name[i] >= 'A' && m->name[i] <= 'Z' ? 0x20 : 0));
            bool hit = false;
            for (const char* kd : kinds) hit = hit || strstr(low, kd) != nullptr;
            uint32_t vms = 0, vls = 0;
            if (hit && FileVersion(reinterpret_cast<const uint8_t*>(base), &vms, &vls))
                Out("%s %s %u.%u.%u.%u", n ? "," : "", m->name, vms >> 16, vms & 0xFFFF, vls >> 16, vls & 0xFFFF), ++n;
            else if (hit)
                Out("%s %s", n ? "," : "", m->name), ++n;
        }
        a = next;
    }
    Out(n ? "\n" : " none loaded\n");
}

void FrameShort(uint64_t pc, char* out, size_t cap)
{
    const Mod* m = ModOf(pc);
    if (!m) { snprintf(out, cap, "x:0x%llX", static_cast<unsigned long long>(pc)); return; }
    const bool exe = m->base == reinterpret_cast<uint64_t>(Game().base), self = m->base == reinterpret_cast<uint64_t>(g_selfBase);
    snprintf(out, cap, "%s+0x%llX", exe ? "exe" : self ? "OPF" : m->name, static_cast<unsigned long long>(pc - m->base));
}

void Ring(int lastN);

// build facts for the summary (start-up): our DLL's CRC-32 and the game exe's version strings
uint32_t g_dllCrc = 0;
char g_startText[32] = "?";
char g_gameVer[64] = "?";
const char* g_testKey = nullptr;  // a lab test key that made this crash or hang (lab builds)
int g_recordNo = 0;                // records written this session (the summary says which one this is)

// The verdict line: where our code is, by the stack. A hook stub or patched site on the route names the key it belongs to;
// it is called the cause only when the fault is in it.
void Verdict(const uint64_t* pc, int depth, bool fault)
{
    const char* route = nullptr;  // the first named owner of a stub / patched site on the stack
    bool faultOurs = false, inStub = false, opfOnStack = false;
    for (int k = 0; k < depth; ++k) {
        const char* owner = nullptr;
        uint32_t by = 0;
        const int kind = TryOwnerOf(reinterpret_cast<const void*>(pc[k]), &owner, &by, nullptr, k > 0);
        const Mod* m = ModOf(pc[k]);
        const bool self = m && m->base == reinterpret_cast<uint64_t>(g_selfBase);
        if (k == 0 && (self || kind == 2)) { faultOurs = true; inStub = kind == 2; }
        if (kind == 1 || kind == 2) {
            if (!route) route = owner ? owner : "a key of ours";
            opfOnStack = true;
        }
        opfOnStack = opfOnStack || self;
    }
    const char* where = fault ? "fault" : "the game thread";
    if (faultOurs)
        Out("verdict: %s inside OblivionPerfFix code%s%s%s\n", where, inStub ? " (a hook stub of " : route ? " (reached through the hook of " : "",
            inStub || route ? (route ? route : "?") : "", inStub || route ? ")" : "");
    else if (opfOnStack)
        Out("verdict: OblivionPerfFix code on the stack (hook of %s), %s elsewhere\n", route ? route : "no named key", fault ? "the fault" : "the thread waits");
    else
        Out("verdict: no OblivionPerfFix code involved\n");
}

void Summary(const char* kind, const char* what, const char* where, const uint64_t* pc, int depth, const char* extra = nullptr,
             bool fault = true)
{
    const bool gdk = strstr(g_exeName, "WinGDK") != nullptr;
    ++g_recordNo;
    Out("---- OblivionPerfFix summary: paste this ----\n");
    Out("OblivionPerfFix " OPF_VERSION OPF_RELEASE_SUFFIX " %s build, dll crc32 %08X, pdb %s, ini crc32 %08X%s%s\n",
        "public",
        g_dllCrc, g_pdb, g_iniCrc, g_testKey ? ", TEST KEY SET: " : "", g_testKey ? g_testKey : "");
    Out("game: %s %s (%s), SizeOfImage 0x%X, TimeDateStamp 0x%08X, up %.0f s; record %d of this session%s\n", gdk ? "Xbox / Game Pass" : "Steam",
        g_gameVer, g_exeName, g_exeSize, g_exeStamp, QSec(static_cast<int64_t>(Qpc() - g_startQpc)), g_recordNo,
        g_recordNo > 1 ? " (paste the first one too)" : "");
    Out("%s\n%s\n", g_sysLine, g_gpuLine);
    UpscalerModules();
    {
        int n = 0;
        Out("other DLLs in the game (not Windows'):");
        int more = 0;
        ForEachModule(400, [&n, &more](const ModInfo& m) {
            if (!Foreign(m)) return;
            if (n >= 8) { ++more; return; }  // the summary stays within ~2 KB; the full list is below it
            Out("%s %s", n++ ? "," : "", m.name);
            if (m.vs.product[0]) Out(" (%.24s", m.vs.product);
            if (m.ver) Out("%s%u.%u.%u.%u", m.vs.product[0] ? " " : " (", m.ms >> 16, m.ms & 0xFFFF, m.ls >> 16, m.ls & 0xFFFF);
            if (m.vs.product[0] || m.ver) Out(")");
            if (strcmp(m.cls, "game") != 0) Out(" [%s]", m.cls);
        });
        if (more) Out(", and %d more (the module list below)", more);
        Out(n ? "\n" : " none\n");
    }
    Out("what: %s: %s\n", kind, what);
    if (extra) Out("%s\n", extra);
    char f[96];
    if (depth > 0) { FrameShort(pc[0], f, sizeof f); Out("at: %s, %s\n", f, where); }
    Out("stack:");
    for (int k = 0; k < depth && k < 6; ++k) { FrameShort(pc[k], f, sizeof f); Out("%s %s", k ? " <" : "", f); }
    Out(depth ? "\n" : " not read\n");
    Verdict(pc, depth, fault);
    Out("last events (seconds before, game frame):\n");
    Ring(5);
    Out("---- end of summary ----\n");
}

void Header(const char* record)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    Out("# OblivionPerfFix crash report v1\n");
    Out("record: %s\n", record);
    Out("version: OblivionPerfFix " OPF_VERSION OPF_RELEASE_SUFFIX "\n");
    Out("exe: %s, TimeDateStamp 0x%08X, SizeOfImage 0x%X\n", g_exeName, g_exeStamp, g_exeSize);
    Out("dll pdb: %s\n", g_pdb);
    Out("ini at start-up: crc32 %08X, %u bytes\n", g_iniCrc, g_iniSize);
    Out("hang clock: %s\n", g_clockName);
    Out("game version: %s\n", g_gameVer);
    Out("process: pid %lu, started %s, up %.1f s\n", GetCurrentProcessId(), g_startText, QSec(static_cast<int64_t>(Qpc() - g_startQpc)));
    Out("local time: %04u-%02u-%02u %02u:%02u:%02u\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    Out("legend: exe+ / OPF+ / name.dll+ = offset in the game exe / OblivionPerfFix / that module; [tNNN] = thread id; fNNN = game frame number; stub:Key = a hook of that ini key; [patched by Key] = game code that key changed\n");
}

// "exe+0x1234", "OPF+0x99", "name.dll+0x10", "stub:Key+0x40", "x:0x..."; owner tag appended. detail 1: the function's start
// and size (+ its first 16 bytes in the exe); 2: also 32 code bytes around the address ("16 before | 16 from it"), so a frame
// of an exe we do not have (Game Pass) can be found in the Steam exe by pattern
void NoteExeFn(uint64_t begin, uint32_t size);

// ret: a return address (frames after the first): the end of a patched range counts as inside it
void Frame(uint64_t pc, int detail, const char** viaOwner, bool ret = true)
{
    const bool fnLine = detail > 0;
    const char* owner = nullptr;
    uint32_t by = 0;
    const void* begin = nullptr;
    const int kind = TryOwnerOf(reinterpret_cast<const void*>(pc), &owner, &by, &begin, ret);
    const Mod* m = ModOf(pc);
    char who[96] = "";
    if (kind == 1 || kind == 2) {
        if (owner) snprintf(who, sizeof who, "%s", owner);
        else if (by) snprintf(who, sizeof who, "installer OPF+0x%X", by);
        else snprintf(who, sizeof who, "a key of ours");
        if (viaOwner) *viaOwner = owner ? owner : "unnamed";
    }
    if (kind == 2 && !m) {
        Out(" stub:%s+0x%llX", who, static_cast<unsigned long long>(pc - reinterpret_cast<uint64_t>(begin)));
        return;
    }
    if (!m) {
        Out(" x:0x%llX", static_cast<unsigned long long>(pc));
        return;
    }
    const bool self = m->base == reinterpret_cast<uint64_t>(g_selfBase);
    const bool exe = m->base == reinterpret_cast<uint64_t>(Game().base);
    if (exe) {  // every exe frame: the deduplicated "exe functions" table of the record
        DWORD64 im = 0;
        if (PRUNTIME_FUNCTION f2 = RtlLookupFunctionEntry(pc, &im, nullptr)) NoteExeFn(im + f2->BeginAddress, f2->EndAddress - f2->BeginAddress);
    }
    Out(" %s+0x%llX", exe ? "exe" : self ? "OPF" : m->name, static_cast<unsigned long long>(pc - m->base));
    if (kind == 1) Out(" [patched by %s]", who);
    if (fnLine && (exe || self)) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(pc, &image, nullptr);
        if (fe) {
            Out(" (fn %s+0x%X size 0x%X", exe ? "exe" : "OPF", fe->BeginAddress, fe->EndAddress - fe->BeginAddress);
            if (exe) {
                Out(" sig ");
                __try {
                    const uint8_t* code = reinterpret_cast<const uint8_t*>(image + fe->BeginAddress);
                    for (int i = 0; i < 16; ++i) Out("%02X", code[i]);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Out("?");
                }
            }
            Out(")");
        }
        if (detail > 1) {
            Out(" code ");
            __try {
                const uint8_t* code = reinterpret_cast<const uint8_t*>(pc);
                for (int i = -16; i < 16; ++i) Out(i ? "%02X" : "|%02X", code[i]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Out("?");
            }
        }
    }
}

// One stack: the frames, then "our code on this stack"
void Stack(const uint64_t* pc, int depth, int fnLines, bool oneLine)
{
    const char* owners[8] = {};
    int nOwners = 0;
    bool selfSeen = false;
    for (int k = 0; k < depth; ++k) {
        if (!oneLine) Out("  %2d", k);
        const char* via = nullptr;
        Frame(pc[k], k < fnLines ? 2 : 0, &via, k > 0);
        const Mod* m = ModOf(pc[k]);
        if (m && m->base == reinterpret_cast<uint64_t>(g_selfBase)) selfSeen = true;
        if (via) {
            bool known = false;
            for (int i = 0; i < nOwners; ++i) known = known || strcmp(owners[i], via) == 0;
            if (!known && nOwners < 8) owners[nOwners++] = via;
        }
        if (!oneLine) Out("\n");
    }
    if (oneLine) Out(" |");
    Out(oneLine ? " ours:" : "  our code on this stack:");
    if (!nOwners && !selfSeen) Out(" none");
    for (int i = 0; i < nOwners; ++i) Out("%s %s", i ? "," : "", owners[i]);
    if (selfSeen && !nOwners) Out(" OblivionPerfFix (see the OPF frames)");
    Out("\n");
}

// The event ring, oldest first, times relative to now (lastN: only the newest so many, no heading)
void Ring(int lastN = 0)
{
    const uint64_t head = g_ringHead.load(std::memory_order_acquire), now = Qpc();
    const uint64_t keep = lastN > 0 ? static_cast<uint64_t>(lastN) : static_cast<uint64_t>(kRing);
    const uint64_t from = head > keep ? head - keep : 0;
    if (!lastN)
        Out("events (the last %llu of %llu; seconds before this record, game frame):\n", static_cast<unsigned long long>(head - from),
            static_cast<unsigned long long>(head));
    for (uint64_t i = from; i < head; ++i) {
        RingEntry& e = g_ring[i % kRing];
        const uint64_t s1 = e.seq.load(std::memory_order_acquire);
        RingEntry c;
        c.qpc = e.qpc;
        c.kind = e.kind;
        c.tid = e.tid;
        c.a = e.a;
        c.b = e.b;
        c.c = e.c;
        memcpy(c.text, e.text, sizeof c.text);
        c.frame = e.frame;
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t s2 = e.seq.load(std::memory_order_relaxed);
        if (s1 != i + 1 || s2 != s1) continue;  // being written, or already overwritten
        c.text[11] = 0;
        Out("  %9.3f f%-8u ", c.qpc <= now ? -QSec(static_cast<int64_t>(now - c.qpc)) : 0.0, c.frame);
        switch (c.kind) {
        case kEvAlive: Out("alive: player at (%lld, %lld, %lld)", static_cast<long long>(c.a), static_cast<long long>(c.b),
                           static_cast<long long>(c.c)); break;
        case kEvTravel: Out(c.a ? "travel begins (door, gate or fast travel)" : "travel ends"); break;
        case kEvCell: if (c.a) Out("interior cell %08llX", static_cast<unsigned long long>(c.a)); else Out("outside (no interior cell)"); break;
        case kEvWorld: Out("worldspace %08llX", static_cast<unsigned long long>(c.a)); break;
        case kEvFreeze: Out("game freeze (menus, loading screens): %lld named, mode %lld", static_cast<long long>(c.a),
                            static_cast<long long>(c.b)); break;
        case kEvException: {
            const Site* site = SiteOf(c.b);
            const uint32_t handled = site ? site->handled.load(std::memory_order_relaxed) : 0;
            Out("%s %s (0x%08llX) at", handled ? "handled fault" : "fault", CodeName(static_cast<DWORD>(c.a)), static_cast<unsigned long long>(c.a));
            Frame(c.b, false, nullptr, false);
            Out(", time %llu at this place%s", static_cast<unsigned long long>(c.c), handled ? " (this place: handled before)" : "");
            break;
        }
        case kEvKey: Out("%s %lld %lld %lld", c.text, static_cast<long long>(c.a), static_cast<long long>(c.b), static_cast<long long>(c.c)); break;
        case kEvFocus: Out(c.a ? "focus lost" : "focus back"); break;
        case kEvHang: if (c.a) Out("hang record stage %llu (%llu s open)", static_cast<unsigned long long>(c.a), static_cast<unsigned long long>(c.b));
                      else Out("the open frame ended after %llu s", static_cast<unsigned long long>(c.b)); break;
        case kEvNote: Out("%s", c.text); break;
        case kEvGpu:
            if (c.a == 1)
                Out("GPU fault drill (lab test): buffer at VA 0x%llX freed, the GPU told to write to VA 0x%llX", static_cast<unsigned long long>(c.b),
                    static_cast<unsigned long long>(c.c));
            else if (c.c)
                Out("GPU crash report: device removed 0x%08llX, page fault at VA 0x%llX", static_cast<unsigned long long>(c.b),
                    static_cast<unsigned long long>(c.c));
            else
                Out("GPU crash report: device removed 0x%08llX, no page fault", static_cast<unsigned long long>(c.b));
            break;
        default: Out("kind %u: %lld %lld %lld %s", c.kind, static_cast<long long>(c.a), static_cast<long long>(c.b),
                     static_cast<long long>(c.c), c.text); break;
        }
        Out(" [t%lu]\n", static_cast<unsigned long>(c.tid));
    }
}

// ---------------------------------------------------------------------------------------------------------------- threads
uint8_t* g_copy = nullptr;  // kCopy, VirtualAlloc'ed at install (touched: no page fault while a thread is stopped)
struct Row {
    DWORD tid;
    int priority, depth;
    uint64_t cycles;
    char name[48];
    uint64_t pc[kDepth];
};
Row* g_rows = nullptr;  // kMaxThreads, VirtualAlloc'ed at the first hang record

// The stack copy buffer is shared by the watchdog and a writer on an ending thread: taken with a 200 ms try (the exit path
// after ExitProcess, alone in the process, takes it as it is).
std::atomic<bool> g_copyBusy{false};
struct CopyLock {
    bool owned = false;
    CopyLock()
    {
        if (g_outNoLock) { owned = true; return; }
        const ULONGLONG until = GetTickCount64() + 200;
        while (!(owned = !g_copyBusy.exchange(true, std::memory_order_acquire)) && GetTickCount64() < until) Sleep(1);
    }
    ~CopyLock() { if (owned && !g_outNoLock) g_copyBusy.store(false, std::memory_order_release); }
    CopyLock(const CopyLock&) = delete;
    CopyLock& operator=(const CopyLock&) = delete;
};

bool TakeStackLocked(DWORD tid, uint64_t* pc, uint64_t* sp, int max, int* depth, int* priority, uint64_t* cycles);

// One thread, stopped only for GetThreadContext and the stack copy; walked after it runs again. False: could not be read.
bool TakeStack(DWORD tid, uint64_t* pc, uint64_t* sp, int max, int* depth, int* priority, uint64_t* cycles)
{
    *depth = 0;
    if (tid == GetCurrentThreadId() || !g_copy) return false;
    CopyLock lock;
    if (!lock.owned) return true;  // read later (busy): "not readable"
    return TakeStackLocked(tid, pc, sp, max, depth, priority, cycles);
}

bool TakeStackLocked(DWORD tid, uint64_t* pc, uint64_t* sp, int max, int* depth, int* priority, uint64_t* cycles)
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return false;
    struct ThreadBasicInfo { LONG exitStatus; PVOID teb; ULONG_PTR pid, tid; ULONG_PTR affinity; LONG priority, basePriority; } bi{};
    DWORD exitCode = 0;
    if (!g_qit || g_qit(h, 0, &bi, sizeof bi, nullptr) < 0 || !bi.teb || bi.pid != GetCurrentProcessId() || !GetExitCodeThread(h, &exitCode) ||
        exitCode != STILL_ACTIVE) {  // pid: a thread id reused by another process since the snapshot
        CloseHandle(h);
        return false;
    }
    if (priority) *priority = GetThreadPriority(h);
    if (cycles) { ULONG64 c = 0; *cycles = QueryThreadCycleTime(h, &c) ? c : 0; }
    const uint8_t* const teb = static_cast<const uint8_t*>(bi.teb);
    alignas(16) CONTEXT c;
    size_t bytes = 0;
    const int prio = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);  // not preempted while a thread is stopped
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(h, &c)) {
            __try {
                const uint64_t base = *reinterpret_cast<const uint64_t*>(teb + 8), limit = *reinterpret_cast<const uint64_t*>(teb + 0x10);
                if (c.Rsp >= limit && c.Rsp < base) {  // [rsp, base) is committed: no guard page is touched
                    bytes = base - c.Rsp < kCopy ? static_cast<size_t>(base - c.Rsp) : kCopy;
                    memcpy(g_copy, reinterpret_cast<const void*>(c.Rsp), bytes);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                bytes = 0;
            }
        }
        ResumeThread(h);
    }
    SetThreadPriority(GetCurrentThread(), prio);
    CloseHandle(h);
    if (!bytes) return true;
    *depth = WalkCopy(&c, g_copy, bytes, pc, sp, max);
    return true;
}

void OneThreadStack(const char* label, DWORD tid, int fnLines)
{
    uint64_t pc[kFrames];
    int depth = 0, prio = 0;
    uint64_t cycles = 0;
    char name[48];
    ThreadName(tid, name, sizeof name);
    const bool ok = TakeStack(tid, pc, nullptr, kFrames, &depth, &prio, &cycles);
    Out("%s: thread %lu%s%s%s, now:%s\n", label, tid, name[0] ? " " : "", name, ThreadLabel(tid), ok ? (depth ? "" : " (stack not readable)") : " (gone or not readable)");
    if (depth) Stack(pc, depth, fnLines, false);
}

// The render thread: the first thread named "RenderThread" (UE names its threads)
DWORD FindNamedThread(const char* prefix)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    DWORD found = 0;
    for (BOOL more = Thread32First(snap, &te); more && !found; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        char name[48];
        ThreadName(te.th32ThreadID, name, sizeof name);
        if (strncmp(name, prefix, strlen(prefix)) == 0) found = te.th32ThreadID;
    }
    CloseHandle(snap);
    return found;
}

void PendingSlots(const Slot* except)
{
    const uint64_t now = Qpc();
    int n = 0;
    for (const Slot& s : g_slots) {
        const int st = s.state.load(std::memory_order_acquire);
        if (&s == except || (st != kPending && st != kTaken)) continue;
        if (!n++) Out("other fatal-type exceptions not yet judged (pending):\n");
        Out("  %s (0x%08X) at", CodeName(s.code), s.code);
        Frame(s.addr, false, nullptr, false);
        Out(", thread %lu, %.1f s ago\n", s.tid, QSec(static_cast<int64_t>(now - s.qpc)));
    }
}

void Tail()
{
    Ring();
    Out("memory and frames (the monitor's last 60 s):\n");
    LogSetSink(&SinkLine);
    GpuReportContextLines();
    GpuReportHistoryLines();
    Out("ini keys in effect:\n");
    GpuReportKeyLines();
    LogSetSink(nullptr);
}

// ---------------------------------------------------------------------------------------------------------------- crash record
// While a record is written on a thread, its own guarded reads that fault are not slots (the handler's flag)
struct OwnThreadFlag {
    int was;
    OwnThreadFlag() : was(t_own) { t_own = 1; }
    ~OwnThreadFlag() { t_own = was; }
    OwnThreadFlag(const OwnThreadFlag&) = delete;
    OwnThreadFlag& operator=(const OwnThreadFlag&) = delete;
};

// A copy of text with the Windows user name blanked: "\Users\<name>\" and "/Users/<name>/" (UE's fatal-error messages hold
// paths), "<user>" in its place
void BlankUser(const char* in, char* out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 8 < cap;) {
        if ((in[i] == '\\' || in[i] == '/') && _strnicmp(in + i + 1, "users", 5) == 0 && (in[i + 6] == '\\' || in[i + 6] == '/')) {
            const char sep = in[i + 6];
            memcpy(out + o, in + i, 7);
            o += 7;
            i += 7;
            size_t e = i;
            while (in[e] && in[e] != '\\' && in[e] != '/' && in[e] != ' ' && in[e] != '"' && in[e] != '\'') ++e;
            if (e > i && o + 7 < cap) { memcpy(out + o, "<user>", 6); o += 6; i = e; }
            (void)sep;
            continue;
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

const char* const kRegNames[kRegs] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};

void Registers(const Slot& s)
{
    Out("registers: rip 0x%llX", static_cast<unsigned long long>(s.rip));
    for (int i = 0; i < kRegs; ++i) Out("%s %s 0x%llX", i % 6 == 5 ? "\n          " : "", kRegNames[i], static_cast<unsigned long long>(s.gpr[i]));
    Out("\n");
    for (int i = 0; i < kRegs; ++i) {
        if (!(s.regMemOk & (1u << i))) continue;
        Out("  [%s] =", kRegNames[i]);
        for (int b = 0; b < 64; ++b) Out("%s%02X", b % 8 ? "" : " ", s.regMem[i][b]);
        uint64_t first;
        memcpy(&first, s.regMem[i], 8);
        const Mod* m = ModOf(first);  // a vtable pointer names the class's module
        if (m) {
            char f[96];
            FrameShort(first, f, sizeof f);
            Out("  (first word -> %s)", f);
        }
        Out("\n");
    }
}

// The modules unloaded so far (Windows keeps the last 64): an "x:0x..." frame may be inside one of them
void Unloaded()
{
    using TraceFn = void(NTAPI*)(PULONG*, PULONG*, PVOID*);
    static const auto fn = reinterpret_cast<TraceFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetUnloadEventTraceEx"));
    if (!fn) return;
    struct Trace { PVOID base; SIZE_T size; ULONG seq, stamp, sum; WCHAR name[32]; ULONG version[2]; };
    PULONG elemSize = nullptr, elemCount = nullptr;
    PVOID trace = nullptr;
    __try {
        fn(&elemSize, &elemCount, &trace);
        if (!elemSize || !elemCount || !trace || *elemSize < sizeof(Trace)) return;
        const auto* t = *reinterpret_cast<const uint8_t* const*>(trace);
        int n = 0;
        for (ULONG i = 0; t && i < *elemCount && i < 64; ++i) {
            const auto* e = reinterpret_cast<const Trace*>(t + static_cast<size_t>(i) * *elemSize);
            if (!e->base) continue;
            char nm[40];
            Narrow(e->name, 32, nm, sizeof nm);
            if (!n++) Out("unloaded modules (base, size, stamp, name):");
            Out("%s 0x%llX 0x%llX 0x%08lX %s", n % 3 == 1 ? "\n " : ";", static_cast<unsigned long long>(reinterpret_cast<uint64_t>(e->base)),
                static_cast<unsigned long long>(e->size), e->stamp, nm);
        }
        if (n) Out("\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The exe's functions seen on any stack of this record (deduplicated), with their size and first 32 bytes: a frame from a
// Game Pass exe (other RVAs, no copy here) is found in the Steam exe by these bytes
uint64_t g_fnSeen[160];
uint32_t g_fnSize[160];
int g_nFnSeen = 0;
void NoteExeFn(uint64_t begin, uint32_t size)
{
    for (int i = 0; i < g_nFnSeen; ++i)
        if (g_fnSeen[i] == begin) return;
    if (g_nFnSeen < 160) { g_fnSeen[g_nFnSeen] = begin; g_fnSize[g_nFnSeen++] = size; }
}
void ExeFunctions()
{
    if (!g_nFnSeen) return;
    Out("exe functions on these stacks (%d; RVA, size, first 32 bytes):\n", g_nFnSeen);
    const uint64_t base = reinterpret_cast<uint64_t>(Game().base);
    for (int i = 0; i < g_nFnSeen; ++i) {
        Out("  exe+0x%llX 0x%X ", static_cast<unsigned long long>(g_fnSeen[i] - base), g_fnSize[i]);
        __try {
            const uint8_t* c = reinterpret_cast<const uint8_t*>(g_fnSeen[i]);
            for (int b = 0; b < 32; ++b) Out("%02X", c[b]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Out("?");
        }
        Out("\n");
    }
    g_nFnSeen = 0;
}

void WriteCrashLocked(Slot& s, const char* why, UINT exitCode, bool atExit);

void WriteCrash(Slot& s, const char* why, UINT exitCode, bool atExit)
{
    OwnThreadFlag own;
    if (!LockWriter()) return;
    WriteCrashLocked(s, why, exitCode, atExit);
}


// A GPU crash (0x8000): memory_monitor's report runs in its own handler on the thread that raised it, right after ours; a
// record written from another thread (the late check, the exit) waits up to 3 s for it to finish
void WaitGpuReport()
{
    size_t len = 0;
    DWORD tid = 0;
    int state = 0;
    for (int i = 0; i < 300; ++i) {
        GpuCrashReportText(&len, &tid, &state);
        if (state != 1 || tid == GetCurrentThreadId()) return;
        Sleep(10);
    }
}

// The summary's GPU lines: the removed reason, the page fault, what was at its VA, where the GPU's work stopped (DRED)
void GpuSummary(char* out, size_t cap)
{
    WaitGpuReport();
    const GpuCrashFacts& f = GpuCrashFactsNow();
    size_t n = 0;
    auto add = [&](const char* fmt, auto... args) {
        if (n >= cap - 2) return;
        const int k = snprintf(out + n, cap - n, fmt, args...);
        if (k > 0) n += static_cast<size_t>(k) < cap - n ? static_cast<size_t>(k) : cap - n - 1;
    };
    out[0] = 0;
    if (!f.valid) {
        add("gpu: no device data (the game's D3D12 device was not found, or GpuCrashReport=0)");
        return;
    }
    add("gpu: device removed 0x%08X %.60s%s%s", f.reason, f.reasonName, f.state[0] ? "; DRED: " : "; no DRED", f.state);
    if (f.pageFault) {
        add("\ngpu page fault at VA 0x%llX", static_cast<unsigned long long>(f.va));
        add("\ngpu memory there (GpuMemoryRecord): %.330s", f.owner[0] ? f.owner : "not known");
        add("\ngpu DRED's own lists there: alive %.100s; recently freed %.100s", f.dredThere, f.dredFreed);
    } else if (f.state[0]) add(", no page fault");
    if (f.passKind == 1)
        add("\ngpu work (DRED): %u of %u command lists stopped part-way; first: %.80s, %.150s; next: %.120s", f.partWay, f.lists, f.passQueue,
            f.passDone, f.passNext);
    else if (f.passKind == 2)
        add("\ngpu work (DRED): no list stopped part-way, %u never started; on %.60s the last finished is %.150s, the first not started %.120s",
            f.notStarted, f.passQueue, f.passDone, f.passNext);
    else if (f.lists)
        add("\ngpu work (DRED): %u command lists, %u stopped part-way, %u never started", f.lists, f.partWay, f.notStarted);
}

// The full GPU crash report (the same lines as the log's) in the record
void GpuReportPart()
{
    WaitGpuReport();
    size_t len = 0;
    DWORD tid = 0;
    int state = 0;
    const char* text = GpuCrashReportText(&len, &tid, &state);
    if (state == 2 && len) {
        Out("GPU crash report (the same lines as in OblivionPerfFix.log):\n");
        OutRaw(text, len);
    } else
        Out("GPU crash report: %s\n", state == 1 ? "still being written by its thread (see OblivionPerfFix.log)"
                                                  : "not written (GpuCrashReport=0, or the game raised no GPU crash report)");
}

void WriteCrashLocked(Slot& s, const char* why, UINT exitCode, bool atExit)
{
    const uint64_t now = Qpc();
    g_nFnSeen = 0;
    char name[48];
    ThreadName(s.tid, name, sizeof name);
    char msg[340];
    BlankUser(s.msg, msg, sizeof msg);
    const int shown = s.depth < kFrames ? s.depth : kFrames;
    {
        char what[420];
        int n = snprintf(what, sizeof what, "%s (0x%08X)", CodeName(s.code), s.code);
        if ((s.code == 0xC0000005 || s.code == 0xC0000006) && s.nParams >= 2 && n > 0)
            n += snprintf(what + n, sizeof what - n, " %s address 0x%llX", s.params[0] == 0 ? "reading" : s.params[0] == 1 ? "writing" :
                          s.params[0] == 8 ? "executing" : "accessing", static_cast<unsigned long long>(s.params[1]));
        if (msg[0] && n > 0) snprintf(what + n, sizeof what - n, ": %.200s", msg);
        char where[96];
        snprintf(where, sizeof where, "thread %lu%s%s%s", s.tid, name[0] ? " " : "", name, ThreadLabel(s.tid));
        uint64_t top[kFrames];
        int depth = shown;
        for (int k = 0; k < depth; ++k) top[k] = s.pc[k];
        if (!depth) { top[0] = s.addr; depth = 1; }
        char gpu[1500];
        if (s.code == kUeGpu) GpuSummary(gpu, sizeof gpu);
        Summary(s.code == kUeGpu ? "GPU crash" : s.code == kUeAssert ? "fatal error" : "crash", what, where, top, depth,
                s.code == kUeGpu ? gpu : nullptr);
        OutFlush();
        KeepFile();  // from here the file stays even if the process ends in the middle of the rest
    }
    Header("crash");
    Out("why written: %s", why);
    if (exitCode) Out(" (exit code %u)", exitCode);
    Out("\n");
    Out("exception: %s (0x%08X) at", CodeName(s.code), s.code);
    Frame(s.addr, false, nullptr, false);
    if ((s.code == 0xC0000005 || s.code == 0xC0000006) && s.nParams >= 2)
        Out(", %s address 0x%llX", s.params[0] == 0 ? "reading" : s.params[0] == 1 ? "writing" : s.params[0] == 8 ? "executing" : "accessing",
            static_cast<unsigned long long>(s.params[1]));
    if (s.code == 0xC0000006 && s.nParams >= 3) Out(", status 0x%08llX", static_cast<unsigned long long>(s.params[2]));
    Out(", thread %lu%s%s%s, %.1f s before this record\n", s.tid, name[0] ? " " : "", name, ThreadLabel(s.tid), QSec(static_cast<int64_t>(now - s.qpc)));
    if (msg[0]) Out("message: %s\n", msg);
    Registers(s);
    Out("stack of the crashing thread (%d frames%s):\n", s.depth,
        s.code == kStackOverflow ? "; a stack overflow: not walked" : s.depth ? "" : "; not walked (little stack left)");
    if (shown) Stack(s.pc, shown, kFrames, false);
    else Out("  our code on this stack: not known (no walk)\n");
    if (s.code == kUeGpu) GpuReportPart();
    if (s.nScan) {
        Out("words on the stack after the last frame that point into code (a raw scan; the unwinder stopped there):");
        for (int k = 0; k < s.nScan; ++k) {
            char f[96];
            FrameShort(s.scan[k], f, sizeof f);
            Out("%s %s", k ? "," : "", f);
        }
        Out("\n");
    }
    for (const ThrowNote& t : g_throws)
        if (t.tid.load(std::memory_order_acquire) == s.tid) {
            Out("last C++ exception on this thread: at");
            Frame(t.ip, false, nullptr, false);
            Out(", %.1f s before this record\n", QSec(static_cast<int64_t>(now - t.qpc)));
        }
    PendingSlots(&s);
    const DWORD gt = g_gtTid.load(std::memory_order_relaxed) ? g_gtTid.load(std::memory_order_relaxed) : g_mainTid;
    if (gt && gt != s.tid) OneThreadStack("game thread", gt, 12);
    if (const DWORD rt = FindNamedThread("RenderThread"); rt && rt != s.tid) OneThreadStack("render thread", rt, 12);
    if (const DWORD rhi = FindNamedThread("RHIThread"); rhi && rhi != s.tid) OneThreadStack("RHI thread", rhi, 12);
    ExeFunctions();
    Modules();
    Unloaded();
    Installs();
    Tail();
    Out("# end\n\n");
    OutFlush();
    if (g_file != INVALID_HANDLE_VALUE && !atExit) FlushFileBuffers(g_file);
    s.state.store(kWritten, std::memory_order_release);
    g_records.fetch_add(1, std::memory_order_relaxed);
    UnlockWriter();
    // the log's summary (try-locks only)
    char line[600];
    char at[160] = "?";
    FrameShort(s.addr, at, sizeof at);
    snprintf(line, sizeof line, "[crash] %s (0x%08X) at %s on thread %lu%s%s%s; the report (stack, our code on it, the last events) is in "
             "OblivionPerfFix-crash.txt", CodeName(s.code), s.code, at, s.tid, name[0] ? " " : "", name, ThreadLabel(s.tid));
    LogCrashTry(line);
}

// Is the slot's exception still being dispatched on its (other) thread? 1 yes, 0 no (handled), -2 the thread is gone,
// -1 not known (its stack could not be read now)
int StillDispatching(const Slot& s)
{
    static uint64_t pc[kWalk], sp[kWalk];  // watchdog / one writer at a time (the copy lock)
    int depth = 0;
    if (!TakeStack(s.tid, pc, sp, kWalk, &depth, nullptr, nullptr)) return -2;
    if (!depth) return -1;
    return AncestorIn(s, pc, sp, depth) && RecordIntact(s) ? 1 : 0;
}

int g_liveCheckMs = 1000;  // the late check looks from this age on (marks sawLive, drops handled ones); writes at g_lateCheckMs

void LateCheck()
{
    if (IsDebuggerPresent()) return;
    const uint64_t now = Qpc();
    for (Slot& s : g_slots) {
        const int seen = s.state.load(std::memory_order_acquire);
        const double ageMs = QSec(static_cast<int64_t>(now - s.qpc)) * 1000.0;
        if (seen == kWritten && s.byLate && !s.lateNoted) {
            // a record of the late check: if its thread has left that exception's dispatch and goes on, it was handled
            const int r = StillDispatching(s);
            if (r == 0 && LockWriter()) {
                s.lateNoted = true;
                Out("# the exception of the record above (thread %lu) was handled after %.1f s: its thread goes on; not a crash\n\n", s.tid,
                    ageMs / 1000.0);
                OutFlush();
                UnlockWriter();
            } else if (r == -2) {
                s.lateNoted = true;
            }
            continue;
        }
        int st = kPending;
        if (seen != kPending || ageMs < g_liveCheckMs) continue;
        if (!s.state.compare_exchange_strong(st, kTaken)) continue;
        const int r = StillDispatching(s);
        if (r == 1) {
            s.sawLive = true;
            if (ageMs >= g_lateCheckMs) {
                s.byLate = true;
                WriteCrash(s, "the thread was still inside this exception's handling 3 s after it (the game's crash handler or Windows' "
                              "error reporting)", 0, false);
            }
            if (s.state.load() == kTaken) s.state.store(kPending);  // not written yet (or the writer was busy): next tick
        } else if (r == 0 || r == -2) {
            if (s.sawLive) {
                s.state.store(kPending, std::memory_order_release);  // it was live: its thread may be ending (the exit rules decide)
            } else {
                s.state.store(kDropped, std::memory_order_release);
                NoteHandled(s);
            }
        } else {
            s.state.store(kPending, std::memory_order_release);
        }
    }
}

// The exit rules (TerminateProcess on our process; written by the watchdog, else by the ending thread):
//  - a record exists already: only the line saying how the game ended;
//  - else the calling thread's pending slot (<= 120 s): one the late check saw live, else one whose place was not found
//    handled before (a probe of the game's or ours that faults again and again is no crash), oldest first;
//  - else another thread's pending slot, only if that thread is still inside the exception's dispatch now;
//  - else, for an error exit code: every thread's stack (an error exit with no exception seen).
void ExitLine(UINT code, bool atExit)
{
    if (!g_fileKept.load() || !LockWriter()) return;
    Out("# the game ended (TerminateProcess, exit code %u), %.1f s after it started\n\n", code, QSec(static_cast<int64_t>(Qpc() - g_startQpc)));
    OutFlush();
    if (g_file != INVALID_HANDLE_VALUE && !atExit) FlushFileBuffers(g_file);
    UnlockWriter();
}

void WriteHang(int stage, double openS, uint64_t frame, uint64_t rt0, uint64_t pacing0);

void ExitDecision(UINT code, DWORD caller)
{
    OwnThreadFlag own;
    if (g_fileKept.load()) { ExitLine(code, true); return; }
    const uint64_t now = Qpc();
    int mine = -1, other = -1;
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        int st = s.state.load(std::memory_order_acquire);
        for (int k = 0; k < 100 && st == kTaken && !g_outNoLock; ++k) { Sleep(10); st = s.state.load(std::memory_order_acquire); }
        if (st != kPending && st != kTaken) continue;
        if (QSec(static_cast<int64_t>(now - s.qpc)) > kSlotMaxAgeS) continue;
        if (s.tid == caller) {
            const Site* site = SiteOf(s.addr);
            if ((s.sawLive || !(site && site->handled.load() >= 2)) && (mine < 0 || s.qpc < g_slots[mine].qpc)) mine = i;
        } else if (s.sawLive || StillDispatching(s) == 1) {
            if (other < 0 || s.qpc < g_slots[other].qpc) other = i;
        }
    }
    const int best = mine >= 0 ? mine : other;
    if (best >= 0) {
        Slot& s = g_slots[best];
        int st = s.state.load(std::memory_order_acquire);
        if ((st == kPending && s.state.compare_exchange_strong(st, kTaken)) || st == kTaken)
            WriteCrash(s, s.tid == caller ? "the game ended after this exception" : "the game ended while this exception was being handled",
                       code, true);
    } else if (code != 0) {
        WriteHang(3, 0, code, caller, 0);
    }
    ExitLine(code, true);
}

// ---------------------------------------------------------------------------------------------------------------- TerminateProcess / exit
using TerminateFn = BOOL(WINAPI*)(HANDLE, UINT);
TerminateFn g_realTerminate = nullptr;

BOOL WINAPI TerminateHook(HANDLE proc, UINT code)
{
    if (g_crashOn.load() && (code != 0 || g_fileKept.load()) && (proc == GetCurrentProcess() || GetProcessId(proc) == GetCurrentProcessId()) &&
        !g_terminating.exchange(true)) {
        if (g_watchdogTid.load() && GetCurrentThreadId() != g_watchdogTid.load()) {
            g_reqCode.store(code);
            g_reqTid.store(GetCurrentThreadId());
            g_reqSlot.store(-3);
            SetEvent(g_wake);
            if (WaitForSingleObject(g_reqDone, 10000) != WAIT_OBJECT_0) ExitDecision(code, GetCurrentThreadId());
        } else {
            ExitDecision(code, GetCurrentThreadId());
        }
    }
    return g_realTerminate(proc, code);
}

using PostQuitFn = void(WINAPI*)(int);
PostQuitFn g_realPostQuit = nullptr;
void WINAPI PostQuitHook(int code)
{
    const DWORD gt = g_gtTid.load(std::memory_order_relaxed) ? g_gtTid.load(std::memory_order_relaxed) : g_mainTid;
    uint64_t none = 0;
    g_quitAnyQpc.compare_exchange_strong(none, Qpc() | 1);
    if ((!gt || GetCurrentThreadId() == gt) && !g_quit.exchange(true)) CrashEvent(kEvNote, 0, 0, 0, "exit asked");
    g_realPostQuit(code);
}

bool RedirectImport(const char* dll, const char* fn, void* hook, void** real)
{
    void** slot = FindImportSlot(Game().base, dll, fn);
    HMODULE owner = nullptr;
    const bool bound = slot && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                  static_cast<LPCWSTR>(*slot), &owner) && owner;
    if (!bound) return false;
    *real = *slot;
    return WritePointer(slot, hook);
}

// ---------------------------------------------------------------------------------------------------------------- hang
struct HangClock { bool started = false; uint64_t seq = 0; double lastMs = 0, openMs = 0; int stage = 0; };

// The open-time clock: returns 1 / 2 when a stage is reached, -stage when a frame that had reached one ended, else 0.
// paused: the game thread is in a window's modal loop (a title-bar drag or resize in windowed mode, a system menu): Windows
// runs that loop inside the game's message pump, so no frame ends while the player holds the title bar; not counted
int HangAdvance(HangClock& c, uint64_t seq, double nowMs, double stage1Ms, double stage2Ms, bool paused = false)
{
    if (!c.started) {
        c = HangClock{};
        c.started = true;
        c.seq = seq;
        c.lastMs = nowMs;
        return 0;
    }
    double d = nowMs - c.lastMs;
    c.lastMs = nowMs;
    if (d < 0) d = 0;
    if (d > kTickCapMs) d = kTickCapMs;  // a PC sleep, a suspended process, a debugger stop: one tick at most
    if (seq != c.seq) {
        const int was = c.stage;
        c.seq = seq;
        c.openMs = 0;
        c.stage = 0;
        return was ? -was : 0;
    }
    if (!paused) c.openMs += d;
    if (c.stage < 1 && c.openMs >= stage1Ms) { c.stage = 1; return 1; }
    if (c.stage == 1 && stage2Ms > 0 && c.openMs >= stage2Ms) { c.stage = 2; return 2; }
    return 0;
}

// The thread is in a window's move / size loop or a menu's modal loop (GetGUIThreadInfo, looked up in user32, which the
// game has loaded: our DLL imports kernel32 only)
bool InModalLoop(DWORD tid)
{
    using Fn = BOOL(WINAPI*)(DWORD, GUITHREADINFO*);
    static const Fn fn = [] {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        return u ? reinterpret_cast<Fn>(GetProcAddress(u, "GetGUIThreadInfo")) : nullptr;
    }();
    if (!fn || !tid) return false;
    GUITHREADINFO gi{};
    gi.cbSize = sizeof gi;
    if (!fn(tid, &gi)) return false;
    return (gi.flags & (GUI_INMOVESIZE | GUI_INMENUMODE | GUI_SYSTEMMENUMODE | GUI_POPUPMENUMODE)) != 0;
}

const volatile uint64_t* g_gframe = nullptr;
std::atomic<uint64_t> g_pacingFrames{0}, g_pacingLast{0};
std::atomic<uint64_t> g_rtFrames{0}, g_rtLast{0};
std::atomic<int> g_hangMs{10000};
std::atomic<int> g_lastNamed{-1}, g_lastMode{-1}, g_lastTravel{0};
std::atomic<uint32_t> g_fsFrames{0}, g_fsMaxUs{0};
uint64_t g_fsLastQpc = 0;  // game thread
uint32_t g_lastCell = 0xFFFFFFFF, g_lastWorld = 0xFFFFFFFF;  // game thread

// The hang clock (chosen at install, named in every record): 1 = the engine's GFrameCounter (a byte pattern), 2 =
// NpcCreationPacing's frame hook (a byte pattern), 3 = the game thread's message pump (the exe's PeekMessageW / PeekMessageA
// imports, by name: works on an exe whose patterns all miss). UE pumps messages on the game thread every frame.
int g_clockKind = 0;
std::atomic<uint64_t> g_pumpCalls{0};
using PeekFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
PeekFn g_realPeekW = nullptr, g_realPeekA = nullptr;
BOOL WINAPI PeekWHook(LPMSG m, HWND h, UINT lo, UINT hi, UINT rm)
{
    if (!g_mainTid || GetCurrentThreadId() == g_mainTid) g_pumpCalls.fetch_add(1, std::memory_order_relaxed);
    return g_realPeekW(m, h, lo, hi, rm);
}
BOOL WINAPI PeekAHook(LPMSG m, HWND h, UINT lo, UINT hi, UINT rm)
{
    if (!g_mainTid || GetCurrentThreadId() == g_mainTid) g_pumpCalls.fetch_add(1, std::memory_order_relaxed);
    return g_realPeekA(m, h, lo, hi, rm);
}

uint64_t FrameCount()
{
    switch (g_clockKind) {
    case 1: return *g_gframe;
    case 2: return g_pacingFrames.load(std::memory_order_acquire);
    case 3: return g_pumpCalls.load(std::memory_order_acquire);
    default: return 0;
    }
}
// The frame number events carry: the engine's counter, else NpcCreationPacing's
uint64_t FrameNow() { return g_gframe ? *g_gframe : g_pacingFrames.load(std::memory_order_relaxed); }

// the GPU helper: asks the device (its calls may block in a GPU hang: only this thread waits)
HANDLE g_gpuGo = nullptr, g_gpuDone = nullptr;
std::atomic<bool> g_gpuBusy{false};
constexpr size_t kGpuBuf = 16u << 10;
char g_gpuBuf[kGpuBuf];
size_t g_gpuLen = 0;
void GpuSink(const char* line)
{
    if (strncmp(line, "[gpucrash] ", 11) == 0) line += 11;
    const int n = snprintf(g_gpuBuf + g_gpuLen, kGpuBuf - g_gpuLen, "  %s\n", line);
    if (n > 0) g_gpuLen += static_cast<size_t>(n) < kGpuBuf - g_gpuLen ? static_cast<size_t>(n) : kGpuBuf - g_gpuLen - 1;
}
// Our threads' C runtime per-thread data is made at their start (the first snprintf), not inside a record (heap)
void WarmCrt()
{
    char b[64];
    snprintf(b, sizeof b, "%d %.1f %s", 1, 2.5, "x");
}

DWORD WINAPI GpuHelper(void*)
{
    t_own = 1;
    WarmCrt();
    for (;;) {
        WaitForSingleObject(g_gpuGo, INFINITE);
        g_gpuLen = 0;
        g_gpuBuf[0] = 0;
        LogSetSink(&GpuSink);
        if (!GpuReportDeviceState()) GpuSink("the game's D3D12 device was not found (no device state)");
        LogSetSink(nullptr);
        g_gpuBusy.store(false);
        SetEvent(g_gpuDone);
    }
}

void GpuPart()
{
    if (!g_gpuGo) { Out("GPU: not asked (no helper thread)\n"); return; }
    if (g_gpuBusy.exchange(true)) { Out("GPU: the device's last question has not been answered yet\n"); return; }
    ResetEvent(g_gpuDone);
    SetEvent(g_gpuGo);
    const bool ok = WaitForSingleObject(g_gpuDone, 3000) == WAIT_OBJECT_0;
    if (ok) { Out("GPU:\n"); OutRaw(g_gpuBuf, g_gpuLen); }
    else Out("GPU: the device did not answer within 3 s (its call is still waiting: the GPU or its driver may be stuck)\n");
}

int TakeAllStacks()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = GetCurrentProcessId();
    int n = 0;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    for (BOOL more = Thread32First(snap, &te); more && n < kMaxThreads; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        Row& r = g_rows[n++];
        r.tid = te.th32ThreadID;
        r.priority = 0;
        r.cycles = 0;
        ThreadName(r.tid, r.name, sizeof r.name);
        TakeStack(r.tid, r.pc, nullptr, kDepth, &r.depth, &r.priority, &r.cycles);
    }
    CloseHandle(snap);
    return n;
}

void WriteHang(int stage, double openS, uint64_t frame, uint64_t rt0, uint64_t pacing0)
{
    OwnThreadFlag own;
    if (!g_rows) {
        g_rows = static_cast<Row*>(VirtualAlloc(nullptr, sizeof(Row) * kMaxThreads, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!g_rows) return;
    }
    // CPU time per thread over 0.25 s before the stacks (which threads spin, which wait); not on the exit path
    static DWORD preTid[kMaxThreads];
    static uint64_t preCyc[kMaxThreads];
    static DWORD s1Tid[kMaxThreads];
    static uint64_t s1Cyc[kMaxThreads];
    static int s1N = 0;
    int preN = 0;
    if (stage != 3) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te;
            te.dwSize = sizeof te;
            const DWORD pid = GetCurrentProcessId();
            for (BOOL more = Thread32First(snap, &te); more && preN < kMaxThreads; more = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid) continue;
                if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID)) {
                    ULONG64 c = 0;
                    if (QueryThreadCycleTime(h, &c)) { preTid[preN] = te.th32ThreadID; preCyc[preN++] = c; }
                    CloseHandle(h);
                }
            }
            CloseHandle(snap);
        }
        Sleep(250);
    }
    const int threads = TakeAllStacks();  // first: before anything that could wait
    auto preOf = [&](DWORD tid, uint64_t* c) { for (int i = 0; i < preN; ++i) if (preTid[i] == tid) { *c = preCyc[i]; return true; } return false; };
    auto s1Of = [&](DWORD tid, uint64_t* c) { for (int i = 0; i < s1N; ++i) if (s1Tid[i] == tid) { *c = s1Cyc[i]; return true; } return false; };
    if (!LockWriter()) return;
    const uint64_t now = Qpc();
    const DWORD gt = g_gtTid.load() ? g_gtTid.load() : g_mainTid;
    g_nFnSeen = 0;
    {
        const Row* gr = nullptr;
        for (int i = 0; i < threads; ++i)
            if (g_rows[i].tid == gt) gr = &g_rows[i];
        char what[160];
        if (stage == 3) snprintf(what, sizeof what, "the game ended with exit code %llu, no crash-type exception seen", static_cast<unsigned long long>(frame));
        else snprintf(what, sizeof what, "the game's main thread has not finished a frame for %.0f s%s", openS, stage == 2 ? " (second look)" : "");
        // the three busiest threads over the 0.25 s, and the game thread's first frame that is not a Windows wait
        char extra[400] = "";
        if (preN) {
            int best[3] = {-1, -1, -1};
            uint64_t bestD[3] = {0, 0, 0};
            for (int i = 0; i < threads; ++i) {
                uint64_t c0 = 0;
                if (!preOf(g_rows[i].tid, &c0) || g_rows[i].cycles < c0) continue;
                const uint64_t d = g_rows[i].cycles - c0;
                for (int b = 0; b < 3; ++b)
                    if (best[b] < 0 || d > bestD[b]) {
                        for (int m = 2; m > b; --m) { best[m] = best[m - 1]; bestD[m] = bestD[m - 1]; }
                        best[b] = i;
                        bestD[b] = d;
                        break;
                    }
            }
            int n = snprintf(extra, sizeof extra, "busiest threads in 0.25 s (million cycles):");
            for (int b = 0; b < 3 && best[b] >= 0 && n > 0 && static_cast<size_t>(n) < sizeof extra; ++b)
                n += snprintf(extra + n, sizeof extra - n, "%s %s %lu %.0f", b ? "," : "", g_rows[best[b]].name[0] ? g_rows[best[b]].name : "?",
                              g_rows[best[b]].tid, static_cast<double>(bestD[b]) / 1e6);
            if (gr && n > 0 && static_cast<size_t>(n) < sizeof extra) {
                for (int k = 0; k < gr->depth; ++k) {
                    const Mod* m = ModOf(gr->pc[k]);
                    if (m && (_stricmp(m->name, "ntdll.dll") == 0 || _stricmp(m->name, "KERNELBASE.dll") == 0 || _stricmp(m->name, "kernel32.dll") == 0 ||
                              _stricmp(m->name, "win32u.dll") == 0 || _stricmp(m->name, "user32.dll") == 0))
                        continue;
                    char f[96];
                    FrameShort(gr->pc[k], f, sizeof f);
                    n += snprintf(extra + n, sizeof extra - n, "; game thread in %s (frame %d)", f, k);
                    break;
                }
                uint64_t g0 = 0;
                if (preOf(gr->tid, &g0) && gr->cycles >= g0 && n > 0 && static_cast<size_t>(n) < sizeof extra) {
                    const double mc = static_cast<double>(gr->cycles - g0) / 1e6;
                    n += snprintf(extra + n, sizeof extra - n, ", %s (%.0f million cycles in 0.25 s)", mc < 25 ? "sleeping or waiting" : mc < 400 ? "partly busy" : "spinning", mc);
                }
            }
            if (stage != 3 && n > 0 && static_cast<size_t>(n) < sizeof extra)
                snprintf(extra + n, sizeof extra - n, "\noutcome: still stuck when this was written (a \"# the frame ... ended\" line at the end of the file says if it recovered)");
        }
        static const uint64_t none[1] = {0};
        Summary(stage == 3 ? "fatal exit" : "hang", what, "the game thread's stack now", gr && gr->depth ? gr->pc : none, gr ? gr->depth : 0,
                extra[0] ? extra : nullptr, stage == 3);
        OutFlush();
        KeepFile();  // from here the file stays even if the process ends in the middle of the rest
    }
    if (stage == 3) {
        Header("the game ended itself with an error exit code, with no fatal-type exception seen (every thread's stack)");
        Out("exit code: %llu (TerminateProcess on thread %lu)\n", static_cast<unsigned long long>(frame), static_cast<unsigned long>(rt0));
    } else {
        Header(stage == 1 ? "hang (the game's main thread has not finished a frame)" : "hang, second look (stacks again)");
        Out("the game thread has not finished a frame for %.1f s (count %llu of the hang clock: %s)\n", openS,
            static_cast<unsigned long long>(frame), g_clockName);
    }
    const uint64_t rtLast = g_rtLast.load(), rt = g_rtFrames.load();
    if (rtLast) Out("render thread: %llu frames begun since this frame began, the last %.2f s ago\n", static_cast<unsigned long long>(rt - rt0),
                    QSec(static_cast<int64_t>(now - rtLast)));
    else Out("render thread: frames not counted (the frame fence's hook is off)\n");
    const uint64_t pf = g_pacingLast.load();
    if (pf) Out("NpcCreationPacing frames: %llu since this frame began, the last %.2f s ago\n",
                static_cast<unsigned long long>(g_pacingFrames.load() - pacing0), QSec(static_cast<int64_t>(now - pf)));
    Out("game state at the last frame: travel %s, freeze %d named (mode %d)\n", g_lastTravel.load() ? "pending (door, gate, fast travel)" : "no",
        g_lastNamed.load(), g_lastMode.load());
    if (stage != 3 && g_quitAnyQpc.load())
        Out("note: written after the game was asked to quit (%.0f s before); may be a shutdown wait\n",
            QSec(static_cast<int64_t>(now - g_quitAnyQpc.load())));
    Out("threads (%d; each stopped once for a few microseconds; frames innermost first; cpu = million cycles in the 0.25 s before%s):\n",
        threads, stage == 2 ? ", since = million cycles since the first look" : "");
    // the game thread, then the render / RHI / loading threads in full (with code bytes), then every other thread on one line
    auto detailed = [&](const Row& r) {
        return r.tid == gt || strncmp(r.name, "RenderThread", 12) == 0 || strncmp(r.name, "RHIThread", 9) == 0 ||
               strstr(r.name, "AsyncLoading") || strstr(r.name, "IOThread") || strstr(r.name, "RHISubmission");
    };
    auto cpu = [&](const Row& r) {
        uint64_t c0 = 0, c1 = 0;
        if (preOf(r.tid, &c0) && r.cycles >= c0) Out(" cpu %.1f", static_cast<double>(r.cycles - c0) / 1e6);
        if (stage == 2 && s1Of(r.tid, &c1) && r.cycles >= c1) Out(" since %.0f", static_cast<double>(r.cycles - c1) / 1e6);
    };
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < threads; ++i) {
            const Row& r = g_rows[i];
            if ((pass == 0) != (r.tid == gt) || !detailed(r)) continue;
            Out("%s %lu %s, priority %d, cycles %llu,", r.tid == gt ? "game thread" : "thread", r.tid, r.name, r.priority,
                static_cast<unsigned long long>(r.cycles));
            cpu(r);
            Out(", %d frames:\n", r.depth);
            Stack(r.pc, r.depth, r.tid == gt ? kDepth : 12, false);
        }
    for (int i = 0; i < threads; ++i) {
        const Row& r = g_rows[i];
        if (detailed(r)) continue;
        Out("T %lu %s%s p%d", r.tid, r.name, ThreadLabel(r.tid), r.priority);
        cpu(r);
        Out(" d%d:", r.depth);
        Stack(r.pc, r.depth, 0, true);
    }
    if (stage == 1) {  // kept for the second look's "since"
        s1N = 0;
        for (int i = 0; i < threads && s1N < kMaxThreads; ++i) { s1Tid[s1N] = g_rows[i].tid; s1Cyc[s1N++] = g_rows[i].cycles; }
    }
    PendingSlots(nullptr);
    ExeFunctions();
    Modules();
    Unloaded();
    Installs();
    Tail();
    if (stage != 3) GpuPart();  // not on the exit path (it can wait 3 s)
    Out("# end\n\n");
    OutFlush();
    if (g_file != INVALID_HANDLE_VALUE && stage != 3) FlushFileBuffers(g_file);
    g_records.fetch_add(1, std::memory_order_relaxed);
    UnlockWriter();
    char line[300];
    if (stage == 3)
        snprintf(line, sizeof line, "[crash] the game ended with exit code %llu and no fatal-type exception seen: every thread's stack is "
                 "in OblivionPerfFix-crash.txt", static_cast<unsigned long long>(frame));
    else
        snprintf(line, sizeof line, "[hang] the game thread has not finished a frame for %.0f s: every thread's stack and the last events "
                 "are in OblivionPerfFix-crash.txt%s", openS, stage == 2 ? " (second look)" : "");
    LogCrashTry(line);
}

void HangEnded(double openS)
{
    if (LockWriter()) {
        Out("# the frame that had not finished ended after %.1f s (the game went on)\n\n", openS);
        OutFlush();
        if (g_file != INVALID_HANDLE_VALUE) FlushFileBuffers(g_file);
        UnlockWriter();
    }
    char line[200];
    snprintf(line, sizeof line, "[hang] the frame ended after %.1f s; the game went on", openS);
    LogCrashTry(line);
}

DWORD WINAPI Watchdog(void*)
{
    t_own = 1;  // our own faults (guarded reads) are never slots
    WarmCrt();
    g_watchdogTid.store(GetCurrentThreadId());
    HangClock clock{};
    uint64_t seq = ~0ull, rt0 = 0, pacing0 = 0, epStart = 0, pumpLast = 0;
    int records = 0, pumpRun = 0;
    bool episode = false, pumpArmed = false;
    for (;;) {
        WaitForSingleObject(g_wake, kTickMs);
        // a TerminateProcess request: the exit rules (ExitDecision)
        if (g_reqSlot.exchange(-1) == -3) {
            ExitDecision(g_reqCode.load(), g_reqTid.load());
            SetEvent(g_reqDone);
        }
        if (g_crashOn.load(std::memory_order_relaxed)) LateCheck();
        if (!g_hangOn.load(std::memory_order_relaxed) || g_quit.load(std::memory_order_relaxed) || IsDebuggerPresent()) {
            clock = HangClock{};
            continue;
        }
        const uint64_t frames = FrameCount();
        if (!frames) continue;  // armed after the first frame
        if (g_clockKind == 3 && !pumpArmed) {
            // the message pump runs before the engine's frame loop too (start-up, the first shader compile): armed only once it
            // has moved in every tick of the last 10 s and the process is at least 2 minutes old
            pumpRun = frames != pumpLast ? pumpRun + 1 : 0;
            pumpLast = frames;
            pumpArmed = pumpRun >= 20 && QSec(static_cast<int64_t>(Qpc() - g_startQpc)) >= 120;
            if (!pumpArmed) continue;
        }
        if (frames != seq) {
            seq = frames;
            rt0 = g_rtFrames.load(std::memory_order_relaxed);
            pacing0 = g_pacingFrames.load(std::memory_order_relaxed);
        }
        const uint64_t now = Qpc();
        const double ms = static_cast<double>(g_hangMs.load(std::memory_order_relaxed));
        const int st = HangAdvance(clock, frames, QSec(static_cast<int64_t>(now)) * 1000.0, ms, 2 * ms, InModalLoop(g_gtTid.load() ? g_gtTid.load() : g_mainTid));
        if (st == 1) {
            epStart = now - static_cast<uint64_t>(clock.openMs / 1000.0 * static_cast<double>(g_freq));
            CrashEvent(kEvHang, 1, static_cast<uint64_t>(clock.openMs / 1000.0));
            episode = records < kMaxHangRecords;
            if (episode) { ++records; WriteHang(1, clock.openMs / 1000.0, frames, rt0, pacing0); }
        } else if (st == 2 && episode) {
            CrashEvent(kEvHang, 2, static_cast<uint64_t>(clock.openMs / 1000.0));
            WriteHang(2, clock.openMs / 1000.0, frames, rt0, pacing0);
        } else if (st < 0) {
            const double open = QSec(static_cast<int64_t>(now - epStart));
            CrashEvent(kEvHang, 0, static_cast<uint64_t>(open));
            if (episode) HangEnded(open);
            episode = false;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------- start-up
void ReadPdb()
{
    __try {
        const uint8_t* b = g_selfBase;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
        g_selfSize = nt->OptionalHeader.SizeOfImage;
        const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        const auto* e = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(b + d.VirtualAddress);
        for (size_t i = 0; d.VirtualAddress && i < d.Size / sizeof *e; ++i) {
            if (e[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !e[i].AddressOfRawData) continue;
            const uint8_t* cv = b + e[i].AddressOfRawData;
            if (memcmp(cv, "RSDS", 4) != 0) continue;
            GUID g;
            memcpy(&g, cv + 4, sizeof g);
            uint32_t age;
            memcpy(&age, cv + 20, 4);
            snprintf(g_pdb, sizeof g_pdb, "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X age %u", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                     g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], age);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void FunctionRange(const char* name, uint64_t* begin, uint64_t* end)
{
    const uint64_t a = reinterpret_cast<uint64_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name));
    *begin = *end = 0;
    if (!a) return;
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(a, &image, nullptr);
    *begin = fe ? image + fe->BeginAddress : a;
    *end = fe ? image + fe->EndAddress : a + 0x100;
}

// The report file: a previous non-empty one kept as .1.txt; the new one marked for deletion until a record is written.
bool OpenReportFile(const wchar_t* dir)
{
    swprintf_s(g_path, L"%s\\OblivionPerfFix-crash.txt", dir ? dir : L".");
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(g_path, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh)) {
        wchar_t prev[MAX_PATH + 32];
        swprintf_s(prev, L"%s\\OblivionPerfFix-crash.1.txt", dir ? dir : L".");
        MoveFileExW(g_path, prev, MOVEFILE_REPLACE_EXISTING);
    }
    g_file = CreateFileW(g_path, GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return false;
    FILE_DISPOSITION_INFO d{};
    d.DeleteFile = TRUE;
    SetFileInformationByHandle(g_file, FileDispositionInfo, &d, sizeof d);
    g_fileKept.store(false);
    return true;
}

bool StartCommon(const wchar_t* dir)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart ? f.QuadPart : 1;
    g_startQpc = Qpc();
    {
        FILETIME created, exited, kernel, user, nowFt;
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            GetSystemTimeAsFileTime(&nowFt);
            const uint64_t c = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            const uint64_t t = (static_cast<uint64_t>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
            if (t > c) g_startQpc -= static_cast<uint64_t>(static_cast<double>(t - c) / 1e7 * static_cast<double>(g_freq));
        }
    }
    g_qit = reinterpret_cast<NtQitFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    ReadSystemLine();
    FunctionRange("KiUserExceptionDispatcher", &g_kiDispatcher, &g_kiEnd);
    FunctionRange("RtlRaiseException", &g_raise, &g_raiseEnd);
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<const void*>(&StartCommon), &mbi, sizeof mbi)) g_selfBase = static_cast<const uint8_t*>(mbi.AllocationBase);
    ReadPdb();
    g_copy = static_cast<uint8_t*>(VirtualAlloc(nullptr, kCopy, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_copy) return false;
    memset(g_copy, 0, kCopy);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_reqDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake || !g_reqDone) return false;
    if (!OpenReportFile(dir)) return false;
    g_ringOn.store(true);
    return true;
}

HANDLE g_watchdog = nullptr;
bool StartWatchdog()
{
    if (g_watchdog) return true;
    g_watchdog = CreateThread(nullptr, 256 * 1024, &Watchdog, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!g_watchdog) return false;
    DescribeThread(g_watchdog, L"OPF watchdog");
    for (int i = 0; i < 200 && !g_watchdogTid.load(); ++i) Sleep(0);
    return true;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------- public
void CrashEvent(uint16_t kind, uint64_t a, uint64_t b, uint64_t c, const char* text)
{
    if (!g_ringOn.load(std::memory_order_relaxed)) return;
    const uint64_t i = g_ringHead.fetch_add(1, std::memory_order_relaxed);
    RingEntry& e = g_ring[i % kRing];
    e.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    e.qpc = Qpc();
    e.kind = kind;
    e.tid = GetCurrentThreadId();
    e.a = a;
    e.b = b;
    e.c = c;
    size_t n = 0;
    if (text)
        for (; text[n] && n < 11; ++n) e.text[n] = text[n];
    for (size_t k = n; k < 12; ++k) e.text[k] = 0;
    e.frame = static_cast<uint32_t>(FrameNow());
    e.seq.store(i + 1, std::memory_order_release);
}

bool InstallCrashReport(const wchar_t* dir, bool beforeGameCode)
{
    if (g_crashOn.load()) return true;
    if (!StartCommon(dir)) {
        Log("[crash] CrashReport not installed: the report file %ls could not be made or set up (error %lu)", g_path, GetLastError());
        return false;
    }
    g_mainTid = beforeGameCode ? GetCurrentThreadId() : 0;
    const GameModule& game = Game();
    g_exeStamp = game.timeDateStamp;
    g_exeSize = static_cast<uint32_t>(game.sizeOfImage);
    {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const wchar_t* name = path;
        for (const wchar_t* p = path; *p; ++p)
            if (*p == L'\\') name = p + 1;
        size_t i = 0;
        for (; name[i] && i < sizeof g_exeName - 1; ++i) g_exeName[i] = name[i] < 127 ? static_cast<char>(name[i]) : '?';
        g_exeName[i] = 0;
    }
    if (!AddVectoredExceptionHandler(1, &Handler)) {
        Log("[crash] CrashReport not installed: no exception handler");
        return false;
    }
    g_crashOn.store(true);
    {
        // the process start (local time), the game exe's version, our DLL file's CRC-32 (the summary's build id)
        FILETIME created, exited, kernel, user, local;
        SYSTEMTIME st;
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) && FileTimeToLocalFileTime(&created, &local) &&
            FileTimeToSystemTime(&local, &st))
            snprintf(g_startText, sizeof g_startText, "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        uint32_t vms = 0, vls = 0;
        VerStrings vs{};
        VersionStrings(Game().base, &vs);
        if (FileVersion(Game().base, &vms, &vls))
            snprintf(g_gameVer, sizeof g_gameVer, "%u.%u.%u.%u%s%s", vms >> 16, vms & 0xFFFF, vls >> 16, vls & 0xFFFF, vs.fileVer[0] ? " / " : "", vs.fileVer);
        wchar_t self[MAX_PATH] = L"";
        if (GetModuleFileNameW(reinterpret_cast<HMODULE>(const_cast<uint8_t*>(g_selfBase)), self, MAX_PATH)) {
            HANDLE h = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                static uint32_t table[256];
                for (uint32_t i = 0; i < 256; ++i) {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
                    table[i] = c;
                }
                uint32_t crc = 0xFFFFFFFFu;
                static uint8_t buf[64 << 10];
                DWORD got = 0;
                while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got)
                    for (DWORD i = 0; i < got; ++i) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
                CloseHandle(h);
                g_dllCrc = ~crc;
            }
        }
    }
    {
        // the game's folder for the module list's relative paths: the exe's mapped path up to "\OblivionRemastered\Binaries\"
        wchar_t p[MAX_PATH] = L"";
        K32GetMappedFileNameW(GetCurrentProcess(), Game().base, p, MAX_PATH);
        if (const wchar_t* at = wcsstr(p, L"\\OblivionRemastered\\Binaries\\")) {
            g_gameRootLen = static_cast<size_t>(at - p) + 1;
            wcsncpy_s(g_gameRoot, p, g_gameRootLen);
        }
    }
    g_gframe = FindGFrameCounter();  // the events' frame numbers (and HangReport's first clock); null on an exe it does not match
    {
        // the ini as read at start-up: its CRC-32 and size for the report's header
        wchar_t ini[MAX_PATH + 32];
        swprintf_s(ini, L"%s\\OblivionPerfFix.ini", dir ? dir : L".");
        HANDLE h = CreateFileW(ini, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            uint32_t crc = 0xFFFFFFFFu, size = 0;
            uint8_t buf[4096];
            DWORD got = 0;
            while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) {
                for (DWORD i = 0; i < got; ++i) {
                    crc ^= buf[i];
                    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                }
                size += got;
            }
            CloseHandle(h);
            g_iniCrc = ~crc;
            g_iniSize = size;
        }
    }
    const bool term = RedirectImport("KERNEL32.dll", "TerminateProcess", reinterpret_cast<void*>(&TerminateHook),
                                     reinterpret_cast<void**>(&g_realTerminate));
    const bool quit = RedirectImport("USER32.dll", "PostQuitMessage", reinterpret_cast<void*>(&PostQuitHook),
                                     reinterpret_cast<void**>(&g_realPostQuit));
    const bool dog = StartWatchdog();
    Log("[crash] CrashReport installed: a crash writes where it happened to OblivionPerfFix-crash.txt next to this DLL (the game's end "
        "%s; the late check %s; the game's own exit %s)", term ? "through its TerminateProcess import" : "NOT seen (its TerminateProcess import was not redirected)",
        dog ? "runs" : "is OFF (no watchdog thread)", quit ? "seen" : "not seen");
    return true;
}

bool InstallHangReport(int seconds, bool pacingFrames)
{
    if (g_hangOn.load()) return true;
    if (!g_crashOn.load()) {
        Log("[hang] HangReport needs CrashReport=1 (its report file and watchdog): not installed");
        return false;
    }
    if (!g_realPostQuit) {
        Log("[hang] HangReport not installed: the game's exit cannot be seen (its PostQuitMessage import was not redirected), and UE's "
            "exit has ~10 s without frames");
        return false;
    }
    // the clock: the engine's frame counter, else NpcCreationPacing's frame hook, else the game thread's message pump (imports by
    // name: no byte pattern, for an exe whose patterns miss)
    if (g_gframe) {
        g_clockKind = 1;
        g_clockName = "the engine's frame counter (GFrameCounter)";
    } else if (pacingFrames) {
        g_clockKind = 2;
        g_clockName = "NpcCreationPacing's frame hook (the engine's frame counter was not found)";
    } else {
        const bool w = RedirectImport("USER32.dll", "PeekMessageW", reinterpret_cast<void*>(&PeekWHook), reinterpret_cast<void**>(&g_realPeekW));
        const bool a = RedirectImport("USER32.dll", "PeekMessageA", reinterpret_cast<void*>(&PeekAHook), reinterpret_cast<void**>(&g_realPeekA));
        if (!w && !a) {
            Log("[hang] HangReport not installed: no clock (the engine's frame counter was not found, NpcCreationPacing's frame hook is "
                "off and the exe's PeekMessage imports could not be redirected)");
            return false;
        }
        g_clockKind = 3;
        g_clockName = "the game thread's message pump (PeekMessage; the frame counter and NpcCreationPacing were not available; armed "
                      "after 2 minutes)";
    }
    SetHangReportSeconds(seconds);
    g_gpuGo = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_gpuDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_gpuGo && g_gpuDone) {
        if (HANDLE t = CreateThread(nullptr, 1024 * 1024, &GpuHelper, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)) {
            DescribeThread(t, L"OPF watchdog GPU");
            CloseHandle(t);
        } else {
            g_gpuGo = nullptr;
        }
    }
    g_hangOn.store(true);
    Log("[hang] HangReport installed: a game-thread frame open %d s writes every thread's stack to OblivionPerfFix-crash.txt (clock: %s)",
        g_hangMs.load() / 1000, g_clockName);
    return true;
}

void CrashReportSetGpuLine(const char* line)
{
    if (line) strncpy_s(g_gpuLine, line, _TRUNCATE);
    // the graphics DLLs are loaded now: every module once in the log, and the DLLs that are not Windows' own
    static bool logged = false;
    if (logged) return;
    logged = true;
    int foreign = 0, proxies = 0, copies = 0;
    ForEachModule(400, [&](const ModInfo& m) {
        char l[640];
        ModuleLine(m, l, sizeof l);
        Log("[mods] %s", l);
        if (!Foreign(m)) return;
        ++foreign;
        if (strcmp(m.cls, "game") == 0 && ProxyName(m.name) && !strstr(m.vs.product, "Microsoft")) {  // not Windows' own copies the game ships
            ++proxies;
            Log("[mods] note: %s in the game's folder is a proxy DLL (%s %s): another mod loads through it (OptiScaler, ReShade, a mod "
                "loader); if the game misbehaves, try without it", m.name, m.vs.product[0] ? m.vs.product : "no product name",
                m.vs.fileVer[0] ? m.vs.fileVer : "");
        }
        if (strstr(m.vs.product, "Oblivion Remastered Performance Fix") || strstr(m.vs.desc, "OblivionPerfFix")) {
            ++copies;
            Log("[mods] WARNING: a second copy of OblivionPerfFix is loaded (%s, %s): install it once only (version.dll or the OBSE64 "
                "plugin)", m.name, m.cls);
        }
    });
    Log("[mods] %d module(s) not from Windows, %d proxy DLL(s) in the game's folder, %d other copy(ies) of OblivionPerfFix", foreign,
        proxies, copies);
}

void CrashReportLogSystem()
{
    if (!g_crashOn.load()) ReadSystemLine();
    Log("[crash] %s", g_sysLine);
}

void CrashReportNoteTestKey(const char* key) { g_testKey = key; }

void NoteInstallResult(const char* name, int result)
{
    const int i = g_nInstalls.fetch_add(1);
    if (i < 320) g_installs[i] = InstallNote{name, result};
}

void SetHangReportSeconds(int seconds)
{
    const int s = seconds < 5 ? 5 : seconds > 120 ? 120 : seconds;
    g_hangMs.store(s * 1000, std::memory_order_relaxed);
}

void CrashReportProcessExit(bool otherThreadsEnded)
{
    if (!g_crashOn.load() || !otherThreadsEnded || g_quit.load()) return;
    // ExitProcess has ended every other thread (the watchdog too, maybe inside a write): no locks from here. Only an exception
    // the late check saw still in dispatch counts (a handled one may be pending without that)
    const uint64_t now = Qpc();
    int i = -1;
    for (int k = 0; k < kSlots; ++k) {
        const Slot& s = g_slots[k];
        const int st = s.state.load();
        if ((st == kPending || st == kTaken) && s.sawLive && QSec(static_cast<int64_t>(now - s.qpc)) <= kSlotMaxAgeS) i = k;
    }
    if (i < 0) return;
    g_outNoLock = true;
    g_outLen = 0;
    t_own = 1;
    WriteCrashLocked(g_slots[i], "the process exited (ExitProcess) while this exception was being handled", 0, true);
}

void CrashReportFrame(const uint8_t* tes, bool travel)
{
    const uint64_t now = Qpc();
    if (!g_gtTid.load(std::memory_order_relaxed)) g_gtTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (g_fsLastQpc && now > g_fsLastQpc) {
        const double us = static_cast<double>(now - g_fsLastQpc) * 1e6 / static_cast<double>(g_freq);
        const uint32_t u = us > 4e9 ? 0xFFFFFFFFu : static_cast<uint32_t>(us);
        g_fsFrames.fetch_add(1, std::memory_order_relaxed);
        if (u > g_fsMaxUs.load(std::memory_order_relaxed)) g_fsMaxUs.store(u, std::memory_order_relaxed);
    }
    g_fsLastQpc = now;
    g_pacingLast.store(now, std::memory_order_relaxed);
    g_pacingFrames.fetch_add(1, std::memory_order_release);
    if (!g_ringOn.load(std::memory_order_relaxed)) return;
    if (static_cast<int>(travel) != g_lastTravel.load(std::memory_order_relaxed)) {
        g_lastTravel.store(travel, std::memory_order_relaxed);
        CrashEvent(kEvTravel, travel);
    }
    int32_t named = 0, mode = 0;
    if (ReadMenuFreeze(&named, &mode) && (named != g_lastNamed.load(std::memory_order_relaxed) || mode != g_lastMode.load(std::memory_order_relaxed))) {
        g_lastNamed.store(named, std::memory_order_relaxed);
        g_lastMode.store(mode, std::memory_order_relaxed);
        CrashEvent(kEvFreeze, static_cast<uint64_t>(named), static_cast<uint64_t>(mode));
    }
    if (tes) {
        uint32_t cell = 0, world = 0;
        uint64_t cellPtr = 0, worldPtr = 0;
        __try {  // TES: +0x50 the interior cell, +0xB0 the worldspace (row_preload.cpp); TESForm +0x10 its form id
            cellPtr = *reinterpret_cast<const uint64_t*>(tes + 0x50);
            worldPtr = *reinterpret_cast<const uint64_t*>(tes + 0xB0);
            if (cellPtr) cell = *reinterpret_cast<const uint32_t*>(cellPtr + 0x10);
            if (worldPtr) world = *reinterpret_cast<const uint32_t*>(worldPtr + 0x10);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        if (cell != g_lastCell) { g_lastCell = cell; CrashEvent(kEvCell, cell, cellPtr); }
        if (world != g_lastWorld && worldPtr) { g_lastWorld = world; CrashEvent(kEvWorld, world, worldPtr); }
    }
}

void CrashReportRenderFrame()
{
    g_rtLast.store(Qpc(), std::memory_order_relaxed);
    g_rtFrames.fetch_add(1, std::memory_order_relaxed);
}

bool CrashReportFrameStats(uint32_t* frames, uint32_t* maxUs)
{
    if (!g_pacingLast.load(std::memory_order_relaxed)) return false;
    *frames = g_fsFrames.exchange(0, std::memory_order_relaxed);
    *maxUs = g_fsMaxUs.exchange(0, std::memory_order_relaxed);
    return true;
}

void CrashReportAlive()
{
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 10000) return;
    last = now;
    double x = 0, y = 0, z = 0;
    if (NpcPacingLastPlayerPos(&x, &y, &z))
        CrashEvent(kEvAlive, static_cast<uint64_t>(static_cast<int64_t>(x)), static_cast<uint64_t>(static_cast<int64_t>(y)),
                   static_cast<uint64_t>(static_cast<int64_t>(z)));
    else
        CrashEvent(kEvNote, 0, 0, 0, "alive");
}

void CrashReportLogStats()
{
    if (!g_crashOn.load()) return;
    Log("[crash] stats: first-chance fatal-type exceptions %llu (handled %llu, slots full %llu), C++ exceptions %llu, records %llu; "
        "events %llu", static_cast<unsigned long long>(g_fatalSeen.load()), static_cast<unsigned long long>(g_handled.load()),
        static_cast<unsigned long long>(g_slotsFull.load()), static_cast<unsigned long long>(g_throwsSeen.load()),
        static_cast<unsigned long long>(g_records.load()), static_cast<unsigned long long>(g_ringHead.load()));
    // where they came from (the first 16 places; the module looked up here, on the monitor thread)
    char line[1000];
    size_t len = 0;
    for (const Site& s : g_sites) {
        const uint64_t a = s.addr.load(std::memory_order_relaxed);
        if (!a || len > sizeof line - 120) continue;
        char where[96] = "x";
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof mbi) && mbi.Type == MEM_IMAGE && mbi.AllocationBase) {
            const uint8_t* b = static_cast<const uint8_t*>(mbi.AllocationBase);
            wchar_t path[MAX_PATH] = L"";
            K32GetMappedFileNameW(GetCurrentProcess(), const_cast<uint8_t*>(b), path, MAX_PATH);
            const wchar_t* nm = path;
            for (const wchar_t* p = path; *p; ++p)
                if (*p == L'\\') nm = p + 1;
            char n8[64];
            Narrow(nm, 60, n8, sizeof n8);
            snprintf(where, sizeof where, "%s+0x%llX", b == Game().base ? "exe" : b == g_selfBase ? "OPF" : n8,
                     static_cast<unsigned long long>(a - reinterpret_cast<uint64_t>(b)));
        }
        const int w = snprintf(line + len, sizeof line - len, "%s 0x%08X at %s x%u", len ? "," : "", s.code, where, s.count.load());
        if (w > 0) len += static_cast<size_t>(w);
    }
    if (len) Log("[crash] first-chance places: %s", line);
}

// ---------------------------------------------------------------------------------------------------------------- self-test
namespace {
struct StArg { HANDLE ev; std::atomic<int> state; int mode; };
std::atomic<int> g_stFilterMs{0};
int StFilter(int ms)
{
    if (ms) Sleep(static_cast<DWORD>(ms));  // a crash handler that takes long (UE's ReportCrash waiting for its minidump)
    return EXCEPTION_EXECUTE_HANDLER;
}
__declspec(noinline) void StFault(int ms)
{
    __try {
        static int* volatile s_null = nullptr;
        *static_cast<volatile int*>(s_null) = 1;
    } __except (StFilter(ms)) {
    }
}
__declspec(noinline) void StAssert()
{
    struct { const wchar_t* msg; void* pc; } info{L"Fatal error: self-test message", nullptr};
    ULONG_PTR arg = reinterpret_cast<ULONG_PTR>(&info);
    __try {
        RaiseException(kUeAssert, 0, 1, &arg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
DWORD WINAPI StThread(void* p)
{
    auto* a = static_cast<StArg*>(p);
    if (a->mode == 0) StFault(0);
    if (a->mode == 1) StFault(g_stFilterMs.load());
    if (a->mode == 2) StAssert();
    a->state.store(1);
    if (a->mode == 3) {  // blocked in its own function (a hang)
        WaitForSingleObject(a->ev, INFINITE);
    }
    a->state.store(2);
    return 0;
}
__declspec(noinline) DWORD WINAPI StBlocked(void* p) { return StThread(p); }

int SlotsOf(DWORD tid, int state)
{
    int n = 0;
    for (const Slot& s : g_slots) n += s.tid == tid && s.state.load() == state;
    return n;
}
const Slot* SlotOf(DWORD tid)
{
    for (const Slot& s : g_slots)
        if (s.tid == tid && s.state.load() != kFree) return &s;
    return nullptr;
}
size_t ReadAll(const wchar_t* path, char* buf, size_t cap)
{
    HANDLE r = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (r == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    ReadFile(r, buf, static_cast<DWORD>(cap - 1), &got, nullptr);
    buf[got] = 0;
    CloseHandle(r);
    return got;
}
bool InStack(const Slot* s, const void* fn)
{
    if (!s) return false;
    for (int k = 0; k < s->depth; ++k) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(s->pc[k], &image, nullptr);
        if (fe && image + fe->BeginAddress == reinterpret_cast<uint64_t>(fn)) return true;
    }
    return false;
}
}  // namespace

int CrashReportSelfTest(const wchar_t* dir)
{
    int fail = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) ++fail;
        Log("[crash] self-test %s: %s", ok ? "PASS" : "FAIL", what);
    };
    // the hang clock
    {
        HangClock c{};
        int bad = HangAdvance(c, 7, 0, 10000, 20000) != 0;
        int s1 = -1, s2 = -1;
        for (int i = 1; i <= 100; ++i) {
            const int r = HangAdvance(c, 7, i * 500.0, 10000, 20000);
            if (r == 1) s1 = i;
            if (r == 2) s2 = i;
        }
        bad += s1 != 20 || s2 != 40;
        bad += HangAdvance(c, 8, 101 * 500.0, 10000, 20000) != -2 || c.openMs != 0 || c.stage != 0;
        HangClock g{};
        HangAdvance(g, 1, 0, 10000, 20000);
        bad += HangAdvance(g, 1, 3600000.0, 10000, 20000) != 0 || g.openMs != 1000;  // a one-hour gap counts 1 s
        HangClock n{};
        HangAdvance(n, 1, 0, 10000, 20000);
        int fired = 0;
        for (int i = 1; i <= 400; ++i) fired += HangAdvance(n, 1 + i, i * 500.0, 10000, 20000) != 0;  // frames keep ending
        bad += fired != 0;
        check(bad == 0, "hang clock: 10 s / 20 s stages, ended, a 1 h gap counts 1 s, ending frames never fire");
    }
    // the ring: wrap-around, order, no torn entry with 8 writers
    {
        g_ringOn.store(true);
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = f.QuadPart;
        const uint64_t h0 = g_ringHead.load();
        struct W { static DWORD WINAPI Run(void* p) {
            const uint64_t id = reinterpret_cast<uint64_t>(p);
            for (uint64_t i = 0; i < 100000; ++i) CrashEvent(kEvKey, id, i, id * 1000003 + i, "w");
            return 0; } };
        HANDLE th[8];
        for (uint64_t i = 0; i < 8; ++i) th[i] = CreateThread(nullptr, 0, &W::Run, reinterpret_cast<void*>(i + 1), 0, nullptr);
        int torn = 0, seen = 0, whileWritten = 0;
        for (bool last = false;;) {  // read while they write, until they are done, then once more
            const bool done = WaitForMultipleObjects(8, th, TRUE, 0) != WAIT_TIMEOUT;
            const uint64_t head = g_ringHead.load();
            for (uint64_t i = head > kRing ? head - kRing : 0; i < head; ++i) {
                RingEntry& e = g_ring[i % kRing];
                const uint64_t s1 = e.seq.load(std::memory_order_acquire);
                const uint64_t a = e.a, b = e.b, c = e.c;
                std::atomic_thread_fence(std::memory_order_acquire);
                if (s1 != i + 1 || e.seq.load(std::memory_order_relaxed) != s1) continue;
                ++seen;
                whileWritten += !done;
                torn += c != a * 1000003 + b;
            }
            if (last) break;
            last = done;
        }
        for (HANDLE t : th) CloseHandle(t);
        const uint64_t h1 = g_ringHead.load();
        int order = 0;
        for (uint64_t i = h1 - kRing + 1; i < h1; ++i) order += g_ring[i % kRing].qpc < g_ring[(i - 1) % kRing].qpc && g_ring[i % kRing].tid == g_ring[(i - 1) % kRing].tid;
        Log("[crash] self-test: ring %llu events, %d read (%d while written), %d torn, %d out of order", static_cast<unsigned long long>(h1 - h0),
            seen, whileWritten, torn, order);
        check(h1 - h0 == 800000 && torn == 0 && seen > 0 && order == 0, "event ring: 8 threads x 100,000 events, no torn entry read, order kept");
    }
    // the handler and the late check, with the report file in the test folder
    wchar_t testDir[MAX_PATH];
    swprintf_s(testDir, L"%s\\crashreport-test", dir ? dir : L".");
    CreateDirectoryW(testDir, nullptr);
    wchar_t file[MAX_PATH + 40], prev[MAX_PATH + 40];
    swprintf_s(file, L"%s\\OblivionPerfFix-crash.txt", testDir);
    swprintf_s(prev, L"%s\\OblivionPerfFix-crash.1.txt", testDir);
    DeleteFileW(file);
    DeleteFileW(prev);
    {
        // (7a) an empty report is never left: the file is marked for deletion until a record is written
        g_selfBase = nullptr;
        if (!StartCommon(testDir)) { check(false, "report file / start-up"); return fail; }
        const bool hidden = GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES || CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE;
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
        const bool gone = GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES;
        check(hidden && gone, "report file: marked for deletion at start; no record -> no file at the end");
        if (!OpenReportFile(testDir)) { check(false, "report file again"); return fail; }
    }
    g_mainTid = GetCurrentThreadId();
    void* const veh = AddVectoredExceptionHandler(1, &Handler);
    if (!veh) { check(false, "handler"); return fail; }
    g_crashOn.store(true);
    StartWatchdog();
    g_lateCheckMs = 400;
    g_liveCheckMs = 150;
    // (1) a handled AV in a test thread: slot filled (code, our module, the test function in the walk), then dropped
    {
        StArg a{CreateEventW(nullptr, TRUE, FALSE, nullptr), {0}, 0};
        HANDLE th = CreateThread(nullptr, 0, &StThread, &a, 0, nullptr);
        WaitForSingleObject(th, 5000);
        const DWORD tid = GetThreadId(th);
        const Slot* s = SlotOf(tid);
        const bool filled = s && s->code == 0xC0000005 && s->depth > 2 && InStack(s, reinterpret_cast<const void*>(&StFault));
        const uint64_t handled0 = g_handled.load();
        for (int i = 0; i < 40 && s && s->state.load() != kDropped; ++i) Sleep(50);
        check(filled && s && s->state.load() == kDropped && g_handled.load() > handled0 && g_records.load() == 0,
              "crash record: a handled access violation fills a slot (code, stack with the faulting function) and is dropped by the late check");
        CloseHandle(th);
        CloseHandle(a.ev);
    }
    // (2) a thread whose exception filter blocks 2 s: written by the late check, naming the function and our code
    {
        g_stFilterMs.store(2000);
        StArg a{CreateEventW(nullptr, TRUE, FALSE, nullptr), {0}, 1};
        {
            // StFault's code counts as a site "patched by SelfTestKey" (nothing is written: only the range is recorded)
            DWORD64 image = 0;
            if (PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(&StFault), &image, nullptr)) {
                InstallOwnerScope scope("SelfTestKey");
                RecordOwnWrite(reinterpret_cast<const void*>(image + fe->BeginAddress), fe->EndAddress - fe->BeginAddress);
            }
        }
        HANDLE th = CreateThread(nullptr, 0, &StThread, &a, 0, nullptr);
        WaitForSingleObject(th, 8000);
        Sleep(200);
        static char buf[1 << 18];
        const size_t n = ReadAll(file, buf, sizeof buf);
        const bool rec = n && strstr(buf, "record: crash") && strstr(buf, "ACCESS_VIOLATION") && strstr(buf, "still inside") &&
                         strstr(buf, "# end") && strstr(buf, "events (");
        const bool owned = n && strstr(buf, "[patched by SelfTestKey]") && strstr(buf, "our code on this stack: SelfTestKey");
        uint32_t vms = 0, vls = 0;
        const bool ver = FileVersion(reinterpret_cast<const uint8_t*>(GetModuleHandleW(L"kernel32.dll")), &vms, &vls) && vms;
        const bool parts = n && strstr(buf, " code ") && strstr(buf, "|") && strstr(buf, "modules (") && strstr(buf, "installs (") &&
                           strstr(buf, "ini at start-up: crc32") && strstr(buf, "hang clock:") &&
                           strncmp(buf, "---- OblivionPerfFix summary: paste this ----\n", 46) == 0 && strstr(buf, "---- end of summary ----\n") &&
                           strstr(buf, "system: Windows ") && strstr(buf, "what: crash: ACCESS_VIOLATION");
        Log("[crash] self-test: kernel32 file version %u.%u.%u.%u, record parts (code bytes, modules, installs, ini, clock) %d", vms >> 16,
            vms & 0xFFFF, vls >> 16, vls & 0xFFFF, parts ? 1 : 0);
        VerStrings vs{};
        VersionStrings(reinterpret_cast<const uint8_t*>(GetModuleHandleW(L"kernel32.dll")), &vs);
        char cls[16], rel[200];
        PathClass(reinterpret_cast<const uint8_t*>(GetModuleHandleW(L"kernel32.dll")), cls, sizeof cls, rel, sizeof rel);
        Log("[crash] self-test: kernel32 product '%s', description '%s', file version '%s', loaded from %s: %s", vs.product, vs.desc,
            vs.fileVer, cls, rel);
        const bool strs = vs.product[0] && vs.desc[0] && vs.fileVer[0] && strcmp(cls, "System32") == 0 && _stricmp(rel, "kernel32.dll") == 0 &&
                          strstr(buf, "other DLLs in the game");
        check(ver && parts && strs, "crash record: code bytes, the module list with versions, product names and where each was loaded "
                                    "from, other DLLs, installs, ini CRC, clock, the summary block");
        check(rec && owned && g_records.load() == 1 && g_fileKept.load(),
              "crash record: an exception still in dispatch after the late-check time is written, naming the key whose code is on the stack");
        CloseHandle(th);
        CloseHandle(a.ev);
    }
    // (3) UE's fatal error: its message is copied
    {
        StArg a{nullptr, {0}, 2};
        HANDLE th = CreateThread(nullptr, 0, &StThread, &a, 0, nullptr);
        WaitForSingleObject(th, 5000);
        const Slot* s = SlotOf(GetThreadId(th));
        check(s && s->code == kUeAssert && strcmp(s->msg, "Fatal error: self-test message") == 0, "crash record: UE's fatal error 0x4000 copies its message");
        CloseHandle(th);
    }
    // (4) a fault inside the handler path does not recurse: a faulting snapshot (bad exception pointers) returns at once
    {
        EXCEPTION_RECORD er{};
        er.ExceptionCode = 0xC0000005;
        CONTEXT* badCtx = reinterpret_cast<CONTEXT*>(static_cast<uintptr_t>(0x10));
        EXCEPTION_POINTERS ep{&er, badCtx};
        const uint64_t before = g_fatalSeen.load();
        const LONG r = Handler(&ep);
        check(r == EXCEPTION_CONTINUE_SEARCH && g_fatalSeen.load() == before + 1 && !t_own,
              "crash record: a fault inside the handler is caught there, no recursion, CONTINUE_SEARCH");
        for (Slot& s : g_slots) if (s.tid == GetCurrentThreadId()) s.state.store(kFree);
    }
    // (5) the exit rules called directly (no termination): a probe place handled again and again is not the crash; a fresh
    // place on the calling thread is; once a record exists only the exit line is added
    {
        for (Slot& o : g_slots) if (o.state.load() == kPending) o.state.store(kDropped);
        Slot& probe = g_slots[kSlots - 1];
        Slot& real = g_slots[kSlots - 2];
        auto fake = [&](Slot& s, uint64_t addr, uint64_t age) {
            s.tid = 4242;
            s.qpc = Qpc() - age;
            s.code = 0xC0000005;
            s.addr = addr;
            s.depth = 0;
            s.nScan = 0;
            s.regMemOk = 0;
            s.msg[0] = 0;
            s.nParams = 0;
            s.sawLive = s.byLate = s.lateNoted = false;
            s.erAddr = s.ctxAddr = 0;
        };
        // the probe's place: seen and handled twice before
        const uint64_t probeAddr = reinterpret_cast<uint64_t>(&StFault) + 1;
        Site* site = NoteSite(0xC0000005, probeAddr);
        if (site) site->handled.store(5);
        fake(probe, probeAddr, 0);
        probe.state.store(kPending);
        g_fileKept.store(false);
        uint64_t r0 = g_records.load();
        ExitDecision(3, 4242);  // only the probe pending: an error exit with no crash seen -> every thread's stack (stage 3)
        const bool probeSkipped = probe.state.load() == kPending && g_records.load() == r0 + 1;
        g_fileKept.store(false);
        fake(real, reinterpret_cast<uint64_t>(&StFault) + 2, static_cast<uint64_t>(g_freq / 10));
        real.state.store(kPending);
        r0 = g_records.load();
        ExitDecision(3, 4242);
        const bool realWritten = real.state.load() == kWritten && probe.state.load() == kPending && g_records.load() == r0 + 1;
        r0 = g_records.load();
        fake(real, reinterpret_cast<uint64_t>(&StFault) + 3, 0);
        real.state.store(kPending);
        ExitDecision(3, 4242);  // a record exists: the exit line only
        const bool lineOnly = g_records.load() == r0 && real.state.load() == kPending;
        probe.state.store(kDropped);
        real.state.store(kDropped);
        g_fileKept.store(true);
        check(probeSkipped && realWritten && lineOnly, "crash record: exit rules: a probe place handled before is not taken for the crash, "
                                                       "a new place on the calling thread is, after a record only the exit line");
    }
    // M1: exact liveness: a handled exception's record is no longer intact once its storage is reused; a live one's walk passes
    // through its dispatcher frame
    {
        Slot s{};
        EXCEPTION_RECORD er{};
        CONTEXT cr{};
        er.ExceptionCode = 0xC0000005;
        er.ExceptionAddress = reinterpret_cast<void*>(0x1234);
        cr.Rip = 0x1234;
        cr.Rsp = 0x5678;
        s.code = 0xC0000005;
        s.addr = 0x1234;
        s.rip = 0x1234;
        s.rsp = 0x5678;
        s.erAddr = reinterpret_cast<uint64_t>(&er);
        s.ctxAddr = reinterpret_cast<uint64_t>(&cr);
        const bool intact = RecordIntact(s);
        er.ExceptionCode = 0xC0000094;  // the storage now holds another exception
        const bool reused = !RecordIntact(s);
        s.erAddr = 0x10;
        const bool badPtr = !RecordIntact(s);
        const uint64_t pc[3] = {0x1000, g_kiDispatcher + 4, 0x2000}, sp[3] = {0x100, s.ctxAddr, s.ctxAddr + 0x600};
        const uint64_t pc2[3] = {0x1000, g_kiDispatcher + 4, 0x2000}, sp2[3] = {0x100, s.ctxAddr + 0x40, s.ctxAddr + 0x600};
        const bool anc = AncestorIn(s, pc, sp, 3) && !AncestorIn(s, pc2, sp2, 3);
        check(intact && reused && badPtr && anc && g_kiDispatcher, "crash record: exact liveness (record intact, reused, bad pointer; the "
                                                                   "dispatcher frame owning the context)");
    }
    // M2: the Windows user name blanked in messages; S11: a modal loop pauses the hang clock (the real title-bar drag is not tested:
    // it cannot be made without input; posting WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE to a test window reaches it but does not
    // put a thread into Windows' move loop, so the GUI flags stay clear)
    {
        char out[200];
        BlankUser("Fatal error: [File:C:\\Users\\SomeName\\Saved\\x.ini] /Users/other/y and C:\\USERS\\Ab\\", out, sizeof out);
        const bool blank = strstr(out, "SomeName") == nullptr && strstr(out, "other") == nullptr && strstr(out, "\\Ab\\") == nullptr &&
                           strstr(out, "\\Users\\<user>\\Saved") != nullptr;
        HangClock c{};
        HangAdvance(c, 1, 0, 10000, 20000);
        int fired = 0;
        for (int i = 1; i <= 60; ++i) fired += HangAdvance(c, 1, i * 500.0, 10000, 20000, true) != 0;  // 30 s in a modal loop
        const bool paused = fired == 0 && c.openMs == 0;
        // user32 through GetProcAddress: this file is part of the DLL, which imports kernel32 only (self-test code included)
        HMODULE u32 = LoadLibraryW(L"user32.dll");
        using DefFn = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
        static DefFn s_def = nullptr;
        s_def = u32 ? reinterpret_cast<DefFn>(GetProcAddress(u32, "DefWindowProcW")) : nullptr;
        auto regClass = u32 ? reinterpret_cast<ATOM(WINAPI*)(const WNDCLASSW*)>(GetProcAddress(u32, "RegisterClassW")) : nullptr;
        auto unregClass = u32 ? reinterpret_cast<BOOL(WINAPI*)(LPCWSTR, HINSTANCE)>(GetProcAddress(u32, "UnregisterClassW")) : nullptr;
        auto create = u32 ? reinterpret_cast<HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID)>(
                                GetProcAddress(u32, "CreateWindowExW")) : nullptr;
        auto destroy = u32 ? reinterpret_cast<BOOL(WINAPI*)(HWND)>(GetProcAddress(u32, "DestroyWindow")) : nullptr;
        auto post = u32 ? reinterpret_cast<BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM)>(GetProcAddress(u32, "PostMessageW")) : nullptr;
        auto peek = u32 ? reinterpret_cast<BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT)>(GetProcAddress(u32, "PeekMessageW")) : nullptr;
        auto dispatch = u32 ? reinterpret_cast<LRESULT(WINAPI*)(const MSG*)>(GetProcAddress(u32, "DispatchMessageW")) : nullptr;
        struct W {
            static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
            {
                if (m == WM_ENTERSIZEMOVE) ++entered();
                if (m == WM_EXITSIZEMOVE) ++exited();
                return s_def ? s_def(h, m, w, l) : 0;
            }
            static int& entered() { static int n = 0; return n; }
            static int& exited() { static int n = 0; return n; }
        };
        bool flagsClear = false;
        if (s_def && regClass && unregClass && create && destroy && post && peek && dispatch) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = &W::Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"OPFCrashReportModalTest";
            regClass(&wc);
            HWND h = create(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);  // never shown
            if (h) {
                post(h, WM_ENTERSIZEMOVE, 0, 0);
                MSG m;
                while (peek(&m, h, 0, 0, PM_REMOVE)) dispatch(&m);
                flagsClear = !InModalLoop(GetCurrentThreadId());
                post(h, WM_EXITSIZEMOVE, 0, 0);
                while (peek(&m, h, 0, 0, PM_REMOVE)) dispatch(&m);
                destroy(h);
            }
            unregClass(wc.lpszClassName, wc.hInstance);
        }
        Log("[crash] self-test: blanked '%s'; modal test window entered %d exited %d, GUI flags clear %d", out, W::entered(), W::exited(), flagsClear ? 1 : 0);
        check(blank && paused && W::entered() == 1 && W::exited() == 1 && flagsClear,
              "crash record: user name blanked in messages; hang clock paused in a modal loop; the test window got the size-move messages");
    }
    // (6) owner lookup: a fake written range and a fake stub region
    {
        static uint8_t fakeSite[64];
        const char* owner = nullptr;
        uint32_t by = 0;
        {
            InstallOwnerScope scope("FakeSiteKey");
            RecordOwnWrite(fakeSite + 8, 6);
        }
        const int k1 = TryOwnerOf(fakeSite + 14, &owner, &by, nullptr);  // a return address just past the call
        const bool siteOk = k1 == 1 && owner && strcmp(owner, "FakeSiteKey") == 0 && by != 0;
        uint8_t* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        {
            InstallOwnerScope scope("FakeStubKey");
            RegisterOwnCode(stub);
        }
        const int k2 = TryOwnerOf(stub + 0x40, &owner, &by, nullptr);
        const bool stubOk = k2 == 2 && owner && strcmp(owner, "FakeStubKey") == 0;
        const int k3 = TryOwnerOf(fakeSite + 40, &owner, &by, nullptr);
        FreeOwnCode(stub);
        check(siteOk && stubOk && k3 == 0 && !CurrentInstallOwner(), "owner registry: a written range and a stub name their key; elsewhere none");
    }
    // a stack set: a thread blocked in its own function, taken while it waits (hang record's stacks), no suspension left behind
    {
        g_rows = static_cast<Row*>(VirtualAlloc(nullptr, sizeof(Row) * kMaxThreads, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        StArg a{CreateEventW(nullptr, TRUE, FALSE, nullptr), {0}, 3};
        HANDLE th = CreateThread(nullptr, 0, &StBlocked, &a, 0, nullptr);
        for (int i = 0; i < 200 && a.state.load() == 0; ++i) Sleep(5);
        Sleep(50);
        const int threads = TakeAllStacks();
        int found = 0, depth = 0;
        for (int i = 0; i < threads; ++i) {
            if (g_rows[i].tid != GetThreadId(th)) continue;
            depth = g_rows[i].depth;
            for (int k = 0; k < g_rows[i].depth; ++k) {
                DWORD64 image = 0;
                PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(g_rows[i].pc[k], &image, nullptr);
                if (fe && image + fe->BeginAddress == reinterpret_cast<uint64_t>(&StThread)) ++found;
            }
        }
        const DWORD susp = SuspendThread(th);
        ResumeThread(th);
        SetEvent(a.ev);
        const bool ended = WaitForSingleObject(th, 5000) == WAIT_OBJECT_0;
        Log("[crash] self-test: %d threads in the set, blocked thread depth %d, its function %d time(s), suspend count after %lu", threads, depth, found, susp);
        check(threads >= 3 && depth > 2 && found >= 1 && susp == 0 && ended, "hang record: every thread's stack, a blocked thread's own function in it, no suspension left");
        // a hang record through the writer (stacks, ring, the GPU part without a helper)
        const uint64_t r0 = g_records.load();
        WriteHang(1, 10.0, 123, 0, 0);
        static char buf[1 << 19];
        ReadAll(file, buf, sizeof buf);
        check(g_records.load() == r0 + 1 && strstr(buf, "record: hang") && strstr(buf, "game thread") && strstr(buf, "GPU: not asked"),
              "hang record: written with the stacks, the events and the GPU line");
        // a hang record written after the game was asked to quit says so (a shutdown wait), and one before does not
        check(!strstr(buf, "asked to quit"), "hang record: no shutdown note while no quit was asked");
        g_quitAnyQpc.store((Qpc() - 3 * g_freq) | 1);
        WriteHang(1, 10.0, 124, 0, 0);
        ReadAll(file, buf, sizeof buf);
        const char* note = strstr(buf, "note: written after the game was asked to quit (");
        check(note && strstr(note, "may be a shutdown wait"), "hang record: written after a quit request, it carries the shutdown-wait note");
        g_quitAnyQpc.store(0);
        CloseHandle(th);
        CloseHandle(a.ev);
    }
    // (7b) a written report is kept, and rotated to .1 at the next start
    {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
        const bool kept = GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES;
        OpenReportFile(testDir);
        const bool rotated = GetFileAttributesW(prev) != INVALID_FILE_ATTRIBUTES;
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
        const bool emptyGone = GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES;
        check(kept && rotated && emptyGone, "report file: a written report stays, moves to .1 at the next start, the new empty one goes");
    }
    g_lateCheckMs = 3000;
    g_liveCheckMs = 1000;
    g_crashOn.store(false);
    RemoveVectoredExceptionHandler(veh);
    return fail;
}

int StGpuFilter(EXCEPTION_POINTERS* e)
{
    const uint64_t at = reinterpret_cast<uint64_t>(e->ExceptionRecord->ExceptionAddress);
    Log("[crash] self-test: the GPU crash record faulted: 0x%08X at image+0x%llX (reading 0x%llX)", e->ExceptionRecord->ExceptionCode,
        static_cast<unsigned long long>(at - reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr))),
        static_cast<unsigned long long>(e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0));
    return EXCEPTION_EXECUTE_HANDLER;
}

// Self-test (pubdiagD): a GPU crash record (code 0x8000, this thread's stack) written to dir\OblivionPerfFix-crash.txt, after
// GpuCrashReportTestRecord gave the facts and the report text
bool CrashReportTestGpuRecord(const wchar_t* dir)
{
    if (!OpenReportFile(dir)) return false;
    static Slot s;
    memset(static_cast<void*>(&s), 0, sizeof s);
    s.code = kUeGpu;
    s.tid = GetCurrentThreadId();
    s.qpc = Qpc();
    RtlCaptureContext(&s.ctx);
    s.addr = s.ctx.Rip;
    uint64_t stop = 0;
    s.depth = WalkLive(&s.ctx, s.pc, s.sp, kWalk, &stop);
    s.state.store(kTaken);
    __try {
        WriteCrash(s, "self-test (a GPU crash record)", 0, false);
    } __except (StGpuFilter(GetExceptionInformation())) {
    }
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    return true;
}

}  // namespace opf
