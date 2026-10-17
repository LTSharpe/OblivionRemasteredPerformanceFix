// PhysicsQueueShrinkOnce self-test, included by selftest.cpp: the call site on the mapped exe, then a model of
// the game thread's catch-up (FlushExternalAccelerationQueue's backwards walk with swap-removals) run with the game's
// shrink after every removal and through the fix (shrink once at the end), on random queues.
#pragma once
#include <random>

namespace shk {
struct Queue { uint8_t* data; int32_t num; int32_t max; };  // TArray layout: data +0, Num +8, Max +0xC
static_assert(offsetof(Queue, num) == 8 && offsetof(Queue, max) == 12, "queue layout");
constexpr size_t kElem = 0x48;
int g_reallocs = 0;
// The game's TArray<FPendingSpatialData>::ResizeShrink (exe+0x15F2100), quantisation left out.
uint64_t FakeShrink(uint8_t* q8)
{
    Queue& q = *reinterpret_cast<Queue*>(q8);
    const int32_t slack = q.max - q.num;
    int32_t want = q.max;
    if ((static_cast<uint64_t>(slack) * kElem > 0x3FFF || 3 * q.num < 2 * q.max) && (slack > 64 || q.num == 0)) want = q.num;
    if (want != q.max) {
        uint8_t* const fresh = want ? static_cast<uint8_t*>(malloc(want * kElem)) : nullptr;
        if (q.num) memcpy(fresh, q.data, q.num * kElem);
        free(q.data);
        q.data = fresh;
        q.max = want;
        ++g_reallocs;
    }
    return 0;
}
using SiteFn = uint64_t (*)(uint8_t*);
SiteFn g_site = &FakeShrink;  // what the removal's call reaches (the game's shrink, or PhysicsQueueShrinkOnce's hook)
int32_t g_stamp = 0;          // the structure's sync timestamp
std::vector<uint64_t> g_trace;  // applied ops and the queue's state after every removal
uint64_t Hash(const Queue& q)
{
    return HashWords(1469598103934665603ull ^ static_cast<uint64_t>(q.num), q.data, static_cast<size_t>(q.num) * kElem);
}
// The catch-up's walk: from the last op down; an op the step already consumed (timestamp below the structure's) is
// swap-removed (last entry moved into its place, Num - 1, then the shrink call); the others are applied.
void FakeCatchUp(uint8_t*, void**, uint8_t* q8)
{
    Queue& q = *reinterpret_cast<Queue*>(q8);
    for (int32_t i = q.num - 1; i >= 0; --i) {
        uint8_t* const op = q.data + i * kElem;  // re-read each time, as the game does
        const int32_t stamp = *reinterpret_cast<int32_t*>(op + 0x3C);
        if (stamp < g_stamp) {
            if (i + 1 < q.num) memcpy(q.data + i * kElem, q.data + (q.num - 1) * kElem, kElem);
            --q.num;
            g_site(q8);
            g_trace.push_back(Hash(q));
        } else {
            g_trace.push_back(*reinterpret_cast<uint64_t*>(op));  // "applied": the op's id
        }
    }
}
Queue Make(std::mt19937& rng, int n, int extra)
{
    Queue q{static_cast<uint8_t*>(malloc((n + extra) * kElem + 1)), n, n + extra};
    for (int i = 0; i < n; ++i) {
        uint8_t* const op = q.data + i * kElem;
        for (size_t b = 0; b < kElem; b += 4) *reinterpret_cast<uint32_t*>(op + b) = rng();
        *reinterpret_cast<uint64_t*>(op) = 1000000ull + i;
        *reinterpret_cast<int32_t*>(op + 0x3C) = static_cast<int32_t>(rng() % 100);
    }
    return q;
}
}  // namespace shk

