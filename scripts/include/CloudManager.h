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
// Safe defaults:
//  * imports and uploads use `rclone copy/copyto` (never delete) and
//    `--immutable` so an existing, different file is reported instead of being
//    silently overwritten (uploads overwrite only after the user said so);
//  * mirroring (`sync`) is only started after a dry-run preview was shown and
//    confirmed, and always with --backup-dir so deleted / replaced files are
//    moved aside instead of destroyed, plus --max-delete as a brake;
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

// Result of a dry run / check. Paths are relative to the compared folders.
struct CloudSyncPreview {
    bool ok = false;
    QString error;
    QStringList added;    // would be copied (new on the destination)
    QStringList changed;  // would replace a different destination file
    QStringList removed;  // would be removed from the destination (sync only)
    int unchanged = 0;
    int problems = 0;     // "!" lines
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
    bool connectAccount(const QString& providerId, const QString& name, bool readOnly);
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
    void makeFolder(const QString& remote, const QString& path, std::function<void(bool, const QString&)> done);

    // --- plans (one at a time) ---
    bool busy() const { return m_planActive; }
    bool startPlan(const CloudPlan& plan);
    void cancelTransfer();
    void cancelDownloads() { cancelTransfer(); } // CleanupManager's name for it

    // Builders. All of them only copy; nothing here deletes anything.
    // Files for a playlist: downloaded into the local cache (reused when unchanged).
    CloudPlan planCacheDownload(const QString& remote, const QList<CloudEntry>& files) const;
    // Download into a chosen folder; an existing different local file is reported, not replaced.
    CloudPlan planDownloadTo(const QString& remote, const QList<CloudEntry>& entries, const QString& localDir) const;
    CloudPlan planUpload(const QString& remote, const QString& remoteDir, const QStringList& localPaths, bool overwrite) const;
    // Copy a whole folder (never deletes anything at the destination; a
    // destination file that gets replaced is first moved to `trashDir`).
    CloudPlan planCopyFolder(const QString& title, const QString& src, const QString& dst, const QString& trashDir) const;
    // Mirror: makes `dst` equal to `src`. Only after previewTransfer + confirmation.
    // `trashDir` receives replaced/removed destination files; rclone refuses to
    // delete more than `maxDelete` files.
    CloudPlan planMirror(const QString& src, const QString& dst, const QString& trashDir, int maxDelete) const;
    // Two-way merge of a PC folder and a cloud folder. Files present on one side only are copied
    // to the other (--ignore-existing: nothing is replaced); a file that differs on both sides
    // is a CONFLICT: both versions are kept - the cloud one is saved next to the local one as
    // "<name> (cloud copy <time>).<ext>". Deletions are never propagated (without a baseline a
    // missing file cannot be told apart from a file that was never there, and a network failure
    // must never count as a deletion).
    CloudPlan planTwoWay(const QString& localDir, const QString& remoteSpec, const QStringList& conflicts) const;
    // A timestamped folder on `remote` (or on disk when remote is empty) for replaced/removed files.
    static QString newTrashDir(const QString& remote);
    // Library snapshot (VACUUM INTO, consistent) uploaded as a new, timestamped file.
    bool planBackupLibrary(const QString& remote, const QString& libraryFile, CloudPlan* plan, QString* error) const;
    CloudPlan planBackupFolder(const QString& remote, const QString& localDir) const;
    // Restore a backed-up file to a local folder under a fresh name (never replaces the live library).
    CloudPlan planRestore(const QString& remote, const CloudEntry& backup, const QString& localDir, QString* savedAs) const;

    using PreviewCallback = std::function<void(const CloudSyncPreview&)>;
    // Dry run (`sync` = mirror, otherwise copy). Nothing is changed.
    void previewTransfer(bool mirror, const QString& src, const QString& dst, PreviewCallback done);
    // Compares two folders by size/hash (rclone check).
    void verify(const QString& src, const QString& dst, PreviewCallback done);

    static QString remotePath(const QString& remote, const QString& path);
    static QString backupFolderName() { return QStringLiteral("Motiva Backup"); }
    static QString syncTrashFolderName() { return QStringLiteral("Motiva Sync Trash"); }
    static QString localTrashRoot();
    static QString uniqueLocalName(const QString& dir, const QString& name);

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
