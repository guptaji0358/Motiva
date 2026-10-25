#pragma once

#include "CloudProvider.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSettings>
#include <QTimer>

#include <memory>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpServer;
class QFile;

// Cloud storage for wallpapers: sign in to OneDrive / Google Drive / Dropbox,
// browse, and download chosen files into a local cache folder. Playlists only
// ever reference the cached LOCAL file, so playback/rendering never waits on
// the network and keeps working offline once a file has been fetched.
//
// Read-only: Motiva never uploads to, changes or deletes anything in the
// user's cloud. Sign-in is OAuth 2.0 code + PKCE in the user's browser (the
// password never reaches Motiva); refresh tokens are kept encrypted with
// Windows DPAPI (current user). All work is asynchronous on the GUI thread.
class CloudManager : public QObject {
    Q_OBJECT
public:
    static CloudManager* instance();
    ~CloudManager() override;

    QStringList providerIds() const;
    CloudProvider* provider(const QString& id) const;

    // The app registration (client id) the user created for this service.
    // Motiva ships none: each user/distributor supplies their own.
    QString clientId(const QString& providerId) const;
    QString clientSecret(const QString& providerId) const;
    void setClientCredentials(const QString& providerId, const QString& clientId, const QString& clientSecret);
    bool isConfigured(const QString& providerId) const { return !clientId(providerId).isEmpty(); }
    // True when Motiva ships its own app registration for this service (CloudAppIds.h),
    // so the user never has to set anything up.
    bool hasBuiltInCredentials(const QString& providerId) const;

    bool isConnected(const QString& providerId) const;
    bool isConnecting(const QString& providerId) const { return m_flows.contains(providerId); }
    // Opens the service's sign-in page in the default browser and waits for
    // it to come back to the local loopback address. Result: accountChanged().
    // `email` (optional) pre-fills the sign-in page so the user only types the
    // password / verification code there.
    void connectAccount(const QString& providerId, const QString& email = QString());
    void cancelConnect(const QString& providerId);
    // Forgets the stored tokens. Cached files stay on disk.
    void disconnectAccount(const QString& providerId);

    // The loopback address every service must have registered.
    static QString redirectUri();

    using ListCallback = std::function<void(const CloudListPage&)>;
    // Lists one page of `folderId` (empty folderId = the drive root).
    void list(const QString& providerId, const QString& folderId, const QString& pageToken, ListCallback done);

    // --- local cache ---
    static QString cacheRoot();
    // Where a given cloud file lives (or will live) locally.
    QString cachePathFor(const QString& providerId, const CloudEntry& entry) const;
    bool isCached(const QString& providerId, const CloudEntry& entry) const;
    // "OneDrive: <name>" for a cached file Motiva fetched; empty otherwise.
    QString originOf(const QString& localPath) const;
    qint64 cacheBytes() const;
    // Drops the in-memory origin map (CleanupManager deletes the files).
    void forgetCacheIndex() { m_index.clear(); }
    // Deliberately no delete here: CleanupManager is the only code that
    // removes Motiva data (a playlist may still point at a cached file).

    // Downloads `entries` one after another (cached, unchanged files are
    // reused). Progress/finish are reported through the signals below; only
    // one batch runs at a time.
    bool downloadMany(const QString& providerId, const QList<CloudEntry>& entries);
    bool isDownloading() const { return m_batchActive; }
    void cancelDownloads();

signals:
    void accountChanged(const QString& providerId);
    void connectFailed(const QString& providerId, const QString& message);
    // done = files finished so far, percent = current file.
    void downloadProgress(int done, int total, const QString& currentName, int percent);
    // `paths` are the local files now available (in request order);
    // `errors` one line per file that failed. `cancelled` when stopped early.
    void downloadFinished(const QStringList& paths, const QStringList& errors, bool cancelled);

private:
    explicit CloudManager(QObject* parent = nullptr);

    struct Token {
        QString access;
        QDateTime expires;
    };
    struct Flow {
        QTcpServer* server = nullptr;
        QString verifier;
        QString state;
        QTimer* timeout = nullptr;
    };

    using TokenCallback = std::function<void(const QString& token, const QString& error)>;
    void withToken(const QString& providerId, TokenCallback cb);
    void requestTokens(const QString& providerId, const QList<QPair<QString, QString>>& form, TokenCallback cb);
    void finishFlow(const QString& providerId);
    void handleLoopbackRequest(const QString& providerId, class QTcpSocket* socket);
    QNetworkReply* send(const QNetworkRequest& req, const QByteArray& postBody, const QString& token);
    void startNextDownload();
    void finishBatch(bool cancelled);
    void loadIndex();
    void saveIndex() const;
    QString refreshToken(const QString& providerId) const;
    void storeRefreshToken(const QString& providerId, const QString& token);

    QNetworkAccessManager* m_net = nullptr;
    mutable QSettings m_settings;
    std::vector<std::unique_ptr<CloudProvider>> m_providers;
    QHash<QString, Token> m_tokens;
    QHash<QString, Flow> m_flows;

    // Download batch state.
    bool m_batchActive = false;
    QString m_batchProvider;
    QQueue<CloudEntry> m_queue;
    int m_total = 0;
    int m_done = 0;
    QStringList m_paths;
    QStringList m_errors;
    QPointer<QNetworkReply> m_reply;
    bool m_cancelRequested = false;

    // local path -> {provider, id, name, size}
    QHash<QString, QJsonObject> m_index;
};
