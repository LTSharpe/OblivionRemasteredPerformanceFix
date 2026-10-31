# Changelog

Downloads, checksums and build attestations: https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/releases

<!-- Each release has a section named after its tag without the "v" (tag v1.0.0-beta -> "## 1.0.0-beta"); the build
     refuses to publish a tag that has no section here, and uses the section as the release notes. Put changes that
     are not released yet under "## Unreleased" and rename it when tagging. -->

Bug fixes say whose bug it was: **game bug** = also happens without OblivionPerfFix (in the game, its engine or a
driver library); **our bug** = caused by an earlier version of OblivionPerfFix.

## Unreleased

- **Game bug fixed: autosaves with every autosave option off** (`AutosaveFollowsMenu`, on): with all four autosave
  options of the Gameplay menu off (rest, wait, travel, entering or leaving an interior) the game still autosaved after
  every level-up and whenever a quest's script asked for a save, two autosaves the menu has no option for. They now
  follow the menu too: while all four options are off, they are not made; with any option on, the game saves as
  before. Checked in play. New key `AutosaveFollowsMenu` (1).
- **Game bug fixed: the real resolution of wide monitors** (`MonitorNativeFromWindows`, `FullscreenAllResolutions`, on):
  monitors wider than 4095 pixels often name a smaller resolution first in their EDID (an Alienware AW3926QW says
  3440x1440 for its 5120x2160), and the game took that as native: borderless showed it, and fullscreen listed only
  resolutions of exactly its shape (3440x1440 alone). The native resolution now comes from Windows' preferred mode, and
  the fullscreen list shows every mode Windows offers. New keys `MonitorNativeFromWindows` (1) and
  `FullscreenAllResolutions` (1). Start-up only.
- **New switch, off by default: bow sneak attacks at 1x** (`BowSneakAttackFix`, game bug): both engines check where a
  player's arrow lands; the new one uses the bow's sneak multiplier, the old one always 1x, and whichever sees the hit
  first wins (in one session of 23 sneak shots: 10 at 2x, 9 at 1x, 4 hit twice). With 1 each arrow hits once, with the
  sneak multiplier (checked in play: 20 of 20 at 2x). Off by default because it changes damage.
- **New trade-off switch, off by default: magenta specks on skin** (`SeparableSkinAbove4K`): with Effects Quality Ultra
  and a render wider than 3840 pixels (an ultrawide at DLAA or FSR Native AA), sunlit skin and cloth edges show glowing
  magenta specks (a game bug, also without this DLL). With 1 skin scattering uses the engine's separable method
  (`r.SSS.Burley.Quality 0`) while the render is that wide; skin looked the same at play distance, about 0.2 ms less GPU
  time at 5120x2160. Live.

## 1.2.3-beta
For Oblivion Remastered, Steam build 19115871 (Game Pass experimental). Thanks to MitochondriaOverlords for issue #5,
to mkusm for pull request #25, to cgisquet for pull request #19, to Hexaae for issue #27, and to everyone who reported
results and problems. Numbers below: our test PC (Ryzen 7 5800X, RX 9070 XT), 4K, FSR Performance, frame generation on,
frame rate capped at 60, unless a line says otherwise; the scenes are public (`bench/scenarios/`).

- **New: horses no longer slow the game's main thread** (`PhysAnimNoEmptyLock`, `RigTraceReuse`, both on; improvements,
  not bug fixes). Near horses (the corral at Brindle Home, west of the Imperial City) the game's main thread was slower
  than anywhere else nearby, for two reasons. Every character, creature and horse has a physical-animation part that the
  game updated every frame under the physics world's exclusive lock although it had nothing to move: that update is now
  skipped when there is nothing to move (the skipped code changes nothing). And a horse checks the ground about 106
  times a frame, 9 in 10 of them exact repeats of a check it made microseconds earlier in the same animation step: such a
  repeat now gets the earlier answer (it could only differ by a split-second race between threads that the game leaves
  to chance; in 2.6 million checked repeats, standing, riding and with a fight among the horses, it never did;
  `RigTraceReuse=2` checks instead of reusing). Together, at the corral (`playlog-spots`, scene `brindle-corral-hold`):
  the main thread's work per frame 10.6 -> 9.2 ms, the time over the 16.7 ms frame budget 87 % lower. Installed at start-up with the version.dll route; with OBSE64 they are not
  available and the log says so. New keys `PhysAnimNoEmptyLock` (1) and `RigTraceReuse` (1; 2 = check only; 0 = off).
