// LumenScanCull self-test (public since 1.4.0), included by selftest.cpp: the sites on the mapped exe, the call-site stub, and
// the cull against the game's OWN scan body (the mapped exe's FLumenSurfaceCacheUpdatePrimitivesTask::AnyThreadTask, called on
// fake groups and tasks; its output arrays are given room for every group, so it never grows them and stays a leaf).
#pragma once
#include <intrin.h>

namespace lscan {

constexpr uint32_t kStride = 0x50, kTaskSize = 0x98, kListOff = 0x180, kPgOff = 0x1B0, kViewSpecOff = 0x944;

struct Fake {
    std::vector<uint8_t> lsd = std::vector<uint8_t>(0x2000, 0);
    std::vector<uint8_t> groups;
    std::vector<uint32_t> bits;
    std::vector<int32_t> list;  // PrimitiveGroupIndicesToUpdateInBuffer
    int32_t num = 0;
    uint8_t* Pg() { return lsd.data() + kPgOff; }
    uint8_t* G(int32_t g) { return groups.data() + static_cast<size_t>(g) * kStride; }
    void Sync()  // the sparse array's and the list's fields in the fake LumenSceneData
    {
        uint8_t* const pg = Pg();
        uint8_t* d = groups.data();
        memcpy(pg, &d, 8);
        memcpy(pg + 8, &num, 4);
        memcpy(pg + 12, &num, 4);
        uint32_t* b = bits.data();
        memcpy(pg + 0x20, &b, 8);
        const int32_t nb = num;
        memcpy(pg + 0x28, &nb, 4);
        int32_t* l = list.data();
        const int32_t ln = static_cast<int32_t>(list.size());
        memcpy(lsd.data() + kListOff, &l, 8);
        memcpy(lsd.data() + kListOff + 8, &ln, 4);
        memcpy(lsd.data() + kListOff + 12, &ln, 4);
    }
    void SetAlloc(int32_t g, bool on) { if (on) bits[g >> 5] |= 1u << (g & 31); else bits[g >> 5] &= ~(1u << (g & 31)); }
    bool Alloc(int32_t g) const { return (bits[g >> 5] >> (g & 31)) & 1u; }
};

struct Rng {
    uint64_t s;
    uint32_t U() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>(s >> 33); }
    float F(float a, float b) { return a + (b - a) * static_cast<float>(U() & 0xFFFFFF) / 16777216.0f; }
    bool P(int percent) { return static_cast<int>(U() % 100) < percent; }
};

void SetGroup(Fake& f, int32_t g, float cx, float cy, float cz, float ext, Rng& r)
{
    uint8_t* const e = f.G(g);
    memset(e, 0, kStride);
    const int32_t m1 = -1;
    memcpy(e + 0x1C, &m1, 4);  // MeshCardsIndex
    memcpy(e + 0x20, &m1, 4);  // HeightfieldIndex
    const float mn[3] = {cx - ext, cy - ext * r.F(0.2f, 1.0f), cz - ext * 0.5f}, mx[3] = {cx + ext * r.F(0.2f, 1.0f), cy + ext, cz + ext * 0.5f};
    memcpy(e + 0x24, mn, 12);
    memcpy(e + 0x30, mx, 12);
    memcpy(e + 0x3C, &m1, 4);  // RayTracingGroupMapElementId: none
    e[0x44] = r.P(92) ? 1 : 0;  // bValidMeshCards
    e[0x45] = r.P(1) ? 1 : 0;   // bFarField
    e[0x47] = r.P(2) ? 1 : 0;   // bEmissiveLightSource
}

struct Task { std::vector<uint8_t> t = std::vector<uint8_t>(kTaskSize, 0); std::vector<uint64_t> adds; std::vector<int32_t> removes; };

void MakeTasks(std::vector<Task>& ts, Fake& f, int32_t perTask, const double views[][3], int nv, float maxDist, float tds, int32_t minRes, int32_t capacity)
{
    const int32_t n = (f.num + perTask - 1) / perTask;
    ts.resize(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        Task& k = ts[static_cast<size_t>(i)];
        std::fill(k.t.begin(), k.t.end(), uint8_t(0));
        k.adds.assign(static_cast<size_t>(capacity), 0);
        k.removes.assign(static_cast<size_t>(capacity), 0);
        uint8_t* const t = k.t.data();
        uint64_t* a = k.adds.data();
        int32_t* rm = k.removes.data();
        memcpy(t + 0x00, &a, 8);
        memcpy(t + 0x0C, &capacity, 4);
        memcpy(t + 0x10, &rm, 8);
        memcpy(t + 0x1C, &capacity, 4);
        uint8_t* pg = f.Pg();
        memcpy(t + 0x30, &pg, 8);
        memcpy(t + 0x38, views, static_cast<size_t>(nv) * 24);  // inline (2), heap pointer 0
        memcpy(t + 0x70, &nv, 4);
        const int32_t two = 2, first = i * perTask;
        memcpy(t + 0x74, &two, 4);
        memcpy(t + 0x78, &first, 4);
        memcpy(t + 0x7C, &perTask, 4);
        const float detail = 1.0f, ffMaxSq = 1e12f, ffTd = 0.001f;
        memcpy(t + 0x80, &detail, 4);
        memcpy(t + 0x84, &maxDist, 4);
        memcpy(t + 0x88, &tds, 4);
        memcpy(t + 0x8C, &minRes, 4);
        memcpy(t + 0x90, &ffMaxSq, 4);
        memcpy(t + 0x94, &ffTd, 4);
    }
}

