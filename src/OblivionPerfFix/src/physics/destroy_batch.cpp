// PhysicsDestroyBatch (worker physwait 2026-10-22, reviewed twice; public since 1.1.9, worker destroyic 2026-10-23):
// the physics step's destruction of removed bodies made shorter, with the same physics state afterwards.
//
// Where the time goes (IC stone bridge, PhysWaitProbe, 2026-10-22): when World Partition unloads a cell, its big
// instanced-mesh components (rocks, trees, ~1,000-2,500 instance bodies each) are unregistered one per frame; each body's
// removal reaches the physics side twice: the push's UnregisterObject command (disable, ~1.8 us) and, at the end of that
// step, FPBDRigidsSolver::DestroyPendingProxies_Internal (exe+0x1A27840, called by AdvanceSolverBy at exe+0x1A209F8):
// per proxy Evolution->DestroyParticle (inlined: unique index, acceleration queues, DisconnectConstraints,
// DestroyTransientConstraints, IslandManager.RemoveParticle, Particles.DestroyParticle = SOA removal + UpdateViews) and
// `delete Proxy`. 3.4 us per body: the proxy's deleting destructor (exe+0x1A653E0: the game-thread particle, its shapes,
// the physics object, IPhysicsProxyBase's RemoveDirtyProxy) 1.44 us, the SOA removal 0.97 us of which UpdateViews ~0.35 us.
// The game thread waits for it in TG_EndPhysics: 4-9 ms in each of ~8 frames at the bridge (31 % of the flight's
// EndPhysics waits over calm, 47-58 % at the bridge).
//
// What it changes (both parts only inside DestroyPendingProxies, only when >= PhysicsDestroyBatchMin bodies are pending):
//  1 (parallel deletes): the loop's `delete Info.Proxy` (exe+0x1A27C0B: mov rax,[rcx]; mov edx,r12d; call [rax+8])
//    hands the proxy to a helper thread instead; the step's thread continues with the next body; before
//    DestroyPendingProxies returns (and so before the step completes) every handed-over proxy is deleted. Why the
//    result is the same: the proxy is unreachable at that point (UnregisterObject cleared the game thread's pointers and
//    removed it from the dirty set; the command cleared its handle; stock deletes it right here), and nothing the loop
//    does afterwards can touch it (stock would read freed memory otherwise). The destructor touches only the proxy's own
//    members (game-thread particle and shapes, the physics object, the timestamp's thread-safe shared pointer, the
//    geometry's thread-safe reference counts) and, in IPhysicsProxyBase's destructor, the dirty set's Remove under the
//    game's dirty-proxy SRW lock (solver+0x128, taken by stock on the step thread the same way), which returns at once
//    when the proxy's DirtyIdx (+0x18) is INDEX_NONE (checked before handing over; else stock). Only the order and the
//    thread of the memory frees differ (heap addresses, as in stock from run to run). The game thread may be running
//    TG_DuringPhysics meanwhile (it can add dirty proxies under that same lock), so the lock is what makes the
//    dirty-set access safe, for the helper exactly as for the stock deletes on the step thread; everything else a
//    delete touches is the proxy's own. One batch at a time (a busy flag): a second solver stepping at the same moment
//    deletes inline as stock.
//  2 (one UpdateViews): Particles.DestroyParticle ends with UpdateViews (exe+0x18FA1AF, call exe+0x178D500), which
//    rebuilds the SOA's ~10 particle views from its arrays. Inside DestroyPendingProxies' loop nothing reads a view (the
//    loop's other calls work on handles, maps and constraint lists; checked in the UE 5.3 source and this exe), so the
//    calls are skipped there and the game's UpdateViews runs once after the loop, on the final arrays: the same views.
//    Only when no geometry-collection or cluster-union proxies are pending (their clean-up after the loop may read
//    views) and only on the calling thread inside the call (thread-local, so concurrent solvers are independent).
// Live: 0 = off (not installed), 1 = parallel deletes, 2 = one UpdateViews, 3 = both, 4 = installed but off (for A/B in
// one game); installed at start-up when != 0; other values are treated as 4. Sites and the solver / proxy offsets by
// byte pattern (build 19115871 addresses in the comments); a mismatch leaves it off with a log line. The helper thread
// only with at least ParallelMinProcessors (8) usable logical processors and WaitOnAddress (looked up at run time, so the
// DLL still imports only KERNEL32); otherwise only the one-UpdateViews part acts.
#include "common.h"

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace opf {
namespace {

// AdvanceSolverBy (exe+0x1A20870): FinalizePullData; if (Step == NumSteps - 1) DestroyPendingProxies_Internal(this)
const char* const kSiteA = "E8 ?? ?? ?? ?? 8B 45 0C FF C8 39 45 08 75 08 48 8B CE E8 ?? ?? ?? ?? 4C 8D 9C 24";
constexpr size_t kSiteACall = 18;
// DestroyPendingProxies_Internal's entry
const char* const kDestroyEntry = "48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00";
// ... in its loop: Particles.DestroyParticle(Handle) (call SOAs::DestroyParticle); if (Proxy) delete Proxy; then the
// loop's RemoveAtSwap reads PendingDestroyPhysicsProxy.Num (+0xB38)
const char* const kSiteB =
    "49 8B D6 E8 ?? ?? ?? ?? 4C 8B 7D ?? 49 8B 0C 24 41 BC 01 00 00 00 48 85 C9 74 09 48 8B 01 41 8B D4 FF 50 08 8B 97 38 0B 00 00";
constexpr size_t kSiteBSoa = 3, kSiteBDelete = 27, kSiteBLen = 9;
// ... the offsets the guard reads, as the function itself reads them: PendingDestroyClusterUnionProxy (data +0xB50, num
// +0xB58: mov rsi,[rdi+0B50h] ... movsxd rax,[rdi+0B58h]) and PendingDestroyGeometryCollectionPhysicsProxy (num +0xB48,
// data +0xB40: mov ecx,[rdi+0B48h] ... lea r14,[rdi+0B40h])
const char* const kCuData = "48 8B B7 50 0B 00 00";
const char* const kCuNum = "48 63 87 58 0B 00 00";
const char* const kGcNum = "8B 8F 48 0B 00 00";
const char* const kGcData = "4C 8D B7 40 0B 00 00";
// SOAs::DestroyParticle's tail (exe+0x18FA1AF): ...; handle index fix-up; UpdateViews(this); return
const char* const kSiteC = "89 59 10 48 8B CD E8 ?? ?? ?? ?? 48 83 C4 40 5F 5D 5B C3";
constexpr size_t kSiteCCall = 6;
// FSingleParticlePhysicsProxy's deleting destructor (exe+0x1A653E0, unique in .text): vtable store, PhysicsObject (+0x58),
// Particle (+0x48), then the base destructor (mov rcx,rbx; call), which calls RemoveDirtyProxy (add rcx,150h; call)
const char* const kProxyDtor = "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 8B FA 48 8B 51 58 48 83 C1 58 E8";
const char* const kDtorBaseCall = "FF 10 48 8B CB E8";  // after the particle's virtual delete
const char* const kBaseRemoveCall = "48 81 C1 50 01 00 00 E8";
// FDirtySet::Remove (exe+0x18DF390): idx = proxy->DirtyIdx (+0x18); if (idx == INDEX_NONE) return
const char* const kDirtyRemove = "40 55 56 57 48 83 EC 20 48 63 7A 18 48 8B EA 48 8B F1 83 FF FF";
// FPBDRigidsSolver (this build), each anchored above: pending +0xB38, GC num +0xB48, CU num +0xB58; DirtyIdx +0x18
constexpr size_t kPendingNumOff = 0xB38, kGcNumOff = 0xB48, kCuNumOff = 0xB58, kDirtyIdxOff = 0x18;

using DestroyFn = void (*)(uint8_t* solver);
using ViewsFn = void (*)(void* soas);
using DelFn = void* (*)(void* proxy, uint32_t flags);
using WaitOnAddressFn = BOOL(WINAPI*)(volatile void*, void*, SIZE_T, DWORD);
using WakeByAddressFn = void(WINAPI*)(void*);

// The batch's claim word: bits 0-31 = next index to claim, bit 32 = closed, bits 33-63 = batch generation. A helper claims
// item i only by a compare-exchange of the word from (gen, open, i) to (gen, open, i + 1), so after the owner closes the
// batch (one exchange) no helper can claim anything, and a helper that read an older batch's word cannot claim in a newer
// one. The owner waits only for deletes that were claimed before the close (done == index at the close).
constexpr uint64_t kIdxMask = 0xFFFFFFFFull;
constexpr uint64_t kClosed = 1ull << 32;
constexpr int kGenShift = 33;
constexpr int kMaxStalls = 3;

struct Stat {
    std::atomic<uint64_t> v{0};
    void add(uint64_t x) { v.fetch_add(x, std::memory_order_relaxed); }
    uint64_t take() { return v.exchange(0, std::memory_order_relaxed); }
};

struct State {
    bool installed = false;
    bool sitesFound = false;
    uint8_t* siteA = nullptr;
    uint8_t* siteB = nullptr;
    uint8_t* siteC = nullptr;
    DestroyFn destroy = nullptr;    // the game's DestroyPendingProxies_Internal (or a self-test fake)
    ViewsFn views = nullptr;        // the game's UpdateViews (or a fake)
    const void* dtorFn = nullptr;   // the proxy's deleting destructor: a proxy is handed over only if its vtable slot 1 is it
    std::atomic<bool> toldVtable{false};
    std::atomic<int> mode{0};
    std::atomic<int> minPending{256};
    // one batch at a time (owner: the thread that set busy; consumers: the helper and, at the end, the owner)
    std::atomic<bool> busy{false};
    std::vector<void*> items;
    std::atomic<int> count{0}, done{0};
    std::atomic<bool> helperSleeping{false};
    std::atomic<uint64_t> word{kClosed};
    uint64_t gen = 0;
    HANDLE go[2] = {};
    int helpers = 0;
    WaitOnAddressFn waitOnAddress = nullptr;  // Windows 8+, from kernelbase (looked up at run time: the DLL imports only KERNEL32)
    WakeByAddressFn wakeOne = nullptr, wakeAll = nullptr;
    // diagnostics: solvers seen, wrappers overlapping
    std::atomic<int> inflight{0};
    std::atomic<uint64_t> solvers[8] = {};
    Stat overlaps, busyInline, longLogs;
    // stats
    Stat particles, offBatches, offParticles, offTicks, batches, handed, inlineDel, viewsSkipped, viewsRun, wrapTicks, waitTicks;
    std::atomic<uint64_t> maxWrapTicks{0}, maxOffTicks{0}, maxHelperWaitTicks{0};
    // a helper that is not running when the step needs its last delete (a busy or small PC): after kMaxStalls waits of
    // more than 1 ms for an in-flight helper delete, the parallel deletes stay off for the session
    std::atomic<int> stalls{0};
    std::atomic<bool> deletesDisabled{false};
    int64_t freq = 1;
} g;

thread_local bool tl_inBatch = false;   // this thread owns the open batch (inside the wrapper)
thread_local bool tl_deferViews = false;
thread_local void* tl_soas = nullptr;
thread_local uint32_t tl_skippedViews = 0;
thread_local uint32_t tl_handed = 0;

int64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void Max(std::atomic<uint64_t>& m, uint64_t v)
{
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

void DeleteNow(void* proxy)
{
    void** vt = *reinterpret_cast<void***>(proxy);
    reinterpret_cast<DelFn>(vt[1])(proxy, 1);
}

// Spin briefly, then give the processor away.
void Backoff(int& spins)
{
    if (++spins < 64) {
        _mm_pause();
    } else {
        SwitchToThread();
        spins = 32;
    }
}

// The helper: claims and deletes handed-over proxies while the batch is open.
void HelperDrain()
{
    int spins = 0;
    int64_t idleSince = 0;
    const int64_t spinTicks = g.freq / 30000;  // ~33 us of spinning (about 15 loop bodies) before sleeping
    for (;;) {
        uint64_t w = g.word.load(std::memory_order_acquire);
        if (w & kClosed) return;
        const int i = static_cast<int>(w & kIdxMask);
        const int c = g.count.load(std::memory_order_acquire);
        if (i < c) {
            if (g.word.compare_exchange_weak(w, w + 1, std::memory_order_acq_rel)) {
                DeleteNow(g.items[static_cast<size_t>(i)]);
                g.done.fetch_add(1, std::memory_order_release);
                spins = 0;
                idleSince = 0;
            }
            continue;
        }
        if (!idleSince) idleSince = Qpc();
        if (++spins < 64 || Qpc() - idleSince < spinTicks) {
            _mm_pause();
            continue;
        }
        // nothing to claim for a while: sleep until the step thread hands over the next proxy or closes the batch
        // (it wakes the helper only while this flag is set), so a waiting helper takes no processor time from it
        g.helperSleeping.store(true, std::memory_order_seq_cst);
        if (g.count.load(std::memory_order_seq_cst) == c && !(g.word.load(std::memory_order_seq_cst) & kClosed)) {
            int seen = c;
            g.waitOnAddress(&g.count, &seen, sizeof seen, 2);
        }
        g.helperSleeping.store(false, std::memory_order_relaxed);
        spins = 0;
        idleSince = 0;
    }
}

DWORD WINAPI Helper(LPVOID p)
{
    const int k = static_cast<int>(reinterpret_cast<intptr_t>(p));
    for (;;) {
        if (WaitForSingleObject(g.go[k], INFINITE) != WAIT_OBJECT_0) return 0;
        HelperThreadCache();
        HelperDrain();
    }
}

// In place of `delete Info.Proxy` inside the loop (rcx = proxy, never null here).
void DeleteStub(void* proxy)
{
    if (tl_inBatch) {
        void** vt = *reinterpret_cast<void***>(proxy);
        const bool known = vt && vt[1] == g.dtorFn;
        const int c = g.count.load(std::memory_order_relaxed);
        if (known && *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(proxy) + kDirtyIdxOff) == -1 &&
            static_cast<size_t>(c) < g.items.size()) {
            g.items[static_cast<size_t>(c)] = proxy;
            g.count.store(c + 1, std::memory_order_seq_cst);
            if (g.helperSleeping.load(std::memory_order_seq_cst)) g.wakeOne(&g.count);
            ++tl_handed;
            return;
        }
        if (!known && !g.toldVtable.exchange(true))
            Log("[destroybatch] a pending proxy with another destructor (vtable %p): deleted inline (logged once)", static_cast<void*>(vt));
        g.inlineDel.add(1);
    }
    DeleteNow(proxy);
}

// In place of SOAs::DestroyParticle's UpdateViews call.
void ViewsStub(void* soas)
{
    if (tl_deferViews) {
        if (tl_soas && tl_soas != soas) g.views(tl_soas);  // another SOA (not in this build): the earlier one's views now
        tl_soas = soas;
        ++tl_skippedViews;
        return;
    }
    g.views(soas);
}


// In place of AdvanceSolverBy's call of DestroyPendingProxies_Internal.
void DestroyWrapper(uint8_t* solver)
{
    const int mode = g.mode.load(std::memory_order_relaxed);
    const int pending = *reinterpret_cast<const int32_t*>(solver + kPendingNumOff);
    const bool big = pending >= g.minPending.load(std::memory_order_relaxed);
    {
        if (g.inflight.fetch_add(1, std::memory_order_acq_rel) > 0) {
            if (g.overlaps.v.fetch_add(1, std::memory_order_relaxed) < 20)
                Log("[destroybatch] two DestroyPendingProxies calls at once (solver %p, thread %lu)", static_cast<void*>(solver),
                    static_cast<unsigned long>(GetCurrentThreadId()));
        }
    }
    struct Leave {
        bool on;
        ~Leave() { if (on) g.inflight.fetch_sub(1, std::memory_order_acq_rel); }
    } leave{true};
    if (mode < 1 || mode > 3 || !big) {
        if (mode == 4 && big) {  // installed-off: timed for A/B
            const int64_t a = Qpc();
            g.destroy(solver);
            const uint64_t t = static_cast<uint64_t>(Qpc() - a);
            g.offBatches.add(1);
            g.offParticles.add(static_cast<uint64_t>(pending));
            g.offTicks.add(t);
            Max(g.maxOffTicks, t);
            return;
        }
        g.destroy(solver);
        return;
    }
    const int64_t t0 = Qpc();
    // a nested call on this thread (none in this build) keeps the outer call's thread-local state
    const bool outerInBatch = tl_inBatch, outerDefer = tl_deferViews;
    void* const outerSoas = tl_soas;
    const uint32_t outerSkipped = tl_skippedViews, outerHanded = tl_handed;
    tl_inBatch = false;
    tl_handed = 0;
    bool deletes = (mode & 1) && g.helpers > 0 && g.dtorFn && !g.deletesDisabled.load(std::memory_order_relaxed);
    if (deletes && g.busy.exchange(true, std::memory_order_acquire)) {  // another solver's batch is open: stock deletes
        deletes = false;
        g.busyInline.add(1);
    }
    const bool views = (mode & 2) && *reinterpret_cast<const int32_t*>(solver + kGcNumOff) == 0 &&
                       *reinterpret_cast<const int32_t*>(solver + kCuNumOff) == 0;
    if (deletes) {
        if (g.items.size() < static_cast<size_t>(pending)) g.items.resize(static_cast<size_t>(pending) + 1024);
        g.count.store(0, std::memory_order_relaxed);
        g.done.store(0, std::memory_order_relaxed);
        g.word.store(++g.gen << kGenShift, std::memory_order_release);  // open, index 0, a new generation
        for (int k = 0; k < g.helpers; ++k) SetEvent(g.go[k]);
        tl_inBatch = true;
    }
    tl_deferViews = views;
    tl_skippedViews = 0;
    tl_soas = nullptr;
    g.destroy(solver);
    tl_deferViews = false;
    tl_inBatch = false;
    if (tl_skippedViews) {  // the views of the final arrays, once
        g.views(tl_soas);
        g.viewsSkipped.add(tl_skippedViews - 1);
        g.viewsRun.add(1);
    }
    g.handed.add(tl_handed);
    tl_inBatch = outerInBatch;
    tl_deferViews = outerDefer;
    tl_soas = outerSoas;
    tl_skippedViews = outerSkipped;
    tl_handed = outerHanded;
    const int64_t tw = Qpc();
    uint64_t helperWait = 0;
    int handedHere = 0, claimedByHelper = 0;
    if (deletes) {
        const uint64_t w = g.word.fetch_or(kClosed, std::memory_order_seq_cst);  // close: no claim succeeds from now on
        // wake a sleeping helper so it sees the close at once (a wake that lands before it blocks is lost: it then sleeps
        // out its 2 ms timeout and finds the batch closed; nothing waits on that)
        if (g.helperSleeping.load(std::memory_order_seq_cst)) g.wakeAll(&g.count);
        const int claimed = static_cast<int>(w & kIdxMask);
        handedHere = g.count.load(std::memory_order_relaxed);
        claimedByHelper = claimed;
        for (int i = claimed; i < handedHere; ++i) DeleteNow(g.items[static_cast<size_t>(i)]);  // the rest, on this thread
        int spins = 0;
        const int64_t th = Qpc();
        while (g.done.load(std::memory_order_acquire) < claimed) Backoff(spins);  // the helper's claimed deletes finish
        const uint64_t hw = static_cast<uint64_t>(Qpc() - th);
        helperWait = hw;
        Max(g.maxHelperWaitTicks, hw);
        if (hw * 1000 > static_cast<uint64_t>(g.freq) && g.stalls.fetch_add(1, std::memory_order_relaxed) + 1 >= kMaxStalls &&
            !g.deletesDisabled.exchange(true))
            Log("[destroybatch] the helper was not running when its last delete was needed (%d waits > 1 ms, last %.2f ms): "
                "parallel deletes off for this session (one UpdateViews stays)", kMaxStalls, static_cast<double>(hw) * 1000.0 / static_cast<double>(g.freq));
        g.busy.store(false, std::memory_order_release);
    }
    const int64_t t1 = Qpc();
    g.batches.add(1);
    g.particles.add(static_cast<uint64_t>(pending));
    g.wrapTicks.add(static_cast<uint64_t>(t1 - t0));
    g.waitTicks.add(static_cast<uint64_t>(t1 - tw));
    Max(g.maxWrapTicks, static_cast<uint64_t>(t1 - t0));
    (void)helperWait;
    (void)claimedByHelper;
}

bool Fail(const char* why)
{
    Log("[destroybatch] PhysicsDestroyBatch off: %s", why);
    return false;
}

bool FindSites(const GameModule& game)
{
    g.sitesFound = false;
    const auto as = FindAll(game.text, ParsePattern(kSiteA));
    if (as.size() != 1) return Fail("AdvanceSolverBy's call of DestroyPendingProxies not found once");
    uint8_t* const destroy = RipTarget(as[0] + kSiteACall, 1, 5);
    if (!game.text.Contains(destroy) || !MatchesAt(destroy, ParsePattern(kDestroyEntry)))
        return Fail("DestroyPendingProxies_Internal's entry not as expected");
    const Section fn{destroy, destroy + 0x700};
    const auto bs = FindAll(fn, ParsePattern(kSiteB));
    if (bs.size() != 1) return Fail("the loop's proxy delete not found once in DestroyPendingProxies");
    for (const char* p : {kCuData, kCuNum, kGcNum, kGcData})
        if (FindAll(fn, ParsePattern(p)).size() != 1) return Fail("DestroyPendingProxies' cluster-union / geometry-collection offsets not as expected");
    uint8_t* const soaDestroy = RipTarget(bs[0] + kSiteBSoa, 1, 5);
    if (!game.text.Contains(soaDestroy)) return Fail("SOAs::DestroyParticle not found");
    const auto cs = FindAll(Section{soaDestroy, soaDestroy + 0x500}, ParsePattern(kSiteC));
    if (cs.size() != 1) return Fail("SOAs::DestroyParticle's UpdateViews call not found once");
    // the proxy's deleting destructor and, through its base destructor, the dirty set's DirtyIdx check
    const auto ds = FindAll(game.text, ParsePattern(kProxyDtor));
    if (ds.size() != 1) return Fail("the proxy's deleting destructor not found once");
    const auto bc = FindAll(Section{ds[0], ds[0] + 0x60}, ParsePattern(kDtorBaseCall));
    if (bc.size() != 1) return Fail("the proxy destructor's base call not found");
    uint8_t* const base = RipTarget(bc[0] + 5, 1, 5);
    const auto rc = game.text.Contains(base) ? FindAll(Section{base, base + 0xA0}, ParsePattern(kBaseRemoveCall)) : std::vector<uint8_t*>{};
    if (rc.empty()) return Fail("the base destructor's RemoveDirtyProxy call not found");
    uint8_t* const remove = RipTarget(rc[0] + 7, 1, 5);
    if (!game.text.Contains(remove) || !MatchesAt(remove, ParsePattern(kDirtyRemove))) return Fail("RemoveDirtyProxy's DirtyIdx check not as expected");
    g.siteA = as[0] + kSiteACall;
    g.siteB = bs[0] + kSiteBDelete;
    g.siteC = cs[0] + kSiteCCall;
    g.destroy = reinterpret_cast<DestroyFn>(destroy);
    g.views = reinterpret_cast<ViewsFn>(RipTarget(g.siteC, 1, 5));
    g.dtorFn = ds[0];
    g.sitesFound = true;
    return true;
}

// call rel32 to a stub (through a jmp in the slot near the game: absolute jump to the C++ stub)
bool PatchCall(uint8_t* site, size_t len, uint8_t* slot, void* stub)
{
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(slot, jmpAbs, 6);
    memcpy(slot + 6, &stub, 8);
    const int64_t rel = slot - (site + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t code[16];
    memset(code, 0x90, sizeof code);
    code[0] = 0xE8;
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(code + 1, &rel32, 4);
    return WriteCode(site, std::span<const uint8_t>(code, len));
}

// One helper (a second only spins: a delete takes 1.44 us, the loop body ~1.9 us); none below ParallelMinProcessors (8)
// usable logical processors (with fewer, a preempted helper can hold up the step's end: in-game test T4, 2026-10-22) or
// without WaitOnAddress.
void StartHelpers(int want)
{
    if (!g.waitOnAddress) {
        if (HMODULE m = GetModuleHandleW(L"kernelbase.dll")) {
            g.waitOnAddress = reinterpret_cast<WaitOnAddressFn>(GetProcAddress(m, "WaitOnAddress"));
            g.wakeOne = reinterpret_cast<WakeByAddressFn>(GetProcAddress(m, "WakeByAddressSingle"));
            g.wakeAll = reinterpret_cast<WakeByAddressFn>(GetProcAddress(m, "WakeByAddressAll"));
        }
        if (!g.waitOnAddress || !g.wakeOne || !g.wakeAll) {
            g.waitOnAddress = nullptr;
            return;
        }
    }
    for (int k = 0; k < want && k < 1; ++k) {  // one helper: the sleep flag and the wake assume one
        g.go[k] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g.go[k] ? CreateThread(nullptr, 0, Helper, reinterpret_cast<LPVOID>(static_cast<intptr_t>(k)), 0, nullptr) : nullptr;
        if (!t) {
            if (g.go[k]) CloseHandle(g.go[k]);
            g.go[k] = nullptr;
            break;
        }
        DescribeThread(t, L"OblivionPerfFix destroy helper");
        // above normal (it sleeps when idle): a preemption in the middle of a delete is the only thing the step can wait for
        SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(t);
        g.helpers = k + 1;
    }
}

}  // namespace

bool InstallPhysicsDestroyBatch(bool beforeGameCode, int minProcessors)
{
    if (g.installed) return true;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    const GameModule& game = Game();
    if (!FindSites(game)) return false;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g.freq = f.QuadPart ? f.QuadPart : 1;
    uint8_t* const region = AllocNearGame(64);
    if (!region) return Fail("no memory near the game's code");
    const int cpus = static_cast<int>(UsableLogicalProcessors());
    StartHelpers(cpus >= minProcessors ? 1 : 0);  // smaller PCs: the one-UpdateViews part only
    g.items.resize(8192);
    DWORD old;
    VirtualProtect(region, 64, PAGE_READWRITE, &old);
    bool ok = PatchCall(g.siteB, kSiteBLen, region, reinterpret_cast<void*>(&DeleteStub)) &&
              PatchCall(g.siteC, 5, region + 16, reinterpret_cast<void*>(&ViewsStub)) &&
              PatchCall(g.siteA, 5, region + 32, reinterpret_cast<void*>(&DestroyWrapper));
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    if (!ok) return Fail("could not patch the calls");
    g.installed = true;
    // why there is no helper (then only the single-thread UpdateViews part acts)
    char none[96] = "";
    if (!g.helpers)
        snprintf(none, sizeof none, "; none: %s", cpus < minProcessors ? "fewer than ParallelMinProcessors" : !g.waitOnAddress ? "WaitOnAddress not available"
                                                                                                                      : "the thread could not be started");
    if (!g.helpers && cpus < minProcessors) snprintf(none + strlen(none), sizeof none - strlen(none), " (%d)", minProcessors);
    Log("[destroybatch] PhysicsDestroyBatch installed: DestroyPendingProxies exe+0x%llX (call exe+0x%llX), proxy delete exe+0x%llX, "
        "UpdateViews call exe+0x%llX, proxy destructor exe+0x%llX; %d helper thread(s) (%d usable logical processors%s)",
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g.destroy) - game.base),
        static_cast<unsigned long long>(g.siteA - game.base), static_cast<unsigned long long>(g.siteB - game.base),
        static_cast<unsigned long long>(g.siteC - game.base),
        static_cast<unsigned long long>(static_cast<const uint8_t*>(g.dtorFn) - game.base), g.helpers, cpus, none);
    return true;
}

