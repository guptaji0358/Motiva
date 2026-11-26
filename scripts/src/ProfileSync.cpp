#include "ProfileSync.h"

#include "PerfProbe.h"
#include "ProfileStore.h"

#include <QDateTime>
#include <algorithm>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>

ProfileSync::ProfileSync(ProfileStore* store, PerfProbe* probe, QObject* parent)
    : QObject(parent), m_store(store), m_probe(probe) {}

void ProfileSync::configure(const QUrl& endpoint, const QString& stateDir) {
    m_endpoint = endpoint;
    m_stateDir = stateDir;
    QDir().mkpath(stateDir);
}

bool ProfileSync::endpointAllowed(const QUrl& u) const {
    if (!u.isValid() || u.host().isEmpty()) {
        return false;
    }
    const bool loopback = u.host() == QLatin1String("127.0.0.1") || u.host() == QLatin1String("localhost") ||
                          u.host() == QLatin1String("::1");
    if (!perf::kProductionKeyConfigured) {
        return loopback && (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https"));
    }
    return u.scheme() == QLatin1String("https") || (loopback && u.scheme() == QLatin1String("http"));
}

qint64 ProfileSync::stateValue(const QString& key) const {
    QFile f(m_stateDir + QStringLiteral("/sync.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        return 0;
    }
    return static_cast<qint64>(QJsonDocument::fromJson(f.readAll()).object().value(key).toDouble());
}

void ProfileSync::setState(const QString& key, const QVariant& v) {
    QFile f(m_stateDir + QStringLiteral("/sync.json"));
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) {
        o = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
    }
    o.insert(key, QJsonValue::fromVariant(v));
    QSaveFile out(m_stateDir + QStringLiteral("/sync.json"));
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        out.commit();
    }
}

void ProfileSync::syncNow(bool force) {
    if (m_busy) {
        return;
    }
    if (!m_probe->consent()) {
        return; // opt-in off: no network traffic at all
    }
    if (!endpointAllowed(m_endpoint)) {
        return;
    }
    m_busy = true;
    m_summary.clear();
    uploadStep(force);
}

void ProfileSync::done(const QString& summary) {
    m_busy = false;
    emit finished(summary);
}

bool ProfileSync::due(qint64 now, qint64 last, qint64 periodSecs) {
    if (last == 0) {
        return true; // never done: the initial sync is always due
    }
    const qint64 slack = std::min<qint64>(300, periodSecs / 20);
    return now - last >= periodSecs - slack;
}

qint64 ProfileSync::nowSecs() const {
    return m_clock ? m_clock() : QDateTime::currentSecsSinceEpoch();
}

void ProfileSync::uploadStep(bool force) {
    const qint64 now = nowSecs();
    if (!force && !due(now, stateValue(QStringLiteral("lastUpload")), m_uploadSecs)) {
        fetchStep(force);
        return;
    }
    const PerfProbe::Batch batchIds = m_probe->peekBatchIds(100);
    const QList<QByteArray>& batch = batchIds.records;
    if (batch.isEmpty()) {
        fetchStep(force);
        return;
    }
    QJsonArray arr;
    for (const QByteArray& line : batch) {
        arr.append(QJsonDocument::fromJson(line).object());
    }
    QUrl url = m_endpoint;
    url.setPath(url.path() + QStringLiteral("/v1/records"));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(m_timeoutMs);
    // No cookies, no identifiers: just the batch.
    QNetworkReply* reply = m_net.post(req, QJsonDocument(QJsonObject{{"records", arr}}).toJson(QJsonDocument::Compact));
    const int count = batch.size();
    const QList<quint64> ids = batchIds.ids;
    connect(reply, &QNetworkReply::finished, this, [this, reply, count, ids, force, now] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        reply->deleteLater();
        if (ok) {
            m_probe->acknowledgeIds(ids); // dropped locally only after the server accepted it
            setState(QStringLiteral("lastUpload"), now);
            m_summary += QStringLiteral("uploaded %1; ").arg(count);
        } else {
            m_summary += QStringLiteral("upload failed; "); // records stay queued for a later retry
        }
        fetchStep(force);
    });
}

void ProfileSync::fetchStep(bool force) {
    const qint64 now = nowSecs();
    if (!force && !due(now, stateValue(QStringLiteral("lastPoll")), m_pollSecs)) {
        done(m_summary + QStringLiteral("poll skipped"));
        return;
    }
    QUrl url = m_endpoint;
    url.setPath(url.path() + QStringLiteral("/v1/profile"));
    QNetworkRequest req(url);
    req.setTransferTimeout(m_timeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QFile etagFile(m_stateDir + QStringLiteral("/profile.etag"));
    if (etagFile.open(QIODevice::ReadOnly)) {
        req.setRawHeader("If-None-Match", etagFile.readAll().trimmed());
    }
    QNetworkReply* reply = m_net.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, now] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->read(perf::kMaxEnvelopeBytes + 1);
        const QByteArray etag = reply->rawHeader("ETag");
        const bool netOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        setState(QStringLiteral("lastPoll"), now);
        if (!netOk && status != 304) {
            done(m_summary + QStringLiteral("profile fetch failed"));
            return;
        }
        if (status == 200) {
            QString err;
            const auto r = m_store->install(body, &err);
            if (r == ProfileStore::InstallResult::Activated && !etag.isEmpty()) {
                QFile f(m_stateDir + QStringLiteral("/profile.etag"));
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(etag);
                }
            }
            m_summary += r == ProfileStore::InstallResult::Activated ? QStringLiteral("profile activated")
                                                                    : QStringLiteral("profile not installed (%1)").arg(err);
        } else {
            m_summary += QStringLiteral("profile unchanged");
        }
        done(m_summary);
    });
}
