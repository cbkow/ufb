// MacOSBootstrap.mm — see MacOSBootstrap.h for the contract.
//
// Probes whether an agent LaunchAgent plist is present and logs it
// (heal-on-open drives the agent lifecycle), and owns the Dock-icon
// activation-policy toggle for one-app tray mode. The SMAppService
// login item that lived here through 1.1.6 was removed in 1.2.0.

#include "MacOSBootstrap.h"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <signal.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QString>

namespace ufb {

namespace {

constexpr const char* kAgentPlistRelative =
    "Library/LaunchAgents/dev.ufb.agent.plist";

}  // namespace

void runMacOSFirstLaunchBootstrap() {
    // Slice 08 had a first-launch path here that copied a
    // dev.ufb.agent.plist template into ~/Library/LaunchAgents/ and
    // ran `launchctl bootstrap` so launchd would auto-start the
    // agent at user login. It worked, but every subsequent UFB
    // launch had to *probe* "is the plist still loaded?" — and
    // every reasonable probe (`launchctl print`, NSRunningApplication,
    // even socket-connect against the running agent) ended up
    // tripping macOS Sequoia's TCC heuristics in some way that
    // produced prompts on every launch. After repeated rounds of
    // narrowing, we removed the install path entirely. The agent
    // now spawns on demand via the heal-on-open chain in
    // `bindings/services/mount.rs::heal_macos`. Auto-start at login
    // is a future polish item, not a blocker.
    const QString plistPath = QDir::homePath()
        + QStringLiteral("/")
        + QString::fromLatin1(kAgentPlistRelative);
    if (QFileInfo::exists(plistPath)) {
        qInfo() << "[bootstrap] LaunchAgent plist present at" << plistPath
                << "— heal-on-open still drives agent lifecycle;"
                << "this UFB build does not probe launchd state.";
    } else {
        qInfo() << "[bootstrap] no LaunchAgent plist."
                << "Heal-on-open will posix_spawn the agent on first mount action.";
    }
}

// (UFBTray and its locate/spawn/retire helpers deleted 1.0.7 — the
//  in-app tray replaced it; the FinderSync appex now ships inside
//  UFB.app/Contents/PlugIns.)

// ── One-app mode (plans/17 slice E) ─────────────────────────────────

void setDockIconVisible(bool visible) {
    // Accessory ↔ Regular activation policy: with the window closed
    // the app lives in the menu bar only (no Dock icon, no Cmd-Tab
    // entry) — matching every tray-resident mac app. Showing the
    // window flips back to Regular so the Dock/Cmd-Tab entry returns.
    NSApplicationActivationPolicy want = visible
        ? NSApplicationActivationPolicyRegular
        : NSApplicationActivationPolicyAccessory;
    if ([NSApp activationPolicy] != want) {
        [NSApp setActivationPolicy:want];
        if (visible) {
            // Re-activating from Accessory needs an explicit poke or
            // the restored window comes up behind the current app.
            [NSApp activateIgnoringOtherApps:YES];
        }
    }
}

}  // namespace ufb

#endif  // __APPLE__
