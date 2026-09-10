// MediaInfo — tiny QML singleton answering "is this image file animated?"
// so the lightbox can route animated GIF / WebP to the video player (FFmpeg
// decodes both; 9.0 added webp_anim) while static ones stay on the plain
// image preview. Mirrors ExrInfo / TextInfo.

#pragma once

#include <QObject>
#include <QString>
#include <QtQmlIntegration>

class MediaInfo : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit MediaInfo(QObject *parent = nullptr) : QObject(parent) {}

    // True when `path` is a multi-frame image (animated GIF / WebP).
    // Header-level check through Qt's image plugins — WebP reads the
    // ANIM chunk, GIF walks the frame table — so it is cheap enough to
    // call synchronously from a QML binding. False on any error.
    Q_INVOKABLE bool isAnimated(const QString &path) const;
};
