// RepathParallel (worker repathfix 2026-11-07, findings "repathfix"; public since 1.3.0, port130g; reviews rev130b, rev130d).
// A TickActor of the navigation data with >= RepathParallelMin queued repaths (ANavigationData::TickActor's RepathRequests,
// UE 5.3 NavigationData.cpp:323-357): before the stock loop runs (the game thread blocked in this detour), every distinct
// queued path's A* (dtNavMeshQuery::findPath) is computed on helper threads with private dtNavMeshQuery objects, from the
// same inputs the stock FindPath will build (stock query ctor, the filter's adjusted end, the stock InitPathfinding). Then
// the stock TickActor runs unchanged. Its findPath calls (shared game-thread query) are served from a precomputed result
// only when every input findPath reads matches byte for byte (navmesh, link filter's NavSys and owner, require-navigable-end
// bit, node budget, start/end polys and points, cost limit, filter pointer and its 0x428 bytes) and nothing in the nav data
// changed since (identical results); else the stock search runs on the game thread. Post-processing, path objects, events
// and their order stay the stock ones, at the stock time. Off-GT FindPath uses a private query in stock UE (its async path
// finding), so the helpers' search is the stock off-thread search.
//
// "Nothing changed since": a counter bumped by every navmesh edit findPath could see: tiles added / removed
// (ARecastNavMesh::InvalidateAffectedPaths), nav-link area / flag edits (dtNavMesh::updateOffMeshConnectionByUserId and
// updateOffMeshSegmentConnectionByUserId: the only writers of a poly's area / flags in this build besides tile changes;
// setPolyFlags / setPolyArea are not in the exe) (rev130b R-S1). Custom nav links (doors, ladders, jump links): findPath
// reads their state only through one call, passLinkFilter -> m_linkFilter->isLinkAllowed(userId) (DetourNavMeshQuery.cpp
// :1596, the only link-filter use inside findPath; FRecastSpeciaLinkFilter::isLinkAllowed = NavSys->GetCustomLink(id) and
// its IsLinkPathfindingAllowed(cached owner), PImplRecastNavMesh.cpp:427-431: game state outside the key). The helpers'
// link filter is the stock one with that slot wrapped: it records every link the helper's findPath asked and the answer it
// got (repath130l, user decision 2026-11-08; port130g declined every such search). At serve time, on the game thread at the
// stock findPath's entry, the stock isLinkAllowed is asked again for every recorded link through the game thread's own link
// filter (its vtable, NavSys and cached owner are in the key); the search is served only when every answer is the same.
// Why that is identical: findPath is deterministic in its keyed inputs, the navmesh (the counter above) and the answers of
// isLinkAllowed. The stock search asks the same first link the helper asked; if the answer is the same, it takes the same
// steps to its next question, and so on: by induction it asks only recorded links and gets the recorded answers, so its
// result is the helper's. A link asked twice in one helper search with different answers, or more than kMaxLinks distinct
// links, is never served. The stock search would ask at this same moment (nothing runs between our check and its start).
// Links the helper's search never asked (a door elsewhere) don't decline it. A
// query filter of another class than FRecastQueryFilter is never served (R-S2): UE's filters are virtual (isVirtual set, the
// default of FRecastQueryFilter), so findPath calls the filter's vtable slots 1 / 3 (passVirtualFilter, getVirtualCost);
// FRecastQueryFilter keeps dtQueryFilter's own, which read only the 0x428 bytes in the key, but a subclass could read more. A precomputed search the stock loop doesn't ask for (a postponed path, a search stock
// skips) is thrown away.
//
// [Fixes] RepathParallel = 0: not installed (start-up); 1 (default): on; then live 0/1. Off while ParallelHelpersAllowed is
// false (fewer usable logical processors than ParallelMinProcessors). Not in the shipped ini: RepathParallelMin (2: queued
// repaths for a batch, live), RepathParallelThreads (4: helper threads, 0..8, start-up). Lab builds only: 2 = proof (the stock
// search also runs and is compared field by field: status, visited nodes, result count, every result's poly, cost and flag).
// Start-up only install (version.dll). Every function, call target, vtable and the struct offsets used are found or checked
// by byte pattern (rev130d's table plus the pins below; no game build gate); any mismatch = not installed, with a log line.
// Helper threads run at normal priority (the game thread's): the game thread waits for every job a helper claimed, so a
// below-normal helper preempted on a busy CPU would stretch the wait past the serial time (rev130b R-S3).
//
// Layout pins (all from the game's own code): FPathFindingQuery +0x08/+0x48/+0x54 (D), +0x58 (C), +0x20/+0x38 (E, kQFilter),
// +0x60/+0x68 (kPathMove: TickActor moves the path into the query), +0x40/+0x68/+0x98 and nothing else to destroy (kQDtor:
// TickActor's query teardown), its size (a canary after our ctor call: nothing written past 0xA8); dtNavMeshQuery {nav +0,
// link filter +8 (kPassLink), require bit +0x78 (B), node pool +0x88, query nodes +0x98 (kFpNodes)} and its size 0xA0 (the
// whole ctor, kQCtor); the node pool's runtime limit +0x20 (kFpNodes); dtQueryFilter isVirtual +0x420 (kFpVirtual);
// dtQueryResult {data, size, cap} with 48-byte items (its resize, kResize); the link filter {vtable, NavSys +8, owner +0x10,
// cached owner +0x18} and its slots (kLfAllowed, kLfInit).
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace opf {
namespace {

// ---- patterns (rev130d's table; build 19115871 RVAs in the comments) ----
// A: FPImplRecastNavMesh::FindPath's body (hit = fn+0x80; fn exe+0x41CA4A0): GetWorld, GetCurrent<UNavigationSystemV1>, the link
// filter's vtable lea, INITIALIZE_NAVQUERY with IsInGameThread, init.
constexpr char kImplA[] =
    "4C 8D 68 08 4D 85 ED 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 98 48 01 00 00 48 85 DB 74 ?? E8 ?? ?? ?? ?? "
    "48 8B 53 10 4C 8D 40 30 48 63 40 38 3B 42 38 7F ?? 48 8B C8 48 8B 42 30 4C 39 04 C8 74 ?? 33 DB 48 89 5D ?? 48 8D 05 "
    "?? ?? ?? ?? 33 DB 48 89 45 ?? 48 8B D7 48 89 5D ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 8D 8D ?? ?? ?? ?? 48 89 5D ?? E8 ?? ?? "
    "?? ?? E8 ?? ?? ?? ?? 49 8D 7F 60 84 C0 75 ?? 48 8D BD ?? ?? ?? ?? 45 8B 46 20 4C 8D 4D ?? 49 8B 57 08 48 8B CF E8 ?? ?? ?? ??";
constexpr size_t kAFromFn = 0x80, kAGetWorld = 0x0D, kANavSysClass = 0x23, kALfLea = 0x4C, kAWeakAssign = 0x64, kAQCtor = 0x74, kAIsGt = 0x79,
                 kAQInit = 0x9C, kAToB = 0xA1;
// B: the same function from A+0xA1: the require bit (+0x78), InitPathfinding, the dtor on failure, findPath.
constexpr char kImplB[] =
    "0F B6 47 78 4C 8B CF 4C 8B 44 24 ?? 24 FE 0A 85 ?? ?? ?? ?? 49 8B D4 88 47 78 49 8B CF 48 8D 45 ?? 48 89 44 24 40 48 8D "
    "45 ?? 48 89 44 24 38 48 8D 45 ?? 48 89 44 24 30 48 8D 45 ?? 48 89 44 24 28 4C 89 6C 24 20 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 "
    "8D 8D ?? ?? ?? ?? BB 01 00 00 00 E8 ?? ?? ?? ?? 8B C3 E9 ?? ?? ?? ?? 4C 8B 45 ?? 48 8D 44 24 ?? 48 8B 55 ?? 4C 8D 4D ?? "
    "48 89 5C 24 40 48 8B CF 48 89 44 24 38 48 8D 45 ?? 4C 89 6C 24 30 F2 0F 11 74 24 28 48 89 44 24 20 48 89 5C 24 ?? 48 89 "
    "5C 24 ?? E8 ?? ?? ?? ??";
constexpr size_t kBInitPf = 0x46, kBQDtor = 0x5B, kBFindPath = 0xA3;
// C: ARecastNavMesh::FindPath (static; the pointer TickActor calls through navdata +0x340) at its start (exe+0x41CA0C0).
constexpr char kStaticC[] = "40 55 56 41 55 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 4C 8B F9 4D 8B E8 49 8D 48 58 E8 ?? ?? ?? ??";
// D: its impl call (exe+0x41CA3D1): query +0x08 / +0x48 / +0x54, impl +0x5C0, impl FindPath.
constexpr char kStaticD[] =
    "48 8B 9B C0 05 00 00 E8 ?? ?? ?? ?? 45 8B 45 54 49 8D 55 08 F2 41 0F 10 5D 48 48 8B CB 48 89 44 24 38 41 D1 E8 41 80 E0 01 "
    "4C 89 64 24 30 4C 89 74 24 28 44 88 44 24 20 4C 8D 45 ?? E8 ?? ?? ?? ??";
constexpr size_t kDWeakGet = 0x07, kDImplFind = 0x3C;
// E: the filter's adjusted end (exe+0x41CA2F6): FNavigationQueryFilter +0x10 (impl), query +0x20, vtable +0x78.
constexpr char kStaticE[] = "49 8B 4C 24 10 4D 8D 45 20 48 8D 55 ?? 48 8B 01 FF 50 78 F2 41 0F 10 45 08";
// kQFilter at E-0x31: mov r12,[r13+38h] (query +0x38 = QueryFilter); test; je.
constexpr char kQFilter[] = "4D 8B 65 38 4D 85 E4 0F 84";
constexpr size_t kQFilterBeforeE = 0x31;
// F: TickActor's loop (exe+0x41B90C1): Pin path +0xA8 querier, Cast<INavAgentInterface>, ShouldPostpone (vt+0x40), the requests' Add.
constexpr char kTickF[] =
    "49 8D 8E A8 00 00 00 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 48 8B CB E8 ?? ?? ?? ?? 48 85 "
    "C0 74 ?? 48 8B 10 48 8B C8 FF 52 40 84 C0 74 ?? 49 63 9F D8 03 00 00 8D 43 01 41 89 87 D8 03 00 00 41 3B 87 DC 03 00 00";
constexpr size_t kFWeakGet = 0x07, kFAgentClass = 0x18, kFGetIface = 0x23;
// G: TickActor builds the query (exe+0x41B9157): FPathFindingQuery ctor into [rbp+disp].
constexpr char kTickG[] = "4C 89 74 24 ?? 48 89 7C 24 ?? F0 FF 47 08 45 33 C0 48 8D 54 24 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? F0 FF 47 08";
constexpr size_t kGQueryDisp = 0x19, kGPqCtor = 0x1A;
// kPathMove at G+0x1F: the path's shared pointer moved into the query's PathInstanceToUpdate ({object, controller} at +0x60).
constexpr char kPathMove[] = "F0 FF 47 08 4C 89 74 24 ?? 48 89 7C 24 ?? F0 FF 47 08 0F 10 4D ??";
constexpr size_t kPathMoveAt = 0x1F, kPathMoveDisp = 0x15;
// H: TickActor's FindPath call through navdata +0x340 (exe+0x41B9218).
constexpr char kTickH[] = "4C 8D 45 ?? 48 8D 55 ?? 48 8D 4C 24 ?? 41 FF 97 40 03 00 00";
// kQDtor at H+0x118: the query's teardown after FindPath: Free(+0x98), release(+0x68), release(+0x40) (shared refs: count +8, weak
// +0xC, slot 0 DestroyObject, slot 1 delete).
constexpr char kQDtor[] =
    "48 8B 4D ?? 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 8B 5D ?? 48 85 DB 74 34 B8 FF FF FF FF F0 0F C1 43 08 83 F8 01 75 25 48 8B 03 "
    "48 8B CB FF 10 B8 FF FF FF FF F0 0F C1 43 0C 83 F8 01 75 0E 48 8B 03 BA 01 00 00 00 48 8B CB FF 50 08 48 8B 5D ?? 48 85 DB";
constexpr size_t kQDtorAt = 0x118, kQDtorFree = 3, kQDtorFreeCall = 9, kQDtorPath = 0x11, kQDtorFilter = 0x4E;
// I: ARecastNavMesh::InvalidateAffectedPaths at its start (exe+0x41D6490).
constexpr char kInvalI[] =
    "48 89 54 24 10 56 41 55 48 83 EC ?? 83 7A 08 00 4C 8B EA 48 8B F1 0F 84 ?? ?? ?? ?? 48 89 7C 24 ?? 8B B9 90 03 00 00 85 FF "
    "0F 84 ?? ?? ?? ?? 48 8B 81 C0 05 00 00 48 8B 48 08";
// J: FMemory::Free (exe+0xE26500).
constexpr char kFreeJ[] =
    "48 85 C9 74 2E 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0C E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8B "
    "D3 FF 50 48 48 83 C4 20 5B C3";
// dtNavMeshQuery's ctor (exe+0x416B770), whole: members up to +0x98, size 0xA0.
constexpr char kQCtor[] =
    "33 C0 0F 57 C0 48 89 01 48 89 41 08 48 89 81 80 00 00 00 48 89 81 88 00 00 00 48 89 81 90 00 00 00 89 81 98 00 00 00 48 8B "
    "C1 0F 11 41 10 0F 11 41 20 0F 11 41 30 0F 11 41 40 0F 11 41 50 0F 11 41 60 0F 11 41 70 C3";
constexpr size_t kQueryObjBytes = 0xA0;
// Inside dtNavMeshQuery::findPath (exe+0x4175E90), searched in its first kFpSpan bytes:
constexpr size_t kFpSpan = 0xC00;
constexpr char kFpClear[] = "41 C7 85 98 00 00 00 00 00 00 00";  // m_queryNodes = 0
constexpr char kFpNodes[] =                                       // ++m_queryNodes; the node pool +0x88; its limit +0x20
    "41 FF 85 98 00 00 00 33 DB 49 8B 85 88 00 00 00 B9 00 00 00 40 F2 45 0F 10 5E 20 48 BA 25 49 92 24 49 92 24 49 89 4C 24 78 "
    "44 8D 63 01 8B 70 20";
constexpr char kFpVirtual[] = "80 BF 20 04 00 00 00";             // filter->isVirtual at +0x420
// FRecastQueryFilter's copy (Clone, exe+0x41BF31B): the dtQueryFilter part's isVirtual copied (+8 +0x420), then the two vtables set;
// the second (at +8) is the vtable of every FRecastQueryFilter's dtQueryFilter (exe+0x79E4198).
constexpr char kRqfClone[] = "0F B6 83 28 04 00 00 41 88 81 28 04 00 00 48 8D 05 ?? ?? ?? ?? 49 89 01 48 8D 05 ?? ?? ?? ?? 49 89 41 08";
constexpr size_t kRqfCloneLea = 0x18;
constexpr char kFpResizeCall[] = "8B 53 08 FF C2 E8";              // result.resize(size + 1)
constexpr char kFpPassLinkCall[] = "4D 8B C6 49 8B D7 49 8B CD E8";  // passLinkFilter(tile, poly)
// dtChunkArray<dtQueryResultPack>::resize from +0xA: {data +0, size +8, cap +0xC}, items 48 bytes, dtAlloc / dtFree.
constexpr char kResize[] =
    "8B 41 0C 8B DA 48 8B F9 3B D0 7E ?? 48 89 74 24 30 85 C0 75 04 8B C2 EB 09 83 C0 20 3B C3 7C F9 8B D0 89 51 0C 8D 0C 40 C1 E1 "
    "04 33 D2 E8 ?? ?? ?? ?? 48 8B F0 8B 47 08 85 C0 74 18 48 85 F6 74 13 48 8B 17 44 8D 04 40 41 C1 E0 04 48 8B CE E8 ?? ?? ?? ?? "
    "48 8B 0F 33 D2 E8 ?? ?? ?? ?? 48 89 37 48 8B 74 24 30 89 5F 08";
constexpr size_t kResizeAt = 0xA;
// dtNavMeshQuery::passLinkFilter: m_linkFilter +8, isLinkAllowed = slot 2 (call [rax+10h]) with the offmesh connection's userId.
constexpr char kPassLink[] =
    "40 53 48 83 EC 20 48 8B 01 4C 8B C9 4C 8B 52 08 48 8B DA 8B 88 ?? ?? ?? ?? B8 01 00 00 00 45 0F B7 5A 1C 48 D3 E0 49 8B 49 08 "
    "FF C8 41 23 C0 44 8B C0 45 2B C3 48 85 C9 74 ?? 41 3B C3 7C ?? 41 0F B7 42 1A 44 3B C0 7D ?? 49 63 C0 48 8D 14 80 48 8B 43 48 "
    "48 03 D2 48 8B 54 D0 40 48 85 D2 74 ?? 48 8B 01 FF 50 10 84 C0";
// FRecastSpeciaLinkFilter's slots: 2 isLinkAllowed (NavSys +8, GetCustomLink, IsLinkPathfindingAllowed(cached owner +0x18)),
// 3 initialize (cached owner = owner(+0x10).Get()).
constexpr char kLfAllowed[] =
    "40 53 48 83 EC 20 48 8B D9 48 8B 49 08 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 10 48 8B C8 4C 8B 42 58 48 8B 53 18 "
    "41 FF D0";
constexpr char kLfInit[] = "40 53 48 83 EC 20 48 8B D9 48 83 C1 10 E8 ?? ?? ?? ?? 48 89 43 18";
// dtNavMesh::updateOffMeshConnectionByUserId (exe+0x4186270) and updateOffMeshSegmentConnectionByUserId (exe+0x4186340): bodies.
constexpr char kOffMesh[] =
    "45 33 F6 41 0F B7 F1 41 0F B6 E8 48 8B DA 48 8B F9 45 8B DE 44 39 B1 ?? ?? ?? ?? 0F 8E ?? ?? ?? ?? 45 8B D6";
constexpr char kOffMeshWrite[] = "32 4A 1F 80 E1 3F 66 89 72 1C 30 4A 1F";
constexpr char kOffMeshSeg[] = "41 0F B7 F1 33 C9 45 0F B6 E0 8B EA 44 8B F9 41 39 8D ?? ?? ?? ?? 0F 8E";
constexpr char kOffMeshSegWrite[] = "80 E1 C0 66 89 72 1C 41 0A CB 88 4A 1F";
// Detour entries (the copied first instructions, no RIP-relative operand):
constexpr char kTickPro[] = "48 8B C4 48 89 58 18";
constexpr char kStaticPro[] = "40 55 56 41 55 41 57";
constexpr char kInvalPro[] = "48 89 54 24 10 56 41 55";
constexpr char kFindPathPro[] = "48 89 5C 24 20 55 56 57";
constexpr char kOffMeshPro[] = "48 89 5C 24 08 48 89 6C 24 10";
constexpr char kOffMeshSegPro[] = "48 89 4C 24 08 55 56 41 54";
// Called functions' first bytes (rev130d: each reached through its stock call site above).
constexpr char kQDtorPro[] = "48 89 5C 24 08 57 48 83 EC 20";
constexpr char kQInitPro[] = "48 89 6C 24 18 57 48 83 EC 20 48 89 11";  // ... mov [rcx],rdx: m_nav at +0
constexpr char kInitPfPro[] = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57";
constexpr char kPqCtorPro[] = "48 8B C4 55 53 56 57 48 8D A8 58 FF FF FF";
constexpr char kIsGtPro[] = "48 83 EC 28 80 3D";
constexpr char kGetWorldPro[] = "40 53 48 83 EC 20 8B 41 08 48 8B D9";
constexpr char kStaticClassPro[] = "4C 8B DC 48 83 EC 78 48 8B 05";
constexpr char kGetIfacePro[] = "48 89 5C 24 08 48 89 74 24 10 57";
constexpr char kWeakGetPro[] = "44 8B 49 04 45 85 C9";

// FPathFindingQuery (UE 5.3 NavigationTypes.h / NavigationData.h; offsets pinned above)
constexpr size_t kQOwner = 0, kQStart = 0x08, kQEnd = 0x20, kQFilterObj = 0x38, kQCost = 0x48, kQFlags = 0x54, kQNavData = 0x58, kQPath = 0x60, kQPathCtrl = 0x68,
                 kQFilterCtrl = 0x40, kQName = 0x98, kQBytes = 0xA8;
constexpr size_t kQueryBuf = 0x180;  // our copy: the struct is 0xA8; the rest is a canary that must stay untouched by the ctor
constexpr uint8_t kCanary = 0x5A;
constexpr size_t kFilterBytes = 0x428;  // dtQueryFilter: vtable, area costs, fixed costs, heuristic scale, flags, isVirtual (+0x420)
constexpr size_t kFilterVirtual = 0x420;
constexpr int kMaxJobs = 32;
constexpr int kMaxLinks = 32;  // distinct custom links one served search may have asked (more: not served)

using TickFn = void (*)(uint8_t*, float, int, void*);
using InvalFn = void (*)(uint8_t*, const uint8_t*);
using OffMeshFn = void (*)(void*, uint64_t, uint8_t, uint16_t);
using WeakGetFn = void* (*)(const void*);
using DtFindFn = uint32_t (*)(uint8_t*, uint64_t, uint64_t, const double*, const double*, double, const uint8_t*, uint8_t*, double*);
using QryCtorFn = void* (*)(uint8_t*);
using QryInitFn = uint32_t (*)(uint8_t*, const void*, int, void*);
using InitPfFn = bool (*)(const uint8_t*, const double*, const double*, uint8_t*, const uint8_t*, double*, uint64_t*, double*, uint64_t*);
using PathQueryCtorFn = void* (*)(uint8_t*, void*, const void*);
using FreeFn = void (*)(void*);
using IsGtFn = bool (*)();
using GetWorldFn = uint8_t* (*)(const void*);
using StaticClassFn = uint8_t* (*)();
using AdjEndFn = double* (*)(const void*, double*, const double*);
using IfaceFn = void* (*)(const void*, const void*);
using PostponeFn = bool (*)(const void*);
using LinkAllowedFn = bool (*)(const void*, uint64_t);

struct Targets {
    uint8_t* tick, *staticFind, *inval, *dtFind, *weakGet, *qCtor, *qDtor, *qInit, *initPf, *pqCtor, *free, *isGt, *getWorld, *navSysClass,
        *agentClass, *getIface, *implFind, *offMesh, *offMeshSeg;
    const void* const* lfVtbl;
    const void* rqfVtbl;  // FRecastQueryFilter's dtQueryFilter vtable
};

TickFn g_tick = nullptr;
InvalFn g_inval = nullptr;
OffMeshFn g_offMesh = nullptr;
OffMeshFn g_offMeshSeg = nullptr;
DtFindFn g_dtFind = nullptr;  // trampoline to the stock findPath
WeakGetFn g_weakGet = nullptr;
QryCtorFn g_qCtor = nullptr;
QryInitFn g_qInit = nullptr;
InitPfFn g_initPf = nullptr;
PathQueryCtorFn g_pqCtor = nullptr;
FreeFn g_free = nullptr;
IsGtFn g_isGt = nullptr;
GetWorldFn g_getWorld = nullptr;
StaticClassFn g_navSysClass = nullptr;
StaticClassFn g_agentIfaceClass = nullptr;  // UNavAgentInterface::StaticClass
IfaceFn g_getIface = nullptr;               // UObjectBaseUtility::GetInterfaceAddress
const void* g_lfVtbl = nullptr;             // FRecastSpeciaLinkFilter's vtable
const void* g_rqfVtbl = nullptr;            // FRecastQueryFilter's dtQueryFilter vtable (R-S2)
LinkAllowedFn g_linkAllowed = nullptr;      // its slot 2
const void* g_ourVtbl[4] = {};              // the same vtable with slot 2 wrapped (the helpers' link filter)
const uint8_t* g_staticFind = nullptr;      // ARecastNavMesh::FindPath (the pointer at navdata +0x340)
Targets g_t{};
double g_qpcMs = 0;

std::atomic<int> g_mode{0};       // live: 0 off, 1 on (lab: 2 proof)
std::atomic<int> g_min{2};        // live: queued repaths needed for a batch
std::atomic<bool> g_broken{false};  // a layout check failed at run time: off until the game ends
int g_threads = 4;                // helper threads (start-up)
std::atomic<uint32_t> g_navEpoch{0};  // navmesh edits findPath could see

double QpcMsNow()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * g_qpcMs;
}

bool SafeCopy(void* out, const void* p, size_t off, size_t n)
{
    __try {
        memcpy(out, static_cast<const uint8_t*>(p) + off, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(out, 0, n);
        return false;
    }
}
// Fault-tolerant field read (a zero value on a fault): the hooks below hold C++ objects, so no __try there.
template <class T> T Rd(const void* p, size_t off)
{
    T v{};
    if (p) SafeCopy(&v, p, off, sizeof v);
    return v;
}

const void* WeakGet(const void* weak)
{
    __try {
        return g_weakGet(weak);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The custom links one search asked and the answers it got (in first-asked order).
struct LinkRec {
    int n;
    bool bad;  // more than kMaxLinks distinct links, or one link answered differently within the search: never served
    uint64_t id[kMaxLinks];
    bool allowed[kMaxLinks];
};
void LinkRecClear(LinkRec* r) { r->n = 0; r->bad = false; }
void LinkRecAdd(LinkRec* r, uint64_t id, bool allowed)
{
    for (int i = 0; i < r->n; ++i)
        if (r->id[i] == id) {
            if (r->allowed[i] != allowed) r->bad = true;
            return;
        }
    if (r->n >= kMaxLinks) { r->bad = true; return; }
    r->id[r->n] = id;
    r->allowed[r->n] = allowed;
    ++r->n;
}
// Game thread, at the stock findPath's entry: the stock isLinkAllowed asked again for every recorded link through the game
// thread's own link filter (`lf`, the query's +8; the key matched its vtable, NavSys and cached owner). True = all the same.
bool LinksSame(const LinkRec& r, const void* lf)
{
    if (r.bad) return false;
    for (int i = 0; i < r.n; ++i)
        if (g_linkAllowed(lf, r.id[i]) != r.allowed[i]) return false;
    return true;
}

// The helpers' link filter's isLinkAllowed: the stock one; the link and its answer are recorded for the search running on
// this thread (t_linkRec, set only around the helper's findPath; InitPathfinding's projections are keyed by their outputs).
thread_local LinkRec* t_linkRec = nullptr;
bool LinkAllowedHook(const void* self, uint64_t userId)
{
    const bool allowed = g_linkAllowed(self, userId);
    if (t_linkRec) LinkRecAdd(t_linkRec, userId, allowed);
    return allowed;
}

// Everything dtNavMeshQuery::findPath reads (UE 5.3 DetourNavMeshQuery.cpp 1484-1755): m_nav, the special-link
// filter (isLinkAllowed reads NavSys and the cached owner), m_query.requireNavigableEndLocation, the node pool's runtime
// limit, and its arguments. Zeroed first so the padding compares equal. A filter of another class is never keyed (R-S2).
struct Key {
    const void* nav;
    const void* lfVtbl;
    const void* navSys;
    const void* owner;
    int require;
    int maxRuntime;
    uint64_t startRef, endRef;
    double sp[3], ep[3];
    double cost;
    const void* filter;
    uint8_t filterBytes[kFilterBytes];
};

bool MakeKey(Key* k, const uint8_t* q, uint64_t s, uint64_t e, const double* sp, const double* ep, double cost, const uint8_t* filter)
{
    memset(k, 0, sizeof *k);
    k->nav = Rd<const void*>(q, 0);
    const uint8_t* const lf = Rd<const uint8_t*>(q, 8);
    const uint8_t* const pool = Rd<const uint8_t*>(q, 0x88);
    if (!lf || !pool || !filter || !sp || !ep) return false;
    k->lfVtbl = Rd<const void*>(lf, 0);
    if (k->lfVtbl == static_cast<const void*>(g_ourVtbl)) k->lfVtbl = g_lfVtbl;  // the helpers' wrapped copy is the stock filter
    k->navSys = Rd<const void*>(lf, 8);
    k->owner = Rd<const void*>(lf, 0x18);
    k->require = Rd<uint8_t>(q, 0x78) & 1;
    k->maxRuntime = Rd<int>(pool, 0x20);
    k->startRef = s;
    k->endRef = e;
    if (!SafeCopy(k->sp, sp, 0, sizeof k->sp) || !SafeCopy(k->ep, ep, 0, sizeof k->ep)) return false;
    k->cost = cost;
    k->filter = filter;
    if (!SafeCopy(k->filterBytes, filter, 0, kFilterBytes)) return false;
    const void* vt = nullptr;
    memcpy(&vt, k->filterBytes, 8);
    return vt == g_rqfVtbl;
}


#pragma warning(push)
#pragma warning(disable : 4324)  // padded for the 16-aligned query bytes
struct Job {
    // inputs (game thread, before the helpers start)
    const uint8_t* impl;
    const void* nav;
    const uint8_t* dtFilter;
    int maxNodes;
    uint8_t require;
    double start[3], adjEnd[3], cost;
    alignas(8) uint8_t linkFilter[0x20];  // FRecastSpeciaLinkFilter {vtable, NavSys, FWeakObjectPtr owner, cached owner}
    const void* pathObj;
    void* pathCtrl;
    bool queryBuilt, valid;
    alignas(16) uint8_t query[kQueryBuf];  // the stock FPathFindingQuery (holds the path and filter alive)
    // outputs (the helper)
    std::atomic<int> state;  // 0 queued, 1 done
    bool searched;
    LinkRec links;  // the custom links the helper's findPath asked, with their answers (checked again at serve time)
    uint32_t status;
    int queryNodes;
    uint8_t* data;  // dtQueryResult {m_data, m_size, m_cap} (dtAlloc'd: moved into the stock result when served)
    int size, cap;
    double ms;
    bool used;
    Key key;
};
#pragma warning(pop)
Job g_jobs[kMaxJobs];
int g_jobCount = 0;
int g_order[kMaxJobs];  // the valid jobs' indexes (helpers claim positions in it)
std::atomic<uint64_t> g_word{0};  // batch generation << 32 | next << 16 | count
uint32_t g_gen = 0;
HANDLE g_sem = nullptr;
bool g_helpersTried = false;
int g_helpers = 0;
thread_local bool t_batch = false;  // the game thread is inside a batch's stock TickActor
uint32_t g_batchEpoch = 0;

// Counters: written by the game thread (relaxed), read and reset by the monitor thread's stats line.
struct ParSum {
    // linked: helper searches that asked a custom link; linkBad: of those, not servable (too many links / a changing answer);
    // linkServed: served (lab mode 2: compared) searches that asked a link; linkChanged: declined, a link's answer changed.
    std::atomic<int> batches{0}, jobs{0}, searched{0}, served{0}, declined{0}, unused{0}, skipped{0}, epochDrops{0}, postponed{0}, linked{0},
        linkBad{0}, linkServed{0}, linkChanged{0}, proofOk{0}, proofBad{0};
    std::atomic<int64_t> wallUs{0}, jobUs{0}, maxWallUs{0};
};
ParSum g_par;
int g_mismatchLogs = 0, g_declineLogs = 0;
inline void Inc(std::atomic<int>& a, int v = 1) { a.fetch_add(v, std::memory_order_relaxed); }
inline void AddUs(std::atomic<int64_t>& a, double ms) { a.fetch_add(static_cast<int64_t>(ms * 1000.0), std::memory_order_relaxed); }
struct CurBatch { int jobs, skipped; double sum, wall; int servedBefore, declinedBefore; double maxJob; } g_cur{};

void ReleaseShared(void* ctrl)
{
    if (!ctrl) return;
    auto* const c = static_cast<uint8_t*>(ctrl);
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(c + 8)) == 0) {
        void** const vt = *reinterpret_cast<void***>(c);
        reinterpret_cast<void (*)(void*)>(vt[0])(c);  // DestroyObject
        if (_InterlockedDecrement(reinterpret_cast<volatile long*>(c + 0xC)) == 0)
            reinterpret_cast<void (*)(void*, int)>(vt[1])(c, 1);  // delete the controller
    }
}
bool PinShared(void* ctrl)  // TWeakPtr::Pin: add a shared reference unless it already reached 0
{
    auto* const p = reinterpret_cast<volatile long*>(static_cast<uint8_t*>(ctrl) + 8);
    long v = *p;
    while (v != 0) {
        const long seen = _InterlockedCompareExchange(p, v + 1, v);
        if (seen == v) return true;
        v = seen;
    }
    return false;
}
void DestroyQuery(uint8_t* q)  // what TickActor does to its query after FindPath, in its order (kQDtor)
{
    if (void* s = Rd<void*>(q, kQName)) g_free(s);
    ReleaseShared(Rd<void*>(q, kQPathCtrl));
    ReleaseShared(Rd<void*>(q, kQFilterCtrl));
}

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

// One search with a private query (helper thread or the game thread): impl FindPath's steps up to the A* (link filter,
// query init with the filter's node budget, require bit, InitPathfinding, findPath).
// The query object stays with its thread (as UE's shared game-thread query does): init clears its node pool and open
// list and sets the node budget each time, and findPath's result depends on the budget only, not on the pool's size
// (nodes are numbered in allocation order; the hash only finds them). Saves a ~0.1 MB pool allocation per search.
thread_local alignas(16) uint8_t t_query[0xC0];  // dtNavMeshQuery is kQueryObjBytes (0xA0, kQCtor)
thread_local bool t_queryMade = false;
void RunJob(Job& j)
{
    uint8_t* const q = t_query;
    if (!t_queryMade) { g_qCtor(q); t_queryMade = true; }
    alignas(8) uint8_t lf[0x20];
    memcpy(lf, j.linkFilter, sizeof lf);
    const void* const ours = g_ourVtbl;
    memcpy(lf, &ours, 8);  // the stock link filter with isLinkAllowed marking the search
    g_qInit(q, j.nav, j.maxNodes, lf);
    q[0x78] = static_cast<uint8_t>((q[0x78] & 0xFE) | j.require);
    double rs[3] = {}, re[3] = {};
    uint64_t sRef = 0, eRef = 0;
    j.searched = false;
    LinkRecClear(&j.links);
    if (g_initPf(j.impl, j.start, j.adjEnd, q, j.dtFilter, rs, &sRef, re, &eRef)) {
        uint8_t res[16] = {};
        t_linkRec = &j.links;  // findPath's isLinkAllowed questions only
        const double t0 = QpcMsNow();
        j.status = g_dtFind(q, sRef, eRef, rs, re, j.cost, j.dtFilter, res, nullptr);
        j.ms = QpcMsNow() - t0;
        t_linkRec = nullptr;
        memcpy(&j.data, res, 8);
        memcpy(&j.size, res + 8, 4);
        memcpy(&j.cap, res + 12, 4);
        j.queryNodes = *reinterpret_cast<int*>(q + 0x98);
        j.searched = !j.links.bad && MakeKey(&j.key, q, sRef, eRef, rs, re, j.cost, j.dtFilter);
        if (!j.searched && j.data) { g_free(j.data); j.data = nullptr; }
    }
    // the link filter on this stack goes now: leave no pointer to it in the kept query
    *reinterpret_cast<void**>(q + 8) = nullptr;
    j.state.store(1, std::memory_order_release);
    WakeWordAll(&j.state);
}

DWORD WINAPI ParHelper(void*)
{
    for (;;) {
        WaitForSingleObject(g_sem, INFINITE);
        int i;
        while ((i = Claim()) >= 0) RunJob(g_jobs[g_order[i]]);
    }
}

void StartHelpers()
{
    g_helpersTried = true;
    g_sem = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);
    if (!g_sem) return;
    for (int i = 0; i < g_threads; ++i) {
        HANDLE t = CreateThread(nullptr, 1 << 20, ParHelper, nullptr, 0, nullptr);
        if (!t) break;
        DescribeThread(t, L"OPF RepathParallel");
        CloseHandle(t);
        ++g_helpers;
    }
    Log("[repath] RepathParallel: %d helper threads started", g_helpers);
}

// FNavigationSystem::GetCurrent<UNavigationSystemV1>(NavMeshOwner->GetWorld()) as impl FindPath computes it (A: world +0x148,
// UClass +0x30 chain / +0x38 depth).
const void* NavSysFor(const void* navMeshOwner)
{
    uint8_t* const world = g_getWorld(navMeshOwner);
    if (!world) return nullptr;
    const uint8_t* const obj = Rd<const uint8_t*>(world, 0x148);
    if (!obj) return nullptr;
    const uint8_t* const cls = g_navSysClass();
    const uint8_t* const oc = Rd<const uint8_t*>(obj, 0x10);
    if (!cls || !oc) return nullptr;
    const int depth = Rd<int>(cls, 0x38);
    if (depth > Rd<int>(oc, 0x38)) return nullptr;
    const uint8_t* const chain = Rd<const uint8_t*>(oc, 0x30);
    return chain && Rd<const uint8_t*>(chain, static_cast<size_t>(depth) * 8) == cls + 0x30 ? obj : nullptr;
}

void FreeJobs()
{
    for (int i = 0; i < g_jobCount; ++i) {
        Job& j = g_jobs[i];
        if (j.data) { g_free(j.data); j.data = nullptr; }
    }
}

bool CanaryIntact(const uint8_t* q)
{
    for (size_t i = kQBytes; i < kQueryBuf; ++i)
        if (q[i] != kCanary) return false;
    return true;
}

// Builds the batch's jobs from RepathRequests, runs them (helpers + this thread), waits. Game thread, before the stock
// TickActor. Returns true when there is a batch to serve from.
bool Precompute(uint8_t* self)
{
    if (Rd<const uint8_t*>(self, 0x340) != g_staticFind) return false;
    const uint8_t* const rq = Rd<const uint8_t*>(self, 0x3D0);
    int n = Rd<int>(self, 0x3D8);
    if (!rq || n < g_min.load(std::memory_order_relaxed)) return false;
    if (n > 1000) n = 1000;
    if (!g_helpersTried) StartHelpers();
    const double t0 = QpcMsNow();
    g_batchEpoch = g_navEpoch.load(std::memory_order_acquire);
    g_jobCount = 0;
    const void* navSysOwner = nullptr;
    const void* navSys = nullptr;
    for (int i = 0; i < n && g_jobCount < kMaxJobs; ++i) {
        void* const obj = Rd<void*>(rq + i * 0x18, 0);
        void* const ctrl = Rd<void*>(rq + i * 0x18, 8);
        if (!obj || !ctrl) continue;
        bool dup = false;
        for (int k = 0; k < g_jobCount && !dup; ++k) dup = g_jobs[k].pathObj == obj;
        if (dup) continue;  // a path queued twice: its later searches stay on the game thread (serial)
        // TickActor's postpone check: such a path is queued again, not searched (searching it here every frame would
        // only block this thread for nothing)
        if (const void* querier = WeakGet(static_cast<uint8_t*>(obj) + 0xA8)) {
            const void* const iface = g_getIface(querier, g_agentIfaceClass());
            const void* const* const ivt = iface ? Rd<const void* const*>(iface, 0) : nullptr;
            if (ivt && reinterpret_cast<PostponeFn>(Rd<const void*>(ivt, 0x40))(iface)) { Inc(g_par.postponed); continue; }
        }
        if (!PinShared(ctrl)) continue;
        Job& j = g_jobs[g_jobCount++];
        j.pathObj = obj;
        j.pathCtrl = ctrl;
        j.valid = false;
        j.searched = false;
        LinkRecClear(&j.links);
        j.used = false;
        j.data = nullptr;
        j.size = j.cap = 0;
        j.ms = 0;
        j.state.store(0, std::memory_order_relaxed);
        // the stock query, as TickActor builds it (the by-value shared ref is released by the ctor)
        _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<uint8_t*>(ctrl) + 8));
        void* ref[2] = {obj, ctrl};
        memset(j.query, 0, kQBytes);
        memset(j.query + kQBytes, kCanary, kQueryBuf - kQBytes);
        g_pqCtor(j.query, ref, nullptr);
        j.queryBuilt = true;
        if (!CanaryIntact(j.query)) {  // FPathFindingQuery grew: none of the offsets can be trusted
            g_broken = true;
            Log("[repath] RepathParallel: the path query is larger than this version knows: off until the game ends");
            break;
        }
        const uint8_t* const q = j.query;
        // ARecastNavMesh::FindPath (static) and impl FindPath's inputs
        const uint8_t* const navData = static_cast<const uint8_t*>(WeakGet(q + kQNavData));
        const uint8_t* const impl = navData ? Rd<const uint8_t*>(navData, 0x5C0) : nullptr;
        const void* const nav = impl ? Rd<const void*>(impl, 8) : nullptr;
        const void* const owner = impl ? Rd<const void*>(impl, 0) : nullptr;
        const uint8_t* const navFilter = Rd<const uint8_t*>(q, kQFilterObj);
        const uint8_t* const fimpl = navFilter ? Rd<const uint8_t*>(navFilter, 0x10) : nullptr;
        if (!Rd<const void*>(q, kQPath) || !nav || !owner || !fimpl) continue;
        const void* const* const fvt = Rd<const void* const*>(fimpl, 0);
        const auto adj = fvt ? reinterpret_cast<AdjEndFn>(Rd<const void*>(fvt, 0x78)) : nullptr;
        if (!adj) continue;
        double end[3];
        SafeCopy(j.start, q, kQStart, sizeof j.start);
        SafeCopy(end, q, kQEnd, sizeof end);
        adj(fimpl, j.adjEnd, end);
        if (std::fabs(j.start[0] - j.adjEnd[0]) <= 1e-4 && std::fabs(j.start[1] - j.adjEnd[1]) <= 1e-4 &&
            std::fabs(j.start[2] - j.adjEnd[2]) <= 1e-4)
            continue;  // stock: no search
        if (owner != navSysOwner) { navSysOwner = owner; navSys = NavSysFor(owner); }
        j.impl = impl;
        j.nav = nav;
        j.dtFilter = fimpl + 8;
        if (Rd<const void*>(j.dtFilter, 0) != g_rqfVtbl) continue;  // a filter of another class: never served (R-S2), so not searched
        j.maxNodes = Rd<int>(navFilter, 0x20);
        j.require = static_cast<uint8_t>((Rd<uint32_t>(q, kQFlags) >> 1) & 1);
        j.cost = Rd<double>(q, kQCost);
        memset(j.linkFilter, 0, sizeof j.linkFilter);
        memcpy(j.linkFilter, &g_lfVtbl, 8);
        memcpy(j.linkFilter + 8, &navSys, 8);
        if (WeakGet(q + kQOwner)) SafeCopy(j.linkFilter + 0x10, q, kQOwner, 8);  // FWeakObjectPtr(Owner.Get()) = the query's own
        j.valid = true;
    }
    // the valid jobs, in queue order, are what the helpers claim (by index into g_order)
    int nv = 0;
    if (!g_broken.load(std::memory_order_relaxed))
        for (int i = 0; i < g_jobCount; ++i)
            if (g_jobs[i].valid) g_order[nv++] = i;
    bool any = false;
    if (nv > 0) {
        ++g_gen;
        g_word.store((static_cast<uint64_t>(g_gen) << 32) | static_cast<uint64_t>(nv), std::memory_order_release);
        if (g_sem && g_helpers > 0 && nv > 1) ReleaseSemaphore(g_sem, std::min(g_helpers, nv - 1), nullptr);
        int i;
        while ((i = Claim()) >= 0) RunJob(g_jobs[g_order[i]]);
        for (int k = 0; k < nv; ++k) {
            Job& j = g_jobs[g_order[k]];
            int spin = 0;
            uint64_t deadline = 0;
            while (j.state.load(std::memory_order_acquire) == 0) {
                if (SpinPhase(spin, deadline)) continue;
                const int seen = 0;
                BlockOnWord(&j.state, &seen, sizeof seen);
            }
            any |= j.searched;
            if (j.links.n > 0 || j.links.bad) Inc(g_par.linked);
            if (j.links.bad) Inc(g_par.linkBad);
        }
        g_word.store(static_cast<uint64_t>(g_gen) << 32, std::memory_order_release);
    }
    // queries and pins go now (on the game thread, stock order of releases per query); the stock loop builds its own
    double jobSum = 0;
    int searched = 0;
    for (int k = 0; k < g_jobCount; ++k) {
        Job& j = g_jobs[k];
        if (j.queryBuilt) DestroyQuery(j.query);
        j.queryBuilt = false;
        ReleaseShared(j.pathCtrl);
        j.pathCtrl = nullptr;
        if (j.valid && j.searched) { ++searched; jobSum += j.ms; }
    }
    const double wall = QpcMsNow() - t0;
    Inc(g_par.batches);
    Inc(g_par.jobs, g_jobCount);
    Inc(g_par.skipped, g_jobCount - nv);
    Inc(g_par.searched, searched);
    AddUs(g_par.wallUs, wall);
    AddUs(g_par.jobUs, jobSum);
    const int64_t wallUs = static_cast<int64_t>(wall * 1000.0);
    if (wallUs > g_par.maxWallUs.load(std::memory_order_relaxed)) g_par.maxWallUs.store(wallUs, std::memory_order_relaxed);
    g_cur = {nv, g_jobCount - nv, jobSum, wall, g_par.served.load(std::memory_order_relaxed), g_par.declined.load(std::memory_order_relaxed), 0};
    for (int k = 0; k < nv; ++k) g_cur.maxJob = std::max(g_cur.maxJob, g_jobs[g_order[k]].ms);
    if (!any) { FreeJobs(); g_jobCount = 0; }
    return any;
}


