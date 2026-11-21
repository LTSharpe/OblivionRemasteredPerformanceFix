# Changelog

Downloads, checksums and build attestations: https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/releases

<!-- Each release has a section named after its tag without the "v" (tag v1.0.0-beta -> "## 1.0.0-beta"); the build
     refuses to publish a tag that has no section here, and uses the section as the release notes. Put changes that
     are not released yet under "## Unreleased" and rename it when tagging. -->

Bug fixes say whose bug it was: **game bug** = also happens without OblivionPerfFix (in the game, its engine or a
driver library); **our bug** = caused by an earlier version of OblivionPerfFix.

## 1.6.0-beta
2026-11-21. For Oblivion Remastered, Steam build 19115871 (Game Pass experimental). Numbers marked "ours" are from our
test PC (Ryzen 7 5800X, RX 9070 XT: one PC with an AMD graphics card; nothing here was tested on an NVIDIA card).

**In short:** fewer sudden drops on the roads and where characters arrive. Characters are half built before they arrive
and the loose objects of the cells ahead have their files loaded first; a long route search and the path-search task no
longer hold up a frame; a frame of 20-30 ms takes one physics step instead of two (Epic's own settings; not bit for bit);
the memory clean-up that runs about once a minute waits for a calm frame and is prepared ahead; the black screen at
start-up and every loading screen are shorter. Three game bugs are fixed: NPC torches heard at full volume from anywhere and other characters'
arrow hits heard at the shooter's bow (issue #52), and a spell's colour left on your weapons (issue #48). The OBSE64
package now gets almost every fix, and the HDR section's Black Floor slider goes down to -8. Everything is on by default
except where a line says otherwise; every key has its own switch (`<Key>=0` goes back to 1.5.0's behaviour).

**What was tested.** On our test PC only: the exported source builds and passes its self-test against the game's exe;
hidden smoke runs with the shipped ini and with 1.5.0's ini; our crash repro (8 far teleports, each followed by mounting a
horse; `bench/scenarios/east-crash.json`) 8 of 8 clean with the public build; the keys were played on lab builds during
the week before the release (our own play, no crash seen); and the release candidate's code with these defaults (a lab
build of the same source with diagnostics added; with ours, the HDR options and the diagnostics, which ship off, were on) was played by us
on 2026-11-21, about 12 minutes: no crash and no stutter we noticed. The OBSE64 route was started once with OBSE64 0.2.2,
hidden: every fix in place before the game's main code, the world reached, no crash. Not tested: NVIDIA or Intel cards,
the Game Pass build, Linux/Proton.

- **Performance, new:** a water zone that the game asks to redraw several times during one cell
  crossing is redrawn once, after the crossing (`WaterInfoAfterCrossing`, game behaviour: Unreal Engine's Water plugin
  redraws a zone's water picture on the render thread for every cell removed or added near it). With a time in milliseconds (500 by default), a redraw asked for while cells are still being added or removed waits until none
  has been for 150 ms, or until that time has passed, and then goes one zone per frame as with `WaterInfoSpread`. The
  picture is drawn with the water and ground of that moment; only the in-between pictures are left out, so water next to
  newly loaded ground can show its shore up to that time later. Nothing waits while a loading screen is up, every redraw
  still waiting goes during the first second after it, and water that follows the camera never waits. Our look check (a pinned camera, eight sequences, 500 against 0) found no difference above noise; doors and fast
  travel were not part of it. `WaterInfoAfterCrossing=0`:
  as before.
- **Performance, new:** no frame waits for one long route search after the navigation data changes
  (`RepathDefer`, game behaviour, equivalent). When cells stream in or out, the routes of the characters around are
  searched again in one frame; most take a few hundredths of a millisecond, but a far character's route across a large
  area can take 10-15 ms on its own (in our Imperial City flights it made the same 22-37 ms frame at the Talos bridge
  every time). With 1 these searches run on `RepathParallel`'s helper threads; what is not done after half a
  millisecond finishes in the background while the character keeps following its old route, as it already does in the
  game while a route waits for its search, and is used in the next frame or a few frames later if the character and its
  goal are still within half a metre of where the search started; otherwise the game searches again as before. A search
  that reaches a navigation link the game must decide on (for example a door) stops in the background; when every search
  of the frame has ended within the half millisecond and one of them stopped at such a link, the frame runs as with
  `RepathDefer=0` (`RepathParallel` searches them side by side). Needs `RepathParallel`. `RepathDefer=0`: as before.
- **OBSE64 route: almost every fix now works there too** (our limitation since the OBSE package exists): OBSE64 calls the plugin before the game's engine starts its threads, and the DLL now checks that at start-up (`[stage]` line) and then puts the start-up-only fixes in place as the `version.dll` install does. Only when the plugin is loaded later than that (or AMD's frame-generation library was already loaded by another program) do those fixes stay off, and the log says so. Also on that route: functions another mod (e.g. UE4SS) has already hooked are now still found (`[hook]` line naming the other mod; this was "FName's TCHAR constructor: 0 matches", which left `HdrMenuRows`, `StuckEffectLookFix`, `PhysicsSubstepCap` and the map pop-up fix off). On both routes the DLL no longer writes a patch where another mod's hook already sits in the game's code (its jump at or just before that place) or in one of the game's function tables: that fix stays off, with a `[hook]` line.
- **Game bug fixed: other characters' arrow hits heard at the shooter's bow**
  (`ArrowHitSoundFix`; **game bug**, issue #52, also without the mod): when an NPC's arrow hits someone, the game plays
  the hit sound at the shooter's bow (once or twice), often besides once where the arrow hits, so a hunter near you can
  make a hit on a deer 100 m away sound right beside you at full volume. With `ArrowHitSoundFix=1` such a hit is heard
  once, where the arrow hits, like a sword's hit, with the sound the game gives that hit; if it cannot be played there, the bow's plays as
  before, so no hit goes silent. Your own arrows are unchanged. `ArrowHitSoundFix=0`: as before.
- **Game bug fixed: NPC torches heard at full volume from anywhere**
  (`WorldSoundPositionFix`; **game bug**, issue #52, also without the mod): the game's sound data plays an NPC lighting a
  torch in both ears at full volume however far away the NPC is (often 100 m or more ahead, before you see anyone), and
  deer, daedra-spider and ogre footsteps and eleven dungeon traps always sound centred. With 1 these 15 sound entries get
  3D sound as the game's other world sounds have: the torch is heard from where it is and fades out within 20 m (the fade
  the game's data sets up for it), the footsteps and traps come from their side with their own fade. Each entry is changed
  only if it is exactly as the game ships it; no files change. `WorldSoundPositionFix=0`: as before.
- **Game bug fixed: a spell's colour stuck on your weapons** (`StuckEffectLookFix`;
  **game bug**, issue #48): after an effect with a look (fire, frost, shock, a drain, an enchanted weapon's hit) ended,
  your sword, bow, staff or quiver and arrows kept its colour until you equipped them again, because the game's clean-up
  skipped weapons; a rider who got off a horse during the horse's effect kept the horse's colour. With 1 the leftover
  colour is taken off once the effect has ended (a look under it that is still on and still belongs to the item, such
  as the item's own, stays). Nothing changes while an
  effect is on. `StuckEffectLookFix=0`: as before.
- **Performance, new:** the path-search task no longer waits behind ray-tracing builds
  (`NavQueryPriority`, game behaviour; equivalent). Every frame the game hands its characters' queued path searches to one
  task and later waits for it; the engine runs that task on background threads only, and while the world is busy those
  are all inside ray-tracing pipeline builds of 20-200 ms, so the 1-10 ms search waited in line and the game with it
  (frames of 116 and 161 ms in 2 of 2 of our test flights along the public `showcase` route over the Imperial City
  waterfront). With 1 the DLL sets
  Epic's own setting `TaskGraph.TaskPriorities.NavTriggerAsyncQueries` to `nnn` (normal priority), so an idle normal
  thread takes the task at once (the same line in your Engine.ini does the same and wins over this key). Only the thread
  that runs the task changes: same searches, same order, the game still waits for them at the same place (the task now
  usually gets through more searches before the game asks it to stop, so results arrive the same frame or earlier). The
  setting is Epic's own (credit: Epic Games, Unreal Engine 5.3). In the same flights with the key on those frames were
  gone. Live once its hook is in place (it shares the one `StreamRegisterGranularity` / `StreamInBudget` install at
  start-up, on by default).
  `NavQueryPriority=0`: as before.
- **Performance, new:** one physics step for frames of up to 30 ms (`PhysicsSubstepCap`, game
  behaviour; not bit for bit). The game moves loose objects, corpses and ragdolls in physics steps of at most 20 ms, so
  a frame over 20 ms gets a second step and the game waits for both (about 2 ms more); with riders, creatures or a fight
  around, that makes slow frames come in runs. With 1 the DLL sets the engine's physics settings `MaxSubstepDeltaTime`
  to 0.03 and `MaxSubsteps` to 4 (the same as those two lines under `[/Script/Engine.PhysicsSettings]` in Engine.ini;
  a line of your own there wins): one step up to 30 ms, two up to 60 ms, the longest stretch a frame simulates stays
  120 ms as in the game. Frames of up to 20 ms (nearly all at a 60 fps cap) do not change; in a frame of 20-30 ms a
  falling object or ragdoll moves in one step instead of two half steps (the same motion in a coarser step; characters
  and horses walk and ride with their own movement code). The settings and their rule are Epic's (credit: Epic Games,
  Unreal Engine 5.3). In our first 4K test rides along the east road (two rides each, measured with steps of up to
  33 ms) frames over 28 ms went from 25 and 29 to 11 and 14; on our Imperial City flight we could measure no
  difference. Live once its hook is in place (it shares the one `StreamRegisterGranularity` / `StreamInBudget` install
  at start-up, on by default). `PhysicsSubstepCap=0`: as before.
- **Performance, new:** loose objects in the cells ahead have their files loaded before you get there
  (`CellObjectPreload`, game behaviour). When a cell is switched on, the game makes the books, food, quills, alchemy sets
  and other things placed in it, and an object whose files were not loaded yet made the game stop and read them from
  disk in that frame (1-5 ms each). With 1, when `RowPreload` has loaded the next row of cells a few seconds ahead, the
  files of the objects (and characters) placed there are asked for in the background, with the game's own loader; the
  objects are then made when and as the game makes them and find their files loaded. In our 4K test rides the objects'
  loads from disk inside a frame went from 5-7 per ride to none (one left at the ride's start, before any cell was loaded
  ahead), and at the bend before Aleswell the cell's 13 objects took 6 ms instead of 14-20 ms and the longest frame there
  went from 26.6 ms to 16.7 ms. Nothing is made earlier or later and nothing is kept: files of cells you turn away from
  are freed by the game's next memory clean-up. Needs `NpcCreationPacing`, `RowPreload` and `CreatePreload`.
  `CellObjectPreload=0`: as before.
- **Performance, new:** empty blocks of video memory for textures and buffers are kept for the next
  streaming burst instead of being destroyed 20 frames (a third of a second at 60 fps) after they went empty and made again (`GpuPoolKeepMB`,
  game behaviour: the engine's fixed 20-frame rule). Making a block waits for Windows with a lock held that the render
  thread needs (1-9 ms in a frame; once 17 ms at an Imperial City bridge in our flights). Up to the number of MB set (384
  by default; any value from 1 to 4096 works), the most recently used first; above it, when Windows runs short of memory, or when the game's video memory
  plus that number would pass what Windows gives the game, blocks are given back one per frame. Same picture: only where a texture sits in video memory differs. In our first test rides it cut the number of
  blocks made by about half and the time the render thread spent waiting for them by about half, for about 60 MB more
  video memory in use; we could not yet measure a change in sudden drops, so treat it as a trial. `GpuPoolKeepMB=0`: as before.
- **Performance, new:** the black screen at start-up, before the first logo, is shorter again: the game's
  old engine sorts its list of dialogue topics by name with a bubble sort (millions of name comparisons) while it loads
  its plugins (`TopicSort`, game behaviour); with 1 it uses a merge sort with the game's own name comparison: exactly the
  same order; the sort went from about 0.4 s to 1-2 ms, the stop from 1.2-1.3 s to 0.9 s in our 4K start-ups. When you learn a new topic the same sort runs (a list of 1-28 items: no slower). Needs a restart to switch on. `TopicSort=0`: as before.
- **Performance, new:** the game's plugin files (Oblivion.esm and the others in Data\Plugins.txt, about
  340 MB) are read once into Windows' file cache by a low-priority helper right after the game starts, 7-9 s before
  the old engine loads them on the black start-up screen (`PluginPrefetch`, game behaviour). In three of our slow
  start-ups those reads took about 2 s longer; we think the files were no longer cached then, but could not force that
  case, so this is a hypothesis and not yet a measured gain. Reads only; tested on an NVMe disk only (on a hard disk, if
  start-ups get slower, set it to 0). `PluginPrefetch=0`: as before.
- **Performance, new:** characters are half built before they arrive (`CreateAhead`, game behaviour).
  The game makes a character in one piece on the game thread (6-7 ms for a person) when its cell is switched on, so a
  crowd arrives as a run of long frames. With 1 the first half of that work (Unreal's SpawnActor) is done a few seconds
  earlier, one character per frame and only in frames with time to spare in the frame-rate cap; the game's own create
  then takes the waiting actor and does the rest as before. In our tests such a create took about half the time
  (2.9-3.4 ms instead of 6.1-6.9 ms), and at a crowd the frames that hold a create were 20 ms or longer about a quarter
  of the time instead of about half; the longest frames there (28 ms and more) did not change. A frame that builds an
  actor ahead averaged 16.7-16.8 ms and reached 18.5-21.75 ms at most. The characters ended at the same place, rotation,
  scale and age. An actor is taken only when the game asks for exactly what was built; nothing is kept across a menu,
  loading screen, door, fast travel or save. Needs a frame-rate cap, `NpcCreationPacing` and `CreatePreload`.
  `CreateAheadPool` (24 by default; 0 = 12 waiting actors, as described) lets 12 to 32 wait and lets a spare of a kind not made for
  20 seconds make room for a kind the game made in the last 20 seconds, so more of the road's levelled creatures find
  an actor waiting; the cost is more actors built ahead and thrown away unused, and more memory.
  `CreateAheadLevelled=1` (on by default) also builds ahead the levelled creatures and people the game already placed in a cell at an
  earlier visit (they come back at the crossing when you return), for that very character instead of a spare of its
  kind; the game's own pick is only read, never made or changed. `CreateAhead=0`: as before.
  Fixed before this release (our bug in a test build, not in any release): a character could be built ahead from a
  class the game's garbage collection was removing at that moment, and the game crashed later when it read that class.
  Nothing is built ahead now while the garbage collection is clearing memory, and each build looks the class up again.
  Also fixed before this release (our bug in a test build, not in any release): a character built ahead was announced to
  the game while it still stood 500 m below its place, so the game's index of where characters stand (which other
  characters use to walk around them) could miss a character that stood still for up to 5 minutes. The game is now told
  of it at its own place, as without the fix.
- **Performance, new:** the stop on the black screen at start-up, before the first logo, is about 1.5 s
  shorter, and so is every loading screen (`StartupBarFinish`, game behaviour; 2.8 s -> 1.2 s in our 4K start-ups). In those 1.5 s the game's old
  engine, its data already loaded, filled a loading bar nobody sees, one percent per tick of the Windows clock, stopping
  its loading threads and redrawing the menu at each percent. With 1 that first bar goes to 100 at once, the state the bar
  ends in anyway. Every loading screen ends with the same 1.5 s; the default, 2, also ends those at once (each loading screen about
  1.5 s shorter: a change you can see, the end of every loading screen comes sooner; `StartupBarFinish=1` keeps the
  game's loading screens and shortens only the black screen at start-up). `StartupBarFinish=0`: as before.
- **Performance, new:** an object placed in a cell (plants, rocks, chests, doors, clutter) is checked for what it touches once,
  at the end of its set-up, instead of after each of the four collision switches the game makes in that set-up
  (`FlowOverlapsOnce`), the way Unreal Engine checks once at the end of a grouped move. Objects that already touch
  something are checked as before. The same contacts afterwards; in our test rides the set-ups of a new cell's objects
  took 13 to 33 % less time, with no change in frame rate measured. `FlowOverlapsOnce=0`: as before.
- **Our bug, fixed (issue #54):** on an Intel Arc card with OptiScaler (DLSS frame generation sent to XeFG), opening a
  menu for the second time could freeze the game and end it with a GPU crash (`GpuMenuShot`, our bug since
  1.3.0-beta; `GpuMenuShot=0` was the workaround). We could not reproduce it on the AMD cards the key was tested on and
  found no fault in its own GPU commands, so the menu picture is now made the game's way, as with `GpuMenuShot=0`, on
  Intel GPUs and whenever a third-party swap-chain or frame-generation layer is loaded (OptiScaler, DLSS Enabler, a
  DLSS-G to FSR 3 replacement, or a dxgi.dll / d3d12.dll / d3d11.dll proxy in the game's folder, which includes ReShade
  or Special K installed that way). The log says so once
  per session ("[gpushot] not taken over on this setup: ..."). Other setups keep the faster menu opening. Nothing drawn
  changes.
- **Our bug, fixed:** the start-up logos stuttered for about 1.5 seconds on every start on AMD cards of 16 GB and up.
  `CmdMemPregrow` made its command allocators about 5 seconds after the graphics device was made, which fell on the
  first second of the logos, and the frames being shown waited behind the driver's queue for their memory (in our test 19
  frames of 30 ms or more in the first 2.5 seconds, 3 without it). It now waits for the first loading screen and does
  the same work there in small steps, at most one per frame, still finishing before the first area fades in (needs
  `NpcCreationPacing`, on by default; without it, as before). Nothing drawn changes.
- **Our bug, fixed:** `CommandAllocatorTrim` no longer gives back the copy queue's command allocators below 64,
  counting those in use (`AllocatorTrimCopyFloor`, new ini key, live; 64 by default, about 64 MB of video memory;
  0 = as before). Earlier versions could trim this pool to
  12-20 after a load; when texture streaming on a ride then needed 15-24 at once, they were made again in one go, and
  their first driver memory queued in front of a new texture pool the render thread was waiting for. We saw that hold
  up the picture for 30-50 ms in one of our plays and expect the floor to prevent it; that is not shown yet. The floor
  only stops give-backs and never makes allocators. The stock game never gives any back; nothing drawn changes.
- **New: `PerceptionLosPrefetch`** (pull request #55 by bpodwinski, thank you; an improvement, not a bug
  fix). Five times a second every character checks the line of sight to
  each character in front of it and in range (up to three sight lines per pair), one after the other on the game's main
  thread; in crowds and battles that is the largest single item of that thread. This works the round's sight lines out on
  4 helper threads first (with the game's own field-of-view test, so nothing is traced for nothing) and hands each answer
  to the game's own loop, in its own order, when it asks; the loop starts before all are done. Equivalent: nothing the
  round does between the two can move anyone or change what a sight line sees. `PerceptionLosPrefetch=2` checks only
  (the game's check runs too; differences are counted in the `[perclos]` line): 0 differences in over 1.1 million
  checked sight lines in the contributor's runs. A round is left to the game as before while the engine lists lightweight-instance managers (a
  UE feature whose sight-line hits must be handled on the main thread) or when a character's eye point is not the one
  checked (both added in our review). Measured by the contributor on one PC (Ryzen 7 5800X3D, RTX 5080), `-Scenario actors -Scenes actors-crowd,actors-battle`,
  A-B-B-A in one game: at 4K (`native-nofg`: FSR Performance, no frame generation) the main thread's work per frame
  60.3 / 59.5 -> 39.6 / 42.2 ms in the 76-character battle (13.7-14.2 -> 19.0-20.8 fps) and 19.4 / 19.6 -> 17.2 / 18.5 ms
  in the 40-soldier crowd; a soldier's perception round 750-900 -> 205-230 us (battle) and 400-450 -> 135-150 us (crowd)
  in the `small` profile. Ours (Ryzen 7 5800X, RX 9070 XT, `small` profile, one game, key switched live): 458,097
  precomputed sight lines compared with the game's own in `actors-battle`, `actors-crowd` and a fight of wolves and a
  troll at a farm, 0 different; in `actors-battle` (off, on, on, off) a soldier's round 424-439 -> 133-142 us, the main
  thread's work per frame 29.6 / 30.4 -> 28.3 / 28.2 ms. Off below 8 usable logical processors.
- **Game behaviour, new:** a power-attack animation file stays loaded when the last character that
  holds it lets go (`PowerAttackAnimKeep`; **game behaviour**, also without this mod). The game keeps these files
  loaded while nobody uses them, but threw one away at that moment, and the next person built then loaded it again in
  the foreground, pausing its background loaders for each file: five times in about an hour of our play, four of them
  1-2 ms, one 75 ms in a single frame (the worst frame of that half hour, 86 ms). With the key the file is left as the
  game leaves the ones nobody held. Nothing is loaded differently and the animations are the same; at most 6 MB of
  animation files (on disk; 1.4 MB for people) stay loaded until the game closes. `PowerAttackAnimKeep=0`: as before.
- **Engine behaviour, new:** the engine's garbage collection pass, which runs about once a minute, waits
  for the first calm frame when it falls due on a busy one (a cell being added to or removed from the world, characters
  being set up, an already slow frame), with the engine's own one-frame delay and for at most 500 ms by default
  (`GcPassAfterBusy`). The same pass runs; only its moment moves, by at most the bound, and a later pass also collects
  what became garbage in the wait. Forced and full collections, menus, loading screens and low memory (as `MemoryRelief`
  sees it; none with `MemoryRelief=0`) are never delayed.
- **Engine behaviour, new:** the cells you leave behind are prepared for the garbage collection pass in
  calm frames of the last second before it, instead of all at once inside the pass (`GcPrepAhead`). The pass, about once
  a minute, first prepares every cell that has been waiting since the last pass (about 0.05-0.1 ms each); after a long
  ride that is a large part of a long pass (at a camp after a ride in our play, 57 cells and a 14.5 ms pass, about 5 ms of
  it this preparation). With 1, the engine's own preparation is run for the oldest waiting cells (waiting at least 3 s),
  at most two cells or 0.5 ms a frame, holding the engine's garbage-collection lock as the pass does, and the pass
  prepares only the rest. Nothing is freed earlier: the
  same pass frees the same things. A cell prepared this way and wanted back before the pass is loaded again instead of
  being taken back as it was (the engine's own behaviour with `LevelStreaming.ShouldReuseUnloadedButStillAroundLevels=0`);
  the window of one second keeps that rare, and the log counts it. Never in a menu, dialogue or loading screen, while
  files load in the background, or in a busy or already slow frame. Needs `NpcCreationPacing`. The gain in the game is
  not measured yet.
- **Engine behaviour, new:** the engine's worker threads that help with a garbage collection pass run
  their part at normal priority while they do it (`GcMarkPriority`), the level of the game's own threads, so that on a
  busy PC a helper is less likely to lose its core while the game thread waits for it (one pass of ours took 16.5 ms where
  our model of passes of that size predicts about 7 ms, with other programs busy; the gain in the game is not measured
  yet). The same work is done; only which thread gets a core first changes. The value 2 (above normal) also raises the
  game thread above the render and RHI threads and is not recommended.
- **Engine behaviour, new:** the large memory blocks the game gives back to Windows are handed back by
  a background thread (`OsPageCacheAsyncRelease`), so the clean-up after a garbage collection pass waits less for
  Windows on the main thread (on the road to Bravil these calls added up to several milliseconds in one frame, ours).
  The memory goes back to Windows a little later; what the game does doesn't change.
- **Menu, changed:** the HDR section's **Black Floor** slider (`HdrMenuRows`) now goes down to -8 (it stopped at -4);
  the default stays -4. On displays with deep blacks (OLED) lower values make the shadows darker while mid-grey and
  everything brighter stay as they were; from about -6 the darkest shadows turn fully black. A saved value of -5 to -8
  is kept.

**Also new in this version, off by default** (not yet played; `<Key>=1` or a value switches one on):
- **Performance, new, off by default:** no frame-rate-limit wait before a frame that is about to be long (`CapSleepSkip`,
  game behaviour). With a frame-rate limit the game waits before each frame, also before a frame that is about to run a
  garbage-collection pass or the next step of removing a cell from the world, and the wait and the long frame add up to
  one sudden drop (an 8 ms wait before a 25 ms frame shows as 33 ms). With 1 (GC pass), 2 (cell removal) or 3 (both) that
  wait is cut by up to 8 ms; the frame starts and is shown earlier, and the game still moves on by the time step it would
  have taken after the full wait, so that frame simulates the same step. The game's clock ends up ahead of the real clock
  by each cut (up to 8 ms, for good), and when the shortened frame then waits for the render thread and the next frame
  does not wait, that next frame takes a step longer by up to the cut. At most one frame in four, never in menus or
  loading screens. Needs `NpcCreationPacing` and `GcPassAfterBusy`. Off by default in this version (not yet tested in
  play). `CapSleepSkip=0`: as before.
- **Performance, new, off by default:** usually no single long route search in the frame after the navigation data
  changes (`RepathEarly`, identical). When a cell's navigation data is loaded, unloaded or rebuilt, the engine searches
  the affected characters' routes again at its next step, and one long search can hold up that frame by itself, which
  `RepathParallel` cannot split. With 1 those searches start on `RepathParallel`'s helper threads at the moment of the
  change, with the engine's own search code; a result is handed over only when every input of the engine's own search
  is byte for byte the same and the navigation data has not changed since; anything else is searched as before, and a
  search that can no longer be handed over (the character has moved on, for example) is stopped instead of waited for.
  Needs `RepathParallel`. `RepathEarly=0`: as before.

## 1.5.0-beta
2026-11-15. For Oblivion Remastered, Steam build 19115871 (Game Pass experimental). Numbers marked "ours" are from our
test PC (Ryzen 7 5800X, RX 9070 XT: one PC with an AMD graphics card; nothing here was tested on an NVIDIA card).

**In short:** fewer sudden drops while characters arrive and while you travel, mostly in towns, at city gates and on
roads. Three of our own timing problems in the character pacing are fixed (our bug, there since 1.0.0-beta), three waits
the game makes for the disk or for itself are removed (the first weapon hit on each surface, a returning creature's
animations, the next row of cells), and a cell you leave, a bow draw and a new row of cells cost less in one frame
(ideas from Epic's later engine versions, credited). Everything is on by default except where a line says otherwise;
every key has its own switch (`<Key>=0` goes back to 1.4.0's behaviour).
- **Our bug, fixed:** hair steps of far characters piling up into one long frame (`PacingHairActor`); the area set-up
  giving up on far characters too early (`PacingFlowProgress`, `CreatePreloadEarly`); the far characters' steps landing on
  frames that were already slow (`PacingBaseGate`, an improvement of our pacing).
- **Game bug, fixed:** the first weapon hit on each kind of surface froze the game for a moment (`ImpactPreload`).
- **Game behaviour, restored from the 2006 game:** the next row of cells is loaded ahead (`RowPreload`); a returning
  creature's animations load in the background (`CreatureAnimPreload`).
- **Engine behaviour, as Epic changed it in later Unreal Engine versions:** a cell behind you leaves the world over a few
  frames (`StreamOutEndPlaySlice`, UE 5.6); a bow draw no longer waits for every background load (`ResetLoadersNoFlush`,
  UE 5.4); far characters hidden behind buildings do not animate in their first second (`FadeInUnseen`, UE 5.5); a
  smaller streaming budget per frame while riding (`StreamInBudget`, UE 5.6's idea).
- **New in the same spirit:** several file reads at once (`ParallelFileReads`), indirect argument buffers from the shared
  buffer memory (`IndirectArgBufferPool`, UE 5.5's setting), a busy cell you left unloaded in steps (`CellEvictSplit`) and a
  far character kept faded out until it is built (`PacingGateShownHoldMs`).

**What was tested.** On our test PC only: the exported source builds and passes its self-test against the game's exe;
hidden smoke runs with the shipped ini and with 1.4.0's ini; our crash repro (8 far teleports, each followed by mounting a
horse; `bench/scenarios/east-crash.json`) 8 of 8 clean, with the public and with the lab build; the keys were played before
the release on lab builds (our own play, no crash seen); the last review fixes of the shown-hold key and the cell-unload
key were checked in one hidden Imperial City flight and the crash repro; and the release candidate itself (this build with
these defaults) was played by us on 2026-11-15, about 13 minutes on the roads from the Old Bridge to Bravil and from the
Old Bridge to the Fort Alessia bridge: no crash, and three short stutters we noticed (approaching Vilverin, the Ayleid
wall approaching Sercen, approaching Aleswell); the two roads named ran without a stutter we noticed. Not tested: NVIDIA or Intel cards, the Game
Pass build, the OBSE64 route (these keys install only with the `version.dll` package, see the README list), Linux/Proton.
This is a beta: please report what you see, with `OblivionPerfFix.log`.

Details:

- **New: far characters arriving out of sight no longer animate for their first second**
  (`FadeInUnseen`, part of `NpcCreationPacing`; **engine behaviour**, also without OblivionPerfFix): the engine counts a
  new character as seen when it is drawn in its first frames, before it knows whether something hides it, and then
  animates it at full rate for a whole second; a crowd arriving behind the Imperial City Waterfront's buildings did that
  every time (up to 15 animation updates a frame instead of 4). Now a far character that is still fading in and has not
  been drawn for 0.2 s no longer counts as seen; one that is drawn animates exactly as before. In our Waterfront test the
  arrival's animation work ended after 0.3 s instead of 1.0 s and the frames' time over 16.7 ms in it fell by about half
  (cold start, 4K, frame generation, 60 fps cap; ours); on the whole Imperial City flight (warm) the Waterfront's frames of
  25 ms or more went from 20 to 15 per flight, while its two short drops remain (other work of the arriving characters).
  Its animation starts when it is first seen, as for any hidden character in the game. Equivalent, not identical: what is
  drawn differs in one way, the pose of such a character when it first comes into view (the game would have animated it
  unseen for up to a second, here it starts when seen; in our tests we saw no odd pose, and nothing jumps once it is
  seen). The idea is Epic's (credit: Epic Games, UE 5.5.0, commit
  8dd2cb4237: a new object counts as drawn only once that is known), done here for far characters only.
  `FadeInUnseen=0`: as in 1.4.0 (live after a start with 1).
- **New: a smaller per-frame streaming budget while riding** (`StreamInBudget`, 0 / 1 / 2): when a new
  row of cells streams in, the engine's own two budgets (adding objects 4 ms, reading files 5 ms per frame) are lowered to
  1 / 3 ms (1) or 2 / 3 ms (2) while you play, so the burst's slowest frame at Memorial Cave went from 28.8 to 22.4 ms
  (ours, 4K, frame generation, 60 fps cap). Far cells finish up to about 0.8 s later (we measured 0.4-0.8 s; 0.15 s with 2); loading screens
  keep the game's budgets; your own Engine.ini or console values are kept. The idea is Epic's (Unreal Engine 5.6's shared
  streaming budget), done with 5.3's own settings. Equivalent: the same work in the same order. On at 1; `StreamInBudget=0`: the game's budgets (live).
- **New: indirect argument buffers from the engine's shared buffer memory** (`IndirectArgBufferPool`; **engine
  behaviour**, also without OblivionPerfFix): the engine made each of the small buffers that tell the graphics card how
  much to draw as a separate block of video memory, each with its own driver call on the render thread. With 1 they come
  from the shared buffer memory like every other buffer: separate blocks per ride 401 -> 11 on the road to Bravil, and the
  indirect-argument ones 188 / 102 -> 0 on the Imperial City flight and 496 -> 0 in our crash test of 16 far teleports
  (ours). Less memory-creation work on the render thread, a small gain: in our tests it removed no drop you would see by
  itself. Only where the buffers sit in memory changes: our crash test ran clean, 7 views compared equal and video memory
  stayed within 0.1 GB. This is Unreal Engine's own setting `d3d12.AllowPoolAllocateIndirectArgBuffers`, on by default
  since UE 5.5 (credit: Epic Games, UE 5.5, commit 97aba58adb); your own Engine.ini line for it still wins.
  On by default; `IndirectArgBufferPool=0` switches it off (start-up only; `version.dll` package only).
- **New: several file reads at once** (`ParallelFileReads`; **engine behaviour**, also without
  OblivionPerfFix): the engine reads the game's container file with one reader that handles a single 256 KB block at a
  time, so blocks wanted at the same moment wait behind each other. The case you can see: when a newly loaded area first
  uses its textures, the render thread waits for the lowest-detail piece of each (23-28 blocks), 8-9 ms on a fast NVMe
  drive, and that frame takes about 31 ms: a hitch at the same place on the first visit in every session (we measured it
  at the waterfall on the road to Bravil). With `ParallelFileReads=8` up to 8 blocks are read at the same time, with the
  engine's own queue order, buffers and hand-over: that wait went from 8.1-9.1 ms to 0.7-4.7 ms and the frame from
  30-33 ms to 23-29 ms (25 ms in the middle of our runs); over the whole ride to Bravil a world piece's block waited 9 ms instead of 19 ms (median) (ours, 4K, frame
  generation, 60 fps cap; 282,000 blocks per ride, none failed). The same bytes are read; nothing else changes. On a hard
  disk, or with the engine's own block cache switched on, the game's reader is kept. Unreal Engine 5.7 replaces this
  backend with a new one; this is our own change to the 5.3 reader. On by default (`ParallelFileReads=8`; 0 = the game's reader); the setting
  is read at start-up, then live. Works with both packages (tested with the `version.dll` package).
- **New: a busy cell you left is unloaded in steps** (`CellEvictSplit`, part of `CellBufferPacedEvict`;
  **game behaviour**, also without OblivionPerfFix, where such a cell is unloaded inside the crossing frame): the old
  engine unloads a cell you left in one step, and a cell with many people in it made that one frame long (about
  20 ms for one cell of the Imperial City Waterfront with ten characters, ours). With `CellEvictSplit=2` the bodies of
  two of its characters are unloaded per frame, with the game's own call for each and in the game's order, before the
  rest of the cell and the cell itself are unloaded: 5-6 steps of 3-5 ms instead of one of 21 ms. The game thread's
  longest frame at that unload went from about 24 ms to 17-18 ms (ours, two hidden runs, 4K, frame generation, 60 fps
  cap, game thread only); what you see on screen at that spot did not change measurably in the flight we watched. While
  any arrow is in the world, and for a character that a spell effect is on, the
  game's own unload is used (taking those out early would remove the arrows or end the effect differently from the game).
  The same cells and characters are unloaded, a few frames apart, far behind you. A small gain: in our tests it removed no
  drop you would see by itself. Live; `version.dll` package only (it needs `CellBufferPacedEvict`).
- **New: a far character stays faded out until it is built** (`PacingGateShownHoldMs` with
  `PacingGateShownHoldEnd`, part of `NpcCreationPacing`; improves our own `PacingBaseGate`, new in this release): while
  the gate makes a far character's body or clothes step wait for a frame with room, that character's fade-in already
  runs, so its unfinished body can start to show faintly (**our side effect** of `PacingBaseGate`; with this key off it
  still can), and the steps the end of its fade-in forces out land on slow frames. With
  `PacingGateShownHoldMs=1000` (and `PacingGateShownHoldEnd=10`) such a character stays fully faded out until its steps
  have run (at most 1 s), and those steps run before its fade-in ends, one a frame (a few a frame only at low frame rates
  when one a frame cannot make it); it then fades in a little faster (up to about 3 times when the hold lasted its full
  window) and is fully visible when the game's own fade-in ends. On the Imperial City flight fewer steps were forced into slow frames (3-5 per flight against 6-10, ours; measured on the
  build before this release's review fixes, which changed the rule that picks when each step runs);
  the frames of 28 ms or more did not differ clearly in our few flights. This changes what you see in one way: when such a
  character becomes visible (later in its fade-in, then faster; never earlier or more visible than in the game); one whose
  weapon was made before its body can show the weapon without the body, usually for about 0.1-0.2 s (at most until the
  hold ends, under a second). Its fade-in timing, its AI and
  everything the game reads are the game's. On by default (`PacingGateShownHoldMs=0` = the game's fade); the setting is
  read at start-up, then live; `version.dll` package only.

- **Fixed: hair steps of far-away characters could pile up into one long frame** (`PacingHairActor`, part of
  `NpcCreationPacing`; **our bug**, there since 1.0.0-beta): a character's hair step was never matched to its character, so
  it counted as near; behind a row of far characters waiting their turn it waited too and then ran at once with all the
  others (14 hair steps on top of a character's other steps, one 43 ms frame at the Imperial City Waterfront, ours). Hair
  steps now wait or run like their character's other steps; once a waiting character starts to fade in, its steps and
  the ones queued before it run at the pace that normally finishes them before it shows (hair landing late was seen on
  our 4K Waterfront flights before this), never more than 2 steps in one frame and, besides the first step, about 16 ms
  of them, so the catching up adds little to any one frame (the first step runs at its real cost). With a very deep
  queue of large steps a character can still begin to show a little before its last step. Timing only, in the game's order.
  `PacingHairActor=0`: as in 1.4.0 (live).
- **Fixed: a far-away character could still make the game wait for the disk when an area finished loading**
  (`PacingFlowProgress` and `CreatePreloadEarly`, parts of `NpcCreationPacing` / `CreatePreload`; **our gaps**, since
  1.0.0-beta and 1.3.0-beta): an area's set-up waits for far characters still owed to it, but gave up after three frames
  while one of them was waiting for its files to load in the background, or while they came one per frame from earlier
  frames; the rest was then created at once (24-28 ms waiting for the disk, or two creations in one frame, at the Imperial
  City Waterfront; ours). The set-up now keeps waiting while the line moves (still at most eight frames; the world logic
  pauses for those frames, as during the one long frame before), which removed those waits. In addition, a character held
  back for the frame's budget now asks for its files at once, a head start of a frame or more (no gain we could measure
  on its own). Timing only, in the game's order. `PacingFlowProgress=0` / `CreatePreloadEarly=0`: as in 1.4.0 (live).
- **Improved: the next row of cells is loaded ahead again while you travel** (`RowPreload`; **game behaviour**, also
  without OblivionPerfFix): the original 2006 game loaded the row of cells you were heading into on a background thread;
  the remaster still asks for that row but never loads it, so every cell-row crossing read the whole row from the game
  files in one frame while the game waited. The cells the game asks for are now loaded with its own loader, one per quiet
  frame before the crossing, and kept with the cells you have just left; the crossing only switches them on. The same cells
  with the same contents; nothing appears earlier. `RowPreload=0`: as in 1.4.0 (start-up, then live).
- **Fixed: the first weapon hit on each kind of surface froze the game for a moment** (`ImpactPreload`; **game bug**,
  also without OblivionPerfFix): the first hit on metal, cloth, stone and so on in a session made the game load that hit's
  effect and the blood effect from disk while it waited (65 ms of an 84 ms frame near Mortal Camp; ours). The game's own
  list of these effects, for every surface, is now loaded in the background when the game's settings are ready (at the
  main menu) and kept loaded, so each hit shows the same effect without the wait. Costs some memory for the session.
  `ImpactPreload=0`: as in 1.4.0 (start-up, then live).
- **Improved: fewer sudden drops while new characters arrive** (`PacingBaseGate`, part of `NpcCreationPacing`, with
  `PacingGateBoundFrames`, `PacingGateCalmMs`, `PacingGateNearM`; improves **our own pacing**, there since 1.0.0-beta): a
  far-away character's body, clothes and hair steps (7-14 ms each) waited only for the frame's time budget, which is
  decided before the frame's own work is known, so while characters arrived (frames of 16-26 ms) one often landed on a
  frame that was already slow and the frame took 25-36 ms (the Imperial City Waterfront, in our tests). Each waiting step
  is now decided on its own: a step of a character that is at least 100 m away and either out of sight or still at the
  faint first stage of its fade-in runs only after the frame's world update and only if the frame then stays within
  25 ms, or on a calm frame (one a frame, so such a frame can go over 25 ms); at most one large step a frame, oldest
  first, and none waits longer than `PacingGateBoundFrames` (45 frames: 0.75 s at 60 fps, 1.5 s at 30 fps). A character
  that comes near, or whose fade-in goes on,
  has its steps run as before, ahead of the waiting ones. So a far character may be finished up to 45 frames later; one
  drawn at that faint first stage shows it that much longer. On the Imperial City flight in our tests (4K, frame
  generation, 60 fps cap; three flights each way in one game) the Waterfront's frames of 28 ms or more went from 19 to
  10 per flight on average (10-24 without, 7-14 with); its short drops remain. Timing only (our own steps; each character's
  steps keep the game's order). `PacingBaseGate=0`: as in 1.4.0 (live).
- **Improved: a cell behind you leaves the world over a few frames instead of in one long one**
  (`StreamOutEndPlaySlice`; engine behaviour, changed by Epic in Unreal Engine 5.6): when a cell is streamed out, the
  engine tells all its objects at once that they are leaving, and in this game each of them then unhooks itself from
  lists the whole world shares: one frame of 4-14 ms per cell on a ride (3 of the 9 slowest frames of a 15-minute ride;
  ours). As Epic's Unreal Engine 5.6 can do it (credit: Epic Games, UE 5.6.0, commit dfeb9ecce7), that step now goes a
  few objects at a time within the engine's own limit of 1 ms per frame. The same objects in the same order; the hidden
  cell leaves the world's list a few frames later. `StreamOutEndPlaySlice=0`: as in 1.4.0 (start-up, then live).
- **Fixed: drawing a bow could freeze the game while something loaded in the background** (`ResetLoadersNoFlush`;
  engine behaviour, fixed by Epic in Unreal Engine 5.4): renaming an object (the bow draw renames the arrow) made the
  game wait for every background load in flight, up to about 20 ms (ours), though the rename needs none of them. As in
  Unreal Engine 5.4 (credit: Epic Games, UE 5.4.0, commit 0d790f0ba5), it now waits only for a package with an
  old-style loader attached, which this game's packages do not have. `ResetLoadersNoFlush=0`: as in 1.4.0 (start-up,
  then live).
- **Improved: a creature that comes back no longer loads its animations while the game waits** (`CreatureAnimPreload`;
  **game behaviour**, also without this mod and the same in the 2006 game): when a kind of creature returned after all
  of its kind had unloaded (a horse, a mudcrab, a deer by a road you rode before), the game found its skeleton still
  loaded, did not ask for its animation files ahead, and then loaded them one by one in the foreground, pausing its
  background loaders for each: up to 64 ms in one frame of a recorded ride (ours). The creature's loading step now asks
  for them the way the game already does for summoned creatures, so the background loader reads them first; same
  files, same animations, and in our tests the creature was ready as soon as or sooner than before.
  `CreatureAnimPreload=0`: as in 1.4.0 (live).

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
