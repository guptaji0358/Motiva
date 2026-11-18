#pragma once

#include "DesktopScaling.h"

#include <QObject>
#include <QPointer>
#include <memory>

class PreviewSource;
class QScreen;
class VirtualDesktopWindow;

// Lifecycle owner for one Virtual Desktop Preview. It owns the PreviewSource
// and the window, and guarantees a single, idempotent shutdown path:
// window closed by the user, end() called by Motiva, or the application
// quitting. Everything it shows is simulated; it never calls into
// WallpaperManager, WindowsDesktopWallpaper, Explorer or any OS wallpaper API.
//
// Phase 1 has one full-screen window on one monitor. Later phases add more
// windows and a windowed mode behind this same session object.
class VirtualDesktopSession : public QObject {
    Q_OBJECT
public:
    // Takes ownership of `source`.
    VirtualDesktopSession(PreviewSource* source, DesktopScaling::Mode mode, QScreen* screen,
                          QObject* parent = nullptr);
    ~VirtualDesktopSession() override;

    void start();
    // Closes the window; safe to call repeatedly or after the user closed it.
    void end();
    bool isActive() const { return m_active; }
    // Set before start(): pause an independent source while on battery.
    void setPauseOnBattery(bool pause) { m_pauseOnBattery = pause; }

signals:
    // Emitted exactly once, after the window is closed.
    void finished();

private:
    void onWindowClosed();

    PreviewSource* m_source = nullptr; // child of this object
    std::unique_ptr<VirtualDesktopWindow> m_window;
    DesktopScaling::Mode m_mode;
    QPointer<QScreen> m_screen;
    bool m_active = false;
    bool m_pauseOnBattery = false;
    bool m_finishedEmitted = false;
};
