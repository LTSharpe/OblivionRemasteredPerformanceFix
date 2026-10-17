// PhysicsPushParallel: the physics side of the game thread's push of new collision bodies, with the
// particles of one push created together and their shapes set up on several threads. Same particles, same numbering,
// same data, same order of everything the solver files.
//
// Where the time goes (Kvatch spot C, 2026-10-15, PhysicsFlushParallel and ParallelStaticBodies-31 on): when a forest streams in, the game thread
// registers a few thousand ISM static bodies per frame and pushes them to the physics side. The next physics step starts
// with FPBDRigidsSolver::ProcessPushedData_Internal (exe+0x1A2E420), which walks the push's dirty proxies
// (FDirtySet::ForEachProxy, exe+0x1A179B0) on one task worker; per new static proxy (ProcessProxyPT, exe+0x1A15990):
// CreateStaticParticles(1) (CreateParticlesHelper exe+0x1A16B70: every SOA array resized by one, a handle, a particle id)
// and UpdateViews (exe+0x178D500: ~12 particle views rebuilt), then PushToPhysicsState (static: exe+0x1A63040):
// SetParticleTransform, InvalidateParticle, SetNonFrequentData (exe+0x18E0930: geometry, shapes array, local and world
// bounds), the world-space state, the acceleration-queue entries, shape data; then RegisterParticle. 69-82 ms per C
// arrival in calls of up to 16 ms, all of it before the step's own work, while the game thread waits in its EndPhysics
// tick group and the other workers are idle. In those bursts: creation ~35 %, UpdateViews ~15 %, SetNonFrequentData
// ~40 % (sampled runs bi3/bi4, 2026-10-15).
//
// What PhysicsPushParallel changes, for each run of consecutive new static proxies in a push (transform and non-frequent data dirty,
// not skipped this step, no rewind data):
//  (1) Batch creation: at the run's first creation, the game's CreateParticlesHelper creates the whole run at once
//      (n instead of 1, the run's unique indices as the existing-index array), and UpdateViews runs once; each proxy's
//      own creation call then hands over its particle, and its UpdateViews call is skipped. The helper is the same
//      code with n: particle and handle indices in the same order (Size, AddHandles), ParticleID = BiggestParticleID++
//      in the same order, the same unique indices, HasCollision = true. Between two creations of a run, stock only
//      runs that run's own push and registration, which create no particle, generate no index and read no view (their
//      code: exe+0x1A63040, 0x1A2F0A0). The views after the run are rebuilt from the same arrays. Only array capacities
//      (growth in one step instead of n) differ.
//  (2) Parallel set-up (PhysicsPushParallel=1, runs of PhysicsPushMin+ proxies): helper threads run, per particle of
//      the run, the game's SetParticleTransform (exe+0x193F0B0: X and R; a new particle has no constraints; teleport)
//      and SetNonFrequentData with the push's own data. The push of that proxy (still on the worker, in order) then
//      waits for its particle (or does it itself) and skips its own SetNonFrequentData call; its SetParticleTransform
//      writes the same X and R again. Stock order is SetParticleTransform, InvalidateParticle, SetNonFrequentData;
//      InvalidateParticle on a new particle does nothing (no graph node, no collisions: IslandManager
//      RemoveParticleConstraints exe+0x18AB370, DestroyTransientConstraints vtable +0x10) and reads no geometry.
//      SetNonFrequentData writes only that particle's SOA slots (geometry and shared geometry, shapes array, has-bounds,
//      local bounds, CCD threshold, world bounds of particle and shapes, unique/spatial index, resim type, particle id)
//      from X, R and the push's data; the geometry is shared read-only (const implicit objects, atomic reference
//      counts); its allocations go through the game allocator (thread-safe; per-thread caches with HelperThreadCaches).
//      The helper then also runs the world-space update the push makes right after SetNonFrequentData (exe+0x18951A0,
//      from the particle's X and R, the push's unit-scale constants and no expansion: the same inputs, since nothing in
//      between changes them), and the push skips that call too.
//      Everything that is shared or ordered (acceleration-queue entries, registration, the proxy tables, the dirty
//      lists, the island graph) stays in the worker's stock sequence.
// So: identical results; only heap addresses and array capacities differ, neither of which is deterministic in stock.
//
// PhysicsPushGrowParallel (needs PhysicsPushParallel on): the batch creation's array growth on several threads. The helper's
// first step (exe+0x1A16B96) is the SOA's AddParticles(n): MSize += n, then each of the ~50 particle arrays'
// virtual Resize(MSize) (TArrayCollectionArray<T>::Resize = TArray::SetNum), one after another; then the same for the
// particle-handle arrays (SOAs +0x658). An array whose capacity runs out is reallocated and copied (with the new pages
// faulted in): ~2.5 of the ~3.2 ms of a 4.8k-statics batch creation at Kvatch C. With 33b on, just before the batch
// creation, the arrays that will be reallocated (capacity < MSize + n) get that same Resize(MSize + n) call on the fix's
// helpers and the push thread side by side; the helper's own calls afterwards find each array at that size already
// (SetNum to its current size does nothing). Each Resize touches only its own array (and the thread-safe allocator); the
// SOA is only used by this push's thread at that point (PhysicsPushParallel's helpers have left their run). Checked live: every array
// at the collection's size before (else no pre-growth), at the new size after, and its header (data, size, capacity)
// unchanged by the helper's own call (else 33b goes off for the session with a log line). Same contents, same calls.
//
// Sites (byte patterns; a mismatch turns the fix off with a log line): ProcessPushedData's call of ForEachProxy, the
// static creation and UpdateViews calls in ProcessProxyPT, the static push's SetNonFrequentData and world-space calls.
// Installed at start-up (five calls redirected); afterwards the ini switches it live (0 off, 1 batch + parallel, 2 batch only,
// 3 = 1 plus a check: the push's own world-space update runs again and must give the same bits; a "[push] verify" line).
#include "common.h"

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace opf {
namespace {

// ProcessPushedData_Internal (exe+0x1A2E420): mov [rbp-19h],rsi; mov [rbp-9],r15; call ForEachProxy<lambda>;
// lea rdx,[rbp-19h]; mov [rbp-19h],rsi; lea rcx,[r15+x]; mov [rbp-11h],r15; mov [rbp-9],rbx; call (constraints pass)
constexpr char kPushCall[] =
    "48 89 75 E7 4C 89 7D F7 E8 ?? ?? ?? ?? 48 8D 55 E7 48 89 75 E7 49 8D 8F ?? ?? 00 00 4C 89 7D EF 48 89 5D F7 E8";
constexpr size_t kPushCallAt = 8;
// ForEachProxy<lambda> (exe+0x1A179B0): prologue; lea rax,[rcx+Buckets]; mov r12,rdx; mov ecx,NumBuckets
constexpr char kForEach[] = "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 48 8D 41 ?? 4C 8B E2 B9 ?? 00 00 00";
// ... mov eax,[rdx+CurrentFrame]; cmp [rdi+IgnoreStep],eax
constexpr char kForEachFrame[] = "8B 82 ?? ?? 00 00 39 47 ??";
// ... mov eax,[rdi+Type]; add eax,-2; cmp eax,N; ja; mov ecx,[r8+rax*4+table]; add rcx,r8; jmp rcx
constexpr char kForEachSwitch[] = "8B 47 ?? 83 C0 FE 83 F8 ?? 0F 87 ?? ?? ?? ?? 41 8B 8C 80 ?? ?? ?? ?? 49 03 C8 FF E1";
// ProcessProxyPT (exe+0x1A15990), the new-proxy part: init step test, non-frequent data (Manager+0x40, 0x28 stride, unique
// index +0x10), the buffer type (+0x0C, static = 0)
constexpr char kProxyNew[] =
    "8B 5D 3C 89 5C 24 30 83 FB FF 0F 85 ?? ?? ?? ?? 41 F6 41 08 10 4C 89 78 D8 74 18 48 8B 01 4A 8D 14 AD 00 00 00 00 "
    "49 03 D5 48 8B 48 40 48 8D 04 D1 EB 03 48 8B C6 48 8B 94 24 ?? ?? 00 00 4C 8D 78 10 48 85 C0 4C 0F 44 FE 48 8B 42 08 "
    "0F B6 48 0C 85 C9 0F 84";
// ... the static case: mov rbx,[rdx]; lea rax,[rsp+params]; add rbx,Particles; ...; mov r8d,1; call Create;
// mov rcx,rbx; call UpdateViews; mov rcx,[rsp+out]; mov rbx,[rcx]; call Free
constexpr char kCreate[] =
    "48 8B 1A 48 8D 84 24 ?? ?? 00 00 48 81 C3 ?? ?? 00 00 48 89 44 24 28 48 8B CB 48 89 5C 24 20 4D 8B CF 48 8D 54 24 ?? "
    "41 B8 01 00 00 00 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 4C 24 ?? 48 8B 19 E8";
constexpr size_t kCreateParticles = 0x0E, kCreateCall = 0x2D, kViewsCall = 0x35, kFreeCall = 0x42;
// ... and its PushToPhysicsState call (rewind data at capture +0x10 first): the last call
constexpr char kPushToPhysics[] =
    "48 39 70 50 74 ?? 48 8B 4F 10 48 85 C9 74 ?? 48 8B 47 08 4D 8B CC 48 8B 17 45 8B C5 48 89 44 24 20 E8 ?? ?? ?? ?? "
    "F2 0F 10 47 18 4D 8B CC 48 8B 47 08 45 8B C5 48 8B 17 49 8B 0E F2 0F 11 44 24 28 48 89 44 24 20 E8";
constexpr size_t kPushToPhysicsCall = 70;
// CreateParticlesHelper<static> (exe+0x1A16B70): prologue; ...; mov [r15+8],edi; test edi,edi; je; xor edx,edx;
// mov rcx,r15; call ResizeGrow (the returned array)
constexpr char kHelper[] = "4C 89 4C 24 20 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 58";
constexpr char kHelperGrow[] = "41 89 7F 08 85 FF 74 ?? 33 D2 49 8B CF E8";
// PushToPhysicsState (exe+0x1A6C600): the rewind test (solver +0x1B0), then the buffer type switch (static = je)
constexpr char kDispatch[] =
    "48 89 5C 24 08 57 48 83 EC 40 48 8B 59 08 48 8B FA 4C 8B D9 4C 8B 93 ?? ?? 00 00 4D 85 D2 74 ?? 41 8B 82 ?? ?? 00 00 "
    "41 39 82 ?? ?? 00 00 7D ?? 8B 41 3C 39 83 ?? ?? 00 00 75 ?? B2 01 EB 02 32 D2 41 0F B6 49 0C 85 C9 74";
constexpr char kDispatchStatic[] =
    "F2 0F 10 44 24 78 48 8B CF 48 8B 44 24 70 F2 0F 11 44 24 38 88 54 24 30 49 8B 53 50 48 89 5C 24 28 48 89 44 24 20 E8";
// The static PushToPhysicsStateImp (exe+0x1A63040) at +0x4C: evolution (solver +0x930), transform (XR = Manager[0] +
// i*0x40, R at +0x20) and non-frequent data (Manager+0x40 + i*0x28) of the push, SetParticleTransform (teleport),
// InvalidateParticle (island manager, vtable +0x10), SetNonFrequentData
constexpr char kStaticPush[] =
    "4C 8B B8 ?? ?? 00 00 4C 89 7D ?? 48 89 4D ?? 4C 89 6C 24 ?? 74 08 49 8B CF E8 ?? ?? ?? ?? 41 8B 4C 24 08 48 8B C7 "
    "F6 C1 01 74 0C 48 8B DF 48 C1 E3 06 49 03 1E EB 02 33 DB F6 C1 10 74 0E 48 8D 0C 80 49 8B 46 40 48 8D 3C C8 EB 02 "
    "33 FF 48 89 7D ?? 48 85 DB 74 17 4C 8D 4B 20 C6 44 24 20 01 4C 8B C3 48 8B D6 49 8B CF E8 ?? ?? ?? ?? 48 85 FF 74 26 "
    "49 8D 8F 00 01 00 00 48 8B D6 E8 ?? ?? ?? ?? 49 8B 07 48 8B D6 49 8B CF FF 50 10 48 8B D7 48 8B CE E8";
constexpr size_t kStaticPushAt = 0x4C, kStaticEvolution = 3, kStaticSpt = 105, kStaticSnfd = 148;
// ... right after SetNonFrequentData: the world-space state from the particle's X (SOA +0x28) and R (+0x68), unit scale
// (two constants), zero bounds expansion: UpdateWorldSpaceState(SOA, index, &transform, &expansion)
constexpr char kStaticUws[] =
    "48 85 DB 75 ?? 48 85 FF 0F 84 ?? ?? ?? ?? 4C 63 5E 08 4C 8D 4D ?? 4C 8B 16 4C 8D 45 ?? 0F 28 05 ?? ?? ?? ?? 49 8B CB "
    "0F 28 0D ?? ?? ?? ?? 4B 8D 14 5B 48 C1 E1 05 49 03 4A 68 49 8B 42 28 0F 29 B4 24 ?? ?? 00 00 0F 57 F6 F2 0F 11 75 ?? "
    "0F 10 19 0F 10 61 10 49 8B CA 66 0F 10 2C D0 F2 0F 10 54 D0 10 41 8B D3 0F 29 45 00 0F 57 C0 0F 11 45 ?? 0F 29 5D ?? "
    "0F 29 65 ?? 0F 29 6D ?? 0F 29 55 ?? 0F 29 4D 10 E8";
constexpr size_t kUwsScaleA = 29, kUwsScaleB = 39, kUwsX = 0x28, kUwsR = 0x68;

struct UeArray { uint8_t* data; int32_t num; int32_t max; };
using ForEachFn = void (*)(uint8_t* dirtySet, void** capture);
using CreateFn = UeArray* (*)(uint8_t* particles, UeArray* out, int n, const int32_t* uniqueIdx, void* soa, void* params);
using ViewsFn = void (*)(uint8_t* particles);
using SnfdFn = void (*)(void* handle, const void* nonFrequent);
using SptFn = void (*)(void* evolution, void* handle, const void* x, const void* r, uint8_t teleport);
using GrowFn = void (*)(UeArray* array, int32_t oldNum);
using UwsFn = void (*)(uint8_t* soa, int32_t index, const void* transform, const void* expansion);
using JobUwsFn = void (*)(void* handle);
using FreeFn = void (*)(void* p);

struct Layout {
    uint32_t buckets = 0x38, numBuckets = 13, entrySize = 32, entryFlags = 8, entryType = 0x0C;
    uint32_t proxyType = 0x38, proxyInit = 0x3C, proxyIgnore = 0x40;
    uint32_t solverFrame = 0x248, solverParticles = 0x290, solverEvolution = 0x930, solverRewind = 0x1B0;
    uint32_t xrStride = 0x40, xrR = 0x20, nfd = 0x40, nfdStride = 0x28, nfdUnique = 0x10;
    int32_t singleType = 0x0B;
};
constexpr uint32_t kFlagXR = 0x01, kFlagNonFrequent = 0x10;

struct Sites {
    uint8_t* pushCall = nullptr;   // ProcessPushedData -> ForEachProxy
    uint8_t* createCall = nullptr; // ProcessProxyPT -> CreateParticlesHelper<static>
    uint8_t* viewsCall = nullptr;  // ProcessProxyPT -> UpdateViews (after the static and kinematic creations)
    uint8_t* snfdCall = nullptr;   // static push -> SetNonFrequentData
    uint8_t* uwsCall = nullptr;    // static push -> UpdateWorldSpaceState (right after)
    ForEachFn forEach = nullptr;
    CreateFn create = nullptr;
    ViewsFn views = nullptr;
    SnfdFn snfd = nullptr;
    SptFn spt = nullptr;
    GrowFn grow = nullptr;
    FreeFn free = nullptr;
    UwsFn uws = nullptr;
    JobUwsFn jobUws = nullptr;     // the job's world-space update (the game's, as the push would call it)
    alignas(16) uint8_t scale[32] = {};  // the push's two transform constants (unit scale)
    Layout lay;
} g;

bool g_installed = false;
std::atomic<int> g_mode{0};   // 0 off, 1 batch + parallel, 2 batch only
int g_minParallel = 32;
constexpr int kMinBatch = 2;
std::atomic<bool> g_broken{false};  // an unexpected call order: the fix stays off for the rest of the session
std::atomic<bool> g_testJitter{false};

// ---- helper pool: K threads that set up the particles of the current run ----
constexpr int kMaxThreads = 8;
struct Job {
    void* handle;
    const uint8_t* xr;
    const uint8_t* nonFrequent;
    void* evolution;
    std::atomic<int> state;  // 0 waiting, 1 taken, 2 done
};
struct Run {
    Job* jobs = nullptr;
    int count = 0;
    std::atomic<int> next{0};
};
Run g_run;
std::vector<Job> g_jobs;
HANDLE g_wake = nullptr;     // semaphore: one release per helper asked to help (extra releases are harmless)
int g_threads = 0;
std::atomic<bool> g_poolBusy{false};  // one run at a time (one solver pushes at a time; a guard for other worlds)

// A helper takes part in a run only while the run is open: it enters (the active count, bits 0-30) only if the open bit
// (bit 31) is set, and reads the run's fields only between entering and leaving. The run's thread closes the run and
// then waits only for helpers inside it, never for a woken helper that has not been scheduled yet (that one finds the
// run closed, or enters a later run, which is as good).
constexpr uint32_t kRunOpen = 0x80000000u;
std::atomic<uint32_t> g_runGate{0};
std::atomic<int> g_testHoldMs{0};  // self-test only: helpers sleep this long after each wake (late helpers)
WaitGate g_drainGate;  // the run's thread waiting for the last helper inside to leave
WaitGate g_jobGate;    // the push thread waiting for a job a helper is running

bool EnterRun()
{
    uint32_t c = g_runGate.load(std::memory_order_acquire);
    for (;;) {
        if (!(c & kRunOpen)) return false;
        if (g_runGate.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel, std::memory_order_acquire)) return true;
    }
}
void LeaveRun()
{
    g_runGate.fetch_sub(1, std::memory_order_acq_rel);
    WakeWord(g_runGate, g_drainGate);
}
void OpenRun() { g_runGate.store(kRunOpen, std::memory_order_release); }  // the run's fields are set; nobody is inside
void CloseRun()
{
    g_runGate.fetch_and(~kRunOpen, std::memory_order_acq_rel);
    WaitWord(g_runGate, g_drainGate, [](uint32_t v) { return v == 0; });
}

void Jitter(uint32_t& rnd)
{
    if (!g_testJitter.load(std::memory_order_relaxed)) return;
    rnd = rnd * 1664525u + 1013904223u;
    switch (rnd >> 29) {
        case 0: case 1: Sleep(0); break;
        case 2: SwitchToThread(); break;
        case 3: if ((rnd & 0xFF) < 32) Sleep(1); break;  // a late or preempted helper
        default: break;
    }
}

// The world-space update exactly as the static push calls it after SetNonFrequentData: the transform from the particle's
// X and R and the unit scale, no expansion.
void GameJobUws(void* handle)
{
    uint8_t* const soa = *static_cast<uint8_t**>(handle);
    const int32_t idx = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(handle) + 8);
    alignas(16) uint8_t xform[0x60] = {};
    alignas(16) uint8_t expansion[0x18] = {};
    memcpy(xform, *reinterpret_cast<uint8_t**>(soa + kUwsR) + static_cast<size_t>(idx) * 0x20, 0x20);
    memcpy(xform + 0x20, *reinterpret_cast<uint8_t**>(soa + kUwsX) + static_cast<size_t>(idx) * 0x18, 0x18);
    memcpy(xform + 0x40, g.scale, 0x20);
    g.uws(soa, idx, xform, expansion);
}