int32_t Num(const Task& k, uint32_t off) { int32_t n; memcpy(&n, k.t.data() + off + 8, 4); return n; }

// A world, frames of moving views; the game's own processing of the requests simulated (adds get cards, removes lose them, both
// listed), random changes listed, the lists reset at frame end. Returns failed-check bits.
int Simulate(void* body, int frames, int perTask, bool unlisted, uint64_t* outMismatch, uint64_t* outSkipped, uint64_t* outScanned, int* outCompared)
{
    Fake f;
    Rng r{12345};
    const int32_t blocks = 400;
    f.num = blocks * 64 + 37;
    f.groups.assign(static_cast<size_t>(f.num) * kStride, 0);
    f.bits.assign(static_cast<size_t>((f.num + 31) / 32 + 1), 0);
    // Index blocks mostly follow space (one primitive's instances in a cell), some scattered.
    for (int32_t g = 0; g < f.num; ++g) {
        const int32_t cell = g / 96;
        Rng rc{static_cast<uint64_t>(cell) * 7919u + 3};
        const float cx = rc.F(-300000.0f, 300000.0f), cy = rc.F(-300000.0f, 300000.0f);
        const bool scatter = r.P(0) || (g % 997) == 5;  // a few groups far from their block's place
        SetGroup(f, g, scatter ? r.F(-300000.0f, 300000.0f) : cx + r.F(-4000.0f, 4000.0f), scatter ? r.F(-300000.0f, 300000.0f) : cy + r.F(-4000.0f, 4000.0f),
                 r.F(-500.0f, 3000.0f), r.P(5) ? r.F(500.0f, 3000.0f) : r.F(10.0f, 300.0f), r);
        if (r.P(97)) f.SetAlloc(g, true);
        if (g % 5000 == 7) { const int32_t rt = 3; memcpy(f.G(g) + 0x3C, &rt, 4); }  // a merged-components group
        if (g % 7000 == 11) { const float nan = std::nanf(""); memcpy(f.G(g) + 0x28, &nan, 4); }  // a broken box
    }
    f.Sync();
    int fail = 0;
    if (!LumenScanCullTestInit(body, kPgOff, kListOff, kViewSpecOff)) return 1;
    LumenScanCullTestMode(2);
    double views[2][3] = {{0, 0, 200}, {50000, 0, 200}};
    std::vector<Task> ts, ref;
    int32_t nextCard = 0;
    int compared = 0;
    for (int fr = 0; fr < frames; ++fr) {
        // Views move (a ride), sometimes jump (fast travel); one or two views.
        const int nv = (fr / 40) % 3 == 2 ? 2 : 1;
        if (fr % 60 == 59) { views[0][0] = r.F(-250000.0f, 250000.0f); views[0][1] = r.F(-250000.0f, 250000.0f); }
        views[0][0] += 900.0; views[0][1] += 300.0 * std::sin(fr * 0.05);
        views[1][0] = views[0][0] + 30000.0;
        const float maxDist = fr % 100 < 50 ? 20000.0f : 35000.0f;  // a parameter change
        const float tds = fr % 70 < 35 ? 1.0f : 4.0f;
        const int32_t minRes = 4;
        // Random changes, listed as the game lists them.
        for (int k = 0; k < 20; ++k) {
            const int32_t g = static_cast<int32_t>(r.U() % static_cast<uint32_t>(f.num));
            const int what = static_cast<int>(r.U() % 4);
            if (what == 0) { SetGroup(f, g, static_cast<float>(views[0][0]) + r.F(-30000.0f, 30000.0f), static_cast<float>(views[0][1]) + r.F(-30000.0f, 30000.0f), 0.0f, r.F(20.0f, 400.0f), r); f.SetAlloc(g, true); }
            else if (what == 1) { f.SetAlloc(g, !f.Alloc(g)); }
            else if (what == 2) { float mn[3]; memcpy(mn, f.G(g) + 0x24, 12); mn[0] -= 5000.0f; memcpy(f.G(g) + 0x24, mn, 12); }
            else { f.G(g)[0x44] ^= 1; }
            f.list.push_back(g);
        }
        if (unlisted && fr == frames / 2) {
            // A change nobody lists: groups of a skipped far block moved right next to the view. Proof must see it.
            for (int32_t g = 64 * 100; g < 64 * 140; ++g) {  // 40 blocks: most of them are skipped then
                if (!f.Alloc(g)) continue;
                float mn[3] = {static_cast<float>(views[0][0]) - 50, static_cast<float>(views[0][1]) - 50, 150}, mx[3] = {mn[0] + 300, mn[1] + 300, 400};
                memcpy(f.G(g) + 0x24, mn, 12);
                memcpy(f.G(g) + 0x30, mx, 12);
                const int32_t m1 = -1, rt = -1;
                memcpy(f.G(g) + 0x1C, &m1, 4);
                memcpy(f.G(g) + 0x3C, &rt, 4);
                f.G(g)[0x44] = 1;
                f.G(g)[0x45] = 0;
            }
        }
        f.Sync();
        // The scan: prep (stub), the tasks (mode 2 compares each with the stock), post.
        LumenScanCullPrep(f.lsd.data());
        MakeTasks(ts, f, perTask, views, nv, maxDist, tds, minRes, f.num);
        for (Task& k : ts) LumenScanCullTask(k.t.data());
        LumenScanCullPost();
        // Every 25th frame also mode 1 against an independent stock run on copies of the tasks.
        if (fr % 25 == 24 && !unlisted) {
            LumenScanCullTestMode(1);
            LumenScanCullPrep(f.lsd.data());
            MakeTasks(ts, f, perTask, views, nv, maxDist, tds, minRes, f.num);
            MakeTasks(ref, f, perTask, views, nv, maxDist, tds, minRes, f.num);
            for (Task& k : ts) LumenScanCullTask(k.t.data());
            LumenScanCullPost();
            for (Task& k : ref) reinterpret_cast<void (*)(void*)>(body)(k.t.data());
            for (size_t i = 0; i < ts.size(); ++i) {
                const int32_t na = Num(ts[i], 0), nr = Num(ts[i], 0x10);
                if (na != Num(ref[i], 0) || nr != Num(ref[i], 0x10) || memcmp(ts[i].adds.data(), ref[i].adds.data(), static_cast<size_t>(na) * 8) ||
                    memcmp(ts[i].removes.data(), ref[i].removes.data(), static_cast<size_t>(nr) * 4))
                    fail |= 2;
                int32_t first;
                memcpy(&first, ts[i].t.data() + 0x78, 4);
                if (first != static_cast<int32_t>(i) * perTask) fail |= 4;  // the range fields restored
            }
            ++compared;
            LumenScanCullTestMode(2);
        }
        // The game's processing: the 2 * captures nearest adds get cards, removes lose them (per task cap ignored here); all listed.
        std::vector<std::pair<float, int32_t>> adds;
        for (Task& k : ts) {
            for (int32_t i = 0; i < Num(k, 0); ++i) {
                int32_t g; float d;
                memcpy(&g, reinterpret_cast<uint8_t*>(&k.adds[static_cast<size_t>(i)]), 4);
                memcpy(&d, reinterpret_cast<uint8_t*>(&k.adds[static_cast<size_t>(i)]) + 4, 4);
                adds.emplace_back(d, g);
            }
            for (int32_t i = 0; i < Num(k, 0x10); ++i) {
                const int32_t g = k.removes[static_cast<size_t>(i)], m1 = -1;
                memcpy(f.G(g) + 0x1C, &m1, 4);
                f.list.push_back(g);
            }
        }
        std::sort(adds.begin(), adds.end());
        for (size_t i = 0; i < adds.size() && i < 64; ++i) {
            const int32_t g = adds[i].second;
            if (r.P(10)) f.G(g)[0x44] = 0;  // could not make cards
            else memcpy(f.G(g) + 0x1C, &nextCard, 4), ++nextCard;
            f.list.push_back(g);
        }
        // The lists' reset (sometimes a second renderer's reset with nothing listed).
        f.Sync();
        LumenScanCullListsReset(f.lsd.data());
        f.list.clear();
        f.Sync();
        if (fr % 9 == 4) LumenScanCullListsReset(f.lsd.data());
    }
    uint64_t verified = 0;
    LumenScanCullTestCounts(outScanned, outSkipped, &verified, outMismatch);
    if (verified == 0) fail |= 8;
    *outCompared = compared;
    LumenScanCullTestDone();
    return fail;
}

