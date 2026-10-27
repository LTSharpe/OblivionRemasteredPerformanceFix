<#
.SYNOPSIS
  The benchmark kit's entry point (Benchmark.cmd, Benchmark-Hidden.cmd and Check-GPU.cmd call it). For players: no
  PowerShell, Build Tools, git or Python knowledge needed. Each mode:
  1. gets the third-party tools if they are missing (tools\Setup-Bench.ps1: PresentMon, UE4SS from its own project);
  2. Benchmark: makes sure this kit's fix is installed in the game (asks once before installing or replacing
     anything, and backs up what it replaces into backup\<time>\);
  3. runs, at the player's own settings, with the game on screen, in front and with the player's sound (Run-Benchmark
     -Watch), so the player sees and feels stock vs fix; with -Hidden hidden and muted instead (to use the PC meanwhile):
     Benchmark  the Imperial City flight (scenario showcase), stock vs fix in turn (2 rounds = 4 game starts, about
                15-20 minutes); each game waits at the main menu until its start-up shader precompile is done, then
                flies; ends with the report path (and opens it).
     GpuCheck   the pond by Fort Doublecross in the rain (scenario gpu-check), 30 s measured: is the graphics card or
                the CPU the limit there? Change settings in the game, run again.
  The game's settings and saves are put back after every run (as the player had them when the run started).

.PARAMETER Mode
  Benchmark (default) or GpuCheck.
.PARAMETER Hidden
  The game hidden and muted (never in front, no taskbar button), so the PC can be used meanwhile (Benchmark-Hidden.cmd).
.PARAMETER Rounds
  Benchmark: stock/fix pairs (default 2).
.PARAMETER InstallFix
  Ask (default): ask before installing this kit's fix. Yes / No: don't ask (No = measure what is installed).
.PARAMETER NoOpen
  Don't open the report at the end ($env:OBLV_KIT_NO_OPEN=1 works too).
.PARAMETER VerdictFrom
  GpuCheck only: print the verdict for an existing gpu-check run folder (no game, no setup).

.EXAMPLE
  Benchmark.cmd
  Benchmark.cmd -Rounds 3
  Benchmark-Hidden.cmd
  Check-GPU.cmd -Hidden
#>
[CmdletBinding()]
param(
    [ValidateSet('Benchmark', 'GpuCheck')][string]$Mode = 'Benchmark',
    [switch]$Hidden,
    [int]$Rounds = 2,
    [ValidateSet('Ask', 'Yes', 'No')][string]$InstallFix = 'Ask',
    [switch]$NoOpen,
    [string]$VerdictFrom
)
$ErrorActionPreference = 'Stop'
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$root = Split-Path $PSScriptRoot
$tools = $PSScriptRoot
if ($env:OBLV_KIT_NO_OPEN -eq '1') { $NoOpen = [switch]$true }
if ($VerdictFrom) { $Mode = 'GpuCheck' }
. (Join-Path $tools 'GamePaths.ps1')

function Say([string]$s = '') { Write-Host $s }
function Head([string]$s) { Write-Host ''; Write-Host "== $s" -ForegroundColor Cyan }
function Stop-Kit([string]$why) {
    Write-Host ''
    Write-Host $why -ForegroundColor Yellow
    Write-Host 'KIT-RESULT stopped'
    exit 1
}

