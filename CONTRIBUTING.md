# Contributing

Thank you for helping. There are four ways in, from easiest to most involved.

## 1. Reports

- **Test report** (did it load, your hardware, how it felt): the *Test report* issue form. Reports from NVIDIA and Intel
  graphics cards, 4-6 core CPUs, Steam Deck and the Game Pass version are the most useful right now.
- **Bug report:** the *Bug report* form, with `OblivionPerfFix.log` attached (it is next to `version.dll`). For a
  crash, also say what you were doing; for stutter, set `HitchLedger=1` in `OblivionPerfFix.ini`, play the spot again
  and attach `OblivionPerfFix-ledger.csv`.
- **Benchmark result:** the benchmark kit (`OblivionPerfFix-<version>-Benchmark.zip` from the release, double-click
  `Benchmark.cmd`) or, from the repository, `tools\Bench-Report.ps1` compares the game with and without the fix on your
  PC and writes a `report.md` to paste into the *Benchmark result* form. See [docs/benchmark.md](docs/benchmark.md).

## 2. A game update broke a fix

The log says `not found` or `disabled` for a fix after a game patch. Follow [docs/porting.md](docs/porting.md): find the
changed pattern with `tools\find_patterns.py`, update it in that fix's file, run the self-test, then a benchmark A/B and
an image comparison. One pull request per fix is easiest to review.

## 3. A new fix

Read [AGENTS.md](AGENTS.md) first: the layout, the build, how a fix finds its code, and the rules. In short:

- **Same image, same game.** A fix may only do the same work in a cheaper way: skip work whose result would be
  identical, reuse memory, move work to a thread that would otherwise wait, or use the engine's own cheaper path. No
  setting that changes what is drawn or how the game plays. A timing change (doing something a few frames later) needs
  bounds, the game's order and an off switch, and must be called out. See "What counts as a fix" below.
- **Fail safe.** Find the code by byte pattern, check everything the fix relies on, and leave the game alone with one
  log line if anything differs. Every fix gets an ini switch with a safe default and a self-test.
- **Prove it.** A benchmark A-B-A (or ABBA) on a public scenario for speed, and an image comparison for anything that
  could touch the picture. Run-to-run noise is about ±10%: per-thread milliseconds are more trustworthy than fps.
- **Test quickly and alike.** Work on a short scene that reproduces just the problem (30-60 s; `tools\slice_flight.py` cuts
  one out of a flight), with the hour, weather and timescale pinned so every run sees the same world; confirm on the full
  scenario at the end ([docs/benchmark.md](docs/benchmark.md), "Mini-scenarios").
- **Licences.** No game files and no game bytes beyond short search patterns. No Unreal Engine source text (names,
  offsets and descriptions in your own words are fine): `tools\Audit-UESource.py` checks for copied text.

Where a new fix goes: one file per fix, in the folder of its area under `src/OblivionPerfFix/src/` (`render`,
`streaming`, `physics`, `npcs`, `loading`, `menus`, `saves`, `framegen`, `guards`, `memory`, `diagnostics`), its switch in
`core/common.h` (`Config`), `core/config.cpp` and the shipped `OblivionPerfFix.ini` (explained in plain words), its
installation in `core/main.cpp`, its checks in `tests/`, and a section in [docs/how-it-works.md](docs/how-it-works.md).
Start the file with a comment that says what was slow, what the fix changes and why the result is the same.

## 4. Open challenges

Some problems are known, measured and still open: we have run out of ideas that keep the game identical, or the
remaining lever needs more measurement than we have done. Each one is an issue with the label
[`challenge`](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues?q=is%3Aissue+is%3Aopen+label%3Achallenge), listed in the pinned [*Open challenges* tracker](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/30).
They are not bug reports: they describe a spot where the game is still slower than it could be, with

- what you see, and how much of the 60 fps budget it costs (our numbers, with the fix version, scene and hardware);
- a save-free scene that reproduces it in a minute or two (`-Scenario challenges` and the scene id), and the commands;
- how to measure it, what we know (the cause chain in Unreal Engine terms), what we tried and why it did not ship;
- the rules a fix for that spot must meet.

**How to take part.** Comment on the challenge to say what you are looking at (so two people don't do the same
measurement), then post what you find there, even a partial result. Useful contributions, from quick to deep:

