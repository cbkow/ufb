// UFB main entry point. Phase 0: open an empty Qt Quick window.
//
// In later phases this gets:
//   - cxx-qt singleton registration (Phase 2)
//   - QQuickAsyncImageProvider for thumbs + icons (Phase 6/7)
//   - Single-instance + deep-link handling (Phase 5)
//   - Mica/dark-titlebar adjustments on Windows (Phase 5)

#include <QGuiApplication>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QDebug>
#include <QDir>
#include <QLockFile>
#ifdef Q_OS_MACOS
#  include <QColorSpace>
#  include <QSurfaceFormat>
#endif
#include <QLocalServer>
#include <QLocalSocket>
#include <cstdio>
#include <cstring>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <dwmapi.h>     // DwmSetWindowAttribute (DWMWA_CLOAK flash fix)
#  include <shobjidl.h>   // SetCurrentProcessExplicitAppUserModelID
#  include <QTimer>       // uncloak safety-net timer
#endif

#include "UfbImageProviders.h"
#include "player/video_decoder.h"
#if defined(Q_OS_WIN)
#  include "player/vulkan/vulkan_device_manager.h"
#endif
#include "AppController.h"
#include "UfbApplication.h"

#ifdef UFB_HAVE_WEBENGINE
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#endif
#include "updater/Updater.h"
#ifdef Q_OS_MACOS
#  include "MacAccent.h"
#  include "MacAccessibility.h"
#  include "MacOSBootstrap.h"
#  include "MacWindowChrome.h"
#endif

namespace {
/// Writable directory shared with the Rust core's settings storage.
/// Mirrors `ufb_core::utils::get_app_data_dir()` so the lock file
/// lives next to settings.json (no surprise location for users
/// inspecting their data).
QString ufbAppDataDir() {
#ifdef Q_OS_WIN
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) base = QDir::homePath();
#elif defined(Q_OS_MACOS)
    QString base = QDir::homePath() + "/Library/Application Support";
#else
    QString base = QDir::homePath() + "/.config";
#endif
    QString dir = base + "/ufb";
    QDir().mkpath(dir);
    return dir;
}

/// QLocalServer / QLocalSocket address.
///
/// Windows: a bare name maps to `\\.\pipe\<name>` (per-user — pipes
/// are session-scoped).
///
/// macOS: full path to a Unix socket inside our App Group container
/// per macOSplans/04. Living inside the Group Container means the
/// sandboxed FinderSync extension could reach it later if we ever
/// route deep links via that path. The filename is short to stay
/// under macOS's 104-byte `sun_path` limit.
///
/// Other unix: relative name; `QLocalServer` resolves it under
/// `$TMPDIR`.
QString singletonServerAddress() {
#ifdef Q_OS_MACOS
    QString home = QDir::homePath();
    QString dir = home
        + "/Library/Group Containers/5Z4S9VHV56.group.com.unionfiles.ufb";
    QDir().mkpath(dir);
    return dir + "/ufb-app.sock";
#else
    return QStringLiteral("ufb-singleton");
#endif
}
}  // namespace

extern "C" {
    // From the bindings crate. Phase 0 stub — does nothing yet.
    void ufb_bindings_phase0_smoke_test(void);
}

