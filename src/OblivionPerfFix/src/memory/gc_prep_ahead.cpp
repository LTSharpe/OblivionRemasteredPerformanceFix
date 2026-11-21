// GcPrepAhead (worker gcprepbuild, 2026-11-19, S-041; design gcprepgrd): the per-level preparation that the engine's
// garbage-collection pass does first for every streamed-out level is done for the oldest pending levels in calm frames of
// the last second before the pass is due, one or two a frame, with the engine's own function; the pass then prepares only
// the rest.
//
// The engine side (UE 5.3, Engine/Private/LevelStreamingGCHelper.cpp; the same in 5.4-5.7 and ue5-main): a level removed
// from the world is put in FLevelStreamingGCHelper::LevelsPendingUnload (a TArray of TWeakObjectPtr<ULevel>, insertion
// order) by RequestUnload, and taken out again by CancelUnloadRequest when it is wanted back before the next pass (the
// engine then reuses it: LevelStreaming.ShouldReuseUnloadedButStillAroundLevels, 1 by default). The pass's pre-GC
// delegate (OnPreGarbageCollect) calls PrepareStreamedOutLevelForGC for every entry (++NumberOfPrepared..., the streaming
// objects' unloaded event, ULevel::CleanupLevel, every object of the level's packages marked garbage, the packages renamed
// `_PreparedForGC`), then empties the list and zeroes NumberOfPreparedStreamedOutLevelsForGC. Nothing is freed there: the
// pass's purge frees. With the cvar at 0, RequestUnload itself calls PrepareStreamedOutLevelForGC at once, outside any
// world tick, and does not list the level: the state "prepared, waiting for the next pass, counted in NumberOfPrepared"
// is the engine's own (the branch is compiled into this exe: RequestUnload's tail jump, checked by the search below).
//
// What we do: inside ConditionalCollectGarbage's timer path (GcPassAfterBusy's hook: one call per frame, after
// UWorld::Tick has set bInTick = false, which is where the pass itself runs), when the pass is predicted within the
// window (timer: TimeSinceLastPendingKillPurge + window > interval; World Partition's soft pass at N pending levels: at
// most maxPerFrame levels short of N and that many arrived within the window, paused for a cycle after a late one) and
// the frame is calm, call the game's PrepareStreamedOutLevelForGC for the oldest entries pending at least the minimum
// age, each while holding the engine's GC lock taken without blocking (TryGCLock, as the pass holds it: gcprepfix,
// review S2), then remove that entry from the list by value (order kept, as the engine's own RemoveAll). NumberOfPrepared was incremented by the call, so
// GetNumLevelsPendingPurge (= Num + Prepared) does not change: the soft and low-memory triggers see what they would see.
// The pass's own loop prepares what is left. The result is the state the engine produces itself with the cvar at 0 for
// those levels; the only difference a player could meet is a level prepared early and wanted again before the pass: the
// engine then builds it anew (a stream-in) instead of reusing it. The window keeps that rare (counted: "wanted again").
//
// Gates, all mirrored from the engine where it has them: not in a frame where the pass is due (it prepares the rest),
// no incremental purge pending, no async loading (the pass's own gate), no world in tick (RequestUnload's own test, read
// the same way), no menu / dialogue / loading screen (the fade hook's freeze reader), not low memory (GIsLowMemory: the
// low-memory soft pass has its own count), a known frame state that is not already long (NpcCreationPacing: < 12 ms of
// the frame gone at the call), not busy (GcPassAfterBusy's signals: a cell added / removed this or the last frame, >= 1 ms
// of character steps run in this frame; unknown = busy), the GC lock free (no other thread inside an FGCScopeGuard: the
// OblivionThread takes one in Altar's bridge functions), the entry pending >= the minimum age (3 s: clear of BodyRemovalSpread's, the
// EndPlay slice's and CellEvictSplit's per-level queues). At most 2 levels or 500 us a frame. Nothing new is kept but
// first-seen stamps keyed by the weak pointer's value (index + serial number, never dereferenced) and, for the
// "wanted again" counter, the package name read from a level just before it is prepared.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace opf {
bool NpcPacingFrameState(uint64_t* elapsedUs, uint64_t* unitUs);  // npcs/npc_pacing.cpp

