#include "VirtualDesktopWindow.h"

#include "GlUi.h"
#include "PreviewSource.h"
#include "VdDesktopChrome.h"
#include "VdWallpaperSurface.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QTimer>
#include <QEvent>
#include <windows.h>
#include <QVBoxLayout>

namespace {
constexpr Qt::WindowFlags kFlags = Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
constexpr Qt::WindowFlags kWindowedFlags = Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                                           Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint;
constexpr int kPowerPollMs = 5000;

bool onBattery() {
    SYSTEM_POWER_STATUS status{};
    return GetSystemPowerStatus(&status) && status.ACLineStatus == 0;
}
constexpr qreal kMargin = 20;
constexpr qreal kExitW = 170;
constexpr qreal kExitH = 36;
} // namespace

VirtualDesktopWindow::VirtualDesktopWindow(PreviewSource* source, DesktopScaling::Mode mode, QScreen* screen,
                                           QWidget* parent)
    : QWidget(parent, kFlags), m_source(source) {
    setAttribute(Qt::WA_DeleteOnClose, false); // the session owns and deletes this window
    setWindowTitle(tr("Motiva Virtual Desktop"));
    setFocusPolicy(Qt::StrongFocus);
    if (screen) {
        setScreen(screen);
        setGeometry(screen->geometry());
    }

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_canvas = new GlCanvas(this);
    // No animated GLSL backdrop: the wallpaper covers it, and Phase 0 showed
    // GL windows are the dominant per-window cost.
    m_canvas->setBackgroundAnimated(false);
    layout->addWidget(m_canvas);

    m_surface = new VdWallpaperSurface(m_canvas);
    m_surface->setScalingMode(mode);
    m_surface->setPlaceholderText(tr("No wallpaper loaded"));

    // Simulated icons + taskbar, above the wallpaper and below the controls.
    m_chrome = new VdDesktopChrome(m_canvas);
    m_chrome->setVisible(false);

    // Created after the surface so it paints above it (items paint in creation order).
    m_exitButton = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_exitButton->setText(tr("Exit Preview  (Esc)"));
    m_exitButton->setToolTip(tr("Leave the virtual desktop and return to Motiva"));
    connect(m_exitButton, &GlButton::clicked, this, &QWidget::close);

    m_modeButton = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_modeButton->setText(tr("Windowed  (F11)"));
    m_modeButton->setToolTip(tr("Switch between full screen and a resizable window"));
    connect(m_modeButton, &GlButton::clicked, this, [this] { setWindowed(!m_windowed); });

    m_chromeButton = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_chromeButton->setText(tr("Show Icons  (I)"));
    m_chromeButton->setToolTip(tr("Show simulated desktop icons and a taskbar over the wallpaper"));
    connect(m_chromeButton, &GlButton::clicked, this, [this] { setChromeVisible(!m_chrome->isVisible()); });

    m_powerTimer = new QTimer(this);
    m_powerTimer->setInterval(kPowerPollMs);
    connect(m_powerTimer, &QTimer::timeout, this, &VirtualDesktopWindow::applyPlayState);

    m_canvas->setLayoutCallback([this](const QSizeF& size) { layoutItems(size); });
    m_canvas->relayout();

    if (m_source) {
        connect(m_source, &PreviewSource::frameReady, this, [this](std::shared_ptr<const QImage> frame) {
            m_surface->setFrame(std::move(frame));
        });
        connect(m_source, &PreviewSource::mediaChanged, this, &VirtualDesktopWindow::applyFrameFromSource);
        applyFrameFromSource();
    }
}

void VirtualDesktopWindow::layoutItems(const QSizeF& size) {
    m_surface->setRect(QRectF(QPointF(0, 0), size));
    m_chrome->setRect(QRectF(QPointF(0, 0), size));
    layoutButtons();
}

void VirtualDesktopWindow::layoutButtons() {
    const QSizeF size = m_surface->rect().size();
    const qreal gap = 10;
    qreal x = size.width() - kExitW - kMargin;
    m_exitButton->setRect(QRectF(x, kMargin, kExitW, kExitH));
    x -= kExitW + gap;
    m_modeButton->setRect(QRectF(x, kMargin, kExitW, kExitH));
    x -= kExitW + gap;
    m_chromeButton->setRect(QRectF(x, kMargin, kExitW, kExitH));
}

void VirtualDesktopWindow::setChromeVisible(bool visible) {
    m_chrome->setVisible(visible);
    m_chromeButton->setText(visible ? tr("Hide Icons  (I)") : tr("Show Icons  (I)"));
    m_canvas->update();
}

void VirtualDesktopWindow::setWindowed(bool windowed) {
    if (windowed == m_windowed) {
        return;
    }
    m_windowed = windowed;
    QScreen* screen = windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
    // Changing flags recreates the native window; the hide/show pair below is
    // handled by applyPlayState() so an independent source just resumes.
    setWindowFlags(windowed ? kWindowedFlags : kFlags);
    if (windowed) {
        if (!m_windowedGeometry.isValid()) {
            const QSize sz(avail.width() * 6 / 10, avail.height() * 6 / 10);
            m_windowedGeometry = QRect(avail.center() - QPoint(sz.width() / 2, sz.height() / 2), sz);
        }
        setGeometry(m_windowedGeometry);
        m_modeButton->setText(tr("Fullscreen  (F11)"));
        show();
    } else {
        m_windowedGeometry = geometry();
        setGeometry(screen ? screen->geometry() : avail);
        m_modeButton->setText(tr("Windowed  (F11)"));
        show();
    }
    raise();
    activateWindow();
    m_canvas->setFocus();
}

void VirtualDesktopWindow::setPauseOnBattery(bool pause) {
    m_pauseOnBattery = pause;
    applyPlayState();
}

void VirtualDesktopWindow::applyPlayState() {
    if (!m_source) {
        return;
    }
    const bool pause = m_pauseOnBattery && onBattery();
    m_batteryPaused = pause;
    m_surface->setNote(pause ? tr("Paused on battery") : QString());
    if (pause || !isVisible()) {
        m_source->pausePreview();
    } else {
        m_source->resumePreview();
    }
}

void VirtualDesktopWindow::applyFrameFromSource() {
    // Keep the last frame when media changes; a null frame (media unloaded)
    // falls back to the placeholder instead of closing the preview.
    m_surface->setFrame(m_source ? m_source->currentFrame() : nullptr);
}

void VirtualDesktopWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    if (event->key() == Qt::Key_F11) {
        setWindowed(!m_windowed);
        return;
    }
    if (event->key() == Qt::Key_I) {
        setChromeVisible(!m_chrome->isVisible());
        return;
    }
    QWidget::keyPressEvent(event);
}

void VirtualDesktopWindow::closeEvent(QCloseEvent* event) {
    event->accept();
    if (!m_closedEmitted) {
        m_closedEmitted = true;
        emit closed();
    }
}

void VirtualDesktopWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    applyPlayState(); // resumes unless paused by the battery policy; no-op for the active source
    m_powerTimer->start();
    m_canvas->setFocus();
}

void VirtualDesktopWindow::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    m_powerTimer->stop();
    if (m_source) {
        m_source->pausePreview(); // stop decoding for a hidden preview
    }
}

void VirtualDesktopWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    // A minimised windowed preview needn't decode.
    if (event->type() == QEvent::WindowStateChange && m_source) {
        if (isMinimized()) {
            m_source->pausePreview();
        } else {
            applyPlayState();
        }
    }
}
