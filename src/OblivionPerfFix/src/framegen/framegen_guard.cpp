// FrameGenCrashGuard: crash in AMD's FidelityFX library (amd_fidelityfx_dx12.dll, FSR 3 frame
// generation) when its GPU queue falls behind.
//
// The library keeps its command lists in a pool: per queue type 8 slots of {queue, allocator, list, fence, value}
// (40 bytes each, after the pool's CRITICAL_SECTION). Taking one (the function this fix wraps, amd_fidelityfx_dx12.dll
// +0x1D7C0 in the copy the game ships) walks the type's 8 slots under the pool's lock and takes the first whose fence
// has reached its value (a slot not created yet is created then). It never waits: if the GPU has not finished any of
// the 8, the walk ends with no slot and the next instruction reads through a null pointer (`mov rcx,[rdi+8]` with
// rdi = 0). The caller, once per frame-generation dispatch, signals the previous slot's fence on its queue and takes
// the next, so this happens when that queue is 8 dispatches behind the CPU: a long GPU stall. Seen 2026-10-08 and
// twice on 2026-10-13 (4K with frame generation on, at the benchmark's return to the main menu while the world was
// torn down, video memory over budget): EXCEPTION_ACCESS_VIOLATION reading 0x8 at amd_fidelityfx_dx12.dll+0x1D9BF on the
// RHI thread, inside the frame-generation present. The same library code runs in the stock game.
//
// AMD's FSR 3.1.4 DLL (1.0.1.41314, SDK 1.1.4), which players drop in for Adrenalin's ML frame generation upgrade, has
// the same pool (Dx12CommandPool<8>, FidelityFX-SDK FrameInterpolationSwapchainDX12_Helpers.h unchanged from v1.1.0;
// AMD raised it to 32 only in swap chain 3.1.5, SDK 2.0) and the same function compiled with other registers (+0x1FF70,
// 7 calls; the slot in rsi instead of rdi, the unchecked read `mov rcx,[rsi+8]` at +0x2016A). The patterns leave the
// register bytes open and check that the free test and the unchecked use name the same register. The SDK 2.x
// DLLs have 32 slots and need no guard.
//
// The fix redirects the library's calls to that function (6 direct calls) to a wrapper. The wrapper takes the pool's
// lock (a CRITICAL_SECTION, so the function can take it again), checks the slots exactly as the function will, and
// calls it when a slot is free. If none is, it lets the lock go, sleeps a millisecond and checks again, until a slot is
// free (the GPU caught up) or 3 s have passed (then the function runs as before). Nothing else changes: with a free
// slot, which is every call in normal play, the function runs as it always did. Installed from a DLL load
// notification, before any of the library's code runs (the library is loaded by the game while it starts).
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace opf {
namespace {

// The function: push rbx/rdi (3.1.4: rsi)/r12-r15; sub rsp; security cookie; r15 = queue, r13 = pool, [rsp+X] = name;
// queue->GetDesc(&desc) (vtable +0x90, desc.Type first); EnterCriticalSection(pool).
constexpr char kAcquire[] =
    "40 53 ?? 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 ?? 48 8B 02 "
    "4C 8B FA 4C 8B E9 4C 89 44 24 ?? 48 8D 54 24 ?? 49 8B CF FF 90 90 00 00 00 49 8B CD FF 15";
// Its slot: pool + (type * 8 + 1 + i) * 40, the allocator at +8.
constexpr char kSlot[] =
    "48 63 44 24 ?? BD 05 40 00 80 48 8D 0C C5 01 00 00 00 49 03 CE 48 8D 04 89 48 8D 1C C5 00 00 00 00 49 03 DD "
    "48 8B 43 08";
// Free: fence (+0x18)->GetCompletedValue (vtable +0x40) >= value (+0x20) -> cmovae slot,rbx; 8 slots; then the
// unchecked use of the slot (mov rcx,[slot+8]; inc qword [slot+20h]). 3.1.0 keeps the slot in rdi, 3.1.4 in rsi.
constexpr char kFree[] = "48 8B 4B 18 48 8B 01 FF 50 40 48 3B 43 20 48 0F 43 ?? 49 FF C6 49 83 FE 08 0F 82 ?? ?? ?? ?? "
                         "48 8B ?? 08 48 FF ?? 20";
constexpr size_t kFreeCmov = 17, kFreeUse = 33, kFreeInc = 37;  // the ModRM bytes of the cmovae, the mov and the inc
constexpr size_t kSearch = 0x300;  // both lie within the function's first 0x300 bytes
constexpr uint32_t kSlotSize = 40, kSlots = 8, kAllocator = 8, kFence = 0x18, kValue = 0x20;
constexpr uint32_t kGetDesc = 0x90, kGetCompletedValue = 0x40;
constexpr uint32_t kMaxType = 7;     // D3D12_COMMAND_LIST_TYPE up to VIDEO_ENCODE
constexpr uint64_t kMaxWaitMs = 3000;
constexpr size_t kRegion = 64;

using AcquireFn = void* (*)(void* pool, void* queue, const void* name);
using GetDescFn = void* (*)(void* queue, void* desc);
using GetCompletedFn = uint64_t (*)(void* fence);

struct State {
    uint8_t* module = nullptr;
    uint8_t* acquire = nullptr;
    uint8_t* region = nullptr;
    int calls = 0;
};
State g;
AcquireFn g_original = nullptr;
std::atomic<bool> g_installed{false}, g_on{false}, g_watching{false};
std::atomic<uint64_t> s_waits{0}, s_waitUs{0}, s_maxWaitUs{0}, s_gaveUp{0};
std::atomic<int> s_logged{0};
void* g_cookie = nullptr;

template <class T> T At(const uint8_t* p, uint32_t off) { T v; memcpy(&v, p + off, sizeof v); return v; }

uint64_t NowUs()
{
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart); }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return QpcToMicroseconds(static_cast<uint64_t>(t.QuadPart), freq);
}

