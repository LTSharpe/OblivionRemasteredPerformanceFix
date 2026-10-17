// AnimTeardownParallel (working name, lab prototype 2026-10-17, off by default): when the old engine destroys
// an NPC's animation data at a cell-row crossing, the sequences' interpolators are destroyed on helper threads.
//
// Why: every ~4,100 units the
// OblivionThread (OT) changes the loaded cell grid and the game thread waits for it (24-39 ms pairing waits). About a
// third of the outgoing teardown is NPC animation data (0x67EFB60 under 0x667B260): at the worst crossing 14-16 ms of a
// 37-41 ms grid change for 8-10 NPCs. Of that, 62-66 % is one loop: each dying sequence's block array (the
// NiControllerSequence destructor's `eh vector destructor iterator'(blocks, 0x20, n, block release)), 130-150k blocks at
// that crossing. The census of the same run: every dying sequence is a BSAnimGroupSequence, inactive, destroyed inside
// its AnimSequenceSingle wrapper (none inside RemoveAllSequences or a KF release); each block's controller slot (+8) is
// already empty (RemoveAllSequences released it), and its interpolator (+0) is always the last reference: one of four
// classes (NiTransformInterpolator, NiBSplineCompTransformInterpolator, NiFloatInterpolator,
// NiBSplineCompFloatInterpolator). Their destructors only release their data / basis objects (NiTransformData,
// NiFloatData, NiBSplineData, NiBSplineBasisData) with atomic reference counts, decrement the global NiObject counter
// atomically, free their key arrays through the game's start-up key-deleter tables and free their memory through the
// thread-safe game allocator: no scene graph, cache, KF map or controller (audit docs/npc-animation-teardown-audit.md;
// shapes verified below).
//
// What the fix does: inside 0x667B260's call of the animation-data destruction (one call site, retargeted), the OT does
// everything as before except two calls in the sequence destructor: the block array's iterator call and the array's free
// right after it. For an array whose every block has an empty +8 and a +0 that is null or one of the four classes
// with reference count 1, the OT queues the array (it stays owned by the queue; the sequence no longer refers to it
// after its destructor) instead of destroying it; otherwise it calls the iterator as before. Helper threads (and the
// OT itself at the end) take queued arrays, check the data / basis objects' classes, and run the game's own iterator and
// free on them. Before 0x67EFB60's call returns to 0x667B260, the OT waits until every queued array is done; an array
// whose data classes were not as expected is destroyed by the OT there, in queue order. So every object of an actor's
// animation data is destroyed before the actor's clean-up goes on, as before; what changes is which thread destroys the
// interpolators (and their data, when the last reference was theirs) and that they die after the rest of their
// sequence instead of before it. Nothing holds them but their block (count 1); data objects shared with the KF cache's
// sequences are released with the same atomic counts in either order, and whichever release is last deletes. Heap
// addresses differ (other threads' caches), which the game does not depend on.
//
// Off (ini 0, live): the two calls behave exactly as the game's. Found by shape (build 19115871 RVAs in comments).
#include "common.h"

