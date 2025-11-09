#include "assets_view.h"
#include "file_browser.h"  // For FileEntry struct
#include "bookmark_manager.h"
#include "subscription_manager.h"
#include "metadata_manager.h"
#include "project_config.h"
#include "utils.h"
#include "ole_drag_drop.h"
#include "ImGuiDatePicker.hpp"
#include <iostream>
#include <algorithm>
#include <shlobj.h>
#include <shellapi.h>
#include <GLFW/glfw3.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <chrono>

// C++20 changed u8"" literals to char8_t, need to cast for ImGui
#define U8(x) reinterpret_cast<const char*>(u8##x)

// Helper functions to convert between tm and uint64_t Unix timestamps
// Note: App stores timestamps in milliseconds, but mktime uses seconds
static tm TimestampToTm(uint64_t timestampMillis)
{
    time_t timeSeconds = static_cast<time_t>(timestampMillis / 1000);  // Convert milliseconds to seconds
    tm result = {};
    #ifdef _WIN32
        localtime_s(&result, &timeSeconds);
    #else
        localtime_r(&timeSeconds, &result);
    #endif
    return result;
}

static uint64_t TmToTimestamp(const tm& time)
{
    tm copy = time;  // mktime modifies the struct
    time_t timeSeconds = mktime(&copy);
    return static_cast<uint64_t>(timeSeconds) * 1000;  // Convert seconds to milliseconds
}

// Static members
bool AssetsView::showHiddenFiles = false;
std::vector<std::wstring> AssetsView::m_cutFiles;
int AssetsView::m_oleRefCount = 0;

// External font pointers (from main.cpp)
extern ImFont* font_regular;
extern ImFont* font_mono;
extern ImFont* font_icons;

// External accent color function (from main.cpp)
extern ImVec4 GetWindowsAccentColor();

AssetsView::AssetsView()
{
    // Initialize OLE for drag-drop support
    if (m_oleRefCount == 0)
    {
        HRESULT hr = OleInitialize(nullptr);
        if (FAILED(hr))
        {
            std::cerr << "[AssetsView] Failed to initialize OLE" << std::endl;
        }
    }
    m_oleRefCount++;
}

AssetsView::~AssetsView()
{
    Shutdown();
}

void AssetsView::Initialize(const std::wstring& assetsFolderPath, const std::wstring& jobName,
                          UFB::BookmarkManager* bookmarkManager,
                          UFB::SubscriptionManager* subscriptionManager,
                          UFB::MetadataManager* metadataManager)
{
    m_assetsFolderPath = assetsFolderPath;
    m_jobName = jobName;
    m_bookmarkManager = bookmarkManager;
    m_subscriptionManager = subscriptionManager;
    m_metadataManager = metadataManager;

    // Register observer for real-time metadata updates
    if (m_metadataManager)
    {
        std::wstring jobPath = std::filesystem::path(assetsFolderPath).parent_path().wstring();
        m_metadataManager->RegisterObserver([this, jobPath](const std::wstring& changedJobPath) {
            // Only reload if the change is for our job
            if (changedJobPath == jobPath)
            {
                std::wcout << L"[AssetsView] Metadata changed for job, reloading..." << std::endl;
                ReloadMetadata();
            }
        });
    }

    // Initialize managers
    m_iconManager.Initialize();
    m_thumbnailManager.Initialize();

    // Initialize FileBrowser for right panel
    m_fileBrowser.Initialize(m_bookmarkManager, m_subscriptionManager);
    m_fileBrowser.SetCurrentDirectory(assetsFolderPath);

    // Set up file browser callbacks to forward to our own callbacks
    m_fileBrowser.onTranscodeToMP4 = [this](const std::vector<std::wstring>& paths) {
        std::wcout << L"[AssetsView] FileBrowser transcode callback triggered with " << paths.size() << L" files" << std::endl;
        if (onTranscodeToMP4) {
            std::wcout << L"[AssetsView] Forwarding to parent onTranscodeToMP4 callback" << std::endl;
            onTranscodeToMP4(paths);
        } else {
            std::wcout << L"[AssetsView] WARNING: Parent onTranscodeToMP4 callback is NULL!" << std::endl;
        }
    };

    m_fileBrowser.onOpenInBrowser1 = [this](const std::wstring& path) {
        if (onOpenInBrowser1) onOpenInBrowser1(path);
    };

    m_fileBrowser.onOpenInBrowser2 = [this](const std::wstring& path) {
        if (onOpenInBrowser2) onOpenInBrowser2(path);
    };

    // Load or create ProjectConfig for this job
    std::wstring jobPath = std::filesystem::path(assetsFolderPath).parent_path().wstring();
    m_projectConfig = new UFB::ProjectConfig();
    bool configLoaded = m_projectConfig->LoadProjectConfig(jobPath);

    if (!configLoaded)
    {
        std::wcerr << L"[AssetsView] ERROR: Failed to load ProjectConfig from: " << jobPath << std::endl;
        std::wcerr << L"[AssetsView] Will use hardcoded fallback defaults for columns" << std::endl;
    }
    else
    {
        std::wcout << L"[AssetsView] Successfully loaded ProjectConfig from: " << jobPath << std::endl;
        std::cout << "[AssetsView] Config version: " << m_projectConfig->GetVersion() << std::endl;
    }

    // Load column visibility for assets folder type
    LoadColumnVisibility();

    // Load initial asset items
    RefreshAssetItems();
}

void AssetsView::Shutdown()
{
    // Clean up ProjectConfig
    if (m_projectConfig)
    {
        delete m_projectConfig;
        m_projectConfig = nullptr;
    }

    m_iconManager.Shutdown();
    m_thumbnailManager.Shutdown();
    m_fileBrowser.Shutdown();

    // Uninitialize OLE
    m_oleRefCount--;
    if (m_oleRefCount == 0)
    {
        OleUninitialize();
    }
}

void AssetsView::SetSelectedAsset(const std::wstring& assetPath)
{
    // Find the asset in the assets list and select it
    for (size_t i = 0; i < m_assetItems.size(); i++)
    {
        if (m_assetItems[i].fullPath == assetPath)
        {
            m_selectedAssetIndex = static_cast<int>(i);

            // Update file browser to show this asset's contents
            m_fileBrowser.SetCurrentDirectory(assetPath);
            break;
        }
    }
}

void AssetsView::SetSelectedAssetAndFile(const std::wstring& assetPath, const std::wstring& filePath)
{
    // Find the asset in the assets list and select it
    for (size_t i = 0; i < m_assetItems.size(); i++)
    {
        if (m_assetItems[i].fullPath == assetPath)
        {
            m_selectedAssetIndex = static_cast<int>(i);

            // Update file browser to show this asset's contents and select the file
            std::wstring parentDir = std::filesystem::path(filePath).parent_path().wstring();
            m_fileBrowser.SetCurrentDirectoryAndSelectFile(parentDir, filePath);
            std::wcout << L"[AssetsView] Selected asset and file in browser: " << filePath << std::endl;
            break;
        }
    }
}

void AssetsView::RefreshAssetItems()
{
    m_assetItems.clear();
    m_assetMetadataMap.clear();

    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(m_assetsFolderPath))
        {
            // Skip hidden files/folders if needed
            if (!showHiddenFiles && (entry.path().filename().wstring()[0] == L'.'))
                continue;

            FileEntry fileEntry;
            fileEntry.name = entry.path().filename().wstring();
            fileEntry.fullPath = entry.path().wstring();
            fileEntry.isDirectory = entry.is_directory();
            fileEntry.size = entry.is_directory() ? 0 : entry.file_size();
            fileEntry.lastModified = entry.last_write_time();

            m_assetItems.push_back(fileEntry);
        }

        // Sort alphabetically
        std::sort(m_assetItems.begin(), m_assetItems.end(), [](const FileEntry& a, const FileEntry& b) {
            return a.name < b.name;
        });

        // Load metadata for all asset items
        LoadMetadata();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[AssetsView] Error refreshing asset items: " << e.what() << std::endl;
    }
}

