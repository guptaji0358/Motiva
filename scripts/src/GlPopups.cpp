#include "GlPopups.h"
#include "GlUi.h"
#include "Theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMap>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <windows.h>
#include <algorithm>

namespace {
constexpr int kRowH = 34;
constexpr int kSepH = 12;
constexpr int kPad = 6;

QColor mixc(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}
QElapsedTimer g_shown;
} // namespace

// ----------------------------------------------------------------- GlMenu

GlMenu::GlMenu(QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setMouseTracking(true);
    setAttribute(Qt::WA_AlwaysShowToolTips, false);
    m_subTimer = new QTimer(this);
    m_subTimer->setSingleShot(true);
    m_subTimer->setInterval(140);
    connect(m_subTimer, &QTimer::timeout, this, [this] { openSub(m_hover); });
}

GlMenu::GlMenu(QWidget* parent, GlMenu* parentMenu) : GlMenu(parent) {
    m_parentMenu = parentMenu;
}

GlMenu::~GlMenu() = default;

GlMenu* GlMenu::root() {
    GlMenu* m = this;
    while (m->m_parentMenu) {
        m = m->m_parentMenu;
    }
    return m;
}

QAction* GlMenu::addAction(const QString& text) {
    return addAction(QIcon(), text);
}

QAction* GlMenu::addAction(const QIcon& icon, const QString& text) {
    auto* a = new QAction(icon, text, this);
    m_entries.push_back({a, nullptr});
    return a;
}

QAction* GlMenu::addAction(QAction* existing) {
    m_entries.push_back({existing, nullptr});
    return existing;
}

void GlMenu::setEnabled(bool on) {
    if (!m_parentMenu) {
        QWidget::setEnabled(on);
        return;
    }
    for (Entry& e : m_parentMenu->m_entries) {
        if (e.sub == this) {
            e.action->setEnabled(on);
        }
    }
}

QAction* GlMenu::addSeparator() {
    auto* a = new QAction(this);
    a->setSeparator(true);
    m_entries.push_back({a, nullptr});
    return a;
}

GlMenu* GlMenu::addMenu(const QString& title) {
    auto* a = new QAction(title, this);
    auto* sub = new GlMenu(this, this);
    m_entries.push_back({a, sub});
    return sub;
}

QSize GlMenu::computeSize() const {
    const QFontMetricsF fm(GlUi::uiFont(10.5));
    qreal w = 190;
    int h = 2 * kPad;
    bool leftCol = false;
    for (const Entry& e : m_entries) {
        leftCol = leftCol || !e.action->icon().isNull() || e.action->isCheckable();
    }
    for (const Entry& e : m_entries) {
        if (e.action->isSeparator()) {
            h += kSepH;
            continue;
        }
        h += kRowH;
        qreal rw = 28 + (leftCol ? 26 : 0) + fm.horizontalAdvance(e.action->text().remove(QLatin1Char('&')));
        const QString sc = e.action->shortcut().toString(QKeySequence::NativeText);
        if (!sc.isEmpty()) {
            rw += 30 + fm.horizontalAdvance(sc);
        }
        if (e.sub) {
            rw += 24;
        }
        w = std::max(w, rw);
    }
    return QSize(int(std::min<qreal>(w, 460)) + 2 * kPad, h);
}

QRect GlMenu::rowRect(int i) const {
    int y = kPad;
    for (int k = 0; k < m_entries.size(); ++k) {
        const int hh = m_entries[k].action->isSeparator() ? kSepH : kRowH;
        if (k == i) {
            return QRect(kPad, y, width() - 2 * kPad, hh);
        }
        y += hh;
    }
    return QRect();
}

int GlMenu::rowAt(const QPoint& p) const {
    for (int i = 0; i < m_entries.size(); ++i) {
        if (rowRect(i).contains(p)) {
            return i;
        }
    }
    return -1;
}

