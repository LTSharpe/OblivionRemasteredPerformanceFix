// SaveThumbnailAsync: the save's thumbnail screenshot read back without stopping the game thread or the render
// thread.
//
// Every quicksave / autosave / manual save without a menu open asks for a screenshot (the save game's thumbnail). The next
// UGameViewportClient::ProcessScreenShots (exe+0x3222CE0 in build 19115871) reads it back with GetViewportScreenShot >
// FRenderTarget::ReadPixels: the ReadSurfaceCommand render command (ImmediateFlush(FlushRHIThread) + RHIReadSurfaceData:
// D3D12 BlockUntilIdle, copy into a readback buffer, wait, convert to FColor) and FlushRenderingCommands, so the game thread
// waits for the render thread, which waits for the GPU to drain: 42-50 ms at 4K (17-25 at 480p) on the save frame
// (2026-10-19, worker savestall). Then the screenshot delegate is broadcast: the save's callback (exe+0x48AF8A0) copies the
// 33 MB picture, removes itself from the delegate and triggers the event that task A (the JPEG thumbnail) has as its
// prerequisite; task B (the save write) runs after task A.
//
// The fix (SaveThumbnailAsync=1), only when every listener bound to the screenshot delegate is the save's callback (a raw
// delegate, checked by its code), the screenshot delegate cvar is on and no other capture is in flight:
// - game thread, in place of the read-back: removes the save's bindings (what the callback would do in this frame) and
//   enqueues the game's own ReadSurfaceCommand for the same render target, rectangle and flags, but into a marker array and
//   without FlushRenderingCommands; returns "no picture", so ProcessScreenShots ends the request as it does after a
//   broadcast (Reset, the processed delegate), without the alpha loop and broadcast.
// - render thread, when that command runs (the lambda's two call sites redirected): for the marker, UE's own
//   FRHIGPUTextureReadback::EnqueueCopy of the same texture records the copy at the same point of the command stream (the
//   same back buffer the stock read would copy: the dummy back buffer resolves when the RHI thread translates it, before
//   the frame's Present), and the command list is dispatched to the RHI thread (no wait). Formats other than the game's
//   R10G10B10A2 back buffer, MSAA or odd rectangles: the stock read on the render thread into the marker (identical; only
//   the game thread no longer waits).
// - a helper thread waits for the copy's GPU fence, maps the readback texture, converts it with the conversion that is
//   byte-identical to UE's (ScreenshotReadbackConvert's), sets alpha 255 as ProcessScreenShots' loop does before the
//   broadcast, and calls the save's callback with (object, width, height, pixels): what the broadcast calls. The callback's
//   copy runs under the game's own lock; its event starts task A. The delivery never needs the game thread, so no game-
//   thread wait on the save tasks (exit, Altar.Save.AsyncSave 0) can deadlock on it.
// The helper calls the save system's screenshot cache (when bound) before the save's callbacks: the callback's event lets
// the save go on on other threads, and the next save reads what the cache wrote. In the game both run inside the
// broadcast before any other code can look at either, and neither reads the other's data, so this order is the game's
// result. The picture's buffer is allocated before the readback is mapped (retried until Windows gives it); a readback
// that cannot be mapped (never seen) delivers a black picture of the same size, so the save always completes.
//
// The HDR route: when the viewport is HDR (FViewport vtable +0x40), ProcessScreenShots reads the screenshot
// with GetViewportScreenShotHDR (exe+0x37E4FB0) instead: FRenderTarget::ReadLinearColorPixels (exe+0x37F32F0), the same
// render command with its own lambda (exe+0x37D2B50, GDynamicRHI->RHIReadSurfaceData for FLinearColor, vtable +0x240),
// into a TArray<FLinearColor> (16 bytes a pixel, 177 MB at 5120x2160). For the delegate it then converts each pixel on the
// game thread with FLinearColor::ToFColorSRGB (exe+0xEAB6A0), sets alpha 255 and broadcasts the FColor array: in the
// user's HDR10 play (trace, 2026-10-30) ~45 ms of loop plus the read-back's wait in every autosave frame (118 ms). The
// D3D12 read (exe+0x252B510) reads the back buffer's raw rows and converts them (exe+0x251C400; R10G10B10A2: each 10-bit
// channel times 1/1023.f, the constant at exe+0x709E164). So the same picture comes from the same copy as above with a
// table per channel: ToFColorSRGB of v x (1/1023.f) for each of the 1024 values, made at start-up with the game's own
// function (its channels are independent: one sRGB table, per-lane constants), alpha 255. The HDR read's call is
// redirected like the SDR one, its lambda's two call sites and its flush too; a back buffer in another format (scRGB) is
// read the game's way on the render thread and the helper runs the game's ToFColorSRGB on each pixel (the game thread
// does not wait). The self-test runs the game's converter and quantiser on the mapped exe and compares.
//
// Installed at start-up (from save_bytes.cpp) only when SaveThumbnailAsync is on; then live (0 = the game's read-back).
#include "common.h"

#include <atomic>
#include <cstring>
#include <vector>

