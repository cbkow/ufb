#include "shot_view.h"
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
    // Clamp to valid range (1970-3000) to prevent crashes from corrupted data
    const uint64_t MAX_TIMESTAMP_MS = 32503680000000ULL;  // Year 3000
    if (timestampMillis > MAX_TIMESTAMP_MS) {
        timestampMillis = MAX_TIMESTAMP_MS;
    }

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
bool ShotView::showHiddenFiles = false;
std::vector<std::wstring> ShotView::m_cutFiles;
int ShotView::m_oleRefCount = 0;

// External font pointers (from main.cpp)
extern ImFont* font_regular;
extern ImFont* font_mono;
extern ImFont* font_icons;

// External accent color function (from main.cpp)
extern ImVec4 GetWindowsAccentColor();

ShotView::ShotView()
{
    // Initialize OLE for drag-drop support
    if (m_oleRefCount == 0)
    {
        HRESULT hr = OleInitialize(nullptr);
        if (FAILED(hr))
        {
            std::cerr << "[ShotView] Failed to initialize OLE" << std::endl;
        }
    }
    m_oleRefCount++;
}

ShotView::~ShotView()
{
    Shutdown();
}

void ShotView::Initialize(const std::wstring& categoryPath, const std::wstring& categoryName,
                          UFB::BookmarkManager* bookmarkManager,
                          UFB::SubscriptionManager* subscriptionManager,
                          UFB::MetadataManager* metadataManager)
{
    m_categoryPath = categoryPath;
    m_categoryName = categoryName;
    m_bookmarkManager = bookmarkManager;
    m_subscriptionManager = subscriptionManager;
    m_metadataManager = metadataManager;

    // Register observer for real-time metadata updates
    if (m_metadataManager)
    {
        std::wstring jobPath = std::filesystem::path(categoryPath).parent_path().wstring();
        m_metadataManager->RegisterObserver([this, jobPath](const std::wstring& changedJobPath) {
            // Only reload if the change is for our job
            if (changedJobPath == jobPath)
            {
                std::wcout << L"[ShotView] Metadata changed for job, reloading..." << std::endl;
                ReloadMetadata();
            }
        });
    }

    // Initialize managers
    m_iconManager.Initialize();
    m_thumbnailManager.Initialize();

    // Load or create ProjectConfig for this job
    std::wstring jobPath = std::filesystem::path(categoryPath).parent_path().wstring();
    m_projectConfig = new UFB::ProjectConfig();
    bool configLoaded = m_projectConfig->LoadProjectConfig(jobPath);

    if (!configLoaded)
    {
        std::wcerr << L"[ShotView] ERROR: Failed to load ProjectConfig from: " << jobPath << std::endl;
        std::wcerr << L"[ShotView] Will use hardcoded fallback defaults for columns" << std::endl;
    }
    else
    {
        std::wcout << L"[ShotView] Successfully loaded ProjectConfig from: " << jobPath << std::endl;
        std::cout << "[ShotView] Config version: " << m_projectConfig->GetVersion() << std::endl;
    }

    // Load column visibility for this category
    LoadColumnVisibility();

    // Load initial shots
    RefreshShots();
}

void ShotView::Shutdown()
{
    // Clean up ProjectConfig
    if (m_projectConfig)
    {
        delete m_projectConfig;
        m_projectConfig = nullptr;
    }

    m_iconManager.Shutdown();
    m_thumbnailManager.Shutdown();

    // Uninitialize OLE
    m_oleRefCount--;
    if (m_oleRefCount == 0)
    {
        OleUninitialize();
    }
}

void ShotView::RefreshShots()
{
    m_shots.clear();
    m_shotMetadataMap.clear();

    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(m_categoryPath))
        {
            if (entry.is_directory())
            {
                // Skip hidden folders if needed
                if (!showHiddenFiles && (entry.path().filename().wstring()[0] == L'.'))
                    continue;

                FileEntry fileEntry;
                fileEntry.name = entry.path().filename().wstring();
                fileEntry.fullPath = entry.path().wstring();
                fileEntry.isDirectory = true;
                fileEntry.size = 0;
                fileEntry.lastModified = entry.last_write_time();

                m_shots.push_back(fileEntry);
            }
        }

        // Sort alphabetically
        std::sort(m_shots.begin(), m_shots.end(), [](const FileEntry& a, const FileEntry& b) {
            return a.name < b.name;
        });

        // Load metadata for all shots
        LoadMetadata();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ShotView] Error refreshing shots: " << e.what() << std::endl;
    }
}

void ShotView::RefreshProjectFiles()
{
    m_projectFiles.clear();

    if (m_selectedShotPath.empty())
        return;

    try
    {
        // Try "project" and "projects" subdirectories
        std::vector<std::filesystem::path> searchPaths = {
            std::filesystem::path(m_selectedShotPath) / L"project",
            std::filesystem::path(m_selectedShotPath) / L"projects"
        };

        for (const auto& searchPath : searchPaths)
        {
            if (std::filesystem::exists(searchPath) && std::filesystem::is_directory(searchPath))
            {
                for (const auto& entry : std::filesystem::directory_iterator(searchPath))
                {
                    if (!entry.is_directory())
                    {
                        FileEntry fileEntry;
                        fileEntry.name = entry.path().filename().wstring();
                        fileEntry.fullPath = entry.path().wstring();
                        fileEntry.isDirectory = false;
                        fileEntry.size = entry.file_size();
                        fileEntry.lastModified = entry.last_write_time();

                        m_projectFiles.push_back(fileEntry);
                    }
                }
            }
        }

        // Sort alphabetically
        std::sort(m_projectFiles.begin(), m_projectFiles.end(), [](const FileEntry& a, const FileEntry& b) {
            return a.name < b.name;
        });
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ShotView] Error refreshing project files: " << e.what() << std::endl;
    }
}

void ShotView::RefreshRenderFiles()
{
    m_renderFiles.clear();

    // If m_renderCurrentDirectory is empty, initialize it to renders/outputs folder
    if (m_renderCurrentDirectory.empty())
    {
        if (m_selectedShotPath.empty())
            return;

        // Try "renders" and "outputs" subdirectories
        std::filesystem::path rendersPath = std::filesystem::path(m_selectedShotPath) / L"renders";
        std::filesystem::path outputsPath = std::filesystem::path(m_selectedShotPath) / L"outputs";

        if (std::filesystem::exists(rendersPath) && std::filesystem::is_directory(rendersPath))
        {
            m_renderCurrentDirectory = rendersPath.wstring();
        }
        else if (std::filesystem::exists(outputsPath) && std::filesystem::is_directory(outputsPath))
        {
            m_renderCurrentDirectory = outputsPath.wstring();
        }
        else
        {
            return;  // No renders or outputs folder found
        }
    }

    try
    {
        if (std::filesystem::exists(m_renderCurrentDirectory) && std::filesystem::is_directory(m_renderCurrentDirectory))
        {
            for (const auto& entry : std::filesystem::directory_iterator(m_renderCurrentDirectory))
            {
                // Include both files and directories
                FileEntry fileEntry;
                fileEntry.name = entry.path().filename().wstring();
                fileEntry.fullPath = entry.path().wstring();
                fileEntry.isDirectory = entry.is_directory();
                fileEntry.size = entry.is_directory() ? 0 : entry.file_size();
                fileEntry.lastModified = entry.last_write_time();

                m_renderFiles.push_back(fileEntry);
            }
        }

        // Sort by last modified (newest first) - will be overridden by table sorting if user clicks column
        std::sort(m_renderFiles.begin(), m_renderFiles.end(), [](const FileEntry& a, const FileEntry& b) {
            return a.lastModified > b.lastModified;
        });
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ShotView] Error refreshing render files: " << e.what() << std::endl;
    }
}

void ShotView::NavigateToRenderDirectory(const std::wstring& path)
{
    // Don't add to history if we're navigating via back/forward
    if (!m_isNavigatingRenderHistory)
    {
        // Add current directory to back history before navigating
        if (!m_renderCurrentDirectory.empty())
        {
            m_renderBackHistory.push_back(m_renderCurrentDirectory);
        }

        // Clear forward history when navigating to a new directory
        m_renderForwardHistory.clear();
    }

    m_renderCurrentDirectory = path;
    RefreshRenderFiles();
}

void ShotView::NavigateRenderUp()
{
    if (m_renderCurrentDirectory.empty())
        return;

    std::filesystem::path currentPath(m_renderCurrentDirectory);
    std::filesystem::path parentPath = currentPath.parent_path();

    // Don't navigate above the shot's renders/outputs folder
    if (m_selectedShotPath.empty())
        return;

    std::filesystem::path shotPath(m_selectedShotPath);
    std::filesystem::path rendersPath = shotPath / L"renders";
    std::filesystem::path outputsPath = shotPath / L"outputs";

    // Check if parent is still within renders or outputs folder
    std::wstring parentStr = parentPath.wstring();
    std::wstring rendersStr = rendersPath.wstring();
    std::wstring outputsStr = outputsPath.wstring();

    if (parentStr.find(rendersStr) != 0 && parentStr.find(outputsStr) != 0)
    {
        // Don't navigate outside of renders/outputs
        return;
    }

    if (parentPath != currentPath)
    {
        NavigateToRenderDirectory(parentPath.wstring());
    }
}

void ShotView::NavigateRenderBack()
{
    if (m_renderBackHistory.empty())
        return;

    // Move current directory to forward history
    if (!m_renderCurrentDirectory.empty())
    {
        m_renderForwardHistory.push_back(m_renderCurrentDirectory);
    }

    // Pop from back history
    std::wstring previousPath = m_renderBackHistory.back();
    m_renderBackHistory.pop_back();

    // Navigate without adding to history
    m_isNavigatingRenderHistory = true;
    m_renderCurrentDirectory = previousPath;
    RefreshRenderFiles();
    m_isNavigatingRenderHistory = false;
}

void ShotView::NavigateRenderForward()
{
    if (m_renderForwardHistory.empty())
        return;

    // Move current directory to back history
    if (!m_renderCurrentDirectory.empty())
    {
        m_renderBackHistory.push_back(m_renderCurrentDirectory);
    }

    // Pop from forward history
    std::wstring nextPath = m_renderForwardHistory.back();
    m_renderForwardHistory.pop_back();

    // Navigate without adding to history
    m_isNavigatingRenderHistory = true;
    m_renderCurrentDirectory = nextPath;
    RefreshRenderFiles();
    m_isNavigatingRenderHistory = false;
}

