#include <QApplication>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QSharedMemory>
#include <cstdio>
#include "MainWindow.h"

namespace {

void fileLogHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    static QFile logFile([] {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        QDir().mkpath(dir);
        return dir + "/VideoWallpaper.log";
    }());
    static bool opened = logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    if (!opened) {
        return;
    }
    const char* level = "INFO";
    if (type == QtWarningMsg) level = "WARN";
    else if (type == QtCriticalMsg) level = "ERROR";
    else if (type == QtFatalMsg) level = "FATAL";

    QTextStream stream(&logFile);
    stream << QDateTime::currentDateTime().toString(Qt::ISODate) << " [" << level << "] " << msg << '\n';
    stream.flush();
}

} // namespace

int main(int argc, char* argv[]) {
    qInstallMessageHandler(fileLogHandler);
    QApplication app(argc, argv);
    app.setApplicationName("VideoWallpaper");
    app.setOrganizationName("VideoWallpaper");
    app.setQuitOnLastWindowClosed(false);

    // Two instances would each attach their own competing render window to
    // the desktop (fighting over z-order every health-check tick), so only
    // ever allow one to run at a time.
    QSharedMemory singleInstanceLock("VideoWallpaper-SingleInstanceLock");
    if (!singleInstanceLock.create(1)) {
        return 0;
    }

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, "Video Wallpaper",
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
