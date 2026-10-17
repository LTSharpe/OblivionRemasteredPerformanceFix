// FrameGenPacingTuning (default 1 = Tuning Set B): AMD's frame-generation present pacing, tuned the way AMD
// documents it. With AMD's FSR 3.1.4 or SDK 2.x DLLs the frame generation swap chain paces each present
// against its frame-time history: target = average - variance x varianceFactor - safetyMarginInMs (FidelityFX SDK
// ffx_framegeneration_api_types.h, FfxApiSwapchainFramePacingTuning; swap chain FrameInterpolationSwapchainDX12.cpp
// `conservativeAvg = avg * 0.5 - variance * varianceFactor`). After a few long frames (an arrival in a new area) the
// history holds the game's present for seconds: at Kvatch spot C the render thread waited ~17-20 ms per frame inside
// AMD's Present (a fence poll) for ~4 s while the GPU's own time stayed flat (measured 2026-10-14). AMD's header
// documents exactly this ("Default Tuning now gets stuck at targeting ~33ms after panning to a complex scene") and
// recommends Tuning Set A (0.75 ms, 0.1) or Set B (0.01 ms, 0.3). The game's FSR 3.1.0 plugin never configures it.
//
// The key FRAMEPACINGTUNING (FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12, key 2) is set with the
// library's own ffxConfigure on the swap chain context the game creates: the loader's ffxCreateContext export is
// redirected when the game loads amd_fidelityfx_dx12.dll (as FrameGenCameraInfo does for ffxDispatch), a frame generation swap chain
// context (wrap / new / for-HWND, DX12) is remembered, ffxDestroyContext forgets it. Only with a library whose swap chain
// has the key (SDK 1.1.3+, FidelityFX-SDK FrameInterpolationSwapchainDX12.cpp): AMD's FSR 3.1.4 DLL (1.0.1.41314) or
// the SDK 2.x loader (swap chain 3.1.7), told apart by the DLL's version resource (DescribeFsrLibrary); with the game's
// own FSR 3.1.0 library (whose Configure ignores the key) it does nothing. Mode 0 = the game's pacing (not configured; switched back live: AMD's defaults 0.1 ms / 0.1),
// 1 = Set B, 2 = Set A. allowHybridSpin false, hybridSpinTime 2, allowWaitForSingleObjectOnFence false (AMD's defaults).
// What is drawn does not change; only when the presents go out.
//
// The same ffxCreateContext redirection also asks each new context which provider runs it (ffxQuery with
// FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, SDK 1.1.4+; the 3.1.0 library would hand the query to the provider
// itself, so it is not asked) and logs it: with AMD's 3.1.4 DLL Adrenalin's driver can hand frame generation its own
// ML provider (ids with bit 31 set are the driver's), which nothing else in the log shows.
#include "common.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace opf {
namespace {

// ---- FidelityFX API (SDK 2.3.0 public headers: ffx_api.h, ffx_api_framegeneration_dx12.h, ffx_framegeneration_api_types.h)
struct ApiHeader {
    uint64_t type;
    ApiHeader* pNext;
};
constexpr uint64_t kTypeSwapWrap = 0x00030001;   // ..._FRAMEGENERATIONSWAPCHAIN_WRAP_DX12
constexpr uint64_t kTypeSwapNew = 0x00030005;    // ..._FRAMEGENERATIONSWAPCHAIN_NEW_DX12
constexpr uint64_t kTypeSwapHwnd = 0x00030006;   // ..._FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12
constexpr uint64_t kTypeKeyValue = 0x00030008;   // FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12
constexpr uint64_t kKeyFramePacingTuning = 2;    // FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING
struct PacingTuning {                            // FfxApiSwapchainFramePacingTuning
    float safetyMarginInMs;
    float varianceFactor;
    bool allowHybridSpin;
    uint32_t hybridSpinTime;
    bool allowWaitForSingleObjectOnFence;
};
static_assert(sizeof(PacingTuning) == 20 && offsetof(PacingTuning, hybridSpinTime) == 12, "FfxApiSwapchainFramePacingTuning layout");
struct KeyValueDesc {                            // ffxConfigureDescFrameGenerationSwapChainKeyValueDX12
    ApiHeader header;
    uint64_t key;
    uint64_t u64;
    void* ptr;
};
static_assert(sizeof(KeyValueDesc) == 40 && offsetof(KeyValueDesc, ptr) == 32, "key-value desc layout");
using CreateFn = uint32_t (*)(void** context, ApiHeader* desc, const void* memCb);
using DestroyFn = uint32_t (*)(void** context, const void* memCb);
using ConfigureFn = uint32_t (*)(void** context, const ApiHeader* desc);
using QueryFn = uint32_t (*)(void** context, ApiHeader* desc);
using DispatchFnApi = uint32_t (*)(void** context, const ApiHeader* desc);
constexpr uint64_t kTypeUpscale = 0x00010000;          // FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE
constexpr uint64_t kTypeFrameGen = 0x00020001;         // FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION
constexpr uint64_t kTypeProviderVersion = 6;           // FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION
struct ProviderVersionDesc {                           // ffxQueryGetProviderVersion
    ApiHeader header;
    uint64_t versionId;
    const char* versionName;
};
static_assert(sizeof(ProviderVersionDesc) == 32, "ffxQueryGetProviderVersion layout");

std::atomic<CreateFn> g_create{nullptr};
std::atomic<DestroyFn> g_destroy{nullptr};
std::atomic<ConfigureFn> g_configure{nullptr};
std::atomic<QueryFn> g_query{nullptr};
uint8_t* g_library = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;   // the remembered context vs ffxDestroyContext and the live switch
void* g_context = nullptr;       // the frame generation swap chain context (ffxContext), under g_lock
int g_applied = 0;               // the mode last configured on it (0 = not configured, the game's pacing)
std::atomic<int> g_mode{0};
std::atomic<bool> g_unsupportedLogged{false};
std::atomic<uint64_t> s_contexts{0}, s_configured{0}, s_failed{0}, s_providerLines{0};
bool (*g_supported)() = nullptr;  // self-test override
// the library, described once (at the first context: not under the loader lock), and the providers last seen
SRWLOCK g_infoLock = SRWLOCK_INIT;
bool g_infoDone = false;
FsrLibraryInfo g_info;
char g_providerFg[64] = {}, g_providerUpscale[64] = {};  // under g_infoLock

const FsrLibraryInfo& LibraryInfo()
{
    AcquireSRWLockExclusive(&g_infoLock);
    if (!g_infoDone) {
        g_info = DescribeFsrLibrary(g_library);
        g_infoDone = true;
        Log("[fgpace] amd_fidelityfx_dx12.dll is %s (file version %u.%u.%u.%u): pacing tuning %s, provider query %s",
            g_info.name, g_info.version[0], g_info.version[1], g_info.version[2], g_info.version[3],
            g_info.pacingKey ? "available" : "not available", g_info.providerQuery ? "available" : "not available");
    }
    ReleaseSRWLockExclusive(&g_infoLock);
    return g_info;
}

bool SdkSupportsTuning()
{
    if (g_supported) return g_supported();
    return LibraryInfo().pacingKey;
}

const char* ContextKind(uint64_t type)
{
    switch (type) {
    case kTypeUpscale: return "upscaling";
    case kTypeFrameGen: return "frame generation";
    case kTypeSwapWrap: case kTypeSwapNew: case kTypeSwapHwnd: return "frame generation swap chain";
    default: return nullptr;
    }
}

// Logs which provider runs a context the game just created (upscaling, frame generation, its swap chain).
void LogProvider(void* context, uint64_t type)
{
    const char* kind = ContextKind(type);
    const QueryFn query = g_query.load();
    if (!kind || !query || !context || !(g_supported ? g_supported() : LibraryInfo().providerQuery)) return;
    ProviderVersionDesc q{};
    q.header.type = kTypeProviderVersion;
    void* ctx = context;
    const uint32_t r = query(&ctx, &q.header);
    const bool ok = r == 0 && q.versionName;
    const bool driver = ok && (q.versionId & 0x80000000ull);
    char name[64];
    snprintf(name, sizeof name, "%s%s", ok ? q.versionName : "?", driver ? " (driver)" : "");
    if (type == kTypeFrameGen || type == kTypeUpscale) {
        AcquireSRWLockExclusive(&g_infoLock);
        strcpy_s(type == kTypeFrameGen ? g_providerFg : g_providerUpscale, sizeof g_providerFg, name);
        ReleaseSRWLockExclusive(&g_infoLock);
    }
    if (s_providerLines.fetch_add(1, std::memory_order_relaxed) >= 32) return;  // contexts come back on resolution changes
    if (ok)
        Log("[fgpace] %s context created: provider %s, id 0x%016llX (%s)", kind, q.versionName,
            static_cast<unsigned long long>(q.versionId), driver ? "Adrenalin's driver provider" : "inside AMD's DLL");
    else
        Log("[fgpace] %s context created: provider query returned %u (no answer)", kind, r);
}

constexpr int kMaxMode = 2;
PacingTuning TuningFor(int mode)
{
    PacingTuning t{0.1f, 0.1f, false, 2, false};  // AMD's defaults (= the game's pacing)
    if (mode == 1) { t.safetyMarginInMs = 0.01f; t.varianceFactor = 0.3f; }   // Tuning Set B
    if (mode == 2) { t.safetyMarginInMs = 0.75f; t.varianceFactor = 0.1f; }   // Tuning Set A
    return t;
}
const char* ModeName(int mode) { return mode == 3 ? "lab middle (0.5 ms, 0.2)" : mode == 1 ? "Set B (0.01 ms, 0.3)" : mode == 2 ? "Set A (0.75 ms, 0.1)" : "AMD's defaults (0.1 ms, 0.1)"; }

// Under g_lock (exclusive). Configures the remembered context for `mode` if it differs from what it has.
void ApplyLocked(int mode)
{
    if (!g_context || g_applied == mode) return;
    const ConfigureFn configure = g_configure.load();
    if (!configure) return;
    if (!SdkSupportsTuning()) {
        if (!g_unsupportedLogged.exchange(true))
            Log("[fgpace] frame pacing tuning not supported by %s (needs AMD's FSR 3.1.4 DLL or the SDK 2.x DLLs); nothing changed",
                g_supported ? "this library" : LibraryInfo().name);
        return;
    }
    PacingTuning t = TuningFor(mode);
    KeyValueDesc d{};
    d.header.type = kTypeKeyValue;
    d.key = kKeyFramePacingTuning;
    d.ptr = &t;
    void* ctx = g_context;
    const uint32_t r = configure(&ctx, &d.header);
    if (r == 0) {
        g_applied = mode;
        s_configured.fetch_add(1, std::memory_order_relaxed);
    } else {
        s_failed.fetch_add(1, std::memory_order_relaxed);
    }
    Log("[fgpace] frame generation swap chain pacing set to %s: ffxConfigure returned %u%s", ModeName(mode), r,
        r ? " (left as it was)" : "");
}


uint32_t CreateHook(void** context, ApiHeader* desc, const void* memCb)
{
    const uint64_t type = desc ? desc->type : 0;
    const bool swapChain = type == kTypeSwapWrap || type == kTypeSwapNew || type == kTypeSwapHwnd;
    const uint32_t r = g_create.load()(context, desc, memCb);
    if (r == 0 && context && *context) LogProvider(*context, type);
    if (r != 0 || !swapChain || !context || !*context) return r;
    s_contexts.fetch_add(1, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_lock);
    g_context = *context;
    g_applied = 0;
    const int mode = g_mode.load();
    Log("[fgpace] frame generation swap chain context created (desc type 0x%llX)%s", static_cast<unsigned long long>(type),
        mode ? "" : "; pacing left as the game has it (FrameGenPacingTuning=0)");
    ApplyLocked(mode);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

uint32_t DestroyHook(void** context, const void* memCb)
{
    AcquireSRWLockExclusive(&g_lock);
    if (context && *context && *context == g_context) {
        g_context = nullptr;
        g_applied = 0;
    }
    const uint32_t r = g_destroy.load()(context, memCb);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

// ---- the call order AMD's ML frame generation requires (log only) ----
// AMD (SDK 2.3.0 frame-interpolation-ml.md): per frame ffxConfigure(frame generation) -> ffxDispatch(prepare) ->
// ffxDispatch(frame generation); "calling these functions out of order may result in incorrect rendering results and
// prohibits future automatic driver upgrades". The game's plugin makes these calls (the generation dispatch from the
// swap chain's callback, at present); the redirected ffxConfigure / ffxDispatch exports note each one by its frameID
// (configure desc +136, prepare +16, generation +304; SDK 1.1.4 layouts, unchanged in 2.x) and pass it on unchanged.
// A frame is in order when its configure came before its prepare and its prepare before its generation dispatch.
constexpr uint64_t kTypeFgConfigure = 0x00020002, kTypeFgDispatch = 0x00020003, kTypeFgPrepare = 0x00020004;
constexpr uint32_t kConfigureFrameId = 136, kPrepareFrameId = 16, kDispatchFrameId = 304;
struct OrderEntry { uint64_t id, seqC, seqP; };
OrderEntry g_order[64];              // by frameID & 63, under g_orderLock
SRWLOCK g_orderLock = SRWLOCK_INIT;
uint64_t g_orderSeq = 0, g_lastPrepareId = 0;
bool g_anyPrepare = false;
FrameGenOrderStats g_os;             // under g_orderLock
int g_orderLogged = 0;               // the first calls, logged one by one
std::atomic<DispatchFnApi> g_dispatchApi{nullptr};
std::atomic<ConfigureFn> g_configureNext{nullptr};  // what the redirected ffxConfigure passes calls to

uint64_t FrameIdAt(const ApiHeader* desc, uint32_t off)
{
    uint64_t id;
    memcpy(&id, reinterpret_cast<const uint8_t*>(desc) + off, sizeof id);
    return id;
}

void NoteOrder(uint64_t type, const ApiHeader* desc)
{
    if (!desc || (type != kTypeFgConfigure && type != kTypeFgPrepare && type != kTypeFgDispatch)) return;
    AcquireSRWLockExclusive(&g_orderLock);
    const uint64_t seq = ++g_orderSeq;
    const uint64_t id = FrameIdAt(desc, type == kTypeFgConfigure ? kConfigureFrameId : type == kTypeFgPrepare ? kPrepareFrameId : kDispatchFrameId);
    OrderEntry& e = g_order[id & 63];
    const char* verdict = "";
    if (type == kTypeFgConfigure) {
        ++g_os.configures;
        if (e.id != id) e = OrderEntry{id, 0, 0};
        if (!e.seqC) e.seqC = seq;
    } else if (type == kTypeFgPrepare) {
        ++g_os.prepares;
        if (g_anyPrepare && id != g_lastPrepareId + 1) ++g_os.idJumps;  // AMD: frameID must step by exactly one
        g_anyPrepare = true;
        g_lastPrepareId = id;
        if (e.id != id) e = OrderEntry{id, 0, 0};
        e.seqP = seq;
    } else {
        ++g_os.dispatches;
        if (e.id != id || !e.seqP) { ++g_os.noPrepare; verdict = " (no prepare for this frame)"; }
        else if (!e.seqC) { ++g_os.noConfigure; verdict = " (no configure for this frame)"; }
        else if (!(e.seqC < e.seqP)) { ++g_os.outOfOrder; verdict = " (configure after prepare)"; }
        else { ++g_os.inOrder; verdict = " (frame in order)"; }
    }
    const bool log = g_orderLogged < 9 && !g_supported;
    if (log) ++g_orderLogged;
    ReleaseSRWLockExclusive(&g_orderLock);
    if (log)
        Log("[fgorder] %s frameID %llu, thread %lu%s",
            type == kTypeFgConfigure ? "configure" : type == kTypeFgPrepare ? "prepare  " : "generate ",
            static_cast<unsigned long long>(id), GetCurrentThreadId(), verdict);
}

uint32_t ConfigureHook(void** context, const ApiHeader* desc)
{
    NoteOrder(desc ? desc->type : 0, desc);
    return g_configureNext.load(std::memory_order_relaxed)(context, desc);
}

uint32_t DispatchHook(void** context, const ApiHeader* desc)
{
    NoteOrder(desc ? desc->type : 0, desc);
    return g_dispatchApi.load(std::memory_order_relaxed)(context, desc);
}

// The export table entry of `name` in a loaded module (32-bit RVA), or nullptr (same as FrameGenCameraInfo's).
uint32_t* ExportSlot(uint8_t* base, const char* name)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;
    const auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* names = reinterpret_cast<const uint32_t*>(base + ex->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const uint16_t*>(base + ex->AddressOfNameOrdinals);
    auto* functions = reinterpret_cast<uint32_t*>(base + ex->AddressOfFunctions);
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        if (strcmp(reinterpret_cast<const char*>(base + names[i]), name) != 0) continue;
        if (ordinals[i] >= ex->NumberOfFunctions) return nullptr;
        uint32_t* slot = &functions[ordinals[i]];
        if (*slot >= dir.VirtualAddress && *slot < dir.VirtualAddress + dir.Size) return nullptr;  // forwarder
        return slot;
    }
    return nullptr;
}

// A jmp [rip+0] stub above the module (export RVAs are 32-bit), then the export entry pointed at it.
bool Redirect(uint8_t* base, uint8_t* stub, uint32_t* slot, const void* hook)
{
    stub[0] = 0xFF;
    stub[1] = 0x25;
    memset(stub + 2, 0, 4);
    memcpy(stub + 6, &hook, sizeof hook);
    const auto rva = static_cast<uint32_t>(stub - base);
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return false;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(rva));
    VirtualProtect(slot, sizeof *slot, old, &old);
    return true;
}

bool HookLibrary(uint8_t* base, uint32_t sizeOfImage)
{
    uint32_t* const create = ExportSlot(base, "ffxCreateContext");
    uint32_t* const destroy = ExportSlot(base, "ffxDestroyContext");
    uint32_t* const configure = ExportSlot(base, "ffxConfigure");
    if (!create || !destroy || !configure) {
        Log("[fgpace] amd_fidelityfx_dx12.dll lacks ffxCreateContext/ffxDestroyContext/ffxConfigure: FrameGenPacingTuning unavailable");
        return false;
    }
    uint32_t* const dispatch = ExportSlot(base, "ffxDispatch");
    uint8_t* const stubs = AllocNear(base, sizeOfImage, 64);
    if (!stubs || stubs <= base || static_cast<uint64_t>(stubs - base) > 0xFFFFFFC0ull) {
        Log("[fgpace] no memory above amd_fidelityfx_dx12.dll for the redirections: FrameGenPacingTuning unavailable");
        return false;
    }
    g_create = reinterpret_cast<CreateFn>(base + *create);
    g_destroy = reinterpret_cast<DestroyFn>(base + *destroy);
    g_configure = reinterpret_cast<ConfigureFn>(base + *configure);  // our own calls go straight to the library
    g_configureNext = g_configure.load();
    // ffxDispatch's entry may already lead to FrameGenCameraInfo's counter (its own stub): the order check passes calls on to it
    if (dispatch) g_dispatchApi = reinterpret_cast<DispatchFnApi>(base + *dispatch);
    if (uint32_t* const query = ExportSlot(base, "ffxQuery")) g_query = reinterpret_cast<QueryFn>(base + *query);
    DWORD old;
    VirtualProtect(stubs, 64, PAGE_READWRITE, &old);
    const bool ok1 = Redirect(base, stubs, create, reinterpret_cast<const void*>(&CreateHook));
    const bool ok2 = Redirect(base, stubs + 16, destroy, reinterpret_cast<const void*>(&DestroyHook));
    const bool ok3 = Redirect(base, stubs + 32, configure, reinterpret_cast<const void*>(&ConfigureHook));
    const bool ok4 = dispatch && Redirect(base, stubs + 48, dispatch, reinterpret_cast<const void*>(&DispatchHook));
    VirtualProtect(stubs, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stubs, 64);
    g_library = base;
    Log("[fgpace] amd_fidelityfx_dx12.dll ffxCreateContext (+0x%X) and ffxDestroyContext (+0x%X) watched%s; ffxConfigure / "
        "ffxDispatch watched for the frame generation call order%s",
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(g_create.load()) - base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(g_destroy.load()) - base), ok1 && ok2 ? "" : " (a redirection failed)",
        ok3 && ok4 ? "" : " (not: a redirection failed)");
    return ok1 && ok2;
}

