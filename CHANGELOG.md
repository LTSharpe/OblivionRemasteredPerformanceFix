# Changelog

Downloads, checksums and build attestations: https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/releases

<!-- Each release has a section named after its tag without the "v" (tag v1.0.0-beta -> "## 1.0.0-beta"); the build
     refuses to publish a tag that has no section here, and uses the section as the release notes. Put changes that
     are not released yet under "## Unreleased" and rename it when tagging. -->

Bug fixes say whose bug it was: **game bug** = also happens without OblivionPerfFix (in the game, its engine or a
driver library); **our bug** = caused by an earlier version of OblivionPerfFix.

## 1.4.0-beta
2026-11-11. For Oblivion Remastered, Steam build 19115871 (Game Pass experimental). Numbers marked "ours" are from our
test PC (Ryzen 7 5800X, RX 9070 XT: one PC with an AMD graphics card; nothing here was tested on an NVIDIA card).
Thanks to AngusHades for the log and details in issue #51, and to the player on Nexus whose map mod made us look at what
our map fix hooks.

**In short:** a crash that 1.3.0-beta can cause is fixed (our bug); Lumen's per-frame check is cheaper (on by default;
a first measurement, below); the map fix hooks fewer functions; and on the OBSE64 package the start-up shaders are
built at the game's own pace.
- **Our bug, fixed: a crash when riding or travelling far** (`BodyRemovalSpread`, since 1.3.0-beta; not the game's). On
  1.3.0-beta, if you cannot update yet: `BodyRemovalSpread=0`.
- **New, on by default: `LumenScanCull`.** Switch off: `LumenScanCull=0` (also while the game runs).
- **`MapPopupSpread`** leaves 37 of the map's 83 functions as the game has them. Switch off: `MapPopupSpread=0` and restart.
- **OBSE64 package: `PsoPrecompileFast`** keeps the game's own shader pace (issue #51). The `version.dll` package is unchanged.

**What was tested.** On our test PC only: the exported source builds and passes its self-test against the game's exe;
hidden smoke runs with the shipped ini and with 1.3.0-beta's ini; our crash repro (8 far teleports, each followed by
mounting a horse; `bench/scenarios/east-crash.json`) 8 of 8 clean. Not tested: NVIDIA or Intel cards, the Game Pass
build, the OBSE64 route (OBSE64 is not installed on our PC; the reporter of issue #51 is asked to confirm), Linux/Proton.
This is a beta: please report what you see, with `OblivionPerfFix.log`.

Details, in the order of the list:
- **New: Lumen's per-frame object check reads only what can change** (`LumenScanCull`, on; not a bug: the engine's
  own cost). Every frame, Lumen (the game's global lighting) checked, on the game's helper threads, whether each of
  its 1.5-2 million object groups in the open world should get or lose its surface cards, although almost none can
  change in a given frame; besides the processor time, reading all of them pushed what the other threads needed out of
  the processor's caches. Epic solved this cost in Unreal Engine 5.6.0 (testing a whole block of groups before reading its groups);
  this fix brings that approach to the game's Unreal Engine 5.3 as our own code: blocks of 64 groups that provably
  can't change this frame are skipped, every other block is checked by the game's own code, so the engine gets the
  same requests in the same order: the fix only removes work whose result is unchanged (`LumenScanCull=2` runs both
  ways side by side and logs any difference).
  **What we checked about the picture.** In that check mode (the game's own scan and the fix run side by side, the game
  gets its own result) the two agreed in every one of more than 170 million compared tasks (91 million in one run, 80
  million in another, at a close-up test view). Screenshots of six of our seven test views match the stock game within
  run-to-run variation. At the seventh, a close-up of a gate in shadow, our test screenshots differ between key on and
  key off. We traced this to our test camera, not to the fix: the camera follows the player character, which settles a
  fraction of a centimetre differently after each teleport (0.2-0.5 cm sideways; at that view 1 cm is about 3.6 pixels
  on the near ground), and the differences grow across the picture the way a sideways camera move does; camera rotation
  and field of view were identical in every shot. A fresh key-on run that happened to stand where a key-off run had
  stood matched it as closely as two key-off runs match each other (mean difference 2.6 against 2.1 within one state),
  while it differed from the two earlier key-on runs. We found no difference attributable to the fix, and none was
  noticed in play. We do not claim identical pictures. `LumenScanCull=0` switches the fix off.
  **What we measured** (ours, one PC: Ryzen 7 5800X, RX 9070 XT; a first measurement, and the size of the effect depends
  on the place: the public, save-free benchmark scenario `bravil-road` with profile `native-cap60`: 4K, FSR
  performance, frame generation, 60 fps cap, Lumen hardware ray tracing; one game, the key switched while it runs; one
  run with the key off, three with it on): Lumen's check took 37-44 ms of processor time per frame with the key off and
  3.0-3.7 ms with it on (summed over the helper threads); the helper threads' load went from 4.2 to 2.7 cores; frames of
  18 ms or more were 13.8 % of all frames with the key off and 10.7-11.4 % with it on. Tested on one PC only; with Global Illumination at Low, Lumen does
  not run this check and nothing changes. Installed at game start with the `version.dll` package (not on the OBSE64
  route); `LumenScanCull=0` switches it off (also while the game runs; set it before starting the game to have it not
  installed at all). A game update that changes this part of the engine switches it off by itself, with a line in the
  log.
- **Fixed: our bug, a crash when riding or travelling far** (`BodyRemovalSpread`, on; **our bug since 1.3.0-beta**,
  not the game's; an access violation on the game thread, at `exe+0x487CBE1` in the crash report): the collision bodies
  of an area you had left wait up to 30 frames to be taken down, and in that time the game's own collision checks could
  still find them; when the object they belonged to was already cleaned up, the game's horse and creature physics code
  got a hit without an object and crashed. Waiting bodies are now hidden from the game's collision checks (line traces,
  sweeps, overlaps) from the moment they are queued, so those checks answer as without the mod; the spread and its gain
  stay. Ours, one PC, lab: our crash repro (8 far teleports, each followed by mounting a horse) crashed 1.3.0-beta in
  the second cycle every time; with the fix 8 of 8 cycles clean, with the public build and with the lab build. Workaround
  on 1.3.0-beta: `BodyRemovalSpread=0`.
- **Changed: without `TaskWaitDeadlockGuard` the start-up shaders are built at the game's own pace**
  (`PsoPrecompileFast`; a precaution after issue #51: on the OBSE64 route under Linux/Proton the game froze at the main
  menu about 40 s after launch, while the start-up shaders were being built). `PsoPrecompileFast` hands the engine more
  pipelines to build at once, and the stock engine's task threads can then all end up waiting for each other (the game
  bug `TaskWaitDeadlockGuard` fixes). That guard can only be put in place by the `version.dll` package; loaded as an
  OBSE64 plugin the fix comes too late for it. From this release the faster pace runs only while
  `TaskWaitDeadlockGuard` is installed and on; without it (the OBSE64 package, `TaskWaitDeadlockGuard=0` at start-up,
  or a game build where the guard is not found) the start-up shaders are built exactly as in the game, and the log says
  why. With the `version.dll` package and the default ini nothing changes. To get the game's own pace with the
  `version.dll` package, set `PsoPrecompileFast=0` (works while the game runs) or `TaskWaitDeadlockGuard=0` before
  starting the game. Setting `TaskWaitDeadlockGuard=0` while the game runs does not do it with the default ini: the
  shader thread pool (`PsoCompileThreadPool`, on) needs the guard, so the game refuses the switch, logs that, and keeps
  the guard and the faster pace. Whose bug: not
  confirmed; if the freeze is the suspected one, it is the game's task-thread freeze, made likelier by our faster pace,
  which has run without the guard on the OBSE64 route since 1.1.0-beta (the first OBSE64 package). What OBSE64 players
  lose (ours, one PC, lab, warm shader cache): the build runs in the background until ~110-150 s after launch instead of
  ~40 s, and while it runs the game holds doors and fast-travel loading screens up to 5 s longer, as in the stock game.
  The same on 1.3.0-beta: `PsoPrecompileFast=0`.
