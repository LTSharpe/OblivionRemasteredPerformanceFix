@echo off
rem OblivionPerfFix benchmark kit: The Imperial City flight, the game without and with the fix, at your own settings, on screen and with sound so you can see and feel the difference (about 15-20 minutes). Ends with a report you can share.
rem Double-click it, then don't touch the mouse or keyboard or switch windows until the report opens. More rounds (less noise): run "Benchmark.cmd -Rounds 3" from a command prompt in this folder.
setlocal
cd /d "%~dp0"
rem Lets PowerShell run this folder's scripts, for this window only (nothing is changed on the PC).
set "PSExecutionPolicyPreference=Bypass"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Bench-Kit.ps1" -Mode Benchmark %*
set "rc=%errorlevel%"
echo.
if not defined OBLV_KIT_NO_PAUSE pause
exit /b %rc%