// ---- Targeted cases (review lumenscanrev T1-T6): a far world, then one situation each, the cull (mode 1) against an
// independent stock run of the game's body on copies of the tasks. ----
struct Params {
    int32_t perTask = 128;
    int nv = 1;
    double views[9][3] = {};
    float maxDist = 20000.0f, tds = 1.0f, ffMaxSq = 1e12f, ffTd = 0.001f;
    int32_t minRes = 4;
};

void MakeTasksP(std::vector<Task>& ts, Fake& f, const Params& P, std::vector<double>& heap)
{
    double inl[2][3] = {};
    for (int i = 0; i < P.nv && i < 2; ++i) for (int a = 0; a < 3; ++a) inl[i][a] = P.views[i][a];
    MakeTasks(ts, f, P.perTask, inl, P.nv <= 2 ? P.nv : 2, P.maxDist, P.tds, P.minRes, f.num);
    heap.assign(static_cast<size_t>(P.nv) * 3, 0.0);
    for (int i = 0; i < P.nv; ++i) for (int a = 0; a < 3; ++a) heap[static_cast<size_t>(i) * 3 + a] = P.views[i][a];
    for (Task& k : ts) {
        uint8_t* const t = k.t.data();
        if (P.nv > 2) {  // the TInlineAllocator<2>'s heap storage
            double* hp = heap.data();
            memcpy(t + 0x68, &hp, 8);
            memcpy(t + 0x70, &P.nv, 4);
            memcpy(t + 0x74, &P.nv, 4);
        }
        memcpy(t + 0x90, &P.ffMaxSq, 4);
        memcpy(t + 0x94, &P.ffTd, 4);
    }
}