// The stock loop's findPath calls (game thread, inside a batch's TickActor)
uint32_t DtFindHook(uint8_t* q, uint64_t s, uint64_t e, const double* sp, const double* ep, double cost, const uint8_t* filter,
                    uint8_t* result, double* total)
{
    if (!t_batch) return g_dtFind(q, s, e, sp, ep, cost, filter, result, total);
    const int mode = g_mode.load(std::memory_order_relaxed);
    Key k;
    const bool keyed = mode > 0 && !total && result && !Rd<void*>(result, 0) && !Rd<int>(result, 8) && !Rd<int>(result, 12) &&
                       MakeKey(&k, q, s, e, sp, ep, cost, filter);
    Job* hit = nullptr;
    if (keyed) {
        if (g_navEpoch.load(std::memory_order_acquire) != g_batchEpoch) {
            Inc(g_par.epochDrops);
        } else {
            for (int i = 0; i < g_jobCount && !hit; ++i) {
                Job& j = g_jobs[i];
                if (j.valid && j.searched && !j.used && !memcmp(&j.key, &k, sizeof k)) hit = &j;
            }
        }
    }
    if (!hit) {
        Inc(g_par.declined);
        return g_dtFind(q, s, e, sp, ep, cost, filter, result, total);
    }
    hit->used = true;
    if (hit->links.n > 0) {
        // the custom links the helper's search asked: the stock search would ask them now (repath130l)
        if (!LinksSame(hit->links, Rd<const void*>(q, 8))) {
            Inc(g_par.linkChanged);
            Inc(g_par.declined);
            return g_dtFind(q, s, e, sp, ep, cost, filter, result, total);
        }
        Inc(g_par.linkServed);
    }
    // serve: the precomputed result's buffer becomes the stock result (it was empty: {0, 0, 0})
    memcpy(result, &hit->data, 8);
    memcpy(result + 8, &hit->size, 4);
    memcpy(result + 12, &hit->cap, 4);
    hit->data = nullptr;
    *reinterpret_cast<int*>(q + 0x98) = hit->queryNodes;  // m_queryNodes, as the stock search leaves it
    Inc(g_par.served);
    return hit->status;
}

