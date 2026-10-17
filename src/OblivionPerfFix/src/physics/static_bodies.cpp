// ParallelStaticBodies: the per-instance physics bodies of an instanced mesh (trees, rocks, bushes with
// collision) are created on several threads instead of one, with the same bodies in the same order afterwards.
//
// When an instanced static mesh component creates its physics state, UInstancedStaticMeshComponent::
// CreateAllInstanceBodies calls FBodyInstance::InitStaticBodies (exe+0x34ECB30) with one FBodyInstance and one transform
// per instance. UE 5.3 (BodyInstance.cpp, FInitBodiesHelperBase): CreateShapesAndActors (exe+0x34DAE60) walks the
// instances (last to first) and for each one fills the body's fields, creates its Chaos particle and proxy
// (FPhysicsInterface::CreateActor) and its shapes (CreateShapes_AssumesLocked: geometry from the body setup's shared
// convex / triangle meshes, materials, filters); afterwards, under the scene's write lock, every body is added to the
// physics scene in array order (AddActorsToScene_AssumesLocked). On a wooded slope streaming in this is 100-300 bodies
// per component and ~166 ms of game thread per Kvatch walk at spot C (2026-10-14), about half of it in
// CreateShapesAndActors.
//
// Why the result is the same with the instances split over threads (UE 5.3 source + this exe):
//  - Everything that orders or numbers bodies happens later, in the scene add, which still runs on the game thread in
//    the stock array order: the particle's unique index (FPBDRigidsSolver::RegisterObject -> GenerateUniqueIdx), the
//    solver's dirty-proxy list, the game-thread acceleration-structure inserts, the component maps. A new particle
//    has unique index -1 and no solver (the particle and proxy constructors, exe+0x16C9050 / 0x1A64C00, only fill
//    their own fields), so its dirty marking does not reach any solver.
//  - The per-instance work only writes that instance's FBodyInstance and the new particle/proxy/shapes. Shared inputs
//    are read: the body setup's geometry (shared through thread-safe shared pointers), physics settings, console
//    variables (game-thread values), the component's collision settings. This exe's CreateShapesAndActors already
//    calls the body setup's CreatePhysicsMeshes only when on the game thread.
//  - First-use initialisations run as in the game: the first instance the game would process (the last one) is done
//    first, alone, on the game thread (physical material handles created on first use, the component's and body
//    setup's weak-pointer serial numbers, function-local statics). Only then do the others run in parallel.
//  - A body that fails (no shapes) is released by the game's own code; for a particle that was never added to a scene
//    that release only deletes the particle and proxy (exe+0x1AA3A40: the scene part needs a unique index).
//  - What differs: heap addresses (allocation order), which are not deterministic in the game either.
// The game's own CreateShapesAndActors does the work: each thread runs it on a copy of the helper whose body and
// transform arrays are views of a slice of the real arrays. Only when no body already has a physics actor (then the
// game removes entries from the arrays, which a view cannot do) and the helper is the static one; else stock.
//
// Site: InitStaticBodies' call of CreateShapesAndActors, right after its IsInGameThread test (found by pattern, the
// two call targets checked). Start-up install (one call redirected); afterwards the ini switches it live.
#include "common.h"

#include <atomic>
#include <cstring>
#include <memory>