// One scan: the cull's tasks and an independent stock run on copies. Returns the number of tasks whose requests differ;
// *skipped = groups the cull skipped in this scan; *adds = stock add requests (to see that a case really emits something).
int ScanCompare(void* body, Fake& f, const Params& P, uint64_t* skipped, int* adds)
{
    std::vector<Task> ts, ref;
    std::vector<double> h1, h2;
    uint64_t s0 = 0, k0 = 0, v0 = 0, m0 = 0, s1 = 0, k1 = 0, v1 = 0, m1 = 0;
    LumenScanCullTestCounts(&s0, &k0, &v0, &m0);
    f.Sync();
    LumenScanCullPrep(f.lsd.data());
    MakeTasksP(ts, f, P, h1);
    for (Task& k : ts) LumenScanCullTask(k.t.data());
    LumenScanCullPost();
    MakeTasksP(ref, f, P, h2);
    for (Task& k : ref) reinterpret_cast<void (*)(void*)>(body)(k.t.data());
    int diff = 0, a = 0;
    for (size_t i = 0; i < ts.size(); ++i) {
        const int32_t na = Num(ts[i], 0), nr = Num(ts[i], 0x10);
        a += Num(ref[i], 0);
        if (na != Num(ref[i], 0) || nr != Num(ref[i], 0x10) || memcmp(ts[i].adds.data(), ref[i].adds.data(), static_cast<size_t>(na) * 8) ||
            memcmp(ts[i].removes.data(), ref[i].removes.data(), static_cast<size_t>(nr) * 4))
            ++diff;
    }
    LumenScanCullTestCounts(&s1, &k1, &v1, &m1);
    if (skipped) *skipped = k1 - k0;
    if (adds) *adds = a;
    // The lists' reset at frame end, as the game does after every scan.
    LumenScanCullListsReset(f.lsd.data());
    f.list.clear();
    f.Sync();
    return diff;
}

// 200 blocks of small groups 100-300 km away (all skipped once summarised); the view at the origin.
void FarWorld(Fake& f, int blocks)
{
    Rng r{777};
    f.num = blocks * 64;
    f.groups.assign(static_cast<size_t>(f.num) * kStride, 0);
    f.bits.assign(static_cast<size_t>((f.num + 31) / 32 + 1), 0);
    f.list.clear();
    for (int32_t g = 0; g < f.num; ++g) {
        Rng rc{static_cast<uint64_t>(g / 64) * 31337u + 1};
        const float ang = rc.F(0.0f, 6.2831f), dist = rc.F(100000.0f, 300000.0f);
        SetGroup(f, g, dist * std::cos(ang) + r.F(-2000.0f, 2000.0f), dist * std::sin(ang) + r.F(-2000.0f, 2000.0f), 0.0f, r.F(20.0f, 200.0f), r);
        f.G(g)[0x44] = 1;
        f.G(g)[0x45] = 0;
        f.SetAlloc(g, true);
    }
    f.Sync();
}

// A whole block's groups moved next to the view (candidates that pass: adds), listed or not.
void MoveNear(Fake& f, int32_t block, float x, bool list)
{
    for (int32_t g = block * 64; g < block * 64 + 64; ++g) {
        uint8_t* const e = f.G(g);
        const float mn[3] = {x, -300.0f, -300.0f}, mx[3] = {x + 600.0f, 300.0f, 300.0f};
        memcpy(e + 0x24, mn, 12);
        memcpy(e + 0x30, mx, 12);
        const int32_t m1 = -1;
        memcpy(e + 0x1C, &m1, 4);
        memcpy(e + 0x3C, &m1, 4);
        e[0x44] = 1;
        e[0x45] = 0;
        f.SetAlloc(g, true);
        if (list) f.list.push_back(g);
    }
}

