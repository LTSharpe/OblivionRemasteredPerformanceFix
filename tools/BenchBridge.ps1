<#
.SYNOPSIS
  Shared helpers for the benchmark (dot-source this file): the command bridge to the OblvBench
  UE4SS mod, game start/stop, and the "game in use" lock. Also handy on its own when
  developing scenes:

    . .\tools\BenchBridge.ps1
    Send-Bench 'state'
    Send-Bench 'tp world CheydinhalWorld 122787 90721 7278 150'
#>

$script:BenchRoot = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot 'GamePaths.ps1')
# Runtime files (shared with the in-game mod; status, locks, save guard): the repo's run\ folder by default
# (Get-BenchRuntimeDir in GamePaths.ps1; $env:OBLV_BENCH_RUNTIME overrides it).
$script:BenchRun = Get-BenchRuntimeDir $script:BenchRoot
$script:BenchCmdFile = Join-Path $script:BenchRun 'bench-commands.txt'
$script:BenchLogFile = Join-Path $script:BenchRun 'bench.log'
$script:BenchLockFile = Join-Path $script:BenchRun 'game.lock'
$script:BenchHeartbeatFile = Join-Path $script:BenchRun 'bench-heartbeat.txt'   # rewritten every second by the mod's frame loop
$script:BenchStatusFile = Join-Path $script:BenchRun 'bench-status.json'       # what the runner is doing (tools\Bench-Status.ps1)
$script:BenchLaunchFile = Join-Path $script:BenchRun 'bench-launch.json'       # the launch a runner asked for, and its game (tools\Runner-Guard.ps1)
$script:BenchStopFile = Join-Path $script:BenchRun 'bench-stop.json'           # Run-Benchmark.ps1 -Stop: which runner process should stop after its scene
$script:BenchDeadline = $null                                                         # [datetime]: Assert-GameOk throws DEADLINE after it
$script:BenchId = [int]((Get-Date).TimeOfDay.TotalSeconds) * 100
$script:GameProcessName = 'OblivionRemastered-Win64-Shipping'
# The resolved game install and the Saved folder where the game itself keeps config and saves
# (GamePaths.ps1; $env:OBLIVION_GAME_DIR / $env:OBLIVION_SAVED_DIR override them).
$script:GameInstall = try { Get-OblivionGame } catch {
    $fallback = 'C:\Program Files (x86)\Steam\steamapps\common\Oblivion Remastered'
    Write-Warning "$_ Trying the default Steam folder."
    [pscustomobject]@{ Source = 'Steam'; LaunchMethod = 'Steam'; GameDir = $fallback
        Exe = Join-Path $fallback 'OblivionRemastered\Binaries\Win64\OblivionRemastered-Win64-Shipping.exe' }
}
$script:GameLaunchMethod = if ($script:GameInstall.LaunchMethod) { $script:GameInstall.LaunchMethod }
                          elseif ($script:GameInstall.Source -eq 'Steam') { 'Steam' } else { 'Direct' }
$script:GameDir = $script:GameInstall.GameDir
$script:GameExe = $script:GameInstall.Exe
$script:GameWin64 = Join-Path $script:GameDir 'OblivionRemastered\Binaries\Win64'
$script:GameSaved = (Get-OblivionSavedDir).Path
$script:SteamAppId = 2623190
$script:SteamProcesses = @('steam', 'steamwebhelper')
$script:GameProcesses = @('OblivionRemastered', $script:GameProcessName)    # launcher stub and the game

if (-not ('OblvBench.Clock' -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot 'BenchLib.cs') -ReferencedAssemblies System.Drawing, System.Core
}

function Get-QpcMs { [OblvBench.Clock]::QpcMs() }

# ---------------------------------------------------------------- failing fast

<# Throws "GAMEDOWN: <kind>: <what>" as soon as the watchdog (BenchLib GameWatch, started by Start-Game or
   Watch-Game) has seen the game exit, crash, hang, stall or show an error, and "DEADLINE: ..." once the
   run is past $script:BenchDeadline. Every wait in the benchmark goes through this, so nothing waits on
   a game that can no longer answer. #>
function Assert-GameOk {
    if ([OblvBench.GameWatch]::Running) {
        $p = [OblvBench.GameWatch]::Problem
        if ($p) { throw "GAMEDOWN: $([OblvBench.GameWatch]::Kind): $p" }
    }
    if ($script:BenchDeadline -and (Get-Date) -gt $script:BenchDeadline) {
        throw ("DEADLINE: the run passed its time limit ({0:HH:mm:ss}, -MaxMinutes)" -f $script:BenchDeadline)
    }
}

<# Start-Sleep that gives up at once when the game goes down (see Assert-GameOk). #>
function Wait-Game([double]$Seconds) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        Assert-GameOk
        $left = $Seconds * 1000 - $sw.ElapsedMilliseconds
        if ($left -le 0) { return }
        Start-Sleep -Milliseconds ([int][math]::Min(250, $left))
    }
}

<# Watches a running game (process exit, crash reporter, error dialogs, hung window, frame heartbeat). #>
function Watch-Game($Game) {
    if ([OblvBench.GameWatch]::Running -and [OblvBench.GameWatch]::Pid -eq $Game.Id) { return }
    [OblvBench.GameWatch]::Start($Game.Id, $script:BenchHeartbeatFile)
}

# ---------------------------------------------------------------- status file

