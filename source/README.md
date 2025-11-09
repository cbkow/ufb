# ufb (Union File Browser)

A lightweight Windows file browser application built with Dear ImGui, featuring:

- **UnionPlayer Theme**: Professional dark theme with system accent color integration
- **Custom Fonts**: Inter (regular) and JetBrains Mono (monospace) fonts
- **Docking Layout**: Full ImGui docking support for flexible window arrangements
- **Native Windows Icons**: Shell-integrated file and folder icons
- **Context Menus**: Right-click context menus with native Windows integration
- **File Navigation**: Browse directories, quick access to common folders
- **No Console Window**: Clean GUI application without command prompt
- **OpenGL Rendering**: Uses GLFW and OpenGL for cross-platform windowing

## Features

- **Professional Theme**: Dark UI inspired by UnionPlayer with consistent styling
- **System Integration**:
  - Windows accent color support (toggleable in View menu)
  - Dark mode window title bar
  - Real Windows shell icons cached by file extension
  - Native Windows context menus (Open, Properties, etc.)
- **Typography**:
  - Inter font for UI text
  - JetBrains Mono for file paths and sizes
- **File Management**:
  - Double-click to open files or navigate into directories
  - Quick access buttons for Home, Desktop, Documents
  - Hide/Show hidden files option
  - File size formatting (B, KB, MB, GB, TB)
  - Sortable file list (directories first, then alphabetically)

## Building

### Prerequisites

- CMake 3.16 or higher
- Visual Studio 2019 or later (or another C++17 compatible compiler)
- Windows SDK

### Build Steps

```bash
cd ImGuiFileBrowser
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

The executable will be in `build/Release/ufb.exe`.

## Project Structure

```
ImGuiFileBrowser/
├── src/
│   ├── main.cpp              # Application entry point with GLFW/OpenGL setup
│   ├── file_browser.h/cpp    # Main file browser widget implementation
│   └── icon_manager.h/cpp    # Windows shell icon caching system
├── assets/
│   └── fonts/                # Inter and JetBrains Mono fonts
├── external/                 # Third-party libraries (ImGui, GLFW, glad)
├── build/                    # CMake build output
└── CMakeLists.txt           # CMake configuration
```

## Usage

- **Navigate**: Double-click folders to enter them
- **Open Files**: Double-click files to open with default application
- **Context Menu**: Right-click any file/folder for options
- **Up Button**: Click "^" to go up one directory level
- **Quick Access**: Use Home/Desktop/Documents buttons for common locations
- **Hidden Files**: Toggle via View menu

## Technical Details

This project adapts Dear ImGui for file browsing with Windows shell integration:

- **Icon Caching**: Icons are retrieved via `SHGetFileInfo` and cached by extension
- **OpenGL Textures**: HICON converted to OpenGL textures for ImGui rendering
- **File Operations**: Uses `ShellExecute` for opening files and showing properties
- **Docking**: Leverages ImGui's docking branch for flexible layouts

## License

Check individual library licenses in the `external/` directory.

## Credits

Based on sketch concepts and built using:
- Dear ImGui
- GLFW
- glad (OpenGL loader)
