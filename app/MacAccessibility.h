// MacAccessibility — QObject exposing the macOS Accessibility
// permission probe + prompt to QML.
//
// The "Show Shell Menu" action in the file browser shells out to
// `osascript` which uses System Events to right-click a revealed
// item in Finder. macOS gates System Events scripting behind the
// Accessibility permission; without it, osascript returns success
// but no menu appears.
//
// First-use UX (QML side):
//   1. User invokes "Show Shell Menu".
//   2. QML calls `Accessibility.isTrusted()`. If true, calls
//      `FileOps.show_shell_menu()` directly.
//   3. If false, QML calls `Accessibility.requestTrust()` which
//      triggers the system prompt with an Open System Settings
//      button. QML can also surface a banner explaining the grant
//      flow.
//   4. After the user grants permission, the next "Show Shell
//      Menu" call works.
//
// Exposed as the `Accessibility` context property. Per macOSplans/07.

#pragma once

#ifdef __APPLE__

#include <QObject>

class MacAccessibility : public QObject {
    Q_OBJECT
public:
    explicit MacAccessibility(QObject* parent = nullptr) : QObject(parent) {}

    /// True if our process has been granted Accessibility permission.
    /// Cheap; safe to call from QML on every shell-menu invocation.
    Q_INVOKABLE bool isTrusted() const;

    /// Trigger the system prompt directing the user to grant
    /// Accessibility permission. Returns the current trust state
    /// immediately (may still be false; the user grants
    /// asynchronously). Show a UI banner alongside this call so the
    /// user understands what to do in System Settings.
    Q_INVOKABLE bool requestTrust();

    /// Open System Settings → Privacy & Security → Accessibility
    /// directly via the documented URL scheme. Useful for a "Open
    /// System Settings" button on a banner if the prompt was
    /// dismissed.
    Q_INVOKABLE void openSystemSettings();
};

#endif  // __APPLE__
