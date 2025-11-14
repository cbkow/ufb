@echo off
REM UFB Server Mode Watchdog Stopper
REM This stops all running PowerShell watchdog instances

echo Stopping UFB Watchdog...
echo.

REM Kill any PowerShell processes running the watchdog script
taskkill /FI "WINDOWTITLE eq Administrator: UFB_Watchdog.ps1*" /F 2>nul
taskkill /FI "WINDOWTITLE eq UFB_Watchdog.ps1*" /F 2>nul

REM Alternative: Kill by process name if window title doesn't work
for /f "tokens=2" %%a in ('tasklist /FI "IMAGENAME eq powershell.exe" /FO LIST ^| find "PID:"') do (
    wmic process where "ProcessId=%%a AND CommandLine LIKE '%%UFB_Watchdog.ps1%%'" delete 2>nul
)

echo.
echo Watchdog stopped (if it was running)
echo.
pause
