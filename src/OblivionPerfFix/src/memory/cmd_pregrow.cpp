// CmdMemPregrow: the direct command allocator pool is brought to its working size, with its command memory, before play starts
// (findings "cmdmem", "cmdpromote"; docs/fixes-explained.md).
//
// AMD's driver gives a D3D12 command allocator a 1 MB chunk at its 1st, 17th, 33rd ... list since its last Reset (empty lists too),
// a 64 KB chunk on the first RT clear and a 256 KB chunk on the first texture copy of each such group, and keeps them all across
// Reset. Stock ends a session with about two groups on nearly every direct allocator, because every allocator of the LIFO pool serves
// in time as the submission thread's barrier allocator. The hitches are the bursts when the pool runs dry at a busy moment and tens of
// brand-new allocators take their first chunks in one frame, all through the driver's one paging fence.
//
// So: once, before the first world is up, on this module's own thread, paced: the pool is brought to N allocators, each with those
// chunks. The lists recorded here are closed and never submitted, so nothing can reach the image; no game logic runs. On by default
// (user, after the play-test; findings "play1105"), for 16 GB cards and up only: the memory is held from early in the session, and no figure at the start can predict a card's budget falling later. The gates
// below lean towards off for that reason. The size and when it runs: findings "cmdpromote" (15.8 s after the DLL loads here, about
// 3 s long, about 9 s before the harness's first world and 10-20 s before a player's; a player who is faster, or a slow start, gets
// "off" or a stop at the fade to the world, never a pass in play).
//
// The give-back (findings "cmdgiveback"). Once, if the pass grew anything: when the game's video memory has been within
// CmdMemGiveBackHeadroomMB (512) of its budget for 3 seconds in a row, the direct pool is trimmed to the session's peak in flight plus a
// margin, about what the stock game would hold by then, with CommandAllocatorTrim's own pass (allocators the engine isn't using,
// stand-ins at the bottom of the pool, released a few at a time), and it is never trimmed again. That is safe on the direct pool for the same
// reasons as on the copy pool; only the copy pool is trimmed in general because repeated trims of a pool whose need returns with every busy
// scene made the engine create them again and again (findings 2026-10-12). One trim can't do that: a later scene that needs more makes them
// as the stock game does, and nothing takes them away again.
// Not the 2 GB "tight" signal of the copy pool: that is a normal state here, reached in every flight at 4K and in play sessions on the test PC (+79 to +204 s), and
// when it comes before the session's busiest moment it hands that moment's burst back (findings "cmdgiveback"). Normal sessions here stay above
// 0.54 GB, so 0.5 GB means the session is really running out.
//
// What happens at shutdown. The pool lives in the engine's FD3D12Device (deleted in FD3D12Adapter::CleanupResources); its destructor
// deletes the wrappers still in the pool, not the ones held here. Rendering ends before that (no obtains), so:
//  - before each batch and before each give-back the thread waits for a recent obtain (kQuietMs). While the engine is quiet it does not
//    touch the pool at all. A give-back waits for as long as it takes: if the game is quitting, the process exit ends the thread here;
//    the held wrappers and their D3D12 allocators are simply never freed by a process that is going away.
//  - recording only touches allocators held here and the D3D12 device, on which this thread holds a reference, so it stays valid
//    whatever the engine has deleted.
//  - everything runs under SEH (no objects with destructors in those frames): an access violation logs a line and ends the pass.
//  - device removed: no more pool or engine calls (the engine's release would turn a failed Reset into a fatal from this thread).
#include "common.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace opf {
IDXGIAdapter3* GameAdapter();  // memory_monitor.cpp: the game's GPU once it renders (not AddRef'd, set once)
namespace {

constexpr int kMax = 1024;              // the most wrappers one pass handles
constexpr int kListsPerGroup = 16;      // chunkstudy: a new group at the 17th, 33rd ... list
constexpr int kGroups = 2;              // census: stock ends with 2 groups on nearly every direct allocator
constexpr int kDredLists = 374;         // lists whose breadcrumb buffers stock the runtime's free list: batch = 22 allocators
constexpr int kReserveMax = 48;         // left in the pool for the engine while the rest is held
constexpr int kPaceMs = 3;              // pause per allocator
constexpr int kDelaySeconds = 5;        // after the device's first obtain
constexpr ULONGLONG kQuietMs = 100;     // an obtain this recent: the engine is alive. A quit from a world deletes the device 0.84-1.22 s after the last obtain (findings "cmdpromote"); a quit from
                                        // the menu or the logos has less to tear down and was not measured, so well under that: a late obtain only makes this thread wait a moment
constexpr ULONGLONG kQuietLimitMs = 30000;  // a batch waits this long for the engine, then the pass ends

// Stop reasons
enum { kGoOn = 0, kStopMemory, kStopWorld, kStopFailed, kStopQuit };
const char* StopName(int s)
{
    switch (s) {
    case kGoOn: return "no";
    case kStopMemory: return "memory floor reached";
    case kStopWorld: return "the world came up";
    case kStopFailed: return "a D3D12 call failed";
    case kStopQuit: return "the engine went quiet (quitting?) or the device was removed";
    }
    return "?";
}

// ---- The gates (pure, for the self-test) ----
struct Facts {
    bool amd = false, dedicated = false, wine = false, translate = false;
    bool fadeHook = false;    // the fade-to-game hook is in: the only way to see the first world coming
    bool engineLive = false;  // an obtain just now: the device is not on its way out
    double budgetMB = 0, usedMB = 0, ramFreeMB = 0, commitRoomMB = 0;
};
constexpr int kAutoN = 300;  // findings "cmdmem": 260 fell short on rw-bridge (in-flight peak 267); "cmdpromote"
// CmdMemPregrow's limits; why each has this value: findings "cmdpromote".
// The memory is video memory in the game (held view, 300 allocators: local +503 MB, non-local +19 MB, RAM free -45 MB; findings "cmdpromote"),
// about 2.7 MB per allocator once used as stock uses them, given back only when the card is nearly full (the give-back, below). Over budget with little RAM free is what preceded this
// PC's GPU hangs (findings "gpuhang"), so the gates lean towards off: an "off" costs stutter, a wrong "on" could cost a hang.
// DXGI's budget before the game fills the card only says how big the card is: 15.06 GB of 15.81 here in all 245 logs that have both figures, also ahead of an
// hour that went down to 6 GB. What others hold shows only under pressure (typically 11.4-13 GB here, so a 16 GB card leaves about 12 GB; the game peaked at 9.7-11.3 GB
// at 4K Ultra/High). A 12 GB card would read about 11.4 GB now and could leave under 9 GB later, over budget in stock already; only 16 GB has been tested. So: 16 GB cards and up.
constexpr double kFloorBudgetMB = 14000;    // budget at the start
constexpr double kFullBudgetMB = 15000;     // full size from here; at the floor 200, about stock's own high-water (170-221 in road flights)
constexpr double kFloorSize = 200;
constexpr double kFloorVramHeadMB = 2048;   // budget minus use at the start
constexpr double kFloorRamMB = 6144;        // available RAM at the start: what goes over budget lands in RAM
constexpr double kFloorCommitMB = 4096;     // commit room at the start
constexpr double kStopVramHeadMB = 1024;    // under way
constexpr double kStopRamMB = 3072;
constexpr double kStopCommitMB = 2048;
constexpr double kAssumeMB = 2.7;           // what one allocator ends up holding (census), at least; the probe sees the recipe's fresh chunks
constexpr double kCostMaxMB = 4.0;          // MB per allocator of the recipe, both memory segments
constexpr double kCostMinMB = 0.5;
constexpr int kMinN = 64;                   // under this the pool would run dry anyway

// The gates that wait for nothing: nullptr = may run.
const char* FixedGate(const Facts& f)
{
    if (!f.amd) return "not an AMD GPU (the driver rule this relies on was only measured there)";
    if (f.wine) return "Wine or Proton (another allocator model, no benefit known)";
    if (!f.dedicated) return "the GPU has no video memory of its own, or that is unknown";
    if (!f.fadeHook) return "the fade-to-game hook is not in (PrepareCollisionBehindLoadingScreen off, or not found), so the first world can't be seen coming";
    return nullptr;
}
// The gates that may open while the game starts: nullptr = may run.
const char* OpeningGate(const Facts& f)
{
    if (!f.engineLive) return "the engine is not rendering just now";  // never with the device on its way out (a quit at the menu frees memory, which opens the gates below)
    if (!f.translate) return "RdgParallelTranslate is not on (it is what drives this pool)";
    if (f.budgetMB < kFloorBudgetMB) return "the video memory budget is under the floor";
    if (f.budgetMB - f.usedMB < kFloorVramHeadMB) return "too little video memory free in the budget";
    if (f.ramFreeMB < kFloorRamMB) return "too little free RAM";
    if (f.commitRoomMB < kFloorCommitMB) return "too little commit room";
    return nullptr;
}
// The size: the requested one (1 = automatic), less where the card or the RAM is near the floors. costs in MB per allocator.
int SizeFor(const Facts& f, int requested, double costLocal, double costNonLocal)
{
    int n = requested;
    if (requested <= 1) {
        const double share = (f.budgetMB - kFloorBudgetMB) / (kFullBudgetMB - kFloorBudgetMB);
        n = share >= 1 ? kAutoN : static_cast<int>(kFloorSize + (kAutoN - kFloorSize) * (share < 0 ? 0 : share));
    }
    if (n > kMax) n = kMax;
    const double cost = costLocal + costNonLocal > kAssumeMB ? costLocal + costNonLocal : kAssumeMB;
    const double spareVram = f.budgetMB - f.usedMB - kFloorVramHeadMB, spareRam = f.ramFreeMB - kFloorRamMB;
    if (spareVram / cost < n) n = static_cast<int>(spareVram / cost);
    if (costNonLocal > 0.1 && spareRam / costNonLocal < n) n = static_cast<int>(spareRam / costNonLocal);
    return n < kMinN ? 0 : n;
}
// Under way, before each batch: a floor reached means stop (a little under the start's, so that a start at the floor is not stopped at once).
bool MemoryOk(const Facts& f)
{
    return f.budgetMB - f.usedMB >= kStopVramHeadMB && f.ramFreeMB >= kStopRamMB && f.commitRoomMB >= kStopCommitMB;
}
// The first-use cost the driver just showed, MB per allocator of the recipe: outside this the rule is not what was measured.
const char* CostGate(double costLocal, double costNonLocal)
{
    const double total = costLocal + costNonLocal;
    if (total > kCostMaxMB) return "the driver takes more memory per allocator than measured";
    if (total < kCostMinMB) return "the driver took almost no memory: the rule no longer holds, nothing to gain";
    return nullptr;
}

// ---- The give-back's decisions (pure, for the self-test) ----
constexpr int kGiveBackSeconds = 3;         // under the line this long in a row: not a blip of an arrival
constexpr int64_t kKeepMargin = 16;         // over the session's peak in flight: this or an eighth of it (the copy trim's margin, doubled at the small end)
constexpr int64_t kKeepFloor = 64;          // the pass's own "under this the pool would run dry anyway"
constexpr int64_t kGiveBackMinSurplus = 8;  // the copy trim's "a clear surplus"; less than this: nothing to gain
struct GiveBackFacts {
    bool armed = false;        // a pass finished and grew something
    bool trimFailed = false;   // an exception in a trim pass: the pool is not to be touched again
    bool engineLive = false;   // an obtain just now
    bool deviceOld = false;    // the device has been the same for a while (as the copy trim: never near its creation)
    double budgetMB = 0, usedMB = 0;
    int headroomMB = 0;        // 0: never
};
struct GiveBackState {
    bool done = false;  // once a session, whatever came of it
    int under = 0;
};
// Once a second. True: trim now (and never again).
bool GiveBackStep(GiveBackState& s, const GiveBackFacts& f)
{
    if (s.done || !f.armed || f.headroomMB <= 0) return false;
    if (f.trimFailed) {
        s.done = true;
        return false;
    }
    // An unknown budget (0) is not a full card.
    s.under = f.budgetMB > 0 && f.budgetMB - f.usedMB < f.headroomMB ? s.under + 1 : 0;
    if (s.under < kGiveBackSeconds || !f.engineLive || !f.deviceOld) return false;  // the count stays: it goes at once when the engine is back
    s.done = true;
    return true;
}
struct GiveBackPlan {
    int64_t keepTotal = 0, keepFree = 0, surplus = 0;
};
// real: D3D12 allocators that exist; inFlight: those the engine has out now; peak: the most it has had out this session.
GiveBackPlan PlanGiveBack(int64_t real, int64_t inFlight, int64_t peak)
{
    GiveBackPlan p;
    const int64_t margin = peak / 8 > kKeepMargin ? peak / 8 : kKeepMargin;
    p.keepTotal = peak + margin > kKeepFloor ? peak + margin : kKeepFloor;
    p.keepFree = p.keepTotal > inFlight ? p.keepTotal - inFlight : 0;
    const int64_t free = real - inFlight;
    p.surplus = free > p.keepFree ? free - p.keepFree : 0;
    if (p.surplus < kGiveBackMinSurplus) p.surplus = 0;
    return p;
}

// ---- The pass, pool handling (injected operations, for the self-test) ----
struct Ops {
    void* ctx;
    void* (*pop)(void* ctx);                 // a free wrapper off the pool, nullptr if none
    void* (*obtain)(void* ctx);              // the engine's obtain: nullptr if it failed
    void (*release)(void* ctx, void* w);     // the engine's release (Reset, back to the pool)
    int64_t (*created)(void* ctx);           // direct allocators that exist
    HRESULT (*record)(void* ctx, void* w, int lists, int* listsDone);  // the recipe on this wrapper's allocator
    HRESULT (*unbind)(void* ctx);            // the private list off the last allocator it recorded into
    HRESULT (*resetAlloc)(void* ctx, void* w);  // the wrapper's allocator Reset, here, with the result checked
    int (*check)(void* ctx);                 // before a batch: 0 or a stop reason
    bool (*mayTouchPool)(void* ctx);         // before a give-back: waits for a live engine; false = never touch it again (device removed)
    void (*pace)(void* ctx);
};
struct PlanResult {
    int had = 0, made = 0, total = 0, grown = 0, lists = 0, lost = 0, abandoned = 0, stop = kGoOn;
    HRESULT hr = S_OK;
    bool obtainNull = false;
    double longestHoldMs = 0;
};

double NowMs()
{
    static const double perMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1000.0; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / perMs;
}

void* s_held[kMax];
void* s_out[kMax];
void* s_warmBack[kMax];
void* s_warm[kMax];  // wrappers grown here: compared by address only, never read after they went back (the 1.1.0-beta race)

// Grows in[0..b): none is Reset until the batch ends, so the batch's lists hold that many DRED buffers at once. Then the private list
// goes off the last allocator, and every allocator is Reset here with the result checked: one whose Reset failed stays off the pool.
void GrowBatch(const Ops& o, void* const* in, int b, int lists, void** out, int* nOut, int* nWarm, PlanResult& r)
{
    for (int i = 0; i < b; ++i) {
        const HRESULT hr = o.record(o.ctx, in[i], lists, &r.lists);
        if (FAILED(hr)) {
            r.hr = hr;
            r.stop = kStopFailed;
            break;
        }
        if (*nWarm < kMax) s_warm[(*nWarm)++] = in[i];
        o.pace(o.ctx);
    }
    const HRESULT u = o.unbind(o.ctx);
    if (FAILED(u)) {  // the list's state is unknown: the Resets below decide, one by one, which allocator may go back
        if (SUCCEEDED(r.hr)) r.hr = u;
        r.stop = kStopFailed;
    }
    for (int i = 0; i < b; ++i) {
        const HRESULT hr = o.resetAlloc(o.ctx, in[i]);
        if (FAILED(hr)) {
            if (SUCCEEDED(r.hr)) r.hr = hr;
            r.stop = kStopFailed;
            ++r.lost;
            continue;
        }
        out[(*nOut)++] = in[i];
    }
}

// Back to the pool, in[] in order (the last is taken next). false: not back (the engine may be gone).
bool GiveBack(const Ops& o, void* const* in, int n, PlanResult& r)
{
    if (!n) return true;
    if (!o.mayTouchPool(o.ctx)) {
        r.abandoned += n;
        r.stop = kStopQuit;
        return false;
    }
    for (int i = 0; i < n; ++i) o.release(o.ctx, in[i]);
    return true;
}

void RunPlan(const Ops& o, int target, int lists, int batch, PlanResult& r)
{
    if (target > kMax) target = kMax;
    int nWarm = 0;
    const auto isWarm = [&](void* w) { for (int i = 0; i < nWarm; ++i) if (s_warm[i] == w) return true; return false; };
    r.stop = o.check(o.ctx);
    if (r.stop) return;
    // 1: the pool brought to the target in one short pass (the engine's own obtain makes what is missing, the constructor ~0.03 ms each).
    double t0 = NowMs();
    int n = 0;
    for (void* w; n < kMax && (w = o.pop(o.ctx)) != nullptr;) s_held[n++] = w;
    r.had = n;
    while (n < kMax && o.created(o.ctx) < target) {
        void* const w = o.obtain(o.ctx);
        if (!w) { r.obtainNull = true; break; }
        s_held[n++] = w;
    }
    r.made = n - r.had;
    if (!GiveBack(o, s_held, n, r)) return;
    {
        const double held = NowMs() - t0;
        if (held > r.longestHoldMs) r.longestHoldMs = held;
    }
    const int64_t c = o.created(o.ctx);
    r.total = c > kMax ? kMax : static_cast<int>(c);
    const int grow = target < r.total ? target : r.total;
    const int reserve = grow / 4 < kReserveMax ? grow / 4 : kReserveMax;

    // 2: batches off the top. Grown wrappers stay here until the pool is down to the reserve, so the next pop finds a cold one.
    int nOut = 0;
    while (!r.stop && nWarm < grow - reserve) {
        r.stop = o.check(o.ctx);
        if (r.stop) break;
        int b = 0;
        while (b < batch && nWarm + b < grow - reserve) {
            void* const w = o.pop(o.ctx);
            if (!w) break;
            s_held[b++] = w;
        }
        if (!b) break;
        GrowBatch(o, s_held, b, lists, s_out, &nOut, &nWarm, r);
    }
    if (!GiveBack(o, s_out, nOut, r)) { r.grown = nWarm; return; }

    // 3: the reserve: everything off the pool for an instant, the grown ones straight back, then the cold ones in the same batches.
    // Twice, for those the engine had out. What exceeds the target goes back as it is: the pass stops growing there.
    for (int pass = 0; pass < 2 && !r.stop; ++pass) {
        r.stop = o.check(o.ctx);
        if (r.stop) break;
        t0 = NowMs();
        int a = 0, cold = 0;
        for (void* w; a + cold < kMax && (w = o.pop(o.ctx)) != nullptr;) {
            if (isWarm(w)) s_warmBack[a++] = w;
            else s_held[cold++] = w;
        }
        const int room = grow - nWarm;
        const int toGrow = cold < room ? cold : (room < 0 ? 0 : room);
        if (!GiveBack(o, s_held + toGrow, cold - toGrow, r)) { r.abandoned += toGrow + a; break; }
        if (!GiveBack(o, s_warmBack, a, r)) { r.abandoned += toGrow; break; }
        {
            const double held = NowMs() - t0;
            if (held > r.longestHoldMs) r.longestHoldMs = held;
        }
        nOut = 0;
        int done = 0;
        while (done < toGrow && !r.stop) {
            const int b = toGrow - done < batch ? toGrow - done : batch;
            GrowBatch(o, s_held + done, b, lists, s_out, &nOut, &nWarm, r);
            done += b;
            if (done < toGrow && !r.stop) r.stop = o.check(o.ctx);
        }
        // Any not reached first (below the grown ones, which are taken next)
        if (!GiveBack(o, s_held + done, toGrow - done, r)) { r.abandoned += nOut; break; }
        if (!GiveBack(o, s_out, nOut, r)) break;
        if (!toGrow) break;
    }
    r.grown = nWarm;
}

// ---- The pass, for real ----
struct Res {
    ID3D12Device* dev = nullptr;
    ID3D12CommandAllocator* own = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* rt = nullptr;
    ID3D12Resource* src = nullptr;
    ID3D12Resource* dst = nullptr;
    ID3D12DescriptorHeap* rtv = nullptr;
};
struct Pass {
    void* device = nullptr;  // the engine's FD3D12Device
    IDXGIAdapter3* adapter = nullptr;
    Res r;
    int requested = 0;
    int test = 0;  // lab build only: a faked condition
    int batches = 0;
};

std::atomic<int> g_requested{0};  // the key, once armed
std::atomic<int> g_state{0};      // 0 waiting, 1 running or finished
std::atomic<int> g_passGrown{0};  // set when the pass has finished: what it grew (arms the give-back; nothing grown or an exception: never)
std::atomic<int> g_giveBackMB{0};
GiveBackState g_giveBack;         // monitor thread
ULONGLONG g_passEndMs = 0, g_giveBackAtMs = 0;  // g_passEndMs: written before g_passGrown
const char* g_lastWhy = nullptr;  // why the waiting gates are still closed (monitor thread)
Pass g_pass;

int PaceMs()
{
    return kPaceMs;
}

ID3D12CommandAllocator* CaOf(void* wrapper) { return static_cast<ID3D12CommandAllocator*>(CmdMemD3dOf(wrapper)); }
bool Growable(ID3D12CommandAllocator* ca) { return ca && reinterpret_cast<void*>(ca) != AllocatorTrimTestStub(); }

// The world test: true once the first loading screen has ended. Without the hook there is nothing to go by: FixedGate keeps the pass off.
bool WorldIsUp()
{
    return FadeToGameCalls(nullptr) != 0;
}

void Sample(IDXGIAdapter3* ad, Facts& f, double* nonLocalMB)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO l{}, nl{};
    if (ad) {
        ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l);
        ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nl);
    }
    constexpr double M = 1048576.0;
    f.budgetMB = static_cast<double>(l.Budget) / M;
    f.usedMB = static_cast<double>(l.CurrentUsage) / M;
    if (nonLocalMB) *nonLocalMB = static_cast<double>(nl.CurrentUsage) / M;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        f.ramFreeMB = static_cast<double>(ms.ullAvailPhys) / M;
        f.commitRoomMB = static_cast<double>(ms.ullAvailPageFile) / M;
    }
    uint64_t room = 0, avail = 0;
    if (LatestCommitRoom(&room, &avail) && static_cast<double>(room) / M > f.commitRoomMB) f.commitRoomMB = static_cast<double>(room) / M;  // with page file growth room
}

