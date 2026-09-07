#pragma once

#include <QObject>
#include <QAbstractNativeEventFilter>
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
//
// Also implements QAbstractNativeEventFilter to detect Explorer restarting:
// Explorer broadcasts the registered "TaskbarCreated" window message to
// every top-level window in the system exactly once, right after it has
// finished recreating its shell UI (this is the standard, well-established
// mechanism every Windows tray-icon app already relies on to know when to
// re-add its tray icon after an Explorer restart). This is purely
// event-driven - no polling, no periodic timer - and only fires on a
// genuine Explorer restart, never during normal playback.
class WallpaperManager : public QObject, public QAbstractNativeEventFilter {
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

    // QAbstractNativeEventFilter
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void errorOccurred(const QString& message);
    void wallpaperActivated();
    void wallpaperRemoved();

private slots:
    void onPlayerError(const QString& message);

private:
    void rebuildWindows();
    void attachAllWindows();
    void teardownWindows();
    void onExplorerRestarted();

    std::unique_ptr<VideoPlayer> m_player;
    std::vector<std::unique_ptr<WallpaperWindow>> m_windows;
    UINT m_taskbarCreatedMessage = 0;

    bool m_active = false;
    QString m_currentPath;
    ScalingMode m_scalingMode = ScalingMode::Fill;
    MonitorSelection m_monitorSelection = MonitorSelection::All;
    int m_specificMonitorIndex = -1;
    bool m_userPaused = false;
};
