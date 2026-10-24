// BulkIndexInsert self-test (included by selftest.cpp after Check): a model of UE 5.3's dynamic AABB tree (AABBTree.h: InsertLeaf,
// AllocateLeafNodeAndLeaf, AllocateInternalNode, UpdateAncestorBounds, RotateNode, RemoveLeafNode, and TAABBTree::
// UpdateElement's dirty-tree part) on the game's node layout, driven through DirtyTreeQuickInsert's two hooks with BulkIndexInsert on and off.
// Two trees get the same random flushes (adds, in-place updates, moves, removes; each element at most once per flush,
// as in the game's pending-data queues); after every flush both must hold exactly the same elements with the same bounds,
// every element in the leaf node its payload index names, consistent parents and bounds, and no lost or doubled nodes.
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

namespace bit {
constexpr double kPad = 5.0;  // p.aabbtree.DynamicTreeBoundingBoxPadding
constexpr int32_t kCap = 8;   // p.aabbtree.DynamicTreeLeafCapacity
struct Elem { int id; double b[6]; };
// The elements a tree holds, sorted by id (a flat list: Valid runs after every flush, on trees of 100k elements).
using ElemList = std::vector<Elem>;
bool SameElems(const ElemList& a, const ElemList& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id) return false;
        for (int k = 0; k < 6; ++k)
            if (a[i].b[k] != b[i].b[k]) return false;
    }
    return true;
}

