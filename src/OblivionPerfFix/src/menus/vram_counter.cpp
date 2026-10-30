// VramCounterFix: the Graphics page's "Show VRAM" counter shows 0/0 (game bug).
//
// The problem. With "Show VRAM" on, the game's debug-info widget (AltarGraphicsSettingsDebugInfo, its VRAMText line
// "VRAM: {0}/{1}") asks one small function for two numbers every few frames, and the Blueprint function GetUsedVRAMMB
// asks the same one. That function (exe+0x47B6AF0 in build 19115871) takes GDynamicRHI's first adapter and reads two
// 64-bit fields at +0x760 and +0x768, scaled to MB (x 2^-20). Their order and use match the engine's cached DXGI video
// memory info on the D3D12 adapter: the first is the budget Windows gives the process (shown as {1}, the total), the
// second its current use ({0}). The shipping build never refreshes that cache, so both stay 0 and the counter reads 0/0
// on every PC (Steam reports on AMD and NVIDIA; Bethesda's support called it a known bug). This PC (RTX 5090): 0/0.
//
// The fix. The query's two direct calls (the widget's update, exe+0x48C2511, and GetUsedVRAMMB's exec thunk,
// exe+0x46019EB) go through a small wrapper. It calls the game's function first; only when both numbers come back 0 it
// fills them with the same two values the engine would have cached, from the monitor thread's once-a-second DXGI sample
// (QueryVideoMemoryInfo, local segment, on the GPU the game uses: memory_monitor.cpp): budget and current use in MB. If a
// game update starts filling the fields, the wrapper passes the game's numbers through unchanged.
//
// What changes. Only the two numbers of the counter you switch on in the Graphics page (and what GetUsedVRAMMB returns,
// which nothing else in the game calls). Nothing is drawn differently otherwise; the wrapper reads two atomics.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

using QueryFn = void (*)(int32_t* budgetMB, int32_t* usedMB);

// The query: mov [rsp+8],rbx; push rdi; sub rsp,20h; xor eax,eax; mov rdi,rcx; mov [rcx],eax; mov rbx,rdx; mov [rdx],eax;
// mov rcx,[GDynamicRHI]; mov rax,[rcx]; call [rax+30h]; xorps xmm0,xmm0; mov rax,[rax+0C8h]; mov r8,[rax];
// mov rcx,[r8+760h] ... mov rcx,[r8+768h]
constexpr const char* kQuery =
    "48 89 5C 24 08 57 48 83 EC 20 33 C0 48 8B F9 89 01 48 8B DA 89 02 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 30 0F 57 C0 "
    "48 8B 80 C8 00 00 00 4C 8B 00 49 8B 88 60 07 00 00";
constexpr const char* kSecondField = "49 8B 88 68 07 00 00";
constexpr size_t kSecondFieldWithin = 0x60;
// The widget's call: lea rdx,[rsp+24h]; mov [rsp+24h],r14d; lea rcx,[rsp+20h]; mov [rsp+20h],r14d; call query;
// movsxd rdi,[rsp+20h]
constexpr const char* kWidgetCall =
    "48 8D 54 24 24 44 89 74 24 24 48 8D 4C 24 20 44 89 74 24 20 E8 ?? ?? ?? ?? 48 63 7C 24 20";
constexpr size_t kWidgetCallOffset = 20;
// GetUsedVRAMMB's exec thunk, after reading its two int references: test rax,rax; setnz dil; add rdi,rax;
// mov [rbx+20h],rdi; call query; mov rbx,[rsp+30h]; mov rsi,[rsp+40h]; add rsp,20h; pop rdi; ret
constexpr const char* kThunkCall =
    "48 85 C0 40 0F 95 C7 48 03 F8 48 89 7B 20 E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B 74 24 40 48 83 C4 20 5F C3";
constexpr size_t kThunkCallOffset = 14;

uint8_t* g_query = nullptr;
uint8_t* g_calls[2] = {};
uint8_t* g_stub = nullptr;
QueryFn g_original = nullptr;
bool (*g_latest)(uint64_t*, uint64_t*) = &LatestGameVram;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_filled{0}, g_gameValues{0}, g_unknown{0};
uint64_t g_filledLogged = 0, g_gameLogged = 0;
bool g_firstLogged = false;