void SetPhysicsDestroyBatch(int mode, int minPending)
{
    if (!g.installed) return;
    if (mode < 0 || mode > 4) mode = 4;  // unknown values: installed but off
    const int was = g.mode.exchange(mode);
    g.minPending.store(minPending > 0 ? minPending : 1);
    if (was != mode) Log("[destroybatch] PhysicsDestroyBatch=%d (parallel deletes %s, one UpdateViews %s)", mode,
                         (mode == 1 || mode == 3) ? "on" : "off", (mode == 2 || mode == 3) ? "on" : "off");
}

void LogPhysicsDestroyBatchStats()
{
    if (!g.installed) return;
    const double msPerTick = 1000.0 / static_cast<double>(g.freq);
    if (const uint64_t ob = g.offBatches.take()) {
        const uint64_t op = g.offParticles.take(), ot = g.offTicks.take();
        Log("[destroybatch] off (4) since last report: %llu batches, %llu pending bodies, %.1f ms (%.2f us per body, max %.2f ms)",
            static_cast<unsigned long long>(ob), static_cast<unsigned long long>(op), static_cast<double>(ot) * msPerTick,
            static_cast<double>(ot) * msPerTick * 1000.0 / static_cast<double>(op ? op : 1),
            static_cast<double>(g.maxOffTicks.exchange(0)) * msPerTick);
    }
    const uint64_t b = g.batches.take();
    if (!b) return;
    const uint64_t p = g.particles.take(), wt = g.wrapTicks.take();
    Log("[destroybatch] on since last report: %llu pending bodies, %.2f us per body", static_cast<unsigned long long>(p),
        static_cast<double>(wt) * msPerTick * 1000.0 / static_cast<double>(p ? p : 1));
    Log("[destroybatch] since last report: %llu batches, %llu proxies deleted by the helper, %llu inline, %llu batches while another "
        "was open; UpdateViews %llu skipped, %llu run after the loop; wrapper %.1f ms (max %.2f), of it %.1f ms after the loop "
        "(last deletes), longest wait for an in-flight helper delete %.2f ms; overlapping calls so far %llu%s",
        static_cast<unsigned long long>(b), static_cast<unsigned long long>(g.handed.take()), static_cast<unsigned long long>(g.inlineDel.take()),
        static_cast<unsigned long long>(g.busyInline.take()), static_cast<unsigned long long>(g.viewsSkipped.take()),
        static_cast<unsigned long long>(g.viewsRun.take()), static_cast<double>(wt) * msPerTick,
        static_cast<double>(g.maxWrapTicks.exchange(0)) * msPerTick, static_cast<double>(g.waitTicks.take()) * msPerTick,
        static_cast<double>(g.maxHelperWaitTicks.exchange(0)) * msPerTick, static_cast<unsigned long long>(g.overlaps.v.load()),
        g.deletesDisabled.load() ? "; parallel deletes off (helper stalls)" : "");
}

