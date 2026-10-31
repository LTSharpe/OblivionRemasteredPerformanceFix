// ClothRegisterLite (and its lab-only first attempt ClothRegisterSkip): a robe's cloth simulation is no longer built in
// full for nothing when it is put on. The shipped key is ClothRegisterLite=3 (default; 0 off); ClothRegisterSkip, the
// learned modes 1 and 2 of ClothRegisterLite and ClothRegisterVerify are lab-only (the public build reads any other
// ClothRegisterLite value than 0 as 3, config.cpp).
//
// Altar dresses an NPC piece by piece: EquipBodyPart (exe+0x484CCD0) sets the slot's child actor class, which spawns the
// piece's actor and registers its components, then attaches it, sets the leader pose and calls the piece's Blueprint event
// OnAttachedToPhenotype (exe+0x46AB3E0). For a robe (any piece with a UChaosClothComponent) the cloth simulation proxy is
// built twice inside that one call: (a) by UChaosClothComponent::OnRegister (UE 5.3 ChaosClothAsset ClothComponent.cpp:
// Super::OnRegister, UpdateComponentSpaceTransforms, ResetConfigProperties, RecreateClothSimulationProxy,
// UpdateVisibility), and (b) again by the robe Blueprint, which calls the BlueprintCallable RecreateClothSimulationProxy.
// (b) starts with ClothSimulationProxy.Reset: proxy (a) is thrown away without ever having been ticked or rendered.
// A build is FClothSimulationProxy's constructor (own solver, context, mesh, colliders and configs; the solver's
// particles, constraint rules and triangle BVH: FClothingSimulationCloth::FLODData::Add) - about 5-6 ms of game thread
// per robe (2026-10-08 samples: 88% of a robe's equipment unit at Peryite's shrine, 55% at Stendarr's).
//
// The fix leaves out build (a) when (b) follows in the same EquipBodyPart call. Why the result is the same:
//  - The proxy is self-contained (no global state, no random numbers, no registration elsewhere) and its constructor
//    writes nothing outside itself; (b) builds the final proxy from the component's state at (b), as in the game.
//  - Between (a) and (b) the only readers of the proxy pointer (component+0x940) are, from the plugin's source and the exe:
//    HandleExistingParallelSimulation / OnPreEndOfFrameSync (no task in flight: nothing either way),
//    GetUpdateClothSimulationData_AnyThread (a proxy that never ticked has empty simulation data: the same empty data and
//    blend weight 0 as without one), IsSimulationSuspended / IsSimulationEnabled (Blueprint-callable) and
//    RequiresPreEndOfFrameSync (only test the pointer for null), and CalcBounds (reads the proxy's particle bounds).
//    While a build is left out, the pointer-testing readers are answered exactly as with a proxy: the pointer holds a
//    non-null placeholder for the duration of the call (none of them dereferences it). CalcBounds cannot be answered
//    that way: if it is called in the window, the proxy is built right then (a "late build", counted) and the class is
//    not skipped again.
//  - The skip is learned per robe class (owner actor class + component name): the first robe of a class is built as in
//    the game and watched; only if the Blueprint rebuilt it inside the same EquipBodyPart call and nothing read its
//    bounds in between are later robes of that class skipped. If a skipped robe is not rebuilt by the end of the call
//    (a Blueprint branch), it is built there ("fallback", counted) and its class is not skipped again.
// Cloth registered outside EquipBodyPart (other actors, other paths) is untouched.
//
// Live result (2026-10-14): nothing can be left out: the registration itself reads (a)'s bounds (render state creation's
// UpdateBounds), so (a) is part of the stock state. Hence the second mode, ClothRegisterLite
// (2026-10-20): build (a) "lite", without FClothConstraints::AddRules (the constraint rules; most of a build). Why the
// result is the same:
//  - Everything (a) shows before (b) throws it away is unchanged: the proxy pointer, IsSimulationSuspended/Enabled, the
//    empty simulation data, and the bounds, which are the AABB of the solver's active particle positions
//    (FClothingSimulationSolver::CalculateBounds). Those come from FLODData::Add's AddParticles / ResetStartPose before
//    AddRules and from the constructor's Solver->Update(0), which with a zero time step never steps the evolution (the
//    only place rules act); FClothConstraints::Update/Enable test every constraint pointer and offset, so a rule-less
//    cloth goes through the same code. AddRules writes only into this proxy's own FClothConstraints and evolution.
//  - A lite proxy is never ticked: the window is one EquipBodyPart call on the game thread (no tick runs inside it).
//  - Generalised (2026-10-20 live): a robe's Blueprint often rebuilds twice in the same call (Peryite's worshippers: (a)
//    at registration, then two in OnAttachedToPhenotype), so every build of a component inside one EquipBodyPart call
//    except the last is made lite: each is thrown away by the next build of the same call, never ticked.
//  - Learned per robe class (owner class + component name): the first two robes of a class are built in full and
//    watched; if both calls made >= 2 builds, later robes of the class get builds 1..n-1 lite (n = the fewer of the two
//    counts). A lite proxy still in place at the end of a call (fewer builds than learned: "lite fallback", counted, the
//    class is then built in full) is rebuilt in full there.
//  - Identical completion (2026-10-20): a lite build records its AddRules calls (arguments + the
//    LOD's particle X/InvM at that moment) and the constructor's FClothConstraints::Update calls; the lite proxy left at
//    the call's end is completed in place (X put back for AddRules, InvM required unchanged, the rules added per LOD in order, Enable for the
//    active particle ranges, the Update calls replayed) = the game's constraints, bit for bit (the lab's ClothRegisterVerify
//    compared it against a twin full build). Mode 3 makes every build of the call lite (no learning). Records of proxies
//    dropped inside the call (OnUnregister, a second registration) are retired: a rebuilt proxy usually gets the old
//    one's address, so the pointer alone is no identity test. See "identical completion" below.
//
// Sites (build 19115871 addresses in the comments), each found by pattern and cross-checked against the others: the
// RecreateClothSimulationProxy exec thunk from the class's native function table (name -> function), OnRegister's call to
// it, SetSkinnedAssetAndUpdate's call to it, the IsSimulationSuspended / IsSimulationEnabled exec thunks, the component's
// vtable slots for RequiresPreEndOfFrameSync and CalcBounds, and EquipBodyPart with its vtable slots and the one direct
// call. The native table entries are only read when the engine registers the class, so the fix is installed only while
// the game starts (version.dll); afterwards the ini switches it on and off live.
#include "common.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <vector>

