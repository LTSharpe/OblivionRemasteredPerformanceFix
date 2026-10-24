// NaniteInitialRootPages (worker tlasfreeze 2026-10-22, public since 1.2.0, default 3072; fixes-explained ## 66) and the lab
// diagnostics NaniteStreamProbe / Nanite*MinMB (lab build only): Nanite's streaming buffers and their re-creations.
//
// Nanite::FStreamingManager::ProcessNewResources (UE 5.3 NaniteStreamingManager.cpp:1391, exe+0x35D2FA0 in build
// 19115871) sizes three GPU buffers from grow-only span allocators every time new Nanite resources come in:
//   Hierarchy        RoundUpToPowerOfTwo(nodes) * 208 bytes            ("Nanite.StreamingManager.Hierarchy")
//   ClusterPageData  MaxStreamingPages * 128 KiB + root pages * 32 KiB  ("Nanite.StreamingManager.ClusterPageData"; root
//                    pages = max(r.Nanite.Streaming.NumInitialRootPages, the allocator's size rounded to 2 significant bits))
//   ImposterData     max(r.Nanite.Streaming.NumInitialImposters, imposters rounded to 2 significant bits) * 41,472 bytes
// with ResizeByteAddressBufferIfNeeded (UnifiedBuffer.cpp:650, exe+0x270C9A0). When the size changes it creates a new
// buffer of the full new size beside the old one (ConvertToExternalBuffer allocates it at once, on the render thread),
// copies the old contents over on the GPU and lets the old one go. ClusterPageData holds the whole 512 MiB streaming
// pool, so each time the root pages pass a step the game makes a ~0.6 GB allocation in one frame. Near the video-memory
// budget that allocation waits on the paging fence: the long freeze past the Talos bridge on the Imperial City flight.
//
// NaniteInitialRootPages: the game's r.Nanite.Streaming.NumInitialRootPages (default 2048), the root pages ClusterPageData
// is first made with, set at start-up to this value (0 = the game's; 2049-8192, values up to 2048 leave the game's). It is
// the same as the line r.Nanite.Streaming.NumInitialRootPages=<n> under [SystemSettings] in Engine.ini (a read-only
// variable: only the ini or code may set it, before the renderer starts); a value the player sets there still wins (it is
// applied later). 3072 is exactly the game's first growth step (just past 2048, rounded up to 2 significant bits): the
// buffer starts at 608 MiB instead of 576 (+32 MiB from the first world) and that step is never taken; past 3072 root
// pages the game grows it as before (4096: +64 MiB from the first world). Identical: root page GPU offsets depend only on
// MaxStreamingPages (GPUPageIndexToGPUOffset), the bigger buffer only has unused space at its end. The variable is found
// through its registration (lea r8,[value]; lea rdx,[name]) and checked against ProcessNewResources' read of it
// (mov ebx,[value] between the Hierarchy resize and the ClusterPageData name), and must hold 2048.
// Routes: on the version.dll route (DllMain) the value is written before the static initialisers register the variable,
// so the console variable starts with it. The OBSE64 route runs our start-up from OBSE64's hook of the CRT's
// _get_narrow_winmain_command_line (obse64.cpp: "runs after global initializers"), so after the registration and before
// WinMain (no ini read, no renderer): the int ProcessNewResources reads is written the same way; only the console's own
// copy of the value (what the console prints for the variable) stays 2048 there. Any later Set of the variable (an
// Engine.ini line) overwrites both, on either route.
//
// The lab probe (NaniteStreamProbe=1) logs every size change of the three buffers with the time the call took on the
// render thread (that is the allocation), and counts the calls. The minimums (Nanite<Hierarchy|ClusterPages|Imposters>MinMB,
// MiB, 0 = the game's size) make a buffer at least that big, so the growth steps below it are not taken: the buffer is
// bigger than the game asked for, its contents are the same (the copy keeps every byte the game wrote; what lies past
// the game's size is never addressed: page and node offsets come from the allocators and MaxStreamingPages, not from
// the buffer's size). All installed only at start-up (any key != 0); the minimums and the probe then switch live.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// ResizeByteAddressBufferIfNeeded: mov rax,rsp; sub rsp,58h; shr r8d,2 (NumBytes / 4-byte elements)
constexpr char kResizeEntry[] = "48 8B C4 48 83 EC 58 41 C1 E8 02";
constexpr size_t kCallSearch = 0x60;  // the call after each name's lea r9 (0x4A, 0x1E, 0x56 bytes in build 19115871)

enum Kind { kHierarchy = 0, kClusterPages, kImposters, kKinds };
const wchar_t* const kNames[kKinds] = {L"Nanite.StreamingManager.Hierarchy", L"Nanite.StreamingManager.ClusterPageData",
                                       L"Nanite.StreamingManager.ImposterData"};
const char* const kShort[kKinds] = {"Hierarchy", "ClusterPageData", "ImposterData"};


struct Sites {
    uint8_t* call[kKinds] = {};
    uint8_t* resize = nullptr;
    const uint8_t* name[kKinds] = {};
};
Sites g;
int32_t* g_rootPagesVar = nullptr;  // GNaniteStreamingNumInitialRootPages


bool Fail(const char* why)
{
    Log("[nanite] %s: NaniteInitialRootPages not used (the game's 2048)", why);
    return false;
}

const uint8_t* FindWide(const Section& s, const wchar_t* text)
{
    const size_t n = (wcslen(text) + 1) * sizeof(wchar_t);
    const uint8_t* found = nullptr;
    for (const uint8_t* p = s.begin; p + n <= s.end; p += 2) {
        if (memcmp(p, text, n) != 0) continue;
        if (found) return nullptr;  // twice: not sure which
        found = p;
    }
    return found;
}

}  // namespace