struct Model {
    alignas(16) uint8_t tree[0x100];  // first member: the tree pointer is the model
    std::vector<uint8_t> nodes;       // 0x70 each
    std::vector<uint8_t> leafRaw;     // 0x48 each, element count at +0x38
    std::vector<std::vector<Elem>> leafElems;
    int32_t freeInternal = -1, freeLeaf = -1;
    std::unordered_map<int, int32_t> dirtyIdx;  // payload -> leaf node (DirtyPayloadIdx)
    Model() { memset(tree, 0, sizeof tree); Root() = -1; Sync(); }
    int32_t& Root() { return *reinterpret_cast<int32_t*>(tree + 0x54); }
    void Sync()
    {
        nodes.reserve(16);
        leafRaw.reserve(16 * 0x48);
        *reinterpret_cast<uint8_t**>(tree + 0x18) = nodes.data();
        *reinterpret_cast<int32_t*>(tree + 0x20) = static_cast<int32_t>(nodes.size() / 0x70);
        *reinterpret_cast<uint8_t**>(tree + 0x28) = leafRaw.data();
    }
    uint8_t* N(int32_t i) { return nodes.data() + static_cast<size_t>(i) * 0x70; }
    int32_t& Kid(int32_t i, int c) { return *reinterpret_cast<int32_t*>(N(i) + 0x60 + 4 * c); }
    int32_t& Par(int32_t i) { return *reinterpret_cast<int32_t*>(N(i) + 0x68); }
    bool Leaf(int32_t i) { return (N(i)[0x6C] & 1) != 0; }
    void GetBox(int32_t i, int c, double* b) { memcpy(b, N(i) + 0x30 * c, 48); }
    void PutBox(int32_t i, int c, const double* b) { memcpy(N(i) + 0x30 * c, b, 48); }
    static void Grow(double* a, const double* b)
    {
        for (int k = 0; k < 3; ++k) { a[k] = std::min(a[k], b[k]); a[3 + k] = std::max(a[3 + k], b[3 + k]); }
    }
    static double Area(const double* b)
    {
        const double dx = b[3] - b[0], dy = b[4] - b[1], dz = b[5] - b[2];
        return 2 * (dx * dy + dx * dz + dy * dz);
    }
    void NodeBox(int32_t i, double* b)
    {
        GetBox(i, 0, b);
        if (!Leaf(i)) { double c[6]; GetBox(i, 1, c); Grow(b, c); }
    }
    void SetCount(int32_t leaf) { *reinterpret_cast<int32_t*>(leafRaw.data() + static_cast<size_t>(leaf) * 0x48 + 0x38) = static_cast<int32_t>(leafElems[leaf].size()); }
    void LeafBoxPadded(int32_t leaf, double* b)
    {
        const auto& e = leafElems[leaf];
        memcpy(b, e[0].b, 48);
        for (size_t k = 1; k < e.size(); ++k) Grow(b, e[k].b);
        for (int k = 0; k < 3; ++k) { b[k] -= kPad; b[3 + k] += kPad; }
    }
    int32_t AppendNode()
    {
        nodes.resize(nodes.size() + 0x70);
        Sync();
        return static_cast<int32_t>(nodes.size() / 0x70) - 1;
    }
    void AllocLeaf(const Elem& e, int32_t& node, int32_t& leaf)
    {
        if (freeLeaf != -1) {
            node = freeLeaf;
            freeLeaf = Kid(node, 1);
            leaf = Kid(node, 0);
            leafElems[leaf].push_back(e);
        } else {
            leaf = static_cast<int32_t>(leafElems.size());
            node = AppendNode();
            Kid(node, 0) = leaf;
            N(node)[0x6C] = 1;
            leafElems.push_back({e});
            leafRaw.resize(leafRaw.size() + 0x48);
            Sync();
        }
        SetCount(leaf);
        double b[6];
        memcpy(b, e.b, 48);
        for (int k = 0; k < 3; ++k) { b[k] -= kPad; b[3 + k] += kPad; }
        PutBox(node, 0, b);
        Par(node) = -1;
    }
    int32_t AllocInternal()
    {
        if (freeInternal != -1) {
            const int32_t n = freeInternal;
            freeInternal = Kid(n, 1);
            return n;
        }
        const int32_t n = AppendNode();
        N(n)[0x6C] = 0;
        return n;
    }
    int Which(int32_t n) { return Kid(Par(n), 0) == n ? 0 : 1; }
    void Rotate(int32_t n)
    {
        int32_t bestG = -1, bestA = -1;
        double bestD = 0;
        for (int al = 0; al < 2; ++al) {
            const int32_t aunt = Kid(n, al), mother = Kid(n, al ^ 1);
            if (Leaf(mother)) continue;
            for (int gc = 0; gc < 2; ++gc) {
                const int32_t g = Kid(mother, gc);
                double nm[6], sib[6], before[6];
                GetBox(n, al, nm);
                GetBox(mother, gc ^ 1, sib);
                Grow(nm, sib);
                GetBox(n, al ^ 1, before);
                const double d = Area(nm) - Area(before);
                if (d < bestD) { bestD = d; bestA = aunt; bestG = g; }
            }
        }
        if (bestG == -1 || bestA == -1) return;
        const int al = Which(bestA), gl = Which(bestG);
        const int32_t mother = Par(bestG);
        Kid(n, al) = bestG;
        Par(bestG) = n;
        Par(bestA) = mother;
        Kid(mother, gl) = bestA;
        double ab[6], gb[6], m0[6], m1[6];
        GetBox(n, al, ab);
        GetBox(mother, gl, gb);
        PutBox(n, al, gb);
        PutBox(mother, gl, ab);
        GetBox(mother, 0, m0);
        GetBox(mother, 1, m1);
        Grow(m0, m1);
        PutBox(n, al ^ 1, m0);
    }
    void UpdateAncestors(int32_t n, bool rot)
    {
        int32_t cur = n, p = Par(n);
        while (p != -1) {
            double b[6];
            NodeBox(cur, b);
            PutBox(p, Which(cur), b);
            if (rot) Rotate(p);
            cur = p;
            p = Par(cur);
        }
    }
    void InsertLeaf(int32_t* out, const Elem& e)
    {
        if (Root() == -1) {
            AllocLeaf(e, out[0], out[1]);
            Root() = out[0];
            return;
        }
        bool add = false;
        const int32_t best = DirtyTreeTestFbs(tree, e.b, &add);
        if (add) {
            const int32_t leaf = Kid(best, 0);
            leafElems[leaf].push_back(e);
            SetCount(leaf);
            double b[6];
            LeafBoxPadded(leaf, b);
            PutBox(best, 0, b);
            UpdateAncestors(best, true);
            out[0] = best;
            out[1] = leaf;
            return;
        }
        int32_t L, l;
        AllocLeaf(e, L, l);
        const int32_t oldParent = Par(best);
        const int32_t np = AllocInternal();
        Par(np) = oldParent;
        Kid(np, 0) = best;
        Kid(np, 1) = L;
        double b[6];
        NodeBox(best, b);
        PutBox(np, 0, b);
        GetBox(L, 0, b);
        PutBox(np, 1, b);
        if (oldParent != -1) Kid(oldParent, Kid(oldParent, 0) == best ? 0 : 1) = np;
        else Root() = np;
        Par(best) = np;
        Par(L) = np;
        UpdateAncestors(np, true);
        out[0] = L;
        out[1] = l;
    }
    void RemoveLeafNode(int32_t ln, int id)
    {
        const int32_t leaf = Kid(ln, 0);
        auto& v = leafElems[leaf];
        const auto it = std::find_if(v.begin(), v.end(), [id](const Elem& e) { return e.id == id; });
        if (it != v.end()) v.erase(it);
        SetCount(leaf);
        if (!v.empty()) {
            double b[6];
            LeafBoxPadded(leaf, b);
            PutBox(ln, 0, b);
            UpdateAncestors(ln, false);
            return;
        }
        const int32_t p = Par(ln);
        if (p != -1) {
            const int32_t gp = Par(p), sib = Kid(p, Kid(p, 0) == ln ? 1 : 0);
            if (gp != -1) Kid(gp, Kid(gp, 0) == p ? 0 : 1) = sib;
            else Root() = sib;
            Par(sib) = gp;
            UpdateAncestors(sib, false);
            Kid(p, 1) = freeInternal;
            freeInternal = p;
        } else {
            Root() = -1;
        }
        leafElems[leaf].clear();
        SetCount(leaf);
        Kid(ln, 1) = freeLeaf;
        freeLeaf = ln;
    }
    // TAABBTree::UpdateElement's dirty-tree part (an element with bounds) through DirtyTreeQuickInsert's site hooks.
    void Update(int id, const double* b)
    {
        Elem e{id, {}};
        memcpy(e.b, b, 48);
        int32_t out[2];
        const auto f = dirtyIdx.find(id);
        if (f == dirtyIdx.end()) {
            DirtyTreeTestInsert(tree, out, &e, e.b);
            dirtyIdx[id] = out[0];
            return;
        }
        const int32_t ln = f->second;
        double nb[6];
        GetBox(ln, 0, nb);
        if (nb[0] <= b[0] && nb[1] <= b[1] && nb[2] <= b[2] && b[3] <= nb[3] && b[4] <= nb[4] && b[5] <= nb[5]) {
            for (auto& x : leafElems[Kid(ln, 0)])
                if (x.id == id) memcpy(x.b, b, 48);
            return;  // leaf bounds recomputed; the node's padded box is kept (as the game)
        }
        RemoveLeafNode(ln, id);
        DirtyTreeTestInsert(tree, out, &e, e.b);
        f->second = out[0];
    }
    void Remove(int id)
    {
        const auto f = dirtyIdx.find(id);
        if (f == dirtyIdx.end()) return;
        RemoveLeafNode(f->second, id);
        dirtyIdx.erase(f);
    }
    // Everything consistent? Fills the reachable elements, sorted by id.
    bool Valid(ElemList* elems, const char** why)
    {
        const int32_t total = static_cast<int32_t>(nodes.size() / 0x70);
        std::vector<uint8_t> seen(total, 0);
        ElemList found;
        if (elems) found.reserve(dirtyIdx.size());
        size_t count = 0;
        std::vector<int32_t> stack;
        if (Root() != -1) {
            if (Par(Root()) != -1) { *why = "root has a parent"; return false; }
            stack.push_back(Root());
        }
        while (!stack.empty()) {
            const int32_t n = stack.back();
            stack.pop_back();
            if (n < 0 || n >= total || seen[n]) { *why = "bad or doubled node"; return false; }
            seen[n] = 1;
            if (Leaf(n)) {
                double nb[6];
                GetBox(n, 0, nb);
                const auto& le = leafElems[Kid(n, 0)];
                for (size_t i = 0; i < le.size(); ++i) {
                    const Elem& e = le[i];
                    for (int k = 0; k < 3; ++k)
                        if (e.b[k] < nb[k] || e.b[3 + k] > nb[3 + k]) { *why = "element outside its leaf node"; return false; }
                    const auto d = dirtyIdx.find(e.id);
                    if (d == dirtyIdx.end() || d->second != n) { *why = "payload index does not name its leaf node"; return false; }
                    // twice in one leaf; in two leaves the payload index check above fails for one of them
                    for (size_t j = 0; j < i; ++j)
                        if (le[j].id == e.id) { *why = "element twice"; return false; }
                    if (elems) found.push_back(e);
                    ++count;
                }
                if (leafElems[Kid(n, 0)].empty()) { *why = "empty leaf in the tree"; return false; }
                continue;
            }
            for (int c = 0; c < 2; ++c) {
                const int32_t k = Kid(n, c);
                if (k < 0 || k >= total || Par(k) != n) { *why = "child/parent mismatch"; return false; }
                double cb[6], kb[6];
                GetBox(n, c, cb);
                NodeBox(k, kb);
                if (memcmp(cb, kb, 48) != 0) { *why = "child bounds not the child's own"; return false; }
                stack.push_back(k);
            }
        }
        if (count != dirtyIdx.size()) { *why = "element count differs from the payload map"; return false; }
        for (int32_t f = freeInternal; f != -1; f = Kid(f, 1)) {
            if (f < 0 || f >= total || seen[f] || Leaf(f)) { *why = "bad internal free list"; return false; }
            seen[f] = 1;
        }
        for (int32_t f = freeLeaf; f != -1; f = Kid(f, 1)) {
            if (f < 0 || f >= total || seen[f] || !Leaf(f)) { *why = "bad leaf free list"; return false; }
            seen[f] = 1;
        }
        for (int32_t i = 0; i < total; ++i)
            if (!seen[i]) { *why = "node lost (neither in the tree nor free)"; return false; }
        if (elems) {
            std::sort(found.begin(), found.end(), [](const Elem& x, const Elem& y) { return x.id < y.id; });
            *elems = std::move(found);
        }
        return true;
    }
    int Depth(int32_t n) { return n == -1 || Leaf(n) ? 0 : 1 + std::max(Depth(Kid(n, 0)), Depth(Kid(n, 1))); }
};

