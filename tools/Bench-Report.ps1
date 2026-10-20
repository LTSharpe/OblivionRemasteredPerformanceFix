<#
.SYNOPSIS
  One command for a shareable "stock vs fix" result: runs the benchmark alternately with every fix switched off and
  with the fix as installed (A-B-A-B), then writes report.md for the "Benchmark result" issue form, plus copies of the
  runs' result.json with anything personal removed. Takes about 10-15 minutes per run with the default scenes.

  Or, with -Base and -Test, only writes the report from runs you already made (no game needed).

  "Stock" = OblivionPerfFix loaded with all its switches at 0 (each fix then leaves the game's code alone); the ini is
  put back after every run. The fix must be installed (see README) and the benchmark set up (tools\Setup-Bench.ps1).

.PARAMETER Rounds
  How many stock/fix pairs (default 2, i.e. 4 runs). More rounds = smaller noise; differences under ~5% are noise.
.PARAMETER Scenario
  Scenario file to run (bench\scenarios\<name>.json; default cheydinhal). E.g. showcase (the Imperial City flight) or
  cheydinhal-show (the Cheydinhal flight); tools\Run-Benchmark.ps1 -List shows them all.
.PARAMETER Scenes
  Scenes to run (default: every scene of the scenario that runs by default; for cheydinhal the CPU-limited views and
  three door transitions).
.PARAMETER Profile
  Benchmark profile (bench\profiles.json). Default small: a small window, so the CPU limits the frame rate.
.PARAMETER Watch
  Watch the runs: the game window and sound on, at your own graphics settings (profile "own" unless -Profile is
  given). The default keeps the game hidden and muted.

.EXAMPLE
  tools\Bench-Report.ps1
  tools\Bench-Report.ps1 -Rounds 3 -Profile cpu
  tools\Bench-Report.ps1 -Watch -Rounds 1                               # watch it, at your own settings
  tools\Bench-Report.ps1 -Scenario showcase -Profile native             # the Imperial City flight at your monitor's resolution
  tools\Bench-Report.ps1 -Base stock-r1,stock-r2 -Test fix-r1,fix-r2      # report from existing runs only
#>
[CmdletBinding()]
param(
    [int]$Rounds = 2,
    [string]$Scenario,
    [string[]]$Scenes,
    [string]$Profile = 'small',
    [string]$Label = 'report',
    [string[]]$Base,
    [string[]]$Test,
    [switch]$Watch
)
if ($Watch -and -not $PSBoundParameters.ContainsKey('Profile')) { $Profile = 'own' }
$ErrorActionPreference = 'Stop'
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$root = Split-Path $PSScriptRoot
$benchDir = Join-Path $root 'captures\bench'
. (Join-Path $PSScriptRoot 'GamePaths.ps1')

# ---------------------------------------------------------------- personal data never leaves the PC
$scrubs = @(
    @{ From = [regex]::Escape($env:USERPROFILE); To = '<profile>' },
    @{ From = [regex]::Escape($env:LOCALAPPDATA); To = '<localappdata>' },
    @{ From = '(?<![A-Za-z0-9])' + [regex]::Escape($env:USERNAME) + '(?![A-Za-z0-9])'; To = '<user>' },
    @{ From = '(?<![A-Za-z0-9])' + [regex]::Escape($env:COMPUTERNAME) + '(?![A-Za-z0-9])'; To = '<pc>' },
    @{ From = '(?<=\b(pid|window)=)\S+'; To = '-' }
)
function Protect-Text([string]$s) {
    foreach ($r in $scrubs) { if ($r.From -and $r.From -ne '(?<![A-Za-z0-9])(?![A-Za-z0-9])') { $s = [regex]::Replace($s, $r.From, $r.To, 'IgnoreCase') } }
    $s
}

function Resolve-Run([string]$name) {
    if (Test-Path -LiteralPath (Join-Path $name 'result.json')) { return (Resolve-Path -LiteralPath $name).Path }
    $dir = Get-ChildItem $benchDir -Directory | Where-Object { $_.Name -match ('^\d{8}-\d{6}-' + [regex]::Escape($name) + '$') } |
        Sort-Object Name | Select-Object -Last 1
    if (-not $dir) { throw "no benchmark run named '$name' in captures\bench" }
    $dir.FullName
}

# ---------------------------------------------------------------- run the benchmark (A-B-A-B)
if (-not $Base -and -not $Test) {
    $game = Get-OblivionGame
    $iniPath = Join-Path $game.Win64 'OblivionPerfFix.ini'
    if (-not (Test-Path -LiteralPath $iniPath)) { throw "OblivionPerfFix is not installed in $($game.Win64): install it first (README)" }
    # Every switch in the installed ini's [Fixes] section, at 0.
    $off = @{}
    $inFixes = $false
    foreach ($line in Get-Content -LiteralPath $iniPath) {
        if ($line -match '^\s*\[(.+)\]') { $inFixes = $Matches[1] -eq 'Fixes'; continue }
        if ($inFixes -and $line -match '^\s*([A-Za-z0-9]+)\s*=') { $off[$Matches[1]] = 0 }
    }
    if (-not $off.Count) { throw "no switches found in $iniPath" }
    $runner = Join-Path $PSScriptRoot 'Run-Benchmark.ps1'
    $Base = @(); $Test = @()
    $stamp = Get-Date -Format 'MMdd-HHmm'
    for ($r = 1; $r -le $Rounds; $r++) {
        foreach ($side in 'stock', 'fix') {
            $lbl = "$Label-$stamp-$side-r$r"
            $argLine = "-Label '$lbl' -Pass perf -Profile '$Profile' -Session auto"
            if ($Watch) { $argLine += ' -Watch' }
            if ($Scenario) { $argLine += " -Scenario '$Scenario'" }
            if ($Scenes) { $argLine += " -Scenes " + (($Scenes | ForEach-Object { "'$_'" }) -join ',') }
            if ($side -eq 'stock') { $argLine += ' -FixIni @{ ' + (($off.Keys | Sort-Object | ForEach-Object { "$_ = 0" }) -join '; ') + ' }' }
            Write-Host "== run $(2 * $r - ($side -eq 'stock')) of $(2 * $Rounds): $lbl"
            $out = & powershell -NoProfile -ExecutionPolicy Bypass -Command "& '$runner' $argLine" 2>&1 | ForEach-Object { "$_" }
            $verdict = $out | Where-Object { $_ -like 'BENCH-RESULT*' } | Select-Object -Last 1
            Write-Host "   $verdict"
            # the runner's step lines, kept with the run: they explain a warning such as "no arrival detected"
            if ("$verdict" -match ' dir=(\S+)' -and (Test-Path -LiteralPath $Matches[1])) { $out | Set-Content (Join-Path $Matches[1] 'runner.log') -Encoding utf8 }
            if ($verdict -notmatch '^BENCH-RESULT (ok|partial)') { throw "benchmark run $lbl failed: $verdict (see its report.md)" }
            if ($side -eq 'stock') { $Base += $lbl } else { $Test += $lbl }
        }
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $runner -Close | Out-Null
}

# ---------------------------------------------------------------- collect
function Load-Side([string[]]$names) {
    foreach ($n in $names) {
        $dir = Resolve-Run $n
        $json = Get-Content -LiteralPath (Join-Path $dir 'result.json') -Raw
        [pscustomobject]@{ Name = $n; Dir = $dir; Json = $json; R = ($json | ConvertFrom-Json) }
    }
}
$baseRuns = @(Load-Side $Base)
$testRuns = @(Load-Side $Test)

function Median([double[]]$v) {
    $v = @($v | Where-Object { $_ -ne $null } | Sort-Object)
    if (-not $v.Count) { return $null }
    if ($v.Count % 2) { return $v[[int][math]::Floor($v.Count / 2)] }
    ($v[$v.Count / 2 - 1] + $v[$v.Count / 2]) / 2
}
# scene id -> list of scene objects over all runs of a side
function Get-Scenes($runs) {
    $map = [ordered]@{}
    foreach ($run in $runs) {
        foreach ($s in @($run.R.sessions)) {
            if ($s.kind -ne 'perf') { continue }
            foreach ($sc in @($s.scenes)) { if (-not $map.Contains($sc.id)) { $map[$sc.id] = New-Object System.Collections.ArrayList }; [void]$map[$sc.id].Add($sc) }
        }
    }
    $map
}
$bs = Get-Scenes $baseRuns
$ts = Get-Scenes $testRuns

function Value($list, [string]$path) {
    Median @($list | ForEach-Object {
        $o = $_
        foreach ($p in $path -split '\.') { if ($null -eq $o) { break }; $o = $o.$p }
        if ($null -ne $o) { [double]$o }
    })
}
function Cell($b, $t, [string]$fmt = 'F1', [switch]$LowerIsBetter) {
    if ($null -eq $b -or $null -eq $t) { return '-' }
    $d = if ($b) { ($t - $b) / $b * 100 } else { 0 }
    $mark = if ([math]::Abs($d) -lt 5) { ' ~' } else { '' }
    '{0} -> {1} ({2:+0;-0}%{3})' -f $b.ToString($fmt), $t.ToString($fmt), $d, $mark
}

# ---------------------------------------------------------------- report
$first = $testRuns[0].R
# The version from the fix's log as a run recorded it (any fix run, any session); else from the log the installed fix
# wrote last (next to the DLL: Win64 as version.dll, OBSE\Plugins as an OBSE64 plugin); else "unknown" rather than blank.
$loadedLine = 'OblivionPerfFix (\S+) loaded'
$fixVersion = @($testRuns | ForEach-Object { $_.R.sessions } | ForEach-Object { $_.perfFixLog } | Where-Object { $_ -match $loadedLine } | Select-Object -First 1)
if (-not $fixVersion) {
    $win64 = (Get-OblivionGame).Win64
    $fixVersion = @('OblivionPerfFix.log', 'OBSE\Plugins\OblivionPerfFix.log' | ForEach-Object { Join-Path $win64 $_ } | Where-Object { Test-Path -LiteralPath $_ } |
        ForEach-Object { Get-Content -LiteralPath $_ -TotalCount 5 } | Where-Object { $_ -match $loadedLine } | Select-Object -First 1)
}
$fixVersion = if ($fixVersion) { "$fixVersion" -replace ".*$loadedLine.*", '$1' } else { 'unknown (no OblivionPerfFix.log found)' }
$cs = Get-CimInstance Win32_ComputerSystem
$os = Get-CimInstance Win32_OperatingSystem
$md = New-Object System.Collections.Generic.List[string]
$md.Add('# OblivionPerfFix: stock vs fix')
$md.Add('')
$md.Add("| | |")
$md.Add("|---|---|")
$md.Add("| Fix version | $fixVersion |")
$md.Add("| Game build | $($first.game.build) |")
$md.Add("| CPU | $($first.system.cpu) |")
$md.Add("| GPU, driver | $($first.system.gpu), $($first.system.gpuDriver) |")
$md.Add("| RAM | $([math]::Round($cs.TotalPhysicalMemory / 1GB)) GB |")
$md.Add("| Windows | $($os.Caption -replace 'Microsoft ', '') build $($os.BuildNumber) |")
$md.Add("| Profile | $($first.profile.name): $($first.profile.res) $($first.profile.mode), upscaler $($first.profile.upscaler), FG $($first.profile.fg), RT $($first.profile.rt) |")
$caps = @(@($baseRuns) + @($testRuns) | ForEach-Object { $_.R.sessions } | ForEach-Object { $_.frameCap } | Where-Object { $_ } | Select-Object -Unique)
if ($caps.Count) { $md.Add("| Frame cap | fps capped at $($caps -join ' / ') by $(if ($first.profile.own) { "the player's settings" } else { 'the profile' }) |") }
$md.Add("| Runs | stock $($baseRuns.Count), fix $($testRuns.Count) (medians; alternated) |")
$md.Add('')
$md.Add('Each cell is stock -> fix (change). `~` = under 5%, within run-to-run noise. Game / Render / RHI = CPU ms per frame')
$md.Add('on that thread; GPU = GPU busy ms per frame.')
$md.Add('')
$md.Add('## Steady views')
$md.Add('')
$md.Add('| Scene | fps | 1% low | Game ms | Render ms | RHI ms | GPU ms |')
$md.Add('|---|---|---|---|---|---|---|')
foreach ($id in $bs.Keys) {
    if (-not $ts.Contains($id)) { continue }
    $b = $bs[$id]; $t = $ts[$id]
    if ($b[0].type -ne 'static') { continue }
    $md.Add("| $id | $(Cell (Value $b fps) (Value $t fps)) | $(Cell (Value $b low1) (Value $t low1)) | $(Cell (Value $b threads.GameThread.msPerFrame) (Value $t threads.GameThread.msPerFrame) 'F2') | $(Cell (Value $b threads.RenderThread.msPerFrame) (Value $t threads.RenderThread.msPerFrame) 'F2') | $(Cell (Value $b threads.RHIThread.msPerFrame) (Value $t threads.RHIThread.msPerFrame) 'F2') | $(Cell (Value $b gpuBusyMs) (Value $t gpuBusyMs) 'F1') |")
}
$doors = @($bs.Keys | Where-Object { $ts.Contains($_) -and $bs[$_][0].PSObject.Properties['afterWorstMs'] })
if ($doors.Count) {
    $md.Add('')
    $md.Add('## Doors and gates')
    $md.Add('')
    $md.Add('Worst frame after arriving = the stutter as the picture comes back. Longest gap = the longest time without a new frame')
    $md.Add('(mostly behind the loading screen). Arrival = seconds from using the door until the new place is playable.')
    $md.Add('')
    $md.Add('| Transition | worst frame after arriving (ms) | longest gap (ms) | arrival (s) |')
    $md.Add('|---|---|---|---|')
    foreach ($id in $doors) {
        $b = $bs[$id]; $t = $ts[$id]
        $md.Add("| $id | $(Cell (Value $b afterWorstMs) (Value $t afterWorstMs) 'F0') | $(Cell (Value $b longestGapMs) (Value $t longestGapMs) 'F0') | $(Cell (Value $b arriveSec) (Value $t arriveSec) 'F1') |")
    }
}
$moving = @($bs.Keys | Where-Object { $ts.Contains($_) -and $bs[$_][0].type -in 'fly', 'walk' })
if ($moving.Count) {
    $md.Add('')
    $md.Add('## Moving through the world')
    $md.Add('')
    $md.Add('| Scene | fps | 1% low | worst frame (ms) | hitches |')
    $md.Add('|---|---|---|---|---|')
    foreach ($id in $moving) {
        $b = $bs[$id]; $t = $ts[$id]
        $md.Add("| $id | $(Cell (Value $b fps) (Value $t fps)) | $(Cell (Value $b low1) (Value $t low1)) | $(Cell (Value $b worstMs) (Value $t worstMs) 'F0') | $(Cell (Value $b hitches) (Value $t hitches) 'F0') |")
    }
}
# What the fix log said on this PC: which fixes found their code. Only lines that say something is off or missing.
$notes = @($testRuns | ForEach-Object { $_.R.sessions } | ForEach-Object { $_.perfFixLog } |
    Where-Object { $_ -match 'not found|not installed|disabled|unavailable|stays off|: off|[1-9][0-9]* skipped|unexpected|nothing patched' -and $_ -notmatch 'in OblivionPerfFix\.ini' } |
    ForEach-Object { $_ -replace '^\S+ \S+ ', '' } | Sort-Object -Unique)
$md.Add('')
$md.Add('## Fix log')
$md.Add('')
if ($notes.Count) { $md.Add('Fixes that did not find their code on this PC (from OblivionPerfFix.log):'); $md.Add(''); $notes | ForEach-Object { $md.Add("- ``$_``") } }
else { $md.Add('Every fix found its code (no "not found" lines in OblivionPerfFix.log).') }
$warn = @($baseRuns + $testRuns | ForEach-Object { $_.R.sessions } | ForEach-Object { $_.warnings } | Where-Object { $_ } | Sort-Object -Unique)
if ($warn.Count) {
    $md.Add('')
    $md.Add('## Benchmark warnings')
    $md.Add('')
    $warn | Select-Object -First 15 | ForEach-Object { $md.Add("- $_") }
}
$md.Add('')
$md.Add("Made with tools\Bench-Report.ps1. Runs: stock $($Base -join ', '); fix $($Test -join ', ').")

$outDir = Join-Path $benchDir ('_report\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $outDir | Out-Null
$report = Protect-Text ($md -join "`n")
[IO.File]::WriteAllText((Join-Path $outDir 'report.md'), $report + "`n", (New-Object Text.UTF8Encoding $false))
$i = 0
foreach ($run in $baseRuns + $testRuns) {
    $i++
    $side = if ($i -le $baseRuns.Count) { 'stock' } else { 'fix' }
    [IO.File]::WriteAllText((Join-Path $outDir ("result-$side-$i.json")), (Protect-Text $run.Json), (New-Object Text.UTF8Encoding $false))
}
# Last check: nothing personal may be left in what is about to be shared.
$leaks = Get-ChildItem $outDir -File | Select-String -Pattern @([regex]::Escape($env:USERNAME), [regex]::Escape($env:COMPUTERNAME)) -SimpleMatch:$false
if ($leaks) { $leaks | ForEach-Object { Write-Warning "personal data left in $($_.Filename):$($_.LineNumber)" }; 'REPORT-RESULT check-before-sharing'; exit 1 }
$report
''
"REPORT-RESULT ok $outDir"
'Paste report.md into the "Benchmark result" issue form and attach the result-*.json files.'
