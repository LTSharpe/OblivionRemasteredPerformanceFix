// ParallelFileReads ([Fixes] ParallelFileReads = N; 0 or 1 = the game's own reader, 2..16 = at most N reads of a game
// container file in flight; default 0 for now, to be played before it is switched on; live once installed): the engine's
// one-at-a-time container reader replaced by one that keeps several reads in flight.
//
// The problem. Everything the game streams in (packages, texture and virtual-texture data, Nanite pages) comes out of its
// container files (.ucas) through UE 5.3's IoStore file backend. On Windows that backend has one reader:
// FGenericFileIoStoreImpl::StartRequests (PakFile/Private/GenericPlatformIoDispatcher.cpp), called in a loop by the
// "IoService" thread. Each call takes ONE queued 256 KB block, reads it synchronously (seek, read, wait) and only then looks
// at the queue again. A read that is not in the Windows file cache takes 0.3-0.5 ms on an NVMe drive, so a burst of N such
// blocks costs N of those in a row, whatever the drive could do in parallel. Measured on the road to Bravil (lab runs
// vf-q1-Q1 / vf-q2-Q2, findings "vtfix"): when a newly loaded cell's materials first use their virtual textures, the render
// thread asks for the lowest-detail tile of each and waits for all of them (the engine's r.VT.SyncProduceLockedTiles 1, so
// that no texture is ever drawn as a flat colour). All 23-28 block requests are queued within 0.2 ms, the reader then needs
// 8-9 ms for them, and the render thread waits exactly that long: one 30-33 ms frame at a fixed place, on the first visit in
// every game session.
//
// The fix. The container file is opened for overlapped I/O (FWindowsPlatformFile::OpenReadNoBuffering passes
// FILE_FLAG_OVERLAPPED), so Windows accepts several reads on it at once. The replacement does what the original does, with
// the engine's own parts: the same queue Pop (the engine's order and priorities), the same read buffers from the engine's
// allocator, the same completed list and the same wake-up of the dispatcher thread. The one difference: it starts up to N
// reads before it waits, and waits for the first of them to finish (or for a new request) instead of for each in turn.
// The bytes read, where they land and everything done with them afterwards are the engine's. Blocks of one burst may reach
// the dispatcher in another order than they were started in; FFileIoStore::GetCompletedRequests handles each raw block on
// its own (a compressed block that spans two raw blocks is copied by its offset and counted down, UE 5.3
// IoDispatcherFileBackend.cpp), so the order does not matter. Like the original, a call returns only when no read of ours
// is in flight. Measured with 8 in flight (16 is no better): the wait at the waterfall 8.1 / 9.1 ms -> 2.7 / 1.3 ms and
// that frame 29.8 / 31.6 -> 26.2 / 24.2 ms on the short cold run (vf-p1-P8, vf-p2-P8b); on the whole ride (vf-f1-F1 /
// vf-g1-G8) the frame 30.7 -> 22.7 ms and a level package's block waits 9.2 ms instead of 19.0 ms (median).
//
// What it leaves to the engine's own reader (one call through, nothing else changed): the engine's raw-block cache on
// (s.IoDispatcherCacheSizeMB > 0, off in the game); a container on a hard disk (a drive that reports a seek penalty: there
// several reads in flight could keep an urgent block waiting behind seeks); a file handle that is not what is expected (see
// below); the key at 0 or 1.
//
// How it is found and checked. The whole original function is matched byte for byte (the displacements of its nine calls
// excepted; its six import calls must be KERNEL32's Enter / LeaveCriticalSection). Every offset used here appears in that
// code, except two:
//   the backend object: +0x08 the dispatcher wake-up delegate (+0x08 bound, +0x00 instance, its Execute at vtable +0x58),
//     +0x18 the buffer allocator (its lock at +0x18, its free list at +0x40; a buffer: +0x00 next, +0x08 memory), +0x20 the
//     block cache (+0x08 its memory, null when off), +0x30 the buffer taken in advance, +0x38 the completed list's lock,
//     +0x60 / +0x68 the list's head / tail
//   a read request: +0x00 next, +0x08 previous, +0x10 the container partition (+0x00 its IFileHandle*), +0x18 offset,
//     +0x20 size, +0x30 buffer, +0xC5 failed, +0xC6 cancelled
//   IFileHandle: Seek at vtable +0x10, Read at +0x20 (the original's own read, used here only as the fallback)
// The two that do not appear in it: the partition's file size (+0x08, UE 5.3 FFileIoStoreContainerFilePartition) and the
// Windows handle inside the engine's file object (FFileHandleWindows +0x08). Both are checked against Windows the first
// time each file object is seen: the handle must be a disk file of exactly that size; if not, that block is read the
// engine's way and the engine's reader takes over for good (one log line).
// Its only pointer in .rdata is its vtable slot; two slots further is ServiceNotify (mov rcx,[rcx+10h]; mov rax,[rcx];
// jmp [rax+10h]: "trigger the reader's event"), wrapped so that a new request also wakes a wait of ours. The queue's Pop is
// the call the original makes. Build 19115871: StartRequests exe+0x1CA2A80, its slot exe+0x70CC240, Pop exe+0x1C99090.
// Both vtable slots are plain pointer writes, done at start-up on either route (version.dll or OBSE64: the reader thread
// fetches the slot at every call); with the key at 0 or 1 at start-up nothing is touched.
#include "common.h"

