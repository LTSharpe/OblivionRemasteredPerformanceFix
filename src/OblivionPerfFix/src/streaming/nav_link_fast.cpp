// NavLinkFast (worker navstream, 2026-10-23, public since 1.2.0, default 1; fixes-explained ## 68): a streamed navmesh
// tile's border links made with less searching: connectExtLinks calls that cannot make a link are left out, and the
// neighbour search looks only at the neighbour's polygons that have a border edge on the facing side. Same links, same
// order (the game's own code does every test); plus counters and timers for the nav-chunk attach.
//
// Where the time goes: World Partition streams navigation as ANavigationDataChunkActor; its BeginPlay (part of
// UWorld::AddToWorld's actor initialisation, so inside one frame and not time-sliced: one actor) attaches the chunk's
// Recast tiles: UNavigationSystemV1::AddNavigationDataChunk -> ARecastNavMesh::OnStreamingNavDataAdded ->
// AttachNavMeshDataChunk -> URecastNavMeshDataChunk::AttachTiles -> dtNavMesh::addTile per tile (UE 5.3
// Navmesh/Private/Detour/DetourNavMesh.cpp). addTile connects the new tile with every other tile (layer) of its grid
// cell (side -1) and with every layer of its four edge neighbours, both ways (connectExtLinks(tile, target, side)).
// connectExtLinks allocates a scratch array, walks the tile's polygons, and for every portal edge pointing to `side`
// (any side for -1) calls findConnectingPolys(va, vb, tile, i, target, oppositeSide, cons), which walks ALL polygons
// of the target to find those with an edge whose neighbour marker is exactly DT_EXT_LINK | oppositeSide, and tests
// those few for touching segments (slab coordinate, overlapSlabs, the detail-mesh height check); each hit becomes a
// link (and, with cluster links, a cluster link). Probe (a road with much navmesh streaming, 4K): a chunk attach
// takes 2-9 ms in one call, ~6700 connectExtLinks calls per attach (0.7 us each), ~1.9 searches per call over ~29
// polygons each.
//
// The fix (NavLinkFast=1), inside one chunk attach (the AddNavigationDataChunk vtable slot) or one connectExtLinks
// call: per tile (cache keyed by the tile slot, its salt, header pointer and polygon count; tile data does not change
// while the tile is attached) one walk of its polygons gives (a) the sides its portal edges point to (neis & 0x8000,
// side byte neis & 0xFF) and (b) per side s the polygons with an edge whose marker is exactly 0x8000 | s.
//  1. Skip: connectExtLinks(tile, target, side) is left out when the tile has no portal edge that the call would look
//     at (side byte == side, or any for -1), or when the target has no polygon with an edge marked for any of the
//     opposite sides ((d - 4) & 7) those edges would search: then every search the stock call would make returns no
//     entry, so it makes no link and no cluster link and changes nothing (it only allocates and frees its scratch
//     array). Tiles with a polygon of more than 6 vertices, or a portal side byte >= 8 with side -1, take the stock call.
//  2. Search: a call that is made searches through FindHook (findConnectingPolys' one call site): for an even side it
//     calls the game's own findConnectingPolys once per listed polygon of the target on a proxy copy of the target
//     tile whose polygon array, detail-mesh array and header (polyCount 1, detailMeshCount 1 or 0) start at that
//     polygon. The game's code then does exactly the per-polygon work of its own loop for that polygon (same edge order,
//     same `break` after the first match, getHeightFromDMesh on the same detail mesh, same pushes into the same array);
//     polygons that are not listed only `continue` in the stock loop. The one address-dependent value, each pushed
//     entry's polygon reference (salt << (tileBits + polyBits) | tileIndex << polyBits | i), is then written as the stock
//     code computes it for the real tile. Odd sides (the stock code reads uninitialised slab bounds there) take the
//     stock search.
// Internal mode 3 = installed, stock path, timers and counters only (NavLinkFast=0 while the game runs). Lab builds:
// NavLinkFast=2 = 1 plus checks: every fast search is compared with the stock search byte for byte (the stock entries
// are kept on a difference), and every skipped call is made anyway and must make no entry; 3 as above; NavLinkLogMs logs
// each attach / detach of at least that many ms. Self-test: random tiles, the fast search against the game's, and the
// skip rule against a brute-force run of every search the stock call would make.
//
// Found by byte patterns (functions' prologues and, in findConnectingPolys, the block that reads m_tiles (+0xE8),
// m_tileBits (+0xF4), m_polyBits (+0xF8), the tile stride 0xA8 (magic 0xC30C30C30C30C30D), header->polyCount (+4));
// getHeightFromDMesh's header->detailMeshCount (+0x12) read checked too. Every call site checked before it is written,
// only while the game starts (version.dll route; on the OBSE64 route the key stays off). Any mismatch: a log line, nothing
// written. The nav-chunk attach's vtable slot (AddNavigationDataChunk, RemoveNavigationDataChunk next) sets the per-tile
// cache's scope to one whole attach; without it the cache lives for one connectExtLinks call.
#include "common.h"

