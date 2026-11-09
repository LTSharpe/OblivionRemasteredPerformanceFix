// GpuMenuShot (worker gpushot 2026-11-02, findings "gpushot"; public since 1.3.0, port130k; review rev130f): the menu-open
// screenshot's backdrop made on the GPU, without the game thread waiting for it. [Fixes] GpuMenuShot = 0: not installed
// (start-up); 1 (default): on; then live 0/1. 3 (test value, not in the shipped ini): for the next 4 menu screenshots also
// lets the game make its own picture (slow: the game thread waits for it, as stock) and logs whether ours is the same
// ("[gpushot] compare ...": every byte in SDR; on the HDR10 route the largest colour-channel difference too), then works
// as 1. Lab builds: 2 = compute only and compare, the game's picture shown.
//
// What the game does (build 19115871, findings "menucost", "menus round 2", "gpushot"): when the player menu (inventory,
// magic, character, map, System page) opens, VBackgroundWidgetBase::RequestCapture binds Altar's screenshot service to the
// engine's screenshot delegate. In that frame N UGameViewportClient::ProcessScreenShots (exe+0x3222CE0) reads the back buffer
// back synchronously (the game thread flushes the render thread, which waits for the GPU; ~45 ms at 4K), converts it to 8-bit
// FColor (SDR: UE's requantize, fix 40; HDR: FLinearColor then ToFColorSRGB), sets alpha 255 and broadcasts it to the
// service (exe+0x48C2AF0: W/H, a copy of the picture into its cache +0x398, then its handlers' delegate +0x370 = the backdrop
// handler exe+0x4924EF0: same-size ImageResize = fix 10's 2x2 filter, CreateTexture2DFromImage, SetBrushFromTexture) and to
// the save system's screenshot cache. Later requests in the same menu get the service's cached picture at once.
//
// What this does instead, in the same frame and in the same order of GPU work:
//  1. game thread (save_thumb.cpp's takeover of the read-back, lab branch): only if the delegate's listeners are exactly the
//     service (+ the save cache) and every handler bound on the service is the backdrop handler (whitelist), the service's
//     delegate runs as the engine's broadcast runs it, with an empty picture: its cache stays empty, the handlers make their
//     textures with ImageResize skipped (GpuMenuShotSkipResize). The real picture comes from SaveThumbnailAsync's
//     asynchronous read-back of the same back buffer: the save cache from its helper, the service's cache on the game thread
//     (GpuMenuShotGameThreadFrame, MenuOpenFlashFix's per-frame hook), with the game's cached re-broadcast to handlers that
//     asked in between.
//  2. game thread: a second ReadSurfaceCommand is enqueued for a marker array (no flush; the render thread runs our code
//     in its place): after the texture's render-thread initialisation (enqueued by the handler), before the frame's present.
//  3. render thread: the back buffer and each new texture's RHI texture are checked (format, size, mips), the job is queued
//     for the RHI thread and a 1x1 FRHIGPUTextureReadback copy of the back buffer is enqueued (the trigger; its staging
//     texture is 1x1, made once).
//  4. RHI thread: FD3D12CommandContext::RHICopyTexture (its context vtable slot) runs the trigger copy (the engine does the
//     back buffer's barriers and flushes its batch), then records on the same D3D12 command list: the back buffer's pixels
//     copied into our buffer, our compute shader (each 10-bit channel through a 1024-entry table built with the game's own
//     conversion, then the 2x2 box filter of fix 10, HdrMenuBackground's byte table when on, alpha 255), and the result
//     copied into the textures (their barriers through the engine's own AddTransitionBarrier / FlushResourceBarriers, as
//     RHICopyTexture does). The engine's state cache is told that the compute PSO and root signature changed (as its own
//     TransitionComputeState does). The texture's initial upload ran earlier on the same (direct) context, and frame N+1's
//     draws come later in the same command stream.
// Fallback: a job that is not drawn (a check fails on the game, render or RHI thread; the lab build can force each) has its
// handlers called again with the real picture when the service's picture is delivered: the game's texture, a few frames late.
// One job at a time: a new one starts only when nothing of ours is in flight and the GPU finished the last one (the trigger's
// fence comes after our commands), so buffers and table blocks are never reused or released under the GPU.
// Bit identity: the requantize and the box filter are integer functions of the same back-buffer bits; GpuMenuShot=3 shows
// the GPU texture and reads it back to compare every byte with the game's picture (it also makes the game's synchronous
// read, so it is slow; the lab build's 2 computes on the GPU but shows the game's picture).
//
// Leftover save-thumbnail bindings (shot130q): a save made in a menu leaves the save's thumbnail callback bound on the
// service for the session (a game bug, also without the mod; see kLeftoverCallback); while the save system is idle they
// are held out of the placeholder call and get the real picture with the service's delivery (equivalent, not identical:
// the finished save's buffer they write gets the picture a few frames later, or keeps older bytes if the delivery drops).
// Not taken over (the game's way): another screenshot or save picture in flight, any other listener or handler, a back
// buffer that is not R10G10B10A2 (scRGB HDR), the first menu screenshot of a session (it records the back buffer), the GPU
// still busy with the last one. An unexpected state on the render or RHI thread switches the key off for the session; a
// back buffer or texture whose size changed between the game thread and the RHI thread (a resize) only declines that shot
// (rev130f G-S4). Device removed: our commands are no-ops, the trigger's fence never signals, every later open declines
// ("GPU has not finished") = the game's way; SaveThumbnailAsync's waits are bounded (10 s), the engine handles the crash.
// The command list and context offsets it writes on the RHI thread (+0xA8 list, +0x28 ID3D12GraphicsCommandList, +0x170
// NumCommands, +0x20 the ID3D12Resource) are pinned in RHICopyTexture's own code (G-S3).
// The texture's CPU-side copy (bulk data) holds the placeholder, not the picture: nothing reads it unless the texture's
// resource were re-created (UpdateResource), which the handler does not do.
// Memory: two buffers of the screen's size in video memory (2 x 31.6 MB at 4K) from the first use, 53 KB of tables (upload
// heap), a 1x1 trigger staging texture; modes 2/3 also a 31.6 MB read-back buffer (system memory). SaveThumbnailAsync's
// read-back for the caches is a full-size staging texture per screenshot, released after its delivery (as for saves).
#include "common.h"

#include <d3d12.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "shaders/gpu_menu_shot_cs.h"

