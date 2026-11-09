// GpuPassMarkers ([General], diagnostics; worker hangcapture 2026-11-03): pass names for the GPU crash report and for AMD's
// Radeon GPU Detective (RGD), so the next GPU hang says which render pass the GPU was in.
//
// Why: on a GPU hang the game's D3D12 runtime keeps DRED breadcrumbs (Lightweight DRED, on in UE 5.3 shipping: markers-only
// auto breadcrumbs with breadcrumb contexts), and our [gpucrash] report prints them. But in this shipping build nothing ever
// calls ID3D12GraphicsCommandList::BeginEvent: UE's FD3D12CommandContext::RHIPushEvent (D3D12CommandContext.cpp:119) sends
// its event names to the GPU profiler, to UE's own breadcrumbs (r.D3D12.BreadCrumbs, 0 in shipping), to AMD's AGS markers
// (only while D3D12.EmitRgpFrameMarkers != 0) and to PIX (compiled out). So every command list in DRED has one operation
// (BEGIN_COMMAND_LIST) and a hang only says "the 3D queue stopped in or after one list, ~240 queued behind it" (worker
// gpuhang, 7 TDRs). The engine already makes the events (Lightweight DRED calls SetEmitDrawEvents(true): RDG pass names and
// the renderer's scopes), they just go nowhere.
//
// D3D12.EmitRgpFrameMarkers is read-only and normally set before the RHI starts, but setting it there is NOT harmless:
// FD3D12Device's set-up (D3D12Device.cpp:307) then counts the game as "under GPU capture" and EnableIdealGPUCaptureOptions
// turns the RHI thread off and r.rhicmdbypass on (a different, slower renderer). After the device exists only RHIPushEvent
// and RHIPopEvent read the variable, so this key sets it only once the game's device exists (the monitor thread sees the
// game's GPU in use), never before.
//
// With the variable set, RHIPushEvent calls agsDriverExtensionsDX12_PushMarker(ags, commandList, ansiName) and RHIPopEvent
// agsDriverExtensionsDX12_PopMarker(ags, commandList) (AGS is linked into the exe; the calls are made only when the AGS
// context exists, i.e. on AMD GPUs). Both calls are redirected to PushHook / PopHook here:
//   bit 1 (DRED): ID3D12GraphicsCommandList::BeginEvent / EndEvent with the name as a PIX3 blob (metadata 2, the encoding
//          WinPixEventRuntime's PIXBeginEvent writes for a command list): DRED records each as a breadcrumb with the name as
//          its context, so [gpucrash] can print the pass where the GPU stopped and the passes queued after it.
//   bit 2 (AMD): the original AGS call, the markers RGD and RGP show (RGD's manual: D3D12 marker strings come from AGS).
// Nothing is drawn differently: markers only label command-list ranges. The cost (CPU on the threads that record command
// lists, a little GPU for DRED's breadcrumb writes) is measured in findings "hangcapture".
// Only on AMD GPUs (no AGS context elsewhere: the engine never reaches the calls); logged. A player's own
// D3D12.EmitRgpFrameMarkers=1 in Engine.ini is left as it is (and its AGS markers are always passed on).
// Live: the key is re-read every second; installed at start-up when it is not 0 (both routes: only two call sites change).
#include "common.h"