namespace opf {
namespace {

// After the read-back call in ProcessScreenShots (kReadBackCall in save_bytes.cpp starts 13 bytes before it): the jump,
// then the delegate branch: mov rax,[cvar ptr]; cmp dword [rax],0; jz; the alpha loop; lea rcx,[delegate]; call Broadcast.
constexpr char kAfterReadBack[] =
    "E8 ?? ?? ?? ?? 0F B6 F0 45 32 F6 E9 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 83 38 00 0F 84 ?? ?? ?? ?? 48 8B 4D A7 48 8D 14 91 "
    "48 3B CA 74 18 66 66 66 0F 1F 84 00 00 00 00 00 C6 41 03 FF 48 83 C1 04 48 3B CA 75 F3 44 8B 45 9B 4C 8D 4D A7 8B 55 97 "
    "48 8D 0D ?? ?? ?? ?? E8";
constexpr size_t kCvarMov = 16, kDelegateLea = 80, kBroadcastCall = 87;
// GetViewportScreenShot: ... call FRenderTarget::ReadPixels; test al,al; setnz al; add rsp,78h; ret
constexpr char kGetShotTail[] = "E8 ?? ?? ?? ?? 84 C0 0F 95 C0 48 83 C4 78 C3";
constexpr size_t kGetShotTailOff = 0x66;
// GetViewportScreenShotHDR ends the same way with FRenderTarget::ReadLinearColorPixels
constexpr size_t kGetShotHdrTailOff = 0x6F;
// The HDR route's read, 0x45 bytes before the SDR read-back call: test r13b,r13b / je (SDR) / mov [rbp-29h],rax /
// lea r8,[rbp-29h] / mov eax,[rbp-69h] / lea rdx,[rbp-49h] / mov [rbp-21h],eax / mov eax,[rbp-65h] / mov [rbp-1Dh],eax /
// call GetViewportScreenShotHDR / movzx esi,al / xor r14b,r14b / jmp
constexpr char kHdrReadBack[] =
    "45 84 ED 74 28 48 89 45 D7 4C 8D 45 D7 8B 45 97 48 8D 55 B7 89 45 DF 8B 45 9B 89 45 E3 E8 ?? ?? ?? ?? 0F B6 F0 45 32 F6 E9";
constexpr size_t kHdrReadBackBefore = 0x45, kHdrReadBackCall = 0x1D;
// The HDR route's loop after the read: lea rdx,[rbp+7Fh] / mov rcx,rbx / call FLinearColor::ToFColorSRGB /
// movsxd rsi,[rbp-51h] / mov byte [rbp+82h],0FFh (within 0x200 bytes after the SDR read-back call)
constexpr char kHdrLoop[] = "48 8D 55 7F 48 8B CB E8 ?? ?? ?? ?? 48 63 75 AF C6 85 82 00 00 00 FF";
constexpr size_t kHdrLoopCall = 7;
// FLinearColor::ToFColorSRGB (exe+0xEAB6A0): movups xmm3,[rcx] / lea r8,[sRGB table] / maxps / minps / movdqa / psubd
constexpr char kToFColorSrgb[] = "0F 10 19 4C 8D 05 ?? ?? ?? ?? 0F 5F 1D ?? ?? ?? ?? 0F 5D 1D ?? ?? ?? ?? 66 0F 6F CB 66 0F FA 0D";
constexpr float kR10Scale = 1.0f / 1023.0f;  // the D3D12 read's R10G10B10A2 -> FLinearColor factor (exe+0x709E164)
// FRenderTarget::ReadPixels: the direct lambda call (in the rendering thread), the task creation, FlushRenderingCommands.
constexpr char kDirectLambda[] = "E8 ?? ?? ?? ?? 48 8B D0 48 8D 4C 24 50 E8 ?? ?? ?? ?? E9";
constexpr size_t kDirectLambdaCall = 13;
constexpr char kCreateTask[] = "33 D2 48 8D 4C 24 30 41 B8 FF 00 00 00 E8";
constexpr size_t kCreateTaskCall = 13;
constexpr char kFlushCall[] = "E8 ?? ?? ?? ?? 83 7E 08 00 48 8B 9C 24 D0 00 00 00 0F 9F C0";
// The ReadSurfaceCommand lambda: GetRenderTargetTexture, ImmediateFlush(FlushRHIThread) (ExecuteAndReset(false),
// InitializeImmediateContexts, WaitForRHIThreadTasks), GDynamicRHI->RHIReadSurfaceData (vtable +0x248 for FColor; the
// HDR route's lambda is the same code with +0x240, the FLinearColor overload).
constexpr char kLambda[] =
    "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 41 56 41 57 48 83 EC 60 0F 10 41 20 48 8B EA 0F 11 40 C8 F2 0F 10 41 30 "
    "F2 0F 11 40 08 44 8B 71 38 4C 8B 79 08 48 8B 59 10 48 8B 79 18 48 8B 09 48 8B 01 FF 50 10 33 D2 48 8B CD 48 8B 30 "
    "E8 ?? ?? ?? ?? 48 8B CD E8 ?? ?? ?? ?? 48 8B CD E8 ?? ?? ?? ?? 0F 10 44 24 40 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 40 "
    "48 89 54 24 20 4C 8D 44 24 30 0F 29 44 24 40 4D 8B CF F2 0F 10 84 24 80 00 00 00 48 8B D6 48 8B 01 48 89 5C 24 30 "
    "48 89 7C 24 38 F2 0F 11 44 24 50 44 89 74 24 58 FF 90 ?? 02 00 00";
constexpr size_t kLambdaExecute = 0x4E, kLambdaInitCtx = 0x56, kLambdaRhi = 0x68, kLambdaSlot = 0xAC;
constexpr uint32_t kRhiReadColor = 0x248, kRhiReadLinear = 0x240;
// The render command's DoTask: GetImmediateCommandList_ForRenderCommand; lea rcx,[rdi+50h]; mov rdx,rax; call lambda;
// mov byte [rdi+90h],0
constexpr char kDoTask[] = "E8 ?? ?? ?? ?? 48 8D 4F 50 48 8B D0 E8 ?? ?? ?? ?? C6 87 90 00 00 00 00";
constexpr size_t kDoTaskOffset = 0x11, kDoTaskLambdaCall = 12;
// The HZB occlusion readback's construction: new(0x58) FRHIGPUTextureReadback(FName("HZBGPUReadback")).
constexpr char kHzbReadback[] = "41 B8 01 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 8B CF 48 8B 10 E8";
constexpr size_t kHzbName = 6, kHzbCtorCall = 29;
// FRHIGPUTextureReadback's constructor: call the base constructor, lea rax,[vtable]; mov [rbx],rax (within 0x20 bytes).
// ProcessScreenShots' end: the two TArray frees (FMemory::Free).
constexpr char kFree[] = "48 8B 4D B7 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 8B 4D A7 48 85 C9 74 05 E8";
constexpr size_t kFreeCall = 9;
// The save's screenshot callback (exe+0x48AF8A0): holder W/H, the copy under the lock, Remove(delegate, holder+0x18), ...
constexpr char kSaveCallback[] =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 89 54 24 30 48 8B F9 44 89 44 24 34 49 8B D9 48 8B 44 24 30 48 89 01 "
    "48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8D 4F 08 48 8B D3 E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? "
    "48 8B 57 18 48 85 D2 74 0C 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 F6 89 35 ?? ?? ?? ?? 89 35 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 8D 4F 20 C7 40 20 00 00 80 3F E8 ?? ?? ?? ?? 48 8B 98 68 01 00 00";
constexpr size_t kCbDelegateLea = 0x55, kCbRemoveCall = 0x5C, kCbCopyCall = 0x3A;
// The save system's screenshot cache (exe+0x48CF190): mov [rcx+1B4h],edx; mov rdx,r9; mov [rcx+1B8h],r8d; add rcx,1C0h;
// jmp TArray copy-assign.
constexpr char kCacheCallback[] = "89 91 B4 01 00 00 49 8B D1 44 89 81 B8 01 00 00 48 81 C1 C0 01 00 00 E9";
constexpr size_t kCacheCopyJmp = 0x17;
// A UObject method delegate's ExecuteIfSafe (exe+0x48B77F0): the object through FWeakObjectPtr::Get(+0x18), fn [+0x20].
constexpr char kExecuteIfSafeUObject[] =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B D9 49 8B F9 48 83 C1 18 41 8B F0 8B EA E8 ?? ?? ?? ?? "
    "48 85 C0 74 24 48 8D 54 24 30 48 89 44 24 30 48 8D 4C 24 30 E8 ?? ?? ?? ?? 4C 8B CF 44 8B C6 8B D5 48 8B C8 FF 53 20 B0 01";
constexpr size_t kWeakGetCall = 0x23;
// A raw method delegate's ExecuteIfSafe (vtable +0x60): fn = [rcx+20h], object = [rcx+18h], returns 1.
constexpr char kExecuteIfSafe[] =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B 41 18 8B F2 48 8B E9 48 89 44 24 30 48 8D 54 24 30 "
    "49 8B D9 48 8D 4C 24 30 41 8B F8 E8 ?? ?? ?? ?? 4C 8B CB 44 8B C7 8B D6 48 8B C8 FF 55 20 48 8B 5C 24 38 B0 01";

constexpr size_t kDelegateExecuteIfSafe = 0x60;
constexpr size_t kInstanceHandle = 0x10, kInstanceObject = 0x18, kInstanceFn = 0x20;
constexpr size_t kHolderHandle = 0x18;
// FRHITextureDesc (UE 5.3): Extent +0x24, NumSamples +0x31, Dimension +0x32, Format +0x33.
constexpr size_t kDescExtent = 0x24, kDescSamples = 0x31, kDescDimension = 0x32, kDescFormat = 0x33;
constexpr uint8_t kPfA2B10G10R10 = 18;
// FRHIGPUTextureReadback: vtable [0] scalar deleting destructor, [1] EnqueueCopy(texture); +0x08 fence, +0x18 staging.
constexpr size_t kReadbackSize = 0x58, kReadbackFence = 0x08, kReadbackStaging = 0x18;
// FRHIGPUFence: Poll() at vtable +0x18, NumPendingWriteCommands at +0x10. FDynamicRHI: +0x250 RHIMapStagingSurface,
// +0x258 RHIUnmapStagingSurface (checked through the default render-thread wrappers at +0x498 / +0x4A0).
constexpr size_t kFencePoll = 0x18, kFencePending = 0x10;
constexpr size_t kRhiMap = 0x250, kRhiUnmap = 0x258, kRhiMapRT = 0x498, kRhiUnmapRT = 0x4A0;

struct ScriptArray { void* data; int32_t num, max; };
struct IntVec { int32_t x, y, z; };

using GetShotFn = bool (*)(void* viewport, void* bitmap, const void* rect);
using BroadcastFn = void (*)(void* delegate, int32_t w, int32_t h, const void* bitmap);
using LambdaFn = void (*)(void* ctx, void* cmdList);
using VoidFn = void (*)();
using CmdFn = void (*)(void* cmdList, int arg);
using CmdFn1 = void (*)(void* cmdList);
using FreeFn = void (*)(void* p);
using RemoveFn = bool (*)(void* delegate, uint64_t handle);
using CallbackFn = void (*)(void* obj, int32_t w, int32_t h, const ScriptArray* pixels);
using CtorFn = void* (*)(void* self, uint64_t name);
using DtorFn = void* (*)(void* self, uint32_t flags);
using EnqueueCopyFn = void (*)(void* self, void* cmdList, void* texture, const IntVec* pos, uint32_t slice, const IntVec* size);
using PollFn = bool (*)(void* fence);
using MapFn = void (*)(void* rhi, void* texture, void* fence, void** data, int32_t* width, int32_t* height, uint32_t gpu);
using UnmapFn = void (*)(void* rhi, void* texture, uint32_t gpu);
using RtTexFn = void** (*)(void* renderTarget);
using DescFn = const uint8_t* (*)(void* texture);
using ToFColorFn = uint32_t* (*)(const float* linear, uint32_t* out);

GetShotFn g_getShot = nullptr;
GetShotFn g_getShotHdr = nullptr;    // GetViewportScreenShotHDR (nullptr: the HDR route stays the game's)
LambdaFn g_lambdaHdr = nullptr;      // ReadLinearColorPixels' lambda
ToFColorFn g_toFColorSrgb = nullptr;
uint8_t* g_hdrCall = nullptr;         // the HDR read's call in ProcessScreenShots, once redirected
uint8_t g_lutB[1024], g_lutG[1024], g_lutR[1024];  // ToFColorSRGB of v x (1/1023.f), per output channel
BroadcastFn g_broadcast = nullptr;
LambdaFn g_lambda = nullptr;
VoidFn g_flush = nullptr;
CmdFn g_executeAndReset = nullptr;
CmdFn1 g_initContexts = nullptr;
FreeFn g_free = nullptr;
CtorFn g_readbackCtor = nullptr;
void* g_readbackVtable = nullptr;
void** g_rhiGlobal = nullptr;   // &GDynamicRHI
int* const* g_delegateCvar = nullptr;  // the screenshot delegate cvar's value pointer
uint8_t* g_delegate = nullptr;  // UGameViewportClient::ScreenshotCapturedDelegate
uint8_t* g_callbackSeen = nullptr;  // the save callback, once its code was checked
RemoveFn g_remove = nullptr;
uint8_t* g_arrayCopy = nullptr;  // TArray<FColor> copy-assign (the callbacks' copy)
HANDLE g_done = nullptr;         // set while no job is in flight
bool g_installed = false;
std::atomic<int> g_mode{0};
thread_local bool t_skipFlush = false;

// FindAll with the search started at a rarer byte of the pattern (anchor): memchr stops less often (a pattern that starts
// with 48 stops ~9 million times in this exe); every hit is checked against the whole pattern.
std::vector<uint8_t*> FindAnchored(const Section& text, const char* pattern, size_t anchor)
{
    const Pattern full = ParsePattern(pattern);
    Pattern tail;
    tail.bytes.assign(full.bytes.begin() + static_cast<std::ptrdiff_t>(anchor), full.bytes.end());
    tail.care.assign(full.care.begin() + static_cast<std::ptrdiff_t>(anchor), full.care.end());
    std::vector<uint8_t*> out;
    for (uint8_t* h : FindAll(text, tail))
        if (h - anchor >= text.begin && MatchesAt(h - anchor, full)) out.push_back(h - anchor);
    return out;
}

constexpr int kMaxListeners = 6;
struct Listener { CallbackFn fn; void* obj; bool save; };
struct Job {
    ScriptArray marker{nullptr, 0, 0};   // the render command's OutData (only the fallback writes it: FColor, or FLinearColor on the HDR route)
    int32_t w = 0, h = 0;
    bool hdr = false;                    // the HDR route (GetViewportScreenShotHDR)
    Listener listeners[kMaxListeners];
    int listenerCount = 0;
    std::atomic<int> state{0};           // 0 enqueued, 1 copy recorded (readback), 2 fallback read done, 3 failed
    void* readback = nullptr;
    uint64_t tEnqueue = 0, tRender = 0;
};
std::atomic<Job*> g_job{nullptr};
HANDLE g_wake = nullptr;
HANDLE g_thread = nullptr;
std::atomic<uint64_t> s_deferred{0}, s_async{0}, s_fallback{0}, s_skipped{0}, s_verified{0}, s_mismatch{0};
std::atomic<bool> g_timers{false};

uint64_t Ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

double Ms(uint64_t ticks)
{
    static const double perMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1000.0; }();
    return static_cast<double>(ticks) / perMs;
}

template <class T> T VSlot(void* object, size_t offset) { return reinterpret_cast<T>((*reinterpret_cast<void***>(object))[offset / 8]); }

bool InText(const void* p) { return Game().text.Contains(static_cast<const uint8_t*>(p)); }

// The bound listeners, if every one is either the save's thumbnail callback (a raw delegate, removed in this frame as it
// removes itself) or the save system's permanent screenshot cache (UVAltarSaveSystem::Initialize binds it as a UObject
// delegate: it stores W/H and copies every screenshot for its "after a level change" path; it stays bound). out[] is in
// invocation-list order; at least one save callback is required.
using WeakGetFn = uint8_t* (*)(const void* weak);
const char* g_why = "";
uint64_t g_whyFn = 0;
uint8_t* g_cacheSeen = nullptr;
int SaveListeners(Listener* out, uint64_t* handles, void** holders, int* saveCount)
{
    g_whyFn = 0;
    *saveCount = 0;
    const uint8_t* const d = g_delegate;
    if (*reinterpret_cast<const int32_t*>(d + 0x14) != 0) { g_why = "inside a broadcast"; return -1; }  // inside a broadcast of it
    const auto* const list = *reinterpret_cast<uint8_t* const*>(d);
    const int32_t num = *reinterpret_cast<const int32_t*>(d + 8);
    const uint64_t base = reinterpret_cast<uint64_t>(Game().base);
    int n = 0;
    uint8_t* cacheCopy = nullptr;
    for (int32_t i = 0; i < num; ++i) {
        const uint8_t* const e = list + 16ll * i;
        uint8_t* const inst = *reinterpret_cast<uint8_t* const*>(e);
        if (*reinterpret_cast<const int32_t*>(e + 8) == 0 || !inst) continue;  // unbound slot: the broadcast skips it too
        if (n == kMaxListeners) { g_why = "too many listeners"; return -1; }
        uint8_t* const exec = VSlot<uint8_t*>(inst, kDelegateExecuteIfSafe);
        uint8_t* const fn = *reinterpret_cast<uint8_t**>(inst + kInstanceFn);
        g_whyFn = reinterpret_cast<uint64_t>(fn) - base;
        if (!InText(exec) || !InText(fn)) { g_why = "a listener outside the game's code"; return -1; }
        if (MatchesAt(exec, ParsePattern(kExecuteIfSafe))) {
            if (fn != g_callbackSeen) {
                if (!MatchesAt(fn, ParsePattern(kSaveCallback))) { g_why = "a raw listener that is not the save's callback"; return -1; }
                if (RipTarget(fn + kCbDelegateLea, 3, 7) != g_delegate) { g_why = "callback for another delegate"; return -1; }
                uint8_t* const remove = RipTarget(fn + kCbRemoveCall, 1, 5);
                uint8_t* const copy = RipTarget(fn + kCbCopyCall, 1, 5);
                if (!InText(remove) || !InText(copy)) { g_why = "unexpected Remove"; return -1; }
                g_remove = reinterpret_cast<RemoveFn>(remove);
                g_arrayCopy = copy;
                g_callbackSeen = fn;
            }
            void* const obj = *reinterpret_cast<void**>(inst + kInstanceObject);
            const uint64_t handle = *reinterpret_cast<const uint64_t*>(inst + kInstanceHandle);
            if (!obj || handle == 0 || *reinterpret_cast<const uint64_t*>(static_cast<uint8_t*>(obj) + kHolderHandle) != handle) { g_why = "handle mismatch"; return -1; }
            out[n] = {reinterpret_cast<CallbackFn>(fn), obj, true};
            handles[n] = handle;
            holders[n] = obj;
            ++*saveCount;
        } else if (MatchesAt(exec, ParsePattern(kExecuteIfSafeUObject))) {
            if (fn != g_cacheSeen) {
                if (!MatchesAt(fn, ParsePattern(kCacheCallback))) {
                    g_why = "a UObject listener that is not the save system's screenshot cache";
                    return -1;
                }
                g_cacheSeen = fn;
            }
            cacheCopy = RipTarget(fn + kCacheCopyJmp, 1, 5);
            uint8_t* const obj = reinterpret_cast<WeakGetFn>(RipTarget(exec + kWeakGetCall, 1, 5))(inst + kInstanceObject);
            if (!obj) { g_why = "the cache's object is gone"; return -1; }
            out[n] = {reinterpret_cast<CallbackFn>(fn), obj, false};
            handles[n] = 0;
            holders[n] = nullptr;
        } else {
            g_why = "a listener of another kind";
            return -1;
        }
        ++n;
    }
    // The cache's copy is the same TArray copy the save callback uses (checked once both were seen).
    if (cacheCopy && cacheCopy != g_arrayCopy) { g_why = "the cache listener copies with another function"; g_cacheSeen = nullptr; return -1; }
    return n;
}


void FreeGame(ScriptArray& a)
{
    if (a.data) g_free(a.data);
    a = {nullptr, 0, 0};
}

void FinishJob(Job* j)
{
    FreeGame(j->marker);
    delete j;
    g_job.store(nullptr, std::memory_order_release);
    SetEvent(g_done);
}

// What the broadcast does for each listener, but the save system's screenshot cache (the listeners that are not a save
// callback) first and the save's callbacks last, each group from the end of the invocation list as the broadcast walks
// it: the save callback's event lets the save go on on other threads, and the next save reads what the cache wrote.
void Deliver(const Listener* ls, int n, int32_t w, int32_t h, const ScriptArray* arr)
{
    for (int i = n - 1; i >= 0; --i)
        if (!ls[i].save) ls[i].fn(ls[i].obj, w, h, arr);
    for (int i = n - 1; i >= 0; --i)
        if (ls[i].save) ls[i].fn(ls[i].obj, w, h, arr);
}

// The picture's buffer, retried until Windows gives it: without a picture the save would never complete.
uint32_t* AllocPixels(size_t bytes)
{
    for (bool logged = false;; logged = true) {
        if (void* const p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return static_cast<uint32_t*>(p);
        if (!logged) Log("[savethumb] no memory for the save's %zu MB picture yet: retrying until there is", bytes >> 20);
        Sleep(20);
    }
}

void FillBlack(uint32_t* pixels, size_t count)
{
    for (size_t i = 0; i < count; ++i) pixels[i] = 0xFF000000u;
}

// The HDR route's picture from the back buffer's R10G10B10A2 rows: what ReadLinearColorPixels and ProcessScreenShots'
// loop make of them (each channel through the tables, alpha 255).
void ConvertR10ToSrgb(uint32_t w, uint32_t rows, const uint8_t* in, uint32_t pitch, uint32_t* out)
{
    for (uint32_t y = 0; y < rows; ++y) {
        const uint32_t* const src = reinterpret_cast<const uint32_t*>(in + static_cast<size_t>(y) * pitch);
        uint32_t* const dst = out + static_cast<size_t>(y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t p = src[x];
            dst[x] = g_lutB[(p >> 20) & 0x3FF] | static_cast<uint32_t>(g_lutG[(p >> 10) & 0x3FF]) << 8 |
                     static_cast<uint32_t>(g_lutR[p & 0x3FF]) << 16 | 0xFF000000u;
        }
    }
}

// The HDR route's loop itself on rows of a TArray<FLinearColor> (the render thread's stock read): the game's
// ToFColorSRGB on each pixel, alpha 255.
void ConvertLinearToSrgb(size_t from, size_t to, const float* in, uint32_t* out)
{
    for (size_t i = from; i < to; ++i) {
        uint32_t c;
        g_toFColorSrgb(in + i * 4, &c);
        out[i] = c | 0xFF000000u;
    }
}

// Helper thread: waits for the copy, converts, delivers to the save's callback.
DWORD WINAPI Worker(void*)
{
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        Job* const j = g_job.load(std::memory_order_acquire);
        if (!j) continue;
        int st;
        while ((st = j->state.load(std::memory_order_acquire)) == 0) WaitForSingleObject(g_wake, 50);
        const uint64_t t0 = Ticks();
        const size_t count = static_cast<size_t>(j->w) * static_cast<size_t>(j->h);
        uint32_t* pixels = nullptr;
        uint32_t* own = nullptr;  // the buffer this thread allocated (freed after the delivery)
        double waitMs = 0, convertMs = 0;
        if (st == 1) {
            uint8_t* const rb = static_cast<uint8_t*>(j->readback);
            void* const fence = *reinterpret_cast<void**>(rb + kReadbackFence);
            void* const staging = *reinterpret_cast<void**>(rb + kReadbackStaging);
            int spins = 0;
            for (;;) {
                if (*reinterpret_cast<volatile int32_t*>(static_cast<uint8_t*>(fence) + kFencePending) == 0 && VSlot<PollFn>(fence, kFencePoll)(fence)) break;
                Sleep(1);
                ++spins;
            }
            const uint64_t t1 = Ticks();
            waitMs = Ms(t1 - t0);
            pixels = own = AllocPixels(count * 4);  // before the map, so the mapped readback is never held while waiting for memory
            void* const rhi = *g_rhiGlobal;
            void* data = nullptr;
            int32_t pitchPx = 0, rows = 0;
            VSlot<MapFn>(rhi, kRhiMap)(rhi, staging, fence, &data, &pitchPx, &rows, 0);
            if (data && pitchPx >= j->w && rows >= j->h) {
                // Row bands on ScreenshotParallel's helpers when that fix is on (else all here): the same conversion, then
                // ProcessScreenShots' alpha loop (A = 255) on the same rows.
                // The HDR route: the tables instead (alpha 255 included).
                struct Ctx { uint32_t w; const uint8_t* in; uint32_t pitch; uint32_t* out; } const ctx{
                    static_cast<uint32_t>(j->w), static_cast<const uint8_t*>(data), static_cast<uint32_t>(pitchPx) * 4u, pixels};
                if (j->hdr)
                    ScreenshotParallelRows(static_cast<uint32_t>(j->h), count * 4, [](const void* c, uint32_t y0, uint32_t y1) {
                        const auto* x = static_cast<const Ctx*>(c);
                        ConvertR10ToSrgb(x->w, y1 - y0, x->in + static_cast<size_t>(y0) * x->pitch, x->pitch,
                                         x->out + static_cast<size_t>(y0) * x->w);
                    }, &ctx);
                else
                    ScreenshotParallelRows(static_cast<uint32_t>(j->h), count * 4, [](const void* c, uint32_t y0, uint32_t y1) {
                        const auto* x = static_cast<const Ctx*>(c);
                        uint32_t* const out = x->out + static_cast<size_t>(y0) * x->w;
                        ConvertR10G10B10A2ToColor(x->w, y1 - y0, x->in + static_cast<size_t>(y0) * x->pitch, x->pitch, out);
                        for (size_t i = 0, n = static_cast<size_t>(y1 - y0) * x->w; i < n; ++i) out[i] |= 0xFF000000u;
                    }, &ctx);
            } else {
                Log("[savethumb] the readback could not be mapped (data %p, pitch %d, rows %d): the save gets a black picture this time",
                    data, pitchPx, rows);
                FillBlack(pixels, count);
            }
            if (data) VSlot<UnmapFn>(rhi, kRhiUnmap)(rhi, staging, 0);
            reinterpret_cast<DtorFn>((*reinterpret_cast<void***>(rb))[0])(rb, 0);
            _aligned_free(rb);
            j->readback = nullptr;
            convertMs = Ms(Ticks() - t1);
        } else if (st == 2 && j->hdr && j->marker.data && static_cast<size_t>(j->marker.num) == count) {
            pixels = own = AllocPixels(count * 4);
            struct Ctx { const float* in; uint32_t* out; uint32_t w; } const ctx{static_cast<const float*>(j->marker.data), pixels,
                                                                               static_cast<uint32_t>(j->w)};
            ScreenshotParallelRows(static_cast<uint32_t>(j->h), count * 16, [](const void* c, uint32_t y0, uint32_t y1) {
                const auto* x = static_cast<const Ctx*>(c);
                ConvertLinearToSrgb(static_cast<size_t>(y0) * x->w, static_cast<size_t>(y1) * x->w, x->in, x->out);
            }, &ctx);
            convertMs = Ms(Ticks() - t0);
        } else if (st == 2 && !j->hdr && j->marker.data && static_cast<size_t>(j->marker.num) == count) {
            pixels = static_cast<uint32_t*>(j->marker.data);
            for (size_t i = 0; i < count; ++i) pixels[i] |= 0xFF000000u;
        }
        // No picture (a stock read with an unexpected size; not seen): a black one of the stated size, so the thumbnail
        // task never reads past an empty array. Then the delivery (the save callbacks were removed from the delegate on
        // the game thread, as they remove themselves in the broadcast).
        if (!pixels) {
            pixels = own = AllocPixels(count * 4);
            FillBlack(pixels, count);
        }
        const uint64_t t2 = Ticks();
        const ScriptArray arr{pixels, static_cast<int32_t>(count), static_cast<int32_t>(count)};
        Deliver(j->listeners, j->listenerCount, j->w, j->h, &arr);
        const double deliverMs = Ms(Ticks() - t2);
        if (own) VirtualFree(own, 0, MEM_RELEASE);
        Log("[savethumb] the save's %dx%d picture (%s route, %s) delivered off the game thread: GPU copy wait %.1f ms, "
            "conversion %.1f ms, the save's callbacks %.1f ms", j->w, j->h, j->hdr ? "HDR" : "SDR",
            st == 1 ? "async copy" : "read on the render thread", waitMs, convertMs, deliverMs);
        FinishJob(j);
    }
}

// Render thread: the ReadSurfaceCommand lambda (both call sites).
bool RecordCopy(Job* j, void** ctx, void* cmdList)
{
    void* const rt = ctx[0];
    void** const ref = VSlot<RtTexFn>(rt, 0x10)(rt);
    void* const tex = ref ? *ref : nullptr;
    if (!tex) return false;
    const uint8_t* const desc = VSlot<DescFn>(tex, 0x08)(tex);
    const int32_t* const rect = reinterpret_cast<const int32_t*>(ctx + 2);
    const int32_t ex = *reinterpret_cast<const int32_t*>(desc + kDescExtent), ey = *reinterpret_cast<const int32_t*>(desc + kDescExtent + 4);
    if (desc[kDescSamples] != 1 || desc[kDescDimension] != 0 || desc[kDescFormat] != kPfA2B10G10R10) return false;
    if (rect[0] < 0 || rect[1] < 0 || rect[2] > ex || rect[3] > ey || rect[2] - rect[0] != j->w || rect[3] - rect[1] != j->h) return false;
    void* const rb = _aligned_malloc(kReadbackSize, 16);
    if (!rb) return false;
    memset(rb, 0, kReadbackSize);
    g_readbackCtor(rb, 0);  // NAME_None
    if (*reinterpret_cast<void**>(rb) != g_readbackVtable || !*reinterpret_cast<void**>(static_cast<uint8_t*>(rb) + kReadbackFence)) {
        reinterpret_cast<DtorFn>((*reinterpret_cast<void***>(rb))[0])(rb, 0);
        _aligned_free(rb);
        return false;
    }
    const IntVec pos{rect[0], rect[1], 0}, size{j->w, j->h, 1};
    reinterpret_cast<EnqueueCopyFn>((*reinterpret_cast<void***>(rb))[1])(rb, cmdList, tex, &pos, 0, &size);
    if (!*reinterpret_cast<void**>(static_cast<uint8_t*>(rb) + kReadbackStaging)) {
        reinterpret_cast<DtorFn>((*reinterpret_cast<void***>(rb))[0])(rb, 0);
        _aligned_free(rb);
        return false;
    }
    // ImmediateFlush(DispatchToRHIThread): the copy goes to the RHI thread and the GPU now, without waiting for it.
    g_executeAndReset(cmdList, 0);
    g_initContexts(cmdList);
    j->readback = rb;
    return true;
}

// Both lambdas (ReadPixels' and, on the HDR route, ReadLinearColorPixels': the same context layout) for the job's route.
void LambdaFor(void* ctx, void* cmdList, LambdaFn orig, bool hdr)
{
    Job* const j = g_job.load(std::memory_order_acquire);
    void** const c = static_cast<void**>(ctx);
    if (!j || j->hdr != hdr || c[1] != &j->marker || j->state.load(std::memory_order_relaxed) != 0) {
        orig(ctx, cmdList);
        return;
    }
    j->tRender = Ticks();
    if (RecordCopy(j, c, cmdList)) {
        s_async.fetch_add(1, std::memory_order_relaxed);
        j->state.store(1, std::memory_order_release);
    } else {
        orig(ctx, cmdList);  // the stock read, into the marker (the render thread waits as before; the game thread not)
        s_fallback.fetch_add(1, std::memory_order_relaxed);
        j->state.store(2, std::memory_order_release);
    }
    SetEvent(g_wake);
}

void LambdaHook(void* ctx, void* cmdList) { LambdaFor(ctx, cmdList, g_lambda, false); }
void LambdaHookHdr(void* ctx, void* cmdList) { LambdaFor(ctx, cmdList, g_lambdaHdr, true); }

// ProcessScreenShots' HDR read: taken over like the SDR one ("no picture" to the caller), else the game's.
bool HdrReadBackHook(void* viewport, void* bitmap, const void* rect)
{
    if (SaveThumbTakeOver(viewport, rect, true)) return false;
    return g_getShotHdr(viewport, bitmap, rect);
}

// ProcessScreenShots' broadcast of any other screenshot: while a taken-over thumbnail is still being delivered by the
// helper (which writes the save system's screenshot cache, a listener of this broadcast too), wait for it first, so the
// listeners never run on two threads at once and see the pictures in the game's order.
std::atomic<uint64_t> s_broadcastWaits{0};
void BroadcastHook(void* delegate, int32_t w, int32_t h, const void* bitmap)
{
    if (g_job.load(std::memory_order_acquire)) {
        const uint64_t t0 = Ticks();
        WaitForSingleObject(g_done, INFINITE);
        s_broadcastWaits.fetch_add(1, std::memory_order_relaxed);
        Log("[savethumb] a screenshot broadcast waited %.1f ms for the save's picture to be delivered first", Ms(Ticks() - t0));
    }
    g_broadcast(delegate, w, h, bitmap);
}

void FlushHook()
{
    if (t_skipFlush) return;
    g_flush();
}

uint8_t* MakeThunk(void* target)
{
    uint8_t* const region = AllocNearGame(16);
    if (!region) return nullptr;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &target, sizeof target);
    DWORD old;
    VirtualProtect(region, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 16);
    return region;
}