bool GlMenu::selectable(int i) const {
    return i >= 0 && i < m_entries.size() && !m_entries[i].action->isSeparator() && m_entries[i].action->isEnabled();
}

void GlMenu::popup(const QPoint& globalPos) {
    m_result = nullptr;
    m_hover = -1;
    const QSize sz = computeSize();
    resize(sz);
    QPoint at = globalPos;
    QScreen* screen = QGuiApplication::screenAt(globalPos);
    const QRect avail = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    if (at.x() + sz.width() > avail.right()) {
        at.setX(m_parentMenu ? std::max(avail.left(), globalPos.x() - sz.width() - m_parentMenu->width() + 12)
                             : avail.right() - sz.width());
    }
    if (at.y() + sz.height() > avail.bottom()) {
        at.setY(std::max(avail.top(), avail.bottom() - sz.height()));
    }
    move(at);
    show();
    if (!m_parentMenu && !QApplication::activeWindow()) {
        SetForegroundWindow(reinterpret_cast<HWND>(winId())); // tray menus have no active window of ours
    }
    raise();
}

QAction* GlMenu::exec(const QPoint& globalPos) {
    QEventLoop loop;
    m_loop = &loop;
    popup(globalPos);
    loop.exec();
    m_loop = nullptr;
    QAction* chosen = m_result;
    if (chosen) {
        chosen->trigger();
    }
    return chosen;
}

void GlMenu::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    g_shown.start();
    GlUi::applyRoundedCorners(this);
}

void GlMenu::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    closeSub();
    m_subTimer->stop();
    if (!m_parentMenu && m_loop) {
        m_loop->quit();
    }
}

void GlMenu::closeSub() {
    if (m_openSub) {
        GlMenu* s = m_openSub;
        m_openSub = nullptr;
        s->hide();
    }
}

void GlMenu::openSub(int i) {
    if (!selectable(i) || !m_entries[i].sub) {
        return;
    }
    GlMenu* sub = m_entries[i].sub;
    if (m_openSub == sub && sub->isVisible()) {
        return;
    }
    closeSub();
    m_openSub = sub;
    sub->popup(mapToGlobal(QPoint(width() - 4, rowRect(i).top() - kPad)));
}

void GlMenu::setHover(int i, bool open) {
    if (i == m_hover) {
        return;
    }
    m_hover = i;
    if (m_openSub && !(i >= 0 && m_entries[i].sub == m_openSub)) {
        closeSub();
    }
    if (open && i >= 0 && m_entries[i].sub && selectable(i)) {
        m_subTimer->start();
    } else {
        m_subTimer->stop();
    }
    update();
}

void GlMenu::choose(int i) {
    if (!selectable(i)) {
        return;
    }
    if (m_entries[i].sub) {
        openSub(i);
        return;
    }
    GlMenu* r = root();
    r->m_result = m_entries[i].action;
    QAction* a = m_entries[i].action;
    const bool blocking = r->m_loop != nullptr;
    r->hide();
    if (!blocking) {
        QTimer::singleShot(0, a, [a] { a->trigger(); });
    }
}

void GlMenu::step(int dir) {
    if (m_entries.isEmpty()) {
        return;
    }
    int i = m_hover;
    for (int n = 0; n < m_entries.size(); ++n) {
        i = i < 0 ? (dir > 0 ? 0 : m_entries.size() - 1) : (i + dir + m_entries.size()) % m_entries.size();
        if (selectable(i)) {
            setHover(i, false);
            return;
        }
    }
}

void GlMenu::mouseMoveEvent(QMouseEvent* e) {
    const int i = rowAt(e->pos());
    setHover(selectable(i) || i < 0 ? i : -1, true);
    // A submenu row stays lit while its list is open.
}

void GlMenu::mousePressEvent(QMouseEvent* e) {
    if (!rect().contains(e->pos())) {
        QWidget::mousePressEvent(e); // outside: Qt closes the popup
    }
}

