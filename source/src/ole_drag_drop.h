#pragma once

#include <windows.h>
#include <shlobj.h>
#include <vector>
#include <string>

// IDropSource implementation for drag and drop
class DropSource : public IDropSource
{
public:
    DropSource();

    // IUnknown methods
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IDropSource methods
    STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override;
    STDMETHODIMP GiveFeedback(DWORD dwEffect) override;

private:
    LONG m_refCount;
};

// IDataObject implementation for file drag and drop
class FileDataObject : public IDataObject
{
public:
    FileDataObject(const std::vector<std::wstring>& filePaths);
    ~FileDataObject();

    // IUnknown methods
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IDataObject methods
    STDMETHODIMP GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) override;
    STDMETHODIMP GetDataHere(FORMATETC* pformatetc, STGMEDIUM* pmedium) override;
    STDMETHODIMP QueryGetData(FORMATETC* pformatetc) override;
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC* pformatectIn, FORMATETC* pformatetcOut) override;
    STDMETHODIMP SetData(FORMATETC* pformatetc, STGMEDIUM* pmedium, BOOL fRelease) override;
    STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) override;
    STDMETHODIMP DAdvise(FORMATETC* pformatetc, DWORD advf, IAdviseSink* pAdvSink, DWORD* pdwConnection) override;
    STDMETHODIMP DUnadvise(DWORD dwConnection) override;
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA** ppenumAdvise) override;

private:
    LONG m_refCount;
    std::vector<std::wstring> m_filePaths;
    HGLOBAL m_hGlobal;
    UINT m_cfShellIDList;  // CFSTR_SHELLIDLIST format ID

    void CreateHDROP();
};

// Helper function to start Windows drag and drop operation
HRESULT StartWindowsDragDrop(const std::vector<std::wstring>& filePaths);
