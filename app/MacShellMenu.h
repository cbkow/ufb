// MacShellMenu — the native "Show Finder Menu" (2026-10-23). Finder's own
// context menu cannot be summoned by another app (no public API), and the
// previous route — an AppleScript that activated Finder, revealed the item
// and had System Events press Ctrl+Return — needed the Accessibility
// permission, switched apps, waited 300 ms and failed silently without
// the grant. This builds the menu in-app from the public APIs behind
// Finder's verbs and pops it at the cursor:
//
//   Open · Open With ▸ (LaunchServices' apps for the file, the default
//   marked, Other…) · Show in Finder · Get Info (an Apple event to Finder —
//   an Automation consent once, no GUI scripting) · Share ▸ (the system
//   share sheet's services, AirDrop included) · Tags ▸ (Finder's label
//   colours, toggled on the files' tag names).
//
// Not available this way: third-party Finder extensions / Quick Actions and
// Finder-only rows ("New Terminal at Folder"). Rename, Duplicate, Trash,
// Compress and Copy Path are UFB's own menu rows already.
#pragma once

#ifdef __APPLE__

#include <QObject>
#include <QStringList>

class MacShellMenu : public QObject {
    Q_OBJECT
public:
    explicit MacShellMenu(QObject* parent = nullptr) : QObject(parent) {}
    /// Pop the menu for `paths` at the current mouse location. Runs the
    /// menu's own event loop; returns when it closes.
    Q_INVOKABLE void show(const QStringList& paths);
};

#endif  // __APPLE__