struct UnicodeStr { USHORT Length, MaximumLength; PWSTR Buffer; };
struct DllNotificationData { ULONG Flags; const UnicodeStr* FullDllName; const UnicodeStr* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
using DllNotificationFn = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, PVOID context);
using RegisterFn = LONG(NTAPI*)(ULONG flags, DllNotificationFn callback, PVOID context, PVOID* cookie);
constexpr wchar_t kLibrary[] = L"amd_fidelityfx_dx12.dll";
PVOID g_cookie = nullptr;

VOID CALLBACK OnDllLoaded(ULONG reason, const DllNotificationData* data, PVOID)
{
    if (reason != 1 || !data || !data->BaseDllName || g_library) return;
    const UnicodeStr& n = *data->BaseDllName;
    if (n.Length != (std::size(kLibrary) - 1) * sizeof(wchar_t) || _wcsnicmp(n.Buffer, kLibrary, std::size(kLibrary) - 1) != 0) return;
    HookLibrary(static_cast<uint8_t*>(data->DllBase), data->SizeOfImage);
}

}  // namespace

FsrLibraryInfo DescribeFsrLibrary(const void* module)
{
    FsrLibraryInfo info;
    const auto mod = static_cast<HMODULE>(const_cast<void*>(module ? module : GetModuleHandleW(kLibrary)));
    if (!mod) {
        snprintf(info.name, sizeof info.name, "not loaded");
        return info;
    }
    // the file version from the mapped module's own version resource (VS_FIXEDFILEINFO, signature 0xFEEF04BD)
    bool found = false;
    if (const HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16) /* RT_VERSION */)) {
        const DWORD size = SizeofResource(mod, res);
        const HGLOBAL h = LoadResource(mod, res);
        const auto* p = static_cast<const uint8_t*>(h ? LockResource(h) : nullptr);
        for (DWORD at = 0; p && at + sizeof(VS_FIXEDFILEINFO) <= size; at += 4) {
            const auto* ffi = reinterpret_cast<const VS_FIXEDFILEINFO*>(p + at);
            if (ffi->dwSignature != 0xFEEF04BD) continue;
            info.version[0] = HIWORD(ffi->dwFileVersionMS);
            info.version[1] = LOWORD(ffi->dwFileVersionMS);
            info.version[2] = HIWORD(ffi->dwFileVersionLS);
            info.version[3] = LOWORD(ffi->dwFileVersionLS);
            found = true;
            break;
        }
    }
    const uint64_t v = (uint64_t{info.version[0]} << 48) | (uint64_t{info.version[1]} << 32) | (uint64_t{info.version[2]} << 16) |
                       info.version[3];
    constexpr uint64_t k310 = (1ull << 48) | 36604;                         // 1.0.0.36604: the game's FSR 3.1.0 (SDK 1.1.0)
    constexpr uint64_t k314 = (1ull << 48) | (1ull << 16) | 41314;          // 1.0.1.41314: AMD's FSR 3.1.4 (SDK 1.1.4)
    if (GetModuleHandleW(L"amd_fidelityfx_framegeneration_dx12.dll") || info.version[0] >= 2) {
        info.kind = FsrLibrary::Sdk2;
        snprintf(info.name, sizeof info.name, "AMD's SDK 2.x loader");
    } else if (found && v == k310) {
        info.kind = FsrLibrary::Game310;
        snprintf(info.name, sizeof info.name, "the game's own FSR 3.1.0");
    } else if (found && v >= k314) {
        info.kind = FsrLibrary::Sdk114;
        snprintf(info.name, sizeof info.name, v == k314 ? "AMD's FSR 3.1.4 (SDK 1.1.4)" : "an AMD SDK 1.x build newer than FSR 3.1.4");
    } else {
        info.kind = FsrLibrary::Sdk11Other;  // 3.1.1-3.1.3 or unknown: whether its swap chain takes the key is not known
        snprintf(info.name, sizeof info.name, found ? "an FSR 3.1 build older than 3.1.4" : "an FSR library without a version resource");
    }
    info.pacingKey = info.providerQuery = info.kind == FsrLibrary::Sdk2 || info.kind == FsrLibrary::Sdk114;
    return info;
}