namespace {

using GetFn = void* (*)(const void* weakPtr);  // FWeakObjectPtr::Get (null for a stale or garbage object)
using PrepFn = void (*)(void* level);          // FLevelStreamingGCHelper::PrepareStreamedOutLevelForGC

// FLevelStreamingGCHelper::OnPreGarbageCollect with PrepareStreamedOutLevelsForGC inlined (exe+0x33261D0 on 19115871):
// mov ecx,[Prepared]; mov edx,[Num]; add; test; jle; lea rcx,[OnGCStreamedOutLevels]; call Broadcast; mov edx,[Num];
// xor ebx,ebx; test; jle; loop { mov rax,[Data]; movsxd; lea rcx,[rax+rcx*8]; call Get; mov rcx,rax; call Prepare;
// inc ebx; cmp ebx,[Num]; jl }; cmp [Max],0; mov [Num],0 (Empty)
constexpr char kOnPreGc[] =
    "40 53 48 83 EC 20 8B 0D ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 03 CA 85 C9 7E ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "8B 15 ?? ?? ?? ?? 33 DB 85 D2 7E ?? 48 8B 05 ?? ?? ?? ?? 48 63 CB 48 8D 0C C8 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? "
    "FF C3 3B 1D ?? ?? ?? ?? 7C ?? 83 3D ?? ?? ?? ?? 00 C7 05 ?? ?? ?? ?? 00 00 00 00";
// (instruction start, displacement offset, instruction length) of each global read in kOnPreGc
constexpr size_t kPgPrepared[3] = {0x06, 0x08, 6}, kPgNum1[3] = {0x0C, 0x0E, 6}, kPgNum2[3] = {0x24, 0x26, 6},
                 kPgData[3] = {0x30, 0x33, 7}, kPgNum3[3] = {0x4D, 0x4F, 6}, kPgMax[3] = {0x55, 0x57, 7},
                 kPgNum4[3] = {0x5C, 0x5E, 10};
constexpr size_t kPgGetCall = 0x3E, kPgPrepCall = 0x46;
// PrepareStreamedOutLevelForGC (exe+0x3326890): test rcx,rcx; je; mov r11,rsp; push rbp; push r13; lea rbp,[r11-78h];
// sub rsp,168h; (stack cookie); inc dword [NumberOfPreparedStreamedOutLevelsForGC]
constexpr char kPrepOne[] =
    "48 85 C9 0F 84 ?? ?? ?? ?? 4C 8B DC 55 41 55 49 8D 6B 88 48 81 EC 68 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 40 FF 05";
constexpr size_t kPoPrepared[3] = {0x28, 0x2A, 6};
// FWeakObjectPtr::Get (exe+0x1126290): SerialNumber, ObjectIndex >= 0, < GUObjectArray count, ...
constexpr char kGet[] = "44 8B 49 04 45 85 C9 74 ?? 8B 01 85 C0 78 ?? 3B 05";
// GetNumLevelsPendingPurge (exe+0x3322470): mov eax,[Num]; add eax,[Prepared]; ret
constexpr char kGetNum[] = "8B 05 ?? ?? ?? ?? 03 05 ?? ?? ?? ?? C3";
// RequestUnload's reuse-0 branch (exe+0x332AB4B): cmp byte [ShouldReuse...],0; jne list; mov rcx,[GEngine];
// mov rax,[rcx+WorldList]; movsxd rcx,[rcx+WorldList.Num]; ... loop { mov rdx,[rax]; mov r8,[rdx+ThisCurrentWorld];
// test; je; test byte [r8+bInTick byte],mask; jne list }; ...; jmp PrepareStreamedOutLevelForGC
constexpr char kInTick[] =
    "80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 81 ?? ?? ?? ?? 48 63 89 ?? ?? ?? ?? 48 8D 0C C8 "
    "48 3B C1 74 ?? 66 66 0F 1F 84 00 00 00 00 00 48 8B 10 4C 8B 82 ?? ?? ?? ?? 4D 85 C0 74 ?? 41 F6 80 ?? ?? ?? ?? ?? "
    "75 ?? 48 83 C0 08 48 3B C1 75 ?? 48 8B CB 48 8B 74 24 ?? 48 8B 7C 24 ?? 48 83 C4 20 5B E9";
constexpr size_t kItEngine[3] = {0x0D, 0x10, 7}, kItWorldList = 0x17, kItWorldNum = 0x1E, kItCtxWorld = 0x3B, kItInTick = 0x47,
                 kItMask = 0x4B;
// UWorld::UpdateLevelStreaming's force block (exe+0x3874138): je; cmp eax,[LowMemoryPendingPurgeCount]; jge force;
// mov ecx,[ContinuouslyIncrementalGCWhileLevelsPendingPurge]; test; je; cmp eax,ecx; jl; mov rcx,[GEngine]; xor edx,edx;
// call ForceGarbageCollection. Before it (searched back): call GetNumLevelsPendingPurge; cmp byte [GIsLowMemory],0
constexpr char kSoft[] =
    "74 08 3B 05 ?? ?? ?? ?? 7D 0E 8B 0D ?? ?? ?? ?? 85 C9 74 12 3B C1 7C 0E 48 8B 0D ?? ?? ?? ?? 33 D2 E8";
constexpr size_t kSfLowCount[3] = {0x02, 0x04, 6}, kSfCount[3] = {0x0A, 0x0C, 6}, kSfEngine[3] = {0x18, 0x1B, 7};
// The engine's GC lock (UE 5.3 FGCCSyncObject, CoreUObject GCScopeLock.h / GarbageCollection.cpp). The stock pass prepares
// the levels while holding it (CollectGarbage / TryCollectGarbage take it before the pre-GC delegate), and non-game threads
// that touch UObjects take FGCScopeGuard (LockAsync), which waits while it is held; the game's OblivionThread does so in
// Altar's bridge functions. We take it without blocking around each early preparation, as TryCollectGarbage does.
// TryCollectGarbage's inlined TryGCLock (exe+0x106E48B): mov rsi,[GGCSingleton]; ...; lea rbx,[rsi+10h] (Critical);
// call [EnterCriticalSection]; xor r15d,r15d; xor eax,eax; lock cmpxchg [rsi],r15d (AsyncCounter == 0?); jne;
// mov rcx,[rsi+38h] (GCUnlockedEvent); mov rax,[rcx]; call [rax+18h] (Reset); lock inc [rsi+4] (GCCounter); ...;
// call [LeaveCriticalSection]. The field offsets are fixed bytes of the pattern.
constexpr char kTryGc[] =
    "48 8B 35 ?? ?? ?? ?? 48 89 7C 24 38 40 32 FF 4C 89 7C 24 30 48 8D 5E 10 48 8B CB FF 15 ?? ?? ?? ?? 45 33 FF 33 C0 "
    "F0 44 0F B1 3E 75 11 48 8B 4E 38 48 8B 01 FF 50 18 F0 FF 46 04 40 B7 01 48 8B 74 24 40 48 85 DB 74 09 48 8B CB FF 15";
constexpr size_t kTgSingleton[3] = {0x00, 0x03, 7}, kTgEnter[3] = {0x1B, 0x1D, 6}, kTgLeave[3] = {0x4B, 0x4D, 6};
// ReleaseGCLock (exe+0x106BC10): mov rbx,[GGCSingleton]; mov rcx,[rbx+38h]; mov rax,[rcx]; call [rax+10h] (Trigger);
// lock dec [rbx+4]; ret
constexpr char kRelGc[] = "40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 8B 4B 38 48 8B 01 FF 50 10 F0 FF 4B 04 48 83 C4 20 5B C3";
constexpr size_t kRgSingleton[3] = {0x06, 0x09, 7};
constexpr uint32_t kGcAsyncCounter = 0x00, kGcCounter = 0x04, kGcCritical = 0x10, kGcEvent = 0x38, kGcEventReset = 0x18;

struct Found {
    uint8_t *onPreGc, *prepOne, *get, *inTick, *soft, *tryGc, *relGc;
    int32_t* num;
    uint64_t** data;
    int32_t* prepared;
    const int32_t* softCount;
    const int32_t* lowCount;
    const uint8_t* lowMemory;
    void* const* engine;
    void* const* gcSingleton;  // GGCSingleton (FGCCSyncObject*)
    void* const* enterSlot;    // the game's import slots of EnterCriticalSection / LeaveCriticalSection
    void* const* leaveSlot;
    uint32_t worldList, worldNum, ctxWorld, inTickOff;
    uint8_t inTickMask;
};

// The real lock (game thread): TryGCLock as the engine inlines it, on the engine's own object; ReleaseGCLock is the game's
using CsFn = void(WINAPI*)(void*);
struct GcLock {
    void* const* singleton = nullptr;
    CsFn enter = nullptr, leave = nullptr;
    void (*release)() = nullptr;
} g_lock;

bool GameTryGcLock()
{
    if (!g_lock.singleton || !g_lock.enter || !g_lock.leave) return false;
    uint8_t* const s = static_cast<uint8_t*>(*g_lock.singleton);
    if (!s) return false;
    void* const cs = s + kGcCritical;
    g_lock.enter(cs);
    bool ok = false;
    // AsyncCounter == 0: no other thread inside an FGCScopeGuard. GCCounter == 0 as well (the engine's check(): not recursive)
    if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(s + kGcAsyncCounter), 0, 0) == 0 &&
        InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(s + kGcCounter), 0, 0) == 0) {
        void* const ev = *reinterpret_cast<void* const*>(s + kGcEvent);
        void* const* const vt = *static_cast<void* const* const*>(ev);
        reinterpret_cast<void (*)(void*)>(vt[kGcEventReset / sizeof(void*)])(ev);  // GCUnlockedEvent->Reset()
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(s + kGcCounter));
        ok = true;
    }
    g_lock.leave(cs);
    return ok;
}
void GameGcUnlock() { g_lock.release(); }