$script:BenchStatus = [ordered]@{}
<# Merges the values into bench-status.json in the runtime folder (written atomically; read by tools\Bench-Status.ps1). #>
function Set-BenchStatus([hashtable]$Values) {
    foreach ($k in $Values.Keys) { $script:BenchStatus[$k] = $Values[$k] }
    $script:BenchStatus['updatedAt'] = (Get-Date).ToString('s')
    $tmp = "$script:BenchStatusFile.tmp"
    try {
        [IO.File]::WriteAllText($tmp, ($script:BenchStatus | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding $false))
        Move-Item -LiteralPath $tmp -Destination $script:BenchStatusFile -Force
    } catch { }
}

# ---------------------------------------------------------------- scenarios

<# Loads bench\scenarios\<name>.json. "include": ["a", "b"] pulls in the scenes, places, weathers and
   masks of other scenarios (the including file wins where both define the same thing). #>
function Get-BenchScenario([string]$Name) {
    $path = Join-Path $script:BenchRoot "bench\scenarios\$Name.json"
    if (-not (Test-Path $path)) { throw "no scenario '$Name' ($path)" }
    $s = Get-Content $path -Raw | ConvertFrom-Json
    foreach ($key in 'settings', 'defaults', 'weathers', 'places') { if (-not $s.PSObject.Properties[$key]) { $s | Add-Member NoteProperty $key ([pscustomobject]@{}) } }
    foreach ($key in 'record', 'masks', 'scenes') { if (-not $s.PSObject.Properties[$key]) { $s | Add-Member NoteProperty $key @() } }
    if ($s.PSObject.Properties['include']) {
        foreach ($inc in @($s.include)) {
            $i = Get-BenchScenario $inc
            foreach ($key in 'settings', 'defaults', 'weathers', 'places') {
                foreach ($p in $i.$key.PSObject.Properties) { if (-not $s.$key.PSObject.Properties[$p.Name]) { $s.$key | Add-Member NoteProperty $p.Name $p.Value } }
            }
            $s.scenes = @($s.scenes) + @($i.scenes)
            $s.record = @(@($s.record) + @($i.record) | Select-Object -Unique)
            if (-not @($s.masks).Count) { $s.masks = $i.masks }
            foreach ($key in @('exec')) { if (-not $s.PSObject.Properties[$key] -and $i.PSObject.Properties[$key]) { $s | Add-Member NoteProperty $key $i.$key } }
        }
    }
    $s
}

# ---------------------------------------------------------------- bridge

function Read-BenchLogFrom([long]$Position) {
    if (-not (Test-Path $script:BenchLogFile)) { return '' }
    $fs = [IO.File]::Open($script:BenchLogFile, 'Open', 'Read', 'ReadWrite')
    try {
        if ($fs.Length -le $Position) { return '' }
        $fs.Position = $Position
        (New-Object IO.StreamReader $fs).ReadToEnd()
    } finally { $fs.Dispose() }
}

function Get-BenchLogLength { if (Test-Path $script:BenchLogFile) { (Get-Item $script:BenchLogFile).Length } else { 0 } }

function Add-BenchLine([string]$Line) {
    $enc = New-Object Text.UTF8Encoding $false
    for ($try = 0; ; $try++) {
        try { [IO.File]::AppendAllText($script:BenchCmdFile, "$Line`n", $enc); return }
        catch [IO.IOException] { if ($try -ge 20) { throw }; Start-Sleep -Milliseconds 10 }
    }
}

<# Sends "<verb> <args>" to the mod and returns its answer. Throws on ERR or timeout unless -NoThrow
   (then returns $null). #>
function Send-Bench {
    param([Parameter(Mandatory, Position = 0)][string]$Command, [int]$TimeoutMs = 8000, [switch]$NoThrow)
    $id = ++$script:BenchId
    # a game that is down cannot answer: fail at once (-NoThrow callers, e.g. clean-up, just get $null)
    if ([OblvBench.GameWatch]::Running -and [OblvBench.GameWatch]::Problem) { if ($NoThrow) { return $null }; Assert-GameOk }
    $pos = Get-BenchLogLength
    Add-BenchLine "$id $Command"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $m = [regex]::Match((Read-BenchLogFrom $pos), "(?m)^\S+ ACK $id (OK|ERR) ?(.*?)\r?$")
        if ($m.Success) {
            if ($m.Groups[1].Value -eq 'OK') { return $m.Groups[2].Value }
            if ($NoThrow) { return $null }
            throw "OblvBench: '$Command' failed: $($m.Groups[2].Value)"
        }
        if ([OblvBench.GameWatch]::Running -and [OblvBench.GameWatch]::Problem) { if ($NoThrow) { return $null }; Assert-GameOk }
        Start-Sleep -Milliseconds 15
    }
    if ($NoThrow) { return $null }
    throw "OblvBench: no answer to '$Command' within $TimeoutMs ms (game stalled, or mod not loaded)"
}

function ConvertFrom-BenchState([string]$Text) {
    $h = @{}
    foreach ($m in [regex]::Matches($Text, '(\w+)=(\S*)')) { $h[$m.Groups[1].Value] = $m.Groups[2].Value }
    $h
}