// Phase 2 bring-up: force every Qt log message to stderr so QML import
// errors are visible from PowerShell. Will go away (or get a better
// handler that writes to a log file) once the GUI shell stabilises.
static void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    const char* prefix = "?";
    switch (type) {
    case QtDebugMsg:    prefix = "DBG"; break;
    case QtInfoMsg:     prefix = "INF"; break;
    case QtWarningMsg:  prefix = "WRN"; break;
    case QtCriticalMsg: prefix = "CRT"; break;
    case QtFatalMsg:    prefix = "FTL"; break;
    }
    fprintf(stderr, "[Qt %s] %s", prefix, qPrintable(msg));
    if (ctx.file) {
        fprintf(stderr, "  (%s:%d)", ctx.file, ctx.line);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // --prime-smartscreen runs at install time so Windows finishes its
    // first-run reputation check on ufb.exe before the user ever
    // launches it. Without this, the first time the user clicks a
    // mount, our list_directory call traverses a junction and Windows
    // returns ERROR_UNTRUSTED_MOUNT_POINT (448) until SmartScreen
    // completes its async scan. The retry in
    // core/src/file_ops::read_dir_with_448_retry handles it but has a
    // finite budget. Stay alive a few seconds with no Qt setup, no
    // window, no side effects, then exit. Must come BEFORE
    // QGuiApplication so no infrastructure windows are spun up.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--prime-smartscreen") == 0) {
            Sleep(3000);
            return 0;
        }
    }

    // AUMID must be set BEFORE any window is created (Microsoft's own
    // contract for SetCurrentProcessExplicitAppUserModelID). Qt's
    // QGuiApplication constructor spins up hidden infrastructure
    // windows for clipboard/DnD/etc.; if we set the AUMID after that,
    // those windows are already bound to the default AUMID and the
    // taskbar's icon-grouping uses that default's icon instead of
    // anything we set later. So this is the very first thing we do.
    SetCurrentProcessExplicitAppUserModelID(L"com.unionfiles.ufb");

    // Release builds set WIN32_EXECUTABLE TRUE (GUI subsystem) which
    // detaches stdout/stderr from any parent console. Reattach when
    // a parent console exists - launching from PowerShell / cmd still
    // shows qInstallMessageHandler output, while Explorer / shortcut
    // launches stay console-free (AttachConsole returns FALSE).
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }
#endif

    qInstallMessageHandler(messageHandler);

    // Pin the scene-graph RHI backend so the in-app video player's
    // QQuickRhiItem has a known device to interop decoded frames with:
    // Metal on macOS, Direct3D 11 on Windows. Both are Qt's defaults
    // today, but pinning makes the zero-copy texture interop (Metal
    // CVPixelBuffer / D3D11 device injection) deterministic across Qt
    // upgrades. Must run before any QQuickWindow is created.
#if defined(Q_OS_MACOS)
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#elif defined(Q_OS_WIN)
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
#endif

#if defined(Q_OS_WIN)
    // Stand up the decode-only Vulkan device early (mirrors QCView's
    // F.2.4.1). FFmpeg's Vulkan hwaccel attach (ProRes zero-copy path)
    // consumes this same VkDevice via createSharedVulkanHwDeviceCtx().
    // Soft init: a failure is logged but does not abort startup —
    // ProRes then falls back to software and non-ProRes still uses
    // D3D11VA, so the lightbox stays usable on Vulkan-less machines.
    if (!ufbplayer::VulkanDeviceManager::instance().initialize()) {
        qWarning("main: VulkanDeviceManager init failed — Vulkan (ProRes) "
                 "decode path unavailable; falling back to software.");
    }
#endif

#ifdef Q_OS_MACOS
    // Pin the default surface format to sRGB on macOS. Qt 6's RHI lets
    // the platform pick the swapchain colour space, and on a Display
    // P3 panel that means our hex colours (authored against sRGB on
    // Windows) come out oversaturated. Setting the colour space on the
    // default format before any QWindow is constructed makes Metal
    // present in sRGB regardless of the screen's native gamut.
    {
        QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
        fmt.setColorSpace(QColorSpace::SRgb);
        QSurfaceFormat::setDefaultFormat(fmt);
    }
#endif

#ifdef UFB_HAVE_WEBENGINE
    // WebEngine (rendered HTML lightbox preview) must initialize before
    // the QGuiApplication exists — it configures context sharing for
    // Chromium's compositor. No-op cost when no HTML is ever previewed;
    // the renderer processes only spawn when a WebEngineView is created.
    //
    // --disable-gpu-compositing: on macOS/Metal (Qt 6.11.1, DPR 2)
    // Chromium's GPU compositor hands Qt frames sized past the item's
    // bounds — a 60-100px band of uninitialized (black, then
    // transparent) texture beside/below the page. Reproduced standalone
    // with a bare WebEngineView; software compositing renders correctly
    // and costs nothing measurable for static document previews (video
    // never goes through WebEngine). Revisit on Qt upgrades.
    {
        QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        if (!flags.contains("disable-gpu-compositing")) {
            if (!flags.isEmpty()) flags += ' ';
            flags += "--disable-gpu-compositing";
            qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
        }
    }
    QtWebEngineQuick::initialize();