#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// FGenericFileIoStoreImpl::StartRequests, whole (build 19115871 exe+0x1CA2A80; 594 bytes; call displacements wild).
constexpr char kStartRequests[] =
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 48 83 79 30 00 4C 8B F2 48 8B F1 75 4F 48 "
    "8B 69 18 48 8D 5D 18 48 8B CB FF 15 ?? ?? ?? ?? 48 8B 7D 40 48 85 FF 75 1C 48 85 DB 74 09 48 8B "
    "CB FF 15 ?? ?? ?? ?? 45 33 F6 4C 89 76 30 32 C0 E9 EA 01 00 00 48 8B 07 48 89 45 40 48 85 DB 74 "
    "09 48 8B CB FF 15 ?? ?? ?? ?? 48 89 7E 30 49 8B CE E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84 BD 01 "
    "00 00 0F B6 80 C6 00 00 00 0A 87 C5 00 00 00 74 15 48 8D 5E 38 48 8B CB FF 15 ?? ?? ?? ?? 45 33 "
    "F6 E9 F4 00 00 00 48 8B 46 30 45 33 F6 48 89 47 30 4C 8B 56 20 4C 89 76 30 48 8B 47 30 48 8B 68 "
    "08 4D 39 72 08 74 6A 41 8B 42 18 4C 8B 5F 28 41 3B 42 44 74 5C 41 8B 4A 58 4D 8D 4A 48 4D 8B 41 "
    "08 FF C9 49 8B C3 48 C1 E8 20 48 6B D0 17 49 03 D3 48 23 D1 4D 85 C0 4D 0F 45 C8 41 8B 04 91 83 "
    "F8 FF 74 2D 49 8B 52 10 0F 1F 84 00 00 00 00 00 48 98 48 8D 0C 40 48 8D 0C CD 00 00 00 00 4C 39 "
    "1C 11 0F 84 84 00 00 00 8B 44 11 10 83 F8 FF 75 DF 48 8B 47 10 41 8B DE 4C 89 7C 24 40 4C 8B 38 "
    "C6 87 C5 00 00 00 01 8B C3 FF C3 83 F8 0A 7D 38 49 8B 07 49 8B CF 48 8B 57 18 FF 50 10 84 C0 74 "
    "E6 49 8B 07 48 8B D5 4C 8B 47 20 49 8B CF FF 50 20 84 C0 74 D2 44 88 B7 C5 00 00 00 48 8B D7 48 "
    "8B 4E 20 E8 ?? ?? ?? ?? 4C 8B 7C 24 40 48 8D 5E 38 48 8B CB FF 15 ?? ?? ?? ?? 48 8B 46 68 48 85 "
    "C0 74 64 48 89 38 48 8B 46 68 EB 62 48 03 D1 74 80 48 8B 52 08 48 85 D2 0F 84 73 FF FF FF 48 8B "
    "0A 48 8B 42 08 48 89 41 08 48 8B 4A 08 48 8B 02 48 89 01 49 8D 4A 60 48 89 0A 49 8B 42 68 48 89 "
    "42 08 48 89 51 08 48 8B 42 08 48 89 10 48 8B 4F 30 4D 8B 82 A0 00 00 00 48 8B 52 18 48 8B 49 08 "
    "E8 ?? ?? ?? ?? EB 86 48 89 7E 60 49 8B C6 48 89 47 08 48 89 7E 68 4C 89 37 48 85 DB 74 09 48 8B "
    "CB FF 15 ?? ?? ?? ?? 48 8B 46 08 44 39 70 08 74 03 4C 8B 30 49 8B 06 49 8B CE FF 50 58 B0 01 48 "
    "8B 5C 24 48 48 8B 6C 24 50 48 83 C4 20 41 5E 5F 5E C3";
