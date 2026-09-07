#include "WallpaperManager.h"
#include <QDebug>

WallpaperManager::WallpaperManager(QObject* parent)
    : QObject(parent), m_player(std::make_unique<VideoPlayer>()) {
    connect(m_player.get(), &VideoPlayer::errorOccurred, this, &WallpaperManager::onPlayerError);

    // Explorer can restart (crash, "Restart Explorer" from Task Manager,
    // shell updates). When that happens our reparented windows get
    // destroyed along with the old WorkerW. Poll periodically and
    // reattach automatically.
    connect(&m_healthTimer, &QTimer::timeout, this, &WallpaperManager::checkWorkerWHealth);
    m_healthTimer.setInterval(3000);
}

WallpaperManager::~WallpaperManager() {
    removeWallpaper();
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
    m_healthTimer.start();
    emit wallpaperActivated();
    return true;
}

void WallpaperManager::removeWallpaper() {
    if (!m_active) {
        return;
    }
    m_healthTimer.stop();
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

void WallpaperManager::checkWorkerWHealth() {
    if (!m_active || m_windows.empty()) {
        return;
    }
    // The ONLY condition that should ever cause us to touch the window
    // hierarchy again after the initial attach is Explorer having
    // genuinely restarted (crash, "Restart Explorer", shell update).
    // Previous versions of this check also reattached on any perceived
    // z-order drift (GW_HWNDPREV no longer being Progman) - but ordinary
    // desktop use (opening a window, Start Menu, etc.) constantly shuffles
    // top-level z-order, so that fired far too often and was the actual
    // cause of visible flicker: SetParent/SetWindowPos/ShowWindow being
    // re-run during normal use instead of "attach once, leave alone".
    if (WindowsDesktopWallpaper::NeedsReattach()) {
        qWarning() << "Explorer restart detected - reattaching wallpaper";
        attachAllWindows();
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