void AssetsView::Draw(const char* title, HWND hwnd)
{
    // Set initial window size (only on first use, then it's dockable/resizable)
    ImGui::SetNextWindowSize(ImVec2(1400, 800), ImGuiCond_FirstUseEver);

    // Use built-in close button (X on tab), disable scrollbars for main window
    bool windowOpen = ImGui::Begin(title, &m_isOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (windowOpen)
    {
        // Show assets folder path
        std::string assetsFolderPathUtf8 = UFB::WideToUtf8(m_assetsFolderPath);

        // Use mono font for path
        if (font_mono)
            ImGui::PushFont(font_mono);

        ImGui::TextDisabled("%s", assetsFolderPathUtf8.c_str());

        if (font_mono)
            ImGui::PopFont();

        ImGui::Separator();

        // Two-panel layout (50/50 split by default)
        ImVec2 availSize = ImGui::GetContentRegionAvail();
        ImVec2 windowPos = ImGui::GetCursorScreenPos();
        float panelSpacing = 8.0f;  // Spacing for divider line
        float leftWidth = availSize.x * 0.50f - panelSpacing / 2.0f;  // 50% for assets list
        float rightWidth = availSize.x * 0.50f - panelSpacing / 2.0f; // 50% for file browser

        // Left panel - Assets Table
        ImGui::BeginChild("AssetsPanel", ImVec2(leftWidth, availSize.y), false);  // No built-in border
        DrawAssetsPanel(hwnd);
        ImGui::EndChild();

        ImGui::SameLine(0.0f, panelSpacing);

        // Draw vertical separator line between Assets and Browser
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float lineX = windowPos.x + leftWidth + panelSpacing / 2.0f;
        ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        drawList->AddLine(ImVec2(lineX, windowPos.y), ImVec2(lineX, windowPos.y + availSize.y), lineColor, 1.0f);

        // Right panel - File Browser
        ImGui::BeginChild("BrowserPanel", ImVec2(rightWidth, availSize.y), false);  // No built-in border
        DrawBrowserPanel(hwnd);
        ImGui::EndChild();

        // Handle keyboard shortcuts (Ctrl+C, Ctrl+X, Ctrl+V, Delete, F2)
        // Note: FileBrowser (right panel) handles its own shortcuts, this is for the assets panel (left)
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
        {
            ImGuiIO& io = ImGui::GetIO();

            // Only handle shortcuts for assets panel if it's the focused child
            // The FileBrowser panel will handle shortcuts for files selected there

            // For now, F2 for rename when an asset is selected
            if (ImGui::IsKeyPressed(ImGuiKey_F2))
            {
                // Only allow rename if exactly one asset is selected
                if (m_selectedAssetIndices.size() == 1)
                {
                    int idx = *m_selectedAssetIndices.begin();
                    if (idx >= 0 && idx < m_assetItems.size())
                    {
                        m_renameOriginalPath = m_assetItems[idx].fullPath;
                        std::wstring filename = m_assetItems[idx].name;
                        WideCharToMultiByte(CP_UTF8, 0, filename.c_str(), -1, m_renameBuffer, sizeof(m_renameBuffer), nullptr, nullptr);
                        m_showRenameDialog = true;
                    }
                }
            }
        }

        // Add new asset modal dialog
        if (m_showAddAssetDialog)
        {
            ImGui::OpenPopup("Add New Asset");
            m_showAddAssetDialog = false;
        }

        if (ImGui::BeginPopupModal("Add New Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Enter asset name:");
            ImGui::Separator();

            ImGui::SetNextItemWidth(300);
            bool enterPressed = ImGui::InputText("##assetname", m_newAssetNameBuffer, sizeof(m_newAssetNameBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::Separator();

            if (ImGui::Button("Create", ImVec2(120, 0)) || enterPressed)
            {
                std::string assetName(m_newAssetNameBuffer);
                if (!assetName.empty())
                {
                    if (CreateNewAsset(assetName))
                    {
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        std::cerr << "[AssetsView] Failed to create asset: " << assetName << std::endl;
                    }
                }
            }

            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();

            if (ImGui::Button("Cancel", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        // Rename dialog modal
        if (m_showRenameDialog)
        {
            ImGui::OpenPopup("Rename");
            m_showRenameDialog = false;  // Only open once
        }

        if (ImGui::BeginPopupModal("Rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Enter new name:");
            ImGui::SetNextItemWidth(300.0f);

            // Auto-focus the input field when opened
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }

            bool enterPressed = ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::Spacing();

            bool doRename = false;
            if (ImGui::Button("OK", ImVec2(120, 0)) || enterPressed)
            {
                doRename = true;
            }

            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }

            // Handle rename
            if (doRename)
            {
                // Convert new name from UTF-8 to wide string
                wchar_t newNameW[256];
                MultiByteToWideChar(CP_UTF8, 0, m_renameBuffer, -1, newNameW, 256);

                // Build new path
                std::filesystem::path originalPath(m_renameOriginalPath);
                std::filesystem::path newPath = originalPath.parent_path() / newNameW;

                // Attempt rename
                try
                {
                    std::filesystem::rename(originalPath, newPath);

                    // Refresh asset list
                    RefreshAssetItems();
                }
                catch (const std::exception& e)
                {
                    std::cerr << "[AssetsView] Rename failed: " << e.what() << std::endl;
                }

                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    ImGui::End();
}

void AssetsView::DrawAssetsPanel(HWND hwnd)
{
    // Store window position and size
    m_assetsPanelPos = ImGui::GetWindowPos();
    m_assetsPanelSize = ImGui::GetWindowSize();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // Draw focus highlight border if this panel is focused
    if (isFocused)
    {
        ImVec4 accentColor = GetAccentColor();
        ImU32 highlightColor = ImGui::ColorConvertFloat4ToU32(ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.3f));

        float borderPadding = 4.0f;
        ImVec2 min = ImVec2(m_assetsPanelPos.x + borderPadding, m_assetsPanelPos.y + borderPadding);
        ImVec2 max = ImVec2(m_assetsPanelPos.x + m_assetsPanelSize.x - borderPadding, m_assetsPanelPos.y + m_assetsPanelSize.y - borderPadding);
        drawList->AddRect(min, max, highlightColor, 0.0f, 0, 3.0f);
    }

    // Create nested child window with padding to make room for the highlight border
    float contentPadding = 6.0f;
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    contentSize.x -= contentPadding * 2;
    contentSize.y -= contentPadding * 2;

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + contentPadding, ImGui::GetCursorPosY() + contentPadding));

    ImGui::BeginChild("##assets_content", contentSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::Text("Assets");

    // Add new asset button
    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE145##addAsset")))  // Material Icons add
        {
            m_showAddAssetDialog = true;
            memset(m_newAssetNameBuffer, 0, sizeof(m_newAssetNameBuffer));
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("+##addAsset"))
        {
            m_showAddAssetDialog = true;
            memset(m_newAssetNameBuffer, 0, sizeof(m_newAssetNameBuffer));
        }
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add New Asset");

    ImGui::SameLine();

    // ===== COMPACT FILTER BUTTONS =====

    // Category Filter Button
    int categoryCount = static_cast<int>(m_filterCategories.size());
    std::string categoryLabel = "Category" + (categoryCount > 0 ? " (" + std::to_string(categoryCount) + ")" : "");
    if (ImGui::Button(categoryLabel.c_str()))
    {
        ImGui::OpenPopup("CategoryFilterPopup");
    }

    // Category Filter Popup
    if (ImGui::BeginPopup("CategoryFilterPopup"))
    {
        ImGui::Text("Filter by Category:");
        ImGui::Separator();
        for (const auto& category : m_availableCategories)
        {
            bool isSelected = (m_filterCategories.find(category) != m_filterCategories.end());
            if (ImGui::Checkbox(category.c_str(), &isSelected))
            {
                if (isSelected)
                    m_filterCategories.insert(category);
                else
                    m_filterCategories.erase(category);
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Clear All"))
        {
            m_filterCategories.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();

    // Date Modified Filter Button
    const char* dateOptions[] = { "All", "Today", "Yesterday", "Last 7 days", "Last 30 days", "This year" };
    std::string dateLabel = "Date Modified";
    if (m_filterDateModified > 0)
    {
        dateLabel = std::string(dateOptions[m_filterDateModified]);
    }
    if (ImGui::Button(dateLabel.c_str()))
    {
        ImGui::OpenPopup("DateModifiedFilterPopup");
    }

    // Date Modified Filter Popup
    if (ImGui::BeginPopup("DateModifiedFilterPopup"))
    {
        ImGui::Text("Filter by Date Modified:");
        ImGui::Separator();
        for (int i = 0; i < 6; i++)
        {
            bool isSelected = (m_filterDateModified == i);
            if (ImGui::Selectable(dateOptions[i], isSelected))
            {
                m_filterDateModified = i;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();

    // Clear Filters Button
    int totalActiveFilters = categoryCount + (m_filterDateModified > 0 ? 1 : 0);
    if (totalActiveFilters > 0)
    {
        if (font_icons) ImGui::PushFont(font_icons);
        if (ImGui::SmallButton(U8("\uE14C##clearFilters")))  // Material Icons close
        {
            m_filterCategories.clear();
            m_filterDateModified = 0;
        }
        if (font_icons) ImGui::PopFont();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clear All Filters");
        ImGui::SameLine();
    }

    // Columns filter button
    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE152##assetsColumns")))  // Material Icons filter_list
        {
            m_showColumnsPopup = true;
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("Cols##assetsColumns"))
        {
            m_showColumnsPopup = true;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Configure Columns");

    ImGui::SameLine();

    // Refresh button
    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE5D5##assets")))  // Material Icons refresh
        {
            RefreshAssetItems();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("R##assets"))
        {
            RefreshAssetItems();
        }
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Refresh");

    // Columns popup menu
    if (m_showColumnsPopup)
    {
        ImGui::OpenPopup("ColumnsPopup");
        m_showColumnsPopup = false;
    }

    if (ImGui::BeginPopup("ColumnsPopup"))
    {
        ImGui::Text("Visible Columns");
        ImGui::Separator();

        // Checkbox for each column (use local bool to avoid taking reference to map element)
        bool statusVisible = m_visibleColumns["Status"];
        if (ImGui::Checkbox("Status", &statusVisible))
        {
            m_visibleColumns["Status"] = statusVisible;
            SaveColumnVisibility();
        }

        bool categoryVisible = m_visibleColumns["Category"];
        if (ImGui::Checkbox("Category", &categoryVisible))
        {
            m_visibleColumns["Category"] = categoryVisible;
            SaveColumnVisibility();
        }

        bool artistVisible = m_visibleColumns["Artist"];
        if (ImGui::Checkbox("Artist", &artistVisible))
        {
            m_visibleColumns["Artist"] = artistVisible;
            SaveColumnVisibility();
        }

        bool priorityVisible = m_visibleColumns["Priority"];
        if (ImGui::Checkbox("Priority", &priorityVisible))
        {
            m_visibleColumns["Priority"] = priorityVisible;
            SaveColumnVisibility();
        }

        bool dueDateVisible = m_visibleColumns["DueDate"];
        if (ImGui::Checkbox("Due Date", &dueDateVisible))
        {
            m_visibleColumns["DueDate"] = dueDateVisible;
            SaveColumnVisibility();
        }

        bool notesVisible = m_visibleColumns["Notes"];
        if (ImGui::Checkbox("Notes", &notesVisible))
        {
            m_visibleColumns["Notes"] = notesVisible;
            SaveColumnVisibility();
        }

        bool linksVisible = m_visibleColumns["Links"];
        if (ImGui::Checkbox("Links", &linksVisible))
        {
            m_visibleColumns["Links"] = linksVisible;
            SaveColumnVisibility();
        }

        ImGui::EndPopup();
    }

    ImGui::Separator();

    // Calculate column count dynamically
    int columnCount = 2;  // Name + Modified (always visible)
    for (const auto& [col, visible] : m_visibleColumns)
    {
        if (visible) columnCount++;
    }

    // Push larger cell padding for taller rows
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 8.0f));

    if (ImGui::BeginTable("AssetsTable", columnCount, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable))
    {
        // Name column (always first)
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);

        // Metadata columns (conditional) - widths adjusted for typical asset content
        if (m_visibleColumns["Status"])
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 130.0f);

        if (m_visibleColumns["Category"])
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 140.0f);

        if (m_visibleColumns["Artist"])
            ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthFixed, 150.0f);

        if (m_visibleColumns["Priority"])
            ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 110.0f);

        if (m_visibleColumns["DueDate"])
            ImGui::TableSetupColumn("Due Date", ImGuiTableColumnFlags_WidthFixed, 110.0f);

        if (m_visibleColumns["Notes"])
            ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthFixed, 300.0f);

        if (m_visibleColumns["Links"])
            ImGui::TableSetupColumn("Links", ImGuiTableColumnFlags_WidthFixed, 60.0f);

        // Modified column (always last)
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 150.0f);

        ImGui::TableHeadersRow();

        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsDirty)
            {
                if (sortSpecs->SpecsCount > 0)
                {
                    m_assetSortSpecs = sortSpecs->Specs[0];

                    // Build column index map (which column index corresponds to which field)
                    std::map<int, std::string> columnIndexMap;
                    int currentIndex = 0;
                    columnIndexMap[currentIndex++] = "Name";  // Name is always first

                    if (m_visibleColumns["Status"]) columnIndexMap[currentIndex++] = "Status";
                    if (m_visibleColumns["Category"]) columnIndexMap[currentIndex++] = "Category";
                    if (m_visibleColumns["Artist"]) columnIndexMap[currentIndex++] = "Artist";
                    if (m_visibleColumns["Priority"]) columnIndexMap[currentIndex++] = "Priority";
                    if (m_visibleColumns["DueDate"]) columnIndexMap[currentIndex++] = "DueDate";
                    if (m_visibleColumns["Notes"]) columnIndexMap[currentIndex++] = "Notes";
                    if (m_visibleColumns["Links"]) columnIndexMap[currentIndex++] = "Links";

                    columnIndexMap[currentIndex] = "Modified";  // Modified is always last

                    // Get the field name for the clicked column
                    std::string sortField = columnIndexMap[m_assetSortSpecs.ColumnIndex];

                    // Sort by column
                    std::sort(m_assetItems.begin(), m_assetItems.end(), [this, sortField](const FileEntry& a, const FileEntry& b) {
                        bool ascending = (m_assetSortSpecs.SortDirection == ImGuiSortDirection_Ascending);

                        if (sortField == "Name")
                        {
                            int cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                            return ascending ? (cmp < 0) : (cmp > 0);
                        }
                        else if (sortField == "Modified")
                        {
                            return ascending ? (a.lastModified < b.lastModified) : (a.lastModified > b.lastModified);
                        }
                        else if (sortField == "Status" || sortField == "Category" || sortField == "Artist" ||
                                 sortField == "Priority" || sortField == "DueDate" || sortField == "Notes" || sortField == "Links")
                        {
                            // Get metadata for both entries
                            UFB::ShotMetadata* metadataA = nullptr;
                            UFB::ShotMetadata* metadataB = nullptr;

                            auto itA = m_assetMetadataMap.find(a.fullPath);
                            if (itA != m_assetMetadataMap.end()) metadataA = &itA->second;

                            auto itB = m_assetMetadataMap.find(b.fullPath);
                            if (itB != m_assetMetadataMap.end()) metadataB = &itB->second;

                            // Sort by metadata field
                            if (sortField == "Status")
                            {
                                std::string statusA = metadataA ? metadataA->status : "";
                                std::string statusB = metadataB ? metadataB->status : "";
                                int cmp = statusA.compare(statusB);
                                return ascending ? (cmp < 0) : (cmp > 0);
                            }
                            else if (sortField == "Category")
                            {
                                std::string catA = metadataA ? metadataA->category : "";
                                std::string catB = metadataB ? metadataB->category : "";
                                int cmp = catA.compare(catB);
                                return ascending ? (cmp < 0) : (cmp > 0);
                            }
                            else if (sortField == "Artist")
                            {
                                std::string artistA = metadataA ? metadataA->artist : "";
                                std::string artistB = metadataB ? metadataB->artist : "";
                                int cmp = artistA.compare(artistB);
                                return ascending ? (cmp < 0) : (cmp > 0);
                            }
                            else if (sortField == "Priority")
                            {
                                int prioA = metadataA ? metadataA->priority : 2;
                                int prioB = metadataB ? metadataB->priority : 2;
                                return ascending ? (prioA < prioB) : (prioA > prioB);
                            }
                            else if (sortField == "DueDate")
                            {
                                uint64_t dateA = metadataA ? metadataA->dueDate : 0;
                                uint64_t dateB = metadataB ? metadataB->dueDate : 0;
                                return ascending ? (dateA < dateB) : (dateA > dateB);
                            }
                            else if (sortField == "Notes")
                            {
                                std::string noteA = metadataA ? metadataA->note : "";
                                std::string noteB = metadataB ? metadataB->note : "";
                                int cmp = noteA.compare(noteB);
                                return ascending ? (cmp < 0) : (cmp > 0);
                            }
                            else if (sortField == "Links")
                            {
                                int linksA = metadataA ? (int)metadataA->links.size() : 0;
                                int linksB = metadataB ? (int)metadataB->links.size() : 0;
                                return ascending ? (linksA < linksB) : (linksA > linksB);
                            }
                        }

                        // Default: sort by name
                        int cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                        return ascending ? (cmp < 0) : (cmp > 0);
                    });
                }
                sortSpecs->SpecsDirty = false;
            }
        }

        for (int i = 0; i < m_assetItems.size(); i++)
        {
            const FileEntry& entry = m_assetItems[i];

            // Apply filters - skip if doesn't pass
            if (!PassesFilters(entry))
                continue;

            // Set minimum row height
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 35.0f);
            ImGui::TableNextColumn();

            ImGui::PushID(i);

            // Get icon
            ImTextureID icon = m_iconManager.GetFileIcon(entry.fullPath, entry.isDirectory);

            bool isSelected = (i == m_selectedAssetIndex);

            // Get or create metadata for this asset (needed for star icon)
            UFB::ShotMetadata* metadata = nullptr;
            auto metadataIt = m_assetMetadataMap.find(entry.fullPath);
            if (metadataIt != m_assetMetadataMap.end())
                metadata = &metadataIt->second;

            // Use accent color for selected items
            ImVec4 accentColor = GetAccentColor();
            if (isSelected)
            {
                ImGui::PushStyleColor(ImGuiCol_Header, accentColor);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accentColor.x * 1.1f, accentColor.y * 1.1f, accentColor.z * 1.1f, accentColor.w));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(accentColor.x * 1.2f, accentColor.y * 1.2f, accentColor.z * 1.2f, accentColor.w));
            }

            // Draw icon or star if tracked
            if (metadata && metadata->isTracked)
            {
                // Draw star icon for tracked items (use bright variant)
                ImVec4 brightAccent = ImVec4(accentColor.x * 1.3f, accentColor.y * 1.3f, accentColor.z * 1.3f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, brightAccent);
                ImGui::Text("\xE2\x98\x85");  // Unicode star character (U+2605)
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            else if (icon)
            {
                // Draw folder icon for untracked items
                ImGui::Image(icon, ImVec2(16, 16));
                ImGui::SameLine();
            }

            // Convert name to UTF-8
            char nameUtf8[512];
            WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);

            // Use explicit height to match row height (35.0f)
            if (ImGui::Selectable(nameUtf8, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 35.0f)))
            {
                // Select this asset item
                m_selectedAssetIndex = i;

                // Navigate right panel to this location
                if (entry.isDirectory)
                {
                    m_fileBrowser.SetCurrentDirectory(entry.fullPath);
                }
                else
                {
                    // If it's a file, navigate to its parent directory
                    std::filesystem::path filePath(entry.fullPath);
                    m_fileBrowser.SetCurrentDirectory(filePath.parent_path().wstring());
                }

                // Double-click detection
                double currentTime = glfwGetTime();
                if (m_lastClickedAssetIndex == i && (currentTime - m_lastClickTime) < 0.3)
                {
                    // Double-clicked - open in file explorer or default app
                    ShellExecuteW(nullptr, L"open", entry.fullPath.c_str(), nullptr, nullptr, SW_SHOW);
                }
                m_lastClickTime = currentTime;
                m_lastClickedAssetIndex = i;
            }

            if (isSelected)
            {
                ImGui::PopStyleColor(3);
            }

            // Context menu
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                ImGui::OpenPopup("asset_context_menu");
            }

            ShowImGuiContextMenu(hwnd, entry);

            // Push mono font for all metadata columns (not the name)
            if (font_mono)
                ImGui::PushFont(font_mono);

            // If no metadata exists, create a default one in memory
            UFB::ShotMetadata tempMetadata;
            if (!metadata)
            {
                tempMetadata.shotPath = entry.fullPath;
                tempMetadata.folderType = "assets";  // Fixed folder type for assets
                tempMetadata.isTracked = false;

                // Load default metadata values from ProjectConfig
                if (m_projectConfig && m_projectConfig->IsLoaded())
                {
                    auto defaultMeta = m_projectConfig->GetDefaultMetadata(tempMetadata.folderType);
                    if (defaultMeta.has_value())
                    {
                        tempMetadata.status = defaultMeta->status;
                        tempMetadata.category = defaultMeta->category;
                        tempMetadata.priority = defaultMeta->priority;
                        tempMetadata.artist = defaultMeta->artist;
                        tempMetadata.note = defaultMeta->note;
                        // dueDate and links stay at defaults (0 and empty)
                    }
                    else
                    {
                        // Fallback if no default metadata found
                        tempMetadata.priority = 2;
                    }
                }
                else
                {
                    // Fallback if ProjectConfig not loaded
                    tempMetadata.priority = 2;
                }

                metadata = &tempMetadata;
            }

            // Store if we need to save changes
            bool metadataChanged = false;

            // Safety check: metadata should always be valid at this point
            if (!metadata)
            {
                std::cerr << "[AssetsView] ERROR: metadata pointer is null for asset: " << UFB::WideToUtf8(entry.fullPath) << std::endl;

                // Fill remaining columns with error text
                for (const auto& [col, visible] : m_visibleColumns)
                {
                    if (visible)
                    {
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("(error)");
                    }
                }

                // Modified column
                ImGui::TableNextColumn();
                ImGui::TextDisabled("(error)");

                if (font_mono)
                    ImGui::PopFont();

                ImGui::PopID();
                continue;
            }

            // Status column
            if (m_visibleColumns["Status"])
            {
                ImGui::TableNextColumn();

                // Get available status options
                std::vector<UFB::StatusOption> statusOptions;
                if (m_projectConfig && m_projectConfig->IsLoaded())
                {
                    statusOptions = m_projectConfig->GetStatusOptions("assets");
                }

                // Color the text
                ImVec4 statusColor = GetStatusColor(metadata->status);
                ImGui::PushStyleColor(ImGuiCol_Text, statusColor);

                // Create combo box ID
                std::string comboId = "##status_" + std::to_string(i);

                // Display current status or "(No options configured)"
                const char* currentStatus = statusOptions.empty() ? "(No options configured)" :
                                           (metadata->status.empty() ? statusOptions[0].name.c_str() : metadata->status.c_str());

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(comboId.c_str(), currentStatus, ImGuiComboFlags_HeightLarge))
                {
                    for (const auto& statusOption : statusOptions)
                    {
                        // Apply color to this option
                        ImVec4 optionColor = GetStatusColor(statusOption.name);
                        ImGui::PushStyleColor(ImGuiCol_Text, optionColor);

                        bool isSelected = (metadata->status == statusOption.name);
                        if (ImGui::Selectable(statusOption.name.c_str(), isSelected))
                        {
                            metadata->status = statusOption.name;
                            metadataChanged = true;
                        }
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();

                        ImGui::PopStyleColor();
                    }
                    ImGui::EndCombo();
                }

                ImGui::PopStyleColor();
            }

            // Category column
            if (m_visibleColumns["Category"])
            {
                ImGui::TableNextColumn();

                // Get available category options
                std::vector<UFB::CategoryOption> categoryOptions;
                if (m_projectConfig && m_projectConfig->IsLoaded())
                {
                    categoryOptions = m_projectConfig->GetCategoryOptions("assets");
                }

                // Color the text
                ImVec4 categoryColor = GetCategoryColor(metadata->category);
                ImGui::PushStyleColor(ImGuiCol_Text, categoryColor);

                // Create combo box ID
                std::string comboId = "##category_" + std::to_string(i);

                // Display current category or "(No options configured)"
                const char* currentCategory = categoryOptions.empty() ? "(No options configured)" :
                                             (metadata->category.empty() ? categoryOptions[0].name.c_str() : metadata->category.c_str());

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(comboId.c_str(), currentCategory, ImGuiComboFlags_HeightLarge))
                {
                    for (const auto& categoryOption : categoryOptions)
                    {
                        // Apply color to this option
                        ImVec4 optionColor = GetCategoryColor(categoryOption.name);
                        ImGui::PushStyleColor(ImGuiCol_Text, optionColor);

                        bool isSelected = (metadata->category == categoryOption.name);
                        if (ImGui::Selectable(categoryOption.name.c_str(), isSelected))
                        {
                            metadata->category = categoryOption.name;
                            metadataChanged = true;
                        }
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();

                        ImGui::PopStyleColor();
                    }
                    ImGui::EndCombo();
                }

                ImGui::PopStyleColor();
            }

            // Artist column
            if (m_visibleColumns["Artist"])
            {
                ImGui::TableNextColumn();

                // Get available users
                std::vector<UFB::User> users;
                if (m_projectConfig && m_projectConfig->IsLoaded())
                {
                    users = m_projectConfig->GetUsers();
                }

                // Create combo box ID
                std::string comboId = "##artist_" + std::to_string(i);

                // Display current artist or "(No options configured)"
                const char* currentArtist = users.empty() ? "(No options configured)" :
                                           (metadata->artist.empty() ? users[0].displayName.c_str() : metadata->artist.c_str());

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(comboId.c_str(), currentArtist, ImGuiComboFlags_HeightLarge))
                {
                    for (const auto& user : users)
                    {
                        bool isSelected = (metadata->artist == user.displayName);
                        if (ImGui::Selectable(user.displayName.c_str(), isSelected))
                        {
                            metadata->artist = user.displayName;
                            metadataChanged = true;
                        }
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            // Priority column
            if (m_visibleColumns["Priority"])
            {
                ImGui::TableNextColumn();

                const char* priorityLabels[] = { "High", "Medium", "Low" };
                const char* currentPriority = priorityLabels[metadata->priority];

                std::string comboId = "##priority_" + std::to_string(i);

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(comboId.c_str(), currentPriority))
                {
                    for (int p = 0; p < 3; p++)
                    {
                        bool isSelected = (metadata->priority == p);
                        if (ImGui::Selectable(priorityLabels[p], isSelected))
                        {
                            metadata->priority = p;
                            metadataChanged = true;
                        }
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            // Due Date column
            if (m_visibleColumns["DueDate"])
            {
                ImGui::TableNextColumn();

                std::string dateStr = FormatTimestamp(metadata->dueDate);
                std::string buttonLabel = dateStr.empty() ? "Set Date##" + std::to_string(i) : dateStr + "##" + std::to_string(i);

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Button(buttonLabel.c_str()))
                {
                    m_showDatePicker = true;
                    m_datePickerAssetIndex = i;
                }

                // Date picker popup
                if (m_showDatePicker && m_datePickerAssetIndex == i)
                {
                    std::string popupId = "DatePicker##" + std::to_string(i);
                    ImGui::OpenPopup(popupId.c_str());

                    if (ImGui::BeginPopup(popupId.c_str()))
                    {
                        tm currentDate = TimestampToTm(metadata->dueDate > 0 ? metadata->dueDate : static_cast<uint64_t>(std::time(nullptr)) * 1000);

                        if (ImGui::DatePicker("##datepicker", currentDate, false))
                        {
                            metadata->dueDate = TmToTimestamp(currentDate);
                            metadataChanged = true;
                        }

                        if (ImGui::Button("Clear"))
                        {
                            metadata->dueDate = 0;
                            metadataChanged = true;
                            m_showDatePicker = false;
                            ImGui::CloseCurrentPopup();
                        }

                        ImGui::SameLine();

                        if (ImGui::Button("Close"))
                        {
                            m_showDatePicker = false;
                            ImGui::CloseCurrentPopup();
                        }

                        ImGui::EndPopup();
                    }
                    else
                    {
                        m_showDatePicker = false;
                    }
                }
            }

            // Notes column
            if (m_visibleColumns["Notes"])
            {
                ImGui::TableNextColumn();

                char noteBuffer[256];
                strncpy_s(noteBuffer, metadata->note.c_str(), sizeof(noteBuffer) - 1);
                noteBuffer[sizeof(noteBuffer) - 1] = '\0';

                std::string inputId = "##note_" + std::to_string(i);
                ImGui::SetNextItemWidth(-FLT_MIN);

                if (ImGui::InputText(inputId.c_str(), noteBuffer, sizeof(noteBuffer)))
                {
                    metadata->note = noteBuffer;
                    metadataChanged = true;
                }
            }

            // Links column
            if (m_visibleColumns["Links"])
            {
                ImGui::TableNextColumn();

                int linkCount = static_cast<int>(metadata->links.size());
                std::string linkText = std::to_string(linkCount);
                ImGui::TextDisabled("%s", linkText.c_str());

                // TODO: Add link management UI in future
            }

            // Pop mono font
            if (font_mono)
                ImGui::PopFont();

            // Modified column
            ImGui::TableNextColumn();
            if (font_mono)
                ImGui::PushFont(font_mono);
            ImGui::TextDisabled("%s", FormatFileTime(entry.lastModified).c_str());
            if (font_mono)
                ImGui::PopFont();

            // Save metadata changes
            if (metadataChanged && m_subscriptionManager)
            {
                // Update the map
                m_assetMetadataMap[entry.fullPath] = *metadata;

                // Save to database
                m_subscriptionManager->CreateOrUpdateShotMetadata(*metadata);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleVar();  // CellPadding

    ImGui::EndChild();  // End nested child window for assets panel
}

void AssetsView::DrawBrowserPanel(HWND hwnd)
{
    // Draw the full FileBrowser directly without extra child window
    // FileBrowser will handle its own highlighting
    m_fileBrowser.Draw("Browser", hwnd, false);
}

void AssetsView::ShowImGuiContextMenu(HWND hwnd, const FileEntry& entry)
{
    if (ImGui::BeginPopup("asset_context_menu"))
    {
        // Convert filename to UTF-8
        char nameUtf8[512];
        WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);

        // Header with filename
        ImGui::TextDisabled("%s", nameUtf8);
        ImGui::Separator();

        // Copy (file operation) - supports multi-select
        if (ImGui::MenuItem("Copy"))
        {
            std::vector<std::wstring> paths;

            // If multiple assets are selected in the assets panel, copy all of them
            if (!m_selectedAssetIndices.empty())
            {
                for (int idx : m_selectedAssetIndices)
                {
                    if (idx >= 0 && idx < m_assetItems.size())
                    {
                        paths.push_back(m_assetItems[idx].fullPath);
                    }
                }
            }
            else
            {
                // Fallback: copy just this entry
                paths.push_back(entry.fullPath);
            }

            CopyFilesToClipboard(paths);
        }

        // Cut (file operation) - supports multi-select
        if (ImGui::MenuItem("Cut"))
        {
            std::vector<std::wstring> paths;

            if (!m_selectedAssetIndices.empty())
            {
                for (int idx : m_selectedAssetIndices)
                {
                    if (idx >= 0 && idx < m_assetItems.size())
                    {
                        paths.push_back(m_assetItems[idx].fullPath);
                    }
                }
            }
            else
            {
                // Fallback: cut just this entry
                paths.push_back(entry.fullPath);
            }

            CutFilesToClipboard(paths);
        }

        // Paste (check if clipboard has files)
        bool hasFilesInClipboard = false;
        if (OpenClipboard(nullptr))
        {
            hasFilesInClipboard = (GetClipboardData(CF_HDROP) != nullptr);
            CloseClipboard();
        }

        if (ImGui::MenuItem("Paste", nullptr, false, hasFilesInClipboard))
        {
            PasteFilesFromClipboard();
        }

        ImGui::Separator();

        // Copy Path
        if (ImGui::MenuItem("Copy Full Path"))
        {
            CopyToClipboard(entry.fullPath);
        }

        // Copy Filename
        if (ImGui::MenuItem("Copy Filename"))
        {
            CopyToClipboard(entry.name);
        }

        ImGui::Separator();

        // Reveal in Explorer
        if (ImGui::MenuItem("Reveal in Explorer"))
        {
            RevealInExplorer(entry.fullPath);
        }

        // Open in New Window
        if (onOpenInNewWindow)
        {
            if (ImGui::MenuItem("Open in New Window"))
            {
                // For files, open the parent directory; for directories, open the directory itself
                std::filesystem::path targetPath(entry.fullPath);
                std::wstring pathToOpen = entry.isDirectory ? entry.fullPath : targetPath.parent_path().wstring();
                onOpenInNewWindow(pathToOpen);
                ImGui::CloseCurrentPopup();
            }
        }

        // Open in Browser 1 and Browser 2
        if (onOpenInBrowser1)
        {
            if (ImGui::MenuItem("Open in Browser 1"))
            {
                // For files, open the parent directory; for directories, open the directory itself
                std::filesystem::path targetPath(entry.fullPath);
                std::wstring pathToOpen = entry.isDirectory ? entry.fullPath : targetPath.parent_path().wstring();
                onOpenInBrowser1(pathToOpen);
                ImGui::CloseCurrentPopup();
            }
        }

        if (onOpenInBrowser2)
        {
            if (ImGui::MenuItem("Open in Browser 2"))
            {
                // For files, open the parent directory; for directories, open the directory itself
                std::filesystem::path targetPath(entry.fullPath);
                std::wstring pathToOpen = entry.isDirectory ? entry.fullPath : targetPath.parent_path().wstring();
                onOpenInBrowser2(pathToOpen);
                ImGui::CloseCurrentPopup();
            }
        }

        // Open (only for files)
        if (!entry.isDirectory)
        {
            if (ImGui::MenuItem("Open"))
            {
                ShellExecuteW(nullptr, L"open", entry.fullPath.c_str(), nullptr, nullptr, SW_SHOW);
            }
        }

        // Transcode to MP4 (only for video files)
        if (!entry.isDirectory)
        {
            // Check if this is a video file
            std::filesystem::path filePath(entry.fullPath);
            std::wstring ext = filePath.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

            static const std::set<std::wstring> videoExtensions = {
                L".mp4", L".mov", L".avi", L".mkv", L".wmv", L".flv", L".webm",
                L".m4v", L".mpg", L".mpeg", L".3gp", L".mxf", L".mts", L".m2ts"
            };

            bool isVideo = videoExtensions.find(ext) != videoExtensions.end();

            if (isVideo && onTranscodeToMP4)
            {
                // Use bright variant of accent color
                ImVec4 accentColor = GetAccentColor();
                ImVec4 brightAccent = ImVec4(
                    accentColor.x * 1.3f,
                    accentColor.y * 1.3f,
                    accentColor.z * 1.3f,
                    1.0f
                );
                ImGui::PushStyleColor(ImGuiCol_Text, brightAccent);

                if (ImGui::MenuItem("Transcode to MP4"))
                {
                    std::vector<std::wstring> selectedVideos = { entry.fullPath };
                    onTranscodeToMP4(selectedVideos);
                    ImGui::CloseCurrentPopup();
                }

                ImGui::PopStyleColor();
            }
        }

        ImGui::Separator();

        // Rename
        if (ImGui::MenuItem("Rename"))
        {
            m_showRenameDialog = true;
            m_renameOriginalPath = entry.fullPath;

            // Copy filename to rename buffer
            WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, m_renameBuffer, sizeof(m_renameBuffer), nullptr, nullptr);

            ImGui::CloseCurrentPopup();
        }

        ImGui::Separator();

        // Track/Untrack menu item
        if (m_subscriptionManager)
        {
            // Get current metadata
            auto metadata = m_subscriptionManager->GetShotMetadata(entry.fullPath);
            bool isTracked = metadata.has_value() && metadata->isTracked;

            // Use bright accent color for menu item
            ImVec4 accentColor = GetAccentColor();
            ImVec4 brightAccent = ImVec4(accentColor.x * 1.3f, accentColor.y * 1.3f, accentColor.z * 1.3f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, brightAccent);

            const char* menuLabel = isTracked ? "Untrack Asset" : "Track Asset";
            if (ImGui::MenuItem(menuLabel))
            {
                // Create or update metadata
                UFB::ShotMetadata assetMeta;
                if (metadata.has_value())
                {
                    assetMeta = *metadata;
                }
                else
                {
                    assetMeta.shotPath = entry.fullPath;
                    assetMeta.itemType = "asset";
                    assetMeta.folderType = "assets";
                    assetMeta.isTracked = false;
                }

                // Toggle tracking status
                assetMeta.isTracked = !isTracked;
                assetMeta.modifiedTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();

                // Save to database
                m_subscriptionManager->CreateOrUpdateShotMetadata(assetMeta);

                // Update local map
                m_assetMetadataMap[entry.fullPath] = assetMeta;
            }

            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        // More Options - opens Windows context menu
        if (ImGui::MenuItem("More Options..."))
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            ShowContextMenu(hwnd, entry.fullPath, mousePos);
            ImGui::CloseCurrentPopup();
        }

        ImGui::Separator();

        // Delete - supports multi-select
        if (ImGui::MenuItem("Delete"))
        {
            std::vector<std::wstring> paths;

            if (!m_selectedAssetIndices.empty())
            {
                for (int idx : m_selectedAssetIndices)
                {
                    if (idx >= 0 && idx < m_assetItems.size())
                    {
                        paths.push_back(m_assetItems[idx].fullPath);
                    }
                }
            }
            else
            {
                // Fallback: delete just this entry
                paths.push_back(entry.fullPath);
            }

            DeleteFilesToRecycleBin(paths);
        }

        ImGui::EndPopup();
    }
}

// Helper functions
void AssetsView::CopyToClipboard(const std::wstring& text)
{
    if (OpenClipboard(nullptr))
    {
        EmptyClipboard();

        size_t size = (text.length() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
        if (hMem)
        {
            memcpy(GlobalLock(hMem), text.c_str(), size);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }

        CloseClipboard();
    }
}

void AssetsView::CopyFilesToClipboard(const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return;

    // Clear cut state since we're copying
    m_cutFiles.clear();

    if (!OpenClipboard(nullptr))
        return;

    EmptyClipboard();

    // Calculate total size needed for DROPFILES structure + all paths + double null terminator
    size_t totalSize = sizeof(DROPFILES);
    for (const auto& path : paths)
    {
        totalSize += (path.length() + 1) * sizeof(wchar_t);
    }
    totalSize += sizeof(wchar_t);

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, totalSize);
    if (hGlobal)
    {
        DROPFILES* pDropFiles = (DROPFILES*)GlobalLock(hGlobal);
        if (pDropFiles)
        {
            pDropFiles->pFiles = sizeof(DROPFILES);
            pDropFiles->pt.x = 0;
            pDropFiles->pt.y = 0;
            pDropFiles->fNC = FALSE;
            pDropFiles->fWide = TRUE;

            wchar_t* pPath = (wchar_t*)((BYTE*)pDropFiles + sizeof(DROPFILES));
            for (const auto& path : paths)
            {
                size_t len = path.length() + 1;
                wcscpy_s(pPath, len, path.c_str());
                pPath += len;
            }
            *pPath = L'\0';

            GlobalUnlock(hGlobal);
            SetClipboardData(CF_HDROP, hGlobal);
        }
    }

    CloseClipboard();
}

void AssetsView::CutFilesToClipboard(const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return;

    CopyFilesToClipboard(paths);
    m_cutFiles = paths;
}

void AssetsView::PasteFilesFromClipboard()
{
    // Paste into the assets folder
    std::wstring targetDir = m_assetsFolderPath;

    if (!OpenClipboard(nullptr))
        return;

    HANDLE hData = GetClipboardData(CF_HDROP);
    if (hData)
    {
        HDROP hDrop = (HDROP)hData;
        UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);

        std::wstring sourceFiles;
        for (UINT i = 0; i < fileCount; i++)
        {
            wchar_t filePath[MAX_PATH];
            if (DragQueryFileW(hDrop, i, filePath, MAX_PATH))
            {
                sourceFiles += filePath;
                sourceFiles += L'\0';
            }
        }
        sourceFiles += L'\0';

        SHFILEOPSTRUCTW fileOp = {};
        fileOp.wFunc = FO_COPY;
        fileOp.pFrom = sourceFiles.c_str();
        fileOp.pTo = targetDir.c_str();
        fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;

        int result = SHFileOperationW(&fileOp);
        if (result == 0 && !m_cutFiles.empty())
        {
            DeleteFilesToRecycleBin(m_cutFiles);
            m_cutFiles.clear();
        }

        // Refresh file lists
        RefreshAssetItems();
    }

    CloseClipboard();
}

