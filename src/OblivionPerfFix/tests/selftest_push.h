// PhysicsPushParallel self-test (included by selftest.cpp after Check): the sites on the mapped exe, then a model of the push loop.
// The model mimics the game's call sequence (ForEachProxy over 13 buckets -> ProcessProxyPT: create, UpdateViews, free,
// PushToPhysicsState: SetParticleTransform, SetNonFrequentData, world state, queue entry; RegisterParticle) on fake memory
// with the game's layout, and calls through PhysicsPushParallel's four hooks. Random pushes (new statics with and without transform /
// non-frequent data, existing and skipped proxies, new rigid proxies in between, other proxy types in other buckets) run
// once with the fix off and once on (batch only; batch + helpers, with random yields), each on a fresh copy of the same
// world: every particle must get the same index, handle index, particle id, unique index, transform, geometry and bounds,
// the serial event log (queue entries, registrations) must be the same sequence, and the SOA counters the same.
#include <map>
#include <memory>

namespace pp {
struct FakeHandle {
    uint8_t* soa = nullptr;  // the game's handle layout: SOA, index
    int32_t idx = -1, pad = 0;
    int index = -1, handleIdx = -1, id = -1, uidx = -1, rigid = 0;
    double wb = 0;
    int uwsCalls = 0;
    double x[3] = {}, r[4] = {};
    int geom = -1, uidx2 = -1;
    double bound = 0;
    std::atomic<int> snfdCalls{0};
};
// PhysicsPushGrowParallel: a TArrayCollectionArray<T> as the game lays it out (vtable, TArray data/size/capacity) with SetNum semantics.
struct FakeArray {
    void** vt;
    uint8_t* data = nullptr;
    int32_t num = 0, max = 0;
    int32_t elem = 8;
    uint8_t fill = 0;
    bool evil = false;  // a Resize that is not a no-op at the same size (the live check must catch it)
};
void FakeResize(uint8_t* self, int32_t n);
void* g_fakeArrayVt[2] = {nullptr, reinterpret_cast<void*>(&FakeResize)};
void FakeResize(uint8_t* self, int32_t n)
{
    FakeArray& a = *reinterpret_cast<FakeArray*>(self);
    if (a.evil && n == a.num) n += 1;
    if (n > a.num) {
        if (n > a.max) {
            const int32_t nm = n + 3 * n / 8 + 16;
            uint8_t* d = static_cast<uint8_t*>(malloc(static_cast<size_t>(nm) * static_cast<size_t>(a.elem)));
            if (a.data) memcpy(d, a.data, static_cast<size_t>(a.num) * static_cast<size_t>(a.elem));
            free(a.data);
            a.data = d;
            a.max = nm;
        }
        for (int32_t i = a.num; i < n; ++i)
            for (int32_t b = 0; b < a.elem; ++b)
                a.data[static_cast<size_t>(i) * static_cast<size_t>(a.elem) + static_cast<size_t>(b)] = static_cast<uint8_t>(a.fill + b);
    }
    a.num = n;
}
struct Coll { uint8_t mem[0x20] = {}; };

struct World {
    std::vector<uint8_t> solver = std::vector<uint8_t>(0x1000), manager = std::vector<uint8_t>(0x100), dirty = std::vector<uint8_t>(0x200);
    std::vector<uint8_t> xr, nfd;
    std::vector<std::vector<uint8_t>> entries;   // per bucket, 32 bytes each
    std::vector<std::unique_ptr<uint8_t[]>> proxies;
    std::vector<std::unique_ptr<FakeHandle>> handles;  // all particles ever created (and the existing ones)
    int size = 0, handleCount = 0, biggestId = 0, genIdx = 100000, viewSize = 0, rigidSize = 0, existing = 0;
    uint8_t staticSoa[16] = {}, rigidSoa[16] = {}, existingSoa[16] = {};
    std::map<std::pair<uint8_t*, int32_t>, FakeHandle*> bySlot;
    std::vector<std::string> log;                  // the serial event sequence
    void* capture[4] = {};
    void* pp[6] = {};
    int gc = 0;
    // PhysicsPushGrowParallel: the static SOA's array collection (size +0x18, arrays +0x08, count +0x10) and the handle arrays on the SOAs
    std::vector<std::unique_ptr<FakeArray>> arrays;
    std::vector<void*> soaArrays, handleArrays;
    Coll soaColl;
    uint8_t* soaRef = nullptr;
    ~World() { for (auto& a : arrays) free(a->data); }
};
World* g_w = nullptr;
std::atomic<bool> g_jit{false};
thread_local uint32_t t_rnd = 12345;
void Jit()
{
    if (!g_jit.load(std::memory_order_relaxed)) return;
    t_rnd = t_rnd * 1664525u + 1013904223u;
    if (t_rnd >> 29 == 0) Sleep(0); else if (t_rnd >> 29 == 1) SwitchToThread();
}
struct Arr { uint8_t* data; int32_t num; int32_t max; };

int32_t& I32(uint8_t* p, size_t off);
void ResizeLoop(uint8_t* coll, size_t sizeOff, size_t arraysOff, size_t numOff, int n)
{
    if (n == 0) return;
    int32_t& size = I32(coll, sizeOff);
    size += n;
    void** const arrays = *reinterpret_cast<void***>(coll + arraysOff);
    for (int32_t k = 0; k < I32(coll, numOff); ++k) {
        uint8_t* const a = static_cast<uint8_t*>(arrays[k]);
        if (a) reinterpret_cast<void (*)(uint8_t*, int32_t)>((*reinterpret_cast<void***>(a))[1])(a, size);
    }
}
Arr* FakeCreate(uint8_t* particles, Arr* out, int n, const int32_t* idx, void* soaRef, void*)
{
    World& w = *g_w;
    if (particles != w.solver.data() + 0x290) printf("      pp: wrong particles pointer\n");
    if (soaRef) {  // the game's helper: AddParticles(n) on the SOA, then the handle arrays
        ResizeLoop(*static_cast<uint8_t**>(soaRef), 0x18, 0x08, 0x10, n);
        ResizeLoop(particles, 0x670, 0x660, 0x668, n);
    }
    const int start = w.size;
    w.size += n;
    out->data = static_cast<uint8_t*>(malloc(static_cast<size_t>(n) * 8));
    out->num = out->max = n;
    for (int k = 0; k < n; ++k) {
        auto h = std::make_unique<FakeHandle>();
        h->index = start + k;
        h->soa = w.staticSoa;
        h->idx = h->index;
        w.bySlot[{h->soa, h->idx}] = h.get();
        h->handleIdx = w.handleCount++;
        h->id = w.biggestId++;
        h->uidx = idx ? idx[k] : w.genIdx++;
        reinterpret_cast<FakeHandle**>(out->data)[k] = h.get();
        w.handles.push_back(std::move(h));
    }
    return out;
}
void FakeUws(uint8_t* soa, int32_t idx, const void*, const void*)
{
    auto it = g_w->bySlot.find({soa, idx});
    if (it == g_w->bySlot.end()) { printf("      pp: world-space update of an unknown particle\n"); return; }
    FakeHandle* h = it->second;
    Jit();
    h->wb = h->bound * 3 + h->x[1] + h->geom;  // from the shapes (geometry) and the transform
    ++h->uwsCalls;
}
void FakeJobUws(void* handle) { FakeHandle* h = static_cast<FakeHandle*>(handle); FakeUws(h->soa, h->idx, nullptr, nullptr); }
void FakeViews(uint8_t*) { g_w->viewSize = g_w->size + g_w->rigidSize; }
void FakeFree(void* p) { free(p); }
void FakeGrow(Arr* a, int32_t) { a->data = static_cast<uint8_t*>(realloc(a->data, static_cast<size_t>(a->num) * 8)); a->max = a->num; }
void FakeSpt(void*, void* handle, const void* x, const void* r, uint8_t)
{
    FakeHandle* h = static_cast<FakeHandle*>(handle);
    Jit();
    memcpy(h->x, x, sizeof h->x);
    memcpy(h->r, r, sizeof h->r);
}
void FakeSnfd(void* handle, const void* nfd)
{
    FakeHandle* h = static_cast<FakeHandle*>(handle);
    const uint8_t* d = static_cast<const uint8_t*>(nfd);
    Jit();
    memcpy(&h->geom, d, 4);
    memcpy(&h->uidx2, d + 0x10, 4);
    h->bound = h->x[0] * 7 + h->x[2] + h->r[3] + h->geom;  // depends on the transform set before it
    Jit();
    h->snfdCalls.fetch_add(1);
}
PhysicsPushTestHooks g_hooks;

int32_t& I32(uint8_t* p, size_t off) { return *reinterpret_cast<int32_t*>(p + off); }

// The game's loop, as far as PhysicsPushParallel is concerned.
void StockForEach(uint8_t* dirtySet, void** capture)
{
    World& w = *g_w;
    uint8_t* const solver = static_cast<uint8_t*>(capture[0]);
    const int frame = I32(solver, 0x248);
    uint8_t* const particles = solver + 0x290;
    for (int b = 0; b < 13; ++b) {
        uint8_t* const base = *reinterpret_cast<uint8_t**>(dirtySet + 0x38 + b * 16);
        const int n = *reinterpret_cast<int32_t*>(dirtySet + 0x38 + b * 16 + 8);
        for (int i = 0; i < n; ++i) {
            uint8_t* const e = base + i * 32;
            uint8_t* const proxy = *reinterpret_cast<uint8_t**>(e);
            if (I32(proxy, 0x40) == frame) continue;
            const uint32_t flags = *reinterpret_cast<uint32_t*>(e + 8);
            if (I32(proxy, 0x38) != 0x0B) { ++w.gc; w.log.push_back("other " + std::to_string(b) + ":" + std::to_string(i)); continue; }
            const bool isNew = I32(proxy, 0x3C) == -1;
            FakeHandle*& handle = *reinterpret_cast<FakeHandle**>(proxy + 0x50);
            const int32_t* uidx = (flags & 0x10) ? reinterpret_cast<const int32_t*>(w.nfd.data() + i * 0x28 + 0x10) : nullptr;
            if (isNew) {
                if (e[0x0C] == 0) {
                    Arr out{};
                    reinterpret_cast<Arr* (*)(uint8_t*, Arr*, int, const int32_t*, void*, void*)>(g_hooks.create)(particles, &out, 1, uidx,
                                                                                                                &w.soaRef, nullptr);
                    reinterpret_cast<void (*)(uint8_t*)>(g_hooks.views)(particles);
                    handle = *reinterpret_cast<FakeHandle**>(out.data);
                    FakeFree(out.data);
                } else {  // a new rigid: its own SOA, the shared handle and id counters, then UpdateViews
                    auto h = std::make_unique<FakeHandle>();
                    h->rigid = 1;
                    h->index = w.rigidSize++;
                    h->soa = w.rigidSoa;
                    h->idx = h->index;
                    w.bySlot[{h->soa, h->idx}] = h.get();
                    h->handleIdx = w.handleCount++;
                    h->id = w.biggestId++;
                    h->uidx = uidx ? *uidx : w.genIdx++;
                    handle = h.get();
                    w.handles.push_back(std::move(h));
                    reinterpret_cast<void (*)(uint8_t*)>(g_hooks.views)(particles);
                }
                w.log.push_back("gt " + std::to_string(handle->index) + " p" + std::to_string(i));
            }
            if (handle) {
                if (e[0x0C] == 0) {
                    if (flags & 1) FakeSpt(nullptr, handle, w.xr.data() + i * 0x40, w.xr.data() + i * 0x40 + 0x20, 1);
                    if (flags & 0x10) reinterpret_cast<void (*)(void*, const void*)>(g_hooks.snfd)(handle, w.nfd.data() + i * 0x28);
                    if (flags & 0x11)
                        reinterpret_cast<void (*)(uint8_t*, int32_t, const void*, const void*)>(g_hooks.uws)(handle->soa, handle->idx, nullptr,
                                                                                                          nullptr);
                    char buf[160];
                    snprintf(buf, sizeof buf, "queue %d %.3f %d %.3f", handle->uidx, handle->x[0], handle->geom, handle->bound);
                    w.log.push_back(buf);
                } else {
                    w.log.push_back("rigid push " + std::to_string(handle->uidx));
                }
            }
            if (isNew) {
                w.log.push_back("register " + std::to_string(handle->id));
                I32(proxy, 0x3C) = frame;
            }
        }
    }
}

// One random push; `seed` decides everything, so two worlds built from one seed are the same.
void Build(World& w, uint32_t seed, int maxEntries, bool twoBuckets)
{
    uint32_t r = seed;
    auto next = [&] { r = r * 1664525u + 1013904223u; return r >> 8; };
    I32(w.solver.data(), 0x248) = 7;
    *reinterpret_cast<void**>(w.solver.data() + 0x930) = w.solver.data() + 0x800;  // the evolution (opaque)
    const int n = static_cast<int>(next() % static_cast<uint32_t>(maxEntries)) + 1;
    w.xr.assign(static_cast<size_t>(n) * 0x40, 0);
    w.nfd.assign(static_cast<size_t>(n) * 0x28, 0);
    *reinterpret_cast<uint8_t**>(w.manager.data()) = w.xr.data();
    *reinterpret_cast<uint8_t**>(w.manager.data() + 0x40) = w.nfd.data();
    w.entries.assign(13, {});
    for (int i = 0; i < n; ++i) {
        double* x = reinterpret_cast<double*>(w.xr.data() + i * 0x40);
        for (int k = 0; k < 7; ++k) x[k] = static_cast<double>(next() % 10000) / 7.0;
        I32(w.nfd.data(), static_cast<size_t>(i) * 0x28) = static_cast<int32_t>(next() % 50);   // "geometry"
        I32(w.nfd.data(), static_cast<size_t>(i) * 0x28 + 0x10) = 1000 + i * 3;               // unique index
    }
    auto addEntry = [&](int bucket, uint32_t flags, uint8_t type, int typeOfProxy, int init, int ignore) {
        auto proxy = std::make_unique<uint8_t[]>(0x60);
        memset(proxy.get(), 0, 0x60);
        I32(proxy.get(), 0x38) = typeOfProxy;
        I32(proxy.get(), 0x3C) = init;
        I32(proxy.get(), 0x40) = ignore;
        if (init != -1) {  // an existing particle
            auto h = std::make_unique<FakeHandle>();
            h->index = -2;
            h->soa = w.existingSoa;
            h->idx = w.existing++;
            w.bySlot[{h->soa, h->idx}] = h.get();
            h->uidx = static_cast<int>(next() % 1000);
            *reinterpret_cast<FakeHandle**>(proxy.get() + 0x50) = h.get();
            w.handles.push_back(std::move(h));
        }
        std::vector<uint8_t> e(32, 0);
        *reinterpret_cast<uint8_t**>(e.data()) = proxy.get();
        memcpy(e.data() + 8, &flags, 4);
        e[0x0C] = type;
        w.entries[static_cast<size_t>(bucket)].insert(w.entries[static_cast<size_t>(bucket)].end(), e.begin(), e.end());
        w.proxies.push_back(std::move(proxy));
    };
    const int others = static_cast<int>(next() % 4);
    for (int k = 0; k < others; ++k) addEntry(2, 0, 0, 2, -1, 0);
    for (int i = 0; i < n; ++i) {
        const uint32_t pick = next() % 100;
        const int bucket = twoBuckets && i > n / 2 ? 12 : 11;
        if (pick < 70) addEntry(bucket, 0x11 | (next() % 2 ? 0x40 : 0), 0, 0x0B, -1, 0);   // new static (the common case)
        else if (pick < 74) addEntry(bucket, 0x10, 0, 0x0B, -1, 0);                    // new static, no transform
        else if (pick < 77) addEntry(bucket, 0x01, 0, 0x0B, -1, 0);                    // new static, no non-frequent data
        else if (pick < 85) addEntry(bucket, 0x01, 0, 0x0B, 3, 0);                     // existing static
        else if (pick < 90) addEntry(bucket, 0x11, 0, 0x0B, -1, 7);                    // skipped this step
        else if (pick < 95) addEntry(bucket, 0x11, 2, 0x0B, -1, 0);                    // new rigid
        else addEntry(bucket, 0x11, 0, 0x0B, -1, 0);
    }
    for (int k = 0; k < 2; ++k) addEntry(12 - (twoBuckets ? 9 : 0), 0, 0, 3, -1, 0);
    for (int b = 0; b < 13; ++b) {
        *reinterpret_cast<uint8_t**>(w.dirty.data() + 0x38 + b * 16) = w.entries[static_cast<size_t>(b)].data();
        I32(w.dirty.data(), 0x38 + static_cast<size_t>(b) * 16 + 8) = static_cast<int32_t>(w.entries[static_cast<size_t>(b)].size() / 32);
    }
    // PhysicsPushGrowParallel: 14 SOA arrays of assorted element sizes and spare capacities (and one null entry), one handle array
    const int s0 = static_cast<int>(next() % 3000);
    auto makeArray = [&](int elem, int spare) {
        auto a = std::make_unique<FakeArray>();
        a->vt = g_fakeArrayVt;
        a->elem = elem;
        a->fill = static_cast<uint8_t>(next());
        FakeResize(reinterpret_cast<uint8_t*>(a.get()), s0);
        if (spare > 0) {  // more capacity than the growth policy gives
            uint8_t* d = static_cast<uint8_t*>(malloc(static_cast<size_t>(a->max + spare) * static_cast<size_t>(elem)));
            if (a->data) memcpy(d, a->data, static_cast<size_t>(a->num) * static_cast<size_t>(elem));
            free(a->data);
            a->data = d;
            a->max += spare;
        }
        void* p = a.get();
        w.arrays.push_back(std::move(a));
        return p;
    };
    const int elems[] = {1, 4, 8, 12, 16, 24, 32, 48, 64, 8, 4, 96, 24, 8};
    for (int k = 0; k < 14; ++k) {
        w.soaArrays.push_back(makeArray(elems[k], next() % 3 == 0 ? static_cast<int>(next() % 3000) : 0));
        if (k == 6) w.soaArrays.push_back(nullptr);
    }
    w.handleArrays.push_back(makeArray(8, 0));
    *reinterpret_cast<void***>(w.soaColl.mem + 0x08) = w.soaArrays.data();
    I32(w.soaColl.mem, 0x10) = static_cast<int32_t>(w.soaArrays.size());
    I32(w.soaColl.mem, 0x18) = s0;
    w.soaRef = w.soaColl.mem;
    uint8_t* const parts = w.solver.data() + 0x290;
    *reinterpret_cast<void***>(parts + 0x660) = w.handleArrays.data();
    I32(parts, 0x668) = 1;
    I32(parts, 0x670) = s0;
    w.pp[0] = w.manager.data();
    w.pp[5] = w.solver.data();
    w.capture[0] = w.solver.data();
    w.capture[1] = w.pp;
    w.capture[2] = w.manager.data();
    w.capture[3] = &w.gc;
}

std::vector<std::string> Outcome(World& w)
{
    std::vector<std::string> out = w.log;
    char buf[256];
    for (auto& p : w.proxies) {
        FakeHandle* h = *reinterpret_cast<FakeHandle**>(p.get() + 0x50);
        if (!h) { out.push_back("none"); continue; }
        snprintf(buf, sizeof buf, "h %d %d %d %d %d %.4f %.4f %.4f %d %d %.4f snfd %d wb %.4f uws %d", h->rigid, h->index, h->handleIdx,
                 h->id, h->uidx, h->x[0], h->x[1], h->r[3], h->geom, h->uidx2, h->bound, h->snfdCalls.load(), h->wb, h->uwsCalls);
        out.push_back(buf);
    }
    snprintf(buf, sizeof buf, "counters %d %d %d %d %d %d", w.size, w.handleCount, w.biggestId, w.genIdx, w.viewSize, w.rigidSize);
    out.push_back(buf);
    uint8_t* const parts = w.solver.data() + 0x290;
    snprintf(buf, sizeof buf, "collections %d %d", I32(w.soaColl.mem, 0x18), I32(parts, 0x670));
    out.push_back(buf);
    for (auto& a : w.arrays) {  // size and contents (the capacity may differ: it is not deterministic in stock either)
        uint64_t hsh = 1469598103934665603ull;
        for (size_t b = 0; b < static_cast<size_t>(a->num) * static_cast<size_t>(a->elem); ++b) hsh = (hsh ^ a->data[b]) * 1099511628211ull;
        snprintf(buf, sizeof buf, "array %d %d %llx %d", a->elem, a->num, static_cast<unsigned long long>(hsh), a->max >= a->num ? 1 : 0);
        out.push_back(buf);
    }
    return out;
}

std::vector<std::string> RunOnce(uint32_t seed, int maxEntries, bool twoBuckets, int mode, bool jitter, int grow = 0, bool evil = false)
{
    World w;
    g_w = &w;
    Build(w, seed, maxEntries, twoBuckets);
    if (evil) w.arrays[3]->evil = true;
    PhysicsPushTestMode(mode, jitter
                        // The stock helper's growth still runs in this model.
    );
    (void)grow;
    g_jit = jitter;
    reinterpret_cast<void (*)(uint8_t*, void**)>(g_hooks.forEach)(w.dirty.data(), w.capture);
    g_jit = false;
    g_w = nullptr;
    return Outcome(w);
}
}  // namespace pp

