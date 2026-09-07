#include "SettingsManager.h"
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <windows.h>

namespace {
constexpr const char* kRunKeyPath = R"(Software\Microsoft\Windows\CurrentVersion\Run)";
constexpr const wchar_t* kRunValueName = L"VideoWallpaper";
}

SettingsManager::SettingsManager()
    : m_settings(QSettings::NativeFormat, QSettings::UserScope, "VideoWallpaper", "VideoWallpaper") {
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
        QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
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
