<#
.SYNOPSIS
  Builds OblvBenchHook.dll (x64, static CRT) with Visual Studio 2022 or later (the free Build Tools are enough; found
  with vswhere by tools\Find-VCVars.ps1): benchmark tooling that keeps the
  game window off-screen, out of the taskbar and without the focus (see BenchHook.cpp). Not part of the fix.
  Output: build\OblvBenchHook.dll. BenchBridge.ps1 builds it on demand when the source is newer.
#>
param([switch]$Force)
# Not 'Stop', also when the caller set it: vcvars writes a harmless vswhere line to stderr, which Windows PowerShell
# would turn into a terminating error. The build checks its exit code instead.
$ErrorActionPreference = 'Continue'
$dll = Join-Path $PSScriptRoot 'build\OblvBenchHook.dll'
$src = Join-Path $PSScriptRoot 'BenchHook.cpp'
$newest = Get-ChildItem (Join-Path $PSScriptRoot 'build') -Filter 'OblvBenchHook*.dll' -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $Force -and $newest -and $newest.LastWriteTimeUtc -ge (Get-Item $src).LastWriteTimeUtc) { return $newest.FullName }
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
$dll
