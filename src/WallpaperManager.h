#pragma once

#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QRect>
#include <QTimer>
#include <QElapsedTimer>
#include <QFutureWatcher>
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
    // Kicks off attachment of every not-yet-attached window on a
    // background thread (see m_attachWatcher) if one isn't already in
    // flight. Never blocks the calling (GUI) thread.
    void attachAllWindows();
    void onAttachAttemptFinished();
    void teardownWindows();
    void onExplorerRestarted();
    void onAttachRetryTick();

    std::unique_ptr<VideoPlayer> m_player;
    std::vector<std::unique_ptr<WallpaperWindow>> m_windows;
    UINT m_taskbarCreatedMessage = 0;

    // WindowsDesktopWallpaper::AttachToDesktop() performs undocumented
    // Progman/WorkerW discovery: SendMessageTimeoutW round-trips to
    // Explorer plus short Sleep-based waits for it to react. On a machine
    // where Explorer never creates a standalone icon-less WorkerW
    // (confirmed on this dev machine - see CLAUDE.md), that full sequence
    // runs on every attach attempt and was measured to take 7+ seconds by
    // itself under light load; under real boot-time CPU/disk contention
    // (antivirus scanning, other startup apps, Explorer's own shell
    // extensions loading) the same fixed sequence of waits can stretch to
    // well over a minute. Previously this ran synchronously on the GUI
    // thread - inside MainWindow's constructor, before the Qt event loop
    // even started - which is what actually produced the ~2 minute
    // "frozen"-looking startup/recovery delay: the whole process
    // (including the message pump needed to receive Explorer's
    // TaskbarCreated broadcast) was blocked for as long as discovery took.
    // Running it via QtConcurrent on a worker thread and only marshaling
    // the boolean result back removes that block entirely; the app stays
    // responsive and reattaches as soon as discovery actually finishes,
    // however long that takes on a given machine/boot.
    QFutureWatcher<bool> m_attachWatcher;
    bool m_attachInFlight = false;
    QElapsedTimer m_attachAttemptElapsed;

    // The Explorer desktop hierarchy (Progman -> SHELLDLL_DefView) is not
    // always present the instant this app starts (observed on boot: this
    // app can start before Explorer has finished building the desktop
    // icon layer). There is no OS notification for "the desktop icon
    // layer just became ready", so once an attach attempt fails we poll
    // for it at a short, fixed interval (not a long arbitrary timeout)
    // and stop the instant it succeeds - this is a bounded "wait for a
    // condition" loop, not a periodic health-check/reattach loop (which
    // was previously the source of z-order flicker; see
    // WindowsDesktopWallpaper.cpp's NeedsReattach comment). Also used as
    // a safety net after TaskbarCreated if the icon layer isn't back yet
    // at that exact moment. Each tick just (re)starts the background
    // attach attempt above - it never blocks itself.
    QTimer m_attachRetryTimer;
    QElapsedTimer m_attachRetryElapsed;

    bool m_active = false;
    QString m_currentPath;
    ScalingMode m_scalingMode = ScalingMode::Fill;
    MonitorSelection m_monitorSelection = MonitorSelection::All;
    int m_specificMonitorIndex = -1;
    bool m_userPaused = false;
};
