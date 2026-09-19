#include <QApplication>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QIcon>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QSharedMemory>
#include <QStringList>
#include <cstdio>
#include <windows.h>
#include "MainWindow.h"
#include "StartupDiagnostics.h"
#include "InstanceIpc.h"
#include "Theme.h"
#include "SettingsManager.h"

namespace {

void fileLogHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    static QFile logFile([] {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        QDir().mkpath(dir);
        return dir + "/Motiva.log";
    }());
    static bool opened = [] {
        // A second launch attempt (see main()'s single-instance handling)
        // opens this same file for append at nearly the same instant the
        // running instance may itself be mid-write; a transient sharing
        // violation there was observed in testing to silently drop that
        // process's entire log output (including the "Second instance
        // detected"/IPC lines this file's whole purpose depends on for
        // diagnosing this exact scenario). A few short retries is enough -
        // this is a brief race, not a persistent lock.
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
                return true;
            }
            Sleep(20);
        }
        return false;
    }();
    if (!opened) {
        return;
    }
    const char* level = "INFO";
    if (type == QtWarningMsg) level = "WARN";
    else if (type == QtCriticalMsg) level = "ERROR";
    else if (type == QtFatalMsg) level = "FATAL";

    // Qt's Append mode only seeks to end-of-file once, at open() time - it
    // does not reseek before every write. With a second process (see
    // main()'s IPC hand-off on a second launch attempt) opening this same
    // file later and writing through its own handle, both processes' idea
    // of "end of file" can go stale as the other one keeps appending,
    // corrupting/interleaving lines (confirmed by testing: a second
    // process's line landed mid-way through an existing one). Seeking to
    // the true current end immediately before every write avoids this.
    logFile.seek(logFile.size());
    QTextStream stream(&logFile);
    stream << QDateTime::currentDateTime().toString("yyyy-MM-ddTHH:mm:ss.zzz") << " [" << level << "] " << msg << '\n';
    stream.flush();
}

} // namespace

int main(int argc, char* argv[]) {
    // Very first line: reference point for every StartupDiagnostics mark
    // recorded anywhere else in the process, including ones that could fire
    // before the log handler below is installed.
    StartupDiagnostics::instance().start();
    StartupDiagnostics::instance().mark("mainStart");
    qInstallMessageHandler(fileLogHandler);
    QApplication app(argc, argv);
    app.setApplicationName("Motiva");
    app.setOrganizationName("Motiva");
    app.setQuitOnLastWindowClosed(false);
    // Process-wide default so dialogs without an explicit icon (e.g. the
    // "no system tray" QMessageBox below) still show Motiva's own icon
    // rather than a generic one. MainWindow/its tray icon set the same
    // resource explicitly too - see MainWindow.cpp's kAppIconResourcePath.
    app.setWindowIcon(QIcon(":/application/motiva.ico"));
    // The selected Motiva Theme (see Theme.h) is the one, fully
    // deterministic source of truth for the app's appearance - applied
    // once here (both QPalette and the global stylesheet, together) so
    // there's no separate "half-styled" moment before MainWindow exists.
    // SettingsDialog re-applies live via Theme::applyTheme() when the
    // user picks a different theme mid-session, so this is only the
    // startup value. No transition overlay here - that's a live-change-
    // only affordance (see SettingsDialog).
    {
        SettingsManager startupSettings;
        const Theme::AppTheme theme = startupSettings.theme();
        startupSettings.setTheme(theme); // persists the one-time legacy migration, if any
        Theme::applyTheme(theme);
    }

    // Parsed once, before the single-instance branch below, since which
    // instance ends up handling it (this one, directly, vs. an existing
    // one via IPC) depends on the very next check.
    bool startMinimized = false;
    QString explorerSelectedFile;
    // QCoreApplication::arguments() (rather than a raw argv scan) handles
    // Windows' own command-line quoting correctly - Explorer's "Set as
    // background" verb supplies the selected path quoted, since it may
    // contain spaces (see WindowsShellIntegration::RegisterSetBackgroundVerb's
    // registered command).
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--autostart")) {
            startMinimized = true;
        } else if (args[i] == QLatin1String("--set-background") && i + 1 < args.size()) {
            explorerSelectedFile = args[++i];
        }
    }

    // Two instances would each attach their own competing render window to
    // the desktop (fighting over z-order every health-check tick), so only
    // ever allow one to run at a time.
    QSharedMemory singleInstanceLock("Motiva-SingleInstanceLock");
    if (!singleInstanceLock.create(1)) {
        // Another instance is already running. Instead of silently exiting
        // (which previously left the user no way to recover a stuck
        // instance short of End Task), ask it to recover/activate itself
        // via IPC, then exit - single-instance semantics are unchanged,
        // exactly one process ever runs. A file supplied via Explorer's
        // "Set as background" verb is forwarded to that existing instance
        // instead of a bare recovery request - no second Motiva process is
        // ever created for this.
        if (!explorerSelectedFile.isEmpty()) {
            InstanceIpc::sendSetBackgroundRequest(explorerSelectedFile);
        } else {
            InstanceIpc::sendRecoverRequest();
        }
        return 0;
    }

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, "Motiva",
            "No system tray was detected on this system. The application will still run.");
    }

    MainWindow window(startMinimized, explorerSelectedFile);
    if (!startMinimized) {
        window.show();
    }

    return app.exec();
}
