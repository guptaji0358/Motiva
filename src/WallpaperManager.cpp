#include "WallpaperManager.h"
#include <QDebug>
#include <QCoreApplication>
#include <QTimer>
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
    qInfo() << "[Lifecycle] WallpaperManager shutting down.";
    // Once this is set, onAttachAttemptFinished will discard whatever a
    // still-in-flight background attach job returns instead of touching
    // m_windows (which teardownWindows()/removeWallpaper() below is about
    // to destroy) - no background worker gets a chance to keep the
    // process alive or dereference freed WallpaperWindow objects.
    m_shuttingDown = true;
    m_attachRetryTimer.stop();
    qApp->removeNativeEventFilter(this);
    if (m_attachWatcher.isRunning()) {
        // The Win32 calls inside AttachToDesktop don't take an external
        // cancellation token, so this waits for the in-flight attempt to
        // actually finish (bounded by however long Explorer takes to
        // respond, same as any other attach attempt) rather than tearing
        // down HWNDs out from under it mid-call.
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
        qInfo() << "[Shell] TaskbarCreated message received (hwnd=" << reinterpret_cast<quintptr>(msg->hwnd)
                << "msgId=" << m_taskbarCreatedMessage << ").";
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
    qInfo() << "[Shell] Explorer restart detected - recovering wallpaper. VideoWallpaper.exe PID="
            << GetCurrentProcessId() << "(process, video decoder and D3D/DComp device/swapchain "
                                          "are NOT touched by this).";
    for (auto& w : m_windows) {
        WindowsDesktopWallpaper::DumpDesktopState(w->handle(), "[pre-recovery]");
    }

    // Bump the generation FIRST, before touching any window: any attach
    // attempt already in flight on the background thread was called with
    // the OLD HWNDs, which recoverFromExplorerRestart() below is about to
    // destroy. That background call still runs to completion (Win32 APIs
    // given a since-destroyed HWND just fail gracefully - they don't
    // block or crash), but its eventual result must never be applied to
    // m_windows once it no longer describes reality. onAttachAttemptFinished
    // checks this and discards stale results instead of e.g. hiding a
    // brand-new window because an old, unrelated attach attempt failed.
    ++m_attachGeneration;

    // Each WallpaperWindow recreates its native HWND and asynchronously
    // rebinds to it (see WallpaperWindow::recoverFromExplorerRestart /
    // onRebindFinished - deliberately non-blocking); the video decoder
    // (m_player) and every window's D3D device/swapchain/visual/shaders/
    // textures are left completely untouched. Once a window's rebind
    // finishes it emits readyForReattach (see onWindowReadyForReattach),
    // which is what actually re-triggers WindowsDesktopWallpaper::
    // AttachToDesktop() - never called directly from here or from
    // WallpaperWindow itself, to keep exactly one call site touching that
    // shared discovery state (see WindowsDesktopWallpaper.cpp).
    for (auto& w : m_windows) {
        w->recoverFromExplorerRestart();
    }

    // TaskbarCreated can fire slightly ahead of the desktop icon layer
    // being rebuilt, so also arm the short readiness-poll as a safety net
    // in case the reattach driven by readyForReattach above is too early.
    if (!m_attachRetryTimer.isActive()) {
        m_attachRetryElapsed.start();
        m_attachRetryTimer.start();
    }
}

