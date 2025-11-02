#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <set>
#include <filesystem>
#include "imgui.h"
#include "icon_manager.h"
#include "thumbnail_manager.h"

struct FileEntry
{
    std::wstring name;
    std::wstring fullPath;
    bool isDirectory;
    uintmax_t size;
    std::filesystem::file_time_type lastModified;
};

class FileBrowser
{
public:
    FileBrowser();
    ~FileBrowser();

    // Initialize the file browser
    void Initialize();

    // Shutdown and cleanup
    void Shutdown();

    // Draw the file browser UI
    void Draw(const char* title, HWND hwnd, bool withWindow = true);

    // Set the current directory
    void SetCurrentDirectory(const std::wstring& path);

    // Get the current directory
    const std::wstring& GetCurrentDirectory() const { return m_currentDirectory; }

    // Handle external drag-drop from Windows Explorer
    void HandleExternalDrop(const std::vector<std::wstring>& droppedPaths);

    // Get window bounds for drop target detection
    bool GetWindowBounds(ImVec2& outPos, ImVec2& outSize) const;

    // Check if this browser is currently hovered
    bool IsHovered() const { return m_isHovered; }

    // Public settings (static so it's shared across all browser instances)
    static bool showHiddenFiles;

private:
    // Refresh the file list for the current directory
    void RefreshFileList();

    // Navigate up one directory level
    void NavigateUp();

    // Navigate back in history
    void NavigateBack();

    // Navigate forward in history
    void NavigateForward();

    // Navigate to a specific directory
    void NavigateTo(const std::wstring& path);

    // Show context menu for a file/folder (Windows native menu)
    void ShowContextMenu(HWND hwnd, const std::wstring& path, const ImVec2& screenPos);

    // Show ImGui context menu for a file/folder
    void ShowImGuiContextMenu(HWND hwnd, const FileEntry& entry);

    // Draw the navigation bar
    void DrawNavigationBar();

    // Draw the file list
    void DrawFileList(HWND hwnd);

    // Draw the file list in list view (table)
    void DrawListView(HWND hwnd);

    // Draw the file list in grid view (thumbnails)
    void DrawGridView(HWND hwnd);

    // Format file size for display
    std::string FormatFileSize(uintmax_t size);

    // Format file time for display
    std::string FormatFileTime(const std::filesystem::file_time_type& ftime);

    // Current directory path
    std::wstring m_currentDirectory;

    // Navigation history
    std::vector<std::wstring> m_backHistory;
    std::vector<std::wstring> m_forwardHistory;
    bool m_isNavigatingHistory = false;  // Flag to prevent adding to history during back/forward

    // List of files in current directory
    std::vector<FileEntry> m_files;

    // Icon manager
    IconManager m_iconManager;

    // Thumbnail manager
    ThumbnailManager m_thumbnailManager;

    // Special folder paths and icons
    std::wstring m_desktopPath;
    std::wstring m_documentsPath;
    std::wstring m_downloadsPath;
    ImTextureID m_desktopIcon = 0;
    ImTextureID m_documentsIcon = 0;
    ImTextureID m_downloadsIcon = 0;

    // View mode
    enum class ViewMode { List, Grid };
    ViewMode m_viewMode = ViewMode::List;
    float m_thumbnailSize = 150.0f;  // Grid thumbnail size (user-adjustable)

    // Selected file indices (multi-select support)
    std::set<int> m_selectedIndices;

    // Last click time and index for double-click detection and shift-select
    double m_lastClickTime = 0.0;
    int m_lastClickedIndex = -1;

    // Sorting state
    enum class SortColumn { Name, Size, Modified };
    SortColumn m_sortColumn = SortColumn::Name;
    bool m_sortAscending = true;

    // Sort the file list
    void SortFileList();

    // Helper functions for context menu actions
    void CopyToClipboard(const std::wstring& text);
    void CopyFilesToClipboard(const std::vector<std::wstring>& paths);
    void CutFilesToClipboard(const std::vector<std::wstring>& paths);
    void PasteFilesFromClipboard();
    void RevealInExplorer(const std::wstring& path);
    void DeleteFilesToRecycleBin(const std::vector<std::wstring>& paths);

    // Drag and drop helpers
    void CopyFileToDestination(const std::wstring& sourcePath, const std::wstring& destDirectory);
    void CopyFilesToDestination(const std::vector<std::wstring>& sourcePaths, const std::wstring& destDirectory);

    // Get Windows accent color
    ImVec4 GetAccentColor();

    // Context menu state
    std::wstring m_contextMenuPath;
    bool m_showRenameDialog = false;
    char m_renameBuffer[256] = {};
    std::wstring m_renameOriginalPath;

    // New folder dialog state
    bool m_showNewFolderDialog = false;
    char m_newFolderNameBuffer[256] = {};

    // Path bar edit state
    char m_pathBuffer[1024] = {};

    // External drag-drop state
    bool m_isHovered = false;
    ImVec2 m_windowPos = {};
    ImVec2 m_windowSize = {};

    // Cut/copy state (static so it's shared across all browser instances)
    static std::vector<std::wstring> m_cutFiles;  // Files marked for cut operation

    // OLE initialization counter (static so it's shared across all instances)
    static int m_oleRefCount;
};
