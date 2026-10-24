<#
.SYNOPSIS
  Builds OblvBenchHook.dll (x64, static CRT) with Visual Studio 2022 or later (the free Build Tools are enough; found
  with vswhere by tools\Find-VCVars.ps1): benchmark tooling that keeps the
  game window off-screen, out of the taskbar and without the focus (see BenchHook.cpp). Not part of the fix.
  Output: build\OblvBenchHook.dll. BenchBridge.ps1 builds it on demand when the source is newer.

  A DLL built from the current source is used as it is, without a compiler: build\OblvBenchHook.source-sha256.txt
  (written by every build) holds the SHA-256 of the BenchHook.cpp it was built from. The benchmark kit ships the DLL
  built by the release workflow this way (file times from a zip prove nothing).

.PARAMETER Prebuilt
  Only return a usable DLL that is already there (newer than the source, or built from the same source); never build.
  Returns nothing if there is none.
#>
param([switch]$Force, [switch]$Prebuilt)
# Not 'Stop', also when the caller set it: vcvars writes a harmless vswhere line to stderr, which Windows PowerShell
# would turn into a terminating error. The build checks its exit code instead.
$ErrorActionPreference = 'Continue'
$dll = Join-Path $PSScriptRoot 'build\OblvBenchHook.dll'
$src = Join-Path $PSScriptRoot 'BenchHook.cpp'
$mark = Join-Path $PSScriptRoot 'build\OblvBenchHook.source-sha256.txt'
$srcHash = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
$newest = Get-ChildItem (Join-Path $PSScriptRoot 'build') -Filter 'OblvBenchHook*.dll' -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $Force -and $newest) {
    $sameSource = (Test-Path -LiteralPath $mark) -and ((Get-Content -LiteralPath $mark -TotalCount 1).Trim() -eq $srcHash) -and (Test-Path -LiteralPath $dll)
    if ($sameSource) { return $(if ($newest.LastWriteTimeUtc -ge (Get-Item $dll).LastWriteTimeUtc) { $newest.FullName } else { $dll }) }
    if ($newest.LastWriteTimeUtc -ge (Get-Item $src).LastWriteTimeUtc) { return $newest.FullName }
}
if ($Prebuilt) { return }
$vcvars = & (Join-Path (Split-Path $PSScriptRoot) 'Find-VCVars.ps1') 2>$null
if (-not $vcvars -or -not (Test-Path -LiteralPath $vcvars)) { throw "Visual Studio 2022 C++ build tools not found (tools\Find-VCVars.ps1): cannot build $dll" }
New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot 'build\obj') | Out-Null
# A game that still has the old DLL loaded (it pins itself) keeps the file locked: build under a new name then.
$out = $dll
if (Test-Path $dll) {
    try { [IO.File]::Open($dll, 'Open', 'ReadWrite', 'None').Dispose() }
    catch { $out = Join-Path $PSScriptRoot ('build\OblvBenchHook-{0:yyyyMMddHHmmss}.dll' -f (Get-Date)) }
}
$cmd = @(
    "call `"$vcvars`" >nul",
    "cd /d `"$PSScriptRoot`"",
    "cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /WX /permissive- /DUNICODE /D_UNICODE /LD BenchHook.cpp /Fobuild\obj\ /Fe`"$out`" /link /NOLOGO user32.lib"
) -join ' && '
$log = cmd /c $cmd 2>&1
if ($LASTEXITCODE) { throw "OblvBenchHook build failed ($LASTEXITCODE): $($log -join "`n")" }
if ($out -ne $dll) { Write-Warning "$dll is in use (a game still has it loaded): built $out instead"; return $out }
Set-Content -LiteralPath $mark $srcHash -Encoding ascii
$dll
