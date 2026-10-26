// FrameGenCameraInfo (optional, off by default): AMD's ML frame generation gets the camera it needs.
//
// The game ships AMD's FSR 3.1.0 Unreal plugin. Its frame generation call (ffxDispatchDescFrameGenerationPrepare: depth,
// motion vectors, jitter, render size, frame time, near/far/FOV) has nothing linked to it. AMD's plugin from 3.1.4 on
// also links ffxDispatchDescFrameGenerationPrepareCameraInfo (the camera position and its up/right/forward vectors in
// world space), which AMD's ML frame generation (FSR Frame Generation 4.0.x, RX 9000) uses. A player who drops AMD's
// SDK 2.x DLLs into the game (the loader as amd_fidelityfx_dx12.dll, plus amd_fidelityfx_upscaler_dx12.dll and
// amd_fidelityfx_framegeneration_dx12.dll) gets ML frame generation by default on an RX 9000, and AMD's library then
// runs it with the camera at zero, logging "Camera view matrix data is a prerequisite for MLFI provider enablement"
// (amd_fidelityfx_framegeneration_dx12.dll 4.0.1.2740, the dispatch at +0xFB2F0 walks pNext for the camera desc; without
// one it warns at +0xFB3AA / +0xFB6CB and dispatches anyway). With the game's own FSR 3.1.0 DLL nothing reads it. With
// AMD's FSR 3.1.4 DLL (1.0.1.41314) the library passes the desc chain to its provider, so Adrenalin's driver ML frame
// generation (the upgrade it offers for 3.1.4) gets it; the DLL's own 3.1.4 frame generation ignores it.
//
// The fix fills the camera desc the way AMD's own plugin does (FFXFrameInterpolation.cpp, 3.1.4+): the position is
// FSceneView::ViewLocation, up/right/forward are FSceneView::GetViewUp/GetViewRight/GetViewDirection (UE 5.3: columns
// 1/0/2 of ViewMatrices.ViewMatrix), as floats in Unreal's world units and axes. The game's code that fills the
// prepare desc (build 19115871: exe+0x40987C9, the FViewInfo in r10, the desc on the stack at [rbp+210h]) stores
// header.pNext = nullptr with a 7-byte mov; that mov becomes a call to a stub that does the same store and then calls
// AttachCamera(view, desc), which links one entry of a ring of camera descs (the plugin copies the prepare desc into a
// render command that runs a frame or two later; 64 entries outlive that by far). Each frame the view's matrices are
// checked first (ViewMatrix a rotation with no projection part, InvViewMatrix its transpose, its translation row equal
// to ViewMatrices.ViewOrigin and to ViewLocation); a view that does not pass gets no camera desc (as without the fix).
// The FSceneView offsets come from the UE 5.3 layout relative to WorldToMetersScale, whose offset the game's code
// itself reads right after the site (divss xmm0,[r10+0B98h]: ViewLocation 68h before it, ViewMatrices 7E0h before that).
//
// To show that AMD's library receives it, the export ffxDispatch of amd_fidelityfx_dx12.dll is redirected (its export
// table entry, at load, before the game looks it up) to a counter that passes every call on unchanged: it counts the
// prepare calls and those with a camera desc linked. Nothing else changes: the same calls, the same values.
#include "common.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace opf {
namespace {

// The prepare desc's header.pNext and header.type stores, with the FViewInfo in r10 (build 19115871: exe+0x40987C9):
//   33 C0                    xor eax,eax
//   0F 11 85 ????????        movups [rbp+..],xmm0
//   48 89 85 [desc+8]        mov [rbp+desc+8],rax        header.pNext = nullptr  -> call stub; 3-byte nop
//   0F 11 85 ????????        movups [rbp+..],xmm0
//   89 85 ????????           mov [rbp+..],eax
//   4C 89 65 ??  44 89 65 ??
//   48 C7 85 [desc] 04 00 02 00   mov qword [rbp+desc],20004h   header.type = ..._FRAMEGENERATION_PREPARE
constexpr char kSite[] =
    "33 C0 0F 11 85 ?? ?? ?? ?? 48 89 85 ?? ?? ?? ?? 0F 11 85 ?? ?? ?? ?? 89 85 ?? ?? ?? ?? 4C 89 65 ?? 44 89 65 ?? "
    "48 C7 85 ?? ?? ?? ?? 04 00 02 00";
constexpr size_t kPNextStore = 9, kTypeStore = 37, kStoreLen = 7;
// Later in the same block: viewSpaceToMetersFactor = 1 / View->WorldToMetersScale (divss xmm0,[r10+disp32]).
constexpr char kScaleRead[] = "F3 41 0F 5E 82";
constexpr size_t kScaleSearch = 0x100;

// UE 5.3 FSceneView: ViewMatrices (FViewMatrices, 7E0h bytes), ViewLocation, ViewRotation, BaseHmdOrientation,
// BaseHmdLocation, WorldToMetersScale. FViewMatrices: 15 FMatrix (double 4x4) from ProjectionMatrix; ViewMatrix is the
// 4th, InvViewMatrix the 5th; ViewOrigin after the matrices and PreViewTranslation.
constexpr uint32_t kLocationBeforeScale = 0x68, kMatricesBeforeLocation = 0x7E0;
constexpr uint32_t kViewMatrix = 3 * 0x80, kInvViewMatrix = 4 * 0x80, kViewOrigin = 15 * 0x80 + 0x18;

constexpr uint64_t kTypePrepare = 0x00020004;     // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE
constexpr uint64_t kTypeCameraInfo = 0x0002000A;  // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO

struct ApiHeader {
    uint64_t type;
    ApiHeader* pNext;
};
// ffxDispatchDescFrameGenerationPrepareCameraInfo (ffx_framegeneration.h, SDK 1.1.4 and 2.x)
struct CameraInfoDesc {
    ApiHeader header;
    float position[3], up[3], right[3], forward[3];
};
static_assert(sizeof(CameraInfoDesc) == 0x40);

constexpr uint32_t kRing = 64;
alignas(64) CameraInfoDesc g_ring[kRing];
std::atomic<uint32_t> g_next{0};
// The last camera that passed ReadCamera (render thread writes, any thread reads; a sequence lock): FrameGenLastCamera.
std::atomic<uint32_t> g_lastSeq{0};
float g_lastPos[3], g_lastFwd[3];
std::atomic<uint64_t> g_lastTick{0};

struct Layout {
    uint32_t matrices = 0, location = 0, scale = 0;
};
Layout g_layout;
uint8_t* g_site = nullptr;
uint8_t* g_stub = nullptr;
int32_t g_descDisp = 0;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> s_attached{0}, s_rejected{0};
std::atomic<bool> s_firstLogged{false}, s_rejectLogged{false};
uint64_t s_attachedLogged = 0, s_rejectedLogged = 0;

// The stub the site calls (entered with the return address on the stack): the game's own store, then
// AttachCamera(r10, rbp+desc) with every volatile register, xmm0-5 and the flags kept. rbx holds the entry rsp while the
// stack is aligned for the call.
constexpr uint8_t kStubCode[] = {
    0x48, 0x89, 0x85, 0, 0, 0, 0,                    // mov [rbp+desc+8],rax   (the game's instruction)
    0x9C,                                            // pushfq
    0x50, 0x51, 0x52,                                // push rax / rcx / rdx
    0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,  // push r8 / r9 / r10 / r11
    0x53,                                            // push rbx
    0x48, 0x89, 0xE3,                                // mov rbx,rsp
    0x48, 0x83, 0xE4, 0xF0,                          // and rsp,-16
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,        // sub rsp,80h   (shadow space, xmm0-5)
    0x0F, 0x29, 0x44, 0x24, 0x20,                    // movaps [rsp+20h],xmm0
    0x0F, 0x29, 0x4C, 0x24, 0x30,                    // movaps [rsp+30h],xmm1
    0x0F, 0x29, 0x54, 0x24, 0x40,                    // movaps [rsp+40h],xmm2
    0x0F, 0x29, 0x5C, 0x24, 0x50,                    // movaps [rsp+50h],xmm3
    0x0F, 0x29, 0x64, 0x24, 0x60,                    // movaps [rsp+60h],xmm4
    0x0F, 0x29, 0x6C, 0x24, 0x70,                    // movaps [rsp+70h],xmm5
    0x4C, 0x89, 0xD1,                                // mov rcx,r10            the FViewInfo
    0x48, 0x8D, 0x95, 0, 0, 0, 0,                    // lea rdx,[rbp+desc]      the prepare desc
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,              // mov rax,AttachCamera
    0xFF, 0xD0,                                      // call rax
    0x0F, 0x28, 0x44, 0x24, 0x20,                    // movaps xmm0,[rsp+20h]
    0x0F, 0x28, 0x4C, 0x24, 0x30,                    // movaps xmm1,[rsp+30h]
    0x0F, 0x28, 0x54, 0x24, 0x40,                    // movaps xmm2,[rsp+40h]
    0x0F, 0x28, 0x5C, 0x24, 0x50,                    // movaps xmm3,[rsp+50h]
    0x0F, 0x28, 0x64, 0x24, 0x60,                    // movaps xmm4,[rsp+60h]
    0x0F, 0x28, 0x6C, 0x24, 0x70,                    // movaps xmm5,[rsp+70h]
    0x48, 0x89, 0xDC,                                // mov rsp,rbx
    0x5B,                                            // pop rbx
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,  // pop r11 / r10 / r9 / r8
    0x5A, 0x59, 0x58,                                // pop rdx / rcx / rax
    0x9D,                                            // popfq
    0xC3,                                            // ret
};
constexpr size_t kStubStoreDisp = 3, kStubLeaDisp = 70, kStubFn = 76, kStubSize = 160;
static_assert(sizeof(kStubCode) <= kStubSize);
static_assert(kStubCode[kStubStoreDisp - 1] == 0x85 && kStubCode[kStubLeaDisp - 1] == 0x95 && kStubCode[kStubFn - 1] == 0xB8 &&
              kStubCode[kStubFn + 8] == 0xFF);

const double* Matrix(const uint8_t* view, uint32_t offset) { return reinterpret_cast<const double*>(view + offset); }
bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// The view's camera, or false if its matrices do not look like a camera (another layout, or an unusual view).
bool ReadCamera(const uint8_t* view, CameraInfoDesc* out)
{
    const Layout& L = g_layout;
    const double* v = Matrix(view, L.matrices + kViewMatrix);    // world -> view, row vectors (m[row * 4 + col])
    const double* inv = Matrix(view, L.matrices + kInvViewMatrix);
    const double* origin = Matrix(view, L.matrices + kViewOrigin);
    const double* location = Matrix(view, L.location);
    const float scale = *reinterpret_cast<const float*>(view + L.scale);
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(v[i]) || !std::isfinite(inv[i])) return false;
    if (!(scale > 0.f) || !std::isfinite(scale)) return false;
    // A rotation in the upper 3x3, nothing in the projection column.
    if (!Near(v[3], 0, 1e-9) || !Near(v[7], 0, 1e-9) || !Near(v[11], 0, 1e-9) || !Near(v[15], 1, 1e-9)) return false;
    double col[3][3];
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) col[c][r] = v[r * 4 + c];
    for (int a = 0; a < 3; ++a) {
        if (!Near(col[a][0] * col[a][0] + col[a][1] * col[a][1] + col[a][2] * col[a][2], 1, 1e-4)) return false;
        for (int b = a + 1; b < 3; ++b)
            if (!Near(col[a][0] * col[b][0] + col[a][1] * col[b][1] + col[a][2] * col[b][2], 0, 1e-4)) return false;
    }
    // InvViewMatrix: the transpose, and the camera's position in its last row - the same as ViewOrigin and ViewLocation.
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            if (!Near(inv[r * 4 + c], v[c * 4 + r], 1e-4)) return false;
    const double tol = 1.0 + 1e-6 * (std::fabs(location[0]) + std::fabs(location[1]) + std::fabs(location[2]));
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(location[i]) || !Near(inv[12 + i], origin[i], tol) || !Near(origin[i], location[i], tol)) return false;
        out->position[i] = static_cast<float>(location[i]);
        out->right[i] = static_cast<float>(col[0][i]);    // FSceneView::GetViewRight: ViewMatrix column 0
        out->up[i] = static_cast<float>(col[1][i]);       // GetViewUp: column 1
        out->forward[i] = static_cast<float>(col[2][i]);  // GetViewDirection: column 2
    }
    return true;
}

