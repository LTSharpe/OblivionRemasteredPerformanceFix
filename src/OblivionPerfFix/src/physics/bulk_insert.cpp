// BulkIndexInsert ("equivalent" class): when one flush of the physics step files many new or moved collision
// elements into a dirty-element tree, they are filed as one subtree that gets its final shape at the end of the flush.
// PhysicsInternalFlushParallel (identical): a swap step's FlushInternal runs beside its FlushAsync.
//
// Where the time goes (Kvatch spot C, 2026-10-14, PhysicsFlushParallel, ParallelStaticBodies, PipelinedSceneAdd and DirtyTreeQuickInsert and the helper caches on): when a forest streams
// in, every physics step files the bodies registered since the last step into the collision index
// (FPBDRigidsEvolutionBase::FlushInternalAccelerationQueue, exe+0x1932160: 4,500-5,300 per step), and a step whose
// background rebuild finished catches the rebuilt structures up with everything queued meanwhile (FlushAsync, 8-11k
// changes, then FlushInternal). Each change is TAABBTree::UpdateElement (AABBTree.h), which files a new or moved element
// into the bucket's dirty-element tree (a dynamic AABB tree, DirtyElementTree->InsertLeaf): a sibling search, a new leaf
// and parent, and a walk to the root that refits bounds and tries a rotation at every level. The game thread waits for
// that step in its tick groups: 51-58 ms over C's arrival, the worst frames 52-54 ms.
//
// BulkIndexInsert: while a flush pass runs (FlushInternal; FlushAsync's two passes, PhysicsFlushParallel), the dirty-tree inserts of that pass
// skip the search and the walk: each new leaf is paired with the pass's subtree so far (the game's own InsertLeaf with its
// sibling search answering "that node", so leaf, element, node and free-list bookkeeping are the game's), and the subtree
// hangs under one node next to the tree's earlier root. At the end of the pass the subtree's internal nodes are re-linked
// into a balanced tree over the same leaves (median split on the longest axis of the leaf centres), and its bounds are
// set from the leaves. So:
//  - The same elements with the same bounds, each in the same leaf node the game returned for it (the payload's
//    DirtyPayloadIdx, which later updates and removals use, stays right); the same number of nodes and leaves as the
//    game's inserts would allocate without leaf sharing; the free lists are the game's.
//  - The tree is a valid tree with every element reachable at every moment, also inside the pass (the subtree is a chain
//    until the end); nobody else reads these trees during the pass (the physics-thread structure is read by the step
//    after the flush; the game-thread copy is handed over only after the swap).
//  - Only the shape differs, which stock already varies from run to run (the background rebuild is polled with
//    IsComplete, so which elements sit in the built tree or the dirty tree, and both shapes, depend on worker timing),
//    and the next rebuild replaces the tree (built from the element cache, not from the dirty tree). Queries visit every
//    element whose bounds intersect; the shape only changes their visiting order (ties between equal-distance hits, the
//    order of unsorted overlap results and of broadphase pairs). Accepted as "equivalent" (same results, other internal order) on 2026-10-14.
// Only passes with at least BulkIndexInsertMin changes (256) are batched; anything unexpected in a tree's subtree (a check
// before every insert and before the re-link) leaves that tree to the game's path for the rest of the pass, still valid.
//
// PhysicsInternalFlushParallel: in a swap step, ComputeIntermediateSpatialAcceleration calls FlushAsync and then (cvar exe+0x8DC0490 == 1)
// FlushInternal. FlushAsync writes the rebuilt structures (+0x2E0, +0x2E8), the element cache and its own queue;
// FlushInternal writes the current structure (+0x2D8) and its own queue; both only read the particles. So FlushInternal
// runs on a helper while FlushAsync runs, and the step's own call afterwards finds an empty queue (an empty flush writes
// the same timestamp and resets nothing else). Identical results.
//
// PhysicsQueueShrinkOnce (identical contents): the game thread's catch-up of a new game-thread structure
// (UpdateExternalAccelerationStructure_External, which tail-jumps into FlushExternalAccelerationQueue, exe+0x1931D50)
// walks its pending-change queue backwards and removes every change the step has already consumed:
// ExternalQueue.Remove(UniqueIdx) -> TArray::RemoveAtSwap with shrinking allowed (PBDRigidsEvolution.cpp 680). With
// 0x48-byte entries, UE's shrink rule (DefaultCalculateSlackShrink: 16 KB or a third of the allocation unused, and more
// than 64 free) reallocates and copies the whole queue every ~228 removals. At a wood arrival (Kvatch C, 2026-10-15) the
// queue holds 9-11k changes and most are removed: ~45 reallocations of up to ~0.8 MB each, every one a fresh block
// from Windows (page faults) and a full copy, 6 of the 9 game-thread samples of C's catch-ups. PhysicsQueueShrinkOnce lets the removals
// keep the allocation during the catch-up and calls the game's own shrink (exe+0x15F2100) once at its end. The queue
// holds the same changes in the same order with the same count at every moment (only its spare capacity differs while
// the catch-up runs); afterwards the game's rule has been applied to the final count, as after its last removal in the
// stock game. Nothing keeps a pointer into the queue across a removal (the loop re-reads the data pointer).
// Found by shape: the removal's call (dec [r12+8]; mov rcx,r12; call shrink; mov rcx,[r12+10h]; ...), the shrink's
// first instructions (0x48-byte entries, the 16 KB rule), and the call's function = the one the catch-up tail-jumps to
// (unwind data). Rides on BulkIndexInsert's catch-up detour; installed at start-up, then live.
//
// Install (start-up only): DirtyTreeQuickInsert's three call sites (also when DirtyTreeQuickInsert is off; its greedy search keeps its own switch) and
// an entry detour of FlushInternal (found through the step's swap: call FlushAsync; cmp [cvar],1; jne; call
// FlushInternal); PhysicsFlushParallel's FlushAsync hook calls in here. Then both keys switch live.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace opf {
namespace {

// TAABBTree layout (pinned by DirtyTreeQuickInsert's patterns): Nodes data +0x18, RootNode +0x54; node 0x70 bytes: ChildrenBounds[2]
// (min xyz, max xyz doubles) +0x00/+0x30, ChildrenNodes +0x60/+0x64, ParentNode +0x68, flags +0x6C (bit 0 = leaf).
constexpr size_t kNodes = 0x18, kRoot = 0x54, kNodeSize = 0x70, kChildren = 0x60, kParent = 0x68, kFlags = 0x6C;
constexpr int kMaxTrees = 8;   // trees batched per pass and thread (buckets of one structure)
constexpr int kGuard = 1 << 20;

// The physics step's swap: mov rcx,rdi; mov bpl,1; call FlushAsync; cmp dword [cvar],1; jne; mov rcx,rdi; call FlushInternal
constexpr char kSwap[] = "48 8B CF 40 B5 01 E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 01 75 ?? 48 8B CF E8";
constexpr size_t kSwapAsync = 6, kSwapCvar = 11, kSwapInternal = 23;
// FlushInternalAccelerationQueue: mov r11,rsp; mov [r11+8],rcx; sub rsp,..; mov [r11-8],rbx; xor r9d,r9d; mov [r11-18h],rsi;
// lea rbx,[rcx+InternalAcceleration]; lea rsi,[rcx+InternalAccelerationQueue]
constexpr char kFiEntry[] = "4C 8B DC 49 89 4B 08 48 81 EC ?? ?? 00 00 49 89 5B F8 45 33 C9 49 89 73 E8 48 8D 99 ?? ?? 00 00 48 8D B1 ?? ?? 00 00";
constexpr size_t kFiQueue = 0x23;
constexpr uint8_t kFiPrologue[7] = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x4B, 0x08};
// FPBDRigidsEvolutionBase::UpdateExternalAccelerationStructure_External (exe+0x19474D0; FlushExternalAccelerationQueue
// inlined): the game thread, at the end of its frame and under the scene's write lock, takes the newest game-thread
// structure from the step and applies its own pending changes to it. push rbx; push rdi; push r14; sub rsp,20h;
// mov [rsp+48h],rsi; mov r14,r8; mov [rsp+50h],r15; mov rsi,rdx; xor r15d,r15d; mov [rsp+40h],rbp; mov edi,r15d; mov rbx,rcx
constexpr char kExtEntry[] =
    "40 53 57 41 56 48 83 EC 20 48 89 74 24 48 4D 8B F0 4C 89 7C 24 50 48 8B F2 45 33 FF 48 89 6C 24 40 41 8B FF 48 8B D9";