void RunJob(Job& j)
{
    const uint8_t* const xr = j.xr;
    g.spt(j.evolution, j.handle, xr, xr + g.lay.xrR, 1);
    g.snfd(j.handle, j.nonFrequent);
    g.jobUws(j.handle);
}

DWORD WINAPI Helper(void*)
{
    uint32_t rnd = GetCurrentThreadId();
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        HelperThreadCache();  // the game allocator's per-thread cache (HelperThreadCaches), between runs only
        if (const int ms = g_testHoldMs.load(std::memory_order_relaxed)) Sleep(static_cast<DWORD>(ms));
        Jitter(rnd);
        if (!EnterRun()) continue;
        for (;;) {
            Jitter(rnd);
            const int i = g_run.next.fetch_add(1, std::memory_order_relaxed);
            if (i >= g_run.count) break;
            Job& j = g_run.jobs[i];
            int expected = 0;
            if (!j.state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) continue;
            RunJob(j);
            Jitter(rnd);
            j.state.store(2, std::memory_order_release);
            WakeWord(j.state, g_jobGate);
        }
        LeaveRun();
    }
}

// ---- per push (thread of the physics step) ----
struct Span { int bucket, begin, count; };
struct PushState {
    uint8_t* solver = nullptr;
    uint8_t* particles = nullptr;
    const uint8_t* manager = nullptr;
    void* evolution = nullptr;
    int mode = 0;
    std::vector<Span> runs;
    size_t nextRun = 0;
    bool serving = false;     // handing out the particles of runs[nextRun]
    int pos = 0;
    std::vector<void*> handles;
    bool skipViews = false;
    bool parallel = false;    // the current (or last) run's set-up went to the pool
    int jobNext = 0, jobCount = 0;
    int newStatics = 0;       // eligible new static proxies in this push
    const uint8_t* uwsSoa = nullptr;  // the particle whose world-space update the job did (the push's next call)
    int32_t uwsIdx = -1;
    uint64_t createUs = 0, waitUs = 0;
};
thread_local PushState* t_push = nullptr;

