#pragma once

#include "RcloneRunner.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSettings>

#include <functional>

// Cloud storage through rclone (https://rclone.org, MIT). Motiva does NOT
// talk to Google / Microsoft / Dropbox itself and implements no OAuth: rclone
// does sign-in (in the user's browser), token refresh, listing and transfers,
// and CloudManager only drives it through RcloneJob (QProcess, asynchronous).
//
// Motiva's cloud access is deliberately narrow:
//  * READ ONLY - accounts are connected with read-only scopes where the service
//    allows it, and the code has no upload, sync, delete, rename or create call;
//  * PICTURES AND VIDEOS ONLY (isAllowedMediaName) - other files are not listed
//    and never fetched, whatever the caller asks for;
//  * downloads use `rclone copy` with `--immutable` so an existing, different local
//    file is reported instead of being silently overwritten;
//  * the rclone config (which holds the OAuth tokens) is encrypted by rclone
//    itself with a random password that is kept under Windows DPAPI (current
//    user). The password reaches rclone through its environment only.
// Everything runs on the GUI thread's event loop; no call blocks.

struct CloudProviderInfo {
    QString id;          // "gdrive" | "onedrive" | "dropbox"
    QString displayName;
    QString rcloneType;  // "drive" | "onedrive" | "dropbox"
    QString consoleUrl;  // where the product owner registers an API client
};

struct CloudRemote {
    QString name;        // rclone remote name = the account name shown to the user
    QString type;        // rclone backend type
    QString providerId;  // matches CloudProviderInfo::id, empty for other backends
};

struct CloudEntry {
    QString name;
    QString path;        // relative to the remote root, '/' separated
    QString mime;
    bool isFolder = false;
    qint64 size = 0;
    QDateTime modified;
};

struct CloudToolInfo {
    bool found = false;
    bool bundled = false;
    bool licenseFilePresent = false;
    QString path;
    QString version;     // "1.75.2"
    QString error;       // user-facing, set when !found or unusable
};

struct CloudStep {
    QStringList args;       // rclone arguments after the common ones
    QString label;          // shown to the user
    QString localResult;    // local file this step produces (downloads for playlists)
};

struct CloudPlan {
    QString title;
    QList<CloudStep> steps;
    std::function<void()> cleanup; // runs once when the plan ends (temp files)
    QStringList partialDirs;       // local dirs whose "*.partial" leftovers are removed after a cancel
};

struct CloudOutcome {
    QString title;
    bool ok = false;
    bool cancelled = false;
    bool needsReconnect = false;
    QStringList paths;     // local files produced by steps with a localResult
    QStringList errors;    // one line per failed step
};

class CloudManager : public QObject {
    Q_OBJECT
public:
    static CloudManager* instance();
    ~CloudManager() override;

    static QList<CloudProviderInfo> providers();
    static CloudProviderInfo provider(const QString& id);

    // --- the rclone tool ---
    using ToolCallback = std::function<void(const CloudToolInfo&)>;
    void checkTool(ToolCallback done);
    const CloudToolInfo& toolInfo() const { return m_tool; }
    // Where rclone is looked up: user override, the copy shipped beside Motiva, then PATH.
    static QString locateRclone(bool* bundled = nullptr);

    // Tool verified + config encrypted and readable. Calls back asynchronously.
    using ReadyCallback = std::function<void(bool ok, const QString& error)>;
    void ensureReady(ReadyCallback done);
    bool configUnreadable() const { return m_configUnreadable; }
    // Deletes ONLY Motiva's own rclone config + key (all connected accounts are forgotten).
    bool resetAccounts();
    static QString configPath();
    // Tests may point Motiva at a private config folder / rclone binary.
    void setConfigDirectoryForTesting(const QString& dir);
    void setRclonePathForTesting(const QString& exe);

    // --- API credentials (the product owner's own app registrations) ---
    // Order: value saved in Motiva > motiva-cloud.ini beside rclone.exe > none
    // (rclone then uses ITS shared client, fine for testing, not for release).
    QString clientId(const QString& providerId) const;
    QString clientSecret(const QString& providerId) const;
    void setClientCredentials(const QString& providerId, const QString& id, const QString& secret);
    bool usesSharedClient(const QString& providerId) const { return clientId(providerId).isEmpty(); }