constexpr uint8_t kExtPrologue[5] = {0x40, 0x53, 0x57, 0x41, 0x56};
// PhysicsQueueShrinkOnce: in FlushExternalAccelerationQueue (exe+0x1931D50, chunk exe+0x1931D98): the queue removal's shrink call
// (exe+0x19320CD): dec dword [r12+8]; mov rcx,r12; call ResizeShrink; mov rcx,[r12+10h]; mov byte [rbp+..],0;
// mov rax,[rbp+..]; mov [rcx+rbx*8],rax
constexpr char kShrinkSite[] = "41 FF 4C 24 08 49 8B CC E8 ?? ?? ?? ?? 49 8B 4C 24 10 C6 45 ?? 00 48 8B 45 ?? 48 89 04 D9";
constexpr size_t kShrinkCall = 8;
// TArray<FPendingSpatialData>::ResizeShrink (exe+0x15F2100): push rbx; sub rsp,30h; mov r8d,[rcx+0Ch] (Max);
// mov rbx,rcx; movsxd r9,[rcx+8] (Num); ...; lea rdx,[rax+rax*8]; shl rdx,3 (slack * 0x48); cmp rdx,4000h
constexpr char kShrinkFn[] =
    "40 53 48 83 EC 30 44 8B 41 0C 48 8B D9 4C 63 49 08 45 8B D0 45 2B D1 49 63 C2 48 8D 14 C0 48 C1 E2 03 48 81 FA 00 40 00 00";
constexpr size_t kExtTailSearch = 0x100;  // the catch-up's tail jump to FlushExternalAccelerationQueue is this close

struct Box { double mn[3], mx[3]; };
inline Box Union(const Box& a, const Box& b)
{
    Box r;
    for (int i = 0; i < 3; ++i) { r.mn[i] = a.mn[i] < b.mn[i] ? a.mn[i] : b.mn[i]; r.mx[i] = a.mx[i] > b.mx[i] ? a.mx[i] : b.mx[i]; }
    return r;
}
inline uint8_t* NodeAt(uint8_t* tree, int32_t i) { return *reinterpret_cast<uint8_t**>(tree + kNodes) + static_cast<size_t>(i) * kNodeSize; }
inline int32_t& RootOf(uint8_t* tree) { return *reinterpret_cast<int32_t*>(tree + kRoot); }
inline int32_t& ChildOf(uint8_t* n, int c) { return *reinterpret_cast<int32_t*>(n + kChildren + 4 * c); }
inline int32_t& ParentOf(uint8_t* n) { return *reinterpret_cast<int32_t*>(n + kParent); }
inline bool IsLeaf(const uint8_t* n) { return (n[kFlags] & 1) != 0; }
inline Box ChildBox(const uint8_t* n, int c) { Box b; memcpy(&b, n + 0x30 * c, sizeof b); return b; }
inline void SetChildBox(uint8_t* n, int c, const Box& b) { memcpy(n + 0x30 * c, &b, sizeof b); }
inline Box NodeBox(const uint8_t* n) { return IsLeaf(n) ? ChildBox(n, 0) : Union(ChildBox(n, 0), ChildBox(n, 1)); }