#include <emmintrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace opf {
namespace {

// ---- shapes ----
// NiControllerSequence's destructor body 0x67D7050 (called by BSAnimGroupSequence's destructor; NiControllerSequence's
// own deleting destructor 0x67D71E0 has an inlined copy, left alone): prologue, deactivation if active, name free, the
// block array (iterator 0x6BF0878 with the block release 0x67D6FF0) and its free (0xD989C0)
constexpr char kSeqBlocks[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8D 05 ?? ?? ?? ?? 48 89 01 83 79 68 00 74 0B 45 33 C0 0F 57 C9 E8 ?? ?? ?? ?? "
    "48 8B 4F 10 E8 ?? ?? ?? ?? 48 8B 4F 20 48 85 C9 74 2C 48 8D 59 F8 4C 8D 0D ?? ?? ?? ?? 4C 8B 03 BA 20 00 00 00 "
    "E8 ?? ?? ?? ?? 48 8B 13 48 C1 E2 05 48 83 C2 08 48 8B CB E8 ?? ?? ?? ?? 90";
constexpr size_t kSeqNameFree = 0x2C, kSeqLea = 0x3E, kSeqIter = 0x4D, kSeqFree = 0x60;
// BSAnimGroupSequence's deleting destructor 0x67EE5D0: releases its anim group, then calls the body above (at +0x41)
constexpr char kGroupSeqDtor[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 8B FA 48 8B 89 A0 00 00 00 48 85 C9 74 19 B8 FF FF "
    "FF FF F0 0F C1 41 08 83 F8 01 75 0A 48 8B 01 BA 01 00 00 00 FF 10 48 8B CB E8";
constexpr size_t kGroupSeqCall = 0x41;
// the block release: count-released +8, then +0 (virtual slot 0 with 1 = deleting destructor)
constexpr char kBlockRelease[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 BB FF FF FF FF 48 8B 49 08 48 85 C9 74 14 8B C3 F0 0F C1 41 08 83 F8 01 75 08 "
    "48 8B 01 8D 53 02 FF 10 48 8B 0F 48 85 C9 74 11 F0 0F C1 59 08 83 FB 01 75 07 48 8B 01 8B D3 FF 10 48 8B 5C 24 30 "
    "48 83 C4 20 5F C3";
// the iterator's prologue (MSVC `eh vector destructor iterator')
constexpr char kIterator[] = "48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 53 56 57 41 56 48 83 EC 38 4D 8B F1 49 8B D8";
// 0x667B260: ad = process+0x290; if (ad && ad != arg) { AnimDataDestroy(ad) 0x67EFB60; Free(ad, 0x140) }; process+0x290 = arg
constexpr char kAnimCall[] =
    "48 8B 99 90 02 00 00 48 8B FA 48 8B F1 48 85 DB 74 ?? 48 3B DA 74 ?? 48 8B CB E8 ?? ?? ?? ?? BA 40 01 00 00 48 8B CB "
    "E8 ?? ?? ?? ?? 48 89 BE 90 02 00 00";
constexpr size_t kAnimCallAt = 0x1A, kAnimFreeAt = 0x27;
// 0x67EFB60's prologue up to its first sequence-slot release
constexpr char kAnimData[] =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 48 8B F9 48 83 B9 18 01 00 00 00 74 ?? "
    "48 8D 91 18 01 00 00 E8";

// Destructor shapes (the vtable's slot 0, the deleting destructor; ?? = rel32 / rip displacements). `calls` names the
// direct call / jmp targets in order: each must be the function of that shape (or the allocator's free).
enum Fn { fFree, fObject, fInterp, fKeyBased, fBSpline, fEuler, fFloatDataBase, fCount };
struct Shape {
    const char* name;
    const char* bytes;
    int calls[3];  // Fn ids, -1 = end
};
const Shape kBase[fCount] = {
    {"free", nullptr, {-1}},
    {"NiObject", "48 8D 05 ?? ?? ?? ?? 48 89 01 F0 FF 0D ?? ?? ?? ?? C3", {-1}},
    {"NiInterpolator", "48 8D 05 ?? ?? ?? ?? 48 89 01 E9 ?? ?? ?? ??", {fObject, -1}},
    {"NiKeyBasedInterpolator", "48 8D 05 ?? ?? ?? ?? 48 89 01 E9 ?? ?? ?? ??", {fInterp, -1}},
    {"NiBSplineInterpolator",
     "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F9 48 89 01 BB FF FF FF FF 48 8B 49 28 48 85 C9 74 14 8B C3 "
     "F0 0F C1 41 08 83 F8 01 75 08 48 8B 01 8D 53 02 FF 10 48 8B 4F 20 48 85 C9 74 11 F0 0F C1 59 08 83 FB 01 75 07 48 8B "
     "01 8B D3 FF 10 48 8B CF 48 8B 5C 24 30 48 83 C4 20 5F E9 ?? ?? ?? ??",
     {fInterp, -1}},
    {"Euler rotation keys",
     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8D 59 20 BE 03 00 00 00 48 8D 79 "
     "30 33 ED 4C 8D 35 ?? ?? ?? ?? 48 8B 0F 48 85 C9 74 07 48 63 03 41 FF 14 C6 48 89 2F 48 83 C7 08 89 6B F4 89 2B 48 "
     "83 C3 04 48 83 EE 01 75 DB",
     {-1}},
    {"NiFloatData base",
     "40 53 48 83 EC 20 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 49 18 48 85 C9 74 0E 48 63 43 20 48 8D 15 ?? ?? ?? ?? "
     "FF 14 C2 48 8B CB 48 83 C4 20 5B E9 ?? ?? ?? ??",
     {fObject, -1}},
};
// the classes, by RTTI name; data offsets the interpolator releases
enum Cls { cTI, cBCTI, cFI, cBCFI, cTD, cFD, cBD, cBBD, cGroupSeq, cClasses };
struct ClassShape {
    const char* rtti;
    const char* bytes;  // slot 0
    int calls[4];
};
const ClassShape kClasses[cClasses] = {
    {".?AVNiTransformInterpolator@@",
     "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 8B FA 48 8B 49 38 48 85 C9 74 19 B8 FF FF FF FF F0 0F C1 41 08 83 F8 01 75 0A "
     "48 8B 01 BA 01 00 00 00 FF 10 48 8B CB E8 ?? ?? ?? ?? 40 F6 C7 01 74 0D BA 48 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 8B C3 "
     "48 8B 5C 24 30 48 83 C4 20 5F C3",
     {fKeyBased, fFree, -1}},
    {".?AVNiBSplineCompTransformInterpolator@@",
     "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? F6 C3 01 74 0D BA 78 00 00 00 48 8B CF E8 ?? ?? ?? ?? 48 8B "
     "5C 24 30 48 8B C7 48 83 C4 20 5F C3",
     {fBSpline, fFree, -1}},
    {".?AVNiFloatInterpolator@@",
     "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 8B FA 48 8B 49 20 48 85 C9 74 19 B8 FF FF FF FF F0 0F C1 41 08 83 F8 01 75 0A "
     "48 8B 01 BA 01 00 00 00 FF 10 48 8B CB E8 ?? ?? ?? ?? 40 F6 C7 01 74 0D BA 30 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 8B C3 "
     "48 8B 5C 24 30 48 83 C4 20 5F C3",
     {fKeyBased, fFree, -1}},
    {".?AVNiBSplineCompFloatInterpolator@@",
     "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? F6 C3 01 74 0D BA 40 00 00 00 48 8B CF E8 ?? ?? ?? ?? 48 8B "
     "5C 24 30 48 8B C7 48 83 C4 20 5F C3",
     {fBSpline, fFree, -1}},
    {".?AVNiTransformData@@",
     "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 8B F2 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 79 28 "
     "4C 8D 35 ?? ?? ?? ?? 48 85 FF 74 1C 83 79 18 04 75 08 48 8B CF E8 ?? ?? ?? ?? 48 63 43 18 49 8B 44 C6 60 48 8B CF FF D0 "
     "48 8B 4B 30 48 85 C9 74 0B 48 63 43 1C 49 8B 44 C6 30 FF D0 48 8B 4B 38 48 85 C9 74 0A 48 63 43 20 49 8B 14 C6 FF D2 "
     "48 8B CB E8 ?? ?? ?? ?? 90 40 F6 C6 01 74 0D BA 40 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 8B C3",
     {fEuler, fObject, fFree, -1}},
    {".?AVNiFloatData@@",
     "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? F6 C3 01 74 0D BA 28 00 00 00 48 8B CF E8 ?? ?? ?? ?? 48 8B "
     "5C 24 30 48 8B C7 48 83 C4 20 5F C3",
     {fFloatDataBase, fFree, -1}},
    {".?AVNiBSplineData@@",
     "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F9 48 89 01 8B DA 48 8B 49 10 E8 ?? ?? ?? ?? 48 8B 4F 18 E8 ?? "
     "?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? F6 C3 01 74 0D BA 28 00 00 00 48 8B CF E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B C7 48 83 C4 "
     "20 5F C3",
     {fFree, fFree, fObject, fFree}},
    {".?AVNiBSplineBasisData@@",
     "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 8B DA 48 89 01 48 8B F9 E8 ?? ?? ?? ?? F6 C3 01 74 0D BA 30 00 00 00 "
     "48 8B CF E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B C7",
     {fObject, fFree, -1}},
    {".?AVBSAnimGroupSequence@@", nullptr, {-1}},  // checked separately (kGroupSeqDtor)
};
// key-deleter helpers in the tables (exe+0x9488D80, 18 slots): null, the empty function (ret 0), or "if (p) {
// iterator(p - 8, size, [p - 8], empty); free(p - 8, ...) }"
constexpr char kKeyFree[] = "48 85 C9 74 ?? 53 48 83 EC 20 48 8D 59 F8 4C 8D 0D ?? ?? ?? ?? 4C 8B 03 BA ?? 00 00 00 E8 ?? ?? ?? ??";
constexpr int kKeySlots = 18;

using IterFn = void (*)(void* arr, uint64_t size, uint64_t count, void* dtor);
using FreeFn = void (*)(void* p, uint64_t size);
using AnimFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);

struct State {
    IterFn iterator = nullptr;
    FreeFn free = nullptr;
    void* blockDtor = nullptr;
    AnimFn animData = nullptr;
    uintptr_t vt[cClasses] = {};
    uint8_t* keyTable = nullptr;
    uint8_t* empty = nullptr;
    uint8_t* sites[3] = {};  // the iterator call, the array free call, 0x667B260's call
    bool found = false, installed = false;
    int keysOk = -1;  // key-deleter tables checked at the first use: 1 ok, 0 not as expected (the fix stays off)
    std::atomic<int> on{0};
} s;

// ---- queue (one owner at a time: the thread inside AnimDataHook) ----
constexpr uint32_t kCap = 16384;
struct Item {
    uint8_t* arr;
    uint64_t count;
    std::atomic<uint32_t> state;  // 0 queued, 1 claimed, 2 done, 3 for the owner (data classes not as expected)
};
Item g_items[kCap];
// generation (bits 48-63) | arrays queued (bits 24-47) | next to claim (bits 0-23): a helper's claim from an earlier
// window fails its compare-exchange on the generation
std::atomic<uint64_t> g_claim{0};
std::atomic<uint32_t> g_done{0};
constexpr uint64_t kPushOne = 1ull << 24;
uint32_t Pushed(uint64_t c) { return static_cast<uint32_t>((c >> 24) & 0xFFFFFF); }
std::atomic<uint32_t> g_owner{0};
std::atomic<bool> g_window{false};
HANDLE g_active = nullptr;  // manual reset: set while an owner is inside the destruction
WaitGate g_claimGate;       // idle helpers waiting for a new array or the window's end (both change g_claim)
WaitGate g_doneGate;        // the owner waiting for arrays a helper is destroying
int g_helpers = 0;
thread_local uint8_t* t_lastQueued = nullptr;  // the base (arr - 8) of the array queued last: its free is skipped
thread_local int t_nested = 0;  // the owner inside a nested animation-data destruction: nothing queued there

std::atomic<uint64_t> g_actors{0}, g_arrays{0}, g_blocks{0}, g_inline{0}, g_byHelper{0}, g_byOwner{0}, g_fallback{0},
    g_full{0}, g_waitUs{0}, g_adataUs{0};
uint64_t g_logged = 0;

uint64_t Us()
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return QpcToMicroseconds(static_cast<uint64_t>(t.QuadPart), static_cast<uint64_t>(f.QuadPart));
}

// The OT's check before it queues: +8 empty; +0 null or one of the four interpolator classes, count 1.
bool QueueEligible(uint8_t* arr, uint64_t n)
{
    for (uint64_t j = 0; j < n; ++j) {
        uint8_t* const b = arr + j * 0x20;
        if (*reinterpret_cast<void**>(b + 8)) return false;
        uint8_t* const o = *reinterpret_cast<uint8_t**>(b);
        if (!o) continue;
        const uintptr_t vt = *reinterpret_cast<uintptr_t*>(o);
        if (vt != s.vt[cTI] && vt != s.vt[cBCTI] && vt != s.vt[cFI] && vt != s.vt[cBCFI]) return false;
        if (*reinterpret_cast<int32_t*>(o + 8) != 1) return false;
    }
    return true;
}

// The worker's check before it destroys: each interpolator's data / basis objects are of the expected classes.
bool DataEligible(uint8_t* arr, uint64_t n)
{
    for (uint64_t j = 0; j < n; ++j) {
        uint8_t* const o = *reinterpret_cast<uint8_t**>(arr + j * 0x20);
        if (!o) continue;
        const uintptr_t vt = *reinterpret_cast<uintptr_t*>(o);
        auto is = [](uint8_t* p, Cls c) { return !p || *reinterpret_cast<uintptr_t*>(p) == s.vt[c]; };
        if (vt == s.vt[cTI]) {
            if (!is(*reinterpret_cast<uint8_t**>(o + 0x38), cTD)) return false;
        } else if (vt == s.vt[cFI]) {
            if (!is(*reinterpret_cast<uint8_t**>(o + 0x20), cFD)) return false;
        } else {  // the two compressed B-spline classes (QueueEligible let only these four through)
            if (!is(*reinterpret_cast<uint8_t**>(o + 0x20), cBD) || !is(*reinterpret_cast<uint8_t**>(o + 0x28), cBBD)) return false;
        }
    }
    return true;
}

void Destroy(const Item& it)
{
    s.iterator(it.arr, 0x20, it.count, s.blockDtor);
    s.free(it.arr - 8, it.count * 0x20 + 8);
}

// Claim and run one queued array; false when none is left to claim.
bool RunOne(bool owner)
{
    uint64_t c = g_claim.load(std::memory_order_acquire);
    uint32_t i;
    for (;;) {
        i = static_cast<uint32_t>(c & 0xFFFFFF);
        if (i >= Pushed(c)) return false;
        if (g_claim.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel, std::memory_order_acquire)) break;
    }
    Item& it = g_items[i];
    if (DataEligible(it.arr, it.count)) {
        Destroy(it);
        it.state.store(2, std::memory_order_release);
        (owner ? g_byOwner : g_byHelper).fetch_add(1, std::memory_order_relaxed);
    } else {
        it.state.store(3, std::memory_order_release);
    }
    g_done.fetch_add(1, std::memory_order_acq_rel);
    WakeWord(g_done, g_doneGate);
    return true;
}

DWORD WINAPI HelperMain(void*)
{
    for (;;) {
        WaitForSingleObject(g_active, INFINITE);
        HelperThreadCache();  // ParallelStaticBodies's allocator caches for this helper (between jobs)
        while (g_window.load(std::memory_order_acquire)) {
            const uint64_t c = g_claim.load(std::memory_order_acquire);
            if (RunOne(false)) continue;
            WaitWord(g_claim, g_claimGate, [c](uint64_t v) { return v != c; });  // spins ~50 us, then sleeps until a push or the end
        }
    }
}

// The sequence destructor's block-array iterator call (rcx = array, rdx = 0x20, r8 = count, r9 = block release).
void IterHook(void* arr, uint64_t size, uint64_t count, void* dtor)
{
    if (g_window.load(std::memory_order_relaxed) && !t_nested && g_owner.load(std::memory_order_relaxed) == GetCurrentThreadId() &&
        dtor == s.blockDtor && size == 0x20 && count && count < (1u << 24)) {
        uint8_t* const a = static_cast<uint8_t*>(arr);
        const uint32_t i = Pushed(g_claim.load(std::memory_order_relaxed));  // only the owner queues
        if (i < kCap && QueueEligible(a, count)) {
            Item& it = g_items[i];
            it.arr = a;
            it.count = count;
            it.state.store(0, std::memory_order_relaxed);
            g_claim.fetch_add(kPushOne, std::memory_order_acq_rel);  // publishes the item (helpers only add to bits 0-23)
            WakeWord(g_claim, g_claimGate);
            t_lastQueued = a - 8;
            g_arrays.fetch_add(1, std::memory_order_relaxed);
            g_blocks.fetch_add(count, std::memory_order_relaxed);
            return;
        }
        (i < kCap ? g_inline : g_full).fetch_add(1, std::memory_order_relaxed);
    }
    s.iterator(arr, size, count, dtor);
}

// The free right after it (rcx = array - 8, rdx = count * 0x20 + 8): skipped for the array just queued.
void FreeHook(void* p, uint64_t size)
{
    if (p && p == t_lastQueued) {
        t_lastQueued = nullptr;
        return;
    }
    s.free(p, size);
}

bool KeyTablesOk();

// 0x667B260's call of the animation-data destruction 0x67EFB60.
uint64_t AnimDataHook(uint64_t ad, uint64_t b, uint64_t c, uint64_t d)
{
    uint32_t expected = 0;
    const uint32_t me = GetCurrentThreadId();
    if (g_owner.load(std::memory_order_relaxed) == me) {  // nested: as the game does it
        ++t_nested;
        const uint64_t r = s.animData(ad, b, c, d);
        --t_nested;
        return r;
    }
    if (!s.on.load(std::memory_order_relaxed) || !g_helpers || s.keysOk == 0 ||
        !g_owner.compare_exchange_strong(expected, me, std::memory_order_acq_rel))
        return s.animData(ad, b, c, d);
    if (s.keysOk < 0 && !KeyTablesOk()) {
        g_owner.store(0, std::memory_order_release);
        return s.animData(ad, b, c, d);
    }
    const uint64_t t0 = Us();
    g_done.store(0, std::memory_order_relaxed);
    g_claim.store(((g_claim.load(std::memory_order_relaxed) >> 48) + 1) << 48, std::memory_order_release);
    t_lastQueued = nullptr;
    g_window.store(true, std::memory_order_release);
    SetEvent(g_active);
    const uint64_t r = s.animData(ad, b, c, d);
    // join: take part, then wait for the helpers' arrays
    const uint64_t t1 = Us();
    while (RunOne(true)) {
    }
    const uint32_t n = Pushed(g_claim.load(std::memory_order_acquire));
    WaitWord(g_done, g_doneGate, [n](uint32_t d) { return d >= n; });
    g_window.store(false, std::memory_order_release);
    ResetEvent(g_active);
    // a new generation with nothing queued: idle helpers wake, see the window closed and go back to g_active
    g_claim.store(((g_claim.load(std::memory_order_relaxed) >> 48) + 1) << 48, std::memory_order_release);
    WakeWord(g_claim, g_claimGate);
    // arrays whose data classes were not as expected: destroyed here, in queue order
    for (uint32_t i = 0; i < n; ++i)
        if (g_items[i].state.load(std::memory_order_acquire) == 3) {
            Destroy(g_items[i]);
            g_fallback.fetch_add(1, std::memory_order_relaxed);
        }
    const uint64_t t2 = Us();
    g_waitUs.fetch_add(t2 - t1, std::memory_order_relaxed);
    g_adataUs.fetch_add(t2 - t0, std::memory_order_relaxed);
    g_actors.fetch_add(1, std::memory_order_relaxed);
    g_owner.store(0, std::memory_order_release);
    return r;
}

// ---- finding ----
bool Fail(const char* why)
{
    Log("[animpar] %s: fix not installed", why);
    return false;
}

const GameModule* g_game = nullptr;

uint8_t* CallTarget(const uint8_t* at) { return RipTarget(at, 1, 5); }

// `fn` has the shape and its direct calls / jmps (E8 / E9 rel32 at wildcard positions, in order) go to calls[].
bool CheckShape(const uint8_t* fn, const char* bytes, const int* calls, int nCalls, uint8_t* const* fns, int depth);

bool CheckFn(const uint8_t* fn, int id, uint8_t** fns, int depth)
{
    if (!g_game->text.Contains(fn) || depth > 4) return false;
    if (id == fFree) return fn == fns[fFree];
    if (fns[id]) return fns[id] == fn;
    if (!CheckShape(fn, kBase[id].bytes, kBase[id].calls, 3, fns, depth + 1)) return false;
    fns[id] = const_cast<uint8_t*>(fn);
    return true;
}

bool CheckShape(const uint8_t* fn, const char* bytes, const int* calls, int nCalls, uint8_t* const* fnsIn, int depth)
{
    const Pattern p = ParsePattern(bytes);
    if (!MatchesAt(fn, p)) return false;
    uint8_t** const fns = const_cast<uint8_t**>(fnsIn);
    int k = 0;
    for (size_t i = 0; i + 5 <= p.size(); ++i) {
        if ((p.bytes[i] != 0xE8 && p.bytes[i] != 0xE9) || !p.care[i] || p.care[i + 1]) continue;
        if (k >= nCalls || calls[k] < 0) return false;
        if (!CheckFn(CallTarget(fn + i), calls[k], fns, depth)) return false;
        ++k;
        i += 4;
    }
    return k == nCalls || calls[k] < 0;
}

struct Col { uint32_t signature, offset, cdOffset, typeDescriptor, hierarchy, self; };

// The complete-object vtables of the classes (RTTI: type descriptors in .data by name; their locators and the
// vtables after them in .rdata); one pass over .rdata for all. False if any is missing or ambiguous.
bool FindVtables()
{
    const GameModule& game = *g_game;
    uint32_t td[cClasses] = {};
    for (int c = 0; c < cClasses; ++c) {
        std::string pat;
        for (const char* ch = kClasses[c].rtti;; ++ch) {
            char hex[4];
            sprintf_s(hex, "%02X ", static_cast<unsigned char>(*ch));
            pat += hex;
            if (!*ch) break;
        }
        const auto hits = FindAll(game.data, ParsePattern(pat));
        if (hits.size() != 1) {
            Log("[animpar] type descriptor of %s: %zu matches", kClasses[c].rtti, hits.size());
            return false;
        }
        td[c] = static_cast<uint32_t>(hits[0] - 0x10 - game.base);
    }
    const Col* col[cClasses] = {};
    for (uint8_t* p = game.rdata.begin; p + sizeof(Col) <= game.rdata.end; p += 4) {
        const auto* k = reinterpret_cast<const Col*>(p);
        if (k->signature != 1 || k->offset != 0 || game.base + k->self != p) continue;
        for (int c = 0; c < cClasses; ++c)
            if (k->typeDescriptor == td[c]) {
                if (col[c]) return false;
                col[c] = k;
            }
    }
    for (int c = 0; c < cClasses; ++c)
        if (!col[c]) return false;
    for (uint8_t* p = game.rdata.begin; p + 16 <= game.rdata.end; p += 8) {
        const Col* const k = *reinterpret_cast<const Col* const*>(p);
        for (int c = 0; c < cClasses; ++c)
            if (k == col[c]) {
                if (s.vt[c]) return false;
                s.vt[c] = reinterpret_cast<uintptr_t>(p + 8);
            }
    }
    for (int c = 0; c < cClasses; ++c)
        if (!s.vt[c]) return false;
    return true;
}

void ResolveEmpty();

bool KeyTablesOk()
{
    const GameModule& game = *g_game;
    ResolveEmpty();
    if (!s.empty) {
        s.keysOk = 0;
        Log("[animpar] key-deleter tables: no helper entry with the empty callback found: fix stays off");
        return false;
    }
    bool ok = true;
    int nul = 0, emp = 0, hel = 0;
    for (int i = 0; i < kKeySlots && ok; ++i) {
        uint8_t* const f = reinterpret_cast<uint8_t**>(s.keyTable)[i];
        if (!f) { ++nul; continue; }
        if (f == s.empty) { ++emp; continue; }
        ok = game.text.Contains(f) && MatchesAt(f, ParsePattern(kKeyFree)) && RipTarget(f + 14, 3, 7) == s.empty &&
             CallTarget(f + 29) == reinterpret_cast<uint8_t*>(s.iterator);
        if (ok) {  // its free within the next 30 bytes
            bool fr = false;
            for (int k = 34; k < 64 && !fr; ++k)
                fr = f[k] == 0xE8 && CallTarget(f + k) == reinterpret_cast<uint8_t*>(s.free);
            ok = fr;
        }
        ++hel;
    }
    s.keysOk = ok ? 1 : 0;
    Log("[animpar] key-deleter tables %s (%d helpers, %d empty, %d null)%s", ok ? "as expected" : "NOT as expected", hel, emp,
        nul, ok ? "" : ": fix stays off");
    return ok;
}

uint8_t* FindOne(const char* pattern, const char* what)
{
    const auto hits = FindAll(g_game->text, ParsePattern(pattern));
    if (hits.size() != 1) {
        Log("[animpar] %s not found (%zu matches)", what, hits.size());
        return nullptr;
    }
    return hits[0];
}

bool StartHelpers(int n = 0)
{
    if (g_helpers) return true;
    if (n <= 0) {
        n = static_cast<int>(UsableLogicalProcessors()) / 4;  // 16 hardware threads -> 4 helpers (+ the OT)
        n = n < 1 ? 1 : n > 4 ? 4 : n;
    }
    g_active = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_active) return false;
    for (int i = 0; i < n; ++i) {
        HANDLE h = CreateThread(nullptr, 64 * 1024, HelperMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!h) break;
        wchar_t name[40];
        swprintf_s(name, L"OblivionPerfFix anim teardown %d", i + 1);
        DescribeThread(h, name);
        CloseHandle(h);
        ++g_helpers;
    }
    return g_helpers > 0;
}

