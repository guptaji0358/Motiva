#include "WallpaperWindow.h"
#include "D3DWallpaperRenderer.h"
#include "WindowsDesktopWallpaper.h"
#include <QDebug>
#include <QMetaObject>
#include <QElapsedTimer>

namespace {
constexpr const wchar_t* kClassName = L"VideoWallpaperRenderWindowClass";
ATOM g_classAtom = 0;

void EnsureClassRegistered() {
    if (g_classAtom != 0) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WallpaperWindow::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // No background brush and no WM_PAINT handling: presentation happens
    // entirely through the DirectComposition visual bound to this HWND
    // (see D3DWallpaperRenderer), not through classic GDI window painting.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    g_classAtom = RegisterClassExW(&wc);
}
} // namespace

LRESULT CALLBACK WallpaperWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        // Nothing to erase - DirectComposition owns this window's visible
        // content. Returning 1 avoids a default background flash.
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND WallpaperWindow::createNativeWindow() {
    EnsureClassRegistered();

    // Created as an ordinary hidden top-level popup window; WallpaperManager
    // reparents it into the desktop via WindowsDesktopWallpaper before ever
    // showing it, so it's never visible in this top-level form.
    // WS_EX_NOREDIRECTIONBITMAP from the very start: this window's content
    // is presented via a DirectComposition visual (D3DWallpaperRenderer),
    // never via classic WM_PAINT/GDI, and diagnostics on this machine
    // found Progman itself carries this same extended style - see the
    // detailed rationale in WindowsDesktopWallpaper.cpp::AttachToDesktop.
    return CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP, kClassName, L"Video Wallpaper Render Surface", WS_POPUP,
        0, 0, 64, 64,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

WallpaperWindow::WallpaperWindow(VideoPlayer* player, QObject* parent)
    : QObject(parent), m_player(player) {
    m_hwnd = createNativeWindow();

    // The renderer must have no parent when moved to another thread (Qt
    // requirement for moveToThread), and must live on its own thread so
    // that per-frame GPU upload + Present() (which blocks on vblank)
    // never runs on the GUI thread.
    m_renderer = new D3DWallpaperRenderer(m_hwnd);
    m_renderer->moveToThread(&m_renderThread);
    m_renderThread.start();

    // Qt automatically delivers this as a queued call on m_renderThread
    // since sender (VideoPlayer, GUI thread) and receiver (m_renderer,
    // render thread) live on different threads - no manual marshaling
    // needed, and this is the ONLY thing that runs at frame rate.
    connect(m_player, &VideoPlayer::frameReady, m_renderer, &D3DWallpaperRenderer::presentFrame);
    // Delivered back via a queued connection (render thread -> GUI
    // thread), continuing recoverFromExplorerRestart() without ever
    // blocking either thread on the other.
    connect(m_renderer, &D3DWallpaperRenderer::rebindFinished, this, &WallpaperWindow::onRebindFinished);
}

WallpaperWindow::~WallpaperWindow() {
    if (m_renderer) {
        // Must run ON the render thread (COM objects release on the
        // thread that created them) and must complete BEFORE the thread
        // is asked to quit.
        QMetaObject::invokeMethod(m_renderer, "shutdown", Qt::BlockingQueuedConnection);
        m_renderThread.quit();
        m_renderThread.wait();
        delete m_renderer;
        m_renderer = nullptr;
    }
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void WallpaperWindow::setScalingMode(ScalingMode mode) {
    m_scalingMode = mode;
    if (m_renderer) {
        QMetaObject::invokeMethod(m_renderer, "setScalingMode", Qt::QueuedConnection,
                                   Q_ARG(ScalingMode, mode));
    }
}

void WallpaperWindow::setMonitorRect(const QRect& rect) {
    const bool sizeChanged = (rect.width() != m_monitorRect.width() || rect.height() != m_monitorRect.height());
    m_monitorRect = rect;
    if (!m_hwnd) {
        return;
    }
    SetWindowPos(m_hwnd, nullptr, rect.x(), rect.y(), rect.width(), rect.height(),
                 SWP_NOZORDER | SWP_NOACTIVATE);

    if (!m_rendererReady) {
        QElapsedTimer t;
        t.start();
        bool ok = false;
        QMetaObject::invokeMethod(m_renderer, "initialize", Qt::BlockingQueuedConnection,
                                   Q_RETURN_ARG(bool, ok),
                                   Q_ARG(int, rect.width()), Q_ARG(int, rect.height()));
        qInfo() << "[DComp] D3D/DirectComposition initialize() took" << t.elapsed()
                << "ms, ok=" << ok;
        m_rendererReady = ok;
        if (!ok) {
            qWarning() << "[WallpaperWindow] D3D renderer failed to initialize - "
                           "this monitor's wallpaper surface will not present any frames "
                           "(see preceding D3DWallpaperRenderer log lines for the exact HRESULT/step).";
        } else {
            QMetaObject::invokeMethod(m_renderer, "setScalingMode", Qt::QueuedConnection,
                                       Q_ARG(ScalingMode, m_scalingMode));
        }
    } else if (sizeChanged) {
        QMetaObject::invokeMethod(m_renderer, "resize", Qt::QueuedConnection,
                                   Q_ARG(int, rect.width()), Q_ARG(int, rect.height()));
    }
}

void WallpaperWindow::recoverFromExplorerRestart() {
    // Unconditionally destroy-and-recreate rather than branching on
    // IsWindow(m_hwnd): a destroyed HWND's numeric value can be reused by
    // one of the many new windows Explorer creates within the next moment
    // during its own restart, so IsWindow() returning true here would not
    // reliably mean "our window survived" - it could just as easily mean
    // some unrelated Explorer-owned window now happens to hold the same
    // value, and code that trusted it would silently reattach the wrong
    // window while our real DirectComposition target stayed bound to a
    // dangling reference. Explicitly destroying first removes that whole
    // class of ambiguity.
    if (IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
    }
    qInfo() << "[Wallpaper] Old wallpaper hwnd invalidated by Explorer's restart - creating a new one.";
    m_hwnd = createNativeWindow();
    if (!m_hwnd) {
        qWarning() << "[Wallpaper] Failed to create a replacement native window.";
        return;
    }
    // Re-apply the geometry the window is supposed to have - a fresh HWND
    // starts at the placeholder 64x64 size from createNativeWindow().
    if (!m_monitorRect.isNull()) {
        SetWindowPos(m_hwnd, nullptr, m_monitorRect.x(), m_monitorRect.y(),
                     m_monitorRect.width(), m_monitorRect.height(), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Non-blocking: onRebindFinished (delivered back via a queued
    // connection) continues the sequence. Never wait here for the render
    // thread - it may still be finishing a presentFrame() call that was
    // already in flight against the old (now-destroyed) target when
    // Explorer tore it down, and blocking the GUI thread on that was
    // confirmed by testing to freeze the whole application.
    QMetaObject::invokeMethod(m_renderer, "rebindToWindow", Qt::QueuedConnection, Q_ARG(HWND, m_hwnd));
}

void WallpaperWindow::onRebindFinished(bool ok) {
    if (!ok) {
        qWarning() << "[Wallpaper] Failed to rebind DirectComposition target to the new window.";
        return;
    }
    // SetParent/SetWindowPos here are plain synchronous Win32 calls on the
    // GUI thread (as they always are for the initial attach too) - not a
    // cross-thread call, so there is nothing here that can be blocked on
    // the render thread.
    if (!WindowsDesktopWallpaper::AttachToDesktop(m_hwnd)) {
        qWarning() << "[Wallpaper] Re-attach after Explorer restart failed.";
        return;
    }
    qInfo() << "[Wallpaper] Recovery complete - wallpaper reattached after Explorer restart.";
}

void WallpaperWindow::showNative() {
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    }
}

void WallpaperWindow::hideNative() {
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_HIDE);
    }
}
