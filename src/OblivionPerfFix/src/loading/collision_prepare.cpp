// PrepareCollisionBehindLoadingScreen: the one slow frame as the picture comes back after a door, city gate or fast travel, without changing when
// the world starts.
//
// During a transition the game keeps the world frozen (VFreezeInMenuSubsystem's loading mode), and freezing clears
// UWorld::bShouldSimulatePhysics, so the physics scene is not ticked at all while the new area loads behind the loading
// screen: none of the thousands of collision bodies the level load registers reach the Chaos solver. The first physics
// tick after the unfreeze (0.5 s into the fade-in) then hands all of them over at once: the solver creates their
// particles and inserts each one into its broad-phase acceleration structures (ComputeIntermediateSpatialAcceleration),
// the game thread waits for it, then re-inserts them into its scene-query structure. 120-270 ms in one frame.
//
// The engine has a designated way to do that work without simulating: a solver tick with a time step of 0 (what
// FChaosScene::Flush does, and what the editor does every frame). With dt 0 the solver takes the game thread's pending
// changes, creates the particles and updates the acceleration structures, but the step loop needs time left (more than
// MinDeltaTime, which the engine keeps >= 1e-8), simulation callbacks are skipped ("if delta time is 0 we are flushing
// data"), and no results go back to the game. Nothing moves, collides, wakes up or fires an event.
//
// So when the game is about to start the fade-in after a transition (the fade-to-game delegate, hooked in
// fade_hook.cpp), and the world and its physics are still frozen, this dispatches that zero-time tick, lets the
// game's fade-to-game run (it blocks on its texture stream-in for a while, behind the loading screen, while the physics
// work runs on worker threads), and afterwards, if the physics work has finished, updates the game thread's scene-query
// structure the way the end of every physics frame does (CopySolverAccelerationStructure). The freeze, the unfreeze
// and the fade-in happen exactly when they did before; the first physics step after the unfreeze finds the work done.
//
// Everything is found by byte patterns, and every offset is read from the game's own code and cross-checked between
// the functions that use it (for example that the freeze writes the very bit the physics tick setup reads).
#include "common.h"

#include <atomic>

