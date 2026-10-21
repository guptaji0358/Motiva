#include "GlUi.h"
#include "Theme.h"

#include <QFocusEvent>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QAbstractItemModel>
#include <QAbstractItemDelegate>
#include <QStyleOptionViewItem>
#include "LibraryDatabase.h"
#include <QWheelEvent>
#include <QScreen>
#include <QWindow>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRadialGradient>
#include <QDebug>
#include <QVector2D>
#include <QSvgRenderer>
#include <QTimer>
#include <QVector3D>
#include <QtMath>
#include <algorithm>

namespace GlUi {

namespace {
QColor mix(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}
} // namespace

Style Style::current() {
    const Theme::ThemePalette& p = Theme::themePalette(Theme::currentTheme());
    Style s;
    s.dark = p.isDark;
    s.accent = p.accent;
    float h, sat, v;
    p.accent.getHsvF(&h, &sat, &v);
    h = std::fmod(h + 0.16f, 1.0f);
    s.accent2 = QColor::fromHsvF(h, std::max(sat, 0.65f), std::max(v, 0.88f));
    s.onAccent = p.selectionText;
    s.bgBase = p.windowBg;
    s.bgTint = mix(p.windowBg, p.accent, s.dark ? 0.30 : 0.16);
    s.glass = s.dark ? QColor(255, 255, 255, 20) : QColor(255, 255, 255, 150);
    s.glassEdge = s.dark ? QColor(255, 255, 255, 46) : QColor(0, 0, 0, 34);
    s.text = p.textPrimary;
    s.textDim = p.textSecondary;
    s.ok = QColor(QLatin1String(Theme::kStatusSuccess));
    s.warn = QColor(QLatin1String(Theme::kStatusWarning));
    s.bad = QColor(QLatin1String(Theme::kStatusError));
    const Theme::AppTheme t = Theme::currentTheme();
    s.radius = (t == Theme::AppTheme::DarkOnyx || t == Theme::AppTheme::LightOnyx) ? 0.4 : 1.0;
    return s;
}

QFont uiFont(qreal pointSize, bool bold) {
    QFont f;
    f.setFamilies({QStringLiteral("Bahnschrift"), QStringLiteral("Segoe UI")});
    f.setPointSizeF(pointSize);
    f.setWeight(bold ? QFont::DemiBold : QFont::Normal);
    return f;
}

bool handleFrameHitTest(QWidget* w, GlCanvas* canvas, const QRectF& captionRect, const void* nativeMsg,
                        qintptr* result, bool resizable) {
    const MSG* msg = static_cast<const MSG*>(nativeMsg);
    if (msg->message != WM_NCHITTEST || !canvas) {
        return false;
    }
    RECT wr;
    GetWindowRect(reinterpret_cast<HWND>(w->winId()), &wr);
    const LONG px = GET_X_LPARAM(msg->lParam) - wr.left;
    const LONG py = GET_Y_LPARAM(msg->lParam) - wr.top;
    const qreal dpr = w->devicePixelRatioF();
    const LONG ww = wr.right - wr.left, hh = wr.bottom - wr.top;
    const LONG edge = LONG(std::lround(7 * dpr));
    if (resizable && !w->isMaximized()) {
        const bool l = px < edge, r = px >= ww - edge, t = py < edge, b = py >= hh - edge;
        if (t && l) { *result = HTTOPLEFT; return true; }
        if (t && r) { *result = HTTOPRIGHT; return true; }
        if (b && l) { *result = HTBOTTOMLEFT; return true; }
        if (b && r) { *result = HTBOTTOMRIGHT; return true; }
        if (l) { *result = HTLEFT; return true; }
        if (r) { *result = HTRIGHT; return true; }
        if (t) { *result = HTTOP; return true; }
        if (b) { *result = HTBOTTOM; return true; }
    }
    const QPointF local(px / dpr, py / dpr);
    *result = (captionRect.contains(local) && !canvas->interactiveAt(local)) ? HTCAPTION : HTCLIENT;
    return true;
}

void applyRoundedCorners(QWidget* w) {
    static const DWORD kCornerPref = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
    const int round = 2;                 // DWMWCP_ROUND
    DwmSetWindowAttribute(reinterpret_cast<HWND>(w->winId()), kCornerPref, &round, sizeof(round));
}

namespace {
QColor bestText(const QColor& bg) {
    return Theme::contrastRatio(Qt::white, bg) >= Theme::contrastRatio(Qt::black, bg) ? QColor(Qt::white)
                                                                                      : QColor(8, 8, 12);
}

void glow(QPainter& p, const QRectF& r, qreal rad, const QColor& c, qreal strength) {
    if (strength <= 0.01) {
        return;
    }
    p.save();
    p.setBrush(Qt::NoBrush);
    for (int i = 1; i <= 7; ++i) {
        QColor k = c;
        k.setAlphaF(std::clamp(strength * 0.20 * (1.0 - i / 8.0), 0.0, 1.0));
        p.setPen(QPen(k, 2.2));
        p.drawRoundedRect(r.adjusted(-i * 1.5, -i * 1.5, i * 1.5, i * 1.5), rad + i * 1.5, rad + i * 1.5);
    }
    p.restore();
}

void drawGlassCard(QPainter& p, const QRectF& r, qreal rad, const Style& s, qreal glowAmt) {
    p.save();
    // Soft layered drop shadow.
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 5; ++i) {
        p.setBrush(QColor(0, 0, 0, s.dark ? 34 - i * 5 : 16 - i * 2));
        p.drawRoundedRect(r.translated(0, 3 + i * 2).adjusted(-i, -i, i, i), rad + i, rad + i);
    }
    // Glass body + top sheen.
    p.setBrush(s.glass);
    p.drawRoundedRect(r, rad, rad);
    QLinearGradient sheen(r.topLeft(), r.bottomLeft());
    sheen.setColorAt(0.0, QColor(255, 255, 255, s.dark ? 30 : 90));
    sheen.setColorAt(0.5, QColor(255, 255, 255, 0));
    p.setBrush(sheen);
    p.drawRoundedRect(r, rad, rad);
    // Rim: accent -> accent2 gradient, brighter with glow.
    QLinearGradient rim(r.topLeft(), r.bottomRight());
    QColor a = s.accent, b = s.accent2;
    a.setAlphaF(0.30 + 0.5 * glowAmt);
    b.setAlphaF(0.12 + 0.5 * glowAmt);
    rim.setColorAt(0, a);
    rim.setColorAt(1, b);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(rim), 1.2));
    p.drawRoundedRect(r.adjusted(0.6, 0.6, -0.6, -0.6), rad, rad);
    p.restore();
    glow(p, r, rad, s.accent, glowAmt * 0.8);
}

// Monochrome glyph from a QIcon, tinted to `color`.
QPixmap tintedGlyph(const QIcon& icon, const QSizeF& size, const QColor& color, qreal dpr) {
    QPixmap src = icon.pixmap(size.toSize(), dpr);
    if (src.isNull()) {
        return src;
    }
    QPainter tp(&src);
    tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
    tp.fillRect(src.rect(), color);
    tp.end();
    return src;
}
} // namespace

} // namespace GlUi

using GlUi::Style;
using namespace GlUi;

// ---------------------------------------------------------------- GlItem

GlItem::GlItem(GlCanvas* canvas) : QObject(canvas), m_canvas(canvas) {
    canvas->registerItem(this);
}

GlItem::~GlItem() {
    if (m_canvas) {
        m_canvas->unregisterItem(this);
    }
}

void GlItem::setRect(const QRectF& r) {
    if (r == m_rect) {
        return;
    }
    m_rect = r;
    update();
}

void GlItem::setClip(const QRectF& r) {
    if (r == m_clip) {
        return;
    }
    m_clip = r;
    update();
}

bool GlItem::isHit(const QPointF& p) const {
    return m_visible && m_rect.contains(p) && (m_clip.isNull() || m_clip.contains(p));
}

void GlItem::setVisible(bool v) {
    if (m_visible == v) {
        return;
    }
    m_visible = v;
    if (!v) {
        m_hovered = m_pressed = false;
        if (m_canvas->focusedItem() == this) {
            m_canvas->setFocusedItem(nullptr);
        }
    }
    update();
}

void GlItem::setEnabled(bool e) {
    if (m_enabled == e) {
        return;
    }
    m_enabled = e;
    if (!e && m_canvas->focusedItem() == this) {
        m_canvas->setFocusedItem(nullptr);
    }
    update();
}

bool GlItem::hasFocus() const {
    return m_canvas->focusedItem() == this && m_canvas->hasFocus() && m_canvas->m_keyboardFocusVisible;
}

void GlItem::update() {
    m_canvas->requestAnimation();
}

bool GlItem::advance(qreal dt) {
    const qreal th = (m_hovered && m_enabled) ? 1.0 : 0.0;
    const qreal tp = (m_pressed && m_enabled) ? 1.0 : 0.0;
    const qreal k = std::min(1.0, dt * 14.0);
    m_hoverT += (th - m_hoverT) * k;
    m_pressT += (tp - m_pressT) * std::min(1.0, dt * 24.0);
    if (std::abs(th - m_hoverT) < 0.01) m_hoverT = th;
    if (std::abs(tp - m_pressT) < 0.01) m_pressT = tp;
    return m_hoverT != th || m_pressT != tp;
}

// --------------------------------------------------------------- GlPanel

GlPanel::GlPanel(GlCanvas* canvas) : GlItem(canvas) {}

void GlPanel::paint(QPainter& p, const Style& s) {
    const qreal rad = m_radius * s.radius;
    GlUi::drawGlassCard(p, rect(), rad, s, 0.0);
    if (m_block == Block::Warning || m_block == Block::Danger) {
        p.save();
        QColor w = m_block == Block::Danger ? s.bad : s.warn;
        w.setAlphaF(0.14);
        p.setPen(Qt::NoPen);
        p.setBrush(w);
        p.drawRoundedRect(rect(), rad, rad);
        w.setAlphaF(0.9);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(w, 1.4));
        p.drawRoundedRect(rect().adjusted(0.7, 0.7, -0.7, -0.7), rad, rad);
        p.setPen(Qt::NoPen);
        p.setBrush(w);
        p.drawRoundedRect(QRectF(rect().left() + 8, rect().top() + 14, 4, rect().height() - 28), 2, 2);
        p.restore();
    } else if (m_block != Block::None) {
        const QColor c = m_block == Block::Accent ? s.accent : s.accent2;
        QRectF tab(rect().left() + 14, rect().top() - 1, 34, 5);
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(tab, 2.5 * s.radius, 2.5 * s.radius);
        p.restore();
    }
}

// --------------------------------------------------------------- GlLabel

GlLabel::GlLabel(GlCanvas* canvas) : GlItem(canvas) {}

void GlLabel::setText(const QString& t) {
    if (t == m_text) {
        return;
    }
    m_text = t;
    update();
}
void GlLabel::setPixmap(const QPixmap& pm) {
    m_pixmap = pm;
    update();
}
void GlLabel::setPointSize(qreal pt) {
    m_pt = pt;
    update();
}
void GlLabel::setBold(bool b) {
    m_bold = b;
    update();
}
void GlLabel::setTone(Tone t) {
    m_tone = t;
    update();
}
void GlLabel::setColor(const QColor& c) {
    m_color = c;
    m_tone = Tone::Custom;
    update();
}
void GlLabel::setAlignment(Qt::Alignment a) {
    m_align = a;
    update();
}
void GlLabel::setCaps(bool caps) {
    m_caps = caps;
    update();
}

qreal GlLabel::heightForWidth(qreal width) const {
    QFont f = GlUi::uiFont(m_pt, m_bold);
    return QFontMetricsF(f).boundingRect(QRectF(0, 0, std::max<qreal>(10, width), 100000),
                                         Qt::TextWordWrap | Qt::AlignLeft, m_text).height();
}

void GlRule::paint(QPainter& p, const Style& s) {
    const QRectF r = rect();
    QLinearGradient g(r.left(), r.center().y(), r.right(), r.center().y());
    QColor a = s.glassEdge, b = s.accent2;
    b.setAlphaF(0.35);
    g.setColorAt(0, b);
    g.setColorAt(0.5, a);
    g.setColorAt(1, b);
    p.fillRect(QRectF(r.left(), r.center().y() - 0.5, r.width(), 1.0), g);
}

GlProgressBar::GlProgressBar(GlCanvas* canvas) : GlItem(canvas) {}

bool GlProgressBar::advance(qreal dt) {
    return GlItem::advance(dt) || m_indeterminate;
}