namespace opf {
namespace {

// InitStaticBodies (exe+0x34ECB30) at exe+0x34ECCAC: call IsInGameThread; mov [rbp+x],al; test al,al; je;
// lea rcx,[rbp+helper]; call CreateShapesAndActors; test al,al; je
constexpr char kSite[] = "E8 ?? ?? ?? ?? 88 45 ?? 84 C0 74 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 84 C0 74";
constexpr size_t kSiteGameThreadCall = 0, kSiteCreateCall = 0x10;
// IsInGameThread (exe+0xE32300): sub rsp,28h; cmp byte [rip+x],0; je
constexpr char kIsGameThread[] = "48 83 EC 28 80 3D ?? ?? ?? ?? 00 74";
// CreateShapesAndActors (exe+0x34DAE60): prologue; mov rax,[rcx] (Bodies); mov rdi,rcx; mov r13d,[rax+8] (Num);
// call IsInGameThread
constexpr char kCreate[] =
    "40 55 57 41 55 41 56 48 8D 6C 24 A8 48 81 EC 58 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 20 48 8B 01 "
    "48 8B F9 44 8B 68 08 E8";
constexpr size_t kCreateGameThreadCall = 0x2B, kCreateSearch = 0x600;
// ... lea r14,[rsi+ActorHandle]; cmp qword [r14],0; je   (the "already has a body" test)
constexpr char kHandleTest[] = "4C 8D B6 ?? ?? 00 00 49 83 3E 00 0F 84";
// ... cmp byte [rdi+bStatic],0; jcc   (IsStatic, three times: the most frequent such test)
constexpr char kStaticTest[] = "80 7F ?? 00";

constexpr size_t kTransformSize = 0x60;  // FTransform (double)
constexpr size_t kHelperCopy = 0x80;     // bytes of the helper copied for a slice (fields read: +0x00..+0x48)
constexpr int kMaxThreads = 8;

struct UeArray { uint8_t* data; int32_t num; int32_t max; };
using CreateFn = bool (*)(void* helper);

struct Sites {
    uint8_t* call = nullptr;
    uint8_t* create = nullptr;
    uint32_t handleOff = 0, staticOff = 0;
} g;

CreateFn g_create = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{false};
int g_minBodies = 64;

// ---- a small pool: K threads that take slices from the current job ----
// Slices are claimed through one word, generation | slices | next slice: a claim succeeds only while the job of that
// generation has slices left, and a job ends when every slice is done (the done count). A woken worker that comes late
// finds nothing to claim (or claims from a later job, which is as good): the game thread never waits for a worker that
// took no slice, only for slices in progress. A claimed slice keeps its job (and so the job's fields) alive until done.
constexpr uint64_t kSliceBits = 20, kSliceMask = (1ull << kSliceBits) - 1;
constexpr int kMaxSlices = static_cast<int>(kSliceMask);
struct Job {
    const uint8_t* helper = nullptr;
    UeArray* bodies = nullptr;
    UeArray* transforms = nullptr;
    int count = 0;       // bodies [0, count) are the parallel part
    int slice = 0;
    std::atomic<uint64_t> claim{0};  // generation (bits 40-63) | slices (bits 20-39) | next slice (bits 0-19)
    std::atomic<int> done{0};
    std::atomic<int> failed{0};  // a slice whose CreateShapesAndActors returned false (never for static bodies)
    int slices = 0;
    std::atomic<uint8_t>* sliceDone = nullptr;  // PipelinedBodyCreation only: one flag per slice, set when that slice is done
};
Job g_job;
WaitGate g_doneGate;          // the game thread waiting for slices in progress
HANDLE g_wake = nullptr;      // semaphore: one release per worker asked to help (extra releases are harmless)
std::atomic<bool> g_testJitter{false};  // self-test only: workers yield or sleep at random points (to provoke races)
std::atomic<int> g_testHoldMs{0};       // self-test only: workers sleep this long after each wake (late workers)
int g_threads = 0;
CRITICAL_SECTION g_jobLock;   // one job at a time (the game thread is the only caller; this is a guard)

void Jitter(uint32_t& rnd)
{
    if (!g_testJitter.load(std::memory_order_relaxed)) return;
    rnd = rnd * 1664525u + 1013904223u;
    switch (rnd >> 29) {
        case 0: case 1: Sleep(0); break;
        case 2: SwitchToThread(); break;
        case 3: if ((rnd & 0xFF) < 32) Sleep(1); break;  // a late or preempted worker
        default: break;
    }
}

// Opens the job's slices to claims (its fields set before). No slice of the previous job is in progress here.
void OpenJob(int slices)
{
    const uint64_t gen = (g_job.claim.load(std::memory_order_relaxed) >> 40) + 1;
    g_job.claim.store((gen << 40) | (static_cast<uint64_t>(slices) << kSliceBits), std::memory_order_release);
}

int NextSlice() { return static_cast<int>(g_job.claim.load(std::memory_order_relaxed) & kSliceMask); }

// Claims the job's next slice; -1 when none is left.
int ClaimSlice()
{
    uint64_t c = g_job.claim.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t slices = (c >> kSliceBits) & kSliceMask, next = c & kSliceMask;
        if (next >= slices) return -1;
        if (g_job.claim.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            return static_cast<int>(next);
    }
}

void RunOneSlice(int s)
{
    {
        const int begin = s * g_job.slice;
        const int end = begin + g_job.slice < g_job.count ? begin + g_job.slice : g_job.count;
        alignas(16) uint8_t copy[kHelperCopy];
        memcpy(copy, g_job.helper, kHelperCopy);
        UeArray b{g_job.bodies->data + static_cast<size_t>(begin) * 8, end - begin, end - begin};
        UeArray t{g_job.transforms->data + static_cast<size_t>(begin) * kTransformSize, end - begin, end - begin};
        reinterpret_cast<UeArray**>(copy)[0] = &b;
        reinterpret_cast<UeArray**>(copy)[1] = &t;
        if (!g_create(copy)) g_job.failed.fetch_add(1);
        if (g_job.sliceDone) g_job.sliceDone[s].store(1, std::memory_order_release);
        g_job.done.fetch_add(1, std::memory_order_acq_rel);  // the job may end from here on: no job field is read after it
        WakeWord(g_job.done, g_doneGate);
    }
}

void RunSlices()
{
    for (int s; (s = ClaimSlice()) >= 0;) RunOneSlice(s);
}

// The game thread: every slice of the job done (it took part before).
void JoinSlices(int slices)
{
    WaitWord(g_job.done, g_doneGate, [slices](int d) { return d >= slices; });
}

// PipelinedBodyCreation (below): creation, registration and inserts of one component side by side. Returns true when it took the
// call (then *result is CreateShapesAndActors' answer for InitStaticBodies).
bool PipelinedCreate(uint8_t* h, UeArray* bodies, UeArray* transforms, int n, bool* result);

// Nothing of a job runs after the hook returns: the hook waits for every claimed slice, and a worker reads the job
// only between a successful claim and that slice's done count.
DWORD WINAPI Worker(void*)
{
    uint32_t rnd = GetCurrentThreadId();
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        HelperThreadCache();  // the game allocator's per-thread cache (HelperThreadCaches), between jobs only
        if (const int ms = g_testHoldMs.load(std::memory_order_relaxed)) Sleep(static_cast<DWORD>(ms));
        Jitter(rnd);
        for (int s; (s = ClaimSlice()) >= 0;) {
            Jitter(rnd);
            RunOneSlice(s);
        }
    }
}

// ---- statistics (both modes, for the A/B in the log) ----
std::atomic<uint64_t> s_callsOn{0}, s_bodiesOn{0}, s_usOn{0}, s_callsOff{0}, s_bodiesOff{0}, s_usOff{0}, s_stock{0}, s_usWait{0};

uint64_t NowUs()
{
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return QpcToMicroseconds(static_cast<uint64_t>(t.QuadPart), static_cast<uint64_t>(freq.QuadPart));
}

bool Stock(void* helper, int n)
{
    const uint64_t t0 = NowUs();
    const bool r = g_create(helper);
    s_callsOff.fetch_add(1, std::memory_order_relaxed);
    s_bodiesOff.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    s_usOff.fetch_add(NowUs() - t0, std::memory_order_relaxed);
    return r;
}


// HitchLedger (hitch_ledger.cpp): the game thread's time in body creation and the scene add, outermost call only
// (PipelinedBodyCreation and 28b run inside CreateHook / SceneAddHook); only on the game thread (LedgerIsGameThread).
std::atomic<uint64_t> s_ledgerUs{0};
thread_local int t_ledgerDepth = 0;
struct LedgerScope {
    uint64_t t0;
    LedgerScope() : t0(t_ledgerDepth++ == 0 ? NowUs() : 0) {}
    ~LedgerScope()
    {
        if (--t_ledgerDepth == 0 && LedgerIsGameThread()) s_ledgerUs.fetch_add(NowUs() - t0, std::memory_order_relaxed);
    }
};

bool CreateHook(void* helper)
{
    LedgerScope ledgerScope;
    auto* const h = static_cast<uint8_t*>(helper);
    UeArray* const bodies = reinterpret_cast<UeArray**>(h)[0];
    UeArray* const transforms = reinterpret_cast<UeArray**>(h)[1];
    const int n = bodies ? bodies->num : 0;
    if (!g_on.load(std::memory_order_relaxed) || g_threads == 0 || n < g_minBodies || !transforms || transforms->num != n ||
        h[g.staticOff] == 0)
        return Stock(helper, n);
    for (int i = 0; i < n; ++i) {  // a body that already has an actor is removed from the arrays by the game: stock
        const uint8_t* body = reinterpret_cast<uint8_t**>(bodies->data)[i];
        if (!body || *reinterpret_cast<void* const*>(body + g.handleOff)) {
            s_stock.fetch_add(1, std::memory_order_relaxed);
            return Stock(helper, n);
        }
    }
    bool pipelined = false;
    if (PipelinedCreate(h, bodies, transforms, n, &pipelined)) return pipelined;
    const uint64_t t0 = NowUs();
    // The game's first instance (the last one), alone on the game thread: first-use initialisations as in the game.
    alignas(16) uint8_t copy[kHelperCopy];
    memcpy(copy, h, kHelperCopy);
    UeArray b1{bodies->data + static_cast<size_t>(n - 1) * 8, 1, 1};
    UeArray t1{transforms->data + static_cast<size_t>(n - 1) * kTransformSize, 1, 1};
    reinterpret_cast<UeArray**>(copy)[0] = &b1;
    reinterpret_cast<UeArray**>(copy)[1] = &t1;
    bool ok = g_create(copy);
    // The rest in slices on the pool and this thread.
    EnterCriticalSection(&g_jobLock);
    const int m = n - 1;
    const int perThread = (m + g_threads) / (g_threads + 1);
    g_job.helper = h;
    g_job.bodies = bodies;
    g_job.transforms = transforms;
    g_job.count = m;
    g_job.slice = perThread / 2 > 8 ? perThread / 2 : 8;
    g_job.slices = (m + g_job.slice - 1) / g_job.slice;  // about 2 per thread (slice >= half a thread's share)
    g_job.sliceDone = nullptr;
    g_job.done.store(0, std::memory_order_relaxed);
    g_job.failed.store(0, std::memory_order_relaxed);
    OpenJob(g_job.slices);  // no slice of the previous job is in progress here (each was done before it returned)
    const int wake = g_job.slices - 1 < g_threads ? g_job.slices - 1 : g_threads;
    if (wake > 0) ReleaseSemaphore(g_wake, wake, nullptr);
    RunSlices();
    const uint64_t tw = NowUs();
    JoinSlices(g_job.slices);
    s_usWait.fetch_add(NowUs() - tw, std::memory_order_relaxed);
    ok = ok && g_job.failed.load() == 0;
    LeaveCriticalSection(&g_jobLock);
    s_callsOn.fetch_add(1, std::memory_order_relaxed);
    s_bodiesOn.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    s_usOn.fetch_add(NowUs() - t0, std::memory_order_relaxed);
    return ok;
}

bool Fail(const char* what)
{
    Log("[bodies] %s: fix disabled", what);
    return false;
}

bool Find()
{
    const GameModule& game = Game();
    auto rva = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
    const Pattern create = ParsePattern(kCreate), isGt = ParsePattern(kIsGameThread);
    Sites s;
    for (uint8_t* m : FindAll(game.text, ParsePattern(kSite))) {
        uint8_t* const gt = RipTarget(m + kSiteGameThreadCall, 1, 5);
        uint8_t* const cr = RipTarget(m + kSiteCreateCall, 1, 5);
        if (!game.text.Contains(gt) || !game.text.Contains(cr) || !MatchesAt(gt, isGt) || !MatchesAt(cr, create)) continue;
        if (RipTarget(cr + kCreateGameThreadCall, 1, 5) != gt) continue;
        if (s.call) return Fail("InitStaticBodies' call ambiguous");
        s.call = m + kSiteCreateCall;
        s.create = cr;
    }
    if (!s.call) return Fail("InitStaticBodies' call of CreateShapesAndActors not found");
    const Pattern handle = ParsePattern(kHandleTest), stat = ParsePattern(kStaticTest);
    int counts[0x80] = {};
    for (size_t i = 0; i + 16 <= kCreateSearch; ++i) {
        const uint8_t* p = s.create + i;
        if (!s.handleOff && MatchesAt(p, handle)) memcpy(&s.handleOff, p + 3, 4);
        if (MatchesAt(p, stat) && (p[4] == 0x74 || p[4] == 0x75 || (p[4] == 0x0F && (p[5] == 0x84 || p[5] == 0x85))) && p[2] < 0x80)
            ++counts[p[2]];
    }
    int statics = 0;
    for (uint32_t off = 0; off < 0x80; ++off)
        if (counts[off] > statics) { statics = counts[off]; s.staticOff = off; }
    if (s.handleOff == 0 || s.handleOff > 0x400 || statics < 2 || s.staticOff < 0x28 || s.staticOff >= kHelperCopy)
        return Fail("CreateShapesAndActors not as expected (actor handle / static flag)");
    g = s;
    Log("[bodies] InitStaticBodies' call exe+0x%X -> CreateShapesAndActors exe+0x%X; body actor handle +0x%X, helper static "
        "flag +0x%X (%d tests)",
        rva(g.call), rva(g.create), g.handleOff, g.staticOff, statics);
    return true;
}

bool StartPool(int threads)
{
    InitializeCriticalSection(&g_jobLock);
    g_wake = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);  // releases of jobs a late worker missed may pile up: harmless
    if (!g_wake) return false;
    for (int i = 0; i < threads; ++i) {
        HANDLE t = CreateThread(nullptr, 1024 * 1024, Worker, nullptr, 0, nullptr);
        if (!t) break;
        DescribeThread(t, L"OblivionPerfFix bodies");
        CloseHandle(t);
        ++g_threads;
    }
    return g_threads > 0;
}

}  // namespace

bool FindParallelStaticBodies() { return g.call != nullptr || Find(); }

bool InstallParallelStaticBodies(bool beforeGameCode, int threads, int minBodies)
{
    if (g_installed) return true;
    if (!FindParallelStaticBodies()) return false;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    if (threads <= 0) {
        threads = static_cast<int>(UsableLogicalProcessors()) / 4;  // 8 cores / 16 threads -> 4
    }
    threads = threads < 1 ? 1 : threads > kMaxThreads ? kMaxThreads : threads;
    g_minBodies = minBodies > 2 ? minBodies : 2;
    g_create = reinterpret_cast<CreateFn>(g.create);
    if (!StartPool(threads)) return Fail("no helper threads");
    uint8_t* const stub = AllocNearGame(16);
    if (!stub) return Fail("no free memory near the game's code");
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&CreateHook);
    memcpy(stub, jmpAbs, sizeof jmpAbs);
    memcpy(stub + sizeof jmpAbs, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const int64_t rel = stub - (g.call + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("the hook is out of reach");
    const int32_t rel32 = static_cast<int32_t>(rel);
    if (!WriteCode(g.call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4))) return Fail("could not patch the call");
    g_installed = true;
    Log("[bodies] installed (%d helper threads, at least %d bodies per component)", g_threads, g_minBodies);
    return true;
}

void SetParallelStaticBodies(bool enabled, int minBodies)
{
    if (minBodies > 2) g_minBodies = minBodies;
    if (!g_installed || g_on.exchange(enabled) == enabled) return;
    Log("[bodies] %s", enabled ? "instanced meshes' static bodies created on several threads"
                               : "instanced meshes' static bodies created on the game thread as before");
}

void LogParallelStaticBodiesStats()
{
    static uint64_t last[8] = {};
    const uint64_t now[8] = {s_callsOn.load(), s_bodiesOn.load(), s_usOn.load(), s_callsOff.load(), s_bodiesOff.load(),
                             s_usOff.load(), s_stock.load(), s_usWait.load()};
    uint64_t d[8];
    bool any = false;
    for (int i = 0; i < 8; ++i) { d[i] = now[i] - last[i]; last[i] = now[i]; any |= d[i] != 0; }
    if (!any) return;
    Log("[bodies] since last report: parallel %llu calls, %llu bodies, %.1f ms (%.1f us/body; game thread waited %.1f ms for "
        "the helpers); game thread %llu calls, %llu bodies, %.1f ms (%.1f us/body; %llu with existing bodies)",
        d[0], d[1], d[2] / 1000.0, d[1] ? static_cast<double>(d[2]) / static_cast<double>(d[1]) : 0.0, d[7] / 1000.0, d[3], d[4],
        d[5] / 1000.0, d[4] ? static_cast<double>(d[5]) / static_cast<double>(d[4]) : 0.0, d[6]);
}

