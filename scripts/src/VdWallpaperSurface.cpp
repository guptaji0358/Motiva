#include "VdWallpaperSurface.h"

#include <QPainter>

VdWallpaperSurface::VdWallpaperSurface(GlCanvas* canvas) : GlItem(canvas) {}

void VdWallpaperSurface::setFrame(std::shared_ptr<const QImage> frame) {
    m_frame = std::move(frame);
    update();
}

void VdWallpaperSurface::setScalingMode(DesktopScaling::Mode mode) {
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    update();
}

void VdWallpaperSurface::setPlaceholderText(const QString& text) {
    m_placeholder = text;
    update();
}

void VdWallpaperSurface::paint(QPainter& p, const GlUi::Style&) {
    const QRectF area = rect();
    p.save();
    p.setClipRect(area);
    p.fillRect(area, QColor(6, 6, 12));
    if (hasFrame()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QRectF dst = DesktopScaling::destRect(m_mode, QSizeF(m_frame->size()), area.size());
        dst.translate(area.topLeft());
        p.drawImage(dst, *m_frame);
    } else if (!m_placeholder.isEmpty()) {
        p.setFont(GlUi::uiFont(13));
        p.setPen(QColor(150, 150, 165));
        p.drawText(area, Qt::AlignCenter, m_placeholder);
    }
    p.restore();
}
