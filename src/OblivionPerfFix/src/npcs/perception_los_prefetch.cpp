// PerceptionLosPrefetch ([Fixes] PerceptionLosPrefetch, default 0 = off; 2026-11-14, contributed by bpodwinski).
//
// The problem: every NPC runs a perception round five times a second on the game thread (a timer method of the paired pawn;
// this build exe+0x485FA60). The round lists the world's pawns, and for every pawn that passes its filters and is within
// its perception range it calls a line-of-sight helper (exe+0x485A170): up to three LineTraceSingleByChannel (channel 17)
// from the NPC's eyes to the other pawn (a component, then two skeleton sockets), ignoring both pawns' attached and child
// actors. In a crowd of 40 NPCs these rounds are the largest single item of the game thread (tick split, 2026-11-14:
// ~0.5 ms per round, ~30 % of the timed game-thread work).
//
// The change: at the start of a round, right after the game built its pawn list, the line-of-sight helper is run on
// helper threads (and this thread) for every pawn the round's loop would reach the helper with, using the same checks in
// the same way (not null, of the round's pawn class, not the NPC itself, a paired old-engine object, its root within the
// range computed from the same settings floats in the same float order), the game thread waiting until all are done
// (with PerceptionLosPrefetchOverlap, the default, the loop starts at once; see Overlap below).
// Then the game's loop runs unchanged, in its own order; at its helper call site (redirected to us) a call for this round's
// NPC, a precomputed pawn and the stock arguments (3 tries, flag 1, result byte 3) gets the precomputed return value and
// result byte; anything else runs the game's helper.
//
// Why the result is the same: the helper only reads (its writes go to its own stack and to the caller's result byte;
// checked in its code: no other store outside its frame), except what its callees do for a hit on a lightweight instance:
// see the guard below (our review). Between the precomputation and the stock call, the round's loop
// does only three things per pawn: a virtual filter on the NPC (vtable +0xA90), an insert into the NPC's own perception map
// (+0xC80), and a command to the old engine's queue (exe+0x4865DB0: a 0x30-byte command "X sees Y at level N"); none of
// them moves an actor, changes an attachment or writes the physics scene the traces read. The game thread is blocked in
// the round while the helpers trace, so no game-thread write can interleave (no garbage collection either: it runs on the
// game thread). Other threads (animation workers moving bones, the physics thread) run during the stock round too, so a
// trace answered microseconds earlier can only differ by a race the game does not order (RigTraceReuse's argument).
// UE runs scene queries off the game thread itself (async traces under the scene read lock).
// The precomputation also traces pawns the virtual filter would reject; that costs helper time only (read-only helper).
//
// Measured 2026-11-14 (Ryzen 7 5800X3D, RTX 5080, profile small, actors-crowd / actors-battle, one game per A-B-B-A, key
// switched live): the perception round of an Imperial Legion soldier 400-450 / 750-900 us in the game, 135-150 / 205-230 us
// with the defaults below (field-of-view filter, overlap on, 4 helpers); battle +12 % fps with the overlap; 0 different
// answers in more than 1.1 million verified checks, 0 checks precomputed for nothing. Helper threads: 2 -> 4 is the gain,
// 6 / 8 add nothing. PerceptionLosIgnoreCache: ~3-4 us saved per check on the helpers, within noise (off). PerceptionLos-
// CrossRound: 0 different in 354,923 checks served to a later round, but slower (the frame batch cannot overlap): off.
// 4K (native-nofg: FSR Performance, no frame generation), same scenes, A-B-B-A in one game: game-thread ms per frame
// 17.2 / 18.5 on vs 19.4 / 19.6 off (crowd), 39.6 / 42.2 on vs 60.3 / 59.5 off (battle, 19.0-20.8 vs 13.7-14.2 fps);
// 357,150 checks served in the battle, 0 precomputed for nothing.
//
// [Fixes] PerceptionLosPrefetch: 0 = not installed (start-up), 1 = on, 2 = verify (the game's helper runs anyway and its
// result is compared with the precomputed one; the game's result is used); installed only if not 0 at start-up (version.dll
// route), then live. Off while ParallelHelpersAllowed is false. Hidden: PerceptionLosPrefetchMin (4: precomputed pawns
// needed for a batch, live), PerceptionLosPrefetchThreads (4: helper threads, 0..8, start-up).
//
// Where (all found by pattern and checked; any mismatch = not installed, with a log line):
// - R: the round after its pawn list (exe+0x485FAF4): the list at [rsp+60h] {data, count}, the pawn class getter call,
//   the settings getter call (exe+0x4711300, no arguments) whose call we redirect to a stub that calls it and then us,
//   the range constant (movss xmm7).
// - L: the loop's checks (R+0x5E): class chain at +0x10 / +0x30 / +0x38, self compare, old-engine object +0x7B8 / +0x120,
//   the range floats +0x4E8 / +0x4EC and their float order, the virtual filter +0xA90, the other's root +0x1A0 and its
//   location +0x280 / +0x290.
// - C: the helper's call site (R+0x1CC): r9d 3, [rsp+20h] 1, r8 = the result byte at [rsp+30h], rdx other, rcx self.
// - H: the helper itself (its prologue up to the result byte's first store), equal to C's call target.
// - the NPC's own root +0x1A0 at R+0x145.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <xmmintrin.h>

