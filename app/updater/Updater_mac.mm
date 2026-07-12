// Updater_mac.mm — Sparkle-backed updater for macOS.
//
// Owns an SPUStandardUpdaterController (the turnkey controller that
// schedules background checks and drives Sparkle's standard UI). Feed
// URL + public key are read from Info.plist (SUFeedURL / SUPublicEDKey).
// The appcast items are informational (see Info.plist.in), so a
// "Check for Updates…" shows the version + release notes and a button
// that opens the DMG URL — Sparkle never tries to swap the multi-bundle
// install in place.
//
// Guarded by UFB_HAVE_SPARKLE: when external/sparkle/Sparkle.framework
// isn't vendored, this compiles to a no-op so the app still builds.
// Compiled with -fobjc-arc (set in app/CMakeLists.txt).

#include "Updater.h"

#ifdef UFB_HAVE_SPARKLE
#import <Sparkle/Sparkle.h>
#endif

namespace ufb {

Updater::Updater(QObject* parent) : QObject(parent) {
#ifdef UFB_HAVE_SPARKLE
    SPUStandardUpdaterController* controller =
        [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                      updaterDelegate:nil
                                                   userDriverDelegate:nil];
    // Hand ownership across the ARC boundary into the void* ivar.
    impl_ = (void*)CFBridgingRetain(controller);
    qInfo("Sparkle: updater started (auto-checks=%d, interval=%.0fs)",
          controller.updater.automaticallyChecksForUpdates ? 1 : 0,
          controller.updater.updateCheckInterval);
#endif
}

Updater::~Updater() {
#ifdef UFB_HAVE_SPARKLE
    if (impl_) {
        CFBridgingRelease(impl_);  // balances CFBridgingRetain above
        impl_ = nullptr;
    }
#endif
}

bool Updater::available() const {
#ifdef UFB_HAVE_SPARKLE
    return impl_ != nullptr;
#else
    return false;
#endif
}

void Updater::checkForUpdates() {
#ifdef UFB_HAVE_SPARKLE
    if (!impl_) return;
    SPUStandardUpdaterController* controller =
        (__bridge SPUStandardUpdaterController*)impl_;
    [controller checkForUpdates:nil];
#else
    qInfo("Updater(macOS): built without Sparkle — no update backend "
          "(vendor external/sparkle/Sparkle.framework to enable).");
#endif
}

}  // namespace ufb
