<#
.SYNOPSIS
  Dead-man switch for one Run-Benchmark.ps1 process (2026-10-20, after a runner was killed while it was starting the
  game: Steam finished the launch with nobody hiding or muting the game, and it appeared on the PC user's screen).

  Started hidden by Run-Benchmark.ps1 once per runner process, right after it takes game.lock (runtime folder). It only waits
  on the runner's process handle until that process ends. Then:
    - game.lock no longer names the runner's pid (a normal end unlocks first, or the runner never had the game):
      nothing to do, exit.
    - it still does (the runner died holding the game): the runner's own game is the one recorded for it, either the
      game pid in bench-launch.json / bench-status.json (same runner pid, process still there and started before the
      runner died), or, if the runner died while launching (bench-launch.json: "launching since T", written before it
      asked Steam), the first game process that appears within -LaunchWindowSec of T. Any other game (one the PC user
      starts, or one a new runner took: a game.lock with another live pid) is left alone; when in doubt nothing is done
      and the reason is logged.
      For that game: the same protections as a runner (window hook, window kept off-screen and the focus given back,
      Steam's launch dialog hidden, muted), then, once the mod answers (at most -ModWaitSec, so it can be silenced
      before it quits), `Run-Benchmark.ps1 -Close -Force` (closes it, restores config, saves, the mute and UE4SS) and
      the stale lock is removed. If the PC user has the game in front and is using it (keyboard or mouse input within
      15 s), it is shown and un-muted and never closed.
  -Visible (on-screen runs that close their game at the end, e.g. the benchmark kit; 2026-10-24): nothing is hidden or
  muted and the player-input check is skipped (the game is a throwaway benchmark game). The guard acts whenever the runner
  ended without its normal end (runtime\runner-clean.txt names it; Ctrl+C runs the runner's finally, which unlocks
  game.lock, so the lock cannot tell): its recorded game (or the one its launch brings within -LaunchWindowSec) is given
  up to -ModWaitSec to answer, then `Run-Benchmark.ps1 -Close -Force` closes it and restores config, saves and UE4SS;
  with no game it still runs -Close -Force for the restores. A game.lock held by another live runner: left alone.
  Log: runner-guard.log in the runtime folder. docs\benchmark.md, "Staying out of the way".

  -Test (offline tests): other process names and files, the hook in the tools
  mapping, and -CloseAction kill instead of Run-Benchmark -Close.
#>
param(
    [Parameter(Mandatory)][int]$RunnerPid,
    [double]$LaunchWindowSec = 120,      # a launch the runner asked for counts only if its game process appears within this
    [double]$ModWaitSec = 90,            # how long to wait for the mod before closing anyway
    [switch]$Audible,                    # the runner kept the sound (-Audible): do not mute
    [switch]$Visible,                    # an on-screen runner (see .SYNOPSIS): clean up after any unclean end, hide nothing
    # ---- tests only
    [switch]$Test,
    [string[]]$GameNames,                # process names (no .exe) to treat as the game
    [string[]]$SteamNames,
    [string]$LockFile, [string]$LaunchFile, [string]$StatusFile, [string]$LogFile,
    [ValidateSet('bench', 'kill', 'none')][string]$CloseAction = 'bench'
)
$ErrorActionPreference = 'Stop'

# ---- phase 1: wait (BenchLib is compiled now, ~0.6 s, so the protections can start within milliseconds of the runner's end)
$root = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot 'GamePaths.ps1')
$run = Get-BenchRuntimeDir $root
if (-not $LockFile) { $LockFile = Join-Path $run 'game.lock' }
if (-not $LaunchFile) { $LaunchFile = Join-Path $run 'bench-launch.json' }
if (-not $StatusFile) { $StatusFile = Join-Path $run 'bench-status.json' }
if (-not $LogFile) { $LogFile = Join-Path $run 'runner-guard.log' }
function Log([string]$m) { try { Add-Content -LiteralPath $LogFile ("{0:yyyy-MM-dd HH:mm:ss.fff} [guard {1} for {2}] {3}" -f (Get-Date), $PID, $RunnerPid, $m) -Encoding utf8 } catch {} }
function Read-Json([string]$path) { try { if (Test-Path -LiteralPath $path) { Get-Content -LiteralPath $path -Raw | ConvertFrom-Json } } catch { $null } }
function Get-LockPid { try { if ((Test-Path -LiteralPath $LockFile) -and ((Get-Content -LiteralPath $LockFile -Raw) -match 'pid=(\d+)')) { [int]$Matches[1] } } catch { $null } }

$runner = $null
try { $runner = [Diagnostics.Process]::GetProcessById($RunnerPid) } catch { Log 'runner already gone at start: nothing to guard'; exit }
. (Join-Path $PSScriptRoot 'BenchBridge.ps1')
Log 'watching'
$runner.WaitForExit()
$died = Get-Date

