#include "GlWindow.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <algorithm>

// ---------------------------------------------------------------- GlStack

void GlStack::add(GlItem* item, qreal height, qreal gapAfter) {
    Row r;
    r.a = item;
    r.h = height;
    r.gapAfter = gapAfter;
    m_rows.push_back(r);
}

void GlStack::addWrapped(GlLabel* label, qreal gapAfter, qreal minHeight) {
    Row r;
    r.a = label;
    r.wrapped = true;
    r.minH = minHeight;
    r.gapAfter = gapAfter;
    m_rows.push_back(r);
}

void GlStack::addPair(GlItem* left, qreal leftWidth, GlItem* right, qreal height, qreal gapAfter, qreal gap) {
    Row r;
    r.a = left;
    r.b = right;
    r.leftW = leftWidth;
    r.h = height;
    r.gapAfter = gapAfter;
    r.gap = gap;
    m_rows.push_back(r);
}

void GlStack::addSpace(qreal height) {
    Row r;
    r.h = height;
    m_rows.push_back(r);
}

qreal GlStack::layout(const QRectF& area, GlItem* clipTo) const {
    qreal y = area.top();
    for (const Row& r : m_rows) {
        if (!r.a) {
            y += r.h;
            continue;
        }
        if (!r.a->isVisible()) {
            if (r.b) r.b->setVisible(false);
            continue; // hidden items take no room
        }
        qreal h = r.h;
        if (r.wrapped) {
            auto* l = static_cast<GlLabel*>(r.a);
            h = std::max(r.minH, l->heightForWidth(area.width()) + 4);
        }
        if (r.b) {
            // A negative leftWidth means "the right item is this wide, the left takes the rest".
            const qreal lw = r.leftW >= 0 ? r.leftW : std::max<qreal>(0, area.width() + r.leftW - r.gap);
            r.a->setRect(QRectF(area.left(), y, lw, h));
            r.b->setRect(QRectF(area.left() + lw + r.gap, y, std::max<qreal>(0, area.width() - lw - r.gap), h));
        } else {
            r.a->setRect(QRectF(area.left(), y, area.width(), h));
        }
        if (clipTo) {
            r.a->setClip(area);
            if (r.b) r.b->setClip(area);
        }
        y += h + r.gapAfter;
    }
    return y - area.top();
}

// --------------------------------------------------------------- GlWindow

GlWindow::GlWindow(const QString& title, const QString& subtitle, QWidget* parent, bool resizable)
    : QDialog(parent), m_resizable(resizable) {
    setWindowTitle(title);
    setWindowFlag(Qt::FramelessWindowHint, true);
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    m_canvas = new GlCanvas(this);
    box->addWidget(m_canvas);
    m_canvas->setAccessibleName(title);
    m_canvas->setLayoutCallback([this](const QSizeF& s) { layoutGl(s); });
    m_title = new GlPanel(m_canvas);
    m_title->setBlock(GlPanel::Block::Accent);
    m_title->setRadiusPx(16);
    m_content = new GlPanel(m_canvas);
    m_content->setRadiusPx(20);
    m_titleLabel = new GlLabel(m_canvas);
    m_titleLabel->setText(title);
    m_titleLabel->setPointSize(14);
    m_titleLabel->setBold(true);
    m_subtitleLabel = new GlLabel(m_canvas);
    m_subtitleLabel->setText(subtitle);
    m_subtitleLabel->setPointSize(7.5);
    m_subtitleLabel->setBold(true);
    m_subtitleLabel->setCaps(true);
    m_subtitleLabel->setTone(GlLabel::Tone::Dim);
    m_close = new GlButton(m_canvas, GlButton::Kind::Close);
    m_close->setToolTip(tr("Close"));
    connect(m_close, &GlButton::clicked, this, &QDialog::close);
    connect(this, &QWidget::windowTitleChanged, this, [this](const QString& t) { m_titleLabel->setText(t); });
}

