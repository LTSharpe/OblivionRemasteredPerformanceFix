<#
.SYNOPSIS
  Unattended benchmark. Gets the game into the world (starting it through Steam, or directly for a non-Steam
  OBLIVION_GAME_DIR install, or reusing a healthy
  game the benchmark left at the main menu), applies a display/graphics profile (bench\profiles.json),
  switches to spectator mode (cinematic camera, no HUD, no head bob, god mode, nobody notices the
  player), runs the selected scenes (bench\scenarios\*.json) through the OblvBench UE4SS mod and writes
  the results to captures\bench\<time>-<label>\:
    result.json   per-scene frame statistics, per-thread CPU, effective settings, restarts
    report.md     the same as tables
    *-frames.csv  PresentMon log of the pass (QPC time base)
    *-threads.csv per-second CPU per thread name, I/O, memory
    shots\ plots\ one screenshot per scene, frame-time graphs of moving scenes and transitions
    visual\       frozen, frame-averaged reference images for Compare-Benchmark.ps1
    crash-<n>\    what a crash, hang or driver timeout left behind (crash report, logs), one per restart
  A watchdog notices within seconds when the game exits, crashes, hangs, stalls or shows an error; the
  runner then closes what is left on screen, restarts the game and carries on with the remaining
  scenes (a scene that takes the game down twice is skipped). Nothing waits on a game that cannot
  answer, and the whole run has a time limit. bench-status.json in the runtime folder says what it is doing
  (tools\Bench-Status.ps1), and the last line printed is "BENCH-RESULT <ok|partial|failed> ...".
  Afterwards the game is parked at the main menu for the next run (or closed: see -After), and
  autosaves and config files touched by the run are restored once it has closed.

.EXAMPLE
  .\Run-Benchmark.ps1 -Label baseline                                   # all Cheydinhal scenes, small quiet window (profile small)
  .\Run-Benchmark.ps1 -List                                             # every scene and every profile
  .\Run-Benchmark.ps1 -Label t -Scenes inn-door -Pass perf              # one scene
  .\Run-Benchmark.ps1 -Label t -Tags traversal -Pass perf -Profile native # all scenes with a tag, at the monitor's resolution with FG
  .\Run-Benchmark.ps1 -Label t -Scenes 'canal-*' -Session fresh         # confirm in a newly started game
  .\Run-Benchmark.ps1 -Close                                            # close the parked game, restore saves and config
  .\Run-Benchmark.ps1 -Label poolsize -Cvars @{ 'r.Streaming.PoolSize' = 6000 }
  .\Run-Benchmark.ps1 -Label nofix -FixIni @{ BoneUploadStores = 0; AtmosphereDirtyFilter = 0 } -Pass perf
#>
[CmdletBinding()]
param(
    [string]$Label = 'run',
    [string]$Scenario,                                   # bench\scenarios\<name>.json. Default: cheydinhal; full when -Scenes, -Tags or -List is given
    [string[]]$Scenes,                                   # scene ids, wildcards allowed: inn-door, canal-*, *flight
    [string[]]$Tags,                                     # scenes with any of these tags (the scene type counts as a tag): traversal, city, actors, transition, dungeon, ...
    [switch]$List,                                       # show the selected scenes, how long they take, and the profiles, then exit
    [ValidateSet('all', 'perf', 'visual')][string]$Pass = 'all',
    [string]$Profile,                                    # bench\profiles.json: small (default), cpu, cpu-nort, cpu-min, native, native-nofg, gpu, gpu-max. Default: the scenario's "profile", else small
    [ValidateSet('fresh', 'continue', 'quicksave')][string]$Start,  # fresh: new game with the default character, no save needed; continue: the latest save; quicksave: quicksave.sav. Default: the scenario's "start", else fresh
    [hashtable]$Cvars = @{},                             # console variables set after loading (the change under test)
    [hashtable]$IniCvars = @{},                          # variables that must be in Engine.ini [SystemSettings] at start-up
    [hashtable]$FixIni = @{},                            # OblivionPerfFix.ini keys for this run
    [hashtable]$Ini = @{},                               # other config: @{ 'Altar.ini|/Script/Module.Class' = @{ Key = 'Value' } } (files in Saved\Config\Windows)
    [int]$Runs = 1,                                      # repeat the whole thing (separate result folders)
    # Which game process: auto = reuse a healthy game this benchmark started if its start-up settings (-IniCvars,
    # -FixIni, -Ini, the fix DLL, the mod bootstrap) are unchanged and it is younger than -MaxSessionMin, else
    # restart; fresh = a newly started game for every pass (confirmation runs); reuse = whatever game is running
    [ValidateSet('auto', 'fresh', 'reuse')][string]$Session = 'auto',
    [Alias('Attach')][switch]$Reuse,                     # old name of -Session reuse
    # Afterwards: auto = park at the main menu (closed instead after -Session fresh, a crash, or past -MaxSessionMin);
    # park; close (and restore saves and config); keep (stay in the world, e.g. to look at it)
    [ValidateSet('auto', 'park', 'close', 'keep')][string]$After = 'auto',
    [switch]$Close,                                      # close the game left open by earlier runs, restore saves and config
    [switch]$Park,                                       # put the running benchmark game at the main menu and exit
    [switch]$Stop,                                       # ask the runner that holds the game (or -StopPid) to stop after its current scene; later runs in that process refuse to start. Never Stop-Process a runner or a chain
    [int]$StopPid = 0,                                   # -Stop: the runner (or chain) process to stop; default: the one in game.lock / bench-status.json
    [string]$StopReason = '',                            # -Stop: why (shown in that run's warnings)
    [switch]$NoSpectate,                                 # use the game's own camera and HUD (default: spectator mode)
    [switch]$KeepWorld,                                  # do not start a fresh world for each pass when the game is already in one
    [switch]$MenuReturn,                                 # frame-generation profiles: go back to the main menu for a fresh world anyway (it can hang there, so by default such a game is restarted instead)
    [int]$MaxRestarts = 2,                               # game restarts after a crash or hang per pass before giving up
    [double]$MaxMinutes = 0,                             # time limit for one run (0: from the scene estimate)
    [double]$MaxSessionMin = 60,                         # -Session auto restarts a game older than this (UE4SS's Lua bindings add ~1 GB/h in a busy session; fps itself did not drift in 105 min, 2026-10-06)
    [double]$IdleCloseMin = 20,                          # a parked game is closed (saves and config restored) after this long without a run; 0 = never
    [double]$OtherCoresWarn = 8,                         # warn when other processes used this many cores over any ~1 s of a scene (a parallel build: ~9-10.5; quiet PC with the harness: 3-6.5; 2026-10-26)
    [string[]]$Stacks,                                   # thread names to stack-sample during door transitions, flights and walks (for Analyze-Hitch.ps1); perturbs timing; wildcards work ('*Worker*')
    [ValidateSet('dbghelp', 'fast')][string]$StackSampler = 'dbghelp',   # fast: FastSampler.cs, ~30 us per thread sample (all workers at a few hundred Hz); dbghelp: the old ~1 ms sampler
    [double]$StackIntervalMs = 2,                        # -StackSampler fast: one sweep over all sampled threads every n ms
    [int]$LegShots = 0,                                  # door legs: also capture the window every n ms (shots\<scene>-<leg>\<ms after activation>.png): what the player sees during a transition; perturbs timing a little
    [switch]$Force,                                      # ignore the "game in use" check; may restart a game the benchmark did not start
    [switch]$Visible,                                    # let Steam and the game come to the front as usual (default: the game window stays off-screen, never takes the focus, no taskbar button; Steam's launch dialog is hidden)
    [switch]$Audible,                                    # keep the game's sound (default: muted in the volume mixer during runs; un-muted before the benchmark closes it)
    [switch]$Watch,                                      # to watch a run: -Visible -Audible and, unless -Profile is given, profile "own" (your settings as they are)
    [switch]$Record,                                     # promo: record each flight scene with OBS (tools\Obs.ps1, docs\promo-video.md) into <run>\video\: from the hover in black to after the fade-out
    [switch]$Pin,                                        # the same conditions in every run: weather (scene -> scenario default -> Clear) held with the weather override, hour (-> 12), timescale (-> 1); a scene opts out with "keep". On by default since 2026-10-20 (user); a scenario's "pin": false turns it off (docs\benchmark.md "Identical runs")
    [switch]$NoPin,                                      # the old behaviour: only what a scene names is set (same as -Pin:$false)
    [switch]$NoFixedSeed,                                # launch the game without UE's -FixedSeed (on by default since 2026-10-20: the engine's random generators start from the same seed in every benchmark game; passed to this launch only, never saved to store launch settings)
    [switch]$SkipPsoWait,                                # lab only: do not wait for the game's PSO precompile after a launch (to measure what it costs)
    [switch]$NoWarmup,                                   # lab only: start the scenes without the shader warm-up wait (driver compilation quiet) either (start-up measurements, 2026-10-23)
    [double]$MenuHoldSec = 0,                            # lab only: after a launch, stay this long at the main menu before entering the world (a player looking at the menu while the precompile runs)
    [switch]$MenuPsoWait,                                # after a launch, wait at the main menu (hidden) until the game's start-up PSO precompile has finished (at most 6 min), then enter the world: the scenes start in the same state with or without the fix, with no warm-up scene (the benchmark kit, 2026-10-24)
    [int]$StallSec = 0,                                  # lab only: the watchdog ends a game whose frame counter stood still this long (0 = its default, 30 s); the window-not-responding limit (20 s) is raised with it; e.g. 150 to let a cold-cache start-up stall or UE's own 120 s render-thread timeout play out
    [int]$CpuLimit = 0,                                  # lab only: act as an N-core CPU without SMT: the game runs in a Job object limited to N logical processors (one per physical core) from its start, and -corelimit=N -physicalcorelimit=N size UE's thread pools for N (0 = off)
    [string[]]$ExtraGameArgs = @(),                      # lab only: more game command-line arguments for this launch (e.g. '-corelimit=8', '-physicalcorelimit=4' to act as a smaller CPU)
    [switch]$SelfTest                                    # passive 15 s capture of the running game: checks PresentMon, sampler, analysis and report without sending it anything
)

$ErrorActionPreference = 'Stop'
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
. (Join-Path $PSScriptRoot 'BenchBridge.ps1')
if ($Record) { . (Join-Path $PSScriptRoot 'Obs.ps1') }

$root = Split-Path $PSScriptRoot
if ($Reuse) { $Session = 'reuse' }
if (-not $Scenario) { $Scenario = if ($Scenes -or $Tags -or $List) { 'full' } else { 'cheydinhal' } }
$scn = Get-BenchScenario $Scenario
$presentMon = Join-Path $root 'third_party\PresentMon-2.6.0-x64.exe'
$configDir = Join-Path $script:GameSaved 'Config\Windows'
$saveDir = Join-Path $script:GameSaved 'SaveGames'
$guardDir = Join-Path $script:BenchRun 'save-guard'
$fixIniPath = Join-Path $script:GameWin64 'OblivionPerfFix.ini'
$idlePidFile = Join-Path $script:BenchRun 'bench-idle.pid'
if (-not ($List -or $Close -or $Park -or $Stop) -and -not (Test-Path $presentMon)) { throw "PresentMon not found at $presentMon (run tools\Setup-Bench.ps1 first)" }
if (-not ($List -or $Close -or $Park -or $Stop)) {
    $access = Get-PresentMonAccess
    if (-not $access.Allowed) { throw $access.Message }
}

# Plain strings from a file another process holds open for writing (Get-Content would also do, but its
# strings carry provider properties that ConvertTo-Json serialises)
function Read-SharedLines([string]$path) {
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try { $text = (New-Object IO.StreamReader $fs).ReadToEnd() } finally { $fs.Dispose() }
    @($text -split "`r?`n" | Where-Object { $_ })
}

# $null, or the warning for start-up settings this run asked for that a reused game does not have: -FixIni keys whose
# value differs from the installed OblivionPerfFix.ini's [Fixes] (missing keys count as different), and any -IniCvars / -Ini
# (Engine.ini and the game's config are read at start-up only). Only meaningful when the running game is reused.
$script:ReuseIgnored = $null
function Get-ReuseIgnoredWarning {
    $parts = @()
    if ($FixIni.Count) {
        $have = @{}
        if (Test-Path -LiteralPath $fixIniPath) {
            $inFixes = $false
            foreach ($l in Read-SharedLines $fixIniPath) {
                if ($l -match '^\s*\[(.+?)\]') { $inFixes = ($Matches[1] -eq 'Fixes'); continue }
                if ($inFixes -and $l -match '^\s*(\w+)\s*=\s*([^;\s]*)') { $have[$Matches[1]] = $Matches[2] }
            }
        }
        $diff = @($FixIni.Keys | Sort-Object | Where-Object { "$($have[$_])" -ne "$($FixIni[$_])" } | ForEach-Object { "$_=$($FixIni[$_]) (installed: $(if ($have.ContainsKey($_)) { $have[$_] } else { 'absent' }))" })
        if ($diff.Count) { $parts += "-FixIni NOT applied: $($diff -join ', ')" }
    }
    if ($IniCvars.Count) { $parts += "-IniCvars NOT applied ($(ConvertTo-SortedText $IniCvars))" }
    if ($Ini.Count) { $parts += "-Ini NOT applied ($(($Ini.Keys | Sort-Object) -join ', '))" }
    if (-not $parts.Count) { return $null }
    "reused game (-Session $Session): start-up settings are applied only when the runner starts a game, so this run measured the installed ones: $($parts -join '; '). Write live keys into the installed ini (shared-open, read back) or use -Session fresh/auto."
}

function Step([string]$msg) {
    Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $msg)
    Set-BenchStatus @{ message = $msg }
}
function Opt($obj, [string]$name, $default) { if ($obj -and $obj.PSObject.Properties[$name]) { $obj.$name } else { $default } }
if (-not $Start) { $Start = Opt $scn 'start' 'fresh' }
if ($Watch) { $Visible = [switch]$true; $Audible = [switch]$true; if (-not $Profile) { $Profile = 'own' } }
if (-not $Profile) { $Profile = Opt $scn 'profile' 'small' }
# -Pin (2026-10-20, benchrepro): on by default (user, 2026-10-20); -NoPin or -Pin:$false turn it off
$pinOn = if ($NoPin) { $false } elseif ($PSBoundParameters.ContainsKey('Pin')) { [bool]$Pin } else { [bool](Opt $scn 'pin' $true) }
# UE's -FixedSeed on the game's command line for benchmark launches (FMath::RandInit / SRandInit from 0 instead of the clock)
$script:GameLaunchArgs = if ($NoFixedSeed) { @() } else { @('-FixedSeed') }
$script:GameLaunchArgs = @($script:GameLaunchArgs) + @($ExtraGameArgs | Where-Object { $_ })
if ($CpuLimit -gt 0) { $script:GameLaunchArgs = @($script:GameLaunchArgs) + @("-corelimit=$CpuLimit", "-physicalcorelimit=$CpuLimit") }
# a scenario that tests weather changes themselves says "pin": false; that wins over -Pin
if ($pinOn -and $scn.PSObject.Properties['pin'] -and -not [bool]$scn.pin) { $pinOn = $false; $script:pinRefused = $true }
function Rnd($v, [int]$digits = 1) { if ($null -eq $v -or [double]::IsNaN([double]$v) -or [double]::IsInfinity([double]$v)) { 0 } else { [math]::Round([double]$v, $digits) } }

# ---------------------------------------------------------------- profiles (bench\profiles.json)

$profilesPath = Join-Path $root 'bench\profiles.json'
$profiles = Get-Content $profilesPath -Raw | ConvertFrom-Json
# investigation-only profiles (e.g. one setting lowered to attribute GPU time) from a second file, kept out of the shared list
if ($env:OBLV_BENCH_PROFILES_EXTRA -and (Test-Path $env:OBLV_BENCH_PROFILES_EXTRA)) {
    foreach ($x in (Get-Content $env:OBLV_BENCH_PROFILES_EXTRA -Raw | ConvertFrom-Json).PSObject.Properties) {
        if ($x.Name -notlike '_*' -and -not $profiles.PSObject.Properties[$x.Name]) { $profiles | Add-Member -NotePropertyName $x.Name -NotePropertyValue $x.Value }
    }
}
function Get-ProfileDef([string]$name) {
    $p = $profiles.PSObject.Properties[$name]
    if (-not $p -or $name -like '_*') { throw "unknown profile '$name' (bench\profiles.json: $((@($profiles.PSObject.Properties.Name | Where-Object { $_ -notlike '_*' })) -join ', '))" }
    $p.Value
}
$prof = Get-ProfileDef $Profile
# A hidden game with frame generation on hangs on its way back to the main menu (known problem; 2026-10-20 npcslice:
# 4 of 4 returns after ic-waterfront-pass at -Profile user): for a fresh world it is restarted instead, and it is
# closed rather than parked at the end. -MenuReturn keeps the old way, -KeepWorld needs no menu at all.

$script:nativeRes = $null
# A profile with "own": true changes no display or graphics setting (the player's own, for -Watch and `user`).
function Test-OwnProfile($p) { [bool](Opt $p 'own' $false) }

# A profile that sets the options decides by its own "fg"; one with the player's settings ("own") by the game's live
# frame generation setting, read when the decision is made (no game or no answer: assume the risk, restart/close is safe).
function Test-FgMenuRisk {
    if ($MenuReturn) { return $false }
    if (-not (Test-OwnProfile $prof)) { return (Opt $prof 'upscaler' 'fsr') -eq 'fsr' -and [int](Opt $prof 'fg' 0) -eq 1 }
    if (-not (Get-Game)) { return $true }
    $v = (Send-Bench 'get r.FidelityFX.FI.Enabled' -NoThrow) -replace '^.*=', ''
    $d = 0.0
    if (-not [double]::TryParse([string]$v, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$d)) { return $true }
    $d -ge 1
}

function Get-ProfileRes($p) {
    if (Test-OwnProfile $p) { return 'own' }
    $r = Opt $p 'res' '1280x720'
    if ($r -ne 'native') { return $r }
    if (-not $script:nativeRes) {
        $v = Get-CimInstance Win32_VideoController | Where-Object { $_.CurrentHorizontalResolution } | Sort-Object CurrentHorizontalResolution -Descending | Select-Object -First 1
        $script:nativeRes = if ($v) { "$($v.CurrentHorizontalResolution)x$($v.CurrentVerticalResolution)" } else { '1920x1080' }
    }
    $script:nativeRes
}

$qualityOptions = 'ViewDistanceQuality', 'ShadowQuality', 'GlobalIlluminationQuality', 'ReflectionQuality', 'PostProcessQuality', 'TextureQuality', 'EffectsQuality', 'FoliageQuality', 'ShadingQuality', 'ClothQuality'

# One quality option's level: the profile's "qualityOptions" (e.g. { "ShadowQuality": 2 }), else its "quality" (default 3)
function Get-QualityLevel($p, [string]$name) { [int](Opt (Opt $p 'qualityOptions' $null) $name (Opt $p 'quality' 3)) }

# FSR 3 quality mode -> render scale (percent of the output resolution per axis)
$fsrScale = @{ 0 = 100.0; 1 = 66.6667; 2 = 58.8235; 3 = 50.0; 4 = 33.3333 }

# Console commands for one part of a profile ('res', 'rt', 'upscaler', 'cvars', or 'all').
# The upscaler only takes a new quality (or comes back after the game switched FSR off when its window
# lost the focus) when the method is applied again, so it is set to another method and back.
function Get-ProfileCommands($p, [string]$part = 'all') {
    $c = New-Object System.Collections.Generic.List[string]
    if (Test-OwnProfile $p) { return , $c }
    if ($part -in 'all', 'res') {
        $mode = @{ windowed = 'w'; borderless = 'wf' }[[string](Opt $p 'mode' 'windowed')]
        if (-not $mode) { throw "profile mode must be windowed or borderless, not '$(Opt $p 'mode' '')'" }
        $c.Add("exec r.SetRes $(Get-ProfileRes $p)$mode")
    }
    if ($part -eq 'all') { foreach ($x in 'cvar Altar.GraphicsOptions.VSync 0', 'cvar Altar.GraphicsOptions.FrameRateLimit 0', 'cvar t.MaxFPS 0') { $c.Add($x) } }
    if ($part -in 'all', 'quality') {
        # the game's own quality options (3 = its highest; what the player uses): set, not inherited from the player's save
        foreach ($n in $qualityOptions) { $c.Add("cvar Altar.GraphicsOptions.$n $(Get-QualityLevel $p $n)") }
    }
    if ($part -in 'all', 'rt') {
        $c.Add("cvar Altar.GraphicsOptions.EnableHardwareRaytracing $([int](Opt $p 'rt' 1))")
        if ([int](Opt $p 'rt' 1)) { $c.Add('cvar Altar.GraphicsOptions.HardwareRaytracingMode 3') }
    }
    if ($part -in 'all', 'upscaler') {
        if ((Opt $p 'upscaler' 'fsr') -eq 'fsr') {
            $c.Add("cvar Altar.FSR3.Quality $([int](Opt $p 'fsrQuality' 3))")
            $c.Add("cvar Altar.FSR3.FI.Enabled $([int](Opt $p 'fg' 0))")
            $c.Add('cvar Altar.UpscalingMethod 0'); $c.Add('cvar Altar.UpscalingMethod 2')
            $c.Add('cvar r.FidelityFX.FSR3.Enabled 1')
        } else {
            $scale = [double](Opt $p 'scale' 100)
            $c.Add('cvar Altar.FSR3.FI.Enabled 0')
            $c.Add("cvar Altar.GraphicsOptions.ScreenPercentage $scale")
            $c.Add('cvar Altar.UpscalingMethod 2'); $c.Add('cvar Altar.UpscalingMethod 0')
            $c.Add('cvar r.FidelityFX.FSR3.Enabled 0')
            $c.Add("cvar r.ScreenPercentage $scale")
        }
    }
    if ($part -in 'all', 'cvars') { foreach ($q in @((Opt $p 'cvars' ([pscustomobject]@{})).PSObject.Properties)) { $c.Add("cvar $($q.Name) $($q.Value)") } }
    $c
}