void GlProgressBar::paint(QPainter& p, const Style& s) {
    const QRectF r = rect();
    const qreal rad = std::min(r.height() / 2, std::max(5.0, r.height() * 0.5 * s.radius + 2));
    p.save();
    p.setPen(QPen(s.glassEdge, 1.0));
    p.setBrush(s.dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 28));
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    if (m_indeterminate) {
        const qreal inner = r.width() - 2;
        const qreal segW = inner * 0.3;
        const qreal t = std::fmod(canvas()->clock() * 0.55, 1.0);
        const qreal x = r.left() + 1 + (inner + segW) * t - segW;
        p.save();
        QPainterPath clipPath;
        clipPath.addRoundedRect(r.adjusted(1, 1, -1, -1), rad, rad);
        p.setClipPath(clipPath);
        QLinearGradient g(x, 0, x + segW, 0);
        g.setColorAt(0, QColor(s.accent.red(), s.accent.green(), s.accent.blue(), 0));
        g.setColorAt(0.5, s.accent2);
        g.setColorAt(1, QColor(s.accent.red(), s.accent.green(), s.accent.blue(), 0));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRect(QRectF(x, r.top() + 1, segW, r.height() - 2));
        p.restore();
        p.restore();
        return;
    }
    const qreal f = std::clamp(qreal(m_value - m_min) / (m_max - m_min), 0.0, 1.0);
    if (f > 0) {
        QRectF fill(r.left() + 1, r.top() + 1, std::max<qreal>(rad, (r.width() - 2) * f), r.height() - 2);
        QLinearGradient g(r.topLeft(), r.topRight());
        g.setColorAt(0, s.accent);
        g.setColorAt(1, s.accent2);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(fill, rad, rad);
    }
    if (!m_text.isEmpty()) {
        p.setFont(GlUi::uiFont(9.5, true));
        p.setPen(s.text);
        p.drawText(r, Qt::AlignCenter, m_text);
    }
    p.restore();
}

void GlLabel::paint(QPainter& p, const Style& s) {
    if (!m_pixmap.isNull()) {
        const QSizeF sz = m_pixmap.deviceIndependentSize();
        p.drawPixmap(QPointF(rect().center().x() - sz.width() / 2, rect().center().y() - sz.height() / 2), m_pixmap);
        return;
    }
    if (m_text.isEmpty()) {
        return;
    }
    QFont f = GlUi::uiFont(m_pt, m_bold);
    if (m_caps) {
        f.setCapitalization(QFont::AllUppercase);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.6);
    }
    p.setFont(f);
    QColor c = m_tone == Tone::Dim ? s.textDim : m_tone == Tone::Accent ? s.accent2 : m_tone == Tone::Custom ? m_color : s.text;
    if (!isEnabled()) {
        c.setAlphaF(0.5);
    }
    p.setPen(c);
    if (m_wrap) {
        p.drawText(rect(), int(m_align) | Qt::TextWordWrap, m_text);
        return;
    }
    const QString shown = QFontMetricsF(f).elidedText(m_text, Qt::ElideMiddle, rect().width());
    p.drawText(rect(), int(m_align), shown);
}

// -------------------------------------------------------------- GlButton

GlButton::GlButton(GlCanvas* canvas, Kind kind) : GlItem(canvas), m_kind(kind) {}

void GlButton::setKind(Kind k) {
    m_kind = k;
    update();
}
void GlButton::setText(const QString& t) {
    if (t == m_text) {
        return;
    }
    m_text = t;
    update();
}
void GlButton::setIcon(const QIcon& icon) {
    m_icon = icon;
    update();
}
void GlButton::activate() {
    if (isEnabled()) {
        emit clicked();
    }
}

void GlButton::paint(QPainter& p, const Style& s) {
    const qreal h = hoverAmount();
    const qreal pr = pressAmount();
    const qreal dpr = p.device()->devicePixelRatioF();
    QRectF r = rect();
    if (pr > 0.01) {
        r = r.adjusted(1.5 * pr, 1.5 * pr, -1.5 * pr, -1.5 * pr);
    }
    const qreal rad = std::min(r.height() / 2.0, std::max(5.0, r.height() * 0.5 * s.radius + (1 - s.radius) * 2));
    const bool on = isEnabled();
    p.save();
    if (!on) {
        p.setOpacity(0.45);
    }

    auto drawIcon = [&](const QRectF& box, const QColor& color) {
        if (m_icon.isNull()) {
            return;
        }
        const QPixmap pm = m_tint ? GlUi::tintedGlyph(m_icon, box.size(), color, dpr)
                                  : m_icon.pixmap(box.size().toSize(), dpr);
        p.drawPixmap(box.topLeft(), pm);
    };

    switch (m_kind) {
    case Kind::Primary: {
        QPainterPath clip;
        clip.addRoundedRect(r, rad, rad);
        if (on) {
            glow(p, r, rad, s.accent2, 0.35 + 0.65 * h);
        }
        p.save();
        p.setClipPath(clip);
        QLinearGradient body(r.topLeft(), r.topRight());
        body.setColorAt(0, s.accent);
        body.setColorAt(1, GlUi::mix(s.accent, s.accent2, 0.85));
        p.fillRect(r, body);
        // Split segment (slanted seam) behind the icon.
        const qreal seg = m_icon.isNull() ? 0 : r.height();
        const qreal skew = r.height() * 0.2;
        if (seg > 0) {
            QPolygonF poly;
            poly << r.topLeft() << QPointF(r.left() + seg + skew, r.top()) << QPointF(r.left() + seg - skew, r.bottom())
                 << r.bottomLeft();
            QLinearGradient sg(r.topLeft(), r.bottomLeft());
            sg.setColorAt(0, s.accent2);
            sg.setColorAt(1, s.accent2.darker(135));
            p.setPen(Qt::NoPen);
            p.setBrush(sg);
            p.drawPolygon(poly);
        }
        // Hover shine.
        QLinearGradient shine(r.topLeft(), r.bottomLeft());
        shine.setColorAt(0, QColor(255, 255, 255, int(70 + 60 * h)));
        shine.setColorAt(0.55, QColor(255, 255, 255, 0));
        p.setBrush(shine);
        p.drawRect(r);
        p.restore();
        const QColor segText = GlUi::bestText(s.accent2);
        const QColor bodyText = GlUi::bestText(GlUi::mix(s.accent, s.accent2, 0.4));
        if (seg > 0) {
            const qreal ic = std::min<qreal>(24, r.height() * 0.46);
            drawIcon(QRectF(r.left() + (seg - ic) / 2 - skew * 0.15, r.center().y() - ic / 2, ic, ic), segText);
        }
        p.setFont(GlUi::uiFont(11.5, true));
        p.setPen(bodyText);
        QRectF tr(r.left() + seg + skew * 0.4, r.top(), r.width() - seg - skew * 0.4, r.height());
        p.drawText(tr, Qt::AlignCenter, m_text);
        break;
    }
    case Kind::Secondary:
    case Kind::Icon: {
        const bool iconOnly = m_kind == Kind::Icon;
        const qreal radius = iconOnly ? std::min(r.width(), r.height()) * (0.28 + 0.22 * s.radius) : rad;
        QColor fill = s.glass;
        fill.setAlphaF(std::min(1.0, fill.alphaF() + 0.12 * h));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(r, radius, radius);
        if (h > 0.01) {
            QColor tintC = s.accent;
            tintC.setAlphaF(0.22 * h);
            p.setBrush(tintC);
            p.drawRoundedRect(r, radius, radius);
        }
        QColor edge = GlUi::mix(s.glassEdge, s.accent2, h);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(edge, 1.1));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        glow(p, r, radius, s.accent2, 0.55 * h);
        const QColor fg = GlUi::mix(s.text, s.accent2, 0.55 * h);
        const qreal ic = iconOnly ? std::min<qreal>(22, r.height() * 0.55) : 18;
        if (iconOnly) {
            drawIcon(QRectF(r.center().x() - ic / 2, r.center().y() - ic / 2, ic, ic), fg);
        } else {
            p.setFont(GlUi::uiFont(10, true));
            const qreal tw = QFontMetricsF(p.font()).horizontalAdvance(m_text);
            const qreal gap = m_icon.isNull() ? 0 : 8;
            const qreal total = (m_icon.isNull() ? 0 : ic) + gap + tw;
            qreal x = r.center().x() - total / 2;
            if (!m_icon.isNull()) {
                drawIcon(QRectF(x, r.center().y() - ic / 2, ic, ic), fg);
                x += ic + gap;
            }
            p.setPen(fg);
            p.drawText(QRectF(x, r.top(), tw + 2, r.height()), Qt::AlignVCenter | Qt::AlignLeft, m_text);
        }
        break;
    }
    case Kind::Tab: {
        const qreal radius = std::max(6.0, 14 * s.radius);
        if (m_selected) {
            QLinearGradient g(r.topLeft(), r.topRight());
            g.setColorAt(0, s.accent);
            g.setColorAt(1, GlUi::mix(s.accent, s.accent2, 0.8));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(r, radius, radius);
        } else if (h > 0.01) {
            QColor c = s.accent;
            c.setAlphaF(0.18 * h);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRoundedRect(r, radius, radius);
        }
        const QColor fg = m_selected ? GlUi::bestText(GlUi::mix(s.accent, s.accent2, 0.4))
                                     : GlUi::mix(s.textDim, s.text, 0.4 + 0.6 * h);
        p.setFont(GlUi::uiFont(10.5, m_selected));
        p.setPen(fg);
        p.drawText(r.adjusted(m_alignLeft ? 16 : 0, 0, -8, 0), (m_alignLeft ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignVCenter,
                   m_text);
        break;
    }
    case Kind::Minimize:
    case Kind::Maximize:
    case Kind::Restore:
    case Kind::Close: {
        const bool close = m_kind == Kind::Close;
        const qreal radius = 8 * s.radius + 2;
        QColor bg = close ? s.bad : s.accent;
        bg.setAlphaF((close ? 0.9 : 0.28) * h);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(r, radius, radius);
        const QColor fg = (close && h > 0.5) ? QColor(Qt::white) : s.text;
        p.setPen(QPen(fg, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        const QPointF c = r.center();
        if (m_kind == Kind::Minimize) {
            p.drawLine(QPointF(c.x() - 5, c.y() + 2), QPointF(c.x() + 5, c.y() + 2));
        } else if (m_kind == Kind::Maximize) {
            p.drawRoundedRect(QRectF(c.x() - 5, c.y() - 5, 10, 10), 2, 2);
        } else if (m_kind == Kind::Restore) {
            p.drawRoundedRect(QRectF(c.x() - 5, c.y() - 2, 8, 8), 2, 2);
            p.drawLine(QPointF(c.x() - 2, c.y() - 5), QPointF(c.x() + 5, c.y() - 5));
            p.drawLine(QPointF(c.x() + 5, c.y() - 5), QPointF(c.x() + 5, c.y() + 2));
        } else {
            p.drawLine(QPointF(c.x() - 5, c.y() - 5), QPointF(c.x() + 5, c.y() + 5));
            p.drawLine(QPointF(c.x() - 5, c.y() + 5), QPointF(c.x() + 5, c.y() - 5));
        }
        break;
    }
    }
    p.restore();

    if (hasFocus()) {
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.accent2, 2.0));
        p.drawRoundedRect(rect().adjusted(-3, -3, 3, 3), rad + 3, rad + 3);
        p.restore();
    }
}

// ------------------------------------------------------------ GlPreview

GlPreview::GlPreview(GlCanvas* canvas) : GlItem(canvas), m_icon(new QSvgRenderer(QStringLiteral(":/video/video-file.svg"))) {}

void GlPreview::setImage(const QImage& img) {
    m_image = img;
    update();
}
void GlPreview::clearImage() {
    m_image = QImage();
    update();
}
void GlPreview::setLoadingText(const QString& t) {
    m_loading = t;
    update();
}
void GlPreview::setShowDropZone(bool show) {
    if (m_showDrop == show) {
        return;
    }
    m_showDrop = show;
    update();
}
void GlPreview::setDropState(DropState s) {
    if (m_state == s) {
        return;
    }
    m_state = s;
    if (s == DropState::Success) {
        m_success = 0.0;
        m_successTarget = 1.0;
    }
    update();
}
void GlPreview::setAspect(double aspect) {
    if (qFuzzyCompare(aspect + 1.0, m_aspect + 1.0)) {
        return;
    }
    m_aspect = aspect;
    update();
}

QRectF GlPreview::contentRect() const {
    QRectF r = rect();
    if (!m_showDrop && m_aspect > 0.0 && r.width() > 0 && r.height() > 0) {
        qreal w = r.width();
        qreal hgt = w / m_aspect;
        if (hgt > r.height()) {
            hgt = r.height();
            w = hgt * m_aspect;
        }
        r = QRectF(r.center().x() - w / 2, r.center().y() - hgt / 2, w, hgt);
    }
    return r;
}

