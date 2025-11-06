#pragma once

#include <windows.h>
#include <shlobj.h>
#include <vector>
#include <string>

// Testing configuration flags
// TESTING: Set to 1 to skip ImGui drag and use immediate OLE drag for better compatibility
// with picky applications like InDesign
#define OLE_DRAG_IMMEDIATE_MODE 0

// Forward declaration
class FileDataObject;

// IEnumFORMATETC implementation for enumerating clipboard formats
class FormatEnumerator : public IEnumFORMATETC
{
public:
    FormatEnumerator(const std::vector<FORMATETC>& formats, ULONG index = 0);

    // IUnknown methods
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IEnumFORMATETC methods
    STDMETHODIMP Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) override;
    STDMETHODIMP Skip(ULONG celt) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Clone(IEnumFORMATETC** ppenum) override;

private:
    LONG m_refCount;
    std::vector<FORMATETC> m_formats;
    ULONG m_index;
};

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
    UINT m_cfPreferredDropEffect;  // CFSTR_PREFERREDDROPEFFECT format ID
    DWORD m_preferredEffect;  // Preferred drop effect (DROPEFFECT_COPY, etc.)

    void CreateHDROP();
};

// Helper function to start Windows drag and drop operation
HRESULT StartWindowsDragDrop(const std::vector<std::wstring>& filePaths);