// ---- self-test (tests/selftest_destroybatch.h) ----
bool PhysicsDestroyBatchTestSites(const GameModule& game)
{
    return FindSites(game) && g.siteA && g.siteB && g.siteC && g.dtorFn;
}

namespace {
struct FakeProxy {
    void** vt;
    uint8_t pad[0x10];
    int32_t dirtyIdx;
    std::atomic<int> deleted;
    std::atomic<bool>* retired;  // set by the test after its wrapper returned: a delete after that is "late"
    int slowMs;                  // the fake destructor sleeps this long
};
std::atomic<int> s_fakeViews{0};
std::atomic<long> s_doubles{0}, s_late{0};
thread_local std::vector<FakeProxy>* tl_fakes = nullptr;
thread_local int tl_gapMs = 0;  // the fake loop sleeps between bodies (the helper then takes its sleep path)
void* FakeDelete(void* p, uint32_t)
{
    auto* f = static_cast<FakeProxy*>(p);
    if (f->slowMs) Sleep(static_cast<DWORD>(f->slowMs));
    if (f->retired && f->retired->load()) s_late.fetch_add(1);
    if (f->deleted.fetch_add(1) != 0) s_doubles.fetch_add(1);
    return p;
}
void FakeViews(void*) { s_fakeViews.fetch_add(1); }
void FakeDestroy(uint8_t*)
{
    for (auto& f : *tl_fakes) {
        for (volatile int k = 0; k < 20; ++k) {}
        if (tl_gapMs) Sleep(static_cast<DWORD>(tl_gapMs));
        ViewsStub(reinterpret_cast<void*>(0x1234));
        DeleteStub(&f);
    }
}
void* s_fakeVt[2] = {nullptr, reinterpret_cast<void*>(&FakeDelete)};
void* s_otherVt[2] = {nullptr, reinterpret_cast<void*>(&FakeDelete)};  // a different vtable object
std::atomic<int> s_otherDeletes{0};
void* OtherDelete(void* p, uint32_t f)  // its own body (identical functions may be folded into one address by the linker)
{
    s_otherDeletes.fetch_add(1);
    return FakeDelete(p, f);
}

struct Fakes {
    DestroyFn d;
    ViewsFn v;
    const void* dtor;
    bool inst;
    Fakes() : d(g.destroy), v(g.views), dtor(g.dtorFn), inst(g.installed)
    {
        g.destroy = &FakeDestroy;
        g.views = &FakeViews;
        g.dtorFn = s_fakeVt[1];  // the fake vtable's slot 1 stands for the game's proxy destructor
        s_otherVt[1] = reinterpret_cast<void*>(&OtherDelete);  // another destructor: never handed over
        if (!g.helpers) StartHelpers(1);  // the self-test has no start-up install: a helper of its own
        g.deletesDisabled.store(false);   // the stall guard is tested on its own
        g.stalls.store(0);
        g.installed = true;
        g.minPending.store(1);
        s_late = 0;
        s_doubles = 0;
        s_fakeViews = 0;
    }
    ~Fakes()
    {
        g.destroy = d;
        g.views = v;
        g.dtorFn = dtor;
        g.installed = inst;
        g.mode.store(0);
        g.deletesDisabled.store(false);
        g.stalls.store(0);
    }
};

void RunOne(std::vector<FakeProxy>* list, int gcNum, int cuNum, std::atomic<bool>* retired = nullptr)
{
    tl_fakes = list;
    alignas(16) uint8_t solver[0xC00] = {};
    *reinterpret_cast<int32_t*>(solver + kPendingNumOff) = static_cast<int32_t>(list->size());
    *reinterpret_cast<int32_t*>(solver + kGcNumOff) = gcNum;
    *reinterpret_cast<int32_t*>(solver + kCuNumOff) = cuNum;
    DestroyWrapper(solver);
    if (retired) retired->store(true);
}

void Prepare(std::vector<FakeProxy>& v, std::atomic<bool>* retired = nullptr)
{
    for (auto& f : v) {
        f.vt = s_fakeVt;
        f.dirtyIdx = -1;
        f.deleted = 0;
        f.retired = retired;
        f.slowMs = 0;
    }
}

bool AllOnce(const std::vector<FakeProxy>& v)
{
    for (auto& f : v)
        if (f.deleted.load() != 1) return false;
    return true;
}

struct Arg {
    std::vector<FakeProxy>* l;
    std::atomic<bool>* r;
};
DWORD WINAPI RunArg(LPVOID p)
{
    auto* a = static_cast<Arg*>(p);
    RunOne(a->l, 0, 0, a->r);
    return 0;
}
}  // namespace