void FinishBatch()
{
    int unused = 0;
    for (int i = 0; i < g_jobCount; ++i)
        if (g_jobs[i].valid && g_jobs[i].searched && !g_jobs[i].used) ++unused;
    Inc(g_par.unused, unused);
    FreeJobs();
    g_jobCount = 0;
}

void TickHook(uint8_t* self, float dt, int type, void* tf)
{
    const double t0 = QpcMsNow();
    const bool batch = g_mode.load(std::memory_order_relaxed) > 0 && !g_broken.load(std::memory_order_relaxed) &&
                       Rd<int>(self, 0x3D8) >= g_min.load(std::memory_order_relaxed) && !t_batch && g_isGt() && Precompute(self);
    t_batch = batch;
    g_tick(self, dt, type, tf);
    t_batch = false;
    const double ms = QpcMsNow() - t0;
    if (batch) {
        FinishBatch();
    }
    (void)ms;
}

void InvalHook(uint8_t* self, const uint8_t* tiles)
{
    g_navEpoch.fetch_add(1, std::memory_order_acq_rel);
    g_inval(self, tiles);
}
void OffMeshHook(void* nav, uint64_t userId, uint8_t area, uint16_t flags)
{
    g_navEpoch.fetch_add(1, std::memory_order_acq_rel);
    g_offMesh(nav, userId, area, flags);
}
void OffMeshSegHook(void* nav, uint64_t userId, uint8_t area, uint16_t flags)
{
    g_navEpoch.fetch_add(1, std::memory_order_acq_rel);
    g_offMeshSeg(nav, userId, area, flags);
}

