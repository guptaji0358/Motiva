#pragma once

#include <QObject>
#include <QTimer>
#include <QRect>
#include <memory>
#include <vector>
#include "VideoPlayer.h"
#include "WallpaperWindow.h"
#include "WindowsDesktopWallpaper.h"

enum class MonitorSelection {
    All,
    Primary,
    Specific // index into WindowsDesktopWallpaper::EnumerateMonitors()
};

// Orchestrates: video decode pipeline + one WallpaperWindow per target
// monitor + attaching those windows behind the desktop icons via
// WindowsDesktopWallpaper. This is the only class that should touch both
// the UI-facing settings and the Win32 desktop-integration module.
class WallpaperManager : public QObject {
    Q_OBJECT
public:
    explicit WallpaperManager(QObject* parent = nullptr);
    ~WallpaperManager() override;

    bool setWallpaper(const QString& videoPath);
    void removeWallpaper();
    bool isActive() const { return m_active; }

    void setLooping(bool loop);
    void setVolume(int percent);
    void setMuted(bool muted);
    void setScalingMode(ScalingMode mode);
    void setMonitorSelection(MonitorSelection selection, int specificIndex = -1);

    void play();
    void pause();
    bool isPlaying() const;

    std::vector<MonitorInfoData> availableMonitors() const;

    VideoPlayer* player() { return m_player.get(); }

signals:
    void errorOccurred(const QString& message);
    void wallpaperActivated();
    void wallpaperRemoved();

private slots:
    void onPlayerError(const QString& message);
    void checkWorkerWHealth();

private:
    void rebuildWindows();
    void attachAllWindows();
    void teardownWindows();

    std::unique_ptr<VideoPlayer> m_player;
    std::vector<std::unique_ptr<WallpaperWindow>> m_windows;
    QTimer m_healthTimer;

    bool m_active = false;
    QString m_currentPath;
    ScalingMode m_scalingMode = ScalingMode::Fill;
    MonitorSelection m_monitorSelection = MonitorSelection::All;
    int m_specificMonitorIndex = -1;
    bool m_userPaused = false;
};
