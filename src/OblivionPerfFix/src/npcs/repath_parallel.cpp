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
// updateOffMeshSegmentConnectionByUserId; setPolyFlags / setPolyArea are not in the exe) (rev130b R-S1). One more in-place
// area writer exists, ARecastNavMesh::ReplaceAreaInTileBounds (Blueprint-callable only; rev-repathearly S2): it does not matter
// to RepathParallel (no game code runs between its precompute and the serve) and is hooked with RepathEarly (counter). Custom nav links (doors, ladders, jump links): findPath
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
//
// RepathEarly (worker repathearly 2026-11-18, review rev-repathearly, fixes repathfix; S-062): at every
// InvalidateAffectedPaths (tile attach / detach, the runtime generator's tile rebuilds) the queued repaths are prepared on the
// game thread and only their findPath runs on the helpers at once; the results are served at that navdata's next TickActor
// under the key above while the nav counter is unchanged. At that TickActor the game thread decides BEFORE it waits
// (repathfix S1): a counter that moved or a key switched off aborts the whole early batch; otherwise this tick's jobs are
// built first and every early search no queued path matches (start point moved, path postponed or gone) is aborted on its
// own (a per-job flag the filter copy's slot 1 reads at filterCopy + kFilterBytes); the game thread then waits only for the
// matched searches (aborted ones drain: no new nodes). Navmesh writers: addTile, removeTile, dtFreeNavMesh (memory: fence,
// abort and wait), the nav-link updates and ReplaceAreaInTileBounds (in place: counter only); every one of them is counted
// in g_writers (seq_cst) while it runs, and an early batch is not started while a writer runs (repathfix S3, re-review S3').
// t_batch also covers nested findPath calls from the stock loop's events: they may be served from the batch too (each call
// checks the counter and the full key, so that is the same identity argument).
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
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

// RepathEarly (worker repathearly 2026-11-18): the navmesh's memory writers, fenced while an early search runs.
// dtNavMesh::addTile (exe+0x416E640) from its start: the header version check (7) and DT_TILE_FREE_DATA-free frame.
constexpr char kAddTile[] = "41 55 48 81 EC 90 0D 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 50 0D 00 00 66 83 3A 07";
// dtNavMesh::removeTile (exe+0x4184B40) from its start: the tile index decoded from the ref (m_saltBits +0xF4, m_tileBits +0xF8)
// and checked against m_maxTiles (+0x88).
constexpr char kRemoveTile[] =
    "4C 89 4C 24 20 53 57 41 54 41 55 41 56 48 81 EC 40 01 00 00 4D 8B E8 4C 8B F1 48 85 D2 0F 84 ?? ?? ?? ?? 44 8B 99 F8 00 00 00";
// dtFreeNavMesh (exe+0x41743A0) from its start: the null check, then the inlined ~dtNavMesh's tile loop (m_maxTiles +0x88).
constexpr char kFreeNav[] =
    "48 85 C9 0F 84 ?? ?? ?? ?? 53 48 83 EC 20 48 89 6C 24 30 48 8B D9 48 89 7C 24 40 4C 89 74 24 48 45 33 F6 41 8B EE 44 39 B1 88 00 00 00";
constexpr size_t kFreeNavSkip = 9;  // its body after `test rcx,rcx; je` (the detour does the null check itself)
// FPImplRecastNavMesh::ReleaseDetourNavMesh (exe+0x41DB690 +9): impl +8 (the dtNavMesh the searches use) freed by dtFreeNavMesh.
constexpr char kReleaseNav[] = "48 8B 49 08 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 C7 43 08 00 00 00 00";
constexpr size_t kReleaseNavCall = 9;
constexpr char kAddTilePro[] = "41 55 48 81 EC 90 0D 00 00";
constexpr char kRemoveTilePro[] = "4C 89 4C 24 20 53 57 41 54";
constexpr char kFreeNavPro[] = "48 85 C9 0F 84 ?? ?? ?? ??";  // test rcx,rcx; je (overwritten; the hook does the check)
// ARecastNavMesh::ReplaceAreaInTileBounds (exe+0x41DBB70; UE 5.3 RecastNavMesh.cpp:2198, BlueprintCallable through
// K2_ReplaceAreaInTileBounds): poly areas rewritten in place on attached tiles, no counter of its own (rev-repathearly S2).
// From its start: RecastNavMeshImpl (+0x5C0) and its dtNavMesh (+8) null checks; the area write (setArea: area & 0x3f).
constexpr char kReplaceArea[] =
    "4C 8B DC 53 55 57 41 54 41 56 48 83 EC 60 48 8B 81 C0 05 00 00 33 DB 49 8B E9 49 8B F8 4C 8B E2 4C 8B F1 48 85 C0 0F 84 ?? ?? ?? ?? "
    "48 39 58 08 0F 84";
constexpr char kReplaceAreaWrite[] = "83 E0 3F 3B C6 75 0E 0F B6 C2 40 32 C5 24 3F 32 C2 88 01 FF C3";
constexpr char kReplaceAreaPro[] = "4C 8B DC 53 55 57";  // mov r11,rsp; push rbx; push rbp; push rdi

// RepathDefer (worker repathdefer 2026-11-19): the stock loop's own handling of RepathRequests' elements.
// kTickAdd at F+0x52: the postpone path's RepathRequests.Add(RecalcRequest): num++ (+0x3D8), cmp max (+0x3DC), jbe; ResizeGrow(
// &RepathRequests (+0x3D0), oldNum); element = data + num*0x18 (lea rcx,[rbx+rbx*2]; [rdx+rcx*8]): object +0, controller +8 with
// its weak count +0xC incremented (lock inc [rax+0Ch]), reason dword +0x10 (exe+0x41B9113).
constexpr char kTickAdd[] =
    "76 0E 8B D3 49 8D 8F D0 03 00 00 E8 ?? ?? ?? ?? 4B 8B 04 2C 48 8D 0C 5B 49 8B 97 D0 03 00 00 48 89 04 CA 4B 8B 44 2C 08 "
    "48 89 44 CA 08 48 85 C0 74 04 F0 FF 40 0C 43 8B 44 2C 10 89 44 CA 10";