// Called by the stub on the render thread right after the game cleared the prepare desc's pNext.
void AttachCamera(const uint8_t* view, uint8_t* desc)
{
    if (!g_on.load(std::memory_order_relaxed) || !view) return;
    CameraInfoDesc cam{};
    if (!ReadCamera(view, &cam)) {
        const uint64_t n = s_rejected.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 600 && s_attached.load(std::memory_order_relaxed) == 0 && !s_rejectLogged.exchange(true))
            Log("[fgcam] the first 600 views did not look like the expected camera layout: no camera info sent (the game's "
                "own behaviour); FrameGenCameraInfo does nothing on this build");
        return;
    }
    g_lastSeq.fetch_add(1, std::memory_order_acq_rel);  // odd: being written
    for (int i = 0; i < 3; ++i) {
        g_lastPos[i] = cam.position[i];
        g_lastFwd[i] = cam.forward[i];
    }
    g_lastSeq.fetch_add(1, std::memory_order_acq_rel);
    g_lastTick.store(GetTickCount64(), std::memory_order_relaxed);
    CameraInfoDesc& slot = g_ring[g_next.fetch_add(1, std::memory_order_relaxed) % kRing];
    cam.header.type = kTypeCameraInfo;
    cam.header.pNext = nullptr;
    slot = cam;
    reinterpret_cast<ApiHeader*>(desc)->pNext = &slot.header;
    s_attached.fetch_add(1, std::memory_order_relaxed);
    if (!s_firstLogged.exchange(true))
        Log("[fgcam] first camera info linked to frame generation: position (%.0f, %.0f, %.0f), forward (%.3f, %.3f, %.3f), "
            "up (%.3f, %.3f, %.3f)", cam.position[0], cam.position[1], cam.position[2], cam.forward[0], cam.forward[1],
            cam.forward[2], cam.up[0], cam.up[1], cam.up[2]);
}

