# Changelog

Downloads, checksums and build attestations: https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/releases

<!-- Each release has a section named after its tag without the "v" (tag v1.0.0-beta -> "## 1.0.0-beta"); the build
     refuses to publish a tag that has no section here, and uses the section as the release notes. Put changes that
     are not released yet under "## Unreleased" and rename it when tagging. -->

Bug fixes say whose bug it was: **game bug** = also happens without OblivionPerfFix (in the game, its engine or a
driver library); **our bug** = caused by an earlier version of OblivionPerfFix.

## Unreleased
- **Game bug fixed: the game stopping in big fights behind a "Havok picks" box** (`HavokPickWarningGuard`; happens
  without OblivionPerfFix too): in fights with many characters the game's old engine can pop up a leftover debug
  message, "42 Havok picks (6 path, 36 LOS, 0 ViewCaster) used this frame.", on its own thread. The whole game then
  stands still, often behind its own window, until someone finds and closes the box (our battle benchmark lost 2 of
  10 runs to it). The box only reports that the engine skipped some sight and path checks that frame, which it does
  with or without the box, so the message now goes to the log instead and the game keeps running. Nothing else
  changes: the engine's limit on those checks (`iMaxPicks` in Oblivion.ini) is left alone.

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
