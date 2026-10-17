// ViewRenameGuard ([Fixes] ViewRenameGuard, on by default; installed at start-up only, switchable while the game runs):
// closes the engine race UE-217692 between D3D12 view creation and buffer/texture pool defragmentation. The stock game
// has it with every defragmentation pass (buffers and textures); with it closed, RdgParallelTranslate's frame fence
// (translate_frame_fence.cpp) lets buffer defragmentation run alongside parallel translate, which saves ~0.2 GB of video
// memory (measured 2026-10-12). The fence is only used while this guard is installed and on (main.cpp).
// Measured (2026-10-12, lab, frame fence on, cpu profile): 6.4 M view creations and 1.1 M guarded first registrations
// per run, 51,000 buffer moves under 1 GB/frame defragmentation stress, 56,757 repairs, no crash and no GPU error in
// 12 launches; no measurable cost (fps 54.6 vs 54.4, render and RHI thread ms within noise); the same images.
// The stale state itself was never produced (0 stale views even with a lab race amplifier): the race is rare here.
//
// The race (UE 5.3.2 source, third_party/ue5.3, D3D12RHI/Private):
//  - View creation, on whichever thread creates the view (render thread, RDG setup tasks, loading threads; only views
//    of dynamic buffers are deferred to the RHI thread, D3D12View.h:378-397): the caller builds the D3D view desc and
//    the FResourceInfo from the buffer's FD3D12ResourceLocation (offset, UnderlyingResource; D3D12SRV.cpp:110-160,
//    D3D12UAV.cpp:92-133), then FD3D12View::CreateView (D3D12View.cpp:264-305) registers the view as a rename
//    listener (AddRenameListener takes the base resource's RenameListenersCS, D3D12Resources.h:774-779) and only
//    then stores the FResourceInfo and writes the descriptor.
//  - Defragmentation, on the RHI thread at the start of a frame (FD3D12DefaultBufferAllocator::BeginFrame,
//    D3D12Allocation.cpp:1570; texture pools likewise): FD3D12PoolAllocator::HandleDefragRequest
//    (D3D12PoolAllocator.cpp:567) -> FD3D12ResourceLocation::OnAllocationMoved (D3D12Resources.cpp:1159-1257) writes the
//    location's UnderlyingResource / offset / GPU address WITHOUT the listener lock, then Owner->ResourceRenamed
//    takes RenameListenersCS and calls every registered view's ResourceRenamed (D3D12Resources.h:795-802). The old
//    resource is freed after the next frame fence.
//  - A view whose creator read the location before the move but registered after the notification loop ran keeps
//    the old resource for good; its next bind after the old resource is freed reads freed memory (the crash of
//    2026-10-07 in UpdateResidency). If the loop runs while the creator is between registration and its own stores,
//    the creator's stale stores can also overwrite the loop's update (and the loop's _RHI CreateView can even
//    register the view a second time while iterating the listener array).
//  - Epic's fix (5.5.4 D3D12View.h:418 vs 5.4.4 :401): every RHI view is created on the RHI thread.
//
// This guard (one detour on FD3D12View::CreateView, one on OnAllocationMoved):
//  - On a view's first registration (its old BaseResource is null, the new one is not) it holds the base resource's
//    RenameListenersCS across the whole original FD3D12View::CreateView, so no notification loop for that resource
//    can run while the view registers and stores its state; and before releasing the lock it checks the view:
//      * its ResourceInfo.Resource differs from the location's current UnderlyingResource (a stale view), or
//      * the location was moved within the last second (the creator may have read a torn location),
//    and if so calls the view's own ResourceRenamed (vtable slot 0) with (base, location), exactly what the
//    notification loop does for registered views. All buffer views are FD3D12*View_RHI, whose ResourceRenamed
//    recreates the view from its FRHIViewDesc and the location's current state; texture views keep their desc.
//  - Why that is complete: a move's location writes all happen before its notification loop, and the loop holds
//    RenameListenersCS. Either the loop finished before the guard took the lock (then every write is visible to the
//    guard's check, and the repair reads final values), or it runs after the guard released it (the view is
//    registered by then and the loop repairs it). A move in progress while the guard holds the lock is followed by
//    its loop, which repairs the view again with the final values. A repair of a view that was not stale gives the
//    same descriptor (it is the engine's own rename path on unchanged values).
//  - Calls that re-create an already registered view (the rename loop's own _RHI CreateView, on the RHI thread),
//    views without a base resource (manual views, e.g. constant buffers) and a view moved to another resource pass
//    through unchanged.
// Lock order: the RHI thread reaches RenameListenersCS holding the default-buffer allocator's CS (buffer defrag,
// D3D12Allocation.cpp:1573) or nothing above the pool, and the pool allocator's CS (FRHIPoolAllocator::Defrag,
// RHIPoolAllocator.cpp). The guard holds RenameListenersCS across the original FD3D12View::CreateView, which takes
// nothing else of D3D12's (the inlined AddRenameListener re-enters the same CS; descriptors and the bindless manager are
// leaves). The repair runs the view's own rename path; for a UAV with a hidden counter (append/consume, atomic counter)
// that path creates a new 4-byte counter buffer (D3D12UAV.cpp:42-56). In 5.3.2 and in this exe that is a committed
// resource (FD3D12Adapter::CreateBuffer(HeapProps,...) -> CreateCommittedResource, D3D12Resources.cpp:751-775, exe
// 0x2540200 -> 0x2540530; the only locks below it are the residency manager's and, on failure, logging), so it takes
// neither allocator lock. Still, as a precaution the default mode 1 does not repair counter UAVs under the lock (they
// are counted and left as in the stock game); mode 3 repairs them too.
// Mode 2 = count only: the same lock and checks, no repair (measures how often the stock race happens). The public ini
// key takes 0 / 1; modes 2 and 3 and the race amplifier (ViewRenameGuardDelayUs) are lab keys.
//
// Found by shape (build 19115871 addresses in the comments), each exactly once:
//  - FD3D12View::CreateView (exe+0x255CDC0): prologue, cmp [view+0x10],[info]; RemoveRenameListener call; the inlined
//    AddRenameListener (lea rbx,[base+10h]; call [EnterCriticalSection]; TArray at base+0x38/+0x40/+0x44, store of the
//    view); at +0x8E the copy of the 32-byte FResourceInfo to view+0x10..0x2F; at +0xE2 UpdateDescriptor (slot 2).
//  - FD3D12ResourceLocation::OnAllocationMoved (exe+0x2549E80): the inlined Owner->ResourceRenamed at +0x1BD
//    (Owner = location+0x8, lock at base+0x10, listeners at base+0x38/+0x40, location = base+0x48, call [vtable+0]),
//    and the store of the new UnderlyingResource at location+0x10.
//  - FD3D12UnorderedAccessView::UpdateDescriptor (exe+0x2567B50, vtable slot 2 of every UAV): Resource = view+0x20,
//    counter = view+0x88 (CounterResource), call [device vtable+0x98] (CreateUnorderedAccessView). A view whose slot 2
//    is this function and whose +0x88 is set is a counter UAV.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