constexpr size_t kPopCall = 113;  // the E8 after mov rcx,r14 (the queue): call FFileIoStoreRequestQueue::Pop
// the function's six import calls (FF 15 disp32), in order: the allocator's lock entered, left (no buffer), left; the
// completed list's lock entered (a cancelled request), entered, left
constexpr size_t kEnterCalls[] = {42, 152, 404}, kLeaveCalls[] = {65, 100, 545};
// IPlatformFileIoStore::ServiceNotify of the generic backend: mov rcx,[rcx+10h]; mov rax,[rcx]; jmp [rax+10h]
constexpr char kServiceNotify[] = "48 8B 49 10 48 8B 01 48 FF 60 10";
constexpr int kNotifySlot = 2;  // StartRequests, GetCompletedRequests, ServiceNotify
constexpr int kMaxReads = 16;
constexpr int kTries = 10;  // the engine's own number of tries for one block
constexpr int kKnownFiles = 64, kKnownVolumes = 8;

// offsets (see the file comment)
constexpr size_t kImplWake = 0x08, kImplAllocator = 0x18, kImplBlockCache = 0x20, kImplAcquired = 0x30, kImplDoneLock = 0x38,
                 kImplDoneHead = 0x60, kImplDoneTail = 0x68;
constexpr size_t kAllocLock = 0x18, kAllocFree = 0x40, kBufferNext = 0x00, kBufferMemory = 0x08, kCacheMemory = 0x08;
constexpr size_t kReqNext = 0x00, kReqPrev = 0x08, kReqPartition = 0x10, kReqOffset = 0x18, kReqSize = 0x20, kReqBuffer = 0x30,
                 kReqFailed = 0xC5, kReqCancelled = 0xC6;
constexpr size_t kPartFile = 0x00, kPartSize = 0x08, kFileOsHandle = 0x08;

using StartFn = bool (*)(void* impl, void* queue);
using PopFn = uint8_t* (*)(void* queue);
using NotifyFn = void (*)(void* impl);

StartFn g_original = nullptr;
PopFn g_pop = nullptr;
NotifyFn g_notifyOriginal = nullptr;
HANDLE g_wake = nullptr;           // set by the wrapped ServiceNotify: a request was queued or a buffer came back
std::atomic<int> g_want{0};        // the key, live
bool g_installed = false;

// Reader-thread state (the IoService thread only).
struct Slot {
    OVERLAPPED ov;
    HANDLE event;
    uint8_t* req;
    HANDLE file;
};
Slot g_slot[kMaxReads];
int g_busy = 0;             // reads in flight; 0 whenever a call returns
bool g_engineOnly = false;  // a check failed at run time: the engine's reader from here on
struct Known {
    const void* object;
    HANDLE os;
};
Known g_known[kKnownFiles];  // file objects already checked (the newest replaces the oldest when full)
int g_knownN = 0, g_knownNext = 0;
bool g_knownFullLogged = false;
struct Volume {
    wchar_t name[80];
    int penalty;
};
Volume g_volume[kKnownVolumes];  // the seek-penalty answer per volume
int g_volumeN = 0;

std::atomic<uint64_t> g_reads{0}, g_inFlightMost{0}, g_waits{0}, g_retried{0}, g_failed{0}, g_engineReads{0}, g_seekQueries{0};
std::atomic<int> g_state{0};  // for the log: 0 running, 1 block cache on, 2 hard disk, 3 unexpected file handle
uint64_t g_lastLogged = 0;

// self-test only (never set in the game): the next N completions count as failed; a forced seek-penalty answer (-2 = ask
// Windows); any handle accepted as a container file
int g_testFailCompletions = 0, g_testSeekPenalty = -2;
bool g_testAnyFile = false;

