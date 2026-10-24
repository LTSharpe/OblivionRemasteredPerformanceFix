<#
.SYNOPSIS
  One-command setup of the benchmark suite (not needed to use the fix): gets the two third-party tools and checks them,
  unpacks UE4SS, and makes sure the benchmark's small window-hook DLL is there (the benchmark kit ships it prebuilt;
  in a repository checkout it is built from source). Safe to run again: anything already in place and verified is kept.

  The tools:
  - PresentMon 2.6.0 (Intel, MIT licence), pinned, SHA-256 checked: frame times.
  - UE4SS (MIT licence), the official experimental build from its own project (github.com/UE4SS-RE/RE-UE4SS, release
    "experimental-latest"): runs the benchmark mod inside the game. Build 1181 or newer (the first with the Lua memory
    fix the benchmark needs, upstream PR #1452). Upstream replaces that release's zip with each new build, so setup
    takes the newest UE4SS_v3.0.1-<build>-g<commit>.zip there (never the zDEV- one), checks it against the SHA-256
    GitHub lists for it, and records build and SHA-256 (the report shows them, "tested build" for the 1181 zip this
    benchmark was tested with, otherwise "newer than tested"). Once unpacked, it is kept until -UpdateUE4SS.

  What it needs on the PC: Windows 10/11, Steam with Oblivion Remastered. In a repository checkout without a built hook
  DLL also the free Visual Studio 2022 Build Tools (C++ workload). Nothing is installed system-wide; downloads go to
  third_party\ in this folder.

.PARAMETER Check
  Only report what is there and what is missing. Changes nothing, downloads nothing.
.PARAMETER Offline
  Never download: use files already in third_party\ (or given with -UE4SSZip / -PresentMonExe) and fail if one is missing.
.PARAMETER UE4SSZip
  Use this UE4SS zip (an official UE4SS_v3.0.1-<build>-g<commit>.zip, build 1181 or newer) instead of downloading it.
  Without it the script uses the newest such zip in third_party\, then in your Downloads folder, and only then
  downloads. $env:OBLIVION_UE4SS_ZIP works the same way as this parameter.
.PARAMETER AllowOtherUE4SS
  Accept a UE4SS zip whose name is not an official build of 1181 or newer (a local build, or an older one). Untested
  with the benchmark mod: say so in any results you share. Builds before 1181 leak memory on every Lua call with an
  'Out' parameter (upstream issue #1446).
.PARAMETER UpdateUE4SS
  Look for a newer official UE4SS build even if one is already unpacked, and switch to it.
.PARAMETER Install
  Kept for older instructions; there is nothing to install any more. The benchmark puts UE4SS and its mod into the
  game folder only while its own game runs (tools\Install-UE4SS.ps1 -Enable / -Disable; docs\benchmark.md, "UE4SS
  only during benchmark runs").

.EXAMPLE
  tools\Setup-Bench.ps1 -Check
  tools\Setup-Bench.ps1
  tools\Setup-Bench.ps1 -UE4SSZip $HOME\Downloads\UE4SS_v3.0.1-1181-g83330635.zip
#>
[CmdletBinding()]
param([switch]$Check, [switch]$Offline, [string]$UE4SSZip = $env:OBLIVION_UE4SS_ZIP, [string]$PresentMonExe, [switch]$AllowOtherUE4SS,
      [switch]$UpdateUE4SS, [switch]$Install)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$third = Join-Path $root 'third_party'
. (Join-Path $PSScriptRoot 'GamePaths.ps1')
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

# PresentMon: pinned. Hash from the copy used on the test PC.
$presentMonPin = [pscustomobject]@{
    Name   = 'PresentMon 2.6.0 (Intel, MIT licence): frame times'
    File   = 'PresentMon-2.6.0-x64.exe'
    Urls   = @('https://github.com/GameTechDev/PresentMon/releases/download/v2.6.0/PresentMon-2.6.0-x64.exe')
    Sha256 = 'B2A706BC6AD475749E3B7E3409263AA1E6906D45BDCF993F6DBC0F660188F1AF'
    Size   = 980320
    Local  = $PresentMonExe
}
# UE4SS: the official experimental build, 1181 or newer. 1181 is the build the benchmark mod was tested with.
$ue4ssRepo = 'UE4SS-RE/RE-UE4SS'
$ue4ssTag = 'experimental-latest'
$ue4ssMinBuild = 1181
$ue4ssTested = @{ Build = 1181; File = 'UE4SS_v3.0.1-1181-g83330635.zip'; Sha256 = 'B9A166A756605B21058A64CDE93BCAAB1A855DBA3553E5C7E6CE0FA8FDFAD7B9'; Size = 8748315 }
$ue4ssNameRx = '^UE4SS_v3\.0\.1-(\d+)-g[0-9a-f]+\.zip$'

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
        if ($hash -ne $pin.Sha256) { $problems.Add("$($pin.Name): $candidate has SHA-256 $hash, expected $($pin.Sha256)"); return }
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
    $problems.Add("$($pin.Name): not available: $($tried -join '; '). Get $($pin.File) by hand and pass it with -PresentMonExe (or put it in your Downloads folder)")
}

function Get-UE4SSBuild([string]$name) { if ($name -match $ue4ssNameRx) { [int]$Matches[1] } else { 0 } }

<# The newest official UE4SS zip (build >= 1181) in a folder, or $null. #>
function Find-UE4SSZip([string]$dir) {
    if (-not $dir -or -not (Test-Path -LiteralPath $dir)) { return $null }
    Get-ChildItem -LiteralPath $dir -File -Filter 'UE4SS_v3.0.1-*.zip' -ErrorAction SilentlyContinue |
        Where-Object { (Get-UE4SSBuild $_.Name) -ge $ue4ssMinBuild } | Sort-Object { Get-UE4SSBuild $_.Name } -Descending | Select-Object -First 1
}

<# Downloads the newest official zip from the experimental-latest release into third_party. Returns its path or $null. #>
function Get-UE4SSDownload {
    $asset = $null
    try {
        $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$ue4ssRepo/releases/tags/$ue4ssTag" -UseBasicParsing -Headers @{ 'User-Agent' = 'OblivionPerfFix-benchmark-setup' }
        $asset = @($rel.assets | Where-Object { $_.name -match $ue4ssNameRx -and (Get-UE4SSBuild $_.name) -ge $ue4ssMinBuild } |
                   Sort-Object { Get-UE4SSBuild $_.name } -Descending)[0]
        if (-not $asset) { $problems.Add("UE4SS: the release $ue4ssTag of $ue4ssRepo has no UE4SS_v3.0.1-<build>-g<commit>.zip of build $ue4ssMinBuild or newer"); return $null }
    } catch {
        # GitHub's API unreachable or rate-limited: try the tested zip's own address (it is gone once upstream replaced it)
        Say "  GitHub's release list not readable ($($_.Exception.Message)); trying the tested build's address"
        $asset = [pscustomobject]@{ name = $ue4ssTested.File; size = $ue4ssTested.Size; digest = "sha256:$($ue4ssTested.Sha256)"
                                    browser_download_url = "https://github.com/$ue4ssRepo/releases/download/$ue4ssTag/$($ue4ssTested.File)" }
    }
    $url = "$($asset.browser_download_url)"
    if ($url -notmatch '^https://github\.com/') { $problems.Add("UE4SS: unexpected download address $url"); return $null }
    New-Item -ItemType Directory -Force $third | Out-Null
    $dest = Join-Path $third $asset.name
    $tmp = "$dest.download"
    Say "  get   $url ($([math]::Round([double]$asset.size / 1MB, 1)) MB)"
    try { Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing }
    catch { $problems.Add("UE4SS: download failed ($($_.Exception.Message)). Download $($asset.name) from https://github.com/$ue4ssRepo/releases/tag/$ue4ssTag into your Downloads folder and run this again"); return $null }
    $hash = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash
    $want = if ("$($asset.digest)" -match '^sha256:([0-9a-fA-F]{64})$') { $Matches[1].ToUpper() } else { $null }
    if ($want -and $hash -ne $want) {
        Remove-Item -LiteralPath $tmp -Force
        $problems.Add("UE4SS: $($asset.name) has SHA-256 $hash, but GitHub lists $want for it (deleted). Try again later"); return $null
    }
    Move-Item -LiteralPath $tmp -Destination $dest -Force
    Say "  ok    $($asset.name) (downloaded$(if ($want) { ', SHA-256 matches GitHub''s' }))"
    $dest
}

Say 'Oblivion Remastered'
try {
    $game = Get-OblivionGame
    Say "  ok    $($game.GameDir) (build $($game.BuildId))"
    $saved = Get-OblivionSavedDir
    if ($saved.Exists) { Say "  ok    config and saves: $($saved.Path)" }
    else { $problems.Add('the game has not been started yet: start it once and go through the first-start screens to the main menu') }
} catch { $problems.Add($_.Exception.Message) }

Say 'PresentMon (pinned, SHA-256 checked)'
$presentMon = Get-Pinned $presentMonPin

Say "UE4SS (official experimental build $ue4ssMinBuild or newer, from github.com/$ue4ssRepo)"
$ue4ssDir = Join-Path $third 'ue4ss'
# Which zip the unpacked copy came from: line 1 the build name (tools\Install-UE4SS.ps1 records it in the game folder too),
# then sha256=, tested= (Bench-Report shows them)
$ue4ssMark = Join-Path $ue4ssDir 'oblvbench-build.txt'
$haveBuild = if (Test-Path -LiteralPath $ue4ssMark) { (Get-Content -LiteralPath $ue4ssMark -TotalCount 1).Trim() }
             elseif (Test-Path -LiteralPath (Join-Path $ue4ssDir 'dwmapi.dll')) { 'an older build (no oblvbench-build.txt)' } else { $null }
$haveOk = $haveBuild -and ((Get-UE4SSBuild "$haveBuild.zip") -ge $ue4ssMinBuild -or ($AllowOtherUE4SS -and $haveBuild -notlike 'an older*'))
$ue4ssZip = $null
if ($UE4SSZip) {
    if (-not (Test-Path -LiteralPath $UE4SSZip)) { $problems.Add("UE4SS: $UE4SSZip does not exist") }
    elseif ((Get-UE4SSBuild (Split-Path $UE4SSZip -Leaf)) -lt $ue4ssMinBuild -and -not $AllowOtherUE4SS) {
        $problems.Add("UE4SS: $(Split-Path $UE4SSZip -Leaf) is not an official build $ue4ssMinBuild or newer (UE4SS_v3.0.1-<build>-g<commit>.zip); -AllowOtherUE4SS accepts it anyway")
    } else { $ue4ssZip = (Resolve-Path -LiteralPath $UE4SSZip).Path }
} elseif ($haveOk -and -not $UpdateUE4SS) {
    Say "  ok    already unpacked ($haveBuild)"
} else {
    $found = Find-UE4SSZip $third
    $dl = Find-UE4SSZip (Get-DownloadsDir)
    if ($dl -and (-not $found -or (Get-UE4SSBuild $dl.Name) -gt (Get-UE4SSBuild $found.Name))) { Say "  found $($dl.FullName)"; $found = $dl }
    if ($found -and -not $UpdateUE4SS) { $ue4ssZip = $found.FullName }
    elseif ($Check) { $problems.Add($(if ($found) { "UE4SS: $($found.Name) found; -UpdateUE4SS would look for a newer one" } else { "UE4SS is not there yet (setup downloads the newest official build from github.com/$ue4ssRepo)" })) }
    elseif ($Offline) { if ($found) { $ue4ssZip = $found.FullName } else { $problems.Add('UE4SS: no UE4SS_v3.0.1-*.zip (build 1181 or newer) in third_party or Downloads, and -Offline was given') } }
    else {
        $ue4ssZip = Get-UE4SSDownload
        if (-not $ue4ssZip -and $found) { $ue4ssZip = $found.FullName }
    }
}
if ($ue4ssZip) {
    $wantBuild = [IO.Path]::GetFileNameWithoutExtension($ue4ssZip)
    $hash = (Get-FileHash -LiteralPath $ue4ssZip -Algorithm SHA256).Hash
    $tested = if ($hash -eq $ue4ssTested.Sha256) { 'tested build' } else { 'newer than tested' }
    if ((Get-UE4SSBuild "$wantBuild.zip") -lt $ue4ssMinBuild) { $tested = 'not an official build 1181 or newer: untested' }
    if ($haveBuild -eq $wantBuild -and -not $UpdateUE4SS) { Say "  ok    already unpacked ($haveBuild)" }
    elseif ($Check) { $problems.Add($(if ($haveBuild) { "third_party\ue4ss holds $haveBuild, not $wantBuild (setup replaces it)" } else { 'UE4SS is not unpacked yet (setup does it)' })) }
    elseif ($haveBuild -eq $wantBuild) { Say "  ok    $wantBuild is the newest; already unpacked" }
    else {
        if (Test-Path -LiteralPath $ue4ssDir) {
            $old = "$ue4ssDir.replaced-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
            Move-Item -LiteralPath $ue4ssDir -Destination $old
            Say "  moved the previous copy ($haveBuild) to $old"
        }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($ue4ssZip, $ue4ssDir)
        if ((Split-Path $ue4ssZip) -ne $third) { Copy-Item -LiteralPath $ue4ssZip -Destination $third -Force }
        Say "  ok    unpacked $wantBuild ($tested; SHA-256 $hash)"
    }
    if (-not $Check -and (Test-Path -LiteralPath $ue4ssDir)) {
        $mark = @($wantBuild, "sha256=$hash", "tested=$tested", "source=https://github.com/$ue4ssRepo/releases/tag/$ue4ssTag")
        if ((Test-Path -LiteralPath $ue4ssMark) -and ((Get-Content -LiteralPath $ue4ssMark) -join '|') -eq ($mark -join '|')) { } else { Set-Content -LiteralPath $ue4ssMark $mark -Encoding ascii }
    }
    if ($tested -ne 'tested build') { Say "  NOTE  $wantBuild is $tested with the benchmark mod (tested: $($ue4ssTested.File)); the report says so" }
}

Say 'Benchmark window hook (tools\benchhook)'
$hookDll = Join-Path $PSScriptRoot 'benchhook\build\OblvBenchHook.dll'
$prebuilt = & (Join-Path $PSScriptRoot 'benchhook\build.ps1') -Prebuilt 2>$null
if ($prebuilt) { Say "  ok    $prebuilt" }
else {
    $vcvars = & (Join-Path $PSScriptRoot 'Find-VCVars.ps1') 2>$null
    if (-not $vcvars) { $problems.Add('the window hook DLL is not there and Visual Studio 2022 C++ build tools were not found to build it: use the benchmark kit (OblivionPerfFix-<version>-Benchmark.zip, it has the DLL built), or install "Build Tools for Visual Studio 2022" (Desktop development with C++). Without the hook the game window would show and take the focus during runs') }
    elseif ($Check) { Say "  ok    compiler: $vcvars (setup builds the DLL)" }
    else {
        try { $hook = & (Join-Path $PSScriptRoot 'benchhook\build.ps1') | Select-Object -Last 1; Say "  ok    $hook" }
        catch { $problems.Add("bench hook build failed: $($_.Exception.Message)") }
    }
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
