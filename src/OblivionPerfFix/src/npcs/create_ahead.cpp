// CreateAhead (lab prototype create106 2026-11-16, equivalence checks ahead2, public port aheadport 2026-11-16; [Fixes]
// CreateAhead, default 0 until a release; CreateAheadSlackMs, default 4). Start-up install when CreateAhead != 0
// (version.dll route; needs NpcCreationPacing, CreatePreload and a frame-rate cap), then live 0/1.
//
// What it fixes (findings "create106", "ahead2", S-005): the game creates a character in one piece on the game thread,
// 6-7 ms for a person: UWorld::SpawnActor (the actor, its components, their registration, the body's first animation
// set-up, physics state: about 3.8 ms) and then AActor::FinishSpawning with everything after it (construction script,
// BeginPlay, appearance, pairing with the old engine's reference: about 2.5 ms). A crowd arrives as a train of such creates,
// one per frame, in frames that are already busy: 25-35 ms frames.
//
// The fix: the SpawnActor half is done seconds earlier, in frames that have time to spare, and the game's own create then
// takes the actor that is already there.
//  - Who will be created. (a) RowPreload loads the next row of cells ahead; a cell's load brings its references. The
//    old-engine thread walks the loaded cell's reference list as the game's attach walks it and notes every character or
//    creature reference that has no actor yet (reference id, base form id, position, and the scale the reference's own
//    virtual gives, the one Spawn_Paired_Actor puts into the create command). (b) Run-time references (levelled spawns, ids
//    FF......) are in no cell ahead: for a base form the game has created in the last 90 s, up to 3 spare actors are kept
//    (built only while fewer than 8 actors are held, so references keep room). CreateAheadPool (aheadwide 2026-11-18,
//    0 = as just said; 12-32 = N): at most N actors held, spares built while fewer than N - 4 are held, and when the pool
//    is full and a base form created in the last 20 s has fewer than 3 spares (and its spare could be built now: class
//    resident, a free entry, the player's position known), one spare of another base form that has not been created for
//    20 s (and longer ago than the one that wants room) is destroyed in a slack frame (that frame's one piece of work),
//    so the pool follows the forms of the road instead of keeping the forms of the last town or fast travel for 90 s.
//    CreateAheadLevelled (1, live): (a) also takes the run-time references in a cell's list (levelled spawns the game made
//    at an earlier attach of the cell, its pick stored; the attach queues them from the same list and re-creates their
//    characters when the cell is switched on again); they are references with their own id, base form, place and scale.
//    Adoption of such an entry also needs the create's base form to be the noted one; one not built yet is let go at its
//    create and the spares apply as for any run-time reference. 0: they are skipped as before.
//  - Building (game thread, NpcCreationPacing's frame-end hook, after the frame cap's wait): at most ONE SpawnActor per
//    frame, only after a frame that slept at least CreateAheadSlackMs in the cap, only while NpcCreationPacing holds no unit
//    and deferred nothing in that frame, only when the base form's actor class is resident (asked through CreatePreload's
//    own prediction). The call is the game's UWorld::SpawnActor with the spawner's own parameters (copied from a stock
//    character create of this session; only a parameter set with no template, owner, instigator, level or parent component
//    is taken), at the reference's place 500 m lower (a spare: under the player), identity rotation, the noted scale. The
//    actor stays in UE's deferred-spawn state: FinishSpawning is not called, so no BeginPlay, no tick, no controller.
//    UWorld::SpawnActor's two broadcasts (OnActorPreSpawnInitialization, OnActorSpawned; their two call sites inside
//    SpawnActor are redirected to a hook that passes everything on except on the thread that is inside a build) are held
//    back: nobody is told of an actor built ahead. (Found the hard way, chain c1 of 2026-11-16: a world subsystem of the
//    game, a listener of OnActorSpawned, keeps every spawned character in a table that only the character's end of play
//    clears; a held actor destroyed unused never began play, stayed in the table, and the table's next pass after a
//    garbage collection read through the nulled entry: access violation at exe+0x450E30F.)
//  - Adoption (the spawner's SpawnActor call, its target redirected; no entry detour): inside the game's create command
//    (NpcCreationPacing's create hook gives the reference and base form ids), when an actor is held for this reference (or a
//    spare for this base form of a run-time reference), the requested class is the held actor's class, the parameters equal
//    the copied ones field by field, the three requested scale components equal the build scale bit for bit and that scale
//    times its reciprocal is exactly 1 (then UE's re-place below is exact), the held actor is returned instead of spawning,
//    after the two held-back broadcasts are made for it (pre-spawn first): the listeners hear of the character at the
//    point of the game's create where the stock SpawnActor tells them, in the stock order of creates, and with the actor at
//    the place the stock SpawnActor would have put it by then (aheadindex 2026-11-19, our bug before any release: they were
//    told while it stood at its park place). The game's pawn index (the SteeringBehaviors plugin's world subsystem, an
//    OnActorSpawned listener; what other characters' avoidance asks for the pawns near them) files the pawn by its root's
//    ComponentToWorld translation at the notice and re-files it only when it moves (or rebuilds itself every 300 s): an
//    adopted character that stood still was filed 500 m below its place, invisible to the avoidance of characters walking
//    past. So for the two broadcasts the root's translation is the requested one (stock: the root's own offset, none here,
//    times the requested transform = exactly the requested translation), and the park translation is put back right after
//    (FinishSpawning's re-place below reads it). The other three listeners read no place at the notice. An actor whose root
//    is not exactly at its build translation (a root with an offset of its own) is not adopted.
//    The spawner goes on unchanged: FinishSpawning with the SAME transform it gave SpawnActor and "not the default
//    transform", for which UE (Actor.cpp, FinishSpawning, the deferred-spawn transform cache) re-places an actor whose
//    spawn transform differs: template * requested, where the template is root * original^-1 = identity here. Anything
//    else: the game's SpawnActor, as stock; a held actor that could not be adopted is destroyed.
//  - Never built from a class the garbage collector is taking away (crash1118, 2026-11-18: our bug in this unreleased key).
//    A reference's class is looked up when it is noted and its build comes frames later; a class that only CreatePreload's
//    early load made resident is referenced by nothing until an actor of it exists, so a garbage-collection pass in between
//    finds it unreachable and its purge destroys and frees it. An actor built from it then outlives its class (GC keeps a
//    class alive through its live instances, UE 5.3 FastReferenceCollector.h ProcessObjects, but this class was already
//    condemned), and the next read of its class (a query's IsA, its own FinishDestroy) reads freed memory: access
//    violations at exe+0x2F8F674 and exe+0xFA381A in the gcfix runs. So: (a) nothing is built while a purge is pending (UE's
//    IsIncrementalPurgePending: from a pass's reachability analysis until its purge has freed everything it found), and
//    (b) every build looks its class up again in the build frame and builds only with what the lookup gives now (the lookup,
//    FSoftObjectPath::ResolveObject, never returns an object marked unreachable: UObjectHash.cpp StaticFindObjectFastInternal);
//    nothing found = the reference is asked for again, as when it was first noted. A held actor keeps its class alive (the
//    level's ActorsForGC references it, LevelActor.cpp SpawnActor), so a class can no longer be condemned while built
//    actors of it are held, and the adoption's class compare stays as it was.
//  - Never held across anything that may change the world: every held actor is destroyed (K2_DestroyActor, its vtable slot
//    read from the exec thunk) when a menu or loading screen freezes the game, when a fast travel is pending, when a save
//    starts (SaveBytesBulk's hook), when the key is switched off; held at most 20 s (a spare: until its base form has not
//    been created for 90 s), at most 12 at a time. Every held actor and the world are weak handles (GUObjectArray index +
//    serial number + the object's address, checked before every use, with the actor's class, "finished spawning" and
//    "being destroyed" bits): an actor the engine destroyed itself is forgotten, never touched.
// Equivalent, not identical: the actor is the one the stock SpawnActor would have made from the same class and parameters;
// what differs is when that half ran (world time, the global random stream's order, the first pose evaluated at the park),
// and that at the notice the actor's rotation, bounds and physics bodies are still the park's (no listener reads them).
// Measured (ahead2): 35 of 35 adopted characters end with the stock translation / rotation / scale; first visible poses
// within the stock run-to-run spread.
//
// Found by pattern: IsIncrementalPurgePending (as GcPassAfterBusy finds it), the spawner's SpawnActor / FinishSpawning call pair (UWorld::SpawnActor and AActor::FinishSpawning are
// the calls' targets; FinishSpawning's head pins "finished spawning" +0x5C bit 0 and the root component +0x1A0),
// AActor::K2_DestroyActor and its exec thunk (the vtable slot; "being destroyed" +0x5D bit 1), the attach's cell walk
// (cell +0xB8, node {reference, next}, the form type byte +0x08 = 0x32 / 0x33, the paired actor +0x48) and
// Spawn_Paired_Actor's reads (reference id +0x10, base form +0x50, position +0x64, the scale virtual +0x200),
// UWorld::SpawnActor's two broadcast calls (one target; the delegates' offsets in UWorld), the pawn index's read of a pawn's
// place (root +0x1A0 as FinishSpawning's, the root's ComponentToWorld translation). Any mismatch: not installed, one log line.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace opf {
namespace {

// The spawner (Altar's create, exe+0x47A35A0): mov rcx,[rsp+58h]; lea r8,[rbp+260h]; mov r9,rsi; mov rdx,rdi; call SpawnActor
constexpr char kSpawnCallLead[] = "48 8B 4C 24 58 4C 8D 85 60 02 00 00 4C 8B CE 48 8B D7 E8";
constexpr size_t kSpawnCallAt = 0x12;
// ... the same transform to FinishSpawning, bIsDefaultTransform 0: xor r9d,r9d; mov [rsp+20h],bl; xor r8d,r8d;
// lea rdx,[rbp+260h]; mov rcx,rdi; call FinishSpawning
constexpr char kFinishCallLead[] = "45 33 C9 88 5C 24 20 45 33 C0 48 8D 95 60 02 00 00 48 8B CF E8";
constexpr size_t kFinishCallAt = 0x14, kCallPairSpan = 0x200;
// AActor::FinishSpawning (exe+0x2DBA830) +0x28: movzx eax,[rcx+5Ch]; ...; test al,1; jne; or al,1; mov [rcx+5Ch],al;
// mov rax,[rcx+1A0h]
constexpr char kFinishHead[] = "0F B6 41 5C 4D 8B E1 45 0F B6 F8 48 8B DA 48 8B F1 A8 01 0F 85 ?? ?? ?? ?? 0C 01 88 41 5C 48 8B 81 A0 01 00 00";
constexpr size_t kFinishHeadAt = 0x28;
// AActor::K2_DestroyActor (exe+0x2DC21D0): test [rcx+5Dh],2; ...; UWorld::DestroyActor(world, this, false, true)
constexpr char kDestroyBody[] =
    "48 89 5C 24 08 57 48 83 EC 20 F6 41 5D 02 48 8B F9 75 25 8B 41 08 A9 00 00 00 60 75 1B E8 ?? ?? ?? ?? 48 85 C0 74 11 41 B1 01 "
    "45 33 C0 48 8B D7 48 8B C8 E8";
// execK2_DestroyActor (exe+0x2DD4FE0): P_FINISH; jmp [vtable + 0x370] (the shape is shared by every parameterless virtual's
// exec thunk; with UE 5.3's slot of K2_DestroyActor it is found once, and the slot is read from it)
constexpr char kDestroyThunk[] = "48 8B 42 20 45 33 C0 48 85 C0 41 0F 95 C0 4C 03 C0 4C 89 42 20 48 8B 01 48 FF A0 70 03 00 00 CC";
constexpr size_t kDestroyThunkSlotAt = 27;
// UWorld::SpawnActor: lea rcx,[r12+OnActorPreSpawnInitialization]; mov rdx,rbx; mov [rax+r14*8],rbx; mov [rbx+152h],r15b;
// call Broadcast (before PostSpawnInitialize)
constexpr char kPreSpawnBroadcast[] = "49 8D 8C 24 ?? ?? 00 00 48 8B D3 4A 89 1C F0 44 88 BB ?? ?? 00 00 E8";
constexpr size_t kPreSpawnCallAt = 22;
// ... its end: lea rcx,[r12+OnActorSpawned]; mov rdx,rbx; call Broadcast; mov rdx,rbx; mov rcx,r12; call AddNetworkActor
constexpr char kSpawnedBroadcast[] = "49 8D 8C 24 ?? ?? 00 00 48 8B D3 E8 ?? ?? ?? ?? 48 8B D3 49 8B CC E8";
constexpr size_t kSpawnedCallAt = 11;
constexpr size_t kSpawnFnSpan = 0x1400;
// AActor::PostSpawnInitialize (called by UWorld::SpawnActor between the two broadcasts): CreationTime =
// World->GetTimeSeconds() (UE 5.3 Actor.cpp:3743; float AActor::CreationTime, Actor.h:739; double UWorld::TimeSeconds,
// World.h:1782): mov rcx,rax; call AreActorsInitialized; movsd xmm0,[rbx+TimeSeconds]; jmp; xorps; movzx edx,[rdi+59h];
// cvtpd2ps xmm0,xmm0; movss [rdi+CreationTime],xmm0 (build 19115871: +0x6C0 and +0x158)
constexpr char kCreationTime[] = "48 8B C8 E8 ?? ?? ?? ?? F2 0F 10 83 ?? ?? 00 00 EB 03 0F 57 C0 0F B6 57 59 66 0F 5A C0 F3 0F 11 87 ?? ?? 00 00";
constexpr size_t kCreationWorldAt = 12, kCreationActorAt = 33, kPostSpawnHeadSpan = 0x100;
// UWorld::DestroyActor (K2_DestroyActor's call): OnActorDestroyed.Broadcast (LevelActor.cpp:875) and
// OnActorRemovedFromWorld.Broadcast (:997), each "lea rcx,[reg+delegate]; mov rdx,reg; call Broadcast" (the same target as
// SpawnActor's); the delegates follow OnActorSpawned in UWorld (World.h:1543-1573: +0x30 and +0x78, 0x18 each)
constexpr size_t kDestroyFnSpan = 0x800, kDestroyedAfterSpawned = 0x30, kRemovedAfterSpawned = 0x78;
// The attach (exe+0x6660630): lea r14,[rbp+0B8h]; mov rsi,r14; test r14,r14; je; mov [rsp+58h],rdi
constexpr char kWalk[] = "4C 8D B5 B8 00 00 00 49 8B F6 4D 85 F6 0F 84 ?? ?? ?? ?? 48 89 7C 24 58";
// ... mov rbx,[rsi]; test rbx,rbx; je; cmp [rbx+48h],r15; jne; mov rax,[rbx]; mov rcx,rbx; call [rax+2D8h]
constexpr char kWalkNode[] = "48 8B 1E 48 85 DB 0F 84 ?? ?? ?? ?? 4C 39 7B 48 75 ?? 48 8B 03 48 8B CB FF 90 D8 02 00 00";
// ... movzx eax,[rcx+8]; sub al,32h; cmp al,1; ja
constexpr char kWalkType[] = "0F B6 41 08 2C 32 3C 01 77 ??";
constexpr size_t kWalkSpan = 0x200;
// Spawn_Paired_Actor (exe+0x6587130; rsi = reference + 0x38): the position (+0x2C), the ids, the scale virtual
constexpr char kPairedPos[] = "8B 46 28 48 8D 4E C8 F2 0F 10 46 20 F2 0F 10 7E 2C 44 8B 6E 34";
constexpr char kPairedIds[] = "8B 86 D0 00 00 00 48 8D 4E 50 44 8B 7E D8 B2 5E 89 45 98 48 8B 46 18 44 8B 60 10";
constexpr char kPairedScale[] = "48 8B 46 C8 FF 90 00 02 00 00 48 8D 4E 50 F3 0F 11 45 C0";
constexpr size_t kPairedSpan = 0x300;
// The pawn's box in the game's pawn index (the SteeringBehaviors plugin's PawnSpatialIndexSubsystem, an OnActorSpawned
// listener that files the pawn by its place at the notice; exe+0x4506D70): lea rcx,[r8+298h]; mov rax,[rcx]; mov rbx,r8;
// movaps; mov rdi,rdx; movaps; call [rax+10h] (the capsule); mov rcx,[rbx+RootComponent]; test rcx,rcx; je;
// movups xmm0,[rcx+ComponentToWorld.Translation]; movups [rsp+20h],xmm0; movups xmm0,[rcx+Translation+10h] (= the inlined
// GetActorLocation: the root's ComponentToWorld translation, +0x1A0 / +0x280 on build 19115871)
constexpr char kPawnBox[] =
    "49 8D 88 ?? ?? 00 00 48 8B 01 49 8B D8 0F 29 7C 24 ?? 48 8B FA 44 0F 29 44 24 ?? FF 50 10 48 8B 8B ?? ?? 00 00 48 85 C9 74 ?? "
    "0F 10 81 ?? ?? 00 00 0F 11 44 24 ?? 0F 10 81 ?? ?? 00 00";
constexpr size_t kPawnBoxRootAt = 33, kPawnBoxLocAt = 45, kPawnBoxLoc2At = 57;
constexpr uint32_t kRootComponent = 0x1A0;  // AActor::RootComponent as FinishSpawning's head (kFinishHead) pins it

constexpr size_t kParamsSize = 0x80, kXfSize = 0x60;
constexpr size_t kObjIndex = 0x0C, kObjClass = 0x10, kActorFinished = 0x5C, kActorDestroying = 0x5D;
constexpr size_t kCellRefs = 0xB8, kFormType = 0x08, kFormId = 0x10, kRefPaired = 0x48, kRefBase = 0x50, kRefPos = 0x64;
constexpr size_t kScaleSlot = 0x200 / 8;
constexpr size_t kArrayChunks = 0x10, kArrayNum = 0x24;  // FUObjectArray::ObjObjects (as render/water_info_skip.cpp)
constexpr int32_t kItemDead = (1 << 21) | (1 << 28) | (1 << 29);  // EInternalObjectFlags Garbage, Unreachable, PendingKill
constexpr double kOldToUe = 100.0 / 70.0;
constexpr double kParkCm = 50000.0, kHoldS = 20.0, kSpareForgetS = 90.0, kRequeueS = 120.0;
constexpr double kBuildWindowS = 10.0;  // a reference noted longer ago than this is no longer built (its create is not coming soon)
constexpr int kMaxHeld = 12, kSparesPerBase = 3;
constexpr int kRoomForRefs = 4;  // spares are only built while fewer than kMaxHeld - this are held (room for references)
// CreateAheadPool: 0 = kMaxHeld and no eviction; else the cap on actors held (kPoolMin..kPoolMax) and an idle spare of
// another base form (not created for kSpareIdleS, and longer ago than the base form that wants a spare) makes room
constexpr int kPoolMin = 12, kPoolMax = 32;
constexpr double kSpareIdleS = 20.0;
constexpr int kMaxQueue = 256, kMaxTable = 256, kMaxLearned = 256, kMaxBases = 32, kMaxDropped = 64;

using SpawnFn = uint8_t* (*)(uint8_t* world, uint8_t* cls, const uint8_t* xf, const uint8_t* params);
using DestroyFn = void (*)(uint8_t* actor);
using BroadcastFn = void (*)(void* delegate, uint8_t* actor);
using ScaleFn = float (*)(const uint8_t* ref);
using PurgeFn = bool (*)();

enum EntryState : uint8_t { kFree, kNew, kReady, kHeld };

struct Handle {  // what an FWeakObjectPtr holds, plus the address
    uint8_t* obj = nullptr;
    int32_t index = -1, serial = 0;
};
struct Seen {
    uint32_t ref, base;
    float pos[3];
    float scale;
};
struct Entry {
    EntryState state;
    bool spare;  // for any run-time reference of this base form (ref 0)
    uint32_t ref, base;
    float pos[3];
    double scale;  // the scale it is built with
    double at[3];  // the translation it is built at (the park place)
    uint8_t* cls;
    Handle actor;
    double tSeen, tBuilt;
    float buildMs;
};

struct State {
    std::atomic<bool> installed{false};
    std::atomic<int> mode{0};
    std::atomic<int> slackUs{4000};
    std::atomic<int> pool{0};  // CreateAheadPool (0 = kMaxHeld, no eviction of idle spares)
    std::atomic<int> levelled{0};  // CreateAheadLevelled (1: run-time references already made, found in the cells ahead)
    uint8_t *gameBase = nullptr, *textBegin = nullptr, *textEnd = nullptr;
    SpawnFn spawn = nullptr;
    size_t destroySlot = 0;
    uint32_t spawnedDelegate = 0, preSpawnDelegate = 0;
    BroadcastFn broadcast = nullptr;
    uint32_t destroyedDelegate = 0, removedDelegate = 0;
    uint32_t creationTimeOff = 0, worldTimeOff = 0;  // AActor::CreationTime (float), UWorld::TimeSeconds (double)
    uint32_t rootOff = 0, locOff = 0;                // AActor::RootComponent, its ComponentToWorld translation (aheadindex)
    PurgeFn purgePending = nullptr;                  // UE's IsIncrementalPurgePending (crash1118)
    // != 0: this thread is inside a build's SpawnActor (kind 1: its two spawn broadcasts are held back) or inside the destroy
    // of an actor built ahead and never adopted (kind 2: its two destroy broadcasts are not made either)
    std::atomic<DWORD> quietThread{0};
    std::atomic<int> quietKind{0};
    std::atomic<bool> faulted{false};  // a build faulted: off for the session, the ini's re-read cannot switch it back on
    uint8_t* objArray = nullptr;
    // self-test fakes
    CreateAheadFakes fakes{};
    bool test = false;
    // OT -> GT
    std::mutex mu;
    Seen q[kMaxQueue];
    int nq = 0;
    std::atomic<uint64_t> cells{0}, actorRefs{0}, qFull{0}, walkFaults{0};
    // game thread
    Entry e[kMaxTable]{};
    int held = 0;
    Handle world;
    uint8_t tmpl[kParamsSize]{};
    bool haveTmpl = false, tmplRefused = false;
    bool inCreate = false, curUsed = false, curAdopted = false;
    uint32_t curRef = 0, curBase = 0;
    struct BaseScale {
        uint32_t base;
        double scale;
    } learned[kMaxLearned]{};
    int nLearned = 0, learnedNext = 0;
    struct SpareBase {
        uint32_t base;
        double scale, lastUse;
    } sb[kMaxBases]{};
    int nSb = 0;
    struct Dropped {
        uint32_t ref;
        double t;
    } dropped[kMaxDropped]{};
    int droppedNext = 0;
    int listenersLogged = -1;
    // classes whose actors' roots carry a translation of their own (reason 6 met once): nothing is built ahead of them any more
    // (aheadindex review SF2: such a class would otherwise cost a wasted build and a destroy at every create)
    uint8_t* offsetCls[16]{};
    int nOffsetCls = 0;
    std::atomic<uint64_t> offsetSkipped{0};
    int eventLogs = 0;
    // stats (game thread writes; the stats line reads)
    std::atomic<uint64_t> queued{0}, built{0}, builtUs{0}, adopted{0}, spareAdopted{0}, late{0}, rejClass{0}, rejParams{0}, rejScale{0},
        rejGone{0}, rejPlace{0}, dropTimeout{0}, dropFreeze{0}, dropTravel{0}, dropSave{0}, dropOff{0}, dropCap{0}, dropWorld{0}, gone{0}, noSlack{0},
        busy{0}, spawnNull{0}, destroySkipped{0}, creates{0}, pawnCreates{0}, heldBack{0}, destroyQuiet{0}, dropIdle{0};
    std::atomic<uint64_t> runtimeNoted{0}, runtimeTook{0}, runtimeLate{0}, rejBase{0};  // CreateAheadLevelled
    // run-time references noted ahead whose create never came (review rev-levelled S3): all, and of them the ones already built
    std::atomic<uint64_t> runtimeNever{0}, runtimeNeverBuilt{0};
    std::atomic<uint64_t> dropAllMaxUs{0};  // the longest drop of every held actor at once (menu, loading, travel, save)
    std::atomic<int> dropAllMaxN{0};        // and how many actors it destroyed
    // crash1118: slack frames that built nothing because a GC purge was pending; builds whose class the build frame's lookup
    // gave anew (another object now) or no longer found
    std::atomic<uint64_t> purgeWait{0}, classRenewed{0}, classLost{0};
    uint64_t statsSeen = 0;
} g;

double NowS()
{
    if (g.test && g.fakes.clockS) return *g.fakes.clockS;
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<double>(v.QuadPart) / static_cast<double>(freq.QuadPart);
}
bool OnGt() { return g.test || NpcPacingOnGameThread(); }
bool PurgePending()
{
    if (g.test) return g.fakes.purge && *g.fakes.purge != 0;
    return g.purgePending && g.purgePending();
}

// The GUObjectArray item of an index: its object, flags and serial number. False: no such item / unreadable.
bool ItemOf(int32_t index, const uint8_t** obj, int32_t* flags, int32_t* serial)
{
    const uint8_t* const arr = g.objArray;
    if (!arr || index < 0) return false;
    __try {
        const int32_t num = *reinterpret_cast<const int32_t*>(arr + kArrayNum);
        uint8_t* const* const chunks = *reinterpret_cast<uint8_t* const* const*>(arr + kArrayChunks);
        if (index >= num || !chunks) return false;
        const uint8_t* const chunk = chunks[index >> 16];
        if (!chunk) return false;
        const uint8_t* const item = chunk + 24 * (index & 0xFFFF);  // FUObjectItem {Object, Flags, ClusterRootIndex, SerialNumber}
        *obj = *reinterpret_cast<const uint8_t* const*>(item);
        *flags = *reinterpret_cast<const int32_t*>(item + 8);
        *serial = *reinterpret_cast<const int32_t*>(item + 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// requireSerial: an actor's handle needs a serial number (PostSpawnInitialize's deferred-transform cache entry is a weak
// pointer, so a spawned actor has one); without one a reused slot could not be told apart.
bool MakeHandle(uint8_t* obj, Handle* h, bool requireSerial)
{
    *h = {};
    if (!obj) return false;
    int32_t index = -1;
    __try {
        index = *reinterpret_cast<const int32_t*>(obj + kObjIndex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const uint8_t* o = nullptr;
    int32_t flags = 0, serial = 0;
    if (!ItemOf(index, &o, &flags, &serial) || o != obj || (flags & kItemDead) || (requireSerial && !serial)) return false;
    *h = {obj, index, serial};
    return true;
}
// The object the handle was made for still sits in its slot, with the same serial number, and is not garbage.
bool Live(const Handle& h)
{
    const uint8_t* o = nullptr;
    int32_t flags = 0, serial = 0;
    return h.obj && ItemOf(h.index, &o, &flags, &serial) && o == h.obj && serial == h.serial && !(flags & kItemDead);
}
// A held actor: live, still of the class it was spawned as, not finished by anybody, not being destroyed.
bool HeldLive(const Entry& en)
{
    if (!Live(en.actor)) return false;
    __try {
        const uint8_t* const a = en.actor.obj;
        return *reinterpret_cast<uint8_t* const*>(a + kObjClass) == en.cls && !(a[kActorFinished] & 1) && !(a[kActorDestroying] & 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallDestroy(uint8_t* a)
{
    __try {
        void** const vt = *reinterpret_cast<void***>(a);
        uint8_t* const fn = static_cast<uint8_t*>(vt[g.destroySlot]);
        if (!g.test && (fn < g.textBegin || fn >= g.textEnd)) return false;
        reinterpret_cast<DestroyFn>(fn)(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The destroy of an actor built ahead and never adopted: UWorld::DestroyActor's OnActorDestroyed and
// OnActorRemovedFromWorld broadcasts are not made for it (their listeners never heard of the actor: its spawn broadcasts
// were held back), so no listener sees a removal without the add. OnPreUnregisterAllActorComponents still fires, paired
// with the OnPostRegisterAllActorComponents its build made.
bool QuietDestroy(uint8_t* a)
{
    g.quietKind.store(2, std::memory_order_relaxed);
    g.quietThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const bool ok = CallDestroy(a);
    g.quietThread.store(0, std::memory_order_relaxed);
    return ok;
}

void NoteDropped(uint32_t ref)
{
    if (!ref) return;
    g.dropped[g.droppedNext] = {ref, NowS()};
    g.droppedNext = (g.droppedNext + 1) % kMaxDropped;
}
bool RecentlyDropped(uint32_t ref, double now)
{
    for (const State::Dropped& d : g.dropped)
        if (d.ref == ref && now - d.t < kRequeueS) return true;
    return false;
}

// Frees the entry; a held actor that is still ours is destroyed, one the engine took away is only forgotten.
void Release(Entry& en, const char* why, std::atomic<uint64_t>* counter)
{
    const bool wasHeld = en.state == kHeld;
    en.state = kFree;
    if (!wasHeld) return;
    --g.held;
    const Handle h = en.actor;
    const bool live = HeldLive(en);
    en.actor = {};
    if (!live) {
        g.gone.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
    const double t0 = NowS();
    const bool ok = QuietDestroy(h.obj);
    if (!ok) g.destroySkipped.fetch_add(1, std::memory_order_relaxed);
    (void)t0;
    (void)why;
}

void ReleaseAll(const char* why, std::atomic<uint64_t>* counter)
{
    LARGE_INTEGER a, b, f;
    QueryPerformanceCounter(&a);
    int n = 0;
    for (Entry& en : g.e)
        if (en.state != kFree) {
            if (en.state == kHeld) ++n;
            Release(en, why, counter);
        }
    if (n) {
        QueryPerformanceCounter(&b);
        QueryPerformanceFrequency(&f);
        const uint64_t us = static_cast<uint64_t>((b.QuadPart - a.QuadPart) * 1000000 / (f.QuadPart ? f.QuadPart : 1));
        if (us > g.dropAllMaxUs.load(std::memory_order_relaxed)) {
            g.dropAllMaxUs.store(us, std::memory_order_relaxed);
            g.dropAllMaxN.store(n, std::memory_order_relaxed);
        }
    }
    g.held = 0;
    std::lock_guard<std::mutex> lock(g.mu);
    g.nq = 0;
}

// FActorSpawnParameters (UE 5.3): Name, Template, Owner, Instigator, OverrideLevel, OverrideParentComponent (0x00-0x2F);
// collision handling 0x30, scale method 0x31, the flag bits 0x32 (remote owned, no fail, defer construction, allow during
// construction script, force globally unique name), name mode 0x33, ObjectFlags 0x34; the pre-spawn TFunction 0x38
// (UE 5.3 Function.h:663-666: Callable 0x38, Storage's heap allocation 0x40; both 0 = unbound). Returns 0 when equal to the
// template, else the first differing offset + 1.
int ParamsDiffer(const uint8_t* tmpl, const uint8_t* p)
{
    for (int o = 0; o < 0x30; o += 8)
        if (memcmp(p + o, tmpl + o, 8)) return o + 1;
    if (p[0x30] != tmpl[0x30]) return 0x31;
    if (p[0x31] != tmpl[0x31]) return 0x32;
    if ((p[0x32] & 0x1F) != (tmpl[0x32] & 0x1F)) return 0x33;
    if (p[0x33] != tmpl[0x33]) return 0x34;
    if (memcmp(p + 0x34, tmpl + 0x34, 4)) return 0x35;
    if (*reinterpret_cast<const uint64_t*>(p + 0x38)) return 0x39;
    if (*reinterpret_cast<const uint64_t*>(p + 0x40)) return 0x41;
    return 0;
}
// A parameter set the fix may copy: a deferred AlwaysSpawn create with no name, template, owner, instigator, level, parent
// component or pre-spawn function (so the copy holds no pointer).
bool ParamsUsable(const uint8_t* p)
{
    for (int o = 0; o < 0x30; o += 8)
        if (*reinterpret_cast<const uint64_t*>(p + o)) return false;
    return p[0x30] == 1 && (p[0x32] & 4) && *reinterpret_cast<const uint64_t*>(p + 0x38) == 0 &&
           *reinterpret_cast<const uint64_t*>(p + 0x40) == 0;
}
// The scale rule: the three requested components equal the build scale bit for bit, and scale * (1 / scale) is exactly 1
// (FinishSpawning's re-place computes root * original^-1 * requested: exact only then).
bool ScaleAdoptable(const double* s, double built)
{
    if (!(s[0] == built && s[1] == built && s[2] == built)) return false;
    volatile double inv = 1.0 / built;
    volatile double k = built * inv;
    return k == 1.0 && built > 0.0;
}
bool ScaleBuildable(double s) { return std::isfinite(s) && s >= 0.1 && s <= 10.0; }

uint8_t* DoSpawn(uint8_t* world, uint8_t* cls, const uint8_t* xf, const uint8_t* params)
{
    if (g.test) return g.fakes.spawn ? g.fakes.spawn(world, cls, xf, params) : nullptr;
    return g.spawn(world, cls, xf, params);
}
uint8_t* GuardedSpawn(uint8_t* world, uint8_t* cls, const uint8_t* xf, const uint8_t* params, bool* fault)
{
    __try {
        return DoSpawn(world, cls, xf, params);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return nullptr;
    }
}
uint8_t* ClassOf(uint32_t base, bool request)
{
    if (g.test) return g.fakes.classOf ? static_cast<uint8_t*>(g.fakes.classOf(base, request)) : nullptr;
    return static_cast<uint8_t*>(CreatePreloadClassFor(base, request));
}

void LearnScale(uint32_t base, double scale)
{
    for (int k = 0; k < g.nLearned; ++k)
        if (g.learned[k].base == base) {
            g.learned[k].scale = scale;
            return;
        }
    int k = g.nLearned;
    if (k < kMaxLearned) ++g.nLearned;
    else k = g.learnedNext = (g.learnedNext + 1) % kMaxLearned;
    g.learned[k] = {base, scale};
}
bool LearnedScale(uint32_t base, double* scale)
{
    for (int k = 0; k < g.nLearned; ++k)
        if (g.learned[k].base == base) {
            *scale = g.learned[k].scale;
            return true;
        }
    return false;
}

Entry* Find(uint32_t ref)
{
    if (!ref) return nullptr;
    for (Entry& en : g.e)
        if (en.state != kFree && !en.spare && en.ref == ref) return &en;
    return nullptr;
}
Entry* FreeSlot()
{
    for (Entry& en : g.e)
        if (en.state == kFree) return &en;
    return nullptr;
}

// A UWorld delegate's listeners (a TMulticastDelegate: TArray of delegate bases {allocation, size}); -1 unknown.
int Listeners(const uint8_t* world, uint32_t off, const char* what)
{
    if (!off || !world) return -1;
    (void)what;
    __try {
        const uint8_t* const d = world + off;
        const uint8_t* const data = *reinterpret_cast<const uint8_t* const*>(d);
        const int32_t num = *reinterpret_cast<const int32_t*>(d + 8);
        if (num < 0 || num > 4096 || (num && !data)) return -1;
        int bound = 0;
        for (int i = 0; i < num; ++i)
            if (*reinterpret_cast<const uint64_t*>(data + 16 * i)) ++bound;
        return bound;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// UWorld::SpawnActor's two broadcasts (OnActorPreSpawnInitialization, OnActorSpawned; both call sites redirected here).
// Inside a build they are held back: the listeners hear of the actor when the game's create takes it, at the place in the
// create where the stock SpawnActor would have told them, and never of an actor that is destroyed unused. (A listener of this
// game keeps every spawned character in a table that only the character's end of play clears: an actor destroyed before it
// began play stayed in it and crashed the table's next pass after the garbage collection.)
// UWorld::DestroyActor's two broadcasts (OnActorDestroyed, OnActorRemovedFromWorld) come here too: they are not made for
// an actor built ahead that is destroyed unused (QuietDestroy), whose listeners never heard of it.
void BroadcastHook(void* delegate, uint8_t* actor)
{
    if (g.quietThread.load(std::memory_order_relaxed) == GetCurrentThreadId()) {
        (g.quietKind.load(std::memory_order_relaxed) == 2 ? g.destroyQuiet : g.heldBack).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g.test) {
        if (g.fakes.broadcast) g.fakes.broadcast(delegate, actor);
        return;
    }
    g.broadcast(delegate, actor);
}
// At the adoption: the actor's CreationTime = the world's TimeSeconds, converted as PostSpawnInitialize converts it.
void StampCreationTime(uint8_t* actor, const uint8_t* world)
{
    if (!g.creationTimeOff || !g.worldTimeOff) return;
    __try {
        *reinterpret_cast<float*>(actor + g.creationTimeOff) = static_cast<float>(*reinterpret_cast<const double*>(world + g.worldTimeOff));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void TellListeners(uint8_t* world, uint8_t* actor)
{
    if (g.test) {
        if (g.fakes.broadcast) {
            g.fakes.broadcast(world + 1, actor);
            g.fakes.broadcast(world + 2, actor);
        }
        return;
    }
    g.broadcast(world + g.preSpawnDelegate, actor);
    g.broadcast(world + g.spawnedDelegate, actor);
}
// The held actor's root component's ComponentToWorld translation (what GetActorLocation returns), or null.
double* RootLoc(uint8_t* actor)
{
    if (!g.rootOff || !g.locOff) return nullptr;
    __try {
        uint8_t* const root = *reinterpret_cast<uint8_t* const*>(actor + g.rootOff);
        return root ? reinterpret_cast<double*>(root + g.locOff) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
// The actor still stands exactly where it was built (its root's translation equals the build transform's, compared as
// numbers: a zero's sign is the only bit that may differ): then the root has no offset of its own and the stock SpawnActor
// would have left it at exactly the requested translation, which the adoption shows the listeners (aheadindex).
bool AtBuildPlace(const Entry& en)
{
    const double* const p = RootLoc(en.actor.obj);
    if (!p) return false;
    __try {
        return p[0] == en.at[0] && p[1] == en.at[1] && p[2] == en.at[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Why a held actor cannot be given to this create (0 = it can): 1 class, 2 scale, 3 parameters, 4 gone, 5 key off,
// 6 not at its build place. s: the create's scale.
int NotAdoptable(const Entry& en, int mode, const uint8_t* cls, const double* s, const uint8_t* params)
{
    if (!mode) return 5;
    if (!HeldLive(en)) return 4;
    if (en.cls != cls) return 1;
    if (!g.haveTmpl || ParamsDiffer(g.tmpl, params)) return 3;
    if (!ScaleAdoptable(s, en.scale)) return 2;
    if (!AtBuildPlace(en)) return 6;
    return 0;
}
bool IsOffsetClass(const uint8_t* cls)
{
    for (int i = 0; i < g.nOffsetCls; ++i)
        if (g.offsetCls[i] == cls) return true;
    return false;
}
void CountNotAdopted(int why, const Entry& en)
{
    (why == 1 ? g.rejClass : why == 2 ? g.rejScale : why == 3 ? g.rejParams : why == 6 ? g.rejPlace : g.rejGone)
        .fetch_add(1, std::memory_order_relaxed);
    if (why != 6 || !en.cls || IsOffsetClass(en.cls) || g.nOffsetCls >= 16) return;
    // this class's root has an offset of its own: no more builds of it (spares: its base form leaves the spare table too)
    g.offsetCls[g.nOffsetCls++] = en.cls;
    for (int k = 0; k < g.nSb; ++k)
        if (g.sb[k].base == en.base) {
            g.sb[k] = g.sb[--g.nSb];
            break;
        }
    Log("[createahead] a character class's root has an offset of its own (base form %X): nothing is built ahead of that class any more",
        en.base);
}
// The spawner's SpawnActor call.
uint8_t* SpawnCallHook(uint8_t* world, uint8_t* cls, const uint8_t* xf, const uint8_t* params)
{
    if (!g.inCreate || g.curUsed || !xf || !params || !OnGt()) return DoSpawn(world, cls, xf, params);
    g.curUsed = true;  // one spawn per create command
    g.creates.fetch_add(1, std::memory_order_relaxed);
    const bool pawn = params[0x30] == 1 && (params[0x32] & 4);
    if (!pawn) return DoSpawn(world, cls, xf, params);
    g.pawnCreates.fetch_add(1, std::memory_order_relaxed);
    const int mode = g.mode.load(std::memory_order_relaxed);
    if (g.world.obj != world || !Live(g.world)) {  // another world: nothing held belongs to it
        if (g.world.obj) ReleaseAll("another world", &g.dropWorld);
        g.haveTmpl = false;
        if (!MakeHandle(world, &g.world, false)) g.world = {};
    }
    if (!g.haveTmpl && !g.tmplRefused && g.world.obj) {
        if (ParamsUsable(params)) {
            memcpy(g.tmpl, params, kParamsSize);
            memset(g.tmpl + 0x38, 0, kParamsSize - 0x38);  // the unbound pre-spawn function's storage is not copied
            g.haveTmpl = true;
            const int n = Listeners(world, g.spawnedDelegate, "OnActorSpawned");
            const int nPre = Listeners(world, g.preSpawnDelegate, "OnActorPreSpawnInitialization");
            const int nDes = Listeners(world, g.destroyedDelegate, "OnActorDestroyed");
            const int nRem = Listeners(world, g.removedDelegate, "OnActorRemovedFromWorld");
            const int sig = ((n & 0xFF) << 24) | ((nPre & 0xFF) << 16) | ((nDes & 0xFF) << 8) | (nRem & 0xFF);
            if (g.listenersLogged != sig) {
                g.listenersLogged = sig;
                Log("[createahead] spawn parameters taken from a stock character create (collision %u, scale method %u, flags %02X, name "
                    "mode %u, object flags %08X); UWorld listeners: OnActorSpawned %d, OnActorPreSpawnInitialization %d, OnActorDestroyed "
                    "%d, OnActorRemovedFromWorld %d",
                    params[0x30], params[0x31], params[0x32] & 0x1F, params[0x33], *reinterpret_cast<const uint32_t*>(params + 0x34), n, nPre,
                    nDes, nRem);
            }
        } else {
            g.tmplRefused = true;
            Log("[createahead] the spawner's parameters carry a name, template, owner, instigator, level, parent or pre-spawn function: "
                "nothing is built ahead");
        }
    }
    const double* const s = reinterpret_cast<const double*>(xf + 0x40);
    const bool uniform = s[0] == s[1] && s[1] == s[2];
    if (uniform) LearnScale(g.curBase, s[0]);
    Entry* en = Find(g.curRef);
    const bool dyn = g.curRef >= 0xFF000000u;
    if (en && dyn) {
        // a run-time reference noted in a cell ahead (CreateAheadLevelled): taken only when it passes the whole adoption check
        // below (built, same base form -- a run-time id could have been given to another reference since --, class, parameters,
        // scale; review rev-levelled S2); otherwise it is let go and the spares below apply as for any run-time reference, so
        // a held actor of this kind never makes a create worse than with the key at 0. With the key at 0 (live) it is not taken
        // at all: the frame hook lets it go (review rev-levelled S1).
        if (g.levelled.load(std::memory_order_relaxed) == 0) {
            en = nullptr;
        } else if (en->state != kHeld) {
            g.runtimeLate.fetch_add(1, std::memory_order_relaxed);
            en->state = kFree;
            en = nullptr;
        } else if (en->base != g.curBase) {
            g.rejBase.fetch_add(1, std::memory_order_relaxed);
            Release(*en, "another base form", nullptr);
            en = nullptr;
        } else if (const int why = NotAdoptable(*en, mode, cls, s, params)) {
            CountNotAdopted(why, *en);
            Release(*en, "not adoptable", nullptr);
            en = nullptr;
        }
    }
    bool spareBase = dyn;
    if (spareBase && mode && !IsOffsetClass(cls)) {
        const double now = NowS();
        if (!en && dyn)
            for (Entry& c : g.e)
                if (c.state == kHeld && c.spare && c.base == g.curBase && c.cls == cls && uniform && c.scale == s[0]) {
                    en = &c;
                    break;
                }
        int k = 0;
        for (; k < g.nSb; ++k)
            if (g.sb[k].base == g.curBase) break;
        if (k == g.nSb && g.nSb == kMaxBases) {  // full: the least recently used base form makes room (its spares time out)
            k = 0;
            for (int j = 1; j < kMaxBases; ++j)
                if (g.sb[j].lastUse < g.sb[k].lastUse) k = j;
        } else if (k == g.nSb) {
            ++g.nSb;
        }
        g.sb[k] = {g.curBase, uniform ? s[0] : 1.0, now};
    }
    if (en && en->state == kHeld) {
        const int why = NotAdoptable(*en, mode, cls, s, params);
        if (!why) {
            uint8_t* const a = en->actor.obj;
            if (en->spare) g.spareAdopted.fetch_add(1, std::memory_order_relaxed);
            else if (dyn) g.runtimeTook.fetch_add(1, std::memory_order_relaxed);
            en->actor = {};
            en->state = kFree;
            --g.held;
            g.adopted.fetch_add(1, std::memory_order_relaxed);
            g.curAdopted = true;
            if (g.eventLogs < 8) {
                ++g.eventLogs;
                Log("[createahead] a create took the actor built ahead for it (reference %X, base form %X)", g.curRef, g.curBase);
            }
            // its age as stock: CreationTime = the world's time now (the frame of the stock SpawnActor, so the same value;
            // PostSpawnInitialize wrote the build's time; UE 5.3 Actor.cpp:3743, read by GetGameTimeSinceCreation, :5938)
            StampCreationTime(a, world);
            // the two broadcasts held back at the build, now, where the stock SpawnActor makes them, and with the actor where
            // the stock SpawnActor would have it at that moment (aheadindex: our bug, in this key before any release): the game's
            // pawn index (an OnActorSpawned listener) files the pawn by its root's ComponentToWorld translation and re-files
            // it only once it moves, so a pawn announced at its park place stayed filed 500 m below for up to 300 s, unseen
            // by other characters' avoidance. The translation is shown for the broadcasts only and put back, so FinishSpawning
            // re-places the actor exactly as before (it reads the root's transform against the deferred-spawn cache's).
            double* const loc = RootLoc(a);  // non-null: NotAdoptable checked it (at the build place)
            double park[3];
            memcpy(park, loc, sizeof park);
            memcpy(loc, reinterpret_cast<const double*>(xf) + 4, sizeof park);
            TellListeners(world, a);
            memcpy(loc, park, sizeof park);
            return a;  // the spawner goes on: FinishSpawning with its transform (UE re-places), the rest
        }
        CountNotAdopted(why, *en);
        Release(*en, "not adoptable", nullptr);
    } else if (en) {
        g.late.fetch_add(1, std::memory_order_relaxed);
        en->state = kFree;
    }
    return DoSpawn(world, cls, xf, params);
}

// The cell's character / creature references without an actor (SEH: no C++ objects here). At most 64. runtime: also the
// run-time references (ids FF......: the game's levelled spawns, made at an earlier attach of the cell; CreateAheadLevelled).
int WalkCell(const uint8_t* cell, Seen* found, bool runtime, int* nRuntime)
{
    int n = 0;
    __try {
        const uint8_t* node = cell + kCellRefs;
        for (int i = 0; node && i < 4096; ++i) {
            const uint8_t* const ref = *reinterpret_cast<const uint8_t* const*>(node);
            if (!ref) break;
            const uint8_t type = ref[kFormType];
            if ((type == 0x32 || type == 0x33) && *reinterpret_cast<const uint64_t*>(ref + kRefPaired) == 0 && n < 64) {
                const uint8_t* const baseForm = *reinterpret_cast<const uint8_t* const*>(ref + kRefBase);
                if (baseForm) {
                    Seen& s = found[n];
                    s.ref = *reinterpret_cast<const uint32_t*>(ref + kFormId);
                    s.base = *reinterpret_cast<const uint32_t*>(baseForm + kFormId);
                    memcpy(s.pos, ref + kRefPos, 12);
                    // the scale Spawn_Paired_Actor puts into the create command: the reference's own virtual
                    void** const vt = *reinterpret_cast<void** const*>(ref);
                    uint8_t* const fn = static_cast<uint8_t*>(vt[kScaleSlot]);
                    s.scale = (g.test || (fn >= g.textBegin && fn < g.textEnd)) ? reinterpret_cast<ScaleFn>(fn)(ref) : 0.0f;
                    const bool rt = s.ref >= 0xFF000000u;
                    if (s.ref && (!rt || runtime) && s.base && std::isfinite(s.pos[0]) && std::isfinite(s.pos[1]) &&
                        std::isfinite(s.pos[2]) && std::fabs(s.pos[2]) < 200000.0f) {
                        ++n;
                        if (rt) ++*nRuntime;
                    }
                }
            }
            node = *reinterpret_cast<const uint8_t* const*>(node + 8);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g.walkFaults.fetch_add(1, std::memory_order_relaxed);
        n = 0;
    }
    return n;
}

uint8_t* One(const GameModule& game, const char* pattern)
{
    const auto hits = FindAll(game.text, ParsePattern(pattern));
    return hits.size() == 1 ? hits[0] : nullptr;
}
bool Within(const uint8_t* at, const uint8_t* from, size_t span) { return at && from && at >= from && at < from + span; }

void Build(Entry* pick, uint64_t sleepUs, double now)
{
    // the class as the lookup gives it now (crash1118): the one noted may have been collected since (see the head comment)
    uint8_t* const cls = ClassOf(pick->base, false);
    if (cls != pick->cls) {
        if (!cls) {
            g.classLost.fetch_add(1, std::memory_order_relaxed);
            pick->cls = nullptr;
            pick->state = pick->spare ? kFree : kNew;  // a reference: its class is asked for again (its packages requested)
            return;
        }
        g.classRenewed.fetch_add(1, std::memory_order_relaxed);
        pick->cls = cls;
    }
    alignas(16) uint8_t params[kParamsSize];
    memcpy(params, g.tmpl, kParamsSize);
    alignas(16) double xf[kXfSize / 8] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0};
    xf[4] = static_cast<double>(pick->pos[0]) * kOldToUe;
    xf[5] = -static_cast<double>(pick->pos[1]) * kOldToUe;
    xf[6] = static_cast<double>(pick->pos[2]) * kOldToUe - kParkCm;
    xf[8] = xf[9] = xf[10] = pick->scale;
    bool fault = false;
    const double t0 = NowS();
    g.quietKind.store(1, std::memory_order_relaxed);
    g.quietThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    uint8_t* const a = GuardedSpawn(g.world.obj, pick->cls, reinterpret_cast<const uint8_t*>(xf), params, &fault);
    g.quietThread.store(0, std::memory_order_relaxed);
    const double ms = (NowS() - t0) * 1000.0;
    if (fault) {
        Log("[createahead] SpawnActor faulted while building ahead: switched off for this session (an ini change does not switch it "
            "back on)");
        g.faulted.store(true);
        g.mode.store(0, std::memory_order_relaxed);
    }
    Handle h;
    if (!a || !MakeHandle(a, &h, true)) {
        g.spawnNull.fetch_add(1, std::memory_order_relaxed);
        pick->state = kFree;
        if (a) QuietDestroy(a);  // no handle can be made for it: not kept
        return;
    }
    pick->actor = h;
    pick->state = kHeld;
    memcpy(pick->at, xf + 4, sizeof pick->at);
    pick->tBuilt = now;
    pick->buildMs = static_cast<float>(ms);
    ++g.held;
    g.built.fetch_add(1, std::memory_order_relaxed);
    g.builtUs.fetch_add(static_cast<uint64_t>(ms * 1000.0), std::memory_order_relaxed);
    (void)sleepUs;
}

}  // namespace

// The cell reference walk's layout alone (CellObjectPreload, streaming/cell_preload.cpp): the attach's walk (cell +0xB8, node
// {reference, next}, the paired actor +0x48, the type byte +0x08) and Spawn_Paired_Actor's reads (reference id +0x10, base
// form +0x50, its id +0x10); the same patterns as CreateAheadDiscover's, independent of CreateAhead's other sites.
bool CellRefWalkFound(const GameModule& game, const char** missing)
{
    *missing = nullptr;
    const uint8_t* const walk = One(game, kWalk);
    const uint8_t* const node = One(game, kWalkNode);
    const uint8_t* const type = One(game, kWalkType);
    if (!walk || !Within(node, walk, kWalkSpan) || !Within(type, walk, kWalkSpan)) {
        *missing = "the attach's cell reference walk";
        return false;
    }
    const uint8_t* const pos = One(game, kPairedPos);
    const uint8_t* const ids = One(game, kPairedIds);
    if (!pos || !Within(ids, pos, kPairedSpan)) {
        *missing = "Spawn_Paired_Actor's reads (reference id, base form)";
        return false;
    }
    return true;
}

bool CreateAheadDiscover(const GameModule& game, CreateAheadSites* out, const char** missing)
{
    *out = {};
    *missing = nullptr;
    const auto fail = [&](const char* what) { *missing = what; return false; };
    uint8_t* const sc = One(game, kSpawnCallLead);
    if (!sc) return fail("the spawner's SpawnActor call");
    out->spawnCall = sc + kSpawnCallAt;
    uint8_t* const fc = One(game, kFinishCallLead);
    if (!fc || !Within(fc, sc, kCallPairSpan)) return fail("the spawner's FinishSpawning call");
    out->finishCall = fc + kFinishCallAt;
    out->spawnFn = RipTarget(out->spawnCall, 1, 5);
    out->finishFn = RipTarget(out->finishCall, 1, 5);
    if (!game.text.Contains(out->spawnFn) || !game.text.Contains(out->finishFn)) return fail("the SpawnActor / FinishSpawning call targets");
    if (!MatchesAt(out->finishFn + kFinishHeadAt, ParsePattern(kFinishHead))) return fail("AActor::FinishSpawning's head (finished bit, root)");
    if (!(out->destroyFn = One(game, kDestroyBody))) return fail("AActor::K2_DestroyActor");
    if (!(out->destroyThunk = One(game, kDestroyThunk))) return fail("K2_DestroyActor's exec thunk (the vtable slot)");
    memcpy(&out->destroySlot, out->destroyThunk + kDestroyThunkSlotAt, 4);
    const uint8_t* const walk = One(game, kWalk);
    if (!walk) return fail("the attach's cell reference walk");
    const uint8_t* const node = One(game, kWalkNode);
    const uint8_t* const type = One(game, kWalkType);
    if (!Within(node, walk, kWalkSpan) || !Within(type, walk, kWalkSpan)) return fail("the attach's reference checks");
    out->walk = const_cast<uint8_t*>(walk);
    const uint8_t* const pos = One(game, kPairedPos);
    const uint8_t* const ids = One(game, kPairedIds);
    const uint8_t* const scale = One(game, kPairedScale);
    if (!pos || !Within(ids, pos, kPairedSpan) || !Within(scale, ids, kPairedSpan)) return fail("Spawn_Paired_Actor's reads");
    out->pairedScale = const_cast<uint8_t*>(scale);
    // UWorld::SpawnActor's two broadcasts (held back inside a build)
    const Section fn{out->spawnFn, std::min(out->spawnFn + kSpawnFnSpan, game.text.end)};
    const auto pre = FindAll(fn, ParsePattern(kPreSpawnBroadcast));
    const auto post = FindAll(fn, ParsePattern(kSpawnedBroadcast));
    if (pre.size() != 1 || post.size() != 1 || pre[0] > post[0]) return fail("UWorld::SpawnActor's two broadcasts");
    out->preSpawnCall = pre[0] + kPreSpawnCallAt;
    out->spawnedCall = post[0] + kSpawnedCallAt;
    memcpy(&out->preSpawnDelegate, pre[0] + 4, 4);
    memcpy(&out->spawnedDelegate, post[0] + 4, 4);
    out->broadcastFn = RipTarget(out->spawnedCall, 1, 5);
    if (!game.text.Contains(out->broadcastFn) || RipTarget(out->preSpawnCall, 1, 5) != out->broadcastFn)
        return fail("the broadcasts' target");
    // AActor::PostSpawnInitialize's CreationTime store: once in the exe, inside a function SpawnActor calls between the broadcasts
    const uint8_t* const ct = One(game, kCreationTime);
    bool ctCalled = false;
    for (const uint8_t* at = out->preSpawnCall + 5; ct && at + 5 <= out->spawnedCall && !ctCalled; ++at)
        if (*at == 0xE8) {
            const uint8_t* const t = RipTarget(const_cast<uint8_t*>(at), 1, 5);
            ctCalled = Within(ct, t, kPostSpawnHeadSpan);
        }
    if (!ctCalled) return fail("AActor::PostSpawnInitialize's CreationTime store");
    memcpy(&out->worldTimeOff, ct + kCreationWorldAt, 4);
    memcpy(&out->creationTimeOff, ct + kCreationActorAt, 4);
    // UWorld::DestroyActor (K2_DestroyActor's call): its two broadcasts, nothing else calling Broadcast that way in its span
    const uint8_t* const dfn = RipTarget(out->destroyFn + sizeof(kDestroyBody) / 3 - 1, 1, 5);
    if (!game.text.Contains(dfn)) return fail("UWorld::DestroyActor");
    uint8_t* hit[3] = {};
    uint32_t disp[3] = {};
    int nHit = 0;
    for (const uint8_t* at = dfn + 10; at + 5 <= std::min<const uint8_t*>(dfn + kDestroyFnSpan, game.text.end); ++at) {
        if (*at != 0xE8 || RipTarget(const_cast<uint8_t*>(at), 1, 5) != out->broadcastFn) continue;
        const uint8_t* const l = at - 10;  // lea rcx,[reg+disp32] (7 bytes, no SIB); mov rdx,reg (3 bytes)
        if ((l[0] == 0x48 || l[0] == 0x49) && l[1] == 0x8D && (l[2] & 0xF8) == 0x88 && (l[2] & 7) != 4 && (l[7] & 0xF0) == 0x40 &&
            l[8] == 0x8B && (l[9] & 0xF8) == 0xD0) {
            if (nHit < 3) {
                hit[nHit] = const_cast<uint8_t*>(at);
                memcpy(&disp[nHit], l + 3, 4);
            }
            ++nHit;
        }
    }
    if (nHit != 2 || disp[0] != out->spawnedDelegate + kDestroyedAfterSpawned || disp[1] != out->spawnedDelegate + kRemovedAfterSpawned)
        return fail("UWorld::DestroyActor's two broadcasts");
    out->destroyedCall = hit[0];
    out->removedCall = hit[1];
    out->destroyedDelegate = disp[0];
    out->removedDelegate = disp[1];
    // the place the pawn index files a pawn by (aheadindex): its root (as FinishSpawning's head has it) and the root's
    // ComponentToWorld translation, three doubles
    uint8_t* const box = One(game, kPawnBox);
    if (!box) return fail("the pawn index's box of a pawn (where it files a character)");
    const uint32_t rootOff = *reinterpret_cast<const uint32_t*>(box + kPawnBoxRootAt);
    const uint32_t locOff = *reinterpret_cast<const uint32_t*>(box + kPawnBoxLocAt);
    if (rootOff != kRootComponent || locOff < 0x100 || locOff > 0x800 || (locOff & 7) ||
        *reinterpret_cast<const uint32_t*>(box + kPawnBoxLoc2At) != locOff + 0x10)
        return fail("the pawn index's read of a pawn's place (root component, ComponentToWorld translation)");
    out->pawnBox = box;
    out->rootOff = rootOff;
    out->locOff = locOff;
    return true;
}

// streaming/row_preload.cpp, old-engine thread: a cell loaded ahead (or found buffered) for the next grid shift.
void CreateAheadCell(const uint8_t* cell)
{
    if (!g.installed.load(std::memory_order_relaxed) || !g.mode.load(std::memory_order_relaxed) || !cell) return;
    Seen found[64];
    int nRuntime = 0;
    const int n = WalkCell(cell, found, g.levelled.load(std::memory_order_relaxed) != 0, &nRuntime);
    if (nRuntime) g.runtimeNoted.fetch_add(static_cast<uint64_t>(nRuntime), std::memory_order_relaxed);
    g.cells.fetch_add(1, std::memory_order_relaxed);
    if (!n) return;
    g.actorRefs.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g.mu);
    for (int i = 0; i < n; ++i) {
        if (g.nq >= kMaxQueue) {
            g.qFull.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        g.q[g.nq++] = found[i];
    }
}

// npcs/npc_pacing.cpp's CreateExecHook, game thread: around the game's create command.
void CreateAheadCreateBegin(uint32_t ref, uint32_t base)
{
    if (!g.installed.load(std::memory_order_relaxed)) return;
    g.inCreate = true;
    g.curUsed = false;
    g.curAdopted = false;
    g.curRef = ref;
    g.curBase = base;
}
bool CreateAheadCreateEnd()
{
    if (!g.installed.load(std::memory_order_relaxed)) return false;
    g.inCreate = false;
    return g.curAdopted;
}

// Game thread: every held actor goes (a save starts, a level change, the key off). why: 0 save, 1 travel / level change.
void CreateAheadDropAll(int why)
{
    if (!g.installed.load(std::memory_order_relaxed) || !OnGt()) return;
    if (!g.held && !g.nq) return;
    ReleaseAll(why == 0 ? "a save starts" : "level change", why == 0 ? &g.dropSave : &g.dropTravel);
}

// npcs/npc_pacing.cpp's frame-end hook, game thread, after the cap's wait. sleepUs: that wait; busy: NpcCreationPacing
// holds a unit or deferred a command in the frame that just ended; travel: a fast travel is pending.
void CreateAheadFrame(uint64_t sleepUs, bool busy, bool travel)
{
    if (!g.installed.load(std::memory_order_relaxed) || !OnGt()) return;
    const int mode = g.mode.load(std::memory_order_relaxed);
    if (!mode) {
        if (g.held || g.nq) ReleaseAll("switched off", &g.dropOff);
        return;
    }
    // a menu or loading screen, a pending fast travel: nothing is held across it
    int32_t named = 0, fmode = 0;
    bool playing;
    if (g.test) playing = !(g.fakes.freeze && *g.fakes.freeze);
    else playing = ReadMenuFreeze(&named, &fmode) && named == 0 && fmode == 0;
    if (!playing || travel) {
        ReleaseAll(travel ? "fast travel" : "menu / loading", travel ? &g.dropTravel : &g.dropFreeze);
        for (Entry& en : g.e) en.state = kFree;
        g.world = {};  // taken again from the next stock create
        g.haveTmpl = false;
        return;
    }
    const double now = NowS();
    // new references from the old-engine thread
    Seen in[kMaxQueue];
    int n = 0;
    {
        std::lock_guard<std::mutex> lock(g.mu);
        n = g.nq;
        if (n) memcpy(in, g.q, sizeof(Seen) * static_cast<size_t>(n));
        g.nq = 0;
    }
    const bool levelledOn = g.levelled.load(std::memory_order_relaxed) != 0;
    for (int i = 0; i < n; ++i) {
        if (!levelledOn && in[i].ref >= 0xFF000000u) continue;  // CreateAheadLevelled switched off live: noted before, not taken now
        if (Find(in[i].ref) || RecentlyDropped(in[i].ref, now)) continue;
        Entry* const slot = FreeSlot();
        if (!slot) break;
        *slot = {};
        slot->state = kNew;
        slot->ref = in[i].ref;
        slot->base = in[i].base;
        memcpy(slot->pos, in[i].pos, sizeof slot->pos);
        slot->scale = static_cast<double>(in[i].scale);  // the create command's own scale; 0 = not known
        slot->tSeen = now;
        g.queued.fetch_add(1, std::memory_order_relaxed);
    }
    // timeouts, actors the engine took away, the packages and classes. A held actor's timeout destroy (0.1-0.3 ms) is paced
    // like a build: only after a frame with slack while pacing holds nothing, one per frame, and that frame builds nothing.
    const bool slack = sleepUs >= static_cast<uint64_t>(g.slackUs.load(std::memory_order_relaxed));
    bool destroyed = false;
    int asked = 0;
    for (Entry& en : g.e) {
        if (en.state == kFree) continue;
        if (en.state == kHeld && !HeldLive(en)) {
            Release(en, "gone", nullptr);  // forgotten, nothing destroyed
            continue;
        }
        const bool mayDestroy = en.state != kHeld || (slack && !busy && !destroyed);
        if (!levelledOn && !en.spare && en.ref >= 0xFF000000u) {
            // CreateAheadLevelled switched off live (review rev-levelled S1): a run-time reference's entry is not built and not
            // adopted any more; a built one is destroyed like a timeout (paced), counted under "key off"
            if (mayDestroy) {
                destroyed = destroyed || en.state == kHeld;
                Release(en, "CreateAheadLevelled switched off", &g.dropOff);
            }
            continue;
        }
        if (en.spare) {
            bool keep = false;
            for (int k = 0; k < g.nSb; ++k)
                if (g.sb[k].base == en.base && now - g.sb[k].lastUse <= kSpareForgetS) keep = true;
            if (!keep && mayDestroy) {
                destroyed = destroyed || en.state == kHeld;
                Release(en, "its base form was not created for a while", &g.dropTimeout);
            }
            continue;
        }
        if (now - en.tSeen > kHoldS) {
            if (mayDestroy) {
                destroyed = destroyed || en.state == kHeld;
                NoteDropped(en.ref);
                if (en.ref >= 0xFF000000u) {  // a run-time reference noted ahead whose create never came (review rev-levelled S3)
                    g.runtimeNever.fetch_add(1, std::memory_order_relaxed);
                    if (en.state == kHeld) g.runtimeNeverBuilt.fetch_add(1, std::memory_order_relaxed);
                }
                Release(en, "no create came", &g.dropTimeout);
            }
            continue;
        }
        if (en.state == kNew && asked < 8) {
            ++asked;
            if (uint8_t* const cls = ClassOf(en.base, true)) {
                if (IsOffsetClass(cls)) {  // its actors' roots have an offset of their own: not built ahead (SF2)
                    en.state = kFree;
                    NoteDropped(en.ref);
                    g.offsetSkipped.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                en.cls = cls;
                en.state = kReady;
            }
        }
    }
    if (!g.haveTmpl || !g.world.obj) return;
    if (!Live(g.world)) {
        ReleaseAll("the world is gone", &g.dropWorld);
        g.world = {};
        g.haveTmpl = false;
        return;
    }
    if (destroyed) return;  // this frame's one piece of work was a destroy
    // one SpawnActor half per frame: only after a frame with slack, and while pacing holds nothing
    Entry* pick = nullptr;
    for (Entry& en : g.e)
        if (en.state == kReady && (!pick || en.tSeen < pick->tSeen)) pick = &en;
    if (pick && (!slack || busy)) {
        (busy ? g.busy : g.noSlack).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!slack || busy) return;
    // nothing built (and nothing destroyed to make room) while a garbage collection's purge is pending (crash1118)
    if (PurgePending()) {
        g.purgeWait.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int poolKey = g.pool.load(std::memory_order_relaxed);
    const int maxHeld = poolKey ? poolKey : kMaxHeld;
    const auto oldestSpare = [] {
        Entry* oldest = nullptr;
        for (Entry& en : g.e)
            if (en.state == kHeld && en.spare && (!oldest || en.tBuilt < oldest->tBuilt)) oldest = &en;
        return oldest;
    };
    if (g.held > maxHeld) {  // CreateAheadPool lowered live: the oldest spare goes, one per slack frame
        if (Entry* const oldest = oldestSpare()) Release(*oldest, "the pool is smaller now", &g.dropCap);
        return;
    }
    if (pick) {
        if (IsOffsetClass(pick->cls)) {  // noted ready before its class was found to have an offset (SF2)
            pick->state = kFree;
            NoteDropped(pick->ref);
            g.offsetSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (pick->scale == 0.0 && !LearnedScale(pick->base, &pick->scale)) pick->scale = 0.0;
        // no scale known for it, or noted too long ago: not built (its create, if it comes, runs as stock)
        if (!ScaleBuildable(pick->scale) || now - pick->tSeen > kBuildWindowS) {
            pick->state = kFree;
            NoteDropped(pick->ref);
            return;
        }
        if (g.held >= maxHeld) {  // room: the oldest spare goes; none = this reference waits
            Entry* const oldest = oldestSpare();
            if (!oldest) return;
            Release(*oldest, "room for a reference", &g.dropCap);
            return;  // the build comes in a later frame (one piece of work per frame)
        }
    } else {
        // a spare: a base form in use (most recently used first) with fewer than kSparesPerBase spares, its class resident
        int best = -1;
        for (int k = 0; k < g.nSb; ++k) {
            if (now - g.sb[k].lastUse > kSpareForgetS || !ScaleBuildable(g.sb[k].scale)) continue;
            int have = 0;
            for (const Entry& en : g.e)
                if (en.state == kHeld && en.spare && en.base == g.sb[k].base) ++have;
            if (have < kSparesPerBase && (best < 0 || g.sb[k].lastUse > g.sb[best].lastUse)) best = k;
        }
        if (best < 0) return;
        const bool full = g.held >= maxHeld - kRoomForRefs;
        if (full && !poolKey) return;  // without the key: nothing (as before)
        // the spare for `best` must be buildable now before anything is destroyed for it (class resident, a free entry, the
        // player's position; review rev-pool SF3)
        double px = 0, py = 0, pz = 0;
        bool havePos;
        if (g.test) {
            havePos = g.fakes.player != nullptr;
            if (havePos) px = g.fakes.player[0], py = g.fakes.player[1], pz = g.fakes.player[2];
        } else {
            havePos = NpcPacingPlayerPos(&px, &py, &pz);
        }
        uint8_t* const cls = ClassOf(g.sb[best].base, false);
        Entry* const slot = FreeSlot();
        if (!cls || !slot || !havePos) return;
        if (full) {
            // the pool is full. CreateAheadPool: for a base form created in the last kSpareIdleS (review rev-pool SF2: the
            // one that wants room and the ones that give it are then never the same kind of idle), a spare of another base
            // form idle for kSpareIdleS (and idle longer than `best`) makes room, the least recently created base form
            // first; this frame's one piece of work is that destroy, the spare for `best` comes in a later slack frame.
            const double bestUse = g.sb[best].lastUse;
            if (now - bestUse >= kSpareIdleS) return;
            Entry* victim = nullptr;
            double victimUse = 0.0;
            for (Entry& en : g.e) {
                if (en.state != kHeld || !en.spare || en.base == g.sb[best].base) continue;
                double use = -1e300;  // a base form no longer in the table: idle for ever
                for (int k = 0; k < g.nSb; ++k)
                    if (g.sb[k].base == en.base) use = g.sb[k].lastUse;
                if (now - use < kSpareIdleS || use >= bestUse) continue;
                if (!victim || use < victimUse || (use == victimUse && en.tBuilt < victim->tBuilt)) {
                    victim = &en;
                    victimUse = use;
                }
            }
            if (victim) Release(*victim, "an idle spare made room for a base form in use", &g.dropIdle);
            return;
        }
        *slot = {};
        slot->state = kReady;
        slot->spare = true;
        slot->base = g.sb[best].base;
        slot->pos[0] = static_cast<float>(px / kOldToUe);
        slot->pos[1] = static_cast<float>(-py / kOldToUe);
        slot->pos[2] = static_cast<float>(pz / kOldToUe);
        slot->scale = g.sb[best].scale;
        slot->cls = cls;
        slot->tSeen = now;
        pick = slot;
    }
    if (pick) Build(pick, sleepUs, now);
}

bool InstallCreateAhead(bool beforeGameCode)
{
    if (g.installed.load()) return true;
    if (!beforeGameCode) {
        Log("[createahead] CreateAhead: start-up only (loaded late here), not installed");
        return false;
    }
    const GameModule& game = Game();
    CreateAheadSites f;
    const char* missing = nullptr;
    if (!CreateAheadDiscover(game, &f, &missing)) {
        Log("[createahead] CreateAhead: %s not recognised, not installed", missing);
        return false;
    }
    uint8_t* const objArray = FindObjectArrayFromDtor();
    if (!objArray) {
        Log("[createahead] CreateAhead: the object array not found, not installed");
        return false;
    }
    uint8_t* const purge = FindIncrementalPurgePending(game);
    if (!purge) {
        Log("[createahead] CreateAhead: the garbage collector's purge query (IsIncrementalPurgePending) not found, not installed");
        return false;
    }
    uint8_t* const stub = AllocNearGame(64);
    if (!stub) {
        Log("[createahead] CreateAhead: no memory near the game, not installed");
        return false;
    }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&SpawnCallHook);
    void* const bhook = reinterpret_cast<void*>(&BroadcastHook);
    memcpy(stub, jmpAbs, 6);
    memcpy(stub + 6, &hook, 8);
    memcpy(stub + 32, jmpAbs, 6);
    memcpy(stub + 38, &bhook, 8);
    DWORD old;
    if (!VirtualProtect(stub, 64, PAGE_EXECUTE_READ, &old)) {
        Log("[createahead] CreateAhead: could not make its call stub executable, not installed");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    g.broadcast = reinterpret_cast<BroadcastFn>(f.broadcastFn);
    g.preSpawnDelegate = f.preSpawnDelegate;
    g.spawnedDelegate = f.spawnedDelegate;
    g.destroyedDelegate = f.destroyedDelegate;
    g.removedDelegate = f.removedDelegate;
    g.creationTimeOff = f.creationTimeOff;
    g.worldTimeOff = f.worldTimeOff;
    g.rootOff = f.rootOff;
    g.locOff = f.locOff;
    // the four broadcast calls first (their hook passes everything on until a build or a destroy of ours runs), then the
    // spawner's call
    for (uint8_t* const site : {f.preSpawnCall, f.spawnedCall, f.destroyedCall, f.removedCall}) {
        const int64_t brel = (stub + 32) - (site + 5);
        uint8_t bcode[5] = {0xE8};
        const int32_t b32 = static_cast<int32_t>(brel);
        memcpy(bcode + 1, &b32, 4);
        if (brel != static_cast<int32_t>(brel) || site[0] != 0xE8 || !WriteCode(site, std::span<const uint8_t>(bcode, 5))) {
            Log("[createahead] CreateAhead: could not redirect UWorld::SpawnActor's / DestroyActor's broadcasts, not installed");
            return false;
        }
    }
    g.gameBase = game.base;
    g.textBegin = game.text.begin;
    g.textEnd = game.text.end;
    g.spawn = reinterpret_cast<SpawnFn>(f.spawnFn);
    g.destroySlot = f.destroySlot / 8;
    g.objArray = objArray;
    g.purgePending = reinterpret_cast<PurgeFn>(purge);
    const int64_t rel = stub - (f.spawnCall + 5);
    uint8_t code[5] = {0xE8};
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(code + 1, &r32, 4);
    if (rel != static_cast<int32_t>(rel) || !WriteCode(f.spawnCall, std::span<const uint8_t>(code, 5))) {
        Log("[createahead] CreateAhead: could not redirect the spawner's SpawnActor call, not installed");
        return false;
    }
    g.installed.store(true);
    Log("[createahead] CreateAhead installed (the spawner's SpawnActor call exe+0x%llX, UWorld::SpawnActor exe+0x%llX, K2_DestroyActor "
        "slot +0x%X, GC purge query exe+0x%llX): at most %d actors held (CreateAheadPool=%d%s), CreateAheadLevelled=%d, %d spares per base form, %.0f s",
        static_cast<unsigned long long>(f.spawnCall - game.base), static_cast<unsigned long long>(f.spawnFn - game.base), f.destroySlot,
        static_cast<unsigned long long>(purge - game.base), g.pool.load() ? g.pool.load() : kMaxHeld, g.pool.load(), g.pool.load() ? ": idle spares make room for base forms in use" : "",
        g.levelled.load(), kSparesPerBase, kHoldS);
    return true;
}

void SetCreateAheadPool(int pool)
{
    if (pool != 0 && pool < kPoolMin) pool = kPoolMin;
    if (pool > kPoolMax) pool = kPoolMax;
    if (pool < 0) pool = 0;
    const bool changed = g.pool.exchange(pool) != pool;
    if (changed && g.installed.load()) {
        if (pool)
            Log("[createahead] CreateAheadPool=%d: at most %d actors held, spares while fewer than %d are held; an idle spare (its base form "
                "not created for %.0f s) makes room for a base form in use",
                pool, pool, pool - kRoomForRefs, kSpareIdleS);
        else
            Log("[createahead] CreateAheadPool=0: at most %d actors held, spares while fewer than %d are held, idle spares wait for their "
                "%.0f s", kMaxHeld, kMaxHeld - kRoomForRefs, kSpareForgetS);
    }
}

void SetCreateAheadLevelled(int levelled)
{
    const int v = levelled ? 1 : 0;
    const bool changed = g.levelled.exchange(v) != v;
    if (changed && g.installed.load())
        Log("[createahead] CreateAheadLevelled=%d: %s", v,
            v ? "the game's levelled spawns already made (run-time references) in the cells loaded ahead are built ahead as references"
              : "run-time references get spares only");
}

void SetCreateAhead(int mode, int slackMs)
{
    if (slackMs < 1) slackMs = 1;
    if (slackMs > 14) slackMs = 14;
    const int m = (mode && !g.faulted.load()) ? 1 : 0;  // a fault in a build keeps it off for the session
    const bool changed = g.mode.exchange(m) != m;
    const bool slackChanged = g.slackUs.exchange(slackMs * 1000) != slackMs * 1000;
    if ((changed || slackChanged) && g.installed.load())
        Log("[createahead] characters' SpawnActor half is %s (CreateAhead %d; after a frame with at least %d ms of frame-cap sleep, "
            "CreateAheadSlackMs)",
            m ? "built ahead" : "not built ahead: every create as the game does it", m, slackMs);
}

void LogCreateAheadStats()
{
    if (!g.installed.load()) return;
    const uint64_t built = g.built.load(), adopted = g.adopted.load(), queued = g.queued.load();
    const uint64_t sum = built + adopted + queued + g.dropTimeout.load() + g.dropFreeze.load() + g.pawnCreates.load() + g.runtimeNoted.load() +
                         g.runtimeNever.load() + g.classLost.load();
    if (sum == g.statsSeen) return;
    g.statsSeen = sum;
    Log("[createahead] cells walked %llu, references noted %llu (queued %llu); built %llu (mean %.2f ms; waited: no slack %llu frames, "
        "pacing busy %llu); character creates %llu: took a built actor %llu (spares %llu), came before it was built %llu, not taken: class "
        "%llu scale %llu parameters %llu gone %llu not at its build place %llu; destroyed unused: no create came %llu, menu / loading %llu, travel %llu, save %llu, "
        "room %llu, idle spare made room %llu, world %llu, key off %llu; vanished %llu, destroy skipped %llu, spawn failed %llu, walk faults %llu, queue full %llu; "
        "broadcasts held back %llu, destroy broadcasts not made %llu; held now %d; all dropped at once: at most %.2f ms (%d actors); "
        "levelled spawns noted ahead %llu: took their actor %llu, came before it was built %llu, another base form %llu, "
        "no create came %llu (of them built %llu); slack frames that waited for a GC purge %llu, class looked up again at the build: "
        "another object %llu, gone %llu",
        static_cast<unsigned long long>(g.cells.load()), static_cast<unsigned long long>(g.actorRefs.load()),
        static_cast<unsigned long long>(queued), static_cast<unsigned long long>(built),
        built ? static_cast<double>(g.builtUs.load()) / 1000.0 / static_cast<double>(built) : 0.0,
        static_cast<unsigned long long>(g.noSlack.load()), static_cast<unsigned long long>(g.busy.load()),
        static_cast<unsigned long long>(g.pawnCreates.load()), static_cast<unsigned long long>(adopted),
        static_cast<unsigned long long>(g.spareAdopted.load()), static_cast<unsigned long long>(g.late.load()),
        static_cast<unsigned long long>(g.rejClass.load()), static_cast<unsigned long long>(g.rejScale.load()),
        static_cast<unsigned long long>(g.rejParams.load()), static_cast<unsigned long long>(g.rejGone.load()),
        static_cast<unsigned long long>(g.rejPlace.load()), static_cast<unsigned long long>(g.dropTimeout.load()), static_cast<unsigned long long>(g.dropFreeze.load()),
        static_cast<unsigned long long>(g.dropTravel.load()), static_cast<unsigned long long>(g.dropSave.load()),
        static_cast<unsigned long long>(g.dropCap.load()), static_cast<unsigned long long>(g.dropIdle.load()),
        static_cast<unsigned long long>(g.dropWorld.load()),
        static_cast<unsigned long long>(g.dropOff.load()), static_cast<unsigned long long>(g.gone.load()),
        static_cast<unsigned long long>(g.destroySkipped.load()), static_cast<unsigned long long>(g.spawnNull.load()),
        static_cast<unsigned long long>(g.walkFaults.load()), static_cast<unsigned long long>(g.qFull.load()),
        static_cast<unsigned long long>(g.heldBack.load()), static_cast<unsigned long long>(g.destroyQuiet.load()), g.held,
        g.dropAllMaxUs.load() / 1000.0, g.dropAllMaxN.load(), static_cast<unsigned long long>(g.runtimeNoted.load()),
        static_cast<unsigned long long>(g.runtimeTook.load()), static_cast<unsigned long long>(g.runtimeLate.load()),
        static_cast<unsigned long long>(g.rejBase.load()), static_cast<unsigned long long>(g.runtimeNever.load()),
        static_cast<unsigned long long>(g.runtimeNeverBuilt.load()), static_cast<unsigned long long>(g.purgeWait.load()),
        static_cast<unsigned long long>(g.classRenewed.load()), static_cast<unsigned long long>(g.classLost.load()));
}

// ---- self-test ----
void CreateAheadTestSetup(const CreateAheadFakes* f)
{
    for (Entry& en : g.e) en = {};
    g.nOffsetCls = 0;
    g.offsetSkipped.store(0);
    g.held = 0;
    g.nq = 0;
    g.world = {};
    g.haveTmpl = g.tmplRefused = false;
    g.inCreate = false;
    g.nLearned = g.nSb = 0;
    g.faulted.store(false);
    g.quietThread.store(0);
    g.creationTimeOff = g.worldTimeOff = 0;
    for (State::Dropped& d : g.dropped) d = {};
    for (std::atomic<uint64_t>* c : {&g.queued, &g.built, &g.adopted, &g.spareAdopted, &g.late, &g.rejClass, &g.rejParams, &g.rejScale,
                                     &g.rejGone, &g.rejPlace, &g.dropTimeout, &g.dropFreeze, &g.dropTravel, &g.dropSave, &g.dropOff, &g.dropCap,
                                     &g.dropWorld, &g.gone, &g.noSlack, &g.busy, &g.spawnNull, &g.destroySkipped, &g.heldBack, &g.dropIdle,
                                     &g.destroyQuiet, &g.runtimeNoted, &g.runtimeTook, &g.runtimeLate, &g.rejBase, &g.runtimeNever, &g.runtimeNeverBuilt,
                                     &g.purgeWait, &g.classRenewed, &g.classLost})
        c->store(0);
    g.pool.store(0);  // CreateAheadPool as the default (the pool test sets it itself)
    g.levelled.store(0);  // CreateAheadLevelled as the default (its test sets it itself)
    if (f) {
        g.fakes = *f;
        g.test = true;
        g.objArray = f->objArray;
        g.destroySlot = f->destroySlot;
        g.installed.store(true);
        g.mode.store(1);
        g.slackUs.store(4000);
        g.creationTimeOff = f->creationTimeOff;
        g.worldTimeOff = f->worldTimeOff;
        g.rootOff = f->rootOff;
        g.locOff = f->locOff;
    } else {
        g.fakes = {};
        g.test = false;
        g.objArray = nullptr;
        g.installed.store(false);
        g.mode.store(0);
    }
}
uint8_t* CreateAheadTestSpawnCall(uint8_t* world, uint8_t* cls, const uint8_t* xf, const uint8_t* params)
{
    return SpawnCallHook(world, cls, xf, params);
}
void CreateAheadTestBroadcast(void* delegate, uint8_t* actor) { BroadcastHook(delegate, actor); }
void CreateAheadTestFault() { g.faulted.store(true); g.mode.store(0); }
int CreateAheadTestParams(const uint8_t* tmpl, const uint8_t* p) { return ParamsUsable(tmpl) ? ParamsDiffer(tmpl, p) : -1; }
bool CreateAheadTestScale(const double* s, double built) { return ScaleAdoptable(s, built); }
CreateAheadTestCounts CreateAheadTestRead()
{
    CreateAheadTestCounts c{};
    c.held = g.held;
    c.built = static_cast<int>(g.built.load());
    c.adopted = static_cast<int>(g.adopted.load());
    c.spareAdopted = static_cast<int>(g.spareAdopted.load());
    c.rejected = static_cast<int>(g.rejClass.load() + g.rejParams.load() + g.rejScale.load() + g.rejGone.load() + g.rejPlace.load());
    c.rejScale = static_cast<int>(g.rejScale.load());
    c.rejPlace = static_cast<int>(g.rejPlace.load());
    c.offsetSkipped = static_cast<int>(g.offsetSkipped.load());
    c.dropped = static_cast<int>(g.dropTimeout.load() + g.dropFreeze.load() + g.dropTravel.load() + g.dropSave.load() + g.dropOff.load() +
                                 g.dropCap.load() + g.dropWorld.load() + g.dropIdle.load());
    c.idleEvicted = static_cast<int>(g.dropIdle.load());
    c.runtimeNoted = static_cast<int>(g.runtimeNoted.load());
    c.runtimeTook = static_cast<int>(g.runtimeTook.load());
    c.runtimeLate = static_cast<int>(g.runtimeLate.load());
    c.rejBase = static_cast<int>(g.rejBase.load());
    c.runtimeNever = static_cast<int>(g.runtimeNever.load());
    c.runtimeNeverBuilt = static_cast<int>(g.runtimeNeverBuilt.load());
    c.dropOff = static_cast<int>(g.dropOff.load());
    c.rejClass = static_cast<int>(g.rejClass.load());
    c.purgeWait = static_cast<int>(g.purgeWait.load());
    c.classRenewed = static_cast<int>(g.classRenewed.load());
    c.classLost = static_cast<int>(g.classLost.load());
    c.gone = static_cast<int>(g.gone.load());
    c.queued = static_cast<int>(g.queued.load());
    c.heldBack = static_cast<int>(g.heldBack.load());
    c.destroyQuiet = static_cast<int>(g.destroyQuiet.load());
    c.mode = g.mode.load();
    return c;
}

}  // namespace opf