#include <d3d12.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace opf {
namespace {

constexpr wchar_t kCvarName[] = L"D3D12.EmitRgpFrameMarkers";
// cmp dword ptr [rip+GEmitRgpFrameMarkers], 0; mov rax, [rip+GD3D12RHI]; mov r??, [rax+0E0h] (the AGS context)
constexpr char kSite[] = "83 3D ?? ?? ?? ?? 00 48 8B 05 ?? ?? ?? ?? 48 8B ?? ?? ?? 00 00";
constexpr size_t kCallWindow = 0x80;  // the AGS call follows within this many bytes (0x45 / 0x5D in build 19115871)
// agsDriverExtensionsDX12_PushMarker(context, commandList, data) / _PopMarker(context, commandList): argument checks, then the
// driver's marker extension at context+0x1F0
constexpr char kPushBody[] = "48 83 EC 28 48 85 C9 74 ?? 48 85 D2 74 ?? 4D 85 C0 74 ?? 48 8B 89 ?? ?? 00 00 48 85 C9";
constexpr char kPopBody[] = "48 83 EC 28 48 85 C9 74 ?? 48 85 D2 74 ?? 48 8B 89 ?? ?? 00 00 48 85 C9";

using PushFn = int (*)(void* ags, ID3D12GraphicsCommandList* list, const char* name);
using PopFn = int (*)(void* ags, ID3D12GraphicsCommandList* list);

struct Sites {
    int32_t* emit = nullptr;        // GEmitRgpFrameMarkers
    uint8_t* pushCall = nullptr;    // the E8 of RHIPushEvent's AGS call
    uint8_t* popCall = nullptr;     // the E8 of RHIPopEvent's AGS call
    uint8_t* push = nullptr;        // agsDriverExtensionsDX12_PushMarker
    uint8_t* pop = nullptr;         // agsDriverExtensionsDX12_PopMarker
    uint8_t** rhi = nullptr;        // GD3D12RHI (FD3D12DynamicRHI*)
    uint32_t agsOffset = 0;         // its AGS context (0xE0)
};

Sites g_s;
PushFn g_push = nullptr;
PopFn g_pop = nullptr;
std::atomic<int> g_mode{0};         // what the hooks do (bits 1, 2)
bool g_installed = false;
int g_applied = -1;                 // the mode last applied (-1 = none yet)
bool g_playerSet = false;           // the variable was already on before we touched it (Engine.ini): keep it, pass AGS on
bool g_checkedAgs = false;
bool g_hasAgs = false;

// The PIX3 blob WinPixEventRuntime's PIXBeginEvent / PIXSetMarker write for a D3D12 command list (metadata 2,
// WINPIX_EVENT_PIX3BLOB_VERSION): event info (timestamp 0, type << 10), colour, string info (alignment 0, copy chunk 8,
// UTF-16, not a shortcut), the string in 8-byte words with its terminating zero. Returns the size in bytes.
UINT EncodePix3(uint64_t* out, size_t words, const char* name, bool marker)
{
    out[0] = (marker ? 0x008ull : 0x002ull) << 10;  // PIXEvent_SetMarker_NoArgs / PIXEvent_BeginEvent_NoArgs
    out[1] = 0xFF7F7F7Full;                         // colour (grey)
    out[2] = 8ull << 55;
    auto* w = reinterpret_cast<wchar_t*>(out + 3);
    const size_t maxChars = (words - 3) * 4 - 1;
    size_t n = 0;
    for (; name && name[n] && n < maxChars; ++n) w[n] = static_cast<wchar_t>(static_cast<unsigned char>(name[n]));
    const size_t used = (n + 1 + 3) / 4;            // words holding the string and its zero
    for (size_t i = n; i < used * 4; ++i) w[i] = 0;
    return static_cast<UINT>((3 + used) * 8);
}


int PushHook(void* ags, ID3D12GraphicsCommandList* list, const char* name)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode & 1) {
        uint64_t blob[3 + 24];  // names up to 95 characters (longer ones are cut)
        const UINT size = EncodePix3(blob, sizeof blob / sizeof blob[0], name, false);
        list->BeginEvent(2, blob, size);
    }
    return (mode & 2) ? g_push(ags, list, name) : 0;
}

int PopHook(void* ags, ID3D12GraphicsCommandList* list)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode & 1) list->EndEvent();
    return (mode & 2) ? g_pop(ags, list) : 0;
}

