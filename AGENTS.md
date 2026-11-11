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
  `WaterInfoSkip` is equivalent too: it skips a water zone's redraw whose inputs equal its last one's, so content those
  inputs can't show (e.g. finer ground mips streamed in since) refreshes at the next real change, not at the next menu.
  `CreatePreload` is timing only too (a far create waits a few frames while its files load in the background). Also
  equivalent, each row below says how (since 1.3.0): `BodyRemovalSpread`, `StreamRegisterGranularity`, `RigImportTemplate`,
  `WaterInfoSpread`, `CellBufferEvictDelayMs`, `DrawBehindLoadingScreen`, `MapPopupSpread`, `OtFrameTrim` bit 1,
  `PacingOwnerWalk` (timing only) and `GpuMenuShot`'s leftover save handlers (a game bug leaves one bound after each save
  made in a menu; only with `SaveThumbLeftoverFix=0` or on the OBSE route, as the fix removes them; they get the menu
  picture a few frames later, or none if it drops, in a finished save's buffer that nothing reads; the menu picture
  itself stays identical).
  `FarPawnMoveHalfRate` / `FarHorseMove` are **not identical** (user decision, on by default, switchable off): calm
  pawns and horses >= 100 m away move at `FarPawnMoveHz` (30) with the skipped frames' time summed, as on a 30 fps PC.
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
| `docs/` | `fix-index.md` (one row per fix: ini key, log tag, source file; search it, don't read it whole), `how-it-works.md` (each fix in plain words, same groups as the source), `research.md` (evidence, rejected ideas), `porting.md` (game update), `benchmark.md` |
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
Every fix has one row (ini key, log tag, source file, what it changes) in [docs/fix-index.md](docs/fix-index.md). It is long
and is not loaded with this file: search it, do not read it whole. Evidence per fix is in `docs/research.md`, plain words
in `docs/how-it-works.md`.
- By ini key: `grep -n "BoneUploadStores" docs/fix-index.md`
- By log tag (from `OblivionPerfFix.log`): `grep -n "\[atm\]" docs/fix-index.md`
- A new fix adds its row to `docs/fix-index.md` (same columns, in its area's group), next to its ini comment and
  `CHANGELOG.md` entry.

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
  reading them whole. Logs: grep the tag. The fix index (`docs/fix-index.md`): grep the key, tag or file. Benchmark runs: read `report.md`, not `result.json`.
- Run `tools\Run-Benchmark.ps1` in the background and poll `tools\Bench-Status.ps1`; its last line is `BENCH-RESULT ...`.
