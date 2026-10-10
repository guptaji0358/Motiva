#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QSystemTrayIcon;
class QWidget;
class SettingsManager;
class ToastHost;

// One notification, as described by the code that noticed the event
// (generation). Presentation and preferences are NotificationManager's job.
struct Notification {
    enum class Kind { Info, Success, Warning, Error };
    // What the event is about. Success/Warning/Error are switched by Kind;
    // Background and Playback additionally have their own switch.
    enum class Category { General, Background, Playback };

    Kind kind = Kind::Info;
    Category category = Category::General;
    QString title;
    QString message; // optional, short
    // Identifies the underlying event: a toast with the same key is updated
    // in place instead of stacking, and repeats are rate limited.
    QString key;
    // Worth a Windows desktop notification when Motiva isn't in the
    // foreground (errors, warnings, finished background work). Routine
    // successes leave this false.
    bool desktop = false;
    // true: drop ANY notification with this key inside the cooldown (e.g.
    // "wallpaper changed" every minute). false: only drop an identical
    // repeat of key+text (e.g. the same playback error over and over).
    bool rateLimit = false;
    int cooldownMs = 0; // 0 = default for the kind
    int durationMs = 0; // 0 = default for the kind
};

// The single notification service. Any part of Motiva calls notify(); this
// class applies the user's preferences, removes duplicates and floods, shows
// an in-app toast and/or a native Windows notification, and never lets a
// delivery problem reach the operation that raised the event.
//
//  - In-app: a ToastHost overlay attached to each interested top-level
//    window (attachHost). The toast appears on the active Motiva window.
//  - Desktop: QSystemTrayIcon::showMessage - Windows' notification
//    mechanism for classic desktop apps (Shell_NotifyIcon). Whether Windows
//    shows it still depends on Focus Assist / the user's notification
//    settings; Motiva cannot override that.
//
// notify() is thread-safe: from a worker thread the work is queued to the
// GUI thread. It must not be called after shutdown().
class NotificationManager : public QObject {
    Q_OBJECT
public:
    static NotificationManager* instance();

    // Preferences are read live from here on every notify(), so Settings
    // changes apply immediately.
    void setSettings(const SettingsManager* settings);
    void setTrayIcon(QSystemTrayIcon* tray);
    // Toasts may appear inside `window` (a top-level widget). `topInset` is
    // kept clear so toasts don't cover the window's header controls.
    void attachHost(QWidget* window, int topInset = 0);
    // Drops every reference (called when the main window is torn down).
    void shutdown();

    // Returns true when a toast was (or will be) shown in-app. A caller with
    // its own inline status line can use `false` as "show it there instead".
    bool notify(const Notification& n);

    // True if this kind/category would currently be delivered in-app.
    bool inAppEnabledFor(const Notification& n) const;

    // Test hooks.
    int visibleToastCount() const;
    int deliveredInApp() const { return m_deliveredInApp; }
    int deliveredDesktop() const { return m_deliveredDesktop; }
    int suppressedCount() const { return m_suppressed; }
    const Notification& lastDelivered() const { return m_last; }

private:
    explicit NotificationManager(QObject* parent = nullptr);
    bool deliver(const Notification& n);
    bool categoryAllowed(const Notification& n) const;
    ToastHost* hostForDisplay() const;
    bool shouldSuppress(const Notification& n);
    bool sendDesktop(const Notification& n);

    struct Recent {
        QString text;
        qint64 at = 0; // ms on m_clock
    };

    const SettingsManager* m_settings = nullptr;
    QPointer<QSystemTrayIcon> m_tray;
    QList<QPointer<ToastHost>> m_hosts;
    QHash<QString, Recent> m_recent;
    QList<qint64> m_burst; // delivery times, for the global flood guard
    QElapsedTimer m_clock;
    Notification m_last;
    int m_deliveredInApp = 0;
    int m_deliveredDesktop = 0;
    int m_suppressed = 0;
    bool m_loggedNoDesktop = false;
};
