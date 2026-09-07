#include "WallpaperWindow.h"
#include "D3DWallpaperRenderer.h"
#include <QDebug>
#include <QMetaObject>

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

WallpaperWindow::WallpaperWindow(VideoPlayer* player, QObject* parent)
    : QObject(parent), m_player(player) {
    EnsureClassRegistered();

    // Created as an ordinary hidden top-level popup window; WallpaperManager
    // reparents it into the desktop via WindowsDesktopWallpaper before ever
    // showing it, so it's never visible in this top-level form.
    // WS_EX_NOREDIRECTIONBITMAP from the very start: this window's content
    // is presented via a DirectComposition visual (D3DWallpaperRenderer),
    // never via classic WM_PAINT/GDI, and diagnostics on this machine
    // found Progman itself carries this same extended style - see the
    // detailed rationale in WindowsDesktopWallpaper.cpp::AttachToDesktop.
    m_hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP, kClassName, L"Video Wallpaper Render Surface", WS_POPUP,
        0, 0, 64, 64,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

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
        bool ok = false;
        QMetaObject::invokeMethod(m_renderer, "initialize", Qt::BlockingQueuedConnection,
                                   Q_RETURN_ARG(bool, ok),
                                   Q_ARG(int, rect.width()), Q_ARG(int, rect.height()));
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