<# Full state, or with -Light only what is cheap to read every 100 ms (position, level, travel flag). #>
function Get-BenchState([int]$TimeoutMs = 8000, [switch]$Light) {
    $s = Send-Bench $(if ($Light) { 'where' } else { 'state' }) -TimeoutMs $TimeoutMs -NoThrow
    if ($null -eq $s) { return $null }
    ConvertFrom-BenchState $s
}

<# Polls the state until the condition (scriptblock taking the state hashtable) has held for
   -Stable consecutive polls. Returns the last state, or $null on timeout. #>
function Wait-BenchState {
    param([Parameter(Mandatory)][scriptblock]$Condition, [double]$TimeoutSec = 60, [int]$Stable = 1, [int]$PollMs = 200)
    $sw = [Diagnostics.Stopwatch]::StartNew(); $ok = 0
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        Assert-GameOk
        $s = Get-BenchState 3000
        if ($s -and (& $Condition $s)) { $ok++; if ($ok -ge $Stable) { return $s } } else { $ok = 0 }
        Start-Sleep -Milliseconds $PollMs
    }
    $null
}

<# "EV ..." lines the mod logged after a log position, with the mod's clock (seconds). #>
function Get-BenchEvents([long]$Position) {
    foreach ($line in (Read-BenchLogFrom $Position) -split "`r?`n") {
        if ($line -match '^(\S+) EV (.*)$') { [pscustomobject]@{ Clock = [double]$Matches[1]; Text = $Matches[2] } }
    }
}

# ---------------------------------------------------------------- game process

function Get-Game { Get-Process $script:GameProcessName -ErrorAction SilentlyContinue | Select-Object -First 1 }

# ---------------------------------------------------------------- staying out of the PC user's way
# You may be working on this PC while runs go on (docs\benchmark.md, "Staying out of the way"): no window, no focus
# change, no confined cursor, no sound, Steam usable as normal.

$script:BenchMutedFile = Join-Path $script:BenchRun 'bench-audio-muted.txt'   # exists while the game may be left muted (Windows remembers it)
[OblvBench.GameAudio]::FlagPath = $script:BenchMutedFile                          # also in a process that only un-mutes (-Close, the idle watcher)

<# Asks Steam to start the game. Not with Start-Process from here: this process descends from the app the PC user
   is typing in, so it can pass on the right to take the foreground (ShellExecute -> Steam -> the game), and then
   Steam's launch dialog or the game takes the keyboard focus from the PC user (2026-10-06: typing interrupted during
   a launch). The request goes through a process that Windows' WMI service creates, which has no such right;
   Start-Process only if WMI fails. #>
function Start-SteamLaunch([string[]]$GameArgs = @()) {
    $cmd = (Get-ItemProperty 'Registry::HKEY_CLASSES_ROOT\steam\Shell\Open\Command' -ErrorAction SilentlyContinue).'(default)'
    $url = "steam://rungameid/$($script:SteamAppId)"
    # Arguments for this launch only (e.g. -FixedSeed): `steam.exe -applaunch <app> <args>` hands them to the running
    # Steam, which adds them to the player's own launch options for this one start; nothing in Steam's config changes.
    if ($GameArgs.Count -and $cmd -match '^\s*"([^"]+steam\.exe)"') {
        $line = '"{0}" -applaunch {1} {2}' -f $Matches[1], $script:SteamAppId, ($GameArgs -join ' ')
        try {
            $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $line } -ErrorAction Stop
            if ($r.ReturnValue -eq 0) { return }
            Write-Warning "launch with arguments through WMI failed (code $($r.ReturnValue)): launching without them"
        } catch { Write-Warning "launch with arguments through WMI failed ($_): launching without them" }
    }
    if ($cmd -match '%1') {
        try {
            $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = ($cmd -replace '%1', $url) } -ErrorAction Stop
            if ($r.ReturnValue -eq 0) { return }
            Write-Warning "launch through WMI failed (code $($r.ReturnValue)): asking Steam directly"
        } catch { Write-Warning "launch through WMI failed ($_): asking Steam directly" }
    }
    Start-Process $url
}

function Start-OblivionLaunch([string[]]$GameArgs = @()) {
    if ($script:GameLaunchMethod -eq 'Steam') {
        Start-SteamLaunch -GameArgs $GameArgs
        return
    }
    if (-not (Test-Path -LiteralPath $script:GameExe)) { throw "Game executable not found: $($script:GameExe)" }
    Start-Process -FilePath $script:GameExe -ArgumentList $GameArgs -WorkingDirectory $script:GameWin64 | Out-Null
}

<# The bench-only hook DLL (tools\benchhook), built on demand; $null (with a warning) if it cannot be built. #>
function Get-BenchHookDll {
    if ($script:BenchHookDll) { return $script:BenchHookDll }
    try { $script:BenchHookDll = & (Join-Path $PSScriptRoot 'benchhook\build.ps1') | Select-Object -Last 1 }
    catch { Write-Warning "OblvBenchHook.dll not available ($_): the game window is only kept off-screen from outside, it can flash up briefly" }
    $script:BenchHookDll
}

<# From now on (until Stop-Unobtrusive or the end of this process): the game's window is created and kept
   off-screen, without a taskbar button and never active (hook DLL in the game's window thread; it stays in
   effect in a game left running after this process ends), Steam's launch dialog goes off-screen, the focus
   goes back to the PC user if the game or that dialog takes it, a cursor confined by the game is released, and
   the game is muted in the volume mixer (unless -Audible). Safe to call again. #>
