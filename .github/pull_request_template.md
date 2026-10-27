### What it changes
<!-- One paragraph: what was slow, what the change does, and which ini key switches it (default value). -->

Closes / helps with: <!-- #issue (a challenge, a bug report) -->

### Why the result is the same
<!-- Identical (the same bytes come out), equivalent (argued from the UE 5.3 source; say what may differ and why the
stock game already varies it), or a timing change (bounds, order, off switch). See CONTRIBUTING, "What counts as a fix". -->

### Measurement
<!-- Scene and command (a public scenario, e.g. -Scenario challenges -Scenes ...), profile, how many runs, A-B-B-A in one
kept game if possible. Before / after: game-thread (and GPU) ms per rendered frame, time over 16.7 ms or hitches.
Hardware: CPU, GPU, RAM, resolution, upscaler, frame generation, frame cap. -->

| | off | on |
|---|---|---|
| | | |

### Pictures
<!-- For anything that could touch the image: before/after screenshots of the same view, or the visual pass of
tools\Compare-Benchmark.ps1. "Cannot touch the picture" needs a sentence why. -->

### Checklist
- [ ] `src\OblivionPerfFix\build.ps1 -SelfTest` passes (0 failures) against the game build named in the README
- [ ] Byte-pattern search with checks; the fix switches itself off with one log line when anything differs
- [ ] New ini key with a safe default, explained in `OblivionPerfFix.ini`; a self-test for the new code
- [ ] `CHANGELOG.md` line under `## Unreleased`; a section in `docs/how-it-works.md` for a new fix
- [ ] `python tools\Audit-UESource.py --ue <your UE checkout> src docs` finds no copied engine text
- [ ] The code is my own work. Idea source, if any (we credit it): <!-- post, video, mod description, or "my own" -->
