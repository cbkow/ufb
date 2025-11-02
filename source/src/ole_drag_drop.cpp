#include "ole_drag_drop.h"
#include <iostream>
#include <shlobj.h>

//=============================================================================
// DropSource Implementation
//=============================================================================

DropSource::DropSource() : m_refCount(1)
{
}

STDMETHODIMP DropSource::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_INVALIDARG;

    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IDropSource)
    {
        *ppv = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DropSource::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) DropSource::Release()
{
    LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0)
    {
        delete this;
        return 0;
    }
    return count;
}

STDMETHODIMP DropSource::QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState)
{
    // Cancel if escape was pressed
    if (fEscapePressed)
        return DRAGDROP_S_CANCEL;

    // Check if the left mouse button is still down
    if (!(grfKeyState & MK_LBUTTON))
    {
        // Mouse button released - drop the data
        return DRAGDROP_S_DROP;
    }

    // Continue dragging
    return S_OK;
}

STDMETHODIMP DropSource::GiveFeedback(DWORD dwEffect)
{
    // Use default drag cursors
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

//=============================================================================
// FileDataObject Implementation
//=============================================================================

FileDataObject::FileDataObject(const std::vector<std::wstring>& filePaths)
    : m_refCount(1), m_filePaths(filePaths), m_hGlobal(nullptr)
{
    // Register the CFSTR_SHELLIDLIST clipboard format
    m_cfShellIDList = RegisterClipboardFormat(CFSTR_SHELLIDLIST);
    CreateHDROP();
}

FileDataObject::~FileDataObject()
{
    if (m_hGlobal)
    {
        GlobalFree(m_hGlobal);
        m_hGlobal = nullptr;
    }
}

STDMETHODIMP FileDataObject::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_INVALIDARG;

    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IDataObject)
    {
        *ppv = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) FileDataObject::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) FileDataObject::Release()
{
    LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0)
    {
        delete this;
        return 0;
    }
    return count;
}

void FileDataObject::CreateHDROP()
{
    if (m_filePaths.empty())
        return;

    // Calculate total size needed for DROPFILES structure + file paths
    size_t totalSize = sizeof(DROPFILES);

    for (const auto& path : m_filePaths)
    {
        totalSize += (path.length() + 1) * sizeof(wchar_t);
    }
    totalSize += sizeof(wchar_t); // Final null terminator

    // Allocate global memory
    m_hGlobal = GlobalAlloc(GHND, totalSize);
    if (!m_hGlobal)
    {
        std::cerr << "[OLE] Failed to allocate global memory for HDROP" << std::endl;
        return;
    }

    // Lock the memory and fill it
    DROPFILES* pDropFiles = (DROPFILES*)GlobalLock(m_hGlobal);
    if (!pDropFiles)
    {
        GlobalFree(m_hGlobal);
        m_hGlobal = nullptr;
        return;
    }

    // Fill DROPFILES structure
    pDropFiles->pFiles = sizeof(DROPFILES);
    pDropFiles->pt.x = 0;
    pDropFiles->pt.y = 0;
    pDropFiles->fNC = FALSE;
    pDropFiles->fWide = TRUE; // Unicode strings

    // Copy file paths
    wchar_t* pData = (wchar_t*)((BYTE*)pDropFiles + sizeof(DROPFILES));
    for (const auto& path : m_filePaths)
    {
        size_t len = path.length();
        wcscpy_s(pData, len + 1, path.c_str());
        pData += len + 1;
    }
    *pData = L'\0'; // Final null terminator

    GlobalUnlock(m_hGlobal);

    std::wcout << L"[OLE] Created HDROP with " << m_filePaths.size() << L" file(s)" << std::endl;
}

