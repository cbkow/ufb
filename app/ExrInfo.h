// ExrInfo — tiny QML singleton exposing an EXR's selectable layers to the
// lightbox's layer grid. Layer pixels come from the image://ufb-exr-layer
// provider; this just enumerates the layer names. Mirrors PdfDoc.
// See devdocs/lightbox-preview-plan.md.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQmlIntegration>

class ExrInfo : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit ExrInfo(QObject *parent = nullptr) : QObject(parent) {}

    // Selectable layers in `path` (named sub-layers / multi-part parts).
    // At least ["default"] for a valid EXR; empty list on error.
    // Synchronous (cheap header-only read); fine for preview use.
    Q_INVOKABLE QStringList layers(const QString &path) const;
};
