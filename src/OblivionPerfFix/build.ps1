<#
.SYNOPSIS
  Builds OblivionPerfFix.dll (x64, static CRT) with Visual Studio 2022 or later (the free Build Tools are enough).
  Output: build\OblivionPerfFix.dll (+ .pdb). Install it as <game>\...\Win64\version.dll
  or as OBSE\Plugins\OblivionPerfFix.dll (see tools\Install-PerfFix.ps1).

  Sources: every .cpp in the area folders under src\ (core, render, streaming, physics, npcs, loading, menus, framegen,
  guards, memory, diagnostics; a new folder is picked up on its own), plus src\core\proxy_stubs.asm. Headers are
  included as "common.h" (src\core is on the include path).

.PARAMETER SelfTest
  Also builds tests\selftest.cpp with the same sources (minus core\main.cpp and core\proxy.cpp) and runs it against
  the game exe (-Exe, else the Steam install found through tools\GamePaths.ps1 when it is next to this folder, else the
  self-test's built-in default path). It reads a mapped copy of the exe; the game is not started.

.PARAMETER OutDir
  Build folder (default: build next to this script). Use a scratch folder to build without touching build\.

.PARAMETER WaitForGame
  If Oblivion Remastered is running, wait until it is closed, then build (default: refuse with a message). A compile
  next to a running game makes it stutter, even at below-normal priority.

.PARAMETER IgnoreRunningGame
  Build even while Oblivion Remastered is running (also $env:OPF_BUILD_IGNORE_GAME=1).

.NOTES
  The compiler is found with vswhere (any VS 2022+ edition with the C++ x64 tools: Build Tools, Community,
  Professional, Enterprise as on GitHub's windows-2022 runners), else the Build Tools default path.
  $env:OPF_VCVARS overrides it with the path of a vcvars64.bat.
#>
param(
    [switch]$Debug,
    [switch]$SelfTest,
    [string]$Exe,
    [string]$OutDir,
    [switch]$WaitForGame,
    [switch]$IgnoreRunningGame
)
# Not 'Stop', also when the caller (or the CI shell) set it: vcvars writes a harmless vswhere warning to stderr, which
# Windows PowerShell would turn into a terminating error. Every step checks its exit code instead.
$ErrorActionPreference = 'Continue'

# A compile next to a game someone is playing makes the game stutter, even at below-normal priority (2026-10-26: a build
# during play tripled the frame-time jitter). So the build refuses while the game runs, or waits for it (-WaitForGame);
# -IgnoreRunningGame or $env:OPF_BUILD_IGNORE_GAME=1 builds anyway.
function Test-PlayerGameRunning {
    if (-not (Get-Process -Name 'OblivionRemastered-Win64-Shipping' -ErrorAction SilentlyContinue)) { return $false }
    return $true
}
if (-not ($IgnoreRunningGame -or $env:OPF_BUILD_IGNORE_GAME -eq '1') -and (Test-PlayerGameRunning)) {
    if (-not $WaitForGame) {
        Write-Host ('build: refused: Oblivion Remastered is running, and a compile now would make it stutter. Close the game ' +
            'and build again, or use -WaitForGame (builds once it is closed) or -IgnoreRunningGame (builds anyway; also ' +
            '$env:OPF_BUILD_IGNORE_GAME=1).')
        exit 3
    }
    Write-Host 'build: Oblivion Remastered is running: waiting until it is closed (-WaitForGame) ...'
    while (Test-PlayerGameRunning) { Start-Sleep -Seconds 15 }
    Write-Host 'build: the game was closed: building'
}

# The parallel compiles (cl /MP: one process per logical processor) run at below-normal priority, so a game running on
# the same PC keeps its processors (2026-10-26: builds during benchmark runs made 0.8-12 s bursts of 25-80 ms game
# frames). The child processes inherit the priority class; it is put back afterwards (also in a caller that runs this
# script in its own process). The output does not depend on it.
function Invoke-BelowNormal([scriptblock]$block) {
    $me = [Diagnostics.Process]::GetCurrentProcess(); $was = $me.PriorityClass
    $want = if ($env:OPF_BUILD_PRIORITY) { $env:OPF_BUILD_PRIORITY } else { 'BelowNormal' }   # e.g. Normal: as before
    try { $me.PriorityClass = $want } catch { }
    try { & $block } finally { try { $me.PriorityClass = $was } catch { } }
}

function Find-VcVars {
    if ($env:OPF_VCVARS) { return $env:OPF_VCVARS }
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
    throw 'Visual Studio 2022 or later with the C++ x64 build tools not found. Install the free "Build Tools for Visual Studio 2022" with the "Desktop development with C++" workload, or set OPF_VCVARS to a vcvars64.bat.'
}

# The source list: the area folders in name order, each folder's files in name order (the same order on every PC, so
# the same source gives the same DLL). Object files are named after the source file, so names must be unique.
function Get-Sources([string[]]$skip) {
    $srcRoot = Join-Path $PSScriptRoot 'src'
    $dirs = @(Get-ChildItem -LiteralPath $srcRoot -Directory | Where-Object { $_.Name -ne 'lab' } | Sort-Object Name)
    $files = foreach ($d in $dirs) {
        Get-ChildItem -LiteralPath $d.FullName -Filter '*.cpp' -File | Sort-Object Name |
            ForEach-Object { 'src\' + $d.Name + '\' + $_.Name } | Where-Object { $skip -notcontains $_ }
    }
    $dup = @($files | Group-Object { [IO.Path]::GetFileName($_) } | Where-Object Count -gt 1)
    if ($dup.Count) { throw "source file names must be unique (one object folder): $(($dup | ForEach-Object Name) -join ', ')" }
    return @($files)
}

$vcvars = Find-VcVars
Write-Host "vcvars: $vcvars"
$out = if ($OutDir) { [IO.Path]::GetFullPath($OutDir) } else { Join-Path $PSScriptRoot 'build' }
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null

$opt = if ($Debug) { '/Od /MTd' } else { '/O2 /MT /GL' }
$ltcg = if ($Debug) { '' } else { '/LTCG /CGTHREADS:8' }
$defs = ''
$sources = Get-Sources @()
# Link exactly the objects of this build (a folder used for another build may hold others).
$objs = (@('proxy_stubs.obj') + @($sources | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) + '.obj' }) |
    ForEach-Object { "`"$obj\$_`"" })
# The version resource (Explorer: Properties > Details), from the one version source: OPF_VERSION and OPF_RELEASE_SUFFIX
# in src\core\common.h (GitHub issue #27). Written as a .rc next to the objects and compiled with the SDK's rc.
function New-VersionResource([string]$rcPath, [string]$fileName, [string]$description, [string]$fileType) {
    $common = Join-Path $PSScriptRoot 'src\core\common.h'
    $ver = (Select-String -LiteralPath $common -Pattern '#define OPF_VERSION "([^"]+)"').Matches[0].Groups[1].Value
    $sfx = (Select-String -LiteralPath $common -Pattern '#define OPF_RELEASE_SUFFIX "([^"]*)"').Matches[0].Groups[1].Value
    if ($ver -notmatch '^\d+\.\d+\.\d+$') { throw "OPF_VERSION '$ver' is not major.minor.patch" }
    $num = ($ver -replace '\.', ',') + ',0'
    $text = @(
        '1 VERSIONINFO',
        "FILEVERSION $num",
        "PRODUCTVERSION $num",
        'FILEFLAGSMASK 0x3fL',
        'FILEFLAGS 0x0L',
        'FILEOS 0x40004L',
        "FILETYPE $fileType",
        'FILESUBTYPE 0x0L',
        'BEGIN',
        '  BLOCK "StringFileInfo"',
        '  BEGIN',
        '    BLOCK "040904b0"',
        '    BEGIN',
        '      VALUE "CompanyName", "LTSharpe and contributors"',
        "      VALUE `"FileDescription`", `"$description`"",
        "      VALUE `"FileVersion`", `"$ver$sfx`"",
        '      VALUE "InternalName", "OblivionPerfFix"',
        '      VALUE "LegalCopyright", "Copyright (c) 2026 LTSharpe and contributors. MIT License."',
        "      VALUE `"OriginalFilename`", `"$fileName`"",
        '      VALUE "ProductName", "Oblivion Remastered Performance Fix"',
        "      VALUE `"ProductVersion`", `"$ver$sfx`"",
        '      VALUE "Comments", "https://github.com/LTSharpe/OblivionRemasteredPerformanceFix"',
        '    END',
        '  END',
        '  BLOCK "VarFileInfo"',
        '  BEGIN',
        '    VALUE "Translation", 0x409, 1200',
        '  END',
        'END')
    [IO.File]::WriteAllLines($rcPath, [string[]]$text, [Text.Encoding]::ASCII)
}
New-VersionResource (Join-Path $obj 'version.rc') 'OblivionPerfFix.dll' 'Oblivion Remastered Performance Fix (OblivionPerfFix; installed as version.dll or as an OBSE64 plugin)' '0x2L'
$objs += "`"$obj\version.res`""
# The source and object lists go to cl and link in response files: on the command line they pass cmd's 8,191-character
# limit in a long checkout path (CI's D:\a\<repo>\<repo>\...).
$srcRsp = Join-Path $obj 'sources.rsp'
$objRsp = Join-Path $obj 'objects.rsp'
[IO.File]::WriteAllLines($srcRsp, [string[]]$sources)
[IO.File]::WriteAllLines($objRsp, [string[]]$objs)
$cmd = @(
    "call `"$vcvars`" >nul",
    "cd /d `"$PSScriptRoot`"",
    "ml64 /nologo /c /Fo`"$obj\proxy_stubs.obj`" src\core\proxy_stubs.asm",
    "rc /nologo /fo `"$obj\version.res`" `"$obj\version.rc`"",
    "cl /nologo /MP /std:c++20 $opt /EHsc /W4 /WX /permissive- /Zi /DUNICODE /D_UNICODE $defs /Isrc\core /c `"@$srcRsp`" /Fo`"$obj\\`" /Fd`"$obj\\`"",
    "link /nologo /DLL $ltcg /DEF:OblivionPerfFix.def /OUT:`"$out\OblivionPerfFix.dll`" /PDB:`"$out\OblivionPerfFix.pdb`" /DEBUG /OPT:REF /OPT:ICF `"@$objRsp`" kernel32.lib"
) -join ' && '
Invoke-BelowNormal { cmd /c $cmd }
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
Get-Item (Join-Path $out 'OblivionPerfFix.dll') | Select-Object Name, Length, LastWriteTime

