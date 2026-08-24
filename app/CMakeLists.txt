qt_add_executable(ufb
    main.cpp
    AppController.h
    UfbApplication.cpp
    UfbApplication.h
    UfbImageProviders.cpp
    UfbImageProviders.h
    thumbnails/SystemIcons.h
    thumbnails/ShellThumbnail.h
    thumbnails/PsdBackend.cpp
    thumbnails/PsdBackend.h
    thumbnails/ExrBackend.cpp
    thumbnails/ExrBackend.h
    thumbnails/HdrBackend.cpp
    thumbnails/HdrBackend.h
    thumbnails/HeifBackend.cpp
    thumbnails/HeifBackend.h
    thumbnails/PdfBackend.cpp
    thumbnails/PdfBackend.h
    thumbnails/VideoBackend.cpp
    thumbnails/VideoBackend.h
    thumbnails/ThumbCache.cpp
    thumbnails/ThumbCache.h
    updater/Updater.h
    # In-app video preview decode core (ported from QCView-Player, namespace
    # ufbplayer, independent copy). Platform frame_handle split below.
    player/frame_handle.h
    player/frame_index.cpp
    player/frame_index.h
    player/scrub_frame_cache.cpp
    player/scrub_frame_cache.h
    player/simple_lru.h
    player/decoder_cleanup_queue.cpp
    player/decoder_cleanup_queue.h
    player/timecode_formatter.cpp
    player/timecode_formatter.h
    player/scrub_decoder.cpp
    player/scrub_decoder.h
    # Audio subsystem (ported from QCView, namespace ufbplayer). AudioPlayer
    # itself is a QML element — see the qt_add_qml_module SOURCES below.
    player/audio_routing.h
    player/audio_ring_buffer.h
    player/i_audio_source.h
    player/audio_decoder.cpp
    player/audio_decoder.h
    player/multi_stream_audio_decoder.cpp
    player/multi_stream_audio_decoder.h
    $<$<PLATFORM_ID:Windows>:ufb.rc>
)

# App version for runtime reporting (main.cpp's
# setApplicationVersion). Derived from project(VERSION) so it can
# never drift from Info.plist / the appcasts the way the old
# hardcoded string did. The WIN32 updater block below also relies
# on this for WinSparkle's appcast comparison.
target_compile_definitions(ufb PRIVATE
    "UFB_APP_VERSION=\"${CMAKE_PROJECT_VERSION}\"")