void AssetsView::RevealInExplorer(const std::wstring& path)
{
    std::wstring command = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", command.c_str(), nullptr, SW_SHOW);
}

void AssetsView::DeleteFilesToRecycleBin(const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return;

    // Build double-null terminated string for file paths
    std::wstring pathsDoubleNull;
    for (const auto& path : paths)
    {
        pathsDoubleNull += path;
        pathsDoubleNull.push_back(L'\0');
    }
    pathsDoubleNull.push_back(L'\0');

    // Use SHFileOperation to move to recycle bin
    SHFILEOPSTRUCTW fileOp = {};
    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = pathsDoubleNull.c_str();
    fileOp.fFlags = FOF_ALLOWUNDO | FOF_NO_UI;

    int result = SHFileOperationW(&fileOp);
    if (result == 0)
    {
        // Refresh file lists
        RefreshAssetItems();
    }
}

void AssetsView::ShowContextMenu(HWND hwnd, const std::wstring& path, const ImVec2& screenPos)
{
    // Get the full native Windows shell context menu with all options
    HRESULT hr = CoInitialize(nullptr);

    // Parse the file path to get parent folder and item
    std::filesystem::path fsPath(path);
    std::wstring parentPath = fsPath.parent_path().wstring();
    std::wstring fileName = fsPath.filename().wstring();

    // Get the desktop folder interface
    IShellFolder* pDesktopFolder = nullptr;
    hr = SHGetDesktopFolder(&pDesktopFolder);
    if (FAILED(hr) || !pDesktopFolder)
    {
        CoUninitialize();
        return;
    }

    // Parse the parent path to get its PIDL
    LPITEMIDLIST pidlParent = nullptr;
    hr = pDesktopFolder->ParseDisplayName(hwnd, nullptr, (LPWSTR)parentPath.c_str(), nullptr, &pidlParent, nullptr);
    if (FAILED(hr) || !pidlParent)
    {
        pDesktopFolder->Release();
        CoUninitialize();
        return;
    }

    // Get the IShellFolder for the parent directory
    IShellFolder* pParentFolder = nullptr;
    hr = pDesktopFolder->BindToObject(pidlParent, nullptr, IID_IShellFolder, (void**)&pParentFolder);
    CoTaskMemFree(pidlParent);
    pDesktopFolder->Release();

    if (FAILED(hr) || !pParentFolder)
    {
        CoUninitialize();
        return;
    }

    // Parse the file name to get its PIDL relative to parent
    LPITEMIDLIST pidlItem = nullptr;
    hr = pParentFolder->ParseDisplayName(hwnd, nullptr, (LPWSTR)fileName.c_str(), nullptr, &pidlItem, nullptr);
    if (FAILED(hr) || !pidlItem)
    {
        pParentFolder->Release();
        CoUninitialize();
        return;
    }

    // Get the IContextMenu interface
    IContextMenu* pContextMenu = nullptr;
    LPCITEMIDLIST pidlArray[1] = { pidlItem };
    hr = pParentFolder->GetUIObjectOf(hwnd, 1, pidlArray, IID_IContextMenu, nullptr, (void**)&pContextMenu);
    CoTaskMemFree(pidlItem);
    pParentFolder->Release();

    if (FAILED(hr) || !pContextMenu)
    {
        CoUninitialize();
        return;
    }

    // Create the context menu
    HMENU hMenu = CreatePopupMenu();
    if (hMenu)
    {
        // Populate the menu with shell items
        hr = pContextMenu->QueryContextMenu(hMenu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_EXPLORE);

        if (SUCCEEDED(hr))
        {
            // Convert ImGui screen coordinates to Windows coordinates
            POINT pt;
            pt.x = (LONG)screenPos.x;
            pt.y = (LONG)screenPos.y;

            // Show the menu
            int cmd = TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);

            // Execute the selected command
            if (cmd > 0)
            {
                CMINVOKECOMMANDINFOEX info = { 0 };
                info.cbSize = sizeof(info);
                info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                info.hwnd = hwnd;
                info.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                info.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                info.nShow = SW_SHOWNORMAL;
                info.ptInvoke = pt;

                pContextMenu->InvokeCommand((LPCMINVOKECOMMANDINFO)&info);
            }
        }

        DestroyMenu(hMenu);
    }

    pContextMenu->Release();
    CoUninitialize();
}

