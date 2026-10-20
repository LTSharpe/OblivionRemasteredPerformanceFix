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

<a id="animteardownparallel"></a>

### Animation data taken apart on helper threads (`AnimTeardownParallel`)

[Explanation](how-it-works.md#animteardownparallel).

**Measured (2026-10-17, 480p `small`, Kvatch wall flight, on/off/off/on in one game):** at the heaviest crossing the
NPCs' animation teardown 9.4-10.0 -> 4.9 ms, the row change 30-32 -> 26 ms and the game thread's wait 33-35 -> 28 ms; at
the next one 7.2 -> 4.2 ms of animation teardown; light crossings unchanged. About 1 million interpolators went through
the helpers with no fallback and no crash. It shortens the worst crossings by ~5-7 ms; it does not remove them (the rest
of the row change is other unloading and loading). `AnimTeardownParallel=1` is on by default.

<a id="npcperceptionparallel"></a>

### Line-of-sight checks on helper threads (`NpcPerceptionParallel`)

[Explanation](how-it-works.md#npcperceptionparallel).

- **Found (2026-10-19, i7-12700 / RX 9070, `actors-crowd` at 480p):** a stack profile of the game thread (200 Hz,
  dbghelp) put 10 % of its wall time under the characters' perception round (`exe+0x485FA60`, a timer delegate of each
  character), 8.5 % of it in the per-pair line-of-sight test (`exe+0x485A170`: collision params with the attached
  actors of both as ignored, then up to three `UWorld::LineTraceSingleByChannel` -> Chaos ray casts). The fix's counting
  mode measured 9,351 rounds a minute, 56 candidates and 20.5 pair tests per round, 18.6 us per test: ~1.3 ms per frame.
- **Change:** the gather call of the round is redirected (with `self`, which the loop keeps in `rdi`) to a hook that
  replays the loop's checks and tests the pairs on helper threads with the game's function; the pair-test call is
  redirected to a hook that returns the prepared result for that pair. The round, its maps and its old-engine messages
  are the game's.
- **Verified:** self-test with 64 fake candidates (results identical and in order, helpers take part, fallbacks); in the
  crowd 184,393 pair tests in a minute, every one prepared in advance (79 % on helpers), a round 107 us instead of ~440.
- **Measured (2026-10-19, 480p `small`, off/on alternated, `-Session auto`):** `actors-crowd` 2 rounds, medians:
  44.4 -> 46.8 fps (+5.5 %), 1 % low 36.9 -> 40.7 (+10 %), game thread 19.82 -> 18.05 ms (-9 %), render and GPU
  unchanged. `actors-battle` 5 rounds, medians: 32.5 -> 34.5 fps (+6 %), game thread 26.96 -> 25.07 ms (-7 %), within
  that scene's run-to-run spread (fights differ; two runs on each side were cut short by the game's own "Havok picks
  used this frame" dialog, once with the fix off and once on). For scale on this PC, the vanilla game -> the 1.1.0 fixes
  gave the crowd 36.8 -> 44.4 fps. `NpcPerceptionParallel=1` is on by default.

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
