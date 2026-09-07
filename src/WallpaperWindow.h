#pragma once

#include <QObject>
#include <QRect>
#include <windows.h>
#include "VideoPlayer.h"

enum class ScalingMode {
    Fill,     // KeepAspectRatioByExpanding (crop)
    Fit,      // KeepAspectRatio (letterbox)
    Stretch,  // IgnoreAspectRatio
    Original  // native pixel size, centered
};

// The render surface for one monitor's worth of desktop wallpaper.
//
// This is a PLAIN Win32 window (our own window class, our own WndProc) -
// deliberately not a QWidget. In testing, a QWidget whose HWND was
// reparented into Explorer's WorkerW/Progman via raw SetParent would
// never actually have its paintEvent invoked: Qt's QPA layer tracks its
// own idea of a window's top-level/child status, DPI context, and
// backing store, none of which are told about a reparent performed
// behind Qt's back, and its paint dispatch silently swallowed every
// WM_PAINT as a result (confirmed by instrumentation: frame-ready
// notifications and forced InvalidateRect/UpdateWindow calls arrived
// fine, but paintEvent was never called). Owning the window class and
// WndProc directly sidesteps all of that - WM_PAINT is guaranteed to
// reach our own handler regardless of how the window gets reparented.
//
// Qt is still used for everything else: decoding (VideoPlayer) and the
// in-app preview (MainWindow paints QImages from the same VideoPlayer
// into a QLabel, which is a normal, never-reparented Qt widget and has
// no such issue).
class WallpaperWindow : public QObject {
    Q_OBJECT
public:
    explicit WallpaperWindow(VideoPlayer* player, QObject* parent = nullptr);
    ~WallpaperWindow() override;

    HWND handle() const { return m_hwnd; }

    void setScalingMode(ScalingMode mode);
    // rect is in the virtual-desktop's physical-pixel coordinate space.
    void setMonitorRect(const QRect& rect);

    void showNative();
    void hideNative();

private slots:
    void onFrameReady();

public:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    void handlePaint();
    QRect computeTargetRect(const QSize& imageSize, const QSize& widgetSize) const;

    VideoPlayer* m_player;
    HWND m_hwnd = nullptr;
    ScalingMode m_scalingMode = ScalingMode::Fill;
    QRect m_monitorRect;
};
