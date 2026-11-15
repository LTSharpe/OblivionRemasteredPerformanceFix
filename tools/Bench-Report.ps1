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
            foreach ($sc in @($s.scenes)) {
                if (-not $map.Contains($sc.id)) { $map[$sc.id] = New-Object System.Collections.ArrayList }
                $sc | Add-Member -NotePropertyName _dir -NotePropertyValue $run.Dir -Force
                [void]$map[$sc.id].Add($sc)
            }
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


# ---------------------------------------------------------------- rendered frames from PresentMon (2026-11-12, worker dropcal)
# Stock has no per-frame log of the game thread, so stock and fix are compared through the runner's PresentMon capture
# (perf-frames.csv, written by every run). With frame generation the game presents two frames per rendered frame, back to
# back: a rendered frame = the pair, its period the sum of the two MsBetweenPresents. Reference implementation and tests:
# tools\analysis\report_line.py (the same numbers; the script checks them against each other).
if (-not ('PmReport' -as [type])) {
    try { Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
public class PmLine {
    public int Frames, Ge25, Ge33, Drops, Mild, Groups;
    public double Seconds, Fps, MeanMs, P99Ms, WorstMs, Ge25Pct, Ge33Pct, OverPerFrame;
}
public static class PmReport {
    const double CapMs = 1000.0 / 60.0;
    public static PmLine Read(string csv, double t0, double t1, bool frameGen, double graceS) {
        var cpuL = new List<double>(); var gapL = new List<double>();
        using (var sr = new StreamReader(csv)) {
            string h = sr.ReadLine(); if (h == null) return null;
            string[] hs = h.TrimStart('\uFEFF').Split(',');
            int ic = Array.IndexOf(hs, "CPUStartQPCTimeInMs"), ib = Array.IndexOf(hs, "MsBetweenPresents");
            if (ic < 0 || ib < 0) return null;
            string line; double lo = t0 - (frameGen ? 1000.0 : 0.0);
            while ((line = sr.ReadLine()) != null) {
                string[] f = line.Split(',');
                double c, b;
                if (f.Length <= Math.Max(ic, ib)) continue;
                if (!double.TryParse(f[ic], NumberStyles.Float, CultureInfo.InvariantCulture, out c)) continue;
                if (!double.TryParse(f[ib], NumberStyles.Float, CultureInfo.InvariantCulture, out b)) continue;
                double e = c + b;
                if (e >= lo && e <= t1) { cpuL.Add(c); gapL.Add(b); }
            }
        }
        int m = gapL.Count;
        var T = new List<double>(); var P = new List<double>();
        if (!frameGen) {
            for (int i = 0; i < m; i++) { double e = cpuL[i] + gapL[i]; if (e >= t0) { T.Add(e); P.Add(gapL[i]); } }
        } else {
            int first = -1, ncore = 0;
            for (int i = 0; i < m; i++) if (cpuL[i] + gapL[i] >= t0) { if (first < 0) first = i; ncore++; }
            if (first < 0 || ncore <= 3) return null;
            double s0 = 0, s1 = 0; int n0 = 0, n1 = 0, half = ncore / 2;
            for (int i = first; i < m && n0 < half; i += 2) { s0 += gapL[i]; n0++; }
            for (int i = first + 1; i < m && n1 < half; i += 2) { s1 += gapL[i]; n1++; }
            double m0 = s0 / Math.Max(n0, 1), m1 = s1 / Math.Max(n1, 1);
            int ph = (m0 < m1) ? first % 2 : (first + 1) % 2;
            int k = (m - ph) / 2 * 2;
            for (int j = 0; j < k; j += 2) {
                double e = cpuL[ph + j + 1] + gapL[ph + j + 1];
                if (e >= t0 && e <= t1) { T.Add(e); P.Add(gapL[ph + j] + gapL[ph + j + 1]); }
            }
        }
        int n = T.Count;
        var r = new PmLine();
        if (n == 0) return r;
        double[] t = T.ToArray(), per = P.ToArray(), st = new double[n];
        for (int i = 0; i < n; i++) st[i] = t[i] - per[i];
        // play = not a menu-length gap; grace = the first graceS seconds and 3 s after each gap of 1 s or more
        var play = new bool[n]; var gok = new bool[n]; double until = st[0] + graceS * 1000.0;
        for (int i = 0; i < n; i++) {
            play[i] = per[i] < 1000.0;
            if (per[i] >= 1000.0) until = Math.Max(until, t[i] + 3000.0);
            gok[i] = st[i] >= until;
        }
        var x = new List<double>(); double sum = 0, over = 0, worst = 0;
        for (int i = 0; i < n; i++) if (play[i] && gok[i]) {
            x.Add(per[i]); sum += per[i]; if (per[i] > worst) worst = per[i];
            if (per[i] >= 25.0) r.Ge25++; if (per[i] >= 33.3) r.Ge33++;
            if (per[i] > CapMs) over += per[i] - CapMs;
        }
        int c2 = x.Count; if (c2 == 0) return r;
        r.Frames = c2; r.Seconds = sum / 1000.0; r.Fps = c2 / r.Seconds; r.MeanMs = sum / c2; r.WorstMs = worst;
        r.Ge25Pct = 100.0 * r.Ge25 / c2; r.Ge33Pct = 100.0 * r.Ge33 / c2; r.OverPerFrame = over / c2;
        double[] xs = x.ToArray(); Array.Sort(xs);
        double pos = (c2 - 1) * 0.99; int pl = (int)Math.Floor(pos); int ph2 = Math.Min(pl + 1, c2 - 1);
        r.P99Ms = xs[pl] + (xs[ph2] - xs[pl]) * (pos - pl);
        // bursts of frames >= 25 ms merged when less than 0.5 s apart; peak >= 28 = drop, 25-28 = mild; sudden = the
        // peak is >= 11 (mild: 8) ms above the median of the half second before (needs 8 play frames, else counted)
        var groups = new List<List<int>>();
        for (int i = 0; i < n; i++) {
            if (!(per[i] >= 25.0 && play[i])) continue;
            if (groups.Count > 0) { var g = groups[groups.Count - 1]; if (st[i] - t[g[g.Count - 1]] < 500.0) { g.Add(i); continue; } }
            groups.Add(new List<int> { i });
        }
        r.Groups = groups.Count;
        foreach (var g in groups) {
            int i0 = g[0], ip = g[0];
            foreach (int i in g) if (per[i] > per[ip]) ip = i;
            double peak = per[ip];
            int j0 = 0; double cut = st[i0] - 500.0;
            while (j0 < n && t[j0] <= cut) j0++;
            var bs = new List<double>();
            for (int i = j0; i < i0; i++) if (play[i]) bs.Add(per[i]);
            bool have = bs.Count >= 8; bool sudden = true;
            if (have) {
                bs.Sort(); int bc = bs.Count;
                double med = (bc % 2 == 1) ? bs[bc / 2] : (bs[bc / 2 - 1] + bs[bc / 2]) / 2.0;
                sudden = (peak - med) >= (peak >= 28.0 ? 11.0 : 8.0);
            }
            if (!gok[i0] || !sudden) continue;
            if (peak >= 28.0) r.Drops++; else r.Mild++;
        }
        return r;
    }
}
'@
    } catch { Write-Warning "the PresentMon reader did not compile ($($_.Exception.Message)): the 'Rendered frames' section is left out" }
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
        # Altar.UpscalingMethod, read from the game's apply function (exe+0x4974BC0, Ghidra, 2026-11-05): 1 switches on the DLSS
        # cvar, 2 FSR3, 3 the XeSS cvar (its handler sets r.XeSS.Enabled); 0 uses the engine's own scaling; 4 does nothing.
        # This agrees with issue #24, where an XeSS setup was printed as value 3. (Before: 1 TSR, 3 DLSS, 4 XeSS, which was wrong.)
        $up = switch ([int][double]$set.'Altar.UpscalingMethod') { 0 { 'none (the engine''s TSR)' } 1 { 'DLSS' } 2 { 'FSR' } 3 { 'XeSS' } default { "method $($set.'Altar.UpscalingMethod')" } }
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

# Rendered frames from PresentMon: the same source for stock and fix (the stock side has no frame log of its own).
$pmRows = New-Object System.Collections.Generic.List[string]
foreach ($id in $(if ('PmReport' -as [type]) { $moving } else { @() })) {
    $side = @{}
    foreach ($pair in @(@('stock', $bs[$id]), @('fix', $ts[$id]))) {
        $res = New-Object System.Collections.ArrayList
        foreach ($sc in $pair[1]) {
            $csv = if ($sc._dir) { Join-Path $sc._dir 'perf-frames.csv' } else { $null }
            if (-not $csv -or -not (Test-Path -LiteralPath $csv) -or $null -eq $sc.t0Ms -or $null -eq $sc.t1Ms) { continue }
            $grace = if ($sc.type -eq 'walk') { 5.0 } else { 3.0 }
            $o = [PmReport]::Read($csv, [double]$sc.t0Ms, [double]$sc.t1Ms, [bool]$sc.frameGen, $grace)
            if ($o -and $o.Frames -gt 0) { [void]$res.Add($o) }
        }
        $side[$pair[0]] = $res
    }
    if (-not $side.stock.Count -or -not $side.fix.Count) { continue }
    $pm = { param($name, $fmt) $b = Median @($side.stock | ForEach-Object { $_.$name }); $t = Median @($side.fix | ForEach-Object { $_.$name }); '{0} -> {1}' -f $b.ToString($fmt), $t.ToString($fmt) }
    $pmRows.Add("| $id | $(& $pm 'Fps' 'F0') | $(& $pm 'Ge25Pct' 'F1') | $(& $pm 'Ge33Pct' 'F2') | $(& $pm 'WorstMs' 'F0') | $(& $pm 'OverPerFrame' 'F2') | $(& $pm 'Drops' 'F0') |")
}
if ($pmRows.Count) {
    $md.Add('')
    $md.Add('## Rendered frames, stock against fix (from PresentMon)')
    $md.Add('')
    $md.Add('The same source for both sides: the PresentMon capture of each run, because the stock game has no frame log of its own. With frame')
    $md.Add('generation the game presents two frames per rendered frame, so a rendered frame is the pair of presents (its time the sum of the two')
    $md.Add('gaps); without it every present is a frame. Only the timed part of each flight or ride counts (without its first 3 s / 5 s). Cells are')
    $md.Add('stock -> fix (medians over the runs). Rendered fps = rendered frames per second. Frames >= 25 ms and >= 33 ms are shares of the')
    $md.Add('rendered frames. Over 16.7 = mean of (frame - 16.7 ms, at least 0) per rendered frame: how far the game is, on average, behind a')
    $md.Add('steady 60 fps.')
    $md.Add('')
    $md.Add('| Scene | rendered fps | frames >= 25 ms (%) | frames >= 33 ms (%) | worst frame (ms) | ms over 16.7 per frame | sudden drops (PresentMon) |')
    $md.Add('|---|---|---|---|---|---|---|')
    foreach ($row in $pmRows) { $md.Add($row) }
    $md.Add('')
    $md.Add('Sudden drops (PresentMon reading) = bursts of rendered frames >= 25 ms (closer than 0.5 s counted as one) whose worst frame is >= 28 ms')
    $md.Add('and at least 11 ms above the median of the half second before: the stutter you see as the frame rate falling suddenly from 60 to 20-30 and')
    $md.Add('back. **Compare this number only with other PresentMon counts** (stock with fix, here), not with the fix log''s own drop count:')
    $md.Add('PresentMon sees the presented frame cadence, which adds single-frame spikes the game itself does not have, so it counts about 3-5')
    $md.Add('times as many. A stock run that is slow all along (it often never holds 60) merges its slow stretches into a few long bursts, so its')
    $md.Add('drop count says little: read the shares and the ms over 16.7.')
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
