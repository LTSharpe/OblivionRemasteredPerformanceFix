# Changelog

Downloads, checksums and build attestations: https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/releases

<!-- Each release has a section named after its tag without the "v" (tag v1.0.0-beta -> "## 1.0.0-beta"); the build
     refuses to publish a tag that has no section here, and uses the section as the release notes. Put changes that
     are not released yet under "## Unreleased" and rename it when tagging. -->

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
- **Crash fixes:** the game's name table and trap code, Unreal Engine's video-memory tidying (UE-217692), AMD's frame
  generation library; FSR and frame generation come back after an alt-tab.
- **Safety:** memory kept on purpose is handed back when Windows runs short; spare GPU command memory is given back when
  video memory gets tight; parallel translation stays off on graphics cards under 9.5 GB; a GPU crash writes a report
  into the log.