// statistics: pushes with at least kMinBatch new statics, by mode at the time
std::atomic<uint64_t> s_pushOn{0}, s_usOn{0}, s_newOn{0}, s_pushOff{0}, s_usOff{0}, s_newOff{0};
std::atomic<uint64_t> s_batched{0}, s_runs{0}, s_parallel{0}, s_selfDone{0}, s_waitUs{0}, s_maxUsOn{0}, s_maxUsOff{0};
std::atomic<uint64_t> s_anomaly{0}, s_verified{0}, s_mismatch{0};

uint64_t NowUs()
{
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return QpcToMicroseconds(static_cast<uint64_t>(c.QuadPart), freq);
}

template <class T> T At(const void* base, uint32_t off) { T v; memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof v); return v; }

const uint8_t* Entry(const uint8_t* dirtySet, int bucket, int i)
{
    const uint8_t* const b = dirtySet + g.lay.buckets + static_cast<size_t>(bucket) * 16;
    return At<const uint8_t*>(b, 0) + static_cast<size_t>(i) * g.lay.entrySize;
}
const uint8_t* NonFrequent(const PushState& s, int i)
{
    return At<const uint8_t*>(s.manager, g.lay.nfd) + static_cast<size_t>(i) * g.lay.nfdStride;
}
const int32_t* UniquePtr(const PushState& s, int i)
{
    return reinterpret_cast<const int32_t*>(NonFrequent(s, i) + g.lay.nfdUnique);
}
const uint8_t* XR(const PushState& s, int i)
{
    return At<const uint8_t*>(s.manager, 0) + static_cast<size_t>(i) * g.lay.xrStride;
}