1. **A measurement on your hardware** (CPU, GPU, settings, fix version; `report.md` and the ledger summary). Other CPUs
   than ours (Ryzen 7 5800X) are especially welcome: X3D chips, Intel, 6-core parts.
2. **A named cost:** a profile (stacks, zones) that shows which work in the slow frames is repeated, wasted or waiting.
3. **A prototype** behind a default-off ini key, measured A-B-B-A on the challenge's scene, with screenshots.
4. **An argument** from the UE 5.3 source that some cheaper path gives the same result.

A challenge with the label `needs-repro` has no reliable scene yet (the numbers come from a play session); a better
repro is a full contribution there. `being characterised` means we know the cost but not yet the cause.

## What counts as a fix

| Kind | Accepted when | Example |
|---|---|---|
| **Identical** | the same bytes come out: skip work whose result is already there ("skip only when nothing changed"), reuse memory, do the same work on a thread that would otherwise wait, in the same order | `PhysAnimNoEmptyLock`: a lock and copy skipped when there is nothing to drive |
| **Equivalent** | the UE 5.3 source shows the result is the same, and anything that may differ is an internal the stock game already varies between runs (e.g. the shape of a collision tree); verified by a check mode that runs both and counts differences | `RigTraceReuse`: a repeated trace with bit-identical inputs in the same rig run reuses the first result (0 differences in millions of verified repeats) |
| **Timing** | far away or unseen work a few frames later, in the game's order, with hard bounds and an off switch | `NpcCreationPacing` |
| **Not accepted** | anything that changes what is drawn (quality settings, LODs, update rates of visible things, ray tracing modes), how the game plays (AI rates, physics), or adds latency (running the game thread a frame ahead of rendering); keeping the world running while the player can't see or act for longer than the stock game | lower NPC perception rates, shadows at High |

Before/after screenshots (`tools\Compare-Benchmark.ps1` visual pass, or the runner's end-of-scene shots) for anything
that could touch the picture.

## Measuring

- **The target:** the 60 fps budget, 16.7 ms per *rendered* frame on the game thread and the GPU. Report time over 16.7
  ms per rendered frame and the frames that stick out (hitches), not only average fps. With frame generation every
  PresentMon row is a presented frame: rendered fps = fps / 2, thread ms per rendered frame = the column x 2.
- **Jitter excess**, the number our challenges quote: J = max(0, t - max(16.7, median of the +-30 neighbouring frames))
  per rendered frame t; a frame with J >= 4 ms is a visible drop at 60. A slow, gradual rise does not count; a spike does.
- **A-B-B-A in one game:** keep one game open (`-Session reuse`), flip your ini key between runs (most keys are re-read
  every second; start-up-only keys need `-Session fresh`), and compare per-thread ms. Noise is about ±10%; check a new
  scene's spread with `tools\Repro-Check.ps1 -N 3` first.
- **GPU time** is bimodal between identical runs on some cards: compare it only between runs made the same way.
- **Where a slow frame went:** `HitchLedger=1` (and `HitchLedgerMs=17` to log every frame over the cap) and
  `tools\stutter_ledger.py`. **What code ran:** `-Stacks` with `tools\Analyze-Hitch.ps1`, or your own profiler
  (Superluminal, PIX, VTune): the exe has no symbols, so name what you find by UE function where you can and by
  `exe+0x...` offset for game build 19115871 otherwise.
- **Say what you ran:** fix version, game build, CPU, GPU, RAM, resolution, upscaler, frame generation, frame cap,
  scene and profile. Numbers from a play session are welcome as leads; numbers for a pull request come from a public
  scene that anyone can run.

## Your own work, and credit

- Contributions must be your own work. Please don't paste code, data, captures, addresses or text from other mods or
  closed projects. Ideas are different: if an idea came from somewhere (a post, a video, another mod's description),
  say where, and we credit the source in the changelog and the docs.
- In a pull request, please confirm that the code is your own and name any idea source.
- Fixes that come from a contribution are credited by name in `CHANGELOG.md` and in the fix's section of
  [docs/how-it-works.md](docs/how-it-works.md).

## Pull requests

- `src\OblivionPerfFix\build.ps1 -SelfTest` passes (0 failures) against the game build named in the README.
- The CI build is green.
- `CHANGELOG.md` has a line under `## Unreleased`.
- The PR description has the measurement (scenario, profile, before/after) and, for anything visual, the image
  comparison; the pull request template lists the rest.
