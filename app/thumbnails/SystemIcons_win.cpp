#include "SystemIcons.h"

#ifdef Q_OS_WIN

#include <windows.h>
#include <commctrl.h>      // ILD_* draw flags
#include <commoncontrols.h>// IImageList, IID_IImageList
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>

#include <vector>

#include <QtCore/QThreadStorage>

namespace ufb {

namespace {

// Per-thread OLE/COM apartment guard.
//
// Windows shell APIs (SHGetFileInfoW with icon-related flags,
// SHGetImageList) require an STA on the calling thread AND
// expect OLE - not just COM - to be initialised. The cleanest
// way is OleInitialize, which sets up COM (apartment-threaded)
// plus clipboard and drag-drop services that shell APIs touch
// internally.
//
// Plain CoInitializeEx (either STA or MTA) was insufficient:
// MTA caused SHGetFileInfoW to fail with icon flags; bare STA
// without OLE caused intermittent first-call failures.
//
// Per-thread because QThreadPool reuses worker threads and
// the apartment must persist for the thread's lifetime.
struct OleInitGuard {
    bool weInitialised = false;
    OleInitGuard() {
        const HRESULT hr = OleInitialize(nullptr);
        // S_OK = we initialised; S_FALSE = somebody beat us to it.
        // Either way OLE is now usable on this thread.
        weInitialised = (hr == S_OK);
    }
    ~OleInitGuard() {
        if (weInitialised) OleUninitialize();
    }
};

void ensureOleInitialised() {
    static QThreadStorage<OleInitGuard*> guards;
    if (!guards.hasLocalData()) {
        guards.setLocalData(new OleInitGuard);
    }
}

// Map requested pixel size to Windows' nearest SHIL_* bucket.
// Mirrors core/src/system_icons.rs::extract_icon's logic so the
// rendered icons match what users were seeing pre-port.
int shilBucket(int pixelSize) {
    if (pixelSize <= 16) return SHIL_SMALL;
    if (pixelSize <= 32) return SHIL_LARGE;
    if (pixelSize <= 48) return SHIL_EXTRALARGE;
    return SHIL_JUMBO;  // 256x256 on Vista+
}

// Convert a Win32 HICON to a QImage with a correct alpha channel.
//
// Modern shell icons (most file-type icons) are 32bpp with a real
// alpha channel — we read it straight from the colour bitmap. But
// some icons, notably the generic folder icon, are still delivered
// the legacy way: a colour bitmap whose alpha bytes are all zero
// plus a separate 1bpp AND-mask. Blitting those with
// ILD_PRESERVEALPHA produced a fully-transparent (blank) result —
// the long-standing folder-icon bug. Here we detect the all-zero-
// alpha case and rebuild alpha from the AND-mask instead.
QImage hiconToQImage(HICON hIcon) {
    if (!hIcon) return QImage();

    ICONINFO ii = {};
    if (!GetIconInfo(hIcon, &ii)) return QImage();

    QImage out;
    BITMAP bm = {};
    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm)
        && bm.bmWidth > 0 && bm.bmHeight > 0) {
        const int w = bm.bmWidth;
        const int h = bm.bmHeight;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;  // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC hdc = CreateCompatibleDC(nullptr);
        if (hdc) {
            // Colour bitmap as 32bpp BGRA.
            std::vector<quint32> color(size_t(w) * size_t(h), 0);
            GetDIBits(hdc, ii.hbmColor, 0, h, color.data(), &bi, DIB_RGB_COLORS);

            // Does the colour bitmap carry a real alpha channel?
            bool hasAlpha = false;
            for (quint32 px : color) {
                if (px & 0xFF000000u) { hasAlpha = true; break; }
            }

            // Mask-based icon → pull the AND-mask to rebuild alpha.
            std::vector<quint32> mask;
            if (!hasAlpha && ii.hbmMask) {
                mask.assign(size_t(w) * size_t(h), 0);
                GetDIBits(hdc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS);
            }
            DeleteDC(hdc);

            QImage img(w, h, QImage::Format_RGBA8888);
            if (!img.isNull()) {
                quint32* dst = reinterpret_cast<quint32*>(img.bits());
                const int n = w * h;
                for (int i = 0; i < n; ++i) {
                    const quint32 c = color[i];  // 0xAARRGGBB (BGRA in memory)
                    quint32 a;
                    if (hasAlpha) {
                        a = (c >> 24) & 0xFFu;
                    } else if (!mask.empty()) {
                        // AND-mask: set bit = transparent, clear = opaque.
                        a = (mask[i] & 0x00FFFFFFu) ? 0u : 255u;
                    } else {
                        a = 255u;
                    }
                    const quint32 r = (c >> 16) & 0xFFu;
                    const quint32 g = (c >> 8)  & 0xFFu;
                    const quint32 b = (c >> 0)  & 0xFFu;
                    // QImage::Format_RGBA8888 byte order is R,G,B,A —
                    // i.e. little-endian quint32 (A<<24)|(B<<16)|(G<<8)|R.
                    dst[i] = (a << 24) | (b << 16) | (g << 8) | r;
                }
                out = img;
            }
        }
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    return out;
}

} // namespace