$kitVersion = if (Test-Path -LiteralPath (Join-Path $root 'VERSION.txt')) { (Get-Content -LiteralPath (Join-Path $root 'VERSION.txt') -TotalCount 1).Trim() } else { $null }
$title = switch ($Mode) { 'Benchmark' { 'Imperial City flight: the game without and with the fix' } 'GpuCheck' { 'Graphics card check: the pond by Fort Doublecross in the rain' } }
# on screen, in front, with sound (default) or hidden and muted (-Hidden); the runner's -Watch = -Visible -Audible -Profile own
$show = if ($Hidden) { '-Profile own' } else { '-Watch' }
Say "OblivionPerfFix benchmark kit$(if ($kitVersion) { " $kitVersion" }): $title"
if (-not $VerdictFrom -and (Test-Path -LiteralPath (Join-Path $root 'third_party\PresentMon-2.6.0-x64.exe'))) {
    $access = Get-PresentMonAccess
    if (-not $access.Allowed) { Stop-Kit $access.Message }
}
# What is about to happen, in plain words (the README's "Check it on your PC" has the same numbered steps)
if ($Mode -eq 'Benchmark' -and -not $VerdictFrom) {
    $n = 2 * $Rounds
    Say ''
    Say 'This measures Oblivion Remastered on your PC without and with the OblivionPerfFix mod. What happens:'
    Say '  1. Benchmark tools: the first time, two free measuring tools are downloaded from their own GitHub projects'
    Say '     (PresentMon from Intel, UE4SS) into this folder.'
    Say '  2. The fix: if this version is not installed, you are asked first; anything replaced is backed up here.'
    Say "  3. The flights: the game opens by itself $n times, in turn WITHOUT the fix and WITH it. Each time it waits at"
    Say '     the main menu while it prepares its shaders (about 2 minutes, less with the fix; the game looks idle), flies over the Imperial'
    Say "     City (about 2 minutes) and closes. About $(4 * $n + 2) minutes in all$(if ($Hidden) { ', hidden and muted' } else { ', on screen with your sound' })."
    Say '  4. The report opens. Your saves are never used; your game settings are put back after every run.'
    if (-not $Hidden) { Say "  Please don't touch the mouse or keyboard or switch windows until the report opens." }
    Say '  To stop early: press Ctrl+C or close this window. The game then closes by itself (within about a minute) and'
    Say '  your settings and the game folder are put back as they were (UE4SS taken out again).'
}

# Files from a downloaded zip carry Windows' "downloaded from the internet" mark: clear it on the kit's own files
# (the .cmd already lets this PowerShell run them; this keeps any helper started later from being refused).
foreach ($d in 'tools', 'bench', 'mods', 'fix') {
    $p = Join-Path $root $d
    if (Test-Path -LiteralPath $p) { Get-ChildItem -LiteralPath $p -Recurse -File | Unblock-File -ErrorAction SilentlyContinue }
}

if ($VerdictFrom) { $Mode = 'GpuCheck' } else {
# ---------------------------------------------------------------- the game
try { $game = Get-OblivionGame } catch { Stop-Kit "Oblivion Remastered was not found: $($_.Exception.Message)`nThe benchmark needs the Steam build of the game (the Game Pass version is not supported). For a copy of it outside Steam's libraries, set OBLIVION_GAME_DIR to the game root folder." }
$win64 = $game.Win64
$running = Get-Process OblivionRemastered-Win64-Shipping -ErrorAction SilentlyContinue
if ($running) {
    # a game this kit's benchmark left at the main menu is closed; any other (the player's own) must be closed first
    $sessFile = Join-Path (Get-BenchRuntimeDir $root) 'bench-session.json'
    $ours = $false
    if (Test-Path -LiteralPath $sessFile) { try { $ours = "$((Get-Content -LiteralPath $sessFile -Raw | ConvertFrom-Json).key)" -like "$($running[0].Id)@*" } catch { } }
    if (-not $ours) { Stop-Kit 'Oblivion Remastered is running. Save your game, quit it, then start this again.' }
    Say 'Closing the benchmark game left from an earlier run...'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'Run-Benchmark.ps1') -Close | Out-Null
}

# ---------------------------------------------------------------- 1. third-party tools
Head "$(if ($Mode -eq 'Benchmark') { '1. ' })Benchmark tools (PresentMon, UE4SS: downloaded once from their own projects)"
$setup = & (Join-Path $tools 'Setup-Bench.ps1') 2>&1 | ForEach-Object { "$_" }
$setup | Where-Object { $_.Trim() -and $_ -notlike 'SETUP-RESULT ok*' } | ForEach-Object { Say "  $_" }
if (-not ($setup | Where-Object { $_ -like 'SETUP-RESULT ok*' })) { Stop-Kit 'The benchmark tools are not ready (see "Needs attention" above).' }
$access = Get-PresentMonAccess
if (-not $access.Allowed) { Stop-Kit $access.Message }

