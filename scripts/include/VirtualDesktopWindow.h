#pragma once

#include "DesktopScaling.h"

#include <QPointer>
#include <QWidget>

class GlButton;
class GlCanvas;
class PreviewSource;
class QScreen;
class VdWallpaperSurface;

// One full-screen preview window on one monitor (Phase 1: a single window).
// Purely a simulated surface: it never touches the real desktop, the real
// wallpaper windows, Explorer, or the OS wallpaper. It is frameless and
// top-most like MediaPreview's viewer, always shows an Exit control, and
// closes on Esc. It does not own its PreviewSource.
class VirtualDesktopWindow : public QWidget {
    Q_OBJECT
public:
    VirtualDesktopWindow(PreviewSource* source, DesktopScaling::Mode mode, QScreen* screen,
                         QWidget* parent = nullptr);

signals:
    // Emitted exactly once when the user (or the session) closes the window.
    void closed();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void layoutItems(const QSizeF& size);
    void applyFrameFromSource();

    QPointer<PreviewSource> m_source;
    GlCanvas* m_canvas = nullptr;
    VdWallpaperSurface* m_surface = nullptr;
    GlButton* m_exitButton = nullptr;
    bool m_closedEmitted = false;
};