#endif

    // UfbApplication overrides QGuiApplication::event() to catch
    // QFileOpenEvent for macOS deep-link delivery (cold-start AND
    // warm-start `open ufb://...`). On Windows + Linux it behaves
    // identically to QGuiApplication. See macOSplans/04 §1.
    UfbApplication app(argc, argv);

    QGuiApplication::setApplicationName("UFB");
    QGuiApplication::setOrganizationName("UFB");
    // UFB_APP_VERSION comes from CMake (CMAKE_PROJECT_VERSION) so the
    // runtime version can't drift from Info.plist across releases.
    QGuiApplication::setApplicationVersion(UFB_APP_VERSION);

    // Launch args (parsed early — the macOS bootstrap below needs
    // --background before the engine exists). One positional arg is a
    // launch path/URI; flags are consumed here.
    QString launchPath;
    bool startInBackground = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--background")) {
            startInBackground = true;
            continue;
        }
        if (a.startsWith("--") || a.startsWith("-")) continue;  // skip flags
        if (launchPath.isEmpty()) launchPath = a;
    }

#ifdef Q_OS_MACOS
    // (MacOSMigration deleted 1.0.7 — every fleet mac ran the Tauri-era
    // cleanup many versions ago.)
    // Detect the LaunchAgent state; install + bootstrap the agent
    // plist if missing. Idempotent. Slice 04 ships the probe path;
    // the install half lands in slice 08 once the .app bundle has a
    // template plist in Resources/.
    ufb::runMacOSFirstLaunchBootstrap();

    // One-app mode (plans/17 slice E): the GUI hosts the tray now.
    // Retire any legacy UFBTray still running (second icon + agent
    // race), register ourselves as the login item (--background via
    // the bundled dev.ufb.gui.plist), and start menu-bar-only when
    // launched in background mode.
    ufb::registerGuiLoginItem();
    if (startInBackground)
        ufb::setDockIconVisible(false);
#endif

    // Window icon for Qt-managed surfaces (titlebar, Alt-Tab, taskbar
    // when AUMID matches). PNG embedded as a Qt resource via
    // qt_add_resources(app_icons ...) so we don't depend on the
    // qico.dll image-format plugin or on a particular filesystem
    // layout. Filesystem path remains as a fallback for dev launches
    // where the resource compile got stale. The exe's separate
    // RT_GROUP_ICON resource (app/ufb.rc) covers OS shell surfaces.
    //
    // Skipped on macOS: setWindowIcon overrides the bundle's
    // Contents/Resources/UFB.icns at runtime with a flat PNG, which
    // means the Dock shows our mark unframed (no Tahoe squircle /
    // Liquid Glass mortise, no proper multi-resolution rendering).
    // Letting the icns drive everything via CFBundleIconFile gives
    // us the system framing for free. macOS doesn't paint a
    // per-window titlebar icon either, so there's nothing else
    // Qt-side that setWindowIcon would feed.
#ifndef Q_OS_MACOS
    {
        QIcon icon(":/icons/icon.png");
        if (icon.isNull()) {
            const QString exeDir = QGuiApplication::applicationDirPath();
            QStringList iconDirs;
            iconDirs << exeDir + "/icons";
            for (const QString& dir : iconDirs) {
                icon = QIcon(dir + "/icon.png");
                if (!icon.isNull()) break;
                icon = QIcon(dir + "/icon.ico");
                if (!icon.isNull()) break;
                icon = QIcon(dir + "/32x32.png");
                if (!icon.isNull()) break;
            }
        }
        if (!icon.isNull()) QGuiApplication::setWindowIcon(icon);
    }
