<#
.SYNOPSIS
  Compares benchmark runs made with Run-Benchmark.ps1: performance per scene (medians when
  several runs are given per side) and the frozen reference images of the visual pass
  (SSIM, mean absolute difference, changed area, brightness, colour and sharpness shift, plus a
  side-by-side sheet "base | test | difference" per scene).
  Runs are named by label (latest run with that label) or by folder path.
  Output: captures\bench\_compare\<base>-vs-<test>\compare.md and the sheets.

.EXAMPLE
  .\Compare-Benchmark.ps1 -Base baseline -Test poolsize
  .\Compare-Benchmark.ps1 -Base base-r1,base-r2,base-r3 -Test fix-r1,fix-r2,fix-r3
  .\Compare-Benchmark.ps1 -Base base-r1 -Test base-r2                     # run-to-run noise
  .\Compare-Benchmark.ps1 -Base base-r1 -Test fix -Floor base-r1,base-r2  # visual thresholds from measured noise
#>
param(
    [Parameter(Mandatory)][string[]]$Base,
    [Parameter(Mandatory)][string[]]$Test,
    [string[]]$Floor,
    [double]$NoisePct = 5        # performance changes below this (or below the run-to-run spread) are marked "~"
)

$ErrorActionPreference = 'Stop'
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$root = Split-Path $PSScriptRoot
$benchDir = Join-Path $root 'captures\bench'
. (Join-Path $PSScriptRoot 'BenchBridge.ps1')

