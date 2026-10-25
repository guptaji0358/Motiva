#include "CloudProvider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>

namespace {

QDateTime parseIso(const QString& s) {
    return QDateTime::fromString(s, Qt::ISODate).toUTC();
}

QString errorText(const QJsonObject& root) {
    const QJsonValue err = root.value(QStringLiteral("error"));
    if (err.isObject()) {
        return err.toObject().value(QStringLiteral("message")).toString();
    }
    if (err.isString()) {
        return root.value(QStringLiteral("error_description")).toString(err.toString());
    }
    return root.value(QStringLiteral("error_summary")).toString();
}

// ---------------------------------------------------------------- Google Drive
class GoogleDrive final : public CloudProvider {
public:
    QString id() const override { return QStringLiteral("gdrive"); }
    QString displayName() const override { return QStringLiteral("Google Drive"); }
    QString registrationUrl() const override { return QStringLiteral("https://console.cloud.google.com/apis/credentials"); }
    bool usesClientSecret() const override { return true; }
    QUrl authorizationEndpoint() const override { return QUrl(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth")); }
    QUrl tokenEndpoint() const override { return QUrl(QStringLiteral("https://oauth2.googleapis.com/token")); }
    QString scopes() const override { return QStringLiteral("https://www.googleapis.com/auth/drive.readonly"); }
    QList<QPair<QString, QString>> extraAuthParams() const override {
        return {{QStringLiteral("access_type"), QStringLiteral("offline")}, {QStringLiteral("prompt"), QStringLiteral("consent")}};
    }
    QString rootId() const override { return QStringLiteral("root"); }

    QNetworkRequest listRequest(const QString& folderId, const QString& pageToken, QByteArray*) const override {
        QUrl url(QStringLiteral("https://www.googleapis.com/drive/v3/files"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("q"), QStringLiteral("'%1' in parents and trashed=false").arg(folderId));
        q.addQueryItem(QStringLiteral("fields"),
                       QStringLiteral("nextPageToken,files(id,name,mimeType,size,modifiedTime)"));
        q.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("200"));
        q.addQueryItem(QStringLiteral("orderBy"), QStringLiteral("folder,name"));
        if (!pageToken.isEmpty()) {
            q.addQueryItem(QStringLiteral("pageToken"), pageToken);
        }
        url.setQuery(q);
        return QNetworkRequest(url);
    }

    CloudListPage parseList(const QByteArray& json) const override {
        CloudListPage page;
        const QJsonObject root = QJsonDocument::fromJson(json).object();
        if (root.contains(QStringLiteral("error"))) {
            page.error = errorText(root);
            return page;
        }
        for (const QJsonValue& v : root.value(QStringLiteral("files")).toArray()) {
            const QJsonObject o = v.toObject();
            CloudEntry e;
            e.id = o.value(QStringLiteral("id")).toString();
            e.name = o.value(QStringLiteral("name")).toString();
            e.isFolder = o.value(QStringLiteral("mimeType")).toString() == QLatin1String("application/vnd.google-apps.folder");
            e.size = o.value(QStringLiteral("size")).toString().toLongLong();
            e.modified = parseIso(o.value(QStringLiteral("modifiedTime")).toString());
            // Google-native documents (Docs, Sheets...) have no bytes to download.
            if (!e.isFolder && o.value(QStringLiteral("mimeType")).toString().startsWith(QLatin1String("application/vnd.google-apps."))) {
                continue;
            }
            page.entries << e;
        }
        page.nextPage = root.value(QStringLiteral("nextPageToken")).toString();
        page.ok = true;
        return page;
    }

    QNetworkRequest downloadRequest(const CloudEntry& entry, QByteArray*) const override {
        return QNetworkRequest(QUrl(QStringLiteral("https://www.googleapis.com/drive/v3/files/%1?alt=media").arg(entry.id)));
    }
};

// -------------------------------------------------------------------- OneDrive
class OneDrive final : public CloudProvider {
public:
    QString id() const override { return QStringLiteral("onedrive"); }
    QString displayName() const override { return QStringLiteral("OneDrive"); }
    QString registrationUrl() const override {
        return QStringLiteral("https://entra.microsoft.com/#view/Microsoft_AAD_RegisteredApps/ApplicationsListBlade");
    }
    QUrl authorizationEndpoint() const override {
        return QUrl(QStringLiteral("https://login.microsoftonline.com/common/oauth2/v2.0/authorize"));
    }
    QUrl tokenEndpoint() const override {
        return QUrl(QStringLiteral("https://login.microsoftonline.com/common/oauth2/v2.0/token"));
    }
    QString scopes() const override { return QStringLiteral("Files.Read offline_access"); }
    QString rootId() const override { return QStringLiteral("root"); }

    QNetworkRequest listRequest(const QString& folderId, const QString& pageToken, QByteArray*) const override {
        // pageToken is the full @odata.nextLink.
        if (!pageToken.isEmpty()) {
            return QNetworkRequest(QUrl(pageToken));
        }
        const QString base = folderId == QLatin1String("root")
                                 ? QStringLiteral("https://graph.microsoft.com/v1.0/me/drive/root/children")
                                 : QStringLiteral("https://graph.microsoft.com/v1.0/me/drive/items/%1/children").arg(folderId);
        QUrl url(base);
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("$top"), QStringLiteral("200"));
        q.addQueryItem(QStringLiteral("$select"), QStringLiteral("id,name,size,folder,file,lastModifiedDateTime"));
        url.setQuery(q);
        return QNetworkRequest(url);
    }

    CloudListPage parseList(const QByteArray& json) const override {
        CloudListPage page;
        const QJsonObject root = QJsonDocument::fromJson(json).object();
        if (root.contains(QStringLiteral("error"))) {
            page.error = errorText(root);
            return page;
        }
        for (const QJsonValue& v : root.value(QStringLiteral("value")).toArray()) {
            const QJsonObject o = v.toObject();
            CloudEntry e;
            e.id = o.value(QStringLiteral("id")).toString();
            e.name = o.value(QStringLiteral("name")).toString();
            e.isFolder = o.contains(QStringLiteral("folder"));
            e.size = qint64(o.value(QStringLiteral("size")).toDouble());
            e.modified = parseIso(o.value(QStringLiteral("lastModifiedDateTime")).toString());
            page.entries << e;
        }
        page.nextPage = root.value(QStringLiteral("@odata.nextLink")).toString();
        page.ok = true;
        return page;
    }

    // The /content endpoint redirects to another host; Qt would forward our
    // Authorization header there. Ask for the pre-authenticated URL instead.
    bool needsDownloadUrlLookup() const override { return true; }
    QNetworkRequest downloadUrlLookupRequest(const CloudEntry& entry) const override {
        QUrl url(QStringLiteral("https://graph.microsoft.com/v1.0/me/drive/items/%1").arg(entry.id));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("select"), QStringLiteral("id,@microsoft.graph.downloadUrl"));
        url.setQuery(q);
        return QNetworkRequest(url);
    }
    QUrl parseDownloadUrl(const QByteArray& json) const override {
        return QUrl(QJsonDocument::fromJson(json).object().value(QStringLiteral("@microsoft.graph.downloadUrl")).toString());
    }
    QNetworkRequest downloadRequest(const CloudEntry&, QByteArray*) const override { return {}; }
};

// --------------------------------------------------------------------- Dropbox
class Dropbox final : public CloudProvider {
public:
    QString id() const override { return QStringLiteral("dropbox"); }
    QString displayName() const override { return QStringLiteral("Dropbox"); }
    QString registrationUrl() const override { return QStringLiteral("https://www.dropbox.com/developers/apps"); }
    QUrl authorizationEndpoint() const override { return QUrl(QStringLiteral("https://www.dropbox.com/oauth2/authorize")); }
    QUrl tokenEndpoint() const override { return QUrl(QStringLiteral("https://api.dropboxapi.com/oauth2/token")); }
    QString scopes() const override { return QStringLiteral("files.metadata.read files.content.read"); }
    QList<QPair<QString, QString>> extraAuthParams() const override {
        return {{QStringLiteral("token_access_type"), QStringLiteral("offline")}};
    }
    QString rootId() const override { return QString(); } // Dropbox's root is the empty path