if ($Visible) {
    $clean = Join-Path $run 'runner-clean.txt'
    if ((Test-Path -LiteralPath $clean) -and ([IO.File]::ReadAllText($clean)).Trim() -eq "$RunnerPid") { Log 'on-screen runner reached its normal end: nothing to do'; exit }
    $lp = Get-LockPid
    if ($lp -and $lp -ne $RunnerPid -and (Get-Process -Id $lp -ErrorAction SilentlyContinue)) { Log "game.lock names another live runner (pid $lp): left alone"; exit }
    Log 'on-screen runner stopped before its normal end (Ctrl+C or window closed): cleaning up'
    $launch = Read-Json $LaunchFile
    $game = $null
    if ($launch -and [int]$launch.runnerPid -eq $RunnerPid) {
        if ($launch.gamePid) { $game = Get-Process -Id ([int]$launch.gamePid) -ErrorAction SilentlyContinue }
        elseif ($launch.launchAt) {      # stopped while Steam was starting the game: wait for it, then close it
            $at = [datetime]$launch.launchAt
            while (-not $game -and ((Get-Date) - $at).TotalSeconds -lt $LaunchWindowSec) {
                $game = Get-Process $script:GameProcessName -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $at.AddSeconds(-2) } | Select-Object -First 1
                if (-not $game) { Start-Sleep -Milliseconds 500 }
            }
        }
    }
    if ($game -and -not $game.HasExited) {
        Log "its game: pid $($game.Id); waiting up to $ModWaitSec s for the mod so it can quit cleanly"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while (-not $game.HasExited -and $sw.Elapsed.TotalSeconds -lt $ModWaitSec) {
            if ((Read-BenchLogFrom 0) -match 'LOADED bench\.lua' -and $null -ne (Send-Bench 'ping' -TimeoutMs 1500 -NoThrow)) { break }
            Start-Sleep -Milliseconds 500
        }
    } else { Log 'no game of its own is running' }
    try { & (Join-Path $PSScriptRoot 'Run-Benchmark.ps1') -Close -Force *>&1 | ForEach-Object { Log "  $_" } } catch { Log "  -Close failed: $_" }
    if ((Get-LockPid) -eq $RunnerPid) { Remove-Item -LiteralPath $LockFile -ErrorAction SilentlyContinue; Log 'removed the stopped runner''s game.lock' }
    Log 'done'
    exit
}

# ---- phase 2: did it die holding the game?
$lockPid = Get-LockPid
if ($lockPid -ne $RunnerPid) { Log "runner ended; game.lock names $(if ($lockPid) { "pid $lockPid" } else { 'nobody' }): nothing to do"; exit }
Log 'runner ended while holding the game (no clean end): looking for its game'
if (-not $GameNames) { $GameNames = @($script:GameProcessName) }
$allGameNames = if ($Test) { $GameNames } else { @($script:GameProcesses) }
if (-not $SteamNames) { $SteamNames = if ($Test) { @() } else { @($script:SteamProcesses) } }
function Get-GuardGame { @(foreach ($n in $GameNames) { Get-Process $n -ErrorAction SilentlyContinue }) | Sort-Object StartTime | Select-Object -First 1 }
function Test-NewOwner {          # another live runner (or a hand measurement) took the game: not ours to act on
    $p = Get-LockPid
    if ($null -eq $p) { return $false }
    if ($p -eq $RunnerPid) { return $false }
    [bool](Get-Process -Id $p -ErrorAction SilentlyContinue)
}

$launch = Read-Json $LaunchFile
$status = Read-Json $StatusFile
$known = $null
if ($launch -and [int]$launch.runnerPid -eq $RunnerPid -and $launch.gamePid) { $known = [int]$launch.gamePid }
elseif ($status -and [int]$status.pid -eq $RunnerPid -and $status.gamePid) { $known = [int]$status.gamePid }
$launchAt = if ($launch -and [int]$launch.runnerPid -eq $RunnerPid -and $launch.launchAt) { [datetime]$launch.launchAt } else { $null }

$protected = $false
function Start-Protection {
    if ($script:protected) { return }
    if ($Test) { [OblvBench.GameHider]::UseToolMapping() }
    $dll = Get-BenchHookDll
    if ($dll) { [OblvBench.GameHider]::Start($dll, [string[]]$GameNames) }
    [OblvBench.WindowKeeper]::Keep([string[]]$SteamNames, [string[]]$allGameNames)
    if (-not $Audible) { [OblvBench.GameAudio]::Start([string[]]$GameNames) }
    $script:protected = $true
    Log "protections on (hook $(if ($dll) { 'yes' } else { 'NOT AVAILABLE' }), off-screen keeper, $(if ($Audible) { 'not muted (-Audible)' } else { 'mute' }))"
}
function Flush-Log { foreach ($l in Get-UnobtrusiveLog) { Log "  $l" } }

