# How OblivionPerfFix works, in plain words

## The big picture

Oblivion Remastered runs two engines at once. The original 2006 engine still runs the game itself (quests, characters,
cells), and Unreal Engine 5 draws the picture on top. Several jobs in that pipeline run on **a single CPU thread**. When
one of those threads is full, the whole game waits for it. That's why the game can run at 30-40 fps in Cheydinhal while
Task Manager shows the CPU only about 30% busy: one maxed-out thread out of 16 barely shows in the total. And every time
a new part of the world streams in, a burst of work lands on those same threads at once: the hitches you feel while
travelling.

Each fix below does **the same work in a cheaper way**: it skips work whose result would be identical, keeps memory the
game would ask for again a moment later, moves work to threads that would otherwise wait, or uses the engine's own
cheaper path. None of them changes *what* is drawn: no lower settings, no shorter view distance, no simplified
lighting. Two of them change only *when* something happens, in the game's own order: far-away characters you can't see
are built a few frames later (`NpcCreationPacing`), and the cells behind you are unloaded a few frames later
(`CellBufferPacedEvict`).

Every fix has a switch in `OblivionPerfFix.ini` (the name in brackets after each heading), all on by default in 1.0.0-beta, and the
DLL turns a fix off by itself if it can't find the exact code it expects (for example after a game update). The fixes
are grouped by what they help with. `RdgParallelTranslate=2` selects automatically (off on cards under 9.5 GB);
`FrameGenPacingTuning=1` selects Tuning Set B. `HitchLedger` is a diagnostic and is off by default. The evidence for every
measurement below, and the ideas we tested and rejected, are in [research.md](research.md).

The test PC's own play settings are 4K, FSR Performance, frame generation, a 60 fps cap, hardware ray tracing and
Ultra with Shadows High. Individual measurements below state any changes to those settings.

## Rendering: the CPU side of each frame

<a id="boneuploadstores"></a>

### Bone upload (`BoneUploadStores`)

