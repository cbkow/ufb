#include "UfbImageProviders.h"
#include "Thumbnailer.h"
#include "ExrBackend.h"   // ufb::decodeExr(path, size, cap, layer)
#include "PdfBackend.h"
#include "ThumbCache.h"
#include "SystemIcons.h"
#include "ShellThumbnail.h"

#include <QAtomicInt>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QUrl>

#include <cstdio>
#include <exception>

#ifdef Q_OS_WIN
#include <eh.h>      // _set_se_translator
#include <windows.h> // EXCEPTION_POINTERS
#endif

#ifdef Q_OS_MACOS
#include <csetjmp>
#include <csignal>
#endif

namespace {

#ifdef Q_OS_WIN
// Win32 structured exceptions (access violation, division by zero,
// stack overflow, etc.) are NOT caught by ordinary C++ try/catch
// blocks even with `catch (...)`. A corrupt media file that makes
// a decoder read past a buffer end produces an
// EXCEPTION_ACCESS_VIOLATION that terminates the entire process —
// taking the whole UFB main app down with it.
//
// `_set_se_translator` registers a thread-local hook that
// translates SEH into a C++ exception we *can* catch. We throw a
// minimal subclass of std::exception so the catch (std::exception&)
// arm in `runFetchSafely` below picks it up just like a normal
// decoder error. The `/EHa` compile flag (set on this file in
// app/CMakeLists.txt) is required for the translator to actually
// fire — with the default `/EHsc`, the translator is registered
// but never called.
//
// Per-thread because QThreadPool reuses worker threads and the
// translator is a TLS slot.
class SEHException : public std::exception {
public:
    explicit SEHException(unsigned int code) noexcept : code_(code) {
        std::snprintf(buffer_, sizeof(buffer_),
                      "Win32 structured exception 0x%08X", code);
    }
    const char* what() const noexcept override { return buffer_; }
private:
    unsigned int code_;
    char buffer_[64]{};
};

void ensureSehTranslator() {
    thread_local bool installed = false;
    if (!installed) {
        _set_se_translator([](unsigned int code, _EXCEPTION_POINTERS*) {
            throw SEHException(code);
        });
        installed = true;
    }
}
#endif

#ifdef Q_OS_MACOS
// macOS counterpart to the Windows SEH translator above. A decoder
// (ffmpeg / OpenEXR / psd_sdk / PDFium) that reads past a buffer end
// on a corrupt file raises SIGSEGV/SIGBUS/SIGILL/SIGFPE — fatal
// signals that ordinary try/catch does NOT catch, so without this the
// whole UFB process dies. We install a process-wide handler that, when
// a guarded thumbnail decode is in flight on this thread, siglongjmp's
// back to runFetchSafely and yields a null thumbnail instead.
//
// This is best-effort recovery (a crash mid-malloc can leave the
// allocator inconsistent); the fully-correct isolation would be an
// out-of-process decoder. But it matches the Windows guard's intent —
// one corrupt file should not take the app down — and the jmp target
// is a worker about to return anyway. Crashes OUTSIDE a guarded decode
// fall through to the default handler so genuine bugs still abort.
namespace {
thread_local sigjmp_buf g_thumbJmp;
thread_local bool g_thumbArmed = false;

extern "C" void thumbCrashHandler(int sig) {
    if (g_thumbArmed) {
        g_thumbArmed = false;
        siglongjmp(g_thumbJmp, sig);
    }
    // Not inside a guarded decode — restore default disposition and
    // re-raise so the crash terminates + reports normally.
    signal(sig, SIG_DFL);
    raise(sig);
}

void ensureThumbCrashHandler() {
    // Function-local static => thread-safe one-time install.
    static const bool installed = []() {
        struct sigaction sa{};
        sa.sa_handler = thumbCrashHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_NODEFER;  // permit re-raise from within handler
        for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) {
            sigaction(sig, &sa, nullptr);
        }
        return true;
    }();
    (void)installed;
}
}  // namespace
#endif  // Q_OS_MACOS