// Bounds of every ancestor of `node` from its own (no rotations; as UpdateAncestorBounds without them).
void RefitUp(uint8_t* tree, int32_t node)
{
    int32_t cur = node;
    for (int g = 0; g < kGuard; ++g) {
        uint8_t* const c = NodeAt(tree, cur);
        const int32_t p = ParentOf(c);
        if (p == -1) return;
        uint8_t* const pn = NodeAt(tree, p);
        SetChildBox(pn, ChildOf(pn, 0) == cur ? 0 : 1, NodeBox(c));
        cur = p;
    }
}

// ---- per-thread batch of one flush pass ----
struct TreeBatch {
    uint8_t* tree = nullptr;
    int32_t attach = -1;  // our node pairing the tree's earlier root (child 0) with the subtree (child 1); -1 = subtree is root
    int32_t sub = -1;     // the subtree's root
    bool broken = false;  // something else changed the subtree: the game's path for the rest of the pass
    std::vector<int32_t> leaves, inner;  // leaf nodes in insert order; inner[k] = parent made by insert k + 1
};
struct Item { uint32_t code; int32_t node; };  // Morton code of the leaf's centre
struct Batch {
    int depth = 0;
    bool active = false;
    int trees = 0;
    TreeBatch t[kMaxTrees];
    std::vector<Item> items, sorted;
    std::vector<double> centres;
    std::vector<int32_t> pool;
};
thread_local Batch* t_batch = nullptr;

std::atomic<bool> g_bulkOn{false};
std::atomic<int> g_minOps{256};
std::atomic<bool> g_parOn{false};
bool g_installed = false;

// statistics
std::atomic<uint64_t> s_passes{0}, s_inserts{0}, s_rebuilt{0}, s_broken{0}, s_fallback{0}, s_maxLeaves{0}, s_buildUs{0}, s_maxBuildUs{0};
std::atomic<uint64_t> s_par{0}, s_parOps{0}, s_parUs{0}, s_parWaitUs{0};

