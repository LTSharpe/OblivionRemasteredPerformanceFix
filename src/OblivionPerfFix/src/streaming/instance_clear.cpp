// InstanceClearReserve: clearing a big instanced mesh (grass, foliage) no longer re-copies its command
// list dozens of times on the game thread.
//
// UHierarchicalInstancedStaticMeshComponent::ClearInstances (UE 5.3, HierarchicalInstancedStaticMesh.cpp) empties the
// component's instance update command buffer (FInstanceUpdateCmdBuffer::Reset, which frees the list) and then queues one
// "hide" command per rendered instance (FInstanceUpdateCmdBuffer::HideInstance, one Cmds.AddDefaulted_GetRef each, 336
// bytes per command). The list grows by the engine's default policy (about 3/8 more each time), so for a component with
// tens of thousands of instances it is reallocated and copied some 30 times in that one call: on the Echo Mine -> Brotch
// Camp walk a 56-64 ms frame under a 60 fps cap (about 22 ms of it copying inside FMallocBinned2::Realloc) at the same
// spot in both runs (2026-10-13).
//
// The fix redirects ClearInstances' call to Reset: the game's Reset runs as before, then the list's capacity is set to
// the number of commands the loop that follows can add (the loop's own bound: the smaller of the component's instance
// count and its render data's instance count), with the engine's own allocation function. The loop then appends to a
// list that is already big enough. The commands, their order and the list's contents are the same; only its spare
// capacity differs (the exact count instead of the growth policy's up to 37% extra).
//
// Found by shape (build 19115871 addresses in the comments): the call to Reset and the loop around HideInstance in
// ClearInstances, HideInstance's grow call and element size, and the grow function's call to the allocation helper.
// Any mismatch: the fix stays off with a log line. The call is redirected only before the game's code runs (four bytes
// that are not one aligned write).
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// ClearInstances (exe+0x31FA640) at exe+0x31FA7C7: lea rcx,[rdi+CmdBuffer]; call Reset; mov rax,[rdi+RenderData];
// test rax,rax; je; mov rax,[rax+InstanceBuffer]; mov ecx,[rax+NumInstances]; test ecx,ecx; jle; mov eax,[rdi+Instances];
// mov esi,ebp; cmp eax,ecx; mov [rsp+50h],r15; cmovl ecx,eax; ... the loop: cmp esi,[rdi+ReorderNum]; ...;
// lea rcx,[rdi+CmdBuffer]; call HideInstance
constexpr char kClearSite[] =
    "48 8D 8F ?? ?? 00 00 E8 ?? ?? ?? ?? 48 8B 87 ?? ?? 00 00 48 85 C0 74 ?? 48 8B 80 ?? ?? 00 00 8B 48 ?? 85 C9 7E ?? "
    "8B 87 ?? ?? 00 00 8B F5 3B C1 4C 89 7C 24 50 0F 4C C8 8B C5 85 C9 0F 4F C1 4C 63 F8 85 C0 7E ?? 48 8B DD "
    "3B B7 ?? ?? 00 00 7D ?? 48 8B 87 ?? ?? 00 00 8B 14 98 83 FA FF 74 ?? EB ?? 8B D6 48 8D 8F ?? ?? 00 00 E8";
constexpr size_t kSiteCmdOff = 3, kSiteResetCall = 7, kSiteRenderOff = 0x0F, kSiteBufferOff = 0x1B, kSiteCountOff = 0x21,
                 kSiteNumOff = 0x28, kSiteCmdOff2 = 0x67, kSiteHideCall = 0x6B;