void ShotView::Draw(const char* title, HWND hwnd)
{
    // Set initial window size (only on first use, then it's dockable/resizable)
    ImGui::SetNextWindowSize(ImVec2(1200, 800), ImGuiCond_FirstUseEver);

    // Use built-in close button (X on tab), disable scrollbars for main window
    bool windowOpen = ImGui::Begin(title, &m_isOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (windowOpen)
    {
        // Show category path
        std::string categoryPathUtf8 = UFB::WideToUtf8(m_categoryPath);

        // Use mono font for path
        if (font_mono)
            ImGui::PushFont(font_mono);

        ImGui::TextDisabled("%s", categoryPathUtf8.c_str());

        if (font_mono)
            ImGui::PopFont();

        ImGui::Separator();

        // Three-panel layout (60/40 split)
        ImVec2 availSize = ImGui::GetContentRegionAvail();
        ImVec2 windowPos = ImGui::GetCursorScreenPos();
        float panelSpacing = 8.0f;  // Spacing for divider lines
        float leftWidth = availSize.x * 0.60f - panelSpacing;  // 60% for shots
        float rightWidth = availSize.x * 0.40f; // 40% split between projects and renders

        // Left panel - Shots
        ImGui::BeginChild("ShotsPanel", ImVec2(leftWidth, availSize.y), false);  // No built-in border
        DrawShotsPanel(hwnd);
        ImGui::EndChild();

        ImGui::SameLine(0.0f, panelSpacing);

        // Draw vertical separator line between Shots and Projects/Renders
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float line1_x = windowPos.x + leftWidth + panelSpacing / 2.0f;
        ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        drawList->AddLine(ImVec2(line1_x, windowPos.y), ImVec2(line1_x, windowPos.y + availSize.y), lineColor, 1.0f);

        // Right panels - Projects (top) and Renders (bottom)
        ImGui::BeginGroup();

        // Projects panel
        ImGui::BeginChild("ProjectsPanel", ImVec2(rightWidth, availSize.y * 0.5f - panelSpacing / 2.0f), false);  // No built-in border
        DrawProjectsPanel(hwnd);
        ImGui::EndChild();

        // Draw horizontal separator line between Projects and Renders
        float rightPanelX = windowPos.x + leftWidth + panelSpacing;
        float line2_y = windowPos.y + availSize.y * 0.5f;
        drawList->AddLine(ImVec2(rightPanelX, line2_y), ImVec2(rightPanelX + rightWidth, line2_y), lineColor, 1.0f);

        // Renders panel
        ImGui::BeginChild("RendersPanel", ImVec2(rightWidth, availSize.y * 0.5f - panelSpacing / 2.0f), false);  // No built-in border
        DrawRendersPanel(hwnd);
        ImGui::EndChild();

        ImGui::EndGroup();

        // Handle keyboard shortcuts (Ctrl+C, Ctrl+X, Ctrl+V, Delete, F2)
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
        {
            ImGuiIO& io = ImGui::GetIO();

            // Ctrl+C - Copy selected files
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
            {
                std::vector<std::wstring> paths;

                // Check which panel has selection
                if (!m_selectedProjectIndices.empty())
                {
                    for (int idx : m_selectedProjectIndices)
                    {
                        if (idx >= 0 && idx < m_projectFiles.size())
                        {
                            paths.push_back(m_projectFiles[idx].fullPath);
                        }
                    }
                }
                else if (!m_selectedRenderIndices.empty())
                {
                    for (int idx : m_selectedRenderIndices)
                    {
                        if (idx >= 0 && idx < m_renderFiles.size())
                        {
                            paths.push_back(m_renderFiles[idx].fullPath);
                        }
                    }
                }

                if (!paths.empty())
                {
                    CopyFilesToClipboard(paths);
                }
            }

            // Ctrl+X - Cut selected files
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X))
            {
                std::vector<std::wstring> paths;

                if (!m_selectedProjectIndices.empty())
                {
                    for (int idx : m_selectedProjectIndices)
                    {
                        if (idx >= 0 && idx < m_projectFiles.size())
                        {
                            paths.push_back(m_projectFiles[idx].fullPath);
                        }
                    }
                }
                else if (!m_selectedRenderIndices.empty())
                {
                    for (int idx : m_selectedRenderIndices)
                    {
                        if (idx >= 0 && idx < m_renderFiles.size())
                        {
                            paths.push_back(m_renderFiles[idx].fullPath);
                        }
                    }
                }

                if (!paths.empty())
                {
                    CutFilesToClipboard(paths);
                }
            }

            // Ctrl+V - Paste files
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V))
            {
                PasteFilesFromClipboard();
            }

            // Delete - Delete selected files
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))
            {
                std::vector<std::wstring> paths;

                if (!m_selectedProjectIndices.empty())
                {
                    for (int idx : m_selectedProjectIndices)
                    {
                        if (idx >= 0 && idx < m_projectFiles.size())
                        {
                            paths.push_back(m_projectFiles[idx].fullPath);
                        }
                    }
                }
                else if (!m_selectedRenderIndices.empty())
                {
                    for (int idx : m_selectedRenderIndices)
                    {
                        if (idx >= 0 && idx < m_renderFiles.size())
                        {
                            paths.push_back(m_renderFiles[idx].fullPath);
                        }
                    }
                }

                if (!paths.empty())
                {
                    DeleteFilesToRecycleBin(paths);
                }
            }

            // F2 - Rename (single file only)
            if (ImGui::IsKeyPressed(ImGuiKey_F2))
            {
                // Only allow rename if exactly one file is selected
                if (m_selectedProjectIndices.size() == 1)
                {
                    int idx = *m_selectedProjectIndices.begin();
                    if (idx >= 0 && idx < m_projectFiles.size())
                    {
                        m_renameOriginalPath = m_projectFiles[idx].fullPath;
                        std::wstring filename = m_projectFiles[idx].name;
                        WideCharToMultiByte(CP_UTF8, 0, filename.c_str(), -1, m_renameBuffer, sizeof(m_renameBuffer), nullptr, nullptr);
                        m_showRenameDialog = true;
                    }
                }
                else if (m_selectedRenderIndices.size() == 1)
                {
                    int idx = *m_selectedRenderIndices.begin();
                    if (idx >= 0 && idx < m_renderFiles.size())
                    {
                        m_renameOriginalPath = m_renderFiles[idx].fullPath;
                        std::wstring filename = m_renderFiles[idx].name;
                        WideCharToMultiByte(CP_UTF8, 0, filename.c_str(), -1, m_renameBuffer, sizeof(m_renameBuffer), nullptr, nullptr);
                        m_showRenameDialog = true;
                    }
                }
            }
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

                    // Refresh appropriate file list
                    RefreshShots();
                    if (!m_selectedShotPath.empty())
                    {
                        RefreshProjectFiles();
                        RefreshRenderFiles();
                    }
                }
                catch (const std::exception& e)
                {
                    std::cerr << "[ShotView] Rename failed: " << e.what() << std::endl;
                }

                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    ImGui::End();
}

std::wstring ShotView::GetJobName() const
{
    // Extract job name from category path
    // categoryPath is like: "C:\Volumes\union-ny-gfx\union-jobs\000000_OH\ae"
    // We want to get "000000_OH" (the parent folder name)
    try
    {
        std::filesystem::path catPath(m_categoryPath);
        std::filesystem::path jobPath = catPath.parent_path();
        return jobPath.filename().wstring();
    }
    catch (...)
    {
        return L"Unknown";
    }
}

void ShotView::SetSelectedShot(const std::wstring& shotPath)
{
    // Find the shot in the shots list and select it
    for (size_t i = 0; i < m_shots.size(); i++)
    {
        if (m_shots[i].fullPath == shotPath)
        {
            m_selectedShotIndex = static_cast<int>(i);
            m_selectedShotPath = shotPath;

            // Refresh project and render files for this shot
            RefreshProjectFiles();
            RefreshRenderFiles();
            break;
        }
    }
}

void ShotView::SetSelectedShotAndFile(const std::wstring& shotPath, const std::wstring& filePath)
{
    // First select the shot
    SetSelectedShot(shotPath);

    // Clear previous file selections
    m_selectedProjectIndices.clear();
    m_selectedRenderIndices.clear();

    // Canonicalize the file path for comparison
    std::wstring canonicalFilePath;
    try {
        canonicalFilePath = std::filesystem::canonical(filePath).wstring();
    } catch (...) {
        canonicalFilePath = filePath;
    }

    // Try to find and select the file in project files
    for (size_t i = 0; i < m_projectFiles.size(); i++)
    {
        if (m_projectFiles[i].fullPath == canonicalFilePath)
        {
            m_selectedProjectIndices.insert(static_cast<int>(i));
            std::wcout << L"[ShotView] Selected file in project panel: " << m_projectFiles[i].name << std::endl;
            return;
        }
    }

    // Try to find and select the file in render files
    for (size_t i = 0; i < m_renderFiles.size(); i++)
    {
        if (m_renderFiles[i].fullPath == canonicalFilePath)
        {
            m_selectedRenderIndices.insert(static_cast<int>(i));
            std::wcout << L"[ShotView] Selected file in render panel: " << m_renderFiles[i].name << std::endl;
            return;
        }
    }

    std::wcout << L"[ShotView] File not found in project or render panels: " << canonicalFilePath << std::endl;
}

