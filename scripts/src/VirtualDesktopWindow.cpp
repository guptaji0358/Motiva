#include "VirtualDesktopWindow.h"

#include "GlUi.h"
#include "PreviewSource.h"
#include "VdWallpaperSurface.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QScreen>
#include <QVBoxLayout>

namespace {
constexpr Qt::WindowFlags kFlags = Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
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

    // Created after the surface so it paints above it (items paint in creation order).
    m_exitButton = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_exitButton->setText(tr("Exit Preview  (Esc)"));
    m_exitButton->setToolTip(tr("Leave the virtual desktop and return to Motiva"));
    connect(m_exitButton, &GlButton::clicked, this, &QWidget::close);

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
    m_exitButton->setRect(QRectF(size.width() - kExitW - kMargin, kMargin, kExitW, kExitH));
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
    if (m_source) {
        m_source->resumePreview(); // no-op for the active wallpaper source
    }
    m_canvas->setFocus();
}

void VirtualDesktopWindow::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_source) {
        m_source->pausePreview(); // stop decoding for a hidden preview
    }
}