bool Retarget(uint8_t* call, const uint8_t* to)
{
    const int64_t rel = to - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}

}  // namespace

bool FindAnimTeardownParallel()
{
    if (s.found) return true;
    g_game = &Game();
    uint8_t* const seq = FindOne(kSeqBlocks, "the sequence destructor's block array");
    if (!seq) return false;
    uint8_t* fns[fCount] = {};
    fns[fFree] = CallTarget(seq + kSeqNameFree);
    if (CallTarget(seq + kSeqFree) != fns[fFree]) return Fail("the two frees differ");
    uint8_t* const iter = CallTarget(seq + kSeqIter);
    uint8_t* const blk = RipTarget(seq + kSeqLea, 3, 7);
    if (!MatchesAt(iter, ParsePattern(kIterator))) return Fail("the iterator is not as expected");
    if (!MatchesAt(blk, ParsePattern(kBlockRelease))) return Fail("the block release is not as expected");
    uint8_t* const call = FindOne(kAnimCall, "the animation data's destruction call");
    if (!call) return false;
    uint8_t* const ad = CallTarget(call + kAnimCallAt);
    if (!MatchesAt(ad, ParsePattern(kAnimData)) || CallTarget(call + kAnimFreeAt) != fns[fFree])
        return Fail("the animation data's destruction is not as expected");
    if (!FindVtables()) return Fail("the classes' vtables (RTTI) not found");
    {
        const auto* gs = *reinterpret_cast<uint8_t* const*>(s.vt[cGroupSeq]);
        if (!g_game->text.Contains(gs) || !MatchesAt(gs, ParsePattern(kGroupSeqDtor)) || CallTarget(gs + kGroupSeqCall) != seq)
            return Fail("BSAnimGroupSequence's destructor does not call the destructor body found");
    }
    for (int c = 0; c < cGroupSeq; ++c) {
        const auto* slot0 = *reinterpret_cast<uint8_t* const*>(s.vt[c]);
        if (!CheckShape(slot0, kClasses[c].bytes, kClasses[c].calls, 4, fns, 0)) {
            Log("[animpar] %s's destructor is not as expected", kClasses[c].rtti);
            return Fail("destructor shape");
        }
    }
    // one key-deleter table base for the transform data, the float data and the Euler keys (lea r14 / rdx)
    uint8_t* const td = *reinterpret_cast<uint8_t* const*>(s.vt[cTD]);
    uint8_t* const kt = RipTarget(td + 0x28, 3, 7);
    if (RipTarget(fns[fFloatDataBase] + 0x20, 3, 7) != kt || RipTarget(fns[fEuler] + 0x29, 3, 7) != kt || !g_game->data.Contains(kt))
        return Fail("the key-deleter tables differ");
    s.keyTable = kt;  // its entries are written by the game's static initialisers: checked at the first use
    s.iterator = reinterpret_cast<IterFn>(iter);
    s.free = reinterpret_cast<FreeFn>(fns[fFree]);
    s.blockDtor = blk;
    s.animData = reinterpret_cast<AnimFn>(ad);
    s.sites[0] = seq + kSeqIter;
    s.sites[1] = seq + kSeqFree;
    s.sites[2] = call + kAnimCallAt;
    s.found = true;
    Log("[animpar] found: sequence block array exe+0x%llX, animation data destruction exe+0x%llX (call exe+0x%llX), "
        "9 classes by RTTI, key-deleter tables exe+0x%llX",
        static_cast<unsigned long long>(seq - g_game->base), static_cast<unsigned long long>(ad - g_game->base),
        static_cast<unsigned long long>(s.sites[2] - g_game->base), static_cast<unsigned long long>(kt - g_game->base));
    return true;
}

