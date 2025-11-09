#include "file_browser.h"
#include "extractors/windows_shell_extractor.h"
#include "extractors/image_thumbnail_extractor.h"
#include "extractors/svg_thumbnail_extractor.h"
#include "extractors/blend_thumbnail_extractor.h"
#include "extractors/video_thumbnail_extractor.h"
#include "extractors/psd_ai_thumbnail_extractor.h"
#include "extractors/exr_extractor.h"
#include "extractors/fallback_icon_extractor.h"
#include "ole_drag_drop.h"
#include "bookmark_manager.h"
#include "subscription_manager.h"
#include "utils.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <set>
#include <sstream>
#include <iomanip>

// C++20 changed u8"" literals to char8_t, need to cast for ImGui
#define U8(x) reinterpret_cast<const char*>(u8##x)
#include <iostream>
#include <ctime>
#include <chrono>
#include <GLFW/glfw3.h>

// Material Icons for drag tooltips
#define ICON_FOLDER U8("\uE2C7")  // folder
#define ICON_FILE U8("\uE873")    // description

// External font references
extern ImFont* font_regular;
extern ImFont* font_mono;
extern ImFont* font_icons;

// External accent color function
extern ImVec4 GetWindowsAccentColor();

// Static member definitions - shared across all FileBrowser instances
std::vector<std::wstring> FileBrowser::m_cutFiles;
bool FileBrowser::showHiddenFiles = false;
int FileBrowser::m_oleRefCount = 0;

FileBrowser::FileBrowser()
{
    Initialize();
}

FileBrowser::~FileBrowser()
{
    Shutdown();
}

void FileBrowser::Initialize(UFB::BookmarkManager* bookmarkManager, UFB::SubscriptionManager* subscriptionManager)
{
    // Store manager dependencies
    m_bookmarkManager = bookmarkManager;
    m_subscriptionManager = subscriptionManager;

    // Initialize OLE for drag and drop support (only once per thread)
    if (m_oleRefCount == 0)
    {
        HRESULT hr = OleInitialize(nullptr);
        if (FAILED(hr))
        {
            std::cerr << "[FileBrowser] Failed to initialize OLE, hr=0x" << std::hex << hr << std::endl;
        }
        else
        {
            std::cout << "[FileBrowser] OLE initialized" << std::endl;
        }
    }
    m_oleRefCount++;

    m_iconManager.Initialize();

    // Initialize thumbnail manager with 4 worker threads
    m_thumbnailManager.Initialize(4);

    // Register thumbnail extractors (sorted by priority)
    m_thumbnailManager.RegisterExtractor(std::make_unique<WindowsShellExtractor>());
    m_thumbnailManager.RegisterExtractor(std::make_unique<EXRExtractor>());
    m_thumbnailManager.RegisterExtractor(std::make_unique<ImageThumbnailExtractor>());
    m_thumbnailManager.RegisterExtractor(std::make_unique<SvgThumbnailExtractor>());
    m_thumbnailManager.RegisterExtractor(std::make_unique<BlendThumbnailExtractor>());
    m_thumbnailManager.RegisterExtractor(std::make_unique<PsdAiThumbnailExtractor>());
    m_thumbnailManager.RegisterExtractor(std::make_unique<VideoThumbnailExtractor>());
    m_thumbnailManager.RegisterExtractor(std::make_unique<FallbackIconExtractor>(&m_iconManager));

    // Get special folder paths
    wchar_t pathBuffer[MAX_PATH];

    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, pathBuffer)))
    {
        m_desktopPath = pathBuffer;
        m_desktopIcon = m_iconManager.GetFileIcon(m_desktopPath, true, 16);
    }

    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0, pathBuffer)))
    {
        m_documentsPath = pathBuffer;
        m_documentsIcon = m_iconManager.GetFileIcon(m_documentsPath, true, 16);
    }

    // Downloads folder (Vista and later)
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, pathBuffer)))
    {
        m_downloadsPath = std::wstring(pathBuffer) + L"\\Downloads";
        m_downloadsIcon = m_iconManager.GetFileIcon(m_downloadsPath, true, 16);
    }

    // Start with Desktop as default
    if (!m_desktopPath.empty())
    {
        SetCurrentDirectory(m_desktopPath);
    }
    else
    {
        SetCurrentDirectory(L"C:\\");
    }
}

void FileBrowser::Shutdown()
{
    // Shutdown thumbnail manager first (stops worker threads)
    m_thumbnailManager.Shutdown();

    m_iconManager.Shutdown();

    // Uninitialize OLE only when the last instance is destroyed
    m_oleRefCount--;
    if (m_oleRefCount == 0)
    {
        OleUninitialize();
        std::cout << "[FileBrowser] OLE uninitialized" << std::endl;
    }
}

void FileBrowser::Draw(const char* title, HWND hwnd, bool withWindow)
{
    // Push unique ID for this FileBrowser instance to avoid popup conflicts
    ImGui::PushID(this);

    if (withWindow)
        ImGui::Begin(title, &m_isOpen);

    // Track hover state and window bounds for external drag-drop
    m_isHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    m_windowPos = ImGui::GetWindowPos();
    m_windowSize = ImGui::GetWindowSize();

    // Draw visual highlight when hovered (for drag-drop feedback)
    if (m_isHovered)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec4 accentColor = GetAccentColor();
        ImU32 highlightColor = ImGui::ColorConvertFloat4ToU32(ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.3f));

        // Draw border overlay with padding
        float borderPadding = 4.0f;
        ImVec2 min = ImVec2(m_windowPos.x + borderPadding, m_windowPos.y + borderPadding);
        ImVec2 max = ImVec2(m_windowPos.x + m_windowSize.x - borderPadding, m_windowPos.y + m_windowSize.y - borderPadding);
        drawList->AddRect(min, max, highlightColor, 0.0f, 0, 3.0f);
    }

    // Create nested child window with padding to make room for the highlight border
    float contentPadding = 6.0f;
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    contentSize.x -= contentPadding * 2;
    contentSize.y -= contentPadding * 2;

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + contentPadding, ImGui::GetCursorPosY() + contentPadding));

    ImGui::BeginChild("##browser_content", contentSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    DrawNavigationBar();
    ImGui::Separator();
    DrawFileList(hwnd);

    ImGui::EndChild();

    if (withWindow)
        ImGui::End();

    // Pop unique ID for this FileBrowser instance
    ImGui::PopID();
}

void FileBrowser::SetCurrentDirectory(const std::wstring& path)
{
    try
    {
        if (std::filesystem::exists(path) && std::filesystem::is_directory(path))
        {
            m_currentDirectory = std::filesystem::canonical(path).wstring();

            // Clear pending thumbnail work before changing directory
            m_thumbnailManager.ClearPendingRequests();
            m_thumbnailManager.ClearCache();

            RefreshFileList();
            m_selectedIndices.clear();  // Clear selection when changing directories
        }
    }
    catch (const std::exception&)
    {
        // Failed to set directory
    }
}

void FileBrowser::SetCurrentDirectoryAndSelectFile(const std::wstring& directoryPath, const std::wstring& filePathToSelect)
{
    try
    {
        if (std::filesystem::exists(directoryPath) && std::filesystem::is_directory(directoryPath))
        {
            m_currentDirectory = std::filesystem::canonical(directoryPath).wstring();

            // Clear pending thumbnail work before changing directory
            m_thumbnailManager.ClearPendingRequests();
            m_thumbnailManager.ClearCache();

            RefreshFileList();
            m_selectedIndices.clear();

            // Find and select the specified file
            std::wstring canonicalFilePathToSelect;
            try {
                canonicalFilePathToSelect = std::filesystem::canonical(filePathToSelect).wstring();
            } catch (...) {
                // If canonical fails (file doesn't exist), just use the path as-is
                canonicalFilePathToSelect = filePathToSelect;
            }

            for (size_t i = 0; i < m_files.size(); i++)
            {
                if (m_files[i].fullPath == canonicalFilePathToSelect)
                {
                    m_selectedIndices.insert(static_cast<int>(i));
                    std::wcout << L"[FileBrowser] Selected file: " << m_files[i].name << std::endl;
                    break;
                }
            }
        }
    }
    catch (const std::exception&)
    {
        // Failed to set directory and select file
    }
}

void FileBrowser::RefreshFileList()
{
    std::lock_guard<std::mutex> lock(m_filesMutex);

    m_files.clear();

    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(m_currentDirectory))
        {
            // Skip hidden files if option is disabled
            if (!showHiddenFiles)
            {
                auto filename = entry.path().filename().wstring();
                if (!filename.empty() && filename[0] == L'.')
                    continue;

                DWORD attrs = GetFileAttributesW(entry.path().c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN))
                    continue;
            }

            FileEntry fileEntry;
            fileEntry.name = entry.path().filename().wstring();
            fileEntry.fullPath = entry.path().wstring();
            fileEntry.isDirectory = entry.is_directory();

            if (!fileEntry.isDirectory)
            {
                try
                {
                    fileEntry.size = entry.file_size();
                }
                catch (...)
                {
                    fileEntry.size = 0;
                }
            }
            else
            {
                fileEntry.size = 0;
            }

            try
            {
                fileEntry.lastModified = entry.last_write_time();
            }
            catch (...)
            {
                // Use epoch time if we can't get the real time
                fileEntry.lastModified = std::filesystem::file_time_type();
            }

            // Apply filter if active
            if (!m_filterExtensions.empty())
            {
                bool shouldShow = false;

                if (fileEntry.isDirectory)
                {
                    // Show directory only if "[folders]" is in the filter set
                    shouldShow = (m_filterExtensions.count(L"[folders]") > 0);
                }
                else
                {
                    // Show file only if its extension is in the filter set
                    std::filesystem::path p(entry.path());
                    std::wstring ext = p.extension().wstring();
                    // Convert to lowercase for case-insensitive comparison
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

                    shouldShow = (m_filterExtensions.count(ext) > 0);
                }

                if (!shouldShow)
                    continue;  // Skip items that don't match any active filter
            }

            m_files.push_back(fileEntry);
        }

        // Sort using current sort settings
        SortFileList();
    }
    catch (const std::exception&)
    {
        // Failed to read directory
    }
}

void FileBrowser::NavigateUp()
{
    std::filesystem::path currentPath(m_currentDirectory);
    if (currentPath.has_parent_path())
    {
        auto parent = currentPath.parent_path();
        if (!parent.empty())
        {
            NavigateTo(parent.wstring());  // Use NavigateTo to add to history
        }
    }
}

void FileBrowser::NavigateTo(const std::wstring& path)
{
    // Add current directory to back history (unless we're navigating via back/forward)
    if (!m_isNavigatingHistory && !m_currentDirectory.empty() && m_currentDirectory != path)
    {
        m_backHistory.push_back(m_currentDirectory);
        // Clear forward history when navigating to a new location
        m_forwardHistory.clear();
    }

    SetCurrentDirectory(path);
}

void FileBrowser::NavigateBack()
{
    // If in search mode, exit search first (which returns to pre-search directory)
    if (m_isSearchMode)
    {
        ExitSearchMode();
        return;
    }

    if (m_backHistory.empty())
        return;

    // Move current location to forward history
    m_forwardHistory.push_back(m_currentDirectory);

    // Get the last back location
    std::wstring backPath = m_backHistory.back();
    m_backHistory.pop_back();

    // Navigate without adding to history
    m_isNavigatingHistory = true;
    SetCurrentDirectory(backPath);
    m_isNavigatingHistory = false;
}

void FileBrowser::NavigateForward()
{
    if (m_forwardHistory.empty())
        return;

    // Move current location to back history
    m_backHistory.push_back(m_currentDirectory);

    // Get the last forward location
    std::wstring forwardPath = m_forwardHistory.back();
    m_forwardHistory.pop_back();

    // Navigate without adding to history
    m_isNavigatingHistory = true;
    SetCurrentDirectory(forwardPath);
    m_isNavigatingHistory = false;
}

void FileBrowser::ShowContextMenu(HWND hwnd, const std::wstring& path, const ImVec2& screenPos)
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

// ============================================================================
// HELPER FUNCTIONS FOR CONTEXT MENU ACTIONS
// ============================================================================

void FileBrowser::CopyToClipboard(const std::wstring& text)
{
    if (text.empty())
        return;

    if (!OpenClipboard(nullptr))
        return;

    EmptyClipboard();

    // Allocate global memory for the text
    size_t size = (text.length() + 1) * sizeof(wchar_t);
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, size);
    if (hGlobal)
    {
        wchar_t* pGlobal = (wchar_t*)GlobalLock(hGlobal);
        if (pGlobal)
        {
            memcpy(pGlobal, text.c_str(), size);
            GlobalUnlock(hGlobal);
            SetClipboardData(CF_UNICODETEXT, hGlobal);
        }
    }

    CloseClipboard();
}

