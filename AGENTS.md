# AGENTS.md

Orientation for coding agents (Claude Code, Codex, Cursor, ...) and for people. It is short on purpose: read it first,
then open only the doc or source file you need. Plain-language overview: [README.md](README.md). How to contribute:
[CONTRIBUTING.md](CONTRIBUTING.md). Repository: https://github.com/LTSharpe/OblivionRemasteredPerformanceFix.

## What this repo is
- **OblivionPerfFix**: one small DLL for *Oblivion Remastered* (Unreal Engine 5.3, Steam build 19115871). It loads as
  `version.dll` next to the game exe (a proxy that forwards to `System32\version.dll`) or as an OBSE64 plugin, finds
  the code each fix needs by byte pattern, checks it, and changes it in memory. Every fix does the same work more
  cheaply: **same image, same gameplay**. Two exceptions are timing only, in the game's own order: `NpcCreationPacing`
  builds far-away, unseen characters a few frames later; `CellBufferPacedEvict` unloads the cells behind the player a few
  frames later. Two are equivalent rather than identical, in a way the game itself already varies: `RigTraceReuse` gives
  a repeated Control Rig ground trace the answer of the identical trace made microseconds earlier in the same rig run
  (it can only differ by a thread race the game does not order), and `ChaosMaxPayloadSize` lets collision queries find
  the same objects through the search tree in another visit order (only exact distance ties can resolve differently).
  A fix whose code does not match switches itself off and logs why.
