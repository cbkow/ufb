#include "ShellThumbnail.h"

#ifdef Q_OS_WIN

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>       // SHCreateItemFromParsingName
#include <shobjidl.h>     // IShellItemImageFactory, SIIGBF_*

#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace ufb {

namespace {

// Copy a GDI HBITMAP into a QImage.
//
// IShellItemImageFactory::GetImage hands back a 32bpp pre-multiplied
// ARGB DIB. GDI stores those pixels BGRA in memory, which is exactly
// QImage::Format_ARGB32_Premultiplied's layout on little-endian
// Windows — so GetDIBits copies straight across with no per-pixel
// swap (unlike the icon path, which targets Format_RGBA8888).
QImage hbitmapToQImage(HBITMAP hbmp) {
    if (!hbmp) return QImage();

    BITMAP bm{};
    if (GetObject(hbmp, sizeof(bm), &bm) == 0) return QImage();
    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    if (w <= 0 || h <= 0) return QImage();

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    if (img.isNull()) return QImage();

    HDC hdc = GetDC(nullptr);
    if (!hdc) return QImage();
    const int got = GetDIBits(hdc, hbmp, 0, h, img.bits(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (got == 0) return QImage();
    return img;
}

// The actual shell call. Runs on a throwaway thread (see below) which
// owns its own OLE apartment for the duration — IShellItemImageFactory
// drives third-party COM thumbnail handlers that expect an initialised
// apartment.
QImage extractShellThumbnailBlocking(const QString& path, QSize requested) {
    const bool ole = (OleInitialize(nullptr) == S_OK);
    struct OleScope {
        bool owned;
        ~OleScope() { if (owned) OleUninitialize(); }
    } oleScope{ole};

    const std::wstring wpath = path.toStdWString();
    IShellItemImageFactory* factory = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(
        wpath.c_str(), nullptr, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return QImage();

    const int side = qMax(16, qMax(requested.width(), requested.height()));
    const SIZE sz { side, side };

    // SIIGBF_THUMBNAILONLY: a real thumbnail or nothing — never an
    // icon substitution (the icon comes from UfbIconProvider, and an
    // icon here would defeat the crossfade gate, which keys on the
    // thumbnail Image having a non-zero source size).
    // SIIGBF_BIGGERSIZEOK: accept a larger cached thumbnail; QML
    // downscales to the cell size anyway.
    HBITMAP hbmp = nullptr;
    hr = factory->GetImage(sz, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hbmp);
    factory->Release();

    if (FAILED(hr) || !hbmp) return QImage();   // -> fall through to backends

    QImage out = hbitmapToQImage(hbmp);
    DeleteObject(hbmp);
    return out;
}

} // namespace

QImage extractShellThumbnail(const QString& path, QSize requested) {
    if (path.isEmpty()) return QImage();

    // Run the shell call on a throwaway thread with a bounded wait.
    // IShellItemImageFactory invokes third-party COM thumbnail handlers;
    // a buggy one (or a stalled network drive) can block indefinitely.
    // The thumbnail pool has only 4 workers, so a hung handler called
    // inline would pin a worker forever and starve the visible grid.
    // On timeout we abandon the throwaway thread (it exits if/when the
    // handler ever returns) and fall through to UFB's own decoder
    // backends — mirroring the 10s QuickLook ceiling on macOS.
    std::packaged_task<QImage()> task(
        [path, requested]() { return extractShellThumbnailBlocking(path, requested); });
    std::future<QImage> fut = task.get_future();
    std::thread th(std::move(task));

    constexpr auto kTimeout = std::chrono::seconds(10);
    if (fut.wait_for(kTimeout) == std::future_status::ready) {
        th.join();
        return fut.get();
    }
    th.detach();  // don't join — that would re-introduce the hang
    qWarning("ShellThumbnail(win): timed out (%s) — falling back to backends",
             qPrintable(path));
    return QImage();
}

} // namespace ufb

#endif  // Q_OS_WIN
// macOS implementation lives in ShellThumbnail_mac.mm; the platform
// branch in app/CMakeLists.txt picks exactly one .o per build.
