#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QTimer>
#include <windows.h>

class PlaylistLibrary;
class SettingsManager;

// Decides WHEN an IMAGE playlist moves to its next image. Applies only to
// the library's active playlist, using that playlist's own settings (each
// playlist has its own in the .mtv). Video playlists don't use these
// triggers at all - they advance when a video ends (MainWindow handles
// VideoPlayer::endOfMedia) - so the two kinds never share rules by accident.
//
//  - Lock -> Unlock: WM_WTSSESSION_CHANGE (WTSRegisterSessionNotification)
//    - purely event-driven. Advances only on an UNLOCK that follows a LOCK
//    this process observed, never on any other session notification.
//  - Windows start: a new Windows logon session since Motiva last ran,
//    identified by the session's own LogonTime (WTSSessionInfo). Restarting
//    Explorer, Motiva being relaunched or recovering via IPC all happen
//    inside the SAME logon session, so none of them can look like a
//    Windows restart. See consumeNewWindowsSession().
//  - Interval: one QTimer, armed only while the active playlist is an image
//    playlist with this trigger enabled; restarted whenever the current
//    image changes so each image gets the full interval. No polling.
//
// Explorer-restart recovery is not a trigger and is not observed here at
// all - WallpaperManager re-presents whatever item is current.
class PlaylistRotation : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    enum class Trigger { LockUnlock, WindowsStart, Interval };

    PlaylistRotation(PlaylistLibrary* library, SettingsManager* settings, QObject* parent = nullptr);
    ~PlaylistRotation() override;

    // Registers for session lock/unlock notifications on a stable,
    // long-lived HWND (MainWindow's own). Call once.
    void registerSessionNotifications(HWND hwnd);

    // Compares this Windows logon session against the one recorded on
    // Motiva's previous run, records the current one, and returns true
    // only if a previous run was recorded AND it was a different session
    // (i.e. Windows restarted / the user signed in again since then).
    // Call exactly once per process, at startup.
    bool consumeNewWindowsSession();

    // Advances the active playlist for `trigger` if it is an image playlist
    // with that trigger enabled. Returns whether the image changed.
    bool fire(Trigger trigger);

    // QAbstractNativeEventFilter
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

    static const char* triggerName(Trigger trigger);

private:
    void reconfigureTimer();
    // Logon time of the current Windows session (100ns FILETIME ticks),
    // falling back to the system boot time if unavailable. 0 on failure.
    static qint64 currentWindowsSessionStamp();

    PlaylistLibrary* m_library;
    SettingsManager* m_settings;
    QTimer m_intervalTimer;
    HWND m_sessionHwnd = nullptr;
    bool m_lockObserved = false;
};