#include <atomic>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace opf {
namespace {

// dtNavMesh::findConnectingPolys (build 19115871 exe+0x4174C30)
constexpr char kFind[] =
    "41 55 41 56 48 81 EC 98 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 D0 00 00 00 4C 8B B4 24 D8 01 00 00 "
    "4C 8B E9 48 8B 8C 24 E8 01 00 00";
// its loop set-up at +0x195: polyBits [r13+F8], tile - m_tiles [r13+E8], / 0xA8, header [r14+8], salt [r14],
// tileBits [r13+F4], i < header->polyCount [rax+4]
constexpr char kFindShape[] =
    "45 8B 85 F8 00 00 00 33 F6 B8 00 80 00 00 F2 0F 11 64 24 40 0F 11 84 24 A0 00 00 00 44 0F B7 CB 49 8B CE F2 0F 10 "
    "42 10 66 44 0B C8 49 2B 8D E8 00 00 00 48 B8 0D C3 30 0C C3 30 0C C3 48 F7 E9 F2 0F 11 84 24 B0 00 00 00 49 8B 46 "
    "08 8B FE 48 03 D1 66 44 89 4C 24 20 48 C1 FA 07 41 8B C8 4C 8B D2 89 74 24 24 49 C1 EA 3F 44 8B FE 4C 03 D2 41 8B "
    "16 45 8B D2 49 D3 E2 41 8B 8D F4 00 00 00 41 03 C8 48 D3 E2 4C 0B D2 4C 89 54 24 48 66 3B 70 04";
constexpr size_t kFindShapeAt = 0x195;
// dtNavMesh::getHeightFromDMesh (exe+0x417B4A0): tile null / polyIdx < 0 / polyIdx >= header->detailMeshCount
constexpr char kHeight[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 40 4D 8B E8 48 8B D9 48 85 C9 "
    "0F 84 ?? ?? ?? ?? 85 D2 0F 88 ?? ?? ?? ?? 48 8B 41 08 44 0F B7 48 12 41 3B D1";
// dtNavMesh::connectExtLinks (exe+0x4171090) and its findConnectingPolys call (+0x20C): mov [rsp+28h],r15 (target) /
// mov [rsp+20h],eax (fromPolyIdx) / call
constexpr char kConnect[] =
    "48 85 D2 0F 84 ?? ?? ?? ?? 48 8B C4 44 89 48 20 48 89 48 08 57 41 56 41 57 48 81 EC E0 00 00 00 48 89 58 18 48 8B "
    "FA 4C 8B F1 48 89 70 D8 BB 10 00 00 00 33 D2 B9 80 01 00 00";
constexpr char kFindCall[] = "4C 89 7C 24 28 89 44 24 20 E8";
constexpr size_t kConnectSpan = 0x500;
// dtNavMesh::addTile (exe+0x416E640): its 4 connectExtLinks calls
constexpr char kAddTile[] =
    "41 55 48 81 EC 90 0D 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 50 0D 00 00 66 83 3A 07";
constexpr size_t kAddTileSpan = 0xB80;
// UNavigationSystemV1::AddNavigationDataChunk (exe+0x4823850; the nav chunk's BeginPlay calls it through vtable +0x320,
// RemoveNavigationDataChunk is the next slot)
constexpr char kAddChunk[] = "48 89 5C 24 18 57 48 83 EC 20 48 8B D9 41 B0 01 48 81 C1 ?? ?? ?? ?? 48 8B FA E8";

constexpr size_t kTileSize = 0xA8, kHeaderSize = 0x58, kPolySize = 0x20, kDetailSize = 6;

using FindFn = int (*)(void*, const double*, const double*, void*, uint64_t, uint8_t*, uint64_t, void*);
using ConnectFn = void (*)(void*, uint8_t*, uint8_t*, uint64_t, uint64_t);
using ChunkFn = void (*)(void*, void*);

FindFn g_find = nullptr;
ConnectFn g_connect = nullptr;
ChunkFn g_addChunk = nullptr, g_removeChunk = nullptr;
bool g_installed = false;
std::atomic<int> g_mode{0};

struct Cons {  // dtChunkArray<FConnectingPolyData>
    uint8_t* data;
    int32_t size;
    int32_t cap;
};
constexpr size_t kEntry = 0x18;  // {double min, double max, dtPolyRef ref}

struct TileInfo {
    uint32_t salt = 0;
    const void* header = nullptr;
    uint16_t polyCount = 0;
    bool unsafe = false;  // no header, or a polygon with more than 6 vertices: the stock path for this tile
    bool bigDir = false;  // a portal edge whose side byte is >= 8
    uint8_t dirs = 0;     // bit d: a portal edge (neis & 0x8000) with side byte d
    uint8_t exact = 0;    // bit s: an edge with neis == 0x8000 | s
    std::vector<uint16_t> list[8];  // per s: the polygons with such an edge, ascending
};
thread_local std::unordered_map<const uint8_t*, TileInfo> t_cache;  // valid for one chunk attach or one connect call
thread_local int t_connectDepth = 0;
thread_local int t_chunkDepth = 0;

// counters (all threads), reset by the report
std::atomic<uint64_t> s_connects{0}, s_skipNoEdge{0}, s_skipNoCand{0};
std::atomic<uint64_t> s_finds{0}, s_findsFast{0}, s_polysScanned{0}, s_candidates{0};
std::atomic<uint64_t> s_chunks{0}, s_chunkTicks{0}, s_chunkMax{0}, s_removes{0}, s_removeTicks{0}, s_removeMax{0};

uint64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
uint64_t Freq()
{
    static uint64_t f = 0;
    if (!f) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = static_cast<uint64_t>(q.QuadPart);
    }
    return f;
}
double Ms(uint64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(Freq()); }
void Max(std::atomic<uint64_t>& m, uint64_t v)
{
    uint64_t c = m.load(std::memory_order_relaxed);
    while (v > c && !m.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}

uint16_t U16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
template <class T> T* Ptr(const uint8_t* p) { T* v; memcpy(&v, p, sizeof v); return v; }

const TileInfo& Info(const uint8_t* tile)
{
    TileInfo& t = t_cache[tile];
    uint32_t salt;
    memcpy(&salt, tile, 4);
    const uint8_t* header = Ptr<uint8_t>(tile + 8);
    const uint16_t polyCount = header ? U16(header + 4) : 0;
    if (header && t.header == header && t.salt == salt && t.polyCount == polyCount) return t;
    for (auto& l : t.list) l.clear();
    t.salt = salt;
    t.header = header;
    t.polyCount = polyCount;
    t.unsafe = !header;
    t.bigDir = false;
    t.dirs = t.exact = 0;
    if (!header) return t;
    const uint8_t* polys = Ptr<uint8_t>(tile + 0x10);
    for (unsigned i = 0; i < polyCount; ++i) {
        const uint8_t* poly = polys + i * kPolySize;
        const unsigned nv = poly[0x1E];
        if (nv > 6) t.unsafe = true;
        unsigned seen = 0;  // exact sides already listed for this polygon
        for (unsigned j = 0; j < nv && j < 6; ++j) {
            const uint16_t nei = U16(poly + 0x10 + 2 * j);
            if (!(nei & 0x8000u)) continue;
            const unsigned dir = nei & 0xFFu;
            if (dir < 8) t.dirs |= static_cast<uint8_t>(1u << dir);
            else t.bigDir = true;
            if ((nei & 0xFFF8u) != 0x8000u) continue;
            const unsigned side = nei & 7u;
            t.exact |= static_cast<uint8_t>(1u << side);
            if (seen & (1u << side)) continue;
            seen |= 1u << side;
            t.list[side].push_back(static_cast<uint16_t>(i));
        }
    }
    return t;
}

// 0 = make the call; 1 = the tile has no portal edge the call would look at; 2 = the target has no polygon marked for
// the sides those edges would search
int SkipClass(const uint8_t* tile, const uint8_t* target, uint64_t side64)
{
    if (tile == nullptr || target == nullptr) return 0;  // the stock call (no class)
    const TileInfo& ti = Info(tile);
    const TileInfo& gi = Info(target);
    if (ti.unsafe || gi.unsafe) return 0;
    const int32_t side = static_cast<int32_t>(static_cast<uint32_t>(side64));
    uint8_t dirs;
    if (side == -1) {
        if (ti.bigDir) return 0;
        dirs = ti.dirs;
    } else if (side >= 0 && side < 8) dirs = ti.dirs & static_cast<uint8_t>(1u << side);
    else return 0;
    if (!dirs) return 1;
    uint8_t need = 0;
    for (unsigned d = 0; d < 8; ++d)
        if (dirs & (1u << d)) need |= static_cast<uint8_t>(1u << ((d - 4) & 7u));
    return (need & gi.exact) ? 0 : 2;
}

// The game's per-polygon work for the listed polygons (see the top of the file); returns the stock return value.
int FastFind(void* nav, const double* va, const double* vb, void* fromTile, uint64_t fromPolyIdx, uint8_t* tile, unsigned side,
             Cons* cons, const std::vector<uint16_t>& list)
{
    alignas(16) uint8_t proxy[kTileSize];
    alignas(16) uint8_t hdr[kHeaderSize];
    const uint8_t* header = Ptr<uint8_t>(tile + 8);
    const uint8_t* polys = Ptr<uint8_t>(tile + 0x10);
    const uint8_t* detail = Ptr<uint8_t>(tile + 0x28);
    memcpy(proxy, tile, kTileSize);
    memcpy(hdr, header, kHeaderSize);
    const uint16_t one = 1, zero = 0;
    memcpy(hdr + 4, &one, 2);
    const unsigned detailCount = U16(header + 0x12);
    uint8_t* const hp = hdr;
    memcpy(proxy + 8, &hp, sizeof hp);
    // the reference base the stock code computes for the real tile: salt << (tileBits + polyBits) | index << polyBits
    const auto* n = static_cast<const uint8_t*>(nav);
    const uint8_t* tiles = Ptr<uint8_t>(n + 0xE8);
    uint32_t tileBits, polyBits, salt;
    memcpy(&tileBits, n + 0xF4, 4);
    memcpy(&polyBits, n + 0xF8, 4);
    memcpy(&salt, tile, 4);
    const int64_t index = (tile - tiles) / static_cast<int64_t>(kTileSize);
    const uint64_t base = (static_cast<uint64_t>(static_cast<uint32_t>(index)) << (polyBits & 63)) |
                          (static_cast<uint64_t>(salt) << ((tileBits + polyBits) & 63));
    int total = 0;
    for (const uint16_t i : list) {
        memcpy(hdr + 0x12, i < detailCount ? &one : &zero, 2);
        const uint8_t* p = polys + static_cast<size_t>(i) * kPolySize;
        memcpy(proxy + 0x10, &p, sizeof p);
        const uint8_t* d = detail ? detail + static_cast<size_t>(i) * kDetailSize : nullptr;
        memcpy(proxy + 0x28, &d, sizeof d);
        const int32_t before = cons->size;
        total += g_find(nav, va, vb, fromTile, fromPolyIdx, proxy, side, cons);
        for (int32_t k = before; k < cons->size; ++k) {
            const uint64_t ref = static_cast<uint64_t>(static_cast<int64_t>(i)) | base;
            memcpy(cons->data + static_cast<size_t>(k) * kEntry + 0x10, &ref, 8);
        }
    }
    return total;
}

int FindHook(void* nav, const double* va, const double* vb, void* fromTile, uint64_t fromPolyIdx, uint8_t* tile, uint64_t side64,
             void* consRaw)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    const unsigned side = static_cast<uint32_t>(side64);
    auto* cons = static_cast<Cons*>(consRaw);
    s_finds.fetch_add(1, std::memory_order_relaxed);
    const bool fast = mode == 1;
    int r;
    if (fast && t_connectDepth > 0 && tile && cons && (side & ~6u) == 0 && !Info(tile).unsafe) {
        const auto& list = Info(tile).list[side];
        s_findsFast.fetch_add(1, std::memory_order_relaxed);
        s_candidates.fetch_add(list.size(), std::memory_order_relaxed);
        if (mode == 1) r = FastFind(nav, va, vb, fromTile, fromPolyIdx, tile, side, cons, list);
        else r = g_find(nav, va, vb, fromTile, fromPolyIdx, tile, side64, consRaw);
    } else {
        if (tile && Ptr<uint8_t>(tile + 8)) s_polysScanned.fetch_add(U16(Ptr<uint8_t>(tile + 8) + 4), std::memory_order_relaxed);
        r = g_find(nav, va, vb, fromTile, fromPolyIdx, tile, side64, consRaw);
    }
    return r;
}

void ConnectHook(void* nav, uint8_t* tile, uint8_t* target, uint64_t side, uint64_t updateCLinks)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    ++t_connectDepth;
    const int skip = mode == 1 ? SkipClass(tile, target, side) : 0;
    s_connects.fetch_add(1, std::memory_order_relaxed);
    if (skip) (skip == 1 ? s_skipNoEdge : s_skipNoCand).fetch_add(1, std::memory_order_relaxed);
    if (!skip) g_connect(nav, tile, target, side, updateCLinks);
    --t_connectDepth;
    if (t_connectDepth == 0 && t_chunkDepth == 0) t_cache.clear();
}