bool RedirectCall(uint8_t* call, uint8_t* thunk)
{
    const int64_t rel = thunk - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}

uint8_t* FindIn(uint8_t* from, size_t len, const char* pattern)
{
    const Pattern p = ParsePattern(pattern);
    uint8_t* hit = nullptr;
    for (uint8_t* q = from; q + p.size() <= from + len; ++q)
        if (MatchesAt(q, p)) {
            if (hit) return nullptr;  // not unique
            hit = q;
        }
    return hit;
}

bool Fail(const char* why)
{
    Log("[savethumb] %s: SaveThumbnailAsync unavailable", why);
    return false;
}

// A read function's render command (FRenderTarget::ReadPixels or ReadLinearColorPixels, the same code): its lambda (whose
// RHI read is the given vtable slot), the lambda's direct call, the task's DoTask call of it, FlushRenderingCommands.
struct ReadChain { uint8_t* lambda = nullptr; uint8_t* directCall = nullptr; uint8_t* doTaskCall = nullptr; uint8_t* flushCall = nullptr; };
const char* FindReadChain(uint8_t* readFn, uint32_t rhiSlot, ReadChain* out)
{
    const GameModule& game = Game();
    uint8_t* const direct = FindIn(readFn, 0x260, kDirectLambda);
    uint8_t* const create = FindIn(readFn, 0x260, kCreateTask);
    uint8_t* const flush = FindIn(readFn, 0x260, kFlushCall);
    if (!direct || !create || !flush) return "its render command is not as expected";
    uint8_t* const lambda = RipTarget(direct + kDirectLambdaCall, 1, 5);
    if (!game.text.Contains(lambda) || !MatchesAt(lambda, ParsePattern(kLambda)) ||
        *reinterpret_cast<const uint32_t*>(lambda + kLambdaSlot) != rhiSlot)
        return "the ReadSurfaceCommand lambda is not as expected";
    uint8_t* const createTask = RipTarget(create + kCreateTaskCall, 1, 5);
    // The task's vtable: a lea of an .rdata address in CreateTask whose slot 1 (DoTask) calls the lambda.
    uint8_t* doTaskCall = nullptr;
    for (uint8_t* q = createTask; q < createTask + 0x200 && game.text.Contains(q + 7); ++q) {
        if (q[0] != 0x48 || q[1] != 0x8D || (q[2] & 0xC7) != 0x05) continue;
        uint8_t* const vt = RipTarget(q, 3, 7);
        if (vt < game.rdata.begin || vt + 16 > game.rdata.end) continue;
        uint8_t* const doTask = *reinterpret_cast<uint8_t**>(vt + 8);
        if (!game.text.Contains(doTask) || !MatchesAt(doTask + kDoTaskOffset, ParsePattern(kDoTask))) continue;
        uint8_t* const call = doTask + kDoTaskOffset + kDoTaskLambdaCall;
        if (RipTarget(call, 1, 5) == lambda) { doTaskCall = call; break; }
    }
    if (!doTaskCall) return "the render command's DoTask was not found";
    *out = {lambda, direct + kDirectLambdaCall, doTaskCall, flush};
    return nullptr;
}

