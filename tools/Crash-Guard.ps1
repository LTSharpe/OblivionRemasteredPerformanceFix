<#
.SYNOPSIS
  Keeps the crash UI of one benchmark game off the PC user's screen (OblvBench.CrashGuard in BenchLib.cs) for as long
  as that game runs and 20 s after: UE's crash reporter and Windows error reporting for the game are ended at once,
  AMD's bug report tool hidden and closed, the game's message boxes moved off-screen and closed after 5 s. Backs up
  the hook DLL in the game (tools\benchhook), which keeps the crash reporter from starting at all.

  Started hidden by BenchBridge's Start-CrashGuard (Start-Game, and every run that finds a game running), one per
  game process (crash-guard.pid in the runtime folder: "<guard pid> <game pid>"); it also covers hand probing between runs.
  Log: crash-guard.log in the runtime folder (run\ by default). See docs\benchmark.md, "Staying out of the way".
#>
param([Parameter(Mandatory)][int]$GamePid, [int]$LingerSec = 20)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BenchBridge.ps1')
[OblvBench.CrashGuard]::LogPath = Join-Path $script:BenchRun 'crash-guard.log'
[OblvBench.CrashGuard]::LingerSec = $LingerSec
[OblvBench.CrashGuard]::Run($GamePid)
