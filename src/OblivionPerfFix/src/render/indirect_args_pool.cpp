// IndirectArgBufferPool (worker poolargs 2026-11-14, from workers rtalloc / flagflight / rtprobe's measurements; review rev98d;
// [Fixes] IndirectArgBufferPool, default 0 for now (to be played before it is switched on); start-up only, version.dll route
// only): the engine's own console variable d3d12.AllowPoolAllocateIndirectArgBuffers set to 1, Epic's default since UE 5.5.
//
// The problem. In UE 5.3 the D3D12 RHI makes every buffer with BUF_DrawIndirect (the GPU-written argument buffers of indirect
// draws and dispatches, e.g. the render graph's per-pass indirect args) a committed resource of its own: a separate
// ID3D12Device::CreateCommittedResource (an implicit heap, and with it a paging fence the render thread waits on) for each
// one (D3D12Allocation.cpp FD3D12DefaultBufferAllocator::AllocDefaultResource, behind !GD3D12AllowPoolAllocateIndirectArgBuffers).
// The render graph keeps indirect-arg buffers out of its transient allocator (r.RDG.TransientAllocator.IndirectArgumentBuffers
// 0), so new ones are real RHI creations whenever its buffer pool has none of the size. Counted (they do not depend on outside
// load): 401 -> 11 committed 64 KB buffers per ride on the road to Bravil (rtprobe c2), 188 / 102 -> 0 indirect-arg creations on
// the Imperial City flight and 496 -> 0 on east-crash (flagflight). The time is small and was not shown to remove a stall a
// player sees by itself (flagflight: the flight's long render-thread frames stay; one confounded run pair at Flooded Mine, with
// another setting and other outside load in the "on" run, is not evidence of a frame-time gain). So: less render-thread
// memory-creation work, a small gain. Harm check (flagflight): east-crash (16 scenes) clean, 7 views equal, video memory within
// 0.1 GB.
//
// The fix. With 1 these buffers take the same path as every other GPU buffer: the engine's buffer pool (FD3D12PoolAllocator,
// 32 MB heaps). A buffer with UAV access needs its own resource state, so the pool gives it a placed resource of its own in a
// shared heap (GetResourceAllocationStrategy: MultiState -> kPlacedResource); one without UAV access is sub-allocated in a
// shared read-only resource (GENERIC_READ includes INDIRECT_ARGUMENT). Every ExecuteIndirect call site (D3D12Commands.cpp,
// D3D12RayTracing.cpp) passes the location's offset in its resource, so a non-zero offset is handled. Freeing follows the
// location's own type, so buffers made before and after a change mix safely. Placement only: the same buffers with the same
// contents and the same commands; nothing drawn or simulated changes. Pooled buffers may be moved by the pool's
// defragmentation like every other pooled buffer (paused while RdgParallelTranslate runs).
//
// Why it is safe on 5.3 (Epic's history, gh api, worker poolargs): the variable came in CL 16425509 (2021-05-21, UE 5.0
// Early Access, UE-115982: GPU crashes in indirect draws seen with several vendors) together with the RDG transient opt-out, as
// a precaution; a fix for the same issue followed the next day in the D3D12 command-list sync tracking (a race that dropped
// sync points, CL 16428988), long before 5.3 (Epic did not say the precaution became unneeded; it stayed until 5.5). Epic
// switched the variable on by default for UE 5.5 (commit 97aba58adb, 2024-07-26, UE-218334), after only removing a check()
// that wrongly assumed every indirect-arg buffer has UAV access (c39a14caa1, the day before; check() is compiled out of the
// shipping exe) and an unused console-only define. The allocator code these buffers then use is the 5.3 code every other
// pooled buffer already uses. The RDG transient opt-out stays 0 in 5.5-5.7 and is not touched here.
//
// How. The variable is ECVF_ReadOnly (the console cannot change it; Engine.ini [SystemSettings] can, at start-up); the RHI reads
// its int at every allocation. Found by its registration: lea r8,[int]; lea rdx,[L"d3d12.AllowPoolAllocateIndirectArgBuffers"]
// with the int-reference registration call (IConsoleManager slot +0x38) as the first call after it, then the auto-object's two
// stores (mov [rip+target],rax; mov [rip+vtable],rcx); the int in .data, and exactly two rip-relative references to it in the
// code (any instruction): that registration and cmp dword [int],0 in AllocDefaultResource. Build 19115871: registration
// exe+0x9C2A2B, the int exe+0x91F9E6C (.bss), the test exe+0x24F24DD, the auto-object's vtable / target at exe+0x91F9F30 /
// 0x91F9F38. In Init (version.dll's DllMain; VERSION.dll is a static import of the exe, so before the exe's static initialisers)
// the int is set to 1 if it is still 0 AND the auto-object's vtable and target are still 0 (the variable not yet registered):
// the engine's auto-variable then registers with 1 as its default (it copies the int when it is constructed), as a 5.5 build
// does; the RHI starts much later (FEngineLoop::PreInit), so no indirect-arg buffer is created before the setting, none stays
// committed. Already registered (a late load): nothing written, logged once. A player's Engine.ini value is applied after our
// write and wins (logged by the tick). Loaded as an OBSE64 plugin: not applied, logged. Any mismatch in the search: nothing
// written, one log line. Later ini changes take a restart (logged once). The search (two passes over .text) runs under the
// loader lock in DllMain: its time is in the log line.
#include "common.h"

