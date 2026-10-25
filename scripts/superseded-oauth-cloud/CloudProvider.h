#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QNetworkRequest>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>

// One file or folder in a cloud drive.
struct CloudEntry {
    QString id;       // provider handle (Drive file id, OneDrive item id, Dropbox path_lower)
    QString name;
    bool isFolder = false;
    qint64 size = 0;
    QDateTime modified; // UTC
};

struct CloudListPage {
    bool ok = false;
    QString error;           // user-facing
    QList<CloudEntry> entries;
    QString nextPage;        // opaque continuation token, empty when done
};

// Describes ONE cloud service: its OAuth endpoints and how to list/download.
// Providers are stateless (no network objects, no tokens) - CloudManager owns
// the network, the sign-in flow and the tokens and asks the provider to build
// requests. That keeps the three services small and testable.
class CloudProvider {
public:
    virtual ~CloudProvider() = default;

    virtual QString id() const = 0;           // "gdrive" | "onedrive" | "dropbox"
    virtual QString displayName() const = 0;
    // Where the user registers the app to get a client id (shown in setup help).
    virtual QString registrationUrl() const = 0;
    // True when the service's token endpoint wants a client secret even for
    // desktop apps (Google does).
    virtual bool usesClientSecret() const { return false; }

    // --- OAuth 2.0 authorization code + PKCE ---
    virtual QUrl authorizationEndpoint() const = 0;
    virtual QUrl tokenEndpoint() const = 0;
    virtual QString scopes() const = 0;
    virtual QList<QPair<QString, QString>> extraAuthParams() const { return {}; }

    // --- Browsing ---
    // Id of the drive's root folder.
    virtual QString rootId() const = 0;
    // Request that lists the children of `folderId` (page `pageToken` or the
    // first page when empty). `postBody` arrives null; a provider that needs a
    // POST assigns it a non-null (possibly empty) body, otherwise it is a GET.
    virtual QNetworkRequest listRequest(const QString& folderId, const QString& pageToken, QByteArray* postBody) const = 0;
    virtual CloudListPage parseList(const QByteArray& json) const = 0;

    // --- Download ---
    // Request that returns the file's bytes directly.
    virtual QNetworkRequest downloadRequest(const CloudEntry& entry, QByteArray* postBody) const = 0;
    // OneDrive hands out a short-lived pre-authenticated URL instead; when
    // this returns a request, CloudManager GETs it first and expects the
    // download URL in the JSON reply (see parseDownloadUrl). Default: none.
    virtual bool needsDownloadUrlLookup() const { return false; }
    virtual QNetworkRequest downloadUrlLookupRequest(const CloudEntry&) const { return {}; }
    virtual QUrl parseDownloadUrl(const QByteArray&) const { return {}; }
};

std::unique_ptr<CloudProvider> makeGoogleDriveProvider();
std::unique_ptr<CloudProvider> makeOneDriveProvider();
std::unique_ptr<CloudProvider> makeDropboxProvider();