uint64_t StaticBodiesGtUs() { return s_ledgerUs.load(std::memory_order_relaxed); }

bool ParallelStaticBodiesInfo(StaticBodiesInfoOut* out)
{
    if (!g.call) return false;
    out->call = g.call;
    out->create = g.create;
    out->handleOff = g.handleOff;
    out->staticOff = g.staticOff;
    out->hook = reinterpret_cast<void*>(&CreateHook);
    out->threads = g_threads;
    return true;
}

void ParallelStaticBodiesTestJitter(bool on) { g_testJitter = on; }
void ParallelStaticBodiesTestHold(int ms) { g_testHoldMs = ms; }

void ParallelStaticBodiesTestSetup(void* fakeCreate, uint32_t handleOff, uint32_t staticOff, int threads, int minBodies)
{
    g_create = reinterpret_cast<CreateFn>(fakeCreate);
    g.handleOff = handleOff;
    g.staticOff = staticOff;
    g_minBodies = minBodies;
    if (g_threads == 0) StartPool(threads);
    g_installed = true;
}

// ---- The game allocator's per-thread caches on our helper threads (HelperThreadCaches) ----
//
// Why (2026-10-14, Kvatch spot C, run kvg-2): ParallelStaticBodies ran 24,317 bodies at 3.3 us per body of wall time on 4 helpers plus
// the game thread, against ~4.4 us on the game thread alone: ~1.3x instead of ~4x, and the game thread waited ~45 ms at
// the job barrier during C's arrival. The game thread's samples inside CreateShapesAndActors end in critical-section
// waits under the allocator (exe+0xE0BFF0, FMallocBinned2's MallocExternal). UE 5.3 FMallocBinned2 serves small blocks
// from per-thread free lists, which a thread only has after FMallocBinned2::SetupTLSCachesOnCurrentThread (the engine
// calls it for its own threads; FRunnableThread). Our helpers are plain Windows threads, so every allocation and free
// they make takes the allocator's global lock, and four of them in the per-body allocations take turns there.
// The fix calls the game's own SetupTLSCachesOnCurrentThread (exe+0xE0D650: TlsAlloc the slot if needed, TlsGetValue,
// a page of free lists from the OS, TlsSetValue, register them under the allocator's lock) on each helper thread before
// its first job. Only where blocks come from changes (heap addresses, already not deterministic); the bodies, their
// order and everything the game computes are the same. Live switch: off = the thread's slot value is set to null
// between jobs (the allocator then takes the global path as before; the parked lists keep their free blocks until on).
// Only the threads of ParallelStaticBodies and PhysicsFlushParallel do this, and only between their jobs (no allocation of theirs is in flight).
namespace {
// FMallocBinned2::SetupTLSCachesOnCurrentThread: sub rsp,28h; mov eax,[slot]; test eax,eax; jne +0C; call [TlsAlloc];
// mov [slot],eax; mov ecx,eax; call [TlsGetValue]; test rax,rax; jne ...
constexpr char kSetupTls[] =
    "48 83 EC 28 8B 05 ?? ?? ?? ?? 85 C0 75 0C FF 15 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? 8B C8 FF 15 ?? ?? ?? ?? 48 85 C0 0F 85";
struct TlsSites {
    void (*setup)() = nullptr;
    uint32_t* slot = nullptr;
    int refs = 0;
    bool tried = false;
} gt;
std::atomic<bool> g_cachesOn{false};
thread_local bool t_cacheSetUp = false;
thread_local void* t_cacheLists = nullptr;
}  // namespace

bool FindHelperThreadCaches()
{
    if (gt.tried) return gt.setup != nullptr;
    gt.tried = true;
    const GameModule& game = Game();
    const Pattern p = ParsePattern(kSetupTls);
    uint8_t* found = nullptr;
    for (uint8_t* m : FindAll(game.text, p)) {
        if (found) { Log("[bodies] allocator thread-cache set-up ambiguous: helper caches unavailable"); return false; }
        found = m;
    }
    if (!found) { Log("[bodies] allocator thread-cache set-up not found: helper caches unavailable"); return false; }
    uint8_t* const read = RipTarget(found + 4, 2, 6);    // mov eax,[slot]
    uint8_t* const write = RipTarget(found + 0x14, 2, 6);  // mov [slot],eax
    if (read != write || !game.base || read < game.base) {
        Log("[bodies] allocator thread-cache set-up not as expected: helper caches unavailable");
        return false;
    }
    // The allocator's paths around it read the same slot (mov ecx,[slot] / mov eax,[slot]): count them nearby.
    int refs = 0;
    for (uint8_t* q = found - 0x8000; q < found + 0x1000; ++q) {
        if (!game.text.Contains(q) || !game.text.Contains(q + 6)) continue;
        if (q[0] == 0x8B && (q[1] == 0x0D || q[1] == 0x05) && RipTarget(q, 2, 6) == read) ++refs;
    }
    if (refs < 4) {
        Log("[bodies] allocator thread-cache slot used %d times near its set-up (expected 4+): helper caches unavailable", refs);
        return false;
    }
    gt.setup = reinterpret_cast<void (*)()>(found);
    gt.slot = reinterpret_cast<uint32_t*>(read);
    gt.refs = refs;
    Log("[bodies] allocator thread-cache set-up exe+0x%X (slot exe+0x%X, read %d times nearby)",
        static_cast<unsigned>(found - game.base), static_cast<unsigned>(read - game.base), refs);
    return true;
}

void SetHelperThreadCaches(bool enabled)
{
    if (!gt.setup) return;
    if (g_cachesOn.exchange(enabled) == enabled) return;
    Log("[bodies] helper threads %s", enabled ? "use the game allocator's per-thread caches"
                                             : "allocate through the game allocator's shared path as before");
}

void HelperThreadCache()
{
    if (!gt.setup) return;
    const uint32_t slot = *reinterpret_cast<volatile uint32_t*>(gt.slot);
    if (slot == 0) return;  // the game has not set up any thread's cache yet: leave it to the game
    if (g_cachesOn.load(std::memory_order_relaxed)) {
        if (!t_cacheSetUp) {
            gt.setup();
            t_cacheSetUp = true;
            t_cacheLists = TlsGetValue(slot);
        } else if (TlsGetValue(slot) != t_cacheLists) {
            TlsSetValue(slot, t_cacheLists);
        }
    } else if (t_cacheSetUp && TlsGetValue(slot) != nullptr) {
        TlsSetValue(slot, nullptr);
    }
}

bool HelperThreadCachesInfo(HelperCachesInfoOut* out)
{
    if (!gt.setup) return false;
    out->setup = reinterpret_cast<uint8_t*>(gt.setup);
    out->slot = gt.slot;
    out->refs = gt.refs;
    return true;
}

void HelperThreadCachesTestSetup(void* fakeSetup, uint32_t* fakeSlot)
{
    gt.setup = reinterpret_cast<void (*)()>(fakeSetup);
    gt.slot = fakeSlot;
    gt.tried = true;
}