LARGE_INTEGER Freq()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    return f;
}
uint64_t Ticks()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}
uint64_t TicksToUs(uint64_t t) { return QpcToMicroseconds(t, static_cast<uint64_t>(Freq().QuadPart)); }
void AtomicMax(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

Batch* ThisBatch()
{
    if (!t_batch) t_batch = new Batch();  // one per thread that runs a flush pass (the physics step's, our helpers); kept
    return t_batch;
}

// Is the subtree still where this batch left it? (sub under attach, attach the root; or sub the root.)
bool SubtreeInPlace(TreeBatch& tb)
{
    uint8_t* const tree = tb.tree;
    const int32_t parent = ParentOf(NodeAt(tree, tb.sub));
    if (tb.attach != -1 && parent == -1 && RootOf(tree) == tb.sub) tb.attach = -1;  // the earlier part was removed (game freed attach)
    if (tb.attach == -1) return parent == -1 && RootOf(tree) == tb.sub;
    if (parent != tb.attach || RootOf(tree) != tb.attach) return false;
    uint8_t* const a = NodeAt(tree, tb.attach);
    return !IsLeaf(a) && ChildOf(a, 1) == tb.sub;
}

int32_t* Fallback(TreeBatch& tb, uint8_t* tree, int32_t* out, void* payload, const double* bounds)
{
    if (!tb.broken) {
        tb.broken = true;
        s_broken.fetch_add(1, std::memory_order_relaxed);
    }
    return DirtyInsertDefault(tree, out, payload, bounds);
}

// One dirty-tree insert of a batched pass.
int32_t* ChainInsert(TreeBatch& tb, int32_t* out, void* payload, const double* bounds)
{
    uint8_t* const tree = tb.tree;
    if (tb.leaves.empty()) {
        const int32_t root = RootOf(tree);
        if (root == -1) {  // empty tree: the game's own first leaf becomes the root
            InsertLeafForcedSibling(tree, out, payload, bounds, -1);
            tb.attach = -1;
            tb.sub = out[0];
        } else {  // pair with the root: the new parent becomes the root, the earlier tree its child 0, the leaf child 1
            if (ParentOf(NodeAt(tree, root)) != -1) return Fallback(tb, tree, out, payload, bounds);
            InsertLeafForcedSibling(tree, out, payload, bounds, root);
            const int32_t a = ParentOf(NodeAt(tree, out[0]));
            if (a == -1 || RootOf(tree) != a || ChildOf(NodeAt(tree, a), 0) != root || ChildOf(NodeAt(tree, a), 1) != out[0]) {
                tb.broken = true;  // inserted and valid, but not ours to reshape
                s_broken.fetch_add(1, std::memory_order_relaxed);
                return out;
            }
            tb.attach = a;
            tb.sub = out[0];
        }
        tb.leaves.push_back(out[0]);
        return out;
    }
    if (!SubtreeInPlace(tb)) return Fallback(tb, tree, out, payload, bounds);
    const int32_t sub = tb.sub, attach = tb.attach, savedRoot = RootOf(tree);
    if (attach != -1) {  // detach the subtree for the game's insert (no parent: no ancestor walk, no rotation)
        ParentOf(NodeAt(tree, sub)) = -1;
        RootOf(tree) = sub;
    }
    InsertLeafForcedSibling(tree, out, payload, bounds, sub);
    const int32_t leaf = out[0];
    const int32_t n = ParentOf(NodeAt(tree, leaf));
    uint8_t* const nn = n >= 0 ? NodeAt(tree, n) : nullptr;
    const bool ok = nn && RootOf(tree) == n && ParentOf(nn) == -1 && ChildOf(nn, 0) == sub && ChildOf(nn, 1) == leaf;
    if (attach != -1) {  // hang the grown subtree back under attach
        const int32_t top = ok ? n : RootOf(tree);
        ParentOf(NodeAt(tree, top)) = attach;
        uint8_t* const a = NodeAt(tree, attach);
        ChildOf(a, 1) = top;
        SetChildBox(a, 1, NodeBox(NodeAt(tree, top)));
        RootOf(tree) = savedRoot;
        RefitUp(tree, attach);
    }
    if (!ok) {
        tb.broken = true;
        s_broken.fetch_add(1, std::memory_order_relaxed);
        return out;
    }
    tb.inner.push_back(n);
    tb.leaves.push_back(leaf);
    tb.sub = n;
    return out;
}

// The subtree as recorded? inner[k] = (k ? inner[k-1] : leaves[0], leaves[k+1]), every leaf a leaf, parents consistent.
bool ChainIntact(const TreeBatch& tb)
{
    uint8_t* const tree = tb.tree;
    const size_t n = tb.leaves.size();
    if (tb.inner.size() + 1 != n || tb.sub != tb.inner.back()) return false;
    for (size_t k = 0; k < tb.inner.size(); ++k) {
        uint8_t* const node = NodeAt(tree, tb.inner[k]);
        const int32_t left = k ? tb.inner[k - 1] : tb.leaves[0], right = tb.leaves[k + 1];
        if (IsLeaf(node) || ChildOf(node, 0) != left || ChildOf(node, 1) != right) return false;
        if (ParentOf(NodeAt(tree, left)) != tb.inner[k] || ParentOf(NodeAt(tree, right)) != tb.inner[k]) return false;
    }
    for (const int32_t l : tb.leaves)
        if (!IsLeaf(NodeAt(tree, l))) return false;
    return true;
}

int32_t Build(uint8_t* tree, const Item* it, int lo, int hi, const int32_t* pool, int& next, Box& box)
{
    if (hi - lo == 1) {
        box = ChildBox(NodeAt(tree, it[lo].node), 0);
        return it[lo].node;
    }
    const int mid = lo + (hi - lo) / 2;  // the leaves are in Morton order: each half is a compact region
    const int32_t self = pool[next++];
    Box lb, rb;
    const int32_t l = Build(tree, it, lo, mid, pool, next, lb);
    const int32_t r = Build(tree, it, mid, hi, pool, next, rb);
    uint8_t* const n = NodeAt(tree, self);
    ChildOf(n, 0) = l;
    ChildOf(n, 1) = r;
    SetChildBox(n, 0, lb);
    SetChildBox(n, 1, rb);
    ParentOf(NodeAt(tree, l)) = self;
    ParentOf(NodeAt(tree, r)) = self;
    box = Union(lb, rb);
    return self;
}

// End of a pass: the subtree's internal nodes re-linked into a balanced tree over its leaves.
void Finish(Batch& b, TreeBatch& tb)
{
    const size_t n = tb.leaves.size();
    s_inserts.fetch_add(n, std::memory_order_relaxed);
    if (tb.broken || n < 3) return;
    if (!SubtreeInPlace(tb) || !ChainIntact(tb)) {
        s_broken.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t t0 = Ticks();
    uint8_t* const tree = tb.tree;
    // leaf centres -> 30-bit Morton codes within the subtree's centre bounds -> radix sort (4 x 8 bits, stable)
    std::vector<Item>& it = b.items;
    it.resize(n);
    b.sorted.resize(n);
    b.centres.resize(3 * n);
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (size_t i = 0; i < n; ++i) {
        const Box lb = ChildBox(NodeAt(tree, tb.leaves[i]), 0);
        for (int a = 0; a < 3; ++a) {
            const double c = 0.5 * (lb.mn[a] + lb.mx[a]);
            b.centres[3 * i + a] = c;
            lo[a] = c < lo[a] ? c : lo[a];
            hi[a] = c > hi[a] ? c : hi[a];
        }
    }
    double scale[3];
    for (int a = 0; a < 3; ++a) scale[a] = hi[a] > lo[a] ? 1023.0 / (hi[a] - lo[a]) : 0.0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t code = 0;
        for (int a = 0; a < 3; ++a) {
            const double v = (b.centres[3 * i + a] - lo[a]) * scale[a];
            uint32_t q = v <= 0 ? 0u : v >= 1023 ? 1023u : static_cast<uint32_t>(v);
            q = (q | (q << 16)) & 0x030000FF;  // 10 bits -> every third bit
            q = (q | (q << 8)) & 0x0300F00F;
            q = (q | (q << 4)) & 0x030C30C3;
            q = (q | (q << 2)) & 0x09249249;
            code |= q << a;
        }
        it[i].code = code;
        it[i].node = tb.leaves[i];
    }
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t count[257] = {};
        for (size_t i = 0; i < n; ++i) ++count[((it[i].code >> shift) & 0xFF) + 1];
        for (int k = 0; k < 256; ++k) count[k + 1] += count[k];
        for (size_t i = 0; i < n; ++i) b.sorted[count[(it[i].code >> shift) & 0xFF]++] = it[i];
        it.swap(b.sorted);
    }
    int next = 0;
    Box box;
    const int32_t top = Build(tree, b.items.data(), 0, static_cast<int>(n), tb.inner.data(), next, box);
    if (tb.attach != -1) {
        uint8_t* const a = NodeAt(tree, tb.attach);
        ChildOf(a, 1) = top;
        ParentOf(NodeAt(tree, top)) = tb.attach;
        SetChildBox(a, 1, box);
        RefitUp(tree, tb.attach);
    } else {
        ParentOf(NodeAt(tree, top)) = -1;
        RootOf(tree) = top;
    }
    tb.sub = top;
    const uint64_t dt = Ticks() - t0;
    s_rebuilt.fetch_add(1, std::memory_order_relaxed);
    AtomicMax(s_maxLeaves, n);
    s_buildUs.fetch_add(TicksToUs(dt), std::memory_order_relaxed);
    AtomicMax(s_maxBuildUs, TicksToUs(dt));
}

