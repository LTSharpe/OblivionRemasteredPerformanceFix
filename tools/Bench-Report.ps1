<#
.SYNOPSIS
  One command for a shareable "stock vs fix" result: runs the benchmark alternately with every fix switched off and
  with the fix as installed (A-B-A-B), then writes report.md for the "Benchmark result" issue form, plus copies of the
  runs' result.json with anything personal removed. Takes about 10-15 minutes per run with the default scenes.

  Or, with -Base and -Test, only writes the report from runs you already made (no game needed).

  "Stock" = OblivionPerfFix loaded with all its switches at 0 (each fix then leaves the game's code alone), except
  FrameRateLimitAtStart: that one only makes the game apply the frame-rate limit the player saved (a game bug with frame
  generation on; since 1.2.2 only with FSR frame generation and no NVIDIA Reflex), so both sides run at the same cap; it is
  not a speed-up. The ini is put back after every run. The fix must be installed (see README) and the benchmark set up (tools\Setup-Bench.ps1).

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
.PARAMETER Session
  How each run gets its game (Run-Benchmark.ps1 -Session): auto (default; stock and fix runs start their own game
  anyway, because the fix's switches are read at start-up), fresh (always a newly started game), reuse.
.PARAMETER Plain
  For the benchmark kit's console: progress in plain words, one line per step with the time ("KIT> " in front, which
  Bench-Kit.ps1 strips), instead of the runner's verdict lines.
.PARAMETER MenuPsoWait
  After each game start, wait at the main menu until the game's start-up shader (PSO) precompile has finished before
  the scenes start (Run-Benchmark.ps1 -MenuPsoWait). The benchmark kit uses it for the Imperial City flight.

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
    [switch]$Watch,
    [ValidateSet('auto', 'fresh', 'reuse')][string]$Session = 'auto',
    [switch]$MenuPsoWait,
    [switch]$Plain
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
    # Kept as installed on the stock side: not a speed-up, it only makes the game honour the player's saved frame cap
    # (with frame generation on the game forgets it after a start), so stock and fix run at the same cap (none with DLSS
    # frame generation or Reflex, where the key leaves it to the game since 1.2.2).
    foreach ($k in @('FrameRateLimitAtStart')) { $off.Remove($k) }
    if (-not $off.Count) { throw "no switches found in $iniPath" }
    $runner = Join-Path $PSScriptRoot 'Run-Benchmark.ps1'
    $Base = @(); $Test = @()
    $stamp = Get-Date -Format 'MMdd-HHmm'
    for ($r = 1; $r -le $Rounds; $r++) {
        foreach ($side in 'stock', 'fix') {
            $lbl = "$Label-$stamp-$side-r$r"
            $argLine = "-Label '$lbl' -Pass perf -Profile '$Profile' -Session $Session"
            if ($MenuPsoWait) { $argLine += ' -MenuPsoWait' }
            if ($Watch) { $argLine += ' -Watch' }
            if ($Scenario) { $argLine += " -Scenario '$Scenario'" }
            if ($Scenes) { $argLine += " -Scenes " + (($Scenes | ForEach-Object { "'$_'" }) -join ',') }
            if ($side -eq 'stock') { $argLine += ' -FixIni @{ ' + (($off.Keys | Sort-Object | ForEach-Object { "$_ = 0" }) -join '; ') + ' }' }
            $runNo = 2 * $r - [int]($side -eq 'stock')
            if ($Plain) {
                Write-Host ''
                Write-Host ("KIT> Run {0} of {1} ({2:HH:mm}): starting the game {3}" -f $runNo, (2 * $Rounds), (Get-Date), $(if ($side -eq 'stock') { 'WITHOUT the fix (stock)' } else { 'WITH the fix' }))
            } else { Write-Host "== run $runNo of $(2 * $Rounds): $lbl" }
            # -Plain: the runner's time-stamped steps that a player should see, in plain words, as they happen
            $flightText = if ($Scenario -eq 'showcase') { 'about 2 minutes' } else { 'a few minutes' }
            $out = & powershell -NoProfile -ExecutionPolicy Bypass -Command "& '$($runner -replace "'", "''")' $argLine" 2>&1 | ForEach-Object {
                $s = "$_"
                if ($Plain -and $s -match '^\[(\d\d:\d\d):\d\d\] (.*)$') {
                    $t = $Matches[1]; $m = $Matches[2]
                    $say = switch -Regex ($m) {
                        '^starting the game' {
                            if ($game.LaunchMethod -ne 'Direct') { 'Steam starts the game; it opens by itself (no save is loaded, your characters are not touched)' }
                            else { 'the game opens by itself (no save is loaded, your characters are not touched)' }
                        }
                        '^waiting for the main menu' { 'skipping the start-up videos' }
                        "^main menu: waiting for the game's PSO precompile" { 'at the main menu the game prepares its shaders: normal, about 2 minutes without the fix, less with it; the game looks idle meanwhile' }
                        '^waited \d+ s at the main menu' { 'shaders ready: starting a new game' }
                        '^in the world' { 'in the world: setting up the flight (weather, time of day, camera)' }
                        '^scene \d+/\d+: ' { "flight running ($flightText)$(if ($Watch) { ": please don't touch the mouse or keyboard" })" }
                        '^closing the game' { 'flight done: closing the game' }
                        '^config files restored' { 'your game settings are back as they were' }
                    }
                    if ($say) { Write-Host "KIT>   $t  $say" }
                } elseif ($Plain -and $s -match '^WARNING: (.*)$') { Write-Host "KIT>   note: $($Matches[1])" }
                $s
            }
            $verdict = $out | Where-Object { $_ -like 'BENCH-RESULT*' } | Select-Object -Last 1
            if ($Plain) { if ("$verdict" -match '^BENCH-RESULT (ok|partial)') { Write-Host ("KIT>   {0:HH:mm}  run {1} done" -f (Get-Date), $runNo) } }
            else { Write-Host "   $verdict" }
            # the runner's step lines, kept with the run: they explain a warning such as "no arrival detected"
            if ("$verdict" -match ' dir=(.+?) warnings=' -and (Test-Path -LiteralPath $Matches[1])) { $out | Set-Content (Join-Path $Matches[1] 'runner.log') -Encoding utf8 }
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
if ($first.profile.own) { $md.Add("| Profile | $($first.profile.name): the player's own settings, nothing changed |") }
else { $md.Add("| Profile | $($first.profile.name): $($first.profile.res) $($first.profile.mode), upscaler $($first.profile.upscaler), FG $($first.profile.fg), RT $($first.profile.rt) |") }
# At the player's own settings (profile own / user): what those settings were, from the run's recorded cvars
if ($first.profile.own) {
    $set = @($testRuns | ForEach-Object { $_.R.sessions } | Where-Object { $_.kind -eq 'perf' -and $_.settings } | Select-Object -First 1).settings
    if ($set) {
        $lv = { param($v) if ($null -eq $v) { '?' } else { @('Low', 'Medium', 'High', 'Ultra', 'Cinematic')[[math]::Min(4, [math]::Max(0, [int][double]$v))] } }
        $up = switch ([int][double]$set.'Altar.UpscalingMethod') { 0 { 'none' } 1 { 'TSR' } 2 { 'FSR' } 3 { 'DLSS' } 4 { 'XeSS' } default { "method $($set.'Altar.UpscalingMethod')" } }
        $txt = "render scale $([math]::Round([double]$set.'r.ScreenPercentage'))%, upscaler $up, frame generation $(if ([double]$set.'r.FidelityFX.FI.Enabled' -ge 1) { 'on (FSR)' } else { 'off (FSR)' }), " +
               "hardware RT $(if ([double]$set.'r.Lumen.HardwareRayTracing' -ge 1) { "on (lighting mode $([int][double]$set.'r.Lumen.HardwareRayTracing.LightingMode'))" } else { 'off' }), " +
               "view distance $(& $lv $set.'sg.ViewDistanceQuality'), shadows $(& $lv $set.'sg.ShadowQuality'), GI $(& $lv $set.'sg.GlobalIlluminationQuality'), " +
               "reflections $(& $lv $set.'sg.ReflectionQuality'), foliage $(& $lv $set.'sg.FoliageQuality'), textures $(& $lv $set.'sg.TextureQuality'), " +
               "effects $(& $lv $set.'sg.EffectsQuality'), post-processing $(& $lv $set.'sg.PostProcessQuality'), shading $(& $lv $set.'sg.ShadingQuality')"
        $md.Add("| Settings (the player's) | $txt |")
    }
}
$caps = @(@($baseRuns) + @($testRuns) | ForEach-Object { $_.R.sessions } | ForEach-Object { $_.frameCap } | Where-Object { $_ } | Select-Object -Unique)
if ($caps.Count) { $md.Add("| Frame cap | fps capped at $($caps -join ' / ') by $(if ($first.profile.own) { "the player's settings" } else { 'the profile' }) |") }
elseif ($first.profile.own -and @($testRuns | ForEach-Object { $_.R.sessions } | ForEach-Object { $_.scenes } | Where-Object { $_.frameGen }).Count) {
    # no cap read back (t.MaxFPS 0) with frame generation on: either no limit saved, or the stock game's start-up bug (with
    # frame generation on the saved Frame Rate Limit is applied only once the Graphics page is opened) where
    # FrameRateLimitAtStart does not act: key 0, or (since 1.2.2) DLSS frame generation / NVIDIA Reflex
    $md.Add("| Frame cap | none in effect (t.MaxFPS 0), stock and fix alike: either no frame-rate limit is saved, or the game did not apply it after the start (a game bug with frame generation on: it does once you open the Graphics settings; FrameRateLimitAtStart=1 applies it with FSR frame generation, not with DLSS frame generation or NVIDIA Reflex) |")
}
$md.Add("| Runs | stock $($baseRuns.Count), fix $($testRuns.Count) (medians; alternated) |")
$md.Add("| Stock | the fix loaded with every switch off except FrameRateLimitAtStart (a game bug fix, not a speed-up: it applies the frame-rate limit you saved where it can), so both sides run at the same frame-rate cap |")
# Short of RAM: Windows pages and the game hitches far more (2-5x the hitches seen at < 3 GB free): such runs are not comparable
$lowRam = @(@($baseRuns) + @($testRuns) | Where-Object {
    @($_.R.sessions | ForEach-Object { $_.scenes } | Where-Object { $null -ne $_.availMinMB -and [double]$_.availMinMB -lt 3072 }).Count } | ForEach-Object Name)
if ($lowRam.Count) { $md.Add("| **Not comparable** | less than 3 GB of RAM was free during $($lowRam -join ', '): close other programs and run it again |") }
# On screen (-Watch): the game must stay the foreground window while it is measured. Behind another window it turns frame
# generation off (FSR3 off on focus loss) and lowers its sound, and Windows schedules it as a background program.
$onScreen = @(@($baseRuns) + @($testRuns) | ForEach-Object { $_.R.sessions } | ForEach-Object { $_.scenes } | Where-Object { $null -ne $_.notForegroundPct })
$md.Add("| Game window | $(if ($onScreen.Count) { "on screen, in front$(if (($m = ($onScreen | Measure-Object notForegroundPct -Maximum).Maximum) -gt 0) { " (not in front for up to $m% of a measured flight)" })" } else { 'hidden and muted (the game was never in front)' }) |")
$notFront = @(@($baseRuns) + @($testRuns) | Where-Object {
    @($_.R.sessions | ForEach-Object { $_.scenes } | Where-Object { $null -ne $_.notForegroundPct -and [double]$_.notForegroundPct -gt 2 }).Count } | ForEach-Object Name)
if ($notFront.Count) { $md.Add("| **Not comparable** | another window was in front of the game for more than 2% of the measured time in $($notFront -join ', '); behind another window the game turns frame generation off and Windows gives it less time: run it again without switching windows |") }
# The benchmark's UE4SS build (tools\Setup-Bench.ps1 writes third_party\ue4ss\oblvbench-build.txt: name, sha256=, tested=)
$ue4ssMark = if ($env:OBLIVION_UE4SS_DIR) { Join-Path $env:OBLIVION_UE4SS_DIR 'oblvbench-build.txt' } else { Join-Path $root 'third_party\ue4ss\oblvbench-build.txt' }
if (Test-Path -LiteralPath $ue4ssMark) {
    $um = @(Get-Content -LiteralPath $ue4ssMark)
    $uSha = "$($um | Where-Object { $_ -like 'sha256=*' } | Select-Object -First 1)" -replace '^sha256=', ''
    $uTested = "$($um | Where-Object { $_ -like 'tested=*' } | Select-Object -First 1)" -replace '^tested=', ''
    $uText = $um[0].Trim()
    if ($uTested) { $uText += " ($uTested)" }
    if ($uSha) { $uText += ', SHA-256 `' + $uSha + '`' }
    $md.Add("| Benchmark's UE4SS | $uText |")
}
$md.Add('')
$md.Add('Each cell is stock -> fix (change). `~` = under 5%, within run-to-run noise. Game / Render / RHI = CPU ms per frame')
$md.Add('on that thread; GPU = GPU busy ms per frame.')
$md.Add('')
$static = @($bs.Keys | Where-Object { $ts.Contains($_) -and $bs[$_][0].type -eq 'static' })
if ($static.Count) {
    $md.Add('## Steady views')
    $md.Add('')
    $md.Add('| Scene | fps | 1% low | Game ms | Render ms | RHI ms | GPU ms |')
    $md.Add('|---|---|---|---|---|---|---|')
}
foreach ($id in $static) {
    $b = $bs[$id]; $t = $ts[$id]
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
    $md.Add('| Scene | fps | 1% low | worst frame (ms) | hitches | Game ms | GPU ms |')
    $md.Add('|---|---|---|---|---|---|---|')
    foreach ($id in $moving) {
        $b = $bs[$id]; $t = $ts[$id]
        $md.Add("| $id | $(Cell (Value $b fps) (Value $t fps)) | $(Cell (Value $b low1) (Value $t low1)) | $(Cell (Value $b worstMs) (Value $t worstMs) 'F0') | $(Cell (Value $b hitches) (Value $t hitches) 'F0') | $(Cell (Value $b threads.GameThread.msPerFrame) (Value $t threads.GameThread.msPerFrame) 'F2') | $(Cell (Value $b gpuBusyMs) (Value $t gpuBusyMs) 'F1') |")
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