# ---------------------------------------------------------------- 2. the fix
function Get-FixVersion([string]$dll) {
    if (-not (Test-Path -LiteralPath $dll)) { return $null }
    $m = [regex]::Match([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll)), 'OblivionPerfFix (\d+\.\d+\.\d+)')
    if ($m.Success) { $m.Groups[1].Value } else { $null }
}
function Test-OurDll([string]$dll) { (Test-Path -LiteralPath $dll) -and (Select-String -LiteralPath $dll -Pattern 'OBSEPlugin_Load' -SimpleMatch -Quiet) }

<# The new ini with the player's values kept for every key both have (new keys get this version's defaults). #>
function Merge-FixIni([string]$newIni, [string]$oldIni) {
    $old = @{}; $sec = ''
    foreach ($l in Get-Content -LiteralPath $oldIni) {
        if ($l -match '^\s*\[(.+?)\]') { $sec = $Matches[1]; continue }
        if ($l -match '^\s*([A-Za-z0-9_.]+)\s*=\s*(.*?)\s*$') { $old["$sec|$($Matches[1])"] = $Matches[2] }
    }
    $kept = 0; $sec = ''
    $out = foreach ($l in Get-Content -LiteralPath $newIni) {
        if ($l -match '^\s*\[(.+?)\]') { $sec = $Matches[1]; $l; continue }
        if ($l -match '^(\s*)([A-Za-z0-9_.]+)(\s*=\s*)(.*?)\s*$' -and $old.ContainsKey("$sec|$($Matches[2])") -and $old["$sec|$($Matches[2])"] -ne $Matches[4]) {
            $kept++; "$($Matches[1])$($Matches[2])$($Matches[3])$($old["$sec|$($Matches[2])"])"
        } else { $l }
    }
    [pscustomobject]@{ Lines = @($out); Kept = $kept }
}

