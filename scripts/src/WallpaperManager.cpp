#include "WallpaperManager.h"
#include "StartupDiagnostics.h"
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
    // WM_SETTINGCHANGE is broadcast to every top-level window whenever
    // SystemParametersInfo is called with SPIF_SENDCHANGE - including by
    // Explorer's own "Set as desktop background" (confirmed by testing:
    // SPI_SETDESKWALLPAPER succeeds and updates the registry even while
    // our video wallpaper is attached - our window was only ever visually
    // covering the result, never blocking the underlying OS call). This
    // app never calls SPI_SETDESKWALLPAPER itself except
    // RefreshDesktopBackground's own no-op "re-apply the same value"
    // nudge (which can't trigger a real change - see
    // onPossibleExternalWallpaperChange), so while our wallpaper is
    // active, any OTHER change to that value can only mean the user (or
    // something else) just picked a new one via Explorer/Settings - purely
    // event-driven, no polling.
    if (msg->message == WM_SETTINGCHANGE) {
        onPossibleExternalWallpaperChange();
    }
    if (msg->message == WM_POWERBROADCAST && msg->wParam == PBT_POWERSETTINGCHANGE) {
        onPowerBroadcast(reinterpret_cast<void*>(msg->lParam));
    }
    return false; // never swallow the message - other listeners may need it too
}

void WallpaperManager::registerPowerNotifications(HWND hwnd) {
    if (m_powerNotifyHandle || !hwnd) {
        return;
    }
    // DEVICE_NOTIFY_WINDOW_HANDLE (0) - deliver as a WM_POWERBROADCAST to
    // this HWND rather than a service-control callback. Windows sends one
    // notification immediately with the current AC/battery state right
    // after a successful registration (documented behavior), so
    // m_onBattery is corrected from its optimistic default without any
    // extra query call here.
    m_powerNotifyHandle = RegisterPowerSettingNotification(hwnd, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!m_powerNotifyHandle) {
        qWarning() << "[Battery] RegisterPowerSettingNotification failed, GetLastError=" << GetLastError()
                   << "- \"Show video on battery\" will have no effect on this system.";
    }
}

void WallpaperManager::onPowerBroadcast(void* lParam) {
    auto* setting = static_cast<POWERBROADCAST_SETTING*>(lParam);
    if (!setting || setting->PowerSetting != GUID_ACDC_POWER_SOURCE || setting->DataLength < sizeof(DWORD)) {
        return;
    }
    // 0 = AC/plugged in, 1 = battery, 2 = "short term" (UPS) - treated the
    // same as battery here, since it means mains power is not currently
    // available either way.
    const DWORD source = *reinterpret_cast<const DWORD*>(setting->Data);
    const bool nowOnBattery = (source != 0);
    if (nowOnBattery == m_onBattery) {
        return;
    }
    m_onBattery = nowOnBattery;
    qInfo() << "[Battery] Power source changed:" << (m_onBattery ? "on battery" : "AC/plugged in");
    reevaluateBatteryPolicy();
}

void WallpaperManager::setShowVideoOnBattery(bool enabled) {
    if (m_showVideoOnBattery == enabled) {
        return;
    }
    m_showVideoOnBattery = enabled;
    reevaluateBatteryPolicy();
}

void WallpaperManager::reevaluateBatteryPolicy() {
    if (!m_active) {
        return;
    }
    const bool shouldHide = m_onBattery && !m_showVideoOnBattery;
    if (shouldHide && !m_batterySuspended) {
        suspendForBattery();
    } else if (!shouldHide && m_batterySuspended) {
        resumeFromBattery();
    }
}

void WallpaperManager::suspendForBattery() {
    if (!m_active || m_batterySuspended || m_windows.empty()) {
        return;
    }
    qInfo() << "[Battery] Hiding video wallpaper - on battery and \"Show video on battery\" is off.";
    for (auto& w : m_windows) {
        WindowsDesktopWallpaper::DetachFromDesktop(w->handle());
        w->hideNative();
    }
    m_batterySuspended = true;
    // Same redraw nudge removeWallpaper() already uses - our own window
    // detaching doesn't by itself make Explorer repaint the real
    // wallpaper underneath. m_active stays true throughout, so this
    // re-applies the SAME already-current wallpaper value (no genuine
    // change), which is exactly what onPossibleExternalWallpaperChange's
    // own baseline comparison already no-ops on - no feedback loop.
    WindowsDesktopWallpaper::RefreshDesktopBackground();
    emit wallpaperSuspendedForBattery();
}

