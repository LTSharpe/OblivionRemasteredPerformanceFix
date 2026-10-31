# What we measured, what we changed, what we left alone

This page is for the curious: how the fix was found, the evidence behind each change, and the many ideas that were tested
and *not* shipped, with the reason. The plain-language version is in the [README](../README.md) and
[how-it-works.md](how-it-works.md).

All fixes are on by default in 1.0.0-beta. `RdgParallelTranslate=2` selects automatically (off on cards under 9.5 GB or CPUs under 8 logical processors),
and `FrameGenPacingTuning=1` selects Tuning Set B. `FrameGenPacingTuning` and `FrameGenCameraInfo` act with AMD's
FSR 3.1.4 DLL and do nothing with the game's own FSR 3.1.0 DLL. `HitchLedger` is a diagnostic, off by default.

Function addresses (`exe+0x...`) refer to the Steam build 19115871 of `OblivionRemastered-Win64-Shipping.exe` (Unreal
Engine 5.3). The fix itself never uses them: it finds every piece of code by byte pattern and checks it before changing
anything.

## How we measured

- **Test PC:** AMD Ryzen 7 5800X (8 cores, 16 threads), AMD Radeon RX 9070 XT 16 GB, 32 GB RAM, Windows 11, game on an NVMe
  drive.

- **CPU-limited runs** used a small window (720p or 854x480, FSR Performance or no upscaler, frame generation off), so the
  CPU side decides the frame rate. **Real-world runs** used 4K with FSR Performance and frame generation, Ultra with Shadows High, hardware ray tracing, and a 60 fps cap where noted.

- **Per-thread CPU time** for every named engine thread (GameThread, RenderThread, RHIThread, the old engine's
  OblivionThread, task helpers), frame times from PresentMon, GPU busy time.

- **Stack sampling and call counting** inside the running game to find where a thread spends its time, and the public
  Unreal Engine 5.3 source to name the functions involved (none of that source is in this repository).

- **A benchmark harness** ([benchmark.md](benchmark.md)) that starts a new game, flies a camera through fixed scenes,
  walks through doors and fast-travels, and records all of the above per scene, plus reference images.

- **Rules we kept to:**
  - Comparisons use repeated or interleaved runs; each section states whether these share a running game or use fresh launches. Two identical runs can differ by up to ±10%
    (NPCs move, weather changes).
  - Steady-state comparisons exclude the first 2-3 minutes after launch; start-up measurements include them (see "Game behaviour worth knowing").
  - Every change gets image checks: frozen, 8-frame-averaged reference images with the fix on and off, compared
    automatically against the difference between two identical runs.
  - **The rendered picture and game results are preserved; `NpcCreationPacing` and `CellBufferPacedEvict` change timing only.** Hardware ray-traced Lumen stays at
    `r.Lumen.HardwareRayTracing.LightingMode 3`.

## The bottleneck chain in Cheydinhal

Cheydinhal runs at 30-40 fps on a fast PC while Task Manager shows the CPU about 30% busy. The game runs two engines: the
2006 engine runs the world (AI, scripts, cells) on its own thread, and Unreal Engine 5 draws it. Each frame passes through
a chain of threads, and the slowest one sets the frame rate. Removing one limit exposes the next:

| Step | Limiting thread | What it was spending time on | Fixes |
|---|---|---|---|
| 1 | RenderThread, ~89% busy | copying bone matrices (a third of its time), rebuilding the sun every frame | `BoneUploadStores`, `AtmosphereDirtyFilter`, `AtmosphereLightFastPath` |
| 2 | RHIThread, 90-95% busy, while the helper threads were 60-88% idle | turning the frame's draw commands into DirectX 12 commands, alone | `RdgParallelTranslate`, `RdgSubmitBatch` |
| 3 | GameThread, 78-86% busy | the game's own logic: NPC perception and movement, animation, cloth, waiting for the old engine | `ClothConfigCache`; the rest is real game work |

Other changes target individual stalls and bugs: `PrepareCollisionBehindLoadingScreen` prepares collision before the picture returns, `DisplayModeCache` and `MenuScreenshotFilter` shorten menu opens, `NameLookupCrashGuard` guards a crash, and `RestoreFsrAfterAltTab` restores FSR after focus changes.

The following two result tables were measured with an earlier build. The 720p Cheydinhal table uses `BoneUploadStores`, `AtmosphereDirtyFilter`, `AtmosphereLightFastPath` and `RdgParallelTranslate`, with FSR Performance and frame generation off:

| View | Before | After | |
|---|---|---|---|
| Canal, noon | 30.6 fps | 42.7 fps | +40% |
| Chapel square, noon | 44.7 fps | 56.6 fps | +27% |
| Bridge Inn front | 53.3 fps | 64.9 fps | +22% |
| Canal, dusk | 32.2 fps | 43.9 fps | +36% |
| Canal, night | 34.1 fps | 48.6 fps | +43% |

RenderThread time per frame -21..-45%, RHIThread -35..-47%, 1% lows +30..+46% at 720p.

The broader comparison against the game without the DLL used the rendering, door, menu and alt-tab changes, at 4K (FSR Performance, frame generation, ray
tracing, Ultra with Shadows High) and CPU-limited at 720p; 2-4 runs per side, each a fresh game launch:

| What you notice | Stock game | With the fix | Change |
|---|---|---|---|
| Cheydinhal at 4K, average fps on screen (3 city views at noon, frame generation on) | 64 fps | 91 fps | +43% |
| Cheydinhal at 4K, 1% low fps (same views) | 50 fps | 68 fps | +35% |
| Cheydinhal when the CPU is the limit (720p, FSR Performance, no frame generation) | 45 fps (1% low 32) | 57 fps (1% low 40) | +25% |
| Stepping out of a door at 4K: worst frame after the loading screen (12 exits, median) | 187 ms | 95 ms | half as long |
| Stepping out of a door when the CPU is the limit: time lost to hitches in the first 5 s (6 exits, mean) | 0.63 s | 0.15 s | -77% |
| Opening the menu on the Graphics settings page at 4K | 0.74 s | 0.43 s | -43% |
| Opening the pause menu or the inventory at 4K | 0.35-0.36 s | 0.19-0.21 s | -45% |
| Alt-tab after using the inventory, map or character menu | FSR and frame generation stay off | back on at once | fixed |

CPU time per rendered frame at 4K (GameThread / RenderThread / RHIThread, ms): canal 16.2 / 18.8 / 14.3 stock,
17.5 / 12.5 / 9.1 with the fix; chapel square 14.5 / 18.4 / 12.9 -> 14.9 / 10.6 / 7.7; inn front 12.1 / 14.2 / 11.8 ->
12.3 / 10.3 / 6.8. Stock is held back by the render and RHI threads; with the fix the GameThread is the limit. The door
loading itself is unchanged (arrival 2.7-3.1 s after the door is used, both).

## Rendering: the CPU side of each frame

<a id="boneuploadstores"></a>

### Bone upload (`BoneUploadStores`)