// Runs the wrapper with fakes in the given mode; returns nullptr if every proxy was deleted exactly once before the wrapper
// returned and UpdateViews ran as expected (n times when off or a GC/CU proxy is pending, once with the views part on).
const char* PhysicsDestroyBatchTestRun(int mode, int n, int gcNum, int cuNum)
{
    Fakes fakes;
    std::vector<FakeProxy> list(static_cast<size_t>(n));
    std::atomic<bool> retired{false};
    Prepare(list, &retired);
    g.mode.store(mode);
    RunOne(&list, gcNum, cuNum, &retired);
    if (!AllOnce(list) || s_doubles.load() || s_late.load()) return "destroy batch: not every proxy deleted exactly once before the wrapper returned";
    const bool viewsOn = (mode == 2 || mode == 3) && gcNum == 0 && cuNum == 0;
    if (s_fakeViews.load() != (viewsOn ? 1 : n)) return "destroy batch: UpdateViews count wrong";
    return nullptr;
}

// Two or more solvers' steps in the wrapper at the same time (mode 3): every proxy deleted exactly once, none missed, none late.
const char* PhysicsDestroyBatchTestConcurrent(int callers, int rounds, int n)
{
    Fakes fakes;
    g.mode.store(3);
    long missed = 0;
    for (int r = 0; r < rounds; ++r) {
        std::vector<std::vector<FakeProxy>> lists;
        lists.reserve(static_cast<size_t>(callers));
        std::vector<std::atomic<bool>> retired(static_cast<size_t>(callers));
        std::vector<Arg> args(static_cast<size_t>(callers));
        for (int c = 0; c < callers; ++c) {
            lists.emplace_back(static_cast<size_t>(n));
            retired[static_cast<size_t>(c)] = false;
            Prepare(lists.back(), &retired[static_cast<size_t>(c)]);
        }
        for (int c = 0; c < callers; ++c) args[static_cast<size_t>(c)] = Arg{&lists[static_cast<size_t>(c)], &retired[static_cast<size_t>(c)]};
        std::vector<HANDLE> ts;
        for (auto& a : args) ts.push_back(CreateThread(nullptr, 0, RunArg, &a, 0, nullptr));
        for (HANDLE t : ts) {
            WaitForSingleObject(t, INFINITE);
            CloseHandle(t);
        }
        for (auto& l : lists)
            for (auto& f : l) missed += f.deleted.load() == 0;
    }
    if (missed || s_doubles.load() || s_late.load()) return "destroy batch: concurrent callers missed, doubled or late deletes";
    return nullptr;
}