STDMETHODIMP FileDataObject::GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium)
{
    if (!pformatetcIn || !pmedium)
        return E_INVALIDARG;

    if (!(pformatetcIn->tymed & TYMED_HGLOBAL))
        return DV_E_TYMED;

    // Handle CF_HDROP format
    if (pformatetcIn->cfFormat == CF_HDROP)
    {
        if (!m_hGlobal)
            return E_UNEXPECTED;

        // Duplicate the global memory
        SIZE_T size = GlobalSize(m_hGlobal);
        HGLOBAL hCopy = GlobalAlloc(GHND, size);
        if (!hCopy)
            return E_OUTOFMEMORY;

        void* pSrc = GlobalLock(m_hGlobal);
        void* pDst = GlobalLock(hCopy);
        if (pSrc && pDst)
        {
            memcpy(pDst, pSrc, size);
        }
        GlobalUnlock(m_hGlobal);
        GlobalUnlock(hCopy);

        // Fill the storage medium
        pmedium->tymed = TYMED_HGLOBAL;
        pmedium->hGlobal = hCopy;
        pmedium->pUnkForRelease = nullptr;

        return S_OK;
    }
    // Handle CFSTR_SHELLIDLIST format (required for Windows Explorer)
    else if (pformatetcIn->cfFormat == m_cfShellIDList)
    {
        std::wcout << L"[OLE] Creating CFSTR_SHELLIDLIST for Windows Explorer" << std::endl;

        // Create PIDLs for each file path
        std::vector<LPITEMIDLIST> pidls;
        pidls.reserve(m_filePaths.size());

        for (const auto& path : m_filePaths)
        {
            LPITEMIDLIST pidl = ILCreateFromPathW(path.c_str());
            if (pidl)
            {
                pidls.push_back(pidl);
            }
            else
            {
                std::wcerr << L"[OLE] Failed to create PIDL for: " << path << std::endl;
                // Clean up previously created PIDLs
                for (auto p : pidls)
                    ILFree(p);
                return E_FAIL;
            }
        }

        // Calculate size needed for CIDA structure
        // CIDA has: UINT cidl + (cidl+1) UINT offsets + actual PIDL data
        size_t cidaSize = sizeof(UINT) + (pidls.size() + 1) * sizeof(UINT);

        // Add empty PIDL size for parent folder (just the terminator)
        const UINT emptyPidlSize = sizeof(USHORT);
        cidaSize += emptyPidlSize;

        // Add size of each PIDL
        std::vector<UINT> pidlSizes;
        pidlSizes.reserve(pidls.size());
        for (auto pidl : pidls)
        {
            UINT pidlSize = ILGetSize(pidl);
            pidlSizes.push_back(pidlSize);
            cidaSize += pidlSize;
        }

        // Allocate global memory for CIDA
        HGLOBAL hGlobal = GlobalAlloc(GHND, cidaSize);
        if (!hGlobal)
        {
            for (auto pidl : pidls)
                ILFree(pidl);
            return E_OUTOFMEMORY;
        }

        // Lock and fill the CIDA structure
        BYTE* pData = (BYTE*)GlobalLock(hGlobal);
        if (!pData)
        {
            GlobalFree(hGlobal);
            for (auto pidl : pidls)
                ILFree(pidl);
            return E_OUTOFMEMORY;
        }

        // Fill CIDA structure
        UINT* pCida = (UINT*)pData;
        pCida[0] = (UINT)pidls.size();  // Number of PIDLs

        // Calculate offsets
        UINT currentOffset = sizeof(UINT) + (pidls.size() + 1) * sizeof(UINT);

        // First offset is for parent folder - use empty PIDL for absolute paths
        pCida[1] = currentOffset;
        BYTE* pPidlData = pData + currentOffset;
        *(USHORT*)pPidlData = 0;  // Empty PIDL (just terminator)
        pPidlData += emptyPidlSize;
        currentOffset += emptyPidlSize;

        // Copy each PIDL and set its offset
        for (size_t i = 0; i < pidls.size(); ++i)
        {
            pCida[i + 2] = currentOffset;
            memcpy(pPidlData, pidls[i], pidlSizes[i]);
            pPidlData += pidlSizes[i];
            currentOffset += pidlSizes[i];
        }

        GlobalUnlock(hGlobal);

        // Free the PIDLs (we've copied them into the global memory)
        for (auto pidl : pidls)
            ILFree(pidl);

        // Fill the storage medium
        pmedium->tymed = TYMED_HGLOBAL;
        pmedium->hGlobal = hGlobal;
        pmedium->pUnkForRelease = nullptr;

        std::wcout << L"[OLE] Created CFSTR_SHELLIDLIST with " << pidls.size() << L" items" << std::endl;

        return S_OK;
    }

    return DV_E_FORMATETC;
}