// The runs of one push: consecutive entries of one bucket that stock handles as new static single-particle proxies with
// transform and non-frequent data. Single-particle proxies in more than one bucket (never seen): no runs.
void Scan(PushState& s, const uint8_t* dirtySet)
{
    const int32_t frame = At<int32_t>(s.solver, g.lay.solverFrame);
    int singleBucket = -1;
    for (int b = 0; b < static_cast<int>(g.lay.numBuckets); ++b) {
        const int n = At<int32_t>(dirtySet + g.lay.buckets + static_cast<size_t>(b) * 16, 8);
        Span cur{b, 0, 0};
        for (int i = 0; i < n; ++i) {
            const uint8_t* const e = Entry(dirtySet, b, i);
            const uint8_t* const proxy = At<const uint8_t*>(e, 0);
            const bool single = At<int32_t>(proxy, g.lay.proxyType) == g.lay.singleType;
            if (single) {
                if (singleBucket >= 0 && singleBucket != b) { s.runs.clear(); s.newStatics = 0; return; }
                singleBucket = b;
            }
            const uint32_t flags = At<uint32_t>(e, g.lay.entryFlags);
            const bool ok = single && At<int32_t>(proxy, g.lay.proxyIgnore) != frame && At<int32_t>(proxy, g.lay.proxyInit) == -1 &&
                            e[g.lay.entryType] == 0 && (flags & (kFlagXR | kFlagNonFrequent)) == (kFlagXR | kFlagNonFrequent);
            if (ok) {
                if (cur.count == 0) cur.begin = i;
                ++cur.count;
                ++s.newStatics;
            }
            if (!ok || i + 1 == n) {
                if (cur.count >= kMinBatch) s.runs.push_back(cur);
                cur.count = 0;
            }
        }
    }
}