# What the profile should read back as: cvar -> value, plus "window" -> WxH (client area)
function Get-ProfileExpect($p) {
    $e = [ordered]@{}
    if (Test-OwnProfile $p) { return $e }
    $fsr = (Opt $p 'upscaler' 'fsr') -eq 'fsr'
    $e['r.FidelityFX.FSR3.Enabled'] = [int]$fsr
    $e['r.ScreenPercentage'] = if ($fsr) { $fsrScale[[int](Opt $p 'fsrQuality' 3)] } else { [double](Opt $p 'scale' 100) }
    $e['r.FidelityFX.FI.Enabled'] = if ($fsr) { [int](Opt $p 'fg' 0) } else { 0 }
    $e['r.Lumen.HardwareRayTracing'] = [int](Opt $p 'rt' 1)
    if ([int](Opt $p 'rt' 1)) { $e['r.Lumen.HardwareRayTracing.LightingMode'] = 3 }
    foreach ($n in 'ViewDistanceQuality', 'ShadowQuality', 'GlobalIlluminationQuality', 'ReflectionQuality', 'PostProcessQuality', 'TextureQuality', 'EffectsQuality', 'FoliageQuality', 'ShadingQuality') { $e["sg.$n"] = Get-QualityLevel $p $n }
    $e['window'] = Get-ProfileRes $p
    $e
}

function Invoke-ProfileCommands($cmds) {
    # The ray-tracing switches rebuild render state; the first time a PC enables hardware ray tracing that took 11 s
    # (2026-10-19, RX 9070, fresh install), past the 8 s answer limit, and the run ended at the main menu.
    foreach ($c in $cmds) { Send-Bench $c -TimeoutMs $(if ($c -match 'Raytracing') { 60000 } else { 8000 }) | Out-Null }
}

# Differences between what the profile wants and what the game has: @{ part = 'res'|'rt'|'upscaler'; text }
function Get-ProfileDrift($p, $hwnd) {
    $out = @()
    foreach ($kv in (Get-ProfileExpect $p).GetEnumerator()) {
        if ($kv.Key -eq 'window') {
            $size = [OblvBench.ImageTools]::ClientSize($hwnd); $now = "$($size[0])x$($size[1])"
            if ($now -ne $kv.Value) { $out += @{ part = 'res'; text = "window $now, profile $($kv.Value)" } }
            continue
        }
        $v = (Send-Bench "get $($kv.Key)" -NoThrow) -replace '^.*=', ''
        if (-not $v -or $v -eq 'nil') { continue }               # no answer (game busy or down): not a drift
        $num = 0.0
        if (-not [double]::TryParse($v, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$num) -or [math]::Abs($num - [double]$kv.Value) -gt 0.5) {
            $part = if ($kv.Key -like 'r.Lumen.*') { 'rt' } elseif ($kv.Key -like 'sg.*') { 'quality' } else { 'upscaler' }
            $out += @{ part = $part; text = "$($kv.Key)=$v, profile $($kv.Value)" }
        }
    }
    $out
}

# ---------------------------------------------------------------- save guard
# Door transitions autosave (the player's own setting, kept so the run matches real play).
# Snapshot the rolling saves before the run and put them back afterwards.

$guardPattern = '^(autosave|quicksave|saves_meta|Save_Settings)'
$guardPending = Join-Path $guardDir 'pending.json'

function Restore-Saves {
    if (-not (Test-Path $guardPending)) { return }
    $state = Get-Content $guardPending -Raw | ConvertFrom-Json
    # A file written well after the benchmark last used the game comes from the player's own play: leave it alone
    $limit = [long]$state.lastActivity + [TimeSpan]::FromMinutes(2).Ticks
    $before = @{}
    foreach ($f in @($state.files)) { $before[$f.Name] = $f }
    $created = @(Get-ChildItem $saveDir -File | Where-Object { -not $before.ContainsKey($_.Name) -and $_.LastWriteTimeUtc.Ticks -le $limit })
    if ($created.Count) {
        $dest = Join-Path $guardDir ('created-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory -Force $dest | Out-Null
        $created | ForEach-Object { Move-Item -LiteralPath $_.FullName -Destination $dest }
        Step "save guard: moved $($created.Count) save(s) created during the run to $dest"
    }
    $restored = 0; $kept = 0
    foreach ($g in Get-ChildItem (Join-Path $guardDir 'files') -File -ErrorAction SilentlyContinue) {
        $cur = Join-Path $saveDir $g.Name
        $b = $before[$g.Name]
        if (Test-Path -LiteralPath $cur) {
            $item = Get-Item -LiteralPath $cur
            if ($item.Length -eq $b.Length -and $item.LastWriteTimeUtc.Ticks -eq $b.Ticks) { continue }
            if ($item.LastWriteTimeUtc.Ticks -gt $limit) { $kept++; continue }
        }
        Copy-Item -LiteralPath $g.FullName -Destination $cur -Force; $restored++
    }
    if ($restored) { Step "save guard: restored $restored rolling save file(s)" }
    if ($kept) { Step "save guard: left $kept file(s) alone that were written after the benchmark (your own play)" }
    if ($created.Count -or $restored) { Set-Content (Join-Path $guardDir 'last-restore.txt') ([DateTime]::UtcNow.Ticks) -Encoding ascii }   # Start-BenchGame waits a little after it
    Remove-Item -LiteralPath $guardPending
}

function Protect-Saves {
    if (Test-Path $guardPending) {
        if (Get-Game) { return }                         # game still open from an earlier run: the snapshot is already in place
        Restore-Saves                                    # left over from an aborted run
    }
    $files = Join-Path $guardDir 'files'
    if (Test-Path $files) { Get-ChildItem $files -File | ForEach-Object { Remove-Item -LiteralPath $_.FullName } }
    New-Item -ItemType Directory -Force $files | Out-Null
    $list = @(Get-ChildItem $saveDir -File | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Length = $_.Length; Ticks = $_.LastWriteTimeUtc.Ticks } })
    Get-ChildItem $saveDir -File | Where-Object { $_.Name -match $guardPattern } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $files }
    [pscustomobject]@{ lastActivity = [DateTime]::UtcNow.Ticks; files = $list } | ConvertTo-Json -Depth 3 | Set-Content $guardPending -Encoding utf8
}

<# The benchmark is about to close its game: the settings and save index the game writes as it quits are the
   benchmark's, not the player's (a parked game may be closed long after the last run). #>
function Stop-BenchGame { Update-SaveGuard; Stop-Game }

function Update-SaveGuard {
    if (-not (Test-Path $guardPending)) { return }
    $state = Get-Content $guardPending -Raw | ConvertFrom-Json
    $state.lastActivity = [DateTime]::UtcNow.Ticks
    $state | ConvertTo-Json -Depth 3 | Set-Content $guardPending -Encoding utf8
}

# ---------------------------------------------------------------- config files

function Set-IniValues([string]$path, [string]$section, [hashtable]$values) {
    $lines = New-Object System.Collections.Generic.List[string]
    if (Test-Path $path) { Get-Content $path | ForEach-Object { $lines.Add($_) } }
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -eq "[$section]") { $start = $i; break } }
    if ($start -lt 0) { $lines.Add(''); $lines.Add("[$section]"); $start = $lines.Count - 1 }
    $end = $lines.Count
    for ($i = $start + 1; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*\[') { $end = $i; break } }
    foreach ($k in $values.Keys) {
        $found = $false
        for ($i = $start + 1; $i -lt $end; $i++) {
            if ($lines[$i] -match ('^\s*' + [regex]::Escape($k) + '\s*=')) { $lines[$i] = "$k=$($values[$k])"; $found = $true }
        }
        if (-not $found) { $lines.Insert($start + 1, "$k=$($values[$k])"); $end++ }
    }
    Set-Content $path $lines -Encoding ascii
}

# Ini changes need the game closed. The originals go to the run folder and are put back once the game has closed.
$configPending = Join-Path $script:BenchRun 'bench-config-restore.json'

function Get-PendingConfig {
    if (-not (Test-Path $configPending)) { return @() }
    $entries = Get-Content $configPending -Raw | ConvertFrom-Json        # a JSON array arrives as one object: assign, then enumerate
    @($entries | ForEach-Object { $_ })
}

function Restore-Config {
    if (-not (Test-Path $configPending)) { return }
    foreach ($e in Get-PendingConfig) {
        if ($e.backup -and (Test-Path -LiteralPath $e.backup)) { Copy-Item -LiteralPath $e.backup -Destination $e.target -Force }
    }
    Remove-Item -LiteralPath $configPending
    Step 'config files restored'
}

function Set-RunConfig([string]$backupDir) {
    New-Item -ItemType Directory -Force $backupDir | Out-Null
    $list = @()
    $engineIni = Join-Path $configDir 'Engine.ini'
    # The game opens its window at the profile's size, windowed (or borderless for a borderless profile), never
    # at the player's: less GPU load (and fan noise) from the first frame, and nothing to resize later. UE reads
    # the size from GameUserSettings.ini at start-up, then the game applies its own options (Save_Settings.sav:
    # r.SetRes, window mode). The game writes both inis back on exit: they are restored afterwards, the save by
    # the save guard.
    $res = Get-ProfileRes $prof
    if (Test-OwnProfile $prof) { $res = $null }      # profile "own": the game opens as the player left it
    $w, $h = "$res" -split 'x'
    $mode = if ((Opt $prof 'mode' 'windowed') -eq 'borderless') { 1 } else { 2 }
    $window = [ordered]@{ ResolutionSizeX = $w; ResolutionSizeY = $h; LastUserConfirmedResolutionSizeX = $w; LastUserConfirmedResolutionSizeY = $h
                          DesiredScreenWidth = $w; DesiredScreenHeight = $h; LastUserConfirmedDesiredScreenWidth = $w; LastUserConfirmedDesiredScreenHeight = $h
                          FullscreenMode = $mode; LastConfirmedFullscreenMode = $mode }
    foreach ($name in 'GameUserSettings.ini', 'AltarGameUserSetting.ini') {
        $f = Join-Path $configDir $name
        if (-not (Test-Path $f)) { continue }
        # backed up in every profile ("own" too): the game rewrites both on exit (e.g. the frame cap the run lifts)
        Copy-Item -LiteralPath $f -Destination $backupDir -Force
        $list += [pscustomobject]@{ backup = (Join-Path $backupDir $name); target = $f }
        if ($res) { Set-IniValues $f '/Script/Altar.AltarGameUserSetting' ([hashtable]$window) }
    }
    $settingsSav = Join-Path $saveDir 'Save_Settings.sav'
    if ($res -and (Test-Path $settingsSav)) {
        try {
            foreach ($c in [OblvBench.SettingsSave]::Set($settingsSav, @{ 'r.SetRes' = $res; 'Altar.GraphicsOptions.WindowMode' = "$mode" })) { Step "start-up window: $c" }
        } catch { Step "start-up window: Save_Settings.sav not changed ($_): the game opens at the player's size first" }
    }
    if ($IniCvars.Count) {
        Copy-Item -LiteralPath $engineIni -Destination $backupDir -Force
        $list += [pscustomobject]@{ backup = (Join-Path $backupDir 'Engine.ini'); target = $engineIni }
        Set-IniValues $engineIni 'SystemSettings' $IniCvars
    }
    if ($FixIni.Count) {
        if (-not (Test-Path $fixIniPath)) { throw 'OblivionPerfFix is not installed (tools\Install-PerfFix.ps1), so -FixIni has nothing to configure.' }
        Copy-Item -LiteralPath $fixIniPath -Destination $backupDir -Force
        $list += [pscustomobject]@{ backup = (Join-Path $backupDir 'OblivionPerfFix.ini'); target = $fixIniPath }
        Set-IniValues $fixIniPath 'Fixes' $FixIni
    }
    foreach ($target in $Ini.Keys) {
        $file, $section = $target -split '\|', 2
        $path = Join-Path $configDir $file
        $copy = Join-Path $backupDir $file
        if (-not ($list | Where-Object { $_.target -eq $path })) {
            if (Test-Path $path) { Copy-Item -LiteralPath $path -Destination $copy -Force } else { Set-Content $copy '' -NoNewline -Encoding ascii }
            $list += [pscustomobject]@{ backup = $copy; target = $path }
        }
        Set-IniValues $path $section $Ini[$target]
    }
    if ($list.Count) { ConvertTo-Json @($list) | Set-Content $configPending -Encoding utf8 }
}

# ---------------------------------------------------------------- the game session (process)
# What the runner knows about the running game process, kept across runs in bench-session.json (runtime folder):
# which console toggles it has switched (they have no "set" form), and the start-up fingerprint it was
# launched with, so a later run can tell whether this process may be reused.
$sessionFile = Join-Path $script:BenchRun 'bench-session.json'

function Get-SessionState($game) {
    $key = "$($game.Id)@$($game.StartTime.Ticks)"
    if (Test-Path $sessionFile) {
        $st = Get-Content $sessionFile -Raw | ConvertFrom-Json
        if ($st.key -eq $key) {
            $st.spawned = @($st.spawned)
            foreach ($k in 'fingerprint', 'launchedAt', 'runs', 'parked', 'lastActivity', 'capBeforePark', 'profileApplied') { if (-not $st.PSObject.Properties[$k]) { $st | Add-Member NoteProperty $k $null } }
            return $st
        }
    }
    [pscustomobject]@{ key = $key; initialized = $false; detectOff = $false; aiOff = $false; spawned = @()
                       fingerprint = $null; launchedAt = $null; runs = 0; parked = $false; lastActivity = $null; capBeforePark = $null
                       profileApplied = $null }
}
function Save-SessionState($state) { $state | ConvertTo-Json -Depth 4 | Set-Content $sessionFile -Encoding utf8 }

function Get-FileStamp([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return 'none' }
    $i = Get-Item -LiteralPath $path; "$($i.Length)@$($i.LastWriteTimeUtc.Ticks)"
}
function Get-TextHash([string]$text) {
    $sha = [Security.Cryptography.SHA1]::Create()
    -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)) | Select-Object -First 8 | ForEach-Object { $_.ToString('x2') })
}
# The config file as the player has it: the backup while a run's changes are in place, else the file itself
function Get-BaseFileHash([string]$target) {
    $e = Get-PendingConfig | Where-Object { $_.target -eq $target } | Select-Object -First 1
    $path = if ($e -and (Test-Path -LiteralPath $e.backup)) { $e.backup } elseif (Test-Path -LiteralPath $target) { $target } else { $null }
    if (-not $path) { return 'none' }
    Get-TextHash ([IO.File]::ReadAllText($path))
}
function ConvertTo-SortedText($h) { (@($h.Keys | Sort-Object | ForEach-Object { if ($h[$_] -is [hashtable]) { "$_={$(ConvertTo-SortedText $h[$_])}" } else { "$_=$($h[$_])" } })) -join ';' }

<# Everything a game process can only take in at start-up. A running game is reused only if it matches. #>
function Get-LaunchFingerprint {
    [ordered]@{
        iniCvars = ConvertTo-SortedText $IniCvars
        fixIni = ConvertTo-SortedText $FixIni
        ini = ConvertTo-SortedText $Ini
        fixDll = Get-FileStamp (Join-Path $script:GameWin64 'version.dll')
        fixIniFile = Get-BaseFileHash $fixIniPath
        engineIni = Get-BaseFileHash (Join-Path $configDir 'Engine.ini')
        # how the window was opened: hidden and muted (default) or for watching (-Visible/-Watch), at the profile's
        # size or the player's own ("own"); a hidden 480p game cannot be watched, nor the reverse be unnoticeable
        window = "visible=$([bool]$Visible) capturable=$([bool]$Record) size=$(if (Test-OwnProfile $prof) { 'own' } else { "$(Get-ProfileRes $prof) $(Opt $prof 'mode' 'windowed')" })"
        modBootstrap = Get-TextHash ([IO.File]::ReadAllText((Join-Path $root 'mods\ue4ss\OblvBench\Scripts\main.lua')))
        # the hook DLL goes into the game at its start and stays (window hiding, crash UI, spectator mode's NPC conversation guard)
        benchHook = Get-TextHash ([IO.File]::ReadAllText((Join-Path $root 'tools\benchhook\BenchHook.cpp')))
        # the UE4SS build Start-Game would enable (a running game has the one from its launch in the game folder)
        ue4ss = Get-FileStamp (Join-Path $(if ($env:OBLIVION_UE4SS_DIR) { $env:OBLIVION_UE4SS_DIR } else { Join-Path $root 'third_party\ue4ss' }) 'ue4ss\UE4SS.dll')
        # the game's own command-line arguments (-FixedSeed): only a new launch changes them
        launchArgs = ($script:GameLaunchArgs -join ' ')
    }
}