// ---- PipelinedSceneAdd: the scene add's two halves side by side ----
//
// After the bodies exist, InitStaticBodies adds them to the physics scene under the scene's write lock:
// FChaosScene::AddActorsToScene_AssumesLocked (exe+0x1A9C8A0) runs, per body in array order, AddActorToSolver
// (exe+0x1A2EEB0: FPBDRigidsSolver::RegisterObject: spatial index, unique index, the solver's game-thread lists, the
// dirty proxy, the pending spatial operation) and then, with bImmediate, the insert into the game thread's collision
// query structure (the bounds, the FAccelerationStructureHandle, ISpatialAccelerationCollection::UpdateElementIn).
// Kvatch spot C (kvg-2): registration 22 ms and inserts 36 ms of the game thread per arrival, one after the other.
//
// Here the game thread does the registrations (the game's AddActorToSolver, in array order) while one helper runs the
// game's own AddActorsToScene on the same array, in which the call of AddActorToSolver is redirected to a wait until the
// game thread has registered that body. So every body is registered in the stock order on the game thread, and inserted
// into the query structure in the stock order by the game's own code, each insert after its body's registration (as in
// the game): the same unique indices, lists and tree. What runs side by side: the registration of body k+1 (solver
// lists, that body's own particle and proxy) and the insert of body k (the query structure, reading body k's particle).
// They share no data. Nobody else can see the structure meanwhile: the game thread holds the scene's write lock (the
// external data lock at +0x200 of the solver, an SRW lock whose recursion count lives in a TLS slot at +8) from before
// the first registration until the helper is done; world scene queries take that lock for reading (FScopedSceneReadLock).
// The helper runs inside the game thread's lock: its own recursion count is set to one write before and put back after,
// so the game's code neither takes nor releases the SRW lock there.
// Only when the game uses that lock (p.Chaos.EnableAsyncInitBody, exe+0x9142320) and for InitStaticBodies' call with
// at least PipelinedSceneAddMin bodies; else stock. Start-up install (two calls redirected), then live.
namespace {
constexpr char kAddSite[] = "48 8D 54 24 20 41 B0 01 48 8B 49 28 E8";  // lea rdx,[rsp+20h]; mov r8b,1; mov rcx,[rcx+28h]; call
constexpr char kAddFn[] =
    "4C 8B DC 55 53 56 57 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 33 DB "
    "4D 89 63 18 38 1D ?? ?? ?? ?? 41 0F B6 F0";
constexpr size_t kAddFnFlag = 0x2C;
constexpr char kSolverCall[] = "49 8B 14 24 E8 ?? ?? ?? ?? 40 84 F6";  // mov rdx,[r12]; call AddActorToSolver; test sil,sil
constexpr char kCmovLock[] = "48 0F 45 99";                           // cmovne rbx,[rcx+lock]
constexpr char kSolverOff[] = "48 8B 49 ??";                          // mov rcx,[rcx+solver]
constexpr char kCollOff[] = "49 8B 7F ??";                            // mov rdi,[r15+collection]

using AddFn = void (*)(uint8_t* scene, UeArray* handles, uint8_t immediate);
using ToSolverFn = void (*)(uint8_t* solver, uint8_t* proxy);
struct PipeSites {
    uint8_t* siteA = nullptr;  // the lambda's call of AddActorsToScene
    uint8_t* siteB = nullptr;  // AddActorsToScene's call of AddActorToSolver
    AddFn add = nullptr;
    ToSolverFn toSolver = nullptr;
    const volatile uint8_t* flag = nullptr;
    uint32_t solverOff = 0, lockOff = 0, collOff = 0;
} gp;
bool g_pipeInstalled = false;
std::atomic<bool> g_pipeOn{false};
int g_pipeMin = 64;
struct PipeJob {
    uint8_t* scene = nullptr;
    UeArray* handles = nullptr;
    uint8_t* lock = nullptr;
    std::atomic<int> registered{0};
    std::atomic<int> mismatches{0};
} g_pj;
HANDLE g_pipeGo = nullptr, g_pipeDone = nullptr;
thread_local int t_pipeIndex = -1;  // >= 0 only on the helper while it runs the game's AddActorsToScene for a job
// PipelinedBodyCreation's stream: one component's scene adds arrive chunk by chunk (one InitBodies lambda call per chunk); the helper
// inserts them in order as one fix-31 batch while the game thread goes on. g_pj.handles is then the whole component's
// handle list (global indices), g_pj.registered counts over all chunks.
struct PipeStream {
    std::vector<uint8_t*> buf;        // handles in array order, chunk after chunk
    std::vector<int> chunkEnd;        // end index in buf of each published chunk
    UeArray arr{};                    // view of buf (what ToSolverStub checks against)
    int published = 0, chunksMade = 0;  // game thread only
    int total = 0;                    // bodies of the component (the batch size for BulkIndexInsert)
    std::atomic<int> chunks{0};       // chunks the helper may insert
    std::atomic<int> inserted{0};     // handles the helper has inserted
    std::atomic<bool> closed{false};  // no more chunks
    std::atomic<int> signal{0};       // bumped after each new chunk and at the close (what an idle helper waits on)
    std::atomic<bool> active{false};  // the helper's next job is a stream
} g_ps;
thread_local bool t_stream = false;      // game thread: this lambda call's scene add belongs to the stream
thread_local bool t_streamSync = false;  // ... and the lambda's later loops touch the chunk's particles: wait for its inserts
std::atomic<uint64_t> s34Sync{0};

// The game thread and the helper wait on each other's progress (spin, then block: see WaitWord).
WaitGate g_regGate;     // the helper waiting for g_pj.registered
WaitGate g_insGate;     // the game thread waiting for g_ps.inserted
WaitGate g_streamGate;  // the helper waiting for g_ps.signal
void PublishRegistered(int n)
{
    g_pj.registered.store(n, std::memory_order_release);
    WakeWord(g_pj.registered, g_regGate);
}
void WaitInserted(int n)
{
    WaitWord(g_ps.inserted, g_insGate, [n](int v) { return v >= n; });
}
void SignalStream()
{
    g_ps.signal.fetch_add(1, std::memory_order_acq_rel);
    WakeWord(g_ps.signal, g_streamGate);
}
std::atomic<uint64_t> s_pCalls{0}, s_pBodies{0}, s_pUs{0}, s_pWaitUs{0}, s_pStock{0}, s_pStockBodies{0}, s_pStockUs{0};

// The game's recursive write lock on the scene lock (as in AddActorsToScene / AddActorToSolver).
void LockWrite(uint8_t* lock)
{
    const DWORD idx = *reinterpret_cast<const uint32_t*>(lock + 8);
    const uint64_t v = reinterpret_cast<uint64_t>(TlsGetValue(idx));
    const uint32_t w = static_cast<uint32_t>(v >> 32) + 1;
    TlsSetValue(idx, reinterpret_cast<void*>((static_cast<uint64_t>(w) << 32) | (v & 0xFFFFFFFFull)));
    if (w + static_cast<uint32_t>(v) == 1) AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(lock));
}
void UnlockWrite(uint8_t* lock)
{
    const DWORD idx = *reinterpret_cast<const uint32_t*>(lock + 8);
    const uint64_t v = reinterpret_cast<uint64_t>(TlsGetValue(idx));
    const uint32_t w = static_cast<uint32_t>(v >> 32) - 1;
    TlsSetValue(idx, reinterpret_cast<void*>((static_cast<uint64_t>(w) << 32) | (v & 0xFFFFFFFFull)));
    if (w + static_cast<uint32_t>(v) == 0) ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(lock));
}

// In place of AddActorsToScene's call of AddActorToSolver: on the helper, wait until the game thread registered body k.
void ToSolverStub(uint8_t* solver, uint8_t* proxy)
{
    if (t_pipeIndex < 0) { gp.toSolver(solver, proxy); return; }
    const int k = t_pipeIndex++;
    WaitWord(g_pj.registered, g_regGate, [k](int v) { return v > k; });
    if (reinterpret_cast<uint8_t**>(g_pj.handles->data)[k] != proxy) g_pj.mismatches.fetch_add(1);
}

DWORD WINAPI PipeHelper(void*)
{
    for (;;) {
        WaitForSingleObject(g_pipeGo, INFINITE);
        HelperThreadCache();
        const DWORD idx = *reinterpret_cast<const uint32_t*>(g_pj.lock + 8);
        void* const saved = TlsGetValue(idx);
        TlsSetValue(idx, reinterpret_cast<void*>(1ull << 32));  // inside the game thread's write lock: one write held
        if (g_ps.active.load(std::memory_order_acquire)) {
            // PipelinedBodyCreation: the chunks of one component, in order, as they are published; one fix-31 batch for all.
            BulkBatchBegin(g_ps.total);
            int c = 0, start = 0;
            for (;;) {
                const int sig = g_ps.signal.load(std::memory_order_acquire);  // before the checks: a chunk or close after them changes it
                if (c < g_ps.chunks.load(std::memory_order_acquire)) {
                    const int end = g_ps.chunkEnd[static_cast<size_t>(c)];
                    UeArray a{reinterpret_cast<uint8_t*>(g_ps.buf.data() + start), end - start, end - start};
                    t_pipeIndex = start;
                    if (end > start) gp.add(g_pj.scene, &a, 1);
                    g_ps.inserted.store(end, std::memory_order_release);
                    WakeWord(g_ps.inserted, g_insGate);
                    start = end;
                    ++c;
                    continue;
                }
                if (g_ps.closed.load(std::memory_order_acquire)) {
                    if (c < g_ps.chunks.load(std::memory_order_acquire)) continue;
                    break;
                }
                WaitWord(g_ps.signal, g_streamGate, [sig](int v) { return v != sig; });
            }
            BulkBatchEnd();
            t_pipeIndex = -1;
            TlsSetValue(idx, saved);
            SetEvent(g_pipeDone);
            continue;
        }
        t_pipeIndex = 0;
        BulkBatchBegin(g_pj.handles->num);  // BulkIndexInsert: these query-structure inserts batched (if on), shaped inside the lock
        gp.add(g_pj.scene, g_pj.handles, 1);
        BulkBatchEnd();
        t_pipeIndex = -1;
        TlsSetValue(idx, saved);
        SetEvent(g_pipeDone);
    }
}

// PipelinedBodyCreation: one chunk of the stream. The game thread already holds the scene's write lock (the game's ExecuteWrite around
// the whole component); it registers the chunk's bodies in order while the helper inserts them behind it, and returns
// without waiting (unless the lambda's later loops will touch these particles: t_streamSync).
void StreamAdd(uint8_t* scene, UeArray* handles)
{
    const int n = handles ? handles->num : 0;
    const int base = g_ps.published;
    uint8_t** const h = reinterpret_cast<uint8_t**>(handles->data);
    for (int i = 0; i < n; ++i) g_ps.buf[static_cast<size_t>(base + i)] = h[i];
    g_ps.published = base + n;
    g_ps.chunkEnd[static_cast<size_t>(g_ps.chunksMade)] = base + n;
    g_ps.chunks.store(++g_ps.chunksMade, std::memory_order_release);
    SignalStream();
    uint8_t* const solver = *reinterpret_cast<uint8_t**>(scene + gp.solverOff);
    for (int i = 0; i < n; ++i) {
        gp.toSolver(solver, h[i]);
        PublishRegistered(base + i + 1);
    }
    if (t_streamSync) {
        s34Sync.fetch_add(1, std::memory_order_relaxed);
        WaitInserted(base + n);
    }
}

void SceneAddHook(uint8_t* scene, UeArray* handles, uint8_t immediate)
{
    LedgerScope ledgerScope;
    if (t_stream) {
        if (immediate && scene == g_pj.scene && handles) { StreamAdd(scene, handles); return; }
        // Not the stream's scene add (never seen): the helper's inserts finish first, then the game's call here.
        WaitInserted(g_ps.published);
        gp.add(scene, handles, immediate);
        return;
    }
    const int n = handles ? handles->num : 0;
    uint8_t* solver = nullptr;
    uint8_t* lock = nullptr;
    if (g_pipeOn.load(std::memory_order_relaxed) && immediate && n >= g_pipeMin && g_pipeGo && *gp.flag && scene &&
        *reinterpret_cast<void**>(scene + gp.collOff)) {
        solver = *reinterpret_cast<uint8_t**>(scene + gp.solverOff);
        if (solver) lock = *reinterpret_cast<uint8_t**>(solver + gp.lockOff);
    }
    if (!lock) {
        if (n < g_pipeMin) { gp.add(scene, handles, immediate); return; }
        const uint64_t ts = NowUs();
        gp.add(scene, handles, immediate);
        s_pStock.fetch_add(1, std::memory_order_relaxed);
        s_pStockBodies.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        s_pStockUs.fetch_add(NowUs() - ts, std::memory_order_relaxed);
        return;
    }
    const uint64_t t0 = NowUs();
    LockWrite(lock);
    g_pj.scene = scene;
    g_pj.handles = handles;
    g_pj.lock = lock;
    g_pj.registered.store(0, std::memory_order_relaxed);
    SetEvent(g_pipeGo);
    uint8_t** const h = reinterpret_cast<uint8_t**>(handles->data);
    for (int i = 0; i < n; ++i) {
        gp.toSolver(solver, h[i]);
        PublishRegistered(i + 1);
    }
    const uint64_t tw = NowUs();
    WaitForSingleObject(g_pipeDone, INFINITE);
    s_pWaitUs.fetch_add(NowUs() - tw, std::memory_order_relaxed);
    UnlockWrite(lock);
    s_pCalls.fetch_add(1, std::memory_order_relaxed);
    s_pBodies.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    s_pUs.fetch_add(NowUs() - t0, std::memory_order_relaxed);
}

bool PipeFail(const char* what)
{
    Log("[bodies] scene add: %s: PipelinedSceneAdd unavailable", what);
    return false;
}

bool RedirectCall(uint8_t* call, void* target)
{
    uint8_t* const stub = AllocNearGame(16);
    if (!stub) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(stub, jmpAbs, sizeof jmpAbs);
    memcpy(stub + sizeof jmpAbs, &target, sizeof target);
    DWORD old;
    VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const int64_t rel = stub - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}
}  // namespace

