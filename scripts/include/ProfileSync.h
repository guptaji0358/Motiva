#pragma once
// Background, best-effort sync with the performance-intelligence service:
//   POST <endpoint>/v1/records   uploads the opted-in anonymous batch
//   GET  <endpoint>/v1/profile   conditional fetch (ETag) of a signed profile
// Fully asynchronous (QNetworkAccessManager), short timeouts, rate-limited, and
// every failure is silent: the wallpaper engine never waits for or depends on it.
// Disabled unless the user opted in AND an endpoint is configured. Until a real
// signing key is configured (perf::kProductionKeyConfigured) only loopback
// endpoints are accepted, so production distribution cannot be turned on by accident.

#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>
#include <functional>

class ProfileStore;
class PerfProbe;

class ProfileSync : public QObject {
    Q_OBJECT
public:
    // Schedule (MainWindow arms one QTimer with kUploadPeriodSecs and a one-shot first sync after
    // kFirstSyncDelayMs); the gating below is what keeps that cadence honest.
    static constexpr int kUploadPeriodSecs = 6 * 3600;
    static constexpr int kPollPeriodSecs = 24 * 3600;
    static constexpr int kFirstSyncDelayMs = 60000;

    // True when a step last done at `last` (0 = never) is due again at `now`. The periodic timer is armed
    // before the first sync stamps `last`, so a tick lands up to kFirstSyncDelayMs short of one period;
    // the slack (min(300 s, period/20)) keeps that tick from being skipped (which doubled the cadence).
    static bool due(qint64 now, qint64 last, qint64 periodSecs);

    explicit ProfileSync(ProfileStore* store, PerfProbe* probe, QObject* parent = nullptr);
    void setClock(std::function<qint64()> nowSecs) { m_clock = std::move(nowSecs); } // tests

    void configure(const QUrl& endpoint, const QString& stateDir);
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    bool endpointAllowed(const QUrl& u) const;

    // Starts a sync pass if allowed; returns immediately.
    void syncNow(bool force = false);
    bool busy() const { return m_busy; }

signals:
    void finished(const QString& summary);

private:
    void uploadStep(bool force);
    void fetchStep(bool force);
    void done(const QString& summary);
    qint64 stateValue(const QString& key) const;
    void setState(const QString& key, const QVariant& v);

    ProfileStore* m_store;
    PerfProbe* m_probe;
    QNetworkAccessManager m_net;
    QUrl m_endpoint;
    QString m_stateDir;
    qint64 nowSecs() const;
    std::function<qint64()> m_clock;
    int m_uploadSecs = kUploadPeriodSecs;
    int m_pollSecs = kPollPeriodSecs;
    int m_timeoutMs = 15000;
    bool m_busy = false;
    QString m_summary;
};