void AddChunkHook(void* navSys, void* chunkActor)
{
    const bool outer = t_chunkDepth++ == 0;
    const uint64_t t0 = Now();
    g_addChunk(navSys, chunkActor);
    const uint64_t t1 = Now(), dt = t1 - t0;
    --t_chunkDepth;
    if (!outer) return;
    if (t_connectDepth == 0) t_cache.clear();
    s_chunks.fetch_add(1, std::memory_order_relaxed);
    s_chunkTicks.fetch_add(dt, std::memory_order_relaxed);
    Max(s_chunkMax, dt);
}

void RemoveChunkHook(void* navSys, void* chunkActor)
{
    const uint64_t t0 = Now();
    g_removeChunk(navSys, chunkActor);
    const uint64_t t1 = Now(), dt = t1 - t0;
    s_removes.fetch_add(1, std::memory_order_relaxed);
    s_removeTicks.fetch_add(dt, std::memory_order_relaxed);
    Max(s_removeMax, dt);
}

uint8_t* FindOne(const char* text, const char* what)
{
    const auto hits = FindAll(Game().text, ParsePattern(text));
    if (hits.size() != 1) {
        Log("[navlink] %s found %zu times (expected 1): NavLinkFast unavailable", what, hits.size());
        return nullptr;
    }
    return hits[0];
}

