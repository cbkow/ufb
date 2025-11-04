#include "postings_view.h"
#include "file_browser.h"  // For FileEntry struct
#include "bookmark_manager.h"
#include "subscription_manager.h"
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
bool PostingsView::showHiddenFiles = false;
std::vector<std::wstring> PostingsView::m_cutFiles;
int PostingsView::m_oleRefCount = 0;

// External font pointers (from main.cpp)
extern ImFont* font_regular;
extern ImFont* font_mono;
extern ImFont* font_icons;

// External accent color function (from main.cpp)
extern ImVec4 GetWindowsAccentColor();

PostingsView::PostingsView()
{
    // Initialize OLE for drag-drop support
    if (m_oleRefCount == 0)
    {
        HRESULT hr = OleInitialize(nullptr);
        if (FAILED(hr))
        {
            std::cerr << "[PostingsView] Failed to initialize OLE" << std::endl;
        }
    }
    m_oleRefCount++;
}

PostingsView::~PostingsView()
{
    Shutdown();
}

void PostingsView::Initialize(const std::wstring& postingsFolderPath, const std::wstring& jobName,
                          UFB::BookmarkManager* bookmarkManager,
                          UFB::SubscriptionManager* subscriptionManager)
{
    m_postingsFolderPath = postingsFolderPath;
    m_jobName = jobName;
    m_bookmarkManager = bookmarkManager;
    m_subscriptionManager = subscriptionManager;

    // Initialize managers
    m_iconManager.Initialize();
    m_thumbnailManager.Initialize();

    // Initialize FileBrowser for right panel
    m_fileBrowser.Initialize(m_bookmarkManager, m_subscriptionManager);
    m_fileBrowser.SetCurrentDirectory(postingsFolderPath);

    // Set up file browser callbacks to forward to our own callbacks
    m_fileBrowser.onTranscodeToMP4 = [this](const std::vector<std::wstring>& paths) {
        std::wcout << L"[PostingsView] FileBrowser transcode callback triggered with " << paths.size() << L" files" << std::endl;
        if (onTranscodeToMP4) {
            std::wcout << L"[PostingsView] Forwarding to parent onTranscodeToMP4 callback" << std::endl;
            onTranscodeToMP4(paths);
        } else {
            std::wcout << L"[PostingsView] WARNING: Parent onTranscodeToMP4 callback is NULL!" << std::endl;
        }
    };

    m_fileBrowser.onOpenInBrowser1 = [this](const std::wstring& path) {
        if (onOpenInBrowser1) onOpenInBrowser1(path);
    };

    m_fileBrowser.onOpenInBrowser2 = [this](const std::wstring& path) {
        if (onOpenInBrowser2) onOpenInBrowser2(path);
    };

    // Load or create ProjectConfig for this job
    std::wstring jobPath = std::filesystem::path(postingsFolderPath).parent_path().wstring();
    m_projectConfig = new UFB::ProjectConfig();
    bool configLoaded = m_projectConfig->LoadProjectConfig(jobPath);

    if (!configLoaded)
    {
        std::wcerr << L"[PostingsView] ERROR: Failed to load ProjectConfig from: " << jobPath << std::endl;
        std::wcerr << L"[PostingsView] Will use hardcoded fallback defaults for columns" << std::endl;
    }
    else
    {
        std::wcout << L"[PostingsView] Successfully loaded ProjectConfig from: " << jobPath << std::endl;
        std::cout << "[PostingsView] Config version: " << m_projectConfig->GetVersion() << std::endl;
    }

    // Load column visibility for postings folder type
    LoadColumnVisibility();

    // Load initial posting items
    RefreshPostingItems();
}

void PostingsView::Shutdown()
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

void PostingsView::SetSelectedPosting(const std::wstring& postingPath)
{
    // Find the posting in the postings list and select it
    for (size_t i = 0; i < m_postingItems.size(); i++)
    {
        if (m_postingItems[i].fullPath == postingPath)
        {
            m_selectedPostingIndex = static_cast<int>(i);

            // Update file browser to show this posting's contents
            m_fileBrowser.SetCurrentDirectory(postingPath);
            break;
        }
    }
}

void PostingsView::RefreshPostingItems()
{
    m_postingItems.clear();
    m_postingMetadataMap.clear();

    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(m_postingsFolderPath))
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

            m_postingItems.push_back(fileEntry);
        }

        // Sort alphabetically
        std::sort(m_postingItems.begin(), m_postingItems.end(), [](const FileEntry& a, const FileEntry& b) {
            return a.name < b.name;
        });

        // Load metadata for all posting items
        LoadMetadata();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[PostingsView] Error refreshing posting items: " << e.what() << std::endl;
    }
}