// ---- discovery ----
uint8_t* One(const GameModule& game, const char* pattern)
{
    const auto hits = FindAll(game.text, ParsePattern(pattern));
    return hits.size() == 1 ? hits[0] : nullptr;
}
uint8_t* CallAt(const GameModule& game, uint8_t* at)  // the target of an E8 call at `at` (in .text), else null
{
    if (!at || !game.text.Contains(at) || at[0] != 0xE8) return nullptr;
    uint8_t* const t = RipTarget(at, 1, 5);
    return game.text.Contains(t) ? t : nullptr;
}
// The one match of a pattern in [from, from + span) (also inside .text), else null.
uint8_t* OneIn(const GameModule& game, uint8_t* from, size_t span, const char* pattern, int* count = nullptr)
{
    if (!from || !game.text.Contains(from)) return nullptr;
    uint8_t* const end = game.text.Contains(from + span) ? from + span : game.text.end;
    const auto hits = FindAll(Section{from, end}, ParsePattern(pattern));
    if (count) *count = static_cast<int>(hits.size());
    return hits.size() == 1 ? hits[0] : nullptr;
}
bool At(const uint8_t* p, const char* pattern) { return p && MatchesAt(p, ParsePattern(pattern)); }
int8_t Disp8(const uint8_t* p) { return static_cast<int8_t>(*p); }