inline uint8_t*& Ptr(uint8_t* base, size_t off) { return *reinterpret_cast<uint8_t**>(base + off); }
inline uint64_t U64(const uint8_t* base, size_t off) { return *reinterpret_cast<const uint64_t*>(base + off); }

// The original's first step: the buffer it took in advance (kept across calls), else one from the allocator's free list.
uint8_t* AcquireBuffer(uint8_t* impl)
{
    if (uint8_t* buf = Ptr(impl, kImplAcquired)) return buf;
    uint8_t* alloc = Ptr(impl, kImplAllocator);
    CRITICAL_SECTION* cs = reinterpret_cast<CRITICAL_SECTION*>(alloc + kAllocLock);
    EnterCriticalSection(cs);
    uint8_t* buf = Ptr(alloc, kAllocFree);
    if (buf) Ptr(alloc, kAllocFree) = Ptr(buf, kBufferNext);
    LeaveCriticalSection(cs);
    Ptr(impl, kImplAcquired) = buf;
    return buf;
}

// CompletedRequests.Add(req), as the original does it.
void AppendCompleted(uint8_t* impl, uint8_t* req)
{
    CRITICAL_SECTION* cs = reinterpret_cast<CRITICAL_SECTION*>(impl + kImplDoneLock);
    EnterCriticalSection(cs);
    uint8_t* tail = Ptr(impl, kImplDoneTail);
    if (tail) Ptr(tail, kReqNext) = req;
    else Ptr(impl, kImplDoneHead) = req;
    Ptr(req, kReqPrev) = tail;
    Ptr(impl, kImplDoneTail) = req;
    Ptr(req, kReqNext) = nullptr;
    LeaveCriticalSection(cs);
}

// WakeUpDispatcherThreadDelegate->Execute(), as the original calls it (the delegate's instance, its slot +0x58).
void WakeDispatcher(uint8_t* impl)
{
    uint8_t* del = Ptr(impl, kImplWake);
    uint8_t* inst = *reinterpret_cast<int32_t*>(del + 8) != 0 ? Ptr(del, 0) : nullptr;
    using Fn = void (*)(void*);
    (*reinterpret_cast<Fn**>(inst))[0x58 / 8](inst);
}

// The original's own read of one block (block cache off): Seek + Read through the engine's file handle, ten tries.
// Only with nothing of ours in flight: the engine's handle waits on the file object itself.
void EngineRead(uint8_t* req)
{
    uint8_t* file = Ptr(Ptr(req, kReqPartition), kPartFile);
    uint8_t* dest = Ptr(Ptr(req, kReqBuffer), kBufferMemory);
    void** vt = *reinterpret_cast<void***>(file);
    using SeekFn = bool (*)(void*, int64_t);
    using ReadFn = bool (*)(void*, uint8_t*, int64_t);
    req[kReqFailed] = 1;
    for (int i = 0; i < kTries; ++i) {
        if (!reinterpret_cast<SeekFn>(vt[2])(file, static_cast<int64_t>(U64(req, kReqOffset)))) continue;
        if (!reinterpret_cast<ReadFn>(vt[4])(file, dest, static_cast<int64_t>(U64(req, kReqSize)))) continue;
        req[kReqFailed] = 0;
        break;
    }
    g_engineReads.fetch_add(1, std::memory_order_relaxed);
}

// 1 = the file is on a drive that reports a seek penalty (a hard disk), 0 = none, -1 = not known (then read our way).
// Asked once per volume.
int SeekPenalty(HANDLE file)
{
    if (g_testSeekPenalty != -2) return g_testSeekPenalty;
    wchar_t path[600];
    const DWORD n = GetFinalPathNameByHandleW(file, path, 600, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);  // \\?\Volume{guid}\...
    if (!n || n >= 600) return -1;
    wchar_t* end = wcschr(path + 4, L'\\');
    if (!end || end - path >= 80) return -1;
    *end = 0;  // the volume itself
    for (int i = 0; i < g_volumeN; ++i)
        if (wcscmp(g_volume[i].name, path) == 0) return g_volume[i].penalty;
    g_seekQueries.fetch_add(1, std::memory_order_relaxed);
    int penalty = -1;
    const HANDLE vol = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (vol != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        query.QueryType = PropertyStandardQuery;
        DEVICE_SEEK_PENALTY_DESCRIPTOR desc{};
        DWORD got = 0;
        if (DeviceIoControl(vol, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &desc, sizeof desc, &got, nullptr) && got >= sizeof desc)
            penalty = desc.IncursSeekPenalty ? 1 : 0;
        CloseHandle(vol);
    }
    if (g_volumeN < kKnownVolumes) {
        wcscpy_s(g_volume[g_volumeN].name, path);
        g_volume[g_volumeN++].penalty = penalty;
    }
    return penalty;
}