void PostingsView::Draw(const char* title, HWND hwnd)
{
    // Set initial window size (only on first use, then it's dockable/resizable)
    ImGui::SetNextWindowSize(ImVec2(1400, 800), ImGuiCond_FirstUseEver);

    // Use built-in close button (X on tab), disable scrollbars for main window
    bool windowOpen = ImGui::Begin(title, &m_isOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (windowOpen)
    {
        // Show postings folder path
        std::string postingsFolderPathUtf8 = UFB::WideToUtf8(m_postingsFolderPath);

        // Use mono font for path
        if (font_mono)
            ImGui::PushFont(font_mono);

        ImGui::TextDisabled("%s", postingsFolderPathUtf8.c_str());

        if (font_mono)
            ImGui::PopFont();

        ImGui::Separator();

        // Two-panel layout (50/50 split by default)
        ImVec2 availSize = ImGui::GetContentRegionAvail();
        ImVec2 windowPos = ImGui::GetCursorScreenPos();
        float panelSpacing = 8.0f;  // Spacing for divider line
        float leftWidth = availSize.x * 0.50f - panelSpacing / 2.0f;  // 50% for postings list
        float rightWidth = availSize.x * 0.50f - panelSpacing / 2.0f; // 50% for file browser

        // Left panel - Postings Table
        ImGui::BeginChild("PostingsPanel", ImVec2(leftWidth, availSize.y), false);  // No built-in border
        DrawPostingsPanel(hwnd);
        ImGui::EndChild();

        ImGui::SameLine(0.0f, panelSpacing);

        // Draw vertical separator line between Postings and Browser
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float lineX = windowPos.x + leftWidth + panelSpacing / 2.0f;
        ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        drawList->AddLine(ImVec2(lineX, windowPos.y), ImVec2(lineX, windowPos.y + availSize.y), lineColor, 1.0f);

        // Right panel - File Browser
        ImGui::BeginChild("BrowserPanel", ImVec2(rightWidth, availSize.y), false);  // No built-in border
        DrawBrowserPanel(hwnd);
        ImGui::EndChild();

        // Add new posting modal dialog
        if (m_showAddPostingDialog)
        {
            ImGui::OpenPopup("Add New Posting");
            m_showAddPostingDialog = false;
        }

        if (ImGui::BeginPopupModal("Add New Posting", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Enter posting name:");
            ImGui::Separator();

            ImGui::SetNextItemWidth(300);
            bool enterPressed = ImGui::InputText("##postingname", m_newPostingNameBuffer, sizeof(m_newPostingNameBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::Separator();

            if (ImGui::Button("Create", ImVec2(120, 0)) || enterPressed)
            {
                std::string postingName(m_newPostingNameBuffer);
                if (!postingName.empty())
                {
                    if (CreateNewPosting(postingName))
                    {
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        std::cerr << "[PostingsView] Failed to create posting: " << postingName << std::endl;
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

                    // Refresh posting list
                    RefreshPostingItems();
                }
                catch (const std::exception& e)
                {
                    std::cerr << "[PostingsView] Rename failed: " << e.what() << std::endl;
                }

                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    ImGui::End();

    // Check if window was closed via X button (check after End() so state is fully updated)
    if (!m_isOpen && onClose)
    {
        onClose();
    }
}

void PostingsView::DrawPostingsPanel(HWND hwnd)
{
    // Store window position and size
    m_postingsPanelPos = ImGui::GetWindowPos();
    m_postingsPanelSize = ImGui::GetWindowSize();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // Draw focus highlight border if this panel is focused
    if (isFocused)
    {
        ImVec4 accentColor = GetAccentColor();
        ImU32 highlightColor = ImGui::ColorConvertFloat4ToU32(ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.3f));

        float borderPadding = 4.0f;
        ImVec2 min = ImVec2(m_postingsPanelPos.x + borderPadding, m_postingsPanelPos.y + borderPadding);
        ImVec2 max = ImVec2(m_postingsPanelPos.x + m_postingsPanelSize.x - borderPadding, m_postingsPanelPos.y + m_postingsPanelSize.y - borderPadding);
        drawList->AddRect(min, max, highlightColor, 0.0f, 0, 3.0f);
    }

    // Create nested child window with padding to make room for the highlight border
    float contentPadding = 6.0f;
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    contentSize.x -= contentPadding * 2;
    contentSize.y -= contentPadding * 2;

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + contentPadding, ImGui::GetCursorPosY() + contentPadding));

    ImGui::BeginChild("##postings_content", contentSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::Text("Postings");

    // Place buttons at the end of the line
    // Calculate the space needed: 3 buttons + spacing
    float buttonWidth = font_icons ? 25.0f : 30.0f;  // Icon buttons are smaller
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float totalWidth = buttonWidth * 3 + spacing * 2;
    float availWidth = ImGui::GetContentRegionAvail().x;

    // Only move to the right if there's enough space (subtract a bit more for safety)
    if (availWidth > totalWidth + 10.0f)
    {
        ImGui::SameLine(availWidth - totalWidth - 16.0f);
    }
    else
    {
        ImGui::SameLine();
    }

    // Add new posting button
    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE145##addPosting")))  // Material Icons add
        {
            m_showAddPostingDialog = true;
            memset(m_newPostingNameBuffer, 0, sizeof(m_newPostingNameBuffer));
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("+##addPosting"))
        {
            m_showAddPostingDialog = true;
            memset(m_newPostingNameBuffer, 0, sizeof(m_newPostingNameBuffer));
        }
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add New Posting");

    ImGui::SameLine();

    // Columns filter button
    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE152##postingsColumns")))  // Material Icons filter_list
        {
            m_showColumnsPopup = true;
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("Cols##postingsColumns"))
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
        if (ImGui::Button(U8("\uE5D5##postings")))  // Material Icons refresh
        {
            RefreshPostingItems();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("R##postings"))
        {
            RefreshPostingItems();
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

    if (ImGui::BeginTable("PostingsTable", columnCount, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable))
    {
        // Name column (always first)
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);

        // Metadata columns (conditional)
        if (m_visibleColumns["Status"])
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);

        if (m_visibleColumns["Category"])
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 100.0f);

        if (m_visibleColumns["Artist"])
            ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthFixed, 120.0f);

        if (m_visibleColumns["Priority"])
            ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 80.0f);

        if (m_visibleColumns["DueDate"])
            ImGui::TableSetupColumn("Due Date", ImGuiTableColumnFlags_WidthFixed, 100.0f);

        if (m_visibleColumns["Notes"])
            ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthFixed, 200.0f);

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
                    m_postingSortSpecs = sortSpecs->Specs[0];

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
                    std::string sortField = columnIndexMap[m_postingSortSpecs.ColumnIndex];

                    // Sort by column
                    std::sort(m_postingItems.begin(), m_postingItems.end(), [this, sortField](const FileEntry& a, const FileEntry& b) {
                        bool ascending = (m_postingSortSpecs.SortDirection == ImGuiSortDirection_Ascending);

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

                            auto itA = m_postingMetadataMap.find(a.fullPath);
                            if (itA != m_postingMetadataMap.end()) metadataA = &itA->second;

                            auto itB = m_postingMetadataMap.find(b.fullPath);
                            if (itB != m_postingMetadataMap.end()) metadataB = &itB->second;

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

        for (int i = 0; i < m_postingItems.size(); i++)
        {
            const FileEntry& entry = m_postingItems[i];

            // Set minimum row height
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 35.0f);
            ImGui::TableNextColumn();

            ImGui::PushID(i);

            // Get icon
            ImTextureID icon = m_iconManager.GetFileIcon(entry.fullPath, entry.isDirectory);

            bool isSelected = (i == m_selectedPostingIndex);

            // Get or create metadata for this posting (needed for star icon)
            UFB::ShotMetadata* metadata = nullptr;
            auto metadataIt = m_postingMetadataMap.find(entry.fullPath);
            if (metadataIt != m_postingMetadataMap.end())
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
                // Select this posting item
                m_selectedPostingIndex = i;

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
                if (m_lastClickedPostingIndex == i && (currentTime - m_lastClickTime) < 0.3)
                {
                    // Double-clicked - open in file explorer or default app
                    ShellExecuteW(nullptr, L"open", entry.fullPath.c_str(), nullptr, nullptr, SW_SHOW);
                }
                m_lastClickTime = currentTime;
                m_lastClickedPostingIndex = i;
            }

            if (isSelected)
            {
                ImGui::PopStyleColor(3);
            }

            // Context menu
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                ImGui::OpenPopup("posting_context_menu");
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
                tempMetadata.folderType = "postings";  // Fixed folder type for postings
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
                std::cerr << "[PostingsView] ERROR: metadata pointer is null for posting: " << UFB::WideToUtf8(entry.fullPath) << std::endl;

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
                    statusOptions = m_projectConfig->GetStatusOptions("postings");
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
                    categoryOptions = m_projectConfig->GetCategoryOptions("postings");
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
                    m_datePickerPostingIndex = i;
                }

                // Date picker popup
                if (m_showDatePicker && m_datePickerPostingIndex == i)
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
                m_postingMetadataMap[entry.fullPath] = *metadata;

                // Save to database
                m_subscriptionManager->CreateOrUpdateShotMetadata(*metadata);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleVar();  // CellPadding

    ImGui::EndChild();  // End nested child window for postings panel
}

void PostingsView::DrawBrowserPanel(HWND hwnd)
{
    // Draw the full FileBrowser directly without extra child window
    // FileBrowser will handle its own highlighting
    m_fileBrowser.Draw("Browser", hwnd, false);
}

void PostingsView::ShowImGuiContextMenu(HWND hwnd, const FileEntry& entry)
{
    if (ImGui::BeginPopup("posting_context_menu"))
    {
        // Convert filename to UTF-8
        char nameUtf8[512];
        WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);

        // Header with filename
        ImGui::TextDisabled("%s", nameUtf8);
        ImGui::Separator();

        // Copy (file operation)
        if (ImGui::MenuItem("Copy"))
        {
            std::vector<std::wstring> paths = { entry.fullPath };
            CopyFilesToClipboard(paths);
        }

        // Cut (file operation)
        if (ImGui::MenuItem("Cut"))
        {
            std::vector<std::wstring> paths = { entry.fullPath };
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

            const char* menuLabel = isTracked ? "Untrack Posting" : "Track Posting";
            if (ImGui::MenuItem(menuLabel))
            {
                // Create or update metadata
                UFB::ShotMetadata postingMeta;
                if (metadata.has_value())
                {
                    postingMeta = *metadata;
                }
                else
                {
                    postingMeta.shotPath = entry.fullPath;
                    postingMeta.itemType = "posting";
                    postingMeta.folderType = "postings";
                    postingMeta.isTracked = false;
                }

                // Toggle tracking status
                postingMeta.isTracked = !isTracked;
                postingMeta.modifiedTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();

                // Save to database
                m_subscriptionManager->CreateOrUpdateShotMetadata(postingMeta);

                // Update local map
                m_postingMetadataMap[entry.fullPath] = postingMeta;
            }

            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        // Delete
        if (ImGui::MenuItem("Delete"))
        {
            std::vector<std::wstring> paths = { entry.fullPath };
            DeleteFilesToRecycleBin(paths);
        }

        ImGui::EndPopup();
    }
}

// Helper functions
void PostingsView::CopyToClipboard(const std::wstring& text)
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

void PostingsView::CopyFilesToClipboard(const std::vector<std::wstring>& paths)
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

void PostingsView::CutFilesToClipboard(const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return;

    CopyFilesToClipboard(paths);
    m_cutFiles = paths;
}

void PostingsView::PasteFilesFromClipboard()
{
    // Paste into the postings folder
    std::wstring targetDir = m_postingsFolderPath;

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
        RefreshPostingItems();
    }

    CloseClipboard();
}

void PostingsView::RevealInExplorer(const std::wstring& path)
{
    std::wstring command = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", command.c_str(), nullptr, SW_SHOW);
}

void PostingsView::DeleteFilesToRecycleBin(const std::vector<std::wstring>& paths)
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
        RefreshPostingItems();
    }
}

