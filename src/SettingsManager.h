#pragma once

#include <QSettings>
#include <QString>

// Thin wrapper around QSettings (stored in the registry under
// HKCU\Software\Motiva via the default Windows QSettings backend).
// No cloud/account/internet dependency.
class SettingsManager {
public:
    SettingsManager();

    QString videoPath() const;
    void setVideoPath(const QString& path);

    int volume() const;
    void setVolume(int percent);

    bool muted() const;
    void setMuted(bool muted);

    bool loop() const;
    void setLoop(bool loop);

    int scalingMode() const; // maps to ScalingMode enum int
    void setScalingMode(int mode);

    int monitorSelection() const; // maps to MonitorSelection enum int
    void setMonitorSelection(int selection);

    int specificMonitorIndex() const;
    void setSpecificMonitorIndex(int index);

    bool startWithWindows() const;
    void setStartWithWindows(bool enabled);

    bool wasWallpaperActive() const;
    void setWasWallpaperActive(bool active);

private:
    QSettings m_settings;
};