namespace opf {
namespace {

constexpr char kRound[] =
    "48 89 87 48 0C 00 00 E8 ?? ?? ?? ?? 48 8B CF 48 8B D8 E8 ?? ?? ?? ?? 4C 8D 44 24 60 48 8B D3 48 8B C8 E8 ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? 4C 8B 74 24 60 48 63 4C 24 68 4D 8D 2C CE 4D 3B F5 0F 84 ?? ?? ?? ?? 0F 29 BC 24 20 01 00 00 48 8B F0 "
    "F3 0F 10 3D ?? ?? ?? ??";
constexpr size_t kRClassCall = 0x07, kRSettingsCall = 0x27, kRConst = 0x4E, kRLoop = 0x5E, kRSelfRoot = 0x145, kRCall = 0x1CC;
constexpr char kLoop[] =
    "49 8B 1E 48 85 DB 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 53 10 4C 8D 40 30 48 63 40 38 3B 42 38 0F 8F ?? ?? ?? ?? 48 8B C8 "
    "48 8B 42 30 4C 39 04 C8 0F 85 ?? ?? ?? ?? 48 3B DF 0F 84 ?? ?? ?? ?? 48 8B 83 B8 07 00 00 48 83 B8 20 01 00 00 00 0F 84 "
    "?? ?? ?? ?? F3 0F 10 8E E8 04 00 00 41 B7 03 F3 0F 10 86 EC 04 00 00 0F 28 F1 48 8B 07 48 8B D3 F3 0F 59 F7 48 8B CF 45 "
    "32 E4 44 88 7C 24 30 F3 0F 59 C8 F3 0F 59 F0 F3 0F 59 F1 FF 90 90 0A 00 00 84 C0 0F 84 ?? ?? ?? ?? 48 8B 83 A0 01 00 00 "
    "F2 0F 10 15 ?? ?? ?? ?? 0F 10 1D ?? ?? ?? ?? 48 85 C0 74 1D 0F 10 80 80 02 00 00";
constexpr size_t kLClassCall = 0x0C;
constexpr char kSelfRoot[] = "48 8B 87 A0 01 00 00";
constexpr char kCall[] =
    "66 0F 2F E0 73 25 41 B9 03 00 00 00 C6 44 24 20 01 4C 8D 44 24 30 48 8B D3 48 8B CF E8 ?? ?? ?? ?? 44 0F B6 7C 24 30 "
    "44 0F B6 E0";
constexpr size_t kCCall = 0x1C;
// J: the loop's single exit (R+0x2E9; both the loop's end and the empty list's jump at R+0x3D land here): movss xmm6;
// mov rcx,rdi; divss xmm6,[rdi+0C3Ch]; call (the NPC's world getter, redirected to the join stub); lea r14,[rdi+0C40h].
constexpr char kJoin[] = "F3 0F 10 35 ?? ?? ?? ?? 48 8B CF F3 0F 5E B7 3C 0C 00 00 E8 ?? ?? ?? ?? 4C 8D B7 40 0C 00 00";
constexpr size_t kRJoin = 0x2E9, kJCall = 0x13, kREmptyJump = 0x3D;
constexpr char kHelper[] =
    "4C 8B DC 55 53 56 57 41 57 49 8D AB A8 FE FF FF 48 81 EC 30 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 10 01 00 00 "
    "41 8B F9 49 8B F0 48 8B DA 4C 8B F9 48 85 D2 0F 84 ?? ?? ?? ?? 48 83 BA B8 07 00 00 00 0F 84 ?? ?? ?? ?? 4D 89 63 D0 48 "
    "8D 4D B0 4D 89 73 C8 41 C6 00 03";

// F: the loop's virtual filter (vtable +0xA90) of the humanoid and creature NPC classes (exe+0x485A900; seen at run time,
// 2026-11-14): a field-of-view test. The NPC root's forward axis (exe+0x3030540: the X axis turned by the component's
// rotation, +0x260 / +0x270; reads only), flattened and normalised, against the flattened, normalised direction to the other
// pawn's root; true when the angle (acos of the clamped dot product, exe+0x6BFC566) is below a constant. It reads the two
// roots and writes only its own frame, so the precomputation may call it for the same NPC and pawn with the same answer.
// It is called only when the NPC's vtable slot holds this function; another class's filter (a player-like vtable points
// at exe+0x4831C00, which calls further virtuals) is not called: its pawns are all precomputed, as before.
constexpr char kFov[] =
    "48 89 5C 24 10 48 89 7C 24 18 55 48 8B EC 48 83 EC 70 48 8B 81 A0 01 00 00 48 8B FA 0F 29 74 24 60 48 8B D9 0F 29 7C 24 "
    "50 48 85 C0 74 18 48 8D 55 C8 48 8B C8 E8 ?? ?? ?? ?? 4C 8B C0 48 8B 83 A0 01 00 00 EB 1C 0F 10 05 ?? ?? ?? ?? 4C 8D 45 "
    "B0 F2 0F 10 0D ?? ?? ?? ?? 0F 11 45 B0 F2 0F 11 4D C0 41 0F 10 30 F2 41 0F 10 40 10 F2 0F 11 45 D8 0F 28 EE 66 0F 15 EE "
    "0F 28 C6 0F 28 CD F2 0F 59 C6 F2 0F 59 CD F2 0F 58 C8 66 0F 2F 0D ?? ?? ?? ?? 76 1F F2 0F 10 1D ?? ?? ?? ?? 0F 57 D2 F2 0F "
    "10 D1 F2 0F 51 CA F2 0F 5E D9 F2 0F 59 F3 F2 0F 59 EB 48 8B 8F A0 01 00 00";
constexpr size_t kFovGetterCall = 0x35, kFovAcosCall = 0x19E, kFovTail = 0x1CF;
constexpr char kFovGetter[] = "4C 8B DC 48 81 EC 88 00 00 00 0F 10 A1 60 02 00 00 0F 57 C0";
constexpr char kFovTailBytes[] = "0F 92 C0 49 8B E3 5D C3";

// G: inside the helper (H+0xDB): its four actor gathers for the query's ignore list, each into an empty TArray (r14 = 0):
// the NPC's attached actors (exe+0x2DBC960, rcx = r15 = the NPC, r8b 1) at +0x15, the other pawn's at +0x36, the NPC's child
// actors (exe+0x2DBCFC0, r8b 1, r9b 0) at +0x64, the other pawn's at +0x88. PerceptionLosIgnoreCache (hidden, live, needs the
// round-end join): the NPC's two lists are gathered once per batched round on the game thread (the game's functions, into
// emptied arrays) and the helper's two NPC calls get a copy (FMemory::Malloc, freed by the helper as its own); nothing in
// the round attaches or detaches an actor. Outside a batched round (and after its join) every gather runs as in the game.
constexpr char kGather[] =
    "41 B0 01 4C 89 74 24 50 48 8D 54 24 50 4C 89 74 24 58 49 8B CF E8 ?? ?? ?? ?? 48 8D 54 24 50 48 8D 8D A0 00 00 00 E8 "
    "?? ?? ?? ?? 41 B0 01 48 8D 54 24 50 48 8B CB E8 ?? ?? ?? ?? 48 8D 54 24 50 48 8D 8D A0 00 00 00 E8 ?? ?? ?? ?? 45 33 C9 "
    "4C 89 74 24 40 41 B0 01 4C 89 74 24 48 48 8D 54 24 40 49 8B CF E8 ?? ?? ?? ?? 48 8D 54 24 40 48 8D 8D A0 00 00 00 E8 ?? "
    "?? ?? ?? 45 33 C9 48 8D 54 24 40 41 B0 01 48 8B CB E8 ?? ?? ?? ??";
constexpr size_t kHGather = 0xDB, kGSelfAtt = 0x15, kGOtherAtt = 0x36, kGSelfChild = 0x64, kGOtherChild = 0x88;
// FMemory::Malloc(count, alignment) (exe+0xE32F40; the pattern of settings/hdr_menu.cpp): GMalloc's slot +0x28.
constexpr char kMalloc[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
    "48 8B 01 44 8B C3 48 8B D7 48 8B 5C 24 30 48 83 C4 20 5F 48 FF 60 28";
constexpr size_t kMallocG1 = 15, kMallocG2 = 32;

// Lightweight-instance guard (added in our review of PR #55, 2026-11-16): the helper asks each hit for its actor
// (FHitResult::GetActor, three calls at H+0x214 / H+0x302 / H+0x39A). For an ordinary actor that is a weak-pointer read,
// but for a hit on a UE lightweight instance (an instance owned by an ALightWeightInstanceManager) FetchActor can spawn an
// actor, and the hit's conversion counts a world-wide id up without an atomic (UE 5.3 ActorInstanceHandle.cpp): game-thread
// work. Both need a trace to hit a body owned by a manager. A manager lists itself in its constructor (UE 5.3, also on the
// loading thread), but its components' bodies reach the scene-query structure only when the game thread registers them,
// after that constructor; removal is in the destructor (garbage collection, game thread, after the components left the
// scene). So with no manager listed at a round's start no manager-owned body is in the scene for the whole round (the game
// thread is in the round), whatever the loading thread does. A round is therefore batched only while the engine's
// lightweight-instance subsystem lists no manager (the subsystem's static, read through GetActor's slow path: the
// subsystem getter, its static, the manager list read by the manager lookup). With no subsystem yet, a helper hit without
// a valid actor may create the empty singleton (the engine's own critical section): an allocation, no world state.
constexpr size_t kHGetActorCalls[] = {0x214, 0x302, 0x39A};
// the helper's eye-point call: call [rax+0A98h] with rcx = the NPC (H+0x18B; our review)
constexpr size_t kHEyeCall = 0x18B;
constexpr char kEyeCall[] = "FF 90 98 0A 00 00";
constexpr char kGetActor[] =
    "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 0D 48 8B CB 48 83 C4 20 5B E9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D3 48 8B "
    "C8 48 83 C4 20 5B E9 ?? ?? ?? ??";
constexpr size_t kGaSubsystemCall = 0x1F, kGaFetchJmp = 0x2F;
constexpr char kLwiGet[] =
    "48 83 EC 38 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 85 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 83 3D ?? ?? ?? ?? 00";
constexpr size_t kLgStatic = 0x04, kLgStatic2 = 0x21;
constexpr char kLwiFetch[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B CA E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 48 8B D3 48 8B CF E8";
constexpr size_t kLfFindCall = 0x26;
// the manager lookup: lock at +0x10, then the manager array {data +0, count +8}
constexpr char kLwiFindList[] = "48 8D 5F 10 48 8B CB FF 15 ?? ?? ?? ?? 4C 8B 37 48 63 47 08";
constexpr size_t kLwiFindListAt = 0x77, kLwiCount = 0x08;
// the eye point the helper asks the NPC for (vtable +0xA98; exe+0x4858700): reads the NPC's components only (its +0x330
// location, or its +0x748 mesh's head socket). A round is batched only for NPCs whose vtable holds this one (our review).
constexpr char kEye[] =
    "48 89 5C 24 08 57 48 83 EC 40 48 8B 81 30 03 00 00 48 8B DA 48 8B F9 0F 10 80 80 02 00 00 0F 11 02 0F 10 88 90 02 00 00 "
    "F2 0F 11 4A 10 45 84 C0";

// layout (pinned by the patterns above)
constexpr size_t kClassPtr = 0x10, kChain = 0x30, kDepth = 0x38, kPaired = 0x7B8, kPairedObj = 0x120, kRoot = 0x1A0, kLoc = 0x280,
                 kRangeA = 0x4E8, kRangeB = 0x4EC;
constexpr int kTries = 3;
constexpr uint8_t kFlag = 1, kOutInit = 3;
constexpr int kMaxJobs = 1024;

template <class T> T Rd(const void* p, size_t off)
{
    T v;
    memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof v);
    return v;
}

using HelperFn = uint8_t (*)(uint8_t* self, uint8_t* other, uint8_t* out, int tries, uint8_t flag);
using ClassFn = const uint8_t* (*)();
using FovFn = uint8_t (*)(uint8_t* self, uint8_t* other);  // the answer is al
HelperFn g_helper = nullptr;
ClassFn g_class = nullptr;
const void* g_fov = nullptr;  // the field-of-view filter, when found
const void* g_eye = nullptr;  // the NPC eye point the helper calls (vtable +0xA98); nullptr = not checked (self-test)
uint8_t* const* g_lwiStatic = nullptr;  // the lightweight-instance subsystem's static (its object pointer); nullptr = self-test
uint32_t g_csr = 0x1F80;      // the round's thread's SSE control word, given to the helpers with each batch

// the ignore-list cache (PerceptionLosIgnoreCache)
struct TArr {
    void** data;
    int32_t num, max;
};
using AttachedFn = void (*)(uint8_t* actor, TArr* out, uint8_t flag);
using ChildFn = void (*)(uint8_t* actor, TArr* out, uint8_t flag, uint8_t descend);
using MallocFn = void* (*)(size_t count, uint32_t alignment);
AttachedFn g_attached = nullptr;
ChildFn g_child = nullptr;
MallocFn g_malloc = nullptr;
bool g_cacheInstalled = false;
std::atomic<int> g_cacheOn{0};  // 0 off, 1 on (mode 2 of the fix verifies the copies too)
TArr g_cacheAtt{}, g_cacheChild{};  // the NPC's lists of the open round (buffers from the game's allocator, reused)
bool g_cacheValid = false;          // game thread writes; the helpers read it only inside a dispatched batch
float g_rangeConst = 1.0f;
bool g_installed = false;

std::atomic<int> g_mode{0};  // 0 off, 1 serve, 2 verify
std::atomic<int> g_min{4};
int g_threads = 4;

struct Job {
    uint8_t *self, *other;
    std::atomic<int> state;  // 0 queued, 1 done
    uint8_t ret, out;
    bool used;
};
Job g_jobs[kMaxJobs];
int g_jobCount = 0;
uint8_t* g_roundSelf = nullptr;
DWORD g_roundThread = 0;
int g_roundBegin = 0, g_roundEnd = 0, g_cursor = 0;  // the current round's jobs [begin, end); the search starts at the cursor
bool g_roundCross = false;                           // the current round's jobs came from an earlier round's frame batch

// PerceptionLosCrossRound (hidden, live, experimental): the first batched round of a frame also precomputes the NPCs whose
// round is expected later in the same frame (each NPC's period is learnt from its own rounds); their rounds then take their
// jobs from that batch. The whole frame batch runs before the first round's loop (never overlapped), so no helper traces
// while the game's other timers run. Whether a precomputed answer still holds a few timers later is what verify measures.
struct Range {
    uint8_t* self;
    int begin, end;
};
constexpr int kMaxRanges = 64;
Range g_ranges[kMaxRanges];
int g_rangeCount = 0;
uint64_t g_batchFrame = ~0ull;  // the frame the jobs belong to (cross-round only)
std::atomic<bool> g_cross{false};
struct Sched {
    uint8_t* npc;
    double lastUs, periodUs;
    uint64_t lastFrame;
};
constexpr int kSched = 1024;  // open addressing on the NPC pointer
Sched g_sched[kSched];
double g_frameUs = 16667.0, g_frameStartUs = 0;
uint64_t g_seenFrame = ~0ull;
uint64_t (*g_frameFn)() = &NpcPacingFrame;  // self-test: a stand-in counter
// gen << 32 | next << 16 | count: the helpers claim jobs by position
std::atomic<uint64_t> g_word{0};
uint32_t g_gen = 0;
HANDLE g_sem = nullptr;
int g_helpers = 0;
bool g_helpersTried = false;

struct Stats {
    std::atomic<uint64_t> rounds{0}, batches{0}, jobs{0}, served{0}, verified{0}, diff{0}, fallback{0}, unused{0}, wallUs{0},
        maxWallUs{0}, outOfView{0}, otherFilter{0}, overlapped{0}, cacheRounds{0}, cacheUs{0}, cacheServed{0},
        cacheVerified{0}, cacheDiff{0}, crossNpcs{0}, crossRounds{0}, crossVerified{0}, crossDiff{0}, crossServed{0},
        lwi{0}, otherEye{0};
};
bool g_lwiLogged = false;
Stats g_s;

int Claim()
{
    uint64_t w = g_word.load(std::memory_order_acquire);
    for (;;) {
        const uint32_t next = static_cast<uint32_t>(w >> 16) & 0xFFFF, count = static_cast<uint32_t>(w) & 0xFFFF;
        if (next >= count) return -1;
        const uint64_t nw = (w & ~0xFFFF0000ull) | (static_cast<uint64_t>(next + 1) << 16);
        if (g_word.compare_exchange_weak(w, nw, std::memory_order_acq_rel, std::memory_order_acquire)) return static_cast<int>(next);
    }
}

void RunJob(Job& j)
{
    // the round's floating-point control state (rounding, denormal handling; published with the batch), so a helper's
    // traces compute as they would on the round's own thread (our review of PR #55; a no-op on that thread)
    if (GetCurrentThreadId() != g_roundThread && _mm_getcsr() != g_csr) _mm_setcsr(g_csr);
    uint8_t out = kOutInit;
    j.ret = g_helper(j.self, j.other, &out, kTries, kFlag);
    j.out = out;
    j.state.store(1, std::memory_order_release);
    WakeWordAll(&j.state);
}

DWORD WINAPI Helper(void*)
{
    for (;;) {
        WaitForSingleObject(g_sem, INFINITE);
        int i;
        while ((i = Claim()) >= 0) RunJob(g_jobs[i]);
    }
}

void StartHelpers()
{
    g_helpersTried = true;
    g_sem = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);
    if (!g_sem) return;
    for (int i = 0; i < g_threads; ++i) {
        HANDLE t = CreateThread(nullptr, 1 << 20, Helper, nullptr, 0, nullptr);
        if (!t) break;
        DescribeThread(t, L"OPF PerceptionLosPrefetch");
        CloseHandle(t);
        ++g_helpers;
    }
    Log("[perclos] PerceptionLosPrefetch: %d helper threads started", g_helpers);
}

// The class chain check of the round's loop (L: [obj+0x10] class, its chain at +0x30, depth at +0x38).
bool IsA(const uint8_t* obj, const uint8_t* cls)
{
    const uint8_t* const oc = Rd<const uint8_t*>(obj, kClassPtr);
    if (!oc) return false;
    const int depth = Rd<int>(cls, kDepth);
    if (depth > Rd<int>(oc, kDepth)) return false;
    const uint8_t* const chain = Rd<const uint8_t*>(oc, kChain);
    return chain && Rd<const uint8_t*>(chain, static_cast<size_t>(depth) * 8) == cls + kChain;
}

double QpcUs()
{
    static int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1e6 / static_cast<double>(freq);
}

// Overlap (PerceptionLosPrefetchOverlap, hidden, live; needs the round-end join): the game thread does not wait for the
// whole batch at the round's start; it runs the loop and waits only for the answer it asks for (claiming unclaimed jobs
// itself meanwhile), and the round-end hook (the loop's single exit, before anything else of the round) waits for the rest.
// While the helpers trace, the game thread runs only the loop's own work (the filter, the NPC's map, the old-engine queue),
// none of which the helper reads or the traces see; no helper work outlives the round.
std::atomic<bool> g_overlap{false};
bool g_joinInstalled = false;
bool g_open = false;  // a dispatched batch the round-end join has not closed yet (game thread)

int g_dispatchStart = 0;  // the first job of the last dispatch (an appended round's jobs start after the earlier ones)

void Dispatch(int start)
{
    if (!g_helpersTried) StartHelpers();
    ++g_gen;
    g_dispatchStart = start;
    g_csr = _mm_getcsr();  // read by RunJob after its claim (the release store below publishes it)
    g_word.store((static_cast<uint64_t>(g_gen) << 32) | (static_cast<uint64_t>(start) << 16) | static_cast<uint64_t>(g_jobCount),
                 std::memory_order_release);
    if (g_sem && g_helpers > 0) ReleaseSemaphore(g_sem, std::min(g_helpers, g_jobCount - start), nullptr);
}

// Game thread: returns once job j is done, running unclaimed jobs meanwhile.
void WaitJob(Job& j)
{
    int spin = 0;
    uint64_t deadline = 0;
    while (j.state.load(std::memory_order_acquire) == 0) {
        const int i = Claim();
        if (i >= 0) { RunJob(g_jobs[i]); continue; }
        if (SpinPhase(spin, deadline)) continue;
        const int seen = 0;
        BlockOnWord(&j.state, &seen, sizeof seen);
    }
}

void WaitAll()
{
    int i;
    while ((i = Claim()) >= 0) RunJob(g_jobs[i]);
    for (int k = g_dispatchStart; k < g_jobCount; ++k) WaitJob(g_jobs[k]);
    g_word.store(static_cast<uint64_t>(g_gen) << 32, std::memory_order_release);
}

void AddJob(uint8_t* self, uint8_t* other)
{
    Job& j = g_jobs[g_jobCount++];
    j.self = self;
    j.other = other;
    j.state.store(0, std::memory_order_relaxed);
    j.ret = 0;
    j.out = kOutInit;
    j.used = false;
}

void Inc(std::atomic<uint64_t>& c, uint64_t n = 1) { c.fetch_add(n, std::memory_order_relaxed); }

// Diagnostics (game thread): each NPC vtable's filter (+0xA90) and eye point (+0xA98) targets, logged once per vtable.
constexpr size_t kFilterSlot = 0xA90, kEyesSlot = 0xA98;
const void* g_seenVt[16];
int g_seenCount = 0;
void NoteVtable(const uint8_t* self)
{
    if (g_seenCount >= 16) return;
    const uint8_t* const vt = Rd<const uint8_t*>(self, 0);
    for (int i = 0; i < g_seenCount; ++i)
        if (g_seenVt[i] == vt) return;
    g_seenVt[g_seenCount++] = vt;
    const GameModule& game = Game();
    if (!game.base || !game.rdata.Contains(vt)) {
        Log("[perclos] NPC vtable %p is outside the exe's read-only data", static_cast<const void*>(vt));
        return;
    }
    const uint8_t* const filter = Rd<const uint8_t*>(vt, kFilterSlot);
    const uint8_t* const eyes = Rd<const uint8_t*>(vt, kEyesSlot);
    Log("[perclos] NPC vtable exe+0x%X: filter (+0xA90) exe+0x%llX, eye point (+0xA98) exe+0x%llX",
        static_cast<unsigned>(vt - game.base), static_cast<unsigned long long>(filter - game.base),
        static_cast<unsigned long long>(eyes - game.base));
}

void AddWait(double t0)
{
    const uint64_t us = static_cast<uint64_t>(QpcUs() - t0);
    Inc(g_s.wallUs, us);
    if (us > g_s.maxWallUs.load(std::memory_order_relaxed)) g_s.maxWallUs.store(us, std::memory_order_relaxed);
}

// ---- the ignore-list cache ----
void Fill(TArr* out, const TArr& src)
{
    if (src.num <= 0) return;  // the game's gather leaves an empty array untouched when it finds nothing
    void** const d = static_cast<void**>(g_malloc(static_cast<size_t>(src.num) * sizeof(void*), 0));
    memcpy(d, src.data, static_cast<size_t>(src.num) * sizeof(void*));
    out->data = d;
    out->num = src.num;
    out->max = src.num;
}
bool SameArr(const TArr& a, const TArr& b)
{
    return a.num == b.num && (a.num == 0 || memcmp(a.data, b.data, static_cast<size_t>(a.num) * sizeof(void*)) == 0);
}
// Any thread: the NPC's gather of the open batched round, into an empty array.
bool CacheServes(const uint8_t* actor, const TArr* out)
{
    return g_cacheValid && actor == g_roundSelf && out && !out->data && out->num == 0 && out->max == 0;
}
void AttachedHook(uint8_t* actor, TArr* out, uint8_t flag)
{
    if (flag == 1 && CacheServes(actor, out)) {
        if (g_mode.load(std::memory_order_relaxed) == 2) {
            g_attached(actor, out, flag);
            Inc(g_s.cacheVerified);
            if (!SameArr(*out, g_cacheAtt)) Inc(g_s.cacheDiff);
            return;
        }
        Fill(out, g_cacheAtt);
        Inc(g_s.cacheServed);
        return;
    }
    g_attached(actor, out, flag);
}
void ChildHook(uint8_t* actor, TArr* out, uint8_t flag, uint8_t descend)
{
    if (flag == 1 && descend == 0 && CacheServes(actor, out)) {
        if (g_mode.load(std::memory_order_relaxed) == 2) {
            g_child(actor, out, flag, descend);
            Inc(g_s.cacheVerified);
            if (!SameArr(*out, g_cacheChild)) Inc(g_s.cacheDiff);
            return;
        }
        Fill(out, g_cacheChild);
        Inc(g_s.cacheServed);
        return;
    }
    g_child(actor, out, flag, descend);
}
// Game thread, before the batch: the NPC's two lists, gathered by the game's functions into emptied arrays.
void BuildCache(uint8_t* self)
{
    const double t0 = QpcUs();
    g_cacheAtt.num = 0;
    g_attached(self, &g_cacheAtt, 1);
    g_cacheChild.num = 0;
    g_child(self, &g_cacheChild, 1, 0);
    g_cacheValid = true;
    Inc(g_s.cacheRounds);
    Inc(g_s.cacheUs, static_cast<uint64_t>(QpcUs() - t0));
}

// The round-end join (the loop's exit, game thread): an open batch's remaining jobs are run or waited for; the round's
// ignore-list cache ends here (no gather after the round may see it).
void Join()
{
    if (g_open) {
        const double t0 = QpcUs();
        WaitAll();
        g_open = false;
        AddWait(t0);
    }
    g_cacheValid = false;
}

// ---- each NPC's round period (cross-round prediction; game thread) ----
Sched* SchedFor(uint8_t* npc, bool insert)
{
    size_t h = (reinterpret_cast<uintptr_t>(npc) >> 4) * 0x9E3779B97F4A7C15ull;
    for (int k = 0; k < kSched; ++k) {
        Sched& s = g_sched[(h + static_cast<size_t>(k)) & (kSched - 1)];
        if (s.npc == npc) return &s;
        if (!s.npc) {
            if (!insert) return nullptr;
            s.npc = npc;
            s.lastUs = 0;
            s.periodUs = 0;
            s.lastFrame = ~0ull;
            return &s;
        }
    }
    return nullptr;  // full: no prediction for this NPC
}
void NoteRound(uint8_t* npc, double now, uint64_t frame)
{
    if (frame != g_seenFrame) {
        if (frame == g_seenFrame + 1 && g_frameStartUs > 0) {
            const double dt = now - g_frameStartUs;
            if (dt > 1000.0 && dt < 500000.0) g_frameUs = 0.8 * g_frameUs + 0.2 * dt;
        }
        g_seenFrame = frame;
        g_frameStartUs = now;
    }
    Sched* const s = SchedFor(npc, true);
    if (!s) return;
    if (s->lastUs > 0) {
        const double p = now - s->lastUs;
        if (p > 20000.0 && p < 2000000.0) s->periodUs = s->periodUs > 0 ? 0.7 * s->periodUs + 0.3 * p : p;
    }
    s->lastUs = now;
    s->lastFrame = frame;
}
// Expected to run its round later in this frame: not yet run in it, and due before the frame's estimated end.
bool DueThisFrame(uint8_t* npc, double now, uint64_t frame)
{
    const Sched* const s = SchedFor(npc, false);
    return s && s->periodUs > 0 && s->lastFrame != frame && s->lastUs + s->periodUs <= now + g_frameUs;
}

const Range* FindRange(const uint8_t* self)
{
    for (int i = 0; i < g_rangeCount; ++i)
        if (g_ranges[i].self == self) return &g_ranges[i];
    return nullptr;
}

void DiscardBatch()
{
    for (int k = 0; k < g_jobCount; ++k)
        if (!g_jobs[k].used) Inc(g_s.unused);
    g_jobCount = 0;
    g_rangeCount = 0;
    g_batchFrame = ~0ull;
}

struct RoundIn {
    uint8_t* const* data;
    int n;
    const uint8_t* settings;
    const uint8_t* cls;
};

// One NPC's jobs, with the loop's checks (class, not itself, paired object, root in range, the field-of-view filter when
// its vtable holds it). Returns false (no jobs, no range) for an NPC whose filter is another one or without a root.
bool AddJobsFor(uint8_t* npc, const RoundIn& in, bool isRoundSelf)
{
    const uint8_t* const sroot = Rd<const uint8_t*>(npc, kRoot);
    if (!sroot || g_rangeCount >= kMaxRanges) return false;
    const uint8_t* const vt = Rd<const uint8_t*>(npc, 0);
    const FovFn fov = g_fov && vt && Rd<const void*>(vt, kFilterSlot) == g_fov ? reinterpret_cast<FovFn>(const_cast<void*>(g_fov)) : nullptr;
    // another filter (the player's, exe+0x4831C00: the camera's direction): its round runs as in the game. Precomputing it
    // would trace pawns its filter rejects (the 1.6 % of unasked checks of 2026-11-14 came from there); it is one actor.
    if (g_fov && !fov) {
        if (isRoundSelf) Inc(g_s.otherFilter);
        return false;
    }
    // another eye point than the one checked (our review): its round runs as in the game
    if (g_eye && (!vt || Rd<const void*>(vt, kEyesSlot) != g_eye)) {
        if (isRoundSelf) Inc(g_s.otherEye);
        return false;
    }
    const double sx = Rd<double>(sroot, kLoc), sy = Rd<double>(sroot, kLoc + 8), sz = Rd<double>(sroot, kLoc + 16);
    // the loop's range, in its float order: x6 = a; x1 = a; x6 *= c; x1 *= b; x6 *= b; x6 *= x1; compared as a double
    const float a = Rd<float>(in.settings, kRangeA), b = Rd<float>(in.settings, kRangeB);
    float x6 = a, x1 = a;
    x6 *= g_rangeConst;
    x1 *= b;
    x6 *= b;
    x6 *= x1;
    const double range = static_cast<double>(x6);
    const int begin = g_jobCount;
    for (int i = 0; i < in.n && g_jobCount < kMaxJobs; ++i) {
        uint8_t* const other = in.data[i];
        if (!other || !IsA(other, in.cls) || other == npc) continue;
        const uint8_t* const paired = Rd<const uint8_t*>(other, kPaired);
        if (!paired || !Rd<const void*>(paired, kPairedObj)) continue;
        const uint8_t* const oroot = Rd<const uint8_t*>(other, kRoot);
        if (!oroot) continue;  // the loop uses a default location then: not precomputed (the game's helper runs)
        const double dx = Rd<double>(oroot, kLoc) - sx, dy = Rd<double>(oroot, kLoc + 8) - sy, dz = Rd<double>(oroot, kLoc + 16) - sz;
        double d = dx * dx;
        d += dy * dy;
        d += dz * dz;
        if (!(d < range)) continue;
        if (fov && !fov(npc, other)) {  // out of the NPC's view: the loop never asks the helper for it
            Inc(g_s.outOfView);
            continue;
        }
        AddJob(npc, other);
    }
    g_ranges[g_rangeCount++] = {npc, begin, g_jobCount};
    return true;
}

void StartRound(uint8_t* self, const Range& r, bool cross)
{
    g_roundSelf = self;
    g_roundThread = GetCurrentThreadId();
    g_roundBegin = r.begin;
    g_roundEnd = r.end;
    g_cursor = r.begin;
    g_roundCross = cross;
}

// Called by the stub at the round's settings getter (game thread), after the getter: the NPC, its pawn list, the settings.
void Prefetch(uint8_t* self, const uint8_t* list, const uint8_t* settings)
{
    Join();  // never expected open here (the round's end closes it), but no batch may outlive its round
    g_roundSelf = nullptr;
    g_roundBegin = g_roundEnd = g_cursor = 0;
    g_roundCross = false;
    Inc(g_s.rounds);
    const double now = QpcUs();
    const uint64_t frame = g_frameFn();
    if (self) NoteRound(self, now, frame);
    // the frame counter must be running (NpcCreationPacing's frame hook): a counter that has not moved for 100 ms would let a
    // frame batch serve rounds of later frames
    const bool cross = g_cross.load(std::memory_order_relaxed) && frame != 0 && now - g_frameStartUs < 100000.0;
    if (g_mode.load(std::memory_order_relaxed) == 0 || !self || !list || !settings) {
        DiscardBatch();
        return;
    }
    // lightweight-instance guard (our review): with any manager in the engine's subsystem, the round runs as in the game
    if (g_lwiStatic) {
        const uint8_t* const sub = *g_lwiStatic;
        if (sub && Rd<int32_t>(sub, kLwiCount) != 0) {
            DiscardBatch();
            Inc(g_s.lwi);
            if (!g_lwiLogged) {
                g_lwiLogged = true;
                Log("[perclos] the engine lists lightweight-instance managers (%d): rounds run as in the game while it does",
                    Rd<int32_t>(sub, kLwiCount));
            }
            return;
        }
    }
    if (g_installed) NoteVtable(self);
    const bool sameFrame = cross && g_jobCount > 0 && g_batchFrame == frame;
    // a later round of this frame's batch: its jobs are done already
    if (sameFrame) {
        if (const Range* r = FindRange(self)) {
            StartRound(self, *r, true);
            Inc(g_s.crossRounds);
            if (g_cacheInstalled && g_joinInstalled && g_cacheOn.load(std::memory_order_relaxed)) BuildCache(self);
            return;
        }
    } else DiscardBatch();
    RoundIn in{Rd<uint8_t* const*>(list, 0), Rd<int>(list, 8), settings, nullptr};
    if (!in.data || in.n <= 0) return;
    in.cls = g_class();
    if (!in.cls) return;
    const int start = g_jobCount, rangesBefore = g_rangeCount;
    if (!AddJobsFor(self, in, true)) return;
    const Range selfRange = g_ranges[g_rangeCount - 1];
    if (cross)  // the NPCs expected later in this frame (the same pawn list: one class, one world)
        for (int i = 0; i < in.n && g_jobCount < kMaxJobs && g_rangeCount < kMaxRanges; ++i) {
            uint8_t* const p = in.data[i];
            if (!p || p == self || !IsA(p, in.cls) || FindRange(p) || !DueThisFrame(p, now, frame)) continue;
            if (AddJobsFor(p, in, false)) Inc(g_s.crossNpcs);
        }
    if (g_jobCount - start < std::max(1, g_min.load(std::memory_order_relaxed))) {  // not worth a batch
        g_jobCount = start;
        g_rangeCount = rangesBefore;
        return;
    }
    const double t0 = QpcUs();
    StartRound(self, selfRange, false);
    if (cross) g_batchFrame = frame;
    if (g_cacheInstalled && g_joinInstalled && g_cacheOn.load(std::memory_order_relaxed)) BuildCache(self);
    Dispatch(start);
    Inc(g_s.batches);
    Inc(g_s.jobs, static_cast<uint64_t>(g_jobCount - start));
    if (!cross && g_overlap.load(std::memory_order_relaxed) && g_joinInstalled) {
        g_open = true;
        Inc(g_s.overlapped);
    } else WaitAll();
    AddWait(t0);
}

// The loop's helper call site (redirected here): serve a precomputed answer, else the game's helper.
uint8_t HelperHook(uint8_t* self, uint8_t* other, uint8_t* out, int tries, uint8_t flag)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    const int n = g_roundEnd - g_roundBegin;
    if (mode != 0 && n > 0 && self == g_roundSelf && tries == kTries && flag == kFlag && out && *out == kOutInit &&
        GetCurrentThreadId() == g_roundThread) {
        Job* hit = nullptr;
        for (int k = 0; k < n && !hit; ++k) {
            Job& j = g_jobs[g_roundBegin + (g_cursor - g_roundBegin + k) % n];
            if (!j.used && j.other == other) hit = &j;
        }
        if (hit) {
            hit->used = true;
            g_cursor = g_roundBegin + static_cast<int>((hit - g_jobs) - g_roundBegin + 1) % n;
            if (g_open && hit->state.load(std::memory_order_acquire) == 0) {
                const double t0 = QpcUs();
                WaitJob(*hit);
                AddWait(t0);
            }
            if (mode == 2) {
                const uint8_t r = g_helper(self, other, out, tries, flag);
                const bool differs = r != hit->ret || *out != hit->out;
                Inc(g_s.verified);
                if (differs) Inc(g_s.diff);
                if (g_roundCross) {
                    Inc(g_s.crossVerified);
                    if (differs) Inc(g_s.crossDiff);
                }
                return r;
            }
            *out = hit->out;
            Inc(g_s.served);
            if (g_roundCross) Inc(g_s.crossServed);
            return hit->ret;
        }
    }
    Inc(g_s.fallback);
    return g_helper(self, other, out, tries, flag);
}

struct Sites {
    uint8_t *round, *settingsCall, *callSite, *helper, *settings, *classFn, *fov, *joinCall, *world;
    uint8_t *selfAttCall, *selfChildCall, *attachedFn, *childFn, *mallocFn;
    uint8_t *eye, *lwiStatic;
    float rangeConst;
};

// The field-of-view filter: its body, its forward-axis getter, its acos call and its end; nullptr if anything differs.
uint8_t* DiscoverFov(const GameModule& game)
{
    const auto f = FindAll(game.text, ParsePattern(kFov));
    if (f.size() != 1) return nullptr;
    uint8_t* const fn = f[0];
    const uint8_t* const getter = RipTarget(fn + kFovGetterCall, 1, 5);
    if (!game.text.Contains(getter) || !MatchesAt(getter, ParsePattern(kFovGetter))) return nullptr;
    if (fn[kFovAcosCall] != 0xE8 || !game.text.Contains(RipTarget(fn + kFovAcosCall, 1, 5))) return nullptr;
    if (!MatchesAt(fn + kFovTail, ParsePattern(kFovTailBytes))) return nullptr;
    return fn;
}

const char* Discover(const GameModule& game, Sites& s)
{
    const auto r = FindAll(game.text, ParsePattern(kRound));
    if (r.size() != 1) return "the perception round's list and settings calls not found exactly once";
    s.round = r[0];
    if (!MatchesAt(s.round + kRLoop, ParsePattern(kLoop))) return "the round's loop checks differ";
    if (!MatchesAt(s.round + kRSelfRoot, ParsePattern(kSelfRoot))) return "the NPC's own root load differs";
    if (!MatchesAt(s.round + kRCall, ParsePattern(kCall))) return "the line-of-sight call site differs";
    s.classFn = RipTarget(s.round + kRClassCall, 1, 5);
    if (RipTarget(s.round + kRLoop + kLClassCall, 1, 5) != s.classFn) return "the loop's class getter is another function";
    s.settingsCall = s.round + kRSettingsCall;
    s.settings = RipTarget(s.settingsCall, 1, 5);
    const uint8_t* const cst = RipTarget(s.round + kRConst, 4, 8);
    if (!game.rdata.Contains(cst) && !game.data.Contains(cst)) return "the range constant is not in the exe's data";
    memcpy(&s.rangeConst, cst, 4);
    s.callSite = s.round + kRCall + kCCall;
    s.helper = RipTarget(s.callSite, 1, 5);
    if (!game.text.Contains(s.helper) || !MatchesAt(s.helper, ParsePattern(kHelper))) return "the line-of-sight helper differs";
    if (!game.text.Contains(s.settings) || !game.text.Contains(s.classFn)) return "a getter is outside the exe's code";
    // the lightweight-instance guard (required, our review): the helper's three hit-actor calls reach one getter, whose slow
    // path gets the subsystem (its static, read twice) and fetches through the manager lookup (its list read)
    uint8_t* getActor = nullptr;
    for (const size_t off : kHGetActorCalls) {
        if (s.helper[off] != 0xE8) return "the helper's hit-actor calls differ";
        uint8_t* const t = RipTarget(s.helper + off, 1, 5);
        if (getActor && t != getActor) return "the helper's hit-actor calls reach different functions";
        getActor = t;
    }
    if (!game.text.Contains(getActor) || !MatchesAt(getActor, ParsePattern(kGetActor))) return "the hit's actor getter differs";
    uint8_t* const lwiGet = RipTarget(getActor + kGaSubsystemCall, 1, 5);
    uint8_t* const lwiFetch = RipTarget(getActor + kGaFetchJmp, 1, 5);
    if (!game.text.Contains(lwiGet) || !MatchesAt(lwiGet, ParsePattern(kLwiGet)) || !game.text.Contains(lwiFetch) ||
        !MatchesAt(lwiFetch, ParsePattern(kLwiFetch)))
        return "the lightweight-instance subsystem's getter or actor fetch differs";
    uint8_t* const lwiStatic = RipTarget(lwiGet + kLgStatic, 3, 7);
    if (lwiStatic != RipTarget(lwiGet + kLgStatic2, 3, 8) || !game.data.Contains(lwiStatic))
        return "the lightweight-instance subsystem's static differs";
    uint8_t* const lwiFind = RipTarget(lwiFetch + kLfFindCall, 1, 5);
    if (!game.text.Contains(lwiFind) || !MatchesAt(lwiFind + kLwiFindListAt, ParsePattern(kLwiFindList)))
        return "the lightweight-instance manager lookup differs";
    s.lwiStatic = lwiStatic;
    // the NPC eye point the helper calls (required, our review): the call through vtable +0xA98, then its target
    if (!MatchesAt(s.helper + kHEyeCall, ParsePattern(kEyeCall))) return "the helper's eye-point call differs";
    const auto eyes = FindAll(game.text, ParsePattern(kEye));
    if (eyes.size() != 1) return "the NPC eye point not found exactly once";
    s.eye = eyes[0];
    s.fov = DiscoverFov(game);  // optional: without it every pawn in range is precomputed
    // optional: the round-end join (without it the overlap stays off: the round's start waits for the whole batch)
    uint8_t* const join = s.round + kRJoin;
    if (MatchesAt(join, ParsePattern(kJoin)) && RipTarget(s.round + kREmptyJump, 2, 6) == join) {
        s.joinCall = join + kJCall;
        s.world = RipTarget(s.joinCall, 1, 5);
        if (!game.text.Contains(s.world)) s.joinCall = s.world = nullptr;
    }
    // optional: the helper's NPC gathers and FMemory::Malloc (without them PerceptionLosIgnoreCache stays off)
    uint8_t* const g = s.helper + kHGather;
    if (MatchesAt(g, ParsePattern(kGather))) {
        uint8_t* const att = RipTarget(g + kGSelfAtt, 1, 5);
        uint8_t* const child = RipTarget(g + kGSelfChild, 1, 5);
        const auto mallocs = FindAllCallable(game.text, ParsePattern(kMalloc), "FMemory::Malloc", kMallocG1);
        bool sameG = !mallocs.empty();
        for (uint8_t* m : mallocs)
            sameG = sameG && RipTarget(m + kMallocG1, 3, 7) == RipTarget(mallocs[0] + kMallocG1, 3, 7) &&
                    RipTarget(m + kMallocG2, 3, 7) == RipTarget(mallocs[0] + kMallocG1, 3, 7);
        if (att == RipTarget(g + kGOtherAtt, 1, 5) && child == RipTarget(g + kGOtherChild, 1, 5) && game.text.Contains(att) &&
            game.text.Contains(child) && sameG) {
            s.selfAttCall = g + kGSelfAtt;
            s.selfChildCall = g + kGSelfChild;
            s.attachedFn = att;
            s.childFn = child;
            s.mallocFn = mallocs[0];
        }
    }
    return nullptr;
}

bool PatchCall(uint8_t* site, const uint8_t* to)
{
    const int64_t rel = to - (site + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t bytes[5] = {0xE8};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(bytes + 1, &rel32, 4);
    return WriteCode(site, bytes);
}

uint64_t g_statFrame = 0;
const char* ModeText(int mode)
{
    return mode == 1 ? "a perception round's line-of-sight checks are precomputed on helper threads and served in the game's order"
         : mode == 2 ? "verify: the game's line-of-sight helper also runs and is compared with the precomputed answer"
                     : "every line-of-sight check runs on the game thread, as in the game";
}

}  // namespace

// Self-test: the RVAs of the round match, the call site and the helper (0 = not found); no writes.
const char* PerceptionLosTestDiscover(const GameModule& game, uint32_t* round, uint32_t* callSite, uint32_t* helper, uint32_t* fov)
{
    Sites s{};
    const char* why = Discover(game, s);
    if (!why) {
        if (round) *round = static_cast<uint32_t>(s.round - game.base);
        if (callSite) *callSite = static_cast<uint32_t>(s.callSite - game.base);
        if (helper) *helper = static_cast<uint32_t>(s.helper - game.base);
        if (fov) *fov = s.fov ? static_cast<uint32_t>(s.fov - game.base) : 0;
    }
    return why;
}

bool InstallPerceptionLosPrefetch(bool beforeGameCode, int threads, bool ignoreCache)
{
    if (g_installed) return true;
    if (!beforeGameCode) {
        Log("[perclos] PerceptionLosPrefetch: only installed when the game starts (loaded late here): not installed");
        return false;
    }
    const GameModule& game = Game();
    Sites s{};
    if (const char* why = Discover(game, s)) {
        Log("[perclos] %s: PerceptionLosPrefetch not installed", why);
        return false;
    }
    uint8_t* const region = AllocNearGame(192);
    if (!region) { Log("[perclos] no memory near the game's code: PerceptionLosPrefetch not installed"); return false; }
    // region +0: the round stub (called instead of the settings getter; the caller's frame is at our entry rsp + 8)
    //   sub rsp,28h; mov rax,getter; call rax; mov [rsp+20h],rax; mov rcx,rdi; lea rdx,[rsp+90h]; mov r8,rax;
    //   mov rax,Prefetch; call rax; mov rax,[rsp+20h]; add rsp,28h; ret
    // region +64: jmp [rip+0]; the helper hook's address
    uint8_t* p = region;
    auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *p++ = x; };
    auto put64 = [&](const void* v) { memcpy(p, &v, 8); p += 8; };
    put({0x48, 0x83, 0xEC, 0x28});
    put({0x48, 0xB8}); put64(s.settings);
    put({0xFF, 0xD0});
    put({0x48, 0x89, 0x44, 0x24, 0x20});
    put({0x48, 0x8B, 0xCF});
    put({0x48, 0x8D, 0x94, 0x24, 0x90, 0x00, 0x00, 0x00});
    put({0x4C, 0x8B, 0xC0});
    put({0x48, 0xB8}); put64(reinterpret_cast<const void*>(&Prefetch));
    put({0xFF, 0xD0});
    put({0x48, 0x8B, 0x44, 0x24, 0x20});
    put({0x48, 0x83, 0xC4, 0x28});
    put({0xC3});
    uint8_t* const jmp = region + 64;
    p = jmp;
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    put64(reinterpret_cast<const void*>(&HelperHook));
    // region +80: the join stub (called instead of the world getter at the loop's exit; rcx = the NPC, kept for the getter)
    //   push rcx; sub rsp,20h; mov rax,Join; call rax; add rsp,20h; pop rcx; mov rax,getter; jmp rax
    uint8_t* const joinStub = region + 80;
    if (s.joinCall) {
        p = joinStub;
        put({0x51});
        put({0x48, 0x83, 0xEC, 0x20});
        put({0x48, 0xB8}); put64(reinterpret_cast<const void*>(&Join));
        put({0xFF, 0xD0});
        put({0x48, 0x83, 0xC4, 0x20});
        put({0x59});
        put({0x48, 0xB8}); put64(s.world);
        put({0xFF, 0xE0});
    }
    // region +128 / +144: jmp [rip+0] to the two gather hooks
    uint8_t* const attJmp = region + 128;
    uint8_t* const childJmp = region + 144;
    if (s.selfAttCall) {
        p = attJmp;
        put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
        put64(reinterpret_cast<const void*>(&AttachedHook));
        p = childJmp;
        put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
        put64(reinterpret_cast<const void*>(&ChildHook));
    }
    DWORD old;
    if (!VirtualProtect(region, 192, PAGE_EXECUTE_READ, &old)) {
        FreeOwnCode(region);
        Log("[perclos] could not make the stubs executable: PerceptionLosPrefetch not installed");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), region, 192);
    g_helper = reinterpret_cast<HelperFn>(s.helper);
    g_class = reinterpret_cast<ClassFn>(s.classFn);
    g_rangeConst = s.rangeConst;
    g_fov = s.fov;
    g_eye = s.eye;
    g_lwiStatic = reinterpret_cast<uint8_t* const*>(s.lwiStatic);
    g_threads = std::clamp(threads, 0, 8);
    // the call site first: alone it only forwards to the game's helper (no round state yet)
    if (!PatchCall(s.callSite, jmp)) { Log("[perclos] could not patch the line-of-sight call: PerceptionLosPrefetch not installed"); return false; }
    // the join next: alone it finds nothing open
    if (s.joinCall && PatchCall(s.joinCall, joinStub)) g_joinInstalled = true;
    else Log("[perclos] the round's exit %s: PerceptionLosPrefetchOverlap stays off", s.joinCall ? "could not be patched" : "differs");
    // the helper's two NPC gathers (alone they find no valid cache and forward), only with the join (which ends the cache)
    if (g_joinInstalled && s.selfAttCall && ignoreCache) {
        g_attached = reinterpret_cast<AttachedFn>(s.attachedFn);
        g_child = reinterpret_cast<ChildFn>(s.childFn);
        g_malloc = reinterpret_cast<MallocFn>(s.mallocFn);
        if (PatchCall(s.selfAttCall, attJmp) && PatchCall(s.selfChildCall, childJmp)) g_cacheInstalled = true;
        else Log("[perclos] could not patch the helper's gathers: PerceptionLosIgnoreCache stays off");
    } else if (ignoreCache && !s.selfAttCall) Log("[perclos] the helper's gathers differ: PerceptionLosIgnoreCache stays off");
    if (!PatchCall(s.settingsCall, region)) {
        Log("[perclos] could not patch the round's settings call (the call site forwards only): PerceptionLosPrefetch not installed");
        return false;
    }
    g_installed = true;
    Log("[perclos] perception round exe+0x%X (settings call exe+0x%X), line-of-sight call exe+0x%X -> helper exe+0x%X hooked; "
        "range constant %g; field-of-view filter %s exe+0x%X; eye point exe+0x%X; lightweight-instance guard exe+0x%X; round-end "
        "join %s; ignore-list cache %s; %d helper threads (on first use); live",
        static_cast<unsigned>(s.round - game.base), static_cast<unsigned>(s.settingsCall - game.base),
        static_cast<unsigned>(s.callSite - game.base), static_cast<unsigned>(s.helper - game.base), static_cast<double>(s.rangeConst),
        s.fov ? "found at" : "not found (every pawn in range is precomputed); expected at",
        s.fov ? static_cast<unsigned>(s.fov - game.base) : 0x485A900u, static_cast<unsigned>(s.eye - game.base),
        static_cast<unsigned>(s.lwiStatic - game.base), g_joinInstalled ? "hooked" : "not hooked",
        g_cacheInstalled ? "hooked" : "not hooked", g_threads);
    g_statFrame = NpcPacingFrame();
    return true;
}