// What the step reads and calls (the game's, or the self-test's fakes)
struct Env {
    int32_t* num = nullptr;
    uint64_t** data = nullptr;
    int32_t* prepared = nullptr;
    GetFn get = nullptr;
    PrepFn prep = nullptr;
    const int32_t* softCount = nullptr;
    const uint8_t* lowMemory = nullptr;
    void* const* engine = nullptr;
    uint32_t worldList = 0, worldNum = 0, ctxWorld = 0, inTickOff = 0;
    uint8_t inTickMask = 0;
    uint64_t (*now)() = nullptr;                    // microseconds
    bool (*frame)(uint64_t*, uint64_t*) = nullptr;  // NpcPacingFrameState
    bool (*menu)(int32_t*, int32_t*) = nullptr;     // ReadMenuFreeze
    bool (*tryLock)() = nullptr;                    // the engine's GC lock, without blocking (GameTryGcLock)
    void (*unlock)() = nullptr;                     // ReleaseGCLock
};
Env g_env;

// Refusals, counted once per frame inside a window (the first gate that fails)
enum Why { kDue, kPurge, kMenu, kLowMem, kAsync, kInTickW, kFrameUnknown, kLong, kBusy, kYoung, kGcLockHeld, kWhyN };
const char* const kWhyName[kWhyN] = {"pass due", "purge", "menu/loading", "low memory", "async loading", "world in tick",
                                     "frame unknown", "frame >= 12 ms", "busy", "none old enough", "GC lock held"};
// A pass that came more than this long after its first early preparation is "late" (the prepared levels waited)
constexpr uint64_t kLateUs = 2000000;
// The frame-time rule: the frame's own time at the call plus the preparation must stay well inside a capped 60 fps
// frame with room for what still follows in it (about 2 ms): 12 ms gone at the call is the limit.
constexpr uint64_t kFrameLimitUs = 12000;

struct Params {
    int on = 0, minAgeMs = 3000, maxPerFrame = 2, budgetUs = 500, windowMs = 1000;
};
std::atomic<int> g_on{0}, g_minAgeMs{3000}, g_maxPerFrame{2}, g_budgetUs{500}, g_windowMs{1000};
bool g_installed = false;

// game thread state
struct Stamp {
    uint64_t key;      // the weak pointer's 8 bytes (ObjectIndex, SerialNumber)
    uint64_t firstUs;  // when we first saw it in the list
    bool nullSeen;
};
std::vector<Stamp> g_stamps, g_next;  // in list order
bool g_prevValid = false;
float g_prevTsl = 0.0f;
int32_t g_prevPrepared = 0, g_numAtDue = 0;
bool g_windowOpen = false;
// this cycle (since the last pass)
int g_cycLevels = 0, g_cycFrames = 0;
uint64_t g_cycUs = 0, g_cycMaxUs = 0, g_cycFirstUs = 0;
bool g_cycSoft = false;    // a level of this cycle was prepared in a soft-trigger window (the timer's window closed)
bool g_softPause = false;  // the last cycle's soft-window preparations waited too long: no soft window until the next pass
// "wanted again": package names of levels prepared early, by pass generation
struct PrepRec {
    uint64_t name;  // FName of the level's outermost package (ComparisonIndex, Number), 0 = free
    uint64_t gen;
};
constexpr int kRing = 256;
PrepRec g_ring[kRing];
int g_ringNext = 0, g_ringLive = 0;
uint64_t g_gen = 1, g_lastPassUs = 0;
const void* g_lastAdded = nullptr;
int g_passLogs = 0;
constexpr int kMaxPassLogs = 400;

struct Stats {
    std::atomic<uint64_t> levels{}, frames{}, us{}, maxUs{}, windows{}, passes{}, passesPrepped{}, left{}, late{}, budget{},
        nulls{}, gone{}, cancelOld{}, cancelYoung{}, wanted{}, wantedAfter{}, softWindows{}, lateSoft{}, softPaused{}, lockStops{};
    std::atomic<uint64_t> why[kWhyN]{};
} s;

uint64_t QpcUs()
{
    static const double perUs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1e6; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(static_cast<double>(t.QuadPart) / perUs);
}

void AtomicMax(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t c = a.load(std::memory_order_relaxed);
    while (v > c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}

Params Load()
{
    Params p;
    p.on = g_on.load(std::memory_order_relaxed);
    p.minAgeMs = g_minAgeMs.load(std::memory_order_relaxed);
    p.maxPerFrame = g_maxPerFrame.load(std::memory_order_relaxed);
    p.budgetUs = g_budgetUs.load(std::memory_order_relaxed);
    p.windowMs = g_windowMs.load(std::memory_order_relaxed);
    return p;
}

// The level's outermost package name (UObjectBase: NamePrivate +0x18, 8 bytes in a shipping build; OuterPrivate +0x20).
// Only for a live level (the one AddToWorld is adding, or one Get() returned just before we prepare it). 0 = none.
uint64_t PackageName(const void* level)
{
    const uint8_t* o = static_cast<const uint8_t*>(level);
    int hops = 0;
    for (; hops < 4; ++hops) {
        const uint8_t* outer = *reinterpret_cast<const uint8_t* const*>(o + 0x20);
        if (!outer) break;
        o = outer;
    }
    if (hops == 0 || hops == 4) return 0;
    uint64_t name;
    memcpy(&name, o + 0x18, 8);
    return name;
}

// RequestUnload's own test: is any world of the engine's world list inside its tick?
bool AnyWorldInTick()
{
    if (!g_env.engine) return true;
    const uint8_t* const engine = static_cast<const uint8_t*>(*g_env.engine);
    if (!engine) return true;
    const uint8_t* const* ctx = *reinterpret_cast<const uint8_t* const* const*>(engine + g_env.worldList);
    const int32_t n = *reinterpret_cast<const int32_t*>(engine + g_env.worldNum);
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t* const world = *reinterpret_cast<const uint8_t* const*>(ctx[i] + g_env.ctxWorld);
        if (world && (world[g_env.inTickOff] & g_env.inTickMask)) return true;
    }
    return false;
}