int32_t* ModelInsertLeaf(uint8_t* tree, int32_t* out, void* payload, const double*)
{
    reinterpret_cast<Model*>(tree)->InsertLeaf(out, *static_cast<const Elem*>(payload));
    return out;
}
int32_t ModelFbs(uint8_t* tree, const double* bounds, bool* add)
{
    return QuickFindBestSibling(tree, bounds, add, kCap);  // a valid search stands in for the game's
}

uint32_t g_rnd = 12345;
uint32_t Rnd() { g_rnd = g_rnd * 1664525u + 1013904223u; return g_rnd >> 8; }
void RandomBox(double* b, bool big)
{
    const double x = Rnd() % 20000, y = Rnd() % 20000, z = Rnd() % 2000;
    const double w = 5 + Rnd() % (big ? 3000 : 200), h = 5 + Rnd() % 200, d = 5 + Rnd() % 300;
    b[0] = x; b[1] = y; b[2] = z; b[3] = x + w; b[4] = y + h; b[5] = z + d;
}

// Option 2: a fake FlushInternal (counts, empties the queue) and a fake evolution with the queue count at +0x368.
std::atomic<int> g_fiCalls{0};
std::atomic<DWORD> g_fiThread{0};
std::atomic<bool> g_fiOverlap{false};
std::atomic<bool> g_asyncRunning{false};
void FakeFlushInternal(uint8_t* evolution)
{
    if (*reinterpret_cast<int32_t*>(evolution + 0x368) == 0) return;  // an empty flush: nothing to see
    g_fiCalls.fetch_add(1);
    g_fiThread = GetCurrentThreadId();
    for (int i = 0; i < 20 && !g_fiOverlap.load(); ++i) {
        if (g_asyncRunning.load()) g_fiOverlap = true;
        Sleep(1);
    }
    *reinterpret_cast<int32_t*>(evolution + 0x368) = 0;
}
}  // namespace bit