bool ApplyFrameGenPacingTuning(bool beforeGameCode, int mode)
{
    g_mode = mode;
    if (g_library || g_cookie) return true;
    if (!beforeGameCode || GetModuleHandleW(kLibrary)) {
        Log("[fgpace] AMD's FidelityFX library is already loaded: FrameGenPacingTuning needs the game's start (version.dll)");
        return false;
    }
    const auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    if (!reg || reg(0, &OnDllLoaded, nullptr, &g_cookie) != 0) {
        Log("[fgpace] no DLL load notification: FrameGenPacingTuning unavailable");
        return false;
    }
    Log("[fgpace] frame generation pacing tuning: waiting for amd_fidelityfx_dx12.dll (mode %d: %s)", mode,
        mode ? ModeName(mode) : "the game's pacing until switched");
    return true;
}

void SetFrameGenPacingTuning(int mode)
{
    if (mode < 0 || mode > kMaxMode) mode = 0;
    if (g_mode.exchange(mode) == mode) return;
    AcquireSRWLockExclusive(&g_lock);
    // switched back to 0 after a tuning: AMD's defaults, which is what the game's pacing is
    if (g_context && !(mode == 0 && g_applied == 0)) ApplyLocked(mode);
    ReleaseSRWLockExclusive(&g_lock);
}