// Each case returns failed-check bits (0 = passed).
int CaseModeGap(void* body, bool legacy)  // T1: 1 -> 0, a change listed and reset while off (no scan), -> 1
{
    Fake f;
    FarWorld(f, 200);
    if (!LumenScanCullTestInit(body, kPgOff, kListOff, kViewSpecOff)) return 1;
    LumenScanCullTestLegacyGap(legacy);
    LumenScanCullTestMode(1);
    Params P;
    P.tds = 100.0f;
    uint64_t sk = 0;
    int fail = 0;
    if (ScanCompare(body, f, P, &sk, nullptr)) fail |= 2;
    if (ScanCompare(body, f, P, &sk, nullptr) || sk == 0) fail |= 4;  // summarised: far blocks skipped
    LumenScanCullTestMode(0);
    MoveNear(f, 50, 500.0f, true);  // a cell streams in while off
    f.Sync();
    LumenScanCullListsReset(f.lsd.data());  // a renderer's reset, no scan
    f.list.clear();
    f.Sync();
    LumenScanCullTestMode(1);
    int adds = 0;
    const int d = ScanCompare(body, f, P, &sk, &adds);
    if (adds == 0) fail |= 8;  // the case must emit something
    if (d) fail |= 16;
    LumenScanCullTestLegacyGap(false);
    LumenScanCullTestDone();
    return fail;
}

// T7 (public port, lumenpub): the oracles catch a wrong decision. A block near the view that emits adds every scan gets a
// forged summary (its box 300 km away, near candidates only, no "added" flag): the cull then skips it through the distance
// test. Mode 1 must differ from the independent stock run; mode 2 must count mismatches and still hand over the stock output.
// The same forgery with the true box must not differ (the forgery is what breaks it, not the test setup).
int CaseWrongSkip(void* body)
{
    Fake f;
    FarWorld(f, 200);
    if (!LumenScanCullTestInit(body, kPgOff, kListOff, kViewSpecOff)) return 1;
    LumenScanCullTestMode(1);
    Params P;
    P.tds = 100.0f;
    uint64_t sk = 0;
    int fail = 0, adds = 0;
    MoveNear(f, 50, 500.0f, true);
    if (ScanCompare(body, f, P, &sk, &adds) || adds == 0) fail |= 2;  // near block scanned (listed), emits adds, matches stock
    int32_t nb = 0;
    LumenScanBlock* const blocks = LumenScanBlocks(&nb);
    if (!blocks || nb <= 50 || !(blocks[50].flags & 64)) return fail | 4;  // its summary says "added"
    const auto Forge = [&](float at) {
        LumenScanBlock& s = blocks[50];
        s.flags = 128;  // near candidates only: no kAlways flag
        for (int a = 0; a < 3; ++a) { s.lo[a] = at - 300.0f; s.hi[a] = at + 300.0f; }
        s.lo[0] = at;
        s.hi[0] = at + 600.0f;
        s.maxExt = 300.0f;
    };
    Forge(500.0f);  // the true box, flags without "added": still scanned by the distance test, same as stock
    if (ScanCompare(body, f, P, &sk, &adds) || adds == 0) fail |= 8;
    Forge(300000.0f);  // forged: far away
    if (ScanCompare(body, f, P, &sk, &adds) == 0) fail |= 16;  // mode 1: the wrong skip shows against stock
    LumenScanCullTestMode(2);
    // A mode change marks everything dirty (the forgery would be rebuilt away): forge again after the next Prep has run, so
    // the proof sees a wrong skip. Prep runs inside ScanCompare; scan once to settle, then forge and scan.
    if (ScanCompare(body, f, P, &sk, &adds)) fail |= 32;
    uint64_t s0 = 0, k0 = 0, v0 = 0, m0 = 0, s1 = 0, k1 = 0, v1 = 0, m1 = 0;
    LumenScanCullTestCounts(&s0, &k0, &v0, &m0);
    Forge(300000.0f);
    if (ScanCompare(body, f, P, &sk, &adds)) fail |= 64;  // mode 2 hands the game the stock output
    LumenScanCullTestCounts(&s1, &k1, &v1, &m1);
    if (m1 == m0) fail |= 128;  // ... and counts the mismatch
    LumenScanCullTestDone();
    return fail;
}

