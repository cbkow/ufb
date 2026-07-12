# scripts/setup-external.ps1
#
# Populates UFB/external/ from sister projects so the build can find
# vendored prebuilt libraries without committing ~150 MB of DLLs to
# git. Run once per fresh clone, or whenever QCView-Player updates
# its prebuilts.
#
# Source of truth for ffmpeg + OpenEXR: the QCView-Player repo cloned as a
# sibling next to ufb/ (i.e. <GitHub>/QCView-Player/external/), which already
# vendors known-good prebuilt Windows binaries. Override with $env:QCVIEW_ROOT.
#
# Source of truth for exiftool + ffmpeg.exe / ffprobe.exe (the runtime
# binaries the transcoder shells out to): the legacy Tauri build cloned as a
# sibling (<GitHub>/ufb-tauri/src-tauri/external/) - kept alongside QCView so we
# don't redownload ~35MB of exiftool's Perl runtime tree on every fresh clone.
# Override with $env:UFB_TAURI_ROOT.
#
# What gets copied/downloaded:
#   external/ffmpeg/include/  ffmpeg headers (from QCView)
#   external/ffmpeg/lib/      import libs (.lib + .def)
#   external/ffmpeg/bin/      runtime DLLs (avcodec, avformat, avutil,
#                             swscale, swresample) + ffmpeg.exe and
#                             ffprobe.exe - the transcoder spawns them
#                             as subprocesses for the actual encode and
#                             frame-count probe steps.
#   external/exiftool/        exiftool.exe + exiftool_files\ (its Perl
#                             runtime). Used by the transcode pipeline
#                             to copy metadata onto the output MP4.
#                             Pulled from ufb-tauri's external tree.
#   external/pdfium/          bblanchon prebuilt - downloaded from
#                             https://github.com/bblanchon/pdfium-binaries/releases
#                             (BSD-3, includes header + DLL + import lib)
#   external/winfsp/          WinFsp redistributable MSI bundled into
#                             the installer; runs silently on target
#                             machines missing HKLM\SOFTWARE\WinFsp.
#
# Skipped: QCView's external/ffmpeg/macos/, doc/, presets/.
# Also skipped: openexr - QCView only ships its import libs without
# the DLLs, so we install OpenEXR via vcpkg instead. See vcpkg.json.

$ErrorActionPreference = 'Stop'
# PS 5.1 defaults to a TLS version GitHub rejects; force 1.2 for all downloads.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$ufbRoot   = Split-Path -Parent $PSScriptRoot
$githubDir = Split-Path -Parent $ufbRoot   # sibling repos live next to ufb/
$ufbExt    = Join-Path $ufbRoot 'external'

# QCView-Player is expected as a sibling repo next to ufb/. Override the
# location with $env:QCVIEW_ROOT if it lives elsewhere.
$qcRoot = if ($env:QCVIEW_ROOT) { $env:QCVIEW_ROOT } else { Join-Path $githubDir 'QCView-Player' }

Write-Host "UFB external setup..."

# ffmpeg source: prefer QCView-Player's vendored prebuilts; otherwise download a
# complete BtbN shared GPL build (headers + import libs + DLLs + ffmpeg.exe /
# ffprobe.exe). Both expose the include/ + lib/*.lib + bin/*.{dll,exe} layout
# that cmake/external.cmake + app/CMakeLists expect (ffmpeg 8.x: avcodec-62,
# etc.). QCView is only needed for ffmpeg, so its absence is no longer fatal.
# GPL build is fine - UFB is GPL-3.0.
$ufbFf = Join-Path $ufbExt 'ffmpeg'
$qcFf  = Join-Path $qcRoot 'external\ffmpeg'
# Pinned BtbN asset (ffmpeg 8.x shared). Bump when moving ffmpeg majors;
# cmake/external.cmake hardcodes the DLL major-version filenames to match.
$ffmpegAsset = 'ffmpeg-n8.1-latest-win64-gpl-shared-8.1'