QRectF GlPreview::dropActionRect() const {
    const QRectF r = rect();
    const qreal w = 190, h = 38;
    return QRectF(r.center().x() - w / 2, r.bottom() - h - 24, w, h);
}

bool GlPreview::advance(qreal dt) {
    bool busy = GlItem::advance(dt);
    const qreal ta = m_state == DropState::DragOver ? 1.0 : 0.0;
    const qreal ti = m_state == DropState::Invalid ? 1.0 : 0.0;
    const qreal k = std::min(1.0, dt * 10.0);
    m_activity += (ta - m_activity) * k;
    m_invalid += (ti - m_invalid) * k;
    if (std::abs(ta - m_activity) < 0.01) m_activity = ta; else busy = true;
    if (std::abs(ti - m_invalid) < 0.01) m_invalid = ti; else busy = true;
    if (m_state == DropState::Success && m_successTarget > 0) {
        m_success = std::min(1.0, m_success + dt / 0.65);
        busy = true;
        if (m_success >= 1.0) {
            m_successTarget = 0;
        }
    } else if (m_state != DropState::Success) {
        m_success = 0;
    }
    return busy;
}

void GlPreview::paint(QPainter& p, const Style& s) {
    const QRectF box = contentRect();
    const qreal rad = 22 * s.radius;
    const qreal dpr = p.device()->devicePixelRatioF();

    // Dark media well regardless of theme (video reads best on near-black),
    // framed by the neon rim.
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(6, 6, 12, 215));
    p.drawRoundedRect(box, rad, rad);
    p.restore();

    if (!m_showDrop) {
        if (!m_image.isNull()) {
            QPainterPath clip;
            clip.addRoundedRect(box.adjusted(1, 1, -1, -1), rad - 1, rad - 1);
            p.save();
            p.setClipPath(clip);
            p.drawImage(box.adjusted(1, 1, -1, -1), m_image);
            p.restore();
        } else if (!m_loading.isEmpty()) {
            p.setFont(GlUi::uiFont(11));
            p.setPen(QColor(150, 150, 165));
            p.drawText(box, Qt::AlignCenter, m_loading);
        }
        QLinearGradient rim(box.topLeft(), box.bottomRight());
        QColor a = s.accent, b = s.accent2;
        a.setAlphaF(0.55);
        b.setAlphaF(0.35);
        rim.setColorAt(0, a);
        rim.setColorAt(1, b);
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QBrush(rim), 1.4));
        p.drawRoundedRect(box.adjusted(0.7, 0.7, -0.7, -0.7), rad, rad);
        p.restore();
        glow(p, box, rad, s.accent, 0.25);
        return;
    }

    // ---- Animated drop zone ----
    const qreal t = canvas()->clock();
    const QColor idle(160, 160, 185), over = s.accent2, bad = s.bad;
    QColor mixed = GlUi::mix(idle, over, m_activity);
    mixed = GlUi::mix(mixed, bad, m_invalid);

    QPen dash(m_activity > 0.01 || m_invalid > 0.01 ? mixed : GlUi::mix(s.accent, QColor(120, 120, 140), 0.5));
    dash.setWidthF(1.8);
    dash.setDashPattern({6, 5});
    dash.setDashOffset(-t * 14.0);
    p.save();
    p.setBrush(Qt::NoBrush);
    p.setPen(dash);
    p.drawRoundedRect(box.adjusted(14, 14, -14, -14), rad * 0.7, rad * 0.7);
    p.restore();

    const QRectF actionBox = dropActionRect();
    const QPointF center(box.center().x(), box.top() + (actionBox.top() - box.top()) * 0.40);
    const qreal bob = std::sin(t * 2.2) * 4.0 * (1.0 - 0.4 * m_activity);

    // Halo.
    {
        QRadialGradient g(center, 70 + 10 * m_activity);
        QColor c = GlUi::mix(s.accent, mixed, std::max(m_activity, m_invalid));
        c.setAlphaF(0.20 + 0.25 * m_activity);
        g.setColorAt(0, c);
        c.setAlphaF(0);
        g.setColorAt(1, c);
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(center, 80, 80);
        p.restore();
    }

    // Icon (tinted SVG).
    {
        const QSizeF isz(52, 52);
        QPixmap pm(isz.toSize() * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter ip(&pm);
        ip.setRenderHint(QPainter::Antialiasing);
        m_icon->render(&ip, QRectF(QPointF(0, 0), isz));
        ip.setCompositionMode(QPainter::CompositionMode_SourceIn);
        ip.fillRect(QRectF(QPointF(0, 0), isz), mixed.lighter(115));
        ip.end();
        p.drawPixmap(QPointF(center.x() - isz.width() / 2, center.y() - isz.height() / 2 + bob), pm);
    }

    // Invalid "x" badge.
    if (m_invalid > 0.01) {
        p.save();
        p.setOpacity(m_invalid);
        p.setPen(QPen(s.bad, 2.6, Qt::SolidLine, Qt::RoundCap));
        const QPointF b(center.x() + 24, center.y() - 20 + bob);
        p.drawLine(b + QPointF(-6, -6), b + QPointF(6, 6));
        p.drawLine(b + QPointF(-6, 6), b + QPointF(6, -6));
        p.restore();
    }

    // Success burst.
    if (m_success > 0.0 && m_state == DropState::Success) {
        p.save();
        const qreal fade = 1.0 - m_success;
        for (int i = 0; i < 8; ++i) {
            const qreal ang = (M_PI * 2.0 / 8.0) * i - M_PI / 2.0;
            const QPointF pt = center + QPointF(std::cos(ang), std::sin(ang)) * (24 + 46 * m_success);
            QColor c = (i % 2) ? s.accent : s.accent2;
            c.setAlphaF(0.9 * fade);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(pt, 3.0, 3.0);
        }
        if (m_success > 0.5) {
            p.setOpacity((m_success - 0.5) / 0.5);
            p.setPen(QPen(s.ok, 3.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath check;
            check.moveTo(center + QPointF(-11, 0));
            check.lineTo(center + QPointF(-3, 9));
            check.lineTo(center + QPointF(13, -11));
            p.drawPath(check);
        }
        p.restore();
    }

    // Texts.
    const QRectF textBox(box.left() + 20, center.y() + 46, box.width() - 40, 24);
    p.setFont(GlUi::uiFont(13, true));
    p.setPen(QColor(235, 235, 245));
    p.drawText(textBox, Qt::AlignHCenter | Qt::AlignVCenter, QObject::tr("Drag & Drop Media"));
    p.setFont(GlUi::uiFont(9.5));
    p.setPen(QColor(150, 150, 168));
    p.drawText(textBox.translated(0, 24), Qt::AlignHCenter | Qt::AlignVCenter,
               QObject::tr("Local video, GIF or image files"));
    p.setFont(GlUi::uiFont(8.5, true));
    QFont of = p.font();
    of.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    p.setFont(of);
    p.setPen(QColor(110, 110, 130));
    p.drawText(QRectF(box.left(), actionBox.top() - 26, box.width(), 18), Qt::AlignCenter, QObject::tr("OR"));

    // Neon rim.
    QLinearGradient rim(box.topLeft(), box.bottomRight());
    QColor a = s.accent, b = s.accent2;
    a.setAlphaF(0.40);
    b.setAlphaF(0.22);
    rim.setColorAt(0, a);
    rim.setColorAt(1, b);
    p.save();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(rim), 1.2));
    p.drawRoundedRect(box.adjusted(0.6, 0.6, -0.6, -0.6), rad, rad);
    p.restore();
    glow(p, box, rad, mixed, 0.2 + 0.5 * m_activity);
}

// -------------------------------------------------------------- GlCanvas

namespace {
const char* kVert = R"(
attribute vec2 aPos;
varying vec2 vUv;
void main() { vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }
)";
const char* kFrag = R"(
#ifdef GL_ES
precision mediump float;
#endif
varying vec2 vUv;
uniform vec2 uRes;
uniform float uTime;
uniform float uDark;
uniform vec3 uBase;
uniform vec3 uTint;
uniform vec3 uA;
uniform vec3 uB;
void main() {
    vec2 uv = vUv;
    float asp = uRes.x / max(uRes.y, 1.0);
    vec2 p = vec2(uv.x * asp, uv.y);
    vec3 col = mix(uBase, uTint, smoothstep(0.0, 1.0, uv.y * 0.9 + uv.x * 0.25));
    float s = mix(0.30, 0.20, uDark);
    // Three slow drifting neon blobs (accent / accent2 / accent).
    vec2 c0 = vec2(asp * (0.22 + 0.10 * sin(uTime * 0.21)), 0.80 + 0.10 * cos(uTime * 0.17));
    vec2 c1 = vec2(asp * (0.80 + 0.08 * cos(uTime * 0.19)), 0.30 + 0.12 * sin(uTime * 0.23));
    vec2 c2 = vec2(asp * (0.55 + 0.12 * sin(uTime * 0.13 + 2.0)), 0.55 + 0.10 * cos(uTime * 0.11));
    col += uA * exp(-dot(p - c0, p - c0) * 5.0) * s;
    col += uB * exp(-dot(p - c1, p - c1) * 6.0) * s * 1.1;
    col += uA * exp(-dot(p - c2, p - c2) * 9.0) * s * 0.5;
    // Fine diagonal scan texture for a "tech" feel.
    float lines = 0.5 + 0.5 * sin((uv.x * asp + uv.y) * 220.0);
    col += (lines - 0.5) * 0.012;
    // Vignette.
    vec2 q = uv - 0.5;
    col *= 1.0 - dot(q, q) * 0.45 * uDark;
    gl_FragColor = vec4(col, 1.0);
}
)";
QVector3D vec(const QColor& c) {
    return QVector3D(c.redF(), c.greenF(), c.blueF());
}
} // namespace

GlCanvas::GlCanvas(QWidget* parent) : QOpenGLWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &GlCanvas::tick);
    m_frameClock.start();
}

GlCanvas::~GlCanvas() {
    makeCurrent();
    m_program.reset();
    m_quad.reset();
    delete m_vao;
    doneCurrent();
}

void GlCanvas::registerItem(GlItem* item) {
    m_items.append(item);
}
void GlCanvas::unregisterItem(GlItem* item) {
    m_items.removeAll(item);
    if (m_hover == item) m_hover = nullptr;
    if (m_press == item) m_press = nullptr;
    if (m_focus == item) m_focus = nullptr;
}

void GlCanvas::setLayoutCallback(std::function<void(const QSizeF&)> fn) {
    m_layout = std::move(fn);
}
void GlCanvas::relayout() {
    if (m_layout) {
        m_layout(size());
    }
    requestAnimation();
}

void GlCanvas::setBackgroundAnimated(bool animated) {
    m_bgAnimated = animated;
    requestAnimation();
}

void GlCanvas::requestAnimation() {
    m_hotFrames = 8;
    if (!m_timer->isActive() && isVisible()) {
        m_lastNs = m_frameClock.nsecsElapsed();
        m_timer->start(16);
    }
    QWidget::update();
}

void GlCanvas::tick() {
    if (!isVisible() || window()->isMinimized()) {
        m_timer->setInterval(250);
        m_lastNs = m_frameClock.nsecsElapsed();
        return;
    }
    const qint64 ns = m_frameClock.nsecsElapsed();
    const qreal dt = std::min<qreal>(0.1, (ns - m_lastNs) / 1e9);
    m_lastNs = ns;
    m_clock += dt;
    bool busy = false;
    for (GlItem* it : m_items) {
        if (it->isVisible()) {
            busy |= it->advance(dt);
        }
    }
    if (busy || m_hotFrames > 0) {
        m_hotFrames = std::max(0, m_hotFrames - 1);
        m_timer->setInterval(16);
    } else if (m_bgAnimated) {
        m_timer->setInterval(33);
    } else {
        m_timer->stop();
    }
    QWidget::update();
}

void GlCanvas::showEvent(QShowEvent* e) {
    QOpenGLWidget::showEvent(e);
    m_shownNs = m_frameClock.nsecsElapsed();
    requestAnimation();
}
void GlCanvas::hideEvent(QHideEvent* e) {
    QOpenGLWidget::hideEvent(e);
    m_timer->stop(); // a tray-hidden Motiva costs nothing
}

