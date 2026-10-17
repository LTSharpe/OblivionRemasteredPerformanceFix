<#
.SYNOPSIS
  What the benchmark is doing, from bench-status.json in the runtime folder (written by Run-Benchmark.ps1), checked
  against reality (is the runner process alive, is the game alive, is the status fresh).

.EXAMPLE
  .\Bench-Status.ps1                    # one summary line
  .\Bench-Status.ps1 -Wait              # print progress lines until the run ends, then the result
  .\Bench-Status.ps1 -Wait -TimeoutMin 30

  Exit codes (also with -Wait): 0 finished ok, 1 finished partial, 2 finished failed, 3 the runner died
  without finishing, 4 -Wait timed out, 5 the runner is alive but has not updated its status for 5 min,
  6 running (no -Wait), 7 no status yet.
#>
param([switch]$Wait, [double]$TimeoutMin = 0, [int]$PollSec = 3)
$root = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot 'GamePaths.ps1')
$file = Join-Path (Get-BenchRuntimeDir $root) 'bench-status.json'

function Read-Status {
    if (-not (Test-Path $file)) { return $null }
    for ($i = 0; $i -lt 5; $i++) { try { return (Get-Content $file -Raw | ConvertFrom-Json) } catch { Start-Sleep -Milliseconds 100 } }
    $null
}
function Test-Runner($st) {
    if (-not $st.pid) { return $false }
    $p = Get-CimInstance Win32_Process -Filter "ProcessId=$($st.pid)" -ErrorAction SilentlyContinue
    [bool]($p -and $p.CommandLine -notmatch 'Bench-Status')
}
function Get-Verdict($st) {
    if (-not $st) { return @{ code = 7; text = "no benchmark status yet ($file)" } }
    $age = ((Get-Date) - [datetime]$st.updatedAt).TotalSeconds
    $game = Get-Process 'OblivionRemastered-Win64-Shipping' -ErrorAction SilentlyContinue | Select-Object -First 1
    $gameText = if ($game) { "game pid $($game.Id) running" } else { 'no game process' }
    if ($st.state -eq 'done') {
        $code = @{ ok = 0; partial = 1 }[[string]$st.outcome]; if ($null -eq $code) { $code = 2 }
        return @{ code = $code; text = "FINISHED $($st.outcome): $($st.label), warnings $($st.warnings), restarts $($st.restarts), game $($st.game); results in $($st.runDir)" }
    }
    if ($st.state -eq 'closed') { return @{ code = 0; text = "game closed ($($st.message)); $gameText" } }
    if (-not (Test-Runner $st)) { return @{ code = 3; text = "RUNNER DIED without finishing (pid $($st.pid), last: $($st.message), $([int]$age) s ago); $gameText. Clean up with tools\Run-Benchmark.ps1 -Close" } }
    $where = if ($st.scene) { "$($st.phase), scene $($st.sceneIndex)/$($st.sceneCount) $($st.scene)" } else { "$($st.phase)" }
    $text = "RUNNING $($st.label): $where; restarts $($st.restarts); last: $($st.message) ($([int]$age) s ago); deadline $(([datetime]$st.deadline).ToString('HH:mm')); $gameText"
    if ($age -gt 300) { return @{ code = 5; text = "STUCK? $text" } }
    @{ code = 6; text = $text }
}

$st = Read-Status
if (-not $Wait) { $v = Get-Verdict $st; $v.text; exit $v.code }

$sw = [Diagnostics.Stopwatch]::StartNew(); $lastMsg = $null
while ($true) {
    $st = Read-Status
    if ($st -and $st.message -and $st.message -ne $lastMsg) {
        $lastMsg = $st.message
        "{0} {1}" -f ([datetime]$st.updatedAt).ToString('HH:mm:ss'), $st.message
    }
    $v = Get-Verdict $st
    if ($v.code -ne 6 -and $v.code -ne 7) { $v.text; exit $v.code }
    if ($TimeoutMin -gt 0 -and $sw.Elapsed.TotalMinutes -ge $TimeoutMin) { "WAIT TIMED OUT: $($v.text)"; exit 4 }
    Start-Sleep -Seconds $PollSec
}