if ($Mode -ne 'GpuCheck') {
    Head '2. The fix'
    $kitDll = Join-Path $root 'fix\version.dll'
    $kitIni = Join-Path $root 'fix\OblivionPerfFix.ini'
    $dll = Join-Path $win64 'version.dll'
    $ini = Join-Path $win64 'OblivionPerfFix.ini'
    $obse = Join-Path $win64 'OBSE\Plugins\OblivionPerfFix.dll'
    $haveVer = if (Test-OurDll $dll) { Get-FixVersion $dll } else { $null }
    if (-not (Test-Path -LiteralPath $kitDll)) {
        # a repository checkout: no fix files here, measure what is installed
        if (-not $haveVer) { Stop-Kit "OblivionPerfFix is not installed as $dll. Install it (README), then run this again." }
        Say "  OblivionPerfFix $haveVer is installed (measured as it is)."
    } else {
        $kitVer = Get-FixVersion $kitDll
        $same = $haveVer -and ((Get-FileHash -LiteralPath $dll).Hash -eq (Get-FileHash -LiteralPath $kitDll).Hash)
        if ($same) { Say "  OblivionPerfFix $kitVer is installed (the version of this kit)." }
        else {
            if ((Test-Path -LiteralPath $dll) -and -not $haveVer) {
                Stop-Kit "$dll belongs to another mod, not to OblivionPerfFix. The benchmark needs OblivionPerfFix as version.dll: remove or rename that mod's version.dll for the benchmark, or measure by hand (docs\benchmark.md)."
            }
            $what = if ($haveVer -eq $kitVer) { "Another build of OblivionPerfFix $haveVer is installed (not the one in this kit)." }
                    elseif ($haveVer) { "OblivionPerfFix $haveVer is installed; this kit is for $kitVer." }
                    elseif (Test-Path -LiteralPath $obse) { "OblivionPerfFix is installed as an OBSE64 plugin. The benchmark needs it as version.dll for its launch path (with both, the fix loads once)." }
                    else { 'OblivionPerfFix is not installed.' }
            Say "  $what"
            $answer = $InstallFix
            if ($answer -eq 'Ask') {
                $r = Read-Host "  Install OblivionPerfFix $kitVer into the game now? Files it replaces are backed up first. [Y/N]"
                $answer = if ($r -match '^\s*(y|yes|j|ja|o|oui|s|si|д|да)\s*$') { 'Yes' } else { 'No' }
            }
            if ($answer -eq 'Yes') {
                $backup = Join-Path $root ("backup\{0:yyyyMMdd-HHmmss}" -f (Get-Date))
                New-Item -ItemType Directory -Force $backup | Out-Null
                foreach ($f in $dll, $ini) { if (Test-Path -LiteralPath $f) { Copy-Item -LiteralPath $f -Destination $backup } }
                $merged = $null
                if (Test-Path -LiteralPath $ini) { $merged = Merge-FixIni $kitIni $ini }
                Copy-Item -LiteralPath $kitDll -Destination $dll -Force
                if ($merged) { [IO.File]::WriteAllLines($ini, [string[]]$merged.Lines, (New-Object Text.UTF8Encoding $false)) }
                else { Copy-Item -LiteralPath $kitIni -Destination $ini -Force }
                Say "  Installed OblivionPerfFix $kitVer$(if ($merged) { "; your OblivionPerfFix.ini settings were kept ($($merged.Kept) differ from this version's defaults)" })."
                if (@(Get-ChildItem -LiteralPath $backup -File).Count) { Say "  Your previous files are in $backup (to go back: copy them into $win64)." }
                else { Remove-Item -LiteralPath $backup }
            } elseif ($haveVer) { Say "  Not installing: measuring the installed OblivionPerfFix $haveVer instead." }
            else { Stop-Kit 'Nothing to compare without the fix. Run this again and answer Y, or install the fix by hand (README).' }
        }
    }
}

}

# ---------------------------------------------------------------- 3. run
$runner = Join-Path $tools 'Run-Benchmark.ps1'
function Get-Verdict([string[]]$lines) { @($lines | Where-Object { $_ -like 'BENCH-RESULT*' })[-1] }
<# Runs a benchmark script in its own PowerShell (as Bench-Report does for each run) and passes its lines through. #>
function Invoke-Bench([string]$script, [string]$argLine, [switch]$StepsOnly, [switch]$Plain) {
    $q = $script -replace "'", "''"
    # -StepsOnly: show the runner's time-stamped steps, warnings and verdict, not its report tables
    # -Plain: show only Bench-Report's plain-words progress lines ("KIT> ...", -Plain there), without the marker
    & powershell -NoProfile -ExecutionPolicy Bypass -Command "& '$q' $argLine" 2>&1 | ForEach-Object {
        $s = "$_"
        if ($Plain) { if ($s -like 'KIT> *') { Write-Host $s.Substring(5) } }
        elseif (-not $StepsOnly -or $s -match '^\[\d\d:\d\d:\d\d\]|^WARNING|^BENCH-RESULT') { Write-Host $s }
        $s
    }
}

