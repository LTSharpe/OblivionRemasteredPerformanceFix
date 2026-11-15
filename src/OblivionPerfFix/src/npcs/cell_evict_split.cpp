// CellEvictSplit (flightrest / evfix 2026-11-13, reviews rev96f; public since 1.5.0, default 0 until the release flips it;
// [Fixes] CellEvictSplit = actors per step, 2 when on; live once found): a paced cell eviction's actors have their 3D
// cleared over the OblivionThread frames before the cell's own teardown, a few per frame, instead of all inside the one
// OT frame that tears the cell down. Needs CellBufferPacedEvict (it runs inside its paced unload).
//
// Why (findings 2026-11-13 flightrest): CellBufferPacedEvict (npcs/cell_buffer.cpp) unloads one buffered cell per OT
// frame. A cell with many actors makes that one OT frame long and the game thread waits for it in the pairing session:
// on the Imperial City flight cell 0x5F4C (187 refs, 10-11 actors) takes 18.5-27.3 ms in every flight (8 of 8) and the
// GT frame reaches 29-33 ms in half of them. Its teardown: 19.9 ms = per-reference 3D clears 19.5 ms, of which the 10
// actors 17.2 ms (animation clean-up 13.5 ms) and 119 statics 1.6 ms. With 2 per step the unload at flight second 43.4
// went from one OT step of 20.7 ms to 5-6 steps of 3.1-5.0 ms.
//
// What it does: npcs/cell_buffer.cpp's paced Trim calls CellEvictSplitStep(cell) just before it would unload the last
// tail cell. If that cell still holds two or more eligible actors (base form NPC_ / CREA, 3D loaded at ref+0x78), up to
// CellEvictSplit of them (at most until 3 ms were spent) get exactly the call the game's 3D removal 0x6658290 makes for
// every reference of the cell: ref->vtable[0x2D0](ref, nullptr), under the same lock (0x694FA70 / 0x694FC80 on the same
// lock object, cell as the second argument), on the same thread (the OT), at the same point of the OT frame (after the
// per-frame grid-change call). The cell stays in its tail slot this frame; the next paced frame comes back to it. Once
// fewer than two eligible actors are left, the cell is unloaded by the game's own teardown 0x662E360 as before (its 3D
// removal then finds the cleared actors without 3D: the game's own path for a reference without 3D, as for disabled or
// never-loaded references).
//
// Equivalence: the cell is detached (not in the loaded grid: Trim stops at an attached cell); all its references' 3D is
// cleared by the same calls, only spread over 2-5 more OT frames (~35-85 ms); the unload itself is the game's.
// Differences: (1) the characters' 3D goes a few OT frames earlier than the rest of the cell, in the game's list order
// among themselves but ahead of the cell's other references (a hit-effect target goes with the stock unload; rev102b N2);
// (2) the cell stays in the tail those frames, so later tail cells are unloaded a few frames later (they are paced one per
// frame anyway); (3) if the game re-attaches that cell in those frames (the player turning back into the oldest buffered
// cell within ~0.1 s), the attach re-queues its references (0x66564D0: QueueReference has no 3D test), so the cleared
// actors load again, never lost: later than with CellBufferPacedEvict alone, never later than the stock game, which had
// unloaded the whole cell in the crossing frame (counted as "left the tail early").
// fix102 (rev102b S1): the cell's reference list is walked under the same lock as the game's own walk (taken before the
// walk, held through the step; the lock is re-entrant: Set3D re-enters it).
// Review rev96f's gates: the stock teardown sets the cell's state byte (cell+0x7B) to 1 before its 3D removal, and
// TESObjectREFR::Set3D(null) 0x658ABF0 makes two extra calls when the ref's cell is NOT in state 1 (0x658B9AF):
// ProcessLists 0x6741C50(ref) (every MagicHitEffect in ProcessLists+0x90 whose target [+0x30] == ref is notified) and,
// while the live ArrowProjectile count [0x94807F4] > 0, 0x66679F0(ref, 1) (arrows stuck in the ref are removed). A tail
// cell is in state 3/4, so a split step runs only where both calls are no-ops:
//   B1.1 no step while the arrow count is > 0 (read before the step and again before every actor; the counter's address
//        comes from Set3D's own `cmp dword [rip+X],0`): the cell then gets the stock paced unload this frame;
//   B1.2 an actor that is the target of a MagicHitEffect in ProcessLists+0x90 is left out (the list walked as 0x6741C50
//        walks it: item vt+8 = RTTI, its parent chain at +8 contains MagicHitEffect's RTTI, [item+0x30] == actor;
//        read-only, no reference count touched), tested again right before the actor's call;
//   B1.3 only eligible actors (3D, not a hit target) are counted: fewer than 2 -> the stock unload;
//   F1   at most ceil(first eligible count / N) + 1 steps per cell, then the stock unload; a step that would clear
//        nothing returns "no split" (stock unload).
// The cell's state byte is not touched. Open (rev96f F2, minor): the cell's temporary effects (decals, 0x6747120) are
// removed by the teardown, so they outlive the cleared actors by 1-5 OT frames in that far cell behind the player: the
// state a stock Disable of an actor with decals leaves, gone at the teardown.
//
// Found by shape (build 19115871: 0x6658290 whole body; lock, unlock, lock object and player pointer read from it;
// Set3D's state-1 block 0x658B9AF (ProcessLists getter, hit-effect notify, arrow counter); 0x6741C50 whole walk up to the
// notify (list offset 0x90, target +0x30, MagicHitEffect RTTI)); any of them not found once: the key stays off (a log
// line). Log: [evictsplit] the find line, the key's state, a stats line when steps ran.
#include "common.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace opf {
namespace {

// 0x6658290 (3D removal of a cell's references): mov [rsp+8],rbx; push rdi; sub rsp,20h; mov rdi,rcx; mov rdx,rcx;
// lea rcx,[lock]; call lock; lea rbx,[rdi+0B8h]; ...; loop: mov rcx,[rbx]; mov rbx,[rbx+8]; test rcx,rcx; je;
// cmp rcx,[player]; je; mov rax,[rcx]; xor edx,edx; call [rax+2D0h]; ...; lea rcx,[lock]; ...; jmp unlock
constexpr char kRemove3D[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B D1 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 9F B8 00 00 00 "
    "48 85 DB 74 2D 0F 1F 84 00 00 00 00 00 48 8B 0B 48 8B 5B 08 48 85 C9 74 14 48 3B 0D ?? ?? ?? ?? 74 0B "
    "48 8B 01 33 D2 FF 90 D0 02 00 00 48 85 DB 75 DB 48 8B D7 48 8D 0D ?? ?? ?? ?? 48 8B 5C 24 30 48 83 C4 20 5F "
    "E9 ?? ?? ?? ??";

// Set3D(null)'s block at 0x658B9AF: mov rax,[rdi+80h]; test rax,rax; je; cmp byte [rax+7Bh],1; je; call ProcessLists
// getter; mov rcx,rax; mov rdx,rdi; call 0x6741C50; cmp dword [arrow count],0; jle; mov dl,1; mov rcx,rdi; call 0x66679F0
constexpr char kSet3DState1[] =
    "48 8B 87 80 00 00 00 48 85 C0 74 ?? 80 78 7B 01 74 ?? E8 ?? ?? ?? ?? 48 8B C8 48 8B D7 E8 ?? ?? ?? ?? "
    "83 3D ?? ?? ?? ?? 00 7E ?? B2 01 48 8B CF E8 ?? ?? ?? ??";
// ProcessLists getter 0x674B050 (the call at +0x12 of the block above): mov rax,[rip+X]; ret = 48 8B 05 ?? ?? ?? ?? C3,
// checked in place
// 0x6741C50 (MagicHitEffects targeting ref are notified): lea rdi,[rcx+90h] ... lea rbp,[MagicHitEffect RTTI]; loop:
// item=[rdi]; ref count +1/-1; rtti=item->vt[1](); walk rtti+8 to rbp; [item+30h]==ref -> (item-18h)->vt[8](); [item+3Ch]=1
constexpr char kHitNotify[] =
    "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 20 33 C0 48 8D B9 90 00 00 00 48 8B F2 89 44 24 30 48 39 47 08 75 0F "
    "8B C8 8D 41 01 48 39 0F 75 0A 0F B6 D8 EB 07 48 8B 4C 24 30 32 DB A8 01 74 1E 48 85 C9 74 19 B8 FF FF FF FF "
    "F0 0F C1 41 08 83 F8 01 75 0A 48 8B 01 BA 01 00 00 00 FF 10 84 DB 75 79 48 85 FF 74 74 48 89 6C 24 38 "
    "48 8D 2D ?? ?? ?? ?? 48 8B 1F 48 85 DB 74 52 F0 FF 43 08 B8 FF FF FF FF F0 0F C1 43 08 83 F8 01 75 0D "
    "48 8B 03 BA 01 00 00 00 48 8B CB FF 10 48 8B 03 48 8B CB FF 50 08 48 85 C0 74 24 48 3B C5 74 0B 48 8B 40 08 "
    "48 85 C0 75 F2 EB 14 48 8D 4B E8 48 39 73 30 75 0A 48 8B 01 FF 50 40 C6 43 3C 01";

using LockFn = void (*)(void* lock, void* cell);
using Set3DFn = void (*)(void* ref, void* node);
using BaseFn = const uint8_t* (*)(const void* ref);
using RttiFn = const uint8_t* (*)(const void* obj);

struct State {
    bool found = false;
    void* lockObj = nullptr;
    LockFn lock = nullptr, unlock = nullptr;
    void** player = nullptr;
    const volatile int32_t* arrows = nullptr;  // live ArrowProjectile count (Set3D's `cmp dword [rip+X],0`)
    uint8_t* const* processLists = nullptr;    // ProcessLists singleton pointer (the getter's global)
    const uint8_t* hitRtti = nullptr;          // MagicHitEffect NiRTTI
    std::atomic<int> perStep{0};
    std::atomic<uint64_t> steps{0}, cleared{0}, cells{0}, maxStepUs{0}, returned{0}, faults{0};
    std::atomic<uint64_t> arrowSkips{0}, hitExcluded{0}, limitEnded{0}, fallbacks{0}, emptySteps{0};
    uint8_t* lastCell = nullptr;  // OT only
    uint32_t stepsLeft = 0;       // OT only: F1 step budget of lastCell
    bool stepped = false;         // OT only: a step ran in this OT frame (CellEvictSplitTookFrame)
    bool testClock = false;       // self-test: Us() counts calls (a step never reaches the 3 ms cap)
} s;

uint64_t Us()
{
    if (s.testClock) return 0;
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart * 1000000 / f.QuadPart);
}

bool IsActor(const void* ref)
{
    const BaseFn base = reinterpret_cast<BaseFn>((*reinterpret_cast<void* const* const*>(ref))[0x310 / 8]);
    const uint8_t* const b = base(ref);
    return b && (b[8] == 0x23 || b[8] == 0x24);
}

int32_t ArrowCount()
{
    return *s.arrows;
}

// Whether `actor` is the target of a MagicHitEffect in ProcessLists+0x90, tested as 0x6741C50 tests it (read-only:
// no reference count is touched). SEH by the caller.
bool IsHitTarget(const void* actor)
{
    const uint8_t* const pl = *s.processLists;
    if (!pl) return false;
    const uint8_t* node = pl + 0x90;
    for (uint32_t i = 0; node && i < 100000; ++i) {
        const uint8_t* const item = *reinterpret_cast<const uint8_t* const*>(node);
        node = *reinterpret_cast<const uint8_t* const*>(node + 8);
        if (!item) continue;
        const RttiFn rttiOf = reinterpret_cast<RttiFn>((*reinterpret_cast<void* const* const*>(item))[1]);
        bool hit = false;
        for (const uint8_t* r = rttiOf(item); r; r = *reinterpret_cast<const uint8_t* const*>(r + 8)) {
            if (r == s.hitRtti) {
                hit = true;
                break;
            }
        }
        if (hit && *reinterpret_cast<const void* const*>(item + 0x30) == actor) return true;
    }
    return false;
}

// The cell's actors with 3D: `*with3d` = all of them, return value = the eligible ones (not a MagicHitEffect target;
// at most `max` collected into out[]); `*excluded` = hit targets left out. SEH by the caller.
uint32_t EligibleActors(uint8_t* cell, void** out, uint32_t max, uint32_t* with3d, uint32_t* excluded)
{
    uint32_t n = 0, all = 0, ex = 0;
    void* const player = s.player ? *s.player : nullptr;
    const uint8_t* node = cell + 0xB8;
    for (uint32_t i = 0; node && i < 100000; ++i) {
        void* const item = *reinterpret_cast<void* const*>(node);
        node = *reinterpret_cast<const uint8_t* const*>(node + 8);
        if (!item || item == player) continue;
        if (!*reinterpret_cast<void* const*>(static_cast<uint8_t*>(item) + 0x78)) continue;  // no 3D
        if (!IsActor(item)) continue;
        ++all;
        if (IsHitTarget(item)) {
            ++ex;
            continue;
        }
        if (n < max) out[n] = item;
        ++n;
    }
    *with3d = all;
    *excluded = ex;
    return n;
}

// A cell with >= 2 actors with 3D gets the stock unload this frame (cell_buffer.cpp unloads it after a false).
bool fallback()
{
    s.fallbacks.fetch_add(1, std::memory_order_relaxed);
    s.lastCell = nullptr;
    return false;
}

bool SafeIsHitTarget(const void* actor, bool* fault)
{
    __try {
        return IsHitTarget(actor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return true;
    }
}

}  // namespace

// Self-test / install: the gates of review rev96f B1 (RVAs: arrow count, ProcessLists pointer, MagicHitEffect RTTI).
bool CellEvictSplitFindGates(const GameModule& game, uint32_t out[3])
{
    const auto blk = FindAll(game.text, ParsePattern(kSet3DState1));
    const auto notify = FindAll(game.text, ParsePattern(kHitNotify));
    if (blk.size() != 1 || notify.size() != 1) return false;
    uint8_t* const b = blk[0];
    uint8_t* const getter = RipTarget(b + 0x12, 1, 5);
    if (RipTarget(b + 0x1D, 1, 5) != notify[0]) return false;  // Set3D's notify call is the walk we copy
    // the getter's shape, checked in place
    if (getter < game.text.begin || getter + 8 > game.text.end || getter[0] != 0x48 || getter[1] != 0x8B ||
        getter[2] != 0x05 || getter[7] != 0xC3)
        return false;
    if (out) {
        out[0] = static_cast<uint32_t>(RipTarget(b + 0x22, 2, 7) - game.base);
        out[1] = static_cast<uint32_t>(RipTarget(getter, 3, 7) - game.base);
        out[2] = static_cast<uint32_t>(RipTarget(notify[0] + 0x6B, 3, 7) - game.base);
    }
    return true;
}

bool CellEvictSplitFind(const GameModule& game, uint32_t out[4])
{
    const auto hits = FindAll(game.text, ParsePattern(kRemove3D));
    if (hits.size() != 1) return false;
    uint8_t* const f = hits[0];
    uint8_t* const lock1 = RipTarget(f + 0x10, 3, 7);
    uint8_t* const lock2 = RipTarget(f + 0x58, 3, 7);
    if (lock1 != lock2) return false;
    if (out) {
        out[0] = static_cast<uint32_t>(f - game.base);
        out[1] = static_cast<uint32_t>(RipTarget(f + 0x17, 1, 5) - game.base);
        out[2] = static_cast<uint32_t>(RipTarget(f + 0x69, 1, 5) - game.base);
        out[3] = static_cast<uint32_t>(RipTarget(f + 0x3C, 3, 7) - game.base);
    }
    return true;
}

bool InstallCellEvictSplit()
{
    const GameModule& game = Game();
    uint32_t o[4] = {};
    if (!CellEvictSplitFind(game, o)) {
        Log("[evictsplit] CellEvictSplit: the cell 3D removal (0x6658290 shape) was not found once: not available");
        return false;
    }
    uint8_t* const f = game.base + o[0];
    s.lockObj = RipTarget(f + 0x10, 3, 7);
    s.lock = reinterpret_cast<LockFn>(game.base + o[1]);
    s.unlock = reinterpret_cast<LockFn>(game.base + o[2]);
    s.player = reinterpret_cast<void**>(game.base + o[3]);
    uint32_t gt[3] = {};
    if (!CellEvictSplitFindGates(game, gt)) {
        Log("[evictsplit] CellEvictSplit: Set3D's cell-state block or the hit-effect walk (0x658B9AF / 0x6741C50 shapes) "
            "was not found once: not available");
        return false;
    }
    s.arrows = reinterpret_cast<const volatile int32_t*>(game.base + gt[0]);
    s.processLists = reinterpret_cast<uint8_t* const*>(game.base + gt[1]);
    s.hitRtti = game.base + gt[2];
    s.found = true;
    Log("[evictsplit] found: 3D removal exe+0x%X, lock exe+0x%X, unlock exe+0x%X, player exe+0x%X, arrow count exe+0x%X, "
        "ProcessLists exe+0x%X, MagicHitEffect RTTI exe+0x%X",
        o[0], o[1], o[2], o[3], gt[0], gt[1], gt[2]);
    return true;
}

void SetCellEvictSplit(int perStep)
{
    if (perStep < 0) perStep = 0;
    if (perStep > 8) perStep = 8;
    if (!s.found) perStep = 0;
    if (s.perStep.exchange(perStep) != perStep)
        Log("[evictsplit] CellEvictSplit=%d (%s)", perStep,
            perStep ? "a paced eviction's actors lose their 3D a few per OT frame before the cell's unload" : "off");
}

// OT, from cell_buffer.cpp's paced Trim, before it unloads `cell` (the last tail cell, not attached). true = a step ran,
// the cell stays this frame.
bool CellEvictSplitStep(uint8_t* cell)
{
    const int per = s.perStep.load(std::memory_order_relaxed);
    if (!per || !cell) return false;
    void* acts[8] = {};
    uint32_t total = 0, excluded = 0, eligible = 0;
    // fix102 (rev102b S1): the game's lock first, as 0x6658290 takes it before its own walk of the list; every return
    // below unlocks
    s.lock(s.lockObj, cell);
    __try {
        eligible = EligibleActors(cell, acts, static_cast<uint32_t>(per), &total, &excluded);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s.unlock(s.lockObj, cell);
        s.faults.fetch_add(1, std::memory_order_relaxed);
        s.lastCell = nullptr;
        return false;
    }
    if (total <= 1) {  // nothing (left) to split: the stock unload
        s.unlock(s.lockObj, cell);
        s.lastCell = nullptr;
        return false;
    }
    // From here the cell has >= 2 actors with 3D: every "no split" below is a fallback to the stock unload this frame
    // (cell_buffer.cpp then unloads it), so lastCell is reset with it.
    if (ArrowCount() > 0) {  // B1.1: Set3D(null) would remove stuck arrows (0x66679F0); stock teardown does not
        s.unlock(s.lockObj, cell);
        s.arrowSkips.fetch_add(1, std::memory_order_relaxed);
        return fallback();
    }
    if (excluded) s.hitExcluded.fetch_add(excluded, std::memory_order_relaxed);  // B1.2
    if (eligible < 2) {                                                          // B1.3
        s.unlock(s.lockObj, cell);
        return fallback();
    }
    if (cell != s.lastCell) {
        // the previous split cell left the tail before its unload by the paced path (found again by the game's insert,
        // or unloaded at once by a jump): counted as "left early" (reviewer item: should be 0 or the jump case only)
        if (s.lastCell) s.returned.fetch_add(1, std::memory_order_relaxed);
        s.cells.fetch_add(1, std::memory_order_relaxed);
        s.stepsLeft = (eligible + static_cast<uint32_t>(per) - 1) / static_cast<uint32_t>(per) + 1;  // F1
    }
    if (s.stepsLeft == 0) {  // F1: the cell's step budget is used up
        s.unlock(s.lockObj, cell);
        s.limitEnded.fetch_add(1, std::memory_order_relaxed);
        return fallback();
    }
    --s.stepsLeft;
    s.lastCell = cell;
    const uint32_t take = eligible < static_cast<uint32_t>(per) ? eligible : static_cast<uint32_t>(per);
    const uint64_t t0 = Us();
    uint32_t done = 0;
    bool fault = false;
    for (uint32_t k = 0; k < take; ++k) {
        void* const ref = acts[k];
        // B1.1 / B1.2 again right before the call (the previous actor's Set3D ran in between)
        if (ArrowCount() > 0) {
            s.arrowSkips.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (SafeIsHitTarget(ref, &fault)) {
            s.hitExcluded.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const Set3DFn set3d = reinterpret_cast<Set3DFn>((*reinterpret_cast<void* const* const*>(ref))[0x2D0 / 8]);
        set3d(ref, nullptr);
        ++done;
        if (Us() - t0 >= 3000) break;
    }
    s.unlock(s.lockObj, cell);
    if (fault) s.faults.fetch_add(1, std::memory_order_relaxed);
    if (!done) {  // F1: a step that clears nothing is no split: the stock unload this frame
        s.emptySteps.fetch_add(1, std::memory_order_relaxed);
        return fallback();
    }
    const uint64_t us = Us() - t0;
    s.steps.fetch_add(1, std::memory_order_relaxed);
    s.cleared.fetch_add(done, std::memory_order_relaxed);
    for (uint64_t m = s.maxStepUs.load(); us > m && !s.maxStepUs.compare_exchange_weak(m, us);) {
    }
    s.stepped = true;
    return true;
}

// OT, cell_buffer.cpp (fix102, rev102b N1): a non-paced Trim ran (a jump, a load: the tail is torn down at once), so a
// split cell's step budget is not carried to a later cell at the same address
void CellEvictSplitForget()
{
    if (s.lastCell) s.returned.fetch_add(1, std::memory_order_relaxed);
    s.lastCell = nullptr;
}

// self-test (fix102, rev102b M1): fake game functions and globals for the decision logic; nullptr restores
void CellEvictSplitTestFakes(const CellEvictSplitFakes* f)
{
    struct Saved {
        bool found;
        void* lockObj;
        LockFn lock, unlock;
        void** player;
        const volatile int32_t* arrows;
        uint8_t* const* processLists;
        const uint8_t* hitRtti;
    };
    static Saved saved{};
    static bool have = false;
    if (f) {
        if (!have) saved = {s.found, s.lockObj, s.lock, s.unlock, s.player, s.arrows, s.processLists, s.hitRtti}, have = true;
        s.found = true;
        s.lock = f->lock, s.unlock = f->unlock, s.lockObj = f->lockObj, s.player = f->player, s.arrows = f->arrows;
        s.processLists = f->processLists, s.hitRtti = f->hitRtti;
        s.testClock = true;
    } else if (have) {
        s.perStep = 0;
        s.found = saved.found, s.lockObj = saved.lockObj, s.lock = saved.lock, s.unlock = saved.unlock;
        s.player = saved.player, s.arrows = saved.arrows, s.processLists = saved.processLists, s.hitRtti = saved.hitRtti;
        s.testClock = false;
        have = false;
    }
    s.lastCell = nullptr;
    s.stepsLeft = 0;
    s.stepped = false;
}

void CellEvictSplitTestCounts(uint64_t out[8])
{
    out[0] = s.steps.exchange(0), out[1] = s.cleared.exchange(0), out[2] = s.arrowSkips.exchange(0);
    out[3] = s.hitExcluded.exchange(0), out[4] = s.limitEnded.exchange(0), out[5] = s.emptySteps.exchange(0);
    out[6] = s.fallbacks.exchange(0), out[7] = s.faults.exchange(0);
    s.cells = 0, s.returned = 0, s.maxStepUs = 0;
}

// OT, cell_buffer.cpp: whether a split step ran in this OT frame (RowPreload then leaves the frame alone), and clear it.
bool CellEvictSplitTookFrame()
{
    const bool v = s.stepped;
    s.stepped = false;
    return v;
}

void LogCellEvictSplitStats()
{
    static uint64_t last = 0;
    const uint64_t st = s.steps.load() + s.fallbacks.load();
    if (st == last) return;
    last = st;
    Log("[evictsplit] cells split %llu, steps %llu, actors cleared ahead %llu, longest step %.2f ms, left the tail early %llu, "
        "faults %llu; arrow skips %llu, hit-target actors left out %llu, cells ended by the step limit %llu, empty steps %llu, "
        "stock-unload fallbacks %llu",
        static_cast<unsigned long long>(s.cells.load()), static_cast<unsigned long long>(s.steps.load()),
        static_cast<unsigned long long>(s.cleared.load()), s.maxStepUs.load() / 1000.0,
        static_cast<unsigned long long>(s.returned.load()), static_cast<unsigned long long>(s.faults.load()),
        static_cast<unsigned long long>(s.arrowSkips.load()), static_cast<unsigned long long>(s.hitExcluded.load()),
        static_cast<unsigned long long>(s.limitEnded.load()), static_cast<unsigned long long>(s.emptySteps.load()),
        static_cast<unsigned long long>(s.fallbacks.load()));
}

}  // namespace opf