<# $null if the running game can be used for this run, else why not. Throws if it is not the benchmark's
   own game (the player's?) and -Force was not given. #>
function Get-RestartReason($game) {
    if (-not $game) { return 'no game running' }
    $state = Get-SessionState $game
    $ping = Send-Bench 'ping' -TimeoutMs 5000 -NoThrow
    if ($Session -eq 'reuse') {
        if ($null -eq $ping) { return 'the running game does not answer (OblvBench mod not loaded?)' }
        return $null
    }
    if (-not $state.fingerprint) {
        if (-not $Force) { throw "A game is running that the benchmark did not start (the player's own session?). Close it, or use -Session reuse to measure in it, or -Force to let the benchmark restart it." }
        return 'the running game was not started by the benchmark'
    }
    if ($Session -eq 'fresh') { return 'a fresh session was asked for (-Session fresh)' }
    if ($null -eq $ping) { return 'the running game does not answer' }
    if ($ping -notmatch 'driver=frame') { return "the mod's frame loop is not running ($ping)" }
    $want = Get-LaunchFingerprint
    $had = $state.fingerprint
    $diff = @($want.Keys | Where-Object { "$($want[$_])" -ne "$($had.$_)" })
    if ($diff.Count) { return "start-up settings changed since it started ($($diff -join ', '))" }
    if ($state.launchedAt) {
        $age = ((Get-Date) - [datetime]$state.launchedAt).TotalMinutes
        if ($age -gt $MaxSessionMin) { return ("the game has been running for {0:N0} min (-MaxSessionMin {1}: UE4SS memory grows in long sessions)" -f $age, $MaxSessionMin) }
    }
    $hb = [OblvBench.GameWatch]::HeartbeatAge
    if ($hb -gt 10) { return ("no game frame for {0:N0} s" -f $hb) }
    if ((Test-FgMenuRisk) -and -not $KeepWorld -and (Get-GamePlace) -eq 'world') { return 'a fresh world at a frame-generation profile: the way back to the main menu can hang there (known problem), so the game is restarted (-KeepWorld keeps the world, -MenuReturn goes back to the menu anyway)' }
    $null
}

# Interiors and the main menu share the level L_PersistentDungeon; only an interior has a sublevel.
$inWorld = { param($s) $s.pawn -eq '1' -and $s.frozen -eq '0' -and $s.loadscreen -eq '0' -and $s.travel -ne '1' -and $s.level -ne 'L_SeamlessTransition' -and
             ($s.level -ne 'L_PersistentDungeon' -or $s.sub) }
$atMenu = { param($s) $s.level -eq 'L_PersistentDungeon' -and -not $s.sub -and $s.frozen -eq '1' -and $s.travel -ne '1' }

<# 'world', 'menu' (main menu, buttons up), or 'busy' (loading, a menu, the start-up videos...). #>
function Get-GamePlace {
    $s = Get-BenchState 5000
    if (-not $s) { return 'busy' }
    if (& $inWorld $s) { return 'world' }
    if ((& $atMenu $s) -and (Send-Bench 'menu' -NoThrow) -eq 'ready') { return 'menu' }
    'busy'
}

<# Steps the start-up screens (logo videos, title screen) until the main menu's buttons are up. #>
function Wait-MainMenu([double]$timeoutSec = 180) {
    $sw = [Diagnostics.Stopwatch]::StartNew(); $menu = ''
    while ($menu -ne 'ready') {
        if ($sw.Elapsed.TotalSeconds -gt $timeoutSec) { throw "GAMEDOWN: stuck: the main menu did not come up within $timeoutSec s (last answer: $menu)" }
        $menu = Send-Bench 'menu' -TimeoutMs 5000 -NoThrow
        if ($menu -in 'eula=0', 'setup=0') {
            throw 'NEEDSUSER: The game is waiting for its first-start steps (licence agreement, display/sound settings). Start it once by hand, go through them to the main menu, close it, then run the benchmark again.'
        }
        if ($menu -ne 'ready') { Wait-Game 0.4 }
    }
    $s = Wait-BenchState $atMenu -TimeoutSec 60 -Stable 2 -PollMs 300
    if (-not $s) { throw 'GAMEDOWN: stuck: the main menu reports ready but the game is not at it' }
}

<# Starts the game (closing one that runs), with this run's start-up config, up to the main menu. #>
function Start-BenchGame([string]$why) {
    Step "starting the game: $why"
    Set-BenchStatus @{ game = 'starting' }
    if (Get-Game) { Stop-BenchGame }
    Restore-Config                                       # left by an earlier session with other start-up settings
    Set-RunConfig $script:configBackup
    # Steam's cloud check at launch once failed 11 s after the save guard had put saves back in the (OneDrive) save
    # folder (2026-10-15 09:14: "cloud sync failed" dialog, every launch ignored until Steam restarted): give the save
    # folder 20 s to settle after a restore.
    $restoreMark = Join-Path $guardDir 'last-restore.txt'
    if (Test-Path $restoreMark) {
        $left = 20 - ([DateTime]::UtcNow - [datetime]::new([long](Get-Content $restoreMark -Raw).Trim(), [DateTimeKind]::Utc)).TotalSeconds
        if ($left -gt 0) { Step "waiting $([int][Math]::Ceiling($left)) s for the save folder to settle after the save guard's restore"; Start-Sleep -Seconds ([int][Math]::Ceiling($left)) }
    }
    $game = $null
    for ($try = 1; -not $game; $try++) {
        $cpuMask = [uint64]0
        if ($StallSec -gt 0) { [OblvBench.GameWatch]::StallSec = $StallSec; [OblvBench.GameWatch]::HungSec = [math]::Max(20, $StallSec) }
        if ($CpuLimit -gt 0) {
            if (-not ('OblvBench.CpuLimitJob' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'CpuLimitJob.cs') }
            $cpuMask = [OblvBench.CpuLimitJob]::MaskForCores($CpuLimit)
            if (-not $cpuMask) { throw "-CpuLimit $CpuLimit is more than this PC's physical cores" }
            # below the minimum spec (6 cores) the game shows "Thank you for installing ..." at start-up: answered with OK, as a player would
            if ($CpuLimit -lt 6) { [OblvBench.GameWatch]::AcceptBoxPrefix = 'Thank you for installing'; [OblvBench.GameWatch]::AcceptedBoxes = 0; [OblvBench.GameWatch]::AcceptLog = '' }
        }
        try { $game = Start-Game -Unobtrusive:(-not $Visible) -Audible:$Audible -GameArgs $script:GameLaunchArgs -CpuMask $cpuMask }
        catch {
            if ($CpuLimit -gt 0 -and $CpuLimit -lt 6) { Step "minimum-spec box: answered $([OblvBench.GameWatch]::AcceptedBoxes) time(s) $([OblvBench.GameWatch]::AcceptLog)" }
            if ($try -ge 2 -or "$_" -match '^(NEEDSUSER|DEADLINE)') { throw }
            Step "start failed ($_), trying once more"
            [OblvBench.GameWatch]::Dismiss() | ForEach-Object { Step "dismissed: $_" }
            if (Get-Game) { Stop-Process -Id (Get-Game).Id -Force -ErrorAction SilentlyContinue }
            Start-Sleep -Seconds 10
        }
    }
    $script:launchMs = Get-QpcMs
    if ($CpuLimit -gt 0 -and $CpuLimit -lt 6) { Step "minimum-spec box: answered $([OblvBench.GameWatch]::AcceptedBoxes) time(s) $([OblvBench.GameWatch]::AcceptLog)" }
    # did the arguments reach the game? (Steam adds the player's own launch options; the shipping exe gets them from the stub)
    $script:gameCmdLine = try { "$((Get-CimInstance Win32_Process -Filter "ProcessId=$($game.Id)").CommandLine)" } catch { '' }
    foreach ($a in $script:GameLaunchArgs) {
        if ($script:gameCmdLine -notmatch "(^|\s)$([regex]::Escape($a))(\s|$)") { Write-Warning "$a did not reach the game (its command line: $($script:gameCmdLine))"; $script:launchArgMissing = $a }
    }
    if ($script:GameLaunchArgs.Count) { Step "game command line: $(($script:gameCmdLine -replace '^\s*("[^"]*"|\S+)\s*', '').Trim())" }
    $state = Get-SessionState $game
    $state.fingerprint = Get-LaunchFingerprint; $state.launchedAt = (Get-Date).ToString('s'); $state.runs = 0
    Save-SessionState $state
    Step 'waiting for the main menu (skipping the start-up videos and the title screen)'
    Wait-MainMenu
    if ($Visible) { Step (Set-GameForeground) }          # a game started by Steam may come up behind the console
    $game
}

<# From the main menu into the world: a new game with the default character (-Start fresh), or a save. #>
function Enter-World {
    # -Watch: black over the view from here (start cell, HUD before spectator mode, teleports) until a scene shows
    # something; the mod keeps it through level changes. A flight with "fadein" lifts it when its timed part starts.
    if ($Watch -and $null -ne (Send-Bench 'fade 1' -NoThrow)) { $script:faded = $true }
    if ($Start -in 'continue', 'quicksave') {
        $s = Get-BenchState
        if ($s.cont -ne '1') { throw 'no saved game to continue: use -Start fresh' }
        Step 'main menu: Continue (latest save)'
        Send-Bench 'continue' -TimeoutMs 60000 | Out-Null
        $sw = [Diagnostics.Stopwatch]::StartNew(); $msg = ''
        while ($sw.Elapsed.TotalSeconds -lt 20 -and $msg -notmatch 'Continue') { Wait-Game 0.3; $msg = Send-Bench 'msg' -TimeoutMs 3000 -NoThrow }
        if ($msg -notmatch 'Continue') { throw "expected the 'Continue from your last saved game?' box, got: $msg" }
        Send-Bench 'msg 0' -TimeoutMs 60000 | Out-Null
        Wait-Game 2
        $s = Wait-BenchState $inWorld -TimeoutSec 300 -Stable 6 -PollMs 500
        if (-not $s) { throw 'the save did not load' }
        # Continue loads the newest save. If that is not quicksave.sav (an autosave or a save made
        # later), load the quicksave from in the world. The scenario's "save" says where it is.
        $qs = Opt $scn 'save' $null
        if ($Start -eq 'quicksave' -and -not ($qs -and $s.level -eq "L_$($qs.world)" -and
                (Get-Dist $s ([pscustomobject]@{ x = $qs.pos[0]; y = $qs.pos[1]; z = $qs.pos[2] })) -lt 1500)) {
            Step "loaded save is elsewhere ($($s.level) $($s.x),$($s.y)): loading quicksave.sav"
            Send-Bench 'quickload' | Out-Null
            Wait-Game 4
            $s = Wait-BenchState $inWorld -TimeoutSec 300 -Stable 6 -PollMs 500
            if (-not $s) { throw 'the quicksave did not load' }
            if ($qs -and (Get-Dist $s ([pscustomobject]@{ x = $qs.pos[0]; y = $qs.pos[1]; z = $qs.pos[2] })) -gt 1500) {
                throw "quicksave.sav is not the save this scenario was made for (player at $($s.x),$($s.y), expected $($qs.pos -join ','))"
            }
        }
    } else {
        # No save needed: the old engine's `coc` from the main menu starts a game with its default
        # character in that cell; closing the menu hands over to it. No intro, no character creation.
        $cell = Opt $scn 'startCell' 'CheydinhalChapelOfArkay'
        Step "main menu: new game in $cell (no save used)"
        Send-Bench "obv coc $cell" | Out-Null
        $s = Wait-BenchState { param($s) $s.sub -eq "L_$cell" -and $s.subloaded -eq '1' } -TimeoutSec 300 -Stable 4 -PollMs 500
        if (-not $s) { throw "the start cell $cell did not load" }
        Send-Bench 'closemenu' | Out-Null
        # the loading screen object stays until the first teleport, so loadscreen is not checked here
        $s = Wait-BenchState { param($s) $s.pawn -eq '1' -and $s.frozen -eq '0' -and $s.travel -ne '1' } -TimeoutSec 60 -Stable 4 -PollMs 500
        if (-not $s) { throw 'the game did not leave the main menu' }
    }
    $s
}

<# From the world back to the main menu (the pause menu's "Main Menu"). #>
function Enter-MainMenu {
    # spectator mode is switched off only at the menu: before, it would show the gameplay HUD over a faded-out view
    Send-Bench 'mainmenu' | Out-Null
    $s = Wait-BenchState $atMenu -TimeoutSec 90 -Stable 3 -PollMs 500
    if (-not $s) { throw 'GAMEDOWN: stuck: the game did not reach the main menu' }
    Send-Bench 'spectate off' -NoThrow | Out-Null
    Set-BenchObserver $false | Out-Null
    Wait-MainMenu 60
}

<# Leaves the game at the main menu, cheap to keep open (frame rate capped), for the next run. #>
function Set-GameParked($game) {
    Step 'parking the game at the main menu for the next run'
    if ((Get-GamePlace) -ne 'menu') { Enter-MainMenu }
    Send-Bench 'fade off' -NoThrow | Out-Null                      # -Watch or a flight left the view black
    # the cap in effect before parking, put back by profile "own" (the player's cap) at the next run
    $capBefore = (Send-Bench 'get t.MaxFPS' -NoThrow) -replace '^.*=', ''
    Send-Bench 'cvar t.MaxFPS 30' -NoThrow | Out-Null              # the menu would otherwise render at hundreds of fps; every profile resets it
    $state = Get-SessionState $game
    if ($null -eq $state.capBeforePark -and $capBefore -match '^[\d.]+$') { $state.capBeforePark = $capBefore }
    $state.parked = $true; $state.lastActivity = (Get-Date).ToString('s')
    Save-SessionState $state
}

function Stop-IdleWatcher {
    if (-not (Test-Path $idlePidFile)) { return }
    $idPid = [int](Get-Content $idlePidFile -Raw)
    $p = Get-CimInstance Win32_Process -Filter "ProcessId=$idPid" -ErrorAction SilentlyContinue
    if ($p -and $p.CommandLine -match 'Bench-Idle') { Stop-Process -Id $idPid -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $idlePidFile -ErrorAction SilentlyContinue
}

<# A small hidden process that restores saves and config when the parked game closes, and closes it
   after -IdleCloseMin without a run (it steps aside as soon as a run starts or the player takes over). #>
function Start-IdleWatcher {
    Stop-IdleWatcher
    $p = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        "`"$(Join-Path $PSScriptRoot 'Bench-Idle.ps1')`"", '-IdleMin', $IdleCloseMin)
    Set-Content $idlePidFile $p.Id -Encoding ascii
}

<# After a crash, hang or driver timeout: keep the evidence, close what is left on screen, end the game. #>
function Invoke-Recovery([string]$runDir, [int]$n, [string]$problem, [datetime]$gameStart) {
    $dir = Join-Path $runDir "crash-$n"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $notes = New-Object System.Collections.Generic.List[string]
    $notes.Add("problem: $problem")
    foreach ($l in [OblvBench.GameWatch]::TakeLog()) { $notes.Add("watch: $l") }
    $crashMsg = $null
    # Lab FullCrashDump: the crashed game waits in UE's crash handling while OblivionPerfFix-dumpwriter.exe writes its
    # full memory dump (UE's minidump file stays open until then): let it finish before copying and ending the game.
    # Since 2026-10-20 (npcpromo) also before the writer exists: the memory snapshot before it can take ~20 s at 4K, and
    # a run that checked only for the writer copied too early and failed, after which the next run ended the game under it.
    $w = Wait-LabCrashDump
    if ($w) { $notes.Add($w); Step $w }
    # UE's crash report folder of this game process (crash context, minidump, breadcrumbs: about 1 MB)
    $crashes = Join-Path $script:GameSaved 'Crashes'
    $wait = [Diagnostics.Stopwatch]::StartNew()
    do {
        $cr = Get-ChildItem $crashes -Directory -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $gameStart } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $cr -and $problem -match 'crash') { Start-Sleep -Milliseconds 500 }
    } while (-not $cr -and $problem -match 'crash' -and $wait.Elapsed.TotalSeconds -lt 10)
    if ($cr) {
        # file by file with shared reads: a file the game still holds open must not fail the run (2026-10-20)
        $crDir = Join-Path $dir $cr.Name
        New-Item -ItemType Directory -Force $crDir | Out-Null
        foreach ($f in Get-ChildItem -LiteralPath $cr.FullName -File -Recurse -ErrorAction SilentlyContinue) {
            $to = Join-Path $crDir $f.FullName.Substring($cr.FullName.Length).TrimStart('\')
            New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
            if (-not (Copy-SharedFile $f.FullName $to)) { $notes.Add("could not copy $($f.Name) (in use)") }
        }
        $ctx = Join-Path $cr.FullName 'CrashContext.runtime-xml'
        if ((Test-Path $ctx) -and ((Get-Content $ctx -Raw) -match '<ErrorMessage>([^<]*)</ErrorMessage>')) {
            $crashMsg = ($Matches[1] -replace '\s+', ' ').Trim()
            if ($crashMsg.Length -gt 300) { $crashMsg = $crashMsg.Substring(0, 300) }
            $notes.Add("crash report: $crashMsg")
        }
    }
    $ue4ssLog = Join-Path $script:GameWin64 'ue4ss\UE4SS.log'
    if (Test-Path $ue4ssLog) { Read-SharedLines $ue4ssLog | Select-Object -Last 150 | Set-Content (Join-Path $dir 'UE4SS-tail.log') -Encoding utf8 }
    if (Test-Path $script:BenchLogFile) { Read-SharedLines $script:BenchLogFile | Select-Object -Last 200 | Set-Content (Join-Path $dir 'bench-tail.log') -Encoding utf8 }
    # the DLL log (its [gpucrash] / [crash] blocks): the next game overwrites it
    $fixLog = Join-Path $script:GameWin64 $script:FixLogName
    if ((Test-Path $fixLog) -and -not (Copy-SharedFile $fixLog (Join-Path $dir $script:FixLogName))) { $notes.Add('could not copy the DLL log') }
    # close what the failure left on screen (crash reporter, error dialogs, AMD driver pop-ups), then the game
    foreach ($a in [OblvBench.GameWatch]::Dismiss()) { $notes.Add("dismissed: $a"); Step "dismissed: $a" }
    [OblvBench.GameWatch]::Stop()
    Update-SaveGuard
    $g = Get-Game
    if ($g) {
        # a hung game still has its audio session: never un-mute it as it is (it may still be playing); freeze it, un-mute
        # only once it reads silence, else end it muted (2026-10-20)
        # not $n: that is this function's [int] run number (a game that had ended by itself failed the run, 2026-10-21)
        foreach ($sq in (Stop-GameQuietly $g)) { $notes.Add($sq) }
        if (-not $g.HasExited) { $notes.Add('the game process did not end within 20 s') }
    }
    Start-Sleep -Seconds 2
    foreach ($a in [OblvBench.GameWatch]::Dismiss()) { $notes.Add("dismissed: $a"); Step "dismissed: $a" }     # a crash reporter that came up as the game ended
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ((Get-Process OblivionRemastered, CrashReportClient -ErrorAction SilentlyContinue) -and $sw.Elapsed.TotalSeconds -lt 20) { Start-Sleep -Milliseconds 500 }
    Start-Sleep -Seconds 3                               # let the driver and Steam settle before the next start
    $notes | Set-Content (Join-Path $dir 'problem.txt') -Encoding utf8
    if ($crashMsg) { "$problem; crash report: $crashMsg" } else { $problem }
}

# ---------------------------------------------------------------- frame capture

function Get-CsvLength([string]$path) { if (Test-Path -LiteralPath $path) { (Get-Item -LiteralPath $path).Length } else { 0 } }

<# Starts PresentMon on the game and makes sure frames actually arrive (the CSV grows past its header
   within a few seconds). A trace session left running by one of our tools (Record-Session's OblvSession,
   2026-10-05) silently starved it once, so stale sessions are stopped and it is tried twice.
   If the first try gets nothing, tools\Diagnose-PresentMon.ps1 looks at why (6 s; verdict in the log, details in
   <csv>.diag-<n>\presentmon-diag.json). When the game presents but PresentMon never sees its frames reach the display
   (2026-10-09: intermittent, the window is off-screen), the second try records CPU-side only (--no_track_display
   --no_track_gpu: frame times and CPU busy as before, no GPU busy) and $script:frameCaptureCpuOnly says so.
   Returns the PresentMon process, or $null if no frames come. #>
function Start-FrameCapture($game, [string]$csv) {
    $script:frameCaptureCpuOnly = $false
    # -Record: CPU-side from the start. Display tracking needs DWM to compose the off-screen window's frames; while the
    # PC's screen is static it does not, the game's presents pile up unfinished, PresentMon's buffer overflows and whole
    # stretches of frames are lost (2026-10-11 promo-stock-1-r2: a 17.8 s hole in a take whose video moved on). The
    # takes only need frame times.
    $cpuOnly = [bool]$Record
    for ($try = 1; $try -le 2; $try++) {
        if (-not (Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='pwsh.exe'" | Where-Object { $_.CommandLine -match 'Record-Session|Profile-Oblivion' })) {
            # sessions of our own tools: Record-Session, this runner, lab LiveAB (uecvars) and frozen windows (drift)
            foreach ($stale in 'OblvSession', 'OblvBench', 'OblvUecvars', 'OblvDrift') { & logman stop $stale -ets 2>&1 | Out-Null }
        }
        if (Test-Path -LiteralPath $csv) { Remove-Item -LiteralPath $csv -ErrorAction SilentlyContinue }
        $pmOut = "$csv.presentmon-$try.txt"
        # harness test: $env:OBLV_BENCH_PM_TEST = 'fail1' (first try records another pid: diagnosis, then a normal second
        # try) or 'cpuonly' (the same, and the second try records CPU-side only)
        $pmPid = if ($try -eq 1 -and $env:OBLV_BENCH_PM_TEST) { 4 } else { $game.Id }
        $pmArgs = @('--process_id', $pmPid, '--output_file', "`"$csv`"", '--no_console_stats',
            '--stop_existing_session', '--session_name', 'OblvBench', '--qpc_time_ms', '--terminate_on_proc_exit',
            '--set_circular_buffer_size', 8192)                # PresentMon's own advice after "overflowed present events" (default 2048)
        if ($cpuOnly) { $pmArgs += '--no_track_display', '--no_track_gpu' }
        $pm = Start-Process $presentMon -PassThru -WindowStyle Hidden -RedirectStandardError "$pmOut.err" -RedirectStandardOutput $pmOut -ArgumentList $pmArgs
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($sw.Elapsed.TotalSeconds -lt 15) {      # 8 s was sometimes too short (2026-10-09)
            Wait-Game 0.5
            if ((Get-CsvLength $csv) -gt 2000) { $script:frameCaptureCpuOnly = $cpuOnly; return $pm }        # header plus a few frames
            if ($pm.HasExited) { break }
        }
        # what PresentMon said (its usual "requires elevated privilege" warning left out)
        $said = @(Get-Content -LiteralPath $pmOut, "$pmOut.err" -ErrorAction SilentlyContinue |
            Where-Object { $_ -and $_ -notmatch 'elevated privilege|short-running|listed as|terminate_on_proc_exit' }) -join ' | '
        Step "PresentMon got no frames within 15 s (try $try; game pid $($game.Id); exited: $($pm.HasExited)$(if ($pm.HasExited) { " code $($pm.ExitCode)" }); said: $said)"
        Start-Process $presentMon -WindowStyle Hidden -Wait -ArgumentList '--terminate_existing_session', '--session_name', 'OblvBench'
        if (-not $pm.HasExited -and -not $pm.WaitForExit(5000)) { $pm.Kill() }
        if ($try -eq 1) {
            try {
                $diag = & (Join-Path $PSScriptRoot 'Diagnose-PresentMon.ps1') -ProcessId $game.Id -OutDir "$csv.diag-$try"
                Step "PresentMon diagnosis: $($diag.verdict)"
                $cpuOnly = $diag.cpuOnlyFrames -gt 0 -and $diag.displayTrackedGameFrames -eq 0
            } catch { Step "PresentMon diagnosis failed: $_" }
            if ($env:OBLV_BENCH_PM_TEST -eq 'cpuonly') { $cpuOnly = $true }
            if ($cpuOnly) { Step 'PresentMon: next try records CPU-side only (frame times and CPU busy; no GPU busy)' }
        }
    }
    $null
}

# ---------------------------------------------------------------- one pass in one game process

function Get-Dist($a, $b) {
    [math]::Sqrt([math]::Pow([double]$a.x - [double]$b.x, 2) + [math]::Pow([double]$a.y - [double]$b.y, 2) + [math]::Pow([double]$a.z - [double]$b.z, 2))
}

<# Waits until the player has left the old spot (or level/interior changed) and stands still again.
   Returns @{ State; ArriveMs } or $null on timeout.
   Fast transition (Altar.ini bIsFastTransitionEnabled): the exit from an injected interior back to its parent
   world moves the player inside the same persistent level and leaves VLevelChangeData's travel flag set
   (until the next teleport), so a jump of more than 400 units within the same level (not the seamless
   transition map) also counts as arrival while the flag is set. #>
function Test-SameLevelJump($s, $before) {
    $before -and $s.level -eq $before.level -and $s.level -ne 'L_SeamlessTransition' -and (Get-Dist $s $before) -gt 400
}
function Wait-Arrival($before, [double]$timeoutSec = 90, [switch]$maybeInPlace) {
    $sw = [Diagnostics.Stopwatch]::StartNew(); $last = $null; $still = 0; $moved = $false; $arrive = 0
    while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
        Assert-GameOk
        $s = Get-BenchState 3000 -Light
        if ($s -and $s.pawn -eq '1' -and ($s.travel -ne '1' -or (Test-SameLevelJump $s $before))) {
            if (-not $moved) {
                $moved = (-not $before) -or ($before.pawn -ne '1') -or ($s.level -ne $before.level) -or ($s.sub -ne $before.sub) -or ((Get-Dist $s $before) -gt 150) -or
                         ($maybeInPlace -and $sw.Elapsed.TotalSeconds -gt 6)
                if ($moved) { $arrive = Get-QpcMs }
            }
            if ($moved) {
                if ($last -and (Get-Dist $s $last) -lt 30) { $still++ } else { $still = 0 }
                if ($still -ge 2) { return @{ State = $s; ArriveMs = $arrive } }
            }
            $last = $s
        } else { $still = 0; $last = $null }
        Start-Sleep -Milliseconds 100
    }
    $null
}

<# One pass (part) in one game process. $startReason: start the game first (why), else use the running one.
   Returns the session object; .gameDown is set when the game went down (crash, hang, stall), .completed
   lists the scenes that finished, .current the scene that was running. #>