function Start-Unobtrusive([switch]$Audible, [switch]$Capturable) {
    $dll = Get-BenchHookDll
    if ($dll) { [OblvBench.GameHider]::Start($dll, [string[]]@($script:GameProcessName)); if ($PSBoundParameters.ContainsKey('Capturable')) { [OblvBench.GameHider]::SetCapturable([bool]$Capturable) } }   # -Capturable (the runner always passes it; Start-Game's call keeps it): owned window, so OBS can capture it
    [OblvBench.WindowKeeper]::Keep([string[]]$script:SteamProcesses, [string[]]$script:GameProcesses)
    if (-not $Audible) { [OblvBench.GameAudio]::Start([string[]]@($script:GameProcessName)) }
    if ($g = Get-Game) { Start-CrashGuard $g.Id }        # a game started earlier (parked, or restarted after a crash)
}

$script:CrashGuardPidFile = Join-Path $script:BenchRun 'crash-guard.pid'   # "<guard pid> <game pid>"

<# Makes sure a hidden tools\Crash-Guard.ps1 keeps this game's crash UI (UE's crash reporter, Windows error
   reporting, its message boxes, AMD's bug report tool) off the screen for as long as the game runs, also between
   benchmark runs (hand probing). One guard per game process; it ends by itself 20 s after the game. Backs up the
   hook DLL in the game, which keeps the crash reporter from starting at all (docs\benchmark.md, "Staying out of
   the way"). #>
function Start-CrashGuard([int]$GamePid) {
    if (Test-Path $script:CrashGuardPidFile) {
        $p = ([IO.File]::ReadAllText($script:CrashGuardPidFile)).Trim() -split '\s+'
        if ($p.Count -ge 2 -and [int]$p[1] -eq $GamePid) {
            $g = Get-CimInstance Win32_Process -Filter "ProcessId=$([int]$p[0])" -ErrorAction SilentlyContinue
            if ($g -and $g.CommandLine -match 'Crash-Guard') { return }
        }
    }
    $proc = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        "`"$(Join-Path $PSScriptRoot 'Crash-Guard.ps1')`"", '-GamePid', $GamePid)
    Set-Content $script:CrashGuardPidFile "$($proc.Id) $GamePid" -Encoding ascii
}

function Set-BenchLaunch([hashtable]$Values) {
    try { [IO.File]::WriteAllText($script:BenchLaunchFile, ($Values | ConvertTo-Json -Compress)) } catch { Write-Warning "could not write $($script:BenchLaunchFile): $_" }
}

$script:RunnerGuardPidFile = Join-Path $script:BenchRun 'runner-guard.pid'   # "<guard pid> <runner pid>"

<# Starts tools\Runner-Guard.ps1 hidden for this process (once): if this runner dies while it holds game.lock (killed,
   or its PowerShell ended), the guard hides and mutes its game, also one Steam is still starting, and closes it
   cleanly (Run-Benchmark.ps1 -Close). A normal end unlocks first, and the guard then just exits. #>
function Start-RunnerGuard([switch]$Audible, [switch]$Visible) {
    if (Test-Path $script:RunnerGuardPidFile) {
        $p = ([IO.File]::ReadAllText($script:RunnerGuardPidFile)).Trim() -split '\s+'
        if ($p.Count -ge 2 -and [int]$p[1] -eq $PID) {
            $g = Get-CimInstance Win32_Process -Filter "ProcessId=$([int]$p[0])" -ErrorAction SilentlyContinue
            if ($g -and $g.CommandLine -match 'Runner-Guard') { return }
        }
    }
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $PSScriptRoot 'Runner-Guard.ps1')`"", '-RunnerPid', $PID)
    if ($Audible) { $a += '-Audible' }
    if ($Visible) { $a += '-Visible' }
    $proc = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList $a
    Set-Content $script:RunnerGuardPidFile "$($proc.Id) $PID" -Encoding ascii
}

function Stop-Unobtrusive {
    [OblvBench.GameAudio]::Stop(); [OblvBench.WindowKeeper]::Stop(); [OblvBench.GameHider]::Stop()
}

<# On-screen runs (-Visible): the game window to the front with the keyboard focus (BenchLib Foreground.Bring: no input
   is sent; a minimised game is left alone). Returns "foreground: already|direct|attached|failed (...)". #>
function Set-GameForeground {
    $p = Get-Game
    if (-not $p) { return 'foreground: no game' }
    "foreground: $([OblvBench.Foreground]::Bring([OblvBench.GameWatch]::FindGameWindow($p.Id), $p.Id))"
}

<# Watch mode (-Visible): the hook DLL goes into the game as well, with "show" set, so its window is left alone (it comes
   to the front as usual) while the game-side parts work as in a hidden run: no crash UI, and spectator mode's NPC
   conversation guard (Set-BenchObserver). #>
function Start-ShownHook {
    $dll = Get-BenchHookDll
    if ($dll) { [OblvBench.GameHider]::Start($dll, [string[]]@($script:GameProcessName), $true) }
}