#include <windows.h>

#include <cstring>

namespace opf {
namespace {

constexpr wchar_t kName[] = L"d3d12.AllowPoolAllocateIndirectArgBuffers";
constexpr int kWatchTicks = 120;  // the tick reports a player's Engine.ini value in the first two minutes

int32_t* g_var = nullptr;
uint64_t* g_vtableSlot = nullptr;  // the auto-object's vtable pointer (0 until its constructor runs)
uint64_t* g_targetSlot = nullptr;  // the auto-object's IConsoleObject* (0 until registered)
bool g_findDone = false, g_findOk = false;
bool g_started = false;
int g_startupKey = 0;
bool g_applied = false;
bool g_restartLogged = false;
bool g_otherLogged = false;
int g_ticks = 0;

const uint8_t* FindWideOnce(const Section& s, const wchar_t* name, size_t bytes)
{
    const uint8_t* hit = nullptr;
    for (const uint8_t* p = s.begin; p + bytes <= s.end; p += 2) {
        if (memcmp(p, name, bytes) != 0) continue;
        if (hit) return nullptr;
        hit = p;
    }
    return hit;
}

// After lea rdx,[name] (its end at `after`): the registration call call [rax+38h] (RegisterConsoleVariableRef, int32&) within
// 24 bytes, with no other call before it. Flags: mov dword [rsp+20h],imm32 on the way, if present (-1 if not). Returns the end
// of the call (nullptr if not found).
const uint8_t* IntRefCallFollows(const uint8_t* after, const uint8_t* end, int64_t* flags)
{
    *flags = -1;
    for (const uint8_t* p = after; p < after + 24 && p + 3 <= end; ++p) {
        if (p + 8 <= end && p[0] == 0xC7 && p[1] == 0x44 && p[2] == 0x24 && p[3] == 0x20) {
            uint32_t f;
            memcpy(&f, p + 4, 4);
            *flags = f;
            p += 7;
            continue;
        }
        if (p[0] == 0xFF && p[1] == 0x50) return p[2] == 0x38 ? p + 3 : nullptr;  // the first virtual call: the int32& registration
        if (p[0] == 0xE8 || (p[0] == 0xFF && (p[1] & 0x38) == 0x10)) return nullptr;  // another call first
    }
    return nullptr;
}

// After the registration call: lea rcx,[vtable] (48 8D 0D rel32), mov [rip+target],rax (48 89 05 rel32), mov [rip+vtable],rcx
// (48 89 0D rel32), within 24 bytes, any order of the two stores; both slots in .data.
bool FindAutoObjectSlots(const uint8_t* after, const GameModule& game, uint64_t** vtable, uint64_t** target)
{
    *vtable = *target = nullptr;
    for (const uint8_t* p = after; p < after + 24 && p + 7 <= game.text.end; ++p) {
        if (p[0] != 0x48 || p[1] != 0x89) continue;
        if (p[2] == 0x05 && !*target) *target = reinterpret_cast<uint64_t*>(RipTarget(p, 3, 7));
        else if (p[2] == 0x0D && !*vtable) *vtable = reinterpret_cast<uint64_t*>(RipTarget(p, 3, 7));
    }
    return *vtable && *target && *vtable != *target && game.data.Contains(*vtable) && game.data.Contains(*target);
}

}  // namespace

bool FindIndirectArgBufferPool()
{
    if (g_findDone) return g_findOk;
    g_findDone = true;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    const GameModule& game = Game();
    const uint8_t* const name = FindWideOnce(game.rdata, kName, sizeof(kName));
    if (!name) {
        Log("[indirectargs] the name d3d12.AllowPoolAllocateIndirectArgBuffers not found (once): IndirectArgBufferPool unavailable");
        return false;
    }
    const auto hits = FindAll(game.text, ParsePattern("4C 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ??"));
    int32_t* var = nullptr;
    const uint8_t* reg = nullptr;
    int regs = 0;
    int64_t flags = -1;
    const uint8_t* callEnd = nullptr;
    for (uint8_t* q : hits) {
        if (RipTarget(q + 7, 3, 7) != name) continue;
        ++regs;
        reg = q;
        var = reinterpret_cast<int32_t*>(RipTarget(q, 3, 7));
        callEnd = IntRefCallFollows(q + 14, game.text.end, &flags);
    }
    uint64_t* vtable = nullptr;
    uint64_t* target = nullptr;
    const bool slots = callEnd && FindAutoObjectSlots(callEnd, game, &vtable, &target);
    if (regs != 1 || !var || !game.data.Contains(var) || !callEnd || !slots) {
        Log("[indirectargs] d3d12.AllowPoolAllocateIndirectArgBuffers: registration not as expected (%d found, int-reference call %s, "
            "auto-object stores %s): IndirectArgBufferPool unavailable", regs, callEnd ? "yes" : "no", slots ? "yes" : "no");
        return false;
    }
    // every rip-relative reference to the int, whatever the instruction: a ModRM with mod 00 rm 101 right before a disp32 whose
    // target (from the end of the instruction: the disp, then the immediate the opcode before the ModRM takes) is the int. The
    // immediate's size comes from the opcode (guessing it would also count a neighbour variable's lea as a reference: the int 4
    // bytes before ours is registered the same way). Expected exactly two: the registration's lea r8 and cmp dword [int],0.
    int refs = 0;
    bool regSeen = false;
    const uint8_t* test = nullptr;
    const uint8_t* const v8 = reinterpret_cast<const uint8_t*>(var);
    for (const uint8_t* d = game.text.begin + 3; d + 8 <= game.text.end; ++d) {
        if ((d[-1] & 0xC7) != 0x05) continue;
        const uint8_t op = d[-2], reg3 = (d[-1] >> 3) & 7;
        const bool op16 = d[-3] == 0x66;
        int imm = 0;
        if (op == 0x80 || op == 0x83 || op == 0xC6 || op == 0x6B || (d[-3] == 0x0F && op == 0xBA)) imm = 1;
        else if (op == 0x81 || op == 0xC7 || op == 0x69 || (op == 0xF7 && reg3 <= 1)) imm = op16 ? 2 : 4;
        int32_t rel;
        memcpy(&rel, d, 4);
        if (d + 4 + imm + rel != v8) continue;
        ++refs;
        if (d == reg + 3 && imm == 0) regSeen = true;
        else if (op == 0x83 && d[-1] == 0x3D && d[4] == 0) test = d - 2;
    }
    if (refs != 2 || !regSeen || !test) {
        Log("[indirectargs] d3d12.AllowPoolAllocateIndirectArgBuffers: %d references to its value in the code (expected 2: the "
            "registration and one test in the D3D12 buffer allocator%s): IndirectArgBufferPool unavailable", refs,
            test ? "" : ", the test not found");
        return false;
    }
    g_var = var;
    g_vtableSlot = vtable;
    g_targetSlot = target;
    g_findOk = true;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    Log("[indirectargs] d3d12.AllowPoolAllocateIndirectArgBuffers at exe+0x%X (now %d, flags 0x%llX; its test at exe+0x%X; "
        "auto-object exe+0x%X; search %.1f ms)",
        static_cast<unsigned>(v8 - game.base), *var, static_cast<unsigned long long>(flags),
        static_cast<unsigned>(test - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(vtable) - game.base),
        1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart));
    return true;
}

