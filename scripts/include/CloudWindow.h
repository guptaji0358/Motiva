#pragma once

#include "CloudManager.h"
#include "GlWindow.h"

#include <functional>

class QStandardItemModel;

// "Cloud Storage": connect Google Drive / OneDrive / Dropbox accounts (through
// rclone), browse them (READ ONLY) and download pictures and videos. Nothing is ever uploaded,
// changed or deleted in the cloud, and only picture / video files are listed.
// A GlWindow like the other utility windows: GL-drawn items on one canvas,
// two pages (Accounts / Browse) and a transfer
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
    enum Page { AccountsPage, BrowsePage, PageCount };

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
    void buildTransferPanel();

    GlButton* makeButton(Page page, const QString& text, GlButton::Kind kind = GlButton::Kind::Secondary);
    GlLabel* makeLabel(Page page, qreal pointSize = 10, bool dim = false, bool wrap = false);
    GlItem* own(Page page, GlItem* item);

    void showPage(int page);
    void updateVisibility();
    void layoutAccounts(const QRectF& area);
    void layoutBrowse(const QRectF& area);
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
    void downloadSelected();
    void addToPlaylist();
    QList<CloudEntry> selectedEntries() const;

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
    GlTextInput* m_search = nullptr;
    GlLabel* m_pathLabel = nullptr;
    GlItemView* m_list = nullptr;
    QStandardItemModel* m_listModel = nullptr;
    GlButton* m_download = nullptr;
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

    // transfer panel
    GlPanel* m_xferPanel = nullptr;
    GlLabel* m_xferTitle = nullptr;
    GlProgressBar* m_xferBar = nullptr;
    GlLabel* m_xferDetail = nullptr;
    GlButton* m_xferCancel = nullptr;
    GlItemView* m_log = nullptr;
    QStandardItemModel* m_logModel = nullptr;

    bool m_addAfterDownload = false;
    QString m_planRemote;
    QString m_planKind;
};
