@echo off
REM UFB Server Mode Watchdog Launcher
REM This starts the PowerShell watchdog script

echo Starting UFB Watchdog...
echo.
echo The watchdog will monitor ufb.exe and restart it if it crashes.
echo Press Ctrl+C to stop the watchdog.
echo.

powershell -ExecutionPolicy Bypass -File "%~dp0UFB_Watchdog.ps1"

pause