std::string AssetsView::FormatFileSize(uintmax_t size)
{
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    int unitIndex = 0;
    double displaySize = static_cast<double>(size);

    while (displaySize >= 1024.0 && unitIndex < 4)
    {
        displaySize /= 1024.0;
        unitIndex++;
    }

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.2f %s", displaySize, units[unitIndex]);
    return std::string(buffer);
}

std::string AssetsView::FormatFileTime(const std::filesystem::file_time_type& ftime)
{
    // Convert to system_clock time
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());

    std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
    std::tm* tm = std::localtime(&cftime);

    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm);
    return std::string(buffer);
}

ImVec4 AssetsView::GetAccentColor()
{
    // Use the global accent color function with transparency
    ImVec4 accent = GetWindowsAccentColor();
    accent.w = 0.3f;
    return accent;
}

void AssetsView::LoadMetadata()
{
    if (!m_subscriptionManager)
        return;

    // Fixed folder type for assets
    std::string folderType = "assets";

    // Get job path (parent of assets folder path)
    std::wstring jobPath = std::filesystem::path(m_assetsFolderPath).parent_path().wstring();

    // Load all asset metadata for this folder type from database
    auto allMetadata = m_subscriptionManager->GetShotMetadataByType(jobPath, folderType);

    // Map by asset path for quick lookup
    for (const auto& metadata : allMetadata)
    {
        m_assetMetadataMap[metadata.shotPath] = metadata;
    }

    // Collect available filter values from metadata
    CollectAvailableFilterValues();
}

