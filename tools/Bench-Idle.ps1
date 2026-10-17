<#
.SYNOPSIS
  Started hidden by Run-Benchmark.ps1 when it leaves the game open (parked at the main menu). Looks every
  10 s and:
    - steps aside (exits) as soon as a run or a hand measurement takes the game (game.lock in the runtime folder);
    - when the game has closed (by anyone), restores the saves and config the benchmark changed;
    - after -IdleMin minutes without a run, closes the parked game and restores (0 = never close);
    - if the player uses the game (in front, with keyboard or mouse input), never closes it, only restores after it closes
      (saves the player made after the benchmark are left alone).
  Log: bench-idle.log in the runtime folder (run\ by default)
#>
param([double]$IdleMin = 20)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BenchBridge.ps1')
$root = Split-Path $PSScriptRoot
$sessionFile = Join-Path $script:BenchRun 'bench-session.json'
$logFile = Join-Path $script:BenchRun 'bench-idle.log'
$runner = Join-Path $PSScriptRoot 'Run-Benchmark.ps1'
Add-Type -Namespace OblvIdle -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO i);
public static double IdleSeconds() { var i = new LASTINPUTINFO(); i.cbSize = 8; GetLastInputInfo(ref i); return ((uint)Environment.TickCount - i.dwTime) / 1000.0; }
'@
function Log([string]$m) { Add-Content $logFile ("{0:yyyy-MM-dd HH:mm:ss} [{1}] {2}" -f (Get-Date), $PID, $m) -Encoding utf8 }
function Invoke-Close([string]$why) {
    Log "$why -> closing/restoring"
    try { & $runner -Close -Force *>&1 | ForEach-Object { Log "  $_" } } catch { Log "  failed: $_" }
}

Log ("watching (idle limit {0} min)" -f $IdleMin)
$playerTookOver = $false
while ($true) {
    Start-Sleep -Seconds 10
    if (Test-Path $script:BenchLockFile) { Log 'a run took the game: done'; exit }
    $g = Get-Game
    if (-not $g) { Invoke-Close 'the game has closed'; exit }
    if (-not $playerTookOver) {
        $fgPid = [uint32]0
        [void][OblvIdle.W]::GetWindowThreadProcessId([OblvIdle.W]::GetForegroundWindow(), [ref]$fgPid)
        # the game takes the focus by itself at times, so only the game in front AND keyboard/mouse input count as the player
        if ($fgPid -eq $g.Id -and [OblvIdle.W]::IdleSeconds() -lt 15) { $playerTookOver = $true; Log 'the player is using the game: it will not be closed; saves and config are restored after it closes'; continue }
        if ($IdleMin -gt 0 -and (Test-Path $sessionFile)) {
            $st = Get-Content $sessionFile -Raw | ConvertFrom-Json
            $last = if ($st.lastActivity) { [datetime]$st.lastActivity } else { $g.StartTime }
            if (((Get-Date) - $last).TotalMinutes -ge $IdleMin) { Invoke-Close ("no run for {0:N0} min" -f ((Get-Date) - $last).TotalMinutes); exit }
        }
    }
}