int CaseCopy(void* body)  // T2: same LumenSceneData address, contents replaced without listing (CopyInitialData)
{
    Fake f;
    FarWorld(f, 200);
    if (!LumenScanCullTestInit(body, kPgOff, kListOff, kViewSpecOff)) return 1;
    LumenScanCullTestMode(1);
    Params P;
    P.tds = 100.0f;
    uint64_t sk = 0;
    int fail = 0, adds = 0;
    ScanCompare(body, f, P, &sk, nullptr);
    if (ScanCompare(body, f, P, &sk, nullptr) || sk == 0) fail |= 2;
    // A new copy of the groups (new element storage), one block's groups now near the view, MeshCardsIndex -1, nothing listed.
    std::vector<uint8_t> copy(f.groups);
    std::vector<uint32_t> bitsCopy(f.bits);
    f.groups.swap(copy);
    f.bits.swap(bitsCopy);
    MoveNear(f, 60, 400.0f, false);
    if (ScanCompare(body, f, P, &sk, &adds)) fail |= 4;
    if (adds == 0) fail |= 8;
    // In place (same storage) but view-specific: that scan runs stock.
    ScanCompare(body, f, P, &sk, nullptr);
    f.lsd[kViewSpecOff] = 1;
    MoveNear(f, 70, 450.0f, false);
    if (ScanCompare(body, f, P, &sk, &adds)) fail |= 16;
    if (sk != 0) fail |= 32;
    f.lsd[kViewSpecOff] = 0;
    LumenScanCullTestDone();
    return fail;
}

int CaseShrinkRegrow(void* body)  // T3: the count shrinks and grows back to the same value between scans, changes listed
{
    Fake f;
    FarWorld(f, 200);
    if (!LumenScanCullTestInit(body, kPgOff, kListOff, kViewSpecOff)) return 1;
    LumenScanCullTestMode(1);
    Params P;
    P.tds = 100.0f;
    uint64_t sk = 0;
    int fail = 0, adds = 0;
    ScanCompare(body, f, P, &sk, nullptr);
    ScanCompare(body, f, P, &sk, nullptr);
    const int32_t full = f.num;
    for (int32_t g = full - 200; g < full; ++g) { f.SetAlloc(g, false); f.list.push_back(g); }  // freed (listed) and consolidated
    f.num = full - 200;
    f.Sync();
    f.num = full;  // grown back before the next scan: new groups near the view, listed
    for (int32_t b = (full - 200) / 64; b < full / 64; ++b) MoveNear(f, b, 300.0f + 50.0f * static_cast<float>(b), true);
    if (ScanCompare(body, f, P, &sk, &adds)) fail |= 2;
    if (adds == 0) fail |= 4;
    LumenScanCullTestDone();
    return fail;
}

int CaseViews(void* body)  // T4: three views (heap storage) and nine (more than the cull takes: stock)
{
    Fake f;
    FarWorld(f, 200);
    if (!LumenScanCullTestInit(body, kPgOff, kListOff, kViewSpecOff)) return 1;
    LumenScanCullTestMode(1);
    Params P;
    P.tds = 100.0f;
    P.nv = 3;
    P.views[1][0] = 150000.0;  // the other views near some far blocks
    P.views[2][1] = -200000.0;
    uint64_t sk = 0;
    int fail = 0;
    ScanCompare(body, f, P, &sk, nullptr);
    if (ScanCompare(body, f, P, &sk, nullptr) || sk == 0) fail |= 2;
    MoveNear(f, 30, 500.0f, true);
    int adds = 0;
    if (ScanCompare(body, f, P, &sk, &adds) || adds == 0) fail |= 4;
    P.nv = 9;
    for (int i = 3; i < 9; ++i) { P.views[i][0] = -100000.0 * i; P.views[i][1] = 1000.0 * i; }
    if (ScanCompare(body, f, P, &sk, nullptr)) fail |= 8;
    if (sk != 0) fail |= 16;  // nine views: stock
    LumenScanCullTestDone();
    return fail;
}

int CaseEdges(void* body)  // T5: values exactly on the thresholds; far-field candidates that pass
{
    Fake f;
    FarWorld(f, 200);
    if (!LumenScanCullTestInit(body, kPgOff, kListOff, kViewSpecOff)) return 1;
    LumenScanCullTestMode(1);
    Params P;
    P.tds = 100.0f;
    P.ffTd = 0.1f;
    uint64_t sk = 0;
    int fail = 0, adds = 0;
    ScanCompare(body, f, P, &sk, nullptr);
    ScanCompare(body, f, P, &sk, nullptr);
    const auto put = [&](int32_t g, const float mn[3], const float mx[3], bool isFar) {
        uint8_t* const e = f.G(g);
        memcpy(e + 0x24, mn, 12);
        memcpy(e + 0x30, mx, 12);
        const int32_t m1 = -1;
        memcpy(e + 0x1C, &m1, 4);
        memcpy(e + 0x3C, &m1, 4);
        e[0x44] = 1;
        e[0x45] = isFar ? 1 : 0;
        e[0x47] = 0;
        f.SetAlloc(g, true);
        f.list.push_back(g);
    };
    // D exactly MaxDist^2 (20000^2 = 4e8, exact in float), R = 100 * 800 / 20000 + 0.01 = 4.01: passes.
    const float a0[3] = {20000.0f, -800.0f, -800.0f}, a1[3] = {21600.0f, 800.0f, 800.0f};
    put(80 * 64, a0, a1, false);
    // One float step farther: fails (the stock's own decision either way; the block must be read, not guessed).
    const float b0[3] = {std::nextafterf(20000.0f, 1e9f), -800.0f, -800.0f}, b1[3] = {21600.0f, 800.0f, 800.0f};
    put(81 * 64, b0, b1, false);
    // R right at MinCardResolution: extent 39900 at D = 1e8 with tds 1 gives 3.99 + 0.01 (and its float neighbours).
    for (int k = 0; k < 5; ++k) {
        const float ext = std::nextafterf(39900.0f, k < 2 ? 0.0f : 1e9f) + static_cast<float>(k - 2) * 0.004f;
        const float c0[3] = {10000.0f, -ext, -ext}, c1[3] = {10000.0f + 2.0f * ext, ext, ext};
        put(82 * 64 + k, c0, c1, false);
    }
    // Far field: 50 000 away, extent 100 * 0.1 = 10 >= 4: passes (add) on the far branch.
    const float d0[3] = {50000.0f, -100.0f, -100.0f}, d1[3] = {50200.0f, 100.0f, 100.0f};
    put(83 * 64, d0, d1, true);
    if (ScanCompare(body, f, P, &sk, &adds)) fail |= 2;
    if (adds < 2) fail |= 4;
    P.tds = 1.0f;  // the R-edge block's own setting
    if (ScanCompare(body, f, P, &sk, &adds)) fail |= 8;
    LumenScanCullTestDone();
    return fail;
}

