OblivionPerfFix benchmark kit
=============================

Measures Oblivion Remastered on your own PC, with and without OblivionPerfFix, by itself. You don't need this kit to
use the fix: it is only for checking what the fix does on your PC.

Needs: Windows 10 or 11, Oblivion Remastered on Steam (started once by hand, so the first-start screens are done),
Steam running, an internet connection the first time, about 1 GB of free disk space.

Extract this zip anywhere (for example your Desktop), then double-click one of these:

  Benchmark.cmd         The Imperial City flight, the game without the fix ("stock") and with it, in turn: 4 game
                        starts, about 15-20 minutes. At your own graphics settings, in your own window mode, on
                        screen and with your sound, so you can see and feel the difference. At the end it prints
                        where the report is and opens it.
                        While it runs: don't touch the mouse or keyboard and don't switch windows. A game that is
                        not in front turns frame generation off and gets less time from Windows; the report marks
                        such a run "not comparable". (Mouse and movement keys are ignored during the flights, but
                        Esc or Tab still open the game's menus.)

  Benchmark-Hidden.cmd  The same, but the game runs hidden and muted, so you can keep using the PC (heavy programs
                        make the numbers noisier). Nothing to watch.

  Check-GPU.cmd         Is your graphics card or your CPU the limit? 30 seconds at the small pond by Fort Doublecross
                        in the rain, at your own settings, on screen (about 3-4 minutes with the game start; hidden
                        and muted: "Check-GPU.cmd -Hidden" from a command prompt in this folder). It prints the frame
                        rate, how busy the graphics card and the game's main thread are, and what to change. Change
                        settings in the game as usual (start it from Steam, change them, quit), then run it again.

What happens
- The first time, it downloads two free tools from their own projects on GitHub: PresentMon (Intel, measures frame
  times) and UE4SS (runs the benchmark's small in-game script). They go into third_party\ in this folder. UE4SS is put
  into the game folder only while a benchmark game runs, and taken out again when it closes.
- Benchmark.cmd and Benchmark-Hidden.cmd check that this kit's version of the fix is installed. If another version or
  none is installed, they say so and ask before installing; whatever they replace is backed up in backup\ here.
- Every run starts a new game without a save (your saves and characters are never touched) and puts your settings
  back as they were when the run started.
- "Stock" means the fix's DLL is loaded with every switch off, so the game's own code runs, except the fix for the
  game's frame-cap bug (with frame generation on, the game forgets your saved frame-rate limit after a start): so both
  sides run at your saved frame-rate cap.

The report
- captures\bench\_report\<time>\report.md: average fps, 1% low, worst frame and hitches during the flight, stock vs
  fix. It holds no user name, PC name or folder paths.
- To share it: open a "Benchmark result" issue at
  https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/new/choose
  paste report.md into it and attach the result-*.json files from the same folder.
- Differences under about 5% are noise. More rounds give steadier numbers: run "Benchmark.cmd -Rounds 3" from a
  command prompt in this folder; "Benchmark.cmd -Rounds 1" is a quick look (one game start each, about 8 minutes).

If something goes wrong
- "Oblivion Remastered is running": save, quit the game, run it again.
- Windows may ask "Do you want to run this file?" the first time: the files come from a download. Choose Run.
- Each run's folder under captures\bench\ has a report.md saying what happened. Please attach them to
  a bug report.

To remove it: delete this folder. If it installed the fix and you want your previous files back, copy them from
backup\<time>\ into the game's OblivionRemastered\Binaries\Win64\ folder.

Source, documentation and the full benchmark: https://github.com/LTSharpe/OblivionRemasteredPerformanceFix
(docs/benchmark.md). MIT licence (LICENSE.txt). PresentMon and UE4SS have their own licences (MIT) and are
downloaded from their own projects; this kit does not contain them.