bool Discover(const GameModule& game, Targets* t, const char** why)
{
    memset(t, 0, sizeof *t);
    const auto Fail = [why](const char* w) { *why = w; return false; };
    uint8_t* const a = One(game, kImplA);
    if (!a) return Fail("FPImplRecastNavMesh::FindPath (A) not found once");
    uint8_t* const b = a + kAToB;
    if (!At(b, kImplB)) return Fail("FPImplRecastNavMesh::FindPath's search part (B) does not match");
    t->implFind = PrimaryFunction(game, a);
    if (!t->implFind || t->implFind + kAFromFn != a) return Fail("FPImplRecastNavMesh::FindPath's start");
    uint8_t* const c = One(game, kStaticC);
    uint8_t* const d = One(game, kStaticD);
    uint8_t* const e = One(game, kStaticE);
    if (!c || !d || !e || PrimaryFunction(game, d) != c || PrimaryFunction(game, e) != c) return Fail("ARecastNavMesh::FindPath (C/D/E) not found once");
    if (!At(e - kQFilterBeforeE, kQFilter)) return Fail("the query's filter read (+0x38) does not match");
    t->staticFind = c;
    uint8_t* const f = One(game, kTickF);
    uint8_t* const g = One(game, kTickG);
    uint8_t* const h = One(game, kTickH);
    if (!f || !g || !h) return Fail("ANavigationData::TickActor's loop (F/G/H) not found once");
    t->tick = PrimaryFunction(game, f);
    if (!t->tick || PrimaryFunction(game, g) != t->tick || PrimaryFunction(game, h) != t->tick) return Fail("TickActor's parts are not in one function");
    // The query's frame slot: G's lea rcx,[rbp+q], H's lea r8,[rbp+q]; its fields at the path move and the teardown.
    const int qd = Disp8(g + kGQueryDisp);
    if (Disp8(h + 3) != qd) return Fail("TickActor's query is not the one FindPath gets");
    if (!At(g + kPathMoveAt, kPathMove) || Disp8(g + kPathMoveAt + kPathMoveDisp) - qd != static_cast<int>(kQPath)) return Fail("the query's path (+0x60) does not match");
    uint8_t* const td = h + kQDtorAt;
    if (!At(td, kQDtor) || Disp8(td + kQDtorFree) - qd != static_cast<int>(kQName) || Disp8(td + kQDtorPath) - qd != static_cast<int>(kQPathCtrl) ||
        Disp8(td + kQDtorFilter) - qd != static_cast<int>(kQFilterCtrl))
        return Fail("the query's teardown (+0x98 / +0x68 / +0x40) does not match");
    t->inval = One(game, kInvalI);
    t->free = One(game, kFreeJ);
    if (!t->inval || !t->free) return Fail("InvalidateAffectedPaths or FMemory::Free not found once");
    if (CallAt(game, td + kQDtorFreeCall) != t->free) return Fail("the query's free is not FMemory::Free");
    t->dtFind = CallAt(game, b + kBFindPath);
    t->weakGet = CallAt(game, f + kFWeakGet);
    t->qCtor = CallAt(game, a + kAQCtor);
    t->qDtor = CallAt(game, b + kBQDtor);
    t->qInit = CallAt(game, a + kAQInit);
    t->initPf = CallAt(game, b + kBInitPf);
    t->pqCtor = CallAt(game, g + kGPqCtor);
    t->isGt = CallAt(game, a + kAIsGt);
    t->getWorld = CallAt(game, a + kAGetWorld);
    t->navSysClass = CallAt(game, a + kANavSysClass);
    t->agentClass = CallAt(game, f + kFAgentClass);
    t->getIface = CallAt(game, f + kFGetIface);
    if (!t->dtFind || !t->weakGet || !t->qCtor || !t->qDtor || !t->qInit || !t->initPf || !t->pqCtor || !t->isGt || !t->getWorld || !t->navSysClass ||
        !t->agentClass || !t->getIface || !CallAt(game, a + kAWeakAssign))
        return Fail("a call target is missing");
    if (CallAt(game, d + kDWeakGet) != t->weakGet || CallAt(game, d + kDImplFind) != t->implFind) return Fail("the static FindPath's calls do not match");
    if (!At(t->dtFind, kFindPathPro) || !At(t->weakGet, kWeakGetPro) || !At(t->qCtor, kQCtor) || !At(t->qDtor, kQDtorPro) || !At(t->qInit, kQInitPro) ||
        !At(t->initPf, kInitPfPro) || !At(t->pqCtor, kPqCtorPro) || !At(t->isGt, kIsGtPro) || !At(t->getWorld, kGetWorldPro) ||
        !At(t->navSysClass, kStaticClassPro) || !At(t->agentClass, kStaticClassPro) || !At(t->getIface, kGetIfacePro) || !At(t->tick, kTickPro) ||
        !At(t->staticFind, kStaticPro) || !At(t->inval, kInvalPro))
        return Fail("a called function's first bytes do not match");
    // findPath's own reads: m_queryNodes, the node pool and its limit, the filter's isVirtual, the result array, the link filter.
    int virt = 0;
    OneIn(game, t->dtFind, kFpSpan, kFpVirtual, &virt);
    uint8_t* const rz = OneIn(game, t->dtFind, kFpSpan, kFpResizeCall);
    uint8_t* const pl = OneIn(game, t->dtFind, kFpSpan, kFpPassLinkCall);
    if (!OneIn(game, t->dtFind, kFpSpan, kFpClear) || !OneIn(game, t->dtFind, kFpSpan, kFpNodes) || virt < 1 || !rz || !pl)
        return Fail("dtNavMeshQuery::findPath's reads do not match");
    uint8_t* const resize = CallAt(game, rz + 5);
    uint8_t* const passLink = CallAt(game, pl + 9);
    if (!resize || !At(resize + kResizeAt, kResize) || !At(passLink, kPassLink)) return Fail("findPath's result array or link filter call does not match");
    // The link filter's vtable (A's lea) and its slots 2 and 3.
    const uint8_t* const lea = a + kALfLea;
    if (lea[0] != 0x48 || lea[1] != 0x8D || lea[2] != 0x05) return Fail("the link filter's vtable lea does not match");
    const auto* const vt = reinterpret_cast<const void* const*>(RipTarget(lea, 3, 7));
    if (!game.rdata.Contains(vt) || !game.rdata.Contains(vt + 4)) return Fail("the link filter's vtable is not in .rdata");
    const uint8_t* const s2 = static_cast<const uint8_t*>(vt[2]);
    const uint8_t* const s3 = static_cast<const uint8_t*>(vt[3]);
    if (!game.text.Contains(s2) || !game.text.Contains(s3) || !At(s2, kLfAllowed) || !At(s3, kLfInit) || CallAt(game, const_cast<uint8_t*>(s3) + 13) != t->weakGet)
        return Fail("the link filter's isLinkAllowed / initialize do not match");
    t->lfVtbl = vt;
    // FRecastQueryFilter's dtQueryFilter vtable (R-S2): from its Clone; slots 1 and 3 (what findPath calls) must be code.
    uint8_t* const clone = One(game, kRqfClone);
    const auto* const rvt = clone ? reinterpret_cast<const void* const*>(RipTarget(clone + kRqfCloneLea, 3, 7)) : nullptr;
    if (!rvt || !game.rdata.Contains(rvt) || !game.rdata.Contains(rvt + 4) || !game.text.Contains(rvt[1]) || !game.text.Contains(rvt[3]))
        return Fail("FRecastQueryFilter's vtable not found");
    t->rqfVtbl = rvt;
    // The nav-link area / flag writers (R-S1).
    uint8_t* const om = One(game, kOffMesh);
    uint8_t* const oms = One(game, kOffMeshSeg);
    if (!om || !oms) return Fail("dtNavMesh's nav-link updates not found once");
    t->offMesh = PrimaryFunction(game, om);
    t->offMeshSeg = PrimaryFunction(game, oms);
    if (!At(t->offMesh, kOffMeshPro) || !At(t->offMeshSeg, kOffMeshSegPro) || !OneIn(game, t->offMesh, 0x100, kOffMeshWrite) ||
        !OneIn(game, t->offMeshSeg, 0x140, kOffMeshSegWrite))
        return Fail("dtNavMesh's nav-link updates do not match");
    // The vcalls the fix makes: ShouldPostpone (vt+0x40) in F, AdjustEndLocation (vt+0x78) in E, the FindPath pointer in H.
    if (f[0x33] != 0xFF || f[0x34] != 0x52 || f[0x35] != 0x40 || e[0x10] != 0xFF || e[0x11] != 0x50 || e[0x12] != 0x78) return Fail("a virtual call does not match");
    return true;
}