// The HDR route (start-up, after the SDR one): GetViewportScreenShotHDR's call in ProcessScreenShots, its read's render
// command, ToFColorSRGB from the conversion loop and the tables. Anything unexpected: the HDR route stays the game's.
bool InstallHdrRoute(uint8_t* readBackCall, uint8_t* flushThunk)
{
    const GameModule& game = Game();
    const char* why = nullptr;
    uint8_t* const site = readBackCall - kHdrReadBackBefore;
    uint8_t* const call = site + kHdrReadBackCall;
    uint8_t* const getShot = MatchesAt(site, ParsePattern(kHdrReadBack)) ? RipTarget(call, 1, 5) : nullptr;
    uint8_t* const loop = FindIn(readBackCall, 0x200, kHdrLoop);
    uint8_t* const toFColor = loop ? RipTarget(loop + kHdrLoopCall, 1, 5) : nullptr;
    ReadChain chain;
    if (!getShot || !game.text.Contains(getShot)) why = "its read call is not as expected";
    else if (!MatchesAt(getShot + kGetShotHdrTailOff, ParsePattern(kGetShotTail))) why = "GetViewportScreenShotHDR is not as expected";
    else if (!toFColor || !game.text.Contains(toFColor) || !MatchesAt(toFColor, ParsePattern(kToFColorSrgb))) why = "its ToFColorSRGB loop is not as expected";
    else why = FindReadChain(RipTarget(getShot + kGetShotHdrTailOff, 1, 5), kRhiReadLinear, &chain);
    uint8_t* const thunk = why ? nullptr : MakeThunk(reinterpret_cast<void*>(&HdrReadBackHook));
    uint8_t* const hdrLambdaThunk = why ? nullptr : MakeThunk(reinterpret_cast<void*>(&LambdaHookHdr));
    if (!why && (!thunk || !hdrLambdaThunk)) why = "no free memory near the game's code";
    if (why) {
        Log("[savethumb] the HDR screenshot route: %s: HDR saves keep the game's read-back", why);
        return false;
    }
    g_toFColorSrgb = reinterpret_cast<ToFColorFn>(toFColor);
    for (uint32_t v = 0; v < 1024; ++v) {
        const float f = static_cast<float>(v) * kR10Scale;
        const float in[4] = {f, f, f, 1.0f};
        uint32_t c = 0;
        g_toFColorSrgb(in, &c);
        g_lutB[v] = static_cast<uint8_t>(c);
        g_lutG[v] = static_cast<uint8_t>(c >> 8);
        g_lutR[v] = static_cast<uint8_t>(c >> 16);
    }
    g_getShotHdr = reinterpret_cast<GetShotFn>(getShot);
    g_lambdaHdr = reinterpret_cast<LambdaFn>(chain.lambda);
    if (!RedirectCall(chain.directCall, hdrLambdaThunk) || !RedirectCall(chain.doTaskCall, hdrLambdaThunk) ||
        !RedirectCall(chain.flushCall, flushThunk) || !RedirectCall(call, thunk)) {
        Log("[savethumb] the HDR screenshot route: a call could not be redirected: HDR saves keep the game's read-back");
        return false;  // the read call is redirected last: without it the other three only pass through
    }
    g_hdrCall = call;
    Log("[savethumb] HDR screenshot route: GetViewportScreenShotHDR exe+0x%X (call exe+0x%X), lambda exe+0x%X, ToFColorSRGB exe+0x%X: installed",
        static_cast<unsigned>(getShot - game.base), static_cast<unsigned>(call - game.base),
        static_cast<unsigned>(chain.lambda - game.base), static_cast<unsigned>(toFColor - game.base));
    return true;
}

}  // namespace