namespace opf {
namespace {

using DispatchFn = void** (*)(void* solver, void** outEvent, double dt);  // returns FGraphEventRef by hidden pointer
using CopyFn = void (*)(void* scene);
using RecycleFn = void (*)(void* graphEvent);

// Bytes from game build 19115871 (UE 5.3 with Chaos).
// FChaosScene::StartFrame (exe+0x1AA6510): if (P.Chaos.Simulation.Enable) { dt = OnStartFrame(MDeltaTime);
// GetSolversMutable(...); solver = SceneSolver; ... AdvanceAndDispatch_External(solver, dt) }
const char* const kStartFrame =
    "41 56 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 4C 8B F1 83 38 00 0F 84 ?? ?? ?? ?? 48 8B 01 F3 0F 10 49 ?? "
    "48 89 5C 24 70 48 89 6C 24 58 48 89 74 24 50 48 89 7C 24 48 4C 89 7C 24 40 0F 29 74 24 30 FF 50 ?? "
    "49 8B 56 ?? 4C 8D 44 24 20 49 8B 4E ?? 45 33 FF 0F 28 F0 4C 89 7C 24 20 4C 89 7C 24 28 E8 ?? ?? ?? ?? "
    "49 8B 76 ?? 48 8B 5C 24 20 48 85 F6";
constexpr size_t kStartFrameSolver = 0x67;  // disp8 of mov rsi,[r14+SceneSolver]
// ... mov rcx,[rbx] / lea rdx,[rsp+80h] / movaps xmm2,xmm6 / call AdvanceAndDispatch_External
const char* const kStartFrameDispatch = "48 8B 0B 48 8D 94 24 ?? ?? ?? ?? 0F 28 D6 E8 ?? ?? ?? ??";
// FChaosScene::EndFrame (exe+0x1AA1110): if (!cvar || !SceneSolver) return; ... CopySolverAccelerationStructure(this)
const char* const kEndFrame = "41 56 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 4C 8B F1 83 38 00 0F 84 ?? ?? ?? ?? 48 83 79 ?? 00 0F 84";
constexpr size_t kEndFrameSolver = 0x1C;
const char* const kEndFrameCopy = "49 8B CE E8 ?? ?? ?? ?? 48 8B 7C 24 20";
// FChaosScene::CopySolverAccelerationStructure (exe+0x1A9FF90): if (SceneSolver) { write lock; update; unlock }
const char* const kCopy = "48 89 5C 24 18 55 48 83 EC 20 48 8B 59 ?? 48 8B E9 48 85 DB 0F 84 ?? ?? ?? ?? 48 8B 9B ?? ?? 00 00";
// FPhysicsSolverBase::AdvanceAndDispatch_External (exe+0x1883DB0), up to the "fixed time step?" test:
// MaxSubSteps, bSolverSubstep, bPaused, ForceDisableAsyncPhysics, AsyncDt, UseAsyncInterpolation.
const char* const kDispatch =
    "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E0 48 81 EC 20 01 00 00 48 8B 05 ?? ?? ?? ?? "
    "48 33 C4 48 89 45 10 33 FF 48 89 4C 24 30 4C 8B E9 89 7C 24 28 8B 89 ?? ?? 00 00 0F 28 EA 83 F9 01 "
    "48 89 54 24 38 48 8B DA 89 4C 24 28 0F 9F C0 44 8B FF 0F 57 C9 41 88 85 ?? ?? 00 00 41 38 BD ?? ?? 00 00 "
    "74 03 0F 57 D2 39 3D ?? ?? ?? ?? 41 B9 01 00 00 00 45 8B C1 0F 85 ?? ?? ?? ?? 66 41 0F 2F 8D ?? ?? 00 00 "
    "0F 87 ?? ?? ?? ?? 39 3D ?? ?? ?? ?? 0F 84";
constexpr size_t kDispatchForceDisable = 0x6C;  // cmp [ForceDisableAsyncPhysics],edi (39 3D disp32)
constexpr size_t kDispatchAsyncDt = 0x86;       // disp32 of comisd xmm1,[r13+AsyncDt]
// In the same function: if (RewindCallback && !bIsShuttingDown) ... / ThreadingMode == SingleThread?
const char* const kDispatchRewind =
    "49 8B 8D ?? ?? 00 00 48 85 C9 74 ?? 41 80 BD ?? ?? 00 00 00 75 ?? 48 8B 01 4C 8D 83 ?? ?? 00 00 8B 93 ?? ?? 00 00 "
    "FF 50 ?? 41 80 7D ?? 02";
// FGraphEvent::IsComplete inlined: atomic read of the subsequents list head, "closed" bit
const char* const kIsComplete = "48 85 C9 74 ?? 33 C0 F0 4C 0F B1 29 48 C1 E8 ?? A8 01";
// FGraphEventRef release: lock xadd [ev+RefCount],-1; last reference -> recycle
const char* const kRelease = "B8 FF FF FF FF F0 0F C1 41 ?? 83 F8 01 75 05 E8 ?? ?? ?? ??";
// AdvanceOneTimeStepTask::DoWork (exe+0x1A28590): solver = task->MSolver; if (solver->MRewindData) ...;
// evolution at solver+0x930
const char* const kDoWork =
    "4C 8B DC 55 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 ?? "
    "48 8B 11 49 89 5B 10 49 89 73 18 48 8B F1 49 89 7B 20 4D 89 63 F0 4D 89 6B E8 4D 89 73 E0 4D 89 7B D8 "
    "45 0F 29 43 A8 48 89 4C 24 ?? 48 8B 8A ?? ?? 00 00 45 0F 29 4B 98 45 0F 29 53 88 48 85 C9 74 ?? "
    "48 8B 82 ?? ?? 00 00";
constexpr size_t kDoWorkRewindData = 0x4F;  // disp32 of mov rcx,[rdx+MRewindData]
constexpr size_t kDoWorkEvolution = 0x65;   // disp32 of mov rax,[rdx+MEvolution]
// ... after the step loop: if (MinDeltaTime > MDeltaTime) Evolution->ComputeIntermediateSpatialAcceleration(false)
const char* const kDoWorkZeroDt =
    "66 44 0F 2F 4E 08 44 0F 28 8C 24 ?? ?? 00 00 44 0F 28 84 24 ?? ?? 00 00 0F 28 BC 24 ?? ?? 00 00 "
    "76 ?? 48 8B 0E 33 D2 48 8B 89 ?? ?? 00 00 E8 ?? ?? ?? ??";
// UWorld::SetupPhysicsTickFunctions (exe+0x3593670): physics on = (byte [world+0x13C] >> 2) & 1 (bShouldSimulatePhysics)
const char* const kSetupPhysics =
    "48 89 5C 24 18 57 48 81 EC 90 00 00 00 80 89 ?? ?? 00 00 02 48 8B D9 80 89 ?? ?? 00 00 02 "
    "48 89 89 ?? ?? 00 00 48 89 89 ?? ?? 00 00 0F B6 89 ?? ?? 00 00 48 8B 83 ?? ?? 00 00 C0 E9 ?? 80 E1 01";
constexpr size_t kSetupPhysicsByte = 0x2F;
constexpr size_t kSetupPhysicsShift = 0x3C;
// ... later in it: PhysicsScene = world->PhysicsScene; if (!PhysicsScene) return;
const char* const kSetupPhysicsScene = "48 8B BB ?? ?? 00 00 48 85 FF 0F 84 ?? ?? ?? ?? 48 89 AC 24";
// The freeze subsystem's apply (exe+0x4958950): world = GetWorld; ...; [world+0x13C] bit 2 = !freeze
const char* const kFreezeApply =
    "FF 90 ?? ?? 00 00 48 8B C8 48 8B D8 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 0F B6 8B ?? ?? 00 00 "
    "40 0F B6 D7 80 F2 01 48 89 B4 24 ?? ?? 00 00 C0 E2 ?? 80 E1 ?? 0A D1 4C 89 A4 24 ?? ?? 00 00 "
    "4C 89 B4 24 ?? ?? 00 00 4C 89 BC 24 ?? ?? 00 00 88 93 ?? ?? 00 00";
constexpr size_t kFreezeApplyByte = 0x1C;
constexpr size_t kFreezeApplyShift = 0x31;
constexpr size_t kFreezeApplyMask = 0x34;
constexpr size_t kFreezeApplyStore = 0x51;

struct State {
    bool found = false;
    uint8_t* startFrame = nullptr;
    uint8_t* endFrame = nullptr;
    uint8_t* dispatch = nullptr;
    uint8_t* copy = nullptr;
    uint8_t* recycle = nullptr;
    uint8_t* doWork = nullptr;
    uint8_t* cisa = nullptr;
    uint8_t* setupPhysics = nullptr;
    uint8_t* freezeApply = nullptr;
    uint8_t** simEnable = nullptr;       // holds the TConsoleVariableData<int32>* of P.Chaos.Simulation.Enable
    const int32_t* forceDisableAsync = nullptr;  // p.ForceDisableAsyncPhysics
    uint32_t sceneSolver = 0;            // FChaosScene::SceneSolver
    uint32_t asyncDt = 0;                // FPhysicsSolverBase::AsyncDt (double; < 0 = synchronous physics)
    uint32_t rewindCallback = 0, shuttingDown = 0, rewindData = 0, evolution = 0;
    uint32_t worldPhysicsByte = 0;       // UWorld byte holding bShouldSimulatePhysics
    uint8_t worldPhysicsBit = 0;
    uint32_t worldScene = 0;             // UWorld::PhysicsScene
    uint32_t refCount = 0;               // FGraphEvent reference count
    uint8_t completeShift = 0;           // FGraphEvent "closed" bit
    DispatchFn dispatchFn = nullptr;
    CopyFn copyFn = nullptr;
    RecycleFn recycleFn = nullptr;
    const int32_t* testSimEnable = nullptr;
};
State g;
std::atomic<bool> g_enabled{false};

// One transition at a time, game thread only.
struct Pending {
    bool active = false;
    void* event = nullptr;
    uint8_t* world = nullptr;
    uint8_t* scene = nullptr;
    uint8_t* solver = nullptr;
    int64_t dispatched = 0;  // QPC after the dispatch returned
    int64_t pushUs = 0;      // game-thread time of the dispatch (the push of the pending changes)
};
Pending g_pending;

std::atomic<uint64_t> g_prepared{0}, g_finished{0}, g_copied{0}, g_late{0}, g_faults{0};
std::atomic<uint64_t> g_skipPhysicsOn{0}, g_skipNoScene{0}, g_skipAsync{0}, g_skipRewind{0}, g_skipOff{0};
std::atomic<int64_t> g_pushUs{0}, g_pushMaxUs{0}, g_copyUs{0}, g_copyMaxUs{0};

int64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t QpcToUs(int64_t ticks)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return QpcToMicroseconds(ticks, static_cast<uint64_t>(f.QuadPart));
}

void AddTime(std::atomic<int64_t>& sum, std::atomic<int64_t>& max, int64_t us)
{
    sum.fetch_add(us);
    int64_t m = max.load();
    while (us > m && !max.compare_exchange_weak(m, us)) {}
}

uint32_t Disp32(const uint8_t* at) { return *reinterpret_cast<const uint32_t*>(at); }

// The only match of a pattern in [from, from + len), or nullptr.
uint8_t* FindIn(uint8_t* from, size_t len, const char* pattern)
{
    const Pattern p = ParsePattern(pattern);
    uint8_t* found = nullptr;
    for (size_t i = 0; i + p.size() <= len; ++i) {
        if (!MatchesAt(from + i, p)) continue;
        if (found) return nullptr;
        found = from + i;
    }
    return found;
}

bool Fail(const char* what)
{
    Log("[collision] game code not recognised (%s): fix disabled", what);
    return false;
}

bool Find(const GameModule& game)
{
    auto rva = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
    auto inText = [&](const uint8_t* p) { return game.text.Contains(p); };

    // FChaosScene::StartFrame: the simulation switch, SceneSolver, and the call to AdvanceAndDispatch_External.
    const auto starts = FindAll(game.text, ParsePattern(kStartFrame));
    if (starts.size() != 1) return Fail("FChaosScene::StartFrame");
    g.startFrame = starts[0];
    g.simEnable = reinterpret_cast<uint8_t**>(RipTarget(g.startFrame + 6, 3, 7));
    g.sceneSolver = g.startFrame[kStartFrameSolver];
    uint8_t* const call = FindIn(g.startFrame + 0x70, 0xA0, kStartFrameDispatch);
    if (!call) return Fail("the solver dispatch in StartFrame");
    g.dispatch = RipTarget(call + 14, 1, 5);

    // FChaosScene::EndFrame: the same switch and SceneSolver, and the call to CopySolverAccelerationStructure.
    for (uint8_t* e : FindAll(game.text, ParsePattern(kEndFrame))) {
        if (reinterpret_cast<uint8_t**>(RipTarget(e + 6, 3, 7)) != g.simEnable || e[kEndFrameSolver] != g.sceneSolver) continue;
        if (g.endFrame) return Fail("FChaosScene::EndFrame (ambiguous)");
        g.endFrame = e;
    }
    if (!g.endFrame) return Fail("FChaosScene::EndFrame");
    uint8_t* const copyCall = FindIn(g.endFrame + 0x100, 0x200, kEndFrameCopy);
    if (!copyCall) return Fail("the scene-query update in EndFrame");
    g.copy = RipTarget(copyCall + 3, 1, 5);
    if (!inText(g.copy) || !MatchesAt(g.copy, ParsePattern(kCopy)) || g.copy[0xD] != g.sceneSolver)
        return Fail("FChaosScene::CopySolverAccelerationStructure");

    // AdvanceAndDispatch_External: how it decides on a fixed time step (async physics), the rewind / shutdown state,
    // and the graph event's completion bit and release.
    if (!inText(g.dispatch) || !MatchesAt(g.dispatch, ParsePattern(kDispatch)))
        return Fail("FPhysicsSolverBase::AdvanceAndDispatch_External");
    g.forceDisableAsync = reinterpret_cast<const int32_t*>(RipTarget(g.dispatch + kDispatchForceDisable, 2, 6));
    g.asyncDt = Disp32(g.dispatch + kDispatchAsyncDt);
    uint8_t* const rewind = FindIn(g.dispatch + 0x100, 0x200, kDispatchRewind);
    uint8_t* const complete = FindIn(g.dispatch + 0x200, 0x200, kIsComplete);
    if (!rewind || !complete) return Fail("AdvanceAndDispatch_External: rewind / completion test");
    g.rewindCallback = Disp32(rewind + 3);
    g.shuttingDown = Disp32(rewind + 15);
    g.completeShift = complete[15];
    for (uint8_t* r : FindAll(Section{g.dispatch, g.dispatch + 0xC00}, ParsePattern(kRelease))) {
        uint8_t* const fn = RipTarget(r + 15, 1, 5);
        if (!g.recycle) { g.recycle = fn; g.refCount = r[9]; }
        else if (fn != g.recycle || r[9] != g.refCount) return Fail("AdvanceAndDispatch_External: graph event release");
    }
    if (!g.recycle || !inText(g.recycle) || g.completeShift >= 64 || g.refCount == 0)
        return Fail("AdvanceAndDispatch_External: graph event layout");
    auto inData = [&](const void* p) {  // a global variable of the exe (not code)
        const uint8_t* b = static_cast<const uint8_t*>(p);
        return b >= game.base && b + 8 <= game.base + game.sizeOfImage && !game.text.Contains(b);
    };
    if (!inData(g.forceDisableAsync) || !inData(g.simEnable)) return Fail("solver mode / simulation switch variables");

    // AdvanceOneTimeStepTask::DoWork: the rewind data and evolution offsets, and that a zero-time tick still updates
    // the acceleration structure (the call that only runs when the time step is below MinDeltaTime).
    const auto works = FindAll(game.text, ParsePattern(kDoWork));
    if (works.size() != 1) return Fail("AdvanceOneTimeStepTask::DoWork");
    g.doWork = works[0];
    g.rewindData = Disp32(g.doWork + kDoWorkRewindData);
    g.evolution = Disp32(g.doWork + kDoWorkEvolution);
    uint8_t* const zeroDt = FindIn(g.doWork + 0x300, 0x400, kDoWorkZeroDt);
    if (!zeroDt || Disp32(zeroDt + 42) != g.evolution) return Fail("DoWork: acceleration structure update for a zero time step");
    g.cisa = RipTarget(zeroDt + 46, 1, 5);
    {
        // ComputeIntermediateSpatialAcceleration names itself (its timer): lea reg,[L"ComputeIntermediate..."]
        static const wchar_t kName[] = L"ComputeIntermediateSpatialAcceleration";
        static const char kNameA[] = "ComputeIntermediateSpatialAcceleration";
        bool named = false;
        for (int k = 0; k < 0x20 && !named; ++k) {
            const uint8_t* insn = g.cisa + k;
            if ((insn[0] != 0x48 && insn[0] != 0x4C) || insn[1] != 0x8D || (insn[2] & 0xC7) != 0x05) continue;
            const uint8_t* s = RipTarget(insn, 3, 7);
            if (!game.rdata.Contains(s)) continue;
            named = memcmp(s, kName, sizeof kName) == 0 || memcmp(s, kNameA, sizeof kNameA) == 0;
        }
        if (!named) return Fail("ComputeIntermediateSpatialAcceleration");
    }

    // UWorld: the physics switch the tick setup reads, and the physics scene.
    const auto setups = FindAll(game.text, ParsePattern(kSetupPhysics));
    if (setups.size() != 1) return Fail("UWorld::SetupPhysicsTickFunctions");
    g.setupPhysics = setups[0];
    g.worldPhysicsByte = Disp32(g.setupPhysics + kSetupPhysicsByte);
    g.worldPhysicsBit = g.setupPhysics[kSetupPhysicsShift];
    uint8_t* const scene = FindIn(g.setupPhysics + 0x100, 0x80, kSetupPhysicsScene);
    if (!scene) return Fail("UWorld::PhysicsScene");
    g.worldScene = Disp32(scene + 3);

    // The freeze must be what switches physics off: it writes !freeze into that very bit of the world.
    const auto applies = FindAll(game.text, ParsePattern(kFreezeApply));
    if (applies.size() != 1) return Fail("the freeze subsystem's apply function");
    g.freezeApply = applies[0];
    const uint8_t* const a = g.freezeApply;
    if (Disp32(a + kFreezeApplyByte) != g.worldPhysicsByte || Disp32(a + kFreezeApplyStore) != g.worldPhysicsByte ||
        a[kFreezeApplyShift] != g.worldPhysicsBit || a[kFreezeApplyMask] != static_cast<uint8_t>(~(1u << g.worldPhysicsBit)))
        return Fail("the freeze does not switch the world's physics as expected");

    if (g.worldPhysicsByte > 0x2000 || g.worldPhysicsBit > 7 || g.worldScene > 0x2000 || g.sceneSolver > 0xF8 ||
        g.asyncDt > 0x2000 || g.rewindCallback > 0x2000 || g.shuttingDown > 0x2000 || g.rewindData > 0x2000 ||
        g.evolution > 0x4000 || g.refCount > 0x100)
        return Fail("offsets out of range");

    g.dispatchFn = reinterpret_cast<DispatchFn>(g.dispatch);
    g.copyFn = reinterpret_cast<CopyFn>(g.copy);
    g.recycleFn = reinterpret_cast<RecycleFn>(g.recycle);
    g.found = true;
    Log("[collision] solver dispatch exe+0x%X (AsyncDt +0x%X, rewind +0x%X/+0x%X, shutting down +0x%X), scene StartFrame "
        "exe+0x%X / EndFrame exe+0x%X (solver +0x%X), scene-query update exe+0x%X, zero-dt structure update exe+0x%X, "
        "world physics bit +0x%X.%u (freeze exe+0x%X), physics scene +0x%X",
        rva(g.dispatch), g.asyncDt, g.rewindCallback, g.rewindData, g.shuttingDown, rva(g.startFrame), rva(g.endFrame),
        g.sceneSolver, rva(g.copy), rva(g.cisa), g.worldPhysicsByte, g.worldPhysicsBit, rva(g.freezeApply), g.worldScene);
    return true;
}

enum class Gate { Go, PhysicsOn, NoScene, Async, Rewind, Off, Fault };

// Reads everything the decision needs; no game code runs here.
Gate Inspect(uint8_t* world, uint8_t** sceneOut, uint8_t** solverOut)
{
    __try {
        if ((world[g.worldPhysicsByte] >> g.worldPhysicsBit) & 1) return Gate::PhysicsOn;  // not frozen: the tick does it
        uint8_t* const scene = *reinterpret_cast<uint8_t**>(world + g.worldScene);
        uint8_t* const solver = scene ? *reinterpret_cast<uint8_t**>(scene + g.sceneSolver) : nullptr;
        if (!scene || !solver) return Gate::NoScene;
        const int32_t* simEnable = g.testSimEnable;
        if (!simEnable) simEnable = *reinterpret_cast<const int32_t* const*>(g.simEnable);
        if (!simEnable || *simEnable == 0) return Gate::Off;  // P.Chaos.Simulation.Enable 0: physics never ticks
        // Synchronous physics only: with async physics (a fixed time step) a zero tick would simulate the time left over.
        const double asyncDt = *reinterpret_cast<const double*>(solver + g.asyncDt);
        if (*g.forceDisableAsync == 0 && asyncDt >= 0.0) return Gate::Async;
        if (*reinterpret_cast<void* const*>(solver + g.rewindCallback) || *reinterpret_cast<void* const*>(solver + g.rewindData) ||
            solver[g.shuttingDown])
            return Gate::Rewind;
        *sceneOut = scene;
        *solverOut = solver;
        return Gate::Go;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Gate::Fault;
    }
}

bool EventComplete(void* ev)
{
    const uint64_t head = static_cast<uint64_t>(InterlockedCompareExchange64(static_cast<volatile LONG64*>(ev), 0, 0));
    return ((head >> g.completeShift) & 1) != 0;
}

void ReleaseEvent(void* ev)
{
    if (!ev) return;
    if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(static_cast<uint8_t*>(ev) + g.refCount)) == 0) g.recycleFn(ev);
}