// Closes the current run and waits for the helpers inside it (then nothing of the run runs any more).
void FinishPoolRun(PushState& s)
{
    if (!s.parallel) return;
    CloseRun();
    g_run.count = 0;
    s.parallel = false;
    g_poolBusy.store(false, std::memory_order_release);
}

void StartPoolRun(PushState& s, const Span& r)
{
    if ((s.mode != 1 && s.mode != 3) || r.count < g_minParallel || g_threads == 0) return;
    bool expected = false;
    if (!g_poolBusy.compare_exchange_strong(expected, true, std::memory_order_acquire)) return;
    if (g_jobs.size() < static_cast<size_t>(r.count)) g_jobs = std::vector<Job>(static_cast<size_t>(r.count) + 256);
    for (int k = 0; k < r.count; ++k) {
        Job& j = g_jobs[static_cast<size_t>(k)];
        j.handle = s.handles[static_cast<size_t>(k)];
        j.xr = XR(s, r.begin + k);
        j.nonFrequent = NonFrequent(s, r.begin + k);
        j.evolution = s.evolution;
        j.state.store(0, std::memory_order_relaxed);
    }
    g_run.jobs = g_jobs.data();
    g_run.count = r.count;
    g_run.next.store(0, std::memory_order_relaxed);
    const int want = (r.count + 15) / 16;  // at least 16 particles per helper
    const int wake = want < g_threads ? want : g_threads;
    OpenRun();
    s.parallel = true;
    s.jobNext = 0;
    s.jobCount = r.count;
    ReleaseSemaphore(g_wake, wake, nullptr);
    s_parallel.fetch_add(static_cast<uint64_t>(r.count), std::memory_order_relaxed);
}

void Broken(const char* what)
{
    s_anomaly.fetch_add(1, std::memory_order_relaxed);
    if (!g_broken.exchange(true)) Log("[push] unexpected call order (%s): PhysicsPushParallel off for this session", what);
}

// ---- the four redirected calls ----
UeArray* CreateStub(uint8_t* particles, UeArray* out, int n, const int32_t* uniqueIdx, void* soa, void* params)
{
    PushState* const s = t_push;
    if (!s || n != 1 || particles != s->particles || !uniqueIdx) return g.create(particles, out, n, uniqueIdx, soa, params);
    if (!s->serving) {
        if (s->nextRun >= s->runs.size() || uniqueIdx != UniquePtr(*s, s->runs[s->nextRun].begin))
            return g.create(particles, out, n, uniqueIdx, soa, params);
        // The run's first creation: create the whole run in one call of the game's helper.
        const Span& r = s->runs[s->nextRun];
        std::vector<int32_t> idx(static_cast<size_t>(r.count));
        for (int k = 0; k < r.count; ++k) idx[static_cast<size_t>(k)] = *UniquePtr(*s, r.begin + k);
        UeArray all{nullptr, 0, 0};
        const uint64_t tc = NowUs();
        FinishPoolRun(*s);  // the previous run's helpers have left (its set-ups were all waited for by its pushes)
        g.create(particles, &all, r.count, idx.data(), soa, params);
        if (all.num != r.count || !all.data) {
            Broken("batch creation");  // cannot happen with the game's helper; the particles exist, hand them out anyway
        }
        s->handles.assign(reinterpret_cast<void**>(all.data), reinterpret_cast<void**>(all.data) + all.num);
        if (all.data) g.free(all.data);
        g.views(particles);
        s->createUs += NowUs() - tc;
        s->serving = true;
        s->pos = 0;
        s_runs.fetch_add(1, std::memory_order_relaxed);
        s_batched.fetch_add(static_cast<uint64_t>(r.count), std::memory_order_relaxed);
        FinishPoolRun(*s);
        StartPoolRun(*s, r);
    } else if (uniqueIdx != UniquePtr(*s, s->runs[s->nextRun].begin + s->pos)) {
        Broken("creation out of order");
    }
    // Hand over this proxy's particle in a one-element array, as the game's helper returns it.
    out->data = nullptr;
    out->max = 0;
    out->num = 1;
    g.grow(out, 0);
    reinterpret_cast<void**>(out->data)[0] = s->pos < static_cast<int>(s->handles.size()) ? s->handles[static_cast<size_t>(s->pos)] : nullptr;
    s->skipViews = true;
    if (++s->pos == s->runs[s->nextRun].count) {
        s->serving = false;
        ++s->nextRun;
    }
    return out;
}

void ViewsStub(uint8_t* particles)
{
    PushState* const s = t_push;
    if (s && s->skipViews) {
        s->skipViews = false;
        return;  // the run's views were rebuilt once, after its batch creation
    }
    g.views(particles);
}

void SnfdStub(void* handle, const void* nonFrequent)
{
    PushState* const s = t_push;
    if (s && s->parallel && s->jobNext < s->jobCount) {
        Job& j = g_run.jobs[s->jobNext];
        if (j.handle == handle && j.nonFrequent == nonFrequent) {
            ++s->jobNext;
            int expected = 0;
            if (j.state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
                RunJob(j);  // not taken yet: do it here
                j.state.store(2, std::memory_order_relaxed);
                s_selfDone.fetch_add(1, std::memory_order_relaxed);
            } else if (j.state.load(std::memory_order_acquire) != 2) {
                const uint64_t t0 = NowUs();
                WaitWord(j.state, g_jobGate, [](int v) { return v == 2; });
                s_waitUs.fetch_add(NowUs() - t0, std::memory_order_relaxed);
                s->waitUs += NowUs() - t0;
            }
            s->uwsSoa = *static_cast<uint8_t**>(handle);
            s->uwsIdx = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(handle) + 8);
            if (s->jobNext == s->jobCount) FinishPoolRun(*s);
            return;  // set up by the job, as this call would have done
        }
    }
    g.snfd(handle, nonFrequent);
}