// The stamps follow the list (entries only leave it, new ones are appended at its end): walk both in order.
// Entries the game took back before the pass (CancelUnloadRequest: reuse) are counted by their age.
void Sync(uint64_t nowUs, uint64_t minAgeUs)
{
    const int32_t n = *g_env.num;
    const uint64_t* const d = *g_env.data;
    g_next.clear();
    size_t j = 0;
    for (int32_t i = 0; i < n; ++i) {
        const uint64_t key = d[i];
        size_t k = j;
        while (k < g_stamps.size() && g_stamps[k].key != key) ++k;
        if (k < g_stamps.size()) {
            for (size_t m = j; m < k; ++m)
                (nowUs - g_stamps[m].firstUs >= minAgeUs ? s.cancelOld : s.cancelYoung).fetch_add(1, std::memory_order_relaxed);
            g_next.push_back(g_stamps[k]);
            j = k + 1;
        } else {
            g_next.push_back(Stamp{key, nowUs, false});
        }
    }
    for (size_t m = j; m < g_stamps.size(); ++m)
        (nowUs - g_stamps[m].firstUs >= minAgeUs ? s.cancelOld : s.cancelYoung).fetch_add(1, std::memory_order_relaxed);
    g_stamps.swap(g_next);
}

// Remove one entry from the game's list by value, order kept (the engine's RemoveAll does the same); false = not there
bool RemoveByValue(uint64_t key)
{
    const int32_t n = *g_env.num;
    uint64_t* const d = *g_env.data;
    for (int32_t i = 0; i < n; ++i) {
        if (d[i] != key) continue;
        memmove(d + i, d + i + 1, static_cast<size_t>(n - i - 1) * sizeof(uint64_t));
        *g_env.num = n - 1;
        return true;
    }
    return false;
}

void Reset()
{
    g_stamps.clear();
    g_prevValid = false;
    g_windowOpen = false;
    g_cycLevels = g_cycFrames = 0;
    g_cycUs = g_cycMaxUs = g_cycFirstUs = 0;
    g_cycSoft = g_softPause = false;
}

void OnPass(uint64_t nowUs)
{
    s.passes.fetch_add(1, std::memory_order_relaxed);
    bool late = false;
    if (g_cycLevels > 0) {
        s.passesPrepped.fetch_add(1, std::memory_order_relaxed);
        s.left.fetch_add(static_cast<uint64_t>(std::max(0, g_numAtDue)), std::memory_order_relaxed);
        const uint64_t ahead = nowUs > g_cycFirstUs ? nowUs - g_cycFirstUs : 0;
        late = ahead > kLateUs;
        if (late) (g_cycSoft ? s.lateSoft : s.late).fetch_add(1, std::memory_order_relaxed);
        if (g_passLogs < kMaxPassLogs) {
            ++g_passLogs;
            Log("[gcprepahead] GC pass: %d levels prepared early in %d frames (%.2f ms, longest %.2f ms), %d left for the pass; "
                "first early preparation %.2f s before the pass%s%s",
                g_cycLevels, g_cycFrames, g_cycUs / 1000.0, g_cycMaxUs / 1000.0, g_numAtDue, ahead / 1e6,
                g_cycSoft ? " (soft-trigger window)" : "", late ? ": LATE (prepared levels waited > 2 s)" : "");
        }
    }
    // A late cycle with soft-window preparations: the prediction failed (the levels stopped coming), so the next cycle
    // opens no soft window (the timer's window still opens); the one after decides again
    g_softPause = late && g_cycSoft;
    if (g_softPause) s.softPaused.fetch_add(1, std::memory_order_relaxed);
    g_cycSoft = false;
    g_cycLevels = g_cycFrames = 0;
    g_cycUs = g_cycMaxUs = g_cycFirstUs = 0;
    g_numAtDue = 0;
    g_windowOpen = false;
    g_stamps.clear();  // the pass emptied the list (new entries of this frame are stamped from now)
    ++g_gen;
    g_lastPassUs = nowUs;
}

}  // namespace

// GcPassAfterBusy's hook calls the step only while this is true (on, or switched off with state still to drop). Game thread.
bool GcPrepAheadActive() { return g_installed && (g_on.load(std::memory_order_relaxed) != 0 || g_prevValid); }