// HideInstance (exe+0x3254850): movsxd rdi,[rcx+8]; ...; inc Num; cmp eax,[rcx+0Ch]; jbe; mov edx,edi; call Grow;
// ...; imul rcx,rdi,ElementSize
constexpr char kHide[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 63 79 08 8B F2 48 8B D9 8D 47 01 89 41 08 3B 41 0C 76 ?? 8B D7 E8 ?? ?? ?? ?? "
    "33 C0 48 69 CF ?? ?? 00 00";
constexpr size_t kHideGrowCall = 0x25, kHideElemSize = 0x2F;
// Grow (exe+0x2298580): ... imul rcx,rax,ElementSize; mov edx,Alignment; call QuantizeSize; ...; mov [rdi+0Ch],ecx;
// mov r8d,ecx; mov rcx,rdi; call ResizeAllocation(this, PreviousNum = edx, NewMax = r8d, ElementSize = r9d, Alignment)
constexpr char kGrowElem[] = "48 69 C8 ?? ?? 00 00 BA ?? 00 00 00 E8";
constexpr char kGrowResize[] = "89 4F 0C 44 8B C1 48 8B CF E8";
constexpr size_t kGrowSearch = 0xC0;
constexpr int kMinCommands = 256;  // smaller clears are left to the growth policy

using ResetFn = void (*)(uint8_t* cmdBuffer);
using ResizeFn = void (*)(uint8_t* array, int32_t previousNum, int32_t newMax, size_t elementSize, uint32_t alignment);

struct Sites {
    uint8_t* site = nullptr;   // the lea before the call to Reset
    uint8_t* reset = nullptr;
    uint8_t* resize = nullptr;
    uint32_t cmdOff = 0, renderOff = 0, bufferOff = 0, numOff = 0;
    uint8_t countOff = 0;
    uint32_t elemSize = 0, alignment = 0;
};
Sites g;
ResetFn g_reset = nullptr;    // the game's Reset (or a fake in the self-test)
ResizeFn g_resize = nullptr;  // the engine's allocation helper (or a fake)
bool g_installed = false;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> s_reserved{0}, s_commands{0}, s_maxCommands{0};

uint32_t Disp32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

// The loop's bound in ClearInstances: min(render data's instance count, PerInstanceSMData.Num), 0 without render data.
int32_t CommandsToCome(const uint8_t* component)
{
    __try {
        const uint8_t* render = *reinterpret_cast<const uint8_t* const*>(component + g.renderOff);
        if (!render) return 0;
        const uint8_t* buffer = *reinterpret_cast<const uint8_t* const*>(render + g.bufferOff);
        if (!buffer) return 0;
        const int32_t rendered = *reinterpret_cast<const int32_t*>(buffer + g.countOff);
        const int32_t instances = *reinterpret_cast<const int32_t*>(component + g.numOff);
        return rendered < instances ? rendered : instances;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// In place of ClearInstances' call to FInstanceUpdateCmdBuffer::Reset (game thread).
void ResetHook(uint8_t* cmdBuffer)
{
    g_reset(cmdBuffer);
    if (!g_on.load(std::memory_order_relaxed)) return;
    const int32_t count = CommandsToCome(cmdBuffer - g.cmdOff);
    if (count < kMinCommands) return;
    // TArray<FInstanceUpdateCommand> Cmds at the buffer's start: data, Num (+8), Max (+0xC); Reset left Num at 0.
    const int32_t num = *reinterpret_cast<const int32_t*>(cmdBuffer + 8);
    int32_t& max = *reinterpret_cast<int32_t*>(cmdBuffer + 0xC);
    if (num != 0 || max >= count) return;
    max = count;
    g_resize(cmdBuffer, num, count, g.elemSize, g.alignment);
    s_reserved.fetch_add(1, std::memory_order_relaxed);
    s_commands.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
    uint64_t cur = s_maxCommands.load(std::memory_order_relaxed);
    while (static_cast<uint64_t>(count) > cur && !s_maxCommands.compare_exchange_weak(cur, static_cast<uint64_t>(count))) {
    }
}

bool Fail(const char* why)
{
    Log("[instances] %s: InstanceClearReserve unavailable", why);
    return false;
}

}  // namespace

bool FindInstanceClearReserve()
{
    if (g.site) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kClearSite));
    if (hits.size() != 1) {
        Log("[instances] ClearInstances' command reset found %zu times (expected 1): InstanceClearReserve unavailable", hits.size());
        return false;
    }
    uint8_t* const site = hits[0];
    Sites s;
    s.site = site;
    s.cmdOff = Disp32(site + kSiteCmdOff);
    s.renderOff = Disp32(site + kSiteRenderOff);
    s.bufferOff = Disp32(site + kSiteBufferOff);
    s.countOff = site[kSiteCountOff];
    s.numOff = Disp32(site + kSiteNumOff);
    s.reset = RipTarget(site + kSiteResetCall, 1, 5);
    uint8_t* const hide = RipTarget(site + kSiteHideCall, 1, 5);
    if (Disp32(site + kSiteCmdOff2) != s.cmdOff) return Fail("the command buffer offsets disagree");
    if (!game.text.Contains(s.reset) || !game.text.Contains(hide) || !MatchesAt(hide, ParsePattern(kHide)))
        return Fail("HideInstance not recognised");
    s.elemSize = Disp32(hide + kHideElemSize);
    uint8_t* const grow = RipTarget(hide + kHideGrowCall, 1, 5);
    if (!game.text.Contains(grow)) return Fail("the list's grow function not found");
    const Pattern elem = ParsePattern(kGrowElem), resize = ParsePattern(kGrowResize);
    uint8_t *elemAt = nullptr, *resizeAt = nullptr;
    for (uint8_t* p = grow; p < grow + kGrowSearch; ++p) {
        if (!elemAt && MatchesAt(p, elem)) elemAt = p;
        if (!resizeAt && MatchesAt(p, resize)) resizeAt = p;
    }
    if (!elemAt || !resizeAt || Disp32(elemAt + 3) != s.elemSize) return Fail("the grow function not recognised");
    s.alignment = elemAt[8];
    s.resize = RipTarget(resizeAt + 9, 1, 5);
    if (!game.text.Contains(s.resize) || s.elemSize == 0 || s.elemSize > 0x1000 || (s.alignment & (s.alignment - 1)))
        return Fail("unexpected allocation helper or element");
    g = s;
    Log("[instances] ClearInstances' reset exe+0x%X (command buffer +0x%X, %u-byte commands, align %u), Reset exe+0x%X, "
        "allocation exe+0x%X",
        static_cast<unsigned>(site - game.base), g.cmdOff, g.elemSize, g.alignment, static_cast<unsigned>(g.reset - game.base),
        static_cast<unsigned>(g.resize - game.base));
    return true;
}

