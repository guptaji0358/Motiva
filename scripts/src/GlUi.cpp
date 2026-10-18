#include "GlUi.h"
#include "Theme.h"

#include <QFocusEvent>
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
                        qintptr* result) {
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
    if (!w->isMaximized()) {
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
    if (m_block != Block::None) {
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
        const QPixmap pm = GlUi::tintedGlyph(m_icon, box.size(), color, dpr);
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
        if (m_hover) m_hover->m_hovered = false;
        m_hover = hit;
        if (m_hover) m_hover->m_hovered = true;
        setCursor(hit && hit->isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        requestAnimation();
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
        m_focus = m_press;
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
    if (item && item == m_hover && item->isEnabled()) {
        item->activate();
    }
}

void GlCanvas::leaveEvent(QEvent* e) {
    QOpenGLWidget::leaveEvent(e);
    if (m_hover) {
        m_hover->m_hovered = false;
        m_hover = nullptr;
        requestAnimation();
    }
}

void GlCanvas::setFocusedItem(GlItem* item) {
    m_focus = item;
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
    m_focus = order[idx];
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
    p.setFont(GlUi::uiFont(10.5));
    p.setPen(s.text);
    p.drawText(QRectF(r.left() + 14, r.top(), r.width() * 0.4, r.height()), Qt::AlignVCenter | Qt::AlignLeft, m_text);
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
