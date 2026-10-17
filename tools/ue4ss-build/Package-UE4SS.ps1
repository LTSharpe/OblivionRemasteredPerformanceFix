<#
.SYNOPSIS
  Makes the benchmark's UE4SS zip: the upstream release zip with dwmapi.dll and ue4ss\UE4SS.dll replaced by a local
  build (tools\ue4ss-build\README.md), plus LICENSE at the top, the fix patch, BUILD-INFO.txt and a .sha256 file.

.EXAMPLE
  tools\ue4ss-build\Package-UE4SS.ps1 -ReleaseZip third_party\UE4SS_v3.0.1-1161-g6eb3d9bc.zip -BinDir third_party\RE-UE4SS\build_vs\Game__Shipping__Win64\bin
#>
param(
    [Parameter(Mandatory)][string]$ReleaseZip,
    [Parameter(Mandatory)][string]$BinDir,
    [string]$Name = 'UE4SS_v3.0.1-1161-g6eb3d9bc-oblvfix1',
    [string]$OutDir,                 # default: third_party\ of this repo
    [string]$SourceInfo = 'UE4SS 6eb3d9bc (experimental 1161) + 0001-fix-Lua-release-out-param-registry-refs.patch (upstream issue #1446)',
    [string]$BuildInfo = ''
)
$ErrorActionPreference = 'Stop'
if (-not $OutDir) { $OutDir = Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'third_party' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stage = Join-Path ([IO.Path]::GetTempPath()) "$Name-stage"
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
[IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path $ReleaseZip).Path, $stage)
foreach ($f in 'dwmapi.dll', 'ue4ss\UE4SS.dll') { if (-not (Test-Path (Join-Path $stage $f))) { throw "$f not in ${ReleaseZip}: not the expected layout" } }

$upstream = @{}
foreach ($f in 'dwmapi.dll', 'ue4ss\UE4SS.dll') { $upstream[$f] = (Get-FileHash (Join-Path $stage $f) -Algorithm SHA256).Hash }
Copy-Item (Join-Path $BinDir 'dwmapi.dll') (Join-Path $stage 'dwmapi.dll') -Force
Copy-Item (Join-Path $BinDir 'UE4SS.dll') (Join-Path $stage 'ue4ss\UE4SS.dll') -Force
Copy-Item (Join-Path $stage 'ue4ss\LICENSE') (Join-Path $stage 'LICENSE') -Force
Copy-Item (Join-Path $PSScriptRoot '0001-fix-Lua-release-out-param-registry-refs.patch') $stage -Force
Set-Content (Join-Path $stage 'oblvbench-build.txt') $Name -Encoding ascii     # read by tools\Install-UE4SS.ps1

$lines = @(
    "$Name",
    '',
    "Source: $SourceInfo",
    "Upstream release zip: $(Split-Path $ReleaseZip -Leaf) (SHA-256 $((Get-FileHash $ReleaseZip -Algorithm SHA256).Hash))",
    $(if ($BuildInfo) { "Built with: $BuildInfo" }),
    'How to rebuild: tools/ue4ss-build/README.md in https://github.com/LTSharpe/OblivionRemasteredPerformanceFix',
    '',
    'Files that differ from the upstream zip (SHA-256 here / upstream):'
)
foreach ($f in 'dwmapi.dll', 'ue4ss\UE4SS.dll') {
    $lines += "  $f  $((Get-FileHash (Join-Path $stage $f) -Algorithm SHA256).Hash)  /  $($upstream[$f])"
}
$lines += 'Added: LICENSE (copy of ue4ss\LICENSE), 0001-fix-Lua-release-out-param-registry-refs.patch, oblvbench-build.txt (the name above, for tools/Install-UE4SS.ps1), BUILD-INFO.txt (this file).'
$lines | Where-Object { $null -ne $_ } | Set-Content (Join-Path $stage 'BUILD-INFO.txt') -Encoding utf8

New-Item -ItemType Directory -Force $OutDir | Out-Null
$zip = Join-Path $OutDir "$Name.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
# entries with forward slashes (ZipFile.CreateFromDirectory in .NET Framework writes backslashes)
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName) {
        $rel = $file.FullName.Substring($stage.Length).TrimStart('\').Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $archive.Dispose() }
Remove-Item -LiteralPath $stage -Recurse -Force
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
"$($hash.ToLower())  $Name.zip" | Set-Content "$zip.sha256" -Encoding ascii
[pscustomobject]@{ Zip = $zip; Sha256 = $hash; Size = (Get-Item $zip).Length }
