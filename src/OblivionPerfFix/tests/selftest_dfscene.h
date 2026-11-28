// DfObjectBufferKeep / DfMappingReserve / DfAddSpread self-test (worker rasavafix, 2026-11-27, S-087; the DfAddSpread cases
// from worker dfaddspread's selftest_dfaddspread.h, 2026-11-19), included by selftest.cpp.
// On the mapped exe (Steam 19115871): the UpdateDistanceFieldObjectBuffers call exe+0x1D96471 (exe+0x1DACA70; the scene's
// DistanceFieldSceneData +0x3368), its object-buffer resize calls exe+0x1DAD094 / exe+0x1DAD0B2 (named DFObjectData /
// DFObjectBounds), the add and update loops' ProcessPrimitiveUpdate calls exe+0x1DACDA5 / exe+0x1DACF95 (exe+0x1D973E0),
// PendingAddOperations +0x470, IndicesToUpdateInObjectBuffers +0xF8, AddPrimitive's insert exe+0xEAFD50, the proxy +0x8 and
// its bounds' radius +0x250, the instance list +0x450 (208-byte entries, alignment 16) and its reallocation exe+0xDE6200.
// Nothing is patched. Then the three keys' logic with fakes.
#pragma once

#include <algorithm>
#include <vector>

namespace dfscene_test {
alignas(16) uint8_t g_dfs[0x600] = {};
alignas(16) uint8_t g_dfs2[0x600] = {};
struct FakePrim { int inst; int pad; void* proxy; };
alignas(16) uint8_t g_proxy[64][0x300] = {};
FakePrim g_prim[64] = {};
std::vector<void*> g_set;            // the fake PendingAddOperations of g_dfs, in the game's loop order
std::vector<void*> g_added;          // primitives processed by the fake ProcessPrimitiveUpdate (add), in order
int g_updates = 0;
struct FakeArray { void* data; int32_t num; int32_t max; };
FakeArray g_adds{}, g_removes{};
bool g_wrongIndices = false;
bool g_posKnown = true;
double g_pos[3] = {0, 0, 0};
int g_loadingState = 0;
// resize / reallocation fakes
uint32_t g_lastBytes = 0;
int g_allocCalls = 0;
int32_t g_allocNum = -1, g_allocMax = -1;
size_t g_allocBytes = 0;
bool g_allocBad = false;
int32_t g_buildTo = 0, g_buildMax = 0;  // the mapping fake: after its call, Num / Max (stock growth) if Max is smaller

void SyncNum() { const int32_t n = static_cast<int32_t>(g_set.size()); memcpy(g_dfs + 0x478, &n, 4); }
int32_t NumObjects() { int32_t v; memcpy(&v, g_dfs, 4); return v; }
int32_t Rd(const uint8_t* d, uint32_t off) { int32_t v; memcpy(&v, d + off, 4); return v; }
void Wr(uint8_t* d, uint32_t off, int32_t v) { memcpy(d + off, &v, 4); }
void* Emplace(void* set, int32_t* outId, void** key, bool* already)
{
    if (set != g_dfs + 0x470) return nullptr;
    for (void* p : g_set) if (p == *key) { if (already) *already = true; return nullptr; }
    g_set.push_back(*key);
    SyncNum();
    if (outId) *outId = static_cast<int32_t>(g_set.size()) - 1;
    return nullptr;
}
void Ppu(bool add, void*, void* prim, void*, void* indices, void*, void*)
{
    if (!add || (!g_wrongIndices && indices != g_dfs + 0xF8)) return;
    const int n = static_cast<FakePrim*>(prim)->inst;
    const int32_t v = NumObjects() + n;
    memcpy(g_dfs, &v, 4);
    g_added.push_back(prim);
}
void Dfob(void* dfs, void*, void*, void* scene, void* adds, void* removes)
{
    ++g_updates;
    if (dfs != g_dfs) {  // the mapping cases: a world build to g_buildTo entries with the game's growth to g_buildMax
        uint8_t* d = static_cast<uint8_t*>(dfs);
        if (g_buildTo > Rd(d, 0x458)) {
            Wr(d, 0x458, g_buildTo);
            if (Rd(d, 0x45C) < g_buildTo) Wr(d, 0x45C, g_buildMax);
        }
        Wr(d, 0x478, 0);
        return;
    }
    const std::vector<void*> loop = g_set;  // the game iterates its set and then resets it
    uint64_t other = 0;
    for (void* p : loop)
        DfAddSpreadAddHook(true, scene, p, nullptr, g_wrongIndices ? static_cast<void*>(&other) : static_cast<uint8_t*>(dfs) + 0xF8,
                           adds, removes);
    g_set.clear();
    SyncNum();
}
void* Resize(void*, void* ext, uint32_t bytes, const wchar_t*)
{
    g_lastBytes = bytes;
    static int made = 0;
    void* buf = &made;  // the game's TRefCountPtr now holds a buffer
    memcpy(ext, &buf, sizeof buf);
    return nullptr;
}
void ResizeAlloc(void* arr, int32_t prevNum, int32_t newMax, size_t bytes, uint32_t align)
{
    ++g_allocCalls;
    g_allocNum = prevNum;
    g_allocMax = newMax;
    g_allocBytes = bytes * static_cast<size_t>(newMax);
    if (arr != g_dfs2 + 0x450 || align != 16) g_allocBad = true;
}
int Loading() { return g_loadingState; }
bool CamPos(void*, double* x, double* y, double* z)
{
    if (!g_posKnown) return false;
    *x = g_pos[0];
    *y = g_pos[1];
    *z = g_pos[2];
    return true;
}
void PlacePrim(int i, double x, double y, double z, double radius)
{
    const double b[7] = {x, y, z, radius, radius, radius, radius};
    memcpy(g_proxy[i] + 0x250 - 48, b, sizeof b);
}
void Reset(int nPrims, int instEach)
{
    memset(g_dfs, 0, sizeof g_dfs);
    g_set.clear();
    g_added.clear();
    g_updates = 0;
    for (int i = 0; i < 64; ++i) {
        g_prim[i] = FakePrim{instEach, 0, g_proxy[i]};
        PlacePrim(i, 1.0e6, 0, 0, 1000.0);  // far (10 km) by default
    }
    for (int i = 0; i < nPrims; ++i) g_set.push_back(&g_prim[i]);
    SyncNum();
}
void Setup(int buf, int map, int spread, int budget, int nearM = 60)
{
    DfSceneKeepTestFakes t{};
    t.dfob = reinterpret_cast<void*>(&Dfob);
    t.ppu = reinterpret_cast<void*>(&Ppu);
    t.emplace = reinterpret_cast<void*>(&Emplace);
    t.resize = reinterpret_cast<void*>(&Resize);
    t.resizeAlloc = reinterpret_cast<void*>(&ResizeAlloc);
    t.loading = reinterpret_cast<void*>(&Loading);
    t.camPos = reinterpret_cast<void*>(&CamPos);
    t.setOff = 0x470;
    t.idxOff = 0xF8;
    t.proxyOff = 8;
    t.radiusOff = 0x250;
    t.mapOff = 0x450;
    t.elemSize = 0xD0;
    t.bufKey = buf;
    t.mapKey = map;
    t.spreadKey = spread;
    t.budget = budget;
    t.nearM = nearM;
    t.settleMs = 0;
    DfSceneKeepTestSetup(t);
}
void Update() { DfAddSpreadDfobHook(g_dfs, nullptr, nullptr, nullptr, &g_adds, &g_removes); }
uint32_t Res(int site, void** ext, uint32_t objects)
{
    g_lastBytes = 0;
    DfSceneKeepTestResize(site, nullptr, ext, objects * (site ? 48u : 160u), L"x");
    return g_lastBytes / (site ? 48u : 160u);
}
}  // namespace dfscene_test