bool FindNaniteStreamBuffers()
{
    if (g.resize) return true;
    const GameModule& game = Game();
    Sites s;
    const Pattern entry = ParsePattern(kResizeEntry);
    for (int k = 0; k < kKinds; ++k) {
        s.name[k] = FindWide(game.rdata, kNames[k]);
        if (!s.name[k]) return Fail("a Nanite streaming buffer's name not found once");
        // lea r9,[rip+name]: 4C 8D 0D disp32, exactly once in the code
        uint8_t* lea = nullptr;
        for (uint8_t* p = game.text.begin; p + 7 <= game.text.end; ++p) {
            if (p[0] != 0x4C || p[1] != 0x8D || p[2] != 0x0D || RipTarget(p, 3, 7) != s.name[k]) continue;
            if (lea) return Fail("a Nanite streaming buffer's name is used twice");
            lea = p;
        }
        if (!lea) return Fail("a Nanite streaming buffer's name is not used");
        uint8_t* call = nullptr;
        for (uint8_t* q = lea + 7; q < lea + 7 + kCallSearch; ++q)
            if (*q == 0xE8) { call = q; break; }
        if (!call) return Fail("no call after a Nanite streaming buffer's name");
        uint8_t* const target = RipTarget(call, 1, 5);
        if (!game.text.Contains(target) || !MatchesAt(target, entry)) return Fail("the call after the name is not the resize");
        if (s.resize && s.resize != target) return Fail("the three buffers use different resize functions");
        s.resize = target;
        s.call[k] = call;
    }
    if (!(s.call[kHierarchy] < s.call[kClusterPages] && s.call[kClusterPages] < s.call[kImposters] &&
          s.call[kImposters] - s.call[kHierarchy] < 0x200))
        return Fail("the three resizes are not together in one function");
    g = s;
    Log("[nanite] ProcessNewResources: resizes of Hierarchy exe+0x%X, ClusterPageData exe+0x%X, ImposterData exe+0x%X "
        "(ResizeByteAddressBufferIfNeeded exe+0x%X)",
        static_cast<unsigned>(g.call[0] - game.base), static_cast<unsigned>(g.call[1] - game.base),
        static_cast<unsigned>(g.call[2] - game.base), static_cast<unsigned>(g.resize - game.base));
    return true;
}


// r.Nanite.Streaming.NumInitialRootPages' value, found by its registration and checked against ProcessNewResources' read.
int32_t* FindNaniteRootPagesVar()
{
    if (g_rootPagesVar) return g_rootPagesVar;
    if (!g.resize && !FindNaniteStreamBuffers()) return nullptr;
    const GameModule& game = Game();
    const uint8_t* const name = FindWide(game.rdata, L"r.Nanite.Streaming.NumInitialRootPages");
    if (!name) { Fail("r.Nanite.Streaming.NumInitialRootPages' name not found once"); return nullptr; }
    int32_t* var = nullptr;
    int regs = 0;
    for (uint8_t* p = game.text.begin + 0x10; p + 7 <= game.text.end; ++p) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15 || RipTarget(p, 3, 7) != name) continue;  // lea rdx,[name]
        ++regs;
        for (uint8_t* q = p - 7; q >= p - 0x10; --q)
            if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0x05) { var = reinterpret_cast<int32_t*>(RipTarget(q, 3, 7)); break; }  // lea r8,[value]
    }
    if (regs != 1 || !var || !game.data.Contains(var)) { Fail("r.Nanite.Streaming.NumInitialRootPages' registration not as expected"); return nullptr; }
    // ProcessNewResources reads it between the Hierarchy resize and the ClusterPageData name: mov ebx,[rip+value] (8B 1D)
    bool read = false;
    for (uint8_t* p = g.call[kHierarchy] + 5; p + 6 <= g.call[kClusterPages] && !read; ++p)
        read = p[0] == 0x8B && p[1] == 0x1D && RipTarget(p, 2, 6) == reinterpret_cast<uint8_t*>(var);
    if (!read) { Fail("ProcessNewResources does not read r.Nanite.Streaming.NumInitialRootPages where expected"); return nullptr; }
    g_rootPagesVar = var;
    Log("[nanite] r.Nanite.Streaming.NumInitialRootPages at exe+0x%X (now %d)", static_cast<unsigned>(reinterpret_cast<uint8_t*>(var) - game.base), *var);
    return var;
}

// Start-up only, before the ini and the renderer (DllMain on the version.dll route, before WinMain on the OBSE64 route; see
// the top of the file): the value is the game's built-in 2048 then; anything else is left alone.
bool SetNaniteInitialRootPages(int pages, bool beforeGameCode)
{
    if (pages <= 2048) {
        if (pages > 0) Log("[nanite] NaniteInitialRootPages=%d is not above the game's 2048: left at the game's", pages);
        return false;
    }
    int32_t* const var = FindNaniteRootPagesVar();
    if (!var) return false;
    if (*var != 2048) {
        Log("[nanite] r.Nanite.Streaming.NumInitialRootPages holds %d, not the game's 2048: NaniteInitialRootPages left alone", *var);
        return false;
    }
    const int v = pages > 8192 ? 8192 : pages;
    *var = v;  // .data, writable; one aligned 4-byte store
    Log("[nanite] NaniteInitialRootPages: r.Nanite.Streaming.NumInitialRootPages 2048 -> %d%s (ClusterPageData starts %d MiB bigger, "
        "its growth steps below that are not taken; an Engine.ini value still wins)",
        v, beforeGameCode ? "" : " on the OBSE64 route, before the engine starts", (v - 2048) / 32);
    return true;
}


}  // namespace opf
