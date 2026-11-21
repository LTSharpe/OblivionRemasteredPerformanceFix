// FlowOverlapsOnce self-test (worker flowport 2026-11-16, S-007; review rev106c S1 / S3 cases), included by selftest.cpp:
// the discovery on the mapped exe (the level flow's adoption, OnComponentCollisionSettingsChanged's call of UpdateOverlaps,
// UpdateOverlaps' prologue, the overlap list offset), the install (the adoption's entry jumps, the call is redirected, the
// trampoline holds the entry's 6 original bytes and jumps back to +6), then the hook's logic with a fake adoption and a fake
// UpdateOverlaps that records its calls, on fake components (flags, bRegistered, overlap count), driven through the real
// patched entry and call stub: mode 0: every update in place; mode 1: a component without overlaps is updated once at the
// end of the adoption, in the order of its last request; one with overlaps is updated in place every time; a component that
// became invalid is skipped; a nested adoption flushes with the outer one; an adoption started by a notification during
// the end-of-adoption updates runs nested and does not cut the outer list short; past 512 components the rest update in
// place; an update outside an adoption is untouched.
#pragma once
namespace fov {
constexpr int kComps = 6, kMany = 513;
alignas(16) uint8_t g_comp[kComps][0x800] = {};
alignas(16) uint8_t g_many[kMany][0x400] = {};
void* g_log[64] = {};
int g_nLog = 0, g_updates = 0;
bool g_reenter = false;
uint32_t g_list = 0;
uint64_t FlowOverlapsTestAdoptVia(uint64_t a1);
uint64_t FakeUpdate(void* comp, const void*, uint64_t, const void*)
{
    if (g_nLog < 64) g_log[g_nLog++] = comp;
    ++g_updates;
    if (g_reenter && comp == g_comp[0]) {  // a notification receiver that starts an adoption (requests c2) mid-flush
        g_reenter = false;
        FlowOverlapsTestAdoptVia(4);
    }
    return 0;
}
void* C(int i) { return g_comp[i]; }
void SetOverlaps(uint8_t* c, int n) { *reinterpret_cast<int32_t*>(c + g_list + 8) = n; }
FlowOverlapsUpdateFn g_site = nullptr;  // the patched call's target (the stub to the hook)
FlowOverlapsAdoptFn g_entry = nullptr;  // the patched adoption entry
void Request(void* comp) { g_site(comp, nullptr, 1, nullptr); }
uint64_t FlowOverlapsTestAdoptVia(uint64_t a1) { return g_entry(a1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
uint64_t FakeAdopt(uint64_t a1, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    switch (a1) {
    case 1:  // c0 (no overlaps), c1 (overlaps), c0, c2 (no overlaps), c0 again
        Request(C(0));
        Request(C(1));
        Request(C(0));
        Request(C(2));
        Request(C(0));
        break;
    case 2:  // c3 requested, then marked pending-kill inside the adoption; c4 requested, then unregistered
        Request(C(3));
        Request(C(4));
        *reinterpret_cast<uint32_t*>(g_comp[3] + 0x08) = 0x20000000;
        g_comp[4][0x8C] = 0;
        break;
    case 3:  // nested adoption: c5 requested inside the inner call, c0 in the outer one afterwards
        Request(C(5));
        FlowOverlapsTestAdoptVia(4);
        Request(C(0));
        break;
    case 4:
        Request(C(2));
        break;
    case 5:  // c0, c2 deferred; the flush of c0 starts an adoption (FakeUpdate) that requests c2
        Request(C(0));
        Request(C(2));
        break;
    case 6:  // 513 components without overlaps
        for (int i = 0; i < kMany; ++i) Request(g_many[i]);
        break;
    }
    return 0x1234 + a1;
}
bool LogIs(std::initializer_list<int> want)
{
    if (g_nLog != static_cast<int>(want.size())) return false;
    int i = 0;
    for (const int w : want)
        if (g_log[i++] != C(w)) return false;
    return true;
}
}  // namespace fov

static void FlowOverlapsRun(const GameModule& game)
{
    using namespace fov;
    FlowOverlapsDiscovery d{};
    Check(FlowOverlapsDiscover(game, &d), "FlowOverlapsOnce: discovery on the mapped exe");
    Check(d.adopts == 1 && d.changes == 1 && d.begins == 1,
          "FlowOverlapsOnce: one adoption, one collision-settings change, one BeginComponentOverlap");
    if (game.timeDateStamp == 0xF19077A4u)
        Check(d.adopt - game.base == 0x472EFB0 && d.site - game.base == 0x358850F && d.update - game.base == 0x3092010 &&
                  d.overlapList == 0x3B8,
              "FlowOverlapsOnce: adoption exe+0x472EFB0, call exe+0x358850F -> UpdateOverlaps exe+0x3092010, list +0x3B8 (build 19115871)");
    if (!d.adopt) return;
    uint8_t entry[6];
    memcpy(entry, d.adopt, 6);
    Check(FlowOverlapsTestInstall(), "FlowOverlapsOnce: installs on the mapped exe");
    Check(d.adopt[0] == 0xE9 && d.adopt[5] == 0x90 && d.site[0] == 0xE8 && FlowOverlapsListOffset() == d.overlapList,
          "FlowOverlapsOnce: the adoption's entry jumps, the update call is redirected");
    const uint8_t* const stub = d.site + 5 + *reinterpret_cast<const int32_t*>(d.site + 1);
    MEMORY_BASIC_INFORMATION mi{};
    Check(VirtualQuery(stub, &mi, sizeof mi) && mi.Protect == PAGE_EXECUTE_READ, "FlowOverlapsOnce: the stubs' page is read-execute");
    const uint8_t* const tramp = FlowOverlapsTestTrampoline();
    Check(tramp && memcmp(tramp, entry, 6) == 0 && memcmp(entry, "\x48\x8B\xC4\x55\x41\x56", 6) == 0 && tramp[6] == 0xFF &&
              tramp[7] == 0x25 && *reinterpret_cast<const uint32_t*>(tramp + 8) == 0 &&
              *reinterpret_cast<const uint8_t* const*>(tramp + 12) == d.adopt + 6,
          "FlowOverlapsOnce: the trampoline = the entry's 6 original bytes, then jmp [rip] back to the adoption +6");

    g_list = d.overlapList;
    for (int i = 0; i < kComps; ++i) g_comp[i][0x8C] = 1;  // registered, valid, no overlaps
    for (int i = 0; i < kMany; ++i) g_many[i][0x8C] = 1;
    SetOverlaps(g_comp[1], 2);
    FlowOverlapsTestSetFakes(&FakeAdopt, &FakeUpdate);
    g_site = reinterpret_cast<FlowOverlapsUpdateFn>(const_cast<uint8_t*>(stub));  // real stub -> the hook
    g_entry = reinterpret_cast<FlowOverlapsAdoptFn>(d.adopt);                      // real entry jump -> stub -> the hook
    uint64_t def0, inp0, fl0, sk0, def1, inp1, fl1, sk1;

    // mode 0: every request runs in place, nothing at the end
    SetFlowOverlapsOnce(0);
    FlowOverlapsTestCounts(&def0, &inp0, &fl0, &sk0);
    g_nLog = 0;
    Check(FlowOverlapsTestAdoptVia(1) == 0x1235 && LogIs({0, 1, 0, 2, 0}),
          "FlowOverlapsOnce: off = every update in place, in order (through the patched entry and call)");
    FlowOverlapsTestCounts(&def1, &inp1, &fl1, &sk1);
    Check(def1 == def0 && fl1 == fl0, "FlowOverlapsOnce: off = nothing deferred");

    // mode 1: c1 (has overlaps) in place; c2 then c0 (order of last request) once each after the adoption
    SetFlowOverlapsOnce(1);
    g_nLog = 0;
    Check(FlowOverlapsTestAdoptVia(1) == 0x1235 && LogIs({1, 2, 0}),
          "FlowOverlapsOnce: on = the component with overlaps in place, the others once at the end in last-request order");
    FlowOverlapsTestCounts(&def0, &inp0, &fl0, &sk0);
    Check(def0 - def1 == 4 && inp0 - inp1 == 1 && fl0 - fl1 == 2, "FlowOverlapsOnce: 4 requests deferred, 1 in place, 2 run at the end");

    // invalid at the end: pending-kill and unregistered components are skipped
    g_nLog = 0;
    FlowOverlapsTestAdoptVia(2);
    FlowOverlapsTestCounts(&def1, &inp1, &fl1, &sk1);
    Check(g_nLog == 0 && sk1 - sk0 == 2, "FlowOverlapsOnce: components no longer valid / registered at the end are skipped");

    // nested adoption: one flush, by the outer call, in last-request order (c5, c2, c0)
    g_nLog = 0;
    Check(FlowOverlapsTestAdoptVia(3) == 0x1237 && LogIs({5, 2, 0}), "FlowOverlapsOnce: a nested adoption is flushed with the outer one");

    // an adoption started by a notification during the flush (rev106c S1): nested, its c2 in place, the outer list complete
    g_nLog = 0;
    g_reenter = true;
    Check(FlowOverlapsTestAdoptVia(5) == 0x1239 && LogIs({0, 2, 2}) && !g_reenter,
          "FlowOverlapsOnce: an adoption started during the end-of-adoption updates runs nested; the outer updates all run");

    // more than 512 components: 512 deferred, the 513th in place (first in the log), then the 512 at the end
    g_nLog = 0;
    g_updates = 0;
    FlowOverlapsTestCounts(&def0, &inp0, &fl0, &sk0);
    FlowOverlapsTestAdoptVia(6);
    FlowOverlapsTestCounts(&def1, &inp1, &fl1, &sk1);
    Check(g_updates == kMany && g_log[0] == g_many[kMany - 1] && g_log[1] == g_many[0] && def1 - def0 == 512 && fl1 - fl0 == 512,
          "FlowOverlapsOnce: past 512 components in one adoption the rest update in place; the 512 run at the end");

    // outside an adoption: untouched
    g_nLog = 0;
    Request(C(0));
    Check(LogIs({0}), "FlowOverlapsOnce: an update outside an adoption runs at once");
    SetFlowOverlapsOnce(0);
}