HRESULT MakeTexture(ID3D12Device* dev, UINT size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ID3D12Resource** out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = size;
    d.Height = size;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(out));
}

HRESULT InitRes(Res& r, ID3D12CommandAllocator* any)
{
    HRESULT hr = any->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&r.dev));
    if (FAILED(hr)) return hr;
    hr = r.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&r.own));
    if (FAILED(hr)) return hr;
    hr = r.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.own, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&r.list));
    if (FAILED(hr)) return hr;
    hr = r.list->Close();
    if (FAILED(hr)) return hr;
    hr = MakeTexture(r.dev, 16, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &r.rt);
    if (FAILED(hr)) return hr;
    hr = MakeTexture(r.dev, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_SOURCE, &r.src);
    if (FAILED(hr)) return hr;
    hr = MakeTexture(r.dev, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &r.dst);
    if (FAILED(hr)) return hr;
    D3D12_DESCRIPTOR_HEAP_DESC dd{};
    dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    dd.NumDescriptors = 1;
    hr = r.dev->CreateDescriptorHeap(&dd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&r.rtv));
    if (FAILED(hr)) return hr;
    r.dev->CreateRenderTargetView(r.rt, nullptr, r.rtv->GetCPUDescriptorHandleForHeapStart());
    return S_OK;
}

