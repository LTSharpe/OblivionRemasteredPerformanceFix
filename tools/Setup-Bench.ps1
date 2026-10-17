<#
.SYNOPSIS
  One-command setup of the benchmark suite (not needed to use the fix): gets the two third-party tools at pinned
  versions and checks their SHA-256, unpacks UE4SS, builds the benchmark's small window-hook DLL, and with -Install puts
  UE4SS and the benchmark mod into the game. Safe to run again: anything already in place and verified is kept.

  What it needs on the PC: Windows 10/11, Steam with Oblivion Remastered, and for the hook DLL the free Visual Studio 2022
  Build Tools (C++ workload). Nothing is installed system-wide; downloads go to third_party\ in this repo.

.PARAMETER Check
  Only report what is there and what is missing. Changes nothing, downloads nothing.
.PARAMETER Offline
  Never download: use files already in third_party\ (or given with -UE4SSZip / -PresentMonExe) and fail if one is missing.
.PARAMETER UE4SSZip
  Use this UE4SS zip instead of downloading it (it must have the pinned SHA-256 unless -AllowOtherUE4SS). Without it
  the script looks for the pinned file name in third_party\, then in your Downloads folder, and only then downloads.
  $env:OBLIVION_UE4SS_ZIP works the same way as this parameter.
.PARAMETER AllowOtherUE4SS
  Accept a UE4SS zip whose hash is not the pinned one (an upstream build, e.g. a newer experimental one). Untested
  with the benchmark mod: say so in any results you share. Upstream builds up to at least 1161 leak memory on every
  Lua call with an 'Out' parameter (the benchmark mod avoids such calls every frame since 2026-10-07).