void FileBrowser::CopyFilesToClipboard(const std::vector<std::wstring>& paths)
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
        totalSize += (path.length() + 1) * sizeof(wchar_t);  // +1 for null terminator after each path
    }
    totalSize += sizeof(wchar_t);  // Final null terminator

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, totalSize);
    if (hGlobal)
    {
        DROPFILES* pDropFiles = (DROPFILES*)GlobalLock(hGlobal);
        if (pDropFiles)
        {
            // Fill DROPFILES structure
            pDropFiles->pFiles = sizeof(DROPFILES);
            pDropFiles->pt.x = 0;
            pDropFiles->pt.y = 0;
            pDropFiles->fNC = FALSE;
            pDropFiles->fWide = TRUE;  // Unicode paths

            // Copy file paths after the DROPFILES structure
            wchar_t* pPath = (wchar_t*)((BYTE*)pDropFiles + sizeof(DROPFILES));
            for (const auto& path : paths)
            {
                size_t len = path.length() + 1;
                wcscpy_s(pPath, len, path.c_str());
                pPath += len;
            }
            *pPath = L'\0';  // Double null terminator

            GlobalUnlock(hGlobal);
            SetClipboardData(CF_HDROP, hGlobal);

            std::wcout << L"[FileBrowser] Copied " << paths.size() << L" file(s) to clipboard" << std::endl;
        }
    }

    CloseClipboard();
}

void FileBrowser::CutFilesToClipboard(const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return;

    // Copy files to clipboard
    CopyFilesToClipboard(paths);

    // Store cut files for deletion after paste
    m_cutFiles = paths;

    std::wcout << L"[FileBrowser] Cut " << paths.size() << L" file(s) to clipboard" << std::endl;
}

void FileBrowser::PasteFilesFromClipboard()
{
    if (!OpenClipboard(nullptr))
        return;

    HANDLE hData = GetClipboardData(CF_HDROP);
    if (hData)
    {
        HDROP hDrop = (HDROP)hData;

        // Get number of files in clipboard
        UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);

        // Build source file list and check if any file is from the same directory
        std::wstring sourceFiles;
        bool sameDirectory = false;
        for (UINT i = 0; i < fileCount; i++)
        {
            wchar_t filePath[MAX_PATH];
            if (DragQueryFileW(hDrop, i, filePath, MAX_PATH))
            {
                sourceFiles += filePath;
                sourceFiles += L'\0';

                // Check if source is in the same directory as destination
                std::filesystem::path sourcePath(filePath);
                std::filesystem::path sourceDir = sourcePath.parent_path();
                if (std::filesystem::equivalent(sourceDir, m_currentDirectory))
                {
                    sameDirectory = true;
                }
            }
        }
        sourceFiles += L'\0';  // Double null terminator

        // Perform copy operation using SHFileOperation
        SHFILEOPSTRUCTW fileOp = {};
        fileOp.wFunc = FO_COPY;
        fileOp.pFrom = sourceFiles.c_str();
        fileOp.pTo = m_currentDirectory.c_str();
        fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;

        // If pasting into same directory, automatically rename to avoid collision
        if (sameDirectory)
        {
            fileOp.fFlags |= FOF_RENAMEONCOLLISION;
        }

        int result = SHFileOperationW(&fileOp);
        if (result == 0)
        {
            // If files were cut, delete them after successful paste
            if (!m_cutFiles.empty())
            {
                DeleteFilesToRecycleBin(m_cutFiles);
                m_cutFiles.clear();
            }
            else
            {
                RefreshFileList();
            }
        }
    }

    CloseClipboard();
}

void FileBrowser::RevealInExplorer(const std::wstring& path)
{
    // Use explorer.exe /select to open Explorer and select the file
    std::wstring command = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", command.c_str(), nullptr, SW_SHOW);
}

void FileBrowser::DeleteFilesToRecycleBin(const std::vector<std::wstring>& paths)
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
    pathsDoubleNull.push_back(L'\0');  // Final null terminator

    // Use SHFileOperation to move to recycle bin
    SHFILEOPSTRUCTW fileOp = {};
    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = pathsDoubleNull.c_str();
    fileOp.fFlags = FOF_ALLOWUNDO | FOF_NO_UI;  // FOF_ALLOWUNDO = move to recycle bin

    int result = SHFileOperationW(&fileOp);
    if (result == 0)
    {
        // Refresh the file list after deletion
        RefreshFileList();
        std::wcout << L"[FileBrowser] Deleted " << paths.size() << L" file(s) to recycle bin" << std::endl;
    }
}

void FileBrowser::CopyFileToDestination(const std::wstring& sourcePath, const std::wstring& destDirectory)
{
    std::vector<std::wstring> paths = { sourcePath };
    CopyFilesToDestination(paths, destDirectory);
}

void FileBrowser::CopyFilesToDestination(const std::vector<std::wstring>& sourcePaths, const std::wstring& destDirectory)
{
    if (sourcePaths.empty())
        return;

    std::wcout << L"[FileBrowser] Copying " << sourcePaths.size() << L" item(s) to: " << destDirectory << std::endl;

    // Build double-null terminated string for sources
    std::wstring sourceDoubleNull;
    for (const auto& path : sourcePaths)
    {
        sourceDoubleNull += path;
        sourceDoubleNull.push_back(L'\0');
    }
    sourceDoubleNull.push_back(L'\0');  // Final null terminator

    // Build null-terminated double-null string for destination
    std::wstring destDoubleNull = destDirectory;
    destDoubleNull.push_back(L'\0');
    destDoubleNull.push_back(L'\0');

    // Use SHFileOperation to copy with native Windows progress dialog
    SHFILEOPSTRUCTW fileOp = {};
    fileOp.hwnd = nullptr;
    fileOp.wFunc = FO_COPY;
    fileOp.pFrom = sourceDoubleNull.c_str();
    fileOp.pTo = destDoubleNull.c_str();
    fileOp.fFlags = FOF_ALLOWUNDO;  // Show progress dialog, allow undo

    int result = SHFileOperationW(&fileOp);
    if (result == 0 && !fileOp.fAnyOperationsAborted)
    {
        std::wcout << L"[FileBrowser] Copy succeeded!" << std::endl;
        // Refresh the file list after copy
        RefreshFileList();
    }
    else if (fileOp.fAnyOperationsAborted)
    {
        std::wcout << L"[FileBrowser] Copy was cancelled by user" << std::endl;
    }
    else
    {
        std::wcout << L"[FileBrowser] Copy failed with error: " << result << std::endl;
    }
}

ImVec4 FileBrowser::GetAccentColor()
{
    // Use the global accent color function with transparency for selection
    ImVec4 accent = GetWindowsAccentColor();
    accent.w = 0.3f;  // Add transparency for selection highlight
    return accent;
}

// ============================================================================
// IMGUI CONTEXT MENU
// ============================================================================