namespace {
// The empty key deleter ("ret 0"): the callback of the first helper-shaped entry of the tables.
void ResolveEmpty()
{
    for (int i = 0; i < kKeySlots && !s.empty; ++i) {
        uint8_t* const f = reinterpret_cast<uint8_t**>(s.keyTable)[i];
        if (f && g_game->text.Contains(f) && MatchesAt(f, ParsePattern(kKeyFree))) {
            uint8_t* const e = RipTarget(f + 14, 3, 7);
            if (g_game->text.Contains(e) && e[0] == 0xC2 && e[1] == 0 && e[2] == 0) s.empty = e;
        }
    }
}
}  // namespace

bool InstallAnimTeardownParallel(bool beforeGameCode)
{
    if (s.installed) return true;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    if (!FindAnimTeardownParallel()) return false;
    if (!StartHelpers()) return Fail("no helper threads");
    uint8_t* const region = AllocNearGame(64);
    if (!region) return Fail("no free memory near the game's code");
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hooks[3] = {reinterpret_cast<void*>(&IterHook), reinterpret_cast<void*>(&FreeHook),
                            reinterpret_cast<void*>(&AnimDataHook)};
    for (int i = 0; i < 3; ++i) {
        memcpy(region + i * 16, jmpAbs, 6);
        memcpy(region + i * 16 + 6, &hooks[i], 8);
    }
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    for (int i = 0; i < 3; ++i) {
        const int64_t rel = region + i * 16 - (s.sites[i] + 5);
        if (rel != static_cast<int32_t>(rel)) return Fail("stubs out of reach");
    }
    for (int i = 0; i < 3; ++i)
        if (!Retarget(s.sites[i], region + i * 16)) return Fail("could not patch a call");  // start-up: nothing runs yet
    s.installed = true;
    Log("[animpar] installed: 3 calls redirected, %d helper threads", g_helpers);
    return true;
}

