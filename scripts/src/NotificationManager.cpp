#include "NotificationManager.h"

#include "SettingsManager.h"
#include "Theme.h"

#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <exception>
#include <functional>

namespace {

constexpr int kMaxVisibleToasts = 3;
constexpr int kMaxQueuedToasts = 8;
constexpr int kToastWidth = 360;
constexpr int kEdgeMargin = 16;
constexpr int kToastGap = 8;
constexpr int kPadX = 12;
constexpr int kPadY = 10;
constexpr int kIconSize = 18;
constexpr int kCloseSize = 12;
constexpr int kAccentWidth = 3;
constexpr int kMaxMessageLines = 3;
constexpr int kHoverResumeMs = 2000;
constexpr int kFloodWindowMs = 10000;
constexpr int kFloodLimit = 8;

bool isOnyx(Theme::AppTheme t) {
    return t == Theme::AppTheme::DarkOnyx || t == Theme::AppTheme::LightOnyx;
}

int defaultDuration(Notification::Kind kind) {
    switch (kind) {
    case Notification::Kind::Error:
        return 9000;
    case Notification::Kind::Warning:
        return 7000;
    default:
        return 4500;
    }
}

int defaultCooldown(Notification::Kind kind) {
    return (kind == Notification::Kind::Error || kind == Notification::Kind::Warning) ? 30000 : 1500;
}

const char* statusColor(Notification::Kind kind) {
    switch (kind) {
    case Notification::Kind::Success:
        return Theme::kStatusSuccess;
    case Notification::Kind::Warning:
        return Theme::kStatusWarning;
    case Notification::Kind::Error:
        return Theme::kStatusError;
    default:
        return Theme::kStatusNeutral;
    }
}

const char* statusIconPath(Notification::Kind kind) {
    switch (kind) {
    case Notification::Kind::Success:
        return ":/status/check.svg";
    case Notification::Kind::Warning:
        return ":/status/warning.svg";
    case Notification::Kind::Error:
        return ":/status/cross.svg";
    default:
        return ":/status/info.svg";
    }
}

} // namespace

// ------------------------------------------------------------- one toast