void FreeRes(Res& r)
{
    if (r.list) r.list->Release();
    if (r.own) r.own->Release();
    if (r.rt) r.rt->Release();
    if (r.src) r.src->Release();
    if (r.dst) r.dst->Release();
    if (r.rtv) r.rtv->Release();
    if (r.dev) r.dev->Release();
    r = Res{};
}

// Lists one allocator needs for `groups` chunk groups: the group's first list opens the group, 16 lists fit one group.
int ListsFor(int groups) { return groups <= 1 ? 1 : kListsPerGroup * (groups - 1) + 1; }
// Allocators in one batch: none is Reset until the batch ends, so the batch's lists hold that many DRED buffers at once.
int BatchFor(int dredLists, int groups)
{
    const int lists = ListsFor(groups);
    const int64_t b = (static_cast<int64_t>(dredLists) + lists - 1) / lists;
    return b < 1 ? 1 : (b > kMax ? kMax : static_cast<int>(b));
}

HRESULT RecordOn(Res& r, ID3D12CommandAllocator* ca, int lists, int* listsDone)
{
    for (int i = 0; i < lists; ++i) {
        HRESULT hr = r.list->Reset(ca, nullptr);
        if (FAILED(hr)) return hr;
        ++*listsDone;
        if (i % kListsPerGroup == 0) {
            const float c[4] = {};
            r.list->ClearRenderTargetView(r.rtv->GetCPUDescriptorHandleForHeapStart(), c, 0, nullptr);
            r.list->CopyResource(r.dst, r.src);
        }
        hr = r.list->Close();
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

HRESULT Unbind(Res& r)
{
    // The private list is closed: its own allocator can be Reset, so it never grows past one group.
    HRESULT hr = r.own->Reset();
    if (FAILED(hr)) return hr;
    hr = r.list->Reset(r.own, nullptr);
    if (FAILED(hr)) return hr;
    return r.list->Close();
}

// What this driver takes now for the recipe, MB per allocator, on allocators of this pass's own (freed again). The game may be
// loading at this moment: each round is a few ms, the median round is free of whatever else allocates.
bool ProbeCost(Pass& p, double* costLocal, double* costNonLocal)
{
    constexpr int kRounds = 5, kEach = 4;
    double l[kRounds], nl[kRounds];
    int rounds = 0, listsIgnored = 0;
    const int lists = ListsFor(kGroups);
    for (int round = 0; round < kRounds; ++round) {
        ID3D12CommandAllocator* a[kEach] = {};
        HRESULT hr = S_OK;
        for (int i = 0; i < kEach && SUCCEEDED(hr); ++i)
            hr = p.r.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&a[i]));
        Facts f0, f1;
        double n0 = 0, n1 = 0;
        if (SUCCEEDED(hr)) Sample(p.adapter, f0, &n0);
        for (int i = 0; i < kEach && SUCCEEDED(hr); ++i) hr = RecordOn(p.r, a[i], lists, &listsIgnored);
        if (SUCCEEDED(hr)) Sample(p.adapter, f1, &n1);
        const HRESULT u = Unbind(p.r);
        for (int i = 0; i < kEach; ++i) if (a[i]) a[i]->Release();
        if (FAILED(hr) || FAILED(u)) return false;
        l[rounds] = (f1.usedMB - f0.usedMB) / kEach;
        nl[rounds] = (n1 - n0) / kEach;
        ++rounds;
    }
    // Median by the sum (a few values: a plain sort).
    for (int i = 1; i < rounds; ++i)
        for (int j = i; j > 0 && l[j - 1] + nl[j - 1] > l[j] + nl[j]; --j) {
            const double a = l[j], b = nl[j];
            l[j] = l[j - 1], nl[j] = nl[j - 1];
            l[j - 1] = a, nl[j - 1] = b;
        }
    *costLocal = l[rounds / 2];
    *costNonLocal = nl[rounds / 2];
    return true;
}

