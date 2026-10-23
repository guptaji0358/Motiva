#pragma once

#include "CloudManager.h"
#include "GlWindow.h"

#include <functional>

class QStandardItemModel;

// "Cloud Storage": connect Google Drive / OneDrive / Dropbox accounts (through
// rclone), browse them, download, upload, synchronise, back up and restore.
// A GlWindow like the other utility windows: GL-drawn items on one canvas,
// four pages (Accounts / Browse / Sync / Backup & Restore) and a transfer
// panel that stays visible at the bottom.
//
// Opened from a playlist, the Browse page also offers "Add to playlist": the
// chosen files are downloaded into Motiva's local cache and the playlist then
// references those local copies, so playback never depends on the network.
class CloudWindow : public GlWindow {
    Q_OBJECT
public:
    explicit CloudWindow(QWidget* parent = nullptr);

    // Which playlist the files are for and which file names it can hold.
    void setTarget(const QString& playlistName, std::function<bool(const QString& fileName)> accepts);
    void present();

    void layoutContent(const QRectF& content, const QRectF& footer) override;

signals:
    // Local, cached copies ready to be added to the playlist.
    void filesReady(const QStringList& localPaths);

private:
    enum Page { AccountsPage, BrowsePage, SyncPage, BackupPage, PageCount };

    struct ProviderRow {
        QString id;
        GlPanel* card = nullptr;
        GlLabel* name = nullptr;
        GlLabel* hint = nullptr;
        GlButton* creds = nullptr;
        GlButton* connect = nullptr;
    };

    void buildHeader();
    void buildAccountsPage();
    void buildBrowsePage();
    void buildSyncPage();
    void buildBackupPage();
    void buildTransferPanel();

    GlButton* makeButton(Page page, const QString& text, GlButton::Kind kind = GlButton::Kind::Secondary);
    GlLabel* makeLabel(Page page, qreal pointSize = 10, bool dim = false, bool wrap = false);
    GlItem* own(Page page, GlItem* item);

    void showPage(int page);
    void updateVisibility();
    void layoutAccounts(const QRectF& area);
    void layoutBrowse(const QRectF& area);
    void layoutSync(const QRectF& area);
    void layoutBackup(const QRectF& area);
    void layoutTransfer(const QRectF& area, bool showLog);

    void refreshTool();
    void reloadRemotes();
    void fillRemoteCombo(GlDropdown* combo);
    void showBanner(const QString& text, bool isError, const QString& reconnectName = QString());
    void appendLog(const QString& line);
    static void setLines(QStandardItemModel* model, const QStringList& lines);

    // accounts
    void connectProvider(const QString& providerId);
    void editCredentials(const QString& providerId);
    void refreshProviderHints();
    void onConnectFinished(const QString& name, bool ok, const QString& message);
    QString selectedAccount() const;
    void reconnect(const QString& name);

    // browse
    void openFolder(const QString& path);
    void reloadListing();
    void populateList();
    void updatePathLabel(const QString& remote);
    void toggleRow(int row);
    void setAllChecked(bool on);
    void goUp();
    void makeFolder();
    void downloadSelected();
    void uploadFiles();
    void uploadFolder();
    void uploadPaths(const QStringList& paths);
    void addToPlaylist();
    QList<CloudEntry> selectedEntries() const;

    // sync
    void previewSync();
    void runSync();
    void verifySync();
    bool syncEndpoints(QString* src, QString* dst, QString* localDir, QString* remote, QString* remoteDir) const;
    void invalidatePreview();
    void setSyncReport(const QString& text);
    void updateModeHint();

    // backup
    void backupLibrary();
    void backupFolder();
    void refreshBackups();
    void restoreBackup();

    // transfers
    bool begin(const CloudPlan& plan);
    void onTransferProgress(const RcloneProgress& p, int stepIndex, int stepCount, const QString& label);
    void onTransferFinished(const CloudOutcome& o);
    void setBusy(bool busy);

    CloudManager* m_cloud;
    std::function<bool(const QString&)> m_accepts;
    QString m_playlistName;
    QList<CloudRemote> m_remotes;

    // header
    GlLabel* m_toolLabel = nullptr;
    GlLabel* m_banner = nullptr;
    GlButton* m_reconnectButton = nullptr;
    QString m_reconnectName;
    GlButton* m_tabButtons[PageCount] = {};
    GlButton* m_closeButton = nullptr;
    QList<GlItem*> m_pageItems[PageCount];
    int m_page = AccountsPage;
    bool m_bannerOn = false;
    QString m_selectRemote; // account to open in Browse once the account list has reloaded