if (Test-Path (Join-Path $qcFf 'include\libavcodec\avcodec.h')) {
    # QCView mirrors bin/ wholesale (ffmpeg.exe + ffprobe.exe + every av*/sw*
    # DLL) - being picky about which DLLs to ship has bitten us before.
    Write-Host "ffmpeg: copying from QCView-Player ($qcFf)..."
    robocopy "$qcFf\include" "$ufbFf\include" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    robocopy "$qcFf\lib"     "$ufbFf\lib"     /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    if (-not (Test-Path "$ufbFf\bin")) { New-Item -ItemType Directory -Path "$ufbFf\bin" | Out-Null }
    robocopy "$qcFf\bin" "$ufbFf\bin" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    foreach ($f in @('LICENSE', 'README.txt')) {
        $src = Join-Path $qcFf $f
        if (Test-Path $src) { Copy-Item $src "$ufbFf\" -Force }
    }
} elseif (Test-Path (Join-Path $ufbFf 'bin\ffmpeg.exe')) {
    Write-Host "[skip] ffmpeg already in external/ffmpeg (bin/ffmpeg.exe present)"
} else {
    Write-Host "ffmpeg: QCView prebuilts absent; downloading BtbN shared build ($ffmpegAsset)..."
    $ffZip = Join-Path $env:TEMP "$ffmpegAsset.zip"
    Invoke-WebRequest -Uri "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/$ffmpegAsset.zip" -OutFile $ffZip -UseBasicParsing
    $ffTmp = Join-Path $env:TEMP 'ufb-ffmpeg-extract'
    if (Test-Path $ffTmp) { Remove-Item $ffTmp -Recurse -Force }
    Expand-Archive $ffZip -DestinationPath $ffTmp -Force
    $ffRoot = (Get-ChildItem $ffTmp -Directory | Select-Object -First 1).FullName
    foreach ($d in 'include', 'lib', 'bin') { New-Item -ItemType Directory -Force -Path (Join-Path $ufbFf $d) | Out-Null }
    robocopy "$ffRoot\include" "$ufbFf\include" /E /NFL /NDL /NJH /NJS /NP | Out-Null
    robocopy "$ffRoot\lib"     "$ufbFf\lib"     /E /NFL /NDL /NJH /NJS /NP | Out-Null
    robocopy "$ffRoot\bin"     "$ufbFf\bin"     /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if (Test-Path "$ffRoot\LICENSE.txt") { Copy-Item "$ffRoot\LICENSE.txt" "$ufbFf\" -Force }
}

# ---- exiftool (vendored from ufb-tauri) ---------------------------
# Runtime binary + its bundled Perl tree. Used by the transcode
# pipeline to copy metadata from the source onto the output MP4.
# Non-fatal at runtime - if missing, transcode logs a warning and
# the MP4 just lacks the source's metadata.
# Sibling repo next to ufb/; override with $env:UFB_TAURI_ROOT.
$tauriRoot = if ($env:UFB_TAURI_ROOT) { $env:UFB_TAURI_ROOT } else { Join-Path $githubDir 'ufb-tauri' }
$tauriExifTool = Join-Path $tauriRoot 'src-tauri\external\exiftool'
$ufbExifTool = Join-Path $ufbExt 'exiftool'
if (Test-Path (Join-Path $ufbExifTool 'exiftool.exe')) {
    Write-Host "[skip] exiftool already in external/exiftool"
} elseif (Test-Path (Join-Path $tauriExifTool 'exiftool.exe')) {
    Write-Host "Copying exiftool from ufb-tauri..."
    if (-not (Test-Path $ufbExifTool)) { New-Item -ItemType Directory -Path $ufbExifTool | Out-Null }
    robocopy $tauriExifTool $ufbExifTool /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
} else {
    # No ufb-tauri vendored copy: download the official Windows package
    # (exiftool(-k).exe + its bundled Perl tree exiftool_files/). exiftool.org
    # stopped hosting the zips directly (404 as of 2026-07); its download links
    # now point at SourceForge. Use the downloads.sourceforge.net mirror
    # endpoint - the sourceforge.net/...(/download) page serves HTML to
    # Invoke-WebRequest, not the zip.
    $exiftoolVersion = '13.59'
    Write-Host "exiftool: ufb-tauri absent; downloading exiftool $exiftoolVersion from SourceForge..."
    $exZip = Join-Path $env:TEMP "exiftool-$($exiftoolVersion)_64.zip"
    Invoke-WebRequest -Uri "https://downloads.sourceforge.net/project/exiftool/exiftool-$($exiftoolVersion)_64.zip" -OutFile $exZip -UseBasicParsing -UserAgent 'ufb-setup'
    $exTmp = Join-Path $env:TEMP 'ufb-exiftool-extract'
    if (Test-Path $exTmp) { Remove-Item $exTmp -Recurse -Force }
    Expand-Archive $exZip -DestinationPath $exTmp -Force
    $exRoot = (Get-ChildItem $exTmp -Directory | Select-Object -First 1).FullName
    New-Item -ItemType Directory -Force -Path $ufbExifTool | Out-Null
    # The distributed exe is named "exiftool(-k).exe"; the runtime + installer
    # expect a plain "exiftool.exe".
    Copy-Item (Join-Path $exRoot 'exiftool(-k).exe') (Join-Path $ufbExifTool 'exiftool.exe') -Force
    robocopy (Join-Path $exRoot 'exiftool_files') (Join-Path $ufbExifTool 'exiftool_files') /E /NFL /NDL /NJH /NJS /NP | Out-Null
}

# OpenEXR comes from vcpkg, not QCView. QCView's external/openexr
# only contains import libs - the DLLs are produced per-project via
# its own build step. vcpkg openexr is a one-time ~5 min install.

# ---- PDFium (bblanchon prebuilts) ---------------------------------
$ufbPdf = Join-Path $ufbExt 'pdfium'
if (Test-Path (Join-Path $ufbPdf 'include\fpdfview.h')) {
    Write-Host "[skip] PDFium already in external/pdfium"
} else {
    Write-Host "Downloading PDFium prebuilts..."
    $tmp = Join-Path $env:TEMP 'pdfium-win-x64.tgz'
    $url = 'https://github.com/bblanchon/pdfium-binaries/releases/latest/download/pdfium-win-x64.tgz'
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
    if (-not (Test-Path $ufbPdf)) { New-Item -ItemType Directory -Path $ufbPdf | Out-Null }
    # Use tar (built into Windows 10+) to extract .tgz
    tar -xzf $tmp -C $ufbPdf
    Remove-Item $tmp -Force
    if (Test-Path (Join-Path $ufbPdf 'include\fpdfview.h')) {
        Write-Host "PDFium ready in external/pdfium"
    } else {
        Write-Warning "PDFium extraction missing fpdfview.h - check $ufbPdf layout"
    }
}

# ---- WinFsp redistributable (for installer bundling) ---------------
# UFB's mount agent depends on WinFsp 2.x at runtime (delay-loaded
# winfsp-x64.dll). Without it, every mount-start hits a delay-load
# failure. The installer auto-runs this MSI silently if HKLM\SOFTWARE\
# WinFsp is missing on the target machine.
#
# Source: official GitHub releases. Pinned to a known-good version so
# the bundle is reproducible; bump when a newer WinFsp drops.
# Saved as a stable name (winfsp.msi) so the installer's
# `dontcopy` + ExtractTemporaryFile flow can reference a literal
# filename. Version is recorded in winfsp/version.txt for traceability.
$winFspVersion = '2.1.25156'   # check https://github.com/winfsp/winfsp/releases for newer
$ufbWinfsp = Join-Path $ufbExt 'winfsp'
$winfspMsi = Join-Path $ufbWinfsp 'winfsp.msi'
$winfspVersionFile = Join-Path $ufbWinfsp 'version.txt'
$existingVersion = if (Test-Path $winfspVersionFile) { (Get-Content $winfspVersionFile -Raw).Trim() } else { '' }
if ((Test-Path $winfspMsi) -and $existingVersion -eq $winFspVersion) {
    Write-Host "[skip] WinFsp $winFspVersion MSI already in external/winfsp"
} else {
    Write-Host "Downloading WinFsp $winFspVersion redistributable..."
    if (-not (Test-Path $ufbWinfsp)) { New-Item -ItemType Directory -Path $ufbWinfsp | Out-Null }
    # WinFsp release tags are the major line (e.g. v2.1), not the full build
    # number (winfsp-2.1.25156.msi lives under tag v2.1), and the mapping isn't
    # derivable from the version string. Resolve the asset URL from the releases
    # API by filename instead of constructing a tag-based URL.
    $asset = "winfsp-$winFspVersion.msi"
    $releases = Invoke-RestMethod -Uri 'https://api.github.com/repos/winfsp/winfsp/releases' -Headers @{ 'User-Agent' = 'ufb-setup' } -UseBasicParsing
    $url = $releases.assets | Where-Object { $_.name -eq $asset } | Select-Object -First 1 -ExpandProperty browser_download_url
    if (-not $url) {
        throw "WinFsp $asset not found in winfsp/winfsp releases. Bump `$winFspVersion to a published build."
    }
    Invoke-WebRequest -Uri $url -OutFile $winfspMsi -UseBasicParsing
    if (Test-Path $winfspMsi) {
        $winFspVersion | Set-Content -Path $winfspVersionFile -Encoding ascii -NoNewline
        Write-Host "WinFsp $winFspVersion MSI ready at $winfspMsi"
    } else {
        Write-Warning "WinFsp download missing - check the version pin"
    }
}

# ---- WinSparkle (auto-update, Phase 2) -----------------------------
# WinSparkle is the Windows auto-updater: it downloads the new Inno
# installer from the LAN appcast, verifies its EdDSA signature, runs
# it, and relaunches. Layout cmake/app/CMakeLists.txt expects:
#   external/winsparkle/include/winsparkle.h
#   external/winsparkle/lib/WinSparkle.lib
#   external/winsparkle/bin/WinSparkle.dll
# Official release ships a flat zip with WinSparkle.dll + .lib +
# include/winsparkle.h; we restage into the include/lib/bin layout.
$winSparkleVersion = '0.9.3'   # https://github.com/vslavik/winsparkle/releases
# NOTE: must be >= 0.9.0 - that's where WinSparkle added EdDSA (Ed25519)
# support (win_sparkle_set_eddsa_public_key). 0.8.x is DSA-only and won't
# compile Updater_win.cpp, which shares the Ed25519 key with the Mac side.
$ufbWinSparkle = Join-Path $ufbExt 'winsparkle'
if (Test-Path (Join-Path $ufbWinSparkle 'include\winsparkle.h')) {
    Write-Host "[skip] WinSparkle already in external/winsparkle"
} else {
    Write-Host "Downloading WinSparkle $winSparkleVersion..."
    $wsZip = Join-Path $env:TEMP "WinSparkle-$winSparkleVersion.zip"
    $wsUrl = "https://github.com/vslavik/winsparkle/releases/download/v$winSparkleVersion/WinSparkle-$winSparkleVersion.zip"
    Invoke-WebRequest -Uri $wsUrl -OutFile $wsZip -UseBasicParsing
    $wsTmp = Join-Path $env:TEMP 'ufb-winsparkle-extract'
    if (Test-Path $wsTmp) { Remove-Item -Recurse -Force $wsTmp }
    Expand-Archive -Path $wsZip -DestinationPath $wsTmp -Force
    # The zip's top folder is WinSparkle-<ver>\; binaries live under
    # x64\Release\ (WinSparkle.dll + .lib) and include\winsparkle.h.
    $wsSrc = Join-Path $wsTmp "WinSparkle-$winSparkleVersion"
    New-Item -ItemType Directory -Force -Path (Join-Path $ufbWinSparkle 'include') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $ufbWinSparkle 'lib')     | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $ufbWinSparkle 'bin')     | Out-Null
    # Copy the whole include\ dir, not just winsparkle.h: it #includes
    # winsparkle-version.h, so a lone header fails to compile (C1083).
    robocopy (Join-Path $wsSrc 'include') (Join-Path $ufbWinSparkle 'include') /E /NFL /NDL /NJH /NJS /NP | Out-Null
    Copy-Item (Join-Path $wsSrc 'x64\Release\WinSparkle.lib') (Join-Path $ufbWinSparkle 'lib\WinSparkle.lib')   -Force
    Copy-Item (Join-Path $wsSrc 'x64\Release\WinSparkle.dll') (Join-Path $ufbWinSparkle 'bin\WinSparkle.dll')   -Force
    Remove-Item -Recurse -Force $wsTmp
    if (Test-Path (Join-Path $ufbWinSparkle 'include\winsparkle.h')) {
        Write-Host "WinSparkle $winSparkleVersion ready at $ufbWinSparkle"
    } else {
        Write-Warning "WinSparkle layout incomplete - check the zip structure for v$winSparkleVersion"
    }
}

Write-Host ""
Write-Host "Done. external now contains:"
Get-ChildItem $ufbExt -Recurse -File |
    Group-Object { $_.Directory.FullName.Replace($ufbExt, '').TrimStart('\').Split('\')[0] } |
    ForEach-Object {
        $size = ($_.Group | Measure-Object Length -Sum).Sum / 1MB
        Write-Host ("  {0,-12} {1,6:N1} MB ({2} files)" -f $_.Name, $size, $_.Count)
    }