// Live (monitor thread): 0 off, 1 serve, 2 verify; no-op until installed. Overlap: hidden, live (read by the game thread at
// each round's start).
void SetPerceptionLosPrefetch(int mode, int minJobs, bool helpersAllowed, bool overlap, bool ignoreCache, bool crossRound)
{
    if (!g_installed) return;
    if (g_cross.load() != crossRound) {
        g_cross = crossRound;
        Log("[perclos] PerceptionLosCrossRound=%d (experimental: the first round of a frame also precomputes the NPCs expected later "
            "in it)", crossRound ? 1 : 0);
    }
    if ((g_cacheOn.load() != 0) != ignoreCache) {
        g_cacheOn = ignoreCache ? 1 : 0;
        Log("[perclos] PerceptionLosIgnoreCache=%d%s", ignoreCache ? 1 : 0, ignoreCache && !g_cacheInstalled ? " (not hooked: stays off)" : "");
    }
    mode = mode == 1 ? 1 : mode == 2 ? 2 : 0;
    if (!helpersAllowed) mode = 0;
    g_min = std::clamp(minJobs, 1, kMaxJobs);
    if (g_overlap.load() != overlap) {
        g_overlap = overlap;
        Log("[perclos] PerceptionLosPrefetchOverlap=%d%s", overlap ? 1 : 0, overlap && !g_joinInstalled ? " (no round-end join: stays off)" : "");
    }
    if (g_mode.load() == mode) return;
    g_mode = mode;
    Log("[perclos] PerceptionLosPrefetch=%d: %s%s", mode, ModeText(mode), helpersAllowed ? "" : " (ParallelHelpersAllowed: no)");
}