// Mode 3 (diagnostic): the push's own world-space update runs as well, and its results (the particle's world bounds, SOA
// +0x170, and each shape's world bounds, +0x18..+0x48) must equal the job's bit for bit.
void VerifyUws(uint8_t* soa, int32_t idx, const void* transform, const void* expansion)
{
    uint8_t* const bounds = *reinterpret_cast<uint8_t**>(soa + 0x170) + static_cast<size_t>(idx) * 0x30;
    const uint8_t* const arr = *reinterpret_cast<uint8_t**>(soa + 0x128) + static_cast<size_t>(idx) * 0x18;
    uint8_t* const* shapes = reinterpret_cast<uint8_t* const*>(At<const uint8_t*>(arr, 8) ? At<const uint8_t*>(arr, 8) : arr);
    const int n = At<int32_t>(arr, 0x10);
    std::vector<uint8_t> before(0x30 + static_cast<size_t>(n > 0 ? n : 0) * 0x30);
    memcpy(before.data(), bounds, 0x30);
    for (int k = 0; k < n; ++k) memcpy(before.data() + 0x30 + k * 0x30, shapes[k] + 0x18, 0x30);
    g.uws(soa, idx, transform, expansion);
    bool same = memcmp(before.data(), bounds, 0x30) == 0;
    for (int k = 0; k < n && same; ++k) same = memcmp(before.data() + 0x30 + k * 0x30, shapes[k] + 0x18, 0x30) == 0;
    s_verified.fetch_add(1, std::memory_order_relaxed);
    if (!same) s_mismatch.fetch_add(1, std::memory_order_relaxed);
}

void UwsStub(uint8_t* soa, int32_t idx, const void* transform, const void* expansion)
{
    PushState* const s = t_push;
    if (s && s->uwsSoa == soa && s->uwsIdx == idx) {
        s->uwsSoa = nullptr;
        if (s->mode == 3) VerifyUws(soa, idx, transform, expansion);
        return;  // done by the job right after SetNonFrequentData, from the same X, R, shapes and constants
    }
    g.uws(soa, idx, transform, expansion);
}