function Resolve-Run([string]$name) {
    if (Test-Path (Join-Path $name 'result.json')) { return (Resolve-Path $name).Path }
    $dir = Get-ChildItem $benchDir -Directory | Where-Object { $_.Name -match ('^\d{8}-\d{6}-' + [regex]::Escape($name) + '$') } | Sort-Object Name | Select-Object -Last 1
    if (-not $dir -or -not (Test-Path (Join-Path $dir.FullName 'result.json'))) { throw "no benchmark run named '$name' in $benchDir" }
    $dir.FullName
}
function Load-Run([string]$name) {
    $dir = Resolve-Run $name
    $r = Get-Content (Join-Path $dir 'result.json') -Raw | ConvertFrom-Json
    $r | Add-Member NoteProperty dir $dir -PassThru
}
function Median($values) {
    $v = @($values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ } | Sort-Object)
    if (-not $v.Count) { return $null }
    if ($v.Count % 2) { $v[($v.Count - 1) / 2] } else { ($v[$v.Count / 2 - 1] + $v[$v.Count / 2]) / 2 }
}
function Spread($values) {
    $v = @($values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
    $m = Median $v
    if ($v.Count -lt 2 -or -not $m) { return 0 }
    100 * (($v | Measure-Object -Maximum).Maximum - ($v | Measure-Object -Minimum).Minimum) / [math]::Abs($m)
}
function Scenes($run) { @($run.sessions | Where-Object { $_.kind -eq 'perf' } | ForEach-Object { $_.scenes }) }

$baseRuns = @($Base | ForEach-Object { Load-Run $_ }); $testRuns = @($Test | ForEach-Object { Load-Run $_ })
$outDir = Join-Path $benchDir ('_compare\{0}-vs-{1}' -f (($baseRuns | ForEach-Object label) -join '+'), (($testRuns | ForEach-Object label) -join '+'))
New-Item -ItemType Directory -Force $outDir | Out-Null
$md = New-Object System.Collections.Generic.List[string]
$md.Add("# $(($testRuns | ForEach-Object label) -join ', ') vs $(($baseRuns | ForEach-Object label) -join ', ')"); $md.Add('')
foreach ($side in @{ n = 'Base'; r = $baseRuns }, @{ n = 'Test'; r = $testRuns }) {
    foreach ($r in $side.r) {
        $ut = @($r.underTest.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ', '
        $md.Add("- $($side.n): ``$($r.label)`` ($($r.time))$(if ($ut) { ", under test: $ut" })")
    }
}
$starts = @(@($baseRuns) + @($testRuns) | ForEach-Object { if ($_.start) { $_.start } else { 'continue' } } | Select-Object -Unique)
if ($starts.Count -gt 1) { $md.Add('- **Note: the runs started differently** (`-Start fresh` = new game with the default character, `continue` = a save; runs from before 2026-10-04 15:30 are `continue`). Character, quest state and NPCs differ, so small differences are not meaningful.') }
$profs = @(@($baseRuns) + @($testRuns) | ForEach-Object { if ($_.profile -and $_.profile.name) { $_.profile.name } else { 'cpu' } } | Select-Object -Unique)
if ($profs.Count -gt 1) { $md.Add("- **Note: the runs used different display profiles** ($($profs -join ', ')): resolution, upscaler and frame generation differ, so fps and GPU ms are not comparable (the default changed from ``cpu`` to ``small`` on 2026-10-06).") }
$restarted = @(@($baseRuns) + @($testRuns) | Where-Object { $_.PSObject.Properties['comparable'] -and -not $_.comparable -or ($_.restarts -and [int]$_.restarts -gt 0) } | ForEach-Object label)
if ($restarted.Count) { $md.Add("- **Not comparable: the game was restarted during $($restarted -join ', ')** (the scenes after a restart ran in a new game process): repeat those runs.") }
if (@(@($baseRuns) + @($testRuns) | Where-Object { $_.mode -eq 'reuse' }).Count) { $md.Add('- Note: at least one run reused a game session (`-Reuse`). Fps drifts within a session: confirm with fresh runs.') }
$md.Add('')

# ---- settings that differ although not declared as under test
$bs = ($baseRuns[0].sessions | Where-Object { $_.kind -eq 'perf' }).settings; $ts = ($testRuns[0].sessions | Where-Object { $_.kind -eq 'perf' }).settings
if ($bs -and $ts) {
    $declared = @($testRuns[0].underTest.PSObject.Properties.Name) + @($baseRuns[0].underTest.PSObject.Properties.Name)
    $diff = foreach ($p in $bs.PSObject.Properties) { if ($ts.PSObject.Properties[$p.Name] -and $ts.($p.Name) -ne $p.Value -and $declared -notcontains $p.Name) { "$($p.Name): $($p.Value) -> $($ts.($p.Name))" } }
    if ($diff) { $md.Add('**Settings differ between the runs (not declared as under test):** ' + ($diff -join '; ')); $md.Add('') }
}

# ---- performance
function Cell($id, [scriptblock]$get, [bool]$higherIsBetter, [string]$fmt = '0.0') {
    $b = @($baseRuns | ForEach-Object { Scenes $_ | Where-Object { $_.id -eq $id } | ForEach-Object $get })
    $t = @($testRuns | ForEach-Object { Scenes $_ | Where-Object { $_.id -eq $id } | ForEach-Object $get })
    $mb = Median $b; $mt = Median $t
    if ($null -eq $mb -or $null -eq $mt) { return '-' }
    $delta = if ($mb -ne 0) { 100 * ($mt - $mb) / [math]::Abs($mb) } else { 0 }
    $noise = [math]::Max($NoisePct, [math]::Max((Spread $b), (Spread $t)))
    $mark = if ([math]::Abs($delta) -lt $noise) { '~' } elseif (($delta -gt 0) -eq $higherIsBetter) { '**better**' } else { '**worse**' }
    '{0} -> {1} ({2}%) {3}' -f $mb.ToString($fmt), $mt.ToString($fmt), $delta.ToString('+0.0;-0.0'), $mark
}

$ids = @(Scenes $baseRuns[0] | ForEach-Object id | Where-Object { $id = $_; Scenes $testRuns[0] | Where-Object { $_.id -eq $id } })
$steady = @(Scenes $baseRuns[0] | Where-Object { $ids -contains $_.id -and $_.type -ne 'door' })
if ($steady.Count) {
    $md.Add('## Scenes'); $md.Add('')
    $md.Add('`~` = within noise (' + $NoisePct + '% or the run-to-run spread). ms values are CPU time per frame on that thread.'); $md.Add('')
    $md.Add('| Scene | fps | 1% low | worst ms | hitch ms | Render ms | RHI ms | Game ms | GPU ms |'); $md.Add('|---|---|---|---|---|---|---|---|---|')
    foreach ($s in $steady) {
        $md.Add(('| {0} | {1} | {2} | {3} | {8} | {4} | {5} | {6} | {7} |' -f $s.id,
            (Cell $s.id { $_.fps } $true), (Cell $s.id { $_.low1 } $true), (Cell $s.id { $_.worstMs } $false),
            (Cell $s.id { $_.threads.RenderThread.msPerFrame } $false '0.00'), (Cell $s.id { $_.threads.RHIThread.msPerFrame } $false '0.00'),
            (Cell $s.id { $_.threads.GameThread.msPerFrame } $false '0.00'), (Cell $s.id { $_.gpuBusyMs } $false '0.00'), (Cell $s.id { $_.hitchMs } $false '0')))
    }
    $md.Add('')
}
$doors = @(Scenes $baseRuns[0] | Where-Object { $ids -contains $_.id -and $_.type -eq 'door' })
if ($doors.Count) {
    $md.Add('## Door transitions'); $md.Add('')
    $md.Add('| Transition | arrive s | stall ms | frames >100 ms | after: hitch ms | after: 1% low | after: fps |'); $md.Add('|---|---|---|---|---|---|---|')
    foreach ($s in $doors) {
        $md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} |' -f $s.id,
            (Cell $s.id { $_.arriveSec } $false '0.00'), (Cell $s.id { $_.longestGapMs } $false '0'), (Cell $s.id { $_.over100 } $false '0'),
            (Cell $s.id { $_.afterHitchMs } $false '0'), (Cell $s.id { $_.afterLow1 } $true), (Cell $s.id { $_.afterFps } $true)))
    }
    $md.Add('')
}

