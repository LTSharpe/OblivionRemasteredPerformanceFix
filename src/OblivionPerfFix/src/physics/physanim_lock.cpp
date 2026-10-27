// PhysAnimNoEmptyLock ([Fixes] PhysAnimNoEmptyLock, default 1; worker corral 2026-10-27, public since 1.2.3): a physical
// animation component that drives no bodies no longer takes the physics scene's write lock every frame. Identical: with
// nothing to drive and no animation task pending, the skipped code changes no state.
//
// UPhysicalAnimationComponent::TickComponent (UE 5.3, PhysicalAnimationComponent.cpp:306, TG_PrePhysics, every frame)
// calls UpdateTargetActors(ETeleportType::None) (:257). That takes the skeletal mesh's physics scene WRITE lock
// (FPhysicsCommand::ExecuteWrite -> FScopedSceneLock_Chaos, Write) and, inside it, copies the mesh's bone-space
// transforms into a local array and loops over DriveData (the bodies driven by a physical animation profile), setting
// each driven body's kinematic target. With DriveData empty the loop does nothing: the lock and the local copy have no
// effect on any state (the lock is the solver's external data lock: TLS recursion count + SRW lock, nothing else; the
// copy is freed at the end of the lambda). Altar's actors carry a VPhysicalAnimationComponent each and tick it every
// frame; at the Brindle Home corral (horses) the game thread waited up to ~0.9 ms per frame in that exclusive lock,
// for worker threads whose animation evaluation (Control Rig with world traces) held read locks, and the waiting
// writer in turn stalled the workers' next traces.
//
// The fix: UpdateTargetActors returns at once when DriveData.Num() <= 0 and the mesh has no pending parallel animation
// task. Any driven body, or a pending task: the game's own code. The lambda's first call, GetBoneSpaceTransforms
// (exe+0x3072280), inlines HandleExistingParallelEvaluationTask(true, true) (`cmp qword [rcx+0DC0h],0`): with a task
// pending it waits for it and runs CompleteParallelAnimationEvaluation right there. On the tick path the mesh's tick is a
// prerequisite, so no task is pending; on the OnTeleport path (exe+0x353E960) one can be, and skipping would move that
// completion later (independent review, worker physrev 2026-10-27). With no task pending, the skipped code only locks,
// copies into a temporary and loops zero times (UE 5.3 source; this build's lambda checks the same DriveData count
// first: exe+0x3516D10 `cmp [rdx+0E0h], r8d; jle`): identical by construction.
//
// Found by shape (build 19115871: entry exe+0x354EA80 `mov [rsp+10h],dl; push rbx; sub rsp,60h; mov rbx,rcx;
// mov rcx,[rcx+0C0h]` ..., then the scene lock ctor (write, r8b=1), the lambda and the lock dtor called at +A2/+AC/+B6;
// the DriveData count offset is read from the lambda's compare, the mesh offset from the entry's load, the pending-task
// offset from GetBoneSpaceTransforms' first compare, reached through the lambda's call at +0x36). The entry is replaced by a jump only before the game's
// code runs; a trampoline runs the game's function; the key is live after that.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// UpdateTargetActors: prologue (9 bytes, position independent), SkeletalMeshComponent load, getters, the scoped
// write lock (r8b = 1), the lambda and the dtor.
constexpr char kUpdate[] =
    "88 54 24 10 53 48 83 EC 60 48 8B D9 48 8B 89 ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 89 44 24 70 "
    "48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 8B ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? 48 8B C8 48 8B 10 FF 92 ?? ?? ?? ?? 48 8B 93 ?? ?? ?? ?? 48 8D 4C 24 20 48 89 44 24 40 41 B0 01 "
    "48 8D 44 24 70 48 89 5C 24 38 48 89 44 24 48";