// Run a fetch lambda and turn any thrown exception (including
// translated Win32 SEH on Windows, or a fatal signal on macOS) into
// an empty QImage with a warning log. Decoder backends throw
// std::exception subclasses for normal failures; SEH (Windows) /
// SIGSEGV-class signals (macOS) on a corrupt file are caught by the
// platform guards. Either way the thumbnail thread keeps running and
// the user sees no thumbnail instead of a process crash.
template <class Fetch>
QImage runFetchSafely(const char* tag, Fetch& fetch) {
#ifdef Q_OS_MACOS
    ensureThumbCrashHandler();
    if (sigsetjmp(g_thumbJmp, 1) != 0) {
        g_thumbArmed = false;
        qWarning("%s: decoder crashed (fatal signal) — returning null thumbnail",
                 tag);
        return QImage();
    }
    g_thumbArmed = true;
#endif
    QImage result;
    try {
        result = fetch();
    } catch (const std::exception& e) {
        qWarning("%s: decoder threw %s — returning null thumbnail",
                 tag, e.what());
    } catch (...) {
        qWarning("%s: decoder threw unknown exception — returning null thumbnail",
                 tag);
    }
#ifdef Q_OS_MACOS
    g_thumbArmed = false;
#endif
    return result;
}

// Downscale a cached master thumbnail to the size QML asked for,
// preserving aspect ratio. Never upscales (the master is already the
// largest size we keep; returning it as-is lets QML handle any final
// fit). A null/zero request returns the master unchanged.
QImage scaleForRequest(const QImage& master, const QSize& req) {
    if (master.isNull() || !req.isValid()
        || (req.width() <= 0 && req.height() <= 0)) {
        return master;
    }
    const QSize target = master.size().scaled(req, Qt::KeepAspectRatio);
    if (target.isEmpty()
        || (target.width() >= master.width()
            && target.height() >= master.height())) {
        return master;  // request is >= master; don't upscale
    }
    return master.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// QQuickImageResponse subclass with cancellation support.
//
// QML calls `cancel()` on the response when the consuming Image
// element is destroyed (e.g. user navigates away from a folder of
// 500 EXRs - all the delegates get torn down). The runner checks
// our atomic flag at start and bails before doing any work, so
// queued-but-not-yet-running tasks finish in microseconds with no
// extraction. The handful that are MID-extraction continue to
// completion (we'd need invasive plumbing through QImageReader /
// OpenEXR / psd_sdk to interrupt them mid-flight; not worth it).
class UfbImageResponse final : public QQuickImageResponse {
public:
    UfbImageResponse() = default;

    QQuickTextureFactory* textureFactory() const override {
        const QImage img = property("__ufb_image").value<QImage>();
        return QQuickTextureFactory::textureFactoryForImage(img);
    }

    // A null decode is a load FAILURE, not a successful empty image.
    // Without this, Qt reports `status: Ready` for a null QImage, so any
    // QML gate based on sourceSize/paintedWidth/implicitWidth is fooled:
    // when the Image sets an explicit sourceSize (we do, for HiDPI), those
    // properties reflect the REQUEST, not the absent content, and stay
    // non-zero — which left declined thumbnails (huge PSD/PSB/TIFF/EXR/HDR)
    // covering the fallback icon with a blank. Reporting an error makes
    // `status === Image.Error` the single authoritative "no image" signal
    // for both the thumbnail layer and FileTypeIcon's OS-icon fallback.
    QString errorString() const override {
        const QImage img = property("__ufb_image").value<QImage>();
        return img.isNull() ? QStringLiteral("ufb: no image produced")
                            : QString();
    }

    void cancel() override {
        cancelled_.storeRelease(1);
    }

    bool isCancelled() const {
        return cancelled_.loadAcquire() != 0;
    }

private:
    QAtomicInt cancelled_{0};
};

// Run a fetch on QThreadPool::globalInstance() and resolve the
// response with the decoded QImage when it returns. Bails early
// if the response was cancelled before run() got scheduled.
template <class Fetch>
class FetchRunner final : public QRunnable {
public:
    FetchRunner(UfbImageResponse* response, Fetch fetch)
        : response_(response), fetch_(std::move(fetch)) {}

    void run() override {
        // The user navigated away while we were queued. Skip the
        // work and resolve with no image (Image element is already
        // destroyed; finished() is just bookkeeping for QML).
        if (response_->isCancelled()) {
            QMetaObject::invokeMethod(response_, [resp = response_]() {
                emit resp->finished();
            }, Qt::QueuedConnection);
            return;
        }

#ifdef Q_OS_WIN
        ensureSehTranslator();
#endif
        QImage image = runFetchSafely("ufb-fetch", fetch_);
        QMetaObject::invokeMethod(response_, [resp = response_, image]() {
            resp->setProperty("__ufb_image", image);
            emit resp->finished();
        }, Qt::QueuedConnection);
    }

private:
    UfbImageResponse* response_;
    Fetch fetch_;
};

// Dedicated thread pool for thumbnail extraction, separate from
// QThreadPool::globalInstance(). Two reasons:
//
//   1. Thumbnail jobs can be heavy (multi-second EXR/PSD reads).
//      When the user navigates away, our cancel() flag stops
//      QUEUED jobs instantly, but in-flight jobs run to
//      completion. Capping concurrency at 4 means catch-up
//      after a navigation only waits for at most 4 in-flight
//      extractions instead of all CPU cores' worth.
//   2. Icons stay on the global pool so they can't get
//      starved by a backlog of thumbnail jobs.
//
// 4 is a balance: enough to keep the visible grid responsive
// (typical visible grid is 30-60 cells) without saturating CPU
// during heavy extraction.
QThreadPool* thumbnailPool() {
    static QThreadPool* pool = []() {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(4);
        // Don't keep idle threads alive for long - thumbnail
        // bursts come and go with navigation.
        p->setExpiryTimeout(5000);
        return p;
    }();
    return pool;
}

} // namespace

