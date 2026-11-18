#include "VdDesktopChrome.h"

#include <QDateTime>
#include <QLocale>
#include <QPainter>

using namespace VdDesktop;

namespace {
struct Icon {
    const char* label;
    QColor color;
};

void drawShadowedText(QPainter& p, const QRectF& r, int flags, const QString& text) {
    p.setPen(QColor(0, 0, 0, 160));
    p.drawText(r.translated(1, 1), flags, text);
    p.setPen(Qt::white);
    p.drawText(r, flags, text);
}
} // namespace

VdDesktopChrome::VdDesktopChrome(GlCanvas* canvas) : GlItem(canvas) {}

void VdDesktopChrome::mousePressAt(const QPointF& p) {
    m_model.setArea(rect().size());
    if (m_model.press(p - rect().topLeft())) {
        update();
    }
}

void VdDesktopChrome::mouseDragTo(const QPointF& p) {
    if (m_model.isDragging()) {
        m_model.dragTo(p - rect().topLeft());
        update();
    }
}

void VdDesktopChrome::mouseReleased(const QPointF&) {
    m_model.release();
}

void VdDesktopChrome::paint(QPainter& p, const GlUi::Style&) {
    const QRectF area = rect();
    if (area.isEmpty()) {
        return;
    }
    m_model.setArea(area.size());
    p.save();
    p.setClipRect(area);
    p.translate(area.topLeft());
    p.setRenderHint(QPainter::Antialiasing, true);
    const QSizeF size = area.size();

    // --- Desktop icons (top-left column) ---
    const qreal unit = qBound<qreal>(0.7, size.height() / 1080.0, 1.4);
    const qreal cell = 92 * unit;
    const qreal glyph = 40 * unit;
    static const Icon icons[] = {
        {"This PC", QColor(90, 160, 235)},
        {"Recycle Bin", QColor(150, 200, 170)},
        {"Documents", QColor(240, 200, 90)},
        {"Motiva", QColor(170, 120, 240)},
    };
    QFont labelFont = GlUi::uiFont(9);
    labelFont.setPointSizeF(qMax<qreal>(7, 9 * unit));
    p.setFont(labelFont);
    qreal y = 12 * unit;
    for (const Icon& icon : icons) {
        const QRectF c(8 * unit, y, cell, cell);
        const QRectF g(c.center().x() - glyph / 2, c.top() + 8 * unit, glyph, glyph);
        p.setPen(Qt::NoPen);
        p.setBrush(icon.color);
        p.drawRoundedRect(g, 7 * unit, 7 * unit);
        p.setBrush(QColor(255, 255, 255, 70));
        p.drawRoundedRect(g.adjusted(glyph * 0.15, glyph * 0.15, -glyph * 0.15, -glyph * 0.5), 3 * unit, 3 * unit);
        drawShadowedText(p, QRectF(c.left(), g.bottom() + 4 * unit, c.width(), 18 * unit),
                         Qt::AlignHCenter | Qt::AlignTop, QObject::tr(icon.label));
        y += cell;
    }

    // --- Simulated windows, bottom -> top ---
    for (const int i : m_model.zOrder()) {
        const SimWindow& win = m_model.window(i);
        if (!win.open) {
            continue;
        }
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 70));
        p.drawRoundedRect(win.rect.translated(0, 6), 8, 8); // shadow
        p.setBrush(QColor(246, 246, 249));
        p.drawRoundedRect(win.rect, 8, 8);
        const QRectF bar = titleBarRect(win.rect);
        p.setBrush(QColor(226, 228, 236));
        p.drawRoundedRect(bar, 8, 8);
        p.drawRect(QRectF(bar.left(), bar.top() + 14, bar.width(), bar.height() - 14));
        p.setFont(GlUi::uiFont(10));
        p.setPen(QColor(40, 40, 50));
        p.drawText(bar.adjusted(12, 0, -40, 0), Qt::AlignVCenter | Qt::AlignLeft, win.title);
        p.setPen(QPen(QColor(60, 60, 70), 1.6));
        const QPointF cc = closeButtonRect(win.rect).center();
        p.drawLine(cc + QPointF(-5, -5), cc + QPointF(5, 5));
        p.drawLine(cc + QPointF(-5, 5), cc + QPointF(5, -5));
        // Placeholder content lines.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(205, 208, 218));
        for (int row = 0; row < 6; ++row) {
            const qreal ly = win.rect.top() + 48 + row * 22;
            if (ly + 10 > win.rect.bottom() - 8) {
                break;
            }
            p.drawRoundedRect(QRectF(win.rect.left() + 16, ly, win.rect.width() * (0.8 - 0.09 * (row % 3)), 10), 4, 4);
        }
    }

    // --- Start menu ---
    if (m_model.startOpen()) {
        const QRectF menu = startMenuRect(size);
        p.setPen(QColor(255, 255, 255, 30));
        p.setBrush(QColor(34, 34, 42, 240));
        p.drawRoundedRect(menu, 10, 10);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 28));
        p.drawRoundedRect(QRectF(menu.left() + 16, menu.top() + 16, menu.width() - 32, 32), 16, 16); // search box
        p.setFont(GlUi::uiFont(9));
        p.setPen(QColor(200, 200, 210));
        p.drawText(QRectF(menu.left() + 32, menu.top() + 16, menu.width() - 64, 32), Qt::AlignVCenter, QObject::tr("Search"));
        static const QColor tiles[] = {QColor(80, 150, 240), QColor(120, 200, 140), QColor(240, 170, 80),
                                       QColor(200, 110, 170), QColor(140, 140, 160), QColor(100, 190, 200)};
        const qreal tile = 44;
        const qreal gx = (menu.width() - 32 - 6 * tile) / 5;
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 6; ++i) {
            p.setBrush(tiles[i]);
            p.drawRoundedRect(QRectF(menu.left() + 16 + i * (tile + gx), menu.top() + 68, tile, tile), 9, 9);
        }
        p.setBrush(QColor(255, 255, 255, 22));
        for (int row = 0; row < 5; ++row) {
            const qreal ry = menu.top() + 132 + row * 34;
            if (ry + 24 > menu.bottom() - 52) {
                break;
            }
            p.drawRoundedRect(QRectF(menu.left() + 16, ry, menu.width() - 32, 24), 6, 6);
        }
        p.setBrush(QColor(255, 255, 255, 14));
        p.drawRect(QRectF(menu.left(), menu.bottom() - 44, menu.width(), 44)); // footer
    }

    // --- Taskbar ---
    const QRectF bar = taskbarRect(size);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(28, 28, 34, 225));
    p.drawRect(bar);
    p.setPen(QColor(255, 255, 255, 25));
    p.drawLine(bar.topLeft(), bar.topRight());
    static const QColor slotColors[kSlotCount] = {QColor(80, 150, 240), QColor(120, 200, 140), QColor(240, 170, 80),
                                                  QColor(200, 110, 170), QColor(140, 140, 160)};
    p.setPen(Qt::NoPen);
    for (int i = 0; i < kSlotCount; ++i) {
        const QRectF s = slotRect(size, i);
        p.setBrush(slotColors[i]);
        p.drawRoundedRect(s, s.width() * 0.22, s.width() * 0.22);
        // Indicator under an open window's slot / the open Start button.
        const bool lit = (i == 0 && m_model.startOpen()) || (i >= 1 && i <= kWindowCount && m_model.window(i - 1).open);
        if (lit) {
            p.setBrush(QColor(255, 255, 255, 220));
            p.drawRoundedRect(QRectF(s.center().x() - 7, bar.bottom() - 4, 14, 3), 1.5, 1.5);
        }
    }

    // Clock on the right.
    QFont clockFont = GlUi::uiFont(9);
    clockFont.setPointSizeF(qMax<qreal>(7, bar.height() * 0.24));
    p.setFont(clockFont);
    p.setPen(QColor(235, 235, 240));
    const QDateTime now = QDateTime::currentDateTime();
    const QString text = QLocale().toString(now.time(), QLocale::ShortFormat) + QLatin1Char('\n') +
                         QLocale().toString(now.date(), QLocale::ShortFormat);
    p.drawText(bar.adjusted(0, 0, -bar.height() * 0.4, 0), Qt::AlignRight | Qt::AlignVCenter, text);

    p.restore();
}
