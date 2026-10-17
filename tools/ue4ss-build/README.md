# UE4SS for the benchmark: how the packaged build is made

The benchmark runs its Lua mod (`mods/ue4ss/OblvBench`) inside the game with [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)
(MIT licence). It is loaded only for benchmark games (see docs/benchmark.md); it is
not part of the performance fix.

The package `UE4SS_v3.0.1-1161-g6eb3d9bc-oblvfix1.zip` is the upstream experimental build 1161 (commit `6eb3d9bc`,
2026-10-06) with one fix:

- **Lua 'Out' parameter leak** (upstream issue [#1446](https://github.com/UE4SS-RE/RE-UE4SS/issues/1446)): every Lua
  call of a UFunction with an 'Out' parameter took a registry reference to the 'Out' table and never released it, so
  the table and everything converted into it stayed in memory for the rest of the session. A mod that calls such a
  function every frame grows the game's memory by about 1 GB per hour. The fix releases the reference once the table
  is back on the stack: `0001-fix-Lua-release-out-param-registry-refs.patch` (offered upstream as
  [pull request #1452](https://github.com/UE4SS-RE/RE-UE4SS/pull/1452)).

Everything else in the zip (settings, default mods, licence) is the upstream 1161 release zip, unchanged; only
`dwmapi.dll` and `ue4ss/UE4SS.dll` are rebuilt. `BUILD-INFO.txt` in the zip lists the source commits, the
compiler and the SHA-256 of every file that differs from upstream.

## Rebuilding it

You need: Windows, Visual Studio 2022 C++ build tools, CMake >= 3.22, a Rust toolchain >= 1.73 (stable MSVC), git, and
access to the private `Re-UE4SS/UEPseudo` repository (link your GitHub account to an Epic Games account; see the UE4SS
README). About 2 GB of disk for the build.

```bat
git clone https://github.com/UE4SS-RE/RE-UE4SS.git
cd RE-UE4SS
git checkout 6eb3d9bc
git submodule update --init --recursive
git am <this folder>\0001-fix-Lua-release-out-param-registry-refs.patch
cmake -B build_vs -G "Visual Studio 17 2022" -A x64
cmake --build build_vs --config Game__Shipping__Win64 --target UE4SS proxy
```

The DLLs are in `build_vs\Game__Shipping__Win64\bin\`. Then put them into the upstream zip's layout:

```powershell
tools\ue4ss-build\Package-UE4SS.ps1 -ReleaseZip UE4SS_v3.0.1-1161-g6eb3d9bc.zip -BinDir <RE-UE4SS>\build_vs\Game__Shipping__Win64\bin
```

MSVC builds are not bit-for-bit reproducible (time stamps, PDB ids), so a rebuild has other SHA-256 hashes than the
published zip; it is the same code.

### What the published zip was built with (2026-10-07)

- `UE4SS_v3.0.1-1161-g6eb3d9bc-oblvfix1.zip`, 8,692,123 bytes, SHA-256
  `C605C22AA02601CE5A98C6824FEB4BE88A286CB63A341EE976E024BB29549E84` (pinned in `tools/Setup-Bench.ps1`); inside it
  `ue4ss/UE4SS.dll` SHA-256 `594768E5AEF357914085AEC1A728ED8063DE0E97F1E0B697A54ABFD4F28C1140` and `dwmapi.dll`
  `1219EB50AA016C0F34676901EA76D580D77E56754DA4F7D94FAC0B288AA8EE2F`. Upstream zip it is based on:
  `UE4SS_v3.0.1-1161-g6eb3d9bc.zip`, SHA-256 `938DC9901E6452CAD3120280681C93A00AE12DF2B259668815A150E221A35563`.
- UE4SS `6eb3d9bc` + the patch above; submodules UEPseudo `38e7171e`, patternsleuth `1d90b02c`.
- MSVC 19.40.33811 (toolset 14.40, VS 2022 17.10 Build Tools), CMake 4.4.4, Rust 1.99.0 (stable, minimal), VS generator.
- UE4SS asks for MSVC toolset 14.43 or newer. With 14.40 the build needs `-DUE4SS_VERSION_CHECK=OFF` and two
  syntax-only changes for features 14.40 lacks, none of which changes what the code does:
  - `msvc-older-than-14.43-ue4ss.patch`: `[&]() -> bool` instead of `[&] -> bool` (a lambda without a parameter list,
    C++23) in `UE4SS/src/Mod/LuaMod.cpp`;
  - in UEPseudo (private, so not included here): `typename` added before `TDetourTraits<...>::Impl` in
    `include/Unreal/Hooks/Internal/DetourInstance.hpp` and `DetourSubclasses.hpp` (C++20 lets it be left out; 14.40 does not).
  With MSVC 14.43 or newer neither change is needed.