    QNetworkRequest listRequest(const QString& folderId, const QString& pageToken, QByteArray* body) const override {
        const bool more = !pageToken.isEmpty();
        QNetworkRequest req(QUrl(more ? QStringLiteral("https://api.dropboxapi.com/2/files/list_folder/continue")
                                      : QStringLiteral("https://api.dropboxapi.com/2/files/list_folder")));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QJsonObject o;
        if (more) {
            o.insert(QStringLiteral("cursor"), pageToken);
        } else {
            o.insert(QStringLiteral("path"), folderId);
            o.insert(QStringLiteral("limit"), 200);
        }
        *body = QJsonDocument(o).toJson(QJsonDocument::Compact);
        return req;
    }

    CloudListPage parseList(const QByteArray& json) const override {
        CloudListPage page;
        const QJsonObject root = QJsonDocument::fromJson(json).object();
        if (root.contains(QStringLiteral("error")) || root.contains(QStringLiteral("error_summary"))) {
            page.error = errorText(root);
            return page;
        }
        for (const QJsonValue& v : root.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject o = v.toObject();
            const QString tag = o.value(QStringLiteral(".tag")).toString();
            if (tag != QLatin1String("file") && tag != QLatin1String("folder")) {
                continue;
            }
            CloudEntry e;
            e.id = o.value(QStringLiteral("path_lower")).toString();
            e.name = o.value(QStringLiteral("name")).toString();
            e.isFolder = tag == QLatin1String("folder");
            e.size = qint64(o.value(QStringLiteral("size")).toDouble());
            e.modified = parseIso(o.value(QStringLiteral("server_modified")).toString());
            page.entries << e;
        }
        if (root.value(QStringLiteral("has_more")).toBool()) {
            page.nextPage = root.value(QStringLiteral("cursor")).toString();
        }
        page.ok = true;
        return page;
    }

    QNetworkRequest downloadRequest(const CloudEntry& entry, QByteArray* body) const override {
        QNetworkRequest req(QUrl(QStringLiteral("https://content.dropboxapi.com/2/files/download")));
        QJsonObject arg;
        arg.insert(QStringLiteral("path"), entry.id);
        // The header must be 7-bit: escape non-ASCII as \uXXXX.
        QByteArray json = QJsonDocument(arg).toJson(QJsonDocument::Compact);
        QByteArray safe;
        const QString s = QString::fromUtf8(json);
        for (const QChar c : s) {
            if (c.unicode() < 0x80) {
                safe.append(char(c.unicode()));
            } else {
                safe.append(QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0')).toLatin1());
            }
        }
        req.setRawHeader("Dropbox-API-Arg", safe);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
        *body = QByteArray(""); // non-null but empty: CloudManager POSTs whenever the body is not null
        return req;
    }
};

} // namespace

std::unique_ptr<CloudProvider> makeGoogleDriveProvider() { return std::make_unique<GoogleDrive>(); }
std::unique_ptr<CloudProvider> makeOneDriveProvider() { return std::make_unique<OneDrive>(); }
std::unique_ptr<CloudProvider> makeDropboxProvider() { return std::make_unique<Dropbox>(); }