    // accounts
    QList<ProviderRow> m_providers;
    QHash<QString, GlButton*> m_connectButtons;
    QHash<QString, GlLabel*> m_providerHints;
    GlLabel* m_accountsIntro = nullptr;
    GlToggle* m_readOnly = nullptr;
    GlButton* m_cancelConnect = nullptr;
    GlLabel* m_connectStatus = nullptr;
    GlItemView* m_accounts = nullptr;
    QStandardItemModel* m_accountsModel = nullptr;
    GlButton* m_reconnectAccount = nullptr;
    GlButton* m_testAccount = nullptr;
    GlButton* m_removeAccount = nullptr;
    GlButton* m_resetAccounts = nullptr;
    QString m_connectingProvider;
    bool m_connectingUi = false;
    bool m_resetVisible = false;
    bool m_credsVisible = false;

    // browse
    GlDropdown* m_browseRemote = nullptr;
    GlButton* m_up = nullptr;
    GlButton* m_refresh = nullptr;
    GlButton* m_newFolder = nullptr;
    GlTextInput* m_search = nullptr;
    GlLabel* m_pathLabel = nullptr;
    GlItemView* m_list = nullptr;
    QStandardItemModel* m_listModel = nullptr;
    GlButton* m_download = nullptr;
    GlButton* m_uploadFiles = nullptr;
    GlButton* m_uploadFolder = nullptr;
    GlButton* m_addToPlaylist = nullptr;
    GlButton* m_selectAll = nullptr;
    GlButton* m_clearSelection = nullptr;
    GlButton* m_prevPage = nullptr;
    GlButton* m_nextPage = nullptr;
    GlLabel* m_pageLabel = nullptr;
    QString m_path;
    int m_listPage = 0;
    QList<CloudEntry> m_entries;
    int m_listGeneration = 0;
    bool m_haveTarget = false;

    // sync
    GlLabel* m_syncIntro = nullptr;
    GlLabel* m_syncRemoteLabel = nullptr;
    GlLabel* m_syncRemoteDirLabel = nullptr;
    GlLabel* m_syncLocalLabel = nullptr;
    GlLabel* m_syncDirectionLabel = nullptr;
    GlLabel* m_syncModeLabel = nullptr;
    GlLabel* m_syncModeHint = nullptr;
    GlDropdown* m_syncRemote = nullptr;
    GlTextInput* m_syncRemoteDir = nullptr;
    GlTextInput* m_syncLocal = nullptr;
    GlButton* m_syncPick = nullptr;
    GlDropdown* m_syncDirection = nullptr;
    GlDropdown* m_syncMode = nullptr;
    GlButton* m_syncPreview = nullptr;
    GlButton* m_syncRun = nullptr;
    GlButton* m_syncVerify = nullptr;
    GlLabel* m_syncSummary = nullptr;
    GlItemView* m_syncReport = nullptr;
    QStandardItemModel* m_syncModel = nullptr;
    bool m_previewValid = false;
    CloudSyncPreview m_lastPreview;
    QString m_previewKey;

    // backup
    GlLabel* m_backupIntro = nullptr;
    GlLabel* m_backupListLabel = nullptr;
    GlDropdown* m_backupRemote = nullptr;
    GlItemView* m_backups = nullptr;
    QStandardItemModel* m_backupModel = nullptr;
    QList<CloudEntry> m_backupEntries;
    GlButton* m_backupLibraryButton = nullptr;
    GlButton* m_backupFolderButton = nullptr;
    GlButton* m_backupRefresh = nullptr;
    GlButton* m_restoreButton = nullptr;

    // transfer panel
    GlPanel* m_xferPanel = nullptr;
    GlLabel* m_xferTitle = nullptr;
    GlProgressBar* m_xferBar = nullptr;
    GlLabel* m_xferDetail = nullptr;
    GlButton* m_xferCancel = nullptr;
    GlItemView* m_log = nullptr;
    QStandardItemModel* m_logModel = nullptr;

    bool m_addAfterDownload = false;
    QString m_restoreSavedAs;
    QString m_planRemote;
    QString m_planKind;
};