void WallpaperManager::resumeFromBattery() {
    if (!m_active || !m_batterySuspended) {
        return;
    }
    qInfo() << "[Battery] AC power restored - restoring video wallpaper.";
    m_batterySuspended = false;
    // Reuses the existing async/generation-guarded/verified attach
    // pipeline - the same one Explorer-restart and IPC recovery already
    // drive. The render windows were only detached, never destroyed, so
    // this just reparents them back, no rebuild needed.
    attachAllWindows();
    emit wallpaperResumedFromBattery();
}

void WallpaperManager::onPossibleExternalWallpaperChange() {
    if (!m_active) {
        return;
    }
    const std::wstring current = WindowsDesktopWallpaper::GetCurrentWallpaperPath();
    if (current.empty() || current == m_wallpaperBaselineAtAttach) {
        return;
    }
    qInfo() << "[Wallpaper] Detected Windows' own wallpaper changed while our video wallpaper was "
                "active (likely the user picked a new one via Explorer/Settings) - stepping aside so "
                "their choice is visible instead of staying on top of it.";
    removeWallpaper();
}

bool WallpaperManager::setWallpaper(const QString& videoPath) {
    qInfo() << "[Lifecycle] setWallpaper begin, path=" << videoPath;
    if (!m_player->loadFile(videoPath)) {
        return false;
    }
    m_currentPath = videoPath;
    m_player->setLooping(m_player->isLooping());

    if (m_recoveryState) {
        m_recoveryState->setVideoPath(videoPath);
        m_recoveryState->setWallpaperAttached(true); // intent, not yet-verified fact
    }

    // Snapshot Windows' own static-wallpaper path now, before we ever
    // attach - see onPossibleExternalWallpaperChange. We never write this
    // value ourselves while active, so any later difference from this
    // baseline can only mean the user changed it via Explorer/Settings.
    m_wallpaperBaselineAtAttach = WindowsDesktopWallpaper::GetCurrentWallpaperPath();

    rebuildWindows();

    // If "Show video on battery" is off and we're already on battery
    // right now, never attach in the first place - avoids a visible
    // flash-then-hide, and avoids racing suspendForBattery()'s detach
    // against this attach attempt still being in flight. Skipping
    // attachAllWindows() here (rather than calling it and immediately
    // suspending) means there's nothing for onAttachAttemptFinished to
    // race against; resumeFromBattery() drives the first real attach
    // once AC power actually returns.
    const bool shouldStartHidden = m_onBattery && !m_showVideoOnBattery;
    if (shouldStartHidden) {
        m_batterySuspended = true;
        qInfo() << "[Battery] Wallpaper set while on battery with \"Show video on battery\" off - "
                    "staying hidden until AC power returns.";
    } else {
        attachAllWindows();
    }

    m_player->play();
    m_userPaused = false;
    m_active = true;
    emit wallpaperActivated();
    if (shouldStartHidden) {
        emit wallpaperSuspendedForBattery();
    }
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
    // m_active MUST be false before calling RefreshDesktopBackground()
    // below: that call's own SPIF_SENDCHANGE broadcasts WM_SETTINGCHANGE
    // to every top-level window in this process, which our own
    // nativeEventFilter reacts to via onPossibleExternalWallpaperChange -
    // if m_active were still true at that point, our own redraw nudge
    // would look like "the user changed the wallpaper again" and
    // re-trigger removeWallpaper() recursively/repeatedly (reproduced
    // live this session as a rapid RefreshDesktopBackground log-spam loop
    // before this ordering fix). Setting it false first makes that guard
    // ignore our own resultant broadcast, as intended.
    m_active = false;
    // An intentional removal always wins over a battery suspension - see
    // the wallpaperSuspendedForBattery doc comment's distinction. Also
    // means AC power returning afterward finds m_active already false and
    // reevaluateBatteryPolicy() correctly does nothing (Test 5: Remove
    // Wallpaper while battery-hidden must not auto-restore on AC return).
    m_batterySuspended = false;
    if (m_recoveryState) {
        m_recoveryState->setWallpaperAttached(false);
    }
    // Once per removeWallpaper() call (not per-window, inside
    // teardownWindows()'s per-HWND DetachFromDesktop loop) - detaching our
    // own window(s) alone does not reliably make Explorer's "Set as
    // desktop background" work again afterward; this nudges Explorer to
    // fully redraw/reclaim the desktop background layer. See
    // WindowsDesktopWallpaper::RefreshDesktopBackground's comment and
    // CLAUDE.md's 2026-09-11/12 "Set as wallpaper interference" entries.
    WindowsDesktopWallpaper::RefreshDesktopBackground();
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

    // Old/new Explorer PID and per-restart D3D/DComp/frame-counter state,
    // so each restart's full picture is self-contained in the log (see
    // CLAUDE.md item 4 - diagnosing why recovery eventually stops working
    // after repeated restarts).
    const DWORD oldExplorerPid = m_lastKnownExplorerPid;
    HWND newProgman = FindWindowW(L"Progman", nullptr);
    DWORD newExplorerPid = 0;
    if (newProgman) {
        GetWindowThreadProcessId(newProgman, &newExplorerPid);
    }
    m_lastKnownExplorerPid = newExplorerPid;

    qInfo() << "[Shell] Explorer restart detected - recovering wallpaper. Motiva.exe PID="
            << GetCurrentProcessId() << "oldExplorerPid=" << oldExplorerPid
            << "newExplorerPid=" << newExplorerPid << "currentAttachGeneration=" << m_attachGeneration
            << "(process, video decoder and D3D/DComp device/swapchain are NOT touched by this).";
    if (m_recoveryState) {
        m_recoveryState->incrementExplorerRecoveryCount();
        m_recoveryState->setExplorerPid(static_cast<qint64>(newExplorerPid));
    }
    for (auto& w : m_windows) {
        WindowsDesktopWallpaper::DumpDesktopState(w->handle(), "[pre-recovery]");
        qInfo() << "[Diag] [pre-recovery] renderer hasValidDCompState=" << w->rendererHasValidDCompState()
                << "hasValidDevice=" << w->rendererHasValidDevice()
                << "presentedFrameCount=" << w->presentedFrames();
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
    if (m_recoveryState) {
        m_recoveryState->setAttachGeneration(m_attachGeneration);
    }

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
            const bool ok = WindowsDesktopWallpaper::AttachToDesktop(hwnd, jobGeneration);
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
            qInfo() << "[Diag] [post-attach] renderer hasValidDCompState=" << w->rendererHasValidDCompState()
                    << "hasValidDevice=" << w->rendererHasValidDevice()
                    << "presentedFrameCount=" << w->presentedFrames();
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
            // Full hierarchy dump exactly at the moment "Active"/COMPLETE is
            // declared (or not) - this is the evidence needed to answer
            // "the app says Active, so which exact HWND/surface is actually
            // visible to the user?" rather than trusting AttachToDesktop()'s
            // return value alone. Deliberately unconditional (both branches)
            // so a STALLED case is equally diagnosable.
            for (auto& w : m_windows) {
                WindowsDesktopWallpaper::DumpFullHierarchy(w->handle(), "[active-check]");
            }
            if (allPresenting) {
                if (m_recoveryState) {
                    m_recoveryState->markVideoPresenting(StartupDiagnostics::instance().elapsedFor("firstFramePresented"));
                    m_recoveryState->setWallpaperAttached(true);
                    // First time this fires, the full startup timeline is
                    // known - persist it so a single, non-repeatable
                    // Windows restart leaves enough evidence on disk for
                    // the next launch's "previous session" summary (see
                    // RecoveryState/StartupDiagnostics).
                    m_recoveryState->setStartupDiagnostics(StartupDiagnostics::instance().toJsonAndLogDeltas());
                }
                // This is the genuine "Active" checkpoint - see the
                // wallpaperVerified header comment. Fires here (and again
                // after every successful Explorer-restart recovery, since
                // that re-enters this same verification path) rather than
                // at attach-request time in setWallpaper().
                emit wallpaperVerified();
            } else {
                if (m_recoveryState) {
                    m_recoveryState->recordFailure("attach succeeded but frames not advancing (STALLED)");
                }
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
        if (m_recoveryState) {
            m_recoveryState->recordFailure("AttachToDesktop failed (desktop hierarchy not ready)");
        }
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

void WallpaperManager::recoverOrActivate() {
    if (!m_active || m_windows.empty()) {
        qInfo() << "[IPC] Recovery requested but no wallpaper is currently configured - nothing to recover here"
                    " (MainWindow handles the never-yet-attached case).";
        return;
    }

    quint64 framesNow = 0;
    for (auto& w : m_windows) {
        framesNow += w->presentedFrames();
    }
    qInfo() << "[IPC] Recovery requested. Current lifecycle state: active=" << m_active
            << "windows=" << m_windows.size() << "attachGeneration=" << m_attachGeneration
            << "attachInFlight=" << m_attachInFlight << "totalPresentedFrames=" << framesNow;

    // Defensively resume playback (a second launch attempt is a reasonable
    // signal the user wants their wallpaper back, even if it was paused)
    // and re-drive the same attach+verify pipeline used for Explorer-
    // restart recovery. attachAllWindows() is single-flight and generation-
    // guarded (see its own comment) so calling it here is always safe,
    // whether the wallpaper is already healthy (a harmless no-op re-attach
    // + verification), not yet attached (attaches it), or attached but
    // stalled (recommits + re-verifies, same as onAttachAttemptFinished's
    // STALLED path).
    if (m_userPaused) {
        m_userPaused = false;
    }
    m_player->play();
    qInfo() << "[IPC] Recovery started - re-driving attach/verify pipeline.";
    attachAllWindows();
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
