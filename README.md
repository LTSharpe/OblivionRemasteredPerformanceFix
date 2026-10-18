# Oblivion Remastered Performance Fix

[![Build](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/actions/workflows/build.yml/badge.svg)](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/LTSharpe/OblivionRemasteredPerformanceFix?include_prereleases)](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/releases)

Higher frame rates in towns and crowds, far fewer hitches while you travel, no long frame when you step out of a door,
quicker menus, and a few crash fixes, for *The Elder Scrolls IV: Oblivion Remastered*. **The game looks and plays the
same**: no lowered settings, no shorter view distance, ray-traced lighting stays on its highest setting. The fix changes
*how* the engine does its work, never *what* it draws.

> **Built by Claude.** The profiling, the reverse engineering, the fixes, the benchmark and these docs were done by Claude,
> Anthropic's AI model, working in Claude Code. [LTSharpe](https://github.com/LTSharpe) set the goal and the rules (never
> change the look), made every decision and played every build. All of the source is here for you to read.
>
> Free and open source. If it makes your game better and you'd like to say thanks, you can
> [support the work on GitHub Sponsors](https://github.com/sponsors/LTSharpe). Entirely optional.

> **Beta.** So far it has been tested on one PC (AMD Ryzen 7 5800X, AMD Radeon RX 9070 XT, Windows 11, Steam). If you try
> it, please [tell us how it went](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/new/choose),
> especially on NVIDIA or Intel hardware, on CPUs with fewer cores, and on the Game Pass version.

[Results](#results) · [Check it on your PC](#check-it-on-your-pc-before--after) · [Install](#install-two-minutes) · [AMD ML frame generation](#optional-amd-ml-frame-generation-radeon-rx-9000) ·
[What it does](#what-it-does) · [Uninstall](#uninstall) · [Is it safe?](#is-it-safe) · [Compatibility](#compatibility) ·
[FAQ](#faq) · [Help improve it](#help-improve-it)

## Results

| On the test PC | Stock game | With the fix |
|---|---|---|
| **Flight round the Imperial City** at 4K with frame generation, frame rate capped at 60 before frame generation (the maintainer's own settings) | 72 fps, 1% low 33, 19.5 hitches of 50 ms or more per flight, worst frame 135 ms | **102 fps** (+41%), 1% low 45, **4.8 hitches**, worst frame 66 ms |
| **Flight through Cheydinhal** at 4K, no frame generation | 55 fps, 1% low 26, 4-6 hitches of 50 ms or more | **63 fps**, 1% low **43**, **no hitch** |
| **Cheydinhal streets** at 4K with frame generation (3 views) | 64 fps (1% low 50) | **91 fps** (1% low 68), +43% |
| **Cheydinhal when the CPU is the limit** (720p, no frame generation) | 45 fps | **57 fps**, +25% |
| **Stepping out of a door** at 4K: the worst frame as the picture comes back | 187 ms | **95 ms** |
| **Opening the menu** on the Graphics page at 4K | 0.74 s freeze | **0.43 s** |
| **Opening the pause menu or the inventory** at 4K | 0.36 s freeze | **0.20 s** |
| **Alt-tab** after using the inventory or the map | FSR and frame generation stay off | back on at once |

![Cheydinhal at 4K, average fps, stock game vs the fix](docs/images/cheydinhal-4k.png)

Test PC: Ryzen 7 5800X (8 cores), Radeon RX 9070 XT (16 GB), 32 GB RAM, Windows 11, Steam build 19115871. "4K" = FSR
Performance, hardware ray tracing, Ultra with Shadows High. Every number is the mean or median of 2-4 runs per side, each
in a freshly started game, stock and fix taken in turn, driven by this repository's benchmark (you can re-run every
row: see [Check it on your PC](#check-it-on-your-pc-before--after)). The Imperial City row was measured with this release; the Cheydinhal
flight with an earlier build that had about two thirds of today's fixes; the street, door and menu rows with the first
third of them (the later fixes target travel, so those rows are unchanged or better now).

- **Your gain depends on your PC.** It is largest where the CPU holds the game back: towns, crowds, travel, high frame
  rates, frame generation (which needs a high base frame rate). If your graphics card is the limit (say native 4K in a
  rainy forest), the frame rate stays about the same; the hitch, door and menu fixes still help.
- **Crowds can still dip.** The game's main thread does the same work as before, and in busy places it is the limit.
- **Some hitches remain.** Crossing into a new part of the world still costs a little, mostly the old engine's own
  loading; at normal travel speed you will rarely notice it.
- **The image is unchanged:** screenshots with the fix on and off were compared automatically at dawn, noon, dusk and
  night, in clear weather, cloud, fog, rain and thunderstorms. The differences are the same size as between two runs
  without the fix.

## Check it on your PC (before / after)

Don't take our word for it: the benchmark in this repository plays the same scenes with and without the fix, by
itself, and writes a report. Every scene starts from a new game, so your saves and characters are never touched.

1. **Get the repository:** the green *Code* button > *Download ZIP*, and extract it (or `git clone` it). Install the fix
   as below, and start the game once by hand if you never have.
2. **Install the free [Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/visual-cpp-build-tools/)**
   with the *Desktop development with C++* workload (the benchmark builds a small helper DLL that keeps the game window
   hidden).
3. **Open PowerShell in the extracted folder** and run:

   ```powershell
   Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass     # allow this folder's scripts, for this window only
   Get-ChildItem -Recurse | Unblock-File                          # files from a ZIP are marked as downloaded
   tools\Setup-Bench.ps1                                         # once: fetches UE4SS and PresentMon (pinned, SHA-256 checked)
   tools\Bench-Report.ps1 -Scenario showcase -Profile own        # the Imperial City flight at YOUR settings, stock vs fix
   ```

4. **Read the result:** `captures\bench\_report\<time>\report.md` (average fps, 1% low, stutters, CPU time per engine
   thread, stock vs fix). It has no user name, PC name or folder paths in it, so you can share it in a
   [Benchmark result](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/new/choose) issue.

The game runs hidden and muted while you keep working (add `-Watch` to see and hear it); "stock" means the DLL is loaded
with every fix switched off, so the game's own code runs. Four game launches, about 15-20 minutes for the flight. To
repeat our table exactly, or to run the town, door and travel scenes, see
[docs/benchmark.md](docs/benchmark.md#reproduce-the-numbers-in-the-readme).

## Install (two minutes)

1. Download `OblivionPerfFix-<version>.zip` from [Releases](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/releases)
   or from [Nexus Mods](https://www.nexusmods.com/oblivionremastered/mods/5709) (the same files).
2. Open the game folder: in Steam, right-click *Oblivion Remastered* > *Manage* > *Browse local files*.
3. Extract the zip into that folder. It puts two files into `OblivionRemastered\Binaries\Win64\`: `version.dll` and
   `OblivionPerfFix.ini`.
4. Play. To check that it works, open `OblivionRemastered\Binaries\Win64\OblivionPerfFix.log`: it lists each fix as
   it is applied.

| You use | Do this |
|---|---|
| Steam, no mod manager | the four steps above |
| Vortex or Mod Organizer 2 | install the same zip as a mod: it has the game's folder layout. In Mod Organizer 2 the files must land in the game's folder, not in `Data` (use its Root Builder plugin, or extract by hand) |
| OBSE64, or another mod that already uses `version.dll` (some upscaler mods do) | don't overwrite that `version.dll`: download `OblivionPerfFix-<version>-OBSE.zip` instead and extract it into the game folder. It puts `OblivionPerfFix.dll` and its ini into `OblivionRemastered\Binaries\Win64\OBSE\Plugins\`, where OBSE64 loads it. A few fixes can only be installed before the game's own code runs and stay off on this route (the log says which). If both are installed, the fix loads once |
| Xbox / PC Game Pass (experimental) | download `OblivionPerfFix-<version>-GamePass.zip`; its files go into `OblivionRemastered\Binaries\WinGDK\` in the game's folder (usually `C:\XboxGames\The Elder Scrolls IV - Oblivion Remastered\`). Not tested yet: the log says which fixes found their code; no log at all means it didn't load. A [test report](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/new/choose) helps everyone |
| Steam Deck / Linux (untested) | as for Steam, then add `WINEDLLOVERRIDES="version=n,b" %command%` to the game's launch options |

**Updates:** track the mod on [Nexus Mods](https://www.nexusmods.com/oblivionremastered/mods/5709) (Vortex and Mod Organizer 2
then show when a new version is out), or on GitHub choose *Watch > Custom > Releases*. To update, extract the new zip
over the old files. If you changed settings in `OblivionPerfFix.ini`, set them again in the new one: new fixes come with
new keys.

Nothing else to set up: all fixes are on by default, and they adapt to your PC (see [Compatibility](#compatibility)).
Every fix has its own switch in `OblivionPerfFix.ini` (1 = on, 0 = off), explained in the file itself.

## Optional: AMD ML frame generation (Radeon RX 9000)

The game ships AMD's FSR 3.1.0, whose frame generation is the older, analytical kind. On a **Radeon RX 9000** graphics
card, AMD's driver can replace it with **AMD's ML (machine-learning) frame generation**, but only for games that use
FSR 3.1.4 or newer. You can give the game AMD's own, signed FSR 3.1.4 file:

1. **Download** `amd_fidelityfx_dx12.dll` from AMD's FidelityFX SDK v1.1.4:
   [github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/PrebuiltSignedDLL/amd_fidelityfx_dx12.dll](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/PrebuiltSignedDLL/amd_fidelityfx_dx12.dll)
   (click *Download raw file*). 6,675,664 bytes, file version 1.0.1.41314, SHA-256
   `12A5081257EC95B0B53AD51B4A87FB3C03F97FE0BBB59F9496968F8D50EF93A6`, digitally signed by Advanced Micro Devices
   (right-click > *Properties* > *Digital Signatures*). It is the file inside the SDK's
   [v1.1.4 release](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v1.1.4) (a 470 MB zip), the
   one AMD publishes for games to ship.
2. **Back up the game's own file:** in the game folder, go to `OblivionRemastered\Binaries\Win64\` and rename
   `amd_fidelityfx_dx12.dll` to `amd_fidelityfx_dx12.dll.original`.
3. **Put the downloaded file there** as `amd_fidelityfx_dx12.dll`.
4. **In AMD Software (Adrenalin)**, open the game's page under *Gaming* and make sure the FSR upgrades (*AMD FSR
   Upscaling* and *AMD FSR Frame Generation*) are on. With the game's own file, frame generation says *Upgrade
   Inactive - Enable FSR 3.1.4+ in-game*; with the 3.1.4 file it no longer does. Needs a driver with ML frame generation
   (it arrived with Adrenalin 25.12.1; tested with 26.9.2).
5. **In the game**, choose FSR as the upscaler and switch frame generation on.

To check it: Adrenalin's in-game overlay shows *AMD FSR Frame Generation: Active*, and `OblivionPerfFix.log` has a line
like `[fgpace] frame generation context created: provider 4.0.1 *, ... (Adrenalin's driver provider)`.

- **Why the fix matters here:** ML frame generation also needs the camera's position and direction each frame, which
  the game's FSR 3.1.0 code never sends; the fix sends it (`FrameGenCameraInfo`), uses AMD's recommended pacing
  setting for this file (`FrameGenPacingTuning`), and guards a crash that the 3.1.4 file still has
  (`FrameGenCrashGuard`). Without the fix, ML frame generation runs with the camera at zero.
- **Undo:** delete the downloaded file and rename `amd_fidelityfx_dx12.dll.original` back. Steam's *Verify integrity
  of game files* also restores the original (and so does a game update: put the 3.1.4 file back afterwards).
- **Other graphics cards:** with an NVIDIA, Intel or older AMD card the 3.1.4 file works like the game's own (FSR 3
  frame generation); there is no reason to swap it there.
- We have not measured a frame-rate difference against the game's own frame generation: ML frame generation is about
  the quality of the generated frames. This is AMD's file and AMD's driver feature: if anything looks wrong, undo it
  and tell us.

## What it does

Oblivion Remastered runs two engines at once: the original 2006 engine runs the game world, and Unreal Engine 5 draws it.
Several steps of that pipeline run on **a single CPU thread**, and when one of them is full, the whole game waits. That's
why Cheydinhal can run at 30-40 fps while Task Manager shows the CPU only about 30% busy, and why the game hitches each
time a new part of the world streams in.

The fix is one small DLL that loads with the game. For each of its fixes (about forty) it finds the specific place in
the game's code, checks that it is exactly what it expects, and changes how that code works, in memory only. Each change does **the same work in a
cheaper way**. Two of them change only *when* something happens: far-away characters you can't see are built a few
frames later, and the cells behind you are unloaded a few frames later, both in the game's own order.

| Area | What was slow or broken | What the fix does |
|---|---|---|
| **Rendering** | One thread turned each frame's drawing work into DirectX 12 commands while the other cores idled; character skeletons were copied to the graphics card with an instruction that is very slow for that memory; the weather system rebuilt the sun, sky and fog every frame; cloth copied its setup every frame | translate on all cores (as Unreal Engine 5.5 does), copy the same bytes the fast way, rebuild only what changed, skip identical copies. **+22-25% fps where the CPU is the limit** |
| **Travel: memory** | While you travel, the game handed memory back and asked for it again seconds later, for lighting lists, texture uploads, the graphics card's object list, grass and its own small blocks: 20-100 ms each time | keep the memory it will need again (bounded, and handed back when Windows runs short) |
| **Travel: physics** | When a new area arrived, the physics engine rebuilt its search tree and took in thousands of trees and rocks on one thread while the frame waited | spread rebuilds over frames, build bodies on helper threads, file new entries in batches; collisions find exactly the same things |
| **Travel: characters** | A group of characters far away was created all at once (50-100 ms); crossing into a new row of cells unloaded the old ones and their characters' animation data in one frame | build far, unseen characters a piece per frame; take animation data apart on helper threads; unload the cells behind you one per frame |
| **Threads** | A thread waiting for something could pick up a whole navigation-map tile build while the frame waited for it (up to 2.5 s seen) | leave those builds to free threads (the engine's own flag, and a gap in it closed) |
| **Doors and start-up** | The first frame after a door or fast travel took in all the new area's physics at once; the start-up shader build held loading screens up to 5 s longer for the first minutes | do the physics part behind the loading screen without simulating anything; build shaders at full pace while you are not playing |
| **Menus** | Opening a menu filtered and copied a full-screen picture one pixel at a time; the Graphics page asked Windows for every display mode eight times over | the same picture computed many times faster; the mode list kept and re-checked |
| **Frame generation** | AMD's frame generation work waited in line behind the game's own frames: -20-25% rendered fps with it on | run it on the graphics card's second queue (an AMD plugin setting the game leaves off): **+22-29% rendered fps with frame generation** |
| **Crashes and bugs** | crashes in the game's name table, its trap code, Unreal's video-memory tidying (UE-217692) and AMD's frame generation library; FSR left off after an alt-tab | each guarded so the game carries on as it would have; FSR comes back by itself |

Every fix, its switch and its numbers, in plain words: [docs/how-it-works.md](docs/how-it-works.md). The measurements
and the ideas we tested and rejected: [docs/research.md](docs/research.md).

## Uninstall

Delete `version.dll`, `OblivionPerfFix.ini` and the `OblivionPerfFix*.log` files from
`OblivionRemastered\Binaries\Win64\` (Game Pass: `Binaries\WinGDK\`; OBSE route: your file in `OBSE\Plugins\`).
That's all: the fix changes nothing on disk, and your saves don't depend on it.

## Is it safe?

- **What it does:** every change happens in memory while the game runs; the game's files are never modified.
- **What it doesn't do:** no internet access, no registry, and it writes no files other than its own log next to itself
  (this session's and the last two, at most 8 MB each; plus a stutter report if you switch that diagnostic on). The DLL
  links only to Windows' `kernel32.dll`, and the only other DLL it loads is Windows' own `version.dll`, the one it
  stands in for.
- **Game updates:** if a piece of code isn't exactly what a fix expects, that fix switches itself off and says so in
  the log. The game then runs as if that fix weren't there.
- **Your saves and achievements:** saves are never touched. The fix uses no console commands and changes no game
  data.
- **Check the download:** release files are built by GitHub Actions from the source in this repository, not on anyone's
  PC. Each release lists its SHA-256 checksums and carries a build attestation you can verify:
  `gh attestation verify OblivionPerfFix-<version>.zip --repo LTSharpe/OblivionRemasteredPerformanceFix`.
- **Antivirus warnings:** a DLL named `version.dll` that loads into a game is a common technique for mods, and also for
  malware, so some scanners flag it on reflex. Each release links its VirusTotal scan, and all of the source is here to
  read (heavily commented C++, one file per fix, grouped by area).

## Compatibility

| | Status |
|---|---|
| Steam version, game build 19115871 | tested |
| AMD Ryzen 7 5800X (8 cores / 16 threads), Radeon RX 9070 XT, Windows 11 | tested |
| CPUs with fewer threads | should work. Helper threads scale with the cores Windows gives the game. Below 8 logical processors (for example 4 cores without SMT, or 6 cores / 6 threads) the fixes that lean most on helper threads stay off and the log says so: we could only test with 16 (and with the game limited to 6 of them, where they were not slower than with them off). To try them anyway, add `ParallelMinProcessors=6` under `[Fixes]` in `OblivionPerfFix.ini`. **Please report** |
| CPUs with AVX-512 (Ryzen 7000/9000, Intel 11th gen), older CPUs without AVX2 | the game uses separate code paths there; each is covered by the self-test (the AVX-512 one byte for byte under Intel's CPU emulator). **Please report** |
| NVIDIA and Intel Arc graphics cards, DLSS, XeSS | should work: the fixes are in the game and the engine, not in the driver. The frame generation fixes only act when you use FSR frame generation; `FrameGenAsyncCompute` was measured on an AMD card only, so if FSR frame generation looks or paces worse on another card, set it to 0 and tell us. **Please report** |
| Graphics cards with 8 GB | work; the parallel translation that costs video memory switches itself off on cards under 9.5 GB (the game itself is over its video-memory budget there at 1080p/1440p High) |
| Windows 10 | should work (nothing in the fix needs Windows 11), untested |
| Steam Deck / Proton | untested |
| Xbox / PC Game Pass version | experimental, untested |
| UE4SS, OBSE64 | compatible (OBSE64 can also load the fix) |
| Other mods that install a `version.dll` | conflict: use the OBSE64 route above |

## FAQ

**Does it change graphics or gameplay?** No. Every fix does the same work in a cheaper way, and screenshots with the fix
on and off match within the noise between two identical runs. Physics and AI are untouched. The only timing changes:
far-away characters you can't see are built a few frames later, and the cells you left behind are unloaded a few frames
later (about a tenth of a second).

**Will it break when the game updates?** It shouldn't break the game: anything it can't find exactly, it leaves alone and
logs. A fix that switched itself off needs a new release to come back.

**I don't see a difference.** Then your graphics card is probably the limit where you are. The fix helps most where the
CPU is the limit: towns, crowds, travel, lower resolutions, upscaling, high frame rates, frame generation.

**Does it use more video memory?** A little: about 0.5-1 GB more than the stock game, almost all of it for translating
on all cores (`RdgParallelTranslate`), which is why that one is off on 8 GB cards. `CommandAllocatorTrim` gives back
about 0.4 GB that the game (stock too) keeps after every load, once memory gets tight. If you play on an 8 GB card at
low settings and the `[mem]` line in the log shows plenty of room, you can try `RdgParallelTranslate=1`. Being over the
budget does not crash the game: Windows moves the extra video memory to system RAM, which costs speed.

**The first minute feels slower than later.** For a while after every start the game compiles graphics pipelines in the
background. The fix runs that at full speed while you are in the main menu and loading screens; if you start playing
straight away, the rest of it runs at the game's slower pace during your first minutes.

**Can I see what it did?** Yes: `OblivionPerfFix.log` lists every fix, and every 60 seconds how much work it saved.

**Can I switch a fix off?** Yes, each one has its switch in `OblivionPerfFix.ini` (1 = on, 0 = off). Most can be switched
while the game runs; the file says which.

**My Documents folder is on OneDrive. Which game folder is the real one?** The game keeps its settings and saves in
`C:\Users\<you>\Documents\My Games\Oblivion Remastered\`, even when Windows has moved *Documents* to OneDrive. The copy
in `OneDrive\Documents\My Games\...` is Steam Cloud's copy of your saves; the game doesn't read it. (The fix itself
keeps nothing there: its ini and log are next to `version.dll`.)

**Can I adapt it to my setup, or to a new game patch, with an AI coding agent?** Yes, that's what
[AGENTS.md](AGENTS.md) is for: point Claude Code, Codex or Cursor at this repository and it gets the layout, the build
and test commands and the rules in a few thousand tokens. [docs/porting.md](docs/porting.md) is the step-by-step for a
game update.

## Help improve it

- **Found a problem?** [Open a bug report](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/new/choose)
  and attach `OblivionPerfFix.log`. For stutter, switch on `HitchLedger=1` first and attach `OblivionPerfFix-ledger.csv`
  too: it says where each slow frame's time went.
- **Tried it on your PC?** A [test report](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/new/choose)
  (hardware, did it load, how it felt) helps us get out of beta.
- **Want to measure?** The [benchmark suite](docs/benchmark.md) plays a fixed set of scenes by itself, each from a new
  game (no save needed); one command (`tools\Bench-Report.ps1`) compares the game with and without the fix and writes a
  report you can share. It runs hidden and muted so you can keep working; add `-Watch` to see and hear it at your own
  settings. Two cinematic flights are included, round the **Imperial City** and through **Cheydinhal**
  (`-Scenario showcase`, `-Scenario cheydinhal-show`). It needs the modding tool UE4SS, but only for its own games: it
  takes UE4SS out of the game folder again when it closes the game, and never touches your saves.
- **Build it yourself:** `src\OblivionPerfFix\build.ps1` with the free Visual Studio 2022 Build Tools. `-SelfTest`
  checks every patch site against your installed game without running it. Developers start with
  [AGENTS.md](AGENTS.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits and licence

Researched and built by Claude (Anthropic), directed and tested by [LTSharpe](https://github.com/LTSharpe). MIT licence:
see [LICENSE](LICENSE).

Not affiliated with or endorsed by Bethesda Softworks, ZeniMax, Virtuos, Epic Games or AMD. *The Elder Scrolls*,
*Oblivion*, *Unreal Engine* and *FSR* are trademarks of their owners. The fix contains no game files and no engine source
code. The benchmark suite (not the fix) uses [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) and
[PresentMon](https://github.com/GameTechDev/PresentMon), which its setup script downloads separately. Its UE4SS is
upstream 3.0.1-1161 with a one-line memory-leak fix ([#1446](https://github.com/UE4SS-RE/RE-UE4SS/issues/1446), offered
upstream as [PR #1452](https://github.com/UE4SS-RE/RE-UE4SS/pull/1452)), published as the `bench-tools-v1` release;
the recipe is in [tools/ue4ss-build](tools/ue4ss-build/README.md).