static void PushSelfTest(const GameModule& game)
{
    // The sites on the mapped exe (build 19115871).
    if (Installed(FindPhysicsPush(), "PhysicsPushParallel: ForEachProxy's call, ProcessProxyPT's creation, the static push found")) {
        PhysicsPushInfoOut pi{};
        PhysicsPushInfo(&pi);
        auto rva = [&](const void* p) { return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - game.base); };
        Check(rva(pi.pushCall) == 0x1A2E7EF && rva(pi.forEach) == 0x1A179B0, "PhysicsPushParallel: call exe+0x1A2E7EF -> ForEachProxy exe+0x1A179B0");
        Check(rva(pi.createCall) == 0x1A15AD8 && rva(pi.create) == 0x1A16B70 && rva(pi.viewsCall) == 0x1A15AE0 &&
                  rva(pi.views) == 0x178D500 && rva(pi.free) == 0xE26500 && rva(pi.grow) == 0xD8B620,
              "PhysicsPushParallel: creation exe+0x1A15AD8 -> exe+0x1A16B70, UpdateViews exe+0x1A15AE0 -> exe+0x178D500, free, ResizeGrow");
        Check(rva(pi.snfdCall) == 0x1A63120 && rva(pi.snfd) == 0x18E0930 && rva(pi.spt) == 0x193F0B0,
              "PhysicsPushParallel: static push's SetNonFrequentData exe+0x1A63120 -> exe+0x18E0930, SetParticleTransform exe+0x193F0B0");
        Check(rva(pi.uwsCall) == 0x1A631AA && rva(pi.uws) == 0x18951A0,
              "PhysicsPushParallel: static push's UpdateWorldSpaceState exe+0x1A631AA -> exe+0x18951A0");
        const uint32_t want[8] = {0x38, 13, 0x248, 0x40, 0x38, 0x290, 0x1B0, 0x930};
        Check(memcmp(pi.offsets, want, sizeof want) == 0, "PhysicsPushParallel: buckets +0x38 x13, frame +0x248, proxy +0x40/+0x38, particles "
                                                          "+0x290, rewind +0x1B0, evolution +0x930");
    }
    // The model: off vs batch vs batch + helpers on the same random pushes.
    PhysicsPushTestFns fns{reinterpret_cast<void*>(&pp::StockForEach), reinterpret_cast<void*>(&pp::FakeCreate),
                           reinterpret_cast<void*>(&pp::FakeViews), reinterpret_cast<void*>(&pp::FakeSnfd),
                           reinterpret_cast<void*>(&pp::FakeSpt), reinterpret_cast<void*>(&pp::FakeGrow),
                           reinterpret_cast<void*>(&pp::FakeFree), reinterpret_cast<void*>(&pp::FakeUws),
                           reinterpret_cast<void*>(&pp::FakeJobUws)};
    pp::g_hooks = PhysicsPushTestSetup(fns, 3, 8);
    int same = 0, total = 0;
    for (uint32_t seed = 1; seed <= 400; ++seed) {
        const int maxEntries = seed % 10 == 0 ? 3000 : seed % 3 == 0 ? 40 : 400;
        const bool two = seed % 37 == 0;
        const std::vector<std::string> off = pp::RunOnce(seed, maxEntries, two, 0, false);
        const int variants[2][2] = {{2, 0}, {1, 0}};
        for (const auto& v : variants) {
            const int mode = v[0], grow = v[1];
            ++total;
            const std::vector<std::string> on = pp::RunOnce(seed, maxEntries, two, mode, mode == 1, grow);
            if (on == off) ++same;
            else if (total - same <= 3) {
                for (size_t k = 0; k < on.size() && k < off.size(); ++k)
                    if (on[k] != off[k]) { printf("      seed %u mode %d: line %zu: off '%s' on '%s'\n", seed, mode, k, off[k].c_str(), on[k].c_str()); break; }
                if (on.size() != off.size()) printf("      seed %u mode %d: %zu vs %zu lines\n", seed, mode, off.size(), on.size());
            }
        }
    }
    char what[160];
    snprintf(what, sizeof what, "PhysicsPushParallel: 400 random pushes, batch and batch + helpers (random yields), give the stock outcome (%d/%d)",
             same, total);
    Check(same == total, what);
    Check(PhysicsPushTestAnomalies() == 0, "PhysicsPushParallel: no unexpected call order seen");
    {
        // Late helpers: they sleep 300 ms after each wake. A push must not wait for them (the push thread sets up every
        // particle itself) and must give the stock outcome.
        const std::vector<std::string> base = pp::RunOnce(10, 3000, false, 0, false);
        PhysicsPushTestHold(300);
        const ULONGLONG t0 = GetTickCount64();
        const std::vector<std::string> held = pp::RunOnce(10, 3000, false, 1, false);
        const ULONGLONG ms = GetTickCount64() - t0;
        Sleep(400);
        PhysicsPushTestHold(0);
        Sleep(50);
        const std::vector<std::string> after = pp::RunOnce(11, 3000, false, 1, false);
        printf("      (late helpers: the push took %llu ms)\n", static_cast<unsigned long long>(ms));
        Check(held == base && ms < 150 && after == pp::RunOnce(11, 3000, false, 0, false) && PhysicsPushTestAnomalies() == 0,
              "PhysicsPushParallel: helpers that wake 300 ms late: the push ends without them with the stock outcome; the next push too");
    }
    PhysicsPushTestMode(0, false);
}