std::string PostingsView::FormatFileSize(uintmax_t size)
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

std::string PostingsView::FormatFileTime(const std::filesystem::file_time_type& ftime)
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

ImVec4 PostingsView::GetAccentColor()
{
    // Use the global accent color function with transparency
    ImVec4 accent = GetWindowsAccentColor();
    accent.w = 0.3f;
    return accent;
}

void PostingsView::LoadMetadata()
{
    if (!m_subscriptionManager)
        return;

    // Fixed folder type for postings
    std::string folderType = "postings";

    // Get job path (parent of postings folder path)
    std::wstring jobPath = std::filesystem::path(m_postingsFolderPath).parent_path().wstring();

    // Load all posting metadata for this folder type from database
    auto allMetadata = m_subscriptionManager->GetShotMetadataByType(jobPath, folderType);

    // Map by posting path for quick lookup
    for (const auto& metadata : allMetadata)
    {
        m_postingMetadataMap[metadata.shotPath] = metadata;
    }
}

void PostingsView::LoadColumnVisibility()
{
    m_visibleColumns.clear();

    std::string folderType = "postings";
    std::cout << "[PostingsView] LoadColumnVisibility called for folder type: " << folderType << std::endl;

    // Get displayMetadata for this folder type from projectConfig
    std::map<std::string, bool> displayMetadata;
    if (m_projectConfig && m_projectConfig->IsLoaded())
    {
        displayMetadata = m_projectConfig->GetDisplayMetadata(folderType);
        std::cout << "[PostingsView] Loaded display metadata for folder type: " << folderType << std::endl;
        std::cout << "[PostingsView] Display metadata size: " << displayMetadata.size() << std::endl;

        // If displayMetadata is empty, the project config doesn't have it - use hardcoded defaults
        if (displayMetadata.empty())
        {
            std::cout << "[PostingsView] WARNING: displayMetadata is empty for '" << folderType << "', using hardcoded defaults" << std::endl;
        }
    }
    else
    {
        std::cerr << "[PostingsView] WARNING: ProjectConfig not loaded! Using hardcoded defaults" << std::endl;
    }

    // If displayMetadata is empty (either from config not loaded or empty in config), use hardcoded defaults
    if (displayMetadata.empty())
    {
        std::cout << "[PostingsView] Applying hardcoded fallback defaults for folder type: " << folderType << std::endl;

        // Hardcoded defaults for postings folder
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
    std::cout << "[PostingsView] Column visibility for " << folderType << ":" << std::endl;
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

void PostingsView::SaveColumnVisibility()
{
    if (!m_projectConfig)
    {
        std::cerr << "[PostingsView] Cannot save column visibility: ProjectConfig is null" << std::endl;
        return;
    }

    if (!m_projectConfig->IsLoaded())
    {
        std::cerr << "[PostingsView] Cannot save column visibility: ProjectConfig not loaded" << std::endl;
        return;
    }

    std::string folderType = "postings";

    std::cout << "[PostingsView] Saving column visibility for folder type: " << folderType << std::endl;

    // Log what we're trying to save
    for (const auto& [col, visible] : m_visibleColumns)
    {
        std::cout << "  " << col << ": " << (visible ? "true" : "false") << std::endl;
    }

    try
    {
        // Save to projectConfig (which will persist to disk)
        m_projectConfig->SetDisplayMetadata(folderType, m_visibleColumns);
        std::cout << "[PostingsView] Column visibility saved successfully" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[PostingsView] Error saving column visibility: " << e.what() << std::endl;
    }
}

ImVec4 PostingsView::GetStatusColor(const std::string& status)
{
    if (!m_projectConfig)
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White default

    std::string folderType = "postings";
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

ImVec4 PostingsView::GetCategoryColor(const std::string& category)
{
    if (!m_projectConfig)
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White default

    std::string folderType = "postings";
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

std::string PostingsView::FormatTimestamp(uint64_t timestamp)
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

void PostingsView::HandleExternalDrop(const std::vector<std::wstring>& droppedPaths)
{
    // Delegate to the browser panel
    m_fileBrowser.HandleExternalDrop(droppedPaths);
}

bool PostingsView::IsBrowserHovered() const
{
    // Check if the browser panel is hovered
    return m_fileBrowser.IsHovered();
}

bool PostingsView::CreateNewPosting(const std::string& postingName)
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

            // Check if any posting folder starts with this prefix
            for (const auto& posting : m_postingItems)
            {
                std::string postingNameUtf8 = UFB::WideToUtf8(posting.fullPath);
                std::filesystem::path postingPath(posting.fullPath);
                std::string folderName = postingPath.filename().string();

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
            std::cerr << "[PostingsView] No available letter suffix for date: " << datePrefix << std::endl;
            return false;
        }

        // Create folder name: YYMMDDx_{PostingName}
        std::string folderName = std::string(datePrefix) + letter + "_" + postingName;
        std::wstring folderNameWide = UFB::Utf8ToWide(folderName);

        // Create full path
        std::filesystem::path newFolderPath = std::filesystem::path(m_postingsFolderPath) / folderNameWide;

        if (std::filesystem::exists(newFolderPath))
        {
            std::cerr << "[PostingsView] Folder already exists: " << folderName << std::endl;
            return false;
        }

        // Create the directory
        std::filesystem::create_directory(newFolderPath);
        std::cout << "[PostingsView] Created posting folder: " << newFolderPath << std::endl;

        // Refresh the posting list
        RefreshPostingItems();

        // Select the newly created posting
        for (size_t i = 0; i < m_postingItems.size(); i++)
        {
            if (m_postingItems[i].fullPath == newFolderPath.wstring())
            {
                m_selectedPostingIndex = static_cast<int>(i);
                break;
            }
        }

        std::cout << "[PostingsView] Successfully created posting: " << folderName << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[PostingsView] Error creating posting: " << e.what() << std::endl;
        return false;
    }
}