int32_t* InsertCore(uint8_t* tree, int32_t* out, void* payload, const double* bounds);

// DirtyTreeQuickInsert's dirty-tree insert sites come here (SetDirtyInsertHook)
int32_t* InsertHook(uint8_t* tree, int32_t* out, void* payload, const double* bounds)
{
    return InsertCore(tree, out, payload, bounds);
}

int32_t* InsertCore(uint8_t* tree, int32_t* out, void* payload, const double* bounds)
{
    Batch* const b = t_batch;
    if (!b || !b->active) {
        return DirtyInsertDefault(tree, out, payload, bounds);
    }
    TreeBatch* tb = nullptr;
    for (int i = 0; i < b->trees && !tb; ++i)
        if (b->t[i].tree == tree) tb = &b->t[i];
    if (!tb) {
        if (b->trees == kMaxTrees) {
            s_fallback.fetch_add(1, std::memory_order_relaxed);
            return DirtyInsertDefault(tree, out, payload, bounds);
        }
        tb = &b->t[b->trees++];
        tb->tree = tree;
        tb->attach = tb->sub = -1;
        tb->broken = false;
        tb->leaves.clear();
        tb->inner.clear();
    }
    if (tb->broken) return DirtyInsertDefault(tree, out, payload, bounds);
    return ChainInsert(*tb, out, payload, bounds);
}

// ---- FlushInternal's entry detour and PhysicsInternalFlushParallel's helper ----
using FlushFn = void (*)(uint8_t* evolution);
FlushFn g_fiOriginal = nullptr;  // trampoline (or a fake in the self-test)
uint8_t* g_fi = nullptr;
uint32_t g_fiNum = 0;           // the internal queue's count (evolution + queue + 8)
const volatile int32_t* g_fiCvar = nullptr;  // p.Chaos.? (exe+0x8DC0490): 1 = the swap step calls FlushInternal
uint8_t* g_region = nullptr;
SRWLOCK g_parLock = SRWLOCK_INIT;
HANDLE g_parGo = nullptr, g_parDone = nullptr, g_parThread = nullptr;
uint8_t* g_parEvolution = nullptr;
uint64_t g_parUs = 0;
thread_local bool t_parRunning = false;
thread_local uint64_t t_parStart = 0;
thread_local uint64_t t_faStart = 0;
constexpr int kParMinOps = 1024;  // 31b only where the overlap pays for the hand-off (streaming arrivals)

void FiHook(uint8_t* evolution)
{
    const int ops = *reinterpret_cast<const int32_t*>(evolution + g_fiNum);
    if (ops <= 0) {
        g_fiOriginal(evolution);
        return;
    }
    BulkBatchBegin(ops);
    g_fiOriginal(evolution);
    BulkBatchEnd();
}

// PhysicsQueueShrinkOnce: the queue removal's shrink, while the game thread's catch-up runs on this thread.
using ShrinkFn = uint64_t (*)(uint8_t* queue);
ShrinkFn g_shrinkReal = nullptr;   // the game's ResizeShrink (or a fake in the self-test)
std::atomic<bool> g_shrinkOn{false};
bool g_shrinkInstalled = false;
uint8_t* g_shrinkSite = nullptr;  // the redirected call (self-test)
thread_local uint8_t* t_extQueue = nullptr;
thread_local uint32_t t_shrinkSkipped = 0;
std::atomic<uint64_t> s_shrinkCatchUps{0}, s_shrinkSkipped{0}, s_shrinkMaxSkipped{0};
uint64_t ShrinkHook(uint8_t* queue)
{
    if (queue && queue == t_extQueue && g_shrinkOn.load(std::memory_order_relaxed)) {
        ++t_shrinkSkipped;
        return 0;  // the caller ignores the result
    }
    return g_shrinkReal(queue);
}


