#include "WallpaperManager.h"
#include <QDebug>
#include <QCoreApplication>

WallpaperManager::WallpaperManager(QObject* parent)
    : QObject(parent), m_player(std::make_unique<VideoPlayer>()) {
    connect(m_player.get(), &VideoPlayer::errorOccurred, this, &WallpaperManager::onPlayerError);

    // See the class comment: "TaskbarCreated" is the standard, purely
    // event-driven signal for "Explorer just finished restarting".
    m_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    qApp->installNativeEventFilter(this);
}

WallpaperManager::~WallpaperManager() {
    qApp->removeNativeEventFilter(this);
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
    return true;
}

void WallpaperManager::removeWallpaper() {
    if (!m_active) {
        return;
    }
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
    for (auto& w : m_windows) {
        HWND hwnd = w->handle();
        if (!WindowsDesktopWallpaper::AttachToDesktop(hwnd)) {
            // Do NOT fall back to showing this as a normal window - that
            // would just be a fake fullscreen overlay, which defeats the
            // whole point of a desktop wallpaper. Keep it hidden and
            // surface the failure instead.
            w->hideNative();
            emit errorOccurred(tr("Could not attach the wallpaper window to the desktop."));
            continue;
        }
        // AttachToDesktop already positions and shows the window (raw
        // Win32 SetWindowPos/ShowWindow) - this is a plain native window
        // with its own WndProc, not a Qt widget, so there is no separate
        // Qt-side visibility/paint state to reconcile here.
    }
}

void WallpaperManager::teardownWindows() {
    for (auto& w : m_windows) {
        WindowsDesktopWallpaper::DetachFromDesktop(w->handle());
        w->hideNative();
    }
    m_windows.clear();
}