static void BulkInsertSelfTest()
{
    using namespace bit;
    static const int32_t cap = kCap;
    DirtyTreeTestSetup(reinterpret_cast<void*>(&ModelInsertLeaf), reinterpret_cast<void*>(&ModelFbs), &cap);
    SetDirtyTreeQuickInsert(false);
    auto stock = std::make_unique<Model>();
    auto bulk = std::make_unique<Model>();
    const int minOps = 50;
    std::vector<int> live;  // ids in the trees
    int nextId = 1;
    bool same = true, valid = true, midValid = true;
    const char* why = "";
    int batchedPasses = 0, maxN = 0, largestDepth = 0;
    for (int pass = 0; pass < 400 && same && valid; ++pass) {
        // one flush: distinct ids (adds, moves, in-place updates, removes); some passes small (not batched)
        const int n = (pass % 5 == 0) ? static_cast<int>(Rnd() % 40) : static_cast<int>(Rnd() % 1500);
        struct Op { int kind; int id; double b[6]; };
        std::vector<Op> ops;
        std::set<int> used;
        for (int k = 0; k < n; ++k) {
            Op op{};
            const unsigned r = Rnd() % 100;
            if (live.empty() || r < 55) {
                op.kind = 0;
                op.id = nextId++;
                RandomBox(op.b, Rnd() % 10 == 0);
            } else {
                op.id = live[Rnd() % live.size()];
                if (!used.insert(op.id).second) continue;
                if (r < 75) { op.kind = 1; RandomBox(op.b, false); }  // moved far
                else if (r < 90) {  // a small move (often inside the padded leaf box)
                    op.kind = 1;
                    RandomBox(op.b, false);
                    for (const auto& le : stock->leafElems[stock->Kid(stock->dirtyIdx[op.id], 0)])
                        if (le.id == op.id) { memcpy(op.b, le.b, 48); for (int a = 0; a < 3; ++a) { op.b[a] += 1; op.b[3 + a] += 1; } }
                } else op.kind = 2;
            }
            ops.push_back(op);
        }
        auto run = [&](Model& m, bool on, bool checkMid) {
            BulkIndexInsertTestSetup(on, minOps);
            BulkBatchBegin(static_cast<int>(ops.size()));
            int k = 0;
            for (const Op& op : ops) {
                if (op.kind == 2) m.Remove(op.id);
                else m.Update(op.id, op.b);
                if (checkMid && (++k % 97 == 0) && !m.Valid(nullptr, &why)) midValid = false;
            }
            BulkBatchEnd();
        };
        run(*stock, false, false);
        run(*bulk, true, pass % 7 == 0);
        if (static_cast<int>(ops.size()) >= minOps) ++batchedPasses;
        maxN = std::max(maxN, static_cast<int>(ops.size()));
        for (const Op& op : ops) {
            if (op.kind == 0) live.push_back(op.id);
            if (op.kind == 2) live.erase(std::find(live.begin(), live.end(), op.id));
        }
        ElemList a, b;
        const char* w1 = "";
        const char* w2 = "";
        const bool v1 = stock->Valid(&a, &w1), v2 = bulk->Valid(&b, &w2);
        if (!v1) why = w1;
        if (!v2) why = w2;
        valid = v1 && v2;
        same = SameElems(a, b);
        if (pass % 50 == 0) largestDepth = std::max(largestDepth, bulk->Depth(bulk->Root()));
    }
    const BulkTestStats st = BulkIndexInsertTestStats();
    printf("      (BulkIndexInsert model: %d of 400 flushes batched, largest %d changes, %d elements at the end; %llu subtrees shaped, "
           "%llu left to the game's path; depth %d (batched) vs %d (stock); %s)\n",
           batchedPasses, maxN, static_cast<int>(live.size()), static_cast<unsigned long long>(st.rebuilt),
           static_cast<unsigned long long>(st.broken), bulk->Depth(bulk->Root()), stock->Depth(stock->Root()), why);
    Check(valid, "BulkIndexInsert: 400 random flushes: both trees consistent (parents, bounds, payload indices, free lists, no lost node)");
    Check(same, "BulkIndexInsert: 400 random flushes: batched and one-by-one trees hold the same elements with the same bounds");
    Check(midValid, "BulkIndexInsert: inside a batched flush the tree is valid and complete after every 97th change");
    Check(st.rebuilt > 0 && st.broken == 0, "BulkIndexInsert: subtrees shaped, none left to the game's path");

    // The earlier tree removed during a batch (its root was a one-element leaf: the attach node is freed by the game),
    // and an element inserted earlier in the same batch removed again (never in the game's queues): still valid.
    {
        Model m;
        BulkIndexInsertTestSetup(true, 1);
        double b[6];
        RandomBox(b, false);
        m.Update(900000, b);  // the earlier tree: one leaf
        BulkBatchBegin(100);
        for (int i = 0; i < 40; ++i) { RandomBox(b, false); m.Update(900001 + i, b); }
        m.Remove(900000);
        for (int i = 40; i < 80; ++i) { RandomBox(b, false); m.Update(900001 + i, b); }
        BulkBatchEnd();
        const bool v1 = m.Valid(nullptr, &why);
        BulkBatchBegin(100);
        for (int i = 80; i < 120; ++i) { RandomBox(b, false); m.Update(900001 + i, b); }
        m.Remove(900001 + 100);  // inserted in this batch
        for (int i = 120; i < 160; ++i) { RandomBox(b, false); m.Update(900001 + i, b); }
        BulkBatchEnd();
        ElemList e;
        const bool v2 = m.Valid(&e, &why);
        Check(v1 && v2 && e.size() == 159, "BulkIndexInsert: earlier tree removed in a batch, an element of the batch removed again: valid, 159 elements");
    }

    // Build time of the largest case seen in the game (an 11k-change swap flush).
    {
        Model m;
        BulkIndexInsertTestSetup(true, 1);
        double b[6];
        const uint64_t t0 = GetTickCount64();
        LARGE_INTEGER f, q0, q1, q2;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        BulkBatchBegin(11000);
        for (int i = 0; i < 11000; ++i) { RandomBox(b, false); m.Update(1000000 + i, b); }
        QueryPerformanceCounter(&q1);
        BulkBatchEnd();
        QueryPerformanceCounter(&q2);
        (void)t0;
        Model s;
        BulkIndexInsertTestSetup(false, 1);
        LARGE_INTEGER s0, s1;
        QueryPerformanceCounter(&s0);
        for (int i = 0; i < 11000; ++i) { RandomBox(b, false); s.Update(1000000 + i, b); }
        QueryPerformanceCounter(&s1);
        const double ms = 1000.0 / static_cast<double>(f.QuadPart);
        printf("      (11,000 inserts into the model: batched %.2f ms + shaping %.2f ms, depth %d; one by one %.2f ms, depth %d)\n",
               (q1.QuadPart - q0.QuadPart) * ms, (q2.QuadPart - q1.QuadPart) * ms, m.Depth(m.Root()), (s1.QuadPart - s0.QuadPart) * ms,
               s.Depth(s.Root()));
        Check(m.Valid(nullptr, &why) && m.Depth(m.Root()) <= 16, "BulkIndexInsert: 11,000 inserts in one batch: valid, balanced (depth <= 16)");
    }
    BulkIndexInsertTestSetup(false, 256);

    // PhysicsInternalFlushParallel: FlushInternal beside FlushAsync (fake FlushInternal, fake evolution).
    {
        alignas(16) static uint8_t evo[0x400];
        static int32_t cvar = 1;
        // helperExpected: "FlushAsync" lasts until the helper has started (up to 2 s), as the game's does for many
        // milliseconds; a fixed 5 ms window failed when a freshly created helper thread was scheduled later than that.
        auto runSwap = [&](bool on, int queued, int cv, bool helperExpected) {
            memset(evo, 0, sizeof evo);
            *reinterpret_cast<int32_t*>(evo + 0x368) = queued;
            cvar = cv;
            g_fiCalls = 0;
            g_fiThread = 0;
            g_fiOverlap = false;
            BulkParallelTestSetup(reinterpret_cast<void*>(&FakeFlushInternal), 0x368, &cvar, on);
            BulkFlushAsyncBegin(evo);
            g_asyncRunning = true;
            if (helperExpected) for (int i = 0; i < 2000 && g_fiCalls.load() == 0; ++i) Sleep(1);
            Sleep(5);  // "FlushAsync"
            g_asyncRunning = false;
            BulkFlushAsyncEnd();
            BulkTestFlushInternal(evo);  // the step's own call afterwards (skipped when the queue is empty)
        };
        runSwap(true, 2000, 1, true);
        const bool ran = g_fiCalls == 1 && g_fiThread != GetCurrentThreadId() && g_fiOverlap.load() &&
                         *reinterpret_cast<int32_t*>(evo + 0x368) == 0;
        runSwap(true, 500, 0, false);
        const bool cvarOff = g_fiCalls == 1 && g_fiThread == GetCurrentThreadId();
        runSwap(false, 500, 1, false);
        const bool off = g_fiCalls == 1 && g_fiThread == GetCurrentThreadId();
        runSwap(true, 10, 1, false);
        const bool fewOps = g_fiCalls == 1 && g_fiThread == GetCurrentThreadId();
        Check(ran, "PhysicsInternalFlushParallel: FlushInternal runs once, on the helper, while FlushAsync runs; the step's own call finds an empty queue");
        Check(cvarOff && off && fewOps, "PhysicsInternalFlushParallel: not when the game drops the queue (switch 0), when off, or for a small queue");
        BulkParallelTestSetup(reinterpret_cast<void*>(&FakeFlushInternal), 0x368, &cvar, false);
    }
}