namespace opf {
namespace {

// ---- code patterns ----

// Exec thunk of RecreateClothSimulationProxy (exe+0x53D00E0): P_FINISH (Stack.Code += Stack.Code != nullptr);
// jmp UChaosClothComponent::RecreateClothSimulationProxy.
constexpr char kExecRecreate[] = "48 8B 42 20 45 33 C0 48 85 C0 41 0F 95 C0 4C 03 C0 4C 89 42 20 E9";
constexpr size_t kExecRecreateJmp = 21;
// IsSimulationSuspended's exec thunk (exe+0x53D00B0): P_FINISH; call IsSimulationSuspended; mov [result],al.
constexpr char kExecSuspended[] =
    "40 53 48 83 EC 20 48 8B 42 20 45 33 C9 48 85 C0 49 8B D8 41 0F 95 C1 4C 03 C8 4C 89 4A 20 E8 ?? ?? ?? ?? 88 03";
constexpr size_t kExecSuspendedCall = 30;
// IsSimulationEnabled's exec thunk (exe+0x53CFFC0): IsSimulationEnabled inlined (cmp qword [rbx+proxy],0).
constexpr char kExecEnabled[] = "48 89 5C 24 08 57 48 83 EC 20 48 8B 42 20 45 33 C9 48 85 C0 48 8B D9";
// RecreateClothSimulationProxy (exe+0x53CD090): if (IsRegistered) ...: test byte [rcx+Registered],1.
constexpr char kRecreate[] = "40 57 48 83 EC 40 F6 81 ?? ?? 00 00 01 48 8B F9 0F 84";
constexpr size_t kRecreateRegisteredDisp = 8;
// IsSimulationSuspended (exe+0x53CCDB0): bSuspendSimulation || !ClothSimulationProxy.IsValid || ...
constexpr char kSuspendedBody[] = "F6 83 ?? ?? 00 00 08 75 ?? 48 83 BB ?? ?? 00 00 00 74";
constexpr size_t kSuspendedProxyDisp = 12, kSuspendedSearch = 0x60;
// UChaosClothComponent::OnRegister (exe+0x53CCF40): Super::OnRegister; UpdateComponentSpaceTransforms;
// ResetConfigProperties; RecreateClothSimulationProxy (call at +0x21); jmp UpdateVisibility.
constexpr char kOnRegister[] =
    "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? "
    "48 8B CB 48 83 C4 20 5B E9";
constexpr size_t kOnRegisterUpdateCall = 0x11, kOnRegisterResetCall = 0x19, kOnRegisterRecreateCall = 0x21;
// SetSkinnedAssetAndUpdate (exe+0x53CE6D0) at exe+0x53CE702: if (IsRegistered) { UpdateComponentSpaceTransforms;
// ResetConfigProperties; RecreateClothSimulationProxy (call at +0x1C) }
constexpr char kSetAsset[] = "F6 83 ?? ?? 00 00 01 74 ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B CB E8";
constexpr size_t kSetAssetUpdateCall = 0x0C, kSetAssetResetCall = 0x14, kSetAssetRecreateCall = 0x1C;
// RequiresPreEndOfFrameSync (exe+0x53CD370): !IsSimulationSuspended && !ShouldWaitForParallelSimulationInTickComponent
constexpr char kRequiresSync[] =
    "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8B CB E8 ?? ?? ?? ?? 84 C0 75 ?? B0 01";
constexpr size_t kRequiresSyncSuspendedCall = 9;
// CalcBounds (exe+0x53C7500): mov rax,rsp prologue; reads the proxy (mov rcx,[rbx+proxy]) for the bounds.
constexpr char kCalcBoundsStart[] = "48 8B C4 48 89 58 20 55 56 57 41 56 41 57";
constexpr size_t kCalcBoundsSearch = 0x300;
// UChaosClothComponent::OnUnregister (exe+0x53CCF80, the vtable slot after OnRegister's): Super::OnUnregister; then
// ClothSimulationProxy.Reset() (lea rcx,[rbp+proxy]).
constexpr char kOnUnregister[] = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B E9 E8 ?? ?? ?? ?? 48 8D 8D";
constexpr size_t kOnUnregisterProxyDisp = 31;
// Vtable slots relative to OnRegister's (slot 105, +0x348): RequiresPreEndOfFrameSync +0x448, CalcBounds +0x568.
constexpr ptrdiff_t kSlotRequiresSync = (0x448 - 0x348) / 8, kSlotCalcBounds = (0x568 - 0x348) / 8;
// EquipBodyPart (exe+0x484CCD0): GetChildActorComponent(slot); SetChildActorClass(cac, info->Class, false); ...
constexpr char kEquip[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 41 56 41 57 48 83 EC 70 49 8B E8 4C 8B F1 E8 ?? ?? ?? ?? "
    "48 8B 55 10 45 33 C0 48 8B C8 48 8B D8 E8";
// A subclass's override (exe+0x4823DC0) that calls EquipBodyPart directly (call at +0x1D), then sets visibility.
constexpr char kEquipCaller[] = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B F8 0F B6 DA 48 8B E9 E8";
constexpr size_t kEquipCallerCall = 0x1D;
// ClothRegisterLite: FClothingSimulationCloth::FLODData::Add's (exe+0x53A0060) call of FClothConstraints::AddRules
// (exe+0x53A2740) at exe+0x53A09EC: bEnabled false; Tethers; MeshScale; ...; mov rcx,rax (GetClothConstraints); call.
constexpr char kAddRulesCall[] =
    "C6 44 24 38 00 49 8D 4E ?? F3 44 0F 11 5C 24 30 4D 8D 44 24 10 48 89 4C 24 28 4D 8D 4E ?? 49 8D 4E ?? 4D 03 C7 "
    "48 89 4C 24 20 49 8B D5 48 8B C8 E8";
constexpr size_t kAddRulesCallAt = 48;
// AddRules' own start (saves rbx, rbp, rsi, rdi; push r14; sub rsp,40h).
constexpr char kAddRules[] = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40";
// Identical completion: AddRules ends with "jmp FClothConstraints::Enable" (exe+0x53ABC00, within its first 0x180
// bytes: ... pop r14; jmp). Enable tests ConstraintInitOffset +0x1E0 and takes the evolution +0x1B0, init view at evolution+0x560.
constexpr char kAddRulesTail[] = "48 83 C4 40 41 5E E9";
constexpr size_t kAddRulesTailSearch = 0x180;
constexpr char kCcEnable[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B6 F2 48 8B F9 8B 91 E0 01 00 00 83 FA FF 74 ?? 48 8B 89 B0 01 00 00 44 0F B6 C6 "
    "48 81 C1 60 05 00 00 E8";
// FLODData::Update's (exe+0x53C0550) call of FClothConstraints::Update (exe+0x53BE940) at exe+0x53C0651: lea r8,[rbx+18h] (weight
// maps); movss [rsp+20h],xmm1 (MaxDistancesScale); movaps xmm3,xmm0 (MeshScale); mov rcx,rsi; call. The only call of Update.
constexpr char kCcUpdateCall[] = "4C 8D 43 18 F3 0F 11 4C 24 20 0F 28 D8 48 8B CE E8";
constexpr size_t kCcUpdateCallAt = 16;
// FClothConstraints::Update's start: tests EdgeConstraints (+0x90) first.
constexpr char kCcUpdate[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B D9 0F 29 74 24 20 48 8B 89 90 00 00 00";
// FClothingSimulationSolver::ResetStartPose (exe+0x53BAB80): PAndInvM at evolution+0xC8 (16 bytes each), X at evolution+0x48
// (12 bytes each): shl rbp,4; lea r8,[rcx+198h]; add rbp,[r9+0C8h]; mov [rsp+38h],rbp; lea rdx,[rax+rax*2]; mov rax,[r9+48h].
constexpr char kResetStartPose[] = "48 C1 E5 04 4C 8D 81 98 01 00 00 49 03 A9 C8 00 00 00 48 89 6C 24 38 48 8D 14 40 49 8B 41 48";
// Right after the AddRules call FLODData::Add reads GetParticleInvMasses(Offset): mov rax,[rcx+0B0h]; lea rcx,[rax+rdx*4] (InvM at
// evolution+0xB0, 4 bytes each), within 0x40 bytes.
constexpr char kInvMRead[] = "48 8B 81 B0 00 00 00 48 8D 0C 90";
constexpr size_t kInvMReadSearch = 0x40;
// Layout checks (a review finding: the offsets the record and the completion read, derived from the game's code and
// required to equal the layout below; off on mismatch). AddRules calls its constraint factories with "mov rcx,r14; call"; the third
// such call (exe+0x53A4D50, within AddRules' first 0x80 bytes) passes NumParticles / ParticleOffset (mov r9d,[rdi+1DCh]; mov
// r8d,[rdi+1D8h]) and counts its constraint init and rule (inc dword [rdi+1ECh]; inc dword [rdi+1F0h]).
constexpr char kAddRulesFactoryCall[] = "49 8B CE E8";
constexpr size_t kAddRulesFactorySearch = 0x80, kFactorySearch = 0x400;
constexpr char kFactoryReadsRange[] = "44 8B 8F ?? ?? 00 00 44 8B 87 ?? ?? 00 00";
constexpr char kFactoryCounts[] = "FF 87 ?? ?? 00 00 FF 87 ?? ?? 00 00";
// FClothingSimulationSolver::EnableParticles (exe+0x53ABFE0 + 0x1A): Evolution (+0x68); the particle view's Ranges num (+0xE8) and
// data (+0xE0); binary search, flip; then GetClothConstraints and "jmp FClothConstraints::Enable" (within 0xB0 bytes).
constexpr char kEnableParticles[] =
    "48 8B 41 68 33 DB 41 0F B6 F0 8B EA 4C 8B F1 44 8B D3 44 8B 88 ?? ?? 00 00 48 8B B8 ?? ?? 00 00";
constexpr size_t kEnableParticlesNum = 0x15, kEnableParticlesData = 0x1C, kEnableParticlesSearch = 0xB0;
constexpr char kEnableParticlesTail[] = "48 83 C4 20 41 5E E9";

// Layout (build 19115871; the patterns above check the offsets they contain). FClothConstraints (0x1F8 bytes): 27 TSharedPtr
// constraint slots from +0 (9 deprecated, then Edge +0x90 ... SelfIntersection +0x1A0, as FClothConstraints::Update tests them),
// Evolution +0x1B0, ParticleOffset +0x1D8, NumParticles +0x1DC, ConstraintInit/Rule/PostCollisionRule offsets +0x1E0/+0x1E4/+0x1E8,
// their counts +0x1EC/+0x1F0/+0x1F4. FPBDEvolution: particle arrays (TArray {data, num, max}) X +0x48, V +0x68, M +0x98, InvM +0xB0,
// PAndInvM +0xC8; the active views' Ranges (TArray<int32>, signed range ends, > 0 = active): particles +0xE0, constraint inits +0x568,
// rules +0x590, post-collision rules +0x5B8.
constexpr size_t kCcSlots = 27, kCcEvolution = 0x1B0, kCcOffset = 0x1D8, kCcNum = 0x1DC, kCcInitOff = 0x1E0, kCcNumInits = 0x1EC,
                 kCcSize = 0x1F8;
constexpr size_t kEvoX = 0x48, kEvoV = 0x68, kEvoM = 0x98, kEvoInvM = 0xB0, kEvoPInvM = 0xC8, kEvoParticleRanges = 0xE0,
                 kEvoInitRanges = 0x568, kEvoRuleRanges = 0x590, kEvoPostRanges = 0x5B8;

// UObjectBase: ClassPrivate +0x10, NamePrivate +0x18, OuterPrivate +0x20; UStruct's class index +0x0C (InternalIndex).
constexpr size_t kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20, kObjIndex = 0x0C;

using RecreateFn = void (*)(uint8_t* component);
using ExecFn = void (*)(uint8_t* context, void* stack, void* result);
using BoolFn = bool (*)(uint8_t* component);
using CalcBoundsFn = void* (*)(uint8_t* component, void* out, const void* localToWorld);
using EquipFn = void (*)(void* character, void* a2, void* a3, void* a4);
using AddRulesFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using CcEnableFn = void (*)(uint8_t* constraints, bool enable);
using CcUpdateFn = void (*)(uint8_t* constraints, const void* props, const void* weightMaps, float meshScale, float maxDistancesScale);

struct Sites {
    uint8_t* recreate = nullptr;
    uint8_t* onRegisterCall = nullptr;
    uint8_t* setAssetCall = nullptr;
    void** execRecreateSlot = nullptr;
    void** execSuspendedSlot = nullptr;
    void** execEnabledSlot = nullptr;
    void** vtable = nullptr;  // the component's vtable slot of OnRegister
    void** requiresSyncSlot = nullptr;
    void** calcBoundsSlot = nullptr;
    void** onUnregisterSlot = nullptr;  // identical completion: proxies dropped by an unregistration inside the call
    uint8_t* equip = nullptr;
    void** equipSlots[4] = {};
    int numEquipSlots = 0;
    uint8_t* equipCall = nullptr;  // the direct call, or nullptr
    uint8_t* addRulesCall = nullptr;  // ClothRegisterLite: FLODData::Add's call of AddRules, or nullptr (lite unavailable)
    uint8_t* ccEnable = nullptr;      // identical completion: FClothConstraints::Enable(bool)
    uint8_t* ccUpdateCall = nullptr;  // FLODData::Update's call of FClothConstraints::Update, or nullptr (lite unavailable)
    void* clothVtable = nullptr;   // a UChaosClothComponent's vtable pointer
    uint32_t proxyOff = 0, registeredOff = 0;
} g;

RecreateFn g_recreate = nullptr;
ExecFn g_execRecreate = nullptr, g_execSuspended = nullptr, g_execEnabled = nullptr;
BoolFn g_requiresSync = nullptr;
RecreateFn g_onUnregister = nullptr;
CalcBoundsFn g_calcBounds = nullptr;
void* g_calcBoundsGame = nullptr;   // the game's CalcBounds as found at installation (for ClothBuildDefer; test set-ups leave it)
EquipFn g_equip = nullptr;
AddRulesFn g_addRules = nullptr;
CcEnableFn g_ccEnable = nullptr;
CcUpdateFn g_ccUpdate = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{false};
std::atomic<bool> g_lite{false};  // ClothRegisterLite (wins over the skip mode when both are on)
std::atomic<int> g_liteMode{1};   // 1 = every build of the call but the last lite (learned); 2 = only the registration build;
                                  // 3 = every build of the call lite, the one left at the call's end completed in place
std::atomic<bool> g_verify{false};  // ClothRegisterVerify (lab builds only): every lite build also built in full (twin) and compared after completion
bool g_liteBuilding = false;      // game thread: inside a lite build (a); AddRules is left out

// ---- learning per robe class ----
enum class Verdict : uint8_t { kLearning, kSkip, kKeep };
struct ClassKey { const uint8_t* cls; int32_t clsIndex; uint64_t name; };
struct ClassEntry {
    ClassKey key;
    Verdict verdict;  // skip mode
    uint32_t seen;
    uint32_t builds;  // full builds, timed
    uint64_t buildUs;
    Verdict lite;     // lite mode: kSkip = every build of a call but the last is lite
    uint32_t liteBuilds;
    uint64_t liteUs;
    int callBuilds;   // lite mode: builds of one component in one EquipBodyPart call, learned (>= 2 for kSkip; the fewest seen)
    int liteObs;      // lite mode: learning calls seen
    char seqs[2][12]; // lite mode: the learning calls' build paths
};
constexpr int kMaxClasses = 512;
ClassEntry g_classes[kMaxClasses];
int g_numClasses = 0;

// ---- the open EquipBodyPart calls (game thread only) ----
struct Pending {
    uint8_t* comp;
    int cls;          // index into g_classes, or -1
    bool skipped;     // build (a) left out
    bool learning;    // built as in the game, watched
    bool rebuilt;     // RecreateClothSimulationProxy ran for it since (a)
    bool boundsRead;  // CalcBounds was called before the rebuild
    bool lite;        // lite mode entry
    bool lastLite;    // lite mode: the latest build of this call was lite (no constraint rules)
    int builds;       // lite mode: builds of this component in this call so far (registration = 1)
    char seq[12];     // lite mode: the builds' paths in order: R registration, E Blueprint Recreate, S SetSkinnedAsset
    void* lastProxy;  // lite mode: the proxy object the latest build left in the component
    int rec = -1;     // lite mode: the latest lite build's record (g_recs), or -1
};
constexpr int kMaxPending = 64;
Pending g_pending[kMaxPending];
int g_numPending = 0;
int g_depth = 0;
DWORD g_equipThread = 0;
alignas(64) uint8_t g_placeholder[64];  // the non-null stand-in for a proxy left out (never dereferenced)

// ---- statistics ----
std::atomic<uint64_t> s_registers{0}, s_skipped{0}, s_learning{0}, s_kept{0}, s_rebuiltAfterSkip{0}, s_lateBuilds{0},
    s_fallbacks{0}, s_readsSync{0}, s_readsSuspended{0}, s_readsEnabled{0}, s_savedUs{0};
// lite mode
std::atomic<uint64_t> s_liteRegisters{0}, s_liteBuilds{0}, s_liteLearning{0}, s_liteKept{0}, s_liteReplaced{0},
    s_liteFallbacks{0}, s_rulesLeftOut{0}, s_liteUs{0}, s_liteSavedUs{0};

uint64_t NowUs()
{
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return QpcToMicroseconds(static_cast<uint64_t>(t.QuadPart), static_cast<uint64_t>(freq.QuadPart));
}

template <typename T> T Read(const uint8_t* p, size_t off) { T v; memcpy(&v, p + off, sizeof v); return v; }
void*& ProxyOf(uint8_t* comp) { return *reinterpret_cast<void**>(comp + g.proxyOff); }
bool Registered(const uint8_t* comp) { return (comp[g.registeredOff] & 1) != 0; }

bool KeyOf(const uint8_t* comp, ClassKey* key)
{
    const uint8_t* const outer = Read<const uint8_t*>(comp, kObjOuter);
    if (!outer) return false;
    const uint8_t* const cls = Read<const uint8_t*>(outer, kObjClass);
    if (!cls) return false;
    key->cls = cls;
    key->clsIndex = Read<int32_t>(cls, kObjIndex);
    key->name = Read<uint64_t>(comp, kObjName);
    return true;
}

int ClassIndex(const uint8_t* comp)
{
    ClassKey k;
    if (!KeyOf(comp, &k)) return -1;
    for (int i = 0; i < g_numClasses; ++i) {
        const ClassKey& e = g_classes[i].key;
        if (e.cls == k.cls && e.clsIndex == k.clsIndex && e.name == k.name) return i;
    }
    if (g_numClasses == kMaxClasses) return -1;
    g_classes[g_numClasses] = ClassEntry{k, Verdict::kLearning, 0, 0, 0, Verdict::kLearning, 0, 0, 0, 0};
    return g_numClasses++;
}

void SetVerdict(int i, Verdict v, const char* why)
{
    if (i < 0 || g_classes[i].verdict == v) return;
    const Verdict old = g_classes[i].verdict;
    g_classes[i].verdict = v;
    const ClassEntry& e = g_classes[i];
    Log("[clothreg] robe class %d (class object %p, index %d, component name %u_%u): %s -> %s (%s)", i,
        static_cast<const void*>(e.key.cls), e.key.clsIndex, static_cast<unsigned>(e.key.name & 0xFFFFFFFF),
        static_cast<unsigned>(e.key.name >> 32), old == Verdict::kLearning ? "learning" : old == Verdict::kSkip ? "skip" : "keep",
        v == Verdict::kSkip ? "skip" : "keep", why);
}

// The open entry for this component in the current EquipBodyPart calls (most recent first), or nullptr.
Pending* Open(const uint8_t* comp)
{
    for (int i = g_numPending - 1; i >= 0; --i)
        if (g_pending[i].comp == comp) return &g_pending[i];
    return nullptr;
}

// Left out and not yet rebuilt: the proxy pointer is null although the game would have one.
Pending* Waiting(const uint8_t* comp)
{
    if (g_numPending == 0 || GetCurrentThreadId() != g_equipThread) return nullptr;
    Pending* p = Open(comp);
    return p && p->skipped && !p->rebuilt ? p : nullptr;
}

void TimedRecreate(uint8_t* comp, int cls)
{
    const uint64_t t0 = NowUs();
    g_recreate(comp);
    const uint64_t us = NowUs() - t0;
    if (cls >= 0) {
        ++g_classes[cls].builds;
        g_classes[cls].buildUs += us;
    }
}

inline void LabSnap(Pending&) {}
inline void LabBoundsRead(const Pending&) {}

void SetLiteVerdict(int i, Verdict v, const char* why)
{
    if (i < 0 || g_classes[i].lite == v) return;
    const Verdict old = g_classes[i].lite;
    g_classes[i].lite = v;
    const ClassEntry& e = g_classes[i];
    Log("[clothreg] robe class %d (class object %p, index %d, component name %u_%u): lite %s -> %s (%s)", i,
        static_cast<const void*>(e.key.cls), e.key.clsIndex, static_cast<unsigned>(e.key.name & 0xFFFFFFFF),
        static_cast<unsigned>(e.key.name >> 32), old == Verdict::kLearning ? "learning" : old == Verdict::kSkip ? "lite" : "full",
        v == Verdict::kSkip ? "lite" : "full", why);
}

// ---- hooks ----

// ---- identical completion (2026-10-20) ----
// A lite build records, per LOD, FLODData::Add's AddRules call (the constraints object and the six arguments, all of them either
// values or references to objects the proxy owns and never changes before it is ticked: its own config copy, the LOD's triangle
// mesh, pattern data, weight maps and tethers) together with what the constraint constructors read from the solver particles: X
// (rest lengths/angles come from it) and InvM (kinematic trimming) over the LOD's particle range, as they were at that call. It also
// records the constructor's FClothConstraints::Update calls (FLODData::Update; their arguments are references to the same objects
// plus MeshScale and MaxDistancesScale by value). Completion = for each LOD in order: X put back to the recorded values (InvM must
// still equal the recorded masses, else no completion), AddRules with the recorded arguments, X back to its current values; then each LOD's constraints Enable(true) if its
// particle range is active (in the game every Enable after AddRules comes from EnableParticles, so the constraints' final flag
// is the particle range's flag); then the recorded Update calls in their order. Same inputs, same order (the evolution's
// constraint ranges get the same offsets) = the game's constraints. See docs/research.md, ClothRegisterLite.
struct LodRec {
    uint8_t* cc;          // FClothConstraints
    uint64_t args[7];     // AddRules' arguments 2..8 (raw: references, MeshScale float in the low half, bEnabled)
    uint8_t* evo;
    int32_t offset, num;
    std::vector<uint8_t> x, invm;
};
struct UpdRec { uint8_t* cc; const void* props; const void* weightMaps; float meshScale, maxDistancesScale; };
// Growable (a review finding): no LOD or Update count can make a record incomplete; the vectors keep their capacity across reuse
// of the record (g_recs), so steady-state dressing allocates nothing new.
struct LiteRec {
    bool used = false, bad = false, completed = false;
    uint8_t* comp = nullptr;
    void* proxy = nullptr;
    void* ctrl = nullptr;
    int numLods = 0, numUpd = 0;
    std::vector<LodRec> lods;  // [0, numLods) in use
    std::vector<UpdRec> upd;   // [0, numUpd) in use
    // verify: the full build made right after the lite one (its own TSharedPtr reference) and its constraints per LOD
    void* tProxy = nullptr;
    void* tCtrl = nullptr;
    int tLods = 0;
    std::vector<uint8_t*> tcc;
    std::vector<const uint8_t*> tTethers;
};
constexpr int kMaxRecs = 64;
LiteRec g_recs[kMaxRecs];
LiteRec* g_curRec = nullptr;   // the lite build in progress (game thread)
LiteRec* g_twinRec = nullptr;  // verify: the twin build in progress
std::atomic<uint64_t> s_completed{0}, s_completeFail{0}, s_completeUs{0}, s_twins{0}, s_verified{0}, s_mismatches{0},
    s_verifyReplaced{0}, s_ticksLite{0};

void*& CtrlOf(uint8_t* comp) { return *reinterpret_cast<void**>(comp + g.proxyOff + 8); }

// A TSharedPtr reference we own, released as UE does (shared count +8, weak +0xC; DestroyObject slot 0, destructor slot 1).
void ReleaseShared(void* ctrl)
{
    if (!ctrl) return;
    auto* c = static_cast<uint8_t*>(ctrl);
    using Fn0 = void (*)(void*);
    using Fn1 = void (*)(void*, uint32_t);
    if (_InterlockedExchangeAdd(reinterpret_cast<volatile long*>(c + 8), -1) == 1) {
        (*reinterpret_cast<Fn0**>(c))[0](c);
        if (_InterlockedExchangeAdd(reinterpret_cast<volatile long*>(c + 0xC), -1) == 1) (*reinterpret_cast<Fn1**>(c))[1](c, 1);
    }
}

LiteRec* AllocRec(uint8_t* comp)
{
    for (LiteRec& r : g_recs)
        if (!r.used) {
            r.used = true; r.bad = false; r.completed = false; r.comp = comp; r.proxy = r.ctrl = nullptr;
            r.numLods = r.numUpd = 0; r.tProxy = r.tCtrl = nullptr; r.tLods = 0;
            return &r;
        }
    return nullptr;
}

void FreeRec(LiteRec& r)
{
    if (r.tCtrl) ReleaseShared(r.tCtrl);
    r.tProxy = r.tCtrl = nullptr;
    r.used = false;
}

template <typename T> T* ArrayData(uint8_t* evo, size_t off) { return *reinterpret_cast<T**>(evo + off); }
int32_t ArrayNum(const uint8_t* evo, size_t off) { return Read<int32_t>(evo, off + 8); }

bool RangeOk(uint8_t* evo, int32_t offset, int32_t num)
{
    // A LOD without particles (num 0) records nothing to put back; otherwise both arrays must cover the range.
    return evo && offset >= 0 && num >= 0 &&
           (num == 0 || (ArrayData<uint8_t>(evo, kEvoX) && ArrayData<uint8_t>(evo, kEvoInvM) && ArrayNum(evo, kEvoX) >= offset + num &&
                         ArrayNum(evo, kEvoInvM) >= offset + num));
}

// Inside a lite build: FLODData::Add's AddRules call, recorded instead of made.
void RecordRules(LiteRec& r, uint64_t a1, const uint64_t* rest)
{
    if (static_cast<size_t>(r.numLods) == r.lods.size()) r.lods.emplace_back();
    LodRec& l = r.lods[r.numLods++];
    l.cc = reinterpret_cast<uint8_t*>(a1);
    memcpy(l.args, rest, sizeof l.args);
    l.evo = Read<uint8_t*>(l.cc, kCcEvolution);
    l.offset = Read<int32_t>(l.cc, kCcOffset);
    l.num = Read<int32_t>(l.cc, kCcNum);
    if (!RangeOk(l.evo, l.offset, l.num)) { r.bad = true; return; }
    l.x.clear();
    l.invm.clear();
    if (l.num == 0) return;
    const auto* x = ArrayData<uint8_t>(l.evo, kEvoX) + static_cast<size_t>(l.offset) * 12;
    const auto* im = ArrayData<uint8_t>(l.evo, kEvoInvM) + static_cast<size_t>(l.offset) * 4;
    l.x.assign(x, x + static_cast<size_t>(l.num) * 12);
    l.invm.assign(im, im + static_cast<size_t>(l.num) * 4);
}

// The constraints object of a lite build still has no rules: no constraint object, no evolution ranges.
bool StillEmpty(const LodRec& l)
{
    static const uint8_t zero[kCcSlots * 16] = {};
    return memcmp(l.cc, zero, sizeof zero) == 0 && Read<int32_t>(l.cc, kCcInitOff) == -1 && Read<int32_t>(l.cc, kCcInitOff + 4) == -1 &&
           Read<int32_t>(l.cc, kCcInitOff + 8) == -1 && Read<int32_t>(l.cc, kCcNumInits) == 0 && Read<int32_t>(l.cc, kCcNumInits + 4) == 0 &&
           Read<int32_t>(l.cc, kCcNumInits + 8) == 0 && Read<uint8_t*>(l.cc, kCcEvolution) == l.evo && Read<int32_t>(l.cc, kCcOffset) == l.offset &&
           Read<int32_t>(l.cc, kCcNum) == l.num;
}

// TPBDActiveView: is the range starting at offset active (Ranges = signed range ends, as EnableParticles' search).
bool RangeActive(uint8_t* evo, size_t rangesOff, int32_t offset)
{
    const int32_t* ranges = ArrayData<int32_t>(evo, rangesOff);
    const int32_t n = ArrayNum(evo, rangesOff);
    for (int32_t i = 0; i < n; ++i)
        if ((ranges[i] < 0 ? -ranges[i] : ranges[i]) > offset) return ranges[i] > 0;
    return false;
}

int g_completeLogs = 0;
// Completes a lite proxy in place (game thread). False (nothing changed) if it cannot be done identically.
bool Complete(LiteRec& r, const char** why)
{
    *why = nullptr;
    if (r.completed) return true;
    if (r.bad) *why = "record incomplete (unexpected particle arrays)";
    else if (ProxyOf(r.comp) != r.proxy || CtrlOf(r.comp) != r.ctrl) *why = "proxy replaced";
    for (int i = 0; !*why && i < r.numLods; ++i) {
        const LodRec& l = r.lods[i];
        if (!StillEmpty(l)) *why = "constraints no longer empty";
        else if (!RangeOk(l.evo, l.offset, l.num)) *why = "particle arrays changed";
        else if (l.num && memcmp(ArrayData<uint8_t>(l.evo, kEvoInvM) + static_cast<size_t>(l.offset) * 4, l.invm.data(), l.invm.size()) != 0)
            *why = "particle masses changed since the build";
    }
    if (*why) return false;
    const uint64_t t0 = NowUs();
    std::vector<uint8_t> cur;
    for (int i = 0; i < r.numLods; ++i) {
        LodRec& l = r.lods[i];
        if (l.num) {
            uint8_t* const x = ArrayData<uint8_t>(l.evo, kEvoX) + static_cast<size_t>(l.offset) * 12;
            cur.assign(x, x + l.x.size());
            memcpy(x, l.x.data(), l.x.size());
        }
        g_addRules(reinterpret_cast<uint64_t>(l.cc), l.args[0], l.args[1], l.args[2], l.args[3], l.args[4], l.args[5], l.args[6]);
        if (l.num) memcpy(ArrayData<uint8_t>(l.evo, kEvoX) + static_cast<size_t>(l.offset) * 12, cur.data(), cur.size());
    }
    for (int i = 0; i < r.numLods; ++i)
        if (RangeActive(r.lods[i].evo, kEvoParticleRanges, r.lods[i].offset)) g_ccEnable(r.lods[i].cc, true);
    for (int i = 0; i < r.numUpd; ++i) {
        const UpdRec& u = r.upd[i];
        g_ccUpdate(u.cc, u.props, u.weightMaps, u.meshScale, u.maxDistancesScale);
    }
    r.completed = true;
    s_completed.fetch_add(1, std::memory_order_relaxed);
    s_completeUs.fetch_add(NowUs() - t0, std::memory_order_relaxed);
    return true;
}

// FLODData::Add's call of FClothConstraints::AddRules: recorded (left out) inside a lite build, else passed through.
uint64_t AddRulesHook(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
{
    if (GetCurrentThreadId() == g_equipThread) {
        if (g_liteBuilding && g_curRec) {
            const uint64_t rest[7] = {a2, a3, a4, a5, a6, a7, a8};
            RecordRules(*g_curRec, a1, rest);
            s_rulesLeftOut.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        if (g_twinRec) {
            if (static_cast<size_t>(g_twinRec->tLods) == g_twinRec->tcc.size()) {
                g_twinRec->tcc.push_back(nullptr);
                g_twinRec->tTethers.push_back(nullptr);
            }
            g_twinRec->tcc[g_twinRec->tLods] = reinterpret_cast<uint8_t*>(a1);
            g_twinRec->tTethers[g_twinRec->tLods] = reinterpret_cast<const uint8_t*>(a6);
            ++g_twinRec->tLods;
        }
    }
    return g_addRules(a1, a2, a3, a4, a5, a6, a7, a8);
}

// FLODData::Update's call of FClothConstraints::Update: recorded inside a lite build (and made: on the empty constraints it does
// nothing, as in the lite build before), else passed through.
void CcUpdateHook(uint8_t* cc, const void* props, const void* weightMaps, float meshScale, float maxDistancesScale)
{
    if (g_liteBuilding && g_curRec && GetCurrentThreadId() == g_equipThread) {
        LiteRec& r = *g_curRec;
        if (static_cast<size_t>(r.numUpd) == r.upd.size()) r.upd.emplace_back();
        r.upd[r.numUpd++] = UpdRec{cc, props, weightMaps, meshScale, maxDistancesScale};
    }
    g_ccUpdate(cc, props, weightMaps, meshScale, maxDistancesScale);
}

void Compare(LiteRec& r, const char* where);  // verify (below)

// Verify: the full build the game would have made at this moment, made right after the lite one and kept aside.
void MakeTwin(LiteRec& r)
{
    void*& slot = ProxyOf(r.comp);
    void*& ctrl = CtrlOf(r.comp);
    void* const p0 = slot;
    void* const c0 = ctrl;
    slot = nullptr;
    ctrl = nullptr;
    g_twinRec = &r;
    r.tLods = 0;
    g_recreate(r.comp);
    g_twinRec = nullptr;
    r.tProxy = slot;
    r.tCtrl = ctrl;
    slot = p0;
    ctrl = c0;
    s_twins.fetch_add(1, std::memory_order_relaxed);
}

constexpr int kLiteObservations = 2;  // learning calls before a robe class is built lite

// Lite mode: one build of a component inside an EquipBodyPart call (game thread). Build k of a call is lite when the
// class has learned that its calls make n >= 2 builds and k < n: a later build of the same call replaces it.
// The component's previous lite build of this call is about to be replaced (or the call ends without completing it): its record
// is dropped (verify: completed and compared with its twin first; the proxy is thrown away by the game right after).
void DropRec(Pending& p, bool verifyFirst)
{
    if (p.rec < 0) return;
    LiteRec& r = g_recs[p.rec];
    if (verifyFirst && g_verify.load(std::memory_order_relaxed) && r.tProxy && ProxyOf(r.comp) == r.proxy) {
        const char* why;
        if (Complete(r, &why)) Compare(r, "replaced");
    }
    FreeRec(r);
    p.rec = -1;
}

template <typename F> void LiteBuild(Pending& p, F&& build, char path)
{
    if (p.builds < 11) { p.seq[p.builds] = path; p.seq[p.builds + 1] = 0; }
    ++p.builds;
    const int cls = p.cls;
    const int mode = g_liteMode.load(std::memory_order_relaxed);
    const int maxLite = mode == 2 ? 1 : (cls >= 0 ? g_classes[cls].callBuilds - 1 : 0);
    bool lite = g.addRulesCall && Registered(p.comp) &&
                (mode == 3 ? (g.ccUpdateCall != nullptr && g_ccEnable != nullptr)
                           : cls >= 0 && g_classes[cls].lite == Verdict::kSkip && p.builds <= maxLite);
    DropRec(p, true);
    LiteRec* rec = lite ? AllocRec(p.comp) : nullptr;
    if (!rec) lite = false;
    const uint64_t t0 = NowUs();
    if (lite) { g_liteBuilding = true; g_curRec = rec; }
    build();
    g_liteBuilding = false;
    g_curRec = nullptr;
    const uint64_t us = NowUs() - t0;
    p.lastLite = lite;
    p.lastProxy = ProxyOf(p.comp);
    if (rec) {
        rec->proxy = p.lastProxy;
        rec->ctrl = CtrlOf(p.comp);
        if (!rec->proxy) {
            FreeRec(*rec);
        } else {
            p.rec = static_cast<int>(rec - g_recs);
            if (g_verify.load(std::memory_order_relaxed)) MakeTwin(*rec);
        }
    }
    if (cls < 0) return;
    if (lite) {
        ++g_classes[cls].liteBuilds;
        g_classes[cls].liteUs += us;
        s_liteBuilds.fetch_add(1, std::memory_order_relaxed);
        s_liteUs.fetch_add(us, std::memory_order_relaxed);
        if (g_classes[cls].builds) {
            const uint64_t full = g_classes[cls].buildUs / g_classes[cls].builds;
            if (full > us) s_liteSavedUs.fetch_add(full - us, std::memory_order_relaxed);
        }
    } else if (p.lastProxy) {  // a full build that made a proxy: its time is the class's reference
        ++g_classes[cls].builds;
        g_classes[cls].buildUs += us;
    }
}

// An entry whose component was unregistered (its proxy dropped) or registered again inside the call: its lite record is dropped
// and it is no longer completed (the proxy it describes is gone; a new one may sit at the same address).
void Retire(Pending& p)
{
    DropRec(p, false);
    p.lastLite = false;
    p.lastProxy = nullptr;
}

// UChaosClothComponent::OnUnregister (the component's vtable slot): the proxy is reset there.
void OnUnregisterHook(uint8_t* comp)
{
    if (g_numPending && GetCurrentThreadId() == g_equipThread)
        for (int i = 0; i < g_numPending; ++i)
            if (g_pending[i].comp == comp && g_pending[i].lite) Retire(g_pending[i]);
    g_onUnregister(comp);
}

// Lite mode's part of OnRegister's call (game thread, inside EquipBodyPart, a cloth component): the call's first build.
void LiteRegister(uint8_t* comp)
{
    s_liteRegisters.fetch_add(1, std::memory_order_relaxed);
    const int cls = ClassIndex(comp);
    if (cls >= 0) ++g_classes[cls].seen;
    Pending p{comp, cls, false, false, false, false};
    p.lite = true;
    if (cls >= 0 && g_classes[cls].lite == Verdict::kLearning) s_liteLearning.fetch_add(1, std::memory_order_relaxed);
    if (ProxyOf(comp) != nullptr) {  // already has a proxy (re-registration): as in the game, not tracked
        for (int i = 0; i < g_numPending; ++i)
            if (g_pending[i].comp == comp && g_pending[i].lite) Retire(g_pending[i]);  // replaced here
        g_recreate(comp);
        s_liteKept.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (int i = 0; i < g_numPending; ++i)
        if (g_pending[i].comp == comp && g_pending[i].lite) Retire(g_pending[i]);  // registered again in the same call
    g_pending[g_numPending++] = p;
    Pending& e = g_pending[g_numPending - 1];
    LiteBuild(e, [&] { g_recreate(comp); }, 'R');
}

// A rebuild inside an open EquipBodyPart call (the robe Blueprint's RecreateClothSimulationProxy, SetSkinnedAsset).
// Not gated on g_lite: an entry that started lite stays lite until the call's end completes it (CloseCalls does that
// whatever the key says), so switching the key off in the middle of a call cannot let a full rebuild free a lite proxy
// whose completion is still pending (a review finding: use-after-free read, then a non-identical fallback).
// The key only decides whether new entries start lite (OnRegisterRecreateHook).
Pending* LiteOpen(const uint8_t* comp)
{
    if (g_numPending == 0 || GetCurrentThreadId() != g_equipThread) return nullptr;
    Pending* p = Open(comp);
    return p && p->lite ? p : nullptr;
}

// OnRegister's call to RecreateClothSimulationProxy.
void OnRegisterRecreateHook(uint8_t* comp)
{
    const bool lite = g_lite.load(std::memory_order_relaxed);
    if ((!g_on.load(std::memory_order_relaxed) && !lite) || g_depth == 0 || GetCurrentThreadId() != g_equipThread ||
        *reinterpret_cast<void**>(comp) != g.clothVtable || g_numPending == kMaxPending) {
        g_recreate(comp);
        return;
    }
    if (lite) {
        LiteRegister(comp);
        return;
    }
    s_registers.fetch_add(1, std::memory_order_relaxed);
    const int cls = ClassIndex(comp);
    const Verdict v = cls >= 0 ? g_classes[cls].verdict : Verdict::kKeep;
    if (cls >= 0) ++g_classes[cls].seen;
    if (v == Verdict::kSkip && ProxyOf(comp) == nullptr && Registered(comp)) {
        g_pending[g_numPending++] = Pending{comp, cls, true, false, false, false};
        LabSnap(g_pending[g_numPending - 1]);
        s_skipped.fetch_add(1, std::memory_order_relaxed);
        if (g_classes[cls].builds) s_savedUs.fetch_add(g_classes[cls].buildUs / g_classes[cls].builds, std::memory_order_relaxed);
        return;
    }
    TimedRecreate(comp, cls);
    if (v == Verdict::kLearning) {
        g_pending[g_numPending++] = Pending{comp, cls, false, true, false, false};
        LabSnap(g_pending[g_numPending - 1]);
        s_learning.fetch_add(1, std::memory_order_relaxed);
    } else {
        s_kept.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteRebuild(uint8_t* comp)
{
    if (g_numPending == 0 || GetCurrentThreadId() != g_equipThread) return;
    if (Pending* p = Open(comp)) {
        if (!p->rebuilt && p->skipped) s_rebuiltAfterSkip.fetch_add(1, std::memory_order_relaxed);
        p->rebuilt = true;
    }
}

void TrackRebuild(uint8_t* comp);  // lab b-last tracking (below)
void TrackBounds(uint8_t* comp);

// SetSkinnedAssetAndUpdate's call to RecreateClothSimulationProxy (a rebuild as well).
void SetAssetRecreateHook(uint8_t* comp)
{
    if (Pending* p = LiteOpen(comp)) {
        p->rebuilt = true;
        LiteBuild(*p, [&] { g_recreate(comp); }, 'S');
        return;
    }
    if (g_lite.load(std::memory_order_relaxed)) TrackRebuild(comp);
    NoteRebuild(comp);
    g_recreate(comp);
}

// Blueprint: RecreateClothSimulationProxy (the robe's (b)).
void ExecRecreateHook(uint8_t* context, void* stack, void* result)
{
    if (Pending* p = LiteOpen(context)) {
        p->rebuilt = true;
        LiteBuild(*p, [&] { g_execRecreate(context, stack, result); }, 'E');
        return;
    }
    if (g_lite.load(std::memory_order_relaxed)) TrackRebuild(context);
    NoteRebuild(context);
    g_execRecreate(context, stack, result);
}

// The pointer-testing readers: a placeholder stands in for the left-out proxy during the call.
template <typename F> auto WithPlaceholder(uint8_t* comp, F&& call)
{
    void*& slot = ProxyOf(comp);
    slot = g_placeholder;
    struct Restore { void*& s; ~Restore() { s = nullptr; } } restore{slot};
    return call();
}

void ExecSuspendedHook(uint8_t* context, void* stack, void* result)
{
    if (Waiting(context)) {
        s_readsSuspended.fetch_add(1, std::memory_order_relaxed);
        WithPlaceholder(context, [&] { g_execSuspended(context, stack, result); return 0; });
        return;
    }
    g_execSuspended(context, stack, result);
}

void ExecEnabledHook(uint8_t* context, void* stack, void* result)
{
    if (Waiting(context)) {
        s_readsEnabled.fetch_add(1, std::memory_order_relaxed);
        WithPlaceholder(context, [&] { g_execEnabled(context, stack, result); return 0; });
        return;
    }
    g_execEnabled(context, stack, result);
}

bool RequiresSyncHook(uint8_t* comp)
{
    if (Waiting(comp)) {
        s_readsSync.fetch_add(1, std::memory_order_relaxed);
        return WithPlaceholder(comp, [&] { return g_requiresSync(comp); });
    }
    return g_requiresSync(comp);
}

void* CalcBoundsHook(uint8_t* comp, void* out, const void* localToWorld)
{
    if (g_lite.load(std::memory_order_relaxed) && !Open(comp)) TrackBounds(comp);
    if (g_numPending && GetCurrentThreadId() == g_equipThread) {
        if (Pending* p = Open(comp); p && !p->rebuilt) {
            LabBoundsRead(*p);
            p->boundsRead = true;
            if (p->skipped) {
                // The game would read proxy (a)'s bounds here: build it now (closest to the game), and keep this class.
                s_lateBuilds.fetch_add(1, std::memory_order_relaxed);
                SetVerdict(p->cls, Verdict::kKeep, "bounds read before the Blueprint rebuilt the proxy (late build)");
                if (Registered(comp) && ProxyOf(comp) == nullptr) TimedRecreate(comp, p->cls);
                p->skipped = false;
                p->learning = false;
            }
        }
    }
    return g_calcBounds(comp, out, localToWorld);
}

void TrackAdd(uint8_t*, int) {}
void TrackRebuild(uint8_t*) {}
void TrackBounds(uint8_t*) {}

// ---- verify (ClothRegisterVerify, lab builds only): the completed lite proxy against its twin, bit for bit ----
struct Diff {
    int n = 0;
    char first[10][200];
    void Add(const char* fmt, ...)
    {
        if (n < 10) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(first[n], sizeof first[n], fmt, ap);
            va_end(ap);
        }
        ++n;
    }
};
int g_verifyLogs = 0;
uint32_t g_slotsSeen = 0;

// TArray {data, int32 num, int32 max} at a/b + off: same num and the same num*elem bytes.
void CmpArray(Diff& d, const uint8_t* a, const uint8_t* b, size_t off, size_t elem, const char* what, int lod)
{
    const int32_t na = Read<int32_t>(a, off + 8), nb = Read<int32_t>(b, off + 8);
    if (na != nb) { d.Add("LOD %d %s: count %d vs %d", lod, what, na, nb); return; }
    if (na <= 0) return;
    const auto* pa = Read<const uint8_t*>(a, off);
    const auto* pb = Read<const uint8_t*>(b, off);
    if (!pa || !pb) { d.Add("LOD %d %s: null data", lod, what); return; }
    if (memcmp(pa, pb, static_cast<size_t>(na) * elem) != 0) {
        size_t i = 0;
        while (memcmp(pa + i * elem, pb + i * elem, elem) == 0) ++i;
        d.Add("LOD %d %s: element %zu of %d differs", lod, what, i, na);
    }
}

struct CmpCtx { const uint8_t* ccA; const uint8_t* ccB; const uint8_t* tethersA; const uint8_t* tethersB; int lod, slot; };
void CompareConstraintObjects(Diff& d, const CmpCtx& c, const uint8_t* oa, const uint8_t* ob);  // per type (below)

void Compare(LiteRec& r, const char* where)
{
    if (!r.tProxy) return;
    Diff d;
    if (r.tLods != r.numLods) d.Add("LOD count %d (lite) vs %d (twin)", r.numLods, r.tLods);
    const uint8_t* evoA = r.numLods ? r.lods[0].evo : nullptr;
    const uint8_t* evoB = r.tLods ? Read<const uint8_t*>(r.tcc[0], kCcEvolution) : nullptr;
    for (int i = 0; i < r.numLods && i < r.tLods; ++i) {
        const uint8_t* a = r.lods[i].cc;
        const uint8_t* b = r.tcc[i];
        for (size_t o = kCcOffset; o < kCcSize; o += 4)
            if (Read<int32_t>(a, o) != Read<int32_t>(b, o)) d.Add("LOD %d constraints +0x%zX: %d vs %d", i, o, Read<int32_t>(a, o), Read<int32_t>(b, o));
        for (size_t s = 0; s < kCcSlots; ++s) {
            const auto* oa = Read<const uint8_t*>(a, s * 16);
            const auto* ob = Read<const uint8_t*>(b, s * 16);
            if (!oa != !ob) { d.Add("LOD %d slot +0x%zX: %s vs %s", i, s * 16, oa ? "set" : "empty", ob ? "set" : "empty"); continue; }
            if (!oa) continue;
            g_slotsSeen |= 1u << s;
            const CmpCtx ctx{a, b, reinterpret_cast<const uint8_t*>(r.lods[i].args[4]), r.tTethers[i], i, static_cast<int>(s)};
            CompareConstraintObjects(d, ctx, oa, ob);
        }
    }
    if (evoA && evoB) {
        const int32_t n = ArrayNum(evoA, kEvoX);
        if (n != ArrayNum(evoB, kEvoX)) d.Add("particle count %d vs %d", n, ArrayNum(evoB, kEvoX));
        CmpArray(d, evoA, evoB, kEvoX, 12, "particles X", -1);
        CmpArray(d, evoA, evoB, kEvoV, 12, "particles V", -1);
        CmpArray(d, evoA, evoB, kEvoM, 4, "particles M", -1);
        CmpArray(d, evoA, evoB, kEvoInvM, 4, "particles InvM", -1);
        // P/InvM: P (set by every ResetStartPose) for all particles; its InvM copy only for the active ranges (written by
        // ApplyPreSimulationTransforms; the inactive LODs' copies stay as allocated, uninitialised, in the game too).
        if (n == ArrayNum(evoB, kEvoX) && n > 0 && ArrayNum(evoA, kEvoPInvM) == n && ArrayNum(evoB, kEvoPInvM) == n) {
            const auto* pa = Read<const uint8_t*>(evoA, kEvoPInvM);
            const auto* pb = Read<const uint8_t*>(evoB, kEvoPInvM);
            for (int32_t i = 0; i < n; ++i)
                if (memcmp(pa + i * 16, pb + i * 16, RangeActive(const_cast<uint8_t*>(evoA), kEvoParticleRanges, i) ? 16 : 12) != 0) {
                    d.Add("particles P/InvM: element %d of %d differs", i, n);
                    break;
                }
        } else {
            d.Add("particles P/InvM: counts differ");
        }
        CmpArray(d, evoA, evoB, kEvoParticleRanges, 4, "particle ranges (active flags)", -1);
        CmpArray(d, evoA, evoB, kEvoInitRanges, 4, "constraint init ranges (active flags)", -1);
        CmpArray(d, evoA, evoB, kEvoRuleRanges, 4, "constraint rule ranges (active flags)", -1);
        CmpArray(d, evoA, evoB, kEvoPostRanges, 4, "post-collision rule ranges (active flags)", -1);
    }
    s_verified.fetch_add(1, std::memory_order_relaxed);
    if (d.n == 0) {
        if (g_verifyLogs < 12) {
            ++g_verifyLogs;
            Log("[clothreg-verify] %s: completed lite proxy == full build (%d LODs, %d updates replayed, slots seen 0x%X)", where,
                r.numLods, r.numUpd, g_slotsSeen);
        }
        return;
    }
    s_mismatches.fetch_add(1, std::memory_order_relaxed);
    Log("[clothreg-verify] %s: MISMATCH, %d differences (%d LODs, %d updates); first: %s%s%s%s%s", where, d.n, r.numLods, r.numUpd,
        d.first[0], d.n > 1 ? " | " : "", d.n > 1 ? d.first[1] : "", d.n > 2 ? " | " : "", d.n > 2 ? d.first[2] : "");
    if (g_verifyLogs < 40) {
        ++g_verifyLogs;
        for (int i = 3; i < d.n && i < 10; ++i) Log("[clothreg-verify]   %s", d.first[i]);
    }
    // At the call's end the game keeps this proxy: the twin (a full build of the same moment) is put in instead.
    if (strcmp(where, "call end") == 0 && ProxyOf(r.comp) == r.proxy) {
        void* const oldCtrl = CtrlOf(r.comp);
        ProxyOf(r.comp) = r.tProxy;
        CtrlOf(r.comp) = r.tCtrl;
        r.tProxy = r.tCtrl = nullptr;
        ReleaseShared(oldCtrl);
        s_verifyReplaced.fetch_add(1, std::memory_order_relaxed);
    }
}

uint32_t g_slotVtLogged = 0;
// Per-type layouts of the constraint classes the game's robes use (UE 5.3 headers, MSVC x64; object sizes match MakeShared's
// allocations in AddRules' Create* functions: spring 0x90, area 0xA0, long range 0xC8, spherical 0x38, backstop 0x60, anim drive
// 0xC0). Fields: plain bytes compared as they are (padding left out), TArrays by count and contents, references into the own
// solver by their offset from the constraints' AnimationPositions reference (cc+0x1B8), the tethers reference against the LOD's
// AddRules argument. A slot of another type is reported as not covered (counts as a difference).
enum FieldKind : uint8_t { kPod, kArr, kRefSolver, kRefTethers, kStiff, kWmap, kEnd };
struct Field { uint16_t off; FieldKind kind; uint16_t len; };  // kPod: len bytes; kArr: element size; kStiff/kWmap: nested
constexpr Field kSpring[] = {{0x00, kPod, 8}, {0x08, kArr, 8}, {0x18, kArr, 4}, {0x28, kPod, 8}, {0x30, kStiff, 0}, {0x78, kArr, 4},
                             {0x88, kPod, 4}, {0, kEnd, 0}};
constexpr Field kArea[] = {{0x00, kPod, 8}, {0x08, kArr, 12}, {0x18, kPod, 8}, {0x20, kArr, 4}, {0x30, kArr, 4}, {0x40, kStiff, 0},
                           {0x88, kArr, 4}, {0x98, kPod, 4}, {0, kEnd, 0}};
constexpr Field kLongRange[] = {{0x00, kPod, 8}, {0x08, kRefTethers, 8}, {0x10, kPod, 8}, {0x18, kStiff, 0}, {0x60, kWmap, 0},
                                {0x98, kArr, 1}, {0xA8, kArr, 4}, {0xC0, kPod, 8}, {0, kEnd, 0}};  // +0xB8: the deprecated Scale,
                                                                                                     // never initialised (no reader)
constexpr Field kSpherical[] = {{0x00, kRefSolver, 8}, {0x08, kPod, 12}, {0x18, kPod, 0x1C}, {0, kEnd, 0}};
constexpr Field kBackstop[] = {{0x00, kRefSolver, 8}, {0x08, kRefSolver, 8}, {0x10, kPod, 12}, {0x20, kPod, 12}, {0x30, kPod, 0x1E},
                               {0x50, kPod, 0x0C}, {0, kEnd, 0}};
constexpr Field kAnimDrive[] = {{0x00, kRefSolver, 8}, {0x08, kRefSolver, 8}, {0x10, kRefSolver, 8}, {0x18, kPod, 9}, {0x28, kStiff, 0},
                                {0x70, kStiff, 0}, {0xB8, kPod, 8}, {0, kEnd, 0}};
constexpr Field kWeightMap[] = {{0x00, kPod, 8}, {0x08, kArr, 1}, {0x18, kArr, 4}, {0x28, kPod, 9}, {0, kEnd, 0}};
constexpr Field kStiffness[] = {{0x00, kPod, 8}, {0x08, kArr, 1}, {0x18, kArr, 4}, {0x28, kPod, 9}, {0x38, kPod, 0x10}, {0, kEnd, 0}};

void CmpFields(Diff& d, const CmpCtx& c, const uint8_t* a, const uint8_t* b, const Field* f, uint16_t base)
{
    for (; f->kind != kEnd; ++f) {
        const size_t o = f->off;
        switch (f->kind) {
        case kPod:
            if (memcmp(a + o, b + o, f->len) != 0) d.Add("LOD %d slot %d +0x%zX: %u bytes differ", c.lod, c.slot, base + o, f->len);
            break;
        case kArr: {
            char what[48];
            snprintf(what, sizeof what, "slot %d +0x%zX array", c.slot, base + o);
            CmpArray(d, a, b, o, f->len, what, c.lod);
            break;
        }
        case kRefSolver: {
            const intptr_t ra = Read<intptr_t>(a, o) - Read<intptr_t>(c.ccA, 0x1B8);
            const intptr_t rb = Read<intptr_t>(b, o) - Read<intptr_t>(c.ccB, 0x1B8);
            if (ra != rb) d.Add("LOD %d slot %d +0x%zX: solver reference at %lld vs %lld", c.lod, c.slot, base + o, static_cast<long long>(ra),
                                static_cast<long long>(rb));
            break;
        }
        case kRefTethers:
            if (Read<const uint8_t*>(a, o) != c.tethersA || Read<const uint8_t*>(b, o) != c.tethersB)
                d.Add("LOD %d slot %d +0x%zX: tethers reference not the LOD's", c.lod, c.slot, base + o);
            break;
        case kStiff: CmpFields(d, c, a + o, b + o, kStiffness, static_cast<uint16_t>(base + o)); break;
        case kWmap: CmpFields(d, c, a + o, b + o, kWeightMap, static_cast<uint16_t>(base + o)); break;
        default: break;
        }
    }
}

uintptr_t g_vtSpring = 0, g_vtArea = 0, g_vtLongRange = 0;  // exe RVAs seen (logged once)
void CompareConstraintObjects(Diff& d, const CmpCtx& c, const uint8_t* oa, const uint8_t* ob)
{
    const int slot = c.slot;
    if (!(g_slotVtLogged & (1u << slot))) {
        g_slotVtLogged |= 1u << slot;
        const auto* va = Read<const uint8_t*>(oa, 0);
        const uint8_t* base = Game().base;
        const bool inExe = va >= base && va < base + Game().sizeOfImage;
        Log("[clothreg-verify] constraint slot %d (+0x%X) in use: first qword %s0x%llX", slot, slot * 16, inExe ? "exe+" : "",
            static_cast<unsigned long long>(inExe ? static_cast<uintptr_t>(va - base) : reinterpret_cast<uintptr_t>(va)));
    }
    const Field* f = nullptr;
    switch (slot) {
    case 9: case 12: f = kSpring; break;     // Edge (FPBDEdgeSpringConstraints), BendingSpring (FPBDBendingSpringConstraints)
    case 17: f = kArea; break;               // Area (FPBDAreaSpringConstraints)
    case 19: f = kLongRange; break;          // LongRange (FPBDLongRangeConstraints)
    case 20: f = kSpherical; break;          // MaxDistance (FPBDSphericalConstraint)
    case 21: f = kBackstop; break;           // Backstop (FPBDSphericalBackstopConstraint)
    case 22: f = kAnimDrive; break;          // AnimDrive (FPBDAnimDriveConstraint)
    default: d.Add("LOD %d slot %d: constraint type not covered by the compare", c.lod, slot); return;
    }
    CmpFields(d, c, oa, ob, f, 0);
}

void CloseCalls(int from)
{
    for (int i = from; i < g_numPending; ++i) {
        Pending& p = g_pending[i];
        if (p.lite) {
            ClassEntry* c = p.cls >= 0 ? &g_classes[p.cls] : nullptr;
            if (p.lastLite && Registered(p.comp) && ProxyOf(p.comp) != nullptr && ProxyOf(p.comp) == p.lastProxy) {
                // The call's last build was lite: completed in place (identical), or, if that cannot be done, rebuilt in full here.
                const char* why = "no record";
                LiteRec* r = p.rec >= 0 ? &g_recs[p.rec] : nullptr;
                if (r && Complete(*r, &why)) {
                    if (g_verify.load(std::memory_order_relaxed)) Compare(*r, "call end");
                } else {
                    s_liteFallbacks.fetch_add(1, std::memory_order_relaxed);
                    s_completeFail.fetch_add(1, std::memory_order_relaxed);
                    Log("[clothreg] lite fallback (%s), class %d: this call's builds %s; rebuilt in full", why, p.cls, p.seq);
                    if (g_liteMode.load(std::memory_order_relaxed) != 3)
                        SetLiteVerdict(p.cls, Verdict::kKeep, "lite build left at the call's end could not be completed");
                    DropRec(p, false);
                    TimedRecreate(p.comp, p.cls);
                }
            } else if (p.builds >= 2 && c && c->lite == Verdict::kSkip) {
                s_liteReplaced.fetch_add(1, std::memory_order_relaxed);
            }
            DropRec(p, false);
            if (Registered(p.comp)) TrackAdd(p.comp, p.cls);
            if (c && c->lite == Verdict::kLearning) {
                // Two learning calls, both with >= 2 builds; the fewer of the two counts is used.
                if (c->liteObs < 2) memcpy(c->seqs[c->liteObs], p.seq, sizeof p.seq);
                if (p.builds >= 2 && Registered(p.comp)) {
                    c->callBuilds = c->liteObs == 0 ? p.builds : (p.builds < c->callBuilds ? p.builds : c->callBuilds);
                    if (++c->liteObs >= kLiteObservations) {
                        char why[160];
                        snprintf(why, sizeof why, "%d+ builds in each of %d EquipBodyPart calls (%s / %s): lite from now on",
                                 c->callBuilds, c->liteObs, c->seqs[0], c->seqs[1]);
                        SetLiteVerdict(p.cls, Verdict::kSkip, why);
                    }
                } else {
                    SetLiteVerdict(p.cls, Verdict::kKeep, "one build in an EquipBodyPart call: nothing to leave out");
                }
            }
            continue;
        }
        if (p.skipped && !p.rebuilt && !Registered(p.comp)) {
            // Unregistered inside the call: the game's OnUnregister would have dropped proxy (a) too. Nothing to do.
        } else if (p.skipped && !p.rebuilt) {
            // No rebuild in this call: build it now so a proxy exists as in the game (counted; the class is kept from now).
            s_fallbacks.fetch_add(1, std::memory_order_relaxed);
            SetVerdict(p.cls, Verdict::kKeep, "no Blueprint rebuild in the same EquipBodyPart call (fallback build)");
            if (ProxyOf(p.comp) == nullptr) TimedRecreate(p.comp, p.cls);
        } else if (p.learning && p.cls >= 0 && g_classes[p.cls].verdict == Verdict::kLearning) {
            if (p.rebuilt && !p.boundsRead) SetVerdict(p.cls, Verdict::kSkip, "rebuilt by its Blueprint in the same call, nothing read it");
            else SetVerdict(p.cls, Verdict::kKeep, p.rebuilt ? "bounds read between the builds" : "no rebuild in the same call");
        }
    }
    g_numPending = from;
}

void EquipHook(void* character, void* a2, void* a3, void* a4)
{
    const DWORD tid = GetCurrentThreadId();
    if (g_depth > 0 && tid != g_equipThread) {  // never expected (game thread only): leave it alone
        g_equip(character, a2, a3, a4);
        return;
    }
    g_equipThread = tid;
    const int mark = g_numPending;
    ++g_depth;
    g_equip(character, a2, a3, a4);
    --g_depth;
    CloseCalls(mark);
}

// ---- finding the sites ----

bool Fail(const char* what)
{
    Log("[clothreg] %s: fix disabled", what);
    return false;
}

std::vector<void**> SlotsHolding(const GameModule& game, const void* target)
{
    std::vector<void**> out;
    const uintptr_t want = reinterpret_cast<uintptr_t>(target);
    for (uint8_t* p = game.rdata.begin; p + 8 <= game.rdata.end; p += 8)
        if (*reinterpret_cast<const uintptr_t*>(p) == want) out.push_back(reinterpret_cast<void**>(p));
    return out;
}

// The function slot of a native table entry {const char* name, FNativeFuncPtr func} whose function matches the pattern.
void** NativeSlot(const GameModule& game, const char* name, const Pattern& start)
{
    const size_t n = strlen(name) + 1;
    void** found = nullptr;
    int hits = 0;
    for (uint8_t* p = game.rdata.begin; p + n <= game.rdata.end; ++p) {
        if (*p != static_cast<uint8_t>(name[0]) || memcmp(p, name, n) != 0 || (p > game.rdata.begin && p[-1] != 0)) continue;
        for (void** s : SlotsHolding(game, p)) {
            if (reinterpret_cast<uint8_t*>(s + 2) > game.rdata.end) continue;
            auto* fn = static_cast<uint8_t*>(s[1]);
            if (game.text.Contains(fn) && MatchesAt(fn, start) && found != s + 1) { found = s + 1; ++hits; }
        }
    }
    return hits == 1 ? found : nullptr;
}

const uint8_t* Within(const uint8_t* from, size_t len, const Pattern& p)
{
    for (size_t i = 0; i + p.size() <= len; ++i)
        if (MatchesAt(from + i, p)) return from + i;
    return nullptr;
}

bool Find()
{
    const GameModule& game = Game();
    auto rva = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
    Sites s;

    s.execRecreateSlot = NativeSlot(game, "RecreateClothSimulationProxy", ParsePattern(kExecRecreate));
    s.execSuspendedSlot = NativeSlot(game, "IsSimulationSuspended", ParsePattern(kExecSuspended));
    s.execEnabledSlot = NativeSlot(game, "IsSimulationEnabled", ParsePattern(kExecEnabled));
    if (!s.execRecreateSlot || !s.execSuspendedSlot || !s.execEnabledSlot)
        return Fail("cloth component's native functions not found");
    s.recreate = RipTarget(static_cast<uint8_t*>(*s.execRecreateSlot) + kExecRecreateJmp, 1, 5);
    if (!game.text.Contains(s.recreate) || !MatchesAt(s.recreate, ParsePattern(kRecreate)))
        return Fail("RecreateClothSimulationProxy not as expected");
    s.registeredOff = Read<uint32_t>(s.recreate, kRecreateRegisteredDisp);
    uint8_t* const suspended = RipTarget(static_cast<uint8_t*>(*s.execSuspendedSlot) + kExecSuspendedCall, 1, 5);
    const uint8_t* const body = game.text.Contains(suspended) ? Within(suspended, kSuspendedSearch, ParsePattern(kSuspendedBody)) : nullptr;
    if (!body) return Fail("IsSimulationSuspended not as expected");
    s.proxyOff = Read<uint32_t>(body, kSuspendedProxyDisp);
    if (s.registeredOff == 0 || s.registeredOff > 0x200 || s.proxyOff < 0x400 || s.proxyOff > 0x2000) return Fail("unexpected offsets");
    {   // IsSimulationEnabled's thunk tests the same pointer
        const uint8_t testProxy[8] = {0x48, 0x83, 0xBB, static_cast<uint8_t>(s.proxyOff), static_cast<uint8_t>(s.proxyOff >> 8), 0, 0, 0};
        const auto* fn = static_cast<const uint8_t*>(*s.execEnabledSlot);
        bool ok = false;
        for (size_t i = 0; i + 8 <= 0x80 && !ok; ++i) ok = memcmp(fn + i, testProxy, 8) == 0;
        if (!ok) return Fail("IsSimulationEnabled's thunk not as expected");
    }

    // OnRegister and SetSkinnedAssetAndUpdate: their call to RecreateClothSimulationProxy.
    uint8_t* onRegister = nullptr;
    for (uint8_t* m : FindAll(game.text, ParsePattern(kOnRegister)))
        if (RipTarget(m + kOnRegisterRecreateCall, 1, 5) == s.recreate) {
            if (onRegister) return Fail("OnRegister ambiguous");
            onRegister = m;
        }
    if (!onRegister) return Fail("OnRegister not found");
    s.onRegisterCall = onRegister + kOnRegisterRecreateCall;
    for (uint8_t* m : FindAll(game.text, ParsePattern(kSetAsset)))
        if (RipTarget(m + kSetAssetRecreateCall, 1, 5) == s.recreate &&
            RipTarget(m + kSetAssetUpdateCall, 1, 5) == RipTarget(onRegister + kOnRegisterUpdateCall, 1, 5) &&
            RipTarget(m + kSetAssetResetCall, 1, 5) == RipTarget(onRegister + kOnRegisterResetCall, 1, 5)) {
            if (s.setAssetCall) return Fail("SetSkinnedAssetAndUpdate ambiguous");
            s.setAssetCall = m + kSetAssetRecreateCall;
        }
    if (!s.setAssetCall) return Fail("SetSkinnedAssetAndUpdate's rebuild not found");

    // The component's vtable: the only slot holding OnRegister; then RequiresPreEndOfFrameSync and CalcBounds.
    const std::vector<void**> regSlots = SlotsHolding(game, onRegister);
    if (regSlots.size() != 1) return Fail("cloth component vtable not unique");
    s.vtable = regSlots[0];
    s.requiresSyncSlot = s.vtable + kSlotRequiresSync;
    s.calcBoundsSlot = s.vtable + kSlotCalcBounds;
    const auto* reqSync = static_cast<const uint8_t*>(*s.requiresSyncSlot);
    if (!game.text.Contains(reqSync) || !MatchesAt(reqSync, ParsePattern(kRequiresSync)) ||
        RipTarget(reqSync + kRequiresSyncSuspendedCall, 1, 5) != suspended)
        return Fail("RequiresPreEndOfFrameSync slot not as expected");
    const auto* bounds = static_cast<const uint8_t*>(*s.calcBoundsSlot);
    const uint8_t readProxy[7] = {0x48, 0x8B, 0x8B, static_cast<uint8_t>(s.proxyOff), static_cast<uint8_t>(s.proxyOff >> 8), 0, 0};
    bool boundsOk = game.text.Contains(bounds) && MatchesAt(bounds, ParsePattern(kCalcBoundsStart));
    bool reads = false;
    for (size_t i = 0; boundsOk && i + 7 <= kCalcBoundsSearch && !reads; ++i) reads = memcmp(bounds + i, readProxy, 7) == 0;
    if (!boundsOk || !reads) return Fail("CalcBounds slot not as expected");
    {
        auto* unreg = static_cast<uint8_t*>(s.vtable[1]);
        if (game.text.Contains(unreg) && MatchesAt(unreg, ParsePattern(kOnUnregister)) &&
            Read<uint32_t>(unreg, kOnUnregisterProxyDisp) == s.proxyOff)
            s.onUnregisterSlot = s.vtable + 1;
        else
            Log("[clothreg] OnUnregister slot not as expected: ClothRegisterLite unavailable");
    }
    // The vtable pointer an instance holds: the slot table's start. Walk back over code pointers to the first slot.
    {
        void** v = s.vtable;
        while (v > reinterpret_cast<void**>(game.rdata.begin) && game.text.Contains(v[-1])) --v;
        s.clothVtable = v;
        if (s.vtable - v != (0x348 / 8)) return Fail("cloth component vtable start not as expected");
    }

    // EquipBodyPart: its vtable slots and the direct call of a subclass's override.
    const std::vector<uint8_t*> equips = FindAll(game.text, ParsePattern(kEquip));
    if (equips.size() != 1) return Fail("EquipBodyPart not found");
    s.equip = equips[0];
    for (void** slot : SlotsHolding(game, s.equip)) {
        if (s.numEquipSlots == 4) return Fail("EquipBodyPart: too many vtable slots");
        s.equipSlots[s.numEquipSlots++] = slot;
    }
    if (s.numEquipSlots == 0) return Fail("EquipBodyPart's vtable slots not found");
    for (uint8_t* m : FindAll(game.text, ParsePattern(kEquipCaller)))
        if (RipTarget(m + kEquipCallerCall, 1, 5) == s.equip) s.equipCall = m + kEquipCallerCall;

    // ClothRegisterLite: the one AddRules call in FLODData::Add (optional: without it only the lite mode is unavailable).
    {
        const std::vector<uint8_t*> calls = FindAll(game.text, ParsePattern(kAddRulesCall));
        uint8_t* const call = calls.size() == 1 ? calls[0] + kAddRulesCallAt : nullptr;
        uint8_t* const rules = call ? RipTarget(call, 1, 5) : nullptr;
        if (call && game.text.Contains(rules) && MatchesAt(rules, ParsePattern(kAddRules))) s.addRulesCall = call;
        else Log("[clothreg] the cloth constraint rules' call not found (%zu matches): ClothRegisterLite unavailable", calls.size());
        // Identical completion: Enable (AddRules' tail jump), the only Update call, and the particle layout. Without them the lite
        // mode is unavailable (a lite build could not be completed).
        if (!s.onUnregisterSlot) s.addRulesCall = nullptr;
        if (s.addRulesCall) {
            const uint8_t* tail = Within(rules, kAddRulesTailSearch, ParsePattern(kAddRulesTail));
            uint8_t* const enable = tail ? RipTarget(tail + 6, 1, 5) : nullptr;
            const std::vector<uint8_t*> ucalls = FindAll(game.text, ParsePattern(kCcUpdateCall));
            uint8_t* const ucall = ucalls.size() == 1 ? ucalls[0] + kCcUpdateCallAt : nullptr;
            uint8_t* const update = ucall ? RipTarget(ucall, 1, 5) : nullptr;
            bool layout = FindAll(game.text, ParsePattern(kResetStartPose)).size() == 1 &&
                          Within(call + 5, kInvMReadSearch, ParsePattern(kInvMRead)) != nullptr;
            {   // the particle range, rule counts and active-range offsets, from the code that uses them (finding 2)
                const uint8_t* factory = nullptr;
                const Pattern fc = ParsePattern(kAddRulesFactoryCall);
                const uint8_t* at = rules;
                for (int n = 0; n < 3 && at; ++n) {
                    const size_t left = kAddRulesFactorySearch - static_cast<size_t>(at - rules);
                    at = Within(at, left, fc);
                    if (at && n == 2) factory = RipTarget(const_cast<uint8_t*>(at) + 3, 1, 5);
                    if (at) at += fc.size();
                }
                const bool fOk = factory && game.text.Contains(factory) && game.text.Contains(factory + kFactorySearch);
                const uint8_t* rd = fOk ? Within(factory, kFactorySearch, ParsePattern(kFactoryReadsRange)) : nullptr;
                const uint8_t* inc = fOk ? Within(factory, kFactorySearch, ParsePattern(kFactoryCounts)) : nullptr;
                const bool range = rd && Read<uint32_t>(rd, 3) == kCcNum && Read<uint32_t>(rd, 10) == kCcOffset;
                const bool counts = inc && Read<uint32_t>(inc, 2) == kCcNumInits && Read<uint32_t>(inc, 8) == kCcNumInits + 4;
                const std::vector<uint8_t*> eps = FindAll(game.text, ParsePattern(kEnableParticles));
                const uint8_t* ept = eps.size() == 1 ? Within(eps[0], kEnableParticlesSearch, ParsePattern(kEnableParticlesTail)) : nullptr;
                const bool ranges = ept && Read<uint32_t>(eps[0], kEnableParticlesData) == kEvoParticleRanges &&
                                    Read<uint32_t>(eps[0], kEnableParticlesNum) == kEvoParticleRanges + 8 &&
                                    RipTarget(const_cast<uint8_t*>(ept) + 6, 1, 5) == enable;
                if (!range || !counts || !ranges) {
                    Log("[clothreg] constraint layout not as expected (particle range %d, rule counts %d, active ranges %d)", range, counts,
                        ranges);
                    layout = false;
                }
            }
            if (enable && game.text.Contains(enable) && MatchesAt(enable, ParsePattern(kCcEnable)) && update && game.text.Contains(update) &&
                MatchesAt(update, ParsePattern(kCcUpdate)) && layout) {
                s.ccEnable = enable;
                s.ccUpdateCall = ucall;
            } else {
                Log("[clothreg] the constraint Enable/Update sites or the particle layout not as expected (enable %d, update %zu, "
                    "layout %d): ClothRegisterLite unavailable", enable != nullptr, ucalls.size(), layout);
                s.addRulesCall = nullptr;
            }
        }
    }

    g = s;
    Log("[clothreg] Recreate exe+0x%X (registered flag +0x%X), proxy +0x%X; OnRegister call exe+0x%X, SetSkinnedAsset call "
        "exe+0x%X; exec slots exe+0x%X/0x%X/0x%X; vtable exe+0x%X (OnRegister slot exe+0x%X, RequiresPreEndOfFrameSync "
        "exe+0x%X, CalcBounds exe+0x%X); EquipBodyPart exe+0x%X (%d vtable slots, direct call %s0x%X)",
        rva(g.recreate), g.registeredOff, g.proxyOff, rva(g.onRegisterCall), rva(g.setAssetCall), rva(g.execRecreateSlot),
        rva(g.execSuspendedSlot), rva(g.execEnabledSlot), rva(g.clothVtable), rva(g.vtable), rva(*g.requiresSyncSlot),
        rva(*g.calcBoundsSlot), rva(g.equip), g.numEquipSlots, g.equipCall ? "exe+" : "none ", g.equipCall ? rva(g.equipCall) : 0);
    if (g.addRulesCall)
        Log("[clothreg] lite build: AddRules call exe+0x%X -> exe+0x%X; completion: Enable exe+0x%X, Update call exe+0x%X -> exe+0x%X",
            rva(g.addRulesCall), rva(RipTarget(g.addRulesCall, 1, 5)), rva(g.ccEnable), rva(g.ccUpdateCall),
            rva(RipTarget(g.ccUpdateCall, 1, 5)));
    return true;
}

// A rel32 call redirected to hook through a "jmp [rip+0]" stub near the game's code (start-up only).
bool RedirectCall(uint8_t* call, void* hook)
{
    uint8_t* const stub = AllocNearGame(16);
    if (!stub) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(stub, jmpAbs, sizeof jmpAbs);
    memcpy(stub + sizeof jmpAbs, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const int64_t rel = stub - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}

}  // namespace

bool FindClothRegisterSkip() { return g.recreate != nullptr || Find(); }

bool InstallClothRegisterSkip(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!FindClothRegisterSkip()) return false;
    if (!beforeGameCode) return Fail("only installed while the game starts (version.dll): the class's native functions are bound then");
    g_recreate = reinterpret_cast<RecreateFn>(g.recreate);
    g_execRecreate = reinterpret_cast<ExecFn>(*g.execRecreateSlot);
    g_execSuspended = reinterpret_cast<ExecFn>(*g.execSuspendedSlot);
    g_execEnabled = reinterpret_cast<ExecFn>(*g.execEnabledSlot);
    g_requiresSync = reinterpret_cast<BoolFn>(*g.requiresSyncSlot);
    g_calcBounds = reinterpret_cast<CalcBoundsFn>(*g.calcBoundsSlot);
    g_calcBoundsGame = *g.calcBoundsSlot;
    g_equip = reinterpret_cast<EquipFn>(g.equip);
    bool ok = RedirectCall(g.onRegisterCall, reinterpret_cast<void*>(&OnRegisterRecreateHook));
    ok = ok && RedirectCall(g.setAssetCall, reinterpret_cast<void*>(&SetAssetRecreateHook));
    if (!ok) return Fail("could not redirect the calls");
    ok &= WritePointer(g.execRecreateSlot, reinterpret_cast<void*>(&ExecRecreateHook));
    ok &= WritePointer(g.execSuspendedSlot, reinterpret_cast<void*>(&ExecSuspendedHook));
    ok &= WritePointer(g.execEnabledSlot, reinterpret_cast<void*>(&ExecEnabledHook));
    ok &= WritePointer(g.requiresSyncSlot, reinterpret_cast<void*>(&RequiresSyncHook));
    ok &= WritePointer(g.calcBoundsSlot, reinterpret_cast<void*>(&CalcBoundsHook));
    if (g.equipCall) ok &= RedirectCall(g.equipCall, reinterpret_cast<void*>(&EquipHook));
    if (g.addRulesCall) {
        g_addRules = reinterpret_cast<AddRulesFn>(RipTarget(g.addRulesCall, 1, 5));
        g_ccEnable = reinterpret_cast<CcEnableFn>(g.ccEnable);
        g_onUnregister = reinterpret_cast<RecreateFn>(*g.onUnregisterSlot);
        if (!WritePointer(g.onUnregisterSlot, reinterpret_cast<void*>(&OnUnregisterHook))) g.addRulesCall = nullptr;
        g_ccUpdate = reinterpret_cast<CcUpdateFn>(RipTarget(g.ccUpdateCall, 1, 5));
        if (!RedirectCall(g.ccUpdateCall, reinterpret_cast<void*>(&CcUpdateHook)) ||
            !RedirectCall(g.addRulesCall, reinterpret_cast<void*>(&AddRulesHook))) {
            Log("[clothreg] could not redirect the constraint rules' / Update call: ClothRegisterLite unavailable");
            g.addRulesCall = nullptr;
        }
    }
    for (int i = 0; i < g.numEquipSlots; ++i) ok &= WritePointer(g.equipSlots[i], reinterpret_cast<void*>(&EquipHook));
    if (!ok) Log("[clothreg] warning: a hook write failed");
    g_installed = true;
    Log("[clothreg] installed");
    return true;
}


void SetClothRegisterLite(int mode)
{
    const bool enabled = mode != 0;
    if (!g_installed) return;
    const int m = mode == 2 ? 2 : mode == 3 ? 3 : 1;
    if (enabled && g_liteMode.exchange(m) != m && g_lite.load()) Log("[clothreg] lite mode %d", m);
    if (enabled && !g.addRulesCall) {
        static bool logged = false;
        if (!logged) Log("[clothreg] ClothRegisterLite unavailable (the constraint rules' call was not found)");
        logged = true;
        return;
    }
    if (g_lite.exchange(enabled) == enabled) return;
    Log("[clothreg] %s", !enabled ? "robes put on: lite builds off"
                         : m == 2 ? "robes put on: the cloth build at registration made without its constraint rules (lite) "
                                    "when the robe's Blueprint rebuilds it in the same call (mode 2)"
                         : m == 3 ? "robes put on: every cloth build of an EquipBodyPart call made without its constraint rules (lite); "
                                    "the one left at the call's end gets them added in place, as the game made them (mode 3)"
                                  : "robes put on: every cloth build that a later build of the same EquipBodyPart call "
                                    "replaces is made without its constraint rules (lite, mode 1)");
}


bool ClothRegisterLiteAvailable() { return g.addRulesCall != nullptr; }

// For ClothBuildDefer (npcs/cloth_defer.cpp): the component's OnUnregister vtable slot while it holds this file's hook
// (ClothBuildDefer's hook then goes in front of it), else nullptr.
void** ClothRegisterUnregisterSlot()
{
    return g.onUnregisterSlot && *g.onUnregisterSlot == reinterpret_cast<void*>(&OnUnregisterHook) ? g.onUnregisterSlot : nullptr;
}

void LogClothRegisterStats()
{
    {
        static uint64_t lastL[9] = {};
        const uint64_t nowL[9] = {s_liteRegisters.load(), s_liteBuilds.load(), s_liteLearning.load(), s_liteKept.load(),
                                  s_liteReplaced.load(), s_liteFallbacks.load(), s_rulesLeftOut.load(), s_liteUs.load(),
                                  s_liteSavedUs.load()};
        uint64_t d[9];
        bool any = false;
        for (int i = 0; i < 9; ++i) { d[i] = nowL[i] - lastL[i]; lastL[i] = nowL[i]; any |= d[i] != 0; }
        static uint64_t lastC[8] = {};
        const uint64_t nowC[8] = {s_completed.load(), s_completeFail.load(), s_completeUs.load(), s_twins.load(), s_verified.load(),
                                  s_mismatches.load(), s_verifyReplaced.load(), s_ticksLite.load()};
        uint64_t dc[8];
        bool anyC = false;
        for (int i = 0; i < 8; ++i) { dc[i] = nowC[i] - lastC[i]; lastC[i] = nowC[i]; anyC |= dc[i] != 0; }
        if (anyC)
            Log("[clothreg] completion since last report: %llu lite proxies completed in place (%.2f ms each), %llu could not be "
                "(rebuilt in full), %llu ticked before completion; verify: %llu twins, %llu compared, %llu mismatches (%llu replaced "
                "by the twin)",
                dc[0], dc[0] ? dc[2] / 1000.0 / dc[0] : 0.0, dc[1], dc[7], dc[3], dc[4], dc[5], dc[6]);
        if (any) {
            int lite = 0, full = 0, learn = 0;
            uint64_t fullUs = 0, fullN = 0;
            for (int i = 0; i < g_numClasses; ++i) {
                (g_classes[i].lite == Verdict::kSkip ? lite : g_classes[i].lite == Verdict::kKeep ? full : learn)++;
                fullUs += g_classes[i].buildUs;
                fullN += g_classes[i].builds;
            }
            Log("[clothreg] lite since last report: %llu cloth registrations while dressing: %llu lite builds (%.2f ms each, %llu "
                "rule sets left out, %llu replaced by the Blueprint, %llu lite fallbacks, ~%.1f ms saved), %llu learning, %llu full; "
                "full builds so far %.2f ms each; robe classes: %d lite, %d full, %d learning",
                d[0], d[1], d[1] ? d[7] / 1000.0 / d[1] : 0.0, d[6], d[4], d[5], d[8] / 1000.0, d[2], d[3],
                fullN ? fullUs / 1000.0 / fullN : 0.0, lite, full, learn);
        }
    }
    static uint64_t last[11] = {};
    const uint64_t now[11] = {s_registers.load(), s_skipped.load(), s_learning.load(), s_kept.load(), s_rebuiltAfterSkip.load(),
                              s_lateBuilds.load(), s_fallbacks.load(), s_readsSync.load(), s_readsSuspended.load(),
                              s_readsEnabled.load(), s_savedUs.load()};
    uint64_t d[11];
    bool any = false;
    for (int i = 0; i < 11; ++i) { d[i] = now[i] - last[i]; last[i] = now[i]; any |= d[i] != 0; }
    if (!any) return;
    int skip = 0, keep = 0, learn = 0;
    for (int i = 0; i < g_numClasses; ++i)
        (g_classes[i].verdict == Verdict::kSkip ? skip : g_classes[i].verdict == Verdict::kKeep ? keep : learn)++;
    Log("[clothreg] since last report: %llu cloth registrations while dressing: %llu builds left out (%llu rebuilt by the "
        "Blueprint, ~%.1f ms saved), %llu learning (built), %llu kept (built); %llu late builds (bounds read), %llu fallback "
        "builds; placeholder answers: %llu end-of-frame checks, %llu IsSimulationSuspended, %llu IsSimulationEnabled; robe "
        "classes: %d skip, %d keep, %d learning",
        d[0], d[1], d[4], d[10] / 1000.0, d[2], d[3], d[5], d[6], d[7], d[8], d[9], skip, keep, learn);
}

bool ClothRegisterInfo(ClothRegisterInfoOut* out)
{
    if (!g.recreate) return false;
    out->recreate = g.recreate;
    out->onRegisterCall = g.onRegisterCall;
    out->setAssetCall = g.setAssetCall;
    out->execRecreateSlot = g.execRecreateSlot;
    out->execSuspendedSlot = g.execSuspendedSlot;
    out->execEnabledSlot = g.execEnabledSlot;
    out->clothVtable = g.clothVtable;
    out->requiresSyncSlot = g.requiresSyncSlot;
    out->calcBoundsSlot = g.calcBoundsSlot;
    out->equip = g.equip;
    out->numEquipSlots = g.numEquipSlots;
    for (int i = 0; i < 4; ++i) out->equipSlots[i] = g.equipSlots[i];
    out->equipCall = g.equipCall;
    out->proxyOff = g.proxyOff;
    out->registeredOff = g.registeredOff;
    out->onRegisterHook = reinterpret_cast<void*>(&OnRegisterRecreateHook);
    out->setAssetHook = reinterpret_cast<void*>(&SetAssetRecreateHook);
    out->execRecreateHook = reinterpret_cast<void*>(&ExecRecreateHook);
    out->execSuspendedHook = reinterpret_cast<void*>(&ExecSuspendedHook);
    out->execEnabledHook = reinterpret_cast<void*>(&ExecEnabledHook);
    out->requiresSyncHook = reinterpret_cast<void*>(&RequiresSyncHook);
    out->calcBoundsHook = reinterpret_cast<void*>(&CalcBoundsHook);
    out->calcBoundsOrig = g_calcBoundsGame;
    out->equipHook = reinterpret_cast<void*>(&EquipHook);
    out->addRulesCall = g.addRulesCall;
    out->addRulesHook = reinterpret_cast<void*>(&AddRulesHook);
    out->ccUpdateCall = g.ccUpdateCall;
    out->ccEnable = g.ccEnable;
    out->ccUpdateHook = reinterpret_cast<void*>(&CcUpdateHook);
    out->onUnregisterSlot = g.onUnregisterSlot;
    out->onUnregisterHook = reinterpret_cast<void*>(&OnUnregisterHook);
    return true;
}

ClothRegisterStats GetClothRegisterStats()
{
    ClothRegisterStats s;
    s.registers = s_registers.load();
    s.skipped = s_skipped.load();
    s.learning = s_learning.load();
    s.kept = s_kept.load();
    s.rebuiltAfterSkip = s_rebuiltAfterSkip.load();
    s.lateBuilds = s_lateBuilds.load();
    s.fallbacks = s_fallbacks.load();
    s.readsSync = s_readsSync.load();
    s.readsSuspended = s_readsSuspended.load();
    s.readsEnabled = s_readsEnabled.load();
    s.liteBuilds = s_liteBuilds.load();
    s.liteLearning = s_liteLearning.load();
    s.liteReplaced = s_liteReplaced.load();
    s.liteFallbacks = s_liteFallbacks.load();
    s.rulesLeftOut = s_rulesLeftOut.load();
    s.completed = s_completed.load();
    s.completeFail = s_completeFail.load();
    s.twins = s_twins.load();
    s.verified = s_verified.load();
    s.mismatches = s_mismatches.load();
    return s;
}

void ClothRegisterTestSetup(void* recreate, void* execRecreate, void* execSuspended, void* execEnabled, void* requiresSync,
                            void* calcBounds, void* equip, void* clothVtable, uint32_t proxyOff, uint32_t registeredOff)
{
    g_recreate = reinterpret_cast<RecreateFn>(recreate);
    g_execRecreate = reinterpret_cast<ExecFn>(execRecreate);
    g_execSuspended = reinterpret_cast<ExecFn>(execSuspended);
    g_execEnabled = reinterpret_cast<ExecFn>(execEnabled);
    g_requiresSync = reinterpret_cast<BoolFn>(requiresSync);
    g_calcBounds = reinterpret_cast<CalcBoundsFn>(calcBounds);
    g_equip = reinterpret_cast<EquipFn>(equip);
    g_lite = false;
    g.clothVtable = clothVtable;
    g.proxyOff = proxyOff;
    g.registeredOff = registeredOff;
    g_installed = true;
    g_numClasses = 0;
    g_numPending = 0;
    g_depth = 0;
}

void ClothRegisterLiteTestSetup(void* addRules, void* ccEnable, void* ccUpdate)
{
    g_addRules = reinterpret_cast<AddRulesFn>(addRules);
    g.addRulesCall = static_cast<uint8_t*>(addRules);  // only tested for non-null
    g_ccEnable = reinterpret_cast<CcEnableFn>(ccEnable);
    g_ccUpdate = reinterpret_cast<CcUpdateFn>(ccUpdate);
    g.ccEnable = static_cast<uint8_t*>(ccEnable);
    g.ccUpdateCall = static_cast<uint8_t*>(ccUpdate);
    for (LiteRec& r : g_recs) r.used = false;
}

}  // namespace opf