bool Fail(const char* why)
{
    Log("[navlink] %s: NavLinkFast unavailable", why);
    return false;
}

// a stub near the game's code: jmp [rip+0] / absolute address
uint8_t* Stub(uint8_t* at, void* target)
{
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, jmpAbs, sizeof jmpAbs);
    memcpy(at + sizeof jmpAbs, &target, sizeof target);
    return at;
}

bool CallTo(const uint8_t* site, const uint8_t* fn) { return site[0] == 0xE8 && RipTarget(site, 1, 5) == fn; }

uint8_t* g_findSite = nullptr;
uint8_t* g_connectSites[4] = {};
void** g_chunkSlot = nullptr;

}  // namespace

bool InstallNavLinkFast(bool beforeGameCode, int mode)
{
    if (g_installed) return true;
    const GameModule& game = Game();
    uint8_t* const find = FindOne(kFind, "findConnectingPolys");
    uint8_t* const connect = FindOne(kConnect, "connectExtLinks");
    uint8_t* const addTile = FindOne(kAddTile, "addTile");
    if (!find || !connect || !addTile) return false;
    if (!MatchesAt(find + kFindShapeAt, ParsePattern(kFindShape))) return Fail("findConnectingPolys has an unexpected shape");
    // findConnectingPolys' two getHeightFromDMesh calls
    {
        const Pattern h = ParsePattern(kHeight);
        int calls = 0;
        for (uint8_t* p = find; p < find + 0x700; ++p)
            if (p[0] == 0xE8) {
                uint8_t* const t = RipTarget(p, 1, 5);
                if (game.text.Contains(t) && t + h.size() <= game.text.end && MatchesAt(t, h)) ++calls;
            }
        if (calls != 2) return Fail("getHeightFromDMesh calls not found in findConnectingPolys");
    }
    uint8_t* findSite = nullptr;
    {
        const Pattern c = ParsePattern(kFindCall);
        for (uint8_t* p = connect; p + c.size() + 4 < connect + kConnectSpan; ++p)
            if (MatchesAt(p, c) && CallTo(p + c.size() - 1, find)) {
                if (findSite) return Fail("two findConnectingPolys calls in connectExtLinks");
                findSite = p + c.size() - 1;
            }
        if (!findSite) return Fail("connectExtLinks' findConnectingPolys call not found");
    }
    uint8_t* sites[4] = {};
    int n = 0;
    for (uint8_t* p = addTile; p + 5 < addTile + kAddTileSpan; ++p)
        if (CallTo(p, connect)) {
            if (n == 4) return Fail("more than 4 connectExtLinks calls in addTile");
            sites[n++] = p;
        }
    if (n != 4) return Fail("addTile's 4 connectExtLinks calls not found");
    // the nav chunk attach/detach (timers only): AddNavigationDataChunk's one vtable slot, RemoveNavigationDataChunk next
    void** slot = nullptr;
    if (uint8_t* const addChunk = FindOne(kAddChunk, "AddNavigationDataChunk")) {
        int hits = 0;
        for (uint8_t* p = game.rdata.begin; p + 16 <= game.rdata.end; p += 8) {
            void* v;
            memcpy(&v, p, sizeof v);
            if (v == addChunk) {
                ++hits;
                slot = reinterpret_cast<void**>(p);
            }
        }
        void* next = slot ? slot[1] : nullptr;
        if (hits != 1 || !game.text.Contains(next)) {
            Log("[navlink] AddNavigationDataChunk's vtable slot found %d times: no chunk attach timers", hits);
            slot = nullptr;
        }
    }
    if (!beforeGameCode) return Fail("can only be installed at start-up (version.dll)");
    uint8_t* const region = AllocNearGame(64);
    if (!region) return Fail("no free memory near the game's code");
    uint8_t* const findStub = Stub(region, reinterpret_cast<void*>(&FindHook));
    uint8_t* const connectStub = Stub(region + 16, reinterpret_cast<void*>(&ConnectHook));
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    auto rel = [](const uint8_t* site, const uint8_t* to, uint8_t out[5]) {
        const int64_t r = to - (site + 5);
        if (r != static_cast<int32_t>(r)) return false;
        const int32_t r32 = static_cast<int32_t>(r);
        out[0] = 0xE8;
        memcpy(out + 1, &r32, 4);
        return true;
    };
    uint8_t bytes[5];
    if (!rel(findSite, findStub, bytes)) return Fail("the hook is out of reach");
    for (uint8_t* s : sites) {
        uint8_t b[5];
        if (!rel(s, connectStub, b)) return Fail("the hook is out of reach");
    }
    g_find = reinterpret_cast<FindFn>(find);
    g_connect = reinterpret_cast<ConnectFn>(connect);
    if (!WriteCode(findSite, bytes)) return Fail("could not patch the findConnectingPolys call");
    g_findSite = findSite;
    for (int i = 0; i < 4; ++i) {
        uint8_t b[5];
        rel(sites[i], connectStub, b);
        if (!WriteCode(sites[i], b)) {
            Log("[navlink] could not patch addTile's connectExtLinks call %d (the search scope is per call: no effect)", i);
            continue;
        }
        g_connectSites[i] = sites[i];
    }
    if (slot) {
        g_addChunk = reinterpret_cast<ChunkFn>(slot[0]);
        g_removeChunk = reinterpret_cast<ChunkFn>(slot[1]);
        if (WritePointer(&slot[0], reinterpret_cast<void*>(&AddChunkHook)) &&
            WritePointer(&slot[1], reinterpret_cast<void*>(&RemoveChunkHook)))
            g_chunkSlot = slot;
    }
    g_installed = true;
    g_mode = mode;
    Log("[navlink] findConnectingPolys exe+0x%X (call exe+0x%X), connectExtLinks exe+0x%X (4 calls in addTile exe+0x%X), "
        "chunk attach timers %s: installed, mode %d",
        static_cast<unsigned>(find - game.base), static_cast<unsigned>(findSite - game.base), static_cast<unsigned>(connect - game.base),
        static_cast<unsigned>(addTile - game.base), g_chunkSlot ? "on" : "off", mode);
    return true;
}