// The OS handle of a request's container file, checked the first time each engine file object is seen: a disk file of
// exactly the partition's size, not on a hard disk. nullptr = not to be read our way (*why: 2 hard disk, 3 unexpected).
HANDLE FileOf(uint8_t* req, int* why)
{
    uint8_t* part = Ptr(req, kReqPartition);
    uint8_t* object = part ? Ptr(part, kPartFile) : nullptr;
    *why = 3;
    if (!object) return nullptr;
    const HANDLE os = *reinterpret_cast<HANDLE*>(object + kFileOsHandle);
    for (int i = 0; i < g_knownN; ++i)
        if (g_known[i].object == object && g_known[i].os == os) return os;
    if (!g_testAnyFile) {
        LARGE_INTEGER size{};
        if (!os || os == INVALID_HANDLE_VALUE || GetFileType(os) != FILE_TYPE_DISK || !GetFileSizeEx(os, &size) ||
            static_cast<uint64_t>(size.QuadPart) != U64(part, kPartSize))
            return nullptr;
        if (SeekPenalty(os) == 1) {
            *why = 2;
            return nullptr;
        }
    }
    if (g_knownN < kKnownFiles) {
        g_known[g_knownN++] = Known{object, os};
    } else {
        if (!g_knownFullLogged) {
            g_knownFullLogged = true;
            Log("[fileread] more than %d container files in use: the oldest checked ones are checked again when they are next read", kKnownFiles);
        }
        g_known[g_knownNext] = Known{object, os};
        g_knownNext = (g_knownNext + 1) % kKnownFiles;
    }
    return os;
}

// One try of a read. 1 = in flight in the slot, 0 = read whole, -1 = failed.
int ReadOnce(Slot& s, uint8_t* req, HANDLE file)
{
    const uint64_t off = U64(req, kReqOffset);
    const DWORD size = static_cast<DWORD>(U64(req, kReqSize));
    memset(&s.ov, 0, sizeof s.ov);
    s.ov.Offset = static_cast<DWORD>(off);
    s.ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    s.ov.hEvent = s.event;
    DWORD got = 0;
    if (ReadFile(file, Ptr(Ptr(req, kReqBuffer), kBufferMemory), size, &got, &s.ov)) return got == size ? 0 : -1;
    return GetLastError() == ERROR_IO_PENDING ? 1 : -1;
}

// A read whose first try failed: the engine tries ten times in all; the other nine one at a time, waiting for each.
void Retry(Slot& s, uint8_t* req, HANDLE file)
{
    g_retried.fetch_add(1, std::memory_order_relaxed);
    for (int i = 1; i < kTries; ++i) {
        int r = ReadOnce(s, req, file);
        if (r == 1) {
            DWORD got = 0;
            r = GetOverlappedResult(file, &s.ov, &got, TRUE) && got == static_cast<DWORD>(U64(req, kReqSize)) ? 0 : -1;
        }
        if (r == 0) {
            req[kReqFailed] = 0;
            return;
        }
    }
    g_failed.fetch_add(1, std::memory_order_relaxed);  // stays failed: the dispatcher reports the read error, as with the original
}

