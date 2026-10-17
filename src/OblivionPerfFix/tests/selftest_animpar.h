// AnimTeardownParallel (lab prototype) self-test, included by selftest.cpp: the shapes on the mapped exe (the
// sequence destructor's block array, the animation data's destruction call, the eight classes by RTTI and their
// destructors, the key-deleter table check), then the queue with fakes: many actors' block arrays destroyed by four
// helpers and the owner, each object exactly once and every array freed once with the game's size before the
// destruction call returns; arrays the owner must not queue (a controller left, a shared interpolator, another class)
// run inline, arrays with unexpected data classes are destroyed by the owner at the join; a nested destruction and the
// switch off run everything inline.
#pragma once

#include <atomic>
#include <random>
#include <vector>

namespace apar {

struct FakeObj {
    uintptr_t vt;
    std::atomic<int32_t> rc;
    int32_t pad;
    uint8_t pad2[0x10];
    FakeObj* d20;  // +0x20
    FakeObj* d28;  // +0x28
    uint8_t pad3[8];
    FakeObj* d38;  // +0x38
    std::atomic<int> destroyed;
};
static_assert(offsetof(FakeObj, rc) == 8 && offsetof(FakeObj, d20) == 0x20 && offsetof(FakeObj, d38) == 0x38);

uintptr_t vtab[9][2];  // 8 classes + "other"
uintptr_t Vt(int c) { return reinterpret_cast<uintptr_t>(&vtab[c][0]); }
int blockDtorTag, otherDtorTag;
std::atomic<int> doubleDestroy{0}, doubleFree{0}, badFreeSize{0}, passThrough{0};

void Release(FakeObj* o)
{
    if (!o) return;
    if (o->rc.fetch_sub(1) != 1) return;
    if (o->destroyed.fetch_add(1) != 0) ++doubleDestroy;
    // an interpolator releases its data / basis (as the four destructors do)
    Release(o->d20);
    Release(o->d28);
    Release(o->d38);
}

struct Arr {
    std::vector<uint8_t> mem;  // count at +0, blocks at +8
    uint64_t n = 0;
    std::atomic<int> freed{0};
    int kind = 0;  // 0 eligible, 1 inline (controller), 2 inline (shared), 3 inline (other class), 4 fallback (data class)
    uint8_t* blocks() { return mem.data() + 8; }
};

std::vector<Arr*> g_all;  // every array of the current run (fake free looks the base up)

void FakeIter(void* arr, uint64_t size, uint64_t count, void* dtor)
{
    if (dtor != &blockDtorTag) {
        ++passThrough;
        return;
    }
    auto* a = static_cast<uint8_t*>(arr);
    for (uint64_t j = count; j-- > 0;) {  // as the iterator: last to first
        Release(*reinterpret_cast<FakeObj**>(a + j * 0x20 + 8));
        Release(*reinterpret_cast<FakeObj**>(a + j * 0x20));
    }
    (void)size;
}

void FakeFree(void* p, uint64_t size)
{
    for (Arr* a : g_all)
        if (a->mem.data() == p) {
            if (a->freed.fetch_add(1) != 0) ++doubleFree;
            if (size != a->n * 0x20 + 8) ++badFreeSize;
            return;
        }
}

struct Actor {
    std::vector<Arr*> arrays;
    Actor* nested = nullptr;
    bool nestedChecked = false, nestedOk = false;
};

bool AllFreed(const Actor& a)
{
    for (Arr* x : a.arrays)
        if (x->freed.load() != 1) return false;
    return true;
}

uint64_t FakeAnimData(uint64_t adArg, uint64_t, uint64_t, uint64_t)
{
    auto* ad = reinterpret_cast<Actor*>(adArg);
    for (size_t i = 0; i < ad->arrays.size(); ++i) {
        Arr* x = ad->arrays[i];
        AnimTeardownParallelTestIter(x->blocks(), 0x20, x->n, &blockDtorTag);
        AnimTeardownParallelTestFree(x->mem.data(), x->n * 0x20 + 8);
        if (i == 0) AnimTeardownParallelTestIter(nullptr, 0x20, 1, &otherDtorTag);  // another iterator use passes through
        if (i == ad->arrays.size() / 2 && ad->nested) {
            AnimTeardownParallelTestAnimData(reinterpret_cast<uint64_t>(ad->nested));
            ad->nestedChecked = true;
            ad->nestedOk = AllFreed(*ad->nested);  // a nested destruction ran inline: all done when it returns
        }
    }
    return 7;
}

std::vector<FakeObj*> g_objs;
FakeObj* NewObj(uintptr_t vt, int rc)
{
    auto* o = new FakeObj{};
    o->vt = vt;
    o->rc = rc;
    g_objs.push_back(o);
    return o;
}

Arr* MakeArray(std::mt19937& rng, int kind)
{
    auto* a = new Arr;
    a->n = 1 + rng() % 160;
    a->kind = kind;
    a->mem.assign(8 + a->n * 0x20, 0);
    memcpy(a->mem.data(), &a->n, 8);
    // a data object shared by several interpolators of this array and held by "the KF cache" too (released later)
    FakeObj* sharedTd = NewObj(Vt(4), 1);
    for (uint64_t j = 0; j < a->n; ++j) {
        uint8_t* b = a->blocks() + j * 0x20;
        if (rng() % 9 == 0) continue;  // empty block
        if (j == 0 && kind >= 2) continue;  // block 0 gets the kind's object below
        const int c = rng() % 4;       // cTI, cBCTI, cFI, cBCFI
        FakeObj* o = NewObj(Vt(c), 1);
        if (c == 0) {
            if (rng() % 3 == 0) { o->d38 = sharedTd; sharedTd->rc.fetch_add(1); }
            else o->d38 = NewObj(Vt(4), 1);
        } else if (c == 2) {
            o->d20 = NewObj(Vt(5), 1);
        } else {
            o->d20 = NewObj(Vt(6), 1);
            o->d28 = NewObj(Vt(7), 1);
        }
        *reinterpret_cast<FakeObj**>(b) = o;
    }
    // the kinds the fix must not hand to a helper (one block changed)
    uint8_t* b0 = a->blocks();
    if (kind == 1) *reinterpret_cast<FakeObj**>(b0 + 8) = NewObj(Vt(8), 1);
    if (kind == 2 || kind == 3 || kind == 4) {
        FakeObj* o = NewObj(kind == 3 ? Vt(8) : Vt(0), kind == 2 ? 2 : 1);
        o->d38 = NewObj(kind == 4 ? Vt(5) : Vt(4), 1);  // kind 4: a transform interpolator with float data
        *reinterpret_cast<FakeObj**>(b0) = o;
    }
    return a;
}

static void Run(const GameModule& game)
{
    // ---- the mapped exe ----
    Check(FindAnimTeardownParallel(), "anim teardown: sequence block array, destruction call, 9 classes by RTTI found and verified");
    {
        // key-deleter tables: the empty function + a helper of the game = as expected; the block release = not
        uint8_t* table[18] = {};
        table[0] = game.base + 0xD4A910;
        table[1] = game.base + 0x658E4F0;
        table[5] = game.base + 0x6593540;
        Check(AnimTeardownParallelTestKeyTables(reinterpret_cast<uint8_t*>(table)),
              "anim teardown: key-deleter tables with the empty function and the game's helpers accepted");
        table[2] = game.base + 0x67D6FF0;
        Check(!AnimTeardownParallelTestKeyTables(reinterpret_cast<uint8_t*>(table)),
              "anim teardown: a key-deleter slot pointing elsewhere refused");
    }
    // ---- the queue with fakes ----
    AnimTeardownTest t{};
    t.iterator = reinterpret_cast<void*>(&FakeIter);
    t.free = reinterpret_cast<void*>(&FakeFree);
    t.blockDtor = &blockDtorTag;
    t.animData = reinterpret_cast<void*>(&FakeAnimData);
    for (int c = 0; c < 8; ++c) t.vt[c] = Vt(c);
    Check(AnimTeardownParallelTestSetup(t, 4), "anim teardown: test set-up with 4 helpers");
    std::mt19937 rng(49);
    int expectInline = 0, expectFallback = 0, expectQueued = 0, wrongResult = 0, nestedBad = 0, notDone = 0;
    std::vector<Actor*> actors;
    for (int k = 0; k < 400; ++k) {
        auto* a = new Actor;
        const int na = 1 + rng() % 40;
        for (int i = 0; i < na; ++i) {
            const int r = rng() % 50;
            const int kind = r == 0 ? 1 : r == 1 ? 2 : r == 2 ? 3 : r == 3 ? 4 : 0;
            Arr* x = MakeArray(rng, kind);
            a->arrays.push_back(x);
            g_all.push_back(x);
            if (kind >= 1 && kind <= 3) ++expectInline;
            else ++expectQueued;
            if (kind == 4) ++expectFallback;
        }
        if (k % 50 == 7) {  // a nested destruction (runs inline)
            a->nested = new Actor;
            for (int i = 0; i < 5; ++i) {
                Arr* x = MakeArray(rng, 0);
                a->nested->arrays.push_back(x);
                g_all.push_back(x);
                // not queued and not counted: a nested destruction runs as the game does it
            }
        }
        actors.push_back(a);
    }
    for (Actor* a : actors) {
        if (AnimTeardownParallelTestAnimData(reinterpret_cast<uint64_t>(a)) != 7) ++wrongResult;
        if (!AllFreed(*a)) ++notDone;  // every array destroyed and freed when the call returns
        if (a->nested && (!a->nestedChecked || !a->nestedOk)) ++nestedBad;
    }
    const AnimTeardownCounts n = AnimTeardownParallelTestCounts();
    Check(wrongResult == 0, "anim teardown: the destruction's return value passed through");
    Check(notDone == 0, "anim teardown: every array of an actor freed before its destruction call returns");
    Check(doubleDestroy.load() == 0 && doubleFree.load() == 0 && badFreeSize.load() == 0,
          "anim teardown: each object destroyed once, each array freed once with the game's size");
    int leftAlive = 0;
    for (FakeObj* o : g_objs)
        if (o->destroyed.load() == 0 && o->rc.load() != 1) ++leftAlive;  // rc 1 left: shared data / the shared interpolators
    Check(leftAlive == 0, "anim teardown: no object left with a wrong count");
    Check(nestedBad == 0, "anim teardown: a nested destruction runs inline");
    Check(n.actors == 400 && n.arrays == static_cast<uint64_t>(expectQueued) && n.inlined == static_cast<uint64_t>(expectInline),
          "anim teardown: eligible arrays queued, controller / shared / other-class arrays inline");
    Check(n.fallback == static_cast<uint64_t>(expectFallback), "anim teardown: arrays with unexpected data classes destroyed by the owner");
    Check(n.byHelper > 0 && n.byHelper + n.byOwner + n.fallback == n.arrays, "anim teardown: helpers took part, every queued array done once");
    Check(passThrough.load() == 408, "anim teardown: other iterator uses pass through");  // 400 actors + 8 nested
    // switched off: nothing queued
    SetAnimTeardownParallel(0);
    auto* off = new Actor;
    for (int i = 0; i < 10; ++i) {
        Arr* x = MakeArray(rng, 0);
        off->arrays.push_back(x);
        g_all.push_back(x);
    }
    AnimTeardownParallelTestAnimData(reinterpret_cast<uint64_t>(off));
    const AnimTeardownCounts n2 = AnimTeardownParallelTestCounts();
    Check(n2.arrays == n.arrays && n2.actors == n.actors && AllFreed(*off), "anim teardown: switched off, everything as the game does it");
}

}  // namespace apar