// Start-up, once (Init): key != 0 sets the int to 1 while it holds the stock 0 and the variable is not yet registered, only
// before the game's code runs.
void SetIndirectArgBufferPool(int key, bool beforeGameCode)
{
    if (g_started) return;
    g_started = true;
    g_startupKey = key;
    if (key == 0) {
        Log("[indirectargs] IndirectArgBufferPool off: indirect argument buffers stay separate allocations (takes a restart to switch on)");
        return;
    }
    if (!beforeGameCode) {
        Log("[indirectargs] IndirectArgBufferPool: start-up only, as version.dll (the engine has already registered the setting): not applied");
        return;
    }
    if (!FindIndirectArgBufferPool()) return;
    volatile uint64_t* const vt = g_vtableSlot;
    volatile uint64_t* const tg = g_targetSlot;
    // Before WinMain (OBSE64 route) the exe's static set-up has already registered the variable: the RHI still reads the int
    // at every allocation and starts later (PreInit), so writing the int has the same effect; only the console's copy of the
    // value keeps the 0 it was registered with (as NaniteInitialRootPages / PsoCompileThreadPool there). Obseaudit class B.
    const bool registered = *vt != 0 || *tg != 0;
    if (registered && CurrentLoadStage() != LoadStage::BeforeWinMain) {
        Log("[indirectargs] d3d12.AllowPoolAllocateIndirectArgBuffers is already registered (loaded late): not applied");
        return;
    }
    volatile int32_t* const v = g_var;
    if (*v != 0) {
        Log("[indirectargs] d3d12.AllowPoolAllocateIndirectArgBuffers is already %d: left alone", *v);
        return;
    }
    *v = 1;
    g_applied = true;
    Log("[indirectargs] indirect argument buffers from the engine's D3D12 buffer pool (d3d12.AllowPoolAllocateIndirectArgBuffers 1, "
        "Epic's default since UE 5.5; %s; start-up only)",
        registered ? "set before WinMain, after the engine registered it (the console still shows 0)"
                   : "set before the engine registers it");
}