// Hands the finished reads to the dispatcher. wait: first wait for one of them (or, with room for more, for the wrapped
// ServiceNotify).
int Harvest(uint8_t* impl, int reads, bool wait)
{
    HANDLE ev[kMaxReads + 1];
    int idx[kMaxReads], n = 0;
    for (int i = 0; i < kMaxReads; ++i)
        if (g_slot[i].req) {
            ev[n] = g_slot[i].event;
            idx[n++] = i;
        }
    if (!n) return 0;
    if (wait) {
        DWORD total = static_cast<DWORD>(n);
        if (g_wake && g_busy < reads) ev[total++] = g_wake;  // room for more: a new request ends the wait too
        g_waits.fetch_add(1, std::memory_order_relaxed);
        WaitForMultipleObjects(total, ev, FALSE, INFINITE);
    }
    int done = 0;
    for (int k = 0; k < n; ++k) {
        Slot& s = g_slot[idx[k]];
        if (!HasOverlappedIoCompleted(&s.ov)) continue;
        uint8_t* req = s.req;
        DWORD got = 0;
        bool ok = GetOverlappedResult(s.file, &s.ov, &got, FALSE) && got == static_cast<DWORD>(U64(req, kReqSize));
        if (ok && g_testFailCompletions > 0) {
            --g_testFailCompletions;
            ok = false;
        }
        if (ok) req[kReqFailed] = 0;
        else Retry(s, req, s.file);
        AppendCompleted(impl, req);
        s.req = nullptr;
        --g_busy;
        ++done;
    }
    if (done) WakeDispatcher(impl);
    return done;
}

void Drain(uint8_t* impl)
{
    while (g_busy) Harvest(impl, kMaxReads, true);
}

void NoteState(int state)
{
    if (g_state.exchange(state, std::memory_order_relaxed) == state) return;
    if (state == 1) Log("[fileread] the engine's block cache is on (s.IoDispatcherCacheSizeMB): its own reader is used while it is");
    else if (state == 2) Log("[fileread] a game container is on a hard disk (the drive reports a seek penalty): the engine's own reader is used");
    else if (state == 3) Log("[fileread] a container's file handle is not what was expected: the engine's own reader is used from here on");
}

// The replacement of FGenericFileIoStoreImpl::StartRequests. true = something was started or completed (the caller calls
// again); false = nothing to do (the caller waits for the reader's event, as with the original). Like the original it
// returns only with nothing in flight: every block taken from the queue is on the completed list by then.
bool StartRequests(uint8_t* impl, void* queue)
{
    int reads = g_want.load(std::memory_order_relaxed);
    const bool blockCache = Ptr(Ptr(impl, kImplBlockCache), kCacheMemory) != nullptr;
    if (reads < 2 || g_engineOnly || blockCache) {
        if (blockCache && reads >= 2 && !g_engineOnly) NoteState(1);
        Drain(impl);  // a guard: nothing of ours may be in flight while the engine reads through its handle
        return g_original(impl, queue);
    }
    bool any = false;
    for (;;) {
        reads = g_want.load(std::memory_order_relaxed);
        if (reads > kMaxReads) reads = kMaxReads;
        if (reads < 2) {  // switched off meanwhile: finish what is in flight; the next call is the engine's reader
            Drain(impl);
            return true;
        }
        bool started = false;
        while (g_busy < reads) {
            uint8_t* buf = AcquireBuffer(impl);
            if (!buf) break;
            uint8_t* req = g_pop(queue);
            if (!req) break;
            started = any = true;
            if (req[kReqCancelled] | req[kReqFailed]) {  // as the original: straight to the dispatcher, the buffer kept for the next
                AppendCompleted(impl, req);
                WakeDispatcher(impl);
                continue;
            }
            Ptr(req, kReqBuffer) = buf;
            Ptr(impl, kImplAcquired) = nullptr;
            int why = 0;
            const HANDLE file = FileOf(req, &why);
            if (!file) {  // this block the engine's way, and its reader from here on
                Drain(impl);
                EngineRead(req);
                AppendCompleted(impl, req);
                WakeDispatcher(impl);
                g_engineOnly = true;
                NoteState(why);
                return true;
            }
            Slot* s = nullptr;
            for (int i = 0; i < kMaxReads; ++i)
                if (!g_slot[i].req) {
                    s = &g_slot[i];
                    break;
                }
            req[kReqFailed] = 1;  // as the original: failed until read
            g_reads.fetch_add(1, std::memory_order_relaxed);
            const int r = ReadOnce(*s, req, file);
            if (r == 1) {
                s->req = req;
                s->file = file;
                ++g_busy;
                if (static_cast<uint64_t>(g_busy) > g_inFlightMost.load(std::memory_order_relaxed))
                    g_inFlightMost.store(static_cast<uint64_t>(g_busy), std::memory_order_relaxed);
                continue;
            }
            if (r == 0) req[kReqFailed] = 0;
            else Retry(*s, req, file);
            AppendCompleted(impl, req);
            WakeDispatcher(impl);
        }
        if (!g_busy) return any;
        // reads in flight: hand over what has finished; if this round started nothing, wait for one (or for a new request)
        if (Harvest(impl, reads, !started) > 0) any = true;
    }
}