constexpr size_t kPrologue = 9, kLambdaCall = 0xAC;
// The lambda: ... mov rcx,[rcx]; mov rcx,[rcx+C0h]; call GetBoneSpaceTransforms; mov rdx,[r14]; xor r8d,r8d;
// mov ebp,r8d; cmp [rdx+DriveData.Num],r8d; jle (no driven body: skip the loop)
constexpr char kLambda[] =
    "4C 8B DC 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 49 89 6B 18 48 8D 54 24 ?? "
    "4D 89 73 E0 4C 8B F1 48 8B 09 48 8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? 49 8B 16 45 33 C0 41 8B E8 44 39 82 ?? ?? ?? ?? 0F 8E";
constexpr size_t kNumOff = 71, kBoneCall = 0x36, kMeshOff = 15;
// GetBoneSpaceTransforms: prologue, then `cmp qword [rcx+ParallelEvaluationTask], 0` (the inlined
// HandleExistingParallelEvaluationTask), the pending-task offset at +18
constexpr char kBoneSpace[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 83 B9 ?? ?? ?? ?? 00";
constexpr size_t kTaskOff = 18;

using UpdateFn = void (*)(uint8_t* self, uint8_t teleport);

uint32_t g_numOff = 0, g_meshOff = 0, g_taskOff = 0;
UpdateFn g_game = nullptr;  // trampoline: the game's function
uint8_t* g_tramp = nullptr;  // its bytes (self-test)
bool g_installed = false;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> s_calls{0}, s_empty{0}, s_skipped{0}, s_pending{0}, s_emptyTicks{0}, s_fullTicks{0}, s_fullCalls{0};

// No driven body and no pending parallel animation task on the mesh: the game's code would only lock and copy.
bool NothingToDo(const uint8_t* self)
{
    if (*reinterpret_cast<const int32_t*>(self + g_numOff) > 0) return false;
    const uint8_t* const mesh = *reinterpret_cast<const uint8_t* const*>(self + g_meshOff);
    if (mesh && *reinterpret_cast<const void* const*>(mesh + g_taskOff) != nullptr) {
        s_pending.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void UpdateHook(uint8_t* self, uint8_t teleport)
{
    s_calls.fetch_add(1, std::memory_order_relaxed);
    const bool empty = NothingToDo(self);
    if (empty) {
        s_empty.fetch_add(1, std::memory_order_relaxed);
        if (g_on.load(std::memory_order_relaxed)) {
            s_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    g_game(self, teleport);
    QueryPerformanceCounter(&b);
    (empty ? s_emptyTicks : s_fullTicks).fetch_add(static_cast<uint64_t>(b.QuadPart - a.QuadPart), std::memory_order_relaxed);
    if (!empty) s_fullCalls.fetch_add(1, std::memory_order_relaxed);
}

bool Fail(const char* why)
{
    Log("[physanim] %s: PhysAnimNoEmptyLock unavailable", why);
    return false;
}

}  // namespace

bool InstallPhysAnimNoEmptyLock(bool beforeGameCode)
{
    if (g_installed) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kUpdate));
    if (hits.size() != 1) {
        Log("[physanim] UpdateTargetActors found %zu times (expected 1): PhysAnimNoEmptyLock unavailable", hits.size());
        return false;
    }
    uint8_t* const site = hits[0];
    if (site[kLambdaCall] != 0xE8) return Fail("no lambda call where expected");
    uint8_t* const lambda = RipTarget(site + kLambdaCall, 1, 5);
    if (!game.text.Contains(lambda) || !MatchesAt(lambda, ParsePattern(kLambda))) return Fail("the lambda does not match");
    const uint32_t numOff = *reinterpret_cast<const uint32_t*>(lambda + kNumOff);
    if (numOff < 0x40 || numOff > 0x400) return Fail("unexpected DriveData offset");
    const uint32_t meshOff = *reinterpret_cast<const uint32_t*>(site + kMeshOff);
    if (meshOff < 0x40 || meshOff > 0x400) return Fail("unexpected SkeletalMeshComponent offset");
    if (lambda[kBoneCall] != 0xE8) return Fail("no GetBoneSpaceTransforms call where expected");
    uint8_t* const bone = RipTarget(lambda + kBoneCall, 1, 5);
    if (!game.text.Contains(bone) || !MatchesAt(bone, ParsePattern(kBoneSpace)))
        return Fail("GetBoneSpaceTransforms does not start with the pending-task check");
    const uint32_t taskOff = *reinterpret_cast<const uint32_t*>(bone + kTaskOff);
    if (taskOff < 0x100 || taskOff > 0x4000) return Fail("unexpected parallel evaluation task offset");
    if (!beforeGameCode) return Fail("only installed when the game starts (version.dll route; not on the OBSE64 route)");
    // region: [0,16) jmp to the hook; [16,48) trampoline = the 9-byte prologue + jmp back to site+9
    uint8_t* const region = AllocNearGame(64);
    if (!region) return Fail("no free memory near the game's code");
    const int64_t rel = region - (site + 5);
    if (rel != static_cast<int32_t>(rel)) { VirtualFree(region, 0, MEM_RELEASE); return Fail("the hook is out of reach"); }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&UpdateHook);
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &hook, sizeof hook);
    uint8_t* const tramp = region + 16;
    memcpy(tramp, site, kPrologue);
    memcpy(tramp + kPrologue, jmpAbs, sizeof jmpAbs);
    uint8_t* const back = site + kPrologue;
    memcpy(tramp + kPrologue + sizeof jmpAbs, &back, sizeof back);
    DWORD old;
    if (!VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(region, 0, MEM_RELEASE);
        return Fail("could not make the trampoline executable");
    }
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    g_numOff = numOff;
    g_meshOff = meshOff;
    g_taskOff = taskOff;
    g_game = reinterpret_cast<UpdateFn>(tramp);
    g_tramp = tramp;
    uint8_t bytes[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(bytes + 1, &rel32, 4);
    if (!WriteCode(site, bytes)) {
        g_game = nullptr;
        g_tramp = nullptr;
        VirtualFree(region, 0, MEM_RELEASE);
        return Fail("could not patch the function");
    }
    g_installed = true;
    Log("[physanim] UpdateTargetActors exe+0x%X (lambda exe+0x%X, DriveData.Num +0x%X, mesh +0x%X; GetBoneSpaceTransforms "
        "exe+0x%X, pending parallel evaluation task +0x%X): installed",
        static_cast<unsigned>(site - game.base), static_cast<unsigned>(lambda - game.base), numOff, meshOff,
        static_cast<unsigned>(bone - game.base), taskOff);
    return true;
}

void SetPhysAnimNoEmptyLock(bool enabled)
{
    if (!g_installed || g_on.load() == enabled) return;
    g_on = enabled;
    Log("[physanim] %s", enabled ? "physical animation components that drive no bodies skip the scene write lock"
                               : "every physical animation component takes the scene write lock each frame, as before");
}

void LogPhysAnimNoEmptyLockStats()
{
    const uint64_t calls = s_calls.exchange(0), empty = s_empty.exchange(0), skipped = s_skipped.exchange(0);
    const uint64_t et = s_emptyTicks.exchange(0), ft = s_fullTicks.exchange(0), fc = s_fullCalls.exchange(0);
    if (!calls) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 / static_cast<double>(f.QuadPart);
    const uint64_t emptyRun = empty - skipped;
    const uint64_t pending = s_pending.exchange(0);
    Log("[physanim] since last report: %llu UpdateTargetActors calls, %llu with nothing to do (%llu skipped; the %llu run "
        "took %.3f ms, %.1f us each), %llu with driven bodies or a pending animation task (%llu pending; %.3f ms, %.1f us each)",
        calls, empty, skipped, emptyRun, et * ms, emptyRun ? et * ms * 1000.0 / emptyRun : 0.0, fc, pending, ft * ms,
        fc ? ft * ms * 1000.0 / fc : 0.0);
}

void PhysAnimTestCall(uint8_t* self, uint8_t teleport, void* fakeGame)
{
    const UpdateFn real = g_game;
    g_game = reinterpret_cast<UpdateFn>(fakeGame);
    UpdateHook(self, teleport);
    g_game = real;  // the trampoline again
}

const uint8_t* PhysAnimTestTrampoline() { return g_tramp; }
uint32_t PhysAnimTestOffset(int which) { return which == 0 ? g_numOff : which == 1 ? g_meshOff : g_taskOff; }

}  // namespace opf
