cmake_minimum_required(VERSION 3.10)
project(imgui)

# ImGui source files
set(IMGUI_SOURCES
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    backends/imgui_impl_glfw.cpp
    backends/imgui_impl_opengl3.cpp
)

# ImGui header files
set(IMGUI_HEADERS
    imgui.h
    imgui_internal.h
    imconfig.h
    imstb_rectpack.h
    imstb_textedit.h
    imstb_truetype.h
    backends/imgui_impl_glfw.h
    backends/imgui_impl_opengl3.h
)

# Create imgui library
add_library(imgui STATIC ${IMGUI_SOURCES} ${IMGUI_HEADERS})

# Include directories
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/../glad/include
)

# Link with GLFW and glad
target_link_libraries(imgui PUBLIC glfw glad)

# Windows-specific settings
if(WIN32)
    target_link_libraries(imgui PUBLIC opengl32)
endif()