// The game thread's catch-up of a new game-thread structure (queue: TArray of 0x48-byte ops, count at +8).
using ExtFn = void (*)(uint8_t* evolution, void** structure, uint8_t* queue);
ExtFn g_extOriginal = nullptr;
void ExtHook(uint8_t* evolution, void** structure, uint8_t* queue)
{
    const int ops = queue ? *reinterpret_cast<const int32_t*>(queue + 8) : 0;
    uint8_t* const outerQueue = t_extQueue;
    const uint32_t outerSkipped = t_shrinkSkipped;
    t_extQueue = queue;
    t_shrinkSkipped = 0;
    BulkBatchBegin(ops);
    g_extOriginal(evolution, structure, queue);
    BulkBatchEnd();
    const uint32_t skipped = t_shrinkSkipped;
    t_extQueue = outerQueue;
    t_shrinkSkipped = outerSkipped;
    if (skipped) {  // PhysicsQueueShrinkOnce: the game's shrink once, on the final count
        g_shrinkReal(queue);
        s_shrinkCatchUps.fetch_add(1, std::memory_order_relaxed);
        s_shrinkSkipped.fetch_add(skipped, std::memory_order_relaxed);
        AtomicMax(s_shrinkMaxSkipped, skipped);
    }
}

// The function a code address belongs to, through the exe's unwind data (chained entries followed to the primary one).
uint8_t* PrimaryFunction(const GameModule& game, const uint8_t* at)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(game.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY)) return nullptr;
    const auto* table = reinterpret_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(game.base + dir.VirtualAddress);
    const size_t n = dir.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
    uint32_t rva = static_cast<uint32_t>(at - game.base);
    for (int depth = 0; depth < 8; ++depth) {
        size_t lo = 0, hi = n;
        while (lo < hi) {  // the last entry that begins at or before rva
            const size_t mid = (lo + hi) / 2;
            if (table[mid].BeginAddress <= rva) lo = mid + 1; else hi = mid;
        }
        if (lo == 0) return nullptr;
        const IMAGE_RUNTIME_FUNCTION_ENTRY& e = table[lo - 1];
        if (rva >= e.EndAddress) return nullptr;
        const uint8_t* const info = game.base + (e.UnwindData & ~1u);
        if (((info[0] >> 3) & 0x4) == 0) return game.base + e.BeginAddress;  // not chained: the primary entry
        const uint8_t codes = info[2];
        const auto* chained = reinterpret_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(info + 4 + 2 * ((codes + 1) & ~1));
        rva = chained->BeginAddress;
    }
    return nullptr;
}

// PhysicsQueueShrinkOnce's call site, checked: the shrink's shape, and the call inside the function the catch-up tail-jumps to.
uint8_t* FindShrinkSite(const GameModule& game, uint8_t* ext, uint8_t** shrinkFn, const char** why)
{
    const auto sites = FindAll(game.text, ParsePattern(kShrinkSite));
    if (sites.size() != 1) { *why = "the queue removal's shrink call not found once"; return nullptr; }
    uint8_t* const call = sites[0] + kShrinkCall;
    uint8_t* const fn = RipTarget(call, 1, 5);
    if (!game.text.Contains(fn) || !MatchesAt(fn, ParsePattern(kShrinkFn))) { *why = "the shrink is not as expected"; return nullptr; }
    uint8_t* const owner = PrimaryFunction(game, call);
    bool tail = false;
    for (uint8_t* p = ext; owner && p < ext + kExtTailSearch && !tail; ++p)
        tail = p[0] == 0xE9 && RipTarget(p, 1, 5) == owner;
    if (!tail) { *why = "the call is not in the function the catch-up jumps to"; return nullptr; }
    *shrinkFn = fn;
    return call;
}

DWORD WINAPI ParHelper(void*)
{
    for (;;) {
        if (WaitForSingleObject(g_parGo, INFINITE) != WAIT_OBJECT_0) return 0;
        HelperThreadCache();  // the game allocator's per-thread cache (HelperThreadCaches), between jobs only
        const uint64_t t0 = Ticks();
        FiHook(g_parEvolution);
        g_parUs = TicksToUs(Ticks() - t0);
        SetEvent(g_parDone);
    }
}


bool PrepareDetour(uint8_t* fn, const uint8_t* prologue, size_t n, void* hook, uint8_t* slot)
{
    if (memcmp(fn, prologue, n) != 0) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + n;
    memcpy(slot, prologue, n);
    memcpy(slot + n, jmpAbs, sizeof jmpAbs);
    memcpy(slot + n + sizeof jmpAbs, &resume, sizeof resume);
    memcpy(slot + 32, jmpAbs, sizeof jmpAbs);
    memcpy(slot + 32 + sizeof jmpAbs, &hook, sizeof hook);
    return true;
}


bool PatchDetour(uint8_t* fn, size_t n, uint8_t* slot)
{
    const int64_t rel = (slot + 32) - (fn + 5);
    if (rel != static_cast<int32_t>(rel) || n < 5 || n > 8) return false;
    uint8_t jmp[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(jmp + 1, &rel32, sizeof rel32);
    return WriteCode(fn, std::span<const uint8_t>(jmp, n));  // start-up only (before the game's code runs)
}

bool Fail(const char* what)
{
    Log("[bulk] %s: BulkIndexInsert unavailable", what);
    return false;
}

uint32_t Disp32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
}  // namespace

void BulkBatchBegin(int ops)
{
    Batch* const b = ThisBatch();
    if (b->depth++ > 0) return;
    b->active = g_bulkOn.load(std::memory_order_relaxed) && ops >= g_minOps.load(std::memory_order_relaxed);
    b->trees = 0;
}

void BulkBatchEnd()
{
    Batch* const b = t_batch;
    if (!b || b->depth == 0 || --b->depth > 0) return;
    if (b->active && b->trees > 0) s_passes.fetch_add(1, std::memory_order_relaxed);
    if (b->active)
        for (int i = 0; i < b->trees; ++i) Finish(*b, b->t[i]);
    b->active = false;
    b->trees = 0;
}