.PARAMETER Install
  Kept for older instructions; there is nothing to install any more. The benchmark puts UE4SS and its mod into the
  game folder only while its own game runs (tools\Install-UE4SS.ps1 -Enable / -Disable; docs\benchmark.md, "UE4SS
  only during benchmark runs").

.EXAMPLE
  tools\Setup-Bench.ps1 -Check
  tools\Setup-Bench.ps1
  tools\Setup-Bench.ps1 -UE4SSZip $HOME\Downloads\UE4SS_v3.0.1-1161-g6eb3d9bc-oblvfix1.zip
#>
[CmdletBinding()]
param([switch]$Check, [switch]$Offline, [string]$UE4SSZip = $env:OBLIVION_UE4SS_ZIP, [string]$PresentMonExe, [switch]$AllowOtherUE4SS, [switch]$Install)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$third = Join-Path $root 'third_party'
. (Join-Path $PSScriptRoot 'GamePaths.ps1')

# Pinned versions: the ones the benchmark was built and tested with. Hashes from the copies used on the test PC.
$pins = @(
    [pscustomobject]@{
        Name   = 'PresentMon 2.6.0 (Intel, MIT licence): frame times'
        File   = 'PresentMon-2.6.0-x64.exe'
        Urls   = @('https://github.com/GameTechDev/PresentMon/releases/download/v2.6.0/PresentMon-2.6.0-x64.exe')
        Sha256 = 'B2A706BC6AD475749E3B7E3409263AA1E6906D45BDCF993F6DBC0F660188F1AF'
        Size   = 980320
        Local  = $PresentMonExe
    },
    [pscustomobject]@{
        Name   = 'UE4SS 3.0.1-1161 experimental with the Lua out-parameter leak fix (MIT licence): runs the benchmark mod inside the game'
        File   = 'UE4SS_v3.0.1-1161-g6eb3d9bc-oblvfix1.zip'
        # Upstream UE4SS 1161 (commit 6eb3d9bc, UE4SS-RE/RE-UE4SS experimental-latest of 2026-10-06) rebuilt with one fix:
        # upstream issue #1446, every Lua call of a UFunction with an 'Out' parameter leaked its table (~1 GB/h in our
        # sessions). How it is built and how to rebuild it: tools\ue4ss-build\README.md. Published as an asset of this
        # project's benchmark-tools release; the URLs are tried in order and the SHA-256 check makes any copy safe to use.
        # -AllowOtherUE4SS accepts an upstream zip instead (e.g. UE4SS_v3.0.1-1161-g6eb3d9bc.zip).
        # Release tag bench-tools-v1 (separate from the fix releases); until it is published, use a local copy (-UE4SSZip).
        Urls   = @('https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/releases/download/bench-tools-v1/UE4SS_v3.0.1-1161-g6eb3d9bc-oblvfix1.zip')
        Sha256 = 'C605C22AA02601CE5A98C6824FEB4BE88A286CB63A341EE976E024BB29549E84'
        Size   = 8692123
        Local  = $UE4SSZip
    }
)

$problems = New-Object System.Collections.Generic.List[string]
function Say([string]$s) { Write-Host $s }

function Get-DownloadsDir {
    # The Downloads known folder (it may have been moved, e.g. to OneDrive or another drive).
    try { (New-Object -ComObject Shell.Application).NameSpace('shell:Downloads').Self.Path } catch { $null }
}

function Get-Pinned($pin) {
    $dest = Join-Path $third $pin.File
    $downloads = Get-DownloadsDir
    $inDownloads = if ($downloads) { Join-Path $downloads $pin.File } else { $null }
    $candidate = if ($pin.Local) { $pin.Local }
                 elseif (Test-Path -LiteralPath $dest) { $dest }
                 elseif ($inDownloads -and (Test-Path -LiteralPath $inDownloads)) { Say "  found $inDownloads"; $inDownloads }
                 else { $null }
    if ($candidate) {
        if (-not (Test-Path -LiteralPath $candidate)) { $problems.Add("$($pin.Name): $candidate does not exist"); return }
        $hash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
        $ok = $hash -eq $pin.Sha256
        if (-not $ok -and -not ($AllowOtherUE4SS -and $pin.File -like 'UE4SS*')) {
            $problems.Add("$($pin.Name): $candidate has SHA-256 $hash, expected $($pin.Sha256)"); return
        }
        if (-not $ok) { Say "  WARNING: $($pin.Name): not the pinned build (SHA-256 $hash); untested with the benchmark mod" }
        if ($Check) { Say "  ok    $($pin.File)"; return $candidate }
        if ($candidate -ne $dest) {
            New-Item -ItemType Directory -Force $third | Out-Null
            Copy-Item -LiteralPath $candidate -Destination $dest -Force
        }
        Say "  ok    $($pin.File) (SHA-256 checked)"
        return $dest
    }
    if ($Check) { $problems.Add("$($pin.Name): missing ($($pin.File)); setup downloads it from $($pin.Urls[0])"); return }
    if ($Offline) { $problems.Add("$($pin.Name): missing in third_party and -Offline was given"); return }
    New-Item -ItemType Directory -Force $third | Out-Null
    $tmp = "$dest.download"
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $tried = @()
    foreach ($url in $pin.Urls) {
        Say "  get   $url ($([math]::Round($pin.Size / 1MB, 1)) MB)"
        try { Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing }
        catch { $tried += "$url (download failed: $($_.Exception.Message))"; continue }
        $hash = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash
        if ($hash -ne $pin.Sha256) {
            Remove-Item -LiteralPath $tmp -Force
            $tried += "$url (SHA-256 $hash, expected $($pin.Sha256); deleted)"
            continue
        }
        Move-Item -LiteralPath $tmp -Destination $dest -Force
        Say "  ok    $($pin.File) (downloaded, SHA-256 checked)"
        return $dest
    }
    $problems.Add("$($pin.Name): not available: $($tried -join '; '). Get $($pin.File) by hand and pass it with -UE4SSZip / -PresentMonExe (or put it in your Downloads folder)")
}

Say 'Oblivion Remastered'
try {
    $game = Get-OblivionGame
    Say "  ok    $($game.GameDir) (build $($game.BuildId))"
    $saved = Get-OblivionSavedDir
    if ($saved.Exists) { Say "  ok    config and saves: $($saved.Path)" }
    else { $problems.Add('the game has not been started yet: start it once and go through the first-start screens to the main menu') }
} catch { $problems.Add($_.Exception.Message) }

Say 'Third-party tools (pinned, SHA-256 checked)'
$presentMon = Get-Pinned $pins[0]
$ue4ssZip = Get-Pinned $pins[1]

Say 'UE4SS unpacked in third_party\ue4ss'
$ue4ssDir = Join-Path $third 'ue4ss'
# Which zip the unpacked copy came from (written here; tools\Install-UE4SS.ps1 records it in the game folder too)
$ue4ssMark = Join-Path $ue4ssDir 'oblvbench-build.txt'
$wantBuild = [IO.Path]::GetFileNameWithoutExtension($(if ($ue4ssZip) { $ue4ssZip } else { $pins[1].File }))
$haveBuild = if (Test-Path -LiteralPath $ue4ssMark) { (Get-Content -LiteralPath $ue4ssMark -TotalCount 1).Trim() }
             elseif (Test-Path -LiteralPath (Join-Path $ue4ssDir 'dwmapi.dll')) { 'an older build (no oblvbench-build.txt)' } else { $null }
if ($haveBuild -eq $wantBuild) { Say "  ok    already unpacked ($haveBuild)" }
elseif ($Check) { $problems.Add($(if ($haveBuild) { "third_party\ue4ss holds $haveBuild, not $wantBuild (setup replaces it)" } else { 'UE4SS is not unpacked yet (setup does it)' })) }
elseif ($ue4ssZip) {
    if ($haveBuild) {
        $old = "$ue4ssDir.replaced-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
        Move-Item -LiteralPath $ue4ssDir -Destination $old
        Say "  moved the previous copy ($haveBuild) to $old"
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($ue4ssZip, $ue4ssDir)
    Set-Content -LiteralPath $ue4ssMark $wantBuild -Encoding ascii
    Say "  ok    unpacked $wantBuild"
}

Say 'Benchmark window hook (tools\benchhook, built from source)'
$vcvars = & (Join-Path $PSScriptRoot 'Find-VCVars.ps1') 2>$null
if (-not $vcvars) { $problems.Add('Visual Studio 2022 C++ build tools not found: install "Build Tools for Visual Studio 2022" (Desktop development with C++). Without the hook the game window would show and take the focus during runs') }
elseif ($Check) { Say "  ok    compiler: $vcvars" }
else {
    try { $hook = & (Join-Path $PSScriptRoot 'benchhook\build.ps1') | Select-Object -Last 1; Say "  ok    $hook" }
    catch { $problems.Add("bench hook build failed: $($_.Exception.Message)") }
}

if ($game) {
    # Nothing to install: the benchmark puts UE4SS into the game only while its own game runs.
    try { Say "UE4SS in the game folder: $(& (Join-Path $PSScriptRoot 'Install-UE4SS.ps1') -Status -GameDir $game.GameDir) (the benchmark enables it only for its own games)" } catch { }
}

''
if ($problems.Count) {
    'Needs attention:'
    $problems | ForEach-Object { "  - $_" }
    'SETUP-RESULT incomplete'
    exit 1
}
if ($Check) { 'SETUP-RESULT ok (checked only)' }
else { 'SETUP-RESULT ok. Next: tools\Bench-Report.ps1 (docs\benchmark.md)' }
