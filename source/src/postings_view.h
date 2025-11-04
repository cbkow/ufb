#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <functional>
#include <filesystem>
#include "imgui.h"
#include "icon_manager.h"
#include "thumbnail_manager.h"
#include "file_browser.h"

// Forward declarations
namespace UFB {
    class BookmarkManager;
    class SubscriptionManager;
    class ProjectConfig;
    struct ShotMetadata;
}

struct FileEntry;

class PostingsView
{
public:
    PostingsView();
    ~PostingsView();

    // Initialize with postings folder path (e.g., "D:\Projects\MyJob\postings")
    void Initialize(const std::wstring& postingsFolderPath, const std::wstring& jobName,
                    UFB::BookmarkManager* bookmarkManager = nullptr,
                    UFB::SubscriptionManager* subscriptionManager = nullptr);

    // Shutdown and cleanup
    void Shutdown();

    // Draw the postings view UI (2-panel layout)
    void Draw(const char* title, HWND hwnd);

    // Get the postings folder path
    const std::wstring& GetPostingsFolderPath() const { return m_postingsFolderPath; }

    // Get the job name (for window title)
    const std::wstring& GetJobName() const { return m_jobName; }

    // Set the selected posting by path
    void SetSelectedPosting(const std::wstring& postingPath);

    // Callback for closing this view
    std::function<void()> onClose;

    // Callbacks for opening path in browsers
    std::function<void(const std::wstring& path)> onOpenInBrowser1;
    std::function<void(const std::wstring& path)> onOpenInBrowser2;

    // Callback for transcoding video files
    std::function<void(const std::vector<std::wstring>&)> onTranscodeToMP4;

    // Handle external drag-drop from Windows Explorer (delegates to browser panel)
    void HandleExternalDrop(const std::vector<std::wstring>& droppedPaths);

    // Check if the browser panel is currently hovered
    bool IsBrowserHovered() const;

    // Public settings (share with FileBrowser)
    static bool showHiddenFiles;

private:
    // Postings folder path and job name
    std::wstring m_postingsFolderPath;  // e.g., "D:\Projects\MyJob\postings"
    std::wstring m_jobName;             // e.g., "MyJob"

    // Manager dependencies
    UFB::BookmarkManager* m_bookmarkManager = nullptr;
    UFB::SubscriptionManager* m_subscriptionManager = nullptr;
    UFB::ProjectConfig* m_projectConfig = nullptr;

    // Icon and thumbnail managers for left panel
    IconManager m_iconManager;
    ThumbnailManager m_thumbnailManager;

    // File browser for right panel
    FileBrowser m_fileBrowser;

    // File list for left panel
    std::vector<FileEntry> m_postingItems;  // Files/folders in postings folder

    // Selected posting index
    int m_selectedPostingIndex = -1;

    // Window state
    bool m_isOpen = true;

    // Refresh file list
    void RefreshPostingItems();

    // Draw each panel
    void DrawPostingsPanel(HWND hwnd);
    void DrawBrowserPanel(HWND hwnd);

    // Show context menu for a file/folder
    void ShowImGuiContextMenu(HWND hwnd, const FileEntry& entry);

    // Helper functions
    void CopyToClipboard(const std::wstring& text);
    void CopyFilesToClipboard(const std::vector<std::wstring>& paths);
    void CutFilesToClipboard(const std::vector<std::wstring>& paths);
    void PasteFilesFromClipboard();
    void RevealInExplorer(const std::wstring& path);
    void DeleteFilesToRecycleBin(const std::vector<std::wstring>& paths);

    // Format helpers
    std::string FormatFileSize(uintmax_t size);
    std::string FormatFileTime(const std::filesystem::file_time_type& ftime);
    std::string FormatTimestamp(uint64_t timestamp);

    // Get Windows accent color
    ImVec4 GetAccentColor();

    // Metadata helpers
    void LoadMetadata();
    void LoadColumnVisibility();
    void SaveColumnVisibility();
    ImVec4 GetStatusColor(const std::string& status);
    ImVec4 GetCategoryColor(const std::string& category);

    // Helper method to create a new posting folder with YYMMDD versioning
    bool CreateNewPosting(const std::string& postingName);

    // Context menu state
    std::wstring m_contextMenuPath;
    bool m_showRenameDialog = false;
    char m_renameBuffer[256] = {};
    std::wstring m_renameOriginalPath;

    // Add new posting modal state
    bool m_showAddPostingDialog = false;
    char m_newPostingNameBuffer[256] = {};

    // Cut/copy state (static shared)
    static std::vector<std::wstring> m_cutFiles;

    // OLE initialization counter
    static int m_oleRefCount;

    // Selected file indices for multi-select
    std::set<int> m_selectedPostingIndices;

    // Last click tracking for double-click detection
    double m_lastClickTime = 0.0;
    int m_lastClickedPostingIndex = -1;

    // Sorting state
    ImGuiTableColumnSortSpecs m_postingSortSpecs = {};

    // Metadata management
    std::map<std::wstring, UFB::ShotMetadata> m_postingMetadataMap;  // Posting metadata cache
    std::map<std::string, bool> m_visibleColumns;                    // Column visibility
    bool m_showColumnsPopup = false;

    // Panel window positions and sizes
    ImVec2 m_postingsPanelPos = {};
    ImVec2 m_postingsPanelSize = {};
    ImVec2 m_browserPanelPos = {};
    ImVec2 m_browserPanelSize = {};

    // Date picker state
    bool m_showDatePicker = false;
    int m_datePickerPostingIndex = -1;
};