// PhysicsFlushParallel's FlushAsync hook (physics_flush.cpp), on the physics step's thread
void BulkFlushAsyncBegin(uint8_t* evolution)
{
    t_parRunning = false;
    if (g_parOn.load(std::memory_order_relaxed) && g_parThread && g_fiCvar && *g_fiCvar == 1 &&
        *reinterpret_cast<const int32_t*>(evolution + g_fiNum) >= kParMinOps && TryAcquireSRWLockExclusive(&g_parLock)) {
        s_parOps.fetch_add(static_cast<uint64_t>(*reinterpret_cast<const int32_t*>(evolution + g_fiNum)), std::memory_order_relaxed);
        g_parEvolution = evolution;
        t_parRunning = true;
        t_parStart = Ticks();
        SetEvent(g_parGo);
    }
}

void BulkFlushAsyncEnd()
{
    if (!t_parRunning) return;
    const uint64_t t0 = Ticks();
    WaitForSingleObject(g_parDone, INFINITE);
    const uint64_t t1 = Ticks();
    t_parRunning = false;
    ReleaseSRWLockExclusive(&g_parLock);
    s_par.fetch_add(1, std::memory_order_relaxed);
    s_parUs.fetch_add(g_parUs, std::memory_order_relaxed);
    s_parWaitUs.fetch_add(TicksToUs(t1 - t0), std::memory_order_relaxed);
}


bool InstallBulkIndexInsert(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    const GameModule& game = Game();
    const auto swaps = FindAll(game.text, ParsePattern(kSwap));
    if (swaps.size() != 1) return Fail("the physics step's swap not found once");
    uint8_t* const fi = RipTarget(swaps[0] + kSwapInternal, 1, 5);
    const uint8_t* const cvar = RipTarget(swaps[0] + kSwapCvar, 2, 7);
    if (!game.text.Contains(fi) || !MatchesAt(fi, ParsePattern(kFiEntry))) return Fail("FlushInternalAccelerationQueue not as expected");
    if (!game.data.Contains(cvar)) return Fail("the swap's FlushInternal switch not found");
    PhysicsFlushInfoOut pf{};
    if (PhysicsFlushInfo(&pf) && pf.flush != RipTarget(swaps[0] + kSwapAsync, 1, 5)) return Fail("the swap's FlushAsync is not PhysicsFlushParallel's");
    const uint32_t queue = Disp32(fi + kFiQueue);
    if (queue < 0x100 || queue > 0x1000) return Fail("internal queue offset out of range");
    if (!InstallDirtyTreeQuickInsert(true)) return Fail("DirtyTreeQuickInsert's dirty-tree insert sites unavailable");
    g_parGo = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_parDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_parGo || !g_parDone) return Fail("no events");
    g_region = AllocNearGame(0x1000);
    if (!g_region) return Fail("no free memory near the game's code");
    if (!PrepareDetour(fi, kFiPrologue, sizeof kFiPrologue, reinterpret_cast<void*>(&FiHook), g_region))
        return Fail("FlushInternal's first instructions changed");
    // the game thread's catch-up of a new game-thread structure (optional: without it only the step's passes are batched)
    uint8_t* ext = nullptr;
    const auto exts = FindAll(game.text, ParsePattern(kExtEntry));
    if (exts.size() == 1 && PrepareDetour(exts[0], kExtPrologue, sizeof kExtPrologue, reinterpret_cast<void*>(&ExtHook), g_region + 128))
        ext = exts[0];
    // PhysicsQueueShrinkOnce: the catch-up's queue shrink (a stub at +0x200 jumps to ShrinkHook; the call is retargeted below)
    uint8_t* shrinkCall = nullptr;
    uint8_t* shrinkFn = nullptr;
    const char* shrinkWhy = "the game thread's catch-up not found";
    if (ext) shrinkCall = FindShrinkSite(game, ext, &shrinkFn, &shrinkWhy);
    if (shrinkCall) {
        const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
        void* const hook = reinterpret_cast<void*>(&ShrinkHook);
        memcpy(g_region + 0x200, jmpAbs, sizeof jmpAbs);
        memcpy(g_region + 0x200 + sizeof jmpAbs, &hook, sizeof hook);
    }
    DWORD old;
    VirtualProtect(g_region, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), g_region, 0x1000);
    g_fiOriginal = reinterpret_cast<FlushFn>(g_region);
    g_fiNum = queue + 8;
    g_fiCvar = reinterpret_cast<const volatile int32_t*>(cvar);
    g_fi = fi;
    if (!PatchDetour(fi, sizeof kFiPrologue, g_region)) return Fail("could not place the detour");
    g_parThread = CreateThread(nullptr, 0, ParHelper, nullptr, 0, nullptr);
    if (ext) {
        g_extOriginal = reinterpret_cast<ExtFn>(g_region + 128);
        if (!PatchDetour(ext, sizeof kExtPrologue, g_region + 128)) ext = nullptr;
    }
    if (ext && shrinkCall) {
        const int64_t rel = (g_region + 0x200) - (shrinkCall + 5);
        const int32_t rel32 = static_cast<int32_t>(rel);
        g_shrinkReal = reinterpret_cast<ShrinkFn>(shrinkFn);
        if (rel == rel32 && WriteCode(shrinkCall + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4))) {
            g_shrinkInstalled = true;
            g_shrinkSite = shrinkCall;
            Log("[bulk] PhysicsQueueShrinkOnce: the catch-up's queue shrink exe+0x%X (call at exe+0x%X) redirected",
                static_cast<unsigned>(shrinkFn - game.base), static_cast<unsigned>(shrinkCall - game.base));
        } else {
            Log("[bulk] PhysicsQueueShrinkOnce unavailable: could not redirect the shrink call");
        }
    } else {
        Log("[bulk] PhysicsQueueShrinkOnce unavailable: %s", ext ? shrinkWhy : "the game thread's catch-up not detoured");
    }
    SetDirtyInsertHook(&InsertHook);
    g_installed = true;
    Log("[bulk] installed: FlushInternalAccelerationQueue exe+0x%X (queue count +0x%X, swap switch exe+0x%X = %d), dirty-tree inserts "
        "through DirtyTreeQuickInsert's sites; the game thread's structure catch-up %s%s",
        static_cast<unsigned>(fi - game.base), g_fiNum, static_cast<unsigned>(cvar - game.base), *g_fiCvar,
        ext ? "batched too" : "not found (one by one)",
        g_parThread ? "" : "; no helper thread (31b off)");
    return true;
}

