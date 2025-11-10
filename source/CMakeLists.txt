cmake_minimum_required(VERSION 3.16)
project(ufb)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add external libraries
add_subdirectory(external/glfw)
add_subdirectory(external/glad)
add_subdirectory(external/imgui)

# SQLite amalgamation (compile as static library)
add_library(sqlite STATIC
    external/sqlite/sqlite3.c
)
target_include_directories(sqlite PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/external/sqlite
)

# Add FFmpeg library directory (must be before add_executable)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/lib)

# Add image library directories
link_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/external/jpeg/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/png/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/tiff/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/lib
)

# Main executable
add_executable(ufb
    src/main.cpp
    src/file_browser.cpp
    src/file_browser.h
    src/icon_manager.cpp
    src/icon_manager.h
    src/utils.cpp
    src/utils.h
    src/texture_utils.cpp
    src/texture_utils.h
    src/thumbnail_extractor.h
    src/extractors/windows_shell_extractor.cpp
    src/extractors/windows_shell_extractor.h
    src/extractors/fallback_icon_extractor.cpp
    src/extractors/fallback_icon_extractor.h
    src/extractors/video_thumbnail_extractor.cpp
    src/extractors/video_thumbnail_extractor.h
    src/extractors/psd_ai_thumbnail_extractor.cpp
    src/extractors/psd_ai_thumbnail_extractor.h
    src/extractors/image_thumbnail_extractor.cpp
    src/extractors/image_thumbnail_extractor.h
    src/extractors/svg_thumbnail_extractor.cpp
    src/extractors/svg_thumbnail_extractor.h
    src/extractors/blend_thumbnail_extractor.cpp
    src/extractors/blend_thumbnail_extractor.h
    src/extractors/exr_extractor.cpp
    src/extractors/exr_extractor.h
    src/thumbnail_manager.cpp
    src/thumbnail_manager.h
    src/subscription_manager.cpp
    src/subscription_manager.h
    src/everything_index_manager.cpp
    src/everything_index_manager.h
    src/metadata_manager.cpp
    src/metadata_manager.h
    src/backup_manager.cpp
    src/backup_manager.h
    src/archival_manager.cpp
    src/archival_manager.h
    src/sync_manager.cpp
    src/sync_manager.h
    src/file_watcher.cpp
    src/file_watcher.h
    src/p2p_manager.cpp
    src/p2p_manager.h
    src/bookmark_manager.cpp
    src/bookmark_manager.h
    src/subscription_panel.cpp
    src/subscription_panel.h
    src/ole_drag_drop.cpp
    src/ole_drag_drop.h
    src/transcode_queue_panel.cpp
    src/transcode_queue_panel.h
    src/deadline_queue_panel.cpp
    src/deadline_queue_panel.h
    src/deadline_submit_dialog.cpp
    src/deadline_submit_dialog.h
    src/settings_dialog.cpp
    src/settings_dialog.h
    src/project_config.cpp
    src/project_config.h
    src/shot_view.cpp
    src/shot_view.h
    src/assets_view.cpp
    src/assets_view.h
    src/postings_view.cpp
    src/postings_view.h
    src/project_tracker_view.cpp
    src/project_tracker_view.h
    src/aggregated_tracker_view.cpp
    src/aggregated_tracker_view.h
    src/console_panel.cpp
    src/console_panel.h
    src/ImGuiDatePicker.cpp
    src/ImGuiDatePicker.hpp
    build/ufb.rc
)

target_include_directories(ufb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/external/glfw/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/glad/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/sqlite
    ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann
    ${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/jpeg/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/png/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/tiff/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/include/OpenEXR
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/include/Imath
    ${CMAKE_CURRENT_SOURCE_DIR}/external/nanosvg
)

target_link_libraries(ufb PRIVATE
    imgui
    glfw
    glad
    sqlite
    opengl32
    shell32
    ole32
    shlwapi
    comctl32
    winhttp
    # FFmpeg libraries
    avformat
    avcodec
    avutil
    swscale
    # Image libraries
    jpeg
    libpng16
    tiff
    # OpenEXR libraries (Imath must be first for half-float support)
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/lib/Imath-3_2.lib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/lib/Iex-3_3.lib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/lib/IlmThread-3_3.lib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/lib/OpenEXRCore-3_3.lib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/lib/OpenEXRUtil-3_3.lib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/lib/OpenEXR-3_3.lib
)

# Console window control
# Default: hide console (using in-app console instead)
# Override with: cmake -DUFB_SHOW_CONSOLE=ON or OFF
if(NOT DEFINED UFB_SHOW_CONSOLE)
    set(UFB_SHOW_CONSOLE OFF)  # Use in-app console by default
endif()
option(UFB_SHOW_CONSOLE "Show console window for debugging" ${UFB_SHOW_CONSOLE})

if(WIN32)
    # Hide console window unless UFB_SHOW_CONSOLE is enabled
    if(NOT UFB_SHOW_CONSOLE)
        set_target_properties(ufb PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()

    # Allow using main() instead of WinMain() for WIN32 subsystem
    if(MSVC)
        set_target_properties(ufb PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
    endif()

    # Link additional Windows libraries for accent color
    target_link_libraries(ufb PRIVATE dwmapi)
endif()

# Copy assets folder to build directory
add_custom_command(TARGET ufb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets
    $<TARGET_FILE_DIR:ufb>/assets
    COMMENT "Copying assets to build directory..."
)

# Copy FFmpeg DLLs to build directory
add_custom_command(TARGET ufb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/bin
    $<TARGET_FILE_DIR:ufb>
    COMMENT "Copying FFmpeg DLLs to build directory..."
)

# Copy ImageMagick to build directory
add_custom_command(TARGET ufb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/external/magick
    $<TARGET_FILE_DIR:ufb>/magick
    COMMENT "Copying ImageMagick to build directory..."
)

# Copy Ghostscript to build directory
add_custom_command(TARGET ufb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/external/ghostscript
    $<TARGET_FILE_DIR:ufb>/ghostscript
    COMMENT "Copying Ghostscript to build directory..."
)

# Copy OpenEXR DLLs to build directory
add_custom_command(TARGET ufb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/bin
    $<TARGET_FILE_DIR:ufb>
    COMMENT "Copying OpenEXR DLLs to build directory..."
)

# Copy Everything Search (es.exe) to build directory
add_custom_command(TARGET ufb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/external/everything/es.exe
    $<TARGET_FILE_DIR:ufb>
    COMMENT "Copying Everything Search (es.exe) to build directory..."
)
