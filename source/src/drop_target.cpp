#include "drop_target.h"
#include "file_browser.h"
#include "assets_view.h"
#include "postings_view.h"
#include <shellapi.h>
#include <iostream>

// ============================================================================
// DROPTARGET IMPLEMENTATION
// ============================================================================

DropTarget::DropTarget(
    HWND hwnd,
    FileBrowser* browser1,
    FileBrowser* browser2,
    std::vector<std::unique_ptr<AssetsView>>* assetsViews,
    std::vector<std::unique_ptr<PostingsView>>* postingsViews,
    std::vector<std::unique_ptr<FileBrowser>>* standaloneBrowsers
)
    : m_refCount(1)
    , m_hwnd(hwnd)
    , m_browser1(browser1)
    , m_browser2(browser2)
    , m_assetsViews(assetsViews)
    , m_postingsViews(postingsViews)
    , m_standaloneBrowsers(standaloneBrowsers)
{
    std::cout << "[DropTarget] Created" << std::endl;
}

DropTarget::~DropTarget()
{
    std::cout << "[DropTarget] Destroyed" << std::endl;
}

// ============================================================================
// IUnknown METHODS
// ============================================================================

STDMETHODIMP DropTarget::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_INVALIDARG;

    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IDropTarget)
    {
        *ppv = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DropTarget::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) DropTarget::Release()
{
    LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0)
    {
        delete this;
        return 0;
    }
    return count;
}

// ============================================================================
// IDropTarget METHODS
// ============================================================================

STDMETHODIMP DropTarget::DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)
{
    if (!pDataObj || !pdwEffect)
        return E_INVALIDARG;

    std::cout << "[DropTarget] DragEnter - activating window to enable drop" << std::endl;

    // Activate the window so ImGui updates hover states correctly
    // This is what most professional apps do (VS Code, Photoshop, etc.)
    SetForegroundWindow(m_hwnd);

    // Check if the data object contains file drop format
    FORMATETC fmtetc = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    if (pDataObj->QueryGetData(&fmtetc) == S_OK)
    {
        *pdwEffect = DROPEFFECT_COPY;
    }
    else
    {
        *pdwEffect = DROPEFFECT_NONE;
    }

    return S_OK;
}

STDMETHODIMP DropTarget::DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)
{
    if (!pdwEffect)
        return E_INVALIDARG;

    *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}

STDMETHODIMP DropTarget::DragLeave()
{
    std::cout << "[DropTarget] DragLeave" << std::endl;
    return S_OK;
}

STDMETHODIMP DropTarget::Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)
{
    if (!pDataObj || !pdwEffect)
        return E_INVALIDARG;

    std::cout << "[DropTarget] Drop" << std::endl;

    // Extract file paths from data object
    std::vector<std::wstring> droppedPaths;
    if (ExtractFilePaths(pDataObj, droppedPaths))
    {
        std::wcout << L"[DropTarget] Extracted " << droppedPaths.size() << L" file(s)" << std::endl;

        // Route the drop to the appropriate panel using hover state
        // (window now has focus, so hover state works correctly)
        RouteDropToPanel(droppedPaths);

        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }
    else
    {
        std::cout << "[DropTarget] Failed to extract file paths" << std::endl;
        *pdwEffect = DROPEFFECT_NONE;
        return E_FAIL;
    }
}

// ============================================================================
// HELPER METHODS
// ============================================================================

bool DropTarget::ExtractFilePaths(IDataObject* pDataObj, std::vector<std::wstring>& outPaths)
{
    outPaths.clear();

    // Request CF_HDROP format
    FORMATETC fmtetc = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stgmed = {};

    if (pDataObj->GetData(&fmtetc, &stgmed) != S_OK)
    {
        return false;
    }

    // Lock the global memory
    HDROP hDrop = static_cast<HDROP>(GlobalLock(stgmed.hGlobal));
    if (!hDrop)
    {
        ReleaseStgMedium(&stgmed);
        return false;
    }

    // Get the number of files
    UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);

    // Extract each file path
    for (UINT i = 0; i < fileCount; i++)
    {
        UINT pathLen = DragQueryFileW(hDrop, i, nullptr, 0);
        if (pathLen > 0)
        {
            std::wstring path(pathLen, L'\0');
            DragQueryFileW(hDrop, i, &path[0], pathLen + 1);
            path.resize(pathLen);
            outPaths.push_back(path);
        }
    }

    // Cleanup
    GlobalUnlock(stgmed.hGlobal);
    ReleaseStgMedium(&stgmed);

    return !outPaths.empty();
}

void DropTarget::RouteDropToPanel(const std::vector<std::wstring>& droppedPaths)
{
    if (droppedPaths.empty())
        return;

    // Use hover state to determine drop target
    // (same logic as GLFW callback in main.cpp - now works because window has focus)

    // Check browsers first
    if (m_browser2 && m_browser2->IsHovered())
    {
        std::cout << "[DropTarget] Routing to Browser 2" << std::endl;
        m_browser2->HandleExternalDrop(droppedPaths);
        return;
    }
    if (m_browser1 && m_browser1->IsHovered())
    {
        std::cout << "[DropTarget] Routing to Browser 1" << std::endl;
        m_browser1->HandleExternalDrop(droppedPaths);
        return;
    }

    // Check assets views
    if (m_assetsViews) {
        for (const auto& assetsView : *m_assetsViews) {
            if (assetsView && assetsView->IsBrowserHovered()) {
                std::wcout << L"[DropTarget] Routing to Assets View: " << assetsView->GetJobName() << std::endl;
                assetsView->HandleExternalDrop(droppedPaths);
                return;
            }
        }
    }

    // Check postings views
    if (m_postingsViews) {
        for (const auto& postingsView : *m_postingsViews) {
            if (postingsView && postingsView->IsBrowserHovered()) {
                std::wcout << L"[DropTarget] Routing to Postings View: " << postingsView->GetJobName() << std::endl;
                postingsView->HandleExternalDrop(droppedPaths);
                return;
            }
        }
    }

    // Check standalone browser windows
    if (m_standaloneBrowsers) {
        for (size_t i = 0; i < m_standaloneBrowsers->size(); i++) {
            const auto& standaloneBrowser = (*m_standaloneBrowsers)[i];
            if (standaloneBrowser && standaloneBrowser->IsHovered()) {
                std::cout << "[DropTarget] Routing to Standalone Browser " << (i + 1) << std::endl;
                standaloneBrowser->HandleExternalDrop(droppedPaths);
                return;
            }
        }
    }

    std::cout << "[DropTarget] Drop ignored (no target browser hovered)" << std::endl;
}
