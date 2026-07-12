// UfbApplication — QGuiApplication subclass that catches QFileOpenEvent.
//
// macOS routes deep links (`ufb://...` / `union://...`) and document
// open events through the Cocoa `application:openURL:` callback, which
// Qt translates into a `QFileOpenEvent` posted to the QGuiApplication
// instance. We override `event()` to catch those.
//
// The wrinkle: QFileOpenEvent can fire BEFORE the QML root is up
// (cold-start with a URI argument from the OS). We stash a pending URI
// in that case; main.cpp drains it once the engine has loaded and
// AppController is wired. After that, every event emits `uriArrived`
// directly.
//
// Per `macOSplans/04-qt-app-shell-macos.md` §1.

#pragma once

#include <QGuiApplication>
#include <QString>

class UfbApplication : public QGuiApplication {
    Q_OBJECT
public:
    UfbApplication(int& argc, char** argv);

    bool event(QEvent* event) override;

    /// True after main.cpp has wired the `uriArrived` connection.
    /// While false, incoming FileOpen events stash to `m_pendingUri`
    /// instead of being signalled.
    bool ready() const { return m_ready; }
    void markReady() { m_ready = true; }

    /// Take the cold-start URI (if any). Empty string after the first
    /// call. Drained by main.cpp on QML root load.
    QString takePendingUri();

signals:
    /// Emitted when macOS routes a deep link / file-open to us AFTER
    /// `markReady()`. Connect to AppController::uriRequested in main.cpp.
    void uriArrived(const QString& uri);

private:
    QString m_pendingUri;
    bool m_ready = false;
};
