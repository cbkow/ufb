// WindowChrome — the Windows half (see WindowChrome.h): the caption metrics.
// Qt's caption strip (qwindowswindow.cpp, getTitleBarHeight_sys): title-bar
// height = SM_CYCAPTION + the resize border (SM_CXSIZEFRAME +
// SM_CXPADDEDBORDER), all at the window's dpi, in NATIVE pixels; each of
// the three buttons is int(height × 1.5) wide. Reported in logical pixels
// for the row.
#include "WindowChrome.h"

#include <QWindow>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

void WindowChrome::captionMetricsWin(QWindow* w, qreal& height, qreal& width) const {
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd) return;
    const UINT dpi = GetDpiForWindow(hwnd);
    const int native = GetSystemMetricsForDpi(SM_CYCAPTION, dpi)
                     + GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                     + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const int button = int(native * 1.5);
    const qreal dpr = w->devicePixelRatio() > 0 ? w->devicePixelRatio() : 1.0;
    height = native / dpr;
    width = button * 3 / dpr;
}