void FileBrowser::ShowImGuiContextMenu(HWND hwnd, const FileEntry& entry)
{
    // Store the path for the context menu
    m_contextMenuPath = entry.fullPath;

    // Convert filename to UTF-8 for ImGui
    char nameUtf8[512];
    WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);

    if (ImGui::BeginPopup("file_context_menu"))
    {
        // Header with filename
        ImGui::TextDisabled("%s", nameUtf8);
        ImGui::Separator();

        // Show in Browser (only in search mode)
        if (m_isSearchMode)
        {
            if (ImGui::MenuItem("Show in Browser"))
            {
                ShowInBrowser(entry.fullPath);
            }
            ImGui::Separator();
        }

        // Copy (file operation)
        if (ImGui::MenuItem("Copy"))
        {
            // Collect all selected file paths
            std::vector<std::wstring> selectedPaths;
            if (m_selectedIndices.empty())
            {
                // If no selection, copy just this file
                selectedPaths.push_back(entry.fullPath);
            }
            else
            {
                // Copy all selected files
                for (int idx : m_selectedIndices)
                {
                    if (idx >= 0 && idx < m_files.size())
                    {
                        selectedPaths.push_back(m_files[idx].fullPath);
                    }
                }
            }
            CopyFilesToClipboard(selectedPaths);
        }

        // Cut (file operation)
        if (ImGui::MenuItem("Cut"))
        {
            // Collect all selected file paths
            std::vector<std::wstring> selectedPaths;
            if (m_selectedIndices.empty())
            {
                // If no selection, cut just this file
                selectedPaths.push_back(entry.fullPath);
            }
            else
            {
                // Cut all selected files
                for (int idx : m_selectedIndices)
                {
                    if (idx >= 0 && idx < m_files.size())
                    {
                        selectedPaths.push_back(m_files[idx].fullPath);
                    }
                }
            }
            CutFilesToClipboard(selectedPaths);
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

        // Open in Other Browser
        if (onOpenInOtherBrowser)
        {
            if (ImGui::MenuItem("Open in Other Browser"))
            {
                onOpenInOtherBrowser(entry.fullPath);
                ImGui::CloseCurrentPopup();
            }
        }

        // Open in New Window
        if (onOpenInNewWindow)
        {
            if (ImGui::MenuItem("Open in New Window"))
            {
                std::wstring pathToOpen = entry.isDirectory ? entry.fullPath : std::filesystem::path(entry.fullPath).parent_path().wstring();
                onOpenInNewWindow(pathToOpen);
                ImGui::CloseCurrentPopup();
            }
        }

        // Open in Browser 1 and Browser 2 (when used in specialized views)
        if (onOpenInBrowser1 || onOpenInBrowser2)
        {
            ImGui::Separator();

            if (onOpenInBrowser1)
            {
                if (ImGui::MenuItem("Open in the Left Browser"))
                {
                    std::wstring pathToOpen = entry.isDirectory ? entry.fullPath : std::filesystem::path(entry.fullPath).parent_path().wstring();
                    onOpenInBrowser1(pathToOpen);
                    ImGui::CloseCurrentPopup();
                }
            }

            if (onOpenInBrowser2)
            {
                if (ImGui::MenuItem("Open in the Right Browser"))
                {
                    std::wstring pathToOpen = entry.isDirectory ? entry.fullPath : std::filesystem::path(entry.fullPath).parent_path().wstring();
                    onOpenInBrowser2(pathToOpen);
                    ImGui::CloseCurrentPopup();
                }
            }
        }

        // Open Shot View (only for shot category folders in synced jobs)
        if (entry.isDirectory && m_subscriptionManager && onOpenShotView)
        {
            // Check if current directory is a synced job
            auto subscription = m_subscriptionManager->GetSubscription(m_currentDirectory);
            if (subscription.has_value() && subscription->isActive)
            {
                // Check if this folder is a shot category type
                static const std::set<std::wstring> shotCategories = {
                    L"3d", L"ae", L"audition", L"illustrator", L"photoshop", L"premiere"
                };

                std::wstring folderName = entry.name;
                std::transform(folderName.begin(), folderName.end(), folderName.begin(), ::towlower);

                if (shotCategories.find(folderName) != shotCategories.end())
                {
                    ImGui::Separator();

                    // Use bright accent color
                    ImVec4 accentColor = GetAccentColor();
                    ImVec4 brightAccent = ImVec4(
                        accentColor.x * 1.3f,
                        accentColor.y * 1.3f,
                        accentColor.z * 1.3f,
                        1.0f
                    );
                    ImGui::PushStyleColor(ImGuiCol_Text, brightAccent);

                    if (ImGui::MenuItem("Open Shot View"))
                    {
                        onOpenShotView(entry.fullPath, entry.name);
                        ImGui::CloseCurrentPopup();
                    }

                    ImGui::PopStyleColor();
                }

                // Check if this folder is "assets" or "postings"
                std::wstring folderNameLower = entry.name;
                std::transform(folderNameLower.begin(), folderNameLower.end(), folderNameLower.begin(), ::towlower);

                if (folderNameLower == L"assets" && onOpenAssetsView)
                {
                    ImGui::Separator();

                    // Use bright accent color
                    ImVec4 accentColor = GetAccentColor();
                    ImVec4 brightAccent = ImVec4(
                        accentColor.x * 1.3f,
                        accentColor.y * 1.3f,
                        accentColor.z * 1.3f,
                        1.0f
                    );
                    ImGui::PushStyleColor(ImGuiCol_Text, brightAccent);

                    if (ImGui::MenuItem("Open Assets View"))
                    {
                        // Get job name from subscription
                        std::wstring jobName = subscription->jobName;
                        onOpenAssetsView(entry.fullPath, jobName);
                        ImGui::CloseCurrentPopup();
                    }

                    ImGui::PopStyleColor();
                }

                if (folderNameLower == L"postings" && onOpenPostingsView)
                {
                    ImGui::Separator();

                    // Use bright accent color
                    ImVec4 accentColor = GetAccentColor();
                    ImVec4 brightAccent = ImVec4(
                        accentColor.x * 1.3f,
                        accentColor.y * 1.3f,
                        accentColor.z * 1.3f,
                        1.0f
                    );
                    ImGui::PushStyleColor(ImGuiCol_Text, brightAccent);

                    if (ImGui::MenuItem("Open Postings View"))
                    {
                        // Get job name from subscription
                        std::wstring jobName = subscription->jobName;
                        onOpenPostingsView(entry.fullPath, jobName);
                        ImGui::CloseCurrentPopup();
                    }

                    ImGui::PopStyleColor();
                }
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
                //std::wcout << L"[FileBrowser] Showing Transcode menu for video file, " << m_selectedIndices.size() << L" files selected" << std::endl;

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
                    // Collect all selected video file paths
                    std::vector<std::wstring> selectedVideos;

                    if (m_selectedIndices.empty())
                    {
                        // If no selection, transcode just this file
                        selectedVideos.push_back(entry.fullPath);
                    }
                    else
                    {
                        // Transcode all selected video files
                        for (int idx : m_selectedIndices)
                        {
                            if (idx >= 0 && idx < m_files.size())
                            {
                                const auto& file = m_files[idx];
                                if (!file.isDirectory)
                                {
                                    std::filesystem::path fp(file.fullPath);
                                    std::wstring fileExt = fp.extension().wstring();
                                    std::transform(fileExt.begin(), fileExt.end(), fileExt.begin(), ::towlower);

                                    if (videoExtensions.find(fileExt) != videoExtensions.end())
                                    {
                                        selectedVideos.push_back(file.fullPath);
                                    }
                                }
                            }
                        }
                    }

                    std::wcout << L"[FileBrowser] Sending " << selectedVideos.size() << L" videos to transcode" << std::endl;
                    for (const auto& vid : selectedVideos) {
                        std::wcout << L"  - " << vid << std::endl;
                    }

                    if (!selectedVideos.empty())
                    {
                        onTranscodeToMP4(selectedVideos);
                    }

                    ImGui::CloseCurrentPopup();
                }

                ImGui::PopStyleColor();
            }
        }

        // Sync as Job (only for directories when in a project folder)
        if (entry.isDirectory && m_bookmarkManager && m_subscriptionManager)
        {
            // Check if current directory (parent) is a project folder
            // Try both exact match and canonicalized path (for symlinks/junctions)
            bool isProjectFolder = false;

            // First try exact match
            auto bookmark = m_bookmarkManager->GetBookmarkByPath(m_currentDirectory);
            if (bookmark.has_value() && bookmark->isProjectFolder)
            {
                isProjectFolder = true;
            }
            else
            {
                // Try canonicalizing current directory and checking all bookmarks
                try
                {
                    std::filesystem::path canonicalCurrent = std::filesystem::canonical(m_currentDirectory);
                    auto allBookmarks = m_bookmarkManager->GetAllBookmarks();

                    for (const auto& bm : allBookmarks)
                    {
                        if (bm.isProjectFolder)
                        {
                            try
                            {
                                std::filesystem::path canonicalBookmark = std::filesystem::canonical(bm.path);
                                if (canonicalCurrent == canonicalBookmark)
                                {
                                    isProjectFolder = true;
                                    break;
                                }
                            }
                            catch (const std::exception&)
                            {
                                // Bookmark path might not exist, skip
                                continue;
                            }
                        }
                    }
                }
                catch (const std::exception&)
                {
                    // Current directory might not be canonical, use original path
                }
            }

            if (isProjectFolder)
            {
                ImGui::Separator();

                // Use bright accent color like "Transcode to MP4"
                ImVec4 accentColor = GetAccentColor();
                ImVec4 brightAccent = ImVec4(
                    accentColor.x * 1.3f,
                    accentColor.y * 1.3f,
                    accentColor.z * 1.3f,
                    1.0f
                );
                ImGui::PushStyleColor(ImGuiCol_Text, brightAccent);

                if (ImGui::MenuItem("Sync as Job"))
                {
                    // Use the subfolder name as job name
                    std::wstring jobName = entry.name;
                    m_subscriptionManager->SubscribeToJob(entry.fullPath, jobName);
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

        // Delete
        if (ImGui::MenuItem("Delete"))
        {
            // Collect all selected file paths
            std::vector<std::wstring> selectedPaths;
            if (m_selectedIndices.empty())
            {
                // If no selection, delete just this file
                selectedPaths.push_back(entry.fullPath);
            }
            else
            {
                // Delete all selected files
                for (int idx : m_selectedIndices)
                {
                    if (idx >= 0 && idx < m_files.size())
                    {
                        selectedPaths.push_back(m_files[idx].fullPath);
                    }
                }
            }
            DeleteFilesToRecycleBin(selectedPaths);
            ImGui::CloseCurrentPopup();
        }

        ImGui::Separator();

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

                std::wcout << L"[FileBrowser] Copied ufb:/// link to clipboard: "
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

        // Custom context menu items (callback)
        if (onCustomContextMenu)
        {
            ImGui::Separator();

            // Collect selected file paths
            std::vector<std::wstring> selectedPaths;
            if (m_selectedIndices.empty())
            {
                selectedPaths.push_back(entry.fullPath);
            }
            else
            {
                for (int idx : m_selectedIndices)
                {
                    if (idx >= 0 && idx < m_files.size())
                    {
                        selectedPaths.push_back(m_files[idx].fullPath);
                    }
                }
            }

            onCustomContextMenu(selectedPaths);
        }

        ImGui::EndPopup();
    }
}

void FileBrowser::DrawNavigationBar()
{
   
    // Editable path bar - sync buffer with current directory when not editing
    if (!ImGui::IsItemActive() || m_pathBuffer[0] == '\0')
    {
        WideCharToMultiByte(CP_UTF8, 0, m_currentDirectory.c_str(), -1, m_pathBuffer, sizeof(m_pathBuffer), nullptr, nullptr);
    }

    // Draw read-only path text with mono font
    if (font_mono)
        ImGui::PushFont(font_mono);

    ImGui::SetNextItemWidth(-1.0f);  // Full width
    ImGui::InputText("##path", m_pathBuffer, sizeof(m_pathBuffer), ImGuiInputTextFlags_ReadOnly);

    if (font_mono)
        ImGui::PopFont();

    // Right-click context menu for path text area
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        ImGui::OpenPopup("PathContextMenu");
    }

    if (ImGui::BeginPopup("PathContextMenu"))
    {
        if (ImGui::MenuItem("Copy Path"))
        {
            // Copy current directory path to clipboard
            ImGui::SetClipboardText(m_pathBuffer);
        }

        if (ImGui::MenuItem("Open in Explorer"))
        {
            // Open Windows Explorer at the current directory
            ShellExecuteW(nullptr, L"explore", m_currentDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        ImGui::EndPopup();
    }

    // Back button (arrow left)
    // Enable back button if we have history OR if we're in search mode
    bool canGoBack = !m_backHistory.empty() || m_isSearchMode;
    if (!canGoBack)
        ImGui::BeginDisabled();

    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE5CB")))  // Material Icons arrow_back
        {
            NavigateBack();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("<"))
        {
            NavigateBack();
        }
    }

    if (!canGoBack)
        ImGui::EndDisabled();

    ImGui::SameLine();

    // Forward button (arrow right)
    bool canGoForward = !m_forwardHistory.empty();
    if (!canGoForward)
        ImGui::BeginDisabled();

    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE5CC")))  // Material Icons arrow_forward
        {
            NavigateForward();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button(">"))
        {
            NavigateForward();
        }
    }

    if (!canGoForward)
        ImGui::EndDisabled();

    ImGui::SameLine();

    // Up button at the left
    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE5CE")))  // Material Icons arrow_upward
        {
            NavigateUp();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("^"))
        {
            NavigateUp();
        }
    }

    ImGui::SameLine();

    // Refresh button
    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE5D5")))  // Material Icons refresh symbol
        {
            RefreshFileList();
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("Refresh"))
        {
            RefreshFileList();
        }
    }

    ImGui::SameLine();

    // New Job button (only show if in project folder)
    if (m_bookmarkManager)
    {
        bool isProjectFolder = false;

        // Check if current directory is a project folder
        auto bookmark = m_bookmarkManager->GetBookmarkByPath(m_currentDirectory);
        if (bookmark.has_value() && bookmark->isProjectFolder)
        {
            isProjectFolder = true;
        }
        else
        {
            // Try canonicalizing current directory and checking all bookmarks
            try
            {
                std::filesystem::path canonicalCurrent = std::filesystem::canonical(m_currentDirectory);
                auto allBookmarks = m_bookmarkManager->GetAllBookmarks();

                for (const auto& bm : allBookmarks)
                {
                    if (bm.isProjectFolder)
                    {
                        try
                        {
                            std::filesystem::path canonicalBookmark = std::filesystem::canonical(bm.path);
                            if (canonicalCurrent == canonicalBookmark)
                            {
                                isProjectFolder = true;
                                break;
                            }
                        }
                        catch (const std::exception&)
                        {
                            continue;
                        }
                    }
                }
            }
            catch (const std::exception&)
            {
                // Current directory might not be canonical, use original path
            }
        }

        if (isProjectFolder)
        {
            ImGui::SameLine();

            if (font_icons)
            {
                ImGui::PushFont(font_icons);
                if (ImGui::Button(U8("\uE145")))  // Material Icons add symbol
                {
                    m_showNewJobDialog = true;
                    // Clear buffers
                    memset(m_newJobNumberBuffer, 0, sizeof(m_newJobNumberBuffer));
                    memset(m_newJobNameBuffer, 0, sizeof(m_newJobNameBuffer));
                }
                ImGui::PopFont();
            }
            else
            {
                if (ImGui::Button("+"))
                {
                    m_showNewJobDialog = true;
                    // Clear buffers
                    memset(m_newJobNumberBuffer, 0, sizeof(m_newJobNumberBuffer));
                    memset(m_newJobNameBuffer, 0, sizeof(m_newJobNameBuffer));
                }
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Create New Job");
            }
        }
    }

    ImGui::SameLine();

    // Filter button
    if (font_icons)
    {
        ImGui::PushFont(font_icons);
        if (ImGui::Button(U8("\uE152")))  // Material Icons filter_list symbol
        {
            ImGui::OpenPopup("FilterPopup");
        }
        ImGui::PopFont();
    }
    else
    {
        if (ImGui::Button("Filter"))
        {
            ImGui::OpenPopup("FilterPopup");
        }
    }

    // Filter popup
    if (ImGui::BeginPopup("FilterPopup"))
    {
        // Reduce padding for more compact menu
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));

        ImGui::TextDisabled("Filter by Type (click outside to close)");
        ImGui::Separator();

        if (ImGui::Button("Reset All"))
        {
            m_filterExtensions.clear();
            RefreshFileList();
        }

        ImGui::Separator();

        // Folders toggle
        bool foldersSelected = m_filterExtensions.count(L"[folders]") > 0;
        if (ImGui::Checkbox("Folders", &foldersSelected))
        {
            if (foldersSelected)
                m_filterExtensions.insert(L"[folders]");
            else
                m_filterExtensions.erase(L"[folders]");
            RefreshFileList();
        }

        ImGui::Separator();

        // Collect unique extensions from current directory (unfiltered)
        std::set<std::wstring> extensions;
        try
        {
            for (const auto& entry : std::filesystem::directory_iterator(m_currentDirectory))
            {
                if (!entry.is_directory())
                {
                    std::filesystem::path p(entry.path());
                    std::wstring ext = p.extension().wstring();
                    if (!ext.empty())
                    {
                        // Convert to lowercase for consistency
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                        extensions.insert(ext);
                    }
                }
            }
        }
        catch (...)
        {
            // Failed to read directory
        }

        // Display each extension as a toggleable checkbox
        for (const auto& ext : extensions)
        {
            // Convert to UTF-8 for ImGui
            char extUtf8[64];
            WideCharToMultiByte(CP_UTF8, 0, ext.c_str(), -1, extUtf8, sizeof(extUtf8), nullptr, nullptr);

            bool isSelected = m_filterExtensions.count(ext) > 0;
            if (ImGui::Checkbox(extUtf8, &isSelected))
            {
                if (isSelected)
                    m_filterExtensions.insert(ext);
                else
                    m_filterExtensions.erase(ext);
                RefreshFileList();
            }
        }

        ImGui::PopStyleVar(2);
        ImGui::EndPopup();
    }

    ImGui::SameLine();

    // Search input box
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputTextWithHint("##search", "Search...", m_searchQuery, sizeof(m_searchQuery), ImGuiInputTextFlags_EnterReturnsTrue))
    {
        // User pressed Enter - execute search
        if (strlen(m_searchQuery) > 0)
        {
            ExecuteSearch(m_searchQuery);
        }
    }

    // Exit Search button (only visible in search mode)
    if (m_isSearchMode)
    {
        ImGui::SameLine();
        if (ImGui::Button("Exit Search"))
        {
            ExitSearchMode();
        }
    }

    // Spacer, separator, spacer
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();

    // View mode toggle
    if (ImGui::RadioButton("List", m_viewMode == ViewMode::List))
    {
        m_viewMode = ViewMode::List;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Grid", m_viewMode == ViewMode::Grid))
    {
        m_viewMode = ViewMode::Grid;
    }

    // Thumbnail size slider (only visible in grid mode)
    if (m_viewMode == ViewMode::Grid)
    {
        ImGui::SameLine();
        ImGui::Text("Size:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderFloat("##thumbsize", &m_thumbnailSize, 64.0f, 512.0f, "%.0f");

        // Regenerate button to clear cache and re-extract all thumbnails at new size
        ImGui::SameLine();
        if (ImGui::Button("Regenerate"))
        {
            m_thumbnailManager.ClearCache();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Clear thumbnail cache and regenerate all thumbnails at current size");
        }
    }
}

void FileBrowser::DrawFileList(HWND hwnd)
{
    // Show search results banner if in search mode
    if (m_isSearchMode)
    {
        ImVec4 bgColor = (m_searchResultCount == 0)
            ? ImVec4(0.4f, 0.2f, 0.2f, 0.3f)  // Reddish for no results
            : ImVec4(0.2f, 0.3f, 0.4f, 0.3f); // Blueish for results

        ImGui::PushStyleColor(ImGuiCol_ChildBg, bgColor);
        ImGui::BeginChild("SearchBanner", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * (m_searchResultCount == 0 ? 2.5f : 1.5f)), true);

        ImVec4 accentColor = GetAccentColor();
        ImGui::PushStyleColor(ImGuiCol_Text, accentColor);

        char bannerText[512];
        if (m_searchResultCount == 0)
        {
            snprintf(bannerText, sizeof(bannerText), "Search Mode: No results found for \"%s\" in %s",
                     m_searchQuery, UFB::WideToUtf8(m_preSearchDirectory).c_str());
            ImGui::TextWrapped("%s", bannerText);

            // Check if network path
            if (m_preSearchDirectory.size() >= 2 && m_preSearchDirectory[0] == L'\\' && m_preSearchDirectory[1] == L'\\')
            {
                ImGui::TextDisabled("Note: Everything may not be indexing this drive or folder");
            }
            else
            {
              /*  ImGui::TextDisabled("Possible causes: 1) Everything hasn't indexed this folder, 2) Folder is excluded in Everything settings,");
                ImGui::TextDisabled("3) Cloud sync/junction point - check Everything > Options > Indexes > NTFS > 'Index folder junctions'");*/
            }
        }
        else
        {
            snprintf(bannerText, sizeof(bannerText), "Search Mode: Found %d result%s for \"%s\" in %s",
                     m_searchResultCount, (m_searchResultCount == 1 ? "" : "s"),
                     m_searchQuery, UFB::WideToUtf8(m_preSearchDirectory).c_str());
            ImGui::TextWrapped("%s", bannerText);
        }

        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // Process completed thumbnails from background threads (convert HBITMAP to GL textures)
    m_thumbnailManager.ProcessCompletedThumbnails();

    // Handle keyboard shortcuts (Ctrl+C, Ctrl+X, Ctrl+V, Del)
    // Check if this specific window or its children are focused (but not siblings)
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
    {
        ImGuiIO& io = ImGui::GetIO();

        // Ctrl+C - Copy selected files to clipboard
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
        {
            if (!m_selectedIndices.empty())
            {
                std::vector<std::wstring> selectedPaths;
                for (int idx : m_selectedIndices)
                {
                    if (idx >= 0 && idx < m_files.size())
                    {
                        selectedPaths.push_back(m_files[idx].fullPath);
                    }
                }
                if (!selectedPaths.empty())
                {
                    CopyFilesToClipboard(selectedPaths);
                }
            }
        }

        // Ctrl+X - Cut selected files to clipboard
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X))
        {
            if (!m_selectedIndices.empty())
            {
                std::vector<std::wstring> selectedPaths;
                for (int idx : m_selectedIndices)
                {
                    if (idx >= 0 && idx < m_files.size())
                    {
                        selectedPaths.push_back(m_files[idx].fullPath);
                    }
                }
                if (!selectedPaths.empty())
                {
                    CutFilesToClipboard(selectedPaths);
                }
            }
        }

        // Ctrl+V - Paste files from clipboard
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V))
        {
            PasteFilesFromClipboard();
        }

        // Delete - Delete selected files to recycle bin
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))
        {
            if (!m_selectedIndices.empty())
            {
                std::vector<std::wstring> selectedPaths;
                for (int idx : m_selectedIndices)
                {
                    if (idx >= 0 && idx < m_files.size())
                    {
                        selectedPaths.push_back(m_files[idx].fullPath);
                    }
                }
                if (!selectedPaths.empty())
                {
                    DeleteFilesToRecycleBin(selectedPaths);
                }
            }
        }

        // F2 - Rename (single file only)
        if (ImGui::IsKeyPressed(ImGuiKey_F2))
        {
            // Only allow rename if exactly one file is selected
            if (m_selectedIndices.size() == 1)
            {
                int idx = *m_selectedIndices.begin();
                if (idx >= 0 && idx < m_files.size())
                {
                    m_renameOriginalPath = m_files[idx].fullPath;
                    std::wstring filename = m_files[idx].name;
                    WideCharToMultiByte(CP_UTF8, 0, filename.c_str(), -1, m_renameBuffer, sizeof(m_renameBuffer), nullptr, nullptr);
                    m_showRenameDialog = true;
                }
            }
        }
    }

    // Switch between view modes
    if (m_viewMode == ViewMode::List)
    {
        DrawListView(hwnd);
    }
    else
    {
        DrawGridView(hwnd);
    }

    // New Folder dialog modal (shared between both view modes)
    if (m_showNewFolderDialog)
    {
        ImGui::OpenPopup("New Folder");
        m_showNewFolderDialog = false;  // Only open once
    }

    if (ImGui::BeginPopupModal("New Folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Enter folder name:");
        ImGui::SetNextItemWidth(300.0f);

        // Auto-focus the input field when opened
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }

        bool enterPressed = ImGui::InputText("##newfolder", m_newFolderNameBuffer, sizeof(m_newFolderNameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Spacing();

        bool doCreate = false;
        if (ImGui::Button("OK", ImVec2(120, 0)) || enterPressed)
        {
            doCreate = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        // Handle folder creation
        if (doCreate && strlen(m_newFolderNameBuffer) > 0)
        {
            // Convert folder name from UTF-8 to wide string
            wchar_t folderNameW[256];
            MultiByteToWideChar(CP_UTF8, 0, m_newFolderNameBuffer, -1, folderNameW, 256);

            // Build new folder path
            std::filesystem::path newFolderPath = std::filesystem::path(m_currentDirectory) / folderNameW;

            // Attempt to create folder
            try
            {
                if (std::filesystem::create_directory(newFolderPath))
                {
                    std::wcout << L"[FileBrowser] Created folder: " << newFolderPath.wstring() << std::endl;
                    RefreshFileList();
                }
                else
                {
                    std::wcerr << L"[FileBrowser] Folder already exists: " << newFolderPath.wstring() << std::endl;
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << "[FileBrowser] Failed to create folder: " << e.what() << std::endl;
            }

            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // New u.f.b. Folder dialog modal
    if (m_showNewUFBFolderDialog)
    {
        ImGui::OpenPopup("New u.f.b. Folder");
        m_showNewUFBFolderDialog = false;  // Only open once
    }

    if (ImGui::BeginPopupModal("New u.f.b. Folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Enter folder name:");
        ImGui::SetNextItemWidth(300.0f);

        // Auto-focus the input field when opened
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }

        bool enterPressed = ImGui::InputText("##newufbfolder", m_newUFBFolderNameBuffer, sizeof(m_newUFBFolderNameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Spacing();

        bool doCreate = false;
        if (ImGui::Button("OK", ImVec2(120, 0)) || enterPressed)
        {
            doCreate = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        // Handle folder creation
        if (doCreate && strlen(m_newUFBFolderNameBuffer) > 0)
        {
            std::string folderName(m_newUFBFolderNameBuffer);
            if (CreateUFBFolder(folderName))
            {
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    }

    // New Job dialog modal
    if (m_showNewJobDialog)
    {
        ImGui::OpenPopup("Create New Job");
        m_showNewJobDialog = false;  // Only open once
    }

    if (ImGui::BeginPopupModal("Create New Job", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Create a new job from template");
        ImGui::Separator();
        ImGui::Spacing();

        // Job Number input
        ImGui::Text("Job Number:");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputText("##jobnumber", m_newJobNumberBuffer, sizeof(m_newJobNumberBuffer));

        ImGui::Spacing();

        // Job Name input
        ImGui::Text("Job Name:");
        ImGui::SetNextItemWidth(300.0f);
        bool enterPressed = ImGui::InputText("##jobname", m_newJobNameBuffer, sizeof(m_newJobNameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Spacing();

        // Preview folder name
        if (strlen(m_newJobNumberBuffer) > 0 || strlen(m_newJobNameBuffer) > 0)
        {
            std::string previewName = std::string(m_newJobNumberBuffer) + "_" + std::string(m_newJobNameBuffer);
            // Convert to lowercase
            std::transform(previewName.begin(), previewName.end(), previewName.begin(),
                [](unsigned char c) { return std::tolower(c); });
            // Replace spaces with underscores
            std::replace(previewName.begin(), previewName.end(), ' ', '_');

            ImGui::TextDisabled("Folder name: %s", previewName.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool doCreate = false;
        if (ImGui::Button("Create", ImVec2(120, 0)) || enterPressed)
        {
            doCreate = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        // Handle job creation
        if (doCreate)
        {
            if (strlen(m_newJobNumberBuffer) > 0 && strlen(m_newJobNameBuffer) > 0)
            {
                CreateJobFromTemplate(m_newJobNumberBuffer, m_newJobNameBuffer);
                RefreshFileList();  // Explicit refresh to update UI
                ImGui::CloseCurrentPopup();
            }
            else
            {
                // Show error - both fields required
                std::cerr << "[FileBrowser] Job number and name are required" << std::endl;
            }
        }

        ImGui::EndPopup();
    }

    // Drop target for entire browser window (for drag and drop between browsers)
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATHS"))
        {
            // Get the newline-delimited list of paths
            const char* allPathsUtf8 = (const char*)payload->Data;
            std::string pathsString(allPathsUtf8);

            std::wcout << L"[FileBrowser] Drop detected! Target: " << m_currentDirectory << std::endl;

            // Parse newline-delimited paths
            std::vector<std::wstring> sourcePaths;
            std::istringstream iss(pathsString);
            std::string line;

            while (std::getline(iss, line))
            {
                if (!line.empty())
                {
                    // Convert UTF-8 to wide string
                    int wideSize = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
                    if (wideSize > 0)
                    {
                        std::wstring sourcePath;
                        sourcePath.resize(wideSize);
                        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, &sourcePath[0], wideSize);
                        sourcePath.resize(wideSize - 1); // Remove null terminator
                        sourcePaths.push_back(sourcePath);
                    }
                }
            }

            // Copy all files/folders to current directory
            if (!sourcePaths.empty())
            {
                CopyFilesToDestination(sourcePaths, m_currentDirectory);
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void FileBrowser::DrawListView(HWND hwnd)
{
    // Create a snapshot of files to avoid holding lock during rendering
    std::vector<FileEntry> filesSnapshot;
    {
        std::lock_guard<std::mutex> lock(m_filesMutex);
        filesSnapshot = m_files;
    }

    // Clear and resize item bounds vector for box selection
    m_itemBounds.clear();
    m_itemBounds.resize(filesSnapshot.size());

    // Check if current directory is a project folder
    // Try both exact match and canonicalized path (for symlinks/junctions)
    bool isProjectFolder = false;
    if (m_bookmarkManager)
    {
        // First try exact match
        auto bookmark = m_bookmarkManager->GetBookmarkByPath(m_currentDirectory);
        if (bookmark.has_value() && bookmark->isProjectFolder)
        {
            isProjectFolder = true;
        }
        else
        {
            // Try canonicalizing current directory and checking all bookmarks
            try
            {
                std::filesystem::path canonicalCurrent = std::filesystem::canonical(m_currentDirectory);
                auto allBookmarks = m_bookmarkManager->GetAllBookmarks();

                for (const auto& bm : allBookmarks)
                {
                    if (bm.isProjectFolder)
                    {
                        try
                        {
                            std::filesystem::path canonicalBookmark = std::filesystem::canonical(bm.path);
                            if (canonicalCurrent == canonicalBookmark)
                            {
                                isProjectFolder = true;
                                break;
                            }
                        }
                        catch (const std::exception&)
                        {
                            // Bookmark path might not exist, skip
                            continue;
                        }
                    }
                }
            }
            catch (const std::exception&)
            {
                // Current directory might not be canonical, use original path
            }
        }
    }

    // Determine number of columns based on whether we're in a project folder
    int columnCount = isProjectFolder ? 4 : 3;

    // Table for file listing
    if (ImGui::BeginTable("FileList", columnCount, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        if (isProjectFolder)
        {
            ImGui::TableSetupColumn("Synced", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        }
        ImGui::TableHeadersRow();

        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsDirty)
            {
                // Get the sort column and direction
                if (sortSpecs->SpecsCount > 0)
                {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                    m_sortColumn = static_cast<SortColumn>(spec.ColumnIndex);
                    m_sortAscending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                    std::lock_guard<std::mutex> lock(m_filesMutex);
                    SortFileList();
                }
                sortSpecs->SpecsDirty = false;
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)filesSnapshot.size());

        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
            {
                const FileEntry& entry = filesSnapshot[i];

                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                // Get icon
                ImTextureID icon = m_iconManager.GetFileIcon(entry.fullPath, entry.isDirectory);

                // Draw icon and name
                ImGui::PushID(i);

                if (icon)
                {
                    ImGui::Image(icon, ImVec2(16, 16));
                    ImGui::SameLine();
                }

                // Convert name to UTF-8
                char nameUtf8[512];
                WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);

                // Check if item is selected
                bool isSelected = (m_selectedIndices.find(i) != m_selectedIndices.end());

                // Use accent color for selected items
                ImVec4 accentColor = GetAccentColor();
                if (isSelected)
                {
                    ImGui::PushStyleColor(ImGuiCol_Header, accentColor);
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accentColor.x * 1.1f, accentColor.y * 1.1f, accentColor.z * 1.1f, accentColor.w));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(accentColor.x * 1.2f, accentColor.y * 1.2f, accentColor.z * 1.2f, accentColor.w));
                }

                // Selectable for clicking
                if (ImGui::Selectable(nameUtf8, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    ImGuiIO& io = ImGui::GetIO();

                    if (io.KeyCtrl)
                    {
                        // Ctrl+Click: Toggle selection
                        if (isSelected)
                            m_selectedIndices.erase(i);
                        else
                            m_selectedIndices.insert(i);
                    }
                    else if (io.KeyShift && m_lastClickedIndex >= 0)
                    {
                        // Shift+Click: Range select
                        int start = (std::min)(m_lastClickedIndex, i);
                        int end = (std::max)(m_lastClickedIndex, i);
                        for (int idx = start; idx <= end; ++idx)
                        {
                            m_selectedIndices.insert(idx);
                        }
                    }
                    else
                    {
                        // Normal click: Select only this item
                        m_selectedIndices.clear();
                        m_selectedIndices.insert(i);
                    }

                    // Double-click detection
                    double currentTime = glfwGetTime();
                    if (m_lastClickedIndex == i && (currentTime - m_lastClickTime) < 0.3)
                    {
                        // Double-clicked
                        if (entry.isDirectory)
                        {
                            NavigateTo(entry.fullPath);
                        }
                        else
                        {
                            ShellExecuteW(nullptr, L"open", entry.fullPath.c_str(), nullptr, nullptr, SW_SHOW);
                        }
                    }
                    m_lastClickTime = currentTime;
                    m_lastClickedIndex = i;
                }

                // Store row bounds for box selection
                ImVec2 rowMin = ImGui::GetItemRectMin();
                ImVec2 rowMax = ImGui::GetItemRectMax();
                m_itemBounds[i] = std::make_pair(rowMin, rowMax);

                if (isSelected)
                {
                    ImGui::PopStyleColor(3);
                }

                // Drag source for file/folder (supports multi-select)
                static bool transitionedToOLEDrag = false;

#if OLE_DRAG_IMMEDIATE_MODE
                // IMMEDIATE OLE DRAG MODE: Skip ImGui drag and go straight to Windows OLE
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    // Build list of file paths for drag operation
                    std::vector<std::wstring> filePaths;

                    // If this item is part of selection, drag all selected items
                    if (m_selectedIndices.find(i) != m_selectedIndices.end())
                    {
                        for (int idx : m_selectedIndices)
                        {
                            if (idx < m_files.size())
                            {
                                filePaths.push_back(m_files[idx].fullPath);
                            }
                        }
                    }
                    else
                    {
                        // Not selected - drag just this one
                        filePaths.push_back(entry.fullPath);
                    }

                    if (!filePaths.empty())
                    {
                        std::wcout << L"[FileBrowser] Starting immediate Windows OLE drag (no ImGui transition)" << std::endl;
                        // Start Windows native drag and drop (this will block until drag completes)
                        StartWindowsDragDrop(filePaths);
                    }
                }
#else
                // HYBRID DRAG MODE: Start with ImGui drag, transition to OLE when leaving window
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                {
                    if (!transitionedToOLEDrag)
                    {
                        // Change cursor to indicate drag operation
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

                        // Build list of file paths for drag operation
                        std::vector<std::wstring> filePaths;
                        std::string allPathsUtf8;

                        // If this item is part of selection, drag all selected items
                        if (m_selectedIndices.find(i) != m_selectedIndices.end())
                        {
                            for (int idx : m_selectedIndices)
                            {
                                if (idx < m_files.size())
                                {
                                    filePaths.push_back(m_files[idx].fullPath);

                                    // Also build UTF-8 string for ImGui payload
                                    std::wstring& selectedPath = m_files[idx].fullPath;
                                    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, selectedPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
                                    if (utf8Size > 0)
                                    {
                                        std::string pathUtf8;
                                        pathUtf8.resize(utf8Size);
                                        WideCharToMultiByte(CP_UTF8, 0, selectedPath.c_str(), -1, &pathUtf8[0], utf8Size, nullptr, nullptr);
                                        pathUtf8.resize(utf8Size - 1); // Remove null terminator
                                        allPathsUtf8 += pathUtf8 + "\n";
                                    }
                                }
                            }
                        }
                        else
                        {
                            // Not selected - drag just this one
                            filePaths.push_back(entry.fullPath);

                            int utf8Size = WideCharToMultiByte(CP_UTF8, 0, entry.fullPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
                            if (utf8Size > 0)
                            {
                                allPathsUtf8.resize(utf8Size);
                                WideCharToMultiByte(CP_UTF8, 0, entry.fullPath.c_str(), -1, &allPathsUtf8[0], utf8Size, nullptr, nullptr);
                            }
                        }

                        // Check if mouse has left the main window (HWND) - if so, start Windows OLE drag
                        POINT cursorPos;
                        GetCursorPos(&cursorPos);

                        RECT windowRect;
                        GetWindowRect(hwnd, &windowRect);

                        bool mouseOutsideHWND = !PtInRect(&windowRect, cursorPos);

                        if (mouseOutsideHWND && !filePaths.empty())
                        {
                            std::wcout << L"[FileBrowser] Mouse left HWND during drag, starting Windows OLE drag" << std::endl;
                            transitionedToOLEDrag = true;
                            ImGui::EndDragDropSource();

                            // Start Windows native drag and drop (this will block until drag completes)
                            StartWindowsDragDrop(filePaths);

                            // Reset flag immediately after OLE drag completes
                            transitionedToOLEDrag = false;

                            // Don't continue with ImGui drag
                        }
                        else
                    {
                        // Set payload with all selected paths for internal ImGui drag
                        ImGui::SetDragDropPayload("FILE_PATHS", allPathsUtf8.c_str(), allPathsUtf8.size() + 1);

                        // Show enhanced preview with background
                        ImGui::BeginTooltip();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

                        int selectionCount = m_selectedIndices.size();
                        if (selectionCount > 1)
                        {
                            ImGui::Text("Dragging %d items", selectionCount);
                        }
                        else
                        {
                            const char* icon = entry.isDirectory ? ICON_FOLDER : ICON_FILE;

                            // Use icon font for icon, regular font for text
                            if (font_icons) ImGui::PushFont(font_icons);
                            ImGui::Text("%s", icon);
                            if (font_icons) ImGui::PopFont();

                            ImGui::SameLine();
                            ImGui::Text("%s", nameUtf8);
                        }

                        ImGui::PopStyleColor();
                        ImGui::EndTooltip();

                        ImGui::EndDragDropSource();
                        }
                    }
                    else
                    {
                        // Already transitioned to OLE drag - just end the drag source
                        ImGui::EndDragDropSource();
                    }
                }
                else
                {
                    // Drag ended - reset flag
                    transitionedToOLEDrag = false;
                }
#endif // OLE_DRAG_IMMEDIATE_MODE

                // ImGui context menu on right-click
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                {
                    ImGui::OpenPopup("file_context_menu");

                    // If right-clicked item is not in selection, select only it
                    if (!isSelected)
                    {
                        m_selectedIndices.clear();
                        m_selectedIndices.insert(i);
                    }
                }

                // Render the context menu
                ShowImGuiContextMenu(hwnd, entry);

                // Size column with mono font and disabled color
                ImGui::TableNextColumn();
                if (!entry.isDirectory)
                {
                    if (font_mono)
                        ImGui::PushFont(font_mono);
                    ImGui::TextDisabled("%s", FormatFileSize(entry.size).c_str());
                    if (font_mono)
                        ImGui::PopFont();
                }

                // Modified date column with mono font and disabled color
                ImGui::TableNextColumn();
                if (font_mono)
                    ImGui::PushFont(font_mono);
                ImGui::TextDisabled("%s", FormatFileTime(entry.lastModified).c_str());
                if (font_mono)
                    ImGui::PopFont();

                // Synced column (only if in project folder and entry is a directory)
                if (isProjectFolder)
                {
                    ImGui::TableNextColumn();
                    if (entry.isDirectory && m_subscriptionManager)
                    {
                        // Check if this directory is a synced job
                        auto subscription = m_subscriptionManager->GetSubscription(entry.fullPath);
                        if (subscription.has_value() && subscription->isActive)
                        {
                            // Use bright accent color
                            ImVec4 accentColor = GetAccentColor();
                            ImVec4 brightAccent = ImVec4(
                                accentColor.x * 1.3f,
                                accentColor.y * 1.3f,
                                accentColor.z * 1.3f,
                                1.0f
                            );
                            ImGui::PushStyleColor(ImGuiCol_Text, brightAccent);

                            if (font_mono)
                                ImGui::PushFont(font_mono);
                            ImGui::Text("✓");
                            if (font_mono)
                                ImGui::PopFont();

                            ImGui::PopStyleColor();
                        }
                    }
                }

                ImGui::PopID();
            }
        }

        ImGui::EndTable();

        // Box selection logic (drag-to-select in list view)
        // Detect box selection start - left-click on empty space
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            // Check if click was on empty space (not on any row)
            bool clickedOnRow = false;
            ImVec2 mousePos = ImGui::GetMousePos();
            for (int i = 0; i < m_itemBounds.size(); ++i)
            {
                const auto& [itemMin, itemMax] = m_itemBounds[i];
                if (mousePos.y >= itemMin.y && mousePos.y <= itemMax.y)
                {
                    clickedOnRow = true;
                    break;
                }
            }

            if (!clickedOnRow)
            {
                m_isBoxSelecting = true;
                m_boxSelectDragged = false;  // Reset drag flag
                m_boxSelectStart = mousePos;
            }
        }

        // Update selection during drag
        if (m_isBoxSelecting && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            m_boxSelectDragged = true;  // Mark that we dragged

            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec2 boxMin((std::min)(m_boxSelectStart.x, mousePos.x), (std::min)(m_boxSelectStart.y, mousePos.y));
            ImVec2 boxMax((std::max)(m_boxSelectStart.x, mousePos.x), (std::max)(m_boxSelectStart.y, mousePos.y));

            // Update selection based on modifier keys
            ImGuiIO& io = ImGui::GetIO();
            if (!io.KeyCtrl)
            {
                m_selectedIndices.clear();
            }

            // Check which rows intersect with the selection box
            for (int i = 0; i < m_itemBounds.size(); ++i)
            {
                const auto& [itemMin, itemMax] = m_itemBounds[i];

                // Check if row intersects with selection box (Y-axis overlap)
                bool intersects = !(itemMax.y < boxMin.y || itemMin.y > boxMax.y);

                if (intersects)
                {
                    m_selectedIndices.insert(i);
                }
            }
        }

        // End box selection on mouse release
        if (m_isBoxSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            // If we didn't drag (just clicked and released), clear selection
            if (!m_boxSelectDragged)
            {
                m_selectedIndices.clear();
            }
            m_isBoxSelecting = false;
        }

        // Draw selection box overlay
        if (m_isBoxSelecting && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec4 accentColor = GetAccentColor();
            ImVec4 fillColor = ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.2f);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(m_boxSelectStart, mousePos, ImGui::GetColorU32(fillColor));
            drawList->AddRect(m_boxSelectStart, mousePos, ImGui::GetColorU32(accentColor), 0.0f, 0, 2.0f);
        }

        // Right-click on empty space for background context menu
        // Only open if not box selecting
        if (!m_isBoxSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsItemHovered())
        {
            ImGui::OpenPopup("background_context_menu");
        }
    }

    // Background context menu (for empty space)
    if (ImGui::BeginPopup("background_context_menu"))
    {
        ImGui::TextDisabled("Current Folder");
        ImGui::Separator();

        if (ImGui::MenuItem("New Folder"))
        {
            m_showNewFolderDialog = true;
            strcpy_s(m_newFolderNameBuffer, "New Folder");
        }

        if (ImGui::MenuItem("New u.f.b. Folder"))
        {
            m_showNewUFBFolderDialog = true;
            memset(m_newUFBFolderNameBuffer, 0, sizeof(m_newUFBFolderNameBuffer));
        }

        if (ImGui::MenuItem("New Date Folder"))
        {
            CreateDateFolder();
        }

        if (ImGui::MenuItem("New Time Folder"))
        {
            CreateTimeFolder();
        }

        ImGui::Separator();

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

        if (ImGui::MenuItem("Refresh"))
        {
            RefreshFileList();
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

            // Build new pathgreat.
            std::filesystem::path originalPath(m_renameOriginalPath);
            std::filesystem::path newPath = originalPath.parent_path() / newNameW;

            // Attempt rename
            try
            {
                std::filesystem::rename(originalPath, newPath);
                RefreshFileList();
            }
            catch (const std::exception& e)
            {
                std::cerr << "Failed to rename: " << e.what() << std::endl;
            }

            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void FileBrowser::DrawGridView(HWND hwnd)
{
    // Create a snapshot of files to avoid holding lock during rendering
    std::vector<FileEntry> filesSnapshot;
    {
        std::lock_guard<std::mutex> lock(m_filesMutex);
        filesSnapshot = m_files;
    }

    // Calculate grid layout
    ImVec2 availableSize = ImGui::GetContentRegionAvail();
    float itemWidth = m_thumbnailSize + 20.0f;  // Thumbnail + padding
    float itemHeight = m_thumbnailSize + 40.0f; // Thumbnail + text
    int columnsPerRow = (std::max)(1, (int)(availableSize.x / itemWidth));

    // Track if any file was right-clicked (to prevent background menu from opening)
    bool fileRightClicked = false;

    // Clear and resize item bounds vector for box selection
    m_itemBounds.clear();
    m_itemBounds.resize(filesSnapshot.size());

    // Child window for scrolling
    ImGui::BeginChild("GridView", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    // Add sortable table header (looks identical to List mode headers)
    if (ImGui::BeginTable("GridViewHeader", 3,
        ImGuiTableFlags_Sortable | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
    {
        // Set up columns (same as List mode)
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();

        // Handle sorting (reuse exact same code from List mode)
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsDirty)
            {
                if (sortSpecs->SpecsCount > 0)
                {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                    m_sortColumn = static_cast<SortColumn>(spec.ColumnIndex);
                    m_sortAscending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                    std::lock_guard<std::mutex> lock(m_filesMutex);
                    SortFileList();
                }
                sortSpecs->SpecsDirty = false;
            }
        }

        ImGui::EndTable();
    }

    // Small spacing between header and grid
    ImGui::Spacing();

    // Calculate visible range for lazy loading (with generous buffer)
    float scrollY = ImGui::GetScrollY();
    float viewportHeight = ImGui::GetWindowHeight();

    // Calculate visible rows with 3-row buffer above and below
    int firstVisibleRow = (std::max)(0, (int)(scrollY / itemHeight) - 3);
    int lastVisibleRow = (int)((scrollY + viewportHeight) / itemHeight) + 3;

    // Convert to item indices
    int firstVisibleItem = firstVisibleRow * columnsPerRow;
    int lastVisibleItem = (std::min)((lastVisibleRow + 1) * columnsPerRow, (int)filesSnapshot.size());

    // Render files in grid
    for (int i = 0; i < filesSnapshot.size(); ++i)
    {
        const FileEntry& entry = filesSnapshot[i];

        ImGui::PushID(i);

        // Calculate position in grid
        if (i % columnsPerRow != 0)
            ImGui::SameLine();

        // Begin group for this item
        ImGui::BeginGroup();

        // Query thumbnail cache ONLY for visible items
        ImTextureID texture = 0;
        ImVec2 displaySize(m_thumbnailSize, m_thumbnailSize);
        ImVec2 padding(0, 0);

        bool isInVisibleRange = (i >= firstVisibleItem && i <= lastVisibleItem);

        if (!entry.isDirectory && isInVisibleRange)
        {
            // Only query cache for visible items
            int width = 0, height = 0;
            ImTextureID thumb = m_thumbnailManager.GetThumbnail(entry.fullPath, width, height);

            if (thumb)
            {
                // Found in cache - use thumbnail with aspect ratio
                texture = thumb;

                if (width > 0 && height > 0)
                {
                    float aspectRatio = (float)width / (float)height;

                    if (aspectRatio > 1.0f)
                    {
                        // Landscape: fit to width
                        displaySize.x = m_thumbnailSize;
                        displaySize.y = m_thumbnailSize / aspectRatio;
                        padding.y = (m_thumbnailSize - displaySize.y) * 0.5f;
                    }
                    else
                    {
                        // Portrait or square: fit to height
                        displaySize.y = m_thumbnailSize;
                        displaySize.x = m_thumbnailSize * aspectRatio;
                        padding.x = (m_thumbnailSize - displaySize.x) * 0.5f;
                    }
                }
            }
            else if (!m_thumbnailManager.IsLoading(entry.fullPath))
            {
                // Not in cache and not loading - request it
                m_thumbnailManager.RequestThumbnail(entry.fullPath, (int)m_thumbnailSize, false);
            }
        }

        // Fallback to icon if no thumbnail or not visible
        if (!texture)
        {
            int iconSize = (int)m_thumbnailSize;
            texture = m_iconManager.GetFileIcon(entry.fullPath, entry.isDirectory, iconSize);
        }

        // Draw thumbnail/icon with aspect ratio
        ImVec2 cursorPos = ImGui::GetCursorPos();

        // Add padding to center the thumbnail
        if (padding.x > 0 || padding.y > 0)
        {
            ImGui::SetCursorPos(ImVec2(cursorPos.x + padding.x, cursorPos.y + padding.y));
        }

        if (texture)
        {
            // Validate texture is still in cache before rendering to prevent crashes
            bool isValidTexture = false;

            if (!entry.isDirectory && isInVisibleRange)
            {
                // This was a thumbnail - validate it's still in thumbnail cache
                ImTextureID currentTexture = m_thumbnailManager.GetThumbnail(entry.fullPath);
                isValidTexture = (currentTexture == texture);
            }
            else
            {
                // This was an icon - validate it's still in icon cache
                int iconSize = (int)m_thumbnailSize;
                ImTextureID currentIcon = m_iconManager.GetFileIcon(entry.fullPath, entry.isDirectory, iconSize);
                isValidTexture = (currentIcon == texture);
            }

            if (isValidTexture)
            {
                ImGui::Image(texture, displaySize);
            }
            else
            {
                // Texture was invalidated - show placeholder
                ImGui::Dummy(displaySize);
            }
        }
        else
        {
            // Placeholder if no texture
            ImGui::Dummy(displaySize);
        }

        // Reset cursor to account for full square size (for layout purposes)
        if (padding.x > 0 || padding.y > 0)
        {
            ImVec2 afterImagePos = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(cursorPos.x, cursorPos.y + m_thumbnailSize));
        }

        // Drag source for file/folder (supports multi-select)
        static bool transitionedToOLEDrag_grid = false;

#if OLE_DRAG_IMMEDIATE_MODE
        // IMMEDIATE OLE DRAG MODE: Skip ImGui drag and go straight to Windows OLE
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            // Build list of file paths for drag operation
            std::vector<std::wstring> filePaths;

            // If this item is part of selection, drag all selected items
            if (m_selectedIndices.find(i) != m_selectedIndices.end())
            {
                for (int idx : m_selectedIndices)
                {
                    if (idx < m_files.size())
                    {
                        filePaths.push_back(m_files[idx].fullPath);
                    }
                }
            }
            else
            {
                // Not selected - drag just this one
                filePaths.push_back(entry.fullPath);
            }

            if (!filePaths.empty())
            {
                std::wcout << L"[FileBrowser Grid] Starting immediate Windows OLE drag (no ImGui transition)" << std::endl;
                // Start Windows native drag and drop (this will block until drag completes)
                StartWindowsDragDrop(filePaths);
            }
        }
#else
        // HYBRID DRAG MODE: Start with ImGui drag, transition to OLE when leaving window
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            if (!transitionedToOLEDrag_grid)
            {
                // Change cursor to indicate drag operation
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

                // Build list of file paths for drag operation
                std::vector<std::wstring> filePaths;
                std::string allPathsUtf8;

                // If this item is part of selection, drag all selected items
                if (m_selectedIndices.find(i) != m_selectedIndices.end())
                {
                    for (int idx : m_selectedIndices)
                    {
                        if (idx < m_files.size())
                        {
                            filePaths.push_back(m_files[idx].fullPath);

                            // Also build UTF-8 string for ImGui payload
                            std::wstring& selectedPath = m_files[idx].fullPath;
                            int utf8Size = WideCharToMultiByte(CP_UTF8, 0, selectedPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
                            if (utf8Size > 0)
                            {
                                std::string pathUtf8;
                                pathUtf8.resize(utf8Size);
                                WideCharToMultiByte(CP_UTF8, 0, selectedPath.c_str(), -1, &pathUtf8[0], utf8Size, nullptr, nullptr);
                                pathUtf8.resize(utf8Size - 1); // Remove null terminator
                                allPathsUtf8 += pathUtf8 + "\n";
                            }
                        }
                    }
                }
                else
                {
                    // Not selected - drag just this one
                    filePaths.push_back(entry.fullPath);

                    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, entry.fullPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    if (utf8Size > 0)
                    {
                        allPathsUtf8.resize(utf8Size);
                        WideCharToMultiByte(CP_UTF8, 0, entry.fullPath.c_str(), -1, &allPathsUtf8[0], utf8Size, nullptr, nullptr);
                    }
                }

                // Check if mouse has left the main window (HWND) - if so, start Windows OLE drag
                POINT cursorPos;
                GetCursorPos(&cursorPos);

                RECT windowRect;
                GetWindowRect(hwnd, &windowRect);

                bool mouseOutsideHWND = !PtInRect(&windowRect, cursorPos);

                if (mouseOutsideHWND && !filePaths.empty())
                {
                    std::wcout << L"[FileBrowser] Mouse left HWND during drag, starting Windows OLE drag" << std::endl;
                    transitionedToOLEDrag_grid = true;
                    ImGui::EndDragDropSource();

                    // Start Windows native drag and drop (this will block until drag completes)
                    StartWindowsDragDrop(filePaths);

                    // Reset flag immediately after OLE drag completes
                    transitionedToOLEDrag_grid = false;

                    // Don't continue with ImGui drag
                }
                else
            {
                // Set payload with all selected paths for internal ImGui drag
                ImGui::SetDragDropPayload("FILE_PATHS", allPathsUtf8.c_str(), allPathsUtf8.size() + 1);

                // Show enhanced preview with background
                ImGui::BeginTooltip();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

                int selectionCount = m_selectedIndices.size();
                if (selectionCount > 1)
                {
                    ImGui::Text("Dragging %d items", selectionCount);
                }
                else
                {
                    const char* icon = entry.isDirectory ? ICON_FOLDER : ICON_FILE;
                    char nameUtf8[512];
                    WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);

                    // Use icon font for icon, regular font for text
                    if (font_icons) ImGui::PushFont(font_icons);
                    ImGui::Text("%s", icon);
                    if (font_icons) ImGui::PopFont();

                    ImGui::SameLine();
                    ImGui::Text("%s", nameUtf8);
                }

                ImGui::PopStyleColor();
                ImGui::EndTooltip();

                ImGui::EndDragDropSource();
                }
            }
            else
            {
                // Already transitioned to OLE drag - just end the drag source
                ImGui::EndDragDropSource();
            }
        }
        else
        {
            // Drag ended - reset flag
            transitionedToOLEDrag_grid = false;
        }
#endif // OLE_DRAG_IMMEDIATE_MODE

        // Check if item is selected
        bool isSelected = (m_selectedIndices.find(i) != m_selectedIndices.end());

        // Draw filename (truncated to fit)
        char nameUtf8[256];
        WideCharToMultiByte(CP_UTF8, 0, entry.name.c_str(), -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);

        // Truncate filename if too long
        ImVec2 textSize = ImGui::CalcTextSize(nameUtf8);
        if (textSize.x > m_thumbnailSize)
        {
            std::string truncated = nameUtf8;
            while (ImGui::CalcTextSize((truncated + "...").c_str()).x > m_thumbnailSize && truncated.length() > 0)
            {
                truncated.pop_back();
            }
            truncated += "...";

            // Center text
            float textWidth = ImGui::CalcTextSize(truncated.c_str()).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (m_thumbnailSize - textWidth) * 0.5f);
            ImGui::TextUnformatted(truncated.c_str());
        }
        else
        {
            // Center text
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (m_thumbnailSize - textSize.x) * 0.5f);
            ImGui::TextUnformatted(nameUtf8);
        }

        ImGui::EndGroup();

        // Add invisible button over the entire group for better hover/click detection
        ImVec2 groupMin = ImGui::GetItemRectMin();
        ImVec2 groupMax = ImGui::GetItemRectMax();
        ImGui::SetCursorScreenPos(groupMin);
        ImGui::InvisibleButton(("##grid_item_" + std::to_string(i)).c_str(), ImVec2(groupMax.x - groupMin.x, groupMax.y - groupMin.y));

        // Show tooltip with full filename on hover
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(nameUtf8);
            ImGui::EndTooltip();
        }

        // Store item bounds for box selection
        m_itemBounds[i] = std::make_pair(groupMin, groupMax);

        // Handle left-click on invisible button
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            ImGuiIO& io = ImGui::GetIO();

            if (io.KeyCtrl)
            {
                // Ctrl+Click: Toggle selection
                if (isSelected)
                    m_selectedIndices.erase(i);
                else
                    m_selectedIndices.insert(i);
            }
            else if (io.KeyShift && m_lastClickedIndex >= 0)
            {
                // Shift+Click: Range select
                int start = (std::min)(m_lastClickedIndex, i);
                int end = (std::max)(m_lastClickedIndex, i);
                for (int idx = start; idx <= end; ++idx)
                {
                    m_selectedIndices.insert(idx);
                }
            }
            else
            {
                // Normal click
                // If clicking on already selected item, don't clear selection yet (might be starting a drag)
                // If clicking on unselected item, clear and select only this one
                if (!isSelected)
                {
                    m_selectedIndices.clear();
                    m_selectedIndices.insert(i);
                }
                // If already selected, selection will be cleared on mouse release (if not dragging)
            }

            // Double-click detection
            double currentTime = glfwGetTime();
            if (m_lastClickedIndex == i && (currentTime - m_lastClickTime) < 0.3)
            {
                // Double-clicked
                if (entry.isDirectory)
                {
                    NavigateTo(entry.fullPath);
                }
                else
                {
                    ShellExecuteW(nullptr, L"open", entry.fullPath.c_str(), nullptr, nullptr, SW_SHOW);
                }
            }
            m_lastClickTime = currentTime;
            m_lastClickedIndex = i;
        }

        // Handle mouse release on already selected item (if not dragging)
        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && isSelected)
        {
            ImGuiIO& io = ImGui::GetIO();
            // Only clear selection if we didn't start a drag, no modifiers, and not box selecting
            if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left, 5.0f) && !io.KeyCtrl && !io.KeyShift && !m_isBoxSelecting)
            {
                m_selectedIndices.clear();
                m_selectedIndices.insert(i);
            }
        }

        // Highlight selected item (after group so it covers the entire item)
        if (isSelected)
        {
            ImVec4 accentColor = GetAccentColor();

            // Draw filled background with accent color (semi-transparent)
            ImVec4 bgColor = ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.3f);
            ImGui::GetWindowDrawList()->AddRectFilled(groupMin, groupMax, ImGui::GetColorU32(bgColor));

            // Draw border around selected item
            ImGui::GetWindowDrawList()->AddRect(groupMin, groupMax, ImGui::GetColorU32(accentColor), 0.0f, 0, 2.0f);
        }

        // Right-click context menu (check on invisible button)
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            ImGui::OpenPopup("file_context_menu");
            fileRightClicked = true;  // Mark that a file was right-clicked

            // If right-clicked item is not in selection, select only it
            if (!isSelected)
            {
                m_selectedIndices.clear();
                m_selectedIndices.insert(i);
            }
        }

        // Render context menu
        ShowImGuiContextMenu(hwnd, entry);

        ImGui::PopID();
    }

    ImGui::EndChild();

    // Box selection logic (drag-to-select in grid view)
    // Detect box selection start - left-click on empty space
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !fileRightClicked)
    {
        // Check if click was on empty space (not on any item)
        bool clickedOnItem = false;
        ImVec2 mousePos = ImGui::GetMousePos();
        for (int i = 0; i < m_itemBounds.size(); ++i)
        {
            const auto& [itemMin, itemMax] = m_itemBounds[i];
            if (mousePos.x >= itemMin.x && mousePos.x <= itemMax.x &&
                mousePos.y >= itemMin.y && mousePos.y <= itemMax.y)
            {
                clickedOnItem = true;
                break;
            }
        }

        if (!clickedOnItem)
        {
            m_isBoxSelecting = true;
            m_boxSelectDragged = false;  // Reset drag flag
            m_boxSelectStart = mousePos;
        }
    }

    // Update selection during drag
    if (m_isBoxSelecting && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        m_boxSelectDragged = true;  // Mark that we dragged

        ImVec2 mousePos = ImGui::GetMousePos();

        // Calculate selection box bounds
        ImVec2 boxMin(
            (std::min)(m_boxSelectStart.x, mousePos.x),
            (std::min)(m_boxSelectStart.y, mousePos.y)
        );
        ImVec2 boxMax(
            (std::max)(m_boxSelectStart.x, mousePos.x),
            (std::max)(m_boxSelectStart.y, mousePos.y)
        );

        // Update selection based on modifier keys
        ImGuiIO& io = ImGui::GetIO();
        if (!io.KeyCtrl)
        {
            // Normal drag: replace selection
            m_selectedIndices.clear();
        }

        // Check which items intersect with selection box
        for (int i = 0; i < m_itemBounds.size(); ++i)
        {
            const auto& [itemMin, itemMax] = m_itemBounds[i];

            // Rectangle intersection test
            bool intersects = !(itemMax.x < boxMin.x || itemMin.x > boxMax.x ||
                               itemMax.y < boxMin.y || itemMin.y > boxMax.y);

            if (intersects)
            {
                m_selectedIndices.insert(i);
            }
        }
    }

    // End box selection on mouse release
    if (m_isBoxSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        // If we didn't drag (just clicked and released), clear selection
        if (!m_boxSelectDragged)
        {
            m_selectedIndices.clear();
        }
        m_isBoxSelecting = false;
    }

    // Draw selection box overlay
    if (m_isBoxSelecting && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec4 accentColor = GetAccentColor();

        // Semi-transparent fill
        ImVec4 fillColor = ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.2f);
        ImU32 fillColorU32 = ImGui::GetColorU32(fillColor);

        // Solid border
        ImU32 borderColorU32 = ImGui::GetColorU32(accentColor);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(m_boxSelectStart, mousePos, fillColorU32);
        drawList->AddRect(m_boxSelectStart, mousePos, borderColorU32, 0.0f, 0, 2.0f);
    }

    // Background context menu (right-click on empty space in grid)
    // Only open if no file was right-clicked and not box selecting
    if (!fileRightClicked && !m_isBoxSelecting && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        ImGui::OpenPopup("background_context_menu");
    }

    // Background context menu (for empty space)
    if (ImGui::BeginPopup("background_context_menu"))
    {
        ImGui::TextDisabled("Current Folder");
        ImGui::Separator();

        if (ImGui::MenuItem("New Folder"))
        {
            m_showNewFolderDialog = true;
            strcpy_s(m_newFolderNameBuffer, "New Folder");
        }

        if (ImGui::MenuItem("New u.f.b. Folder"))
        {
            m_showNewUFBFolderDialog = true;
            memset(m_newUFBFolderNameBuffer, 0, sizeof(m_newUFBFolderNameBuffer));
        }

        if (ImGui::MenuItem("New Date Folder"))
        {
            CreateDateFolder();
        }

        if (ImGui::MenuItem("New Time Folder"))
        {
            CreateTimeFolder();
        }

        ImGui::Separator();

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

        if (ImGui::MenuItem("Refresh"))
        {
            RefreshFileList();
        }

        ImGui::EndPopup();
    }

    // Rename dialog modal (shared with list view)
    if (m_showRenameDialog)
    {
        ImGui::OpenPopup("Rename");
        m_showRenameDialog = false;
    }

    if (ImGui::BeginPopupModal("Rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Enter new name:");
        ImGui::SetNextItemWidth(300.0f);

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

        if (doRename)
        {
            wchar_t newNameW[256];
            MultiByteToWideChar(CP_UTF8, 0, m_renameBuffer, -1, newNameW, 256);

            std::filesystem::path originalPath(m_renameOriginalPath);
            std::filesystem::path newPath = originalPath.parent_path() / newNameW;

            try
            {
                std::filesystem::rename(originalPath, newPath);
                RefreshFileList();
            }
            catch (const std::exception& e)
            {
                std::cerr << "Failed to rename: " << e.what() << std::endl;
            }

            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

std::string FileBrowser::FormatFileSize(uintmax_t size)
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double displaySize = (double)size;

    while (displaySize >= 1024.0 && unitIndex < 4)
    {
        displaySize /= 1024.0;
        unitIndex++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << displaySize << " " << units[unitIndex];
    return oss.str();
}

std::string FileBrowser::FormatFileTime(const std::filesystem::file_time_type& ftime)
{
    try
    {
        // Convert file_time_type to system_clock time_point
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
        );

        // Convert to time_t
        auto tt = std::chrono::system_clock::to_time_t(sctp);

        // Format the time
        std::tm tm_local;
        localtime_s(&tm_local, &tt);

        std::ostringstream oss;
        oss << std::put_time(&tm_local, "%Y-%m-%d %H:%M");
        return oss.str();
    }
    catch (...)
    {
        return "---";
    }
}

void FileBrowser::SortFileList()
{
    // NOTE: Caller must hold m_filesMutex lock before calling this method
    std::sort(m_files.begin(), m_files.end(), [this](const FileEntry& a, const FileEntry& b) -> bool {
        // Always keep directories separate from files
        if (a.isDirectory != b.isDirectory)
        {
            return a.isDirectory; // Directories first
        }

        // Sort by the selected column
        bool result = false;
        switch (m_sortColumn)
        {
        case SortColumn::Name:
            {
                // Case-insensitive comparison
                std::wstring aName = a.name;
                std::wstring bName = b.name;
                std::transform(aName.begin(), aName.end(), aName.begin(), ::towlower);
                std::transform(bName.begin(), bName.end(), bName.begin(), ::towlower);
                result = aName < bName;
            }
            break;

        case SortColumn::Size:
            result = a.size < b.size;
            break;

        case SortColumn::Modified:
            result = a.lastModified < b.lastModified;
            break;
        }

        // Apply sort direction
        return m_sortAscending ? result : !result;
    });
}

bool FileBrowser::GetWindowBounds(ImVec2& outPos, ImVec2& outSize) const
{
    outPos = m_windowPos;
    outSize = m_windowSize;
    return (m_windowSize.x > 0 && m_windowSize.y > 0);
}

void FileBrowser::HandleExternalDrop(const std::vector<std::wstring>& droppedPaths)
{
    if (droppedPaths.empty())
        return;

    std::wcout << L"[FileBrowser] Handling external drop of " << droppedPaths.size() << L" item(s) into: " << m_currentDirectory << std::endl;

    // Copy dropped files to current directory
    CopyFilesToDestination(droppedPaths, m_currentDirectory);

    // Refresh file list to show newly copied files
    RefreshFileList();
}

// Create a new u.f.b. folder with YYMMDDx_{Name} versioning
bool FileBrowser::CreateUFBFolder(const std::string& folderName)
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

            // Check if any folder in current directory starts with this prefix
            for (const auto& entry : m_files)
            {
                if (!entry.isDirectory)
                    continue;

                std::filesystem::path entryPath(entry.fullPath);
                std::string folderNameStr = entryPath.filename().string();

                if (folderNameStr.substr(0, testFolderName.length()) == testFolderName)
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
            std::cerr << "[FileBrowser] No available letter suffix for date: " << datePrefix << std::endl;
            return false;
        }

        // Create folder name: YYMMDDx_{FolderName}
        std::string newFolderName = std::string(datePrefix) + letter + "_" + folderName;

        // Convert to wide string
        wchar_t newFolderNameW[512];
        MultiByteToWideChar(CP_UTF8, 0, newFolderName.c_str(), -1, newFolderNameW, 512);
        std::wstring newFolderNameWide(newFolderNameW);

        // Create full path
        std::filesystem::path newFolderPath = std::filesystem::path(m_currentDirectory) / newFolderNameWide;

        if (std::filesystem::exists(newFolderPath))
        {
            std::cerr << "[FileBrowser] Folder already exists: " << newFolderName << std::endl;
            return false;
        }

        // Create the directory
        std::filesystem::create_directory(newFolderPath);
        std::cout << "[FileBrowser] Created u.f.b. folder: " << newFolderPath << std::endl;

        // Refresh the file list
        RefreshFileList();

        std::cout << "[FileBrowser] Successfully created u.f.b. folder: " << newFolderName << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FileBrowser] Error creating u.f.b. folder: " << e.what() << std::endl;
        return false;
    }
}