// Entry detours: the first instructions copied to a slot that jumps back; the entry jumps to the slot's second half, a jump to the hook.
constexpr size_t kSlot = 96;
bool BuildSlot(uint8_t* fn, const Pattern& expect, void* hook, uint8_t* slot)
{
    const size_t n = expect.bytes.size();
    if (n < 5 || n > 24 || !MatchesAt(fn, expect)) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + n;
    memcpy(slot, fn, n);
    memcpy(slot + n, jmpAbs, 6);
    memcpy(slot + n + 6, &resume, 8);
    memcpy(slot + 64, jmpAbs, 6);
    memcpy(slot + 64 + 6, &hook, 8);
    return true;
}
bool Patch(uint8_t* fn, size_t n, uint8_t* stub)
{
    const int64_t rel = stub - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t code[24];
    memset(code, 0x90, sizeof code);
    code[0] = 0xE9;
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(code + 1, &rel32, 4);
    return WriteCode(fn, std::span<const uint8_t>(code, n));
}

}  // namespace

// Monitor thread, every second: the key (live once installed) and the gate.
void SetRepathParallel(int mode, int minQueued, bool helpersAllowed)
{
    if (!g_dtFind) return;
    int m = mode != 0 ? 1 : 0;
    if (!helpersAllowed) m = 0;
    const int n = minQueued < 1 ? 1 : minQueued;
    if (m != g_mode.load() || n != g_min.load()) {
        Log("[repath] RepathParallel %d -> %d, min %d -> %d%s", g_mode.load(), m, g_min.load(), n, helpersAllowed ? "" : " (ParallelHelpersAllowed: no)");
        g_mode = m;
        g_min = n;
    }
}