bool FindPipelinedSceneAdd()
{
    if (gp.siteA) return true;
    const GameModule& game = Game();
    const Pattern fnPat = ParsePattern(kAddFn);
    PipeSites s;
    for (uint8_t* m : FindAll(game.text, ParsePattern(kAddSite))) {
        uint8_t* const fn = RipTarget(m + 12, 1, 5);
        if (!game.text.Contains(fn) || !MatchesAt(fn, fnPat)) continue;
        if (s.siteA && reinterpret_cast<uint8_t*>(s.add) != fn) return PipeFail("several AddActorsToScene candidates");
        if (s.siteA) continue;  // the dynamic helper's lambda makes the same call later in the code: keep the first one
        s.siteA = m + 12;
        s.add = reinterpret_cast<AddFn>(fn);
    }
    if (!s.siteA) return PipeFail("InitBodies' call of AddActorsToScene not found");
    uint8_t* const fn = reinterpret_cast<uint8_t*>(s.add);
    s.flag = RipTarget(fn + kAddFnFlag, 2, 6);
    const Pattern solverCall = ParsePattern(kSolverCall), cmov = ParsePattern(kCmovLock), solverOff = ParsePattern(kSolverOff),
                  collOff = ParsePattern(kCollOff);
    for (size_t i = 0; i < 0x200; ++i) {
        uint8_t* const p = fn + i;
        if (!s.siteB && MatchesAt(p, solverCall)) s.siteB = p + 4;
        if (!s.lockOff && MatchesAt(p, cmov)) memcpy(&s.lockOff, p + 4, 4);
        if (!s.solverOff && i < 0x60 && MatchesAt(p, solverOff)) s.solverOff = p[3];
        if (!s.collOff && MatchesAt(p, collOff)) s.collOff = p[3];
    }
    if (!s.siteB || !s.lockOff || !s.solverOff || !s.collOff) return PipeFail("AddActorsToScene not as expected");
    uint8_t* const toSolver = RipTarget(s.siteB, 1, 5);
    // AddActorToSolver takes the same lock under the same flag: cmp byte [flag] within its first 0x20 bytes.
    bool sameFlag = false;
    for (size_t i = 0; i < 0x20 && !sameFlag && game.text.Contains(toSolver); ++i)
        sameFlag = toSolver[i] == 0x38 && toSolver[i + 1] == 0x1D && RipTarget(toSolver + i, 2, 6) == s.flag;
    if (!sameFlag) return PipeFail("AddActorToSolver not as expected");
    s.toSolver = reinterpret_cast<ToSolverFn>(toSolver);
    gp = s;
    Log("[bodies] scene add: call exe+0x%X -> AddActorsToScene exe+0x%X (its AddActorToSolver call exe+0x%X -> exe+0x%X; "
        "lock flag exe+0x%X, scene +0x%X -> solver +0x%X -> lock; query structure +0x%X)",
        static_cast<unsigned>(gp.siteA - game.base), static_cast<unsigned>(fn - game.base),
        static_cast<unsigned>(gp.siteB - game.base), static_cast<unsigned>(toSolver - game.base),
        static_cast<unsigned>(const_cast<const uint8_t*>(gp.flag) - game.base), gp.solverOff, gp.lockOff, gp.collOff);
    return true;
}

bool InstallPipelinedSceneAdd(bool beforeGameCode, int minBodies)
{
    if (g_pipeInstalled) return true;
    if (!FindPipelinedSceneAdd()) return false;
    if (!beforeGameCode) return PipeFail("only installed while the game starts");
    g_pipeMin = minBodies > 2 ? minBodies : 2;
    g_pipeGo = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_pipeDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE t = (g_pipeGo && g_pipeDone) ? CreateThread(nullptr, 1024 * 1024, PipeHelper, nullptr, 0, nullptr) : nullptr;
    if (!t) { g_pipeGo = nullptr; return PipeFail("no helper thread"); }
    DescribeThread(t, L"OblivionPerfFix scene add");
    CloseHandle(t);
    // The inner call first (it passes straight through while nothing is pipelined), then the outer one.
    if (!RedirectCall(gp.siteB, reinterpret_cast<void*>(&ToSolverStub))) return PipeFail("could not patch AddActorToSolver's call");
    if (!RedirectCall(gp.siteA, reinterpret_cast<void*>(&SceneAddHook))) return PipeFail("could not patch AddActorsToScene's call");
    g_pipeInstalled = true;
    Log("[bodies] scene add installed (at least %d bodies)", g_pipeMin);
    return true;
}

void SetPipelinedSceneAdd(bool enabled, int minBodies)
{
    if (minBodies > 2) g_pipeMin = minBodies;
    if (!g_pipeInstalled || g_pipeOn.exchange(enabled) == enabled) return;
    Log("[bodies] scene add: %s", enabled ? "registration and query-structure inserts side by side"
                                          : "registration and inserts one after the other as before");
}

void LogPipelinedSceneAddStats()
{
    static uint64_t last[8] = {};
    const uint64_t now[8] = {s_pCalls.load(), s_pBodies.load(), s_pUs.load(), s_pWaitUs.load(), s_pStock.load(),
                             static_cast<uint64_t>(g_pj.mismatches.load()), s_pStockBodies.load(), s_pStockUs.load()};
    uint64_t d[8];
    bool any = false;
    for (int i = 0; i < 8; ++i) { d[i] = now[i] - last[i]; last[i] = now[i]; any |= d[i] != 0; }
    if (!any) return;
    Log("[bodies] scene add since last report: side by side %llu calls, %llu bodies, %.1f ms (%.2f us/body; the game thread "
        "waited %.1f ms for the inserts); as before %llu calls, %llu bodies, %.1f ms (%.2f us/body); order mismatches %llu",
        d[0], d[1], d[2] / 1000.0, d[1] ? static_cast<double>(d[2]) / static_cast<double>(d[1]) : 0.0, d[3] / 1000.0, d[4], d[6],
        d[7] / 1000.0, d[6] ? static_cast<double>(d[7]) / static_cast<double>(d[6]) : 0.0, d[5]);
}

void* PipelinedSceneAddTestSetup(void* add, void* toSolver, const volatile uint8_t* flag, uint32_t solverOff, uint32_t lockOff,
                                 uint32_t collOff, int minBodies)
{
    gp.add = reinterpret_cast<AddFn>(add);
    gp.toSolver = reinterpret_cast<ToSolverFn>(toSolver);
    gp.flag = flag;
    gp.solverOff = solverOff;
    gp.lockOff = lockOff;
    gp.collOff = collOff;
    g_pipeMin = minBodies;
    if (!g_pipeGo) {
        g_pipeGo = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_pipeDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (HANDLE t = CreateThread(nullptr, 0, PipeHelper, nullptr, 0, nullptr)) CloseHandle(t);
    }
    g_pipeInstalled = true;
    return reinterpret_cast<void*>(&ToSolverStub);
}
void* PipelinedSceneAddTestHook() { return reinterpret_cast<void*>(&SceneAddHook); }
bool PipelinedSceneAddInfo(uint8_t** sites, uint32_t* offsets)
{
    if (!gp.siteA) return false;
    sites[0] = gp.siteA; sites[1] = reinterpret_cast<uint8_t*>(gp.add); sites[2] = gp.siteB;
    sites[3] = reinterpret_cast<uint8_t*>(gp.toSolver); sites[4] = const_cast<uint8_t*>(gp.flag);
    offsets[0] = gp.solverOff; offsets[1] = gp.lockOff; offsets[2] = gp.collOff;
    return true;
}

// ---- DirtyTreeQuickInsert ("equivalent" class): DirtyTreeQuickInsert ----
//
// New and moved collision elements go into a dirty-element tree (a dynamic AABB tree) until the next background rebuild
// of the collision index takes them into its built tree. Each insert picks its sibling with UE's
// TAABBTree::FindBestSibling (exe+0x192FD60): a surface-area branch and bound over the whole dirty tree, about half of
// each insert's time, with 5,000-11,000 inserts per physics step while a forest streams in (Kvatch C).
// This fix replaces that search, for dirty-element trees only, by the usual greedy descent (Box2D's b2DynamicTree
// cost rule: stop where pairing with this node is cheaper than descending, else descend into the child whose bounds
// grow least): a different but valid tree with exactly the same elements and bounds. Queries visit every element whose
// bounds intersect (the tree only changes the visiting order), so results are the same except where the order decides
// (ties between equal-distance hits, the order of unsorted overlap results, the order of broadphase pairs), which the
// game itself varies from run to run (the background rebuild is polled with IsComplete). User decision 2026-10-14:
// allowed if the game looks and plays the same. Key DirtyTreeQuickInsert (installed at start-up when on, then live).
// The rebuilt tree is built from the element cache, not from the dirty tree, so the shape difference ends at the next
// rebuild swap; the dynamic-particle tree (whose leaf-overlap cache feeds the broadphase) keeps the game's search.
//
// Layout (TAABBTree, this build): nodes TArray at +0x18 (Num +0x20), 0x70 bytes each: ChildrenBounds[2] (6 doubles each,
// +0x00 / +0x30), ChildrenNodes[2] (+0x60 / +0x64), ParentNode +0x68, flags +0x6C (bit 0 = leaf); RootNode +0x54; leaves
// TArray at +0x28, 0x48 bytes each, element count +0x38 (leaf capacity = p.aabbtree.DynamicTreeLeafCapacity).
namespace {
constexpr char kQiFbsCall[] = "48 8B D3 48 89 BC 24 ?? ?? ?? ?? 4C 89 A4 24 ?? ?? ?? ?? E8";  // in InsertLeaf
constexpr char kQiInsertLeaf[] = "40 53 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 83 79 54 FF";
constexpr char kQiSite1[] = "48 8B 8F D0 00 00 00 48 85 C9 74 ?? 4D 8B CC 48 8D 55 ?? 4D 8B C7 E8";  // new dirty element
constexpr char kQiSite2[] = "48 8B 8F D0 00 00 00 48 8D 55 ?? 4D 8B CC 4D 8B C7 E8";                 // moved dirty element
// In FindBestSibling: lea rcx,[rax+rax*8]; mov rax,[rsi+28h] (leaves, 0x48 each); ...; movsxd rdx,[rax+rcx*8+38h] (count);
// movsxd rax,[capacity]; cmp; setb. And the node walk: movsxd r8,[rsi+54h] (root); imul rbx,r8,70h; ...; add rbx,[rsi+18h];
// test byte [rbx+6Ch],1 (leaf flag). Both pin the layout QuickFindBestSibling reads.
constexpr char kQiCapacity[] = "48 8D 0C C0 48 8B 46 28 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 63 54 C8 38 48 63 05 ?? ?? ?? ?? 48 3B D0 0F 92 C0";
constexpr size_t kQiCapacityRip = 0x19;
constexpr char kQiNodeWalk[] = "4C 63 46 54 49 6B D8 70 ?? ?? ?? ?? 48 03 5E 18 F6 43 6C 01";
using FbsFn = int32_t (*)(uint8_t* tree, const double* bounds, bool* addToLeaf);
using InsertLeafFn = int32_t* (*)(uint8_t* tree, int32_t* out, void* payload, const double* bounds);
FbsFn g_fbs = nullptr;
InsertLeafFn g_insertLeaf = nullptr;
const int32_t* g_leafCap = nullptr;
std::atomic<bool> g_qiOn{false};
bool g_qiInstalled = false;
thread_local bool t_dirtyInsert = false;
std::atomic<uint64_t> s_qiGreedy{0}, s_qiStock{0};

inline double Area(double x0, double y0, double z0, double x1, double y1, double z1)
{
    const double dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
    return 2.0 * (dx * dy + dx * dz + dy * dz);
}
struct Box { double mn[3], mx[3]; };
inline Box Union(const Box& a, const Box& b)
{
    Box r;
    for (int i = 0; i < 3; ++i) { r.mn[i] = a.mn[i] < b.mn[i] ? a.mn[i] : b.mn[i]; r.mx[i] = a.mx[i] > b.mx[i] ? a.mx[i] : b.mx[i]; }
    return r;
}
inline double Area(const Box& b) { return Area(b.mn[0], b.mn[1], b.mn[2], b.mx[0], b.mx[1], b.mx[2]); }
inline Box NodeChildBox(const uint8_t* node, int c) { Box b; memcpy(&b, node + c * 0x30, sizeof b); return b; }
inline bool IsLeafNode(const uint8_t* node) { return (node[0x6C] & 1) != 0; }
inline Box NodeBox(const uint8_t* node)
{
    return IsLeafNode(node) ? NodeChildBox(node, 0) : Union(NodeChildBox(node, 0), NodeChildBox(node, 1));
}
}  // namespace