// ---- the ffxDispatch counter (verification only) ----
using DispatchFn = int32_t (*)(void* context, const ApiHeader* desc);
std::atomic<DispatchFn> g_dispatch{nullptr};  // the library's own ffxDispatch
uint8_t* g_library = nullptr;
std::atomic<uint64_t> s_prepares{0}, s_withCamera{0}, s_withOurs{0};
uint64_t s_preparesLogged = 0;
std::atomic<bool> s_libraryLogged{false};

bool InRing(const ApiHeader* h)
{
    const auto* p = reinterpret_cast<const uint8_t*>(h);
    const auto* begin = reinterpret_cast<const uint8_t*>(g_ring);
    return p >= begin && p < begin + sizeof g_ring;
}

int32_t DispatchCounter(void* context, const ApiHeader* desc)
{
    if (desc && desc->type == kTypePrepare) {
        s_prepares.fetch_add(1, std::memory_order_relaxed);
        int depth = 0;
        for (const ApiHeader* h = desc->pNext; h && depth < 8; h = h->pNext, ++depth) {
            if (h->type != kTypeCameraInfo) continue;
            s_withCamera.fetch_add(1, std::memory_order_relaxed);
            if (InRing(h)) s_withOurs.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (!s_libraryLogged.exchange(true)) {
            const FsrLibraryInfo lib = DescribeFsrLibrary(g_library);
            const char* what = "its provider gets the camera desc; whether it reads it depends on the provider";
            if (lib.kind == FsrLibrary::Game310) what = "the game's FSR 3.1.0 frame generation ignores it";
            else if (lib.kind == FsrLibrary::Sdk2) what = "its ML frame generation (amd_fidelityfx_framegeneration_dx12.dll) reads it";
            else if (lib.kind == FsrLibrary::Sdk114)
                what = "the library passes it to the provider: a driver ML frame generation (Adrenalin) reads it, the DLL's own "
                       "3.1.4 one ignores it; [fgpace] names the provider";
            Log("[fgcam] first frame generation prepare call reached amd_fidelityfx_dx12.dll, %s (%s)", lib.name, what);
        }
    }
    return g_dispatch.load(std::memory_order_relaxed)(context, desc);
}

// The export table entry of `name` in a loaded module (32-bit RVA), or nullptr.
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
        // a forwarder points into the export directory itself
        if (*slot >= dir.VirtualAddress && *slot < dir.VirtualAddress + dir.Size) return nullptr;
        return slot;
    }
    return nullptr;
}