# Mark Theme.qml as a singleton BEFORE qt_add_qml_module sees it,
# otherwise the auto-generated qmldir registers it as a regular type
# and `Theme.colors.x` from other QML files fails to resolve.
set_source_files_properties(qml/Ufb/App/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(ufb
    URI Ufb.App
    VERSION 1.0
    SOURCES
        thumbnails/Thumbnailer.cpp
        thumbnails/Thumbnailer.h
        player/VideoSurfaceItem.cpp
        player/VideoSurfaceItem.h
        player/video_decoder.cpp
        player/video_decoder.h
        player/audio_player.cpp
        player/audio_player.h
        PdfDoc.cpp
        PdfDoc.h
        ExrInfo.cpp
        ExrInfo.h
        TextInfo.cpp
        TextInfo.h
        MndbDoc.cpp
        MndbDoc.h
        # miniz: zip reader for .mnpkg packages (MndbDoc staging); vendored
        # single-file amalgamation, same copy minNotes ships (external/miniz).
        ${CMAKE_SOURCE_DIR}/external/miniz/miniz.c
    QML_FILES
        qml/Ufb/App/Main.qml
        qml/Ufb/App/Theme.qml
        qml/Ufb/App/Icon.qml
        qml/Ufb/App/FileTypeIcon.qml
        qml/Ufb/App/FlatButton.qml
        qml/Ufb/App/FlatSlider.qml
        qml/Ufb/App/ToolStrip.qml
        qml/Ufb/App/SegmentedControl.qml
        qml/Ufb/App/SectionHeader.qml
        qml/Ufb/App/DualBrowserView.qml
        qml/Ufb/App/JobView.qml
        qml/Ufb/App/FolderTabView.qml
        qml/Ufb/App/ItemListPanel.qml
        qml/Ufb/App/TrackerView.qml
        qml/Ufb/App/ColumnEditor.qml
        qml/Ufb/App/ColumnManagerDialog.qml
        qml/Ufb/App/ColumnTrashDialog.qml
        qml/Ufb/App/BackupManagerDialog.qml
        qml/Ufb/App/TranscodeQueue.qml
        qml/Ufb/App/MeshSettingsDialog.qml
        qml/Ufb/App/MountManagerDialog.qml
        qml/Ufb/App/AddBookmarkDialog.qml
        qml/Ufb/App/NewJobDialog.qml
        qml/Ufb/App/PathSwapsDialog.qml
        qml/Ufb/App/CellEditor.qml
        qml/Ufb/App/FileBrowser.qml
        qml/Ufb/App/Sidebar.qml
        qml/Ufb/App/UfbMenu.qml
        qml/Ufb/App/UfbScrollBar.qml
        qml/Ufb/App/PreviewLightbox.qml
        qml/Ufb/App/VideoPreview.qml
        qml/Ufb/App/AudioPreview.qml
        qml/Ufb/App/HtmlPreview.qml
        qml/Ufb/App/ImagePreview.qml
        qml/Ufb/App/PdfReader.qml
        qml/Ufb/App/TextPreview.qml
    RESOURCES
        qml/Ufb/App/PhosphorIcons.js
        qml/Ufb/App/FileExtensionIcons.js
        qml/Ufb/App/ListingState.js
        qml/Ufb/App/fonts/Phosphor.ttf
        qml/Ufb/App/fonts/Phosphor-Fill.ttf
        qml/Ufb/App/fonts/Phosphor-Duotone.ttf
        qml/Ufb/App/fonts/Phosphor-Thin.ttf
)

# qt_add_qml_module's auto-generated type registrar emits
# `#include <ClassName.h>` for each QML_ELEMENT class, resolved
# against the include path. Thumbnailer.h sits in thumbnails/ so
# add that dir explicitly. Future thumbnail backend headers will
# benefit from the same line.
target_include_directories(ufb PRIVATE thumbnails)
target_include_directories(ufb PRIVATE player)
target_include_directories(ufb PRIVATE "${CMAKE_SOURCE_DIR}/external/miniz")

# Compile the video YUV->RGB + passthrough GLSL to .qsb bundles, embedded
# under :/ufb/player/shaders/. VideoSurfaceItem loads them at runtime.
qt_add_shaders(ufb "ufb_player_shaders"
    PREFIX "/ufb"
    FILES
        player/shaders/fullscreen.vert
        player/shaders/passthrough.frag
        player/shaders/yuv_to_rgb_biplanar.frag
        player/shaders/yuv_to_rgb_ayuv.frag
)

# OS-native icons + OS-first thumbnails. The headers are cross-
# platform; the implementations are split per OS — the _win.cpp
# files use the Windows Shell APIs, the _mac.mm files use
# NSWorkspace / QuickLook. Exactly one set per build.
if(WIN32)
    target_sources(ufb PRIVATE
        thumbnails/SystemIcons_win.cpp
        thumbnails/ShellThumbnail_win.cpp
        player/frame_handle_nonapple.cpp
        # Decode-only Vulkan device (shared VkDevice handed to FFmpeg for
        # the ProRes zero-copy path) + the AVVulkanDeviceContext handoff.
        player/vulkan/vulkan_device_manager.h
        player/vulkan/vulkan_device_manager.cpp
        player/vulkan_hw_device_ctx.h
        player/vulkan_hw_device_ctx.cpp
        # WASAPI audio output (parallel to coreaudio_device on macOS).
        player/wasapi_audio_device.h
        player/wasapi_audio_device.cpp
        # ProRes zero-copy (W1 / M5): adopt Qt's QRhi D3D11 device, run
        # FFmpeg's Vulkan-decoded VkImages through a Vulkan compute
        # YUV→RGB pass into a D3D11-shared RGBA16F texture (NT-handle
        # interop), wrapped as a QRhiTexture by VideoSurfaceItem.
        player/d3d11/d3d11_device_manager.h
        player/d3d11/d3d11_device_manager.cpp
        player/d3d11/d3d11_vulkan_yuv_compositor.h
        player/d3d11/d3d11_vulkan_yuv_compositor.cpp
        player/d3d11/d3d11_vulkan_decode_bridge.h
        player/d3d11/d3d11_vulkan_decode_bridge.cpp)
    target_include_directories(ufb PRIVATE player/d3d11)

    # Vulkan: VulkanDeviceManager + FrameHandle::Vulkan expose Vulkan
    # types in headers, and FFmpeg's hwcontext_vulkan handoff needs the
    # loader. VK_USE_PLATFORM_WIN32_KHR gates the win32 external-memory /
    # semaphore handle entry points the F.2.4.3 bridge uses.
    find_package(Vulkan REQUIRED)
    target_link_libraries(ufb PRIVATE Vulkan::Vulkan)
    target_compile_definitions(ufb PRIVATE VK_USE_PLATFORM_WIN32_KHR)

    # shaderc (Vulkan SDK): the YUV→RGB compositor compiles its GLSL
    # compute shader to SPIR-V at runtime. shaderc_combined is the
    # static lib (glslang + SPIRV-Tools folded in) so no extra DLL ships.
    # Headers come via Vulkan::Vulkan's include dir ($VULKAN_SDK/Include).
    target_link_libraries(ufb PRIVATE
        "$ENV{VULKAN_SDK}/Lib/shaderc_combined.lib")

    # D3D11 + DXGI for the device manager / bridge / compositor. dxguid
    # supplies the IID_* GUIDs used by IID_PPV_ARGS / QueryInterface.
    target_link_libraries(ufb PRIVATE d3d11 dxgi dxguid)

    # WASAPI device: ole32 (COM, already linked below) + avrt (MMCSS
    # AvSetMmThreadCharacteristics on the audio render thread).
    target_link_libraries(ufb PRIVATE avrt)
elseif(APPLE)
    target_sources(ufb PRIVATE
        thumbnails/SystemIcons_mac.mm
        thumbnails/ShellThumbnail_mac.mm
        # FrameHandle's CVPixelBuffer retain/release for the zero-copy path.
        player/frame_handle.mm
        # VideoToolbox CVPixelBuffer -> QRhiTexture (Metal) zero-copy bridge.
        player/zero_copy_bridge_metal.h
        player/zero_copy_bridge_metal.mm
        # CoreAudio output device for the audio subsystem.
        player/coreaudio_device.h
        player/coreaudio_device.mm)
    target_link_libraries(ufb PRIVATE
        "-framework CoreVideo"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework AudioUnit"
        # SMAppService login-item registration (plans/17 slice E,
        # MacOSBootstrap.mm::registerGuiLoginItem).
        "-framework ServiceManagement"
        # NetFSMountURLSync for GUI-owned plain SMB mounts (plans/17
        # F1, core::macos_mounts). The #[link] attribute in the Rust
        # staticlib doesn't survive into the C++ link.
        "-framework NetFS")
    # ShellThumbnail_mac.mm alloc/inits a QLThumbnailGenerationRequest,
    # so it needs ARC for correct memory management. Compile just that
    # file with -fobjc-arc regardless of the project's default Obj-C
    # mode. SystemIcons_mac.mm only uses autoreleased objects, so it
    # is fine either way and left on the default.
    set_source_files_properties(thumbnails/ShellThumbnail_mac.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

# ── Auto-update (Phase 2): Sparkle (macOS) / WinSparkle (Windows) ────
#
# The updater object is platform-split like ShellThumbnail. The actual
# framework link is CONDITIONAL on the vendored dependency being
# present, so the app still builds without it — the stub path compiles
# and Updater.available() returns false (the menu item hides). Vendor:
#   macOS:   external/Sparkle/Sparkle.framework  (Sparkle 2.x release)
#   Windows: external/winsparkle/{include,bin,lib}  (WinSparkle release)
#
# UFB_UPDATE_PUBLIC_KEY (root CMakeLists cache var) is the base64 EdDSA
# public key from Sparkle's generate_keys — substituted into Info.plist
# as SUPublicEDKey (macOS) and compiled into Updater_win.cpp (Windows).
if(APPLE)
    target_sources(ufb PRIVATE updater/Updater_mac.mm)
    set_source_files_properties(updater/Updater_mac.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")  # ARC for SPU* objects

    # Path matches QCView-Player's layout (external/Sparkle/) so its
    # vendored Sparkle.framework can be copied across verbatim.
    set(_sparkle_fw "${CMAKE_SOURCE_DIR}/external/Sparkle/Sparkle.framework")
    if(EXISTS "${_sparkle_fw}")
        message(STATUS "ufb: Sparkle found at ${_sparkle_fw}")
        target_compile_definitions(ufb PRIVATE UFB_HAVE_SPARKLE)
        target_link_options(ufb PRIVATE "-F${CMAKE_SOURCE_DIR}/external/Sparkle")
        target_link_libraries(ufb PRIVATE "${_sparkle_fw}")
        # Embed the framework into the bundle BEFORE the Developer-ID sign
        # step (the last POST_BUILD, at the end of this file). Use `ditto`,
        # not `cmake -E copy_directory`: a framework relies on its
        # Versions/Current + top-level symlinks and ad-hoc signature
        # metadata, which copy_directory flattens/breaks; ditto preserves
        # them (same approach QCView-Player uses).
        add_custom_command(TARGET ufb POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:ufb>/Contents/Frameworks"
            COMMAND ditto
                "${_sparkle_fw}"
                "$<TARGET_BUNDLE_DIR:ufb>/Contents/Frameworks/Sparkle.framework"
            COMMENT "Embedding Sparkle.framework into bundle (ditto)"
            VERBATIM)
    else()
        message(STATUS "ufb: Sparkle NOT found at ${_sparkle_fw} — "
                "auto-update disabled (vendor it to enable). Copy "
                "QCView-Player/external/Sparkle/Sparkle.framework (2.9.2) there.")
    endif()

    # Login-item agent plist (plans/17 slice E): SMAppService requires
    # it inside the bundle at Contents/Library/LaunchAgents/. Copied by
    # an early POST_BUILD (before the deploy + codesign steps later in
    # this file) so the signature covers it — a file added after
    # signing would break the seal.
    add_custom_command(TARGET ufb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:ufb>/Contents/Library/LaunchAgents"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/macos/dev.ufb.gui.plist"
            "$<TARGET_BUNDLE_DIR:ufb>/Contents/Library/LaunchAgents/dev.ufb.gui.plist"
        COMMENT "Bundling login-item plist (dev.ufb.gui.plist)"
        VERBATIM)
elseif(WIN32)
    target_sources(ufb PRIVATE updater/Updater_win.cpp)
    # UFB_APP_VERSION (WinSparkle's appcast comparison — ufb.exe has
    # no VERSIONINFO resource) is defined for all platforms near the
    # top of this file.
    target_compile_definitions(ufb PRIVATE
        "UFB_UPDATE_PUBLIC_KEY=\"${UFB_UPDATE_PUBLIC_KEY}\"")

    set(_winsparkle_root "${CMAKE_SOURCE_DIR}/external/winsparkle")
    if(EXISTS "${_winsparkle_root}/include/winsparkle.h")
        message(STATUS "ufb: WinSparkle found at ${_winsparkle_root}")
        target_compile_definitions(ufb PRIVATE UFB_HAVE_WINSPARKLE)
        target_include_directories(ufb PRIVATE "${_winsparkle_root}/include")
        target_link_libraries(ufb PRIVATE "${_winsparkle_root}/lib/WinSparkle.lib")
        # WinSparkle.dll is copied next to ufb.exe by the deploy block
        # further down (with the other runtime DLLs).
    else()
        message(STATUS "ufb: WinSparkle NOT found at ${_winsparkle_root} — "
                "auto-update disabled (run scripts/setup-external.ps1).")
    endif()
else()
    target_sources(ufb PRIVATE updater/Updater_stub.cpp)
endif()

target_include_directories(ufb PRIVATE updater)

target_link_libraries(ufb PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate   # rhi/qrhi.h for the video QQuickRhiItem (lightbox preview)
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    Qt6::Sql
    bindings
)

# Optional rendered-HTML preview (see root CMakeLists). main.cpp keys
# QtWebEngineQuick::initialize() + the _webEngineAvailable context
# property off this define; PreviewLightbox.qml gates the WebEngine
# route on that property, so absence degrades to TextPreview cleanly.
if(TARGET Qt6::WebEngineQuick)
    target_link_libraries(ufb PRIVATE Qt6::WebEngineQuick)
    target_compile_definitions(ufb PRIVATE UFB_HAVE_WEBENGINE)
endif()

# CredentialPrompt deleted (plans/17 slice B): credentials are
# OS-owned (Keychain via NetFS on macOS, Credential Manager via the
# SMB redirector on Windows) — UFB never prompts for or stores an SMB
# secret. credui/advapi32 links went with it; ole32 stays for shell
# thumbnail COM usage below.
if(WIN32)
    target_link_libraries(ufb PRIVATE ole32)

    # OS-native file-type icons (thumbnails/SystemIcons_win.cpp).
    # shell32  = SHGetFileInfoW + SHGetImageList
    # comctl32 = IImageList interface
    # gdi32    = CreateDIBSection + IImageList::Draw bitmap blit
    # user32   = generic GDI/USER surface the shell APIs touch
    # dwmapi   = DwmSetWindowAttribute (DWMWA_CLOAK startup-flash fix)
    target_link_libraries(ufb PRIVATE shell32 comctl32 gdi32 user32 dwmapi)
endif()

# Thumbnail backends from cmake/external.cmake.
if(TARGET ufb::psd)
    target_link_libraries(ufb PRIVATE ufb::psd)
endif()
if(OpenEXR_FOUND)
    target_link_libraries(ufb PRIVATE OpenEXR::OpenEXR)
    # Force the Release-config OpenEXR + Imath imported targets even
    # when the host build is Debug. Our vcpkg debug variants
    # (OpenEXR-3_4_d.dll etc.) either don't build cleanly or don't run
    # cleanly on this toolchain; in any case we ship Release across the
    # board, so the debug build picks them up too. Without this map,
    # the Debug link pulls in the `_d`-suffixed DLLs which aren't
    # synced next to ufb.exe and the process fails to load with
    # "OpenEXR-3_4_d.dll not found".
    if(WIN32)
        foreach(_imp_target
                OpenEXR::OpenEXR OpenEXR::OpenEXRCore OpenEXR::OpenEXRUtil
                OpenEXR::Iex OpenEXR::IlmThread
                Imath::Imath)
            if(TARGET ${_imp_target})
                set_property(TARGET ${_imp_target} PROPERTY
                    MAP_IMPORTED_CONFIG_DEBUG Release)
                set_property(TARGET ${_imp_target} PROPERTY
                    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
                set_property(TARGET ${_imp_target} PROPERTY
                    MAP_IMPORTED_CONFIG_MINSIZEREL Release)
            endif()
        endforeach()
    endif()
endif()
if(TARGET ufb::pdfium)
    target_link_libraries(ufb PRIVATE ufb::pdfium)
endif()
if(libheif_FOUND AND TARGET heif)
    # HeifBackend.cpp compiles to a null-return stub without this
    # define, so the build stays green when vcpkg hasn't run yet.
    target_compile_definitions(ufb PRIVATE UFB_HAVE_LIBHEIF)
    target_link_libraries(ufb PRIVATE heif)
    # Same Release-config forcing as OpenEXR above: we ship Release
    # DLLs across the board, and the `debug` preset is RelWithDebInfo.
    if(WIN32)
        foreach(_imp_target heif de265)
            if(TARGET ${_imp_target})
                set_property(TARGET ${_imp_target} PROPERTY
                    MAP_IMPORTED_CONFIG_DEBUG Release)
                set_property(TARGET ${_imp_target} PROPERTY
                    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
                set_property(TARGET ${_imp_target} PROPERTY
                    MAP_IMPORTED_CONFIG_MINSIZEREL Release)
            endif()
        endforeach()
    endif()
endif()
if(TARGET ufb::ffmpeg)
    target_link_libraries(ufb PRIVATE ufb::ffmpeg)
endif()


# cxx-qt registers QML types via static initializers inside the
# bindings static library. MSVC dead-strips object files that look
# unreferenced — including the registration init code — so the
# QML engine reports `module "Ufb.Backend" is not installed`.
# Force-include every object file via /WHOLEARCHIVE:bindings.lib.
if(MSVC)
    # /WHOLEARCHIVE pulls in Rust stdlib helpers that are also linked via
    # the regular dependency chain — LNK1169 multiply-defined errors. The
    # /FORCE:MULTIPLE flag picks the first definition silently. Acceptable
    # for Phase 2 bring-up; the proper fix is a surgical /INCLUDE:<symbol>
    # naming the cxx-qt-generated QML registration entry-point so we can
    # drop WHOLEARCHIVE entirely. Tracked as Phase 2 follow-up.
    target_link_options(ufb PRIVATE
        "/WHOLEARCHIVE:bindings.lib"
        "/FORCE:MULTIPLE"
        "/IGNORE:4006"  # silence "symbol already defined" warnings
        "/IGNORE:4088"  # WHOLEARCHIVE compatibility note
    )

    # SEH translation for the thumbnail thread pool. The default
    # `/EHsc` only handles C++ exceptions; corrupt media files that
    # cause libtiff / stb_image / OpenEXR / etc. to read past a
    # buffer end raise a Win32 EXCEPTION_ACCESS_VIOLATION that
    # would otherwise terminate the entire process. `/EHa` enables
    # asynchronous exception handling so the `_set_se_translator`
    # hook in UfbImageProviders.cpp can convert SEH into C++
    # exceptions caught by the surrounding try/catch. Scoped to
    # the one source file that registers and uses the translator
    # — the rest of the app stays on the cheaper `/EHsc`.
    set_source_files_properties(UfbImageProviders.cpp PROPERTIES
        COMPILE_OPTIONS "/EHa"
    )

    # WHOLEARCHIVE pulls in object files from Rust crates that reference
    # extra Windows libraries (cldapi for cloud_filter, propsys for
    # PROPVARIANT helpers, runtimeobject for WinRT error originating).
    # Without these the link fails on unresolved __imp_Cf*, __imp_PropVariant*
    # and __imp_RoOriginateErrorW symbols.
    target_link_libraries(ufb PRIVATE
        cldapi          # cloud_filter — Cf* APIs
        propsys         # PropVariant helpers (windows-result)
        runtimeobject   # WinRT RoOriginateError (windows-result)
        pathcch         # arboard — PathCchStripPrefix
        bcrypt          # getrandom — BCryptGenRandom
    )
endif()

if(APPLE)
    # macOS equivalent of /WHOLEARCHIVE:bindings.lib — `-Wl,-force_load`
    # against libbindings.a forces every object file in the Rust static
    # archive to be linked, including the cxx-qt static initializers
    # that register QML types. Without this, ld dead-strips them and
    # the QML engine reports "module \"Ufb.Backend\" is not installed".
    # Surfaced during the slice-07 dogfood test.
    #
    # `bindings` is a Corrosion-imported target, not a normal CMake
    # library target, so $<TARGET_FILE:bindings> doesn't resolve.
    # Corrosion places the staticlib at ${CMAKE_BINARY_DIR}/libbindings.a.
    target_link_options(ufb PRIVATE
        "-Wl,-force_load,${CMAKE_BINARY_DIR}/libbindings.a"
    )
endif()

if(WIN32)
    # GUI subsystem (WIN32_EXECUTABLE TRUE) only for true Release
    # builds so the installer ships without a console window. The
    # `debug` preset is actually RelWithDebInfo (Rust ABI requires
    # /MD across the C++/Rust boundary), so we explicitly include
    # only Release here - leaving RelWithDebInfo on the console
    # subsystem keeps qInstallMessageHandler's stderr output visible
    # for dev work.
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set_target_properties(ufb PROPERTIES WIN32_EXECUTABLE TRUE)
    else()
        set_target_properties(ufb PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()
endif()

if(APPLE)
    target_sources(ufb PRIVATE
        # Synchronous psd_sdk File backend — replaces ps::NativeFile on
        # macOS to avoid the GCD-block UAF in the vendored async impl.
        thumbnails/SyncPsdFile.h
        thumbnails/SyncPsdFile.cpp
    )
endif()

if(APPLE)
    set_target_properties(ufb PROPERTIES
        MACOSX_BUNDLE TRUE
        # Drive the bundle's Info.plist from our template so:
        #   • CFBundleIdentifier=dev.ufb.app (was Qt's randomised
        #     com.yourcompany.<hash>.ufb default — TCC consent never
        #     persisted across rebuilds because the identifier
        #     changed every build).
        #   • NS*FolderUsageDescription strings present so macOS
        #     prompts cleanly instead of silently denying.
        #   • CFBundleURLTypes registers ufb:// + union:// schemes
        #     for slice 04's QFileOpenEvent handler.
        # @CMAKE_PROJECT_VERSION@ etc. are filled in via
        # MACOSX_BUNDLE_*_PROPERTIES, set below.
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "UFB"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.ufb.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "${CMAKE_PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${CMAKE_PROJECT_VERSION}"
    )

    # ── macOS 26+ layered app icon (.icon → Assets.car) ────────────────
    # New-format Icon Composer source at assets/icons/ufbmacOS.icon
    # (background-fill + foreground layers). `actool` compiles it to
    # Assets.car; macOS 26 reads CFBundleIconName (set in Info.plist.in)
    # + Assets.car and applies the system squircle + Liquid Glass, so we
    # don't bake our own corners. Same pipeline the QCView-Player sister
    # app uses for a non-Apple-native (CMake/Qt) build. Requires full
    # Xcode — `actool` isn't in Command Line Tools alone. The asset name
    # is the .icon filename stem and MUST match --app-icon and
    # CFBundleIconName ("ufbmacOS").
    set(UFB_APPICON_ICON "${CMAKE_SOURCE_DIR}/assets/icons/ufbmacOS.icon")
    set(UFB_APPICON_NAME "ufbmacOS")
    if(EXISTS "${UFB_APPICON_ICON}")
        set(UFB_ASSETS_DIR "${CMAKE_BINARY_DIR}/AppIconAssets")
        set(UFB_ASSETS_CAR "${UFB_ASSETS_DIR}/Assets.car")
        add_custom_command(
            OUTPUT  "${UFB_ASSETS_CAR}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${UFB_ASSETS_DIR}"
            COMMAND xcrun actool "${UFB_APPICON_ICON}"
                    --compile "${UFB_ASSETS_DIR}"
                    --output-format human-readable-text
                    --notices --warnings --errors
                    --output-partial-info-plist "${UFB_ASSETS_DIR}/partial.plist"
                    --app-icon "${UFB_APPICON_NAME}" --include-all-app-icons
                    --enable-on-demand-resources NO
                    --development-region en
                    --target-device mac
                    --minimum-deployment-target 26.0
                    --platform macosx
            DEPENDS "${UFB_APPICON_ICON}"
            COMMENT "Compiling ${UFB_APPICON_NAME}.icon → Assets.car (actool)"
            VERBATIM
        )
        set_source_files_properties("${UFB_ASSETS_CAR}"
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(ufb PRIVATE "${UFB_ASSETS_CAR}")
        message(STATUS "UFB: macOS layered icon ${UFB_APPICON_NAME}.icon → Assets.car")
    else()
        message(WARNING "UFB: ${UFB_APPICON_ICON} not found — app icon skipped")
    endif()
    # Bundle leaf is `ufb.app` for the dev tree (matches the linker
    # target's lowercase name and every downstream script that
    # references it). The DMG packaging step in
    # `scripts/build-mac-dmg.sh` renames the copy inside the staged
    # tree to `UFB.app` so end users see the cased form in
    # /Applications/UFB/.

    # Objective-C++ sources for macOS-only surfaces.
    # MacOSMigration (Tauri-era first-launch cleanup) deleted 1.0.7 —
    # every fleet mac ran it long ago. CredentialPrompt_mac.mm deleted
    # with the credential subsystem (plans/17 slices C+B) — the
    # Keychain via NetFS owns sign-in.
    # OBJCXX language is enabled at the top-level CMakeLists.txt so it's
    # in scope before qt_add_executable runs.
    target_sources(ufb PRIVATE
        # Slice 04 — Qt-app shell macOS items.
        MacOSBootstrap.h
        MacOSBootstrap.mm
        MacWindowChrome.h
        MacWindowChrome.mm
        MacAccent.h
        MacAccent.mm
        # Slice 07 — Accessibility probe for the AppleScript-mediated
        # "Show Shell Menu" path.
        MacAccessibility.h
        MacAccessibility.mm
    )
    target_link_libraries(ufb PRIVATE
        "-framework Foundation"
        "-framework AppKit"
        # OS-native icons + thumbnails: SystemIcons_mac.mm uses
        # UniformTypeIdentifiers (UTType); ShellThumbnail_mac.mm uses
        # QuickLookThumbnailing; both render via CoreGraphics.
        "-framework UniformTypeIdentifiers"
        "-framework QuickLookThumbnailing"
        "-framework CoreGraphics"
    )

    # ── Slice 06: bundle vendored dylibs into UFB.app/Contents/Frameworks/
    # ──
    # macdeployqt (slice 08) handles Qt frameworks; this POST_BUILD
    # step lays the ffmpeg + PDFium dylibs alongside them. Both have
    # already been install_name_tool-rewritten to @rpath/<name> by
    # the build/setup scripts, so the executable's @executable_path
    # /../Frameworks rpath (added below) resolves them at runtime.
    set(_ufb_ffmpeg_dylib_glob "${CMAKE_SOURCE_DIR}/external/ffmpeg/lib/lib*.dylib")
    set(_ufb_pdfium_dylib "${CMAKE_SOURCE_DIR}/external/pdfium/lib/libpdfium.dylib")

    add_custom_command(TARGET ufb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:ufb>/Frameworks"
        # `cmake -E copy` on a glob: fan out via the shell so missing
        # files don't trip ENOENT (the `|| true` keeps the build green
        # if the user hasn't run scripts/setup-external-mac.sh yet —
        # they'll see the OpenEXR/ffmpeg/PDFium messages from
        # cmake/external.cmake telling them what to do).
        COMMAND bash -c "cp ${_ufb_ffmpeg_dylib_glob} '$<TARGET_BUNDLE_CONTENT_DIR:ufb>/Frameworks/' 2>/dev/null || true"
        COMMAND bash -c "cp '${_ufb_pdfium_dylib}' '$<TARGET_BUNDLE_CONTENT_DIR:ufb>/Frameworks/' 2>/dev/null || true"
        COMMENT "Copy vendored dylibs (ffmpeg + PDFium) into UFB.app/Contents/Frameworks/"
        VERBATIM
    )

    # Add @executable_path/../Frameworks to the executable's runtime
    # path so the bundled dylibs resolve from inside the .app.
    set_target_properties(ufb PROPERTIES
        INSTALL_RPATH "@executable_path/../Frameworks"
        BUILD_WITH_INSTALL_RPATH TRUE)

    # ── macdeployqt — embed Qt frameworks + plugins + QML modules
    # ──
    # Without this the .app would only run with
    # DYLD_FALLBACK_FRAMEWORK_PATH set to Qt's install dir; double-
    # clicking from Finder would crash with
    # "Library not loaded: @rpath/QtQuickControls2.framework/...".
    # macdeployqt copies the needed Qt frameworks into Contents/
    # Frameworks/, the needed plugins into Contents/PlugIns/, and
    # writes Contents/Resources/qt.conf to point Qt at them. Surfaced
    # during the slice-07 dogfood test.
    #
    # Slice 08 wraps this + codesign + notarize in `just mac-bundle`;
    # for now running it from the build step makes the .app
    # immediately launchable from Finder.
    #
    # `-no-strip` keeps debug symbols. `-qmldir=` is essential —
    # macdeployqt scans QML files for `import` statements and bundles
    # the modules they reference.
    get_target_property(_qt_qmake_loc Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_qmake_loc}" DIRECTORY)
    set(MACDEPLOYQT_EXE "${_qt_bin_dir}/macdeployqt")

    if(EXISTS "${MACDEPLOYQT_EXE}")
        add_custom_command(TARGET ufb POST_BUILD
            COMMAND "${MACDEPLOYQT_EXE}"
                "$<TARGET_BUNDLE_DIR:ufb>"
                "-qmldir=${CMAKE_CURRENT_SOURCE_DIR}/qml"
                "-no-strip"
                # Intentionally NOT passing `-codesign=...` here —
                # macdeployqt's auto-sign trips on the templates/
                # dir we mirror under MacOS/ (sealed-resource
                # validation rejects the .gitkeep paths). Slice 08's
                # codesign-mac.sh does proper deep signing post-
                # macdeployqt; for the in-tree dev build we skip
                # codesign and let the app run unsigned.
            COMMENT "Running macdeployqt (Qt frameworks + plugins + QML)"
            VERBATIM
        )
        # macdeployqt bundles EVERY Qt SQL driver once we link Qt6::Sql,
        # but UFB only uses QSQLITE (the thumbnail cache). The Postgres /
        # Mimer / ODBC drivers link against libraries that aren't present
        # (libpq, libmimerapi), so macdeployqt emits otool errors and we
        # would otherwise ship dylibs with dangling @rpath references.
        # Drop them — keep only libqsqlite.dylib.
        add_custom_command(TARGET ufb POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E rm -f
                "$<TARGET_BUNDLE_DIR:ufb>/Contents/PlugIns/sqldrivers/libqsqlpsql.dylib"
                "$<TARGET_BUNDLE_DIR:ufb>/Contents/PlugIns/sqldrivers/libqsqlmimer.dylib"
                "$<TARGET_BUNDLE_DIR:ufb>/Contents/PlugIns/sqldrivers/libqsqlodbc.dylib"
            COMMENT "Pruning unused Qt SQL drivers (keep QSQLITE only)"
            VERBATIM
        )
        # Replace macdeployqt's leftover ad-hoc signatures on Qt
        # frameworks + plugins with Developer-ID-Application signatures
        # bound to UFB.entitlements. Stable cdhash → TCC remembers
        # granted permissions across rebuilds; entitled app →
        # SecAccessControl keychain writes succeed (errSecMissingEntitlement
        # otherwise on ad-hoc dev builds).
        # The script falls through with a clear error if the cert
        # isn't available; set UFB_SKIP_DEV_SIGN=1 to skip and ship
        # an unsigned bundle (e.g. on CI without the cert).
        # The Developer ID sign step is intentionally NOT here. It
        # has to be the LAST POST_BUILD command we register on the
        # target, because every other POST_BUILD copies into
        # Contents/Resources/ (icons, templates, the LaunchAgent
        # plist template) and signing before those copies seals
        # over a bundle that's still missing files — `codesign
        # --verify` would then report "a sealed resource is missing
        # or invalid". The sign call lives at the end of this file
        # in its own `if(APPLE)` block.
    else()
        message(WARNING "macdeployqt not found at ${MACDEPLOYQT_EXE} — "
                "the resulting .app will only run with "
                "DYLD_FALLBACK_FRAMEWORK_PATH set to your Qt install.")
    endif()
endif()

# App icon embedded as a Qt resource. The exe's RT_GROUP_ICON resource
# (via app/ufb.rc) covers OS shell surfaces - File Explorer, Programs
# and Features, file properties. The Qt window-icon path on the other
# hand is `setWindowIcon(QIcon(":/icons/icon.png"))` in main.cpp - it
# needs an in-binary Qt resource because the filesystem path requires
# qico.dll to decode .ico, and one missing plugin / qt.conf quirk on
# a remote machine takes the taskbar / Alt-Tab icon back to generic.
# PNG decoding is built into QtGui and never depends on a plugin.
qt_add_resources(ufb "app_icons"
    PREFIX "/"
    FILES
        icons/icon.png
)

# Project templates (used by "New Job from Template" UI). On Windows
# this lands next to ufb.exe (`<exe_dir>/templates/...`). On macOS it
# lands in Contents/Resources/ — codesign rejects loose directories
# under Contents/MacOS/, and Apple's bundle convention puts
# non-executable assets there anyway. The runtime lookup in
# `subscription.rs::create_from_template` checks both locations.
if(APPLE)
    add_custom_command(TARGET ufb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/templates
            $<TARGET_BUNDLE_CONTENT_DIR:ufb>/Resources/templates
        COMMENT "Sync templates/ into UFB.app/Contents/Resources/"
    )
else()
    add_custom_command(TARGET ufb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/templates
            $<TARGET_FILE_DIR:ufb>/templates
        COMMENT "Sync templates/ next to ufb.exe"
    )
endif()

# Icon assets. main.cpp first tries the Qt resource (`:/icons/icon.png`),
# then falls back to a filesystem path. On Windows and Linux that path
# is `<exe_dir>/icons/`; on macOS it's `Contents/Resources/icons/`
# alongside the templates above. Same reasoning — loose dirs under
# Contents/MacOS/ break codesign.
if(APPLE)
    add_custom_command(TARGET ufb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/icons
            $<TARGET_BUNDLE_CONTENT_DIR:ufb>/Resources/icons
        COMMENT "Sync icons/ into UFB.app/Contents/Resources/"
    )
else()
    add_custom_command(TARGET ufb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/icons
            $<TARGET_FILE_DIR:ufb>/icons
        COMMENT "Sync icons/ next to ufb.exe"
    )
endif()

# windeployqt — Windows parallel of macdeployqt.
#
# Stages every Qt DLL, plugin, and QML module the binary needs into
# `<exe_dir>/`, plus a `qt.conf` that points Qt at them. After this
# runs, double-clicking `ufb.exe` (or launching it from any cmd shell
# without env wrangling) Just Works — no need for `set PATH=…` or
# `set QML_IMPORT_PATH=…`.
#
# `--qmldir <app/qml>` is critical: windeployqt scans .qml files in
# that directory for `import` statements and bundles every referenced
# module. Without it, a runtime `module "QtQuick.Dialogs" is not
# installed` is the standard symptom.
#
# Skipped:
#   --no-translations         — we don't ship translations
#   --no-system-d3d-compiler  — already on every Windows install
#   --no-opengl-sw            — software OpenGL fallback (~20 MB) we
#                                don't use; QtQuick is RHI-direct
#   --no-compiler-runtime     — VS C runtime: assume already installed
#                                on dev boxes; the installer handles it
#                                for end users separately
if(WIN32)
    get_target_property(_ufb_qt_qmake_loc Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_ufb_qt_bin_dir "${_ufb_qt_qmake_loc}" DIRECTORY)
    set(WINDEPLOYQT_EXE "${_ufb_qt_bin_dir}/windeployqt.exe")
    if(EXISTS "${WINDEPLOYQT_EXE}")
        add_custom_command(TARGET ufb POST_BUILD
            COMMAND "${WINDEPLOYQT_EXE}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                --no-compiler-runtime
                "$<TARGET_FILE:ufb>"
            COMMENT "Running windeployqt (Qt DLLs + plugins + QML modules)"
            VERBATIM
        )
    else()
        message(WARNING "windeployqt not found at ${WINDEPLOYQT_EXE} — "
                "ufb.exe will need PATH and QML_IMPORT_PATH set manually.")
    endif()
endif()

# Transcode subprocess binaries — ffmpeg.exe + ffprobe.exe drive the
# encode + frame-count probe; exiftool.exe (with its bundled Perl
# tree) copies metadata onto the output MP4.
# bindings/src/services/transcode.rs's `bundled_tool` helper looks
# for these next to the running exe and falls back to PATH if absent,
# so a dev build without external/ synced still works for users who
# have ffmpeg on PATH — but a packaged install always carries them.
# `skipifsourcedoesntexist`-style softness here too: file(COPY) at
# configure time, so a fresh clone whose external/ is unpopulated
# doesn't fail the configure; the warning surfaces in the Qt log.
if(WIN32)
    # Mirror every payload in external/ffmpeg/bin next to the exe.
    # ffmpeg.exe + ffprobe.exe are dynamically linked against every
    # av* DLL (including avdevice + avfilter, which our C++ thumbnail
    # backend doesn't link), and refuse to load when any are absent —
    # so the right granularity is "ship the whole bin/", not "ship the
    # subset our linker pulled in via IMPORTED_LOCATION." Globbing at
    # configure time means new DLLs added by setup-external.ps1 ride
    # along on the next reconfigure without needing a hand-edit here.
    set(_ufb_ffmpeg_bin "${CMAKE_SOURCE_DIR}/external/ffmpeg/bin")
    file(GLOB _ufb_ffmpeg_payload
        "${_ufb_ffmpeg_bin}/*.dll"
        "${_ufb_ffmpeg_bin}/*.exe"
    )
    foreach(_pay IN LISTS _ufb_ffmpeg_payload)
        get_filename_component(_pay_name "${_pay}" NAME)
        add_custom_command(TARGET ufb POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_pay}"
                "$<TARGET_FILE_DIR:ufb>/${_pay_name}"
            COMMENT "Sync ${_pay_name} next to ufb.exe"
        )
    endforeach()
    if(NOT _ufb_ffmpeg_payload)
        message(STATUS "ufb: external/ffmpeg/bin empty — run scripts/setup-external.ps1")
    endif()

    # pdfium.dll next to ufb.exe. windeployqt covers Qt and the vcpkg
    # toolchain app-local-deploys the vcpkg DLLs (OpenEXR, libheif),
    # but pdfium is vendored in external/ — without this sync a fresh
    # build tree produces a ufb.exe that fails to launch.
    set(_ufb_pdfium_dll "${CMAKE_SOURCE_DIR}/external/pdfium/bin/pdfium.dll")
    if(EXISTS "${_ufb_pdfium_dll}")
        add_custom_command(TARGET ufb POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_ufb_pdfium_dll}"
                "$<TARGET_FILE_DIR:ufb>/pdfium.dll"
            COMMENT "Sync pdfium.dll next to ufb.exe"
        )
    else()
        message(STATUS "ufb: pdfium.dll missing in external/pdfium/bin — "
                "run scripts/setup-external.ps1")
    endif()

    # qt.conf next to ufb.exe — pins Qt's prefix to the exe dir so
    # WebEngine finds translations/qtwebengine_locales and resources/ on
    # machines without a Qt SDK installed (see installer/qt.conf).
    add_custom_command(TARGET ufb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/installer/qt.conf"
            "$<TARGET_FILE_DIR:ufb>/qt.conf"
        COMMENT "Sync qt.conf next to ufb.exe"
    )

    set(_ufb_exiftool "${CMAKE_SOURCE_DIR}/external/exiftool")
    if(EXISTS "${_ufb_exiftool}/exiftool.exe")
        add_custom_command(TARGET ufb POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_ufb_exiftool}/exiftool.exe"
                "$<TARGET_FILE_DIR:ufb>/exiftool.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_ufb_exiftool}/exiftool_files"
                "$<TARGET_FILE_DIR:ufb>/exiftool_files"
            COMMENT "Sync exiftool + exiftool_files next to ufb.exe"
        )
    else()
        message(STATUS "ufb: exiftool missing in external/exiftool — run scripts/setup-external.ps1")
    endif()

    # WinSparkle.dll next to ufb.exe (the Inno [Files] *.dll glob picks
    # it up from there). Updater_win.cpp links WinSparkle.lib; the DLL
    # must sit beside the exe at runtime.
    set(_ufb_winsparkle_dll "${CMAKE_SOURCE_DIR}/external/winsparkle/bin/WinSparkle.dll")
    if(EXISTS "${_ufb_winsparkle_dll}")
        add_custom_command(TARGET ufb POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_ufb_winsparkle_dll}"
                "$<TARGET_FILE_DIR:ufb>/WinSparkle.dll"
            COMMENT "Sync WinSparkle.dll next to ufb.exe"
        )
    else()
        message(STATUS "ufb: WinSparkle.dll missing in external/winsparkle/bin — "
                "auto-update disabled (run scripts/setup-external.ps1)")
    endif()
endif()

if(APPLE)
    # Mirror of the Windows transcode-binary block above, but
    # split across two destinations:
    #
    #   * ffmpeg + ffprobe → UFB.app/Contents/MacOS/. Mach-O, signs
    #     cleanly with hardened runtime; sit next to ufb so the
    #     bundled_tool helper finds them via current_exe()'s parent.
    #     LC_RPATH was set to @executable_path/../Frameworks by
    #     scripts/build-external-ffmpeg-mac.sh, so they resolve the
    #     bundle's libav* dylibs without DYLD_* env vars.
    #
    #   * exiftool + lib/ tree → UFB.app/Contents/Resources/exiftool/.
    #     exiftool is a Perl script, not a Mach-O — Apple's bundle
    #     convention puts non-executables under Resources/, and
    #     Contents/MacOS/ subdirectories trip codesign --strict.
    #     bindings/src/services/transcode.rs::bundled_tool gained a
    #     macOS-only fallback that checks
    #     <exe_dir>/../Resources/<name>/<name> after the local
    #     <exe_dir>/<name> miss.
    set(_ufb_ffmpeg_mac_bin "${CMAKE_SOURCE_DIR}/external/ffmpeg/bin")
    foreach(_tool ffmpeg ffprobe)
        if(EXISTS "${_ufb_ffmpeg_mac_bin}/${_tool}")
            add_custom_command(TARGET ufb POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_ufb_ffmpeg_mac_bin}/${_tool}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:ufb>/MacOS/${_tool}"
                COMMENT "Sync ${_tool} into UFB.app/Contents/MacOS/"
            )
        else()
            message(STATUS "ufb: external/ffmpeg/bin/${_tool} missing — run scripts/setup-external-mac.sh")
        endif()
    endforeach()

    set(_ufb_exiftool_mac "${CMAKE_SOURCE_DIR}/external/exiftool")
    if(EXISTS "${_ufb_exiftool_mac}/exiftool")
        add_custom_command(TARGET ufb POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:ufb>/Resources/exiftool"
            COMMAND ${CMAKE_COMMAND} -E copy
                "${_ufb_exiftool_mac}/exiftool"
                "$<TARGET_BUNDLE_CONTENT_DIR:ufb>/Resources/exiftool/exiftool"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_ufb_exiftool_mac}/lib"
                "$<TARGET_BUNDLE_CONTENT_DIR:ufb>/Resources/exiftool/lib"
            COMMENT "Sync exiftool + lib/ into UFB.app/Contents/Resources/exiftool/"
        )
    else()
        message(STATUS "ufb: exiftool missing in external/exiftool — run scripts/setup-external-mac.sh")
    endif()
endif()

# ── Developer ID sign UFB.app (must be LAST POST_BUILD) ──────────────
# Every prior POST_BUILD writes into UFB.app's Contents/Resources/ —
# Qt frameworks via macdeployqt, icons, templates, the LaunchAgent
# plist template. Signing has to happen after all of those land or
# `codesign --verify` reports "a sealed resource is missing or
# invalid" because the resource hashes embedded in _CodeSignature/
# don't reflect the final bundle state. CMake runs POST_BUILD
# commands in declaration order, so this stays at the bottom of
# the file by convention.
if(APPLE)
    add_custom_command(TARGET ufb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E env
            bash "${CMAKE_SOURCE_DIR}/scripts/sign-mac-dev.sh"
                --only-app
        COMMENT "Developer ID sign UFB.app (scripts/sign-mac-dev.sh)"
        VERBATIM
    )
endif()

# Phase 0: skeleton only. Real install rules come in Phase 14.
install(TARGETS ufb
    RUNTIME DESTINATION bin
    BUNDLE  DESTINATION .
)
install(DIRECTORY templates/ DESTINATION bin/templates)
