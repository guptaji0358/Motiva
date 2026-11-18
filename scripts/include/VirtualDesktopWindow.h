#pragma once

#include "DesktopScaling.h"

#include <QPointer>
#include <QWidget>

class GlButton;
class GlCanvas;
class PreviewSource;
class QScreen;
class VdDesktopChrome;
class VdWallpaperSurface;

// One preview window on one monitor. Purely a simulated surface: it never
// touches the real desktop, the real wallpaper windows, Explorer, or the OS
// wallpaper. Full screen it is frameless and top-most like MediaPreview's
// viewer; windowed it is a normal resizable window. It does not own its
// PreviewSource and does not decide when the source plays - the session does
// (it sees every window and the power state); this window only reports when
// its visibility changes.
//
// One window per session is the "controller": it alone shows the Exit /
// Windowed / Icons buttons and the simulated icons, taskbar, Start menu and
// windows (like the real primary monitor). The others show just the wallpaper.
class VirtualDesktopWindow : public QWidget {
    Q_OBJECT
public:
    VirtualDesktopWindow(PreviewSource* source, DesktopScaling::Mode mode, QScreen* screen,
                         QWidget* parent = nullptr);

    // Fullscreen (frameless, top-most) <-> a normal resizable window.
    void setWindowed(bool windowed);
    bool isWindowed() const { return m_windowed; }
    void setController(bool controller);
    bool isController() const { return m_controller; }
    // Simulated desktop icons + taskbar (controller only).
    void setChromeVisible(bool visible);
    // Small status line over the wallpaper ("Paused on battery"); empty hides it.
    void setNote(const QString& text);
    // Fits the window to its screen again (resolution change); no-op windowed.
    void fitToScreen();
    // True when this window is on screen and not minimised.
    bool isShowing() const { return isVisible() && !isMinimized(); }
    // True once the first frame has been swapped to the screen.
    bool hasPresented() const { return m_presented; }

signals:
    // Emitted exactly once when the user (or the session) closes the window.
    void closed();
    // The user asked for windowed <-> fullscreen; the session decides what
    // that means for the other monitors' windows.
    void windowedToggleRequested();
    // Shown / hidden / minimised / restored.
    void visibilityChanged();
    // Emitted once, when the first frame has actually been presented.
    void firstFramePresented();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void layoutItems(const QSizeF& size);
    void layoutButtons();
    void applyFrameFromSource();

    QPointer<PreviewSource> m_source;
    GlCanvas* m_canvas = nullptr;
    VdWallpaperSurface* m_surface = nullptr;
    GlButton* m_exitButton = nullptr;
    GlButton* m_modeButton = nullptr;
    GlButton* m_chromeButton = nullptr;
    VdDesktopChrome* m_chrome = nullptr;
    QRect m_windowedGeometry;
    bool m_windowed = false;
    bool m_controller = true;
    bool m_closedEmitted = false;
    bool m_presented = false;
};