// Stats interval: one line when something happened.
void LogRepathParallelStats()
{
    if (!g_dtFind) return;
    ParSum& s = g_par;
    const auto X = [](std::atomic<int>& a) { return a.exchange(0, std::memory_order_relaxed); };
    const auto XU = [](std::atomic<int64_t>& a) { return static_cast<double>(a.exchange(0, std::memory_order_relaxed)) / 1000.0; };
    if (!s.batches.load(std::memory_order_relaxed) && !s.declined.load(std::memory_order_relaxed)) return;
    const int batches = X(s.batches), jobs = X(s.jobs), skipped = X(s.skipped), searched = X(s.searched), served = X(s.served), declined = X(s.declined),
              unused = X(s.unused), epoch = X(s.epochDrops), postponed = X(s.postponed), linked = X(s.linked), linkBad = X(s.linkBad),
              linkServed = X(s.linkServed), linkChanged = X(s.linkChanged), ok = X(s.proofOk), bad = X(s.proofBad);
    const double wall = XU(s.wallUs), job = XU(s.jobUs), maxWall = XU(s.maxWallUs);
    (void)ok;
    (void)bad;
    Log("[repath] stats: batches %d, searches %d of %d queued (skipped %d) took %.1f ms on helpers, game thread blocked %.1f ms (max %.2f); "
        "served %d, searched on the game thread %d, unused %d, nav changed %d, postponed %d; through nav links: %d searches, served %d, "
        "link changed %d, not servable %d",
        batches, searched, jobs, skipped, job, wall, maxWall, served, declined, unused, epoch, postponed, linked, linkServed, linkChanged, linkBad);
}

// Start-up only (version.dll). key = RepathParallel at start-up (0 = not installed; lab: the probe needs the detours too).
bool InstallRepathParallel(bool beforeGameCode, int key, int minQueued, int threads, bool helpersAllowed, bool labProbe)
{
    (void)labProbe;
    if (!key) { Log("[repath] RepathParallel off in OblivionPerfFix.ini (takes a restart to switch on)"); return false; }
    if (!beforeGameCode) { Log("[repath] RepathParallel: start-up only (version.dll), not installed"); return false; }
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    g_qpcMs = fq.QuadPart ? 1000.0 / static_cast<double>(fq.QuadPart) : 0.0;
    const GameModule& game = Game();
    Targets t{};
    const char* why = "";
    if (!Discover(game, &t, &why)) { Log("[repath] RepathParallel: %s: not installed", why); return false; }
    g_t = t;
    struct Site { uint8_t* fn; const char* pro; void* hook; void** orig; const char* name; };
    const Site sites[] = {
        {t.tick, kTickPro, reinterpret_cast<void*>(&TickHook), reinterpret_cast<void**>(&g_tick), "TickActor"},
        {t.inval, kInvalPro, reinterpret_cast<void*>(&InvalHook), reinterpret_cast<void**>(&g_inval), "InvalidateAffectedPaths"},
        {t.offMesh, kOffMeshPro, reinterpret_cast<void*>(&OffMeshHook), reinterpret_cast<void**>(&g_offMesh), "updateOffMeshConnectionByUserId"},
        {t.offMeshSeg, kOffMeshSegPro, reinterpret_cast<void*>(&OffMeshSegHook), reinterpret_cast<void**>(&g_offMeshSeg), "updateOffMeshSegmentConnectionByUserId"},
        {t.dtFind, kFindPathPro, reinterpret_cast<void*>(&DtFindHook), reinterpret_cast<void**>(&g_dtFind), "findPath"},
    };
    constexpr size_t kN = sizeof sites / sizeof sites[0];
    uint8_t* const region = AllocNearGame(kN * kSlot);
    if (!region) { Log("[repath] RepathParallel: no memory near the game's code: not installed"); return false; }
    for (size_t i = 0; i < kN; ++i)
        if (!BuildSlot(sites[i].fn, ParsePattern(sites[i].pro), sites[i].hook, region + i * kSlot)) {
            Log("[repath] RepathParallel: %s's entry does not match: not installed", sites[i].name);
            return false;
        }
    g_weakGet = reinterpret_cast<WeakGetFn>(t.weakGet);
    g_qCtor = reinterpret_cast<QryCtorFn>(t.qCtor);
    g_qInit = reinterpret_cast<QryInitFn>(t.qInit);
    g_initPf = reinterpret_cast<InitPfFn>(t.initPf);
    g_pqCtor = reinterpret_cast<PathQueryCtorFn>(t.pqCtor);
    g_free = reinterpret_cast<FreeFn>(t.free);
    g_isGt = reinterpret_cast<IsGtFn>(t.isGt);
    g_getWorld = reinterpret_cast<GetWorldFn>(t.getWorld);
    g_navSysClass = reinterpret_cast<StaticClassFn>(t.navSysClass);
    g_agentIfaceClass = reinterpret_cast<StaticClassFn>(t.agentClass);
    g_getIface = reinterpret_cast<IfaceFn>(t.getIface);
    g_lfVtbl = t.lfVtbl;
    g_rqfVtbl = t.rqfVtbl;
    for (int i = 0; i < 4; ++i) g_ourVtbl[i] = t.lfVtbl[i];
    g_linkAllowed = reinterpret_cast<LinkAllowedFn>(const_cast<void*>(t.lfVtbl[2]));
    g_ourVtbl[2] = reinterpret_cast<const void*>(&LinkAllowedHook);
    g_staticFind = t.staticFind;
    g_threads = threads < 0 ? 0 : threads > 8 ? 8 : threads;
    int m = key != 0 ? 1 : 0;
    if (!helpersAllowed) m = 0;
    g_mode = m;
    g_min = minQueued < 1 ? 1 : minQueued;
    for (size_t i = 0; i < kN; ++i) *sites[i].orig = region + i * kSlot;
    DWORD old;
    VirtualProtect(region, kN * kSlot, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, kN * kSlot);
    // findPath last: until it is in, the TickActor hook's batches find no served call (g_dtFind is the trampoline already).
    int n = 0;
    for (size_t i = 0; i < kN; ++i) {
        if (Patch(sites[i].fn, ParsePattern(sites[i].pro).bytes.size(), region + i * kSlot + 64)) {
            ++n;
            continue;
        }
        Log("[repath] RepathParallel: patching %s failed: off", sites[i].name);
        g_mode = 0;
        g_broken = true;
        break;
    }
    if (n != static_cast<int>(kN)) return n > 0;
    Log("[repath] RepathParallel installed: TickActor exe+0x%X, findPath exe+0x%X, InvalidateAffectedPaths exe+0x%X, nav-link updates exe+0x%X / "
        "exe+0x%X; mode %d, min %d queued, %d helper threads (on first use)%s; live",
        static_cast<unsigned>(t.tick - game.base), static_cast<unsigned>(t.dtFind - game.base), static_cast<unsigned>(t.inval - game.base),
        static_cast<unsigned>(t.offMesh - game.base), static_cast<unsigned>(t.offMeshSeg - game.base), g_mode.load(), g_min.load(), g_threads,
        helpersAllowed ? "" : " (off: ParallelHelpersAllowed)");
    return true;
}


// ---- self-test ----
int RepathParallelTestDiscover(const GameModule& game, uint32_t* rvas, const char** why)
{
    Targets t{};
    if (!Discover(game, &t, why)) return 0;
    uint8_t* const list[] = {t.tick, t.staticFind, t.inval, t.dtFind, t.weakGet, t.qCtor, t.qDtor, t.qInit, t.initPf, t.pqCtor, t.free, t.isGt,
                             t.getWorld, t.navSysClass, t.agentClass, t.getIface, t.implFind, reinterpret_cast<uint8_t*>(const_cast<void**>(t.lfVtbl)),
                             t.offMesh, t.offMeshSeg};
    for (size_t i = 0; i < sizeof list / sizeof list[0]; ++i) rvas[i] = static_cast<uint32_t>(list[i] - game.base);
    rvas[20] = static_cast<uint32_t>(static_cast<const uint8_t*>(t.rqfVtbl) - game.base);
    return 1;
}