void GlWindow::layoutGl(const QSizeF& size) {
    if (!m_close) {
        return;
    }
    const qreal W = size.width(), H = size.height(), m = 12;
    const QRectF strip(m, m, W - 2 * m, 46);
    m_title->setRect(strip);
    m_titleLabel->setRect(QRectF(strip.left() + 20, strip.top() + 5, strip.width() - 80, 24));
    m_subtitleLabel->setRect(QRectF(strip.left() + 20, strip.top() + 27, strip.width() - 80, 14));
    m_close->setRect(QRectF(strip.right() - 10 - 36, strip.top() + 8, 36, 30));
    const QRectF footer(m, H - m - m_footerH, W - 2 * m, m_footerH);
    const QRectF panel(m, strip.bottom() + 10, W - 2 * m, footer.top() - 6 - (strip.bottom() + 10));
    m_content->setRect(panel);
    layoutContent(panel.adjusted(24, 20, -24, -16), footer.adjusted(8, 4, -8, 0));
}

void GlWindow::layoutFooter(const QList<GlButton*>& buttons, const QRectF& footer, qreal width) {
    qreal x = footer.right();
    for (int i = int(buttons.size()) - 1; i >= 0; --i) {
        GlButton* b = buttons[i];
        if (!b->isVisible()) {
            continue;
        }
        const qreal w = std::max(width, QFontMetricsF(GlUi::uiFont(10, true)).horizontalAdvance(b->text()) + 48);
        b->setRect(QRectF(x - w, footer.top() + 6, w, 40));
        x -= w + 10;
    }
}

bool GlWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG" && m_title &&
        GlUi::handleFrameHitTest(this, m_canvas, m_title->rect(), message, result, m_resizable)) {
        return true;
    }
    return QDialog::nativeEvent(eventType, message, result);
}

void GlWindow::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    GlUi::applyRoundedCorners(this);
}

// ------------------------------------------------------------ delegates

void GlTextDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const GlUi::Style s = GlUi::Style::current();
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(option.rect).adjusted(2, 1, -2, -1);
    if (option.state & (QStyle::State_Selected | QStyle::State_MouseOver)) {
        QColor c = s.accent;
        c.setAlphaF((option.state & QStyle::State_Selected) ? 0.35 : 0.16);
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawRoundedRect(r, 8 * s.radius + 2, 8 * s.radius + 2);
    }
    p->setFont(option.font);
    p->setPen(s.text);
    p->drawText(r.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, int(r.width() - 24)));
    p->restore();
}

void GlCheckRowDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const GlUi::Style s = GlUi::Style::current();
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRectF r = QRectF(option.rect).adjusted(2, 2, -2, -2);
    const bool checked = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
    const qreal rad = 12 * s.radius + 3;
    if (checked || (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))) {
        QColor c = s.accent;
        c.setAlphaF(checked ? 0.22 : 0.12);
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawRoundedRect(r, rad, rad);
    }
    // Checkbox.
    const QRectF box(r.left() + 12, r.center().y() - 10, 20, 20);
    const qreal br = 5 * s.radius + 2;
    if (checked) {
        QLinearGradient g(box.topLeft(), box.bottomRight());
        g.setColorAt(0, s.accent);
        g.setColorAt(1, s.accent2);
        p->setPen(Qt::NoPen);
        p->setBrush(g);
        p->drawRoundedRect(box, br, br);
        p->setPen(QPen(Qt::white, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->drawPolyline(QPolygonF({box.center() + QPointF(-5, 0), box.center() + QPointF(-1.5, 4), box.center() + QPointF(5, -4)}));
    } else {
        p->setPen(QPen(s.glassEdge.alpha() ? s.textDim : s.textDim, 1.4));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(box.adjusted(0.7, 0.7, -0.7, -0.7), br, br);
    }
    qreal x = box.right() + 12;
    const QVariant deco = index.data(Qt::DecorationRole);
    QPixmap pm;
    if (deco.canConvert<QIcon>()) {
        pm = deco.value<QIcon>().pixmap(QSize(40, 40), p->device()->devicePixelRatioF());
    }
    if (!pm.isNull()) {
        const QRectF ir(x, r.center().y() - 20, 40, 40);
        QPainterPath clip;
        clip.addRoundedRect(ir, 6, 6);
        p->save();
        p->setClipPath(clip, Qt::IntersectClip);
        p->drawPixmap(ir, pm, QRectF(QPointF(0, 0), QSizeF(pm.size())));
        p->restore();
    }
    x += 52;
    p->setFont(option.font);
    p->setPen(s.text);
    p->drawText(QRectF(x, r.top(), r.right() - x - 12, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, int(r.right() - x - 12)));
    p->restore();
}

void GlTableDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const GlUi::Style s = GlUi::Style::current();
    const QAbstractItemModel* m = index.model();
    const int row = index.row();
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(option.rect).adjusted(2, 1, -2, -1);
    const int depth = m->index(row, 0).data(kDepthRole).toInt();
    const bool checkable = m->index(row, 0).data(kCheckableRole).toBool();
    const bool checked = m->index(row, 0).data(Qt::CheckStateRole).toInt() == Qt::Checked;
    if (checked || (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))) {
        QColor c = s.accent;
        c.setAlphaF(checked ? 0.2 : ((option.state & QStyle::State_Selected) ? 0.3 : 0.12));
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawRoundedRect(r, 8 * s.radius + 2, 8 * s.radius + 2);
    }
    qreal x = r.left() + 10 + depth * 22;
    if (checkable) {
        const QRectF box(x, r.center().y() - 9, 18, 18);
        const qreal br = 4 * s.radius + 2;
        if (checked) {
            QLinearGradient g(box.topLeft(), box.bottomRight());
            g.setColorAt(0, s.accent);
            g.setColorAt(1, s.accent2);
            p->setPen(Qt::NoPen);
            p->setBrush(g);
            p->drawRoundedRect(box, br, br);
            p->setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->drawPolyline(QPolygonF({box.center() + QPointF(-4.5, 0), box.center() + QPointF(-1.2, 3.6), box.center() + QPointF(4.6, -3.6)}));
        } else {
            p->setPen(QPen(s.textDim, 1.3));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(box.adjusted(0.7, 0.7, -0.7, -0.7), br, br);
        }
        x += 28;
    }
    const int cols = m->columnCount();
    qreal total = 0;
    for (int c = 0; c < cols; ++c) {
        total += c < m_fractions.size() ? m_fractions[c] : 1.0 / std::max(1, cols);
    }
    const qreal inner = r.right() - 10 - x;
    const qreal firstShift = 0; // column 0 already starts after the checkbox
    qreal cx = x;
    p->setFont(option.font);
    for (int c = 0; c < cols; ++c) {
        const qreal frac = (c < m_fractions.size() ? m_fractions[c] : 1.0 / std::max(1, cols)) / total;
        qreal w = inner * frac - 8;
        const QModelIndex cell = m->index(row, c);
        qreal tx = cx + firstShift;
        const QVariant deco = cell.data(Qt::DecorationRole);
        if (deco.canConvert<QIcon>() && !deco.value<QIcon>().isNull()) {
            const QPixmap pm = deco.value<QIcon>().pixmap(QSize(16, 16), p->device()->devicePixelRatioF());
            p->drawPixmap(QPointF(tx, r.center().y() - 8), pm);
            tx += 22;
            w -= 22;
        }
        QColor tc = c == 0 ? s.text : (c == 1 ? s.text : s.textDim);
        const QVariant fg = cell.data(Qt::ForegroundRole);
        if (fg.canConvert<QColor>()) {
            tc = fg.value<QColor>();
        }
        p->setPen(tc);
        p->drawText(QRectF(tx, r.top(), std::max<qreal>(10, w), r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                    p->fontMetrics().elidedText(cell.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, int(std::max<qreal>(10, w))));
        cx += inner * frac;
    }
    p->restore();
}
