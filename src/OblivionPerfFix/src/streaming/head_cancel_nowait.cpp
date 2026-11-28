// HeadCancelNoWait (worker s057fix 2026-11-23, S-057; [Fixes] HeadCancelNoWait, default 0; start-up only).
//
// The stutter (S-057, Robber's Glen; findings cellunload, s057design, play1116, play112b, s057fix): a 64-68 ms frame in
// which the game thread waits 57-60 ms for the old engine's thread, which is inside the old engine's per-reference cancel
// of a character's queued 3D load (exe+0x67E6AA0, from Set3D(null)). The load is a composite task (QueuedCharacter, state
// 5 = its own step done) whose child QueuedHead is RUNNING on a loader worker (state 3): the worker builds the NPC's
// FaceGen head (QueuedHead::Run exe+0x67ED270: the global FaceGen lock, then the head build for the TESNPC), 62 ms for the
// NPC seen (Dro'Nahrahe, a travelling NPC at the edge of the loaded cells).
//
// Why the stock cancel waits: the task cancel (exe+0x67E0E00) cannot stop a running task (the worker checks no cancel
// flag and writes the states 4 and 5 with plain stores), so it polls Sleep(1) until the worker is done and then finishes
// the task itself (state 5 -> 6, the task's finish slot). For a child of a composite in state 6 nothing needs that child's
// result: the task cancel's compare-exchange to 6 comes before it calls the composite's slot 3, both when it cancels a
// load and when it finishes one (review revheadcancel S3: the gate below holds on both paths), so when the worker
// finishes the head, the composite's child-done step (vtable slot 9, exe+0x67E9480) counts it and the composite's "post
// for completion" (slot 10, exe+0x67E7240) sees state 6 and posts nothing; nothing reads the head after the loop.
//
// The fix: in the composite's child loop (exe+0x67E6610, the call of the task cancel at +0x48), on the OblivionThread, a
// QueuedHead child in state 3 or 4 of a QueuedReference / Tree / Character / Creature composite (not QueuedPlayer: review
// M1, the player's face data is rewritten exactly where the player's 3D is re-queued) in state 6 is not waited for. We
// take one reference on the child and one on the composite and keep the pair in a small list (16). The worker finishes
// the head exactly as it would while the stock canceller waits. Later, on the OblivionThread (the next time it reaches
// this loop, many times a second), once the child is in state 5 we make the stock finish call (the task cancel: 5 -> 6 and
// the child's finish slot, which for a QueuedHead only releases its parent link) and release our two references, so the
// composite's destructor runs on that thread as it does in the stock game. Everything else is unchanged: the same tasks
// run, the same results are discarded, the loader's map entry is erased at the same point, the composite's other
// children are cancelled as before.
// What moves: the moment the discarded head task is finished and freed (later, same thread), and the head build now
// overlaps the rest of the reference's teardown. The head build reads the NPC base form (TESNPC, a persistent form; we
// exclude dynamic FF forms and the player's form 7) and writes its own task and the NPC's face-cache fields (+0x2F0 /
// +0x2F8 released and zeroed at its start, +0x308 at its end). Those fields are NOT covered by the global FaceGen lock
// (review M2): the game's other face code touches them without it (exe+0x6845A60, 0x6845980, 0x6844F30, 0x6849130), and
// in the stock game those paths already overlap a running build whenever two references share one NPC base. For a
// unique NPC that overlap is new, so a child is held only while the NPC's face cache is empty (+0x2F0 and +0x2F8 zero,
// review S1): the running build's start then releases nothing that exists now, and its late +0x308 store is the value
// every builder writes ([[npc+0x1B0]+0x580]). It never reads the reference.
// Not taken over (stock wait): other threads, other child classes, other composites, a face cache that is set, a full
// list, any gate that fails.
//
// Start-up checks (all must hold, else not installed, one log line): the child loop (one match) with its call of the task
// cancel (the task cancel's state machine bytes at the call target), the composite cancel that calls that loop (one match),
// the worker's run step (state 4, run, state 5; one match), the QueuedHead vtable (the one vtable whose slot 3 is the child
// loop, slot 1 QueuedHead::Run naming "QueuedHead::Run()", slot 2 the jump to slot 10, slot 10 the parent notify) and the
// composite vtables (slot 3 the composite cancel, slot 9 the child-done step, slot 10 the post with its state-6 check;
// QueuedPlayer found once and left out).
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// exe+0x67E6610 (build 19115871), the base task's finish / cancel slot: for each child (array [this+20h]+8, count
// ushort [this+20h]+12h) the task cancel (mgr from a global), then release this+18h (the parent link).
constexpr char kLoop[] =
    "48 89 6C 24 18 56 48 83 EC 20 48 8B 41 20 33 ED 48 8B F1 48 85 C0 74 49 0F B7 40 12 66 3B E8 73 40 48 89 5C 24 30 "
    "8B DD 48 89 7C 24 38 8B F8 90 48 8B 46 20 48 8B 48 08 48 8B 14 0B 48 85 D2 74 0C 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 83 C3 08 48 83 EF 01 75 D9 48 8B 7C 24 38 48 8B 5C 24 30 48 8B 4E 18 48 85 C9 74 1D B8 FF FF FF FF F0 0F C1 41 08 "
    "83 F8 01 75 0A 48 8B 01 BA 01 00 00 00 FF 10 48 89 6E 18 48 8B 6C 24 40 48 83 C4 20 5E C3";