void SetAnimTeardownParallel(int mode)
{
    if (!s.installed) return;
    if (s.on.exchange(mode ? 1 : 0) != (mode ? 1 : 0))
        Log("[animpar] %s", mode ? "interpolators of dying NPC animation data destroyed on helper threads"
                                 : "animation data destroyed as the game does it");
}

void LogAnimTeardownParallelStats()
{
    if (!s.installed) return;
    const uint64_t a = g_actors.load();
    if (a == g_logged) return;
    g_logged = a;
    Log("[animpar] actors %llu (%.2f ms each, join wait %.2f ms each), arrays queued %llu (%llu blocks): helpers %llu, OT %llu, "
        "OT fallback %llu; run inline %llu, queue full %llu",
        static_cast<unsigned long long>(a), a ? g_adataUs.load() / 1000.0 / a : 0.0, a ? g_waitUs.load() / 1000.0 / a : 0.0,
        static_cast<unsigned long long>(g_arrays.load()), static_cast<unsigned long long>(g_blocks.load()),
        static_cast<unsigned long long>(g_byHelper.load()), static_cast<unsigned long long>(g_byOwner.load()),
        static_cast<unsigned long long>(g_fallback.load()), static_cast<unsigned long long>(g_inline.load()),
        static_cast<unsigned long long>(g_full.load()));
}