bool InstallInstanceClearReserve(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!g.site && !FindInstanceClearReserve()) return false;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    uint8_t* const region = AllocNearGame(64);
    if (!region) return Fail("no free memory near the game's code");
    // region: jmp [rip+0] -> ResetHook
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&ResetHook);
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    uint8_t* const call = g.site + kSiteResetCall;
    const int64_t rel = region - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("the hook is out of reach");
    g_reset = reinterpret_cast<ResetFn>(g.reset);
    g_resize = reinterpret_cast<ResizeFn>(g.resize);
    const int32_t rel32 = static_cast<int32_t>(rel);
    if (!WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4))) return Fail("could not patch the call");
    g_installed = true;
    Log("[instances] installed (ClearInstances' Reset call redirected)");
    return true;
}

void SetInstanceClearReserve(bool enabled)
{
    if (!g_installed || g_on.load() == enabled) return;
    g_on = enabled;
    Log("[instances] %s", enabled ? "clearing a big instanced mesh reserves its hide commands at once"
                                  : "clearing a big instanced mesh grows its command list as before");
}

void LogInstanceClearStats()
{
    const uint64_t n = s_reserved.exchange(0), cmds = s_commands.exchange(0), mx = s_maxCommands.exchange(0);
    if (n) Log("[instances] since last report: %llu clears reserved (%llu commands, largest %llu)", n, cmds, mx);
}

bool InstanceClearInfo(InstanceClearInfoOut* out)
{
    if (!g.site) return false;
    out->site = g.site;
    out->reset = g.reset;
    out->resize = g.resize;
    out->cmdOff = g.cmdOff;
    out->renderOff = g.renderOff;
    out->bufferOff = g.bufferOff;
    out->countOff = g.countOff;
    out->numOff = g.numOff;
    out->elemSize = g.elemSize;
    out->alignment = g.alignment;
    return true;
}

void InstanceClearTestSetup(void* fakeReset, void* fakeResize)
{
    g_reset = reinterpret_cast<ResetFn>(fakeReset);
    g_resize = reinterpret_cast<ResizeFn>(fakeResize);
    g_installed = true;
}

void InstanceClearTestCall(uint8_t* cmdBuffer) { ResetHook(cmdBuffer); }

}  // namespace opf