// Create a new date folder with YYMMDD format
bool FileBrowser::CreateDateFolder()
{
    try
    {
        // Get current date in YYMMDD format
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        struct tm tm_now;
        localtime_s(&tm_now, &time_t_now);

        char dateFolderName[7];
        snprintf(dateFolderName, sizeof(dateFolderName), "%02d%02d%02d",
                 tm_now.tm_year % 100, tm_now.tm_mon + 1, tm_now.tm_mday);

        // Convert to wide string
        wchar_t dateFolderNameW[256];
        MultiByteToWideChar(CP_UTF8, 0, dateFolderName, -1, dateFolderNameW, 256);
        std::wstring dateFolderNameWide(dateFolderNameW);

        // Create full path
        std::filesystem::path newFolderPath = std::filesystem::path(m_currentDirectory) / dateFolderNameWide;

        if (std::filesystem::exists(newFolderPath))
        {
            std::cerr << "[FileBrowser] Date folder already exists: " << dateFolderName << std::endl;
            return false;
        }

        // Create the directory
        std::filesystem::create_directory(newFolderPath);
        std::cout << "[FileBrowser] Created date folder: " << newFolderPath << std::endl;

        // Refresh the file list
        RefreshFileList();

        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FileBrowser] Error creating date folder: " << e.what() << std::endl;
        return false;
    }
}