// The greedy sibling choice (also used by the self-test on a fake tree).
int32_t QuickFindBestSibling(uint8_t* tree, const double* bounds, bool* addToLeaf, int32_t leafCapacity)
{
    *addToLeaf = false;
    Box nb;
    memcpy(&nb, bounds, sizeof nb);
    const uint8_t* const nodes = *reinterpret_cast<uint8_t* const*>(tree + 0x18);
    const uint8_t* const leaves = *reinterpret_cast<uint8_t* const*>(tree + 0x28);
    int32_t idx = *reinterpret_cast<const int32_t*>(tree + 0x54);
    for (int guard = 0; guard < 256; ++guard) {
        const uint8_t* const node = nodes + static_cast<size_t>(idx) * 0x70;
        if (IsLeafNode(node)) {
            const int32_t leaf = *reinterpret_cast<const int32_t*>(node + 0x60);
            const int32_t count = *reinterpret_cast<const int32_t*>(leaves + static_cast<size_t>(leaf) * 0x48 + 0x38);
            *addToLeaf = static_cast<uint32_t>(count) < static_cast<uint32_t>(leafCapacity);
            return idx;
        }
        const Box self = NodeBox(node);
        const double area = Area(self), combined = Area(Union(self, nb));
        const double cost = 2.0 * combined, inherit = 2.0 * (combined - area);
        double childCost[2];
        for (int c = 0; c < 2; ++c) {
            const Box cb = NodeChildBox(node, c);
            const int32_t ci = *reinterpret_cast<const int32_t*>(node + 0x60 + 4 * c);
            const bool leafChild = IsLeafNode(nodes + static_cast<size_t>(ci) * 0x70);
            const double grown = Area(Union(cb, nb));
            childCost[c] = (leafChild ? grown : grown - Area(cb)) + inherit;
        }
        if (cost < childCost[0] && cost < childCost[1]) return idx;  // pair with this node
        idx = *reinterpret_cast<const int32_t*>(node + 0x60 + 4 * (childCost[1] < childCost[0] ? 1 : 0));
    }
    return idx;
}

namespace {
thread_local int32_t t_forcedSibling = -1;             // BulkIndexInsert: InsertLeafForcedSibling's answer for this call
std::atomic<DirtyInsertHookFn> g_dirtyHook{nullptr};  // BulkIndexInsert (bulk_insert.cpp) takes the dirty inserts
int32_t FbsHook(uint8_t* tree, const double* bounds, bool* addToLeaf)
{
    if (t_forcedSibling >= 0) {  // BulkIndexInsert's batched insert: pair with this node (never add to its leaf)
        *addToLeaf = false;
        return t_forcedSibling;
    }
    if (t_dirtyInsert && g_qiOn.load(std::memory_order_relaxed)) {
        s_qiGreedy.fetch_add(1, std::memory_order_relaxed);
        return QuickFindBestSibling(tree, bounds, addToLeaf, *g_leafCap);
    }
    s_qiStock.fetch_add(1, std::memory_order_relaxed);
    return g_fbs(tree, bounds, addToLeaf);
}
int32_t* DirtyInsertHook(uint8_t* tree, int32_t* out, void* payload, const double* bounds)
{
    if (const DirtyInsertHookFn hook = g_dirtyHook.load(std::memory_order_acquire)) return hook(tree, out, payload, bounds);
    t_dirtyInsert = true;
    int32_t* const r = g_insertLeaf(tree, out, payload, bounds);
    t_dirtyInsert = false;
    return r;
}
bool QiFail(const char* what)
{
    Log("[bodies] dirty-tree quick insert: %s: unavailable", what);
    return false;
}
}  // namespace

bool InstallDirtyTreeQuickInsert(bool beforeGameCode)
{
    if (g_qiInstalled) return true;
    if (!beforeGameCode) return QiFail("only installed while the game starts");
    const GameModule& game = Game();
    const Pattern ilPat = ParsePattern(kQiInsertLeaf), callPat = ParsePattern(kQiFbsCall), capPat = ParsePattern(kQiCapacity);
    std::vector<uint8_t*> s1 = FindAll(game.text, ParsePattern(kQiSite1)), s2 = FindAll(game.text, ParsePattern(kQiSite2));
    if (s1.size() != 1 || s2.size() != 1) return QiFail("dirty-tree insert sites not unique");
    uint8_t* const call1 = s1[0] + 0x16;
    uint8_t* const call2 = s2[0] + 0x11;
    uint8_t* const il = RipTarget(call1, 1, 5);
    if (il != RipTarget(call2, 1, 5) || !game.text.Contains(il) || !MatchesAt(il, ilPat)) return QiFail("InsertLeaf not as expected");
    uint8_t* fbsCall = nullptr;
    for (size_t i = 0; i < 0x100 && !fbsCall; ++i)
        if (MatchesAt(il + i, callPat)) fbsCall = il + i + 0x13;
    if (!fbsCall) return QiFail("FindBestSibling's call not found");
    uint8_t* const fbs = RipTarget(fbsCall, 1, 5);
    const Pattern walkPat = ParsePattern(kQiNodeWalk);
    bool walkFound = false;
    for (size_t i = 0; i < 0x200 && !walkFound; ++i) walkFound = MatchesAt(fbs + i, walkPat);
    if (!walkFound) return QiFail("tree node layout not as expected (another game version?)");
    for (size_t i = 0; i < 0x600 && !g_leafCap; ++i)
        if (MatchesAt(fbs + i, capPat)) g_leafCap = reinterpret_cast<const int32_t*>(RipTarget(fbs + i + kQiCapacityRip, 3, 7));
    if (!g_leafCap) return QiFail("leaf layout or capacity not as expected (another game version?)");
    if (*g_leafCap < 1 || *g_leafCap > 1024) return QiFail("leaf capacity out of range");
    g_fbs = reinterpret_cast<FbsFn>(fbs);
    g_insertLeaf = reinterpret_cast<InsertLeafFn>(il);
    if (!RedirectCall(fbsCall, reinterpret_cast<void*>(&FbsHook))) return QiFail("could not patch FindBestSibling's call");
    if (!RedirectCall(call1, reinterpret_cast<void*>(&DirtyInsertHook)) || !RedirectCall(call2, reinterpret_cast<void*>(&DirtyInsertHook)))
        return QiFail("could not patch the dirty-tree inserts");
    g_qiInstalled = true;
    Log("[bodies] dirty-tree quick insert installed: InsertLeaf exe+0x%X (dirty calls exe+0x%X / 0x%X), FindBestSibling exe+0x%X, "
        "leaf capacity exe+0x%X (%d)",
        static_cast<unsigned>(il - game.base), static_cast<unsigned>(call1 - game.base), static_cast<unsigned>(call2 - game.base),
        static_cast<unsigned>(fbs - game.base), static_cast<unsigned>(reinterpret_cast<const uint8_t*>(g_leafCap) - game.base), *g_leafCap);
    return true;
}

// ---- For BulkIndexInsert (bulk_insert.cpp): the same three call sites ----
bool DirtyTreeSitesInstalled() { return g_qiInstalled; }
void SetDirtyInsertHook(DirtyInsertHookFn hook) { g_dirtyHook.store(hook, std::memory_order_release); }
// DirtyTreeQuickInsert's path for one dirty-tree insert (greedy while DirtyTreeQuickInsert is on, else the game's search).
int32_t* DirtyInsertDefault(uint8_t* tree, int32_t* out, void* payload, const double* bounds)
{
    t_dirtyInsert = true;
    int32_t* const r = g_insertLeaf(tree, out, payload, bounds);
    t_dirtyInsert = false;
    return r;
}
// The game's InsertLeaf with its sibling search answering `sibling` (a node without a parent; never added to its leaf).
int32_t* InsertLeafForcedSibling(uint8_t* tree, int32_t* out, void* payload, const double* bounds, int32_t sibling)
{
    t_forcedSibling = sibling;
    int32_t* const r = g_insertLeaf(tree, out, payload, bounds);
    t_forcedSibling = -1;
    return r;
}
// Self-test only: InsertLeaf and FindBestSibling replaced by models; the two hooks callable directly.
void DirtyTreeTestSetup(void* insertLeaf, void* fbs, const int32_t* leafCap)
{
    g_insertLeaf = reinterpret_cast<InsertLeafFn>(insertLeaf);
    g_fbs = reinterpret_cast<FbsFn>(fbs);
    g_leafCap = leafCap;
    g_qiInstalled = true;
}
int32_t* DirtyTreeTestInsert(uint8_t* tree, int32_t* out, void* payload, const double* bounds)
{
    return DirtyInsertHook(tree, out, payload, bounds);
}
int32_t DirtyTreeTestFbs(uint8_t* tree, const double* bounds, bool* addToLeaf) { return FbsHook(tree, bounds, addToLeaf); }

