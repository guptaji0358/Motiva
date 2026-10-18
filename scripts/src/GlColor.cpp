#include "GlColor.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace {
QColor textOn(const QColor& bg) {
    return Theme::contrastRatio(Qt::white, bg) >= Theme::contrastRatio(Qt::black, bg) ? QColor(Qt::white)
                                                                                      : QColor(8, 8, 12);
}
} // namespace

void GlSwatchButton::paint(QPainter& p, const GlUi::Style& s) {
    const QRectF r = rect();
    const qreal h = hoverAmount();
    const qreal rad = std::max(5.0, 12 * s.radius);
    p.save();
    p.setPen(QPen(GlUi::Style::current().glassEdge, 1.2));
    p.setBrush(m_color);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    if (h > 0.01) {
        QColor g = s.accent2;
        g.setAlphaF(0.8 * h);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(g, 2.0));
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), rad, rad);
    }
    p.setFont(GlUi::uiFont(10, true));
    p.setPen(textOn(m_color));
    p.drawText(r.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, m_color.name().toUpper());
    if (m_custom) {
        p.setFont(GlUi::uiFont(8.5));
        p.drawText(r.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::AlignRight, QObject::tr("custom"));
    }
    p.restore();
    if (hasFocus()) {
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.accent2, 2.0));
        p.drawRoundedRect(r.adjusted(-2, -2, 2, 2), rad + 2, rad + 2);
        p.restore();
    }
}

void GlColorSquare::pick(const QPointF& pt) {
    const QRectF r = rect();
    const qreal sat = std::clamp((pt.x() - r.left()) / r.width(), 0.0, 1.0);
    const qreal val = 1.0 - std::clamp((pt.y() - r.top()) / r.height(), 0.0, 1.0);
    emit picked(sat, val);
}

void GlColorSquare::paint(QPainter& p, const GlUi::Style& s) {
    const QRectF r = rect();
    const qreal rad = std::max(4.0, 12 * s.radius);
    QPainterPath clip;
    clip.addRoundedRect(r, rad, rad);
    p.save();
    p.setClipPath(clip);
    QLinearGradient hx(r.topLeft(), r.topRight());
    hx.setColorAt(0, Qt::white);
    hx.setColorAt(1, QColor::fromHsvF(m_h, 1, 1));
    p.fillRect(r, hx);
    QLinearGradient vy(r.topLeft(), r.bottomLeft());
    vy.setColorAt(0, QColor(0, 0, 0, 0));
    vy.setColorAt(1, Qt::black);
    p.fillRect(r, vy);
    p.restore();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(s.glassEdge, 1.2));
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    const QPointF c(r.left() + m_s * r.width(), r.top() + (1.0 - m_v) * r.height());
    p.setPen(QPen(Qt::black, 3));
    p.drawEllipse(c, 8, 8);
    p.setPen(QPen(Qt::white, 2));
    p.drawEllipse(c, 8, 8);
}

void GlHueBar::pick(const QPointF& pt) {
    emit picked(std::clamp((pt.x() - rect().left()) / rect().width(), 0.0, 0.9999));
}

void GlHueBar::paint(QPainter& p, const GlUi::Style& s) {
    const QRectF r = rect();
    const qreal rad = r.height() / 2;
    QLinearGradient g(r.topLeft(), r.topRight());
    for (int i = 0; i <= 6; ++i) {
        g.setColorAt(i / 6.0, QColor::fromHsvF(i == 6 ? 0.0 : i / 6.0, 1, 1));
    }
    p.save();
    p.setPen(QPen(s.glassEdge, 1.0));
    p.setBrush(g);
    p.drawRoundedRect(r, rad, rad);
    const QPointF c(r.left() + m_h * r.width(), r.center().y());
    p.setPen(QPen(Qt::black, 3));
    p.setBrush(QColor::fromHsvF(m_h, 1, 1));
    p.drawEllipse(c, rad + 2, rad + 2);
    p.setPen(QPen(Qt::white, 2));
    p.drawEllipse(c, rad + 2, rad + 2);
    p.restore();
}

// ------------------------------------------------------------ GlColorDialog