void GlCanvas::initializeGL() {
    m_program = std::make_unique<QOpenGLShaderProgram>();
    bool ok = m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert) &&
              m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
    if (ok) {
        m_program->bindAttributeLocation("aPos", 0);
        ok = m_program->link();
    }
    if (!ok) {
        qWarning() << "[GlUi] Background shader unavailable, using gradient fallback:" << m_program->log();
        m_program.reset();
        m_glFailed = true;
        return;
    }
    m_vao = new QOpenGLVertexArrayObject(this);
    m_vao->create();
    m_quad = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
    m_quad->create();
    m_quad->bind();
    static const float verts[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    m_quad->allocate(verts, sizeof(verts));
    m_quad->release();
}

void GlCanvas::resizeGL(int, int) {
    relayout();
}

void GlCanvas::drawBackground(const Style& s) {
    QOpenGLFunctions* f = context()->functions();
    f->glDisable(GL_BLEND);
    f->glDisable(GL_DEPTH_TEST);
    m_program->bind();
    const qreal dpr = devicePixelRatioF();
    m_program->setUniformValue("uRes", QVector2D(width() * dpr, height() * dpr));
    m_program->setUniformValue("uTime", float(m_bgAnimated ? m_clock : 0.0));
    m_program->setUniformValue("uDark", s.dark ? 1.0f : 0.0f);
    m_program->setUniformValue("uBase", vec(s.bgBase));
    m_program->setUniformValue("uTint", vec(s.bgTint));
    m_program->setUniformValue("uA", vec(s.accent));
    m_program->setUniformValue("uB", vec(s.accent2));
    m_vao->bind();
    m_quad->bind();
    m_program->enableAttributeArray(0);
    m_program->setAttributeBuffer(0, GL_FLOAT, 0, 2, 0);
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_program->disableAttributeArray(0);
    m_quad->release();
    m_vao->release();
    m_program->release();
}

void GlCanvas::paintGL() {
    const Style s = Style::current();
    const bool shader = m_program && m_quad;
    if (shader) {
        drawBackground(s);
    }
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    if (!shader) {
        QLinearGradient g(0, 0, width(), height());
        g.setColorAt(0, s.bgBase);
        g.setColorAt(1, s.bgTint);
        p.fillRect(rect(), g);
    }
    for (GlItem* it : m_items) {
        if (it->isVisible()) {
            if (!it->clip().isNull()) {
                p.save();
                p.setClipRect(it->clip());
                it->paint(p, s);
                p.restore();
            } else {
                it->paint(p, s);
            }
        }
    }
    if (m_popup) {
        m_popup->paintOverlay(p, s);
    }
    // Window edge: neon hairline (the window itself is borderless).
    QLinearGradient rim(0, 0, width(), height());
    QColor a = s.accent, b = s.accent2;
    a.setAlphaF(0.55);
    b.setAlphaF(0.30);
    rim.setColorAt(0, a);
    rim.setColorAt(1, b);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(rim), 1.0));
    if (!window()->isMaximized()) {
        p.drawRect(QRectF(0.5, 0.5, width() - 1.0, height() - 1.0));
    }
}

QImage GlCanvas::snapshot() {
    return grabFramebuffer();
}

GlItem* GlCanvas::itemAt(const QPointF& pt) const {
    for (int i = m_items.size() - 1; i >= 0; --i) {
        GlItem* it = m_items[i];
        if (it->isHit(pt)) {
            return it;
        }
    }
    return nullptr;
}

bool GlCanvas::interactiveAt(const QPointF& pt) const {
    for (int i = m_items.size() - 1; i >= 0; --i) {
        GlItem* it = m_items[i];
        if (it->isInteractive() && it->isHit(pt)) {
            return true;
        }
    }
    return false;
}

void GlCanvas::updateHover(const QPointF& pt) {
    GlItem* hit = nullptr;
    for (int i = m_items.size() - 1; i >= 0; --i) {
        GlItem* it = m_items[i];
        if (it->isInteractive() && it->isHit(pt)) {
            hit = it;
            break;
        }
    }
    if (hit != m_hover) {
        if (m_hover) {
            m_hover->m_hovered = false;
            m_hover->hoverMoved(QPointF(-1e9, -1e9));
        }
        m_hover = hit;
        if (m_hover) m_hover->m_hovered = true;
        setCursor(hit && hit->isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        requestAnimation();
    }
    if (m_hover) {
        m_hover->hoverMoved(pt);
    }
    QString tip;
    for (int i = m_items.size() - 1; i >= 0 && tip.isEmpty(); --i) {
        GlItem* it = m_items[i];
        if (!it->toolTip().isEmpty() && it->isHit(pt)) {
            tip = it->toolTip();
        }
    }
    if (tip != toolTip()) {
        setToolTip(tip);
    }
}

void GlCanvas::mouseMoveEvent(QMouseEvent* e) {
    if (m_popup) {
        m_popup->popupHover(e->position());
        return;
    }
    if (m_press && (e->buttons() & Qt::LeftButton)) {
        m_press->mouseDragTo(e->position());
        requestAnimation();
        return;
    }
    updateHover(e->position());
}

void GlCanvas::setPopup(GlItem* item) {
    m_popup = item;
    requestAnimation();
}

void GlCanvas::wheelEvent(QWheelEvent* e) {
    if (m_popup) {
        return;
    }
    for (int i = m_items.size() - 1; i >= 0; --i) {
        GlItem* it = m_items[i];
        if (it->isInteractive() && it->isHit(e->position()) && it->wheelAt(e->angleDelta().y(), e->position())) {
            e->accept();
            requestAnimation();
            return;
        }
    }
    if (m_wheel) {
        m_wheel(e->angleDelta().y(), e->position());
        e->accept();
    } else {
        QOpenGLWidget::wheelEvent(e);
    }
}

void GlCanvas::changeEvent(QEvent* e) {
    QOpenGLWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange) {
        requestAnimation(); // theme switched: repaint from the new palette
    }
}

void GlCanvas::mousePressEvent(QMouseEvent* e) {
    setFocus(Qt::MouseFocusReason);
    if (m_popup) {
        GlItem* pop = m_popup;
        if (!pop->popupPress(e->position())) {
            m_popup = nullptr; // click outside closes the list and is consumed
        }
        requestAnimation();
        return;
    }
    updateHover(e->position());
    if (e->button() == Qt::LeftButton && m_hover && m_hover->isEnabled()) {
        m_press = m_hover;
        m_press->m_pressed = true;
        m_press->mousePressAt(e->position());
        m_keyboardFocusVisible = false;
        setFocusedItem(m_press);
        requestAnimation();
    }
}

void GlCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !m_press) {
        return;
    }
    QPointer<GlItem> item = m_press;
    item->m_pressed = false;
    m_press = nullptr;
    updateHover(e->position());
    requestAnimation();
    if (item) {
        item->mouseReleased(e->position());
    }
    if (item && item == m_hover && item->isEnabled()) {
        item->activate();
    }
}

void GlCanvas::leaveEvent(QEvent* e) {
    QOpenGLWidget::leaveEvent(e);
    if (m_hover) {
        m_hover->m_hovered = false;
        m_hover->hoverMoved(QPointF(-1e9, -1e9));
        m_hover = nullptr;
        requestAnimation();
    }
}

void GlCanvas::setFocusedItem(GlItem* item) {
    if (item == m_focus) {
        requestAnimation();
        return;
    }
    GlItem* old = m_focus;
    m_focus = item;
    if (old) old->focusChanged(false);
    if (item) item->focusChanged(true);
    requestAnimation();
}

void GlCanvas::cycleFocus(bool forward) {
    QList<GlItem*> order;
    for (GlItem* it : m_items) {
        if (it->isVisible() && it->isEnabled() && it->isInteractive() && !it->rect().isEmpty()) {
            order << it;
        }
    }
    if (order.isEmpty()) {
        return;
    }
    int idx = order.indexOf(m_focus);
    idx = idx < 0 ? (forward ? 0 : order.size() - 1) : (idx + (forward ? 1 : -1) + order.size()) % order.size();
    setFocusedItem(order[idx]);
    m_keyboardFocusVisible = true;
    requestAnimation();
}

bool GlCanvas::focusNextPrevChild(bool next) {
    cycleFocus(next);
    return true;
}

void GlCanvas::keyPressEvent(QKeyEvent* e) {
    if (m_popup) {
        if (m_popup->popupKey(e)) {
            return;
        }
    } else if (m_focus && m_focus->isEnabled() && m_focus->isVisible() && m_focus->keyPressed(e)) {
        m_keyboardFocusVisible = true;
        requestAnimation();
        return;
    }
    if (e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (m_focus && m_focus->isEnabled() && m_focus->isVisible()) {
            m_keyboardFocusVisible = true;
            QPointer<GlItem> it = m_focus;
            it->activate();
            return;
        }
    }
    QOpenGLWidget::keyPressEvent(e);
}

void GlCanvas::focusInEvent(QFocusEvent* e) {
    QOpenGLWidget::focusInEvent(e);
    // A window being shown also focuses its first child with Tab reason;
    // only a real Tab press (well after show) should reveal the focus ring.
    const bool justShown = (m_frameClock.nsecsElapsed() - m_shownNs) < 400000000;
    if (!justShown && (e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason)) {
        cycleFocus(e->reason() == Qt::TabFocusReason);
    }
    requestAnimation();
}
void GlCanvas::focusOutEvent(QFocusEvent* e) {
    QOpenGLWidget::focusOutEvent(e);
    requestAnimation();
}


// ------------------------------------------------------------- GlToggle

GlToggle::GlToggle(GlCanvas* canvas) : GlItem(canvas) {}

void GlToggle::setChecked(bool on) {
    if (on == m_checked) {
        return;
    }
    m_checked = on;
    update();
    emit toggled(on);
}

bool GlToggle::advance(qreal dt) {
    bool busy = GlItem::advance(dt);
    const qreal target = m_checked ? 1.0 : 0.0;
    m_knob += (target - m_knob) * std::min(1.0, dt * 16.0);
    if (std::abs(target - m_knob) < 0.01) m_knob = target; else busy = true;
    return busy;
}

bool GlToggle::keyPressed(QKeyEvent* e) {
    if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) {
        setChecked(e->key() == Qt::Key_Right);
        return true;
    }
    return false;
}

void GlToggle::paint(QPainter& p, const Style& s) {
    const QRectF r = rect();
    const qreal h = hoverAmount();
    p.save();
    if (!isEnabled()) {
        p.setOpacity(0.5);
    }
    if (h > 0.01) {
        QColor c = s.accent;
        c.setAlphaF(0.08 * h);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(r, 10 * s.radius + 2, 10 * s.radius + 2);
    }
    p.setFont(GlUi::uiFont(10.5));
    p.setPen(s.text);
    p.drawText(QRectF(r.left() + 14 + m_indent, r.top(), r.width() - 90 - m_indent, r.height()),
               Qt::AlignVCenter | Qt::AlignLeft, QFontMetricsF(p.font()).elidedText(m_text, Qt::ElideRight, r.width() - 100 - m_indent));
    // Switch: gradient track when on, glass when off, sliding knob.
    const QRectF track(r.right() - 14 - 46, r.center().y() - 12, 46, 24);
    const qreal tr = track.height() / 2 * (0.55 + 0.45 * s.radius);
    if (m_knob > 0.01) {
        QLinearGradient g(track.topLeft(), track.topRight());
        g.setColorAt(0, s.accent);
        g.setColorAt(1, s.accent2);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.setOpacity(p.opacity() * (0.35 + 0.65 * m_knob));
        p.drawRoundedRect(track, tr, tr);
        p.setOpacity(isEnabled() ? 1.0 : 0.5);
    }
    QColor off = s.glass;
    off.setAlphaF(std::min(1.0, off.alphaF() + 0.10));
    if (m_knob < 0.99) {
        p.setOpacity(p.opacity() * (1.0 - m_knob));
        p.setPen(QPen(s.glassEdge, 1.2));
        p.setBrush(off);
        p.drawRoundedRect(track.adjusted(0.6, 0.6, -0.6, -0.6), tr, tr);
        p.setOpacity(isEnabled() ? 1.0 : 0.5);
    }
    glow(p, track, tr, s.accent2, 0.5 * m_knob);
    const qreal kd = track.height() - 8;
    const QPointF kc(track.left() + 4 + kd / 2 + (track.width() - 8 - kd) * m_knob, track.center().y());
    p.setPen(Qt::NoPen);
    p.setBrush(GlUi::mix(QColor(200, 200, 215), QColor(Qt::white), m_knob));
    p.drawEllipse(kc, kd / 2, kd / 2);
    p.restore();
    if (hasFocus()) {
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.accent2, 2.0));
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 10 * s.radius + 2, 10 * s.radius + 2);
        p.restore();
    }
}

// ------------------------------------------------------------- GlSlider

GlSlider::GlSlider(GlCanvas* canvas) : GlItem(canvas) {}

