#pragma once

#include <QObject>
#include <QImage>
#include <QSize>
#include <memory>
#include <atomic>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include "WallpaperWindow.h" // ScalingMode

// Owns the entire D3D11 + DirectComposition presentation pipeline for one
// monitor's wallpaper surface, bound to a single HWND.
//
// This object is deliberately a QObject moved to its own QThread (one per
// monitor, owned by the corresponding WallpaperWindow) rather than living
// on the GUI thread: uploading a decoded frame to a GPU texture and
// presenting it (Present() with a sync interval blocks until the next
// vblank) is real per-frame work that must never run on the thread also
// pumping mouse/keyboard input. presentFrame() is the only method meant to
// be invoked at frame rate; initialize()/resize()/shutdown() are one-shot
// or rare lifecycle calls, invoked via queued/blocking-queued
// QMetaObject::invokeMethod from the owning WallpaperWindow.
//
// Desktop-attach (SetParent/SetWindowPos into Progman) happens exactly
// once, elsewhere (WindowsDesktopWallpaper::AttachToDesktop), and is
// independent of this class: DirectComposition's CreateTargetForHwnd binds
// to the HWND itself, not to its parent, so re-parenting the HWND (which
// happens once, before first frame) does not require recreating anything
// here.
class D3DWallpaperRenderer : public QObject {
    Q_OBJECT
public:
    explicit D3DWallpaperRenderer(HWND hwnd, QObject* parent = nullptr);
    ~D3DWallpaperRenderer() override;

public slots:
    // Creates the D3D11 device, DirectComposition device/target/visual,
    // swap chain, and shader pipeline. Must be called once, on this
    // object's own thread, before presentFrame()/resize(). Returns false
    // (and logs the specific HRESULT/step that failed) if any step fails;
    // callers must not assume success.
    bool initialize(int width, int height);

    void setScalingMode(ScalingMode mode);

    // Resizes the swap chain to match a new monitor rect. Not expected to
    // be called at frame rate - only when monitor selection/arrangement
    // changes.
    void resize(int width, int height);

    // Uploads the given frame to the GPU and presents it. This is the one
    // method invoked once per decoded frame.
    void presentFrame(std::shared_ptr<const QImage> frame);

    // Releases every D3D11/DXGI/DirectComposition object in dependency
    // order. Must be called on this object's own thread before the thread
    // is asked to quit (COM objects must be released on the thread that
    // created them).
    void shutdown();

    // Explorer restarting destroys the old Progman, which (per documented
    // Win32 behavior) cascades to destroy our reparented child HWND too -
    // the DirectComposition target bound to that now-destroyed HWND is
    // left dangling. Call this after WallpaperWindow has created a *new*
    // native HWND to rebind the existing device/swap chain/visual/shaders/
    // textures to it, without recreating any of them.
    //
    // Deliberately void/asynchronous, not a blocking call: this can run
    // concurrently with a presentFrame() that was already mid-flight (e.g.
    // inside Present()) when Explorer destroyed the old target. A caller
    // on the GUI thread blocking on this via BlockingQueuedConnection was
    // confirmed by testing to freeze the entire application whenever that
    // race was hit - Present() can take an indeterminate amount of time to
    // return once its presentation target has been invalidated out from
    // under it. Completion is reported via rebindFinished() instead, which
    // Qt delivers back to the caller's thread as a normal queued signal.
    void rebindToWindow(HWND newHwnd);

    // Explorer's SetParent (in WindowsDesktopWallpaper::AttachToDesktop)
    // happens on a background thread, strictly after rebindToWindow's own
    // CreateTargetForHwnd/SetRoot/Commit sequence completed - i.e. after
    // this object already committed a composition target for the window
    // while it was still a top-level, unparented popup. That matches the
    // FIRST-ever attach at app startup exactly... except that at startup
    // no frames have been presented yet, whereas during Explorer-restart
    // recovery the video was actively playing right up until the old
    // window was destroyed. Re-committing once more here, after the
    // reparent into the new desktop hierarchy has actually happened,
    // costs nothing and rules out (or fixes, if it turns out to matter)
    // any DWM-side composition-tree state that specifically depends on
    // being committed post-reparent rather than only pre-reparent.
    void recommitAfterReparent();

signals:
    void rebindFinished(bool ok);

public:
    // Incremented once per successfully presented frame (Present()
    // returned S_OK or DXGI_STATUS_OCCLUDED). Atomic because it's written
    // on this object's render thread but read from the GUI thread (see
    // WallpaperWindow::presentedFrames()) purely as a "is video actually
    // still flowing" signal after Explorer-restart recovery - not for
    // synchronization of any other state.
    std::atomic<quint64> presentedFrameCount{0};

private:
    bool createDeviceAndSwapChain(int width, int height);
    bool createShaderPipeline();
    void updateSourceTexture(const QImage& img);
    void computeUvTransform(const QSize& imgSize, float outScale[2], float outOffset[2]) const;
    void releaseSizeDependentResources();

    HWND m_hwnd;
    ScalingMode m_scalingMode = ScalingMode::Fill;
    int m_width = 0;
    int m_height = 0;
    bool m_initialized = false;

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGIDevice* m_dxgiDevice = nullptr;
    IDCompositionDevice* m_dcompDevice = nullptr;
    IDCompositionTarget* m_dcompTarget = nullptr;
    IDCompositionVisual* m_dcompVisual = nullptr;
    IDXGISwapChain1* m_swapChain = nullptr;
    ID3D11RenderTargetView* m_rtv = nullptr;

    ID3D11VertexShader* m_vertexShader = nullptr;
    ID3D11PixelShader* m_pixelShader = nullptr;
    ID3D11SamplerState* m_sampler = nullptr;
    ID3D11Buffer* m_uvTransformBuffer = nullptr;

    ID3D11Texture2D* m_sourceTexture = nullptr;
    ID3D11ShaderResourceView* m_sourceSrv = nullptr;
    QSize m_sourceTextureSize;
};
