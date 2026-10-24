@echo off
rem OblivionPerfFix benchmark kit: The same as Benchmark.cmd, but the game runs hidden and muted, so you can keep using the PC (about 15-20 minutes). Ends with a report you can share.
setlocal
cd /d "%~dp0"
rem Lets PowerShell run this folder's scripts, for this window only (nothing is changed on the PC).
set "PSExecutionPolicyPreference=Bypass"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Bench-Kit.ps1" -Mode Benchmark -Hidden %*
set "rc=%errorlevel%"
echo.
if not defined OBLV_KIT_NO_PAUSE pause
exit /b %rc%