function Invoke-Session([string]$kind, [string]$runDir, [object[]]$sceneList, [int]$part, [string]$startReason) {
    $session = [ordered]@{ kind = $kind; part = $part; segments = New-Object System.Collections.ArrayList; visual = New-Object System.Collections.ArrayList
                           warnings = New-Object System.Collections.ArrayList; completed = New-Object System.Collections.ArrayList; current = $null }
    $sampler = $null; $pm = $null
    $tag = if ($part -gt 1) { "$kind-$part" } else { $kind }
    $framesCsv = Join-Path $runDir "$tag-frames.csv"
    $shotDir = Join-Path $runDir 'shots'; $visDir = Join-Path $runDir 'visual'
    $plotDir = Join-Path $runDir 'plots'
    New-Item -ItemType Directory -Force $shotDir, $visDir, $plotDir | Out-Null
    function Warn([string]$m) { Write-Warning $m; [void]$session.warnings.Add($m) }
    # a CPU-side-only frame capture (Start-FrameCapture's fallback) has frame times and CPU busy but no GPU busy
    function Set-CpuOnlyNote {
        if ($pm -and $script:frameCaptureCpuOnly) {
            $session.frameCapture = 'cpu-only'
            $why = if ($Record) { 'chosen for -Record' } else { 'its frames never showed as displayed' }
            Warn "PresentMon records this pass CPU-side only ($why): fps, frame times and CPU busy are valid, GPU busy and the GPU-bound share are not measured"
        }
    }
    $script:launchMs = $null

    try {
        # ---- the game, in the world
        if ($SelfTest) {
            $game = Get-Game
            if (-not $game) { throw '-SelfTest needs a running game' }
        } else {
            $game = if ($startReason) { Start-BenchGame $startReason } else { Get-Game }
            $session.startedGame = [bool]$startReason
            Watch-Game $game
            if (-not $Visible) { Start-Unobtrusive -Audible:$Audible -Capturable:$Record }    # again after a restart (the recovery un-muted the old game)
            if (-not $startReason) { Send-Bench 'reload' -TimeoutMs 15000 | Out-Null }    # a reused game gets the current bench.lua
            $session.gameStart = $game.StartTime.ToString('s')
            $place = Get-GamePlace
            if ($place -eq 'busy') {
                # loading, or a menu: give it a moment to settle into the world or the main menu
                $sw = [Diagnostics.Stopwatch]::StartNew()
                while ($place -eq 'busy' -and $sw.Elapsed.TotalSeconds -lt 60) { Wait-Game 2; $place = Get-GamePlace }
                if ($place -eq 'busy') { throw 'GAMEDOWN: stuck: the game is neither in the world nor at the main menu' }
            }
            if ($place -eq 'world' -and -not $KeepWorld) {
                Step 'back to the main menu for a fresh world'
                Enter-MainMenu; $place = 'menu'
            }
            $freshWorld = $place -eq 'menu'
            if ($freshWorld -and $startReason -and $MenuHoldSec -gt 0) {
                Step "holding at the main menu for $MenuHoldSec s (-MenuHoldSec)"
                Set-BenchStatus @{ phase = 'main menu hold' }
                Wait-Game $MenuHoldSec
                $session.menuHoldSec = $MenuHoldSec
            }
            if ($freshWorld -and $startReason -and $MenuPsoWait -and -not $SelfTest) {
                # the precompile runs at the main menu as it would for a player waiting there (the fix's PsoPrecompileFast
                # makes it faster); waiting here rather than in the world keeps the world's clock and streaming out of it
                Set-BenchStatus @{ phase = 'main menu: waiting for the PSO precompile' }
                Step "main menu: waiting for the game's PSO precompile (-MenuPsoWait)"
                $base = 0L
                try { $base = $game.MainModule.BaseAddress.ToInt64() } catch { }
                $sw = [Diagnostics.Stopwatch]::StartNew(); $v = -1
                while ($base) {
                    $v = [OblvBench.GameFlags]::PsoPrecompiling($game.Id, $base)
                    if ($v -le 0 -or $sw.Elapsed.TotalSeconds -ge 360) { break }
                    Wait-Game 2
                }
                $session.menuPsoWaitSec = if ($v -lt 0) { -1 } else { [int]$sw.Elapsed.TotalSeconds }
                if ($v -lt 0) { Step 'PSO precompile state not readable in this game build: not waiting at the main menu' }
                elseif ($v -gt 0) { Warn "the game's PSO precompile still running after 360 s at the main menu: entering the world anyway" }
                else { Step "waited $($session.menuPsoWaitSec) s at the main menu for the game's PSO precompile (-MenuPsoWait)" }
            }
            if ($freshWorld) { $s = Enter-World } else { $s = Get-BenchState; Step 'using the world as it is (-KeepWorld)' }
            if ($script:launchMs) { $session.loadSec = Rnd (((Get-QpcMs) - $script:launchMs) / 1000) }
            Step ("in the world: {0}{1}" -f $s.level, $(if ($script:launchMs) { " ($($session.loadSec) s after launch)" }))
            Set-BenchStatus @{ game = 'world'; gamePid = $game.Id }
            if (-not $Visible) {
                # a hook that came after the window of a game this run started (2026-10-20): the window may have been on
                # screen for a moment. In a reused game the hook DLL is in it since its launch, so a re-hook finds the window up.
                # only for this run's game: the previous (parked) game is re-hooked before -Session fresh closes it, and its
                # window is up by then (a false warning seen 2026-10-22/23)
                foreach ($line in Get-UnobtrusiveLog) {
                    $otherPid = $line -match '\bpid (\d+)\b' -and [int]$Matches[1] -ne $game.Id
                    if ($session.startedGame -and -not $otherPid -and $line -match 'its window was already up|before its window came up') { Warn "late window hook: $line" } else { Step $line }
                }
                $session.hidden = [OblvBench.GameHider]::Stats()
                if ([OblvBench.GameHider]::HookedPid -ne $game.Id) { Warn "the window hook is not in this game (pid $($game.Id); hook: $($session.hidden)): its window is only kept off-screen from outside" }
            }
        }
        $hwnd = [OblvBench.GameWatch]::FindGameWindow($game.Id)   # not MainWindowHandle: it skips owned windows (-Record)
        if ($hwnd -eq [IntPtr]::Zero) { $hwnd = $game.MainWindowHandle }
        # next to the DLL: Win64 as version.dll, OBSE\Plugins as an OBSE64 plugin
        $fixLog = @('OblivionPerfFix.log', 'OBSE\Plugins\OblivionPerfFix.log' | ForEach-Object { Join-Path $script:GameWin64 $_ } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1)
        if ($fixLog) { $session.perfFixLog = @(Read-SharedLines $fixLog | Select-Object -First 20) }

        # ---- settings: scenario defaults, the profile, then the change under test; record what is in effect
        $effective = [ordered]@{}
        if (-not $SelfTest) {
            Send-Bench 'prep' | Out-Null
            # No damage (lava, the fall after a flight) and nobody reacting to the player. `tdetect` and `tai` are
            # toggles, so what was switched in this game process is remembered across runs.
            $state = Get-SessionState $game
            $state.parked = $false; $state.runs = [int]$state.runs + 1
            if ($freshWorld) { $state.spawned = @() }      # a new world has none of the actors earlier runs spawned
            Save-SessionState $state
            if ((Send-Bench 'god') -notmatch 'true') {
                Send-Bench 'god toggle' | Out-Null; Wait-Game 1.5
                if ((Send-Bench 'god') -notmatch 'true') { Warn 'god mode could not be confirmed' }
            }
            # every map marker known: flying past a place would otherwise "discover" it, with its sound and banner
            # (distracting in recordings, and a UI event in the measurement). Not a toggle; a new world needs it again.
            if ($null -eq (Send-Bench 'obv tmm 1' -NoThrow)) { Warn 'could not mark the map markers known (tmm 1): places may be discovered during scenes' }
            foreach ($cmd in @(Opt $scn 'exec' @())) { Send-Bench "exec $cmd" | Out-Null }
            if (@(Opt $scn 'exec' @()).Count) { Wait-Game 3 }
            if (-not $state.initialized) {
                Send-Bench 'obv tdetect' | Out-Null
                $state.initialized = $true; $state.detectOff = $true
            }
            if ($state.aiOff) { Send-Bench 'obv tai' | Out-Null; $state.aiOff = $false }      # left over from an aborted visual pass
            Save-SessionState $state
            foreach ($p in $scn.settings.PSObject.Properties) { Send-Bench "cvar $($p.Name) $($p.Value)" | Out-Null }
            Step "profile $Profile`: $(Opt $prof 'description' '')"
            if ($pinOn) { Step 'conditions pinned (-Pin): every scene gets its weather (held), hour and timescale' }
            elseif ($script:pinRefused) { Warn "scenario $($scn.name) says ""pin"": false (it tests weather changes): -Pin ignored" }
            # -KeepWorld in a game that already has this profile from an earlier run in the same world: apply only what
            # drifted (plus the profile's own cvars). The full set switches the upscaler to TAA at 100 % and back, which at
            # 4K with a loaded world is a large render-target reallocation: two 4K GPU hangs (DXGI_ERROR_DEVICE_HUNG, VRAM over
            # its budget) came right at that toggle in the second reused run of a held world (2026-10-20, npcpromo).
            $kept = $KeepWorld -and -not $freshWorld -and $state.profileApplied -eq $Profile -and $null -eq $state.capBeforePark -and $Cvars.Count -eq 0
            if ($kept) {
                $parts = @(@(Get-ProfileDrift $prof $hwnd) | ForEach-Object { $_.part } | Select-Object -Unique)
                Step "profile $Profile kept from the last run in this world$(if ($parts.Count) { "; re-applied: $($parts -join ', ')" } else { '' })"
                foreach ($partName in $parts + @('cvars')) { Invoke-ProfileCommands (Get-ProfileCommands $prof $partName) }
            } else { Invoke-ProfileCommands (Get-ProfileCommands $prof) }
            $state.profileApplied = $Profile; Save-SessionState $state
            # Profile "own" keeps the player's frame cap: a game parked by an earlier run gets its cap back (parking sets 30).
            # The other profiles set it themselves (0 = uncapped, or their own cvar).
            if ($null -ne $state.capBeforePark) {
                if (Test-OwnProfile $prof) { Send-Bench "cvar t.MaxFPS $($state.capBeforePark)" | Out-Null }
                $state.capBeforePark = $null; Save-SessionState $state
            }
            foreach ($k in $Cvars.Keys) { Step ('cvar under test: ' + (Send-Bench "cvar $k $($Cvars[$k])")) }
            Wait-Game 2                                   # a new window size takes a moment
            foreach ($d in @(Get-ProfileDrift $prof $hwnd)) { Warn "profile $Profile not in effect after applying it: $($d.text)" }
            if (-not $NoSpectate) {
                Step ('spectator mode: ' + (Send-Bench 'spectate on'))
                # NPC AI treats the invisible player as not there: forced greetings need no detection, so `tdetect` does
                # not stop them, and the open conversation pauses the world (the hook DLL turns them away)
                $session.talkGuard = Set-BenchObserver $true $game.Id
                if ([OblvBench.GameHider]::TalkGuardFor($game.Id) -eq 1) { Step "NPC conversation guard: $($session.talkGuard)" }
                else { Warn "NPCs can still start conversations with the player (a forced greeting pauses the world for the rest of its scene): the hook's conversation guard is $($session.talkGuard)" }
                $script:talksSeen = [OblvBench.GameHider]::TalksRefused; $script:targetsSeen = [OblvBench.GameHider]::TargetsRefused
            }
            # the mod keeps these as the profile has them for the whole pass (the game switches FSR off by itself)
            Send-Bench 'pin clear' | Out-Null
            foreach ($kv in (Get-ProfileExpect $prof).GetEnumerator()) { if ($kv.Key -notin 'window', 'r.ScreenPercentage' -and $kv.Key -notlike 'sg.*') { Send-Bench "pin $($kv.Key) $($kv.Value)" | Out-Null } }
            $script:pinCounts = @{}
            $names = @($scn.record) + @((Get-ProfileExpect $prof).Keys | Where-Object { $_ -ne 'window' }) + @('Altar.GraphicsOptions.EnableHardwareRaytracing', 'Altar.FSR3.FI.Enabled') + @($Cvars.Keys) + @($IniCvars.Keys)
            foreach ($name in $names | Select-Object -Unique) { $effective[$name] = (Send-Bench "get $name") -replace '^.*=', '' }
        }
        $size = [OblvBench.ImageTools]::ClientSize($hwnd)
        $effective['window'] = "$($size[0])x$($size[1])"
        $session.settings = $effective
        if (-not $SelfTest) {
            # a frame cap in effect during the pass (t.MaxFPS; the game's Frame Rate Limit option sets it): fps are capped
            $cap = (Send-Bench 'get t.MaxFPS' -NoThrow) -replace '^.*=', ''
            $capValue = 0.0
            # the profile's own cap (`user`: 60 before frame generation); every other profile runs uncapped
            $profCap = [double](Opt (Opt $prof 'cvars' $null) 't.MaxFPS' 0)
            if (-not (Test-OwnProfile $prof) -and [double]::TryParse($cap, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$capValue) -and $capValue -gt 0 -and [math]::Abs($capValue - $profCap) -gt 0.5) {
                # After a fresh launch the game re-applies the player's Frame Rate Limit a little after the profile cleared
                # it (seen in about one run in three on a PC whose limit is 60): set the profile's cap again (none for most
                # profiles; until 2026-10-13 this cleared the `user` profile's own 60 cap too, so those runs were uncapped).
                foreach ($x in 'cvar Altar.GraphicsOptions.FrameRateLimit 0', "cvar t.MaxFPS $profCap") { Send-Bench $x -NoThrow | Out-Null }
                Start-Sleep -Milliseconds 500
                $cap = (Send-Bench 'get t.MaxFPS' -NoThrow) -replace '^.*=', ''
                Step "frame cap $capValue re-applied by the game after the profile; set back to the profile's $profCap (now t.MaxFPS = $cap)"
                if ($null -ne $effective['t.MaxFPS']) { $effective['t.MaxFPS'] = $cap }   # the settings this pass starts with (checked again at its end)
                $capValue = 0.0
            }
            if ([double]::TryParse($cap, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$capValue) -and $capValue -gt 0) {
                $session.frameCap = $capValue
                Step ("fps capped at {0} by {1}" -f $capValue, $(if (Test-OwnProfile $prof) { "the player's settings" } else { "profile $Profile" }))
            }
        }

        # ---- capture
        $pm = Start-FrameCapture $game $framesCsv
        # No frames right after the set-up happens (2026-10-09: 6 of 11 runs, while PresentMon started by hand a minute
        # later recorded the same game at once): tried again after the warm-up, warned only if that fails too.
        if (-not $pm) {
            if ($kind -eq 'perf' -and -not $SelfTest) { Step 'PresentMon: no frames yet, trying again after the warm-up' }
            else { Warn 'PresentMon records no frames of the game (the diagnosis line above says why): fps cannot be measured in this pass' }
        }
        Set-CpuOnlyNote
        $sampler = New-Object OblvBench.ProcSampler $game.Id, 250
        $sampler.Start()

        function Save-Shot([string]$name) {
            $file = Join-Path $shotDir "$name.png"
            Assert-GameOk                                  # a frozen game cannot be captured (and the scene is void)
            try { [OblvBench.ImageTools]::CaptureToFile($hwnd, $file, 5000); "shots/$name.png" }
            catch {
                # a window that does not answer a capture for 5 s is probably frozen: wait for the watchdog's verdict
                # (it throws GAMEDOWN within its stall limit) so this scene, not the next, is the one that is repeated
                $sw = [Diagnostics.Stopwatch]::StartNew()
                while ($sw.Elapsed.TotalSeconds -lt 45 -and [OblvBench.GameWatch]::HeartbeatAge -gt 2) { Wait-Game 1 }
                Warn "screenshot $name failed: $($_.Exception.InnerException.Message)"; $null
            }
        }

        # -LegShots: a capture loop on its own runspace (same process, so BenchLib's types are loaded) that saves the
        # window every $intervalMs as <dir>\<QPC ms>.png until stopped (at most 120 s); line the files up with the
        # leg's activation and arrival by their QPC names
        function Start-LegShots([string]$dir, [int]$intervalMs) {
            New-Item -ItemType Directory -Force $dir | Out-Null
            $sync = [hashtable]::Synchronized(@{ stop = $false; n = 0 })
            $ps = [powershell]::Create()
            [void]$ps.AddScript({
                param($hwnd, $dir, $intervalMs, $sync)
                $end = [OblvBench.Clock]::QpcMs() + 120000
                while (-not $sync.stop -and [OblvBench.Clock]::QpcMs() -lt $end) {
                    $t = [OblvBench.Clock]::QpcMs()
                    try {
                        # 640 px wide JPEG: enough to tell black / loading screen / world apart, small at 4K too
                        $b = [OblvBench.ImageTools]::Capture($hwnd)
                        try {
                            $h = [int]($b.Height * 640 / $b.Width)
                            $s = New-Object System.Drawing.Bitmap $b, 640, $h
                            try { $s.Save((Join-Path $dir ('{0:F0}.jpg' -f $t)), [System.Drawing.Imaging.ImageFormat]::Jpeg) } finally { $s.Dispose() }
                        } finally { $b.Dispose() }
                        $sync.n++
                    } catch { }
                    $wait = $intervalMs - ([OblvBench.Clock]::QpcMs() - $t)
                    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) }
                }
            }).AddArgument($hwnd).AddArgument($dir).AddArgument($intervalMs).AddArgument($sync)
            @{ PS = $ps; Handle = $ps.BeginInvoke(); Sync = $sync }
        }
        function Stop-LegShots($h) {
            if (-not $h) { return }
            $h.Sync.stop = $true
            if (-not $h.Handle.AsyncWaitHandle.WaitOne(5000)) { Warn 'leg shots: the capture loop did not stop' }
            try { $h.PS.Dispose() } catch { }
        }

        # Driver shader compilation after a start keeps cores busy for minutes on a cold cache: wait it out.
        function Wait-Quiet([int]$maxSec) {
            $sw = [Diagnostics.Stopwatch]::StartNew()
            while ($sw.Elapsed.TotalSeconds -lt $maxSec) {
                Wait-Game 5
                $now = Get-QpcMs
                $d3d = ($sampler.Query($now - 5000, $now) | Where-Object { $_.Name -eq 'D3D Background Thread' } | Measure-Object CpuMs -Sum).Sum
                if ($d3d / 5000 -lt 0.25) { return [int]$sw.Elapsed.TotalSeconds }
            }
            Warn "driver shader compilation still busy after $maxSec s: results may be low"
            $maxSec
        }

        # After a launch the game precompiles its bundled PSO cache for ~2-3 min (FShaderPipelineCache::IsPrecompiling):
        # every thread runs slower meanwhile (canal 34 vs 53 fps, 2026-10-06) and doors hold the loading screen up to
        # 5 s longer. Wait it out (read from the game's memory; build-specific, skipped when the code does not match).
        function Wait-PsoPrecompile([int]$maxSec) {
            $base = 0L
            try { $base = $game.MainModule.BaseAddress.ToInt64() } catch { }
            if (-not $base) { return -1 }
            $sw = [Diagnostics.Stopwatch]::StartNew()
            while ($true) {
                $v = [OblvBench.GameFlags]::PsoPrecompiling($game.Id, $base)
                if ($v -lt 0) { if ($sw.Elapsed.TotalSeconds -lt 1) { Step 'PSO precompile state not readable in this game build: not waiting for it' }; return -1 }
                if ($v -eq 0) { return [int]$sw.Elapsed.TotalSeconds }
                if ($sw.Elapsed.TotalSeconds -ge $maxSec) { Warn "the game's PSO precompile still running after $maxSec s: results may be low"; return $maxSec }
                Wait-Game 3
            }
        }

        # The game changes settings under us (it turns FSR off whenever its window loses the focus): put the profile back
        function Repair-Settings([string]$before) {
            if ($SelfTest) { return }
            $drift = @(Get-ProfileDrift $prof $hwnd)
            if (-not $drift.Count) { return }
            Warn "before $before the game had changed the profile's settings ($(($drift | ForEach-Object { $_.text }) -join '; ')): applied again"
            foreach ($partName in @($drift | ForEach-Object { $_.part } | Select-Object -Unique)) { Invoke-ProfileCommands (Get-ProfileCommands $prof $partName) }
            Wait-Game 1.5
            foreach ($d in @(Get-ProfileDrift $prof $hwnd)) { Warn "still not in effect before ${before}: $($d.text)" }
        }

        $script:place = $null
        # Teleports to a flight's first waypoint, facing along the path; nothing to do if the player is already there
        # (pre-positioned before the warm-up, see below). Returns the path heading (Oblivion degrees).
        function Move-ToFlightStart($scene) {
            $path = @($scene.path)
            $heading = ([math]::Atan2($path[1][0] - $path[0][0], $path[1][1] - $path[0][1]) * 180 / [math]::PI + 360) % 360
            $before = Get-BenchState
            # (horizontally, within 1500 units: the player falls into the water and drifts after the teleport; the flight lifts it back)
            $start = [pscustomobject]@{ x = $path[0][0]; y = $path[0][1]; z = $before.z }
            # "cell": a flight inside an interior (always teleported to: interiors share coordinates)
            $cell = Opt $scene 'cell' $null
            if (-not $cell -and $before.level -eq "L_$($scene.world)" -and $before.travel -ne '1' -and (Get-Dist $before $start) -lt 1500) { return $heading }
            # held in the air at the first waypoint from the first frame there: no fall into the water (splash, underwater sound)
            Send-Bench ("airhold {0} {1} {2} {3:F1} {4}" -f $path[0][0], $path[0][1], $path[0][2], $heading, (Opt $scene 'pitch' 0)) -NoThrow | Out-Null
            $where = if ($cell) { "cell $cell" } else { "world $($scene.world)" }
            # The game at times holds a teleport into an interior back (2026-10-11, 2 of 3 runs: PositionCell from the
            # chapel steps in CheydinhalWorld only turned the player; it ran after the next cell change): check the
            # arrival (right cell or world, near the start); the second try for an interior is `coc <cell>` (its marker
            # is by the entrance; the held-back PositionCell or the flight's lift then moves the player to the start)
            for ($try = 1; $try -le 2; $try++) {
                if ($try -eq 2 -and $cell) { Send-Bench "obv coc $cell" | Out-Null }
                else { Send-Bench ("tp $where {0} {1} {2} {3:F1}" -f $path[0][0], $path[0][1], $path[0][2], $heading) -TimeoutMs 120000 | Out-Null }
                $arr = Wait-Arrival $before 120 -maybeInPlace
                if (-not $arr) { throw 'did not arrive at the start of the path' }
                $s = $arr.State
                $there = if ($cell) { $s.sub -eq "L_$cell" } else { $s.level -eq "L_$($scene.world)" }
                if ($there -and (Get-Dist $s ([pscustomobject]@{ x = $path[0][0]; y = $path[0][1]; z = $s.z })) -lt 3000) { return $heading }
                Warn "$($scene.id): the teleport left the player at $($s.level)/$($s.sub) $($s.x),$($s.y) (try $try)"
                if ($try -eq 1) {
                    Wait-Game 2
                    Send-Bench ("airhold {0} {1} {2} {3:F1} {4}" -f $path[0][0], $path[0][1], $path[0][2], $heading, (Opt $scene 'pitch' 0)) -NoThrow | Out-Null
                    $before = Get-BenchState
                }
            }
            throw 'did not arrive at the start of the path'
        }

        function Move-To([string]$name) {
            $p = $scn.places.$name
            if (-not $p) { throw "unknown place '$name'" }
            # "here": true = where the loaded save (or the previous scene) left the player: no teleport, no turn
            if (Opt $p 'here' $false) {
                $script:place = $name
                Send-Bench 'prep' | Out-Null
                if ($p.PSObject.Properties['heading']) { Send-Bench ("look {0} {1}" -f $p.heading, (Opt $p 'pitch' 0)) | Out-Null }
                return
            }
            if ($script:place -ne $name) {
                $before = Get-BenchState
                $where = if ($p.PSObject.Properties['cell']) { "cell $($p.cell)" } else { "world $($p.world)" }
                Send-Bench ("tp $where {0} {1} {2} {3}" -f $p.pos[0], $p.pos[1], $p.pos[2], $p.heading) -TimeoutMs 120000 | Out-Null
                $arr = Wait-Arrival $before 120 -maybeInPlace
                if (-not $arr) { throw "did not arrive at '$name'" }
                # "hover": true = a view from the air (e.g. along a flight): the mod lifts the player back to the place and
                # holds it there, facing `heading` at `pitch`, until the next flight or teleport
                if (Opt $p 'hover' $false) {
                    $r = [double]$p.heading * [math]::PI / 180
                    Send-Bench ("fly default 600 {0} 0 hold=100000 {1},{2},{3} {4:F0},{5:F0},{3}" -f (Opt $p 'pitch' 0), $p.pos[0], $p.pos[1], $p.pos[2],
                        ($p.pos[0] + 1000 * [math]::Sin($r)), ($p.pos[1] + 1000 * [math]::Cos($r))) | Out-Null
                    Wait-Game 3
                    $script:place = $name
                    return
                }
                # A long teleport inside the same loaded world (e.g. Tamriel -> Tamriel, or out of an interior that fast
                # transition injected into Tamriel) can drop the player before the terrain there has streamed in: the
                # player falls through it (2026-10-05: z = -319 instead of 1125 at Weye). Teleport once more, now that it is loaded.
                if ($p.PSObject.Properties['world'] -and $arr.State -and [math]::Abs([double]$arr.State.z - [double]$p.pos[2]) -gt 400) {
                    Warn "teleport to '$name' left the player at z=$($arr.State.z) (place z=$($p.pos[2])): teleporting again"
                    Wait-Game 2
                    $again = Get-BenchState
                    Send-Bench ("tp $where {0} {1} {2} {3}" -f $p.pos[0], $p.pos[1], $p.pos[2], $p.heading) -TimeoutMs 120000 | Out-Null
                    if (-not (Wait-Arrival $again 60 -maybeInPlace)) { throw "did not arrive at '$name' (second try)" }
                }
                $script:place = $name
            }
            Send-Bench 'prep' | Out-Null
            Send-Bench ("look {0} {1}" -f $p.heading, (Opt $p 'pitch' 0)) | Out-Null
        }

        # -Pin: one value per condition, scene -> scenario defaults -> the fixed default; "keep" leaves it as the world
        # has it. A run from a save keeps the save's conditions where neither the scene nor the defaults name one.
        function Get-PinValue($scene, [string]$key, $fallback) {
            $v = Opt $scene $key $null
            if ($null -eq $v) { $v = Opt $scn.defaults $key $null }
            if ($null -eq $v) { $v = if ($Start -ne 'fresh') { 'keep' } else { $fallback } }
            $v
        }
        function Get-WeatherId([string]$name, $scene) {
            $id = Opt $scn.weathers $name $null
            if (-not $id) { $id = @{ Clear = '00038EEE'; SEClear = '00078801' }[$name] }
            if (-not $id) { throw "unknown weather '$name'" }
            $id
        }
        function Set-PinnedConditions($scene) {
            $c = [ordered]@{ pinned = $true }
            $hour = Get-PinValue $scene 'hour' 12
            if ("$hour" -ne 'keep') { Send-Bench "obv set gamehour to $hour" | Out-Null }
            $c.hour = "$hour"
            $ts = Get-PinValue $scene 'timescale' 1
            if ("$ts" -ne 'keep') { Send-Bench "obv set timescale to $ts" | Out-Null; $script:timescaleSet = $true }
            elseif ($script:timescaleSet) { Send-Bench 'obv set timescale to 30' | Out-Null; $script:timescaleSet = $false }
            $c.timescale = "$ts"
            # no weather named anywhere: Clear (SEClear in the Shivering Isles' worldspaces)
            $world = "$(Opt $scene 'world' (Opt (Opt $scn.places "$($scene.at)" $null) 'world' ''))"
            $w = Get-PinValue $scene 'weather' $(if ($world -like 'SE*') { 'SEClear' } else { 'Clear' })
            if ("$w" -eq 'keep') {
                if ($state.wxOverride) { Send-Bench 'obv ReleaseWeatherOverride' | Out-Null; $state | Add-Member -Force NoteProperty wxOverride $false; Save-SessionState $state }
            } else {
                # the 1 = override: the climate cannot change it until ReleaseWeatherOverride (checked 2026-10-20: held
                # over 17 game hours at timescale 2000); the atmosphere tool switches within a frame
                $id = Get-WeatherId $w $scene
                Send-Bench "obv fw $id 1" | Out-Null
                $state | Add-Member -Force NoteProperty wxOverride $true; Save-SessionState $state
                # After a teleport or load, or after the hour was set back, the atmosphere tool blends into the weather
                # over 15 game minutes (30 s at the game's timescale 30, ~15 min at timescale 1), and forcing the same
                # weather again does not restart it; forcing another weather and then this one switches at once
                # (2026-10-20). So: until the weather reads back fully applied, flip once through another weather.
                Wait-Game 0.3
                for ($try = 0; $try -lt 2; $try++) {
                    $now = Send-Bench 'cond 0' -NoThrow
                    if ("$now" -match '(?:^|\s)wx=([^\s,]+):(1\.00)(?:\s|$)') { break }
                    $other = if ($id -eq '00038EEE') { '00038EF0' } else { '00038EEE' }
                    Send-Bench "obv fw $other 1" | Out-Null; Wait-Game 0.3
                    Send-Bench "obv fw $id 1" | Out-Null; Wait-Game 0.3
                    $c.flipped = $try + 1
                }
            }
            $c.weather = "$w"
            Wait-Game 0.3
            $c.atSet = Send-Bench 'cond 0' -NoThrow
            $script:sceneCond = $c
        }

        function Set-Conditions($scene) {
            $script:sceneCond = $null
            if ($pinOn) { Set-PinnedConditions $scene; return }
            $hour = Opt $scene 'hour' (Opt $scn.defaults 'hour' $null)
            if ($null -ne $hour) { Send-Bench "obv set gamehour to $hour" | Out-Null }     # no hour: keep the save's
            # "timescale": game minutes per real minute (game default 30). A low value holds the light (sunset) for the
            # whole scene, so recordings of the same flight match; put back to 30 for the next scene that has none
            $ts = Opt $scene 'timescale' $null
            if ($null -ne $ts -or $script:timescaleSet) {
                $v = if ($null -ne $ts) { $ts } else { 30 }
                Send-Bench "obv set timescale to $v" | Out-Null
                $script:timescaleSet = $null -ne $ts
            }
            $w = Opt $scene 'weather' $null
            if ($w) {
                $id = $scn.weathers.$w
                if (-not $id) { throw "unknown weather '$w'" }
                Send-Bench "obv fw $id" | Out-Null
            }
        }

        # the conditions the scene had (Set-Conditions, and read back again at its end; "cond" in bench.lua)
        function Add-Segment([hashtable]$seg) {
            if ($script:sceneCond -and -not $SelfTest) {
                $c = [ordered]@{}; foreach ($k in $script:sceneCond.Keys) { $c[$k] = $script:sceneCond[$k] }
                $c.atEnd = Send-Bench 'cond' -NoThrow
                $seg.cond = $c
                if ($c.pinned -and "$($c.weather)" -ne 'keep' -and "$($c.atSet)" -match 'wx=(\S+)' -and "$($c.atEnd)" -match 'wx=(\S+)') {
                    $a = ("$($c.atSet)" -replace '^.*wx=(\S+).*$', '$1'); $b = $Matches[1]
                    if ($a -ne $b) { Warn "$($seg.id): the weather changed during the scene ($a -> $b) although it was pinned" }
                }
            }
            [void]$session.segments.Add($seg)
        }

        # Did the game change a pinned setting during the scene? (the mod put it back within half a second)
        function Test-Pins([string]$scene) {
            if ($SelfTest) { return }
            $a = Send-Bench 'pin' -NoThrow
            if ($a -notmatch '^pins=(.+)$' -or $Matches[1] -eq 'none') { return }
            foreach ($e in $Matches[1] -split ',') {
                if ($e -notmatch '^(.+)=(.+):(\d+)$') { continue }
                $n = $Matches[1]; $c = [int]$Matches[3]; $was = [int]$script:pinCounts[$n]
                if ($c -gt $was) { Warn "${scene}: the game changed $n during the scene ($($c - $was) time(s)); the mod set it back within half a second each time" }
                $script:pinCounts[$n] = $c
            }
        }

        # NPCs whose AI went for the player during the scene, turned away by the hook (spectator mode): AI packages that
        # found no target in the player, and conversation starts refused. A package aimed at the player looks for its
        # target again on the NPC's next AI update, so one NPC can count many times.
        function Test-Talks([string]$scene) {
            if ($SelfTest -or $NoSpectate) { return }
            $n = [OblvBench.GameHider]::TalksRefused; $t = [OblvBench.GameHider]::TargetsRefused
            $dn = $n - [int]$script:talksSeen; $dt = $t - [int]$script:targetsSeen
            if ($dn -gt 0 -or $dt -gt 0) {
                $who = if ($dn -gt 0) { " (last NPC: form $([OblvBench.GameHider]::LastRefusedForm), from $([OblvBench.GameHider]::LastRefusedCaller))" } else { '' }
                Step "${scene}: NPC AI turned away from the player: $dt package target(s), $dn conversation start(s)$who; target call sites so far: $([OblvBench.GameHider]::TargetCallers)"
                $session.talksRefused = [int]$session.talksRefused + $dn
                $session.targetsRefused = [int]$session.targetsRefused + $dt
            }
            $script:talksSeen = $n; $script:targetsSeen = $t
        }

        if ($SelfTest) {
            Step 'self-test: passive capture, 15 s'
            Start-Sleep -Seconds 2
            $t0 = Get-QpcMs; Start-Sleep -Seconds 6; $t1 = Get-QpcMs
            Add-Segment @{ id = 'selftest-static'; type = 'static'; t0 = $t0; t1 = $t1; shot = Save-Shot 'selftest' }
            $t0 = Get-QpcMs; Start-Sleep -Seconds 2; $tA = Get-QpcMs; Start-Sleep -Seconds 4; $t1 = Get-QpcMs
            Add-Segment @{ id = 'selftest-door/enter'; type = 'door'; t0 = $t0; t1 = $t1; tA = $tA; arrived = $true }
            $sceneList = @()
        } elseif ($kind -eq 'perf') {
            # A flight that fades in (the showcase flights) as the first scene: go to its start first, so the start area
            # streams in during the warm-up instead of during the flight's own lift and hover (the scene then finds the
            # player in place and skips its teleport)
            $first = @($sceneList | Where-Object { Opt $_ 'perf' $true })[0]
            if ($first -and $first.type -eq 'fly' -and "$(Opt $first 'flyopt' '')" -match '\bfadein=') {
                Step "going to the start of $($first.id) before the warm-up"
                $null = Move-ToFlightStart $first
                $script:place = $null
                Send-Bench 'prep' | Out-Null
            }
            Set-BenchStatus @{ phase = 'shader warm-up' }
            if ($NoWarmup) { $session.quietSec = 0; Step 'no shader warm-up wait (-NoWarmup)' }
            else {
                $session.quietSec = Wait-Quiet 240
                Step "warm-up done (waited $($session.quietSec) s for shader compilation)"
            }
            if ($SkipPsoWait) {
                # 1 = the game was still precompiling when the scenes started (measured on purpose), 0 = not, -1 = unknown
                $session.psoRunningAtStart = try { [OblvBench.GameFlags]::PsoPrecompiling($game.Id, $game.MainModule.BaseAddress.ToInt64()) } catch { -1 }
                $session.psoWaitSec = 0
                Step "not waiting for the game's PSO precompile (-SkipPsoWait; running now: $($session.psoRunningAtStart))"
            } else { $session.psoWaitSec = Wait-PsoPrecompile 360 }
            if ($session.psoWaitSec -gt 0) { Step "waited $($session.psoWaitSec) s for the game's PSO precompile" }
            if (-not $pm) {
                $pm = Start-FrameCapture $game $framesCsv
                if ($pm) { Step 'PresentMon records frames now' }
                else { Warn 'PresentMon records no frames of the game (the diagnosis line above says why): fps cannot be measured in this pass' }
                Set-CpuOnlyNote

            }
        } else {
            Send-Bench 'obv tai' | Out-Null          # freeze NPC AI so characters stay where the save has them
            $state.aiOff = $true; Save-SessionState $state
        }

        # -Watch and a scenario with "music" (the showcase flights): each flight plays the track from its own offset
        # (music.at[<scene id>] = where its fade-in starts) with ffplay, hidden; the game's music is muted meanwhile.
        # The scenes are cut together in the edit, so the gaps between them (loads, hovers) are silent here.
        $musicFile = $null; $musicVolume = $null
        if ($Watch -and $kind -eq 'perf' -and $scn.PSObject.Properties['music']) {
            $f = Join-Path (Split-Path (Split-Path $script:GameWin64)) ('Content\Dev\ObvData\' + ($scn.music.file -replace '/', '\'))
            $ffplay = Get-Command ffplay -ErrorAction SilentlyContinue
            if (-not (Test-Path -LiteralPath $f)) { Warn "music: $f not found" }
            elseif (-not $ffplay) { Warn 'music: ffplay not found (ffmpeg on the PATH plays the track): watching without it' }
            else {
                $musicFile = $f
                $musicVolume = (Send-Bench 'get Altar.Audio.MusicVolume' -NoThrow) -replace '^.*=', ''
                Send-Bench 'cvar Altar.Audio.MusicVolume 0' -NoThrow | Out-Null
                Step "music: $($scn.music.file) with each flight (the game's own music muted until the end of the pass)"
            }
        }
        $index = 0
        foreach ($scene in $sceneList) { try {
            $index++
            if (-not $SelfTest -and ($stopReq = Get-StopRequest)) {
                if (-not $script:stopNoted) { Warn "stop requested ($(([datetime]$stopReq.time).ToString('HH:mm:ss'))$(if ($stopReq.reason) { ": $($stopReq.reason)" })): the remaining scenes are not run"; $script:stopNoted = $true }
                [void]$session.completed.Add($scene.id); continue
            }
            $settle =[double](Opt $scene 'settle' $scn.defaults.settle)
            $isVisual = [bool](Opt $scene 'visual' $false)
            if ($kind -eq 'visual' -and -not $isVisual) { [void]$session.completed.Add($scene.id); continue }
            if ($kind -eq 'perf' -and -not (Opt $scene 'perf' $true)) { [void]$session.completed.Add($scene.id); continue }    # a scene only for the visual pass
            $session.current = $scene.id
            $left = ($sceneList | Select-Object -Skip ($index - 1) | ForEach-Object { Get-SceneSeconds $_ } | Measure-Object -Sum).Sum
            Set-BenchStatus @{ phase = "$kind pass"; scene = $scene.id; sceneIndex = $index; sceneCount = $sceneList.Count; passEta = (Get-Date).AddSeconds($left).ToString('s') }
            Repair-Settings $scene.id
            # on-screen runs: the game in front with the keyboard focus when each measured scene starts (no input is sent)
            if ($Visible -and $kind -eq 'perf' -and -not $SelfTest) { $fgr = Set-GameForeground; if ($fgr -notmatch 'already$') { Step $fgr } }
            # a flight that fades itself in ("fadein" in its flyopt) keeps the black over its teleport and hover;
            # every other scene clears it (they show their set-up as before)
            $fadesIn = $scene.type -eq 'fly' -and "$(Opt $scene 'flyopt' '')" -match '\bfadein='
            if ($script:faded -and -not ($fadesIn -and $kind -eq 'perf')) { Send-Bench 'fade 0 0.5' -NoThrow | Out-Null; $script:faded = $false }
            # "obv": old-engine console commands run before the scene's set-up (e.g. "0006161E.enable": world state a
            # player's game has but a new game does not; 2026-10-14, kvatch). They change the world for the rest of the pass.
            foreach ($obvCmd in @(Opt $scene 'obv' @())) { Send-Bench "obv $obvCmd" | Out-Null; Wait-Game 0.3 }
            if ($kind -eq 'visual') {
                Step "visual: $($scene.id)"
                Move-To $scene.at; Set-Conditions $scene
                Wait-Game ([math]::Max($settle, 10))
                Send-Bench 'hud 0' | Out-Null
                Send-Bench 'freeze 1' | Out-Null
                Wait-Game 2.5
                $frames = @(for ($i = 0; $i -lt 8; $i++) {
                    $f = Join-Path $visDir "$($scene.id)-f$i.png"
                    Assert-GameOk
                    [OblvBench.ImageTools]::CaptureToFile($hwnd, $f, 5000); $f
                    Start-Sleep -Milliseconds 150
                })
                Send-Bench 'freeze 0' | Out-Null
                if ($NoSpectate) { Send-Bench 'hud 1' | Out-Null }
                $mean = Join-Path $visDir "$($scene.id).png"
                $a = Join-Path $visDir "$($scene.id)-a.png"; $b = Join-Path $visDir "$($scene.id)-b.png"
                [OblvBench.ImageTools]::Average($frames, $mean)
                [OblvBench.ImageTools]::Average($frames[0..3], $a); [OblvBench.ImageTools]::Average($frames[4..7], $b)
                $noise = [OblvBench.ImageTools]::Compare($a, $b, '', '')
                @($frames) + $a + $b | ForEach-Object { Remove-Item -LiteralPath $_ }
                [void]$session.visual.Add([ordered]@{ id = $scene.id; image = "visual/$($scene.id).png"; size = "$($noise.Width)x$($noise.Height)"
                    meanLuma = Rnd $noise.MeanLumaA 2; temporalNoise = Rnd $noise.MeanAbsDiff 3 })
                Test-Pins $scene.id; Test-Talks $scene.id
                [void]$session.completed.Add($scene.id)
                continue
            }

            switch ($scene.type) {
                'static' {
                    Step "scene $index/$($sceneList.Count): $($scene.id)"
                    Move-To $scene.at; Set-Conditions $scene
                    $spawn = @(Opt $scene 'spawn' @())
                    $detect = [bool](Opt $scene 'detect' $false)
                    $aiHeld = $false
                    try {
                        if ($detect -and $state.detectOff) { Send-Bench 'obv tdetect' | Out-Null; $state.detectOff = $false; Save-SessionState $state }
                        # "aiHold": true (shots only, 2026-10-20): NPC AI off from before the spawn to the end of the scene, so
                        # the characters stand where they are for the screenshot (switched off after the spawn, a character
                        # already walking kept walking) (`tai`, as in the visual pass). The scene's
                        # numbers then measure a world without AI: they are marked (aiHeld) and must not be compared.
                        if ([bool](Opt $scene 'aiHold' (Opt $scn.defaults 'aiHold' $false)) -and -not $state.aiOff) {
                            Send-Bench 'obv tai' | Out-Null; $state.aiOff = $true; Save-SessionState $state; $aiHeld = $true
                        }
                        if ($spawn.Count) {
                            if ($state.spawned -contains $scene.id) { Warn "$($scene.id): its actors were already spawned in this game session, so there are more of them than in a fresh run" }
                            foreach ($sp in $spawn) {
                                $side = @{ front = 0; back = 1; left = 2; right = 3 }[[string](Opt $sp 'side' 'front')]
                                Send-Bench "obv player.placeatme $($sp.id) $($sp.count) $($sp.distance) $side" | Out-Null
                            }
                            $state.spawned = @($state.spawned) + $scene.id; Save-SessionState $state
                        }
                        # "aim": "<max distance> [name part]" = stand in front of the nearest such other character and turn the
                        # camera to its head (bench.lua `aim`), again just before the screenshot (spawned actors stand where the
                        # old engine faces, not the camera)
                        $aim = Opt $scene 'aim' $null
                        if ($aim) {
                            Wait-Game 2; $r = Send-Bench "aim $aim front" -NoThrow; if ($r) { Step "  $r" }
                            Wait-Game 1.5; $r = Send-Bench "aim $aim" -NoThrow; if ($r) { Step "  $r" }
                        }
                        Wait-Game $settle
                        $t0 = Get-QpcMs
                        Wait-Game ([double](Opt $scene 'measure' $scn.defaults.measure))
                        $t1 = Get-QpcMs
                        if ($aim) {   # characters walk about: stand in front of it again, then look at its head
                            $r = Send-Bench "aim $aim front" -NoThrow; if ($r) { Step "  $r" }
                            Wait-Game 1.5; $r = Send-Bench "aim $aim" -NoThrow; if ($r) { Step "  $r" }; Wait-Game 0.5
                        }
                        $endState = Get-BenchState
                        Add-Segment @{ id = $scene.id; type = 'static'; t0 = $t0; t1 = $t1; shot = Save-Shot $scene.id; state = $endState; aiHeld = $aiHeld }
                        # A menu or a conversation (e.g. a forced greeting) pauses much of the world: such a scene measured the menu
                        if ($endState -and ($endState.menu -eq '1' -or $endState.frozen -eq '1')) {
                            Warn "$($scene.id): a menu or conversation was open at the end of the scene (menu=$($endState.menu) frozen=$($endState.frozen)): its numbers measure the paused world, not the scene (see the shot)"
                        }
                    } finally {
                        if ($detect -and -not $state.detectOff) {
                            if ($null -ne (Send-Bench 'obv tdetect' -NoThrow)) { $state.detectOff = $true; Save-SessionState $state }
                        }
                        if ($spawn.Count) { $script:place = $null }
                        if ($aiHeld -and $state.aiOff) {
                            if ($null -ne (Send-Bench 'obv tai' -NoThrow)) { $state.aiOff = $false; Save-SessionState $state }
                        }
                    }
                }
                'door' {
                    Step "scene $index/$($sceneList.Count): $($scene.id) (door transition)"
                    Move-To $scene.at; Set-Conditions $scene
                    Wait-Game $settle
                    $sceneHold = [double](Opt $scene 'hold' $scn.defaults.hold)
                    $legs = if ($scene.PSObject.Properties['legs']) { @($scene.legs) } else {
                        @([pscustomobject]@{ name = 'enter'; door = $scene.enter }, [pscustomobject]@{ name = 'exit'; door = $scene.exit }) }
                    foreach ($legDef in $legs) {
                        $leg = $legDef.name
                        # a leg can start somewhere else (another door in the same world: "at") and hold longer ("hold")
                        $hold = [double](Opt $legDef 'hold' $sceneHold)
                        $legAt = Opt $legDef 'at' $null
                        if ($legAt) { Move-To $legAt; Wait-Game $settle }
                        $before = Get-BenchState
                        # a leg can be a map fast travel instead of a door ("fasttravel": "<map icon name>", see the ft verb)
                        $ftTarget = Opt $legDef 'fasttravel' $null
                        if (-not $ftTarget) {
                            # A door leaves the player just in front of it, facing away (and door scenes start at such a
                            # spot): turn around so the door is under the crosshair.
                            Send-Bench ("look {0:F1} 0" -f (([double]$before.heading + 180) % 360)) | Out-Null
                        }
                        Wait-Game 1.5
                        $stackProc = $null
                        if ($Stacks) {
                            # what are the threads doing during the stall? time-stamped stack samples for Analyze-Hitch.ps1
                            $stackPrefix = Join-Path $runDir ('stacks\' + "$($scene.id)-$leg")
                            $stackProc = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                                "`"$(Join-Path $PSScriptRoot 'Sample-Timeline.ps1')`"", '-OutPrefix', "`"$stackPrefix`"", '-Seconds', ($hold + 14), '-Threads', "`"$($Stacks -join ',')`"", '-Sampler', $StackSampler, '-IntervalMs', $StackIntervalMs)
                            # above the game's loading threads, or it gets almost no samples during the long frames
                            try { $stackProc.PriorityClass = 'High' } catch { }
                            Wait-Game 5                       # compiling and attaching takes a few seconds
                        }
                        $logPos = Get-BenchLogLength
                        if ($ftTarget) {
                            # map fast travel: open the map, click the icon (what the map UI sends to the old engine),
                            # then answer its "travel to ...?" box; t0 is the answer
                            Send-Bench 'ft open' | Out-Null
                            # click only once the map page is up and lists its icons (a click into a half-open map
                            # crashed the old engine's handler once: OblivionThread AV exe+0x6561A86, 2026-10-05 12:28)
                            Wait-Game 1.5
                            for ($q = 0; $q -lt 10 -and (Send-Bench "ft list $ftTarget" -NoThrow) -notmatch '^icons=[1-9]'; $q++) { Wait-Game 0.5 }
                            Wait-Game 1
                            $click = Send-Bench "ft go $ftTarget" -NoThrow
                            $question = $null
                            for ($q = 0; $q -lt 10 -and $question -notmatch [regex]::Escape($ftTarget); $q++) { Wait-Game 0.5; $question = Send-Bench 'msg' -NoThrow }   # the box names the destination (a stale box would not)
                            Step "  fast travel to ${ftTarget}: $click; $question"
                            if ($click -notmatch '^clicked' -or $question -notmatch [regex]::Escape($ftTarget)) {
                                Warn "$($scene.id)/${leg}: fast travel to $ftTarget not offered ($click; $question)"
                                Send-Bench 'ft open' -NoThrow | Out-Null      # close the map again
                                break
                            }
                        }
                        Send-Bench 'watch 1' | Out-Null
                        $shotLoop = if ($LegShots -gt 0) { Start-LegShots (Join-Path $shotDir "$($scene.id)-$leg") $LegShots } else { $null }
                        $t0 = Get-QpcMs
                        if ($ftTarget) { Send-Bench 'msg 0' | Out-Null }   # "Yes"
                        else { Send-Bench 'activate' | Out-Null }       # the game's Activate input action, injected: no window focus needed
                        $retries = 0
                        try {
                            $arr = if ($ftTarget) { $null } else { Wait-Arrival $before 8 }
                            # In long sessions the game sometimes ignores the injected activation (2026-10-06, drift: 4 of
                            # 12 door legs after ~15 min, the player at the door, no menu): if nothing has started after
                            # 8 s, activate once more; the leg then starts at the second activation.
                            if (-not $arr -and -not $ftTarget) {
                                # full state: a transition that has begun freezes the world or shows the loading screen
                                # first; never activate again into one (a second activation during a slow start was
                                # followed by a world left frozen, d3-m03 2026-10-06)
                                $now = Get-BenchState 3000
                                if ($now -and $before -and $now.travel -ne '1' -and $now.frozen -ne '1' -and $now.loadscreen -ne '1' -and $now.menu -ne '1' -and
                                    $now.level -eq $before.level -and $now.sub -eq $before.sub -and (Get-Dist $now $before) -lt 50) {
                                    Step "  $($scene.id)/${leg}: nothing started 8 s after the activation: activating again"
                                    $retries = 1; $t0 = Get-QpcMs
                                    Send-Bench 'activate' | Out-Null
                                }
                            }
                            if (-not $arr) { $arr = Wait-Arrival $before 90 }
                            $tA = if ($arr) { $arr.ArriveMs } else { Warn "$($scene.id)/${leg}: no arrival detected"; Get-QpcMs }
                            Wait-Game $hold
                        } finally { Stop-LegShots $shotLoop }
                        $t1 = Get-QpcMs
                        Send-Bench 'watch 0' -NoThrow | Out-Null
                        if ($stackProc -and -not $stackProc.WaitForExit(30000)) { $stackProc.Kill(); Warn "$($scene.id)/${leg}: the stack sampler did not finish" }
                        $events = @(Get-BenchEvents $logPos | ForEach-Object { '{0:F2} {1}' -f $_.Clock, $_.Text })
                        Add-Segment @{ id = "$($scene.id)/$leg"; type = 'door'; t0 = $t0; t1 = $t1; tA = $tA; arrived = [bool]$arr; activationRetries = $retries
                            shot = Save-Shot "$($scene.id)-$leg"; state = Get-BenchState; events = $events }
                        if ($ftTarget -and $arr) {
                            # the map stays in the menu stack after the travel (menu=1, 2026-10-17 anvil-docks): a door
                            # activated afterwards does nothing, so close it the way the player menu does
                            for ($q = 0; $q -lt 3 -and (Get-BenchState).menu -eq '1'; $q++) { Send-Bench 'ft close' -NoThrow | Out-Null; Wait-Game 1 }
                            $menuNow = (Get-BenchState).menu
                            Step "  ${ftTarget}: menu=$menuNow after closing the map"
                            if ($menuNow -eq '1') { Warn "$($scene.id)/${leg}: a menu is still open after the fast travel" }
                        }
                        $script:place = $null
                        if (-not $arr) { break }
                    }
                }
                'walk' {
                    Step "scene $index/$($sceneList.Count): $($scene.id) (walk)"
                    Move-To $scene.at; Set-Conditions $scene
                    Wait-Game $settle
                    $measure = [double](Opt $scene 'measure' $scn.defaults.measure)
                    $pts = (@($scene.waypoints) | ForEach-Object { "$($_[0]),$($_[1])" }) -join ' '
                    $logPos = Get-BenchLogLength
                    try {
                        $stackProc = $null
                        if ($Stacks) {
                            $stackProc = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                                "`"$(Join-Path $PSScriptRoot 'Sample-Timeline.ps1')`"", '-OutPrefix', "`"$(Join-Path $runDir ('stacks\' + $scene.id))`"", '-Seconds', ($measure + 8), '-Threads', "`"$($Stacks -join ',')`"", '-Sampler', $StackSampler, '-IntervalMs', $StackIntervalMs)
                            Wait-Game 5                       # compiling and attaching takes a few seconds
                        }
                        Send-Bench 'watch 1' | Out-Null          # cell and level changes along the way, to line hitches up with
                        $t0 = Get-QpcMs
                        $answer = Send-Bench "walk $(Opt $scene 'mode' 'inject') $measure $pts"
                        $clock0 = if ($answer -match 'clock=([\d.]+)') { [double]$Matches[1] } else { $null }
                        $sw = [Diagnostics.Stopwatch]::StartNew(); $done = $null
                        while (-not $done -and $sw.Elapsed.TotalSeconds -lt $measure + 5) {
                            $done = @(Get-BenchEvents $logPos | Where-Object { $_.Text -match '^walk=done' })[0]
                            if (-not $done) { Wait-Game 0.2 }
                        }
                        $t1 = if ($done -and $null -ne $clock0) { $t0 + 1000 * ($done.Clock - $clock0) } else { Get-QpcMs }
                        if ($stackProc -and -not $stackProc.WaitForExit(30000)) { $stackProc.Kill(); Warn "$($scene.id): the stack sampler did not finish" }
                        if (-not $done) { Warn "$($scene.id): the walk did not report its end" }
                        elseif ($done.Text -notmatch 'reason=arrived') { Warn "$($scene.id): $($done.Text)" }
                        $events = @(Get-BenchEvents $logPos | ForEach-Object { '{0:F2} {1}' -f $(if ($null -ne $clock0) { $_.Clock - $clock0 } else { $_.Clock }), $_.Text })
                        Add-Segment @{ id = $scene.id; type = 'walk'; t0 = $t0; t1 = $t1; shot = Save-Shot $scene.id; state = Get-BenchState; events = $events }
                    } finally {
                        Send-Bench 'walkstop' -NoThrow | Out-Null
                        Send-Bench 'watch 0' -NoThrow | Out-Null
                        $script:place = $null
                    }
                }
                'fly' {
                    Step "scene $index/$($sceneList.Count): $($scene.id) (flight)"
                    $path = @($scene.path)
                    $pitch = Opt $scene 'pitch' -5
                    $tcl = [bool](Opt $scene 'tcl' $false)    # the old engine's collision toggle; the mod switches the UE pawn's collision itself
                    $before = Get-BenchState
                    if ($tcl) { Send-Bench 'obv tcl' | Out-Null }
                    $flyOpt = "$(Opt $scene 'flyopt' '')"
                    # "fadein": black over the teleport and the hover, cleared by the mod as the timed part starts;
                    # "fadeout": black again at the end (so no end screenshot: it would be black)
                    if ($flyOpt -match '\bfadein=' -and $null -ne (Send-Bench 'fade 1' -NoThrow)) { $script:faded = $true }
                    try {
                        $heading = Move-ToFlightStart $scene
                        $script:place = $null
                        Send-Bench 'prep' | Out-Null
                        Send-Bench ("look {0:F1} {1}" -f $heading, $pitch) | Out-Null
                        Set-Conditions $scene
                        $pts = ($path | ForEach-Object { (@($_) | ForEach-Object { "$_" }) -join ',' }) -join ' '      # x,y,z or x,y,z,pitch
                        $logPos = Get-BenchLogLength
                        Send-Bench 'watch 1' | Out-Null          # cell and level changes along the way, to line hitches up with
                        # The mod lifts the player to the first waypoint, hovers there for the settle time ("fly=start"), then
                        # flies the path on a fixed timetable ("fly=done"). Only that timed part is measured.
                        # "look": [heading, pitch] to face while hovering at the start (measured as "<id>/look"), then turn onto the path
                        $look = @(Opt $scene 'look' @())
                        $lookOpt = if ($look.Count -ge 2) { "holdyaw=$($look[0]) holdpitch=$($look[1])" } else { '' }
                        if ($look.Count -ge 2) { Send-Bench ("look {0} {1}" -f $look[0], $look[1]) | Out-Null; Wait-Game 3 }
                        # -Record: OBS starts while the view is still black (hover), so the video has the whole fade-in
                        $rec = $null; $recStop = $null
                        if ($Record) { $recStats0 = Get-ObsStats; $rec = Start-ObsRecord (Join-Path $runDir 'video') }
                        $answer = Send-Bench "fly $(Opt $scene 'method' 'default') $($scene.speed) $pitch $(Opt $scene 'lookOffset' 0) hold=$settle $lookOpt $(Opt $scene 'flyopt' '') $pts"
                        $ackMs = Get-QpcMs
                        $ackClock = if ($answer -match 'clock=([\d.]+)') { [double]$Matches[1] } else { $null }
                        $expect = if ($answer -match 'secs=([\d.]+)') { [double]$Matches[1] } else { 60 }
                        $stackProc = $null
                        if ($Stacks) {
                            # time-stamped stack samples of the flight for Analyze-Hitch.ps1; attaching takes ~5 s, which the hover covers
                            $stackProc = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                                "`"$(Join-Path $PSScriptRoot 'Sample-Timeline.ps1')`"", '-OutPrefix', "`"$(Join-Path $runDir ('stacks\' + $scene.id))`"", '-Seconds', ($settle + $expect + 4), '-Threads', "`"$($Stacks -join ',')`"", '-Sampler', $StackSampler, '-IntervalMs', $StackIntervalMs)
                        }
                        function Wait-FlyEvent([string]$pattern, [double]$sec) {
                            $sw = [Diagnostics.Stopwatch]::StartNew()
                            while ($sw.Elapsed.TotalSeconds -lt $sec) {
                                $ev = @(Get-BenchEvents $logPos | Where-Object { $_.Text -match $pattern })[0]
                                if ($ev) { return $ev }
                                Wait-Game 0.1
                            }
                            $null
                        }
                        # mod clock (seconds) -> QPC ms, anchored at the answer to "fly" (off by the answer's latency, ~20 ms)
                        function ConvertTo-QpcMs([double]$clock) { if ($null -ne $ackClock) { $ackMs + 1000 * ($clock - $ackClock) } else { Get-QpcMs } }
                        $start = Wait-FlyEvent '^fly=(start|done)' (40 + $settle)
                        if (-not $start -or $start.Text -notmatch '^fly=start') { throw "the flight did not start: $(if ($start) { $start.Text } else { 'no answer' })" }
                        $music = $null
                        if ($musicFile -and $start -and $scn.music.at.PSObject.Properties[$scene.id]) {
                            # from the track's offset for this scene, plus what has passed since the fade-in began
                            $late = [math]::Max(0, ((Get-QpcMs) - (ConvertTo-QpcMs $start.Clock)) / 1000 + 0.15)
                            $from = [double]$scn.music.at.($scene.id) + $late
                            $music = Start-Process $ffplay.Source -PassThru -WindowStyle Hidden -ArgumentList @('-nodisp', '-autoexit', '-loglevel', 'quiet',
                                '-ss', ('{0:F2}' -f $from), '-t', ('{0:F2}' -f [math]::Max(1, $expect - $late)), "`"$musicFile`"")
                        }
                        $done = Wait-FlyEvent '^fly=done' ($expect + 15)
                        $t0 = ConvertTo-QpcMs $start.Clock
                        $t1 = if ($done) { ConvertTo-QpcMs $done.Clock } else { Get-QpcMs }
                        if ($stackProc -and -not $stackProc.WaitForExit(30000)) { $stackProc.Kill(); Warn "$($scene.id): the stack sampler did not finish" }
                        if (-not $done) { Warn "$($scene.id): the flight did not report its end" }
                        elseif ($done.Text -notmatch 'reason=arrived') { Warn "$($scene.id): $($done.Text)" }
                        # events relative to the start of the timed flight: lifted, start, prog, done, cell/level changes
                        $events = @(Get-BenchEvents $logPos | ForEach-Object { '{0:F2} {1}' -f ($_.Clock - $start.Clock), $_.Text })
                        if ($look.Count -ge 2) {
                            $lifted = @(Get-BenchEvents $logPos | Where-Object { $_.Text -match '^fly=lifted' })[0]
                            if ($lifted) { Add-Segment @{ id = "$($scene.id)/look"; type = 'static'; t0 = (ConvertTo-QpcMs $lifted.Clock); t1 = $t0 } }
                        }
                        $clips = @(Get-BenchEvents $logPos | Where-Object { $_.Text -match '^fly=clip' })
                        if ($clips) { Warn "$($scene.id): the camera went through something $($clips.Count) time(s): $(($clips | Select-Object -First 5 | ForEach-Object { $_.Text -replace '^fly=clip ', '' }) -join ' | ')" }
                        $video = $null
                        if ($rec) {
                            if ($flyOpt -match '\bfadeout=([\d.]+)') { Wait-Game ([double]$Matches[1] + 1) }
                            $recStop = Stop-ObsRecord; $recStats1 = Get-ObsStats
                            $file = $recStop.file
                            if ($file -and (Test-Path -LiteralPath $file)) {
                                $ext = [IO.Path]::GetExtension($file)
                                $name = "$($scene.id -replace '[\/:]', '_')$ext"
                                if (Test-Path -LiteralPath (Join-Path (Split-Path $file) $name)) { $name = "$($scene.id -replace '[\/:]', '_')-$(Get-Date -Format HHmmss)$ext" }
                                Rename-Item -LiteralPath $file -NewName $name; $file = Join-Path (Split-Path $file) $name
                            } else { Warn "$($scene.id): OBS reported no recording file" }
                            $skipR = if ($recStats0 -and $recStats1) { $recStats1.renderSkippedFrames - $recStats0.renderSkippedFrames } else { $null }
                            $skipO = if ($recStats0 -and $recStats1) { $recStats1.outputSkippedFrames - $recStats0.outputSkippedFrames } else { $null }
                            if ($skipR -or $skipO) { Warn "$($scene.id): OBS skipped frames while recording (render $skipR, encode $skipO): the video may show stutter the game did not have" }
                            $video = [ordered]@{ file = $(if ($file) { $file.Substring($runDir.Length + 1) }); startQpcMs = $rec.activeQpcMs; startSentQpcMs = $rec.sentQpcMs
                                                 stopQpcMs = $recStop.stopQpcMs; seconds = $recStop.durationSec; obsRenderSkipped = $skipR; obsEncodeSkipped = $skipO }
                            Step "$($scene.id): recorded $($video.file) ($($video.seconds) s)"
                        }
                        $shot = if ($flyOpt -match '\bfadeout=') { $null } else { Save-Shot $scene.id }
                        Add-Segment @{ id = $scene.id; type = 'fly'; t0 = $t0; t1 = $t1; shot = $shot; state = Get-BenchState; events = $events; video = $video }
                    } finally {
                        if ($rec -and -not $recStop) { try { $null = Stop-ObsRecord } catch { Warn "OBS: $_" } }
                        if ($music -and -not $music.HasExited) { try { $music.Kill() } catch {} }
                        Send-Bench 'watch 0' -NoThrow | Out-Null
                        # a flight that fades out and stays keeps hovering in the black (no fall, no splash) until the
                        # next scene's teleport or flight, or the way to the main menu
                        if ($flyOpt -match '\bstay=1\b' -and $flyOpt -match '\bfadeout=') { $script:faded = $true }
                        else { Send-Bench 'flystop' -NoThrow | Out-Null }
                        if ($tcl) { Send-Bench 'obv tcl' -NoThrow | Out-Null }
                        $script:place = $null
                    }
                }
                default { Warn "scene $($scene.id): unknown type $($scene.type)" }
            }
            Test-Pins $scene.id; Test-Talks $scene.id
            [void]$session.completed.Add($scene.id)
        } catch {
            if ("$_" -match '^(GAMEDOWN|DEADLINE|NEEDSUSER)') { throw }      # the pass cannot go on in this game
            # one failed scene must not cost the whole unattended run
            Warn "scene $($scene.id) failed: $_"
            [void]$session.completed.Add($scene.id)
            $script:place = $null
            if (-not (Get-Game)) { throw 'GAMEDOWN: exited: the game is gone' }
            Send-Bench 'freeze 0' -NoThrow | Out-Null; Send-Bench 'watch 0' -NoThrow | Out-Null; Send-Bench 'walkstop' -NoThrow | Out-Null; Send-Bench 'flystop' -NoThrow | Out-Null
        } }
        $session.current = $null
        # -Pin: give the weather back to the climate at the end of the pass, so a later run in this game process (pinned
        # or not) starts as without -Pin; timescale and hour go with the next world
        if ($pinOn -and $state.PSObject.Properties['wxOverride'] -and $state.wxOverride) {
            if ($null -ne (Send-Bench 'obv ReleaseWeatherOverride' -NoThrow)) { $state.wxOverride = $false; Save-SessionState $state }
        }
        if ($null -ne $musicVolume -and $musicVolume -ne '') { Send-Bench "cvar Altar.Audio.MusicVolume $musicVolume" -NoThrow | Out-Null }
        if (-not $SelfTest) {
            # settings can change under us (the upscaler's render scale did): compare with the start of the pass
            foreach ($name in @($effective.Keys | Where-Object { $_ -ne 'window' })) {
                $now = (Send-Bench "get $name" -NoThrow) -replace '^.*=', ''
                if ($now -ne $effective[$name]) { Warn "$name changed during the $kind pass: $($effective[$name]) -> $now" }
            }
        }
        if ($kind -eq 'visual' -and -not $SelfTest -and $state.aiOff) {
            if ($null -ne (Send-Bench 'obv tai' -NoThrow)) { $state.aiOff = $false; Save-SessionState $state }
        }
        $session.endPrivateGB = Rnd ($sampler.PrivateGB((Get-QpcMs))) 2
    }
    catch {
        $msg = "$_"
        $session.error = $msg
        if ($msg -match '^GAMEDOWN: ?(.*)$') { $session.gameDown = $Matches[1] }
        elseif ($msg -match '^DEADLINE') { $session.deadline = $true }
        elseif ($msg -match '^NEEDSUSER: ?(.*)$') { $session.needsUser = $Matches[1] }
        Warn "$tag pass stopped: $msg (line $($_.InvocationInfo.ScriptLineNumber))"
    }
    finally {
        if ($pm) {
            Start-Process $presentMon -WindowStyle Hidden -Wait -ArgumentList '--terminate_existing_session', '--session_name', 'OblvBench'
            if (-not $pm.HasExited -and -not $pm.WaitForExit(10000)) { $pm.Kill() }
        }
        if ($sampler) { $sampler.Stop() }
    }

    # ---- analysis
    if ($sampler -and (Test-Path $framesCsv) -and $session.segments.Count) {
        $log = [OblvBench.FrameLog]::Load($framesCsv)
        $sampler.WriteTimeline((Join-Path $runDir "$tag-threads.csv"), $session.segments[0].t0)
        $results = foreach ($seg in $session.segments) {
            $f = $log.Stats($seg.t0, $seg.t1)
            $wall = $seg.t1 - $seg.t0
            $use = $sampler.Query($seg.t0, $seg.t1)
            $threads = [ordered]@{}
            foreach ($n in 'GameThread', 'RenderThread', 'RHIThread', 'OblivionThread') {
                $ms = ($use | Where-Object Name -eq $n | Measure-Object CpuMs -Sum).Sum
                $threads[$n] = [ordered]@{ pct = Rnd (100 * $ms / $wall); msPerFrame = if ($f.Frames) { Rnd ($ms / $f.Frames) 2 } else { 0 } }
            }
            $workers = ($use | Where-Object { $_.Name -match 'Worker' } | Measure-Object CpuMs -Sum).Sum
            $d3d = ($use | Where-Object { $_.Name -eq 'D3D Background Thread' } | Measure-Object CpuMs -Sum).Sum
            # FSR 3 in the game's own DLLs: "FSR Presenter/Interpolation"; the driver's FG through AMD's SDK 2.3.0 loader
            # (FG 4.0.2): "AMD ML Presenter/Interpolation Thread" (2026-10-25 pmcheck: frameGen read False with FG on)
            $fgMs = ($use | Where-Object { $_.Name -match '(FSR|AMD ML) (Presenter|Interpolation)' } | Measure-Object CpuMs -Sum).Sum
            $total = ($use | Measure-Object CpuMs -Sum).Sum
            $r = [ordered]@{
                id = $seg.id; type = $seg.type; seconds = Rnd ($wall / 1000); t0Ms = Rnd $seg.t0 1; t1Ms = Rnd $seg.t1 1
                frames = $f.Frames; fps = Rnd $f.AvgFps; low1 = Rnd $f.Low1Fps; low01 = Rnd $f.Low01Fps
                avgMs = Rnd $f.AvgMs 2; p50Ms = Rnd $f.P50Ms 2; p95Ms = Rnd $f.P95Ms 2; p99Ms = Rnd $f.P99Ms 2; worstMs = Rnd $f.WorstMs
                hitches = $f.Hitches; hitchMs = Rnd $f.HitchMs; over50 = $f.Over50; over100 = $f.Over100; over250 = $f.Over250
                longestGapMs = Rnd $f.LongestGapMs
                cpuBusyMs = Rnd $f.CpuBusyMs 2
                gpuBusyMs = if ($session.frameCapture -eq 'cpu-only') { $null } else { Rnd $f.GpuBusyMs 2 }       # not measured in a CPU-side capture
                gpuBoundPct = if ($session.frameCapture -eq 'cpu-only') { $null } else { Rnd $f.GpuBoundPct }
                frameGen = ($wall -gt 0 -and $fgMs / $wall -gt 0.01)          # FSR frame generation ran: fps counts generated frames too
                # on-screen runs (-Visible/-Watch): share of the measured time the game was not the foreground window (the
                # game turns frame generation off and lowers its sound then); hidden runs: $null (never in front, by design)
                notForegroundPct = if ($Visible -and ($fgs = $sampler.ForegroundShare($seg.t0, $seg.t1)) -ge 0) { Rnd (100 * (1 - $fgs)) 1 } else { $null }
                threads = $threads; workerCores = Rnd ($workers / $wall) 2; driverCompileCores = Rnd ($d3d / $wall) 2; processCores = Rnd ($total / $wall) 2
                ioReadMBps = Rnd ($sampler.IoReadMB($seg.t0, $seg.t1) / ($wall / 1000)); privateGB = Rnd ($sampler.PrivateGB($seg.t1)) 2
                # memory state (2026-10-06): door and hitch numbers follow the PC's memory pressure (hard paging)
                wsGB = Rnd ($sampler.WorkingSetGB($seg.t1)) 2; faultsPerSec = Rnd ($sampler.FaultsPerSec($seg.t0, $seg.t1))
                hardFaultsPerSec = Rnd ($sampler.HardFaultsPerSec($seg.t0, $seg.t1)) 1
                availMinMB = Rnd ($sampler.AvailMinMB($seg.t0, $seg.t1)); pageInsPerSec = Rnd ($sampler.PageInsPerSec($seg.t0, $seg.t1))
                pageInsMaxPerSec = Rnd ($sampler.PageInsMaxPerSec($seg.t0, $seg.t1))
                # other processes' CPU (2026-10-26): a parallel build on this PC made bursts of 25-80 ms game frames
                otherCores = Rnd ($sampler.OtherCores($seg.t0, $seg.t1)) 2; otherCoresMax = Rnd ($sampler.OtherCoresMax($seg.t0, $seg.t1)) 2
                shot = $seg.shot
            }
            # CPU ms per frame of every thread name that used at least 0.05 ms per frame (RHISubmissionThread, workers, pools, ...)
            $all = [ordered]@{}
            if ($f.Frames) { foreach ($u in $use) { $v = $u.CpuMs / $f.Frames; if ($v -ge 0.05) { $all[$u.Name] = Rnd $v 2 } } }
            $r.allThreads = $all
            if (-not $f.Frames) { Warn "$($seg.id): no frames in the measured time (game frozen?): the numbers of this scene are void" }
            if ($r.otherCoresMax -ge $OtherCoresWarn) { Warn "$($seg.id): other processes used up to $($r.otherCoresMax) cores (mean $($r.otherCores)) during the measured time (a build or another busy program on this PC competes with the game: longer frames, jitter bursts; perf-threads.csv column OtherCores says when): not comparable" }
            if ($null -ne $r.notForegroundPct -and $r.notForegroundPct -gt 2) { Warn "$($seg.id): the game was not the foreground window for $($r.notForegroundPct)% of the measured time (another window was in front; the game then turns frame generation off): not comparable" }
            if ($seg.type -eq 'door') {
                $after = $log.Stats($seg.tA, $seg.t1)
                $r.arrived = $seg.arrived; if ($seg.activationRetries) { $r.activationRetries = $seg.activationRetries }; $r.arriveSec = Rnd (($seg.tA - $seg.t0) / 1000) 2; $r.tAMs = Rnd $seg.tA 1
                $r.afterFps = Rnd $after.AvgFps; $r.afterLow1 = Rnd $after.Low1Fps; $r.afterHitches = $after.Hitches; $r.afterHitchMs = Rnd $after.HitchMs; $r.afterWorstMs = Rnd $after.WorstMs
            }
            if ($seg.type -ne 'static') {
                $plot = Join-Path $plotDir (($seg.id -replace '[\/]', '-') + '.png')
                $marks = if ($seg.type -eq 'door') { , [double[]]@(($seg.tA - $seg.t0)) } else { , [double[]]@() }
                $labels = if ($seg.type -eq 'door') { , [string[]]@('arrived') } else { , [string[]]@() }
                [OblvBench.Plot]::FrameTimes($log.Series($seg.t0, $seg.t1), $plot, "$($seg.id): frame time", $marks, $labels)
                $r.plot = 'plots/' + (Split-Path $plot -Leaf)
                # which thread was busy when: one row per 250 ms sample
                $sampler.WriteWindow(($plot -replace '\.png$', '-threads.csv'), $seg.t0, $seg.t1, 10)
            }
            if ($seg.events) { $r.events = @($seg.events) }
            if ($seg.cond) { $r.cond = $seg.cond }
            if ($seg.aiHeld) { $r.aiHeld = $true }
            if ($seg.video) { $r.video = $seg.video }
            if ($seg.state) { $r.hour = $seg.state.hour; $r.level = $seg.state.level; $r.sub = $seg.state.sub; $r.pos = "$($seg.state.x),$($seg.state.y),$($seg.state.z)"
                if ($null -ne $seg.state.menu) { $r.menu = $seg.state.menu }; if ($null -ne $seg.state.frozen) { $r.frozen = $seg.state.frozen } }
            $r
        }
        $session.scenes = @($results)
        $fgExpected = (Opt $prof 'upscaler' 'fsr') -eq 'fsr' -and [int](Opt $prof 'fg' 0)
        $fgSeen = @($results | Where-Object { $_.frameGen }).Count
        if ($fgExpected -and $fgSeen -lt @($results).Count) { Warn "profile $Profile has frame generation on, but it ran in only $fgSeen of $(@($results).Count) scenes" }
        if (-not $fgExpected -and $fgSeen -and -not (Test-OwnProfile $prof)) { Warn "frame generation ran in $fgSeen scene(s) although profile $Profile has it off: those fps include generated frames" }
    }
    elseif ($session.segments.Count) { Warn "no frame log ($tag-frames.csv): $($session.segments.Count) measured segment(s) could not be analysed" }
    $session.Remove('segments')
    if ($sampler) { $sampler.Dispose() }
    $session
}

<# A pass, restarting the game after a crash or hang and carrying on with the scenes that are left
   (a scene that took the game down twice is skipped). The parts are merged into one session. #>
function Invoke-Pass([string]$kind, [string]$runDir, [object[]]$sceneList, [string]$startReason) {
    $parts = @(); $remaining = @($sceneList); $downs = @{}; $restarts = @()
    for ($part = 1; ; $part++) {
        Set-BenchStatus @{ phase = "$kind pass"; part = $part }
        $gameStart = if (Get-Game) { (Get-Game).StartTime } else { Get-Date }
        $s = Invoke-Session $kind $runDir $remaining $part $startReason
        $parts += , $s
        if (-not $s.gameDown) { break }
        $n = $script:restartCount + 1
        $what = Invoke-Recovery $runDir $n $s.gameDown $gameStart
        $script:restartCount = $n
        $restarts += [ordered]@{ pass = $kind; part = $part; scene = $s.current; problem = $what; evidence = "crash-$n" }
        Set-BenchStatus @{ restarts = $script:restartCount }
        Step "the game went down$(if ($s.current) { " during $($s.current)" }): $what"
        if ($s.current) { $downs[$s.current] = 1 + [int]$downs[$s.current] }
        $done = @($s.completed)
        $skipped = @($remaining | Where-Object { $done -notcontains $_.id -and [int]$downs[$_.id] -ge 2 } | ForEach-Object { $_.id })
        foreach ($id in $skipped) { [void]$s.warnings.Add("scene $id skipped: it took the game down twice") }
        $remaining = @($remaining | Where-Object { $done -notcontains $_.id -and $skipped -notcontains $_.id })
        if (-not $remaining.Count) { break }
        if ($part -gt $MaxRestarts) { [void]$s.warnings.Add("gave up after $MaxRestarts restart(s): $(@($remaining | ForEach-Object id) -join ', ') not run"); break }
        $startReason = "restart after: $what"
    }
    # merge the parts
    $m = $parts[0]
    for ($i = 1; $i -lt $parts.Count; $i++) {
        $p = $parts[$i]
        $m.scenes = @($m.scenes) + @($p.scenes) | Where-Object { $_ }
        foreach ($v in $p.visual) { [void]$m.visual.Add($v) }
        foreach ($w in $p.warnings) { [void]$m.warnings.Add($w) }
        foreach ($c in $p.completed) { [void]$m.completed.Add($c) }
        foreach ($k in 'error', 'gameDown', 'deadline', 'needsUser', 'endPrivateGB') { if ($p.Contains($k)) { $m[$k] = $p[$k] } elseif ($m.Contains($k) -and $k -ne 'endPrivateGB') { $m.Remove($k) } }
        if (-not $m.settings -and $p.settings) { $m.settings = $p.settings }
    }
    $m.parts = $parts.Count
    $m.restarts = @($restarts)
    $m.Remove('current'); $m.Remove('part')
    $m
}

# ---------------------------------------------------------------- report

function Get-Limiter($s) {
    if ($s.gpuBoundPct -ge 50) { return 'GPU' }
    $best = 'GameThread'
    foreach ($n in 'RenderThread', 'RHIThread') { if ($s.threads[$n].pct -gt $s.threads[$best].pct) { $best = $n } }
    '{0} {1:N0}%' -f ($best -replace 'Thread$', ''), $s.threads[$best].pct
}

function Write-Report($result, [string]$runDir) {
    $md = New-Object System.Collections.Generic.List[string]
    $md.Add("# Benchmark $($result.label)"); $md.Add('')
    $md.Add("$($result.time) | scenario ``$($result.scenario)`` | profile ``$($result.profile.name)`` | start ``$(if ($result.start) { $result.start } else { 'continue' })`` | game build $($result.game.build) | $($result.system.cpu) | $($result.system.gpu) (driver $($result.system.gpuDriver))"); $md.Add('')
    $talks = ($result.sessions | ForEach-Object { [int]$_.talksRefused } | Measure-Object -Sum).Sum
    $targets = ($result.sessions | ForEach-Object { [int]$_.targetsRefused } | Measure-Object -Sum).Sum
    $md.Add("Profile: $($result.profile.description). Camera: $(if ($result.spectate) { "spectator (cinematic camera, no HUD; NPC AI turned away from the player: $targets package target(s), $talks conversation start(s))" } else { "the game's own" }). Game process: $($result.mode)."); $md.Add('')
    $caps = @($result.sessions | ForEach-Object { $_.frameCap } | Where-Object { $_ } | Select-Object -Unique)
    if ($caps.Count) { $md.Add("Frame rate capped at $($caps -join ' / ') fps during the run (t.MaxFPS; $(if ($result.profile.own) { "the player's own settings" } else { 'set by the profile' })): fps cannot go above it."); $md.Add('') }
    if (-not @($result.sessions | Where-Object { $_.startedGame }).Count) { $md.Add('The game process was reused from an earlier run (with a fresh world per pass): good for iterating, but fps can drift within a long session, so confirm small differences with -Session fresh.'); $md.Add('') }
    if ($result.underTest.Count) { $md.Add('Under test: ' + (($result.underTest.GetEnumerator() | ForEach-Object { "``$($_.Key)=$($_.Value)``" }) -join ', ')); $md.Add('') }
    $perf = $result.sessions | Where-Object { $_.kind -eq 'perf' }
    if ($perf -and $perf.scenes) {
        $md.Add('## Scenes'); $md.Add('')
        $md.Add('static = standing still; fly and walk = moving through the world (streaming shows up as hitches). ms columns are CPU time per frame on that thread. FG = frame generation ran (fps include generated frames).'); $md.Add('')
        $md.Add('| Scene | type | fps | 1% low | worst ms | hitches | hitch ms | Game ms | Render ms | RHI ms | GPU ms | limited by | IO MB/s | plot |')
        $md.Add('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
        foreach ($s in $perf.scenes | Where-Object { $_.type -ne 'door' }) {
            $plot = if ($s.plot) { "[plot]($($s.plot))" } else { '' }
            $fg = if ($s.frameGen) { ' (FG)' } else { '' }
            $held = if ($s.aiHeld) { ' (AI held: not a measurement)' } else { '' }
            $md.Add("| $($s.id) | $($s.type)$held | $($s.fps)$fg | $($s.low1) | $($s.worstMs) | $($s.hitches) | $($s.hitchMs) | $($s.threads.GameThread.msPerFrame) | $($s.threads.RenderThread.msPerFrame) | $($s.threads.RHIThread.msPerFrame) | $($s.gpuBusyMs) | $(Get-Limiter $s) | $($s.ioReadMBps) | $plot |")
        }
        $doors = @($perf.scenes | Where-Object { $_.type -eq 'door' })
        if ($doors.Count) {
            $md.Add(''); $md.Add('## Door transitions'); $md.Add('')
            $md.Add('Stall = longest time without a new frame. "After" columns cover the seconds after arriving.'); $md.Add('')
            $md.Add('| Transition | arrive s | stall ms | frames >100 ms | after: fps | after: 1% low | after: hitches | after: hitch ms | after: worst ms | plot |')
            $md.Add('|---|---|---|---|---|---|---|---|---|---|')
            foreach ($s in $doors) {
                $md.Add("| $($s.id) | $($s.arriveSec) | $($s.longestGapMs) | $($s.over100) | $($s.afterFps) | $($s.afterLow1) | $($s.afterHitches) | $($s.afterHitchMs) | $($s.afterWorstMs) | [plot]($($s.plot)) |")
            }
        }
        # the conditions each scene had (2026-10-20): what was set, and what the game reported right after and at the end
        $conds = @($perf.scenes | Where-Object { $_.cond })
        if ($conds.Count) {
            $md.Add(''); $md.Add('## Conditions (pinned: -Pin)'); $md.Add('')
            $md.Add('| Scene | weather set | hour set | timescale set | right after | at the end |'); $md.Add('|---|---|---|---|---|---|')
            foreach ($s in $conds) { $md.Add("| $($s.id) | $($s.cond.weather) | $($s.cond.hour) | $($s.cond.timescale) | $($s.cond.atSet) | $($s.cond.atEnd) |") }
        }
        $md.Add(''); $md.Add("Shader warm-up wait: $($perf.quietSec) s$(if ($perf.psoWaitSec -gt 0) { ", then $($perf.psoWaitSec) s for the game's PSO precompile" } elseif ($perf.psoWaitSec -lt 0) { ' (PSO precompile state not readable)' }). Private memory at the end: $($perf.endPrivateGB) GB.")
        # memory pressure: hard paging stalls the game (idle-process frames), so door and hitch numbers depend on it
        $mem = @($perf.scenes | Where-Object { $null -ne $_.availMinMB -and $_.availMinMB -ge 0 })
        if ($mem.Count) {
            $paged = @($mem | Where-Object { $_.pageInsMaxPerSec -ge 1000 } | ForEach-Object { "$($_.id) (max $($_.pageInsMaxPerSec)/s, available min $($_.availMinMB) MB)" })
            $av = @($mem | ForEach-Object { [double]$_.availMinMB }); $pin = @($mem | ForEach-Object { [double]$_.pageInsMaxPerSec }); $wsv = @($mem | ForEach-Object { [double]$_.wsGB })
            $hf = @($mem | ForEach-Object { [double]$_.hardFaultsPerSec })
            $md.Add("System memory during the scenes: available min $(($av | Measure-Object -Minimum).Minimum) MB; the game's working set $(($wsv | Measure-Object -Minimum).Minimum)-$(($wsv | Measure-Object -Maximum).Maximum) GB, its own hard page faults up to $(($hf | Measure-Object -Maximum).Maximum)/s (scene average); system-wide page-ins up to $(($pin | Measure-Object -Maximum).Maximum) pages/s (these include the file cache reading game data from disk, not only paging)." + $(if ($paged.Count) { " Scenes with >= 1000 system page-ins/s: $($paged -join ', ')." } else { '' }))
        }
        # other processes' CPU per scene (2026-10-26): absent in older results
        $oc = @($perf.scenes | Where-Object { $_ -and $null -ne $_.otherCoresMax })
        if ($oc.Count) { $md.Add(''); $md.Add("Other processes' CPU during the scenes (cores; a build or scan competes with the game): $(($oc | ForEach-Object { "$($_.id) mean $($_.otherCores) max $($_.otherCoresMax)" }) -join ', ').") }
    }
    $vis = $result.sessions | Where-Object { $_.kind -eq 'visual' }
    if ($vis -and $vis.visual.Count) {
        $md.Add(''); $md.Add('## Reference images'); $md.Add('')
        $md.Add('| Scene | image | mean luma | temporal noise |'); $md.Add('|---|---|---|---|')
        foreach ($v in $vis.visual) { $md.Add("| $($v.id) | $($v.image) | $($v.meanLuma) | $($v.temporalNoise) |") }
    }
    $rs = @($result.sessions | ForEach-Object { $_.restarts } | Where-Object { $_ })
    if ($rs.Count) {
        $md.Add(''); $md.Add('## Game restarts'); $md.Add('')
        foreach ($r in $rs) {
            $during = if ($r.scene) { ', during `' + $r.scene + '`' } else { '' }
            $md.Add("- $($r.pass) pass$during`: $($r.problem) (evidence in ``$($r.evidence)\``)")
        }
    }
    $warn = @($result.sessions | ForEach-Object { $_.warnings } | Where-Object { $_ })
    if ($warn.Count) { $md.Add(''); $md.Add('## Warnings'); $warn | ForEach-Object { $md.Add("- $_") } }
    Set-Content (Join-Path $runDir 'report.md') $md -Encoding utf8
    $md
}

# ---------------------------------------------------------------- main

function Get-SceneTags($s) { @($s.type) + @(Opt $s 'tags' @()) }

function Get-SceneSeconds($s) {
    $settle = [double](Opt $s 'settle' $scn.defaults.settle)
    switch ($s.type) {
        'static' { $settle + [double](Opt $s 'measure' $scn.defaults.measure) + 6 }
        'door'   {
            $h = [double](Opt $s 'hold' $scn.defaults.hold)
            $legs = if ($s.PSObject.Properties['legs']) { @($s.legs) } else { @(1, 2) }
            $t = $settle + 6
            foreach ($l in $legs) { $t += [double](Opt $l 'hold' $h) + 8 + $(if (Opt $l 'at' $null) { $settle + 4 } else { 0 }) }
            $t
        }
        'walk'   { $settle + [double](Opt $s 'measure' $scn.defaults.measure) + 8 }
        'fly'    {
            $len = 0; $p = @($s.path)
            for ($i = 1; $i -lt $p.Count; $i++) { $len += [math]::Sqrt([math]::Pow($p[$i][0] - $p[$i - 1][0], 2) + [math]::Pow($p[$i][1] - $p[$i - 1][1], 2) + [math]::Pow($p[$i][2] - $p[$i - 1][2], 2)) }
            $settle + $len / [double]$s.speed + 18      # + teleport, lift to the first waypoint, easing in and out
        }
        default  { 30 }
    }
}

$sceneList = @($scn.scenes | Where-Object {
    $s = $_; $sceneTags = Get-SceneTags $s
    (-not $Scenes -or @($Scenes | Where-Object { $s.id -like $_ }).Count) -and (-not $Tags -or @($Tags | Where-Object { $sceneTags -contains $_ }).Count)
})

if ($List) {
    $sceneList | ForEach-Object {
        [pscustomobject]@{ Scene = $_.id; Type = $_.type; Tags = (@(Opt $_ 'tags' @()) -join ','); Where = $(if ($_.PSObject.Properties['at']) { $_.at } else { if ($_.PSObject.Properties['cell']) { $_.cell } else { $_.world } })
            Hour = (Opt $_ 'hour' $scn.defaults.hour); Weather = (Opt $_ 'weather' ''); Visual = $(if (Opt $_ 'visual' $false) { 'yes' } else { '' }); Seconds = [int](Get-SceneSeconds $_) }
    } | Format-Table -AutoSize | Out-String -Width 200
    $total = ($sceneList | ForEach-Object { Get-SceneSeconds $_ } | Measure-Object -Sum).Sum
    "{0} scene(s) in scenario '{1}', about {2:N0} min of scenes plus about 1 min per game start (15 s from a parked game)." -f $sceneList.Count, $scn.name, ($total / 60)
    'Tags: ' + ((@($scn.scenes | ForEach-Object { Get-SceneTags $_ }) | Group-Object | Sort-Object Name | ForEach-Object { "$($_.Name) ($($_.Count))" }) -join ', ')
    ''
    'Scenarios (-Scenario, bench\scenarios; -List -Scenario <name> shows its scenes):'
    foreach ($f in Get-ChildItem (Join-Path $script:BenchRoot 'bench\scenarios') -Filter *.json | Sort-Object Name) {
        $d = try { (Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json).description } catch { '' }
        $first = if ($d) { ($d -split '(?<=[.!?])\s', 2)[0] } else { '' }
        if ($first.Length -gt 150) { $first = $first.Substring(0, 147) + '...' }
        "  {0,-18} {1}{2}" -f $f.BaseName, $first, $(if ($f.BaseName -eq $scn.name) { '  [listed above]' } else { '' })
    }
    ''
    'Profiles (-Profile, bench\profiles.json):'
    foreach ($p in $profiles.PSObject.Properties | Where-Object { $_.Name -notlike '_*' }) { "  {0,-12} {1}{2}" -f $p.Name, $p.Value.description, $(if ($p.Name -eq $Profile) { '  [default here]' } else { '' }) }
    return
}

# ---- -Stop (2026-10-20): a graceful stop instead of Stop-Process (which once left a half-launched game on the PC
# user's screen). The runner checks the request before every scene and before every run.
function Get-StopRequest {
    try {
        if (-not (Test-Path $script:BenchStopFile)) { return $null }
        $r = Get-Content $script:BenchStopFile -Raw | ConvertFrom-Json
        if ([int]$r.pid -ne $PID -or ((Get-Date) - [datetime]$r.time).TotalHours -gt 12) { return $null }
        # only for this process, not for a later one that got the same pid from Windows (2026-10-26: a request from
        # 08:51 stopped another runner at 18:24): a request made before this process started is stale, and since
        # 2026-10-26 the request names the target's start time
        $myStart = [Diagnostics.Process]::GetCurrentProcess().StartTime
        if ([datetime]$r.time -lt $myStart.AddSeconds(-1)) { return $null }
        if ($r.PSObject.Properties['start'] -and $r.start -and [math]::Abs(([datetime]$r.start - $myStart).TotalSeconds) -gt 1) { return $null }
        $r
    } catch { $null }
}
if ($Stop) {
    $target = $StopPid
    if (-not $target -and (Test-Path $script:BenchLockFile) -and ((Get-Content $script:BenchLockFile -Raw) -match 'pid=(\d+)')) { $target = [int]$Matches[1] }
    if (-not $target -and (Test-Path $script:BenchStatusFile)) { try { $st = Get-Content $script:BenchStatusFile -Raw | ConvertFrom-Json; if ($st.state -eq 'running') { $target = [int]$st.pid } } catch {} }
    if (-not $target -or -not (Get-Process -Id $target -ErrorAction SilentlyContinue)) { Step 'no running benchmark to stop'; exit 0 }
    $targetStart = try { (Get-Process -Id $target -ErrorAction Stop).StartTime.ToString('o') } catch { $null }
    @{ pid = $target; start = $targetStart; time = (Get-Date).ToString('o'); reason = $StopReason; by = $PID } | ConvertTo-Json | Set-Content $script:BenchStopFile -Encoding utf8
    Step "stop requested for runner pid $target$(if ($StopReason) { " ($StopReason)" }): it ends after its current scene and parks or closes the game as its -After says; later runs in that process do not start"
    exit 0
}

if ($Close -or $Park) {
    $busy = Get-GameBusyReason
    if ($busy -and -not $Force) { throw "The game is in use ($busy). Not touching it; use -Force to override." }
    Stop-IdleWatcher
    if ($Park) {
        $g = Get-Game
        if (-not $g) { Step 'no game running'; return }
        if (-not (Get-SessionState $g).fingerprint -and -not $Force) { throw 'The running game was not started by the benchmark; not parking it (-Force to override).' }
        Watch-Game $g
        Set-GameParked $g
        Start-IdleWatcher
        Step 'parked'
        return
    }
    if (Get-Game) { Step 'closing the game'; Stop-BenchGame }
    if (Get-Game) { throw 'the game did not close' }
    Restore-Config; Restore-Saves; Step (Disable-BenchUE4SS)
    Set-BenchStatus @{ state = 'closed'; game = 'closed'; message = 'game closed, saves and config restored' }
    Step 'done'
    return
}

if (-not $sceneList.Count) { throw "no scenes selected in scenario '$($scn.name)' (see -List)" }
if ($SelfTest) { $Session = 'reuse'; $Pass = 'perf'; $Label = 'selftest' }
$passes = @(); if ($Pass -ne 'visual') { $passes += 'perf' }; if ($Pass -ne 'perf' -and ($sceneList | Where-Object { Opt $_ 'visual' $false })) { $passes += 'visual' }
$exitCode = 0

for ($run = 1; $run -le $Runs; $run++) {
    $runLabel = if ($Runs -gt 1) { "$Label-r$run" } else { $Label }
    if (-not $SelfTest -and ($stopReq = Get-StopRequest)) {
        Write-Host ("BENCH-RESULT stopped {0} (stop requested {1}{2}: not started)" -f $runLabel, ([datetime]$stopReq.time).ToString('HH:mm:ss'), $(if ($stopReq.reason) { ", $($stopReq.reason)" }))
        $exitCode = 3; break
    }
    $busy = if ($SelfTest) { $null } else { Get-GameBusyReason }
    if ($busy -and -not $Force) { throw "The game is in use ($busy). Not starting; use -Force to override." }
    $runDir = Join-Path $root ('captures\bench\{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $runLabel)
    New-Item -ItemType Directory -Force $runDir | Out-Null
    $script:configBackup = Join-Path $runDir 'config-before'
    $script:restartCount = 0
    $estimate = ($sceneList | ForEach-Object { Get-SceneSeconds $_ } | Measure-Object -Sum).Sum
    $limit = if ($MaxMinutes -gt 0) { $MaxMinutes } else { [math]::Ceiling(1.6 * $estimate * $passes.Count / 60 + 7 * $passes.Count + 3 * $MaxRestarts + 5) }
    $script:BenchStatus = [ordered]@{}
    Set-BenchStatus @{ state = 'running'; label = $runLabel; runDir = $runDir; pid = $PID; started = (Get-Date).ToString('s')
                       deadline = (Get-Date).AddMinutes($limit).ToString('s'); scenario = $scn.name; profile = $Profile; passes = ($passes -join ','); restarts = 0 }
    $result = $null; $outcome = 'failed'; $gameLeft = 'unknown'
    if (-not $SelfTest) { Lock-Game "benchmark $runLabel"; Stop-IdleWatcher }
    # dead-man switch: if this process dies holding the game (killed mid-launch or mid-scene), tools\Runner-Guard.ps1
    # keeps its game hidden and muted and closes it cleanly
    if (-not $SelfTest -and -not $Visible) { Start-RunnerGuard -Audible:$Audible }
    # on-screen runs that close their game at the end (the kit: -Watch -Session fresh): a guard that, if this runner is
    # stopped (Ctrl+C, console window closed), closes its game and restores config, saves and UE4SS (nothing hidden)
    if (-not $SelfTest -and $Visible -and ($Session -eq 'fresh' -or $After -eq 'close')) { Start-RunnerGuard -Audible:$Audible -Visible }
    if (-not $SelfTest -and -not $Visible) { Start-Unobtrusive -Audible:$Audible -Capturable:$Record }   # also hides (and mutes) a game parked by an earlier run
    try {
        $script:BenchDeadline = (Get-Date).AddMinutes($limit)
        $startReason = $null
        if ($Record) { Step 'starting OBS in the tray for recording'; Start-Obs }
        if (-not $SelfTest) {
            & (Join-Path $PSScriptRoot 'Install-UE4SS.ps1') -ModsOnly -Quiet | Out-Null
            Step ("{0} scene(s), about {1:N0} min of scenes, profile {2}, time limit {3:N0} min: {4}" -f $sceneList.Count, ($estimate / 60), $Profile, $limit, (($sceneList | ForEach-Object id) -join ', '))
            $game = Get-Game
            if ($game) { Watch-Game $game }
            $startReason = Get-RestartReason $game
            if ($startReason) { if ($game) { Step "not reusing the running game: $startReason" } }
            else {
                Step "reusing the running game (started $([math]::Round(((Get-Date) - $game.StartTime).TotalMinutes)) min ago)"
                # -FixIni (and -IniCvars, -Ini) are applied only when the runner starts a game (Set-RunConfig in
                # Start-BenchGame). A reused game (-Session reuse) keeps the installed ini: say so when that ini differs
                # (2026-10-26, worker brumaburst; the behaviour is unchanged).
                $script:ReuseIgnored = Get-ReuseIgnoredWarning
                if ($script:ReuseIgnored) { Write-Warning $script:ReuseIgnored }
            }
            if (-not $game) { Restore-Config }            # left by an earlier session
            Protect-Saves
        }

        $gpu = Get-CimInstance Win32_VideoController | Sort-Object AdapterRAM -Descending | Select-Object -First 1
        $manifest = Join-Path (Split-Path (Split-Path $script:GameDir)) "appmanifest_$($script:SteamAppId).acf"
        $build = if ((Test-Path $manifest) -and ((Get-Content $manifest -Raw) -match '"buildid"\s+"(\d+)"')) { "$($Matches[1])" } else { '?' }
        $fixDll = Join-Path $script:GameWin64 'version.dll'
        $underTest = [ordered]@{}
        foreach ($k in $Cvars.Keys) { $underTest[$k] = $Cvars[$k] }
        foreach ($k in $IniCvars.Keys) { $underTest["ini:$k"] = $IniCvars[$k] }
        foreach ($k in $FixIni.Keys) { $underTest["fix:$k"] = $FixIni[$k] }
        foreach ($target in $Ini.Keys) { foreach ($k in $Ini[$target].Keys) { $underTest["$(($target -split '\|')[0]):$k"] = $Ini[$target][$k] } }
        $profRecord = [ordered]@{ name = $Profile; description = (Opt $prof 'description' ''); res = (Get-ProfileRes $prof) }
        foreach ($p in $prof.PSObject.Properties) { if ($p.Name -notin 'description', 'res') { $profRecord[$p.Name] = $p.Value } }
        $result = [ordered]@{
            label = $runLabel; time = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'); scenario = $scn.name
            mode = $Session; start = $Start; profile = $profRecord; spectate = (-not $NoSpectate)
            stackSampled = [bool]$Stacks
            underTest = $underTest
            game = [ordered]@{ build = $build }
            perfFix = [ordered]@{
                installed = (Test-Path $fixDll)
                dllTime = if (Test-Path $fixDll) { (Get-Item $fixDll).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') } else { $null }
                ini = @()   # filled in after the passes: the previous run's -FixIni may still be in place here (its game parked)
            }
            system = [ordered]@{ cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim(); gpu = $gpu.Name; gpuDriver = $gpu.DriverVersion }
            sessions = @()
        }
        if ($pinOn) { $result.pinned = $true }        # -Pin (else absent, as before)
        $sessions = @()
        foreach ($kind in $passes) {
            $reason = if ($SelfTest) { $null } elseif ($kind -eq $passes[0]) { $startReason } elseif ($Session -eq 'fresh') { 'a fresh session for each pass (-Session fresh)' } elseif (-not (Get-Game)) { 'no game running' } elseif ((Test-FgMenuRisk) -and -not $KeepWorld) { 'a fresh world at a frame-generation profile (the main-menu return can hang there): restarting the game' } else { $null }
            $s = Invoke-Pass $kind $runDir $sceneList $reason
            $sessions += , $s
            if ($s.deadline -or $s.needsUser) { break }
        }
        $result.sessions = $sessions
        $result.restarts = $script:restartCount
        # The ini as this run's games had it: Start-BenchGame has put this run's -FixIni in place by now (Restore-Config
        # runs only after the result is written). .NET read: strings from Get-Content carry provider properties that
        # ConvertTo-Json would serialise.
        if (Test-Path -LiteralPath $fixIniPath) { $result.perfFix.ini = @(Read-SharedLines $fixIniPath | Where-Object { $_ -match '^\s*\w+\s*=' }) }
        # -FixIni / -IniCvars / -Ini asked for but not in effect in a reused game (Get-ReuseIgnoredWarning)
        if ($script:ReuseIgnored -and $sessions.Count) { [void]$sessions[0].warnings.Add($script:ReuseIgnored) }
        # the game's command-line arguments as the game process has them (-FixedSeed since 2026-10-20)
        if (-not $SelfTest -and ($gNow = Get-Game)) {
            $cl = try { "$((Get-CimInstance Win32_Process -Filter "ProcessId=$($gNow.Id)").CommandLine)" } catch { '' }
            $result.fixedSeed = $cl -match '(^|\s)-FixedSeed(\s|$)'
            if ($CpuLimit -gt 0) { $result.cpuLimit = [ordered]@{ logical = $CpuLimit; launch = "$script:CpuLimitReport"; minSpecBoxesAnswered = [OblvBench.GameWatch]::AcceptedBoxes; minSpecBoxLog = [OblvBench.GameWatch]::AcceptLog } }
            if ($script:GameLaunchArgs -contains '-FixedSeed' -and -not $result.fixedSeed -and $sessions.Count) { [void]$sessions[0].warnings.Add("-FixedSeed is not on the game's command line ($cl): its random generators were seeded from the clock") }
        }
        $result | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $runDir 'result.json') -Encoding utf8
        Write-Report $result $runDir | Out-Host
        # ok: every pass ran to its end and no scene failed; partial: some results; failed: none
        $measured = ($sessions | ForEach-Object { @($_.scenes | Where-Object { $_ }).Count + @($_.visual | Where-Object { $_ }).Count } | Measure-Object -Sum).Sum
        $failedScenes = @($sessions | ForEach-Object { $_.warnings } | Where-Object { $_ -match '^(scene \S+ (failed|skipped)|gave up)|: no frames in the measured time' }).Count
        $stopped = @($sessions | Where-Object { $_.error }).Count
        $outcome = if (-not $measured) { 'failed' } elseif ($failedScenes -or $stopped -or $sessions.Count -lt $passes.Count) { 'partial' } else { 'ok' }
        # a restart inside the run (2026-10-20): the scenes after it ran in a new game process (cold caches, shader and
        # PSO state, another world): such a run is not comparable with runs that had none
        if ($script:restartCount -gt 0) {
            $result.comparable = $false
            $msg = "the game was restarted $($script:restartCount) time(s) during this run: NOT COMPARABLE with runs without a restart (the scenes after it ran in a new game process)"
            if ($sessions.Count) { [void]$sessions[0].warnings.Add($msg) }
            $result | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $runDir 'result.json') -Encoding utf8
            Write-Report $result $runDir | Out-Null
            Write-Warning $msg
        }
        Step "results: $runDir"
    }
    catch {
        Write-Warning "run failed: $_ (line $($_.InvocationInfo.ScriptLineNumber))"
        Set-BenchStatus @{ error = "$_" }
        $outcome = 'failed'
    }
    finally {
        $script:BenchDeadline = $null
        try {
            if (-not $SelfTest) {
                Update-SaveGuard
                # what to do with the game: park it for the next run, close it, or leave it in the world
                $g = Get-Game
                $policy = $After
                if ($policy -eq 'auto') {
                    $age = if ($g) { ((Get-Date) - $g.StartTime).TotalMinutes } else { 0 }
                    $sick = [OblvBench.GameWatch]::Running -and [OblvBench.GameWatch]::Problem
                    $policy = if (-not $g) { 'close' } elseif ($Session -eq 'fresh') { 'close' } elseif ($sick) { 'close' } elseif ($age -gt $MaxSessionMin) { 'close' } elseif ($run -lt $Runs) { 'keep' } elseif ((Test-FgMenuRisk)) { 'close' } else { 'park' }
                }
                if ($policy -eq 'park' -and $g) {
                    try { Set-GameParked $g; $gameLeft = 'parked at the main menu' }
                    catch { Write-Warning "could not park the game ($_): closing it"; $policy = 'close' }
                }
                if ($policy -eq 'keep' -and $g) { if ($run -eq $Runs) { Send-Bench 'spectate off' -NoThrow | Out-Null; Set-BenchObserver $false | Out-Null; Send-Bench 'fade off' -NoThrow | Out-Null }; $gameLeft = 'left in the world' }
                if ($policy -eq 'close' -or -not $g) {
                    if (Get-Game) { Step 'closing the game'; Stop-BenchGame }
                    if (-not (Get-Game)) { Restore-Config; Restore-Saves; Step (Disable-BenchUE4SS); $gameLeft = 'closed (saves and config restored)' }
                } elseif ($run -eq $Runs -and ($IdleCloseMin -gt 0 -or $policy -eq 'park')) { Start-IdleWatcher }
                if ($gameLeft -match 'parked|world') { Step "game $gameLeft. Tools\Run-Benchmark.ps1 -Close closes it and restores saves and config$(if ($IdleCloseMin -gt 0) { " (done by itself after $IdleCloseMin min without a run)" })." }
            }
        } catch { Write-Warning "clean-up: $_" }
        finally {
            [OblvBench.GameWatch]::Stop()
            if (-not $Visible) { foreach ($line in Get-UnobtrusiveLog) { Step $line } }
            # the parked game stays hidden (the hook DLL stays in it) and muted until it is closed (Stop-Game un-mutes)
            Stop-Unobtrusive; Unlock-Game
            if ($Record) { try { Stop-Obs } catch { Write-Warning "OBS: $_" } }
            if ((Test-Path $script:BenchMutedFile) -and -not (Get-Game)) {
                Write-Warning 'the game went down while muted: Windows keeps it muted for its next launch. The next benchmark run (or Record-Session) un-mutes it; or un-mute it in the volume mixer.'
            }
        }
        $nWarn = if ($result) { @($result.sessions | ForEach-Object { $_.warnings } | Where-Object { $_ }).Count } else { 0 }
        Set-BenchStatus @{ state = 'done'; outcome = $outcome; game = $gameLeft; warnings = $nWarn; finished = (Get-Date).ToString('s'); scene = $null; message = "finished: $outcome" }
        if ($outcome -ne 'ok') { $exitCode = [math]::Max($exitCode, $(if ($outcome -eq 'partial') { 1 } else { 2 })) }
        Write-Host ("BENCH-RESULT {0} {1} dir={2} warnings={3} restarts={4} game={5}{6}" -f $outcome, $runLabel, $runDir, $nWarn, $script:restartCount, $gameLeft, $(if ($script:restartCount -gt 0) { ' comparable=no' } else { '' }))
    }
}
# for an on-screen run's guard (Runner-Guard -Visible): this runner reached its normal end
if ($Visible) { try { Set-Content (Join-Path $script:BenchRun 'runner-clean.txt') "$PID" -Encoding ascii } catch { } }
exit $exitCode
