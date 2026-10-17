// Guard for RdgParallelTranslate (parallel translate): the D3D12 RHI's buffer-pool defragmentation is paused while
// RDG pass sets are translated on worker threads.
//
// What defragmentation does (UE 5.3 D3D12Allocation.cpp, FD3D12DefaultBufferAllocator::BeginFrame, run by the RHI
// thread at the start of every frame, d3d12.VRAMBufferPoolDefrag = 1 by default): it moves up to 32 MB of live GPU
// buffers per frame into fuller memory pools. For each moved buffer it creates a new placed D3D12 resource, copies
// the data on the GPU, frees the old resource one frame later, and tells every view of the buffer about the new
// resource (FD3D12ResourceLocation::OnAllocationMoved -> ResourceRenamed). Pure memory housekeeping: the moved
// buffer holds the same data.
//
// Why it must not run together with parallel translate:
//  1. The crash of 2026-10-07 (access violation reading 0x8 in FD3D12ContextCommon::UpdateResidency, called
//     from TransitionResource(UAV) <- FD3D12DescriptorCache::SetUAVs <- RHIDispatchComputeShader on a translate
//     worker): a buffer UAV whose D3D12 resource had already been freed. A view takes its resource pointer when it
//     is created and only then registers for move notifications (FD3D12View::CreateView), so a view created while
//     the RHI thread moves that buffer keeps the old resource for good, and the next use after the old resource is
//     freed reads freed memory. Epic tracked this race as UE-217692 ("the D3D12 resource allocator defragmentation
//     operation races with view creation ... reproduces very rarely with a crash in residency"), fixed in 5.5 by
//     creating views on the RHI thread. It is an engine race, also possible without our fix; it shows up after
//     loading a new area, when many buffers are freed (fragmentation, so defragmentation runs) and many new views
//     are created.
//  2. A hazard that parallel translate itself adds: a worker can translate the next frame's pass sets before the
//     RHI thread has run that frame's defragmentation. A compute pass that writes a buffer then records the old
//     resource, and the GPU runs it after the defragmentation copy, so its writes would land in the old copy and be
//     lost. In stock 5.3 this cannot happen to compute passes: they are replayed on the RHI thread, after it.
// With defragmentation paused neither can happen: buffers never move. The picture is unchanged (moving a buffer
// never changes its contents); the cost is that partly used buffer pools are not compacted (empty pools are still
// released), so video memory can be somewhat higher after much streaming.
//
// Found by shape: the console variable's registration (lea r8,[global]; lea rdx,[L"d3d12.VRAMBufferPoolDefrag"])
// and the start-of-frame code that reads it together with d3d12.VRAMBufferPoolDefrag.MaxCopySizePerFrame
// (cmp dword [global],0; jle; mov esi,[maxCopy]; test esi,esi; jle). Both must be found exactly once and agree,
// otherwise the guard is unavailable and parallel translate stays off.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

constexpr char kRegisterPattern[] = "4C 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ??";  // lea r8,[global]; lea rdx,[name]
constexpr char kBeginFramePattern[] = "83 3D ?? ?? ?? ?? 00 7E ?? 8B 35 ?? ?? ?? ?? 85 F6 7E";

volatile LONG* g_defrag = nullptr;   // GD3D12VRAMBufferPoolDefrag (int)
volatile LONG* g_maxCopy = nullptr;  // GD3D12VRAMBufferPoolDefragMaxCopySizePerFrame (int, bytes)
LONG g_defragOriginal = 0;
LONG g_maxCopyOriginal = 0;
std::atomic<bool> g_paused{false};
std::atomic<LONGLONG> g_pausedAtMs{0};

const uint8_t* FindWideString(const Section& s, const wchar_t* text)
{
    const size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);  // with the terminating zero
    const uint8_t* hit = nullptr;
    for (const uint8_t* p = s.begin; p + bytes <= s.end; p += 2) {
        if (memcmp(p, text, bytes) != 0) continue;
        if (hit) return nullptr;  // ambiguous
        hit = p;
    }
    return hit;
}

// The global registered under the given console variable name (lea r8 right before lea rdx,[name]).
uint8_t* RegisteredGlobal(const std::vector<uint8_t*>& registrations, const uint8_t* name)
{
    uint8_t* global = nullptr;
    for (uint8_t* at : registrations) {
        if (RipTarget(at + 7, 3, 7) != name) continue;
        if (global) return nullptr;  // registered twice: not the expected layout
        global = RipTarget(at, 3, 7);
    }
    return global;
}

