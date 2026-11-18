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

void VdWallpaperSurface::setNote(const QString& text) {
    if (m_note == text) {
        return;
    }
    m_note = text;
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
    if (!m_note.isEmpty()) {
        p.setFont(GlUi::uiFont(11));
        const QRectF box(area.left() + 16, area.top() + 16, 260, 30);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(box, 6, 6);
        p.setPen(QColor(235, 235, 240));
        p.drawText(box, Qt::AlignCenter, m_note);
    }
    p.restore();
}
