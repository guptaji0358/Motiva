#include "SettingsManager.h"
#include "WindowsShellIntegration.h"
#include "MainWindow.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <windows.h>

namespace {
constexpr const char* kRunKeyPath = R"(Software\Microsoft\Windows\CurrentVersion\Run)";
constexpr const wchar_t* kRunValueName = L"Motiva";
} // namespace

// In a normal dev build, applicationFilePath() is exactly what should be
// launched. In the packaged deployment layout (see the "Organize Final
// Deployment Output" task), the real Qt-linked binary instead runs at
// resources/bin/<exe>, launched indirectly by a tiny native launcher at
// the deployment root that adds resources/Qt to the child process's DLL
// search PATH before creating it - the real binary's own implicitly-
// linked Qt/FFmpeg DLLs can only be found that way, since nothing inside
// the real binary's own startup code can redirect its own load-time
// imports. Autostart, the Start Menu shortcut, and the Explorer "Set as
// background" verb's command must therefore all point at that same
// launcher in a deployed install, or they'd silently fail/point at a
// binary that can't find its own DLLs. Detected purely from directory
// shape (applicationDirPath() ending in resources/bin, with a same-named
// exe present one level above resources/) - a normal dev build's
// applicationDirPath() never matches this, so its resolved path is
// unaffected.
QString SettingsManager::motivaExecutablePath() {
    const QString ownPath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    QDir binDir(QCoreApplication::applicationDirPath());
    if (binDir.dirName().compare("bin", Qt::CaseInsensitive) != 0) {
        // Dev build tree (<repo>/build/Motiva.exe): its Qt/FFmpeg DLLs are
        // not beside it, so it is NOT a valid target for shortcuts/verbs
        // (this was the Windows Search DLL-not-found bug). Prefer the
        // sibling deployment output the `deploy` target produces
        // (<repo>/deployment/Motiva.exe + resources/bin/Motiva.exe),
        // located relative to this exe, never by a hardcoded path.
        QDir parent = binDir;
        if (parent.cdUp()) {
            const QString launcher = parent.filePath(QStringLiteral("deployment/") + QFileInfo(ownPath).fileName());
            const QString realBin = parent.filePath(QStringLiteral("deployment/resources/bin/") + QFileInfo(ownPath).fileName());
            if (QFileInfo::exists(launcher) && QFileInfo::exists(realBin)) {
                return QDir::toNativeSeparators(QFileInfo(launcher).absoluteFilePath());
            }
        }
        return ownPath;
    }
    QDir resourcesDir = binDir;
    resourcesDir.cdUp();
    if (resourcesDir.dirName().compare("resources", Qt::CaseInsensitive) != 0) {
        return ownPath;
    }
    QDir deployRoot = resourcesDir;
    deployRoot.cdUp();
    const QString launcherPath = deployRoot.filePath(QFileInfo(ownPath).fileName());
    if (QFileInfo::exists(launcherPath)) {
        return QDir::toNativeSeparators(launcherPath);
    }
    return ownPath;
}

SettingsManager::SettingsManager()
    : m_settings(QSettings::NativeFormat, QSettings::UserScope, "Motiva", "Motiva") {
}

QString SettingsManager::videoPath() const {
    return m_settings.value("video/path").toString();
}
void SettingsManager::setVideoPath(const QString& path) {
    m_settings.setValue("video/path", path);
}

int SettingsManager::volume() const {
    return m_settings.value("video/volume", 0).toInt();
}
void SettingsManager::setVolume(int percent) {
    m_settings.setValue("video/volume", percent);
}

bool SettingsManager::muted() const {
    return m_settings.value("video/muted", true).toBool();
}
void SettingsManager::setMuted(bool muted) {
    m_settings.setValue("video/muted", muted);
}

bool SettingsManager::loop() const {
    return m_settings.value("video/loop", true).toBool();
}
void SettingsManager::setLoop(bool loop) {
    m_settings.setValue("video/loop", loop);
}