void LogFrameGenPacingStats()
{
    static uint64_t orderLogged = 0;
    AcquireSRWLockShared(&g_orderLock);
    const FrameGenOrderStats o = g_os;
    ReleaseSRWLockShared(&g_orderLock);
    if (o.configures + o.prepares + o.dispatches != orderLogged) {
        orderLogged = o.configures + o.prepares + o.dispatches;
        Log("[fgorder] since start: %llu frames generated, %llu in AMD's order (configure, prepare, generate), %llu without "
            "a prepare, %llu without a configure, %llu with the configure after the prepare; calls: %llu configure, %llu "
            "prepare; prepare frameIDs not +1: %llu",
            static_cast<unsigned long long>(o.dispatches), static_cast<unsigned long long>(o.inOrder),
            static_cast<unsigned long long>(o.noPrepare), static_cast<unsigned long long>(o.noConfigure),
            static_cast<unsigned long long>(o.outOfOrder), static_cast<unsigned long long>(o.configures),
            static_cast<unsigned long long>(o.prepares), static_cast<unsigned long long>(o.idJumps));
    }
    static uint64_t logged = 0;
    const uint64_t n = s_contexts.load() + s_configured.load() + s_failed.load();
    if (n == logged) return;
    logged = n;
    char providers[160] = {};
    AcquireSRWLockShared(&g_infoLock);
    if (g_providerFg[0] || g_providerUpscale[0])
        snprintf(providers, sizeof providers, "; providers: frame generation %s, upscaling %s", g_providerFg[0] ? g_providerFg : "?",
                 g_providerUpscale[0] ? g_providerUpscale : "?");
    ReleaseSRWLockShared(&g_infoLock);
    Log("[fgpace] swap chain contexts %llu, configured %llu, failed %llu; now %s%s", static_cast<unsigned long long>(s_contexts.load()),
        static_cast<unsigned long long>(s_configured.load()), static_cast<unsigned long long>(s_failed.load()), ModeName(g_applied),
        providers);
}