void MaxStore(std::atomic<uint64_t>& m, uint64_t v)
{
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

void ForEachHook(uint8_t* dirtySet, void** capture)
{
    const int mode = g_broken.load(std::memory_order_relaxed) ? 0 : g_mode.load(std::memory_order_relaxed);
    void** const pp = capture ? static_cast<void**>(capture[1]) : nullptr;
    if (t_push || !pp) { g.forEach(dirtySet, capture); return; }  // nested (never seen): stock
    PushState s;
    s.solver = static_cast<uint8_t*>(capture[0]);
    s.particles = s.solver + g.lay.solverParticles;
    s.manager = static_cast<const uint8_t*>(pp[0]);
    s.evolution = At<void*>(s.solver, g.lay.solverEvolution);
    s.mode = mode;
    const bool rewind = pp[2] != nullptr || At<void*>(s.solver, g.lay.solverRewind) != nullptr;
    if (!rewind) Scan(s, dirtySet);
    const bool counted = s.newStatics >= kMinBatch;
    const uint64_t t0 = counted ? NowUs() : 0;
    if (mode == 0 || s.runs.empty()) {
        g.forEach(dirtySet, capture);
    } else {
        t_push = &s;
        g.forEach(dirtySet, capture);
        t_push = nullptr;
        if (s.parallel) {
            if (s.jobNext != s.jobCount) Broken("a particle's set-up was not taken");
            for (int k = 0; k < s.jobCount; ++k)  // never needed: every job was taken above
                WaitWord(g_run.jobs[k].state, g_jobGate, [](int v) { return v != 1; });
            FinishPoolRun(s);
        }
        if (s.serving || s.nextRun != s.runs.size()) Broken("a run was not created in full");
    }
    if (!counted) return;
    const uint64_t dt = NowUs() - t0;
    (mode ? s_pushOn : s_pushOff).fetch_add(1, std::memory_order_relaxed);
    (mode ? s_usOn : s_usOff).fetch_add(dt, std::memory_order_relaxed);
    (mode ? s_newOn : s_newOff).fetch_add(static_cast<uint64_t>(s.newStatics), std::memory_order_relaxed);
    MaxStore(mode ? s_maxUsOn : s_maxUsOff, dt);
}

bool Fail(const char* what)
{
    Log("[push] %s: PhysicsPushParallel unavailable", what);
    return false;
}

bool RedirectCall(uint8_t* call, void* target)
{
    if (call[0] != 0xE8) return false;
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

uint8_t* Search(uint8_t* from, size_t len, const Pattern& p)
{
    for (size_t i = 0; i + p.size() <= len; ++i)
        if (MatchesAt(from + i, p)) return from + i;
    return nullptr;
}
}  // namespace

bool FindPhysicsPush()
{
    if (g.pushCall) return true;
    const GameModule& game = Game();
    Sites s;
    // 1. ProcessPushedData's call of ForEachProxy, and ForEachProxy itself.
    const std::vector<uint8_t*> calls = FindAll(game.text, ParsePattern(kPushCall));
    if (calls.size() != 1) return Fail(calls.empty() ? "ProcessPushedData's call not found" : "several ProcessPushedData candidates");
    s.pushCall = calls[0] + kPushCallAt;
    uint8_t* const fe = RipTarget(s.pushCall, 1, 5);
    if (!game.text.Contains(fe) || !MatchesAt(fe, ParsePattern(kForEach))) return Fail("ForEachProxy not as expected");
    s.forEach = reinterpret_cast<ForEachFn>(fe);
    s.lay.buckets = fe[0x17];
    s.lay.numBuckets = At<uint32_t>(fe, 0x1C);
    uint8_t* const fr = Search(fe, 0x100, ParsePattern(kForEachFrame));
    uint8_t* const sw = Search(fe, 0x100, ParsePattern(kForEachSwitch));
    if (!fr || !sw || s.lay.numBuckets == 0 || s.lay.numBuckets > 64) return Fail("ForEachProxy's loop not as expected");
    s.lay.solverFrame = At<uint32_t>(fr, 2);
    s.lay.proxyIgnore = fr[8];
    s.lay.proxyType = sw[2];
    const uint32_t cases = sw[8];
    const uint32_t table = At<uint32_t>(sw, 0x13);
    if (static_cast<uint32_t>(s.lay.singleType - 2) > cases) return Fail("the proxy type switch has no single-particle case");
    uint8_t* const caseBlock = game.base + At<uint32_t>(game.base + table, 4 * static_cast<uint32_t>(s.lay.singleType - 2));
    // 2. ProcessProxyPT: the call in the single-particle case; its new-proxy part, static creation, UpdateViews, free.
    uint8_t* const ppCall = Search(caseBlock, 0x40, ParsePattern("E8"));
    uint8_t* const pp = ppCall ? RipTarget(ppCall, 1, 5) : nullptr;
    if (!pp || !game.text.Contains(pp)) return Fail("the single-particle case not as expected");
    uint8_t* const create = Search(pp, 0x300, ParsePattern(kCreate));
    uint8_t* const fresh = Search(pp, 0x300, ParsePattern(kProxyNew));
    uint8_t* const push = Search(pp, 0x400, ParsePattern(kPushToPhysics));
    if (!create || !fresh || fresh > create || !push) return Fail("ProcessProxyPT not as expected");
    s.lay.solverParticles = At<uint32_t>(create, kCreateParticles);
    s.createCall = create + kCreateCall;
    s.viewsCall = create + kViewsCall;
    s.create = reinterpret_cast<CreateFn>(RipTarget(s.createCall, 1, 5));
    s.views = reinterpret_cast<ViewsFn>(RipTarget(s.viewsCall, 1, 5));
    s.free = reinterpret_cast<FreeFn>(RipTarget(create + kFreeCall, 1, 5));
    uint8_t* const helper = reinterpret_cast<uint8_t*>(s.create);
    if (!MatchesAt(helper, ParsePattern(kHelper))) return Fail("CreateParticlesHelper not as expected");
    uint8_t* const grow = Search(helper, 0x100, ParsePattern(kHelperGrow));
    if (!grow) return Fail("CreateParticlesHelper's array not as expected");
    s.grow = reinterpret_cast<GrowFn>(RipTarget(grow + 13, 1, 5));
    // 3. PushToPhysicsState -> the static PushToPhysicsStateImp: SetParticleTransform, SetNonFrequentData.
    uint8_t* const dispatch = RipTarget(push + kPushToPhysicsCall, 1, 5);
    const Pattern dp = ParsePattern(kDispatch);
    if (!game.text.Contains(dispatch) || !MatchesAt(dispatch, dp)) return Fail("PushToPhysicsState not as expected");
    s.lay.solverRewind = At<uint32_t>(dispatch, 23);
    uint8_t* const staticCase = dispatch + dp.size() + 1 + static_cast<int8_t>(dispatch[dp.size()]);
    if (!MatchesAt(staticCase, ParsePattern(kDispatchStatic))) return Fail("PushToPhysicsState's static case not as expected");
    uint8_t* const sp = RipTarget(staticCase + 38, 1, 5);
    if (!game.text.Contains(sp) || !MatchesAt(sp + kStaticPushAt, ParsePattern(kStaticPush)))
        return Fail("the static PushToPhysicsState not as expected");
    s.lay.solverEvolution = At<uint32_t>(sp + kStaticPushAt, kStaticEvolution);
    s.spt = reinterpret_cast<SptFn>(RipTarget(sp + kStaticPushAt + kStaticSpt, 1, 5));
    s.snfdCall = sp + kStaticPushAt + kStaticSnfd;
    s.snfd = reinterpret_cast<SnfdFn>(RipTarget(s.snfdCall, 1, 5));
    const Pattern up = ParsePattern(kStaticUws);
    uint8_t* const uwsAt = s.snfdCall + 5;
    if (!MatchesAt(uwsAt, up)) return Fail("the static push's world-space update not as expected");
    s.uwsCall = uwsAt + up.size() - 1;
    s.uws = reinterpret_cast<UwsFn>(RipTarget(s.uwsCall, 1, 5));
    const uint8_t* const scaleA = RipTarget(uwsAt + kUwsScaleA, 3, 7);
    const uint8_t* const scaleB = RipTarget(uwsAt + kUwsScaleB, 3, 7);
    if (!game.rdata.Contains(scaleA) || !game.rdata.Contains(scaleB)) return Fail("the push's transform constants not found");
    memcpy(s.scale, scaleA, 16);
    memcpy(s.scale + 16, scaleB, 16);
    s.jobUws = &GameJobUws;
    for (void* f : {reinterpret_cast<void*>(s.create), reinterpret_cast<void*>(s.views), reinterpret_cast<void*>(s.free),
                    reinterpret_cast<void*>(s.grow), reinterpret_cast<void*>(s.spt), reinterpret_cast<void*>(s.snfd),
                    reinterpret_cast<void*>(s.uws)})
        if (!game.text.Contains(f)) return Fail("a call target outside the game's code");
    g = s;
    Log("[push] ForEachProxy call exe+0x%X (exe+0x%X; buckets +0x%X x%u, frame +0x%X, single-particle case 0x%X), "
        "ProcessProxyPT exe+0x%X: create exe+0x%X -> exe+0x%X, UpdateViews exe+0x%X -> exe+0x%X; static push exe+0x%X: "
        "SetNonFrequentData exe+0x%X -> exe+0x%X, UpdateWorldSpaceState exe+0x%X -> exe+0x%X, SetParticleTransform exe+0x%X; "
        "particles +0x%X, evolution +0x%X"
        ,
        static_cast<unsigned>(g.pushCall - game.base), static_cast<unsigned>(fe - game.base), g.lay.buckets, g.lay.numBuckets,
        g.lay.solverFrame, g.lay.singleType, static_cast<unsigned>(pp - game.base), static_cast<unsigned>(g.createCall - game.base),
        static_cast<unsigned>(helper - game.base), static_cast<unsigned>(g.viewsCall - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(g.views) - game.base), static_cast<unsigned>(sp - game.base),
        static_cast<unsigned>(g.snfdCall - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(g.snfd) - game.base),
        static_cast<unsigned>(g.uwsCall - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(g.uws) - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(g.spt) - game.base), g.lay.solverParticles, g.lay.solverEvolution
        );
    return true;
}

namespace {
bool StartPool(int threads)
{
    if (threads <= 0) {
        threads = static_cast<int>(UsableLogicalProcessors()) / 4;
    }
    if (threads < 1) threads = 1;
    if (threads > kMaxThreads) threads = kMaxThreads;
    g_wake = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);  // releases of runs a late helper missed may pile up: harmless
    if (!g_wake) return false;
    for (int i = 0; i < threads; ++i) {
        HANDLE t = CreateThread(nullptr, 1024 * 1024, Helper, nullptr, 0, nullptr);
        if (!t) break;
        DescribeThread(t, L"OblivionPerfFix physics push");
        CloseHandle(t);
        ++g_threads;
    }
    return g_threads > 0;
}
}  // namespace