int SettingsManager::scalingMode() const {
    return m_settings.value("video/scalingMode", 0).toInt();
}
void SettingsManager::setScalingMode(int mode) {
    m_settings.setValue("video/scalingMode", mode);
}

int SettingsManager::monitorSelection() const {
    return m_settings.value("display/monitorSelection", 0).toInt();
}
void SettingsManager::setMonitorSelection(int selection) {
    m_settings.setValue("display/monitorSelection", selection);
}

int SettingsManager::specificMonitorIndex() const {
    return m_settings.value("display/specificMonitorIndex", -1).toInt();
}
void SettingsManager::setSpecificMonitorIndex(int index) {
    m_settings.setValue("display/specificMonitorIndex", index);
}

bool SettingsManager::startWithWindows() const {
    return m_settings.value("app/startWithWindows", false).toBool();
}

void SettingsManager::setStartWithWindows(bool enabled) {
    m_settings.setValue("app/startWithWindows", enabled);

    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }

    if (enabled) {
        QString exePath = motivaExecutablePath();
        QString cmd = "\"" + exePath + "\" --autostart";
        std::wstring wcmd = cmd.toStdWString();
        RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(wcmd.c_str()),
                        static_cast<DWORD>((wcmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValueName);
    }
    RegCloseKey(key);
}

bool SettingsManager::wasWallpaperActive() const {
    return m_settings.value("app/wasWallpaperActive", false).toBool();
}
void SettingsManager::setWasWallpaperActive(bool active) {
    m_settings.setValue("app/wasWallpaperActive", active);
}

bool SettingsManager::showVideoOnBattery() const {
    return m_settings.value("app/showVideoOnBattery", true).toBool();
}
void SettingsManager::setShowVideoOnBattery(bool enabled) {
    m_settings.setValue("app/showVideoOnBattery", enabled);
}

bool SettingsManager::showInWindowsSearch() const {
    return m_settings.value("app/showInWindowsSearch", false).toBool();
}
void SettingsManager::setShowInWindowsSearch(bool enabled) {
    m_settings.setValue("app/showInWindowsSearch", enabled);
    if (enabled) {
        WindowsShellIntegration::CreateStartMenuShortcut(motivaExecutablePath());
    } else {
        WindowsShellIntegration::RemoveStartMenuShortcut();
    }
}

bool SettingsManager::explorerIntegrationEnabled() const {
    return m_settings.value("app/explorerIntegrationEnabled", false).toBool();
}
void SettingsManager::setExplorerIntegrationEnabled(bool enabled) {
    m_settings.setValue("app/explorerIntegrationEnabled", enabled);
    const QStringList extensions = MainWindow::explorerIntegrationExtensions();
    if (enabled) {
        WindowsShellIntegration::RegisterSetBackgroundVerb(motivaExecutablePath(), extensions);
    } else {
        WindowsShellIntegration::UnregisterSetBackgroundVerb(extensions);
    }
}

Theme::AppTheme SettingsManager::theme() const {
    if (m_settings.contains("app/theme")) {
        return Theme::themeFromSettingsKey(m_settings.value("app/theme").toString(), Theme::AppTheme::DarkAurora);
    }
    // One-time migration from the old Appearance x Visual Style settings
    // (pre-Theme-system installs) - see Theme::migrateLegacySettings().
    // Once "app/theme" is written below (by setTheme(), called right
    // after this on the same startup path - see main.cpp), these legacy
    // keys are never read again.
    if (m_settings.contains("app/uiStyle") || m_settings.contains("app/appearance")) {
        const int legacyAppearance = m_settings.value("app/appearance", 0).toInt();
        const int legacyUiStyle = m_settings.value("app/uiStyle", 0).toInt();
        return Theme::migrateLegacySettings(legacyAppearance, legacyUiStyle);
    }
    return Theme::AppTheme::DarkAurora;
}
void SettingsManager::setTheme(Theme::AppTheme theme) {
    m_settings.setValue("app/theme", Theme::themeSettingsKey(theme));
}