bool Discover(const GameModule& game, Sites* s, const char** why)
{
    // The variable: its registration passes its address in r8 and its name in rdx.
    Pattern name;
    for (size_t i = 0; i < sizeof kCvarName; ++i) {
        name.bytes.push_back(reinterpret_cast<const uint8_t*>(kCvarName)[i]);
        name.care.push_back(1);
    }
    const auto names = FindAll(game.rdata, name);
    if (names.size() != 1) { *why = "the variable's name not found once"; return false; }
    int32_t* emit = nullptr;
    int regs = 0;
    for (uint8_t* at : FindAll(game.text, ParsePattern("4C 8D 05 ?? ?? ?? ?? 48 8D 15"))) {
        if (RipTarget(at + 7, 3, 7) != names[0]) continue;
        emit = reinterpret_cast<int32_t*>(RipTarget(at, 3, 7));
        ++regs;
    }
    if (regs != 1 || !game.data.Contains(emit)) { *why = "the variable's registration not found once"; return false; }
    const Pattern pushBody = ParsePattern(kPushBody), popBody = ParsePattern(kPopBody);
    for (uint8_t* at : FindAll(game.text, ParsePattern(kSite))) {
        if (RipTarget(at, 2, 7) != reinterpret_cast<uint8_t*>(emit)) continue;
        uint8_t** rhi = reinterpret_cast<uint8_t**>(RipTarget(at + 7, 3, 7));
        uint32_t off = 0;
        memcpy(&off, at + 17, 4);
        if (!game.data.Contains(rhi) || off > 0x1000) { *why = "the AGS context read does not match"; return false; }
        if (s->rhi && (s->rhi != rhi || s->agsOffset != off)) { *why = "the two sites read different AGS contexts"; return false; }
        s->rhi = rhi;
        s->agsOffset = off;
        for (uint8_t* c = at + 21; c < at + 21 + kCallWindow && c + 5 <= game.text.end; ++c) {
            if (*c != 0xE8) continue;
            uint8_t* t = RipTarget(c, 1, 5);
            if (!game.text.Contains(t)) continue;
            if (MatchesAt(t, pushBody)) {
                if (s->pushCall) { *why = "two push sites"; return false; }
                s->pushCall = c, s->push = t;
                break;
            }
            if (MatchesAt(t, popBody)) {
                if (s->popCall) { *why = "two pop sites"; return false; }
                s->popCall = c, s->pop = t;
                break;
            }
        }
    }
    if (!s->pushCall || !s->popCall) { *why = "RHIPushEvent / RHIPopEvent's AGS calls not found"; return false; }
    if (memcmp(s->push + 22, s->pop + 17, 4) != 0) { *why = "AGS push and pop use different extension slots"; return false; }
    s->emit = emit;
    return true;
}

// The AGS context of the game's RHI (non-null on AMD GPUs once the RHI exists).
bool ReadAgsContext(bool* has)
{
    __try {
        const uint8_t* rhi = *g_s.rhi;
        if (!rhi) return false;
        *has = *reinterpret_cast<void* const*>(rhi + g_s.agsOffset) != nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


}  // namespace

bool InstallGpuPassMarkers(int mode, const wchar_t* ctlPath)
{
    (void)ctlPath;
    if (g_installed) return true;
    if (!mode) return false;
    const GameModule& game = Game();
    const char* why = "";
    Sites s;
    if (!Discover(game, &s, &why)) {
        Log("[gpucrash] GpuPassMarkers: %s (another game build?): off", why);
        return false;
    }
    uint8_t* const mem = AllocNearGame(32);
    if (!mem) { Log("[gpucrash] GpuPassMarkers: no memory near the game's code: off"); return false; }
    g_s = s;
    g_push = reinterpret_cast<PushFn>(s.push);
    g_pop = reinterpret_cast<PopFn>(s.pop);
    // Stubs first (read-write memory), then made executable, then the two calls (each a single 4-byte write).
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const pushHook = reinterpret_cast<void*>(&PushHook);
    void* const popHook = reinterpret_cast<void*>(&PopHook);
    memcpy(mem, jmpAbs, 6), memcpy(mem + 6, &pushHook, 8);
    memcpy(mem + 16, jmpAbs, 6), memcpy(mem + 22, &popHook, 8);
    DWORD old;
    VirtualProtect(mem, 32, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, 32);
    auto rel = [](uint8_t* call, uint8_t* stub, int32_t* out) {
        const int64_t d = stub - (call + 5);
        *out = static_cast<int32_t>(d);
        return d == static_cast<int32_t>(d);
    };
    int32_t relPush = 0, relPop = 0;
    if (!rel(s.pushCall, mem, &relPush) || !rel(s.popCall, mem + 16, &relPop)) {
        Log("[gpucrash] GpuPassMarkers: stub out of reach: off");
        return false;
    }
    if (!WriteCode(s.pushCall + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&relPush), 4))) {
        Log("[gpucrash] GpuPassMarkers: could not patch the push call: off");
        return false;
    }
    if (!WriteCode(s.popCall + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&relPop), 4))) {
        // the push call alone: point it back at AGS
        int32_t back = 0;
        rel(s.pushCall, s.push, &back);
        WriteCode(s.pushCall + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&back), 4));
        Log("[gpucrash] GpuPassMarkers: could not patch the pop call: off");
        return false;
    }
    g_installed = true;
    Log("[gpucrash] GpuPassMarkers installed: D3D12.EmitRgpFrameMarkers exe+0x%X (now %d), AGS push call exe+0x%X -> exe+0x%X, pop "
        "call exe+0x%X -> exe+0x%X, AGS context at GD3D12RHI(exe+0x%X)+0x%X; markers start once the game's GPU is in use",
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(s.emit) - game.base), *s.emit,
        static_cast<unsigned>(s.pushCall - game.base), static_cast<unsigned>(s.push - game.base),
        static_cast<unsigned>(s.popCall - game.base), static_cast<unsigned>(s.pop - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(s.rhi) - game.base), s.agsOffset);
    return true;
}

