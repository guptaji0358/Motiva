#include "WebMediaCache.h"

#include "PlaylistModel.h"
#include "VideoPlayer.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <memory>

namespace {

bool isMediaExtension(const QString& suffix) {
    const QString name = QStringLiteral("x.") + suffix;
    return VideoPlayer::hasVideoExtension(name) || VideoPlayer::hasStaticImageExtension(name) ||
           suffix.compare(QLatin1String("gif"), Qt::CaseInsensitive) == 0;
}

QString extensionForMime(QString mime) {
    mime = mime.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    static const QHash<QString, QString> map = {
        {QStringLiteral("image/jpeg"), QStringLiteral("jpg")},
        {QStringLiteral("image/jpg"), QStringLiteral("jpg")},
        {QStringLiteral("image/png"), QStringLiteral("png")},
        {QStringLiteral("image/webp"), QStringLiteral("webp")},
        {QStringLiteral("image/bmp"), QStringLiteral("bmp")},
        {QStringLiteral("image/gif"), QStringLiteral("gif")},
        {QStringLiteral("image/avif"), QStringLiteral("avif")},
        {QStringLiteral("video/mp4"), QStringLiteral("mp4")},
        {QStringLiteral("video/webm"), QStringLiteral("webm")},
        {QStringLiteral("video/quicktime"), QStringLiteral("mov")},
        {QStringLiteral("video/x-matroska"), QStringLiteral("mkv")},
        {QStringLiteral("video/x-m4v"), QStringLiteral("m4v")},
    };
    return map.value(mime);
}

QString shortHash(const QString& text) {
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

} // namespace

WebMediaCache::WebMediaCache(QObject* parent) : QObject(parent) {}

QString WebMediaCache::cacheRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/web-cache");
}

QString WebMediaCache::incomingDir() {
    return cacheRoot() + QStringLiteral("/_incoming");
}

bool WebMediaCache::isSafeId(const QString& id) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{8,64}$"));
    return re.match(id).hasMatch();
}

QString WebMediaCache::incomingPath(const QString& uploadId) {
    return incomingDir() + QLatin1Char('/') + uploadId + QStringLiteral(".partial");
}

QString WebMediaCache::sanitizedName(const QString& name) {
    QString clean = QFileInfo(name).fileName(); // no folders
    clean.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")), QStringLiteral("_"));
    clean = clean.trimmed();
    while (clean.endsWith(QLatin1Char('.'))) {
        clean.chop(1);
    }
    return clean.left(120);
}

void WebMediaCache::sweepIncomplete() {
    QDirIterator it(incomingDir(), {QStringLiteral("*.partial")}, QDir::Files);
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        if (fi.lastModified() < cutoff) {
            QFile::remove(fi.absoluteFilePath());
        }
    }
}

QString WebMediaCache::appendChunk(const QString& uploadId, int seq, const QByteArray& data) {
    if (!isSafeId(uploadId)) {
        return tr("Invalid upload id.");
    }
    Upload& up = m_uploads[uploadId];
    if (seq != up.nextSeq) {
        abortUpload(uploadId);
        return tr("Upload chunks arrived out of order.");
    }
    if (up.received + data.size() > kMaxBytes) {
        abortUpload(uploadId);
        return tr("The file is larger than 2 GB.");
    }
    QDir().mkpath(incomingDir());
    QFile f(incomingPath(uploadId));
    if (!f.open(seq == 0 ? QIODevice::WriteOnly | QIODevice::Truncate : QIODevice::WriteOnly | QIODevice::Append)) {
        abortUpload(uploadId);
        return tr("Motiva could not write to its cache (%1).").arg(f.errorString());
    }
    if (f.write(data) != data.size()) {
        const QString err = f.errorString();
        f.close();
        abortUpload(uploadId);
        return tr("Motiva could not write to its cache (%1).").arg(err);
    }
    up.received += data.size();
    up.nextSeq++;
    return QString();
}

void WebMediaCache::abortUpload(const QString& uploadId) {
    m_uploads.remove(uploadId);
    if (isSafeId(uploadId)) {
        QFile::remove(incomingPath(uploadId));
    }
}

QString WebMediaCache::entryDir(const Request& request) {
    const QString key = !request.url.isEmpty() ? request.url : request.uploadId;
    return cacheRoot() + QLatin1Char('/') + shortHash(key);
}

QString WebMediaCache::finalName(const Request& request, const QString& contentType) {
    QString name = sanitizedName(request.name);
    if (name.isEmpty() && !request.url.isEmpty()) {
        name = sanitizedName(QUrl::fromPercentEncoding(QUrl(request.url).path().section(QLatin1Char('/'), -1).toUtf8()));
    }
    const QString suffix = QFileInfo(name).suffix();
    if (!name.isEmpty() && isMediaExtension(suffix)) {
        return name;
    }
    QString ext = extensionForMime(request.mime);
    if (ext.isEmpty()) {
        ext = extensionForMime(contentType);
    }
    if (ext.isEmpty()) {
        return QString();
    }
    QString base = QFileInfo(name).completeBaseName();
    if (base.isEmpty()) {
        base = QStringLiteral("web-media");
    }
    return base + QLatin1Char('.') + ext;
}

