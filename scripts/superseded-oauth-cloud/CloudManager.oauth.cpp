#include "CloudManager.h"
#include "CloudAppIds.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>

#include <windows.h>
#include <wincrypt.h>

namespace {

constexpr quint16 kLoopbackPort = 53682;
constexpr int kSignInTimeoutMs = 5 * 60 * 1000;
constexpr int kRequestTimeoutMs = 30 * 1000;

QByteArray base64Url(const QByteArray& in) {
    return in.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray randomToken(int bytes) {
    QByteArray raw(bytes, 0);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(raw.data()), bytes / 4);
    return base64Url(raw);
}

// Windows DPAPI, current-user scope: only this Windows account can decrypt.
QByteArray dpapiProtect(const QByteArray& plain) {
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Motiva cloud", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    const QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return result;
}

QByteArray dpapiUnprotect(const QByteArray& cipher) {
    DATA_BLOB in{DWORD(cipher.size()), reinterpret_cast<BYTE*>(const_cast<char*>(cipher.constData()))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    const QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

QString sanitizedFileName(const QString& name) {
    QString out;
    for (const QChar c : name) {
        out += (c.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(c)) ? QLatin1Char('_') : c;
    }
    out = out.trimmed();
    while (out.endsWith(QLatin1Char('.'))) {
        out.chop(1);
    }
    if (out.isEmpty()) {
        out = QStringLiteral("file");
    }
    // Keep paths short of MAX_PATH while preserving the extension.
    const QFileInfo fi(out);
    if (out.size() > 120) {
        const QString ext = fi.suffix().left(10);
        out = fi.completeBaseName().left(110) + (ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
    }
    return out;
}

QString indexKey(const QString& path) {
    return QDir::fromNativeSeparators(QDir::cleanPath(path)).toLower();
}

QString httpProblem(QNetworkReply* reply) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 || status == 403) {
        return QObject::tr("The service refused access (%1). Try disconnecting and connecting again.").arg(status);
    }
    if (status == 429) {
        return QObject::tr("The service is rate limiting requests. Try again in a minute.");
    }
    return reply->errorString();
}

} // namespace

CloudManager* CloudManager::instance() {
    static CloudManager* s = new CloudManager(); // lives for the whole process
    return s;
}

CloudManager::CloudManager(QObject* parent)
    : QObject(parent), m_settings(QSettings::NativeFormat, QSettings::UserScope, "Motiva", "Motiva") {
    m_net = new QNetworkAccessManager(this);
    m_providers.push_back(makeOneDriveProvider());
    m_providers.push_back(makeGoogleDriveProvider());
    m_providers.push_back(makeDropboxProvider());
    loadIndex();
}

CloudManager::~CloudManager() = default;

QStringList CloudManager::providerIds() const {
    QStringList ids;
    for (const auto& p : m_providers) {
        ids << p->id();
    }
    return ids;
}

CloudProvider* CloudManager::provider(const QString& id) const {
    for (const auto& p : m_providers) {
        if (p->id() == id) {
            return p.get();
        }
    }
    return nullptr;
}

QString CloudManager::redirectUri() {
    return QStringLiteral("http://localhost:%1/").arg(kLoopbackPort);
}

// ------------------------------------------------------------------ credentials

namespace {
QString builtInClientId(const QString& id) {
    if (id == QLatin1String("onedrive")) return QString::fromLatin1(CloudAppIds::kOneDriveClientId);
    if (id == QLatin1String("gdrive")) return QString::fromLatin1(CloudAppIds::kGoogleDriveClientId);
    if (id == QLatin1String("dropbox")) return QString::fromLatin1(CloudAppIds::kDropboxClientId);
    return QString();
}
QString builtInClientSecret(const QString& id) {
    return id == QLatin1String("gdrive") ? QString::fromLatin1(CloudAppIds::kGoogleDriveClientSecret) : QString();
}
} // namespace

bool CloudManager::hasBuiltInCredentials(const QString& id) const {
    return !builtInClientId(id).isEmpty();
}

// A user's own registration (Set Up...) wins over the built-in one.
QString CloudManager::clientId(const QString& id) const {
    const QString own = m_settings.value(QStringLiteral("cloud/%1/clientId").arg(id)).toString().trimmed();
    return own.isEmpty() ? builtInClientId(id) : own;
}

QString CloudManager::clientSecret(const QString& id) const {
    const QByteArray cipher = QByteArray::fromBase64(m_settings.value(QStringLiteral("cloud/%1/clientSecret").arg(id)).toByteArray());
    if (cipher.isEmpty()) {
        return m_settings.value(QStringLiteral("cloud/%1/clientId").arg(id)).toString().trimmed().isEmpty() ? builtInClientSecret(id) : QString();
    }
    return QString::fromUtf8(dpapiUnprotect(cipher));
}

void CloudManager::setClientCredentials(const QString& id, const QString& clientId, const QString& secret) {
    m_settings.setValue(QStringLiteral("cloud/%1/clientId").arg(id), clientId.trimmed());
    if (secret.trimmed().isEmpty()) {
        m_settings.remove(QStringLiteral("cloud/%1/clientSecret").arg(id));
    } else {
        m_settings.setValue(QStringLiteral("cloud/%1/clientSecret").arg(id),
                            dpapiProtect(secret.trimmed().toUtf8()).toBase64());
    }
    emit accountChanged(id);
}

QString CloudManager::refreshToken(const QString& id) const {
    const QByteArray cipher = QByteArray::fromBase64(m_settings.value(QStringLiteral("cloud/%1/refresh").arg(id)).toByteArray());
    return cipher.isEmpty() ? QString() : QString::fromUtf8(dpapiUnprotect(cipher));
}

void CloudManager::storeRefreshToken(const QString& id, const QString& token) {
    const QByteArray cipher = dpapiProtect(token.toUtf8());
    if (cipher.isEmpty()) {
        return; // never fall back to storing it in the clear
    }
    m_settings.setValue(QStringLiteral("cloud/%1/refresh").arg(id), cipher.toBase64());
}

bool CloudManager::isConnected(const QString& id) const {
    return !refreshToken(id).isEmpty();
}

void CloudManager::disconnectAccount(const QString& id) {
    if (m_batchActive && m_batchProvider == id) {
        cancelDownloads();
    }
    m_tokens.remove(id);
    m_settings.remove(QStringLiteral("cloud/%1/refresh").arg(id));
    emit accountChanged(id);
}

// ---------------------------------------------------------------------- sign-in

void CloudManager::connectAccount(const QString& id, const QString& email) {
    CloudProvider* p = provider(id);
    if (!p) {
        return;
    }
    if (!isConfigured(id)) {
        emit connectFailed(id, tr("Enter the client ID of your %1 app registration first.").arg(p->displayName()));
        return;
    }
    if (!m_flows.isEmpty()) {
        emit connectFailed(id, tr("Another sign-in is already waiting for your browser."));
        return;
    }

    Flow flow;
    flow.server = new QTcpServer(this);
    // Loopback only: nothing outside this PC can reach the listener.
    if (!flow.server->listen(QHostAddress::LocalHost, kLoopbackPort)) {
        const QString why = flow.server->errorString();
        delete flow.server;
        emit connectFailed(id, tr("Could not start the local sign-in listener on port %1: %2").arg(kLoopbackPort).arg(why));
        return;
    }
    flow.verifier = QString::fromLatin1(randomToken(48));
    flow.state = QString::fromLatin1(randomToken(24));
    flow.timeout = new QTimer(this);
    flow.timeout->setSingleShot(true);
    connect(flow.timeout, &QTimer::timeout, this, [this, id] {
        finishFlow(id);
        emit connectFailed(id, tr("Sign-in timed out. Click Connect to try again."));
    });
    flow.timeout->start(kSignInTimeoutMs);
    m_flows.insert(id, flow);

    connect(flow.server, &QTcpServer::newConnection, this, [this, id] {
        Flow f = m_flows.value(id);
        while (f.server && f.server->hasPendingConnections()) {
            QTcpSocket* socket = f.server->nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, id, socket] { handleLoopbackRequest(id, socket); });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    QUrl url = p->authorizationEndpoint();
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    q.addQueryItem(QStringLiteral("client_id"), clientId(id));
    q.addQueryItem(QStringLiteral("redirect_uri"), redirectUri());
    q.addQueryItem(QStringLiteral("scope"), p->scopes());
    q.addQueryItem(QStringLiteral("state"), flow.state);
    q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    q.addQueryItem(QStringLiteral("code_challenge"),
                   QString::fromLatin1(base64Url(QCryptographicHash::hash(flow.verifier.toLatin1(), QCryptographicHash::Sha256))));
    if (!email.trimmed().isEmpty()) {
        q.addQueryItem(QStringLiteral("login_hint"), email.trimmed());
    }
    for (const auto& extra : p->extraAuthParams()) {
        q.addQueryItem(extra.first, extra.second);
    }
    url.setQuery(q);
    if (!QDesktopServices::openUrl(url)) {
        finishFlow(id);
        emit connectFailed(id, tr("Could not open your web browser."));
    }
    emit accountChanged(id); // "Connecting..." state
}

void CloudManager::cancelConnect(const QString& id) {
    if (m_flows.contains(id)) {
        finishFlow(id);
        emit accountChanged(id);
    }
}

void CloudManager::finishFlow(const QString& id) {
    const Flow f = m_flows.take(id);
    if (f.timeout) {
        f.timeout->stop();
        f.timeout->deleteLater();
    }
    if (f.server) {
        f.server->close();
        f.server->deleteLater();
    }
}

void CloudManager::handleLoopbackRequest(const QString& id, QTcpSocket* socket) {
    const QByteArray data = socket->peek(socket->bytesAvailable());
    const int lineEnd = data.indexOf("\r\n");
    if (lineEnd < 0) {
        return; // request line not complete yet
    }
    const QList<QByteArray> parts = data.left(lineEnd).split(' ');
    const auto respond = [socket](int status, const QString& html) {
        const QByteArray body = html.toUtf8();
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                      "Connection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    };
    if (parts.size() < 2 || !m_flows.contains(id)) {
        respond(404, QString());
        return;
    }
    const QUrl target(QStringLiteral("http://localhost") + QString::fromLatin1(parts.at(1)));
    const QUrlQuery q(target);
    if (!q.hasQueryItem(QStringLiteral("code")) && !q.hasQueryItem(QStringLiteral("error"))) {
        respond(404, QString()); // e.g. /favicon.ico - keep waiting for the real redirect
        return;
    }
    const Flow flow = m_flows.value(id);
    if (q.queryItemValue(QStringLiteral("state")) != flow.state) {
        respond(400, tr("<h3>Sign-in rejected</h3><p>The response did not match this request. Return to Motiva and try again.</p>"));
        finishFlow(id);
        emit connectFailed(id, tr("The sign-in response did not match the request, so it was ignored."));
        return;
    }
    if (q.hasQueryItem(QStringLiteral("error"))) {
        const QString why = q.queryItemValue(QStringLiteral("error_description"), QUrl::FullyDecoded);
        respond(200, tr("<h3>Sign-in was not completed</h3><p>You can close this tab and return to Motiva.</p>"));
        finishFlow(id);
        emit connectFailed(id, why.isEmpty() ? tr("Sign-in was cancelled.") : why);
        return;
    }
    const QString code = q.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
    respond(200, tr("<h3>Motiva is connected</h3><p>You can close this tab and return to Motiva.</p>"));
    const QString verifier = flow.verifier;
    finishFlow(id);

    QList<QPair<QString, QString>> form = {
        {QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
        {QStringLiteral("code"), code},
        {QStringLiteral("redirect_uri"), redirectUri()},
        {QStringLiteral("client_id"), clientId(id)},
        {QStringLiteral("code_verifier"), verifier},
    };
    requestTokens(id, form, [this, id](const QString& token, const QString& error) {
        if (token.isEmpty()) {
            emit connectFailed(id, error);
        }
        emit accountChanged(id);
    });
}

void CloudManager::requestTokens(const QString& id, const QList<QPair<QString, QString>>& form, TokenCallback cb) {
    CloudProvider* p = provider(id);
    QUrlQuery body;
    for (const auto& kv : form) {
        body.addQueryItem(kv.first, kv.second);
    }
    if (p->usesClientSecret() && !clientSecret(id).isEmpty()) {
        body.addQueryItem(QStringLiteral("client_secret"), clientSecret(id));
    }
    QNetworkRequest req(p->tokenEndpoint());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    req.setTransferTimeout(kRequestTimeoutMs);
    QNetworkReply* reply = m_net->post(req, body.query(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, id, reply, cb] {
        reply->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString access = o.value(QStringLiteral("access_token")).toString();
        if (access.isEmpty()) {
            const QString why = o.value(QStringLiteral("error_description")).toString(
                o.value(QStringLiteral("error")).toString(httpProblem(reply)));
            if (o.value(QStringLiteral("error")).toString() == QLatin1String("invalid_grant")) {
                // The refresh token was revoked/expired: sign out so the UI offers Connect again.
                m_settings.remove(QStringLiteral("cloud/%1/refresh").arg(id));
                m_tokens.remove(id);
                emit accountChanged(id);
            }
            cb(QString(), tr("Sign-in failed: %1").arg(why));
            return;
        }
        Token t;
        t.access = access;
        t.expires = QDateTime::currentDateTimeUtc().addSecs(o.value(QStringLiteral("expires_in")).toInt(3600));
        m_tokens.insert(id, t);
        const QString refresh = o.value(QStringLiteral("refresh_token")).toString();
        if (!refresh.isEmpty()) {
            storeRefreshToken(id, refresh);
        }
        cb(access, QString());
    });
}

void CloudManager::withToken(const QString& id, TokenCallback cb) {
    const auto it = m_tokens.constFind(id);
    if (it != m_tokens.constEnd() && it->expires > QDateTime::currentDateTimeUtc().addSecs(60)) {
        cb(it->access, QString());
        return;
    }
    const QString refresh = refreshToken(id);
    if (refresh.isEmpty()) {
        cb(QString(), tr("Not connected to %1.").arg(provider(id) ? provider(id)->displayName() : id));
        return;
    }
    requestTokens(id,
                  {{QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
                   {QStringLiteral("refresh_token"), refresh},
                   {QStringLiteral("client_id"), clientId(id)}},
                  std::move(cb));
}

QNetworkReply* CloudManager::send(const QNetworkRequest& request, const QByteArray& postBody, const QString& token) {
    QNetworkRequest req = request;
    if (!token.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    }
    req.setTransferTimeout(kRequestTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return postBody.isNull() ? m_net->get(req) : m_net->post(req, postBody);
}

// ----------------------------------------------------------------------- listing

void CloudManager::list(const QString& id, const QString& folderId, const QString& pageToken, ListCallback done) {
    CloudProvider* p = provider(id);
    if (!p) {
        done({});
        return;
    }
    withToken(id, [this, p, folderId, pageToken, done](const QString& token, const QString& error) {
        if (token.isEmpty()) {
            CloudListPage page;
            page.error = error;
            done(page);
            return;
        }
        QByteArray body; // stays null for GET
        const QNetworkRequest req = p->listRequest(folderId.isEmpty() ? p->rootId() : folderId, pageToken, &body);
        QNetworkReply* reply = send(req, body, token);
        connect(reply, &QNetworkReply::finished, this, [p, reply, done] {
            reply->deleteLater();
            const QByteArray data = reply->readAll();
            CloudListPage page;
            if (data.isEmpty() && reply->error() != QNetworkReply::NoError) {
                page.error = httpProblem(reply);
            } else {
                page = p->parseList(data);
                if (!page.ok && page.error.isEmpty()) {
                    page.error = httpProblem(reply);
                }
            }
            done(page);
        });
    });
}

// ------------------------------------------------------------------------- cache

QString CloudManager::cacheRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/cloud-cache");
}

QString CloudManager::cachePathFor(const QString& id, const CloudEntry& entry) const {
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash((id + QLatin1Char(':') + entry.id).toUtf8(), QCryptographicHash::Sha1).toHex().left(10));
    return QStringLiteral("%1/%2/%3/%4").arg(cacheRoot(), id, key, sanitizedFileName(entry.name));
}

bool CloudManager::isCached(const QString& id, const CloudEntry& entry) const {
    const QFileInfo fi(cachePathFor(id, entry));
    return fi.isFile() && (entry.size <= 0 || fi.size() == entry.size);
}

QString CloudManager::originOf(const QString& localPath) const {
    const auto it = m_index.constFind(indexKey(localPath));
    if (it == m_index.constEnd()) {
        return QString();
    }
    CloudProvider* p = provider(it->value(QStringLiteral("provider")).toString());
    return tr("%1: %2").arg(p ? p->displayName() : it->value(QStringLiteral("provider")).toString(),
                            it->value(QStringLiteral("name")).toString());
}

qint64 CloudManager::cacheBytes() const {
    qint64 total = 0;
    QDirIterator it(cacheRoot(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileName() != QLatin1String("index.json")) {
            total += it.fileInfo().size();
        }
    }
    return total;
}

void CloudManager::loadIndex() {
    QFile f(cacheRoot() + QStringLiteral("/index.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        m_index.insert(it.key(), it.value().toObject());
    }
}

void CloudManager::saveIndex() const {
    QDir().mkpath(cacheRoot());
    QJsonObject root;
    for (auto it = m_index.begin(); it != m_index.end(); ++it) {
        root.insert(it.key(), it.value());
    }
    QFile f(cacheRoot() + QStringLiteral("/index.json.tmp"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
        QFile::remove(cacheRoot() + QStringLiteral("/index.json"));
        f.rename(cacheRoot() + QStringLiteral("/index.json"));
    }
}

// --------------------------------------------------------------------- downloads

bool CloudManager::downloadMany(const QString& id, const QList<CloudEntry>& entries) {
    if (m_batchActive || entries.isEmpty() || !provider(id)) {
        return false;
    }
    m_batchActive = true;
    m_cancelRequested = false;
    m_batchProvider = id;
    m_queue.clear();
    for (const CloudEntry& e : entries) {
        if (!e.isFolder) {
            m_queue.enqueue(e);
        }
    }
    m_total = m_queue.size();
    m_done = 0;
    m_paths.clear();
    m_errors.clear();
    if (m_total == 0) {
        finishBatch(false);
        return true;
    }
    emit downloadProgress(0, m_total, QString(), 0);
    startNextDownload();
    return true;
}

void CloudManager::cancelDownloads() {
    if (!m_batchActive) {
        return;
    }
    m_cancelRequested = true;
    if (m_reply) {
        m_reply->abort(); // finished handler ends the batch
    }
}

void CloudManager::finishBatch(bool cancelled) {
    m_batchActive = false;
    m_queue.clear();
    const QStringList paths = m_paths;
    const QStringList errors = m_errors;
    emit downloadFinished(paths, errors, cancelled);
}

void CloudManager::startNextDownload() {
    // Reuse everything already cached without touching the network.
    while (!m_queue.isEmpty()) {
        const CloudEntry& head = m_queue.head();
        if (!isCached(m_batchProvider, head)) {
            break;
        }
        m_paths << cachePathFor(m_batchProvider, head);
        m_queue.dequeue();
        ++m_done;
        emit downloadProgress(m_done, m_total, QString(), 0);
    }
    if (m_cancelRequested) {
        finishBatch(true);
        return;
    }
    if (m_queue.isEmpty()) {
        finishBatch(false);
        return;
    }

    const CloudEntry entry = m_queue.dequeue();
    const QString dest = cachePathFor(m_batchProvider, entry);
    const auto fail = [this](const QString& name, const QString& why) {
        m_errors << tr("%1: %2").arg(name, why);
        ++m_done;
        emit downloadProgress(m_done, m_total, QString(), 0);
        startNextDownload();
    };

    QDir().mkpath(QFileInfo(dest).absolutePath());
    const QStorageInfo drive(QFileInfo(dest).absolutePath());
    if (drive.isValid() && entry.size > 0 && drive.bytesAvailable() < entry.size + 100LL * 1024 * 1024) {
        fail(entry.name, tr("not enough free disk space"));
        return;
    }
    auto file = std::make_shared<QFile>(dest + QStringLiteral(".partial"));
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(entry.name, tr("could not write to the cache folder (%1)").arg(file->errorString()));
        return;
    }

    CloudProvider* p = provider(m_batchProvider);
    const QString providerId = m_batchProvider;

    // Streams `reply` into the .partial file, then publishes it under its real name.
    const auto stream = [this, file, dest, entry, providerId, fail](QNetworkReply* reply) {
        m_reply = reply;
        connect(reply, &QNetworkReply::readyRead, this, [file, reply] { file->write(reply->readAll()); });
        connect(reply, &QNetworkReply::downloadProgress, this, [this, entry](qint64 got, qint64 total) {
            const qint64 expected = total > 0 ? total : entry.size;
            emit downloadProgress(m_done, m_total, entry.name, expected > 0 ? int(got * 100 / expected) : 0);
        });
        connect(reply, &QNetworkReply::finished, this, [this, file, reply, dest, entry, providerId, fail] {
            reply->deleteLater();
            file->write(reply->readAll());
            file->close();
            const bool cancelled = reply->error() == QNetworkReply::OperationCanceledError && m_cancelRequested;
            if (reply->error() != QNetworkReply::NoError) {
                QFile::remove(file->fileName());
                if (cancelled) {
                    finishBatch(true);
                } else {
                    fail(entry.name, httpProblem(reply));
                }
                return;
            }
            if (entry.size > 0 && QFileInfo(file->fileName()).size() != entry.size) {
                QFile::remove(file->fileName());
                fail(entry.name, tr("the download was incomplete"));
                return;
            }
            QFile::remove(dest);
            if (!QFile::rename(file->fileName(), dest)) {
                QFile::remove(file->fileName());
                fail(entry.name, tr("could not save the file"));
                return;
            }
            QJsonObject origin;
            origin.insert(QStringLiteral("provider"), providerId);
            origin.insert(QStringLiteral("id"), entry.id);
            origin.insert(QStringLiteral("name"), entry.name);
            origin.insert(QStringLiteral("size"), double(entry.size));
            m_index.insert(indexKey(dest), origin);
            saveIndex();
            m_paths << dest;
            ++m_done;
            emit downloadProgress(m_done, m_total, QString(), 0);
            if (m_cancelRequested) {
                finishBatch(true);
            } else {
                startNextDownload();
            }
        });
    };

    withToken(providerId, [this, p, entry, stream, fail](const QString& token, const QString& error) {
        if (token.isEmpty()) {
            fail(entry.name, error);
            return;
        }
        if (m_cancelRequested) {
            finishBatch(true);
            return;
        }
        if (p->needsDownloadUrlLookup()) {
            // Step 1: ask for the short-lived pre-authenticated URL.
            QNetworkReply* lookup = send(p->downloadUrlLookupRequest(entry), QByteArray(), token);
            m_reply = lookup;
            connect(lookup, &QNetworkReply::finished, this, [this, p, lookup, entry, stream, fail] {
                lookup->deleteLater();
                if (m_cancelRequested) {
                    finishBatch(true);
                    return;
                }
                const QUrl url = p->parseDownloadUrl(lookup->readAll());
                if (!url.isValid() || url.isEmpty()) {
                    fail(entry.name, lookup->error() != QNetworkReply::NoError ? httpProblem(lookup)
                                                                              : tr("no download link was provided"));
                    return;
                }
                // Step 2: the link carries its own authorization - send no token.
                stream(send(QNetworkRequest(url), QByteArray(), QString()));
            });
            return;
        }
        QByteArray body;
        const QNetworkRequest req = p->downloadRequest(entry, &body);
        stream(send(req, body, token));
    });
}
