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
#include "RecoveryState.h"

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

    // Not owned; may be null (diagnostics-only, never load-bearing for
    // lifecycle decisions). Set once, before setWallpaper() is first
    // called.
    void setRecoveryState(RecoveryState* state) { m_recoveryState = state; }

    // Called when a second launch attempt asked this instance to recover
    // (see InstanceIpc): if no wallpaper is configured yet, nothing to do
    // here (MainWindow handles the "attach for the first time" case). If
    // one is active, re-drives the same attach+verify pipeline used for
    // Explorer-restart recovery (idempotent/single-flight - see
    // attachAllWindows()) rather than assuming the running instance is
    // already healthy.
    void recoverOrActivate();

    // QAbstractNativeEventFilter
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void errorOccurred(const QString& message);
    // Emitted as soon as setWallpaper() begins attaching - i.e. attach
    // REQUESTED/in-progress, not yet visually confirmed. UI code must not
    // treat this alone as "the wallpaper is active" - see wallpaperVerified.
    void wallpaperActivated();
    void wallpaperRemoved();
    // Emitted only once the existing post-attach verification (see
    // onAttachAttemptFinished's presentedFrameCount check) has confirmed
    // frames are actually advancing through to the desktop - this is the
    // one signal that corresponds to genuine "Active" state, distinct from
    // wallpaperActivated (attach merely requested). Added purely to let UI
    // code observe an existing internal checkpoint; the verification logic
    // itself is unchanged.
    void wallpaperVerified();

private slots:
    void onPlayerError(const QString& message);
    void onWindowReadyForReattach(bool ok);

private:
    // Checks whether Windows' own static-wallpaper path has changed since
    // m_wallpaperBaselineAtAttach was captured (see setWallpaper) - called
    // on every WM_SETTINGCHANGE while m_active, purely event-driven (no
    // polling). If it changed, the user picked a new wallpaper via
    // Explorer/Settings while our video was active, so we detach to let
    // their choice actually be visible instead of staying on top of it.
    void onPossibleExternalWallpaperChange();

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
    // Bumped every time the set of windows/HWNDs being attached changes
    // (a fresh attachAllWindows() call, or Explorer restarting mid-attempt
    // and recreating HWNDs out from under an in-flight one). The
    // in-flight attempt captures the generation it was started with; if
    // that no longer matches m_attachGeneration by the time it finishes,
    // its result refers to HWNDs/state that no longer exist and MUST be
    // discarded rather than applied - see onAttachAttemptFinished.
    quint64 m_attachGeneration = 0;
    // The generation m_attachGeneration held when the currently/most-
    // recently in-flight job was launched - compared against the live
    // m_attachGeneration in onAttachAttemptFinished to detect staleness.
    quint64 m_attachJobGeneration = 0;
    // Set in the destructor before tearing anything down, so a
    // just-finishing background attempt's completion handler (which can
    // still fire after ~this runs, since QFutureWatcher::finished is a
    // queued signal) knows not to touch already-destroyed WallpaperWindow
    // objects.
    bool m_shuttingDown = false;

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
    // Windows' own static-wallpaper path (SPI_GETDESKWALLPAPER), captured
    // right before we attach - see onPossibleExternalWallpaperChange.
    std::wstring m_wallpaperBaselineAtAttach;
    ScalingMode m_scalingMode = ScalingMode::Fill;
    MonitorSelection m_monitorSelection = MonitorSelection::All;
    int m_specificMonitorIndex = -1;
    bool m_userPaused = false;

    RecoveryState* m_recoveryState = nullptr;
    // Explorer's PID as of the last restart this instance observed (0 until
    // the first restart) - purely diagnostic, logged alongside the new PID
    // on each subsequent restart (see onExplorerRestarted).
    DWORD m_lastKnownExplorerPid = 0;
};