    // --- accounts ---
    using RemotesCallback = std::function<void(const QList<CloudRemote>&, const QString& error)>;
    void listRemotes(RemotesCallback done);
    static bool isValidRemoteName(const QString& name, QString* why = nullptr);
    // Starts rclone's own sign-in (opens the system browser). Reports through
    // connectStatus / connectFinished. One at a time.
    bool connectAccount(const QString& providerId, const QString& name); // always read-only access
    bool reconnectAccount(const QString& name);
    bool isConnecting() const { return m_connectActive; }
    void cancelConnect();
    // Removes the account from Motiva (rclone config delete). Access granted in the
    // provider's own security settings stays until the user revokes it there.
    void removeAccount(const QString& name, std::function<void(bool, const QString&)> done);
    // Cheap reachability/authentication probe of the account root.
    void testAccount(const QString& name, std::function<void(bool ok, bool needsReconnect, const QString&)> done);

    // --- browsing ---
    using ListCallback = std::function<void(bool ok, const QList<CloudEntry>&, const QString& error, bool needsReconnect)>;
    void list(const QString& remote, const QString& path, ListCallback done);

    // --- plans (one at a time) ---
    bool busy() const { return m_planActive; }
    bool startPlan(const CloudPlan& plan);
    void cancelTransfer();
    void cancelDownloads() { cancelTransfer(); } // CleanupManager's name for it

    // Builders. Read-only: they only copy FROM the cloud, and skip anything that is
    // not a picture or video.
    // Files for a playlist: downloaded into the local cache (reused when unchanged).
    CloudPlan planCacheDownload(const QString& remote, const QList<CloudEntry>& files) const;
    // Download into a chosen folder; an existing different local file is reported, not replaced.
    CloudPlan planDownloadTo(const QString& remote, const QList<CloudEntry>& entries, const QString& localDir) const;

    // Picture / video file name test (extension based); folders are not judged here.
    static bool isAllowedMediaName(const QString& fileName);

    static QString remotePath(const QString& remote, const QString& path);
    // --- local cache of files fetched for playlists ---
    static QString cacheRoot();
    QString cachePathFor(const QString& remote, const CloudEntry& entry) const;

signals:
    void connectStatus(const QString& message);
    void connectFinished(const QString& name, bool ok, const QString& message);
    void transferStarted(const QString& title);
    void transferProgress(const RcloneProgress& p, int stepIndex, int stepCount, const QString& label);
    void transferLog(const QString& line);
    void transferFinished(const CloudOutcome& outcome);

private:
    explicit CloudManager(QObject* parent = nullptr);

    QProcessEnvironment environment(bool withConfigPassword) const;
    QStringList commonArgs() const;
    RcloneJob* makeJob(const QStringList& args, bool withConfigPassword = true);
    void prepareConfig();
    void finishReady(bool ok, const QString& error);
    bool isConfigEncrypted() const;
    QByteArray loadKey() const;
    void storeKey(const QByteArray& key);
    void clearKey();

    void configStep(const QString& name, const QByteArray& jsonOut, int iteration, bool createdByUs);
    void failConnect(const QString& name, const QString& message, bool deleteRemote);
    void runConnectJob(const QString& name, const QStringList& args, int iteration, bool createdByUs);

    void runNextStep();
    void finishPlan();
    QString m_configDirOverride;
    QString m_rcloneOverride;

    mutable QSettings m_settings;
    CloudToolInfo m_tool;
    bool m_toolChecked = false;
    bool m_ready = false;
    bool m_readyBusy = false;
    bool m_configUnreadable = false;
    QByteArray m_key;
    QList<ReadyCallback> m_readyWaiters;

    // account connection
    bool m_connectActive = false;
    QPointer<RcloneJob> m_connectJob;
    bool m_connectCancelled = false;

    // plan execution
    bool m_planActive = false;
    CloudPlan m_plan;
    int m_stepIndex = 0;
    CloudOutcome m_outcome;
    QPointer<RcloneJob> m_stepJob;
    bool m_cancelRequested = false;
};
