@echo off
setlocal
set "PSModulePath=%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" %*
set "build_exit=%ERRORLEVEL%"
if not "%build_exit%"=="0" echo Build failed. See the message above.
pause
exit /b %build_exit%