STDMETHODIMP FileDataObject::GetDataHere(FORMATETC* pformatetc, STGMEDIUM* pmedium)
{
    // Not implemented
    return E_NOTIMPL;
}

STDMETHODIMP FileDataObject::QueryGetData(FORMATETC* pformatetc)
{
    if (!pformatetc)
        return E_INVALIDARG;

    // We support CF_HDROP and CFSTR_SHELLIDLIST with HGLOBAL
    if ((pformatetc->cfFormat == CF_HDROP || pformatetc->cfFormat == m_cfShellIDList) &&
        (pformatetc->tymed & TYMED_HGLOBAL))
        return S_OK;

    return DV_E_FORMATETC;
}

STDMETHODIMP FileDataObject::GetCanonicalFormatEtc(FORMATETC* pformatectIn, FORMATETC* pformatetcOut)
{
    if (!pformatetcOut)
        return E_INVALIDARG;

    pformatetcOut->ptd = nullptr;
    return E_NOTIMPL;
}

STDMETHODIMP FileDataObject::SetData(FORMATETC* pformatetc, STGMEDIUM* pmedium, BOOL fRelease)
{
    // Not needed for drag source
    return E_NOTIMPL;
}

STDMETHODIMP FileDataObject::EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc)
{
    if (!ppenumFormatEtc)
        return E_INVALIDARG;

    if (dwDirection == DATADIR_GET)
    {
        // We could implement a proper enumerator, but for now we'll just return E_NOTIMPL
        // Most drop targets will use QueryGetData instead
        return E_NOTIMPL;
    }

    return E_NOTIMPL;
}

STDMETHODIMP FileDataObject::DAdvise(FORMATETC* pformatetc, DWORD advf, IAdviseSink* pAdvSink, DWORD* pdwConnection)
{
    // Not needed for simple drag and drop
    return OLE_E_ADVISENOTSUPPORTED;
}

STDMETHODIMP FileDataObject::DUnadvise(DWORD dwConnection)
{
    // Not needed for simple drag and drop
    return OLE_E_ADVISENOTSUPPORTED;
}

STDMETHODIMP FileDataObject::EnumDAdvise(IEnumSTATDATA** ppenumAdvise)
{
    // Not needed for simple drag and drop
    return OLE_E_ADVISENOTSUPPORTED;
}

//=============================================================================
// Helper Function
//=============================================================================

HRESULT StartWindowsDragDrop(const std::vector<std::wstring>& filePaths)
{
    if (filePaths.empty())
    {
        std::cerr << "[OLE] No files to drag" << std::endl;
        return E_INVALIDARG;
    }

    std::wcout << L"[OLE] Starting drag operation with " << filePaths.size() << L" file(s)" << std::endl;

    // Create COM objects
    DropSource* pDropSource = new DropSource();
    FileDataObject* pDataObject = new FileDataObject(filePaths);

    if (!pDropSource || !pDataObject)
    {
        if (pDropSource) pDropSource->Release();
        if (pDataObject) pDataObject->Release();
        return E_OUTOFMEMORY;
    }

    // Start the drag-drop operation
    DWORD dwEffect = 0;
    HRESULT hr = DoDragDrop(
        pDataObject,
        pDropSource,
        DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK,
        &dwEffect
    );

    // Release COM objects
    pDropSource->Release();
    pDataObject->Release();

    if (SUCCEEDED(hr))
    {
        std::cout << "[OLE] Drag operation completed with effect: " << dwEffect << std::endl;
    }
    else
    {
        std::cout << "[OLE] Drag operation failed or cancelled, hr=0x" << std::hex << hr << std::endl;
    }

    return hr;
}