#endif

    // ── Single-instance gate ─────────────────────────────────────────
    // Try to acquire the lock. If another UFB is already running we
    // forward our launch arg (a `ufb://` URI or native path) over a
    // local socket and exit with code 0 - the existing window will
    // raise itself and navigate to the URI via AppController.
    //
    // setStaleLockTime(0) means "immediately reclaim a lock whose
    // holder appears to be dead" so a hard kill (Task Manager,
    // unclean shutdown) doesn't permanently brick subsequent launches.
    const QString lockPath = ufbAppDataDir() + "/ufb.lock";
    QLockFile lockFile(lockPath);
    lockFile.setStaleLockTime(0);
    const bool isPrimary = lockFile.tryLock(100);
    if (!isPrimary) {
        // Secondary: forward argv to the running primary, exit.
        QString forward;
        for (int i = 1; i < argc; ++i) {
            const QString a = QString::fromLocal8Bit(argv[i]);
            if (a.startsWith("--") || a.startsWith("-")) continue;
            forward = a;
            break;
        }
        QLocalSocket sock;
        sock.connectToServer(singletonServerAddress());
        if (sock.waitForConnected(800)) {
            sock.write(forward.toUtf8());
            sock.flush();
            sock.waitForBytesWritten(800);
            sock.disconnectFromServer();
        } else {
            qWarning("ufb: secondary couldn't reach primary - aborting forward");
        }
        return 0;
    }

#ifdef Q_OS_WIN
    QFont base("Segoe UI Variable Display", 9);
    QGuiApplication::setFont(base);
    QQuickStyle::setStyle("FluentWinUI3");
#elif defined(Q_OS_MACOS)
    // Take Qt's default (which routes to .AppleSystemUIFont → SF Pro
    // on macOS 10.11+) but bump to 13pt to match Finder's default UI
    // text size. Per macOSplans/04 §5.
    {
        QFont base = QGuiApplication::font();
        base.setPointSize(13);
        QGuiApplication::setFont(base);
    }
    // Use FluentWinUI3 on macOS too, matching Windows. The native
    // `macOS` style is locked-down — its Dialog ignores our
    // Theme.dim padding tokens and its scrollbars / rectangles
    // refuse customization (cf. the `current style does not support
    // customization of this control` warnings the macOS style emits
    // in the launch log). Our dark theme overrides the look of every
    // control we render anyway, so the native style only created a
    // mismatch — modals had Apple HIG padding while the rest of the
    // shell sized itself off Theme tokens.
    QQuickStyle::setStyle("FluentWinUI3");
#endif

    // Smoke-test the Rust↔C++ link. Should print a one-line message
    // through the Rust logger.
    ufb_bindings_phase0_smoke_test();

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection
    );

    // Image providers. QML consumes them via
    //   image://ufb-thumbs/<absolute-path>  — real file previews
    //   image://ufb-icons/<extension>       — OS file-type icons
    // The engine takes ownership of both pointers.
    engine.addImageProvider(QStringLiteral("ufb-thumbs"), new UfbThumbnailProvider);
    engine.addImageProvider(QStringLiteral("ufb-preview"), new UfbPreviewProvider);
    engine.addImageProvider(QStringLiteral("ufb-pdf"), new UfbPdfProvider);
    engine.addImageProvider(QStringLiteral("ufb-exr-layer"), new UfbExrLayerProvider);
    engine.addImageProvider(QStringLiteral("ufb-icons"),  new UfbIconProvider);
    qInfo("ufb: image providers registered (ufb-thumbs, ufb-icons)");

    // Pick up a single argv path/URI argument and expose it to QML
    // as the global context property `_launchPath`. Main.qml reads
    // it on Component.onCompleted and navigates the active pane.
    //
    // Accepts:
    //   - a `ufb://` / `union://` URI (deep link from another OS or
    //     browser association)
    //   - a plain native path, e.g. `ufb.exe C:\Users\me\Downloads`
    //
    // Resolution (URI parsing + path-mapping swap) happens QML-side
    // via FileOps.resolve_ufb_uri so we don't need to drag the
    // bindings into main.cpp.
    engine.rootContext()->setContextProperty("_launchPath", launchPath);

    // Rendered-HTML preview availability. PreviewLightbox routes html/htm
    // to the WebEngine-based HtmlPreview only when true; otherwise they
    // stay on the QTextDocument TextPreview (builds without the optional
    // WebEngine module, e.g. Windows until it's installed there).
#ifdef UFB_HAVE_WEBENGINE
    engine.rootContext()->setContextProperty("_webEngineAvailable", true);
#else
    engine.rootContext()->setContextProperty("_webEngineAvailable", false);
