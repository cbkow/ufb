// UfbApplication.cpp — see UfbApplication.h for the contract.
//
// Plain C++; QFileOpenEvent is a portable Qt event type, no
// Objective-C++ needed even though only macOS actually fires it.

#include "UfbApplication.h"

#include <QDebug>
#include <QEvent>
#include <QFileOpenEvent>

UfbApplication::UfbApplication(int& argc, char** argv)
    : QGuiApplication(argc, argv) {}

bool UfbApplication::event(QEvent* event) {
    if (event->type() == QEvent::FileOpen) {
        auto* foe = static_cast<QFileOpenEvent*>(event);
        QString uri = foe->url().toString();
        if (uri.isEmpty() && !foe->file().isEmpty()) {
            uri = foe->file();
        }
        qInfo() << "[UfbApplication] FileOpen event:" << uri;
        if (m_ready) {
            emit uriArrived(uri);
        } else {
            // Cold-start delivery — QML root isn't up yet. Stash and
            // let main.cpp drain on engine load.
            m_pendingUri = uri;
        }
        return true;
    }
    return QGuiApplication::event(event);
}

QString UfbApplication::takePendingUri() {
    QString out = m_pendingUri;
    m_pendingUri.clear();
    return out;
}