- **Map fix, smaller footprint for other map mods** (`MapPopupSpread`, on; a compatibility improvement of ours): after a
  Nexus report that our map hooks get in the way of a minimap mod, we read each of the map functions it wrapped in the
  game's code. The 37 that read and write none of the marker fields (the local map's texture and material loads and
  unloads, the getters of zoom, sizes, DPI and textures, the coordinate helpers) are now left exactly as the game has
  them; 46 of the 83 keep the small step that first finishes the marker work the fix holds back. The marker-list
  function itself is still patched as before. What is gone is the patch on the small stub that Blueprint calls go
  through to reach it (its Blueprint exec thunk; the small step in front of it always runs first, so that patch had
  nothing left to do): nine such patches are left instead of ten. In our lab tests the map functions ran in the same order as
  before, outdoors (world map) and in an interior (local map, with identical pictures). The map opens as fast as before
  (ours, the lab menu test of 1.3.0, small window, later opens: open frame 212 ms before and after this change, 340-345
  ms with the switch off). The ini comment, the README's compatibility table and the
  how-it-works page now say what the fix touches; `MapPopupSpread=0` plus a restart installs none of it.
- Version 1.4.0 in the DLL's version resource and the OBSE64 plugin version.

## 1.3.0-beta
For Oblivion Remastered, Steam build 19115871 (Game Pass experimental). 1.2.5 was announced but never released: its
changes are in this version. Thanks to luisAzcuaga for pull request #45 (issue #44), to mkusm for the code behind
`PacingOwnerWalk` (from their pull request #25), and to everyone who reported results and problems. Numbers marked "ours"
are from our test PC (Ryzen 7 5800X, RX 9070 XT); the contributors' own measurements are in their pull requests.

**Against 1.2.4**, all switches at their defaults (ours: 4K, FSR Performance, frame generation, ray tracing, Ultra with
Shadows High, 60 cap before frame generation; the public save-free flights `showcase` and `bravil-road`, profile
`native-cap60`, 3 fresh games each, 1.2.4 and 1.3.0 taken in turn; frame times from the fix's own `HitchLedger` with
`HitchLedgerMs=1`, read with `tools/stutter_ledger.py`): Imperial City flight frames of 33 ms or more 23 -> 8 per
flight, time over 16.7 ms -14 % (7.5 -> 6.5 s per flight), 99th percentile frame 27.7 -> 26.3 ms; road to Bravil time
over 16.7 ms -30 % (4.6 -> 3.2 s per run), 99th percentile frame 21.4 -> 20.0 ms (its frames of 33 ms or more are too
few to compare: 2.0 and 1.7 per run). The numbers below are each for that switch alone; each says where it comes from:
a public scenario of the benchmark (re-measurable), a lab measurement (a route or tool not in the public benchmark),
or play.

**In short:** fewer stutters while travelling (land loading and unloading around you, route searches, water, the
graphics driver's command memory), less main-thread work when characters appear and in big fights, faster menus and
loading screens, one rare crash fixed, and menus no longer slower after every fast travel. Every new fix is on by
default, and setting it to 0 in `OblivionPerfFix.ini` gives 1.2.4's behaviour (most of them live, while the game runs).
Most do the same work more cheaply with the same result. These are **equivalent, not identical** (the same result a
little later or reached another way; each paragraph says how): `BodyRemovalSpread`, `StreamRegisterGranularity`,
`WaterInfoSpread`, `CellBufferEvictDelayMs`, `RigImportTemplate`, `CreatePreload`, `PacingOwnerWalk`, `OtFrameTrim`,
`WaterInfoSkip`, `DrawBehindLoadingScreen`, `MapPopupSpread` and, in one detail, `GpuMenuShot`. `FarPawnMoveHalfRate`
and `FarHorseMove` are **not identical**: far-away calm characters and horses move in 30 steps a second. If you keep
your own ini from an earlier version, the new switches take their defaults (on).

### Performance improvements

#### Travelling
- **New: on AMD graphics cards with 16 GB of video memory or more, fewer stutters when a busy moment needs new command
  memory** (`CmdMemPregrow`, on by default; a performance improvement, not a bug fix): AMD's driver gives each of the
  game's command allocators its memory on first use, and when the game's stock of them runs out at a busy moment, tens
  of new ones take their memory in one go and the frame waits on the driver (the stock game too). Now the stock is made
  once before the first area is up, on its own thread, in about 3 s: lists are recorded and never sent to the card, so
  the picture can't change, and no game logic runs. Ours, on one PC (RX 9070 XT, 4K, FSR Performance, frame generation,
  60 cap), fresh games, scripted flights: Imperial City bridge slice (lab: a cut of the public `showcase` flight; 6 runs
  each) frames >= 33 ms 21 -> 5, ms over 16.7 per run 607 -> 437; road to Bravil (the public `bravil-road`; 3 runs each)
  frames >= 33 ms 13 -> 1, ms over 16.7 per run 789 -> 524. Other stutters stay, and one bridge run with it on had a 133
  ms game-thread frame we can't explain (no driver wait in it). Costs: up to about 0.8 GB of video memory from the start
  of the session (ours: the session's peak 0.2-0.3 GB higher in those flights); the shader precompile in the background
  finishes about 6 s later, with no slower frames. **The risk:** that memory is taken early, nothing at the start can
  tell that another program will fill the card later, and going over the video memory Windows gives the game is what
  came before the rare graphics driver hangs on our test PC. If the game gets within 0.5 GB of that memory for 3 s, it
  hands back once what the session has not used so far (ours, lab: about 0.4 GB, no slow frame while it does in a test
  that only pretends the card is full; a really full card is untested); a card that fills up quickly can get there
  first, and a busy moment right after it stutters more than in the stock game. Not tested on any other PC. It runs only
  on an AMD card with its own memory and at least 16 GB of it (no handhelds, no Wine or Proton); it stays off, with a
  log line saying why, on other cards, without `RdgParallelTranslate`, under 6 GB of RAM or 4 GB of commit free, or once
  the first area is up, and it stops if memory gets short. Smaller cards may gain too, but going over their video memory
  would cost more, and we have not tried them. If another program fills your card while you play, or you get graphics
  driver hangs, set `CmdMemPregrow=0`: as in 1.2.4.

- **New: fewer red frames when the area behind you unloads** (`BodyRemovalSpread`, on; a performance improvement, not a
  bug fix; the engine's behaviour, also without the mod): when you ride away from a stretch of land, the game removes
  its rock and plant fields' invisible collision bodies (7,000-26,000 at one place on the Bravil and Kvatch roads) in a
  few frames, and the physics step then takes them apart while the main thread waits. Now they are taken down over the
  next frames instead, with the game's own code, at the end of a frame: at least 500 a frame, every one within 30 frames
  (half a second at 60 fps); only those at least 50 m from you, only while you play (loading screens as before). Ours,
  one PC, lab measurement (the Kvatch road from a play session and cuts of `bravil-road`, not in the public benchmark),
  scripted rides at low resolution, the moments of removal only: Kvatch road frames >= 30 ms about 2 per pass ->
  0, worst 35-36 -> 20-24 ms, ms over 16.7 -62..-88 %; Bravil road places 72 -> 15-33, 69 -> 27-41 and 30 -> 12-16 ms
  over 16.7; after a teleport no worse than the game. **Equivalent, not identical:** nothing you see changes, but for up
  to half a second those far, out-of-reach bodies are still in the physics world (something far away could still touch
  one in that time). Costs: none we could measure (the same work, ~1 us of main thread per body, spread out). Needs
  `NpcCreationPacing`. `BodyRemovalSpread=0`: the game's way (live).

- **New: fewer red frames when a wooded slope or rock field streams in** (`StreamRegisterGranularity`, on with 1; a
  performance improvement, not a bug fix; the engine's behaviour, also without the mod): the engine adds a new area's
  objects a few ms per frame but checks the time only after every 10 objects, and 4-5 of those can be grass or rock
  fields of ~1,000 collision bodies each: one 9-14 ms piece of work, then a slow physics step. Now it checks after every
  object (the engine's own `s.LevelStreamingComponentsRegistrationGranularity` 10 -> 1, while you play; loading screens
  keep the game's 10). Ours, one PC, lab measurement (the Kvatch road from a play session, not in the public benchmark),
  its arrivals: frames >= 30 ms 8 -> 0, ms over 16.7 -42 % at your kind of settings and -50 % at low resolution; door
  loading screens as before. **Equivalent, not identical:** the same objects in the same order, but a newly streamed-in
  cell appears about 0.1-0.3 s later than in the game (what the game itself shows at about 40 fps). If your Engine.ini
  or the console sets that variable, yours is kept. `StreamRegisterGranularity=0`: the game's 10 (live).

- **New: no route-search spikes when the navigation data changes around you** (`RepathParallel`, on; a performance
  improvement, not a bug fix; the engine's behaviour, also without the mod): when cells stream in on a road, the engine
  searches the routes of every walking character again, one after another on the game thread (from play: 29-34 ms in one
  frame near Skingrad in a ride). Now those searches run at the start of that frame on four helper threads with the
  engine's own search, and a result is handed over only when every input of the engine's own search is byte for byte the
  same, every door, ladder or jump link the helper's search asked about still gives the same answer, and nothing in the
  navigation data changed in between; otherwise the game searches as before. **Identical:** the same routes (lab proof
  build: 27 of 27 compared results identical, 13 of them searches through door, ladder or jump links; 0 mismatches; two
  play sessions: 47 and 38 searches served). Ours, one PC, from play (not re-measurable with the benchmark): the
  Skingrad bursts 29-34 ms -> 8-9 ms. Costs four idle helper threads; off below 8 usable logical processors
  (`ParallelMinProcessors`). `RepathParallel=0`: as in 1.2.4 (live once installed; installed at start-up).

- **New: no hitch from Lumen's object list in the first outdoor area after starting the game** (`LumenGroupsPrior`, part
  of `LumenKeepArrayCapacity`, on; a performance improvement, not a bug fix): Lumen's list of every tree and grass
  instance (1 to 2.4 million entries outdoors) grew in steps, each a copy of the whole list while the frame waited. The
  stock game makes these copies too, more of them in smaller steps (+37.5 % each, and again after every shrink);
  `LumenKeepArrayCapacity` (since 0.12.0) made them one series of doublings per session, but the last doublings in the
  first big outdoor area still copied 50-100 MB at once (20-63 ms, the worst frame of our Imperial City flight, under
  the stone bridge; rarely a second one much later in a denser area). Now the list gets room for 2.6 million entries in
  one small step as soon as it holds 1/32 of that. Ours, the public `showcase` flight, 8 fresh games at 4K with frame
  generation: that frame 33-86 ms (mean 57) -> 30-42 ms (mean 36). Same entries, same picture. Cost: 198 MiB counted
  against Windows' commit (RAM plus page file) from the first outdoor area on, taking RAM only as the list fills
  (outdoors it ends near that size anyway); it is only done while Windows has that much commit available and 2 GB more
  to spare, never while `MemoryRelief` is giving memory back, and never on graphics that share system RAM.
  `LumenGroupsPrior=0`: as in 1.2.4.

- **New: on graphics cards with 16 GB or more, one fewer driver wait on a first ride** (`InstanceBufferFloor`, on; a
  performance improvement, not a bug fix; the engine's behaviour, also without the mod): the graphics card's list of
  object instances grows from 96 to 192 MB once in most sessions, and making that block waited on the driver mid-ride
  (lab traces: median about 5 ms, up to 24 ms; one 14.7 ms render-thread wait in a ride trace from play). Now the first
  time it needs 96 MB (mostly while a load ends) it is made 192 MB at once. The same picture. Small: once per session at
  most. Costs up to 96 MB of video memory in a session that would have stayed at 96 MB. Only when Windows' video memory
  budget for the game is at least 14000 MB with 1 GB free at that moment (decided once per session); needs
  `GpuSceneKeepCapacity`. `InstanceBufferFloor=0`: as in 1.2.4 (start-up).

- **New: water redraws while you travel are spread over frames** (`WaterInfoSpread=3`, `WaterInfoSpreadNearM=-1`, on; a
  performance improvement, not a bug fix; the game's behaviour, also without the mod): when areas stream in or out the
  game redraws the water picture of the lakes and rivers they touch, often several in the same frame or a few frames
  apart (5 ms or more of the render thread each at 4K, lab traces). Now at most one is redrawn per frame, oldest first,
  and a water redrawn less than a quarter of a second ago waits out that time so the requests in between become one
  redraw. Every redraw still happens, with the ground and water of the moment it is drawn, at most 4 frames or 250 ms
  after it was asked for. **Equivalent, not identical:** a lake's water picture can follow a change of the ground around
  it that much later. Ours, one PC, lab measurements: in a scripted bridge slice (a cut of `showcase`) the render-thread
  excess from water updates went down 28 % with peaks unchanged within noise; at cuts of `bravil-road` at the spots the
  maintainer marked in play no difference was measurable; from play, in the maintainer's own rides the longest wait was
  15 frames / 249 ms. On by the maintainer's choice although the gain is small. `WaterInfoSpread=0`: as in 1.2.4 (live
  once installed; installed at start-up).

- **New: the cells behind you are unloaded a little later after a crossing** (`CellBufferEvictDelayMs=1500`, on; part of
  `CellBufferPacedEvict`; a performance improvement, not a bug fix): `CellBufferPacedEvict` unloads the cells that fall
  out of the old engine's cell buffer one per frame right after a crossing, when the game is busiest bringing in the new
  row. Now that starts 1.5 seconds after the crossing. **Equivalent:** the same cells, in the same order, on the same
  thread; until then the cells about 150 m behind you stay loaded, as with a bigger `uExterior Cell Buffer`. A second
  crossing within those 1.5 s (moving diagonally near a cell corner) can make the game unload some of them inside that
  crossing frame, as the game always does; the log counts it. Ours, lab analysis of play rides: the cell unload is about
  1.5-2 % of a ride's time over 16.7 ms; no frame-level gain was measurable. On by the maintainer's choice.
  `CellBufferEvictDelayMs=0`: as in 1.2.4 (live).

#### Characters and crowds
- **New: less main-thread work when characters appear** (`RigImportTemplate`, on; a performance improvement, not a bug
  fix): each new character's, creature's or horse's Control Rig (the animation rig that, among other things, places the
  feet) first builds its bone list from the skeleton, one bone at a time: 0.75-1.5 ms per rig (lab timings). Now the
  first rig of each kind with a given skeleton is built by the game as before and kept as a template; later rigs of that
  kind with exactly the same skeleton (bone names, parents and reference pose compared) get a copy of it in about 0.2
  ms, and only while the same listeners are attached as when the template was made. **Equivalent, not identical:** a lab
  build compared the copy with the game's own build over the whole bone data: 33 of 33 rigs the same, apart from how the
  memory is laid out (a copy's bones sit in one block instead of one block each). Ours, one PC, the public `ic-spots`
  Waterfront pass with lab counters: the main-thread time of character creation -1.0 to -1.1 ms (-14 %; 8 fresh passes,
  ABBA). **Whose bug the guard prevents: ours.** Because a copy's bones share one block, removing the first bone of a
  copied rig later would free the block under the bones still in use (the game's own build allocates each bone alone, so
  without our copy there is no such block). The game never did that in our tests (0 times in all our lab sessions, up to
  3,752 later re-imports per session); if it ever does, that block is kept instead of freed (about 0.3 MB at most per
  rig; after 8 such blocks the copies switch off until the game restarts) and the log counts it. The stock engine has
  the same latent weakness for rigs that copy their hierarchy themselves (an engine bug that also exists without the
  mod, not seen to fire); the fix leaves those as they are and only counts them (`stock batch-owner removes`). Costs:
  one stored copy per kind of rig and skeleton (at most 64, a few hundred KB each). `RigImportTemplate=0`: as in 1.2.4
  (live once installed; installed at start-up).

- **New: far-away calm characters and horses move in 30 steps a second** (`FarPawnMoveHalfRate=1`, `FarPawnMoveHz=30`,
  `FarHorseMove=2`, on; a performance improvement, not a bug fix). **Not identical to the game:** a calm character or
  horse at least 100 m from you (walking, wandering, travelling, eating, sleeping, waiting; at walking pace) runs its
  movement update every second frame at 60 fps, with both frames' time in one step: what every character gets on a PC
  that runs the game at 30 fps. Far away such characters move in small steps; routes, speeds and arrival points stay the
  same. Everyone near you, and anyone fighting, fleeing, following, talking, swimming, running, mounted, or playing an
  animation that moves the character (root motion), moves every frame as before. Inputs added in the skipped frame reach
  the next step as their average, and a character that stops qualifying (comes near, starts a fight) moves at once with
  all its waiting time. Ours, one PC, lab routes not in the public benchmark: the movement update of far calm characters
  on a gallop route 64 -> 52 us per character and frame (-0.17 ms per frame in total); a first held-game pair on a
  walking route with many far wanderers: game-thread ms 12.75 -> 11.82 (-7 %, one pair, small profile). Also: a far
  character standing on or pushing a loose physics object pushes it with about half the force. `FarPawnMoveHalfRate=0`
  and `FarHorseMove=0`: as in 1.2.4 (live once installed; installed at start-up when either is on; needs
  `NpcCreationPacing`).

- **New: no hitch when the first horse, creature or item of its kind is created far away** (`CreatePreload`,
  `CreatePreloadNear=12`, on; a performance improvement, not a bug fix): creating a character, creature or object whose
  model or kind is not loaded yet (the first horse, rat or skeleton of a trip, a book next to a ruin, or one the game
  has unloaded since) made the main thread wait for the disk: 4-72 ms for a horse at 4K, up to 160 ms next to a ruin
  (lab traces). A far one that `NpcCreationPacing` may let wait now waits at the head of the queue while the same files
  load in the background, then is created as before. **Equivalent, not identical:** the same files, loaded by the same
  loader, a few frames earlier; the character is created a few frames later, far away, in the same order; at most as
  long as `PacingMaxLagFrames` (then the game loads the rest itself, as in 1.2.4). Ours, one PC, lab routes not in the
  public benchmark, 4K traversal (ABBA): the synchronous loads inside creates 4 / 42 ms -> 0 / 0, no create over 25 ms
  (about one frame of 33 ms or more less per pass by the Chorrol stable); cold Imperial City Waterfront (480p): 39 ms ->
  0. `CreatePreloadNear`: a character created near you may also wait, at most 12 frames, while its spot is more than 75
  degrees off the camera's view direction (a fixed angle, not tied to the field of view); created at once when it comes
  into view; only with frame generation (it needs its camera). Ours, lab: the Crucible dog's create 40-47 ms -> 6.6-8.1
  ms. `CreatePreload=0`: as in 1.2.4 (live once installed; installed at start-up; needs `NpcCreationPacing`).

- **New: far characters' equipment and hair wait their turn** (`PacingOwnerWalk`, on; part of `NpcCreationPacing`; pull
  request #25 by mkusm, thank you): equipment and hair steps of a character far away and out of view now wait their turn
  like its other steps; before, a read fault on a small object in the owner chain made them count as near (so they ran
  at once, as in the stock game: a gap in our pacing, not a game bug). mkusm wrote it in the first version of pull
  request #25 and later left it out after their NVIDIA runs showed no measurable gain; we kept it after testing it on AMD
  (lab counters on the public `ic-spots` Waterfront pass: equipment steps with an unknown owner 12 -> 2 per pass, now
  paced; whole passes within our noise). Timing only, in the game's order. `PacingOwnerWalk=0`: as in 1.2.4 (live).

#### Fights
- **New: less waiting for the old engine in big fights** (`OtFrameTrim=3`, on; a performance improvement, not a bug
  fix): in a big fight the main thread waits each frame for the old engine's own thread. Bit 1: that thread started each
  frame with a 1-2 ms sleep so its path finder could run; the sleep now ends as soon as the path finder is done (or at
  once when it has nothing to do), never later than before, and the old engine sees the same result (**equivalent**:
  thread timing only). Bit 2: a timer the old engine reads only for its own profiling (66 calls, nothing reads the
  result: checked in the code and with hardware watches in fights; not covered: menus, saving and loading, the console)
  is no longer read (**identical** by that audit). Ours, one PC, the public `actors` scenario's 36-character fight
  (`actors-battle`) at 4K with frame generation, 3 fresh games, 6 ABBA pairs: -1.2 ms per rendered frame (the main
  thread's wait for the old engine 6.6 -> 4.9 ms), about -5 % of the time over 16.7 ms. `OtFrameTrim=0`: as in 1.2.4
  (live once installed; installed at start-up).

- **New: no disk checks for missing voice files in fights** (`LooseMissCache`, on; a performance improvement, not a bug
  fix): every spoken combat line made the old engine check twice whether a loose voice file exists on disk (it never
  does: the voices are in the game's archives), on the thread the game waits for. A voice path found missing once is now
  answered from memory as missing. **Identical** while no loose voice file appears under `Data\Sound\Voice` while the
  game runs; anything else, and anything found, is checked as before. Ours: ~0.24 ms of that thread per rendered frame
  in the `actors-battle` fight (lab timers; too small to show in whole-frame numbers). Costs: the remembered paths (a
  few thousand, at most 65,536). `LooseMissCache=0`: as in 1.2.4 (live once installed; installed at start-up).

#### Menus and loading screens
- **New: no water redraws when you open or close a menu** (`WaterInfoSkip`, on; a performance improvement, not a bug
  fix; the game's behaviour, also without the mod): opening or closing the map, the inventory or saving makes the game
  redraw the water picture of every loaded lake and river (8 at once, 20-40 ms of the render thread, lab traces) with
  nothing changed. Now a redraw whose inputs (the water, the ground pieces, their positions and sizes, the picture's
  height) are exactly those of that water's last redraw, and whose inputs last changed at least 2 seconds ago, is
  skipped. **Equivalent, not identical:** in our checks the skipped redraws were bit for bit the same picture (32 of 32
  after map opens, map closes and saves; 144 of 144 in three test sessions), but content the inputs can't show (finer
  ground detail or a river's own data arriving after its last redraw) is refreshed at the water's next real change, not
  at the next menu as in the game. One such case was seen in testing: right after a teleport a zone redrew with the same
  inputs 0.15 s later and 138 of its 65,536 pixels differed; the 2-second wait keeps such redraws. Ours, one PC, lab
  menu test (not in the public benchmark), small profile: the 26-40 ms frame after closing the map is gone (7 of 9 below
  18 ms). It does nothing for riding: those redraws really change the picture. `WaterInfoSkip=0`: as in 1.2.4 (live once
  installed; installed at start-up).

- **New: the area you arrive in appears sooner after a loading screen** (`DrawBehindLoadingScreen=3`, on; a performance
  improvement, not a bug fix; the game's behaviour, also without the mod): after a door, gate, fast travel or save load
  the game draws nothing of the new area until the loading screen fades out, so its first frame does all the first-time
  work at once while the loading picture stays on screen. Now, once the area has loaded, the frozen area is drawn for 3
  frames behind the opaque loading screen (nothing moves or runs; only drawing), then hidden again until the game's own
  fade-in. Ours, one PC, lab door and fast-travel tests (not in the public benchmark): 55-72 % of what was left after a
  load removed; the loading picture went away 76 ms (small window) to 350-500 ms (4K) sooner on average. **Equivalent:**
  the first visible frame builds on those frames' image history instead of starting fresh. Cost: rarely the loading
  screen stays up one more half second (1 of 28 test arrivals at 4K). Needs `StreamBehindLoadingScreen`,
  `PrepareCollisionBehindLoadingScreen` and `NpcCreationPacing`. `DrawBehindLoadingScreen=0`: as in 1.2.4 (live).

- **New: the map opens faster** (`MapPopupSpread=5`, on; a performance improvement, not a bug fix; the game's behaviour,
  also without the mod): opening the map, the game builds all its markers (a few hundred small widgets) three to five
  times in the same frame, each time removing the ones it built before anything has drawn or looked at them, and builds
  them again from an equal list in the next frames. Now the lists removed unseen are not built, and an equal rebuild
  keeps the markers already there with the new list's data. **Equivalent:** the markers shown come from the same final
  list; any marker that is shown, hovered or looked up is built first; every map open starts fresh. Ours, one PC, a lab
  menu test at Cheydinhal's chapel square (the map opened by a lab script, not in the public benchmark), small window
  (854x480), this release's DLL: later map opens 338 -> 207 ms for the open frame and 179 -> 61 ms for the next (the
  time over 16.7 ms around an open 549 -> 279 ms, -49 %); a session's first map open 413 -> 297 ms (one launch each).
  `MapPopupSpread=0`: as in 1.2.4 (live once installed; installed at start-up).

- **New: menus open without waiting for the screen picture** (`GpuMenuShot`, on; a performance improvement, not a bug
  fix): the picture behind an open menu (inventory, magic, map, pause, character) is a filtered copy of the screen, and
  even with `ScreenshotReadbackConvert` and `ScreenshotParallel` the game stopped until that copy came back from the
  graphics card. Now the same picture is made on the graphics card itself, in the same frame, with the game's own colour
  conversion and filter. **Identical:** the same bytes (every byte compared in our lab tests, SDR). Ours, one PC (RX
  9070 XT), 4K, a lab menu test (the menus opened by a lab script, not in the public benchmark): inventory, magic, pause
  and character open frames 42-56 ms (31-45 %) shorter. Costs: two screen-sized buffers in video memory from the first
  menu on (about 63 MB at 4K). Tested on AMD only; any picture it can't make (another screenshot in flight, an unusual
  screen format) is left to the game. It also keeps working after fast travels: a save made while a menu is open (the
  fast-travel autosave, a save from the System menu) leaves its thumbnail handler bound to the menus' screenshot for the
  rest of the session (a game bug, also without the mod; `SaveThumbLeftoverFix` below removes them, so they remain only
  with `SaveThumbLeftoverFix=0` or with the OBSE package). Those handlers only copy each menu picture into the finished
  save's buffer, which nothing reads before the next save resets it; they now get the same picture a few frames later
  (or, if that picture is dropped, keep the older one): **equivalent** for this part, not identical. While a save is in
  progress the shot is left to the game. `GpuMenuShot=0`: as in 1.2.4 (live once installed; installed at start-up).

#### Rendering
- **New: less render-thread work when Lumen's object table changes** (`LumenCardsNarrow`, on; a performance improvement,
  not a bug fix; the engine's behaviour, also without the mod): when one instance of a big object (a tree of a forest
  object) gets or loses its Lumen surface cards, the engine sends the Lumen table entries of every instance of that
  object to the graphics card again. Now only the changed entries are sent; the table on the card holds the same values
  in the same frame. **Identical:** checked against the game's own values in a lab proof build, 0 differences in 272.5
  million instances. Ours, one PC, a 37 s lab cut of `bravil-road` (the cut is not in the public benchmark): the step
  took 0.85 ms per rendered frame before and 0.08 ms after (about 0.8 ms of the render thread a frame; 0.23 ms on a
  second pass of the same road); at a 60 fps cap the frame times don't show it on their own. Costs up to about 35 MB of
  memory (bookkeeping, used only as far as the table goes). `LumenCardsNarrow=0`: as in 1.2.4 (live once installed;
  installed at start-up).

### Game bug fixes
- **A rare crash when arriving somewhere new** (`PairReleaseOnce`, on; a game bug that our `NpcCreationPacing` exposes):
  the game hands each buffer of its old engine's "pairing" sessions back twice, which is harmless on its own; but when
  another session takes the buffer in between, the second hand-back frees it under that session and two sessions share
  one buffer (the crash at `exe+0x4590F5C`). The double release is the game's own code; in our tests it only collided
  with our NPC pacing on (a lab stress test: 10 of 44 widened teardowns with it on, 0 of 44 with it off), and we have
  not seen it without the mod. Now each buffer is handed back once, by the game's own last release; the log counts the
  skipped early ones (`[pairrace] PairReleaseOnce: N early releases skipped`). Nothing else changes.
  `PairReleaseOnce=0`: as in 1.2.4 (start-up). What it does not do: our pacing still lets the end of a pairing session
  overlap with the old engine starting another one; a second, unlocked look-up of the session table in that overlap
  could in theory still fail (not seen in any test or play session; a review finding we are still working on).
- **Game bug fixed: menus open slower after every fast travel** (`SaveThumbLeftoverFix`, on; a game bug, also without
  the mod): a save made while a menu is open (the fast-travel autosave, a save from the System menu) attaches its
  thumbnail handler to the menus' screenshot; the game meant to remove it once the save had its picture, but removes it
  from the wrong list, so it stays until you quit, one more per such save. Every later menu then copies its background
  picture once more per leftover handler (our estimate from timing a 33 MB copy on one PC: about 1.5-3.5 ms each at 4K,
  so after ten fast travels a menu would open 15-35 ms slower). Now each handler is removed once its save has the picture, as the game intended. Saves,
  thumbnails and gameplay stay the same. `SaveThumbLeftoverFix=0`: as in 1.2.4 (installed at start-up, then live).

### Other
- **Writing the log no longer holds up the game** (our bug, in every version since 1.0.0-beta; not a game bug): each
  line of `OblivionPerfFix.log` was written to disk by the thread that logged it, so a slow disk write (Windows
  sometimes takes tens of ms) stopped that thread, which can be the game's own; in our test build, which logs a lot
  more, one such write made a 50 ms frame. Lines are now handed to a background thread that writes them in order: the
  same lines, order and time stamps. At a crash, and when the game ends, every waiting line is written first, so crash
  reports stay complete. `LogFile=0` is unchanged.

- **The fix no longer runs without a log and without saying so** (our bug, in every version since 1.0.0-beta): if
  `OblivionPerfFix.log` could not be opened (a game process that did not fully close still holding it, a folder the game
  may not write to), nothing was logged at all while the fix kept running, and the README's "no log means the fix was
  not loaded" was wrong for that case. The log now falls back to `OblivionPerfFix.<process id>.log` in the same folder,
  then to the same name in `%TEMP%`; its first lines say which file failed and why. These are new file names you may
  see, only when the normal log fails. At most three stay in each folder (the new one and the two newest others; older
  ones are deleted when a new one is created); a normal start touches none of them. `LogFile=0` still writes nothing,
  except a GPU crash report as before, which takes the same fallback.

- **The log now says when the game's exe is not a build we know** (not a bug; a message that was missing; issue #43):
  one plain start-up line, `game build not recognised`, with the exe's TimeDateStamp and SizeOfImage, saying that fixes
  that can't find their code stay off, which is safe. The line switches nothing off.

- **A fuller report when the game dies with "GPU crashed or D3D device removed"** (diagnostics, not a bug fix;
  `GpuCrashReport`, on): besides what each graphics queue had reached (now per queue, with the passes still open and the
  memory page a fault hit), the log holds the last minute of memory use and frame times, one line per second (video
  memory against its budget, free RAM and commit, the game thread's longest frame and the GPU's frame time), the
  settings of this fix in effect and where in the world the player was (coordinates). The report names no other program.
  Nothing is drawn or paced differently. Please include that part of the log in a crash report. One new key under
  `[General]`:
  - `GpuPassMarkers` (**0**; 1 = pass names in the crash report, 2 = AMD's own markers for Radeon GPU Detective and
    Profiler, 3 = both; AMD graphics cards only, nothing changes elsewhere): names the render pass the GPU stopped in.
    It costs a little CPU time on the threads that record the GPU's work (about 0.25-0.5 ms per presented frame on the
    render hardware thread in our lab test with frame generation on, only while on), and draws nothing differently. For
    people who are asked to switch it on when hunting a crash.

- **Save pictures can no longer wait forever on a crashed graphics card** (our own `SaveThumbnailAsync` code, not a game
  bug; not seen in play): the helper's wait for the graphics card's copy of the save picture and the screenshot
  broadcast's wait for the helper stop after 10 seconds (the picture is then black, a line is logged) instead of never,
  so a crash cannot turn into a freeze there. No change while the graphics card works.

- **A narrower safety window when the game quits** (`CommandAllocatorTrim`; our bug, latent: not seen in any log): the
  once-a-second trim of the copy queue's spare command memory runs only while the game has asked for that memory
  recently. "Recently" was 2 seconds, but when the game quits it deletes its graphics device 0.84-1.22 s after its last
  request (ours, lab, 6 quits), so a trim in that gap could have touched freed memory (it also needs video memory to be
  tight at that moment). The window is now 250 ms. The game asks every frame, so play is unaffected; a trim that misses
  the window waits for the next second.

- **Benchmark report: the upscaler was named wrongly** (our bug in the report, not in the fix; issues #24 and #28): the
  value 3 of the game's upscaling method is XeSS and 1 is DLSS, but the "settings (the player's)" line printed 3 as DLSS
  and 1 as TSR (an Arc B580 with XeSS showed "DLSS"). Only the text of the report changed.

- **Benchmark:** a run now says when your own `Engine.ini` (`Documents\My Games\Oblivion
  Remastered\Saved\Config\Windows`) holds settings the game does not write itself, usually an engine-tweak mod's file,
  which stays there after an uninstall: a warning in the report (and the stock-vs-fix report) with how many settings, in
  which sections, and the file's first comment, and `engineIni` in `result.json`. Every benchmark game runs with those
  settings, so such results are not the stock game's. With a read-only `Engine.ini`, `-IniCvars` now stops with a clear
  message instead of "access denied". Pull request #45 by luisAzcuaga, thanks (issue #44).

- **Benchmark: a run that stopped during its set-up could leave your saved resolution changed** (our bug in the
  benchmark, not in the fix or the game): with a fixed profile such as the default `small`, a stop while the run
  prepared its config (for example `-IniCvars` with a read-only `Engine.ini`) left the 854x480 window in
  `GameUserSettings.ini` and `AltarGameUserSetting.ini`, and `-Close` reported them as restored. What can refuse is now
  checked before anything is written, every file is on record before it changes, so any later stop restores it, and
  `-Close` says what it really did. A run with your own settings (`-Profile own`, `user`) warns if the saved size is
  that of the small window.

## 1.2.4-beta
For Oblivion Remastered, Steam build 19115871 (Game Pass experimental). Thanks to Mishu117 for pull requests #33, #34,
#35 and #36, to mkusm for pull request #31, to solir2 for the ROG Ally report, and to everyone who reported results and
problems. Numbers marked "ours" are from our test PC (Ryzen 7 5800X, RX 9070 XT); the contributors' own measurements are
in their pull requests.

- **The ini now has three parts** (`OblivionPerfFix.ini`): **performance improvements** (the game's own work done in a
  cheaper way; all on), **game bug fixes** (the game does what it was meant to; on, except `AutosaveFollowsMenu` and
  `FullscreenAllResolutions`, which say why they are off; set any of them to 0 to stay closer to the stock game; the
  crash and freeze guards moved here, with a note to leave them on) and **optional tweaks** (they may change the look or
  how the game plays; all off). Then the diagnostics and `[General]`, as before. Every key keeps its name, value and
  text, and all of them are still read from `[Fixes]`: an ini from an earlier version still works (new keys then have
  their defaults).

### Performance improvements
- **New: fewer hitches where robed characters appear in the distance** (`ClothBuildDefer`, on; a timing change of
  `NpcCreationPacing`'s kind, not a bug fix; pull request #31 by mkusm, thank you): a far-away character you cannot see
  built its robe's or cape's cloth simulation in the middle of its creation, often two or three times. That first build
  now waits a few frames and the repeated requests become one build; it is made at once when the character comes near
  or into view, and after at most 60 frames. Right after that build the robe's bounding box is computed from the new
  cloth and handed to the renderer, as the game does when the robe is put on (without it the robe kept an empty box,
  which the renderer uses for shadows; our addition, which mkusm asked for). The build comes at the latest in the frame
  after the character is first drawn (1-2 frames, like `NpcCreationPacing`'s own steps, mostly inside the spawn
  fade-in). Peryite's Shrine, where five robed worshippers appear (ours): the NPC work in those creation frames 181 ->
  110 ms, frames over 25 ms there 1-2 -> 0-1. Installed at start-up with the version.dll route; needs
  `NpcCreationPacing`. New key `ClothBuildDefer` (1; 0 = off).
- **The world appears sooner after a door, a city gate or a fast travel** (`StreamBehindLoadingScreen`, on; pull
  request #34 by Mishu117, thank you): during a transition the game empties its texture memory, and once the new area
  has loaded it held the last loading-screen image until that area's textures were streamed back in. They now stream
  in while the loading screen is still idling (the same pool size set back with the same call, earlier, and the
  streaming update the game's own wait runs first), so that wait is mostly over when the loading screen ends. The game's
  own wait still runs, so the first picture is the fully textured one (a texture not drawn for half a second gets no mip
  fade in the engine, so textures streamed earlier come in as the game's own wait brings them), and the world does not
  run longer than before. Their measurements: pull request #34. Ours (our PC, small profile (854x480), Cheydinhal inn
  door, switched live A B B A A B: the fade-in's texture wait 0.3-1.9 ms with it against 99-710 ms without; the
  loading screen a little shorter at every transition (arriving outside 6325 -> 5977 ms, entering the inn 89 ms
  shorter, leaving it 4402 -> 3949 ms); hitch time after the exit -29 %; the end pictures within the run-to-run
  difference of two runs without it; 2.1-2.7 ms more main-thread work per loading-screen frame, behind the loading
  screen). New key `StreamBehindLoadingScreen` (1). Our
  review change: the hooks only the optional `UnfreezeBehindLoadingScreen` needs are installed only when it is on.
- **HDR autosaves: the long frame taken off the main thread** (`SaveThumbnailAsync`; pull request #33 by Mishu117, thank
  you): with HDR output
  the game reads the save's picture a second way (as high-precision colour, then converted on the main thread), which
  the takeover did not cover, so every autosave in HDR still stopped the game's main thread. That read is now copied and
  converted off the main thread too, with tables built from the game's own conversion (the self-test runs the game's
  converter and quantiser on every 10-bit value and 8,192 random pixels: identical). The gain in HDR is his
  measurement (pull request #33): our benchmark game runs hidden in SDR, where we checked that nothing changes.
- **`ClothConfigCache`: a cheaper identical check.** The byte-for-byte comparison is unchanged; the data it reads is now
  fetched ahead in one pass instead of one cache miss after another, so the check takes about 28 % less time (ours;
  what is skipped is exactly what was skipped before).
- **Changed: less memory kept on handhelds and PCs with integrated graphics** (`SharedMemoryGpu`, 2; after a report of
  hard freezes on a ROG Ally by solir2, thank you; **whose bug that freeze is, ours or also the game's, is not known
  yet**): on graphics without memory of their own (integrated graphics, APUs, handhelds such as the ROG Ally, Legion Go
  or Steam Deck) the graphics use the PC's RAM, and several of our fixes keep memory on purpose to avoid re-allocation
  stutter (upload memory, spare buffer and list capacity: a few hundred MB, up to about 0.5 GB). On a 16 GB handheld,
  where the game already uses nearly all of the RAM, that memory is missing for the game and Windows, and our memory
  safety net looked at video memory, which on such a PC does not show RAM running out. The DLL now asks the game's
  graphics at start-up whether they use system RAM; if so, and the PC runs Windows itself (not Proton, CrossOver or Wine:
  Steam Deck and Mac players are unaffected, nothing changes there) with less than 20 GB of RAM (16 GB handhelds such
  as the ROG Ally, Legion Go or MSI Claw; the 24 GB ROG Xbox Ally X, where players report the fixes working well, and
  PCs with 20 GB or more keep everything; `SharedMemoryGpu=1` turns it on there, for example if a 24 GB handheld
  freezes), `UploadMemoryKeep`, `LumenKeepArrayCapacity`, `GpuSceneKeepCapacity`,
  `RtSceneBufferKeep` and `OsPageCache` behave as the game does, the automatic `RdgParallelTranslate` stays off, and
  `CommandAllocatorTrim` hands spare command memory back after every load (less than the game keeps). The log says what
  was found and why (`[mem] the game's GPU memory: ... UMA yes, Wine no, RAM 16.0 GB installed: SharedMemoryGpu=2
  (automatic) applies`). Nothing you see changes; on such PCs the travel stutter those fixes remove comes back, the
  other fixes stay. New key `SharedMemoryGpu` (2 = automatic; 1 = always; 0 = never, as on a graphics card with its own
  memory). Not tested on a handheld yet.

### Game bug fixes
- **Our bug fixed: a rare start-up crash in `PsoPrecompileFast`'s driver-thread check** (our bug since 1.2.0-beta, both
  the version.dll and the OBSE64 route; found and fixed in pull request #33 by Mishu117): the cold-cache guard found the
  driver's compile threads with a Windows thread snapshot, which maps memory once for every thread on the PC. The game's
  built-in web browser (CEF) hooks that call while it starts, and a call at the wrong moment jumped to address 0 (the
  monitor thread in the crash report). The OBSE64 route's look-up of the game's main thread used the same kind of
  snapshot (since 1.0.0-beta, before the game's code starts). Both now list the threads with one
  `NtQuerySystemInformation` call, which maps nothing; the same threads are found. This may also explain some start-up
  failures reported since 1.2.0, on both routes.
- **Game bug fixed: the shader screen after a driver update** (`ShaderScreenAfterDriverChange`, on; pull request #33 by
  Mishu117): the game waits for its shader building only on its very first start, so after a new graphics driver it
  builds everything while you play. Now a changed card or driver brings back the game's own first-start shader screen,
  at the engine's fast pace (a few minutes once, after a driver update); your settings are kept, and the first start
  with the fix only notes the card. Their measurements: pull request #33. New key `ShaderScreenAfterDriverChange` (1). It
  notes the card in a new small file, `OblivionPerfFix-gpu.txt`, next to the DLL. Our review change: the card is also
  recorded when the main menu is reached with no precompile running. version.dll route only. Good to know (stock
  behaviour, with or without the fix): after a graphics driver update the first game session also rebuilds the
  driver's own shader cache, so expect higher CPU and memory use and lower fps for its first minutes; once you restart
  the game, sessions run as before.
- **Game bug fixed: "Show VRAM" showed 0/0** (`VramCounterFix`, on; pull request #33 by Mishu117): the counter reads two
  numbers the engine never fills in, so it said "VRAM: 0/0" on every PC. When both are 0 it now shows the game's video
  memory use and the budget Windows gives it, from the DXGI reading the DLL already makes once a second. New key
  `VramCounterFix` (1).
- **Game bug fixed: the real resolution of wide monitors** (`MonitorNativeFromWindows`, on; pull request #35 by
  Mishu117, thank you): monitors wider than 4095 pixels can name a smaller resolution first in their EDID (an Alienware
  AW3926QW says 3440x1440 for its 5120x2160), and the game took that as native: borderless showed it, and fullscreen
  listed only resolutions of exactly its shape. The native resolution now comes from Windows' preferred mode. New key
  `MonitorNativeFromWindows` (1). Start-up only.
- **New, off: every fullscreen resolution** (`FullscreenAllResolutions`, **0**; pull request #35): the fullscreen list
  shows every mode Windows offers (other shapes, and modes above native such as DSR/VSR). Off by default because
  `MonitorNativeFromWindows` already fixes the wide-monitor bug; this only adds choices. Start-up only.
- **Game bug fixed: bow sneak attacks at 1x** (`BowSneakAttackFix`, on; pull request #35 by Mishu117): both engines check
  where a player's arrow lands; the new one uses the bow's sneak multiplier, the old one always 1x, and whichever sees
  the hit first wins (sometimes both: the target is hit twice). With 1 each arrow hits once, with the sneak multiplier.
  It changes damage, back to what the game intends; 0 = the game's behaviour. New key `BowSneakAttackFix` (1).
- **Game bug fixed: magenta specks on skin** (`SeparableSkinAbove4K`, on; pull request #35 by Mishu117): with Effects
  Quality Ultra and a render wider than 3840 pixels (an ultrawide at DLAA or FSR Native AA), sunlit skin and cloth edges
  show glowing magenta specks (a game bug, also without this DLL). With 1 skin scattering uses the engine's separable
  method (`r.SSS.Burley.Quality 0`) while the render is that wide, which shades skin a little differently; at 3840 or
  less nothing changes. New key `SeparableSkinAbove4K` (1). Live.
- **New, off: autosaves that follow the menu** (`AutosaveFollowsMenu`, **0**; pull request #35 by Mishu117): with all four
  autosave options of the Gameplay menu off, the game still autosaves after every level-up and when a quest's script
  asks for a save, two autosaves the menu has no option for. With 1 they follow the menu too. Off by default because it
  also takes away those two automatic saves, which you may want to go back to.

### Optional tweaks (all off by default)
Switches that may change the look or how the game plays, on purpose: not speed-ups and not game-bug fixes; switch one on
only if you want its change. From pull requests #34 and #36 by Mishu117, thank you:
- `UnfreezeBehindLoadingScreen` (+ `UnfreezeBehindLoadingScreenFastTravel`): exits, city gates and fast travel work
  like entering an interior: the world is unfrozen and drawn behind the loading screen for N ms (e.g. 500-1000) before
  the fade-in, so your character already stands normally when the picture comes in. The loading screen is that much
  longer and the world runs that long unseen. Takes a restart to switch on.
- `FadeVtFlushSkip`: 1.5 s into every fade-in the game empties its streamed ("virtual") texture cache (`r.VT.Flush`),
  so walls and roofs go blurry and reload in view; with 1 that flush is skipped. Why the game flushes is not known.
- `VtStreamingUploads`: streamed ("virtual") texture pages uploaded per frame (the game's 4); e.g. 64 makes walls
  reach full detail sooner after stepping outdoors, for more upload work per frame.
- `HdrGamma22`: in HDR the tone curve is refitted so darks and mid-tones match the SDR picture on a monitor
  (gamma 2.2); nothing changes in SDR.
- `HdrMenuBackground`, `HdrSaveThumbnail`: in HDR10 the menu's background and a save's thumbnail hold PQ values that
  the UI shows as colour (grey, washed out); with 1 they are converted to the brightness the world had. Take a restart
  to switch on.
- `HdrMenuRows`: an HDR section on the PC Graphics page (HDR, Peak Brightness, Paper White, Black Floor, Mid-Tone),
  made from the game's own rows and saved like them; with HDR on the saved peak brightness is also used from the start
  (the game used 1000 nits after every launch until the value was changed). Our review changes: the settings save is
  found where the game keeps it (also when Documents is redirected, e.g. to OneDrive), and it is written only after the
  game has written it itself.
- `HudAspectRow`: a "HUD Aspect Ratio" row (Wide, 4:3, 16:9, 21:9, 32:9) that keeps the HUD in a centred area on wide
  screens.
- `DlssRayReconstruction`: a "DLSS - Ray Reconstruction" row (a ray-tracing denoiser mode the game's DLSS plugin has but
  never offers); NVIDIA only, needs nvngx_dlssd.dll (not included).

### Other
- **New diagnostic** `DoorDiag` (off, not in the shipped ini; pull request #34): per transition, the loading-screen
  timeline, each texture stream-all's wait and render-target pool misses, in the log.
- Version 1.2.4 in the DLL's version resource and the OBSE64 plugin version.
- **Benchmark (our bug in the benchmark, not in the fix):** after a graphics driver update the benchmark's first game start
  shows the game's shader screen (`ShaderScreenAfterDriverChange`), and the runner took the still start-up for a hang
  and restarted the game. It now waits for the screen (at most 6 minutes from the launch), and the in-game mod starts
  its per-frame driver once the screen is gone; after a fresh start the runner also waits (up to 90 s) until that
  driver runs before the first scene, and its watchdog notes now appear in the run output and `result.json`.

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