// self-test: the create/destroy/configure path with fakes (no library)
bool FrameGenPacingTestRun(void* fakeCreate, void* fakeDestroy, void* fakeConfigure, bool supported, int mode, void** outContext)
{
    static bool sup = false;
    sup = supported;
    g_supported = [] { return sup; };
    g_create = reinterpret_cast<CreateFn>(fakeCreate);
    g_destroy = reinterpret_cast<DestroyFn>(fakeDestroy);
    g_configure = reinterpret_cast<ConfigureFn>(fakeConfigure);
    g_mode = mode;
    alignas(8) uint8_t desc[0x40] = {};  // a whole swap chain desc; no factory (lab: nothing to time)
    reinterpret_cast<ApiHeader*>(desc)->type = kTypeSwapNew;
    void* ctx = nullptr;
    const uint32_t r = CreateHook(&ctx, reinterpret_cast<ApiHeader*>(desc), nullptr);
    if (outContext) *outContext = ctx;
    return r == 0;
}
void FrameGenPacingTestSwitch(int mode) { SetFrameGenPacingTuning(mode); }
void FrameGenPacingTestDestroy(void* context) { DestroyHook(&context, nullptr); }
// self-test: a context of `type` created through the hook with `fakeQuery` as ffxQuery; returns the frame generation
// provider the stats line would name ("" if none)
const char* FrameGenPacingTestProvider(void* fakeCreate, void* fakeQuery, uint64_t type)
{
    static char out[64];
    g_supported = [] { return true; };
    g_create = reinterpret_cast<CreateFn>(fakeCreate);
    g_query = reinterpret_cast<QueryFn>(fakeQuery);
    alignas(8) uint8_t desc[0x40] = {};
    reinterpret_cast<ApiHeader*>(desc)->type = type;
    void* ctx = nullptr;
    CreateHook(&ctx, reinterpret_cast<ApiHeader*>(desc), nullptr);
    g_query = nullptr;
    AcquireSRWLockShared(&g_infoLock);
    strcpy_s(out, g_providerFg);
    ReleaseSRWLockShared(&g_infoLock);
    return out;
}
void FrameGenOrderTestReset(void* fakeConfigure, void* fakeDispatch)
{
    g_supported = [] { return true; };  // also keeps the per-call lines out of the self-test's log
    g_configureNext = reinterpret_cast<ConfigureFn>(fakeConfigure);
    g_dispatchApi = reinterpret_cast<DispatchFnApi>(fakeDispatch);
    AcquireSRWLockExclusive(&g_orderLock);
    g_os = FrameGenOrderStats{};
    memset(g_order, 0, sizeof g_order);
    g_anyPrepare = false;
    ReleaseSRWLockExclusive(&g_orderLock);
}
uint32_t FrameGenOrderTestCall(bool configure, void* desc)
{
    void* ctx = reinterpret_cast<void*>(0x1234);
    return configure ? ConfigureHook(&ctx, static_cast<ApiHeader*>(desc)) : DispatchHook(&ctx, static_cast<ApiHeader*>(desc));
}
FrameGenOrderStats FrameGenOrderTestStats()
{
    AcquireSRWLockShared(&g_orderLock);
    const FrameGenOrderStats o = g_os;
    ReleaseSRWLockShared(&g_orderLock);
    return o;
}
int FrameGenPacingTestApplied() { return g_applied; }
void* FrameGenPacingTestContext() { return g_context; }

}  // namespace opf