void* OpPop(void* c) { return CmdMemPopDirect(static_cast<Pass*>(c)->device); }
void* OpObtain(void* c) { return CmdMemObtainDirect(static_cast<Pass*>(c)->device); }
void OpRelease(void* c, void* w) { CmdMemReleaseDirect(static_cast<Pass*>(c)->device, w); }
int64_t OpCreated(void*) { return CmdMemDirectCreated(); }
HRESULT OpRecord(void* c, void* w, int lists, int* done)
{
    ID3D12CommandAllocator* const ca = CaOf(w);
    if (!Growable(ca)) return S_OK;
    return RecordOn(static_cast<Pass*>(c)->r, ca, lists, done);
}
HRESULT OpUnbind(void* c) { return Unbind(static_cast<Pass*>(c)->r); }
HRESULT OpResetAlloc(void*, void* w)
{
    ID3D12CommandAllocator* const ca = CaOf(w);
    return Growable(ca) ? ca->Reset() : S_OK;
}
bool EngineActive() { return GetTickCount64() - CmdMemLastObtainMs() <= kQuietMs; }
int OpCheck(void* c)
{
    Pass& p = *static_cast<Pass*>(c);
    if (FAILED(p.r.dev->GetDeviceRemovedReason())) return kStopQuit;
    for (const ULONGLONG t0 = GetTickCount64(); !EngineActive();) {
        if (GetTickCount64() - t0 > kQuietLimitMs) return kStopQuit;
        Sleep(10);
    }
    if (WorldIsUp()) return kStopWorld;
    ++p.batches;
    Facts f;
    Sample(p.adapter, f, nullptr);
    return MemoryOk(f) ? kGoOn : kStopMemory;
}
bool OpMayTouchPool(void* c)
{
    Pass& p = *static_cast<Pass*>(c);
    for (;;) {
        if (FAILED(p.r.dev->GetDeviceRemovedReason())) return false;
        if (EngineActive()) return true;
        Sleep(10);  // for as long as it takes: at a quit the process exit ends this thread here
    }
}
void OpPace(void*)
{
    const int ms = PaceMs();
    if (ms > 0) Sleep(static_cast<DWORD>(ms));
}