void SetDirtyTreeQuickInsert(bool enabled)
{
    if (!g_qiInstalled || g_qiOn.exchange(enabled) == enabled) return;
    Log("[bodies] dirty-tree quick insert %s", enabled ? "on (greedy sibling choice in dirty-element trees)" : "off (the game's search)");
}

void LogDirtyTreeQuickInsertStats()
{
    static uint64_t last[2] = {};
    const uint64_t now[2] = {s_qiGreedy.load(), s_qiStock.load()};
    if (now[0] == last[0] && now[1] == last[1]) return;
    Log("[bodies] dirty-tree inserts since last report: greedy %llu, the game's search %llu", now[0] - last[0], now[1] - last[1]);
    last[0] = now[0]; last[1] = now[1];
}

// ---- PipelinedBodyCreation: one component's body creation, registration and inserts side by side ----
//
// Why (2026-10-15, Kvatch spot C, run bi4-s-b1 with ParallelStaticBodies, PipelinedSceneAdd, DirtyTreeQuickInsert and BulkIndexInsert on): the frames in which a World Partition area
// is added to the world spend ~120 ms of game thread per C arrival in InitStaticBodies, three steps one after the other
// for each instanced mesh: the bodies are made (ParallelStaticBodies: on 4 helpers + the game thread, ~46 ms), then added to the scene
// (PipelinedSceneAdd: registration here, query-structure inserts on a helper, ~27 ms), then filed in the scene's component maps
// (~18 ms, game thread). Here they overlap: the helpers make the bodies in small slices in array order while the game
// thread, as soon as a chunk of 64 bodies exists, runs the game's own scene-add code on that chunk (registration, maps),
// and 28b's helper inserts every registered body in order behind it.
//
// How, with the game's own code only (UE 5.3 BodyInstance.cpp, FInitBodiesHelperBase::InitBodies; this exe):
// InitStaticBodies calls CreateShapesAndActors (ParallelStaticBodies's hook) and, if that returned true, runs FPhysicsCommand::
// ExecuteWrite(PhysScene, lambda) (exe+0x35303B0: the scene's write lock around the lambda, exe+0x34D0060). The lambda
// skips creation when its first capture says it was done, then: per body the shapes' sim/trace flags (pre-add),
// AddActorsToScene over the bodies' handles (PipelinedSceneAdd's hook), per body AddToComponentMaps and the collision-event
// registration, per body the max-depenetration update (a flag in the body), and dynamic set-up (not for static bodies).
// Every loop is per body in array order and nothing is done once per call except the local handle array, so running the
// lambda over consecutive chunks of the array does the same per-body operations in the same order as one call over the
// array. This fix calls the game's ExecuteWrite once (the same lock the game takes, in the same InitStaticBodies call)
// with a callable that runs the lambda chunk by chunk, and returns false to InitStaticBodies (which then skips its own
// ExecuteWrite: `test al,al; je` straight to its end).
// What changes in time, and why it is still the same result:
//  - Body j (j > k) may be made while body k is registered / inserted / filed. Making a body touches only that body's
//    FBodyInstance and its new particle, proxy and shapes (ParallelStaticBodies's argument; a new particle has no solver, so its dirty
//    marking reaches no solver), and reads shared inputs no step of the scene add writes (body setup, component, cvars).
//    Creation never takes the scene lock: its call tree (direct calls) reaches the lock only through ExecuteRead on the
//    new actor (no scene yet: no lock) and ReleaseActor of a failed body (the scene part needs a unique index).
//  - The pre-add flags of chunk c+1 are set after chunk c's registration: they touch only chunk c+1's unregistered
//    particles (as in the game, before their own registration).
//  - Maps of chunk c while 28b's helper still inserts chunk c: the maps are the scene's own (game thread, own lock);
//    the inserts touch the query structure and read the particles. 28b already registers ahead of the inserts.
//  - The lambda's max-depenetration loop writes a particle that the insert reads: for a chunk with such a body the
//    chunk's inserts are finished before the lambda goes on (t_streamSync).
//  - Unique indices, solver lists, query-structure insert order (and so the tree), map insertion order: array order as in
//    the game. Everything happens inside one write-lock section, so no reader sees a partial state.
// Only for what ParallelStaticBodies takes (static helper, >= ParallelStaticBodiesMin bodies, none with a body yet) and when the game
// uses the scene lock (p.Chaos.EnableAsyncInitBody); the inserts go to 28b's helper only while 28b is on (else the game's
// AddActorsToScene runs per chunk on the game thread). No code patch of its own (ParallelStaticBodies's and 28b's hooks); live switch.
namespace {
// The lambda: push r13; sub rsp,70h; mov rax,[rcx]; lea r13,[rcx+8]; cmp byte [rax],0; jne; mov rcx,[r13]; call Create
constexpr char kLambda[] = "41 55 48 83 EC 70 48 8B 01 4C 8D 69 08 80 38 00 75 ?? 49 8B 4D 00 E8";
constexpr size_t kLambdaCreateCall = 0x16;
// ExecuteWrite(FPhysScene*, TFunctionRef<void>): push rbx; sub rsp,40h; mov rbx,rdx; test rcx,rcx; je; mov rax,[rcx+X]
constexpr char kExecWrite[] = "40 53 48 83 EC 40 48 8B DA 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 41";
constexpr char kExecCallable[] = "48 8B 4B 08 FF 13";  // mov rcx,[rbx+8]; call [rbx]
// The lambda's max-depenetration loop: mov rax,[rbx]; test byte [rax+off],bit; je
constexpr char kPostTest[] = "48 8B 03 F6 40 ?? ?? 74";
constexpr int kChunk = 64, kSlice34 = 16;
constexpr size_t kHelperScene = 0x28;  // the helper's PhysScene (the lambda: mov rcx,[helper+28h] before AddActorsToScene)

struct FRef { void (*call)(void*); void* ctx; };  // TFunctionRef<void>: callable thunk, then its object
using LambdaFn = void (*)(void* capture);
using ExecWriteFn = uint8_t (*)(uint8_t* scene, FRef* fn);
struct Sites34 {
    LambdaFn lambda = nullptr;
    ExecWriteFn execWrite = nullptr;
    uint32_t postOff = 0;
    uint8_t postBit = 0;
} g34;
bool g34Found = false;
std::atomic<bool> g34On{false};
std::unique_ptr<std::atomic<uint8_t>[]> g_sliceFlags;
int g_sliceFlagsCap = 0;
std::atomic<uint64_t> s34Calls{0}, s34Bodies{0}, s34Us{0}, s34WaitUs{0}, s34GtSlices{0}, s34TailUs{0};
uint8_t* g34ExecCall = nullptr;  // InitStaticBodies' call of ExecuteWrite

struct Ctx34 {
    uint8_t* h;
    UeArray* bodies;
    UeArray* transforms;
    int n;
    bool stream;
    uint64_t waitUs = 0, tailUs = 0;
    int gtSlices = 0;
};

// The game thread needs slice s: while no helper has taken it yet, make the next unclaimed slice (<= s: they go in array
// order) itself; once s is taken, wait for it (slices beyond s would only delay the game thread's own next chunk).
void WaitSlice(int s, Ctx34& c)
{
    uint64_t t0 = 0;
    for (;;) {
        const int done = g_job.done.load(std::memory_order_acquire);  // before the flag: a slice finished after it changes it
        if (g_job.sliceDone[s].load(std::memory_order_acquire)) break;
        if (NextSlice() <= s) {
            const int k = ClaimSlice();
            if (k >= 0) { RunOneSlice(k); ++c.gtSlices; continue; }
        }
        if (!t0) t0 = NowUs();
        WaitWord(g_job.done, g_doneGate, [done](int v) { return v != done; });
    }
    if (t0) c.waitUs += NowUs() - t0;
}

bool ChunkHasPostFlag(const UeArray& b)
{
    for (int i = 0; i < b.num; ++i)
        if (reinterpret_cast<uint8_t* const*>(b.data)[i][g34.postOff] & g34.postBit) return true;
    return false;
}

// Runs inside the game's ExecuteWrite (the scene's write lock held by this thread).
void ChunkLoop(void* p)
{
    Ctx34& c = *static_cast<Ctx34*>(p);
    if (c.stream) {
        g_ps.buf.resize(static_cast<size_t>(c.n));
        g_ps.chunkEnd.resize(static_cast<size_t>(c.n / kChunk + 2));
        g_ps.arr = UeArray{reinterpret_cast<uint8_t*>(g_ps.buf.data()), c.n, c.n};
        g_ps.published = g_ps.chunksMade = 0;
        g_ps.total = c.n;
        g_ps.chunks.store(0);
        g_ps.inserted.store(0);
        g_ps.closed.store(false);
        g_pj.scene = *reinterpret_cast<uint8_t**>(c.h + kHelperScene);
        uint8_t* const solver = *reinterpret_cast<uint8_t**>(g_pj.scene + gp.solverOff);
        g_pj.lock = *reinterpret_cast<uint8_t**>(solver + gp.lockOff);
        g_pj.handles = &g_ps.arr;
        g_pj.registered.store(0);
        g_ps.active.store(true, std::memory_order_release);
        SetEvent(g_pipeGo);
    }
    const int m = c.n - 1;  // bodies [0, m) come from the slices; body m (the game's first) was made before
    uint8_t made = 1;       // the lambda's first capture: creation already done
    for (int start = 0; start < c.n; start += kChunk) {
        const int end = start + kChunk < c.n ? start + kChunk : c.n;
        const int slicedEnd = end < m ? end : m;
        for (int s = start / kSlice34; s * kSlice34 < slicedEnd; ++s) WaitSlice(s, c);
        alignas(16) uint8_t copy[kHelperCopy];
        memcpy(copy, c.h, kHelperCopy);
        UeArray b{c.bodies->data + static_cast<size_t>(start) * 8, end - start, end - start};
        UeArray t{c.transforms->data + static_cast<size_t>(start) * kTransformSize, end - start, end - start};
        reinterpret_cast<UeArray**>(copy)[0] = &b;
        reinterpret_cast<UeArray**>(copy)[1] = &t;
        void* capture[2] = {&made, copy};
        t_streamSync = c.stream && ChunkHasPostFlag(b);
        t_stream = c.stream;
        g34.lambda(capture);
        t_stream = t_streamSync = false;
    }
    if (c.stream) {
        const uint64_t t0 = NowUs();
        g_ps.closed.store(true, std::memory_order_release);
        SignalStream();
        WaitForSingleObject(g_pipeDone, INFINITE);
        g_ps.active.store(false, std::memory_order_relaxed);
        c.tailUs = NowUs() - t0;
    }
}

bool Fail34(const char* what)
{
    Log("[bodies] pipelined creation: %s: PipelinedBodyCreation unavailable", what);
    return false;
}

bool PipelinedCreate(uint8_t* h, UeArray* bodies, UeArray* transforms, int n, bool* result)
{
    if (!g34Found || !g34On.load(std::memory_order_relaxed) || !gp.flag || !*gp.flag) return false;
    if ((n - 1 + kSlice34 - 1) / kSlice34 > kMaxSlices) return false;  // more slices than the claim word holds (never seen)
    uint8_t* const scene = *reinterpret_cast<uint8_t**>(h + kHelperScene);
    uint8_t* const solver = scene ? *reinterpret_cast<uint8_t**>(scene + gp.solverOff) : nullptr;
    if (!solver || !*reinterpret_cast<uint8_t**>(solver + gp.lockOff)) return false;
    const bool stream = g_pipeInstalled && g_pipeOn.load(std::memory_order_relaxed) && g_pipeGo &&
                        *reinterpret_cast<void**>(scene + gp.collOff) != nullptr;
    const uint64_t t0 = NowUs();
    // The game's first instance (the last one), alone on the game thread, first: first-use initialisations (ParallelStaticBodies).
    alignas(16) uint8_t copy[kHelperCopy];
    memcpy(copy, h, kHelperCopy);
    UeArray b1{bodies->data + static_cast<size_t>(n - 1) * 8, 1, 1};
    UeArray t1{transforms->data + static_cast<size_t>(n - 1) * kTransformSize, 1, 1};
    reinterpret_cast<UeArray**>(copy)[0] = &b1;
    reinterpret_cast<UeArray**>(copy)[1] = &t1;
    g_create(copy);  // static bodies: always true (false only from the dynamic weld path)
    EnterCriticalSection(&g_jobLock);
    const int m = n - 1;
    const int slices = (m + kSlice34 - 1) / kSlice34;
    if (slices > g_sliceFlagsCap) {
        g_sliceFlags.reset(new std::atomic<uint8_t>[static_cast<size_t>(slices) * 2]);
        g_sliceFlagsCap = slices * 2;
    }
    for (int s = 0; s < slices; ++s) g_sliceFlags[s].store(0, std::memory_order_relaxed);
    g_job.helper = h;
    g_job.bodies = bodies;
    g_job.transforms = transforms;
    g_job.count = m;
    g_job.slice = kSlice34;
    g_job.slices = slices;
    g_job.sliceDone = g_sliceFlags.get();
    g_job.done.store(0, std::memory_order_relaxed);
    g_job.failed.store(0, std::memory_order_relaxed);
    OpenJob(slices);
    const int wake = slices < g_threads ? slices : g_threads;
    if (wake > 0) ReleaseSemaphore(g_wake, wake, nullptr);
    Ctx34 ctx{h, bodies, transforms, n, stream};
    FRef fr{&ChunkLoop, &ctx};
    g34.execWrite(scene, &fr);  // the game's ExecuteWrite: write lock, ChunkLoop, unlock
    for (int s = 0; s < slices; ++s) WaitSlice(s, ctx);  // all made already (every chunk waited); a guard
    JoinSlices(slices);  // every done count in (a slice reads the job only until its count)
    if (g_job.failed.load() != 0) Log("[bodies] pipelined creation: a slice failed (not expected for static bodies)");
    g_job.sliceDone = nullptr;
    LeaveCriticalSection(&g_jobLock);
    s34Calls.fetch_add(1, std::memory_order_relaxed);
    s34Bodies.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    s34Us.fetch_add(NowUs() - t0, std::memory_order_relaxed);
    s34WaitUs.fetch_add(ctx.waitUs, std::memory_order_relaxed);
    s34TailUs.fetch_add(ctx.tailUs, std::memory_order_relaxed);
    s34GtSlices.fetch_add(static_cast<uint64_t>(ctx.gtSlices), std::memory_order_relaxed);
    *result = false;  // the scene part is done: InitStaticBodies skips its own ExecuteWrite
    return true;
}
}  // namespace