<# Spectator mode's observer: with $true NPC AI treats the player as not there. The hook in the game
   (tools\benchhook\BenchHook.cpp, "observer mode") gives no AI package the player as its target and refuses every
   conversation an NPC starts with the player (forced greetings, which `tdetect` does not stop); and the player's
   Chameleon is 100, so even with detection on (the battle's "detect": true) nobody detects the player, and no enemy
   fights it. $false gives the player back. Returns the hook's state in the given game, e.g. "installed, process
   SetPackageTarget slot +0x1C8, ...". #>
function Set-BenchObserver([bool]$On, [int]$GamePid = 0) {
    [OblvBench.GameHider]::SetObserver($On)
    Send-Bench "obv player.setav chameleon $(if ($On) { 100 } else { 0 })" -NoThrow | Out-Null
    if (-not $GamePid -and ($g = Get-Game)) { $GamePid = $g.Id }
    [OblvBench.GameHider]::TalkGuardText([OblvBench.GameHider]::TalkGuardFor($GamePid))
}

<# What the hider, the keeper and the muter did since the last call, one line each. #>
function Get-UnobtrusiveLog {
    foreach ($l in [OblvBench.GameHider]::TakeLog()) { "hook: $l" }
    foreach ($l in [OblvBench.WindowKeeper]::TakeLog()) { "window: $l" }
    foreach ($l in [OblvBench.GameAudio]::TakeLog()) { "audio: $l" }
}

<# Un-mutes the game in the volume mixer (Windows would keep it muted for the next launch too). Returns the
   number of audio sessions found: 0 = no running game with audio, so nothing could be done (the flag file
   then stays, and the next benchmark game or Repair-GameMute un-mutes it). #>
function Set-GameUnmuted {
    [OblvBench.GameAudio]::Stop()
    $n = 0
    try { $n = [OblvBench.GameAudio]::SetMute([string[]]@($script:GameProcessName), $false) } catch { Write-Warning "un-muting the game failed: $_" }
    $n
}

<# Turns the game's own master volume to 0 (only for this game session: the settings the game writes as it
   quits are put back by the save guard) and waits until its audio session's meter shows silence, so the
   mixer mute can be lifted without a sound. $true when silent. #>
function Set-GameSilent([int]$TimeoutMs = 3000) {
    $r = Send-Bench 'cvar Altar.Audio.MasterVolume 0' -TimeoutMs $TimeoutMs -NoThrow
    if ($r -notmatch '=0(\.0*)?$') { return $false }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $peak = try { [OblvBench.GameAudio]::Peak([string[]]@($script:GameProcessName)) } catch { 1 }
        if ($peak -lt 0.0001) { Start-Sleep -Milliseconds 100; return $true }   # also -1: no audio session, nothing to hear
        Start-Sleep -Milliseconds 50
    }
    $false
}

