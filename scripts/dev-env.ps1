# scripts/dev-env.ps1
#
# Dot-source this to load the Windows build environment into the current
# PowerShell session:
#
#     . scripts/dev-env.ps1
#
# It imports the MSVC toolchain (vcvars64) and prepends Qt's msvc2022_64
# kit, plus Qt's bundled CMake and Ninja, to PATH. After sourcing,
# `cmake --preset release`, `ninja`, `windeployqt`, `cl`, and `signtool`
# all resolve. Idempotent within a session (guards on $env:UFB_DEVENV).
#
# Override locations with these env vars before sourcing if your install
# differs:
#   $env:UFB_QT_KIT   - Qt msvc kit root (default C:\Qt\6.11.1\msvc2022_64)
#   $env:UFB_QT_TOOLS - Qt Tools root    (default C:\Qt\Tools)
#   $env:VCPKG_ROOT   - vcpkg root        (default C:\vcpkg)

$ErrorActionPreference = 'Stop'

if ($env:UFB_DEVENV -eq '1') {
    Write-Host "[dev-env] already loaded in this session"
    return
}

$qtKit   = if ($env:UFB_QT_KIT)   { $env:UFB_QT_KIT }   else { 'C:\Qt\6.11.1\msvc2022_64' }
$qtTools = if ($env:UFB_QT_TOOLS) { $env:UFB_QT_TOOLS } else { 'C:\Qt\Tools' }
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = 'C:\vcpkg' }

foreach ($p in @($qtKit, $qtTools)) {
    if (-not (Test-Path $p)) { throw "[dev-env] path not found: $p (set the matching UFB_QT_* env var)" }
}

# --- MSVC: import vcvars64.bat's environment into this PS session --------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "[dev-env] vswhere not found - is Visual Studio 2022 installed?" }
$vsRoot = & $vswhere -latest -products * -property installationPath
if (-not $vsRoot) { throw "[dev-env] no Visual Studio installation found via vswhere" }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "[dev-env] vcvars64.bat not found at $vcvars" }

Write-Host "[dev-env] loading MSVC env from $vsRoot ..."
# Run vcvars in a child cmd, then dump the resulting environment back to us.
$lines = cmd /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $lines) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path ("env:{0}" -f $matches[1]) -Value $matches[2]
    }
}

# --- Qt kit + Qt-bundled CMake/Ninja onto PATH ---------------------------
$cmakeBin = Join-Path $qtTools 'CMake_64\bin'
$ninjaBin = Join-Path $qtTools 'Ninja'
$qtBin    = Join-Path $qtKit   'bin'
$env:PATH = "$qtBin;$cmakeBin;$ninjaBin;$env:PATH"

# Help find_package(Qt6) locate the kit (the presets toolchain handles
# vcpkg; Qt is found via prefix path).
$env:CMAKE_PREFIX_PATH = if ($env:CMAKE_PREFIX_PATH) { "$qtKit;$env:CMAKE_PREFIX_PATH" } else { $qtKit }

# winfsp-sys' bindgen step (ufb-agent build) needs libclang. Override the
# LLVM root with $env:UFB_LLVM_ROOT before sourcing if it lives elsewhere.
$llvmBin = if ($env:UFB_LLVM_ROOT) { Join-Path $env:UFB_LLVM_ROOT 'bin' } else { 'C:\Program Files\LLVM\bin' }
if (Test-Path (Join-Path $llvmBin 'libclang.dll')) {
    $env:LIBCLANG_PATH = $llvmBin
} else {
    Write-Host "[dev-env] WARN: libclang.dll not found at $llvmBin - ufb-agent (cargo) build will fail bindgen"
}

$env:UFB_DEVENV = '1'

Write-Host "[dev-env] ready:"
foreach ($t in 'cmake','ninja','windeployqt','cl','signtool') {
    $c = Get-Command $t -ErrorAction SilentlyContinue
    Write-Host ("  {0,-12} {1}" -f $t, $(if ($c) { $c.Source } else { 'NOT FOUND' }))
}
