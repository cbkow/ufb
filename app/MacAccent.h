// MacAccent — observe macOS accent color changes and re-emit as a
// Qt signal.
//
// Reads `NSColor.controlAccentColor` and watches
// `NSSystemColorsDidChangeNotification` (which fires on accent
// changes, light↔dark appearance changes, and increase-contrast
// toggles — we re-read on any of those because the accent value
// differs between light/dark mode).
//
// The QML side binds `Theme.accent` to this object's `accentChanged`
// signal so selection highlights, active-tab indicators, etc. follow
// System Settings live.
//
// Per macOSplans/04 §4.

#pragma once

#ifdef __APPLE__

#include <QColor>
#include <QObject>

namespace ufb {

class MacAccentWatcher : public QObject {
    Q_OBJECT
    /// QML reads `MacAccent.accent` and binds; accentChanged() drives
    /// the property notify so binding re-evaluates on system changes.
    Q_PROPERTY(QColor accent READ current NOTIFY accentChanged)
public:
    explicit MacAccentWatcher(QObject* parent = nullptr);
    ~MacAccentWatcher() override;

    /// Read the current `NSColor.controlAccentColor`, converted to
    /// sRGB QColor. Safe to call from any thread, but this class
    /// itself lives on the main thread.
    QColor current() const;

signals:
    /// Fires whenever the OS notifies of a colour-state change (accent
    /// switch, appearance switch, contrast toggle). Always emits with
    /// the freshly-read current accent.
    void accentChanged(const QColor& color);

private:
    void* m_observer = nullptr;  // bridged from Objective-C side
};

}  // namespace ufb

#endif  // __APPLE__