GlColorDialog::GlColorDialog(const QColor& initial, QWidget* parent, const QString& title)
    : GlWindow(title, tr("Pick a color"), parent, false), m_initial(initial) {
    setModal(true);
    GlCanvas* c = canvas();
    m_square = new GlColorSquare(c);
    m_hue = new GlHueBar(c);
    m_hexLabel = new GlLabel(c);
    m_hexLabel->setText(tr("Hex"));
    m_hexLabel->setTone(GlLabel::Tone::Dim);
    m_hex = new GlTextInput(c);
    m_beforeLabel = new GlLabel(c);
    m_beforeLabel->setText(tr("Current"));
    m_beforeLabel->setTone(GlLabel::Tone::Dim);
    m_beforeLabel->setPointSize(9);
    m_afterLabel = new GlLabel(c);
    m_afterLabel->setText(tr("New"));
    m_afterLabel->setTone(GlLabel::Tone::Dim);
    m_afterLabel->setPointSize(9);
    m_before = new GlSwatchButton(c);
    m_before->setColor(initial);
    m_before->setEnabled(false);
    m_after = new GlSwatchButton(c);
    m_after->setEnabled(false);
    m_ok = new GlButton(c, GlButton::Kind::Primary);
    m_ok->setText(tr("Select"));
    m_cancel = new GlButton(c, GlButton::Kind::Secondary);
    m_cancel->setText(tr("Cancel"));
    connect(m_ok, &GlButton::clicked, this, &QDialog::accept);
    connect(m_cancel, &GlButton::clicked, this, &QDialog::reject);
    connect(m_square, &GlColorSquare::picked, this, [this](qreal s, qreal v) {
        m_s = s;
        m_v = v;
        setColor(QColor::fromHsvF(m_h, m_s, m_v), true);
    });
    connect(m_hue, &GlHueBar::picked, this, [this](qreal h) {
        m_h = h;
        setColor(QColor::fromHsvF(m_h, m_s, m_v), true);
    });
    connect(m_hex, &GlTextInput::textChanged, this, [this](const QString& t) {
        if (m_syncing) {
            return;
        }
        QString hex = t.trimmed();
        if (!hex.startsWith(QLatin1Char('#'))) {
            hex.prepend(QLatin1Char('#'));
        }
        const QColor parsed(hex);
        if (parsed.isValid() && hex.size() == 7) {
            float h, s, v, a;
            parsed.getHsvF(&h, &s, &v, &a);
            if (h >= 0) m_h = h;
            m_s = s;
            m_v = v;
            setColor(parsed, false);
        }
    });
    connect(m_hex, &GlTextInput::editingFinished, this, &QDialog::accept);
    float h, s, v, a;
    initial.getHsvF(&h, &s, &v, &a);
    m_h = h < 0 ? 0 : h;
    m_s = s;
    m_v = v;
    setColor(initial, true);
    setFooterHeight(0);
    setFixedSize(480, 560);
}

void GlColorDialog::setColor(const QColor& c, bool updateHex) {
    m_color = c;
    m_square->setHsv(m_h, m_s, m_v);
    m_hue->setHue(m_h);
    m_after->setColor(c);
    if (updateHex) {
        m_syncing = true;
        m_hex->setText(c.name().toUpper());
        m_syncing = false;
    }
}

void GlColorDialog::layoutContent(const QRectF& content, const QRectF&) {
    m_square->setRect(QRectF(content.left(), content.top(), content.width(), 240));
    m_hue->setRect(QRectF(content.left() + 6, content.top() + 262, content.width() - 12, 18));
    qreal y = content.top() + 300;
    m_hexLabel->setRect(QRectF(content.left(), y, 50, 38));
    m_hex->setRect(QRectF(content.left() + 56, y, 150, 38));
    const qreal sw = (content.width() - 230 - 12) / 2;
    m_beforeLabel->setRect(QRectF(content.left() + 226, y - 18, sw, 16));
    m_afterLabel->setRect(QRectF(content.left() + 226 + sw + 12, y - 18, sw, 16));
    m_before->setRect(QRectF(content.left() + 226, y, sw, 38));
    m_after->setRect(QRectF(content.left() + 226 + sw + 12, y, sw, 38));
    m_cancel->setRect(QRectF(content.right() - 130 - 10 - 130, content.bottom() - 46, 130, 44));
    m_ok->setRect(QRectF(content.right() - 130, content.bottom() - 46, 130, 44));
}

QColor GlColorDialog::getColor(const QColor& initial, QWidget* parent, const QString& title) {
    GlColorDialog d(initial, parent, title);
    if (d.exec() == QDialog::Accepted) {
        return d.m_color;
    }
    return QColor();
}
