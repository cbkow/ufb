// AppController — bridges single-instance IPC + cross-cutting UI
// state into QML.
//
// main.cpp creates one instance, registers it as a context property
// ("AppCtl"), and:
//   1. emits `uriRequested` whenever a secondary UFB process forwards
//      its argv via the QLocalServer (deep-link delivery) — Main.qml
//      raises the window and navigates the active Files tab.
//   2. on macOS, also emits `uriRequested` for `QFileOpenEvent`s the
//      OS routes to UfbApplication (cold-start AND warm-start
//      `open ufb://...`).
//   3. exposes `agentBanner` — a QML-readable string that the
//      notification strip in Main.qml shows when non-empty. QML
//      drives it from `Connections { target: Mount; onConnectedChanged
//      { AppCtl.agentBanner = Mount.connected ? "" : "Mount service
//      unavailable — reconnecting…" } }`. Centralising the source of
//      truth in C++ leaves room for other notifications (file-op
//      progress, mesh warnings) to land here later without each
//      QML site managing its own banner.

#pragma once

#include <QObject>
#include <QString>

#ifdef __APPLE__
#include "MacOSBootstrap.h"
#endif

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString agentBanner READ agentBanner WRITE setAgentBanner
               NOTIFY agentBannerChanged)
public:
    explicit AppController(QObject* parent = nullptr) : QObject(parent) {}

    QString agentBanner() const { return m_agentBanner; }

public slots:
    void setAgentBanner(const QString& msg) {
        if (m_agentBanner == msg) return;
        m_agentBanner = msg;
        emit agentBannerChanged();
    }

    /// One-app mode (plans/17 slice E): QML calls this on window
    /// visibility changes so the Dock icon follows the window —
    /// Accessory (menu-bar only) while tray-resident, Regular while a
    /// window is up. No-op on non-mac platforms.
    void setDockIconVisible(bool visible) {
#ifdef __APPLE__
        ufb::setDockIconVisible(visible);
#else
        Q_UNUSED(visible);
#endif
    }

signals:
    /// Fired when a secondary UFB process forwards its launch URI to
    /// us via the single-instance IPC channel, or (macOS) when the OS
    /// delivers a deep link via QFileOpenEvent. `path` is the raw arg
    /// (URI or native path); QML resolves via FileOps.resolve_ufb_uri.
    void uriRequested(const QString& path);

    /// Fires whenever `agentBanner` changes (notify for the property).
    void agentBannerChanged();

private:
    QString m_agentBanner;
};