void GlSlider::setRange(int lo, int hi) {
    m_min = lo;
    m_max = std::max(lo, hi);
    m_value = std::clamp(m_value, m_min, m_max);
    update();
}

void GlSlider::setValue(int v) {
    v = std::clamp(v, m_min, m_max);
    if (v == m_value) {
        return;
    }
    m_value = v;
    update();
    emit valueChanged(v);
}

QRectF GlSlider::trackRect() const {
    const QRectF r = rect();
    const qreal left = r.left() + std::min<qreal>(190, r.width() * 0.34);
    return QRectF(left, r.center().y() - 3, std::max<qreal>(40, r.right() - 64 - left), 6);
}

void GlSlider::setFromX(qreal x) {
    const QRectF t = trackRect();
    const qreal f = std::clamp((x - t.left()) / t.width(), 0.0, 1.0);
    setValue(m_min + qRound(f * (m_max - m_min)));
}

bool GlSlider::keyPressed(QKeyEvent* e) {
    const int step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
    if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Down) {
        setValue(m_value - step);
        return true;
    }
    if (e->key() == Qt::Key_Right || e->key() == Qt::Key_Up) {
        setValue(m_value + step);
        return true;
    }
    return false;
}

void GlSlider::paint(QPainter& p, const Style& s) {
    const QRectF r = rect();
    p.save();
    if (!isEnabled()) {
        p.setOpacity(0.5);
    }
    p.setFont(GlUi::uiFont(10.5));
    p.setPen(s.text);
    p.drawText(QRectF(r.left() + 14, r.top(), 170, r.height()), Qt::AlignVCenter | Qt::AlignLeft, m_text);
    const QRectF t = trackRect();
    p.setPen(Qt::NoPen);
    p.setBrush(s.glass.alpha() < 100 ? QColor(255, 255, 255, 34) : QColor(0, 0, 0, 40));
    p.drawRoundedRect(t, 3, 3);
    const qreal f = m_max > m_min ? qreal(m_value - m_min) / (m_max - m_min) : 0;
    QRectF fill(t.left(), t.top(), t.width() * f, t.height());
    QLinearGradient g(t.topLeft(), t.topRight());
    g.setColorAt(0, s.accent);
    g.setColorAt(1, s.accent2);
    p.setBrush(g);
    p.drawRoundedRect(fill, 3, 3);
    const QPointF kc(t.left() + t.width() * f, t.center().y());
    const qreal kr = 8 + 2 * hoverAmount();
    glow(p, QRectF(kc.x() - kr, kc.y() - kr, kr * 2, kr * 2), kr, s.accent2, 0.5 + 0.5 * hoverAmount());
    p.setBrush(QColor(Qt::white));
    p.drawEllipse(kc, kr, kr);
    p.setPen(s.textDim);
    p.setFont(GlUi::uiFont(10, true));
    p.drawText(QRectF(r.right() - 56, r.top(), 44, r.height()), Qt::AlignVCenter | Qt::AlignRight, QString::number(m_value));
    p.restore();
    if (hasFocus()) {
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.accent2, 2.0));
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 8, 8);
        p.restore();
    }
}

// ----------------------------------------------------------- GlDropdown

GlDropdown::GlDropdown(GlCanvas* canvas) : GlItem(canvas) {}

void GlDropdown::clear() {
    m_items.clear();
    m_data.clear();
    m_index = -1;
    update();
}

void GlDropdown::setCurrentIndex(int i) {
    i = (i >= 0 && i < m_items.size()) ? i : (m_items.isEmpty() ? -1 : std::clamp(i, 0, int(m_items.size()) - 1));
    if (i == m_index) {
        return;
    }
    m_index = i;
    update();
    emit currentIndexChanged(i);
}

QRectF GlDropdown::fieldRect() const {
    const QRectF r = rect();
    if (m_compact) {
        return QRectF(r.left(), r.center().y() - 17, r.width(), 34);
    }
    const qreal w = std::min<qreal>(280, r.width() * 0.58);
    return QRectF(r.right() - 14 - w, r.center().y() - 17, w, 34);
}

QRectF GlDropdown::popupRect() const {
    const QRectF f = fieldRect();
    const qreal h = m_items.size() * 34 + 10;
    qreal top = f.bottom() + 4;
    const qreal limit = canvas()->height() - 8;
    if (top + h > limit) {
        top = std::max<qreal>(8, f.top() - 4 - h); // open upward when there is no room below
    }
    return QRectF(f.left(), top, f.width(), h);
}

int GlDropdown::indexAt(const QPointF& pt) const {
    const QRectF pr = popupRect();
    if (!pr.contains(pt)) {
        return -1;
    }
    const int i = int((pt.y() - pr.top() - 5) / 34);
    return (i >= 0 && i < m_items.size()) ? i : -1;
}

void GlDropdown::closePopup() {
    m_open = false;
    if (canvas()->popup() == this) {
        canvas()->setPopup(nullptr);
    }
}

void GlDropdown::activate() {
    if (m_open) {
        closePopup();
        return;
    }
    if (m_items.isEmpty()) {
        return;
    }
    m_open = true;
    m_highlight = m_index;
    canvas()->setPopup(this);
}

bool GlDropdown::popupPress(const QPointF& pt) {
    const int i = indexAt(pt);
    if (i >= 0) {
        closePopup();
        setCurrentIndex(i);
        return true;
    }
    closePopup();
    return fieldRect().contains(pt); // clicking the field again just closes
}

void GlDropdown::popupHover(const QPointF& pt) {
    const int i = indexAt(pt);
    if (i != m_highlight && i >= 0) {
        m_highlight = i;
        update();
    }
}

bool GlDropdown::popupKey(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_Escape:
        closePopup();
        return true;
    case Qt::Key_Up:
        m_highlight = std::max(0, m_highlight - 1);
        update();
        return true;
    case Qt::Key_Down:
        m_highlight = std::min(int(m_items.size()) - 1, m_highlight + 1);
        update();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space: {
        const int i = m_highlight;
        closePopup();
        if (i >= 0) {
            setCurrentIndex(i);
        }
        return true;
    }
    default:
        return true; // swallow everything else while open
    }
}

bool GlDropdown::keyPressed(QKeyEvent* e) {
    if (e->key() == Qt::Key_Down || e->key() == Qt::Key_Up) {
        setCurrentIndex(m_index + (e->key() == Qt::Key_Down ? 1 : -1));
        return true;
    }
    return false;
}

void GlDropdown::paint(QPainter& p, const Style& s) {
    const QRectF r = rect();
    const qreal h = hoverAmount();
    p.save();
    if (!isEnabled()) {
        p.setOpacity(0.5);
    }
    if (!m_compact) {
        p.setFont(GlUi::uiFont(10.5));
        p.setPen(s.text);
        p.drawText(QRectF(r.left() + 14, r.top(), r.width() * 0.4, r.height()), Qt::AlignVCenter | Qt::AlignLeft, m_text);
    }
    const QRectF f = fieldRect();
    const qreal rad = std::max(6.0, 14 * s.radius);
    QColor fill = s.glass;
    fill.setAlphaF(std::min(1.0, fill.alphaF() + 0.12 + 0.1 * h));
    p.setPen(QPen(GlUi::mix(s.glassEdge, s.accent2, std::max(h, m_open ? 1.0 : 0.0)), 1.1));
    p.setBrush(fill);
    p.drawRoundedRect(f.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    glow(p, f, rad, s.accent2, 0.4 * h);
    p.setFont(GlUi::uiFont(10.5, true));
    p.setPen(s.text);
    const QString cur = (m_index >= 0 && m_index < m_items.size()) ? m_items[m_index] : QString();
    p.drawText(f.adjusted(14, 0, -34, 0), Qt::AlignVCenter | Qt::AlignLeft,
               QFontMetricsF(p.font()).elidedText(cur, Qt::ElideRight, f.width() - 50));
    // Chevron.
    p.setPen(QPen(GlUi::mix(s.textDim, s.accent2, h), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF c(f.right() - 18, f.center().y() + (m_open ? 1 : -1));
    if (m_open) {
        p.drawPolyline(QPolygonF({QPointF(c.x() - 5, c.y() + 2), QPointF(c.x(), c.y() - 3), QPointF(c.x() + 5, c.y() + 2)}));
    } else {
        p.drawPolyline(QPolygonF({QPointF(c.x() - 5, c.y() - 2), QPointF(c.x(), c.y() + 3), QPointF(c.x() + 5, c.y() - 2)}));
    }
    p.restore();
    if (hasFocus()) {
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.accent2, 2.0));
        p.drawRoundedRect(f.adjusted(-2, -2, 2, 2), rad + 2, rad + 2);
        p.restore();
    }
}

void GlDropdown::paintOverlay(QPainter& p, const Style& s) {
    if (!m_open) {
        return;
    }
    const QRectF pr = popupRect();
    const qreal rad = std::max(6.0, 14 * s.radius);
    p.save();
    // Opaque body so the list stays readable over any content.
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 4; ++i) {
        p.setBrush(QColor(0, 0, 0, 40 - i * 8));
        p.drawRoundedRect(pr.translated(0, 3 + i * 2).adjusted(-i, -i, i, i), rad + i, rad + i);
    }
    p.setBrush(GlUi::mix(s.bgBase, s.bgTint, 0.5));
    p.drawRoundedRect(pr, rad, rad);
    p.setBrush(s.glass);
    p.drawRoundedRect(pr, rad, rad);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(GlUi::mix(s.glassEdge, s.accent2, 0.7), 1.2));
    p.drawRoundedRect(pr.adjusted(0.6, 0.6, -0.6, -0.6), rad, rad);
    for (int i = 0; i < m_items.size(); ++i) {
        const QRectF row(pr.left() + 5, pr.top() + 5 + i * 34, pr.width() - 10, 34);
        if (i == m_highlight) {
            QLinearGradient g(row.topLeft(), row.topRight());
            QColor a = s.accent, b = s.accent2;
            a.setAlphaF(0.55);
            b.setAlphaF(0.35);
            g.setColorAt(0, a);
            g.setColorAt(1, b);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(row, std::max(4.0, 10 * s.radius), std::max(4.0, 10 * s.radius));
        }
        p.setFont(GlUi::uiFont(10.5, i == m_index));
        p.setPen(i == m_index ? s.accent2 : s.text);
        p.drawText(row.adjusted(12, 0, -28, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetricsF(p.font()).elidedText(m_items[i], Qt::ElideRight, row.width() - 40));
        if (i == m_index) {
            p.setPen(QPen(s.accent2, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF c(row.right() - 16, row.center().y());
            p.drawPolyline(QPolygonF({QPointF(c.x() - 5, c.y()), QPointF(c.x() - 1, c.y() + 4), QPointF(c.x() + 6, c.y() - 5)}));
        }
    }
    p.restore();
}

// ---------------------------------------------------------- GlScrollBar

GlScrollBar::GlScrollBar(GlCanvas* canvas) : GlItem(canvas) {}

void GlScrollBar::setMetrics(qreal viewport, qreal content, qreal offset) {
    m_viewport = std::max<qreal>(1, viewport);
    m_content = std::max<qreal>(1, content);
    m_offset = std::clamp(offset, 0.0, std::max<qreal>(0, m_content - m_viewport));
    update();
}

QRectF GlScrollBar::thumb() const {
    const QRectF r = rect();
    const qreal th = std::max<qreal>(32, r.height() * m_viewport / m_content);
    const qreal range = std::max<qreal>(1, m_content - m_viewport);
    return QRectF(r.left(), r.top() + (r.height() - th) * (m_offset / range), r.width(), th);
}

void GlScrollBar::mousePressAt(const QPointF& p) {
    const QRectF t = thumb();
    if (t.contains(p)) {
        m_grab = p.y() - t.top();
    } else {
        m_grab = t.height() / 2;
        mouseDragTo(p);
    }
}

void GlScrollBar::mouseDragTo(const QPointF& p) {
    const QRectF r = rect();
    const qreal th = thumb().height();
    const qreal span = std::max<qreal>(1, r.height() - th);
    const qreal f = std::clamp((p.y() - m_grab - r.top()) / span, 0.0, 1.0);
    m_offset = f * (m_content - m_viewport);
    update();
    emit offsetChanged(m_offset);
}

void GlScrollBar::paint(QPainter& p, const Style& s) {
    if (m_content <= m_viewport + 0.5) {
        return;
    }
    const QRectF t = thumb();
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, s.dark ? 18 : 0) );
    p.drawRoundedRect(rect(), rect().width() / 2, rect().width() / 2);
    QColor c = GlUi::mix(s.accent, s.accent2, 0.5);
    c.setAlphaF(0.55 + 0.35 * std::max(hoverAmount(), pressAmount()));
    p.setBrush(c);
    p.drawRoundedRect(t, t.width() / 2, t.width() / 2);
    p.restore();
}