<# A game that went down while the benchmark had it muted stays muted in Windows' volume mixer: if one runs
   now (e.g. the player's), un-mute it. #>
function Repair-GameMute {
    if ((Test-Path $script:BenchMutedFile) -and (Get-Game) -and -not [OblvBench.GameAudio]::Running) {
        if ((Set-GameUnmuted) -gt 0) { 'the game had been left muted by a benchmark that went down: un-muted' }
    }
}

<# Brings a benchmark game (hidden off-screen) onto the primary monitor with a taskbar button, un-muted and in
   front, to look at it or play; Hide-GameWindow puts it back. A new run hides it again. #>
function Show-GameWindow {
    $g = Get-Game
    if (-not $g) { throw 'no game running' }
    [OblvBench.GameHider]::SetShow($true)
    Set-BenchObserver $false $g.Id | Out-Null               # to play it, NPCs must see the player and talk to it
    Set-GameUnmuted | Out-Null
    [OblvBench.WindowKeeper]::BringOnScreen([OblvBench.GameWatch]::FindGameWindow($g.Id))
}

function Hide-GameWindow {
    $g = Get-Game
    if (-not $g) { throw 'no game running' }
    [OblvBench.GameHider]::SetShow($false)
    [OblvBench.WindowKeeper]::SendOffScreen([OblvBench.GameWatch]::FindGameWindow($g.Id))
}

# ---------------------------------------------------------------- UE4SS only while the benchmark uses the game
# UE4SS (and the OblvBench/OblvLab mods) is put into the game folder just before the benchmark starts the game and taken
# out again once the game has closed (Install-UE4SS.ps1 -Enable / -Disable), so a game started from Steam by the player
# runs without it (docs\benchmark.md, "UE4SS only during benchmark runs"). $env:OBLIVION_UE4SS_DIR picks another
# unpacked UE4SS build for the benchmark (default third_party\ue4ss).

<# Puts UE4SS and the project's mods into the game folder (the game must be closed). Throws if it cannot. #>
function Enable-BenchUE4SS {
    & (Join-Path $PSScriptRoot 'Install-UE4SS.ps1') -Enable -Quiet
}

<# Takes UE4SS out of the game folder once no game runs (UE4SS.log and crash dumps go to <runtime>\ue4ss-logs).
   Never throws: returns what it did, or why not. #>
function Disable-BenchUE4SS {
    if (Get-Process $script:GameProcesses -ErrorAction SilentlyContinue) { return 'UE4SS left enabled: the game is still running' }
    try { & (Join-Path $PSScriptRoot 'Install-UE4SS.ps1') -Disable -Quiet; 'UE4SS disabled (a game started outside the benchmark runs without it)' }
    catch { "could not disable UE4SS: $_" }
}

<# Why Steam did not start the game, from its UI log (logs\webhelper_js.txt) since $Since; $null if nothing known.
   (2026-10-15, worker cellrow) A launch-time Steam Cloud sync failure makes Steam show a "cloud sync failed" dialog in
   its launch window, which the benchmark keeps hidden, and Steam then ignores every further launch request until
   someone answers it in Steam (a person's decision: the benchmark does not answer it). #>
function Get-SteamLaunchProblem([datetime]$Since) {
    try {
        $root = if (Get-Command Get-SteamRoot -ErrorAction SilentlyContinue) { Get-SteamRoot } else { $null }
        if (-not $root) { $root = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath }
        $f = if ($root) { Join-Path $root 'logs\webhelper_js.txt' } else { $null }
        if (-not $f -or -not (Test-Path -LiteralPath $f)) { return $null }
        $fs = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite')
        try { $len = $fs.Length; $fs.Position = [Math]::Max(0, $len - 65536); $tail = (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
        foreach ($l in ($tail -split "`n")) {
            if ($l -notmatch '^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\].*(cloud sync failed dialog|syncfailed)') { continue }
            if ([datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss', $null) -lt $Since.AddSeconds(-2)) { continue }
            return "Steam's cloud sync of the game's saves failed at launch and Steam waits for an answer in its (hidden) launch dialog ($($l.Trim())); it ignores further launches until someone answers it in Steam or Steam is restarted"
        }
    } catch {}
    $null
}

# ---------------------------------------------------------------- crash evidence (2026-10-20, worker npcpromo)

if (-not ('OblvBench.ProcessFreeze' -as [type])) {
    Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
namespace OblvBench {
    public static class ProcessFreeze {
        [DllImport("ntdll.dll")] static extern int NtSuspendProcess(IntPtr h);
        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(int access, bool inherit, int pid);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
        // freezes every thread of the process (PROCESS_SUSPEND_RESUME); true on success
        public static bool Suspend(int pid) {
            IntPtr h = OpenProcess(0x0800, false, pid);
            if (h == IntPtr.Zero) return false;
            try { return NtSuspendProcess(h) >= 0; } finally { CloseHandle(h); }
        }
    }
}
'@
}

$script:FixLogName = 'OblivionPerfFix.log'

<# The last $Bytes of a file another process is writing (shared read), as text; '' if it cannot be read. #>
function Read-FileTail([string]$path, [int]$Bytes = 1MB) {
    try {
        $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite, Delete')
        try {
            $n = [int][Math]::Min($fs.Length, $Bytes)
            $fs.Seek(-$n, 'End') | Out-Null
            $buf = New-Object byte[] $n
            $got = $fs.Read($buf, 0, $n)
            [Text.Encoding]::UTF8.GetString($buf, 0, $got)
        } finally { $fs.Dispose() }
    } catch { '' }
}

<# Copies a file another process may hold open (shared read); $true on success. #>
function Copy-SharedFile([string]$from, [string]$to) {
    try {
        $src = [IO.File]::Open($from, 'Open', 'Read', 'ReadWrite, Delete')
        try { $dst = [IO.File]::Create($to); try { $src.CopyTo($dst) } finally { $dst.Dispose() } } finally { $src.Dispose() }
        $true
    } catch { $false }
}

<# Is the lab DLL writing its crash dumps right now? (FullCrashDump: the crashed game waits in UE's crash handling while
   OblivionPerfFix-dumpwriter.exe writes the full dump; the memory snapshot before the writer starts took ~20 s at 4K on
   2026-10-20.) A reason string while it is, $null when not (no lab dump, finished, or the game gone with no writer left). #>
function Get-LabCrashDumpState {
    if (Get-Process OblivionPerfFix-dumpwriter -ErrorAction SilentlyContinue) { return 'the lab dump writer is running' }
    if (-not (Get-Game)) { return $null }
    $tail = Read-FileTail (Join-Path $script:GameWin64 $script:FixLogName) 1MB
    $i = $tail.LastIndexOf('[crash] UE is reporting a crash')
    if ($i -lt 0) { return $null }
    if ($tail.Substring($i) -match 'UE goes on with the crash|full memory dump (written|FAILED|skipped)') { return $null }
    'the game is writing its crash dumps (the DLL log has the crash, not yet the end of its dump)'
}

<# Waits (at most $MaxSec) until the lab DLL's crash dumps are written, so the game is not ended under them. Returns a
   note ('' when there was nothing to wait for). #>
# 1800 s: the lab DLL now writes a complete full dump at every crash, from the game itself when no snapshot can be taken
# (minutes at 4K; 542.7 s for 23.3 GB on 2026-10-21), and the game must not be ended under it (2026-10-21, worker crashfull)
function Wait-LabCrashDump([int]$MaxSec = 1800) {
    $why = Get-LabCrashDumpState
    if (-not $why) { return '' }
    Write-Host "waiting for the lab crash dump ($why; at most $MaxSec s)"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($why -and $sw.Elapsed.TotalSeconds -lt $MaxSec) { Start-Sleep -Milliseconds 500; $why = Get-LabCrashDumpState }
    if ($why) { 'lab crash dump: still not finished after {0:F0} s ({1}); ending the game anyway' -f $sw.Elapsed.TotalSeconds, $why }
    else { 'lab crash dump: waited {0:F0} s for it' -f $sw.Elapsed.TotalSeconds }
}

<# Keeps the DLL log of the previous game before a new one overwrites it: <runtime>\fixlogs\OblivionPerfFix-<its last
   write time>.log (shared read; the newest 6 are kept). Returns the copy's path or $null. #>
function Save-PreviousFixLog {
    $log = Join-Path $script:GameWin64 $script:FixLogName
    if (-not (Test-Path -LiteralPath $log)) { return $null }
    $item = Get-Item -LiteralPath $log
    if ($item.Length -eq 0) { return $null }
    $dir = Join-Path $script:BenchRun 'fixlogs'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $to = Join-Path $dir ('OblivionPerfFix-{0:yyyyMMdd-HHmmss}.log' -f $item.LastWriteTime)
    if ((Test-Path -LiteralPath $to) -and (Get-Item -LiteralPath $to).Length -eq $item.Length) { return $to }
    if (-not (Copy-SharedFile $log $to)) { return $null }
    Get-ChildItem $dir -Filter 'OblivionPerfFix-*.log' | Sort-Object Name -Descending | Select-Object -Skip 6 |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -ErrorAction SilentlyContinue }
    $to
}

<# Ends a game that does not answer (hung, crashed) without a sound: it is frozen first (no new audio), and only when its
   audio session's meter then reads silence for 200 ms is it un-muted (Windows would keep the mute for the next launch);
   otherwise it ends muted (the flag file stays: the next benchmark game's close or Repair-GameMute un-mutes). Waits for
   the lab crash dump first. Returns notes. #>
function Stop-GameQuietly($p) {
    $notes = New-Object System.Collections.Generic.List[string]
    $w = Wait-LabCrashDump
    if ($w) { $notes.Add($w) }
    $p.Refresh()
    if ($p.HasExited) { $notes.Add('the game had ended by itself'); return $notes }
    $frozen = [OblvBench.ProcessFreeze]::Suspend($p.Id)
    $quiet = 0
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($frozen -and $sw.ElapsedMilliseconds -lt 3000 -and $quiet -lt 4) {
        $peak = try { [OblvBench.GameAudio]::Peak([string[]]@($script:GameProcessName)) } catch { 1 }
        if ($peak -lt 0.0001) { $quiet++ } else { $quiet = 0 }
        Start-Sleep -Milliseconds 50
    }
    if ($quiet -ge 4) {
        $n = Set-GameUnmuted
        $notes.Add("froze the game, its audio read silence: un-muted ($n session(s)) and ended it")
    } else {
        [OblvBench.GameAudio]::Stop()
        $notes.Add("ended the game muted ($(if ($frozen) { 'its audio did not read silence after freezing it' } else { 'could not freeze it' })); the next benchmark game un-mutes it")
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    $p.WaitForExit(20000) | Out-Null
    $notes
}

function Start-Game {
    param([int]$TimeoutSec = 240, [switch]$Unobtrusive, [switch]$Audible, [string[]]$GameArgs = @(), [uint64]$CpuMask = 0)
    if (Get-Game) { throw 'The game is already running.' }
    # Steam refuses a launch while the previous instance (launcher stub, crash reporter) is still around
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ((Get-Process OblivionRemastered, CrashReportClient -ErrorAction SilentlyContinue) -and $sw.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 500 }
    # New run: the mod starts reading at the end of the command file, so empty both files first.
    Set-Content $script:BenchCmdFile '' -NoNewline -Encoding ascii
    Set-Content $script:BenchLogFile '' -NoNewline -Encoding ascii
    if (Test-Path $script:BenchHeartbeatFile) { Remove-Item -LiteralPath $script:BenchHeartbeatFile -ErrorAction SilentlyContinue }
    $kept = Save-PreviousFixLog            # the new game overwrites the DLL log: keep the last one
    if ($kept) { Write-Host "previous DLL log kept: $kept" }
    Enable-BenchUE4SS                      # only for this game: taken out again once it has closed
    if ($Unobtrusive) { Start-Unobtrusive -Audible:$Audible } else { Start-ShownHook }
    $launchedAt = Get-Date
    # for tools\Runner-Guard.ps1: if this process dies now, the game about to start is this runner's
    Set-BenchLaunch @{ runnerPid = $PID; launchAt = $launchedAt.ToString('o') }
    if ($CpuMask) {                        # lab (-CpuLimit): the stub and the game join a Job object limited to these processors
        if (-not ('OblvBench.CpuLimitJob' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'CpuLimitJob.cs') }
        Write-Host ([OblvBench.CpuLimitJob]::Arm($CpuMask, [string[]]@($script:GameProcesses | ForEach-Object { "$_.exe" }), ($TimeoutSec + 30) * 1000))
    }
    Start-OblivionLaunch -GameArgs $GameArgs
    $sw = [Diagnostics.Stopwatch]::StartNew(); $asked = 1; $checked = 0
    while (-not ($p = Get-Game)) {
        if ($script:GameLaunchMethod -eq 'Steam') {
            if ($sw.Elapsed.TotalSeconds -gt 60 * $asked -and $asked -lt 2) { Start-SteamLaunch -GameArgs $GameArgs; $asked++ }   # Steam sometimes drops the first request
            if ($sw.Elapsed.TotalSeconds -gt $checked + 10) {   # a known Steam reason ends the wait early
                $checked = $sw.Elapsed.TotalSeconds
                $why = Get-SteamLaunchProblem $launchedAt
                if ($why) { throw "The game did not start: $why" }
            }
        }
        if ($sw.Elapsed.TotalSeconds -gt $TimeoutSec) {
            $details = if ($script:GameLaunchMethod -eq 'Steam') {
                $why = Get-SteamLaunchProblem (Get-Date).AddHours(-12)
                " (is Steam running? Did it show an error? The benchmark keeps Steam minimised; Steam\logs\console_log.txt and webhelper_js.txt have the reason)$(if ($why) { ". Last known Steam problem: $why" })"
            } else { " (started directly from $($script:GameExe))" }
            throw "The game process did not appear within $TimeoutSec s$details"
        }
        if ($script:BenchDeadline) { Assert-GameOk }
        Start-Sleep -Milliseconds 500
    }
    Set-BenchLaunch @{ runnerPid = $PID; launchAt = $launchedAt.ToString('o'); gamePid = $p.Id }
    if ($CpuMask) {
        Start-Sleep -Milliseconds 200     # the stub's child, if the stub got there first, is in the job already
        [OblvBench.CpuLimitJob]::Disarm()
        $script:CpuLimitReport = [OblvBench.CpuLimitJob]::Report($p.Id)
        Write-Host "CPU limit: $($script:CpuLimitReport)"
    }
    Watch-Game $p                          # from here on a crash or hang during start-up ends the wait at once
    if ($Unobtrusive) { Start-CrashGuard $p.Id }   # its crash UI stays off the screen for as long as it runs
    while ($true) {
        Assert-GameOk
        $p.Refresh()
        if ([OblvBench.GameWatch]::FindGameWindow($p.Id) -ne [IntPtr]::Zero -and (Read-BenchLogFrom 0) -match 'LOADED bench\.lua') { break }   # not MainWindowHandle: it skips owned windows (-Record)
        if ($sw.Elapsed.TotalSeconds -gt $TimeoutSec) { throw "No window / OblvBench mod within $TimeoutSec s. Is UE4SS installed (tools\Install-UE4SS.ps1)?" }
        Start-Sleep -Milliseconds 500
    }
    $p
}

function Stop-Game {
    param([int]$GraceSec = 12)
    [OblvBench.GameWatch]::Stop()          # quitting is not a crash
    $p = Get-Game
    if (-not $p) { return }
    $w = Wait-LabCrashDump                 # a crashed lab game may still be writing its dumps: never end it under them
    if ($w) { Write-Host $w }
    if ((Test-Path $script:BenchMutedFile) -or [OblvBench.GameAudio]::Running) {
        # Windows would remember the mute, so un-mute while the game still runs, but silence it first: a game
        # parked at the main menu plays the main theme, and the PC user heard it for the seconds the quit took.
        if (Set-GameSilent) { Set-GameUnmuted | Out-Null }
        else {
            # could not silence it (mod not answering: hung or crashed): never un-mute it as it is (2026-10-20: a hung
            # game was un-muted before it was ended); freeze it first, un-mute only once its audio reads silence
            Write-Host 'Could not silence the game; ending it quietly.'
            foreach ($n in (Stop-GameQuietly $p)) { Write-Host $n }
        }
    }
    if (-not $p.HasExited) { Send-Bench 'quit' -TimeoutMs 3000 -NoThrow | Out-Null }
    if (-not $p.WaitForExit($GraceSec * 1000)) {
        Write-Host 'Game did not quit by itself; closing it.'
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        $p.WaitForExit(15000) | Out-Null
    }
    # the launcher stub exits with the game; give it a moment so the next start is clean
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ((Get-Process OblivionRemastered -ErrorAction SilentlyContinue) -and $sw.Elapsed.TotalSeconds -lt 15) { Start-Sleep -Milliseconds 500 }
}

# ---------------------------------------------------------------- "game in use" lock

<# Other sessions measure on the same game instance. A lock file plus a scan for running capture
   tools keeps the benchmark from restarting the game under them. #>
function Get-GameBusyReason {
    if (Test-Path $script:BenchLockFile) {
        $text = (Get-Content $script:BenchLockFile -Raw)
        $stale = $false
        if ($text -match 'pid=(\d+)') { $stale = ([int]$Matches[1] -eq $PID) -or -not (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue) }
        if (-not $stale) { return "lock file $($script:BenchLockFile): $($text.Trim())" }
    }
    $mine = $PID
    $busy = Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='pwsh.exe' or Name like 'PresentMon%'" |
        Where-Object { $_.ProcessId -ne $mine -and $_.CommandLine -match 'Profile-Oblivion|Run-ABTest|Sample-Stacks|Count-Calls|Trace-Vtable|Snap-Callers|Log-Args|Compare-Settings|Run-Benchmark|PresentMon' -and $_.CommandLine -notmatch 'Get-CimInstance' }
    if ($busy) { return "another capture is running: " + (($busy | ForEach-Object { "pid $($_.ProcessId) $($_.Name)" }) -join ', ') }
    $null
}

function Lock-Game([string]$Purpose) {
    Set-Content $script:BenchLockFile ("owner=Run-Benchmark pid=$PID since={0} purpose=$Purpose" -f (Get-Date -Format 'HH:mm:ss')) -Encoding ascii
}

function Unlock-Game {
    if ((Test-Path $script:BenchLockFile) -and ((Get-Content $script:BenchLockFile -Raw) -match "pid=$PID\b")) { Remove-Item -LiteralPath $script:BenchLockFile }
}