bool FindPipelinedBodyCreation()
{
    if (g34Found) return true;
    if (!g.call && !Find()) return false;
    if (!FindPipelinedSceneAdd()) return Fail34("PipelinedSceneAdd's sites not found");
    const GameModule& game = Game();
    // After InitStaticBodies' creation call: test al,al; je <end>; ...; lea rax,[rip+thunk]; ...; call ExecuteWrite; <end>
    uint8_t* const after = g.call + 5;
    if (!(after[0] == 0x84 && after[1] == 0xC0 && after[2] == 0x74)) return Fail34("InitStaticBodies not as expected after the creation call");
    uint8_t* thunk = nullptr;
    uint8_t* exec = nullptr;
    for (size_t i = 4; i + 7 < 0x60 && !exec; ++i) {
        const uint8_t* p = after + i;
        if (!thunk && p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05) thunk = RipTarget(p, 3, 7);
        else if (thunk && p[0] == 0xE8) { exec = RipTarget(p, 1, 5); g34ExecCall = const_cast<uint8_t*>(p); }
    }
    if (!thunk || !exec || !game.text.Contains(thunk) || !game.text.Contains(exec)) return Fail34("ExecuteWrite call not found");
    // The je after the creation call must skip exactly that call (InitStaticBodies' end follows it).
    uint8_t* const skip = after + 4 + static_cast<int8_t>(after[3]);
    if (!(skip > after && skip <= after + 0x60 && skip[-5] == 0xE8 && RipTarget(skip - 5, 1, 5) == exec))
        return Fail34("a false creation result does not skip ExecuteWrite");
    uint8_t* const lambda = thunk[0] == 0xE9 ? RipTarget(thunk, 1, 5) : nullptr;
    if (!lambda || !game.text.Contains(lambda) || !MatchesAt(lambda, ParsePattern(kLambda)) ||
        RipTarget(lambda + kLambdaCreateCall, 1, 5) != g.create)
        return Fail34("InitBodies' lambda not as expected");
    if (!(gp.siteA > lambda && gp.siteA < lambda + 0x400)) return Fail34("AddActorsToScene's call is not in the lambda");
    if (!MatchesAt(exec, ParsePattern(kExecWrite)) || exec[0x15] != gp.solverOff) return Fail34("ExecuteWrite not as expected");
    const Pattern callable = ParsePattern(kExecCallable);
    bool calls = false;
    for (size_t i = 0x18; i < 0x50 && !calls; ++i) calls = MatchesAt(exec + i, callable);
    if (!calls) return Fail34("ExecuteWrite does not call its function");
    const Pattern post = ParsePattern(kPostTest);
    uint8_t* postAt = nullptr;
    for (uint8_t* p = gp.siteA; p < gp.siteA + 0x180 && !postAt; ++p)
        if (MatchesAt(p, post)) postAt = p;
    if (!postAt || postAt[5] < 0x40 || postAt[5] >= 0x80 || !postAt[6] || (postAt[6] & (postAt[6] - 1)))
        return Fail34("the lambda's body flag test not found");
    g34.lambda = reinterpret_cast<LambdaFn>(lambda);
    g34.execWrite = reinterpret_cast<ExecWriteFn>(exec);
    g34.postOff = postAt[5];
    g34.postBit = postAt[6];
    g34Found = true;
    Log("[bodies] pipelined creation: ExecuteWrite exe+0x%X, InitBodies' lambda exe+0x%X (thunk exe+0x%X), body flag +0x%X & 0x%X",
        static_cast<unsigned>(exec - game.base), static_cast<unsigned>(lambda - game.base), static_cast<unsigned>(thunk - game.base),
        g34.postOff, g34.postBit);
    return true;
}

void SetPipelinedBodyCreation(bool enabled)
{
    if (!g34Found || !g_installed || g34On.exchange(enabled) == enabled) return;
    Log("[bodies] pipelined creation: %s", enabled ? "on (bodies made, registered and inserted side by side)"
                                                   : "off (made, then added, as ParallelStaticBodies / 28b)");
}

void LogPipelinedBodyCreationStats()
{
    static uint64_t last[7] = {};
    const uint64_t now[7] = {s34Calls.load(), s34Bodies.load(), s34Us.load(), s34WaitUs.load(), s34TailUs.load(),
                             s34GtSlices.load(), s34Sync.load()};
    uint64_t d[7];
    bool any = false;
    for (int i = 0; i < 7; ++i) { d[i] = now[i] - last[i]; last[i] = now[i]; any |= d[i] != 0; }
    if (any)
        Log("[bodies] pipelined creation since last report: %llu calls, %llu bodies, %.1f ms (%.2f us/body; the game thread "
        "waited %.1f ms for bodies and %.1f ms for the last inserts, made %llu slices itself; %llu chunks waited for their inserts)",
        d[0], d[1], d[2] / 1000.0, d[1] ? static_cast<double>(d[2]) / static_cast<double>(d[1]) : 0.0, d[3] / 1000.0, d[4] / 1000.0,
        d[5], d[6]);
}

bool PipelinedBodyCreationInfo(uint8_t** sites, uint32_t* offsets)
{
    if (!g34Found) return false;
    sites[0] = reinterpret_cast<uint8_t*>(g34.execWrite);
    sites[1] = reinterpret_cast<uint8_t*>(g34.lambda);
    offsets[0] = g34.postOff;
    offsets[1] = g34.postBit;
    return true;
}

void PipelinedBodyCreationTestSetup(void* lambda, void* execWrite, uint32_t postOff, uint8_t postBit)
{
    g34.lambda = reinterpret_cast<LambdaFn>(lambda);
    g34.execWrite = reinterpret_cast<ExecWriteFn>(execWrite);
    g34.postOff = postOff;
    g34.postBit = postBit;
    g34Found = true;
}

}  // namespace opf
