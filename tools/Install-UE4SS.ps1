<#
.SYNOPSIS
  Puts the benchmark's copy of UE4SS (third_party\ue4ss) and this project's Lua mods (mods\ue4ss\) into the game's
  Binaries\Win64 folder, or takes them out again. UE4SS is only there while the benchmark uses the game: the
  benchmark enables it just before it starts the game and disables it once the game has closed, so the player's
  own sessions run without it (docs\benchmark.md, "UE4SS only during benchmark runs").

.EXAMPLE
  .\Install-UE4SS.ps1 -Status     # enabled / disabled, which build
  .\Install-UE4SS.ps1             # same as -Enable (game must be closed)
  .\Install-UE4SS.ps1 -Disable    # take it out again (game must be closed); -Uninstall is the old name
  .\Install-UE4SS.ps1 -ModsOnly   # (re)deploy the project mods into an enabled UE4SS; fine while the game runs,
                                  # but a running game only picks up main.lua after a restart

.NOTES
  Enable copies UE4SS from -Source (default third_party\ue4ss, or $env:OBLIVION_UE4SS_DIR) and writes a marker
  (ue4ss\oblvbench-install.json). Disable keeps UE4SS.log and crash dumps in <runtime>\ue4ss-logs\ and removes only
  what Enable put there. Another dwmapi.dll in the folder (another mod loader) is kept as dwmapi.dll.pre-ue4ss, and a
  UE4SS install that is not ours (the player's own mods) as ue4ss.pre-oblvbench; Disable puts both back.
#>
param(
    [switch]$Enable,
    [switch]$Disable,
    [switch]$Uninstall,
    [switch]$ModsOnly,
    [switch]$Status,
    [string]$Source = $env:OBLIVION_UE4SS_DIR,
    [string]$GameDir,
    [switch]$Quiet
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot 'GamePaths.ps1')
if (-not $GameDir) { $GameDir = (Get-OblivionGame).GameDir }
if (-not $Source) { $Source = Join-Path $root 'third_party\ue4ss' }
# The mod's bootstrap (Scripts\main.lua) gets this repo's folder and the runtime folder written into its deployed copy.
$runtime = Get-BenchRuntimeDir $root
function ConvertTo-LuaPath([string]$p) { ($p.TrimEnd('\', '/') -replace '\\', '/') + '/' }
function Say([string]$s) { if (-not $Quiet) { Write-Host $s } }
$bin = Join-Path $GameDir 'OblivionRemastered\Binaries\Win64'
if (-not (Test-Path (Join-Path $bin 'OblivionRemastered-Win64-Shipping.exe'))) { throw "Game exe not found in $bin" }

$dll = Join-Path $bin 'dwmapi.dll'
$dir = Join-Path $bin 'ue4ss'
$marker = Join-Path $dir 'oblvbench-install.json'
$backupDll = "$dll.pre-ue4ss"
$backupDir = "$dir.pre-oblvbench"
$logKeep = Join-Path $runtime 'ue4ss-logs'

function Test-UE4SSProxy([string]$path) {
    (Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path).VersionInfo.ProductName -eq 'UE4SS Injection Proxy')
}
# Ours = installed by this script (marker), or by its older version (no marker, but our benchmark mod is in it).
function Test-OurInstall { (Test-Path -LiteralPath $marker) -or (Test-Path -LiteralPath (Join-Path $dir 'Mods\OblvBench')) }
function Test-Enabled { (Test-UE4SSProxy $dll) -and (Test-Path -LiteralPath (Join-Path $dir 'UE4SS.dll')) -and (Test-OurInstall) }
function Test-GameRunning { [bool](Get-Process OblivionRemastered-Win64-Shipping, OblivionRemastered -ErrorAction SilentlyContinue) }
function Get-Sha([string]$p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { $null } }

function Deploy-ProjectMods {
    $modsDir = Join-Path $dir 'Mods'
    if (-not (Test-Path $modsDir)) { throw "UE4SS not installed at $dir" }
    $modsTxt = Join-Path $modsDir 'mods.txt'
    foreach ($mod in Get-ChildItem (Join-Path $root 'mods\ue4ss') -Directory) {
        $dest = Join-Path $modsDir $mod.Name
        Copy-Item $mod.FullName $modsDir -Recurse -Force
        $boot = Join-Path $dest 'Scripts\main.lua'
        if (Test-Path -LiteralPath $boot) {
            $lua = [IO.File]::ReadAllText($boot)
            $lua = $lua.Replace('@PROJECT@', (ConvertTo-LuaPath $root)).Replace('@RUNTIME@', (ConvertTo-LuaPath $runtime))
            [IO.File]::WriteAllText($boot, $lua)
        }
        Set-Content (Join-Path $dest 'enabled.txt') '' -Encoding ascii
        # mods.txt entries must stay above the built-in Keybinds line
        $lines = @(Get-Content $modsTxt)
        if (-not ($lines -match "^\s*$([regex]::Escape($mod.Name))\s*:")) {
            $i = [array]::FindIndex([string[]]$lines, [Predicate[string]] { param($l) $l -match 'Built-in keybinds' })
            if ($i -lt 0) { $i = $lines.Count }
            $lines = @($lines[0..($i - 1)]) + "$($mod.Name) : 1" + @($lines[$i..($lines.Count - 1)])
            Set-Content $modsTxt $lines -Encoding ascii
        }
        Say "Deployed mod $($mod.Name)"
    }
}

# UE4SS.log and crash dumps of the last game outlive the removal (the runner's crash evidence reads the log earlier).
function Save-UE4SSLogs {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $files = @(Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq 'UE4SS.log' -or $_.Extension -eq '.dmp' })
    if (-not $files.Count) { return }
    New-Item -ItemType Directory -Force $logKeep | Out-Null
    foreach ($f in $files) { Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $logKeep "$stamp-$($f.Name)") -Force }
    # keep the newest 20 logs and 3 dumps
    Get-ChildItem $logKeep -Filter '*-UE4SS.log' | Sort-Object Name -Descending | Select-Object -Skip 20 | Remove-Item -Force
    Get-ChildItem $logKeep -Filter '*.dmp' | Sort-Object Name -Descending | Select-Object -Skip 3 | Remove-Item -Force
}

function Get-StatusText {
    if (Test-Enabled) {
        $info = if (Test-Path -LiteralPath $marker) { Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json } else { $null }
        $build = if ($info) { $info.build } else { 'installed by an older Install-UE4SS.ps1' }
        "enabled ($build) in $bin"
    } elseif ((Test-UE4SSProxy $dll) -or (Test-Path -LiteralPath $dir)) { "not ours or incomplete: a UE4SS that this script did not install is in $bin" }
    else { "disabled (no UE4SS in $bin)" }
}

function Disable-UE4SS {
    if (Test-GameRunning) { throw 'Close the game first (UE4SS is loaded in it).' }
    if (Test-Path -LiteralPath $dir) {
        if (Test-OurInstall) {
            Save-UE4SSLogs
            Remove-Item -LiteralPath $dir -Recurse -Force
            if (Test-UE4SSProxy $dll) { Remove-Item -LiteralPath $dll -Force }
        } else { Say "Left alone: $dir is not the benchmark's (no marker, no OblvBench mod)" }
    } elseif ((Test-UE4SSProxy $dll) -and -not (Test-Path -LiteralPath $backupDir)) {
        Remove-Item -LiteralPath $dll -Force            # a proxy without its UE4SS folder: a half-finished enable of ours
    }
    if ((Test-Path -LiteralPath $backupDir) -and -not (Test-Path -LiteralPath $dir)) { Rename-Item -LiteralPath $backupDir -NewName (Split-Path $dir -Leaf); Say "Put back the UE4SS that was there before: $dir" }
    if ((Test-Path -LiteralPath $backupDll) -and -not (Test-Path -LiteralPath $dll)) { Move-Item -LiteralPath $backupDll -Destination $dll; Say "Put back the dwmapi.dll that was there before" }
    Say (Get-StatusText)
}

function Enable-UE4SS {
    if (Test-GameRunning) {
        if (Test-Enabled) { Deploy-ProjectMods; Say 'The game is running: mods deployed, UE4SS itself left as it is.'; return }
        throw 'Close the game first.'
    }
    $srcDll = Join-Path $Source 'ue4ss\UE4SS.dll'
    if (-not (Test-Path -LiteralPath (Join-Path $Source 'dwmapi.dll')) -or -not (Test-Path -LiteralPath $srcDll)) {
        throw "UE4SS not unpacked at $Source (tools\Setup-Bench.ps1 does it)"
    }
    $want = Get-Sha $srcDll
    if ((Test-Enabled) -and (Get-Sha (Join-Path $dir 'UE4SS.dll')) -eq $want -and (Get-Sha $dll) -eq (Get-Sha (Join-Path $Source 'dwmapi.dll'))) {
        Deploy-ProjectMods                              # already in place: only refresh the mods
        Say (Get-StatusText); return
    }
    if (Test-Path -LiteralPath $dir) {
        if (Test-OurInstall) { Save-UE4SSLogs; Remove-Item -LiteralPath $dir -Recurse -Force }   # another build of ours: start clean
        elseif (-not (Test-Path -LiteralPath $backupDir)) { Rename-Item -LiteralPath $dir -NewName (Split-Path $backupDir -Leaf); Say "Kept the UE4SS that was there as $backupDir (put back by -Disable)" }
        else { throw "$dir is not ours and $backupDir exists already: sort it out by hand" }
    }
    # Keep any pre-existing dwmapi.dll that isn't a UE4SS proxy of ours (e.g. another mod loader, or the player's UE4SS)
    if ((Test-Path -LiteralPath $dll) -and -not (Test-Path -LiteralPath $backupDll) -and ((Test-Path -LiteralPath $backupDir) -or -not (Test-UE4SSProxy $dll))) {
        Move-Item -LiteralPath $dll -Destination $backupDll
        Say "Existing dwmapi.dll kept as $backupDll"
    }
    Copy-Item -LiteralPath (Join-Path $Source 'dwmapi.dll') -Destination $bin -Force
    Copy-Item -LiteralPath (Join-Path $Source 'ue4ss') -Destination $bin -Recurse -Force
    $build = Split-Path (Resolve-Path -LiteralPath $Source).Path -Leaf
    $verFile = Join-Path $Source 'oblvbench-build.txt'   # written by Setup-Bench.ps1: which release zip this is
    if (Test-Path -LiteralPath $verFile) { $build = (Get-Content -LiteralPath $verFile -TotalCount 1).Trim() }
    [ordered]@{ build = $build; source = $Source; ue4ssDllSha256 = $want; installed = (Get-Date).ToString('s') } |
        ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding utf8
    Deploy-ProjectMods
    Say "UE4SS enabled in $bin ($build). Log: $dir\UE4SS.log"
}

if ($Status) { Get-StatusText; return }
if ($ModsOnly) {
    if (Test-Enabled) { Deploy-ProjectMods } else { Say 'UE4SS is not enabled: the mods are deployed when the benchmark enables it.' }
    return
}
if ($Disable -or $Uninstall) { Disable-UE4SS; return }
Enable-UE4SS
