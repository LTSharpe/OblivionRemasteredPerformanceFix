// PipelinedBodyCreation self-test (included by selftest.cpp after PipelinedSceneAdd's fakes, namespace pst): the sites on the mapped exe, then the
// game's InitStaticBodies flow on fakes, run once the stock way (PipelinedBodyCreation off) and once pipelined, for random components.
// The flow: the creation call (ParallelStaticBodies's hook) and, when it returns true, ExecuteWrite (the scene's write lock) over the
// InitBodies lambda. The fake lambda does the game's per-body steps: pre-add (the body must exist and be unregistered),
// AddActorsToScene through PipelinedSceneAdd's hook (pst's fakes: registration on the calling thread, inserts in order), maps
// (registered by then), and the flagged max-depenetration step (that body's insert must be done by then). Every body's
// creation count, pre-add, registration, insert, maps and post positions must equal the stock run's, with every
// registration and insert inside the write lock, and nothing running after InitStaticBodies returned.
namespace pbc {
struct Arr { uint8_t* data; int32_t num; int32_t max; };
constexpr int kMax = pst::kMax;
constexpr uint32_t kHandleOff = 0x158, kPostOff = 0x5D;
constexpr uint8_t kPostBit = 0x40;
alignas(16) uint8_t g_bodies[kMax][0x200];
uint8_t* g_bodyPtrs[kMax];
alignas(16) uint8_t g_transforms[kMax][0x60];
std::atomic<int> g_made[kMax];
int g_pre[kMax], g_map[kMax], g_post[kMax];
int g_preSeq = 0, g_mapSeq = 0, g_postSeq = 0;
std::atomic<int> g_bad{0}, g_inExec{0}, g_after{0}, g_lambdaCalls{0};
std::atomic<bool> g_returned{false};
int g_work = 200;

int IndexOf(const uint8_t* body) { return static_cast<int>((body - &g_bodies[0][0]) / 0x200); }

bool FakeCreate(void* helper)
{
    auto* h = static_cast<uint8_t*>(helper);
    Arr* b = reinterpret_cast<Arr**>(h)[0];
    Arr* t = reinterpret_cast<Arr**>(h)[1];
    if (b->num != t->num || h[0x31] == 0) g_bad.fetch_add(1);
    for (int i = b->num - 1; i >= 0; --i) {
        uint8_t* body = reinterpret_cast<uint8_t**>(b->data)[i];
        const int idx = IndexOf(body);
        int64_t tIdx;
        memcpy(&tIdx, t->data + static_cast<size_t>(i) * 0x60, 8);
        if (tIdx != idx || *reinterpret_cast<void**>(body + kHandleOff)) g_bad.fetch_add(1);
        for (volatile int k = 0; k < g_work; ++k) {}
        *reinterpret_cast<void**>(body + kHandleOff) = &pst::g_proxies[idx];
        g_made[idx].fetch_add(1);
        if (g_returned.load()) g_after.fetch_add(1);
    }
    return true;
}

using AddHookFn = void (*)(uint8_t*, pst::Arr*, uint8_t);
void FakeLambda(void* capture)
{
    void** cap = static_cast<void**>(capture);
    uint8_t* const h = static_cast<uint8_t*>(cap[1]);
    g_lambdaCalls.fetch_add(1);
    if (!g_inExec.load() || *static_cast<uint8_t*>(cap[0]) == 0) g_bad.fetch_add(1);
    Arr* b = reinterpret_cast<Arr**>(h)[0];
    std::vector<uint8_t*> handles;
    for (int i = 0; i < b->num; ++i) {
        uint8_t* body = reinterpret_cast<uint8_t**>(b->data)[i];
        const int idx = IndexOf(body);
        auto* p = *reinterpret_cast<pst::Proxy**>(body + kHandleOff);
        if (!p || g_made[idx].load() != 1 || p->reg.load() >= 0) g_bad.fetch_add(1);
        g_pre[idx] = g_preSeq++;
        if (p) handles.push_back(reinterpret_cast<uint8_t*>(p));
    }
    pst::Arr a{reinterpret_cast<uint8_t*>(handles.data()), static_cast<int32_t>(handles.size()), static_cast<int32_t>(handles.size())};
    reinterpret_cast<AddHookFn>(opf::PipelinedSceneAddTestHook())(*reinterpret_cast<uint8_t**>(h + 0x28), &a, 1);
    for (int i = 0; i < b->num; ++i) {
        uint8_t* body = reinterpret_cast<uint8_t**>(b->data)[i];
        auto* p = *reinterpret_cast<pst::Proxy**>(body + kHandleOff);
        if (!p || p->reg.load() < 0) g_bad.fetch_add(1);
        g_map[IndexOf(body)] = g_mapSeq++;
    }
    for (int i = 0; i < b->num; ++i) {
        uint8_t* body = reinterpret_cast<uint8_t**>(b->data)[i];
        if (!(body[kPostOff] & kPostBit)) continue;
        auto* p = *reinterpret_cast<pst::Proxy**>(body + kHandleOff);
        if (!p || p->ins < 0) g_bad.fetch_add(1);  // the post step reads/writes a particle whose insert must be done
        g_post[IndexOf(body)] = g_postSeq++;
    }
}

struct FRef { void (*call)(void*); void* ctx; };
uint8_t FakeExecWrite(uint8_t* scene, FRef* fr)
{
    if (scene != pst::g_scene) g_bad.fetch_add(1);
    pst::Lock();
    g_inExec.store(1);
    fr->call(fr->ctx);
    g_inExec.store(0);
    pst::Unlock();
    return 1;
}

alignas(16) uint8_t g_helper[0x80];
Arr g_b, g_t;
struct Result { std::vector<int> made, pre, reg, ins, map, post; };

// The game's InitStaticBodies on n bodies (flags from the seed), through ParallelStaticBodies's hook.
Result InitStatic(bool (*hook)(void*), int n, uint32_t seed, int flagPerMille)
{
    for (int i = 0; i < n; ++i) {
        memset(g_bodies[i], 0, 0x200);
        g_bodyPtrs[i] = g_bodies[i];
        int64_t v = i;
        memcpy(g_transforms[i], &v, 8);
        g_made[i].store(0);
        g_pre[i] = g_map[i] = g_post[i] = -1;
        pst::g_proxies[i].reg.store(-1); pst::g_proxies[i].ins = -1; pst::g_proxies[i].regTid = pst::g_proxies[i].insTid = 0;
        seed = seed * 1664525u + 1013904223u;
        if (static_cast<int>((seed >> 8) % 1000) < flagPerMille) g_bodies[i][kPostOff] |= kPostBit;
    }
    pst::g_reg = 0; pst::g_ins = 0; pst::g_bad = 0;
    g_preSeq = g_mapSeq = g_postSeq = 0;
    g_b = Arr{reinterpret_cast<uint8_t*>(g_bodyPtrs), n, n};
    g_t = Arr{&g_transforms[0][0], n, n};
    memset(g_helper, 0, sizeof g_helper);
    reinterpret_cast<Arr**>(g_helper)[0] = &g_b;
    reinterpret_cast<Arr**>(g_helper)[1] = &g_t;
    *reinterpret_cast<uint8_t**>(g_helper + 0x28) = pst::g_scene;
    g_helper[0x31] = 1;
    g_returned = false;
    if (hook(g_helper)) {
        uint8_t made = 1;
        void* cap[2] = {&made, g_helper};
        FRef fr{&FakeLambda, cap};
        FakeExecWrite(pst::g_scene, &fr);
    }
    g_returned = true;
    if (pst::g_inJob.load() != 0) g_bad.fetch_add(1);
    if (pst::g_bad.load() != 0) g_bad.fetch_add(1);
    Result r;
    for (int i = 0; i < n; ++i) {
        r.made.push_back(g_made[i].load());
        r.pre.push_back(g_pre[i]);
        r.reg.push_back(pst::g_proxies[i].reg.load());
        r.ins.push_back(pst::g_proxies[i].ins);
        r.map.push_back(g_map[i]);
        r.post.push_back(g_post[i]);
    }
    return r;
}
bool Same(const Result& a, const Result& b)
{
    return a.made == b.made && a.pre == b.pre && a.reg == b.reg && a.ins == b.ins && a.map == b.map && a.post == b.post;
}
bool Identity(const Result& a)
{
    for (size_t i = 0; i < a.reg.size(); ++i)
        if (a.made[i] != 1 || a.pre[i] != static_cast<int>(i) || a.reg[i] != static_cast<int>(i) || a.ins[i] != static_cast<int>(i) ||
            a.map[i] != static_cast<int>(i))
            return false;
    return true;
}
}  // namespace pbc