// ----------------------------------------------------- GlCanvas extras

void GlCanvas::contextMenuEvent(QContextMenuEvent* e) {
    if (m_popup) {
        return;
    }
    if (e->reason() == QContextMenuEvent::Keyboard) {
        if (m_focus && m_focus->isVisible()) {
            m_focus->contextMenuAt(m_focus->rect().center(), mapToGlobal(m_focus->rect().center().toPoint()), true);
        }
        return;
    }
    const QPointF pt = e->pos();
    for (int i = m_items.size() - 1; i >= 0; --i) {
        GlItem* it = m_items[i];
        if (it->isInteractive() && it->isHit(pt)) {
            it->contextMenuAt(pt, e->globalPos(), false);
            return;
        }
    }
}

void GlCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    mousePressEvent(e); // a fast second click must still act like a click
    if (m_popup) {
        return;
    }
    if (m_hover && m_hover->isEnabled()) {
        m_hover->doubleClickAt(e->position());
    }
}

bool GlCanvas::event(QEvent* e) {
    if (e->type() == QEvent::ShortcutOverride && qobject_cast<GlTextInput*>(m_focus)) {
        e->accept(); // typing in a field beats window shortcuts
        return true;
    }
    return QOpenGLWidget::event(e);
}

// ---------------------------------------------------------- GlTextInput

GlTextInput::GlTextInput(GlCanvas* canvas) : GlItem(canvas) {}

bool GlTextInput::anyFocused() {
    auto* c = qobject_cast<GlCanvas*>(QApplication::focusWidget());
    return c && qobject_cast<GlTextInput*>(c->focusedItem());
}

void GlTextInput::setText(const QString& t) {
    if (t == m_text) {
        return;
    }
    m_text = t;
    m_cursor = m_anchor = int(t.size());
    update();
    emit textChanged(m_text);
}

void GlTextInput::focusInput(bool selectAll) {
    canvas()->setFocusedItem(this);
    canvas()->setFocus(Qt::OtherFocusReason);
    if (selectAll) {
        m_anchor = 0;
        m_cursor = int(m_text.size());
    }
    m_blinkBase = canvas()->clock();
    update();
}

void GlTextInput::focusChanged(bool focused) {
    if (focused) {
        if (m_selectAll) {
            m_anchor = 0;
            m_cursor = int(m_text.size());
        }
        m_blinkBase = canvas()->clock();
    } else {
        emit focusLost();
    }
}

bool GlTextInput::advance(qreal dt) {
    bool busy = GlItem::advance(dt);
    return busy || (canvas()->focusedItem() == this && canvas()->hasFocus());
}

QRectF GlTextInput::textRect() const {
    const QRectF r = rect();
    return QRectF(r.left() + 14, r.top(), r.width() - 28 - (m_clearable && !m_text.isEmpty() ? 22 : 0), r.height());
}

QRectF GlTextInput::clearRect() const {
    const QRectF r = rect();
    return QRectF(r.right() - 30, r.center().y() - 10, 20, 20);
}

int GlTextInput::posAt(qreal x) const {
    const QFontMetricsF fm(GlUi::uiFont(10.5));
    const qreal rel = x - textRect().left() + m_scroll;
    int best = 0;
    qreal bestD = 1e9;
    for (int i = 0; i <= m_text.size(); ++i) {
        const qreal d = std::abs(fm.horizontalAdvance(m_text.left(i)) - rel);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void GlTextInput::mousePressAt(const QPointF& p) {
    if (m_clearable && !m_text.isEmpty() && clearRect().adjusted(-4, -4, 4, 4).contains(p)) {
        clear();
        m_cursor = m_anchor = 0;
        return;
    }
    m_cursor = m_anchor = posAt(p.x());
    m_blinkBase = canvas()->clock();
    update();
}

void GlTextInput::mouseDragTo(const QPointF& p) {
    m_cursor = posAt(p.x());
    update();
}

void GlTextInput::edit(const QString& t, int cursor) {
    const bool changed = t != m_text;
    m_text = t;
    m_cursor = m_anchor = std::clamp(cursor, 0, int(m_text.size()));
    m_blinkBase = canvas()->clock();
    update();
    if (changed) {
        emit textChanged(m_text);
    }
}

bool GlTextInput::keyPressed(QKeyEvent* e) {
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const int lo = std::min(m_cursor, m_anchor), hi = std::max(m_cursor, m_anchor);
    const auto moveTo = [&](int pos) {
        m_cursor = std::clamp(pos, 0, int(m_text.size()));
        if (!shift) {
            m_anchor = m_cursor;
        }
        m_blinkBase = canvas()->clock();
        update();
    };
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        emit editingFinished();
        return true;
    case Qt::Key_Escape:
        emit cancelled();
        return m_escConsumed;
    case Qt::Key_Left:
        moveTo(!shift && lo != hi ? lo : m_cursor - 1);
        return true;
    case Qt::Key_Right:
        moveTo(!shift && lo != hi ? hi : m_cursor + 1);
        return true;
    case Qt::Key_Home:
        moveTo(0);
        return true;
    case Qt::Key_End:
        moveTo(int(m_text.size()));
        return true;
    case Qt::Key_Backspace:
        if (lo != hi) edit(m_text.left(lo) + m_text.mid(hi), lo);
        else if (m_cursor > 0) edit(m_text.left(m_cursor - 1) + m_text.mid(m_cursor), m_cursor - 1);
        return true;
    case Qt::Key_Delete:
        if (lo != hi) edit(m_text.left(lo) + m_text.mid(hi), lo);
        else if (m_cursor < m_text.size()) edit(m_text.left(m_cursor) + m_text.mid(m_cursor + 1), m_cursor);
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    case Qt::Key_Up:
    case Qt::Key_Down:
        return false;
    default:
        break;
    }
    if (ctrl) {
        if (e->key() == Qt::Key_A) {
            m_anchor = 0;
            m_cursor = int(m_text.size());
            update();
            return true;
        }
        if ((e->key() == Qt::Key_C || e->key() == Qt::Key_X) && lo != hi) {
            QGuiApplication::clipboard()->setText(m_text.mid(lo, hi - lo));
            if (e->key() == Qt::Key_X) edit(m_text.left(lo) + m_text.mid(hi), lo);
            return true;
        }
        if (e->key() == Qt::Key_V) {
            QString clip = QGuiApplication::clipboard()->text();
            clip.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QString());
            edit(m_text.left(lo) + clip + m_text.mid(hi), lo + int(clip.size()));
            return true;
        }
        if (!(e->modifiers() & Qt::AltModifier)) {
            return false; // other Ctrl shortcuts are not text
        }
    }
    const QString t = e->text();
    if (!t.isEmpty() && t.at(0).isPrint()) {
        edit(m_text.left(lo) + t + m_text.mid(hi), lo + int(t.size()));
        return true;
    }
    return false;
}

void GlTextInput::paint(QPainter& p, const Style& s) {
    const QRectF r = rect();
    const bool focused = canvas()->focusedItem() == this && canvas()->hasFocus();
    const qreal rad = std::max(6.0, 14 * s.radius);
    p.save();
    if (!isEnabled()) {
        p.setOpacity(0.5);
    }
    QColor fill = s.glass;
    fill.setAlphaF(std::min(1.0, fill.alphaF() + 0.08));
    p.setPen(QPen(focused ? s.accent2 : s.glassEdge, focused ? 1.6 : 1.1));
    p.setBrush(fill);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    if (focused) {
        glow(p, r, rad, s.accent2, 0.35);
    }
    const QFont f = GlUi::uiFont(10.5);
    const QFontMetricsF fm(f);
    p.setFont(f);
    const QRectF tr = textRect();
    // Keep the caret in view.
    const qreal caretX = fm.horizontalAdvance(m_text.left(m_cursor));
    if (caretX - m_scroll > tr.width() - 2) m_scroll = caretX - tr.width() + 2;
    if (caretX - m_scroll < 0) m_scroll = caretX;
    if (fm.horizontalAdvance(m_text) <= tr.width()) m_scroll = 0;
    p.save();
    p.setClipRect(tr);
    if (m_text.isEmpty()) {
        p.setPen(s.textDim);
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, m_placeholder);
    } else {
        const int lo = std::min(m_cursor, m_anchor), hi = std::max(m_cursor, m_anchor);
        if (focused && lo != hi) {
            const qreal x0 = tr.left() - m_scroll + fm.horizontalAdvance(m_text.left(lo));
            const qreal x1 = tr.left() - m_scroll + fm.horizontalAdvance(m_text.left(hi));
            QColor sel = s.accent;
            sel.setAlphaF(0.55);
            p.setPen(Qt::NoPen);
            p.setBrush(sel);
            p.drawRect(QRectF(x0, r.center().y() - fm.height() / 2, x1 - x0, fm.height()));
        }
        p.setPen(s.text);
        p.drawText(QRectF(tr.left() - m_scroll, tr.top(), 100000, tr.height()), Qt::AlignVCenter | Qt::AlignLeft, m_text);
    }
    if (focused && std::fmod(canvas()->clock() - m_blinkBase, 1.0) < 0.55) {
        const qreal x = tr.left() - m_scroll + caretX;
        p.setPen(QPen(s.text, 1.4));
        p.drawLine(QPointF(x, r.center().y() - fm.height() / 2), QPointF(x, r.center().y() + fm.height() / 2));
    }
    p.restore();
    if (m_clearable && !m_text.isEmpty()) {
        const QRectF c = clearRect();
        p.setPen(QPen(GlUi::mix(s.textDim, s.text, hoverAmount()), 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c.center() + QPointF(-4, -4), c.center() + QPointF(4, 4));
        p.drawLine(c.center() + QPointF(-4, 4), c.center() + QPointF(4, -4));
    }
    p.restore();
}

// -------------------------------------------------------- GlTreeDropdown

GlTreeDropdown::GlTreeDropdown(GlCanvas* canvas) : GlItem(canvas) {
    m_allLabel = QObject::tr("All");
    rebuild();
}

void GlTreeDropdown::setCategories(const QVector<PlaylistFilterInfo>& categories, const QString& allLabel) {
    m_allLabel = allLabel;
    m_ids.clear();
    m_parents.clear();
    m_names.clear();
    bool exists = m_current == 0;
    QSet<qint64> ids;
    for (const PlaylistFilterInfo& f : categories) {
        m_ids << f.id;
        m_parents << f.parentId;
        m_names << f.name;
        ids.insert(f.id);
        exists = exists || f.id == m_current;
    }
    m_collapsed.intersect(ids);
    m_current = exists ? m_current : 0;
    rebuild();
}

void GlTreeDropdown::rebuild() {
    m_nodes.clear();
    m_nodes.push_back({0, m_allLabel, 0, false});
    const QSet<qint64> known(m_ids.begin(), m_ids.end());
    QVector<bool> placed(m_ids.size(), false);
    // Depth-first so a parent is always followed by its subtree.
    std::function<void(qint64, int)> addChildren = [&](qint64 parent, int depth) {
        for (int i = 0; i < m_ids.size(); ++i) {
            const bool isRoot = m_parents[i] <= 0 || !known.contains(m_parents[i]);
            const bool match = parent == 0 ? isRoot : m_parents[i] == parent;
            if (!match || placed[i] || m_ids[i] == parent) {
                continue;
            }
            placed[i] = true;
            bool hasChildren = false;
            for (qint64 par : std::as_const(m_parents)) {
                hasChildren = hasChildren || par == m_ids[i];
            }
            m_nodes.push_back({m_ids[i], m_names[i], depth, hasChildren});
            addChildren(m_ids[i], depth + 1);
        }
    };
    addChildren(0, 1);
    QString leaf = m_allLabel;
    for (int i = 0; i < m_ids.size(); ++i) {
        if (m_ids[i] == m_current && m_current != 0) {
            leaf = m_names[i];
        }
    }
    setToolTip(QObject::tr("Selected category: %1").arg(pathOf(m_current)));
    update();
}

QString GlTreeDropdown::pathOf(qint64 id) const {
    if (id == 0) {
        return m_allLabel;
    }
    QStringList parts;
    QSet<qint64> seen;
    while (id > 0 && !seen.contains(id)) {
        seen.insert(id);
        const int i = m_ids.indexOf(id);
        if (i < 0) {
            break;
        }
        parts.prepend(m_names[i]);
        id = m_parents[i];
    }
    return parts.join(QStringLiteral("  >  "));
}

