#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QTimer>
#include <windows.h>

class ImagePlaylist;
class SettingsManager;

// Decides WHEN the image playlist moves to its next image. Three
// independent, user-selectable triggers; any enabled one may advance the
// playlist, and none of them resets it (each just calls
// ImagePlaylist::advance(), which continues from the current image):
//
//  - Lock -> Unlock: WM_WTSSESSION_CHANGE (WTSRegisterSessionNotification)
//    - purely event-driven. Advances only on an UNLOCK that follows a LOCK
//    this process observed, never on any other session notification.
//  - Windows start: a new Windows logon session since Motiva last ran,
//    identified by the session's own LogonTime (WTSSessionInfo). Restarting
//    Explorer, Motiva being relaunched or recovering via IPC all happen
//    inside the SAME logon session, so none of them can look like a
//    Windows restart. See consumeNewWindowsSession().
//  - Interval: one QTimer, armed only while the playlist is on and this
//    trigger is enabled; restarted whenever the current image changes so
//    each image gets the full interval. No polling anywhere.
//
// Explorer-restart recovery is not a trigger and is not observed here at
// all - WallpaperManager re-presents whatever image is current.
class PlaylistRotation : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    enum class Trigger { LockUnlock, WindowsStart, Interval };

    PlaylistRotation(ImagePlaylist* playlist, SettingsManager* settings, QObject* parent = nullptr);
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

    bool rotateOnUnlock() const { return m_rotateOnUnlock; }
    bool rotateOnWindowsStart() const { return m_rotateOnWindowsStart; }
    bool rotateOnInterval() const { return m_rotateOnInterval; }
    int intervalMinutes() const { return m_intervalMinutes; }
    void setRotateOnUnlock(bool enabled);
    void setRotateOnWindowsStart(bool enabled);
    void setRotateOnInterval(bool enabled);
    void setIntervalMinutes(int minutes);

    // Advances the playlist for `trigger` if the playlist is on. Returns
    // whether the image changed.
    bool fire(Trigger trigger);

    // QAbstractNativeEventFilter
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

    static const char* triggerName(Trigger trigger);

private:
    void reconfigureTimer();
    // Logon time of the current Windows session (100ns FILETIME ticks),
    // falling back to the system boot time if unavailable. 0 on failure.
    static qint64 currentWindowsSessionStamp();

    ImagePlaylist* m_playlist;
    SettingsManager* m_settings;
    QTimer m_intervalTimer;
    HWND m_sessionHwnd = nullptr;
    bool m_lockObserved = false;

    bool m_rotateOnUnlock = true;
    bool m_rotateOnWindowsStart = true;
    bool m_rotateOnInterval = false;
    int m_intervalMinutes = 30;
};