bool SceneStillThere(const Pending& p)
{
    __try {
        return *reinterpret_cast<uint8_t**>(p.world + g.worldScene) == p.scene &&
               *reinterpret_cast<uint8_t**>(p.scene + g.sceneSolver) == p.solver;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool FindCollisionPrepare()
{
    if (g.found) return true;
    return Find(Game());
}

void SetCollisionPrepareEnabled(bool enabled)
{
    if (!g.found) enabled = false;
    if (g_enabled.exchange(enabled) != enabled && g.found)
        Log("[collision] %s", enabled ? "enabled" : "disabled (physics takes the new area at the first step after the unfreeze, as before)");
}

bool CollisionPrepareEnabled() { return g_enabled.load(std::memory_order_relaxed); }

bool PrepareCollisionBegin(void* worldPtr)
{
    if (!g_enabled.load() || !worldPtr || g_pending.active) return false;
    uint8_t* const world = static_cast<uint8_t*>(worldPtr);
    uint8_t* scene = nullptr;
    uint8_t* solver = nullptr;
    switch (Inspect(world, &scene, &solver)) {
    case Gate::Go: break;
    case Gate::PhysicsOn: g_skipPhysicsOn.fetch_add(1); return false;
    case Gate::NoScene: g_skipNoScene.fetch_add(1); return false;
    case Gate::Async: g_skipAsync.fetch_add(1); Log("[collision] left to the game: asynchronous physics"); return false;
    case Gate::Rewind: g_skipRewind.fetch_add(1); Log("[collision] left to the game: solver rewinding or shutting down"); return false;
    case Gate::Off: g_skipOff.fetch_add(1); return false;
    case Gate::Fault:
        g_faults.fetch_add(1);
        g_enabled.store(false);
        Log("[collision] unexpected memory layout: fix disabled");
        return false;
    }
    const int64_t t0 = Qpc();
    void* ev = nullptr;
    g.dispatchFn(solver, &ev, 0.0);  // the engine's flush: push, create particles, update the structures; no step
    const int64_t t1 = Qpc();
    g_pending = Pending{true, ev, world, scene, solver, t1, QpcToUs(t1 - t0)};
    g_prepared.fetch_add(1);
    AddTime(g_pushUs, g_pushMaxUs, g_pending.pushUs);
    return true;
}

void PrepareCollisionEnd()
{
    if (!g_pending.active) return;
    Pending p = g_pending;
    g_pending = Pending{};
    const int64_t waited = QpcToUs(Qpc() - p.dispatched);
    const bool done = p.event && EventComplete(p.event);
    bool copied = false;
    int64_t copyUs = 0;
    if (done) {
        g_finished.fetch_add(1);
        if (SceneStillThere(p)) {
            const int64_t t0 = Qpc();
            g.copyFn(p.scene);  // what the end of a physics frame does: take the new structure, apply pending changes
            copyUs = QpcToUs(Qpc() - t0);
            AddTime(g_copyUs, g_copyMaxUs, copyUs);
            g_copied.fetch_add(1);
            copied = true;
        }
    } else {
        g_late.fetch_add(1);
    }
    ReleaseEvent(p.event);
    Log("[collision] transition: new area handed to physics behind the loading screen (game thread %.1f ms); %s %.0f ms "
        "later%s",
        p.pushUs / 1000.0, done ? "finished" : "still running", waited / 1000.0,
        copied ? "" : (done ? "; scene gone, nothing updated" : "; the first physics step completes it"));
    if (copied) Log("[collision] scene queries updated (game thread %.1f ms)", copyUs / 1000.0);
}

void LogCollisionPrepareStats()
{
    if (!g.found) return;
    const uint64_t n = g_prepared.load();
    Log("[collision] transitions prepared behind the loading screen: %llu (finished before the fade-in %llu, scene queries "
        "updated %llu, still running %llu); game thread %.1f ms avg / %.1f ms max to hand over, scene-query update %.1f ms avg / "
        "%.1f ms max; left to the game: physics already on %llu, no scene %llu, async physics %llu, rewind/shutdown %llu, "
        "simulation off %llu; faults %llu",
        static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_finished.load()),
        static_cast<unsigned long long>(g_copied.load()), static_cast<unsigned long long>(g_late.load()),
        n ? g_pushUs.load() / 1000.0 / static_cast<double>(n) : 0.0, g_pushMaxUs.load() / 1000.0,
        g_copied.load() ? g_copyUs.load() / 1000.0 / static_cast<double>(g_copied.load()) : 0.0, g_copyMaxUs.load() / 1000.0,
        static_cast<unsigned long long>(g_skipPhysicsOn.load()), static_cast<unsigned long long>(g_skipNoScene.load()),
        static_cast<unsigned long long>(g_skipAsync.load()), static_cast<unsigned long long>(g_skipRewind.load()),
        static_cast<unsigned long long>(g_skipOff.load()), static_cast<unsigned long long>(g_faults.load()));
}

bool CollisionPrepareInfo(CollisionPrepareFixInfo* out)
{
    if (!g.found || !out) return false;
    out->dispatch = g.dispatch;
    out->copy = g.copy;
    out->recycle = g.recycle;
    out->startFrame = g.startFrame;
    out->endFrame = g.endFrame;
    out->doWork = g.doWork;
    out->cisa = g.cisa;
    out->setupPhysics = g.setupPhysics;
    out->freezeApply = g.freezeApply;
    out->simEnable = reinterpret_cast<void*>(g.simEnable);
    out->forceDisableAsync = g.forceDisableAsync;
    out->sceneSolver = g.sceneSolver;
    out->asyncDt = g.asyncDt;
    out->rewindCallback = g.rewindCallback;
    out->rewindData = g.rewindData;
    out->shuttingDown = g.shuttingDown;
    out->evolution = g.evolution;
    out->worldPhysicsByte = g.worldPhysicsByte;
    out->worldPhysicsBit = g.worldPhysicsBit;
    out->worldScene = g.worldScene;
    out->refCount = g.refCount;
    out->completeShift = g.completeShift;
    return true;
}

void CollisionPrepareTestFunctions(void* fakeDispatch, void* fakeCopy, void* fakeRecycle, const int32_t* fakeSimEnable,
                                   const int32_t* fakeForceDisableAsync)
{
    g.dispatchFn = reinterpret_cast<DispatchFn>(fakeDispatch);
    g.copyFn = reinterpret_cast<CopyFn>(fakeCopy);
    g.recycleFn = reinterpret_cast<RecycleFn>(fakeRecycle);
    g.testSimEnable = fakeSimEnable;
    if (fakeForceDisableAsync) g.forceDisableAsync = fakeForceDisableAsync;
}

}  // namespace opf
