<#
.SYNOPSIS
  Finds Oblivion Remastered on this PC: the Steam install (through Steam's own library list) and the folder where the
  game keeps its config and saves. Dot-source it for the functions, or run it to print what it found.

  Get-OblivionGame      Steam root, library, game folder, Binaries\Win64, exe, Steam build id
  Get-OblivionSavedDir  ...\My Games\Oblivion Remastered\Saved (Config\Windows, SaveGames), resolved the way the game does

  Overrides (for unusual setups): $env:OBLIVION_GAME_DIR (the folder that contains OblivionRemastered\Binaries) and
  $env:OBLIVION_SAVED_DIR (the Saved folder).

.NOTES
  Where the game writes (checked on build 19115871, 2026-10-07): the exe holds the relative path
  "../../Documents/My Games/Oblivion Remastered" and asks Windows only for the ProgramData, Documents and LocalAppData
  known folders. Taken from LocalAppData (C:\Users\<name>\AppData\Local) that path is <profile>\Documents\My Games\...,
  and that is where the game writes even when the Windows Documents folder is moved (to OneDrive, another drive):
  on the test PC Documents points to OneDrive\Documents, the game's config and saves are in <profile>\Documents, and
  only Steam Cloud's copy of the saves lands in OneDrive\Documents. So the rule here is LocalAppData\..\..\Documents,
  then the Documents known folder as a fallback, preferring whichever already has the game's Config folder.
  %USERPROFILE% is not used: LocalAppData is what the game's path is relative to.

.EXAMPLE
  tools\GamePaths.ps1
  . tools\GamePaths.ps1; (Get-OblivionGame).Win64
#>
[CmdletBinding()] param()

$script:OblivionAppId = '2623190'

function Get-SteamRoot {
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        $p = Get-ItemProperty -Path $k -ErrorAction SilentlyContinue
        foreach ($v in @($p.SteamPath, $p.InstallPath)) {
            if ($v -and (Test-Path -LiteralPath $v)) { return (Resolve-Path -LiteralPath $v).Path }
        }
    }
    $null
}

<# Values of a flat key in a Valve KeyValues (.vdf/.acf) text, e.g. all "path" entries. #>
function Get-VdfValues([string]$Text, [string]$Key) {
    [regex]::Matches($Text, '"' + [regex]::Escape($Key) + '"\s+"((?:[^"\\]|\\.)*)"') | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
}