void AssetsView::ReloadMetadata()
{
    // Reload metadata from database (called by observer when remote changes arrive)
    LoadMetadata();

    // Note: We don't need to refresh assets here - just updating the metadata map
    // The UI will pick up the new metadata on next frame
    std::wcout << L"[AssetsView] Metadata reloaded successfully" << std::endl;
}

void AssetsView::LoadColumnVisibility()
{
    m_visibleColumns.clear();

    std::string folderType = "assets";
    std::cout << "[AssetsView] LoadColumnVisibility called for folder type: " << folderType << std::endl;

    // Get displayMetadata for this folder type from projectConfig
    std::map<std::string, bool> displayMetadata;
    if (m_projectConfig && m_projectConfig->IsLoaded())
    {
        displayMetadata = m_projectConfig->GetDisplayMetadata(folderType);
        std::cout << "[AssetsView] Loaded display metadata for folder type: " << folderType << std::endl;
        std::cout << "[AssetsView] Display metadata size: " << displayMetadata.size() << std::endl;

        // If displayMetadata is empty, the project config doesn't have it - use hardcoded defaults
        if (displayMetadata.empty())
        {
            std::cout << "[AssetsView] WARNING: displayMetadata is empty for '" << folderType << "', using hardcoded defaults" << std::endl;
        }
    }
    else
    {
        std::cerr << "[AssetsView] WARNING: ProjectConfig not loaded! Using hardcoded defaults" << std::endl;
    }

    // If displayMetadata is empty (either from config not loaded or empty in config), use hardcoded defaults
    if (displayMetadata.empty())
    {
        std::cout << "[AssetsView] Applying hardcoded fallback defaults for folder type: " << folderType << std::endl;

        // Hardcoded defaults for assets folder
        displayMetadata["Status"] = true;
        displayMetadata["Category"] = true;
        displayMetadata["Artist"] = true;
        displayMetadata["Priority"] = false;
        displayMetadata["DueDate"] = false;
        displayMetadata["Notes"] = false;
        displayMetadata["Links"] = false;
    }

    // Populate visible columns map - if key exists in displayMetadata use it, otherwise default to false
    m_visibleColumns["Status"] = (displayMetadata.count("Status") > 0) ? displayMetadata["Status"] : false;
    m_visibleColumns["Category"] = (displayMetadata.count("Category") > 0) ? displayMetadata["Category"] : false;
    m_visibleColumns["Artist"] = (displayMetadata.count("Artist") > 0) ? displayMetadata["Artist"] : false;
    m_visibleColumns["Priority"] = (displayMetadata.count("Priority") > 0) ? displayMetadata["Priority"] : false;
    m_visibleColumns["DueDate"] = (displayMetadata.count("DueDate") > 0) ? displayMetadata["DueDate"] : false;
    m_visibleColumns["Notes"] = (displayMetadata.count("Notes") > 0) ? displayMetadata["Notes"] : false;
    m_visibleColumns["Links"] = (displayMetadata.count("Links") > 0) ? displayMetadata["Links"] : false;

    // Log the loaded column visibility
    std::cout << "[AssetsView] Column visibility for " << folderType << ":" << std::endl;
    std::cout << "  Status: " << m_visibleColumns["Status"] << std::endl;
    std::cout << "  Category: " << m_visibleColumns["Category"] << std::endl;
    std::cout << "  Artist: " << m_visibleColumns["Artist"] << std::endl;
    std::cout << "  Priority: " << m_visibleColumns["Priority"] << std::endl;
    std::cout << "  DueDate: " << m_visibleColumns["DueDate"] << std::endl;
    std::cout << "  Notes: " << m_visibleColumns["Notes"] << std::endl;
    std::cout << "  Links: " << m_visibleColumns["Links"] << std::endl;

    // Force flush to ensure we see the output
    std::cout.flush();
    std::cerr.flush();
}

