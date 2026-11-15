// ParallelFileReads self-test (vtfix 2026-11-14; extended after review rev99a), included by selftest.cpp.
// (1) On the mapped exe: the engine's container reader FGenericFileIoStoreImpl::StartRequests matched whole and once (its
//     six import calls KERNEL32's Enter / LeaveCriticalSection), its one vtable slot, the queue Pop it calls and the
//     ServiceNotify two slots further (Steam 19115871: exe+0x1CA2A80, slot exe+0x70CC240, Pop exe+0x1C99090, ServiceNotify
//     exe+0xE07450). Nothing is patched.
// (2) The replacement run against stand-ins laid out like the engine's objects (the offsets the original's code uses) and a
//     real temporary file: opened the engine's way (FILE_FLAG_OVERLAPPED), and a second time without buffering so that reads
//     really stay in flight. Checked: every block's bytes are the file's; each request reaches the completed list exactly
//     once with consistent links; a call never returns with a read in flight; several reads were in flight at once; a
//     cancelled request passes through unread; a read past the end of the file stays failed after ten tries; a read whose
//     completion fails is read again; with two buffers for six requests the reader waits for buffers to come back; block
//     cache on and the key at 1 go to the original; a file handle that is not a disk file of the partition's size is read
//     through the engine's file object once, with nothing of ours in flight at that moment, and the original is used from
//     then on; a hard disk likewise; more file objects than the table of checked ones holds; the wrapped ServiceNotify calls
//     the original and sets the wake event; a request that arrives while a read is in flight is read before that read ends
//     (a pipe stands in for a slow read).
#pragma once