bool InstallPhysicsPush(bool beforeGameCode, int threads, int minParallel)
{
    if (g_installed) return true;
    if (!FindPhysicsPush()) return false;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    if (minParallel > 0) g_minParallel = minParallel;
    if (!StartPool(threads)) Log("[push] no helper threads: batch creation only");
    // The inner calls first (they pass straight through while no push is in progress), the outer one last.
    if (!RedirectCall(g.createCall, reinterpret_cast<void*>(&CreateStub)) || !RedirectCall(g.viewsCall, reinterpret_cast<void*>(&ViewsStub)) ||
        !RedirectCall(g.snfdCall, reinterpret_cast<void*>(&SnfdStub)) || !RedirectCall(g.uwsCall, reinterpret_cast<void*>(&UwsStub)) ||
        !RedirectCall(g.pushCall, reinterpret_cast<void*>(&ForEachHook)))
        return Fail("could not patch the calls");
    g_installed = true;
    Log("[push] installed (%d helper threads, runs of %d+ proxies set up in parallel)", g_threads, g_minParallel);
    return true;
}

void SetPhysicsPushParallel(int mode, int minParallel)
{
    if (minParallel > 0) g_minParallel = minParallel;
    if (mode < 0 || mode > 3) mode = 0;
    if (!g_installed || g_mode.exchange(mode) == mode) return;
    Log("[push] %s", mode == 1 ? "on (batch creation, shapes set up in parallel)" : mode == 2 ? "on (batch creation only)"
                     : mode == 3 ? "on, verifying (the world-space update done again and compared)" : "off");
}

void LogPhysicsPushStats()
{
    if (!g_installed) return;
    const uint64_t pOn = s_pushOn.exchange(0), uOn = s_usOn.exchange(0), nOn = s_newOn.exchange(0), mOn = s_maxUsOn.exchange(0);
    const uint64_t pOff = s_pushOff.exchange(0), uOff = s_usOff.exchange(0), nOff = s_newOff.exchange(0), mOff = s_maxUsOff.exchange(0);
    const uint64_t runs = s_runs.exchange(0), batched = s_batched.exchange(0), par = s_parallel.exchange(0), self = s_selfDone.exchange(0),
                   wait = s_waitUs.exchange(0), bad = s_anomaly.exchange(0), ver = s_verified.exchange(0),
                   mis = s_mismatch.exchange(0);
    if (!pOn && !pOff && !bad) return;
    Log("[push] since last report: on %llu pushes, %llu new statics, %.1f ms (%.2f us/new static, max %.2f ms); off %llu pushes, "
        "%llu new statics, %.1f ms (%.2f us/new static, max %.2f ms); %llu runs, %llu batched, %llu set up by helpers (%llu by "
        "the push itself, waited %.1f ms)%s%s",
        static_cast<unsigned long long>(pOn), static_cast<unsigned long long>(nOn), uOn / 1000.0, nOn ? double(uOn) / double(nOn) : 0.0,
        mOn / 1000.0, static_cast<unsigned long long>(pOff), static_cast<unsigned long long>(nOff), uOff / 1000.0,
        nOff ? double(uOff) / double(nOff) : 0.0, mOff / 1000.0, static_cast<unsigned long long>(runs),
        static_cast<unsigned long long>(batched), static_cast<unsigned long long>(par - self), static_cast<unsigned long long>(self),
        wait / 1000.0, bad ? "; UNEXPECTED ORDER SEEN" : "", "");
    if (ver) Log("[push] verify: %llu world-space updates done again, %llu differed", static_cast<unsigned long long>(ver),
                 static_cast<unsigned long long>(mis));
}

bool PhysicsPushInfo(PhysicsPushInfoOut* out)
{
    if (!g.pushCall) return false;
    out->pushCall = g.pushCall;
    out->createCall = g.createCall;
    out->viewsCall = g.viewsCall;
    out->snfdCall = g.snfdCall;
    out->uwsCall = g.uwsCall;
    out->uws = reinterpret_cast<void*>(g.uws);
    out->forEach = reinterpret_cast<void*>(g.forEach);
    out->create = reinterpret_cast<void*>(g.create);
    out->views = reinterpret_cast<void*>(g.views);
    out->snfd = reinterpret_cast<void*>(g.snfd);
    out->spt = reinterpret_cast<void*>(g.spt);
    out->grow = reinterpret_cast<void*>(g.grow);
    out->free = reinterpret_cast<void*>(g.free);
    const uint32_t offs[] = {g.lay.buckets, g.lay.numBuckets, g.lay.solverFrame, g.lay.proxyIgnore, g.lay.proxyType,
                             g.lay.solverParticles, g.lay.solverRewind, g.lay.solverEvolution};
    memcpy(out->offsets, offs, sizeof offs);
    return true;
}

PhysicsPushTestHooks PhysicsPushTestSetup(const PhysicsPushTestFns& f, int threads, int minParallel)
{
    g.forEach = reinterpret_cast<ForEachFn>(f.forEach);
    g.create = reinterpret_cast<CreateFn>(f.create);
    g.views = reinterpret_cast<ViewsFn>(f.views);
    g.snfd = reinterpret_cast<SnfdFn>(f.snfd);
    g.spt = reinterpret_cast<SptFn>(f.spt);
    g.grow = reinterpret_cast<GrowFn>(f.grow);
    g.free = reinterpret_cast<FreeFn>(f.free);
    g.uws = reinterpret_cast<UwsFn>(f.uws);
    g.jobUws = reinterpret_cast<JobUwsFn>(f.jobUws);
    g.lay = Layout{};
    if (!g_installed) {
        if (threads > 0 && g_threads == 0) StartPool(threads);
        g_installed = true;
    }
    if (minParallel > 0) g_minParallel = minParallel;
    g_broken = false;
    return PhysicsPushTestHooks{reinterpret_cast<void*>(&ForEachHook), reinterpret_cast<void*>(&CreateStub),
                                reinterpret_cast<void*>(&ViewsStub), reinterpret_cast<void*>(&SnfdStub),
                                reinterpret_cast<void*>(&UwsStub)};
}

void PhysicsPushTestHold(int ms) { g_testHoldMs = ms; }
void PhysicsPushTestMode(int mode, bool jitter
)
{
    g_mode = mode;
    g_testJitter = jitter;
    g_broken = false;
}

uint64_t PhysicsPushTestAnomalies() { return s_anomaly.load(); }

}  // namespace opf
