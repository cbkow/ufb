# Overlay of vcpkg's builtin x64-osx triplet — see arm64-osx.cmake in
# this directory for why the deployment target is pinned. x64 is a CI
# matrix point only; Apple Silicon is the primary target.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)
set(VCPKG_OSX_DEPLOYMENT_TARGET "14.0")
