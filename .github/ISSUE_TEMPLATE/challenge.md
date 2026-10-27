---
name: Open challenge
about: A known, measured spot where the game is still slower than it could be, for the community to help solve (not a bug report)
title: "Challenge: <place or situation>, <what it costs>"
labels: ["challenge", "help wanted"]
---

> **This is an open challenge, not a bug report.** <One or two sentences: what is still slow, what the fix already does
> about it, what kind of help we are looking for.> See [CONTRIBUTING.md](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/blob/main/CONTRIBUTING.md#4-open-challenges)
> and the pinned [*Open challenges* tracker](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/30).

### What you see
<What a player notices, where, and how often. A small table of the numbers that matter: game-thread / GPU ms per rendered
frame, time over 16.7 ms, hitches, against the 60 fps budget. Say where each number comes from: fix version, scene,
profile, hardware. Numbers from a play session are labelled as such.>

### Reproduce it (save-free, <how long>)
```powershell
tools\Run-Benchmark.ps1 -Label <x> -Scenario challenges -Scenes <scene ids> -Profile own -Session reuse
tools\Run-Benchmark.ps1 -Close
```
<What the scene does (start, path, hour, weather), how repeatable it is (`tools\Repro-Check.ps1 -N 3`), and whether it
reproduces the whole problem or only part of it.>

### How to measure
<Which numbers in `report.md`, which ledger columns (`HitchLedger=1`, `HitchLedgerMs`), which log lines, stacks or
profiler views, and what they mean (game-thread vs GPU bound, J, time over 16.7 ms).>

### What we know
<The cause chain in Unreal Engine terms (functions, components, tick groups); our numbers with version, scene and
hardware; shares of the problem. Long parts in <details> blocks.>

<details><summary>What we tried and why it did not ship</summary>

| Idea | Result | Why not |
|---|---|---|
| | | |
</details>

### Rules a fix must meet
<Identical or equivalent results (CONTRIBUTING, "What counts as a fix"), plus anything specific to this spot.>

### What a good contribution looks like
<Measurements on other hardware, a named cost, a prototype with an A-B-B-A on the scene, an argument from the UE source.>
