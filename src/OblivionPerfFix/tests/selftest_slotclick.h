// SlotClickPayloadCopy self-test (S-085), included by selftest.cpp: the sites on the mapped exe (the copy constructor
// chosen by the click handler's cross-check, the other copy left alone; the three moves, the empty capture, the no-op and
// the copies after it left to the game), then the hooks on a model of the click's chain with a fake copy constructor in
// modes 0 and 1 (the bytes the command's member gets, which copies are made, what is left to free). Lab: the probe's fill
// stubs against the CRT in modes 0 / 2 / 4 on many sizes, alignments and an overlap, with fresh-memory timings.
#pragma once

namespace sclick {

static int32_t Rel(const uint8_t* call) { int32_t r; memcpy(&r, call + 1, 4); return r; }
static const uint8_t* Target(const uint8_t* call) { return call + 5 + Rel(call); }

struct V { uint8_t *first, *last, *end; };
static int g_fakeCopies = 0;
static size_t g_fakeBytes = 0;
static void* FakeCtor(void* dstP, void* srcP)  // the game's copy constructor: a fresh exact-size buffer (none for size 0)
{
    V* const d = static_cast<V*>(dstP);
    const V* const s = static_cast<const V*>(srcP);
    const size_t n = static_cast<size_t>(s->last - s->first);
    *d = V{nullptr, nullptr, nullptr};
    ++g_fakeCopies;
    g_fakeBytes += n;
    if (n) {
        d->first = new uint8_t[n];
        memcpy(d->first, s->first, n);
        d->last = d->end = d->first + n;
    }
    return d;
}
static int g_frees = 0;
static void Free(V& v)  // the game's destructor: frees a non-null buffer, leaves the vector empty
{
    if (v.first) { delete[] v.first; ++g_frees; }
    v = V{nullptr, nullptr, nullptr};
}

// The click's chain as the game runs it: P0 -c1-> T1 -c2-> T2 -c3-> member; member -c4-> L1 -c5-> L2 -c6-> L3 -c7-> A
// -c8-> B (inside the no-op); then every local destroyed (B, A, L3, L2, L1, T2, T1, P0), the member kept.
static bool Chain(size_t n, int mode, int* copies, size_t* copied, int* frees, bool* sameBytes, bool* capOk)
{
    SetSlotClickPayloadCopy(mode);
    std::vector<uint8_t> src(n);
    for (size_t i = 0; i < n; ++i) src[i] = static_cast<uint8_t>(i * 131 + 7);
    V p0{nullptr, nullptr, nullptr};
    if (n) { p0.first = new uint8_t[n]; memcpy(p0.first, src.data(), n); p0.last = p0.end = p0.first + n; }
    V t1{}, t2{}, member{}, l1{}, l2{}, l3{}, a{}, b{};
    g_fakeCopies = 0;
    g_fakeBytes = 0;
    g_frees = 0;
    bool ret = SlotClickMoveHook(&t1, &p0) == &t1;
    ret = ret && SlotClickMoveHook(&t2, &t1) == &t2;
    ret = ret && SlotClickMoveHook(&member, &t2) == &member;
    ret = ret && SlotClickEmptyHook(&l1, &member) == &l1;
    FakeCtor(&l2, &l1);
    FakeCtor(&l3, &l2);
    FakeCtor(&a, &l3);
    FakeCtor(&b, &a);
    for (V* v : {&b, &a, &l3, &l2, &l1, &t2, &t1, &p0}) Free(*v);
    *sameBytes = static_cast<size_t>(member.last - member.first) == n && (n == 0 || memcmp(member.first, src.data(), n) == 0);
    *capOk = member.end == member.last;
    *copies = g_fakeCopies;
    *copied = g_fakeBytes;
    *frees = g_frees;
    Free(member);
    return ret;
}

static void Run(const GameModule& game)
{
    Check(InstallSlotClickPayloadCopy(true), "slot click copy: handler, bridge, command builder and no-op found and patched");
    SlotClickCopySites s{};
    if (!Installed(SlotClickPayloadCopyInfo(&s), "SlotClickPayloadCopy (slot click copy)")) return;
    const uint8_t* const b = game.base;
    Check(s.copyCtor == b + 0x48A7100, "std::vector<uint8_t> copy constructor at exe+0x48A7100 (the one the click handler calls)");
    Check(s.otherCopyCtor == b + 0x66EA500, "its identical twin at exe+0x66EA500 found and left alone");
    Check(b[0x66EA59F] == 0xE8 && Target(b + 0x66EA59F) == b + 0x6BFC45E, "the twin's memmove (exe+0x66EA59F) still calls the game's memmove thunk");
    Check(s.handler == b + 0x4999A40, "the click handler at exe+0x4999A40");
    {
        const uint8_t* const lea = b + 0x4999C5C;  // lea r9,"Send_On_Item_Clicked_Handler"
        int32_t d;
        memcpy(&d, lea + 3, 4);
        Check(lea[0] == 0x4C && lea[1] == 0x8D && strcmp(reinterpret_cast<const char*>(lea + 7 + d), "Send_On_Item_Clicked_Handler") == 0,
              "the handler's event block names Send_On_Item_Clicked_Handler");
    }
    const uint32_t moves[3] = {0x4999C3B, 0x4995E8F, 0x4995FF4};
    bool mv = true;
    for (int i = 0; i < 3; ++i) mv = mv && s.moveSites[i] == b + moves[i] && b[moves[i]] == 0xE8 && Target(b + moves[i]) == s.moveStub;
    Check(mv, "the handler's (exe+0x4999C3B), the bridge's (exe+0x4995E8F) and the command builder's (exe+0x4995FF4) copies call the move stub");
    Check(s.emptySite == b + 0x4996028 && Target(b + 0x4996028) == s.emptyStub, "the capture chain's first copy (exe+0x4996028) calls the empty stub");
    Check(s.noop == b + 0x49956D0, "the no-op at exe+0x49956D0");
    {
        const uint32_t left[4] = {0x4996063, 0x499609D, 0x49960C3, 0x49956EB};
        bool all = true;
        for (uint32_t r : left) all = all && b[r] == 0xE8 && Target(b + r) == s.copyCtor;
        Check(all, "the four copies after it (exe+0x4996063, 0x499609D, 0x49960C3, 0x49956EB) still call the game's constructor");
        void* to = nullptr;
        memcpy(&to, s.moveStub + 6, 8);
        void* to2 = nullptr;
        memcpy(&to2, s.emptyStub + 6, 8);
        Check(s.moveStub[0] == 0xFF && s.moveStub[1] == 0x25 && to == reinterpret_cast<void*>(&SlotClickMoveHook) &&
                  s.emptyStub[0] == 0xFF && to2 == reinterpret_cast<void*>(&SlotClickEmptyHook),
              "the stubs jump to the move and empty hooks");
    }
    Check(Target(b + 0x48A719F) == b + 0x6BFC45E, "public build: the constructor's own memmove is untouched");
    Check(InstallSlotClickPayloadCopy(true), "second application: already installed");

    // the chain model with a fake constructor
    SlotClickTestSetCtor(reinterpret_cast<void*>(&FakeCtor));
    for (size_t n : {size_t{0}, size_t{1}, size_t{4095}, size_t{4048} << 10, size_t{5856} << 10}) {
        int copies = 0, frees = 0;
        size_t copied = 0;
        bool same = false, cap = false;
        bool ret = Chain(n, 0, &copies, &copied, &frees, &same, &cap);
        char msg[200];
        snprintf(msg, sizeof msg, "%zu bytes, mode 0: the game's 8 copies (%d made, %zu bytes), the member has the bytes, %d frees", n, copies, copied, frees);
        Check(ret && same && cap && copies == 8 && copied == 8 * n && frees == (n ? 8 : 0), msg);
        ret = Chain(n, 1, &copies, &copied, &frees, &same, &cap);
        snprintf(msg, sizeof msg, "%zu bytes, mode 1: no copy of the bytes (%d size-0 copies), the member has the same bytes and capacity, nothing left to free (%d)",
                 n, copies, frees);
        Check(ret && same && cap && copies == 4 && copied == 0 && frees == 0, msg);
    }
    SetSlotClickPayloadCopy(0);
    SlotClickTestSetCtor(s.copyCtor);  // the game's constructor again

    // fail closed: one changed byte in the handler, the bridge's prologue, the capture chain or the no-op -> not found
    {
        Check(SlotClickTestFind(), "the finder holds on the unchanged exe");
        struct Flip { uint32_t rva; const char* what; };
        {
            // a seventh rsp-relative access to P0 inside the handler's body (bytes 44 24 58 written over 3 bytes at +0x100)
            uint8_t* const at = const_cast<uint8_t*>(b) + 0x4999B40;
            DWORD old;
            VirtualProtect(at, 3, PAGE_EXECUTE_READWRITE, &old);
            uint8_t keep[3];
            memcpy(keep, at, 3);
            const uint8_t inj[3] = {0x44, 0x24, 0x58};
            memcpy(at, inj, 3);
            const bool found = SlotClickTestFind();
            memcpy(at, keep, 3);
            VirtualProtect(at, 3, old, &old);
            Check(!found, "fail closed: a seventh access to the handler's payload vector -> not installed");
        }
        const Flip flips[] = {{0x4999A62, "the handler's entry"},
                              {0x4995E1D, "the bridge's prologue load of T1"},
                              {0x4995FBB, "the builder's prologue load of T2"},
                              {0x4996040, "the capture chain"},
                              {0x4995760, "the no-op's body"}};
        for (const Flip& fl : flips) {
            uint8_t* const at = const_cast<uint8_t*>(b) + fl.rva;
            DWORD old;
            VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old);
            const uint8_t keep = *at;
            *at = static_cast<uint8_t>(keep ^ 0x5A);
            const bool found = SlotClickTestFind();
            *at = keep;
            VirtualProtect(at, 1, old, &old);
            char msg[200];
            snprintf(msg, sizeof msg, "fail closed: one changed byte in %s -> not installed", fl.what);
            Check(!found, msg);
        }
        Check(SlotClickTestFind(), "the finder holds again after the bytes are back");
    }

    LogSlotClickPayloadCopyStats();
}

}  // namespace sclick
