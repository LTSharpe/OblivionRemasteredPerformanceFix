// GpuPoolKeepMB (worker asyncpool, 2026-11-18; review fixes poolkeepfix; S-001, S-002): empty D3D12 texture and buffer
// memory pools are kept, up to a budget, instead of being destroyed 20 frames after they went empty and created again at the
// next streaming burst.
//
// UE 5.3's D3D12 RHI sub-allocates textures and buffers from memory pools (FD3D12PoolAllocator: 64 MB pools for streamed
// read-only textures, 4 MB for small ones, 32 MB for buffers). FD3D12Adapter::EndFrame calls each allocator's
// CleanUpAllocations with a frame lag of 20 (hard-coded: D3D12Allocation.cpp FD3D12TextureAllocatorPool::CleanUpAllocations,
// D3D12CommandContext.cpp BufferPoolDeletionFrameLag; unchanged through 5.7.4 and ue5-main), which destroys every pool that
// has been empty for 20 frames. Riding through the world, texture streaming and new buffers fill and empty pools all the
// time, so the same memory is destroyed and created again within seconds: in our IC flight (play108, 2 min in play) 61
// buffer pools were created on the render thread and 10 texture pools on streaming workers while the game's video memory
// stayed flat. A creation happens inside FRHIPoolAllocator::TryAllocateInternal with the allocator's lock held, and waits
// for Windows to create and make resident 32-64 MB (1-9 ms on the render thread per frame in that flight; 16.6 ms once on a
// streaming worker, with the render and RHI threads queued behind that lock: the IC bridge frame of findings "bridge").
// A destruction is the same lock held on the RHI thread at EndFrame.
//
// Epic's later engine (ue5-main, after 5.7.4; FRHIPoolAllocator with ERHIPoolAllocationStrategy::Async and the cvars
// d3d12.UploadHeap.OverFlowPoolsCount / d3d12.DefaultBuffer.OverFlowPoolsCount) attacks the same wait by creating a spare
// pool ahead on a background task, for upload and default-buffer allocators. This key takes the other half: it removes
// the re-creations (most of the creations in our data) by not destroying what will be needed again; its keep is ours.
//
// How: the 20-frame call sites (`xor r8d,r8d; lea edx,[r8+14h]; call CleanUpAllocations`: EndFrame's texture-allocator
// loop and default-buffer loop, and the out-of-line texture clean-up) call our hook. Only allocators with defragmentation
// on are kept (the 64 MB read-only texture pools and the video-memory buffer pools: the ones that churn in our data). There
// the game sorts empty pools last for new allocations, so a kept pool is used only when nothing else has room, exactly where
// the game would create a new pool. Allocators without defragmentation (4 MB small textures, read-back buffers, ray-tracing
// structures) order pools by index, so a kept pool would take allocations ahead of free space in other pools: they stay as
// the game has them.
// Under the allocator's own lock (the critical section the function takes itself; it is recursive) the hook lists the empty
// pools that the game would destroy now (empty, last used 20 or more frames ago). All allocators' kept pools are ranked
// together by their last use (one frame fence per adapter); the most recently used stay while their bytes fit the budget.
// An allocator with a pool outside the budget passes the game's function a frame lag that destroys only its oldest empty
// pool (and any pool exactly as old), so a budget cut, a burst of pools going empty at once or a list longer than ours is
// given back one pool per allocator per frame, never more at once than the game would. Otherwise it passes a lag that
// destroys none. The lag passed is never below the game's 20, so no pool is destroyed earlier than in the game.
//
// Defragmentation: FRHIPoolAllocator::Defrag walks a cursor (LastDefragPoolIndex) over the pools and moves the allocations of
// the pool at the cursor into fuller pools; the cursor stays on that pool until it is gone (destroyed: its slot is empty and
// the walk skips it). A kept drained pool would hold the cursor for good, so the hook moves the cursor past the kept pools it
// sits on, as the walk does over the empty slots the game leaves (checked against Defrag's own `inc [rdi+60h]` step).
//
// Identical results: an empty pool holds no resource; a later allocation is placed into a kept pool instead of a new one
// (placement only: same resources, same contents, same picture). Cost: the kept pools' video memory: at most GpuPoolKeepMB,
// plus, while they are given back one per allocator per frame, pools the game itself would already have destroyed.
// Given back (budget 0): MemoryRelief (RAM or commit short, its video-memory stages, a GPU that uses system RAM), and
// while the game's video memory plus the budget is over the DXGI budget (CommandAllocatorTrim's "tight" step with this key's
// budget as the headroom; it ends 512 MB below), and for good if the frame fence cannot be read.
//
// [Fixes] GpuPoolKeepMB = 0: not installed (start-up); > 0: installed, the budget in MB, live (0 later = give back).
#include "common.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace opf {
namespace {

// FD3D12PoolAllocator::CleanUpAllocations(uint64 FrameLag, bool bForceFree), exe+0x251B2A0 in build 19115871:
// mov [rsp+18h],r8b; mov [rsp+10h],rdx; push rbx..r15; sub rsp,28h; lea rbx,[rcx+28h] (the allocator's CS); mov rsi,rcx;
// mov rcx,rbx; movzx r12d,r8b; call [EnterCriticalSection]; mov rax,[rsi+88h] (device); xor r14d,r14d; xor r13d,r13d;
// lea rbp,[r14-1]; mov r9,[rax+10h] (adapter); mov r15,[r9+548h] (frame fence)
constexpr char kPrologue[] =
    "44 88 44 24 18 48 89 54 24 10 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 28 48 8D 59 28 48 8B F1 48 8B CB 45 0F B6 E0 "
    "FF 15 ?? ?? ?? ?? 48 8B 86 88 00 00 00 45 33 F6 45 33 ED 49 8D 6E FF 4C 8B 48 10 4D 8B B9 48 05 00 00";
constexpr size_t kLockCall = 40;  // the prologue's `call [rip+rel32]` (the lock's import)
// GetCompletedFenceValue(true) inlined: the minimum of FencePairs' (+8 data, +10h num, 16-byte elements) ID3D12Fence
// GetCompletedValue (vtable +40h), stored as the cached value at +20h
constexpr char kFenceLoop[] =
    "49 8B 7F 08 4D 63 77 10 49 C1 E6 04 4C 03 F7 49 3B FE 74 ?? 66 0F 1F 44 00 00 48 8B 0F 48 8B 01 FF 50 40 48 3B C5 "
    "48 0F 42 E8 48 83 C7 10 49 3B FE 75 E7 49 89 6F 20";
// the trim loop: Pools data +50h, pool PoolSize +10h == FreeSize +28h (IsEmpty), LastUsedFrameFence +0D8h + FrameLag <= completed
constexpr char kTrimLoop[] =
    "48 8B 7E 50 49 8B 3C 3E 48 85 FF 74 ?? 48 8B 47 10 48 3B 47 28 75 ?? 45 84 E4 75 ?? 48 8B 8F D8 00 00 00 49 03 CD 48 3B CD";
constexpr size_t kBodyFrom = 0x100, kBodyTo = 0x300;  // both loops lie in this range after the prologue
// FRHIPoolAllocator::Defrag (exe+0x1CBEAD0 area): its entry `cmp byte [rcx+24h],0` (bDefragEnabled; return when off); mov rdi,rcx;
// je; mov [rax+20h],rbx; lea rbx,[rcx+28h] (the same CS)
constexpr char kDefragEntry[] = "80 79 24 00 48 8B F9 0F 84 ?? ?? ?? ?? 48 89 58 20 48 8D 59 28";
// ... and its cursor step: mov eax,[rdi+60h] (LastDefragPoolIndex); inc eax; mov [rdi+60h],eax; cmp eax,[rdi+58h] (Pools.Num)
constexpr char kDefragStep[] = "8B 47 60 FF C0 89 47 60 3B 47 58";
constexpr size_t kDefragSpan = 0x800;  // the step lies this close after the entry check (0x417 in build 19115871)
// a 20-frame call site: xor r8d,r8d (bForceFree = false); lea edx,[r8+14h] (FrameLag = 20); call CleanUpAllocations
constexpr char kSite[] = "45 33 C0 41 8D 50 14 E8";
constexpr size_t kSiteCall = 7;
constexpr int kMaxSites = 8;

constexpr uint32_t kDefragOn = 0x24, kCs = 0x28, kPools = 0x50, kPoolNum = 0x58, kCursor = 0x60, kDevice = 0x88, kAdapter = 0x10,
                   kFrameFence = 0x548, kCompleted = 0x20;
constexpr uint32_t kPoolSize = 0x10, kFreeSize = 0x28, kLastUsed = 0xD8;
constexpr uint64_t kGameLag = 20;
constexpr uint64_t kNever = 1ull << 40;  // a frame lag no fence reaches (60 fps: 580 years)
constexpr uint64_t kStaleFrames = 600;   // kept entries of an allocator whose clean-up has not run for this long are dropped
constexpr int kMaxKept = 1024;           // kept pools of all allocators (the budget at 4096 MB holds 128 of 32 MB)
constexpr int kMaxCand = 512;            // one allocator's empty pools past 20 frames that one call lists

using CleanFn = void (*)(void* allocator, uint64_t frameLag, bool forceFree);
CleanFn g_orig = nullptr;
CleanFn g_testOrig = nullptr;  // self-test only
bool g_installed = false;
std::atomic<uint64_t> g_budget{0};  // bytes; 0 = the game's 20 frames (kept pools given back one per allocator per frame)
uint64_t g_budgetMB = 0;
std::atomic<bool> g_failed{false};  // the frame fence could not be read: budget 0 until the game restarts

// One pool kept beyond the game's 20 frames (or a candidate in the ranking).
struct Ent {
    const void* alloc;
    const void* pool;
    uint64_t last, size;
    uint64_t seen;  // the completed frame at the owner's last call (staleness)
    bool mine, in;
};
SRWLOCK g_lock = SRWLOCK_INIT;  // a leaf lock: taken inside the allocator's CS, nothing is taken under it
Ent g_kept[kMaxKept];
int g_nKept = 0;
// scratch for one call (under g_lock)
Ent g_rank[kMaxKept + kMaxCand];
const void* g_prev[kMaxKept];

std::atomic<uint64_t> g_lastCompleted{0};
std::atomic<uint64_t> g_reused{0}, g_released{0}, g_maxKept{0}, g_cursorMoves{0}, g_overflow{0}, g_fenceFaults{0};
std::atomic<bool> g_anyKept{false};  // some allocator keeps pools beyond the game's 20 frames
bool g_failLogged = false;

// The ranking (under g_lock). r: the other allocators' kept pools and the caller's candidates (mine). The most recently used
// stay while their bytes fit the budget; the first that does not fit ends the list (no older pool is kept in its place).
// The caller's lag: kNever when all its candidates stay, else the age of its oldest (that one goes now, and any as old).
// overflow: the caller had more candidates than listed (oldestAll = the oldest of all of them); room: table entries left.
uint64_t Decide(Ent* r, int n, uint64_t budget, uint64_t completed, bool overflow, uint64_t oldestAll, int room, uint64_t* keptBytes,
                int* keptCount)
{
    std::sort(r, r + n, [](const Ent& x, const Ent& y) { return x.last > y.last; });  // most recently used first
    uint64_t cum = 0, oldest = oldestAll;
    bool fits = true, out = overflow;
    int mineN = 0;
    for (int i = 0; i < n; ++i) {
        r[i].in = fits && cum + r[i].size <= budget;
        if (r[i].in) cum += r[i].size;
        else fits = false;
        if (!r[i].mine) continue;
        ++mineN;
        oldest = std::max(oldest, completed - r[i].last);
        if (!r[i].in) out = true;
    }
    if (mineN > room) out = true;  // the table cannot hold them: give back as over the budget
    const uint64_t lag = out ? std::max(kGameLag, oldest) : kNever;
    uint64_t kb = 0;
    int kc = 0;
    for (int i = 0; i < n; ++i)
        if (r[i].mine && completed - r[i].last < lag) kb += r[i].size, ++kc;
    *keptBytes = kb;
    *keptCount = kc;
    return lag;
}

bool ReadCompleted(const uint8_t* a, uint64_t* out)
{
    __try {
        const uint8_t* dev = *reinterpret_cast<uint8_t* const*>(a + kDevice);
        const uint8_t* ad = *reinterpret_cast<uint8_t* const*>(dev + kAdapter);
        const uint8_t* fence = *reinterpret_cast<uint8_t* const*>(ad + kFrameFence);
        *out = *reinterpret_cast<const uint64_t*>(fence + kCompleted);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct PoolView {
    uint64_t size, last;
    bool empty;
};
PoolView View(const uint8_t* p)
{
    PoolView v;
    v.size = *reinterpret_cast<const uint64_t*>(p + kPoolSize);
    v.empty = v.size == *reinterpret_cast<const uint64_t*>(p + kFreeSize);
    v.last = *reinterpret_cast<const uint64_t*>(p + kLastUsed);
    return v;
}

void Hook(void* self, uint64_t lag, bool force)
{
    CleanFn const orig = g_testOrig ? g_testOrig : g_orig;
    uint8_t* const a = static_cast<uint8_t*>(self);
    // not ours (another lag, a forced free), off with nothing kept, or an allocator without defragmentation: the game's call
    if (force || lag != kGameLag || (!g_budget.load(std::memory_order_relaxed) && !g_anyKept.load(std::memory_order_relaxed)) ||
        !a[kDefragOn]) {
        orig(self, lag, force);
        return;
    }
    auto* const cs = reinterpret_cast<CRITICAL_SECTION*>(a + kCs);
    EnterCriticalSection(cs);  // the allocator's own lock (the game's function takes it again: recursive)
    uint64_t completed = 0;
    if (ReadCompleted(a, &completed)) g_lastCompleted.store(completed, std::memory_order_relaxed);
    else {
        g_fenceFaults.fetch_add(1, std::memory_order_relaxed);
        g_failed.store(true, std::memory_order_relaxed);
        completed = g_lastCompleted.load(std::memory_order_relaxed);  // ages from the last good reading: give back one per frame
    }
    const uint64_t budget = g_failed.load(std::memory_order_relaxed) ? 0 : g_budget.load(std::memory_order_relaxed);
    uint64_t useLag = kGameLag;
    AcquireSRWLockExclusive(&g_lock);
    if (completed) {
        // this allocator's entries come out of the table (rebuilt from its pools now); others' stale entries are dropped
        int nPrev = 0, keep = 0;
        for (int i = 0; i < g_nKept; ++i) {
            const Ent& k = g_kept[i];
            if (k.alloc == self) g_prev[nPrev++] = k.pool;
            else if (k.seen + kStaleFrames >= completed) g_kept[keep++] = k;
        }
        g_nKept = keep;
        int n = 0;
        for (int i = 0; i < g_nKept; ++i) {
            g_rank[n] = g_kept[i];
            g_rank[n++].mine = false;  // the other allocators' kept pools (stored with mine set by their owner)
        }
        uint8_t* const* const pools = *reinterpret_cast<uint8_t* const* const*>(a + kPools);
        const int32_t num = *reinterpret_cast<const int32_t*>(a + kPoolNum);
        bool overflow = false;
        uint64_t oldestAll = 0, reused = 0;
        int cand = 0;
        for (int32_t i = 0; i < num && pools; ++i) {
            const uint8_t* const p = pools[i];
            if (!p) continue;
            const PoolView v = View(p);
            const bool old = v.empty && v.last + kGameLag <= completed;
            if (!old) {
                for (int t = 0; t < nPrev; ++t)  // a pool kept last time that is in use again (or freshly used)
                    if (g_prev[t] == p) { ++reused; break; }
                continue;
            }
            oldestAll = std::max(oldestAll, completed - v.last);
            if (cand == kMaxCand) { overflow = true; continue; }
            g_rank[n++] = {self, p, v.last, v.size, completed, true, false};
            ++cand;
        }
        g_reused.fetch_add(reused, std::memory_order_relaxed);
        if (overflow) g_overflow.fetch_add(1, std::memory_order_relaxed);
        uint64_t keptBytes = 0;
        int keptCount = 0;
        useLag = cand || overflow ? Decide(g_rank, n, budget, completed, overflow, oldestAll, kMaxKept - g_nKept, &keptBytes, &keptCount)
                                  : kNever;
        if (useLag != kNever) g_released.fetch_add(static_cast<uint64_t>(cand - keptCount), std::memory_order_relaxed);
        for (int i = 0; i < n && g_nKept < kMaxKept; ++i)
            if (g_rank[i].mine && completed - g_rank[i].last < useLag) g_kept[g_nKept++] = g_rank[i];
        // Defrag's cursor: past the kept pools it sits on, as its walk passes the empty slots the game leaves (stock: the pool is
        // destroyed now, its slot skipped). At or past the end the walk starts over at 0 (as Defrag does itself).
        int32_t* const cursor = reinterpret_cast<int32_t*>(a + kCursor);
        int32_t at = *cursor;
        if (at >= num) at = 0;
        const int32_t from = at;
        // Empty slots in between are passed too (the walk skips them itself, review SF1), but only when a kept pool follows them:
        // trailing empty slots stay for the game's own walk, so its pace over them is the stock one.
        int32_t end = at;
        for (int32_t j = at; j >= 0 && j < num && pools; ++j) {
            if (!pools[j]) continue;
            const PoolView v = View(pools[j]);
            if (!(v.empty && v.last + kGameLag <= completed && completed - v.last < useLag)) break;
            end = j + 1;
        }
        at = end;
        if (at != from) {
            *cursor = at;
            g_cursorMoves.fetch_add(1, std::memory_order_relaxed);
        }
        uint64_t total = 0;
        for (int i = 0; i < g_nKept; ++i) total += g_kept[i].size;
        g_anyKept.store(g_nKept != 0, std::memory_order_relaxed);
        if (total > g_maxKept.load(std::memory_order_relaxed)) g_maxKept.store(total, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_lock);
    orig(self, useLag, false);
    LeaveCriticalSection(cs);
}

struct Found {
    uint8_t* fn = nullptr;
    uint8_t* calls[kMaxSites] = {};
    int n = 0;
    uint8_t* defragEntry = nullptr;
    uint8_t* defragStep = nullptr;
};

bool InRange(uint8_t* fn, const char* pattern)
{
    const Pattern p = ParsePattern(pattern);
    for (uint8_t* q = fn + kBodyFrom; q < fn + kBodyTo; ++q)
        if (MatchesAt(q, p)) return true;
    return false;
}

// True if `slot` is the exe's import entry for KERNEL32's EnterCriticalSection (a recursive lock: the hook enters the
// allocator's lock and the game's function enters it again): the import directory names it (that holds for a mapped copy
// too), and its value is the file's (the name entry's RVA: a mapped copy, or not resolved yet) or the function Windows exports.
bool ImportIsEnterCriticalSection(const GameModule& game, const uint8_t* slot)
{
    if (slot < game.base || slot + sizeof(void*) > game.base + game.sizeOfImage) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(game.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.VirtualAddress + dir.Size > game.sizeOfImage) return false;
    bool named = false;
    uint64_t nameRva = 0;
    for (const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(game.base + dir.VirtualAddress); desc->Name && !named;
         ++desc) {
        if (desc->Name >= game.sizeOfImage || _stricmp(reinterpret_cast<const char*>(game.base + desc->Name), "KERNEL32.dll") != 0) continue;
        const DWORD namesRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        if (namesRva >= game.sizeOfImage || desc->FirstThunk >= game.sizeOfImage) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(game.base + namesRva);
        const auto* entries = reinterpret_cast<const IMAGE_THUNK_DATA64*>(game.base + desc->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
            if (reinterpret_cast<const uint8_t*>(entries + i) != slot) continue;
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal) || names[i].u1.AddressOfData >= game.sizeOfImage) break;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(game.base + names[i].u1.AddressOfData);
            named = strcmp(byName->Name, "EnterCriticalSection") == 0;
            nameRva = names[i].u1.AddressOfData;
            break;
        }
    }
    if (!named) return false;
    const uint64_t value = *reinterpret_cast<const uint64_t*>(slot);
    if (value == nameRva) return true;
    const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    return k32 && value == reinterpret_cast<uint64_t>(GetProcAddress(k32, "EnterCriticalSection"));
}

bool Discover(const GameModule& game, Found* f, const char** why)
{
    const auto a = FindAll(game.text, ParsePattern(kPrologue));
    if (a.size() != 1) { *why = "CleanUpAllocations' start not found once"; return false; }
    uint8_t* const fn = a[0];
    if (!InRange(fn, kFenceLoop)) { *why = "CleanUpAllocations' frame-fence read does not match"; return false; }
    if (!InRange(fn, kTrimLoop)) { *why = "CleanUpAllocations' empty-pool loop does not match"; return false; }
    if (!ImportIsEnterCriticalSection(game, RipTarget(fn + kLockCall, 2, 6))) {
        *why = "CleanUpAllocations' lock is not EnterCriticalSection";
        return false;
    }
    const auto de = FindAll(game.text, ParsePattern(kDefragEntry));
    const auto ds = FindAll(game.text, ParsePattern(kDefragStep));
    if (de.size() != 1 || ds.size() != 1 || ds[0] <= de[0] || static_cast<size_t>(ds[0] - de[0]) > kDefragSpan) {
        *why = "Defrag's switch (+24h) or cursor (+60h) does not match";
        return false;
    }
    f->defragEntry = de[0];
    f->defragStep = ds[0];
    for (uint8_t* p : FindAll(game.text, ParsePattern(kSite))) {
        uint8_t* const call = p + kSiteCall;
        if (RipTarget(call, 1, 5) != fn) continue;
        if (f->n == kMaxSites) { *why = "more 20-frame call sites than expected"; return false; }
        f->calls[f->n++] = call;
    }
    if (f->n == 0) { *why = "no 20-frame call site"; return false; }
    f->fn = fn;
    return true;
}

Found g_found;
bool g_tight = false;  // video memory plus the budget over the DXGI budget (GpuPoolKeepBudgetStep)

}  // namespace

bool InstallGpuPoolKeep(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!beforeGameCode) { Log("[poolkeep] GpuPoolKeepMB: only installed while the game starts"); return false; }
    const GameModule& game = Game();
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) { Log("[poolkeep] GpuPoolKeepMB: %s: not installed", why); return false; }
    uint8_t* const mem = AllocNearGame(16);
    if (!mem) { Log("[poolkeep] GpuPoolKeepMB: no memory near the game's code: not installed"); return false; }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    const void* hook = reinterpret_cast<const void*>(&Hook);
    memcpy(mem, jmpAbs, 6);
    memcpy(mem + 6, &hook, 8);
    DWORD old;
    if (!VirtualProtect(mem, 16, PAGE_EXECUTE_READ, &old)) { Log("[poolkeep] GpuPoolKeepMB: stub not executable: not installed"); return false; }
    FlushInstructionCache(GetCurrentProcess(), mem, 16);
    g_orig = reinterpret_cast<CleanFn>(f.fn);
    int32_t rel[kMaxSites] = {};
    for (int i = 0; i < f.n; ++i) {  // all in reach before anything is written
        const int64_t d = mem - (f.calls[i] + 5);
        if (d != static_cast<int32_t>(d)) { Log("[poolkeep] GpuPoolKeepMB: the stub is out of reach: not installed"); return false; }
        rel[i] = static_cast<int32_t>(d);
    }
    for (int i = 0; i < f.n; ++i) {
        if (!WriteCode(f.calls[i] + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel[i]), 4))) {
            for (int j = 0; j < i; ++j) {  // put the ones written back
                const int32_t back = static_cast<int32_t>(f.fn - (f.calls[j] + 5));
                WriteCode(f.calls[j] + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&back), 4));
            }
            Log("[poolkeep] GpuPoolKeepMB: could not patch a call site: not installed");
            return false;
        }
    }
    g_found = f;
    g_installed = true;
    Log("[poolkeep] GpuPoolKeepMB installed: CleanUpAllocations exe+0x%X, %d call site(s) with the 20-frame lag (first exe+0x%X); "
        "Defrag's cursor step exe+0x%X",
        static_cast<unsigned>(f.fn - game.base), f.n, static_cast<unsigned>(f.calls[0] - game.base),
        static_cast<unsigned>(f.defragStep - game.base));
    return true;
}

