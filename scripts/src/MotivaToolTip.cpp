#include "MotivaToolTip.h"
#include "Theme.h"

#include <QAbstractItemView>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTextOption>
#include <QtMath>

namespace {
constexpr int kShadow = 6; // transparent margin the soft shadow is drawn into
constexpr int kPadX = 10;
constexpr int kPadY = 6;
constexpr int kMaxTextWidth = 340;
constexpr int kCursorOffsetX = 12;
constexpr int kCursorOffsetY = 20;
constexpr int kAuroraRadius = 8; // mirrors Theme.cpp's per-family corner language
constexpr int kOnyxRadius = 3;

bool isOnyx(Theme::AppTheme t) {
    return t == Theme::AppTheme::DarkOnyx || t == Theme::AppTheme::LightOnyx;
}

MotivaToolTip* g_instance = nullptr;
} // namespace

void MotivaToolTip::install(QApplication* app) {
    if (g_instance || !app) {
        return;
    }
    g_instance = new MotivaToolTip();
    app->installEventFilter(g_instance);
}

MotivaToolTip::MotivaToolTip()
    : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint |
                           Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setObjectName(QStringLiteral("motivaToolTip"));

    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); // long file paths
    m_doc.setDefaultTextOption(option);
    m_doc.setDocumentMargin(0);

    m_hideTimer.setSingleShot(true);
    connect(&m_hideTimer, &QTimer::timeout, this, [this] { hideTip(); });
}

QString MotivaToolTip::textFor(QWidget* widget, const QPoint& localPos) {
    if (!widget) {
        return {};
    }
    // Item-view rows and graphics-view items deliver the ToolTip event to the
    // viewport, whose own toolTip() is empty - resolve the row/item text the
    // way the view itself would.
    if (auto* view = qobject_cast<QAbstractItemView*>(widget->parentWidget());
        view && view->viewport() == widget) {
        const QModelIndex index = view->indexAt(localPos);
        return index.isValid() ? index.data(Qt::ToolTipRole).toString() : QString();
    }
    if (auto* gv = qobject_cast<QGraphicsView*>(widget->parentWidget()); gv && gv->viewport() == widget) {
        for (QGraphicsItem* item = gv->itemAt(localPos); item; item = item->parentItem()) {
            if (!item->toolTip().isEmpty()) {
                return item->toolTip();
            }
        }
        return {};
    }
    return widget->toolTip();
}

bool MotivaToolTip::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::ToolTip: {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget || widget == this || !widget->isVisible()) {
            break;
        }
        auto* help = static_cast<QHelpEvent*>(event);
        const QString text = textFor(widget, help->pos());
        if (text.isEmpty()) {
            hideTip();
            return false; // nothing to show; let Qt do whatever it normally would
        }
        showTip(text, help->globalPos(), widget);
        return true;
    }
    case QEvent::MouseMove:
        // Moving to another row/item inside the same view: drop a stale tip.
        if (isVisible() && m_anchor && watched == m_anchor) {
            const auto* me = static_cast<QMouseEvent*>(event);
            if (textFor(m_anchor, me->position().toPoint()) != m_text) {
                hideTip();
            }
        }
        break;
    case QEvent::Leave:
        if (watched == m_anchor) {
            hideTip();
        }
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::KeyPress:
    case QEvent::Wheel:
    case QEvent::FocusOut:
    case QEvent::WindowDeactivate:
    case QEvent::ApplicationDeactivate:
        if (isVisible()) {
            hideTip();
        }
        break;
    case QEvent::Hide:
    case QEvent::Close:
        if (watched == m_anchor) {
            hideTip();
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void MotivaToolTip::showTip(const QString& text, const QPoint& globalPos, QWidget* anchor) {
    m_anchor = anchor;
    m_text = text;
    if (Qt::mightBeRichText(text)) {
        m_doc.setHtml(text);
    } else {
        m_doc.setPlainText(text);
    }
    m_doc.setDefaultFont(QApplication::font("QTipLabel"));
    m_doc.setTextWidth(kMaxTextWidth);
    m_doc.setTextWidth(qMin<qreal>(kMaxTextWidth, m_doc.idealWidth())); // shrink to the text

    const QSizeF docSize = m_doc.size();
    const QSize card(qCeil(docSize.width()) + 2 * kPadX, qCeil(docSize.height()) + 2 * kPadY);
    resize(card.width() + 2 * kShadow, card.height() + 2 * kShadow);

    // Below-right of the pointer, flipped to stay on the screen it is on.
    const QScreen* screen = QGuiApplication::screenAt(globalPos);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    int x = globalPos.x() + kCursorOffsetX - kShadow;
    int y = globalPos.y() + kCursorOffsetY - kShadow;
    if (x + width() > avail.right()) {
        x = avail.right() - width() + 1;
    }
    if (y + height() > avail.bottom()) {
        y = globalPos.y() - height() - 6 + kShadow;
    }
    move(qMax(x, avail.left()), qMax(y, avail.top()));

    m_hideTimer.start(qBound(4000, 2500 + int(text.size()) * 50, 20000));
    update();
    if (!isVisible()) {
        show();
    }
}

void MotivaToolTip::hideTip() {
    m_hideTimer.stop();
    m_anchor.clear();
    m_text.clear();
    if (isVisible()) {
        hide();
    }
}

void MotivaToolTip::paintEvent(QPaintEvent*) {
    const Theme::AppTheme theme = Theme::currentTheme();
    const Theme::ThemePalette& pal = Theme::themePalette(theme);
    const qreal radius = isOnyx(theme) ? kOnyxRadius : kAuroraRadius;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF cardRect = QRectF(rect()).adjusted(kShadow, kShadow, -kShadow, -kShadow);

    // Restrained shadow: a few expanding hairline rings with falling alpha.
    // Dark Aurora tints it faintly with the accent (a soft glow); the other
    // themes stay a neutral shadow.
    const QColor shadowBase = (!isOnyx(theme) && pal.isDark) ? pal.accent : QColor(0, 0, 0);
    p.setBrush(Qt::NoBrush);
    for (int i = 1; i <= kShadow; ++i) {
        const qreal falloff = 1.0 - qreal(i) / (kShadow + 1);
        QColor c = shadowBase;
        c.setAlphaF((pal.isDark ? 0.20 : 0.12) * falloff * falloff);
        p.setPen(QPen(c, 1));
        p.drawRoundedRect(cardRect.adjusted(-i + 0.5, -i + 0.5, i - 0.5, i - 0.5), radius + i, radius + i);
    }

    p.setPen(QPen(pal.borderStrong, 1));
    p.setBrush(pal.panelBg);
    p.drawRoundedRect(cardRect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

    p.translate(cardRect.left() + kPadX, cardRect.top() + kPadY);
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, pal.textPrimary);
    m_doc.documentLayout()->draw(&p, ctx);
}