bool HookDispatch(uint8_t* base, uint32_t sizeOfImage)
{
    uint32_t* const slot = ExportSlot(base, "ffxDispatch");
    if (!slot) {
        Log("[fgcam] amd_fidelityfx_dx12.dll has no ffxDispatch export: calls not counted");
        return false;
    }
    uint8_t* const stub = AllocNear(base, sizeOfImage, 16);
    if (!stub || stub <= base || static_cast<uint64_t>(stub - base) > 0xFFFFFFF0ull) {
        Log("[fgcam] no memory above amd_fidelityfx_dx12.dll for the counter: calls not counted");
        return false;
    }
    g_dispatch = reinterpret_cast<DispatchFn>(base + *slot);
    const void* const hook = reinterpret_cast<const void*>(&DispatchCounter);
    stub[0] = 0xFF;  // jmp [rip+0]
    stub[1] = 0x25;
    memset(stub + 2, 0, 4);
    memcpy(stub + 6, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const auto rva = static_cast<uint32_t>(stub - base);
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return false;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(rva));
    VirtualProtect(slot, sizeof *slot, old, &old);
    g_library = base;
    Log("[fgcam] amd_fidelityfx_dx12.dll ffxDispatch (+0x%X) counted through +0x%X", static_cast<unsigned>(
        reinterpret_cast<uint8_t*>(g_dispatch.load()) - base), rva);
    return true;
}