$game = $null
$g0 = Get-GuardGame
if ($known) {
    if ($g0 -and $g0.Id -eq $known -and $g0.StartTime -le $died) { $game = $g0; Start-Protection; Log "its game: pid $known (recorded by the runner)" }
    elseif (-not $g0) { Log "its recorded game pid $known is gone: nothing to do"; exit }
    else { Log "a game is running (pid $($g0.Id)) but not the recorded one (pid $known): left alone"; exit }
} elseif ($launchAt) {
    if ($g0 -and $g0.StartTime -lt $launchAt.AddSeconds(-2)) { Log "a game older than the runner's launch is running (pid $($g0.Id)): left alone"; exit }
    if (((Get-Date) - $launchAt).TotalSeconds -gt $LaunchWindowSec -and -not $g0) { Log "the runner's launch ($($launchAt.ToString('HH:mm:ss'))) is older than $LaunchWindowSec s and no game came: nothing to do"; exit }
    Start-Protection           # before the game's window exists: the hook goes in as in a run
    Log "the runner died while launching (since $($launchAt.ToString('HH:mm:ss'))): watching for its game until $($launchAt.AddSeconds($LaunchWindowSec).ToString('HH:mm:ss'))"
    while (-not $game) {
        if (Test-NewOwner) { Log 'a new runner took the game lock: leaving the launch to it'; Flush-Log; exit }
        $g = Get-GuardGame
        if ($g) {
            if ($g.StartTime -ge $launchAt.AddSeconds(-2) -and $g.StartTime -le $launchAt.AddSeconds($LaunchWindowSec)) { $game = $g; Log "its game appeared: pid $($g.Id) (started $($g.StartTime.ToString('HH:mm:ss.fff')))" }
            else { Log "a game outside the launch window appeared (pid $($g.Id)): left alone"; Flush-Log; exit }
        } elseif (((Get-Date) - $launchAt).TotalSeconds -gt $LaunchWindowSec) { Log 'no game within the launch window: done'; Flush-Log; exit }
        Start-Sleep -Milliseconds 20
    }
} else {
    if ($g0) { Log "a game is running (pid $($g0.Id)) but the runner recorded no game and no launch: left alone (in doubt)" } else { Log 'no game and no launch recorded: nothing to do' }
    exit
}

# ---- phase 3: keep it hidden and quiet until it answers, then close it cleanly
Add-Type -Namespace OblvGuard -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO i);
public static double IdleSeconds() { var i = new LASTINPUTINFO(); i.cbSize = 8; GetLastInputInfo(ref i); return ((uint)Environment.TickCount - i.dwTime) / 1000.0; }
'@
function Test-PlayerUsing($g) {
    $fg = [uint32]0
    [void][OblvGuard.W]::GetWindowThreadProcessId([OblvGuard.W]::GetForegroundWindow(), [ref]$fg)
    $fg -eq $g.Id -and [OblvGuard.W]::IdleSeconds() -lt 15
}
$sw = [Diagnostics.Stopwatch]::StartNew()
$answered = $false
while (-not $game.HasExited -and $sw.Elapsed.TotalSeconds -lt $ModWaitSec) {
    if (Test-NewOwner) { Log 'a new runner took the game lock: handing the game over'; Flush-Log; exit }
    if (Test-PlayerUsing $game) {
        Log 'the PC user is using the game: showing and un-muting it; it will not be closed'
        [OblvBench.GameHider]::SetShow($true); [OblvBench.WindowKeeper]::Stop(); [OblvBench.GameHider]::Stop()
        if (-not $Test) { Set-GameUnmuted | Out-Null }
        Flush-Log; exit
    }
    if (-not $Test) {
        if ((Read-BenchLogFrom 0) -match 'LOADED bench\.lua' -and $null -ne (Send-Bench 'ping' -TimeoutMs 1500 -NoThrow)) { $answered = $true; break }
    } elseif ($sw.Elapsed.TotalSeconds -ge [math]::Min(3, $ModWaitSec)) { $answered = $true; break }
    Flush-Log
    Start-Sleep -Milliseconds 500
}
Flush-Log
if ($game.HasExited) { Log 'its game ended by itself' }
else {
    Log "closing its game (pid $($game.Id); mod answering: $answered)"
    switch ($CloseAction) {
        'bench' { try { & (Join-Path $PSScriptRoot 'Run-Benchmark.ps1') -Close -Force *>&1 | ForEach-Object { Log "  $_" } } catch { Log "  -Close failed: $_" } }
        'kill' { Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue }
        'none' { }
    }
}
if ((Get-LockPid) -eq $RunnerPid) { Remove-Item -LiteralPath $LockFile -ErrorAction SilentlyContinue; Log 'removed the dead runner''s game.lock' }
Flush-Log
Stop-Unobtrusive
Log 'done'