// The stub: Prep sees the LumenSceneData, the original all five arguments, Post runs after it, a stack walk from the original
// gets through the stub to its caller.
void* g_seenPrep = nullptr;
int g_posts = 0, g_order = 0, g_prepAt = 0, g_postAt = 0, g_origAt = 0;
struct Seen { void* a; void* b; float c, d; void* e; USHORT frames; void* caller; } g_seen{};
void* g_callStubReturn = nullptr;
void* g_frames[8] = {};
void TestPrep(void* lsd) { g_seenPrep = lsd; g_prepAt = ++g_order; }
void TestPost() { ++g_posts; g_postAt = ++g_order; }
__declspec(noinline) void TestOriginal(void* a, void* b, float c, float d, void* e)
{
    g_origAt = ++g_order;
    g_seen = {a, b, c, d, e, 0, nullptr};
    // A real unwind from here: this function, the stub (its dynamic unwind data), CallStub.
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    USHORT n = 0;
    for (; n < 8; ++n) {
        g_frames[n] = reinterpret_cast<void*>(ctx.Rip);
        DWORD64 base = 0;
        RUNTIME_FUNCTION* const rf = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!rf) break;
        void* handler = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &handler, &establisher, nullptr);
        if (!ctx.Rip) break;
    }
    g_seen.frames = n;
    void** const frames = g_frames;
    g_seen.caller = frames[3];  // [0] here, [1] the stub, [2] CallStub, [3] CallStub's return address
}
__declspec(noinline) int CallStub(uint8_t* stub, void** retAddr)
{
    *retAddr = _ReturnAddress();
    using Fn = void (*)(void*, void*, float, float, void*);
    reinterpret_cast<Fn>(stub)(reinterpret_cast<void*>(0x1111), reinterpret_cast<void*>(0x2222), 1.5f, 2.5f, reinterpret_cast<void*>(0x5555));
    return 1;
}
int TestStub()
{
    uint8_t* const stub = LumenScanCullTestStub(reinterpret_cast<const void*>(&TestPrep), reinterpret_cast<const void*>(&TestOriginal), reinterpret_cast<const void*>(&TestPost));
    if (!stub) return 1;
    void* ret = nullptr;
    CallStub(stub, &ret);
    int fail = 0;
    if (g_seenPrep != reinterpret_cast<void*>(0x1111)) fail |= 2;
    if (g_seen.a != reinterpret_cast<void*>(0x1111) || g_seen.b != reinterpret_cast<void*>(0x2222) || g_seen.c != 1.5f || g_seen.d != 2.5f || g_seen.e != reinterpret_cast<void*>(0x5555)) fail |= 4;
    if (g_posts != 1 || !(g_prepAt < g_origAt && g_origAt < g_postAt)) fail |= 8;
    // The walk gets through the stub: its fourth frame is CallStub's own return address.
    if (g_seen.frames < 4 || g_seen.caller != ret) {
        fail |= 16;
        printf("    walk: %u frames, want %p at [3]; stub %p:", g_seen.frames, ret, static_cast<void*>(stub));
        for (int i = 0; i < 8; ++i) printf(" %p", g_frames[i]);
        printf("\n");
    }
    LumenScanCullTestFreeStub(stub);
    return fail;
}

