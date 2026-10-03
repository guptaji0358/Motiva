#include "PlaylistRotation.h"
#include "PlaylistLibrary.h"
#include "PlaylistModel.h"
#include "SettingsManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <wtsapi32.h>

#ifndef WTS_SESSION_LOCK
#define WTS_SESSION_LOCK 0x7
#endif
#ifndef WTS_SESSION_UNLOCK
#define WTS_SESSION_UNLOCK 0x8
#endif

PlaylistRotation::PlaylistRotation(PlaylistLibrary* library, SettingsManager* settings, QObject* parent)
    : QObject(parent), m_library(library), m_settings(settings) {
    m_intervalTimer.setSingleShot(false);
    m_intervalTimer.setTimerType(Qt::VeryCoarseTimer); // minute-scale; no precision needed
    connect(&m_intervalTimer, &QTimer::timeout, this, [this]() { fire(Trigger::Interval); });

    connect(m_library, &PlaylistLibrary::activeChanged, this, &PlaylistRotation::reconfigureTimer);
    connect(m_library, &PlaylistLibrary::activeSettingsChanged, this, [this] {
        m_intervalTimer.stop(); // a changed interval starts counting from now
        reconfigureTimer();
    });
    connect(m_library, &PlaylistLibrary::playlistsChanged, this, &PlaylistRotation::reconfigureTimer);
    // Whatever changed the image (any trigger, Show now, removal), the new
    // image gets a full interval before the timer moves on from it.
    connect(m_library, &PlaylistLibrary::activeCurrentChanged, this, [this]() {
        if (m_intervalTimer.isActive()) {
            m_intervalTimer.start();
        }
    });

    qApp->installNativeEventFilter(this);
    reconfigureTimer();
}

PlaylistRotation::~PlaylistRotation() {
    qApp->removeNativeEventFilter(this);
    if (m_sessionHwnd) {
        WTSUnRegisterSessionNotification(m_sessionHwnd);
    }
}

const char* PlaylistRotation::triggerName(Trigger trigger) {
    switch (trigger) {
    case Trigger::LockUnlock: return "lock->unlock";
    case Trigger::WindowsStart: return "Windows start";
    case Trigger::Interval: return "interval";
    }
    return "?";
}

void PlaylistRotation::registerSessionNotifications(HWND hwnd) {
    if (m_sessionHwnd || !hwnd) {
        return;
    }
    if (WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION)) {
        m_sessionHwnd = hwnd;
        qInfo() << "[Rotation] Registered for session lock/unlock notifications.";
    } else {
        qWarning() << "[Rotation] WTSRegisterSessionNotification failed, GetLastError=" << GetLastError()
                   << "- the Lock -> Unlock trigger will not work.";
    }
}

bool PlaylistRotation::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
    Q_UNUSED(result);
    // Only the per-window hook: it sees every message reaching the window
    // (sent or posted). The dispatcher hook would show posted messages a
    // second time.
    if (eventType != "windows_generic_MSG") {
        return false;
    }
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg->message != WM_WTSSESSION_CHANGE || msg->hwnd != m_sessionHwnd) {
        return false;
    }
    if (msg->wParam == WTS_SESSION_LOCK) {
        m_lockObserved = true;
        qInfo() << "[Rotation] Session locked.";
    } else if (msg->wParam == WTS_SESSION_UNLOCK) {
        // Only a genuine lock -> unlock pair counts; a stray/duplicate
        // unlock without a preceding lock never advances.
        const bool wasLocked = m_lockObserved;
        m_lockObserved = false;
        qInfo() << "[Rotation] Session unlocked (lock observed=" << wasLocked << ").";
        if (wasLocked) {
            fire(Trigger::LockUnlock);
        }
    }
    return false; // observe only
}

bool PlaylistRotation::fire(Trigger trigger) {
    PlaylistModel* active = m_library->activePlaylist();
    if (!active || active->isVideo()) {
        return false; // video playlists advance on end-of-video, not on these triggers
    }
    const RotationSettings r = active->rotation();
    const bool enabled = (trigger == Trigger::LockUnlock && r.onUnlock) ||
                         (trigger == Trigger::WindowsStart && r.onWindowsStart) ||
                         (trigger == Trigger::Interval && r.onInterval);
    if (!enabled) {
        return false;
    }
    qInfo() << "[Rotation] Trigger fired:" << triggerName(trigger) << "- playlist" << active->name();
    return active->advance(true);
}

qint64 PlaylistRotation::currentWindowsSessionStamp() {
    qint64 stamp = 0;
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfo,
                                    &buffer, &bytes) && buffer && bytes >= sizeof(WTSINFOW)) {
        stamp = reinterpret_cast<const WTSINFOW*>(buffer)->LogonTime.QuadPart;
    }
    if (buffer) {
        WTSFreeMemory(buffer);
    }
    if (stamp == 0) {
        // Fallback: system boot time, rounded to the minute so the
        // millisecond jitter of "now - uptime" can't fake a new boot.
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const qint64 nowTicks = (static_cast<qint64>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        const qint64 bootTicks = nowTicks - static_cast<qint64>(GetTickCount64()) * 10000;
        constexpr qint64 kMinute = 60LL * 10000000LL;
        stamp = (bootTicks / kMinute) * kMinute;
        qInfo() << "[Rotation] Session logon time unavailable - using boot time instead.";
    }
    return stamp;
}

bool PlaylistRotation::consumeNewWindowsSession() {
    const qint64 current = currentWindowsSessionStamp();
    const qint64 previous = m_settings->lastWindowsSessionStamp();
    if (current != 0) {
        m_settings->setLastWindowsSessionStamp(current);
    }
    const bool isNew = (previous != 0 && current != 0 && previous != current);
    qInfo() << "[Rotation] Windows session stamp previous=" << previous << "current=" << current
            << "-> new Windows session since last run:" << isNew;
    return isNew;
}

void PlaylistRotation::reconfigureTimer() {
    PlaylistModel* active = m_library->activePlaylist();
    const bool shouldRun = active && !active->isVideo() && active->rotation().onInterval && active->count() > 1;
    if (!shouldRun) {
        if (m_intervalTimer.isActive()) {
            qInfo() << "[Rotation] Interval timer stopped.";
        }
        m_intervalTimer.stop();
        return;
    }
    const int minutes = qBound(1, active->rotation().intervalMinutes, 24 * 60);
    const int intervalMs = minutes * 60 * 1000;
    if (m_intervalTimer.isActive() && m_intervalTimer.interval() == intervalMs) {
        return; // already counting toward the next change - don't reset it
    }
    m_intervalTimer.start(intervalMs);
    qInfo() << "[Rotation] Interval timer running - every" << minutes << "minute(s) for" << active->name();
}