void GlMenu::mouseReleaseEvent(QMouseEvent* e) {
    if (g_shown.isValid() && g_shown.elapsed() < 220 && e->pos() == QPoint()) {
        return;
    }
    const int i = rowAt(e->pos());
    if (i >= 0) {
        choose(i);
    }
}

void GlMenu::leaveEvent(QEvent* e) {
    QWidget::leaveEvent(e);
    if (!m_openSub) {
        setHover(-1, false);
    }
}

void GlMenu::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_Escape:
        hide();
        break;
    case Qt::Key_Up:
        step(-1);
        break;
    case Qt::Key_Down:
        step(+1);
        break;
    case Qt::Key_Right:
        if (m_hover >= 0 && m_entries[m_hover].sub) {
            openSub(m_hover);
            if (m_openSub) {
                m_openSub->step(+1);
            }
        }
        break;
    case Qt::Key_Left:
        if (m_parentMenu) {
            hide();
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        choose(m_hover);
        break;
    default:
        QWidget::keyPressEvent(e);
    }
}

void GlMenu::paintEvent(QPaintEvent*) {
    const GlUi::Style s = GlUi::Style::current();
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), mixc(s.bgBase, s.bgTint, 0.5));
    p.fillRect(rect(), s.glass);
    QLinearGradient rim(0, 0, width(), height());
    QColor a = s.accent, b = s.accent2;
    a.setAlphaF(0.7);
    b.setAlphaF(0.45);
    rim.setColorAt(0, a);
    rim.setColorAt(1, b);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(rim), 1.2));
    p.drawRect(QRectF(0.6, 0.6, width() - 1.2, height() - 1.2));

    bool leftCol = false;
    for (const Entry& e : m_entries) {
        leftCol = leftCol || !e.action->icon().isNull() || e.action->isCheckable();
    }
    const qreal rowRad = std::max(4.0, 10 * s.radius);
    const QFont font = GlUi::uiFont(10.5);
    p.setFont(font);
    for (int i = 0; i < m_entries.size(); ++i) {
        QAction* act = m_entries[i].action;
        const QRect r = rowRect(i);
        if (act->isSeparator()) {
            p.setPen(QPen(s.glassEdge, 1));
            p.drawLine(r.left() + 10, r.center().y(), r.right() - 10, r.center().y());
            continue;
        }
        const bool on = act->isEnabled();
        const bool hot = i == m_hover && on;
        if (hot) {
            QLinearGradient g(r.topLeft(), r.topRight());
            QColor c1 = s.accent, c2 = s.accent2;
            c1.setAlphaF(0.5);
            c2.setAlphaF(0.32);
            g.setColorAt(0, c1);
            g.setColorAt(1, c2);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(QRectF(r), rowRad, rowRad);
        }
        QColor fg = on ? s.text : s.textDim;
        if (!on) {
            fg.setAlphaF(0.55);
        }
        qreal x = r.left() + 14;
        if (leftCol) {
            if (act->isCheckable() && act->isChecked()) {
                p.setPen(QPen(s.accent2, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                const QPointF c(x + 6, r.center().y());
                p.drawPolyline(QPolygonF({QPointF(c.x() - 5, c.y()), QPointF(c.x() - 1, c.y() + 4), QPointF(c.x() + 6, c.y() - 5)}));
            } else if (!act->icon().isNull()) {
                QPixmap pm = act->icon().pixmap(QSize(18, 18), devicePixelRatioF());
                QPainter tp(&pm);
                tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
                tp.fillRect(pm.rect(), fg);
                tp.end();
                p.drawPixmap(QPointF(x, r.center().y() - 9), pm);
            }
            x += 26;
        }
        p.setPen(fg);
        const QString text = act->text().remove(QLatin1Char('&'));
        const QString sc = act->shortcut().toString(QKeySequence::NativeText);
        const qreal rightPad = m_entries[i].sub ? 26 : 12;
        p.drawText(QRectF(x, r.top(), r.right() - x - rightPad, r.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
        if (!sc.isEmpty()) {
            p.setPen(on ? s.textDim : fg);
            p.drawText(QRectF(x, r.top(), r.right() - x - rightPad, r.height()), Qt::AlignVCenter | Qt::AlignRight, sc);
        }
        if (m_entries[i].sub) {
            p.setPen(QPen(fg, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF c(r.right() - 14, r.center().y());
            p.drawPolyline(QPolygonF({QPointF(c.x() - 2, c.y() - 4), QPointF(c.x() + 3, c.y()), QPointF(c.x() - 2, c.y() + 4)}));
        }
    }
}

// ------------------------------------------------------------ GlMessageBox

namespace {
QString standardText(QMessageBox::StandardButton b) {
    switch (b) {
    case QMessageBox::Ok: return QObject::tr("OK");
    case QMessageBox::Cancel: return QObject::tr("Cancel");
    case QMessageBox::Yes: return QObject::tr("Yes");
    case QMessageBox::No: return QObject::tr("No");
    case QMessageBox::Save: return QObject::tr("Save");
    case QMessageBox::Discard: return QObject::tr("Discard");
    case QMessageBox::Retry: return QObject::tr("Retry");
    case QMessageBox::Close: return QObject::tr("Close");
    case QMessageBox::Abort: return QObject::tr("Abort");
    case QMessageBox::Ignore: return QObject::tr("Ignore");
    case QMessageBox::Apply: return QObject::tr("Apply");
    case QMessageBox::Open: return QObject::tr("Open");
    default: return QObject::tr("OK");
    }
}
QMessageBox::ButtonRole standardRole(QMessageBox::StandardButton b) {
    switch (b) {
    case QMessageBox::Cancel:
    case QMessageBox::Close:
    case QMessageBox::No:
    case QMessageBox::Abort:
        return QMessageBox::RejectRole;
    case QMessageBox::Discard:
        return QMessageBox::DestructiveRole;
    case QMessageBox::Yes:
        return QMessageBox::YesRole;
    default:
        return QMessageBox::AcceptRole;
    }
}
bool isRejecting(QMessageBox::ButtonRole r) {
    return r == QMessageBox::RejectRole || r == QMessageBox::NoRole;
}
} // namespace

GlMessageBox::GlMessageBox(Icon icon, const QString& title, const QString& text, QWidget* parent)
    : QDialog(parent), m_text(text), m_icon(icon) {
    setWindowTitle(title);
    setWindowFlag(Qt::FramelessWindowHint, true);
    buildChrome(title, text);
}

void GlMessageBox::buildChrome(const QString& title, const QString& text) {
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    m_canvas = new GlCanvas(this);
    box->addWidget(m_canvas);
    m_canvas->setAccessibleName(title);
    m_canvas->setLayoutCallback([this](const QSizeF& s) { layoutGl(s); });
    connect(this, &QWidget::windowTitleChanged, this, [this](const QString& t) {
        if (m_titleLabel) m_titleLabel->setText(t);
    });
    m_titlePanel = new GlPanel(m_canvas);
    m_titlePanel->setBlock(m_icon == Icon::Warning || m_icon == Icon::Critical ? GlPanel::Block::Warning
                                                                              : GlPanel::Block::Accent);
    m_titlePanel->setRadiusPx(16);
    m_bodyPanel = new GlPanel(m_canvas);
    m_bodyPanel->setRadiusPx(20);
    m_titleLabel = new GlLabel(m_canvas);
    m_titleLabel->setText(title);
    m_titleLabel->setPointSize(13);
    m_titleLabel->setBold(true);
    m_textLabel = new GlLabel(m_canvas);
    m_textLabel->setText(text);
    m_textLabel->setPointSize(10.5);
    m_textLabel->setWordWrap(true);
    m_textLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
}

static GlMessageBox::Icon fromQt(QMessageBox::Icon i) {
    switch (i) {
    case QMessageBox::Information: return GlMessageBox::Icon::Information;
    case QMessageBox::Question: return GlMessageBox::Icon::Question;
    case QMessageBox::Warning: return GlMessageBox::Icon::Warning;
    case QMessageBox::Critical: return GlMessageBox::Icon::Critical;
    default: return GlMessageBox::Icon::None;
    }
}

GlMessageBox::GlMessageBox(QMessageBox::Icon icon, const QString& title, const QString& text, QWidget* parent)
    : GlMessageBox(fromQt(icon), title, text, parent) {}

GlMessageBox::GlMessageBox(QMessageBox::Icon icon, const QString& title, const QString& text,
                           QMessageBox::StandardButtons buttons, QWidget* parent)
    : GlMessageBox(fromQt(icon), title, text, parent) {
    static const QMessageBox::StandardButton order[] = {QMessageBox::Save, QMessageBox::Yes, QMessageBox::Ok,
                                                        QMessageBox::Discard, QMessageBox::No, QMessageBox::Cancel};
    for (QMessageBox::StandardButton b : order) {
        if (buttons & b) {
            addButton(b);
        }
    }
}

GlMessageBox::GlMessageBox(QWidget* parent) : GlMessageBox(Icon::None, QString(), QString(), parent) {}

void GlMessageBox::setIcon(QMessageBox::Icon icon) {
    m_icon = fromQt(icon);
    m_titlePanel->setBlock(m_icon == Icon::Warning || m_icon == Icon::Critical ? GlPanel::Block::Warning
                                                                              : GlPanel::Block::Accent);
}

void GlMessageBox::setText(const QString& text) {
    m_text = text;
    m_textLabel->setText(text);
}

void GlMessageBox::setInformativeText(const QString& info) {
    if (!info.isEmpty()) {
        m_text += QStringLiteral("\n\n") + info;
        m_textLabel->setText(m_text);
    }
}

GlMessageBox::Button* GlMessageBox::addButton(const QString& text, QMessageBox::ButtonRole role) {
    auto* handle = new Button(this);
    handle->id = m_nextId++;
    auto* item = new GlButton(m_canvas, GlButton::Kind::Secondary);
    item->setText(text);
    m_slots.push_back({handle, item, role, QMessageBox::NoButton});
    const int idx = m_slots.size() - 1;
    connect(item, &GlButton::clicked, this, [this, idx] { finish(m_slots[idx]); });
    return handle;
}

GlMessageBox::Button* GlMessageBox::addButton(QMessageBox::StandardButton b) {
    Button* h = addButton(standardText(b), standardRole(b));
    m_slots.last().std = b;
    return h;
}

void GlMessageBox::finish(Slot& s) {
    m_clicked = s.handle;
    m_clickedStd = s.std;
    if (isRejecting(s.role)) {
        reject();
    } else {
        accept();
    }
}

QSize GlMessageBox::wantedSize() const {
    const int W = 500;
    const QFontMetricsF fm(GlUi::uiFont(10.5));
    const qreal textH = fm.boundingRect(QRectF(0, 0, W - 24 - 56, 10000), Qt::TextWordWrap | Qt::AlignLeft, m_text).height();
    qreal h = 12 + 46 + 10 + 24 + std::max<qreal>(textH, 22) + 20 + 44 + 22 + 12;
    if (m_input || m_choice) {
        h += 62;
    }
    return QSize(W, int(std::ceil(h)));
}

void GlMessageBox::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    // Buttons: accepting ones first, the rejecting one last (right-most).
    std::stable_sort(m_slots.begin(), m_slots.end(), [](const Slot& a, const Slot& b) {
        return !isRejecting(a.role) && isRejecting(b.role);
    });
    // The re-sorted slots need fresh click wiring by identity.
    for (int i = 0; i < m_slots.size(); ++i) {
        m_slots[i].item->disconnect(this);
        connect(m_slots[i].item, &GlButton::clicked, this, [this, i] { finish(m_slots[i]); });
        const bool def = (m_default && m_slots[i].handle == m_default) ||
                         (!m_default && i == 0 && !isRejecting(m_slots[i].role));
        m_slots[i].item->setKind(def ? GlButton::Kind::Primary : GlButton::Kind::Secondary);
    }
    setFixedSize(wantedSize());
    GlUi::applyRoundedCorners(this);
    m_canvas->relayout();
    if (m_input) {
        m_input->focusInput(true);
    }
}

void GlMessageBox::layoutGl(const QSizeF& size) {
    const qreal W = size.width(), H = size.height(), m = 12;
    const QRectF strip(m, m, W - 2 * m, 46);
    m_titlePanel->setRect(strip);
    m_titleLabel->setRect(QRectF(strip.left() + 20, strip.top() + 8, strip.width() - 40, 30));
    const QRectF body(m, strip.bottom() + 10, W - 2 * m, H - m - (strip.bottom() + 10));
    m_bodyPanel->setRect(body);
    const QFontMetricsF fm(GlUi::uiFont(10.5));
    const qreal textH = std::max<qreal>(22, fm.boundingRect(QRectF(0, 0, body.width() - 56, 10000),
                                                           Qt::TextWordWrap | Qt::AlignLeft, m_text).height());
    m_textLabel->setRect(QRectF(body.left() + 28, body.top() + 24, body.width() - 56, textH + 4));
    qreal y = body.top() + 24 + textH + 12;
    if (m_input) {
        m_input->setRect(QRectF(body.left() + 28, y, body.width() - 56, 38));
        y += 50;
    }
    if (m_choice) {
        m_choice->setRect(QRectF(body.left() + 28, y, body.width() - 56, 38));
        y += 50;
    }
    // Buttons right-aligned along the bottom.
    qreal x = body.right() - 22;
    const qreal by = body.bottom() - 22 - 40;
    for (int i = int(m_slots.size()) - 1; i >= 0; --i) {
        const qreal w = std::max<qreal>(104, QFontMetricsF(GlUi::uiFont(10, true)).horizontalAdvance(m_slots[i].item->text()) + 44);
        m_slots[i].item->setRect(QRectF(x - w, by, w, 40));
        x -= w + 10;
    }
}

bool GlMessageBox::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG" && m_titlePanel &&
        GlUi::handleFrameHitTest(this, m_canvas, m_titlePanel->rect(), message, result, /*resizable=*/false)) {
        return true;
    }
    return QDialog::nativeEvent(eventType, message, result);
}

void GlMessageBox::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        for (Slot& s : m_slots) {
            if (m_escape && s.handle == m_escape) {
                finish(s);
                return;
            }
        }
        for (Slot& s : m_slots) {
            if (isRejecting(s.role)) {
                finish(s);
                return;
            }
        }
        if (m_slots.size() == 1) {
            finish(m_slots[0]);
            return;
        }
        reject();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        for (Slot& s : m_slots) {
            if (m_default ? s.handle == m_default : !isRejecting(s.role)) {
                finish(s);
                return;
            }
        }
    }
    QDialog::keyPressEvent(e);
}