void SetGpuPoolKeep(int mb)
{
    if (mb < 0) mb = 0;
    if (mb > 4096) mb = 4096;
    const uint64_t bytes = static_cast<uint64_t>(mb) << 20;
    if (g_budget.exchange(bytes) != bytes && g_installed) {
        if (mb) Log("[poolkeep] keeping empty texture and buffer pools up to %d MB (game: destroyed 20 frames after going empty)", mb);
        else Log("[poolkeep] kept pools given back, one per allocator per frame; then the game's 20 frames");
    }
    g_budgetMB = static_cast<uint64_t>(mb);
}

int GpuPoolKeepBudgetStep(int mb, bool relief, uint64_t vram, uint64_t vramBudget)
{
    const bool was = g_tight;
    // CommandAllocatorTrim's "tight" step with this key's budget as the headroom (on with it, off 512 MB below; unknown: as before)
    g_tight = mb > 0 && AllocatorTrimTightStep(g_tight, vram, vramBudget, static_cast<uint64_t>(mb) << 20, false);
    if (g_tight != was && g_installed)
        Log(g_tight ? "[poolkeep] video memory %.2f GB + the %d MB budget is over the %.2f GB budget: kept pools given back"
                    : "[poolkeep] video memory %.2f GB leaves room for the %d MB budget again (budget %.2f GB): keeping pools",
            vram / 1073741824.0, mb, vramBudget / 1073741824.0);
    return relief || g_tight ? 0 : mb;
}

