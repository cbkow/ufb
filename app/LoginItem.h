// LoginItem — "Start UFB at login" on Windows, plus the package-identity
// probes main.cpp needs before the engine exists.
//
// Two backends behind one QML surface (context property "LoginItem"):
//   * Store (MSIX) build: the manifest's windows.startupTask (TaskId
//     kStartupTaskId, Enabled="true" so it is on after the first launch).
//     Toggled through the WinRT StartupTask API. A task the user switched
//     off in Task Manager / Settings ("DisabledByUser") can't be turned
//     back on by the app — `blockedBySystem` tells QML to point at the
//     Startup settings page instead.
//   * Unpackaged build (dev, Inno): the HKCU Run value "UFB" =
//     "<exe>" --background, the same value the Inno installer's
//     app_autostart task wrote.
// A startup task can't pass arguments, so main.cpp asks
// launchedByStartupTask() and treats a yes as --background.
//
// macOS / other: available == false (the mac login item was dropped in
// 1.2.0 — the mount agent heals on demand).

#pragma once

#include <QObject>

#include <thread>
#include <vector>

class LoginItem : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool packaged READ packaged CONSTANT)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(bool blockedBySystem READ blockedBySystem NOTIFY changed)
public:
    explicit LoginItem(QObject* parent = nullptr);
    ~LoginItem() override;

    /// True when this process runs with MSIX package identity.
    static bool isPackaged();
    /// True when Windows launched this process through the package's
    /// startup task. Always false unpackaged / off Windows.
    static bool launchedByStartupTask();

    bool available() const;
    bool packaged() const { return m_packaged; }
    bool enabled() const { return m_enabled; }
    bool blockedBySystem() const { return m_blocked; }

    Q_INVOKABLE void setEnabled(bool on);
    /// Re-read the state (the user may have changed it in Task Manager).
    Q_INVOKABLE void refresh();
    /// Open Settings → Apps → Startup.
    Q_INVOKABLE void openSystemSettings();

signals:
    void changed();

private:
    void apply(bool enabled, bool blocked);
    void joinWorkers();

    bool m_packaged = false;
    bool m_enabled = false;
    bool m_blocked = false;
    // WinRT StartupTask calls in flight (off the GUI thread). Joined in the
    // destructor: LoginItem dies before the QGuiApplication (main.cpp), so a
    // worker can never post to a half-destroyed qApp at quit.
    std::vector<std::thread> m_workers;
};