#endif

    // One-app mode (plans/17 slice E): the GUI hosts the tray icon and
    // stays resident when the last window closes — closing the window
    // no longer tears down the mesh, thumbnails, or the mount client.
    // Quit is explicit (tray menu / Cmd-Q). `--background` (login item
    // via the bundled SMAppService plist) starts tray-only, no window.
    QGuiApplication::setQuitOnLastWindowClosed(false);
    engine.rootContext()->setContextProperty("_startInBackground",
                                             startInBackground);

    // Single-instance IPC server. Listens for forwarded launch args
    // from secondary UFB processes (see the lock-file gate at the
    // top of main()). Each connection delivers one URI/path; we
    // forward it to QML via AppController::uriRequested.
    AppController appController;
    engine.rootContext()->setContextProperty("AppCtl", &appController);

    // The "Credentials" context property (CredentialPrompt) is gone —
    // plans/17 slice B: credentials are OS-owned; connecting IS the
    // sign-in and the OS dialog appears via the mount allow_ui path.

    // Auto-update controller (Sparkle on macOS, WinSparkle on Windows;
    // no-op stub elsewhere). QML's Help menu calls
    // `Updater.checkForUpdates()`; `Updater.available` gates the item.
    // When no backend is vendored, available() is false and the menu
    // item hides. Schedules a background check on construction.
    ufb::Updater updater;
    engine.rootContext()->setContextProperty("Updater", &updater);

#ifdef Q_OS_MACOS
    // Live accent-colour observer. QML reads `MacAccent.accent` and
    // binds; the QObject's accentChanged() signal is the property
    // notify so bindings re-evaluate on System Settings changes.
    // Per macOSplans/04 §4. (Theme.qml is updated to consume it in
    // the broader theme-system slice; for v0.10.0 the wiring is in
    // place even if no QML site reads it yet.)
    ufb::MacAccentWatcher macAccent;
    engine.rootContext()->setContextProperty("MacAccent", &macAccent);

    // Accessibility probe + prompt. QML calls
    // `Accessibility.isTrusted()` before `FileOps.show_shell_menu()`;
    // if false, calls `Accessibility.requestTrust()` to surface the
    // system prompt. Per macOSplans/07.
    MacAccessibility accessibility;
    engine.rootContext()->setContextProperty("Accessibility", &accessibility);
#endif

    QLocalServer singletonServer;
    // Belt-and-suspenders: if a previous primary crashed without
    // unlinking its socket file, listen() would fail. removeServer()
    // is a no-op when no stale socket exists.
    const QString singletonAddress = singletonServerAddress();
    QLocalServer::removeServer(singletonAddress);
    if (!singletonServer.listen(singletonAddress)) {
        qWarning("ufb: QLocalServer.listen failed (%s) - deep links from "
                 "secondary launches won't reach this window",
                 qPrintable(singletonServer.errorString()));
    }
    QObject::connect(
        &singletonServer, &QLocalServer::newConnection,
        &appController, [&singletonServer, &appController]() {
            QLocalSocket* sock = singletonServer.nextPendingConnection();
            if (!sock) return;
            sock->waitForReadyRead(500);
            const QByteArray bytes = sock->readAll();
            sock->disconnectFromServer();
            sock->deleteLater();
            const QString uri = QString::fromUtf8(bytes);
            // QML side raises the window + navigates. Empty payload
            // is still useful (just raises the window).
            emit appController.uriRequested(uri);
        }
    );

    engine.loadFromModule("Ufb.App", "Main");

#ifdef Q_OS_MACOS
    // ── macOS-specific post-engine setup ─────────────────────────────
    //
    // 1. Bridge UfbApplication's QFileOpenEvent path into AppController
    //    so deep links arriving warm-start route the same way as
    //    secondary-launch IPC forwards. After this, mark the app
    //    "ready" and drain any URI that arrived during cold-start
    //    (before the QML engine loaded).
    QObject::connect(
        &app, &UfbApplication::uriArrived,
        &appController, &AppController::uriRequested
    );
    app.markReady();
    {
        const QString pending = app.takePendingUri();
        if (!pending.isEmpty()) {
            // Queue so QML's AppCtl handler is registered first.
            QMetaObject::invokeMethod(
                &appController,
                [&appController, pending]() {
                    emit appController.uriRequested(pending);
                },
                Qt::QueuedConnection
            );
        }
    }

    // 2. Apply native window chrome to every top-level QQuickWindow
    //    the engine produced (transparent titlebar, Finder-like
    //    blended toolbar look). Per macOSplans/04 §3.
    {
        const auto roots = engine.rootObjects();
        for (QObject* obj : roots) {
            if (auto* w = qobject_cast<QQuickWindow*>(obj)) {
                ufb::applyMacWindowChrome(w);
            }
        }
    }