// LdrRegisterDllNotification (ntdll; documented): called under the loader lock once a DLL is mapped, before its code
// runs and before the game's GetProcAddress for it.
struct UnicodeStr { USHORT Length, MaximumLength; PWSTR Buffer; };
struct DllNotificationData { ULONG Flags; const UnicodeStr* FullDllName; const UnicodeStr* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
using DllNotificationFn = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, PVOID context);
using RegisterFn = LONG(NTAPI*)(ULONG flags, DllNotificationFn callback, PVOID context, PVOID* cookie);
constexpr ULONG kReasonLoaded = 1;
constexpr wchar_t kLibrary[] = L"amd_fidelityfx_dx12.dll";
PVOID g_cookie = nullptr;

VOID CALLBACK OnDllLoaded(ULONG reason, const DllNotificationData* data, PVOID)
{
    if (reason != kReasonLoaded || !data || !data->BaseDllName || g_library) return;
    const UnicodeStr& n = *data->BaseDllName;
    if (n.Length != (std::size(kLibrary) - 1) * sizeof(wchar_t) || _wcsnicmp(n.Buffer, kLibrary, std::size(kLibrary) - 1) != 0) return;
    HookDispatch(static_cast<uint8_t*>(data->DllBase), data->SizeOfImage);
}

bool Fail(const char* why)
{
    Log("[fgcam] frame generation camera info not installed: %s", why);
    return false;
}

}  // namespace

bool FindFrameGenCameraSite()
{
    if (g_site) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kSite));
    if (hits.size() != 1) {
        char why[96];
        snprintf(why, sizeof why, "the prepare desc's code found %zu times (expected once)", hits.size());
        return Fail(why);
    }
    uint8_t* const site = hits[0];
    int32_t pNextDisp, typeDisp;
    memcpy(&pNextDisp, site + kPNextStore + 3, 4);
    memcpy(&typeDisp, site + kTypeStore + 3, 4);
    if (pNextDisp != typeDisp + 8) return Fail("the pNext store is not next to the type store");
    const Pattern scaleRead = ParsePattern(kScaleRead);
    uint32_t scale = 0;
    for (uint8_t* p = site + kTypeStore; p < site + kTypeStore + kScaleSearch && p + 9 <= game.text.end; ++p) {
        if (!MatchesAt(p, scaleRead)) continue;
        memcpy(&scale, p + 5, 4);
        break;
    }
    if (scale < kMatricesBeforeLocation + kLocationBeforeScale || scale > 0x4000 || scale % 4 != 0)
        return Fail("View->WorldToMetersScale not read where expected");
    g_layout.scale = scale;
    g_layout.location = scale - kLocationBeforeScale;
    g_layout.matrices = g_layout.location - kMatricesBeforeLocation;
    g_descDisp = typeDisp;
    g_site = site + kPNextStore;
    Log("[fgcam] frame generation prepare desc filled at exe+0x%X ([rbp+0x%X]); view: WorldToMetersScale +0x%X, ViewLocation "
        "+0x%X, ViewMatrices +0x%X", static_cast<unsigned>(g_site - game.base), static_cast<unsigned>(typeDisp), g_layout.scale,
        g_layout.location, g_layout.matrices);
    return true;
}

