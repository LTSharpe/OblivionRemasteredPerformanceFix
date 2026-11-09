// MapPopupSpread (worker mapspread, 2026-11-02, findings "mapspread"; public since 1.3.0, port130k; review rev130f).
// [Fixes] MapPopupSpread = 0: not installed (start-up); 5 (default) = drop (1) + reuse (4); then live 0, 1, 4, 5. Installed at
// start-up (version.dll route: the native tables are patched before the classes register them) when every site below is found
// by pattern and the map widget's layout reads match; a mismatch leaves it off with a log line. Needs NpcCreationPacing's
// frame hooks (its pairing run is the window the kept work lives in).
//
// What the map open costs (findings "mapspread"): the map widget's Blueprint function "Update Icons" = ClearAllMapIcons;
// UpdateOrCreateMapIcons(list); UpdateIconsSizeSmoothly. Altar runs it 3-5 times in the open frame (lists of 115 / 363 /
// 363 / 363 entries, from the OT->GT pairing run in OnWorldPreActorTick) and once more in the next frame. Every run starts
// by clearing the icons the previous run created (~0.3 ms per icon: a WBP_Modern_MapIcon_C widget tree each), so in the
// open frame all but the last run's icons are created only to be removed again, before anything draws or reads them.
//
// Bit 1 (drop): inside the frame's pairing run (NpcCreationPacing's FrameRun), UpdateOrCreateMapIcons keeps a copy of its list
// (Blueprint passes its own array by reference; each entry copied with the game's FMapIconData copy constructor into zeroed
// memory) instead of creating the icons; UpdateIconsSizeSmoothly right after it (the map holds no other icon) is recorded. If
// ClearAllMapIcons comes next for that widget, the kept list is destroyed as the exec thunk would have (nothing was created,
// nothing to remove). Anything else that could see the icons first runs the kept work in stock order ("flush"): every native
// of the map widget, icon filter and icon widget classes (their native tables are patched before registration), the map
// widget's own natives called from C++ (hover finder, zoom iterators, player position / marker, single icon, icon position),
// any ProcessEvent on a map widget other than "Update Icons", a new list for the same widget, and the end of the pairing run
// (so the last list's icons exist before the world ticks on and long before anything is drawn). Safety flushes at the
// post-actor tick and the next frame start count as "late" (0 expected). Lists cleared before anything sees them are never
// built: the result is what stock shows.
// Bit 4 (reuse): when a map widget's icons were built by the previous list and only quiet events ran since, its
// ClearAllMapIcons is held; if the next list equals the icons' entries (all values but the per-list IconAddress string and the
// FText instance) and was built for the same kind of map (UpdateOrCreateMapIcons' !IsWorldMap flag, rev130f M-S1), the icons
// stay and take the new entries in place (icon map key, each icon's Properties, the Blueprint's PlayerIcon for type 14), else
// the held clear runs as stock. Within one map open only: the map widget is destroyed at every close (its Destruct is not a
// quiet event), so every open starts fresh, as stock (user: menus start fresh).
//
// EQUIVALENT: the icons shown are made from the same final list as stock; dropped lists' icons never existed for anything to
// see; reused icons hold the same values (only the IconAddress string instance differs). Measured (findings "mapspread",
// play1107e): map open frame -100..-300 ms of game thread; play: 6 reuses / 689 icons, 0 late, 0 Properties mismatches.
//
// Found by pattern (build 19115871 addresses in comments): the three classes' native tables by an entry's name string (the one
// {name, exec thunk} array, not the {name, construct} link info), the map's functions through their exec thunks (each call
// checked at its place in the thunk, each target's entry bytes checked), the hover finder by its own signature, ProcessEvent's
// vtable slot from a dynamic-delegate call and its function by its prologue, FName's constructor by its prologue.
// Fixed while porting (port130k): the lab key patched the icon filter's and icon widget's link-info arrays (their construct
// functions) instead of their native tables, so those natives never flushed, and missed the map table's 65th entry; and the
// ProcessEvent slot is now checked to hold ProcessEvent before it is written (M-S2).
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <intrin.h>

