#include "VdDesktopChrome.h"

#include <QDateTime>
#include <QLocale>
#include <QPainter>

namespace {
struct Icon {
    const char* label;
    QColor color;
};
} // namespace

VdDesktopChrome::VdDesktopChrome(GlCanvas* canvas) : GlItem(canvas) {}

qreal VdDesktopChrome::taskbarHeight(const QSizeF& size) {
    return qBound<qreal>(32, size.height() * 0.045, 48);
}

void VdDesktopChrome::paint(QPainter& p, const GlUi::Style&) {
    const QRectF area = rect();
    if (area.isEmpty()) {
        return;
    }
    p.save();
    p.setClipRect(area);
    p.setRenderHint(QPainter::Antialiasing, true);

    // --- Desktop icons (top-left column) ---
    const qreal unit = qBound<qreal>(0.7, area.height() / 1080.0, 1.4);
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
    qreal y = area.top() + 12 * unit;
    for (const Icon& icon : icons) {
        const QRectF c(area.left() + 8 * unit, y, cell, cell);
        const QRectF g(c.center().x() - glyph / 2, c.top() + 8 * unit, glyph, glyph);
        p.setPen(Qt::NoPen);
        p.setBrush(icon.color);
        p.drawRoundedRect(g, 7 * unit, 7 * unit);
        p.setBrush(QColor(255, 255, 255, 70));
        p.drawRoundedRect(g.adjusted(glyph * 0.15, glyph * 0.15, -glyph * 0.15, -glyph * 0.5), 3 * unit, 3 * unit);
        const QRectF t(c.left(), g.bottom() + 4 * unit, c.width(), 18 * unit);
        // Soft shadow keeps the white label readable on any wallpaper, like Explorer's.
        p.setPen(QColor(0, 0, 0, 160));
        p.drawText(t.translated(1, 1), Qt::AlignHCenter | Qt::AlignTop, QObject::tr(icon.label));
        p.setPen(Qt::white);
        p.drawText(t, Qt::AlignHCenter | Qt::AlignTop, QObject::tr(icon.label));
        y += cell;
    }

    // --- Taskbar ---
    const qreal th = taskbarHeight(area.size());
    const QRectF bar(area.left(), area.bottom() - th, area.width(), th);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(28, 28, 34, 225));
    p.drawRect(bar);
    p.setPen(QColor(255, 255, 255, 25));
    p.drawLine(bar.topLeft(), bar.topRight());

    const qreal btn = th * 0.62;
    const qreal gap = th * 0.28;
    // Start button + a few app slots, centred like the default Windows 11 layout.
    constexpr int kSlots = 5;
    const qreal total = kSlots * btn + (kSlots - 1) * gap;
    qreal x = bar.center().x() - total / 2;
    const qreal by = bar.center().y() - btn / 2;
    static const QColor slotColors[kSlots] = {QColor(80, 150, 240), QColor(120, 200, 140), QColor(240, 170, 80),
                                              QColor(200, 110, 170), QColor(140, 140, 160)};
    p.setPen(Qt::NoPen);
    for (int i = 0; i < kSlots; ++i) {
        p.setBrush(slotColors[i]);
        p.drawRoundedRect(QRectF(x, by, btn, btn), btn * 0.22, btn * 0.22);
        x += btn + gap;
    }

    // Clock on the right.
    QFont clockFont = GlUi::uiFont(9);
    clockFont.setPointSizeF(qMax<qreal>(7, th * 0.24));
    p.setFont(clockFont);
    p.setPen(QColor(235, 235, 240));
    const QDateTime now = QDateTime::currentDateTime();
    const QString text = QLocale().toString(now.time(), QLocale::ShortFormat) + QLatin1Char('\n') +
                         QLocale().toString(now.date(), QLocale::ShortFormat);
    p.drawText(bar.adjusted(0, 0, -th * 0.4, 0), Qt::AlignRight | Qt::AlignVCenter, text);

    p.restore();
}
