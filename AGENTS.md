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
  frames later. A fix whose code does not match switches itself off and logs why.
- **A benchmark suite** (PowerShell, C# compiled at run time, a UE4SS Lua mod) that starts the game hidden and muted,
  runs fixed scenes unattended and compares runs. Only needed to measure, not to use the fix.

## Layout
| Path | What |
|---|---|
| `src/OblivionPerfFix/src/core/` | `main.cpp` (entry points: proxy `DllMain`, OBSE64 `OBSEPlugin_Load`; installs the fixes in order; the monitor thread that re-reads the ini every second), `common.h` (`OPF_VERSION`, `Config` with every default, the pattern API, every fix's entry points), `config.cpp` (ini reading), `log.cpp`, `mem.cpp` (pattern search, code and pointer writes), `proxy.cpp` + `proxy_stubs.asm` (`version.dll` forwarding) |
| `src/OblivionPerfFix/src/<area>/` | one file per fix, grouped: `render`, `streaming`, `physics`, `npcs`, `loading`, `menus`, `framegen`, `guards`, `memory`, `diagnostics` (index below). The comment at the top of each file explains the problem, the patch and why the output is identical |
| `src/OblivionPerfFix/tests/` | offline self-test: maps the game exe, applies every fix to that copy, checks the result |
| `src/OblivionPerfFix/OblivionPerfFix.ini` | the shipped ini (every switch, explained in plain words) |
| `tools/` | benchmark scripts, `find_patterns.py`, `GamePaths.ps1`, `Setup-Bench.ps1`, `Bench-Report.ps1`, `Audit-UESource.py`, `stutter_ledger.py` |
| `bench/`, `mods/ue4ss/OblvBench/` | benchmark profiles and scenes; the in-game mod that drives them |
| `tools/ue4ss-build/` | how the benchmark's UE4SS build (upstream 3.0.1-1161 + a Lua memory-leak fix) is made |
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
words in `docs/how-it-works.md`, both in this order. All on by default except the diagnostic.

| Area | ini key | log tag | source | what it changes |
|---|---|---|---|---|
| render | `BoneUploadStores` | `[bone]` | `render/bone_upload.cpp` | masked stores -> plain stores in the bone-matrix upload loops (AVX2, AVX, AVX-512 variants; same bytes written) |
| render | `AtmosphereDirtyFilter` | `[atm]` | `render/atmosphere.cpp` | the weather system's property writes mark render state dirty only when the value changed |
| render | `AtmosphereLightFastPath`, `AtmosphereLightCloudPath` | `[atm]` | `render/atmosphere.cpp` | brightness/colour-only light changes use the engine's light update; the sun's/moon's cloud values go into the live light proxy instead of a re-add |
| render | `RdgParallelTranslate` | `[rdg]` | `render/parallel_translate.cpp`, `translate_defrag_guard.cpp`, `translate_frame_fence.cpp` | all render-graph pass sets translated on worker threads (UE 5.5's default); `2` = automatic (off under 9.5 GB of VRAM or under 8 logical processors); descriptor-heap guard; buffer-pool defragmentation paused while on unless `ViewRenameGuard` is on, then a per-frame fence orders each frame's start before its translates |
| render | `RdgSubmitBatch` | `[rdg]` | `render/submit_batch.cpp` | groups parallel translate's command-list submissions, same order |
| render | `GpuSceneParallelUpload` | `[gpuscene-par]` | `render/gpuscene_upload.cpp` | `r.GPUScene.ParallelUpdate 1`: the GPU scene upload's batches on all workers (each writes its own range) |
| render | `ClothConfigCache` | `[cloth]` | `render/cloth_config.cpp` | skips the per-frame cloth config copy when it would come out identical (deep compare, else copy) |
| streaming | `LumenKeepArrayCapacity`, `UploadMemoryKeep`, `GpuSceneKeepCapacity` | `[lumen]` `[upload]` `[gpuscene]` | `streaming/stream_churn.cpp` | Lumen's scene arrays, the D3D12 upload pools and the GPU scene buffers keep their capacity instead of shrink/regrow copies and kernel allocations |
| streaming | `InstanceClearReserve` | `[instances]` | `streaming/instance_clear.cpp` | clearing a big instanced mesh reserves its hide-command list once |
| streaming | `NavmeshBuildNoBusyWait`, `BusyWaitFlagCheck` | `[navmesh]` `[busywait]` | `streaming/navmesh_wait.cpp`, `busywait_check.cpp` | navmesh tile builds never run inside another task's busy wait (the engine's flag, and the gap where a put-back task was taken unchecked) |
| physics | `PhysicsTreeTimeSlicing` | `[chaos]` | `streaming/stream_churn.cpp`, `physics/physics_flush.cpp` | big physics acceleration-structure updates time-sliced like small ones, with bigger slices while a rebuild has a backlog |
| physics | `PhysicsFlushParallel` | `[chaos]` | `physics/physics_flush.cpp` | a rebuild's catch-up fills the game-thread copy on a helper while the game's function does the physics copy |
| physics | `ParallelStaticBodies`, `PipelinedBodyCreation` | `[bodies]` | `physics/static_bodies.cpp` | an instanced mesh's static bodies created on helper threads with the game's code, filed in the game's order, handed over in groups as they are made |
| physics | `DirtyTreeQuickInsert`, `BulkIndexInsert`, `PhysicsInternalFlushParallel`, `PhysicsQueueShrinkOnce` | `[bodies]` `[bulk]` | `physics/bulk_insert.cpp` | the dirty-element tree: quick sibling choice, a batch filed as one subtree, two catch-ups side by side, the pending queue shrunk once (same entries; tree shape may differ, as it already does between runs) |
| physics | `PhysicsPushParallel` | `[push]` | `physics/physics_push.cpp` | a push of new bodies created as one batch, shapes set up on helper threads, same order |
| npcs | `NpcCreationPacing` | `[pacing]` | `npcs/npc_pacing.cpp` | far (> 4096 units), unseen actors' creation work under a per-frame budget; the old-engine -> UE command queue stays FIFO, anything 30 frames old runs at once, off during loading and menus. **Timing changes** (a few frames) |
| npcs | `AnimTeardownParallel` | `[animpar]` | `npcs/anim_teardown.cpp` | at a cell-row crossing, dying NPCs' recognised animation interpolators destroyed on helper threads, joined before the NPC's clean-up goes on |
| npcs | `CellBufferPacedEvict` | `[cellbuf]` | `npcs/cell_buffer.cpp` | the exterior cell buffer gets 2 x uGridsToLoad - 1 spare slots; overflow cells unloaded one per old-engine frame with the game's unload; the ini writer keeps the player's value. **Timing changes** (a few frames) |
| loading | `PrepareCollisionBehindLoadingScreen` | `[collision]` `[fade]` | `loading/collision_prepare.cpp`, `fade_hook.cpp` | a new area's collision handed to physics with a zero-time step while the loading screen is still up |
| loading | `PsoPrecompileFast` | `[pso]` | `loading/pso_precompile.cpp` | the start-up PSO precompile at UE's fast pace outside play, the game's pace in play |
| menus | `DisplayModeCache` | `[display]` | `menus/display_modes.cpp` | the Graphics page's display-mode lists from a cache re-checked against Windows each time |
| menus | `MenuScreenshotFilter`, `ScreenshotReadbackConvert`, `ScreenshotParallel` | `[resize]` `[readback]` `[shotpar]` | `menus/image_resize.cpp`, `readback_convert.cpp`, `screenshot_parallel.cpp` | the menu screenshot's resize and colour conversion in SSE2 (identical bytes), its big copies on helper threads |
| framegen | `FrameGenAsyncCompute` | `[fgasync]` | `framegen/framegen_async.cpp` | `r.FidelityFX.FI.AllowAsyncWorkloads 1` at start-up (an Engine.ini line still wins) |
| framegen | `FrameGenPacingTuning` | `[fgpace]` `[fgorder]` | `framegen/framegen_pacing.cpp` | AMD's frame-pacing tuning set B on AMD's FSR 3.1.4 / SDK 2.x libraries (nothing on the game's 3.1.0); logs each FSR context's provider |
| framegen | `FrameGenCameraInfo` | `[fgcam]` | `framegen/framegen_camera.cpp` | links AMD's camera desc (0x2000A) to each frame-generation prepare call, as AMD's newer UE plugin does; read only by ML frame generation |
| guards | `NameLookupCrashGuard` | `[edid]` | `guards/name_lookup_guard.cpp` | name-table lookups skip entries whose string memory was freed (game bug) |
| guards | `TrapCrashGuard` | `[trap]` | `guards/trap_guard.cpp` | traps drop actor entries that no longer exist, with the game's own removal (game bug) |
| guards | `ViewRenameGuard` | `[view]` | `render/view_rename_guard.cpp` | UE-217692: view creation and pool defragmentation serialised; a stale view repaired on the spot |
| guards | `FrameGenCrashGuard` | `[fg]` | `framegen/framegen_guard.cpp` | AMD FidelityFX 3.1.0/3.1.4: the 8-slot command-list pool waits for a free slot instead of reading a null one |
| guards | `RestoreFsrAfterAltTab` | `[focus]` | `menus/focus_restore.cpp` | FSR back on after a focus loss once no menu holds the world (game bug) |
| memory | `OsPageCache` | `[oscache]` | `memory/os_page_cache.cpp` | blocks the allocator releases to Windows kept (64 MB, exact size, 5 s) and reused |
| memory | `MemoryRelief`, `[General] GpuCrashReport` | `[mem]` `[gpucrash]` | `memory/memory_monitor.cpp` | RAM/commit under 1 GB: the memory-keeping fixes behave as stock; DRED + memory report in the log on a GPU crash |
| memory | `CommandAllocatorTrim` | `[alloc]` | `memory/allocator_trim.cpp` | the D3D12 copy queue's command-allocator pool shrinks to recent need when within 2 GB of the VRAM budget |
| diagnostics | `HitchLedger` (off) | `[ledger]` | `diagnostics/hitch_ledger.cpp` | per slow game-thread frame, where its time went, to `OblivionPerfFix-ledger.csv` (`tools/stutter_ledger.py` ranks it) |

`[General] StatsIntervalSeconds` writes per-fix counters to the log (default every 60 s). The log keeps the last two
sessions as `OblivionPerfFix.1.log` / `.2.log` and stops at 8 MB per session (crash report excepted).

## Adapting it
**A game update broke a fix** (the log says `not found` / `disabled` for a tag): follow [docs/porting.md](docs/porting.md).
Short version: `find_patterns.py` shows which patterns changed; open that fix file, read the expected disassembly in its
comments, find the same code in the new exe, update the pattern and checks, run the self-test, then a benchmark A/B
and an image comparison. Run `python tools\find_patterns.py --save tools\pattern-baseline.tsv` on the build you tested.

**Another PC:** the fix has no paths and no per-PC settings; helper-thread counts follow the logical processors the
game may use, and the helper-heavy fixes stay off below 8 of them (hidden key `ParallelMinProcessors`). For the benchmark: `tools\Setup-Bench.ps1` (pinned
UE4SS and PresentMon, SHA-256 checked; UE4SS is in the game folder only while a benchmark game runs), game and
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
- **Licences.** No game files, and no game bytes beyond short search patterns. No Unreal Engine source text: names,
  offsets and descriptions in your own words are fine, copied engine code or comments are not
  (`python tools\Audit-UESource.py --ue <your UE checkout> src docs` checks for copied runs of text).
- **Benchmark etiquette.** Runs keep the game window hidden and muted. Never minimise the game (it crashes); start the
  game for measurements only through `tools\Run-Benchmark.ps1`, which also restores your settings and saves afterwards.

## Saving tokens
- `tests/selftest.cpp` and the larger fix files are long: grep for the fix's `Check(` lines or its log tag instead of
  reading them whole. Logs: grep the tag. Benchmark runs: read `report.md`, not `result.json`.
- Run `tools\Run-Benchmark.ps1` in the background and poll `tools\Bench-Status.ps1`; its last line is `BENCH-RESULT ...`.