bool ApplyFrameGenCameraInfo(bool beforeGameCode)
{
    if (g_stub) return true;
    if (!beforeGameCode) return Fail("can only be installed at start-up (version.dll)");
    if (!FindFrameGenCameraSite()) return false;
    const uint8_t expected[kStoreLen] = {0x48, 0x89, 0x85};
    if (memcmp(g_site, expected, 3) != 0) return Fail("the site was changed by something else");
    uint8_t* const stub = AllocNearGame(kStubSize);
    if (!stub) return Fail("no memory near the game's code for the stub");
    uint8_t code[kStubSize] = {};
    memcpy(code, kStubCode, sizeof kStubCode);
    memcpy(code + kStubStoreDisp, g_site + 3, 4);  // the game's own displacement (pNext)
    memcpy(code + kStubLeaDisp, &g_descDisp, 4);
    const void* const fn = reinterpret_cast<const void*>(&AttachCamera);
    memcpy(code + kStubFn, &fn, sizeof fn);
    memcpy(stub, code, sizeof code);
    DWORD old;
    VirtualProtect(stub, kStubSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, kStubSize);

    // The ffxDispatch counter: only if AMD's library is not loaded yet (the game looks the export up when it loads it).
    if (GetModuleHandleW(kLibrary)) {
        Log("[fgcam] amd_fidelityfx_dx12.dll already loaded: its calls are not counted");
    } else if (const auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
               !reg || reg(0, &OnDllLoaded, nullptr, &g_cookie) != 0) {
        Log("[fgcam] no DLL load notification: frame generation calls not counted");
    }

    uint8_t call[kStoreLen] = {0xE8, 0, 0, 0, 0, 0x66, 0x90};  // call stub; 2-byte nop
    const auto rel = static_cast<int32_t>(stub - (g_site + 5));
    memcpy(call + 1, &rel, 4);
    if (!WriteCode(g_site, call)) return Fail("could not write the call to the stub");
    g_stub = stub;
    g_on = true;
    Log("[fgcam] frame generation camera info installed: the prepare desc's pNext store at exe+0x%X calls the stub",
        static_cast<unsigned>(g_site - Game().base));
    return true;
}

void SetFrameGenCameraInfoEnabled(bool enabled)
{
    if (!g_stub || g_on.load() == enabled) return;
    g_on = enabled;
    Log("[fgcam] frame generation camera info %s", enabled ? "on" : "off (nothing linked, as the game does)");
}

void LogFrameGenCameraInfoStats()
{
    const uint64_t attached = s_attached.load(), rejected = s_rejected.load(), prepares = s_prepares.load();
    if (attached == s_attachedLogged && rejected == s_rejectedLogged && prepares == s_preparesLogged) return;
    Log("[fgcam] camera info linked %llu times, %llu views without a valid camera; AMD's library got %llu prepare calls, %llu "
        "with camera info (%llu of them ours)", static_cast<unsigned long long>(attached), static_cast<unsigned long long>(rejected),
        static_cast<unsigned long long>(prepares), static_cast<unsigned long long>(s_withCamera.load()),
        static_cast<unsigned long long>(s_withOurs.load()));
    s_attachedLogged = attached;
    s_rejectedLogged = rejected;
    s_preparesLogged = prepares;
}

// The camera of the last frame-generation view (UE world units; forward a unit vector) and its age; false if there is
// none yet, it is older than maxAgeMs (frame generation off, menus), or it was being written on every try.
bool FrameGenLastCamera(float pos[3], float fwd[3], uint64_t maxAgeMs)
{
    const uint64_t tick = g_lastTick.load(std::memory_order_relaxed);
    if (!tick || GetTickCount64() - tick > maxAgeMs) return false;
    for (int tries = 0; tries < 4; ++tries) {
        const uint32_t s0 = g_lastSeq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        for (int i = 0; i < 3; ++i) {
            pos[i] = g_lastPos[i];
            fwd[i] = g_lastFwd[i];
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_lastSeq.load(std::memory_order_relaxed) == s0) return true;
    }
    return false;
}

bool FrameGenCameraInfoTestInfo(FrameGenCameraInfoOut* out)
{
    if (!g_site) return false;
    out->site = g_site;
    out->stub = g_stub;
    out->descDisp = g_descDisp;
    out->scale = g_layout.scale;
    out->location = g_layout.location;
    out->matrices = g_layout.matrices;
    out->attached = s_attached.load();
    out->rejected = s_rejected.load();
    out->prepares = s_prepares.load();
    out->withCamera = s_withCamera.load();
    out->withOurs = s_withOurs.load();
    return true;
}

void FrameGenCameraInfoTestAttach(const void* view, void* desc) { AttachCamera(static_cast<const uint8_t*>(view), static_cast<uint8_t*>(desc)); }

void* FrameGenCameraInfoTestDispatch(void* module, uint32_t sizeOfImage, void* original)
{
    if (!g_library && !HookDispatch(static_cast<uint8_t*>(module), sizeOfImage)) return nullptr;
    return reinterpret_cast<void*>(g_dispatch.exchange(reinterpret_cast<DispatchFn>(original)));
}

}  // namespace opf