// readBackCall: the call of GetViewportScreenShot in ProcessScreenShots (already redirected by save_bytes.cpp).
bool InstallSaveThumbAsync(uint8_t* readBackCall, void* getViewportScreenShot)
{
    if (g_installed) return true;
    const uint64_t install0 = Ticks();
    const GameModule& game = Game();
    if (!MatchesAt(readBackCall, ParsePattern(kAfterReadBack))) return Fail("ProcessScreenShots' delegate branch is not as expected");
    g_delegateCvar = reinterpret_cast<int* const*>(RipTarget(readBackCall + kCvarMov, 3, 7));
    g_delegate = RipTarget(readBackCall + kDelegateLea, 3, 7);
    uint8_t* const getShot = static_cast<uint8_t*>(getViewportScreenShot);
    if (!game.text.Contains(getShot) || !MatchesAt(getShot + kGetShotTailOff, ParsePattern(kGetShotTail)))
        return Fail("GetViewportScreenShot is not as expected");
    uint8_t* const readPixels = RipTarget(getShot + kGetShotTailOff, 1, 5);
    if (!game.text.Contains(readPixels)) return Fail("FRenderTarget::ReadPixels not found");
    ReadChain chain;
    if (const char* why = FindReadChain(readPixels, kRhiReadColor, &chain)) return Fail(why);
    uint8_t* const lambda = chain.lambda;
    uint8_t* const doTaskCall = chain.doTaskCall;
    uint8_t* const flush = chain.flushCall;
    uint8_t* const freeSite = FindIn(readBackCall, 0x100, kFree);
    if (!freeSite) return Fail("FMemory::Free not found");
    const auto hzb = FindAnchored(game.text, kHzbReadback, 1);
    uint8_t* ctor = nullptr;
    for (uint8_t* h : hzb) {
        static const wchar_t kName[] = L"HZBGPUReadback";
        const uint8_t* const name = RipTarget(h + kHzbName, 3, 7);
        if (name >= game.rdata.begin && name + sizeof kName <= game.rdata.end && memcmp(name, kName, sizeof kName) == 0) {
            ctor = RipTarget(h + kHzbCtorCall, 1, 5);
            break;
        }
    }
    if (!ctor || !game.text.Contains(ctor)) return Fail("FRHIGPUTextureReadback's constructor not found");
    void* vtable = nullptr;
    for (uint8_t* q = ctor; q < ctor + 0x20; ++q)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x05) {
            uint8_t* const vt = RipTarget(q, 3, 7);
            if (vt >= game.rdata.begin && vt + 16 <= game.rdata.end && game.text.Contains(*reinterpret_cast<uint8_t**>(vt)) &&
                game.text.Contains(*reinterpret_cast<uint8_t**>(vt + 8))) vtable = vt;
            break;
        }
    if (!vtable) return Fail("FRHIGPUTextureReadback's vtable not found");
    uint8_t* const executeAndReset = RipTarget(lambda + kLambdaExecute, 1, 5);
    uint8_t* const initContexts = RipTarget(lambda + kLambdaInitCtx, 1, 5);
    void** const rhiGlobal = reinterpret_cast<void**>(RipTarget(lambda + kLambdaRhi, 3, 7));
    uint8_t* const flushFn = RipTarget(flush, 1, 5);
    if (!game.text.Contains(executeAndReset) || !game.text.Contains(initContexts) || !game.text.Contains(flushFn))
        return Fail("unexpected call targets");
    uint8_t* const broadcastCall = readBackCall + kBroadcastCall;
    uint8_t* const broadcastFn = RipTarget(broadcastCall, 1, 5);
    if (!game.text.Contains(broadcastFn)) return Fail("the screenshot broadcast is not as expected");
    uint8_t* const lambdaThunk = MakeThunk(reinterpret_cast<void*>(&LambdaHook));
    uint8_t* const broadcastThunk = MakeThunk(reinterpret_cast<void*>(&BroadcastHook));
    uint8_t* const flushThunk = MakeThunk(reinterpret_cast<void*>(&FlushHook));
    if (!lambdaThunk || !flushThunk || !broadcastThunk) return Fail("no free memory near the game's code");
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!g_done) return Fail("no event");
    g_thread = g_wake ? CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr) : nullptr;
    if (!g_thread) return Fail("no helper thread");
    DescribeThread(g_thread, L"OPF save thumbnail");
    g_getShot = reinterpret_cast<GetShotFn>(getShot);
    g_broadcast = reinterpret_cast<BroadcastFn>(broadcastFn);
    g_lambda = reinterpret_cast<LambdaFn>(lambda);
    g_flush = reinterpret_cast<VoidFn>(flushFn);
    g_executeAndReset = reinterpret_cast<CmdFn>(executeAndReset);
    g_initContexts = reinterpret_cast<CmdFn1>(initContexts);
    g_free = reinterpret_cast<FreeFn>(RipTarget(freeSite + kFreeCall, 1, 5));
    g_readbackCtor = reinterpret_cast<CtorFn>(ctor);
    g_readbackVtable = vtable;
    g_rhiGlobal = rhiGlobal;
    if (!RedirectCall(broadcastCall, broadcastThunk) || !RedirectCall(chain.directCall, lambdaThunk) ||
        !RedirectCall(doTaskCall, lambdaThunk) || !RedirectCall(flush, flushThunk))
        return Fail("a call could not be redirected");
    g_installed = true;
    InstallHdrRoute(readBackCall, flushThunk);
    Log("[savethumb] ReadPixels exe+0x%X (lambda exe+0x%X, DoTask call exe+0x%X, flush exe+0x%X), screenshot delegate exe+0x%X, "
        "FRHIGPUTextureReadback ctor exe+0x%X: installed (%.1f ms)",
        static_cast<unsigned>(readPixels - game.base), static_cast<unsigned>(lambda - game.base), static_cast<unsigned>(doTaskCall - game.base),
        static_cast<unsigned>(flush - game.base), static_cast<unsigned>(g_delegate - game.base), static_cast<unsigned>(ctor - game.base),
        Ms(Ticks() - install0));
    return true;
}