constexpr char kCreateViewPattern[] =
    "40 57 41 57 48 83 EC 48 4C 8B 79 18 48 8B F9 48 8B 49 10 48 89 5C 24 68 48 89 74 24 40 4C 89 64 24 38 "
    "4C 89 74 24 30 4C 8B F2 4D 63 E0 48 3B 0A 74 ?? 48 85 C9 74 ?? 48 8B D7 E8 ?? ?? ?? ?? 49 8B 36 48 85 F6 "
    "74 ?? 48 8D 5E 10 48 89 6C 24 70 48 8B CB FF 15 ?? ?? ?? ?? 48 63 6E 40 8D 45 01 89 46 40 3B 46 44 76 ?? "
    "8B D5 48 8D 4E 38 E8 ?? ?? ?? ?? 48 8B 46 38 48 89 3C E8";
constexpr size_t kCreateViewEnterCall = 0x53;  // FF 15 rel32: call [EnterCriticalSection]
constexpr size_t kCreateViewInfoCopy = 0x8E;
constexpr char kCreateViewInfoCopyPattern[] = "41 0F 10 06 0F 11 47 10 41 0F 10 4E 10 4C 8B 74 24 30 0F 11 4F 20 48 8B 5F 20";
constexpr size_t kCreateViewUpdateDescriptor = 0xE2;
constexpr char kUpdateDescriptorPattern[] = "48 8B 07 FF 50 10";
constexpr uint8_t kCreateViewPrologue[8] = {0x40, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x48};  // push rdi; push r15; sub rsp,48h