static void PipelinedBodiesSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    if (!Installed(FindPipelinedBodyCreation(), "pipelined creation: ExecuteWrite and InitBodies' lambda found")) return;
    uint8_t* sites[2] = {};
    uint32_t offs[2] = {};
    PipelinedBodyCreationInfo(sites, offs);
    Check(sites[0] - game.base == 0x35303B0 && sites[1] - game.base == 0x34D0060 && offs[0] == 0x5D && offs[1] == 0x40,
          "pipelined creation: ExecuteWrite exe+0x35303B0, lambda exe+0x34D0060, body flag +0x5D & 0x40");
    StaticBodiesInfoOut si{};
    if (!ParallelStaticBodiesInfo(&si)) return;
    const auto hook = reinterpret_cast<bool (*)(void*)>(si.hook);
    ParallelStaticBodiesTestSetup(reinterpret_cast<void*>(&pbc::FakeCreate), pbc::kHandleOff, 0x31, 3, 64);
    PipelinedBodyCreationTestSetup(reinterpret_cast<void*>(&pbc::FakeLambda), reinterpret_cast<void*>(&pbc::FakeExecWrite),
                                   pbc::kPostOff, pbc::kPostBit);
    SetParallelStaticBodies(true, 64);
    // Stock reference (PipelinedBodyCreation off, ParallelStaticBodies off: the game's own creation call) vs PipelinedBodyCreation with and without 28b's helper.
    auto run = [&](bool p34, bool p28b, bool p28, int n, uint32_t seed, int flags) {
        SetParallelStaticBodies(p28, 64);
        SetPipelinedSceneAdd(p28b, 64);
        SetPipelinedBodyCreation(p34);
        return pbc::InitStatic(hook, n, seed, flags);
    };
    pbc::g_bad = 0;
    const pbc::Result stock = run(false, false, false, 1000, 7, 20);
    Check(pbc::Identity(stock) && pbc::g_bad.load() == 0, "pipelined creation: the stock flow on the fakes (reference) in array order");
    pbc::g_lambdaCalls = 0;
    const pbc::Result s34 = run(true, true, true, 1000, 7, 20);
    Check(pbc::Same(stock, s34) && pbc::g_bad.load() == 0 && pbc::g_after.load() == 0 && pbc::g_lambdaCalls.load() == 16,
          "pipelined creation: 1000 bodies in 16 lambda calls: made once, pre-added, registered, inserted, filed as the stock flow");
    const pbc::Result n34 = run(true, false, true, 1000, 7, 20);
    Check(pbc::Same(stock, n34) && pbc::g_bad.load() == 0, "pipelined creation without 28b's helper: as the stock flow");
    pbc::g_lambdaCalls = 0;
    run(true, true, true, 40, 7, 0);
    Check(pbc::g_lambdaCalls.load() == 1 && pbc::g_bad.load() == 0, "pipelined creation: fewer than 64 bodies: the game's flow");
    pst::g_flag = 0;
    pbc::g_lambdaCalls = 0;
    run(true, true, true, 500, 7, 0);
    Check(pbc::g_lambdaCalls.load() == 1 && pbc::g_bad.load() == 0, "pipelined creation: the game without the scene lock: ParallelStaticBodies's flow");
    pst::g_flag = 1;
    // Random components, helpers yielding at random points, random post flags (some chunks wait for their inserts).
    ParallelStaticBodiesTestJitter(true);
    pbc::g_work = 20;
    uint32_t rnd = 4242;
    int bad = 0;
    for (int j = 0; j < 300; ++j) {
        rnd = rnd * 1664525u + 1013904223u;
        const int n = 64 + static_cast<int>((rnd >> 4) % 937);
        const int flags = (rnd >> 20) % 3 == 0 ? 0 : static_cast<int>((rnd >> 22) % 30);
        const bool with28b = (rnd >> 28) % 4 != 0;
        pbc::g_bad = 0;
        const pbc::Result a = run(false, false, false, n, rnd, flags);
        const pbc::Result b = run(true, with28b, true, n, rnd, flags);
        if (!pbc::Same(a, b) || !pbc::Identity(b) || pbc::g_bad.load() != 0 || pbc::g_after.load() != 0) ++bad;
    }
    ParallelStaticBodiesTestJitter(false);
    pbc::g_work = 200;
    printf("      (stress: 300 random components vs the stock flow, %d different)\n", bad);
    Check(bad == 0, "pipelined creation: 300 random components: the same per-body results as the stock flow");
    SetPipelinedBodyCreation(false);
    SetPipelinedSceneAdd(false, 64);
    SetParallelStaticBodies(false, 64);
}