namespace filereads_test {

struct FakeBuffer {
    FakeBuffer* next;
    uint8_t* memory;
};
struct FakeAllocator {
    uint8_t pad[0x18];
    CRITICAL_SECTION lock;
    FakeBuffer* firstFree;
};
struct FakeBlockCache {
    void* unused;
    uint8_t* memory;
};
struct FakeDelegateInstance {
    void** vtable;
    int calls;
};
struct FakeDelegate {
    FakeDelegateInstance* instance;
    int32_t size;
};
struct FakeFile {
    void** vtable;
    HANDLE os;
    int64_t pos;
    int seeks, reads, busyAtRead;
};
struct FakePartition {
    FakeFile* file;
    uint64_t fileSize;
};
struct FakeRequest {
    FakeRequest* next;
    FakeRequest* prev;
    FakePartition* partition;
    uint64_t offset;
    uint64_t size;
    uint64_t key;
    FakeBuffer* buffer;
    uint8_t pad[0xC5 - 0x38];
    uint8_t failed;
    uint8_t cancelled;
    uint8_t pad2[9];
};
struct FakeImpl {
    void* vtable;
    FakeDelegate* wake;
    void* serviceEvent;
    FakeAllocator* allocator;
    FakeBlockCache* blockCache;
    void* stats;
    FakeBuffer* acquired;
    CRITICAL_SECTION doneLock;
    FakeRequest* head;
    FakeRequest* tail;
};
static_assert(offsetof(FakeAllocator, lock) == 0x18 && offsetof(FakeAllocator, firstFree) == 0x40, "allocator layout");
static_assert(offsetof(FakeImpl, wake) == 0x08 && offsetof(FakeImpl, allocator) == 0x18 && offsetof(FakeImpl, blockCache) == 0x20 &&
              offsetof(FakeImpl, acquired) == 0x30 && offsetof(FakeImpl, doneLock) == 0x38 && offsetof(FakeImpl, head) == 0x60 &&
              offsetof(FakeImpl, tail) == 0x68, "backend layout");
static_assert(offsetof(FakeRequest, partition) == 0x10 && offsetof(FakeRequest, offset) == 0x18 && offsetof(FakeRequest, size) == 0x20 &&
              offsetof(FakeRequest, buffer) == 0x30 && offsetof(FakeRequest, failed) == 0xC5 && offsetof(FakeRequest, cancelled) == 0xC6,
              "request layout");

constexpr uint32_t kBlock = 256 * 1024;
inline uint8_t ByteAt(uint64_t i) { return static_cast<uint8_t>(((i * 0x9E3779B1ull) >> 24) ^ (i >> 12)); }

struct FakeQueue {
    CRITICAL_SECTION lock;
    std::vector<FakeRequest*> items;
    size_t next = 0;
};
static uint8_t* Pop(void* queue)
{
    FakeQueue* q = static_cast<FakeQueue*>(queue);
    EnterCriticalSection(&q->lock);
    FakeRequest* r = q->next < q->items.size() ? q->items[q->next++] : nullptr;
    LeaveCriticalSection(&q->lock);
    return reinterpret_cast<uint8_t*>(r);
}
static int g_originalCalls = 0, g_originalBusy = 0;
static bool Original(void*, void*)
{
    ++g_originalCalls;
    g_originalBusy += opf::ParallelFileReadsTestBusy();
    return false;
}
static int g_notifyCalls = 0;
static void NotifyOriginal(void*) { ++g_notifyCalls; }
static void DelegateExecute(void* self) { ++static_cast<FakeDelegateInstance*>(self)->calls; }
// the engine's IFileHandle: Seek at vtable slot 2, Read at slot 4
static bool FileSeek(void* self, int64_t pos)
{
    FakeFile* f = static_cast<FakeFile*>(self);
    f->pos = pos;
    ++f->seeks;
    return true;
}
static bool FileRead(void* self, uint8_t* dest, int64_t bytes)
{
    FakeFile* f = static_cast<FakeFile*>(self);
    ++f->reads;
    f->busyAtRead += opf::ParallelFileReadsTestBusy();  // must be 0: the engine's handle waits on the file object itself
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(f->pos);
    ov.OffsetHigh = static_cast<DWORD>(f->pos >> 32);
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD got = 0;
    BOOL ok = ReadFile(f->os, dest, static_cast<DWORD>(bytes), &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(f->os, &ov, &got, TRUE);
    CloseHandle(ov.hEvent);
    return ok && got == static_cast<DWORD>(bytes);
}

struct World {
    FakeImpl impl{};
    FakeAllocator allocator{};
    FakeBlockCache cache{};
    FakeDelegateInstance delegateInstance{};
    FakeDelegate delegate{};
    void* delegateVtable[12]{};
    void* fileVtable[5]{};
    std::vector<FakeFile> files;
    std::vector<FakePartition> partitions;
    std::vector<FakeBuffer> buffers;
    std::vector<FakeRequest> requests;
    FakeQueue queue;

    // fileCount file objects, all on the same OS handle (each with its own partition)
    void Init(HANDLE os, uint64_t fileSize, int bufferCount, int fileCount = 1)
    {
        InitializeCriticalSection(&allocator.lock);
        InitializeCriticalSection(&impl.doneLock);
        InitializeCriticalSection(&queue.lock);
        delegateVtable[11] = reinterpret_cast<void*>(&DelegateExecute);  // the slot +0x58 the original calls
        delegateInstance.vtable = delegateVtable;
        delegate.instance = &delegateInstance;
        delegate.size = 1;
        fileVtable[2] = reinterpret_cast<void*>(&FileSeek);
        fileVtable[4] = reinterpret_cast<void*>(&FileRead);
        files.assign(static_cast<size_t>(fileCount), FakeFile{});
        partitions.assign(static_cast<size_t>(fileCount), FakePartition{});
        for (int i = 0; i < fileCount; ++i) {
            files[static_cast<size_t>(i)].vtable = fileVtable;
            files[static_cast<size_t>(i)].os = os;
            partitions[static_cast<size_t>(i)].file = &files[static_cast<size_t>(i)];
            partitions[static_cast<size_t>(i)].fileSize = fileSize;
        }
        buffers.resize(static_cast<size_t>(bufferCount));
        for (int i = 0; i < bufferCount; ++i) {
            buffers[static_cast<size_t>(i)].memory = static_cast<uint8_t*>(_aligned_malloc(kBlock, 4096));  // as the engine's: page-aligned
            memset(buffers[static_cast<size_t>(i)].memory, 0xEE, kBlock);
            buffers[static_cast<size_t>(i)].next = allocator.firstFree;
            allocator.firstFree = &buffers[static_cast<size_t>(i)];
        }
        impl.wake = &delegate;
        impl.allocator = &allocator;
        impl.blockCache = &cache;
    }
    // ranges: offset, size, index of the file object
    void SetRequests(const std::vector<std::tuple<uint64_t, uint64_t, int>>& ranges, size_t queued = SIZE_MAX)
    {
        requests.assign(ranges.size(), FakeRequest{});
        queue.items.clear();
        queue.next = 0;
        for (size_t i = 0; i < ranges.size(); ++i) {
            requests[i].partition = &partitions[static_cast<size_t>(std::get<2>(ranges[i]))];
            requests[i].offset = std::get<0>(ranges[i]);
            requests[i].size = std::get<1>(ranges[i]);
            if (i < queued) queue.items.push_back(&requests[i]);
        }
    }
    void Push(FakeRequest* r)
    {
        EnterCriticalSection(&queue.lock);
        queue.items.push_back(r);
        LeaveCriticalSection(&queue.lock);
    }
    // What the dispatcher does with the completed list: takes it, and gives each block's buffer back (with the notify that
    // FFileIoStore::FreeBuffer makes). Returns the requests taken; *linksOk = the list was consistent. A block with wrong
    // bytes gets failed = 2 (the test's mark).
    std::vector<FakeRequest*> TakeCompleted(bool* linksOk)
    {
        std::vector<FakeRequest*> out;
        EnterCriticalSection(&impl.doneLock);
        FakeRequest* r = impl.head;
        FakeRequest* prev = nullptr;
        bool ok = true;
        while (r) {
            if (r->prev != prev) ok = false;
            out.push_back(r);
            prev = r;
            r = r->next;
        }
        if (impl.tail != prev) ok = false;
        impl.head = impl.tail = nullptr;
        LeaveCriticalSection(&impl.doneLock);
        if (linksOk && !ok) *linksOk = false;
        for (FakeRequest* q : out) {
            if (!q->failed && q->buffer) {
                for (uint64_t i = 0; i < q->size; i += 97)
                    if (q->buffer->memory[i] != ByteAt(q->offset + i)) { q->failed = 2; break; }
                if (q->buffer->memory[q->size - 1] != ByteAt(q->offset + q->size - 1)) q->failed = 2;
            }
            if (q->buffer) {
                EnterCriticalSection(&allocator.lock);
                q->buffer->next = allocator.firstFree;
                allocator.firstFree = q->buffer;
                LeaveCriticalSection(&allocator.lock);
                opf::ParallelFileReadsTestNotify();
            }
        }
        return out;
    }
    ~World()
    {
        for (FakeBuffer& b : buffers) _aligned_free(b.memory);
        DeleteCriticalSection(&allocator.lock);
        DeleteCriticalSection(&impl.doneLock);
        DeleteCriticalSection(&queue.lock);
    }
};

// Calls the replacement until it has nothing to do, playing the dispatcher between calls. *busyAtReturn sums the reads in
// flight at each return (must stay 0).
static void Run(World& w, int reads, std::vector<FakeRequest*>* done, bool* linksOk, int* busyAtReturn)
{
    for (int calls = 0; calls < 100000; ++calls) {
        const bool more = opf::ParallelFileReadsTestStart(&w.impl, &w.queue, reads, reinterpret_cast<void*>(&Pop), reinterpret_cast<void*>(&Original));
        *busyAtReturn += opf::ParallelFileReadsTestBusy();
        const auto got = w.TakeCompleted(linksOk);
        done->insert(done->end(), got.begin(), got.end());
        if (!more && got.empty()) break;
    }
}

struct PipeHelper {
    World* world;
    FakeRequest* late;
    HANDLE client;
    uint64_t pipeOffset;
};
static DWORD WINAPI PipeThread(void* p)
{
    PipeHelper* h = static_cast<PipeHelper*>(p);
    Sleep(60);  // the reader is waiting for the pipe read by now
    h->world->Push(h->late);
    opf::ParallelFileReadsTestNotify();  // as the engine's ServiceNotify after a push
    Sleep(100);  // time for the late request to be read while the pipe read is still in flight
    std::vector<uint8_t> data(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) data[i] = ByteAt(h->pipeOffset + i);
    DWORD wrote = 0;
    WriteFile(h->client, data.data(), kBlock, &wrote, nullptr);
    return 0;
}

}  // namespace filereads_test

static void FileReadsRun(const GameModule& game)
{
    using namespace filereads_test;
    ParallelFileReadsInfo info;
    const bool found = FindParallelFileReads(game, &info);
    Check(found, "filereads: the engine's container reader matched whole and once (its import calls Enter / LeaveCriticalSection), its one vtable slot, Pop and ServiceNotify found");
    if (found && game.timeDateStamp == 0xF19077A4u)
        Check(info.startRequests - game.base == 0x1CA2A80 && reinterpret_cast<uint8_t*>(info.slot) - game.base == 0x70CC240 &&
                  info.pop - game.base == 0x1C99090 && info.serviceNotify - game.base == 0xE07450,
              "filereads: StartRequests exe+0x1CA2A80, slot exe+0x70CC240, Pop exe+0x1C99090, ServiceNotify exe+0xE07450 (Steam 19115871)");

    // a real file of 20 blocks + 12345 bytes with known bytes
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"opf", 0, path);
    const uint64_t fileSize = 20ull * kBlock + 12345;
    {
        const HANDLE w = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::vector<uint8_t> chunk(kBlock);
        for (uint64_t at = 0; at < fileSize; at += kBlock) {
            const DWORD n = static_cast<DWORD>(fileSize - at < kBlock ? fileSize - at : kBlock);
            for (DWORD i = 0; i < n; ++i) chunk[i] = ByteAt(at + i);
            DWORD wrote = 0;
            WriteFile(w, chunk.data(), n, &wrote, nullptr);
        }
        FlushFileBuffers(w);
        CloseHandle(w);
    }
    // the engine's way (buffered, overlapped), and without buffering: those reads go to the drive and stay in flight
    const HANDLE os = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    const HANDLE raw = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, nullptr);
    Check(os != INVALID_HANDLE_VALUE && raw != INVALID_HANDLE_VALUE, "filereads: test file written and opened overlapped (buffered and unbuffered)");
    if (os == INVALID_HANDLE_VALUE || raw == INVALID_HANDLE_VALUE) return;
    uint64_t nReads = 0, most = 0, retried = 0, failed = 0, engine = 0, seekQueries = 0;
    int state = 0;
    // each case starts clean; the drive's answer is fixed at "no seek penalty" except where a case sets it (the real question
    // is asked once, below), so the cases do not depend on what the temporary folder's drive is
    const auto fresh = [](int failCompletions = 0, int seekPenalty = 0, bool anyFile = false) {
        ParallelFileReadsTestReset();
        ParallelFileReadsTestHooks(failCompletions, seekPenalty, anyFile);
    };
    const auto allGood = [](const std::vector<FakeRequest*>& done) {
        for (FakeRequest* r : done) if (r->failed) return false;
        return true;
    };

    {  // 21 blocks (the last one short), 32 buffers, 8 in flight, the engine's kind of handle
        fresh();
        World w;
        w.Init(os, fileSize, 32);
        std::vector<std::tuple<uint64_t, uint64_t, int>> ranges;
        for (int i = 20; i >= 0; --i) ranges.push_back({static_cast<uint64_t>(i) * kBlock, i == 20 ? 12345ull : kBlock, 0});
        w.SetRequests(ranges);
        std::vector<FakeRequest*> done;
        bool links = true;
        int busy = 0;
        g_originalCalls = 0;
        Run(w, 8, &done, &links, &busy);
        bool allOnce = done.size() == 21;
        std::vector<int> seen(21, 0);
        for (FakeRequest* r : done) ++seen[static_cast<size_t>(r - w.requests.data())];
        for (int s : seen) if (s != 1) allOnce = false;
        ParallelFileReadsTestCounts(&nReads, &most, &retried, &failed, &engine, &state);
        Check(allOnce && links && busy == 0, "filereads: 21 blocks, 8 in flight: each request completed exactly once, the completed list consistent, no call returned with a read in flight");
        Check(allGood(done), "filereads: every block holds the file's bytes at its offset (the short last block too), none failed");
        Check(nReads == 21 && most <= 8 && retried == 0 && failed == 0 && engine == 0 && w.delegateInstance.calls >= 1 && g_originalCalls == 0 &&
                  w.files[0].reads == 0,
              "filereads: 21 reads of ours, at most 8 in flight, no retry, the dispatcher woken, the engine's reader and file object not used");
        printf("      (filereads: buffered handle, most in flight: %llu)\n", static_cast<unsigned long long>(most));
    }
    {  // 16 whole blocks without buffering: the reads go to the drive, so several are in flight at once
        fresh();
        World w;
        w.Init(raw, fileSize, 32);
        std::vector<std::tuple<uint64_t, uint64_t, int>> ranges;
        for (int i = 0; i < 16; ++i) ranges.push_back({static_cast<uint64_t>((i * 7) % 20) * kBlock, kBlock, 0});
        w.SetRequests(ranges);
        std::vector<FakeRequest*> done;
        bool links = true;
        int busy = 0;
        Run(w, 8, &done, &links, &busy);
        ParallelFileReadsTestCounts(&nReads, &most, &retried, &failed, &engine, &state);
        Check(done.size() == 16 && links && busy == 0 && allGood(done) && most >= 2 && most <= 8,
              "filereads: 16 blocks read from the drive: several reads were in flight at once (2 to 8), all bytes right");
        printf("      (filereads: unbuffered handle, most in flight: %llu)\n", static_cast<unsigned long long>(most));
    }
    {  // a completion that fails is read again (twice here), and the block is good
        fresh(2);
        World w;
        w.Init(raw, fileSize, 8);
        std::vector<std::tuple<uint64_t, uint64_t, int>> ranges;
        for (int i = 0; i < 6; ++i) ranges.push_back({static_cast<uint64_t>(i * 2) * kBlock, kBlock, 0});
        w.SetRequests(ranges);
        std::vector<FakeRequest*> done;
        bool links = true;
        int busy = 0;
        Run(w, 8, &done, &links, &busy);
        ParallelFileReadsTestCounts(&nReads, &most, &retried, &failed, &engine, &state);
        Check(done.size() == 6 && links && busy == 0 && allGood(done) && retried == 2 && failed == 0,
              "filereads: two reads whose completion failed were read again and are good");
    }
    {  // a cancelled request passes through unread; the buffer taken for it serves the next request
        fresh();
        World w;
        w.Init(os, fileSize, 1);
        w.SetRequests({{0, kBlock, 0}, {kBlock, kBlock, 0}});
        w.requests[0].cancelled = 1;
        std::vector<FakeRequest*> done;
        bool links = true;
        int busy = 0;
        Run(w, 8, &done, &links, &busy);
        ParallelFileReadsTestCounts(&nReads, &most, &retried, &failed, &engine, &state);
        Check(done.size() == 2 && links && w.requests[0].buffer == nullptr && w.requests[0].failed == 0 && w.requests[1].buffer == &w.buffers[0] &&
                  w.requests[1].failed == 0 && nReads == 1,
              "filereads: a cancelled request goes to the dispatcher unread and without a buffer; the one buffer serves the next request");
    }
    {  // a read past the end of the file: failed after ten tries, the others unaffected
        fresh();
        World w;
        w.Init(os, fileSize, 4);
        w.SetRequests({{0, kBlock, 0}, {40ull * kBlock, kBlock, 0}, {2ull * kBlock, kBlock, 0}});
        std::vector<FakeRequest*> done;
        bool links = true;
        int busy = 0;
        Run(w, 8, &done, &links, &busy);
        ParallelFileReadsTestCounts(&nReads, &most, &retried, &failed, &engine, &state);
        Check(done.size() == 3 && links && busy == 0 && w.requests[0].failed == 0 && w.requests[1].failed == 1 && w.requests[2].failed == 0 && retried == 1 &&
                  failed == 1,
              "filereads: a block past the end of the file stays failed after ten tries (as the engine's reader leaves it); its neighbours are read");
    }
    {  // two buffers for six requests: the reader waits for buffers to come back
        fresh();
        World w;
        w.Init(raw, fileSize, 2);
        std::vector<std::tuple<uint64_t, uint64_t, int>> ranges;
        for (int i = 0; i < 6; ++i) ranges.push_back({static_cast<uint64_t>(i * 3) * kBlock, kBlock, 0});
        w.SetRequests(ranges);
        std::vector<FakeRequest*> done;
        bool links = true;
        int busy = 0;
        Run(w, 8, &done, &links, &busy);
        ParallelFileReadsTestCounts(&nReads, &most, &retried, &failed, &engine, &state);
        Check(done.size() == 6 && links && busy == 0 && allGood(done) && most <= 2, "filereads: 6 requests with 2 buffers: all read correctly, never more in flight than buffers");
    }
    {  // the engine's block cache on, and the key at 1: the original
        fresh();
        World w;
        w.Init(os, fileSize, 4);
        w.SetRequests({{0, kBlock, 0}});
        uint8_t cacheMemory = 0;
        w.cache.memory = &cacheMemory;
        g_originalCalls = 0;
        const bool r1 = ParallelFileReadsTestStart(&w.impl, &w.queue, 8, reinterpret_cast<void*>(&Pop), reinterpret_cast<void*>(&Original));
        ParallelFileReadsTestCounts(&nReads, &most, &retried, &failed, &engine, &state);
        Check(!r1 && g_originalCalls == 1 && w.queue.next == 0 && state == 1, "filereads: the engine's block cache on: its own reader is called, nothing taken from the queue");
        w.cache.memory = nullptr;
        const bool r2 = ParallelFileReadsTestStart(&w.impl, &w.queue, 1, reinterpret_cast<void*>(&Pop), reinterpret_cast<void*>(&Original));
        Check(!r2 && g_originalCalls == 2 && w.queue.next == 0, "filereads: the key at 1: the engine's own reader");
    }
    {  // an unexpected file object in the middle of a burst: read through the engine's file object with nothing of ours in flight
        fresh();
        World w;
        w.Init(raw, fileSize, 8, 2);
        w.partitions[1].fileSize = fileSize + 1;  // not the size Windows reports
        w.SetRequests({{0, kBlock, 0}, {kBlock, kBlock, 0}, {2ull * kBlock, kBlock, 0}, {3ull * kBlock, kBlock, 0}, {5ull * kBlock, kBlock, 1},
                       {6ull * kBlock, kBlock, 0}});
        g_originalCalls = g_originalBusy = 0;
        const bool r1 = ParallelFileReadsTestStart(&w.impl, &w.queue, 8, reinterpret_cast<void*>(&Pop), reinterpret_cast<void*>(&Original));
        const int busy1 = ParallelFileReadsTestBusy();
        bool links = true;
        const auto got = w.TakeCompleted(&links);
        const bool r2 = ParallelFileReadsTestStart(&w.impl, &w.queue, 8, reinterpret_cast<void*>(&Pop), reinterpret_cast<void*>(&Original));
        ParallelFileReadsTestCounts(&nReads, &most, &retried, &failed, &engine, &state);
        Check(r1 && busy1 == 0 && got.size() == 5 && links && allGood(got) && w.files[1].seeks == 1 && w.files[1].reads == 1 && w.files[1].busyAtRead == 0 &&
                  engine == 1 && nReads == 4 && state == 3,
              "filereads: an unexpected file handle mid-burst: our four reads finish first, then that block is read through the engine's file object, correctly");
        Check(!r2 && g_originalCalls == 1 && g_originalBusy == 0 && w.queue.next == 5, "filereads: ... and the engine's own reader is used from then on (the next request left to it)");
    }
    {  // a hard disk (the drive's answer forced): the engine's reader
        fresh(0, 1);
        World w;
        w.Init(os, fileSize, 4);
        w.SetRequests({{0, kBlock, 0}, {kBlock, kBlock, 0}});
        g_originalCalls = 0;
        const bool r1 = ParallelFileReadsTestStart(&w.impl, &w.queue, 8, reinterpret_cast<void*>(&Pop), reinterpret_cast<void*>(&Original));
        bool links = true;
        const auto got = w.TakeCompleted(&links);
        const bool r2 = ParallelFileReadsTestStart(&w.impl, &w.queue, 8, reinterpret_cast<void*>(&Pop), reinterpret_cast<void*>(&Original));
        ParallelFileReadsTestCounts(&nReads, &most, &retried, &failed, &engine, &state);
        Check(r1 && got.size() == 1 && allGood(got) && engine == 1 && nReads == 0 && state == 2 && !r2 && g_originalCalls == 1,
              "filereads: a container on a hard disk: read the engine's way, its own reader from then on");
    }
    {  // 70 file objects, more than the table of checked ones holds: all read, the drive asked once
        fresh();
        World w;
        w.Init(os, fileSize, 16, 70);
        std::vector<std::tuple<uint64_t, uint64_t, int>> ranges;
        for (int i = 0; i < 140; ++i) ranges.push_back({static_cast<uint64_t>(i % 20) * kBlock, kBlock, i % 70});
        w.SetRequests(ranges);
        std::vector<FakeRequest*> done;
        bool links = true;
        int busy = 0;
        Run(w, 8, &done, &links, &busy);
        ParallelFileReadsTestCounts(&nReads, &most, &retried, &failed, &engine, &state);
        Check(done.size() == 140 && links && busy == 0 && allGood(done) && engine == 0 && state == 0 && nReads == 140,
              "filereads: 70 file objects (the table of checked ones holds 64): all 140 blocks read our way and right");
    }
    {  // the real question to Windows about the drive: an answer (hard disk, not, or unknown), asked once per volume
        ParallelFileReadsTestReset();
        const int a1 = ParallelFileReadsTestSeekPenalty(os), a2 = ParallelFileReadsTestSeekPenalty(os);
        ParallelFileReadsTestCounts(&nReads, &most, &retried, &failed, &engine, &state, &seekQueries);
        Check(a1 == a2 && a1 >= -1 && a1 <= 1 && seekQueries <= 1, "filereads: the drive's seek-penalty answer is asked once per volume and remembered");
        printf("      (filereads: the temporary folder's drive: seek penalty %d; 1 = hard disk, 0 = none, -1 = unknown)\n", a1);
    }
    {  // the wrapped ServiceNotify: the original called, our wake event set
        fresh();
        g_notifyCalls = 0;
        int dummy = 0;
        const bool set = ParallelFileReadsTestNotifyHook(&dummy, reinterpret_cast<void*>(&NotifyOriginal));
        Check(set && g_notifyCalls == 1, "filereads: the wrapped ServiceNotify calls the engine's and sets the reader's wake event");
    }
    {  // a request that arrives while a read is in flight is read before that read ends (a pipe stands in for a slow read)
        fresh(0, 0, true);  // the pipe is not a disk file
        wchar_t name[96];
        swprintf_s(name, L"\\\\.\\pipe\\opf-selftest-%lu", GetCurrentProcessId());
        const HANDLE server = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 2 * kBlock, 2 * kBlock, 0, nullptr);
        const HANDLE client = CreateFileW(name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE, "filereads: a pipe for the slow read");
        if (server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE) {
            World w;
            w.Init(os, fileSize, 8, 2);
            w.files[1].os = server;
            w.SetRequests({{7ull * kBlock, kBlock, 1}, {3ull * kBlock, kBlock, 0}}, 1);  // only the pipe read is queued at first
            PipeHelper helper{&w, &w.requests[1], client, 7ull * kBlock};
            const HANDLE thread = CreateThread(nullptr, 0, &PipeThread, &helper, 0, nullptr);
            const bool r1 = ParallelFileReadsTestStart(&w.impl, &w.queue, 8, reinterpret_cast<void*>(&Pop), reinterpret_cast<void*>(&Original));
            const int busy1 = ParallelFileReadsTestBusy();
            WaitForSingleObject(thread, 5000);
            CloseHandle(thread);
            bool links = true;
            const auto got = w.TakeCompleted(&links);
            Check(r1 && busy1 == 0 && got.size() == 2 && links && allGood(got) && got[0] == &w.requests[1] && got[1] == &w.requests[0],
                  "filereads: a request arriving during a slow read is read and handed on first; the call returns when both are done, bytes right");
        }
        if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    }
    ParallelFileReadsTestReset();
    g_originalCalls = 0;
    CloseHandle(os);
    CloseHandle(raw);
    DeleteFileW(path);
}
