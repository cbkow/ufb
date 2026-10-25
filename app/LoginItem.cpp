// LoginItem — see LoginItem.h.

#ifdef _WIN32
// C++/WinRT before anything that drags in <windows.h> with min/max macros.
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <appmodel.h>   // GetCurrentPackageFullName
#  include <winrt/Windows.Foundation.h>
#  include <winrt/Windows.ApplicationModel.h>
#  include <winrt/Windows.ApplicationModel.Activation.h>
#  include <thread>
#endif

#include "LoginItem.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QPointer>
#include <QSettings>
#include <QUrl>

#ifdef _WIN32
namespace {

// Must match the StartupTask TaskId in packaging/windows/msix/AppxManifest.xml.in.
constexpr wchar_t kStartupTaskId[] = L"ufbStartup";

// The HKCU Run value the Inno installer's app_autostart task wrote.
const char kRunKey[] = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const char kRunValue[] = "UFB";

using winrt::Windows::ApplicationModel::StartupTask;
using winrt::Windows::ApplicationModel::StartupTaskState;

// WinRT calls run on a throwaway MTA thread: the GUI thread is Qt's OLE
// STA (clipboard, drag and drop), and initialising it as MTA first would
// make Qt's OleInitialize fail; blocking .get() on an STA is off-limits.
template <typename F>
void onMtaThread(F&& f, bool wait)
{
    std::thread t([f = std::forward<F>(f)]() mutable {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            f();
        } catch (const winrt::hresult_error& e) {
            qWarning("LoginItem: WinRT call failed: 0x%08x %s",
                     static_cast<unsigned>(e.code().value),
                     qPrintable(QString::fromWCharArray(e.message().c_str())));
        } catch (...) {
            qWarning("LoginItem: WinRT call failed");
        }
        winrt::uninit_apartment();
    });
    if (wait) t.join();
    else t.detach();
}

bool stateEnabled(StartupTaskState s)
{
    return s == StartupTaskState::Enabled || s == StartupTaskState::EnabledByPolicy;
}

bool stateBlocked(StartupTaskState s)
{
    // The app can't change these; only the user (Settings / Task
    // Manager) or policy can.
    return s == StartupTaskState::DisabledByUser
        || s == StartupTaskState::DisabledByPolicy
        || s == StartupTaskState::EnabledByPolicy;
}

QString runCommand()
{
    return QStringLiteral("\"%1\" --background")
        .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
}

}  // namespace
#endif

LoginItem::LoginItem(QObject* parent)
    : QObject(parent)
    , m_packaged(isPackaged())
{
    refresh();
}

bool LoginItem::isPackaged()
{
#ifdef _WIN32
    UINT32 len = 0;
    return GetCurrentPackageFullName(&len, nullptr) == ERROR_INSUFFICIENT_BUFFER;
#else
    return false;
#endif
}

bool LoginItem::launchedByStartupTask()
{
#ifdef _WIN32
    if (!isPackaged()) return false;
    bool viaTask = false;
    onMtaThread([&viaTask]() {
        using namespace winrt::Windows::ApplicationModel;
        const auto args = AppInstance::GetActivatedEventArgs();
        viaTask = args && args.Kind() == Activation::ActivationKind::StartupTask;
    }, /*wait=*/true);
    return viaTask;
#else
    return false;
#endif
}

bool LoginItem::available() const
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

void LoginItem::apply(bool enabled, bool blocked)
{
    if (enabled == m_enabled && blocked == m_blocked) return;
    m_enabled = enabled;
    m_blocked = blocked;
    emit changed();
}

void LoginItem::refresh()
{
#ifdef _WIN32
    if (!m_packaged) {
        QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
        apply(run.contains(QString::fromLatin1(kRunValue)), false);
        return;
    }
    QPointer<LoginItem> self(this);
    onMtaThread([self]() {
        const StartupTaskState s = StartupTask::GetAsync(kStartupTaskId).get().State();
        QMetaObject::invokeMethod(qApp, [self, s]() {
            if (self) self->apply(stateEnabled(s), stateBlocked(s));
        }, Qt::QueuedConnection);
    }, /*wait=*/false);
#endif
}

void LoginItem::setEnabled(bool on)
{
#ifdef _WIN32
    if (!m_packaged) {
        QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
        if (on) run.setValue(QString::fromLatin1(kRunValue), runCommand());
        else run.remove(QString::fromLatin1(kRunValue));
        run.sync();
        refresh();
        return;
    }
    QPointer<LoginItem> self(this);
    onMtaThread([self, on]() {
        const StartupTask task = StartupTask::GetAsync(kStartupTaskId).get();
        StartupTaskState s = task.State();
        if (on && !stateEnabled(s) && !stateBlocked(s))
            s = task.RequestEnableAsync().get();
        else if (!on && s == StartupTaskState::Enabled) {
            task.Disable();
            s = task.State();
        }
        QMetaObject::invokeMethod(qApp, [self, s]() {
            if (self) self->apply(stateEnabled(s), stateBlocked(s));
        }, Qt::QueuedConnection);
    }, /*wait=*/false);
#else
    Q_UNUSED(on);
#endif
}

void LoginItem::openSystemSettings()
{
#ifdef _WIN32
    QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:startupapps")));
#endif
}
