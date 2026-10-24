@echo off
rem OblivionPerfFix benchmark kit: Is your graphics card or your CPU the limit? 30 s at the pond by Fort Doublecross in the rain, at your own settings, on screen (about 3-4 minutes). Don't touch the mouse or keyboard meanwhile.
rem Hidden and muted instead: run "Check-GPU.cmd -Hidden" from a command prompt in this folder.
setlocal
cd /d "%~dp0"
rem Lets PowerShell run this folder's scripts, for this window only (nothing is changed on the PC).
set "PSExecutionPolicyPreference=Bypass"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Bench-Kit.ps1" -Mode GpuCheck %*
set "rc=%errorlevel%"
echo.
if not defined OBLV_KIT_NO_PAUSE pause
exit /b %rc%
