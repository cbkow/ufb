# x64-windows overlay triplet — identical to vcpkg's builtin x64-windows
# (dynamic CRT, dynamic libraries) plus one port-specific rule.
#
# libde265 is MISCOMPILED by Visual Studio 2026's MSVC 14.51 at /O2: its
# scan-position tables come out wrong and init_scan_orders() reads out of
# bounds during static initialization, so any process loading
# libde265.dll dies with STATUS_DLL_INIT_FAILED (0xC0000142) before
# main(). Seen on the first 1.4.1 build on rhombus (2026-11-06) with
# both 1.0.18 and 1.1.0; the 1.4.0 package's DLL from an older toolset
# was fine. Reported elsewhere with the same diagnosis
# (github.com/julyx10/lap/issues/389, github.com/ermig1979/AntiDupl/issues/243).
# Building that one port at /O1 sidesteps the bug; cl's D9025
# "overriding /O2 with /O1" warning is expected.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

if(PORT STREQUAL "libde265")
    set(VCPKG_C_FLAGS_RELEASE "/O1")
    set(VCPKG_CXX_FLAGS_RELEASE "/O1")
endif()
