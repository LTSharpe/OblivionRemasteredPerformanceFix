# The benchmark suite

The benchmark plays fixed scenes in Oblivion Remastered by itself and measures them: frame rate, the slowest frames,
CPU time per engine thread, GPU time, and the stutter around doors, gates and while travelling. Every scene starts from
a **new game without a save**, so anyone gets the same scenes. It is how the numbers in this repository were measured,
and how you can check the fix on your own PC, or just watch two cinematic flights with and without it.
You don't need any of this to use the fix.

**Just want to check the fix on your PC?** Use the benchmark kit: download `OblivionPerfFix-<version>-Benchmark.zip`
from [Releases](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/releases), extract it and double-click
`Benchmark.cmd` (the [README](../README.md#check-it-on-your-pc-before--after) explains it; the game comes up on
screen with your sound, `Benchmark-Hidden.cmd` runs it hidden and muted). No Build Tools, git or Python
needed. This page is about the full suite in the repository and what is behind the kit.

**Contents:** [The kit](#the-benchmark-kit) · [Quick start](#quick-start) · [Reproduce the README numbers](#reproduce-the-numbers-in-the-readme) · [What to run](#what-to-run) · [Watch the flights](#watch-the-flights) ·
[Profiles](#profiles) · [Running scenes yourself](#running-scenes-yourself) · [While it runs](#while-it-runs) ·
[Reading the results](#reading-the-results) · [Troubleshooting](#troubleshooting) · [UE4SS](#ue4ss-only-for-the-benchmark) ·
[Writing scenes](#writing-scenes)

## The benchmark kit
`OblivionPerfFix-<version>-Benchmark.zip` is a fourth file of every release, built by the release workflow like the
fix zips (listed in `SHA256SUMS.txt`, with a build attestation and a VirusTotal scan). It holds what the benchmark needs
to run and nothing to build: the scripts and the in-game mod from this repository (the list is `kit/files.txt`), the
window-hook DLL built by the workflow (`tools/benchhook/build/`), this release's fix in `fix/`, three launchers and a
`README.txt`. UE4SS and PresentMon are not in it; it downloads them from their own projects on first use, as
`tools\Setup-Bench.ps1` does.

| Launcher | What it does |
|---|---|
| `Benchmark.cmd` | gets the tools if missing; checks that the kit's fix is installed (asks once before installing another version or none, backs up what it replaces in `backup\<time>\`, keeps your ini values); then `tools\Bench-Report.ps1 -Scenario showcase -Profile own -Session fresh -MenuPsoWait`: the Imperial City flight, stock vs fix, 2 rounds, each run in a newly started game that waits hidden at the main menu until its start-up shader precompile is done, then flies at once (no warm-up scene). On screen, in front, with your sound (`-Watch`), in your own window mode: don't touch the mouse or keyboard or switch windows while it runs (a game that is not in front turns frame generation off; the report marks such runs "not comparable"; mouse look and movement keys are ignored during the flights, Esc and Tab are not). Prints and opens the report. `Benchmark.cmd -Rounds 3` for more rounds, `-Rounds 1` for a quick look |
| `Benchmark-Hidden.cmd` | the same, hidden and muted (never in front, no taskbar button), to keep using the PC meanwhile |
| `Check-GPU.cmd` | scenario `gpu-check` (the pond by Fort Doublecross in the rain, 30 s) on screen at your settings (`-Hidden`: hidden and muted); prints fps, the graphics card's and the game thread's busy time per rendered frame, and a verdict (graphics card, CPU or frame cap). Change settings in the game, run it again: each run puts back the settings you had when it started |

The `.cmd` files run `tools\Bench-Kit.ps1` with PowerShell's execution policy set to *Bypass* for that window only, and
clear the downloaded-file mark on the kit's own files, so there is nothing to unblock or allow by hand. The kit's
working files go to `run\`, results to `captures\bench\`, downloads to `third_party\`, all inside the kit's folder.

## Quick start
From the repository (to run other scenes, change scenes or develop): you need Windows 10 or 11, Oblivion Remastered on
Steam (started once by hand, so the licence screen and first-start settings are done), the fix installed
([README](../README.md#install-two-minutes)), the free
[Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/visual-cpp-build-tools/) (*Desktop development
with C++*, to build the small window-hook DLL; or copy `tools\benchhook\build\` from the benchmark kit of the same
release) and about 1 GB of disk space. Download this repository (*Code* > *Download ZIP*, extract it, or `git clone`) and
open PowerShell in its folder:

```powershell
# 0. Each new PowerShell window: allow this folder's scripts (this window only); once after a ZIP download: unblock them.
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
Get-ChildItem -Recurse | Unblock-File

# 1. Once: get the tools (PresentMon pinned; UE4SS the newest official build, 1181 or newer) and build the hook.
tools\Setup-Bench.ps1 -Check      # what's there and what's missing; changes nothing
tools\Setup-Bench.ps1

# 2. Stock vs fix on the default Cheydinhal scenes: 4 runs, under an hour, hidden and muted.
tools\Bench-Report.ps1

# 3. Share: paste captures\bench\_report\<time>\report.md into a "Benchmark result" issue and attach the json files.
```

`Bench-Report.ps1` runs the benchmark alternately with every fix switched off ("stock": the DLL loaded with all its
switches at 0, so it leaves the game's code alone, except `FrameRateLimitAtStart`, which only makes the game apply the
frame-rate limit you saved, a game bug with frame generation on, where it can (FSR frame generation without NVIDIA
Reflex): so both sides run at the same cap) and with the fix
as installed, then writes `report.md` and copies of
the runs' `result.json` with your user name, PC name and folder paths removed. Share it in a
[Benchmark result](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/new/choose) issue.

Useful options: `-Rounds 3` (more stock/fix pairs, less noise), `-Profile cpu` (display profile, [below](#profiles)),
`-Scenario showcase` (another scenario, [below](#what-to-run)), `-Scenes canal-noon,inn-door` (some scenes only),
`-Watch` (on screen with sound, at your own settings), `-Base <runs> -Test <runs>` (a report from runs you already made).

## Reproduce the numbers in the README
Each row of the README's results table, as one command (stock vs fix, alternating, a report at the end). Profiles that
use your monitor's resolution (`promo`, `native*`) give the README's numbers only on a 4K monitor; on another
one they measure the same scene at your resolution. `user` (like `own`) runs with your own settings as they are, so its
numbers are yours, not the README's.

| README row | Command | What the profile sets |
|---|---|---|
| Imperial City flight, 4K, frame generation, 60 fps cap | `tools\Bench-Report.ps1 -Scenario showcase -Profile native-cap60 -Rounds 2` | your monitor's resolution, FSR Performance, frame generation, ray tracing, Ultra with Shadows High, 60 fps cap before frame generation |
| Imperial City flight, 4K, no frame generation (the video) | `tools\Bench-Report.ps1 -Scenario showcase -Profile promo -Rounds 2` | the same without frame generation, no cap |
| Cheydinhal flight, 4K, no frame generation | `tools\Bench-Report.ps1 -Scenario cheydinhal-show -Profile promo -Rounds 2` | as above |
| Cheydinhal streets when the CPU is the limit | `tools\Bench-Report.ps1 -Profile cpu -Rounds 2` | 1280x720 window, FSR Performance, no frame generation |
| Your own settings, any scene | `tools\Bench-Report.ps1 -Scenario showcase -Profile own` | nothing: the game as you set it up |
| 1.2.3: the horse corral (`PhysAnimNoEmptyLock` + `RigTraceReuse` off vs on) | `tools\Run-Benchmark.ps1 -Scenario playlog-spots -Scenes brindle-corral-hold -Profile native-cap60 -Label corral-off -FixIni @{ PhysAnimNoEmptyLock = 0; RigTraceReuse = 0; HitchLedger = 1; HitchLedgerTickSplit = 1 }`, then the same with `-Label corral-on` and both keys at 1, in turn, fresh games | as the first row; compare the game thread's tick work per frame with `py -3 tools\tick_split.py <ticksplit csv> --run <run folder>` |
| 1.2.3: collision checks (`ChaosMaxPayloadSize` 0 vs 10000000) | the same pattern with `-Scenario skingrad-spots -Scenes sk-d` and `-Scenario ic-spots -Scenes ic-vilverin-pass`, `-FixIni @{ ChaosMaxPayloadSize = 0; ... }` vs `10000000` (start-up key: a fresh game per side) | as above |

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
| `gpu-check` | the pond by Fort Doublecross north-east of Leyawiin in the rain, late afternoon, 30 s, profile `own` (the kit's `Check-GPU.cmd`) | 1, ~1 min | is the graphics card or the CPU the limit at your settings? |
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
In the benchmark kit: `Benchmark.cmd` shows the Imperial City flight on screen, stock and fix. From the repository:
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
- **After a graphics driver update** the fix shows the game's own shader screen once at the first start (`ShaderScreenAfterDriverChange`):
  the runner waits for it (at most 6 minutes from the launch) instead of treating the still start-up as a hang, and the in-game mod
  starts its per-frame driver once the screen is gone. The pause shows as `watch: ...` lines in the run output and under
  "Watchdog:" in `report.md`
- If the game crashes or hangs, it keeps the evidence in the run folder, restarts the game and carries on; such a run
  is marked not comparable (`comparable=no` in its last line, a warning in the report): repeat it before comparing.
- **To stop a run, use `tools\Run-Benchmark.ps1 -Stop`** (it ends after the current scene and closes or parks the game
  properly), not Task Manager or `Stop-Process`. If the runner is killed anyway, a small hidden guard it started keeps
  its game hidden and muted and closes it cleanly; a game you start yourself is never touched.
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
- **Noise:** two identical runs differ by up to about 10% (NPCs move; with `-NoPin` the weather differs too). Compare
  alternating runs (`Bench-Report.ps1` does), treat differences under 5% as noise, and check a scene's spread with
  `tools\Repro-Check.ps1` ([mini-scenarios](#identical-runs--pin-and-toolsrepro-checkps1)).
- **The first 2-3 minutes after a game start are slower** (the game compiles graphics pipelines in the background);
  the runner waits for that before measuring.
- **Frame generation:** frame times are presented frames, generated ones included.

### What a slow frame was doing: stack timelines (`-Stacks`)
To see which code runs during a hitch, sample the threads' call stacks with time stamps on PresentMon's clock and join
them with the slow frames afterwards:
```powershell
tools\Run-Benchmark.ps1 -Label st -Scenes inn-door -Pass perf -Stacks GameThread,OblivionThread
tools\Analyze-Hitch.ps1 -Run st -Segment inn-door/exit -MinMs 50      # per slow frame: what each sampled thread was in
tools\Sample-Timeline.ps1 -OutPrefix captures\t1 -Seconds 10          # the sampler alone, on a running game
```
- `-Stacks <threads>` samples those threads during door legs, flights and walks into `stacks\<scene>-timeline.csv`
  (time, thread, stack index) and `stacks\<scene>-stacks.txt` (one collapsed stack per line, root first, frames as
  `module+0xFUNCSTART`). Thread names may use wildcards (`'*Worker*'`).
- `-StackSampler fast -StackIntervalMs 2` uses `tools\FastSampler.cs` (its own x64 unwinder, ~30 us per thread sample)
  instead of dbghelp (`tools\StackSampler.cs`, ~1 ms or more), so the workers can be sampled too.
- Sampling suspends each thread for a moment: a `-Stacks` run's fps and frame times are not comparable with other runs.
- `Analyze-Hitch.ps1` names functions from `tools\symbols.csv` (`rva,name,note`) when that file is there; without it the
  frames stay `exe+0x...`.

## Troubleshooting
| You see | Do |
|---|---|
| `NEEDSUSER` | the game shows a screen the benchmark won't click through (licence, first start): start the game once from Steam and get to the main menu |
| a run was stopped half-way, or the game is still running hidden | `tools\Run-Benchmark.ps1 -Close`: closes it and restores settings, saves and UE4SS |
| `BENCH-RESULT partial` | some scenes failed; the report says which and why (often a crash, then the runner restarted the game) |
| `frameCapture: cpu-only` in a result | PresentMon could not follow frames to the display; frame times are CPU-side, GPU busy is missing. The log has a `PresentMon diagnosis:` line |
| your game or Documents folder isn't found | set `$env:OBLIVION_GAME_DIR` (the folder with `OblivionRemastered\`) or `$env:OBLIVION_SAVED_DIR` (`...\My Games\Oblivion Remastered\Saved`); `tools\GamePaths.ps1` shows what it found. A copy of the game outside Steam (no `steam_api64.dll` in the folder) is started directly |
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

**Which UE4SS:** the official experimental build from the UE4SS project, build 1181 or newer: the first with the fix
for a memory leak the benchmark would otherwise hit (every Lua call of a game function with an 'Out' parameter kept its
table in memory for good, about 1 GB per hour for a mod that does it every frame; upstream issue
[#1446](https://github.com/UE4SS-RE/RE-UE4SS/issues/1446), fixed by
[pull request #1452](https://github.com/UE4SS-RE/RE-UE4SS/pull/1452)). Upstream keeps only the newest build on its
[`experimental-latest` release](https://github.com/UE4SS-RE/RE-UE4SS/releases/tag/experimental-latest) and replaces the
file with each new one, so setup takes the newest `UE4SS_v3.0.1-<build>-g<commit>.zip` there (never the `zDEV-` one),
checks it against the SHA-256 GitHub lists for it, and records build and SHA-256 in `third_party\ue4ss\oblvbench-build.txt`;
the report shows them. The benchmark was tested with `UE4SS_v3.0.1-1181-g83330635.zip` (SHA-256
`B9A166A756605B21058A64CDE93BCAAB1A855DBA3553E5C7E6CE0FA8FDFAD7B9`): the report says "tested build" for that file and
"newer than tested" for a later one. Once unpacked, a build is kept; `tools\Setup-Bench.ps1 -UpdateUE4SS` looks for a
newer one. If setup can't download it, download the zip yourself and leave it in your Downloads folder or pass it with
`-UE4SSZip <path>`. Any other build (older, or your own) needs `-AllowOtherUE4SS` and is untested: say so when you share
results.

## Mini-scenarios: iterate on one problem spot
Full scenarios are for confirming; while working on one problem, run a scene that reproduces just that spot in 30-60 s
and keep the game open between runs (`-Session reuse`), switching the fix's ini key between runs (A-B-B-A).
`tools/slice_flight.py` cuts such scenes out of a timed `fly` scene, with the original's waypoints, speeds, hour, weather
and timescale, starting a few seconds before each spot so its cells and characters load during the measured part:

```
py -3 tools\slice_flight.py --src showcase --scene rumare-loop-flight --out my-spots --spot waterfront:23.9:26.5:"the Waterfront"
.\tools\Run-Benchmark.ps1 -Scenario my-spots -Scenes waterfront -Runs 2
```

Spot times are seconds into the full flight (the plots' time axis). Check once that the slice shows the problem before
you iterate on it. Runs are made comparable by default (pinned conditions, a fixed random seed: below); start each pass from a fresh
world (the default), and note what still differs between runs (characters' behaviour, GPU state).

### The challenge scenes (`challenges`)
The open problems we invite help on (GitHub issues labelled "challenge") each have a save-free scene in
`bench/scenarios/challenges.json`: `ic-waterfront-pass` (the Imperial City Waterfront's character bursts),
`brindle-corral-hold`, `brindle-control-hold` and `brindle-corral-walk` (the Brindle Home horse corral, a control spot
~120 m west, and a walk inside the corral), `ride-track-fly` and `ride-track-foot` (a riding track flown at horse speed
and walked; riding a horse itself is not in the benchmark yet). Iterate small with one game kept open, then confirm at
your own settings; with `HitchLedger=1` and `HitchLedgerTickSplit=1` in the ini (set before the game starts) each run
also leaves the tick split, which `py -3 tools\tick_split.py <csv> --run <run folder>` ranks per scene:

```
.\tools\Run-Benchmark.ps1 -Scenario challenges -Scenes brindle-corral-hold, brindle-control-hold -Profile small -Session reuse
.\tools\Run-Benchmark.ps1 -Scenario challenges -Scenes brindle-corral-hold -Profile own
```

Run the hold and its control in the same game, one after the other, so they differ only in the place. The issue for each
challenge says what we know and what we already tried.

### Identical runs: pinned conditions, `-FixedSeed` and `tools\Repro-Check.ps1`
Every run is pinned by default (`-NoPin` for the old behaviour): every scene gets the same conditions in every run: the weather (the scene's, else the scenario's default, else
Clear) forced and held (the climate cannot change it), the hour (else 12) and a slow clock (`timescale` 1: one game
minute per real minute instead of 30, so the light does not move). A scene opts out of any of them with `"keep"`, e.g.
`"weather": "keep"` for a weather test; a run from a save keeps the save's conditions where nothing is named. A scenario
can refuse it with `"pin": false` (a test of weather changes). With `-NoPin` only what a scene names is set: a fresh world then picks
its own weather (overcast in one run, sunny in the next), and after a teleport the game may blend into a forced weather
over 15 game minutes, which a short scene never finishes. In a pinned run each scene's `cond` in `result.json` (and the "Conditions"
table in `report.md`) records what was set and what the game reported right after and at the end
(`hour=18.251 wx=Clear:1.00`: the weather and how far its blend is).

Before iterating on a new mini-scenario, check how repeatable it is:

```
.\tools\Repro-Check.ps1 -Scenario my-spots -Scenes waterfront -N 3
.\tools\Repro-Check.ps1 -Runs a-r1,a-r2,a-r3          # runs you already made
```

It prints per scene the spread of fps, worst frame, frames >= 33 ms, hitches and Game / Render / GPU ms, how alike the
end screenshots are (SSIM), and the conditions each run had, with a verdict (`captures\bench\_repro\<label>\repro.md`).
A difference smaller than the spread it reports cannot be seen with that many runs. Runs whose frame log lost most of
its frames are left out of the spread. The benchmark also starts the game with Unreal's `-FixedSeed` (`-NoFixedSeed` turns it off): the engine's random
generators start from the same seed in every benchmark game instead of from the clock. It is passed to that launch only
(`steam -applaunch`), never written into your Steam launch options, and `result.json` records whether it reached the
game (`fixedSeed`). The seed is set once per game start, so only runs in a newly started game (`-Session fresh`) begin
from the same state. Still not identical: characters' behaviour (when and where they walk
depends on frame timing), stutter counts on flights, and the GPU's speed state. For pictures of characters, a static
scene's `"aiHold": true` switches NPC AI off from before its spawns to its end, so they stand still for the screenshot
(such a scene's numbers are marked `aiHeld` and are not a measurement).

## Writing scenes
A scenario file (`bench/scenarios/<name>.json`) has `places` (a worldspace or interior cell, a position, a heading),
`defaults`, `weathers` (weather form IDs) and `scenes`. Copy a scene from `cheydinhal.json` and change it:

| Key | Meaning |
|---|---|
| `id`, `type`, `tags` | name, `static` / `door` / `fly` / `walk`, tags for `-Tags` |
| `at`, `hour`, `weather` | a place from `places`, the game hour, a weather name from `weathers` (always set both: runs then see the same world) |
| `timescale` | game minutes per real minute during the scene (game default 30; a low value such as 1 holds the light) |
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