static void DfSceneRun(const GameModule& game)
{
    using namespace dfscene_test;
    if (game.timeDateStamp == 0xF19077A4u) {
        uint32_t rva[9] = {}, off[12] = {};
        const bool ok = DfSceneKeepTestDiscover(game, rva, off);
        Check(ok && rva[0] == 0x1D96471 && rva[1] == 0x1DACA70 && rva[2] == 0x1DACDA5 && rva[3] == 0x1D973E0 &&
                  rva[4] == 0x1DACF95 && rva[5] == 0xEAFD50,
              "DfSceneKeep: the UpdateDistanceFieldObjectBuffers call, its add and update loops' calls and AddPrimitive's insert "
              "found on the mapped exe");
        Check(ok && rva[6] == 0x1DAD094 && rva[7] == 0x1DAD0B2 && rva[8] == 0xDE6200,
              "DfSceneKeep: the DFObjectData / DFObjectBounds resize calls and the instance list's reallocation found");
        Check(ok && off[0] == 0x470 && off[1] == 0xF8 && off[2] == 0x3368 && off[3] == 0x450 && off[4] == 8 && off[5] == 0x250 &&
                  off[6] == 0xD0 && off[7] == 16,
              "DfSceneKeep: pending set +0x470, indices +0xF8, scene +0x3368, instance list +0x450 (208 B, align 16), proxy +8, "
              "radius +0x250");
        Check(ok && off[8] == 0x58 && off[9] == 0x548 && off[10] == 0xB30 && off[11] == 0xAE8,
              "DfSceneKeep: the renderer's Scene +0x58 and Views +0x548, the view's ViewLocation +0xB30 and ViewOrigin +0xAE8");
        if (!ok || rva[0] != 0x1D96471 || off[3] != 0x450)
            printf("    found %d: %X %X %X %X %X %X %X %X %X | %X %X %X %X %X %X %X %X\n", ok, rva[0], rva[1], rva[2], rva[3],
                   rva[4], rva[5], rva[6], rva[7], rva[8], off[0], off[1], off[2], off[3], off[4], off[5], off[6], off[7]);
        if (ok) printf("    renderer Scene +%X Views +%X, view location +%X origin +%X\n", off[8], off[9], off[10], off[11]);
    }
    // ---- the render-thread camera read (DfAddSpread's near test) with a fake renderer and view ----
    {
        alignas(16) static uint8_t renderer[0x600] = {}, view[0xC00] = {};
        uint8_t* slot = renderer;
        int sceneTag = 0;
        void* scenePtr = &sceneTag;
        void* viewPtr = view;
        int32_t one = 1;
        memcpy(renderer + 0x58, &scenePtr, sizeof scenePtr);
        memcpy(renderer + 0x548, &viewPtr, sizeof viewPtr);
        memcpy(renderer + 0x550, &one, 4);
        const double loc[3] = {1000.0, -2000.0, 300.0};
        memcpy(view + 0xB30, loc, sizeof loc);
        memcpy(view + 0xAE8, loc, sizeof loc);
        DfSceneKeepTestFakes t{};
        t.rendererSlot = &slot;
        t.sceneOff = 0x58;
        t.viewsOff = 0x548;
        t.locOff = 0xB30;
        t.originOff = 0xAE8;
        DfSceneKeepTestSetup(t);
        double x = 0, y = 0, z = 0;
        const bool got = DfSceneKeepTestCamera(scenePtr, &x, &y, &z) && x == 1000.0 && y == -2000.0 && z == 300.0;
        int otherScene = 0;
        const bool wrongScene = !DfSceneKeepTestCamera(&otherScene, &x, &y, &z);
        const double moved[3] = {5000.0, -2000.0, 300.0};
        memcpy(view + 0xAE8, moved, sizeof moved);
        const bool disagree = !DfSceneKeepTestCamera(scenePtr, &x, &y, &z);
        memcpy(view + 0xAE8, loc, sizeof loc);
        int32_t zero = 0;
        memcpy(renderer + 0x550, &zero, 4);
        const bool noViews = !DfSceneKeepTestCamera(scenePtr, &x, &y, &z);
        slot = nullptr;
        const bool noRenderer = !DfSceneKeepTestCamera(scenePtr, &x, &y, &z);
        Check(got && wrongScene && disagree && noViews && noRenderer,
              "DfAddSpread camera: this frame's view location read from the renderer; another scene, a location that is not the "
              "view origin, no views or no renderer: unknown");
    }
    uint64_t c0[8] = {}, c1[8] = {};
    // ---- 1. DfObjectBufferKeep ----
    {
        void* a = nullptr; void* b = nullptr; void* c = nullptr;  // three buffer owners (their address is the key)
        Setup(1, 0, 0, 4096);
        const bool first = Res(0, &a, 1 << 17) == (1u << 18) && Res(1, &a, 1 << 17) == (1u << 18) && Res(0, &a, 1 << 18) == (1u << 18) &&
                          Res(1, &a, 1 << 18) == (1u << 18);
        Check(first, "DfObjectBufferKeep: a world scene's first 2^17 is made at the first size 2^18 (data and bounds), its 2^18 as asked");
        Check(Res(0, &a, 1 << 17) == (1u << 18) && Res(1, &a, 1 << 17) == (1u << 18),
              "DfObjectBufferKeep: a shrink to 2^17 is asked as the session's 2^18 (data and bounds)");
        Check(Res(0, &b, 1 << 10) == (1u << 10) && Res(0, &b, 1 << 9) == (1u << 10),
              "DfObjectBufferKeep: a small scene (never 16384 objects) keeps its own largest size, not the session's");
        Check(Res(0, &c, 1 << 17) == (1u << 18), "DfObjectBufferKeep: a new world scene starts at the session's largest size");
        Check(Res(0, &a, 1 << 20) == (1u << 20) && Res(0, &a, 1 << 17) == (1u << 18),
              "DfObjectBufferKeep: above the 2^19 cap the game's size passes, and the held size stays at most the capped peak");
        {
            void* e = nullptr; void* f = nullptr; void* g = nullptr;  // a fresh session: the world boundary and growth past the first size
            Setup(1, 0, 0, 4096);
            const bool notWorld = Res(0, &e, 1 << 13) == (1u << 13);              // 8192 objects: not a world, no first size
            const bool atMin = Res(0, &f, 1 << 14) == (1u << 18);              // 16384: a world, the first size
            const bool past = Res(0, &f, 1 << 19) == (1u << 19) && Res(0, &f, 1 << 17) == (1u << 19);  // past it: the session peak
            const bool next = Res(0, &g, 1 << 17) == (1u << 19);              // a later world: the session peak (above the first size)
            Check(notWorld && atMin && past && next,
                  "DfObjectBufferKeep: the first size only for world scenes (>= 16384 objects, not 8192); above it the session peak holds");
        }
        void* d = nullptr;  // a small scene's buffers made at a dead world scene's address: they start afresh
        Setup(1, 0, 0, 4096);
        Res(0, &d, 1 << 17);
        Res(0, &a, 1 << 18);
        d = nullptr;
        Check(Res(0, &d, 1 << 8) == (1u << 8), "DfObjectBufferKeep: an owner whose buffer is not made yet starts afresh (no inherited peak)");
        Setup(0, 0, 0, 4096);
        Check(Res(0, &a, 1 << 18) == (1u << 18) && Res(0, &a, 1 << 17) == (1u << 17), "DfObjectBufferKeep: key 0: the game's sizes");
    }
    // ---- 2. DfMappingReserve ----
    {
        Setup(0, 1, 0, 4096);
        memset(g_dfs2, 0, sizeof g_dfs2);
        Wr(g_dfs2, 0x478, 3843);  // a world build pending
        g_buildTo = 117656;
        g_buildMax = 130816;
        g_allocCalls = 0;
        g_loadingState = 1;
        DfAddSpreadDfobHook(g_dfs2, nullptr, nullptr, nullptr, &g_adds, &g_removes);  // 1st loading call: not yet (boundary frame)
        Check(g_allocCalls == 0 && Rd(g_dfs2, 0x45C) == 130816, "DfMappingReserve: the first loading-screen call reserves nothing");
        DfAddSpreadDfobHook(g_dfs2, nullptr, nullptr, nullptr, &g_adds, &g_removes);  // 2nd: after the call, Num + 1/4
        Check(g_allocCalls == 1 && g_allocNum == 117656 && g_allocMax == 147456 && Rd(g_dfs2, 0x45C) == 147456 &&
                  g_allocBytes == size_t(147456) * 0xD0,
              "DfMappingReserve: behind the screen 117656 in use -> capacity 147456 (Num + 1/4, 1024 steps) with the game's realloc");
        DfAddSpreadDfobHook(g_dfs2, nullptr, nullptr, nullptr, &g_adds, &g_removes);
        Check(g_allocCalls == 1, "DfMappingReserve: no second raise when the capacity is already there");
        Check(!g_allocBad, "DfMappingReserve: the realloc gets the instance list's own address and the element alignment 16");
        g_loadingState = 0;
        g_buildTo = 135612;  // in play the count rises past the reserve's peak; nothing reserved in play
        Wr(g_dfs2, 0x45C, 130816);
        DfAddSpreadDfobHook(g_dfs2, nullptr, nullptr, nullptr, &g_adds, &g_removes);
        Check(g_allocCalls == 1, "DfMappingReserve: nothing reserved in a play frame");
        // next load: a new scene; before its build the session's target (peak 135612 + 1/8)
        memset(g_dfs2, 0, sizeof g_dfs2);
        Wr(g_dfs2, 0x478, 3000);
        g_buildTo = 100000;
        g_buildMax = 110000;
        g_loadingState = 1;
        DfAddSpreadDfobHook(g_dfs2, nullptr, nullptr, nullptr, &g_adds, &g_removes);  // streak 1 (also builds: Num 100000)
        Wr(g_dfs2, 0x478, 3000);
        Wr(g_dfs2, 0x458, 0);
        Wr(g_dfs2, 0x45C, 0);
        g_allocCalls = 0;
        DfAddSpreadDfobHook(g_dfs2, nullptr, nullptr, nullptr, &g_adds, &g_removes);  // streak 2: before the build
        Check(g_allocCalls >= 1 && g_allocNum == 0 && Rd(g_dfs2, 0x45C) >= 152576,
              "DfMappingReserve: before a world build behind the screen, capacity raised to the session's peak + 1/8 (152576)");
        // the cap
        memset(g_dfs2, 0, sizeof g_dfs2);
        g_buildTo = 600000;
        g_buildMax = 600000;
        g_allocCalls = 0;
        DfAddSpreadDfobHook(g_dfs2, nullptr, nullptr, nullptr, &g_adds, &g_removes);
        Check(g_allocCalls == 0, "DfMappingReserve: above the 2^19 cap nothing is reserved");
        // the loading state unknown
        g_loadingState = -1;
        memset(g_dfs2, 0, sizeof g_dfs2);
        g_buildTo = 117656;
        g_buildMax = 130816;
        DfAddSpreadDfobHook(g_dfs2, nullptr, nullptr, nullptr, &g_adds, &g_removes);
        DfAddSpreadDfobHook(g_dfs2, nullptr, nullptr, nullptr, &g_adds, &g_removes);
        Check(g_allocCalls == 0, "DfMappingReserve: loading state unknown: nothing reserved");
        // the screen up for less than the settle time (a fade, the render thread's lagging frame): nothing reserved
        {
            DfSceneKeepTestFakes t{};
            t.dfob = reinterpret_cast<void*>(&Dfob);
            t.resizeAlloc = reinterpret_cast<void*>(&ResizeAlloc);
            t.loading = reinterpret_cast<void*>(&Loading);
            t.setOff = 0x470;
            t.mapOff = 0x450;
            t.elemSize = 0xD0;
            t.mapKey = 1;
            t.budget = 4096;
            t.settleMs = 1.0e9;
            DfSceneKeepTestSetup(t);
            g_loadingState = 1;
            memset(g_dfs2, 0, sizeof g_dfs2);
            g_buildTo = 117656;
            g_buildMax = 130816;
            for (int i = 0; i < 5; ++i) DfAddSpreadDfobHook(g_dfs2, nullptr, nullptr, nullptr, &g_adds, &g_removes);
            Check(g_allocCalls == 0, "DfMappingReserve: five calls within the settle time after the screen came up: nothing reserved");
        }
        g_loadingState = 0;
        g_buildTo = 0;
    }
    // ---- 3. DfAddSpread (the DfAddSpread cases of 2026-11-19, then near and oldest-first) ----
    // (1) 10 primitives of 1000 instances, budget 4096: 5 added in the first update, 5 wait and are added in the next one
    Setup(0, 0, 1, 4096);
    DfSceneKeepTestCounts(c0);
    Reset(10, 1000);
    Update();
    Check(g_added.size() == 5 && NumObjects() == 5000 && g_set.size() == 5,
          "DfAddSpread: budget 4096, 10 x 1000 far instances: 5 primitives added, 5 back in the game's set");
    bool order = true;
    for (int i = 0; i < 5; ++i) order = order && g_added[i] == &g_prim[i] && g_set[i] == &g_prim[5 + i];
    Check(order, "DfAddSpread: the first five in the game's order added, the other five put back in order");
    Update();
    Check(g_added.size() == 10 && NumObjects() == 10000 && g_set.empty(),
          "DfAddSpread: the next update adds the rest (10000 instances in all, as stock in one)");
    DfSceneKeepTestCounts(c1);
    Check(c1[0] - c0[0] == 1 && c1[1] - c0[1] == 5, "DfAddSpread: one spread update, 5 primitives held back");
    // (2) budget 256, 10 x 1000: one per update until the waiting ones have been held 3 times; then all go through
    Setup(0, 0, 1, 256);
    DfSceneKeepTestCounts(c0);
    Reset(10, 1000);
    Update();
    Update();
    Update();
    const size_t after3 = g_added.size();
    Update();
    DfSceneKeepTestCounts(c1);
    Check(after3 == 3 && g_added.size() == 10 && g_set.empty() && NumObjects() == 10000,
          "DfAddSpread: a primitive held back 3 times goes through: all 10 added by the 4th update (1, 1, 1, then 7)");
    Check(c1[2] - c0[2] == 6 && c1[4] == 3, "DfAddSpread: 6 through after 3 holds, most holds 3");
    // (3) key 0: stock
    Setup(0, 0, 0, 256);
    Reset(10, 1000);
    Update();
    Check(g_added.size() == 10 && g_set.empty(), "DfAddSpread: key 0: every pending primitive in one update (stock)");
    // (4) more than 1024 pending: stock
    Setup(0, 0, 1, 256);
    DfSceneKeepTestCounts(c0);
    {
        static FakePrim many[1100];
        Reset(0, 0);
        for (FakePrim& m : many) { m = FakePrim{1, 0, g_proxy[0]}; g_set.push_back(&m); }
        SyncNum();
        Update();
        DfSceneKeepTestCounts(c1);
        Check(g_added.size() == 1100 && g_set.empty() && c1[3] - c0[3] == 1,
              "DfAddSpread: 1100 pending primitives (a level load): all in one update (stock), counted");
    }
    // (5) another indices array: passed on; (6) outside our update hook: passed on
    g_wrongIndices = true;
    Reset(10, 1000);
    Update();
    g_wrongIndices = false;
    Check(g_added.size() == 10 && g_set.empty(), "DfAddSpread: a call with another indices array is passed on (stock)");
    Reset(0, 0);
    g_prim[0].inst = 7;
    DfAddSpreadAddHook(true, nullptr, &g_prim[0], nullptr, g_dfs + 0xF8, &g_adds, &g_removes);
    Check(g_added.size() == 1 && NumObjects() == 7, "DfAddSpread: the add loop's call outside our update hook is passed on");
    // (7) near the player: never held (prims 6..9 within 60 m of the player at the origin); budget 256
    Setup(0, 0, 1, 256);
    DfSceneKeepTestCounts(c0);
    Reset(10, 1000);
    for (int i = 6; i < 10; ++i) PlacePrim(i, 3000.0 + 100 * i, 0, 0, 500.0);  // 25-34 m away (bounds surface)
    PlacePrim(5, 7000.0, 0, 0, 1500.0);  // centre 70 m, surface 55 m: near
    Update();
    DfSceneKeepTestCounts(c1);
    bool nearIn = true;
    for (int i = 5; i < 10; ++i) nearIn = nearIn && std::find(g_added.begin(), g_added.end(), &g_prim[i]) != g_added.end();
    Check(nearIn && g_added.size() == 6 && g_set.size() == 4 && c1[5] - c0[5] == 5,
          "DfAddSpread: primitives within 60 m of the camera (bounds surface) go in at once; of the far ones 1 added, 4 wait");
    // (8) the player's position unknown: nothing held
    g_posKnown = false;
    Setup(0, 0, 1, 256);
    Reset(10, 1000);
    Update();
    g_posKnown = true;
    Check(g_added.size() == 10 && g_set.empty(), "DfAddSpread: camera unknown: every primitive in one update");
    // (9) oldest first: after a spread (3..9 waiting), 5 is removed by the game and a new primitive (20) takes a free slot
    // before the older ones; budget 2500: it goes in, and the older ones then go in over the budget rather than wait behind it
    Setup(0, 0, 1, 2500);
    DfSceneKeepTestCounts(c0);
    Reset(10, 1000);
    Update();  // 0, 1, 2 added (3000 >= 2500 before the 4th), 3..9 wait
    g_set = {&g_prim[3], &g_prim[20], &g_prim[4], &g_prim[6], &g_prim[7], &g_prim[8], &g_prim[9]};  // 5 removed; 20 in its slot
    SyncNum();
    g_added.clear();
    Update();
    DfSceneKeepTestCounts(c1);
    bool olderIn = g_added.size() == 7 && g_added[0] == &g_prim[3] && g_added[1] == &g_prim[20] && g_set.empty();
    Check(olderIn && c1[6] - c0[6] == 4,
          "DfAddSpread: a new primitive that went in before older held-back ones: those go in too, over the budget (oldest first)");
    // (10) the same without a free slot: the new one at the end waits with the older ones still over the budget
    Setup(0, 0, 1, 2500);
    Reset(10, 1000);
    Update();  // 0, 1, 2 added, 3..9 wait
    g_set.push_back(&g_prim[20]);
    SyncNum();
    g_added.clear();
    Update();  // 3, 4, 5 added; 6..9 and 20 wait
    olderIn = g_added.size() == 3 && g_added[0] == &g_prim[3] && g_set.size() == 5 && g_set[4] == &g_prim[20];
    Check(olderIn, "DfAddSpread: without a free slot the older ones go first, the new one waits at the end");
    // (11) removes of held-back primitives, then an update under the budget: nothing held
    Setup(0, 0, 1, 4096);
    Reset(10, 1000);
    Update();  // 0..4 added, 5..9 wait
    g_set = {&g_prim[8], &g_prim[9], &g_prim[21]};  // 5, 6, 7 removed by the game; one new (3000 instances in all)
    SyncNum();
    g_added.clear();
    Update();
    Check(g_added.size() == 3 && g_set.empty(),
          "DfAddSpread: held-back primitives removed by the game, then an update under the budget: nothing held");
    // (12) a stock update in between (key 0), then an update under the budget: nothing held
    Setup(0, 0, 1, 4096);
    Reset(10, 1000);
    Update();  // 5..9 wait
    DfSceneKeepTestKeys(0, 0, 0);
    Update();  // the game adds them all
    const bool stockAll = g_set.empty() && g_added.size() == 10;
    DfSceneKeepTestKeys(0, 0, 1);
    g_set = {&g_prim[22], &g_prim[23], &g_prim[24]};
    SyncNum();
    g_added.clear();
    Update();
    Check(stockAll && g_added.size() == 3 && g_set.empty(),
          "DfAddSpread: a stock update in between ends the episode; the next update under the budget holds nothing");
    // (13) exactly at the budget: 4 x 1024 = 4096 instances: nothing held
    Setup(0, 0, 1, 4096);
    Reset(4, 1024);
    Update();
    Check(g_added.size() == 4 && g_set.empty(), "DfAddSpread: an update adding exactly the budget holds nothing");
    // leave the fakes switched off
    Setup(0, 0, 0, 4096);
}