QImage extractSystemIcon(const QString& extension, int pixelSize) {
    ensureOleInitialised();

    const int shil = shilBucket(pixelSize);
    const bool isFolder = (extension == QLatin1String("folder"));

    // Build the fake path Windows uses with SHGFI_USEFILEATTRIBUTES.
    // The path doesn't need to exist; Windows just looks at the
    // extension and the FILE_ATTRIBUTE_* flag.
    std::wstring fakePath;
    DWORD fileAttrs;
    if (isFolder) {
        fakePath = L"C:\\fake";
        fileAttrs = FILE_ATTRIBUTE_DIRECTORY;
    } else {
        fakePath = L"C:\\fake.";
        fakePath += extension.toStdWString();
        fileAttrs = FILE_ATTRIBUTE_NORMAL;
    }

    // The first SHGetFileInfoW call on a freshly-spun worker thread
    // intermittently fails even after OleInitialize succeeded — the
    // shell icon subsystem needs the COM apartment to settle. Retry
    // with a short yield; once a thread is warm every later call
    // succeeds first try, so in practice this only ever costs the
    // very first icon request on each pool thread one extra attempt.
    SHFILEINFOW sfi = {};
    const UINT flags = SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES;
    DWORD_PTR sfiOk = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        sfiOk = SHGetFileInfoW(fakePath.c_str(), fileAttrs, &sfi,
                               sizeof(sfi), flags);
        if (sfiOk != 0) break;
        Sleep(4);
    }
    if (sfiOk == 0) {
        qWarning("SystemIcons: SHGetFileInfoW failed for ext=%s", qPrintable(extension));
        return QImage();
    }

    IImageList* imageList = nullptr;
    HRESULT hr = SHGetImageList(shil, IID_IImageList, reinterpret_cast<void**>(&imageList));
    if (FAILED(hr) || !imageList) {
        qWarning("SystemIcons: SHGetImageList(shil=%d) failed hr=0x%lx for ext=%s",
                 shil, (unsigned long)hr, qPrintable(extension));
        return QImage();
    }

    // Pull the icon as a real HICON rather than blitting it with
    // IImageList::Draw. Draw + ILD_PRESERVEALPHA blanks any icon
    // whose colour bitmap has no real alpha channel (the folder
    // icon); going through the HICON lets hiconToQImage rebuild
    // alpha from the AND-mask for exactly those icons.
    HICON hIcon = nullptr;
    hr = imageList->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hIcon);
    imageList->Release();
    if (FAILED(hr) || !hIcon) {
        qWarning("SystemIcons: IImageList::GetIcon failed hr=0x%lx for ext=%s",
                 (unsigned long)hr, qPrintable(extension));
        return QImage();
    }

    QImage out = hiconToQImage(hIcon);
    DestroyIcon(hIcon);
    if (out.isNull()) {
        qWarning("SystemIcons: HICON conversion produced an empty image for ext=%s",
                 qPrintable(extension));
    }
    return out;
}

} // namespace ufb

#endif  // Q_OS_WIN
// macOS implementation lives in SystemIcons_mac.mm; the platform
// branch in app/CMakeLists.txt picks exactly one .o per build.