// The step: GcPassAfterBusy's hook in ConditionalCollectGarbage's timer path calls this once per frame, after its own
// decision (the delay flag may be set by then) and before the engine's pass test. Game thread, no world in tick.
void GcPrepAheadFrame(const GcTimerFrame& f)
{
    if (!g_installed) return;
    const Params p = Load();
    if (!p.on) {
        if (g_prevValid || !g_stamps.empty()) Reset();
        return;
    }
    const uint64_t nowUs = g_env.now();
    const uint64_t minAgeUs = static_cast<uint64_t>(p.minAgeMs) * 1000, windowUs = static_cast<uint64_t>(p.windowMs) * 1000;
    const int32_t prepared = *g_env.prepared;
    // A pass ran since the last frame: it zeroes NumberOfPrepared, and resets the timer
    if (g_prevValid && (prepared < g_prevPrepared || (f.tsl < g_prevTsl && prepared == 0))) OnPass(nowUs);
    g_prevValid = true;
    g_prevTsl = f.tsl;
    Sync(nowUs, minAgeUs);
    struct End {  // the counts the next frame compares with
        ~End() { g_prevPrepared = *g_env.prepared; }
    } end;
    const int32_t num = static_cast<int32_t>(g_stamps.size());
    // the engine's pass test (the delay flag aside): the pass runs now, or is held for a frame; it prepares the rest itself
    const bool due = !f.purgePending && f.tsl > f.interval && f.interval > 0.0f;
    if (due) g_numAtDue = num;  // what the pass will find (read again every due frame until it runs)
    if (num == 0) { g_windowOpen = false; return; }
    // The window: the timer pass within windowMs, or World Partition's soft pass (UpdateLevelStreaming forces a pass when
    // Num + Prepared reaches the count). The soft pass has no clock, so its window is kept short: it opens only when the
    // pass is at most maxPerFrame levels away and that many levels arrived within windowMs (they are coming now), and not
    // in the cycle after one whose soft-window preparations waited > 2 s for their pass (the levels stopped coming).
    const bool timer = f.interval > 0.0f && static_cast<double>(f.tsl) + p.windowMs / 1000.0 > static_cast<double>(f.interval);
    bool soft = false;
    const int32_t count = *g_env.softCount;
    if (count >= 16 && !g_softPause) {
        const int32_t need = count - (num + prepared);
        soft = need <= 0 ||
               (need <= p.maxPerFrame && need <= num && nowUs - g_stamps[static_cast<size_t>(num - need)].firstUs <= windowUs);
    }
    if (!timer && !soft) { g_windowOpen = false; return; }
    if (!g_windowOpen) {
        g_windowOpen = true;
        s.windows.fetch_add(1, std::memory_order_relaxed);
        if (!timer) s.softWindows.fetch_add(1, std::memory_order_relaxed);
    }
    // The gates (the first that fails is counted)
    Why why = kWhyN;
    int32_t named = 0, mode = 0;
    uint64_t elapsed = 0, unitUs = 0;
    if (due) why = kDue;
    else if (f.purgePending) why = kPurge;
    else if (!g_env.menu || !g_env.menu(&named, &mode) || named != 0 || mode != 0) why = kMenu;
    else if (*g_env.lowMemory) why = kLowMem;
    else if (GcTimerAsyncLoading()) why = kAsync;
    else if (AnyWorldInTick()) why = kInTickW;
    else if (!g_env.frame || !g_env.frame(&elapsed, &unitUs)) why = kFrameUnknown;
    else if (elapsed >= kFrameLimitUs) why = kLong;
    else if (GcTimerBusy() & 7) why = kBusy;  // cell added / removed this or the last frame, >= 1 ms of character steps
    else if (nowUs - g_stamps[0].firstUs < minAgeUs) why = kYoung;
    if (why != kWhyN) {
        s.why[why].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Prepare the oldest entries, in list order, within the frame's budget
    uint64_t spent = 0;
    int done = 0;
    size_t i = 0;
    while (i < g_stamps.size() && done < p.maxPerFrame) {
        Stamp& st = g_stamps[i];
        if (nowUs - st.firstUs < minAgeUs) break;  // the rest are younger
        const uint64_t key = st.key;
        void* const level = g_env.get(&key);
        if (!level) {  // stale or already garbage: the pass's own loop gets it (and skips it)
            if (!st.nullSeen) { st.nullSeen = true; s.nulls.fetch_add(1, std::memory_order_relaxed); }
            ++i;
            continue;
        }
        const uint64_t name = PackageName(level);  // before the call renames the package
        // The engine's GC lock, as the stock pass holds it while it prepares: refused (another thread is inside an
        // FGCScopeGuard) = no preparation in this frame
        if (!g_env.tryLock || !g_env.tryLock()) {
            if (done == 0) s.why[kGcLockHeld].fetch_add(1, std::memory_order_relaxed);
            else s.lockStops.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        g_env.prep(level);
        QueryPerformanceCounter(&t1);
        g_env.unlock();
        static const double perUs = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return static_cast<double>(q.QuadPart) / 1e6; }();
        const uint64_t us = static_cast<uint64_t>(static_cast<double>(t1.QuadPart - t0.QuadPart) / perUs);
        // the call may have changed the list (the unloaded event's listeners): re-read it, remove by value
        if (!RemoveByValue(key)) s.gone.fetch_add(1, std::memory_order_relaxed);
        g_stamps.erase(g_stamps.begin() + static_cast<ptrdiff_t>(i));
        Sync(nowUs, minAgeUs);  // anything else the call changed
        if (name) {
            if (g_ring[g_ringNext].name == 0) ++g_ringLive;
            g_ring[g_ringNext] = PrepRec{name, g_gen};
            g_ringNext = (g_ringNext + 1) % kRing;
        }
        spent += us;
        ++done;
        if (!g_cycLevels) g_cycFirstUs = nowUs;
        ++g_cycLevels;
        if (!timer) g_cycSoft = true;
        g_cycUs += us;
        g_cycMaxUs = std::max(g_cycMaxUs, us);
        s.levels.fetch_add(1, std::memory_order_relaxed);
        s.us.fetch_add(us, std::memory_order_relaxed);
        AtomicMax(s.maxUs, us);
        if (spent >= static_cast<uint64_t>(p.budgetUs) || elapsed + spent >= kFrameLimitUs) {
            if (i < g_stamps.size() && done < p.maxPerFrame) s.budget.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    if (done) {
        ++g_cycFrames;
        s.frames.fetch_add(1, std::memory_order_relaxed);
    }
}

// StreamRegisterGranularity's AddToWorld hook (game thread): a level being added whose package was prepared early by us
// before its pass = a level the engine would have reused (counted; nothing else is done)
void GcPrepAheadNoteAdd(const void* level)
{
    if (!g_installed || g_ringLive == 0 || !level || level == g_lastAdded) return;
    g_lastAdded = level;
    const uint64_t name = PackageName(level);
    if (!name) return;
    const uint64_t nowUs = g_env.now();
    for (PrepRec& r : g_ring) {
        if (r.name != name) continue;
        if (r.gen == g_gen) s.wanted.fetch_add(1, std::memory_order_relaxed);
        else if (r.gen + 1 == g_gen && nowUs - g_lastPassUs <= 2000000) s.wantedAfter.fetch_add(1, std::memory_order_relaxed);
        else continue;
        r.name = 0;
        --g_ringLive;
        break;
    }
}

namespace {

bool Discover(const GameModule& game, Found* f, const char** why)
{
    const auto pg = FindAll(game.text, ParsePattern(kOnPreGc));
    const auto po = FindAll(game.text, ParsePattern(kPrepOne));
    const auto it = FindAll(game.text, ParsePattern(kInTick));
    const auto sf = FindAll(game.text, ParsePattern(kSoft));
    if (pg.size() != 1 || po.size() != 1) { *why = "the GC pass's level preparation (OnPreGarbageCollect / PrepareStreamedOutLevelForGC) not found once"; return false; }
    if (it.size() != 1) { *why = "RequestUnload's world-tick test not found once"; return false; }
    if (sf.size() != 1) { *why = "UpdateLevelStreaming's pending-level trigger not found once"; return false; }
    uint8_t* const g = pg[0];
    uint8_t* const p = po[0];
    auto at = [](uint8_t* base, const size_t (&r)[3]) { return RipTarget(base + r[0], r[1] - r[0], r[2]); };
    if (RipTarget(g + kPgPrepCall, 1, 5) != p) { *why = "OnPreGarbageCollect's loop does not call PrepareStreamedOutLevelForGC"; return false; }
    uint8_t* const get = RipTarget(g + kPgGetCall, 1, 5);
    if (!game.text.Contains(get) || !MatchesAt(get, ParsePattern(kGet))) { *why = "the loop's weak-pointer Get is not FWeakObjectPtr::Get"; return false; }
    uint8_t* const prepared = at(g, kPgPrepared);
    uint8_t* const num = at(g, kPgNum1);
    if (at(p, kPoPrepared) != prepared) { *why = "the two reads of NumberOfPreparedStreamedOutLevelsForGC differ"; return false; }
    if (at(g, kPgNum2) != num || at(g, kPgNum3) != num || at(g, kPgNum4) != num || at(g, kPgData) + 8 != num || at(g, kPgMax) != num + 4) {
        *why = "LevelsPendingUnload is not laid out as a TArray (Data, Num, Max)";
        return false;
    }
    if (!game.data.Contains(num) || !game.data.Contains(prepared)) { *why = "the pending-level list or count is not in .data"; return false; }
    // RequestUnload's reuse-0 branch: its tail jump goes to the same PrepareStreamedOutLevelForGC
    uint8_t* const t = it[0];
    const size_t itLen = ParsePattern(kInTick).size();
    if (RipTarget(t + itLen - 1, 1, 5) != p) { *why = "RequestUnload's world-tick test does not lead to PrepareStreamedOutLevelForGC"; return false; }
    uint8_t* const engine = at(t, kItEngine);
    uint32_t wl, wn, cw, ti;
    memcpy(&wl, t + kItWorldList, 4);
    memcpy(&wn, t + kItWorldNum, 4);
    memcpy(&cw, t + kItCtxWorld, 4);
    memcpy(&ti, t + kItInTick, 4);
    const uint8_t mask = t[kItMask];
    if (!game.data.Contains(engine) || wn != wl + 8 || wl > 0x4000 || cw > 0x1000 || ti > 0x1000 || !mask || (mask & (mask - 1))) {
        *why = "the engine's world list or a world's bInTick is not laid out as expected";
        return false;
    }
    // The pending-level trigger: GEngine as above; GetNumLevelsPendingPurge (Num + Prepared) called just before, then GIsLowMemory
    uint8_t* const sp = sf[0];
    if (at(sp, kSfEngine) != engine) { *why = "the pending-level trigger reads another GEngine"; return false; }
    uint8_t* lowMem = nullptr;
    for (size_t d = 5; d <= 0x40 && !lowMem; ++d) {
        uint8_t* const c = sp - d;
        if (c[0] != 0xE8 || c[5] != 0x80 || c[6] != 0x3D || c[11] != 0x00) continue;
        uint8_t* const fn = RipTarget(c, 1, 5);
        if (!game.text.Contains(fn) || !MatchesAt(fn, ParsePattern(kGetNum))) continue;
        if (RipTarget(fn, 2, 6) != num || RipTarget(fn + 6, 2, 6) != prepared) continue;
        lowMem = RipTarget(c + 5, 2, 7);
    }
    uint8_t* const count = at(sp, kSfCount);
    uint8_t* const lowCount = at(sp, kSfLowCount);
    if (!lowMem || !game.data.Contains(lowMem) || !game.data.Contains(count) || !game.data.Contains(lowCount)) {
        *why = "the pending-level trigger's count, its GetNumLevelsPendingPurge call or GIsLowMemory not found";
        return false;
    }
    // The engine's GC lock: TryGCLock (inlined in TryCollectGarbage) and ReleaseGCLock on the same GGCSingleton
    const auto tg = FindAll(game.text, ParsePattern(kTryGc));
    const auto rg = FindAll(game.text, ParsePattern(kRelGc));
    if (tg.size() != 1 || rg.size() != 1) { *why = "the engine's GC lock (TryGCLock / ReleaseGCLock) not found once"; return false; }
    uint8_t* const single = at(tg[0], kTgSingleton);
    uint8_t* const enter = at(tg[0], kTgEnter);
    uint8_t* const leave = at(tg[0], kTgLeave);
    // the two calls are the game's imports of KERNEL32 EnterCriticalSection / LeaveCriticalSection (by the import name
    // table: also right before the loader has filled the slots), so the lock's Critical is a CRITICAL_SECTION
    if (at(rg[0], kRgSingleton) != single || !game.data.Contains(single) ||
        enter != reinterpret_cast<uint8_t*>(FindImportSlot(game.base, "KERNEL32.dll", "EnterCriticalSection")) ||
        leave != reinterpret_cast<uint8_t*>(FindImportSlot(game.base, "KERNEL32.dll", "LeaveCriticalSection"))) {
        *why = "the GC lock's TryGCLock and ReleaseGCLock do not use one lock object, or its critical-section calls are not the system's";
        return false;
    }
    f->tryGc = tg[0];
    f->relGc = rg[0];
    f->gcSingleton = reinterpret_cast<void* const*>(single);
    f->enterSlot = reinterpret_cast<void* const*>(enter);
    f->leaveSlot = reinterpret_cast<void* const*>(leave);
    f->onPreGc = g;
    f->prepOne = p;
    f->get = get;
    f->inTick = t;
    f->soft = sp;
    f->num = reinterpret_cast<int32_t*>(num);
    f->data = reinterpret_cast<uint64_t**>(num - 8);
    f->prepared = reinterpret_cast<int32_t*>(prepared);
    f->softCount = reinterpret_cast<const int32_t*>(count);
    f->lowCount = reinterpret_cast<const int32_t*>(lowCount);
    f->lowMemory = lowMem;
    f->engine = reinterpret_cast<void* const*>(engine);
    f->worldList = wl;
    f->worldNum = wn;
    f->ctxWorld = cw;
    f->inTickOff = ti;
    f->inTickMask = mask;
    return true;
}

// Discovery once (start-up, game thread): main.cpp asks before it installs the shared timer hook for us
Found g_found{};
int g_findState = 0;  // 0 not tried, 1 found, -1 not found
const char* g_findWhy = "";

bool FindOnce()
{
    if (g_findState == 0) {
        g_findState = Discover(Game(), &g_found, &g_findWhy) ? 1 : -1;
    }
    return g_findState > 0;
}

}  // namespace

// Start-up, before InstallGcPassAfterBusy: GcPrepAhead's code is found in this exe (so the shared hook has a use)
bool GcPrepAheadFindable() { return FindOnce(); }

bool InstallGcPrepAhead(bool beforeGameCode, int key)
{
    if (g_installed) return true;
    if (key == 0) {
        Log("[gcprepahead] GcPrepAhead off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[gcprepahead] GcPrepAhead: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    if (!FindOnce()) {
        Log("[gcprepahead] GcPrepAhead NOT installed (the game's way): %s", g_findWhy);
        return false;
    }
    if (!GcPassAfterBusyInstalled()) {
        Log("[gcprepahead] GcPrepAhead NOT installed (the game's way): it runs from the garbage collection timer's hook, which is not installed");
        return false;
    }
    const GameModule& game = Game();
    const Found& f = g_found;
    g_lock.singleton = f.gcSingleton;
    g_lock.enter = reinterpret_cast<CsFn>(&EnterCriticalSection);  // the functions the game's slots import (checked by name)
    g_lock.leave = reinterpret_cast<CsFn>(&LeaveCriticalSection);
    g_lock.release = reinterpret_cast<void (*)()>(f.relGc);
    g_env = Env{};
    g_env.tryLock = &GameTryGcLock;
    g_env.unlock = &GameGcUnlock;
    g_env.num = f.num;
    g_env.data = f.data;
    g_env.prepared = f.prepared;
    g_env.get = reinterpret_cast<GetFn>(f.get);
    g_env.prep = reinterpret_cast<PrepFn>(f.prepOne);
    g_env.softCount = f.softCount;
    g_env.lowMemory = f.lowMemory;
    g_env.engine = f.engine;
    g_env.worldList = f.worldList;
    g_env.worldNum = f.worldNum;
    g_env.ctxWorld = f.ctxWorld;
    g_env.inTickOff = f.inTickOff;
    g_env.inTickMask = f.inTickMask;
    g_env.now = &QpcUs;
    g_env.frame = &NpcPacingFrameState;
    g_env.menu = &ReadMenuFreeze;
    g_stamps.reserve(256);
    g_next.reserve(256);
    g_installed = true;
    Log("[gcprepahead] PrepareStreamedOutLevelForGC exe+0x%X (from OnPreGarbageCollect exe+0x%X), LevelsPendingUnload exe+0x%X, "
        "prepared count exe+0x%X, pending-level trigger exe+0x%X (count now %d), world-tick test exe+0x%X, GC lock exe+0x%X "
        "(TryGCLock exe+0x%X, ReleaseGCLock exe+0x%X): installed, %s",
        static_cast<unsigned>(f.prepOne - game.base), static_cast<unsigned>(f.onPreGc - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(f.data) - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(f.prepared) - game.base),
        static_cast<unsigned>(f.soft - game.base), *f.softCount, static_cast<unsigned>(f.inTick - game.base),
        static_cast<unsigned>(reinterpret_cast<const uint8_t*>(f.gcSingleton) - game.base), static_cast<unsigned>(f.tryGc - game.base),
        static_cast<unsigned>(f.relGc - game.base),
        key ? "streamed-out levels are prepared for the next garbage collection pass in calm frames just before it"
            : "off (GcPrepAhead=0)");
    return true;
}

// Monitor thread, every second: the key (0 off, 1 on) and the hidden tuning keys
void SetGcPrepAhead(int key, int minAgeMs, int maxPerFrame, int budgetUs, int windowMs)
{
    g_minAgeMs.store(std::clamp(minAgeMs, 1000, 30000), std::memory_order_relaxed);
    g_maxPerFrame.store(std::clamp(maxPerFrame, 1, 8), std::memory_order_relaxed);
    g_budgetUs.store(std::clamp(budgetUs, 100, 2000), std::memory_order_relaxed);
    g_windowMs.store(std::clamp(windowMs, 200, 2000), std::memory_order_relaxed);
    const int on = key ? 1 : 0;
    const int was = g_on.exchange(on);
    if (g_installed && was != on)
        Log(on ? "[gcprepahead] streamed-out levels are prepared for the next garbage collection pass in calm frames just before it"
               : "[gcprepahead] the garbage collection pass prepares all streamed-out levels itself, as the game does");
}

void LogGcPrepAheadStats()
{
    if (!g_installed) return;
    const uint64_t windows = s.windows.exchange(0), passes = s.passes.exchange(0), levels = s.levels.exchange(0);
    uint64_t w[kWhyN];
    for (int i = 0; i < kWhyN; ++i) w[i] = s.why[i].exchange(0);
    const uint64_t frames = s.frames.exchange(0), us = s.us.exchange(0), mu = s.maxUs.exchange(0), pp = s.passesPrepped.exchange(0),
                   left = s.left.exchange(0), late = s.late.exchange(0), budget = s.budget.exchange(0), nulls = s.nulls.exchange(0),
                   gone = s.gone.exchange(0), cOld = s.cancelOld.exchange(0), cYoung = s.cancelYoung.exchange(0),
                   wanted = s.wanted.exchange(0), wantedAfter = s.wantedAfter.exchange(0), softW = s.softWindows.exchange(0),
                   lateSoft = s.lateSoft.exchange(0), paused = s.softPaused.exchange(0), lockStops = s.lockStops.exchange(0);
    if (!windows && !passes && !levels && !cOld && !cYoung && !wanted && !wantedAfter) return;
    auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    Log("[gcprepahead] since last report: %llu levels prepared early in %llu frames (%.2f ms, longest %.2f ms, %.3f ms a level), "
        "%llu windows (%llu by the soft trigger); %llu GC passes, %llu with early preparation (%llu levels left for them); "
        "late passes (prepared levels waited > 2 s): %llu timer + %llu soft-trigger (soft windows paused after it %llu); "
        "prepared early and wanted again: before the pass %llu, within 2 s after it %llu; "
        "frames refused in a window: pass due %llu, purge %llu, menu/loading %llu, low memory %llu, async loading %llu, "
        "world in tick %llu, frame unknown %llu, frame >= 12 ms %llu, busy %llu, none old enough %llu, GC lock held %llu "
        "(+ %llu frames stopped by it after a level); budget stops %llu; stale entries left for the pass %llu; gone from the "
        "list during the call %llu; taken back by the game (reuse) %llu (pending >= min age) + %llu (younger)",
        u(levels), u(frames), us / 1000.0, mu / 1000.0, levels ? us / 1000.0 / static_cast<double>(levels) : 0.0, u(windows),
        u(softW), u(passes), u(pp), u(left), u(late), u(lateSoft), u(paused), u(wanted), u(wantedAfter), u(w[kDue]), u(w[kPurge]),
        u(w[kMenu]), u(w[kLowMem]), u(w[kAsync]), u(w[kInTickW]), u(w[kFrameUnknown]), u(w[kLong]), u(w[kBusy]), u(w[kYoung]),
        u(w[kGcLockHeld]), u(lockStops), u(budget), u(nulls), u(gone), u(cOld), u(cYoung));
}

// ---- self-test ----
// rva: OnPreGarbageCollect, PrepareStreamedOutLevelForGC, Get, RequestUnload's test, the trigger, Num, Prepared, the soft
// count, the low-memory count, GIsLowMemory, GEngine, TryGCLock's site, ReleaseGCLock, GGCSingleton, the two import slots
bool GcPrepAheadTestDiscover(const GameModule& game, uint32_t rva[16], uint32_t off[5], const char** why)
{
    Found f{};
    *why = "";
    if (!Discover(game, &f, why)) return false;
    const uint8_t* const p[16] = {f.onPreGc, f.prepOne, f.get, f.inTick, f.soft, reinterpret_cast<const uint8_t*>(f.num),
                                  reinterpret_cast<const uint8_t*>(f.prepared), reinterpret_cast<const uint8_t*>(f.softCount),
                                  reinterpret_cast<const uint8_t*>(f.lowCount), f.lowMemory, reinterpret_cast<const uint8_t*>(f.engine),
                                  f.tryGc, f.relGc, reinterpret_cast<const uint8_t*>(f.gcSingleton),
                                  reinterpret_cast<const uint8_t*>(f.enterSlot), reinterpret_cast<const uint8_t*>(f.leaveSlot)};
    for (int i = 0; i < 16; ++i) rva[i] = static_cast<uint32_t>(p[i] - game.base);
    off[0] = f.worldList;
    off[1] = f.worldNum;
    off[2] = f.ctxWorld;
    off[3] = f.inTickOff;
    off[4] = f.inTickMask;
    return true;
}

bool GcPrepAheadTestFindOn(const GameModule& game) { Found f{}; const char* why = ""; return Discover(game, &f, &why); }

// The real TryGCLock on a lock object laid out as the engine's (the self-test's fake FGCCSyncObject with a real
// CRITICAL_SECTION and a fake event); release = the self-test's ReleaseGCLock. Returns GameTryGcLock's answer.
bool GcPrepAheadTestTryLock(void* const* singleton, void (*release)(), bool unlockAfter)
{
    const GcLock saved = g_lock;
    g_lock.singleton = singleton;
    g_lock.enter = reinterpret_cast<CsFn>(&EnterCriticalSection);
    g_lock.leave = reinterpret_cast<CsFn>(&LeaveCriticalSection);
    g_lock.release = release;
    const bool ok = GameTryGcLock();
    if (ok && unlockAfter) GameGcUnlock();
    g_lock = saved;
    return ok;
}

void GcPrepAheadTestSetup(const GcPrepAheadFakes& k)
{
    g_env = Env{};
    g_env.num = k.num;
    g_env.data = k.data;
    g_env.prepared = k.prepared;
    g_env.get = k.get;
    g_env.prep = k.prep;
    g_env.softCount = k.softCount;
    g_env.lowMemory = k.lowMemory;
    g_env.engine = k.engine;
    g_env.worldList = k.worldList;
    g_env.worldNum = k.worldNum;
    g_env.ctxWorld = k.ctxWorld;
    g_env.inTickOff = k.inTickOff;
    g_env.inTickMask = k.inTickMask;
    g_env.now = k.now;
    g_env.frame = k.frame;
    g_env.menu = k.menu;
    g_env.tryLock = k.tryLock;
    g_env.unlock = k.unlock;
    Reset();
    g_prevPrepared = g_numAtDue = 0;
    for (auto& r : g_ring) r = PrepRec{};
    g_ringNext = g_ringLive = 0;
    g_gen = 1;
    g_lastPassUs = 0;
    g_lastAdded = nullptr;
    g_passLogs = 0;
    s.levels = s.frames = s.us = s.maxUs = s.windows = s.passes = s.passesPrepped = s.left = s.late = s.budget = s.nulls = s.gone =
        s.cancelOld = s.cancelYoung = s.wanted = s.wantedAfter = s.softWindows = s.lateSoft = s.softPaused = s.lockStops = 0;
    for (auto& w : s.why) w = 0;
    g_installed = true;
}

void GcPrepAheadTestTeardown()
{
    g_env = Env{};
    g_installed = false;
    g_on.store(0);
    Reset();
    g_prevPrepared = g_numAtDue = 0;
    for (auto& r : g_ring) r = PrepRec{};
    g_ringNext = g_ringLive = 0;
}

// 0 levels, 1 frames, 2 windows, 3 passes, 4 passes with early preparation, 5 left, 6 late (timer), 7 budget stops,
// 8 stale, 9 gone, 10 reuse old, 11 reuse young, 12 wanted before, 13 wanted after, 14.. refusals (kWhyN = 11),
// 25 soft windows, 26 late (soft), 27 soft paused, 28 lock stops
void GcPrepAheadTestCounts(uint64_t out[32])
{
    out[0] = s.levels; out[1] = s.frames; out[2] = s.windows; out[3] = s.passes; out[4] = s.passesPrepped; out[5] = s.left;
    out[6] = s.late; out[7] = s.budget; out[8] = s.nulls; out[9] = s.gone; out[10] = s.cancelOld; out[11] = s.cancelYoung;
    out[12] = s.wanted; out[13] = s.wantedAfter;
    for (int i = 0; i < kWhyN; ++i) out[14 + i] = s.why[i];
    out[25] = s.softWindows; out[26] = s.lateSoft; out[27] = s.softPaused; out[28] = s.lockStops;
}

bool GcPrepAheadTestRemove(uint64_t key) { return RemoveByValue(key); }

}  // namespace opf