// Create a new time folder with HHMM format
bool FileBrowser::CreateTimeFolder()
{
    try
    {
        // Get current time in HHMM format
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        struct tm tm_now;
        localtime_s(&tm_now, &time_t_now);

        char timeFolderName[5];
        snprintf(timeFolderName, sizeof(timeFolderName), "%02d%02d",
                 tm_now.tm_hour, tm_now.tm_min);

        // Convert to wide string
        wchar_t timeFolderNameW[256];
        MultiByteToWideChar(CP_UTF8, 0, timeFolderName, -1, timeFolderNameW, 256);
        std::wstring timeFolderNameWide(timeFolderNameW);

        // Create full path
        std::filesystem::path newFolderPath = std::filesystem::path(m_currentDirectory) / timeFolderNameWide;

        if (std::filesystem::exists(newFolderPath))
        {
            std::cerr << "[FileBrowser] Time folder already exists: " << timeFolderName << std::endl;
            return false;
        }

        // Create the directory
        std::filesystem::create_directory(newFolderPath);
        std::cout << "[FileBrowser] Created time folder: " << newFolderPath << std::endl;

        // Refresh the file list
        RefreshFileList();

        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FileBrowser] Error creating time folder: " << e.what() << std::endl;
        return false;
    }
}

void FileBrowser::CreateJobFromTemplate(const std::string& jobNumber, const std::string& jobName)
{
    try
    {
        // Build folder name: {number}_{name} (lowercase, underscores for spaces)
        std::string folderName = jobNumber + "_" + jobName;

        // Convert to lowercase
        std::transform(folderName.begin(), folderName.end(), folderName.begin(),
            [](unsigned char c) { return std::tolower(c); });

        // Replace spaces with underscores
        std::replace(folderName.begin(), folderName.end(), ' ', '_');

        // Build destination path
        std::wstring wideFolderName = UFB::Utf8ToWide(folderName);
        std::filesystem::path destPath = std::filesystem::path(m_currentDirectory) / wideFolderName;

        // Check if folder already exists
        if (std::filesystem::exists(destPath))
        {
            std::cerr << "[FileBrowser] Job folder already exists: " << folderName << std::endl;
            return;
        }

        // Get executable directory to find template
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        std::filesystem::path templatePath = exeDir / "assets" / "projectTemplate";

        // Check if template exists
        if (!std::filesystem::exists(templatePath))
        {
            std::cerr << "[FileBrowser] Template not found: " << templatePath << std::endl;
            return;
        }

        std::cout << "[FileBrowser] Creating job from template: " << templatePath << std::endl;

        // Create the job folder
        std::filesystem::create_directory(destPath);

        // Copy template contents recursively
        for (const auto& entry : std::filesystem::directory_iterator(templatePath))
        {
            std::filesystem::path sourcePath = entry.path();
            std::filesystem::path targetPath = destPath / sourcePath.filename();

            // Copy recursively
            std::filesystem::copy(sourcePath, targetPath,
                std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::overwrite_existing);

            std::cout << "[FileBrowser] Copied: " << sourcePath.filename() << std::endl;
        }

        // Rename any folders/files containing _t_project_name to the actual job name
        std::wstring templateMarker = L"_t_project_name";
        std::wstring replacement = UFB::Utf8ToWide(folderName);

        for (const auto& entry : std::filesystem::recursive_directory_iterator(destPath))
        {
            std::filesystem::path oldPath = entry.path();
            std::wstring oldName = oldPath.filename().wstring();

            // Check if filename contains the template marker
            size_t pos = oldName.find(templateMarker);
            if (pos != std::wstring::npos)
            {
                // Replace template marker with actual job name
                std::wstring newName = oldName;
                newName.replace(pos, templateMarker.length(), replacement);

                // Build new path
                std::filesystem::path newPath = oldPath.parent_path() / newName;

                // Rename
                std::filesystem::rename(oldPath, newPath);
                std::cout << "[FileBrowser] Renamed: " << UFB::WideToUtf8(oldName) << " -> " << UFB::WideToUtf8(newName) << std::endl;
            }
        }

        std::cout << "[FileBrowser] Created job folder: " << folderName << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FileBrowser] Error creating job from template: " << e.what() << std::endl;
    }
}