constexpr size_t kLoopCall = 0x48;  // E8 rel32 -> the task cancel (the patched instruction; rsi = the parent here)
// exe+0x67E0E00, the task cancel (the CancelWaitProbe's pattern): state > 5 -> return; jump table on the state
constexpr char kCancel[] =
    "48 89 5C 24 18 57 48 83 EC 20 48 8B DA 48 8B F9 8B 52 0C 83 FA 05 0F 87 ?? ?? ?? ?? 48 89 6C 24 30 BD 06 00 00 00";
// exe+0x67E66E0, the composite cancel (slot 3 of QueuedReference / Tree / Character / Creature / Player): [this+48h]
// state = 6; the child loop; erase the loader's map entry of [this+28h] (tail jump)
constexpr char kComposite[] =
    "40 53 48 83 EC 20 48 8B 41 48 48 8B D9 48 85 C0 74 07 C7 40 0C 06 00 00 00 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
    "48 8B 53 28 48 8B 48 10 48 8B 01 48 83 C4 20 5B 48 FF 60 28";
constexpr size_t kCompositeCall = 0x19;
// exe+0x67E0F20, the worker's run step (IOManager slot 18): state = 4; task slot 2; state = 5; then the counters
constexpr char kWorkerRun[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 02 48 8B D9 48 8B CA C7 42 0C 04 00 00 00 48 8B FA FF 50 10 48 8B 57 10 48 8B CB "
    "C7 47 0C 05 00 00 00";
// slot 9 of the composites (exe+0x67E9480): lock inc [[this+20h]+18h] (children done); jmp slot 10
constexpr char kChildDone[] = "48 8B 41 20 F0 FF 40 18 48 8B 01 48 FF 60 50";
// slot 10 of the composites (exe+0x67E7240): state >= 4, all children done, state != 6 -> post for completion
constexpr char kPost[] =
    "48 83 EC 28 8B 41 0C 4C 8B C1 83 F8 04 7C ?? 48 8B 41 20 48 85 C0 74 0E 0F B7 50 14 8B 40 18 3B D0 0F 94 C1 EB 05 "
    "B9 01 00 00 00 84 C9 74 ?? 41 8B 40 0C 83 F8 06 74";
// QueuedHead slot 1 = QueuedHead::Run (exe+0x67ED270): lea rdx,"QueuedHead::Run()"; lea rcx,FaceGen lock; call enter;
// mov rcx,[rsi+28h] (the TESNPC); ...
constexpr char kHeadRun[] =
    "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B F1 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 8B 4E 28 4C 8D 44 24 38";
constexpr size_t kHeadRunName = 0x12;  // lea rdx,[rip+X]
constexpr char kHeadRunText[] = "QueuedHead::Run()";
// QueuedHead slot 2: mov rax,[rcx]; jmp [rax+50h]
constexpr char kJump10[] = "48 8B 01 48 FF 60 50";
// QueuedHead slot 10 (exe+0x67E6D10): parent link set, state >= 4, own children done -> parent slot 9; release the link
constexpr char kHeadDone[] =
    "40 53 48 83 EC 20 48 83 79 18 00 48 8B D9 74 ?? 8B 41 0C 83 F8 04 7C ?? 48 8B 41 20 48 85 C0 74 0E 0F B7 48 14 "
    "8B 40 18 3B C8 0F 94 C2 EB 05 BA 01 00 00 00 84 D2 74 ?? 48 8B 4B 18 48 8B D3 48 8B 01 FF 50 48";

constexpr int kMaxComposites = 8;
constexpr uint8_t kNpcType = 0x23;  // TESNPC form type (QueuedHelmet's own check: cmp byte [rax+8],23h)
constexpr uint32_t kPlayerBaseId = 0x7;  // the player's TESNPC
constexpr char kPlayerComposite[] = ".?AVQueuedPlayer@@";  // review M1: the player's load keeps the stock wait

struct Item {
    uint8_t* child;
    uint8_t* parent;
    uint64_t mgr;
    DWORD thread;
    int64_t t0;
};

struct Ctx {
    HeadCancelCtx* api = nullptr;  // the functions and vtables in use (the game's, or the self-test's fakes)
    SRWLOCK lock = SRWLOCK_INIT;
    Item items[kHeadCancelMax] = {};
    std::atomic<int> count{0};
    std::atomic<uint64_t> deferred{0}, finished{0}, full{0}, maxHeldUs{0}, faceCacheSet{0}, offOt{0};
    std::atomic<uint32_t> testOt{0};  // self-test: 1 = the calling thread counts as the OblivionThread, 2 = it does not
};

Ctx g_ctx;
HeadCancelCtx g_api{};
bool g_installed = false;
uint8_t* g_site = nullptr;
uint8_t* g_stub = nullptr;
uint64_t g_loggedDeferred = 0, g_loggedFull = 0;
double g_qpcPerUs = 1;

int32_t Load32(const uint8_t* p) { return reinterpret_cast<const volatile int32_t*>(p)[0]; }
void* VtOf(const uint8_t* obj) { return *reinterpret_cast<void* const*>(obj); }
void AddRef(uint8_t* obj) { _InterlockedIncrement(reinterpret_cast<volatile long*>(obj + 8)); }
// The game's release: lock xadd [obj+8],-1; at 1 -> vtable slot 0 (obj, 1) (delete)
void Release(uint8_t* obj)
{
    if (_InterlockedExchangeAdd(reinterpret_cast<volatile long*>(obj + 8), -1) == 1) {
        using Dtor = void* (*)(uint8_t*, uint32_t);
        (*reinterpret_cast<Dtor const*>(VtOf(obj)))(obj, 1);
    }
}

int64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void MaxInto(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

thread_local int t_ot = -1;  // -1 not checked yet, 1 = the thread the game names "OblivionThread", 0 = any other

// The thread the game names "OblivionThread" (SetThreadDescription; as RiderBindGuard and OblivionThreadCaches tell it).
bool OnOblivionThread(const Ctx& c)
{
    if (const uint32_t t = c.testOt.load(std::memory_order_relaxed)) return t == 1;
    if (t_ot >= 0) return t_ot == 1;
    using DescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto desc = reinterpret_cast<DescFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    PWSTR name = nullptr;
    int ot = 0;
    if (desc && SUCCEEDED(desc(GetCurrentThread(), &name)) && name) {
        ot = wcscmp(name, L"OblivionThread") == 0 ? 1 : 0;
        LocalFree(name);
    }
    t_ot = ot;
    return ot == 1;
}

bool Eligible(Ctx& c, const uint8_t* child, const uint8_t* parent)
{
    const HeadCancelCtx& api = *c.api;
    if (!api.mode || !child || !parent) return false;
    if (VtOf(child) != api.headVt) return false;
    bool composite = false;
    for (int i = 0; i < api.composites; ++i) composite |= VtOf(parent) == api.compositeVt[i];
    if (!composite) return false;
    if (Load32(parent + 0xC) != 6) return false;  // the composite is being cancelled or finished (its CAS to 6 came first)
    const int32_t state = Load32(child + 0xC);
    if (state != 3 && state != 4) return false;  // running on a worker: what the stock cancel waits for
    const uint8_t* const npc = *reinterpret_cast<const uint8_t* const*>(child + 0x28);
    if (!npc || npc[8] != kNpcType) return false;
    const uint32_t id = *reinterpret_cast<const uint32_t*>(npc + 0x10);
    if ((id >> 24) == 0xFF) return false;  // not a dynamic form (those can be freed; ESM / plugin forms live for the session)
    if (id == kPlayerBaseId) return false;  // the player's own face (review M1)
    // the NPC's face cache must be empty (review S1): the running build's start releases +0x2F0 / +0x2F8
    if (*reinterpret_cast<const volatile uint64_t*>(npc + 0x2F0) || *reinterpret_cast<const volatile uint64_t*>(npc + 0x2F8)) {
        c.faceCacheSet.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!OnOblivionThread(c)) {  // review S2: held and finished on the OblivionThread only
        c.offOt.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool Defer(Ctx& c, uint64_t mgr, uint8_t* child, uint8_t* parent)
{
    AcquireSRWLockExclusive(&c.lock);
    const int n = c.count.load(std::memory_order_relaxed);
    if (n >= kHeadCancelMax) {
        ReleaseSRWLockExclusive(&c.lock);
        c.full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    AddRef(child);
    AddRef(parent);
    c.items[n] = Item{child, parent, mgr, GetCurrentThreadId(), Now()};
    c.count.store(n + 1, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&c.lock);
    c.deferred.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Finishes this thread's deferred children whose worker is done (state 5 or later): the stock finish call, then our refs.
int Drain(Ctx& c)
{
    Item done[kHeadCancelMax];
    int nd = 0;
    const DWORD self = GetCurrentThreadId();
    AcquireSRWLockExclusive(&c.lock);
    int n = c.count.load(std::memory_order_relaxed);
    for (int i = 0; i < n;) {
        const Item& it = c.items[i];
        if (it.thread == self && Load32(it.child + 0xC) >= 5) {
            done[nd++] = it;
            c.items[i] = c.items[--n];
        } else {
            ++i;
        }
    }
    c.count.store(n, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&c.lock);
    const int64_t now = Now();
    for (int i = 0; i < nd; ++i) {
        // state 5: the stock finish (CAS 5 -> 6, the child's finish slot). Nothing else can finish it: only the
        // composite's child loop knows the child, and the composite is cancelled.
        if (Load32(done[i].child + 0xC) == 5) c.api->cancel(done[i].mgr, reinterpret_cast<uint64_t>(done[i].child));
        Release(done[i].child);
        Release(done[i].parent);
        MaxInto(c.maxHeldUs, static_cast<uint64_t>(static_cast<double>(now - done[i].t0) / g_qpcPerUs));
    }
    if (nd) c.finished.fetch_add(static_cast<uint64_t>(nd), std::memory_order_relaxed);
    return nd;
}

uint64_t Step(Ctx& c, uint64_t mgr, uint8_t* child, uint8_t* parent)
{
    if (c.count.load(std::memory_order_relaxed)) Drain(c);
    if (Eligible(c, child, parent) && Defer(c, mgr, child, parent)) return 0;  // the loop ignores the result
    return c.api->cancel(mgr, reinterpret_cast<uint64_t>(child));
}

// Reached from the child loop's call site through the stub (rcx mgr, rdx child, r8 = rsi = the parent).
uint64_t LoopCallHook(uint64_t mgr, uint8_t* child, uint8_t* parent) { return Step(g_ctx, mgr, child, parent); }

struct Col {
    uint32_t signature, offset, cdOffset, typeDescriptor, hierarchy, self;
};
bool IsVtableStart(const GameModule& game, void* const* vt)
{
    const auto* col = reinterpret_cast<const Col*>(vt[-1]);
    if (!game.rdata.Contains(col)) return false;
    return col->signature == 1 && col->offset == 0 && game.base + col->self == reinterpret_cast<const uint8_t*>(col);
}

// The class name (".?AV...@@") of a complete-object vtable, or nullptr.
const char* RttiName(const GameModule& game, void* const* vt)
{
    const auto* col = reinterpret_cast<const Col*>(vt[-1]);
    const char* const name = reinterpret_cast<const char*>(game.base + col->typeDescriptor + 0x10);
    return game.data.Contains(name) ? name : nullptr;
}

bool CodeMatches(const GameModule& game, const void* fn, const char* pattern)
{
    return game.text.Contains(fn) && MatchesAt(static_cast<const uint8_t*>(fn), ParsePattern(pattern));
}

// Vtables in .rdata whose slot `slot` is fn (complete-object vtables only).
std::vector<void* const*> VtablesWithSlot(const GameModule& game, const void* fn, size_t slot)
{
    std::vector<void* const*> out;
    for (uint8_t* p = game.rdata.begin + 8 * (slot + 1); p + 8 <= game.rdata.end; p += 8) {
        if (*reinterpret_cast<void* const*>(p) != fn) continue;
        void* const* const vt = reinterpret_cast<void* const*>(p) - slot;
        if (IsVtableStart(game, vt)) out.push_back(vt);
    }
    return out;
}

bool Discover(const GameModule& game, HeadCancelNoWaitInfo* d, const char** why)
{
    const auto loops = FindAll(game.text, ParsePattern(kLoop));
    const auto comps = FindAll(game.text, ParsePattern(kComposite));
    const auto runs = FindAll(game.text, ParsePattern(kWorkerRun));
    d->matches = static_cast<int>(loops.size() * 100 + comps.size() * 10 + runs.size());
    if (loops.size() != 1 || comps.size() != 1 || runs.size() != 1) { *why = "the loader's task code not found once"; return false; }
    uint8_t* const loop = loops[0];
    uint8_t* const site = loop + kLoopCall;
    if (site[0] != 0xE8) { *why = "the child loop's call is already patched"; return false; }
    uint8_t* const cancel = RipTarget(site, 1, 5);
    if (!CodeMatches(game, cancel, kCancel)) { *why = "the child loop does not call the task cancel"; return false; }
    uint8_t* const composite = comps[0];
    if (RipTarget(composite + kCompositeCall, 1, 5) != loop) { *why = "the composite cancel does not call the child loop"; return false; }
    // QueuedHead: slot 3 the child loop, slot 1 QueuedHead::Run, slot 2 the jump to slot 10, slot 10 the parent notify
    void* const* head = nullptr;
    int heads = 0;
    for (void* const* vt : VtablesWithSlot(game, loop, 3)) {
        if (!CodeMatches(game, vt[1], kHeadRun)) continue;
        const char* const name = reinterpret_cast<const char*>(RipTarget(static_cast<const uint8_t*>(vt[1]) + kHeadRunName, 3, 7));
        if (!game.rdata.Contains(name) || strncmp(name, kHeadRunText, sizeof kHeadRunText) != 0) continue;
        if (!CodeMatches(game, vt[2], kJump10) || !CodeMatches(game, vt[10], kHeadDone)) continue;
        head = vt;
        ++heads;
    }
    if (heads != 1) { *why = "the QueuedHead vtable not found once"; return false; }
    int n = 0;
    d->playerSkipped = 0;
    for (void* const* vt : VtablesWithSlot(game, composite, 3)) {
        if (!CodeMatches(game, vt[9], kChildDone) || !CodeMatches(game, vt[10], kPost)) continue;
        const char* const name = RttiName(game, vt);
        if (!name) { *why = "a composite vtable without its class name"; return false; }
        if (strcmp(name, kPlayerComposite) == 0) {
            ++d->playerSkipped;
            continue;
        }
        if (n == kMaxComposites) { *why = "more composite vtables than expected"; return false; }
        d->compositeVt[n++] = vt;
    }
    if (n == 0) { *why = "no composite vtable with the expected child-done and post steps"; return false; }
    if (d->playerSkipped != 1) { *why = "QueuedPlayer's vtable not found once (it must be left out)"; return false; }
    d->loop = loop;
    d->site = site;
    d->cancel = cancel;
    d->composite = composite;
    d->workerRun = runs[0];
    d->headVt = head;
    d->composites = n;
    return true;
}

}  // namespace

bool HeadCancelNoWaitDiscover(const GameModule& game, HeadCancelNoWaitInfo* out)
{
    const char* why = "";
    return Discover(game, out, &why);
}

bool InstallHeadCancelNoWait(bool beforeGameCode, int mode)
{
    if (g_installed) return true;
    if (!mode) {
        Log("[headcancel] HeadCancelNoWait off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[headcancel] HeadCancelNoWait: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    const GameModule& game = Game();
    HeadCancelNoWaitInfo d{};
    const char* why = "";
    if (!Discover(game, &d, &why)) {
        Log("[headcancel] HeadCancelNoWait: %s (matches %d), not installed", why, d.matches);
        return false;
    }
    // stub: mov r8,rsi (the parent: the loop keeps `this` in rsi); jmp [rip+0] -> LoopCallHook. Reached by the loop's
    // 5-byte call, so the return goes back into the loop; r8 is volatile there (the original call may change it too).
    uint8_t* const stub = AllocNearGame(32);
    if (!stub) {
        Log("[headcancel] HeadCancelNoWait: no memory near the game's code, not installed");
        return false;
    }
    void* const hook = reinterpret_cast<void*>(&LoopCallHook);
    const uint8_t head[9] = {0x4C, 0x8B, 0xC6, 0xFF, 0x25, 0, 0, 0, 0};
    memcpy(stub, head, sizeof head);
    memcpy(stub + sizeof head, &hook, 8);
    DWORD old;
    if (!VirtualProtect(stub, 32, PAGE_EXECUTE_READ, &old)) {
        Log("[headcancel] HeadCancelNoWait: could not make the stub executable, not installed");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, 32);
    const int64_t rel = stub - (d.site + 5);
    if (rel != static_cast<int32_t>(rel)) {
        Log("[headcancel] HeadCancelNoWait: stub out of reach, not installed");
        return false;
    }
    g_api.cancel = reinterpret_cast<HeadCancelCtx::CancelFn>(d.cancel);
    g_api.headVt = d.headVt;
    g_api.composites = d.composites;
    for (int i = 0; i < d.composites; ++i) g_api.compositeVt[i] = d.compositeVt[i];
    g_api.mode = 1;
    g_ctx.api = &g_api;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcPerUs = static_cast<double>(f.QuadPart) / 1e6;
    uint8_t call[5] = {0xE8, 0, 0, 0, 0};
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(call + 1, &r32, 4);
    if (!WriteCode(d.site, std::span<const uint8_t>(call, 5))) {
        Log("[headcancel] HeadCancelNoWait: could not patch the child loop, not installed");
        return false;
    }
    g_site = d.site;
    g_stub = stub;
    g_installed = true;
    Log("[headcancel] HeadCancelNoWait installed (child loop exe+0x%X, call exe+0x%X of the task cancel exe+0x%X; QueuedHead "
        "vtable exe+0x%X, %d composite vtables, QueuedPlayer left out): a character load being cancelled or finished no "
        "longer waits for its running FaceGen head build on the OblivionThread",
        static_cast<unsigned>(d.loop - game.base), static_cast<unsigned>(d.site - game.base),
        static_cast<unsigned>(d.cancel - game.base),
        static_cast<unsigned>(reinterpret_cast<const uint8_t*>(d.headVt) - game.base), d.composites);
    return true;
}

void LogHeadCancelNoWaitStats()
{
    if (!g_installed) return;
    const uint64_t n = g_ctx.deferred.load(std::memory_order_relaxed), full = g_ctx.full.load(std::memory_order_relaxed);
    if (n == g_loggedDeferred && full == g_loggedFull) return;
    g_loggedDeferred = n;
    g_loggedFull = full;
    Log("[headcancel] character loads that did not wait for their running head build: %llu so far (%llu finished later on "
        "the OblivionThread, %d still held, held up to %.1f ms); waited as before: %llu list full, %llu face cache set, %llu "
        "not on the OblivionThread",
        static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_ctx.finished.load()), g_ctx.count.load(),
        g_ctx.maxHeldUs.load() / 1000.0, static_cast<unsigned long long>(full),
        static_cast<unsigned long long>(g_ctx.faceCacheSet.load()), static_cast<unsigned long long>(g_ctx.offOt.load()));
}

bool HeadCancelNoWaitState(HeadCancelNoWaitInfo* out)
{
    if (!g_installed) return false;
    out->site = g_site;
    out->stub = g_stub;
    out->headVt = g_api.headVt;
    out->composites = g_api.composites;
    return true;
}

// ---- self-test: the decision and the drain with fakes (no game code runs) ----
struct HeadCancelTest {
    Ctx ctx;
};
HeadCancelTest* HeadCancelTestNew(HeadCancelCtx* api)
{
    auto* t = new HeadCancelTest;
    t->ctx.api = api;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcPerUs = static_cast<double>(f.QuadPart) / 1e6;
    return t;
}
void HeadCancelTestFree(HeadCancelTest* t) { delete t; }
uint64_t HeadCancelTestStep(HeadCancelTest* t, uint64_t mgr, uint8_t* child, uint8_t* parent)
{
    return Step(t->ctx, mgr, child, parent);
}
int HeadCancelTestDrain(HeadCancelTest* t) { return Drain(t->ctx); }
int HeadCancelTestHeld(HeadCancelTest* t) { return t->ctx.count.load(); }
uint64_t HeadCancelTestDeferred(HeadCancelTest* t) { return t->ctx.deferred.load(); }
uint64_t HeadCancelTestFull(HeadCancelTest* t) { return t->ctx.full.load(); }
void HeadCancelTestOnOt(HeadCancelTest* t, int onOt) { t->ctx.testOt = onOt ? 1u : 2u; }
uint64_t HeadCancelTestFaceCacheSet(HeadCancelTest* t) { return t->ctx.faceCacheSet.load(); }
uint64_t HeadCancelTestOffOt(HeadCancelTest* t) { return t->ctx.offOt.load(); }

}  // namespace opf