namespace opf {
namespace {

std::atomic<int> g_mode{0};
int g_runMode = 0;  // the mode latched at the start of the frame's pairing run (game thread)
bool g_testGt = false;
bool IsGt() { return g_testGt || NpcPacingOnGameThread(); }

uint64_t NowUs()
{
    static const LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart / f.QuadPart * 1000000 + c.QuadPart % f.QuadPart * 1000000 / f.QuadPart);
}

int Clean(int mode) { return mode < 0 ? 0 : (mode & 5); }

// ---- the map widget (layout pinned by the checks in Discover) ----
// Icon TMap<FMapIconData (0x50), UWidget*> at +0x590: TSparseArray data +0x590 (element 0x60, widget at +0x50), Num +0x598,
// allocation bits inline +0x5A0 / heap +0x5B0, NumBits +0x5B8, NumFreeIndices +0x5C4 (UE 5.3 TSet layout). MapIconFilter
// +0x4D8 (its "all" mask +0x598), the hovered icon +0x608; zoom +0x580, its range +0x630 / +0x634 (UpdateIconsSizeSmoothly's
// inputs). UObject: class +0x10, name +0x18.
constexpr uint32_t kOffMap = 0x590, kOffFilter = 0x4D8, kOffHover = 0x608, kFilterMask = 0x598;
// The filter's other field ClearAllMapIcons resets next to the mask (+0x5F0; both stores pinned by kClearFilter; rev130n MS-S1)
// is its count of displayed icons, not a flag: the clear zeroes it and recounts at once (exe+0x493C890 walks the filter's
// registered widgets, +1 per displayed one), each new icon's registration adds 1 (exe+0x4925640), and its only reader is
// IsFilterHasAtLeastOneDisplayedIcon (cmp [+0x5F0], 0). A reused list leaves the count of the previous equal build (the
// filter showing all, as guarded); stock's clear + rebuild recounts the same registrations: the same answer to its only
// reader. So it is not a reuse guard (requiring 0 blocked every reuse: "guards 7" in rel130p's first live check, as the
// count is never 0 with icons shown). Used by the self-test only.
constexpr uint32_t kFilterShown = 0x5F0;
constexpr uint32_t kOffZoom = 0x580, kOffZoomMin = 0x630, kOffZoomMax = 0x634;
constexpr uint32_t kObjClass = 0x10, kObjName = 0x18;
int IconCount(const uint8_t* self)
{
    __try {
        return *reinterpret_cast<const int*>(self + kOffMap + 8) - *reinterpret_cast<const int*>(self + kOffMap + 0x34);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool SparseAllocated(const uint8_t* set, uint32_t bitsInline, uint32_t bitsPtr, int idx)
{
    const uint32_t* bits = *reinterpret_cast<const uint32_t* const*>(set + bitsPtr);
    if (!bits) bits = reinterpret_cast<const uint32_t*>(set + bitsInline);
    return (bits[idx >> 5] >> (idx & 31)) & 1;
}

// ---- game functions ----
struct TArr {
    uint8_t* data;
    int num, max;
};
struct NameId {  // FName: comparison index, number
    uint32_t index, number;
};
using LoopFn = void (*)(uint8_t* self, TArr* arr, uint8_t flag);
using SizeFn = void (*)(uint8_t* self);
using FreeFn = void (*)(void* p);
using PeFn = void (*)(void* self, void* fn, void* params);
using CopyFn = void* (*)(void* dst, const void* src);
using ClearFn = void (*)(uint8_t* self);
using RehashFn = void (*)(void* set);
using FNameFn = NameId* (*)(NameId* out, const wchar_t* name, int findType);
LoopFn g_origLoop = nullptr;   // UpdateOrCreateMapIcons exe+0x493F910 (trampoline)
SizeFn g_origSize = nullptr;   // UpdateIconsSizeSmoothly exe+0x493E820 (trampoline)
ClearFn g_origClear = nullptr; // ClearAllMapIcons exe+0x49136A0 (trampoline)
FreeFn g_free = nullptr;       // FMemory::Free exe+0xE26500
CopyFn g_copyEntry = nullptr;  // FMapIconData copy constructor exe+0x490C1B0
RehashFn g_rehash = nullptr;   // the icon TSet's rehash exe+0x4929730 (what Add does when it grows)
FNameFn g_fname = nullptr;     // FName::FName(const TCHAR*, EFindName) exe+0xF3B8B0
uint8_t* g_processEvent = nullptr;  // UObject::ProcessEvent
uint32_t g_peSlotOff = 0;           // its vtable slot (0x268)
uint8_t* g_thunkReturn = nullptr;   // exe+0x46A79B7: the exec thunk's return address after its call of the loop
bool g_testAnyCaller = false;       // self-test: the loop's caller is not the exec thunk

// Our copy of a list: zeroed memory (ours), each entry built by the game's copy constructor.
TArr CopyList(const TArr& src)
{
    TArr a{};
    if (src.num <= 0) return a;
    a.data = static_cast<uint8_t*>(calloc(static_cast<size_t>(src.num), 0x50));
    if (!a.data) return a;
    for (int i = 0; i < src.num; ++i) g_copyEntry(a.data + static_cast<size_t>(i) * 0x50, src.data + static_cast<size_t>(i) * 0x50);
    a.num = a.max = src.num;
    return a;
}
// One entry's clean-up as the exec thunk does it for its own list copy (exe+0x46A79B7..0x46A7A2D): the FString's allocation
// (+0x40, FMemory) and the FText's shared reference (controller +0x20: shared count +8, weak count +0xC, DestroyObject
// vtable[0], DeleteThis vtable[1]).
void DestroyEntry(uint8_t* e)
{
    if (void* s = *reinterpret_cast<void**>(e + 0x40)) g_free(s);
    uint8_t* const ctrl = *reinterpret_cast<uint8_t**>(e + 0x20);
    if (ctrl && _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(ctrl + 8), -1) == 1) {
        void** const vt = *reinterpret_cast<void***>(ctrl);
        reinterpret_cast<void (*)(void*)>(vt[0])(ctrl);
        if (_InterlockedExchangeAdd(reinterpret_cast<volatile long*>(ctrl + 0xC), -1) == 1)
            reinterpret_cast<void (*)(void*, uint32_t)>(vt[1])(ctrl, 1);
    }
}
void DestroyList(TArr& a)
{
    for (int i = 0; i < a.num; ++i) DestroyEntry(a.data + static_cast<size_t>(i) * 0x50);
    free(a.data);
    a = TArr{};
}

// ---- kept work ----
struct Pending {
    uint8_t* self;
    TArr list;
    uint8_t flag;
    bool size;        // UpdateIconsSizeSmoothly came right after the list
    float zoom[3];    // its inputs then (+0x580, +0x630, +0x634)
};
constexpr int kMaxPending = 4;
Pending g_pend[kMaxPending];
int g_numPend = 0;
int g_inFlush = 0;
bool g_window = false;  // inside the frame's pairing run
struct Stats {
    uint64_t kept, dropped, droppedEntries, flushed, flushedEntries, late, sizeKept, sizeZoomRestored, notKept;
    uint64_t flushUs;
} g_st{};

int FindPend(const uint8_t* self)
{
    for (int i = 0; i < g_numPend; ++i)
        if (g_pend[i].self == self) return i;
    return -1;
}
Pending TakePend(int i)
{
    const Pending p = g_pend[i];
    for (int k = i; k + 1 < g_numPend; ++k) g_pend[k] = g_pend[k + 1];
    --g_numPend;
    return p;
}

void MarkBuilt(uint8_t* self, uint8_t flag);  // reuse (bit 4): this widget's icons were just built by a list (with this flag)
// Run kept work i in stock order: the list, then the recorded size pass with the inputs it had then.
void Flush(int i)
{
    Pending p = TakePend(i);
    ++g_inFlush;
    const uint64_t t0 = NowUs();
    g_origLoop(p.self, &p.list, p.flag);
    if (p.size) {
        float* const z[3] = {reinterpret_cast<float*>(p.self + kOffZoom), reinterpret_cast<float*>(p.self + kOffZoomMin),
                             reinterpret_cast<float*>(p.self + kOffZoomMax)};
        float now[3];
        bool changed = false;
        for (int k = 0; k < 3; ++k) {
            now[k] = *z[k];
            changed |= memcmp(&now[k], &p.zoom[k], 4) != 0;
        }
        if (changed) {
            ++g_st.sizeZoomRestored;
            for (int k = 0; k < 3; ++k) *z[k] = p.zoom[k];
        }
        g_origSize(p.self);
        if (changed)
            for (int k = 0; k < 3; ++k) *z[k] = now[k];
    }
    g_st.flushUs += NowUs() - t0;
    ++g_st.flushed;
    g_st.flushedEntries += static_cast<uint64_t>(p.list.num);
    MarkBuilt(p.self, p.flag);
    DestroyList(p.list);
    --g_inFlush;
}
void FlushAll()
{
    while (g_numPend) Flush(0);
}

// ---- reuse (bit 4) state ----
struct WState {
    uint8_t* self;
    bool clean;         // built by a list, nothing but quiet events since
    bool clearPending;  // a ClearAllMapIcons held
    uint8_t flag;       // the UpdateOrCreateMapIcons flag (!IsWorldMap) the icons were built with (M-S1)
};
WState g_ws[8] = {};
int g_numWs = 0;
WState* Ws(uint8_t* self, bool create)
{
    for (int i = 0; i < g_numWs; ++i)
        if (g_ws[i].self == self) return &g_ws[i];
    if (!create) return nullptr;
    if (g_numWs == 8) {  // forget the oldest (widgets of closed menus)
        for (int i = 1; i < 8; ++i) g_ws[i - 1] = g_ws[i];
        --g_numWs;
    }
    g_ws[g_numWs] = WState{self, false, false, 0};
    return &g_ws[g_numWs++];
}
struct ReuseStats {
    uint64_t held, reused, reusedEntries, heldRun, fbCount, fbHole, fbEntry, fbDiscover, fbGuard, fbFlag, propMismatch;
} g_rs{};
void RunClear(uint8_t* self)
{
    ++g_inFlush;
    g_origClear(self);
    --g_inFlush;
}
void RunHeldClears()
{
    for (int i = 0; i < g_numWs; ++i)
        if (g_ws[i].clearPending) {
            g_ws[i].clearPending = false;
            g_ws[i].clean = false;
            ++g_rs.heldRun;
            RunClear(g_ws[i].self);
        }
}
// Something other than a quiet event touches the map: held clears run, no icon set counts as untouched any more.
void Disturb()
{
    RunHeldClears();
    for (int i = 0; i < g_numWs; ++i) g_ws[i].clean = false;
}
void MarkBuilt(uint8_t* self, uint8_t flag)
{
    WState* const w = Ws(self, true);
    w->clean = true;
    w->flag = flag;
}
int EffMode() { return g_window ? g_runMode : g_mode.load(std::memory_order_relaxed); }
bool Outstanding()
{
    if (g_numPend) return true;
    for (int i = 0; i < g_numWs; ++i)
        if (g_ws[i].clearPending) return true;
    return false;
}
void DrainAll()
{
    RunHeldClears();
    FlushAll();
}

// ---- hooks ----
// Before-call notification from the stubs (the function's registers restored afterwards): the readers flush.
int g_idClearThunk = -1, g_idIconsThunk = -1, g_idSizeThunk = -1;  // table ids of the three natives the hooks decide on
void PreCall(uint8_t* self, int id, uint8_t* a2)
{
    (void)self;
    (void)a2;
    if (!IsGt()) return;
    const int mode = EffMode();
    if (!mode) {
        if (!g_inFlush && Outstanding()) DrainAll();
        return;
    }
    if (g_inFlush) return;  // inside a list, clear or size pass we run
    if (id == 11 || id == g_idClearThunk || id == g_idIconsThunk || id == g_idSizeThunk) return;  // the hooks below decide
    Disturb();
    if (g_numPend) FlushAll();
}

// ProcessEvent of the map widget class (its vtable slot): any event but "Update Icons" flushes first; the quiet events (their
// Blueprint code does not touch the icons: findings "mapspread" round 2) keep the icons clean for reuse.
constexpr const wchar_t* kUpdateIcons = L"Update Icons";
constexpr const wchar_t* kQuiet[] = {L"GetWidgetLayer",       L"IsNavigable",          L"BP_OnActivated",     L"OnFocus",
                                     L"OnUnfocus",            L"BP_OnDeactivated",     L"OnMapPositionChanged",
                                     L"OnMapInputMethodChanged", L"BP_GetDesiredFocusTarget", L"OnAddedToFocusPath",
                                     L"OnFocusReceived"};
constexpr int kNumQuiet = sizeof kQuiet / sizeof kQuiet[0];
NameId g_nameUpdate{}, g_nameQuiet[kNumQuiet]{};
bool g_namesResolved = false;
void** g_peSlot = nullptr;
PeFn g_origPe = nullptr;
const void* g_peFns[64] = {};
uint8_t g_peKind[64] = {};  // 1 "Update Icons", 2 quiet, 0 other
int g_numPe = 0;
bool SameName(const NameId& a, const NameId& b) { return a.index && a.index == b.index && a.number == b.number; }
// A UFunction's kind by its FName (UObject +0x18) against the names resolved with FNAME_Find (never added to the pool).
uint8_t KindOf(const void* fn)
{
    NameId n{};
    __try {
        memcpy(&n, static_cast<const uint8_t*>(fn) + kObjName, sizeof n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (SameName(n, g_nameUpdate)) return 1;
    for (const NameId& q : g_nameQuiet)
        if (SameName(n, q)) return 2;
    return 0;
}
void PeHook(void* self, void* fn, void* params)
{
    if (IsGt() && !EffMode()) {
        if (!g_inFlush && Outstanding()) DrainAll();
    } else if (IsGt()) {
        int k = 0;
        while (k < g_numPe && g_peFns[k] != fn) ++k;
        if (k == g_numPe && k < 64) {
            g_peFns[k] = fn;
            g_peKind[k] = KindOf(fn);
            ++g_numPe;
        }
        const uint8_t kind = k < 64 ? g_peKind[k] : KindOf(fn);
        if (!g_inFlush && kind == 0) Disturb();
        if (g_numPend && !g_inFlush && kind != 1) FlushAll();
    }
    g_origPe(self, fn, params);
}
// First map widget seen (game thread): its class's ProcessEvent slot, checked to hold ProcessEvent (M-S2), and the names.
bool g_peFailed = false;
void PatchPe(uint8_t* self)
{
    if (g_peSlot || g_peFailed) return;
    if (!g_namesResolved) {
        g_namesResolved = true;
        g_fname(&g_nameUpdate, kUpdateIcons, 0);  // FNAME_Find: a name not in the pool gives None (never matches)
        for (int i = 0; i < kNumQuiet; ++i) g_fname(&g_nameQuiet[i], kQuiet[i], 0);
    }
    // The one slot of the widget's vtable (first 0x800 bytes) that holds ProcessEvent; it must be the slot the code calls.
    void** slot = nullptr;
    __try {
        void** const vt = *reinterpret_cast<void***>(self);
        int hits = 0;
        for (uint32_t i = 0; i < 0x800 / 8; ++i)
            if (vt[i] == g_processEvent) {
                slot = vt + i;
                ++hits;
            }
        if (hits != 1 || (g_peSlotOff && slot != vt + g_peSlotOff / 8)) slot = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot = nullptr;
    }
    if (!slot) {
        g_peFailed = true;  // another mod's hook there, or not ProcessEvent: events then flush as unknown natives can't be seen
        Log("[mapspread] MapPopupSpread: the map widget's ProcessEvent slot does not hold ProcessEvent: switched off");
        g_mode = 0;
        g_runMode = 0;
        return;
    }
    g_origPe = reinterpret_cast<PeFn>(*slot);
    if (!WritePointer(slot, reinterpret_cast<void*>(&PeHook))) {
        g_peFailed = true;
        g_mode = 0;
        g_runMode = 0;
        Log("[mapspread] MapPopupSpread: the ProcessEvent slot could not be written: switched off");
        return;
    }
    g_peSlot = slot;
    Log("[mapspread] ProcessEvent of the map widget class hooked (vtable slot +0x%llX at exe+0x%llX; \"Update Icons\" %s, %d quiet "
        "events known)", static_cast<unsigned long long>((reinterpret_cast<uint8_t*>(slot) - *reinterpret_cast<uint8_t**>(self))),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(slot) - Game().base),
        g_nameUpdate.index ? "known" : "NOT known (no drop)", [] {
            int n = 0;
            for (const NameId& q : g_nameQuiet) n += q.index != 0;
            return n;
        }());
}

// ---- reuse ----
const wchar_t* FStr(const uint8_t* fstring, int* len)
{
    const wchar_t* const s = *reinterpret_cast<const wchar_t* const*>(fstring);
    *len = *reinterpret_cast<const int*>(fstring + 8);
    return s;
}
bool StrEq(const uint8_t* a, const uint8_t* b)  // two FStrings
{
    int la, lb;
    const wchar_t* const sa = FStr(a, &la);
    const wchar_t* const sb = FStr(b, &lb);
    if (la <= 1 && lb <= 1) return true;  // empty (Num 0, or just the terminator)
    return la == lb && sa && sb && memcmp(sa, sb, static_cast<size_t>(la) * 2) == 0;
}
bool TextEq(const uint8_t* a, const uint8_t* b)  // two FTexts by their display strings (ITextData vtable [2])
{
    const uint8_t* const ta = *reinterpret_cast<const uint8_t* const*>(a);
    const uint8_t* const tb = *reinterpret_cast<const uint8_t* const*>(b);
    if (ta == tb) return true;
    if (!ta || !tb) return false;
    using GetStr = const uint8_t* (*)(const void*);
    const uint8_t* const sa = reinterpret_cast<GetStr>((*reinterpret_cast<void* const* const*>(ta))[2])(ta);
    const uint8_t* const sb = reinterpret_cast<GetStr>((*reinterpret_cast<void* const* const*>(tb))[2])(tb);
    return sa && sb && StrEq(sa, sb);
}
// equal for reuse: every FMapIconData value but IconAddress (+0x40) and the FText instance
bool EntryEq(const uint8_t* a, const uint8_t* b)
{
    return memcmp(a, b, 0x18) == 0 && memcmp(a + 0x28, b + 0x28, 4) == 0 && memcmp(a + 0x30, b + 0x30, 2) == 0 &&
           memcmp(a + 0x34, b + 0x34, 6) == 0 && memcmp(a + 0x3C, b + 0x3C, 4) == 0 && TextEq(a + 0x18, b + 0x18);
}
void ReplaceEntry(uint8_t* dst, const uint8_t* src)
{
    DestroyEntry(dst);
    memset(dst, 0, 0x50);
    g_copyEntry(dst, src);
}
// Field offsets of the entry copies, found per class by matching a unique copy and then checked on every object before use:
// the icon class's Properties (WBP_Modern_MapIcon_C: +0x3A8) and the map class's PlayerIcon (WBP_Modern_MapWidget_C: +0x828).
struct OffCache {
    const void* cls;
    int off;  // > 0 found, -1 not found
};
OffCache g_propCache[4] = {}, g_playerCache[4] = {};
int FindCopy(const uint8_t* obj, int from, int to, const uint8_t* key)
{
    int found = 0;
    for (int o = from; o + 0x50 <= to; o += 8)
        if (EntryEq(obj + o, key) && StrEq(obj + o + 0x40, key + 0x40)) {
            if (found) return -1;  // not unique
            found = o;
        }
    return found;
}
// a copy of key at obj + off: every value, the IconAddress text and the FText's shared object and controller
bool IsCopyAt(const uint8_t* obj, int off, const uint8_t* key)
{
    const uint8_t* const c = obj + off;
    return EntryEq(c, key) && StrEq(c + 0x40, key + 0x40) && memcmp(c + 0x18, key + 0x18, 0x10) == 0;
}
int CachedOff(OffCache* cache, const void* cls, const uint8_t* obj, int from, int to, const uint8_t* key, const char* what)
{
    for (int i = 0; i < 4; ++i)
        if (cache[i].cls == cls) return cache[i].off;
    int o = FindCopy(obj, from, to, key);
    if (o <= 0) o = -1;
    int slot = 0;
    while (slot < 3 && cache[slot].cls) ++slot;
    cache[slot] = OffCache{cls, o};
    Log("[mapspread] reuse: %s of class %p %s +0x%X", what, cls, o > 0 ? "at" : "NOT FOUND (no reuse for this class), searched to",
        o > 0 ? o : to);
    return o;
}
bool ReuseGuards(const uint8_t* self)
{
    const int n = IconCount(self);
    if (n <= 0) return false;
    if (*reinterpret_cast<const int*>(self + kOffMap + 0x34) != 0) return false;  // holes
    if (*reinterpret_cast<void* const*>(self + kOffHover)) return false;           // a hovered icon
    const uint8_t* const f = *reinterpret_cast<const uint8_t* const*>(self + kOffFilter);
    if (f && *reinterpret_cast<const int*>(f + kFilterMask) != 0x7FFF) return false;  // a filter active
    return true;
}
// The list against the icons; if equal, the new entries go in place. True = reused (the caller skips clear and create).
// Two passes: every check (entries, each icon's Properties copy, the PlayerIcon copy) before the first write.
int g_offs[4096];  // game thread only (pass 1 -> pass 2)
bool TryReuse(uint8_t* self, const TArr* arr)
{
    __try {
        const int n = arr->num;
        if (n <= 0 || n > 4096 || n != IconCount(self) || *reinterpret_cast<const int*>(self + kOffMap + 8) != n) {
            ++g_rs.fbCount;
            return false;
        }
        uint8_t* const data = *reinterpret_cast<uint8_t* const*>(self + kOffMap);
        int player = -1;
        for (int i = 0; i < n; ++i) {
            const uint8_t* const key = data + i * 0x60;
            if (!SparseAllocated(self, kOffMap + 0x10, kOffMap + 0x20, i) || !*reinterpret_cast<void* const*>(key + 0x50)) {
                ++g_rs.fbHole;
                return false;
            }
            if (!EntryEq(key, arr->data + static_cast<size_t>(i) * 0x50)) {
                ++g_rs.fbEntry;
                return false;
            }
            if (key[0x30] == 14) {
                if (player >= 0) {  // two player entries: leave it to stock
                    ++g_rs.fbDiscover;
                    return false;
                }
                player = i;
            }
        }
        // pass 1: every icon's Properties must be a copy of its key at its class's offset
        for (int i = 0; i < n; ++i) {
            const uint8_t* const key = data + i * 0x60;
            const uint8_t* const w = *reinterpret_cast<const uint8_t* const*>(key + 0x50);
            const void* const cls = *reinterpret_cast<const void* const*>(w + kObjClass);
            const int o = CachedOff(g_propCache, cls, w, 0x200, 0x900, key, "icon Properties");
            if (o <= 0 || !IsCopyAt(w, o, key)) {
                ++g_rs.propMismatch;
                return false;
            }
            g_offs[i] = o;
        }
        int playerOff = 0;
        if (player >= 0) {
            const uint8_t* const pkey = data + player * 0x60;
            playerOff = CachedOff(g_playerCache, *reinterpret_cast<const void* const*>(self + kObjClass), self, 0x640, 0xC00, pkey, "PlayerIcon");
            if (playerOff <= 0 || !IsCopyAt(self, playerOff, pkey)) {
                ++g_rs.fbDiscover;
                return false;
            }
        }
        // pass 2: the writes, then the set's buckets from the new key bytes (as Add's rehash does)
        for (int i = 0; i < n; ++i) {
            uint8_t* const key = data + i * 0x60;
            uint8_t* const w = *reinterpret_cast<uint8_t* const*>(key + 0x50);
            const uint8_t* const src = arr->data + static_cast<size_t>(i) * 0x50;
            ReplaceEntry(w + g_offs[i], src);
            ReplaceEntry(key, src);
            if (i == player) ReplaceEntry(self + playerOff, src);
        }
        g_rehash(self + kOffMap);
        ++g_rs.reused;
        g_rs.reusedEntries += static_cast<uint64_t>(n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_rs.fbDiscover;
        return false;
    }
}

void ClearHook(uint8_t* self)
{
    if (!IsGt() || g_inFlush) return g_origClear(self);
    const int mode = EffMode();
    if (!mode) {
        if (Outstanding()) DrainAll();  // stock order: kept work first
        return g_origClear(self);
    }
    const int i = FindPend(self);
    if (i >= 0) {  // kept work for this widget is removed unseen
        Pending p = TakePend(i);
        ++g_st.dropped;
        g_st.droppedEntries += static_cast<uint64_t>(p.list.num);
        DestroyList(p.list);
    }
    WState* const w = Ws(self, true);
    if (w->clearPending) {  // a second clear: the held one first
        w->clearPending = false;
        ++g_rs.heldRun;
        RunClear(self);
    }
    if ((mode & 4) && g_window && w->clean && ReuseGuards(self)) {
        w->clearPending = true;
        ++g_rs.held;
        return;
    }
    if ((mode & 4) && w->clean) ++g_rs.fbGuard;
    w->clean = false;
    g_origClear(self);
}

void LoopHook(uint8_t* self, TArr* arr, uint8_t flag)
{
    if (!IsGt() || g_inFlush) return g_origLoop(self, arr, flag);
    const int mode = EffMode();
    if (!mode) {
        if (Outstanding()) DrainAll();  // a held clear runs before this list, kept lists in their order
        return g_origLoop(self, arr, flag);
    }
    if (!g_testAnyCaller) {
        PatchPe(self);
        if (!g_mode.load(std::memory_order_relaxed)) {  // switched off by PatchPe: stock from here on
            DrainAll();
            return g_origLoop(self, arr, flag);
        }
    }
    // only Blueprint's call through the exec thunk (the only caller in this build)
    const bool thunkCopy = g_testAnyCaller || static_cast<uint8_t*>(_ReturnAddress()) == g_thunkReturn;
    if (WState* const ws = Ws(self, false); ws && ws->clearPending) {
        ws->clearPending = false;
        if ((mode & 4) && thunkCopy) {
            if (ws->flag != flag) ++g_rs.fbFlag;  // M-S1: icons made for the other kind of map are never kept
            else if (TryReuse(self, arr)) return;  // the icons stay (clean), with the new entries
        }
        ++g_rs.heldRun;
        ws->clean = false;
        RunClear(self);
    }
    const int at = FindPend(self);
    if (at >= 0) Flush(at);  // stock order: the earlier list first
    if ((mode & 1) && g_window && thunkCopy && arr->num > 0) {
        if (g_numPend == kMaxPending) FlushAll();
        Pending& p = g_pend[g_numPend++];
        p = Pending{};
        p.self = self;
        p.list = CopyList(*arr);
        p.flag = flag;
        if (!p.list.data) {  // out of memory: stock
            --g_numPend;
            g_origLoop(self, arr, flag);
            return;
        }
        ++g_st.kept;
        return;
    }
    if (mode & 1) ++g_st.notKept;
    ++g_inFlush;
    g_origLoop(self, arr, flag);
    --g_inFlush;
    MarkBuilt(self, flag);
}

void SizeHook(uint8_t* self)
{
    const int mode = IsGt() && !g_inFlush ? EffMode() : 0;
    if (!mode && IsGt() && !g_inFlush && Outstanding()) DrainAll();
    if (mode) {
        if (WState* const ws = Ws(self, false); ws && ws->clearPending) {
            ws->clearPending = false;
            ws->clean = false;
            ++g_rs.heldRun;
            RunClear(self);
        }
        const int i = FindPend(self);
        if (i >= 0 && !g_pend[i].size && IconCount(self) == 0) {  // only the kept list's icons would be sized
            Pending& p = g_pend[i];
            p.size = true;
            memcpy(&p.zoom[0], self + kOffZoom, 4);
            memcpy(&p.zoom[1], self + kOffZoomMin, 4);
            memcpy(&p.zoom[2], self + kOffZoomMax, 4);
            ++g_st.sizeKept;
            return;
        }
        if (i >= 0) Flush(i);
    }
    g_origSize(self);
}

// ---- stubs ----
// Notification stub: save rcx/rdx/r8/r9 + xmm0-3, PreCall(rcx, id, rdx), restore, jmp target.
size_t BuildNotifyStub(uint8_t* p, int id, const void* target)
{
    size_t n = 0;
    auto put = [&](std::initializer_list<uint8_t> b) {
        for (uint8_t x : b) p[n++] = x;
    };
    put({0x51, 0x52, 0x41, 0x50, 0x41, 0x51});  // push rcx, rdx, r8, r9
    put({0x48, 0x83, 0xEC, 0x68});              // sub rsp, 0x68 (16-byte aligned call, 0x20 home space)
    put({0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20});  // movdqu [rsp+20h], xmm0
    put({0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30});  // xmm1
    put({0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40});  // xmm2
    put({0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50});  // xmm3
    put({0x4C, 0x8B, 0xC2});                    // mov r8, rdx
    put({0xBA});                                // mov edx, id
    memcpy(p + n, &id, 4);
    n += 4;
    put({0x48, 0xB8});  // mov rax, PreCall
    const void* cb = reinterpret_cast<const void*>(&PreCall);
    memcpy(p + n, &cb, 8);
    n += 8;
    put({0xFF, 0xD0});  // call rax
    put({0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20});
    put({0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30});
    put({0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40});
    put({0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50});
    put({0x48, 0x83, 0xC4, 0x68});              // add rsp, 0x68
    put({0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});  // pop r9, r8, rdx, rcx
    put({0xFF, 0x25, 0, 0, 0, 0});              // jmp [rip]
    memcpy(p + n, &target, 8);
    n += 8;
    return n;
}
size_t BuildAbsJmp(uint8_t* p, const void* target)
{
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(p, jmpAbs, 6);
    memcpy(p + 6, &target, 8);
    return 14;
}
// Trampoline: the function's first len bytes (whole instructions, no RIP-relative operand), then jmp back to fn + len.
size_t BuildTrampoline(uint8_t* p, uint8_t* fn, size_t len)
{
    memcpy(p, fn, len);
    return len + BuildAbsJmp(p + len, fn + len);
}
bool PatchJmp(uint8_t* at, size_t len, const uint8_t* to)
{
    const int64_t rel = to - (at + 5);
    if (rel != static_cast<int32_t>(rel) || len < 5 || len > 16) return false;
    uint8_t code[16];
    memset(code, 0x90, sizeof code);
    code[0] = 0xE9;
    const int32_t r = static_cast<int32_t>(rel);
    memcpy(code + 1, &r, 4);
    return WriteCode(at, std::span<const uint8_t>(code, len));
}
bool PatchCall(uint8_t* call, const uint8_t* to)
{
    const int64_t nrel = to - (call + 5);
    if (nrel != static_cast<int32_t>(nrel)) return false;
    const int32_t r = static_cast<int32_t>(nrel);
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&r), 4));
}

// ---- discovery (patterns; rev130f's table) ----
// Native tables: the three classes' {const char* name, FNativeFuncPtr} arrays (FNameNativePtrPair), found by their first
// entry's name (one copy in .rdata). Other arrays point at the same string: the {construct, name} link info read 8 bytes
// shifted (its "code" is a Z_Construct_UFunction: sub rsp,28h; mov rax,[static]) and a .data copy with no code; the native
// table is the one whose code is an exec thunk; its end is the first entry that is not {identifier in .rdata, code}; the
// entry before its start must not be one either.
constexpr char kConstructPro[] = "48 83 EC 28 48 8B 05";
struct NativeTable {
    uint8_t* at = nullptr;
    int count = 0;
};
bool IdentifierAt(const GameModule& game, const char* s)
{
    if (!s || !game.rdata.Contains(reinterpret_cast<const uint8_t*>(s))) return false;
    int n = 0;
    for (; n < 100 && game.rdata.Contains(reinterpret_cast<const uint8_t*>(s + n)) && s[n]; ++n) {
        const char c = s[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return n > 0 && n < 100 && !s[n];
}
bool PairAt(const GameModule& game, const uint8_t* q)
{
    const char* const name = *reinterpret_cast<const char* const*>(q);
    uint8_t* const code = *reinterpret_cast<uint8_t* const*>(q + 8);
    return game.text.Contains(code) && IdentifierAt(game, name);
}
NativeTable FindNativeTable(const GameModule& game, const char* first)
{
    NativeTable t;
    const size_t len = strlen(first);
    const uint8_t* str = nullptr;
    int copies = 0;
    for (const uint8_t* p = game.rdata.begin + 1; p + len + 1 <= game.rdata.end; ++p)
        if (p[-1] == 0 && p[len] == 0 && memcmp(p, first, len) == 0) {
            str = p;
            ++copies;
        }
    if (copies != 1) return t;
    int found = 0;
    for (const Section* sec : {&game.rdata, &game.data})
        for (uint8_t* q = sec->begin; q + 16 <= sec->end; q += 8) {
            if (*reinterpret_cast<const uint8_t* const*>(q) != str) continue;
            uint8_t* const code = *reinterpret_cast<uint8_t* const*>(q + 8);
            if (!game.text.Contains(code) || MatchesAt(code, ParsePattern(kConstructPro))) continue;
            t.at = q;
            ++found;
        }
    if (found != 1 || !game.rdata.Contains(t.at) || PairAt(game, t.at - 16)) return NativeTable{};
    while (t.count < 256 && game.rdata.Contains(t.at + t.count * 16 + 16) && PairAt(game, t.at + t.count * 16)) ++t.count;
    return t;
}
uint8_t* ThunkIn(const NativeTable& t, const char* name)
{
    for (int i = 0; i < t.count; ++i) {
        const char* const n = *reinterpret_cast<const char* const*>(t.at + i * 16);
        if (strcmp(n, name) == 0) return *reinterpret_cast<uint8_t* const*>(t.at + i * 16 + 8);
    }
    return nullptr;
}

// The detoured entries' first bytes (checked at the derived addresses; the trampolines copy whole instructions).
constexpr char kLoopBytes[] = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 1A";
constexpr char kSizeBytes[] = "40 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC 00 01 00 00 45 33 C0";
constexpr char kClearBytes[] = "48 8B C4 55 56 48 8D 68 A1 48 81 EC 98 00 00 00";
constexpr size_t kLoopCopy = 5, kSizeCopy = 6, kClearCopy = 5;
// Entries with a notification stub: id (as PreCall sees it), first bytes, trampoline length, what
struct Entry {
    int id;
    const char* bytes;
    size_t copy;
    const char* what;
};
const Entry kEntries[] = {
    {2, "40 55 57 48 8D 6C 24 B1 48 81 EC D8 00 00 00 48 8B 81 08 06 00 00", 8, "hover finder"},  // +0x608: the hovered icon
    {3, "40 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC 00 01 00 00 4C 8D B1 90 05 00 00", 6, "UpdateIconsSizeOnMapSetup"},  // +0x590
    {4, "40 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC 00 01 00 00 4C 8D B1 90 05 00 00", 6, "UpdateIconsSizeOnMapSwitch"},
    {6, "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 55 41 57", 5, "GetPlayerPosition"},
    {7, "48 89 5C 24 08 57 48 83 EC 20 48 8B 81 28 06 00 00", 5, "UpdatePlayerMarkerPosition"},
    {10, "48 8B C4 53 48 81 EC A0 00 00 00", 11, "IsIconInLeftHalfOfView"},
    {11, "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30", 5, "UpdateOrCreateMapIcons exec thunk"},
};
constexpr int kNumEntries = sizeof kEntries / sizeof kEntries[0];
// ClearAllMapIcons' exec thunk: Stack.Code += !!Stack.Code (no parameters); jmp ClearAllMapIcons
constexpr char kClearThunk[] = "48 8B 42 20 45 33 C0 48 85 C0 41 0F 95 C0 4C 03 C0 4C 89 42 20 E9";
constexpr size_t kClearThunkJmp = 0x15;
// UpdateOrCreateMapIcons' exec thunk: call UpdateOrCreateMapIcons at +0xD2 (its return +0xD7 starts the list clean-up),
// FMemory::Free at +0xF9
constexpr char kThunkCallBytes[] = "E8 ?? ?? ?? ?? 8B 74 24 28 85 F6 74 64 48 8B 7C 24 20 48 83 C7 20";
constexpr size_t kIconsThunkCall = 0xD2, kIconsThunkFree = 0xF9;
constexpr char kFreeBytes[] = "48 85 C9 74 2E 53 48 83 EC 20 48 8B D9";
constexpr size_t kSizeThunkCall = 0x72, kPlayerPosCall = 0x23, kMarkerCall = 0x79, kLeftHalfCall = 0x81;
constexpr size_t kSingleCall = 0x131, kIconPosCall = 0xBD;  // UpdateOrCreateMapIcon / UpdateMapIconPosition in their thunks
// The FMapIconData copy constructor, called in UpdateIconsSizeSmoothly at +0x1E7 (and in OnMapSetup at +0x1F7); the zoom
// reads right after it pin +0x630 / +0x580 / +0x634; the map +0x590 (lea r15,[rcx+590h]) at +0x1F.
constexpr char kSizeCopyZoom[] =
    "E8 ?? ?? ?? ?? 48 8B 5B 50 48 89 5D 17 48 85 DB 74 ?? F3 41 0F 10 8E 30 06 00 00 F3 41 0F 10 B6 80 05 00 00 F3 41 0F 10 86 "
    "34 06 00 00";
constexpr size_t kSizeCopyAt = 0x1E7, kSetupCopyAt = 0x1F7, kSizeMapAt = 0x1F;
constexpr char kSizeMap[] = "4C 8D B9 90 05 00 00";
constexpr char kCopyBytes[] = "40 53 48 83 EC 20 0F 10 02 48 8B D9 0F 11 01 48 8B 42 10 48 89 41 10";
// UpdateOrCreateMapIcons reads the filter at +0x58: mov rax,[rdi+4D8h]
constexpr char kLoopFilter[] = "48 8B 87 D8 04 00 00";
constexpr size_t kLoopFilterAt = 0x58;
// ClearAllMapIcons: the filter's reset (mov rcx,[rsi+4D8h]; mov [rcx+5F0h],0; mov [rcx+598h],7FFFh) and the icon set's
// rehash call (mov [rdi+48h],1; call rehash)
constexpr char kClearFilter[] = "48 8B 8E D8 04 00 00 C7 81 F0 05 00 00 00 00 00 00 C7 81 98 05 00 00 FF 7F 00 00";
constexpr char kClearRehash[] = "C7 47 48 01 00 00 00 E8";
constexpr size_t kClearSpan = 0x400;
constexpr char kRehashBytes[] = "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8B EC 48 83 EC 70 48 8B F9";
// ProcessEvent's vtable slot: the timer manager's dynamic-delegate call (mov r9,[rbx]; ...; call [r9+slot]; mov edi,r14d)
constexpr char kPeCall[] = "4C 8B 0B 45 33 C0 48 8B D0 48 8B CB 41 FF 91 ?? ?? 00 00 41 8B FE";
constexpr size_t kPeCallSlot = 0x0F;
// FName::FName(const TCHAR*, EFindName) (exe+0xF3B8B0, as HdrMenuRows finds it)
constexpr char kFNameCtor[] =
    "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 48 89 54 24 20 33 C9 41 8B F8 4C 8B D2 44 8B C9 48 85 D2 74 ?? 0F B7 02 "
    "66 85 C0 74 ?? 66 0F 1F 44 00 00 48 83 C2 02 0F B7 C0 44 0B C8 0F B7 02 66 85 C0 75 ?? 41 F7 C1 80 FF FF FF "
    "0F 95 44 24 2C";

struct Sites {
    NativeTable tables[3];
    uint8_t* loop = nullptr, *size = nullptr, *clear = nullptr;
    uint8_t* entries[kNumEntries] = {};
    uint8_t* calls[2] = {};  // the single-icon and icon-position calls (in their thunks)
    uint8_t* free = nullptr, *copy = nullptr, *rehash = nullptr, *thunkReturn = nullptr, *fname = nullptr, *pe = nullptr;
    uint32_t peSlot = 0;
    const char* missing = nullptr;
};
uint8_t* CallTarget(const GameModule& game, uint8_t* thunk, size_t at)
{
    if (!thunk || !game.text.Contains(thunk + at + 5) || thunk[at] != 0xE8) return nullptr;
    uint8_t* const t = RipTarget(thunk + at, 1, 5);
    return game.text.Contains(t) ? t : nullptr;
}
bool Discover(const GameModule& game, Sites* s)
{
    auto miss = [&](const char* what) {
        s->missing = what;
        return false;
    };
    const char* const firsts[3] = {"AbsoluteSpaceToMapSpace", "ActivateControlFilter", "SetIconProperties"};
    for (int i = 0; i < 3; ++i) {
        s->tables[i] = FindNativeTable(game, firsts[i]);
        if (!s->tables[i].count) return miss(firsts[i]);
    }
    const NativeTable& map = s->tables[0];
    // ClearAllMapIcons
    uint8_t* const clearThunk = ThunkIn(map, "ClearAllMapIcons");
    if (!clearThunk || !MatchesAt(clearThunk, ParsePattern(kClearThunk))) return miss("ClearAllMapIcons' exec thunk");
    s->clear = RipTarget(clearThunk + kClearThunkJmp, 1, 5);
    if (!game.text.Contains(s->clear + kClearSpan) || !MatchesAt(s->clear, ParsePattern(kClearBytes))) return miss("ClearAllMapIcons");
    const Section clearBody{s->clear, s->clear + kClearSpan};
    if (FindAll(clearBody, ParsePattern(kClearFilter)).size() != 1) return miss("ClearAllMapIcons' filter reset (+0x4D8, +0x598)");
    const auto rh = FindAll(clearBody, ParsePattern(kClearRehash));
    if (rh.size() != 1) return miss("ClearAllMapIcons' rehash call");
    s->rehash = CallTarget(game, rh[0], 7);
    if (!s->rehash || !MatchesAt(s->rehash, ParsePattern(kRehashBytes))) return miss("the icon set's rehash");
    // UpdateOrCreateMapIcons through its exec thunk (entry 11)
    uint8_t* const iconsThunk = ThunkIn(map, "UpdateOrCreateMapIcons");
    if (!iconsThunk || !game.text.Contains(iconsThunk + 0x100) || !MatchesAt(iconsThunk + kIconsThunkCall, ParsePattern(kThunkCallBytes)))
        return miss("UpdateOrCreateMapIcons' exec thunk");
    s->loop = CallTarget(game, iconsThunk, kIconsThunkCall);
    s->thunkReturn = iconsThunk + kIconsThunkCall + 5;
    s->free = CallTarget(game, iconsThunk, kIconsThunkFree);
    if (!s->loop || !MatchesAt(s->loop, ParsePattern(kLoopBytes)) || !MatchesAt(s->loop + kLoopFilterAt, ParsePattern(kLoopFilter)))
        return miss("UpdateOrCreateMapIcons");
    if (!s->free || !MatchesAt(s->free, ParsePattern(kFreeBytes))) return miss("FMemory::Free");
    // the size passes
    s->size = CallTarget(game, ThunkIn(map, "UpdateIconsSizeSmoothly"), kSizeThunkCall);
    if (!s->size || !MatchesAt(s->size, ParsePattern(kSizeBytes)) || !MatchesAt(s->size + kSizeMapAt, ParsePattern(kSizeMap)) ||
        !MatchesAt(s->size + kSizeCopyAt, ParsePattern(kSizeCopyZoom)))
        return miss("UpdateIconsSizeSmoothly (+0x590, zoom +0x580 / +0x630 / +0x634)");
    uint8_t* const setup = CallTarget(game, ThunkIn(map, "UpdateIconsSizeOnMapSetup"), kSizeThunkCall);
    uint8_t* const sw = CallTarget(game, ThunkIn(map, "UpdateIconsSizeOnMapSwitch"), kSizeThunkCall);
    s->copy = CallTarget(game, s->size, kSizeCopyAt);
    if (!setup || !sw || !s->copy || CallTarget(game, setup, kSetupCopyAt) != s->copy || !MatchesAt(s->copy, ParsePattern(kCopyBytes)))
        return miss("the FMapIconData copy constructor");
    // the readers (notification stubs)
    const auto hovers = FindAll(game.text, ParsePattern(kEntries[0].bytes));
    uint8_t* const found[kNumEntries] = {
        hovers.size() == 1 ? hovers[0] : nullptr, setup, sw, CallTarget(game, ThunkIn(map, "GetPlayerPosition"), kPlayerPosCall),
        CallTarget(game, ThunkIn(map, "UpdatePlayerMarkerPosition"), kMarkerCall),
        CallTarget(game, ThunkIn(map, "IsIconInLeftHalfOfView"), kLeftHalfCall), iconsThunk};
    for (int i = 0; i < kNumEntries; ++i) {
        if (!found[i] || !MatchesAt(found[i], ParsePattern(kEntries[i].bytes))) return miss(kEntries[i].what);
        s->entries[i] = found[i];
    }
    uint8_t* const single = ThunkIn(map, "UpdateOrCreateMapIcon");
    uint8_t* const iconPos = ThunkIn(map, "UpdateMapIconPosition");
    if (!CallTarget(game, single, kSingleCall)) return miss("UpdateOrCreateMapIcon's call");
    if (!CallTarget(game, iconPos, kIconPosCall)) return miss("UpdateMapIconPosition's call");
    s->calls[0] = single + kSingleCall;
    s->calls[1] = iconPos + kIconPosCall;
    // ProcessEvent: the function by its prologue; its slot from a call in code when that call is unpatched (HitchLedger's tick
    // split may have rewritten it), else 0; PatchPe finds the slot in the map widget's own vtable and checks it against this.
    const auto pes = FindAll(game.text, ParsePattern(kPeCall));
    if (pes.size() == 1) {
        memcpy(&s->peSlot, pes[0] + kPeCallSlot, 4);
        if (s->peSlot == 0 || s->peSlot > 0x1000 || (s->peSlot & 7)) return miss("ProcessEvent's vtable slot");
    }
    s->pe = FindProcessEvent();
    if (!s->pe) return miss("ProcessEvent");
    const auto fn = FindAll(game.text, ParsePattern(kFNameCtor));
    if (fn.size() != 1) return miss("FName's constructor");
    s->fname = fn[0];
    return true;
}

int PatchTables(const Sites& s)
{
    int n = 0, id = 100;
    for (const NativeTable& t : s.tables) {
        uint8_t* const stubs = AllocNearGame(static_cast<size_t>(t.count) * 128);
        if (!stubs) return n;
        for (int i = 0; i < t.count; ++i, ++id) {
            void** const pair = reinterpret_cast<void**>(t.at + i * 16);
            const char* const name = static_cast<const char*>(pair[0]);
            if (&t == &s.tables[0]) {
                if (strcmp(name, "ClearAllMapIcons") == 0) g_idClearThunk = id;
                if (strcmp(name, "UpdateOrCreateMapIcons") == 0) g_idIconsThunk = id;
                if (strcmp(name, "UpdateIconsSizeSmoothly") == 0) g_idSizeThunk = id;
            }
            BuildNotifyStub(stubs + i * 128, id, pair[1]);
        }
        DWORD old;
        VirtualProtect(stubs, static_cast<size_t>(t.count) * 128, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), stubs, static_cast<size_t>(t.count) * 128);
        for (int i = 0; i < t.count; ++i) {
            void** const pair = reinterpret_cast<void**>(t.at + i * 16);
            if (WritePointer(pair + 1, stubs + i * 128)) ++n;
        }
    }
    return n;
}

bool g_installed = false;
bool InstallOn(const GameModule& game, const Sites& s)
{
    constexpr size_t kSlot = 192;
    constexpr size_t kN = 3 + kNumEntries + 2;
    uint8_t* const region = AllocNearGame(kN * kSlot);
    if (!region) {
        Log("[mapspread] MapPopupSpread: no memory near the game's code: not installed");
        return false;
    }
    g_free = reinterpret_cast<FreeFn>(s.free);
    g_copyEntry = reinterpret_cast<CopyFn>(s.copy);
    g_rehash = reinterpret_cast<RehashFn>(s.rehash);
    g_fname = reinterpret_cast<FNameFn>(s.fname);
    g_processEvent = s.pe;
    g_peSlotOff = s.peSlot;
    g_thunkReturn = s.thunkReturn;
    // build every stub first, then make the region executable, then patch
    uint8_t* const loopS = region;
    uint8_t* const sizeS = region + kSlot;
    uint8_t* const clearS = region + 2 * kSlot;
    BuildTrampoline(loopS, s.loop, kLoopCopy);
    BuildAbsJmp(loopS + 64, reinterpret_cast<const void*>(&LoopHook));
    BuildTrampoline(sizeS, s.size, kSizeCopy);
    BuildAbsJmp(sizeS + 64, reinterpret_cast<const void*>(&SizeHook));
    BuildTrampoline(clearS, s.clear, kClearCopy);
    BuildAbsJmp(clearS + 64, reinterpret_cast<const void*>(&ClearHook));
    for (int i = 0; i < kNumEntries; ++i) {
        uint8_t* const p = region + (3 + i) * kSlot;
        BuildTrampoline(p, s.entries[i], kEntries[i].copy);
        BuildNotifyStub(p + 48, kEntries[i].id, p);
    }
    const int callIds[2] = {9, 8};
    for (int c = 0; c < 2; ++c) BuildNotifyStub(region + (3 + kNumEntries + c) * kSlot, callIds[c], RipTarget(s.calls[c], 1, 5));
    DWORD old;
    VirtualProtect(region, kN * kSlot, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, kN * kSlot);
    g_origLoop = reinterpret_cast<LoopFn>(loopS);
    g_origSize = reinterpret_cast<SizeFn>(sizeS);
    g_origClear = reinterpret_cast<ClearFn>(clearS);
    int ok = 0;
    ok += PatchJmp(s.loop, kLoopCopy, loopS + 64);
    ok += PatchJmp(s.size, kSizeCopy, sizeS + 64);
    ok += PatchJmp(s.clear, kClearCopy, clearS + 64);
    for (int i = 0; i < kNumEntries; ++i) ok += PatchJmp(s.entries[i], kEntries[i].copy, region + (3 + i) * kSlot + 48);
    for (int c = 0; c < 2; ++c) ok += PatchCall(s.calls[c], region + (3 + kNumEntries + c) * kSlot);
    const int tabled = PatchTables(s);
    const int tableTotal = s.tables[0].count + s.tables[1].count + s.tables[2].count;
    if (ok != static_cast<int>(kN) || tabled != tableTotal) {
        // A part could not be written: never keep or hold anything (the hooks that are in pass straight through in mode 0).
        Log("[mapspread] MapPopupSpread: %d of %zu code sites, %d of %d native-table entries written: switched off", ok, kN, tabled,
            tableTotal);
        g_installed = true;
        g_mode = 0;
        return false;
    }
    g_installed = true;
    Log("[mapspread] MapPopupSpread installed: UpdateOrCreateMapIcons exe+0x%llX, ClearAllMapIcons exe+0x%llX, %zu code sites, "
        "native tables %d + %d + %d entries (map widget, icon filter, icon)",
        static_cast<unsigned long long>(s.loop - game.base), static_cast<unsigned long long>(s.clear - game.base), kN,
        s.tables[0].count, s.tables[1].count, s.tables[2].count);
    return true;
}
bool g_offForGood = false;  // installed but switched off (a write failed or ProcessEvent's slot did not match)

}  // namespace

// Start-up only (the native tables must be patched before the classes register them). mode 0 = not installed.
bool InstallMapPopupSpread(bool beforeGameCode, int mode, bool pacingHooked)
{
    if (!mode) { Log("[mapspread] MapPopupSpread off in OblivionPerfFix.ini (takes a restart to switch on)"); return false; }
    if (!beforeGameCode) { Log("[mapspread] MapPopupSpread: start-up only (version.dll), not installed"); return false; }
    if (!pacingHooked) { Log("[mapspread] MapPopupSpread: NpcCreationPacing's frame hooks are not installed: not installed"); return false; }
    Sites s;
    if (!Discover(Game(), &s)) {
        Log("[mapspread] MapPopupSpread: %s not found (pattern): not installed", s.missing ? s.missing : "?");
        return false;
    }
    if (!InstallOn(Game(), s)) {
        g_offForGood = true;
        return false;
    }
    SetMapPopupSpread(mode);
    return true;
}

// Monitor thread (live once installed): 0 off, 1 drop, 4 reuse, 5 both. A change takes effect at the next pairing run;
// anything kept or held is run first by the hooks in mode 0 (stock order).
void SetMapPopupSpread(int mode)
{
    if (!g_installed || g_offForGood || g_peFailed) return;
    mode = Clean(mode);
    const int was = g_mode.exchange(mode, std::memory_order_relaxed);
    if (was != mode) Log("[mapspread] MapPopupSpread=%d (drop %s, reuse %s)", mode, (mode & 1) ? "on" : "off", (mode & 4) ? "on" : "off");
}

// NpcCreationPacing's FrameRun (game thread): begin = its main pairing run starts (kept work allowed), end = it ended:
// everything kept runs now, before the actor tick.
void MapPopupSpreadPairing(bool begin)
{
    if (!g_installed) return;
    if (begin) {
        g_runMode = g_mode.load(std::memory_order_relaxed);  // one mode for the whole run
        g_window = true;
        return;
    }
    g_window = false;
    RunHeldClears();
    if (g_numPend) FlushAll();
}

// Post-actor tick and the next frame's start: kept work should never get here.
void MapPopupSpreadPostTick()
{
    if (!g_installed) return;
    RunHeldClears();
    if (!g_numPend) return;
    g_st.late += static_cast<uint64_t>(g_numPend);
    FlushAll();
}

void MapPopupSpreadFrameEnd(uint64_t)
{
    if (!g_installed) return;
    RunHeldClears();
    if (g_numPend) {
        g_st.late += static_cast<uint64_t>(g_numPend);
        FlushAll();
    }
}

void LogMapPopupSpreadStats()
{
    if (!g_installed || !(g_st.kept | g_st.notKept | g_rs.held)) return;
    static uint64_t logged = ~0ull;
    const uint64_t n = g_st.kept + g_st.notKept + g_rs.held;
    if (n == logged) return;
    logged = n;
    Log("[mapspread] MapPopupSpread mode %d (since start): lists kept %llu, dropped %llu (%llu icons never built), run later %llu "
        "(%llu icons, %.1f ms), not kept %llu, size passes kept %llu (zoom restored %llu), late %llu; reuse: clears held %llu, "
        "lists reused %llu (%llu icons), held clears run %llu; not reused: count %llu, holes %llu, entry differs %llu, map kind "
        "%llu, discovery %llu, guards %llu, Properties not matching %llu",
        g_mode.load(), static_cast<unsigned long long>(g_st.kept), static_cast<unsigned long long>(g_st.dropped),
        static_cast<unsigned long long>(g_st.droppedEntries), static_cast<unsigned long long>(g_st.flushed),
        static_cast<unsigned long long>(g_st.flushedEntries), g_st.flushUs / 1000.0, static_cast<unsigned long long>(g_st.notKept),
        static_cast<unsigned long long>(g_st.sizeKept), static_cast<unsigned long long>(g_st.sizeZoomRestored),
        static_cast<unsigned long long>(g_st.late), static_cast<unsigned long long>(g_rs.held),
        static_cast<unsigned long long>(g_rs.reused), static_cast<unsigned long long>(g_rs.reusedEntries),
        static_cast<unsigned long long>(g_rs.heldRun), static_cast<unsigned long long>(g_rs.fbCount),
        static_cast<unsigned long long>(g_rs.fbHole), static_cast<unsigned long long>(g_rs.fbEntry),
        static_cast<unsigned long long>(g_rs.fbFlag), static_cast<unsigned long long>(g_rs.fbDiscover),
        static_cast<unsigned long long>(g_rs.fbGuard), static_cast<unsigned long long>(g_rs.propMismatch));
}

// ---- self-test ----
// Discovery on a module image (no writes): sites found, the three native tables' entry counts, the ProcessEvent slot offset.
const char* MapSpreadTestDiscover(const GameModule& game, MapSpreadFound* out)
{
    Sites s;
    const bool ok = Discover(game, &s);
    if (out) {
        for (int i = 0; i < 3; ++i) out->tableCounts[i] = s.tables[i].count;
        out->loop = s.loop;
        out->clear = s.clear;
        out->size = s.size;
        out->hover = s.entries[0];
        out->copy = s.copy;
        out->rehash = s.rehash;
        out->free = s.free;
        out->thunkReturn = s.thunkReturn;
        out->peSlot = s.peSlot;
        out->filterTable = s.tables[1].at;
        out->iconTable = s.tables[2].at;
    }
    return ok ? nullptr : (s.missing ? s.missing : "?");
}

namespace {
int t_loops = 0, t_sizes = 0, t_frees = 0, t_loopN = 0, t_lastType1 = 0;
uint8_t* t_lastData = nullptr;
float t_zoomSeen = 0;
void FakeLoop(uint8_t* self, TArr* arr, uint8_t)
{
    ++t_loops;
    t_loopN = arr->num;
    t_lastData = arr->data;
    t_lastType1 = arr->num > 1 ? arr->data[0x50 + 0x30] : 0;
    (void)self;
}
void FakeSize(uint8_t* self)
{
    ++t_sizes;
    memcpy(&t_zoomSeen, self + kOffZoom, 4);
}
void FakeFree(void*) { ++t_frees; }
int t_clears = 0;
void FakeClear(uint8_t*) { ++t_clears; }
int t_rehash = 0;
void FakeRehash(void*) { ++t_rehash; }
int t_copies = 0;
void* FakeCopy(void* dst, const void* src)
{
    ++t_copies;
    memcpy(dst, src, 0x50);
    return dst;
}
}  // namespace

// Runs the keep / drop / flush / reuse state machine with fakes for the game's functions on a fake map widget; returns a bit
// per failed check (0 = all passed).
int MapSpreadTestLogic()
{
    int fail = 0, bit = 1;
    auto check = [&](bool c) {
        if (!c) fail |= bit;
        bit <<= 1;
    };
    const LoopFn sl = g_origLoop;
    const SizeFn ss = g_origSize;
    const FreeFn sf = g_free;
    const CopyFn sc = g_copyEntry;
    const ClearFn scl = g_origClear;
    const RehashFn sr = g_rehash;
    const int sm = g_mode.load();
    const bool si = g_installed;
    g_origClear = &FakeClear;
    g_origLoop = &FakeLoop;
    g_origSize = &FakeSize;
    g_free = &FakeFree;
    g_copyEntry = &FakeCopy;
    g_rehash = &FakeRehash;
    g_testGt = true;
    g_testAnyCaller = true;
    g_installed = true;
    alignas(16) static uint8_t self[0x700];
    memset(self, 0, sizeof self);
    alignas(16) static uint8_t elems[3 * 0x50];
    memset(elems, 0, sizeof elems);
    static uint8_t str[16];
    for (int i = 0; i < 3; ++i) {
        void* sp = str;
        memcpy(elems + i * 0x50 + 0x40, &sp, 8);
        elems[i * 0x50 + 0x30] = static_cast<uint8_t>(i + 1);
    }
    TArr bp{elems, 3, 3};  // Blueprint's own array: must stay untouched
    auto untouched = [&] { return bp.data == elems && bp.num == 3 && elems[0x30] == 1 && elems[0x50 + 0x30] == 2; };
    const float z1 = 1.5f, z2 = 2.5f;
    // 1: keep a copy, size kept, clear drops: nothing created, the copy destroyed (3 strings), Blueprint's array untouched
    g_mode.store(1);
    MapPopupSpreadPairing(true);
    t_loops = t_sizes = t_frees = t_copies = 0;
    LoopHook(self, &bp, 0);
    check(t_loops == 0 && t_copies == 3 && g_numPend == 1 && untouched());
    memcpy(self + kOffZoom, &z1, 4);
    SizeHook(self);
    check(t_sizes == 0 && g_pend[0].size);
    t_clears = 0;
    ClearHook(self);
    check(g_numPend == 0 && t_frees == 3 && t_loops == 0 && t_clears == 1 && untouched());
    // 2: keep, size kept, the pairing end runs both in order on the copy (same values) with the zoom of then
    t_loops = t_sizes = t_frees = 0;
    LoopHook(self, &bp, 0);
    SizeHook(self);
    memcpy(self + kOffZoom, &z2, 4);
    MapPopupSpreadPairing(false);
    check(t_loops == 1 && t_loopN == 3 && t_lastData != elems && t_lastType1 == 2 && t_sizes == 1 && t_zoomSeen == z1 && t_frees == 3);
    float back;
    memcpy(&back, self + kOffZoom, 4);
    check(back == z2 && g_numPend == 0 && untouched());
    // 3: a reader (hover finder, id 2) flushes
    MapPopupSpreadPairing(true);
    t_loops = t_frees = 0;
    LoopHook(self, &bp, 0);
    PreCall(self, 2, nullptr);
    check(t_loops == 1 && g_numPend == 0 && t_frees == 3);
    // 4: outside the pairing run nothing is kept: the game's call on Blueprint's array
    MapPopupSpreadPairing(false);
    t_loops = t_frees = t_copies = 0;
    LoopHook(self, &bp, 0);
    check(t_loops == 1 && t_lastData == elems && t_copies == 0 && g_numPend == 0);
    // 5: mode 0: stock
    g_mode.store(0);
    MapPopupSpreadPairing(true);
    t_loops = 0;
    LoopHook(self, &bp, 0);
    check(t_loops == 1 && t_lastData == elems && g_numPend == 0);
    // 6: two lists in a row without a clear: the first runs before the second is kept (a new run latches mode 1)
    g_mode.store(1);
    MapPopupSpreadPairing(false);
    MapPopupSpreadPairing(true);
    t_loops = t_frees = 0;
    LoopHook(self, &bp, 0);
    LoopHook(self, &bp, 0);
    check(t_loops == 1 && g_numPend == 1 && t_frees == 3);
    MapPopupSpreadPairing(false);
    check(t_loops == 2 && g_numPend == 0 && t_frees == 6 && untouched());
    // 7: the mode is latched for the pairing run: switched off mid-run, the kept list still runs (once) at the run's end
    g_mode.store(1);
    MapPopupSpreadPairing(true);
    t_loops = 0;
    LoopHook(self, &bp, 0);
    g_mode.store(0);
    LoopHook(self, &bp, 0);  // still mode 1 for this run: the first list runs, the second is kept
    MapPopupSpreadPairing(false);
    check(t_loops == 2 && g_numPend == 0 && untouched());
    // 8: reuse: a list equal to the icons but for IconAddress; an icon whose Properties are not a copy of its key -> no write at
    // all (pass 1 fails); then with both copies -> every entry replaced (icon Properties + key) and the set rehashed
    alignas(16) static uint8_t map[0x1000], icons[2][0x900], keys[2 * 0x60], list[2 * 0x50];
    static const wchar_t oldAddr[] = L"0x1", newAddr[] = L"0x2";
    static uint8_t mapCls[0x40], iconCls[0x40];
    auto buildMap = [&] {
        memset(map, 0, sizeof map);
        memset(icons, 0, sizeof icons);
        memset(keys, 0, sizeof keys);
        memset(list, 0, sizeof list);
        const void* p;
        p = mapCls;
        memcpy(map + kObjClass, &p, 8);
        p = keys;
        memcpy(map + kOffMap, &p, 8);
        const int two = 2, three = 3, all = 0x7FFF;
        memcpy(map + kOffMap + 8, &two, 4);      // Num
        memcpy(map + kOffMap + 0x10, &three, 4); // allocation bits 0, 1
        memcpy(map + kOffMap + 0x28, &two, 4);   // NumBits
        (void)all;
        for (int i = 0; i < 2; ++i) {
            uint8_t* const k = keys + i * 0x60;
            const double pos[2] = {100.0 * (i + 1), 50.0};
            memcpy(k, pos, 16);
            k[0x30] = 3;
            p = oldAddr;
            memcpy(k + 0x40, &p, 8);
            const int len = 4;
            memcpy(k + 0x48, &len, 4);
            p = icons[i];
            memcpy(k + 0x50, &p, 8);
            p = iconCls;
            memcpy(icons[i] + kObjClass, &p, 8);
            memcpy(list + i * 0x50, k, 0x50);
            p = newAddr;
            memcpy(list + i * 0x50 + 0x40, &p, 8);
        }
    };
    const OffCache savedProp[4] = {g_propCache[0], g_propCache[1], g_propCache[2], g_propCache[3]};
    const OffCache savedPlayer[4] = {g_playerCache[0], g_playerCache[1], g_playerCache[2], g_playerCache[3]};
    {
        buildMap();
        memcpy(icons[0] + 0x3A8, keys, 0x50);  // icon 1 has no copy of its key
        memset(g_propCache, 0, sizeof g_propCache);
        memset(g_playerCache, 0, sizeof g_playerCache);
        TArr l{list, 2, 2};
        t_copies = t_frees = t_rehash = 0;
        check(!TryReuse(map, &l) && t_copies == 0 && t_frees == 0 && t_rehash == 0);
        memcpy(icons[1] + 0x3A8, keys + 0x60, 0x50);
        t_copies = t_frees = t_rehash = 0;
        const bool ok = TryReuse(map, &l);
        const wchar_t* got = nullptr;
        memcpy(&got, icons[1] + 0x3A8 + 0x40, 8);
        const wchar_t* gotKey = nullptr;
        memcpy(&gotKey, keys + 0x40, 8);
        check(ok && t_copies == 4 && t_frees == 4 && t_rehash == 1 && got == newAddr && gotKey == newAddr);
    }
    // 9: reuse through the hooks: built with flag 1 (local map), clear held in the next run, the same list -> reused (no clear,
    // no create); then built with flag 1 and the next list comes with flag 0 (world map, M-S1) -> the held clear runs, rebuilt
    {
        buildMap();
        memcpy(icons[0] + 0x3A8, keys, 0x50);
        memcpy(icons[1] + 0x3A8, keys + 0x60, 0x50);
        memset(g_propCache, 0, sizeof g_propCache);
        TArr l{list, 2, 2};
        g_numWs = 0;
        g_mode.store(4);
        MapPopupSpreadPairing(true);
        MarkBuilt(map, 1);           // as after a stock build with !IsWorldMap = 1
        t_clears = t_loops = t_rehash = 0;
        ClearHook(map);              // held
        LoopHook(map, &l, 1);        // equal list, same flag -> reused
        MapPopupSpreadPairing(false);
        check(t_clears == 0 && t_loops == 0 && t_rehash == 1 && g_rs.reused >= 1);
        buildMap();
        memcpy(icons[0] + 0x3A8, keys, 0x50);
        memcpy(icons[1] + 0x3A8, keys + 0x60, 0x50);
        const uint64_t fb = g_rs.fbFlag;
        MapPopupSpreadPairing(true);
        MarkBuilt(map, 1);
        t_clears = t_loops = t_rehash = 0;
        ClearHook(map);              // held
        LoopHook(map, &l, 0);        // equal list, the other map kind -> the held clear runs, then the list is built
        MapPopupSpreadPairing(false);
        check(t_clears == 1 && t_loops == 1 && t_rehash == 0 && g_rs.fbFlag == fb + 1);
        // 10: a non-quiet event (an unknown native, id 150) between the build and the next clear: no hold
        MapPopupSpreadPairing(true);
        MarkBuilt(map, 1);
        PreCall(map, 150, nullptr);
        t_clears = 0;
        ClearHook(map);
        check(t_clears == 1);
        MapPopupSpreadPairing(false);
    }
    // 12: reuse guards: no filter, or a filter showing all (mask 0x7FFF) -> ok, whatever its displayed-icon count (+0x5F0, a
    // count the clear recomputes, not a reuse condition: MS-S1); a filter mask set, or a hovered icon -> no reuse
    {
        buildMap();
        alignas(16) static uint8_t filt[0x700];
        memset(filt, 0, sizeof filt);
        const int all = 0x7FFF, one = 1;
        memcpy(filt + kFilterMask, &all, 4);
        check(ReuseGuards(map));  // no filter
        const void* p = filt;
        memcpy(map + kOffFilter, &p, 8);
        const bool okCleared = ReuseGuards(map);
        const int shown = 2;
        memcpy(filt + kFilterShown, &shown, 4);
        const bool okSecond = ReuseGuards(map);
        memcpy(filt + kFilterMask, &one, 4);
        const bool okMask = ReuseGuards(map);
        memcpy(filt + kFilterMask, &all, 4);
        p = icons[0];
        memcpy(map + kOffHover, &p, 8);
        const bool okHover = ReuseGuards(map);
        check(okCleared && okSecond && !okMask && !okHover);
        memset(map + kOffFilter, 0, 8);
        memset(map + kOffHover, 0, 8);
    }
    // 11: public values keep only the drop and reuse bits
    check(Clean(5) == 5 && Clean(7) == 5 && Clean(2) == 0 && Clean(-1) == 0 && Clean(32) == 0);
    for (int i = 0; i < 4; ++i) {
        g_propCache[i] = savedProp[i];
        g_playerCache[i] = savedPlayer[i];
    }
    g_origLoop = sl;
    g_origSize = ss;
    g_free = sf;
    g_copyEntry = sc;
    g_origClear = scl;
    g_rehash = sr;
    g_numWs = 0;
    g_numPend = 0;
    g_mode.store(sm);
    g_testGt = false;
    g_testAnyCaller = false;
    g_installed = si;
    g_window = false;
    g_st = Stats{};
    g_rs = ReuseStats{};
    return fail;
}

}  // namespace opf