// What the function's walk will find: a slot not created yet (it creates one) or whose fence reached its value.
// Called with the pool's lock held.
bool AnySlotFree(const uint8_t* pool, uint32_t type)
{
    for (uint32_t i = 0; i < kSlots; ++i) {
        const uint8_t* slot = pool + (type * kSlots + 1 + i) * kSlotSize;
        if (!At<void*>(slot, kAllocator)) return true;
        void* const fence = At<void*>(slot, kFence);
        if (!fence) return true;  // as the function would find it; not this fix's case
        const auto completed = reinterpret_cast<GetCompletedFn>(At<void* const*>(static_cast<uint8_t*>(fence), 0)[kGetCompletedValue / 8]);
        if (completed(fence) >= At<uint64_t>(slot, kValue)) return true;
    }
    return false;
}


void* AcquireHook(void* poolPtr, void* queue, const void* name)
{
    if (!g_on.load(std::memory_order_relaxed)) return g_original(poolPtr, queue, name);
    auto* pool = static_cast<uint8_t*>(poolPtr);
    auto* lock = static_cast<CRITICAL_SECTION*>(poolPtr);
    uint32_t desc[4] = {};
    reinterpret_cast<GetDescFn>(At<void* const*>(static_cast<uint8_t*>(queue), 0)[kGetDesc / 8])(queue, desc);
    const uint32_t type = desc[0];
    if (type > kMaxType) return g_original(poolPtr, queue, name);
    uint64_t start = 0;
    bool gaveUp = false;
    for (;;) {
        EnterCriticalSection(lock);
        if (AnySlotFree(pool, type)) break;
        LeaveCriticalSection(lock);
        // all 8 still on the GPU: the function would read through a null slot
        const uint64_t now = NowUs();
        if (!start) start = now;
        if (now - start >= kMaxWaitMs * 1000) {
            s_gaveUp.fetch_add(1, std::memory_order_relaxed);
            gaveUp = true;
            Log("[fg] frame generation's command lists (queue type %u): none free after %llu ms, the library runs as before",
                type, static_cast<unsigned long long>(kMaxWaitMs));
            EnterCriticalSection(lock);
            break;
        }
        Sleep(1);
    }
    if (start && !gaveUp) {
        const uint64_t us = NowUs() - start;
        s_waits.fetch_add(1, std::memory_order_relaxed);
        s_waitUs.fetch_add(us, std::memory_order_relaxed);
        uint64_t m = s_maxWaitUs.load(std::memory_order_relaxed);
        while (us > m && !s_maxWaitUs.compare_exchange_weak(m, us)) {}
        if (s_logged.fetch_add(1, std::memory_order_relaxed) < 8)
            Log("[fg] frame generation's command lists (queue type %u): all 8 still on the GPU, waited %.1f ms for one "
                "(the game would have crashed here)", type, us / 1000.0);
    }
    void* const slot = g_original(poolPtr, queue, name);
    LeaveCriticalSection(lock);
    return slot;
}