if ($Mode -eq 'Benchmark') {
    Head "3. The flights: $(2 * $Rounds) runs, in turn without and with the fix (about $(8 * $Rounds + 2) minutes)"
    if ($Hidden) { Say '  The game runs hidden and muted: you can keep using the PC (heavy programs make the numbers noisier).' }
    else {
        Say '  The game comes up on screen, in front, with your sound: watch the flight without the fix, then with it.'
        Say '  Please do not touch the mouse or keyboard and do not switch windows until the report opens: a game that is not'
        Say '  in front turns frame generation off and gets less time from Windows (the report then marks the run "not comparable").'
    }
    Say '  "Stock" = the fix loaded with every switch off (the game''s own code runs), except the fix that makes the game'
    Say '  apply your saved frame-rate limit (a game bug), so both sides run at your cap. Your saves are never touched.'
    $out = Invoke-Bench (Join-Path $tools 'Bench-Report.ps1') "-Scenario showcase -Profile own$(if (-not $Hidden) { ' -Watch' }) -Rounds $Rounds -Session fresh -MenuPsoWait -Label icflight -Plain" -Plain
    $res = @($out | Where-Object { $_ -like 'REPORT-RESULT*' })[-1]
    if ("$res" -notmatch '^REPORT-RESULT ok (.+)$') {
        $why = @($out | Where-Object { $_ -match 'benchmark run .* failed|BENCH-RESULT (?!ok)' } | Select-Object -Last 1)
        Stop-Kit "The benchmark did not finish$(if ($why) { ": $($why[0] -replace '^.*?(benchmark run|BENCH-RESULT)', '$1')" }). Your settings were put back. The run folders under captures\bench say why (report.md in each)."
    }
    $report = Join-Path $Matches[1].Trim() 'report.md'
    Head '4. The report'
    Say "  Report: $report"
    Say '  To share it: open a "Benchmark result" issue at https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/new/choose,'
    Say '  paste report.md into it and attach the result-*.json files from the same folder (they hold no user or PC name).'
    if (-not $NoOpen) { try { Start-Process notepad.exe -ArgumentList "`"$report`"" } catch { } }
    Say 'KIT-RESULT ok'
    exit 0
}

# GpuCheck
if ($VerdictFrom) { $runDir = (Resolve-Path -LiteralPath $VerdictFrom).Path } else {
Head 'Graphics card check (about 3-4 minutes with the game start; 30 s measured in the rain by Fort Doublecross)'
Say $(if ($Hidden) { '  The game runs hidden and muted at your own settings.' } else { '  The game comes up on screen at your own settings. Please do not touch the mouse or keyboard or switch windows until the result shows.' })
$out = Invoke-Bench $runner "-Label gpu-check -Scenario gpu-check $show -Pass perf -Session fresh -After close -MenuPsoWait" -StepsOnly
$v = Get-Verdict $out
if ("$v" -notmatch '^BENCH-RESULT (ok|partial) \S+ dir=(.+?) warnings=') { $out | Select-Object -Last 15 | ForEach-Object { Say "  $_" }; Stop-Kit "The check did not finish: $v" }
$runDir = $Matches[2]
}
$r = Get-Content -LiteralPath (Join-Path $runDir 'result.json') -Raw | ConvertFrom-Json
$sess = @($r.sessions | Where-Object { $_.kind -eq 'perf' })[0]
$sc = @($sess.scenes | Where-Object { $_.id -eq 'doublecross-pond-rain' })[0]
if (-not $sc) { $sc = @($sess.scenes | Where-Object { $_.fps })[0] }      # -VerdictFrom another run: its first measured scene
if (-not $sc -or -not $sc.fps) { Stop-Kit "No frame data in $runDir (see its report.md)." }
$fg = [bool]$sc.frameGen
$rendered = if ($fg) { $sc.fps / 2 } else { $sc.fps }
$frameMs = 1000 / $rendered
$gpuUtil = if ($null -ne $sc.gpuBusyMs) { [math]::Min(1, $sc.gpuBusyMs * $sc.fps / 1000) } else { $null }
$gtPct = [double]$sc.threads.GameThread.pct
$cpuThreads = [ordered]@{ "the game's main thread" = $gtPct; 'the render thread' = [double]$sc.threads.RenderThread.pct; 'the RHI thread' = [double]$sc.threads.RHIThread.pct }
$busiest = @($cpuThreads.Keys | Sort-Object { $cpuThreads[$_] } -Descending)[0]
$cap = if ($sess.frameCap) { [double]$sess.frameCap } else { 0 }
$st = $sess.settings
function Level($v) { switch ([int][double]$v) { 0 { 'Low' } 1 { 'Medium' } 2 { 'High' } 3 { 'Ultra' } 4 { 'Cinematic' } default { "$v" } } }
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add(('Frame rate: {0:N0} fps{1}' -f $sc.fps, $(if ($fg) { (' shown, {0:N0} rendered (frame generation on)' -f $rendered) } else { '' })))
$lines.Add(('Per rendered frame ({0:N1} ms): graphics card busy {1}, game main thread busy {2:N1} ms{3}' -f $frameMs,
    $(if ($null -ne $gpuUtil) { '{0:N1} ms ({1:N0}% of the time)' -f ($gpuUtil * $frameMs), (100 * $gpuUtil) } else { 'not measured' }),
    ($gtPct / 100 * $frameMs), $(if ($cap) { "; frame cap $cap fps = $([math]::Round(1000 / $cap, 1)) ms" } else { '' })))
$lines.Add(('1% low: {0:N0} fps{2}; worst frame {1:N0} ms' -f $sc.low1, $sc.worstMs, $(if ($fg) { ' (shown frames)' } else { '' })))
$lines.Add('')
if ($cap -and $rendered -ge 0.95 * $cap) {
    $lines.Add("Verdict: your frame cap ($cap fps) is the limit here; the graphics card and the CPU keep up with it.")
    $lines.Add('There is room to spare at these settings in this place (towns and crowds need more from the CPU, not the graphics card).')
} elseif ($null -ne $gpuUtil -and $gpuUtil -ge 0.9) {
    $lines.Add('Verdict: your graphics card is the limit here.')
    $lines.Add('To get more fps: lower the settings that cost the graphics card most, one step at a time, and run this again:')
    $pct = [double]$st.'r.ScreenPercentage'
    $lines.Add("  - the upscaler's quality mode (Quality -> Balanced -> Performance: fewer pixels rendered$(if ($pct -gt 0) { "; now $([math]::Round($pct))% of the screen's resolution" }))")
    $lines.Add("  - Global Illumination (now $(Level $st.'sg.GlobalIlluminationQuality')), Reflections (now $(Level $st.'sg.ReflectionQuality')), Shadows (now $(Level $st.'sg.ShadowQuality')), Foliage (now $(Level $st.'sg.FoliageQuality'))")
    $lines.Add("  - hardware ray tracing (now $(if ([double]$st.'r.Lumen.HardwareRayTracing' -ge 1) { 'on' } else { 'off' }))")
    $lines.Add("Frame generation needs a good base frame rate: aim for about 50-60 rendered fps here before using it$(if ($fg) { ' (it is on now; it also costs the graphics card some time per frame)' }).")
} elseif ($cpuThreads[$busiest] -ge 85) {
    $lines.Add("Verdict: your CPU is the limit here ($busiest is busy $([math]::Round($cpuThreads[$busiest]))% of the time).")
    $lines.Add('Lowering graphics settings will not raise the frame rate much. The fix helps exactly here (and in towns and crowds).')
} else {
    $lines.Add(('Verdict: neither the graphics card ({0}) nor the CPU ({1} {2:N0}%) is fully busy here.' -f $(if ($null -ne $gpuUtil) { '{0:N0}%' -f (100 * $gpuUtil) } else { 'not measured' }), $busiest, $cpuThreads[$busiest]))
    $lines.Add('Something else holds the frame rate: V-Sync, a frame cap in the driver, or short stalls. Your settings have room here.')
}
$lines.Add('')
$lines.Add('Change settings in the game (start it from Steam as usual, change them, quit), then run Check-GPU.cmd again.')
$lines.Add("Your settings and saves are as you left them. Details: $(Join-Path $runDir 'report.md')")
Head 'Result'
$lines | ForEach-Object { Say "  $_" }
[IO.File]::WriteAllLines((Join-Path $runDir 'gpu-check.txt'), [string[]]$lines, (New-Object Text.UTF8Encoding $false))
Say 'KIT-RESULT ok'
exit 0