constexpr char kMovedLoopPattern[] =
    "48 8B 73 08 48 8D 5E 10 48 8B CB FF 15 ?? ?? ?? ?? 48 8B 7E 38 48 63 46 40 48 8D 2C C7 48 3B FD 74 ?? 90 "
    "48 8B 0F 4C 8D 46 48 48 8B D6 48 8B 01 FF 10";
constexpr size_t kMovedLoopEnterCall = 0x0B;
constexpr char kMovedStartPattern[] = "48 8B C4 53 48 81 EC A0 00 00 00 48 89 68 10 48 8B D9";
constexpr char kMovedStoreResourcePattern[] = "48 89 43 10 48 8B 48 40 48 03 4B 68 48 89 4B 60";
constexpr uint8_t kMovedPrologue[11] = {0x48, 0x8B, 0xC4, 0x53, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00};  // mov rax,rsp; push rbx; sub rsp,0A0h

constexpr char kUavUpdateDescriptorPattern[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B 41 20 48 8B D9 48 8B 78 28 48 85 FF 75 04 48 8B 78 20 "
    "48 8B 81 88 00 00 00 48 85 C0 74 06 48 8B 70 20 EB 02 33 F6 48 8B 49 08 E8 ?? ?? ?? ?? 48 8B 4B 40 4C 8D 4B 60 "
    "48 89 4C 24 20 4C 8B C6 48 8B D7 48 8B C8 4C 8B 10 41 FF 92 98 00 00 00";

// Layout (build 19115871, matches UE 5.3.2's classes)
constexpr size_t kViewBase = 0x10;      // FD3D12View::ResourceInfo.BaseResource
constexpr size_t kViewLocation = 0x18;  // ResourceInfo.ResourceLocation
constexpr size_t kViewResource = 0x20;  // ResourceInfo.Resource
constexpr size_t kBaseLock = 0x10;      // FD3D12BaseShaderResource::RenameListenersCS (CRITICAL_SECTION)
constexpr size_t kBaseLocation = 0x48;  // FD3D12BaseShaderResource::ResourceLocation
constexpr size_t kLocationResource = 0x10;  // FD3D12ResourceLocation::UnderlyingResource
constexpr size_t kUavCounter = 0x88;        // FD3D12UnorderedAccessView::CounterResource

using CreateViewFn = void (*)(uint8_t* view, uint8_t* const* info, uintptr_t nullDescriptor);
using MovedFn = bool (*)(uint8_t* location, void* newData);
using RenamedFn = void (*)(uint8_t* listener, uint8_t* base, uint8_t* location);

struct State {
    uint8_t* createView = nullptr;
    uint8_t* moved = nullptr;
    uint8_t* uavUpdateDescriptor = nullptr;
    uint8_t* region = nullptr;
    bool found = false;
    bool installed = false;
};
State g;
CreateViewFn g_origCreateView = nullptr;
MovedFn g_origMoved = nullptr;
std::atomic<int> g_mode{0};  // 0 off (pass through), 1 repair (not counter UAVs), 2 count only, 3 repair all

thread_local int t_inGuard = 0;  // > 0 inside the guarded part (nested CreateView calls of the repair pass through)
thread_local int t_inMove = 0;   // > 0 inside OnAllocationMoved on this thread (its rename loop)

std::atomic<uint64_t> g_calls{0}, g_guarded{0}, g_moves{0}, g_stale{0}, g_recentOnly{0}, g_repairs{0},
    g_retarget{0}, g_overflow{0}, g_counterLeft{0};

// Recently moved locations: open addressing, 8 probes, the oldest entry of the probe window is replaced.
constexpr size_t kSlots = 8192;
constexpr size_t kProbe = 8;
constexpr ULONGLONG kRecentMs = 1000;
struct Slot {
    std::atomic<uintptr_t> location{0};
    std::atomic<ULONGLONG> ms{0};
};
Slot g_movedAt[kSlots];

size_t HashOf(uintptr_t p) { return static_cast<size_t>(((p >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - 13)); }

void NoteMove(uint8_t* location)
{
    const uintptr_t key = reinterpret_cast<uintptr_t>(location);
    const ULONGLONG now = GetTickCount64();
    const size_t h = HashOf(key);
    size_t victim = h & (kSlots - 1);
    ULONGLONG oldest = ~0ull;
    for (size_t i = 0; i < kProbe; ++i) {
        Slot& s = g_movedAt[(h + i) & (kSlots - 1)];
        const uintptr_t k = s.location.load(std::memory_order_acquire);
        if (k == key) {
            s.ms.store(now, std::memory_order_release);
            return;
        }
        const ULONGLONG ms = k ? s.ms.load(std::memory_order_relaxed) : 0;
        if (ms < oldest) {
            oldest = ms;
            victim = (h + i) & (kSlots - 1);
        }
    }
    if (oldest != 0 && now - oldest <= kRecentMs) g_overflow.fetch_add(1, std::memory_order_relaxed);
    Slot& s = g_movedAt[victim];
    s.location.store(0, std::memory_order_release);
    s.ms.store(now, std::memory_order_release);
    s.location.store(key, std::memory_order_release);
}

bool MovedRecently(uint8_t* location)
{
    const uintptr_t key = reinterpret_cast<uintptr_t>(location);
    const ULONGLONG now = GetTickCount64();
    const size_t h = HashOf(key);
    for (size_t i = 0; i < kProbe; ++i) {
        const Slot& s = g_movedAt[(h + i) & (kSlots - 1)];
        if (s.location.load(std::memory_order_acquire) != key) continue;
        const ULONGLONG ms = s.ms.load(std::memory_order_acquire);
        return now >= ms ? now - ms <= kRecentMs : true;
    }
    return false;
}

template <typename T>
T& At(uint8_t* p, size_t offset) { return *reinterpret_cast<T*>(p + offset); }

bool MovedHook(uint8_t* location, void* newData)
{
    g_moves.fetch_add(1, std::memory_order_relaxed);
    if (location) NoteMove(location);  // before the location's fields change
    ++t_inMove;
    const bool result = g_origMoved(location, newData);
    --t_inMove;
    return result;
}

void CreateViewHook(uint8_t* view, uint8_t* const* info, uintptr_t nullDescriptor)
{
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const int mode = g_mode.load(std::memory_order_relaxed);
    uint8_t* const base = info ? info[0] : nullptr;
    uint8_t* const oldBase = At<uint8_t*>(view, kViewBase);
    if (mode == 0 || !base || t_inGuard || t_inMove) {
        g_origCreateView(view, info, nullDescriptor);
        return;
    }
    if (oldBase) {  // an update of a registered view (rename loop) or a view moved to another resource
        if (oldBase != base) g_retarget.fetch_add(1, std::memory_order_relaxed);
        g_origCreateView(view, info, nullDescriptor);
        return;
    }
    g_guarded.fetch_add(1, std::memory_order_relaxed);
    ++t_inGuard;
    auto* const lock = reinterpret_cast<CRITICAL_SECTION*>(base + kBaseLock);
    EnterCriticalSection(lock);
    g_origCreateView(view, info, nullDescriptor);
    uint8_t* const location = At<uint8_t*>(view, kViewLocation);
    if (location == base + kBaseLocation && At<uint8_t*>(view, kViewBase) == base) {
        uint8_t* const viewResource = At<uint8_t*>(view, kViewResource);
        uint8_t* const current = At<uint8_t*>(location, kLocationResource);
        const bool stale = viewResource != current;
        const bool recent = !stale && MovedRecently(location);
        if (stale) {
            const uint64_t n = g_stale.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 20)
                Log("[view] %s view %p of base %p: Resource %p, its location %p now uses %p",
                    mode == 2 ? "found stale (count only)" : "found stale", view, base, viewResource, location,
                    current);
        } else if (recent) {
            g_recentOnly.fetch_add(1, std::memory_order_relaxed);
        }
        void** const vtable = *reinterpret_cast<void***>(view);
        const bool counterUav = vtable[2] == g.uavUpdateDescriptor && At<void*>(view, kUavCounter) != nullptr;
        if ((stale || recent) && mode == 1 && counterUav) {
            const uint64_t n = g_counterLeft.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 20 && stale)
                Log("[view] stale counter UAV %p left as in the stock game (mode 1)", view);
        } else if ((stale || recent) && (mode == 1 || mode == 3)) {
            g_repairs.fetch_add(1, std::memory_order_relaxed);
            RenamedFn renamed = reinterpret_cast<RenamedFn>(vtable[0]);
            renamed(view, base, location);
        }
    }
    LeaveCriticalSection(lock);
    --t_inGuard;
}

