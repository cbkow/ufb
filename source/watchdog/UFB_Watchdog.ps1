# UFB Server Mode Watchdog
# Monitors ufb.exe and restarts it if it crashes or stops running

param(
    [string]$ExePath = "C:\Program Files\ufb\ufb.exe",
    [int]$CheckIntervalSeconds = 30,
    [string]$LogPath = ".\watchdog.log"
)

function Write-Log {
    param([string]$Message)
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    $logMessage = "[$timestamp] $Message"
    Write-Host $logMessage
    Add-Content -Path $LogPath -Value $logMessage
}

function Test-ProcessRunning {
    param([string]$ProcessName)
    $process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
    return $null -ne $process
}

function Start-UFB {
    param([string]$ExePath)

    if (-not (Test-Path $ExePath)) {
        Write-Log "ERROR: Executable not found at: $ExePath"
        return $false
    }

    try {
        $workingDir = Split-Path -Parent $ExePath
        $process = Start-Process -FilePath $ExePath -WorkingDirectory $workingDir -PassThru
        Write-Log "Started ufb.exe (PID: $($process.Id))"
        return $true
    }
    catch {
        Write-Log "ERROR: Failed to start ufb.exe: $_"
        return $false
    }
}

# Main watchdog loop
Write-Log "=== UFB Watchdog Started ==="
Write-Log "Executable: $ExePath"
Write-Log "Check Interval: $CheckIntervalSeconds seconds"
Write-Log "Press Ctrl+C to stop watchdog"
Write-Host ""

# Initial start
if (-not (Test-ProcessRunning "ufb")) {
    Write-Log "UFB not running, starting initial instance..."
    Start-UFB -ExePath $ExePath
}
else {
    Write-Log "UFB already running"
}

# Monitor loop
while ($true) {
    Start-Sleep -Seconds $CheckIntervalSeconds

    if (-not (Test-ProcessRunning "ufb")) {
        Write-Log "WARNING: UFB process not found, restarting..."
        Start-UFB -ExePath $ExePath
    }
}
