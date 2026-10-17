# Contributing

Thank you for helping. There are three ways in, from easiest to most involved.

## 1. Reports

- **Test report** (did it load, your hardware, how it felt): the *Test report* issue form. Reports from NVIDIA and Intel
  graphics cards, 4-6 core CPUs, Steam Deck and the Game Pass version are the most useful right now.
- **Bug report:** the *Bug report* form, with `OblivionPerfFix.log` attached (it is next to `version.dll`). For a
  crash, also say what you were doing; for stutter, set `HitchLedger=1` in `OblivionPerfFix.ini`, play the spot again
  and attach `OblivionPerfFix-ledger.csv`.
- **Benchmark result:** `tools\Bench-Report.ps1` compares the game with and without the fix on your PC and writes a
  `report.md` to paste into the *Benchmark result* form. See [docs/benchmark.md](docs/benchmark.md).

## 2. A game update broke a fix

The log says `not found` or `disabled` for a fix after a game patch. Follow [docs/porting.md](docs/porting.md): find the
changed pattern with `tools\find_patterns.py`, update it in that fix's file, run the self-test, then a benchmark A/B and
an image comparison. One pull request per fix is easiest to review.

## 3. A new fix

Read [AGENTS.md](AGENTS.md) first: the layout, the build, how a fix finds its code, and the rules. In short:

- **Same image, same game.** A fix may only do the same work in a cheaper way: skip work whose result would be
  identical, reuse memory, move work to a thread that would otherwise wait, or use the engine's own cheaper path. No
  setting that changes what is drawn or how the game plays. A timing change (doing something a few frames later) needs
  bounds, the game's order and an off switch, and must be called out.
- **Fail safe.** Find the code by byte pattern, check everything the fix relies on, and leave the game alone with one
  log line if anything differs. Every fix gets an ini switch with a safe default and a self-test.
- **Prove it.** A benchmark A-B-A (or ABBA) on a public scenario for speed, and an image comparison for anything that
  could touch the picture. Run-to-run noise is about ±10%: per-thread milliseconds are more trustworthy than fps.
- **Licences.** No game files and no game bytes beyond short search patterns. No Unreal Engine source text (names,
  offsets and descriptions in your own words are fine): `tools\Audit-UESource.py` checks for copied text.

Where a new fix goes: one file per fix, in the folder of its area under `src/OblivionPerfFix/src/` (`render`,
`streaming`, `physics`, `npcs`, `loading`, `menus`, `framegen`, `guards`, `memory`, `diagnostics`), its switch in
`core/common.h` (`Config`), `core/config.cpp` and the shipped `OblivionPerfFix.ini` (explained in plain words), its
installation in `core/main.cpp`, its checks in `tests/`, and a section in [docs/how-it-works.md](docs/how-it-works.md).
Start the file with a comment that says what was slow, what the fix changes and why the result is the same.

## Pull requests

- `src\OblivionPerfFix\build.ps1 -SelfTest` passes (0 failures) against the game build named in the README.
- The CI build is green.
- `CHANGELOG.md` has a line under `## Unreleased`.
- The PR description has the measurement (scenario, profile, before/after) and, for anything visual, the image
  comparison.