void SetSaveThumbAsync(int mode, bool timers)
{
    g_timers = timers;
    if (!g_installed) return;
    mode = mode != 0;
    if (g_mode.exchange(mode) != mode)
        Log("[savethumb] %s", mode == 2 ? "SaveThumbnailAsync 2: the thumbnail read back asynchronously and compared with the game's read-back"
                              : mode ? "SaveThumbnailAsync on: the save's thumbnail is read back without stopping the game"
                                     : "SaveThumbnailAsync off: the save's thumbnail is read back the game's way");
}

// Game thread, in place of GetViewportScreenShot in ProcessScreenShots. true = taken over (the caller returns "no
// picture"); false = the caller does the game's read-back.
bool SaveThumbTakeOver(void* viewport, const void* rect, bool hdr)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (!g_installed || mode == 0) return false;
    if (g_job.load(std::memory_order_acquire)) { s_skipped.fetch_add(1, std::memory_order_relaxed); return false; }
    if (!*g_delegateCvar || **g_delegateCvar == 0) return false;
    Listener ls[kMaxListeners];
    uint64_t handles[kMaxListeners];
    void* holders[kMaxListeners];
    int saves = 0;
    const int n = SaveListeners(ls, handles, holders, &saves);
    if (n <= 0 || saves == 0) {
        s_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const int32_t* const r = static_cast<const int32_t*>(rect);
    const int32_t w = r[2] - r[0], h = r[3] - r[1];
    if (r[0] != 0 || r[1] != 0 || w <= 0 || h <= 0 || w > 16384 || h > 16384) { s_skipped.fetch_add(1, std::memory_order_relaxed); return false; }
    Job* const j = new Job;
    j->w = w;
    j->h = h;
    j->hdr = hdr;
    for (int i = 0; i < n; ++i) j->listeners[i] = ls[i];
    j->listenerCount = n;
    // What the save callbacks would do in this frame: leave the delegate (their own Remove is then skipped: handle 0).
    // The save system's cache listener stays bound, as it does in the game.
    for (int i = 0; i < n; ++i) {
        if (!ls[i].save) continue;
        *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(holders[i]) + kHolderHandle) = 0;
        g_remove(g_delegate, handles[i]);
    }
    ResetEvent(g_done);
    j->tEnqueue = Ticks();
    g_job.store(j, std::memory_order_release);
    t_skipFlush = true;
    (hdr ? g_getShotHdr : g_getShot)(viewport, &j->marker, rect);  // enqueues the ReadSurfaceCommand into the marker; no flush
    t_skipFlush = false;
    s_deferred.fetch_add(1, std::memory_order_relaxed);
    SetEvent(g_wake);
    return true;
}