bool Fail(const char* why)
{
    Log("[view] view rename guard: %s, off (buffer defragmentation stays paused while RdgParallelTranslate translates)", why);
    return false;
}

uint8_t* RipTarget(uint8_t* insn, size_t dispOffset, size_t length)
{
    int32_t disp;
    memcpy(&disp, insn + dispOffset, sizeof disp);
    return insn + length + disp;
}

}  // namespace

bool FindViewRenameGuard()
{
    if (g.found) return true;
    const GameModule& game = Game();
    const auto create = FindAll(game.text, ParsePattern(kCreateViewPattern));
    if (create.size() != 1) return Fail("FD3D12View::CreateView not found exactly once");
    uint8_t* const cv = create[0];
    if (!MatchesAt(cv + kCreateViewInfoCopy, ParsePattern(kCreateViewInfoCopyPattern)) ||
        !MatchesAt(cv + kCreateViewUpdateDescriptor, ParsePattern(kUpdateDescriptorPattern)))
        return Fail("FD3D12View::CreateView's body differs");
    const auto loop = FindAll(game.text, ParsePattern(kMovedLoopPattern));
    if (loop.size() != 1) return Fail("OnAllocationMoved's rename loop not found exactly once");
    uint8_t* moved = nullptr;
    const Pattern start = ParsePattern(kMovedStartPattern);
    for (uint8_t* p = loop[0] - 1; p > loop[0] - 0x400 && p >= game.text.begin; --p) {
        if (!MatchesAt(p, start)) continue;
        if (moved) return Fail("OnAllocationMoved's start ambiguous");
        moved = p;
    }
    if (!moved) return Fail("OnAllocationMoved's start not found");
    const auto store = FindAll(Section{moved, loop[0]}, ParsePattern(kMovedStoreResourcePattern));
    if (store.size() != 1) return Fail("OnAllocationMoved's store of the new resource not found");
    // Both lock calls must go through the game's EnterCriticalSection import.
    void** enter = FindImportSlot(game.base, "KERNEL32.dll", "EnterCriticalSection");
    if (!enter || RipTarget(cv + kCreateViewEnterCall, 2, 6) != reinterpret_cast<uint8_t*>(enter) ||
        RipTarget(loop[0] + kMovedLoopEnterCall, 2, 6) != reinterpret_cast<uint8_t*>(enter))
        return Fail("the listener lock is not an EnterCriticalSection call");
    const auto uavUpdate = FindAll(game.text, ParsePattern(kUavUpdateDescriptorPattern));
    if (uavUpdate.size() != 1) return Fail("FD3D12UnorderedAccessView::UpdateDescriptor not found exactly once");
    if (memcmp(cv, kCreateViewPrologue, sizeof kCreateViewPrologue) != 0 ||
        memcmp(moved, kMovedPrologue, sizeof kMovedPrologue) != 0)
        return Fail("prologue differs");
    g.createView = cv;
    g.moved = moved;
    g.uavUpdateDescriptor = uavUpdate[0];
    g.found = true;
    return true;
}