bool StartRequestsHook(void* impl, void* queue) { return StartRequests(static_cast<uint8_t*>(impl), queue); }

void ServiceNotifyHook(void* impl)
{
    g_notifyOriginal(impl);
    SetEvent(g_wake);
}

}  // namespace

bool FindParallelFileReads(const GameModule& game, ParallelFileReadsInfo* info)
{
    *info = ParallelFileReadsInfo{};
    const auto fn = FindAll(game.text, ParsePattern(kStartRequests));
    if (fn.size() != 1) return false;
    // its six import calls: KERNEL32's EnterCriticalSection / LeaveCriticalSection
    void** const enter = FindImportSlot(game.base, "KERNEL32.dll", "EnterCriticalSection");
    void** const leave = FindImportSlot(game.base, "KERNEL32.dll", "LeaveCriticalSection");
    if (!enter || !leave) return false;
    for (size_t off : kEnterCalls)
        if (RipTarget(fn[0] + off, 2, 6) != reinterpret_cast<uint8_t*>(enter)) return false;
    for (size_t off : kLeaveCalls)
        if (RipTarget(fn[0] + off, 2, 6) != reinterpret_cast<uint8_t*>(leave)) return false;
    void** slot = nullptr;
    int slots = 0;
    const uintptr_t want = reinterpret_cast<uintptr_t>(fn[0]);
    for (uint8_t* p = game.rdata.begin; p + 8 <= game.rdata.end; p += 8)
        if (*reinterpret_cast<const uintptr_t*>(p) == want) {
            slot = reinterpret_cast<void**>(p);
            ++slots;
        }
    if (slots != 1 || reinterpret_cast<uint8_t*>(slot + kNotifySlot + 1) > game.rdata.end) return false;
    uint8_t* const notify = reinterpret_cast<uint8_t*>(slot[kNotifySlot]);
    if (!game.text.Contains(notify) || !MatchesAt(notify, ParsePattern(kServiceNotify))) return false;
    uint8_t* const pop = RipTarget(fn[0] + kPopCall, 1, 5);
    if (!game.text.Contains(pop)) return false;
    info->startRequests = fn[0];
    info->slot = slot;
    info->pop = pop;
    info->serviceNotify = notify;
    return true;
}

// Start-up (either route), once: with reads >= 2 the two vtable slots are replaced. The key is live afterwards
// (SetParallelFileReads); with 0 or 1 at start-up nothing is touched and a later change takes a restart.
bool InstallParallelFileReads(int reads)
{
    if (g_installed) return true;
    if (reads < 2) {
        Log("[fileread] ParallelFileReads off: the game's container files are read one block at a time (takes a restart to switch on)");
        return false;
    }
    const GameModule& game = Game();
    ParallelFileReadsInfo info;
    if (!FindParallelFileReads(game, &info)) {
        Log("[fileread] the engine's container reader is not the code ParallelFileReads was written for: not installed");
        return false;
    }
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = g_wake != nullptr;
    for (int i = 0; ok && i < kMaxReads; ++i) {
        g_slot[i].event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ok = g_slot[i].event != nullptr;
    }
    if (!ok) {
        Log("[fileread] ParallelFileReads: no events: not installed");
        return false;
    }
    g_original = reinterpret_cast<StartFn>(info.startRequests);
    g_pop = reinterpret_cast<PopFn>(info.pop);
    g_notifyOriginal = reinterpret_cast<NotifyFn>(info.serviceNotify);
    g_want.store(reads > kMaxReads ? kMaxReads : reads, std::memory_order_relaxed);
    // ServiceNotify first: once StartRequests is ours, every new request must reach g_wake.
    if (!WritePointer(info.slot + kNotifySlot, reinterpret_cast<void*>(&ServiceNotifyHook)) ||
        !WritePointer(info.slot, reinterpret_cast<void*>(&StartRequestsHook))) {
        Log("[fileread] ParallelFileReads: could not replace the reader's vtable slots: not installed");
        return false;
    }
    g_installed = true;
    Log("[fileread] ParallelFileReads: up to %d reads of a container file in flight (the engine's reader exe+0x%X matched whole, its "
        "slot exe+0x%X, queue Pop exe+0x%X)",
        g_want.load(), static_cast<unsigned>(info.startRequests - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(info.slot) - game.base), static_cast<unsigned>(info.pop - game.base));
    return true;
}