void QueryWrapper(int32_t* budgetMB, int32_t* usedMB)
{
    g_original(budgetMB, usedMB);
    if (!g_on.load(std::memory_order_relaxed) || !budgetMB || !usedMB) return;
    if (*budgetMB != 0 || *usedMB != 0) {
        g_gameValues.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint64_t used = 0, budget = 0;
    if (!g_latest(&used, &budget)) {
        g_unknown.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    *budgetMB = static_cast<int32_t>(budget >> 20);
    *usedMB = static_cast<int32_t>(used >> 20);
    g_filled.fetch_add(1, std::memory_order_relaxed);
    if (!g_firstLogged) {
        g_firstLogged = true;
        Log("[vram] the VRAM counter's query returned 0/0; filled from DXGI: %d / %d MB (used / budget)", *usedMB, *budgetMB);
    }
}

// One call's rel32 aimed at the stub: atomically within one qword, or (before the game's code runs) with WriteCode.
bool Redirect(uint8_t* call, uint8_t* stub, bool beforeGameCode)
{
    const int64_t rel = stub - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(call) + 1, qword = lo & ~uintptr_t{7};
    if (lo + 3 < qword + 8) {
        DWORD old;
        auto* q = reinterpret_cast<volatile LONG64*>(qword);
        if (!VirtualProtect(reinterpret_cast<void*>(qword), 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        const LONG64 before = *q;
        LONG64 after = before;
        memcpy(reinterpret_cast<uint8_t*>(&after) + (lo - qword), &rel32, 4);
        const bool ok = InterlockedCompareExchange64(q, after, before) == before;
        VirtualProtect(reinterpret_cast<void*>(qword), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(qword), 8);
        return ok;
    }
    if (!beforeGameCode) return false;
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}

// The one hit of a call-site pattern whose call goes to the query (nullptr if none or several).
uint8_t* CallTo(const GameModule& game, const char* pattern, size_t offset, const uint8_t* query, size_t* count)
{
    uint8_t* found = nullptr;
    *count = 0;
    for (uint8_t* p : FindAll(game.text, ParsePattern(pattern))) {
        uint8_t* const call = p + offset;
        if (call[0] != 0xE8 || RipTarget(call, 1, 5) != query) continue;
        found = call;
        ++*count;
    }
    return *count == 1 ? found : nullptr;
}

}  // namespace

bool ApplyVramCounterFix(bool beforeGameCode)
{
    if (g_query) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kQuery));
    if (hits.size() != 1) {
        Log("[vram] the VRAM counter's query not found (%zu matches): VramCounterFix not installed", hits.size());
        return false;
    }
    uint8_t* const query = hits[0];
    bool second = false;
    const auto field = ParsePattern(kSecondField);
    for (size_t i = 0; i < kSecondFieldWithin && !second; ++i) second = MatchesAt(query + 50 + i, field);
    if (!second) {
        Log("[vram] the query at exe+0x%llX does not read the second field (+768h): VramCounterFix not installed",
            static_cast<unsigned long long>(query - game.base));
        return false;
    }
    size_t widgetHits = 0, thunkHits = 0;
    uint8_t* const widget = CallTo(game, kWidgetCall, kWidgetCallOffset, query, &widgetHits);
    uint8_t* const thunk = CallTo(game, kThunkCall, kThunkCallOffset, query, &thunkHits);
    if (!widget || !thunk) {
        Log("[vram] the query's calls not found (widget %zu, GetUsedVRAMMB %zu; expected 1 each): VramCounterFix not installed",
            widgetHits, thunkHits);
        return false;
    }
    constexpr size_t kRegion = 0x1000;
    uint8_t* const region = AllocNearGame(kRegion);
    if (!region) {
        Log("[vram] no free memory near the game's code for the call stub: VramCounterFix not installed");
        return false;
    }
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&QueryWrapper);
    memcpy(region, jmp, sizeof jmp);
    memcpy(region + sizeof jmp, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, kRegion, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 14);
    g_original = reinterpret_cast<QueryFn>(query);
    if (!Redirect(widget, region, beforeGameCode)) {
        VirtualFree(region, 0, MEM_RELEASE);
        g_original = nullptr;
        Log("[vram] writing the widget's call at exe+0x%llX failed: VramCounterFix not installed",
            static_cast<unsigned long long>(widget - game.base));
        return false;
    }
    if (!Redirect(thunk, region, beforeGameCode)) {
        // The widget's call already goes to the wrapper, which is correct on its own; leave the stub in place.
        Log("[vram] writing GetUsedVRAMMB's call at exe+0x%llX failed: only the counter itself goes through VramCounterFix",
            static_cast<unsigned long long>(thunk - game.base));
    } else {
        g_calls[1] = thunk;
    }
    g_calls[0] = widget;
    g_query = query;
    g_stub = region;
    Log("[vram] the VRAM counter's query (exe+0x%llX) called at exe+0x%llX and exe+0x%llX through VramCounterFix",
        static_cast<unsigned long long>(query - game.base), static_cast<unsigned long long>(widget - game.base),
        static_cast<unsigned long long>(thunk - game.base));
    return true;
}

void SetVramCounterFix(bool on)
{
    if (!g_query) return;
    if (g_on.exchange(on) != on) Log("[vram] VramCounterFix %s", on ? "on" : "off");
}

void LogVramCounterFixStats()
{
    const uint64_t f = g_filled.load(), g = g_gameValues.load();
    if (!g_query || (f == g_filledLogged && g == g_gameLogged)) return;
    g_filledLogged = f;
    g_gameLogged = g;
    Log("[vram] VRAM counter: %llu readings filled from DXGI, %llu with the game's own values, %llu before the first sample",
        static_cast<unsigned long long>(f), static_cast<unsigned long long>(g),
        static_cast<unsigned long long>(g_unknown.load()));
}

bool VramCounterFixInfo(VramCounterInfo* out)
{
    if (!g_query) return false;
    out->query = g_query;
    out->calls[0] = g_calls[0];
    out->calls[1] = g_calls[1];
    out->stub = g_stub;
    out->hook = reinterpret_cast<void*>(&QueryWrapper);
    out->filled = g_filled.load();
    out->gameValues = g_gameValues.load();
    out->unknown = g_unknown.load();
    return true;
}

void VramCounterFixTestSetup(void* original, bool (*latest)(uint64_t*, uint64_t*))
{
    g_original = original ? reinterpret_cast<QueryFn>(original) : reinterpret_cast<QueryFn>(g_query);
    g_latest = latest ? latest : &LatestGameVram;
}

}  // namespace opf