- **A benchmark suite** (PowerShell, C# compiled at run time, a UE4SS Lua mod) that starts the game hidden and muted,
  runs fixed scenes unattended and compares runs. Only needed to measure, not to use the fix. The **benchmark kit** for
  players (`kit/`, released as `OblivionPerfFix-<version>-Benchmark.zip`) runs the Imperial City flight stock vs fix on
  screen and with sound by default (`Benchmark-Hidden.cmd`: hidden and muted).

## Layout
| Path | What |
|---|---|
| `src/OblivionPerfFix/src/core/` | `main.cpp` (entry points: proxy `DllMain`, OBSE64 `OBSEPlugin_Load`; installs the fixes in order; the monitor thread that re-reads the ini every second), `common.h` (`OPF_VERSION`, `Config` with every default, the pattern API, every fix's entry points), `config.cpp` (ini reading), `log.cpp`, `mem.cpp` (pattern search, code and pointer writes), `proxy.cpp` + `proxy_stubs.asm` (`version.dll` forwarding) |
| `src/OblivionPerfFix/src/<area>/` | one file per fix, grouped: `render`, `streaming`, `physics`, `npcs`, `loading`, `menus`, `saves`, `framegen`, `guards`, `memory`, `diagnostics` (index below). The comment at the top of each file explains the problem, the patch and why the output is identical |
| `src/OblivionPerfFix/tests/` | offline self-test: maps the game exe, applies every fix to that copy, checks the result |
| `src/OblivionPerfFix/OblivionPerfFix.ini` | the shipped ini (every switch, explained in plain words) |
| `tools/` | benchmark scripts, `find_patterns.py`, `GamePaths.ps1`, `Setup-Bench.ps1`, `Bench-Report.ps1`, `Audit-UESource.py`, `stutter_ledger.py` |
| `bench/`, `mods/ue4ss/OblvBench/` | benchmark profiles and scenes; the in-game mod that drives them |
| `kit/` | the benchmark kit's double-click launchers (`Benchmark.cmd`, `Benchmark-Hidden.cmd`, `Check-GPU.cmd`), its `README.txt` and `files.txt` (what of `tools/`, `bench/`, `mods/` goes into the kit zip); entry point `tools/Bench-Kit.ps1` |
| `docs/` | `how-it-works.md` (each fix in plain words, same groups as the source), `research.md` (evidence, rejected ideas), `porting.md` (game update), `benchmark.md` |
| `.github/` | CI build + draft release with attestation, issue forms |

## Build, test, install
Needs Windows, Visual Studio 2022 or later with the C++ x64 tools (the free *Build Tools* are enough; `build.ps1` finds
any edition with vswhere, or set `$env:OPF_VCVARS` to a `vcvars64.bat`), PowerShell 5.1+, and Python 3 (standard
library only) for `tools/*.py`.

```powershell
src\OblivionPerfFix\build.ps1              # -> src\OblivionPerfFix\build\OblivionPerfFix.dll (+ .pdb)
src\OblivionPerfFix\build.ps1 -SelfTest    # also builds and runs the self-test against the installed Steam exe
src\OblivionPerfFix\build.ps1 -SelfTest -Exe <path\to\exe> -OutDir <scratch>   # another exe (e.g. a new game build)
python tools\find_patterns.py              # every byte pattern in src/: matches in the installed exe vs the tested build
tools\GamePaths.ps1                        # where the game, its exe and its config/saves are on this PC
```

- Self-test output: one `PASS`/`FAIL` line per check, then the fixes' own log lines; exit code 1 on any failure. Checks
  named `... at exe+0x...` pin build 19115871 and fail on any other build by design; the "found / applied / hooked"
  checks are what matter. A fix that is not found gets one `FAIL <fix>` line ("not installed") and its remaining
  checks are skipped.
- Install a build: game closed, copy `build\OblivionPerfFix.dll` to `<game>\OblivionRemastered\Binaries\Win64\version.dll`
  and the ini next to it (or `tools\Install-PerfFix.ps1`). Uninstall = delete both. The DLL writes
  `OblivionPerfFix.log` next to itself: the version, the exe's TimeDateStamp, then the fixes' lines, each tagged.

## How each fix finds its code
1. Search a byte pattern (hex with `??` wildcards, written in the fix file with the disassembly it expects) in the exe's
   `.text`; most must match exactly once.
2. Decode what the match points to (`RipTarget` for RIP-relative calls and data), check offsets, call targets, vtable
   slots and struct layouts against what the fix relies on.
3. Only then patch: a few code bytes, a vtable or import slot, a console variable's value, or a call redirected to a
   stub in free memory within ±2 GB of the code. Anything unexpected: log `[tag] ...: fix disabled` / `not installed` /
   `not found` and leave the game's code alone. Most fixes re-read the ini every second and can be switched live.

## Fix index
The ini key is a fix's stable name (docs and source comments use it). Evidence per fix in `docs/research.md`, plain
words in `docs/how-it-works.md`, both in this order. All on by default except the diagnostics.

| Area | ini key | log tag | source | what it changes |
|---|---|---|---|---|
| render | `BoneUploadStores` | `[bone]` | `render/bone_upload.cpp` | masked stores -> plain stores in the bone-matrix upload loops (AVX2, AVX, AVX-512 variants; same bytes written) |
| render | `AtmosphereDirtyFilter` | `[atm]` | `render/atmosphere.cpp` | the weather system's property writes mark render state dirty only when the value changed |
| render | `AtmosphereLightFastPath`, `AtmosphereLightCloudPath` | `[atm]` | `render/atmosphere.cpp` | brightness/colour-only light changes use the engine's light update; the sun's/moon's cloud values go into the live light proxy instead of a re-add; lit lights sharing an atmosphere index are rebuilt as stock (the sky's light choice) |
| render | `RdgParallelTranslate` | `[rdg]` | `render/parallel_translate.cpp`, `translate_defrag_guard.cpp`, `translate_frame_fence.cpp` | all render-graph pass sets translated on worker threads (UE 5.5's default); `2` = automatic (off under 9.5 GB of VRAM or under 8 logical processors); descriptor-heap guard; buffer-pool defragmentation paused while on unless `ViewRenameGuard` is on, then a per-frame fence orders each frame's start before its translates |
| render | `RdgSubmitBatch` | `[rdg]` | `render/submit_batch.cpp` | groups parallel translate's command-list submissions, same order |
| render | `GpuSceneParallelUpload` | `[gpuscene-par]` | `render/gpuscene_upload.cpp` | `r.GPUScene.ParallelUpdate 1`: the GPU scene upload's batches on all workers (each writes its own range) |
| render | `ClothConfigCache` | `[cloth]` | `render/cloth_config.cpp` | skips the per-frame cloth config copy when it would come out identical (deep compare, else copy) |
| render | `WaterCaptureFsrSkip`, `WaterCaptureNoRhiFlush`, `WaterCaptureNoOcclusionWait`, `WaterCaptureFastMeshScan` | `[water]` | `render/water_info.cpp` | inside the Water plugin's water-info render (`UpdateWaterInfoRendering_RenderThread` detoured as a scope): FSR3's pre-alpha copy, the first-renderer RHI flush and the occlusion-fence wait skipped (never read there); `FEnableWaterInfoMeshesScoped` finds the same proxies from `FScene`'s packed component ids. Start-up only (version.dll route), then live |
| render | `RayTracingBuildBatch` | `[rtbatch]` | `render/rt_build_batch.cpp` | InitRHI's Normal BLAS build requests (render thread, outside a scene render, build budget <= 0, not fast-build or allow-update) take the geometry manager's queue path instead of building at once; `FDeferredShadingSceneRenderer::Render`'s vtable slot builds the queue as one batch first (same builds, same frame); a request that would take the queue past 64 MiB of scratch (a loading screen, no render) builds it first, in the gate. Start-up only (version.dll route), then live |
| render | `HdrGamma22` (on, trade-off; HDR only) | `[hdrgamma]` | `render/hdr_gamma.cpp` | the HDR tone curve's block (20 floats at exe+0x8DD9340, read by every pass through one getter) refitted on the monitor thread so the curve's output below W = 9.5 x Mid-Tone is re-decoded as gamma 2.2 instead of sRGB (spline coefficients and split points; floor, peak, mid join kept); written from NpcCreationPacing's frame hook, the game's values put back when off; re-done on each HDR setting change. Changes the picture on purpose. Live |
| streaming | `LumenKeepArrayCapacity`, `UploadMemoryKeep`, `GpuSceneKeepCapacity` | `[lumen]` `[upload]` `[gpuscene]` | `streaming/stream_churn.cpp` | Lumen's scene arrays, the D3D12 upload pools and the GPU scene buffers keep their capacity instead of shrink/regrow copies and kernel allocations |
| streaming | `RtSceneBufferKeep` | `[rtscene]` | `streaming/rt_scene_keep.cpp` | `FRayTracingScene::Create` keeps a TLAS buffer that is big enough instead of re-creating it smaller when the scene needs less than half (grows as before; at most the session's largest size); its three buffer creations timed for the stats line |
| streaming | `NaniteInitialRootPages` | `[nanite]` | `streaming/nanite_stream_buffers.cpp` | `r.Nanite.Streaming.NumInitialRootPages` 2048 -> 3072 at start-up (both routes; only while it holds 2048; an Engine.ini line wins): Nanite's ClusterPageData starts at its first growth step (+32 MiB) instead of being re-created mid-flight |
| streaming | `InstanceClearReserve` | `[instances]` | `streaming/instance_clear.cpp` | clearing a big instanced mesh reserves its hide-command list once |
| streaming | `NavmeshBuildNoBusyWait`, `BusyWaitFlagCheck` | `[navmesh]` `[busywait]` | `streaming/navmesh_wait.cpp`, `busywait_check.cpp` | navmesh tile builds never run inside another task's busy wait (the engine's flag, and the gap where a put-back task was taken unchecked) |
| streaming | `NavLinkFast` | `[navlink]` | `streaming/nav_link_fast.cpp` | a streamed nav chunk's `dtNavMesh::addTile`: `connectExtLinks` calls that cannot make a link left out, `findConnectingPolys` run once per polygon with an edge on the facing side (per-tile cache per chunk attach); the game's code makes every link, same order. Start-up only (version.dll route), then live |
| physics | `PhysicsTreeTimeSlicing` | `[chaos]` | `streaming/stream_churn.cpp`, `physics/physics_flush.cpp` | big physics acceleration-structure updates time-sliced like small ones, with bigger slices while a rebuild has a backlog |
| physics | `PhysicsFlushParallel` | `[chaos]` | `physics/physics_flush.cpp` | a rebuild's catch-up fills the game-thread copy on a helper while the game's function does the physics copy |
| physics | `ParallelStaticBodies`, `PipelinedBodyCreation` | `[bodies]` | `physics/static_bodies.cpp` | an instanced mesh's static bodies created on helper threads with the game's code, filed in the game's order, handed over in groups as they are made |
| physics | `DirtyTreeQuickInsert`, `BulkIndexInsert`, `PhysicsInternalFlushParallel`, `PhysicsQueueShrinkOnce` | `[bodies]` `[bulk]` | `physics/bulk_insert.cpp` | the dirty-element tree: quick sibling choice, a batch filed as one subtree, two catch-ups side by side, the pending queue shrunk once (same entries; tree shape may differ, as it already does between runs) |
| physics | `PhysicsPushParallel` | `[push]` | `physics/physics_push.cpp` | a push of new bodies created as one batch, shapes set up on helper threads, same order |
| physics | `PhysicsDestroyBatch` | `[destroybatch]` | `physics/destroy_batch.cpp` | the step's `DestroyPendingProxies` of 256+ removed bodies: proxy deletes on one helper (all joined before the function returns), `UpdateViews` once after the loop instead of per body; same physics state |
| physics | `PhysAnimNoEmptyLock` | `[physanim]` | `physics/physanim_lock.cpp` | `UPhysicalAnimationComponent::UpdateTargetActors` returns at once when the component drives no body (DriveData empty) and its mesh has no pending parallel animation task: no scene write lock, no bone copy, no empty loop (identical: no state changes). Start-up only (version.dll route), then live |
| physics | `ChaosMaxPayloadSize` (10000000) | `[chaosmps]` | `physics/chaos_payload.cpp` | `p.MaxPayloadSize` 100000 -> 10000000 cm at start-up (both routes; only while it holds 100000; an Engine.ini line wins): the ~730 bodies over 1 km stay in the Chaos acceleration trees instead of the `GlobalPayloads` list every scene query walks first (equivalent: same overlap sets and closest hits, another visit order) |
| npcs | `NpcFadeInNoRebuild` | `[fadein]` | `render/npc_fade_in.cpp` | a spawning character's dither fade-in value goes to the proxy and `FScene::UpdateCustomPrimitiveData` instead of recreating every primitive's render state (and skinned BLAS) every frame |
| npcs | `NpcCreationPacing` | `[pacing]` | `npcs/npc_pacing.cpp` | far (> 4096 units), unseen actors' creation work under a per-frame budget; "unseen" = not rendered within 0.5 s, or (since 1.2.3, hidden key `PacingFadeAware`) rendered but still invisible in the spawn fade-in (dither >= 3, read from the fade component; a waiting head is re-checked each frame and runs once it is seen and visible); the old-engine -> UE command queue stays FIFO, anything 30 frames old runs at once, off during loading and menus; a level flow at its wait bound drains the main queue only (`PacingLeanFlowDrain`). **Timing changes** (a few frames) |
| npcs | `RigTraceReuse` | `[rigtrace]` | `npcs/rig_trace_reuse.cpp` | inside one `UControlRig::Execute` (per-thread generation), a `FRigUnit_SphereTraceByTraceChannel` call whose inputs (context, owner component, actor, world, start, end, radius, channel, rig-to-world transform) are bit-identical to an earlier one of the same run gets that call's outputs (equivalent: the horses' rigs re-run the pure node per consumer; 2 = verify). Start-up only (version.dll route), then live |
| npcs | `ClothRegisterLite` | `[clothreg]` | `npcs/cloth_register.cpp` | every cloth build inside one `EquipBodyPart` call made without `FClothConstraints::AddRules` (the builds the robe's Blueprint throws away); the one left at the call's end gets the recorded rules added in place (same inputs, same order: the game's constraints), else it is rebuilt in full |
| npcs | `RigVmHashMemo`, `RigLogicMapCache` | `[rigsetup]` | `npcs/rig_setup.cpp` | `URigVM::ComputeVMHash` memoised on all its inputs (compared in full); the head rig's DNA name -> hierarchy index maps replayed from the FNames the stock code made for the same DNA content, every lookup done live on the new hierarchy |
| npcs | `NpcBodiesOneLock` | `[npcbodies]` | `npcs/npc_bodies.cpp` | `InitArticulated`'s `InstantiatePhysicsAsset_Internal` (a character's bodies and constraints) under one hold of the scene's write lock instead of one per body; call tree pinned by hash; same bodies, same order |
| npcs | `AnimTeardownParallel` | `[animpar]` | `npcs/anim_teardown.cpp` | at a cell-row crossing, dying NPCs' recognised animation interpolators destroyed on helper threads, joined before the NPC's clean-up goes on |
| npcs | `CellBufferPacedEvict` | `[cellbuf]` | `npcs/cell_buffer.cpp` | the exterior cell buffer gets 2 x uGridsToLoad - 1 spare slots; overflow cells unloaded one per old-engine frame with the game's unload; the ini writer keeps the player's value. **Timing changes** (a few frames) |
| loading | `PrepareCollisionBehindLoadingScreen` | `[collision]` `[fade]` | `loading/collision_prepare.cpp`, `fade_hook.cpp` | a new area's collision handed to physics with a zero-time step while the loading screen is still up |
| loading | `PsoPrecompileFast` | `[pso]` | `loading/pso_precompile.cpp` | the start-up PSO precompile faster outside play (menus: a bounded render-thread budget; loading screens: UE's fast pace), the game's pace in play and once UE's queue or the driver's compile threads show a cold driver cache |
| loading | `PsoCompileThreadPool` (on, -1) | `[psopool]` | `loading/pso_pool.cpp` | `r.pso.PrecompileThreadPoolSize` at start-up only: precompile and precache PSOs on UE's own pool, not the task workers (game bug: cold-cache loading starved; applied only with `TaskWaitDeadlockGuard=1` installed; the game's or an Engine.ini value wins) |
| menus | `DisplayModeCache` | `[display]` | `menus/display_modes.cpp` | the Graphics page's display-mode lists from a cache re-checked against Windows each time |
| menus | `HdrMenuRows`, `HudAspectRow`, `DlssRayReconstruction` (off) | `[hdrmenu]` `[hudaspect]` `[rrrow]` | `settings/hdr_menu.cpp` | rows on the PC Graphics page, made in memory from the game's own rows (a new `VModernSettingTable` from SpawnObject, rows deep-copied by reflection): an HDR section (HDR, Peak Brightness, Paper White, Black Floor, Mid-Tone; saved HDR options read from Save_Settings.sav early and put back with the game's AddOrUpdateSettingsToSave), a HUD Aspect Ratio row (OPF.HudAspect: the HUD's overlay slots padded to a centred box), a DLSS - Ray Reconstruction row (r.NGX.DLSS.DenoiserMode; needs NVIDIA's nvngx_dlssd.dll; trade-off). Start-up only |
| menus | `MenuScreenshotFilter`, `ScreenshotReadbackConvert`, `ScreenshotParallel` | `[resize]` `[readback]` `[shotpar]` | `menus/image_resize.cpp`, `readback_convert.cpp`, `screenshot_parallel.cpp` | the menu screenshot's resize and colour conversion in SSE2 (identical bytes), its big copies on helper threads |
| menus | `MenuOpenFlashFix` | `[menuflash]` | `menus/menu_open_flash.cpp` | `r.AllowOcclusionQueries` 0 for the player menu's open frame only (set in `VBackgroundWidgetBase::RequestCapture`'s exec thunk, back to 1 in `UGameViewportClient::ProcessScreenShots` on the same game-thread frame; the variable's interface checked on the live object first), so the menu's background box is not occlusion-culled on the next frame (game bug, issue #5). Start-up only (version.dll route), then live |
| saves | `SaveBytesBulk`, `SaveThumbnailAsync`, `SaveRefIdIndex` | `[savestall]` `[savethumb]` `[saverefid]` | `saves/save_bytes.cpp`, `save_thumb.cpp`, `save_refid.cpp` | a save: the save list's byte arrays written in one `Serialize` inside `SaveGameToMemory` (same bytes; call sites found by an E8 scan, each hit checked: inside a .pdata function, `test al,al` after); the thumbnail's read-back as an async GPU copy delivered by a helper thread (save system's cache first, then the save's callback; same pixels); the old engine's formID -> refID lookups through a hash index (same refIDs). Each installed at start-up only when its key is on, then live |
| menus, saves | `HdrMenuBackground`, `HdrSaveThumbnail` (off, trade-off) | `[hdrbg]` `[hdrthumb]` | `menus/menu_hdr_background.cpp` | in HDR10 (r.HDR.EnableHDROutput, OutputDevice 3/4) the menu screenshot's ImageResize result (exe+0x4924F69) and the save screenshot callback's copy (exe+0x48AF8DA) mapped from sRGB-encoded PQ to sRGB at the same nits relative to r.HDR.UI.Luminance (soft shoulder); SaveThumbnailAsync looks through the copy's stub. Changes the picture on purpose. Start-up install, then live |
| framegen | `FrameGenAsyncCompute` | `[fgasync]` | `framegen/framegen_async.cpp` | `r.FidelityFX.FI.AllowAsyncWorkloads 1` at start-up (an Engine.ini line still wins) |
| framegen | `FrameGenPacingTuning` | `[fgpace]` `[fgorder]` | `framegen/framegen_pacing.cpp` | AMD's frame-pacing tuning set B on AMD's FSR 3.1.4 / SDK 2.x DLL swap chains (Adrenalin's driver swap chain, provider id bit 31, keeps its own pacing; 3 = Set B everywhere; nothing on the game's 3.1.0); logs each FSR context's provider |
| framegen | `FrameGenCameraInfo` | `[fgcam]` | `framegen/framegen_camera.cpp` | links AMD's camera desc (0x2000A) to each frame-generation prepare call, as AMD's newer UE plugin does; read only by ML frame generation |
| guards | `NameLookupCrashGuard` | `[edid]` | `guards/name_lookup_guard.cpp` | name-table lookups skip entries whose string memory was freed (game bug) |
| guards | `TrapCrashGuard` | `[trap]` | `guards/trap_guard.cpp` | traps drop actor entries that no longer exist, with the game's own removal (game bug) |
| guards | `HavokPickWarningGuard` | `[picks]` | `guards/havok_pick_warning.cpp` | the old engine's "Havok picks" warning box (a modal message box on its thread that stops the game) is logged instead of shown; the pick cap stays (game bug) |
| guards | `ObjectReplaceCrashGuard` | `[replace]` | `guards/object_replace_guard.cpp` | `StaticAllocateObject`'s in-place re-creation: a `GUObjectArray` slot the destructor left holding the destroyed object is freed as `FreeUObjectIndex` does, instead of the `UObjectArray.cpp:234` fatal error (game bug) |
| guards | `ViewRenameGuard` | `[view]` | `render/view_rename_guard.cpp` | UE-217692: view creation and pool defragmentation serialised; a stale view repaired on the spot |
| guards | `FrameGenCrashGuard` | `[fg]` | `framegen/framegen_guard.cpp` | AMD FidelityFX 3.1.0/3.1.4: the 8-slot command-list pool waits for a free slot instead of reading a null one |
| guards | `RestoreFsrAfterAltTab` | `[focus]` | `menus/focus_restore.cpp` | FSR back on after a focus loss once no menu holds the world (game bug) |
| guards | `FrozenClothStretchFix` | `[frozencloth]` | `render/frozen_cloth_lod.cpp` | a suspended cloth's zero-time LOD update requests the engine's per-cloth teleport, so the frozen pose moves fully with its owner (game bug: stretched robes) |
| guards | `FrameRateLimitAtStart` | `[fpscap]` | `framegen/frame_cap_start.cpp` | after frame generation's first off -> on switch (the start-up apply, no menu open), the saved `Altar.GraphicsOptions.FrameRateLimit` run through Altar's setter as the Graphics page does (game bug: the saved cap is 0 after a launch with FG on). Since 1.2.2 only when that switch came from FSR's sink, the GPU is not NVIDIA's (GRHIVendorId through the vendor check T calls for its Reflex flag), T's Reflex flag is 0 and the limit is still 0: only FSR FG on AMD/Intel GPUs (DLSS FG / NVIDIA left to the game). Start-up only, both routes |
| guards | `TaskWaitDeadlockGuard` (on) | `[taskwait]` | `guards/task_wait_guard.cpp` | a task worker waiting for tasks (UE 5.3 reserve-worker wait) is also woken by its reserve worker, not only by the trigger task that may never get a thread (game bug: cold-cache deadlock, RenderThread 120 s timeout) |
| memory | `OsPageCache` | `[oscache]` | `memory/os_page_cache.cpp` | blocks the allocator releases to Windows kept (64 MB, exact size, 5 s) and reused |
| memory | `MemoryRelief`, `[General] GpuCrashReport` | `[mem]` `[gpucrash]` | `memory/memory_monitor.cpp` | RAM/commit under 1 GB: the memory-keeping fixes behave as stock; DRED + memory report in the log on a GPU crash |
| memory | `CommandAllocatorTrim` | `[alloc]` | `memory/allocator_trim.cpp` | the D3D12 copy queue's command-allocator pool shrinks to recent need when within 2 GB of the VRAM budget |
| diagnostics | `HitchLedger` (off) | `[ledger]` | `diagnostics/hitch_ledger.cpp` | per slow game-thread frame, where its time went, to `OblivionPerfFix-ledger.csv` (`tools/stutter_ledger.py` ranks it) |
| diagnostics | `HitchLedgerTickSplit` (off; needs `HitchLedger=1`) | `[ticksplit]` | `diagnostics/tick_split.cpp` | the tick task's `ExecuteTick` call, the tickables' `Tick` call, the timer manager's three delegate calls and UWorld::Tick's direct calls redirected to timing stubs: game-thread time per (tick kind, ticked class, group) in 2 s windows plus each slow frame's top 3, to `OblivionPerfFix-ticksplit.csv` (`tools/tick_split.py` ranks it). Start-up only (version.dll route), then live |

`[General] StatsIntervalSeconds` writes per-fix counters to the log (default every 60 s). The log keeps the last two
sessions as `OblivionPerfFix.1.log` / `.2.log` and stops at 8 MB per session (crash report excepted). `[General] LogFile=0`
(start-up only, default 1; `core/log.cpp`, `LogOpen(..., enabled)`) writes no log: nothing is created, renamed or deleted at
start-up, and the first `[gpucrash]` line creates the log (rotating the old ones as usual) so a GPU crash still leaves
its report. Lab builds ignore the key.

## Adapting it
**A game update broke a fix** (the log says `not found` / `disabled` for a tag): follow [docs/porting.md](docs/porting.md).
Short version: `find_patterns.py` shows which patterns changed; open that fix file, read the expected disassembly in its
comments, find the same code in the new exe, update the pattern and checks, run the self-test, then a benchmark A/B
and an image comparison. Run `python tools\find_patterns.py --save tools\pattern-baseline.tsv` on the build you tested.

**Another PC:** the fix has no paths and no per-PC settings; helper-thread counts follow the logical processors the
game may use, and the helper-heavy fixes stay off below 8 of them (hidden key `ParallelMinProcessors`). For the benchmark: `tools\Setup-Bench.ps1` (the
official UE4SS experimental build >= 1181 and PresentMon, downloaded from their own projects, SHA-256 recorded; UE4SS is in the game folder only while a benchmark game runs), game and
Documents paths from `tools\GamePaths.ps1` (override with `$env:OBLIVION_GAME_DIR` / `$env:OBLIVION_SAVED_DIR`),
display/quality profiles in `bench/profiles.json` (`small` is the quiet default; `native` uses the monitor's
resolution). `tools\Bench-Report.ps1` = stock vs fix, shareable report.

**Xbox / PC Game Pass:** the DLL accepts `OblivionRemastered-WinGDK-Shipping.exe` (experimental, untested); it is a
separate compile, so expect some patterns to differ. The benchmark is Steam-only.

## Rules
- **Identical output.** A fix may only skip or reorder work when the result is bit-identical ("skip only when nothing
  changed"), or when the UE 5.3 source shows it is equivalent. No setting that changes the image or the gameplay; the
  world must never run while the player can't see or act longer than in the stock game. Timing changes like
  `NpcCreationPacing`'s or `CellBufferPacedEvict`'s need the same bounds (a few frames, the game's order, off-screen
  only) and an off switch.
- **Fail safe.** Pattern search plus checks before every write; a clean off with one log line when anything differs; a
  new ini key gets a safe default; the self-test must pass. Bump `OPF_VERSION` (`core/common.h`) and the OBSE64
  `pluginVersion` (`core/main.cpp`), and add a `CHANGELOG.md` entry.
- **Prove it.** Image comparison (`tools\Compare-Benchmark.ps1`, visual pass) for anything that could touch the picture;
  benchmark A-B-A for speed (run-to-run noise is about ±10%; trust per-thread ms more than fps).
- **Iterate on a mini-scenario.** From a report to a fix: reproduce it as a save-free scenario, find the problem spots,
  cut a 30-60 s scene per spot (`py -3 tools\slice_flight.py` slices timed flights; start ~9 s before the spot so its
  cells and NPCs load during the measured part), check in one or two runs that it reproduces the problem, iterate there
  (one game kept open, ini switches flipped live, ABBA), and confirm once on the full scenario.
- **Identical runs.** Runs are pinned by default (weather forced and held, hour, `timescale` 1; a scene opts out with
  `"keep"`, `-NoPin` = old behaviour) and the game starts with Unreal's `-FixedSeed` (this launch only; `-NoFixedSeed`);
  start from a fresh world, compare only runs made the same way, certify a new mini-scenario with
  `tools\Repro-Check.ps1 -N 3`, and say what still varies (NPC behaviour, GPU state).
- **Licences.** No game files, and no game bytes beyond short search patterns. No Unreal Engine source text: names,
  offsets and descriptions in your own words are fine, copied engine code or comments are not
  (`python tools\Audit-UESource.py --ue <your UE checkout> src docs` checks for copied runs of text).
- **Benchmark etiquette.** Runs keep the game window hidden and muted. Never minimise the game (it crashes); start the
  game for measurements only through `tools\Run-Benchmark.ps1`, which also restores your settings and saves afterwards.
  Stop a run or a chain with `tools\Run-Benchmark.ps1 -Stop`, never with `Stop-Process` (run chains in-process so
  `-Stop` reaches their later runs).

## Saving tokens
- `tests/selftest.cpp` and the larger fix files are long: grep for the fix's `Check(` lines or its log tag instead of
  reading them whole. Logs: grep the tag. Benchmark runs: read `report.md`, not `result.json`.
- Run `tools\Run-Benchmark.ps1` in the background and poll `tools\Bench-Status.ps1`; its last line is `BENCH-RESULT ...`.
