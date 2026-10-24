<#
.SYNOPSIS
  How repeatable is a scenario? Runs a scene N times the same way (or takes N runs already made) and prints,
  per scene, the spread of the key numbers, how alike the end screenshots are and the conditions each run
  actually had (weather, game hour, timescale read back from the game). Use it as the "certificate" of a
  new mini-scenario before iterating on it: a change smaller than the spread it reports cannot be seen
  with that many runs.

.EXAMPLE
  .\Repro-Check.ps1 -Scenario ic-spots -Scenes ic-waterfront-pass -N 3 -Pin        # 3 runs, then the report
  .\Repro-Check.ps1 -Runs br-base-ic-r1,br-base-ic-r2,br-base-ic-r3                 # report on runs already made
  .\Repro-Check.ps1 -Scenario faces -Scenes face-kewan -N 3 -Pin -Bench @{ After = 'close' }

.NOTES
  Run it like Run-Benchmark.ps1 (in the background; it takes the turn rules of the runner). Output:
  captures\bench\_repro\<label>\repro.md and the screenshot sheets (run 1 | run k | difference).
  Spread = (max - min) / median in percent; for counts (frames >= 33 ms, hitches) the min..max is shown.
#>
param(
    [string[]]$Runs,                     # existing runs (labels or folders) to analyse instead of running
    [string]$Scenario,
    [string[]]$Scenes,
    [int]$N = 3,
    [string]$Label,                      # default: repro-<scenario>
    [switch]$Pin,                        # pass -Pin to the runner (the default since 2026-10-20)
    [switch]$NoPin,                      # pass -NoPin (old behaviour)
    [switch]$NoFixedSeed,                # pass -NoFixedSeed
    [string]$Profile,
    [hashtable]$Bench = @{},             # more Run-Benchmark.ps1 parameters, e.g. @{ FixIni = @{ X = 1 }; After = 'close' }
    [double]$FpsLimitPct = 5,            # verdict thresholds (spread in %)
    [double]$GameLimitPct = 5,
    [double]$SsimMin = 0.90
)
$ErrorActionPreference = 'Stop'
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$root = Split-Path $PSScriptRoot
$benchDir = Join-Path $root 'captures\bench'
. (Join-Path $PSScriptRoot 'BenchBridge.ps1')
# "a,b,c" from powershell -File arrives as one string
if ($Runs) { $Runs = @($Runs | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) }
if ($Scenes) { $Scenes = @($Scenes | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) }

if (-not $Runs) {
    if (-not $Scenario -or -not $Scenes) { throw 'give -Runs, or -Scenario and -Scenes to run' }
    if (-not $Label) { $Label = "repro-$Scenario" }
    $args2 = @{ Label = $Label; Scenario = $Scenario; Scenes = $Scenes; Runs = $N; Pass = 'perf' } + $Bench
    if ($Pin) { $args2.Pin = $true }
    if ($NoPin) { $args2.NoPin = $true }
    if ($NoFixedSeed) { $args2.NoFixedSeed = $true }
    if ($Profile) { $args2.Profile = $Profile }
    & (Join-Path $PSScriptRoot 'Run-Benchmark.ps1') @args2
    $Runs = @(1..$N | ForEach-Object { if ($N -gt 1) { "$Label-r$_" } else { $Label } })
}

