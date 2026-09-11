#pragma once

#include <QObject>
#include <QRect>
#include <QThread>
#include <windows.h>
#include "VideoPlayer.h"

enum class ScalingMode {
    Fill,     // KeepAspectRatioByExpanding (crop)
    Fit,      // KeepAspectRatio (letterbox)
    Stretch,  // IgnoreAspectRatio
    Original  // native pixel size, centered
};
Q_DECLARE_METATYPE(ScalingMode) // needed to pass across threads via QMetaObject::invokeMethod

class D3DWallpaperRenderer;

// The render surface for one monitor's worth of desktop wallpaper.
//
// This is a PLAIN Win32 window (our own window class, our own WndProc) -
// deliberately not a QWidget (a reparented QWidget's paintEvent was
// confirmed in testing to never fire after SetParent).
//
// Presentation itself no longer happens via WM_PAINT/GDI: content is
// pushed to the desktop through a D3DWallpaperRenderer (D3D11 +
// DirectComposition) that lives on its own dedicated QThread, so
// uploading a frame to the GPU and calling Present() (which blocks on
// vblank) never runs on the GUI thread. This class owns that thread and
// simply forwards VideoPlayer::frameReady to the renderer's presentFrame
// slot - Qt marshals that call onto the renderer's thread automatically
// since sender and receiver live on different threads.
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

    // True once the D3D/DirectComposition pipeline reported successful
    // initialization. AttachToDesktop() should still be called regardless
    // of window content readiness (the HWND itself is valid either way),
    // but callers that want to know whether frames can actually be
    // presented should check this.
    bool isRendererReady() const { return m_rendererReady; }

    // Called after Explorer has been detected to have restarted (see
    // WallpaperManager's "TaskbarCreated" handling - event-driven, not
    // polled). Explorer destroying the old Progman cascades to destroy our
    // reparented child HWND too (documented Win32 behavior for parent/
    // child destruction), so this always creates a fresh native HWND and
    // rebinds the existing D3D/DirectComposition pipeline to it (device/
    // swapchain/visual/shaders/textures are all reused untouched - only
    // the DirectComposition target is recreated). Asynchronous: the
    // rebind is a non-blocking queued call to the render thread (see
    // onRebindFinished), so this never waits on that thread.
    void recoverFromExplorerRestart();

    // Best-effort "is video actually still flowing" signal, read from the
    // GUI thread while the renderer itself lives on its own thread - see
    // D3DWallpaperRenderer::presentedFrameCount (atomic, no marshaling
    // needed for a plain counter read).
    quint64 presentedFrames() const;

    // Diagnostics-only passthroughs to the renderer, used by
    // WallpaperManager's per-Explorer-restart state dump - see
    // D3DWallpaperRenderer::hasValidDCompState()/hasValidDevice().
    bool rendererHasValidDCompState() const;
    bool rendererHasValidDevice() const;

    // Tells the renderer this window has just been (re)parented into the
    // desktop hierarchy, so it can re-commit the DirectComposition target
    // post-reparent - see D3DWallpaperRenderer::recommitAfterReparent.
    // Non-blocking (queued call to the render thread).
    void notifyAttachedToDesktop();

public:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

signals:
    // Emitted once the native HWND has been recreated and the DComp
    // target rebound to it after Explorer restarted - i.e. this window is
    // now ready to be (re)parented into the new desktop hierarchy.
    // Deliberately does NOT call WindowsDesktopWallpaper::AttachToDesktop
    // itself: that call is centralized in WallpaperManager's async attach
    // path (see m_attachWatcher there) so there is only ever ONE place
    // that touches the shared Progman/WorkerW discovery state - calling
    // it from here too (as an earlier version did) raced with that
    // background attach attempt on the same static state, which is
    // exactly the kind of undefined behavior that can crash the whole
    // process, not just this window.
    void readyForReattach(bool ok);

private slots:
    void onRebindFinished(bool ok);

private:
    HWND createNativeWindow();

    VideoPlayer* m_player;
    HWND m_hwnd = nullptr;
    ScalingMode m_scalingMode = ScalingMode::Fill;
    QRect m_monitorRect;

    QThread m_renderThread;
    D3DWallpaperRenderer* m_renderer = nullptr; // lives on m_renderThread
    bool m_rendererReady = false;
};
