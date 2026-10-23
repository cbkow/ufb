// WindowChrome — the chromeless window (2026-10-23, the minNotes model). The
// native title bar is gone; Main.qml's title row — the tab strip — is the
// title bar: every stretch of it no control claims drags the window, a
// double-click zooms. "As native as possible":
//   macOS   — the window keeps its titled style with Qt's
//             ExpandedClientAreaHint + NoTitleBarBackgroundHint (Main.qml
//             flags): the content extends under a transparent title bar,
//             the NATIVE traffic lights stay (zoom / fullscreen / tiling
//             keep working) and the row insets its content past them.
//             attachMac() hides the title text AppKit still draws.
//   Windows — Qt's expanded client area (Main.qml flags): no painted top
//             border, the DWM frame's shadow and corners, and Qt draws +
//             hit-tests the Windows 11 caption buttons at the row's right
//             end (snap layouts included); the row only reserves their
//             width. An unclaimed press on the row is a native caption
//             drag. (minNotes tried its own buttons + drag handle on
//             Windows and reverted: Qt's hit test synthesizes presses into
//             QML in the title zone and anything there fought it.)
//   Resizing is the OS's on both. `--native-frame` (dev) keeps the native
//   frame; the row then has no inset and no spacer.
#pragma once

#include <QObject>

class QWindow;

class WindowChrome : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool chromeless READ chromeless CONSTANT)
    // Qt draws Windows' caption buttons over the row's right end.
    Q_PROPERTY(bool captionButtons READ captionButtons CONSTANT)
    // The native traffic lights sit in the row (macOS).
    Q_PROPERTY(bool nativeLights READ nativeLights CONSTANT)
    // Windows: Qt's caption strip measured at the WINDOW'S dpi, in logical
    // px — the row's height and the width of the three buttons it must
    // keep clear of. (QML's SafeArea.margins.top is Qt's title-bar height
    // computed at 96 dpi and divided by the pixel ratio: 24.8 on a 125 %
    // screen where the buttons are really 31.2 tall and 139 wide.) 0 elsewhere.
    Q_PROPERTY(qreal captionHeight READ captionHeight NOTIFY captionMetricsChanged)
    Q_PROPERTY(qreal captionWidth READ captionWidth NOTIFY captionMetricsChanged)
public:
    explicit WindowChrome(bool chromeless, QObject* parent = nullptr);
    bool chromeless() const { return chromeless_; }
    bool captionButtons() const;
    bool nativeLights() const;
    qreal captionHeight() const { return captionHeight_; }
    qreal captionWidth() const { return captionWidth_; }
    // Apply what the flags can't once the window exists (macOS: hide the
    // title text) and start tracking the caption metrics. Re-applied
    // every time the NATIVE window is created again: an accepted close
    // (close-to-tray) destroys the NSWindow / HWND behind the QWindow, and
    // the one the next show() creates is a stock one — on macOS it drew
    // the title text over the tab strip again (2026-10-23).
    void attach(QWindow* w);
    bool eventFilter(QObject* obj, QEvent* ev) override;
    // The row's gestures: a drag past the threshold hands the press to the
    // OS move loop; a double-click zooms.
    Q_INVOKABLE bool startMove(QWindow* w);
    Q_INVOKABLE void toggleZoom(QWindow* w);
signals:
    void captionMetricsChanged();
private:
    bool chromeless_ = true;
    qreal captionHeight_ = 0;
    qreal captionWidth_ = 0;
    QWindow* window_ = nullptr;
    bool nativeWindow_ = false;   // a real cocoa / win32 window (not the offscreen platform's)
    void updateCaptionMetrics();  // on attach, a screen change (dpi), a state change
    void applyNative();           // the native side, once the platform window exists
#ifdef Q_OS_MACOS
    void attachMac(QWindow* w);   // WindowChrome_mac.mm
#endif
#ifdef Q_OS_WIN
    void captionMetricsWin(QWindow* w, qreal& height, qreal& width) const;   // WindowChrome_win.cpp
#endif
};