// Monitor thread, once a second. deviceReady: the game's device exists (its GPU is in use).
void GpuPassMarkersTick(int mode, bool deviceReady)
{
    if (!g_installed || !deviceReady) return;
    if (mode < 0 || mode > 3) mode = 0;
    if (!g_checkedAgs) {
        if (!ReadAgsContext(&g_hasAgs)) return;  // the RHI not up yet
        g_checkedAgs = true;
        g_playerSet = *g_s.emit != 0;
        if (!g_hasAgs)
            Log("[gpucrash] GpuPassMarkers: no AMD AGS context (not an AMD GPU): the engine sends no markers here, nothing changes");
        else if (g_playerSet)
            Log("[gpucrash] GpuPassMarkers: D3D12.EmitRgpFrameMarkers is already %d (Engine.ini): left as it is, AMD markers kept", *g_s.emit);
    }
    if (!g_hasAgs || mode == g_applied) return;
    const int hookMode = g_playerSet ? (mode | 2) : mode;
    g_mode.store(hookMode, std::memory_order_relaxed);
    if (!g_playerSet) *g_s.emit = mode ? 1 : 0;  // read by RHIPushEvent / RHIPopEvent only (the device exists)
    Log("[gpucrash] GpuPassMarkers %d: %s", mode,
        mode == 0   ? "off (the engine's events go nowhere, as in the game)"
        : mode == 1 ? "render pass names in DRED (the GPU crash report names the pass where the GPU stopped)"
        : mode == 2 ? "AMD markers (Radeon GPU Detective / GPU Profiler see the pass names)"
                    : "pass names in DRED and AMD markers");
    g_applied = mode;
}

int GpuPassMarkersMode() { return g_installed ? g_applied : -1; }

void LogGpuPassMarkersStats()
{
}

// ---- self-test ----
UINT GpuPassMarkersTestEncode(uint64_t* out, size_t words, const char* name, bool marker) { return EncodePix3(out, words, name, marker); }

bool GpuPassMarkersTestDiscover(const GameModule& game, uint32_t* emitRva, uint32_t* pushRva, uint32_t* popRva)
{
    Sites s;
    const char* why = "";
    if (!Discover(game, &s, &why)) return false;
    *emitRva = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(s.emit) - game.base);
    *pushRva = static_cast<uint32_t>(s.pushCall - game.base);
    *popRva = static_cast<uint32_t>(s.popCall - game.base);
    return true;
}

// The hooks against a fake AGS and a real command list (self-test): mode as set.
int GpuPassMarkersTestHooks(int mode, void* fakePush, void* fakePop, void* list, const char* name)
{
    const PushFn p = g_push;
    const PopFn q = g_pop;
    const int m = g_mode.load();
    g_push = reinterpret_cast<PushFn>(fakePush);
    g_pop = reinterpret_cast<PopFn>(fakePop);
    g_mode = mode;
    const int r = PushHook(nullptr, static_cast<ID3D12GraphicsCommandList*>(list), name) +
                  PopHook(nullptr, static_cast<ID3D12GraphicsCommandList*>(list));
    g_push = p;
    g_pop = q;
    g_mode = m;
    return r;
}

}  // namespace opf