// Live (once a second): 0 or 1 = the engine's own reader (after the reads in flight have finished), 2..16 = that many in flight.
void SetParallelFileReads(int reads)
{
    if (!g_installed) return;
    if (reads > kMaxReads) reads = kMaxReads;
    if (reads < 0) reads = 0;
    const int was = g_want.exchange(reads, std::memory_order_relaxed);
    if (was != reads) Log("[fileread] ParallelFileReads %d -> %d%s", was, reads, reads < 2 ? " (the engine's own reader)" : "");
}

// Once a minute: one line when blocks were read since the last one.
void LogParallelFileReadsStats()
{
    if (!g_installed) return;
    const uint64_t reads = g_reads.load(std::memory_order_relaxed);
    if (reads == g_lastLogged) return;
    g_lastLogged = reads;
    Log("[fileread] %llu container blocks read so far, up to %llu in flight, %llu waits; first try failed %llu times (%llu blocks failed "
        "after ten tries), %llu read the engine's way",
        static_cast<unsigned long long>(reads), static_cast<unsigned long long>(g_inFlightMost.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_waits.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_retried.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_failed.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_engineReads.load(std::memory_order_relaxed)));
}

// ---- self-test only: the replacement run against stand-ins for the engine's objects ----
static void TestEvents()
{
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (int i = 0; i < kMaxReads; ++i)
        if (!g_slot[i].event) g_slot[i].event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

bool ParallelFileReadsTestStart(void* impl, void* queue, int reads, void* pop, void* original)
{
    TestEvents();
    g_pop = reinterpret_cast<PopFn>(pop);
    g_original = reinterpret_cast<StartFn>(original);
    g_want.store(reads, std::memory_order_relaxed);
    return StartRequests(static_cast<uint8_t*>(impl), queue);
}

void ParallelFileReadsTestNotify()
{
    if (g_wake) SetEvent(g_wake);
}

// The wrapped ServiceNotify itself: true = the original was called through and our wake event is set afterwards.
bool ParallelFileReadsTestNotifyHook(void* impl, void* original)
{
    TestEvents();
    g_notifyOriginal = reinterpret_cast<NotifyFn>(original);
    WaitForSingleObject(g_wake, 0);  // clear
    ServiceNotifyHook(impl);
    return WaitForSingleObject(g_wake, 0) == WAIT_OBJECT_0;
}

void ParallelFileReadsTestHooks(int failCompletions, int seekPenalty, bool anyFile)
{
    g_testFailCompletions = failCompletions;
    g_testSeekPenalty = seekPenalty;
    g_testAnyFile = anyFile;
}

int ParallelFileReadsTestBusy() { return g_busy; }

int ParallelFileReadsTestSeekPenalty(void* file) { return SeekPenalty(static_cast<HANDLE>(file)); }

void ParallelFileReadsTestReset()
{
    for (int i = 0; i < kMaxReads; ++i) g_slot[i].req = nullptr;
    g_busy = 0;
    g_engineOnly = false;
    g_knownN = g_knownNext = 0;
    g_knownFullLogged = false;
    g_volumeN = 0;
    g_state.store(0);
    g_reads.store(0);
    g_inFlightMost.store(0);
    g_waits.store(0);
    g_retried.store(0);
    g_failed.store(0);
    g_engineReads.store(0);
    g_seekQueries.store(0);
    g_testFailCompletions = 0;
    g_testSeekPenalty = -2;
    g_testAnyFile = false;
    if (g_wake) WaitForSingleObject(g_wake, 0);
}

void ParallelFileReadsTestCounts(uint64_t* reads, uint64_t* inFlightMost, uint64_t* retried, uint64_t* failed, uint64_t* engineReads, int* state,
                                 uint64_t* seekQueries)
{
    *reads = g_reads.load();
    *inFlightMost = g_inFlightMost.load();
    *retried = g_retried.load();
    *failed = g_failed.load();
    *engineReads = g_engineReads.load();
    *state = g_state.load();
    if (seekQueries) *seekQueries = g_seekQueries.load();
}

}  // namespace opf
