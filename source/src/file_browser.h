#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <functional>
#include <filesystem>
#include "imgui.h"
#include "icon_manager.h"
#include "thumbnail_manager.h"

// Forward declarations
namespace UFB {
    class BookmarkManager;
    class SubscriptionManager;
}

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

    // Initialize the file browser with optional manager dependencies
    void Initialize(UFB::BookmarkManager* bookmarkManager = nullptr, UFB::SubscriptionManager* subscriptionManager = nullptr);

    // Shutdown and cleanup
    void Shutdown();

    // Draw the file browser UI
    void Draw(const char* title, HWND hwnd, bool withWindow = true);

    // Set the current directory
    void SetCurrentDirectory(const std::wstring& path);

    // Set the current directory and select a specific file
    void SetCurrentDirectoryAndSelectFile(const std::wstring& directoryPath, const std::wstring& filePathToSelect);

    // Get the current directory
    const std::wstring& GetCurrentDirectory() const { return m_currentDirectory; }

    // Check if window is open (for cleanup of closed windows)
    bool IsOpen() const { return m_isOpen; }

    // Handle external drag-drop from Windows Explorer
    void HandleExternalDrop(const std::vector<std::wstring>& droppedPaths);

    // Get window bounds for drop target detection
    bool GetWindowBounds(ImVec2& outPos, ImVec2& outSize) const;

    // Check if this browser is currently hovered
    bool IsHovered() const { return m_isHovered; }

    // Public settings (static so it's shared across all browser instances)
    static bool showHiddenFiles;

    // Callback for transcoding video files
    std::function<void(const std::vector<std::wstring>&)> onTranscodeToMP4;

    // Callback for opening shot view
    std::function<void(const std::wstring& categoryPath, const std::wstring& categoryName)> onOpenShotView;

    // Callback for opening assets view
    std::function<void(const std::wstring& assetsFolderPath, const std::wstring& jobName)> onOpenAssetsView;

    // Callback for opening postings view
    std::function<void(const std::wstring& postingsFolderPath, const std::wstring& jobName)> onOpenPostingsView;

    // Callback for opening path in other browser
    std::function<void(const std::wstring& path)> onOpenInOtherBrowser;

    // Callback for opening path in new standalone browser window
    std::function<void(const std::wstring& path)> onOpenInNewWindow;

    // Callbacks for opening path in Browser 1 or Browser 2 (used in specialized views)
    std::function<void(const std::wstring& path)> onOpenInBrowser1;
    std::function<void(const std::wstring& path)> onOpenInBrowser2;

    // Callback for adding custom context menu items
    // Called during context menu rendering, passes selected file paths
    std::function<void(const std::vector<std::wstring>&)> onCustomContextMenu;

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

    // Create new job folder from template
    void CreateJobFromTemplate(const std::string& jobNumber, const std::string& jobName);

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
    mutable std::mutex m_filesMutex;  // Protects m_files from concurrent access

    // Icon manager
    IconManager m_iconManager;

    // Thumbnail manager
    ThumbnailManager m_thumbnailManager;

    // Manager dependencies (for project/job features)
    UFB::BookmarkManager* m_bookmarkManager = nullptr;
    UFB::SubscriptionManager* m_subscriptionManager = nullptr;

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

    // Box selection state (grid view only)
    bool m_isBoxSelecting = false;
    bool m_boxSelectDragged = false;  // Track if we actually dragged during box selection
    ImVec2 m_boxSelectStart = ImVec2(0, 0);
    std::vector<std::pair<ImVec2, ImVec2>> m_itemBounds;  // Bounds for each grid item (min, max)

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

    // Folder creation helpers
    bool CreateUFBFolder(const std::string& folderName);
    bool CreateDateFolder();
    bool CreateTimeFolder();

    // Context menu state
    std::wstring m_contextMenuPath;
    bool m_showRenameDialog = false;
    char m_renameBuffer[256] = {};
    std::wstring m_renameOriginalPath;

    // New folder dialog state
    bool m_showNewFolderDialog = false;
    char m_newFolderNameBuffer[256] = {};

    // New u.f.b. folder dialog state
    bool m_showNewUFBFolderDialog = false;
    char m_newUFBFolderNameBuffer[256] = {};

    // New job dialog state
    bool m_showNewJobDialog = false;
    char m_newJobNumberBuffer[64] = {};
    char m_newJobNameBuffer[256] = {};

    // Path bar edit state
    char m_pathBuffer[1024] = {};              // Path input buffer

    // File extension filter (multi-selection)
    std::set<std::wstring> m_filterExtensions;  // Empty = show all, can contain multiple extensions and/or "[folders]"

    // Search state
    bool m_isSearchMode = false;                    // True when showing search results
    char m_searchQuery[256] = {};                   // Current search query
    std::wstring m_preSearchDirectory;              // Directory before search (to return to)
    int m_searchResultCount = 0;                    // Number of search results found

    // Search helper methods
    void ExecuteSearch(const std::string& query);   // Execute es.exe and populate results
    void ExitSearchMode();                          // Return to pre-search directory
    void ShowInBrowser(const std::wstring& filePath); // Navigate to file's parent directory and select it

    // Window open state (for close button)
    bool m_isOpen = true;

    // External drag-drop state
    bool m_isHovered = false;
    ImVec2 m_windowPos = {};
    ImVec2 m_windowSize = {};

    // OLE drag transition flags (prevent internal drop after external drop)
    bool m_transitionedToOLEDrag_list = false;  // For list view
    bool m_transitionedToOLEDrag_grid = false;  // For grid view

    // Cut/copy state (static so it's shared across all browser instances)
    static std::vector<std::wstring> m_cutFiles;  // Files marked for cut operation

    // OLE initialization counter (static so it's shared across all instances)
    static int m_oleRefCount;
};
