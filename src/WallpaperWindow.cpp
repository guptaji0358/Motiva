#include "WallpaperWindow.h"
#include <QImage>

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
    wc.hbrBackground = nullptr; // we paint the entire client area ourselves
    wc.lpszClassName = kClassName;
    g_classAtom = RegisterClassExW(&wc);
}
} // namespace

LRESULT CALLBACK WallpaperWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WallpaperWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<WallpaperWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<WallpaperWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (msg == WM_PAINT && self) {
        self->handlePaint();
        return 0;
    }
    if (msg == WM_ERASEBKGND) {
        // We always paint the full client area in handlePaint(); skipping
        // the separate erase step avoids a visible flicker/flash.
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
    m_hwnd = CreateWindowExW(
        0, kClassName, L"Video Wallpaper Render Surface", WS_POPUP,
        0, 0, 64, 64,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);

    connect(m_player, &VideoPlayer::frameReady, this, &WallpaperWindow::onFrameReady);
}

WallpaperWindow::~WallpaperWindow() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void WallpaperWindow::setScalingMode(ScalingMode mode) {
    m_scalingMode = mode;
    if (m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void WallpaperWindow::setMonitorRect(const QRect& rect) {
    m_monitorRect = rect;
    if (m_hwnd) {
        SetWindowPos(m_hwnd, nullptr, rect.x(), rect.y(), rect.width(), rect.height(),
                     SWP_NOZORDER | SWP_NOACTIVATE);
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

void WallpaperWindow::onFrameReady() {
    if (m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

QRect WallpaperWindow::computeTargetRect(const QSize& imageSize, const QSize& widgetSize) const {
    const QRect widgetRect(QPoint(0, 0), widgetSize);
    QRect target;
    switch (m_scalingMode) {
        case ScalingMode::Stretch:
            target = widgetRect;
            break;
        case ScalingMode::Original:
            target = QRect(QPoint(0, 0), imageSize);
            target.moveCenter(widgetRect.center());
            break;
        case ScalingMode::Fit: {
            QSize scaled = imageSize.scaled(widgetSize, Qt::KeepAspectRatio);
            target = QRect(QPoint(0, 0), scaled);
            target.moveCenter(widgetRect.center());
            break;
        }
        case ScalingMode::Fill:
        default: {
            QSize scaled = imageSize.scaled(widgetSize, Qt::KeepAspectRatioByExpanding);
            target = QRect(QPoint(0, 0), scaled);
            target.moveCenter(widgetRect.center());
            break;
        }
    }
    return target;
}

void WallpaperWindow::handlePaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);

    RECT clientRect{};
    GetClientRect(m_hwnd, &clientRect);
    const int widgetW = clientRect.right - clientRect.left;
    const int widgetH = clientRect.bottom - clientRect.top;

    FillRect(hdc, &clientRect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    auto frame = m_player->currentFrame();
    if (frame && !frame->isNull() && widgetW > 0 && widgetH > 0) {
        // Already normalized to Format_RGB32 once in VideoPlayer (shared
        // across every monitor window) - no per-window conversion here.
        const QImage& img = *frame;

        QRect target = computeTargetRect(img.size(), QSize(widgetW, widgetH));

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = img.width();
        bmi.bmiHeader.biHeight = -img.height(); // negative: top-down DIB, matches QImage row order
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        // HALFTONE is GDI's highest-quality resample filter but is far too
        // slow for continuous real-time video (it's meant for one-off
        // stretches). COLORONCOLOR is a simple, fast filter appropriate
        // for per-frame blits; the source video's own resolution/bitrate
        // is untouched, only the on-screen scaling algorithm changes.
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(hdc,
                      target.x(), target.y(), target.width(), target.height(),
                      0, 0, img.width(), img.height(),
                      img.constBits(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    }

    EndPaint(m_hwnd, &ps);
}