// ── UfbThumbnailProvider ────────────────────────────────────────────
//
// Phase 14 step 3: backend changed from Rust extractor to C++
// Thumbnailer. Same image://ufb-thumbs/<path> URL scheme; QML
// only sees a faster, more reliable thumbnail.

QQuickImageResponse* UfbThumbnailProvider::requestImageResponse(const QString& id,
                                                                const QSize& requestedSize) {
    // Qt percent-encodes path separators and unicode in image
    // URLs - the provider receives `C:%5CUsers%5C...` and we
    // need a real filesystem path for the extractors.
    const QString path = QUrl::fromPercentEncoding(id.toUtf8());
    qDebug("ufb-thumbs req: path=%s size=%dx%d",
           path.toUtf8().constData(), requestedSize.width(), requestedSize.height());
    auto* response = new UfbImageResponse;
    auto fetch = [path, requestedSize]() -> QImage {
        // Stat once for the cache staleness key (size + mtime).
        const QFileInfo info(path);
        const qint64 fileSize = info.size();
        const qint64 mtimeMs  = info.lastModified().toMSecsSinceEpoch();

        // Cache hit — decode the stored master and downscale to the
        // request. The master is cached at kThumbMasterSize so it
        // serves every grid size / DPR sharply.
        QImage master = ufb::ThumbCache::instance().get(path, fileSize, mtimeMs);
        if (!master.isNull()) {
            qDebug("ufb-thumbs out: path=%s -> %dx%d (cache)",
                   path.toUtf8().constData(), master.width(), master.height());
            return scaleForRequest(master, requestedSize);
        }

        // Miss — extract a fresh master at the canonical size.
        const QSize masterSize(ufb::kThumbMasterSize, ufb::kThumbMasterSize);
        Thumbnailer t;

        // Stage 0 — ask the OS shell for a real thumbnail first, EXCEPT
        // for formats QImageReader decodes natively: the in-process
        // path is faster and dodges a hung thumbnaild / shell handler.
        // For RAW / Office / HEIC / PSD / PDF / video the shell handler
        // may beat us (or be the only source), so keep stage-0 there.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        if (!t.isQtNative(path)) {
            master = ufb::extractShellThumbnail(path, masterSize);
        }
#endif
        const char* source = "shell";
        if (master.isNull()) {
            master = t.extract(path, masterSize);
            source = "backend";
        }

        if (!master.isNull()) {
            ufb::ThumbCache::instance().put(path, fileSize, mtimeMs, master);
        }
        qDebug("ufb-thumbs out: path=%s -> %dx%d null=%d (%s)",
               path.toUtf8().constData(), master.width(), master.height(),
               master.isNull(), source);
        return master.isNull() ? master : scaleForRequest(master, requestedSize);
    };
    thumbnailPool()->start(new FetchRunner<decltype(fetch)>(response, fetch));
    return response;
}

