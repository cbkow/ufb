// Updater_stub.cpp — no-op updater for platforms without an update
// backend (anything that isn't macOS+Sparkle or Windows+WinSparkle).
// Keeps the QML-facing API present so Main.qml's menu compiles
// everywhere; `available()` is false so the item can hide/disable.

#include "Updater.h"

#include <QtGlobal>

namespace ufb {

Updater::Updater(QObject* parent) : QObject(parent) {}
Updater::~Updater() = default;

bool Updater::available() const { return false; }

void Updater::checkForUpdates() {
    qInfo("Updater: no update backend on this platform.");
}

}  // namespace ufb