function Resolve-Run([string]$name) {
    if (Test-Path (Join-Path $name 'result.json')) { return (Resolve-Path $name).Path }
    $dir = Get-ChildItem $benchDir -Directory | Where-Object { $_.Name -match ('^\d{8}-\d{6}-' + [regex]::Escape($name) + '$') } | Sort-Object Name | Select-Object -Last 1
    if (-not $dir -or -not (Test-Path (Join-Path $dir.FullName 'result.json'))) { throw "no benchmark run named '$name' in $benchDir" }
    $dir.FullName
}
function Median($values) {
    $v = @($values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ } | Sort-Object)
    if (-not $v.Count) { return $null }
    if ($v.Count % 2) { $v[($v.Count - 1) / 2] } else { ($v[$v.Count / 2 - 1] + $v[$v.Count / 2]) / 2 }
}
function SpreadPct($values) {
    $v = @($values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
    $m = Median $v
    if ($v.Count -lt 2 -or -not $m) { return $null }
    [math]::Round(100 * (($v | Measure-Object -Maximum).Maximum - ($v | Measure-Object -Minimum).Minimum) / [math]::Abs($m), 1)
}
function Kv([string]$s) {          # "hour=12.00 wx=Clear:1.00 ..." -> hashtable
    $h = @{}
    foreach ($t in "$s" -split '\s+') { if ($t -match '^([^=]+)=(.*)$') { $h[$Matches[1]] = $Matches[2] } }
    $h
}

$loaded = @(foreach ($r in $Runs) {
    $dir = Resolve-Run $r
    $res = Get-Content (Join-Path $dir 'result.json') -Raw | ConvertFrom-Json
    $frames = Join-Path $dir 'perf-frames.csv'
    $log = if (Test-Path $frames) { try { [OblvBench.FrameLog]::Load($frames) } catch { $null } } else { $null }
    [pscustomobject]@{ name = $res.label; dir = $dir; res = $res; log = $log }
})
if ($loaded.Count -lt 2) { throw 'need at least two runs' }
$scnName = $loaded[0].res.scenario
$mask = $null
try { $mask = [double[]]@((Get-BenchScenario $scnName).masks | ForEach-Object { $_ }) } catch { }

$outDir = Join-Path $benchDir ('_repro\' + ($loaded[0].name -replace '-r\d+$', ''))
New-Item -ItemType Directory -Force $outDir | Out-Null
$md = New-Object System.Collections.Generic.List[string]
$md.Add("# Repeatability: $(($loaded | ForEach-Object name) -join ', ')"); $md.Add('')
$seeds = @($loaded | ForEach-Object { if ($_.res.PSObject.Properties['fixedSeed']) { "$($_.res.fixedSeed)" } else { 'not recorded' } } | Select-Object -Unique)
$md.Add("Scenario ``$scnName``, profile ``$($loaded[0].res.profile.name)``, $($loaded.Count) runs, -FixedSeed on the game's command line: $($seeds -join ' / '). Spread = (max - min) / median.")
$md.Add('')

function SceneOf($run, [string]$id) { @($run.res.sessions | Where-Object { $_.kind -eq 'perf' } | ForEach-Object { $_.scenes } | Where-Object { $_.id -eq $id })[0] }
$ids = @($loaded[0].res.sessions | Where-Object { $_.kind -eq 'perf' } | ForEach-Object { $_.scenes } | ForEach-Object { $_.id })
$verdicts = @()
foreach ($id in $ids) {
    $rows = @(foreach ($run in $loaded) {
        $s = SceneOf $run $id
        if (-not $s) { continue }
        $o33 = $null; $o25 = $null; $over167 = $null
        if ($run.log -and $s.t0Ms) {
            $ser = $run.log.Series([double]$s.t0Ms, [double]$s.t1Ms)
            $o33 = @($ser | Where-Object { $_[1] -ge 33.3 }).Count
            $o25 = @($ser | Where-Object { $_[1] -ge 25 }).Count
        }
        $cond = if ($s.PSObject.Properties['cond']) { $s.cond } else { $null }
        $set = if ($cond -and $cond.PSObject.Properties['atSet']) { Kv $cond.atSet } else { @{} }
        $end = if ($cond -and $cond.PSObject.Properties['atEnd']) { Kv $cond.atEnd } else { @{} }
        [pscustomobject]@{
            run = $run.name; fps = $s.fps; worst = $s.worstMs; o33 = $o33; o25 = $o25; hitches = $s.hitches
            gtPct = $s.threads.GameThread.pct; game = $s.threads.GameThread.msPerFrame; render = $s.threads.RenderThread.msPerFrame; gpu = $s.gpuBusyMs
            frames = $s.frames; shot = if ($s.shot) { Join-Path $run.dir $s.shot } else { $null }
            pinned = if ($cond) { [bool]$cond.pinned } else { $false }
            wxSet = $set['wx']; wxEnd = $end['wx']; hourSet = $set['hour']; hourEnd = if ($end['hour']) { $end['hour'] } else { $s.hour }
            ts = if ($cond -and $cond.PSObject.Properties['timescale']) { $cond.timescale } else { '' }
            aiHeld = [bool]($s.PSObject.Properties['aiHeld'] -and $s.aiHeld)
            restarted = [bool]($run.res.restarts -and [int]$run.res.restarts -gt 0)
            npc = $(if ($end['npcs']) { "$($end['npcs']) $($end['near'])" } else { '' }); pos = $s.pos
        }
    })
    if ($rows.Count -lt 2) { continue }
    # PresentMon sometimes loses most of a scene's frames (seen 2026-10-20): such a run's per-frame numbers are void
    $medFrames = Median ($rows.frames)
    $lost = @($rows | Where-Object { $_.frames -lt 0.6 * $medFrames -or $_.restarted })
    foreach ($r in $lost) { $r | Add-Member NoteProperty void $true }
    $ok = @($rows | Where-Object { -not $_.PSObject.Properties['void'] })
    $md.Add("## $id"); $md.Add('')
    $md.Add('| run | frames | fps | worst ms | frames >= 33 ms | >= 25 ms | hitches | Game % of wall | Game ms | Render ms | GPU ms | weather set -> end | hour set -> end | timescale | NPCs near (end) |')
    $md.Add('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    foreach ($r in $rows) {
        $void = if ($r.PSObject.Properties['void']) { ' (frames lost: left out)' } else { '' }
        if ($r.aiHeld) { $void += ' (AI held: shots only)' }
        if ($r.restarted) { $void += ' (game restarted in this run: not comparable, left out)' }
        $md.Add("| $($r.run)$void | $($r.frames) | $($r.fps) | $($r.worst) | $($r.o33) | $($r.o25) | $($r.hitches) | $($r.gtPct) | $($r.game) | $($r.render) | $($r.gpu) | $($r.wxSet) -> $($r.wxEnd) | $($r.hourSet) -> $($r.hourEnd) | $($r.ts) | $($r.npc) |")
    }
    $mm = { param($k) $v = @($ok | ForEach-Object { $_.$k } | Where-Object { $null -ne $_ }); if ($v.Count) { "$(($v | Measure-Object -Minimum).Minimum)..$(($v | Measure-Object -Maximum).Maximum)" } else { '' } }
    $sp = [ordered]@{ fps = SpreadPct ($ok.fps); worst = SpreadPct ($ok.worst); gtPct = SpreadPct ($rows.gtPct); game = SpreadPct ($ok.game); render = SpreadPct ($ok.render); gpu = SpreadPct ($ok.gpu) }
    $md.Add("| **spread** | | $($sp.fps) % | $($sp.worst) % | $(& $mm 'o33') | $(& $mm 'o25') | $(& $mm 'hitches') | $($sp.gtPct) % | $($sp.game) % | $($sp.render) % | $($sp.gpu) % | | | | |")
    $md.Add('')
    # screenshots: every run against the first
    $ssim = @(); $shotRows = @()
    for ($k = 1; $k -lt $rows.Count; $k++) {
        $a = $rows[0].shot; $b = $rows[$k].shot
        if (-not $a -or -not $b -or -not (Test-Path $a) -or -not (Test-Path $b)) { continue }
        $sheet = Join-Path $outDir ("{0}-1-vs-{1}.png" -f ($id -replace '[\\/]', '-'), ($k + 1))
        try {
            $d = [OblvBench.ImageTools]::Compare($a, $b, '', $sheet, $mask)
            $ssim += $d.Ssim
            $shotRows += "| 1 vs $($k + 1) | $([math]::Round($d.Ssim, 3)) | $([math]::Round($d.MeanAbsDiff, 2)) | $([math]::Round($d.ChangedPct, 1)) | $([math]::Round($d.LumaDeltaPct, 1)) | $(Split-Path $sheet -Leaf) |"
        } catch { $shotRows += "| 1 vs $($k + 1) | cannot compare: $($_.Exception.InnerException.Message) | | | | |" }
    }
    if ($shotRows.Count) {
        $md.Add('End screenshots (first run against each other run):'); $md.Add('')
        $md.Add('| pair | SSIM | mean abs diff | changed % | brightness % | sheet |'); $md.Add('|---|---|---|---|---|---|')
        $shotRows | ForEach-Object { $md.Add($_) }; $md.Add('')
    }
    $wxEnds = @($rows | ForEach-Object { $_.wxEnd } | Where-Object { $_ } | Select-Object -Unique)
    $issues = @()
    if ($null -ne $sp.fps -and $sp.fps -gt $FpsLimitPct) { $issues += "fps spread $($sp.fps) %" }
    if ($null -ne $sp.game -and $sp.game -gt $GameLimitPct) { $issues += "Game ms spread $($sp.game) %" }
    if ($ok.Count -lt 2) { $issues += 'fewer than two runs with complete frame logs' }
    if ($lost.Count) { $issues += "$($lost.Count) run(s) with lost frames or a restart left out" }
    if ($ssim.Count -and ($ssim | Measure-Object -Minimum).Minimum -lt $SsimMin) { $issues += "screenshot SSIM down to $([math]::Round(($ssim | Measure-Object -Minimum).Minimum, 3))" }
    if ($wxEnds.Count -gt 1) { $issues += "weather differed at the end ($($wxEnds -join ' / '))" }
    if (-not ($rows | Where-Object pinned)) { $issues += 'conditions not pinned (run without -NoPin)' }
    $verdict = if ($issues.Count) { 'NOISY: ' + ($issues -join '; ') } else { 'REPEATABLE' }
    $md.Add("Verdict: **$verdict** (limits: fps and Game ms spread <= $FpsLimitPct / $GameLimitPct %, SSIM >= $SsimMin, same weather)")
    $md.Add('')
    $verdicts += "${id}: $verdict"
}
$out = Join-Path $outDir 'repro.md'
[IO.File]::WriteAllLines($out, $md)
$md | ForEach-Object { Write-Host $_ }
Write-Host ''
$verdicts | ForEach-Object { Write-Host $_ }
Write-Host "report: $out"