void AssetsView::SaveColumnVisibility()
{
    if (!m_projectConfig)
    {
        std::cerr << "[AssetsView] Cannot save column visibility: ProjectConfig is null" << std::endl;
        return;
    }

    if (!m_projectConfig->IsLoaded())
    {
        std::cerr << "[AssetsView] Cannot save column visibility: ProjectConfig not loaded" << std::endl;
        return;
    }

    std::string folderType = "assets";

    std::cout << "[AssetsView] Saving column visibility for folder type: " << folderType << std::endl;

    // Log what we're trying to save
    for (const auto& [col, visible] : m_visibleColumns)
    {
        std::cout << "  " << col << ": " << (visible ? "true" : "false") << std::endl;
    }

    try
    {
        // Save to projectConfig (which will persist to disk)
        m_projectConfig->SetDisplayMetadata(folderType, m_visibleColumns);
        std::cout << "[AssetsView] Column visibility saved successfully" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[AssetsView] Error saving column visibility: " << e.what() << std::endl;
    }
}

ImVec4 AssetsView::GetStatusColor(const std::string& status)
{
    if (!m_projectConfig)
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White default

    std::string folderType = "assets";
    auto colorHex = m_projectConfig->GetStatusColor(folderType, status);

    if (colorHex.has_value())
    {
        // Parse hex color (e.g., "#3B82F6")
        std::string hex = colorHex.value();
        if (hex.length() == 7 && hex[0] == '#')
        {
            int r = std::stoi(hex.substr(1, 2), nullptr, 16);
            int g = std::stoi(hex.substr(3, 2), nullptr, 16);
            int b = std::stoi(hex.substr(5, 2), nullptr, 16);
            return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
        }
    }

    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White default
}

