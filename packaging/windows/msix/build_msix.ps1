# build_msix.ps1 -- package ufb (ufb.exe + the tree its POST_BUILD steps stage next to it, plus the cargo-built
# ufb-agent.exe) as an MSIX for the Microsoft Store. Ported from minNotes packaging/windows/msix/build_msix.ps1.
#
# Invocation (preferred -- via CMake, from a VS x64 dev shell so the C++ runtime can be found):
#     cargo build --release --target x86_64-pc-windows-msvc --manifest-path agent/Cargo.toml
#     cmake --preset release
#     cmake --build --preset release --target ufb_msix
# Output: packaging\windows\msix\dist\ufb-<version>-x64.msix
#
# The staged set is the one installer/ufb_installer.iss packed, minus what the Store package must not or
# need not carry: winfsp.msi (a driver -- users install WinFsp themselves), open_union_link.ps1 (ufb.exe
# handles union:// now) and WinSparkle.dll (the Store owns updates).
#
# Signing
# -------
# Unset (the default) = an UNSIGNED package: the Microsoft Store artifact (Partner Center signs it on ingestion).
# Set these for a signed SIDELOAD package (the family's dev cert, same publisher CN as the manifest):
#     $env:UFB_SIGNING_PFX      = "C:\absolute\path\to\dev.pfx"    # ABSOLUTE: this script runs from another cwd
#     $env:UFB_SIGNING_PASSWORD = "..."
#     $env:UFB_TIMESTAMP_URL    = "http://timestamp.digicert.com"   # optional
# signtool's PFX import fails in a plain SSH session (0x80090010) -- sign from the logged-on desktop session
# (from the Mac: through the box's C:\qcb-lab\run.ps1 desktop runner).
#
# ASCII-only on purpose: Windows PowerShell 5.1 parses a BOM-less script in the system codepage.

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $BuildDir,
    [Parameter(Mandatory)] [string] $AgentExe,
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $ManifestPath,
    [Parameter()]          [string] $ImagesDir = (Join-Path $PSScriptRoot "images"),
    [Parameter()]          [string] $OutputDir = (Join-Path $PSScriptRoot "dist")
)

$ErrorActionPreference = "Stop"