#endif

#ifdef Q_OS_WIN
    // Belt-and-suspenders for the Windows taskbar / Alt-Tab icon.
    // QGuiApplication::setWindowIcon() above SHOULD propagate to the
    // QQuickWindow's HWND via Qt's WM_SETICON path, but on freshly-
    // installed remote machines we've seen the taskbar fall back to
    // generic - a class of "Qt's icon plumbing didn't take" failures
    // (timing, window-handle realization, AUMID grouping, etc).
    //
    // Loading RT_GROUP_ICON id #1 directly from the exe's embedded
    // resource (app/ufb.rc) and pushing it via WM_SETICON skips
    // every layer that could fail. The big/small variants drive
    // taskbar (BIG) and titlebar (SMALL) respectively.
    {
        HMODULE mod = GetModuleHandleW(nullptr);
        HICON hIconBig = (HICON)LoadImageW(mod, MAKEINTRESOURCEW(1),
            IMAGE_ICON, GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
        HICON hIconSmall = (HICON)LoadImageW(mod, MAKEINTRESOURCEW(1),
            IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
        // Theme shell color (Theme.colors.surface = #1a1a1a), reused for
        // the QQuickWindow clear color and the native class brush.
        static HBRUSH s_winBgBrush = CreateSolidBrush(RGB(0x1a, 0x1a, 0x1a));
        const auto roots = engine.rootObjects();
        for (QObject* obj : roots) {
            auto* w = qobject_cast<QQuickWindow*>(obj);
            if (!w)
                continue;

            // winId() realizes the native HWND WITHOUT showing it — on
            // Windows, Main.qml deliberately leaves the window hidden
            // (it does NOT set visible=true) so we can do all of this
            // setup *before* the first show. Showing the window calls
            // ShowWindow, which sends WM_ERASEBKGND synchronously; if we
            // set up after that, the white flash has already happened.
            const HWND hwnd = reinterpret_cast<HWND>(w->winId());
            if (hIconBig)
                SendMessageW(hwnd, WM_SETICON, ICON_BIG,
                             reinterpret_cast<LPARAM>(hIconBig));
            if (hIconSmall)
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                             reinterpret_cast<LPARAM>(hIconSmall));

            // Kill the white startup flash. Before the first D3D11 frame
            // is presented the window would otherwise paint its default
            // (white) class brush. Three layers of defense:
            //   1. DWMWA_CLOAK hides the window from the DWM compositor
            //      while it renders its first frame, so nothing is shown
            //      on screen until real content exists. Uncloaked on the
            //      first frameSwapped.
            //   2. Dark RHI clear color → the first real frame is dark.
            //   3. Dark native class brush → any OS erase matches.
            BOOL cloak = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
            w->setColor(QColor(0x1a, 0x1a, 0x1a));
            SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND,
                             reinterpret_cast<LONG_PTR>(s_winBgBrush));

            auto uncloak = [hwnd]() {
                BOOL off = FALSE;
                DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &off, sizeof(off));
            };
            // Reveal once the first frame is actually on the swapchain.
            QObject::connect(w, &QQuickWindow::frameSwapped, w, uncloak,
                             Qt::SingleShotConnection);
            // Safety net: if no frame ever swaps (e.g. shown minimized),
            // uncloak anyway so the window can't stay invisible.
            QTimer::singleShot(1500, w, uncloak);

            // Now show — cloaked, so it renders off-screen first. Honour
            // the persisted maximized state that Main.qml stashed in
            // `startMaximized` (on Windows it does not show the window
            // itself, leaving that to us). --background skips the show
            // entirely: tray-only until the user opens the window.
            if (startInBackground)
                ; // stay hidden
            else if (w->property("startMaximized").toBool())
                w->setVisibility(QWindow::Maximized);
            else
                w->setVisible(true);
        }
    }
#endif

    return app.exec();
}
