#include "WallpaperManager.h"
#include <QDebug>
#include <QCoreApplication>
#include <QtConcurrent/QtConcurrentRun>

WallpaperManager::WallpaperManager(QObject* parent)
    : QObject(parent), m_player(std::make_unique<VideoPlayer>()) {
    connect(m_player.get(), &VideoPlayer::errorOccurred, this, &WallpaperManager::onPlayerError);

    // See the class comment: "TaskbarCreated" is the standard, purely
    // event-driven signal for "Explorer just finished restarting".
    m_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    qApp->installNativeEventFilter(this);

    connect(&m_attachWatcher, &QFutureWatcher<bool>::finished, this, &WallpaperManager::onAttachAttemptFinished);

    // Short, self-terminating poll for desktop-hierarchy readiness - see
    // the m_attachRetryTimer comment in the header for why this exists
    // instead of a fixed startup delay.
    m_attachRetryTimer.setInterval(300);
    connect(&m_attachRetryTimer, &QTimer::timeout, this, &WallpaperManager::onAttachRetryTick);
}

WallpaperManager::~WallpaperManager() {
    qApp->removeNativeEventFilter(this);
    if (m_attachWatcher.isRunning()) {
        m_attachWatcher.waitForFinished();
    }
    removeWallpaper();
}

bool WallpaperManager::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
    Q_UNUSED(result);
    // Native events on Windows are always MSG-shaped for both event-type
    // strings Qt has used across versions; confirm before trusting the
    // cast rather than assuming.
    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG") {
        return false;
    }
    auto* msg = static_cast<MSG*>(message);
    if (m_taskbarCreatedMessage != 0 && msg->message == m_taskbarCreatedMessage) {
        onExplorerRestarted();
    }
    return false; // never swallow the message - other listeners may need it too
}

bool WallpaperManager::setWallpaper(const QString& videoPath) {
    qInfo() << "[Lifecycle] setWallpaper begin, path=" << videoPath;
    if (!m_player->loadFile(videoPath)) {
        return false;
    }
    m_currentPath = videoPath;
    m_player->setLooping(m_player->isLooping());

    rebuildWindows();
    attachAllWindows();

    m_player->play();
    m_userPaused = false;
    m_active = true;
    emit wallpaperActivated();
    qInfo() << "[Lifecycle] setWallpaper returning - desktop attach continues "
                "asynchronously in the background (see [Shell]/[Discover] log lines).";
    return true;
}

void WallpaperManager::removeWallpaper() {
    if (!m_active) {
        return;
    }
    m_attachRetryTimer.stop();
    m_player->stop();
    teardownWindows();
    m_active = false;
    emit wallpaperRemoved();
}

void WallpaperManager::setLooping(bool loop) {
    m_player->setLooping(loop);
}

void WallpaperManager::setVolume(int percent) {
    m_player->setVolume(percent);
}

void WallpaperManager::setMuted(bool muted) {
    m_player->setMuted(muted);
}

void WallpaperManager::setScalingMode(ScalingMode mode) {
    m_scalingMode = mode;
    for (auto& w : m_windows) {
        w->setScalingMode(mode);
    }
}

void WallpaperManager::setMonitorSelection(MonitorSelection selection, int specificIndex) {
    m_monitorSelection = selection;
    m_specificMonitorIndex = specificIndex;
    if (m_active) {
        rebuildWindows();
        attachAllWindows();
    }
}

void WallpaperManager::play() {
    m_userPaused = false;
    m_player->play();
}

void WallpaperManager::pause() {
    m_userPaused = true;
    m_player->pause();
}

bool WallpaperManager::isPlaying() const {
    return m_player->isPlaying();
}

std::vector<MonitorInfoData> WallpaperManager::availableMonitors() const {
    return WindowsDesktopWallpaper::EnumerateMonitors();
}

void WallpaperManager::onPlayerError(const QString& message) {
    emit errorOccurred(message);
}

void WallpaperManager::onExplorerRestarted() {
    if (!m_active || m_windows.empty()) {
        return;
    }
    // This only ever runs in response to the "TaskbarCreated" broadcast
    // (see nativeEventFilter) - i.e. a genuine, one-time Explorer restart
    // event, never a periodic check. Each WallpaperWindow recreates its
    // native HWND and asynchronously rebinds to it (see
    // WallpaperWindow::recoverFromExplorerRestart/onRebindFinished -
    // deliberately non-blocking); the video decoder (m_player) and every
    // window's D3D device/swapchain/visual/shaders/textures are left
    // completely untouched. Failure is reported via each window's own
    // logging as recovery proceeds asynchronously, not returned here.
    qInfo() << "[Shell] Explorer restart detected - recovering wallpaper.";
    for (auto& w : m_windows) {
        w->recoverFromExplorerRestart();
    }
    // recoverFromExplorerRestart's own async rebind (onRebindFinished)
    // calls AttachToDesktop once the new HWND/DComp target are ready.
    // TaskbarCreated can fire slightly ahead of the desktop icon layer
    // being rebuilt though, so also arm the short readiness-poll as a
    // safety net in case that first reattach attempt is too early.
    if (!m_attachRetryTimer.isActive()) {
        m_attachRetryElapsed.start();
        m_attachRetryTimer.start();
    }
}

