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
    // Single centralized stylesheet for the whole app (see Theme.h) -
    // purely visual, applied once here rather than scattered per-widget
    // setStyleSheet() calls throughout MainWindow/SettingsDialog. Which of
    // the two named styles (Modern Aurora / Motiva Onyx) is read from the
    // persisted setting so a choice made in a previous session survives a
    // relaunch; SettingsDialog re-applies live via qApp->setStyleSheet()
    // when the user changes it mid-session, so this is only the initial
    // value.
    // Must run before any setPalette() call - this is the only chance to
    // remember what the real OS-driven palette was, so a later "System"
    // appearance choice has something genuine to restore to.
    Theme::captureSystemPalette();
    {
        SettingsManager startupSettings;
        app.setStyleSheet(Theme::appStyleSheet(static_cast<Theme::StyleId>(startupSettings.uiStyle())));
        Theme::applyAppearance(static_cast<Theme::AppearanceId>(startupSettings.appearance()));
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
        // exactly one process ever runs.
        InstanceIpc::sendRecoverRequest();
        return 0;
    }

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, "Motiva",
            "No system tray was detected on this system. The application will still run.");
    }

    bool startMinimized = false;
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == "--autostart") {
            startMinimized = true;
        }
    }

    MainWindow window(startMinimized);
    if (!startMinimized) {
        window.show();
    }

    return app.exec();
}