# ---- visual fidelity
function Visuals($run) { @($run.sessions | Where-Object { $_.kind -eq 'visual' } | ForEach-Object { $_.visual }) }
$scenario = Get-BenchScenario $baseRuns[0].scenario
$mask = [double[]]@($scenario.masks | ForEach-Object { $_ })
$bv = @(Visuals $baseRuns[0]); $tv = @(Visuals $testRuns[0])
$floorRuns = if ($Floor) { @($Floor | ForEach-Object { Load-Run $_ }) } else { @() }
if ($floorRuns.Count -and $floorRuns.Count -ne 2) { throw '-Floor takes exactly two runs of the same configuration' }
$check = 0
if ($bv.Count -and $tv.Count) {
    $md.Add('## Visual fidelity'); $md.Add('')
    $md.Add('Frozen, frame-averaged images of the same view. MAD = mean absolute difference (0-255); changed = share of 16x16 blocks that differ visibly.')
    $md.Add($(if ($floorRuns.Count) { "Thresholds per scene come from the difference between ``$($floorRuns[0].label)`` and ``$($floorRuns[1].label)`` (same configuration)." } else { 'Fixed thresholds (MAD 2.5, changed 5%, brightness 2%, sharpness 5%): pass -Floor with two baseline runs to use measured run-to-run noise instead.' }))
    $md.Add('')
    $md.Add('| Scene | verdict | SSIM | MAD | PSNR dB | changed % | brightness % | colour shift | sharpness % | sheet |'); $md.Add('|---|---|---|---|---|---|---|---|---|---|')
    foreach ($v in $bv) {
        $t = $tv | Where-Object { $_.id -eq $v.id }
        if (-not $t) { continue }
        $a = Join-Path $baseRuns[0].dir $v.image; $b = Join-Path $testRuns[0].dir $t.image
        $sheet = Join-Path $outDir "$($v.id).png"
        try { $d = [OblvBench.ImageTools]::Compare($a, $b, '', $sheet, $mask) }
        catch { $md.Add("| $($v.id) | **CHECK** (cannot compare: $($_.Exception.InnerException.Message)) | | | | | | | | |"); $check++; continue }
        $lim = @{ Mad = 2.5; Changed = 5.0; Luma = 2.0; Sharp = 5.0 }
        if ($floorRuns.Count) {
            $fa = @(Visuals $floorRuns[0]) | Where-Object { $_.id -eq $v.id }; $fb = @(Visuals $floorRuns[1]) | Where-Object { $_.id -eq $v.id }
            if ($fa -and $fb) {
                $f = [OblvBench.ImageTools]::Compare((Join-Path $floorRuns[0].dir $fa.image), (Join-Path $floorRuns[1].dir $fb.image), '', '', $mask)
                $lim = @{ Mad = 1.5 * $f.MeanAbsDiff + 0.3; Changed = $f.ChangedPct + 2; Luma = [math]::Abs($f.LumaDeltaPct) + 1; Sharp = [math]::Abs($f.SharpnessDeltaPct) + 3 }
            }
        }
        $bad = @()
        if ($d.MeanAbsDiff -gt $lim.Mad) { $bad += 'MAD' }
        if ($d.ChangedPct -gt $lim.Changed) { $bad += 'changed area' }
        if ([math]::Abs($d.LumaDeltaPct) -gt $lim.Luma) { $bad += 'brightness' }
        if ([math]::Abs($d.SharpnessDeltaPct) -gt $lim.Sharp) { $bad += 'sharpness' }
        $verdict = if ($bad.Count) { $check++; "**CHECK** ($($bad -join ', '))" } else { 'same' }
        $md.Add(('| {0} | {1} | {2:F4} | {3:F2} | {4:F1} | {5:F1} | {6:+0.00;-0.00} | {7:F2} | {8:+0.0;-0.0} | {9} |' -f $v.id, $verdict, $d.Ssim, $d.MeanAbsDiff, $d.Psnr, $d.ChangedPct, $d.LumaDeltaPct, $d.ColorShift, $d.SharpnessDeltaPct, (Split-Path $sheet -Leaf)))
    }
    $md.Add(''); $md.Add($(if ($check) { "**$check scene(s) need a look:** open the sheets (base | test | difference) in $outDir" } else { 'No visual difference above the thresholds.' }))
} else { $md.Add('No visual pass in one of the runs: visual fidelity not compared.') }

Set-Content (Join-Path $outDir 'compare.md') $md -Encoding utf8
$md
"`nSaved: $(Join-Path $outDir 'compare.md')"
