// MacOSBootstrap.mm — see MacOSBootstrap.h for the contract.
//
// Slice 04 ships the PROBE half: detects whether the agent's
// LaunchAgent is loaded and logs the result. The INSTALL half (copy
// plist from .app Resources, launchctl bootstrap, register Login Item
// via SMAppService) lands in slice 08, when the .app bundle has a
// template plist in Resources/ to copy from.

#include "MacOSBootstrap.h"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>
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

void registerGuiLoginItem() {
    // Register this app as a launchd Login Item via SMAppService with
    // a bundled agent plist (Contents/Library/LaunchAgents/
    // dev.ufb.gui.plist) whose ProgramArguments carry --background —
    // so login starts UFB tray-only, which then heals the mount agent.
    // Replaces UFBTray's SMAppService.mainApp registration. Idempotent
    // (status check first); user can disable in System Settings →
    // Login Items. First registration shows Apple's one-time
    // "added a Login Item" notification.
    if (@available(macOS 13.0, *)) {
        SMAppService* svc =
            [SMAppService agentServiceWithPlistName:@"dev.ufb.gui.plist"];
        switch (svc.status) {
        case SMAppServiceStatusEnabled:
            qInfo() << "[bootstrap] GUI login item already registered";
            return;
        case SMAppServiceStatusRequiresApproval:
            qInfo() << "[bootstrap] GUI login item awaiting approval in"
                    << "System Settings";
            return;
        default: {
            NSError* err = nil;
            if ([svc registerAndReturnError:&err]) {
                qInfo() << "[bootstrap] registered GUI login item"
                        << "(--background)";
            } else {
                qWarning() << "[bootstrap] GUI login item registration"
                           << "failed:"
                           << QString::fromNSString(err.localizedDescription);
            }
        }
        }
    } else {
        qInfo() << "[bootstrap] macOS < 13 — SMAppService unavailable";
    }
}

}  // namespace ufb

#endif  // __APPLE__