bool Fail(const char* why)
{
    Log("[fg] frame generation crash guard not installed: %s", why);
    return false;
}

bool Install(HMODULE module)
{
    if (g_installed) return true;
    GameModule m;
    if (!ParseModule(m, module)) return Fail("unexpected library layout");
    const auto hits = FindAll(m.text, ParsePattern(kAcquire));
    if (hits.size() != 1) {
        char why[96];
        snprintf(why, sizeof why, "the command list pool function found %zu times (expected once; AMD's SDK 2.x DLLs, with "
                 "32 lists, do not need the guard)", hits.size());
        return Fail(why);
    }
    uint8_t* const fn = hits[0];
    const Section body{fn, fn + kSearch < m.text.end ? fn + kSearch : m.text.end};
    const auto free = FindAll(body, ParsePattern(kFree));
    if (FindAll(body, ParsePattern(kSlot)).size() != 1 || free.size() != 1)
        return Fail("the command list pool's slots are not laid out as expected");
    // the register the free test picks the slot into (cmovae reg,rbx: ModRM 11 reg 011) is the one the unchecked use
    // reads through (mov rcx,[reg+8]: 01 001 reg; inc qword [reg+20h]: 01 000 reg); rsp/rbp would need other encodings
    const uint8_t cmov = free[0][kFreeCmov], use = free[0][kFreeUse], inc = free[0][kFreeInc];
    const uint8_t reg = (cmov >> 3) & 7;
    if ((cmov & 0xC7) != 0xC3 || use != (0x48 | reg) || inc != (0x40 | reg) || reg == 4 || reg == 5)
        return Fail("the command list pool's free test does not match its use");
    // its direct calls
    std::vector<uint8_t*> calls;
    for (uint8_t* p = m.text.begin; p + 5 <= m.text.end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0xE8, m.text.end - 5 - p + 1));
        if (!p) break;
        if (RipTarget(p, 1, 5) == fn) calls.push_back(p);
    }
    if (calls.empty() || calls.size() > 32) return Fail("no calls of the command list pool function");
    uint8_t* const region = AllocNear(m.base, m.sizeOfImage, kRegion);
    if (!region) return Fail("no free memory near the library");
    // region: jmp [rip+0] -> AcquireHook
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&AcquireHook);
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, kRegion, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, kRegion);
    for (uint8_t* call : calls)
        if (region - (call + 5) != static_cast<int32_t>(region - (call + 5))) return Fail("the wrapper is out of reach");
    g_original = reinterpret_cast<AcquireFn>(fn);
    g.module = m.base;
    g.acquire = fn;
    g.region = region;
    int patched = 0;
    for (uint8_t* call : calls) {
        const int32_t rel32 = static_cast<int32_t>(region - (call + 5));
        if (WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4))) ++patched;
    }
    g.calls = patched;
    g_installed = patched > 0;
    Log("[fg] frame generation crash guard installed: amd_fidelityfx_dx12.dll's command list pool function +0x%X (unchecked "
        "read at +0x%X), %d of %zu calls through the guard",
        static_cast<unsigned>(fn - m.base), static_cast<unsigned>(free[0] + kFreeUse - 2 - m.base), patched, calls.size());
    return g_installed;
}