constexpr size_t kTickAddAt = 0x52, kTickAddCall = 0x0B;
// kTickSkip at F-0x39: the loop's Pin of the work-queue element (r12+r13): controller +8 null or its shared count 0 -> the
// element is skipped (exe+0x41B9088), so an element emptied to {null, null} is never searched.
constexpr char kTickSkip[] = "4B 8B 7C 2C 08 48 85 FF 0F 84 ?? ?? ?? ?? 8B 47 08 85 C0 0F 84 ?? ?? ?? ?? 8D 48 01 F0 0F B1 4F 08";
constexpr size_t kTickSkipBeforeF = 0x39;
// TArray<0x18-byte element>::ResizeGrow(int OldNum) (exe+0xD82700): ArrayNum (+8) already incremented; its element size.
constexpr char kResizeGrowPro[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 63 59 08 8B F2 48 8B F9 3B DA";
constexpr char kResizeGrowElem[] = "41 B9 18 00 00 00";

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
using AddTileFn = uint32_t (*)(void*, uint8_t*, int, int, uint64_t, uint64_t*);
using RemoveTileFn = uint32_t (*)(void*, uint64_t, uint8_t**, int*);
using FreeNavFn = void (*)(void*);
using ReplaceAreaFn = int32_t (*)(void*, const void*, void*, void*, bool, void*);
using PassFilterFn = bool (*)(const void*, uint64_t, const void*, const void*);  // dtQueryFilter::passVirtualFilter (slot 1)

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
// RepathEarly: the early searches' link filter (slot 2 = no game call, the search is dropped; slot 3 = no-op) and query filter
// copy (FRecastQueryFilter's dtQueryFilter vtable with slot 1 wrapped: false once the search is aborted).
const void* g_earlyLVt[4] = {};
const void* g_earlyFVt[4] = {};
PassFilterFn g_passVirtual = nullptr;       // FRecastQueryFilter's slot 1 (stock)
AddTileFn g_addTile = nullptr;              // trampolines to the stock writers (RepathEarly's fence)
RemoveTileFn g_removeTile = nullptr;
FreeNavFn g_freeNavBody = nullptr;          // dtFreeNavMesh after its null check
ReplaceAreaFn g_replaceArea = nullptr;      // ARecastNavMesh::ReplaceAreaInTileBounds (trampoline)
const uint8_t* g_staticFind = nullptr;      // ARecastNavMesh::FindPath (the pointer at navdata +0x340)
Targets g_t{};
double g_qpcMs = 0;

std::atomic<int> g_mode{0};       // live: 0 off, 1 on (lab: 2 proof)
std::atomic<int> g_min{2};        // live: queued repaths needed for a batch
std::atomic<bool> g_broken{false};  // a layout check failed at run time: off until the game ends
int g_threads = 4;                // helper threads (start-up)
std::atomic<uint32_t> g_navEpoch{0};  // navmesh edits findPath could see
std::atomic<int> g_writers{0};        // hooked navmesh writers running now, any thread (seq_cst: against an early publish, S3)

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

bool MakeKey(Key* k, const uint8_t* q, uint64_t s, uint64_t e, const double* sp, const double* ep, double cost, const uint8_t* filter,
             const uint8_t* filterId = nullptr)
{
    memset(k, 0, sizeof *k);
    k->nav = Rd<const void*>(q, 0);
    const uint8_t* const lf = Rd<const uint8_t*>(q, 8);
    const uint8_t* const pool = Rd<const uint8_t*>(q, 0x88);
    if (!lf || !pool || !filter || !sp || !ep) return false;
    k->lfVtbl = Rd<const void*>(lf, 0);
    // the helpers' wrapped copies are the stock filter (an early search that asked a link is never keyed)
    if (k->lfVtbl == static_cast<const void*>(g_ourVtbl) || k->lfVtbl == static_cast<const void*>(g_earlyLVt)) k->lfVtbl = g_lfVtbl;
    k->navSys = Rd<const void*>(lf, 8);
    k->owner = Rd<const void*>(lf, 0x18);
    k->require = Rd<uint8_t>(q, 0x78) & 1;
    k->maxRuntime = Rd<int>(pool, 0x20);
    k->startRef = s;
    k->endRef = e;
    if (!SafeCopy(k->sp, sp, 0, sizeof k->sp) || !SafeCopy(k->ep, ep, 0, sizeof k->ep)) return false;
    k->cost = cost;
    k->filter = filterId ? filterId : filter;  // an early search reads a private copy of the stock filter `filterId`
    if (!SafeCopy(k->filterBytes, filter, 0, kFilterBytes)) return false;
    const void* vt = nullptr;
    memcpy(&vt, k->filterBytes, 8);
    if (vt == static_cast<const void*>(g_earlyFVt)) {  // the early copy's wrapped vtable keys as FRecastQueryFilter's
        vt = g_rqfVtbl;
        memcpy(k->filterBytes, &vt, 8);
    }
    return vt == g_rqfVtbl;
}


// RepathDefer's tolerant key: every input the same except the start and end points (InitPathfinding's projections), each within
// `tol` uu of the searched ones (the polys they lie on are part of the key). `moved`: the larger of the two distances.
bool KeyNear(const Key& searched, const Key& live, int tol, double* moved)
{
    Key t = live;
    memcpy(t.sp, searched.sp, sizeof t.sp);
    memcpy(t.ep, searched.ep, sizeof t.ep);
    if (memcmp(&t, &searched, sizeof t)) return false;
    double ds = 0, de = 0;
    for (int i = 0; i < 3; ++i) {
        ds += (live.sp[i] - searched.sp[i]) * (live.sp[i] - searched.sp[i]);
        de += (live.ep[i] - searched.ep[i]) * (live.ep[i] - searched.ep[i]);
    }
    *moved = std::sqrt(std::max(ds, de));
    return *moved <= static_cast<double>(tol);
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
    // RepathEarly: what the game thread prepared at the invalidation (the helper's findPath reads only these, its own query
    // and the navmesh): InitPathfinding's polys and points, the stock filter and a private copy of its bytes.
    uint64_t sRef, eRef;
    double rs[3], re[3];
    const uint8_t* origFilter;
    alignas(16) uint8_t filterCopy[kFilterBytes];
    // Right after the copy (EarlyPassFilter reads it as self + kFilterBytes: a plain load, no TLS or global): 1 = this early
    // search stops (no new nodes; the open list drains): a custom link asked (linkAsked), not matched at TickActor, or the
    // batch aborted (a writer's fence, a new change, a counter that moved).
    std::atomic<uint8_t> stop;
    bool linkAsked;
    // RepathDefer: the request in RepathRequests this job came from ({path object, controller} as queued), the path's navigation
    // data (its query's FWeakObjectPtr and what it resolved to), and the job's place in a deferred batch.
    const void* reqObj;
    void* reqCtrl;
    const void* navData;
    uint64_t navWeak;
    uint8_t phase;      // kPhNone, kPhHeld (its request taken out of RepathRequests), kPhOffer (offered to this TickActor), kPhDone
    bool held;          // its request was taken out at the start (served under the tolerant key when put back)
    bool offer;         // its result may be served by this TickActor's findPath calls (state is 1: the helper is done with it)
    uint8_t req[0x18];  // the taken-out element, moved back as it was
    int age;            // TickActors of its navigation data since it was taken out
    bool servedTol;     // lab line: served under the tolerant key
    double movedUu;     // ... with the points this far from the searched ones
    // lab (why a put-back was not served): the declined findPath calls while it was offered, the nearest one's key difference
    // (KeyDiff mask, fewest bits) and how far its points were from the searched ones
    int nearSeen;
    uint32_t nearMask;
    double nearMoved;
};
#pragma warning(pop)
static_assert(offsetof(Job, stop) == offsetof(Job, filterCopy) + kFilterBytes, "the early stop flag follows the filter copy");
constexpr size_t kEarlyLfJob = 0x20;  // the early link filter: the stock 0x20 bytes, then its Job*
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
// The batch the helpers claim from (published before the claim word; read after a successful claim).
Job* g_runJobs = g_jobs;
const int* g_runOrder = g_order;
bool g_runEarly = false;

// RepathEarly: the batch started at an invalidation (game thread), searched by the helpers while the game thread runs on,
// collected at that navigation data's next TickActor or by a fence. g_earlyLock: start / collect / fence (the fence's writer
// may run on another thread: dtFreeNavMesh). g_earlyLive: helpers may be inside its searches.
Job g_early[kMaxJobs];
int g_earlyCount = 0;               // jobs set up (some not valid)
int g_earlyOrder[kMaxJobs];
int g_earlyN = 0;                   // valid jobs published to the helpers
const uint8_t* g_earlyNavData = nullptr;  // the ANavigationData whose RepathRequests they came from
uint32_t g_earlyEpoch = 0;          // the nav counter when it started (served only while unchanged)
bool g_earlyHave = false;           // results kept (collected or not) until that TickActor or the next start
std::atomic<bool> g_earlyLive{false};
SRWLOCK g_earlyLock = SRWLOCK_INIT;
std::atomic<int> g_earlyMode{0};    // live key: 0 off, 1 on (needs RepathParallel on)
bool g_earlyInstalled = false;
bool g_earlyServe = false;          // this TickActor may serve from the early batch

// RepathDefer (worker repathdefer 2026-11-19): a TickActor's queued repaths start on the helpers (the early batch's machinery:
// prepared on the game thread, findPath only on the helpers, navmesh writers fenced); what is not done after a short grace has
// its request taken out of RepathRequests and put back (served) when its search is done. g_deferHave: such a batch exists in
// g_early (its navigation data g_earlyNavData); the RepathEarly key does nothing while it does.
enum : uint8_t { kPhNone = 0, kPhHeld = 1, kPhOffer = 2, kPhDone = 3 };
using ResizeGrowFn = void (*)(void* array, int oldNum);  // TArray<FNavPathRecalculationRequest>::ResizeGrow (the stock Add's)
ResizeGrowFn g_resizeGrow = nullptr;
std::atomic<int> g_deferMode{0};          // live key: 0 off, 1 on (needs RepathParallel on)
std::atomic<int> g_deferGraceUs{500};     // live: the game thread's wait for the helpers before requests are taken out
std::atomic<int> g_deferMaxFrames{6};     // live: TickActors a request may stay out; then the game thread waits for its search
std::atomic<int> g_deferTolerance{50};    // live: start / end point distance (uu) a put-back result may be served across
bool g_deferInstalled = false;
bool g_deferHave = false;
int g_deferHeld = 0;                      // requests out of RepathRequests now
// review R3 (repathdefertest): a batch stopped by a navigation change is started again from the queue in the same TickActor;
// each request's TickActors out carry over (keyed by the queued path object + controller), so the RepathDeferMaxFrames bound
// holds across restarts and a stream of navigation changes can never keep a request out for longer
struct DeferCarry { const void* obj; const void* ctrl; int age; };
DeferCarry g_deferCarry[64];
int g_deferCarryN = 0;
bool g_deferNavAbort = false;             // the last DeferResume stopped its batch because the navigation counter moved
uint64_t g_deferNavWeak = 0;              // the batch's navigation data as an FWeakObjectPtr (its paths' queries' NavData)

// Counters: written by the game thread (relaxed), read and reset by the monitor thread's stats line.
struct ParSum {
    // linked: helper searches that asked a custom link; linkBad: of those, not servable (too many links / a changing answer);
    // linkServed: served (lab mode 2: compared) searches that asked a link; linkChanged: declined, a link's answer changed.
    std::atomic<int> batches{0}, jobs{0}, searched{0}, served{0}, declined{0}, unused{0}, skipped{0}, epochDrops{0}, postponed{0}, linked{0},
        linkBad{0}, linkServed{0}, linkChanged{0}, proofOk{0}, proofBad{0};
    std::atomic<int64_t> wallUs{0}, jobUs{0}, maxWallUs{0};
    // RepathEarly: batches / searches started at invalidations, searched (no link asked, not aborted), served, covered (the
    // TickActor batch skipped the path), linked (dropped: asked a custom link), stale (navmesh changed before the serve),
    // unused; batches aborted by a navmesh writer's fence (eFences), by the next change (eRestarts), whole at TickActor (counter
    // moved or key off: eTickAborts), not started because a writer ran (eWriterAtStart); searches aborted at TickActor
    // because no queued path matched them (eUncovered); hooked writers seen off the game thread (eOffGt, of them while a batch
    // was live eOffGtLive); the game thread's waits at TickActor and in the fence.
    std::atomic<int> eBatches{0}, eJobs{0}, eSearched{0}, eServed{0}, eCovered{0}, eLinked{0}, eStale{0}, eUnused{0}, eFences{0}, eRestarts{0},
        eTickAborts{0}, eWriterAtStart{0}, eUncovered{0}, eOffGt{0}, eOffGtLive{0};
    std::atomic<int64_t> eSearchUs{0}, eWaitUs{0}, eMaxWaitUs{0}, eFenceUs{0}, eGtUs{0};
    // RepathDefer: batches started at TickActor, their searches; done in the grace (offered at once; served there: dGraceServed);
    // requests taken out (dHeld) and put back (dPutBack, by TickActors out: dAge 1, 2, 3, 4-6, more); served after put-back on
    // identical inputs (dServedStrict) or within the tolerance (dServedTol), not served (dPutBackMissed: the key differs beyond
    // the tolerance, a link was asked, the path was gone); put back unsearched because the navmesh changed or the key went off
    // (dAborted) or the search asked a nav link (dLinkStop); TickActors that waited for a held search at the bound (dDeadline);
    // batches dropped with their navigation data (dDropped); a path queued twice left to the stock loop (dDup); requests the
    // stock loop searched while a batch was out (dOverlap); the grace's and the bound's waits on the game thread.
    std::atomic<int> dStarts{0}, dJobs{0}, dGraceDone{0}, dGraceServed{0}, dHeld{0}, dPutBack{0}, dServedStrict{0}, dServedTol{0},
        dPutBackMissed{0}, dAborted{0}, dLinkStop{0}, dDeadline{0}, dDropped{0}, dDup{0}, dOverlap{0}, dAge[5]{}, dProofSame{0},
        dProofDiff{0}, dHeldShort{0}, dRestarts{0};
    // review R2: the grace runs from the first helper's claim (wake latency dWakeUs / dMaxWakeUs); dHeldShort = held searches
    // that took less than the grace on the helper (held only because the helper started late)
    std::atomic<int64_t> dGraceUs{0}, dMaxGraceUs{0}, dDeadlineUs{0}, dMaxMoveMu{0}, dWakeUs{0}, dMaxWakeUs{0};
    // S-067 (repathfix 2026-11-20): starts handed back to RepathParallel's batch because every search ended in the grace and
    // some could not be served (a nav-link question stops a deferred search) (dHandBack, its searches dHandBackJobs, of them
    // stopped by a link dHandBackLinked); unservable searches left to the stock loop because others were still running
    // (dLinkLeft); DeferStart that started nothing: nothing preparable (dNoPrep), a navmesh writer running (dWriterStart);
    // RepathParallel batch jobs skipped because a deferred result is offered for their path (dBatchCovered); put-backs not
    // served by reason (lab): RepathParallel's fresh search served the path (dMissFresh), the stock loop queued it again
    // (postponed: dMissQueued), no findPath call while it was offered (dMissNoCall), the nearest call's key differed
    // (dMissKey); findPath calls the stock loop ran on the game thread inside navigation TickActors (sStock*, all builds).
    std::atomic<int> dHandBack{0}, dHandBackJobs{0}, dHandBackLinked{0}, dLinkLeft{0}, dNoPrep{0}, dWriterStart{0}, dBatchCovered{0},
        dMissFresh{0}, dMissQueued{0}, dMissNoCall{0}, dMissKey{0}, sStockN{0};
    std::atomic<int64_t> sStockUs{0}, sStockMaxUs{0};
};
ParSum g_par;
int g_mismatchLogs = 0, g_declineLogs = 0, g_earlyDeclineLogs = 0, g_deferLines = 0, g_deferTickLines = 0;
// What RepathDefer did at the current TickActor (lab line "[repath-defer] tick"): set by DeferTick / DeferStart.
enum DeferWhy : uint8_t {
    kWNone = 0,      // RepathDefer off or nothing queued
    kWStarted,       // a batch started (held and / or offered)
    kWResumed,       // a kept batch resumed (put-backs)
    kWRestarted,     // a kept batch stopped by a navmesh change and started again (R3)
    kWHandBack,      // every search ended in the grace, some unservable: handed to RepathParallel's batch (S-067)
    kWLinkLeft,      // started, but unservable searches were left to the stock loop while others still ran
    kWNotRecast,     // not a recast navigation data (or no find function)
    kWNoHelpers,     // no helper threads
    kWNoPrep,        // nothing preparable (other navigation data's paths, non-virtual filter, InitPathfinding failed)
    kWWriter,        // a navmesh writer ran at the publish
    kWOtherOut,      // another navigation data's batch is out
};
const char* const kDeferWhyText[] = {"off / nothing queued", "started", "resumed", "started again after a navmesh change",
                                     "handed to RepathParallel (every search ended in the grace, some unservable)",
                                     "started, unservable searches left to the stock loop (others still running)",
                                     "not a recast navigation data", "no helper threads",
                                     "not started: nothing preparable", "not started: a navmesh writer ran",
                                     "another navigation data's batch is out"};
DeferWhy g_deferWhy = kWNone;
// The game thread inside a navigation TickActor (TickHook): its stock findPath calls are timed (StockFind).
thread_local bool t_inTick = false;
thread_local int t_stockN = 0;
thread_local double t_stockMs = 0, t_stockMax = 0;
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

// ---- RepathEarly, helper side ----
// The early search's link filter (the stock 0x20 bytes, then its Job* at kEarlyLfJob): isLinkAllowed makes no game call (the
// game thread runs on); the question drops the search (it can't be served: its answer is unknown) and stops it. initialize:
// no-op (the cached owner is the game thread's).
bool EarlyLinkAllowed(const void* self, uint64_t)
{
    Job* const j = *reinterpret_cast<Job* const*>(static_cast<const uint8_t*>(self) + kEarlyLfJob);
    j->linkAsked = true;
    j->stop.store(1, std::memory_order_relaxed);
    return true;
}
void EarlyLinkInit(void*) {}
// The early search's filter copy, slot 1: the stock passVirtualFilter on the copy's bytes (the stock filter's bytes, keyed);
// false once the search is stopped (its job's flag right after the copy: no new nodes, the open list drains, findPath returns).
bool EarlyPassFilter(const void* self, uint64_t ref, const void* tile, const void* poly)
{
    if (reinterpret_cast<const std::atomic<uint8_t>*>(static_cast<const uint8_t*>(self) + kFilterBytes)->load(std::memory_order_relaxed))
        return false;
    return g_passVirtual(self, ref, tile, poly);
}

// One early search (helper thread): findPath only, from the inputs the game thread prepared at the invalidation, with a private
// query, the early link filter and the private filter copy. Reads: the navmesh (fenced against its writers), those copies.
void RunEarlyJob(Job& j)
{
    uint8_t* const q = t_query;
    if (!t_queryMade) { g_qCtor(q); t_queryMade = true; }
    alignas(8) uint8_t lf[kEarlyLfJob + 8];
    memcpy(lf, j.linkFilter, 0x20);
    const void* const ours = g_earlyLVt;
    memcpy(lf, &ours, 8);
    Job* const self = &j;
    memcpy(lf + kEarlyLfJob, &self, 8);
    g_qInit(q, j.nav, j.maxNodes, lf);
    q[0x78] = static_cast<uint8_t>((q[0x78] & 0xFE) | j.require);
    j.searched = false;
    LinkRecClear(&j.links);
    if (!j.stop.load(std::memory_order_acquire)) {
        uint8_t res[16] = {};
        const double t0 = QpcMsNow();
        j.status = g_dtFind(q, j.sRef, j.eRef, j.rs, j.re, j.cost, j.filterCopy, res, nullptr);
        j.ms = QpcMsNow() - t0;
        memcpy(&j.data, res, 8);
        memcpy(&j.size, res + 8, 4);
        memcpy(&j.cap, res + 12, 4);
        j.queryNodes = *reinterpret_cast<int*>(q + 0x98);
        j.links.bad = j.linkAsked;  // "asked a custom link" (counted; never served)
        j.searched = !j.linkAsked && !j.stop.load(std::memory_order_acquire) &&
                     MakeKey(&j.key, q, j.sRef, j.eRef, j.rs, j.re, j.cost, j.filterCopy, j.origFilter);
        if (!j.searched && j.data) { g_free(j.data); j.data = nullptr; }
    }
    *reinterpret_cast<void**>(q + 8) = nullptr;
    j.state.store(1, std::memory_order_release);
    WakeWordAll(&j.state);
}

DWORD WINAPI ParHelper(void*)
{
    for (;;) {
        WaitForSingleObject(g_sem, INFINITE);
        int i;
        while ((i = Claim()) >= 0) {
            Job& j = g_runJobs[g_runOrder[i]];  // published before the claim word (release), read after the claim (acquire)
            if (g_runEarly) RunEarlyJob(j);
            else RunJob(j);
        }
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

// RepathRequests' paths -> jobs with the inputs TickActor's stock FindPath will build (game thread): each kept path pinned and
// its stock query built (the caller destroys them and releases the pins); `valid` when its search would run and can be served (a filter of
// FRecastQueryFilter's class). Stops when the query layout check fails (g_broken). `countStats`: postponed paths are counted
// (only by the build of a batch RepathParallel runs: an early batch's build and a match-only build do not count them again).
void BuildJobs(const uint8_t* rq, int n, Job* jobs, int* count, bool countStats)
{
    const void* navSysOwner = nullptr;
    const void* navSys = nullptr;
    for (int i = 0; i < n && *count < kMaxJobs; ++i) {
        void* const obj = Rd<void*>(rq + i * 0x18, 0);
        void* const ctrl = Rd<void*>(rq + i * 0x18, 8);
        if (!obj || !ctrl) continue;
        bool dup = false;
        for (int k = 0; k < *count && !dup; ++k) dup = jobs[k].pathObj == obj;
        if (dup) continue;  // a path queued twice: its later searches stay on the game thread (serial)
        // TickActor's postpone check: such a path is queued again, not searched (searching it here every frame would
        // only block this thread for nothing)
        if (const void* querier = WeakGet(static_cast<uint8_t*>(obj) + 0xA8)) {
            const void* const iface = g_getIface(querier, g_agentIfaceClass());
            const void* const* const ivt = iface ? Rd<const void* const*>(iface, 0) : nullptr;
            if (ivt && reinterpret_cast<PostponeFn>(Rd<const void*>(ivt, 0x40))(iface)) {
                if (countStats) Inc(g_par.postponed);
                continue;
            }
        }
        if (!PinShared(ctrl)) continue;
        Job& j = jobs[(*count)++];
        j.pathObj = obj;
        j.pathCtrl = ctrl;
        j.reqObj = obj;
        j.reqCtrl = ctrl;
        j.navData = nullptr;
        j.navWeak = 0;
        j.phase = 0;
        j.held = j.offer = j.servedTol = false;
        j.movedUu = 0;
        j.nearSeen = 0;
        j.nearMask = 0;
        j.nearMoved = 0;
        j.age = 0;
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
        j.navData = navData;
        j.navWeak = Rd<uint64_t>(q, kQNavData);
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
}

// ---- RepathEarly, game thread side ----
// The claim word's unclaimed positions retired (no helper can start one now). Returns how many positions were claimed.
int CancelUnclaimed()
{
    uint64_t w = g_word.load(std::memory_order_acquire);
    for (;;) {
        const uint32_t next = static_cast<uint32_t>(w >> 16) & 0xFFFF, count = static_cast<uint32_t>(w) & 0xFFFF;
        if (next >= count) return static_cast<int>(count);
        const uint64_t nw = (w & ~0xFFFF0000ull) | (static_cast<uint64_t>(count) << 16);
        if (g_word.compare_exchange_weak(w, nw, std::memory_order_acq_rel, std::memory_order_acquire)) return static_cast<int>(next);
    }
}

// g_earlyLock held. Afterwards no helper is inside the early batch: unclaimed searches are cancelled, claimed ones waited for
// (stopped ones drain at once: no new nodes). `abortAll`: every search of the batch is stopped first. Returns the ms waited.
double EarlyCollectLocked(bool abortAll)
{
    if (!g_earlyLive.load(std::memory_order_acquire)) return 0;
    const double t0 = QpcMsNow();
    if (abortAll)
        for (int k = 0; k < g_earlyN; ++k) g_early[g_earlyOrder[k]].stop.store(1, std::memory_order_release);
    const int claimed = CancelUnclaimed();
    for (int k = claimed; k < g_earlyN; ++k) {
        Job& j = g_early[g_earlyOrder[k]];
        j.searched = false;
        j.state.store(1, std::memory_order_release);
    }
    for (int k = 0; k < claimed && k < g_earlyN; ++k) {
        Job& j = g_early[g_earlyOrder[k]];
        int spin = 0;
        uint64_t deadline = 0;
        while (j.state.load(std::memory_order_acquire) == 0) {
            if (SpinPhase(spin, deadline)) continue;
            const int seen = 0;
            BlockOnWord(&j.state, &seen, sizeof seen);
        }
    }
    g_word.store(static_cast<uint64_t>(g_gen) << 32, std::memory_order_release);
    g_earlyLive.store(false, std::memory_order_seq_cst);
    return QpcMsNow() - t0;
}

// The early batch's end (game thread, collected): counts, results freed. `offered`: it was offered to its TickActor.
void EarlyEnd(bool offered)
{
    int searched = 0, linked = 0, unused = 0;
    double ms = 0;
    for (int i = 0; i < g_earlyCount; ++i) {
        Job& j = g_early[i];
        if (!j.valid) continue;
        if (j.links.bad) ++linked;
        if (j.searched) {
            ++searched;
            ms += j.ms;
            if (offered && !j.used) ++unused;
        }
        if (j.data) { g_free(j.data); j.data = nullptr; }
    }
    Inc(g_par.eSearched, searched);
    Inc(g_par.eLinked, linked);
    Inc(g_par.eUnused, unused);
    AddUs(g_par.eSearchUs, ms);
    g_earlyCount = g_earlyN = 0;
    g_earlyHave = false;
    g_earlyNavData = nullptr;
}

// A hooked navmesh writer's entry (any thread; WriterLeave after the stock writer returns): counted in g_writers while it runs
// (S3: an early batch about to be published meanwhile is not started), and the nav counter moves (nothing searched before it is
// served after it). `memory` (dtNavMesh::addTile / removeTile, dtFreeNavMesh: they allocate, free or relink navmesh memory): an
// early batch searching THIS navmesh is stopped and waited for, so the writer never runs while a helper reads that navmesh.
// The in-place writers (nav-link updates, ReplaceAreaInTileBounds) cannot break a search's memory (the area is masked to 0..63,
// flags feed only AND tests): the counter is enough, such a result is never served.
void WriterEnter(const void* nav, bool memory)
{
    g_writers.fetch_add(1, std::memory_order_seq_cst);
    g_navEpoch.fetch_add(1, std::memory_order_acq_rel);
    const bool live = g_earlyLive.load(std::memory_order_seq_cst);
    if ((g_earlyInstalled || g_deferInstalled) && g_isGt && !g_isGt()) {  // expected never (every caller found is game-thread code): counted
        Inc(g_par.eOffGt);
        if (live) Inc(g_par.eOffGtLive);
    }
    if (!memory || !live) return;
    AcquireSRWLockExclusive(&g_earlyLock);
    bool mine = false;
    if (g_earlyLive.load(std::memory_order_acquire))
        for (int k = 0; k < g_earlyN && !mine; ++k) mine = g_early[g_earlyOrder[k]].nav == nav;
    if (mine) {
        const double ms = EarlyCollectLocked(true);
        Inc(g_par.eFences);
        AddUs(g_par.eFenceUs, ms);
    }
    ReleaseSRWLockExclusive(&g_earlyLock);
}
void WriterLeave() { g_writers.fetch_sub(1, std::memory_order_seq_cst); }

// g_earlyLock held, game thread: the prepared early jobs (g_earlyOrder[0, nv)) go to the helpers; false = not started (the
// caller ends the batch). A hooked writer running now (on another thread) is checked for between the g_earlyLive store and the
// hand-off, both sides seq_cst (Dekker: the writer counts itself, then reads g_earlyLive): a writer counted after this check
// reads live and fences (waits for the lock, then stops and waits for the batch); one counted before it either still runs
// (g_writers != 0: the batch is not started, no helper ever sees it; repathfix S3, re-review S3') or has finished, and its
// writes happen before the helpers start (its count's decrement is seen here). Never aborted after the hand-off for this.
bool EarlyPublishLocked(int nv)
{
    g_earlyLive.store(true, std::memory_order_seq_cst);
    if (g_writers.load(std::memory_order_seq_cst) != 0) {
        g_earlyLive.store(false, std::memory_order_seq_cst);  // a writer that saw true takes the lock after us and finds it false
        for (int k = 0; k < nv; ++k) g_early[g_earlyOrder[k]].searched = false;
        Inc(g_par.eWriterAtStart);
        return false;
    }
    ++g_gen;
    g_runJobs = g_early;
    g_runOrder = g_earlyOrder;
    g_runEarly = true;
    g_word.store((static_cast<uint64_t>(g_gen) << 32) | static_cast<uint64_t>(nv), std::memory_order_release);
    if (g_sem && g_helpers > 0) ReleaseSemaphore(g_sem, std::min(g_helpers, nv), nullptr);
    return true;
}

// The queued repaths of `self` prepared as TickActor will build them (game thread): stock query, adjusted end, then
// InitPathfinding with this thread's query and the stock link filter; the helpers run only findPath. Queries and pins go at
// once: the helpers need none of them. `ownOnly` (RepathDefer): only paths whose navigation data is `self`. Fills g_early /
// g_earlyCount / g_earlyOrder; returns how many are valid (g_earlyOrder[0, n)).
int EarlyPrepare(uint8_t* self, const uint8_t* rq, int n, bool ownOnly)
{
    g_earlyEpoch = g_navEpoch.load(std::memory_order_acquire);  // any navmesh edit from here on: never served
    g_earlyCount = 0;
    BuildJobs(rq, n, g_early, &g_earlyCount, false);
    uint8_t* const q = t_query;  // this thread's own query (RunJob's on the game thread)
    if (!t_queryMade) { g_qCtor(q); t_queryMade = true; }
    for (int i = 0; i < g_earlyCount; ++i) {
        Job& j = g_early[i];
        j.stop.store(0, std::memory_order_relaxed);
        j.linkAsked = false;
        if (!j.valid) continue;
        j.valid = false;
        if (ownOnly && j.navData != self) continue;  // a path of another navigation data queued here: left to the stock loop
        if (Rd<uint8_t>(j.dtFilter, kFilterVirtual) != 1) continue;  // the stop needs findPath to call the filter's slot 1
        // InitPathfinding with the stock link filter (its initialize takes the cached owner), as impl FindPath will
        alignas(8) uint8_t lf[0x20];
        memcpy(lf, j.linkFilter, sizeof lf);
        g_qInit(q, j.nav, j.maxNodes, lf);
        q[0x78] = static_cast<uint8_t>((q[0x78] & 0xFE) | j.require);
        j.sRef = j.eRef = 0;
        const bool ok = g_initPf(j.impl, j.start, j.adjEnd, q, j.dtFilter, j.rs, &j.sRef, j.re, &j.eRef);
        memcpy(j.linkFilter + 0x18, lf + 0x18, 8);
        *reinterpret_cast<void**>(q + 8) = nullptr;
        if (!ok) continue;  // stock: no findPath either
        if (!SafeCopy(j.filterCopy, j.dtFilter, 0, kFilterBytes)) continue;
        const void* const fvt = g_earlyFVt;
        memcpy(j.filterCopy, &fvt, 8);
        j.origFilter = j.dtFilter;
        j.valid = true;
    }
    int nv = 0;
    for (int i = 0; i < g_earlyCount; ++i) {
        Job& j = g_early[i];
        if (j.queryBuilt) DestroyQuery(j.query);
        j.queryBuilt = false;
        ReleaseShared(j.pathCtrl);
        j.pathCtrl = nullptr;
        if (j.valid && !g_broken.load(std::memory_order_relaxed)) g_earlyOrder[nv++] = i;
    }
    return nv;
}

// InvalidateAffectedPaths' return (game thread): the queued repaths of `self` prepared (EarlyPrepare) and handed to the helpers.
void EarlyStart(uint8_t* self)
{
    if (!g_earlyInstalled || g_earlyMode.load(std::memory_order_relaxed) == 0 || g_mode.load(std::memory_order_relaxed) == 0 ||
        g_broken.load(std::memory_order_relaxed) || t_batch || !g_isGt())
        return;
    // RepathDefer owns the helpers and the early slot while it is on or a deferred batch is kept (RepathEarly is superseded)
    if (g_deferHave || g_deferMode.load(std::memory_order_relaxed) != 0) return;
    // a batch still running (its navmesh changed: this invalidation follows a tile change) or kept: gone
    AcquireSRWLockExclusive(&g_earlyLock);
    if (g_earlyLive.load(std::memory_order_acquire)) {
        EarlyCollectLocked(true);
        Inc(g_par.eRestarts);
    }
    ReleaseSRWLockExclusive(&g_earlyLock);
    if (g_earlyHave) EarlyEnd(false);
    if (Rd<const uint8_t*>(self, 0x340) != g_staticFind) return;
    const uint8_t* const rq = Rd<const uint8_t*>(self, 0x3D0);
    int n = Rd<int>(self, 0x3D8);
    if (!rq || n < 1) return;
    if (n > 1000) n = 1000;
    if (!g_helpersTried) StartHelpers();
    if (!g_sem || g_helpers == 0) return;
    const double t0 = QpcMsNow();
    const int nv = EarlyPrepare(self, rq, n, false);
    AddUs(g_par.eGtUs, QpcMsNow() - t0);
    if (nv == 0) { g_earlyCount = 0; return; }
    g_earlyNavData = self;
    g_earlyN = nv;
    g_earlyHave = true;
    AcquireSRWLockExclusive(&g_earlyLock);
    const bool started = EarlyPublishLocked(nv);
    ReleaseSRWLockExclusive(&g_earlyLock);
    if (!started) {  // a navmesh writer was running: nothing handed to the helpers, nothing to serve
        EarlyEnd(false);
        return;
    }
    Inc(g_par.eBatches);
    Inc(g_par.eJobs, nv);
}

// An early search's pre-search inputs equal a TickActor job's (`live`: that job's filter bytes now): the stock InitPathfinding
// then gets the same polys and points unless a custom link answers differently (then the key misses and the stock search runs).
bool EarlyInputsSame(const Job& e, const Job& j, const uint8_t* live)
{
    return e.impl == j.impl && e.nav == j.nav && e.origFilter == j.dtFilter && e.maxNodes == j.maxNodes && e.require == j.require &&
           !memcmp(e.start, j.start, sizeof e.start) && !memcmp(e.adjEnd, j.adjEnd, sizeof e.adjEnd) && !memcmp(&e.cost, &j.cost, sizeof e.cost) &&
           !memcmp(e.linkFilter, j.linkFilter, 0x18) && !memcmp(e.filterCopy + 8, live + 8, kFilterBytes - 8);
}
// A TickActor batch's path an early search already covers (searched, same pre-search inputs): not searched again.
bool EarlyCovers(const Job& j)
{
    alignas(16) uint8_t live[kFilterBytes];
    if (!SafeCopy(live, j.dtFilter, 0, kFilterBytes)) return false;
    for (int i = 0; i < g_earlyCount; ++i) {
        const Job& e = g_early[i];
        if (e.valid && e.searched && EarlyInputsSame(e, j, live)) return true;
    }
    return false;
}
// TickActor, before it waits (game thread): every published early search that no valid job of this tick (g_jobs) matches on
// its pre-search inputs (the querier moved, the path was postponed or left the queue) is stopped, so the game thread does not
// wait for a search it could never serve (S1). Returns how many were stopped.
int EarlyStopUnmatched()
{
    bool keep[kMaxJobs] = {};
    alignas(16) uint8_t live[kFilterBytes];
    for (int i = 0; i < g_jobCount; ++i) {
        const Job& j = g_jobs[i];
        if (!j.valid || !SafeCopy(live, j.dtFilter, 0, kFilterBytes)) continue;
        for (int k = 0; k < g_earlyN; ++k)
            if (!keep[k] && EarlyInputsSame(g_early[g_earlyOrder[k]], j, live)) keep[k] = true;
    }
    int n = 0;
    for (int k = 0; k < g_earlyN; ++k)
        if (!keep[k]) {
            g_early[g_earlyOrder[k]].stop.store(1, std::memory_order_release);
            ++n;
        }
    return n;
}
// The early batch can still be served at this TickActor: RepathParallel and RepathEarly on, nav counter unchanged since it started.
bool EarlyUsable(bool on)
{
    return on && g_earlyMode.load(std::memory_order_relaxed) > 0 && g_navEpoch.load(std::memory_order_acquire) == g_earlyEpoch;
}
// TickActor's collect of its early batch (game thread; this tick's jobs already built when `usable`): not usable -> the whole
// batch is stopped; usable -> the searches nothing matches are stopped. Then unclaimed ones are cancelled and the claimed waited
// for (stopped ones drain at once). Returns the ms waited.
double EarlyTickCollect(bool usable)
{
    if (usable && g_earlyLive.load(std::memory_order_acquire)) Inc(g_par.eUncovered, EarlyStopUnmatched());
    AcquireSRWLockExclusive(&g_earlyLock);
    const bool live = g_earlyLive.load(std::memory_order_acquire);
    const double ms = EarlyCollectLocked(!usable);
    ReleaseSRWLockExclusive(&g_earlyLock);
    if (!usable && live) Inc(g_par.eTickAborts);
    return ms;
}

// ---- RepathDefer, game thread side ----
// RepathRequests: navdata +0x3D0 {data, num +8, max +0xC}; element 0x18 {path object +0, controller +8 (weak count +0xC),
// reason +0x10} (pinned by the stock loop's own Add: kTickAdd). The stock loop skips an element whose controller is null
// (Pin fails: kTickSkip) and its copy / Reset destroy such a weak pointer without a call.
constexpr size_t kRqData = 0x3D0, kRqNum = 0x3D8, kRqMax = 0x3DC, kRqStride = 0x18;

void ReleaseWeak(void* ctrl)  // TWeakPtr's destructor: the controller goes when its weak count reaches 0
{
    if (!ctrl) return;
    auto* const c = static_cast<uint8_t*>(ctrl);
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(c + 0xC)) == 0) {
        void** const vt = *reinterpret_cast<void***>(c);
        reinterpret_cast<void (*)(void*, int)>(vt[1])(c, 1);
    }
}

// The job's request taken out of `self`'s RepathRequests: its element's weak pointer moved into the job (the element stays,
// {null, null}: the stock loop skips it). False (left queued, the stock loop searches it now) when the path is not queued
// exactly once.
bool TakeRequest(uint8_t* self, Job& j)
{
    uint8_t* const data = Rd<uint8_t*>(self, kRqData);
    const int num = Rd<int>(self, kRqNum);
    if (!data || num < 1) return false;
    int at = -1, seen = 0;
    for (int i = 0; i < num; ++i) {
        uint8_t* const e = data + static_cast<size_t>(i) * kRqStride;
        if (Rd<const void*>(e, 0) != j.reqObj) continue;
        ++seen;
        if (Rd<void*>(e, 8) == j.reqCtrl) at = i;
    }
    if (seen != 1 || at < 0) return false;
    uint8_t* const e = data + static_cast<size_t>(at) * kRqStride;
    memcpy(j.req, e, kRqStride);
    memset(e, 0, 16);  // {object, controller}: the weak reference now belongs to the job
    return true;
}

// Put-back (game thread, before the stock loop): the taken-out elements, in their order, go to the FRONT of RepathRequests
// (the stock game ran them a TickActor or more ago, so before everything queued since), grown with the stock Add's steps.
void PutBackFront(uint8_t* self, const uint8_t (*recs)[kRqStride], int k)
{
    if (k <= 0) return;
    int num = Rd<int>(self, kRqNum);
    for (int i = 0; i < k; ++i) {
        const int old = num++;
        *reinterpret_cast<int*>(self + kRqNum) = num;
        if (static_cast<unsigned>(num) > static_cast<unsigned>(Rd<int>(self, kRqMax))) g_resizeGrow(self + kRqData, old);
    }
    uint8_t* const data = Rd<uint8_t*>(self, kRqData);
    memmove(data + static_cast<size_t>(k) * kRqStride, data, static_cast<size_t>(num - k) * kRqStride);
    for (int i = 0; i < k; ++i) memcpy(data + static_cast<size_t>(i) * kRqStride, recs[i], kRqStride);
}

void DeferWaitCount(std::atomic<int64_t>& sum, std::atomic<int64_t>* maxUs, double ms)
{
    AddUs(sum, ms);
    const int64_t us = static_cast<int64_t>(ms * 1000.0);
    if (maxUs && us > maxUs->load(std::memory_order_relaxed)) maxUs->store(us, std::memory_order_relaxed);
}

// TickActor entry with requests queued and no deferred batch (game thread, RepathDefer and RepathParallel on): every queued
// path is prepared and handed to the helpers; the game thread waits up to RepathDeferGraceUs. Searches done by then are offered
// to this TickActor (served on identical inputs, as RepathEarly's); the rest have their requests taken out and stay on the
// helpers. True when something is offered.
// S-067 (repathfix 2026-11-20): after the grace, before any request is taken out. A deferred search stops at its first custom
// nav-link question (EarlyLinkAllowed: the answer would need game code while the game thread runs on), so it ends at once
// unsearched; the stock loop would then run it serially on the game thread, and while the deferred batch exists RepathParallel's
// own batch is off (one batch on the helpers). The road after a fast travel queues bursts of 5 link-crossing searches of ~7 ms:
// 5 x 7 ms serially in one TickActor (S-067) where RepathParallel alone ran them in parallel (game thread ~7-9 ms). So: when
// every published search has ended (none is still running, so nothing would be kept out) and at least one ended unsearched,
// the start is handed back: nothing is taken out or offered, and this TickActor runs exactly as with RepathDefer off
// (RepathParallel's batch, which asks the links on its helpers while the game thread waits, or the stock loop below
// RepathParallelMin). Returns the unsearched count (0 = keep the batch); *linked: of them, stopped by a nav-link question.
int DeferUnservableAllDone(int nv, int* linked)
{
    int running = 0, unsearched = 0, lk = 0;
    for (int k = 0; k < nv; ++k) {
        const Job& j = g_early[g_earlyOrder[k]];
        if (j.state.load(std::memory_order_acquire) == 0) { ++running; continue; }
        if (!j.searched) {
            ++unsearched;
            if (j.linkAsked) ++lk;  // written by the helper before its state store (release), read after the acquire
        }
    }
    *linked = lk;
    return running == 0 ? unsearched : 0;
}

// The hand-back (game thread): every search has ended; the batch is collected and ended, nothing was taken out.
void DeferHandBack(int nv, int unsearched, int linked)
{
    AcquireSRWLockExclusive(&g_earlyLock);
    EarlyCollectLocked(true);  // all ended: nothing to wait for; the claim word is cleared
    ReleaseSRWLockExclusive(&g_earlyLock);
    for (int k = 0; k < nv; ++k) {
        Job& j = g_early[g_earlyOrder[k]];
        j.phase = kPhDone;
        j.offer = false;
    }
    g_deferHave = false;
    g_deferHeld = 0;
    g_deferCarryN = 0;
    Inc(g_par.dHandBack);
    Inc(g_par.dHandBackJobs, nv);
    Inc(g_par.dHandBackLinked, linked);
    (void)unsearched;
    g_deferWhy = kWHandBack;
    EarlyEnd(false);
}

bool DeferAfterPublish(uint8_t* self, int nv, double t0);
bool DeferStart(uint8_t* self)
{
    if (!g_staticFind || Rd<const uint8_t*>(self, 0x340) != g_staticFind) { g_deferWhy = kWNotRecast; return false; }
    const uint8_t* const rq = Rd<const uint8_t*>(self, kRqData);
    int n = Rd<int>(self, kRqNum);
    if (!rq || n < 1) return false;
    if (n > 1000) n = 1000;
    if (!g_helpersTried) StartHelpers();
    if (!g_sem || g_helpers == 0) { g_deferWhy = kWNoHelpers; return false; }
    const double t0 = QpcMsNow();
    const int nv = EarlyPrepare(self, rq, n, true);
    if (nv == 0) {
        g_earlyCount = 0;
        g_deferWhy = kWNoPrep;
        Inc(g_par.dNoPrep);
        return false;
    }
    g_earlyNavData = self;
    g_earlyN = nv;
    g_deferNavWeak = g_early[g_earlyOrder[0]].navWeak;
    AcquireSRWLockExclusive(&g_earlyLock);
    const bool started = EarlyPublishLocked(nv);
    ReleaseSRWLockExclusive(&g_earlyLock);
    if (!started) {  // a navmesh writer was running: nothing handed out; RepathParallel / the stock loop searches as usual
        EarlyEnd(false);
        g_deferWhy = kWWriter;
        Inc(g_par.dWriterStart);
        return false;
    }
    return DeferAfterPublish(self, nv, t0);
}

// DeferStart's part after the publish (game thread; g_early / g_earlyOrder[0, nv) handed to the helpers at t0): the grace, the
// S-067 hand-back, the take-out of unfinished requests. Split out so the self-test can drive it with fake jobs and a fake queue.
bool DeferAfterPublish(uint8_t* self, int nv, double t0)
{
    g_deferHave = true;
    g_deferHeld = 0;
    Inc(g_par.dStarts);
    Inc(g_par.dJobs, nv);
    // the grace: cheap searches end in tens of microseconds (a spin; nothing else runs on this thread meanwhile). It runs from
    // the first helper's claim (review R2: a helper's wake from its semaphore can take 0.1-1 ms on a loaded CPU), at most
    // kDeferWakeMaxMs longer than from the publish.
    constexpr double kDeferWakeMaxMs = 1.0;
    const double grace = static_cast<double>(g_deferGraceUs.load(std::memory_order_relaxed)) / 1000.0;
    double tClaim = 0;
    for (;;) {
        bool all = true;
        for (int k = 0; k < nv && all; ++k) all = g_early[g_earlyOrder[k]].state.load(std::memory_order_acquire) != 0;
        const double now = QpcMsNow();
        if (tClaim == 0 && ((g_word.load(std::memory_order_acquire) >> 16) & 0xFFFF) != 0) tClaim = now;
        if (all || (tClaim != 0 && now - tClaim >= grace) || now - t0 >= grace + kDeferWakeMaxMs) break;
        for (int p = 0; p < 32; ++p) _mm_pause();
    }
    DeferWaitCount(g_par.dWakeUs, &g_par.dMaxWakeUs, (tClaim != 0 ? tClaim : QpcMsNow()) - t0);
    g_deferWhy = kWStarted;
    {
        int linked = 0;
        if (const int unservable = DeferUnservableAllDone(nv, &linked); unservable > 0) {  // S-067: the whole start goes back
            DeferHandBack(nv, unservable, linked);
            DeferWaitCount(g_par.dGraceUs, &g_par.dMaxGraceUs, QpcMsNow() - t0);
            return false;
        }
    }
    bool any = false, unheld = false;
    int unservableLeft = 0, linkedLeft = 0, graceDone = 0, dups = 0;
    for (int k = 0; k < nv; ++k) {
        Job& j = g_early[g_earlyOrder[k]];
        if (j.state.load(std::memory_order_acquire) != 0) {
            Inc(g_par.dGraceDone);
            ++graceDone;
            j.phase = kPhOffer;
            j.offer = j.searched;
            any |= j.offer;
            if (!j.searched) {  // others still run (else handed back above): the stock loop searches it now
                ++unservableLeft;
                if (j.linkAsked) ++linkedLeft;
            }
        } else if (TakeRequest(self, j)) {
            j.phase = kPhHeld;
            j.held = true;
            j.age = 0;
            for (int c = 0; c < g_deferCarryN; ++c)  // R3: a restart after a navigation change keeps the request's age
                if (g_deferCarry[c].obj == j.reqObj && g_deferCarry[c].ctrl == j.reqCtrl) { j.age = g_deferCarry[c].age; break; }
            ++g_deferHeld;
            Inc(g_par.dHeld);
        } else {  // queued twice (or gone): the stock loop searches it now; this search is stopped (drains at once)
            Inc(g_par.dDup);
            ++dups;
            j.stop.store(1, std::memory_order_release);
            j.phase = kPhDone;
            unheld = true;
        }
    }
    if (g_deferHeld == 0 && unservableLeft > 0) {  // the running ones ended meanwhile, nothing was taken out: handed back as above
        Inc(g_par.dGraceDone, -graceDone);
        Inc(g_par.dDup, -dups);  // handed back: the stock loop or RepathParallel searches them as with RepathDefer off
        DeferHandBack(nv, unservableLeft, linkedLeft);
        DeferWaitCount(g_par.dGraceUs, &g_par.dMaxGraceUs, QpcMsNow() - t0);
        return false;
    }
    if (unservableLeft > 0) {
        Inc(g_par.dLinkLeft, unservableLeft);
        g_deferWhy = kWLinkLeft;
    }
    if (g_deferHeld == 0 && unheld) {  // nothing kept out: no helper may stay inside the batch past this TickActor
        AcquireSRWLockExclusive(&g_earlyLock);
        EarlyCollectLocked(false);
        ReleaseSRWLockExclusive(&g_earlyLock);
    }
    DeferWaitCount(g_par.dGraceUs, &g_par.dMaxGraceUs, QpcMsNow() - t0);
    g_deferCarryN = 0;
    return any;
}

// A kept batch at a TickActor of its own navigation data (game thread, before the stock loop): requests whose search is done
// are put back (offered; under the tolerant key); the navmesh changed or a key off: the batch is stopped and everything put
// back unsearched (the stock loop searches it now, on the navmesh as it is); a request out RepathDeferMaxFrames TickActors:
// the game thread waits for its search. True when something is offered.
void DeferDrop();
bool DeferResume(uint8_t* self, bool on)
{
    if (WeakGet(&g_deferNavWeak) != self) {  // another object at its address: the requests belonged to the old one
        DeferDrop();
        return false;
    }
    const bool off = !on || g_deferMode.load(std::memory_order_relaxed) == 0 || g_broken.load(std::memory_order_relaxed);
    const bool abort = off || g_navEpoch.load(std::memory_order_acquire) != g_earlyEpoch;
    g_deferNavAbort = abort && !off;
    g_deferCarryN = 0;
    const int bound = g_deferMaxFrames.load(std::memory_order_relaxed);
    bool due = false;
    for (int i = 0; i < g_earlyCount; ++i) {
        Job& j = g_early[i];
        if (j.phase != kPhHeld) continue;
        ++j.age;
        due |= j.age >= bound;
    }
    if (const int queued = Rd<int>(self, kRqNum); queued > 0) Inc(g_par.dOverlap, queued);
    if (abort || due) {
        const double t0 = QpcMsNow();
        AcquireSRWLockExclusive(&g_earlyLock);
        EarlyCollectLocked(abort);  // abort: stopped searches drain at once; due: waits for the running one(s)
        ReleaseSRWLockExclusive(&g_earlyLock);
        if (due && !abort) {
            Inc(g_par.dDeadline);
            DeferWaitCount(g_par.dDeadlineUs, nullptr, QpcMsNow() - t0);
        }
    }
    uint8_t recs[kMaxJobs][kRqStride];
    int k = 0;
    bool any = false;
    for (int i = 0; i < g_earlyCount; ++i) {
        Job& j = g_early[i];
        if (j.phase != kPhHeld || j.state.load(std::memory_order_acquire) == 0) continue;
        memcpy(recs[k++], j.req, kRqStride);
        --g_deferHeld;
        Inc(g_par.dPutBack);
        Inc(g_par.dAge[j.age <= 1 ? 0 : j.age == 2 ? 1 : j.age == 3 ? 2 : j.age <= 6 ? 3 : 4]);
        if (abort) {
            Inc(g_par.dAborted);
            if (g_deferNavAbort && g_deferCarryN < 64) g_deferCarry[g_deferCarryN++] = {j.reqObj, j.reqCtrl, j.age};
        } else if (j.linkAsked) Inc(g_par.dLinkStop);
        if (!abort && j.searched && j.ms * 1000.0 < static_cast<double>(g_deferGraceUs.load(std::memory_order_relaxed)))
            Inc(g_par.dHeldShort);
        j.offer = !abort && j.searched;
        j.phase = j.offer ? kPhOffer : kPhDone;
        any |= j.offer;
    }
    PutBackFront(self, recs, k);
    if (g_deferHeld == 0 && g_earlyLive.load(std::memory_order_acquire)) {  // every search done (or stopped): collected
        AcquireSRWLockExclusive(&g_earlyLock);
        EarlyCollectLocked(false);
        ReleaseSRWLockExclusive(&g_earlyLock);
    }
    return any;
}

// The batch dropped without a put-back (its navigation data is gone: the requests belong to nothing that ticks any more):
// searches stopped and collected, the held weak references released.
void DeferDrop()
{
    AcquireSRWLockExclusive(&g_earlyLock);
    EarlyCollectLocked(true);
    ReleaseSRWLockExclusive(&g_earlyLock);
    for (int i = 0; i < g_earlyCount; ++i) {
        Job& j = g_early[i];
        if (j.phase == kPhHeld) ReleaseWeak(Rd<void*>(j.req, 8));
        j.phase = kPhDone;
        j.offer = false;
    }
    g_deferHeld = 0;
    g_deferHave = false;
    Inc(g_par.dDropped);
    EarlyEnd(false);
}

// Why a put-back result was not served (lab line and counters; game thread, after the stock TickActor `self`, before this
// tick's RepathParallel batch is finished): 0 RepathParallel's fresh search of this tick served the path; 1 the stock loop queued
// it again (ShouldPostponePathUpdates: it searches it at a later TickActor); 2 no findPath call was declined while it was offered
// (the path was gone, or FindPath ended before the A*: start = end, InitPathfinding failed); 3 a declined call's key differed
// (j.nearMask / j.nearMoved name the nearest one).
int DeferMissReason(const uint8_t* self, const Job& j)
{
    for (int k = 0; k < g_jobCount; ++k)
        if (g_jobs[k].pathObj == j.reqObj && g_jobs[k].valid && g_jobs[k].used) return 0;
    if (self) {
        const uint8_t* const data = Rd<const uint8_t*>(self, kRqData);
        const int num = Rd<int>(self, kRqNum);
        for (int i = 0; data && i < num && i < 1000; ++i)
            if (Rd<const void*>(data + static_cast<size_t>(i) * kRqStride, 0) == j.reqObj) return 1;
    }
    return j.nearSeen == 0 ? 2 : 3;
}

// After the stock TickActor that had offers (game thread): offers end (unused put-back results counted); the batch ends when
// no request is out any more. `self`: the navigation data that ticked (null: no queue to look at for the lab reason).
void DeferAfterTick(const uint8_t* self = nullptr)
{
    for (int i = 0; i < g_earlyCount; ++i) {
        Job& j = g_early[i];
        if (j.phase != kPhOffer) continue;
        if (j.held && !j.used) {
            Inc(g_par.dPutBackMissed);
            const int why = DeferMissReason(self, j);
            Inc(why == 0 ? g_par.dMissFresh : why == 1 ? g_par.dMissQueued : why == 2 ? g_par.dMissNoCall : g_par.dMissKey);
        }
        j.offer = false;
        j.phase = kPhDone;
        if (j.data) { g_free(j.data); j.data = nullptr; }
    }
    if (g_deferHeld > 0) return;
    if (g_earlyLive.load(std::memory_order_acquire)) {
        AcquireSRWLockExclusive(&g_earlyLock);
        EarlyCollectLocked(false);
        ReleaseSRWLockExclusive(&g_earlyLock);
    }
    g_deferHave = false;
    EarlyEnd(true);
}

// TickActor's RepathDefer step (game thread, before the stock loop). Returns true when this TickActor may serve from the batch
// (g_early offers); *handled: the batch exists after this step (RepathParallel's own batch and RepathEarly stay out of it).
bool DeferTick(uint8_t* self, bool on, bool* handled)
{
    *handled = false;
    if (!g_deferInstalled) return false;
    if (g_deferHave) {
        if (g_earlyNavData == self) {
            g_deferNavAbort = false;
            g_deferWhy = kWResumed;
            const bool any = DeferResume(self, on);
            // R3: stopped by a navigation change with everything put back unsearched: end that batch and start a new one from
            // the queue as it is now (the put-back requests are at its front; their ages carried over)
            if (g_deferNavAbort && !any && g_deferHeld == 0 && g_deferHave && !g_broken.load(std::memory_order_relaxed)) {
                DeferAfterTick(self);
                if (!g_deferHave && !g_earlyHave && Rd<int>(self, kRqNum) >= 1) {
                    Inc(g_par.dRestarts);
                    const bool any2 = DeferStart(self);
                    if (g_deferWhy == kWStarted) g_deferWhy = kWRestarted;
                    *handled = g_deferHave;
                    return any2;
                }
                g_deferCarryN = 0;
            }
            *handled = g_deferHave;
            return any;
        }
        // another navigation data ticks: ours must still exist (its paths' NavData resolves to it), else the batch goes
        if (WeakGet(&g_deferNavWeak) != g_earlyNavData) DeferDrop();
        if (g_deferHave) g_deferWhy = kWOtherOut;
        *handled = g_deferHave;
        return false;
    }
    if (!on || g_deferMode.load(std::memory_order_relaxed) == 0 || g_earlyHave || g_broken.load(std::memory_order_relaxed)) return false;
    if (Rd<int>(self, kRqNum) < 1) return false;
    const bool any = DeferStart(self);
    *handled = g_deferHave;
    return any;
}

// This tick's jobs from RepathRequests (game thread): g_jobs / g_jobCount with their stock queries and pins, g_batchEpoch. false
// (nothing built) when the navdata is not a recast one or fewer than `minQueued` repaths are queued.
double g_buildMs = 0;
bool BuildBatch(uint8_t* self, int minQueued)
{
    g_jobCount = 0;
    g_buildMs = 0;
    if (Rd<const uint8_t*>(self, 0x340) != g_staticFind) return false;
    const uint8_t* const rq = Rd<const uint8_t*>(self, 0x3D0);
    int n = Rd<int>(self, 0x3D8);
    if (!rq || n < minQueued) return false;
    if (n > 1000) n = 1000;
    const double t0 = QpcMsNow();
    g_batchEpoch = g_navEpoch.load(std::memory_order_acquire);
    BuildJobs(rq, n, g_jobs, &g_jobCount, n >= g_min.load(std::memory_order_relaxed));
    g_buildMs = QpcMsNow() - t0;
    return true;
}
// Built jobs not run (a TickActor below RepathParallelMin, built only to match its early batch): queries and pins go.
void DropBatch()
{
    for (int k = 0; k < g_jobCount; ++k) {
        Job& j = g_jobs[k];
        if (j.queryBuilt) DestroyQuery(j.query);
        j.queryBuilt = false;
        ReleaseShared(j.pathCtrl);
        j.pathCtrl = nullptr;
    }
    g_jobCount = 0;
}

int DeferUncoveredQueued(const uint8_t* self);
// TickHook's `want` (S-067): RepathParallel's own batch runs at this TickActor when it is on, no deferred search is out on the
// helpers (one claim word), and at least RepathParallelMin queued requests have no deferred result offered.
bool ParallelBatchAllowed(bool on, const uint8_t* self)
{
    if (!on || (g_deferHave && g_deferHeld > 0)) return false;
    const int queued = g_deferHave ? DeferUncoveredQueued(self) : Rd<int>(self, kRqNum);
    return queued >= g_min.load(std::memory_order_relaxed);
}
// RepathDefer (S-067): a deferred result is offered to this TickActor for the queued path `pathObj` (served from it under its key).
bool DeferOffered(const void* pathObj)
{
    for (int i = 0; i < g_earlyCount; ++i) {
        const Job& j = g_early[i];
        if (j.phase == kPhOffer && j.offer && !j.used && j.reqObj == pathObj) return true;
    }
    return false;
}
// The queued requests RepathParallel's batch would search while a deferred batch has offers: non-empty elements whose path has
// no offered result (an emptied, taken-out element is skipped by the stock loop and by BuildJobs alike).
int DeferUncoveredQueued(const uint8_t* self)
{
    const uint8_t* const data = Rd<const uint8_t*>(self, kRqData);
    int num = Rd<int>(self, kRqNum);
    if (!data || num < 1) return 0;
    if (num > 1000) num = 1000;
    int n = 0;
    for (int i = 0; i < num; ++i) {
        const uint8_t* const e = data + static_cast<size_t>(i) * kRqStride;
        const void* const obj = Rd<const void*>(e, 0);
        if (obj && Rd<const void*>(e, 8) && !DeferOffered(obj)) ++n;
    }
    return n;
}

// RepathDefer (S-067): RepathParallel's jobs for paths with a deferred result offered to this TickActor are not searched (the path
// is served from the offer); results are still matched by key in DtFindHook, as before.
void DeferSkipOffered()
{
    for (int i = 0; i < g_jobCount; ++i)
        if (g_jobs[i].valid && DeferOffered(g_jobs[i].pathObj)) { g_jobs[i].valid = false; Inc(g_par.dBatchCovered); }
}

// Runs the batch's jobs (helpers + this thread), waits. Game thread, before the stock TickActor; the jobs are built here, or
// already (`built`: BuildBatch at the early collect). Returns true when there is a batch to serve from.
bool Precompute(uint8_t* self, bool early, bool built)
{
    if (!built && !BuildBatch(self, g_min.load(std::memory_order_relaxed))) return false;
    if (!g_helpersTried) StartHelpers();
    const double t0 = QpcMsNow() - g_buildMs;
    if (early)  // RepathEarly: paths an early search already covers are not searched again
        for (int i = 0; i < g_jobCount; ++i)
            if (g_jobs[i].valid && EarlyCovers(g_jobs[i])) { g_jobs[i].valid = false; Inc(g_par.eCovered); }
    if (g_deferHave) DeferSkipOffered();  // RepathDefer: a path with a deferred result offered now is served from it, not searched again
    // the valid jobs, in queue order, are what the helpers claim (by index into g_order)
    int nv = 0;
    if (!g_broken.load(std::memory_order_relaxed))
        for (int i = 0; i < g_jobCount; ++i)
            if (g_jobs[i].valid) g_order[nv++] = i;
    bool any = false;
    if (nv > 0) {
        ++g_gen;
        g_runJobs = g_jobs;
        g_runOrder = g_order;
        g_runEarly = false;
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


// The stock search (any thread); timed when the stock loop of a navigation TickActor runs it on the game thread (t_inTick).
uint32_t StockFind(uint8_t* q, uint64_t s, uint64_t e, const double* sp, const double* ep, double cost, const uint8_t* filter,
                   uint8_t* result, double* total)
{
    if (!t_inTick) return g_dtFind(q, s, e, sp, ep, cost, filter, result, total);
    const double t0 = QpcMsNow();
    const uint32_t st = g_dtFind(q, s, e, sp, ep, cost, filter, result, total);
    const double ms = QpcMsNow() - t0;
    ++t_stockN;
    t_stockMs += ms;
    if (ms > t_stockMax) t_stockMax = ms;
    return st;
}

// DtFindHook's lookup (game thread, inside a batch's TickActor): this TickActor's RepathParallel batch first (exact key, its
// epoch), then RepathEarly's / RepathDefer's results offered now (exact key), then RepathDefer's put-backs (tolerant key).
// Null: none (the stock search runs). *early: from the early / deferred batch; *tolerant: a put-back within the tolerance.
Job* ServedFor(const Key& k, bool* early, bool* tolerant, double* moved)
{
    Job* hit = nullptr;
    const uint32_t epoch = g_navEpoch.load(std::memory_order_acquire);
    if (g_jobCount > 0) {
        if (epoch != g_batchEpoch) {
            Inc(g_par.epochDrops);
        } else {
            for (int i = 0; i < g_jobCount && !hit; ++i) {
                Job& j = g_jobs[i];
                if (j.valid && j.searched && !j.used && !memcmp(&j.key, &k, sizeof k)) hit = &j;
            }
        }
    }
    // RepathEarly: a search started at the invalidation, on the navmesh as it still is (counter unchanged since its start),
    // that asked no custom link (else not searched), with every input the same
    if (!hit && g_earlyServe) {
        if (epoch != g_earlyEpoch) {
            Inc(g_par.eStale);
        } else {
            for (int i = 0; i < g_earlyCount && !hit; ++i) {
                Job& j = g_early[i];
                if (g_deferHave && !j.offer) continue;  // RepathDefer: only results whose helper is done and that are offered now
                if (j.valid && j.searched && !j.used && !memcmp(&j.key, &k, sizeof k)) hit = &j;
            }
            // RepathDefer: a put-back result, searched up to RepathDeferMaxFrames TickActors ago from the start / end points
            // of then: served when everything else is the same and both points are within RepathDeferTolerance
            for (int i = 0; i < g_earlyCount && !hit && g_deferHave; ++i) {
                Job& j = g_early[i];
                if (j.offer && j.held && j.valid && j.searched && !j.used &&
                    KeyNear(j.key, k, g_deferTolerance.load(std::memory_order_relaxed), moved)) {
                    hit = &j;
                    *tolerant = true;
                }
            }
            *early = hit != nullptr;
        }
    }
    return hit;
}

// The stock loop's findPath calls (game thread, inside a batch's TickActor)
uint32_t DtFindHook(uint8_t* q, uint64_t s, uint64_t e, const double* sp, const double* ep, double cost, const uint8_t* filter,
                    uint8_t* result, double* total)
{
    if (!t_batch) return StockFind(q, s, e, sp, ep, cost, filter, result, total);
    const int mode = g_mode.load(std::memory_order_relaxed);
    Key k;
    const bool keyed = mode > 0 && !total && result && !Rd<void*>(result, 0) && !Rd<int>(result, 8) && !Rd<int>(result, 12) &&
                       MakeKey(&k, q, s, e, sp, ep, cost, filter);
    Job* hit = nullptr;
    bool early = false, tolerant = false;
    double moved = 0;
    if (keyed) hit = ServedFor(k, &early, &tolerant, &moved);
    if (!hit) {
        Inc(g_par.declined);
        return StockFind(q, s, e, sp, ep, cost, filter, result, total);
    }
    hit->used = true;
    if (early && !g_deferHave) Inc(g_par.eServed);
    if (early && g_deferHave) {
        if (!hit->held) Inc(g_par.dGraceServed);
        else if (tolerant) Inc(g_par.dServedTol);
        else Inc(g_par.dServedStrict);
        hit->servedTol = tolerant;
        hit->movedUu = moved;
        const int64_t mu = static_cast<int64_t>(moved * 1000.0);
        if (mu > g_par.dMaxMoveMu.load(std::memory_order_relaxed)) g_par.dMaxMoveMu.store(mu, std::memory_order_relaxed);
    }
    if (hit->links.n > 0) {
        // the custom links the helper's search asked: the stock search would ask them now (repath130l)
        if (!LinksSame(hit->links, Rd<const void*>(q, 8))) {
            Inc(g_par.linkChanged);
            Inc(g_par.declined);
            return StockFind(q, s, e, sp, ep, cost, filter, result, total);
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

// TickHook's `want` and the step before Precompute: a live early batch (another navigation data's RepathEarly batch, or this one's
// own deferred batch with nothing held, whose searches have all ended) is collected first, so the helpers are free and
// Precompute's new generation never shares the claim word with it.
bool TickBatchWanted(bool on, const uint8_t* self)
{
    const bool want = ParallelBatchAllowed(on, self);
    if (want && g_earlyLive.load(std::memory_order_acquire)) {
        AcquireSRWLockExclusive(&g_earlyLock);
        EarlyCollectLocked(false);
        ReleaseSRWLockExclusive(&g_earlyLock);
    }
    return want;
}

void TickHook(uint8_t* self, float dt, int type, void* tf)
{
    const double t0 = QpcMsNow();
    const bool on = g_mode.load(std::memory_order_relaxed) > 0 && !g_broken.load(std::memory_order_relaxed) && !t_batch && g_isGt();
    g_deferWhy = kWNone;
    // RepathEarly: this navigation data's early batch is collected and offered to this TickActor while the navmesh is unchanged
    // since it started. Decided before the wait (S1): a counter that moved or a key switched off stops the whole batch; else
    // this tick's jobs are built first and the early searches none of them matches are stopped; the game thread then waits
    // only for what is left of the matched searches (stopped ones drain at once).
    bool early = false, built = false;
    double earlyWait = 0;
    int earlySearched = 0;
    // RepathDefer: a kept batch is resumed (put-backs) or a new one started; while one of its searches is out on the helpers
    // RepathParallel's own batch and RepathEarly stay out (one batch on the helpers at a time): this TickActor's other requests are
    // searched by the stock loop. With nothing out (S-067: a start handed back, or a batch whose requests are all back),
    // RepathParallel's batch runs as with RepathDefer off, minus the paths a deferred result is offered for.
    bool deferBatch = false, deferServe = false;
    if (!t_batch && g_isGt()) deferServe = DeferTick(self, on, &deferBatch);
    const bool deferTick = deferBatch || deferServe;
    if (!deferTick && g_earlyHave && g_earlyNavData == self && !t_batch && g_isGt()) {
        const bool usable = EarlyUsable(on);
        if (usable) built = BuildBatch(self, 1);
        earlyWait = EarlyTickCollect(usable);
        AddUs(g_par.eWaitUs, earlyWait);
        const int64_t waitUs = static_cast<int64_t>(earlyWait * 1000.0);
        if (waitUs > g_par.eMaxWaitUs.load(std::memory_order_relaxed)) g_par.eMaxWaitUs.store(waitUs, std::memory_order_relaxed);
        for (int i = 0; i < g_earlyCount; ++i) earlySearched += g_early[i].valid && g_early[i].searched;
        early = usable && earlySearched > 0 && g_navEpoch.load(std::memory_order_acquire) == g_earlyEpoch;
        if (!early) {
            if (earlySearched > 0) Inc(g_par.eStale);
            EarlyEnd(false);
        }
    }
    const bool want = TickBatchWanted(on, self);
    if (built && !want) {  // built only to match the early batch (fewer than RepathParallelMin queued): not run
        DropBatch();
        built = false;
    }
    const bool batch = want && Precompute(self, early, built);
    g_earlyServe = early || deferServe;
    t_batch = batch || early || deferServe;
    const bool timed = g_deferInstalled && g_isGt();
    if (timed) {
        t_inTick = true;
        t_stockN = 0;
        t_stockMs = t_stockMax = 0;
    }
    g_tick(self, dt, type, tf);
    t_inTick = false;
    t_batch = false;
    g_earlyServe = false;
    if (deferServe || (g_deferHave && g_earlyNavData == self)) DeferAfterTick(self);
    const double ms = QpcMsNow() - t0;
    if (timed && t_stockN > 0) {
        Inc(g_par.sStockN, t_stockN);
        AddUs(g_par.sStockUs, t_stockMs);
        const int64_t mu = static_cast<int64_t>(t_stockMax * 1000.0);
        if (mu > g_par.sStockMaxUs.load(std::memory_order_relaxed)) g_par.sStockMaxUs.store(mu, std::memory_order_relaxed);
    }
    if (early) {
        EarlyEnd(true);
    }
    if (batch) {
        FinishBatch();
    }
    (void)ms;
}

void InvalHook(uint8_t* self, const uint8_t* tiles)
{
    g_navEpoch.fetch_add(1, std::memory_order_acq_rel);
        g_inval(self, tiles);
    EarlyStart(self);  // RepathEarly: the paths it just queued start searching now (no-op when off)
}
// RepathEarly's fence: the navmesh's memory writers wait for an early search on that navmesh (aborted) before they run.
uint32_t AddTileHook(void* nav, uint8_t* data, int size, int flags, uint64_t lastRef, uint64_t* result)
{
    WriterEnter(nav, true);
    const uint32_t r = g_addTile(nav, data, size, flags, lastRef, result);
    WriterLeave();
    return r;
}
uint32_t RemoveTileHook(void* nav, uint64_t ref, uint8_t** data, int* size)
{
    WriterEnter(nav, true);
    const uint32_t r = g_removeTile(nav, ref, data, size);
    WriterLeave();
    return r;
}
void FreeNavHook(void* nav)  // replaces dtFreeNavMesh's null check; g_freeNavBody is the rest of it
{
    if (!nav) return;
    WriterEnter(nav, true);
    g_freeNavBody(nav);
    WriterLeave();
}
// In-place writers (nav-link area / flag updates: RepathParallel's R-S1; ReplaceAreaInTileBounds: RepathEarly's S2): the counter.
void OffMeshHook(void* nav, uint64_t userId, uint8_t area, uint16_t flags)
{
    WriterEnter(nav, false);
    g_offMesh(nav, userId, area, flags);
    WriterLeave();
}
void OffMeshSegHook(void* nav, uint64_t userId, uint8_t area, uint16_t flags)
{
    WriterEnter(nav, false);
    g_offMeshSeg(nav, userId, area, flags);
    WriterLeave();
}
int32_t ReplaceAreaHook(void* self, const void* bounds, void* oldArea, void* newArea, bool replaceLinks, void* outTouched)
{
    WriterEnter(nullptr, false);
    const int32_t r = g_replaceArea(self, bounds, oldArea, newArea, replaceLinks, outTouched);
    WriterLeave();
    return r;
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

// RepathEarly's targets: the navmesh's memory writers (the fence) and the free's caller that ties it to impl +8 (the navmesh
// every search uses: Precompute reads it there).
struct EarlyTargets { uint8_t *addTile, *removeTile, *freeNav, *releaseNav, *replaceArea; };
bool DiscoverEarly(const GameModule& game, const Targets& t, EarlyTargets* e, const char** why)
{
    memset(e, 0, sizeof *e);
    const auto Fail = [why](const char* w) { *why = w; return false; };
    e->addTile = One(game, kAddTile);
    e->removeTile = One(game, kRemoveTile);
    e->freeNav = One(game, kFreeNav);
    uint8_t* const rel = One(game, kReleaseNav);
    if (!e->addTile || !e->removeTile || !e->freeNav || !rel) return Fail("dtNavMesh's addTile / removeTile / dtFreeNavMesh not found once");
    // each a function's own start (a unique match inside another function after a game update must not be hooked: N1)
    if (PrimaryFunction(game, e->addTile) != e->addTile || PrimaryFunction(game, e->removeTile) != e->removeTile ||
        PrimaryFunction(game, e->freeNav) != e->freeNav)
        return Fail("dtNavMesh's addTile / removeTile / dtFreeNavMesh is not a function start");
    // the null check the dtFreeNavMesh hook replaces jumps to a plain ret (any work on the null path: not hooked, N2)
    const uint8_t* const nullPath = RipTarget(e->freeNav + 3, 2, 6);
    if (!game.text.Contains(nullPath) || *nullPath != 0xC3) return Fail("dtFreeNavMesh's null path is not a plain return");
    // ARecastNavMesh::ReplaceAreaInTileBounds: in-place area writes (S2)
    e->replaceArea = One(game, kReplaceArea);
    if (!e->replaceArea || PrimaryFunction(game, e->replaceArea) != e->replaceArea || !OneIn(game, e->replaceArea, 0x300, kReplaceAreaWrite))
        return Fail("ARecastNavMesh::ReplaceAreaInTileBounds not found once");
    e->releaseNav = PrimaryFunction(game, rel);
    if (CallAt(game, rel + kReleaseNavCall) != e->freeNav || !e->releaseNav) return Fail("ReleaseDetourNavMesh does not free impl +8 through dtFreeNavMesh");
    // the slots the early copies wrap: FRecastQueryFilter's dtQueryFilter vtable has 4 (dtor, passVirtualFilter, 2 x getVirtualCost),
    // the link filter's 4 (its slots 2 / 3 checked by Discover)
    const auto* const rvt = static_cast<const void* const*>(t.rqfVtbl);
    for (int i = 0; i < 4; ++i)
        if (!game.text.Contains(rvt[i]) || !game.text.Contains(t.lfVtbl[i])) return Fail("a filter vtable slot is not code");
    return true;
}

// RepathDefer's targets: the stock loop's Add of a request (ResizeGrow, element layout) and its skip of an empty element.
bool DiscoverDefer(const GameModule& game, uint8_t** resizeGrow, const char** why)
{
    *resizeGrow = nullptr;
    uint8_t* const f = One(game, kTickF);
    if (!f) { *why = "TickActor's loop (F) not found once"; return false; }
    if (!At(f + kTickAddAt, kTickAdd)) { *why = "TickActor's RepathRequests.Add does not match"; return false; }
    if (!At(f - kTickSkipBeforeF, kTickSkip)) { *why = "TickActor's skip of an empty request does not match"; return false; }
    uint8_t* const rg = CallAt(game, f + kTickAddAt + kTickAddCall);
    if (!rg || !At(rg, kResizeGrowPro) || !OneIn(game, rg, 0x100, kResizeGrowElem)) { *why = "RepathRequests' ResizeGrow does not match"; return false; }
    *resizeGrow = rg;
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
// A detour whose hook never returns into the copied bytes (dtFreeNavMesh: its first instructions are a null check with a
// relative jump; the hook does the check and calls the code after it): only the jump to the hook.
bool BuildHookOnlySlot(uint8_t* fn, const Pattern& expect, void* hook, uint8_t* slot)
{
    const size_t n = expect.bytes.size();
    if (n < 5 || n > 24 || !MatchesAt(fn, expect)) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memset(slot, 0xCC, 64);
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

// Monitor thread, every second: RepathEarly (live once installed; acts only while RepathParallel is on).
void SetRepathEarly(int key)
{
    if (!g_earlyInstalled) return;
    const int m = key != 0 ? 1 : 0;
    if (m != g_earlyMode.load()) {
        Log("[repath] RepathEarly %d -> %d", g_earlyMode.load(), m);
        g_earlyMode = m;
    }
}

// Monitor thread, every second: RepathDefer (live once installed; acts only while RepathParallel is on).
void SetRepathDefer(int key, int graceUs, int maxFrames, int toleranceUu)
{
    if (!g_deferInstalled) return;
    const int m = key != 0 ? 1 : 0;
    const int g = graceUs < 0 ? 0 : graceUs > 4000 ? 4000 : graceUs;
    const int f = maxFrames < 1 ? 1 : maxFrames > 60 ? 60 : maxFrames;
    const int t = toleranceUu < 0 ? 0 : toleranceUu > 500 ? 500 : toleranceUu;
    if (m != g_deferMode.load() || g != g_deferGraceUs.load() || f != g_deferMaxFrames.load() || t != g_deferTolerance.load()) {
        Log("[repath] RepathDefer %d -> %d (grace %d us, at most %d TickActors out, tolerance %d uu)", g_deferMode.load(), m, g, f, t);
        g_deferMode = m;
        g_deferGraceUs = g;
        g_deferMaxFrames = f;
        g_deferTolerance = t;
    }
}

// Stats interval: one line when something happened.
void LogRepathParallelStats()
{
    if (!g_dtFind) return;
    ParSum& s = g_par;
    const auto X = [](std::atomic<int>& a) { return a.exchange(0, std::memory_order_relaxed); };
    const auto XU = [](std::atomic<int64_t>& a) { return static_cast<double>(a.exchange(0, std::memory_order_relaxed)) / 1000.0; };
    if (g_deferInstalled && (s.dStarts.load(std::memory_order_relaxed) || s.dPutBack.load(std::memory_order_relaxed) ||
                             s.dDropped.load(std::memory_order_relaxed))) {
        const int st = X(s.dStarts), jb = X(s.dJobs), gd = X(s.dGraceDone), gs = X(s.dGraceServed), hd = X(s.dHeld), pb = X(s.dPutBack),
                  ss = X(s.dServedStrict), stl = X(s.dServedTol), pm = X(s.dPutBackMissed), ab = X(s.dAborted), ls = X(s.dLinkStop),
                  dl = X(s.dDeadline), dr = X(s.dDropped), du = X(s.dDup), ov = X(s.dOverlap), a1 = X(s.dAge[0]), a2 = X(s.dAge[1]),
                  a3 = X(s.dAge[2]), a46 = X(s.dAge[3]), a7 = X(s.dAge[4]), ps = X(s.dProofSame), pd = X(s.dProofDiff),
                  hs = X(s.dHeldShort), rs = X(s.dRestarts), hb = X(s.dHandBack), hbj = X(s.dHandBackJobs), hbl = X(s.dHandBackLinked),
                  ll = X(s.dLinkLeft), np = X(s.dNoPrep), ws = X(s.dWriterStart), bc = X(s.dBatchCovered), mf = X(s.dMissFresh),
                  mq = X(s.dMissQueued), mn = X(s.dMissNoCall), mk = X(s.dMissKey), sn = X(s.sStockN);
        const double gUs = XU(s.dGraceUs), gMax = XU(s.dMaxGraceUs), dUs = XU(s.dDeadlineUs), mv = XU(s.dMaxMoveMu),
                     wUs = XU(s.dWakeUs), wMax = XU(s.dMaxWakeUs), sUs = XU(s.sStockUs), sMax = XU(s.sStockMaxUs);
        (void)ps;
        (void)pd;
        (void)hs;
        (void)rs;
        (void)wUs;
        (void)wMax;
        (void)hbj;
        (void)np;
        (void)ws;
        (void)bc;
        (void)mf;
        (void)mq;
        (void)mn;
        (void)mk;
        Log("[repath] deferred: %d starts, %d searches; %d done at once (%d served); %d put off and given back after 1/2/3/4-6/more frames "
            "%d/%d/%d/%d/%d (%d), served %d on identical inputs and %d within the tolerance (points moved up to %.1f uu), %d not served; "
            "given back unsearched: %d (navmesh changed or off), %d (nav link); waited at the bound %d (%.2f ms); dropped %d; queued twice %d; "
            "searched as usual while one was out %d; game thread waited %.2f ms (max %.3f); %d starts left to the parallel search "
            "(counted in the starts and searches above; %d of their searches stopped at a nav link), %d searches through nav links left to the game thread; game thread searches in navigation ticks %d "
            "(%.1f ms, longest %.2f)",
            st, jb, gd, gs, hd, a1, a2, a3, a46, a7, pb, ss, stl, mv, pm, ab, ls, dl, dUs, dr, du, ov, gUs, gMax, hb, hbl, ll, sn, sUs, sMax);
    } else {  // stock-loop time of windows without a RepathDefer start is not carried into the next line
        s.sStockN.store(0, std::memory_order_relaxed);
        s.sStockUs.store(0, std::memory_order_relaxed);
        s.sStockMaxUs.store(0, std::memory_order_relaxed);
    }
    if ((g_earlyInstalled || g_deferInstalled) && (s.eBatches.load(std::memory_order_relaxed) || s.eFences.load(std::memory_order_relaxed) ||
                             s.eOffGt.load(std::memory_order_relaxed) || s.eWriterAtStart.load(std::memory_order_relaxed))) {
        const int eb = X(s.eBatches), ej = X(s.eJobs), es = X(s.eSearched), ev = X(s.eServed), ec = X(s.eCovered), el = X(s.eLinked),
                  et = X(s.eStale), eu = X(s.eUnused), ef = X(s.eFences), er = X(s.eRestarts), eta = X(s.eTickAborts), ew = X(s.eWriterAtStart),
                  eun = X(s.eUncovered), eo = X(s.eOffGt), eol = X(s.eOffGtLive);
        const double eSearch = XU(s.eSearchUs), eWait = XU(s.eWaitUs), eMax = XU(s.eMaxWaitUs), eFence = XU(s.eFenceUs), eGt = XU(s.eGtUs);
        Log("[repath] early: %d batches at nav changes, %d searches (%.1f ms on helpers; prepared on the game thread in %.2f ms), "
            "%d searched, served %d (covered %d), not served %d, navmesh changed first %d, through a nav link %d; stopped at TickActor: "
            "%d not matched, %d batches whole (nav changed / off); batches ended by a navmesh-writer fence %d, by the next change %d; "
            "not started: by a writer at the start %d; game thread waited %.2f ms at TickActor (max %.2f), %.2f ms in fences; writers off the game "
            "thread %d (%d during a batch)",
            eb, ej, eSearch, eGt, es, ev, ec, eu, et, el, eun, eta, ef, er, ew, eWait, eMax, eFence, eo, eol);
    }
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
bool InstallRepathParallel(bool beforeGameCode, int key, int minQueued, int threads, bool helpersAllowed, bool labProbe, int earlyKey,
                           int deferKey)
{
    (void)labProbe;
    const bool none = !key;
    if (none) {
        Log("[repath] RepathParallel off in OblivionPerfFix.ini (takes a restart to switch on)%s",
            earlyKey || deferKey ? "; RepathEarly / RepathDefer need it: not installed either" : "");
        return false;
    }
    if (!beforeGameCode) { Log("[repath] RepathParallel: start-up only (loaded late here), not installed"); return false; }
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    g_qpcMs = fq.QuadPart ? 1000.0 / static_cast<double>(fq.QuadPart) : 0.0;
    const GameModule& game = Game();
    Targets t{};
    const char* why = "";
    if (!Discover(game, &t, &why)) { Log("[repath] RepathParallel: %s: not installed", why); return false; }
    g_t = t;
    // RepathEarly (start-up key): the navmesh writers' fence, patched first (no early batch can start before it is in)
    // RepathDefer (start-up key) uses the same fence and early copies; the request handling needs its own pins
    EarlyTargets et{};
    bool early = false, defer = false;
    uint8_t* resizeGrow = nullptr;
    if (earlyKey || deferKey) {
        const char* ewhy = "";
        early = DiscoverEarly(game, t, &et, &ewhy);
        if (!early) Log("[repath] RepathEarly / RepathDefer: %s: not installed (RepathParallel goes on without them)", ewhy);
    }
    if (!earlyKey) Log("[repath] RepathEarly off in OblivionPerfFix.ini (takes a restart to switch on)");
    if (deferKey && early) {
        const char* dwhy = "";
        defer = DiscoverDefer(game, &resizeGrow, &dwhy);
        if (!defer) Log("[repath] RepathDefer: %s: not installed (RepathParallel goes on without it)", dwhy);
    } else if (!deferKey) {
        Log("[repath] RepathDefer off in OblivionPerfFix.ini (takes a restart to switch on)");
    }
    struct Site { uint8_t* fn; const char* pro; void* hook; void** orig; const char* name; bool hookOnly; };
    Site sites[10] = {};
    size_t kN = 0;
    if (early) {
        sites[kN++] = {et.addTile, kAddTilePro, reinterpret_cast<void*>(&AddTileHook), reinterpret_cast<void**>(&g_addTile), "dtNavMesh::addTile", false};
        sites[kN++] = {et.removeTile, kRemoveTilePro, reinterpret_cast<void*>(&RemoveTileHook), reinterpret_cast<void**>(&g_removeTile),
                       "dtNavMesh::removeTile", false};
        sites[kN++] = {et.freeNav, kFreeNavPro, reinterpret_cast<void*>(&FreeNavHook), nullptr, "dtFreeNavMesh", true};
        sites[kN++] = {et.replaceArea, kReplaceAreaPro, reinterpret_cast<void*>(&ReplaceAreaHook), reinterpret_cast<void**>(&g_replaceArea),
                       "ReplaceAreaInTileBounds", false};
    }
    sites[kN++] = {t.tick, kTickPro, reinterpret_cast<void*>(&TickHook), reinterpret_cast<void**>(&g_tick), "TickActor", false};
    sites[kN++] = {t.inval, kInvalPro, reinterpret_cast<void*>(&InvalHook), reinterpret_cast<void**>(&g_inval), "InvalidateAffectedPaths", false};
    sites[kN++] = {t.offMesh, kOffMeshPro, reinterpret_cast<void*>(&OffMeshHook), reinterpret_cast<void**>(&g_offMesh), "updateOffMeshConnectionByUserId", false};
    sites[kN++] = {t.offMeshSeg, kOffMeshSegPro, reinterpret_cast<void*>(&OffMeshSegHook), reinterpret_cast<void**>(&g_offMeshSeg),
                   "updateOffMeshSegmentConnectionByUserId", false};
    sites[kN++] = {t.dtFind, kFindPathPro, reinterpret_cast<void*>(&DtFindHook), reinterpret_cast<void**>(&g_dtFind), "findPath", false};
    uint8_t* const region = AllocNearGame(kN * kSlot);
    if (!region) { Log("[repath] RepathParallel: no memory near the game's code: not installed"); return false; }
    for (size_t i = 0; i < kN; ++i) {
        const Pattern pro = ParsePattern(sites[i].pro);
        if (sites[i].hookOnly ? !BuildHookOnlySlot(sites[i].fn, pro, sites[i].hook, region + i * kSlot)
                              : !BuildSlot(sites[i].fn, pro, sites[i].hook, region + i * kSlot)) {
            Log("[repath] RepathParallel: %s's entry does not match: not installed", sites[i].name);
            return false;
        }
    }
    if (early) {
        g_freeNavBody = reinterpret_cast<FreeNavFn>(et.freeNav + kFreeNavSkip);
        const auto* const rvt = static_cast<const void* const*>(t.rqfVtbl);
        for (int i = 0; i < 4; ++i) { g_earlyFVt[i] = rvt[i]; g_earlyLVt[i] = t.lfVtbl[i]; }
        g_passVirtual = reinterpret_cast<PassFilterFn>(const_cast<void*>(rvt[1]));
        g_earlyFVt[1] = reinterpret_cast<const void*>(&EarlyPassFilter);
        g_earlyLVt[2] = reinterpret_cast<const void*>(&EarlyLinkAllowed);
        g_earlyLVt[3] = reinterpret_cast<const void*>(&EarlyLinkInit);
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
    for (size_t i = 0; i < kN; ++i)
        if (sites[i].orig) *sites[i].orig = region + i * kSlot;
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
    if (defer) {
        g_resizeGrow = reinterpret_cast<ResizeGrowFn>(resizeGrow);
        g_deferMode = 1;
        g_deferInstalled = true;
        Log("[repath] RepathDefer installed: a TickActor's queued repaths start on the helpers; what is not done after the grace is "
            "taken out of RepathRequests and given back when its search is done (RepathRequests' grow exe+0x%X); live",
            static_cast<unsigned>(resizeGrow - game.base));
    }
    if (early && earlyKey) {
        g_earlyMode = 1;
        g_earlyInstalled = true;
        Log("[repath] RepathEarly installed: a navigation change's queued repaths start on the helpers at once, served at TickActor "
            "on identical inputs; navmesh writers fenced: addTile exe+0x%X, removeTile exe+0x%X, dtFreeNavMesh exe+0x%X; area writer "
            "ReplaceAreaInTileBounds exe+0x%X counted; live",
            static_cast<unsigned>(et.addTile - game.base), static_cast<unsigned>(et.removeTile - game.base), static_cast<unsigned>(et.freeNav - game.base),
            static_cast<unsigned>(et.replaceArea - game.base));
    }
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

// ---- RepathEarly self-tests ----
// Discovery on the mapped exe: addTile, removeTile, dtFreeNavMesh, ReleaseDetourNavMesh, ReplaceAreaInTileBounds (5 RVAs). 1 = found.
int RepathEarlyTestDiscover(const GameModule& game, uint32_t* rvas, const char** why)
{
    Targets t{};
    if (!Discover(game, &t, why)) return 0;
    EarlyTargets e{};
    if (!DiscoverEarly(game, t, &e, why)) return 0;
    uint8_t* const list[] = {e.addTile, e.removeTile, e.freeNav, e.releaseNav, e.replaceArea};
    for (size_t i = 0; i < 5; ++i) rvas[i] = static_cast<uint32_t>(list[i] - game.base);
    // the entries the detours overwrite
    return At(e.addTile, kAddTilePro) && At(e.removeTile, kRemoveTilePro) && At(e.freeNav, kFreeNavPro) && At(e.replaceArea, kReplaceAreaPro) ? 1 : 0;
}

namespace {
int g_fakePassCalls = 0;
bool FakePass(const void*, uint64_t, const void*, const void*) { ++g_fakePassCalls; return true; }
}  // namespace

// The early copies: the filter's slot 1 passes through until the search is dropped or aborted; the link filter's question drops
// the search without a game call; both copies key as the stock filters (the filter by its stock pointer).
bool RepathEarlyTestCopies()
{
    const PassFilterFn savedPass = g_passVirtual;
    g_passVirtual = &FakePass;
    g_fakePassCalls = 0;
    // the filter copy reads its own job's flag (self + kFilterBytes); the link question sets it on the job named in the link filter
    Job& j = g_early[3];
    Job& other = g_early[4];
    j.stop.store(0);
    j.linkAsked = false;
    other.stop.store(0);
    alignas(8) uint8_t elf[kEarlyLfJob + 8] = {};
    Job* const jp = &j;
    memcpy(elf + kEarlyLfJob, &jp, 8);
    bool ok = EarlyPassFilter(j.filterCopy, 1, nullptr, nullptr) && g_fakePassCalls == 1;
    ok = ok && EarlyLinkAllowed(elf, 7) && j.linkAsked && j.stop.load() == 1;  // the question: dropped (no game call)
    ok = ok && !EarlyPassFilter(j.filterCopy, 1, nullptr, nullptr) && g_fakePassCalls == 1;
    ok = ok && EarlyPassFilter(other.filterCopy, 1, nullptr, nullptr) && g_fakePassCalls == 2 && !other.linkAsked;  // another job runs on
    j.stop.store(0);
    j.linkAsked = false;
    other.stop.store(1);  // stopped (TickActor or a fence)
    ok = ok && !EarlyPassFilter(other.filterCopy, 1, nullptr, nullptr) && EarlyPassFilter(j.filterCopy, 1, nullptr, nullptr) && g_fakePassCalls == 3;
    other.stop.store(0);
    g_passVirtual = savedPass;
    // keys: the early link filter / filter copy against the stock ones
    alignas(16) static uint8_t q[0xC0], pool[0x30], lf[0x20], filter[kFilterBytes], copy[kFilterBytes];
    memset(q, 0, sizeof q);
    memset(pool, 0, sizeof pool);
    memset(lf, 0, sizeof lf);
    memset(filter, 0, sizeof filter);
    static const void* const fakeLfVt[4] = {};
    static const void* const fakeRqf[4] = {};
    const void* const savedLf = g_lfVtbl, *const savedRqf = g_rqfVtbl;
    g_lfVtbl = fakeLfVt;
    g_rqfVtbl = fakeRqf;
    const void* nav = q;
    const void* lfp = lf;
    const void* pp = pool;
    memcpy(q, &nav, 8);
    memcpy(q + 8, &lfp, 8);
    memcpy(q + 0x88, &pp, 8);
    *reinterpret_cast<int*>(pool + 0x20) = 2048;
    const void* rv = fakeRqf;
    memcpy(filter, &rv, 8);
    filter[0x30] = 3;
    filter[kFilterVirtual] = 1;
    const double sp[3] = {1, 2, 3}, ep[3] = {4, 5, 6};
    const void* lv = fakeLfVt;
    memcpy(lf, &lv, 8);
    Key a, b;
    ok = ok && MakeKey(&a, q, 11, 22, sp, ep, 1e30, filter);
    memcpy(copy, filter, kFilterBytes);
    const void* ev = g_earlyFVt;
    memcpy(copy, &ev, 8);
    lv = g_earlyLVt;
    memcpy(lf, &lv, 8);
    ok = ok && MakeKey(&b, q, 11, 22, sp, ep, 1e30, copy, filter) && !memcmp(&a, &b, sizeof a);
    copy[0x30] = 4;  // a filter byte differs: another key
    ok = ok && MakeKey(&b, q, 11, 22, sp, ep, 1e30, copy, filter) && memcmp(&a, &b, sizeof a) != 0;
    g_lfVtbl = savedLf;
    g_rqfVtbl = savedRqf;
    return ok;
}

// The serve decision for an early search, through the real findPath detour with a fake stock findPath. `which`: 0 served;
// 1 the nav counter moved since the early start -> declined; 2 another end point -> declined; 3 the search asked a custom link
// (not searched) -> declined; 4 the TickActor was not offered the early batch -> declined. 1 = the expected outcome.
int RepathEarlyTestServe(int which)
{
    alignas(16) static uint8_t q[0xC0], pool[0x30], lf[0x20], filter[kFilterBytes];
    memset(q, 0, sizeof q);
    memset(pool, 0, sizeof pool);
    memset(lf, 0, sizeof lf);
    memset(filter, 0, sizeof filter);
    static const void* const fakeLfVt[4] = {};
    static const void* const fakeRqf[4] = {};
    const void* const savedLf = g_lfVtbl, *const savedRqf = g_rqfVtbl;
    const DtFindFn savedFind = g_dtFind;
    const int savedMode = g_mode.load(), savedJobs = g_jobCount, savedEarly = g_earlyCount;
    const uint32_t savedEpoch = g_earlyEpoch;
    g_lfVtbl = fakeLfVt;
    g_rqfVtbl = fakeRqf;
    g_dtFind = &FakeStockFind;
    g_mode = 1;
    g_jobCount = 0;
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
    const double sp[3] = {1, 2, 3}, ep[3] = {4, 5, 6}, ep2[3] = {4, 5, 7};
    static uint8_t resultItems[48] = {};
    Job& j = g_early[0];
    j.valid = true;
    j.searched = which != 3;
    j.used = false;
    j.status = 0x40000000u;
    j.size = j.cap = 1;
    j.data = resultItems;
    j.queryNodes = 23;
    LinkRecClear(&j.links);
    bool ok = MakeKey(&j.key, q, 11, 22, sp, ep, 1e30, filter);
    g_earlyCount = 1;
    g_earlyEpoch = g_navEpoch.load();
    if (which == 1) g_navEpoch.fetch_add(1);
    uint8_t result[16] = {};
    const int served0 = g_par.served.load(), early0 = g_par.eServed.load();
    g_earlyServe = which != 4;
    t_batch = true;
    const uint32_t st = DtFindHook(q, 11, 22, sp, which == 2 ? ep2 : ep, 1e30, filter, result, nullptr);
    t_batch = false;
    g_earlyServe = false;
    uint8_t* got = nullptr;
    memcpy(&got, result, 8);
    const bool served = st == 0x40000000u && got == resultItems && Rd<int>(result, 8) == 1 && Rd<int>(q, 0x98) == 23 &&
                        g_par.served.load() == served0 + 1 && g_par.eServed.load() == early0 + 1;
    const bool declined = st == kFakeStockStatus && got == nullptr && g_par.served.load() == served0 && g_par.eServed.load() == early0;
    ok = ok && (which == 0 ? served : declined);
    j.data = nullptr;  // a static buffer
    j.valid = j.searched = false;
    g_earlyCount = savedEarly;
    g_earlyEpoch = savedEpoch;
    g_jobCount = savedJobs;
    g_mode = savedMode;
    g_dtFind = savedFind;
    g_lfVtbl = savedLf;
    g_rqfVtbl = savedRqf;
    return ok ? 1 : 0;
}

// The fence on a published early batch with no helper running (none started in the self-test): `which` 0: a writer on another
// navmesh only moves the counter; 1: a writer on its navmesh cancels every unclaimed search, aborts, leaves it collected;
// 2: one search already claimed and finished, the other unclaimed: only the unclaimed one is cancelled. 1 = as expected.
int RepathEarlyTestFence(int which)
{
    static int navA = 0, navB = 0;
    const uint32_t gen0 = g_gen;
    for (int i = 0; i < 2; ++i) {
        Job& j = g_early[i];
        j.valid = true;
        j.searched = true;
        j.nav = &navA;
        j.state.store(0);
        g_earlyOrder[i] = i;
    }
    g_earlyN = 2;
    g_earlyCount = 2;
    ++g_gen;
    const uint32_t next = which == 2 ? 1u : 0u;
    if (which == 2) g_early[0].state.store(1);  // claimed and done
    g_word.store((static_cast<uint64_t>(g_gen) << 32) | (static_cast<uint64_t>(next) << 16) | 2u);
    for (int i = 0; i < 2; ++i) g_early[i].stop.store(0);
    g_earlyLive = true;
    const uint32_t e0 = g_navEpoch.load();
    const int w0 = g_writers.load();
    WriterEnter(which == 0 ? &navB : &navA, true);
    bool ok = g_navEpoch.load() == e0 + 1 && g_writers.load() == w0 + 1;
    WriterLeave();
    ok = ok && g_writers.load() == w0;
    if (which == 0) {
        ok = ok && g_earlyLive.load() && !g_early[0].stop.load() && !g_early[1].stop.load() && g_early[0].state.load() == 0 &&
             g_early[1].state.load() == 0;
        AcquireSRWLockExclusive(&g_earlyLock);
        EarlyCollectLocked(false);  // cancels both (no helper claims in the self-test)
        ReleaseSRWLockExclusive(&g_earlyLock);
        ok = ok && !g_earlyLive.load() && g_early[0].state.load() == 1 && g_early[1].state.load() == 1;
    } else {
        ok = ok && !g_earlyLive.load() && g_early[0].stop.load() && g_early[1].stop.load() && g_early[1].state.load() == 1 && !g_early[1].searched &&
             g_early[0].state.load() == 1 && g_early[0].searched == (which == 2);
    }
    ok = ok && (g_word.load() & 0xFFFF) == 0;  // the claim word idle again
    for (int i = 0; i < 2; ++i) { g_early[i].valid = g_early[i].searched = false; g_early[i].nav = nullptr; }
    g_earlyN = g_earlyCount = 0;
    for (int i = 0; i < 2; ++i) g_early[i].stop.store(0);
    (void)gen0;
    return ok ? 1 : 0;
}

// repathfix S1 / N9 / S2 / S3 self-tests: two early jobs and one TickActor job with settable inputs (static buffers).
namespace {
alignas(16) uint8_t g_tfA[kFilterBytes], g_tfB[kFilterBytes];
int g_tImpl = 0, g_tNav = 0;
void TestEarlyInputs(Job& j, double startX, const uint8_t* filter)
{
    j.impl = reinterpret_cast<const uint8_t*>(&g_tImpl);
    j.nav = &g_tNav;
    j.maxNodes = 2048;
    j.require = 1;
    j.start[0] = startX;
    j.start[1] = 2;
    j.start[2] = 3;
    j.adjEnd[0] = 4;
    j.adjEnd[1] = 5;
    j.adjEnd[2] = 6;
    j.cost = 1e30;
    memset(j.linkFilter, 0, sizeof j.linkFilter);
    j.linkFilter[8] = 9;
    j.dtFilter = filter;
    j.origFilter = filter;
    memcpy(j.filterCopy, filter, kFilterBytes);
    const void* const fvt = g_earlyFVt;
    memcpy(j.filterCopy, &fvt, 8);  // the copy's own vtable (slot 1 wrapped); compared from byte 8
}
// A fake helper for the TickActor tests: early job k finishes when its stop flag is set (a stopped search drains at once) or
// after its own search time, whichever comes first; searched = not stopped.
struct FakeHelperArg { int ms[2]; };
DWORD WINAPI FakeEarlyHelper(void* p)
{
    const auto* const a = static_cast<const FakeHelperArg*>(p);
    const ULONGLONG t0 = GetTickCount64();
    bool done[2] = {};
    for (int left = 2; left > 0;) {
        for (int k = 0; k < 2; ++k) {
            if (done[k]) continue;
            Job& j = g_early[k];
            const bool stopped = j.stop.load(std::memory_order_acquire) != 0;
            if (stopped || GetTickCount64() - t0 >= static_cast<ULONGLONG>(a->ms[k])) {
                j.searched = !stopped;
                j.state.store(1, std::memory_order_release);
                WakeWordAll(&j.state);
                done[k] = true;
                --left;
            }
        }
        Sleep(1);
    }
    return 0;
}
int g_fakeReplaceCalls = 0;
int32_t FakeReplaceArea(void*, const void*, void*, void* newArea, bool, void*)
{
    ++g_fakeReplaceCalls;
    return newArea ? 7 : 0;
}
}  // namespace

// TickActor's decision before its wait (S1), with a fake helper running both early searches (already claimed). `which`:
// 0 early search 1 matches no queued path (its querier moved): it is stopped and not waited for, search 0 (matched) is;
// 1 the nav counter moved since the early start: both stopped; 2 RepathEarly switched off live: both stopped;
// 3 RepathParallel off (TickActor's `on` false): both stopped. 1 = as expected (nothing waits for a 5 s search).
int RepathEarlyTestTick(int which)
{
    const int savedEarlyMode = g_earlyMode.load(), savedJobs = g_jobCount;
    const uint32_t savedEpoch = g_earlyEpoch;
    memset(g_tfA, 0, sizeof g_tfA);
    g_tfA[0x30] = 3;
    g_tfA[kFilterVirtual] = 1;
    for (int i = 0; i < 2; ++i) {
        Job& e = g_early[i];
        TestEarlyInputs(e, i == 0 ? 1.0 : 1.5, g_tfA);  // search 1: another start point
        e.valid = true;
        e.searched = false;
        e.stop.store(0);
        e.linkAsked = false;
        e.state.store(0);
        e.data = nullptr;
        g_earlyOrder[i] = i;
    }
    Job& j = g_jobs[0];
    TestEarlyInputs(j, 1.0, g_tfA);  // this tick's only queued path: search 0's inputs
    j.valid = true;
    j.queryBuilt = false;
    j.pathCtrl = nullptr;
    g_jobCount = 1;
    g_earlyN = g_earlyCount = 2;
    g_earlyEpoch = g_navEpoch.load();
    ++g_gen;
    g_word.store((static_cast<uint64_t>(g_gen) << 32) | (2ull << 16) | 2u);  // both claimed
    g_earlyLive = true;
    g_earlyMode = which == 2 ? 0 : 1;
    if (which == 1) g_navEpoch.fetch_add(1);
    FakeHelperArg arg{{which == 0 ? 30 : 5000, 5000}};
    HANDLE th = CreateThread(nullptr, 0, FakeEarlyHelper, &arg, 0, nullptr);
    bool ok = th != nullptr;
    const int unc0 = g_par.eUncovered.load(), ta0 = g_par.eTickAborts.load();
    const ULONGLONG t0 = GetTickCount64();
    if (ok) EarlyTickCollect(EarlyUsable(which != 3));  // as TickHook (the jobs are built before it when usable)
    const ULONGLONG waited = GetTickCount64() - t0;
    if (th) {
        WaitForSingleObject(th, 10000);
        CloseHandle(th);
    }
    ok = ok && waited < 2000 && !g_earlyLive.load() && (g_word.load() & 0xFFFF) == 0;
    if (which == 0)
        ok = ok && !g_early[0].stop.load() && g_early[0].searched && g_early[1].stop.load() && !g_early[1].searched &&
             g_par.eUncovered.load() == unc0 + 1 && g_par.eTickAborts.load() == ta0;
    else
        ok = ok && g_early[0].stop.load() && g_early[1].stop.load() && !g_early[0].searched && !g_early[1].searched &&
             g_par.eTickAborts.load() == ta0 + 1;
    for (int i = 0; i < 2; ++i) {
        g_early[i].valid = g_early[i].searched = false;
        g_early[i].stop.store(0);
    }
    j.valid = false;
    g_jobCount = savedJobs;
    g_earlyN = g_earlyCount = 0;
    g_earlyEpoch = savedEpoch;
    g_earlyMode = savedEarlyMode;
    return ok ? 1 : 0;
}

// EarlyCovers (N9): a TickActor path is skipped only for a searched early search with the same pre-search inputs. `which`:
// 0 same -> covered; 1 another start point; 2 a filter byte changed since the early start; 3 the early search was not searched
// (stopped / asked a link); 4 another filter object (same bytes). 1 = as expected.
int RepathEarlyTestCovers(int which)
{
    memset(g_tfA, 0, sizeof g_tfA);
    g_tfA[0x40] = 5;
    memcpy(g_tfB, g_tfA, sizeof g_tfB);
    Job& e = g_early[0];
    TestEarlyInputs(e, 1.0, g_tfA);
    e.valid = true;
    e.searched = which != 3;
    g_earlyCount = 1;
    Job j;
    TestEarlyInputs(j, which == 1 ? 1.25 : 1.0, which == 4 ? g_tfB : g_tfA);
    if (which == 2) g_tfA[0x40] = 6;  // the live filter now differs from the early copy
    const bool covered = EarlyCovers(j);
    e.valid = e.searched = false;
    g_earlyCount = 0;
    return covered == (which == 0) ? 1 : 0;
}

// The writer counter (S2, S3). `which`: 0 ReplaceAreaInTileBounds' hook moves the nav counter, calls the original with its
// arguments and returns its result, g_writers back to where it was; 1 an early batch about to be published while a writer runs
// (another thread, g_writers 1) is not started (false, not live, no claimable word, nothing searched, counted); 2 published
// with no writer: live, claimable.
int RepathEarlyTestWriters(int which)
{
    if (which == 0) {
        const ReplaceAreaFn saved = g_replaceArea;
        g_replaceArea = &FakeReplaceArea;
        g_fakeReplaceCalls = 0;
        const uint32_t e0 = g_navEpoch.load();
        const int w0 = g_writers.load();
        int area = 0;
        const int32_t r = ReplaceAreaHook(nullptr, nullptr, nullptr, &area, true, nullptr);
        g_replaceArea = saved;
        return r == 7 && g_fakeReplaceCalls == 1 && g_navEpoch.load() == e0 + 1 && g_writers.load() == w0 ? 1 : 0;
    }
    for (int i = 0; i < 2; ++i) {
        Job& e = g_early[i];
        e.valid = e.searched = true;
        e.stop.store(0);
        e.state.store(0);
        g_earlyOrder[i] = i;
    }
    g_earlyN = g_earlyCount = 2;
    const int ws0 = g_par.eWriterAtStart.load();
    g_word.store(static_cast<uint64_t>(g_gen) << 32);  // nothing claimable before
    if (which == 1) g_writers.fetch_add(1);
    AcquireSRWLockExclusive(&g_earlyLock);
    const bool started = EarlyPublishLocked(2);
    const bool live = g_earlyLive.load();
    const bool claimable = (g_word.load() & 0xFFFF) != 0;
    EarlyCollectLocked(false);  // cleanup (no helper claims in the self-test)
    ReleaseSRWLockExclusive(&g_earlyLock);
    if (which == 1) g_writers.fetch_sub(1);
    bool ok;
    if (which == 1)
        ok = !started && !live && !claimable && !g_early[0].searched && !g_early[1].searched && !g_early[0].stop.load() &&
             g_early[0].state.load() == 0 && g_par.eWriterAtStart.load() == ws0 + 1;
    else
        ok = started && live && claimable && !g_early[0].stop.load() && g_par.eWriterAtStart.load() == ws0;
    ok = ok && !g_earlyLive.load() && (g_word.load() & 0xFFFF) == 0;
    for (int i = 0; i < 2; ++i) {
        g_early[i].valid = g_early[i].searched = false;
        g_early[i].stop.store(0);
    }
    g_earlyN = g_earlyCount = 0;
    return ok ? 1 : 0;
}

// ---- RepathDefer self-tests ----
int RepathDeferTestDiscover(const GameModule& game, uint32_t* rva, const char** why)
{
    uint8_t* rg = nullptr;
    if (!DiscoverDefer(game, &rg, why)) return 0;
    *rva = static_cast<uint32_t>(rg - game.base);
    return 1;
}

// The tolerant key on fake queries. `which`: 0 start moved 30 uu -> near; 1 start moved 80 uu -> not; 2 end moved 40 uu -> near;
// 3 another start poly (points the same) -> not; 4 a filter byte differs -> not; 5 identical -> near, moved 0. 1 = as expected.
int RepathDeferTestKey(int which)
{
    alignas(16) static uint8_t q[0xC0], pool[0x30], lf[0x20], filter[kFilterBytes];
    memset(q, 0, sizeof q);
    memset(pool, 0, sizeof pool);
    memset(lf, 0, sizeof lf);
    memset(filter, 0, sizeof filter);
    static const void* const fakeLfVt[4] = {};
    static const void* const fakeRqf[4] = {};
    const void* const savedLf = g_lfVtbl, *const savedRqf = g_rqfVtbl;
    g_lfVtbl = fakeLfVt;
    g_rqfVtbl = fakeRqf;
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
    filter[0x40] = 5;
    const double sp[3] = {100, 200, 30}, ep[3] = {9000, 4000, 50};
    double sp2[3] = {100, 200, 30}, ep2[3] = {9000, 4000, 50};
    uint64_t s2 = 11;
    if (which == 0) sp2[0] += 30;
    if (which == 1) sp2[1] += 80;
    if (which == 2) ep2[2] += 40;
    if (which == 3) s2 = 12;
    Key a, b;
    bool ok = MakeKey(&a, q, 11, 22, sp, ep, 1e30, filter);
    if (which == 4) filter[0x40] = 6;
    ok = ok && MakeKey(&b, q, s2, 22, sp2, ep2, 1e30, filter);
    double moved = -1;
    const bool isNear = KeyNear(a, b, 50, &moved);
    g_lfVtbl = savedLf;
    g_rqfVtbl = savedRqf;
    if (!ok) return 0;
    switch (which) {
    case 0: return isNear && std::fabs(moved - 30) < 1e-9 ? 1 : 0;
    case 2: return isNear && std::fabs(moved - 40) < 1e-9 ? 1 : 0;
    case 5: return isNear && moved == 0 ? 1 : 0;
    default: return isNear ? 0 : 1;
    }
}

namespace {
// A fake navigation data: RepathRequests at +0x3D0 (data, num, max) over a static element buffer; ResizeGrow's stand-in moves the
// elements to a larger buffer and sets max = num (so every growth step calls it again).
alignas(16) uint8_t g_fakeNav[0x600];
alignas(16) uint8_t g_fakeElems[2][16 * kRqStride];
int g_fakeGrowCalls = 0, g_fakeGrowOld[4] = {};
void SetFakeQueue(int num, int max, uint8_t* data)
{
    memcpy(g_fakeNav + kRqData, &data, 8);
    memcpy(g_fakeNav + kRqNum, &num, 4);
    memcpy(g_fakeNav + kRqMax, &max, 4);
}
void FakeGrow(void* arr, int oldNum)
{
    uint8_t* const a = static_cast<uint8_t*>(arr);
    uint8_t* const from = Rd<uint8_t*>(a, 0);
    uint8_t* const to = from == g_fakeElems[0] ? g_fakeElems[1] : g_fakeElems[0];
    if (g_fakeGrowCalls < 4) g_fakeGrowOld[g_fakeGrowCalls] = oldNum;
    ++g_fakeGrowCalls;
    memcpy(to, from, static_cast<size_t>(oldNum) * kRqStride);
    memcpy(a, &to, 8);
    const int num = Rd<int>(a, 8);
    memcpy(a + 0xC, &num, 4);
}
void FakeElem(uint8_t* e, uintptr_t obj, uintptr_t ctrl, int reason)
{
    memset(e, 0xEE, kRqStride);
    memcpy(e, &obj, 8);
    memcpy(e + 8, &ctrl, 8);
    memcpy(e + 0x10, &reason, 4);
}
}  // namespace

// TakeRequest / PutBackFront on the fake queue. `which`: 0 the job's path queued once: taken out (element emptied, num unchanged,
// the job holds the bytes); 1 queued twice: left; 2 two put back with room: at the front in their order; 3 two put back into a
// full queue: grown twice with the stock Add's old counts, then at the front. 1 = as expected.
int RepathDeferTestQueue(int which)
{
    const ResizeGrowFn savedGrow = g_resizeGrow;
    g_resizeGrow = &FakeGrow;
    g_fakeGrowCalls = 0;
    memset(g_fakeElems, 0, sizeof g_fakeElems);
    uint8_t* const e = g_fakeElems[0];
    bool ok = true;
    if (which <= 1) {
        FakeElem(e, 0xA0, 0xA8, 0);
        FakeElem(e + kRqStride, 0xB0, 0xB8, 1);
        FakeElem(e + 2 * kRqStride, which == 1 ? 0xB0 : 0xC0, which == 1 ? 0xC8 : 0xC8, 2);
        SetFakeQueue(3, 4, e);
        uint8_t before[3 * kRqStride];
        memcpy(before, e, sizeof before);
        Job& j = g_early[5];
        j.reqObj = reinterpret_cast<const void*>(0xB0);
        j.reqCtrl = reinterpret_cast<void*>(0xB8);
        const bool took = TakeRequest(g_fakeNav, j);
        if (which == 0) {
            uint8_t zero[16] = {};
            ok = took && !memcmp(j.req, before + kRqStride, kRqStride) && !memcmp(e + kRqStride, zero, 16) &&
                 !memcmp(e + kRqStride + 16, before + kRqStride + 16, 8) && !memcmp(e, before, kRqStride) &&
                 !memcmp(e + 2 * kRqStride, before + 2 * kRqStride, kRqStride) && Rd<int>(g_fakeNav, kRqNum) == 3;
        } else {
            ok = !took && !memcmp(e, before, sizeof before);
        }
    } else {
        uint8_t recs[2][kRqStride];
        FakeElem(recs[0], 0x10, 0x18, 1);
        FakeElem(recs[1], 0x20, 0x28, 1);
        FakeElem(e, 0x30, 0x38, 0);
        if (which == 3) FakeElem(e + kRqStride, 0x40, 0x48, 2);
        const int n0 = which == 2 ? 1 : 2;
        SetFakeQueue(n0, which == 2 ? 8 : 2, e);
        uint8_t old[2 * kRqStride];
        memcpy(old, e, sizeof old);
        PutBackFront(g_fakeNav, recs, 2);
        uint8_t* const d = Rd<uint8_t*>(g_fakeNav, kRqData);
        ok = Rd<int>(g_fakeNav, kRqNum) == n0 + 2 && !memcmp(d, recs[0], kRqStride) && !memcmp(d + kRqStride, recs[1], kRqStride) &&
             !memcmp(d + 2 * kRqStride, old, static_cast<size_t>(n0) * kRqStride);
        if (which == 2) ok = ok && g_fakeGrowCalls == 0 && d == e;
        else ok = ok && g_fakeGrowCalls == 2 && g_fakeGrowOld[0] == 2 && g_fakeGrowOld[1] == 3 && Rd<int>(g_fakeNav, kRqMax) == 4;
    }
    g_resizeGrow = savedGrow;
    return ok ? 1 : 0;
}

namespace {
uint64_t g_fakeNavWeakAlive = 0x77;
void* FakeWeakGetNav(const void* weak)
{
    return Rd<uint64_t>(weak, 0) == g_fakeNavWeakAlive ? static_cast<void*>(g_fakeNav) : nullptr;
}
DWORD WINAPI FakeDeferHelper(void*)
{
    Sleep(30);
    Job& j = g_early[0];
    j.searched = true;
    j.state.store(1, std::memory_order_release);
    WakeWordAll(&j.state);
    return 0;
}
alignas(16) uint8_t g_fakeCtrl[0x20];
}  // namespace

// A deferred batch of one held request at the next TickActor of its navigation data (fake queue, fake FWeakObjectPtr::Get).
// `which`: 0 its search still runs: kept out (nothing put back); 1 done: put back at the front and offered, then the batch ends
// after the TickActor; 2 the navmesh changed meanwhile (unclaimed search): stopped, put back unsearched, not offered; 3 the
// bound reached while a (fake) helper still searches: the game thread waits for it, put back and offered; 4 the navigation data
// is gone: the batch is dropped and the held weak reference released. 1 = as expected.
int RepathDeferTestResume(int which)
{
    const WeakGetFn savedWeak = g_weakGet;
    const ResizeGrowFn savedGrow = g_resizeGrow;
    const int savedMode = g_deferMode.load(), savedFrames = g_deferMaxFrames.load();
    const uint32_t savedEpoch = g_earlyEpoch;
    const bool savedInst = g_deferInstalled;
    g_deferInstalled = true;
    g_weakGet = &FakeWeakGetNav;
    g_resizeGrow = &FakeGrow;
    g_deferMode = 1;
    g_deferMaxFrames = 6;
    memset(g_fakeElems, 0, sizeof g_fakeElems);
    SetFakeQueue(0, 4, g_fakeElems[0]);
    memset(g_fakeCtrl, 0, sizeof g_fakeCtrl);
    *reinterpret_cast<int*>(g_fakeCtrl + 0xC) = 2;  // weak count: ours and one more (nothing is deleted)
    Job& j = g_early[0];
    j.valid = true;
    j.searched = false;
    j.used = false;
    j.data = nullptr;
    j.held = true;
    j.offer = false;
    j.phase = kPhHeld;
    j.age = which == 3 ? 5 : 0;
    j.linkAsked = false;
    j.stop.store(0);
    FakeElem(j.req, 0x50, reinterpret_cast<uintptr_t>(g_fakeCtrl), 1);
    j.state.store(which == 1 ? 1 : 0);
    if (which == 1) j.searched = true;
    g_earlyCount = g_earlyN = 1;
    g_earlyOrder[0] = 0;
    g_earlyNavData = g_fakeNav;
    g_deferNavWeak = which == 4 ? 0x66 : g_fakeNavWeakAlive;
    g_deferHave = true;
    g_deferHeld = 1;
    g_earlyEpoch = g_navEpoch.load();
    if (which == 2) g_navEpoch.fetch_add(1);
    ++g_gen;
    // the claim word: 0/2/4 unclaimed, 3 claimed by the fake helper; 1 not live (done before)
    if (which != 1) {
        g_word.store((static_cast<uint64_t>(g_gen) << 32) | (static_cast<uint64_t>(which == 3 ? 1 : 0) << 16) | 1u);
        g_earlyLive = true;
    } else {
        g_word.store(static_cast<uint64_t>(g_gen) << 32);
        g_earlyLive = false;
    }
    HANDLE th = which == 3 ? CreateThread(nullptr, 0, FakeDeferHelper, nullptr, 0, nullptr) : nullptr;
    const int pb0 = g_par.dPutBack.load(), ab0 = g_par.dAborted.load(), dl0 = g_par.dDeadline.load(), dr0 = g_par.dDropped.load();
    bool handled = false;
    const bool offered = which == 3 && !th ? false : DeferTick(g_fakeNav, true, &handled);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
    bool ok = which != 3 || th != nullptr;
    const int num = Rd<int>(g_fakeNav, kRqNum);
    const uint8_t* const d = Rd<const uint8_t*>(g_fakeNav, kRqData);
    const bool backAtFront = num == 1 && !memcmp(d, j.req, kRqStride);
    switch (which) {
    case 0:
        ok = ok && !offered && handled && g_deferHave && g_deferHeld == 1 && num == 0 && j.phase == kPhHeld && j.age == 1 &&
             g_par.dPutBack.load() == pb0;
        AcquireSRWLockExclusive(&g_earlyLock);  // cleanup: cancels the unclaimed search
        EarlyCollectLocked(true);
        ReleaseSRWLockExclusive(&g_earlyLock);
        break;
    case 1:
        ok = ok && offered && backAtFront && j.offer && j.phase == kPhOffer && g_deferHeld == 0 && g_par.dPutBack.load() == pb0 + 1;
        DeferAfterTick();
        ok = ok && !g_deferHave && !j.offer && j.phase == kPhDone && g_earlyCount == 0;
        break;
    case 2:
        ok = ok && !offered && backAtFront && !j.offer && j.phase == kPhDone && !g_earlyLive.load() && j.stop.load() == 1 &&
             g_par.dAborted.load() == ab0 + 1;
        DeferAfterTick();
        ok = ok && !g_deferHave;
        break;
    case 3:
        ok = ok && offered && backAtFront && j.offer && j.searched && !g_earlyLive.load() && g_par.dDeadline.load() == dl0 + 1;
        DeferAfterTick();
        ok = ok && !g_deferHave;
        break;
    case 4:
        ok = ok && !offered && !handled && !g_deferHave && num == 0 && *reinterpret_cast<int*>(g_fakeCtrl + 0xC) == 1 &&
             !g_earlyLive.load() && g_par.dDropped.load() == dr0 + 1;
        break;
    default:
        ok = false;
    }
    ok = ok && (g_word.load() & 0xFFFF) == 0;
    j.valid = j.searched = j.held = j.offer = false;
    j.phase = kPhNone;
    j.stop.store(0);
    g_earlyCount = g_earlyN = 0;
    g_deferHave = false;
    g_deferHeld = 0;
    g_earlyNavData = nullptr;
    g_earlyLive = false;
    g_earlyEpoch = savedEpoch;
    g_deferMode = savedMode;
    g_deferMaxFrames = savedFrames;
    g_weakGet = savedWeak;
    g_deferInstalled = savedInst;
    g_resizeGrow = savedGrow;
    return ok ? 1 : 0;
}

// S-067 hand-back (repathfix 2026-11-20) on fake jobs after the grace (three published searches of a fake navigation data):
// 0 all ended, one stopped by a nav link: handed back (batch ended, claim word cleared, nothing taken out or offered, counted);
// 1 one still running, one stopped by a link: kept (0 returned; the caller defers the running one and leaves the link one to
// the stock loop); 2 all ended and searched: kept; 3 DeferOffered / DeferUncoveredQueued: a queued path with an offered result is
// covered, an emptied element and a path without an offer are not / counted. 1 = as expected.
int RepathDeferTestHandBack(int which)
{
    const bool savedInst = g_deferInstalled;
    g_deferInstalled = true;
    for (int i = 0; i < 3; ++i) {
        Job& j = g_early[i];
        j.valid = true;
        j.searched = true;
        j.linkAsked = false;
        j.used = false;
        j.held = false;
        j.offer = false;
        j.data = nullptr;
        j.phase = kPhNone;
        j.stop.store(0);
        j.state.store(1);
        j.reqObj = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x100 + 0x10 * i));
        j.reqCtrl = reinterpret_cast<void*>(static_cast<uintptr_t>(0x108 + 0x10 * i));
        g_earlyOrder[i] = i;
    }
    g_earlyCount = g_earlyN = 3;
    g_earlyNavData = g_fakeNav;
    SetFakeQueue(0, 4, g_fakeElems[0]);
    bool ok = true;
    if (which <= 2) {
        if (which <= 1) {  // the second search asked a link: stopped, unsearched
            g_early[1].searched = false;
            g_early[1].linkAsked = true;
        }
        if (which == 1) g_early[2].state.store(0);  // still running
        ++g_gen;
        g_word.store((static_cast<uint64_t>(g_gen) << 32) | (3ull << 16) | 3u);  // all three claimed
        g_earlyLive = true;
        g_deferHave = true;
        g_deferHeld = 0;
        const int hb0 = g_par.dHandBack.load(), hl0 = g_par.dHandBackLinked.load();
        int linked = -1;
        const int unservable = DeferUnservableAllDone(3, &linked);
        if (which == 0) {
            ok = unservable == 1 && linked == 1;
            if (ok) DeferHandBack(3, unservable, linked);
            ok = ok && !g_deferHave && !g_earlyLive.load() && g_earlyCount == 0 && (g_word.load() & 0xFFFF) == 0 &&
                 g_par.dHandBack.load() == hb0 + 1 && g_par.dHandBackLinked.load() == hl0 + 1 && g_deferWhy == kWHandBack &&
                 !g_early[0].offer && g_early[0].phase == kPhDone && Rd<int>(g_fakeNav, kRqNum) == 0;
        } else {
            ok = unservable == 0 && g_deferHave && g_earlyLive.load() && g_par.dHandBack.load() == hb0;
            if (which == 1) ok = ok && linked == 1;
            g_word.store(static_cast<uint64_t>(g_gen) << 32);
            g_earlyLive = false;
        }
    } else {
        memset(g_fakeElems, 0, sizeof g_fakeElems);
        uint8_t* const e = g_fakeElems[0];
        FakeElem(e, 0x100, 0x108, 1);              // offered below
        FakeElem(e + kRqStride, 0, 0, 1);          // emptied (taken out)
        FakeElem(e + 2 * kRqStride, 0x200, 0x208, 1);  // no offer
        FakeElem(e + 3 * kRqStride, 0x300, 0x308, 1);  // no offer
        SetFakeQueue(4, 4, e);
        g_deferHave = true;
        g_early[0].phase = kPhOffer;
        g_early[0].offer = true;
        g_early[1].phase = kPhOffer;  // offered but not searched: covers nothing
        g_early[1].offer = false;
        ok = DeferOffered(reinterpret_cast<const void*>(0x100)) && !DeferOffered(reinterpret_cast<const void*>(0x110)) &&
             !DeferOffered(reinterpret_cast<const void*>(0x200)) && DeferUncoveredQueued(g_fakeNav) == 2;
        g_early[0].used = true;  // served already: no longer covers
        ok = ok && !DeferOffered(reinterpret_cast<const void*>(0x100)) && DeferUncoveredQueued(g_fakeNav) == 3;
        SetFakeQueue(0, 4, e);
    }
    for (int i = 0; i < 3; ++i) {
        Job& j = g_early[i];
        j.valid = j.searched = j.held = j.offer = j.used = j.linkAsked = false;
        j.phase = kPhNone;
        j.state.store(0);
    }
    g_earlyCount = g_earlyN = 0;
    g_deferHave = false;
    g_deferHeld = 0;
    g_earlyNavData = nullptr;
    g_earlyLive = false;
    g_deferWhy = kWNone;
    g_deferInstalled = savedInst;
    return ok ? 1 : 0;
}

// S-067 burst on fakes (repathfix 2026-11-20, the coordinator's offline proof): five searches published for g_fakeNav with five
// queued requests, all claimed by (fake) helpers; DeferAfterPublish then runs the real grace, hand-back and take-out.
// 0 the road-start shape: all five stopped by a nav-link question within the grace -> handed back: nothing taken out (the queue's
// five elements unchanged), no batch, ParallelBatchAllowed true (RepathParallel searches all five, as with RepathDefer=0);
// 1 five long searches without links, still running after the grace -> all five taken out (held), the queue's elements emptied,
// ParallelBatchAllowed false (the helpers are busy), so nothing is searched on the game thread; 2 four stopped by a link, one
// running -> kept: one held, four left to the stock loop (counted), ParallelBatchAllowed false; 3 (review S1, the second hand-back
// site) four stopped by a link, one running whose path is not in the queue (TakeRequest fails: queued twice or gone, its search
// stopped) and which a (fake) helper then ends -> nothing held: handed back after the take-out loop (queue untouched, grace and
// queued-twice counts taken back, batch collected), ParallelBatchAllowed true. 1 = as expected.
namespace {
DWORD WINAPI FakeStoppedHelper(void*)  // ends job 0 once the game thread has stopped it (as a helper's search drains)
{
    Job& j = g_early[0];
    for (int i = 0; i < 5000 && j.stop.load(std::memory_order_acquire) == 0; ++i) Sleep(1);
    j.searched = false;
    j.state.store(1, std::memory_order_release);
    WakeWordAll(&j.state);
    return 0;
}
}  // namespace
int RepathDeferTestBurst(int which)
{
    const bool savedInst = g_deferInstalled;
    const int savedGrace = g_deferGraceUs.load(), savedMin = g_min.load();
    const double savedQpc = g_qpcMs;
    if (g_qpcMs == 0) {  // set at install; the grace needs the clock
        LARGE_INTEGER fq;
        QueryPerformanceFrequency(&fq);
        g_qpcMs = 1000.0 / static_cast<double>(fq.QuadPart);
    }
    g_deferInstalled = true;
    g_deferGraceUs = 500;
    g_min = 2;
    memset(g_fakeElems, 0, sizeof g_fakeElems);
    uint8_t* const e = g_fakeElems[0];
    for (int i = 0; i < 5; ++i) {
        FakeElem(e + i * kRqStride, 0x1000 + 0x100 * i, 0x1008 + 0x100 * i, 1);
        Job& j = g_early[i];
        j.valid = true;
        j.used = j.held = j.offer = false;
        j.data = nullptr;
        j.phase = kPhNone;
        j.age = 0;
        j.ms = 0;
        j.stop.store(0);
        j.reqObj = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x1000 + 0x100 * i));
        j.reqCtrl = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1008 + 0x100 * i));
        const bool link = which == 0 || (which == 2 && i < 4) || (which == 3 && i >= 1);
        j.searched = false;
        j.linkAsked = link;
        if (link) j.ms = 0.02;
        j.state.store(link ? 1 : 0);  // a link stop ends at once; a long search is still running
        g_earlyOrder[i] = i;
    }
    if (which == 3) g_early[0].reqObj = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x9000));  // not queued
    SetFakeQueue(5, 8, e);
    uint8_t before[5 * kRqStride];
    memcpy(before, e, sizeof before);
    g_earlyCount = g_earlyN = 5;
    g_earlyNavData = g_fakeNav;
    g_deferCarryN = 0;
    ++g_gen;
    g_word.store((static_cast<uint64_t>(g_gen) << 32) | (5ull << 16) | 5u);  // all five claimed
    g_earlyLive = true;
    const int hb0 = g_par.dHandBack.load(), hl0 = g_par.dHandBackLinked.load(), ll0 = g_par.dLinkLeft.load(), hd0 = g_par.dHeld.load();
    const int gd0 = g_par.dGraceDone.load(), du0 = g_par.dDup.load();
    HANDLE th = which == 3 ? CreateThread(nullptr, 0, FakeStoppedHelper, nullptr, 0, nullptr) : nullptr;
    const double t0 = QpcMsNow();
    const bool offered = which == 3 && !th ? true : DeferAfterPublish(g_fakeNav, 5, t0);
    const double waited = QpcMsNow() - t0;
    if (th) {
        WaitForSingleObject(th, 10000);
        CloseHandle(th);
    }
    bool ok = !offered;
    uint8_t zero[16] = {};
    int emptied = 0;
    for (int i = 0; i < 5; ++i) emptied += !memcmp(e + i * kRqStride, zero, 16);
    switch (which) {
    case 0:
        ok = ok && !g_deferHave && g_deferHeld == 0 && !g_earlyLive.load() && (g_word.load() & 0xFFFF) == 0 && !memcmp(e, before, sizeof before) &&
             g_par.dHandBack.load() == hb0 + 1 && g_par.dHandBackLinked.load() == hl0 + 5 && g_deferWhy == kWHandBack &&
             ParallelBatchAllowed(true, g_fakeNav) && waited < 50.0;
        break;
    case 1:
        ok = ok && g_deferHave && g_deferHeld == 5 && emptied == 5 && g_par.dHeld.load() == hd0 + 5 && g_par.dHandBack.load() == hb0 &&
             !ParallelBatchAllowed(true, g_fakeNav) && waited >= 0.5;
        break;
    case 2:
        ok = ok && g_deferHave && g_deferHeld == 1 && emptied == 1 && !memcmp(e, before, 4 * kRqStride) && g_par.dLinkLeft.load() == ll0 + 4 &&
             g_deferWhy == kWLinkLeft && g_par.dHandBack.load() == hb0 && !ParallelBatchAllowed(true, g_fakeNav);
        break;
    case 3:
        ok = ok && !g_deferHave && g_deferHeld == 0 && !g_earlyLive.load() && (g_word.load() & 0xFFFF) == 0 && !memcmp(e, before, sizeof before) &&
             g_par.dHandBack.load() == hb0 + 1 && g_par.dHandBackLinked.load() == hl0 + 4 && g_par.dGraceDone.load() == gd0 &&
             g_par.dDup.load() == du0 && g_par.dLinkLeft.load() == ll0 && g_par.dHeld.load() == hd0 && g_deferWhy == kWHandBack &&
             g_early[0].stop.load() == 1 && ParallelBatchAllowed(true, g_fakeNav);
        break;
    default:
        ok = false;
    }
    // cleanup: the "running" searches end, the batch is collected; nothing of the fake requests is released (fake controllers)
    for (int i = 0; i < 5; ++i) g_early[i].state.store(1);
    if (g_earlyLive.load()) {
        AcquireSRWLockExclusive(&g_earlyLock);
        EarlyCollectLocked(true);
        ReleaseSRWLockExclusive(&g_earlyLock);
    }
    for (int i = 0; i < 5; ++i) {
        Job& j = g_early[i];
        j.valid = j.searched = j.held = j.offer = j.used = j.linkAsked = false;
        j.phase = kPhNone;
        j.state.store(0);
        j.stop.store(0);
    }
    g_earlyCount = g_earlyN = 0;
    g_deferHave = false;
    g_deferHeld = 0;
    g_earlyNavData = nullptr;
    g_earlyLive = false;
    g_deferWhy = kWNone;
    SetFakeQueue(0, 4, g_fakeElems[0]);
    g_deferGraceUs = savedGrace;
    g_min = savedMin;
    g_qpcMs = savedQpc;
    g_deferInstalled = savedInst;
    return ok ? 1 : 0;
}

// S-067 (review S2): the TickHook path with RepathParallel's batch beside an offer-only deferred batch, on fakes. A deferred batch
// of one ended search for path B (offered, nothing held) is still live on the claim word; the queue holds A, B, C. TickBatchWanted:
// the batch is wanted (two queued paths without an offer >= RepathParallelMin 2) and the deferred batch is collected first (claim
// word cleared, not live). DeferSkipOffered: RepathParallel's job for B is not searched, A's and C's are. ServedFor (DtFindHook's
// lookup): B's call is served from the offer, C's from the batch, each once; a call with A's key is served from the batch. 1 = as expected.
int RepathDeferTestBeside()
{
    const bool savedInst = g_deferInstalled;
    const int savedMin = g_min.load();
    const uint32_t savedEpoch = g_earlyEpoch, savedBatchEpoch = g_batchEpoch;
    g_deferInstalled = true;
    g_min = 2;
    memset(g_fakeElems, 0, sizeof g_fakeElems);
    uint8_t* const e = g_fakeElems[0];
    const uintptr_t paths[3] = {0x2000, 0x2100, 0x2200};  // A, B, C
    for (int i = 0; i < 3; ++i) FakeElem(e + i * kRqStride, paths[i], paths[i] + 8, 1);
    SetFakeQueue(3, 4, e);
    Key keys[3];
    memset(keys, 0, sizeof keys);
    for (int i = 0; i < 3; ++i) {
        keys[i].nav = g_fakeNav;
        keys[i].startRef = 0x100 + i;
        keys[i].endRef = 0x200 + i;
        keys[i].sp[0] = 10.0 * i;
    }
    Job& d = g_early[0];  // the deferred search for B: ended, searched, offered in the grace (not held)
    d.valid = true;
    d.searched = true;
    d.used = d.held = false;
    d.offer = true;
    d.phase = kPhOffer;
    d.linkAsked = false;
    d.data = nullptr;
    d.key = keys[1];
    d.reqObj = reinterpret_cast<const void*>(paths[1]);
    d.stop.store(0);
    d.state.store(1);
    g_earlyOrder[0] = 0;
    g_earlyCount = g_earlyN = 1;
    g_earlyNavData = g_fakeNav;
    g_deferHave = true;
    g_deferHeld = 0;
    ++g_gen;
    g_word.store((static_cast<uint64_t>(g_gen) << 32) | (1ull << 16) | 1u);  // claimed by a helper, ended
    g_earlyLive = true;
    const int bc0 = g_par.dBatchCovered.load();
    bool ok = TickBatchWanted(true, g_fakeNav) && !g_earlyLive.load() && (g_word.load() & 0xFFFF) == 0;
    for (int i = 0; i < 3; ++i) {  // RepathParallel's jobs, one per queued path
        Job& j = g_jobs[i];
        j.valid = true;
        j.searched = j.used = false;
        j.data = nullptr;
        j.queryBuilt = false;
        j.pathCtrl = nullptr;
        j.pathObj = reinterpret_cast<const void*>(paths[i]);
        j.key = keys[i];
        j.state.store(0);
    }
    g_jobCount = 3;
    DeferSkipOffered();
    ok = ok && g_jobs[0].valid && !g_jobs[1].valid && g_jobs[2].valid && g_par.dBatchCovered.load() == bc0 + 1;
    for (int i = 0; i < 3; ++i) g_jobs[i].searched = g_jobs[i].valid;  // the batch ran
    g_batchEpoch = g_earlyEpoch = g_navEpoch.load();
    g_earlyServe = true;
    auto serve = [](const Key& k, bool* early) {  // DtFindHook's use of a hit
        bool tolerant = false;
        double moved = 0;
        *early = false;
        Job* const hit = ServedFor(k, early, &tolerant, &moved);
        if (hit) hit->used = true;
        return hit;
    };
    bool early = false;
    ok = ok && serve(keys[1], &early) == &d && early;
    ok = ok && serve(keys[1], &early) == nullptr;
    ok = ok && serve(keys[2], &early) == &g_jobs[2] && !early;
    ok = ok && serve(keys[2], &early) == nullptr;
    ok = ok && serve(keys[0], &early) == &g_jobs[0] && !early;
    g_earlyServe = false;
    for (int i = 0; i < 3; ++i) {
        Job& j = g_jobs[i];
        j.valid = j.searched = j.used = false;
        j.pathObj = nullptr;
    }
    g_jobCount = 0;
    d.valid = d.searched = d.offer = d.used = false;
    d.phase = kPhNone;
    d.state.store(0);
    g_earlyCount = g_earlyN = 0;
    g_deferHave = false;
    g_earlyNavData = nullptr;
    g_earlyLive = false;
    SetFakeQueue(0, 4, g_fakeElems[0]);
    g_earlyEpoch = savedEpoch;
    g_batchEpoch = savedBatchEpoch;
    g_min = savedMin;
    g_deferInstalled = savedInst;
    return ok ? 1 : 0;
}

}  // namespace opf