// A single toast. Painted by hand from the active theme at paint time (like
// MotivaToolTip), so a theme change needs no restyling, and it owns its own
// dismissal timer - nothing outlives the widget.
class ToastWidget : public QWidget {
public:
    ToastWidget(const Notification& n, int durationMs, QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_NoSystemBackground, false);
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);
        m_timer.setSingleShot(true);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { dismiss(); });
        setContent(n, durationMs);
    }

    void setContent(const Notification& n, int durationMs) {
        m_n = n;
        m_durationMs = durationMs;
        setAccessibleName(n.title);
        setAccessibleDescription(n.message);
        setToolTip(QString()); // the text is fully visible; no tooltip needed
        restartTimer();
        updateGeometryFor(width() > 0 ? width() : kToastWidth);
        update();
    }

    const QString& key() const { return m_n.key; }
    Notification::Kind kind() const { return m_n.kind; }

    // Called by the host once it knows the width; sets the height.
    void updateGeometryFor(int width) {
        setFixedSize(width, heightFor(width));
    }

    std::function<void(ToastWidget*)> onDismiss;

    void dismiss() {
        m_timer.stop();
        if (onDismiss) {
            auto cb = std::move(onDismiss);
            cb(this);
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const Theme::AppTheme theme = Theme::currentTheme();
        const Theme::ThemePalette& pal = Theme::themePalette(theme);
        const QColor accent{QLatin1String(statusColor(m_n.kind))};
        const int radius = isOnyx(theme) ? 3 : 8;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath body;
        body.addRoundedRect(r, radius, radius);
        p.fillPath(body, pal.panelBg);
        p.setClipPath(body);
        p.fillRect(QRectF(r.left(), r.top(), kAccentWidth, r.height()), accent);
        p.setClipping(false);
        p.setPen(QPen(pal.borderStrong, 1));
        p.drawPath(body);

        const int textLeft = kAccentWidth + kPadX + kIconSize + 10;
        const int textRight = width() - kPadX - kCloseSize - 8;
        p.drawPixmap(kAccentWidth + kPadX, kPadY + 1,
                     Theme::tintedIcon(QLatin1String(statusIconPath(m_n.kind)), accent, kIconSize));
        p.drawPixmap(width() - kPadX - kCloseSize, kPadY + 3,
                     Theme::tintedIcon(QStringLiteral(":/status/cross.svg"), pal.textSecondary, kCloseSize));

        QFont titleFont = font();
        titleFont.setBold(true);
        p.setFont(titleFont);
        p.setPen(pal.textPrimary);
        const QFontMetrics tfm(titleFont);
        p.drawText(QRect(textLeft, kPadY, textRight - textLeft, tfm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   tfm.elidedText(m_n.title, Qt::ElideRight, textRight - textLeft));
        if (!m_n.message.isEmpty()) {
            p.setFont(font());
            p.setPen(pal.textSecondary);
            const QFontMetrics mfm(font());
            const QRect box(textLeft, kPadY + tfm.height() + 2, textRight - textLeft,
                            mfm.lineSpacing() * kMaxMessageLines);
            p.drawText(box, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                       elidedMessage(mfm, box.width()));
        }
    }

    void mousePressEvent(QMouseEvent* e) override {
        // A click anywhere on the toast dismisses it - it never needs to be
        // aimed at the small close mark.
        if (e->button() == Qt::LeftButton || e->button() == Qt::RightButton) {
            dismiss();
            e->accept();
        }
    }
    void enterEvent(QEnterEvent*) override { m_timer.stop(); } // hovering = still reading
    void leaveEvent(QEvent*) override { m_timer.start(kHoverResumeMs); }

private:
    void restartTimer() { m_timer.start(m_durationMs); }

    QString elidedMessage(const QFontMetrics& fm, int width) const {
        // Word-wrapped to at most kMaxMessageLines; the last line is elided.
        const QRect full = fm.boundingRect(QRect(0, 0, width, 100000), Qt::TextWordWrap, m_n.message);
        if (full.height() <= fm.lineSpacing() * kMaxMessageLines) {
            return m_n.message;
        }
        QString text = m_n.message;
        while (text.size() > 4) {
            text.chop(std::max(1, int(text.size() / 12)));
            const QString candidate = text.trimmed() + QStringLiteral("...");
            if (fm.boundingRect(QRect(0, 0, width, 100000), Qt::TextWordWrap, candidate).height() <=
                fm.lineSpacing() * kMaxMessageLines) {
                return candidate;
            }
        }
        return text;
    }

    int heightFor(int width) const {
        QFont titleFont = font();
        titleFont.setBold(true);
        const QFontMetrics tfm(titleFont);
        const QFontMetrics mfm(font());
        int h = kPadY + tfm.height() + kPadY;
        if (!m_n.message.isEmpty()) {
            const int textWidth = width - (kAccentWidth + kPadX + kIconSize + 10) - (kPadX + kCloseSize + 8);
            const QRect r = mfm.boundingRect(QRect(0, 0, textWidth, 100000), Qt::TextWordWrap, m_n.message);
            h += 2 + std::min(r.height(), mfm.lineSpacing() * kMaxMessageLines);
        }
        return std::max(h, kPadY * 2 + kIconSize);
    }

    Notification m_n;
    int m_durationMs = 0;
    QTimer m_timer;
};

// --------------------------------------------------------------- the host

// Overlay that stacks toasts at the top-right of one window. It is itself
// only as big as the toast column (so it never intercepts clicks meant for
// the window underneath) and re-anchors on window resize.
class ToastHost : public QWidget {
public:
    ToastHost(QWidget* window, int topInset) : QWidget(window), m_window(window), m_topInset(topInset) {
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        hide();
        window->installEventFilter(this);
    }

    QWidget* window() const { return m_window; }
    int visibleCount() const { return int(m_toasts.size()); }

    void present(const Notification& n, int durationMs) {
        for (ToastWidget* t : std::as_const(m_toasts)) {
            if (!n.key.isEmpty() && t->key() == n.key) {
                t->setContent(n, durationMs); // same event: update in place
                relayout();
                return;
            }
        }
        if (m_toasts.size() >= kMaxVisibleToasts) {
            // Keep a bounded backlog; the least important waiting entries
            // go first so a burst can't build an unbounded queue.
            if (m_queue.size() >= kMaxQueuedToasts) {
                auto it = std::find_if(m_queue.begin(), m_queue.end(), [](const Queued& q) {
                    return q.n.kind == Notification::Kind::Info || q.n.kind == Notification::Kind::Success;
                });
                m_queue.erase(it != m_queue.end() ? it : m_queue.begin());
            }
            m_queue.push_back({n, durationMs});
            return;
        }
        addToast(n, durationMs);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_window && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
            relayout();
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    struct Queued {
        Notification n;
        int durationMs;
    };

    void addToast(const Notification& n, int durationMs) {
        auto* toast = new ToastWidget(n, durationMs, this);
        toast->onDismiss = [this](ToastWidget* t) { remove(t); };
        m_toasts.push_back(toast);
        relayout();
        toast->show();
        QWidget::show();
        raise();
    }

    void remove(ToastWidget* toast) {
        m_toasts.removeOne(toast);
        toast->hide();
        toast->deleteLater();
        if (!m_queue.isEmpty() && m_toasts.size() < kMaxVisibleToasts) {
            const Queued q = m_queue.takeFirst();
            addToast(q.n, q.durationMs);
        } else {
            relayout();
        }
        if (m_toasts.isEmpty()) {
            QWidget::hide();
        }
    }

    void relayout() {
        const int width = std::max(160, std::min(kToastWidth, m_window->width() - 2 * kEdgeMargin));
        int y = 0;
        for (ToastWidget* t : std::as_const(m_toasts)) {
            t->updateGeometryFor(width);
            t->move(0, y);
            y += t->height() + kToastGap;
        }
        const int height = std::max(0, y - kToastGap);
        setGeometry(m_window->width() - width - kEdgeMargin, m_topInset + kEdgeMargin / 2, width, height);
    }

    QWidget* m_window;
    int m_topInset;
    QList<ToastWidget*> m_toasts;
    QList<Queued> m_queue;
};

// ------------------------------------------------------------ the manager

NotificationManager* NotificationManager::instance() {
    static NotificationManager* s = new NotificationManager(qApp);
    return s;
}

NotificationManager::NotificationManager(QObject* parent) : QObject(parent) {
    m_clock.start();
}

void NotificationManager::setSettings(const SettingsManager* settings) {
    m_settings = settings;
}

void NotificationManager::setTrayIcon(QSystemTrayIcon* tray) {
    m_tray = tray;
}

void NotificationManager::attachHost(QWidget* window, int topInset) {
    if (!window) {
        return;
    }
    for (const auto& h : std::as_const(m_hosts)) {
        if (h && h->window() == window) {
            return;
        }
    }
    m_hosts.push_back(new ToastHost(window, topInset)); // owned by `window`
}

void NotificationManager::shutdown() {
    m_settings = nullptr;
    m_tray = nullptr;
    m_hosts.clear();
    m_recent.clear();
}

int NotificationManager::visibleToastCount() const {
    int n = 0;
    for (const auto& h : m_hosts) {
        n += h ? h->visibleCount() : 0;
    }
    return n;
}

bool NotificationManager::categoryAllowed(const Notification& n) const {
    if (!m_settings) {
        return true;
    }
    switch (n.kind) {
    case Notification::Kind::Success:
        if (!m_settings->notifySuccess()) {
            return false;
        }
        break;
    case Notification::Kind::Error:
        if (!m_settings->notifyError()) {
            return false;
        }
        break;
    case Notification::Kind::Warning:
        if (!m_settings->notifyWarning()) {
            return false;
        }
        break;
    default:
        break;
    }
    if (n.category == Notification::Category::Background && !m_settings->notifyBackground()) {
        return false;
    }
    if (n.category == Notification::Category::Playback && !m_settings->notifyPlayback()) {
        return false;
    }
    return true;
}

bool NotificationManager::inAppEnabledFor(const Notification& n) const {
    return (!m_settings || m_settings->notifyInApp()) && categoryAllowed(n);
}

ToastHost* NotificationManager::hostForDisplay() const {
    ToastHost* fallback = nullptr;
    for (const auto& h : m_hosts) {
        if (!h || !h->window() || !h->window()->isVisible() || h->window()->isMinimized()) {
            continue;
        }
        if (h->window()->isActiveWindow()) {
            return h;
        }
        if (!fallback) {
            fallback = h;
        }
    }
    return fallback;
}

bool NotificationManager::shouldSuppress(const Notification& n) {
    const qint64 now = m_clock.elapsed();
    QString key = n.key.isEmpty() ? QString::number(int(n.kind)) + QLatin1Char('|') + n.title : n.key;
    const QString text = n.title + QLatin1Char('\n') + n.message;
    const int cooldown = n.cooldownMs > 0 ? n.cooldownMs : defaultCooldown(n.kind);
    const auto it = m_recent.constFind(key);
    if (it != m_recent.constEnd() && now - it->at < cooldown && (n.rateLimit || it->text == text)) {
        return true;
    }
    // Global flood guard: routine feedback is dropped when a burst is
    // already underway; errors and warnings always get through (they have
    // their own per-key dedupe above).
    while (!m_burst.isEmpty() && now - m_burst.first() > kFloodWindowMs) {
        m_burst.removeFirst();
    }
    if (m_burst.size() >= kFloodLimit &&
        (n.kind == Notification::Kind::Info || n.kind == Notification::Kind::Success)) {
        return true;
    }
    m_recent.insert(key, {text, now});
    m_burst.push_back(now);
    if (m_recent.size() > 256) { // bounded memory
        m_recent.clear();
    }
    return false;
}

bool NotificationManager::sendDesktop(const Notification& n) {
    if (!m_tray || !m_tray->isVisible() || !QSystemTrayIcon::supportsMessages()) {
        if (!m_loggedNoDesktop) {
            m_loggedNoDesktop = true;
            qInfo() << "[Notify] Windows notifications unavailable (no visible tray icon); using in-app only.";
        }
        return false;
    }
    QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information;
    if (n.kind == Notification::Kind::Warning) {
        icon = QSystemTrayIcon::Warning;
    } else if (n.kind == Notification::Kind::Error) {
        icon = QSystemTrayIcon::Critical;
    }
    // Fire-and-forget: showMessage() returns void and Windows decides
    // whether to present it (Focus Assist etc.).
    m_tray->showMessage(n.title, n.message.isEmpty() ? n.title : n.message, icon, 6000);
    ++m_deliveredDesktop;
    return true;
}

bool NotificationManager::deliver(const Notification& n) {
    if (n.title.isEmpty()) {
        return false;
    }
    const bool inApp = inAppEnabledFor(n);
    const bool desktopOn = (!m_settings || m_settings->notifyDesktop()) && categoryAllowed(n) && n.desktop;
    if (!inApp && !desktopOn) {
        return false; // disabled by the user; the operation itself is unaffected
    }
    if (shouldSuppress(n)) {
        ++m_suppressed;
        return false;
    }
    qInfo().noquote() << "[Notify]" << int(n.kind) << n.title << "-" << n.message;

    bool shown = false;
    if (inApp) {
        if (ToastHost* host = hostForDisplay()) {
            host->present(n, n.durationMs > 0 ? n.durationMs : defaultDuration(n.kind));
            ++m_deliveredInApp;
            m_last = n;
            shown = true;
        }
    }
    // A desktop notification is for when the user isn't looking at Motiva.
    if (desktopOn && qApp->applicationState() != Qt::ApplicationActive) {
        sendDesktop(n);
    }
    return shown;
}

bool NotificationManager::notify(const Notification& n) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, n] { notify(n); }, Qt::QueuedConnection);
        return false;
    }
    try {
        return deliver(n);
    } catch (const std::exception& e) {
        qWarning() << "[Notify] Delivery failed:" << e.what();
    } catch (...) {
        qWarning() << "[Notify] Delivery failed (unknown error).";
    }
    return false;
}