// ── UfbPreviewProvider ──────────────────────────────────────────────
//
// Full-resolution still preview for the lightbox. Extracts at the
// requested view size (no 512 master / no persistent cache), so large
// EXR / PSD / TIFF / JPEG stay sharp. Falls back to the OS shell
// thumbnail for formats the backends can't decode (RAW / HEIC / Office),
// and returns null on failure (the QML side then shows the file icon).

QQuickImageResponse* UfbPreviewProvider::requestImageResponse(const QString& id,
                                                              const QSize& requestedSize) {
    const QString path = QUrl::fromPercentEncoding(id.toUtf8());
    // QML passes the on-screen view size * DPR via Image.sourceSize. If it
    // didn't, fall back to a generous default so we don't decode full-res
    // of a huge source for nothing.
    QSize target = requestedSize;
    if (target.width() <= 0 && target.height() <= 0)
        target = QSize(2048, 2048);
    qDebug("ufb-preview req: path=%s size=%dx%d",
           path.toUtf8().constData(), target.width(), target.height());
    auto* response = new UfbImageResponse;
    auto fetch = [path, target]() -> QImage {
        Thumbnailer t;
        // Backend decode at view resolution (the backends downscale to
        // `target` internally; JPEG uses DCT-scaled decode). fullResPreview
        // raises the decode caps via a ~1.5 GB byte budget so large EXR/PSB/
        // TIFF preview sharply; enormous files still decline → shell/icon.
        QImage img = t.extract(path, target, /*fullResPreview=*/true);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        // RAW / HEIC / Office etc. have no in-process backend — let the OS
        // shell produce what it can (thumbnail-resolution, but better than
        // the file icon).
        if (img.isNull())
            img = ufb::extractShellThumbnail(path, target);
#endif
        qDebug("ufb-preview out: path=%s -> %dx%d null=%d",
               path.toUtf8().constData(), img.width(), img.height(), img.isNull());
        return img;
    };
    thumbnailPool()->start(new FetchRunner<decltype(fetch)>(response, fetch));
    return response;
}

// ── UfbPdfProvider ──────────────────────────────────────────────────
//
// Renders one PDF page for the continuous-scroll reader. id is
// "<page>/<path>" (page = 0-based index before the first '/'; the path
// follows and itself begins with '/' on macOS).

QQuickImageResponse* UfbPdfProvider::requestImageResponse(const QString& id,
                                                          const QSize& requestedSize) {
    const QString raw = QUrl::fromPercentEncoding(id.toUtf8());
    const int slash = raw.indexOf('/');
    const int page = (slash > 0) ? raw.left(slash).toInt() : 0;
    const QString path = (slash >= 0) ? raw.mid(slash + 1) : raw;

    // Render at the requested width; default if QML didn't size it.
    QSize target = requestedSize;
    if (target.width() <= 0 && target.height() <= 0)
        target = QSize(1600, 1600);
    qDebug("ufb-pdf req: page=%d path=%s size=%dx%d",
           page, path.toUtf8().constData(), target.width(), target.height());
    auto* response = new UfbImageResponse;
    auto fetch = [path, page, target]() -> QImage {
        return ufb::decodePdfPage(path, page, target);
    };
    thumbnailPool()->start(new FetchRunner<decltype(fetch)>(response, fetch));
    return response;
}

// ── UfbExrLayerProvider ─────────────────────────────────────────────
//
// Decodes one EXR layer for the lightbox layer grid + selected-layer
// full view. id is "<encodedLayer>/<encodedPath>" (both percent-encoded
// on the QML side, so the split on the first '/' is unambiguous even
// though the decoded path contains slashes).