function Get-OblivionGame {
    $bin = 'OblivionRemastered\Binaries\Win64'
    $exeName = 'OblivionRemastered-Win64-Shipping.exe'
    if ($env:OBLIVION_GAME_DIR) {
        $exe = Join-Path $env:OBLIVION_GAME_DIR "$bin\$exeName"
        if (-not (Test-Path -LiteralPath $exe)) { throw "OBLIVION_GAME_DIR is set, but $exe does not exist" }
        $steamDll = @(
            (Join-Path $env:OBLIVION_GAME_DIR 'steam_api64.dll'),
            (Join-Path $env:OBLIVION_GAME_DIR 'steam_api.dll'),
            (Join-Path (Split-Path $exe) 'steam_api64.dll'),
            (Join-Path (Split-Path $exe) 'steam_api.dll')
        ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        return [pscustomobject]@{ Source = 'OBLIVION_GAME_DIR'; LaunchMethod = $(if ($steamDll) { 'Steam' } else { 'Direct' }); SteamDll = $steamDll
            SteamRoot = Get-SteamRoot; Library = $null; GameDir = $env:OBLIVION_GAME_DIR
            Win64 = Split-Path $exe; Exe = $exe; BuildId = $null }
    }
    $steam = Get-SteamRoot
    if (-not $steam) { throw 'Steam not found (no Valve\Steam registry key). Set $env:OBLIVION_GAME_DIR to the game folder.' }
    $libs = New-Object System.Collections.Generic.List[string]
    $libs.Add($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
        foreach ($p in Get-VdfValues (Get-Content -LiteralPath $vdf -Raw) 'path') { if (-not $libs.Contains($p)) { $libs.Add($p) } }
    }
    foreach ($lib in $libs) {
        $acf = Join-Path $lib "steamapps\appmanifest_$script:OblivionAppId.acf"
        if (-not (Test-Path -LiteralPath $acf)) { continue }
        $text = Get-Content -LiteralPath $acf -Raw
        $installDir = @(Get-VdfValues $text 'installdir')[0]
        if (-not $installDir) { continue }
        $gameDir = Join-Path $lib "steamapps\common\$installDir"
        $exe = Join-Path $gameDir "$bin\$exeName"
        if (-not (Test-Path -LiteralPath $exe)) { continue }
        return [pscustomobject]@{ Source = 'Steam'; LaunchMethod = 'Steam'; SteamRoot = $steam; Library = $lib; GameDir = $gameDir
            Win64 = Split-Path $exe; Exe = $exe; BuildId = @(Get-VdfValues $text 'buildid')[0] }
    }
    throw "Oblivion Remastered (Steam app $script:OblivionAppId) not found in the Steam libraries: $($libs -join '; '). Set `$env:OBLIVION_GAME_DIR."
}

<# The folder for the benchmark's runtime files: the command and log files shared with the in-game mod, the run status,
   the game lock, the save guard. $env:OBLV_BENCH_RUNTIME if set; else <repo>\lab when the repo has a lab folder (the
   private lab the benchmark was developed in, which keeps its files there); else <repo>\run (git-ignored). Created if
   missing. tools\Install-UE4SS.ps1 writes this path into the deployed mod, so redeploy (-ModsOnly) after changing it. #>
function Get-BenchRuntimeDir([string]$Root = (Split-Path $PSScriptRoot)) {
    $dir = if ($env:OBLV_BENCH_RUNTIME) { $env:OBLV_BENCH_RUNTIME }
           elseif (Test-Path -LiteralPath (Join-Path $Root 'lab') -PathType Container) { Join-Path $Root 'lab' }
           else { Join-Path $Root 'run' }
    New-Item -ItemType Directory -Force $dir | Out-Null
    (Resolve-Path -LiteralPath $dir).Path
}

function Get-OblivionSavedDir {
    if ($env:OBLIVION_SAVED_DIR) {
        return [pscustomobject]@{ Path = $env:OBLIVION_SAVED_DIR; Rule = 'OBLIVION_SAVED_DIR'; Exists = (Test-Path -LiteralPath $env:OBLIVION_SAVED_DIR) }
    }
    $sub = 'My Games\Oblivion Remastered\Saved'
    $candidates = New-Object System.Collections.Generic.List[object]
    $local = [Environment]::GetFolderPath('LocalApplicationData')
    if ($local) {
        $profileDocs = [IO.Path]::GetFullPath((Join-Path $local '..\..\Documents'))
        $candidates.Add([pscustomobject]@{ Path = (Join-Path $profileDocs $sub); Rule = 'LocalAppData\..\..\Documents (what the game uses)' })
    }
    $docs = [Environment]::GetFolderPath('MyDocuments')
    if ($docs) {
        $p = Join-Path $docs $sub
        if (-not ($candidates | Where-Object { $_.Path -eq $p })) { $candidates.Add([pscustomobject]@{ Path = $p; Rule = 'Documents known folder (fallback)' }) }
    }
    # The first candidate that has the game's config; else the first one (a game that never ran creates it there).
    $pick = $candidates | Where-Object { Test-Path -LiteralPath (Join-Path $_.Path 'Config\Windows') } | Select-Object -First 1
    if (-not $pick) { $pick = $candidates[0] }
    [pscustomobject]@{ Path = $pick.Path; Rule = $pick.Rule; Exists = (Test-Path -LiteralPath $pick.Path)
        Others = @($candidates | Where-Object { $_.Path -ne $pick.Path -and (Test-Path -LiteralPath $_.Path) } | ForEach-Object { $_.Path }) }
}

# Run directly (not dot-sourced): print what was found.
if ($MyInvocation.InvocationName -ne '.') {
    $g = Get-OblivionGame
    $s = Get-OblivionSavedDir
    "Game ($($g.Source)): $($g.GameDir)"
    "  exe:      $($g.Exe)"
    "  launch:   $($g.LaunchMethod)$(if ($g.SteamDll) { ' (marker: ' + [IO.Path]::GetFileName($g.SteamDll) + ')' })"
    if ($g.BuildId) { "  build id: $($g.BuildId) (Steam)" }
    "Saved: $($s.Path)"
    "  rule:     $($s.Rule)$(if (-not $s.Exists) { ' (does not exist yet: start the game once)' })"
    foreach ($o in $s.Others) { "  also exists (not used by the game; e.g. Steam Cloud's copy of the saves): $o" }
}
