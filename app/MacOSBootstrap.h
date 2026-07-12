// MacOSBootstrap — first-launch installation of the agent's
// LaunchAgent plist + registration of UFBTray.app as a Login Item via
// SMAppService.
//
// Declaration lives here so app/main.cpp can call it. The full
// implementation is split across slices:
//
//   • Slice 04 (this slice): probe path. Detects whether the agent's
//     LaunchAgent is loaded. Logs and returns. No mutating actions.
//   • Slice 08 (packaging): install path. Copies the bundled
//     `dev.ufb.agent.plist` from `UFB.app/Contents/Resources/` into
//     `~/Library/LaunchAgents/` if missing, then launchctl bootstrap.
//     Registers UFBTray.app as a Login Item via SMAppService.
//   • Both slices: idempotent. Gated on a settings.json flag so
//     subsequent launches no-op.
//
// Per macOSplans/03 §"First-launch bootstrap entry point" and
// macOSplans/08 §"First-launch bootstrap".

#pragma once

#ifdef __APPLE__

namespace ufb {

/// Probe the agent's LaunchAgent state; install + bootstrap if
/// missing (slice 08). Idempotent — second call returns immediately.
/// Logs every action via qInfo() / qWarning(); failures are
/// non-fatal (the user can rerun manually or reinstall).
///
/// Safe to call from main() before the QML engine loads.
void runMacOSFirstLaunchBootstrap();

// (launchMacOSTrayIfNeeded / retireLegacyTray deleted 1.0.7 with
//  UFBTray itself.)

// ── One-app mode (plans/17 slice E) ──────────────────────────────

/// Accessory ↔ Regular activation policy: hide the Dock icon while
/// the app is tray-only (window closed / --background), restore it
/// (and activate) when the window shows.
void setDockIconVisible(bool visible);

/// Register UFB.app as a launchd Login Item (SMAppService agent plist
/// with --background) so login starts the app tray-only. Idempotent.
void registerGuiLoginItem();

}  // namespace ufb

#endif  // __APPLE__
