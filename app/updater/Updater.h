// Updater.h — "Check for Updates…" controller exposed to QML.
//
// Thin platform-split wrapper over the OS update frameworks:
//   macOS   — Sparkle (SPUStandardUpdaterController). Informational
//             appcast items: Sparkle shows the version + release notes
//             and opens the DMG URL (the multi-bundle drag-install DMG
//             can't be atomically swapped; notarization is integrity).
//   Windows — WinSparkle. Downloads the new installer, verifies the
//             EdDSA signature, runs it, relaunches.
//
// Implementations are selected per-platform in app/CMakeLists.txt
// (Updater_mac.mm / Updater_win.cpp / Updater_stub.cpp), exactly like
// the ShellThumbnail_{mac,win} pair. Registered to QML in main.cpp via
//   engine.rootContext()->setContextProperty("Updater", &updater);
// so QML calls `Updater.checkForUpdates()`.
//
// The framework link is conditional: if external/sparkle (macOS) or
// external/winsparkle (Windows) isn't present, the stub build compiles
// and `available()` returns false — the menu item can hide/disable.

#pragma once

#include <QObject>

namespace ufb {

class Updater : public QObject {
    Q_OBJECT
    // True when a real update backend is compiled in. QML can bind the
    // menu item's visibility/enabled to this.
    Q_PROPERTY(bool available READ available CONSTANT)
public:
    explicit Updater(QObject* parent = nullptr);
    ~Updater() override;

    bool available() const;

    // User-initiated check. Brings up the framework's own update UI
    // (release notes, progress). Safe to call from QML / the GUI thread.
    Q_INVOKABLE void checkForUpdates();

private:
    void* impl_ = nullptr;  // SPUStandardUpdaterController* on macOS
};

}  // namespace ufb