void FileBrowser::ExecuteSearch(const std::string& query)
{
    if (query.empty())
        return;

    std::cout << "[FileBrowser] Executing search for: " << query << std::endl;

    // Save current directory to return to later
    m_preSearchDirectory = m_currentDirectory;

    // Add current directory to back history so user can navigate back
    if (!m_currentDirectory.empty())
    {
        m_backHistory.push_back(m_currentDirectory);
        // Clear forward history when entering search (similar to navigating to new location)
        m_forwardHistory.clear();
    }

    // Build es.exe command
    // Use -path to limit search to current directory recursively
    // Use -csv for easy parsing
    std::string pathUtf8 = UFB::WideToUtf8(m_currentDirectory);
    std::string command = "es.exe \"" + query + "\" -path \"" + pathUtf8 + "\" -csv -n 1000";

    std::cout << "[FileBrowser] Command: " << command << std::endl;

    // Execute command and capture output
    std::string output;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hRead, hWrite;

    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
    {
        std::cerr << "[FileBrowser] Failed to create pipe" << std::endl;
        return;
    }

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;

    if (CreateProcessA(NULL, const_cast<char*>(command.c_str()), NULL, NULL, TRUE,
                       0, NULL, NULL, &si, &pi))
    {
        CloseHandle(hWrite);

        // Read output
        char buffer[4096];
        DWORD bytesRead;

        while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
        {
            buffer[bytesRead] = '\0';
            output += buffer;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hRead);
    }
    else
    {
        CloseHandle(hWrite);
        CloseHandle(hRead);
        std::cerr << "[FileBrowser] Failed to execute es.exe" << std::endl;
        return;
    }

    // Parse CSV output (format: Filename)
    // First line is header, skip it
    std::lock_guard<std::mutex> lock(m_filesMutex);
    m_files.clear();
    std::istringstream stream(output);
    std::string line;
    bool firstLine = true;

    std::cout << "[FileBrowser] Raw output length: " << output.length() << " bytes" << std::endl;

    while (std::getline(stream, line))
    {
        // Remove trailing carriage return if present (Windows line endings)
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (firstLine)
        {
            firstLine = false;
            continue;  // Skip header
        }

        if (line.empty())
            continue;

        // Remove quotes if present
        if (!line.empty() && line.front() == '\"' && line.back() == '\"')
        {
            line = line.substr(1, line.length() - 2);
        }

        std::cout << "[FileBrowser] Processing line: " << line << std::endl;

        // Convert to wstring
        int wchars_num = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, NULL, 0);
        std::wstring wpath(wchars_num, 0);
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, &wpath[0], wchars_num);
        wpath.resize(wchars_num - 1);  // Remove null terminator

        // Create FileEntry
        try
        {
            if (std::filesystem::exists(wpath))
            {
                FileEntry entry;
                entry.fullPath = wpath;
                entry.name = std::filesystem::path(wpath).filename().wstring();
                entry.isDirectory = std::filesystem::is_directory(wpath);
                entry.size = entry.isDirectory ? 0 : std::filesystem::file_size(wpath);
                entry.lastModified = std::filesystem::last_write_time(wpath);

                m_files.push_back(entry);
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[FileBrowser] Error processing file: " << e.what() << std::endl;
        }
    }

    // Update search state
    m_isSearchMode = true;
    m_searchResultCount = static_cast<int>(m_files.size());
    m_selectedIndices.clear();

    // Sort results
    SortFileList();

    std::cout << "[FileBrowser] Search completed: " << m_searchResultCount << " results" << std::endl;

    // Check if searching on network drive and warn user
    if (m_searchResultCount == 0 && m_preSearchDirectory.size() >= 2)
    {
        // Check if it's a network path (starts with \\ or is mapped drive pointing to network)
        if (m_preSearchDirectory[0] == L'\\' && m_preSearchDirectory[1] == L'\\')
        {
            std::cerr << "[FileBrowser] Warning: Searching on network path. Everything may not index network drives by default." << std::endl;
            std::cerr << "[FileBrowser] Network path: " << UFB::WideToUtf8(m_preSearchDirectory) << std::endl;
        }
    }
}