# --- SDK tools --------------------------------------------------------------
function Find-SdkTool {
    param([string] $name)
    foreach ($root in @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "${env:ProgramFiles}\Windows Kits\10\bin")) {
        if (-not (Test-Path $root)) { continue }
        # Newest SDK first (the bin dir holds one folder per SDK version).
        $hits = Get-ChildItem -Path $root -Recurse -Filter $name -ErrorAction SilentlyContinue `
            | Where-Object { $_.FullName -match "\\x64\\" } | Sort-Object FullName -Descending
        if ($hits) { return ($hits | Select-Object -First 1).FullName }
    }
    return $null
}
$MakeAppx = Find-SdkTool "MakeAppx.exe"
$MakePri  = Find-SdkTool "MakePri.exe"
$SignTool = Find-SdkTool "signtool.exe"
if (-not $MakeAppx) { throw "MakeAppx.exe not found. Install the Windows 10/11 SDK." }
if (-not $MakePri)  { throw "MakePri.exe not found. Install the Windows 10/11 SDK." }
Write-Host "MakeAppx : $MakeAppx"
Write-Host "MakePri  : $MakePri"

# --- Inputs -----------------------------------------------------------------
$exe = Join-Path $BuildDir "ufb.exe"
if (-not (Test-Path $exe)) { throw "ufb.exe not found at $exe. Build first." }
if (-not (Test-Path (Join-Path $BuildDir "Qt6Core.dll"))) {
    throw "Qt6Core.dll missing from $BuildDir -- the windeployqt POST_BUILD step did not run."
}
if (-not (Test-Path $AgentExe)) {
    throw "ufb-agent.exe not found at $AgentExe. Run: cargo build --release --target x86_64-pc-windows-msvc --manifest-path agent/Cargo.toml"
}
# A stale agent is the classic mismatched-pair bug: warn when it predates the GUI by more than a day.
$agentAge = (Get-Item $exe).LastWriteTime - (Get-Item $AgentExe).LastWriteTime
if ($agentAge.TotalHours -gt 24) {
    Write-Warning ("ufb-agent.exe is {0:N0} h older than ufb.exe -- rebuild it if the agent changed." -f $agentAge.TotalHours)
}
if (-not (Test-Path $ManifestPath)) { throw "AppxManifest.xml not found at $ManifestPath. Re-run CMake configure." }

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")

# --- Stage ------------------------------------------------------------------
$staging = Join-Path $PSScriptRoot "staging"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Path $staging | Out-Null
Write-Host "Staging into $staging ..."

# Executables next to ufb.exe. The transcode / archive tools are resolved relative to the running exe
# (bindings/src/services/{transcode,archive}.rs); QtWebEngineProcess.exe + qt.conf back the HTML previews.
Copy-Item -Path $exe -Destination $staging
Copy-Item -Path $AgentExe -Destination $staging
foreach ($f in @("QtWebEngineProcess.exe", "qt.conf", "ffmpeg.exe", "ffprobe.exe", "exiftool.exe", "7z.exe")) {
    $src = Join-Path $BuildDir $f
    if (Test-Path $src) { Copy-Item -Path $src -Destination $staging }
    else { Write-Warning "$f not in $BuildDir -- the package ships without it" }
}

# Every runtime DLL beside the exe (Qt, ffmpeg, pdfium, 7z.dll, the vcpkg OpenEXR / libheif set). An old tree
# may still hold WinSparkle.dll from before 1.4.0 -- it must not ride along.
Get-ChildItem -Path $BuildDir -Filter "*.dll" | Where-Object { $_.Name -ne "WinSparkle.dll" } `
    | Copy-Item -Destination $staging

# Qt plugin / QML / WebEngine trees (windeployqt lays plugins out as top-level dirs), the project templates
# (NewJobDialog) and exiftool's Perl tree.
foreach ($subdir in @("qml", "platforms", "imageformats", "iconengines", "networkinformation", "tls",
                       "sqldrivers", "generic", "styles", "multimedia", "translations", "resources",
                       "templates", "exiftool_files")) {
    $src = Join-Path $BuildDir $subdir
    if (Test-Path $src) { Copy-Item -Recurse -Path $src -Destination (Join-Path $staging $subdir) }
}
foreach ($required in @("platforms\qwindows.dll", "sqldrivers\qsqlite.dll", "templates")) {
    if (-not (Test-Path (Join-Path $staging $required))) { throw "$required missing from the staged tree." }
}

# The MSVC C++ runtime, app-local: windeployqt runs with --no-compiler-runtime (the Inno installs leaned on a
# system-wide VC++ redistributable), and a packaged app -- ufb.exe and the Rust agent alike -- must not.
# VCToolsRedistDir comes from the VS dev shell.
$crt = $null
if ($env:VCToolsRedistDir) {
    $crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
}
if (-not $crt) { throw "The MSVC CRT redist folder was not found (VCToolsRedistDir='$env:VCToolsRedistDir'). Run from a VS x64 dev shell." }
Write-Host "C++ runtime: $($crt.FullName)"
Get-ChildItem -Path $crt.FullName -Filter "*.dll" | Copy-Item -Destination $staging -Force

# --- Manifest, tiles, icons, licences ----------------------------------------
Copy-Item -Path $ManifestPath -Destination (Join-Path $staging "AppxManifest.xml")
Copy-Item -Recurse -Path $ImagesDir -Destination (Join-Path $staging "images")
# main.cpp's window-icon fallback looks in <exe>\icons (the Qt resource normally wins).
$icons = New-Item -ItemType Directory -Path (Join-Path $staging "icons")
foreach ($f in @("32x32.png", "icon.ico")) { Copy-Item -Path (Join-Path $repoRoot "app\icons\$f") -Destination $icons }
Copy-Item -Path (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $staging "LICENSE.txt")
Copy-Item -Recurse -Path (Join-Path $repoRoot "LICENSES") -Destination (Join-Path $staging "LICENSES")

# --- resources.pri (the target-size / unplated qualifiers on the tile images) --
Write-Host "Generating resources.pri ..."
$priConfig = Join-Path $staging "priconfig.xml"
& $MakePri createconfig /cf $priConfig /dq "en-us" /o | Out-Null
& $MakePri new /pr $staging /cf $priConfig /mn (Join-Path $staging "AppxManifest.xml") /of (Join-Path $staging "resources.pri") /o
if ($LASTEXITCODE -ne 0) { throw "MakePri new failed (exit $LASTEXITCODE)" }
Remove-Item -Path $priConfig

# --- Pack -------------------------------------------------------------------
if (-not (Test-Path $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir | Out-Null }
$output = Join-Path $OutputDir "ufb-${Version}-x64.msix"
if (Test-Path $output) { Remove-Item -Path $output }
Write-Host "Packing $output ..."
& $MakeAppx pack /d $staging /p $output /o
if ($LASTEXITCODE -ne 0) { throw "MakeAppx pack failed (exit $LASTEXITCODE)" }
Remove-Item -Recurse -Force $staging

# --- Sign (sideload only) -----------------------------------------------------
$pfx = $env:UFB_SIGNING_PFX
if ($pfx) {
    if (-not (Test-Path $pfx)) { throw "UFB_SIGNING_PFX='$pfx' does not exist (it must be an ABSOLUTE path)." }
    if (-not $SignTool) { throw "UFB_SIGNING_PFX is set but signtool.exe was not found." }
    Write-Host "Signing $output ..."
    $signArgs = @("sign", "/fd", "SHA256", "/f", $pfx)
    if ($env:UFB_SIGNING_PASSWORD) { $signArgs += @("/p", $env:UFB_SIGNING_PASSWORD) }
    if ($env:UFB_TIMESTAMP_URL)    { $signArgs += @("/tr", $env:UFB_TIMESTAMP_URL, "/td", "SHA256") }
    $signArgs += $output
    & $SignTool @signArgs
    if ($LASTEXITCODE -ne 0) { throw "signtool sign failed (exit $LASTEXITCODE)" }
} else {
    Write-Host "UFB_SIGNING_PFX not set -- UNSIGNED package (the Store artifact)."
}

Write-Host ""
Write-Host "SUCCESS: $output"
