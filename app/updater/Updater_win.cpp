// Updater_win.cpp — WinSparkle-backed updater for Windows.
//
// WinSparkle's native model is exactly what UFB wants on Windows:
// download the new installer (the Inno Setup ufb-setup-*.exe), verify
// its EdDSA signature against the embedded public key, run it, relaunch.
// The feed is the committed docs/appcast-win.xml served by GitHub Pages
// at ufbrowser.com; the installer enclosure lives on GitHub Releases.
//
// Guarded by UFB_HAVE_WINSPARKLE: when external/winsparkle isn't
// vendored, this compiles to a no-op so the app still builds.
//
// UFB_UPDATE_PUBLIC_KEY is the base64 EdDSA public key (from Sparkle's
// generate_keys — same key as macOS SUPublicEDKey), injected as a
// compile definition by app/CMakeLists.txt. Empty string => key not
// set yet (verification disabled; WinSparkle will refuse signed feeds).

#include "Updater.h"

#include <QtGlobal>

#ifdef UFB_HAVE_WINSPARKLE
#include <winsparkle.h>
#endif

#ifndef UFB_UPDATE_PUBLIC_KEY
#define UFB_UPDATE_PUBLIC_KEY ""
#endif

// App version, compiled in from CMAKE_PROJECT_VERSION (see app/CMakeLists.txt).
// WinSparkle needs this to know the INSTALLED version for its appcast
// version comparison — ufb.exe carries no VERSIONINFO resource, so without
// set_app_details WinSparkle can't tell what's running.
#ifndef UFB_APP_VERSION
#define UFB_APP_VERSION "0.0.0"
#endif
#define UFB_WIDEN2(x) L##x
#define UFB_WIDEN(x) UFB_WIDEN2(x)

namespace {
constexpr char kAppcastUrl[] = "https://ufbrowser.com/appcast-win.xml";
}

namespace ufb {

Updater::Updater(QObject* parent) : QObject(parent) {
#ifdef UFB_HAVE_WINSPARKLE
    win_sparkle_set_appcast_url(kAppcastUrl);
    // Tell WinSparkle the installed version + identity so its appcast
    // sparkle:version comparison works (no VERSIONINFO on ufb.exe).
    win_sparkle_set_app_details(L"cbkow", L"UFB", UFB_WIDEN(UFB_APP_VERSION));
    if (UFB_UPDATE_PUBLIC_KEY[0] != '\0') {
        win_sparkle_set_eddsa_public_key(UFB_UPDATE_PUBLIC_KEY);
    } else {
        qWarning("Updater(win): no EdDSA public key compiled in "
                 "(-DUFB_UPDATE_PUBLIC_KEY=...); signed feeds will be rejected.");
    }
    // Daily background check, matching the macOS Sparkle cadence.
    win_sparkle_set_automatic_check_for_updates(1);
    win_sparkle_set_update_check_interval(86400);
    win_sparkle_init();
    impl_ = this;  // non-null marker for available()
#endif
}

Updater::~Updater() {
#ifdef UFB_HAVE_WINSPARKLE
    win_sparkle_cleanup();
    impl_ = nullptr;
#endif
}

bool Updater::available() const {
#ifdef UFB_HAVE_WINSPARKLE
    return impl_ != nullptr;
#else
    return false;
#endif
}

void Updater::checkForUpdates() {
#ifdef UFB_HAVE_WINSPARKLE
    win_sparkle_check_update_with_ui();
#else
    qInfo("Updater(win): built without WinSparkle — no update backend "
          "(vendor external/winsparkle to enable).");
#endif
}

}  // namespace ufb
