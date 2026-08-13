# Overlay of vcpkg's builtin arm64-osx triplet (same name, so it wins
# wherever VCPKG_OVERLAY_TRIPLETS points here) that pins the macOS
# deployment target. Without the pin, vcpkg builds for the host OS —
# which is how Homebrew-era libheif/OpenEXR dylibs stamped minos 26.0
# shipped inside a bundle whose Info.plist claimed 13.0, and dyld
# refused to load the app anywhere older than the build machine.
# Keep in lockstep with CMAKE_OSX_DEPLOYMENT_TARGET in /CMakeLists.txt.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET "14.0")
