#include "VideoSurfaceItem.h"

#include "frame_handle.h"
#include "video_decoder.h"

#if defined(Q_OS_MACOS)
#  include "zero_copy_bridge_metal.h"
#endif

#if defined(Q_OS_WIN)
#  include "d3d11/d3d11_device_manager.h"
#  include "d3d11/d3d11_vulkan_yuv_compositor.h"
#  include "d3d11/d3d11_vulkan_decode_bridge.h"
#endif

#include <QFile>
#include <rhi/qrhi.h>
#if defined(Q_OS_WIN)
#  include <rhi/qrhi_platform.h>   // QRhiD3D11NativeHandles
#endif

#include <memory>

namespace {

QShader loadShader(const char *path)
{
    QFile f(QString::fromLatin1(path));
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("VideoSurfaceItem: cannot open shader %s", path);
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

using ufbplayer::FrameHandle;

// Single-pass renderer: fetch the latest frame, convert to RGB on the GPU
// (Metal YUV planes) or upload the CPU RGBA image, and draw it fitted into
// the item's render target. SDR only — no compositor/OCIO passes.
class VideoSurfaceRenderer : public QQuickRhiItemRenderer
{
public:
    void initialize(QRhiCommandBuffer *) override {}

    void synchronize(QQuickRhiItem *item) override
    {
        auto *s = static_cast<VideoSurfaceItem *>(item);
        m_fill = s->fillColor();
        m_decoder = s->videoDecoder();
        if (m_decoder)
            m_decoder->fetchLatest(&m_frame);   // no-op if no newer frame
    }

    void render(QRhiCommandBuffer *cb) override
    {
        QRhi *r = rhi();
        QRhiResourceUpdateBatch *u = r->nextResourceUpdateBatch();
        ensureStatic(r, u);

        QRhiGraphicsPipeline *pipe = nullptr;
        QRhiShaderResourceBindings *srb = nullptr;
        QSize frameSize;

        if (m_frame.isValid())
            prepareFrame(r, u, cb, &pipe, &srb, &frameSize);

        // Clear to the backdrop fill (letterbox bars + the surface behind
        // a frame). Alpha-bearing formats composite "over" this fill, so
        // transparent ProRes regions read as the lightbox backdrop rather
        // than black/garbage. Set fillColor to the lightbox scrim tone in
        // QML so the composite is seamless with the surrounding panel.
        const QColor clear = m_fill;
        cb->beginPass(renderTarget(),
                      QColor::fromRgbF(clear.redF(), clear.greenF(),
                                       clear.blueF(), 1.0f),
                      { 1.0f, 0 }, u);

        if (pipe && srb && frameSize.width() > 0 && frameSize.height() > 0) {
            const QSize rt = renderTarget()->pixelSize();
            const float scale = qMin(float(rt.width()) / frameSize.width(),
                                     float(rt.height()) / frameSize.height());
            const float vw = frameSize.width() * scale;
            const float vh = frameSize.height() * scale;
            const float vx = (rt.width() - vw) * 0.5f;
            const float vy = (rt.height() - vh) * 0.5f;
            cb->setGraphicsPipeline(pipe);
            cb->setViewport(QRhiViewport(vx, vy, vw, vh));
            cb->setShaderResources(srb);
            QRhiCommandBuffer::VertexInput vb(m_vbuf.get(), 0);
            cb->setVertexInput(0, 1, &vb);
            cb->draw(3);
        }
        cb->endPass();
    }

private:
    void ensureStatic(QRhi *r, QRhiResourceUpdateBatch *u)
    {
        if (!m_vbuf) {
            // Oversized clip-space triangle covering the viewport.
            static const float verts[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
            m_vbuf.reset(r->newBuffer(QRhiBuffer::Immutable,
                                      QRhiBuffer::VertexBuffer, sizeof(verts)));
            m_vbuf->create();
            u->uploadStaticBuffer(m_vbuf.get(), verts);
        }
        if (!m_sampler) {
            m_sampler.reset(r->newSampler(
                QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
            m_sampler->create();
        }
        if (!m_ubuf) {
            // std140: { int matrixIdx; int fullRange; } padded to 16.
            m_ubuf.reset(r->newBuffer(QRhiBuffer::Dynamic,
                                      QRhiBuffer::UniformBuffer, 16));
            m_ubuf->create();
        }
    }

    // `blend` enables straight-alpha "over" compositing so formats that
    // carry real alpha (ProRes 4444 → AYUV on macOS, the Vulkan YUV
    // compositor on Windows) blend against the cleared backdrop instead
    // of writing transparent pixels as opaque RGB (which read as jagged,
    // un-composited edges). Opaque formats emit alpha=1 so the blend is a
    // no-op; the CPU passthrough pipeline keeps blend OFF so a stray
    // alpha=0 from a software frame can never make normal video vanish.
    std::unique_ptr<QRhiGraphicsPipeline> makePipeline(
        QRhi *r, const char *frag, QRhiShaderResourceBindings *srb,
        bool blend = false)
    {
        std::unique_ptr<QRhiGraphicsPipeline> p(r->newGraphicsPipeline());
        p->setShaderStages({
            { QRhiShaderStage::Vertex,
              loadShader(":/ufb/player/shaders/fullscreen.vert.qsb") },
            { QRhiShaderStage::Fragment, loadShader(frag) },
        });
        QRhiVertexInputLayout layout;
        layout.setBindings({ QRhiVertexInputBinding(2 * sizeof(float)) });
        layout.setAttributes({ QRhiVertexInputAttribute(
            0, 0, QRhiVertexInputAttribute::Float2, 0) });
        p->setVertexInputLayout(layout);
        p->setShaderResourceBindings(srb);
        p->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        p->setSampleCount(renderTarget()->sampleCount());
        p->setCullMode(QRhiGraphicsPipeline::None);
        p->setDepthTest(false);
        p->setDepthWrite(false);
        if (blend) {
            QRhiGraphicsPipeline::TargetBlend tb;
            tb.enable   = true;
            tb.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            tb.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            tb.srcAlpha = QRhiGraphicsPipeline::One;
            tb.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            p->setTargetBlends({ tb });
        }
        p->create();
        return p;
    }

    void prepareFrame(QRhi *r, QRhiResourceUpdateBatch *u, QRhiCommandBuffer *cb,
                      QRhiGraphicsPipeline **pipe,
                      QRhiShaderResourceBindings **srb, QSize *frameSize)
    {
        using B = QRhiShaderResourceBinding;
#if defined(Q_OS_WIN)
        if (m_frame.kind() == FrameHandle::Kind::Vulkan) {
            prepareVulkanFrame(r, cb, pipe, srb, frameSize);
            return;
        }
#endif
#if defined(Q_OS_MACOS)
        if (m_frame.kind() == FrameHandle::Kind::Metal) {
            if (!m_bridge)
                m_bridge = std::make_unique<ufbplayer::ZeroCopyBridgeMetal>(r);
            ufbplayer::ZeroCopyBridgeMetal::Bridged b;
            if (m_bridge->bridge(m_frame, &b)
                != ufbplayer::ZeroCopyBridgeMetal::Result::Ok)
                return;

            const int mi = b.colorMatrix;
            const int fr = b.fullRange;
            u->updateDynamicBuffer(m_ubuf.get(), 0, 4, &mi);
            u->updateDynamicBuffer(m_ubuf.get(), 4, 4, &fr);
            *frameSize = QSize(m_frame.width(), m_frame.height());

            if (b.layout == ufbplayer::ZeroCopyBridgeMetal::YuvLayout::AYpCbCr16) {
                if (!m_srbAyuv) m_srbAyuv.reset(r->newShaderResourceBindings());
                m_srbAyuv->setBindings({
                    B::uniformBuffer(0, B::FragmentStage, m_ubuf.get()),
                    B::sampledTexture(1, B::FragmentStage, b.yPlane, m_sampler.get()),
                });
                m_srbAyuv->create();
                if (!m_pipeAyuv)
                    m_pipeAyuv = makePipeline(
                        r, ":/ufb/player/shaders/yuv_to_rgb_ayuv.frag.qsb",
                        m_srbAyuv.get(), /*blend*/ true);   // ProRes 4444 alpha
                *pipe = m_pipeAyuv.get();
                *srb = m_srbAyuv.get();
            } else {
                if (!m_srbBi) m_srbBi.reset(r->newShaderResourceBindings());
                m_srbBi->setBindings({
                    B::uniformBuffer(0, B::FragmentStage, m_ubuf.get()),
                    B::sampledTexture(1, B::FragmentStage, b.yPlane, m_sampler.get()),
                    B::sampledTexture(2, B::FragmentStage, b.uvPlane, m_sampler.get()),
                });
                m_srbBi->create();
                if (!m_pipeBi)
                    m_pipeBi = makePipeline(
                        r, ":/ufb/player/shaders/yuv_to_rgb_biplanar.frag.qsb",
                        m_srbBi.get(), /*blend*/ true);   // opaque (a=1) → no-op
                *pipe = m_pipeBi.get();
                *srb = m_srbBi.get();
            }
            return;
        }
#endif
        if (m_frame.kind() == FrameHandle::Kind::Cpu) {
            const QImage &img = m_frame.cpuImage();
            if (img.isNull())
                return;
            if (!m_cpuTex || m_cpuTex->pixelSize() != img.size()) {
                m_cpuTex.reset(r->newTexture(QRhiTexture::RGBA8, img.size(), 1));
                m_cpuTex->create();
                if (!m_srbCpu) m_srbCpu.reset(r->newShaderResourceBindings());
                m_srbCpu->setBindings({
                    B::sampledTexture(0, B::FragmentStage, m_cpuTex.get(),
                                      m_sampler.get()),
                });
                m_srbCpu->create();
                m_pipeCpu = makePipeline(
                    r, ":/ufb/player/shaders/passthrough.frag.qsb",
                    m_srbCpu.get());
            }
            const QImage up = (img.format() == QImage::Format_RGBA8888)
                                  ? img
                                  : img.convertToFormat(QImage::Format_RGBA8888);
            u->uploadTexture(m_cpuTex.get(), up);
            *frameSize = img.size();
            *pipe = m_pipeCpu.get();
            *srb = m_srbCpu.get();
        }
    }

#if defined(Q_OS_WIN)
    // ProRes (and any Vulkan-decoded) frame → zero-copy on Qt's D3D11
    // device. The bridge runs FFmpeg's Vulkan VkImages through a Vulkan
    // compute YUV→RGB pass into a D3D11-shared RGBA16F ID3D11Texture2D
    // (NT-handle interop), which we wrap as a QRhiTexture and draw with
    // the passthrough pipeline (already RGBA — no YUV shader needed).
    void prepareVulkanFrame(QRhi *r, QRhiCommandBuffer *cb,
                            QRhiGraphicsPipeline **pipe,
                            QRhiShaderResourceBindings **srb, QSize *frameSize)
    {
        using B = QRhiShaderResourceBinding;
        if (m_d3dInitFailed)
            return;

        // Lazy one-time init: adopt Qt's QRhi D3D11 device, then stand up
        // the shared compositor + per-stream bridge.
        if (!m_vkBridge) {
            const auto *nh = static_cast<const QRhiD3D11NativeHandles *>(
                r->nativeHandles());
            if (!nh || !nh->dev || !nh->context) {
                qWarning("VideoSurfaceItem: no D3D11 native handles from QRhi");
                m_d3dInitFailed = true;
                return;
            }
            if (!ufbplayer::D3D11DeviceManager::instance()
                     .initializeWithDevice(nh->dev, nh->context)) {
                m_d3dInitFailed = true;
                return;
            }
            auto comp = std::make_unique<ufbplayer::D3D11VulkanYuvCompositor>();
            if (!comp->initialize()) {
                qWarning("VideoSurfaceItem: YUV compositor init failed — "
                         "ProRes/Vulkan frames will not display");
                m_d3dInitFailed = true;
                return;
            }
            auto bridge = std::make_unique<ufbplayer::D3D11VulkanDecodeBridge>();
            if (!bridge->initialize(comp.get())) {
                qWarning("VideoSurfaceItem: Vulkan→D3D11 bridge init failed");
                m_d3dInitFailed = true;
                return;
            }
            m_vkCompositor = std::move(comp);
            m_vkBridge     = std::move(bridge);
        }

        // The consume() does native Vulkan compute + D3D11 context work
        // behind QRhi's back; bracket it so QRhi invalidates its cached
        // D3D11 pipeline state before our subsequent draw.
        const ufbplayer::D3D11VulkanDecodeBridge::ImportedFrame *imp = nullptr;
        cb->beginExternal();
        imp = m_vkBridge->consume(m_frame, /*rangeOverride*/ 0);
        cb->endExternal();
        if (!imp || imp->planes.empty() || !imp->planes.front().texture)
            return;

        ID3D11Texture2D *tex = imp->planes.front().texture;
        const QSize sz(imp->pictureWidth, imp->pictureHeight);

        // Re-wrap only when the underlying D3D11 texture or size changes
        // (the bridge re-uses a small pool of output textures).
        if (m_vkTexNative != reinterpret_cast<quintptr>(tex)
            || !m_vkTex || m_vkTex->pixelSize() != sz) {
            m_vkTex.reset(r->newTexture(QRhiTexture::RGBA16F, sz, 1));
            QRhiTexture::NativeTexture nt;
            nt.object = static_cast<quint64>(reinterpret_cast<quintptr>(tex));
            nt.layout = 0;
            if (!m_vkTex->createFrom(nt)) {
                qWarning("VideoSurfaceItem: createFrom(ID3D11Texture2D) failed");
                m_vkTex.reset();
                return;
            }
            m_vkTexNative = reinterpret_cast<quintptr>(tex);

            if (!m_srbVk) m_srbVk.reset(r->newShaderResourceBindings());
            m_srbVk->setBindings({
                B::sampledTexture(0, B::FragmentStage, m_vkTex.get(),
                                  m_sampler.get()),
            });
            m_srbVk->create();
            if (!m_pipeVk)
                m_pipeVk = makePipeline(
                    r, ":/ufb/player/shaders/passthrough.frag.qsb",
                    m_srbVk.get(), /*blend*/ true);   // ProRes 4444 alpha (Win)
        }

        *frameSize = sz;
        *pipe = m_pipeVk.get();
        *srb = m_srbVk.get();
    }
#endif

    QColor m_fill{ 0, 0, 0 };
    ufbplayer::VideoDecoder *m_decoder = nullptr;
    FrameHandle m_frame;

    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiBuffer> m_ubuf;

    std::unique_ptr<QRhiTexture> m_cpuTex;
    std::unique_ptr<QRhiShaderResourceBindings> m_srbCpu;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeCpu;

    std::unique_ptr<QRhiShaderResourceBindings> m_srbBi;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeBi;
    std::unique_ptr<QRhiShaderResourceBindings> m_srbAyuv;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeAyuv;

#if defined(Q_OS_MACOS)
    std::unique_ptr<ufbplayer::ZeroCopyBridgeMetal> m_bridge;
#endif

#if defined(Q_OS_WIN)
    std::unique_ptr<ufbplayer::D3D11VulkanYuvCompositor> m_vkCompositor;
    std::unique_ptr<ufbplayer::D3D11VulkanDecodeBridge>  m_vkBridge;
    std::unique_ptr<QRhiTexture>                m_vkTex;
    std::unique_ptr<QRhiShaderResourceBindings> m_srbVk;
    std::unique_ptr<QRhiGraphicsPipeline>       m_pipeVk;
    quintptr m_vkTexNative   = 0;     // last wrapped ID3D11Texture2D*
    bool     m_d3dInitFailed = false; // one-shot: don't retry on every frame
#endif
};

} // namespace

VideoSurfaceItem::VideoSurfaceItem(QQuickItem *parent)
    : QQuickRhiItem(parent)
{
}

void VideoSurfaceItem::setFillColor(const QColor &c)
{
    if (m_fillColor == c)
        return;
    m_fillColor = c;
    emit fillColorChanged();
    update();
}

void VideoSurfaceItem::setVideoDecoder(ufbplayer::VideoDecoder *decoder)
{
    if (m_decoder == decoder)
        return;
    if (m_frameConn)
        QObject::disconnect(m_frameConn);
    m_decoder = decoder;
    if (m_decoder) {
        // Repaint whenever a new frame is published (queued to the GUI
        // thread; the render thread pulls it via fetchLatest in synchronize).
        m_frameConn = QObject::connect(
            m_decoder, &ufbplayer::VideoDecoder::frameAvailable,
            this, [this] { update(); });
    }
    emit videoDecoderChanged();
    update();
}

QQuickRhiItemRenderer *VideoSurfaceItem::createRenderer()
{
    return new VideoSurfaceRenderer;
}
