<#
.SYNOPSIS
  Installs the locally built OblivionPerfFix as the game's version.dll proxy (takes effect at the
  next game start), or removes it (game closed: a loaded DLL is locked). The ini is only copied if none exists yet (-ResetIni overwrites it).
  The log is written next to the DLL: <Win64>\OblivionPerfFix.log

.EXAMPLE
  .\Install-PerfFix.ps1
  .\Install-PerfFix.ps1 -Uninstall
#>
param([switch]$Uninstall, [switch]$ResetIni)

. (Join-Path $PSScriptRoot 'GamePaths.ps1')
$win64 = (Get-OblivionGame).Win64
$src = Join-Path (Split-Path $PSScriptRoot) 'src\OblivionPerfFix'
$running = [bool](Get-Process OblivionRemastered-Win64-Shipping -ErrorAction SilentlyContinue)

$dll = Join-Path $win64 'version.dll'
$ini = Join-Path $win64 'OblivionPerfFix.ini'
if ($Uninstall) {
    if ($running) { throw 'Close the game first.' }
    foreach ($f in $dll, $ini, (Join-Path $win64 'OblivionPerfFix.log'), (Join-Path $win64 'OblivionPerfFix.1.log'), (Join-Path $win64 'OblivionPerfFix.2.log')) { if (Test-Path $f) { Remove-Item -LiteralPath $f; "removed $f" } }
    return
}
if ((Test-Path $dll) -and -not (Select-String -Path $dll -Pattern 'OBSEPlugin_Load' -SimpleMatch -Quiet)) {
    throw "$dll exists and is not OblivionPerfFix (another mod?). Not overwriting."
}
if ($running -and (Get-Process OblivionRemastered-Win64-Shipping).Modules.FileName -contains $dll) { throw 'The running game has this DLL loaded: close the game first.' }
Copy-Item (Join-Path $src 'build\OblivionPerfFix.dll') $dll -Force
"installed $dll"
if ($ResetIni -or -not (Test-Path $ini)) { Copy-Item (Join-Path $src 'OblivionPerfFix.ini') $ini -Force; "installed $ini" }