ImVec4 AssetsView::GetCategoryColor(const std::string& category)
{
    if (!m_projectConfig)
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White default

    std::string folderType = "assets";
    auto colorHex = m_projectConfig->GetCategoryColor(folderType, category);

    if (colorHex.has_value())
    {
        // Parse hex color (e.g., "#8B5CF6")
        std::string hex = colorHex.value();
        if (hex.length() == 7 && hex[0] == '#')
        {
            int r = std::stoi(hex.substr(1, 2), nullptr, 16);
            int g = std::stoi(hex.substr(3, 2), nullptr, 16);
            int b = std::stoi(hex.substr(5, 2), nullptr, 16);
            return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
        }
    }

    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White default
}

std::string AssetsView::FormatTimestamp(uint64_t timestamp)
{
    if (timestamp == 0)
        return "";

    // Convert Unix timestamp (milliseconds) to time_t (seconds)
    std::time_t time = static_cast<std::time_t>(timestamp / 1000);
    std::tm* tm = std::localtime(&time);

    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", tm);
    return std::string(buffer);
}

void AssetsView::HandleExternalDrop(const std::vector<std::wstring>& droppedPaths)
{
    // Delegate to the browser panel
    m_fileBrowser.HandleExternalDrop(droppedPaths);
}

bool AssetsView::IsBrowserHovered() const
{
    // Check if the browser panel is hovered
    return m_fileBrowser.IsHovered();
}

