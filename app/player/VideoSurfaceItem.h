// VideoSurfaceItem — the QQuickRhiItem that renders decoded video frames
// into the in-app preview overlay's scene graph (no native window).
//
// It pulls the latest FrameHandle from a bound VideoDecoder each present and
// draws it fitted (letterboxed) into the item's render target:
//   - macOS Metal zero-copy: CVPixelBuffer -> CVMetalTextureCache plane
//     textures (ZeroCopyBridgeMetal) -> YUV->RGB shader (biplanar/AYUV).
//   - software/CPU fallback: upload the RGBA8 QImage -> passthrough shader.
// SDR only; single render pass (QCView's compositor/OCIO passes dropped).
//
// See devdocs/lightbox-preview-plan.md.

#pragma once

#include "video_decoder.h"   // complete type required for the Q_PROPERTY meta-type

#include <QColor>
#include <QQuickRhiItem>
#include <QQuickWindow>
#include <QtQmlIntegration>

class VideoSurfaceItem : public QQuickRhiItem
{
    Q_OBJECT
    Q_PROPERTY(QColor fillColor READ fillColor WRITE setFillColor
                   NOTIFY fillColorChanged FINAL)
    Q_PROPERTY(ufbplayer::VideoDecoder *videoDecoder READ videoDecoder
                   WRITE setVideoDecoder NOTIFY videoDecoderChanged FINAL)
    QML_ELEMENT

public:
    explicit VideoSurfaceItem(QQuickItem *parent = nullptr);

    // Phase K.1 (Windows): register the window's QRhi D3D11 device with
    // the decoder side as soon as the scene graph is initialized, so
    // the very first clip a lightbox opens can attach a shared-device
    // D3D11VA context (zero-copy) instead of FFmpeg's own device +
    // readback. The registration lives with the window's scene graph
    // (cleared on sceneGraphInvalidated), not with any lightbox item —
    // decoders open before the item has rendered a frame. No-op on
    // other platforms / backends. Call once per top-level window from
    // main.cpp.
    static void installD3D11DeviceHook(QQuickWindow *window);

    QColor fillColor() const { return m_fillColor; }
    void setFillColor(const QColor &c);

    ufbplayer::VideoDecoder *videoDecoder() const { return m_decoder; }
    void setVideoDecoder(ufbplayer::VideoDecoder *decoder);

signals:
    void fillColorChanged();
    void videoDecoderChanged();

protected:
    QQuickRhiItemRenderer *createRenderer() override;

private:
    QColor m_fillColor{ 0, 0, 0 };
    ufbplayer::VideoDecoder *m_decoder = nullptr;
    QMetaObject::Connection m_frameConn;
};