static QMessageBox::StandardButton runStandard(GlMessageBox::Icon icon, QWidget* parent, const QString& title,
                                               const QString& text, QMessageBox::StandardButtons buttons,
                                               QMessageBox::StandardButton def) {
    GlMessageBox box(icon, title, text, parent);
    static const QMessageBox::StandardButton order[] = {
        QMessageBox::Save, QMessageBox::Yes, QMessageBox::Apply, QMessageBox::Open, QMessageBox::Retry, QMessageBox::Ok,
        QMessageBox::Discard, QMessageBox::Ignore, QMessageBox::No, QMessageBox::Close, QMessageBox::Abort,
        QMessageBox::Cancel};
    QMap<QMessageBox::StandardButton, GlMessageBox::Button*> made;
    for (QMessageBox::StandardButton b : order) {
        if (buttons & b) {
            made.insert(b, box.addButton(b));
        }
    }
    if (made.isEmpty()) {
        made.insert(QMessageBox::Ok, box.addButton(QMessageBox::Ok));
    }
    if (made.contains(def)) {
        box.setDefaultButton(made.value(def));
    }
    box.exec();
    if (box.clickedStandardButton() != QMessageBox::NoButton) {
        return box.clickedStandardButton();
    }
    return made.contains(QMessageBox::Cancel) ? QMessageBox::Cancel : made.firstKey();
}

