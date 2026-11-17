@echo off
setlocal
set "PSModulePath=%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
set "setup_exit=%ERRORLEVEL%"
if not "%setup_exit%"=="0" echo Setup failed. See the message above.
pause
exit /b %setup_exit%