// Self-test: the key on fake queries (every input compared; a virtual filter and the helpers' wrapped vtable).
bool RepathParallelTestKey()
{
    alignas(16) uint8_t q[0xC0] = {}, pool[0x30] = {}, lf[0x20] = {}, filter[kFilterBytes] = {};
    const void* nav = &q;
    uint8_t* lfp = lf;
    uint8_t* pp = pool;
    memcpy(q, &nav, 8);
    memcpy(q + 8, &lfp, 8);
    memcpy(q + 0x88, &pp, 8);
    q[0x78] = 1;
    *reinterpret_cast<int*>(pool + 0x20) = 2048;
    filter[0x408] = 7;
    const double sp[3] = {1, 2, 3}, ep[3] = {4, 5, 6};
    static const void* const fakeVt[4] = {};
    const void* const savedVt = g_lfVtbl;
    const void* const savedRqf = g_rqfVtbl;
    static const void* const fakeRqf[4] = {};
    g_rqfVtbl = fakeRqf;
    const void* const rq = fakeRqf;
    memcpy(filter, &rq, 8);
    g_lfVtbl = fakeVt;
    const void* v = fakeVt;
    memcpy(lf, &v, 8);
    Key a, b;
    bool ok = MakeKey(&a, q, 11, 22, sp, ep, 1e30, filter) && MakeKey(&b, q, 11, 22, sp, ep, 1e30, filter) && !memcmp(&a, &b, sizeof a);
    filter[0x20] = 1;  // one area cost byte
    ok = ok && MakeKey(&b, q, 11, 22, sp, ep, 1e30, filter) && memcmp(&a, &b, sizeof a) != 0;
    filter[0x20] = 0;
    const double ep2[3] = {4, 5, 6.0000001};
    ok = ok && MakeKey(&b, q, 11, 23, sp, ep2, 1e30, filter) && memcmp(&a, &b, sizeof a) != 0;
    *reinterpret_cast<int*>(pool + 0x20) = 1024;  // node budget
    ok = ok && MakeKey(&b, q, 11, 22, sp, ep, 1e30, filter) && memcmp(&a, &b, sizeof a) != 0;
    *reinterpret_cast<int*>(pool + 0x20) = 2048;
    q[0x78] = 0;  // require bit
    ok = ok && MakeKey(&b, q, 11, 22, sp, ep, 1e30, filter) && memcmp(&a, &b, sizeof a) != 0;
    q[0x78] = 1;
    // the helpers' wrapped vtable keys as the stock one
    v = g_ourVtbl;
    memcpy(lf, &v, 8);
    ok = ok && MakeKey(&b, q, 11, 22, sp, ep, 1e30, filter) && !memcmp(&a, &b, sizeof a);
    // a filter of another class is never keyed (isVirtual set or not)
    filter[kFilterVirtual] = 1;
    ok = ok && MakeKey(&b, q, 11, 22, sp, ep, 1e30, filter);
    const void* const other = fakeVt;
    memcpy(filter, &other, 8);
    ok = ok && !MakeKey(&b, q, 11, 22, sp, ep, 1e30, filter);
    memcpy(filter, &rq, 8);
    g_lfVtbl = savedVt;
    g_rqfVtbl = savedRqf;
    return ok;
}

// Self-test: the epoch hooks and the link marker (no game code: fake originals).
namespace {
int g_fakeCalls = 0;
void FakeOffMesh(void*, uint64_t, uint8_t, uint16_t) { ++g_fakeCalls; }
bool FakeAllowed(const void*, uint64_t id) { ++g_fakeCalls; return id == 5; }
bool g_fakeLinkState[8] = {};  // the fake custom links' answers (id 0..7)
bool FakeLinkState(const void*, uint64_t id) { ++g_fakeCalls; return id < 8 && g_fakeLinkState[id]; }
constexpr uint32_t kFakeStockStatus = 0x40000123u;  // what the fake stock findPath returns (a decline)
uint32_t FakeStockFind(uint8_t*, uint64_t, uint64_t, const double*, const double*, double, const uint8_t*, uint8_t*, double*) { return kFakeStockStatus; }
}  // namespace
bool RepathParallelTestHooks()
{
    const OffMeshFn s1 = g_offMesh, s2 = g_offMeshSeg;
    const LinkAllowedFn s3 = g_linkAllowed;
    g_offMesh = g_offMeshSeg = &FakeOffMesh;
    g_linkAllowed = &FakeAllowed;
    g_fakeCalls = 0;
    const uint32_t e0 = g_navEpoch.load();
    OffMeshHook(nullptr, 1, 2, 3);
    OffMeshSegHook(nullptr, 1, 2, 3);
    bool ok = g_navEpoch.load() == e0 + 2 && g_fakeCalls == 2;
    // the helper's hook passes the stock answer and records each link once, in first-asked order, with its answer
    LinkRec r;
    LinkRecClear(&r);
    t_linkRec = &r;
    ok = ok && LinkAllowedHook(nullptr, 5) && !LinkAllowedHook(nullptr, 6) && LinkAllowedHook(nullptr, 5) && g_fakeCalls == 5;
    t_linkRec = nullptr;
    ok = ok && r.n == 2 && !r.bad && r.id[0] == 5 && r.allowed[0] && r.id[1] == 6 && !r.allowed[1];
    LinkAllowedHook(nullptr, 7);  // not recording: nothing added
    ok = ok && r.n == 2;
    // a link answered differently within one search, or too many links: not servable
    LinkRecAdd(&r, 6, true);
    ok = ok && r.bad && !LinksSame(r, nullptr);
    LinkRecClear(&r);
    for (int i = 0; i < kMaxLinks; ++i) LinkRecAdd(&r, 100 + static_cast<uint64_t>(i), true);
    ok = ok && !r.bad && r.n == kMaxLinks;
    LinkRecAdd(&r, 100, true);  // a repeat is no new link
    ok = ok && !r.bad;
    LinkRecAdd(&r, 99, true);
    ok = ok && r.bad;
    g_offMesh = s1;
    g_offMeshSeg = s2;
    g_linkAllowed = s3;
    return ok;
}

// Self-test: the serve decision for a precomputed search that asked custom links (repath130l), through the real findPath
// detour with a fake stock findPath and fake link answers. `which`: 0 links unchanged -> served; 1 a link it asked flips ->
// declined (the stock search runs); 2 a link it never asked flips -> served; 3 no links asked, a link flips -> served;
// 4 the nav counter moved -> declined. Returns 1 when the outcome is the expected one.
int RepathParallelTestLinkServe(int which)
{
    alignas(16) static uint8_t q[0xC0], pool[0x30], lf[0x20], filter[kFilterBytes];
    memset(q, 0, sizeof q);
    memset(pool, 0, sizeof pool);
    memset(lf, 0, sizeof lf);
    memset(filter, 0, sizeof filter);
    static const void* const fakeLfVt[4] = {};
    static const void* const fakeRqf[4] = {};
    const void* const savedLf = g_lfVtbl, *const savedRqf = g_rqfVtbl;
    const LinkAllowedFn savedAllowed = g_linkAllowed;
    const DtFindFn savedFind = g_dtFind;
    const int savedMode = g_mode.load(), savedCount = g_jobCount;
    const uint32_t savedEpoch = g_batchEpoch;
    g_lfVtbl = fakeLfVt;
    g_rqfVtbl = fakeRqf;
    g_linkAllowed = &FakeLinkState;
    g_dtFind = &FakeStockFind;
    g_mode = 1;
    // the game thread's query: nav, its link filter {vtable, NavSys, owner, cached owner}, node pool and budget
    const void* nav = q;
    const void* lfp = lf;
    const void* pp = pool;
    memcpy(q, &nav, 8);
    memcpy(q + 8, &lfp, 8);
    memcpy(q + 0x88, &pp, 8);
    const void* lv = fakeLfVt;
    memcpy(lf, &lv, 8);
    *reinterpret_cast<int*>(pool + 0x20) = 2048;
    const void* rv = fakeRqf;
    memcpy(filter, &rv, 8);
    const double sp[3] = {1, 2, 3}, ep[3] = {4, 5, 6};
    // the precomputed search: same key, asked links 1 (allowed) and 2 (not allowed); link 3 exists but it never asked it
    for (int i = 0; i < 8; ++i) g_fakeLinkState[i] = false;
    g_fakeLinkState[1] = true;
    g_fakeLinkState[3] = true;
    static uint8_t resultItems[48] = {};
    Job& j = g_jobs[0];
    j.valid = j.searched = true;
    j.used = false;
    j.status = 0x40000000u;
    j.size = 1;
    j.cap = 1;
    j.data = resultItems;
    j.queryNodes = 17;
    LinkRecClear(&j.links);
    if (which != 3) {
        LinkRecAdd(&j.links, 1, true);
        LinkRecAdd(&j.links, 2, false);
    }
    bool ok = MakeKey(&j.key, q, 11, 22, sp, ep, 1e30, filter);
    g_jobCount = 1;
    g_batchEpoch = g_navEpoch.load();
    // between precompute and serve
    if (which == 1) g_fakeLinkState[2] = true;
    if (which == 2 || which == 3) g_fakeLinkState[3] = false;
    if (which == 4) g_navEpoch.fetch_add(1);
    uint8_t result[16] = {};
    const int served0 = g_par.served.load(), changed0 = g_par.linkChanged.load();
    t_batch = true;
    const uint32_t st = DtFindHook(q, 11, 22, sp, ep, 1e30, filter, result, nullptr);
    t_batch = false;
    uint8_t* got = nullptr;
    memcpy(&got, result, 8);
    const bool served = st == 0x40000000u && got == resultItems && Rd<int>(result, 8) == 1 && Rd<int>(q, 0x98) == 17 &&
                        g_par.served.load() == served0 + 1;
    const bool declined = st == kFakeStockStatus && got == nullptr && g_par.served.load() == served0;
    switch (which) {
    case 0: case 2: case 3: ok = ok && served; break;
    case 1: ok = ok && declined && g_par.linkChanged.load() == changed0 + 1 && j.data == resultItems; break;
    default: ok = ok && declined; break;
    }
    j.data = nullptr;  // not ours to free (a static buffer)
    g_jobCount = savedCount;
    g_batchEpoch = savedEpoch;
    g_mode = savedMode;
    g_dtFind = savedFind;
    g_linkAllowed = savedAllowed;
    g_lfVtbl = savedLf;
    g_rqfVtbl = savedRqf;
    return ok ? 1 : 0;
}

}  // namespace opf