// LdrRegisterDllNotification (ntdll; documented): called under the loader lock once a DLL is mapped, before its code runs.
struct UnicodeStr { USHORT Length, MaximumLength; PWSTR Buffer; };
struct DllNotificationData { ULONG Flags; const UnicodeStr* FullDllName; const UnicodeStr* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
using DllNotificationFn = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, PVOID context);
using RegisterFn = LONG(NTAPI*)(ULONG flags, DllNotificationFn callback, PVOID context, PVOID* cookie);
constexpr ULONG kReasonLoaded = 1, kReasonUnloaded = 2;
constexpr wchar_t kLibrary[] = L"amd_fidelityfx_dx12.dll";

VOID CALLBACK OnDllLoaded(ULONG reason, const DllNotificationData* data, PVOID)
{
    if (!data || !data->BaseDllName) return;
    const UnicodeStr& n = *data->BaseDllName;
    if (n.Length != (std::size(kLibrary) - 1) * sizeof(wchar_t) || _wcsnicmp(n.Buffer, kLibrary, std::size(kLibrary) - 1) != 0) return;
    if (reason == kReasonUnloaded && g_installed && data->DllBase == g.module) {
        g_installed = false;  // its patched calls went with it; installed again if it is loaded again
        Log("[fg] amd_fidelityfx_dx12.dll unloaded");
    } else if (reason == kReasonLoaded && !g_installed && Install(static_cast<HMODULE>(data->DllBase))) {
        g_on = true;
    }
}

}  // namespace

bool WatchFrameGenLibrary(bool beforeGameCode)
{
    if (g_installed) return true;
    if (HMODULE m = GetModuleHandleW(kLibrary)) {
        // already loaded: only before the game's code runs is nothing using it yet
        if (!beforeGameCode) return Fail("the library was already loaded (install as version.dll)");
        if (Install(m)) g_on = true;
        return g_installed;
    }
    if (g_watching.exchange(true)) return false;
    const auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    if (!reg || reg(0, &OnDllLoaded, nullptr, &g_cookie) != 0) {
        g_watching = false;
        return Fail("no DLL load notification");
    }
    Log("[fg] frame generation crash guard: waiting for amd_fidelityfx_dx12.dll to load");
    return false;
}

void SetFrameGenCrashGuardEnabled(bool enabled)
{
    if (!g_installed || g_on.load() == enabled) return;
    g_on = enabled;
    Log("[fg] frame generation crash guard %s", enabled ? "on" : "off (the library's command list pool as before)");
}

void LogFrameGenCrashGuardStats()
{
    const uint64_t n = s_waits.exchange(0), us = s_waitUs.exchange(0), mx = s_maxWaitUs.exchange(0), gave = s_gaveUp.exchange(0);
    if (n || gave)
        Log("[fg] since last report: %llu waits for a free frame generation command list (%.1f ms in all, longest %.1f ms), "
            "%llu gave up", static_cast<unsigned long long>(n), us / 1000.0, mx / 1000.0, static_cast<unsigned long long>(gave));
}

bool FrameGenCrashGuardInfo(FrameGenGuardInfo* out)
{
    if (!g_installed) return false;
    out->module = g.module;
    out->acquire = g.acquire;
    out->calls = g.calls;
    out->waits = s_waits.load();
    out->gaveUp = s_gaveUp.load();
    return true;
}

void* FrameGenCrashGuardTestAcquire(void* pool, void* queue, const void* name) { return AcquireHook(pool, queue, name); }


}  // namespace opf