void LogPerceptionLosPrefetchStats()
{
    if (!g_installed) return;
    const uint64_t frame = NpcPacingFrame();
    const double frames = static_cast<double>(frame - g_statFrame);
    g_statFrame = frame;
    const uint64_t rounds = g_s.rounds.exchange(0), batches = g_s.batches.exchange(0), jobs = g_s.jobs.exchange(0),
                   served = g_s.served.exchange(0), verified = g_s.verified.exchange(0), diff = g_s.diff.exchange(0),
                   fallback = g_s.fallback.exchange(0), unused = g_s.unused.exchange(0), wallUs = g_s.wallUs.exchange(0),
                   maxWallUs = g_s.maxWallUs.exchange(0), outOfView = g_s.outOfView.exchange(0),
                   otherFilter = g_s.otherFilter.exchange(0), overlapped = g_s.overlapped.exchange(0),
                   lwi = g_s.lwi.exchange(0), otherEye = g_s.otherEye.exchange(0);
    const uint64_t cacheRounds = g_s.cacheRounds.exchange(0), cacheUs = g_s.cacheUs.exchange(0),
                   cacheServed = g_s.cacheServed.exchange(0), cacheVerified = g_s.cacheVerified.exchange(0),
                   cacheDiff = g_s.cacheDiff.exchange(0);
    if (cacheRounds)
        Log("[perclos] ignore-list cache: %llu rounds (the NPC's two gathers took %.2f us per round on the game thread), %llu gathers "
            "served from it, %llu verified (%llu different)",
            (unsigned long long)cacheRounds, static_cast<double>(cacheUs) / static_cast<double>(cacheRounds),
            (unsigned long long)cacheServed, (unsigned long long)cacheVerified, (unsigned long long)cacheDiff);
    const uint64_t crossNpcs = g_s.crossNpcs.exchange(0), crossRounds = g_s.crossRounds.exchange(0),
                   crossServed = g_s.crossServed.exchange(0), crossVerified = g_s.crossVerified.exchange(0),
                   crossDiff = g_s.crossDiff.exchange(0);
    if (crossNpcs || crossRounds)
        Log("[perclos] cross-round: %llu NPCs precomputed ahead (frame estimate %.1f ms), %llu rounds took their checks from an earlier "
            "round's batch: %llu served, %llu verified (%llu different)",
            (unsigned long long)crossNpcs, g_frameUs / 1000.0, (unsigned long long)crossRounds, (unsigned long long)crossServed,
            (unsigned long long)crossVerified, (unsigned long long)crossDiff);
    if (!rounds) return;
    Log("[perclos] since last report (%.0f frames): %llu perception rounds (%llu with another filter, %llu another eye point, %llu "
        "with lightweight-instance managers: as in the game), %llu batched "
        "(%llu overlapped with the loop) "
        "(%llu checks precomputed, %llu pawns out of view skipped, %.1f per batch, game thread blocked %.2f ms in total, max %.3f ms), "
        "%llu served, %llu verified (%llu different), %llu ran as in the game, %llu precomputed but not asked (PerceptionLosPrefetch=%d)",
        frames, (unsigned long long)rounds, (unsigned long long)otherFilter, (unsigned long long)otherEye, (unsigned long long)lwi,
        (unsigned long long)batches,
        (unsigned long long)overlapped, (unsigned long long)jobs,
        (unsigned long long)outOfView,
        batches ? static_cast<double>(jobs) / static_cast<double>(batches) : 0.0, static_cast<double>(wallUs) / 1000.0,
        static_cast<double>(maxWallUs) / 1000.0, (unsigned long long)served, (unsigned long long)verified, (unsigned long long)diff,
        (unsigned long long)fallback, (unsigned long long)unused, g_mode.load());
}