int GpuPoolKeepBudgetNow(int mb, bool relief)
{
    uint64_t vram = 0, budget = 0;
    LatestGameVram(&vram, &budget);
    return GpuPoolKeepBudgetStep(mb, relief, vram, budget);
}

void LogGpuPoolKeepStats()
{
    if (!g_installed) return;
    const uint64_t reused = g_reused.exchange(0), released = g_released.exchange(0), moves = g_cursorMoves.exchange(0),
                   overflow = g_overflow.exchange(0);
    if (g_failed.load() && !g_failLogged) {
        g_failLogged = true;
        Log("[poolkeep] GpuPoolKeepMB: the frame fence could not be read (%llu times): kept pools given back, off until the game restarts",
            static_cast<unsigned long long>(g_fenceFaults.load()));
    }
    uint64_t kept = 0;
    int pools = 0;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_nKept; ++i) kept += g_kept[i].size;
    pools = g_nKept;
    ReleaseSRWLockShared(&g_lock);
    const uint64_t maxKept = g_maxKept.exchange(kept);
    if (!reused && !released && !kept && !moves && !overflow) return;
    Log("[poolkeep] kept %d empty pools, %.0f MB (most %.0f MB; budget %llu MB); %llu reused (each a pool creation and destruction "
        "avoided), %llu given back over the budget; defrag cursor moved past a kept pool %llu times%s",
        pools, kept / 1048576.0, maxKept / 1048576.0, static_cast<unsigned long long>(g_budgetMB), static_cast<unsigned long long>(reused),
        static_cast<unsigned long long>(released), static_cast<unsigned long long>(moves),
        overflow ? "; an allocator had more empty pools than listed (given back one per frame)" : "");
}