void SetNavLinkFast(int mode)
{
    if (!g_installed) return;
    mode = mode == 1 ? 1 : 3;  // 0 at run time = the stock path (the patch stays)
    if (g_mode.exchange(mode) != mode)
        Log("[navlink] mode %d (%s)", mode,
            mode == 1   ? "calls that cannot make a link left out, fast neighbour search"
            : mode == 2 ? "as 1, every left-out call and fast search checked against the game's"
                        : "the game's calls and search, timed");
}


void LogNavLinkFastStats()
{
    if (!g_installed) return;
    const uint64_t c = s_connects.exchange(0), sk1 = s_skipNoEdge.exchange(0), sk2 = s_skipNoCand.exchange(0);
    const uint64_t f = s_finds.exchange(0), ff = s_findsFast.exchange(0), ps = s_polysScanned.exchange(0), cand = s_candidates.exchange(0);
    const uint64_t ch = s_chunks.exchange(0), cht = s_chunkTicks.exchange(0), chm = s_chunkMax.exchange(0);
    const uint64_t r = s_removes.exchange(0), rt = s_removeTicks.exchange(0), rm = s_removeMax.exchange(0);
    if (!c && !ch && !r) return;
    Log("[navlink] since last report (mode %d): chunk attaches %llu (%.2f ms, max %.2f), detaches %llu (%.2f ms, max %.2f); tile connects "
        "%llu (left out: %llu no edge, %llu no neighbour polygon); neighbour searches %llu (fast %llu: %llu polygons tried; stock: %llu "
        "polygons walked)",
        g_mode.load(), ch, Ms(cht), Ms(chm), r, Ms(rt), Ms(rm), c, sk1, sk2, f, ff, cand, ps);
}