bool SaveThumbHdrInfo(uint8_t** readCall, void** toFColorSrgb)
{
    *readCall = g_hdrCall;
    *toFColorSrgb = reinterpret_cast<void*>(g_toFColorSrgb);
    return g_hdrCall != nullptr;
}

void SaveThumbTestHdrConvert(const uint32_t* r10, uint32_t n, uint32_t* out)
{
    ConvertR10ToSrgb(n, 1, reinterpret_cast<const uint8_t*>(r10), n * 4, out);
}

// Self-test: the delivery order of listeners marked save callback / cache (order[k] = the index of the k-th call).
namespace {
int* t_testOrder = nullptr;
int t_testCalls = 0;
void TestRecord(void* obj, int32_t, int32_t, const ScriptArray*) { t_testOrder[t_testCalls++] = static_cast<int>(reinterpret_cast<intptr_t>(obj)); }
}  // namespace
int SaveThumbTestDeliverOrder(const bool* save, int n, int* order)
{
    if (n < 0 || n > kMaxListeners) return -1;
    Listener ls[kMaxListeners];
    for (int i = 0; i < n; ++i) ls[i] = {&TestRecord, reinterpret_cast<void*>(static_cast<intptr_t>(i)), save[i]};
    t_testOrder = order;
    t_testCalls = 0;
    const ScriptArray arr{nullptr, 0, 0};
    Deliver(ls, n, 1, 1, &arr);
    return t_testCalls;
}

void LogSaveThumbStats()
{
    const uint64_t d = s_deferred.exchange(0), a = s_async.exchange(0), f = s_fallback.exchange(0), s = s_skipped.exchange(0),
                   v = s_verified.exchange(0), m = s_mismatch.exchange(0);
    if (d || s || v || m)
        Log("[savethumb] since last report: %llu thumbnails taken over (%llu copied asynchronously, %llu read the game's way on the "
            "render thread), %llu left to the game; verify: %llu identical, %llu different",
            static_cast<unsigned long long>(d), static_cast<unsigned long long>(a), static_cast<unsigned long long>(f),
            static_cast<unsigned long long>(s), static_cast<unsigned long long>(v), static_cast<unsigned long long>(m));
}

}  // namespace opf