// SEH frame: no objects with destructors.
int RunGuarded(Pass& p, int target, PlanResult& r)
{
    __try {
        const Ops o{&p, &OpPop, &OpObtain, &OpRelease, &OpCreated, &OpRecord, &OpUnbind, &OpResetAlloc, &OpCheck, &OpMayTouchPool, &OpPace};
        RunPlan(o, target, ListsFor(kGroups), BatchFor(kDredLists, kGroups), r);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}
int InitGuarded(Pass& p, double* costLocal, double* costNonLocal, HRESULT* hr, LUID* luid)
{
    __try {
        if (!EngineActive()) { *hr = E_ABORT; return 0; }  // the engine stopped between the tick and this thread: the device may be going
        // Any allocator will do for the device: one off the pool, straight back.
        void* w = CmdMemPopDirect(p.device);
        if (!w) w = CmdMemObtainDirect(p.device);
        if (!w) { *hr = E_OUTOFMEMORY; return 0; }
        ID3D12CommandAllocator* const ca = CaOf(w);
        const HRESULT made = Growable(ca) ? InitRes(p.r, ca) : E_FAIL;
        // The rule of every give-back: while the engine is quiet the pool is not touched. A failed InitRes may not have the device: then only the wait.
        while (!EngineActive()) {
            if (p.r.dev && FAILED(p.r.dev->GetDeviceRemovedReason())) { *hr = E_ABORT; return 0; }
            Sleep(10);
        }
        *hr = made;
        CmdMemReleaseDirect(p.device, w);
        if (FAILED(*hr)) return 0;
        *luid = p.r.dev->GetAdapterLuid();
        if (!ProbeCost(p, costLocal, costNonLocal)) *hr = E_FAIL;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}
int FreeGuarded(Res& r)
{
    __try {
        FreeRes(r);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// What the pass grew (0: nothing, or an exception: nothing may rely on the pool's state then).
int PassBody()
{
    // Normal priority: it holds the pool and takes driver locks for moments, so it must not wait behind the game's workers while it
    // does (the pace keeps the pass light).
    Pass& p = g_pass;
    const double t0 = NowMs();
    double costLocal = 0, costNonLocal = 0;
    HRESULT hr = S_OK;
    LUID luid{};
    if (InitGuarded(p, &costLocal, &costNonLocal, &hr, &luid)) {
        Log("[cmdmem] pre-grow off: an exception while setting up: nothing done");
        return 0;
    }
    if (hr == E_ABORT) {
        Log("[cmdmem] pre-grow off: the engine stopped rendering as it started (quitting?): nothing done");
        FreeGuarded(p.r);
        return 0;
    }
    if (FAILED(hr)) {
        Log("[cmdmem] pre-grow off: setting up failed (hr 0x%08X): nothing done", static_cast<unsigned>(hr));
        FreeGuarded(p.r);
        return 0;
    }
    DXGI_ADAPTER_DESC ad{};
    const bool sameGpu = SUCCEEDED(p.adapter->GetDesc(&ad)) && ad.AdapterLuid.LowPart == luid.LowPart && ad.AdapterLuid.HighPart == luid.HighPart;
    bool otherGpu = !sameGpu;
    if (otherGpu) {
        Log("[cmdmem] pre-grow off: the device is not on the GPU the memory figures come from: nothing done");
        FreeGuarded(p.r);
        return 0;
    }
    if (const char* const why = CostGate(costLocal, costNonLocal)) {
        Log("[cmdmem] pre-grow off: %s (%.2f MB video memory + %.2f MB system memory): nothing done", why, costLocal, costNonLocal);
        FreeGuarded(p.r);
        return 0;
    }
    Facts f0;
    double nl0 = 0;
    Sample(p.adapter, f0, &nl0);
    const int target = SizeFor(f0, p.requested, costLocal, costNonLocal);
    if (target <= 0) {
        Log("[cmdmem] pre-grow off: not enough memory to spare for a useful size: nothing done (budget %.0f MB, used %.0f MB, RAM free %.0f MB)", f0.budgetMB, f0.usedMB, f0.ramFreeMB);
        FreeGuarded(p.r);
        return 0;
    }
    Log("[cmdmem] pre-grow: starting, %d direct allocators, about %.2f MB video memory + %.2f MB system memory each; budget %.0f MB, used %.0f MB, non-local %.0f MB, RAM free %.0f MB, commit room %.0f MB",
        target, costLocal, costNonLocal, f0.budgetMB, f0.usedMB, nl0, f0.ramFreeMB, f0.commitRoomMB);
    PlanResult r;
    const int ex = RunGuarded(p, target, r);
    if (ex) Log("[cmdmem] pre-grow: an exception in the pass: it ends here; whatever it held stays off the pool; no give-back");
    Facts f1;
    double nl1 = 0;
    Sample(p.adapter, f1, &nl1);
    if (!ex) FreeGuarded(p.r);
    Log("[cmdmem] pre-grow: %d direct allocators grown, %d exist (%d new), %d lists, %.0f ms, longest pool hold %.1f ms; video memory %.0f -> %.0f MB, non-local %.0f -> %.0f MB, RAM free %.0f -> %.0f MB, commit room %.0f -> %.0f MB; stopped early: %s; hr 0x%08X, %d left off the pool%s",
        r.grown, r.total, r.made, r.lists, NowMs() - t0, r.longestHoldMs, f0.usedMB, f1.usedMB, nl0, nl1, f0.ramFreeMB, f1.ramFreeMB, f0.commitRoomMB, f1.commitRoomMB, StopName(r.stop), static_cast<unsigned>(r.hr), r.lost + r.abandoned, r.obtainNull ? ", the engine's obtain returned nothing" : "");
    return ex ? 0 : r.grown;
}

DWORD WINAPI PassThread(void*)
{
    const int grown = PassBody();
    g_passEndMs = GetTickCount64();
    g_passGrown.store(grown);
    return 0;
}


void Off(const char* why)
{
    g_state = 1;
    Log("[cmdmem] pre-grow off: %s: nothing done", why);
}

}  // namespace

// allocator_trim.cpp, AllocatorTrimTick (monitor thread, once a second)
void CmdMemPregrowTick(void* device, uint64_t deviceAgeMs)
{
    if (g_state.load() || g_requested.load() <= 0 || !device) return;
    const uint64_t delay = static_cast<uint64_t>(kDelaySeconds) * 1000;
    if (deviceAgeMs < delay) return;
    // Only before play: after the first loading screen it would be the very burst it exists to prevent.
    if (WorldIsUp()) {
        char why[220];
        snprintf(why, sizeof why, "the world came up before it could start%s%s", g_lastWhy ? ": " : "", g_lastWhy ? g_lastWhy : "");
        Off(why);
        return;
    }
    IDXGIAdapter3* const ad = GameAdapter();
    if (!ad) { g_lastWhy = "the game's GPU is not known yet"; return; }
    if (GameGpuMemoryKind() == GpuMemoryKind::Pending) { g_lastWhy = "the kind of GPU memory is not known yet"; return; }
    Facts f;
    DXGI_ADAPTER_DESC d{};
    f.amd = SUCCEEDED(ad->GetDesc(&d)) && d.VendorId == 0x1002;
    f.dedicated = GameGpuMemoryKind() == GpuMemoryKind::Dedicated;
    f.wine = SharedGpuHostFacts().wine;
    f.translate = RdgParallelTranslateActive();
    FadeToGameCalls(&f.fadeHook);
    f.engineLive = EngineActive();
    Sample(ad, f, nullptr);
    if (const char* const why = FixedGate(f)) {
        char line[200];
        snprintf(line, sizeof line, "%s (vendor 0x%04X)", why, d.VendorId);
        Off(line);
        return;
    }
    if (const char* const why = OpeningGate(f)) {
        char buf[260];
        static char s_why[260];
        snprintf(buf, sizeof buf, "%s (budget %.0f MB, used %.0f MB, RAM free %.0f MB, commit room %.0f MB)", why, f.budgetMB, f.usedMB, f.ramFreeMB, f.commitRoomMB);
        memcpy(s_why, buf, sizeof s_why);
        g_lastWhy = s_why;
        return;
    }
    g_state = 1;
    g_pass.device = device;
    g_pass.adapter = ad;
    g_pass.requested = g_requested.load();
    HANDLE th = CreateThread(nullptr, 0, &PassThread, nullptr, 0, nullptr);
    if (!th) {
        Log("[cmdmem] pre-grow off: no thread: nothing done");
        return;
    }
    DescribeThread(th, L"OPF-CmdMemPregrow");
    CloseHandle(th);
}

// allocator_trim.cpp, AllocatorTrimTick (monitor thread, once a second: the thread of the trim's own pass)
void CmdMemGiveBackTick(void* device)
{
    if (g_passGrown.load() <= 0 || !device) return;
    IDXGIAdapter3* const ad = GameAdapter();
    if (!ad) return;
    const ULONGLONG now = GetTickCount64();
    if (g_giveBack.done) {
        if (g_giveBackAtMs && now - g_giveBackAtMs >= 5000) {  // the driver's figure once it has settled
            g_giveBackAtMs = 0;
            Facts f;
            Sample(ad, f, nullptr);
            int64_t real = 0, inFlight = 0, peak = 0;
            CmdMemDirectStats(&real, &inFlight, &peak);
            Log("[cmdmem] give-back: 5 s later: video memory %.0f MB of a budget of %.0f MB; direct pool %lld real, %lld in flight, session peak %lld", f.usedMB, f.budgetMB, static_cast<long long>(real), static_cast<long long>(inFlight), static_cast<long long>(peak));
        }
        return;
    }
    Facts f;
    double nl0 = 0;
    Sample(ad, f, &nl0);
    GiveBackFacts g;
    g.armed = true;
    g.trimFailed = AllocatorTrimFailed();
    g.engineLive = EngineActive();
    g.deviceOld = CmdMemDeviceAgeMs() >= 30000;
    g.budgetMB = f.budgetMB;
    g.usedMB = f.usedMB;
    g.headroomMB = g_giveBackMB.load();
    if (!GiveBackStep(g_giveBack, g)) {
        if (g_giveBack.done) Log("[cmdmem] give-back off: an exception in a trim pass: the pool stays as it is");
        return;
    }
    int64_t real = 0, inFlight = 0, peak = 0;
    CmdMemDirectStats(&real, &inFlight, &peak);
    const GiveBackPlan plan = PlanGiveBack(real, inFlight, peak);
    if (!plan.surplus) {
        Log("[cmdmem] give-back: video memory within %d MB of the budget for %d s (used %.0f of %.0f MB): nothing to give back: direct pool %lld real, session peak %lld in flight, keeping %lld; done for this session",
            g.headroomMB, kGiveBackSeconds, f.usedMB, f.budgetMB, static_cast<long long>(real), static_cast<long long>(peak), static_cast<long long>(plan.keepTotal));
        return;
    }
    // Before the pass, not only after it: the releases take about a second, and a line saying it began is what a crash in them would leave.
    Log("[cmdmem] give-back: starting: video memory %.0f of %.0f MB, direct pool %lld real, session peak %lld in flight, %lld now, keeping %lld", f.usedMB, f.budgetMB, static_cast<long long>(real), static_cast<long long>(peak), static_cast<long long>(inFlight), static_cast<long long>(plan.keepTotal));
    const double t0 = NowMs();
    CmdMemTrimOut out{};
    const int ok = CmdMemTrimDirect(device, plan.keepFree, static_cast<int>(plan.surplus > kMax ? kMax : plan.surplus), &out);
    const double ms = NowMs() - t0;
    if (ok < 0) {
        Log("[cmdmem] give-back: an exception in the trim's pass: it ends here; done for this session");
        return;
    }
    Facts f1;
    double nl1 = 0;
    Sample(ad, f1, &nl1);
    g_giveBackAtMs = now;
    Log("[cmdmem] give-back: video memory within %d MB of the budget for %d s: direct pool %lld -> %lld real (session peak %lld in flight, %lld now, keeping %lld), %d of %d free ones released, %.1f ms in the driver (longest %.1f ms), %.0f ms in all (qpc %.1f ms on); video memory %.0f -> %.0f MB of %.0f MB, non-local %.0f -> %.0f MB, RAM free %.0f -> %.0f MB; once per session: what is needed later is made as in the stock game",
        g.headroomMB, kGiveBackSeconds, static_cast<long long>(real), static_cast<long long>(real - out.swapped), static_cast<long long>(peak), static_cast<long long>(inFlight), static_cast<long long>(plan.keepTotal), out.swapped, out.freeReal, out.releaseMs, out.longestOneMs, ms, t0, f.usedMB, f1.usedMB, f.budgetMB, nl0, nl1, f.ramFreeMB, f1.ramFreeMB);
}

void SetCmdMemPregrow(int requested, bool poolsFound, bool beforeGameCode, int giveBackHeadroomMB, const wchar_t* iniPath)
{
    (void)iniPath;
    if (requested <= 0) return;
    if (!beforeGameCode) {
        // The size counts the allocators made since the hooks went in, the world test the fades since then: both would be wrong with a device or a world that exists already.
        Log("[cmdmem] pre-grow off: not installed on the OBSE64 route (it must start before the game's code, as the version.dll route does): nothing done");
        return;
    }
    if (!poolsFound) {
        Log("[cmdmem] pre-grow off: the engine's allocator pools were not found (other game version?): nothing done");
        return;
    }
    g_giveBackMB.store(giveBackHeadroomMB);
    g_requested.store(requested);
    if (requested == 1) Log("[cmdmem] pre-grow armed (automatic size, up to %d direct allocators; AMD with its own video memory and parallel translate; once, before the first world)", kAutoN);
    else Log("[cmdmem] pre-grow armed (up to %d direct allocators; AMD with its own video memory and parallel translate; once, before the first world)", requested);
}

// ---- Self-test: a fake pool ----
namespace {
struct FakeW {
    int id = 0;
    int where = 0;  // 0 pool, 1 held here, 2 with the engine, 3 kept off (Reset failed)
    int lists = 0, pending = 0, returned = 0, resets = 0;
    bool bound = false;
};
struct FakePool {
    static constexpr int kN = 400;
    FakeW w[kN];
    int stack[kN], top = 0, made = 0;
    int nullAfter = 1 << 30;
    int recordFailAt = -1, resetFailAt = -1, records = 0, resetsSeen = 0;
    int stopAtCheck = -1, stopReason = 0, checks = 0, engineEvery = 0, engineOps = 0;
    bool quiet = false;
    int boundId = -1;
    const char* fail = nullptr;
    int listsSinceReset = 0, maxListsSinceReset = 0, engineHeld[64] = {}, engineN = 0;
    void Bad(const char* why) { if (!fail) fail = why; }
    void EngineStep()
    {
        // The engine obtains and returns while the pass holds wrappers.
        if (!engineEvery || ++engineOps % engineEvery) return;
        if (engineN < 8) {
            int id;
            if (top) id = stack[--top];
            else { id = made++; }
            w[id].where = 2;
            engineHeld[engineN++] = id;
        } else {
            while (engineN) {
                const int id = engineHeld[--engineN];
                w[id].where = 0;
                stack[top++] = id;
            }
        }
    }
};
void* FPop(void* c)
{
    auto& p = *static_cast<FakePool*>(c);
    p.EngineStep();
    if (p.quiet) p.Bad("the pool was touched after the engine went quiet");
    if (!p.top) return nullptr;
    FakeW& w = p.w[p.stack[--p.top]];
    w.where = 1;
    return &w;
}
void* FObtain(void* c)
{
    auto& p = *static_cast<FakePool*>(c);
    if (p.quiet) p.Bad("obtain after the engine went quiet");
    if (p.made >= p.nullAfter) return nullptr;
    if (p.top) return FPop(c);
    FakeW& w = p.w[p.made++];
    w.where = 1;
    return &w;
}
void FRelease(void* c, void* v)
{
    auto& p = *static_cast<FakePool*>(c);
    auto& w = *static_cast<FakeW*>(v);
    p.EngineStep();
    if (p.quiet) p.Bad("a wrapper went back after the engine went quiet");
    if (w.where != 1) p.Bad("a wrapper went back that was not held here");
    if (w.bound) p.Bad("a wrapper went back with the private list still on it");
    w.where = 0;
    ++w.returned;
    p.stack[p.top++] = w.id;
}
int64_t FCreated(void* c) { return static_cast<FakePool*>(c)->made; }
HRESULT FRecord(void* c, void* v, int lists, int* done)
{
    auto& p = *static_cast<FakePool*>(c);
    auto& w = *static_cast<FakeW*>(v);
    p.EngineStep();
    if (w.where != 1) p.Bad("recorded on a wrapper not held here");
    if (p.recordFailAt == p.records++) return E_FAIL;
    if (p.boundId >= 0) p.w[p.boundId].bound = false;
    w.bound = true;
    p.boundId = w.id;
    w.lists += lists;
    w.pending += lists;
    *done += lists;
    p.listsSinceReset += lists;
    if (p.listsSinceReset > p.maxListsSinceReset) p.maxListsSinceReset = p.listsSinceReset;
    return S_OK;
}
HRESULT FUnbind(void* c)
{
    auto& p = *static_cast<FakePool*>(c);
    if (p.boundId >= 0) p.w[p.boundId].bound = false;
    p.boundId = -1;
    return S_OK;
}
HRESULT FResetAlloc(void* c, void* v)
{
    auto& p = *static_cast<FakePool*>(c);
    auto& w = *static_cast<FakeW*>(v);
    if (w.where != 1) p.Bad("Reset on a wrapper not held here");
    if (w.bound) p.Bad("Reset with the private list still on it");
    if (p.resetFailAt == p.resetsSeen++) { w.where = 3; return E_FAIL; }
    ++w.resets;
    p.listsSinceReset -= w.pending;
    w.pending = 0;
    return S_OK;
}
int FCheck(void* c)
{
    auto& p = *static_cast<FakePool*>(c);
    if (p.stopAtCheck == p.checks++) {
        if (p.stopReason == kStopQuit) p.quiet = true;
        return p.stopReason;
    }
    return kGoOn;
}
bool FMayTouch(void* c) { return !static_cast<FakePool*>(c)->quiet; }
void FPace(void*) {}
const Ops kFakeOps(FakePool& p)
{
    return Ops{&p, &FPop, &FObtain, &FRelease, &FCreated, &FRecord, &FUnbind, &FResetAlloc, &FCheck, &FMayTouch, &FPace};
}
void FakeInit(FakePool& p, int inPool)
{
    p = FakePool{};
    for (int i = 0; i < FakePool::kN; ++i) p.w[i].id = i;
    for (int i = 0; i < inPool; ++i) p.stack[p.top++] = p.made++;
}
// After a pass: every wrapper in exactly one place, none held here (unless the pass stopped holding), each reset allocator back.
const char* FakeBooks(const FakePool& p, int abandoned, int lost)
{
    int inPool = 0, heldHere = 0, kept = 0;
    bool seen[FakePool::kN] = {};
    for (int i = 0; i < p.top; ++i) {
        if (seen[p.stack[i]]) return "a wrapper is in the pool twice";
        seen[p.stack[i]] = true;
    }
    for (int i = 0; i < p.made; ++i) {
        const FakeW& w = p.w[i];
        if (w.where == 0) { ++inPool; if (!seen[i]) return "a wrapper marked in the pool is not"; }
        if (w.where == 1) ++heldHere;
        if (w.where == 3) ++kept;
        if (w.bound) return "the private list is still on a wrapper";
    }
    if (inPool != p.top) return "pool count wrong";
    if (heldHere != abandoned) return "wrappers still held here";
    if (kept != lost) return "failed-Reset wrappers not kept off the pool";
    return nullptr;
}
}  // namespace

const char* CmdPregrowSelfTest()
{
    // Arithmetic
    if (ListsFor(1) != 1 || ListsFor(2) != 17 || ListsFor(3) != 33) return "list count per group";
    if (BatchFor(374, 2) != 22 || BatchFor(0, 2) != 1 || BatchFor(100000, 1) != kMax) return "batch size";
    if (BatchFor(0x7FFFFFFF, 2) != kMax) return "batch size near the integer limit";
    // Gates
    Facts ok;
    ok.amd = ok.dedicated = ok.translate = ok.fadeHook = ok.engineLive = true;
    ok.budgetMB = 15400; ok.usedMB = 2000; ok.ramFreeMB = 20000; ok.commitRoomMB = 40000;
    if (FixedGate(ok) || OpeningGate(ok)) return "gates closed on a good PC";
    Facts f = ok; f.amd = false; if (!FixedGate(f)) return "another vendor passes";
    f = ok; f.dedicated = false; if (!FixedGate(f)) return "shared-memory GPU passes";
    f = ok; f.wine = true; if (!FixedGate(f)) return "Wine passes";
    f = ok; f.fadeHook = false; if (!FixedGate(f)) return "no fade hook passes";
    f = ok; f.engineLive = false; if (!OpeningGate(f)) return "the start passes with the engine quiet";
    f = ok; f.translate = false; if (!OpeningGate(f)) return "translate off passes";
    f = ok; f.budgetMB = kFloorBudgetMB - 1; if (!OpeningGate(f)) return "low budget passes";
    f = ok; f.usedMB = f.budgetMB - kFloorVramHeadMB + 1; if (!OpeningGate(f)) return "low video memory headroom passes";
    f = ok; f.ramFreeMB = kFloorRamMB - 1; if (!OpeningGate(f)) return "low RAM passes";
    f = ok; f.commitRoomMB = kFloorCommitMB - 1; if (!OpeningGate(f)) return "low commit passes";
    if (SizeFor(ok, 1, 1.9, 0.0) != kAutoN) return "automatic size on a good PC";
    if (SizeFor(ok, 100, 1.9, 0.0) != 100) return "asked size on a good PC";
    if (SizeFor(ok, 100000, 0.0, 0.0) != kMax) return "size cap";
    f = ok; f.budgetMB = kFloorBudgetMB; if (SizeFor(f, 1, 1.9, 0.0) != static_cast<int>(kFloorSize)) return "size at the budget floor";
    f = ok; f.budgetMB = (kFloorBudgetMB + kFullBudgetMB) / 2; if (SizeFor(f, 1, 1.9, 0.0) != static_cast<int>((kFloorSize + kAutoN) / 2)) return "size between floor and full";
    f = ok; f.ramFreeMB = kFloorRamMB + 100 * 2.3; if (SizeFor(f, 1, 0.3, 2.3) != 100) return "size scaled by RAM when the chunks sit there";
    f = ok; f.usedMB = f.budgetMB - kFloorVramHeadMB - kAssumeMB * 100; if (SizeFor(f, 1, 1.9, 0.0) != 100) return "size scaled by video memory";
    f = ok; f.usedMB = f.budgetMB - kFloorVramHeadMB - kAssumeMB * 10; if (SizeFor(f, 1, 1.9, 0.0) != 0) return "a useless size";
    if (!MemoryOk(ok)) return "memory check stops a good PC";
    f = ok; f.usedMB = f.budgetMB - kStopVramHeadMB + 1; if (MemoryOk(f)) return "memory check misses the card filling up";
    f = ok; f.ramFreeMB = kStopRamMB - 1; if (MemoryOk(f)) return "memory check misses RAM";
    if (CostGate(0.3, 2.0) || !CostGate(0.3, kCostMaxMB) || !CostGate(0.0, 0.01)) return "cost limits";

    // The give-back's decisions, as seconds of a session.
    {
        GiveBackFacts full;
        full.armed = full.engineLive = full.deviceOld = true;
        full.budgetMB = 12000; full.usedMB = 11900; full.headroomMB = 512;
        GiveBackFacts roomy = full;
        roomy.usedMB = 11000;
        const auto seconds = [](GiveBackState& s, const GiveBackFacts& g, int n) { int fired = 0; for (int i = 0; i < n; ++i) fired += GiveBackStep(s, g) ? 1 : 0; return fired; };
        GiveBackState s;
        // No pass grew anything: never, however full, and the pressure before arming doesn't count.
        GiveBackFacts g = full; g.armed = false;
        if (seconds(s, g, 100)) return "give-back without a pass";
        if (s.done || s.under) return "give-back: pressure before arming counted";
        // A blip does not fire: 2 s under, 1 s over the line, 2 s under.
        if (seconds(s, full, 2) || seconds(s, roomy, 1) || seconds(s, full, 2)) return "give-back fired on a blip";
        // The third second in a row does.
        if (seconds(s, full, 1) != 1 || !s.done) return "give-back did not fire after 3 s under the line";
        // Latched: further pressure, a later regrowth with the card full or not: never again.
        if (seconds(s, full, 1000) || seconds(s, roomy, 10) || seconds(s, full, 1000)) return "give-back ran twice";
        // The engine quiet (a quit, a minimised game) or a young device: it waits, unlatched, and goes as soon as both are fine.
        s = GiveBackState{};
        g = full; g.engineLive = false;
        if (seconds(s, g, 20) || s.done) return "give-back ran with the engine quiet";
        g = full; g.deviceOld = false;
        if (seconds(s, g, 20) || s.done) return "give-back ran on a young device";
        if (seconds(s, full, 1) != 1) return "give-back did not go when the engine came back";
        // An exception in a trim pass: never, latched. Key at 0: never. An unknown budget is not a full card.
        s = GiveBackState{};
        g = full; g.trimFailed = true;
        if (seconds(s, g, 10) || !s.done) return "give-back after an exception in the trim";
        s = GiveBackState{};
        g = full; g.headroomMB = 0;
        if (seconds(s, g, 10) || s.done) return "give-back with the key at 0";
        g = full; g.budgetMB = 0;
        if (seconds(s, g, 10) || s.done) return "give-back on an unknown budget";
        // What it keeps.
        GiveBackPlan pl = PlanGiveBack(300, 40, 120);
        if (pl.keepTotal != 136 || pl.keepFree != 96 || pl.surplus != 164) return "give-back keeps the wrong number";
        pl = PlanGiveBack(300, 64, 264);  // the session has really used them: 297 kept, 3 is not worth it
        if (pl.surplus != 0) return "give-back of a tiny surplus";
        pl = PlanGiveBack(300, 0, 400);
        if (pl.surplus != 0) return "give-back under the session's peak";
        pl = PlanGiveBack(300, 10, 20);  // a session that has hardly started keeps the floor
        if (pl.keepTotal != kKeepFloor || pl.keepFree != kKeepFloor - 10 || pl.surplus != 300 - kKeepFloor) return "give-back under the floor";
        pl = PlanGiveBack(30, 50, 60);   // a racing read: more out than exist
        if (pl.surplus != 0 || pl.keepFree < 0) return "give-back on a racing read";
    }

    static FakePool p;
    int abandoned;
    const char* why;
    // Normal: the pool grows to the target, every wrapper recorded once, back once, reserve last; never over the DRED rule.
    for (int inPool : {0, 30, 100, 250}) {
        FakeInit(p, inPool);
        PlanResult r;
        const Ops o = kFakeOps(p);
        RunPlan(o, 260, 17, 22, r);
        if (p.fail) return p.fail;
        if ((why = FakeBooks(p, 0, 0)) != nullptr) return why;
        if (r.stop || r.grown != 260 || p.made != 260 || r.lists != 260 * 17) return "normal pass: wrong counts";
        for (int i = 0; i < p.made; ++i) if (p.w[i].lists != 17) return "normal pass: a wrapper not grown exactly once";
        if (p.maxListsSinceReset > 22 * 17) return "lists over the DRED batch";
    }
    // Pool already over the target: the extra ones are not grown, not lost.
    FakeInit(p, 300);
    {
        PlanResult r;
        const Ops o = kFakeOps(p);
        RunPlan(o, 260, 17, 22, r);
        if (p.fail) return p.fail;
        if ((why = FakeBooks(p, 0, 0)) != nullptr) return why;
        if (r.grown != 260 || p.top != 300) return "pool over the target";
    }
    // Small target: reserve scales.
    FakeInit(p, 10);
    {
        PlanResult r;
        const Ops o = kFakeOps(p);
        RunPlan(o, 40, 17, 22, r);
        if (p.fail) return p.fail;
        if ((why = FakeBooks(p, 0, 0)) != nullptr) return why;
        if (r.grown != 40) return "small target";
    }
    // The engine obtains and returns all the while: nothing lost, no wrapper in two places, growth stops at the target.
    FakeInit(p, 100);
    {
        p.engineEvery = 7;
        PlanResult r;
        const Ops o = kFakeOps(p);
        RunPlan(o, 260, 17, 22, r);
        if (p.fail) return p.fail;
        if (r.grown != 260) return "engine running: growth not at the target";
        while (p.engineN) { const int id = p.engineHeld[--p.engineN]; p.w[id].where = 0; p.stack[p.top++] = id; }
        if ((why = FakeBooks(p, 0, 0)) != nullptr) return why;
    }
    // A record fails mid-batch: the rest back, stop, no more growth.
    FakeInit(p, 100);
    {
        p.recordFailAt = 30;
        PlanResult r;
        const Ops o = kFakeOps(p);
        RunPlan(o, 260, 17, 22, r);
        if (p.fail) return p.fail;
        if (r.stop != kStopFailed || r.hr != E_FAIL || r.grown != 30) return "a failed record does not stop the pass";
        if ((why = FakeBooks(p, 0, 0)) != nullptr) return why;
    }
    // An allocator's Reset fails: that wrapper stays off the pool (the engine's release would make it a fatal), the rest back.
    FakeInit(p, 100);
    {
        p.resetFailAt = 25;
        PlanResult r;
        const Ops o = kFakeOps(p);
        RunPlan(o, 260, 17, 22, r);
        if (p.fail) return p.fail;
        if (r.stop != kStopFailed || r.lost != 1) return "a failed Reset does not stop the pass";
        if ((why = FakeBooks(p, 0, 1)) != nullptr) return why;
    }
    // The memory floor or the world, at each possible check: everything back, grown ones on top.
    for (const int reason : {static_cast<int>(kStopMemory), static_cast<int>(kStopWorld)})
        for (int at = 0; at < 14; ++at) {
            FakeInit(p, 100);
            p.stopAtCheck = at;
            p.stopReason = reason;
            PlanResult r;
            const Ops o = kFakeOps(p);
            RunPlan(o, 260, 17, 22, r);
            if (p.fail) return p.fail;
            if (at == 0 && r.grown != 0) return "stopped at the first check, still grew";
            if ((why = FakeBooks(p, 0, 0)) != nullptr) return why;
        }
    // The engine goes quiet (a quit) at each possible check: the pool is never touched again, held wrappers stay here.
    for (int at = 0; at < 14; ++at) {
        FakeInit(p, 100);
        p.stopAtCheck = at;
        p.stopReason = kStopQuit;
        PlanResult r;
        const Ops o = kFakeOps(p);
        RunPlan(o, 260, 17, 22, r);
        if (p.fail) return p.fail;
        abandoned = r.abandoned;
        if ((why = FakeBooks(p, abandoned, 0)) != nullptr) return why;
    }
    // The engine's obtain returns nothing: no crash, what there is gets grown.
    FakeInit(p, 50);
    {
        p.nullAfter = 60;
        PlanResult r;
        const Ops o = kFakeOps(p);
        RunPlan(o, 260, 17, 22, r);
        if (p.fail) return p.fail;
        if (!r.obtainNull || p.made != 60 || r.grown != 60) return "null obtain";
        if ((why = FakeBooks(p, 0, 0)) != nullptr) return why;
    }
    // Stopping without a pool: nothing to do, no crash.
    FakeInit(p, 0);
    {
        p.nullAfter = 0;
        PlanResult r;
        const Ops o = kFakeOps(p);
        RunPlan(o, 260, 17, 22, r);
        if (p.fail) return p.fail;
        if (r.grown != 0) return "no pool and no obtain";
    }
    return nullptr;
}

}  // namespace opf