// Once a second (monitor thread): later ini changes are only logged; a value set by the player's Engine.ini is reported once.
void IndirectArgBufferPoolTick(int key)
{
    if (!g_started) return;
    if ((key != 0) != (g_startupKey != 0) && !g_restartLogged) {
        g_restartLogged = true;
        Log("[indirectargs] IndirectArgBufferPool changed in the ini (%d, at start-up %d): takes a restart, ignored while the game runs",
            key, g_startupKey);
    }
    if (!g_applied || g_otherLogged || g_ticks >= kWatchTicks) return;
    ++g_ticks;
    volatile int32_t* const v = g_var;
    if (*v != 1) {
        g_otherLogged = true;
        Log("[indirectargs] d3d12.AllowPoolAllocateIndirectArgBuffers is %d, not our 1 (set by Engine.ini): the player's value is kept", *v);
    }
}

bool IndirectArgBufferPoolApplied() { return g_applied; }
int32_t* IndirectArgBufferPoolVar() { return g_findOk ? g_var : nullptr; }
uint64_t* IndirectArgBufferPoolVtableSlot() { return g_findOk ? g_vtableSlot : nullptr; }

void IndirectArgBufferPoolTestReset()  // self-test only: the start-up state again (the search result is kept)
{
    g_started = false;
    g_startupKey = 0;
    g_applied = false;
    g_restartLogged = false;
    g_otherLogged = false;
    g_ticks = 0;
}

}  // namespace opf