void GlTreeDropdown::setCurrentCategory(qint64 id) {
    const qint64 next = (id == 0 || m_ids.contains(id)) ? id : 0;
    if (next == m_current) {
        return;
    }
    m_current = next;
    rebuild();
    emit currentCategoryChanged(m_current);
}

QVector<GlTreeDropdown::Node> GlTreeDropdown::visibleNodes() const {
    QVector<Node> out;
    int skipDepth = -1;
    for (const Node& n : m_nodes) {
        if (skipDepth >= 0 && n.depth > skipDepth) {
            continue;
        }
        skipDepth = -1;
        out.push_back(n);
        if (n.hasChildren && m_collapsed.contains(n.id)) {
            skipDepth = n.depth;
        }
    }
    return out;
}

QRectF GlTreeDropdown::popupRect() const {
    const QRectF f = rect();
    const int rows = int(visibleNodes().size());
    const qreal h = std::min<qreal>(340, rows * 32 + 10);
    const qreal w = std::max<qreal>(f.width(), 260);
    qreal top = f.bottom() + 4;
    if (top + h > canvas()->height() - 8) {
        top = std::max<qreal>(8, f.top() - 4 - h);
    }
    return QRectF(f.left(), top, w, h);
}

QRectF GlTreeDropdown::rowRect(int i) const {
    const QRectF pr = popupRect();
    return QRectF(pr.left() + 5, pr.top() + 5 + i * 32, pr.width() - 10, 32);
}

int GlTreeDropdown::indexAt(const QPointF& pt) const {
    const int n = int(visibleNodes().size());
    for (int i = 0; i < n; ++i) {
        if (rowRect(i).contains(pt)) {
            return i;
        }
    }
    return -1;
}

void GlTreeDropdown::closePopup() {
    m_open = false;
    if (canvas()->popup() == this) {
        canvas()->setPopup(nullptr);
    }
}

void GlTreeDropdown::activate() {
    if (m_open) {
        closePopup();
        return;
    }
    m_open = true;
    const QVector<Node> vis = visibleNodes();
    m_highlight = 0;
    for (int i = 0; i < vis.size(); ++i) {
        if (vis[i].id == m_current) {
            m_highlight = i;
        }
    }
    canvas()->setPopup(this);
}

bool GlTreeDropdown::popupPress(const QPointF& pt) {
    const QVector<Node> vis = visibleNodes();
    const int i = indexAt(pt);
    if (i >= 0) {
        const Node& n = vis[i];
        const qreal chevX = rowRect(i).left() + 8 + n.depth * 18;
        if (n.hasChildren && pt.x() >= chevX - 6 && pt.x() <= chevX + 16) {
            if (!m_collapsed.remove(n.id)) {
                m_collapsed.insert(n.id);
            }
            update();
            return true; // stay open
        }
        closePopup();
        setCurrentCategory(n.id);
        return true;
    }
    closePopup();
    return rect().contains(pt);
}

void GlTreeDropdown::popupHover(const QPointF& pt) {
    const int i = indexAt(pt);
    if (i >= 0 && i != m_highlight) {
        m_highlight = i;
        update();
    }
}

bool GlTreeDropdown::popupKey(QKeyEvent* e) {
    const QVector<Node> vis = visibleNodes();
    switch (e->key()) {
    case Qt::Key_Escape:
        closePopup();
        return true;
    case Qt::Key_Up:
        m_highlight = std::max(0, m_highlight - 1);
        break;
    case Qt::Key_Down:
        m_highlight = std::min(int(vis.size()) - 1, m_highlight + 1);
        break;
    case Qt::Key_Left:
        if (m_highlight < vis.size() && vis[m_highlight].hasChildren) m_collapsed.insert(vis[m_highlight].id);
        break;
    case Qt::Key_Right:
        if (m_highlight < vis.size()) m_collapsed.remove(vis[m_highlight].id);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space: {
        const qint64 id = m_highlight < vis.size() ? vis[m_highlight].id : 0;
        closePopup();
        setCurrentCategory(id);
        return true;
    }
    default:
        break;
    }
    update();
    return true;
}

bool GlTreeDropdown::keyPressed(QKeyEvent* e) {
    if (e->key() == Qt::Key_Down || e->key() == Qt::Key_F4) {
        activate();
        return true;
    }
    return false;
}

void GlTreeDropdown::contextMenuAt(const QPointF&, const QPoint& global, bool) {
    emit contextMenuRequested(global);
}

void GlTreeDropdown::paint(QPainter& p, const Style& s) {
    const QRectF f = rect();
    const qreal h = hoverAmount();
    const qreal rad = std::max(6.0, 14 * s.radius);
    p.save();
    if (!isEnabled()) {
        p.setOpacity(0.5);
    }
    QColor fill = s.glass;
    fill.setAlphaF(std::min(1.0, fill.alphaF() + 0.12 + 0.1 * h));
    p.setPen(QPen(GlUi::mix(s.glassEdge, s.accent2, std::max(h, m_open ? 1.0 : 0.0)), 1.1));
    p.setBrush(fill);
    p.drawRoundedRect(f.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    glow(p, f, rad, s.accent2, 0.4 * h);
    QString leaf = m_allLabel;
    for (int i = 0; i < m_ids.size(); ++i) {
        if (m_ids[i] == m_current && m_current != 0) {
            leaf = m_names[i];
        }
    }
    p.setFont(GlUi::uiFont(10.5, true));
    p.setPen(s.text);
    p.drawText(f.adjusted(14, 0, -34, 0), Qt::AlignVCenter | Qt::AlignLeft,
               QFontMetricsF(p.font()).elidedText(leaf, Qt::ElideRight, f.width() - 50));
    p.setPen(QPen(GlUi::mix(s.textDim, s.accent2, h), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF c(f.right() - 18, f.center().y() + (m_open ? 1 : -1));
    if (m_open) {
        p.drawPolyline(QPolygonF({QPointF(c.x() - 5, c.y() + 2), QPointF(c.x(), c.y() - 3), QPointF(c.x() + 5, c.y() + 2)}));
    } else {
        p.drawPolyline(QPolygonF({QPointF(c.x() - 5, c.y() - 2), QPointF(c.x(), c.y() + 3), QPointF(c.x() + 5, c.y() - 2)}));
    }
    p.restore();
    if (hasFocus()) {
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.accent2, 2.0));
        p.drawRoundedRect(f.adjusted(-2, -2, 2, 2), rad + 2, rad + 2);
        p.restore();
    }
}

void GlTreeDropdown::paintOverlay(QPainter& p, const Style& s) {
    if (!m_open) {
        return;
    }
    const QVector<Node> vis = visibleNodes();
    const QRectF pr = popupRect();
    const qreal rad = std::max(6.0, 14 * s.radius);
    p.save();
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 4; ++i) {
        p.setBrush(QColor(0, 0, 0, 40 - i * 8));
        p.drawRoundedRect(pr.translated(0, 3 + i * 2).adjusted(-i, -i, i, i), rad + i, rad + i);
    }
    p.setBrush(GlUi::mix(s.bgBase, s.bgTint, 0.5));
    p.drawRoundedRect(pr, rad, rad);
    p.setBrush(s.glass);
    p.drawRoundedRect(pr, rad, rad);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(GlUi::mix(s.glassEdge, s.accent2, 0.7), 1.2));
    p.drawRoundedRect(pr.adjusted(0.6, 0.6, -0.6, -0.6), rad, rad);
    p.setClipRect(pr.adjusted(0, 4, 0, -4));
    for (int i = 0; i < vis.size(); ++i) {
        const Node& n = vis[i];
        const QRectF row = rowRect(i);
        if (i == m_highlight) {
            QLinearGradient g(row.topLeft(), row.topRight());
            QColor a = s.accent, b = s.accent2;
            a.setAlphaF(0.55);
            b.setAlphaF(0.35);
            g.setColorAt(0, a);
            g.setColorAt(1, b);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(row, std::max(4.0, 10 * s.radius), std::max(4.0, 10 * s.radius));
        }
        const qreal x = row.left() + 8 + n.depth * 18;
        if (n.hasChildren) {
            p.setPen(QPen(s.textDim, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF c(x + 5, row.center().y());
            if (m_collapsed.contains(n.id)) {
                p.drawPolyline(QPolygonF({QPointF(c.x() - 2, c.y() - 4), QPointF(c.x() + 3, c.y()), QPointF(c.x() - 2, c.y() + 4)}));
            } else {
                p.drawPolyline(QPolygonF({QPointF(c.x() - 4, c.y() - 2), QPointF(c.x(), c.y() + 3), QPointF(c.x() + 4, c.y() - 2)}));
            }
        }
        const bool cur = n.id == m_current;
        p.setFont(GlUi::uiFont(10.5, cur));
        p.setPen(cur ? s.accent2 : s.text);
        p.drawText(QRectF(x + 20, row.top(), row.right() - x - 28, row.height()), Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetricsF(p.font()).elidedText(n.name, Qt::ElideRight, row.right() - x - 30));
    }
    p.restore();
}

// ------------------------------------------------------------ GlItemView

GlItemView::GlItemView(GlCanvas* canvas) : GlItem(canvas) {}

void GlItemView::setModel(QAbstractItemModel* model) {
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }
    m_model = model;
    m_hidden.clear();
    m_current = QPersistentModelIndex();
    m_scrollY = 0;
    m_visibleDirty = true;
    if (m_model) {
        const auto structural = [this] {
            m_hidden.clear(); // the owner re-applies its filter after structural changes
            m_visibleDirty = true;
            clampScroll();
            update();
        };
        connect(m_model, &QAbstractItemModel::modelReset, this, structural);
        connect(m_model, &QAbstractItemModel::layoutChanged, this, structural);
        connect(m_model, &QAbstractItemModel::rowsInserted, this, structural);
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, structural);
        connect(m_model, &QAbstractItemModel::rowsMoved, this, structural);
        connect(m_model, &QAbstractItemModel::dataChanged, this, [this] { update(); });
        connect(m_model, &QObject::destroyed, this, [this] {
            m_model = nullptr;
            m_visibleDirty = true;
            update();
        });
    }
    update();
}

void GlItemView::setMode(Mode m, const QSize& cell, int spacing) {
    m_mode = m;
    m_cell = cell;
    m_spacing = spacing;
    update();
}

void GlItemView::setEmptyText(const QString& title, const QString& subtitle) {
    m_emptyTitle = title;
    m_emptySub = subtitle;
    update();
}

int GlItemView::rowCount() const {
    return m_model ? m_model->rowCount() : 0;
}

void GlItemView::rebuildVisible() const {
    m_visible.clear();
    const int n = rowCount();
    for (int r = 0; r < n; ++r) {
        if (!m_hidden.contains(r)) {
            m_visible.push_back(r);
        }
    }
    m_visibleDirty = false;
}

QRectF GlItemView::viewport() const {
    const qreal inset = m_framed ? 8 : 0;
    return rect().adjusted(inset, inset, -inset - 12, -inset);
}

int GlItemView::columns() const {
    if (m_mode == Mode::List) {
        return 1;
    }
    return std::max(1, int((viewport().width() + m_spacing) / (m_cell.width() + m_spacing)));
}

QSizeF GlItemView::contentSize() const {
    if (m_visibleDirty) rebuildVisible();
    const int n = int(m_visible.size());
    const int cols = columns();
    const int rows = (n + cols - 1) / cols;
    return QSizeF(viewport().width(), rows > 0 ? rows * (m_cell.height() + m_spacing) - m_spacing : 0);
}

void GlItemView::clampScroll() {
    const qreal maxS = std::max<qreal>(0, contentSize().height() - viewport().height());
    m_scrollY = std::clamp(m_scrollY, 0.0, maxS);
}

QRectF GlItemView::cellRect(int vi) const {
    const QRectF vp = viewport();
    const int cols = columns();
    const int col = vi % cols, row = vi / cols;
    if (m_mode == Mode::List) {
        return QRectF(vp.left(), vp.top() + row * (m_cell.height() + m_spacing) - m_scrollY, vp.width(), m_cell.height());
    }
    // Cells stretch to share the leftover width, so the last column never leaves a blank strip.
    const qreal w = std::max<qreal>(m_cell.width(), (vp.width() + m_spacing) / cols - m_spacing);
    return QRectF(vp.left() + col * (w + m_spacing), vp.top() + row * (m_cell.height() + m_spacing) - m_scrollY,
                  w, m_cell.height());
}

QRectF GlItemView::rowRect(int row) const {
    if (m_visibleDirty) rebuildVisible();
    const int vi = m_visible.indexOf(row);
    return vi < 0 ? QRectF() : cellRect(vi);
}

