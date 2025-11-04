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

// Forward declarations
namespace UFB {
    class BookmarkManager;
    class SubscriptionManager;
    class ProjectConfig;
    struct ShotMetadata;
}

struct FileEntry;  // Use same FileEntry from file_browser.h

class ShotView
{
public:
    ShotView();
    ~ShotView();

    // Initialize with shot category path (e.g., "D:\Projects\MyJob\ae")
    void Initialize(const std::wstring& categoryPath, const std::wstring& categoryName,
                    UFB::BookmarkManager* bookmarkManager = nullptr,
                    UFB::SubscriptionManager* subscriptionManager = nullptr);

    // Shutdown and cleanup
    void Shutdown();

    // Draw the shot view UI (3-panel layout)
    void Draw(const char* title, HWND hwnd);

    // Get the category path
    const std::wstring& GetCategoryPath() const { return m_categoryPath; }

    // Get the category name (for window title)
    const std::wstring& GetCategoryName() const { return m_categoryName; }

    // Get the job name (parent folder of category)
    std::wstring GetJobName() const;

    // Set the selected shot by path
    void SetSelectedShot(const std::wstring& shotPath);

    // Callback for closing this view
    std::function<void()> onClose;

    // Callback for transcoding video files
    std::function<void(const std::vector<std::wstring>&)> onTranscodeToMP4;

    // Callbacks for opening path in browsers
    std::function<void(const std::wstring& path)> onOpenInBrowser1;
    std::function<void(const std::wstring& path)> onOpenInBrowser2;

    // Public settings (share with FileBrowser)
    static bool showHiddenFiles;

private:
    // Category path and name
    std::wstring m_categoryPath;     // e.g., "D:\Projects\MyJob\ae"
    std::wstring m_categoryName;     // e.g., "ae"

    // Manager dependencies
    UFB::BookmarkManager* m_bookmarkManager = nullptr;
    UFB::SubscriptionManager* m_subscriptionManager = nullptr;
    UFB::ProjectConfig* m_projectConfig = nullptr;

    // Icon and thumbnail managers
    IconManager m_iconManager;
    ThumbnailManager m_thumbnailManager;

    // File lists for each panel
    std::vector<FileEntry> m_shots;          // Directories in category root
    std::vector<FileEntry> m_projectFiles;   // Files from selected shot's project/projects folder
    std::vector<FileEntry> m_renderFiles;    // Files from selected shot's renders/outputs folder

    // Selected shot index
    int m_selectedShotIndex = -1;
    std::wstring m_selectedShotPath;

    // Renders panel navigation state
    std::wstring m_renderCurrentDirectory;  // Current directory being viewed in renders panel
    std::vector<std::wstring> m_renderBackHistory;
    std::vector<std::wstring> m_renderForwardHistory;
    bool m_isNavigatingRenderHistory = false;

    // Window state
    bool m_isOpen = true;

    // Refresh file lists
    void RefreshShots();
    void RefreshProjectFiles();
    void RefreshRenderFiles();

    // Navigation methods for renders panel
    void NavigateToRenderDirectory(const std::wstring& path);
    void NavigateRenderUp();
    void NavigateRenderBack();
    void NavigateRenderForward();

    // Draw each panel
    void DrawShotsPanel(HWND hwnd);
    void DrawProjectsPanel(HWND hwnd);
    void DrawRendersPanel(HWND hwnd);

    // Panel identifiers for context menus
    enum class PanelType {
        Shots,
        Projects,
        Renders
    };

    // Show context menu for a file/folder (ImGui context menu with same options as FileBrowser)
    void ShowImGuiContextMenu(HWND hwnd, const FileEntry& entry, PanelType panelType);

    // Helper functions (same as FileBrowser)
    void CopyToClipboard(const std::wstring& text);
    void CopyFilesToClipboard(const std::vector<std::wstring>& paths);
    void CutFilesToClipboard(const std::vector<std::wstring>& paths);
    void PasteFilesFromClipboard();
    void RevealInExplorer(const std::wstring& path);
    void DeleteFilesToRecycleBin(const std::vector<std::wstring>& paths);

    // Format helpers
    std::string FormatFileSize(uintmax_t size);
    std::string FormatFileTime(const std::filesystem::file_time_type& ftime);
    std::string FormatTimestamp(uint64_t timestamp);  // Format Unix timestamp to date string

    // Get Windows accent color
    ImVec4 GetAccentColor();

    // Metadata helpers
    void LoadMetadata();                                          // Load shot metadata from SubscriptionManager
    void LoadColumnVisibility();                                  // Load column visibility from ProjectConfig
    void SaveColumnVisibility();                                  // Save column visibility to ProjectConfig
    ImVec4 GetStatusColor(const std::string& status);             // Get status color from ProjectConfig
    ImVec4 GetCategoryColor(const std::string& category);         // Get category color from ProjectConfig

    // Context menu state
    std::wstring m_contextMenuPath;
    bool m_showRenameDialog = false;
    char m_renameBuffer[256] = {};
    std::wstring m_renameOriginalPath;

    // Cut/copy state (static shared with FileBrowser)
    static std::vector<std::wstring> m_cutFiles;

    // OLE initialization counter
    static int m_oleRefCount;

    // Selected file indices for multi-select (per panel)
    std::set<int> m_selectedShotIndices;
    std::set<int> m_selectedProjectIndices;
    std::set<int> m_selectedRenderIndices;

    // Last click tracking for double-click detection
    double m_lastClickTime = 0.0;
    int m_lastClickedShotIndex = -1;
    int m_lastClickedProjectIndex = -1;
    int m_lastClickedRenderIndex = -1;
    double m_lastProjectClickTime = 0.0;
    double m_lastRenderClickTime = 0.0;

    // Sorting state
    ImGuiTableColumnSortSpecs m_shotSortSpecs = {};
    ImGuiTableColumnSortSpecs m_projectSortSpecs = {};
    ImGuiTableColumnSortSpecs m_renderSortSpecs = {};

    // Metadata management
    std::map<std::wstring, UFB::ShotMetadata> m_shotMetadataMap;  // Shot metadata cache (by shot path)
    std::map<std::string, bool> m_visibleColumns;                 // Column visibility for current category
    bool m_showColumnsPopup = false;                              // Show columns filter popup

    // Panel window positions and sizes (for focus highlighting)
    ImVec2 m_shotsPanelPos = {};
    ImVec2 m_shotsPanelSize = {};
    ImVec2 m_projectsPanelPos = {};
    ImVec2 m_projectsPanelSize = {};
    ImVec2 m_rendersPanelPos = {};
    ImVec2 m_rendersPanelSize = {};

    // Date picker state
    bool m_showDatePicker = false;
    int m_datePickerShotIndex = -1;

    // Add new shot modal state
    bool m_showAddShotDialog = false;
    char m_newShotNameBuffer[256] = {};

    // Helper method to create a new shot from template
    bool CreateNewShot(const std::string& shotName);
};