void WallpaperManager::onWindowReadyForReattach(bool ok) {
    if (!ok) {
        qWarning() << "[Wallpaper] A window's post-restart DComp rebind failed; "
                       "will keep retrying via the readiness poll.";
    }
    if (!m_active || m_shuttingDown) {
        return;
    }
    // Kick off (or let an already-running one continue toward) a fresh
    // attach attempt now that at least one window's native side is ready
    // again, instead of waiting for the next 300ms poll tick.
    attachAllWindows();
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
        connect(win.get(), &WallpaperWindow::readyForReattach, this, &WallpaperManager::onWindowReadyForReattach);
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
    if (m_shuttingDown) {
        return;
    }
    if (m_attachInFlight) {
        // Already discovering/attaching on the background thread; let
        // that attempt finish rather than piling up overlapping Win32
        // discovery calls (which contend on the same static cooldown
        // state in WindowsDesktopWallpaper and gain nothing by racing).
        // If it turns out to be stale by the time it finishes (generation
        // changed - e.g. Explorer restarted again mid-attempt),
        // onAttachAttemptFinished starts a fresh one itself.
        return;
    }
    if (m_windows.empty()) {
        return;
    }
    m_attachInFlight = true;
    m_attachJobGeneration = m_attachGeneration;
    const quint64 jobGeneration = m_attachJobGeneration;
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

    QFuture<bool> future = QtConcurrent::run([hwnds, jobGeneration]() {
        bool allOk = true;
        for (HWND hwnd : hwnds) {
            QElapsedTimer t;
            t.start();
            const bool ok = WindowsDesktopWallpaper::AttachToDesktop(hwnd);
            qInfo() << "[Shell] (gen" << jobGeneration << ") AttachToDesktop hwnd="
                    << reinterpret_cast<quintptr>(hwnd) << "result=" << ok << "took" << t.elapsed() << "ms";
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

    if (m_shuttingDown) {
        return;
    }

    if (m_attachJobGeneration != m_attachGeneration) {
        // The job that just finished was launched against an OLDER
        // generation - Explorer restarted (or the wallpaper was
        // reconfigured/removed) while it was still running, recreating
        // HWNDs or replacing m_windows entirely. Its result describes
        // state that no longer exists; applying it (e.g. hiding a
        // brand-new window because the OLD one failed to attach) would
        // corrupt the new attach cycle. Discard it and, if we're still
        // active, kick off a fresh attempt against the current windows.
        qInfo() << "[Shell] Discarding stale attach result from generation" << m_attachJobGeneration
                << "(current generation" << m_attachGeneration << ").";
        if (m_active && !m_windows.empty()) {
            attachAllWindows();
        }
        return;
    }

    const bool allOk = m_attachWatcher.result();

    if (m_windows.empty()) {
        return;
    }

    if (allOk) {
        if (m_attachRetryTimer.isActive()) {
            qInfo() << "[Shell] Desktop hierarchy became ready after"
                    << m_attachRetryElapsed.elapsed() << "ms - wallpaper reparented.";
            m_attachRetryTimer.stop();
        }

        // AttachToDesktop()==true only proves SetParent/SetWindowPos
        // succeeded - it says nothing about whether DirectComposition is
        // still actually presenting frames through the new parentage.
        // Nudge the renderer to re-commit post-reparent (see
        // D3DWallpaperRenderer::recommitAfterReparent) and dump the live
        // hierarchy now, then check back shortly to confirm frames are
        // actually still advancing before calling this "Attached".
        std::vector<quint64> framesBefore;
        framesBefore.reserve(m_windows.size());
        for (auto& w : m_windows) {
            w->notifyAttachedToDesktop();
            WindowsDesktopWallpaper::DumpDesktopState(w->handle(), "[post-attach]");
            framesBefore.push_back(w->presentedFrames());
        }

        const quint64 generationAtCheck = m_attachGeneration;
        QTimer::singleShot(700, this, [this, framesBefore, generationAtCheck]() {
            if (m_shuttingDown || !m_active || generationAtCheck != m_attachGeneration ||
                framesBefore.size() != m_windows.size()) {
                // Another restart/reconfigure happened in the meantime -
                // whatever attach cycle is current now will run its own
                // verification; this stale one has nothing left to check.
                return;
            }
            bool allPresenting = true;
            for (size_t i = 0; i < m_windows.size(); ++i) {
                const quint64 after = m_windows[i]->presentedFrames();
                const bool presenting = after > framesBefore[i];
                if (!presenting) {
                    allPresenting = false;
                }
                qInfo() << "[Verify] window" << i << "framesBefore=" << framesBefore[i]
                        << "framesAfter=" << after << "presenting=" << presenting;
            }
            qInfo() << "[Lifecycle] Wallpaper recovery" << (allPresenting ? "COMPLETE" : "INCOMPLETE")
                    << "- DComp=OK Renderer=RUNNING Video=" << (allPresenting ? "PLAYING" : "NOT ADVANCING")
                    << "Present=" << (allPresenting ? "ACTIVE" : "STALLED");
            if (!allPresenting) {
                // Reparented successfully but frames aren't actually
                // flowing through to the desktop - treat this the same as
                // a failed attach so the readiness poll keeps retrying
                // instead of declaring victory on a black/frozen wallpaper.
                if (!m_attachRetryTimer.isActive()) {
                    m_attachRetryElapsed.start();
                    m_attachRetryTimer.start();
                }
            }
        });
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
    // The window set itself is about to change/disappear - see the
    // m_attachGeneration comment in the header. Any attach job already in
    // flight was launched against the windows being destroyed below; its
    // eventual result must be discarded (onAttachAttemptFinished checks
    // this), not applied to whatever m_windows holds afterward.
    ++m_attachGeneration;
    for (auto& w : m_windows) {
        WindowsDesktopWallpaper::DetachFromDesktop(w->handle());
        w->hideNative();
    }
    m_windows.clear();
}