// ---- self-test ----
bool AnimTeardownParallelTestKeyTables(uint8_t* table)
{
    s.keyTable = table;
    s.empty = nullptr;
    s.keysOk = -1;
    return KeyTablesOk();
}

bool AnimTeardownParallelTestSetup(const AnimTeardownTest& t, int helpers)
{
    s.iterator = reinterpret_cast<IterFn>(t.iterator);
    s.free = reinterpret_cast<FreeFn>(t.free);
    s.blockDtor = t.blockDtor;
    s.animData = reinterpret_cast<AnimFn>(t.animData);
    memcpy(s.vt, t.vt, sizeof t.vt);
    s.keyTable = t.keyTable;
    s.empty = t.empty;
    s.keysOk = 1;
    if (!StartHelpers(helpers)) return false;
    for (auto* c : {&g_actors, &g_arrays, &g_blocks, &g_inline, &g_byHelper, &g_byOwner, &g_fallback, &g_full, &g_waitUs, &g_adataUs})
        c->store(0);
    s.installed = true;
    s.on.store(1);
    return true;
}

void AnimTeardownParallelTestIter(void* arr, uint64_t size, uint64_t count, void* dtor) { IterHook(arr, size, count, dtor); }
void AnimTeardownParallelTestFree(void* p, uint64_t size) { FreeHook(p, size); }
uint64_t AnimTeardownParallelTestAnimData(uint64_t ad) { return AnimDataHook(ad, 0, 0, 0); }

AnimTeardownCounts AnimTeardownParallelTestCounts()
{
    return {g_actors.load(), g_arrays.load(), g_blocks.load(), g_byHelper.load(), g_byOwner.load(), g_fallback.load(),
            g_inline.load()};
}

}  // namespace opf
