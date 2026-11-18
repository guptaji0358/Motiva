#pragma once

#include "DesktopScaling.h"

#include <QObject>
#include <QPointer>
#include <memory>
#include <vector>

class PreviewSource;
class QScreen;
class QTimer;
class VirtualDesktopWindow;

// Lifecycle owner for one Virtual Desktop Preview. It owns the PreviewSource
// and one window per monitor, and guarantees a single, idempotent shutdown
// path: any window closed by the user, end() called by Motiva, or the
// application quitting (all windows close together). Everything it shows is
// simulated; it never calls into WallpaperManager, WindowsDesktopWallpaper,
// Explorer or any OS wallpaper API.
//
// Monitors: fullscreen shows a window on every screen (like the real
// wallpaper); the window on the starting screen is the "controller" with the
// buttons and simulated icons/taskbar. Windowed mode shows only the
// controller window. Monitors added/removed during the preview are picked up
// (VdDesktop::planScreenSync). The session also owns the play/pause decision
// for an independent source (battery policy, nothing showing).
class VirtualDesktopSession : public QObject {
    Q_OBJECT
public:
    // Takes ownership of `source`.
    VirtualDesktopSession(PreviewSource* source, DesktopScaling::Mode mode, QScreen* screen,
                          QObject* parent = nullptr);
    ~VirtualDesktopSession() override;

    void start();
    // Closes the windows; safe to call repeatedly or after the user closed one.
    void end();
    bool isActive() const { return m_active; }
    // Set before start(): pause an independent source while on battery.
    void setPauseOnBattery(bool pause) { m_pauseOnBattery = pause; }

signals:
    // Emitted exactly once, after the windows are closed.
    void finished();

private:
    struct Entry {
        QPointer<QScreen> screen;
        std::unique_ptr<VirtualDesktopWindow> window;
    };

    void onWindowClosed();
    void addWindowFor(QScreen* screen);
    void scheduleScreenSync();
    void syncScreens();
    void toggleWindowed();
    void applyPlayState();
    Entry* controllerEntry();
    void ensureController();

    PreviewSource* m_source = nullptr; // child of this object
    std::vector<Entry> m_entries;
    DesktopScaling::Mode m_mode;
    QPointer<QScreen> m_screen;     // where the preview started
    QPointer<QScreen> m_controllerScreen;
    QTimer* m_powerTimer = nullptr;
    bool m_active = false;
    bool m_windowed = false;
    bool m_pauseOnBattery = false;
    bool m_syncQueued = false;
    bool m_finishedEmitted = false;
    bool m_mutating = false; // entries being destroyed: skip playback decisions
};