// ---- self-test: the batch and the serve with a fake helper and fake pawns ----
void PerceptionLosTestSetup(void* fakeHelper, void* fakeClass, void* fakeFov, float rangeConst, int mode, int minJobs, int threads)
{
    g_helper = reinterpret_cast<HelperFn>(fakeHelper);
    g_class = reinterpret_cast<ClassFn>(fakeClass);
    g_fov = fakeFov;
    g_rangeConst = rangeConst;
    g_mode = mode;
    g_min = minJobs;
    g_threads = threads;
}
void PerceptionLosTestPrefetch(uint8_t* self, const uint8_t* list, const uint8_t* settings) { Prefetch(self, list, settings); }
uint8_t PerceptionLosTestCall(uint8_t* self, uint8_t* other, uint8_t* out, int tries, uint8_t flag)
{
    return HelperHook(self, other, out, tries, flag);
}
void PerceptionLosTestCounts(uint64_t* served, uint64_t* verified, uint64_t* diff, uint64_t* fallback, uint64_t* jobs)
{
    if (served) *served = g_s.served.load();
    if (verified) *verified = g_s.verified.load();
    if (diff) *diff = g_s.diff.load();
    if (fallback) *fallback = g_s.fallback.load();
    if (jobs) *jobs = g_s.jobs.load();
}
// Self-test (our review): the eye point's and the lightweight-instance static's RVAs (0 = not found); no writes.
void PerceptionLosTestGuardSites(const GameModule& game, uint32_t* eye, uint32_t* lwiStatic)
{
    Sites s{};
    *eye = *lwiStatic = 0;
    if (Discover(game, s)) return;
    *eye = static_cast<uint32_t>(s.eye - game.base);
    *lwiStatic = static_cast<uint32_t>(s.lwiStatic - game.base);
}
// Self-test: a stand-in subsystem static (nullptr = none) for the guard.
void PerceptionLosTestSetLwi(void* fakeStatic) { g_lwiStatic = static_cast<uint8_t* const*>(fakeStatic); }
// Self-test: a stand-in eye point (nullptr = not checked).
void PerceptionLosTestSetEye(const void* fakeEye) { g_eye = fakeEye; }
uint64_t PerceptionLosTestOtherEyeRounds() { return g_s.otherEye.load(); }
uint64_t PerceptionLosTestLwiRounds() { return g_s.lwi.load(); }
// Self-test: the round-end join's call site RVA (0 = not found); no writes.
uint32_t PerceptionLosTestJoinSite(const GameModule& game)
{
    Sites s{};
    if (Discover(game, s) || !s.joinCall) return 0;
    return static_cast<uint32_t>(s.joinCall - game.base);
}
// Self-test: the helper's two NPC gather call sites' RVAs (0 = not found); no writes.
void PerceptionLosTestGatherSites(const GameModule& game, uint32_t* att, uint32_t* child)
{
    Sites s{};
    *att = *child = 0;
    if (Discover(game, s) || !s.selfAttCall) return;
    *att = static_cast<uint32_t>(s.selfAttCall - game.base);
    *child = static_cast<uint32_t>(s.selfChildCall - game.base);
}
// Self-test: the cache with fake gathers and a fake malloc (the join must be on: SetOverlap's stand-in installs it).
void PerceptionLosTestSetCache(void* fakeAttached, void* fakeChild, void* fakeMalloc, bool on)
{
    g_attached = reinterpret_cast<AttachedFn>(fakeAttached);
    g_child = reinterpret_cast<ChildFn>(fakeChild);
    g_malloc = reinterpret_cast<MallocFn>(fakeMalloc);
    g_cacheInstalled = on;
    g_cacheOn = on ? 1 : 0;
}
void PerceptionLosTestGather(uint8_t* actor, void* out, bool child)
{
    if (child) ChildHook(actor, static_cast<TArr*>(out), 1, 0);
    else AttachedHook(actor, static_cast<TArr*>(out), 1);
}
void PerceptionLosTestCacheCounts(uint64_t* served, uint64_t* verified, uint64_t* diff)
{
    *served = g_s.cacheServed.load();
    *verified = g_s.cacheVerified.load();
    *diff = g_s.cacheDiff.load();
}
// Self-test: the overlap with a stand-in for the round-end hook; returns whether a batch was still open before the join.
void PerceptionLosTestSetOverlap(bool on) { g_overlap = on; g_joinInstalled = on; }
// Self-test: cross-round with a stand-in frame counter (nullptr = the real one).
void PerceptionLosTestSetCross(bool on, uint64_t (*frameFn)())
{
    g_cross = on;
    g_frameFn = frameFn ? frameFn : &NpcPacingFrame;
    memset(g_sched, 0, sizeof g_sched);
    g_seenFrame = ~0ull;
    g_frameStartUs = 0;
    g_frameUs = 16667.0;
}
void PerceptionLosTestCrossCounts(uint64_t* npcs, uint64_t* rounds, uint64_t* served)
{
    *npcs = g_s.crossNpcs.load();
    *rounds = g_s.crossRounds.load();
    *served = g_s.crossServed.load();
}
bool PerceptionLosTestJoin()
{
    const bool wasOpen = g_open;
    Join();
    return wasOpen;
}
}  // namespace opf
