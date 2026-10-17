# Porting a fix to a new game build

The game updated and `OblivionPerfFix.log` says a fix is `not found`, `disabled` or `not installed`. Nothing is broken:
that fix left the game's code alone and the game runs as if it weren't there. This page is how to bring it back. It
assumes the build tools from [AGENTS.md](../AGENTS.md#build-test-install) and some x64 assembly reading.

The fix was made against Steam build **19115871** (exe TimeDateStamp `0xF19077A4`). Every `exe+0x...` address in the
source comments refers to that build.

## 1. See what moved (2 minutes, no compiler)

```powershell
python tools\find_patterns.py               # finds the installed Steam exe; or pass the exe path
```
It lists every byte pattern in `src/OblivionPerfFix/src/` (all area folders) with its number of matches in the new exe, next to the count
on the tested build (`tools/pattern-baseline.tsv`). Only the `CHANGED` lines need attention:
- **1 -> 0:** the code changed (a new compiler layout, a changed offset, or the function was rewritten).
- **1 -> 2+:** the pattern is no longer unique: usually a longer pattern fixes it.
- Patterns that always matched several places (short ones checked at a known address, one bone loop per CPU type) only
  matter if their count changed.

Then the authoritative check, the self-test against the new exe:
```powershell
src\OblivionPerfFix\build.ps1 -SelfTest                 # the installed game
src\OblivionPerfFix\build.ps1 -SelfTest -Exe <path\to\exe>   # or any copy of an exe
```
`FAIL` lines of the form `... at exe+0x...` only pin the old build's addresses; they fail on every new build. Look at the
"found", "applied", "hooked" checks first. A fix that was not found shows one `FAIL <fix> ... not installed` line
and its remaining checks are skipped; the end of the output repeats each fix's own log lines, the same lines the game's
`OblivionPerfFix.log` would show.

## 2. Read what the fix expects

Open the fix's file (index in AGENTS.md). The comment at the top says which game function it changes and why the
result stays identical; the comment above each pattern shows the instructions it expects, with the old addresses. Note
which parts are literal (the instructions the fix depends on) and which are already wildcards (displacements, stack
offsets).

Example, `RdgParallelTranslate` (`render/parallel_translate.cpp`): one pattern of 34 bytes, the last four of which are `setne r8b`
(`41 0F 95 C0`), which the fix replaces with `mov r8b,1 ; nop`. If the pattern is not found but those four bytes still
exist in the same function, only the surrounding instructions changed.

It has a second, required part: `render/translate_defrag_guard.cpp` pauses the engine's D3D12 buffer-pool
defragmentation while parallel translation is on (Unreal 5.3 bug UE-217692). It finds the console variable
`d3d12.VRAMBufferPoolDefrag` by its name in `.rdata` and its registration (`lea r8,[variable]; lea rdx,[name]`), then
checks that exactly one start-of-frame read of that variable is followed by a read of
`d3d12.VRAMBufferPoolDefrag.MaxCopySizePerFrame` (`cmp dword [variable],0; jle; mov esi,[maxCopy]; test esi,esi`). If
any step fails, the log says `[rdg] defrag guard: ... guard unavailable` and parallel translation stays off. When
porting, update both patterns; never let parallel translation run without the pause (or, with `ViewRenameGuard`
on, without the frame fence in `render/translate_frame_fence.cpp`).

## 3. Find the same code in the new exe

Work from anchors that survive updates, cheapest first:
1. **A shorter version of the old pattern.** Drop the outer bytes and wildcard displacements:
   `python tools\find_patterns.py --find "41 0F 95 C0 4C 89 AD"` (the instruction plus the next few bytes: one match
   on build 19115871, at `exe+0x26B9B08`).
2. **A neighbour that is still found.** Many fixes match several patterns in the same function or its callers; a
   pattern that still matches gives you the area (`--dump 0x<rva> --len 256` shows the bytes there).
3. **Strings and constants.** Some fixes start from a string or an import (`DisplayModeCache` from
   `EnumDisplaySettingsW`, `NameLookupCrashGuard` from `_stricmp`); search for them in a disassembler.
4. **The engine source.** The functions are Unreal Engine 5.3 (named in the comments); reading the public source tells
   you what the compiled code must contain (calls, constants, member accesses).

To read the code around a match, any disassembler works: x64dbg, Ghidra, IDA Free, or the one in the Visual Studio
Build Tools you already have:
```bat
dumpbin /nologo /disasm /range:0x1426B9A00,0x1426B9C00 OblivionRemastered-Win64-Shipping.exe
```
(addresses are the image base `0x140000000` plus the RVA).

## 4. Update the fix

- Change the pattern so that it matches the new build **and** still means the same thing. Keep literal every byte the
  fix's correctness depends on (the instruction it replaces, the call it redirects, the member offset it reads); wildcard
  only what can change without changing meaning (RIP displacements, stack offsets, register allocation the fix does
  not rely on).
- If a struct offset moved, update the expected value in the fix's checks (most read it from the matched code and
  compare it with the expected one; that check exists so a different layout switches the fix off instead of
  corrupting memory).
- If the function was rewritten or the game fixed the problem itself, retire the fix: leave its code, make it log
  why it is off on that build, and say so in `CHANGELOG.md`.
- Supporting both the old and the new build is good when it costs nothing more than an alternative pattern; it is not
  required.

## 5. Verify

1. Self-test: 0 failures. Update the `exe+0x...` expectations in `tests/selftest.cpp` to the new build's addresses
   once the fix is confirmed there.
2. Start the game with the new DLL; the log must show the fix applied (`hooked`, `patched`, `installed`, `at exe+0x...`)
   and no `disabled` line for it.
3. Measure and compare images with the benchmark ([benchmark.md](benchmark.md)): `tools\Bench-Report.ps1` for stock
   vs fix, plus a visual pass and `tools\Compare-Benchmark.ps1` for anything that touches rendering (the `render/` fixes).
   `PrepareCollisionBehindLoadingScreen` shows in the door scenes, the `menus/` fixes when opening a menu,
   `RestoreFsrAfterAltTab` after alt-tab (in play after using the inventory, and with a menu open). That one reads the
   freeze subsystem that `loading/fade_hook.cpp` finds: if the `[fade]` code is not found, it logs that it falls back to
   "pause or main menu only", so port `fade_hook.cpp` first.
4. Record the new baseline: `python tools\find_patterns.py --save tools\pattern-baseline.tsv`.
5. Bump `OPF_VERSION` in `core/common.h` and `pluginVersion` in `core/main.cpp`, add a `CHANGELOG.md` entry naming the game
   build, and open a pull request with the log and the report.

## Things that are easy to get wrong
- A pattern that matches once is not proof: the checks after the search (call targets, offsets, vtable layouts) are
  what keep a wrong match from being patched. Don't remove them to make a fix "work".
- Patched bytes in memory differ from the file on disk. `find_patterns.py` and the self-test read the file; a debugger
  attached to a game running the fix sees the patched code.
- The Game Pass exe (`Binaries\WinGDK`) is a separate compile: port it from its own exe, not from Steam addresses.