// Start-up only (before the game's code runs): two 5-byte jumps.
bool InstallViewRenameGuard(int mode)
{
    if (g.installed) {
        g_mode.store(mode);
        return true;
    }
    if (!FindViewRenameGuard()) return false;
    const GameModule& game = Game();
    if (!g.region) g.region = AllocNearGame(0x1000);
    if (!g.region) return Fail("no free memory near the game's code");
    // region: [0x00] CreateView trampoline, [0x20] stub -> CreateViewHook, [0x40] OnAllocationMoved trampoline,
    // [0x60] stub -> MovedHook
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    auto emitJump = [&](uint8_t* at, const void* to) {
        memcpy(at, jmpAbs, sizeof jmpAbs);
        memcpy(at + sizeof jmpAbs, &to, sizeof to);
    };
    memcpy(g.region, kCreateViewPrologue, sizeof kCreateViewPrologue);
    emitJump(g.region + sizeof kCreateViewPrologue, g.createView + sizeof kCreateViewPrologue);
    emitJump(g.region + 0x20, reinterpret_cast<const void*>(&CreateViewHook));
    memcpy(g.region + 0x40, kMovedPrologue, sizeof kMovedPrologue);
    emitJump(g.region + 0x40 + sizeof kMovedPrologue, g.moved + sizeof kMovedPrologue);
    emitJump(g.region + 0x60, reinterpret_cast<const void*>(&MovedHook));
    DWORD old;
    if (!VirtualProtect(g.region, 0x1000, PAGE_EXECUTE_READ, &old)) return Fail("cannot make the stubs executable");
    FlushInstructionCache(GetCurrentProcess(), g.region, 0x80);
    if (!g_origCreateView) g_origCreateView = reinterpret_cast<CreateViewFn>(g.region);
    if (!g_origMoved) g_origMoved = reinterpret_cast<MovedFn>(g.region + 0x40);

    auto jumpTo = [](uint8_t* from, uint8_t* to, uint8_t (&out)[5]) {
        const int64_t rel = to - (from + 5);
        if (rel != static_cast<int32_t>(rel)) return false;
        const int32_t rel32 = static_cast<int32_t>(rel);
        out[0] = 0xE9;
        memcpy(out + 1, &rel32, sizeof rel32);
        return true;
    };
    uint8_t jmpCreate[5], jmpMoved[5];
    if (!jumpTo(g.createView, g.region + 0x20, jmpCreate) || !jumpTo(g.moved, g.region + 0x60, jmpMoved))
        return Fail("the hook is out of reach");
    g_mode.store(mode);
    // OnAllocationMoved first: the guard's "recently moved" record must exist before views are guarded.
    if (!WriteCode(g.moved, jmpMoved)) return Fail("could not place the OnAllocationMoved detour");
    if (!WriteCode(g.createView, jmpCreate)) {
        WriteCode(g.moved, std::span<const uint8_t>(kMovedPrologue, 5));
        return Fail("could not place the CreateView detour");
    }
    g.installed = true;
    Log("[view] view rename guard installed (%s): FD3D12View::CreateView exe+0x%X, OnAllocationMoved exe+0x%X, "
        "UAV UpdateDescriptor exe+0x%X",
        mode == 1 ? "repair, counter UAVs left as stock" : mode == 3 ? "repair all" : "count only",
        static_cast<unsigned>(g.createView - game.base), static_cast<unsigned>(g.moved - game.base),
        static_cast<unsigned>(g.uavUpdateDescriptor - game.base));
    return true;
}