[Explanation](how-it-works.md#boneuploadstores).

- **Found:** 32.7% of the RenderThread's samples were in one loop, the AVX2 variant of Unreal's ISPC
  `UpdateBoneData_CopyBones` (`exe+0x5C7550`). It copies the skeletons of every animated character into a GPU upload
  buffer: ~146 calls and ~7,200 bones per frame at the Cheydinhal chapel.

- **Why it was slow:** each bone's last 16 bytes were written with a masked store (`vmaskmovps`). The destination is
  write-combined memory (GPU upload heap, page protection `PAGE_WRITECOMBINE`). Masked stores defeat write combining
  there and are extremely slow.

- **The change:** the mask constant is `{-1,-1,-1,-1,0,0,0,0}`, i.e. "write exactly the low 16 bytes". A plain 16-byte
  store (`vmovups xmm`) writes the same bytes. This covers five sites: the AVX2, AVX and AVX-512 variants, chosen by the
  CPU. Each site's mask is verified at runtime before patching.

- **Result:** the loop went from 32.7% to 1.1% of the RenderThread, and 1% lows rose from 38.6 to 45.4 fps. The GPU
  receives byte-identical data.

<a id="atmospheredirtyfilter"></a>

### Atmosphere dirty filter (`AtmosphereDirtyFilter`)

[Explanation](how-it-works.md#atmospheredirtyfilter).

- **Found:** the sun light was fully removed from and re-added to the scene every frame. Each re-add loops over all
  ~13,000 primitives to build light interactions (`FLightSceneInfo::AddToScene`, `exe+0x1F1E890`).

- **Cause:** the game's weather and time-of-day plugin (`AtmosphericToolRuntime`) applies its values through reflection
  accessors. It calls `Set` and then **always** `PostSet` (`exe+0x4574080`), which calls
  `UActorComponent::MarkRenderStateDirty`, whether the value changed or not. That came to about 3,700 rebuild requests a
  second across the sun, moon, height fog, sky atmosphere and sky light.

- **The change:** the 15 accessor vtables are wrapped. The fix compares the object's bytes before and after `Set` and
  skips `PostSet` when nothing changed. Unreal's own setters behave this way: they dirty an object only when a value
  changes.

- **Result:**
  - `MarkRenderStateDirty` went from ~3,700/s to ~10/s, and sun re-adds from one per frame to zero.
  - The RenderThread became 13-20 percentage points less busy.
  - The frame rate didn't change yet, because the RHIThread was the next limit.

<a id="atmospherelightfastpath"></a>

### Light updates (`AtmosphereLightFastPath`, `AtmosphereLightCloudPath`)

[Explanation](how-it-works.md#atmospherelightfastpath).

- **Found:** from afternoon to sunset, the sun's Intensity, Temperature and IndirectLightingIntensity change a little
  every frame, so `AtmosphereDirtyFilter` alone still rebuilt the sun every frame for much of the day.

- **The change:** when only Intensity, Temperature, IndirectLightingIntensity or VolumetricScatteringIntensity changed on
  a non-static light, the fix calls `ULightComponent::UpdateColorAndBrightness` (`exe+0x304C150`), the call the stock
  setters for those four values make, instead of a full rebuild.

- **Result:**
  - `FScene::AddLight` went from 49.8/s to 0, and the RenderThread dropped 11.5 percentage points.
  - Screenshots taken while toggling the fix during a sunset show a smooth brightness curve with no step. The difference
    maps show only water ripples and swaying plants.

- **The cloud path (`AtmosphereLightCloudPath`).**
  - **Found (Echo Mine -> Brotch Camp):** the render thread re-added the sun every few frames
    (`FScene::AddLightSceneInfo_RenderThread` exe+0x2262080 -> `AddToScene`, ~9% of its samples on the walk). A
    probe of the forwarded Sets: Altar's weather blend changes the directional light's `CloudShadowOnSurfaceStrength`
    (+0x4BC) and `CloudScatteredLuminanceScale` (+0x4D0) (~2,000 changes a minute on the walk). UE 5.3 has no setter
    for either; they live in `FDirectionalLightSceneProxy` (copied in its constructor) and only the renderer reads them,
    for the atmosphere lights.
  - **The change:** when a Set changed only these two (and `AtmosphereLightFastPath`'s fields) on a movable directional light with a
    proxy, the game thread leaves the values for that proxy and skips PostSet; a wrapper of the proxy vtable's
    `SetAtmosphereRelatedProperties` (slot +0x118 of exe+0x75E9400, setter exe+0x3043800), which
    `FDeferredShadingSceneRenderer::Render` calls on the render thread for each atmosphere light every frame before
    anything reads the cloud values, writes them into the proxy (+0x2C8, +0x2D8) after the renderer's own values.
    Offsets from the constructor's copies (exe+0x301A92A); the component's proxy pointer (+0x3A8) found on first use.
    Values not picked up within 250 ms go the stock way (the next Set rebuilds the light); a light without a proxy
    (hidden: the moon by day) has nothing to update. At most one frame earlier than the rebuild.
  - **Result:** walk (480p, uncapped, one game, 5 on / 4 off taken in turn): render thread 8.3-10.1 -> 6.3-8.2 ms per
    frame (medians 8.6 -> 7.0), samples in the re-add 8.4-8.7% -> 0; game thread the same (11.2-13.3 vs 11.4-13.6 ms).
    The DLL's own check: before each change the sun's proxy held the light's previous values 2,021 of 2,021 times in a
    walk minute (0 out of step; mismatches only while loading, when no world frame is drawn). A static view where the
    clouds hold still (0 changes), switched every 10 s: 67.7 vs 67.4 fps.

**Weather transitions.** During a rain-to-clear blend below Echo Mine (480p, 60 fps cap, three runs each, taken in turn), handing over the sun's additional render-side fields reduced render-thread time from 6.9-7.9 to 5.5-6.4 ms. About 270 rebuilds every five seconds became none; before every update, the rendered sun held the expected previous values. The twelve-field path is limited to recently rendered sun and moon lights; other lights rebuild as before.

**The sky's light choice (1.1.0, issue #1 "Flickering Cloud Layer"; a bug in OblivionPerfFix 1.0.0-beta, not in the game).**
- **Found:** the game has three directional lights, all `bAtmosphereSunLight`: the sun (`DirectionalLight`, index 0),
  `Thunder` on the same actor (index 0, `bAffectsWorld` 0 at rest) and `DirectionalLight_Moon` (index 1).
  `FScene::AtmosphereLights[i]` (sky, sun disc, cloud lighting) is chosen in `ProcessAtmosphereLightAddition` only when a
  light is added (the brightest at that index; removal re-picks among the others). Stock, every weather light is
  re-added every frame (PostSet), so the choice follows the brightness. The filter and the fast path (and stock UE's own
  `UpdateLightColorAndBrightness`) do not re-pick, so a flash that took index 0 kept it while dimming.
- **Reproduced (2026-10-18, 480p, cloudy noon at Hircine's Shrine, Thunder made to affect the world and set 5000 -> 2
  -> 0 through its stock setter):** filter on: screen luma 75 -> 169 -> 22-27 (sky and clouds dark) -> 86; filter off
  (stock): 75 -> 168 -> 78/75 -> 75. The reporter's log shows a lit directional light that was not the sky light
  (8 per minute "not drawn as sky lights") at the shrine, i.e. Thunder lit.
- **The change:** on every Set of a light component the DLL notes whether it has a directional proxy and its
  `AtmosphereSunLightIndex` (offset from the property tables, 0x49C); while another lit directional light at the same
  index was Set in the last 2 s, or the light has a proxy but was not drawn as a sky light in the last 100 ms, its
  PostSets go through (rebuilt every frame, as stock), which re-picks the brightest. Afterwards: 72.6/69.7 (fix) vs
  73.0/70.4 (stock); the check counted only during the flashes and while the world loaded; 0 in normal play.
- **Also:** cloud values left for a proxy carry their component; they are dropped when the light is rebuilt or found
  without a proxy, and the render side writes them only into a proxy whose `LightComponent` is that component (a
  removed proxy's address reused by a later one got the old values before).

<a id="rdgparalleltranslate"></a>

### Parallel command translation (`RdgParallelTranslate`)

[Explanation](how-it-works.md#rdgparalleltranslate).

- **Found:** about 55% of the RHIThread's time went to replaying command lists that the render graph had already recorded
  in parallel on helper threads.

- **Cause:** in UE 5.3 only pass sets flagged `ParallelTranslate` (in practice, Nanite's raster passes) are translated on
  helpers. `FRDGBuilder::Execute` picks `Normal` or `Disabled` priority with a `setne r8b`. UE 5.5 changed the default to
  translate every such command list in parallel (`r.RHICmd.ParallelTranslate.Enable`).

- **The change:** one 4-byte write (`setne r8b` becomes `mov r8b,1`) sends every render-graph pass set down the parallel
  path that 5.3 already uses for Nanite. Ray-tracing commands (shader bindings, acceleration-structure builds, ray
  dispatches) are issued on the immediate command list, so they stay on the RHIThread in their original order. We checked
  this in captures at three locations.

- **A crash found and fixed on the way:** an Unreal 5.3 bug ended the game with
  `D3D12DescriptorCache.cpp:772 Unable to reserve slot`. A brand-new command context can ask the shader-visible
  descriptor heap for a slot (a structured-buffer UAV clear) before it has a descriptor block. More parallel translation
  creates more fresh contexts, so this happened, mostly when the fix was switched on during a full-screen video. The fix
  wraps the heap's `GetTotalSize` so that a context without a block gets one first, exactly as opening its command list
  would. Without this guard, parallel translation stays off.

- **Buffer safety (UE-217692):** a view created while its buffer moves can miss the rename notification and retain a resource freed a frame later. One crash in five launches occurred two seconds after an area load in `FD3D12ContextCommon::UpdateResidency`. The stock engine has this race too. Parallel translation also needs frame-start moves to finish before commands are translated, or writes may target the old copy. With `ViewRenameGuard` on, a frame fence orders frame start before translation and buffer defragmentation stays enabled. Otherwise buffer tidying pauses before translation is enabled (250 ms safety interval), is re-paused if an Engine.ini value enables it, and resumes one second after translation stops. Translation stays off if the safety variable cannot be found. Texture tidying is left alone; it moves about one texture a second.
- **Safety evidence:** about 50 further area loads in 11 launches, some moving 32 times more memory per frame, did not reproduce the rare crash without the pause. A probe found no stale view. With the pause, there were no crashes or stale views in 42 million binds; fps across six scenes stayed within noise. Pausing cost +0.3 GB on average, up to +0.7 GB after several worldspace changes. Six 720p scenes averaged 8.04 GB stock, 8.70 GB with paused tidying, 8.33 GB with tidying running, and 7.88 GB with parallel translation off. Roughly half the extra 0.6-0.7 GB was the pause and half was translation's own command memory.

- **Memory range:** keeping GPU scene buffers in place reduced the pause's cost in one six-scene comparison (8.94 vs 8.99 GB, three fresh launches per side). Other routes measured 0.65-1 GB over stock; switching translation off saved 0.7-0.95 GB. Current automatic mode, `RdgParallelTranslate=2`, disables translation on cards under 9.5 GB: at an 8 GB card's ~7 GB budget, 1440p FSR Performance High, Rumare/road walk ran at 30/39 fps with translation, 48/55 without, against 39/46 stock. The extra command memory is 0.4-1 GB.
- **Result:**
  - The RHIThread went from 82-93% to 53-59% busy.
  - **+22% to +25% fps** where the CPU is the limit.
  - Reference images with the fix on and off match (SSIM 0.99, brightness within 0.6%).

<a id="rdgsubmitbatch"></a>

### Submission batching (`RdgSubmitBatch`)

[Explanation](how-it-works.md#rdgsubmitbatch).

- **Found:** with `RdgParallelTranslate`, each parallel chunk became its own GPU submission: ~115 per frame instead of ~5. The driver pays
  a fixed cost at each one: +2.4 ms of GPU time per frame. At 4K, where the GPU is the limit, `RdgParallelTranslate` alone cost about 5%
  fps.

- **The change:** finished chunks are held back briefly (never longer than 1 ms) and handed over in groups of up to 32.
  The engine's own payload merge then submits each group at once. Order is always kept, and anything that cannot be merged
  safely goes out alone as before.

- **Result:**
  - About 25 submissions per frame, and GPU time per frame 15.7 ms down to 12.2-12.6 ms at the canal.
  - In a GPU-limited test (render scale 200) it ran 38.5 fps, against 35.8 without batching and 34.0 with parallel
    translation off. So `RdgParallelTranslate` and `RdgSubmitBatch` together help also where the GPU is the limit.

<a id="gpusceneparallelupload"></a>

### Scene data uploaded on helper threads (`GpuSceneParallelUpload`)

[Explanation](how-it-works.md#gpusceneparallelupload).

**Measured:** Kvatch C, the arrival's worst frame 50-55 -> 38-43 ms (480p and 4K FSR Performance with frame generation); Imperial City flight -5.5 % ms over budget; same images.

<a id="clothconfigcache"></a>

### Cloth config cache (`ClothConfigCache`)

[Explanation](how-it-works.md#clothconfigcache).

- **Found:** `FClothSimulationProxy::Tick_GameThread` re-runs `FClothingSimulationConfig::Initialize` for every simulated
  cloth piece every frame. That is a deep copy of every level of detail's property collection (stiffness, damping, wind,
  collision thickness...). This is stock UE 5.3 behaviour and is unchanged up to 5.6. At the chapel square that was
  ~2,000 copies a second and 8% of the GameThread.

- **The change:** the call (`exe+0x53CEEFE`) is redirected. The copy is skipped when the five arrays the solver reads are
  already byte-identical. After every real copy the fix checks that copy and source compare equal; if that check ever
  fails, the fix switches itself off.

- **Result:**
  - The copies went from 2,056/s to 0.5/s (99.98% skipped; the real copies were all genuine flag changes).
  - GameThread time per frame went from 12.99 to 12.46 ms.
  - Frozen images on vs off: SSIM 0.992, the same as off vs off.

<a id="watercapture"></a>

### Water pictures redrawn without work nobody sees (`WaterCaptureFsrSkip`, `WaterCaptureNoRhiFlush`, `WaterCaptureNoOcclusionWait`, `WaterCaptureFastMeshScan`)

[Explanation](how-it-works.md#watercapture).

- **Found (2026-10-22, Imperial City flight, 4K at the test PC's play settings):** the stone-bridge and lake hitches were
  render-thread-bound frames. The render thread was in `UpdateWaterInfoRendering_RenderThread` (exe+0x42EEDA0, UE 5.3
  Water plugin): three standalone `FSceneRenderer`s (ground depth, water bodies, dilation) into a zone's 32 x 32 or
  256 x 256 water-info texture. Trigger: `AWaterZone::OnLevelChanged` / `AddWaterBodyComponent`, i.e. every World
  Partition level streamed in or out that holds a landscape proxy or a water body touching the zone. A timing probe
  counted about 155 zone updates per flight, none with the same inputs as the zone's previous update, so skipping
  updates is never identical.
- **What each update did that the result does not need:**
  (1) AMD's FSR3 FX system `PostRenderOpaque` (vtable slot exe+0x799AF18) re-creates its `FFXFSR3SceneColorPreAlpha`
  texture whenever a renderer's scene colour has another size: once at the water size, then again at full size for the
  next main view (97 % of the callback is `RHICreateTexture`). The copy is read only by FSR3's upscaler; the water views
  use no anti-aliasing method and are never upscaled, and the main view copies its whole scene colour over the texture
  before its upscaler reads it.
  (2) A scene renderer created on its own defaults to `bIsFirstSceneRenderer = true` (SceneRendering.cpp), so each water
  renderer calls `EndFlushResourcesRHI` (exe+0x26B77C0): the render thread waits until the RHI thread has translated
  everything queued. UE's own non-first view families of a frame skip that flush; the main renderer still flushes once
  per frame.
  (3) With the flush gone the wait moved to `FSceneRenderer::WaitOcclusionTests` (exe+0x22EC8E0): each renderer waits on
  the occlusion fence the previous renderer pushed. The fence protects reads of occlusion-query results, and every read
  goes through the view's `FSceneViewState`; the water views have none (WaterInfoRendering.cpp), so they read no results.
  The renderers still push their own fences, so the main view still waits on the latest one, which follows its own
  previous-frame queries in RHI order. A renderer with several view families switches the skip off for the session.
  (4) `FEnableWaterInfoMeshesScoped` (exe+0x42B0540) runs twice per update and reads every scene proxy's
  `PrimitiveComponentId` (a cache miss per heap object) to find the few in the view's `ShowOnlyPrimitives` set. `FScene`
  keeps the same ids packed index for index in `PrimitiveComponentIds`; both arrays change only together inside
  `FScene::UpdateAllPrimitiveSceneInfos`, and the constructor runs outside it.
- **The change:** the water-info render's entry is detoured to mark a thread-local scope. Inside it only: (1) the FSR3
  callback returns at once, (2) `EndFlushResourcesRHI` returns at once, (3) `WaitOcclusionTests` returns at once, and
  (4) the mesh scan reads the packed ids with the stock set lookup and runs the game's constructor on a stand-in scene
  that lists only the matching proxies, so the game's code does its own test, enable and append. Per call it falls back
  to the game's scan when the scene's two array getters are not as expected, the arrays differ in length, or the first
  or last packed id is not its proxy's own id. Each function is found through its single caller (or its unique body)
  and its first bytes are checked; any miss leaves that part off. Installed at start-up (version.dll route) when any of
  the four keys is on, then live.
- **Why identical:** reviewed independently from the UE 5.3 source for each key. A forced re-draw of every water zone
  with the keys off and on gave bit-identical raw texture values (10 zones, 32 x 32 and 256 x 256); the maintainers'
  check mode of the mesh scan compared every fast scan with the game's (no difference, no fallback); screenshots of
  the bridge and the lakes: the same within the run-to-run variation.
- **Results** (showcase flight, the test PC's play settings: 4K, FSR Performance, frame generation, 60 fps cap; one
  held game, keys switched live): `WaterCaptureFsrSkip` + `WaterCaptureNoRhiFlush`, A,B,A,B: render-thread-bound time
  over 16.7 ms 889 / 588 -> 411 / 368 ms per flight, frames of 33 ms or more 67 / 37 -> 37 / 28, all frames' time over
  16.7 ms 3,731 / 2,958 -> 2,886 / 2,558 ms; the water-info frames' share of the slow frames' excess 22-24 % -> 15 %.
  `WaterCaptureNoOcclusionWait` on top, A,B,A,A,B: render thread per water update 7.5 -> 5.1 ms (1,183 -> 799 ms per
  flight), render-thread-bound time over 16.7 ms 318 -> 223 ms; frame totals within the run-to-run noise.
  `WaterCaptureFastMeshScan`: the constructor's own time was measured with a timer on maintainer slices of this flight
  (not in the public benchmark), so no number is quoted here; it is small next to the three others.

<a id="raytracingbuildbatch"></a>

### New objects' ray-tracing structures built together (`RayTracingBuildBatch`)

[Explanation](how-it-works.md#raytracingbuildbatch).

- **Found (2026-10-21, Imperial City flight, 4K, DLSS Performance, RTX 4090):** the climb above the Niben has one frame
  (97.6 s into the flight) with 42-50 ms of GPU busy against ~14 ms around it, in every pass; no engine setting moved it
  (ray tracing of skinned meshes and landscape, skin cache, water, scalability groups). A PIX capture of that frame,
  replayed with timings: ~145 back-to-back acceleration-structure builds for newly loaded static meshes, each after its
  buffer uploads and followed by a UAV barrier, 65-350 us each whatever the size (12 to 7.6k triangles), no overlap.
  The flight's 41.2 s frame is the same pattern (a 35-build block).
- **Where:** `FRayTracingGeometry::InitRHI` (exe+0x26823C0) -> `CreateRayTracingGeometry` (exe+0x266FBA0, priority
  Normal) -> `RequestBuildAccelerationStructure` of the geometry manager (exe+0x26C47C0). With
  `r.RayTracing.Geometry.MaxBuiltPrimitivesPerFrame` <= 0 (the game: -1) or priority Immediate it builds at once on the
  immediate command list (its own scratch buffer, then a barrier); otherwise it queues the request, and
  `ProcessBuildRequests` (exe+0x26C2760) builds the queue as one batch at the start of the scene's ray-tracing update
  (exe+0x1D51E60, called by `FDeferredShadingSceneRenderer::Render`, exe+0x1D5BC90). A budget is not an option: with
  50000 the game hit DEVICE_HUNG in 3 of 3 runs.
- **The change:** `CreateRayTracingGeometry`'s call goes to a gate. While the switch is on, a Normal request from
  InitRHI, on the render thread outside a scene render, with the budget <= 0, for a geometry that is neither fast-build
  nor allow-update (the skin cache refits those before the batch would run), enters the request function past its
  "build at once" test, on its queue path; everything else calls the game's function. `Render`'s vtable slot holds a
  wrapper that, when requests are queued, first calls `ProcessBuildRequests` on the frame's command list. Checked
  before writing: the call site's shape, the request function's prologue and its budget test, the ray-tracing update's
  call to `ProcessBuildRequests`, `Render`'s single vtable slot in .rdata; anything else: not installed, one log line
  (the self-test changes one byte of the request function and checks that). Installed at start-up on the version.dll
  route; on the OBSE64 route (loaded after the game's code started) it stays off with one log line. Then live.
- **Why identical:** the same geometries get the same builds (inputs, flags, build mode), in the same frame, before the
  frame's ray-tracing gather; a geometry released while queued drops its request (the engine's `ReleaseRHI`). An
  offline D3D12 test with the frame's meshes: 71 builds one at a time 16.0 ms, as one batch (one barrier) 0.8 ms.
  Image check (`water-visual`: the climb flight, then two hovering views of the Niben; switch off twice and on twice,
  WaterRebuildSpread on in all four): the same as the off-versus-off floor (niben-bridge SSIM 0.99; niben-above 0.79
  against 0.78 between the two off runs, which differ by fine outlines along every edge of the frame).
- **Memory, and a cap:** with no scene render (a loading screen) the queue held a whole load for one call: 2,445
  builds and 1.23 GB of scratch at an Anvil dock exit, one 52-117 ms call on the render thread (measured by LTSharpe on
  an RX 9070 XT, where the video memory peak stayed 11.5-11.6 GB and the stock game ran those builds' GPU work in one
  frame as well, ~450 ms against ~55 ms batched). So a request that would take the queued builds' scratch past 64 MiB
  first has the queue built: in the gate, before it is queued (the request index Create stores in the geometry at
  +0x9C then stays the queued one), with the game's `ProcessBuildRequests` on the immediate command list that the
  "build at once" path uses there. The queue is the manager's own request array (count +0x30, free count +0x5C,
  entries +0x28 of 0x18 bytes with the geometry at +8, allocation bits +0x38 / +0x48, as the queue path writes them,
  checked at install): a request the engine drops is not counted. A batch's scratch is one buffer (the engine splits a
  batch only at 2 GB); stock makes one per build, each living until the GPU has finished. Not measured on an 8 GB card.
- **Results** (1.2.1 base + this switch only, stock Engine.ini, the runner's pinned runs, profile `promo-dlss`: 4K,
  DLSS Performance, ray tracing mode 3; RTX 4090): `showcase`, a fresh game per run, off / on, three rounds (the first
  side alternating; `tools\Run-Benchmark.ps1 -Scenario showcase -Profile promo-dlss -Pass perf -FixIni @{
  RayTracingBuildBatch = 0 }`, then `= 1`). GPU busy (PresentMon) of the 97.6 s frame 37.1 / 45.1 / 31.9 -> 18.8 /
  20.0 / 21.0 ms, of the 41.2 s frame 52.1 / 67.3 / 62.6 -> 22.1 / 20.4 / 23.9 ms; frames >= 33 ms 158 -> 133 per
  flight, hitches 47.7 -> 44.3, fps 54.5 -> 55.4, 1% low 25.9 -> 26.4. (In one on run PresentMon had no GPU data for
  30 frames at 99.2-99.8 s and then booked 320 ms on one 28 ms frame: left out.) In one game on the 28 s
  `showcase-climb` cut, three passes each (a build with this series' other switches on in both sides): the spike
  frame's GPU busy 51.7 / 50.3 / 36.8 -> at most 19.3 ms. With
  Lumen's hardware ray tracing off (profile `cpu-nort`) the game still builds these structures; the switch batches them
  (1,841 builds in the first minute, 13 batches, longest call 5.4 ms) without errors.

<a id="physanimnoemptylock"></a>

### No exclusive physics lock for animation parts with nothing to move (`PhysAnimNoEmptyLock`, 1.2.3)

- **What the game does:** each character's `UPhysicalAnimationComponent` ticks in TG_PrePhysics and calls
  `UpdateTargetActors` (exe+0x354EA80), which takes the physics scene's write lock, copies the mesh's bone-space
  transforms and loops over `DriveData` (the bodies a physical-animation profile drives). In ~600,000 calls (Brindle Home
  corral, its control spot, `actors-battle`) `DriveData` was always empty. At the corral the horses' Control Rig traces
  hold read locks on helper threads, so each empty call waited ~38-41 us (6 us elsewhere): ~0.9 ms per frame for 23
  components, and the waiting writer held up the helpers' next traces.
- **The fix:** return at once when `DriveData.Num() <= 0` and the mesh has no pending parallel animation task (the
  lambda's first call, `GetBoneSpaceTransforms`, would complete such a task; possible on the teleport path). Identical:
  without a pending task the skipped code only locks (a TLS recursion count and an SRW lock), fills and frees a
  temporary and loops zero times. Found by pattern (entry, the lambda's `DriveData` compare, the pending-task compare);
  any mismatch leaves it off with a log line. Start-up only (version.dll route), then live.
- **Measured** (1.2.3 candidate, Ryzen 7 5800X + RX 9070 XT, `playlog-spots` scene `brindle-corral-hold`): `small`, live
  A/B in one game: TG_PrePhysics -0.98 ms, frame -0.59 ms. 4K, FSR Performance, frame generation, 60 fps cap, together
  with `RigTraceReuse`, live ABBA x2: game-thread work 11.02 / 10.48 / 10.24 / 10.59 -> 9.68 / 8.85 / 8.79 / 9.36 ms
  (-1.41), period 16.99 -> 16.74 ms (excess over 16.7 ms 0.29 -> 0.04, 87 % removed). Riding among the horses was
  measured too, but without a public way to mount a horse in the benchmark yet, so we give no number.

<a id="rigtracereuse"></a>

### A horse's repeated ground traces answered once (`RigTraceReuse`, 1.2.3)

- **What the game does:** the horses' Control Rig runs `FRigUnit_SphereTraceByTraceChannel` (exe+0x4B83E90, a
  `SweepSingleByChannel` against the ground) as a pure node that the rig VM re-runs for each consumer of its result.
  At the corral each horse made 106 traces per frame, 91.5 % of them bit-identical repeats of an earlier trace of the
  same rig run, on the same thread, 20 us to 2 ms later (~9 distinct traces, each ~12 times): 6-10 ms of helper time
  per frame, each trace holding the scene's read lock.
- **The fix:** inside one `UControlRig::Execute` (exe+0x4AB6250; a per-thread generation bumped at its entry and exit),
  a trace whose inputs (rig context, owner component, owner actor, world, start, end, radius, channel and the
  rig-to-world transform) are bit-identical to an earlier one of the same run gets that trace's outputs (hit flag,
  location, normal). Anything else runs the game's trace; nothing is kept across runs.
- **Equivalent, not identical:** the repeat asks the same scene the same question microseconds later on the same
  thread; a different answer needs a body write from another thread in between, an interleaving the game does not
  order. One caveat: if such a write lands between a trace and its repeat, another trace made in between can already
  see the new state while the repeat keeps the old answer, a combination the game would not produce in that order;
  it is the same unordered race the game already varies from frame to frame. Verify mode (`RigTraceReuse=2`, the
  repeats run and are compared): 0 differences in ~536,000 repeats standing at the corral, ~960,000 riding among the
  horses and ~1.09 million with wolves and a troll fighting by them.
- **Measured** (`small`, live ABBA x2 at `brindle-corral-hold`, with `PhysAnimNoEmptyLock` on): standing -0.53 ms
  frame, -0.47 ms TG_PrePhysics; the game thread's scene-lock waits
  0.57-0.86 -> 0.08-0.13 ms per frame, the helpers' read-lock waits 2.0-2.9 -> 0.2-0.4 ms. The NPCs' rig trace (another
  unit) repeats 67-75 % in crowds too, but reusing it showed no measurable gain: not shipped.

<a id="chaosmaxpayloadsize"></a>

### Large collision shapes filed in the search tree (`ChaosMaxPayloadSize`, 1.2.3)

- **What the game does:** every Chaos scene query (`TAABBTree` sweep, overlap, raycast; game thread, helpers' rig
  traces, the physics thread's broad phase) first tests every element of the tree's `GlobalPayloads` list: elements whose
  bounds exceed `p.MaxPayloadSize` (100000 cm). This game's static tree holds ~730 of them (~713 columns with z exactly
  -80000..+80000 cm, 4-50 m wide, spread over the whole map, and a few boxes up to 8 km), so each query made ~730
  pre-filter calls and bounds tests before the tree found ~14 candidates: 117-154 thousand list tests per game-thread
  frame at the Imperial City Waterfront, Vilverin and the Skingrad forest, 95 % of all elements visited.
- **The fix:** `p.MaxPayloadSize` 100000 -> 10000000 at start-up, before the engine reads its settings and before any
  physics scene exists (both routes), only while it holds the game's 100000; an Engine.ini line is applied later and
  wins; the trees read the value when they are created. Found through the console variable's registration (found once,
  value in .data, read elsewhere in the code); otherwise off with a log line. The live value is logged.
- **Equivalent, not identical:** the list loop and the tree's leaves run the same pre-filter and the same bounds test on
  the same stored bounds and call the visitor with the same payload; tree nodes only skip elements whose bounds miss the
  query. So overlaps visit the same set and sweeps/raycasts keep the same closest hit; only the visit order changes
  (stock visits the big elements first). That matters only for an exact tie: a raycast starting inside a column and
  another body at once (distance 0) stops at the first one visited. The columns' distance-0 stops kept the same share of
  raycast visits (0.278 % stock vs 0.287 % with the fix, Vilverin + `sk-d`); ties among the tree's own bodies already
  depend on its shape, which the game varies as cells stream in. Overlap candidates per overlap query 2.766 vs 2.756.
- **Measured** (1.2.3 candidate, Ryzen 7 5800X + RX 9070 XT). `small`, live ABBA: game-thread ms per frame -0.65
  (`skingrad-spots` `sk-d`) and -0.72 (`ic-spots` `ic-vilverin-pass`); query time on the game thread 3.07 -> 1.32 ms per
  frame, elements per query 385 -> 14. 4K, FSR Performance, frame generation, 60 fps cap, start-up key A/B/A over three
  launches: game-thread tick work 9.36 / 9.81 / 9.37 / 10.14 -> 8.11 / 8.85 ms (`sk-d`, -1.19) and 8.60 / 8.72 / 8.99 /
  9.02 -> 7.60 / 7.61 ms (Vilverin, -1.22); time over 16.7 ms per frame 0.51 -> 0.25 (-50 %) and 0.83 -> 0.56 (-32 %).
  Doors (tree rebuilds with the extra leaves), `small`, A/B/A: no added hitch. End screenshots: same positions within 1 cm.

## Travelling: memory and threads

<a id="lumenkeeparraycapacity"></a>

### Lumen's object lists keep their memory (`LumenKeepArrayCapacity`)

[Explanation](how-it-works.md#lumenkeeparraycapacity).

- **Found:** while travelling (a forest route north and south at running speed, 854x480, CPU-limited) slow frames
  repeat at the same places. A stack sampler covering every game thread (`tools/FastSampler.cs`) shows the render thread
  waiting 25-37 ms in Lumen's scene update for a helper that is copying memory.

- **Cause:** `FLumenSceneData::ResetAndConsolidate` (`exe+0x1F8B990`, once per frame in the Lumen scene-update task the
  render thread waits for) consolidates five `TSparseSpanArray`s. Consolidating calls `SetNum` with shrinking allowed,
  so when streaming frees entries at the end of an array, the whole array is reallocated to fit exactly; the next
  addition past the end grows it again by 37.5%. The primitive-group array has an 80-byte entry for every unmerged
  instance Lumen tracks, so each of these copies a large array into freshly committed memory (in 5.3 the allocator
  also holds its global lock around the OS allocation and release). Four such copies on the route, each a visible hitch.

- **The change:** the consolidate step's two `RemoveAt` calls get "don't shrink" and its three `ResizeShrink` calls
  are skipped (calls redirected to stubs; found by the shrink-call pattern, each target checked to be `RemoveAt` or
  `ResizeShrink`). The arrays hold the same entries; only unused capacity stays reserved, up to the largest size
  needed so far.

- **Grow policy (same fix):** the primitive-group array reaches 1.5-2.1 million entries (120-170 MB). It only grows at
  one place, the inlined `AddSpan` in `UpdateLumenScenePrimitives` (`exe+0x1F97902` -> `ResizeGrow` `exe+0xE79870`).
  That call is redirected: the game's `ResizeGrow` sizes the block from `Num`, so the hook lets it size it from a larger
  number and puts `Num` back (capacity only). Policy: at least double; and a new Lumen scene (each exterior arrival
  starts from an empty one) that reaches an eighth of the session's peak entry count goes straight to the peak + 1/4.
  Result: no grow copies on the route in a fresh session (stock: one of 44-48 ms right at the NPC burst), one instead of
  four at a door exit (9 ms of copying instead of 64 ms).

- **Result:** the Lumen spots disappear (hitch time per spot, stock / fix: 73 / 0, 22 / 0 ms; mixed spot 64 / 34);
  no visible change (visual pass, four scenes).

<a id="uploadmemorykeep"></a>

### Upload memory kept between streaming bursts (`UploadMemoryKeep`)

[Explanation](how-it-works.md#uploadmemorykeep).

- **Found:** at other spots the render thread waits 25-41 ms either inside a new D3D12 upload allocation (distance-field
  brick upload) or in the render graph's setup waiting for a task that waits for the upload allocator's lock, while one
  thread is in the graphics driver creating and mapping a new upload buffer.

- **Cause:** `FD3D12Adapter::EndFrame` (`exe+0x24FFDA0`) cleans the upload heap allocator's three pool allocators with
  a fixed lag of 20 frames, and the fast allocator keeps only `d3d12.FastAllocator.MinPagesToRetain` (5) of its 4 MB
  pages. Streaming comes in bursts a few seconds apart, so each burst creates its upload pools again (committed upload
  resource + map in the kernel), partly under the fast allocator's lock.

- **The change:** the three clean-up calls get a lag that never expires (a lag that lasts for the session) instead of 20, and the page count is raised to the retained page count (24: travel never needed more than that, measured with a live page counter; keeping 64 cost an extra 160 MB). Memory is kept, never
  requested beyond what the game itself asked for at its peak, and reused by later bursts: +0.04-0.46 GB committed
  over a route and four transitions, working set unchanged, no growth. A 3600-frame limit was dropped: the pools of a
  save load then expired together a minute later and were destroyed in one frame on the RHI thread under the
  big-block lock (~140 ms of driver calls, the render thread waiting ~60 ms).

- **Result:** the upload spots go (110 -> 14 ms hitch time at the worst one); south leg hitch time with `LumenKeepArrayCapacity` and `UploadMemoryKeep`
  573 -> 210 ms at the 60 fps cap.

- **Not to do:** `r.DistanceFields.UseNewAtlasUploadBufferShrink=0` makes it much worse: the volatile brick upload
  buffer then stays large and every frame with an upload takes all of it from the upload heap.

<a id="gpuscenekeepcapacity"></a>

### The GPU's object lists keep their size (`GpuSceneKeepCapacity`)

[Explanation](how-it-works.md#gpuscenekeepcapacity).

- **Found:** on a night flight from Roxey Inn (4K, FSR Performance, frame generation), the same spot 2,500 units west
  of the inn gave the run's worst frame in two runs: 109 and 118.6 ms. The render thread was in `GPUScene.Update` ->
  `FGPUScene::UpdateBufferState` (`exe+0x1DD8D40`) -> `ResizeStructuredBufferSOAIfNeeded` (`exe+0x270CB60`) -> a D3D12
  committed-resource creation in the kernel (15 of 45 stack samples; the rest waiting on tasks), the game thread
  waiting at frame sync: `GPUScene.InstanceSceneData` growing to the next power of two, 2M -> 4M instances
  (96 -> 192 MB). In two other runs of the same routes the same growth cost 22-34 ms: the kernel allocation's cost
  varies run to run.

- **Cause:** UE 5.3 sizes every GPU-scene buffer (primitive data, 672 bytes per primitive; instance data, whose
  reserve is also the SOA stride every shader reads; instance payload; BVH; lightmap) to the next power of two of what
  it holds and re-creates it whenever that changes, in both directions (a new RDG buffer plus a GPU copy: a fresh
  block of video memory while the frame waits). The update runs twice per frame (the main update `exe+0x1DD87BF` and
  the per-view dynamic-primitive upload `exe+0x1DD8D40`), and the two disagree about the small payload buffer: it
  flips between 4 KB and 8-16 KB every frame, ~2,100 re-creation pairs per minute. On these routes the big instance
  buffer only ever grew (2M -> 4M); it did not shrink even after a forest-to-city teleport.

- **The change:** the instance reserve is changed where the game computes it (a thunk at `exe+0x1DD8913` /
  `exe+0x1DD8E9E`, both instances or none), so buffer size and stride always agree; `GPUScene.PrimitiveData` and
  `GPUScene.InstancePayloadData` are held through their `ResizeStructuredBufferIfNeeded` calls (`exe+0x1DD88C0` /
  `exe+0x1DD8E51` / `exe+0x1DD899E` / `exe+0x1DD8F29` -> `exe+0x270CB10`, found by their name strings). Policy per
  buffer: keep the capacity while the need stays above a quarter of it; never shrink a buffer under 4 MB (ends the
  payload flip); a grow past an eighth of the session's largest size goes straight to it (`LumenKeepArrayCapacity`'s rule). Every change
  is logged (`[gpuscene]`, with per-minute counts).

- **Check:** with every GPU-scene buffer held at twice the size the policy chose, so the instance stride differed from
  the game's own value everywhere, the image comparison matched the stock game (forest at noon: SSIM 0.965 against a
  0.941 stock-vs-stock floor; Roxey at night: 0.984 against 0.954). In a session that returns to the main menu between
  worlds, the instance buffer shrank 4M -> 1M (the need had fallen to a quarter, a real shrink) and grew straight back
  to 4M in one step 4 s later instead of doubling.

- **What it does not fix:** the first growth to a size the session has not had yet, which is what the 109/118 ms frame
  was. It removes the repeats (shrink and regrow, the per-frame payload flip, the doubling steps after a fast travel
  or door), which these one-way benchmark routes do not produce; its effect over a long play session is not measured
  yet.

<a id="rtscenebufferkeep"></a>

### The ray-traced scene's list keeps its memory (`RtSceneBufferKeep`)

[Explanation](how-it-works.md#rtscenebufferkeep).

- **Found (2026-10-20, 4K at the test PC's play settings, horse routes):** the render-thread stalls of the worst frames
  were the render thread in `NtGdiDdDDIWaitForSynchronizationObjectFromCpu` under D3D12 resource creation, called from
  `FRayTracingScene::Create` (exe+0x2208A30, UE 5.3 RayTracingScene.cpp:88) creating the TLAS result buffer
  "FRayTracingScene::SceneBuffer"; the GPU was idle meanwhile (its paging fence while video memory made room). UE 5.3
  re-creates that buffer when the need is above its size or below half of it (16 MiB steps): it dropped after every
  scene start and grew back in several creations, at the same places on every pass. Each creation is normally a few
  milliseconds; the stalls occurred only near the video memory budget.
- **The change:** the check `cmp; ja grow; shr rax,1; cmp; jae keep` becomes a call to a stub that answers "keep" for the
  shrink case (the growth case and the registers as in the game). The three `RHICreateBuffer` calls of `Create`, recognised
  by the debug names they pass, are timed for the stats line (a creation is logged only when the TLAS changes size or a
  creation takes 1 ms or more, 5 ms for the upload buffers). Start-up install; then 1 = keep, 0 / 2 = the game's way.
- **Why identical:** the TLAS is built at the start of the buffer it is given and the ray tracing layers' views use the
  scene's own offsets, so the acceleration structure is the same; screenshots of the same flight with and without the
  fix showed no difference beyond the flight's own end-point variation. Extra video memory: at most the session's largest
  TLAS size minus the current need.
- **Results:** with the fix the buffer was kept instead of shrinking for over a thousand frames per pass and not
  re-created again within a pass; on a loading screen the engine drops it (the "no buffer" path), so the first growth
  after a door still happens, now in one creation. The routes used are maintainer routes that are not in the public
  benchmark, so no frame numbers are quoted here. Self-test: the site, the patch and the stub's decisions in both modes.

<a id="naniteinitialrootpages"></a>

### Nanite's page block starts big enough (`NaniteInitialRootPages`)

[Explanation](how-it-works.md#naniteinitialrootpages).

- **Found (2026-10-22, showcase flight at the test PC's play settings, fresh game processes):** a freeze of up to
  ~1.7 s about 45 s into the Imperial City flight, just past the Talos bridge, always at the same place. In the long
  frame the render thread was in `Nanite::FStreamingManager::ProcessNewResources` (exe+0x35D2FA0, UE 5.3
  NaniteStreamingManager.cpp) -> `ResizeByteAddressBufferIfNeeded` -> `CreateCommittedResource`, waiting on the paging
  fence, and the GPU then needed up to ~1.2 s more; video memory rose by ~0.55 GB at that moment and fell back. The
  buffer is ClusterPageData: the 512 MiB streaming pool plus root pages x 32 KiB, root pages = max(
  `r.Nanite.Streaming.NumInitialRootPages` (2048), the root-page allocator's size rounded up to 2 significant bits). The
  first time a process needs more than 2048 root pages it makes a new 608 MiB buffer beside the 576 MiB one and copies
  it on the GPU. Once grown it does not grow again in that process, which is why runs in one held game never froze.
- **The change:** at start-up the value of `r.Nanite.Streaming.NumInitialRootPages` becomes 3072, the first growth step
  (2048 rounded up to 2 significant bits, just past it), so the buffer starts at 608 MiB and that re-creation is never
  needed; past 3072 root pages the engine grows it as before. The variable is found through its registration and
  checked against `ProcessNewResources`' read of it, and is written only while it holds the built-in 2048 (an Engine.ini
  line, applied later, still wins). It is the same as `r.Nanite.Streaming.NumInitialRootPages=3072` under
  `[SystemSettings]` in Engine.ini (tested that way too: the same buffer sizes). The version.dll route writes it before
  the variable is registered; the OBSE64 route runs the start-up after the static initialisers and before `WinMain`
  (no ini read, no renderer yet), and writes the same int (the console variable's displayed value stays 2048 there).
- **Why identical:** root pages are addressed from `MaxStreamingPages` (`GPUPageIndexToGPUOffset`), not from the buffer's
  size; the bigger buffer only has unused space at its end. The other users of the count are `RootPageInfos` and a
  stat. Visual pass (small profile, Cheydinhal canal and chapel square at noon): the same.
- **Cost:** +32 MiB of video memory from the first area on (stock reaches the same 608 MiB at the bridge). 4096 would
  also skip the next step, for +64 MiB.
- **Results** (showcase, the test PC's play settings, a fresh game per run): stock, five runs: long frames adding up to
  952, 1,739 (1,416 + 323), 1,333 (731 + 602) and 121 ms at the spot, and once 19 ms (the re-creation is deterministic,
  its cost depends on how full video memory is); with 4096 and with 3072, one run each: the buffer was never
  re-created, no freeze, worst frames of the flight 119 and 66 ms.

<a id="instanceclearreserve"></a>

### Clearing grass without re-copying its list (`InstanceClearReserve`)

[Explanation](how-it-works.md#instanceclearreserve).

- **Found (Echo Mine -> Brotch Camp):** under a 60 fps cap the walk had a 56-64 ms frame at ~11.7 s
  in both runs: ~22 ms of the game thread copying inside `FMallocBinned2::Realloc`, called from
  `UHierarchicalInstancedStaticMeshComponent::ClearInstances` (`exe+0x31FA640`): it calls
  `FInstanceUpdateCmdBuffer::Reset` (`exe+0x325A7B0`, `Cmds.Empty()`, frees the list) and then `HideInstance`
  (`exe+0x3254850`, `Cmds.AddDefaulted_GetRef()`, 0x150-byte commands) once per rendered instance; the list grows by the
  default policy (+3/8 + 16) through `exe+0x2298580`, i.e. ~30 reallocations and copies for 29k instances.

- **The change:** the call to Reset at `exe+0x31FA7CE` is redirected (rel32, written before the game's code runs) to a
  stub that calls the game's Reset and then sets the list's capacity to the loop's own bound,
  min(`[[comp+0x6D0]+0xC0]+0x40` render instances, `[comp+0x668]` instances), through the engine's allocation helper
  (`exe+0xDE6200`, as the grow function calls it). Only for 256+ commands. Same commands, same order; less spare capacity.

- **Results:** 15 clears per walk (122,457 commands, the largest 29,458). Capped walk A/B, one game process, 3+3 runs: time
  over 25 ms per walk 69-131 -> 46-57 ms, frames >= 25 ms 17-22 -> 12-14, mean frame 17.0-17.4 -> 16.8-16.9 ms (cap 16.7).

<a id="ospagecache"></a>

### Spare memory blocks reused (`OsPageCache`)

[Explanation](how-it-works.md#ospagecache).

**Measured (2026-10-16, with every call traced, Kvatch spot C flight in a new world, 480p, one running game,
ABBA):** page faults during the flight halved (19,000 -> 9,800 a second); in the 7.2 s around spot C the collision
index passes took 14-16 ms instead of 24-26, the trees' growth 9-12 ms instead of 17-19, the time over 16.7 ms
319-326 ms instead of 526-547, frames over 25 ms 7-9 instead of 12-15, the worst frame 38-41 ms instead of 48-49.
Cheydinhal canal at noon: page faults 4,800-5,000 a second instead of 105,000-116,000, but no measurable change in
frame rate or thread times there. At 4K with the test PC's own play settings (60 cap, frame generation) the index passes and tree
growth were again shorter (cisa 16-19 vs 19-23 ms, growth 10-13 vs 13-21 ms) but the frame counts stayed within noise.
Memory: the game's private bytes and working set rose by about the cap (0-60 MB) or less.

<a id="navmeshbuildnobusywait"></a>

### Navigation tiles built only by free threads (`NavmeshBuildNoBusyWait`)

[Explanation](how-it-works.md#navmeshbuildnobusywait).

- **Found:** the worst of a sampled 4K run's 12 long presents (83.7 ms) had the game thread waiting ~28 ms in
  `FTickTaskSequencer::ReleaseTickGroup` (`exe+0x37C8160`) -> `FTaskGraphImplementation::WaitUntilTasksComplete`
  (`exe+0xDD66B0`), the wait for a tick group's parallel tasks, with every helper but one idle. That helper had run
  over 100 ms of Altar's NPC steering tick: a path-follow nav query issued a `UE::Tasks` busy wait (`exe+0xE595C0`) for
  a per-navmesh-data task, and while waiting the helper had picked up a queued-pool task it may not abandon: a Recast
  navmesh tile build (`RecastBuildLayers` `exe+0x41FE7C0`, `rcBuildHeightfieldLayers` `exe+0x41810C0`), 20-110 ms each.

- **Cause:** a priority inversion. UE 5.3's scheduler runs any queued task flagged `AllowBusyWaiting` inside a busy
  wait, and the queued-thread pool clears that flag for work marked `EQueuedWorkFlags::DoNotRunInsideBusyWait`; the
  navmesh generator starts every tile task without flags (`StartBackgroundTask`), in 5.3.2, 5.4.0 and 5.5.0 alike.

- **How often:** in that run, 76 of 1,898 helper samples inside Recast were nested in a busy wait, all in the steering
  tick's, and it produced the run's longest frame; in a sampled 854x480 run, 3 of 3,300 and none in a long frame; 11
  further sampled runs: none. Rare, but each occurrence is one frame as long as a tile build while the other helpers
  sit idle.

- **The change:** one byte. The tile generator's inlined task start writes priority and flags as one 16-bit constant
  (`mov word [rdi+28h], 3` at `exe+0x41FD57E`); the fix sets the flags byte at `exe+0x41FD583` to 1
  (`DoNotRunInsideBusyWait`). The site is found by the generator's checks, the inlined start, the pool call and the
  task's constructor and vtable.

- **Result:** the same tiles are built from the same data; only which helper builds them, and when within the frame,
  changes. In a sampled 854x480 run with the fix: 1,858 Recast helper samples, all at the top of a helper, none inside
  a busy wait, and no long present with a task nested in a tick (the one nested sample left is a steering tick running
  a Detour path query inside another's wait, not a tile build).

<a id="busywaitflagcheck"></a>

### The navigation wait check completed (`BusyWaitFlagCheck`)

[Explanation](how-it-works.md#busywaitflagcheck).

**Measured:** Imperial City flight, small profile: tiles built inside a wait 6 in 10 flights without the fix (frame stalls of 82 ms to 2.6 s), 0 in 7 with it (largest tick-group-0 wait 29 ms); 4K FG test completed without a crash.

<a id="navlinkfast"></a>

### Navigation tiles linked with less searching (`NavLinkFast`)

[Explanation](how-it-works.md#navlinkfast).

- **Found (2026-10-23, a maintainer road route at the test PC's play settings):** some slow frames' tail was level
  streaming's `AddToWorld`, and its largest single items were `ANavigationDataChunkActor::BeginPlay`:
  `UNavigationSystemV1::AddNavigationDataChunk` -> `ARecastNavMesh::OnStreamingNavDataAdded` -> `AttachTiles` ->
  `dtNavMesh::addTile` per tile (UE 5.3 DetourNavMesh.cpp). `addTile` calls `connectExtLinks` for every other layer of the
  tile's cell and every layer of the 4 neighbouring cells, both ways; each call walks the tile's polygons and, for each
  portal edge on the side, calls `findConnectingPolys`, which walks all polygons of the target. A probe counted many
  `connectExtLinks` calls per chunk, most of which could not make a link (the tile had no portal edge for that side,
  or the target had no polygon facing back). One chunk attach is one actor, so the engine's time-slicing of
  `AddToWorld` cannot split it.
- **The change:** per tile, one walk (cached for the chunk attach, keyed by the tile's slot, salt, header and polygon
  count) notes the sides its portal edges point to and, per side, the polygons with an edge marked exactly for it.
  (1) A `connectExtLinks` call is left out when the tile has no portal edge the call would look at, or the target has
  no polygon marked for any side those edges would search: then every search the stock call makes returns nothing, so
  it makes no link and changes nothing. (2) A call that is made searches through `findConnectingPolys`' one call site:
  the game's own `findConnectingPolys` runs once per listed polygon on a copy of the target's tile descriptor that
  starts at that polygon, so it does exactly the per-polygon work of its own loop; each entry's polygon reference is then
  written as the stock code computes it for the real tile. Tiles with polygons of more than 6 vertices and odd sides take
  the stock path. The four call sites in `addTile` and the one in `connectExtLinks` are checked before they are written,
  only while the game starts (version.dll route).
- **Why identical:** the maintainers' check mode compared every fast search with the stock search in the game, byte
  for byte (no difference), and made every left-out call anyway (none made an entry); the self-test does the same on
  thousands of random tiles, against a brute-force run of every search the stock call would make.
- **Results:** chunk attaches became cheaper with the same links, and the frame totals moved within the run-to-run
  noise. The route is a maintainer route that is not in the public benchmark, so no numbers are quoted here.

## Travelling: physics, when a new area arrives

<a id="physicstreetimeslicing"></a>

### Physics search tree rebuilt a little at a time (`PhysicsTreeTimeSlicing`)

[Explanation](how-it-works.md#physicstreetimeslicing).

- **Found:** at one spot of the route the game thread waits ~30 ms in the physics step while a helper builds an AABB tree.

- **Cause:** above `p.Chaos.AccelerationStructureTimeSlicingMaxQueueSizeBeforeForce` (1000) queued changes, Chaos
  rebuilds its acceleration structure in one go instead of over several frames.

- **The change:** the limit is 100000 (the variable, found by its registration). The same tree is built, over more
  frames; until then queries and collision detection use the current tree plus the tree of changed bodies, so they find
  the same bodies.

- **Result:** that spot 48 -> 21 ms hitch time (two runs each).
- **Adaptive slices (Echo Mine -> Brotch Camp):** each slice of the rebuild handles
  `p.IterationsPerTimeSlice` = 4000 elements and the finished tree is copied to the game thread's structure
  `p.Chaos.AccelerationStructureTimeSlicingMaxBytesCopy` = 100,000 bytes per frame, so with a streamed forest a rebuild
  cycle lasts 38-131 physics steps; every change made meanwhile is queued and applied to both new trees in the step the
  rebuild finishes (`FPBDRigidsEvolutionBase::FlushAsyncAccelerationQueue`, PBDRigidsEvolution.cpp): 9,000-13,000
  changes, 25-44 ms while the game thread waits. The fix also sets the slices to 40000 elements and 1,000,000 bytes
  (both variables found by their registration; never below the game's values, read lazily because a constructor sets the
  first after the DLL starts). The copy slice adapts per step through a detour of
  `ComputeIntermediateSpatialAcceleration`: the game's 100,000 bytes while quiet (a cycle then lasts ~10 steps as in the
  stock game), the bigger slice from the 8th step of a cycle or with 2048 changes waiting. Cycles while streaming: 6-18
  steps; the largest catch-up 9.6-12.8 ms without `PhysicsFlushParallel`. `PhysicsTreeTimeSlicing` off (limit 1000) does not avoid the first big catch-up
  (28.6 ms).

<a id="physicsflushparallel"></a>

### Collision catch-up on two threads (`PhysicsFlushParallel`)

[Explanation](how-it-works.md#physicsflushparallel).

- **Found (Echo Mine -> Brotch Camp):** the worst frames of the walk (49-79 ms at the test PC's 4K play settings, 48-72 ms at 480p) are the game thread waiting for one physics step whose helper runs
  `FlushAsyncAccelerationQueue` (`exe+0x1931660`): for each queued change it updates the rebuilt physics-thread copy
  (AsyncInternal, `+0x2E0`, plus the spatial cache) and then the game-thread copy (AsyncExternal, `+0x2E8`), each an
  insert into the copy's dirty tree (~1.4 us).

- **The change:** an entry detour; for at least 1024 queued changes the game-thread copy's
  updates run on a helper thread with the game's own calls (the per-op block at `exe+0x1931B14` replicated: remove via
  vtable +0x80 for a delete, else update via +0x88 with the handle built from the particle arrays +0xE0 / +0x50, the
  query/sim filter helpers `exe+0x16F11F0` / `0x16F1260`, the bounds +0x170 and the bounds flag +0x188; dynamic-tree
  buckets skipped as the game skips them), while the game's function runs with an inert stand-in in the copy's slot and
  does the physics copy and the cache. The copy gets the step's timestamp (+0xC) afterwards. The two copies share no
  substructures (UE 5.3 copies dynamic trees with DeepAssign), the game-thread copy is used by nothing else until the swap,
  and the particle data is not written during the step.

- **Results:** 10 splits per walk, the largest 4,227 changes in 6.6 ms (helper 6.2 ms). With `PhysicsTreeTimeSlicing`'s adaptive slices
  (one game process, 3+3 runs taken in turn, 480p): the largest catch-up 7.3-8.0 ms instead of 31.6-35.9 ms
  without these two changes, worst frame 33-36 instead of 56-57 ms, no frame >= 50 ms (was 1-2), time over 25 ms in the
  spike window 17-45 instead of 63-81 ms, the same mean frame time (14.5 ms). A fixed 1 MB copy slice instead of the
  adaptive one cost ~1.5 ms of mean frame on the walk (a swap every step; the game thread re-applies its pending
  changes to each new structure).

<a id="parallelstaticbodies"></a>

### Collision for woods and rock fields on several threads (`ParallelStaticBodies`)

[Explanation](how-it-works.md#parallelstaticbodies).

**Measured (2026-10-14, 4K with frame generation, three fresh games, Kvatch walls flight):** below the north cliff
37.4 rendered fps with it against 35.2 / 35.6 without, frames of 25 ms or more 126 against 158 / 154, the same image
(visual comparison "same"); on the Brotch walk no measurable change. No crash in any run.

**Two parts of this fix:**

- *The helpers' own tool boxes.* The game's memory manager gives each of its own threads a
  private stock of small memory blocks; a thread without one has to queue at a shared counter for every block. `ParallelStaticBodies`'s helper threads (and `PhysicsFlushParallel`'s) are the DLL's own threads, so they queued there for every one of the many small
  pieces a physics body is made of, and mostly waited for each other: four helpers made the bodies only about 1.3 times
  as fast as the main thread alone. Each helper first asks the game for its own stock, exactly as the
  game's threads do. Only where in memory the pieces sit changes. *Analogy:* the helpers each get their own box of
  nails instead of lining up at the one box the foreman keeps.

- *Filing while numbering.* After the bodies are made, the game numbers each one in the physics
  world and then files it into the collision search index, body by body. Now the main thread numbers them while one
  helper files them, in the same order, each one only after it has its number; the physics world is locked against
  everyone else until both are done, so nothing can look at a half-filed index. The game's own code does both jobs.

Measured (2026-10-14, 480p, Kvatch flight past the north cliff, one fresh game per run, 2 runs each way): making the
bodies 2.6-2.8 -> 1.5 microseconds each (95-102 -> 54-56 ms per flight), adding them to the physics world 2.2-2.3 ->
1.8-1.9 microseconds each (70-75 -> 58-61 ms): about 55 ms less main-thread time per pass below the cliff. The same
bodies in the same order in every run (36,475 made, 32,482 filed, no order mismatch). The frames there are still
held up by the physics step that follows (inserting the same bodies on the physics side).

<a id="pipelinedbodycreation"></a>

### Collision bodies handed over in batches (`PipelinedBodyCreation`)

[Explanation](how-it-works.md#pipelinedbodycreation).

**Evidence.** The self-test checks ordering and results for 300 random meshes. An isolated timing measurement for this part is not available.

<a id="dirtytreequickinsert"></a>

### A quicker filing rule for new collision (`DirtyTreeQuickInsert`)

[Explanation](how-it-works.md#dirtytreequickinsert).

**Measured (2026-10-14, 480p, Kvatch flight past the north cliff, one held game, 3 runs each way interleaved):** the
physics step's index work below the cliff 51-58 ms instead of 88-96 ms, its longest step 23-29 ms instead of 38-41 ms,
adding the bodies on the main thread 1.0 instead of 1.8 microseconds each (the main thread waited 20 instead of 44 ms
for the inserts, with pipelined scene addition); below the cliff the worst frame 52-54 ms instead of 56-59 ms, frames of
33 ms or more 6-7 instead of 9-10, time over 16.7 ms 303-387 ms instead of 400-438 ms. Whole flight: time over 16.7 ms
363-471 instead of 490-504 ms.

<a id="bulkindexinsert"></a>

### New collision filed as one branch (`BulkIndexInsert`, `PhysicsInternalFlushParallel`)

[Explanation](how-it-works.md#bulkindexinsert).

**Measured (2026-10-15, 480p, Kvatch flight to the north cliff, one held game, 3 + 2 runs interleaved):** the physics step's index work below the cliff 33-42 ms instead of 55-58 ms (filing the new
bodies 0.55 instead of 0.95 microseconds per change; the largest catch-up step 7.5-11.8 instead of 12.7-13.5 ms). The
frames there do not get measurably better at 480p (time over 16.7 ms 257-310 vs 262-321 ms, worst frame 52-55 vs
50-51 ms): the main thread's wait for the physics step in those frames is now mostly an earlier, single-threaded part
of the step (the physics side of handing the new bodies over, 70-82 ms per arrival, single frames up to 13-16 ms),
which runs before the index work. `PhysicsPushParallel` targets that intake. At the test PC's own play settings
(4K, frame generation, 60 fps cap, one run each) the same: no change beyond noise. No regression on the Brotch and
Imperial City flights, the same path on foot through the streaming area, and the same image (SSIM 0.98).

<a id="physicspushparallel"></a>

### New collision taken in by the physics side in one go (`PhysicsPushParallel`)

[Explanation](how-it-works.md#physicspushparallel).

**Measured (2026-10-15, 480p, Kvatch flight to the north cliff, one running game per comparison, on/off interleaved):**
the physics side's intake below the cliff 26-28 ms instead of 59-60 ms per arrival (the biggest single frame's 6.2-6.5
instead of 12.6-13.6 ms; batching alone, without bounds on the helpers: 33-45 vs 69-83 ms). Frames below the cliff over
five on and five off runs: time over 16.7 ms 272 vs 323 ms (-16 %), frames of 33 ms or more 4.8 vs 6.6 per flight, worst
frame about the same (51-55 ms: what is left there is other work). No regression on the Brotch and Imperial City
flights; the same image (only the animated night sky differs between two captures).
At the test PC's own play settings (4K, FSR Performance, frame generation, 60 fps cap; one fresh game per run, two on, one off):
below the cliff time over 16.7 ms 369 / 374 vs 427 ms, frames of 33 ms or more 4 / 4 vs 6, the intake 30 ms per arrival
(biggest frame 7 ms).

<a id="physicsqueueshrinkonce"></a>

### Collision change list shrunk once (`PhysicsQueueShrinkOnce`)

[Explanation](how-it-works.md#physicsqueueshrinkonce).

**Measured (2026-10-15, Kvatch flight over spot C, 480p, one held game, five runs each way):** the main thread's
hand-overs at C 1-4 ms in all instead of 11-20 ms; each big one (10,000 changes) under 1-2.6 ms instead of 4-6.7 ms. The
frames that carry them 21-27 ms instead of 24-32 ms. At the test PC's own play settings (4K, FSR Performance, frame generation, two
runs each way) the main thread's hand-overs at C 1.3-1.4 ms in all instead of 9.5-15.1 ms; the frame totals there are
within the run-to-run noise. No change on the Brotch walk.

<a id="physicsdestroybatch"></a>

### Removed collision taken apart faster (`PhysicsDestroyBatch`)

[Explanation](how-it-works.md#physicsdestroybatch).

**Where the time went (2026-10-22, Imperial City flight, timers on the physics step's stages):** the main thread's
EndPhysics waits at the stone bridge were bodies being removed, not added: World Partition unregisters one big
instanced mesh (1,000-2,500 bodies) per frame, and each body costs the step ~1.8 us to disable and ~3.4 us to take apart
(1.44 us deleting its proxy object, 0.35 us rebuilding the particle views). Over the flight the destruction was 31 % of
the EndPhysics waits above a calm frame's.

**Measured.** 480p, one held game, four runs each way: 3.46 -> 1.88 us per body; at the stone bridge (outbound) the
EndPhysics wait in frames waiting 5 ms or more 120 -> 73 ms. At the test PC's own play settings (4K, FSR Performance,
frame generation, 60 fps cap; showcase flight, one held game, two A-B-B-A blocks, four flights each way): 3.67 -> 2.02 us
per body; EndPhysics waits 950 -> 767 ms per flight (878-1080 vs 738-795); their part in frames of 25 ms or more
365 -> 260 ms (328-442 vs 243-275), about 5.6 % of those frames' time over 16.7 ms; stone bridge (outbound): frames of
25 ms or more 14.0 -> 11.5, worst 41 -> 32 ms. Frame totals: time over 16.7 ms 6,119 -> 5,851 ms, frames of 25 ms or
more 149 -> 134 (within the run-to-run noise: one block showed the gain, the other none).

## Travelling: characters and cells

<a id="npcfadeinnorebuild"></a>

### Characters fade in without being rebuilt every frame (`NpcFadeInNoRebuild`)

[Explanation](how-it-works.md#npcfadeinnorebuild).

- **Found (2026-10-17, a player's report of more than 16 GB of video memory at Anvil's docks):** at every dock exit the
  game's DXGI usage rose 2-3.5 GiB within ~1 s, 2.5-5 s after arrival, and went over the budget for 2-2.75 s; UE 5.3's
  residency manager then evicted and made resident ~4 GiB in 2.5 s and frames took 420-620 ms with the GPU nearly idle
  (`D3D12.ResidencyManagement=0` removed the freeze but left memory demoted to system RAM). A D3D12 allocation trace
  named the rise: RDG transient heaps and buffer pools (~1.7 GiB, normal) plus +1.5 GiB of acceleration-structure pool
  from 698 skinned-mesh BLAS built in 2.5 s, each for a brand-new `FSkeletalMeshObjectGPUSkin`: ~160 components had their
  render state recreated ~50 times in 16 s.
- **Cause:** the remaster's `UPrimitiveComponent` fields `bDoDither` (+0x316) and `DitherScale` (+0x318), copied by the
  scene proxy at construction (+0x7D, +0x80) and read only by the primitive uniform / GPU-scene parameter builder. Their
  setters (`exe+0x3044B20`, `exe+0x3044B00`) call `MarkRenderStateDirty` on every change. `UVCharacterFadeInOutComponent`
  (tick `exe+0x48D5A40`) runs a fade-in for every spawned actor (5 -> 0 at 4 per second, then dither off) and sets the
  value on every primitive of the actor every frame.
- **The change:** the float setter's entry jumps to a hook. A changed value on a registered component with a live,
  not-dirty render state whose proxy matches the bool is stored, written to the proxy's copy (one aligned 4-byte store)
  and pushed with `FScene::UpdateCustomPrimitiveData` (the engine's per-primitive path; its apply step marks the
  uniform buffer and the GPU-scene entry dirty). Everything else, and every change of the bool, goes through
  `MarkRenderStateDirty` as before. Found by shape; the proxy offset is read from `UpdateCustomPrimitiveData`'s code.
- **Results:** ABBA on `anvil-docks` (4K user profile, fresh launch per arm): dock-exit VRAM peak 12.93-13.11 -> 11.23-
  11.49 GiB, over budget 2.0-2.75 s -> 0 s, skinned mesh objects per run 24,740 -> 2,441, after-arrival fps 18.5 -> 52.3
  (exit) and 46.3 -> 66.9 / 43.6 -> 72.8 (exit2), dock circuit 78.1 -> 95.4 fps (Render 5.61 -> 4.55 ms, RHI 8.74 ->
  6.68, Game 8.54 -> 7.31, GPU 12.38 -> 10.35), settled docks VRAM -0.6..-0.8 GiB. Visual: one guard held at dither
  values 0-5 with the key 0/1/1/0, identical pictures; component and proxy values sampled every 5 ms: off = a new proxy
  every frame, on = the same proxy, its value equal in all but single-sample read races. A day of normal play on it.

<a id="npccreationpacing"></a>

### Far-away characters created a piece per frame (`NpcCreationPacing`)

[Explanation](how-it-works.md#npccreationpacing).

- **Found:** when travelling attaches a new row of cells, the game creates every NPC and creature in it within a few
  frames: at Sheogorath's shrine on the benchmark forest route (see "What still limits the game") 31 actors in 4 frames
  of 40-110 ms; measured more closely, 5-6 frames of 21-72 ms at 854x480, and at Peryite's shrine (all five wear robes
  there, 31-49 ms per NPC) four frames of 42-105 ms, 384-397 ms over the median per run. Each NPC costs the game thread
  20-40 ms, in units that cannot be split: the create command (spawn, first animation setup, construction script, AI
  controller, 4-12 ms), a head/headwear command (~7 ms), then one frame later the appearance apply (the real body and
  head meshes with a second animation setup, 4.6-14.6 ms), one equipment unit per clothing piece (a wizard robe 14 ms)
  and the hair (1-3 ms).

- **The change** (14 vtable-slot hooks, no code patching): these units run under a per-frame game-thread budget
  (4 ms, or under a frame-rate cap the cap period minus the game thread's own busy time minus 1.5 ms).
  The first unit of a frame always runs; a further unit waits only if its actor is farther than 4096 units and, for the completion units, has not been rendered in the last 0.5 s. The old engine's command queue
  to the Unreal side stays FIFO: a waiting command holds everything behind it, nothing is reordered. The fix counts the
  queue's runs (S) against the executed end commands (E) and continues only while E < S, so the game thread never waits
  for an old-engine frame that itself waits for the game thread. The budget grows with the age of the oldest waiting
  work (triple at 15 frames), and everything older than twice that (30 frames) runs at once. Nothing
  waits during travel, loading screens or menus, or whenever a check fails; switching the fix off drains the backlog
  at once.

- **Nested completions:** a completion that finished inside a running unit (hair or equipment completing
  during an appearance apply) must inherit its parent's distance. Treating it as "near" regardless of budget meant a distant appearance unit
  pulled its pieces into the same frame: one measured frame held 4 units, 115 ms of them (an appearance apply of 59 ms
  plus three hair units), presented at 117.7 ms. Nested completions inherit the running unit's distance.

- **Owner walk (lab key `PacingOwnerWalk`, not shipped):** an equipment delegate's owner is the character's
  `VCharacterBodyPairingComponent`, whose word at the actor's root-component offset is data, not a pointer; reading it
  faults and ends the owner walk, so the unit counts as near and runs whatever the budget. Pull request #25's first
  version let the walk go on to the outer (the character); its author withdrew that part on 2026-10-27 after re-runs
  showed no measurable gain. Our AMD check agrees (4K, FSR Performance, frame generation, 60 fps cap, RX 9070 XT;
  `ic-spots` scene `ic-waterfront-pass`, one game, live toggles): about 10 more equipment units per pass are paced,
  frame-level numbers within the noise. It stays a lab switch.

- **Area-loading order:** the game's per-area "level flows" (set-up for each newly loaded area, run at the end of the frame's world
  update) met an owed queue and forced the waiting creates through: 2-3 creates plus equipment, 17-27 ms of units in
  one frame, 25-40 ms frames at fixed spots of the Silver Road climb. The flow waits (at most three frames without progress, up to eight while creation progresses), and so does the frame's end command to the old engine,
  which pauses it exactly as a long stock frame does; the command order between the threads stays the game's.

- **Result** (interleaved runs, off -> on; worst frame in the burst window):

  | Spot | Worst frame | Frames >= 33 ms |
  |---|---|---|
  | Sheogorath's shrine, 854x480 | 61-68 -> 36-43 ms | 4-5 -> 1-2 |
  | Peryite's shrine, 854x480 | 89-98 -> 41-51 ms | 4-5 -> 4-5 |
  | Sheogorath's shrine, 4K, FSR Performance, frame generation, 60 fps cap | 68-86 -> 31-54 ms | 2-4 -> 0-2 |
  | Peryite's shrine, same settings | 79-85 -> 34-52 ms | |

  The same work runs in about twice as many shorter frames. The flow-wait A/B on the Silver Road climb (854x480,
  interleaved): where a flow met an owed queue the worst frame dropped by 8-14 ms (35.3 -> 21.4-23.5 ms at one spot,
  38.0-39.3 -> 26.2-31.0 ms at another); whole-climb presents >= 25 / >= 33 ms were 58/16 and 57/22 without the wait
  and 48/8 and 59/17 with it. No wait reached the 3-frame bound. Which spots meet an owed queue varies from run to run.

- **What it does not fix:** the unsplittable units still take one frame each (an appearance apply of 16-22 ms, a
  robe's equipment unit of 13-21 ms), and the first frame of a flow event is the same with and without the wait
  (41-44 ms at one spot): a burst becomes a series of shorter hitches, not none.

- Robustness: 15+ flights over both shrine spots, the Cheydinhal and fast-travel scenarios, crowds and a quickload
  0.4 s into a spread, without a hang or crash; the self-test runs a simulated old engine (order, bounds, off switch).
  It changes timing only (a few frames for far-away, unseen NPCs); used in ordinary play on the test PC, and on by default.

- **The flow keeps waiting while creation progresses.** Echo Mine -> Brotch Camp at 4K (with a per-frame unit log): 5-13 creature creates of 7-11 ms at ~15 s, one per frame
  (17-25 ms frames under the 60 fps cap); in 2 of 6 runs a level flow had waited the 3-frame bound, the queue drained
  and that frame ran two creates (29-30 ms). The bound stays 3 only without progress; while each held frame ran a
  create or head/BP unit of the main queue and nothing older than the flow's own OT frame is owed, the flow waits up
  to 8 frames. Self-test: 6 creates in one OT frame -> one per frame,
  no drain; 10 -> drain after 8 held frames; a three-frame bound reproduces the drain. In 14 further runs (4K walk 4 + 4,
  Peryite 3 + 3, the bound changed live) the bound was not reached with either bound: no frame with two creates, no
  regression. On the Silver Road climb (3 runs) a flow waited 5 frames with one create per frame (19.8-22.8 ms
  frames): the two creates the old bound would have drained into one ~29 ms frame ran in frames of 20.1 and 21.7 ms. The order of every command between the threads is unchanged; the OT waits up to 8 GT frames instead of
  3 (it waits for the stock game's single long frame the same way).

- **The bound's drain runs the main queue only (`PacingLeanFlowDrain`, pull request #25 by mkusm, 1.2.3):** at
  y -56660 on the Imperial City flight one level flow (its RunSession called from exe+0x4881C55) reaches its 3-frame
  bound in most runs, the queue owing 4-6 older OT frames with 13-21 delegates waiting, and `Drain` then ran the owed
  queue and every waiting delegate in one frame. A flow on a complete queue already leaves the delegates queued (in the
  game the flows run before the frame's delegates); at the bound the flow needs only the main queue before it, so
  `Drain(manager, false)` leaves the delegates to the frame's replay. Travel and other sessions drain as before. The
  same idea had been a lab switch of ours since 2026-10-20 (`PacingFlowDrainLite`), with no measurable gain on its own
  at our settings; the pull request's version ships.
  mkusm measured it on NVIDIA (pull request #25). Our check (2026-10-27, 1.2.3 candidate, RX 9070 XT, 4K, FSR
  Performance, frame generation, 60 fps cap; `ic-spots` scene `ic-waterfront-pass`, one game, live toggles in a
  palindrome): with the switch on, the drain frame at the Waterfront flow's bound ran the owed main queue only in 4 of 4
  runs, the 10-12 kept delegates following in later frames, and it was shorter than the one off run whose flow reached
  its bound (that off run also had a lab-only change to the owner walk, so we give no number). Whole passes within the
  noise.

- **Fade-aware "seen" test (1.2.3, hidden key `PacingFadeAware`, on):** the pacing let a far character's pieces wait only
  while the engine had not drawn it in the last half second. New far characters failed that test although nobody could
  see them: every character that appears dither-fades in (fully invisible at first; the fade component's value 5 -> 0,
  >= 3 for about the first 0.75 s of 1.25 s), and at Namira's Shrine on the Bruma night route (`bench/scenarios/bruma-night.json`)
  the worshippers about 180 m away were behind terrain and trees as well: the engine draws a newly shown object as visible
  until its first occlusion result comes back. In our runs (4K, FSR Performance, frame generation, 60 fps cap) every far
  character the test called "seen" while its pieces were due was still fully faded out. A far character whose fade
  component (found from its tick by byte pattern, owner class checked) is fading in at 3 or above now counts as unseen,
  and the first waiting piece is re-checked every frame so it runs as soon as the character could be seen. Bruma night
  route: about 28 % less of the frame-time jitter from building characters; Imperial City Waterfront: no change; no
  piece of a far character ran after it had become visible. Near characters are untouched.

<a id="clothregisterlite"></a>

### Robes built once instead of up to three times (`ClothRegisterLite`)

[Explanation](how-it-works.md#clothregisterlite).

- **Found (2026-10-08 to 10-20, NPC creation profiles):** inside one `EquipBodyPart` call (exe+0x484CCD0) a robe's
  `UChaosClothComponent` builds `FClothSimulationProxy` at registration (`OnRegister` -> `RecreateClothSimulationProxy`,
  UE 5.3 ChaosClothAsset ClothComponent.cpp) and its Blueprint's `OnAttachedToPhenotype` rebuilds it once or twice; each
  rebuild starts with `ClothSimulationProxy.Reset`, so the earlier proxies are never ticked or rendered. Most of a build
  is `FClothingSimulationCloth::FLODData::Add`'s `FClothConstraints::AddRules`. A first attempt that left the
  registration build out altogether was not identical: the registration itself reads its bounds (`UpdateBounds`).
- **The change:** every build inside the call is made without `AddRules` ("lite"). Each lite build records its `AddRules`
  calls (the constraints object and arguments, the LOD's particle X / InvM at that moment) and the constructor's
  `FClothConstraints::Update` calls. At the end of the call the proxy that is left is completed in place: X put back for
  `AddRules` (InvM must be unchanged), the rules added per LOD in order, `FClothConstraints::Enable` for the active particle
  ranges, the Update calls replayed. Records of proxies dropped inside the call (`OnUnregister`, a second registration)
  are retired; anything unexpected rebuilds that cloth in full. Hooks: the native table's exec thunks, `OnRegister`'s and
  `SetSkinnedAssetAndUpdate`'s calls, two vtable slots, `EquipBodyPart`, the `AddRules` and `Update` call sites; layout
  offsets checked against the code that uses them. Start-up install (the native table is read at class registration).
- **Why identical:** the bounds a lite proxy shows come from the particles' positions (`AddParticles` / `ResetStartPose`
  before `AddRules`, and `Solver->Update(0)`, which with a zero time step never steps the evolution, the only place rules
  act); `FClothConstraints::Update` / `Enable` test every constraint pointer, so a rule-less cloth runs through the same
  code. The completed proxy gets rules from the same inputs in the same order (the evolution's constraint ranges get the
  same offsets). Verified with the maintainers' build: every completed proxy compared bit for bit with a full build made
  beside it, on routes with hundreds of robes: no mismatch, no fallback.
- **Results:** cheaper NPC equipment steps and fewer hitch frames where many robed characters appear at once, measured
  at the test PC's play settings together with the three keys below; the routes are not in the public benchmark yet, so
  no numbers are quoted here. Self-test: every site, the install, and lite builds and completions against fakes (key
  switched off mid-call, 12 LODs, spoiled constraints, unregistration).

<a id="clothbuilddefer"></a>

### A far-away robe's cloth built a few frames later (`ClothBuildDefer`)

[Explanation](how-it-works.md#clothbuilddefer).

- **Found (2026-10-19/20, stack samples and a build log at Peryite's Shrine, `npcburst`):** most of a robed NPC's creation
  unit is `UChaosClothComponent::RecreateClothSimulationProxy` (exe+0x53CD090) building a whole `FClothSimulationProxy`,
  a few ms per robe; per robe an `OnRegister` build, a script rebuild ~5 ms later and another 40-90 ms later after the
  proxy ran 1-3 ticks, each inside the NPC's creation unit. Every rebuild starts from nothing, so only the last build's
  proxy survives.
- **The change:** `RecreateClothSimulationProxy`'s entry is detoured. A request on the game thread inside a
  `NpcCreationPacing` unit whose actor may wait (far, not rendered recently, a frame that defers; the unit's owner is
  passed from `TimeUnit`), for a registered component with no proxy yet, is held. A robe is built in an equipment unit,
  whose owner is the character's `VCharacterBodyPairingComponent`: its word at the actor's root-component offset holds
  data, not a pointer, so pacing's owner walk faults there and runs the unit as near. This question walks on past such
  an object to the character (`ActorLocationGuarded`; pacing's own walk is unchanged): without it almost no robe build
  is held. Later requests from such units merge into it; a request from anywhere else builds at
  once (dropping the held one). Held builds run at pacing's frame start with the game's function and the component's
  state then: when quiet for `ClothDeferQuietFrames` frames (6) or 30
  frames after the first request, inside the frame's pacing budget (which grows with the wait as pacing's does), and
  counted as a unit; at once when the actor is near or rendered, the frame does not defer, or 60 frames after the first
  request. `execIsSimulationSuspended`'s call and `execIsSimulationEnabled` (both answer from the proxy) build a held
  build first, so Blueprint logic sees the stock answer. `OnUnregister`'s vtable slot (exe+0x803EF20; in front of
  ClothRegisterLite's hook there, found through `ClothRegisterUnregisterSlot`) drops a held build. A held build runs only
  if the component is still the same registered object (vtable, object index) with no proxy.
- **Why the result is the same:** the same component state is built by the game's own function, a few frames later and
  only off-screen. Until then the component has no proxy: `TickComponent` skips the proxy tick (null check), the render
  data falls back to plain skinning, `OnUnregister` resets nothing. With ClothRegisterLite a held build leaves no proxy,
  so its completion at the `EquipBodyPart` call's end has nothing to complete, and the later build is a full one.
- **Bounds (not yet as stock):** the registration's render state creation runs `UpdateBounds` -> `CalcBounds`, which
  caches the proxy's particle bounds in local space (stock: the first build's). With that build held there is no proxy,
  so it caches an empty box, and the build made later does not refresh the cache; the renderer takes the primitive's
  local bounds from it.
- **Checked before writing:** `RecreateClothSimulationProxy`'s prologue and its proxy field, `OnUnregister`'s single
  vtable slot (or ClothRegisterLite's hook in it), both Blueprint readers' code; anything else: not installed, one log
  line (the self-test changes one byte of the entry and checks that nothing is written). Installed at start-up on the
  version.dll route, after ClothRegisterLite; on the OBSE64 route it stays off with one log line. Then live.
- **Image check** (`npcburst-visual`: the shrine flight, then two frozen views of the five worshippers, near and from
  above; `tools\Compare-Benchmark.ps1`, visual pass, off and on in fresh games): every robe present, draped and of
  normal length; the worshippers stand and pose differently, as between any two runs, and the foreground grass moves in
  the wind.

<a id="rigvmhashmemo"></a>

### Animation rig set-up remembered (`RigVmHashMemo`, `RigLogicMapCache`)

[Explanation](how-it-works.md#rigvmhashmemo).

- **Found (2026-10-20, NPC creation profiles):** every `UControlRig` an NPC's anim graph creates (2-3 per humanoid) calls
  `URigVM::ComputeVMHash` (exe+0x49A95D0, RigVM.cpp:327) four times (`PostInitInstance` and `InstantiateVMFromCDO`, on
  the instance and on the class default object). The head rig's `FRigUnit_RigLogic` (RigUnit_RigLogic.cpp:85) on its
  first run maps every DNA joint, control, blend shape and animated-map name to a hierarchy index through `FString`
  building, `FName` creation (name pool lock) and the hierarchy lookup, on a worker the game thread waits for.
- **The change:** `ComputeVMHash`'s entry jumps to a memo keyed on everything the function reads (function names, byte
  code with its alignment, branch infos, external variables' names and types, the memory storage classes' hashes computed
  with the game's own functions), compared in full. `InitializeRigLogic`'s four map calls go to a cache that keeps, per
  DNA content, the `FName`s the stock function created (captured at its own `FName` call site) and the reader-side
  values, keyed on everything it read from the DNA reader in its order; a later head with the same reads replays the
  same `TArray` operations and does every hierarchy lookup live. Reader classes are checked once (the `Unwrap` code shape,
  the dna getters) and every captured dna string must equal the stock `FString` getter's result, else that class is left
  to the game. Both functions' bytes are checked in full; start-up install, then live.
- **Why identical:** `ComputeVMHash` is a pure function of its inputs; an `FName` of a given string never changes once
  it exists and the lookups are live, so the indices are the stock ones. Verified with the maintainers' build (a mode
  that runs the game's function beside the cache and compares): thousands of hashes and hundreds of heads per map,
  Argonian and Khajiit heads included, no mismatch.
- **Results:** see `ClothRegisterLite` above (measured together). Self-test: the sites, the memo against a fake hash over
  every input (any change misses, a copy at another address hits), the map replay against reference functions written
  from the UE 5.3 source, refused reader classes, verify mode.

<a id="npcbodiesonelock"></a>

### A character's physics bodies made under one lock (`NpcBodiesOneLock`)

[Explanation](how-it-works.md#npcbodiesonelock).

- **Found (2026-10-20, NPC creation profiles):** `USkeletalMeshComponent::InitArticulated` (exe+0x36E3100,
  SkeletalMeshComponentPhysics.cpp:665) -> `InstantiatePhysicsAsset_Internal` (exe+0x36E8080) creates ~37 bodies per
  humanoid and their constraints; every `FBodyInstance::InitBody` and `InitConstraint` goes through
  `FPhysicsCommand::ExecuteWrite`, whose lock scope takes the solver's recursive SRW write lock (~550 lock scopes per
  humanoid create); the game thread waited in those acquires behind scene queries on other threads.
- **The change:** `InitArticulated`'s call of `InstantiatePhysicsAsset_Internal` is redirected to a wrapper that takes
  the same lock once, the game's way (write count in the lock's TLS slot, `AcquireSRWLockExclusive` for the first hold),
  and releases it after; the nested `ExecuteWrite`s only count. `InitArticulated`, `InstantiatePhysicsAsset_Internal`,
  `InstantiatePhysicsAssetBodies_Internal` and `InitConstraint` must equal build 19115871's bytes (FNV-1a over each whole
  function); the solver and lock offsets are read from `ExecuteWrite` and its lock scope.
- **Why equivalent:** the game thread runs the same code with the same arguments in the same order. Other threads see the
  scene before the first body or after the last constraint, both states the stock game can show them (lock order between
  threads is not fixed), so the outcomes are a subset of the stock ones. Nothing inside the hold waits for another
  thread (5.3 source), so it cannot deadlock. The hold also covers shape creation, which readers now wait for: measured
  in the maintainers' build with a lock-wait probe, the extra reader waits were a very small share of the workers' time;
  holds are logged every minute (count, mean, longest).
- **Results:** see `ClothRegisterLite` above (measured together). Self-test: the sites and pinned functions, then the
  wrapper with a fake: off, on (one hold, released after), nested inside a hold, no solver, no scene.

<a id="animteardownparallel"></a>

### Animation data taken apart on helper threads (`AnimTeardownParallel`)

[Explanation](how-it-works.md#animteardownparallel).

**Measured (2026-10-17, 480p `small`, Kvatch wall flight, on/off/off/on in one game):** at the heaviest crossing the
NPCs' animation teardown 9.4-10.0 -> 4.9 ms, the row change 30-32 -> 26 ms and the game thread's wait 33-35 -> 28 ms; at
the next one 7.2 -> 4.2 ms of animation teardown; light crossings unchanged. About 1 million interpolators went through
the helpers with no fallback and no crash. It shortens the worst crossings by ~5-7 ms; it does not remove them (the rest
of the row change is other unloading and loading). `AnimTeardownParallel=1` is on by default.

<a id="cellbufferpacedevict"></a>

### Old cells unloaded over a few frames (`CellBufferPacedEvict`)

[Explanation](how-it-works.md#cellbufferpacedevict).

**Measured (2026-10-17, Kvatch wall flight, `AnimTeardownParallel` on in every comparison, the fix switched live in one game):**

| | 480p `small`, on/off/off/on | 4K play settings (60 cap, FG), off/on/off |
|---|---|---|
| crossing frame (old engine), mean of 7 crossings | 24.9 / 26.2 -> 13.2 / 14.3 ms | 27.6 -> 13.7 ms |
| game thread's wait in it, mean | 21.9 / 22.6 -> 10.6 / 11.7 ms | 19.9 -> under 4 to 9 ms |
| worst crossing's wait | 31 / 29 -> 12 / 13 ms | 28 -> 9 ms |
| each later unloading frame | +3.5 ms old-engine frame on average (1.2 ms median per cell, one heavy cell 10-13 ms); the game thread's wait in it 6.8 ms mean, 17 ms worst | worst wait 16 ms |
| slow game-thread frames (>= 25 ms) with a wait >= 8 ms | | 7 (worst 46 ms) -> 2 (26, 32 ms) |

What is left of the crossing frame is the incoming row (temporary data load ~3.5 ms, attaching the cells ~3.5 ms) and the
old engine's ordinary frame work. `CellBufferPacedEvict=1` is on by default.

## Doors, loading screens and start-up

<a id="preparecollisionbehindloadingscreen"></a>

### Collision prepared behind the loading screen (`PrepareCollisionBehindLoadingScreen`)

[Explanation](how-it-works.md#preparecollisionbehindloadingscreen).

- **Found:** after a door, city gate or fast travel, the picture comes back with one frame of 120-220 ms (720p) or
  140-270 ms (4K with frame generation), about a tenth of a second into the fade-in. Stack samples of that frame showed
  the first physics step rebuilding Chaos's broad-phase tree (`ComputeIntermediateSpatialAcceleration`, `exe+0x192CC60`),
  with the GameThread waiting.

- **Cause:** while a transition loads, the game freezes the world, and the freeze clears
  `UWorld::bShouldSimulatePhysics`. So nothing the level load adds reaches the physics engine until the world starts
  again, and the first step then takes in every body at once.

- **The change:** just before the game starts the fade-in, while still frozen, the fix asks the solver for a zero-time
  tick, the engine's own "flush" that the editor also uses. That tick takes in pending changes and updates the
  acceleration structures but simulates nothing. Afterwards the fix refreshes the game thread's scene-query structure, as
  the end of every physics frame does. The freeze, the fade, the timers and the moment the player gets control are not
  touched. The fix only acts on the transition's own freeze, never on a menu.

- **Result:**

  | Setting | Frame as the world starts, before | After |
  |---|---|---|
  | 720p, door exits | 144-199 ms (median 164) | 29-60 ms (median 37-41) |
  | 4K + frame generation, door exits | 123-136 ms | 18-26 ms, no slow frame left in the fade-in |
  | Fast travel, 720p / 4K | 86-147 / 77-129 ms | 36-59 / 19-36 ms |

<a id="psoprecompilefast"></a>

### Start-up shaders built faster in menus (`PsoPrecompileFast`)

[Explanation](how-it-works.md#psoprecompilefast).

**Measured (2026-10-16, 480p `small`, fresh launches, scenes right after arriving, ABBA on/off/off/on plus stock after
its own precompile):** the precompile finished at 22-25 s after launch instead of 110-148 s (8 s of it at the Fast
pace, during the main menu). The first doors after arriving: the world appeared 0.7 s (inn entry) and 1.6-1.7 s (inn
exit) after arrival, as in stock once its precompile is done, instead of 5.7-5.8 s and 6.6-6.7 s while stock's
precompile was still running. Frame rate in Cheydinhal right after arriving: 42.7-44.5 fps with the fix, 45.0 in stock
after its precompile; stock during its precompile was a little faster (46-47.5 fps; it has not finished building yet),
so the fix costs nothing lasting. Cost: for those ~8 seconds in the main menu, menu frames of up to ~30 ms (p99)
instead of ~4 ms. At 4K with the test PC's play settings (frame generation, 60 fps cap) it finished 27 s after launch (10 s
at the Fast pace), menu frames p99 31 ms meanwhile, and the first doors showed the world 0.7 s / 1.6 s after arrival.
With a cold driver cache (the AMD cache moved away, as after a driver update) the Fast pace ran in the main menu and
during the first load (3,400 pipelines in 27 s, against ~45 in the 15 s at the game's pace before the menu appeared),
the game's pace came back in play, and the precompile ended after ~6 minutes, about as long as a cold start took
without the fix. Start-up videos before the main menu count as play for the fix (no menu freeze yet), so
those seconds keep the game's pace.

**Start-up crash in the driver-thread check** (2026-10-29). Two start-up crashes
(2026-10-27 14:13 and 2026-10-29 00:59, 2 in roughly 40 launches since the guard's driver-thread reading existed) on
the monitor thread: access violation calling address 0. The 2026-10-29 dump: the crashing frame returns into
chrome_elf.dll+0x14D7 (CEF 90.6, the game's own Engine\Binaries\ThirdParty\CEF3), which had called a function pointer
it keeps at +0x11E000; below it chrome_elf+0x1902 (a 10-argument hook, NtMapViewOfSection's shape), kernel32, and our
call to Thread32Next. Toolhelp's Thread32First/Next map the snapshot with NtMapViewOfSection on every call, one call per
thread on the PC (about 7,000 per search here), and they do it outside the loader lock. Chromium's chrome_elf hooks
NtMapViewOfSection while it loads, under the loader lock, which normal DLL loads respect; a call between the hook going
in and the original's address being stored jumps to 0. Reading: our search is the one caller that can land in that
window, thousands of times per second while it runs. (The window itself is from how chrome_elf installs its hook; the
pointer's value could not be read, the dump does not hold that page.) The fix lists this process's threads with one
NtQuerySystemInformation(SystemProcessInformation) call, which copies into our buffer and maps nothing; the game
thread at install (the first-created thread) comes from the same list. Self-test: the list against a snapshot in the
test process (same count, same first thread). Not reproducible on demand, so the check is the absence of the crash over
time.

## Menus

<a id="displaymodecache"></a>

### Display modes kept for the Graphics page (`DisplayModeCache`)

[Explanation](how-it-works.md#displaymodecache).

- **Found:** opening the menu on the Graphics settings page cost one frame of 720-771 ms at 4K (450-515 ms at 854x480),
  every time. The menu reopens on the last page used, so for anyone who adjusts graphics that is every press of Esc.
  About a quarter of that frame was display-mode enumeration.

- **Cause:** the page forces a rebuild of the engine's display metrics four times
  (`FDisplayMetrics::RebuildDisplayMetrics`, `exe+0x1183FB0`). Each rebuild's monitor scan (`exe+0x1172DB0`) reads every
  display mode of every monitor twice with `EnumDisplaySettingsW`: once for the game's own list of resolutions and once
  for the engine's maximum-resolution loop. The test monitor reports 683 modes at ~0.1 ms each, so 21-30 ms per full
  read and about 180 ms per page show at 4K, more with several monitors.

- **The change:** the game exe's import slot for `EnumDisplaySettingsW` is hooked. Only the calls from that monitor scan
  are answered from a per-monitor copy of the list; every other caller goes to Windows as before. Every read is first
  checked against Windows (~0.7 ms): the current mode and modes 0, N/4, N/2, 3N/4 and N-1 compared byte for byte, and
  mode N must not exist. Any difference reads the list again. `EnumDisplaySettingsW` writes exactly its returned `dmSize`
  (188 bytes) whatever the buffer held, so a copy gives the caller the same bytes. The fix checks this once at its first
  use and stays off if it doesn't hold.

- **Result:**
  - 2 ms instead of ~60 ms per enumeration.
  - In game, 163 of 166 reads came from the copy, and none was ever out of date.
  - The engine's display data after the page's rebuilds, read from memory with the fix off and on, is identical
    (monitor, native resolution, all 24 resolutions, work area, DPI).

<a id="menuscreenshotfilter"></a>

### The menu's screenshot filtered in one go (`MenuScreenshotFilter`)

[Explanation](how-it-works.md#menuscreenshotfilter).

- **Found:** each menu open takes a screenshot of the game view, presumably for the save-game picture. The game then
  passes it through `FImageUtils::ImageResize` (`exe+0x32548C0`) with a destination as large as the source. At the same
  size that is a 2x2 box filter, computed in double precision one pixel at a time: 139 ms at 3840x2160 on the game
  thread, 31% of the 4K menu-open frame.

- **The change:** the one call of that function (`exe+0x32549A3`) goes to a hook. The hook computes the same-size,
  non-linear case with integer SSE2 and leaves every other case to the game. For 1, 2 or 4 pixels the double-precision
  average is exact before truncation, so an integer sum shifted right gives the same byte.

- **Result:**
  - 139 ms becomes 3.1 ms at 4K.
  - The game's own function (run from the mapped exe in the self-test) and the hook give identical bytes on 120 images
    (20 sizes from 1x1 to 1279x719; random, white and noise; alpha forced or kept) and at 3840x2160.

**`DisplayModeCache` and `MenuScreenshotFilter` together**, 4K, FSR Performance, frame generation, the open on the Graphics page, switched live in one
game session:

| Fixes | Frame as the menu opens |
|---|---|
| neither (as the game ships) | 720-724 ms |
| display-mode cache only | 530-554 ms |
| screenshot filter only | 550-573 ms |
| both | 390-426 ms |

The rest of that frame is the game's own work, which we left alone:
- the player's cloth is rebuilt for the menu view (~19%);

- the game thread waits for the screenshot's GPU readback (~18%);
- the menu widgets are built (~30%).

<a id="screenshotreadbackconvert"></a>

### Screenshot colours converted four pixels at a time (`ScreenshotReadbackConvert`)

[Explanation](how-it-works.md#screenshotreadbackconvert).

**Measured (2026-10-15, 4K, the test PC's own play settings, inventory opened at the Cheydinhal canal, one held game, switched live
on/off/off/on, four opens each, plus one more off block):** the frame that opens the inventory 130-148 ms (median 134.5)
instead of 166-195 ms (median 170): -35 ms; the time the screen stood still 78-99 ms (median 80.5) instead of 114-145 ms
(median 115). The conversion itself 40 ms -> 9-10 ms in the game (5 ms offline; the rest is fresh memory being touched for
the first time). Containers open without a screenshot, so they are not affected; nothing changes while a menu is open or
when it closes. Pictures of the inventory and of the world after closing look the same with and without.

<a id="screenshotparallel"></a>

### Screenshots copied on helper threads (`ScreenshotParallel`)

[Explanation](how-it-works.md#screenshotparallel).

**Measured (2026-10-15, 4K, the test PC's own play settings, inventory opened at the Cheydinhal canal, one held game with `ScreenshotReadbackConvert` on,
switched live on/off/off/on, four opens each):** the frame that opens the inventory ~121 ms instead of ~140 ms (median,
-19 ms); the time the screen stood still ~70 ms instead of ~88 ms. Each 33 MB copy now ~3 ms (was 6-9). With `ScreenshotReadbackConvert`:
170 ms -> ~121 ms, screen still ~115 ms -> ~70 ms. Pictures of the inventory and of the world after closing look the
same with and without. Closing the menu is not affected (that hitch is the world's first frame being drawn again).

## Saving

Measured 2026-10-19 in a held save-free scene (quicksaves by `Altar.QuickSave` through the benchmark bridge, also
`Altar.AutoSave`, the quicksave input handler and door autosaves with `Altar.SaveOnLoading 1`), frames from the hitch
ledger, keys switched live in A-B-B-A order.

<a id="savebytesbulk"></a>

### `SaveBytesBulk`

[Explanation](how-it-works.md#savebytesbulk).

- **Found:** stack samples of a save frame: `UGameplayStatics::SaveGameToSlot` for `saves_meta.sav` on the game thread,
  twice per save, inside `SaveGameToMemory`; ~1.6 MB of the list are `TArray<uint8>` thumbnails, and a tagged (not
  unversioned) archive sends them through `FArrayProperty::SerializeItem` -> `FByteProperty::SerializeItem` ->
  `Ar.Serialize(&b, 1)` per byte (UE 5.3 PropertyArray.cpp / PropertyByte.cpp): 14-17 ms per list write at any resolution.
- **Result:** per write 14-17 -> 1.7-2.7 ms; the save frame at 4K (user's settings) 91-109 ms (median 104) -> 80-88 ms
  (median 84) with this key alone.
- **Identity:** for an archive whose `Serialize(p, n)` equals n calls of `Serialize(p + i, 1)` (memory writer behind the
  name-as-string proxy, as `SaveGameToMemory` builds it) the bytes are the same; the self-test runs the game's
  function and the hook on random arrays into a recording archive; the test build's verify mode wrote every save both
  ways in game and compared (identical, 21/21). Call sites: an SSE2 scan for `E8` calls to `SaveGameToMemory` (4 in
  build 19115871), each checked to lie in a `.pdata` function and be followed by `test al,al` within 8 bytes.

<a id="savethumbnailasync"></a>

### `SaveThumbnailAsync`

[Explanation](how-it-works.md#savethumbnailasync).

- **Found:** `UGameViewportClient::ProcessScreenShots` reads the save's screenshot with `GetViewportScreenShot` ->
  `FRenderTarget::ReadPixels` (a render command with `ImmediateFlush(FlushRHIThread)` + `RHIReadSurfaceData`, then
  `FlushRenderingCommands`): 42-50 ms of game-thread wait at 4K, 17-25 ms at 480p. Its broadcast reaches the save's
  thumbnail callback (copy, remove, trigger the JPEG task's event) and the save system's permanent screenshot cache.
- **Result (all three keys):** 4K, 60 cap, frame generation: save frame 90-113 ms (median 110) + a 30-43 ms second frame
  -> 25-32 ms (median 28), no second frame above ~28 ms; 480p 70-77 (median 75) -> 27-30 ms. The picture is delivered
  50-80 ms after the request at 4K (25-45 ms at 480p).
- **Identity:** the same render command's copy point (`FRHIGPUTextureReadback::EnqueueCopy`), the same conversion as
  `ScreenshotReadbackConvert`; the test build's verify mode compared every pixel with the game's own read-back: 12/12
  identical (480p and 4K with frame generation; quick, auto, input-handler and door autosaves). Delivery order: the
  cache listener first, then the save callbacks (the save callback's event lets the save continue, which reads the
  cache). Taken over only when every listener is the save's callback or the cache; others (pause-menu save, rest/wait)
  run the game's way.
- **HDR route (2026-10-30).**
  - Found (WPR CPU trace of a play session, 5120x2160 HDR10, RTX 5090): every autosave frame was 115-174 ms (six in
    one session, about 2.5 s after each fade-in). In the ledger it is game-thread work late in the frame (post 110-165 ms,
    sync 0). On the game thread, 45 of 72 samples were in ProcessScreenShots' FLinearColor -> FColor loop
    (exe+0x3223140), the rest in the save's callback and allocations.
  - Why the SDR takeover missed it: a DIAG at the takeover logged no call. ProcessScreenShots picks its read by the
    viewport's HDR flag (FViewport vtable +0x40). The SDR read is GetViewportScreenShot (exe+0x37E4F30, the call the fix
    redirects). The HDR read is GetViewportScreenShotHDR (exe+0x37E4FB0): ReadLinearColorPixels (exe+0x37F32F0), the same
    render command shape, lambda exe+0x37D2B50 calling RHIReadSurfaceData's FLinearColor overload (vtable +0x240). With
    delegate listeners and the delegate cvar on, it then converts per pixel with FLinearColor::ToFColorSRGB
    (exe+0xEAB6A0), sets alpha 0xFF and broadcasts through the same call.
  - Identity: the D3D12 read (exe+0x252B510) converts the back buffer's raw rows with exe+0x251C400. Its format-18
    (A2B10G10R10) case multiplies each 10-bit channel by 1/1023.f (exe+0x709E164 = 0x3A802008). ToFColorSRGB's channels
    are independent (one sRGB table, per-lane constants). So per channel, table[v] = ToFColorSRGB(v x 1/1023.f), built
    with the game's function. The self-test calls the mapped exe's converter and quantiser on every 10-bit value per
    channel plus 8,192 random pixels and compares: 0 differences. Other back-buffer formats: the render thread does the
    stock linear read into the marker, and the helper applies the game's ToFColorSRGB per pixel.
  - Result (a play session, eight door autosaves in HDR10): every picture went through the async copy (GPU wait
    10.8-21.4 ms, conversion 2.7-6.4 ms, the save's callbacks 10.2-19.1 ms, all on the helper). No game-thread frame of
    25 ms or more within 1.5 s before to 0.5 s after any delivery; before, six frames of 115-174 ms (median about 118).
    The new autosaves' thumbnails in the Load menu were checked: they look as before.
  - Benchmark (2026-10-30, `cheydinhal` scene `inn-door`, profile `native-nofg` with HDR10 on, `Altar.SaveOnLoading 1`
    for the run so both doors autosave; fresh launches, on/off interleaved, 5 runs each; the frame of each save found
    from the save file's time, 300 ms before to 200 ms after): entering the inn, worst frame off 100.6 ms (98.1-104.0),
    on 11.8 ms (10.7-12.5); leaving it, off 126.1 ms (96.7-137.2), on 22.0 ms (20.5-22.5; one run's second save fell
    after the frame capture ended). Ledger: every off save had a game-thread frame of 97-150 ms of its own work
    (actor or post time, sync about 0). No on save had a frame of 25 ms or more, except the one past the capture: a
    108 ms frame that is the game thread waiting for the render thread (sync 95 ms), the run's last ledger frame, so
    the runner's end of run rather than the read (the picture went through the async copy, per the log). On the
    helper: GPU copy wait 6.5-10.8 ms, conversion 2.5-6.6 ms, the save's callbacks 4.6-11.3 ms.


<a id="saverefidindex"></a>

### `SaveRefIdIndex`

[Explanation](how-it-works.md#saverefidindex).

- **Found:** stack samples of the old engine thread during saves: `TESSaveLoadGame::SaveGame` spends its time in formID
  -> refID lookups, a linear search over the save's formID table (inline in `SaveFormIDs` and `GetRefID`) with an append
  on a miss. A fresh game: ~4,000 entries, 32,000-40,000 lookups, 22-28 million entries read per save; the table only
  grows over a game.
- **Result:** the old engine's `SaveGame` 30-36 -> 23-31 ms at 480p, 28-35 -> 19-23 ms at 4K (new game). The gain on a
  long game's save is larger but not measured (it needs a long-game save; the benchmark is save-free).
- **Identity:** the index returns the first position, as the linear search does; self-test over a million random
  lookups with repeats, clears, outside appends and reallocations; the test build compared every lookup in game:
  0 differences in ~300,000.

## Frame generation

<a id="framegenasynccompute"></a>

### Frame generation's GPU work beside the next frame (`FrameGenAsyncCompute`)

[Explanation](how-it-works.md#framegenasynccompute).

- **Found (Cheydinhal frame-generation comparison):** at 4K FSR Performance, RT 3, Shadows
  High, FG on vs off in one game per comparison: 44.6-47.2 / 48.3-49.5 / 57.3-60.1 vs 56-59 / 62-68 / 75-80 rendered
  fps (canal / chapel square / inn front). Per rendered frame +1.5-4.3 ms of GPU work and 0-3.7 ms with neither the GPU
  nor a main thread busy. AMD's library is FSR 3.1.0: the proxy `Present` (+0x1C2F0) waits for the previous frame's
  interpolation on the GPU (interpolation event) and the game queue waits for the previous real frame's composition, so
  the game runs at most one frame ahead; with `r.FidelityFX.FI.AllowAsyncWorkloads` 0 (the game's value) the
  interpolation runs on the game's graphics queue between frames. Its presenter and interpolation threads busy-wait
  (fence and QPC loops, ~70-90 % of a core each), but two external spinning threads cost FG-off runs ~0 fps (chapel -4):
  not the cost in these views. `r.FidelityFX.FI.UIMode 1`: within noise.

- **The change:** `r.FidelityFX.FI.AllowAsyncWorkloads` = 1 (AMD's supported option: interpolation on the swap chain's
  async compute queue). It is registered read-only (flags 4, `ECVF_ReadOnly`) during the exe's static initialisation
  (exe+0xAA0690; the `TConsoleVariableData<int32>*` stored at exe+0x92BB250) and read when the plugin creates its swap
  chain (exe+0x51EC110) and frame generation context (exe+0x4098580). A thread from the DLL's start-up polls the pointer
  (1 ms sleeps) and writes 1 into both values once it is set, unless `amd_fidelityfx_dx12.dll` is already loaded (too
  late; left alone). Anything that sets it later (an Engine.ini line) wins. Found by the name string, the
  registration's shape (default 0, flags 4, `AsVariableInt` result stored at auto+0x10).

- **Results:** `-IniCvars` A/B, two fresh games, FG on / off / on / off each: FG on **54.6 / 63.5 / 74.7 vs
  44.6 / 49.5 / 58.0** (+22 / +28 / +29 %), 1 % lows 41.6 / 43.5 / 55.0 vs 32.4 / 35.4 / 41.5; FG-on rate 93-94 % of the
  same game's FG-off rate instead of 76-79 %; idle time 0.1-0.6 ms; present-to-present spread unchanged. In the game
  through the DLL (one world): 54.1 / 61.8 / 74.2, the value set ~1.7 s before AMD's library loads
  and read back 1 by the benchmark; with the key at 0 it stays 0; main-menu returns with FG on: no crash.

<a id="framegenpacingtuning"></a>

### Frame generation pacing after a slow moment (`FrameGenPacingTuning`)

[Explanation](how-it-works.md#framegenpacingtuning).

**Measured (2026-10-14, 4K, FSR Performance, ML frame generation with the SDK 2.3.0 DLLs, 60 fps cap, Kvatch walls
flight, two games, the key switched while running):** below the walls (spot C) the rendered frame rate stayed at
48 fps instead of 39-45 and was back to normal within 0.5-1 s instead of 3-5 s. The spacing of the frames on screen
was not worse: standing still the same (95th percentile 9.5-9.7 ms against 9.9-11.2 ms), over the whole flight a
little more even (95th percentile 11.0 against 11.7-11.9 ms). The one difference: a hitch the game itself makes (a
rendered frame of 33 ms or more, as often with and without) more often shows as one longer gap instead of being
spread over a few frames, about five more gaps over 25 ms in a two-minute flight.

**With Adrenalin's own swap chain (2026-10-26, driver FG 4.0.2 / swap chain 4.0.0 / upscaler 4.1.2, all the driver's;
4K, FSR Performance, frame generation, RT mode 3, 60 fps cap; hidden runs, the key switched live in held games, ABBA;
`rumare-loop-flight` and `kvatch-walls`):** the driver's swap chain accepts the key, and left unconfigured it paces like
AMD's defaults. Set B there: game-thread frames over 17.5 ms 16.0-16.5 % against 18.1-19.2 %, the game's present
blocked less (95th percentile about 5 against 10 ms), rendered frame rate +1-1.5 %; but the shown frames less even:
present gaps over 16.7 ms 127-146 against 85-104 per flight, over 25 ms 21-28 against 11-15, frame-to-frame timing error
(present step minus content step) +40 % on the Imperial City flight and +22 % on the Kvatch walls. The Kvatch spot C
slow-down that Set B was chosen for did not arise in a warm world. Since 1.2.2, `FrameGenPacingTuning=1` therefore
leaves the driver's swap chain (provider id bit 31) at its own pacing and sets Set B only on AMD's DLL swap chains;
`3` sets Set B everywhere.

<a id="framegencamerainfo"></a>

### The camera for AMD's ML frame generation (`FrameGenCameraInfo`)

[Explanation](how-it-works.md#framegencamerainfo).

**Verification.** The self-test checks the call site and view offsets, valid and invalid camera matrices, the off switch, the injected call and a pass-through export counter. Every frame is checked before camera data is attached; the log counts calls that reached AMD's library. `FrameGenCameraInfo=1` is on by default and acts with AMD's FSR 3.1.4 DLL; the game's own FSR 3.1.0 DLL ignores the data. See [the README](../README.md#optional-amd-ml-frame-generation-radeon-rx-9000). No generated-image quality difference or frame-rate gain has been measured; the frames rendered by the game itself are unchanged.

## Crash and bug fixes

Game bugs (they also happen without OblivionPerfFix). Bugs in our own fixes are documented in the fix's section.

<a id="namelookupcrashguard"></a>

### A crash in the game's name table (`NameLookupCrashGuard`)

[Explanation](how-it-works.md#namelookupcrashguard).

- **Found:** a crash on the old engine's thread inside `_stricmp`, three times in one day of testing.

- **Cause:** a game bug. The old engine's name-to-form table (`exe+0x8FAB580`) stores pointers to each form's editor-ID
  string and never updates them. Interior cells re-create that string whenever they reload, so entries point at freed
  memory: about 1,565 after start-up and about 3,400 after a new game, the same without our fix installed. Lookups
  (console names, collision sounds, opening a body) read those strings, and crash once Windows has taken the memory back.

- **The change:** the table's key compare is replaced with the same `_stricmp`, called inside an exception guard. Only a
  read fault inside the stored key counts as "no match", which is what such an entry yields while its memory still
  exists. Every other compare and every other exception behaves as before.

- **Result:** with a dead entry forced onto unmapped memory, the game without the guard crashed with the exact recorded stack. With the
  guard, both teleports arrived and the log reported two lookups kept from crashing.

<a id="trapcrashguard"></a>

### Traps let go of characters that no longer exist (`TrapCrashGuard`)

[Explanation](how-it-works.md#trapcrashguard).

- **Found (2026-10-12 crash, Kvatch Oblivion gate):** the old engine's `TESTrapListener`
  (RTTI-named, one per Havok world) keeps raw actor pointers per trap node and, after every Havok step
  (`bhkWorld::Update` -> listener vtable slot 5, `exe+0x66D67E0`), calls each actor's trap hit (vtable +0x250,
  `exe+0x65A8B90`: damage and push-back from the trap script's variables). Entries leave on contact end, on the actor's
  character controller leaving the world, or through the destructors' clean-up `exe+0x657AF30`, which finds the
  listener through the actor's current parent cell and is skipped for form flag 0x4000 or a global flag. The crash:
  `call [rax+250h]` with `rax` = FMallocBinned2's free-block header (896-byte block, canary 0xE3): the clannfear the
  rocks had hit and thrown was freed while still listed.

- **The change:** the update's vtable slot (the only `.rdata` qword holding it, its vtable's RTTI checked) points at a
  wrapper that, before the game's update, checks every actor entry (readable under SEH, vtable in `.rdata`, class
  derived from `Actor` by the RTTI base-class list, cached per vtable) and removes a dead one with the game's own
  contact-ended removal `exe+0x66D6560` (which also drops an emptied trap node). Live entries are never touched; the
  only behaviour change is no crash. Not covered: freed memory reused by another actor before the next step.

- **Results:** self-test with the real vtables and the crash's freed header; installed live (Kvatch plane runs).

<a id="objectreplacecrashguard"></a>

### A rare object-list crash turned into a log line (`ObjectReplaceCrashGuard`)

[Explanation](how-it-works.md#objectreplacecrashguard).

- **Found (crash dumps of 2026-10-12 and 2026-10-21):** "LowLevelFatalError UObjectArray.cpp:234 Attempting to add
  CombatMovement_IDLE (ProcedureExecution_WANDER) at index ... but another object exists at that index". The object being
  added is the "other object": `StaticAllocateObject` (exe+0x111F2D0) replacing an existing object in place (find by
  outer and name, BeginDestroy / FinishDestroy, then under `GUObjectArray`'s lock `~UObject()` with index recycling off,
  then the same memory constructed again at the old index). The AI's combat-movement setter (exe+0x47E5690) drops its
  old movement object and calls `NewObject(controller, "CombatMovement_<mode>")`, so re-entering a mode before the next
  garbage collection re-creates the old object in place: routine in fights. In both dumps the slot held the state after
  FinishDestroy with the free never done; `~UObjectBase` (exe+0x110BEE0) skips `FreeUObjectIndex` when the class pointer
  reads null (or `GIsCriticalError` is set, ruled out). Why the header changed in that window is not known. None of the
  DLL's code was on the stack or in that path; both crashes were with the DLL installed, so a cause on our side is not
  ruled out.
- **The change:** the 8 bytes `mov rcx,rdi; mov rax,[rdi]; call [rax]` at that destructor call become `mov rcx,rdi; call`
  a near jump to a function that makes the same call, reads the old index from the caller's frame and checks that slot.
  Freed (every normal case): nothing else. Still holding the destroyed object: the slot is cleared as `FreeUObjectIndex`
  does with recycling off, and the object's header before and after is logged. Patterns: the destroy sequence (the lock
  and recycling targets must be `GUObjectArray`+0x30 / +0xB4), the old index's store and `AddObject`'s read at the same
  frame offset, and `~UObjectBase` with the same `GUObjectArray`; any mismatch: not installed, with a log line. Start-up
  install, then live (0 = count only, the stock fatal).
- **Results:** self-test (the sites, a normal destroy untouched, a kept slot released or only counted, another object's
  slot and bad indices untouched). In the game: in-place re-creations are routine (about 40 a second in a kept battle),
  all untouched; a test that cleared the class pointer of the next three re-created objects got three "slot released by
  the guard" lines and the game played on; with the guard only counting, the same test stopped the game with the same
  "UObjectArray.cpp:234" message as the player's crash. Kept on in the maintainers' own play since 2026-10-21: it never
  had to act.

<a id="taskwaitdeadlockguard"></a>

### Task threads that wait for each other woken up (`TaskWaitDeadlockGuard`)

[Explanation](how-it-works.md#taskwaitdeadlockguard).

- **Found (2026-10-23, a stock crash dump with a cold driver cache, then live stack samples):** every task worker asleep in
  `~FScopedEvent` inside `WaitUntilTasksComplete`'s worker branch (exe+0xDD66B0), called from the
  `FD3D12RayTracingPipelineState` constructor (waiting for its shader-collection compiles); every reserve worker idle; the
  render thread waiting for tasks, the game thread for the render thread, until UE's `RenderingThread.cpp:1272` fatal
  error after 120 s. UE 5.3 `TaskGraph.cpp` (old front end): a waiting worker hands its slot to a reserve worker
  (`DoReserveWorkUntil`, condition "tasks done") and sleeps on an event set by an `FTriggerEventGraphTask`; the reserve
  returns to sleep as soon as the condition holds, which is exactly when the trigger task becomes ready, so with all
  workers waiting nobody runs the trigger tasks.
- **Fix:** the worker-wait vtable slot (found by byte pattern, layout of the stock condition checked at run time) points to
  the same steps, with a condition that also wakes the waiter when it sees the tasks done; the trigger task gets a
  reference-counted stand-in event; whichever comes first wakes. Start-up install only (`version.dll`; not on the OBSE64
  route, logged). Self-test: addresses, protocol with fake reserve/trigger threads, a 6-lane stress test.
- **Results:** 8-core cold start: no deadlock (2026-10-23). The test PC limited to 4 cores (a Job object, one logical
  processor per core, and `-corelimit=4 -physicalcorelimit=4`; 2026-10-24), warm cache: the stock game (no DLL) froze in
  the first teleport and ended with the `RenderingThread.cpp:1272` fatal error after 120 s; the DLL with the guard off froze
  at the main menu (same stacks); with the guard on (and the pool) no freeze, also cold at 4 and 6 cores. Not tested on
  NVIDIA (the bug is in UE's task system, not the driver).

<a id="psocompilethreadpool"></a>

### Graphics pipelines prepared on the engine's own pipeline threads (`PsoCompileThreadPool`)

[Explanation](how-it-works.md#psocompilethreadpool).

- **Found (2026-10-23, cold driver cache, UE sized for 6 cores):** the first teleport froze 33-36 s: the game thread in
  `FlushAsyncLoading`, every task worker inside the driver compiling graphics pipelines (`FCompilePipelineStateTask`),
  the loader's shader-preload tasks queued behind them.
- **Fix:** `r.pso.PrecompileThreadPoolSize` (UE 5.3's `FPSOPrecacheThreadPool`, off in this game) set at start-up to the
  logical processors minus 2: precompile and precache pipelines compile on that pool at UE's below-normal priority. Only
  with `TaskWaitDeadlockGuard` installed in mode 1 (the pool alone hung 125 s in the deadlock above); the player's own
  Engine.ini pool setting wins.
- **Results (cold driver cache):** 6 cores (UE-sized, 16 CPUs): no freeze, longest stall 4.2 s (stock: froze >= 33 s); 8
  cores: inn door 5.2 / 4.7 s instead of 6.4 / 16.2 s. Real CPU limit (Job object, 2026-10-24): 4 cores, stock code paths:
  never got from the main menu into the world (stalls up to 176 s); with the pool and guard: into the world, teleport
  (longest frame 26 s) and door done; 6 cores: teleport's longest frame 18 s, no freeze. Warm cache: no difference
  (5 runs, 2026-10-23); 4K, FSR performance, frame generation, 60 fps cap, Imperial City slices, both keys on vs off in
  alternating fresh games (ABBA, 2026-10-24): game thread 6.82 / 6.47 vs 6.89 / 6.61 ms per frame, render thread 3.54 / 3.54
  vs 3.70 / 3.71 ms, stutter 202 / 332 vs 430 / 394 ms: no difference beyond noise. Not tested on NVIDIA.

<a id="havokpickwarningguard"></a>

### The game no longer stops behind a "Havok picks" box (`HavokPickWarningGuard`)

[Explanation](how-it-works.md#havokpickwarningguard).

- **Found (2026-10-20, actors-battle runs lost to a dialog):** the old engine's pick function (`exe+0x65E1B60`) counts
  picks per frame by kind (path / LOS / view caster / other, from `pick+0x48 & 0x3f`) and drops any pick past the
  setting `iMaxPicks:HAVOK` (setting object `exe+0x8FABF50`, default 40, copied to `exe+0x8FAAB98` at Havok init by
  `exe+0x66A8D60`; 31 path picks is a second limit): result cleared, fraction 1.0, no cast. The Havok world's update
  (`exe+0x65E1030`, OblivionThread) then, unless the counting is off (`exe+0x941F986`, set from the frame loop), checks
  `picks > iMaxPicks`, formats `"%d Havok picks (%d path, %d LOS, %d ViewCaster) used this frame."` (`exe+0x861BBB0`,
  or the variant with `(Other type = %d)`) and calls the handler pointer at `exe+0x8FB7CC8` -> `exe+0x691B460` =
  `MessageBoxA(NULL, "WARNING", text, MB_TOPMOST)`; afterwards it zeroes the counters. The box blocks the
  OblivionThread; the game thread then blocks in the pairing session at its next OnWorldPreActorTick, so the game
  freezes until the box is closed (the benchmark's dialog watcher restarts the game). The shipped `ObvData\Oblivion.ini` sets
  its default (`iMaxPicks=40` under `[HAVOK]`); raising it would change which casts the engine skips, i.e. gameplay, so it stays.

- **The change:** the handler pointer (in `.data`; its value checked to be the MessageBoxA function above, the call
  site checked by pattern: counters, cap, the two format strings, `"WARNING"`) points at a wrapper. A call whose
  return address is the pick warning's call site (`exe+0x65E10F5`) is counted and logged (the text three times, then
  counts with the stats) and returns 0 as the handler does; every other caller (`exe+0x699E850`, `exe+0x6A23240`) gets
  the game's handler with the same arguments. The counter reset after the call and the cap in the pick function are
  untouched: the engine skips the same picks as before, with or without the box.

- **Results:** self-test with the real pointer and call site (held back from the call site, forwarded from another
  caller, switch off = the box). Live (2026-10-20): actors-crowd with `iMaxPicks=2` put first under `[HAVOK]` in
  `ObvData\Oblivion.ini` for the run only, so the warning fires as soon as the crowd paths: the log shows the held-back
  messages ("20 Havok picks (20 path, 0 LOS, 0 ViewCaster) used this frame.", then 10 a frame) from the first second
  of the scene, and the run went through at 42.1 fps with 0 restarts, where the stock box would have stopped the game
  on its first frame. Without the forced setting the box is rare (2 of 10 actors-battle runs); the guard costs nothing
  per frame (one pointer, no code on the frame path).

<a id="viewrenameguard"></a>

### Views kept up to date when the graphics card's memory is tidied (`ViewRenameGuard`)

[Explanation](how-it-works.md#viewrenameguard).

- **Found (UE 5.3.2 source and the game executable, 2026-10-12):** `FD3D12View::CreateView`
  (`exe+0x255CDC0`, not inlined; every SRV/UAV/RTV/DSV and texture view goes through it) registers the view as a rename
  listener (the inlined `AddRenameListener` takes the base resource's `RenameListenersCS`, base+0x10) *after* its
  caller read the buffer's location, and stores `ResourceInfo` after registering. The RHI thread's pool
  defragmentation (`FD3D12ResourceLocation::OnAllocationMoved`, `exe+0x2549E80`) rewrites the location without that
  lock, then notifies the listeners under it. A notification between the caller's read and the registration is missed
  (the view keeps the old resource, freed a frame later: UE-217692, the 2026-10-07 crash); one between registration and
  the caller's stores is overwritten. Epic's 5.5 fix creates every view on the RHI thread (5.5.4 `D3D12View.h:418`).

- **The change:** both entries detoured at start-up (5-byte jumps, trampolines in near memory). A view's first
  registration runs under `RenameListenersCS` for the whole original call; before the lock is released the view's
  resource is compared with the location's current one (and with moves of that location in the last second, recorded by
  the `OnAllocationMoved` detour), and a stale view is repaired by its own `ResourceRenamed` (vtable slot 0), as the
  engine's notification loop would. Every move's notification takes the same lock, so it is either complete before the
  check or repairs the registered view after it. UAVs with a counter resource (`FD3D12UnorderedAccessView`, UpdateDescriptor
  `exe+0x2567B50` at slot 2, counter at +0x88) are not repaired under the lock (their rename creates a counter buffer;
  a committed resource, so no allocator lock, but left as stock as a precaution). Lock order checked: the RHI thread
  takes the default-buffer allocator and pool locks, then `RenameListenersCS`; nothing under `RenameListenersCS` takes
  an allocator or pool lock.

- **Results (live, cpu profile, frame fence on):** with 1 GB/frame buffer defragmentation and a race amplifier
  (500 us delay on every 8th registration): 6.3-6.4 M view creations, 1.1 M guarded registrations, 44-51 K moves per run;
  0 stale views found (count-only mode), 56,757 repairs of views of recently moved buffers (repair mode), and a separate
  probe saw 0 stale UAVs in 31 M binds; no crash in 12 launches. Cost: fps 55.0 / 54.2 vs 54.1 / 54.7 without, thread
  times within noise. Images: the same (visual pass with a noise floor; one scene flagged by an NPC standing elsewhere).

- **What it enables:** `RdgParallelTranslate`'s frame fence is used only while `ViewRenameGuard` is installed and on, and
  then buffer defragmentation runs alongside parallel translation: -0.24 GB at 720p (three fresh launches each),
  -0.21 GB at 1440p at a 12 GB card's budget, the same frame rate.

<a id="framegencrashguard"></a>

### Frame generation waits for the graphics card instead of crashing (`FrameGenCrashGuard`)

[Explanation](how-it-works.md#framegencrashguard).

- **Found (three crashes on 2026-10-08 and 2026-10-13 at 4K with frame generation, all at the benchmark's return to the main
  menu):** `EXCEPTION_ACCESS_VIOLATION` reading 0x8 at `amd_fidelityfx_dx12.dll+0x1D9BF` on the RHI thread. The library's
  command-list pool (+0x1D7C0, 6 direct calls; per D3D12 queue type 8 slots {queue, allocator, list, fence, value} after a
  CRITICAL_SECTION) takes the first slot whose fence reached its value and never waits; with all 8 on the GPU it uses a
  null slot. The caller (+0x20222) signals the previous slot and takes the next once per frame-generation dispatch, so
  the queue must be 8 dispatches behind: a long GPU stall (video memory over budget while the world unloads).

- **The change:** when the game loads the library (`LdrRegisterDllNotification`, before its code runs) the function is
  found by its prologue and slot layout and its 6 calls are redirected (rel32 to a jump near the library) to a wrapper:
  under the pool's own (re-entrant) lock it checks the slots as the function will; if none is free it lets the lock go,
  sleeps 1 ms and checks again, at most 3 s, then calls the function. A reloaded library is patched again.

- **Results:** self-test with the game's own library on a WARP device: a queue held by a fence wait, 8 lists taken, the
  9th waits ~220 ms for the queue (the library alone: the same access violation at +0x1D9BF). In the game: installed at
  start-up, 6 of 6 calls. Normal frames always find a free slot (nothing changes).

- **AMD FSR 3.1.4:** the same eight-list failure reproduced at `+0x2016A` in AMD's newer DLL and was prevented by the guard. Later SDK 2.x libraries use 32 slots and need no check.

<a id="restorefsrafteralttab"></a>

### FSR and frame generation back after alt-tab (`RestoreFsrAfterAltTab`)

[Explanation](how-it-works.md#restorefsrafteralttab).

- **Found:** after an alt-tab the game often kept rendering at native resolution with frame generation off, although
  the settings page still said FSR was on. Only changing the upscaler in the settings brought it back.

- **Cause:** the game's focus handler (`exe+0x495EE00`) writes only `r.FidelityFX.FSR3.Enabled`. On focus loss it writes
  0, which also ends the frame-generation threads. On focus gain it writes 1 only if its "menu visible" flag
  (`VAltarUISubsystem+0xE2`) is clear, and nothing re-enables FSR later. That flag goes stale: the menu-visibility
  function (`exe+0x4729660`) sets it when any menu opens but clears it only when the pause menu (the System page) or the
  main menu closes, or a loading screen passes. After the inventory, map, character sheet or wait menu has been opened
  and closed once, every alt-tab in plain play leaves FSR off. And with a menu or a conversation really open at the
  focus gain, nothing switches FSR on when it closes. The game does not open a menu by itself when it loses focus
  (`bPauseOnLossOfFocus` is off; also checked with the full alt-tab message sequence).

- **The change:** the fix ignores the stale flag and uses a signal the game keeps right: the freeze subsystem's count of
  named freezes (`UVFreezeInMenuSubsystem`, read by its own `IsFreezing`), which every menu, conversation and the main
  menu push and pop. After a focus loss that switched FSR off, FSR is switched back on once the window has the focus,
  nothing holds the world, FSR is still the chosen upscaler and still off. It is the same cvar write, with the same
  set-by priority, as the handler's own focus-gain branch. The check runs at the focus gain, after each menu-visibility
  call, and every 100 ms only while a restore is pending (2.7-3.4 microseconds per check). If the player picked another
  upscaler meanwhile, the fix does nothing; FSR quality and the frame-generation option stay as the player set them.

- **Result** (4K, FSR Performance, frame generation, 60 fps cap): plain play after the inventory was used once, then an
  alt-tab: stock stays at native resolution without frame generation; with the fix FSR is back at the focus gain,
  87.8 fps shown / 43.7 rendered. Pause menu open across the alt-tab: 60/60 in the menu, 82.6/39.8 after it closed.
  Also checked: Esc, Tab, closing from another page, the wait menu, a conversation, a loading screen and the main menu;
  sharpness of screenshots confirms the render scale (FSR on 545, off 263, back 548). `Save_Settings.sav` was
  byte-identical before and after every test.

<a id="menuopenflashfix"></a>

### No bright flash when the menu opens (`MenuOpenFlashFix`)

[Explanation](how-it-works.md#menuopenflashfix).

- **Found (GitHub issue #5):** the first time the pause menu, inventory or map opens after some play, one frame shows a
  bright, washed-out landscape behind the menu panels. It happens without OblivionPerfFix too.

- **Cause:** the player menu does not draw its background in 2D. The view switches to a Blueprint preview box
  (`BP_Box_Preview_Inventory`) placed about 336 km outside the playable world: its camera looks at a translucent plane
  showing the menu's screenshot of the game view, inside a large unlit cube that hides the world around the box. On the
  open frame the menu widget (`VBackgroundWidgetBase::RequestCapture`) requests that screenshot and the box's parts are
  shown while the camera is still the game's. When the box's far-away location lies in the game camera's view (UE has
  no far plane), occlusion culling judges the just-shown cube and plane from the old camera, where they are hidden; on
  the next frame, inside the box, it culls both, and that frame shows the world outside the box. It needs more than about
  8 s since the last menu (the engine trusts recent "proven visible" results for that long), which is why a quick second
  open is fine.

- **The change:** `r.AllowOcclusionQueries` is 0 for the open frame only. The request call (game thread, before that
  frame's scene goes to the render thread) sets it with the variable's own set-by priority, and
  `UGameViewportClient::ProcessScreenShots`, which the viewport calls on the game thread every frame right after the
  frame's scene was sent, sets it back to 1. The variable is render-thread-safe, so the render thread sees each change in
  command order and only the open frame renders without occlusion culling. The fix switches nothing unless the restore
  point ran within the last second, the call is on that game thread and the variable's interface checked out on the
  live object; if a restore does not take three times, it stops for the session and the log says what the variable was
  left at.

- **Why the picture is otherwise the same:** without occlusion culling a frame draws more, never less; whatever the
  culling would have removed lies behind opaque geometry and is depth-tested away. Custom depth and stencil effects are
  never occlusion-culled by UE, so they cannot change. UE switches occlusion off the same way for its high-resolution
  screenshots. Not on screen: for that one frame the hidden objects get a fresh "last rendered" time while the game is
  entering the paused menu.

- **Measured:** hidden game, the frame rate capped low so every rendered frame is captured, camera facing the box: fix
  off: the wrong frame in 6 of 6 opens (854x480 and 4K, FSR, frame generation), fix on: 0 of 7; with the fix
  the second frame shows the menu's normal background. It costs one longer frame at menu open.

- **Where:** `src/menus/menu_open_flash.cpp`, `[menuflash]`; installed at start-up with the version.dll route (not on
  the OBSE64 route, which loads after the game's code has started; the log says so), then switched live.

<a id="frameratelimitatstart"></a>

### The saved frame-rate cap works from the start with frame generation (`FrameRateLimitAtStart`)

[Explanation](how-it-works.md#frameratelimitatstart).

- **Found (2026-10-22, game bug, also without the DLL):** with frame generation on, the saved Frame Rate Limit (60 on
  the test PC) was not in effect after a launch: `t.MaxFPS` 0 from the start-up videos through the main menu and the
  world, while the settings page showed 60; showing the Graphics page set it to 60; switching frame generation off kept
  60, on again 0. The same without the DLL in the game folder.
- **Cause:** at start-up the game applies the saved options (`Save_Settings.sav`, `UAltarGameUserSetting::SavedSettings`,
  console commands run through Altar's command setter exe+0x48F7F10). Frame generation's switch (exe+0x495F610) is called
  by the FSR and DLSS console-variable sinks; on "on" it stores V-Sync and the Frame Rate Limit and sets both to 0 (it
  restores them on "off"). Replaying the saved options switches frame generation on after the limit was applied, so
  every launch with frame generation on starts uncapped. The Graphics page applies every row's saved value when it is
  built (the Frame Rate Limit row is not locked while frame generation is on), which is why viewing it fixes it.
- **The change:** the switch's entry gets a 5-byte jump (one atomic 8-byte write). After its first switch from off to on
  in the process, with no menu open, the saved `Altar.GraphicsOptions.FrameRateLimit` is read from `SavedSettings` with
  the game's own lookup and run once through the game's setter with the same name and force off, as the page does; the
  game's own sink then sets the cap. Every later switch is left to the game. Installed only while the switch's state is
  still off: from `DllMain` on the version.dll route, and from OBSE64's start-up hook (after the static initialisers,
  before `WinMain`) on the OBSE64 route.
- **Result:** with the fix, `t.MaxFPS` was 60 from the first sample through the main menu and the world; a Graphics page
  visit changed nothing; frame generation off: 60; on again in play: 0, as stock (left to the game; the next page visit
  restores it).
- **Narrowed in 1.2.2 (2026-10-26, worker framecap2; findings "2026-10-26 (worker framecap2)"):** two players with NVIDIA
  DLSS frame generation reported a much lower frame rate with the key on (120 cap -> ~29 fps; 90 -> 50-60), normal with
  it off. From the exe: the DLSS sinks (exe+0x495F160 / 0x495EF90) are per-frame console-variable sinks like FSR's and
  only check that the mode is a valid `UStreamlineDLSSGMode` value, so the start-up T(on) and the lost cap happen with
  DLSS too. T(on) also sets `Altar.Reflex.Mode` 2 when its Reflex flag (exe+0x930A86C: hardware GPU scheduling on,
  GRHIVendorId 0x10DE, Reflex available; so always 0 on AMD) is set, and with Reflex on, Streamline's MaxTickRateHandler (exe+0x44C2C00, `t.Streamline.Reflex.HandleMaxTickRate`
  default 1) takes `t.MaxFPS` as Reflex's `frameLimitUs` (only on frames faster than the cap) instead of the engine's own
  sleep. That path, and how NVIDIA's limiter treats it with DLSS frame generation, was never measured (AMD test PC). The
  fix now acts only when the switch came from FSR's sink (exe+0x495F380, by T's return address), T's Reflex flag is 0 and
  the limit is still 0; otherwise it logs and leaves it to the game.
- **And not on NVIDIA (1.2.2, 2026-10-26, worker rel122fix after review rel122review; findings "2026-10-26 (worker
  rel122fix)"):** the Reflex flag means "T switches Reflex on" (HAGS && NVIDIA && feature), not "Reflex is off": an NVIDIA
  PC with hardware GPU scheduling off and Reflex on from an old saved `Altar.Reflex.Mode` or Engine.ini would pass that
  gate. The hook now also leaves it to the game when GRHIVendorId is 0x10DE (or not known, 0), read through the vendor
  check T itself calls for its flag: T's lazy initialisation block (exe+0x495FC0C, unique pattern; checked to jump back
  into T and to store into the flag T reads) calls exe+0x265B230 (`cmp GRHIVendorId,0x10DE / sete al / ret`, checked
  bytes). So the key acts only with FSR frame generation on AMD and Intel GPUs; on NVIDIA it is the stock game. Self-test:
  the variable found; NVIDIA with flag 0, vendor 0 -> left; Intel -> applied.

<a id="frozenclothstretchfix"></a>

### Frozen cloth keeps its shape (`FrozenClothStretchFix`)

[Explanation](how-it-works.md#frozenclothstretchfix).

- **Found (2026-10-13, Ilav's robe stretched in play; full process dump):** the robe's cloth was suspended, at LOD 1,
  with zero solver time, and its 353 particles were 6.6-7.8 m from their animation targets; the renderer's positions
  matched the solver's, so the fault was in the CPU-side cloth state. Stock `WriteSimulationData` (`exe+0x53CF250`)
  calls `Solver::Update(0)` (`exe+0x53CF40B`) when a suspended cloth's LOD changes; that moves the reference frame with
  the asset's ordinary velocity scales (robe linear 0.5, guard 0.3) and no physics step follows.
- **Reproduced without the DLL:** a frozen guard moved 1000 cm, then forced to LOD 1: every particle 300.0 cm behind; the
  same with the DLL removed. Recreating the proxy repairs it.
- **The change:** only that call goes through a stub. When the proxy has no running task and the component is
  explicitly suspended, each cloth's teleport flag (+0x45) is set before the update unless a reset (+0x44) is pending;
  `Cloth::Update` already applies teleport by zeroing the velocity scales and clears both flags. Positive time steps,
  running cloth and the normal simulation tick are untouched. All layouts are checked by pattern at start-up.
- **Results:** guard 300.0 cm -> 0.0001 cm; grey robe (Ilav's asset) 500 cm -> median 0.24 / max 2.83 cm (its own drape);
  rotation, LOD 2 and the non-adjacent LOD 2 -> 0 also clean; ordinary simulation after resuming unchanged.

<a id="shaderscreenafterdriverchange"></a>

### The first-launch shader screen after a driver change (`ShaderScreenAfterDriverChange`, game bug)

[Explanation](how-it-works.md#shaderscreenafterdriverchange).

- **What decides it** (static, build 19115871): at start-up exe+0x44E1290 sets the start-up movie player's +0x89 to
  "no Save_Settings save" (UGameplayStatics::DoesSaveGameExist at exe+0x31A4F30, called at exe+0x44E12C4: the platform
  save system's vtable +0x10). exe+0x44E8350 adds the ShaderLoadingScreenMovie, its timeout and the precompile callbacks
  (begin: r.ShaderPipelineCache.MaxPrecompileTime saved and set to 0; complete; clean-up: restored, SetBatchMode
  Background) only while +0x89 is set; only those read it. The screen's widget (exe+0x44E2330) shows "Shaders
  preloading:" and "Compiling shaders to optimize performance during gameplay." This also explains the PCGamingWiki tip
  (rename Save_Settings.sav).
- **The change:** that one call goes through a wrapper. It compares DXGI's vendor, device, subsystem, revision and
  user-mode driver version of the hardware adapter with the most dedicated memory with `OblivionPerfFix-gpu.txt`; on a
  difference it answers "no save" to this caller, and tells PsoPrecompileFast to apply UE's Fast values (16 ms, no
  cold-cache guard) until the main menu: the game itself asks only for Background behind the screen. The record is
  written when PsoPrecompileFast sees the precompile end.
- **First build, without the Fast pace:** with a warm cache the screen held start-up past the benchmark's 180 s
  main-menu limit on four launches in a row (closed by the runner each time, no crash): 8 pipelines per frame at 1 ms,
  the precompile unfinished after 162 s.
- **Result** (2026-10-28, probe games, warm cache, the tester's own settings, the record set to an older driver): the screen
  shown, 49.3 s at the Fast pace, the precompile finished at +73.9 s after load and the world at +90 s; the record
  written then. The same launch without the change: world at +24 s, precompile finished at +82.9 s (about 59 s of it in
  play at the game's pace).
- **Cold cache** (2026-10-28 20:30-21:25; NVIDIA Shader Cache Size disabled, the game's user pipeline cache
  moved aside before every run, DXCache unchanged at 135 files throughout; a test build; native-nofg; fresh launch
  straight into silver-road-north with -SkipPsoWait -NoWarmup; off = the switch at 0, on = 1 with the record set to an
  older driver; 3 flights each, medians and ranges):
  - Start-up, DLL load to the world: off 11.8 s (11.7-13.0), on 204 s (197-208). The precompile ended at +254 s
    (253-265) in play with the switch off, at +172 s (167-176) behind the screen with it on (156-166 s at the Fast pace).
  - Flight: worst frame off 2108 ms (1965-2253), on 335 ms (275-473); 1 % low off 64 (58-64), on 75 (70-78); hitches
    off 59 (55-67), on 91 (85-93); hitch time off 7.8 s (7.5-8.1), on 9.6 s (9.0-9.7); fps 107 vs 109. No range
    overlaps for worst frame, 1 % low, hitches and hitch time.
  - Why the screen had more hitches here (the same runs, read again): every on-side hitch is the game thread waiting
    for the render thread (ledger: post_ms ~ sync_ms), at the same route spots in each run, mostly 60-250 ms; UE's PSO
    pool is idle in play (2-18 % of a core) while NVIDIA's driver threads compile (100-165 % in the first 60 s). Off:
    about half the hitches are game-thread actor time and about 25 per flight are render-thread waits; off's 2.1 s worst
    frame is the flight's first frame. The cause, from the code: UE's runtime PSO cache drops PSOs not used since the
    last check, every r.pso.evictiontime seconds (60; exe+0x26376C0 calls the cache's discard-and-swap at
    exe+0x2633EA0), and counts on the driver's disk cache to make the next creation cheap. With that cache switched off,
    the PSOs the screen built were long gone by the flight and were rebuilt from scratch on the render thread. This
    condition does not occur after a real driver update (an empty but writable cache), so it is a test artefact.
  - Harness note, the same for both sides: the runner moved the game's user pipeline cache aside before every run.
- **Cold cache, writable** (2026-10-28 23:03-23:54; NVIDIA Shader Cache Size back at Driver Default, the DXCache files
  moved aside (same volume, never deleted) before every run so each launch starts with an empty cache the driver can
  write; 20 files held open by other programs stayed, none of them the game's; the rest as above; order off on on off
  off on, then on on):
  - Flight: off (3) fps 98.8 (98.1-100.3), 1 % low 60.7 (60.2-60.9), worst 2041 ms (308-2049), hitches 41 (39-46),
    hitch time 5.0 s (3.9-6.4); on (2) fps 102.1 / 103.6, 1 % low 68.8 / 72.6, worst 50 / 56 ms, 1 hitch each, hitch
    time 0.04 s. Ledger: no game-thread frame of 40 ms or more in either on flight; off 43-47, mostly actor time.
  - Start-up: the precompile ended behind the screen at +165-178 s after load with the switch on, in play at +255-260 s
    with it off. The first play frame after the teleport to the Silver Road (before the flight, not measured) took
    19.9-20.6 s off and 8.5-9.2 s on (post_ms, not a render-thread wait).
  - Three of the five on launches stopped there: the OblvBench command sent during that 8.5-9.2 s frame timed out (the
    bridge waits 8 s) and the runner gave up on the flight. Lab only; the two that passed sent it after the frame.
  - Reading: with a writable cache the screen removes the flight's shader hitches almost entirely. Few runs (2 on), but
    no overlap on any measure. On by default: it restores the game's own first-launch
    behaviour where the game skips it, and the world does not run behind the screen; the cost is the screen's wait
    (about 3 minutes with an empty cache), once per driver or card change.
  - Harness, lab only: the runner's 180 s main-menu limit raised for these runs ($env:OPF_MENU_WAIT_SEC), and the
    OblvBench bootstrap's frame-loop wait raised from 20 to 600 s (the screen runs no game frames; with 20 s the
    bootstrap fell back to its async poll and the flight could not start: two screen runs lost, repeated).
- **Upstream's cold-cache guard on NVIDIA** (found on the way; fresh launches A B B A A B B A A B, warm cache, A =
  PsoPrecompileColdQueue 1000, B = 0, which also lifts the menu budget to 16 ms): waiting at the main menu, A 48.6 s
  (46.8-49.0) and B 48.1 s (47.1-48.7) to the precompile's end, no trips. Going straight into the world (the tester's own
  settings), A 73.0 s (68.3-74.0) with the guard tripped 5 of 5 times at +33 s on "driver threads" (NVIDIA's D3D
  background threads at 99-123 % of a core while the world loads), B 59.4 s (57.7-61.8), no trips. A false trip with a
  warm cache: the driver is compiling the new world's pipelines, not a cold cache. Unchanged here.
- **Self-test:** the call at exe+0x44E12C4 to DoesSaveGameExist at exe+0x31A4F30 and the flag store; the wrapper with a
  fake save check and a fake card: first record, unchanged, changed (the Fast hold set, the old record kept until the
  precompile ends), no settings save at all.

<a id="vramcounterfix"></a>

### The "Show VRAM" counter (`VramCounterFix`, game bug)

[Explanation](how-it-works.md#vramcounterfix).

- **Reports:** Steam discussions for the game: the counter reads 0/0 on AMD and NVIDIA cards; one player quotes support
  calling it a known bug. Reproduced here (RTX 5090, with the fix off): "VRAM: 0/0".
- **What draws it** (static, build 19115871): the widget class AltarGraphicsSettingsDebugInfo formats VRAMText as
  "VRAM: {0}/{1}" in its update (exe+0x48C2170), with the two outputs of one query, exe+0x47B6AF0, called at
  exe+0x48C2511. The Blueprint function GetUsedVRAMMB is a thin exec thunk around the same query (its call at
  exe+0x46019EB). The query zeroes both outputs, then reads GDynamicRHI (exe+0x9206108, as pinned by RdgSubmitBatch's
  self-test), calls its vtable+0x30 (the RHI behind any validation layer), takes the first adapter of the array at
  +0xC8, and scales two 64-bit fields at +0x760 and +0x768 by 2^-20 (MB). The widget passes the +0x768 value as {0}
  and the +0x760 value as {1}: the order of DXGI's video memory info (budget, then current use), so the counter was
  meant to read use / budget.
- **Why 0:** nothing in the shipping build refreshes those two fields; read in a probe game they stay 0 (the counter shows
  0/0 for the whole session).
- **The change:** both call sites go through one stub to a wrapper that calls the game's query, then, only if both
  results are 0, writes the monitor thread's latest local-segment DXGI sample (`QueryVideoMemoryInfo` on the GPU the
  game uses the most memory on: budget and current use, in MB). The game thread only reads two atomics; the sample is
  at most a second old. If the game's own numbers are non-zero they are passed through and counted.
- **Result** (2026-10-28, probe game, IC Market, Show VRAM on, switched live): on "VRAM: 7,635/31,419", off
  "VRAM: 0/0", on again "VRAM: 7,571/31,419"; the log's first fill was 8038 / 31419 MB (the 5090's budget is 31.4 GB).
- **Self-test:** the query, both calls and GDynamicRHI at their build-19115871 addresses, both calls going to the stub;
  the wrapper with a fake query and a fake sample fills 0/0, passes the game's own numbers through, leaves 0/0 before
  the first sample and while switched off.

## Memory safety and diagnostics

<a id="memoryrelief"></a>

### Memory given back when Windows runs short (`MemoryRelief`, `GpuCrashReport`)

[Explanation](how-it-works.md#memoryrelief).

- **Observed crash:** a GPU driver timeout after 31 minutes of 4K play with frame generation left 0.87 GiB of 31.9 GiB RAM and 1.72 GiB of 63.9 GiB commit free; the game held 21 GiB of private memory. Windows watchdog dumps show timeouts on this PC months before the mod. One event cannot exclude the extra ~0.7 GB video memory and up to ~0.5 GB RAM retained by `RdgParallelTranslate`, `LumenKeepArrayCapacity` and `UploadMemoryKeep`.

- **Current behaviour:** `MemoryRelief` checks once a second. RAM below 1 GB for three seconds suspends upload retention; commit below 1 GB also lets Lumen release spare capacity. `OsPageCache` returns spare blocks during either shortage. Normal retention resumes after 2 GB has been free for 60 seconds. Commit includes page-file growth room, capped per volume at free space minus 4 GB; this avoids treating Windows' page-file growth (63.9 -> 65.9-69 GB in testing) as a shortage. GPU scene capacity is unchanged. No fix is switched off for video-memory pressure; `CommandAllocatorTrim` releases unused command memory instead.
- **Release ramp:** retained upload pools are released oldest first over 900 frames, about 15 seconds at 60 fps, down to the game's 20-frame lag. After the same 854x480 flight, the longest clean-up was 54.8 ms with the ramp and 111.2 ms without (one run each). Releasing everything in one call had cost ~140 ms, with a 136 ms present. Pools from one burst still expire together.

- **Live RAM check:** relief engaged at 0.94 GB free during an 854x480 run and stayed engaged (about 1.1 GB free parked, 0.4 GB during transitions).
- **`GpuCrashReport=1`:** the handler finds the real D3D12 device on the crashing thread and writes the removal reason, DRED state, unfinished command lists and pass markers, page-fault details and the last 30 seconds of memory use. A device-creation hook only saw test devices because AMD's AGS extension creates the real one internally. A software-device self-test and deliberate live device removal verified the report: written in 2 ms, followed by the engine's normal crash path. It only reads state and writes the log.

<a id="commandallocatortrim"></a>

### Command memory given back after loads (`CommandAllocatorTrim`)

[Explanation](how-it-works.md#commandallocatortrim).

- **Found:** counting `FD3D12CommandAllocator` constructions per queue (`exe+0x24ED9D0`, r8d = queue type) through
  six scenes with teleports between them (720p): the stock game ends with 475 allocators (84 direct, 389 copy), the
  DLL with parallel translation 607 (~152 direct). The pools jump at every load and never shrink: every buffer or
  texture created with initial data gets its own copy context, allocator and submission (`FD3D12CopyScope`), so a
  world load leaves 350-450 idle copy allocators. Releasing them showed what each holds: 311 copy allocators -> 0.27
  GB less local video memory, 444 copy + 42 direct -> 0.50 GB (per-second DXGI budget log): about 1 MiB each, the
  driver's minimum command chunk, also for tiny copy lists. (RMV had shown 400-640 resident command chunks, 430-490 MiB.)

- **Cause:** UE 5.3 keeps a lock-free pool per device and queue (`ObtainCommandAllocator` `exe+0x2527730`,
  `ReleaseCommandAllocator` `exe+0x252E2E0`, pool at `Queues[type]+0x240`, queue stride 0x300); a released allocator
  is reset and pushed back, never destroyed, and the driver keeps an allocator's chunks across resets. A pool is as
  large as the most allocators ever in flight at once.

- **The change:** the obtain, release and constructor entries are detoured (5-byte entry jumps, trampolines in near
  memory) to count allocators in flight per queue and their peak over the last 20 s. Once a second, if the pool has a
  clear surplus over that peak (plus max(8, peak/8)), the monitor thread pops its free allocators with the engine's own
  lock-free pop (`exe+0xDE3320`, link table `exe+0x8FFF980`), swaps the surplus wrappers' `ID3D12CommandAllocator` for
  an inert stand-in (its `Reset` returns S_OK; the release path calls it before pushing), pushes every wrapper back
  through the engine's own release (stand-ins first, so the real ones are on top), then releases the real allocators
  four at a time with 25 ms pauses, at most 32 a second. A wrapper popped by the engine with the stand-in in it gets a
  new allocator from the engine's own constructor in the obtain detour. Only the obtain function pops this pool (all
  28 calls of the pop function checked). Policy: only the copy pool, when the game is within 2048 MB (0.5 GB hysteresis)
  of its DXGI budget, with a 3 s window once it has been over the budget for 10 s in a row.

- **Results:** mean local VRAM over six 720p scenes 8.25 / 8.31 / 8.26 / 8.56 GB trimming always vs 8.48 / 8.78 / 8.87
  without (-0.36 GB), fps 58.1-58.4 vs 57.6-57.9, 1% lows 46.0-46.1 vs 44.2-45.4. At 1440p with a 12 GB card's
  budget (11.27 GB), the inn-exit peak was 11.61 GB with every fix vs 11.97 GB for the stock game. Re-creating an
  allocator costs ~0.03 ms (258 in 6.7 ms).

- **Pitfalls met:** (1) releasing hundreds in one run held up the game's own submissions (418 in 415 ms: frames of
  174 / 79 / 158 ms at 1440p; 395: 194 ms; 524: 224 ms; nothing at 720p); four at a time with pauses: frames inside the
  release windows at most 37 ms (median 16) and the scenes' worst frames 19-73 ms instead of 31-224 ms. (2) Trimming the
  direct pool made the engine create ~300 direct allocators again in 5 minutes on a roomy card (~60 MB), and ~960 in 6
  minutes over an ~8.2 GB budget, where each new one took fresh chunks from a paging driver and two scenes ran 9-19%
  slower than the stock game: direct and compute pools are never trimmed. (3) A second process patching the constructor entry first makes the fix stand
  down ("the constructor is not the expected one").

<a id="hitchledger"></a>

### The stutter ledger (`HitchLedger`)

[Explanation](how-it-works.md#hitchledger).

**Verification.** The self-test measured about 0.15 microseconds per frame. In-game overhead has not been quantified here. The diagnostic records frames of at least 25 ms by default, including the cap wait, plus ten-second summaries for rates per hour. A fixed memory queue keeps disk writes off the main thread; a background thread writes once a second, up to 48 MB, keeping the previous recording. It reads clocks and counters without changing game results. Garbage collection is part of the remaining frame time rather than separately timed; missing instrumentation is recorded as -1. `HitchLedger=0` is the default; set it to 1 before starting the game to install the recording hooks.

**The tick split (`HitchLedgerTickSplit`, lab since 2026-10-16, public since 1.2.3, off).** Redirects the
tick task's `ExecuteTick`, `FTickableGameObject::TickObjects`' per-object `Tick`, the timer manager's native, dynamic
and TFunction delegate calls, plus six of UWorld::Tick's direct calls (physics set-up, the collision tree, latent actions,
timers, tickables, streaming) to timing stubs, each found by pattern and checked as a call into the game's code; any
mismatch leaves that part untimed with a log line. Game-thread time per (kind = the tick function's `ExecuteTick`,
class of the ticked object, released group), exclusive of nested ticks; ticks on other threads per kind. Fixed tables,
no locks or allocation on the game thread; the monitor thread writes 2 s windows and each slow frame's top 3 to
`OblivionPerfFix-ticksplit.csv` (~0.3 MB per minute in our runs, capped at 64 MB). Cost: ~45 ns per timed call in the
self-test; in a held crowd (~1,300 timed calls per frame, a 2026-10-16 lab build) live A/B 4 x 30 s: 33.37 vs 32.93 ms per frame
(+0.44 ms, +1.3 %, within the noise). With 0 at start-up nothing is installed. `tools/tick_split.py` names the kinds
and ranks the windows (per benchmark scene with `--run`).

## Tried and not shipped

| Idea | What it gave | Why it is not in the fix |
|---|---|---|
| Bundling AMD's signed FSR 3.1.4 DLL | Enables the driver's ML frame generation upgrade on Radeon RX 9000; no frame-rate gain measured against the game's own frame generation | AMD's 6.7 MB file is an optional, separate download: see [the README](../README.md#optional-amd-ml-frame-generation-radeon-rx-9000). The mod supplies camera information, pacing tuning and the crash guard |
| Patching AMD's frame generation busy-waits (presenter QPC loop, fence spins) | two external spinning threads cost FG-off runs ~0 fps in Cheydinhal at 4K (chapel -4) | not where the frame rate goes in these views |
| `r.FidelityFX.FI.UIMode 1` (HUD extracted instead of redrawn for generated frames) | FG-on Cheydinhal views within noise (the benchmark camera has no HUD) | no measured gain; can show HUD artefacts on generated frames |
| `p.Chaos.EnableAsyncInitBody=0` (the game ships 1) | Echo Mine walk at 4K: AddToWorld and physics-state samples about doubled, worst present 41 ms | the game's own setting is already the better one |
| `s.AdaptiveAddToWorld.AddToWorldTimeSliceMax 2` (game 4), `s.LevelStreamingComponentsRegistrationGranularity 2` (game 10) | the same spike window within run-to-run noise (4K, 2 + 2 runs each) | no gain; smaller slices only make cells appear later |
| Collision index settings: `p.Chaos.AccelerationStructureUseDirtyTreeInsteadOfGrid 0` (dirty elements in a grid), `p.aabbtree.DynamicTreeLeafCapacity 32` (game 8) | Echo Mine walk, 480p with the 60 fps cap, 3 runs each in one game: the spike window's worst frame and time over 25 ms within the defaults' spread | the inserts they cheapen run on the physics helper; no gain |
| Deferring the immediate insert of new bodies into the game thread's collision structure | ~35 ms of game thread spread over the spike second at 4K | queries (movement, traces) must see newly streamed collision in the same frame; deferring risks falling through new ground |
| Spreading GPU work of newly streamed geometry (`r.RayTracing.Geometry.MaxBuiltPrimitivesPerFrame`, `r.LumenScene.SurfaceCache.CardCapturesPerFrame`) | at 480p (where that work is the same) the spike window's GPU time equals a quiet stretch's: the 4K GPU spike is resolution-bound work, not these builds | would not help; also a look change (new objects' ray-traced lighting a few frames late) |
| Hardware ray tracing off | +39-46% fps | Changes the look |
| `r.Lumen.HardwareRayTracing.LightingMode` 0 instead of 3 | +8% fps, halves the ray-tracing shader bindings | Changes the look (hit lighting); kept at 3 |
| `r.RayTracing.Culling.Radius` 5000 (from 30000) | +10% fps | Water reflections visibly change (sharper, bluer) |
| Starting the world behind the loading screen (an alternative to `PrepareCollisionBehindLoadingScreen`) | Removed the visible slow frame | The world and your controls ran about 1.5 s before you could see them. That is not the stock experience, so it was removed |
| The game's own "fast transition" system (`bIsFastTransitionEnabled` in `Altar.ini`) | Leaving an inn took ~1 s instead of ~3.6 s | The game leaves it off: the next door stops working, hangs and a crash on returning to the main menu, missing light through windows, interiors 25% slower |
| A longer black screen after doors | Fade-in clean | The same hitches simply move to after it |
| `CommonLoadingScreen.*` settings | Nothing | Dead code in this game |
| Switching parallel translation on and off automatically by RHIThread load | Avoided the GPU cost before batching existed | Not needed once batching removed that cost; switching it on at runtime also exposed the descriptor crash |
| Pinning the GameThread and old-engine thread to their own cores | Old-engine thread -16..-40% | No fps change in 11 runs; hardware-specific |
| `tick.AllowAsyncTickDispatch`, `tick.AllowAsyncTickCleanup`, `a.ForceParallelAnimUpdate` | ≤ 0.07 ms | Too small, and some change the order objects tick in |
| `r.Streaming.PoolSize 6000`, `p.EnableDeferredPhysicsCreation 1`, finer level streaming budgets | No effect on travel hitches | Nothing to gain |
| `r.Nanite.MinPixelsPerEdgeHW` | -0.4..-0.8 ms in one rainy test | No gain at real settings |
| UE 5.4+ defaults: PSO precompile pool, `r.RayTracing.AsyncBuild`, transient skin-cache scratch, five GPU-side 5.8 defaults, kinematic deferral, a bigger upload heap with DRED off | Nothing beyond noise in those test scenes | No gain there; `GpuSceneParallelUpload` has a separate gain when large areas stream in |
| `r.RDG.Events=0` | RenderThread -0.3 ms per frame | No fps change on the test PC; it also removes the pass names GPU crash reports and profilers rely on |
| `D3D12.ResidencyManagement=0` | Submission thread -16..-21%, RHIThread -6% | No fps change on the test PC, and risky on graphics cards with less than 16 GB of memory (the game uses ~9-10 GB at 4K and briefly up to ~13 GB after a door) |
| NPC perception at 2.5 Hz instead of 5 Hz | +10% fps in a 40-NPC crowd | NPCs would notice things half as often: a gameplay change |
| `bAlwaysCheckFloor` off for NPCs | +4% fps in a crowd | A standing NPC would not notice the floor changing under it: a gameplay change |
| Foliage reflection roughness 0.2 (game: 0.4) | +12% fps in rain | Some leaf highlights get dimmer |
| Shadows "High" instead of "Ultra" | 13-25% less GPU time outdoors | Visible (dappled leaf shadows): a choice for each player in the game's own menu |
| Internet "Engine.ini tweak packs" | - | Many keys don't exist in UE5 or this version, and comments after a value aren't comments in Unreal ini files |
| `ClothRegisterSkip` | No cloth build skipped at Peryite's shrine | The first build supplies drawing bounds, so omitting it would need another way to compute identical bounds |
| `OblivionThreadCaches` | ~97% fewer shared-counter trips; 131,000 frees/18,500 allocations became ~2,000/~400; Windows returns 163-177 vs 246-261; one crossing 26.8-28.2 vs 29.6-31.3 ms | Across crossings and town scenes, frame times stayed within noise; unloading itself dominates |
| `PhysicsPushGrowParallel` | Three big batches 4.3-5.6 vs 7.8-8.7 ms at 480p; 5.2 vs 8.9 ms at 4K, about 1-1.5 ms per affected frame | Too small to show in frame totals |
| `PhysicsFlushThreeWay` | Swap steps 4.3-7.1 ms (mean 5.4) vs 4.7-10.4 (7.4); own-copy work 5.6-6 ms, list 2.4-3.1 ms | At 4K, 8.8/5.0 and 11.1/7.3 ms on vs 9.9/5.6 and 9.3/5.8 off: no measurable gain |
| `TreeBatchReserve` | Enlargements 161-182 -> 19-21, but filing 27.7 ms on vs 26.5 off; swaps 5.6-9.0 vs 6.1-8.6 ms | First writes into fresh Windows pages still cost the same; frames unchanged |
| `TreeLeafTransfer` | 124,535 entries compared with no difference; swaps 9.5-13.2 ms on vs 7.1-11.1 off | No frame-time gain; the fresh memory's first write dominates rather than the removed copy |
| `PerceptionIgnoreReuse` | 440,971 comparisons, zero differences; 1.4% conservative fallbacks; crowd 510.7 vs 544.5 microseconds per round (-6.2%, about 0.3-0.35 ms/frame); repeat -35 microseconds; battle -31 to -35 (-9%) in five noisy pairs, then -8 (-3%) as it thinned | Small saving, below whole-frame noise; not included in this release |
| `MeshMergeReserve` | Temporary-array growth calls ~400 -> ~230 per minute, but mixed timings and unchanged NPC-burst waits at both small and 4K settings | No measurable gain |
| Reusing immediate command allocators for 16 lists | Direct pool 156/157 -> 113/117, about 40-43 MB saved; same frame rate | Too little saving for a change to the main command lists' memory use |
| Fewer, larger translation lists (`r.RDG.ParallelExecute.PassMin` 4 or 8) | A third fewer command lists; 8.49 vs 8.51 GB | No video-memory saving |
| A cap on simultaneous translation jobs | Limits 2/4 gave no fps gain in a 40-NPC crowd or Imperial City market; 8.60/8.50 GB vs 8.42-8.54 without a cap (two fresh launches each); limit 2 lost ~2.5 fps in 1% lows | Neither CPU nor memory benefit |
| Keeping a character's first animation instance | No measurable gain in the repeated appearance set-up | Leaves animation set-up different from the game's |
| Releasing upload pools after 3600 frames | Pools from one load expired together: ~140 ms of driver calls and ~60 ms render-thread wait | Creates a later hitch instead of removing the round trip |
| Pausing retention or translation when over the video-memory budget | Other apps used 4.3 GB of a 16 GB card; budget fell 15.06 -> 10.96 GB. Fresh worlds used 9.0-9.8 GB, played worlds 10.4-11.4 GB; a trial stayed at 11.0-11.3 GB, 44-57 seconds per minute over. Three ~8.2 GB-budget tests rose 8.30 -> 8.64-8.75 GB and added 25-73 ms clean-ups; translation pause saved only ~0.2 GB. A simulated-budget test fell 9.54 -> 9.30 against 8.79 GB and still failed | Trials did not reliably restore headroom and sacrificed useful CPU gains; RAM/commit relief remains enabled |

## What still limits the game

- **The GameThread, in crowds and battles.** Each standing NPC costs about 0.4 ms of GameThread time per frame.
  - **Perception:** the game's perception code checks the line of sight between every pair of NPCs five times a second
    (1-3 ray casts per pair), 4 ms per frame in a 40-NPC crowd.
  - **Waiting for the old engine:** the GameThread waits for the old engine's thread every frame (2-6 ms), whose
    per-actor scene-graph update dominates its time.
  - **Everyday checks:** standing NPCs still re-check the floor under their feet, run overlap queries and move their armour
    and weapon bodies with their idle animation.

  All of these produce real results, so nothing can simply be skipped. In a 40-NPC crowd the GameThread needs 25-27 ms
  per frame, and in battles up to 35 ms.

- **The GPU at 4K.** In the rainy forest near Leyawiin, the biggest costs per frame are:
  - virtual shadow maps (the Nanite shadow raster: 6.4 ms);
  - Lumen reflections (5.1 ms, mostly wet foliage);
  - Nanite's visibility buffer (2.8 ms) and post-processing (2.8 ms).

  These come from the game's above-stock quality choices. Async compute can't be used, because the game was built without
  inline ray tracing. We found nothing to save without changing the look.

- **Fast travel across the landscape** at high speed: the GameThread is saturated. We saw no blocking streaming waits.
- **A group of NPCs arriving at once** while travelling: when the old engine attaches a new row of cells, the game
  creates all of its NPCs and creatures within a few frames (on the forest route: five worshippers at Sheogorath's
  shrine with their clothing and weapons, 31 actors in 4 frames of 40-110 ms). Most of it is the game's own work on the
  GameThread: spawning and registering each actor and its clothing pieces, initialising every skeletal mesh's animation
  twice (once at registration, again when the game swaps in the NPC's real body a frame later), Blueprint code, and
  waits for the allocator's global lock while helpers compile the new materials' ray-tracing pipelines. `NpcCreationPacing` spreads far-away, unseen creation over frames. Individual body/head and robe steps still cannot be split (about 16-22 and 13-21 ms respectively).

- **New materials streaming in** keep the background helpers busy compiling ray-tracing pipelines; the render thread
  can then wait for the ray-tracing instance setup (40-50 ms frames at one spot of the route).

- **Doors:** the big stalls (the old engine attaching the cell, 1.2-2.9 s; re-reading about 2.4 GB when leaving a city
  interior) happen behind the loading screen, where you don't see them.

## Game behaviour worth knowing

- **Start-up pipeline compilation:** the stock game builds pipelines in the background for about 100-200 seconds. Measurements found canal frame rates of 34-36 instead of 41-53 fps while this work ran, and door/fast-travel screens up to five seconds longer. `PsoPrecompileFast` uses the fast pace in menus and loading screens: a warm-cache precompile finished 22-25 seconds after launch at 480p and 27 seconds at 4K. In play it returns to the game's pace.

- **After a driver update or a fresh install,** the graphics driver compiles shaders in the background for ~6 minutes,
  keeping four cores busy, with nothing on screen to say so.

- **Don't minimise or alt-tab while the game is starting up.** Minimising before the main menu appears crashed it three
  out of three times, with and without our fix (a D3D12 copy-queue error).

- **FSR switches off when the game's window loses focus**, and frame generation with it. Without our fix, an alt-tab
  often leaves it off until you change the upscaler in the settings, because the game thinks a menu is still open once
  you have used the inventory, map or wait menu (`RestoreFsrAfterAltTab` explains why). With the fix it comes back as soon as no
  menu is open.

- **UE4SS Lua mods can grow the game's memory over time.** In our test harness unpatched UE4SS 3.0.1 kept about 1 GB/h of Lua
  wrapper objects; the benchmark build includes a fix for that leak. The game itself does not leak: no object build-up and no frame-rate drift over 105 minutes.

## Reproduce it

- [benchmark.md](benchmark.md) runs the scenes above and compares runs with and without the fix.

- `src\OblivionPerfFix\build.ps1 -SelfTest` checks every patch site against your installed game without running it.