QMessageBox::StandardButton GlMessageBox::question(QWidget* parent, const QString& title, const QString& text,
                                                   QMessageBox::StandardButtons buttons,
                                                   QMessageBox::StandardButton def) {
    return runStandard(Icon::Question, parent, title, text, buttons, def);
}
QMessageBox::StandardButton GlMessageBox::information(QWidget* parent, const QString& title, const QString& text,
                                                      QMessageBox::StandardButtons buttons,
                                                      QMessageBox::StandardButton def) {
    return runStandard(Icon::Information, parent, title, text, buttons, def);
}
QMessageBox::StandardButton GlMessageBox::warning(QWidget* parent, const QString& title, const QString& text,
                                                  QMessageBox::StandardButtons buttons,
                                                  QMessageBox::StandardButton def) {
    return runStandard(Icon::Warning, parent, title, text, buttons, def);
}
QMessageBox::StandardButton GlMessageBox::critical(QWidget* parent, const QString& title, const QString& text,
                                                   QMessageBox::StandardButtons buttons,
                                                   QMessageBox::StandardButton def) {
    return runStandard(Icon::Critical, parent, title, text, buttons, def);
}

QString GlMessageBox::getText(QWidget* parent, const QString& title, const QString& label, const QString& text,
                              bool* ok) {
    return getText(parent, title, label, text, QString(), QString(), ok);
}