void ShotView::DrawShotsPanel(HWND hwnd)
{
    // Store window position and size
    m_shotsPanelPos = ImGui::GetWindowPos();
    m_shotsPanelSize = ImGui::GetWindowSize();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // Draw focus highlight border if this panel is focused
    if (isFocused)
    {
        ImVec4 accentColor = GetAccentColor();
        ImU32 highlightColor = ImGui::ColorConvertFloat4ToU32(ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.3f));

        float borderPadding = 4.0f;
        ImVec2 min = ImVec2(m_shotsPanelPos.x + borderPadding, m_shotsPanelPos.y + borderPadding);
        ImVec2 max = ImVec2(m_shotsPanelPos.x + m_shotsPanelSize.x - borderPadding, m_shotsPanelPos.y + m_shotsPanelSize.y - borderPadding);
        drawList->AddRect(min, max, highlightColor, 0.0f, 0, 3.0f);
    }

    // Create nested child window with padding to make room for the highlight border
    float contentPadding = 6.0f;
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    contentSize.x -= contentPadding * 2;
    contentSize.y -= contentPadding * 2;

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + contentPadding, ImGui::GetCursorPosY() + contentPadding));

    ImGui::BeginChild("##shots_content", contentSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::Text("Shots");

    // Add new shot button
    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE145##addShot")))  // Material Icons add
        {
            m_showAddShotDialog = true;
            memset(m_newShotNameBuffer, 0, sizeof(m_newShotNameBuffer));
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("+##addShot"))
        {
            m_showAddShotDialog = true;
            memset(m_newShotNameBuffer, 0, sizeof(m_newShotNameBuffer));
        }
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add New Shot");

    ImGui::SameLine();

    // ===== COMPACT FILTER BUTTONS =====

    // Status Filter Button
    int statusCount = static_cast<int>(m_filterStatuses.size());
    std::string statusLabel = "Status" + (statusCount > 0 ? " (" + std::to_string(statusCount) + ")" : "");
    if (ImGui::Button(statusLabel.c_str()))
    {
        ImGui::OpenPopup("StatusFilterPopup");
    }

    // Status Filter Popup
    if (ImGui::BeginPopup("StatusFilterPopup"))
    {
        ImGui::Text("Filter by Status:");
        ImGui::Separator();
        for (const auto& status : m_availableStatuses)
        {
            bool isSelected = (m_filterStatuses.find(status) != m_filterStatuses.end());
            if (ImGui::Checkbox(status.c_str(), &isSelected))
            {
                if (isSelected)
                    m_filterStatuses.insert(status);
                else
                    m_filterStatuses.erase(status);
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Clear All"))
        {
            m_filterStatuses.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();

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

    // Artist Filter Button
    int artistCount = static_cast<int>(m_filterArtists.size());
    std::string artistLabel = "Artist" + (artistCount > 0 ? " (" + std::to_string(artistCount) + ")" : "");
    if (ImGui::Button(artistLabel.c_str()))
    {
        ImGui::OpenPopup("ArtistFilterPopup");
    }

    // Artist Filter Popup
    if (ImGui::BeginPopup("ArtistFilterPopup"))
    {
        ImGui::Text("Filter by Artist:");
        ImGui::Separator();
        for (const auto& artist : m_availableArtists)
        {
            bool isSelected = (m_filterArtists.find(artist) != m_filterArtists.end());
            if (ImGui::Checkbox(artist.c_str(), &isSelected))
            {
                if (isSelected)
                    m_filterArtists.insert(artist);
                else
                    m_filterArtists.erase(artist);
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Clear All"))
        {
            m_filterArtists.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();

    // Priority Filter Button
    int priorityCount = static_cast<int>(m_filterPriorities.size());
    std::string priorityLabel = "Priority" + (priorityCount > 0 ? " (" + std::to_string(priorityCount) + ")" : "");
    if (ImGui::Button(priorityLabel.c_str()))
    {
        ImGui::OpenPopup("PriorityFilterPopup");
    }

    // Priority Filter Popup
    if (ImGui::BeginPopup("PriorityFilterPopup"))
    {
        ImGui::Text("Filter by Priority:");
        ImGui::Separator();
        const char* priorityLabels[] = { "High", "Medium", "Low" };
        for (int priority : m_availablePriorities)
        {
            if (priority >= 1 && priority <= 3)
            {
                bool isSelected = (m_filterPriorities.find(priority) != m_filterPriorities.end());
                if (ImGui::Checkbox(priorityLabels[priority - 1], &isSelected))
                {
                    if (isSelected)
                        m_filterPriorities.insert(priority);
                    else
                        m_filterPriorities.erase(priority);
                }
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Clear All"))
        {
            m_filterPriorities.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();

    // Due Date Filter Button
    const char* dueDateOptions[] = { "All", "Today", "Yesterday", "Last 7 days", "Last 30 days", "This year" };
    std::string dueDateLabel = "Due Date";
    if (m_filterDueDate > 0)
    {
        dueDateLabel = std::string(dueDateOptions[m_filterDueDate]);
    }
    if (ImGui::Button(dueDateLabel.c_str()))
    {
        ImGui::OpenPopup("DueDateFilterPopup");
    }

    // Due Date Filter Popup
    if (ImGui::BeginPopup("DueDateFilterPopup"))
    {
        ImGui::Text("Filter by Due Date:");
        ImGui::Separator();
        for (int i = 0; i < 6; i++)
        {
            bool isSelected = (m_filterDueDate == i);
            if (ImGui::Selectable(dueDateOptions[i], isSelected))
            {
                m_filterDueDate = i;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();

    // Clear Filters Button
    int totalActiveFilters = statusCount + categoryCount + artistCount + priorityCount + (m_filterDueDate > 0 ? 1 : 0);
    if (totalActiveFilters > 0)
    {
        if (font_icons) ImGui::PushFont(font_icons);
        if (ImGui::SmallButton(U8("\uE14C##clearFilters")))  // Material Icons close
        {
            m_filterStatuses.clear();
            m_filterCategories.clear();
            m_filterArtists.clear();
            m_filterPriorities.clear();
            m_filterDueDate = 0;
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
        if (ImGui::Button(U8("\uE152##shotsColumns")))  // Material Icons filter_list
        {
            m_showColumnsPopup = true;
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("Cols##shotsColumns"))
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
        if (ImGui::Button(U8("\uE5D5##shots")))  // Material Icons refresh
        {
            RefreshShots();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("R##shots"))
        {
            RefreshShots();
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

    // Debug: Warn if no metadata columns are visible
    if (columnCount == 2)
    {
        std::cout << "[ShotView] WARNING: No metadata columns are visible in Shots table!" << std::endl;
        std::cout << "[ShotView] Current m_visibleColumns state:" << std::endl;
        for (const auto& [col, visible] : m_visibleColumns)
        {
            std::cout << "  " << col << ": " << (visible ? "true" : "false") << std::endl;
        }
    }
    else
    {
        //std::cout << "[ShotView] Shots table will have " << columnCount << " columns (" << (columnCount - 2) << " metadata columns)" << std::endl;
    }

    // Push larger cell padding for taller rows (match transcoding queue style)
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 8.0f));

    if (ImGui::BeginTable("ShotsTable", columnCount, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable))
    {
        // Name column (always first)
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);

        // Metadata columns (conditional) - widths adjusted for typical shot content
        if (m_visibleColumns["Status"])
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 140.0f);

        if (m_visibleColumns["Category"])
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 130.0f);

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
                    m_shotSortSpecs = sortSpecs->Specs[0];

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
                    std::string sortField = columnIndexMap[m_shotSortSpecs.ColumnIndex];

                    // Sort by column
                    std::sort(m_shots.begin(), m_shots.end(), [this, sortField](const FileEntry& a, const FileEntry& b) {
                        bool ascending = (m_shotSortSpecs.SortDirection == ImGuiSortDirection_Ascending);

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

                            auto itA = m_shotMetadataMap.find(a.fullPath);
                            if (itA != m_shotMetadataMap.end()) metadataA = &itA->second;

                            auto itB = m_shotMetadataMap.find(b.fullPath);
                            if (itB != m_shotMetadataMap.end()) metadataB = &itB->second;

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

        for (int i = 0; i < m_shots.size(); i++)
        {
            const FileEntry& entry = m_shots[i];

            // Apply filters - skip if doesn't pass
            if (!PassesFilters(entry))
                continue;

            // Set minimum row height (match transcoding queue style)
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 35.0f);
            ImGui::TableNextColumn();

            ImGui::PushID(i);

            // Get icon
            ImTextureID icon = m_iconManager.GetFileIcon(entry.fullPath, true);

            bool isSelected = (i == m_selectedShotIndex);

            // Get or create metadata for this shot (needed for star icon)
            UFB::ShotMetadata* metadata = nullptr;
            auto metadataIt = m_shotMetadataMap.find(entry.fullPath);
            if (metadataIt != m_shotMetadataMap.end())
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
                // Select this shot
                m_selectedShotIndex = i;
                m_selectedShotPath = entry.fullPath;

                // Reset render navigation state when switching shots
                m_renderCurrentDirectory.clear();
                m_renderBackHistory.clear();
                m_renderForwardHistory.clear();

                // Refresh project and render files
                RefreshProjectFiles();
                RefreshRenderFiles();

                // Double-click detection
                double currentTime = glfwGetTime();
                if (m_lastClickedShotIndex == i && (currentTime - m_lastClickTime) < 0.3)
                {
                    // Double-clicked - open in file explorer
                    ShellExecuteW(nullptr, L"open", entry.fullPath.c_str(), nullptr, nullptr, SW_SHOW);
                }
                m_lastClickTime = currentTime;
                m_lastClickedShotIndex = i;
            }

            if (isSelected)
            {
                ImGui::PopStyleColor(3);
            }

            // Context menu
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                ImGui::OpenPopup("shot_context_menu");
            }

            ShowImGuiContextMenu(hwnd, entry, PanelType::Shots);

            // Push mono font for all metadata columns (not the name)
            if (font_mono)
                ImGui::PushFont(font_mono);

            // If no metadata exists, create a default one in memory
            UFB::ShotMetadata tempMetadata;
            if (!metadata)
            {
                tempMetadata.shotPath = entry.fullPath;
                tempMetadata.folderType = UFB::WideToUtf8(m_categoryName);
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
                std::cerr << "[ShotView] ERROR: metadata pointer is null for shot: " << UFB::WideToUtf8(entry.fullPath) << std::endl;

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
                ImGui::TextDisabled("%s", FormatFileTime(entry.lastModified).c_str());

                // Pop mono font before continuing
                if (font_mono)
                    ImGui::PopFont();

                ImGui::PopID();
                continue;  // Skip to next shot
            }

            // Status column - Editable combo box
            if (m_visibleColumns["Status"])
            {
                ImGui::TableNextColumn();

                // Get status options from ProjectConfig using the shot's folderType
                std::vector<UFB::StatusOption> statusOptions;
                if (m_projectConfig && !metadata->folderType.empty())
                {
                    statusOptions = m_projectConfig->GetStatusOptions(metadata->folderType);
                    // Debug logging
                    if (i == 0)  // Only log for first item to avoid spam
                    {
                       /* printf("[DEBUG] Shot View - folderType: '%s', statusOptions.size(): %zu\n",
                               metadata->folderType.c_str(), statusOptions.size());*/
                    }
                }
                else
                {
                    if (i == 0)
                    {
                       /* printf("[DEBUG] Shot View - Cannot get status options. ProjectConfig: %p, folderType: '%s'\n",
                               (void*)m_projectConfig, metadata->folderType.c_str());*/
                    }
                }

                std::string currentStatus = metadata->status;
                std::string displayStatus = currentStatus.empty() ? "Not Set" : currentStatus;

                // Color the combo box button based on status
                ImVec4 statusColor = currentStatus.empty() ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f) : GetStatusColor(currentStatus);
                ImGui::PushStyleColor(ImGuiCol_Text, statusColor);

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(("##status" + std::to_string(i)).c_str(), displayStatus.c_str()))
                {
                    if (statusOptions.empty())
                    {
                        ImGui::TextDisabled("(No options configured)");
                    }
                    else
                    {
                        for (const auto& option : statusOptions)
                        {
                            bool isSelected = (currentStatus == option.name);
                            ImVec4 optionColor = GetStatusColor(option.name);
                            ImGui::PushStyleColor(ImGuiCol_Text, optionColor);

                            if (ImGui::Selectable(option.name.c_str(), isSelected))
                            {
                                metadata->status = option.name;
                                metadataChanged = true;
                            }

                            ImGui::PopStyleColor();

                            if (isSelected)
                                ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::PopStyleColor();
            }

            // Category column - Editable combo box
            if (m_visibleColumns["Category"])
            {
                ImGui::TableNextColumn();

                // Get category options from ProjectConfig using the shot's folderType
                std::vector<UFB::CategoryOption> categoryOptions;
                if (m_projectConfig && !metadata->folderType.empty())
                {
                    categoryOptions = m_projectConfig->GetCategoryOptions(metadata->folderType);
                    // Debug logging
                    if (i == 0)  // Only log for first item to avoid spam
                    {
          /*              printf("[DEBUG] Shot View - folderType: '%s', categoryOptions.size(): %zu\n",
                               metadata->folderType.c_str(), categoryOptions.size());*/
                    }
                }
                else
                {
                    if (i == 0)
                    {
                       /* printf("[DEBUG] Shot View - Cannot get category options. ProjectConfig: %p, folderType: '%s'\n",
                               (void*)m_projectConfig, metadata->folderType.c_str());*/
                    }
                }

                std::string currentCategory = metadata->category;
                std::string displayCategory = currentCategory.empty() ? "Not Set" : currentCategory;

                // Color the combo box button based on category
                ImVec4 categoryColor = currentCategory.empty() ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f) : GetCategoryColor(currentCategory);
                ImGui::PushStyleColor(ImGuiCol_Text, categoryColor);

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(("##category" + std::to_string(i)).c_str(), displayCategory.c_str()))
                {
                    if (categoryOptions.empty())
                    {
                        ImGui::TextDisabled("(No options configured)");
                    }
                    else
                    {
                        for (const auto& option : categoryOptions)
                        {
                            bool isSelected = (currentCategory == option.name);
                            ImVec4 optionColor = GetCategoryColor(option.name);
                            ImGui::PushStyleColor(ImGuiCol_Text, optionColor);

                            if (ImGui::Selectable(option.name.c_str(), isSelected))
                            {
                                metadata->category = option.name;
                                metadataChanged = true;
                            }

                            ImGui::PopStyleColor();

                            if (isSelected)
                                ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::PopStyleColor();
            }

            // Artist column - Editable combo box (from users list)
            if (m_visibleColumns["Artist"])
            {
                ImGui::TableNextColumn();

                // Get users from ProjectConfig
                std::vector<UFB::User> users;
                if (m_projectConfig)
                    users = m_projectConfig->GetUsers();

                std::string currentArtist = metadata->artist;
                std::string displayArtist = currentArtist.empty() ? "Not Assigned" : currentArtist;

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(("##artist" + std::to_string(i)).c_str(), displayArtist.c_str()))
                {
                    // "Not Assigned" option (always available)
                    if (ImGui::Selectable("Not Assigned", currentArtist.empty()))
                    {
                        metadata->artist = "";
                        metadataChanged = true;
                    }

                    if (users.empty())
                    {
                        ImGui::TextDisabled("(No users configured)");
                    }
                    else
                    {
                        for (const auto& user : users)
                        {
                            bool isSelected = (currentArtist == user.displayName);
                            if (ImGui::Selectable(user.displayName.c_str(), isSelected))
                            {
                                metadata->artist = user.displayName;
                                metadataChanged = true;
                            }

                            if (isSelected)
                                ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }

            // Priority column - Editable combo box
            if (m_visibleColumns["Priority"])
            {
                ImGui::TableNextColumn();

                const char* priorityText = (metadata->priority == 1) ? "High" :
                                          (metadata->priority == 2) ? "Medium" : "Low";
                ImVec4 priorityColor = (metadata->priority == 1) ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) :
                                      (metadata->priority == 2) ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f) :
                                                                 ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

                ImGui::PushStyleColor(ImGuiCol_Text, priorityColor);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(("##priority" + std::to_string(i)).c_str(), priorityText))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    if (ImGui::Selectable("High", metadata->priority == 1))
                    {
                        metadata->priority = 1;
                        metadataChanged = true;
                    }
                    ImGui::PopStyleColor();

                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
                    if (ImGui::Selectable("Medium", metadata->priority == 2))
                    {
                        metadata->priority = 2;
                        metadataChanged = true;
                    }
                    ImGui::PopStyleColor();

                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    if (ImGui::Selectable("Low", metadata->priority == 3))
                    {
                        metadata->priority = 3;
                        metadataChanged = true;
                    }
                    ImGui::PopStyleColor();

                    ImGui::EndCombo();
                }
                ImGui::PopStyleColor();
            }

            // DueDate column - Button to open date picker
            if (m_visibleColumns["DueDate"])
            {
                ImGui::TableNextColumn();

                std::string dateStr = (metadata->dueDate > 0) ? FormatTimestamp(metadata->dueDate) : "Not Set";
                ImGui::SetNextItemWidth(-FLT_MIN);

                if (ImGui::Button((dateStr + "##duedate" + std::to_string(i)).c_str(), ImVec2(-FLT_MIN, 0)))
                {
                    m_showDatePicker = true;
                    m_datePickerShotIndex = i;
                }

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Click to select date");
            }

            // Notes column - Text input
            if (m_visibleColumns["Notes"])
            {
                ImGui::TableNextColumn();

                char noteBuffer[256];
                strncpy(noteBuffer, metadata->note.c_str(), sizeof(noteBuffer) - 1);
                noteBuffer[sizeof(noteBuffer) - 1] = '\0';

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText(("##note" + std::to_string(i)).c_str(), noteBuffer, sizeof(noteBuffer)))
                {
                    metadata->note = noteBuffer;
                    metadataChanged = true;
                }
            }

            // Links column - Button (placeholder for now)
            if (m_visibleColumns["Links"])
            {
                ImGui::TableNextColumn();

                // Count links from JSON array (simplified for now)
                int linkCount = 0;
                if (!metadata->links.empty() && metadata->links != "[]")
                    linkCount = 1;  // Simplified

                ImGui::SetNextItemWidth(-FLT_MIN);
                char linkLabel[32];
                snprintf(linkLabel, sizeof(linkLabel), "Links (%d)##%d", linkCount, i);
                if (ImGui::SmallButton(linkLabel))
                {
                    // TODO: Open links dialog
                }
            }

            // Save metadata if changed
            if (metadataChanged && m_subscriptionManager)
            {
                // Update timestamps
                uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();

                if (metadata->id == 0)  // New metadata
                {
                    metadata->createdTime = now;
                }
                metadata->modifiedTime = now;

                // Save to database
                if (m_subscriptionManager->CreateOrUpdateShotMetadata(*metadata))
                {
                    // Update cache
                    m_shotMetadataMap[entry.fullPath] = *metadata;
                    std::cout << "[ShotView] Saved metadata for: " << UFB::WideToUtf8(entry.fullPath) << std::endl;
                }
                else
                {
                    std::cerr << "[ShotView] Failed to save metadata for: " << UFB::WideToUtf8(entry.fullPath) << std::endl;
                }
            }

            // Modified date column (always last)
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", FormatFileTime(entry.lastModified).c_str());

            // Pop mono font that was pushed for metadata columns
            if (font_mono)
                ImGui::PopFont();

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    // Pop the style variable for cell padding
    ImGui::PopStyleVar();

    // Date picker modal
    if (m_showDatePicker)
    {
        ImGui::OpenPopup("Select Due Date");
        m_showDatePicker = false;
    }

    if (ImGui::BeginPopupModal("Select Due Date", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // Reduce padding to make date picker more compact
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 4.0f));

        if (m_datePickerShotIndex >= 0 && m_datePickerShotIndex < m_shots.size())
        {
            const FileEntry& entry = m_shots[m_datePickerShotIndex];
            auto metadataIt = m_shotMetadataMap.find(entry.fullPath);

            if (metadataIt != m_shotMetadataMap.end())
            {
                UFB::ShotMetadata& metadata = metadataIt->second;

                // Convert current timestamp to tm struct (or use current date if not set)
                // Get current time in milliseconds if not set
                uint64_t currentTimeMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
                tm dateTime = metadata.dueDate > 0 ? TimestampToTm(metadata.dueDate) : TimestampToTm(currentTimeMillis);

                // Show date picker with reduced item spacing for compact layout
                if (ImGui::DatePicker("##datepicker", dateTime, false, 80.0f))
                {
                    // Date changed - convert tm to timestamp
                    metadata.dueDate = TmToTimestamp(dateTime);

                    // Save to database
                    if (m_subscriptionManager)
                    {
                        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()
                        ).count();

                        metadata.modifiedTime = now;

                        if (m_subscriptionManager->CreateOrUpdateShotMetadata(metadata))
                        {
                            m_shotMetadataMap[entry.fullPath] = metadata;
                            std::cout << "[ShotView] Updated due date for: " << UFB::WideToUtf8(entry.fullPath) << std::endl;
                        }
                    }
                }

                ImGui::Spacing();

                if (ImGui::Button("Clear Date", ImVec2(120, 0)))
                {
                    metadata.dueDate = 0;

                    // Save to database
                    if (m_subscriptionManager)
                    {
                        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()
                        ).count();

                        metadata.modifiedTime = now;

                        if (m_subscriptionManager->CreateOrUpdateShotMetadata(metadata))
                        {
                            m_shotMetadataMap[entry.fullPath] = metadata;
                            std::cout << "[ShotView] Cleared due date for: " << UFB::WideToUtf8(entry.fullPath) << std::endl;
                        }
                    }

                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine();

                if (ImGui::Button("Close", ImVec2(120, 0)))
                {
                    ImGui::CloseCurrentPopup();
                }
            }
        }

        // Pop style variables (3: FramePadding, ItemSpacing, CellPadding)
        ImGui::PopStyleVar(3);

        ImGui::EndPopup();
    }

    // Add new shot modal dialog
    if (m_showAddShotDialog)
    {
        ImGui::OpenPopup("Add New Shot");
        m_showAddShotDialog = false;
    }

    if (ImGui::BeginPopupModal("Add New Shot", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Enter shot name:");
        ImGui::Separator();

        ImGui::SetNextItemWidth(300);
        bool enterPressed = ImGui::InputText("##shotname", m_newShotNameBuffer, sizeof(m_newShotNameBuffer),
                                              ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Separator();

        if (ImGui::Button("Create", ImVec2(120, 0)) || enterPressed)
        {
            std::string shotName(m_newShotNameBuffer);
            if (!shotName.empty())
            {
                if (CreateNewShot(shotName))
                {
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    // Show error message (could be improved with a proper error display)
                    std::cerr << "[ShotView] Failed to create shot: " << shotName << std::endl;
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

    ImGui::EndChild();  // End nested child window for shots panel
}

void ShotView::DrawProjectsPanel(HWND hwnd)
{
    // Store window position and size
    m_projectsPanelPos = ImGui::GetWindowPos();
    m_projectsPanelSize = ImGui::GetWindowSize();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // Draw focus highlight border if this panel is focused
    if (isFocused)
    {
        ImVec4 accentColor = GetAccentColor();
        ImU32 highlightColor = ImGui::ColorConvertFloat4ToU32(ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.3f));

        float borderPadding = 4.0f;
        ImVec2 min = ImVec2(m_projectsPanelPos.x + borderPadding, m_projectsPanelPos.y + borderPadding);
        ImVec2 max = ImVec2(m_projectsPanelPos.x + m_projectsPanelSize.x - borderPadding, m_projectsPanelPos.y + m_projectsPanelSize.y - borderPadding);
        drawList->AddRect(min, max, highlightColor, 0.0f, 0, 3.0f);
    }

    // Create nested child window with padding to make room for the highlight border
    float contentPadding = 6.0f;
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    contentSize.x -= contentPadding * 2;
    contentSize.y -= contentPadding * 2;

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + contentPadding, ImGui::GetCursorPosY() + contentPadding));

    ImGui::BeginChild("##projects_content", contentSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::Text("Projects");

    // Refresh button
    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE5D5##projects")))  // Material Icons refresh
        {
            RefreshProjectFiles();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("R##projects"))
        {
            RefreshProjectFiles();
        }
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Refresh");

    ImGui::Separator();

    if (m_selectedShotPath.empty())
    {
        ImGui::TextDisabled("Select a shot to view projects");
        ImGui::EndChild();  // End nested child window before early return
        return;
    }

    if (ImGui::BeginTable("ProjectsTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 150.0f);
        ImGui::TableHeadersRow();

        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsDirty)
            {
                if (sortSpecs->SpecsCount > 0)
                {
                    m_projectSortSpecs = sortSpecs->Specs[0];

                    // Sort by column
                    std::sort(m_projectFiles.begin(), m_projectFiles.end(), [this](const FileEntry& a, const FileEntry& b) {
                        bool ascending = (m_projectSortSpecs.SortDirection == ImGuiSortDirection_Ascending);

                        if (m_projectSortSpecs.ColumnIndex == 0)  // Name
                        {
                            int cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                            return ascending ? (cmp < 0) : (cmp > 0);
                        }
                        else  // Modified
                        {
                            return ascending ? (a.lastModified < b.lastModified) : (a.lastModified > b.lastModified);
                        }
                    });
                }
                sortSpecs->SpecsDirty = false;
            }
        }

        for (int i = 0; i < m_projectFiles.size(); i++)
        {
            const FileEntry& entry = m_projectFiles[i];

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            ImGui::PushID(1000 + i);  // Offset to avoid ID conflicts

            // Get icon
            ImTextureID icon = m_iconManager.GetFileIcon(entry.fullPath, false);

            // Draw icon
            if (icon)
            {
                ImGui::Image(icon, ImVec2(16, 16));
                ImGui::SameLine();
            }

            // Convert name to UTF-8
            char nameUtf8[512];
            WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);

            bool isSelected = m_selectedProjectIndices.find(i) != m_selectedProjectIndices.end();
            if (ImGui::Selectable(nameUtf8, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                // Handle multi-select
                bool ctrlPressed = ImGui::GetIO().KeyCtrl;
                bool shiftPressed = ImGui::GetIO().KeyShift;

                if (ctrlPressed)
                {
                    // Ctrl+Click: toggle selection
                    if (isSelected)
                        m_selectedProjectIndices.erase(i);
                    else
                        m_selectedProjectIndices.insert(i);
                }
                else if (shiftPressed && m_lastClickedProjectIndex != -1)
                {
                    // Shift+Click: range select
                    int start = (std::min)(m_lastClickedProjectIndex, i);
                    int end = (std::max)(m_lastClickedProjectIndex, i);
                    for (int idx = start; idx <= end; ++idx)
                    {
                        m_selectedProjectIndices.insert(idx);
                    }
                }
                else
                {
                    // Normal click: select only this item
                    m_selectedProjectIndices.clear();
                    m_selectedProjectIndices.insert(i);

                    // Double-click detection
                    double currentTime = glfwGetTime();
                    if (m_lastClickedProjectIndex == i && (currentTime - m_lastProjectClickTime) < 0.3)
                    {
                        // Double-clicked - open file
                        ShellExecuteW(nullptr, L"open", entry.fullPath.c_str(), nullptr, nullptr, SW_SHOW);
                    }
                    m_lastProjectClickTime = currentTime;
                }

                m_lastClickedProjectIndex = i;
            }

            // Context menu
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                // If right-clicked item is not in selection, select only it
                if (!isSelected)
                {
                    m_selectedProjectIndices.clear();
                    m_selectedProjectIndices.insert(i);
                }
                ImGui::OpenPopup("project_context_menu");
            }

            ShowImGuiContextMenu(hwnd, entry, PanelType::Projects);

            // Modified column
            ImGui::TableNextColumn();
            if (font_mono)
                ImGui::PushFont(font_mono);
            ImGui::TextDisabled("%s", FormatFileTime(entry.lastModified).c_str());
            if (font_mono)
                ImGui::PopFont();

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();  // End nested child window for projects panel
}

void ShotView::DrawRendersPanel(HWND hwnd)
{
    // Store window position and size
    m_rendersPanelPos = ImGui::GetWindowPos();
    m_rendersPanelSize = ImGui::GetWindowSize();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // Draw focus highlight border if this panel is focused
    if (isFocused)
    {
        ImVec4 accentColor = GetAccentColor();
        ImU32 highlightColor = ImGui::ColorConvertFloat4ToU32(ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.3f));

        float borderPadding = 4.0f;
        ImVec2 min = ImVec2(m_rendersPanelPos.x + borderPadding, m_rendersPanelPos.y + borderPadding);
        ImVec2 max = ImVec2(m_rendersPanelPos.x + m_rendersPanelSize.x - borderPadding, m_rendersPanelPos.y + m_rendersPanelSize.y - borderPadding);
        drawList->AddRect(min, max, highlightColor, 0.0f, 0, 3.0f);
    }

    // Create nested child window with padding to make room for the highlight border
    float contentPadding = 6.0f;
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    contentSize.x -= contentPadding * 2;
    contentSize.y -= contentPadding * 2;

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + contentPadding, ImGui::GetCursorPosY() + contentPadding));

    ImGui::BeginChild("##renders_content", contentSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::Text("Renders / Outputs");

    // Navigation buttons
    bool canGoBack = !m_renderBackHistory.empty();
    bool canGoForward = !m_renderForwardHistory.empty();
    bool canGoUp = !m_renderCurrentDirectory.empty();

    // Back button
    if (!canGoBack)
        ImGui::BeginDisabled();

    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE5CB##renderBack")))  // Material Icons arrow_back
        {
            NavigateRenderBack();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("<##renderBack"))
        {
            NavigateRenderBack();
        }
    }

    if (!canGoBack)
        ImGui::EndDisabled();

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back");

    ImGui::SameLine();

    // Forward button
    if (!canGoForward)
        ImGui::BeginDisabled();

    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE5CC##renderForward")))  // Material Icons arrow_forward
        {
            NavigateRenderForward();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button(">##renderForward"))
        {
            NavigateRenderForward();
        }
    }

    if (!canGoForward)
        ImGui::EndDisabled();

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Forward");

    ImGui::SameLine();

    // Up button
    if (!canGoUp)
        ImGui::BeginDisabled();

    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE5CE##renderUp")))  // Material Icons arrow_upward
        {
            NavigateRenderUp();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("^##renderUp"))
        {
            NavigateRenderUp();
        }
    }

    if (!canGoUp)
        ImGui::EndDisabled();

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Up");

    ImGui::SameLine();

    // Refresh button
    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE5D5##renderRefresh")))  // Material Icons refresh
        {
            RefreshRenderFiles();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("Refresh##renderRefresh"))
        {
            RefreshRenderFiles();
        }
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Refresh");

    // Show current path
    if (!m_renderCurrentDirectory.empty())
    {
        ImGui::SameLine();
        std::string renderPathUtf8 = UFB::WideToUtf8(m_renderCurrentDirectory);

        // Use mono font for path
        if (font_mono)
            ImGui::PushFont(font_mono);

        ImGui::TextDisabled("%s", renderPathUtf8.c_str());

        if (font_mono)
            ImGui::PopFont();
    }

    ImGui::Separator();

    if (m_selectedShotPath.empty())
    {
        ImGui::TextDisabled("Select a shot to view renders");
        ImGui::EndChild();  // End nested child window before early return
        return;
    }

    if (ImGui::BeginTable("RendersTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 150.0f);
        ImGui::TableHeadersRow();

        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsDirty)
            {
                if (sortSpecs->SpecsCount > 0)
                {
                    m_renderSortSpecs = sortSpecs->Specs[0];

                    // Sort by column
                    std::sort(m_renderFiles.begin(), m_renderFiles.end(), [this](const FileEntry& a, const FileEntry& b) {
                        bool ascending = (m_renderSortSpecs.SortDirection == ImGuiSortDirection_Ascending);

                        if (m_renderSortSpecs.ColumnIndex == 0)  // Name
                        {
                            int cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                            return ascending ? (cmp < 0) : (cmp > 0);
                        }
                        else  // Modified
                        {
                            return ascending ? (a.lastModified < b.lastModified) : (a.lastModified > b.lastModified);
                        }
                    });
                }
                sortSpecs->SpecsDirty = false;
            }
        }

        for (int i = 0; i < m_renderFiles.size(); i++)
        {
            const FileEntry& entry = m_renderFiles[i];

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            ImGui::PushID(2000 + i);  // Offset to avoid ID conflicts

            // Get icon
            ImTextureID icon = m_iconManager.GetFileIcon(entry.fullPath, false);

            // Draw icon
            if (icon)
            {
                ImGui::Image(icon, ImVec2(16, 16));
                ImGui::SameLine();
            }

            // Convert name to UTF-8
            char nameUtf8[512];
            WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);

            bool isSelected = (m_selectedRenderIndices.find(i) != m_selectedRenderIndices.end());

            // Use accent color for selected items
            ImVec4 accentColor = GetAccentColor();
            if (isSelected)
            {
                ImGui::PushStyleColor(ImGuiCol_Header, accentColor);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accentColor.x * 1.1f, accentColor.y * 1.1f, accentColor.z * 1.1f, accentColor.w));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(accentColor.x * 1.2f, accentColor.y * 1.2f, accentColor.z * 1.2f, accentColor.w));
            }

            if (ImGui::Selectable(nameUtf8, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                // Handle multi-select with Ctrl/Shift
                if (ImGui::GetIO().KeyCtrl)
                {
                    // Ctrl+Click: toggle selection
                    if (isSelected)
                        m_selectedRenderIndices.erase(i);
                    else
                        m_selectedRenderIndices.insert(i);
                }
                else if (ImGui::GetIO().KeyShift && !m_selectedRenderIndices.empty())
                {
                    // Shift+Click: select range
                    int minIdx = *m_selectedRenderIndices.begin();
                    int maxIdx = *m_selectedRenderIndices.rbegin();
                    int rangeStart = (std::min)({i, minIdx, maxIdx});
                    int rangeEnd = (std::max)({i, minIdx, maxIdx});
                    m_selectedRenderIndices.clear();
                    for (int idx = rangeStart; idx <= rangeEnd; idx++)
                    {
                        m_selectedRenderIndices.insert(idx);
                    }
                }
                else
                {
                    // Regular click: clear selection and select this item
                    m_selectedRenderIndices.clear();
                    m_selectedRenderIndices.insert(i);
                }

                // Double-click detection
                double currentTime = glfwGetTime();
                if (m_lastClickedRenderIndex == i && (currentTime - m_lastClickTime) < 0.3)
                {
                    // Double-clicked
                    if (entry.isDirectory)
                    {
                        // Navigate into folder
                        NavigateToRenderDirectory(entry.fullPath);
                    }
                    else
                    {
                        // Open file
                        ShellExecuteW(nullptr, L"open", entry.fullPath.c_str(), nullptr, nullptr, SW_SHOW);
                    }
                }
                m_lastClickTime = currentTime;
                m_lastClickedRenderIndex = i;
            }

            if (isSelected)
            {
                ImGui::PopStyleColor(3);
            }

            // Drag source for file/folder (supports multi-select)
            static bool transitionedToOLEDrag = false;

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                if (!transitionedToOLEDrag)
                {
                    // Build list of file paths for drag operation
                    std::vector<std::wstring> filePaths;

                    // If this item is part of selection, drag all selected items
                    if (m_selectedRenderIndices.find(i) != m_selectedRenderIndices.end())
                    {
                        for (int idx : m_selectedRenderIndices)
                        {
                            if (idx < m_renderFiles.size())
                            {
                                filePaths.push_back(m_renderFiles[idx].fullPath);
                            }
                        }
                    }
                    else
                    {
                        // Not selected - drag just this one
                        filePaths.push_back(entry.fullPath);
                    }

                    // Check if mouse has left the main window (HWND) - if so, start Windows OLE drag
                    POINT cursorPos;
                    GetCursorPos(&cursorPos);

                    RECT windowRect;
                    GetWindowRect(hwnd, &windowRect);

                    bool mouseOutsideHWND = !PtInRect(&windowRect, cursorPos);

                    if (mouseOutsideHWND && !filePaths.empty())
                    {
                        transitionedToOLEDrag = true;
                        ImGui::EndDragDropSource();

                        // Start Windows native drag and drop
                        StartWindowsDragDrop(filePaths);
                    }
                    else
                    {
                        // Show drag preview
                        char dragPreviewUtf8[512];
                        if (filePaths.size() > 1)
                        {
                            snprintf(dragPreviewUtf8, sizeof(dragPreviewUtf8), "%zu files", filePaths.size());
                        }
                        else
                        {
                            WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, dragPreviewUtf8, sizeof(dragPreviewUtf8), nullptr, nullptr);
                        }
                        ImGui::Text("%s", dragPreviewUtf8);

                        ImGui::EndDragDropSource();
                    }
                }
                else
                {
                    ImGui::EndDragDropSource();
                }
            }

            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                transitionedToOLEDrag = false;
            }

            // Context menu
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                // If right-clicked item is not in selection, select only it
                if (!isSelected)
                {
                    m_selectedRenderIndices.clear();
                    m_selectedRenderIndices.insert(i);
                }
                ImGui::OpenPopup("render_context_menu");
            }

            ShowImGuiContextMenu(hwnd, entry, PanelType::Renders);

            // Modified column
            ImGui::TableNextColumn();
            if (font_mono)
                ImGui::PushFont(font_mono);
            ImGui::TextDisabled("%s", FormatFileTime(entry.lastModified).c_str());
            if (font_mono)
                ImGui::PopFont();

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();  // End nested child window for renders panel
}

void ShotView::ShowImGuiContextMenu(HWND hwnd, const FileEntry& entry, PanelType panelType)
{
    // Use different popup IDs based on panel type
    const char* popupId = nullptr;
    switch (panelType)
    {
        case PanelType::Shots:
            popupId = "shot_context_menu";
            break;
        case PanelType::Projects:
            popupId = "project_context_menu";
            break;
        case PanelType::Renders:
            popupId = "render_context_menu";
            break;
    }

    if (ImGui::BeginPopup(popupId))
    {
        // Convert filename to UTF-8
        char nameUtf8[512];
        WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);

        // Header with filename
        int selectionCount = 0;
        if (panelType == PanelType::Projects)
            selectionCount = static_cast<int>(m_selectedProjectIndices.size());
        else if (panelType == PanelType::Renders)
            selectionCount = static_cast<int>(m_selectedRenderIndices.size());

        if (selectionCount > 1)
        {
            ImGui::TextDisabled("%d items selected", selectionCount);
        }
        else
        {
            ImGui::TextDisabled("%s", nameUtf8);
        }
        ImGui::Separator();

        // Copy (file operation) - supports multi-select
        if (ImGui::MenuItem("Copy"))
        {
            std::vector<std::wstring> paths;

            if (panelType == PanelType::Projects && !m_selectedProjectIndices.empty())
            {
                // Copy all selected project files
                for (int idx : m_selectedProjectIndices)
                {
                    if (idx >= 0 && idx < m_projectFiles.size())
                    {
                        paths.push_back(m_projectFiles[idx].fullPath);
                    }
                }
            }
            else if (panelType == PanelType::Renders && !m_selectedRenderIndices.empty())
            {
                // Copy all selected render files
                for (int idx : m_selectedRenderIndices)
                {
                    if (idx >= 0 && idx < m_renderFiles.size())
                    {
                        paths.push_back(m_renderFiles[idx].fullPath);
                    }
                }
            }
            else
            {
                // Fallback: copy just this file
                paths.push_back(entry.fullPath);
            }

            CopyFilesToClipboard(paths);
        }

        // Cut (file operation) - supports multi-select
        if (ImGui::MenuItem("Cut"))
        {
            std::vector<std::wstring> paths;

            if (panelType == PanelType::Projects && !m_selectedProjectIndices.empty())
            {
                // Cut all selected project files
                for (int idx : m_selectedProjectIndices)
                {
                    if (idx >= 0 && idx < m_projectFiles.size())
                    {
                        paths.push_back(m_projectFiles[idx].fullPath);
                    }
                }
            }
            else if (panelType == PanelType::Renders && !m_selectedRenderIndices.empty())
            {
                // Cut all selected render files
                for (int idx : m_selectedRenderIndices)
                {
                    if (idx >= 0 && idx < m_renderFiles.size())
                    {
                        paths.push_back(m_renderFiles[idx].fullPath);
                    }
                }
            }
            else
            {
                // Fallback: cut just this file
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
            if (ImGui::MenuItem("Open in the Left Browser"))
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
            if (ImGui::MenuItem("Open in the Right Browser"))
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
                    // Collect all selected video files
                    std::vector<std::wstring> selectedVideos;

                    std::wcout << L"[ShotView] Transcode menu clicked" << std::endl;
                    std::wcout << L"  panelType: " << (int)panelType << std::endl;
                    std::wcout << L"  m_selectedProjectIndices.size(): " << m_selectedProjectIndices.size() << std::endl;
                    std::wcout << L"  m_selectedRenderIndices.size(): " << m_selectedRenderIndices.size() << std::endl;

                    if (panelType == PanelType::Projects && !m_selectedProjectIndices.empty())
                    {
                        std::wcout << L"  Using selected project files" << std::endl;
                        // Get videos from selected project files
                        for (int idx : m_selectedProjectIndices)
                        {
                            if (idx >= 0 && idx < m_projectFiles.size())
                            {
                                const auto& file = m_projectFiles[idx];
                                if (!file.isDirectory)
                                {
                                    std::filesystem::path fp(file.fullPath);
                                    std::wstring fileExt = fp.extension().wstring();
                                    std::transform(fileExt.begin(), fileExt.end(), fileExt.begin(), ::towlower);
                                    if (videoExtensions.find(fileExt) != videoExtensions.end())
                                    {
                                        selectedVideos.push_back(file.fullPath);
                                        std::wcout << L"    Added: " << file.fullPath << std::endl;
                                    }
                                }
                            }
                        }
                    }
                    else if (panelType == PanelType::Renders && !m_selectedRenderIndices.empty())
                    {
                        std::wcout << L"  Using selected render files" << std::endl;
                        // Get videos from selected render files
                        for (int idx : m_selectedRenderIndices)
                        {
                            if (idx >= 0 && idx < m_renderFiles.size())
                            {
                                const auto& file = m_renderFiles[idx];
                                if (!file.isDirectory)
                                {
                                    std::filesystem::path fp(file.fullPath);
                                    std::wstring fileExt = fp.extension().wstring();
                                    std::transform(fileExt.begin(), fileExt.end(), fileExt.begin(), ::towlower);
                                    if (videoExtensions.find(fileExt) != videoExtensions.end())
                                    {
                                        selectedVideos.push_back(file.fullPath);
                                        std::wcout << L"    Added: " << file.fullPath << std::endl;
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        std::wcout << L"  Using single clicked item (fallback)" << std::endl;
                        // No selection or single-click without selection - just use clicked item
                        selectedVideos.push_back(entry.fullPath);
                        std::wcout << L"    Added: " << entry.fullPath << std::endl;
                    }

                    std::wcout << L"  Total videos to transcode: " << selectedVideos.size() << std::endl;
                    if (!selectedVideos.empty())
                    {
                        onTranscodeToMP4(selectedVideos);
                    }
                    ImGui::CloseCurrentPopup();
                }

                ImGui::PopStyleColor();
            }
        }

        // Submit to Deadline (only for .blend files in Projects panel)
        if (!entry.isDirectory && panelType == PanelType::Projects)
        {
            // Check if this is a .blend file
            std::filesystem::path filePath(entry.fullPath);
            std::wstring ext = filePath.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

            if (ext == L".blend" && onSubmitToDeadline)
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

                if (ImGui::MenuItem("Submit to Deadline"))
                {
                    // Get job name from shot name
                    std::wstring jobName = GetJobName() + L" - " + m_categoryName;
                    if (m_selectedShotIndex >= 0 && m_selectedShotIndex < m_shots.size())
                    {
                        jobName = jobName + L" - " + m_shots[m_selectedShotIndex].name;
                    }

                    // Call callback with single blend file
                    if (onSubmitToDeadline)
                    {
                        onSubmitToDeadline(entry.fullPath, jobName);
                    }

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

        // Track/Untrack menu item (only for Shots panel)
        if (panelType == PanelType::Shots && m_subscriptionManager)
        {
            // Get current metadata
            auto metadata = m_subscriptionManager->GetShotMetadata(entry.fullPath);
            bool isTracked = metadata.has_value() && metadata->isTracked;

            // Use bright accent color for menu item
            ImVec4 accentColor = GetAccentColor();
            ImVec4 brightAccent = ImVec4(accentColor.x * 1.3f, accentColor.y * 1.3f, accentColor.z * 1.3f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, brightAccent);

            const char* menuLabel = isTracked ? "Untrack Shot" : "Track Shot";
            if (ImGui::MenuItem(menuLabel))
            {
                // Create or update metadata
                UFB::ShotMetadata shotMeta;
                if (metadata.has_value())
                {
                    shotMeta = *metadata;
                }
                else
                {
                    shotMeta.shotPath = entry.fullPath;
                    shotMeta.itemType = "shot";
                    shotMeta.folderType = UFB::WideToUtf8(m_categoryName);
                    shotMeta.isTracked = false;
                }

                // Toggle tracking status
                shotMeta.isTracked = !isTracked;
                shotMeta.modifiedTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();

                // Save to database
                m_subscriptionManager->CreateOrUpdateShotMetadata(shotMeta);

                // Update local map
                m_shotMetadataMap[entry.fullPath] = shotMeta;
            }

            ImGui::PopStyleColor();
        }

        // Copy ufb:/// link
        {
            ImVec4 accentColor = GetAccentColor();
            ImVec4 brightAccent = ImVec4(
                accentColor.x * 1.3f,
                accentColor.y * 1.3f,
                accentColor.z * 1.3f,
                1.0f
            );
            ImGui::PushStyleColor(ImGuiCol_Text, brightAccent);

            if (ImGui::MenuItem("Copy ufb:/// link"))
            {
                // Generate URI for this path
                std::string uri = UFB::BuildPathURI(entry.fullPath);

                // Copy to clipboard
                ImGui::SetClipboardText(uri.c_str());

                std::wcout << L"[ShotView] Copied ufb:/// link to clipboard: "
                           << UFB::Utf8ToWide(uri) << std::endl;

                ImGui::CloseCurrentPopup();
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

            if (panelType == PanelType::Projects && !m_selectedProjectIndices.empty())
            {
                // Delete all selected project files
                for (int idx : m_selectedProjectIndices)
                {
                    if (idx >= 0 && idx < m_projectFiles.size())
                    {
                        paths.push_back(m_projectFiles[idx].fullPath);
                    }
                }
            }
            else if (panelType == PanelType::Renders && !m_selectedRenderIndices.empty())
            {
                // Delete all selected render files
                for (int idx : m_selectedRenderIndices)
                {
                    if (idx >= 0 && idx < m_renderFiles.size())
                    {
                        paths.push_back(m_renderFiles[idx].fullPath);
                    }
                }
            }
            else
            {
                // Fallback: delete just this file
                paths.push_back(entry.fullPath);
            }

            DeleteFilesToRecycleBin(paths);
        }

        ImGui::EndPopup();
    }
}

// Helper functions
void ShotView::CopyToClipboard(const std::wstring& text)
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

void ShotView::CopyFilesToClipboard(const std::vector<std::wstring>& paths)
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

void ShotView::CutFilesToClipboard(const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return;

    CopyFilesToClipboard(paths);
    m_cutFiles = paths;
}

void ShotView::PasteFilesFromClipboard()
{
    // Paste into the selected shot's folder if available, otherwise into category root
    std::wstring targetDir = m_selectedShotPath.empty() ? m_categoryPath : m_selectedShotPath;

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
        RefreshShots();
        if (!m_selectedShotPath.empty())
        {
            RefreshProjectFiles();
            RefreshRenderFiles();
        }
    }

    CloseClipboard();
}

void ShotView::RevealInExplorer(const std::wstring& path)
{
    std::wstring command = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", command.c_str(), nullptr, SW_SHOW);
}

void ShotView::DeleteFilesToRecycleBin(const std::vector<std::wstring>& paths)
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
        RefreshShots();
        if (!m_selectedShotPath.empty())
        {
            RefreshProjectFiles();
            RefreshRenderFiles();
        }
    }
}

void ShotView::ShowContextMenu(HWND hwnd, const std::wstring& path, const ImVec2& screenPos)
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

std::string ShotView::FormatFileSize(uintmax_t size)
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

std::string ShotView::FormatFileTime(const std::filesystem::file_time_type& ftime)
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

ImVec4 ShotView::GetAccentColor()
{
    // Use the global accent color function with transparency
    ImVec4 accent = GetWindowsAccentColor();
    accent.w = 0.3f;
    return accent;
}

void ShotView::LoadMetadata()
{
    if (!m_subscriptionManager)
        return;

    // Get category name as UTF-8 for folderType lookup
    std::string folderType = UFB::WideToUtf8(m_categoryName);

    // Get job path (parent of category path)
    std::wstring jobPath = std::filesystem::path(m_categoryPath).parent_path().wstring();

    // Load all shot metadata for this category from database
    auto allMetadata = m_subscriptionManager->GetShotMetadataByType(jobPath, folderType);

    // Map by shot path for quick lookup
    for (const auto& metadata : allMetadata)
    {
        m_shotMetadataMap[metadata.shotPath] = metadata;
    }

    // Collect available filter values from metadata
    CollectAvailableFilterValues();
}

void ShotView::ReloadMetadata()
{
    // Reload metadata from database (called by observer when remote changes arrive)
    LoadMetadata();

    // Note: We don't need to refresh shots here - just updating the metadata map
    // The UI will pick up the new metadata on next frame
    std::wcout << L"[ShotView] Metadata reloaded successfully" << std::endl;
}

void ShotView::LoadColumnVisibility()
{
    m_visibleColumns.clear();

    std::string folderType = UFB::WideToUtf8(m_categoryName);
    std::wcout << L"[ShotView] LoadColumnVisibility called for: " << m_categoryName << L" (folder type: " << folderType.c_str() << L")" << std::endl;

    // Get displayMetadata for this folder type from projectConfig
    std::map<std::string, bool> displayMetadata;
    if (m_projectConfig && m_projectConfig->IsLoaded())
    {
        displayMetadata = m_projectConfig->GetDisplayMetadata(folderType);
        std::cout << "[ShotView] Loaded display metadata for folder type: " << folderType << std::endl;
        std::cout << "[ShotView] Display metadata size: " << displayMetadata.size() << std::endl;

        // Debug: Show all available folder types in config
        auto allTypes = m_projectConfig->GetAllFolderTypes();
        std::cout << "[ShotView] Available folder types in ProjectConfig (" << allTypes.size() << " total):" << std::endl;
        for (const auto& type : allTypes)
        {
            std::cout << "  - \"" << type << "\"" << std::endl;
        }
        std::cout << "[ShotView] Requested folder type: \"" << folderType << "\"" << std::endl;

        // If displayMetadata is empty, the project config doesn't have it - use hardcoded defaults
        if (displayMetadata.empty())
        {
            std::cout << "[ShotView] WARNING: displayMetadata is empty for '" << folderType << "', using hardcoded defaults" << std::endl;
        }
    }
    else
    {
        std::cerr << "[ShotView] WARNING: ProjectConfig not loaded! Using hardcoded defaults" << std::endl;
    }

    // If displayMetadata is empty (either from config not loaded or empty in config), use hardcoded defaults
    if (displayMetadata.empty())
    {
        std::cout << "[ShotView] Applying hardcoded fallback defaults for folder type: " << folderType << std::endl;

        // Hardcoded fallback defaults for common folder types
        if (folderType == "ae")
        {
            displayMetadata["Status"] = true;
            displayMetadata["Category"] = true;
            displayMetadata["Artist"] = true;
            displayMetadata["Priority"] = true;
            displayMetadata["DueDate"] = true;
        }
        else if (folderType == "3d")
        {
            displayMetadata["Status"] = true;
            displayMetadata["Artist"] = true;
            displayMetadata["DueDate"] = true;
        }
        else
        {
            // Default: show Status and Artist for any other type
            displayMetadata["Status"] = true;
            displayMetadata["Artist"] = true;
        }
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
    std::cout << "[ShotView] Column visibility for " << folderType << ":" << std::endl;
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

void ShotView::SaveColumnVisibility()
{
    if (!m_projectConfig)
    {
        std::cerr << "[ShotView] Cannot save column visibility: ProjectConfig is null" << std::endl;
        return;
    }

    if (!m_projectConfig->IsLoaded())
    {
        std::cerr << "[ShotView] Cannot save column visibility: ProjectConfig not loaded" << std::endl;
        return;
    }

    std::string folderType = UFB::WideToUtf8(m_categoryName);

    std::cout << "[ShotView] Saving column visibility for folder type: " << folderType << std::endl;

    // Log what we're trying to save
    for (const auto& [col, visible] : m_visibleColumns)
    {
        std::cout << "  " << col << ": " << (visible ? "true" : "false") << std::endl;
    }

    try
    {
        // Save to projectConfig (which will persist to disk)
        m_projectConfig->SetDisplayMetadata(folderType, m_visibleColumns);
        std::cout << "[ShotView] Column visibility saved successfully" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ShotView] Error saving column visibility: " << e.what() << std::endl;
    }
}

ImVec4 ShotView::GetStatusColor(const std::string& status)
{
    if (!m_projectConfig)
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White default

    std::string folderType = UFB::WideToUtf8(m_categoryName);
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

ImVec4 ShotView::GetCategoryColor(const std::string& category)
{
    if (!m_projectConfig)
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White default

    std::string folderType = UFB::WideToUtf8(m_categoryName);
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

std::string ShotView::FormatTimestamp(uint64_t timestamp)
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

bool ShotView::CreateNewShot(const std::string& shotName)
{
    if (!m_projectConfig)
    {
        std::cerr << "[ShotView] No project config available" << std::endl;
        return false;
    }

    // Get folder type config for this category
    std::string categoryNameUtf8 = UFB::WideToUtf8(m_categoryName);
    auto folderTypeConfig = m_projectConfig->GetFolderTypeConfig(categoryNameUtf8);

    if (!folderTypeConfig)
    {
        std::cerr << "[ShotView] No folder type config for category: " << categoryNameUtf8 << std::endl;
        return false;
    }

    // Check if this folder type has a template
    if (folderTypeConfig->addActionTemplate.empty())
    {
        std::cerr << "[ShotView] No template configured for category: " << categoryNameUtf8 << std::endl;
        return false;
    }

    try
    {
        // Get the executable directory (where assets folder should be)
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        std::filesystem::path templatePath = exeDir / "assets" / folderTypeConfig->addActionTemplate;

        if (!std::filesystem::exists(templatePath))
        {
            std::cerr << "[ShotView] Template not found: " << templatePath << std::endl;
            return false;
        }

        // Create destination path: category_path / shot_name
        std::wstring shotNameWide = UFB::Utf8ToWide(shotName);
        std::filesystem::path destPath = std::filesystem::path(m_categoryPath) / shotNameWide;

        if (std::filesystem::exists(destPath))
        {
            std::cerr << "[ShotView] Shot already exists: " << shotName << std::endl;
            return false;
        }

        // Copy template folder recursively
        std::filesystem::copy(templatePath, destPath, std::filesystem::copy_options::recursive);
        std::cout << "[ShotView] Copied template from " << templatePath << " to " << destPath << std::endl;

        // Rename the template file if specified
        if (!folderTypeConfig->addActionTemplateFile.empty())
        {
            std::filesystem::path templateFilePath = exeDir / "assets" / folderTypeConfig->addActionTemplateFile;
            std::filesystem::path templateFileName = templateFilePath.filename();

            // The file should now be in destPath with the same relative structure
            std::filesystem::path relativeToTemplate = std::filesystem::relative(templateFilePath, templatePath);
            std::filesystem::path copiedFilePath = destPath / relativeToTemplate;

            if (std::filesystem::exists(copiedFilePath))
            {
                // Extract file extension
                std::string ext = copiedFilePath.extension().string();

                // Create new filename: {ShotName}_v001.{ext}
                std::string newFileName = shotName + "_v001" + ext;
                std::filesystem::path newFilePath = copiedFilePath.parent_path() / newFileName;

                // Rename the file
                std::filesystem::rename(copiedFilePath, newFilePath);
                std::cout << "[ShotView] Renamed " << copiedFilePath.filename() << " to " << newFileName << std::endl;
            }
            else
            {
                std::cerr << "[ShotView] Warning: Template file not found at expected location: " << copiedFilePath << std::endl;
            }
        }

        // Refresh the shots list
        RefreshShots();

        // Select the newly created shot
        for (size_t i = 0; i < m_shots.size(); i++)
        {
            if (m_shots[i].fullPath == destPath.wstring())
            {
                m_selectedShotIndex = static_cast<int>(i);
                m_selectedShotPath = destPath.wstring();
                RefreshProjectFiles();
                RefreshRenderFiles();
                break;
            }
        }

        std::cout << "[ShotView] Successfully created shot: " << shotName << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ShotView] Error creating shot: " << e.what() << std::endl;
        return false;
    }
}

void ShotView::CollectAvailableFilterValues()
{
    // Clear previous values
    m_availableStatuses.clear();
    m_availableCategories.clear();
    m_availableArtists.clear();
    m_availablePriorities.clear();

    if (!m_projectConfig || !m_projectConfig->IsLoaded())
        return;

    // Get folder type (category name as UTF-8)
    std::string folderType = UFB::WideToUtf8(m_categoryName);

    // Get status options from ProjectConfig
    auto statusOptions = m_projectConfig->GetStatusOptions(folderType);
    for (const auto& statusOpt : statusOptions)
    {
        m_availableStatuses.insert(statusOpt.name);
    }

    // Get category options from ProjectConfig
    auto categoryOptions = m_projectConfig->GetCategoryOptions(folderType);
    for (const auto& categoryOpt : categoryOptions)
    {
        m_availableCategories.insert(categoryOpt.name);
    }

    // Get users (artists) from ProjectConfig
    auto users = m_projectConfig->GetUsers();
    for (const auto& user : users)
    {
        m_availableArtists.insert(user.displayName);
    }

    // Get priority options from ProjectConfig
    auto priorityOptions = m_projectConfig->GetPriorityOptions();
    for (int priority : priorityOptions)
    {
        m_availablePriorities.insert(priority);
    }
}

bool ShotView::PassesFilters(const FileEntry& entry)
{
    // Get metadata for this shot
    auto it = m_shotMetadataMap.find(entry.fullPath);
    if (it == m_shotMetadataMap.end())
    {
        // No metadata - only show if all filters are empty (showing all)
        return m_filterStatuses.empty() && m_filterCategories.empty() &&
               m_filterArtists.empty() && m_filterPriorities.empty() && m_filterDueDate == 0;
    }

    const UFB::ShotMetadata& metadata = it->second;

    // Check status filter
    if (!m_filterStatuses.empty())
    {
        if (m_filterStatuses.find(metadata.status) == m_filterStatuses.end())
            return false;
    }

    // Check category filter
    if (!m_filterCategories.empty())
    {
        if (m_filterCategories.find(metadata.category) == m_filterCategories.end())
            return false;
    }

    // Check artist filter
    if (!m_filterArtists.empty())
    {
        if (m_filterArtists.find(metadata.artist) == m_filterArtists.end())
            return false;
    }

    // Check priority filter
    if (!m_filterPriorities.empty())
    {
        if (m_filterPriorities.find(metadata.priority) == m_filterPriorities.end())
            return false;
    }

    // Check due date filter
    if (m_filterDueDate != 0 && metadata.dueDate > 0)
    {
        auto now = std::chrono::system_clock::now();
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

        auto dueDateMs = metadata.dueDate;
        auto diff = dueDateMs - nowMs;

        bool passes = false;
        switch (m_filterDueDate)
        {
            case 1: // Today
            {
                // Due date is within next 24 hours
                passes = (diff >= 0 && diff < 24 * 60 * 60 * 1000);
                break;
            }
            case 2: // Yesterday
            {
                // Due date was within last 24 hours
                passes = (diff < 0 && diff > -24 * 60 * 60 * 1000);
                break;
            }
            case 3: // Last 7 days
            {
                // Due date is within next 7 days
                passes = (diff >= 0 && diff < 7 * 24 * 60 * 60 * 1000);
                break;
            }
            case 4: // Last 30 days
            {
                // Due date is within next 30 days
                passes = (diff >= 0 && diff < 30 * 24 * 60 * 60 * 1000);
                break;
            }
            case 5: // This year
            {
                // Due date is within this calendar year
                auto dueTime = std::chrono::system_clock::time_point(std::chrono::milliseconds(dueDateMs));
                std::time_t dueTimeT = std::chrono::system_clock::to_time_t(dueTime);
                std::time_t nowTimeT = std::chrono::system_clock::to_time_t(now);

                std::tm dueTm;
                std::tm nowTm;
                localtime_s(&dueTm, &dueTimeT);
                localtime_s(&nowTm, &nowTimeT);

                passes = (dueTm.tm_year == nowTm.tm_year);
                break;
            }
        }

        if (!passes)
            return false;
    }

    return true;
}