// ---- self-test ----
int GpuPoolKeepTestDiscover(const GameModule& game, uint32_t* fn, uint32_t* sites, int max)
{
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) return 0;
    *fn = static_cast<uint32_t>(f.fn - game.base);
    for (int i = 0; i < f.n && i < max; ++i) sites[i] = static_cast<uint32_t>(f.calls[i] - game.base);
    return f.n;
}

bool GpuPoolKeepTestDefrag(const GameModule& game, uint32_t* entry, uint32_t* step)
{
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) return false;
    *entry = static_cast<uint32_t>(f.defragEntry - game.base);
    *step = static_cast<uint32_t>(f.defragStep - game.base);
    return true;
}

bool GpuPoolKeepTestLockImport(const GameModule& game, uint32_t fnRva)
{
    return ImportIsEnterCriticalSection(game, RipTarget(game.base + fnRva + kLockCall, 2, 6));
}

uint64_t GpuPoolKeepTestDecide(const uint64_t* ages, const uint64_t* sizes, int n, uint64_t avail, uint64_t* keptBytes, int* keptCount)
{
    static Ent e[kMaxCand];
    if (n > kMaxCand) n = kMaxCand;
    for (int i = 0; i < n; ++i) e[i] = {nullptr, nullptr, 1000 - ages[i], sizes[i], 1000, true, false};
    return Decide(e, n, avail, 1000, false, 0, kMaxKept, keptBytes, keptCount);
}

void GpuPoolKeepTestCall(void* allocator, uint64_t lag, bool force, void* fakeOrig)
{
    g_testOrig = reinterpret_cast<CleanFn>(fakeOrig);
    Hook(allocator, lag, force);
    g_testOrig = nullptr;
}

void GpuPoolKeepTestReset()
{
    AcquireSRWLockExclusive(&g_lock);
    g_nKept = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_reused = 0;
    g_released = 0;
    g_maxKept = 0;
    g_cursorMoves = 0;
    g_overflow = 0;
    g_fenceFaults = 0;
    g_lastCompleted = 0;
    g_failed = false;
    g_anyKept = false;
    g_tight = false;
}

uint64_t GpuPoolKeepTestKept() { uint64_t k = 0; for (int i = 0; i < g_nKept; ++i) k += g_kept[i].size; return k; }
uint64_t GpuPoolKeepTestReused() { return g_reused.load(); }
uint64_t GpuPoolKeepTestCursorMoves() { return g_cursorMoves.load(); }
bool GpuPoolKeepTestFailed() { return g_failed.load(); }
bool GpuPoolKeepTestInstalled() { return g_installed; }

}  // namespace opf