void SetViewRenameGuardMode(int mode) { g_mode.store(mode); }
bool ViewRenameGuardRepairing() { return g.installed && (g_mode.load() == 1 || g_mode.load() == 3); }

void LogViewRenameGuardStats()
{
    if (!g.installed) return;
    Log("[view] view rename guard: %llu view creations (%llu first registrations guarded, %llu re-targeted), %llu moves; "
        "%llu stale views found, %llu moved-recently re-checks, %llu repairs, %llu counter UAVs left as stock; "
        "%llu young move records evicted",
        static_cast<unsigned long long>(g_calls.load()), static_cast<unsigned long long>(g_guarded.load()),
        static_cast<unsigned long long>(g_retarget.load()), static_cast<unsigned long long>(g_moves.load()),
        static_cast<unsigned long long>(g_stale.load()), static_cast<unsigned long long>(g_recentOnly.load()),
        static_cast<unsigned long long>(g_repairs.load()), static_cast<unsigned long long>(g_counterLeft.load()),
        static_cast<unsigned long long>(g_overflow.load()));
}

bool ViewRenameGuardInfo(ViewRenameGuardInfoData* out)
{
    if (!g.found || !out) return false;
    out->createView = g.createView;
    out->onAllocationMoved = g.moved;
    out->stale = g_stale.load();
    out->recentOnly = g_recentOnly.load();
    out->repairs = g_repairs.load();
    out->guarded = g_guarded.load();
    out->moves = g_moves.load();
    out->uavUpdateDescriptor = g.uavUpdateDescriptor;
    out->counterLeft = g_counterLeft.load();
    return true;
}

void ViewRenameGuardTestSetOriginals(void* createView, void* onAllocationMoved)
{
    g_origCreateView = reinterpret_cast<CreateViewFn>(createView);
    g_origMoved = reinterpret_cast<MovedFn>(onAllocationMoved);
}

}  // namespace opf