// The other paths: proxies with DirtyIdx set or another vtable are deleted inline (once); a run without a helper; a loop
// with gaps (the helper sleeps between hand-overs); the stall guard (helper deletes that take 3 ms at the close)
// switches the parallel deletes off, after which nothing is handed over.
const char* PhysicsDestroyBatchTestPaths()
{
    Fakes fakes;
    g.mode.store(3);
    {  // inline paths
        std::vector<FakeProxy> list(600);
        std::atomic<bool> retired{false};
        Prepare(list, &retired);
        for (size_t i = 0; i < list.size(); i += 3) list[i].dirtyIdx = 5;
        for (size_t i = 1; i < list.size(); i += 3) list[i].vt = s_otherVt;
        g.inlineDel.take();
        g.handed.take();
        RunOne(&list, 0, 0, &retired);
        if (!AllOnce(list) || s_doubles.load() || s_late.load()) return "destroy batch: inline paths (DirtyIdx set / another vtable): not deleted exactly once";
        if (g.inlineDel.take() != 400 || g.handed.take() != 200) return "destroy batch: inline paths: wrong inline / handed counts";
    }
    {  // no helper
        const int saved = g.helpers;
        g.helpers = 0;
        std::vector<FakeProxy> list(500);
        std::atomic<bool> retired{false};
        Prepare(list, &retired);
        g.handed.take();
        RunOne(&list, 0, 0, &retired);
        g.helpers = saved;
        if (!AllOnce(list) || s_late.load() || g.handed.take() != 0) return "destroy batch: without a helper: not all deleted inline exactly once";
    }
    {  // gaps: the helper sleeps between hand-overs
        std::vector<FakeProxy> list(60);
        std::atomic<bool> retired{false};
        Prepare(list, &retired);
        g.handed.take();
        tl_gapMs = 1;
        RunOne(&list, 0, 0, &retired);
        tl_gapMs = 0;
        if (!AllOnce(list) || s_doubles.load() || s_late.load() || g.handed.take() != 60) return "destroy batch: loop with gaps (helper sleeping): not deleted exactly once";
    }
    {  // stall guard: the last proxies' deletes take 3 ms, so the close waits for them (when the helper took them)
        int batches = 0;
        while (!g.deletesDisabled.load() && batches < 40) {
            std::vector<FakeProxy> list(40);
            std::atomic<bool> retired{false};
            Prepare(list, &retired);
            list.back().slowMs = 3;
            list[list.size() - 2].slowMs = 3;
            RunOne(&list, 0, 0, &retired);
            ++batches;
            if (!AllOnce(list) || s_late.load()) return "destroy batch: stall-guard batches: not deleted exactly once before the wrapper returned";
        }
        if (!g.deletesDisabled.load()) return "destroy batch: the stall guard did not switch the parallel deletes off";
        std::vector<FakeProxy> list(300);
        std::atomic<bool> retired{false};
        Prepare(list, &retired);
        g.handed.take();
        RunOne(&list, 0, 0, &retired);
        if (!AllOnce(list) || g.handed.take() != 0) return "destroy batch: after the stall guard: proxies still handed over";
    }
    return nullptr;
}
}  // namespace opf
