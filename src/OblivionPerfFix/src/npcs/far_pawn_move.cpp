// FarPawnMoveHalfRate / FarPawnMoveHz / FarHorseMove (lab farmove + fartick 2026-11-01, played by the maintainer since; public since
// 1.3.0, worker port130j 2026-11-08 after review rev130e; [Fixes] FarPawnMoveHalfRate, default 1; FarPawnMoveHz, default 30;
// FarHorseMove, default 2 (user 2026-11-08: on by default at 30 Hz, switchable off). Installed at start-up when either key is
// not 0 (version.dll route; needs NpcCreationPacing for the player's position), then live.
//
// NOT identical: far calm NPCs and horses move in small steps. The character movement tick of a calm pawn at least 100 m
// from the player (FarPawnMoveMinDistance, UE units, not in the shipped ini) runs at FarPawnMoveHz (30 = every second frame
// at 60 fps) with the skipped frames' time steps summed into the next tick: what every pawn gets on a PC running at that
// frame rate. Positions and arrivals of those pawns are quantised to the 30 Hz step; nothing else about them changes.
// 1 = calm far pawns at walking speed or slower (horizontal speed <= 300 units/s), 2 = at any speed; 0 = off.
// FarHorseMove: 0 off, 1 far riderless horses, 2 also far horses with an NPC rider sitting on them (walking pace unless
// FarPawnMoveHalfRate=2).
//
// Calm = the pawn's AI procedure (Altar's GetCurrentProcedureType, called as the game calls it: the controller's class is
// checked) is TRAVEL 1, WANDER 2, SLEEP 5, EAT 6, WAIT 14 or DO_NOTHING 26 (enum values from the exe's UEnum table, rev130e).
// Follow, escort, combat, dialogue, flee, pursue, accompany, mount procedures etc. tick every frame; so do swimming pawns
// (MovementMode MOVE_Swimming), attached pawns (a mounted rider), a horse whose rider is not on it (mounting, dismounting)
// or whose rider or own controller is not calm, and pawns without an AI controller (the player).
//
// Root motion (rev130e F-M1): a pawn playing root motion (ACharacter::IsPlayingRootMotion, the game's own function, found
// as the call in CMC TickComponent) is never throttled and ticks with its own frame's time only: on a skipped frame the
// mesh's own tick advances the pose (it runs after the movement tick by prerequisite, and the movement tick did not tick
// the pose that frame), so a tick with the summed step would advance a montage twice (1.5x speed and root-motion distance
// at 30 Hz / 60 fps). Carried time is kept for the first tick after the root motion ends (it is movement time).
// Input (rev130e F-S1): inputs added on skipped frames (APawn::ControlInputVector, path following's acceleration) sum up
// until the movement tick consumes them; before a tick that follows k-1 skipped frames the sum is divided by k, so the
// tick sees the frames' mean input: the velocity integral (mean x summed step) equals the per-frame one.
// Cadence: a qualifying pawn ticks once its carried time reaches the period minus half a frame (30 Hz at 60 fps = exactly
// every second frame; 45-60 fps every second frame, steps 34-44 ms; below 45 fps every frame; 90 fps every third). A pawn
// that starts qualifying gets its first tick at a phase from its address (F-S2: compared without the half frame, so pawns
// that start in the same frame split 50/50 over the two frames at 30 Hz / 60 fps). FarPawnMoveMaxStepMs (400 = UE's
// MaxSimulationIterations 8 x MaxSimulationTimeStep 50 ms; not in the shipped ini) bounds every carried step. A pawn that
// stops qualifying ticks at once with all carried time added. An entry stays the same component while its CharacterOwner
// matches; carried time is never dropped for time (F-S5): a long pause keeps it (a slot of a component unseen for 60 s
// may be reused; one unseen for 10 s only when it carries nothing).
// Not changed by the cadence (F-S6, named in docs): forces a pawn applies per movement tick (standing on / pushing a
// simulated physics object: ApplyDownwardForce / ApplyRepulsionForce) are applied on half the frames at 30 Hz, so a far
// calm pawn pushes such an object with about half the impulse; RVO avoidance (unused by Altar's NPCs as far as seen)
// would update at 30 Hz.
//
// How: VPairedPawnMovementComponent's own vtable and VHorseMovementComponent's (found from their constructors: the
// paired constructor by pattern, the horse constructor as the one of four pattern hits that calls it) have slot 127 =
// UCharacterMovementComponent::TickComponent (pattern, checked in both); that slot is redirected in both vtables. Field
// offsets are pinned by the game's code (CMC TickComponent's reads of CharacterOwner / Mesh / UpdatedComponent; the
// vtable's IsSwimming = MovementMode and MOVE_Swimming; UpdateComponentVelocity = Velocity; GetCurrentProcedureType =
// Controller and the procedure byte; AttachParent = USceneComponent's execGetAttachParent, the one native thunk of that
// name, reached through its {name, thunk} table entry) or by the exe's generated property tables (Rider,
// ControlInputVector, RootComponent; NpcCreationPacing's pattern-found actor location). Any mismatch: not installed,
// one log line (horses alone: the horse part only). rev130m FP-S1: AttachParent used to be pinned by the property
// tables alone, where three declarations have that name (0xC8, 0x0, 0x98) and any one at 0xC8 passed.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace opf {
namespace {

// VPairedPawnMovementComponent's constructor (exe+0x47EF250 on 19115871): vtable store = lea rax,[vtable] at +0x29.
constexpr char kPairedCtor[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 33 F6 66 C7 87 ?? ?? ?? ?? 01 00 "
                               "C6 87 ?? ?? ?? ?? 01 48 8D 05 ?? ?? ?? ?? 48 89 07";
constexpr size_t kPairedVtLea = 0x29;
// VHorseMovementComponent's constructor (exe+0x47EDFF0): calls the paired one at +0x14, vtable lea at +0x19 (4 hits; the
// one calling the paired constructor is it).
constexpr char kHorseCtor[] = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B D0 48 8B CB E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? "
                              "C6 83 ?? ?? ?? ?? 00 48 89 03";
constexpr size_t kHorseCtorCall = 0x14, kHorseVtLea = 0x19;
// UCharacterMovementComponent::TickComponent (exe+0x3004340), slot 127 of both vtables.
constexpr char kTick[] = "4C 8B DC 55 56 49 8D 6B C8 48 81 EC 28 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 E0 48 8B 81 A8 00 00 00";
constexpr int kSlot = 127;
// In TickComponent's async-tick branch: CharacterOwner (+0x180) -> Mesh (+0x320) -> ShouldTickPose, then
// CharacterOwner->IsPlayingRootMotion() (call at +0x29).
constexpr char kTickRootMotion[] = "48 8B 83 80 01 00 00 48 8B B8 20 03 00 00 48 8B CF 48 8B 07 FF 90 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? "
                                   "48 8B 8B 80 01 00 00 E8";
constexpr size_t kTickRootMotionCall = 0x29;
// ACharacter::IsPlayingRootMotion: Mesh (+0x320) ? Mesh->IsPlayingRootMotion() : false.
constexpr char kIsRootMotion[] = "48 8B 89 20 03 00 00 48 85 C9 74 ?? 48 8B 01 48 FF A0 ?? ?? ?? ?? 32 C0 C3";
// In TickComponent: UpdatedComponent (+0xB8)->IsSimulatingPhysics(), then CharacterOwner (+0x180).
constexpr char kTickUpdated[] = "48 8B 8B B8 00 00 00 49 8B D4 4C 89 64 24 20 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B 8B 80 01 00 00";
constexpr size_t kTickBodySpan = 0x800;  // both must lie in TickComponent
// The CMC vtable's IsSwimming: MovementMode (+0x221) == MOVE_Swimming (4) && UpdatedComponent (+0xB8).
constexpr char kIsSwimming[] = "80 B9 21 02 00 00 04 75 0D 48 83 B9 B8 00 00 00 00 74 03 B0 01 C3 32 C0";
// The CMC vtable's UpdateComponentVelocity: UpdatedComponent (+0xB8)->ComponentVelocity = Velocity (+0xD0).
constexpr char kUpdateVelocity[] = "48 8B 81 B8 00 00 00 48 85 C0 74 1E 0F 10 89 D0 00 00 00 F2 0F 10 81 E0 00 00 00 0F 11 88 ?? ?? ?? ?? "
                                   "F2 0F 11 80 ?? ?? ?? ?? C3";
constexpr int kVtScan = 400;  // vtable slots searched for the two getters
// Altar's GetCurrentProcedureType(pawn) (exe+0x4856F60), from +6: Controller (+0x2D0), IsA(VPairedPawnAIController) and
// the flags check, then the procedure byte (+0x448).
constexpr char kGetProc[] = "48 8B 99 D0 02 00 00 48 85 DB 74 ?? E8 ?? ?? ?? ?? 48 8B 4B 10 48 83 C0 30 48 63 50 08 3B 51 38 7F ?? 48 8B "
                            "49 30 48 39 04 D1 75 ?? 8B 4B 08 F7 C1 00 00 00 60 75 ?? 0F B6 83 48 04 00 00";
constexpr size_t kGetProcBody = 6;
constexpr uint8_t kGetProcPrologue[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
// USceneComponent's execGetAttachParent (exe+0x2D90D00): P_FINISH (Stack.Code += !!Stack.Code), then
// *(USceneComponent**)Result = Context->AttachParent: mov rax,[rcx+0C8h]; mov [r8],rax; ret. Its disp32 (at +0x18) is
// the offset the fix reads.
constexpr char kExecGetAttachParent[] = "48 8B 42 20 45 33 C9 48 85 C0 41 0F 95 C1 4C 03 C8 4C 89 4A 20 48 8B 81 ?? ?? ?? ?? 49 89 00 C3";
constexpr size_t kExecAttachDisp = 0x18;

constexpr uint32_t kOwner = 0x180, kUpdated = 0xB8, kVelocity = 0xD0, kMoveMode = 0x221, kController = 0x2D0, kMesh = 0x320;
constexpr uint32_t kAttachParent = 0xC8, kRider = 0xE30, kInput = 0x2E8;
constexpr uint8_t kMoveSwimming = 4;
constexpr uint64_t kCalmMask = (1ull << 1) | (1ull << 2) | (1ull << 5) | (1ull << 6) | (1ull << 14) | (1ull << 26);
constexpr double kRiderOnHorse = 300.0;  // an NPC rider within 3 m of its horse sits on it

using TickFn = void (*)(void*, float, int, void*);
using ProcFn = uint8_t (*)(const void* pawn);
using BoolFn = bool (*)(const void* character);

struct Offsets {
    uint32_t root = 0, location = 0;  // NpcCreationPacing's (pattern-found): AActor::RootComponent, the component location
};
Offsets g_off;
void* g_orig = nullptr;
void* g_origHorse = nullptr;
ProcFn g_proc = nullptr;
BoolFn g_rootMotion = nullptr;
bool g_installed = false, g_horseInstalled = false;
std::atomic<int> g_mode{0}, g_hz{30}, g_horseMode{0};
double g_minDist = 10000.0;
float g_maxStep = 0.4f;
bool (*g_playerPos)(double*, double*, double*) = &NpcPacingPlayerPos;  // self-test: a fake

std::atomic<uint64_t> g_calls{0}, g_qualified{0}, g_skipped{0}, g_overMax{0}, g_rootKept{0}, g_inputs{0};
std::atomic<uint64_t> g_hCalls{0}, g_hQualified{0}, g_hSkipped{0}, g_hRidden{0}, g_swimKept{0};

struct Entry {
    void* comp;
    const void* owner;
    FarPawnStep s;
    int64_t lastQpc;
};
constexpr int kEntries = 2048;
Entry g_tab[kEntries] = {};
int64_t g_qpcFreq = 1;

float Phase(const void* comp)  // 0..1 from the address: spreads the first ticks of new entries over one period
{
    const uint64_t h = (reinterpret_cast<uintptr_t>(comp) >> 4) * 0x9E3779B97F4A7C15ull;
    return static_cast<float>((h >> 40) & 0xFFFF) / 65536.f;
}

// Game thread only (the caller checked NpcPacingPlayerPos, which answers only there).
Entry* Find(void* comp, const void* owner, int64_t now)
{
    const uint64_t h = (reinterpret_cast<uintptr_t>(comp) >> 4) * 0x9E3779B97F4A7C15ull;
    const int start = static_cast<int>(h >> 53) & (kEntries - 1);
    Entry* free = nullptr;
    for (int i = 0; i < 16; ++i) {
        Entry& e = g_tab[(start + i) & (kEntries - 1)];
        if (e.comp == comp) {
            if (e.owner != owner) { e.owner = owner; e.s = FarPawnStep{}; }  // the address now holds another component
            return &e;
        }
        if (!free) {
            const int64_t idle = now - e.lastQpc;
            if (!e.comp || (idle > 10 * g_qpcFreq && e.s.acc == 0.f) || idle > 60 * g_qpcFreq) free = &e;
        }
    }
    if (!free) return nullptr;
    free->comp = comp;
    free->owner = owner;
    free->s = FarPawnStep{};
    free->lastQpc = now;
    return free;
}

double PeriodOf(int hz) { return hz <= 0 || hz >= 60 ? 0.0 : 1.0 / hz; }
bool Calm(int proc) { return proc > 0 && proc < 64 && ((kCalmMask >> proc) & 1); }
int Procedure(const void* pawn) { return g_proc(pawn); }

double Dist2(const double* a, double x, double y, double z)
{
    const double dx = a[0] - x, dy = a[1] - y, dz = a[2] - z;
    return dx * dx + dy * dy + dz * dz;
}

template <class T>
T Field(const void* p, uint32_t off)
{
    T v;
    memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof v);
    return v;
}

// Horses: with a rider only at FarHorseMove 2 and only an NPC rider sitting on it (within 3 m) whose procedure is calm (the
// player's own horse is near him, so never here); the horse's own controller, if any, calm too; no controller and no rider:
// not throttled.
bool HorseQualifies(const uint8_t* horse, const double* hl, int hmode)
{
    const uint8_t* rider = Field<const uint8_t*>(horse, kRider);
    if (rider) {
        if (hmode < 2) return false;
        const uint8_t* rroot = Field<const uint8_t*>(rider, g_off.root);
        if (!rroot) return false;
        const double* rl = reinterpret_cast<const double*>(rroot + g_off.location);
        if (Dist2(rl, hl[0], hl[1], hl[2]) > kRiderOnHorse * kRiderOnHorse) return false;  // mounting / dismounting
        if (!Calm(Procedure(rider))) return false;
    }
    if (Field<const void*>(horse, kController)) {
        if (!Calm(Procedure(horse))) return false;
    } else if (!rider) {
        return false;
    }
    if (rider) g_hRidden.fetch_add(1, std::memory_order_relaxed);
    return true;
}

enum Verdict { kNo, kYes, kRootMotion };

Verdict Qualifies(const uint8_t* c, int mode, double px, double py, double pz, bool horseComp, bool carrying)
{
    __try {
        const uint8_t* pawn = Field<const uint8_t*>(c, kOwner);
        const uint8_t* upd = Field<const uint8_t*>(c, kUpdated);
        if (!pawn || !upd) return kNo;
        // A pawn carrying time from skipped frames asks about root motion first: it then keeps the time (see the top).
        if (carrying && g_rootMotion(pawn)) return kRootMotion;
        if (c[kMoveMode] == kMoveSwimming) { g_swimKept.fetch_add(1, std::memory_order_relaxed); return kNo; }
        const double* l = reinterpret_cast<const double*>(upd + g_off.location);
        if (Dist2(l, px, py, pz) < g_minDist * g_minDist) return kNo;
        if (horseComp) {
            const int hmode = g_horseMode.load(std::memory_order_relaxed);
            if (hmode == 0 || !HorseQualifies(pawn, l, hmode)) return kNo;
        } else {
            if (Field<const void*>(upd, kAttachParent)) return kNo;  // attached (a mounted rider): every frame
            if (!Calm(Procedure(pawn))) return kNo;
        }
        if (mode != 2) {  // FarPawnMoveHalfRate 2 = any speed; otherwise (also for horses) walking pace or slower
            const double* v = reinterpret_cast<const double*>(c + kVelocity);
            if (v[0] * v[0] + v[1] * v[1] > 300.0 * 300.0) return kNo;
        }
        if (!carrying && g_rootMotion(pawn)) return kRootMotion;
        return kYes;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kNo;
    }
}

// Before a tick that follows k-1 skipped frames: the pawn's summed input becomes the frames' mean (see the top).
void MeanInput(const uint8_t* comp, int k)
{
    __try {
        uint8_t* pawn = Field<uint8_t*>(comp, kOwner);
        if (!pawn) return;
        double* in = reinterpret_cast<double*>(pawn + kInput);
        if (in[0] == 0.0 && in[1] == 0.0 && in[2] == 0.0) return;
        const double f = 1.0 / k;
        in[0] *= f;
        in[1] *= f;
        in[2] *= f;
        g_inputs.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TickCommon(void* self, float dt, int tickType, void* tf, bool horseComp)
{
    const auto orig = reinterpret_cast<TickFn>(horseComp ? g_origHorse : g_orig);
    (horseComp ? g_hCalls : g_calls).fetch_add(1, std::memory_order_relaxed);
    const int mode = g_mode.load(std::memory_order_relaxed);
    const bool on = horseComp ? g_horseMode.load(std::memory_order_relaxed) != 0 : mode != 0;
    double px, py, pz;  // false off the game thread or without NpcCreationPacing's player position: stock tick
    if (!g_playerPos(&px, &py, &pz)) { orig(self, dt, tickType, tf); return; }
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    const uint8_t* c = static_cast<const uint8_t*>(self);
    const void* owner = nullptr;
    __try { owner = Field<const void*>(c, kOwner); } __except (EXCEPTION_EXECUTE_HANDLER) { owner = nullptr; }
    Entry* e = Find(self, owner, q.QuadPart);
    if (!e) { orig(self, dt, tickType, tf); return; }
    e->lastQpc = q.QuadPart;
    double p = on ? PeriodOf(g_hz.load(std::memory_order_relaxed)) : 0.0;
    if (p > g_maxStep) p = g_maxStep;
    const float period = static_cast<float>(p);
    const bool carrying = e->s.acc > 0.f;
    Verdict v = kNo;
    if (period > 0.f || carrying) v = Qualifies(c, mode, px, py, pz, horseComp, carrying);
    if (v == kYes && period <= 0.f) v = kNo;
    if (v == kYes) (horseComp ? g_hQualified : g_qualified).fetch_add(1, std::memory_order_relaxed);
    if (v == kRootMotion) g_rootKept.fetch_add(1, std::memory_order_relaxed);
    float step = dt;
    const int frames = e->s.n + 1;
    if (!FarPawnDecide(e->s, dt, period, g_maxStep, v == kYes, v == kRootMotion, Phase(self), &step)) {
        (horseComp ? g_hSkipped : g_skipped).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Also on a root-motion tick (step = dt, carried time kept; rev130m FP-N1): the mean is one frame's share of the summed
    // input, which is what a dt step gets in the stock game; the skipped frames' share is consumed with it (the stock game
    // consumes each frame's input in its own tick), and root motion sets the velocity itself, so input barely acts there.
    if (frames > 1) MeanInput(c, frames);
    if (step > g_maxStep + 0.0005f && step > dt + 0.0005f) g_overMax.fetch_add(1, std::memory_order_relaxed);
    orig(self, step, tickType, tf);
}

void HookTick(void* self, float dt, int tickType, void* tf) { TickCommon(self, dt, tickType, tf, false); }
void HookTickHorse(void* self, float dt, int tickType, void* tf) { TickCommon(self, dt, tickType, tf, true); }

// The offsets the exe's generated property tables give a property name (FGenericPropertyParams: the name pointer first,
// ArrayDim and Offset as uint16 at +0x30 / +0x32; one entry per class that declares the name).
std::vector<uint32_t> ReflectedOffsets(const GameModule& game, const char* name)
{
    std::vector<const uint8_t*> strs;
    const size_t n = strlen(name) + 1;
    const uint8_t* end = game.rdata.end;
    for (const uint8_t* p = game.rdata.begin + 1; p + n <= end; ++p) {
        p = static_cast<const uint8_t*>(memchr(p, name[0], end - p));
        if (!p || p + n > end) break;
        if (p[-1] == 0 && memcmp(p, name, n) == 0) strs.push_back(p);
    }
    std::vector<uint32_t> out;
    if (strs.empty()) return out;
    for (auto* const* s = reinterpret_cast<void* const*>(game.rdata.begin); s + 7 <= reinterpret_cast<void* const*>(end); ++s)
        for (const uint8_t* str : strs)
            if (*s == str) {
                const auto* params = reinterpret_cast<const uint8_t*>(s);
                uint16_t dim, off;
                memcpy(&dim, params + 0x30, 2);
                memcpy(&off, params + 0x32, 2);
                if (dim == 1) out.push_back(off);
            }
    return out;
}

bool Reflected(const GameModule& game, const char* name, uint32_t off)
{
    const auto offs = ReflectedOffsets(game, name);
    return std::find(offs.begin(), offs.end(), off) != offs.end();
}

// The exec thunk of the native function `name`: the code pointer after a pointer to the name string in .rdata / .data
// (the {name, thunk} entries of the classes' native tables) whose code matches `pattern`. nullptr unless exactly one.
uint8_t* NativeThunk(const GameModule& game, const char* name, const char* pattern)
{
    const size_t n = strlen(name) + 1;
    std::vector<const uint8_t*> strs;
    for (const uint8_t* p = game.rdata.begin + 1; p + n <= game.rdata.end; ++p) {
        p = static_cast<const uint8_t*>(memchr(p, name[0], game.rdata.end - p));
        if (!p || p + n > game.rdata.end) break;
        if (p[-1] == 0 && memcmp(p, name, n) == 0) strs.push_back(p);
    }
    if (strs.empty()) return nullptr;
    const auto pat = ParsePattern(pattern);
    uint8_t* found = nullptr;
    int hits = 0;
    for (const Section* sec : {&game.rdata, &game.data})
        for (uint8_t* q = sec->begin; q + 16 <= sec->end; q += 8) {
            const uint8_t* const s = *reinterpret_cast<const uint8_t* const*>(q);
            if (std::find(strs.begin(), strs.end(), s) == strs.end()) continue;
            uint8_t* const code = *reinterpret_cast<uint8_t* const*>(q + 8);
            if (!game.text.Contains(code) || !game.text.Contains(code + 32) || !MatchesAt(code, pat)) continue;
            if (code != found) ++hits;
            found = code;
        }
    return hits == 1 ? found : nullptr;
}

uint8_t* One(const GameModule& game, const char* pattern)
{
    const auto hits = FindAll(game.text, ParsePattern(pattern));
    return hits.size() == 1 ? hits[0] : nullptr;
}

// Exactly one of the vtable's first kVtScan slots holds fn.
bool InVtable(const GameModule& game, void* const* vt, const uint8_t* fn)
{
    int found = 0;
    for (int i = 0; i < kVtScan && game.rdata.Contains(vt + i); ++i) found += vt[i] == fn;
    return found == 1;
}

}  // namespace

bool FarPawnDecide(FarPawnStep& s, float dt, float period, float maxStep, bool qualifies, bool rootMotion, float phase, float* step)
{
    if (qualifies) {
        bool first = false;
        if (s.thr < 0.f) {  // newly qualifying: first tick at a spread phase
            s.thr = s.acc + period * phase;
            first = true;
        }
        const float have = s.acc + dt;
        const float thr = std::min(s.thr, period);
        if (have < (first ? thr : thr - 0.5f * dt) && have + dt <= maxStep) {  // skip this frame, keep its time step
            s.acc = have;
            ++s.n;
            return false;
        }
        s.thr = period;
        *step = have;
        s.acc = 0.f;
    } else if (rootMotion) {
        s.thr = -1.f;
        *step = dt;  // carried time stays for the first tick after the root motion
    } else {
        s.thr = -1.f;
        *step = dt + s.acc;
        s.acc = 0.f;
    }
    s.n = 0;
    return true;
}

bool FarPawnMoveDiscover(const GameModule& game, uint32_t rootOffset, FarPawnMoveDiscovery* out)
{
    *out = FarPawnMoveDiscovery{};
    const auto fail = [&](const char* what) { out->missing = what; return false; };
    uint8_t* const pairedCtor = One(game, kPairedCtor);
    if (!pairedCtor) return fail("the paired movement component's constructor");
    out->pawnVtable = reinterpret_cast<void**>(RipTarget(pairedCtor + kPairedVtLea, 3, 7));
    if (!game.rdata.Contains(out->pawnVtable)) return fail("the paired movement component's vtable");
    uint8_t* const tick = One(game, kTick);
    if (!tick || out->pawnVtable[kSlot] != tick) return fail("CMC TickComponent in slot 127");
    out->tick = tick;
    uint8_t* const rm = One(game, kTickRootMotion);
    uint8_t* const upd = One(game, kTickUpdated);
    if (!rm || rm < tick || rm >= tick + kTickBodySpan || !upd || upd < tick || upd >= tick + kTickBodySpan)
        return fail("TickComponent's CharacterOwner / Mesh / UpdatedComponent reads");
    out->isRootMotion = RipTarget(rm + kTickRootMotionCall, 1, 5);
    if (!game.text.Contains(out->isRootMotion) || !MatchesAt(out->isRootMotion, ParsePattern(kIsRootMotion)))
        return fail("ACharacter::IsPlayingRootMotion");
    uint8_t* const swim = One(game, kIsSwimming);
    uint8_t* const vel = One(game, kUpdateVelocity);
    if (!swim || !InVtable(game, out->pawnVtable, swim) || !vel || !InVtable(game, out->pawnVtable, vel))
        return fail("the vtable's IsSwimming / UpdateComponentVelocity (MovementMode, Velocity)");
    uint8_t* const proc = One(game, kGetProc);
    if (!proc || memcmp(proc - kGetProcBody, kGetProcPrologue, sizeof kGetProcPrologue) != 0)
        return fail("GetCurrentProcedureType");
    out->getProc = proc - kGetProcBody;
    uint8_t* const getAttach = NativeThunk(game, "GetAttachParent", kExecGetAttachParent);
    uint32_t attachOff = 0;
    if (getAttach) memcpy(&attachOff, getAttach + kExecAttachDisp, 4);
    if (!getAttach || attachOff != kAttachParent) return fail("USceneComponent's execGetAttachParent (AttachParent)");
    out->getAttachParent = getAttach;
    if (!Reflected(game, "ControlInputVector", kInput) || (rootOffset && !Reflected(game, "RootComponent", rootOffset)))
        return fail("the property tables' ControlInputVector / RootComponent");
    // Horses (optional): the constructor that calls the paired one, its vtable's slot 127, the Rider property.
    for (uint8_t* h : FindAll(game.text, ParsePattern(kHorseCtor)))
        if (RipTarget(h + kHorseCtorCall, 1, 5) == pairedCtor) {
            if (out->horseVtable) { out->horseVtable = nullptr; break; }
            out->horseVtable = reinterpret_cast<void**>(RipTarget(h + kHorseVtLea, 3, 7));
        }
    if (out->horseVtable && (!game.rdata.Contains(out->horseVtable) || out->horseVtable[kSlot] != tick ||
                             !Reflected(game, "Rider", kRider)))
        out->horseVtable = nullptr;
    return true;
}

bool InstallFarPawnMove(bool beforeGameCode, int mode, int hz, int horseMode, int minDistance, int maxStepMs)
{
    if (g_installed) return true;
    if (!beforeGameCode) { Log("[farmove] FarPawnMoveHalfRate: start-up only (version.dll route), not installed"); return false; }
    NpcPacingInfoOut pi{};
    if (!NpcPacingInfo(&pi) || !pi.rootOffset || !pi.locationOffset) {
        Log("[farmove] FarPawnMoveHalfRate: needs NpcCreationPacing (the player's position), not installed");
        return false;
    }
    FarPawnMoveDiscovery d;
    if (!FarPawnMoveDiscover(Game(), pi.rootOffset, &d)) {
        Log("[farmove] FarPawnMoveHalfRate: %s not recognised, not installed", d.missing);
        return false;
    }
    g_off.root = pi.rootOffset;
    g_off.location = pi.locationOffset;
    g_proc = reinterpret_cast<ProcFn>(d.getProc);
    g_rootMotion = reinterpret_cast<BoolFn>(d.isRootMotion);
    g_minDist = std::max(1000, minDistance);
    g_maxStep = static_cast<float>(std::clamp(maxStepMs, 20, 1000)) / 1000.f;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = f.QuadPart;
    SetFarPawnMove(mode, hz, horseMode);
    g_orig = d.pawnVtable[kSlot];
    if (!WritePointer(&d.pawnVtable[kSlot], reinterpret_cast<void*>(&HookTick))) {
        Log("[farmove] FarPawnMoveHalfRate: vtable slot not writable, not installed");
        return false;
    }
    g_installed = true;
    if (d.horseVtable) {
        g_origHorse = d.horseVtable[kSlot];
        if (WritePointer(&d.horseVtable[kSlot], reinterpret_cast<void*>(&HookTickHorse))) g_horseInstalled = true;
        else g_origHorse = nullptr;
    }
    Log("[farmove] FarPawnMoveHalfRate installed (VPairedPawnMovementComponent exe+0x%llX slot %d%s): FarPawnMoveHalfRate=%d, "
        "FarPawnMoveHz=%d, FarHorseMove=%d, min distance %.0f, max step %.0f ms",
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(d.pawnVtable) - Game().base), kSlot,
        g_horseInstalled ? ", horses too" : "; horses not covered (their constructor, vtable or Rider not recognised)",
        g_mode.load(), g_hz.load(), g_horseMode.load(), g_minDist, g_maxStep * 1000.f);
    return true;
}

void SetFarPawnMove(int mode, int hz, int horseMode)
{
    g_mode.store(mode == 1 || mode == 2 ? mode : 0, std::memory_order_relaxed);
    g_hz.store(std::clamp(hz, 1, 60), std::memory_order_relaxed);
    g_horseMode.store(std::clamp(horseMode, 0, 2), std::memory_order_relaxed);
}

void LogFarPawnMoveStats()
{
    if (!g_installed) return;
    static uint64_t last[11] = {};
    const uint64_t now[11] = {g_calls.load(), g_qualified.load(), g_skipped.load(), g_rootKept.load(), g_inputs.load(),
                              g_overMax.load(), g_swimKept.load(), g_hCalls.load(), g_hQualified.load(), g_hRidden.load(),
                              g_hSkipped.load()};
    if (now[1] == last[1] && now[8] == last[8] && now[3] == last[3]) return;  // nothing qualified: no line
    uint64_t d[11];
    for (int i = 0; i < 11; ++i) d[i] = now[i] - last[i];
    memcpy(last, now, sizeof last);
    Log("[farmove] pawns: ticks %llu, qualified %llu, skipped %llu (%d Hz); root motion kept %llu, inputs averaged %llu, steps over "
        "the max %llu, swimming %llu; horses: ticks %llu, qualified %llu (ridden %llu), skipped %llu",
        static_cast<unsigned long long>(d[0]), static_cast<unsigned long long>(d[1]), static_cast<unsigned long long>(d[2]),
        g_hz.load(), static_cast<unsigned long long>(d[3]), static_cast<unsigned long long>(d[4]),
        static_cast<unsigned long long>(d[5]), static_cast<unsigned long long>(d[6]), static_cast<unsigned long long>(d[7]),
        static_cast<unsigned long long>(d[8]), static_cast<unsigned long long>(d[9]), static_cast<unsigned long long>(d[10]));
}

// Self-test: the qualification and tick path on fakes (the game's procedure getter and IsPlayingRootMotion replaced).
void FarPawnMoveTestSetup(const FarPawnMoveTestFakes& f)
{
    g_off.root = f.rootOffset;
    g_off.location = f.locationOffset;
    g_proc = reinterpret_cast<ProcFn>(f.getProc);
    g_rootMotion = reinterpret_cast<BoolFn>(f.isRootMotion);
    g_playerPos = f.playerPos ? f.playerPos : &NpcPacingPlayerPos;
    g_orig = f.origTick;
    g_origHorse = f.origTick;
    g_minDist = 10000.0;
    g_maxStep = 0.4f;
    LARGE_INTEGER q;
    QueryPerformanceFrequency(&q);
    g_qpcFreq = q.QuadPart;
    memset(g_tab, 0, sizeof g_tab);
}

void FarPawnMoveTestTick(void* comp, float dt, bool horse) { TickCommon(comp, dt, 0, nullptr, horse); }

}  // namespace opf