void SetBulkIndexInsert(bool bulk, int minOps, bool internalParallel)
{
    g_minOps = minOps < 1 ? 1 : minOps;
    if (!g_installed) return;
    if (g_bulkOn.exchange(bulk) != bulk)
        Log("[bulk] %s", bulk ? "on: a flush's dirty-tree inserts filed as one subtree, shaped at the flush's end"
                              : "off: dirty-tree inserts one by one, as the game");
    if (g_parOn.exchange(internalParallel) != internalParallel)
        Log("[bulk] %s", internalParallel ? "31b on: a swap step's FlushInternal beside its FlushAsync" : "31b off");
}

void SetPhysicsQueueShrinkOnce(bool on)
{
    if (!g_shrinkInstalled || g_shrinkOn.load() == on) return;
    g_shrinkOn = on;
    Log("[bulk] PhysicsQueueShrinkOnce %s", on ? "on: the game thread's catch-up shrinks its pending queue once, at the end"
                               : "off: the catch-up shrinks its queue after removals, as the game does");
}

void LogBulkIndexInsertStats()
{
    if (const uint64_t c = s_shrinkCatchUps.exchange(0)) {
        const uint64_t sk = s_shrinkSkipped.exchange(0), mx = s_shrinkMaxSkipped.exchange(0);
        Log("[bulk] PhysicsQueueShrinkOnce since last report: %llu catch-ups shrank their queue once, after %llu removals (most %llu in one)",
            c, sk, mx);
    }
    const uint64_t passes = s_passes.exchange(0), ins = s_inserts.exchange(0), rebuilt = s_rebuilt.exchange(0);
    const uint64_t broken = s_broken.exchange(0), fb = s_fallback.exchange(0), maxLeaves = s_maxLeaves.exchange(0);
    const uint64_t buildUs = s_buildUs.exchange(0), maxBuild = s_maxBuildUs.exchange(0);
    const uint64_t par = s_par.exchange(0), parOps = s_parOps.exchange(0), parUs = s_parUs.exchange(0), parWait = s_parWaitUs.exchange(0);
    if (passes || broken || fb)
        Log("[bulk] since last report: %llu batched passes, %llu inserts, %llu subtrees shaped (largest %llu leaves, %.1f ms in all, "
            "longest %.2f ms), %llu left to the game's path, %llu trees over the per-pass limit",
            passes, ins, rebuilt, maxLeaves, buildUs / 1000.0, maxBuild / 1000.0, broken, fb);
    if (par)
        Log("[bulk] 31b: %llu FlushInternal beside FlushAsync (%llu changes, %.1f ms on the helper, %.1f ms waited for it)", par, parOps,
            parUs / 1000.0, parWait / 1000.0);
}

BulkTestStats BulkIndexInsertTestStats()
{
    return BulkTestStats{s_passes.load(), s_inserts.load(), s_rebuilt.load(), s_broken.load(), s_fallback.load()};
}

void BulkIndexInsertTestSetup(bool bulk, int minOps)
{
    g_installed = true;
    SetDirtyInsertHook(&InsertHook);
    g_bulkOn = bulk;
    g_minOps = minOps;
}

void BulkParallelTestSetup(void* fakeFlushInternal, uint32_t numOffset, const int32_t* cvar, bool on)
{
    g_fiOriginal = reinterpret_cast<FlushFn>(fakeFlushInternal);
    g_fiNum = numOffset;
    g_fiCvar = cvar;
    if (!g_parGo) g_parGo = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_parDone) g_parDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_parThread) g_parThread = CreateThread(nullptr, 0, ParHelper, nullptr, 0, nullptr);
    g_parOn = on;
}

void BulkTestFlushInternal(uint8_t* evolution) { FiHook(evolution); }

ShrinkTestInfo BulkShrinkTestInfo()
{
    ShrinkTestInfo i{};
    i.installed = g_shrinkInstalled;
    i.site = g_shrinkSite;
    i.hook = reinterpret_cast<void*>(&ShrinkHook);
    i.real = reinterpret_cast<void*>(g_shrinkReal);
    return i;
}
void BulkShrinkTestSetup(void* fakeExtOriginal, void* fakeShrink, bool on)
{
    g_extOriginal = reinterpret_cast<ExtFn>(fakeExtOriginal);
    g_shrinkReal = reinterpret_cast<ShrinkFn>(fakeShrink);
    g_shrinkInstalled = true;
    g_shrinkOn = on;
}
void BulkTestCatchUp(uint8_t* evolution, void** structure, uint8_t* queue) { ExtHook(evolution, structure, queue); }

}  // namespace opf