bool InImageData(const GameModule& game, const uint8_t* p)
{
    return p >= game.base && p + 4 <= game.base + game.sizeOfImage && !game.text.Contains(p) && !game.rdata.Contains(p) &&
           (reinterpret_cast<uintptr_t>(p) & 3) == 0;
}

LONGLONG NowMs() { return static_cast<LONGLONG>(GetTickCount64()); }

}  // namespace

bool FindPoolDefragSwitch()
{
    const GameModule& game = Game();
    const uint8_t* defragName = FindWideString(game.rdata, L"d3d12.VRAMBufferPoolDefrag");
    const uint8_t* maxCopyName = FindWideString(game.rdata, L"d3d12.VRAMBufferPoolDefrag.MaxCopySizePerFrame");
    if (!defragName || !maxCopyName) {
        Log("[rdg] defrag guard: console variable names not found: guard unavailable");
        return false;
    }
    const auto registrations = FindAll(game.text, ParsePattern(kRegisterPattern));
    uint8_t* defrag = RegisteredGlobal(registrations, defragName);
    uint8_t* maxCopy = RegisteredGlobal(registrations, maxCopyName);
    if (!defrag || !maxCopy || !InImageData(game, defrag) || !InImageData(game, maxCopy)) {
        Log("[rdg] defrag guard: console variable registrations not found: guard unavailable");
        return false;
    }
    int reads = 0;
    for (uint8_t* at : FindAll(game.text, ParsePattern(kBeginFramePattern)))
        if (RipTarget(at, 2, 7) == defrag && RipTarget(at + 9, 2, 6) == maxCopy) ++reads;
    if (reads != 1) {
        Log("[rdg] defrag guard: start-of-frame defragmentation found %d times (expected 1): guard unavailable", reads);
        return false;
    }
    g_defrag = reinterpret_cast<volatile LONG*>(defrag);
    g_maxCopy = reinterpret_cast<volatile LONG*>(maxCopy);
    g_defragOriginal = *g_defrag;
    g_maxCopyOriginal = *g_maxCopy;
    Log("[rdg] defrag guard: d3d12.VRAMBufferPoolDefrag at exe+0x%X (%ld), MaxCopySizePerFrame at exe+0x%X (%ld)",
        static_cast<unsigned>(defrag - game.base), g_defragOriginal, static_cast<unsigned>(maxCopy - game.base),
        g_maxCopyOriginal);
    return true;
}

void SetPoolDefragPaused(bool paused)
{
    if (!g_defrag) return;
    if (g_paused.load() == paused) {
        // The engine sets the variable from Engine.ini ([SystemSettings]) after we paused it at start-up: keep that
        // value as the game's setting and pause again.
        const LONG now = *g_defrag;
        if (paused && now != 0) {
            g_defragOriginal = now;
            InterlockedExchange(g_defrag, 0);
            Log("[rdg] D3D12 buffer pool defragmentation was set to %ld by the game's settings: paused again", now);
        }
        return;
    }
    if (paused) {
        g_defragOriginal = InterlockedExchange(g_defrag, 0);  // the game's setting now (it may have changed since)
        g_pausedAtMs = NowMs();
    } else {
        InterlockedExchange(g_defrag, g_defragOriginal);
    }
    g_paused = paused;
    Log("[rdg] D3D12 buffer pool defragmentation %s", paused ? "paused (parallel translate on)" : "back to the game's setting");
}

bool PoolDefragPausedFor(int ms)
{
    return g_defrag && g_paused.load() && NowMs() - g_pausedAtMs.load() >= ms;
}

bool PoolDefragSwitchFound() { return g_defrag != nullptr; }


void PoolDefragTestAddresses(uint8_t** defrag, uint8_t** maxCopy)
{
    *defrag = g_defrag ? reinterpret_cast<uint8_t*>(const_cast<LONG*>(g_defrag)) : nullptr;
    *maxCopy = g_maxCopy ? reinterpret_cast<uint8_t*>(const_cast<LONG*>(g_maxCopy)) : nullptr;
}

}  // namespace opf
