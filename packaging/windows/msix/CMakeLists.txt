# The Microsoft Store package (Windows only; ported from minNotes packaging/windows/msix).
#   ufb_msix        packs ufb.exe + the tree its POST_BUILD steps stage next to it, plus ufb-agent.exe
#                   (cargo-built separately with --target x86_64-pc-windows-msvc, as the release script
#                   does — NOT agent/target/release, which held a stale 1.2.3 agent on the build box), into
#                   packaging/windows/msix/dist/ufb-${PROJECT_VERSION}-x64.msix (see build_msix.ps1)
#   ufb_msix_clean  wipes dist/ and any leftover staging/
# Windows has no self-updater since 1.4.0 (WinSparkle retired): the Store owns updates, so every
# release tree is a Store tree — build it from the `release` preset.

if(NOT WIN32)
    return()
endif()

# The identity Partner Center assigned (Product management → Product identity). Ingestion rejects a
# mismatch. The Store name is "ufbBrowser" ("ufb" was taken). The publisher is the cbkow account's
# (same as minNotes / QCView). Confirmed against the Product identity page 2026-10-24:
# PFN cbkow.ufbBrowser_k3nw8mfd599r4, Store ID 9NM0B07CF0HV.
# Plain variables, not CACHE: these are fixed by Partner Center, and a cache entry from an older
# configure would silently win over a corrected value here (bit once: cbkow.ufb survived the rename).
set(UFB_MSIX_IDENTITY "cbkow.ufbBrowser")                                   # Identity/Name
set(UFB_MSIX_PUBLISHER "CN=C64A1D41-EFAA-4EED-AE81-0C074867D4E9")           # Identity/Publisher (sideload cert subject too)
set(UFB_MSIX_PUBLISHER_DISPLAY_NAME "cbkow")                                # Properties/PublisherDisplayName
set(UFB_MSIX_VERSION "${PROJECT_VERSION}.0")   # MSIX versions have four parts

set(_ufb_manifest_out "${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/AppxManifest.xml.in" "${_ufb_manifest_out}" @ONLY)

find_program(UFB_PWSH NAMES pwsh powershell)
if(NOT UFB_PWSH)
    message(WARNING "No PowerShell on PATH — the ufb_msix target is unavailable.")
    return()
endif()

add_custom_target(ufb_msix
    COMMAND "${UFB_PWSH}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/build_msix.ps1"
        -BuildDir "$<TARGET_FILE_DIR:ufb>"
        -AgentExe "${CMAKE_SOURCE_DIR}/agent/target/x86_64-pc-windows-msvc/release/ufb-agent.exe"
        -Version "${PROJECT_VERSION}"
        -ManifestPath "${_ufb_manifest_out}"
        -ImagesDir "${CMAKE_CURRENT_SOURCE_DIR}/images"
        -OutputDir "${CMAKE_CURRENT_SOURCE_DIR}/dist"
    DEPENDS ufb
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Packaging ufb ${PROJECT_VERSION} as MSIX (packaging/windows/msix/dist/)"
    VERBATIM)

add_custom_target(ufb_msix_clean
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_CURRENT_SOURCE_DIR}/dist" "${CMAKE_CURRENT_SOURCE_DIR}/staging"
    COMMENT "Removing packaging/windows/msix/dist and staging"
    VERBATIM)