void Run(const GameModule& game)
{
    uint32_t rva[4] = {};
    uint32_t pgOff = 0, viewSpec = 0;
    const int ok = LumenScanCullTestDiscover(game, rva, &pgOff, &viewSpec);
    Check(ok == 1, "LumenScanCull: the scan body, its thunk, UpdateSurfaceCachePrimitives, its call and the group layout found by pattern");
    if (game.timeDateStamp == 0xF19077A4u) {
        const bool want = ok == 1 && rva[0] == 0x1FBF320 && rva[1] == 0x1FC1603 && rva[2] == 0x1FE4E90 && rva[3] == 0x1FBAFF0 && pgOff == 0x1B0 && viewSpec == 0x944;
        Check(want, "LumenScanCull: every site and offset match build 19115871's");
        if (!want) printf("    found %d: body 0x%X thunk jmp 0x%X dispatch 0x%X call 0x%X groups +0x%X\n", ok, rva[0], rva[1], rva[2], rva[3], pgOff);
    }
    const int s = TestStub();
    Check(s == 0, "LumenScanCull: the call-site stub passes all five arguments, runs Prep before and Post after, walks to its caller");
    if (s) printf("    failed checks (bits): 0x%X\n", s);
    if (ok != 1) return;
    void* const body = game.base + rva[0];
    uint64_t mm = 0, skipped = 0, scanned = 0;
    int compared = 0;
    int fail = Simulate(body, 300, 128, false, &mm, &skipped, &scanned, &compared);
    const bool good = fail == 0 && mm == 0 && skipped > scanned / 2 && compared >= 10;
    Check(good, "LumenScanCull: 300 frames of the game's own scan body on fake groups (moves, jumps, two views, parameter changes, listed changes, card adds and removes, "
                "merged and broken groups): proof and an independent stock run agree with the cull, most groups skipped");
    if (!good) printf("    fail 0x%X, mismatches %llu, groups skipped %llu, scanned %llu, mode-1 comparisons %d\n", fail, static_cast<unsigned long long>(mm),
                      static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(scanned), compared);
    fail = Simulate(body, 120, 100, false, &mm, &skipped, &scanned, &compared);  // tasks of 100: not whole blocks
    Check((fail & ~8) == 0 && mm == 0 && skipped == 0 && scanned == 0, "LumenScanCull: tasks that are not whole blocks run stock (nothing skipped)");
    if ((fail & ~8) || skipped || scanned) printf("    fail 0x%X, skipped %llu, scanned %llu\n", fail, static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(scanned));
    fail = Simulate(body, 200, 256, true, &mm, &skipped, &scanned, &compared);
    Check(mm > 0, "LumenScanCull: proof catches an unlisted change of a skipped block (the negative test)");
    // Review lumenscanrev's cases.
    int c = CaseModeGap(body, true);
    Check((c & 16) != 0, "LumenScanCull T1: the first version's mode handling misses a change made while off (1 -> 0 -> 1 with a reset in between): reproduced");
    c = CaseModeGap(body, false);
    Check(c == 0, "LumenScanCull T1: with every mode change and every reset while off marking all dirty, the same sequence matches stock");
    if (c) printf("    failed checks (bits): 0x%X\n", c);
    c = CaseCopy(body);
    Check(c == 0, "LumenScanCull T2: groups replaced without listing at the same address (new storage) match stock; a view-specific copy runs stock");
    if (c) printf("    failed checks (bits): 0x%X\n", c);
    c = CaseShrinkRegrow(body);
    Check(c == 0, "LumenScanCull T3: the group count shrinking and growing back to the same value between scans (changes listed) matches stock");
    if (c) printf("    failed checks (bits): 0x%X\n", c);
    c = CaseViews(body);
    Check(c == 0, "LumenScanCull T4: three views (heap storage) are culled and match stock; nine views run stock");
    if (c) printf("    failed checks (bits): 0x%X\n", c);
    c = CaseEdges(body);
    Check(c == 0, "LumenScanCull T5: distance exactly MaxDist^2 and one float step past it, R at MinCardResolution, far-field passes: match stock");
    if (c) printf("    failed checks (bits): 0x%X\n", c);
    c = CaseWrongSkip(body);
    Check(c == 0, "LumenScanCull T7: a forged block summary (a wrong skip) differs from stock in mode 1, and mode 2 counts it and hands over the stock output");
    if (c) printf("    failed checks (bits): 0x%X\n", c);
    for (const int per : {64, 192}) {
        fail = Simulate(body, 120, per, false, &mm, &skipped, &scanned, &compared);
        Check(fail == 0 && mm == 0 && skipped > 0, per == 64 ? "LumenScanCull T6: tasks of 64 groups: proof and stock agree, skipping on"
                                                             : "LumenScanCull T6: tasks of 192 groups (three blocks): proof and stock agree, skipping on");
        if (fail || mm || !skipped) printf("    fail 0x%X, mismatches %llu, skipped %llu\n", fail, static_cast<unsigned long long>(mm), static_cast<unsigned long long>(skipped));
    }
}

}  // namespace lscan