QString GlMessageBox::getText(QWidget* parent, const QString& title, const QString& label, const QString& text,
                              const QString& okText, const QString& placeholder, bool* ok) {
    GlMessageBox box(Icon::Question, title, label, parent);
    box.m_input = new GlTextInput(box.m_canvas);
    box.m_input->setText(text);
    box.m_input->setPlaceholder(placeholder);
    Button* okBtn = okText.isEmpty() ? box.addButton(QMessageBox::Ok)
                                     : box.addButton(okText, QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(okBtn);
    // Enter inside the field commits like pressing OK.
    QObject::connect(box.m_input, &GlTextInput::editingFinished, &box, [&box] { box.finish(box.m_slots.first()); });
    QObject::connect(box.m_input, &GlTextInput::cancelled, &box, [&box] { box.reject(); });
    box.m_input->setConsumeEscape(true);
    const bool accepted = box.exec() == QDialog::Accepted;
    if (ok) {
        *ok = accepted;
    }
    return accepted ? box.m_input->text() : QString();
}

QString GlMessageBox::getItem(QWidget* parent, const QString& title, const QString& label, const QStringList& items,
                              int current, bool* ok) {
    GlMessageBox box(Icon::Question, title, label, parent);
    box.m_choice = new GlDropdown(box.m_canvas);
    box.m_choice->setCompact(true);
    box.m_choice->addItems(items);
    box.m_choice->setCurrentIndex(std::max(0, current));
    Button* okBtn = box.addButton(QMessageBox::Ok);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(okBtn);
    const bool accepted = box.exec() == QDialog::Accepted;
    if (ok) {
        *ok = accepted;
    }
    return accepted && box.m_choice->currentIndex() >= 0 ? items.value(box.m_choice->currentIndex()) : QString();
}