// self-test: one neighbour search through the hook inside a connect scope (mode 1/2/3), and the original
int NavLinkFastTestFind(int mode, void* nav, const double* va, const double* vb, void* fromTile, int fromPolyIdx, uint8_t* tile, int side,
                        void* cons)
{
    const int keep = g_mode.exchange(mode);
    ++t_connectDepth;
    const int r = FindHook(nav, va, vb, fromTile, static_cast<uint32_t>(fromPolyIdx), tile, static_cast<uint32_t>(side), cons);
    --t_connectDepth;
    t_cache.clear();
    g_mode = keep;
    return r;
}
int NavLinkFastTestOriginal(void* nav, const double* va, const double* vb, void* fromTile, int fromPolyIdx, uint8_t* tile, int side,
                            void* cons)
{
    return g_find(nav, va, vb, fromTile, static_cast<uint32_t>(fromPolyIdx), tile, static_cast<uint32_t>(side), cons);
}
int NavLinkFastTestSkip(const uint8_t* tile, const uint8_t* target, int side)  // the skip rule (0 = the call is made)
{
    const int r = SkipClass(tile, target, static_cast<uint32_t>(side));
    t_cache.clear();
    return r;
}
uint8_t* NavLinkFastTestSite(int which) { return which < 0 ? g_findSite : which < 4 ? g_connectSites[which] : nullptr; }

}  // namespace opf