inline void ShrinkSelfTest(const GameModule& game)
{
    const ShrinkTestInfo info = BulkShrinkTestInfo();
    Check(info.installed && info.site == game.base + 0x19320CD && info.real == game.base + 0x15F2100,
          "PhysicsQueueShrinkOnce: the catch-up's queue shrink exe+0x15F2100, its call at exe+0x19320CD (in the function the catch-up jumps to)");
    std::mt19937 rng(38);
    bool same = true, once = true, rule = true, offStock = true;
    int cases = 0;
    for (int t = 0; t < 300; ++t) {
        const int n = t < 3 ? t : static_cast<int>(rng() % (t < 150 ? 600 : 12000));
        const int extra = static_cast<int>(rng() % 300);
        shk::g_stamp = static_cast<int32_t>(rng() % 101);  // 0: nothing consumed .. 100: everything consumed
        const uint32_t seed = rng();
        // stock: the game's shrink after every removal
        std::mt19937 r1(seed);
        shk::Queue a = shk::Make(r1, n, extra);
        shk::g_site = &shk::FakeShrink;
        shk::g_trace.clear();
        shk::g_reallocs = 0;
        shk::FakeCatchUp(nullptr, nullptr, reinterpret_cast<uint8_t*>(&a));
        const std::vector<uint64_t> ta = shk::g_trace;
        const int reA = shk::g_reallocs;
        // PhysicsQueueShrinkOnce on: through the catch-up's detour, the removal's call reaching the hook
        std::mt19937 r2(seed);
        shk::Queue b = shk::Make(r2, n, extra);
        BulkShrinkTestSetup(reinterpret_cast<void*>(&shk::FakeCatchUp), reinterpret_cast<void*>(&shk::FakeShrink), true);
        shk::g_site = reinterpret_cast<shk::SiteFn>(info.hook);
        shk::g_trace.clear();
        shk::g_reallocs = 0;
        BulkTestCatchUp(nullptr, nullptr, reinterpret_cast<uint8_t*>(&b));
        const int reB = shk::g_reallocs;
        // the hook gives the trace's removal states before the copy only by contents: compare contents, not addresses
        same = same && ta == shk::g_trace && a.num == b.num && shk::Hash(a) == shk::Hash(b);
        once = once && reB <= 1;
        const int32_t maxB = b.max;
        shk::FakeShrink(reinterpret_cast<uint8_t*>(&b));
        rule = rule && (b.num == n || b.max == maxB);  // after any removal the game's rule holds for the final count
        // PhysicsQueueShrinkOnce off: the hook passes every call on
        std::mt19937 r3(seed);
        shk::Queue c = shk::Make(r3, n, extra);
        BulkShrinkTestSetup(reinterpret_cast<void*>(&shk::FakeCatchUp), reinterpret_cast<void*>(&shk::FakeShrink), false);
        shk::g_trace.clear();
        shk::g_reallocs = 0;
        BulkTestCatchUp(nullptr, nullptr, reinterpret_cast<uint8_t*>(&c));
        offStock = offStock && shk::g_trace == ta && shk::g_reallocs == reA && c.max == a.max && c.num == a.num;
        // a shrink call from elsewhere (another queue) on this thread is never skipped
        free(a.data); free(b.data); free(c.data);
        ++cases;
    }
    {
        std::mt19937 r4(7);
        shk::Queue other = shk::Make(r4, 10, 500);
        BulkShrinkTestSetup(reinterpret_cast<void*>(&shk::FakeCatchUp), reinterpret_cast<void*>(&shk::FakeShrink), true);
        shk::g_reallocs = 0;
        reinterpret_cast<shk::SiteFn>(info.hook)(reinterpret_cast<uint8_t*>(&other));
        Check(shk::g_reallocs == 1 && other.max == 10, "PhysicsQueueShrinkOnce: a shrink outside the catch-up is the game's");
        free(other.data);
    }
    Check(same, "PhysicsQueueShrinkOnce: 300 random catch-ups: the same ops applied, the same queue contents after every removal and at the end");
    Check(once, "PhysicsQueueShrinkOnce: at most one reallocation per catch-up (the shrink at the end)");
    Check(rule, "PhysicsQueueShrinkOnce: after the catch-up the game's shrink rule holds for the final count");
    Check(offStock, "PhysicsQueueShrinkOnce off: every removal's shrink is the game's (same reallocations and capacity)");
    BulkShrinkTestSetup(nullptr, info.real, false);
}
