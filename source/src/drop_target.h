#pragma once

#include <windows.h>
#include <ole2.h>
#include <vector>
#include <string>
#include <memory>

// Forward declarations
class FileBrowser;
class AssetsView;
class PostingsView;

// Simple IDropTarget implementation that auto-activates the window on drag
// This allows drag-drop to work from unfocused apps by giving UFB focus
class DropTarget : public IDropTarget
{
public:
    DropTarget(
        HWND hwnd,
        FileBrowser* browser1,
        FileBrowser* browser2,
        std::vector<std::unique_ptr<AssetsView>>* assetsViews,
        std::vector<std::unique_ptr<PostingsView>>* postingsViews,
        std::vector<std::unique_ptr<FileBrowser>>* standaloneBrowsers
    );
    ~DropTarget();

    // IUnknown methods
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IDropTarget methods
    STDMETHODIMP DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
    STDMETHODIMP DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
    STDMETHODIMP DragLeave() override;
    STDMETHODIMP Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;

private:
    LONG m_refCount;
    HWND m_hwnd;

    // References to browser panels for routing drops
    FileBrowser* m_browser1;
    FileBrowser* m_browser2;
    std::vector<std::unique_ptr<AssetsView>>* m_assetsViews;
    std::vector<std::unique_ptr<PostingsView>>* m_postingsViews;
    std::vector<std::unique_ptr<FileBrowser>>* m_standaloneBrowsers;

    // Helper methods
    bool ExtractFilePaths(IDataObject* pDataObj, std::vector<std::wstring>& outPaths);
    void RouteDropToPanel(const std::vector<std::wstring>& droppedPaths);
};