int GlItemView::rowAt(const QPointF& p) const {
    if (m_visibleDirty) rebuildVisible();
    if (!viewport().contains(p)) {
        return -1;
    }
    const int cols = columns();
    const int n = int(m_visible.size());
    const int firstRow = std::max(0, int((p.y() - viewport().top() + m_scrollY) / (m_cell.height() + m_spacing)));
    for (int r = std::max(0, firstRow - 1); r <= firstRow + 1; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int vi = r * cols + c;
            if (vi >= 0 && vi < n && cellRect(vi).contains(p)) {
                return m_visible[vi];
            }
        }
    }
    return -1;
}

int GlItemView::insertPositionAt(const QPointF& p) const {
    if (m_visibleDirty) rebuildVisible();
    for (int vi = 0; vi < m_visible.size(); ++vi) {
        const QRectF r = cellRect(vi);
        if (p.y() < r.top() - m_spacing) {
            return m_visible[vi];
        }
        if (p.y() <= r.bottom() + m_spacing && p.x() < r.center().x()) {
            return m_visible[vi];
        }
    }
    return rowCount();
}

void GlItemView::setDropIndicator(int pos) {
    if (pos != m_dropPos) {
        m_dropPos = pos;
        update();
    }
}

void GlItemView::setRowHidden(int row, bool hidden) {
    if (hidden == m_hidden.contains(row)) {
        return;
    }
    if (hidden) m_hidden.insert(row); else m_hidden.remove(row);
    m_visibleDirty = true;
    clampScroll();
    update();
}

void GlItemView::setCurrentRow(int row, bool scroll) {
    const QModelIndex idx = (m_model && row >= 0 && row < m_model->rowCount()) ? m_model->index(row, 0) : QModelIndex();
    const int old = currentRow();
    m_current = idx;
    if (scroll && idx.isValid()) {
        scrollTo(row);
    }
    update();
    if (old != currentRow()) {
        emit currentRowChanged(currentRow());
    }
}

void GlItemView::clearCurrent() {
    setCurrentRow(-1);
}

void GlItemView::scrollTo(int row) {
    if (m_visibleDirty) rebuildVisible();
    const int vi = m_visible.indexOf(row);
    if (vi < 0) {
        return;
    }
    const QRectF vp = viewport();
    const QRectF c = cellRect(vi);
    if (c.top() < vp.top()) {
        m_scrollY -= vp.top() - c.top();
    } else if (c.bottom() > vp.bottom()) {
        m_scrollY += c.bottom() - vp.bottom();
    }
    clampScroll();
    update();
}

void GlItemView::scrollBy(qreal dy) {
    m_scrollY += dy;
    clampScroll();
    update();
}

QRectF GlItemView::scrollTrack() const {
    const QRectF r = rect();
    const qreal inset = m_framed ? 6 : 0;
    return QRectF(r.right() - inset - 8, r.top() + inset, 8, r.height() - 2 * inset);
}

QRectF GlItemView::scrollThumb() const {
    const QRectF t = scrollTrack();
    const qreal ch = contentSize().height(), vh = viewport().height();
    if (ch <= vh + 0.5) {
        return QRectF();
    }
    const qreal th = std::max<qreal>(32, t.height() * vh / ch);
    return QRectF(t.left(), t.top() + (t.height() - th) * (m_scrollY / (ch - vh)), t.width(), th);
}

void GlItemView::mousePressAt(const QPointF& p) {
    m_dragging = false;
    const QRectF thumb = scrollThumb();
    if (!thumb.isNull() && scrollTrack().adjusted(-4, 0, 4, 0).contains(p)) {
        m_sbDrag = true;
        m_sbGrab = thumb.contains(p) ? p.y() - thumb.top() : thumb.height() / 2;
        mouseDragTo(p);
        return;
    }
    m_pressRow = rowAt(p);
    m_pressPos = p;
    if (m_pressRow >= 0 && (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier)) {
        const int row = m_pressRow;
        m_pressRow = -1; // not the start of a drag
        emit shiftClicked(row);
        return;
    }
    if (m_pressRow >= 0) {
        setCurrentRow(m_pressRow, false);
        emit clicked(m_pressRow);
    }
}

void GlItemView::mouseDragTo(const QPointF& p) {
    if (m_sbDrag) {
        const QRectF t = scrollTrack();
        const qreal th = scrollThumb().height();
        const qreal span = std::max<qreal>(1, t.height() - th);
        const qreal f = std::clamp((p.y() - m_sbGrab - t.top()) / span, 0.0, 1.0);
        m_scrollY = f * (contentSize().height() - viewport().height());
        clampScroll();
        update();
        return;
    }
    if (m_pressRow < 0 || !m_reorder) {
        return;
    }
    if (!m_dragging && QLineF(p, m_pressPos).length() > 8) {
        m_dragging = true;
    }
    if (m_dragging) {
        m_dragPos = p;
        setDropIndicator(insertPositionAt(p));
        const QRectF vp = viewport();
        m_autoScroll = p.y() < vp.top() + 26 ? -320 : p.y() > vp.bottom() - 26 ? 320 : 0;
        update();
    }
}

void GlItemView::mouseReleased(const QPointF&) {
    if (m_dragging && m_pressRow >= 0 && m_dropPos >= 0) {
        const int from = m_pressRow, pos = m_dropPos;
        m_dragging = false;
        m_autoScroll = 0;
        setDropIndicator(-1);
        m_pressRow = -1;
        emit reorderRequested(from, pos);
    }
    m_dragging = false;
    m_autoScroll = 0;
    m_sbDrag = false;
    if (m_dropPos >= 0 && !m_dragging) {
        setDropIndicator(-1);
    }
    m_pressRow = -1;
    update();
}

void GlItemView::doubleClickAt(const QPointF& p) {
    const int row = rowAt(p);
    if (row >= 0) {
        emit doubleClicked(row);
    }
}

void GlItemView::hoverMoved(const QPointF& p) {
    const int row = rowAt(p);
    if (row != m_hoverRow) {
        m_hoverRow = row;
        update();
    }
}

void GlItemView::contextMenuAt(const QPointF& p, const QPoint& global, bool kb) {
    int row = kb ? currentRow() : rowAt(p);
    if (row >= 0) {
        setCurrentRow(row, false);
    }
    emit contextMenuRequested(kb && row >= 0 ? rowRect(row).center() : p, global, row);
}

bool GlItemView::wheelAt(int delta, const QPointF&) {
    if (contentSize().height() <= viewport().height() + 0.5) {
        return false;
    }
    scrollBy(-delta * 0.8);
    return true;
}

bool GlItemView::advance(qreal dt) {
    bool busy = GlItem::advance(dt);
    if (m_dragging && m_autoScroll != 0) {
        scrollBy(m_autoScroll * dt);
        setDropIndicator(insertPositionAt(m_dragPos));
        busy = true;
    }
    return busy || m_dragging;
}

void GlItemView::navigate(int dRow, int dCol) {
    if (m_visibleDirty) rebuildVisible();
    if (m_visible.isEmpty()) {
        return;
    }
    const int cols = columns();
    int vi = m_visible.indexOf(currentRow());
    if (vi < 0) {
        vi = 0;
    } else {
        vi = std::clamp(vi + dRow * cols + dCol, 0, int(m_visible.size()) - 1);
    }
    setCurrentRow(m_visible[vi], true);
}

bool GlItemView::keyPressed(QKeyEvent* e) {
    if (m_keyFilter && m_keyFilter(e)) {
        return true;
    }
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
        return false;
    }
    const int pageRows = std::max(1, int(viewport().height() / (m_cell.height() + m_spacing)));
    switch (e->key()) {
    case Qt::Key_Up: navigate(-1, 0); return true;
    case Qt::Key_Down: navigate(1, 0); return true;
    case Qt::Key_Left: if (m_mode == Mode::Grid) { navigate(0, -1); return true; } return false;
    case Qt::Key_Right: if (m_mode == Mode::Grid) { navigate(0, 1); return true; } return false;
    case Qt::Key_PageUp: navigate(-pageRows, 0); return true;
    case Qt::Key_PageDown: navigate(pageRows, 0); return true;
    case Qt::Key_Home: navigate(-100000, 0); return true;
    case Qt::Key_End: navigate(100000, 0); return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (currentRow() >= 0) {
            emit activated(currentRow());
            return true;
        }
        return false;
    default:
        return false;
    }
}

void GlItemView::paint(QPainter& p, const Style& s) {
    const QRectF r = rect();
    if (m_visibleDirty) rebuildVisible();
    clampScroll();
    p.save();
    if (m_framed) {
        const qreal rad = 16 * s.radius + 2;
        p.setPen(QPen(s.glassEdge, 1.0));
        p.setBrush(QColor(0, 0, 0, s.dark ? 46 : 14));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    }
    const QRectF vp = viewport();
    if (rowCount() == 0 && !m_emptyTitle.isEmpty()) {
        p.setFont(GlUi::uiFont(13, true));
        p.setPen(s.text);
        p.drawText(r.adjusted(16, 0, -16, -30), Qt::AlignCenter | Qt::TextWordWrap, m_emptyTitle);
        p.setFont(GlUi::uiFont(10));
        p.setPen(s.textDim);
        p.drawText(r.adjusted(16, 36, -16, 0), Qt::AlignCenter | Qt::TextWordWrap, m_emptySub);
    }
    if (m_delegate && m_model) {
        p.save();
        p.setClipRect(vp.adjusted(-6, -2, 6, 2).intersected(rect()));
        const int cols = columns();
        const int firstRow = std::max(0, int(m_scrollY / (m_cell.height() + m_spacing)));
        const int lastRow = int((m_scrollY + vp.height()) / (m_cell.height() + m_spacing)) + 1;
        const int viFrom = firstRow * cols, viTo = std::min(int(m_visible.size()), (lastRow + 1) * cols);
        for (int vi = viFrom; vi < viTo; ++vi) {
            const int row = m_visible[vi];
            const QRectF cr = cellRect(vi);
            if (!cr.intersects(vp)) {
                continue;
            }
            QStyleOptionViewItem opt;
            opt.rect = cr.toAlignedRect();
            opt.font = GlUi::uiFont(10);
            opt.state = QStyle::State_Enabled;
            if (row == currentRow()) opt.state |= QStyle::State_Selected;
            if (row == m_hoverRow) opt.state |= QStyle::State_MouseOver;
            if (m_dragging && row == m_pressRow) {
                p.setOpacity(0.45);
            }
            m_delegate->paint(&p, opt, m_model->index(row, 0));
            p.setOpacity(1.0);
        }
        // Drop / reorder insertion bar.
        if (m_dropPos >= 0 && !m_visible.isEmpty()) {
            int vi = 0;
            while (vi < m_visible.size() && m_visible[vi] < m_dropPos) {
                ++vi;
            }
            QRectF bar;
            if (vi < m_visible.size()) {
                const QRectF c = cellRect(vi);
                bar = m_mode == Mode::List ? QRectF(c.left(), c.top() - 2, c.width(), 3) : QRectF(c.left() - 2, c.top() + 6, 4, c.height() - 12);
            } else {
                const QRectF c = cellRect(int(m_visible.size()) - 1);
                bar = m_mode == Mode::List ? QRectF(c.left(), c.bottom() - 1, c.width(), 3) : QRectF(c.right() - 2, c.top() + 6, 4, c.height() - 12);
            }
            QLinearGradient g(bar.topLeft(), bar.bottomLeft());
            g.setColorAt(0, s.accent);
            g.setColorAt(1, s.accent2);
            glow(p, bar, 2, s.accent2, 0.9);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(bar, 2, 2);
        }
        // Card being dragged follows the pointer.
        if (m_dragging && m_pressRow >= 0) {
            QStyleOptionViewItem opt;
            opt.rect = QRectF(m_dragPos.x() - m_cell.width() / 2.0, m_dragPos.y() - m_cell.height() / 2.0, m_cell.width(), m_cell.height()).toAlignedRect();
            opt.font = GlUi::uiFont(10);
            opt.state = QStyle::State_Enabled | QStyle::State_Selected;
            p.setOpacity(0.8);
            m_delegate->paint(&p, opt, m_model->index(m_pressRow, 0));
            p.setOpacity(1.0);
        }
        p.restore();
    }
    // Scrollbar.
    const QRectF thumb = scrollThumb();
    if (!thumb.isNull()) {
        QColor c = GlUi::mix(s.accent, s.accent2, 0.5);
        c.setAlphaF(0.5 + 0.4 * (m_sbDrag ? 1.0 : hoverAmount()));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(thumb, 4, 4);
    }
    p.restore();
    if (hasFocus()) {
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.accent2, 2.0));
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 14 * s.radius + 2, 14 * s.radius + 2);
        p.restore();
    }
}