bool AssetsView::CreateNewAsset(const std::string& assetName)
{
    try
    {
        // Get current date in YYMMDD format
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        struct tm tm_now;
        localtime_s(&tm_now, &time_t_now);

        char datePrefix[7];
        snprintf(datePrefix, sizeof(datePrefix), "%02d%02d%02d",
                 tm_now.tm_year % 100, tm_now.tm_mon + 1, tm_now.tm_mday);

        // Find existing folders with this date prefix to determine next letter
        char letter = 'a';
        bool foundAvailable = false;

        for (char c = 'a'; c <= 'z' && !foundAvailable; c++)
        {
            std::string testFolderName = std::string(datePrefix) + c + "_";
            bool exists = false;

            // Check if any asset folder starts with this prefix
            for (const auto& asset : m_assetItems)
            {
                std::string assetNameUtf8 = UFB::WideToUtf8(asset.fullPath);
                std::filesystem::path assetPath(asset.fullPath);
                std::string folderName = assetPath.filename().string();

                if (folderName.substr(0, testFolderName.length()) == testFolderName)
                {
                    exists = true;
                    break;
                }
            }

            if (!exists)
            {
                letter = c;
                foundAvailable = true;
            }
        }

        if (!foundAvailable)
        {
            std::cerr << "[AssetsView] No available letter suffix for date: " << datePrefix << std::endl;
            return false;
        }

        // Create folder name: YYMMDDx_{AssetName}
        std::string folderName = std::string(datePrefix) + letter + "_" + assetName;
        std::wstring folderNameWide = UFB::Utf8ToWide(folderName);

        // Create full path
        std::filesystem::path newFolderPath = std::filesystem::path(m_assetsFolderPath) / folderNameWide;

        if (std::filesystem::exists(newFolderPath))
        {
            std::cerr << "[AssetsView] Folder already exists: " << folderName << std::endl;
            return false;
        }

        // Create the directory
        std::filesystem::create_directory(newFolderPath);
        std::cout << "[AssetsView] Created asset folder: " << newFolderPath << std::endl;

        // Refresh the asset list
        RefreshAssetItems();

        // Select the newly created asset
        for (size_t i = 0; i < m_assetItems.size(); i++)
        {
            if (m_assetItems[i].fullPath == newFolderPath.wstring())
            {
                m_selectedAssetIndex = static_cast<int>(i);
                break;
            }
        }

        std::cout << "[AssetsView] Successfully created asset: " << folderName << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[AssetsView] Error creating asset: " << e.what() << std::endl;
        return false;
    }
}

void AssetsView::CollectAvailableFilterValues()
{
    // Clear previous values
    m_availableCategories.clear();

    if (!m_projectConfig || !m_projectConfig->IsLoaded())
        return;

    // Get category options from ProjectConfig (assets folder type)
    auto categoryOptions = m_projectConfig->GetCategoryOptions("assets");
    for (const auto& categoryOpt : categoryOptions)
    {
        m_availableCategories.insert(categoryOpt.name);
    }
}

bool AssetsView::PassesFilters(const FileEntry& entry)
{
    // Get metadata for this asset
    auto it = m_assetMetadataMap.find(entry.fullPath);
    if (it == m_assetMetadataMap.end())
    {
        // No metadata - only show if all filters are empty (showing all)
        return m_filterCategories.empty() && m_filterDateModified == 0;
    }

    const UFB::ShotMetadata& metadata = it->second;

    // Check category filter
    if (!m_filterCategories.empty())
    {
        if (m_filterCategories.find(metadata.category) == m_filterCategories.end())
            return false;
    }

    // Check date modified filter
    if (m_filterDateModified != 0)
    {
        // Use file system modified time, not metadata modified time
        auto fileModifiedTime = entry.lastModified;
        auto now = std::filesystem::file_time_type::clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::hours>(now - fileModifiedTime).count();

        bool passes = false;
        switch (m_filterDateModified)
        {
            case 1: // Today
                passes = (diff < 24);
                break;
            case 2: // Yesterday
                passes = (diff >= 24 && diff < 48);
                break;
            case 3: // Last 7 days
                passes = (diff < 7 * 24);
                break;
            case 4: // Last 30 days
                passes = (diff < 30 * 24);
                break;
            case 5: // This year
            {
                // Check if modified within this calendar year (365 days)
                passes = (diff < 365 * 24);
                break;
            }
        }

        if (!passes)
            return false;
    }

    return true;
}
