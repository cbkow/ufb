# cmake/external.cmake
#
# Phase 14 thumbnail-pipeline external libraries, per-platform.
#
# Two sources of libraries:
#   1. external/<lib>/   — prebuilt or built-from-source binaries
#      populated by:
#        Windows: scripts/setup-external.ps1   (mirrors QCView prebuilts)
#        macOS:   scripts/setup-external-mac.sh
#                  (PDFium darwin from bblanchon, ffmpeg from source)
#   2. vcpkg            — openexr from vcpkg manifest mode
#                          (vcpkg.json at repo root). Triplets:
#                          x64-windows on Windows,
#                          arm64-osx / x64-osx on macOS.

# ---- ffmpeg --------------------------------------------------------
set(UFB_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/external/ffmpeg")

if(WIN32 AND EXISTS "${UFB_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    add_library(ufb::ffmpeg INTERFACE IMPORTED)
    target_include_directories(ufb::ffmpeg
        INTERFACE "${UFB_FFMPEG_ROOT}/include")

    set(_ufb_ffmpeg_libs
        avcodec
        avformat
        avutil
        avfilter      # multi-track audio downmix/routing (lightbox AudioPlayer)
        swscale
        swresample
    )
    foreach(_lib IN LISTS _ufb_ffmpeg_libs)
        add_library(ufb::${_lib} SHARED IMPORTED)
        set_target_properties(ufb::${_lib} PROPERTIES
            IMPORTED_IMPLIB "${UFB_FFMPEG_ROOT}/lib/${_lib}.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${UFB_FFMPEG_ROOT}/include")
        target_link_libraries(ufb::ffmpeg INTERFACE ufb::${_lib})
    endforeach()

    # Per-DLL IMPORTED_LOCATION uses the actual DLL filename, which
    # encodes the major version (avcodec-62.dll, etc.). Set them
    # individually since the version differs per library.
    set_target_properties(ufb::avcodec    PROPERTIES IMPORTED_LOCATION "${UFB_FFMPEG_ROOT}/bin/avcodec-62.dll")
    set_target_properties(ufb::avformat   PROPERTIES IMPORTED_LOCATION "${UFB_FFMPEG_ROOT}/bin/avformat-62.dll")
    set_target_properties(ufb::avutil     PROPERTIES IMPORTED_LOCATION "${UFB_FFMPEG_ROOT}/bin/avutil-60.dll")
    set_target_properties(ufb::avfilter   PROPERTIES IMPORTED_LOCATION "${UFB_FFMPEG_ROOT}/bin/avfilter-11.dll")
    set_target_properties(ufb::swscale    PROPERTIES IMPORTED_LOCATION "${UFB_FFMPEG_ROOT}/bin/swscale-9.dll")
    set_target_properties(ufb::swresample PROPERTIES IMPORTED_LOCATION "${UFB_FFMPEG_ROOT}/bin/swresample-6.dll")

    message(STATUS "ufb: ffmpeg (Windows) found at ${UFB_FFMPEG_ROOT}")
elseif(APPLE AND EXISTS "${UFB_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    # Darwin: shared dylibs in lib/ named lib<name>.<MAJOR>.dylib
    # plus an unversioned symlink lib<name>.dylib that the linker
    # uses. Major version differs per ffmpeg release; we glob to
    # avoid hardcoding numbers that drift.
    add_library(ufb::ffmpeg INTERFACE IMPORTED)
    target_include_directories(ufb::ffmpeg
        INTERFACE "${UFB_FFMPEG_ROOT}/include")

    set(_ufb_ffmpeg_libs
        avcodec
        avformat
        avutil
        avfilter      # multi-track audio downmix/routing (lightbox AudioPlayer)
        swscale
        swresample
    )
    foreach(_lib IN LISTS _ufb_ffmpeg_libs)
        # Pick the versioned dylib (lib<name>.<MAJOR>.dylib) so the
        # IMPORTED_LOCATION + install_name match what the bundle
        # ships under Contents/Frameworks/.
        file(GLOB _dylib_match
            "${UFB_FFMPEG_ROOT}/lib/lib${_lib}.[0-9]*.dylib")
        list(LENGTH _dylib_match _match_count)
        if(_match_count GREATER 0)
            list(GET _dylib_match 0 _dylib)
            add_library(ufb::${_lib} SHARED IMPORTED)
            set_target_properties(ufb::${_lib} PROPERTIES
                IMPORTED_LOCATION "${_dylib}"
                INTERFACE_INCLUDE_DIRECTORIES "${UFB_FFMPEG_ROOT}/include")
            target_link_libraries(ufb::ffmpeg INTERFACE ufb::${_lib})
        else()
            message(WARNING
                "ufb::${_lib}: no darwin dylib found in ${UFB_FFMPEG_ROOT}/lib/ "
                "(expected lib${_lib}.<MAJOR>.dylib). "
                "Run scripts/setup-external-mac.sh to build.")
        endif()
    endforeach()

    message(STATUS "ufb: ffmpeg (darwin) found at ${UFB_FFMPEG_ROOT}")
else()
    message(STATUS "ufb: ffmpeg NOT found at ${UFB_FFMPEG_ROOT}")
    if(WIN32)
        message(STATUS "     Run scripts/setup-external.ps1 to populate it.")
    elseif(APPLE)
        message(STATUS "     Run scripts/setup-external-mac.sh to build.")
    endif()
    message(STATUS "     Video thumbnails will be disabled until then.")
endif()

# ---- psd_sdk (FetchContent, MIT, Molecule Engine) ----------------
# Cross-platform — same wiring on Windows + macOS.
include(FetchContent)
FetchContent_Declare(
    psd_sdk
    GIT_REPOSITORY https://github.com/MolecularMatters/psd_sdk.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
# psd_sdk's CMakeLists.txt declares cmake_minimum_required(3.2),
# which CMake 3.27+ rejects. Override the floor for this dep
# only. Also skip the samples subdir we don't need by populating
# manually instead of MakeAvailable.
set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "" FORCE)
FetchContent_GetProperties(psd_sdk)
if(NOT psd_sdk_POPULATED)
    FetchContent_Populate(psd_sdk)
    add_subdirectory(${psd_sdk_SOURCE_DIR}/src/Psd
                     ${psd_sdk_BINARY_DIR}/Psd EXCLUDE_FROM_ALL)
endif()
if(TARGET Psd)
    target_include_directories(Psd PUBLIC ${psd_sdk_SOURCE_DIR}/src/Psd)
    add_library(ufb::psd ALIAS Psd)
    message(STATUS "ufb: psd_sdk (Psd target) ready")
else()
    message(WARNING "ufb: psd_sdk fetched but Psd target missing")
endif()

# ---- PDFium ------------------------------------------------------
set(_ufb_pdf_root "${CMAKE_SOURCE_DIR}/external/pdfium")

if(WIN32 AND EXISTS "${_ufb_pdf_root}/include/fpdfview.h")
    add_library(ufb::pdfium SHARED IMPORTED)
    set_target_properties(ufb::pdfium PROPERTIES
        IMPORTED_LOCATION "${_ufb_pdf_root}/bin/pdfium.dll"
        IMPORTED_IMPLIB   "${_ufb_pdf_root}/lib/pdfium.dll.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_ufb_pdf_root}/include")
    message(STATUS "ufb: PDFium (Windows) found at ${_ufb_pdf_root}")
elseif(APPLE AND EXISTS "${_ufb_pdf_root}/include/fpdfview.h")
    # Darwin: bblanchon prebuilt ships libpdfium.dylib in lib/.
    add_library(ufb::pdfium SHARED IMPORTED)
    set_target_properties(ufb::pdfium PROPERTIES
        IMPORTED_LOCATION "${_ufb_pdf_root}/lib/libpdfium.dylib"
        INTERFACE_INCLUDE_DIRECTORIES "${_ufb_pdf_root}/include")
    message(STATUS "ufb: PDFium (darwin) found at ${_ufb_pdf_root}")
else()
    message(STATUS "ufb: PDFium NOT found at ${_ufb_pdf_root}")
    if(WIN32)
        message(STATUS "     Run scripts/setup-external.ps1 to populate it.")
    elseif(APPLE)
        message(STATUS "     Run scripts/setup-external-mac.sh to populate it.")
    endif()
endif()

# ---- OpenEXR (vcpkg, has proper CMake config) --------------------
# vcpkg's CMake toolchain auto-extends CMAKE_PREFIX_PATH, but when
# the build dir was first configured WITHOUT the toolchain we have
# to point find_package at the vcpkg_installed share dir explicitly.
# Doing both keeps find_package happy whether or not the toolchain
# is active.
if(WIN32)
    list(APPEND CMAKE_PREFIX_PATH
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/share")
elseif(APPLE)
    # Triplet selection follows VCPKG_TARGET_TRIPLET, which the
    # `just mac-*` recipes set per `uname -m`. Default to
    # arm64-osx since Apple Silicon is the primary target;
    # x64-osx is a CI matrix point only.
    if(VCPKG_TARGET_TRIPLET STREQUAL "x64-osx")
        list(APPEND CMAKE_PREFIX_PATH
            "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-osx/share")
    else()
        list(APPEND CMAKE_PREFIX_PATH
            "${CMAKE_SOURCE_DIR}/vcpkg_installed/arm64-osx/share")
    endif()
endif()
find_package(OpenEXR CONFIG QUIET)
find_package(Imath CONFIG QUIET)
if(OpenEXR_FOUND)
    message(STATUS "ufb: OpenEXR ${OpenEXR_VERSION} found")
else()
    message(STATUS "ufb: OpenEXR NOT found - run `vcpkg install` first")
endif()
