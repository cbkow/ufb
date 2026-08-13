cmake_minimum_required(VERSION 3.24)

# vcpkg manifest integration. Honor an existing toolchain
# (CI / IDE) over our default. CMakePresets.json points
# CMAKE_TOOLCHAIN_FILE at the user's vcpkg checkout; this fallback
# kicks in for command-line cmake invocations that skip presets.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
endif()

# Minimum supported macOS. Must be set before project() or the
# compiler/linker default to the *host* OS version — every Mach-O then
# gets stamped minos=<host> and dyld refuses to load the app on any
# older system (found the hard way: 1.0.x binaries built on macOS 26
# were 26.0-only while Info.plist claimed 13.0). Ignored on Windows.
# LSMinimumSystemVersion in app/Info.plist.in and the vcpkg overlay
# triplets in cmake/vcpkg-triplets/ derive from / mirror this value.
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")

# Pin vcpkg's OSX deployment target too, for the toolchain-file path
# (classic `vcpkg install` runs pass --overlay-triplets explicitly —
# see scripts/release-mac.sh). Same-name overlays win over builtins.
if(CMAKE_HOST_APPLE)
    set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vcpkg-triplets"
        CACHE PATH "vcpkg overlay triplets (pins VCPKG_OSX_DEPLOYMENT_TARGET)")
endif()

project(UFB
    VERSION 1.1.1
    DESCRIPTION "Union File Browser (Qt edition)"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Auto-update (Phase 2) EdDSA public key — base64 string from Sparkle's
# `generate_keys`. Used as macOS Info.plist SUPublicEDKey and compiled
# into the Windows WinSparkle updater. Same key for both platforms.
# The private half never lives in this repo (login Keychain only).
# Override at configure time if needed: -DUFB_UPDATE_PUBLIC_KEY=<base64>.
set(UFB_UPDATE_PUBLIC_KEY "+Xlrad3WswxIRTyzBTp8KaTEjyfPO5Ap4llmPdZf3RQ=" CACHE STRING
    "Sparkle/WinSparkle EdDSA public key (base64 from generate_keys)")

# Enable Objective-C++ early on macOS so qt_add_executable / target_sources
# can pick up .mm files for the slice-01..07 macOS surfaces. If we leave
# this for app/CMakeLists.txt to enable AFTER qt_add_executable, CMake
# generate fails with "CMAKE_OBJCXX_COMPILE_OBJECT not set" because the
# rule for .mm wasn't loaded by the time the target was created.
if(APPLE)
    enable_language(OBJCXX)
endif()

# -----------------------------------------------------------------------------
# Find Qt
# -----------------------------------------------------------------------------

if(NOT DEFINED CMAKE_PREFIX_PATH AND NOT DEFINED Qt6_DIR)
    if(WIN32)
        # Default Qt install location; override via -DCMAKE_PREFIX_PATH
        set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64" CACHE PATH "Qt install root")
    elseif(APPLE)
        # Default Qt install location; override via -DCMAKE_PREFIX_PATH.
        # Derive from $QMAKE (release-mac.sh sets it) when present so we
        # honour a non-default install; otherwise fall back to the
        # standard layout.
        if(DEFINED ENV{QMAKE} AND EXISTS "$ENV{QMAKE}")
            get_filename_component(_qt_bin_dir "$ENV{QMAKE}" DIRECTORY)
            get_filename_component(_qt_root "${_qt_bin_dir}" DIRECTORY)
            set(CMAKE_PREFIX_PATH "${_qt_root}" CACHE PATH "Qt install root")
        else()
            set(CMAKE_PREFIX_PATH "$ENV{HOME}/Qt/6.11.0/macos" CACHE PATH "Qt install root")
        endif()
    endif()
endif()

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core
    Gui
    GuiPrivate      # rhi/qrhi.h for the in-app video QQuickRhiItem (lightbox)
    Network
    Qml
    Quick
    QuickControls2
    QuickDialogs2
    ShaderTools     # qt_add_shaders -> .qsb (YUV->RGB video shaders)
    Sql
)

qt_standard_project_setup(REQUIRES 6.11)

# Qt WebEngine — OPTIONAL. Drives the lightbox's rendered HTML preview
# (HtmlPreview.qml); without it the build stays green and HTML falls
# back to the QTextDocument-based TextPreview. It's an add-on module
# (Qt MaintenanceTool: extensions.qtwebengine.<ver>.clang_64 / win64),
# installed on the mac build machine 2026-08-13; install on the
# Windows box to get rendered HTML there too.
find_package(Qt6 6.11 QUIET COMPONENTS WebEngineQuick)
if(TARGET Qt6::WebEngineQuick)
    message(STATUS "ufb: Qt WebEngineQuick found — rendered HTML preview enabled")
else()
    message(STATUS "ufb: Qt WebEngineQuick NOT found — HTML preview falls back to rich text")
endif()

# -----------------------------------------------------------------------------
# Corrosion: bridge Cargo to CMake
# -----------------------------------------------------------------------------

include(FetchContent)

FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.1
)

# Manual populate-then-add_subdirectory so we can prepend Corrosion's
# bundled FindRust to CMAKE_MODULE_PATH BEFORE Corrosion's CMakeLists
# does its `find_package(Rust)`. Otherwise Qt 6.11.0+ ships a stub
# `FindRust.cmake` (only sets Rustc_EXECUTABLE) that wins the
# resolution and Corrosion crashes with "could not find TARGET
# Rust::Rustc". Confirmed against Qt 6.11.0 macos / lib/cmake/Qt6/
# FindRust.cmake. Per macOSplans/10 risk-register entry surfaced
# during the slice-07 dogfood test.
FetchContent_GetProperties(Corrosion)
if(NOT corrosion_POPULATED)
    FetchContent_Populate(Corrosion)
    list(PREPEND CMAKE_MODULE_PATH "${corrosion_SOURCE_DIR}/cmake")
    add_subdirectory(${corrosion_SOURCE_DIR} ${corrosion_BINARY_DIR})
endif()

# Import the Rust bindings crate as a static library that CMake can link.
corrosion_import_crate(
    MANIFEST_PATH ${CMAKE_SOURCE_DIR}/bindings/Cargo.toml
    CRATES bindings
)

# cxx-qt-build (run inside the bindings crate's build script) locates Qt
# via the QMAKE env var. Forward whichever Qt CMake found via Qt6_DIR
# so cargo builds against the same Qt this CMake project does. Without
# this, cxx-qt-build panics with "Could not find Qt installation".
get_target_property(_qt_qmake_loc Qt6::qmake IMPORTED_LOCATION)
if(_qt_qmake_loc)
    # On macOS also forward the deployment target: rustc pins its own
    # floor, but cc-rs-compiled C/C++ inside cargo deps reads the
    # MACOSX_DEPLOYMENT_TARGET env var and otherwise targets the host.
    if(APPLE)
        corrosion_set_env_vars(bindings
            "QMAKE=${_qt_qmake_loc}"
            "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        corrosion_set_env_vars(bindings "QMAKE=${_qt_qmake_loc}")
    endif()
    message(STATUS "Set QMAKE for cxx-qt: ${_qt_qmake_loc}")
endif()

# -----------------------------------------------------------------------------
# Phase 14 thumbnail-pipeline external libraries
# -----------------------------------------------------------------------------

include(${CMAKE_SOURCE_DIR}/cmake/external.cmake)

# -----------------------------------------------------------------------------
# Subprojects
# -----------------------------------------------------------------------------

add_subdirectory(app)
