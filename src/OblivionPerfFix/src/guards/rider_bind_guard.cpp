// RiderBindGuard (worker crash1121r 2026-11-22, S-073; review revs073 applied by fixs073b; default 0 until the release
// decides): a horse's animation no longer writes into the animation data of a rider whose animation data is gone (game bug:
// crash on the OblivionThread while riding).
//
// The game bug (findings 2026-11-22 crash1121r; dump of a play114 crash 2026-11-21 21:28 near Bruma): horse .kf
// files carry the rider's tracks (Bip01 *, Weapon, Torch, SideWeapon). NiControllerSequence::StoreTargets (exe+0x67DB9C0 in
// build 19115871) ends with a Bethesda step: it looks up "Bip01" in the horse's object palette, takes the
// NiMultiTargetTransformController of that bone's parent (the rider's skeleton) and stores, for every track whose bone that
// controller drives, a RAW pointer to the rider controller's blend interpolator (array at +0x50, 0x2028 bytes each) in the
// horse sequence's item (+0x10). No reference is held, and nothing clears these pointers when the rider's array goes: when
// the rider's controller is destroyed (its 3D removed, or its own animations re-stored: the re-store takes the controller
// off the skeleton and builds a new one) or reset (exe+0x6853790 frees the array and allocates a new one, run by an
// animation-data reset exe+0x67F47C0 through the scene-graph walk exe+0x67F0C90). The horse's sequences then point into
// freed memory: an active one (an idle) writes blend weights there every frame (NiControllerSequence::Update exe+0x67DCA50)
// and crashes once the pages are released; an inactive one writes into it when it starts or stops, by then possibly another
// NPC's live animation data (the dump had both: Rest.kf into a freed array, 20 sequences into an address now owned by
// another rider).
//
// The fix: just before the game frees a NiMultiTargetTransformController's blend array (its vtable's deleting destructor,
// slot 0, and the one direct call of its reset), every horse sequence item whose blend pointer lies inside that array gets
// null, on the OblivionThread only. Null is the game's own "not bound" state of an item: Update, Activate and Deactivate
// skip it (they test the pointer), and StoreTargets' first pass binds it again the next time the game binds the horse's
// animations (at the latest at the next mount: exe+0x65C24B0 -> 0x65C4EC0 -> 0x67F4360). Which items: the sequences that
// came out of a StoreTargets call with a blend pointer outside their own controller's array (StoreTargets' direct calls are
// wrapped: after the game's call the sequence is noted or dropped); a noted sequence is dropped in its destructor
// (BSAnimGroupSequence's vtable slot 0) before the game destroys it. The decision is taken at the moment the array dies, so
// a later reuse of the address cannot be mistaken for a live binding.
//
// Stock's branch choice in StoreTargets is kept (review S1): its item loop takes a Gamebryo resolve branch for a null item
// (exe+0x67DBB51) that, when the bone has no controller of its own, calls item+8's SetTarget (vt+0xB0) with the bone; item+8
// of a rider item is the horse's controller, so it would move the horse's controller onto a rider bone. Stock never takes
// it for these items (their pointer is stale, not null), and the first pass does not always rebind them: it gives a bone a
// slot of the horse controller's target list (+0x58) only if the bone is there already or a slot is free, and nothing
// clears the old rider's slots (the list's only writers are the first pass, the reset above, which zeroes them, and the
// detach of an attached subtree exe+0x6893A10; the full re-store 0x67F4360 takes the controller off and builds a new one,
// but AddSequence 0x68517A0 and the manager-wide stores 0x6852250 / 0x68522B0 reuse it). So every item the guard nulled is
// remembered, holds a static sentinel during each later StoreTargets call (non-null, like stock's stale pointer; the call
// only tests the item for null, the first pass and the rider step overwrite it) and is null again afterwards unless the
// game bound it. The sentinel's interp-info pointer (+0x2000) points at a scratch block, so even an Update racing a
// StoreTargets on another thread (a race of the game's own) only writes into the scratch.
//
// Off the OblivionThread (review S2) the items are left as stock leaves them and counted: Update re-reads +0x10 after its
// null test (0x67DCD9E, 0x67DCE15), so a clearing on another thread while the OT updates the horse could turn a write into
// freed memory into a null dereference. In the game the rider's controller is destroyed and reset on the OT.
//
// Not covered: the horse controller's target list (+0x58) keeps raw pointers to the rider's bones; the guard leaves it as the
// game has it (the game rebuilds the controller at a full re-store and reuses free slots; changing live targets would be a
// behaviour change), so a stale target is stock behaviour.
//
// Found by: RTTI (".?AVNiMultiTargetTransformController@@", ".?AVBSAnimGroupSequence@@") for the two vtables, the shapes of
// both slot-0 destructors (the controller's body frees the array at +0x50 with its count at [array-8] and the targets at
// +0x58), one pattern over StoreTargets' rider step (it gives every offset used here), StoreTargets as the .pdata function
// holding it, the reset's own pattern, and their direct calls (E8) within 8 MB, each accepted only as an instruction start
// (inside a .pdata function, right after a complete `mov rcx|rdx, ...` loading the call's argument; a StoreTargets call
// that is not: the key stays off). Anything not found or not as expected: the key stays off with a log line (a missing reset
// only leaves that path as stock, with a log line). Start-up only; `RiderBindGuard=0` in the ini later only stops the nulling.
#include "common.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace opf {
namespace {

// StoreTargets' rider step (build 19115871: exe+0x67DC224, inside exe+0x67DB9C0). r13 = sequence, rbx = the rider's
// multi-target controller (found from "Bip01"'s parent), r14 = the palette, esi = item index:
//   mov rax,[r13+60h]; mov r14,[rax+0B8h]          owner manager -> its object palette
//   xor r12d,r12d; mov esi,r12d; cmp [r13+18h],r12d; jbe   item count +0x18
//   ... mov rcx,[r13+28h]; movzx edx,[rcx+rax*8+8] ...     ID tags +0x28 (0x18 each), the bone name
//   call [rax+98h]                                         palette lookup
//   movzx edx,word [rbx+60h]; ... mov r9,[rbx+58h]          controller: target count +0x60, targets +0x58
//   cmp [r9+rax*8],r8 ...                                   the bone among its targets
//   shl rdi,5; add rdi,[r13+20h]                            items +0x20, 32 bytes each
//   imul rax,rax,2028h; add rax,[rbx+50h]; mov [rdi+10h],rax   item +0x10 = &controller.blend[i]
constexpr char kRiderStep[] =
    "49 8B 45 60 4C 8B B0 B8 00 00 00 45 33 E4 41 8B F4 45 39 65 18 0F 86 ?? ?? ?? ?? 90 8B FE 48 8D 04 7F 49 8B 4D 28 "
    "0F B7 54 C1 08 66 41 3B D7 74 0A 48 8B 04 C1 48 03 50 10 EB 03 49 8B D4 49 8B 06 49 8B CE FF 90 98 00 00 00 4C 8B C0 "
    "48 85 C0 74 4D 41 0F B7 CC 0F B7 53 60 66 44 3B E2 73 3F 4C 8B 4B 58 0F 1F 84 00 00 00 00 00 0F B7 C1 4D 39 04 C1 "
    "74 0A 66 FF C1 66 3B CA 72 EF EB 20 66 3B 0D ?? ?? ?? ?? 74 17 48 C1 E7 05 49 03 7D 20 48 69 C0 28 20 00 00 48 03 43 50 "
    "48 89 47 10 FF C6 41 3B 75 18 0F 82";
constexpr size_t kRiderStepStore = 155;  // the "mov [rdi+10h],rax"
// The controller's deleting destructor (vtable slot 0; exe+0x67D7300): body call at +0x0F, then the 0x68-byte free.
constexpr char kMttSlot0[] =
    "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? F6 C3 01 74 0D BA 68 00 00 00 48 8B CF E8 ?? ?? ?? ?? "
    "48 8B 5C 24 30 48 8B C7 48 83 C4 20 5F C3";
constexpr size_t kMttSlot0Body = 0x0F;
// Its body (exe+0x68535A0): vtable store (lea at +6), the blend array +0x50 freed as an array with its count at [-8]
// (vector deleting destructor, flag 3), the target array +0x58 freed.
constexpr char kMttBody[] =
    "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 48 8B 49 50 48 85 C9 74 21 48 83 79 F8 00 74 0C 48 8B 01 "
    "BA 03 00 00 00 FF 10 EB 0E BA 08 00 00 00 48 83 C1 F8 E8 ?? ?? ?? ?? 48 8B 4B 58 E8";
constexpr size_t kMttBodyLea = 6;
// BSAnimGroupSequence's deleting destructor (vtable slot 0; exe+0x67EE5D0): its anim group released, then the
// NiControllerSequence body (at +0x41) and the 0xA8-byte free (same shape as AnimTeardownParallel's check).
constexpr char kGroupSeqSlot0[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 8B FA 48 8B 89 A0 00 00 00 48 85 C9 74 19 B8 FF FF "
    "FF FF F0 0F C1 41 08 83 F8 01 75 0A 48 8B 01 BA 01 00 00 00 FF 10 48 8B CB E8";
constexpr size_t kGroupSeqSlot0Lea = 0x0A;
// The controller's reset (exe+0x6853790): if it has interpolators (+0x60) and a blend array (+0x50), the array is freed
// (vector deleting destructor, count at [-8]) and a new one of +0x60 elements (0x2028 bytes each) allocated; then the
// targets (+0x58) are zeroed. One direct call (exe+0x67F0D17, in the scene-graph walk exe+0x67F0C90).
constexpr char kMttReset[] =
    "48 89 5C 24 18 55 56 57 48 83 EC 30 48 8B F9 66 83 79 60 00 0F 84 ?? ?? ?? ?? 48 8B 49 50 48 85 C9 0F 84 ?? ?? ?? ?? "
    "48 83 79 F8 00 74 0C 48 8B 01 BA 03 00 00 00 FF 10 EB 0E BA 08 00 00 00 48 83 C1 F8 E8 ?? ?? ?? ?? 0F B7 6F 60 "
    "48 89 6C 24 50 B8 28 20 00 00 48 F7 E5";

constexpr size_t kBlendSize = 0x2028;  // one NiBlendTransformInterpolator in the controller's array (checked in kRiderStep)
constexpr size_t kItemSize = 0x20;
constexpr size_t kMaxSites = 16;
constexpr size_t kStubSize = 16;
constexpr size_t kCallScan = 8u << 20;  // the callers are searched this far before and after the callee

using StoreFn = uint64_t (*)(uint8_t* seq, void* root);
using DtorFn = void* (*)(uint8_t* self, uint32_t flags);
using ResetFn = void (*)(uint8_t* mtt);

struct Range { uintptr_t lo, hi; };  // the sequence's foreign blend pointers lie in [lo, hi]
constexpr Range kNone{~uintptr_t(0), 0};

struct Entry {
    Range r = kNone;                // foreign blend pointers now (kNone: none)
    std::vector<uint32_t> nulled;   // items this guard set to null that the game has not bound again
};

struct State {
    bool found = false, installed = false;
    uint8_t* store = nullptr;               // StoreTargets
    uint8_t* sites[kMaxSites] = {};         // its direct calls
    size_t nSites = 0;
    uint8_t* resetSite = nullptr;           // the reset's direct call (nullptr: not wrapped)
    void** mttVt = nullptr;                 // NiMultiTargetTransformController's vtable
    void** seqVt = nullptr;                 // BSAnimGroupSequence's vtable
    DtorFn mttDtor = nullptr, seqDtor = nullptr;  // their original slot 0
    ResetFn reset = nullptr;                // the controller's reset
    uint8_t* stubs = nullptr;
    std::atomic<bool> on{true};
} s;

// The sentinel an item the guard nulled holds during StoreTargets: a zeroed blend interpolator whose interp-info array
// (+0x2000) is a scratch block (an Update racing on another thread writes there; blend index < 256, 32 bytes each).
alignas(64) uint8_t g_sentinel[kBlendSize];
alignas(64) uint8_t g_scratch[256 * 0x20 + 0x40];
uintptr_t Sentinel() { return reinterpret_cast<uintptr_t>(g_sentinel); }
void InitSentinel()
{
    memset(g_sentinel, 0, sizeof g_sentinel);
    void* const scratch = g_scratch;
    memcpy(g_sentinel + 0x2000, &scratch, sizeof scratch);
}

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<const uint8_t*, Entry>* g_noted = nullptr;  // sequences with foreign or nulled items (under g_lock)
std::atomic<uint32_t> g_count{0};
std::atomic<uint64_t> g_nulled{0}, g_deaths{0}, g_resets{0}, g_notes{0}, g_dropped{0}, g_armed{0}, g_offOt{0}, g_noMem{0};
std::atomic<uint32_t> g_testOt{0};  // self-test: 1 = the calling thread counts as the OblivionThread, 2 = it does not
std::atomic<uint64_t> g_logged{~0ull};
thread_local int t_ot = -1;  // -1 not checked yet, 1 = the thread the game names "OblivionThread", 0 = any other

bool Fail(const char* why)
{
    Log("[riderbind] %s: crash guard not installed", why);
    return false;
}

uint8_t* CallTarget(const uint8_t* at) { return RipTarget(at, 1, 5); }

// The thread the game names "OblivionThread" (SetThreadDescription; as OblivionThreadCaches tells it apart).
bool OnOblivionThread()
{
    if (const uint32_t t = g_testOt.load(std::memory_order_relaxed)) return t == 1;
    if (t_ot >= 0) return t_ot == 1;
    using DescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto desc = reinterpret_cast<DescFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    PWSTR name = nullptr;
    int ot = 0;
    if (desc && SUCCEEDED(desc(GetCurrentThread(), &name)) && name) {
        ot = wcscmp(name, L"OblivionThread") == 0 ? 1 : 0;
        LocalFree(name);
    }
    t_ot = ot;
    return ot == 1;
}

// A direct call is taken only as an instruction start (review nit): inside a .pdata function and right after a complete
// `mov rcx|rdx, reg` (REX.W 8B /r, mod 3) or `mov rcx|rdx, [reg+disp8]` (mod 1, no SIB) loading the call's argument, as at
// all five calls in build 19115871. A stray E8 inside another instruction is not preceded by such a mov ending exactly there.
bool ArgMovBefore(const GameModule& game, const uint8_t* site)
{
    const auto mov = [](const uint8_t* p, int mod) {
        if ((p[0] & 0xF0) != 0x40 || !(p[0] & 0x08) || (p[0] & 0x04) || p[1] != 0x8B) return false;  // REX.W, no REX.R
        const int reg = (p[2] >> 3) & 7;
        if ((p[2] >> 6) != mod || (reg != 1 && reg != 2)) return false;
        return mod == 3 || (p[2] & 7) != 4;
    };
    if (site - 4 < game.text.begin) return false;
    return (mov(site - 3, 3) || mov(site - 4, 1)) && PrimaryFunction(game, site) != nullptr;
}

// The direct calls of `callee` within kCallScan around it; false (with *bad set) if one is not an instruction start.
bool CallsOf(const GameModule& game, const uint8_t* callee, uint8_t** out, size_t cap, size_t* n, bool* bad)
{
    *n = 0;
    *bad = false;
    const uint8_t* const lo = callee - kCallScan < game.text.begin ? game.text.begin : callee - kCallScan;
    const uint8_t* const hi = callee + kCallScan + 5 > game.text.end ? game.text.end - 5 : callee + kCallScan;
    for (const uint8_t* p = lo; p < hi; ++p) {
        p = static_cast<const uint8_t*>(memchr(p, 0xE8, hi - p));
        if (!p) break;
        if (CallTarget(p) != callee) continue;
        if (!ArgMovBefore(game, p)) {
            *bad = true;
            return false;
        }
        if (*n == cap) return false;
        out[(*n)++] = const_cast<uint8_t*>(p);
    }
    return *n > 0;
}

// The controller's blend array [lo, hi): its element count from the array allocation ([-8], what the destructor uses),
// else the controller's interpolator count (+0x60).
bool BlendArray(const uint8_t* mtt, uintptr_t* lo, uintptr_t* hi)
{
    const uint8_t* const arr = *reinterpret_cast<const uint8_t* const*>(mtt + 0x50);
    if (!arr) return false;
    uint64_t n = *reinterpret_cast<const uint64_t*>(arr - 8);
    if (n == 0 || n > 0xFFFF) n = *reinterpret_cast<const uint16_t*>(mtt + 0x60);
    if (n == 0) return false;
    *lo = reinterpret_cast<uintptr_t>(arr);
    *hi = *lo + n * kBlendSize;
    return true;
}

// The range of the sequence's blend pointers that do not lie in their own item controller's array (that controller being
// a multi-target transform controller); false if there are none.
bool ForeignRange(const uint8_t* seq, Range* out)
{
    const uint32_t n = *reinterpret_cast<const uint32_t*>(seq + 0x18);
    const uint8_t* const items = *reinterpret_cast<const uint8_t* const*>(seq + 0x20);
    if (!items) return false;
    uintptr_t lo = ~uintptr_t(0), hi = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const uint8_t* const it = items + k * kItemSize;
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(it + 0x10);
        const uint8_t* const c = *reinterpret_cast<const uint8_t* const*>(it + 8);
        if (!b || !c || *reinterpret_cast<void* const*>(c) != s.mttVt) continue;
        uintptr_t clo, chi;
        if (BlendArray(c, &clo, &chi) && b >= clo && b < chi) continue;
        if (b < lo) lo = b;
        if (b > hi) hi = b;
    }
    if (!hi) return false;
    *out = {lo, hi};
    return true;
}

uintptr_t* BlendSlot(const uint8_t* seq, uint32_t k)
{
    uint8_t* const items = *reinterpret_cast<uint8_t* const*>(seq + 0x20);
    return reinterpret_cast<uintptr_t*>(items + k * kItemSize + 0x10);
}
uint32_t ItemCount(const uint8_t* seq)
{
    return *reinterpret_cast<const uint8_t* const*>(seq + 0x20) ? *reinterpret_cast<const uint32_t*>(seq + 0x18) : 0;
}

// Keep only the remembered items that are still null (the game bound the others again); no allocation.
void PruneLocked(const uint8_t* seq, Entry& e)
{
    const uint32_t n = ItemCount(seq);
    size_t w = 0;
    for (const uint32_t k : e.nulled)
        if (k < n && *BlendSlot(seq, k) == 0) e.nulled[w++] = k;
    e.nulled.resize(w);
}

// Remember item k as nulled by the guard; false if it cannot be remembered (out of memory: the item is then left as is).
bool RememberLocked(Entry& e, uint32_t k)
{
    for (const uint32_t j : e.nulled)
        if (j == k) return true;
    try {
        e.nulled.push_back(k);
    } catch (...) {
        return false;
    }
    return true;
}

// The array [lo, hi) is about to be freed (OT, exclusive lock): null every noted item pointing into it and remember it.
uint64_t NullIntoLocked(uintptr_t lo, uintptr_t hi)
{
    uint64_t nulled = 0;
    for (auto& [seq, e] : *g_noted) {
        if (e.r.hi < lo || e.r.lo >= hi) continue;
        const uint32_t n = ItemCount(seq);
        for (uint32_t k = 0; k < n; ++k) {
            uintptr_t* const b = BlendSlot(seq, k);
            if (*b < lo || *b >= hi) continue;
            if (!RememberLocked(e, k)) {
                g_noMem.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            *b = 0;
            ++nulled;
        }
        Range r;
        e.r = ForeignRange(seq, &r) ? r : kNone;  // entries stay while they hold nulled items (the sentinel needs them)
    }
    return nulled;
}

// Off the OT (shared lock): how many noted items point into [lo, hi) (left as stock leaves them).
uint64_t CountIntoLocked(uintptr_t lo, uintptr_t hi)
{
    uint64_t n = 0;
    for (const auto& [seq, e] : *g_noted) {
        if (e.r.hi < lo || e.r.lo >= hi) continue;
        const uint32_t c = ItemCount(seq);
        for (uint32_t k = 0; k < c; ++k) {
            const uintptr_t b = *BlendSlot(seq, k);
            if (b >= lo && b < hi) ++n;
        }
    }
    return n;
}

// The controller's blend array is about to be freed (its destructor or its reset).
void ClearInto(const uint8_t* mtt)
{
    if (!g_count.load(std::memory_order_relaxed) || !s.on.load(std::memory_order_relaxed)) return;
    uintptr_t lo, hi;
    if (!BlendArray(mtt, &lo, &hi)) return;
    if (!OnOblivionThread()) {
        AcquireSRWLockShared(&g_lock);
        const uint64_t n = CountIntoLocked(lo, hi);
        ReleaseSRWLockShared(&g_lock);
        if (n) g_offOt.fetch_add(n, std::memory_order_relaxed);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    const uint64_t n = NullIntoLocked(lo, hi);
    ReleaseSRWLockExclusive(&g_lock);
    if (n) g_nulled.fetch_add(n, std::memory_order_relaxed);
}

uint64_t StoreHook(uint8_t* seq, void* root)
{
    const bool ours = seq && *reinterpret_cast<void* const*>(seq) == s.seqVt;
    bool known = false, armed = false;
    if (ours && g_count.load(std::memory_order_relaxed)) {
        // the items the guard nulled hold the sentinel during the game's call (stock: a stale non-null pointer), so its
        // item loop skips them as in stock (review S1)
        AcquireSRWLockExclusive(&g_lock);
        const auto it = g_noted->find(seq);
        if (it != g_noted->end()) {
            known = true;
            PruneLocked(seq, it->second);
            for (const uint32_t k : it->second.nulled) *BlendSlot(seq, k) = Sentinel();
            armed = !it->second.nulled.empty();
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (armed) g_armed.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t r = reinterpret_cast<StoreFn>(s.store)(seq, root);
    if (!ours) return r;
    if (armed) {  // what the game did not bind again is null again (only this call's items hold the sentinel)
        const uint32_t n = ItemCount(seq);
        for (uint32_t k = 0; k < n; ++k)
            if (*BlendSlot(seq, k) == Sentinel()) *BlendSlot(seq, k) = 0;
    }
    Range rg;
    const bool foreign = ForeignRange(seq, &rg);  // outside the lock (review nit)
    if (!foreign && !known) return r;
    AcquireSRWLockExclusive(&g_lock);
    const auto it = g_noted->find(seq);
    if (it == g_noted->end()) {
        if (foreign) {
            try {
                g_noted->emplace(seq, Entry{rg, {}});
                g_notes.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                g_noMem.fetch_add(1, std::memory_order_relaxed);  // not noted: this sequence stays as stock has it
            }
        }
    } else {
        PruneLocked(seq, it->second);
        it->second.r = foreign ? rg : kNone;
        if (!foreign && it->second.nulled.empty()) g_noted->erase(it);
    }
    g_count.store(static_cast<uint32_t>(g_noted->size()), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

void* MttDtorHook(uint8_t* self, uint32_t flags)
{
    g_deaths.fetch_add(1, std::memory_order_relaxed);
    ClearInto(self);
    return s.mttDtor(self, flags);
}

void ResetHook(uint8_t* mtt)
{
    g_resets.fetch_add(1, std::memory_order_relaxed);
    if (mtt && *reinterpret_cast<const uint16_t*>(mtt + 0x60)) ClearInto(mtt);  // the reset frees only in this case
    s.reset(mtt);
}

void* SeqDtorHook(uint8_t* self, uint32_t flags)
{
    if (g_count.load(std::memory_order_relaxed)) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_noted->erase(self)) g_dropped.fetch_add(1, std::memory_order_relaxed);
        g_count.store(static_cast<uint32_t>(g_noted->size()), std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&g_lock);
    }
    return s.seqDtor(self, flags);
}

struct Col { uint32_t signature, offset, cdOffset, typeDescriptor, hierarchy, self; };

// The complete-object vtable of an RTTI class (type descriptor in .data by its name, its locator and the vtable after the
// locator's pointer in .rdata); nullptr if missing or ambiguous.
void** VtableOf(const GameModule& game, const char* rtti)
{
    std::string pat;
    for (const char* ch = rtti;; ++ch) {
        char hex[4];
        sprintf_s(hex, "%02X ", static_cast<unsigned char>(*ch));
        pat += hex;
        if (!*ch) break;
    }
    const auto hits = FindAll(game.data, ParsePattern(pat));
    if (hits.size() != 1) return nullptr;
    const uint32_t td = static_cast<uint32_t>(hits[0] - 0x10 - game.base);
    const Col* col = nullptr;
    for (uint8_t* p = game.rdata.begin; p + sizeof(Col) <= game.rdata.end; p += 4) {
        const auto* k = reinterpret_cast<const Col*>(p);
        if (k->signature != 1 || k->offset != 0 || k->typeDescriptor != td || game.base + k->self != p) continue;
        if (col) return nullptr;
        col = k;
    }
    if (!col) return nullptr;
    void** vt = nullptr;
    for (uint8_t* p = game.rdata.begin; p + 16 <= game.rdata.end; p += 8)
        if (*reinterpret_cast<const Col* const*>(p) == col) {
            if (vt) return nullptr;
            vt = reinterpret_cast<void**>(p + 8);
        }
    return vt;
}

bool Discover(const GameModule& game, RiderBindGuardInfo* out)
{
    *out = {};
    const auto steps = FindAll(game.text, ParsePattern(kRiderStep));
    out->matches = static_cast<int>(steps.size());
    if (steps.size() != 1) {
        Log("[riderbind] StoreTargets' rider step found %zu times (needs 1)", steps.size());
        return false;
    }
    out->riderStore = steps[0] + kRiderStepStore;
    out->store = PrimaryFunction(game, steps[0]);
    if (!out->store) return Fail("StoreTargets (the function holding the rider step) not found in the unwind data");
    bool bad = false;
    if (!CallsOf(game, out->store, out->sites, kMaxSites, &out->nSites, &bad))
        return Fail(bad ? "a direct call of StoreTargets is not as expected (an argument load right before it)"
                        : out->nSites ? "more StoreTargets calls than expected" : "no direct call of StoreTargets found");
    out->mttVt = VtableOf(game, ".?AVNiMultiTargetTransformController@@");
    out->seqVt = VtableOf(game, ".?AVBSAnimGroupSequence@@");
    if (!out->mttVt || !out->seqVt) return Fail("the controller's or BSAnimGroupSequence's vtable (RTTI) not found");
    uint8_t* const m0 = static_cast<uint8_t*>(out->mttVt[0]);
    uint8_t* const g0 = static_cast<uint8_t*>(out->seqVt[0]);
    if (!game.text.Contains(m0) || !MatchesAt(m0, ParsePattern(kMttSlot0))) return Fail("the controller's destructor is not as expected");
    uint8_t* const body = CallTarget(m0 + kMttSlot0Body);
    if (!game.text.Contains(body) || !MatchesAt(body, ParsePattern(kMttBody)) ||
        RipTarget(body + kMttBodyLea, 3, 7) != reinterpret_cast<uint8_t*>(out->mttVt))
        return Fail("the controller's destructor body is not as expected");
    if (!game.text.Contains(g0) || !MatchesAt(g0, ParsePattern(kGroupSeqSlot0)) ||
        RipTarget(g0 + kGroupSeqSlot0Lea, 3, 7) != reinterpret_cast<uint8_t*>(out->seqVt))
        return Fail("BSAnimGroupSequence's destructor is not as expected");
    out->mttDtor = m0;
    out->seqDtor = g0;
    // the controller's reset and its one direct call (optional: without it that path stays as stock, with a log line)
    const auto resets = FindAll(game.text, ParsePattern(kMttReset));
    uint8_t* resetSites[2] = {};
    size_t nReset = 0;
    if (resets.size() == 1 && CallsOf(game, resets[0], resetSites, 2, &nReset, &bad) && nReset == 1) {
        out->reset = resets[0];
        out->resetSite = resetSites[0];
    } else {
        Log("[riderbind] the controller's reset (found %zu times) or its one direct call not as expected: an array freed "
            "there stays as the game has it",
            resets.size());
    }
    return true;
}

}  // namespace

bool RiderBindGuardDiscover(const GameModule& game, RiderBindGuardInfo* out) { return Discover(game, out); }

bool InstallRiderBindGuard(bool beforeGameCode, bool enabled)
{
    if (s.installed) return true;
    if (!enabled) return false;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    const GameModule& game = Game();
    RiderBindGuardInfo d{};
    if (!Discover(game, &d)) return false;
    uint8_t* const region = AllocNearGame(2 * kStubSize);
    if (!region) return Fail("no free memory near the game's code");
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hooks[2] = {reinterpret_cast<void*>(&StoreHook), reinterpret_cast<void*>(&ResetHook)};
    for (int i = 0; i < 2; ++i) {
        memcpy(region + i * kStubSize, jmpAbs, 6);
        memcpy(region + i * kStubSize + 6, &hooks[i], 8);
    }
    DWORD old;
    VirtualProtect(region, 2 * kStubSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 2 * kStubSize);
    const auto reach = [](const uint8_t* site, const uint8_t* to) {
        const int64_t rel = to - (site + 5);
        return rel == static_cast<int32_t>(rel);
    };
    for (size_t i = 0; i < d.nSites; ++i)
        if (!reach(d.sites[i], region)) return Fail("the stub is out of reach");
    if (d.resetSite && !reach(d.resetSite, region + kStubSize)) return Fail("the stub is out of reach");
    InitSentinel();
    g_noted = new std::unordered_map<const uint8_t*, Entry>();
    g_noted->reserve(1024);
    s.store = d.store;
    s.mttVt = d.mttVt;
    s.seqVt = d.seqVt;
    s.mttDtor = reinterpret_cast<DtorFn>(d.mttDtor);
    s.seqDtor = reinterpret_cast<DtorFn>(d.seqDtor);
    s.reset = reinterpret_cast<ResetFn>(d.reset);
    s.stubs = region;
    // the destructors first (they only drop or null what the store hook notes), then the calls
    if (!WritePointer(&d.mttVt[0], reinterpret_cast<void*>(&MttDtorHook)) ||
        !WritePointer(&d.seqVt[0], reinterpret_cast<void*>(&SeqDtorHook)))
        return Fail("could not write a vtable slot");
    const auto patch = [](uint8_t* site, const uint8_t* to) {
        const int32_t rel32 = static_cast<int32_t>(to - (site + 5));
        return WriteCode(site + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
    };
    if (d.resetSite) {
        if (!patch(d.resetSite, region + kStubSize)) return Fail("could not patch the controller reset's call");
        s.resetSite = d.resetSite;
    }
    for (size_t i = 0; i < d.nSites; ++i) {
        if (!patch(d.sites[i], region)) return Fail("could not patch a StoreTargets call");  // start-up: nothing runs yet
        s.sites[i] = d.sites[i];
    }
    s.nSites = d.nSites;
    s.found = s.installed = true;
    Log("[riderbind] installed: StoreTargets exe+0x%llX (%zu calls), rider step exe+0x%llX, controller destructor exe+0x%llX "
        "(vtable exe+0x%llX), controller reset exe+0x%llX (call exe+0x%llX), BSAnimGroupSequence destructor exe+0x%llX (vtable exe+0x%llX)",
        static_cast<unsigned long long>(d.store - game.base), d.nSites, static_cast<unsigned long long>(d.riderStore - game.base),
        static_cast<unsigned long long>(d.mttDtor - game.base),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(d.mttVt) - game.base),
        d.reset ? static_cast<unsigned long long>(d.reset - game.base) : 0ull,
        d.resetSite ? static_cast<unsigned long long>(d.resetSite - game.base) : 0ull,
        static_cast<unsigned long long>(d.seqDtor - game.base),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(d.seqVt) - game.base));
    return true;
}

void SetRiderBindGuard(bool enabled)
{
    if (!s.installed) return;
    if (s.on.exchange(enabled) != enabled)
        Log("[riderbind] %s", enabled ? "horse animation items into a dying rider's animation data are cleared"
                                      : "horse animation items left as the game leaves them (sequences still noted)");
}

void LogRiderBindGuardStats()
{
    if (!s.installed) return;
    const uint64_t n = g_nulled.load(), off = g_offOt.load();
    if (n + off == g_logged.load()) return;
    g_logged = n + off;
    Log("[riderbind] horse animation items cleared before their rider's animation data was freed: %llu so far (left as the "
        "game has them off the OblivionThread %llu; sequences noted %llu, now %u, dropped at destruction %llu; StoreTargets "
        "calls with cleared items kept %llu; controllers destroyed %llu, reset %llu; not remembered for lack of memory %llu)",
        static_cast<unsigned long long>(n), static_cast<unsigned long long>(off), static_cast<unsigned long long>(g_notes.load()),
        g_count.load(), static_cast<unsigned long long>(g_dropped.load()), static_cast<unsigned long long>(g_armed.load()),
        static_cast<unsigned long long>(g_deaths.load()), static_cast<unsigned long long>(g_resets.load()),
        static_cast<unsigned long long>(g_noMem.load()));
}

bool RiderBindGuardState(RiderBindGuardInfo* out)
{
    if (!s.installed) return false;
    *out = {};
    out->store = s.store;
    out->nSites = s.nSites;
    memcpy(out->sites, s.sites, sizeof s.sites);
    out->mttVt = s.mttVt;
    out->seqVt = s.seqVt;
    out->mttDtor = reinterpret_cast<uint8_t*>(s.mttDtor);
    out->seqDtor = reinterpret_cast<uint8_t*>(s.seqDtor);
    out->reset = reinterpret_cast<uint8_t*>(s.reset);
    out->resetSite = s.resetSite;
    out->stub = s.stubs;
    out->nulled = g_nulled.load();
    out->noted = g_count.load();
    return true;
}

void* RiderBindGuardGroupSeqDtor(void* const* vtable)
{
    void* const slot = vtable[0];
    if (s.installed && vtable == s.seqVt && slot == reinterpret_cast<void*>(&SeqDtorHook)) return reinterpret_cast<void*>(s.seqDtor);
    return slot;
}

// ---- self-test: the hooks' logic on fakes (after the install on the mapped exe; the originals are replaced by fakes) ----
void RiderBindGuardTestSetup(void* mttVt, void* seqVt, void* fakeStore, void* fakeMttDtor, void* fakeSeqDtor, void* fakeReset)
{
    s.mttVt = static_cast<void**>(mttVt);
    s.seqVt = static_cast<void**>(seqVt);
    s.store = static_cast<uint8_t*>(fakeStore);
    s.mttDtor = reinterpret_cast<DtorFn>(fakeMttDtor);
    s.seqDtor = reinterpret_cast<DtorFn>(fakeSeqDtor);
    s.reset = reinterpret_cast<ResetFn>(fakeReset);
    InitSentinel();
    if (!g_noted) g_noted = new std::unordered_map<const uint8_t*, Entry>();
    g_noted->clear();
    g_count = 0;
    g_nulled = 0;
    g_offOt = 0;
    g_testOt = 1;
    s.installed = true;
    s.on = true;
}
uint64_t RiderBindGuardTestStore(void* seq, void* root) { return StoreHook(static_cast<uint8_t*>(seq), root); }
void* RiderBindGuardTestMttDtor(void* mtt, uint32_t flags) { return MttDtorHook(static_cast<uint8_t*>(mtt), flags); }
void* RiderBindGuardTestSeqDtor(void* seq, uint32_t flags) { return SeqDtorHook(static_cast<uint8_t*>(seq), flags); }
void RiderBindGuardTestReset(void* mtt) { ResetHook(static_cast<uint8_t*>(mtt)); }
void RiderBindGuardTestOnOt(bool onOt) { g_testOt = onOt ? 1 : 2; }
uint32_t RiderBindGuardTestNoted() { return g_count.load(); }
uint64_t RiderBindGuardTestOffOt() { return g_offOt.load(); }
const void* RiderBindGuardTestSentinel() { return g_sentinel; }

}  // namespace opf
