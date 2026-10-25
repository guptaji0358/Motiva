#pragma once

#include <QByteArray>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <functional>

class QNetworkReply;

// Local cache for pictures and videos that come from the web through the
// browser extension (see extension/ and scripts/browser/MotivaBrowserHost.cpp).
//
// A web file is never played from its web address: it is stored here once and
// every consumer (preview, playlists, the desktop wallpaper) uses the cached
// local file. Two ways in, matching the extension's "both, with fallback":
//   * the browser streams the bytes in chunks (appendChunk) and the request
//     then names that upload, or
//   * the request carries only the URL and Motiva downloads it itself.
//
// Layout: <AppLocalData>/web-cache/<16 hex of the source>/<file name>. Cached
// files are only ever removed by the Cleanup window (a playlist may point at
// them), never automatically.
class WebMediaCache : public QObject {
    Q_OBJECT
public:
    struct Request {
        QString url;      // http(s) address, empty for blob:/data: uploads
        QString pageUrl;  // sent as Referer when Motiva downloads
        QString uploadId; // chunks already received from the browser, or empty
        QString name;     // suggested file name
        QString mime;     // content type reported by the browser
    };
    // path is the cached local file on success, otherwise error is set.
    using Callback = std::function<void(const QString& path, const QString& error)>;

    explicit WebMediaCache(QObject* parent = nullptr);

    static QString cacheRoot();
    static constexpr qint64 kMaxBytes = 2LL * 1024 * 1024 * 1024;

    // Browser -> Motiva upload, one chunk at a time (in order, seq from 0).
    QString appendChunk(const QString& uploadId, int seq, const QByteArray& data);
    void abortUpload(const QString& uploadId);

    // Resolves a request to a validated cached file (async for downloads).
    void fetch(const Request& request, Callback done);

    // Removes upload leftovers from sessions that ended mid-transfer.
    static void sweepIncomplete();

private:
    struct Upload {
        qint64 received = 0;
        int nextSeq = 0;
    };
    static QString incomingDir();
    static QString incomingPath(const QString& uploadId);
    static bool isSafeId(const QString& id);
    static QString sanitizedName(const QString& name);
    // Name + extension a cached file gets; empty when the type is not a
    // picture or video Motiva can use.
    static QString finalName(const Request& request, const QString& contentType);
    static QString entryDir(const Request& request);
    static QString validate(const QString& path);

    void download(const Request& request, Callback done);

    QHash<QString, Upload> m_uploads;
    QNetworkAccessManager m_network;
};