// Rejects what is not really a picture/video (an HTML error page saved under
// an image name, a truncated or empty download).
QString WebMediaCache::validate(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() == 0) {
        return tr("The downloaded file is empty.");
    }
    const QByteArray head = f.read(512).trimmed().toLower();
    f.close();
    if (head.startsWith("<!doctype") || head.startsWith("<html") || head.startsWith("<?xml") || head.startsWith("{")) {
        return tr("That address is a web page, not a picture or video file.");
    }
    if (!PlaylistModel::isVideoFile(path)) {
        QImageReader reader(path);
        reader.setDecideFormatFromContent(true);
        if (!reader.canRead()) {
            return tr("Motiva can't read this picture (unsupported or damaged).");
        }
    }
    return QString();
}

void WebMediaCache::fetch(const Request& request, Callback done) {
    if (!request.uploadId.isEmpty()) {
        if (!isSafeId(request.uploadId) || !QFileInfo::exists(incomingPath(request.uploadId))) {
            done(QString(), tr("The browser's upload was not found."));
            return;
        }
        const QString name = finalName(request, QString());
        if (name.isEmpty()) {
            abortUpload(request.uploadId);
            done(QString(), tr("This is not a picture or video type Motiva supports."));
            return;
        }
        const QString dir = entryDir(request);
        const QString target = dir + QLatin1Char('/') + name;
        QDir().mkpath(dir);
        QFile::remove(target);
        if (!QFile::rename(incomingPath(request.uploadId), target)) {
            abortUpload(request.uploadId);
            done(QString(), tr("Motiva could not store the file in its cache."));
            return;
        }
        m_uploads.remove(request.uploadId);
        const QString problem = validate(target);
        if (!problem.isEmpty()) {
            QFile::remove(target);
            done(QString(), problem);
            return;
        }
        done(target, QString());
        return;
    }
    download(request, std::move(done));
}

void WebMediaCache::download(const Request& request, Callback done) {
    const QUrl url(request.url);
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
        done(QString(), tr("Only http and https pictures and videos can be downloaded."));
        return;
    }
    // Already cached from an earlier send of the same address.
    const QString cachedName = finalName(request, QString());
    if (!cachedName.isEmpty()) {
        const QString existing = entryDir(request) + QLatin1Char('/') + cachedName;
        if (QFileInfo(existing).size() > 0) {
            done(existing, QString());
            return;
        }
    }

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                                 "Chrome/124.0 Safari/537.36"));
    req.setRawHeader("Accept", "image/*,video/*,*/*;q=0.5");
    const QUrl referer(request.pageUrl);
    if (referer.isValid() && (referer.scheme() == QLatin1String("http") || referer.scheme() == QLatin1String("https"))) {
        req.setRawHeader("Referer", request.pageUrl.toUtf8());
    }

    QNetworkReply* reply = m_network.get(req);
    QDir().mkpath(incomingDir());
    const QString partialPath =
        incomingDir() + QLatin1Char('/') + shortHash(request.url + QString::number(QDateTime::currentMSecsSinceEpoch())) +
        QStringLiteral("-dl.partial");
    auto file = std::make_shared<QFile>(partialPath);
    auto failed = std::make_shared<QString>();
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reply->abort();
        reply->deleteLater();
        done(QString(), tr("Motiva could not write to its cache."));
        return;
    }

    auto fail = [reply, failed](const QString& why) {
        if (failed->isEmpty()) {
            *failed = why;
            reply->abort();
        }
    };
    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, fail]() {
        const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        if (type.startsWith(QLatin1String("text/")) || type.contains(QLatin1String("mpegurl")) ||
            type.contains(QLatin1String("dash+xml"))) {
            fail(tr("That address is a web page or a streaming playlist, not a picture or video file."));
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [reply, file, fail]() {
        const QByteArray bytes = reply->readAll();
        if (file->size() + bytes.size() > kMaxBytes) {
            fail(tr("The file is larger than 2 GB."));
            return;
        }
        if (file->write(bytes) != bytes.size()) {
            fail(tr("Motiva could not write to its cache."));
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, failed, partialPath, request, done]() {
        file->close();
        reply->deleteLater();
        auto cleanupAndFail = [&](const QString& why) {
            QFile::remove(partialPath);
            done(QString(), why);
        };
        if (!failed->isEmpty()) {
            cleanupAndFail(*failed);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            cleanupAndFail(tr("Download failed: %1").arg(reply->errorString()));
            return;
        }
        const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        const QString name = finalName(request, type);
        if (name.isEmpty()) {
            cleanupAndFail(tr("This is not a picture or video type Motiva supports."));
            return;
        }
        const QString dir = entryDir(request);
        const QString target = dir + QLatin1Char('/') + name;
        QDir().mkpath(dir);
        QFile::remove(target);
        if (!QFile::rename(partialPath, target)) {
            cleanupAndFail(tr("Motiva could not store the file in its cache."));
            return;
        }
        const QString problem = validate(target);
        if (!problem.isEmpty()) {
            QFile::remove(target);
            done(QString(), problem);
            return;
        }
        done(target, QString());
    });
}