- **New: collision checks no longer go through a list of the whole map first** (`ChaosMaxPayloadSize`, 10000000; an
  improvement, not a bug fix). Every collision check (walking, sight lines, horses' ground checks, falling objects) first
  went through about 730 very large shapes spread over the whole map (most of them 1.6 km tall columns), one by one,
  before looking at what is nearby: the engine keeps shapes over 1 km (`p.MaxPayloadSize`, 100000 cm) out of its search
  tree. The fix raises that setting at start-up, so a check tests about 14 shapes instead of 385. Every check finds the
  same objects; only the order in which they are looked at changes, which could settle an exact tie differently (the
  tree's shape already varies in the game). The main thread's tick work per frame at the Skingrad forest (`skingrad-spots`,
  `sk-d`) 9.7 -> 8.5 ms and at Vilverin (`ic-spots`, `ic-vilverin-pass`) 8.8 -> 7.6 ms; the time over 16.7 ms 50 % and
  32 % lower. No added hitch at doors. Both routes, also OBSE64. A `p.MaxPayloadSize` line in your Engine.ini wins. New
  key `ChaosMaxPayloadSize` (10000000; 0 = the game's value).
- **Changed: an area's set-up no longer pulls waiting characters' clothes into one frame** (`NpcCreationPacing`,
  `PacingLeanFlowDrain`, on; pull request #25 by mkusm, thanks; an improvement, not a bug fix): when a newly loaded
  area's set-up has waited its three frames for far-away characters, their creation runs first, as before, but their
  clothes, hair and body steps now stay paced instead of all running in that frame. At the Imperial City Waterfront
  (`ic-spots`, `ic-waterfront-pass`) that frame was shorter in our tests; over whole passes within the noise.
  New key `PacingLeanFlowDrain` (1; 0 = as before).
- **The DLL now carries version information** (issue #27, thanks to Hexaae): Explorer's Properties > Details shows the
  product name, version (1.2.3-beta) and copyright, so you can check which build is installed as `version.dll` or in
  `OBSE\Plugins`. The log's first line says `1.2.3-beta` too.

- **Game bug fixed: a bright flash when opening the menu** (`MenuOpenFlashFix`, on; issue #5, thanks to
  MitochondriaOverlords; happens without OblivionPerfFix too): when you opened the pause menu, inventory or map after a
  while without one, one frame could show a bright, washed-out view of a far-away landscape behind the menu. The menu's
  background is a small scene far outside the world, filmed by its own camera. On the frame the menu opens, the
  engine's visibility check (occlusion culling) still judges that scene from your old camera, and on the next frame it
  hides the scene's walls and backdrop, so the world around it shows through. That one opening frame is now drawn
  without occlusion culling, which is switched back on right after; the frame looks the same (only things hidden behind
  others are drawn and then covered), it is one longer frame at menu open, and the menu opens straight onto its normal
  background. In our tests the wrong frame went from every time to never. Installed at start-up with the version.dll
  route; with OBSE64 it is not available and the log says so. New key `MenuOpenFlashFix` (1; 0 = the game's way).
- **Changed: far characters still invisible in their fade-in count as out of sight** (`NpcCreationPacing`; not a bug
  fix): every character that appears fades in, and for its first part it cannot be seen at all. The pacing builds
  far-away characters a piece per frame only while they are out of sight, and its "seen" test counted a character as
  seen as soon as the engine drew it, even fully faded out (and, behind trees or buildings, only drawn because the
  engine had not yet learnt that it is hidden). Such characters are now treated as out of sight until they become
  visible, so a crowd appearing far away (the worshippers at a shrine on the Bruma night route) no longer builds all at
  once. On that route about 28 % less of the frame-time jitter from building characters (4K, FSR Performance, frame
  generation, RX 9070 XT); no change at the Imperial City Waterfront, and no frame in which a waiting character was
  visible. Hidden key `PacingFadeAware` (1; 0 = the 1.2.2 test).
- **Benchmark: copies of the game outside Steam** (pull request #19 by cgisquet, thanks): with `$env:OBLIVION_GAME_DIR`
  set to a game folder that has no Steam library the benchmark starts the game's exe directly; a Steam install keeps
  its Steam launch (we fixed one detail: Steam keeps `steam_api64.dll` under `Engine\Binaries\ThirdParty\Steamworks\`,
  not next to the exe). PresentMon's permission (administrator or *Performance Log Users*) is now checked before a run,
  with the command that fixes it, instead of showing up later as "PresentMon got no frames".
- **New diagnostic: the tick split** (`HitchLedgerTickSplit`, off): with `HitchLedger=1` it times the game's main-thread
  updates by what they update (characters, horses, their animation and physics parts, timers) and writes them every 2 s
  to `OblivionPerfFix-ticksplit.csv` (about 0.3 MB a minute); `tools/tick_split.py` ranks it. It helped us confirm where
  the horses' time went, and it is there for anyone who wants to find what makes a place slow. Nothing is installed while it is
  0 at start-up; when on, well under a microsecond per update. Only with the version.dll install (with OBSE64 it stays
  off); `tools/tick_split.py` is in the GitHub repository, not in the mod's zips.
- **Benchmark:** three mini-scenarios are public, so the numbers above can be measured again: `ic-spots` (the Imperial
  City flight's hitch spots), `skingrad-spots` (the Skingrad forest's) and `playlog-spots` (the Brindle Home corral and
  three cell crossings).
- **Building from source:** `build.ps1` compiles at below-normal priority and refuses to build while Oblivion
  Remastered is running (a compile next to the game made it stutter in our tests); `-WaitForGame` waits until it is
  closed, `-IgnoreRunningGame` builds anyway.

## 1.2.2-beta
For Oblivion Remastered, Steam build 19115871 (Game Pass experimental). Thanks to mkusm for pull requests #20 and #21,
to luisAzcuaga for pull request #11, to hexaae for the frame-rate report and ScorpioKing for a similar one, and to
everyone who reported results and problems.

- **Our bug fixed (since 1.2.0-beta): a much lower frame rate with `FrameRateLimitAtStart` on** (reported by hexaae with
  NVIDIA DLSS frame generation; thanks also to ScorpioKing for a similar report). With DLSS frame generation the game
  switches NVIDIA Reflex on, and the frame-rate limit we put back then goes to Reflex's own frame limiter instead of the
  game's usual wait: a path we could not test on our AMD PC. The key now acts only with FSR frame generation on AMD and
  Intel graphics cards (the case we measured: the saved limit in effect from the start, as before); with DLSS frame
  generation, and on NVIDIA cards (where the game can turn Reflex on), it leaves the limit to the game, as without the
  mod, and the log says so. No change to the key's name or default.
- **New: fewer GPU spikes where many objects appear at once with ray tracing on** (`RayTracingBuildBatch`, on; an
  improvement, not a bug fix; pull request #21 by mkusm, thanks): the game built each newly loaded object's ray-tracing
  structure on its own, one after another, about a quarter of a millisecond each; where 100-150 arrive together (the
  climb above the Niben on the Imperial City flight) that frame took 30-40 ms longer on the graphics card. They are now
  built together at the start of that same frame, before anything uses them; the picture is the same. Those frames'
  graphics card time on the Imperial City flight: 32-67 -> 19-24 ms (4K, DLSS Performance, RTX 4090); at a Skingrad
  forest spot with 134 new objects 44 -> 15-16 ms (4K, FSR Performance, frame generation, RX 9070 XT). Installed at
  start-up with the version.dll route. During loading screens the builds are made in parts of at most 64 MiB of build
  memory. New key `RayTracingBuildBatch` (1; 0 = off).
- **Game bug fixed: the game stopping in big fights behind a "Havok picks" box** (`HavokPickWarningGuard`, on; happens
  without OblivionPerfFix too; pull request #11 by luisAzcuaga, thanks): in fights with many characters the game's old
  engine can pop up a leftover debug message, "42 Havok picks (6 path, 36 LOS, 0 ViewCaster) used this frame.", on its
  own thread. The whole game then stands still, often behind its own window, until someone finds and closes the box
  (in our own battle tests it stopped 3 of about 14 runs on one day). The box only reports that the engine skipped some sight and path
  checks that frame, which it does with or without the box, so the message now goes to the log instead and the game
  keeps running. Nothing else changes: the engine's limit on those checks (`iMaxPicks` in Oblivion.ini) is left alone.
  New key `HavokPickWarningGuard` (1; 0 = the box as before).
- **Changed: frame pacing with frame generation on the newest AMD drivers** (`FrameGenPacingTuning`; not a bug fix):
  recent Adrenalin drivers (26.10.x) can run frame generation's swap chain, the part that decides when each frame is
  shown, inside the driver. AMD's recommended setting that the fix applies (Tuning Set B) made the shown frames less even
  there in our tests (about twice as many gaps over 25 ms, 40 % more frame-to-frame timing error on the Imperial City
  flight), while the driver's own pacing kept them even. `FrameGenPacingTuning=1` (the default) now sets Set B only on
  AMD's own FSR files' swap chain and leaves the driver's swap chain at its own pacing; the log says which. New value 3 =
  Set B everywhere (what 1 did up to 1.2.1). Only when frames are shown changes, not what is drawn.
- **Benchmark kit:** the report's "Frame cap" and "Stock" lines no longer claim that both sides ran at your saved cap
  (issue #23, thanks to brad1138): with no cap in effect they now say it can be either no limit saved or the game's
  start-up bug, and that `FrameRateLimitAtStart` applies it only with FSR frame generation.
- **Benchmark:** stack timelines are public: `tools/Sample-Timeline.ps1` with its two samplers and the reader
  `tools/Analyze-Hitch.ps1`, so `Run-Benchmark.ps1 -Stacks` works outside our lab (promised in issue #18; how to use it in
  `docs/benchmark.md`). Two new scenarios from pull request #21: `showcase-climb` (the Imperial City flight's climb,
  28 s) and `water-visual` (image check along the Niben).
- **Self-test:** builds with Windows SDKs before 10.0.26100 (10.0.22621 and older), which do not declare
  `ID3D12DeviceRemovedExtendedDataSettings2`; the crash-report check's DRED set-up is then skipped with a `SKIP` line
  and the rest of that check runs as before (pull request #20 by mkusm, thanks). No change to the fix itself.

## 1.2.1-beta
For Oblivion Remastered, Steam build 19115871 (Game Pass experimental). Thanks to luisAzcuaga for pull requests #6-#8,
and to everyone who reported results and problems.

- **New: a switch for the log file** (`[General] LogFile`, 1 = as before; requested in issue #4): `LogFile=0` writes no
  `OblivionPerfFix.log`. Nothing is created, renamed or deleted at start-up, and logs from earlier sessions are left
  alone. The one exception is a GPU crash ("GPU crashed or D3D device removed"): the log is then created and holds just
  the crash report, so a crash still leaves what we need to look into it. Read at start-up only. The log's size caps are
  unchanged (8 MB per file, the last two sessions kept, 24 MB at most).
- **Fixed: game bug, freezes and "GameThread timed out waiting for RenderThread" crashes, mostly after a driver update
  or on a new install, and on 4-core CPUs** (`TaskWaitDeadlockGuard=1`, `PsoCompileThreadPool=-1`, both on; also without
  the mod). (1) The engine's task threads could all end up waiting for wake-up calls that nobody was left to send; after
  2 minutes the game ended itself. It happens with nothing in the driver's shader cache, and on 4-core CPUs also with
  everything cached (the stock game froze at the first teleport). With `TaskWaitDeadlockGuard=1` a waiting task thread is
  also woken by the helper thread that covers for it. (2) With nothing in the driver's cache, every task thread was busy
  preparing graphics pipelines and loading waited behind them: on 6 cores the first teleport froze for 30 s and more, on
  4 cores the game did not get into the world. With `PsoCompileThreadPool=-1` they are prepared on the engine's own
  pipeline thread pool instead (read at start-up only, and used only together with `TaskWaitDeadlockGuard=1`: the pool
  alone can run into the first freeze). Same picture, same play. Not installed when the mod is loaded as an OBSE64
  plugin (too late at that point; the log says so). Tested on AMD (RX 9070 XT) with 4, 6 and 8 cores; untested on
  NVIDIA.
- **New: less waiting for physics when an area unloads** (`PhysicsDestroyBatch`, on; an improvement on the stock game's
  own behaviour, not a bug fix): when the cells behind you unload, the physics step takes up to 2,500 collision bodies
  apart in one frame while the main thread waits. Their leftover objects are now deleted on a helper thread during the
  step, and the physics lists' indexes are rebuilt once instead of after every body; the physics world afterwards is the
  same. About 45 % less time per removed body; on the Imperial City flight at 4K with frame generation and a 60 fps cap,
  the physics part of slow frames is about 30 % smaller (the stone bridge's worst frame 41 -> 32 ms). New key
  `PhysicsDestroyBatch` (3; 0 = off).
- **Our bug fixed: a rare crash after video memory was tidied** (`CommandAllocatorTrim`; caused by 0.17.0 to 1.1.0-beta,
  not by the game): when video memory was tight, the fix that gives spare GPU command memory back could, in a rare
  timing, hand the game a block that was already in use. That could crash the game (an access violation, or a graphics
  error when the game reused the block for a second batch of copy work). Each block's place is now decided before any
  of them is handed back, and if anything goes wrong the trim stays off until the game is restarted. How much memory it
  gives back is unchanged.
- **Our bug fixed: menus lagged for a few minutes after a graphics driver update** (`PsoPrecompileFast`; caused by
  0.30.0 to 1.1.0-beta, not by the game): the fix ran the game's start-up shader build at full speed whenever a menu was
  open, including the inventory, map and dialogue while playing. After a driver update that build takes several
  minutes, so those menus lagged (reported after NVIDIA 621.01). Menus opened while playing now keep the game's own
  pace; start-up, the main menu and loading screens stay fast. New key `PsoPrecompileFastInGameMenus` (0; 1 = as before).
- **Our bug fixed: a slow main menu, and long or frozen loading screens, on a new install or after a graphics driver
  update** (`PsoPrecompileFast`; caused by 0.30.0 to 1.1.0-beta, not by the game): the fast shader build used the engine's
  full fast pace, which made the main menu stutter for its first seconds even normally (frames up to 30 ms and more,
  "700 -> 15 fps" on slower PCs). When the graphics driver has no shader cache yet (a new install, a driver update) the
  driver has to compile everything for real, and pushing it faster only queued work in front of what the game needed:
  the main menu lagged and the first doors took up to a minute or froze. Now the main menu gets a small, fixed share of
  each frame (smooth while the build runs; it still finishes about three times sooner than without the fix), and as
  soon as the build turns out to be slow (an empty driver cache) the game's own pace is kept until it ends, as without
  the fix. New keys `PsoPrecompileFastBudgetMs` (4; 16 = as before) and `PsoPrecompileColdQueue` (1000; 0 = none of these checks, as before).
- **New: robed characters dressed more cheaply** (`ClothRegisterLite`, on; an improvement on the stock game's own
  behaviour, not a bug fix): when a character puts on a robe, tabard or cape, the game built its cloth simulation up to
  three times in a row and threw all but the last build away. The builds that are thrown away are now made without
  their most expensive part (the cloth's constraint rules), and the one that stays gets those rules added from the same
  values, so it ends up exactly as the game builds it. The cloth looks and moves the same. New key `ClothRegisterLite`
  (3; 0 = off).
- **New: a new character's animation rigs set up with less repeated work** (`RigVmHashMemo`, `RigLogicMapCache`, both
  on; an improvement, not a bug fix): each rig worked out the same checksum of its animation program four times, and
  the face rig built every name in its face data as text again before looking it up on the skeleton. The checksum is
  now remembered for exactly the same program, and the face rig reuses the names the game made for the same face data
  (every lookup is still made on the new skeleton). Same results as the game's. New keys `RigVmHashMemo` (1) and
  `RigLogicMapCache` (1; 0 = off).
- **New: a character's physics bodies made under one lock** (`NpcBodiesOneLock`, on; an improvement, not a bug fix): a
  new person's physics bodies and joints (about 37) each took the physics world's lock on their own, and the main thread
  waited behind other threads every time. The lock is now taken once for all of them; the same bodies and joints are
  made in the same order. New key `NpcBodiesOneLock` (1; 0 = off). Together with the two entries above this makes
  creating characters cheaper where many appear at once (towns, shrines, chapels); measured on routes that are not in
  the public benchmark yet, so no numbers here.
- **New: fewer ray tracing freezes when video memory is nearly full** (`RtSceneBufferKeep`, on; an improvement, not a
  bug fix): with ray tracing on, the graphics card's list of the ray-traced scene moved to a smaller block of video
  memory whenever it needed less than half of its block, and to a bigger one again when it grew back, at the same
  places on every ride. At 4K with video memory nearly full, each new block could freeze the game for a fraction of a
  second while Windows made room. The block is now kept when it is big enough (at most the largest size already needed
  in the session); it still grows as before, and the picture is the same. New key `RtSceneBufferKeep` (1; 0 = off).
- **New: a guard against a rare "UObjectArray.cpp:234" crash** (`ObjectReplaceCrashGuard`, on): the game could stop with
  "LowLevelFatalError UObjectArray.cpp:234 Attempting to add ... but another object exists at that index" when its AI
  made a helper object again in the place of an old one with the same name. The guard clears the old object's entry the
  way the engine itself does in every other case, and writes a line to the log; nothing else changes. Most likely a
  **game bug**: it happens in the engine's own code with none of ours involved, but both crashes we know of were on PCs
  with OblivionPerfFix installed, so a cause on our side is not ruled out. New key `ObjectReplaceCrashGuard` (1; 0 = off).
- **Game bug fixed: the saved frame-rate cap did nothing after a start with frame generation on**
  (`FrameRateLimitAtStart`, on; also without the mod): with frame generation on, the Frame Rate Limit saved in the
  Graphics settings was not in effect after the game started. The game ran uncapped although the settings page showed
  the limit, until that page was opened. The fix applies the saved limit once at start-up, as opening the page does;
  later changes in the settings are left to the game. Works on both routes (version.dll and OBSE64). New key
  `FrameRateLimitAtStart` (1; 0 = off).
- **Our bug fixed: a freeze past the Talos bridge** (`NaniteInitialRootPages`, on; in effect caused by 1.0.0-beta to
  1.1.0-beta): the first time a session needs more room for Nanite's detailed geometry, the engine makes a new, bigger
  block of video memory and copies the old one over in one frame. On the Imperial City flight that is always just past
  the Talos bridge. The stock game takes that step too, but the video memory our earlier fixes keep made it wait for
  Windows to make room: at 4K with video memory nearly full it froze the game for up to about 1.7 seconds (the stock
  game's worst frame there: about 0.3 s).
  The block now starts at that bigger size (the engine's own setting `r.Nanite.Streaming.NumInitialRootPages`, 2048 ->
  3072; a line in your own Engine.ini still wins), so the step is never taken. It costs 32 MB of video memory from the
  first area on, the size a session reaches anyway at the bridge. Same picture. Works on both routes. New key
  `NaniteInitialRootPages` (3072; 0 = the game's size).
- **New: shorter render-thread waits near lakes and rivers** (`WaterCaptureFsrSkip`, `WaterCaptureNoRhiFlush`,
  `WaterCaptureNoOcclusionWait`, `WaterCaptureFastMeshScan`, all on; an improvement, not a bug fix): whenever an area
  streams in or out next to a lake or river, the game redraws that water's small "water info" picture with three extra
  renders, and the frame often waited for them. Inside those renders, FSR no longer re-creates its full-screen copy,
  the render thread no longer waits for the graphics driver thread or for visibility results these renders never read,
  and the few water meshes are found from the scene's compact id list instead of by looking through every object. The
  water pictures come out bit for bit the same. On the Imperial City flight at 4K with frame generation and a 60 fps
  cap, the frames that waited for the render thread lost about half of their time over 16.7 ms, and frames of 33 ms or
  more went from 67 / 37 to 37 / 28 per flight. Installed at start-up with the version.dll route (they stay off on the
  OBSE64 route). New keys, each 1 (0 = off).
- **New: navigation pieces fitted together with less searching** (`NavLinkFast`, on; an improvement, not a bug fix):
  when a piece of the navigation map streams in, the game links its tiles to the tiles around it in one frame, and
  looked through every polygon of every neighbouring tile, also where two tiles share no border at all. It now skips
  the pairs that cannot be linked and looks only at the polygons on the shared border; the game's own code makes every
  link, so the links are exactly the same. A small gain on roads where much streams in; measured on routes that are
  not in the public benchmark yet, so no numbers here. Installed at start-up with the version.dll route. New key
  `NavLinkFast` (1; 0 = off).
- **New: shorter save hitches** (`SaveBytesBulk`, `SaveThumbnailAsync`, `SaveRefIdIndex`, on; improvements, not bug
  fixes): a quicksave or autosave no longer stops the game while the save's picture is copied back from the graphics
  card (it is copied in the background and handed to the save a few frames later), the list of saves is written in one
  piece instead of byte by byte, and the old engine's part of the save finds object references through an index instead
  of reading a list from the start each time. Saves hold exactly the same bytes and pictures. With all three, a
  quicksave's frame at 4K with frame generation and a 60 fps cap went from about 110 ms plus a second slow frame to
  about 28 ms. Installed at start-up with the version.dll route (they stay off on the OBSE64 route). New keys, each 1
  (0 = off).
- **Benchmark kit:** a fourth release file, `OblivionPerfFix-<version>-Benchmark.zip`, for checking the fix on your own
  PC without Build Tools, git or Python: extract it, double-click `Benchmark.cmd` (the Imperial City flight straight away,
  stock vs fix, at your own settings, on screen and with your sound so you can see and feel the difference, about 15-20
  minutes, then the report; don't touch the mouse or keyboard or switch windows meanwhile: the report marks runs where
  the game was not in front "not comparable"). `Benchmark-Hidden.cmd` runs the same hidden and muted, to keep using the
  PC. `Check-GPU.cmd` (new scene `gpu-check`: the pond by Fort Doublecross in the rain) says whether your graphics card
  or your CPU is the limit. It installs this version of the fix only after asking, backing up what it replaces and
  keeping your ini values. Built, checksummed, attested and VirusTotal-scanned like the fix zips.
- **UE4SS: the official build.** The benchmark now uses the official UE4SS experimental build 1181 or newer (it has the
  Lua memory-leak fix we contributed upstream, PR #1452), downloaded from the UE4SS project; the report records its build
  and SHA-256 ("tested build" for 1181). Our own UE4SS build (`bench-tools-v1`, `tools/ue4ss-build/`) is retired.
- **Benchmark:** each run can wait at the main menu until the game's start-up shader precompile is done
  (`-MenuPsoWait`), so stock and fix runs start flying in the same state with no warm-up scene. Reports at your own
  settings list them.
- **Benchmark:** `-Profile user` now runs with your own settings as they are (like `own`). The fixed 4K settings it used
  before (Shadows High, the rest Ultra, 60 fps cap before frame generation) are now `-Profile native-cap60`, which the
  README's 4K numbers use.
- Self-test: the `PhysicsInternalFlushParallel` overlap check no longer fails when the freshly created helper thread
  is scheduled more than 5 ms late (2 of 6 runs on a 20-thread PC); the simulated FlushAsync now lasts until the
  helper has started. No change to the fix itself (PR #6).
- Benchmark: `result.json` recorded the previous run's `OblivionPerfFix.ini` when that run's game was still parked
  (a "fix" run's record showed every switch at 0, as in issue #2's attachment); the ini is now read after the passes,
  with this run's settings in place. The measurement itself was right (PR #7).
- Benchmark: `Bench-Report.ps1` left "Fix version" blank when the first run had no fix-log excerpt; it now looks at
  every fix run, then at the installed fix's own log, and says "unknown" otherwise. The log is also found when the fix
  runs as an OBSE64 plugin (`OBSE\Plugins`) (PR #7).
- Benchmark: the profile's ray-tracing switches get 60 s for the game's answer instead of 8 s. The first time a PC
  enabled hardware ray tracing (fresh install, Radeon RX 9070) the game took 11 s to apply it, and the run ended at
  the main menu with "no answer ... within 8000 ms" (PR #7).
- Benchmark: `Bench-Report.ps1` keeps each run's runner output as `runner.log` in the run's folder (it was discarded;
  a warning such as "chapel-door/enter: no arrival detected" could not be traced afterwards) (PR #7).
- Benchmark: the `chapel-door` scene (cheydinhal, full, transitions) never entered the chapel ("no arrival detected",
  90 s arrival in issue #2's report and on two more PCs): from `chapel-square`'s heading the Activate ray met the
  porch column. The scene now starts from its own `chapel-door` spot, the same place turned to face the door. Benchmark
  mod: `flytest doors [radius]` lists the door refs around the player with the heading to each (PR #8).
- README: reordered (Results, Install, What it does, then Check it on your PC), clearer that the fix works on any PC,
  how to find graphics settings your card can carry, and the compatibility table from players' reports.

## 1.1.0-beta
For Oblivion Remastered, Steam build 19115871 (Game Pass experimental). Thanks to everyone who reported results on
NVIDIA, Linux and Mac.

- **Characters fading in no longer use extra video memory** (`NpcFadeInNoRebuild`): each character that appears fades in
  over about a second, and the game rebuilt every piece of it for the graphics card on every frame of that fade. Where
  many appear at once, as on Anvil's docks, that briefly took more than a gigabyte of extra video memory and could
  freeze the game for 2-3 seconds. Now only the fade value is sent. Anvil docks at 4K: video memory peak at a dock exit
  13.0 -> 11.3 GB, no freeze, about three times the frame rate in the seconds after stepping out, +22 % fps walking the
  docks. The fade looks the same.
- **Game bug fixed: stretched robes and capes** (`FrozenClothStretchFix`; happens without OblivionPerfFix too): a
  character's robe, tabard or cape could be left stretched into a long strip back to where they had stood. Frozen cloth now moves fully with its wearer when it
  changes detail level.
- **Our bug fixed: clouds vanishing after lightning** (caused by 1.0.0-beta, not by the game; issue #1, thanks to the
  reporter's video and log from the Great Forest):
  after a lightning flash the sky and the cloud layer could go dark for a while and come back, flickering in and out
  through a storm. Our fix had stopped the game from choosing again, every frame, which light lights the sky, so the
  fading lightning light kept that role. While two lights compete for the sky, the fix now leaves them to the game as
  before. A rare case where old cloud values could reach a newly made sun or moon is fixed too.
- **New download for OBSE64 users:** `OblivionPerfFix-1.1.0-beta-OBSE.zip` puts the fix into `OBSE\Plugins`, ready to
  use, for anyone whose `version.dll` is taken by another mod. No renaming needed.
- Updates now also reach Nexus Mods automatically when a release is published.

## 1.0.0-beta
First public release (beta: tested on one PC so far, an AMD Ryzen 7 5800X with a Radeon RX 9070 XT; reports from other
hardware are very welcome). For Oblivion Remastered, Steam build 19115871; the Game Pass build is supported
experimentally.

- **Rendering:** the frame's drawing work is translated on all CPU cores, the bone upload, weather and cloth updates do
  only the work that changed, and the graphics card's scene data for newly streamed areas is written by all helper
  threads. +22-25% fps where the CPU is the limit.
- **Travel:** memory the game kept handing back and asking for again is kept; the physics engine takes in new areas
  over several frames and on helper threads; far-away characters are built a piece per frame; at a cell-row crossing,
  characters' animation data is taken apart on helper threads and the cells behind you are unloaded one per frame;
  navigation-map builds no longer stall the frame. On the Imperial City flight at 4K with frame generation: 72 -> 102
  fps, hitches of 50 ms or more 19.5 -> 4.8 per flight.
- **Doors and start-up:** the new area's physics is prepared behind the loading screen (the first frame after a door
  about half as long); the start-up shader build runs at full speed while you are in menus.
- **Menus:** menu opens about 0.15-0.3 s quicker at 4K.
- **Frame generation:** +22-29% rendered fps with FSR frame generation on; AMD's ML frame generation (FSR 3.1.4 file,
  Radeon RX 9000) gets the camera data it needs and AMD's recommended pacing.
- **Game bugs fixed (crashes and more):** the game's name table and trap code, Unreal Engine's video-memory tidying
  (UE-217692), AMD's frame generation library; FSR and frame generation come back after an alt-tab.
- **Safety:** memory kept on purpose is handed back when Windows runs short; spare GPU command memory is given back when
  video memory gets tight; parallel translation stays off on graphics cards under 9.5 GB; a GPU crash writes a report
  into the log.