void WallpaperManager::onAttachRetryTick() {
    if (!m_active || m_windows.empty()) {
        m_attachRetryTimer.stop();
        return;
    }
    // attachAllWindows() is fully non-blocking now (see m_attachWatcher) -
    // this tick just (re)kicks it off; onAttachAttemptFinished decides
    // whether to keep polling.
    attachAllWindows();
}

void WallpaperManager::rebuildWindows() {
    teardownWindows();

    const auto monitors = availableMonitors();
    if (monitors.empty()) {
        return;
    }

    auto makeWindowFor = [this](const MonitorInfoData& mon) {
        auto win = std::make_unique<WallpaperWindow>(m_player.get());
        win->setScalingMode(m_scalingMode);
        QRect r(mon.rect.left, mon.rect.top,
                mon.rect.right - mon.rect.left,
                mon.rect.bottom - mon.rect.top);
        win->setMonitorRect(r);
        // Deliberately not shown here: showing a normal top-level window
        // would put it on top of everything else, which is exactly the
        // "fake fullscreen wallpaper" behavior we must avoid. It is only
        // made visible once it has been successfully reparented behind
        // the desktop icons in attachAllWindows().
        m_windows.push_back(std::move(win));
    };

    switch (m_monitorSelection) {
        case MonitorSelection::All:
            for (const auto& mon : monitors) {
                makeWindowFor(mon);
            }
            break;
        case MonitorSelection::Primary:
            for (const auto& mon : monitors) {
                if (mon.isPrimary) {
                    makeWindowFor(mon);
                    break;
                }
            }
            break;
        case MonitorSelection::Specific:
            if (m_specificMonitorIndex >= 0 &&
                m_specificMonitorIndex < static_cast<int>(monitors.size())) {
                makeWindowFor(monitors[m_specificMonitorIndex]);
            }
            break;
    }
}

void WallpaperManager::attachAllWindows() {
    if (m_attachInFlight) {
        // Already discovering/attaching on the background thread; let
        // that attempt finish rather than piling up overlapping Win32
        // discovery calls (which contend on the same static cooldown
        // state in WindowsDesktopWallpaper and gain nothing by racing).
        return;
    }
    if (m_windows.empty()) {
        return;
    }
    m_attachInFlight = true;
    m_attachAttemptElapsed.start();

    // The actual Win32 work (WindowsDesktopWallpaper::AttachToDesktop, see
    // its header comment for why) can block for anywhere from
    // milliseconds to well over a minute depending on how quickly
    // Explorer's desktop shell responds - see the m_attachWatcher comment
    // in the header. Running it here, on a QtConcurrent worker thread,
    // keeps the GUI thread (and this process's Windows message pump)
    // free the entire time.
    std::vector<HWND> hwnds;
    hwnds.reserve(m_windows.size());
    for (auto& w : m_windows) {
        hwnds.push_back(w->handle());
    }

    QFuture<bool> future = QtConcurrent::run([hwnds]() {
        bool allOk = true;
        for (HWND hwnd : hwnds) {
            QElapsedTimer t;
            t.start();
            const bool ok = WindowsDesktopWallpaper::AttachToDesktop(hwnd);
            qInfo() << "[Shell] AttachToDesktop hwnd=" << reinterpret_cast<quintptr>(hwnd)
                    << "result=" << ok << "took" << t.elapsed() << "ms";
            if (!ok) {
                allOk = false;
            }
        }
        return allOk;
    });
    m_attachWatcher.setFuture(future);
}

void WallpaperManager::onAttachAttemptFinished() {
    m_attachInFlight = false;
    const bool allOk = m_attachWatcher.result();

    if (allOk) {
        for (auto& w : m_windows) {
            // AttachToDesktop already positions and shows each window
            // (raw Win32 SetWindowPos/ShowWindow) - this is a plain
            // native window with its own WndProc, not a QWidget, so
            // there is no separate Qt-side visibility state to reconcile.
            Q_UNUSED(w);
        }
        if (m_attachRetryTimer.isActive()) {
            qInfo() << "[Shell] Desktop hierarchy became ready after"
                    << m_attachRetryElapsed.elapsed() << "ms - wallpaper attached.";
            m_attachRetryTimer.stop();
        }
        return;
    }

    // Do NOT fall back to showing any window as a normal top-level window
    // - that would just be a fake fullscreen overlay, which defeats the
    // whole point of a desktop wallpaper. Keep failed ones hidden and
    // keep waiting instead.
    for (auto& w : m_windows) {
        w->hideNative();
    }

    if (!m_attachRetryTimer.isActive()) {
        qInfo() << "[Shell] Desktop hierarchy not fully ready yet - "
                    "will keep checking at a short interval until it is.";
        emit errorOccurred(tr("Could not attach the wallpaper window to the desktop."));
        m_attachRetryElapsed.start();
        m_attachRetryTimer.start();
    } else if (m_attachRetryElapsed.elapsed() % 5000 < 1000) {
        // Not a fixed timeout and not fatal - just make the ongoing wait
        // visible in the log instead of silently polling forever.
        qInfo() << "[Shell] Still waiting for desktop hierarchy... elapsed="
                << m_attachRetryElapsed.elapsed() << "ms";
    }
}

void WallpaperManager::teardownWindows() {
    for (auto& w : m_windows) {
        WindowsDesktopWallpaper::DetachFromDesktop(w->handle());
        w->hideNative();
    }
    m_windows.clear();
}
