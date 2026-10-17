<#
.SYNOPSIS
  Prints the path of vcvars64.bat of the newest Visual Studio 2022 (or later) install that has the C++ x64 tools:
  Build Tools, Community, Professional or Enterprise (GitHub's windows-2022 runners have Enterprise). Uses vswhere,
  which every Visual Studio installer puts in a fixed place; falls back to the Build Tools default path.
  Exit code 1 (and a message on stderr) if none is found.

.EXAMPLE
  $vcvars = & tools\Find-VCVars.ps1
  cmd /c "call `"$vcvars`" >nul && cl /?"
#>
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $paths = & $vswhere -latest -products * -version '[17.0,' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    foreach ($p in @($paths)) {
        if (-not $p) { continue }
        $bat = Join-Path $p 'VC\Auxiliary\Build\vcvars64.bat'
        if (Test-Path -LiteralPath $bat) { return $bat }
    }
}
$fallback = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (Test-Path -LiteralPath $fallback) { return $fallback }
[Console]::Error.WriteLine('Visual Studio 2022 with the C++ x64 build tools not found. Install the free "Build Tools for Visual Studio 2022" with the "Desktop development with C++" workload.')
exit 1