QQuickImageResponse* UfbExrLayerProvider::requestImageResponse(const QString& id,
                                                               const QSize& requestedSize) {
    const int slash = id.indexOf('/');
    const QString layer = (slash >= 0)
        ? QUrl::fromPercentEncoding(id.left(slash).toUtf8()) : QString();
    const QString path = QUrl::fromPercentEncoding(
        (slash >= 0 ? id.mid(slash + 1) : id).toUtf8());

    QSize target = requestedSize;
    if (target.width() <= 0 && target.height() <= 0)
        target = QSize(2048, 2048);
    qDebug("ufb-exr-layer req: layer=%s path=%s size=%dx%d",
           layer.toUtf8().constData(), path.toUtf8().constData(),
           target.width(), target.height());

    auto* response = new UfbImageResponse;
    auto fetch = [path, layer, target]() -> QImage {
        // ~1.5 GB peak budget at ~16 bytes/px (half RGBA + float
        // intermediate) → ~96 MP, matching the full-res preview EXR cap.
        const qint64 cap = (1536LL * 1024 * 1024) / 16;
        return ufb::decodeExr(path, target, cap, layer);
    };
    thumbnailPool()->start(new FetchRunner<decltype(fetch)>(response, fetch));
    return response;
}

// ── UfbIconProvider ─────────────────────────────────────────────────
//
// OS-native file-type icons. QML consumes it via
//   image://ufb-icons/<extension>
// where <extension> is a bare lower-case extension ("psd", "mov"),
// or one of the special keys "folder" / "file". The id is NOT a file
// path — it is never percent-decoded; extensions are ASCII with no
// separators.
//
// Backend is ufb::extractSystemIcon (Windows Shell APIs;
// NSWorkspace on macOS). Runs on QThreadPool::globalInstance() so a
// backlog of heavy thumbnail decodes on thumbnailPool() can't starve
// icon rendering.

// Per-(extension, pixel-size) icon cache. Icons are tiny
// (256×256 RGBA8888 ≈ 256 KB) and a session touches only a handful
// of distinct extensions, so the cache is unbounded by design — it
// converges to a small fixed set. QImage is copy-on-write with
// atomic refcounts, so handing copies to consumers across threads
// is safe.
//
// Beyond the throughput win, the cache collapses concurrent shell
// calls when a directory of N files of one type lands: only the
// first call does real extraction; the rest hit memory.
namespace {

struct IconCache {
    QMutex mutex;
    QHash<QString, QImage> entries;
};

IconCache& iconCache() {
    static IconCache c;
    return c;
}

QString iconCacheKey(const QString& ext, int pixel) {
    return ext + QLatin1Char('|') + QString::number(pixel);
}

}  // namespace

QQuickImageResponse* UfbIconProvider::requestImageResponse(const QString& id,
                                                           const QSize& requestedSize) {
    qDebug("ufb-icons req: id=%s size=%dx%d",
           id.toUtf8().constData(), requestedSize.width(), requestedSize.height());
    auto* response = new UfbImageResponse;
    const QString ext = id;

    // Always extract from SHIL_JUMBO (256×256), regardless of the
    // QML-side sourceSize hint. SHIL_SMALL / SHIL_LARGE / SHIL_EXTRALARGE
    // routinely returned blank or broken folder icons in list and tree
    // views on Win10/11 — the shell's small image-list buckets are an
    // old, fragile code path; jumbo always works because its bitmaps
    // are uniformly 32bpp. Qt's Image element downscales the 256-px
    // source to whatever its layout size is, with anti-aliasing at
    // least as good as the native small icons.
    constexpr int kJumboPixel = 256;
    const int pixel = kJumboPixel;
    (void)requestedSize;  // intentionally ignored; see comment above.

    auto fetch = [ext, pixel]() -> QImage {
        const QString key = iconCacheKey(ext, pixel);
        {
            QMutexLocker lock(&iconCache().mutex);
            auto it = iconCache().entries.constFind(key);
            if (it != iconCache().entries.constEnd()) return it.value();
        }
        const QImage img = ufb::extractSystemIcon(ext, pixel);
        qDebug("ufb-icons out: ext=%s pixel=%d -> %dx%d null=%d",
               ext.toUtf8().constData(), pixel, img.width(), img.height(),
               img.isNull());
        if (!img.isNull()) {
            QMutexLocker lock(&iconCache().mutex);
            iconCache().entries.insert(key, img);
        }
        return img;
    };
    QThreadPool::globalInstance()->start(new FetchRunner<decltype(fetch)>(response, fetch));
    return response;
}