namespace opf {
namespace {

// ---- patterns (build 19115871 addresses in the comments) ----
// FD3D12CommandContext::RHICopyTexture (exe+0x254BF50, IRHICommandContext subobject = context + 0xE8): its prologue, the
// source / destination RetrieveTexture (presentable back-buffer reference: +0x250 / +0x248, viewport +0x118 / +0x2A,
// +0x110 / +0x90; GetTextureBaseRHI vtable +0x30, linked object +0x130 + gpu * 8; FD3D12Resource at +0xB8, tracking bit
// +0xDF, default state +0xC0), then the scoped barriers: TransitionResource (+0x167), AddTransitionBarrier (+0x1A5) and
// FlushResourceBarriers (+0x22D).
constexpr char kCopyTexture[] =
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 48 FF FF FF 48 81 EC B8 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 "
    "85 A0 00 00 00 0F BC 81 18 01 00 00 BF 20 00 00 00 48 89 55 B0 8B F7 48 89 4D B8 0F 45 F0 4D 8B F1 4D 8B E0 4C 8B EA 4C "
    "8B F9 48 85 D2 74 5B 48 8B 02 48 8B CA 48 8B DA FF 50 08 8B 08 48 C1 E9 12 F6 C1 01 74 2C 80 BB 50 02 00 00 00 48 8B 8B "
    "48 02 00 00 74 15 0F B6 81 18 01 00 00 38 41 2A 74 09 48 8B 99 10 01 00 00 EB 07 48 8B 99 90 00 00 00 48 8B 03 48 8B CB "
    "FF 50 30 48 85 C0 74 0A 48 8B B4 F0 30 01 00 00 EB 02 33 F6 41 0F BC 87 18 01 00 00 48 89 75 80 0F 45 F8 4D 85 E4 74 5F "
    "49 8B 04 24 49 8B CC 49 8B DC FF 50 08 8B 08 48 C1 E9 12 F6 C1 01 74 2F 41 80 BC 24 50 02 00 00 00 49 8B 8C 24 48 02 00 "
    "00 74 15 0F B6 81 18 01 00 00 38 41 2A 74 09 48 8B 99 10 01 00 00 EB 07 48 8B 99 90 00 00 00 48 8B 03 48 8B CB FF 50 30 "
    "48 85 C0 74 0A 48 8B 9C F8 30 01 00 00 EB 02 33 DB 48 8B 8E B8 00 00 00 49 8D B7 18 FF FF FF 48 89 5D 88 48 89 4D D8 C6 "
    "44 24 51 00 F6 81 DF 00 00 00 01 74 21 48 8B D1 C7 44 24 20 FF FF FF FF 48 8B CE 41 B9 00 08 00 00 41 B8 FF FF FF 7F E8 "
    "?? ?? ?? ?? EB 41 44 8B 81 C0 00 00 00 41 81 F8 00 08 00 00 74 31 41 8B C0 C1 E8 0B A8 01 75 27 48 8B D1 48 C7 44 24 28 "
    "00 00 00 00 48 8B CE C7 44 24 20 FF FF FF FF 41 B9 00 08 00 00 E8 ?? ?? ?? ?? C6 44 24 51 01 48 8B 8B B8 00 00 00 48 89 "
    "4D D0 C6 44 24 50 00 F6 81 DF 00 00 00 01 74 21 48 8B D1 C7 44 24 20 FF FF FF FF 48 8B CE 41 B9 00 04 00 00 41 B8 FF FF "
    "FF 7F E8 ?? ?? ?? ?? EB 41 44 8B 81 C0 00 00 00 41 81 F8 00 04 00 00 74 31 41 8B C0 C1 E8 0A A8 01 75 27 48 8B D1 48 C7 "
    "44 24 28 00 00 00 00 48 8B CE C7 44 24 20 FF FF FF FF 41 B9 00 04 00 00 E8 ?? ?? ?? ?? C6 44 24 50 01 48 8B CE E8 ?? ?? "
    "?? ?? 49 8B 04 24 49 8B CC FF";
constexpr size_t kCopyAnchor = 0x2D;  // "0F BC 81 18 01 00 00 BF 20 ..." (rarer than the prologue)
constexpr size_t kCallTransition = 0x167, kCallAddBarrier = 0x1A5, kCallFlush = 0x22D;
// RHICopyTexture's own recording of the copy (+0x35B, rev130f G-S3): mov rax,[rsi+0A8h] (the context's command list); mov
// rdx,[rbp-80h]; mov rcx,[rax+28h] (its ID3D12GraphicsCommandList); inc dword [rax+170h] (NumCommands); mov r8,[rdx+0B8h];
// mov rdx,[rbp-78h]; mov rax,[rcx]; mov r8,[r8+20h] (the ID3D12Resource)
constexpr char kCopyPins[] =
    "48 8B 86 A8 00 00 00 48 8B 55 80 48 8B 48 28 FF 80 70 01 00 00 4C 8B 82 B8 00 00 00 48 8B 55 88 48 8B 01 4D 8B 40 20";
constexpr size_t kCopyPinsAt = 0x35B;
// Its end: UpdateResidency for source and destination (exe+0x250B1F0), then ConditionalSplitCommandList.
constexpr char kResidency[] =
    "48 8B 45 80 48 8B CE 48 8B 90 B8 00 00 00 E8 ?? ?? ?? ?? 48 8B 45 88 48 8B CE 48 8B 90 B8 00 00 00 E8";
constexpr size_t kResidencyCall = 14;
// The engine's TransitionComputeState (raytracing, inlined on the context, exe+0x25219DE): bNeedSetPSO (+0x3228) and the
// compute root signature's bNeedSetRootSignature (+0xA58), then the command list (+0xA8).
constexpr char kDirtyCompute[] = "C6 87 28 32 00 00 01 C6 87 58 0A 00 00 01 C6 87 A0 34 00 00 02 48 83 BF A8 00 00 00 00";
constexpr size_t kCtxNeedSetPso = 0x3228, kCtxNeedComputeRootSig = 0xA58, kCtxCommandList = 0xA8;
constexpr size_t kListD3D = 0x28, kListNumCommands = 0x170;   // FD3D12CommandList: ID3D12GraphicsCommandList*, NumCommands
constexpr size_t kContextFromIrhi = 0xE8, kIrhiGpuMask = 0x118;
// FD3D12DynamicRHI vtable: RHIGetDevice(index) (+0x560) and RHIGetGraphicsCommandList(index) (+0x570), which show the
// device -> default context (+0xAC0) -> command list (+0xA8) -> ID3D12GraphicsCommandList (+0x28) chain used above.
constexpr char kRhiGetDevice[] = "48 8B 81 C8 00 00 00 44 8B C2 48 8B 08 4A 8B 8C C1 F8 07 00 00 E9";
constexpr char kRhiGetCmdList[] =
    "40 53 48 83 EC 20 48 8B 81 C8 00 00 00 44 8B C2 48 8B 08 4A 8B 84 C1 F8 07 00 00 48 8B 98 C0 0A 00 00 48 83 BB A8 00 00 "
    "00 00 75 09 48 8B 03 48 8B CB FF 50 08 48 8B 8B A8 00 00 00 48 8B 41 28 FF 81 70 01 00 00";
constexpr size_t kRhiSlotGetDevice = 0x560, kRhiSlotGetCmdList = 0x570;
// Altar's screenshot handler (exe+0x4924EF0): unbind, ImageResize (same size, bForceOpaque), CreateTexture2DFromImage
// (+0xA4), the brush texture's release, SetBrushFromTexture (vtable +0x328 on widget +0x420).
constexpr char kHandler[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 70 49 8B E9 41 8B F8 8B F2 48 8B D9 E8 ?? ?? "
    "?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 93 00 05 00 00 48 85 D2 74 0C 48 8D 88 70 03 00 00 E8 ?? ?? ?? ?? 45 33 F6 C6 44 24 "
    "38 01 48 8D 44 24 40 44 88 74 24 30 48 89 44 24 28 44 8B CE 4C 8B C5 89 7C 24 20 8B D7 4C 89 74 24 40 8B CE 4C 89 74 24 "
    "48 E8 ?? ?? ?? ?? 48 8B 44 24 40 48 8D 4C 24 50 48 89 44 24 60 89 74 24 50 89 7C 24 54 C7 44 24 58 01 00 00 00 66 C7 44 "
    "24 5C 01 02 E8 ?? ?? ?? ?? 48 8B 8B 20 04 00 00 48 8B F8 E8";
constexpr size_t kHandlerCreateTexCall = 0xA4;
// UTexture::SetResource (exe+0x37B3BF0): PrivateResource at +0x110 (the render thread's copy at +0x118).
constexpr char kSetResource[] = "48 89 5C 24 10 57 48 83 EC 50 48 8B DA 48 89 91 10 01 00 00 48 8B F9 48 89 4C 24 20 48 89 54 24 28";
// Altar's screenshot service, the engine delegate's listener (exe+0x48C2AF0): unbind (+0x3A8, the engine delegate lea at +0x31),
// W/H (+0x38C/+0x390), the picture copied into its cache (+0x398, TArray copy exe+0xF9DEB0 at +0x53), then a tail jump to the
// broadcast of its own handlers' delegate (+0x370, exe+0x31F5A20, at +0x85).
constexpr char kService[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 8B EA 4D 8B F1 48 8B 91 A8 03 00 00 41 8B F0 "
    "48 8B F9 48 85 D2 74 0C 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 49 8B D6 89 AF 8C 03 00 00 48 8D 8F 98 03 00 00 89 B7 90 03 00 "
    "00 E8 ?? ?? ?? ?? 48 8D 8F 70 03 00 00 44 8B C6 4C 8D 8F 98 03 00 00 8B D5 48 8B 5C 24 30 48 8B 6C 24 38 48 8B 74 24 40 48 "
    "8B 7C 24 48 48 83 C4 20 41 5E E9 ?? ?? ?? ??";
constexpr size_t kServiceDelegateLea = 0x31, kServiceCopy = 0x53, kServiceBroadcast = 0x85;
constexpr size_t kSvcHandlers = 0x370, kSvcCaptured = 0x388, kSvcW = 0x38C, kSvcH = 0x390, kSvcPicture = 0x398, kSvcHandle = 0x3A8;
// A UObject method delegate's ExecuteIfSafe with a this-adjustment (exe+0x48B7860): object = Get(+0x18) + [+0x28], fn [+0x20].
constexpr char kExecAdj[] =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B D9 49 8B F9 48 83 C1 18 41 8B F0 8B EA E8 ?? ?? ?? ?? 48 "
    "85 C0 74 2E 48 8D 54 24 30 48 89 44 24 30 48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 8B C8 4C 8B CF 48 63 43 28 44 8B C6 48 03 C8 8B "
    "D5 48 8B 43 20 FF D0 B0";
// Leftover save-thumbnail bindings on the service (worker shot130q 2026-11-08, findings "shot130q"; a game bug, also
// without the mod). A save made while a menu's screenshot service holds a capture (UVAltarSaveSystem's save function
// exe+0x48CBAE0, its service branch: the fast travel's autosave, a save from the System page) binds the save's thumbnail
// callback (exe+0x48AF8A0, a raw delegate whose object is the save system's holder +0x180) on the service's handlers'
// delegate (+0x370) and stores that binding's handle in holder+0x18; the engine branch binds it on the engine's
// ScreenshotCapturedDelegate instead. The callback always removes holder+0x18 from the ENGINE delegate (its own lea at
// +0x55), so a service binding is never removed: it stays for the session, one more per such save, and the service calls
// it with every later menu screenshot (and every cached re-request): holder W/H, the picture copied into the holder under
// the save lock, Remove(engine delegate, holder+0x18) (finds nothing), GScreenshotResolutionX/Y = 0, the high-res
// screenshot config's multiplier = 1.0, the save's thumbnail event triggered unless already done (bit 26 of its state).
// While the save system is idle (+0x154 == 4: the save's tasks are done and the next save empties the holder before it
// binds) and those writes change nothing else (event done, resolution 0/0, multiplier 1.0, the handle not on the engine
// delegate), such a call only rewrites the idle holder. GpuMenuShot then holds these leftovers out of its placeholder call
// (an empty picture would empty the holder) and they get the real picture with the service's delivery on the game thread
// (the broadcast of the service's handlers' delegate, as the game's cached path calls them): the holder ends as in the
// game, a few frames later (a delivery that drops leaves older bytes in it, which nothing reads before the next save
// resets it): equivalent, not identical. At the delivery each is checked again (rev130s SF-1): one whose call is no longer
// a no-op there (a save began, a high-resolution screenshot was set up) is skipped, as the game called it before that.
// Not idle, or any of those writes not a no-op at the screenshot: the game's way.
// The callback, through its event check: holder W/H, the copy under the lock, Remove(engine delegate, holder+0x18) (lea
// +0x55, call +0x5C), GScreenshotResolutionX/Y = 0 (+0x63, +0x69), GetHighResScreenshotConfig (+0x6F) ->
// ResolutionMultiplier (+0x20) = 1.0, the save system through the holder's weak pointer (+0x20, Get at +0x7F), its event
// (+0x168) -> state (+0x58, atomic read), bit 26 set = done.
constexpr char kLeftoverCallback[] =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 89 54 24 30 48 8B F9 44 89 44 24 34 49 8B D9 48 8B 44 24 30 48 89 01 "
    "48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8D 4F 08 48 8B D3 E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? "
    "48 8B 57 18 48 85 D2 74 0C 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 F6 89 35 ?? ?? ?? ?? 89 35 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 8D 4F 20 C7 40 20 00 00 80 3F E8 ?? ?? ?? ?? 48 8B 98 68 01 00 00 48 8B 0B 48 85 C9 74 ?? 33 C0 F0 48 0F B1 71 58 "
    "48 C1 E8 1A A8 01 75";
constexpr size_t kLeftoverAnchor = 0x78;  // the store of 1.0f into the multiplier (rarer first byte)
constexpr size_t kLcDelegateLea = 0x55, kLcResX = 0x63, kLcResY = 0x69, kLcGetConfig = 0x6F, kLcWeakGet = 0x7F;
constexpr size_t kHolderHandle = 0x18, kHolderSaveSystem = 0x20, kConfigMultiplier = 0x20, kSaveEvent = 0x168, kEventState = 0x58;
constexpr uint64_t kEventDoneBit = uint64_t{1} << 26;
// The save function's start (exe+0x48CBAE0): mov eax,[rcx+154h] (the save state) ... cmp eax,4 / je (4 = idle; else it
// returns at once).
constexpr char kSaveFunction[] =
    "40 55 56 57 41 57 48 8D AC 24 E8 FE FF FF 48 81 EC 18 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 F8 00 00 00 8B 81 "
    "54 01 00 00 49 8B F0 4C 8B FA 48 8B F9 90 83 F8 04 74";
constexpr size_t kSaveFunctionAnchor = 0x27;  // the state read's ModRM byte (rarer first byte)
constexpr size_t kSaveState = 0x154;
constexpr int32_t kSaveIdle = 4;
constexpr size_t kSaveFunctionRefs = 0x500;  // the save function's direct call and AddRaw leas of the callback lie within
constexpr size_t kInstanceHandle = 0x10, kInstanceObject = 0x18, kInstanceFn = 0x20;  // a raw delegate instance
constexpr size_t kTexResourceRT = 0x118, kTexResourceGT = 0x110, kResourceTextureRhi = 0x10;
// FRHITextureDesc (UE 5.3): Flags +0x00, Extent +0x24, NumMips +0x30, NumSamples +0x31, Dimension +0x32, Format +0x33.
constexpr size_t kDescExtent = 0x24, kDescMips = 0x30, kDescSamples = 0x31, kDescDimension = 0x32, kDescFormat = 0x33;
constexpr uint8_t kPfA2B10G10R10 = 18, kPfB8G8R8A8 = 2;
constexpr size_t kReadbackSize = 0x58, kReadbackStaging = 0x18;
constexpr D3D12_RESOURCE_STATES kStateTbd = static_cast<D3D12_RESOURCE_STATES>(0x7FFFFFFF);
constexpr uint32_t kAllSubresources = 0xFFFFFFFFu;

struct ScriptArray { void* data; int32_t num, max; };
struct IntVec { int32_t x, y, z; };

using CopyTextureFn = void (*)(void* ctx, void* src, void* dst, const void* info);
using TransitionFn = bool (*)(void* common, void* resource, uint32_t before, uint32_t after, uint32_t subresource);
using AddBarrierFn = void (*)(void* common, void* resource, uint32_t before, uint32_t after, uint32_t subresource, void* state);
using CommonFn = void (*)(void* common);
using ResidencyFn = void (*)(void* common, void* resource);
using CreateTexFn = void* (*)(const void* imageView);
using HandlerFn = void (*)(void* obj, int32_t w, int32_t h, const ScriptArray* pixels);
using CtorFn = void* (*)(void* self, uint64_t name);
using EnqueueCopyFn = void (*)(void* self, void* cmdList, void* texture, const IntVec* pos, uint32_t slice, const IntVec* size);
using CmdFn = void (*)(void* cmdList, int arg);
using CmdFn1 = void (*)(void* cmdList);
using RtTexFn = void** (*)(void* renderTarget);
using DescFn = const uint8_t* (*)(void* texture);
using BaseFn = uint8_t* (*)(void* texture);
using GetDeviceFn = ID3D12Device* (*)(void* rhi, uint32_t index);

template <class T> T VSlot(void* object, size_t offset) { return reinterpret_cast<T>((*reinterpret_cast<void***>(object))[offset / 8]); }

struct Sites {
    uint8_t* copyTexture = nullptr;
    void** copySlot = nullptr;     // the context vtable's RHICopyTexture slot
    uint8_t* transition = nullptr, *addBarrier = nullptr, *flush = nullptr, *residency = nullptr;
    uint8_t* handler = nullptr, *createTexCall = nullptr, *createTex = nullptr;
    uint8_t* service = nullptr, *serviceDelegate = nullptr, *arrayCopy = nullptr, *broadcast = nullptr, *execAdj = nullptr;
    // Leftover save-thumbnail bindings (optional: leftoverWhy != nullptr = not handled, those shots stay the game's).
    const char* leftoverWhy = "not looked for";
    uint8_t* saveCallback = nullptr, *saveFunction = nullptr;
    const int32_t* resX = nullptr, *resY = nullptr;
    uint8_t* getConfig = nullptr, *weakGet = nullptr;
};
Sites g_sites;
bool g_installed = false;
std::atomic<int> g_mode{0};
std::atomic<bool> g_hdrBackground{false};
CopyTextureFn g_origCopy = nullptr;
SaveThumbRhi g_st{};
constexpr uint64_t kCompareShots = 4;  // value 3: how many menu screenshots are compared before it works as 1

// ---- per-capture jobs (game thread -> render thread -> RHI thread) ----
constexpr int kJobs = 4;
struct Job {
    std::atomic<int> state{0};   // 0 free, 1 enqueued (GT), 2 armed (RT), 3 recorded (RHI), 4 failed
    void* textures[4] = {};      // the UTexture2Ds the handlers made (none in mode 2: compute only)
    int textureCount = 0;
    int32_t w = 0, h = 0, x0 = 0, y0 = 0;
    bool hdr = false;
    int mode = 0;
    bool post = false;           // HdrMenuBackground's table applied after the filter (as the handler would)
    uint8_t postTable[256] = {};
    void* bbTex = nullptr;       // FRHITexture* (render thread)
    void* outTex[4] = {};        // the textures' FRHITexture* (render thread)
    uint64_t tGt = 0, tRt = 0, tRhi = 0;
    double rhiMs = 0;
    // Snapshot of the buffers at Prepare (no resize while a job is in flight or the GPU may still use them).
    ID3D12Resource* inBuf = nullptr, *outBuf = nullptr, *readback = nullptr;
    uint8_t* readbackMap = nullptr;
    uint32_t pitchIn = 0, pitchOut = 0;
    // The service's handlers (their delegates' weak object + adjustment): re-served with the real picture if this job fails.
    int handlerCount = 0;
    uint64_t handlerWeak[4] = {};
    int32_t handlerAdj[4] = {};
    int leftovers = 0;           // leftover save-thumbnail bindings held out of the placeholder call (shot130q)
    uint8_t* leftoverInst[GpuMenuShotHeld::kMax] = {};  // their delegate instances (re-checked at the delivery)
};
Job g_jobs[kJobs];
ScriptArray g_markers[kJobs];    // the ReadSurfaceCommand's OutData for job k (never written: our command replaces the read)
std::atomic<uint32_t> g_nextJob{0};
// RHI-thread FIFO of armed jobs (pushed on the render thread before the trigger copy is enqueued)
std::atomic<uint32_t> g_fifoHead{0}, g_fifoTail{0};
Job* g_fifo[16];
std::atomic<void*> g_triggerStaging{nullptr};
std::atomic<void*> g_trigger{nullptr};  // our FRHIGPUTextureReadback (1x1 copies of the back buffer), made on the render thread
constexpr size_t kReadbackFence = 0x08, kFencePoll = 0x18, kFencePending = 0x10;

// ---- D3D12 objects (created on the game thread at first use) ----
std::mutex g_resLock;
ID3D12Device* g_device = nullptr;
ID3D12RootSignature* g_rootSig = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12Resource* g_lut = nullptr;       // upload heap, one block per job: B, G, R tables (1024 uints each) + the byte table (256)
uint32_t* g_lutMap = nullptr;          // persistently mapped
constexpr uint32_t kLutUints = 3072 + 256, kLutBytes = kLutUints * 4;
uint32_t g_tabSdr[3072], g_tabHdr[3072];
ID3D12Resource* g_inBuf = nullptr;     // default heap, COMMON between uses
ID3D12Resource* g_outBuf = nullptr;    // default heap, UAV allowed, COMMON between uses
ID3D12Resource* g_readback = nullptr;  // readback heap (modes 2/3)
uint8_t* g_readbackMap = nullptr;
int32_t g_resW = 0, g_resH = 0;
uint32_t g_pitchIn = 0, g_pitchOut = 0;   // bytes
std::atomic<uint64_t> g_lastUseTicks{0};
bool g_hdrLut = false;
std::atomic<int32_t> g_bbW{0}, g_bbH{0};  // the back buffer last seen on the render thread (format R10G10B10A2)
std::atomic<int> g_bbFormat{-1};
std::atomic<bool> g_probeWanted{false};

// stats
std::atomic<uint64_t> s_taken{0}, s_recorded{0}, s_declined{0}, s_failed{0}, s_verifyOk{0}, s_verifyBad{0}, s_skipResize{0};
std::atomic<uint64_t> s_gtTicks{0};
std::atomic<uint64_t> s_leftoverShots{0};
std::atomic<uint64_t> s_compares{0};  // value 3: menu screenshots compared with the game's picture so far
thread_local bool t_active = false;
thread_local Job* t_job = nullptr;

// The service's picture, delivered on the game thread once the asynchronous read-back has it. word = generation << 8 | state
// (0 none, 1 armed: waiting for pixels, 2 pixels ready); the helper's hand-over and the game thread's drop use compare-exchange,
// and the pixel buffer has exactly one owner (exchange).
struct ServicePending {
    std::atomic<uint64_t> word{0};
    std::atomic<uint32_t*> pixels{nullptr};  // VirtualAlloc'd
    uint64_t weak = 0;           // FWeakObjectPtr of the delegate (object index + serial)
    int32_t adj = 0;
    void* weakGet = nullptr;
    int32_t w = 0, h = 0;
    uint64_t tArm = 0;
    uint64_t handle = 0;         // the service's engine-delegate handle of this capture (a new capture binds a new one)
    uint8_t* obj = nullptr;
    int job = 0;                 // 1-based
};
ServicePending g_svc;
uint32_t g_svcGen = 0;           // game thread
int SvcState() { return static_cast<int>(g_svc.word.load(std::memory_order_acquire) & 0xFF); }
std::atomic<uint64_t> s_svcDelivered{0}, s_svcDropped{0}, s_svcLate{0}, s_gtFrames{0};
std::atomic<uint64_t> g_lastGtFrameTicks{0};

uint64_t Ticks() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return static_cast<uint64_t>(t.QuadPart); }
double Ms(uint64_t ticks)
{
    static const double perMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1000.0; }();
    return static_cast<double>(ticks) / perMs;
}
uint32_t Align256(uint32_t v) { return (v + 255u) & ~255u; }

void SwitchOff(const char* why)
{
    if (g_mode.exchange(0) != 0) Log("[gpushot] %s: GpuMenuShot switched off for this session (menu screenshots the game's way)", why);
}

// ---- RHI thread ----
// RetrieveTexture as RHICopyTexture does it: the presentable back-buffer reference resolved, then the linked object for
// the context's GPU. Returns the FD3D12Resource*.
uint8_t* D3D12ResourceOf(void* texture, uint32_t gpu)
{
    uint8_t* t = static_cast<uint8_t*>(texture);
    if (!t) return nullptr;
    const uint8_t* desc = VSlot<DescFn>(t, 0x08)(t);
    if ((*reinterpret_cast<const uint32_t*>(desc) >> 18) & 1) {
        uint8_t* const vp = *reinterpret_cast<uint8_t**>(t + 0x248);
        if (!vp) return nullptr;
        if (t[0x250] != 0 && vp[0x2A] != vp[0x118]) t = *reinterpret_cast<uint8_t**>(vp + 0x110);
        else t = *reinterpret_cast<uint8_t**>(vp + 0x90);
        if (!t) return nullptr;
    }
    uint8_t* const base = VSlot<BaseFn>(t, 0x30)(t);
    if (!base) return nullptr;
    uint8_t* const linked = *reinterpret_cast<uint8_t**>(base + 0x130 + 8ull * gpu);
    return linked ? *reinterpret_cast<uint8_t**>(linked + 0xB8) : nullptr;
}

ID3D12Resource* NativeOf(uint8_t* d3d12Resource) { return d3d12Resource ? *reinterpret_cast<ID3D12Resource**>(d3d12Resource + 0x20) : nullptr; }

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

bool IsR10(DXGI_FORMAT f) { return f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_TYPELESS; }
bool IsBgra8(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// After the trigger's own copy (the engine's barriers done and its batch flushed): our commands on the same list.
// declineOnly (G-S4): set when the reason is a size that changed since the game thread (a resize): that shot is left to the
// game's picture, the key stays on.
const char* Record(uint8_t* irhi, Job* j, bool* declineOnly)
{
    uint8_t* const common = irhi - kContextFromIrhi;
    const uint32_t mask = *reinterpret_cast<uint32_t*>(irhi + kIrhiGpuMask);
    unsigned long gpu = 0;
    if (mask && !_BitScanForward(&gpu, mask)) gpu = 0;
    uint8_t* const list = *reinterpret_cast<uint8_t**>(common + kCtxCommandList);
    if (!list) return "no open command list";
    auto* const cl = *reinterpret_cast<ID3D12GraphicsCommandList**>(list + kListD3D);
    if (!cl) return "no D3D12 command list";
    ID3D12Resource* const bb = NativeOf(D3D12ResourceOf(j->bbTex, gpu));
    if (!bb) return "the back buffer's D3D12 resource not found";
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    if (bd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !IsR10(bd.Format) || bd.SampleDesc.Count != 1)
        return "the back buffer is not as expected";
    if (bd.Width < static_cast<UINT64>(j->x0 + j->w) || bd.Height < static_cast<UINT>(j->y0 + j->h)) {
        *declineOnly = true;
        return "the back buffer is smaller than the shot (resized)";
    }
    uint8_t* outResList[4] = {};
    ID3D12Resource* outList[4] = {};
    {
        for (int k = 0; k < j->textureCount; ++k) {
            outResList[k] = D3D12ResourceOf(j->outTex[k], gpu);
            outList[k] = NativeOf(outResList[k]);
            if (!outList[k]) return "the texture's D3D12 resource not found";
            const D3D12_RESOURCE_DESC od = outList[k]->GetDesc();
            if (od.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !IsBgra8(od.Format) || od.MipLevels != 1 ||
                od.DepthOrArraySize != 1 || od.SampleDesc.Count != 1)
                return "the texture is not as expected";
            if (od.Width != static_cast<UINT64>(j->w) || od.Height != static_cast<UINT>(j->h)) {
                *declineOnly = true;
                return "the texture's size differs from the shot's";
            }
            if ((outResList[k][0xDF] & 1) == 0 &&
                *reinterpret_cast<uint32_t*>(outResList[k] + 0xC0) !=
                    static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
                return "the texture's default state is not shader-read";
        }
    }
    ID3D12Resource* const inBuf = j->inBuf;
    ID3D12Resource* const outBuf = j->outBuf;
    ID3D12Resource* const readback = j->readback;
    const uint32_t pitchIn = j->pitchIn, pitchOut = j->pitchOut;
    if (!inBuf || !outBuf) return "buffers not ready";
    int commands = 0;
    // 1. The back buffer's pixels (in COPY_SOURCE since the trigger's copy) into our buffer.
    {
        D3D12_RESOURCE_BARRIER b[2] = {Transition(inBuf, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
                                       Transition(outBuf, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        cl->ResourceBarrier(2, b);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = inBuf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R10G10B10A2_UNORM, static_cast<UINT>(j->w), static_cast<UINT>(j->h), 1, pitchIn};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = bb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        const D3D12_BOX box{static_cast<UINT>(j->x0), static_cast<UINT>(j->y0), 0, static_cast<UINT>(j->x0 + j->w),
                            static_cast<UINT>(j->y0 + j->h), 1};
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        D3D12_RESOURCE_BARRIER b2 = Transition(inBuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl->ResourceBarrier(1, &b2);
        commands += 3;
    }
    // 2. The compute pass.
    {
        const uint32_t consts[4] = {static_cast<uint32_t>(j->w), static_cast<uint32_t>(j->h), pitchIn / 4, pitchOut / 4};
        cl->SetComputeRootSignature(g_rootSig);
        cl->SetPipelineState(g_pso);
        cl->SetComputeRoot32BitConstants(0, 4, consts, 0);
        cl->SetComputeRootShaderResourceView(1, g_lut->GetGPUVirtualAddress() + static_cast<UINT64>(j - g_jobs) * kLutBytes);
        cl->SetComputeRootShaderResourceView(2, inBuf->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(3, outBuf->GetGPUVirtualAddress());
        cl->Dispatch((static_cast<UINT>(j->w) + 7) / 8, (static_cast<UINT>(j->h) + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER b[2] = {Transition(inBuf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                                       Transition(outBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        cl->ResourceBarrier(2, b);
        commands += 8;
    }
    D3D12_TEXTURE_COPY_LOCATION outFoot{};
    outFoot.pResource = outBuf;
    outFoot.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    outFoot.PlacedFootprint.Offset = 0;
    outFoot.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, static_cast<UINT>(j->w), static_cast<UINT>(j->h), 1, pitchOut};
    {
        // 3. Into each texture, with the engine's own barrier handling for it (as RHICopyTexture's destination).
        for (int k = 0; k < j->textureCount; ++k) {
            uint8_t* const outRes = outResList[k];
            ID3D12Resource* const out = outList[k];
            const bool tracked = (outRes[0xDF] & 1) != 0;
            const uint32_t def = *reinterpret_cast<uint32_t*>(outRes + 0xC0);
            if (tracked)
                reinterpret_cast<TransitionFn>(g_sites.transition)(common, outRes, kStateTbd, D3D12_RESOURCE_STATE_COPY_DEST, kAllSubresources);
            else
                reinterpret_cast<AddBarrierFn>(g_sites.addBarrier)(common, outRes, def, D3D12_RESOURCE_STATE_COPY_DEST, kAllSubresources, nullptr);
            reinterpret_cast<CommonFn>(g_sites.flush)(common);
            D3D12_TEXTURE_COPY_LOCATION texLoc{};
            texLoc.pResource = out;
            texLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            texLoc.SubresourceIndex = 0;
            outFoot.PlacedFootprint.Footprint.Format =
                out->GetDesc().Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
            cl->CopyTextureRegion(&texLoc, 0, 0, 0, &outFoot, nullptr);
            commands += 1;
            if (j->mode == 3 && k == 0 && readback) {
                // Verify: the (first) texture itself read back.
                if (tracked)
                    reinterpret_cast<TransitionFn>(g_sites.transition)(common, outRes, kStateTbd, D3D12_RESOURCE_STATE_COPY_SOURCE, kAllSubresources);
                else
                    reinterpret_cast<AddBarrierFn>(g_sites.addBarrier)(common, outRes, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                         D3D12_RESOURCE_STATE_COPY_SOURCE, kAllSubresources, nullptr);
                reinterpret_cast<CommonFn>(g_sites.flush)(common);
                D3D12_TEXTURE_COPY_LOCATION rb{};
                rb.pResource = readback;
                rb.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                rb.PlacedFootprint = outFoot.PlacedFootprint;
                cl->CopyTextureRegion(&rb, 0, 0, 0, &texLoc, nullptr);
                commands += 1;
                if (!tracked)
                    reinterpret_cast<AddBarrierFn>(g_sites.addBarrier)(common, outRes, D3D12_RESOURCE_STATE_COPY_SOURCE, def, kAllSubresources, nullptr);
            } else if (!tracked) {
                reinterpret_cast<AddBarrierFn>(g_sites.addBarrier)(common, outRes, D3D12_RESOURCE_STATE_COPY_DEST, def, kAllSubresources, nullptr);
            }
            reinterpret_cast<ResidencyFn>(g_sites.residency)(common, outRes);
        }
    }
    {
        D3D12_RESOURCE_BARRIER b = Transition(outBuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        cl->ResourceBarrier(1, &b);
        commands += 1;
    }
    // The engine's state cache: the compute PSO and root signature on this list are ours now (as TransitionComputeState).
    common[kCtxNeedSetPso] = 1;
    common[kCtxNeedComputeRootSig] = 1;
    *reinterpret_cast<int32_t*>(list + kListNumCommands) += commands;
    return nullptr;
}

void CopyTextureHook(void* ctx, void* src, void* dst, const void* info)
{
    if (dst != g_triggerStaging.load(std::memory_order_acquire) || g_fifoHead.load(std::memory_order_acquire) == g_fifoTail.load(std::memory_order_relaxed)) {
        g_origCopy(ctx, src, dst, info);
        return;
    }
    const uint32_t tail = g_fifoTail.load(std::memory_order_relaxed);
    Job* const j = g_fifo[tail % 16];
    g_fifoTail.store(tail + 1, std::memory_order_release);
    g_origCopy(ctx, src, dst, info);  // the trigger: the engine's barriers for the back buffer, its batch flushed
    const uint64_t t0 = Ticks();
    bool declineOnly = false;
    const bool forced = false;
    const char* why =
                      (j->bbTex == src) ? Record(static_cast<uint8_t*>(ctx), j, &declineOnly)
                                        : "the trigger copy's source is not the job's back buffer";
    j->rhiMs = Ms(Ticks() - t0);
    j->tRhi = Ticks();
    g_lastUseTicks.store(j->tRhi, std::memory_order_relaxed);
    if (why) {
        Log("[gpushot] RHI thread: %s: this menu screenshot is not drawn by the GPU pass (its handlers get the read-back picture)", why);
        s_failed.fetch_add(1, std::memory_order_relaxed);
        j->state.store(4, std::memory_order_release);
        if (!declineOnly && !forced) SwitchOff("unexpected state on the RHI thread");
        return;
    }
    s_recorded.fetch_add(1, std::memory_order_relaxed);
    j->state.store(3, std::memory_order_release);
}

// ---- render thread ----
// 0 = armed; 1 = declined, expected (the back buffer changed format or size: the cache is refreshed); 2 = unexpected.
int RenderThreadJob(Job* j, void** ctx, void* cmdList)
{
    void* const rt = ctx[0];
    void** const ref = VSlot<RtTexFn>(rt, 0x10)(rt);
    void* const bbTex = ref ? *ref : nullptr;
    if (!bbTex) return 2;
    const uint8_t* const desc = VSlot<DescFn>(bbTex, 0x08)(bbTex);
    const int32_t ex = *reinterpret_cast<const int32_t*>(desc + kDescExtent), ey = *reinterpret_cast<const int32_t*>(desc + kDescExtent + 4);
    g_bbFormat.store(desc[kDescFormat], std::memory_order_relaxed);
    g_bbW.store(ex, std::memory_order_relaxed);
    g_bbH.store(ey, std::memory_order_relaxed);
    if (j->mode < 0) return 0;  // a probe: the back buffer recorded
    const int32_t* const rect = reinterpret_cast<const int32_t*>(ctx + 2);
    if (desc[kDescSamples] != 1 || desc[kDescDimension] != 0 || desc[kDescFormat] != kPfA2B10G10R10) return 1;
    if (rect[0] != j->x0 || rect[1] != j->y0 || rect[2] - rect[0] != j->w || rect[3] - rect[1] != j->h || rect[2] > ex || rect[3] > ey) return 1;
    j->bbTex = bbTex;
    {
        if (j->textureCount <= 0) return 2;
        for (int k = 0; k < j->textureCount; ++k) {
            auto* const tex = static_cast<uint8_t*>(j->textures[k]);
            if (!tex) return 2;
            uint8_t* const res = *reinterpret_cast<uint8_t**>(tex + kTexResourceRT);
            if (!res || res != *reinterpret_cast<uint8_t**>(tex + kTexResourceGT)) return 2;
            void* const outTex = *reinterpret_cast<void**>(res + kResourceTextureRhi);
            if (!outTex) return 2;
            const uint8_t* const od = VSlot<DescFn>(outTex, 0x08)(outTex);
            if (*reinterpret_cast<const int32_t*>(od + kDescExtent) != j->w || *reinterpret_cast<const int32_t*>(od + kDescExtent + 4) != j->h ||
                od[kDescFormat] != kPfB8G8R8A8 || od[kDescMips] != 1 || od[kDescSamples] != 1 || od[kDescDimension] != 0)
                return 2;
            j->outTex[k] = outTex;
        }
    }
    const IntVec pos{j->x0, j->y0, 0}, size{1, 1, 1};
    void* trigger = g_trigger.load(std::memory_order_acquire);
    if (!trigger) {
        void* const rb = _aligned_malloc(kReadbackSize, 16);
        if (!rb) return 2;
        memset(rb, 0, kReadbackSize);
        reinterpret_cast<CtorFn>(g_st.readbackCtor)(rb, 0);
        if (*reinterpret_cast<void**>(rb) != g_st.readbackVtable) { _aligned_free(rb); return 2; }
        // A first 1x1 copy, not armed, makes the staging texture, so every armed copy has a known destination.
        reinterpret_cast<EnqueueCopyFn>((*reinterpret_cast<void***>(rb))[1])(rb, cmdList, bbTex, &pos, 0, &size);
        void* const staging = *reinterpret_cast<void**>(static_cast<uint8_t*>(rb) + kReadbackStaging);
        if (!staging) return 2;
        const uint8_t* const sd = VSlot<DescFn>(staging, 0x08)(staging);
        Log("[gpushot] trigger read-back's staging texture: %dx%d (format %d)", *reinterpret_cast<const int32_t*>(sd + kDescExtent),
            *reinterpret_cast<const int32_t*>(sd + kDescExtent + 4), sd[kDescFormat]);
        g_triggerStaging.store(staging, std::memory_order_release);
        g_trigger.store(rb, std::memory_order_release);
        trigger = rb;
    }
    // Arm (before the copy is enqueued: with the RHI thread bypassed the copy runs inside EnqueueCopy).
    const uint32_t head = g_fifoHead.load(std::memory_order_relaxed);
    if (head - g_fifoTail.load(std::memory_order_acquire) >= 16) return 2;
    g_fifo[head % 16] = j;
    j->state.store(2, std::memory_order_release);
    g_fifoHead.store(head + 1, std::memory_order_release);
    reinterpret_cast<EnqueueCopyFn>((*reinterpret_cast<void***>(trigger))[1])(trigger, cmdList, bbTex, &pos, 0, &size);
    if (*reinterpret_cast<void**>(static_cast<uint8_t*>(trigger) + kReadbackStaging) != g_triggerStaging.load(std::memory_order_relaxed))
        Log("[gpushot] render thread: the trigger's staging texture changed (unexpected)");
    reinterpret_cast<CmdFn>(g_st.executeAndReset)(cmdList, 0);  // ImmediateFlush(DispatchToRHIThread), as save_thumb does
    reinterpret_cast<CmdFn1>(g_st.initContexts)(cmdList);
    return 0;
}

// ---- game thread ----
bool BuildTables()
{
    uint32_t* const t = g_tabSdr;
    // SDR: the read-back's requantize (fix 40's conversion = UE's), one channel at a time.
    for (uint32_t v = 0; v < 1024; ++v) {
        const uint32_t px[3] = {v << 20, v << 10, v};
        uint32_t c[3] = {};
        ConvertR10G10B10A2ToColor(3, 1, reinterpret_cast<const uint8_t*>(px), 12, c);
        t[v] = c[0] & 0xFF;
        t[1024 + v] = (c[1] >> 8) & 0xFF;
        t[2048 + v] = (c[2] >> 16) & 0xFF;
    }
    // Each byte depends on its own field only (fix 40's self-test proves it; checked again on mixed pixels here).
    uint32_t seed = 12345;
    for (int i = 0; i < 4096; ++i) {
        seed = seed * 1664525u + 1013904223u;
        uint32_t c = 0;
        ConvertR10G10B10A2ToColor(1, 1, reinterpret_cast<const uint8_t*>(&seed), 4, &c);
        const uint32_t e = t[(seed >> 20) & 1023] | t[1024 + ((seed >> 10) & 1023)] << 8 | t[2048 + (seed & 1023)] << 16;
        if ((c & 0xFFFFFF) != e) return false;
    }
    // HDR: SaveThumbnailAsync's tables (the game's ToFColorSRGB of v / 1023), if its HDR route is installed.
    g_hdrLut = g_st.hdrRoute;
    if (g_hdrLut) {
        for (uint32_t v = 0; v < 1024; ++v) {
            const uint32_t px[3] = {v << 20, v << 10, v};
            uint32_t c[3] = {};
            SaveThumbTestHdrConvert(px, 3, c);
            g_tabHdr[v] = c[0] & 0xFF;
            g_tabHdr[1024 + v] = (c[1] >> 8) & 0xFF;
            g_tabHdr[2048 + v] = (c[2] >> 16) & 0xFF;
        }
    }
    void* p = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(g_lut->Map(0, &none, &p)) || !p) return false;
    g_lutMap = static_cast<uint32_t*>(p);
    return true;
}

ID3D12Resource* MakeBuffer(D3D12_HEAP_TYPE heap, uint64_t bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    r->SetName(name);
    return r;
}

const char* EnsureResources(int32_t w, int32_t h, int mode)
{
    std::lock_guard<std::mutex> lock(g_resLock);
    if (!g_device) {
        void* const rhi = g_st.rhiGlobal ? *g_st.rhiGlobal : nullptr;
        if (!rhi) return "no RHI";
        const GameModule& game = Game();
        uint8_t* const getDev = VSlot<uint8_t*>(rhi, kRhiSlotGetDevice);
        uint8_t* const getList = VSlot<uint8_t*>(rhi, kRhiSlotGetCmdList);
        if (!game.text.Contains(getDev) || !MatchesAt(getDev, ParsePattern(kRhiGetDevice)) || !game.text.Contains(getList) ||
            !MatchesAt(getList, ParsePattern(kRhiGetCmdList)))
            return "the D3D12 RHI's vtable is not as expected";
        g_device = reinterpret_cast<GetDeviceFn>(getDev)(rhi, 0);
        if (!g_device) return "no D3D12 device";
    }
    if (!g_rootSig) {
        if (FAILED(g_device->CreateRootSignature(0, kGpuMenuShotCs, sizeof kGpuMenuShotCs, IID_PPV_ARGS(&g_rootSig)))) return "root signature";
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rootSig;
        pd.CS = {kGpuMenuShotCs, sizeof kGpuMenuShotCs};
        if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)))) return "pipeline state";
        g_lut = MakeBuffer(D3D12_HEAP_TYPE_UPLOAD, static_cast<uint64_t>(kJobs) * kLutBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                           L"OPF GpuMenuShot tables");
        if (!g_lut) return "table buffer";
        if (!BuildTables()) return "the conversion tables did not check out";
    }
    if (g_resW != w || g_resH != h || !g_inBuf) {
        if (g_inBuf) {
            // The old pair is released only once its last GPU use is long past.
            if (Ms(Ticks() - g_lastUseTicks.load(std::memory_order_relaxed)) < 2000.0) return "buffers of another size still in use";
            g_inBuf->Release();
            g_outBuf->Release();
            g_inBuf = g_outBuf = nullptr;
            if (g_readback) { g_readback->Release(); g_readback = nullptr; g_readbackMap = nullptr; }
        }
        g_pitchIn = Align256(static_cast<uint32_t>(w) * 4);
        g_pitchOut = Align256(static_cast<uint32_t>(w) * 4);
        g_inBuf = MakeBuffer(D3D12_HEAP_TYPE_DEFAULT, static_cast<uint64_t>(g_pitchIn) * h, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON,
                             L"OPF GpuMenuShot in");
        g_outBuf = MakeBuffer(D3D12_HEAP_TYPE_DEFAULT, static_cast<uint64_t>(g_pitchOut) * h, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COMMON, L"OPF GpuMenuShot out");
        if (!g_inBuf || !g_outBuf) {
            if (g_inBuf) g_inBuf->Release();
            if (g_outBuf) g_outBuf->Release();
            g_inBuf = g_outBuf = nullptr;
            return "no video memory for the buffers";
        }
        g_resW = w;
        g_resH = h;
        Log("[gpushot] buffers for %dx%d made (2 x %.1f MB of video memory)", w, h, static_cast<double>(g_pitchIn) * h / 1048576.0);
    }
    if (mode >= 2 && !g_readback) {
        g_readback = MakeBuffer(D3D12_HEAP_TYPE_READBACK, static_cast<uint64_t>(g_pitchOut) * h, D3D12_RESOURCE_FLAG_NONE,
                                D3D12_RESOURCE_STATE_COPY_DEST, L"OPF GpuMenuShot verify");
        void* p = nullptr;
        if (!g_readback || FAILED(g_readback->Map(0, nullptr, &p))) return "no read-back buffer";
        g_readbackMap = static_cast<uint8_t*>(p);
    }
    return nullptr;
}

void* CreateTexHook(const void* imageView)
{
    void* const t = reinterpret_cast<CreateTexFn>(g_sites.createTex)(imageView);
    if (t_active && t_job && t) {
        if (t_job->textureCount < 4) t_job->textures[t_job->textureCount++] = t;
        else t_job->textureCount = 5;  // too many: the job is not drawn (checked by the caller)
    }
    return t;
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

uint8_t* FindUnique(const Section& s, const char* pattern, size_t anchor, int* count)
{
    const Pattern full = ParsePattern(pattern);
    Pattern tail;
    tail.bytes.assign(full.bytes.begin() + static_cast<std::ptrdiff_t>(anchor), full.bytes.end());
    tail.care.assign(full.care.begin() + static_cast<std::ptrdiff_t>(anchor), full.care.end());
    uint8_t* hit = nullptr;
    *count = 0;
    for (uint8_t* h : FindAll(s, tail))
        if (h - anchor >= s.begin && MatchesAt(h - anchor, full)) { hit = h - anchor; ++*count; }
    return *count == 1 ? hit : nullptr;
}

}  // namespace

// The sites, on the running game or on a mapped exe (self-test). nullptr = all found.
const char* GpuMenuShotDiscover(const GameModule& game, GpuMenuShotSites* out)
{
    Sites s;
    int n = 0;
    s.copyTexture = FindUnique(game.text, kCopyTexture, kCopyAnchor, &n);
    if (!s.copyTexture) return "FD3D12CommandContext::RHICopyTexture";
    s.transition = RipTarget(s.copyTexture + kCallTransition, 1, 5);
    s.addBarrier = RipTarget(s.copyTexture + kCallAddBarrier, 1, 5);
    s.flush = RipTarget(s.copyTexture + kCallFlush, 1, 5);
    {
        const Pattern p = ParsePattern(kResidency);
        for (uint8_t* q = s.copyTexture + 0x300; q < s.copyTexture + 0xA00 && !s.residency; ++q)
            if (MatchesAt(q, p)) s.residency = RipTarget(q + kResidencyCall, 1, 5);
    }
    if (!game.text.Contains(s.transition) || !game.text.Contains(s.addBarrier) || !game.text.Contains(s.flush) ||
        !s.residency || !game.text.Contains(s.residency))
        return "RHICopyTexture's barrier / residency calls";
    // The context vtable slot: the one .rdata pointer to RHICopyTexture.
    {
        const uint64_t v = reinterpret_cast<uint64_t>(s.copyTexture);
        int refs = 0;
        for (uint8_t* q = game.rdata.begin; q + 8 <= game.rdata.end; q += 8)
            if (*reinterpret_cast<const uint64_t*>(q) == v) { s.copySlot = reinterpret_cast<void**>(q); ++refs; }
        if (refs != 1) return "the context vtable's RHICopyTexture slot";
    }
    if (!game.text.Contains(s.copyTexture + kCopyPinsAt + 40) || !MatchesAt(s.copyTexture + kCopyPinsAt, ParsePattern(kCopyPins)))
        return "RHICopyTexture's command list offsets (+0xA8, +0x28, +0x170, +0x20)";
    if (FindAll(game.text, ParsePattern(kDirtyCompute)).size() != 1) return "the context's state-cache flags (TransitionComputeState)";
    if (FindAll(game.text, ParsePattern(kSetResource)).size() != 1) return "UTexture::SetResource";
    s.handler = FindUnique(game.text, kHandler, 0x1A, &n);
    if (!s.handler) return "the backdrop's screenshot handler";
    s.createTexCall = s.handler + kHandlerCreateTexCall;
    s.createTex = RipTarget(s.createTexCall, 1, 5);
    if (s.createTexCall[0] != 0xE8 || !game.text.Contains(s.createTex)) return "the handler's CreateTexture2DFromImage call";
    s.service = FindUnique(game.text, kService, 0, &n);
    if (!s.service) return "Altar's screenshot service";
    s.serviceDelegate = RipTarget(s.service + kServiceDelegateLea, 3, 7);
    s.arrayCopy = RipTarget(s.service + kServiceCopy, 1, 5);
    s.broadcast = RipTarget(s.service + kServiceBroadcast, 1, 5);
    if (!game.text.Contains(s.arrayCopy) || !game.text.Contains(s.broadcast) || !game.data.Contains(s.serviceDelegate))
        return "the screenshot service's calls";
    s.execAdj = FindUnique(game.text, kExecAdj, 0, &n);
    if (!s.execAdj) return "the adjusting UObject delegate's ExecuteIfSafe";
    // Optional: the leftover save-thumbnail bindings (shot130q). The callback must remove itself from the engine delegate
    // the service binds to (the leftover's Remove then finds nothing there but a handle it checks at run time).
    s.leftoverWhy = nullptr;
    s.saveCallback = FindUnique(game.text, kLeftoverCallback, kLeftoverAnchor, &n);
    s.saveFunction = FindUnique(game.text, kSaveFunction, kSaveFunctionAnchor, &n);
    if (!s.saveCallback) s.leftoverWhy = "the save's thumbnail callback";
    else if (!s.saveFunction) s.leftoverWhy = "the save function's idle check";
    else if (RipTarget(s.saveCallback + kLcDelegateLea, 3, 7) != s.serviceDelegate) s.leftoverWhy = "the callback's delegate";
    else {
        s.resX = reinterpret_cast<const int32_t*>(RipTarget(s.saveCallback + kLcResX, 2, 6));
        s.resY = reinterpret_cast<const int32_t*>(RipTarget(s.saveCallback + kLcResY, 2, 6));
        s.getConfig = RipTarget(s.saveCallback + kLcGetConfig, 1, 5);
        s.weakGet = RipTarget(s.saveCallback + kLcWeakGet, 1, 5);
        if (!game.data.Contains(reinterpret_cast<const uint8_t*>(s.resX)) || !game.data.Contains(reinterpret_cast<const uint8_t*>(s.resY)) ||
            s.resX == s.resY || !game.text.Contains(s.getConfig) || !game.text.Contains(s.weakGet))
            s.leftoverWhy = "the callback's globals and calls";
    }
    if (!s.leftoverWhy) {
        // The save function must be the one that binds this callback (so its idle state is the callback's save system's):
        // its direct call (the cached-shot branch) and an AddRaw lea r9 of the callback, within its code.
        bool call = false, lea = false;
        for (uint8_t* q = s.saveFunction + 3; q < s.saveFunction + kSaveFunctionRefs && q + 4 <= game.text.end; ++q) {
            if (q + 4 + *reinterpret_cast<const int32_t*>(q) != s.saveCallback) continue;
            if (q[-1] == 0xE8) call = true;
            if (q[-3] == 0x4C && q[-2] == 0x8D && q[-1] == 0x0D) lea = true;
        }
        if (!call || !lea) s.leftoverWhy = "the save function's references to the callback";
    }
    if (s.leftoverWhy) s.saveCallback = s.saveFunction = nullptr;
    if (out) {
        out->saveCallback = s.saveCallback;
        out->saveFunction = s.saveFunction;
        out->leftoverWhy = s.leftoverWhy;
        out->service = s.service;
        out->copyTexture = s.copyTexture;
        out->copySlot = s.copySlot;
        out->handler = s.handler;
        out->createTexCall = s.createTexCall;
        out->createTex = s.createTex;
        out->transition = s.transition;
        out->addBarrier = s.addBarrier;
        out->flush = s.flush;
        out->residency = s.residency;
    }
    g_sites = s;
    return nullptr;
}

bool InstallGpuMenuShot()
{
    if (g_installed) return true;
    const GameModule& game = Game();
    if (!SaveThumbInternals(&g_st)) {
        Log("[gpushot] SaveThumbnailAsync's read-back hooks are not installed: GpuMenuShot unavailable");
        return false;
    }
    uint8_t* resizeCall = nullptr, *resizeOrig = nullptr, *resizeStub = nullptr;
    void* resizeHook = nullptr;
    if (!ImageResizeFixInfo(&resizeCall, &resizeOrig, &resizeStub, &resizeHook) || !resizeCall) {
        Log("[gpushot] MenuScreenshotFilter's ImageResize hook is not installed: GpuMenuShot unavailable");
        return false;
    }
    if (const char* why = GpuMenuShotDiscover(game, nullptr)) {
        Log("[gpushot] %s not found: GpuMenuShot unavailable", why);
        return false;
    }
    uint8_t* const thunk = MakeThunk(reinterpret_cast<void*>(&CreateTexHook));
    if (!thunk) { Log("[gpushot] no memory near the game's code: GpuMenuShot unavailable"); return false; }
    const int64_t rel = thunk - (g_sites.createTexCall + 5);
    if (rel != static_cast<int32_t>(rel)) { Log("[gpushot] thunk out of reach: GpuMenuShot unavailable"); return false; }
    for (int k = 0; k < kJobs; ++k) g_markers[k] = {nullptr, 0, 0};
    // The vtable slot: what is in it at the moment of the write is what we chain to, and it must be the engine's function
    // (another hook there: decline). One atomic compare-exchange of the aligned 8-byte slot.
    {
        void* const current = *g_sites.copySlot;
        if (current != g_sites.copyTexture) {
            Log("[gpushot] the context vtable's RHICopyTexture slot holds %p, not the engine's function (another hook?): GpuMenuShot unavailable", current);
            return false;
        }
        g_origCopy = reinterpret_cast<CopyTextureFn>(current);
        DWORD old = 0;
        if (!VirtualProtect(g_sites.copySlot, 8, PAGE_READWRITE, &old)) { Log("[gpushot] vtable slot not writable: GpuMenuShot unavailable"); return false; }
        void* const was = InterlockedCompareExchangePointer(g_sites.copySlot, reinterpret_cast<void*>(&CopyTextureHook), current);
        VirtualProtect(g_sites.copySlot, 8, old, &old);
        if (was != current) { Log("[gpushot] the vtable slot changed during the write: GpuMenuShot unavailable"); return false; }
    }
    const int32_t rel32 = static_cast<int32_t>(rel);
    if (!WriteCode(g_sites.createTexCall + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4))) {
        Log("[gpushot] the handler's texture call could not be redirected: GpuMenuShot unavailable (the vtable slot passes through)");
        return false;
    }
    g_installed = true;
    Log("[gpushot] installed: RHICopyTexture exe+0x%X (context vtable slot exe+0x%X), handler exe+0x%X (CreateTexture2DFromImage "
        "call exe+0x%X)", static_cast<unsigned>(g_sites.copyTexture - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(g_sites.copySlot) - game.base),
        static_cast<unsigned>(g_sites.handler - game.base), static_cast<unsigned>(g_sites.createTexCall - game.base));
    if (g_sites.leftoverWhy)
        Log("[gpushot] leftover save-thumbnail bindings: %s not found: menu screenshots with one stay the game's", g_sites.leftoverWhy);
    else
        Log("[gpushot] leftover save-thumbnail bindings (a save made in a menu leaves one on the screenshot service, a game "
            "bug): handled while the save system is idle (callback exe+0x%X, save function exe+0x%X)",
            static_cast<unsigned>(g_sites.saveCallback - game.base), static_cast<unsigned>(g_sites.saveFunction - game.base));
    return true;
}

// The leftover rule (shot130q): nullptr = the binding's call now would only rewrite the idle save system's holder.
const char* GpuMenuShotLeftoverRule(const GpuMenuShotLeftoverView& v)
{
    if (!v.callback) return "a handler on the screenshot service that is neither the menu backdrop's nor the save's callback";
    if (!v.rawExec) return "the save's callback bound with another delegate kind";
    if (!v.holder || !v.saveSystem) return "a save callback without its save system";
    if (v.state != kSaveIdle) return "a save callback on the screenshot service while a save is in progress";
    if (!v.eventSlot) return "a save callback without its event";
    if (!v.eventDone) return "a save callback whose thumbnail event is still to be triggered";
    if (v.handleOnEngine) return "a save callback whose handle is bound on the engine delegate";
    if (v.resX != 0 || v.resY != 0 || v.multiplier != 1.0f) return "a save callback while a high-resolution screenshot is set up";
    return nullptr;
}

// The callback's own read of its event (lock cmpxchg [ev+58h] with 0/0): done = no event or bit 26 set.
bool GpuMenuShotEventDone(const void* ev)
{
    if (!ev) return true;
    auto* const state = reinterpret_cast<volatile LONG64*>(static_cast<uint8_t*>(const_cast<void*>(ev)) + kEventState);
    return (static_cast<uint64_t>(InterlockedCompareExchange64(state, 0, 0)) & kEventDoneBit) != 0;
}

// Is a binding with this handle on the delegate (a multicast delegate: invocation list data +0, count +8; 16-byte
// entries: instance, size; the instance's handle at +0x10)? What the callback's Remove would find.
bool GpuMenuShotHandleOnDelegate(const uint8_t* delegate, uint64_t handle)
{
    if (!delegate || handle == 0) return false;
    const uint8_t* const list = *reinterpret_cast<uint8_t* const*>(delegate);
    const int32_t num = *reinterpret_cast<const int32_t*>(delegate + 8);
    for (int32_t i = 0; i < num; ++i) {
        const uint8_t* const e = list + 16ll * i;
        const uint8_t* const inst = *reinterpret_cast<uint8_t* const*>(e);
        if (*reinterpret_cast<const int32_t*>(e + 8) != 0 && inst && *reinterpret_cast<const uint64_t*>(inst + kInstanceHandle) == handle)
            return true;
    }
    return false;
}

// In place of a held-out leftover's function (its raw ExecuteIfSafe calls [instance+20h] and returns 1).
void LeftoverHeldOut(void*, int32_t, int32_t, const void*) {}

bool GpuMenuShotHeldAdd(GpuMenuShotHeld& h, uint8_t* inst)
{
    if (h.n >= GpuMenuShotHeld::kMax) return false;
    h.inst[h.n] = inst;
    h.fn[h.n] = nullptr;
    ++h.n;
    return true;
}

// hold: each instance's function slot -> LeftoverHeldOut (the old value kept); !hold: the old values back. Game thread,
// around one call of the service's handlers' delegate (nothing else uses it meanwhile).
void GpuMenuShotHeldSwap(GpuMenuShotHeld& h, bool hold)
{
    for (int i = 0; i < h.n; ++i) {
        void** const slot = reinterpret_cast<void**>(h.inst[i] + kInstanceFn);
        if (hold) {
            h.fn[i] = *slot;
            *slot = reinterpret_cast<void*>(&LeftoverHeldOut);
        } else if (h.fn[i]) {
            *slot = h.fn[i];
            h.fn[i] = nullptr;
        }
    }
}

// GT: a handler on the service that is not the backdrop's (fn = its function, holder = its object): may it be held out?
bool GpuMenuShotLeftover(const uint8_t* fn, uint8_t* holder, bool rawExec, const char** why)
{
    GpuMenuShotLeftoverView v{};
    v.callback = g_installed && !g_sites.leftoverWhy && fn == g_sites.saveCallback;
    v.rawExec = rawExec;
    v.holder = holder != nullptr;
    v.state = -1;
    if (v.callback && v.rawExec && v.holder) {
        using WeakGet = uint8_t* (*)(const void* weak);
        uint8_t* const sys = reinterpret_cast<WeakGet>(g_sites.weakGet)(holder + kHolderSaveSystem);
        v.saveSystem = sys != nullptr;
        if (sys) {
            v.state = *reinterpret_cast<const volatile int32_t*>(sys + kSaveState);
            void* const* const slot = *reinterpret_cast<void* const* const*>(sys + kSaveEvent);
            v.eventSlot = slot != nullptr;
            if (slot) v.eventDone = GpuMenuShotEventDone(*slot);
        }
        v.handleOnEngine = GpuMenuShotHandleOnDelegate(g_sites.serviceDelegate, *reinterpret_cast<const uint64_t*>(holder + kHolderHandle));
        v.resX = *g_sites.resX;
        v.resY = *g_sites.resY;
        using GetConfig = uint8_t* (*)();
        v.multiplier = *reinterpret_cast<const float*>(reinterpret_cast<GetConfig>(g_sites.getConfig)() + kConfigMultiplier);
    }
    *why = GpuMenuShotLeftoverRule(v);
    return *why == nullptr;
}

// GT: the leftovers the job's placeholder call held out (re-checked at the delivery, which gives them the picture).
void GpuMenuShotNoteLeftovers(int job, const GpuMenuShotHeld& h)
{
    Job& j = g_jobs[job - 1];
    j.leftovers = h.n;
    for (int i = 0; i < h.n; ++i) j.leftoverInst[i] = h.inst[i];
    if (h.n) s_leftoverShots.fetch_add(1, std::memory_order_relaxed);
}

void SetGpuMenuShot(int mode, bool hdrMenuBackground)
{
    g_hdrBackground.store(hdrMenuBackground, std::memory_order_relaxed);
    mode = mode == 3 ? 3 : (mode != 0 ? 1 : 0);  // 3 = the compare test; anything else not 0 = on
    if (!g_installed) {
        if (mode) {
            static bool logged = false;
            if (!logged) Log("[gpushot] GpuMenuShot=%d but not installed (it installs at start-up when the key is not 0)", mode);
            logged = true;
        }
        return;
    }
    if (g_mode.exchange(mode) != mode)
        Log("[gpushot] GpuMenuShot %d: %s", mode,
            mode == 0 ? "menu screenshots the game's way"
            : mode == 1 ? "the menu backdrop made on the GPU, the save cache read back asynchronously"
                        : "compare: for the next 4 menu screenshots the GPU texture is shown, read back and compared with the game's "
                          "own picture (slow), then as 1");
}

int GpuMenuShotMode()
{
    if (!g_installed) return 0;
    const int m = g_mode.load(std::memory_order_relaxed);
    return m == 3 && s_compares.load(std::memory_order_relaxed) >= kCompareShots ? 1 : m;
}

uint8_t* GpuMenuShotHandlerFn() { return g_installed ? g_sites.handler : nullptr; }

bool GpuMenuShotSkipResize(int32_t srcW, int32_t srcH, int32_t dstW, int32_t dstH, bool linear)
{
    if (!t_active || linear || srcW != dstW || srcH != dstH) return false;
    s_skipResize.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Game thread, from save_thumb's takeover (the backdrop handler bound): 0 = not taken (the caller continues the game's
// way); otherwise the job index + 1, ready for GpuMenuShotEnqueue.
int GpuMenuShotPrepare(int32_t x0, int32_t y0, int32_t w, int32_t h, bool hdr, const char** why)
{
    const int mode = GpuMenuShotMode();
    *why = nullptr;
    if (!mode) { *why = "off"; return 0; }
    if (g_bbFormat.load(std::memory_order_relaxed) != kPfA2B10G10R10 || g_bbW.load() < x0 + w || g_bbH.load() < y0 + h) {
        g_probeWanted.store(true, std::memory_order_relaxed);
        *why = g_bbFormat.load() < 0 ? "the back buffer not seen yet (probe queued)" : "the back buffer is not R10G10B10A2 at this size (probe queued)";
        return 0;
    }
    if (Ms(Ticks() - g_lastGtFrameTicks.load(std::memory_order_relaxed)) > 1000.0) {
        *why = "MenuOpenFlashFix's per-frame ProcessScreenShots hook is not running (the service's picture could not be delivered)";
        return 0;
    }
    // Gates, all before any resource is touched: nothing of ours in flight anywhere.
    if (SvcState() != 0) { *why = "the previous service picture is not delivered yet"; return 0; }
    for (int i = 0; i < kJobs; ++i) {
        const int st = g_jobs[i].state.load(std::memory_order_acquire);
        if (st == 1 || st == 2) {
            if (Ms(Ticks() - g_jobs[i].tGt) > 5000.0) SwitchOff("a job did not reach the RHI thread within 5 s");
            *why = "an earlier job still in flight";
            return 0;
        }
    }
    // The GPU has finished everything we recorded: the trigger's fence comes after our commands in the stream (EnqueueCopy:
    // copy (+ ours), transition, WriteGPUFence). So buffers and table blocks may be reused or released.
    if (void* const trigger = g_trigger.load(std::memory_order_acquire)) {
        void* const fence = *reinterpret_cast<void**>(static_cast<uint8_t*>(trigger) + kReadbackFence);
        if (fence && (*reinterpret_cast<volatile int32_t*>(static_cast<uint8_t*>(fence) + kFencePending) != 0 ||
                      !VSlot<bool (*)(void*)>(fence, kFencePoll)(fence))) {
            *why = "the GPU has not finished the previous menu screenshot yet";
            return 0;
        }
    }
    if (hdr && !g_hdrLut && g_device) { *why = "no HDR tables"; return 0; }
    if (const char* e = EnsureResources(w, h, mode)) { *why = e; return 0; }
    if (hdr && !g_hdrLut) { *why = "no HDR tables"; return 0; }
    const uint32_t k = g_nextJob.fetch_add(1, std::memory_order_relaxed) % kJobs;
    Job& j = g_jobs[k];
    for (int i = 0; i < 4; ++i) j.textures[i] = j.outTex[i] = nullptr;
    j.textureCount = 0;
    j.handlerCount = 0;
    j.w = w; j.h = h; j.x0 = x0; j.y0 = y0;
    j.hdr = hdr;
    j.mode = mode;
    j.bbTex = nullptr;
    j.inBuf = g_inBuf;
    j.outBuf = g_outBuf;
    j.readback = g_readback;
    j.readbackMap = g_readbackMap;
    j.pitchIn = g_pitchIn;
    j.pitchOut = g_pitchOut;
    // The tables for this job's block (the GPU is idle, see above): the route's, then HdrMenuBackground's byte table if the
    // handler would apply it now.
    j.post = HdrMenuBackgroundShotTable(j.postTable);
    if (!j.post)
        for (int i = 0; i < 256; ++i) j.postTable[i] = static_cast<uint8_t>(i);
    uint32_t* const blk = g_lutMap + static_cast<size_t>(k) * kLutUints;
    memcpy(blk, hdr ? g_tabHdr : g_tabSdr, sizeof g_tabSdr);
    for (int i = 0; i < 256; ++i) blk[3072 + i] = j.postTable[i];
    j.tGt = Ticks();
    j.state.store(0, std::memory_order_relaxed);
    return static_cast<int>(k) + 1;
}


// Game thread: the service's handlers (checked by the caller to be the backdrop handler), kept for the fallback.
void GpuMenuShotSetHandlers(int job, const uint64_t* weak, const int32_t* adj, int n)
{
    Job& j = g_jobs[job - 1];
    j.handlerCount = n < 0 ? 0 : (n > 4 ? 4 : n);
    j.leftovers = 0;
    for (int i = 0; i < j.handlerCount; ++i) { j.handlerWeak[i] = weak[i]; j.handlerAdj[i] = adj[i]; }
}

uint8_t* GpuMenuShotServiceFn() { return g_installed ? g_sites.service : nullptr; }
const uint8_t* GpuMenuShotServiceDelegate() { return g_installed ? g_sites.serviceDelegate : nullptr; }
bool GpuMenuShotIsAdjustingExec(const uint8_t* exec) { return g_installed && exec == g_sites.execAdj; }
bool GpuMenuShotSkipping() { return t_active; }

// Game thread: the service's handlers run inside (placeholder picture: ImageResize skipped); the textures they make are kept.
// Returns the number of textures (0: nothing to draw; > 4: too many, the job is not drawn).
void GpuMenuShotBeginCapture(int job)
{
    t_job = &g_jobs[job - 1];
    t_job->textureCount = 0;
    t_active = true;
}
int GpuMenuShotEndCapture()
{
    t_active = false;
    int n = t_job ? t_job->textureCount : 0;
    const bool forced = false;
    const char* const forcedWhy = "";
    if (t_job && (n > 4 || forced)) {
        // Not drawn by the GPU: the handlers get the read-back picture at its delivery (GpuMenuShotGameThreadFrame).
        Log("[gpushot] game thread: %s: the handlers get the read-back picture instead", n > 4 ? "more than 4 textures" : forcedWhy);
        t_job->textureCount = 0;
        t_job->state.store(4, std::memory_order_release);
        s_failed.fetch_add(1, std::memory_order_relaxed);
        n = 0;
    }
    t_job = nullptr;
    return n;
}

// Game thread: the service now waits for its picture (delivered by GpuMenuShotGameThreadFrame once read back). Returns the
// generation the helper hands the picture over with.
uint32_t GpuMenuShotServiceArm(int job, uint64_t weak, int32_t adj, void* weakGet, int32_t w, int32_t h)
{
    if (uint32_t* const stale = g_svc.pixels.exchange(nullptr)) VirtualFree(stale, 0, MEM_RELEASE);
    g_svc.weak = weak;
    g_svc.adj = adj;
    g_svc.weakGet = weakGet;
    g_svc.w = w;
    g_svc.h = h;
    g_svc.job = job;
    g_svc.tArm = Ticks();
    g_svc.obj = nullptr;
    g_svc.handle = 0;
    if (uint8_t* const o = reinterpret_cast<uint8_t* (*)(const void*)>(weakGet)(&g_svc.weak)) {
        g_svc.obj = o + adj;
        g_svc.handle = *reinterpret_cast<uint64_t*>(g_svc.obj + kSvcHandle);
    }
    const uint32_t gen = ++g_svcGen & 0xFFFFFF;
    g_svc.word.store(static_cast<uint64_t>(gen) << 8 | 1, std::memory_order_release);
    return gen;
}

// SaveThumbnailAsync's helper: the read-back picture (unfiltered, alpha 255, what the game's broadcast would have passed);
// ownership of pixels (VirtualAlloc) passes here. Only for the armed generation; anything else is freed here.
void GpuMenuShotServicePixels(uint32_t gen, uint32_t* pixels, int32_t w, int32_t h)
{
    if (!pixels) return;
    uint64_t expect = static_cast<uint64_t>(gen) << 8 | 1;
    if (g_svc.word.load(std::memory_order_acquire) != expect || w != g_svc.w || h != g_svc.h) {
        VirtualFree(pixels, 0, MEM_RELEASE);
        return;
    }
    g_svc.pixels.store(pixels, std::memory_order_release);
    if (!g_svc.word.compare_exchange_strong(expect, static_cast<uint64_t>(gen) << 8 | 2, std::memory_order_acq_rel)) {
        // Dropped meanwhile: whoever gets the pointer back frees it (exactly one owner).
        if (uint32_t* const back = g_svc.pixels.exchange(nullptr)) VirtualFree(back, 0, MEM_RELEASE);
    }
}

namespace {
using WeakGetFn2 = uint8_t* (*)(const void* weak);
using ArrayCopyFn = void (*)(void* dst, const void* src);
using BroadcastFn = void (*)(void* delegate, int32_t w, int32_t h, const void* pixels);
void ReleaseService()
{
    uint64_t cur = g_svc.word.load(std::memory_order_acquire);
    while (!g_svc.word.compare_exchange_weak(cur, cur & ~uint64_t{0xFF}, std::memory_order_acq_rel)) {}
    if (uint32_t* const px = g_svc.pixels.exchange(nullptr)) VirtualFree(px, 0, MEM_RELEASE);
}
void DropService(const char* why)
{
    ReleaseService();
    s_svcDropped.fetch_add(1, std::memory_order_relaxed);
    Log("[gpushot] the service's picture not delivered: %s", why);
}
}  // namespace

// Game thread, every frame (MenuOpenFlashFix's ProcessScreenShots hook, before the game's): the service's picture, and the
// fallback for a job that was not drawn by the GPU (its handlers are called again with the real picture, as the game's
// handler is called: a stock texture, some frames later).
void GpuMenuShotGameThreadFrame()
{
    g_lastGtFrameTicks.store(Ticks(), std::memory_order_relaxed);
    s_gtFrames.fetch_add(1, std::memory_order_relaxed);
    const int st = SvcState();
    if (st == 0) return;
    const double age = Ms(Ticks() - g_svc.tArm);
    if (st == 1) {
        if (age > 3000.0) DropService("no read-back after 3 s");
        return;
    }
    // The job's own outcome first (the read-back can be ready before the RHI thread recorded the GPU pass).
    const Job& job = g_jobs[g_svc.job - 1];
    const int js = job.state.load(std::memory_order_acquire);
    if ((js == 1 || js == 2) && age <= 3000.0) return;
    const bool reServe = js == 4 || js == 1 || js == 2;
    const uint64_t weak = g_svc.weak;
    uint8_t* const o = reinterpret_cast<WeakGetFn2>(g_svc.weakGet)(&weak);
    if (!o) { DropService("the service object is gone"); return; }
    uint8_t* const svc = o + g_svc.adj;
    if (!svc[kSvcCaptured]) { DropService("the menu closed first (the game empties the picture then)"); return; }
    if (svc != g_svc.obj || *reinterpret_cast<uint64_t*>(svc + kSvcHandle) != g_svc.handle) { DropService("a new capture began"); return; }
    if (*reinterpret_cast<int32_t*>(svc + kSvcW) != g_svc.w || *reinterpret_cast<int32_t*>(svc + kSvcH) != g_svc.h ||
        *reinterpret_cast<int32_t*>(svc + kSvcPicture + 8) != 0) {
        DropService("the service holds another picture");
        return;
    }
    const uint64_t t0 = Ticks();
    const int32_t n = g_svc.w * g_svc.h;
    const ScriptArray arr{g_svc.pixels.load(std::memory_order_acquire), n, n};
    reinterpret_cast<ArrayCopyFn>(g_sites.arrayCopy)(svc + kSvcPicture, &arr);
    // Handlers that asked meanwhile (bound, not called: the game's cached path found no picture) get it now, as the
    // game's cached path gives it.
    int bound = 0;
    const uint8_t* const list = *reinterpret_cast<uint8_t* const*>(svc + kSvcHandlers);
    const int32_t num = *reinterpret_cast<const int32_t*>(svc + kSvcHandlers + 8);
    for (int32_t i = 0; i < num; ++i)
        if (*reinterpret_cast<const int32_t*>(list + 16ll * i + 8) != 0 && *reinterpret_cast<void* const*>(list + 16ll * i)) ++bound;
    // The leftover save bindings held out of the placeholder call (shot130q) are bound too: they get the picture here,
    // each only if the rule still holds (rev130s SF-1). One that fails now (a save began in between, a high-resolution
    // screenshot was set up) is held out of this call too: the game called it before that change, and the change
    // overwrites or keeps what that call did (a new save resets the holder; the setup's values stay).
    int leftovers = 0;
    GpuMenuShotHeld changed{};
    const char* changedWhy = nullptr;
    for (int32_t i = 0; i < num; ++i) {
        uint8_t* const inst = *reinterpret_cast<uint8_t* const*>(list + 16ll * i);
        if (*reinterpret_cast<const int32_t*>(list + 16ll * i + 8) == 0 || !inst) continue;
        bool held = false;
        for (int k = 0; k < job.leftovers && !held; ++k) held = job.leftoverInst[k] == inst;
        if (!held) continue;
        ++leftovers;
        const char* why = nullptr;
        if (!GpuMenuShotLeftover(*reinterpret_cast<uint8_t* const*>(inst + kInstanceFn),
                                 *reinterpret_cast<uint8_t* const*>(inst + kInstanceObject), true, &why)) {
            GpuMenuShotHeldAdd(changed, inst);  // never more than the takeover held
            changedWhy = why;
        }
    }
    if (bound) {
        GpuMenuShotHeldSwap(changed, true);
        reinterpret_cast<BroadcastFn>(g_sites.broadcast)(svc + kSvcHandlers, g_svc.w, g_svc.h, svc + kSvcPicture);
        GpuMenuShotHeldSwap(changed, false);
        s_svcLate.fetch_add(static_cast<uint64_t>(bound - leftovers), std::memory_order_relaxed);
    }
    int reServed = 0;
    if (reServe) {
        // The job was not drawn: its handlers (the backdrop handler, checked at the takeover) made a texture with the
        // placeholder; called again with the real picture they make the game's texture and set it, as at a cached request.
        using HandlerFn2 = void (*)(void* obj, int32_t w, int32_t h, const void* pixels);
        for (int i = 0; i < job.handlerCount; ++i) {
            const uint64_t hw = job.handlerWeak[i];
            uint8_t* const ho = reinterpret_cast<WeakGetFn2>(g_svc.weakGet)(&hw);
            if (!ho) continue;
            reinterpret_cast<HandlerFn2>(g_sites.handler)(ho + job.handlerAdj[i], g_svc.w, g_svc.h, svc + kSvcPicture);
            ++reServed;
        }
    }
    ReleaseService();
    s_svcDelivered.fetch_add(1, std::memory_order_relaxed);
    Log("[gpushot] the service's %dx%d picture delivered on the game thread %.1f ms after the screenshot (%.2f ms)%s%s", g_svc.w,
        g_svc.h, Ms(t0 - g_svc.tArm), Ms(Ticks() - t0), bound > leftovers ? ", late requesters served from it" : "",
        reServe ? ", FALLBACK: the job was not drawn by the GPU" : "");
    if (bound > leftovers) Log("[gpushot]   %d handler(s) had asked meanwhile (the game would have served them at once)", bound - leftovers);
    if (leftovers > changed.n)
        Log("[gpushot]   %d leftover save binding(s) given the picture (the game calls them with the screenshot)", leftovers - changed.n);
    if (changed.n) Log("[gpushot]   %d leftover save binding(s) not given it: %s since the screenshot", changed.n, changedWhy ? changedWhy : "?");
    if (reServe) Log("[gpushot]   fallback: %d of %d handler(s) called again with the real picture (job state %d)", reServed, job.handlerCount, js);
}

// Game thread, mode 2: after the game's own read (SDR FColor, before ProcessScreenShots' alpha loop), compare with the GPU result.

// Game thread: the marker for job (1-based) to pass to ReadPixels; the job is then in flight.
void* GpuMenuShotMarker(int job)
{
    g_jobs[job - 1].state.store(1, std::memory_order_release);
    return &g_markers[job - 1];
}

// A probe marker: the next ReadSurfaceCommand for it only records the back buffer's format and size.
ScriptArray g_probeMarker{nullptr, 0, 0};
Job g_probeJob;
void* GpuMenuShotProbeMarker()
{
    if (!g_probeWanted.exchange(false)) return nullptr;
    g_probeJob.mode = -1;
    return &g_probeMarker;
}

bool GpuMenuShotIsMarker(const void* outData)
{
    if (outData == &g_probeMarker) return true;
    for (int k = 0; k < kJobs; ++k)
        if (outData == &g_markers[k]) return true;
    return false;
}

// Render thread: our ReadSurfaceCommand (in place of the read).
void GpuMenuShotRenderThread(void** ctx, void* cmdList)
{
    if (ctx[1] == &g_probeMarker) {
        (void)RenderThreadJob(&g_probeJob, ctx, cmdList);
        Log("[gpushot] back buffer: format %d, %dx%d", g_bbFormat.load(), g_bbW.load(), g_bbH.load());
        return;
    }
    Job* j = nullptr;
    for (int k = 0; k < kJobs; ++k)
        if (ctx[1] == &g_markers[k]) j = &g_jobs[k];
    if (!j) return;
    j->tRt = Ticks();
    const int r = RenderThreadJob(j, ctx, cmdList);
    if (r != 0) {
        Log("[gpushot] render thread: %s: this menu screenshot is not drawn by the GPU pass (its handlers get the read-back picture)",
            r == 1 ? "the back buffer changed (format / size)"
                   : "the back buffer or the new texture is not as expected");
        s_failed.fetch_add(1, std::memory_order_relaxed);
        j->state.store(4, std::memory_order_release);
        if (r == 2) SwitchOff("unexpected state on the render thread");
    }
}

// Game thread, modes 2/3, after the game's own synchronous read (which waited for the GPU): compare. ref = the game's
// FColor picture (alpha 255). Returns true when identical.
bool GpuMenuShotVerify(int job, const uint32_t* ref, int32_t w, int32_t h)
{
    Job& j = g_jobs[job - 1];
    if (j.state.load(std::memory_order_acquire) != 3 || !j.readbackMap) {
        Log("[gpushot] verify: the GPU pass did not run (state %d)", j.state.load());
        s_verifyBad.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    D3D12_RANGE all{0, static_cast<SIZE_T>(j.pitchOut) * h};
    D3D12_RANGE none{0, 0};
    void* p = nullptr;
    if (FAILED(j.readback->Map(0, &all, &p))) {
        Log("[gpushot] compare: the read-back buffer could not be mapped");
        s_verifyBad.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const size_t n = static_cast<size_t>(w) * h;
    uint32_t* const expect = static_cast<uint32_t*>(VirtualAlloc(nullptr, n * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!expect) {
        j.readback->Unmap(0, &none);  // rev130n GN-3: every Map has its Unmap
        Log("[gpushot] compare: no memory for the expected picture");
        s_verifyBad.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    BoxFilter2x2(w, h, ref, expect, true);  // fix 10's filter (identical to the game's ImageResize at the same size)
    if (j.post)  // HdrMenuBackground's conversion of the resized picture, as its wrapper does it (colour bytes only)
        for (size_t i = 0; i < n; ++i) {
            uint8_t* const q = reinterpret_cast<uint8_t*>(expect + i);
            q[0] = j.postTable[q[0]];
            q[1] = j.postTable[q[1]];
            q[2] = j.postTable[q[2]];
        }
    size_t diffs = 0, first = n;
    uint32_t fe = 0, fg = 0;
    int maxDiff = 0;  // the largest difference of one colour (or alpha) byte between the game's picture and ours
    for (int32_t y = 0; y < h; ++y) {
        const uint32_t* const g = reinterpret_cast<const uint32_t*>(j.readbackMap + static_cast<size_t>(y) * j.pitchOut);
        const uint32_t* const e = expect + static_cast<size_t>(y) * w;
        if (memcmp(g, e, static_cast<size_t>(w) * 4) == 0) continue;
        for (int32_t x = 0; x < w; ++x)
            if (g[x] != e[x]) {
                if (first == n) { first = static_cast<size_t>(y) * w + x; fe = e[x]; fg = g[x]; }
                ++diffs;
                for (int c = 0; c < 4; ++c) {
                    const int d = static_cast<int>((g[x] >> (8 * c)) & 0xFF) - static_cast<int>((e[x] >> (8 * c)) & 0xFF);
                    if (d > maxDiff) maxDiff = d;
                    if (-d > maxDiff) maxDiff = -d;
                }
            }
    }
    const uint64_t done = s_compares.fetch_add(1, std::memory_order_relaxed) + 1;
    VirtualFree(expect, 0, MEM_RELEASE);
    j.readback->Unmap(0, &none);
    if (diffs == 0) {
        s_verifyOk.fetch_add(1, std::memory_order_relaxed);
        Log("[gpushot] compare %llu (mode %d, %s%s): %dx%d backdrop identical to the game's (every byte; max channel difference 0)",
            static_cast<unsigned long long>(done), j.mode, j.hdr ? "HDR10" : "SDR", j.post ? ", HdrMenuBackground table" : "", w, h);
    } else {
        s_verifyBad.fetch_add(1, std::memory_order_relaxed);
        Log("[gpushot] COMPARE %llu MISMATCH (mode %d, %s%s): %dx%d, %zu pixels differ, max channel difference %d, first at (%zu, %zu): "
            "game %08X, GPU %08X", static_cast<unsigned long long>(done), j.mode, j.hdr ? "HDR10" : "SDR",
            j.post ? ", HdrMenuBackground table" : "", w, h, diffs, maxDiff, first % static_cast<size_t>(w), first / static_cast<size_t>(w),
            fe, fg);
    }
    if (j.mode == 3 && done == kCompareShots)
        Log("[gpushot] compare finished: %llu identical, %llu different (GpuMenuShot now works as 1)",
            static_cast<unsigned long long>(s_verifyOk.load()), static_cast<unsigned long long>(s_verifyBad.load()));
    return diffs == 0;
}

void GpuMenuShotTaken(int job, uint64_t gtTicks)
{
    s_taken.fetch_add(1, std::memory_order_relaxed);
    s_gtTicks.fetch_add(gtTicks, std::memory_order_relaxed);
    const Job& j = g_jobs[job - 1];
    Log("[gpushot] menu screenshot %dx%d (%s route%s, mode %d, %d texture(s)) taken over: game thread %.2f ms%s", j.w, j.h,
        j.hdr ? "HDR" : "SDR", j.post ? ", HdrMenuBackground table" : "", j.mode, j.textureCount, Ms(gtTicks),
        j.leftovers ? " (leftover save binding(s) on the service: they get the picture with its delivery)" : "");
}

void GpuMenuShotDeclined(const char* why)
{
    s_declined.fetch_add(1, std::memory_order_relaxed);
    Log("[gpushot] menu screenshot left to the game: %s", why ? why : "?");
}

void LogGpuMenuShotStats()
{
    if (!g_installed) return;
    const uint64_t sd = s_svcDelivered.exchange(0), sx = s_svcDropped.exchange(0), sl = s_svcLate.exchange(0);
    if (sd || sx || sl)
        Log("[gpushot] service pictures: %llu delivered on the game thread, %llu dropped, %llu late requesters served",
            static_cast<unsigned long long>(sd), static_cast<unsigned long long>(sx), static_cast<unsigned long long>(sl));
    const uint64_t t = s_taken.exchange(0), r = s_recorded.exchange(0), d = s_declined.exchange(0), f = s_failed.exchange(0),
                   ok = s_verifyOk.load(), bad = s_verifyBad.load(), sk = s_skipResize.exchange(0), gt = s_gtTicks.exchange(0),
                   lo = s_leftoverShots.exchange(0);
    // Only when something happened since the last report (rev130n GN-1: the cumulative compare counts kept it repeating).
    static uint64_t lastOk = 0, lastBad = 0;
    const bool compared = ok != lastOk || bad != lastBad;
    lastOk = ok;
    lastBad = bad;
    if (t || r || d || f || sk || compared)
        Log("[gpushot] since last report: %llu taken over (game thread %.2f ms each), %llu recorded on the RHI thread, %llu left to the "
            "game, %llu failed, %llu resizes skipped; compares since start %llu identical, %llu different; %llu taken over with "
            "leftover save bindings",
            static_cast<unsigned long long>(t), t ? Ms(gt) / static_cast<double>(t) : 0.0, static_cast<unsigned long long>(r),
            static_cast<unsigned long long>(d), static_cast<unsigned long long>(f), static_cast<unsigned long long>(sk),
            static_cast<unsigned long long>(ok), static_cast<unsigned long long>(bad), static_cast<unsigned long long>(lo));
}

// Self-test: the shader's arithmetic on the CPU (the same integer steps, with the tables the game's conversion makes)
// against fix 40's conversion + fix 10's filter.
void GpuMenuShotTestReference(const uint32_t* r10, int32_t w, int32_t h, const uint32_t* lut, uint32_t* out)
{
    for (int32_t y = 0; y < h; ++y)
        for (int32_t x = 0; x < w; ++x) {
            const bool right = x + 1 < w, down = y + 1 < h;
            uint32_t s[3] = {};
            auto add = [&](uint32_t p) {
                s[0] += lut[(p >> 20) & 1023];
                s[1] += lut[1024 + ((p >> 10) & 1023)];
                s[2] += lut[2048 + (p & 1023)];
            };
            add(r10[static_cast<size_t>(y) * w + x]);
            if (right) add(r10[static_cast<size_t>(y) * w + x + 1]);
            if (down) add(r10[static_cast<size_t>(y + 1) * w + x]);
            if (right && down) add(r10[static_cast<size_t>(y + 1) * w + x + 1]);
            const uint32_t sh = (right ? 1u : 0u) + (down ? 1u : 0u);
            out[static_cast<size_t>(y) * w + x] =
                lut[3072 + (s[0] >> sh)] | lut[3072 + (s[1] >> sh)] << 8 | lut[3072 + (s[2] >> sh)] << 16 | 0xFF000000u;
        }
}

size_t GpuMenuShotShaderSize() { return sizeof kGpuMenuShotCs; }

const unsigned char* GpuMenuShotShaderBlob(size_t* size)
{
    if (size) *size = sizeof kGpuMenuShotCs;
    return kGpuMenuShotCs;
}

}  // namespace opf
