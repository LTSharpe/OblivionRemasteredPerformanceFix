# The benchmark suite

The benchmark plays fixed scenes in Oblivion Remastered by itself and measures them: frame rate, the slowest frames,
CPU time per engine thread, GPU time, and the stutter around doors, gates and while travelling. Every scene starts from
a **new game without a save**, so anyone gets the same scenes. It is how the numbers in this repository were measured,
and how you can check the fix on your own PC, or just watch two cinematic flights with and without it.
You don't need any of this to use the fix.

**Contents:** [Quick start](#quick-start) · [Reproduce the README numbers](#reproduce-the-numbers-in-the-readme) · [What to run](#what-to-run) · [Watch the flights](#watch-the-flights) ·
[Profiles](#profiles) · [Running scenes yourself](#running-scenes-yourself) · [While it runs](#while-it-runs) ·
[Reading the results](#reading-the-results) · [Troubleshooting](#troubleshooting) · [UE4SS](#ue4ss-only-for-the-benchmark) ·
[Writing scenes](#writing-scenes)

## Quick start
You need Windows 10 or 11, Oblivion Remastered on Steam (started once by hand, so the licence screen and first-start
settings are done), the fix installed ([README](../README.md#install-two-minutes)), the free
[Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/visual-cpp-build-tools/) (*Desktop development
with C++*, for a small window-hook DLL) and about 1 GB of disk space. Download this repository (*Code* > *Download ZIP*,
extract it, or `git clone`) and open PowerShell in its folder:

```powershell
# 0. Each new PowerShell window: allow this folder's scripts (this window only); once after a ZIP download: unblock them.
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
Get-ChildItem -Recurse | Unblock-File

# 1. Once: get the tools (UE4SS and PresentMon, pinned versions, SHA-256 checked) and build the hook.
tools\Setup-Bench.ps1 -Check      # what's there and what's missing; changes nothing
tools\Setup-Bench.ps1

# 2. Stock vs fix on the default Cheydinhal scenes: 4 runs, under an hour, hidden and muted.
tools\Bench-Report.ps1

# 3. Share: paste captures\bench\_report\<time>\report.md into a "Benchmark result" issue and attach the json files.
```

`Bench-Report.ps1` runs the benchmark alternately with every fix switched off ("stock": the DLL loaded with all its
switches at 0, so it leaves the game's code alone) and with the fix as installed, then writes `report.md` and copies of
the runs' `result.json` with your user name, PC name and folder paths removed. Share it in a
[Benchmark result](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/new/choose) issue.

Useful options: `-Rounds 3` (more stock/fix pairs, less noise), `-Profile cpu` (display profile, [below](#profiles)),
`-Scenario showcase` (another scenario, [below](#what-to-run)), `-Scenes canal-noon,inn-door` (some scenes only),
`-Watch` (on screen with sound, at your own settings), `-Base <runs> -Test <runs>` (a report from runs you already made).

## Reproduce the numbers in the README
Each row of the README's results table, as one command (stock vs fix, alternating, a report at the end). Profiles that
use your monitor's resolution (`user`, `promo`, `native*`) give the README's numbers only on a 4K monitor; on another
one they measure the same scene at your resolution.

| README row | Command | What the profile sets |
|---|---|---|
| Imperial City flight, 4K, frame generation, 60 fps cap | `tools\Bench-Report.ps1 -Scenario showcase -Profile user -Rounds 2` | your monitor's resolution, FSR Performance, frame generation, ray tracing, Ultra with Shadows High, 60 fps cap before frame generation |
| Imperial City flight, 4K, no frame generation (the video) | `tools\Bench-Report.ps1 -Scenario showcase -Profile promo -Rounds 2` | the same without frame generation, no cap |
| Cheydinhal flight, 4K, no frame generation | `tools\Bench-Report.ps1 -Scenario cheydinhal-show -Profile promo -Rounds 2` | as above |
| Cheydinhal streets when the CPU is the limit | `tools\Bench-Report.ps1 -Profile cpu -Rounds 2` | 1280x720 window, FSR Performance, no frame generation |
| Your own settings, any scene | `tools\Bench-Report.ps1 -Scenario showcase -Profile own` | nothing: the game as you set it up |

The report's numbers are means over the rounds. Run-to-run noise is about ±5-10 % on fps, more on stutter counts, so
use `-Rounds 3` or more for small differences, and close heavy programs while it runs (we saw a busy PC double the
stutter counts). The profiles change the game's display settings only for the benchmark's own games and put yours back
afterwards.

## What to run
Every scenario is a file in `bench/scenarios/`; `tools\Run-Benchmark.ps1 -List` lists them, and
`-List -Scenario <name>` lists one scenario's scenes. Times are the scenes alone; each game start adds about a minute.

| Scenario | What it is | Scenes | Good for |
|---|---|---|---|
| `cheydinhal` (default) | canal and chapel square at noon, dusk, night and in rain, inn and chapel interiors, three doors and the city gate both ways, a ride and a walk out of town | 15, ~10 min | the CPU-limited town, door hitches |
| **`showcase`** | **the Imperial City flight**: up the Niben under the stone bridge, once round the Imperial City island over Lake Rumare at golden hour, a climb for a panorama; 111 s, timed to the main theme | 1, ~2 min | watching, before/after at your own settings, streaming at speed |
| **`cheydinhal-show`** | **the Cheydinhal flight**: over the treetops to the west gate, through the town (main street, both covered bridges, the isle, the chapel square) and into the chapel; 115 s in three scenes, timed to the town music | 3, ~2 min | watching; the CPU-limited town in motion, where the fix shows most |
| `world` | flights over the Niben and near Leyawiin, a dungeon exit, Kvatch's Oblivion plane, the Shivering Isles | 13, ~9 min | open world, GPU-heavy places |
| `actors` | the Imperial City market, a crowd of 40 soldiers, a battle | 5, ~3 min | many characters |
| `silver-road` | the Silver Road north from Roxey Inn to Bruma at running speed (`silver-road-upper`: the climb only) | 1, ~5 min | stutter while travelling (the streaming, physics and navigation-mesh fixes) |
| `roxey-night` | Roxey Inn at night, then the road west to Sercen (`roxey-ft`: a forest first, then a jump there, like a fast travel) | 3, ~3 min | stutter after fast travel (`UploadMemoryKeep`, `GpuSceneKeepCapacity`) |
| `blackwood` | the Blackwood forest at dusk: a low flight north towards the Wayshrine of Stendarr as Sheogorath's shrine and its worshippers load, and back south (`blackwood-onfoot`: the way north on foot) | 2, ~3 min | where the first travel-stutter fixes were found: streaming stutter and an NPC burst |
| `npcburst` | a flight past Peryite's shrine as its five worshippers are created | 1, ~1 min | NPC creation hitches (`NpcCreationPacing`) |
| `npc` | a field south of Bruma, empty, then with 10, 20 and 40 soldiers | 4, ~2 min | the cost of each character on screen |
| `leyawiin-forest` | the forest by Fort Doublecross near Leyawiin in rain, fog and clear weather, then a low flight to Niben Bay | 8, ~5 min | GPU-heavy foliage, water and weather |
| `gpuscene-vis` | instance-heavy views (forest, roadside) for image comparisons (`-Pass visual`) | 3, ~2 min | checking that `GpuSceneKeepCapacity` leaves the picture unchanged |
| `transitions` | every door and gate in `cheydinhal` and `world`, plus open-world doors | 45, ~40 min | door and loading behaviour in depth |
| `sun` | the canal across the day and in every weather (image pass) | 13, ~6 min | image comparisons of the weather fixes |
| `full` | `cheydinhal` + `world` + `actors` | 33, ~23 min | a broad check |

```powershell
tools\Bench-Report.ps1 -Scenario cheydinhal-show -Profile native     # the Cheydinhal flight, stock vs fix, at your monitor's resolution
tools\Bench-Report.ps1 -Scenario silver-road -Rounds 3               # travel stutter, small window
```

## Watch the flights
```powershell
tools\Run-Benchmark.ps1 -Label look -Scenario showcase -Watch          # the Imperial City flight
tools\Run-Benchmark.ps1 -Label look -Scenario cheydinhal-show -Watch   # the Cheydinhal flight
tools\Run-Benchmark.ps1 -Close                                         # afterwards: close the game, restore everything
```
`-Watch` shows the game on screen with sound, at your own display and graphics settings (profile `own`, nothing is
changed), and still measures everything. The flights fade in from black, fly a fixed path on a fixed timetable (the
same picture in every run, so stock and fix runs can be compared side by side), and fade out. With ffmpeg's `ffplay` on
your PATH, the music each flight was timed to plays along (the game's own track, read from your game folder; the
game's music is muted meanwhile). Hands off the keyboard and mouse while it plays. To see the difference the fix makes,
run it once with the fix and once with `-FixIni` switches at 0 (or use `Bench-Report.ps1 -Scenario showcase -Watch`).

## Profiles
`-Profile <name>`, defined in `bench/profiles.json` (`-List` shows them all).

| Profile | Settings | Use |
|---|---|---|
| `small` (default) | 854x480 window, no upscaler, ray tracing on, no frame generation | CPU-limited and quiet: little GPU load or fan noise while you work |
| `cpu` | 1280x720 window, FSR Performance, ray tracing on | CPU-limited, closer to real play |
| `native` | your monitor's resolution, borderless, FSR Performance, ray tracing on, frame generation on | what most players see |
| `native-shadowhigh` | `native` with Shadows at High | the README's 4K numbers (on a 4K monitor) |
| `native-nofg` | `native` without frame generation | every frame rendered |
| `gpu`, `gpu-max` | monitor resolution, FSR Quality / no upscaling, no frame generation | GPU-limited |
| `own` | your settings as they are (used by `-Watch`) | your real setup; say which settings you used when you share |

All profiles except `own` set every quality option to its highest level and hardware ray tracing to its highest
lighting mode, and check that before every scene. They run uncapped unless they set a cap.

## Running scenes yourself
```powershell
tools\Run-Benchmark.ps1 -List                                  # scenarios, the scenes of 'full', profiles
tools\Run-Benchmark.ps1 -Label base                            # the Cheydinhal scenes (default)
tools\Run-Benchmark.ps1 -Label t -Scenes inn-door -Pass perf   # one scene
tools\Run-Benchmark.ps1 -Label t -Tags transition -Pass perf   # every scene with a tag
tools\Run-Benchmark.ps1 -Label nofix -FixIni @{ RdgParallelTranslate = 0 }   # one fix off for this run
tools\Compare-Benchmark.ps1 -Base base -Test nofix             # per scene, with images
tools\Run-Benchmark.ps1 -Close                                 # close the game, restore everything
```
`-Pass perf` measures only; `-Pass visual` only captures the frozen reference images; the default does both. Run it in
the background and follow it with `tools\Bench-Status.ps1 -Wait`; the runner's last line is
`BENCH-RESULT ok|partial|failed <label> dir=<run folder>`. After a run the game waits hidden at the main menu, so the
next run starts in seconds; it closes by itself after 20 idle minutes.

## While it runs
- It starts the game through Steam, **off-screen and muted**: no window, no taskbar button, no sound, and it never
  takes the keyboard focus, so you can keep working. Steam stays usable. (Add `-Watch` to see and hear it instead.)
- It starts a **new game with the default character**: your saves are not loaded or changed. The camera is in a
  spectator mode (no HUD, nothing can die, and nobody notices the player: no character walks up to it, talks to it or
  attacks it; characters among themselves behave as usual).
- Unless you watch, it changes the game's display settings for the run and **puts your settings, config files,
  `OblivionPerfFix.ini` and autosaves back** when the game closes.
- If the game crashes or hangs, it keeps the evidence in the run folder, restarts the game and carries on.
- **Don't minimise the game or play it during a run.** The game crashes when it is restored after loading while
  minimised (a game bug), and your input would change the results.
- **Achievements:** the benchmark uses console commands (teleport, time, weather), which the game counts against
  achievements, but only in its own new character; it never loads yours.

## Reading the results
Each run writes `captures\bench\<time>-<label>\`: `report.md` (tables), `result.json` (everything, including the
settings in effect and `OblivionPerfFix.log`), frame logs, screenshots and frame-time plots.
- **fps and 1% low** (the frame rate of the slowest 1% of frames, what you feel as stutter).
- **Game / Render / RHI ms**: CPU time per frame of the three main engine threads; the busiest one limits the frame
  rate when the GPU doesn't. These move less with noise than fps does.
- **Doors:** the worst frame after arriving (the hitch as the picture comes back), the longest gap without a frame, and
  the time until the new place is playable.
- **worst ms, hitches, hitch ms:** the slowest frame, the number of hitches and the time they took; for flights and
  walks this is the stutter while travelling.
- **Noise:** two identical runs differ by up to about 10% (NPCs move, weather changes). Compare alternating runs
  (`Bench-Report.ps1` does), and treat differences under 5% as noise.
- **The first 2-3 minutes after a game start are slower** (the game compiles graphics pipelines in the background);
  the runner waits for that before measuring.
- **Frame generation:** frame times are presented frames, generated ones included.

## Troubleshooting
| You see | Do |
|---|---|
| `NEEDSUSER` | the game shows a screen the benchmark won't click through (licence, first start): start the game once from Steam and get to the main menu |
| a run was stopped half-way, or the game is still running hidden | `tools\Run-Benchmark.ps1 -Close`: closes it and restores settings, saves and UE4SS |
| `BENCH-RESULT partial` | some scenes failed; the report says which and why (often a crash, then the runner restarted the game) |
| `frameCapture: cpu-only` in a result | PresentMon could not follow frames to the display; frame times are CPU-side, GPU busy is missing. The log has a `PresentMon diagnosis:` line |
| your game or Documents folder isn't found | set `$env:OBLIVION_GAME_DIR` (the folder with `OblivionRemastered\`) or `$env:OBLIVION_SAVED_DIR` (`...\My Games\Oblivion Remastered\Saved`); `tools\GamePaths.ps1` shows what it found |
| UE4SS state unclear | `tools\Install-UE4SS.ps1 -Status` |

**Paths and privacy.** The game and its Documents folder are found through Steam's library list (`tools\GamePaths.ps1`;
with Documents on OneDrive, the game still uses `C:\Users\<you>\Documents`). The runner's working files go to `run\`
in the repository (git-ignored; `$env:OBLV_BENCH_RUNTIME` moves it). Run logs name only the game's and Steam's windows,
never other programs' window titles (`$env:OBLV_BENCH_LOG_TITLES = 1` logs all of them, for debugging).

## UE4SS: only for the benchmark
PresentMon (by Intel) records frame times. UE4SS is a widely used Unreal modding tool; the benchmark's Lua mod
(`mods/ue4ss/OblvBench`) runs inside it to drive the scenes. **The fix never uses UE4SS.** The benchmark copies UE4SS and
its mod into the game folder just before it starts its own game, and takes them out again when that game closes. When
you start the game from Steam to play, UE4SS is not loaded. A UE4SS or another `dwmapi.dll` mod you installed yourself is
set aside during the run and put back afterwards.

**Which UE4SS:** `UE4SS_v3.0.1-1161-g6eb3d9bc-oblvfix1.zip`, the upstream experimental build 3.0.1-1161 with a one-line
fix for a memory leak: every Lua call of a game function with an 'Out' parameter kept its table in memory for good
(about 1 GB per hour for a mod that does it every frame; upstream issue
[#1446](https://github.com/UE4SS-RE/RE-UE4SS/issues/1446), fix offered as
[pull request #1452](https://github.com/UE4SS-RE/RE-UE4SS/pull/1452)). Setup downloads it from this repository's
[`bench-tools-v1` release](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/releases/tag/bench-tools-v1) and
checks its SHA-256 (`C605C22AA02601CE5A98C6824FEB4BE88A286CB63A341EE976E024BB29549E84`). How it was built, and how to
rebuild it: [`tools/ue4ss-build/`](../tools/ue4ss-build/README.md). If setup can't download it, download the file
yourself and leave it in your Downloads folder or pass it with `tools\Setup-Bench.ps1 -UE4SSZip <path>`; the hash is
checked either way. Another UE4SS build works with `-AllowOtherUE4SS`, but is untested (and upstream builds up to at
least 1161 have the leak): say so when you share results.

## Writing scenes
A scenario file (`bench/scenarios/<name>.json`) has `places` (a worldspace or interior cell, a position, a heading),
`defaults`, `weathers` (weather form IDs) and `scenes`. Copy a scene from `cheydinhal.json` and change it:

| Key | Meaning |
|---|---|
| `id`, `type`, `tags` | name, `static` / `door` / `fly` / `walk`, tags for `-Tags` |
| `at`, `hour`, `weather` | a place from `places`, the game hour, a weather name from `weathers` |
| `settle`, `measure`, `hold` | seconds to wait before measuring, to measure, and to stay after a door |
| `visual` | `true`: also capture a frozen reference image for image comparisons |
| `enter`, `exit` or `legs` | `door` scenes: the door reference IDs to use (as the console shows them) |
| `world` or `cell`, `path`, `speed` | `fly` scenes: worldspace or interior cell, waypoints `[x, y, z]` (optionally `pitch`, `speed` per point), units per second |
| `flyopt` | `fly` scenes: fades, camera smoothing, hovering at the end (see `FLY_DEFAULTS` in `mods/ue4ss/OblvBench/Scripts/bench.lua`) |
| `waypoints`, `measure` | `walk` scenes: ground waypoints from the place `at`, and a time limit |
| `spawn`, `detect` | `static` scenes: actors to create first (`[{ "id", "count", "distance", "side" }]`); `true` to let them see each other |
| `music` (scenario) | `{ "file", "at": { "<scene id>": seconds } }`: the track `-Watch` plays with each flight |

Positions and door IDs can be read in the game with the console (`getpos x`, clicking a door) or with any Oblivion
record viewer. The two showcase flights were shaped from control points by a route tool that is not in this repository yet; their
waypoints are complete in the scenario files.