if ($SelfTest) {
    # Offline test against a mapped copy of the game exe (see tests\selftest.cpp).
    if (-not $Exe) {
        $paths = Join-Path $PSScriptRoot '..\..\tools\GamePaths.ps1'
        if (Test-Path -LiteralPath $paths) {
            try { . $paths; $Exe = (Get-OblivionGame).Exe } catch { Write-Warning "GamePaths.ps1: $_" }
        }
    }
    $test = Join-Path $out 'test'
    New-Item -ItemType Directory -Force $test | Out-Null
    # The DLL's entry points (DllMain, the version.dll forwarders) are not part of the test; everything else is.
    $skip = @('src\core\main.cpp', 'src\core\proxy.cpp')
    $testRsp = Join-Path $test 'sources.rsp'
    [IO.File]::WriteAllLines($testRsp, [string[]](@('tests\selftest.cpp') + (Get-Sources $skip)))
    Invoke-BelowNormal { cmd /c "call `"$vcvars`" >nul && cd /d `"$PSScriptRoot`" && cl /nologo /MP /std:c++20 /O2 /MT /EHsc /W4 /WX /permissive- /DUNICODE /D_UNICODE $defs /Isrc\core `"@$testRsp`" /Fo`"$test\\`" /Fe`"$test\selftest.exe`" /link /DYNAMICBASE" }
    if ($LASTEXITCODE) { throw "selftest build failed ($LASTEXITCODE)" }
    Push-Location $test
    try {
        if ($Exe) { Write-Host "self-test against $Exe"; & .\selftest.exe $Exe } else { & .\selftest.exe }
        if ($LASTEXITCODE) { throw "selftest failed ($LASTEXITCODE)" }
    } finally { Pop-Location }
}
