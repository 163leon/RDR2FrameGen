@echo off
rem Double-click installer: runs install.ps1 from this folder. Arguments are passed through.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