[Evidence](research.md#boneuploadstores).

**The problem.** Every animated character (NPCs, creatures, the player) has a skeleton. Each frame the CPU copies all bone positions into a buffer the graphics card reads. In Cheydinhal that's about 7,200 bones per frame. The copy used a CPU instruction called a *masked store*, which is fine for normal memory. This buffer, though, lives in a special kind of memory meant for sending data to the GPU ("write-combined" memory), and masked stores are extremely slow there.

*Analogy:* a delivery van is meant to leave only when it's full. The masked store makes it drive off with every single parcel.

**What we did.** In every case the game used it, the mask said "write exactly the first 16 bytes". We swapped that instruction for a plain 16-byte write, which is a different instruction that writes the same bytes.

**Why the image can't change.** The GPU receives **byte-for-byte identical data**. Only the way the CPU writes it changed.

**Effect.** This copy was 33% of the render thread's time. After the fix it's 1%.

---

<a id="atmospheredirtyfilter"></a>

### Atmosphere dirty filter (`AtmosphereDirtyFilter`)

[Evidence](research.md#atmospheredirtyfilter).

**The problem.** The game's weather and time-of-day system pushes about 40 settings every frame to the sun, the moon, the fog, the sky and the sky light (brightness, colour, fog density...). After every push it tells Unreal "this object changed, rebuild it", **even when the value is the same as last frame**. That came to about 3,700 rebuild requests per second. Rebuilding the sun is especially expensive: Unreal removes it from the scene and adds it back, and checks it against all ~13,000 objects in view each time.

*Analogy:* repainting a wall every morning because someone asked "is the wall still white?" and the answer was yes.

**What we did.** We take a snapshot of each object before the setting is applied. If nothing changed, we skip the "rebuild" request. If anything did change, the request goes through as before. Unreal's own setters already work this way: they ask for a rebuild only when the value actually changes.

**Why the image can't change.** The rebuild we skip would have produced exactly the light, fog and sky that are already on screen. Every real change still triggers a rebuild.

**Effect.** Rebuild requests dropped from ~3,700/s to ~10/s, and the sun stopped being rebuilt every frame. The render thread became about 13-20 percentage points less busy.

---

<a id="atmospherelightfastpath"></a>

### Light updates (`AtmosphereLightFastPath`, `AtmosphereLightCloudPath`)

[Evidence](research.md#atmospherelightfastpath).

**The problem.** `AtmosphereDirtyFilter` only helps when nothing changes. For long stretches of the day, though, the sun's brightness and colour temperature really do change a tiny bit every frame (the sun slowly setting, a weather blend). So the sun was still being torn down and rebuilt every frame for most of the day.

*Analogy:* to dim a lamp, you unplug it, take it apart and put it back together, when there's a dimmer switch right there.

**What we did.** When only brightness, colour temperature, indirect lighting strength or volumetric scattering changed, we call Unreal's own "update colour and brightness" function, the dimmer switch. Unreal's standard light setters use exactly this call for exactly these four values. The light's colour goes the same way: the game's own "set light colour" makes exactly this call too (a thunderstorm tints the sun several times a second). Any other kind of change still takes the full rebuild.

**Why the image can't change.** The new values reach the renderer in the same frame through the official route. We checked with screenshots taken while switching the fix on and off during a sunset: the brightness changes smoothly with no step, and the difference maps show only water ripples and swaying plants.

**Effect.** Full sun rebuilds dropped from ~50/s to 0, and the render thread became another ~11 percentage points less busy.

**The clouds (`AtmosphereLightCloudPath`).** On some routes the weather also moves two cloud settings of the sun and moon a little every few frames: how strongly clouds shade the ground under the sun, and how bright the clouds look in its light (walking down from Echo Mine: ~2,000 changes a minute). Unreal has no dimmer switch for these two, so each change still tore the sun down and rebuilt it, about 9% of the render thread's time there. Every frame, before it draws anything that uses them, the renderer itself writes the sun's other sky values into the sun's render-side copy. The fix now hands it the two new cloud values at that same moment, instead of the rebuild. A moon that is hidden (by day) has no render-side copy at all, so there is nothing to update for it; when it shows again, it is built from the values it has then.

*Why the image can't change.* The renderer gets the same two numbers, before the frame that uses them is drawn (at most one frame earlier than the rebuild would have delivered them). The DLL checks this as it goes: before each change it compares the sun's render-side copy with the values the sun had; on the walk they matched every time (2,021 of 2,021 in one minute at 480p; at the test PC's 4K settings, where the values change every frame, every change too). Values the renderer has not picked up within a quarter of a second (no frame drawn, as behind a loading screen) go the old way, by a rebuild.

*Effect.* On that walk (480p, uncapped, one game, runs taken in turn): the render thread's time per frame 8.3-10.1 -> 6.3-8.2 ms (medians 8.6 -> 7.0), and the share of its time spent re-adding the sun ~9% -> 0. The game thread is unchanged. Where the clouds hold still (a static view at noon in clear weather) the fix has nothing to do: switching it on and off every 10 seconds there gave the same frame rate (67.7 vs 67.4 fps).


**Weather changes.** When the weather itself changes (rain clearing up, a storm coming in), the blend moves more of the sun's settings every frame for half a minute or so: how wide the sun's disc is and how soft its shadows are, how much it lights water and shiny surfaces, how dark its light shafts get behind clouds, and how much it glows (bloom). Unreal has no dimmer switch for these either, and the renderer also reads all of them from the sun's render-side copy every frame, so the fix hands them over at the same moment as the cloud values: twelve settings in all, each one only where the game's own code shows that plain copy, under the name the game itself gives it. The sun and moon get this; any other light, such as the thunderstorm's lightning light, is rebuilt as before, because the renderer only makes that per-frame call for the sun and moon (the DLL checks that the renderer drew the light as a sun or moon in the last quarter of a second).

*Effect.* A rain-to-clear change below Echo Mine (480p, 60 fps cap, three runs each, taken in turn): during the blend, the render thread's time per frame 6.9-7.9 -> 5.5-6.4 ms, and the sun is no longer rebuilt at all (without the fix: about 270 rebuilds every 5 seconds). The DLL's check held for every change: before each one, the sun the renderer draws had exactly the values the game had given it.

**Which light lights the sky (1.1.0, issue #1: a bug in our fix as shipped in 1.0.0-beta, not in the game).** The sky, the sun's disc and the clouds are lit by one chosen light (per "atmosphere slot"). Unreal chooses it only when a light is added to the scene: the brightest one for that slot. The game rebuilds every weather light every frame, so in the stock game the choice is made again every frame. The sun and the thunderstorm's lightning light share a slot. With the fixes above as they were in 1.0.0-beta, a lightning flash took the slot when it began and kept it while it faded, so the sky and the cloud layer went dark until the lightning light was fully off: the clouds flickered in and out through a storm. Now, while two lit lights share a slot (or a lit sun has not been drawn as the sky's light for a tenth of a second), their updates go the game's way and they are rebuilt every frame, so the brightest one is chosen again every frame, exactly as in the game. That happens only during flashes; the rest of the time nothing changes.

*Checked:* lightning made by hand at Hircine's Shrine (cloudy noon, the lightning light switched on bright, then dimmed, then off): with 1.1.0 as first built the clouds disappeared while it was dim (screen brightness 75 -> 22-27); with this change, and in the stock game, they stay (73/70 vs 73/70). During normal play the new check never triggered.

*A second, rarer case:* cloud values left for the sun or moon are now dropped when that light loses its render-side copy, and only ever written into the copy of the light they were left for, so a copy made later at the same memory address cannot get old values.
---

<a id="rdgparalleltranslate"></a>

### Parallel command translation (`RdgParallelTranslate`)

[Evidence](research.md#rdgparalleltranslate).

**The problem.** After `BoneUploadStores`, `AtmosphereDirtyFilter` and `AtmosphereLightFastPath` the render thread was no longer the bottleneck. The next one was the **RHI thread**, which turns Unreal's drawing instructions into DirectX 12 commands for the graphics card. Unreal already *records* most of a frame on many CPU cores. In this engine version (UE 5.3), though, it then *translates* almost all of that work on the single RHI thread. Only Nanite's passes are allowed to translate in parallel. The RHI thread sat at 90-95% busy while the other cores were 60-88% idle.

*Analogy:* ten writers write the chapters of a book at the same time, then one translator translates the whole book alone.

**What we did.** We allow every recorded chunk to be translated in parallel, on the cores that were idle. Unreal 5.5 made this the default, so this is the engine's own newer behaviour. Ray-tracing commands still run on the RHI thread in their original order, because they share data and must not be split. This fix also includes a guard against a start-up crash we found (an Unreal 5.3 bug where a new command context could ask for memory before it had any).

**Why the image can't change.** The graphics card receives the same commands in the same order. Only which CPU core prepares them changed. Reference images with the fix on and off match (SSIM 0.99, brightness within 0.6%).

**Keeping buffers safe.** At the start of each frame the RHI thread tidies video memory: it moves up to 32 MB of live buffers into fuller pools, copies their contents and frees the old copy a frame later. A view created during a move can miss the move notice and keep pointing at freed memory (UE-217692, fixed in Unreal Engine 5.5). We saw this once on 2026-10-07, about two seconds after leaving the chapel for Cheydinhal. Parallel translation also needs the frame's moves to finish before its commands are prepared, or a command could write into the old copy after it was copied.

With `ViewRenameGuard` on, the guard closes the view race and a frame fence orders the start of each frame before its translation, as Unreal Engine 5.5 does. Buffer tidying can therefore run alongside translation, saving about 0.2 GB. If `ViewRenameGuard` is off or cannot be installed, the DLL pauses buffer tidying while translation runs, and restores it a second after translation stops. If the required safety checks cannot be installed, parallel translation stays off. Moving a buffer preserves its contents, so the image cannot change.

**Memory measurements (2026-10-07, 720p, six places in four regions, two fresh launches per variant).** Measured with an earlier build, the total increase over stock was about 0.65 GB: about 0.37 GB for paused tidying and 0.3-0.4 GB for parallel translation's command allocators, upload pages and descriptor heaps. The pause kept partly used pools; empty pools were still released. Reference images in a dark Rumare view matched stock with the safety guard; without it they varied 2-7% darker. The guard's frame-rate cost was within noise across six places. Letting tidying run only behind loading screens saved no memory: pools fragment during play.

**Effect.** The RHI thread went from ~90% to ~55% busy, for **+22-25% fps** in CPU-bound spots on its own.

**On 8 GB cards.** The extra command buffers cost 0.4-1 GB of video memory. On a card with less than 9.5 GB the game is already over the memory Windows gives it at 1080p/1440p High, and with the graphics card as the limit the CPU gain does not show; with an 8 GB card's budget (about 7 GB) and translation on, most scenes ran slower than the stock game (1440p FSR Performance High, Rumare 30 vs 39 fps, road walk 39 vs 46), while with it off every scene ran faster than stock (48 and 55 fps) at the same video memory. So the default `RdgParallelTranslate=2` switches it on only for cards with 9.5 GB or more (`1` always on, `0` off).

---

<a id="rdgsubmitbatch"></a>

### Submission batching (`RdgSubmitBatch`)

[Evidence](research.md#rdgsubmitbatch).

**The problem.** `RdgParallelTranslate` created a side cost. Each parallel chunk was sent to the GPU as its own package: about 115 submissions per frame instead of about 5. The AMD driver adds a small pause at every package boundary, which added ~2-3 ms of GPU time per frame. That doesn't matter at 720p, but at 4K (where the GPU is the limit) `RdgParallelTranslate` alone lost about 5% fps.

*Analogy:* mailing 115 separate envelopes instead of a few boxes.

**What we did.** We hold the finished chunks back briefly (never longer than 1 ms) and hand them over in groups of up to 32. Unreal's own existing merge then sends each group as one package. The order is always kept: a group is sent before anything else reaches the GPU, and anything that can't be merged safely goes out on its own, exactly as before.

**Why the image can't change.** Same commands, same order, just in fewer packages. The GPU does identical work.

**Effect.** About 25 submissions per frame instead of ~115, and GPU time per frame dropped ~3 ms (canal noon 15.7 → 12.2-12.6 ms). Parallel translation no longer costs anything on a busy GPU, and in a GPU-limited test (render scale 200) it was even faster than with parallel translation off (38.5 vs 34.0 fps, one run each).

---

<a id="gpusceneparallelupload"></a>

### Scene data uploaded on helper threads (`GpuSceneParallelUpload`)

[Evidence](research.md#gpusceneparallelupload).

**The problem.** When a wood or rock field streams in, its trees and rocks are added to the renderer, and in the next
frame Unreal writes each new tree's and rock's data for the graphics card (where it stands, how it is turned, how big
it is) into an upload buffer. Unreal already cuts that job into small batches of up to 127 objects so that many helper
threads can share it, but in the engine version the game uses (5.3) it then runs all the batches on one helper thread,
one after the other, unless a setting (`r.GPUScene.ParallelUpdate`) says otherwise, and its default is off. At Kvatch's
spot C that one thread works for about 50 ms; the render thread waits for it and the main thread waits for the render
thread: the longest frame of the arrival (50-53 ms at 480p and at the test PC's 4K play settings, with every physics fix on).
Later Unreal versions (5.8) run it in parallel by default once there is enough work.

*Analogy:* the foreman has already sorted the bricks into 300 small piles for the crew, then lets one bricklayer carry
all of them.

**What we did.** The fix switches Unreal's own setting on, so the batches go to all helper threads. It finds the setting
in the game the way Unreal registers it (its name, its default 0, then where the game keeps its value) and writes 1
into it once the game has registered it; switching the fix off puts the 0 back. A value the player set for that
setting (in Engine.ini or the console) is left alone.

**Why the game can't change.** Nothing that is drawn: each batch writes its objects into its
own place in the upload buffer, places that are worked out before the batches start, so the buffer holds the same bytes
whichever thread writes which batch and in which order. The upload is still finished in the same frame, at the same
point, as before. Only the time it takes changes. The self-test finds the setting in the game file and checks the
switch on a stand-in value (on, off, a player's own value).
`GpuSceneParallelUpload=1`; it can be switched while the game runs.

**Measured:** Kvatch C, the arrival's worst frame 50-55 -> 38-43 ms (480p and 4K FSR Performance with frame generation); Imperial City flight -5.5 % ms over budget; same images.

---

<a id="clothconfigcache"></a>

### Cloth config cache (`ClothConfigCache`)

[Evidence](research.md#clothconfigcache).

**The problem.** Many clothes are simulated as cloth: shirts, skirts, the town guards' tabards. Each cloth piece has a settings list (stiffness, damping, wind, collision thickness and so on, about 40 entries per level of detail). Every frame, for every cloth piece, Unreal 5.3 throws away the simulation's copy of that list and copies it again from the original, even though the game never changes it. At the Cheydinhal chapel square that was about 2,000 copies per second, which took 5-7% of the game thread. The game thread is the one that limits the frame rate there. Unreal 5.6 still works this way.

*Analogy:* reprinting the whole recipe book every time you cook, in case a recipe changed.

**What we did.** Before each copy we compare the simulation's copy with the original, byte by byte. If they're identical we skip the copy, because it would produce exactly what is already there. If anything differs, even a single flag, the copy is made as before. After every copy we check that the copy now matches the original. If that check ever fails, the fix switches itself off.

**Why the image can't change.** The cloth simulation reads the same settings either way: the copy we skip would have been byte-for-byte identical to the one in place. Reference images with the fix on and off match (SSIM 0.99, brightness within 0.3%, the same as two runs with it off). A clothed character also looked the same in screenshot sequences taken while the fix was switched on, off and on again.

**Effect.** At the chapel square the copies dropped from about 2,000 to under 1 per second (99.98% skipped). Cloth went from 8-11% of the game thread to 3.4-4.7%, which includes the comparison itself. The game thread's CPU time dropped by about 0.5-0.6 ms per frame (about 4-5%). The fps gain was small and within the run-to-run noise (+1% to +10%).

## Travelling: memory and threads

<a id="lumenkeeparraycapacity"></a>

### Lumen's object lists keep their memory (`LumenKeepArrayCapacity`)

[Evidence](research.md#lumenkeeparraycapacity).

**The problem.** Lumen, the game's lighting, keeps lists of everything it lights: one entry for every tree, rock and wall piece near you (tens of thousands of entries), plus their cards and pages. Once per frame, after objects stream in and out, it tidies those lists. When the entries at the end of a list were removed, the tidy-up gives the unused memory back by moving the whole list into a smaller block; when the next objects stream in, the list no longer fits and is moved into a bigger block again. Each move copies the whole list into fresh memory, 25-40 ms, and the frame waits for it. While you travel, areas stream in and out every few hundred metres, so this happened again and again (four times on our 1.5-minute test route through the Blackwood forest, each one a visible hitch).

*Analogy:* moving house every time you throw out a few boxes, and again every time you buy a few new ones.

**What we did.** The tidy-up still runs; it just keeps the list's memory. The list holds exactly the same entries in the same order; only its unused space stays reserved, up to the most it has ever needed. The biggest list (one entry per tracked object: 1.5 million entries, 120 MB, on our forest route) also grows in bigger steps (double instead of +37.5%), and when a new area's lighting starts from scratch after a door or a fast travel, its list goes straight to the size this session has needed before once it is an eighth of the way there, instead of being copied again and again as it fills up. So the lists move rarely: on our route not once while travelling, and at a door exit once instead of four times.

**Why the image can't change.** Lumen reads the entries, never the size of the block they live in.

**Effect:** measured with `UploadMemoryKeep` below.

---

<a id="uploadmemorykeep"></a>

### Upload memory kept between streaming bursts (`UploadMemoryKeep`)

[Evidence](research.md#uploadmemorykeep).

**The problem.** New textures, meshes and lighting data reach the graphics card through "upload" memory: the CPU writes them there and the card copies them out. The game gives that memory back to Windows 20 frames (a third of a second) after a burst of streaming, and keeps only 5 of its 4 MB blocks for small uploads. While you travel, the next burst comes a few seconds later and has to ask Windows for the memory again. Getting a block from the graphics driver takes several milliseconds (up to 30 ms for a large one), and the game holds a lock while it does, so texture streaming, the renderer's setup work and the render thread all wait for it.

*Analogy:* returning the moving van after every trip and renting it again an hour later.

**What we did.** Empty upload blocks are kept for the rest of the session instead of 20 frames, and the next bursts reuse them; up to 24 of the small-upload blocks are kept instead of 5 (travel never needed more than 24 at 480p or 4K; keeping 64 cost an extra 160 MB of video memory). Memory is only kept, never added: the game still asks for exactly what it asked for before, and what is kept is at most what the game needed at its busiest moment . A time limit (about a minute) was tried first and dropped: after a big burst such as loading a save, all its blocks then expired together a minute later and were handed back in one go, which itself caused a hitch.

**Why the image can't change.** The graphics card receives the same data at the same time; only where it is staged in memory differs.

**Effect** (`LumenKeepArrayCapacity` and `UploadMemoryKeep` together; the Blackwood forest route north to the Wayshrine of Stendarr and back south past Water's Edge at running speed, 854x480, CPU-limited, three runs each, game time over the median spent in frames of 2x the median or more):

| | As the game ships | With `LumenKeepArrayCapacity` and `UploadMemoryKeep` |
|---|---|---|
| South leg, hitch time | 459-686 ms | 226-249 ms |
| South leg, worst frame | 68-71 ms | 40-55 ms |
| North leg (without the NPC spawn spot), hitch time | 393-513 ms | 299-337 ms |
| At 4K, FSR Performance, frame generation, 60 fps cap (two runs each): south leg, hitch time | 183-200 ms | 0-84 ms |
| Same, north leg without the NPC spawn spot | 193-232 ms | 122-211 ms |
| Cheydinhal road at running speed (854x480, two runs each) | 218 ms, 1% low 27 fps | 142 ms, 1% low 30 fps |
| After leaving the inn / the west gate in Cheydinhal | 2.9 s / 2.5 s, 1% low 11 / 19 fps | 2.6 s / 2.1 s, 1% low 15 / 27 fps |

The cost is a little more system memory (+0.04-0.46 GB committed in a route plus four doors and fast travels; the memory actually in use, the working set, did not change), and it does not grow over time.

---

<a id="gpuscenekeepcapacity"></a>

### The GPU's object lists keep their size (`GpuSceneKeepCapacity`)

[Evidence](research.md#gpuscenekeepcapacity).

**The problem.** The graphics card keeps a list of every object in view range and one of every instance (each tree, bush and grass clump): in a forest 2-4 million instances, 100-250 MB. The engine sizes each list to the next power of two of what it holds and, whenever that changes, in either direction, makes a new list and copies the old one over. A new list of that size is a fresh block of video memory from Windows, made while the frame waits: on the Roxey Inn road at night it was a 118 ms frame in one run and 22-34 ms in others (the cost depends on the state of video memory). After a fast travel or a door the scene refills in several doublings, each a new list. And one small list (per-instance extra data) flips between two sizes every frame, because two parts of the engine disagree on its size: about 2,000 new lists a minute.

*Analogy:* a shop that moves into a bigger warehouse each time stock passes a round number, and into a smaller one each time it drops below, packing everything every move.

**What we did.** A list keeps its size when what it needs drops by one step, and only shrinks when the need falls to a quarter (for example when you go indoors). Lists under 4 MB never shrink, which ends the per-frame flip. And when a list grows again past an eighth of the largest it has been this session (after a fast travel or a door), it goes straight to that size instead of doubling several times. For the instance list the size is also how far apart its columns lie for the shaders, so the fix changes the size where the engine computes it, before both are taken from it; checked in the game by making every list twice its size: the pictures matched the unmodified game.

**Why the image can't change.** Nothing you see: the same data in a list with room to spare. The cost is at most one step of extra video memory for these lists (typically 50-100 MB). **What it does not fix:** the first time in a session that a list grows to a size it never had, which is what the 118 ms frame on the Roxey road was. Every size change is written to the log.

---

<a id="instanceclearreserve"></a>

### Clearing grass without re-copying its list (`InstanceClearReserve`)

[Evidence](research.md#instanceclearreserve).

**The problem.** Grass and other mass-placed plants are drawn as "instanced" meshes: one object holding thousands of
copies. When the game clears one as you move on, it writes a "hide" order for every copy into a list, growing the list a
little at a time. For a clump of 29,000 blades that list is moved to a bigger place in memory and copied some 30 times
in that one frame. Below Echo Mine 15 such clears happen in a 20-second walk, and the biggest stalled the game thread for
about 20 ms (56-64 ms frames under a 60 fps cap).

*Analogy:* writing a 30,000-line list on index cards and moving to a bigger box every time the box is full, instead of
taking the right box first.

**What we did.** Right after the game empties the list, the DLL makes room for exactly as many orders as the game is
about to write (the same count the game's loop uses), with the engine's own memory function. The game then fills it
without moving it.

**Why the image can't change.** Nothing you see: the same orders, in the same order. The list just has no spare room left over.
**Measured.** On that walk under a 60 fps cap (three runs each, taken in turn): time lost to frames over 25 ms 69-131 -> 46-57
ms, frames of 25 ms or more 17-22 -> 12-14.

---

<a id="ospagecache"></a>

### Spare memory blocks reused (`OsPageCache`)

[Evidence](research.md#ospagecache).

**The problem.** Unreal's memory manager gets its memory from Windows in blocks (64 KB pages for small objects, and
blocks of up to 16 MB for bigger arrays). When a block is no longer needed it goes onto a short list of spare blocks,
and the next request of exactly that size takes one from the list. The list holds only 64 blocks. With many threads and
many different sizes it is always full, so most blocks go back to Windows and fresh ones are requested a moment later:
in a town about 1,700 times a second each way (~450 MB/s), even at 480p. Each round trip costs two system calls
(9 and 19 microseconds on average in the game, because many threads do it at once) and, worse, every fresh block
arrives empty: the first write to each of its 4 KB pages is a page fault (about 60,000-110,000 a second in Cheydinhal,
and up to 16,000 in one frame when Kvatch's woods arrive). This "first write into fresh memory" was the floor every
optimisation of the collision work reached.

*Analogy:* a workshop that returns every empty box to the warehouse and orders a new one a minute later; each new box
has to be unfolded before use.

**What we did.** The two tiny functions through which the manager asks Windows for a block and gives one back are
redirected. A block it gives back is kept in a second, bigger list (up to 64 MB, at most half of it one size), and when
the manager's own list has no block of the size it needs, it gets a kept one of exactly that size (the most recently
kept first). Blocks unused for 5 seconds go back to Windows; all of them go back at once while Windows is short of RAM
or commit (`MemoryRelief`), when a request to Windows fails (then it is retried), or when the switch is turned off.

**Why the game can't change.** Only which addresses the blocks have and how much memory the
game keeps committed (at most 64 MB more). A recycled block still holds old bytes; it is handed only to the two
places where the manager would itself have handed out a spare block of the same exact size with old bytes, so no code
can depend on getting zeroed memory there. Everything else that asks Windows directly (the manager's own tables, the
per-thread caches, the 32 MB emergency reserve, blocks over 16 MB) still gets fresh memory, and the emergency reserve
is still given back for real. The fix checks both functions' code and that they go through the game's own
VirtualAlloc/VirtualFree imports, finds the manager's two calls by their shape, and stays out with a log line
otherwise. The self-test runs the cache with fake Windows functions: exact-size reuse, the cap, the per-size limit,
the 5-second trim, the low-memory flush, a failed request, the off switch, and eight threads at once (no block given
to two owners, every block back to Windows at the end).

**Measured (2026-10-16, with every call traced, Kvatch spot C flight in a new world, 480p, one running game,
ABBA):** page faults during the flight halved (19,000 -> 9,800 a second); in the 7.2 s around spot C the collision
index passes took 14-16 ms instead of 24-26, the trees' growth 9-12 ms instead of 17-19, the time over 16.7 ms
319-326 ms instead of 526-547, frames over 25 ms 7-9 instead of 12-15, the worst frame 38-41 ms instead of 48-49.
Cheydinhal canal at noon: page faults 4,800-5,000 a second instead of 105,000-116,000, but no measurable change in
frame rate or thread times there. At 4K with the test PC's own play settings (60 cap, frame generation) the index passes and tree
growth were again shorter (cisa 16-19 vs 19-23 ms, growth 10-13 vs 13-21 ms) but the frame counts stayed within noise.
Memory: the game's private bytes and working set rose by about the cap (0-60 MB) or less.

<a id="navmeshbuildnobusywait"></a>

### Navigation tiles built only by free threads (`NavmeshBuildNoBusyWait`)

[Evidence](research.md#navmeshbuildnobusywait).

**The problem.** While the world streams in, the game builds the map its characters walk on (the navigation mesh) in the background, one tile at a time, 20-110 ms each. The engine's helper threads share one job list. When a helper has to wait for another job, it does not sit idle: it takes other jobs from the list and runs them meanwhile, and it cannot stop waiting until the job it took is finished. The characters' movement (Altar's steering, which runs on the helpers every frame) does such a wait, for a navigation query. If the helper happens to pick up a tile build while waiting, that character's movement for the frame is stuck until the tile is built, and the game thread, which needs all characters moved before it can go on, waits too. On the Silver Road at 4K this made an 84 ms frame while every other helper was idle. It is rare (about 4% of the tile-building time ran inside such a wait in a 4K run, far less at small sizes) but each time it is a long frame.

*Analogy:* a cashier waiting a moment for a price check starts unpacking a whole delivery pallet meanwhile, and the queue waits for the pallet.

**What we did.** The engine has a flag for jobs that must not be picked up while waiting ("do not run inside a busy wait"); the navigation tiles are started without it (in UE 5.3, and still in 5.5). The fix sets that flag where the tile job is started: one byte. The tiles are then built only by helpers that have nothing else to do.

**Why the image can't change.** Nothing you see or play: the same tiles from the same data, so the same paths. Only which helper builds a tile, and when within the frame, changes. In a test run all ~1,860 tile-building samples ran on free helpers, none inside a wait.

---

<a id="busywaitflagcheck"></a>

### The navigation wait check completed (`BusyWaitFlagCheck`)

[Evidence](research.md#busywaitflagcheck).

**The problem.** `NavmeshBuildNoBusyWait` marks the navigation-mesh tiles (the map the characters walk on, built in the background while the
world streams in; one tile takes 0.1-0.8 s) as "do not run me inside someone else's wait". A thread that waits for a job
may run other queued jobs meanwhile, and Unreal 5.3 checks that mark there, but only on the first job it takes: when that
one is marked, it puts it back, takes the next one from the queue and runs it without looking at its mark. When a
streaming burst has queued dozens of tiles, the next one is often another tile. In the Imperial City flight the thread
that was waiting was working on the characters' steering for the current frame, so the whole frame waited for the tile:
2.6 s once and 0.25 s once in four sampled flights, and the 75-130 ms "tick group 0" waits seen there before look the same.

*Analogy:* the rule says "a cook waiting for the oven may help with small jobs, but not with the big roast". The cook
checks the first ticket, sees "roast", puts it back, grabs the next ticket without reading it, and it is the next roast.

**What we did.** After putting a marked job back, the waiting thread now stops there and reports "nothing done", exactly
as when the queue is empty; its waiting loop then looks at what it is waiting for and tries again, and the next try
checks the next job properly. In the game file that is one byte in each of the four places where Unreal puts a job back
(a jump that was taken only when something had already run becomes a jump that is always taken, to the same place).

**Why the game can't change.** Every job still runs, with the same code and the same inputs; a
marked job runs on a thread that is free instead of inside a wait, which is what the mark asks for and what Unreal
already does for the first job it takes. Which thread runs a background job, and when, already varies from run to run
with thread timing. The tiles come out the same; the frame no longer waits for one. The self-test finds the four places
and switches them on and off.
`BusyWaitFlagCheck=1` in the shipped ini; it can be switched while the game runs.

**Measured:** Imperial City flight, small profile: tiles built inside a wait 6 in 10 flights without the fix (frame stalls of 82 ms to 2.6 s), 0 in 7 with it (largest tick-group-0 wait 29 ms); 4K FG test completed without a crash.

## Travelling: physics, when a new area arrives

<a id="physicstreetimeslicing"></a>

### Physics search tree rebuilt a little at a time (`PhysicsTreeTimeSlicing`)

[Evidence](research.md#physicstreetimeslicing).

**The problem.** The physics engine keeps a search tree of every solid object, so collisions and line-of-sight checks only look at objects nearby. When objects are added it rebuilds the tree in the background, a little each frame. But when more than 1000 changes are waiting (a whole newly loaded area), it rebuilds everything in one go, and the game waits for that at the end of the physics step: about 30 ms, a hitch.

*Analogy:* a library that re-shelves a few returned books every hour, but closes for the afternoon when a big donation arrives.

**What we did.** The limit goes from 1000 to 100000 waiting changes, so big updates are spread over frames like small ones. It is one of the engine's own settings (`p.Chaos.AccelerationStructureTimeSlicingMaxQueueSizeBeforeForce`); the DLL sets it and puts the game's value back when switched off.

**Why the game can't change.** The same tree is built either way, just over more frames. Until it is done, the engine looks up the new objects in a separate small tree of recent changes, so every collision and every line-of-sight check finds exactly the objects it found before.

**Effect:** at the spot on the test route where this happened, hitch time 48 -> 21 ms (two runs each).

**Bigger steps.** The tree is still rebuilt a little at a time, but "a little" was very little: 4000 objects per
frame, and the finished tree is copied to the game's side 100 KB per frame. When a forest streams in that takes one to
two seconds, and every object that arrives meanwhile has to be added to the new tree in the one frame the rebuild
finishes (`PhysicsFlushParallel`): the 25-45 ms hitch below Echo Mine. Each step now does ten times more of the work, in the
background, so a rebuild is done in a few frames. The bigger copy step is only used while a rebuild has a backlog
(it has run eight frames, or two thousand objects are waiting); in a quiet world the rebuilds swap at the game's own
pace, so nothing changes there. The same trees are built.

---

<a id="physicsflushparallel"></a>

### Collision catch-up on two threads (`PhysicsFlushParallel`)

[Evidence](research.md#physicsflushparallel).

**The problem.** The physics engine keeps two copies of its collision search tree (`PhysicsTreeTimeSlicing`): one for the physics
itself, one for the game's own checks (line of sight, the camera, footsteps). When a background rebuild finishes, every
object that arrived while it ran is added to both new copies before they are swapped in, first to one, then to the
other, while the game waits for that physics step. Walking down from Echo Mine towards Brotch Camp a whole forest
streams in at once, and that catch-up held 9,000-13,000 objects: 25-45 ms in one frame, the worst hitch of the walk.

*Analogy:* two clerks who must file the same pile of new cards into two card catalogues, and only one of them works while
the other watches.

**What we did.** When at least a thousand objects are waiting, the DLL lets a helper thread fill the game's copy while the
game's own code fills the physics copy (the two copies share nothing). The helper makes exactly the calls the game makes
for its copy, in the same order with the same values. Together with the shorter rebuilds of `PhysicsTreeTimeSlicing`, the largest
catch-up on that walk went from 32-36 ms to 7-8 ms, and the walk's worst frame from 56-57 ms to 33-36 ms
(480p, three runs each).

**Why the image can't change.** Nothing you see: both copies end up exactly as they would have, at the same moment. Only the wait is
shorter.

---

<a id="parallelstaticbodies"></a>

### Collision for woods and rock fields on several threads (`ParallelStaticBodies`)

[Evidence](research.md#parallelstaticbodies).

**The problem.** Forests, bushes and rock fields are "instanced" meshes: one object with hundreds of copies. When such
an area streams in, every copy that has collision gets its own physics body. The game makes them one after the other on
its main thread: for each copy it creates the physics object and its collision shapes, then adds them all to the
physics world. Below Kvatch's north cliff that is about 170 ms of main-thread time per pass, spread over a few frames,
and part of the drop to 30-40 fps there (2026-10-14).

*Analogy:* a builder who cuts every stone of a wall himself before laying them, while three helpers stand by.

**What we did.** The cutting is shared out: the game's own "create the bodies" step runs on a few helper threads, each
on its own share of the copies, while the laying (adding them to the physics world, which numbers them and files them
into the collision search tree) stays on the main thread, in the game's order. The first body of each group is still
made first and alone, as the game does, because that one also sets up things that are made only once (the physics
material). Groups with fewer than 64 bodies, and groups where some bodies already exist, are left to the game.

**Why the game can't change.** Nothing in the world: the same bodies with the same shapes, numbered and filed in the same order;
only where in memory they sit differs, which also varies from run to run in the game itself. The reasoning from
Unreal's source and the game's code is in the code's header. On by default (it installs when the game starts with `ParallelStaticBodies=1`; afterwards it can be switched off and on while it runs). The log
shows the time per body with and without it.

**Measured (2026-10-14, 4K with frame generation, three fresh games, Kvatch walls flight):** below the north cliff
37.4 rendered fps with it against 35.2 / 35.6 without, frames of 25 ms or more 126 against 158 / 154, the same image
(visual comparison "same"); on the Brotch walk no measurable change. No crash in any run.

**Two parts of this fix:**

- *The helpers' own tool boxes.* The game's memory manager gives each of its own threads a
  private stock of small memory blocks; a thread without one has to queue at a shared counter for every block. `ParallelStaticBodies`'s helper threads (and `PhysicsFlushParallel`'s) are the DLL's own threads, so they queued there for every one of the many small
  pieces a physics body is made of, and mostly waited for each other: four helpers made the bodies only about 1.3 times
  as fast as the main thread alone. Each helper first asks the game for its own stock, exactly as the
  game's threads do. Only where in memory the pieces sit changes. *Analogy:* the helpers each get their own box of
  nails instead of lining up at the one box the foreman keeps.

- *Filing while numbering.* After the bodies are made, the game numbers each one in the physics
  world and then files it into the collision search index, body by body. Now the main thread numbers them while one
  helper files them, in the same order, each one only after it has its number; the physics world is locked against
  everyone else until both are done, so nothing can look at a half-filed index. The game's own code does both jobs.

Measured (2026-10-14, 480p, Kvatch flight past the north cliff, one fresh game per run, 2 runs each way): making the
bodies 2.6-2.8 -> 1.5 microseconds each (95-102 -> 54-56 ms per flight), adding them to the physics world 2.2-2.3 ->
1.8-1.9 microseconds each (70-75 -> 58-61 ms): about 55 ms less main-thread time per pass below the cliff. The same
bodies in the same order in every run (36,475 made, 32,482 filed, no order mismatch). The frames there are still
held up by the physics step that follows (inserting the same bodies on the physics side).

---

<a id="pipelinedbodycreation"></a>

### Collision bodies handed over in batches (`PipelinedBodyCreation`)

[Evidence](research.md#pipelinedbodycreation).

**The problem.** When a wood or rock field streams in (Kvatch's north cliff, the Brotch walk, the Imperial City's stone
bridge and Waterfront), the frames in which its area is added to the world are mostly the main thread setting up the
collision of instanced meshes (trees, rocks, bushes): at Kvatch's spot C about 120 of the 140 ms that area costs the
main thread there. With `ParallelStaticBodies` that happens in three steps per mesh, one after the other: all its bodies are made (on
the helpers and the main thread), then all of them are numbered and filed into the physics world (`ParallelStaticBodies`), then all of
them are entered into the physics world's lists of which object owns which body.

*Analogy:* the bricklayer waits until the whole pallet of bricks is cut before laying the first one, and only starts
the paperwork when the last brick is laid.

**What we did.** The helpers now cut the bricks in small batches in wall order, and the main thread lays each group of
64 as soon as it is cut, does that group's paperwork, and goes on, while `ParallelStaticBodies`'s helper files the laid ones into the
search index right behind it. When the main thread gets ahead of the helpers, it cuts the next batch itself instead of
waiting. All of it runs with the game's own code: the game's step that hands the bodies over is simply run once per
group instead of once for the whole mesh, inside the same lock of the physics world that the game takes for it.

**Why the game can't change.** Nothing the game can see: every body is made the same way, gets
the same number, is filed and listed in the same order, and all of it happens in the same frame, inside one lock of the
physics world, so nobody sees a half-finished state (the reasoning, step by step against Unreal's source and the game's
code, is in the code's header). Only where in memory the pieces sit differs, as with `ParallelStaticBodies`. The self-test runs the
game's flow on stand-ins both ways for 300 random meshes and compares every body's numbering, filing and listing.
`PipelinedBodyCreation=1` (needs `ParallelStaticBodies=1` at start-up; filing runs on
the helper as part of `ParallelStaticBodies`); it can be switched while the game runs.

**Evidence.** The self-test checks ordering and results for 300 random meshes. An isolated timing measurement for this part is not available.

---

<a id="dirtytreequickinsert"></a>

### A quicker filing rule for new collision (`DirtyTreeQuickInsert`)

[Evidence](research.md#dirtytreequickinsert).

**The problem.** The physics world keeps a search index of everything that can be collided with, so that a footstep,
a line of sight or a falling object only tests what is nearby. Rebuilding that index takes a while, so the game does it
in the background; anything new or moved in the meantime goes into a small second tree of "recent changes", which
every search also looks in. When a wood or a rock field streams in, thousands of trees and rocks arrive at once
(below Kvatch's north cliff: 5,000-11,000 per physics step). For each one the game picks its place in that tree with
a thorough search over the whole tree (Unreal's "best sibling" branch and bound), and the main thread waits for the
physics step to finish those inserts: 13-22 ms in single frames there.

*Analogy:* a librarian who, for every returned book, walks the whole "returns" trolley to find the perfect gap, while
the queue grows; the books go to the real shelves at the next re-shelving anyway.

**What we did.** For that tree of recent changes only, the place is picked with the usual quick rule (walk down from
the top, always into the branch that grows least; stop where pairing is cheaper, as in Box2D's dynamic tree). After
that, the game's own code does the insert as before (new branch, bounds, its own rebalancing). The main tree, the
tree of moving objects and everything else keep the game's own search.

**Why the game can't change.** The tree holds exactly the same entries with exactly the same
sizes; only how they are grouped differs. Every collision search visits every entry whose box touches what is
searched, whatever the grouping, so it finds the same things; the grouping only decides in what order they are
visited, which matters only between two hits at exactly the same distance or for the order of an unsorted list. The
game already varies this grouping from run to run: it checks whether the background rebuild has finished without
waiting for it, so which entries sit in the main tree and which in the recent-changes tree, and how both are shaped,
depends on thread timing. The next rebuild is made from the full list of entries, not from this tree, so the
difference ends there. The old engine (quests, AI, scripts) does not use this index at all.
Checked in the game: on foot down the slope through the streaming area, the character's path is the same with and
without it. Byte patterns check the tree's layout in the game's code; if anything does not match, the
fix stays off and says so in the log. `DirtyTreeQuickInsert=1` before the game starts; afterwards it can be switched off
and on while it runs.

**Measured (2026-10-14, 480p, Kvatch flight past the north cliff, one held game, 3 runs each way interleaved):** the
physics step's index work below the cliff 51-58 ms instead of 88-96 ms, its longest step 23-29 ms instead of 38-41 ms,
adding the bodies on the main thread 1.0 instead of 1.8 microseconds each (the main thread waited 20 instead of 44 ms
for the inserts, with pipelined scene addition); below the cliff the worst frame 52-54 ms instead of 56-59 ms, frames of
33 ms or more 6-7 instead of 9-10, time over 16.7 ms 303-387 ms instead of 400-438 ms. Whole flight: time over 16.7 ms
363-471 instead of 490-504 ms.

---

<a id="bulkindexinsert"></a>

### New collision filed as one branch (`BulkIndexInsert`, `PhysicsInternalFlushParallel`)

[Evidence](research.md#bulkindexinsert).

**The problem.** `DirtyTreeQuickInsert` made the "recent changes" tree of the physics search index quicker to file into, but each new
entry still goes in on its own: find a place, add a branch, then walk up to the top of the tree refitting and
re-balancing every level. When a wood streams in below Kvatch's north cliff, the physics step does this 4,500-5,300
times per frame, and 8,000-11,000 times in a frame where a rebuilt index is swapped in; the same happens on the main
thread when it takes over a new copy of the index, and on `ParallelStaticBodies`'s helper while the main thread adds bodies.

*Analogy:* the librarian files every returned book on its own, walking to the shelf end each time; instead the whole
trolley is wheeled to one spot and sorted there in one go.

**What we did.** During one such pass (the step's filing of new bodies, its two catch-up passes after a rebuild, the
main thread's catch-up, `ParallelStaticBodies`'s adds), each new entry is hung on one branch that belongs to the pass, with the game's
own code (the game's leaf, the game's free lists, the entry's bookkeeping exactly as the game makes it), without the
walk to the top. At the end of the pass that branch is re-linked into a balanced shape (entries sorted along a
space-filling curve, split in halves), still inside the same call. Separately, `PhysicsInternalFlushParallel` acts in a frame that swaps in a rebuilt
index, the game files the frame's new bodies into the old index only after catching the new one up, one after the
other although the two passes touch different trees; this runs them side by side.

**Why the game can't change.** Exactly the same entries with exactly the same sizes, each in the
same leaf the game made for it; only how they are grouped differs, as with `DirtyTreeQuickInsert`, which the game itself varies from run
to run and only until the next background rebuild. The tree is complete and valid after every single entry, also in the
middle of a pass, and every change is made inside the same call (and lock) as the game's own changes, so nobody can see
a half-finished tree. The old engine does not use this index. `PhysicsInternalFlushParallel` preserves identical results: the two passes write different
structures and only read the bodies. Checks of the tree's layout run before every entry and before the re-link; on any
surprise that tree goes the game's way for the rest of the pass. Self-test: a model of Unreal's tree on the game's
layout, 400 random passes filed both ways: the same entries and sizes, valid in mid-pass, no lost or doubled nodes.

**Measured (2026-10-15, 480p, Kvatch flight to the north cliff, one held game, 3 + 2 runs interleaved):** the physics step's index work below the cliff 33-42 ms instead of 55-58 ms (filing the new
bodies 0.55 instead of 0.95 microseconds per change; the largest catch-up step 7.5-11.8 instead of 12.7-13.5 ms). The
frames there do not get measurably better at 480p (time over 16.7 ms 257-310 vs 262-321 ms, worst frame 52-55 vs
50-51 ms): the main thread's wait for the physics step in those frames is now mostly an earlier, single-threaded part
of the step (the physics side of handing the new bodies over, 70-82 ms per arrival, single frames up to 13-16 ms),
which runs before the index work. `PhysicsPushParallel` targets that intake. At the test PC's own play settings
(4K, frame generation, 60 fps cap, one run each) the same: no change beyond noise. No regression on the Brotch and
Imperial City flights, the same path on foot through the streaming area, and the same image (SSIM 0.98).

---

<a id="physicspushparallel"></a>

### New collision taken in by the physics side in one go (`PhysicsPushParallel`)

[Evidence](research.md#physicspushparallel).

**The problem.** When a wood or rock field streams in (Kvatch's north cliff, the Brotch walk, the Imperial City's stone
bridge and Waterfront), the main thread registers thousands of new collision bodies and hands them to the physics side.
The next physics step starts by taking them in, on one thread, one body at a time: make the body's slot in the physics
world (every one of the world's ~50 per-body lists grows by one, and ~12 overviews of those lists are rebuilt), copy its
shape and position in and work out its bounds, then file it. Below Kvatch's north cliff that is 4,000-5,000 bodies in a
frame, 12-14 ms (60-83 ms over the whole arrival), and the main thread waits for it while the other cores are idle.
`DirtyTreeQuickInsert` and `BulkIndexInsert` made the later part of that physics step quicker; this is the part before it.

*Analogy:* the clerk who books in a delivery opens a new page in every ledger for each parcel separately, and unpacks
each parcel himself; now he opens the pages for the whole delivery at once and has helpers unpack while he writes.

**What we did.** For each run of new static bodies in a hand-over: (1) the game's own code makes all their slots in one
call instead of one call per body, and the overviews are rebuilt once; (2) helper threads copy each body's shape and
position in and work out its bounds, with the game's own functions, while the physics thread goes through the bodies
in the game's order and does everything else exactly as before (filing into the search index, registration, the
physics world's lists); it waits for a body's helper work (or does it itself) when it reaches that body, and skips the
calls the helper already made.

**Why the game can't change.** Nothing the game can see: the slots, numbers and ids come out in
the same order with the same values (the game's own function makes them, just for several bodies at once; in between,
the game's code only works on the bodies already made and reads none of what the batch changes), each body's shape,
position and bounds are computed by the same game functions from the same data (a helper touches only its own body;
the shapes themselves are shared read-only), and everything that is filed, listed or numbered is done by the physics
thread in the game's order, in the same step. Only where in memory things sit and how much spare room the lists keep
differ, which the game does not keep the same from run to run either. A verification test ran the
game's own bounds update again after the helper's and compared the results bit for bit: 35,000 bodies at Kvatch,
0 differences. The self-test runs the game's hand-over loop on stand-ins (new, existing, skipped and moving bodies mixed)
400 times both ways and compares every body and the order of every filing. `PhysicsPushParallel=1` before the game
starts (2 = only the one-call slot making); afterwards it can be switched off and on while it runs.

**Measured (2026-10-15, 480p, Kvatch flight to the north cliff, one running game per comparison, on/off interleaved):**
the physics side's intake below the cliff 26-28 ms instead of 59-60 ms per arrival (the biggest single frame's 6.2-6.5
instead of 12.6-13.6 ms; batching alone, without bounds on the helpers: 33-45 vs 69-83 ms). Frames below the cliff over
five on and five off runs: time over 16.7 ms 272 vs 323 ms (-16 %), frames of 33 ms or more 4.8 vs 6.6 per flight, worst
frame about the same (51-55 ms: what is left there is other work). No regression on the Brotch and Imperial City
flights; the same image (only the animated night sky differs between two captures).
At the test PC's own play settings (4K, FSR Performance, frame generation, 60 fps cap; one fresh game per run, two on, one off):
below the cliff time over 16.7 ms 369 / 374 vs 427 ms, frames of 33 ms or more 4 / 4 vs 6, the intake 30 ms per arrival
(biggest frame 7 ms).


<a id="physicsqueueshrinkonce"></a>

### Collision change list shrunk once (`PhysicsQueueShrinkOnce`)

[Evidence](research.md#physicsqueueshrinkonce).

**The problem.** The main thread keeps a list of every collision change it has made (a body added, moved or removed)
until the physics side has taken it into its search index. Every few frames the physics side hands over a freshly built
index, and the main thread, at the end of its frame, goes through its list: changes the new index already contains are
dropped, the rest are applied. Dropping an entry calls Unreal's "give back unused room" rule for lists, which, once
about 228 entries have been dropped, copies the whole remaining list into a new, smaller block. When a forest streams in
the list holds 9,000-12,000 changes and nearly all of them are dropped: some 45 copies of a list of up to 0.8 MB, each
into fresh memory from Windows. At Kvatch's wood (spot C) that took 4-7 ms of the main thread in each of two or three
frames.

*Analogy:* clearing 10,000 finished tickets off a board, and after every 228 moving the rest onto a smaller board.

**What we did.** While the main thread goes through its list, dropping an entry no longer shrinks the list; when it is
done, the game's own shrink is called once, which gives back the unused room in one step.

**Why the game can't change.** The list holds the same changes in the same order and the same
count at every moment, and the main thread applies the same changes in the same order; only the list's spare room
differs while it is being gone through, and afterwards the game's rule has been applied to the final count. Nothing
keeps a pointer into the list across a drop (the game reads it again each time). The fix finds the one call by its
code shape, checks that it belongs to the function the main thread's hand-over jumps to (the program's own unwind
tables) and that the shrink is the expected one (entries of 72 bytes, the 16 KB rule), else it stays off with a log
line. The self-test runs 300 random hand-overs with and without the fix and compares the applied changes and the list
after every drop. It rides on `BulkIndexInsert`'s hook of the hand-over, so it is installed when the game starts.
`PhysicsQueueShrinkOnce=1` turns it on; it can then be switched while the game runs.

**Measured (2026-10-15, Kvatch flight over spot C, 480p, one held game, five runs each way):** the main thread's
hand-overs at C 1-4 ms in all instead of 11-20 ms; each big one (10,000 changes) under 1-2.6 ms instead of 4-6.7 ms. The
frames that carry them 21-27 ms instead of 24-32 ms. At the test PC's own play settings (4K, FSR Performance, frame generation, two
runs each way) the main thread's hand-overs at C 1.3-1.4 ms in all instead of 9.5-15.1 ms; the frame totals there are
within the run-to-run noise. No change on the Brotch walk.

## Travelling: characters and cells

<a id="npcfadeinnorebuild"></a>

### Characters fade in without being rebuilt every frame (`NpcFadeInNoRebuild`)

[Evidence](research.md#npcfadeinnorebuild).

**The problem.** Every character the game spawns (arriving in a town, stepping out of a door, a guard on patrol coming into range) fades in over about 1.25 seconds with a screen-door "dither" pattern. The remaster stores the fade amount on every piece of the character (body, head, clothes, armour, weapons) and, whenever it changes, tells the engine that piece's whole drawing setup is out of date. The engine then throws that setup away and builds a new one, including a fresh ray tracing copy of the animated mesh. During a fade the amount changes every frame, so every piece of every fading character was rebuilt every frame. On Anvil's docks, where dozens of characters appear when you step outside, that came to about 160 pieces rebuilt some 50 times each and more than a gigabyte of extra video memory for a few seconds; on a 16 GB card it went over the budget Windows gives the game. Unreal then shuffled memory in and out every frame and the game froze for 2-3 seconds.

*Analogy:* repainting a whole wall every time you want it one shade lighter, instead of turning the dimmer.

**What we did.** When the fade amount changes on a piece that is already drawn and has nothing else pending, the DLL writes the new amount into the copy the renderer reads and asks the engine to re-send that piece's per-object data to the graphics card: the route the engine uses for other per-object values a material can read. Anything else (the piece isn't drawn yet, it is already being rebuilt for another reason, the fade switches on or off) goes the game's way, as before.

**Why the image can't change.** The graphics card receives the same fade amount in the same frame; only the needless rebuild is skipped. Held side by side at every fade amount from none to fully invisible, with the fix on and off, the pictures are the same, and a memory trace shows the renderer's copy follows the game's value every frame.

**Effect** (Anvil docks, 4K, FSR Performance, frame generation; four runs off/on/on/off):

| | Off | On |
|---|---|---|
| Game video memory at a dock exit (peak) | 12.9-13.1 GB | 11.2-11.5 GB |
| Seconds over the video memory budget per exit | 2.0-2.75 | 0 |
| Frame rate in the seconds after stepping out | 18.5 / 46.3 fps | 52.3 / 66.9 fps |
| Walking the docks | 78.1 fps | 95.4 fps |

---

<a id="npccreationpacing"></a>

### Far-away characters created a piece per frame (`NpcCreationPacing`)

[Evidence](research.md#npccreationpacing).

**The problem.** The game's world logic (the original Oblivion engine, on its own thread) decides which characters exist; the Unreal side builds them. When you travel and a new strip of land loads, every character in it is created at once: for each one the game thread spawns the Unreal character (body, animation setup, AI), then its head and headwear, and one frame later swaps in the real body and head meshes (a second animation setup), adds each piece of clothing (a robe alone is 14 ms) and the hair. That is 20-40 ms per character, in pieces of 1-15 ms, and all pieces that are due run in the same frame. A shrine with five worshippers 11000 units away (Sheogorath's on the road to the Wayshrine of Stendarr, Peryite's further east) gives 3-6 frames of 45-110 ms.

*Analogy:* a delivery of five flat-pack wardrobes; the shop builds them all in one go while the till queue waits, instead of one between customers.

**What we did.** The pieces get a time budget per frame (by default the time a 60 fps cap would otherwise sleep, else 4 ms). The first piece of a frame always runs; a further piece that does not fit waits for the next frame, but only if its character is far from you (more than one cell, 4096 units). Nothing is skipped and the order is kept: the world logic's messages to the Unreal side are a queue, and when a creation waits, the whole queue waits behind it (usually a few frames: the budget grows once work has waited 8 frames and triples at 15, and anything that has waited 30 frames, half a second at 60 fps, runs at once). The clothing, mesh and hair steps wait in one queue of their own, in their original order. During loading screens, menus, fast travel and doors, and whenever anything does not look as expected, everything runs immediately as before; switching the option off runs whatever waits at once.

**Why the game can't change.** Characters far away (out of sight on our test routes: there were trees and hills in between) are completed a few frames later; for that short time the rest of the world logic's updates to the Unreal side also arrive a few frames late, as they do during the one long frame this replaces. No character near you waits. Like `CellBufferPacedEvict`, this changes timing only; it changes nothing in what is drawn or in what happens in the game (nothing is skipped or reordered, and the quest and script state lives in the world logic, which is not touched). It is on by default and has been used in ordinary play without a problem. If the game ever freezes while travelling, switch it off and report it.

**Effect** (the five-worshipper bursts at Sheogorath's and Peryite's shrines; interleaved runs, option off -> on):

| | Off | On |
|---|---|---|
| Sheogorath's shrine, 854x480: worst frame | 61-68 ms | 36-43 ms |
| same, frames of 33 ms or more | 4-5 | 1-2 |
| Peryite's shrine, 854x480: worst frame | 89-98 ms | 41-51 ms |
| same, frames of 33 ms or more | 4-5 | 4-5 |
| Sheogorath's shrine at 4K, FSR Performance, frame generation, 60 fps cap: worst frame | 79 ms | 25-53 ms |

**What it does not fix (yet).** Some single steps cannot be split: applying a character's real body and head (18-20 ms) or putting on a robe (13-21 ms) still take one frame each, so a burst becomes a series of shorter hitches rather than none.

**Keeping area loading in order.** Each newly loaded area's set-up must wait for all earlier world-logic messages. It waits up to three frames rather than forcing the waiting characters through at once. While each held frame completes a create or head unit from the same world-logic frame, it may wait up to eight frames; without that progress the three-frame limit applies. The game thread's frame-done message waits too, pausing the old engine just as the long frame would. Every message stays in order. On the Silver Road the shorter wait avoided 2-3 creates in a single 25-40 ms frame; on the Echo Mine walk at 4K, allowing continued progress avoided two creates being drained into one ~30 ms frame instead of ~20 ms frames.

---

<a id="animteardownparallel"></a>

### Animation data taken apart on helper threads (`AnimTeardownParallel`)

[Evidence](research.md#animteardownparallel).

**The problem.** Every ~4,100 units of travel the old engine unloads the row of cells behind you and loads the one ahead,
and the game thread waits for it: the short hitch you feel when crossing open country. A large part of the unloading
is the animation data of the NPCs that are left behind: each NPC carries around a hundred animation sequences, each with
up to a few hundred small pieces ("interpolators", the per-bone curves of one animation), all taken apart one after
another on one thread. At the worst crossing measured, 8-10 NPCs' animation data took 14-16 ms of a 37-41 ms row change,
and two thirds of that was destroying those interpolators.

*Analogy:* a removal crew clearing a house where one person carries out every box while four colleagues stand at the van.

**What we did.** When the old engine takes an NPC's animation data apart, each sequence's list of interpolators is
handed to four helper threads instead of being destroyed in place; the old engine carries on with the rest of the
sequence. Before the animation data's destruction returns, the old engine waits until the helpers are done, so every
piece of that NPC is gone before its clean-up goes on, exactly as before. The helpers run the game's own destruction code
on the lists. Only lists the fix has positively recognised are handed over: every interpolator must be one of the four
kinds checked in the game's code (transform, compressed transform curve, float, compressed float curve), held by nothing
else, with no controller attached; anything else is destroyed in place as the game does it.

**Why the game can't change.** Which thread frees the interpolators, and that they are freed
just after the rest of their sequence instead of just before it. Nothing else refers to them (their reference count is
1); their key data, which can be shared with the game's animation cache, is released with the game's own atomic
counters, so whichever release comes last frees it, as in the game. Their destruction touches only their own memory and
the game's thread-safe allocator. The fix finds its three call sites, the classes (by their type information) and their
destruction code by shape and stays out if any one differs.

**Measured (2026-10-17, 480p `small`, Kvatch wall flight, on/off/off/on in one game):** at the heaviest crossing the
NPCs' animation teardown 9.4-10.0 -> 4.9 ms, the row change 30-32 -> 26 ms and the game thread's wait 33-35 -> 28 ms; at
the next one 7.2 -> 4.2 ms of animation teardown; light crossings unchanged. About 1 million interpolators went through
the helpers with no fallback and no crash. It shortens the worst crossings by ~5-7 ms; it does not remove them (the rest
of the row change is other unloading and loading). `AnimTeardownParallel=1` is on by default.

<a id="cellbufferpacedevict"></a>

### Old cells unloaded over a few frames (`CellBufferPacedEvict`)

[Evidence](research.md#cellbufferpacedevict).

**The problem.** Every ~4,100 units of travel the old engine shifts its 5 x 5 grid of loaded cells, and the game thread
waits for that frame (the short hitch when crossing open country). About half of that frame is unloading: not the row
you just left (it stays loaded), but the oldest of the cells the engine keeps around "just in case" behind the grid
(its exterior cell buffer, 36 slots for the default 5 x 5 grid, set by `uExterior Cell Buffer` in Oblivion.ini). The
buffer is kept in "most recently used" order; when the new row's 5 cells (9 on a diagonal) come in, the 5-9 oldest
buffered cells fall off its end and are taken apart on the spot: their buildings, NPCs and animation, all inside the
crossing frame. At Kvatch that is 5-20 ms of a 17-34 ms frame.

*Analogy:* a shelf that holds exactly 36 boxes. To put 5 new boxes on it, someone has to empty and fold 5 old boxes first,
while everyone waits. Give the shelf 9 spare places and the old boxes can be emptied one at a time over the next moments.

**What we did.** When the game makes the buffer at start-up, the fix gives it 2 x uGridsToLoad - 1 spare slots (9 for
the default grid: as many as one grid shift can bring in), the same as a player who sets a bigger `uExterior Cell
Buffer` in Oblivion.ini, a setting the old engine has always had. A crossing now pushes the oldest cells into the spare
slots instead of off the end. Then, in each of the old engine's following frames that do not shift the grid, the fix
unloads one cell from the end of the buffer with the game's own unload call, exactly as the game unloads a cell that falls
off the end: oldest first, on the same thread, at the point of the frame where the game itself shifts and unloads cells.
After 5-9 frames (a tenth of a second) the buffer is as the game would have it. Doors, coc, fast travel, loading a
save and any jump of more than one cell unload the overflow in their own call, as before. The game writes Oblivion.ini
back when it quits; the fix makes it write your own value, so the spare slots are never saved.

**Why the game can't change.** The first 36 slots hold the same cells in the same order as
in the game at every moment (each new cell is inserted the same way; only what falls off goes to a spare slot instead of
being unloaded); the cells the game would unload are unloaded a few frames later, in the game's order. A cell you
turn back to before it was unloaded is found in a spare slot and used again instead of being unloaded and loaded
again, as with a bigger buffer setting. Every part of the old engine that reads the buffer loops over the setting's
value and skips empty slots; nothing assumes a smaller count. The world simulation, the threads and the hand-over
between the game thread and the old engine are unchanged; no frame waits longer. Found by shape (the buffer's
construction, the insert and its unload, the two per-frame grid-change calls, the ini writer); if any one differs the
fix stays out.

**Measured (2026-10-17, Kvatch wall flight, `AnimTeardownParallel` on in every comparison, the fix switched live in one game):**

| | 480p `small`, on/off/off/on | 4K play settings (60 cap, FG), off/on/off |
|---|---|---|
| crossing frame (old engine), mean of 7 crossings | 24.9 / 26.2 -> 13.2 / 14.3 ms | 27.6 -> 13.7 ms |
| game thread's wait in it, mean | 21.9 / 22.6 -> 10.6 / 11.7 ms | 19.9 -> under 4 to 9 ms |
| worst crossing's wait | 31 / 29 -> 12 / 13 ms | 28 -> 9 ms |
| each later unloading frame | +3.5 ms old-engine frame on average (1.2 ms median per cell, one heavy cell 10-13 ms); the game thread's wait in it 6.8 ms mean, 17 ms worst | worst wait 16 ms |
| slow game-thread frames (>= 25 ms) with a wait >= 8 ms | | 7 (worst 46 ms) -> 2 (26, 32 ms) |

What is left of the crossing frame is the incoming row (temporary data load ~3.5 ms, attaching the cells ~3.5 ms) and the
old engine's ordinary frame work. `CellBufferPacedEvict=1` is on by default.

## Doors, loading screens and start-up

<a id="preparecollisionbehindloadingscreen"></a>

### Collision prepared behind the loading screen (`PrepareCollisionBehindLoadingScreen`)

[Evidence](research.md#preparecollisionbehindloadingscreen).

**The problem.** After a door into another area, a city gate or a fast travel, the picture comes back with one long frame (120-230 ms) about a tenth of a second into the fade-in. While the new area loads behind the loading screen, the game keeps the world frozen, and freezing also switches off the physics engine's tick. So the physics engine is never told about the thousands of collision objects the new area brings (walls, rocks, furniture, trees). When the world starts again, the first physics step has to take all of them in at once: create each one and sort it, one by one, into the lookup structure that finds what touches what. The game waits for that, in a single frame, while you watch the fade-in.

*Analogy:* a librarian who isn't allowed to shelve anything while the delivery trucks unload, and then has to shelve the whole delivery before the library can open.

**What we did.** Just before the game starts the fade-in, while the world is still frozen behind the loading screen, we ask the physics engine for a "flush": a tick with a time step of zero. That's the engine's own way of saying "take all pending changes and update your lookup structures, but don't move time forward". Unreal uses the same zero-time tick in its editor every frame and to flush a physics scene. The work runs on spare CPU cores while the game is still busy behind the loading screen, and when the game's own last loading step is done we also refresh the copy of the lookup structure that the game uses for line-of-sight and collision checks, just as the end of every physics frame does.

**Why the game can't change.** With a time step of zero nothing is simulated: the engine's step loop needs time to pass, and the engine itself skips all physics callbacks for a zero tick. Nothing moves, falls, collides or wakes up, no collision or hit event fires, and nothing is reported back to the game. The world, the fade-in and your controls start at exactly the same moment as before, because the fix doesn't touch the freeze, the timers or the fade. It only runs for a frozen door, gate or fast-travel transition (not for menus, interiors that are already running, or loading a save), and only with the engine's normal (synchronous) physics. The DLL checks every piece of game code it relies on, including that the freeze really is what switches physics off. The objects and views after arriving looked the same in screenshots with the fix on and off.

**Effect** (wawnet inn exits, Cheydinhal city gate, fast travel; same game session, fix switched on and off):

| Setting | Frame as the world starts, before | After |
|---|---|---|
| 720p, FSR Performance | 144-199 ms (median 164) | 29-60 ms (median 37-41) |
| 4K, FSR Performance + frame generation | 123-136 ms (median 132) | 18-26 ms (median 21), no slow frame left in the fade-in |
| Fast travel (720p / 4K) | 86-147 / 77-129 ms | 36-59 / 19-36 ms |

The extra work behind the loading screen takes 2-14 ms of game-thread time plus 3-12 ms for the refresh, so the picture comes back as early as before.

---

<a id="psoprecompilefast"></a>

### Start-up shaders built faster in menus (`PsoPrecompileFast`)

[Evidence](research.md#psoprecompilefast).

**The problem.** After every start the game builds about 18,000 graphics pipelines (each a combination of shaders and
render states it will draw with: 15,551 from the game's own list, then 2,536 from a second list, most likely the one the
game records on each PC). Unreal
does this in the background a few per frame, at a pace set by a "batch mode": Fast (up to 50 per frame, 16 ms of
render-thread time) or Background (here 5 per frame, 1 ms). The game asks for Fast only while it loads a map the old
way and for Background after every map load; since the main menu, loading a save, doors and fast travel all use the
seamless kind of travel, the whole precompile runs in Background: from ~10 s to ~110-150 s after launch, through the
main menu, the first load and the first one to two minutes of play. While it runs, the game holds every door and
fast-travel loading screen up to 5 seconds longer (its `FadeInExtraDurationPSOCompilation`).

*Analogy:* a restaurant that preps the whole menu only in the quiet minutes between orders, even while the dining room
is still closed, so the first guests wait at the door until prep is finished.

**What we did.** The fix wraps Unreal's `SetBatchMode`. It remembers what the game asks for, and while the precompile
runs and the player is not playing (start-up videos, main menu, loading screens, menus: the game's own freeze state,
which every menu and loading screen sets) it gives Unreal Fast instead of Background. A timer on the game thread looks
four times a second and hands the game's own Background back the moment play starts or the precompile ends. Fast and
Precompile requests of the game pass unchanged, and pausing or resuming stays the game's.

**Why the game can't change.** Only when the pipelines are built: the same ones, by the same
code, sooner. Nothing else is touched. In play the pace is the game's own; with a cold driver cache (after a driver
update) the precompile takes much longer, and then only the parts behind menus and loading screens go faster. The fix
finds `SetBatchMode` by its code (its three cases checked to store the same batch size), the precompile flag through
Unreal's `IsPrecompiling`, and the freeze state through the loading-screen hook's search; it stays out with a log line
if any of them does not match. The self-test runs the moved entry instructions on the game's code and drives the
wrapper and the timer through every case with fakes (menu, play, loading screen, start-up, the game's own Fast and
Precompile, a request from another thread, the switch, the end of the precompile).

**Measured (2026-10-16, 480p `small`, fresh launches, scenes right after arriving, ABBA on/off/off/on plus stock after
its own precompile):** the precompile finished at 22-25 s after launch instead of 110-148 s (8 s of it at the Fast
pace, during the main menu). The first doors after arriving: the world appeared 0.7 s (inn entry) and 1.6-1.7 s (inn
exit) after arrival, as in stock once its precompile is done, instead of 5.7-5.8 s and 6.6-6.7 s while stock's
precompile was still running. Frame rate in Cheydinhal right after arriving: 42.7-44.5 fps with the fix, 45.0 in stock
after its precompile; stock during its precompile was a little faster (46-47.5 fps; it has not finished building yet),
so the fix costs nothing lasting. Cost: for those ~8 seconds in the main menu, menu frames of up to ~30 ms (p99)
instead of ~4 ms. At 4K with the test PC's play settings (frame generation, 60 fps cap) it finished 27 s after launch (10 s
at the Fast pace), menu frames p99 31 ms meanwhile, and the first doors showed the world 0.7 s / 1.6 s after arrival.
With a cold driver cache (the AMD cache moved away, as after a driver update) the Fast pace ran in the main menu and
during the first load (3,400 pipelines in 27 s, against ~45 in the 15 s at the game's pace before the menu appeared),
the game's pace came back in play, and the precompile ended after ~6 minutes, about as long as a cold start took
without the fix. Start-up videos before the main menu count as play for the fix (no menu freeze yet), so
those seconds keep the game's pace.

## Menus

<a id="displaymodecache"></a>

### Display modes kept for the Graphics page (`DisplayModeCache`)

[Evidence](research.md#displaymodecache).

**The problem.** Opening the menu on the Graphics settings page froze the game for about three quarters of a second at 4K. The menu reopens on the page you used last, so if you adjust graphics, that was every press of Esc. A good part of it: to fill its "Select Monitor" and "Display Resolution" lists, the page has the engine rebuild its picture of your monitors four times, and each rebuild asks Windows for every display mode of every monitor, twice. A 4K monitor reports hundreds of modes (683 on our test PC: every resolution at every refresh rate and colour depth), and Windows answers each one separately, about a tenth of a millisecond each. That's 20-30 ms per list, eight lists per page, more with several monitors.

*Analogy:* reading the whole phone book aloud eight times to find out which towns are in it.

**What we did.** The first time the game reads a monitor's mode list, the DLL keeps a copy. Every later read still starts with Windows: the DLL asks for the monitor's current mode and for a handful of entries spread across the list (the first, a quarter of the way, half way, three quarters, the last, and "is there one more?"), compares them byte for byte with the copy, and only then hands the game the rest from the copy. If anything differs (a new monitor, another resolution, a driver setting that adds modes), the list is read from Windows again, as before. Only the game's monitor-list function gets copies; every other caller asks Windows as before.

**Why the game can't change.** The game receives exactly the bytes Windows gave it: Windows writes the same 188 bytes for a mode whatever was in the buffer before (the DLL checks this the first time and stays off if not). The monitor list, the resolution list and every other value the engine keeps about your displays were read from memory with the fix off and on: identical.

**Effect.** At 4K with frame generation, the open on the Graphics page went from 720-724 ms to 530-554 ms with this fix alone (with `MenuScreenshotFilter` too: below). Each check costs 0.7 ms. The game also rebuilds these lists about 30 times while it starts, which gets quicker too.

---

<a id="menuscreenshotfilter"></a>

### The menu's screenshot filtered in one go (`MenuScreenshotFilter`)

[Evidence](research.md#menuscreenshotfilter).

**The problem.** Every time the menu opens (any page), the game takes a picture of the screen, probably for the save-game picture, and runs it through Unreal's image "resize" function with the target as large as the picture. At the same size that function is a small blur: every pixel becomes the average of itself and its right, lower and lower-right neighbours. Unreal computes it one pixel at a time in high-precision floating point: 139 ms for a 4K picture, on the game's main thread, while the game stands still.

*Analogy:* repainting a wall with a fine brush one tile at a time, instead of with a roller.

**What we did.** The DLL computes the same average with whole numbers, 4 pixels per instruction: 3 ms for a 4K picture. Every other use of the function (a real change of size, or the "linear colour" variant) is left to the game.

**Why the image can't change.** Averaging 1, 2 or 4 colour values of 0-255 and dropping the fraction gives exactly the same number whichever way it's computed, and the DLL's version was checked against the game's own function on 120 test images of 20 sizes (and a 4K one): identical bytes.

**Effect** (4K, FSR Performance, frame generation, opening the menu on the Graphics page; same game session, switched on and off):

| Fixes | Frame as the menu opens |
|---|---|
| neither (as the game ships) | 720-724 ms |
| `DisplayModeCache` only | 530-554 ms |
| `MenuScreenshotFilter` only | 550-573 ms |
| both | 390-426 ms |

What is left is the game's own work: it rebuilds the cloth of your character for the menu view, waits for the graphics card to hand over the screenshot, and builds the menu's widgets.

---

<a id="screenshotreadbackconvert"></a>

### Screenshot colours converted four pixels at a time (`ScreenshotReadbackConvert`)

[Evidence](research.md#screenshotreadbackconvert).

**The problem.** Every time the player menu opens (inventory, map, magic, ...) the game takes a picture of the screen.
The picture is read back from the graphics card and turned into ordinary 8-bit colours by Unreal, one pixel at a time:
the game's screen stores each colour with 10 bits, and for every pixel Unreal scales the three 10-bit values down to 8
bits and looks up the alpha, writing the four bytes one by one and then reading them back as one number, which the
processor handles badly (it has to wait for the four small writes before it can read the whole). At 4K that is 8.3
million pixels and ~45 ms, while the game waits for the picture: about a quarter of the ~0.18 s freeze when the
inventory opens (`MenuScreenshotFilter` already sped up what the game does with the picture afterwards).

*Analogy:* copying a phone book by writing each digit on its own sticky note and then reading the four notes back as
one number before moving on.

**What we did.** The same arithmetic (multiply by 255, add 512, fold and shift, exactly as Unreal does it) on four
pixels at once with the processor's vector instructions, writing each pixel's four bytes in one go. Only this screen
format is taken over; any other format, and anything unusual (empty or overlapping pictures, sizes beyond what the game's
own loop can index), is left to the game's code.

**Why the game can't change.** Nothing the game can see: every byte of the picture is the same.
Each output byte depends on one 10-bit (or 2-bit) value of the input pixel, and the self-test runs the game's own
conversion and the fix on every possible value of every field (with random other fields), on random pictures of many
sizes and row layouts, and at 4K: identical. In the game, the check mode (`2`) also runs the game's conversion on every
real screenshot and compares: no difference in any. When the game starts, the fix finds the one call by its code shape
and checks, through the function's own tables, that the case for this format is exactly the loop described above;
otherwise it stays off with a log line. `ScreenshotReadbackConvert=1` turns it on (installed when the game starts; then
it can be switched while the game runs); `2` = on and checks itself (slower).

**Measured (2026-10-15, 4K, the test PC's own play settings, inventory opened at the Cheydinhal canal, one held game, switched live
on/off/off/on, four opens each, plus one more off block):** the frame that opens the inventory 130-148 ms (median 134.5)
instead of 166-195 ms (median 170): -35 ms; the time the screen stood still 78-99 ms (median 80.5) instead of 114-145 ms
(median 115). The conversion itself 40 ms -> 9-10 ms in the game (5 ms offline; the rest is fresh memory being touched for
the first time). Containers open without a screenshot, so they are not affected; nothing changes while a menu is open or
when it closes. Pictures of the inventory and of the world after closing look the same with and without.

<a id="screenshotparallel"></a>

### Screenshots copied on helper threads (`ScreenshotParallel`)

[Evidence](research.md#screenshotparallel).

**The problem.** After `MenuScreenshotFilter` and `ScreenshotReadbackConvert`, most of what is left of the inventory picture's cost is plain copying: at 4K the
picture is 33 MB, and on its way from the graphics card to the menu it is copied about six times (out of the card's
readback buffer, into colours, into a copy the game keeps, through the resize filter, into a texture, and once more when
the texture is set up). Unreal's memory manager gets blocks that big fresh from Windows every time, and Windows hands
out fresh memory one 4 KB page at a time, on first touch: each copy stops ~8,000 times to have a page mapped in. One
thread does all of it while the game waits: 6-9 ms per copy. The resize step also fills its 33 MB output with zeros
first, only to overwrite every pixel right after.

*Analogy:* moving into a new house where every drawer has to be unlocked by the caretaker before you can put anything in
it; one person unpacking waits for the caretaker at every drawer, four people unpacking keep him busy.

**What we did.** A few helper threads (a quarter of the processor's threads, at most four) share these copies: the
thread that needs the copy cuts it into pieces, works on them together with the helpers and only carries on when all
pieces are done, so nothing finishes later than before. The same goes for `MenuScreenshotFilter`'s filter and `ScreenshotReadbackConvert`'s colour
conversion (bands of rows). Only copies of at least 4 MB use the helpers; everything smaller goes straight on as before.
The zero fill before the resize is skipped: Unreal's resize writes every pixel of its output, whatever the sizes, so the
zeros are never seen (it is kept in the two odd cases where that would not hold).

**Why the game can't change.** Nothing the game can see: a copy cut into pieces is the same copy,
and each row of the filtered or converted picture is computed from the same input rows as before. The self-test runs the
patched code paths (including the two small machine-code pieces that replace the readback's row loop and the zero fill)
against the plain versions: the same bytes, nothing written beyond the end. When the game starts, the fix finds each of
its places by its code shape and checks that each call really goes to Windows' memcpy / memset; if anything is not as
expected it stays off with a log line. `ScreenshotParallel=1` turns it on (installed when the game starts; then it can
be switched while the game runs).

**Measured (2026-10-15, 4K, the test PC's own play settings, inventory opened at the Cheydinhal canal, one held game with `ScreenshotReadbackConvert` on,
switched live on/off/off/on, four opens each):** the frame that opens the inventory ~121 ms instead of ~140 ms (median,
-19 ms); the time the screen stood still ~70 ms instead of ~88 ms. Each 33 MB copy now ~3 ms (was 6-9). With `ScreenshotReadbackConvert`:
170 ms -> ~121 ms, screen still ~115 ms -> ~70 ms. Pictures of the inventory and of the world after closing look the
same with and without. Closing the menu is not affected (that hitch is the world's first frame being drawn again).

## Frame generation

<a id="framegenasynccompute"></a>

### Frame generation's GPU work beside the next frame (`FrameGenAsyncCompute`)

[Evidence](research.md#framegenasynccompute).

**The problem.** With frame generation on, the frame rate the game actually renders (before the generated frames are
added) dropped much more than the extra work explains: in Cheydinhal at 4K (FSR Performance, ray tracing, Shadows High,
RX 9070 XT) from 56-59 / 62-68 / 75-80 fps to 45-47 / 48-50 / 57-60 in three town views. The game uses AMD's own FSR 3
frame generation (version 3.1.0). Two things in it add up. The generated frame is computed on the same GPU queue as the
game's frames, in a line between one rendered frame and the next. And AMD's swap chain lets the game run only about one
frame ahead of the graphics card: every frame the game hands over waits until the previous generated frame is finished.
So the card often sits idle waiting for the CPU part of the next frame, and the generated frame's work comes on top.
AMD's plugin has a setting for exactly this, `r.FidelityFX.FI.AllowAsyncWorkloads`, which puts that work on a second
GPU queue where it runs beside the next frame; the game leaves it off, and it can only be set before the game starts its
renderer.

*Analogy:* a kitchen where the dessert for table 1 is plated at the same counter, after the main course for table 1 and
before the main course for table 2 may start, while a second counter stands empty.

**What we did.** The DLL switches that setting on while the game starts: it finds where the game registers the setting
and writes "on" into it as soon as it exists, a few seconds before the renderer and frame generation start. A line
`r.FidelityFX.FI.AllowAsyncWorkloads=0` under `[SystemSettings]` in your own `Engine.ini` still turns it off.

**Effect.** With frame generation on, the same three views rendered 54.6 / 63.5 / 74.7 fps instead of 44.6 / 49.5 /
58.0 (+22-29 %), with better 1% lows and the same frame pacing. The images are the same: the same passes, only on
another queue of the graphics card. Without frame generation nothing changes.

---

<a id="framegenpacingtuning"></a>

### Frame generation pacing after a slow moment (`FrameGenPacingTuning`)

[Evidence](research.md#framegenpacingtuning).

**The problem.** This acts with AMD's FSR 3.1.4 DLL (see [Optional: AMD ML frame generation](../README.md#optional-amd-ml-frame-generation-radeon-rx-9000)); with the game's own FSR 3.1.0 DLL it does nothing.
With frame generation
on, AMD's swap chain holds each rendered frame back a little so that real and generated frames go out evenly. How long
it waits is worked out from the last frames: about half their average time, minus their variance times a "variance
factor", minus a safety margin. After a few long frames, such as arriving below Kvatch's walls while a new area streams
in, that target stays too long for seconds: at Kvatch the game's render thread then waited 17-20 ms per frame inside
AMD's present for about 4 s, and the rendered frame rate stayed near 40 fps, although the game thread and the graphics
card (17-19 ms per frame throughout) were fast again. AMD's own header describes this ("default tuning gets stuck" after
panning to a complex scene) and recommends two other settings, Tuning Set A and Set B. The game's FSR 3.1.0 plugin
never sets them.

*Analogy:* a conductor who sets the tempo from the last few bars keeps the orchestra slow for a while after one
musician stumbled, although everybody is back on time.

**What we did.** When the game creates AMD's frame generation swap chain, the DLL remembers it and passes AMD's
recommended setting through AMD's own configuration call (`ffxConfigure`, key "frame pacing tuning"): 1 = Set B
(safety margin 0.01 ms, variance factor 0.3), 2 = Set A (0.75 ms, 0.1), 0 = left as the game has it (switched back while
running: AMD's defaults, 0.1 ms and 0.1). AMD added that configuration key in SDK 1.1.3. The fix recognises AMD's FSR 3.1.4 DLL by its version; with the game's own FSR 3.1.0 DLL it logs "not supported" and does nothing. It can be
switched while the game runs. The same hook also writes into the log which version runs upscaling and frame
generation (AMD's own query, with the 3.1.4 DLL), so the log shows whether the driver's ML frame
generation is in use.

**Why the image can't change.** Nothing that is drawn; only when the frames are shown. `FrameGenPacingTuning=1` selects Set B by default;
`FrameGenPacingTuning=0` selects the game's own pacing.

**Measured (2026-10-14, 4K, FSR Performance, ML frame generation with the SDK 2.3.0 DLLs, 60 fps cap, Kvatch walls
flight, two games, the key switched while running):** below the walls (spot C) the rendered frame rate stayed at
48 fps instead of 39-45 and was back to normal within 0.5-1 s instead of 3-5 s. The spacing of the frames on screen
was not worse: standing still the same (95th percentile 9.5-9.7 ms against 9.9-11.2 ms), over the whole flight a
little more even (95th percentile 11.0 against 11.7-11.9 ms). The one difference: a hitch the game itself makes (a
rendered frame of 33 ms or more, as often with and without) more often shows as one longer gap instead of being
spread over a few frames, about five more gaps over 25 ms in a two-minute flight.

---

<a id="framegencamerainfo"></a>

### The camera for AMD's ML frame generation (`FrameGenCameraInfo`)

[Evidence](research.md#framegencamerainfo).

**The problem.** This acts with AMD's FSR 3.1.4 DLL on a Radeon RX 9000, as described in [Optional: AMD ML frame generation](../README.md#optional-amd-ml-frame-generation-radeon-rx-9000). Adrenalin's driver upgrade provides ML frame generation (its overlay shows "AMD FSR Frame Generation: Active"). ML frame generation also uses
the camera's position and its up, right and forward directions, which AMD's Unreal plugin sends from version 3.1.4 on.
The game's plugin is 3.1.0 and never sends them, so AMD's library runs ML frame generation with the camera at zero (it
logs "Camera view matrix data is a prerequisite for MLFI provider enablement" and carries on).

*Analogy:* someone asked to draw the in-between picture of a film is told how every point moved, but not where the
camera stood or which way it looked.

**What we did.** Where the game fills in its frame generation call, the DLL attaches that frame's camera, taken from the
same view the game uses and in the same form as AMD's own newer plugin (the view's location and the columns of its view
matrix, in Unreal's units). Every frame it first checks that the view's matrices look like a camera; if not, nothing is
attached, as without the fix. With the game's own FSR 3.1.0 DLL nothing reads it. `FrameGenCameraInfo=1` is on by default
(before the game starts; afterwards it can be switched off and on while it runs). The log counts how
many frame generation calls reached AMD's library with the camera attached.

**What changes.** AMD's ML frame generation gets the inputs it was designed for. How much the generated frames look
different is not measured yet; nothing changes for the frames the game renders itself. With the game's own FSR 3.1.0 DLL this has no effect.

## Crash and bug fixes

Bugs in the game itself, its engine or AMD's library: they also happen without OblivionPerfFix. (Bugs in our own
fixes are fixed in the fix's own section and marked "our bug" in the changelog.)

<a id="namelookupcrashguard"></a>

### A crash in the game's name table (`NameLookupCrashGuard`)

[Evidence](research.md#namelookupcrashguard).

**The problem.** This one is not about speed. The original engine keeps a table that turns names into game objects:
places, sounds, quests, dialogue topics (about 9,500 entries). The console uses it, and so do a few game systems, for
example to find the sound of an object hitting the ground or of opening a body. The table doesn't keep its own copy of
each name: it points at the object's own name text. Indoor cells throw that text away and create it again whenever they
are reloaded, but the table keeps pointing at the old place. After the game starts there are about 1,500 such stale
entries, after a new game about 3,400. Looking one up reads memory the game has already given back, and if Windows has
taken it back by then, the game crashes. In our testing that happened three times in one day.

*Analogy:* an address book that still lists the old address of everyone who moved. Usually the old house is still
standing and empty, so the visit just finds nobody; sometimes it has been demolished, and the visitor falls into the hole.

**What we did.** The table's name comparison is the same comparison as before, but wrapped in a guard: if reading the
stored name fails because that memory is gone, the entry counts as "not this one", which is exactly what such an entry
gives while its old memory still exists. Every other comparison, and every other kind of error, behaves as before.

**Why the image can't change.** A stale entry never matched anything (its text is gone), so skipping it can't change which
object a name finds. The guard only acts in the moment the game would otherwise crash.

**Effect.** With a stale entry deliberately pointed at memory Windows had taken back, the game without the guard crashed
with exactly the recorded crash; with the guard it carried on, and the log counted two lookups kept from crashing.

---

<a id="trapcrashguard"></a>

### Traps let go of characters that no longer exist (`TrapCrashGuard`)

[Evidence](research.md#trapcrashguard).

**The problem.** A crash in the game's own code, not a slowdown. A trap (falling rocks, a swinging log, spikes) keeps a list of the characters it is touching, and after every physics step it hurts and pushes each of them. A character leaves that list when it steps away from the trap, when its body leaves the physics world, or when the character is removed from the game. That last clean-up looks for the trap's list through the place the character is in at that moment, so it can miss: in our report a clannfear was hit by the falling rocks on a bridge in the Kvatch Oblivion gate, thrown far by their push-back (that part is how traps work in Oblivion), and later removed from the game while the rocks' list still held it. The next physics step tried to hurt a character whose memory had been given back, and the game crashed.

*Analogy:* a doorman with a guest list who is told when a guest leaves through the front door, but not when one leaves through the back. He keeps calling the name of someone who has already gone home.

**What we did.** Just before each trap update, the DLL checks every character on each trap's list: is its memory still there, and is it still a character (the game's own type information says so)? One that isn't is taken off the list with the game's own function for "this character stepped away from the trap", and the update runs as before. Characters that are there are never touched.

**Why the image can't change.** Nothing you see: traps hit, hurt and push every character that is there exactly as before. The guard only acts at the moment the game would otherwise crash, and then writes a line to the log naming the trap.

---

<a id="viewrenameguard"></a>

### Views kept up to date when the graphics card's memory is tidied (`ViewRenameGuard`)

[Evidence](research.md#viewrenameguard).

**The problem.** A crash in the game's engine, not a slowdown: Epic's bug UE-217692 ("reproduces very rarely with a crash in residency"), fixed in Unreal Engine 5.5. At the start of every frame the engine tidies the graphics card's memory: it moves live buffers and textures closer together and frees the old place a frame later. Every "view" of a buffer (how a shader sees it) is told about such a move and updated. But a view is created in two steps: it first reads where its buffer is, and only then signs up to be told about moves. A move that happens between the two steps is missed, and the view keeps pointing at the old place; the next time a shader uses it after that memory was freed, the game crashes. The stock game has this race whenever the tidying runs (always, for buffers and textures). We saw it once, on 2026-10-07, during parallel translation.

*Analogy:* a delivery driver writes down your address, and only then subscribes to your change-of-address notices. If you move in between, the parcel goes to your old house for good.

**What we did.** When a new view signs up, the DLL holds the same lock the engine's own "this buffer moved" notice takes, for the whole sign-up, so a notice can no longer slip in between. Before letting go it checks the view against where its buffer is now (and whether the buffer moved within the last second), and if they differ it updates the view exactly the way the engine updates views that were already signed up. Views with a hidden counter (rare) are left as the stock game handles them, as a precaution. Nothing else changes: views that are re-created or have no buffer go straight through.

**Why the image can't change.** Nothing you see (frame-averaged reference images the same as without it). With it on, `RdgParallelTranslate` lets the tidying run alongside parallel translation again (its "frame fence", which orders each frame's start before that frame's translation, as Unreal Engine 5.5 does), so partly used memory pools are compacted again: about 0.2 GB less video memory (720p 8.18-8.35 instead of 8.42-8.54 GB in three fresh launches each; 1440p at a 12 GB card's budget 8.87-8.90 instead of 9.05-9.14 GB). If `ViewRenameGuard` is off or can't be installed, `RdgParallelTranslate` pauses the tidying as before. Measured cost: none (fps and thread times within noise). In 12 test launches, with the tidying forced to move up to 1 GB a frame and an artificial delay to widen the race, the DLL repaired over 56,000 freshly made views of recently moved buffers and the game never crashed; a view actually left stale was never produced, so the crash it removes is rare in this game.

---

<a id="framegencrashguard"></a>

### Frame generation waits for the graphics card instead of crashing (`FrameGenCrashGuard`)

[Evidence](research.md#framegencrashguard).

**The problem.** A crash in AMD's FidelityFX library, which the game ships for FSR 3 frame generation, not a slowdown.
For its work on the graphics card the library keeps 8 command lists per GPU queue and, for every generated frame, takes
one the card has already finished with. It never waits: when the card is still busy with all 8, it uses a list that
isn't there and the game crashes. That needs a long stall on the card, which we saw at 4K with frame generation on and
video memory full, at the moment the world was unloaded (our benchmark's return to the main menu): three crashes, all
at the same instruction (`amd_fidelityfx_dx12.dll+0x1D9BF`). The library is the game's own copy; the stock game has the
same code.

*Analogy:* a cook with 8 plates who takes a clean one for every dish, and when all 8 are still at the tables, serves
the next dish onto the counter.

**What we did.** When the game loads the library, the DLL routes the library's "give me a free command list" calls
through a check: is one of the 8 finished? If yes (every time in normal play), the library's own code runs as before.
If not, the game waits a millisecond at a time until the card has finished one (at most 3 seconds), and then the
library carries on. We reproduced the crash outside the game with the library itself (a GPU queue held back, 8 lists
taken, the 9th crashes at that same instruction), and with the check the 9th waited a fifth of a second and carried on.
AMD's FSR 3.1.4 DLL, which some players drop in to get Adrenalin's ML frame generation upgrade, has the same 8-list pool
(AMD only raised it to 32 in the later SDK 2.x DLLs, which need no check) and the same code compiled a little
differently; the check finds it there too (the crash reproduced at `+0x2016A` in that DLL, and avoided).

**Why the image can't change.** Nothing you see while the card keeps up. During such a stall the game pauses for the moment it takes
the card to catch up instead of crashing, and the log says how long it waited. Only matters with frame generation on.

---

<a id="restorefsrafteralttab"></a>

### FSR and frame generation back after alt-tab (`RestoreFsrAfterAltTab`)

[Evidence](research.md#restorefsrafteralttab).

**The problem.** When the game window loses the focus, the game switches FSR off. It then renders at native resolution, and frame generation stops. When the window gets the focus back, the game switches FSR on again, but only if it thinks no menu is open. Two things go wrong:
- **The game's "a menu is open" flag is wrong most of the time.** It is set when any menu opens, but cleared only when the menu closes from its System (pause) page, or when you go to the main menu or through a loading screen. Open the inventory, the map, the character sheet or the wait menu and close it again, and the game keeps thinking a menu is open. Every alt-tab after that, in plain play, leaves FSR and frame generation off. This is what players usually hit.

- If a menu, or a conversation, really is open when you come back, FSR stays off, and nothing switches it on when you close it.

Either way the game runs at native resolution without frame generation until you change the upscaler in the settings.

*Analogy:* a light that switches itself off when you leave the room and back on when you return, unless a "do not disturb" sign hangs on the door. The trouble is that the sign is only taken down when you leave by one particular door.

**What we did.** The DLL doesn't trust that flag. It uses a signal the game does keep right: every menu, every conversation and the main menu "hold" the world (the game's freeze system keeps a list of who holds it), and let go when they close. After an alt-tab that switched FSR off, the DLL waits until the window has the focus again and nothing holds the world, then switches FSR back on. It changes the same setting, in the same way, as the game does when you come back. Usually that is the moment you come back; with a menu open, it is the moment you close it (checked when a menu closes, and ten times a second while it waits; nothing runs otherwise).

- If you picked another upscaler in the menu meanwhile, the DLL leaves it alone. If you changed the FSR quality or switched frame generation off, your choice stays (the DLL only switches FSR on; frame generation follows your option).
- Esc, Tab, a controller's button, closing from another page of the menu, leaving a conversation, the wait menu, a loading screen, the main menu: all were tested. The game has no "Resume" button (Esc or Tab closes the pause menu).

**Why the image can't change.** It only restores what your settings say: FSR with your quality (50% render scale for Performance), and frame generation if you have it on. The game's own flag is left as it is. Nothing is written to your settings file (`Save_Settings.sav` was byte-identical before and after every test). A check takes about 3 microseconds and only runs while FSR is waiting to come back.

**Effect** (4K, FSR Performance, frame generation, 60 fps cap; Cheydinhal canal):

| | Shown fps / rendered fps |
|---|---|
| alt-tab in play after opening the inventory once, as the game ships | native resolution, no frame generation, until the upscaler is applied again |
| the same with `RestoreFsrAfterAltTab` | 87.8 / 43.7 (FSR and frame generation back at once) |
| pause menu open after the alt-tab (FSR off, as the game ships) | 60 / 60 |
| pause menu closed, with the fix | 82.6 / 39.8 (frame generation back) |

<a id="frozenclothstretchfix"></a>

### Frozen cloth keeps its shape (`FrozenClothStretchFix`)

[Evidence](research.md#frozenclothstretchfix).

**The problem.** A game bug, also without the DLL: now and then a character's robe, tabard or cape is stretched into a long flat strip that reaches back to where the character stood earlier. Cloth that is out of sight or not needed is "frozen": it isn't simulated, it just keeps its shape relative to the body. When such a character is moved and its cloth then switches level of detail (a simpler or finer version of the same cloth), the engine runs one zero-length update that moves the frozen cloth along with the body. That update treats the move like ordinary motion and lets the cloth lag behind by its usual inertia (half the distance for a robe), and with no simulation running afterwards nothing pulls it back. A grey robe moved 10 metres was left 5 metres behind.

*Analogy:* carrying a coat on its hanger across the room, but only the hanger arrives; the coat stays stretched between the two places.

**What we did.** For exactly that update, and only when the cloth is frozen, the DLL first sets the engine's own "teleport" request for the cloth. The engine then moves it all the way with the body, as it already does when a character is teleported. A pending reset still wins, and the engine clears the request itself.

**Why the image can't change.** Only the broken case changes: in tests the same robe stays within its normal drape (under 3 cm) instead of 5 metres off. Cloth that is simulating is not touched. Cloth that was already stretched before the fix straightens out when the game next rebuilds it.

---

## Memory safety and diagnostics

<a id="memoryrelief"></a>

### Memory given back when Windows runs short (`MemoryRelief`, `GpuCrashReport`)

[Evidence](research.md#memoryrelief).

**The problem.** `LumenKeepArrayCapacity` and `UploadMemoryKeep` keep memory on purpose so it does not have to be requested again (up to about 0.5 GB of RAM), and parallel translation uses about 0.7 GB more video memory. That is a good trade on a PC with memory to spare, but not on one that is already short: on the test PC, with other applications open, Windows had under 1 GB of RAM free while the game ran, even at the smallest settings, and the game's GPU crash after 31 minutes at 4K happened with 0.9 GB of RAM and 1.7 GB of commit left. Separately, when the game crashes with "GPU crashed or D3D device removed", nothing on disk says what the graphics card was doing: the engine collects that (Windows' DRED data) but writes it only to a log the released game does not have.

*Analogy:* a lodger who keeps a spare room for convenience should hand the key back when the landlord runs out of rooms.

**What we did.** Once a second the DLL checks Windows' free RAM and commit (including room for the page file to grow). Below 1 GB of free RAM for three seconds, `UploadMemoryKeep` behaves as stock; below 1 GB of commit, `LumenKeepArrayCapacity` does too. `OsPageCache` returns its spare blocks while either shortage applies. Retention resumes after twice that memory has been free for a minute. Upload pools are handed back oldest first over about 15 seconds, rather than all together: after a flight the longest clean-up was 55 ms instead of 111 ms. GPU scene capacity is unchanged, and the player's ini choices still apply. Each change is logged.

No fix is switched off for video memory. Windows moves excess video memory into system RAM, which costs speed; `CommandAllocatorTrim` gives back unused command memory instead. The observed crash was a driver timeout with 0.9 GB of RAM left, rather than an identified video-memory allocation failure. Experiments that paused retention or translation did not reliably bring usage below budget; their measurements are in [research.md](research.md#tried-and-not-shipped).

When the game reports a GPU crash, `GpuCrashReport` finds its graphics device on the crashing thread and logs why the device was removed, the render passes reached by unfinished command lists, any memory page fault, and the last 30 seconds of memory use.

**Why the image can't change.** Nothing you see. While RAM is short, the traversal-stutter gains of `LumenKeepArrayCapacity` and `UploadMemoryKeep` are given up. The crash report only reads and writes the log.

---

<a id="commandallocatortrim"></a>

### Command memory given back after loads (`CommandAllocatorTrim`)

[Evidence](research.md#commandallocatortrim).

**The problem.** Every batch of work the game sends to the graphics card is first written into a "command allocator", a block of video memory the driver hands out (on AMD at least 1 MiB, even for a tiny batch). The engine keeps a stock of these per GPU queue and reuses them, but it never gives any back: the stock stays as large as the most batches that were ever on their way to the GPU at once. Loading an area is the worst case: the engine sends every new mesh and texture to the GPU's copy queue as a separate little batch, each with its own allocator, so after one load the copy stock holds 350-450 allocators, about 0.4 GB of video memory, that are not needed again until the next load. This is how the unmodified game behaves too.

*Analogy:* a delivery firm that hires a van for every parcel during the Christmas rush and keeps paying for all the vans all year.

**What we did.** The DLL watches how many allocators of each queue are in use (the most at once over the last 20 seconds) and how close the game is to the video memory Windows gives it. Once the game is within 2 GB of that budget, it gives the copy queue's spare allocators back about 20 seconds after a load: it takes them out of the stock with the engine's own functions and releases them in the driver, 32 a second, four at a time with short pauses (a long run of releases held up the game's own GPU submissions: frames of 160-220 ms at 1440p; four at a time: frames at their normal length). The emptied places in the stock stay, holding a harmless stand-in; if the engine takes one out again, it gets a fresh allocator made by the engine's own code, exactly as when its stock runs empty (about 0.03 ms each). The other queues (the main one and compute) always keep their peak as in the game: their need comes back with every busy scene, and when the game was over its budget, trimming them made the engine take ~960 new ones from a driver that was already busy moving memory, and the scenes ran slower. Once the game has been over its budget for 10 seconds, the copy pool is trimmed to what the last few seconds needed. With plenty of memory to spare it does nothing.

**Why the image can't change.** Nothing you see: the same commands reach the GPU. When memory is tight, about 0.3-0.5 GB less video memory once an area has loaded (single trims freed 0.27 and 0.50 GB; mean use over six 720p scenes 8.25-8.31 GB instead of 8.48-8.87), which is about what parallel translation costs, and lower peaks after doors (1440p, 12 GB budget: 11.61 instead of the stock game's 11.97 GB at the inn exit).

---

<a id="hitchledger"></a>

### The stutter ledger (`HitchLedger`)

[Evidence](research.md#hitchledger).

**What it is for.** This diagnostic is off by default. A benchmark scene answers "is this spot faster?", not "what makes the hitches I meet in normal
play?". The ledger answers the second: switched on, the DLL notes for every slow main-thread frame (25 ms or longer,
`HitchLedgerMs`, the frame-rate cap's wait included) where its time went, and `tools/stutter_ledger.py` turns a
session into a table of causes ranked by the milliseconds they cost per hour of play.

*Analogy:* a stopwatch at each station of an assembly line that only writes a note when a car took too long, so that at
the end of the week you know which station made most of the delays.

**What it measures.** The frame from one end of the cap's wait to the next, cut where the engine itself switches work:
before the world update (loading of streamed objects, travel), the actor update (the wait for the old engine's thread,
the tick groups, one of which is the wait for the physics step), the rest of the frame (garbage collection, menus,
drawing, and the wait for the render thread at the frame's end), and the cap's wait. Inside these it adds what the
other fixes already time: NPC creation (`NpcCreationPacing`), new collision bodies (`ParallelStaticBodies` and `PipelinedBodyCreation`), the loading screen's hand-over
(`PrepareCollisionBehindLoadingScreen`), the old engine's cell-grid change (a cell-row crossing), the GPU's frame time and the start-up shader
precompile's progress. It also notes whether a menu or a loading screen was up, whether the game had the focus, and
the player's position. Every 10 seconds it adds a summary of all frames (how many, how long, in which state), so the
slow frames can be counted per hour.

**Why it does not change the game.** It only reads the clock around calls the game makes anyway (three of them get a
small detour or a redirected call at start-up, and only when the switch is on at start-up) and reads a few counters.
The main thread writes nothing to disk: a slow frame's note goes into a fixed list in memory, and the DLL's own
background thread writes the list to `OblivionPerfFix-ledger.csv` (next to the log, at most 48 MB, the previous session
kept as `.1.csv`) once a second. Cost: about 0.15 microseconds per frame in the self-test; in-game overhead has not been quantified here.
Garbage collection has no cheap anchor in this game build, so it is not timed on its own (it shows up in the frame's
rest). Where a piece of the game's code is not found, its column is left empty (-1) and the log says so.

**How to use it.** `HitchLedger=1` in `OblivionPerfFix.ini`, then start the game; `tools/Record-Session.ps1` copies the
file into its capture and writes `ledger.md`; or run `python tools/stutter_ledger.py <Win64 folder or csv>`. With the
switch at 0 at start-up nothing is installed; once installed, the switch stops and restarts the recording while the
game runs.

## All together (Cheydinhal, 720p, FSR Performance, frame generation off)

This early-build table shows the result of removing the rendering bottlenecks: the game's own logic thread
then limited the frame rate in these views.

| View | Before | After | Change |
|---|---|---|---|
| Canal, noon | 30.6 fps | 42.7 fps | +40% |
| Chapel square, noon | 44.7 fps | 56.6 fps | +27% |
| Bridge Inn front | 53.3 fps | 64.9 fps | +22% |
| Canal, dusk | 32.2 fps | 43.9 fps | +36% |
| Canal, night | 34.1 fps | 48.6 fps | +43% |

1% lows improved by 30-46%. This 720p Cheydinhal table was measured with an earlier build using
`BoneUploadStores`, `AtmosphereDirtyFilter`, `AtmosphereLightFastPath` and `RdgParallelTranslate`.
`RdgSubmitBatch` lowers GPU time but does not change fps in these views.

**How we checked that nothing looks different:** frozen, 8-frame-averaged reference images with the fixes on and off, compared automatically. That covered 13 conditions: dawn, noon, dusk, night, clear, cloudy, fog, rain and thunderstorm. Every brightness difference was within about 2%, the same size as the gap between two runs with identical settings.

**Travelling:** `LumenKeepArrayCapacity`, `UploadMemoryKeep` and `PhysicsTreeTimeSlicing` remove the two memory round-trips and the physics catch-up behind most hitches while travelling across the landscape, `NavmeshBuildNoBusyWait` the navigation-tile waits and `GpuSceneKeepCapacity` the GPU list re-creations; `NpcCreationPacing` spreads the burst when the game creates a group of NPCs at once as their area loads (for example Sheogorath's shrine, seen from the road to the Wayshrine of Stendarr). Still open: single steps of one character (a body or a robe, 17-27 ms each) and the first growth of the GPU's object lists to a new size. The long frame as the picture comes back after a door, a city gate or a fast travel is fixed by `PrepareCollisionBehindLoadingScreen`; the big stalls of a transition happen behind the loading screen, where you don't see them.