void FileBrowser::ExitSearchMode()
{
    std::cout << "[FileBrowser] Exiting search mode" << std::endl;

    m_isSearchMode = false;
    m_searchResultCount = 0;
    m_searchQuery[0] = '\0';  // Clear search query

    // Return to pre-search directory
    if (!m_preSearchDirectory.empty())
    {
        SetCurrentDirectory(m_preSearchDirectory);
        m_preSearchDirectory.clear();
    }
    else
    {
        // Fallback: just refresh current directory
        RefreshFileList();
    }
}

void FileBrowser::ShowInBrowser(const std::wstring& filePath)
{
    std::cout << "[FileBrowser] ShowInBrowser: " << UFB::WideToUtf8(filePath) << std::endl;

    try
    {
        std::filesystem::path path(filePath);

        // Get parent directory
        std::wstring parentDir = path.parent_path().wstring();

        // Exit search mode first
        if (m_isSearchMode)
        {
            ExitSearchMode();
        }

        // Navigate to parent directory
        SetCurrentDirectory(parentDir);

        // Find and select the file in the list
        for (size_t i = 0; i < m_files.size(); ++i)
        {
            if (m_files[i].fullPath == filePath)
            {
                m_selectedIndices.clear();
                m_selectedIndices.insert(static_cast<int>(i));
                std::cout << "[FileBrowser] Selected file at index " << i << std::endl;
                break;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FileBrowser] Error in ShowInBrowser: " << e.what() << std::endl;
    }
}
