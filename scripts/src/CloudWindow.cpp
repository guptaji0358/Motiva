#include "CloudWindow.h"

#include "GlPopups.h"
#include "LibraryDatabase.h"

#include <QApplication>
#include <QCollator>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kEntryRole = Qt::UserRole + 1;
constexpr int kFolderRole = Qt::UserRole + 2;
constexpr int kSizeRole = Qt::UserRole + 3;
constexpr int kDateRole = Qt::UserRole + 4;
constexpr int kMaxLogLines = 300;
constexpr qreal kRowH = 40;

QString sizeText(qint64 bytes) {
    return QLocale().formattedDataSize(qMax<qint64>(0, bytes), 1, QLocale::DataSizeTraditionalFormat);
}

QString etaText(int seconds) {
    if (seconds < 0) {
        return QString();
    }
    if (seconds >= 3600) {
        return QObject::tr("%1 h %2 min left").arg(seconds / 3600).arg((seconds % 3600) / 60);
    }
    if (seconds >= 60) {
        return QObject::tr("%1 min %2 s left").arg(seconds / 60).arg(seconds % 60);
    }
    return QObject::tr("%1 s left").arg(seconds);
}

QString trimSlashes(QString s) {
    s = s.trimmed();
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (s.startsWith(QLatin1Char('/'))) {
        s.remove(0, 1);
    }
    while (s.endsWith(QLatin1Char('/'))) {
        s.chop(1);
    }
    return s;
}

QString parentOf(const QString& path) {
    const int i = path.lastIndexOf(QLatin1Char('/'));
    return i < 0 ? QString() : path.left(i);
}

void clearRows(QStandardItemModel* model) {
    model->removeRows(0, model->rowCount());
}

// Cloud folder listing: check box, folder / file glyph, name, size, date.
class CloudRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(200, 38); }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const GlUi::Style s = GlUi::Style::current();
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(option.rect).adjusted(2, 2, -2, -2);
        const bool folder = index.data(kFolderRole).toBool();
        const bool checkable = index.data(Qt::CheckStateRole).isValid();
        const bool checked = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        const qreal rad = 10 * s.radius + 3;
        if (checked || (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))) {
            QColor c = s.accent;
            c.setAlphaF(checked ? 0.22 : 0.12);
            p->setPen(Qt::NoPen);
            p->setBrush(c);
            p->drawRoundedRect(r, rad, rad);
        }
        qreal x = r.left() + 12;
        if (checkable) {
            const QRectF box(x, r.center().y() - 9, 18, 18);
            const qreal br = 5 * s.radius + 2;
            if (checked) {
                QLinearGradient g(box.topLeft(), box.bottomRight());
                g.setColorAt(0, s.accent);
                g.setColorAt(1, s.accent2);
                p->setPen(Qt::NoPen);
                p->setBrush(g);
                p->drawRoundedRect(box, br, br);
                p->setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p->drawPolyline(QPolygonF({box.center() + QPointF(-4.5, 0), box.center() + QPointF(-1.5, 3.5),
                                           box.center() + QPointF(4.5, -3.5)}));
            } else {
                p->setPen(QPen(s.textDim, 1.4));
                p->setBrush(Qt::NoBrush);
                p->drawRoundedRect(box.adjusted(0.7, 0.7, -0.7, -0.7), br, br);
            }
            x += 30;
        }
        if (folder) {
            // Folder glyph: a tab and a body.
            const QRectF f(x, r.center().y() - 8, 20, 16);
            p->setPen(Qt::NoPen);
            p->setBrush(s.accent);
            p->drawRoundedRect(QRectF(f.left(), f.top(), 9, 5), 2, 2);
            p->drawRoundedRect(QRectF(f.left(), f.top() + 3, f.width(), f.height() - 3), 3, 3);
        } else {
            const QRectF f(x + 3, r.center().y() - 8, 14, 17);
            p->setPen(QPen(s.textDim, 1.4));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(f, 3, 3);
            p->drawLine(QPointF(f.left() + 3, f.center().y() - 1), QPointF(f.right() - 3, f.center().y() - 1));
            p->drawLine(QPointF(f.left() + 3, f.center().y() + 3), QPointF(f.right() - 3, f.center().y() + 3));
        }
        x += 32;
        constexpr qreal kSizeW = 90, kDateW = 150;
        p->setFont(option.font);
        const qreal nameW = std::max<qreal>(40, r.right() - x - kSizeW - kDateW - 20);
        p->setPen(s.text);
        p->drawText(QRectF(x, r.top(), nameW, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                    p->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, int(nameW)));
        p->setPen(s.textDim);
        p->drawText(QRectF(r.right() - kSizeW - kDateW - 12, r.top(), kSizeW, r.height()), Qt::AlignVCenter | Qt::AlignRight,
                    index.data(kSizeRole).toString());
        p->drawText(QRectF(r.right() - kDateW - 8, r.top(), kDateW, r.height()), Qt::AlignVCenter | Qt::AlignRight,
                    index.data(kDateRole).toString());
        p->restore();
    }
};

QPushButton* plainButton(const QString& text, QWidget* parent = nullptr) {
    auto* b = new QPushButton(text, parent);
    b->setAutoDefault(false);
    return b;
}

} // namespace

CloudWindow::CloudWindow(QWidget* parent)
    : GlWindow(tr("Cloud Storage"), tr("Google Drive, OneDrive and Dropbox through rclone"), parent),
      m_cloud(CloudManager::instance()) {
    setModal(false);

    buildHeader();
    buildAccountsPage();
    buildBrowsePage();
    buildSyncPage();
    buildBackupPage();
    buildTransferPanel();

    m_closeButton = new GlButton(canvas(), GlButton::Kind::Secondary);
    m_closeButton->setText(tr("Close"));
    connect(m_closeButton, &GlButton::clicked, this, &QDialog::close);

    connect(m_cloud, &CloudManager::connectStatus, this, [this](const QString& s) { m_connectStatus->setText(s); });
    connect(m_cloud, &CloudManager::connectFinished, this, &CloudWindow::onConnectFinished);
    connect(m_cloud, &CloudManager::transferStarted, this, [this](const QString& title) {
        m_xferTitle->setText(title);
        m_xferBar->setIndeterminate(true);
        m_xferBar->setText(QString());
        m_xferDetail->setText(tr("Starting…"));
        m_xferCancel->setEnabled(true);
        setBusy(true);
    });
    connect(m_cloud, &CloudManager::transferProgress, this, &CloudWindow::onTransferProgress);
    connect(m_cloud, &CloudManager::transferLog, this, [this](const QString& line) { appendLog(line); });
    connect(m_cloud, &CloudManager::transferFinished, this, &CloudWindow::onTransferFinished);

    refreshProviderHints();
    updateModeHint();
    setBusy(false);
    showPage(AccountsPage);

    const QScreen* screen = QGuiApplication::primaryScreen();
    const QSize avail = screen ? screen->availableGeometry().size() : QSize(1280, 800);
    setMinimumSize(std::min(860, int(avail.width() * 0.92)), std::min(800, int(avail.height() * 0.92)));
    resize(std::min(1040, int(avail.width() * 0.92)), std::min(900, int(avail.height() * 0.92)));
}

// ------------------------------------------------------------------ construction

GlItem* CloudWindow::own(Page page, GlItem* item) {
    m_pageItems[page] << item;
    return item;
}

GlButton* CloudWindow::makeButton(Page page, const QString& text, GlButton::Kind kind) {
    auto* b = new GlButton(canvas(), kind);
    b->setText(text);
    own(page, b);
    return b;
}

GlLabel* CloudWindow::makeLabel(Page page, qreal pointSize, bool dim, bool wrap) {
    auto* l = new GlLabel(canvas());
    l->setPointSize(pointSize);
    if (dim) {
        l->setTone(GlLabel::Tone::Dim);
    }
    if (wrap) {
        l->setWordWrap(true);
        l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    }
    own(page, l);
    return l;
}

void CloudWindow::buildHeader() {
    GlCanvas* c = canvas();
    m_toolLabel = new GlLabel(c);
    m_toolLabel->setPointSize(9.5);
    m_toolLabel->setTone(GlLabel::Tone::Dim);
    m_toolLabel->setWordWrap(true);
    m_toolLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    m_banner = new GlLabel(c);
    m_banner->setPointSize(10);
    m_banner->setWordWrap(true);
    m_banner->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_banner->setVisible(false);
    m_reconnectButton = new GlButton(c, GlButton::Kind::Secondary);
    m_reconnectButton->setText(tr("Reconnect…"));
    m_reconnectButton->setVisible(false);
    connect(m_reconnectButton, &GlButton::clicked, this, [this] { reconnect(m_reconnectName); });

    const QStringList names{tr("Accounts"), tr("Browse"), tr("Sync"), tr("Backup & Restore")};
    for (int i = 0; i < PageCount; ++i) {
        m_tabButtons[i] = new GlButton(c, GlButton::Kind::Tab);
        m_tabButtons[i]->setText(names.at(i));
        connect(m_tabButtons[i], &GlButton::clicked, this, [this, i] { showPage(i); });
    }
}

void CloudWindow::buildAccountsPage() {
    GlCanvas* c = canvas();
    m_accountsIntro = makeLabel(AccountsPage, 10, true, true);
    m_accountsIntro->setText(tr("Connect an account. Sign-in happens in your web browser and is handled by rclone; "
                                "Motiva never sees your password."));

    // Client IDs are the product owner's business (motiva-cloud.ini); end users never see them.
    m_credsVisible = qEnvironmentVariableIsSet("MOTIVA_CLOUD_DEV");
    for (const CloudProviderInfo& p : CloudManager::providers()) {
        ProviderRow row;
        row.id = p.id;
        row.card = new GlPanel(c);
        row.card->setRadiusPx(14);
        own(AccountsPage, row.card);
        row.name = makeLabel(AccountsPage, 11);
        row.name->setBold(true);
        row.name->setText(p.displayName);
        row.hint = makeLabel(AccountsPage, 9.5, true, true);
        m_providerHints.insert(p.id, row.hint);
        row.creds = new GlButton(c, GlButton::Kind::Secondary);
        row.creds->setText(tr("API credentials…"));
        connect(row.creds, &GlButton::clicked, this, [this, id = p.id] { editCredentials(id); });
        row.connect = new GlButton(c, GlButton::Kind::Primary);
        row.connect->setText(tr("Connect"));
        connect(row.connect, &GlButton::clicked, this, [this, id = p.id] { connectProvider(id); });
        own(AccountsPage, row.connect);
        m_connectButtons.insert(p.id, row.connect);
        m_providers << row;
    }

    m_readOnly = new GlToggle(c);
    m_readOnly->setText(tr("Google Drive: read-only access (cannot upload, sync or back up to this account)"));
    own(AccountsPage, m_readOnly);

    m_connectStatus = makeLabel(AccountsPage, 10, true);
    m_cancelConnect = makeButton(AccountsPage, tr("Cancel sign-in"));
    connect(m_cancelConnect, &GlButton::clicked, m_cloud, &CloudManager::cancelConnect);

    m_accountsModel = new QStandardItemModel(this);
    m_accounts = new GlItemView(c);
    m_accounts->setModel(m_accountsModel);
    m_accounts->setDelegate(new GlTableDelegate({0.34, 0.26, 0.40}, m_accounts));
    m_accounts->setMode(GlItemView::Mode::List, QSize(200, 34), 0);
    m_accounts->setAccessibleName(tr("Connected accounts"));
    m_accounts->setEmptyText(tr("No account connected yet"), tr("Use Connect above to add one."));
    own(AccountsPage, m_accounts);

    m_testAccount = makeButton(AccountsPage, tr("Test connection"));
    m_reconnectAccount = makeButton(AccountsPage, tr("Reconnect…"));
    m_removeAccount = makeButton(AccountsPage, tr("Remove account"));
    m_resetAccounts = makeButton(AccountsPage, tr("Reset cloud accounts…"));
    m_resetAccounts->setToolTip(tr("Forgets every connected account stored by Motiva. Files in the cloud are not touched."));

    connect(m_testAccount, &GlButton::clicked, this, [this] {
        const QString name = selectedAccount();
        if (name.isEmpty()) {
            showBanner(tr("Select an account in the list first."), false);
            return;
        }
        showBanner(tr("Testing %1…").arg(name), false);
        m_cloud->testAccount(name, [this, name](bool ok, bool needsReconnect, const QString& error) {
            for (int i = 0; i < m_accountsModel->rowCount(); ++i) {
                if (m_accountsModel->item(i, 0)->data(Qt::UserRole).toString() == name) {
                    m_accountsModel->item(i, 2)->setText(ok ? tr("Connected")
                                                           : needsReconnect ? tr("Sign-in expired - reconnect")
                                                                            : tr("Problem - see message above"));
                }
            }
            if (ok) {
                showBanner(tr("%1 is connected and reachable.").arg(name), false);
            } else {
                showBanner(error, true, needsReconnect ? name : QString());
            }
        });
    });
    connect(m_reconnectAccount, &GlButton::clicked, this, [this] {
        const QString name = selectedAccount();
        if (name.isEmpty()) {
            showBanner(tr("Select an account in the list first."), false);
            return;
        }
        reconnect(name);
    });
    connect(m_removeAccount, &GlButton::clicked, this, [this] {
        const QString name = selectedAccount();
        if (name.isEmpty()) {
            showBanner(tr("Select an account in the list first."), false);
            return;
        }
        if (GlMessageBox::question(this, tr("Remove account"),
                                   tr("Remove “%1” from Motiva?\n\nFiles in the cloud and files already downloaded are not "
                                      "touched. To fully revoke Motiva's access, also remove it in the service's own "
                                      "security settings.")
                                       .arg(name),
                                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        m_cloud->removeAccount(name, [this, name](bool ok, const QString& error) {
            showBanner(ok ? tr("“%1” was removed.").arg(name) : error, !ok);
            reloadRemotes();
        });
    });
    connect(m_resetAccounts, &GlButton::clicked, this, [this] {
        if (GlMessageBox::warning(this, tr("Reset cloud accounts"),
                                  tr("This forgets ALL connected cloud accounts in Motiva. Nothing is deleted in the cloud and "
                                     "downloaded files stay on this PC. You will need to connect again.\n\nContinue?"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        if (!m_cloud->resetAccounts()) {
            showBanner(tr("Couldn't reset right now (a transfer or sign-in is running)."), true);
            return;
        }
        showBanner(tr("Cloud accounts were reset."), false);
        reloadRemotes();
    });
}

void CloudWindow::buildBrowsePage() {
    GlCanvas* c = canvas();
    m_browseRemote = new GlDropdown(c);
    m_browseRemote->setCompact(true);
    m_browseRemote->setToolTip(tr("Account to browse"));
    own(BrowsePage, m_browseRemote);
    m_up = makeButton(BrowsePage, tr("Up"));
    m_refresh = makeButton(BrowsePage, tr("Refresh"));
    m_newFolder = makeButton(BrowsePage, tr("New folder…"));
    m_search = new GlTextInput(c);
    m_search->setPlaceholder(tr("Search this folder"));
    m_search->setClearable(true);
    own(BrowsePage, m_search);

    m_pathLabel = makeLabel(BrowsePage, 10);
    m_pathLabel->setBold(true);

    m_listModel = new QStandardItemModel(this);
    m_list = new GlItemView(c);
    m_list->setModel(m_listModel);
    m_list->setDelegate(new CloudRowDelegate(m_list));
    m_list->setMode(GlItemView::Mode::List, QSize(200, 38), 2);
    m_list->setAccessibleName(tr("Files and folders in the cloud"));
    m_list->setEmptyText(tr("Nothing to show"), tr("Connect an account, then open a folder."));
    own(BrowsePage, m_list);
    connect(m_list, &GlItemView::clicked, this, &CloudWindow::toggleRow);
    connect(m_list, &GlItemView::doubleClicked, this, [this](int row) {
        const int i = m_listModel->item(row) ? m_listModel->item(row)->data(kEntryRole).toInt() : -1;
        if (i >= 0 && i < m_entries.size() && m_entries.at(i).isFolder) {
            openFolder(m_entries.at(i).path);
        }
    });
    m_list->setKeyFilter([this](QKeyEvent* e) {
        if (e->key() == Qt::Key_Space && !(e->modifiers() & Qt::ControlModifier)) {
            toggleRow(m_list->currentRow());
            return true;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            const int row = m_list->currentRow();
            const int i = (row >= 0 && m_listModel->item(row)) ? m_listModel->item(row)->data(kEntryRole).toInt() : -1;
            if (i >= 0 && i < m_entries.size() && m_entries.at(i).isFolder) {
                openFolder(m_entries.at(i).path);
                return true;
            }
        }
        return false;
    });

    m_download = makeButton(BrowsePage, tr("Download to folder…"));
    m_uploadFiles = makeButton(BrowsePage, tr("Upload files…"));
    m_uploadFolder = makeButton(BrowsePage, tr("Upload folder…"));
    m_addToPlaylist = makeButton(BrowsePage, tr("Add to playlist"), GlButton::Kind::Primary);
    m_selectAll = makeButton(BrowsePage, tr("Select all"));
    m_clearSelection = makeButton(BrowsePage, tr("Clear"));
    m_prevPage = makeButton(BrowsePage, tr("Previous"));
    m_nextPage = makeButton(BrowsePage, tr("Next"));
    m_pageLabel = makeLabel(BrowsePage, 9.5, true);
    m_pageLabel->setAlignment(Qt::AlignCenter);

    connect(m_prevPage, &GlButton::clicked, this, [this] { --m_listPage; populateList(); });
    connect(m_nextPage, &GlButton::clicked, this, [this] { ++m_listPage; populateList(); });
    connect(m_search, &GlTextInput::textChanged, this, [this] { m_listPage = 0; populateList(); });
    connect(m_browseRemote, &GlDropdown::currentIndexChanged, this, [this] { openFolder(QString()); });
    connect(m_up, &GlButton::clicked, this, &CloudWindow::goUp);
    connect(m_refresh, &GlButton::clicked, this, &CloudWindow::reloadListing);
    connect(m_newFolder, &GlButton::clicked, this, &CloudWindow::makeFolder);
    connect(m_selectAll, &GlButton::clicked, this, [this] { setAllChecked(true); });
    connect(m_clearSelection, &GlButton::clicked, this, [this] { setAllChecked(false); });
    connect(m_download, &GlButton::clicked, this, &CloudWindow::downloadSelected);
    connect(m_uploadFiles, &GlButton::clicked, this, &CloudWindow::uploadFiles);
    connect(m_uploadFolder, &GlButton::clicked, this, &CloudWindow::uploadFolder);
    connect(m_addToPlaylist, &GlButton::clicked, this, &CloudWindow::addToPlaylist);
}

void CloudWindow::buildSyncPage() {
    GlCanvas* c = canvas();
    m_syncIntro = makeLabel(SyncPage, 10, true, true);
    m_syncIntro->setText(tr("Keep a folder on this PC and a folder in the cloud in step. Always preview first."));

    auto formLabel = [this](const QString& text) {
        GlLabel* l = makeLabel(SyncPage, 10.5);
        l->setText(text);
        return l;
    };
    m_syncRemoteLabel = formLabel(tr("Account"));
    m_syncRemote = new GlDropdown(c);
    m_syncRemote->setCompact(true);
    own(SyncPage, m_syncRemote);

    m_syncRemoteDirLabel = formLabel(tr("Cloud folder"));
    m_syncRemoteDir = new GlTextInput(c);
    m_syncRemoteDir->setPlaceholder(tr("Folder in the cloud, e.g. Wallpapers"));
    own(SyncPage, m_syncRemoteDir);

    m_syncLocalLabel = formLabel(tr("Folder on this PC"));
    m_syncLocal = new GlTextInput(c);
    m_syncLocal->setPlaceholder(tr("Folder on this PC"));
    own(SyncPage, m_syncLocal);
    m_syncPick = makeButton(SyncPage, tr("Choose…"));

    m_syncDirectionLabel = formLabel(tr("Direction"));
    m_syncDirection = new GlDropdown(c);
    m_syncDirection->setCompact(true);
    m_syncDirection->addItem(tr("Upload: this PC → cloud"));
    m_syncDirection->addItem(tr("Download: cloud → this PC"));
    m_syncDirection->setCurrentIndex(0);
    own(SyncPage, m_syncDirection);

    m_syncModeLabel = formLabel(tr("Mode"));
    m_syncMode = new GlDropdown(c);
    m_syncMode->setCompact(true);
    m_syncMode->addItem(tr("Copy (never removes)"));
    m_syncMode->addItem(tr("Mirror (match the source)"));
    m_syncMode->addItem(tr("Two-way (never deletes)"));
    m_syncMode->setCurrentIndex(0);
    own(SyncPage, m_syncMode);
    m_syncModeHint = makeLabel(SyncPage, 9.5, true, true);

    m_syncPreview = makeButton(SyncPage, tr("Preview"));
    m_syncRun = makeButton(SyncPage, tr("Run"), GlButton::Kind::Primary);
    m_syncVerify = makeButton(SyncPage, tr("Verify"));
    m_syncVerify->setToolTip(tr("Compares both folders afterwards (size and checksum where the service provides one)."));

    m_syncSummary = makeLabel(SyncPage, 10, false, true);
    m_syncModel = new QStandardItemModel(this);
    m_syncReport = new GlItemView(c);
    m_syncReport->setModel(m_syncModel);
    m_syncReport->setDelegate(new GlTextDelegate(m_syncReport));
    m_syncReport->setMode(GlItemView::Mode::List, QSize(200, 26), 0);
    m_syncReport->setAccessibleName(tr("Sync report"));
    m_syncReport->setEmptyText(tr("No report yet"), tr("Choose the folders and press Preview."));
    own(SyncPage, m_syncReport);

    connect(m_syncPick, &GlButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Folder on this PC"), m_syncLocal->text());
        if (!d.isEmpty()) {
            m_syncLocal->setText(QDir::toNativeSeparators(d));
        }
    });
    for (GlDropdown* d : {m_syncRemote, m_syncDirection, m_syncMode}) {
        connect(d, &GlDropdown::currentIndexChanged, this, &CloudWindow::invalidatePreview);
    }
    connect(m_syncMode, &GlDropdown::currentIndexChanged, this, &CloudWindow::updateModeHint);
    connect(m_syncRemoteDir, &GlTextInput::textChanged, this, &CloudWindow::invalidatePreview);
    connect(m_syncLocal, &GlTextInput::textChanged, this, &CloudWindow::invalidatePreview);
    connect(m_syncPreview, &GlButton::clicked, this, &CloudWindow::previewSync);
    connect(m_syncRun, &GlButton::clicked, this, &CloudWindow::runSync);
    connect(m_syncVerify, &GlButton::clicked, this, &CloudWindow::verifySync);
}

void CloudWindow::buildBackupPage() {
    GlCanvas* c = canvas();
    m_backupIntro = makeLabel(BackupPage, 10, true, true);
    m_backupIntro->setText(tr("Backups go to a “%1” folder in the chosen account. Nothing is ever deleted there by Motiva; "
                              "restoring saves a copy next to your files and never replaces the library you are using.")
                               .arg(CloudManager::backupFolderName()));

    m_backupRemote = new GlDropdown(c);
    m_backupRemote->setCompact(true);
    m_backupRemote->setToolTip(tr("Account to back up to"));
    own(BackupPage, m_backupRemote);
    m_backupLibraryButton = makeButton(BackupPage, tr("Back up library now"), GlButton::Kind::Primary);
    m_backupFolderButton = makeButton(BackupPage, tr("Back up a folder…"));

    m_backupListLabel = makeLabel(BackupPage, 10);
    m_backupListLabel->setBold(true);
    m_backupListLabel->setText(tr("Library backups in this account"));
    m_backupModel = new QStandardItemModel(this);
    m_backups = new GlItemView(c);
    m_backups->setModel(m_backupModel);
    m_backups->setDelegate(new GlTextDelegate(m_backups));
    m_backups->setMode(GlItemView::Mode::List, QSize(200, 34), 0);
    m_backups->setAccessibleName(tr("Library backups"));
    m_backups->setEmptyText(tr("No library backups"), tr("Use Back up library now to create the first one."));
    own(BackupPage, m_backups);

    m_backupRefresh = makeButton(BackupPage, tr("Refresh list"));
    m_restoreButton = makeButton(BackupPage, tr("Restore selected…"));

    connect(m_backupLibraryButton, &GlButton::clicked, this, &CloudWindow::backupLibrary);
    connect(m_backupFolderButton, &GlButton::clicked, this, &CloudWindow::backupFolder);
    connect(m_backupRefresh, &GlButton::clicked, this, &CloudWindow::refreshBackups);
    connect(m_backupRemote, &GlDropdown::currentIndexChanged, this, &CloudWindow::refreshBackups);
    connect(m_restoreButton, &GlButton::clicked, this, &CloudWindow::restoreBackup);
}

void CloudWindow::buildTransferPanel() {
    GlCanvas* c = canvas();
    m_xferPanel = new GlPanel(c);
    m_xferPanel->setRadiusPx(14);
    m_xferTitle = new GlLabel(c);
    m_xferTitle->setPointSize(10.5);
    m_xferTitle->setBold(true);
    m_xferTitle->setText(tr("Nothing running."));
    m_xferCancel = new GlButton(c, GlButton::Kind::Secondary);
    m_xferCancel->setText(tr("Cancel"));
    m_xferCancel->setEnabled(false);
    connect(m_xferCancel, &GlButton::clicked, this, [this] {
        m_xferCancel->setEnabled(false);
        m_xferDetail->setText(tr("Stopping…"));
        m_cloud->cancelTransfer();
    });
    m_xferBar = new GlProgressBar(c);
    m_xferBar->setRange(0, 1000);
    m_xferBar->setValue(0);
    m_xferDetail = new GlLabel(c);
    m_xferDetail->setPointSize(9.5);
    m_xferDetail->setTone(GlLabel::Tone::Dim);
    m_logModel = new QStandardItemModel(this);
    m_log = new GlItemView(c);
    m_log->setModel(m_logModel);
    m_log->setDelegate(new GlTextDelegate(m_log));
    m_log->setMode(GlItemView::Mode::List, QSize(200, 22), 0);
    m_log->setAccessibleName(tr("Transfer log"));
}

// ------------------------------------------------------------------ layout

void CloudWindow::showPage(int page) {
    m_page = std::clamp(page, 0, int(PageCount) - 1);
    for (int p = 0; p < PageCount; ++p) {
        m_tabButtons[p]->setSelected(p == m_page);
    }
    updateVisibility();
}

void CloudWindow::updateVisibility() {
    for (int p = 0; p < PageCount; ++p) {
        for (GlItem* it : m_pageItems[p]) {
            it->setVisible(p == m_page);
        }
    }
    const bool accounts = m_page == AccountsPage;
    for (ProviderRow& r : m_providers) {
        r.creds->setVisible(accounts && m_credsVisible);
    }
    m_cancelConnect->setVisible(accounts && m_connectingUi);
    m_resetAccounts->setVisible(accounts && m_resetVisible);
    m_addToPlaylist->setVisible(m_page == BrowsePage && m_haveTarget);
    m_banner->setVisible(m_bannerOn);
    m_reconnectButton->setVisible(m_bannerOn && !m_reconnectName.isEmpty());
    canvas()->relayout();
}

void CloudWindow::layoutContent(const QRectF& content, const QRectF& footer) {
    if (!m_closeButton) {
        return; // still constructing
    }
    const qreal L = content.left(), W = content.width();
    qreal y = content.top();

    m_closeButton->setRect(QRectF(footer.right() - 120, footer.top() + 6, 120, 40));

    const bool haveTool = !m_toolLabel->text().isEmpty();
    m_toolLabel->setVisible(haveTool);
    if (haveTool) {
        const qreal h = m_toolLabel->heightForWidth(W) + 2;
        m_toolLabel->setRect(QRectF(L, y, W, h));
        y += h + 6;
    }
    if (m_bannerOn) {
        const bool reconnect = m_reconnectButton->isVisible();
        const qreal bw = reconnect ? W - 160 : W;
        const qreal h = std::max<qreal>(reconnect ? 36 : 24, m_banner->heightForWidth(bw) + 6);
        m_banner->setRect(QRectF(L, y, bw, h));
        m_reconnectButton->setRect(QRectF(L + W - 150, y + (h - 36) / 2, 150, 36));
        y += h + 6;
    }
    const qreal tabW = std::min<qreal>(190, W / PageCount - 6);
    for (int i = 0; i < PageCount; ++i) {
        m_tabButtons[i]->setRect(QRectF(L + i * (tabW + 6), y, tabW, 38));
    }
    y += 38 + 10;

    const bool showLog = content.height() >= 600;
    const qreal xferH = showLog ? 158 : 98;
    const QRectF xfer(L, content.bottom() - xferH, W, xferH);
    const QRectF area(L, y, W, std::max<qreal>(120, xfer.top() - 10 - y));
    switch (m_page) {
    case AccountsPage: layoutAccounts(area); break;
    case BrowsePage: layoutBrowse(area); break;
    case SyncPage: layoutSync(area); break;
    case BackupPage: layoutBackup(area); break;
    default: break;
    }
    layoutTransfer(xfer, showLog);
}

void CloudWindow::layoutAccounts(const QRectF& a) {
    const qreal L = a.left(), W = a.width();
    qreal y = a.top();
    const bool compact = a.height() < 480;
    m_accountsIntro->setVisible(!compact);
    if (!compact) {
        const qreal h0 = m_accountsIntro->heightForWidth(W) + 2;
        m_accountsIntro->setRect(QRectF(L, y, W, h0));
        y += h0 + 8;
    }

    constexpr qreal kConnectW = 130, kCredsW = 170;
    for (ProviderRow& r : m_providers) {
        const qreal textW = std::max<qreal>(80, W - 40 - kConnectW - (r.creds->isVisible() ? kCredsW + 10 : 0));
        const qreal hintH = std::max<qreal>(18, r.hint->heightForWidth(textW) + 2);
        const qreal cardH = std::max<qreal>(compact ? 52 : 58, (compact ? 8 : 12) + 24 + hintH + (compact ? 8 : 12));
        const QRectF card(L, y, W, cardH);
        r.card->setRect(card);
        r.name->setRect(QRectF(L + 20, y + (compact ? 6 : 10), textW, 24));
        r.hint->setRect(QRectF(L + 20, y + (compact ? 30 : 34), textW, hintH));
        r.connect->setRect(QRectF(card.right() - 16 - kConnectW, card.center().y() - 20, kConnectW, 40));
        r.creds->setRect(QRectF(card.right() - 16 - kConnectW - 10 - kCredsW, card.center().y() - 20, kCredsW, 40));
        y += cardH + (compact ? 5 : 8);
    }
    m_readOnly->setRect(QRectF(L, y, W, 34));
    y += compact ? 34 : 38;
    const qreal cancelW = m_cancelConnect->isVisible() ? 150 : 0;
    m_connectStatus->setRect(QRectF(L, y, std::max<qreal>(40, W - cancelW - 10), 30));
    m_cancelConnect->setRect(QRectF(L + W - cancelW, y - 2, cancelW, 34));
    const bool statusRow = !m_connectStatus->text().isEmpty() || cancelW > 0;
    m_connectStatus->setVisible(m_page == AccountsPage && statusRow);
    if (statusRow || !compact) {
        y += 36;
    }

    const qreal rowY = a.bottom() - 40;
    const qreal bw = 160;
    m_testAccount->setRect(QRectF(L, rowY, bw, 40));
    m_reconnectAccount->setRect(QRectF(L + bw + 8, rowY, bw - 20, 40));
    m_removeAccount->setRect(QRectF(L + 2 * bw - 12 + 16, rowY, bw, 40));
    m_resetAccounts->setRect(QRectF(L + W - 230, rowY, 230, 40));
    m_accounts->setRect(QRectF(L, y, W, std::max<qreal>(50, rowY - 8 - y)));
}

void CloudWindow::layoutBrowse(const QRectF& a) {
    const qreal L = a.left(), W = a.width(), R = a.right();
    qreal y = a.top();
    constexpr qreal H = 38;
    m_browseRemote->setRect(QRectF(L, y, 220, H));
    m_up->setRect(QRectF(L + 228, y, 70, H));
    m_refresh->setRect(QRectF(L + 306, y, 100, H));
    m_newFolder->setRect(QRectF(L + 414, y, 130, H));
    const qreal sw = std::clamp<qreal>(W - 560, 140, 280);
    m_search->setRect(QRectF(R - sw, y, sw, H));
    y += H + 6;
    m_pathLabel->setRect(QRectF(L + 4, y, W - 8, 24));
    y += 28;

    const qreal pagerY = a.bottom() - 36;
    const qreal actionsY = pagerY - 8 - H;
    m_list->setRect(QRectF(L, y, W, std::max<qreal>(80, actionsY - 8 - y)));

    m_download->setRect(QRectF(L, actionsY, 190, H));
    m_uploadFiles->setRect(QRectF(L + 198, actionsY, 140, H));
    m_uploadFolder->setRect(QRectF(L + 346, actionsY, 150, H));
    const QString addText = m_addToPlaylist->text();
    const qreal addW = std::clamp<qreal>(QFontMetricsF(GlUi::uiFont(10, true)).horizontalAdvance(addText) + 56, 200, W * 0.4);
    m_addToPlaylist->setRect(QRectF(R - addW, actionsY, addW, H));

    m_selectAll->setRect(QRectF(L, pagerY, 120, 36));
    m_clearSelection->setRect(QRectF(L + 128, pagerY, 90, 36));
    m_nextPage->setRect(QRectF(R - 100, pagerY, 100, 36));
    m_pageLabel->setRect(QRectF(R - 100 - 8 - 230, pagerY, 230, 36));
    m_prevPage->setRect(QRectF(R - 100 - 8 - 230 - 8 - 100, pagerY, 100, 36));
}

void CloudWindow::layoutSync(const QRectF& a) {
    const qreal L = a.left(), W = a.width();
    qreal y = a.top();
    const bool compact = a.height() < 430;
    m_syncIntro->setVisible(!compact);
    if (!compact) {
        const qreal ih = m_syncIntro->heightForWidth(W) + 2;
        m_syncIntro->setRect(QRectF(L, y, W, ih));
        y += ih + 8;
    }

    const qreal H = compact ? 34 : 38, kLabelW = 170, kGap = compact ? 4 : 8;
    const qreal fx = L + kLabelW + 20;
    const qreal fieldW = std::min<qreal>(520, W - kLabelW - 20 - 140);
    auto row = [&](GlLabel* label, GlItem* field, qreal width) {
        label->setRect(QRectF(L + 4, y, kLabelW, H));
        field->setRect(QRectF(fx, y, width, H));
        y += H + kGap;
    };
    row(m_syncRemoteLabel, m_syncRemote, std::min<qreal>(fieldW, 320));
    row(m_syncRemoteDirLabel, m_syncRemoteDir, fieldW);
    m_syncLocalLabel->setRect(QRectF(L + 4, y, kLabelW, H));
    m_syncLocal->setRect(QRectF(fx, y, fieldW, H));
    m_syncPick->setRect(QRectF(fx + fieldW + 8, y, 120, H));
    y += H + kGap;
    row(m_syncDirectionLabel, m_syncDirection, std::min<qreal>(fieldW, 320));
    row(m_syncModeLabel, m_syncMode, std::min<qreal>(fieldW, 320));
    const qreal mh = std::max<qreal>(18, m_syncModeHint->heightForWidth(W - kLabelW - 24) + 2);
    m_syncModeHint->setRect(QRectF(fx, y - 2, W - kLabelW - 24, mh));
    y += mh + (compact ? 4 : 8);

    m_syncPreview->setRect(QRectF(L, y, 130, H));
    m_syncRun->setRect(QRectF(L + 138, y, 130, H));
    m_syncVerify->setRect(QRectF(L + 276, y, 130, H));
    y += H + 10;

    const bool summary = !m_syncSummary->text().isEmpty();
    m_syncSummary->setVisible(m_page == SyncPage && summary);
    if (summary) {
        const qreal sh = m_syncSummary->heightForWidth(W) + 4;
        m_syncSummary->setRect(QRectF(L, y, W, sh));
        y += sh + 6;
    }
    m_syncReport->setRect(QRectF(L, y, W, std::max<qreal>(50, a.bottom() - y)));
}

void CloudWindow::layoutBackup(const QRectF& a) {
    const qreal L = a.left(), W = a.width();
    qreal y = a.top();
    constexpr qreal H = 38;
    const qreal ih = m_backupIntro->heightForWidth(W) + 2;
    m_backupIntro->setRect(QRectF(L, y, W, ih));
    y += ih + 10;
    m_backupRemote->setRect(QRectF(L, y, 220, H));
    m_backupLibraryButton->setRect(QRectF(L + 230, y, 190, H));
    m_backupFolderButton->setRect(QRectF(L + 428, y, 170, H));
    y += H + 10;
    m_backupListLabel->setRect(QRectF(L + 4, y, W - 8, 24));
    y += 28;
    const qreal rowY = a.bottom() - H;
    m_backups->setRect(QRectF(L, y, W, std::max<qreal>(70, rowY - 8 - y)));
    m_backupRefresh->setRect(QRectF(L, rowY, 150, H));
    m_restoreButton->setRect(QRectF(L + 158, rowY, 190, H));
}

void CloudWindow::layoutTransfer(const QRectF& a, bool showLog) {
    m_xferPanel->setRect(a);
    const qreal L = a.left() + 16, W = a.width() - 32;
    m_xferCancel->setRect(QRectF(a.right() - 16 - 110, a.top() + 8, 110, 34));
    m_xferTitle->setRect(QRectF(L, a.top() + 10, std::max<qreal>(40, W - 124), 26));
    m_xferBar->setRect(QRectF(L, a.top() + 46, W, 16));
    m_xferDetail->setRect(QRectF(L, a.top() + 66, W, 22));
    m_log->setVisible(showLog);
    if (showLog) {
        m_log->setRect(QRectF(L, a.top() + 92, W, a.height() - 92 - 8));
    }
}

// ------------------------------------------------------------------ shared helpers

void CloudWindow::setTarget(const QString& playlistName, std::function<bool(const QString&)> accepts) {
    m_playlistName = playlistName;
    m_accepts = std::move(accepts);
    m_addToPlaylist->setText(tr("Add to playlist “%1”").arg(playlistName));
    m_haveTarget = true;
    updateVisibility();
}

void CloudWindow::present() {
    show();
    raise();
    activateWindow();
    refreshTool();
    reloadRemotes();
}

void CloudWindow::refreshTool() {
    m_cloud->checkTool([this](const CloudToolInfo& t) {
        if (!t.found) {
            m_toolLabel->setText(tr("⚠ %1").arg(t.error));
        } else {
            QString text = tr("Cloud access runs through rclone %1 (MIT licence) — %2.")
                               .arg(t.version, t.bundled ? tr("the copy shipped with Motiva") : tr("found on this PC"));
            if (!t.licenseFilePresent) {
                text += tr(" The rclone licence file is missing next to rclone.exe; reinstall Motiva to restore it.");
            }
            m_toolLabel->setText(text);
        }
        canvas()->relayout();
    });
}

void CloudWindow::showBanner(const QString& text, bool isError, const QString& reconnectName) {
    m_banner->setText(isError ? tr("⚠ %1").arg(text) : text);
    if (isError) {
        m_banner->setColor(GlUi::Style::current().bad);
    } else {
        m_banner->setTone(GlLabel::Tone::Normal);
    }
    m_bannerOn = !text.isEmpty();
    m_reconnectName = reconnectName;
    updateVisibility();
}

void CloudWindow::appendLog(const QString& line) {
    m_logModel->appendRow(new QStandardItem(line));
    while (m_logModel->rowCount() > kMaxLogLines) {
        m_logModel->removeRow(0);
    }
    m_log->scrollTo(m_logModel->rowCount() - 1);
}

void CloudWindow::setLines(QStandardItemModel* model, const QStringList& lines) {
    clearRows(model);
    for (const QString& l : lines) {
        model->appendRow(new QStandardItem(l));
    }
}

void CloudWindow::fillRemoteCombo(GlDropdown* combo) {
    const QVariant previous = combo->currentData();
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (const CloudRemote& r : m_remotes) {
        combo->addItem(r.name, r.name);
    }
    const int i = previous.isValid() ? combo->findData(previous) : -1;
    combo->setCurrentIndex(i >= 0 ? i : 0);
}

void CloudWindow::reloadRemotes() {
    m_cloud->listRemotes([this](const QList<CloudRemote>& remotes, const QString& error) {
        m_remotes = remotes;
        clearRows(m_accountsModel);
        for (const CloudRemote& r : remotes) {
            auto* name = new QStandardItem(r.name);
            name->setData(r.name, Qt::UserRole);
            auto* service = new QStandardItem(CloudManager::provider(r.providerId).displayName);
            auto* status = new QStandardItem(tr("Saved - use Test connection to check"));
            m_accountsModel->appendRow({name, service, status});
        }
        if (remotes.size() == 1) {
            m_accounts->setCurrentRow(0);
        }
        for (GlDropdown* d : {m_browseRemote, m_syncRemote, m_backupRemote}) {
            fillRemoteCombo(d);
        }
        if (!m_selectRemote.isEmpty()) {
            const int i = m_browseRemote->findData(m_selectRemote);
            if (i >= 0) {
                const QSignalBlocker blocker(m_browseRemote);
                m_browseRemote->setCurrentIndex(i);
            }
            m_selectRemote.clear();
        }
        m_resetVisible = m_cloud->configUnreadable() || !remotes.isEmpty();
        if (!error.isEmpty()) {
            m_resetVisible = true;
            showBanner(error, true);
        }
        if (!remotes.isEmpty()) {
            if (m_path.isEmpty() && m_entries.isEmpty()) {
                openFolder(QString());
            }
        } else {
            clearRows(m_listModel);
            m_entries.clear();
            m_pageLabel->clear();
            m_pathLabel->clear();
        }
        setBusy(m_cloud->busy());
        updateVisibility();
    });
}

QString CloudWindow::selectedAccount() const {
    const int row = m_accounts->currentRow();
    const QStandardItem* item = row >= 0 ? m_accountsModel->item(row, 0) : nullptr;
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void CloudWindow::reconnect(const QString& name) {
    if (name.isEmpty() || !m_cloud->reconnectAccount(name)) {
        return;
    }
    m_connectingProvider = name;
    m_connectingUi = true;
    showPage(AccountsPage);
    setBusy(m_cloud->busy());
}

// ------------------------------------------------------------------ accounts

void CloudWindow::refreshProviderHints() {
    for (const CloudProviderInfo& p : CloudManager::providers()) {
        GlLabel* hint = m_providerHints.value(p.id);
        if (!hint) {
            continue;
        }
        if (m_cloud->usesSharedClient(p.id) && p.id == QLatin1String("gdrive")) {
            hint->setText(tr("Sign in with your Google account in your web browser."));
        } else {
            hint->setText(tr("Sign in with your %1 account in your web browser.").arg(p.displayName));
        }
    }
    canvas()->relayout();
}

void CloudWindow::editCredentials(const QString& providerId) {
    // Developer-only (MOTIVA_CLOUD_DEV): a plain dialog so the secret can be masked.
    const CloudProviderInfo info = CloudManager::provider(providerId);
    QDialog dlg(this);
    dlg.setWindowTitle(tr("%1 API credentials").arg(info.displayName));
    auto* v = new QVBoxLayout(&dlg);
    auto* text = new QLabel(tr("Register your own application with the service and enter its client ID (and secret, where it "
                               "has one) here. Leave the ID empty to use rclone's shared registration.\n\nRegistration page: %1\n"
                               "The secret is stored encrypted for your Windows account only.")
                                .arg(info.consoleUrl),
                            &dlg);
    text->setWordWrap(true);
    v->addWidget(text);
    auto* form = new QFormLayout;
    auto* id = new QLineEdit(m_cloud->clientId(providerId), &dlg);
    auto* secret = new QLineEdit(&dlg);
    secret->setEchoMode(QLineEdit::Password);
    secret->setPlaceholderText(m_cloud->clientSecret(providerId).isEmpty() ? tr("(none)") : tr("(saved - leave empty to keep)"));
    form->addRow(tr("Client ID"), id);
    form->addRow(tr("Client secret"), secret);
    v->addLayout(form);
    auto* row = new QHBoxLayout;
    auto* open = plainButton(tr("Open registration page"), &dlg);
    auto* ok = plainButton(tr("Save"), &dlg);
    auto* cancel = plainButton(tr("Cancel"), &dlg);
    row->addWidget(open);
    row->addStretch(1);
    row->addWidget(ok);
    row->addWidget(cancel);
    v->addLayout(row);
    connect(open, &QPushButton::clicked, &dlg, [info] { QDesktopServices::openUrl(QUrl(info.consoleUrl)); });
    connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    dlg.resize(520, dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const QString newSecret = secret->text().trimmed().isEmpty() ? m_cloud->clientSecret(providerId) : secret->text();
    m_cloud->setClientCredentials(providerId, id->text(), newSecret);
    refreshProviderHints();
}

void CloudWindow::connectProvider(const QString& providerId) {
    const CloudProviderInfo info = CloudManager::provider(providerId);
    QString suggestion = info.displayName;
    auto taken = [this](const QString& n) {
        return std::any_of(m_remotes.begin(), m_remotes.end(), [&n](const CloudRemote& r) {
            return r.name.compare(n, Qt::CaseInsensitive) == 0;
        });
    };
    for (int i = 2; taken(suggestion); ++i) {
        suggestion = QStringLiteral("%1 %2").arg(info.displayName).arg(i);
    }
    for (;;) {
        bool ok = false;
        const QString name = GlMessageBox::getText(this, tr("Connect %1").arg(info.displayName),
                                                   tr("Name for this account (you can connect several):"), suggestion, &ok)
                                 .trimmed();
        if (!ok) {
            return;
        }
        QString why;
        if (!CloudManager::isValidRemoteName(name, &why)) {
            GlMessageBox::warning(this, tr("Account name"), why);
            suggestion = name;
            continue;
        }
        if (taken(name)) {
            GlMessageBox::warning(this, tr("Account name"), tr("An account with that name already exists."));
            suggestion = name;
            continue;
        }
        if (!m_cloud->connectAccount(providerId, name, providerId == QLatin1String("gdrive") && m_readOnly->isChecked())) {
            showBanner(tr("A sign-in is already in progress."), true);
            return;
        }
        m_connectingProvider = providerId;
        m_connectingUi = true;
        showBanner(QString(), false);
        setBusy(m_cloud->busy());
        return;
    }
}

void CloudWindow::onConnectFinished(const QString& name, bool ok, const QString& message) {
    m_connectingUi = false;
    m_connectStatus->clear();
    m_connectingProvider.clear();
    showBanner(ok ? tr("“%1” is connected.").arg(name) : message, !ok);
    if (ok) {
        // The account list reloads asynchronously; reloadRemotes() opens this account when it is there.
        m_selectRemote = name;
        m_path.clear();
        m_entries.clear();
    }
    reloadRemotes();
    if (ok) {
        // rclone's own page says "go back to rclone"; bring Motiva forward instead.
        raise();
        activateWindow();
        showPage(BrowsePage);
    }
}

// ------------------------------------------------------------------ browse

void CloudWindow::openFolder(const QString& path) {
    m_path = path;
    reloadListing();
}

void CloudWindow::goUp() {
    if (!m_path.isEmpty()) {
        openFolder(parentOf(m_path));
    }
}

void CloudWindow::reloadListing() {
    const QString remote = m_browseRemote->currentData().toString();
    if (remote.isEmpty()) {
        return;
    }
    const int generation = ++m_listGeneration;
    m_pageLabel->setText(tr("Loading…"));
    m_list->setEnabled(false);
    m_cloud->list(remote, m_path, [this, generation, remote](bool ok, const QList<CloudEntry>& entries, const QString& error, bool reconnectNeeded) {
        if (generation != m_listGeneration) {
            return; // a newer request superseded this one
        }
        m_list->setEnabled(true);
        clearRows(m_listModel);
        m_entries.clear();
        m_listPage = 0;
        {
            const QSignalBlocker blocker(m_search);
            m_search->clear();
        }
        updatePathLabel(remote);
        if (!ok) {
            m_pageLabel->clear();
            showBanner(error, true, reconnectNeeded ? remote : QString());
            return;
        }
        showBanner(QString(), false);
        QList<CloudEntry> sorted = entries;
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(sorted.begin(), sorted.end(), [&collator](const CloudEntry& a, const CloudEntry& b) {
            if (a.isFolder != b.isFolder) {
                return a.isFolder;
            }
            return collator.compare(a.name, b.name) < 0;
        });
        m_entries = sorted;
        populateList();
    });
}

void CloudWindow::updatePathLabel(const QString& remote) {
    QString text = remote;
    for (const QString& part : m_path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        text += QStringLiteral("  ›  ") + part;
    }
    m_pathLabel->setText(text);
    m_up->setEnabled(!m_path.isEmpty() && !m_cloud->busy());
}

void CloudWindow::populateList() {
    constexpr int kPageSize = 200;
    clearRows(m_listModel);
    const QString needle = m_search->text().trimmed();
    QList<int> shown;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (needle.isEmpty() || m_entries.at(i).name.contains(needle, Qt::CaseInsensitive)) {
            shown << i;
        }
    }
    const int pages = qMax(1, (shown.size() + kPageSize - 1) / kPageSize);
    m_listPage = qBound(0, m_listPage, pages - 1);
    for (int n = m_listPage * kPageSize; n < qMin(shown.size(), (m_listPage + 1) * kPageSize); ++n) {
        const int i = shown.at(n);
        const CloudEntry& e = m_entries.at(i);
        auto* item = new QStandardItem(e.name);
        item->setData(i, kEntryRole);
        item->setData(e.isFolder, kFolderRole);
        item->setData(e.isFolder ? QString() : sizeText(e.size), kSizeRole);
        item->setData(e.modified.isValid() ? QLocale().toString(e.modified.toLocalTime(), QLocale::ShortFormat) : QString(),
                      kDateRole);
        item->setData(Qt::Unchecked, Qt::CheckStateRole);
        m_listModel->appendRow(item);
    }
    m_list->setEmptyText(needle.isEmpty() ? tr("This folder is empty.") : tr("Nothing matches the search."), QString());
    m_prevPage->setEnabled(m_listPage > 0);
    m_nextPage->setEnabled(m_listPage + 1 < pages);
    m_pageLabel->setText(tr("%1 item(s) - page %2 of %3").arg(shown.size()).arg(m_listPage + 1).arg(pages));
}

void CloudWindow::toggleRow(int row) {
    QStandardItem* item = (row >= 0 && row < m_listModel->rowCount()) ? m_listModel->item(row) : nullptr;
    if (!item) {
        return;
    }
    item->setData(item->data(Qt::CheckStateRole).toInt() == Qt::Checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
}

void CloudWindow::setAllChecked(bool on) {
    for (int i = 0; i < m_listModel->rowCount(); ++i) {
        m_listModel->item(i)->setData(on ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
    }
}

QList<CloudEntry> CloudWindow::selectedEntries() const {
    QList<CloudEntry> out;
    for (int r = 0; r < m_listModel->rowCount(); ++r) {
        const QStandardItem* item = m_listModel->item(r);
        if (item->data(Qt::CheckStateRole).toInt() != Qt::Checked) {
            continue;
        }
        const int i = item->data(kEntryRole).toInt();
        if (i >= 0 && i < m_entries.size()) {
            out << m_entries.at(i);
        }
    }
    return out;
}

void CloudWindow::makeFolder() {
    const QString remote = m_browseRemote->currentData().toString();
    if (remote.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString name = trimSlashes(GlMessageBox::getText(this, tr("New folder"), tr("Folder name:"), QString(), &ok));
    if (!ok || name.isEmpty()) {
        return;
    }
    const QString path = m_path.isEmpty() ? name : m_path + QLatin1Char('/') + name;
    m_cloud->makeFolder(remote, path, [this](bool done, const QString& error) {
        if (!done) {
            showBanner(error, true);
        }
        reloadListing();
    });
}

bool CloudWindow::begin(const CloudPlan& plan) {
    if (!m_cloud->startPlan(plan)) {
        showBanner(tr("Another transfer is already running."), true);
        return false;
    }
    return true;
}

void CloudWindow::downloadSelected() {
    const QString remote = m_browseRemote->currentData().toString();
    const QList<CloudEntry> sel = selectedEntries();
    if (remote.isEmpty() || sel.isEmpty()) {
        showBanner(tr("Tick the files or folders to download first."), false);
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Download to"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (dir.isEmpty()) {
        return;
    }
    m_addAfterDownload = false;
    m_planKind = QStringLiteral("download");
    begin(m_cloud->planDownloadTo(remote, sel, dir));
}

void CloudWindow::uploadFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Upload files"));
    if (!files.isEmpty()) {
        uploadPaths(files);
    }
}

void CloudWindow::uploadFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Upload folder"));
    if (!dir.isEmpty()) {
        uploadPaths({dir});
    }
}

void CloudWindow::uploadPaths(const QStringList& paths) {
    const QString remote = m_browseRemote->currentData().toString();
    if (remote.isEmpty()) {
        return;
    }
    QStringList clashes;
    for (const QString& p : paths) {
        const QString name = QFileInfo(p).fileName();
        for (const CloudEntry& e : m_entries) {
            if (e.name.compare(name, Qt::CaseInsensitive) == 0) {
                clashes << name;
            }
        }
    }
    bool overwrite = false;
    QStringList toSend = paths;
    if (!clashes.isEmpty()) {
        GlMessageBox box(QMessageBox::Question, tr("Already in the cloud"),
                         tr("%n item(s) with the same name already exist in this cloud folder:\n%1\n\nReplace them? Replaced "
                            "files are moved to “%2” in the same account, not deleted.",
                            nullptr, int(clashes.size()))
                             .arg(clashes.mid(0, 8).join(QLatin1Char('\n')), CloudManager::syncTrashFolderName()),
                         this);
        auto* replace = box.addButton(tr("Replace"), QMessageBox::AcceptRole);
        auto* skip = box.addButton(tr("Skip existing"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(skip);
        box.exec();
        if (box.clickedButton() == replace) {
            overwrite = true;
        } else if (box.clickedButton() == skip) {
            toSend.erase(std::remove_if(toSend.begin(), toSend.end(),
                                        [&clashes](const QString& p) { return clashes.contains(QFileInfo(p).fileName()); }),
                         toSend.end());
        } else {
            return;
        }
    }
    if (toSend.isEmpty()) {
        return;
    }
    m_planRemote = remote;
    m_planKind = QStringLiteral("upload");
    begin(m_cloud->planUpload(remote, m_path, toSend, overwrite));
}

void CloudWindow::addToPlaylist() {
    const QString remote = m_browseRemote->currentData().toString();
    const QList<CloudEntry> sel = selectedEntries();
    QList<CloudEntry> files;
    int skipped = 0;
    for (const CloudEntry& e : sel) {
        if (!e.isFolder && (!m_accepts || m_accepts(e.name))) {
            files << e;
        } else {
            ++skipped;
        }
    }
    if (files.isEmpty()) {
        showBanner(sel.isEmpty() ? tr("Tick the files to add first.")
                                 : tr("None of the selected items can be added to this playlist (folders and unsupported "
                                      "file types are skipped)."),
                   false);
        return;
    }
    if (skipped > 0) {
        showBanner(tr("%n selected item(s) were skipped (folders or unsupported types).", nullptr, skipped), false);
    }
    m_addAfterDownload = true;
    m_planKind = QStringLiteral("playlist");
    if (!begin(m_cloud->planCacheDownload(remote, files))) {
        m_addAfterDownload = false;
    }
}

// ------------------------------------------------------------------ sync

void CloudWindow::updateModeHint() {
    static const char* const kHints[] = {
        QT_TR_NOOP("Copy only adds new files and updates changed ones. Nothing is removed."),
        QT_TR_NOOP("Mirror makes the destination identical to the source. Files that are not in the source are moved to a "
                   "safety folder, not deleted."),
        QT_TR_NOOP("Two-way adds new files in both directions. If a file differs on both sides, both copies are kept. "
                   "Nothing is deleted."),
    };
    const int i = std::clamp(m_syncMode->currentIndex(), 0, 2);
    m_syncModeHint->setText(tr(kHints[i]));
    canvas()->relayout();
}

void CloudWindow::setSyncReport(const QString& text) {
    const QStringList lines = text.split(QLatin1Char('\n'));
    m_syncSummary->setText(lines.value(0));
    QStringList rest;
    for (int i = 1; i < lines.size(); ++i) {
        if (!lines.at(i).trimmed().isEmpty()) {
            rest << lines.at(i);
        }
    }
    setLines(m_syncModel, rest);
    canvas()->relayout();
}

void CloudWindow::invalidatePreview() {
    m_previewValid = false;
    m_syncRun->setEnabled(false);
    m_syncVerify->setEnabled(!m_cloud->busy());
}

bool CloudWindow::syncEndpoints(QString* src, QString* dst, QString* localDir, QString* remote, QString* remoteDir) const {
    *remote = m_syncRemote->currentData().toString();
    *remoteDir = trimSlashes(m_syncRemoteDir->text());
    *localDir = QDir::fromNativeSeparators(m_syncLocal->text().trimmed());
    if (remote->isEmpty() || localDir->isEmpty()) {
        return false;
    }
    const QString localNative = QDir::toNativeSeparators(QDir::cleanPath(*localDir));
    const QString remoteSpec = CloudManager::remotePath(*remote, *remoteDir);
    if (m_syncDirection->currentIndex() == 0) {
        *src = localNative;
        *dst = remoteSpec;
    } else {
        *src = remoteSpec;
        *dst = localNative;
    }
    return true;
}

void CloudWindow::previewSync() {
    QString src, dst, local, remote, remoteDir;
    if (!syncEndpoints(&src, &dst, &local, &remote, &remoteDir)) {
        setSyncReport(tr("Choose an account and a folder on this PC first."));
        return;
    }
    const bool upload = m_syncDirection->currentIndex() == 0;
    const bool mirror = m_syncMode->currentIndex() == 1;
    const bool twoWay = m_syncMode->currentIndex() == 2;
    if ((upload || twoWay) && !QFileInfo(local).isDir()) {
        setSyncReport(tr("The folder on this PC does not exist."));
        return;
    }
    if (mirror) {
        // Guard rails for the one destructive mode.
        const QDir d(local);
        const QStringList sensitive{QDir::homePath(), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                    QStandardPaths::writableLocation(QStandardPaths::DesktopLocation),
                                    QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)};
        const bool unsafeLocal = d.isRoot() || std::any_of(sensitive.begin(), sensitive.end(), [&local](const QString& s) {
                                     return !s.isEmpty() && QDir::cleanPath(s).compare(QDir::cleanPath(local), Qt::CaseInsensitive) == 0;
                                 });
        if (!upload && unsafeLocal) {
            setSyncReport(tr("Mirroring into a drive root or a main user folder is blocked: it could remove "
                             "unrelated files. Pick a dedicated sub-folder."));
            return;
        }
        if (upload && remoteDir.isEmpty()) {
            setSyncReport(tr("Mirroring onto the root of a cloud account is blocked. Enter a dedicated cloud folder."));
            return;
        }
        if (remoteDir.startsWith(CloudManager::syncTrashFolderName(), Qt::CaseInsensitive) ||
            remoteDir.startsWith(CloudManager::backupFolderName(), Qt::CaseInsensitive)) {
            setSyncReport(tr("That cloud folder is reserved for Motiva's own safety copies."));
            return;
        }
    }
    setSyncReport(tr("Comparing…"));
    m_syncPreview->setEnabled(false);
    const QString key = src + QLatin1Char('|') + dst + QLatin1Char('|') + QString::number(mirror) + QString::number(twoWay);
    if (twoWay) {
        const QString remoteSpec = CloudManager::remotePath(remote, remoteDir);
        m_cloud->verify(QDir::toNativeSeparators(local), remoteSpec, [this, key](const CloudSyncPreview& p) {
            m_syncPreview->setEnabled(!m_cloud->busy());
            if (!p.ok) {
                setSyncReport(p.error);
                m_previewValid = false;
                return;
            }
            // verify(): added = only on this PC, removed = only in the cloud, changed = on both but different.
            QString text = tr("%1 only on this PC (will be uploaded), %2 only in the cloud (will be downloaded), "
                              "%3 conflict(s) (both versions kept), %4 identical.")
                               .arg(p.added.size()).arg(p.removed.size()).arg(p.changed.size()).arg(p.unchanged);
            if (p.problems > 0) {
                text += tr("\n%1 item(s) could not be compared - the sync stays blocked until the comparison is complete.").arg(p.problems);
            }
            auto section = [&text](const QString& head, const QStringList& items) {
                if (items.isEmpty()) {
                    return;
                }
                text += QStringLiteral("\n%1\n").arg(head);
                for (int i = 0; i < items.size() && i < 200; ++i) {
                    text += QStringLiteral("    %1\n").arg(items.at(i));
                }
                if (items.size() > 200) {
                    text += tr("    … and %1 more\n").arg(items.size() - 200);
                }
            };
            section(tr("Conflicts (different on both sides - neither is overwritten):"), p.changed);
            section(tr("Will be uploaded:"), p.added);
            section(tr("Will be downloaded:"), p.removed);
            setSyncReport(text.trimmed());
            m_lastPreview = p;
            m_previewKey = key;
            m_previewValid = p.problems == 0;
            m_syncRun->setEnabled(m_previewValid && !m_cloud->busy() && (!p.added.isEmpty() || !p.removed.isEmpty() || !p.changed.isEmpty()));
        });
        return;
    }
    m_cloud->previewTransfer(mirror, src, dst, [this, key, mirror](const CloudSyncPreview& p) {
        m_syncPreview->setEnabled(!m_cloud->busy());
        if (!p.ok) {
            setSyncReport(p.error);
            m_previewValid = false;
            return;
        }
        QString text = tr("%1 new, %2 changed, %3 to remove, %4 already identical.")
                           .arg(p.added.size())
                           .arg(p.changed.size())
                           .arg(p.removed.size())
                           .arg(p.unchanged);
        if (p.problems > 0) {
            text += tr("\n%1 item(s) could not be compared.").arg(p.problems);
        }
        auto section = [&text](const QString& head, const QStringList& items) {
            if (items.isEmpty()) {
                return;
            }
            text += QStringLiteral("\n%1\n").arg(head);
            for (int i = 0; i < items.size() && i < 200; ++i) {
                text += QStringLiteral("    %1\n").arg(items.at(i));
            }
            if (items.size() > 200) {
                text += tr("    … and %1 more\n").arg(items.size() - 200);
            }
        };
        section(tr("Will be added:"), p.added);
        section(tr("Will replace a different file:"), p.changed);
        section(mirror ? tr("Will be moved to the safety folder (not in the source):") : tr("Only at the destination (kept):"), p.removed);
        setSyncReport(text.trimmed());
        m_lastPreview = p;
        m_previewKey = key;
        m_previewValid = true;
        m_syncRun->setEnabled(!m_cloud->busy() && (!p.added.isEmpty() || !p.changed.isEmpty() || (mirror && !p.removed.isEmpty())));
    });
}

void CloudWindow::runSync() {
    QString src, dst, local, remote, remoteDir;
    if (!syncEndpoints(&src, &dst, &local, &remote, &remoteDir) || !m_previewValid) {
        return;
    }
    const bool upload = m_syncDirection->currentIndex() == 0;
    const bool mirror = m_syncMode->currentIndex() == 1;
    const bool twoWay = m_syncMode->currentIndex() == 2;
    const QString key = src + QLatin1Char('|') + dst + QLatin1Char('|') + QString::number(mirror) + QString::number(twoWay);
    if (key != m_previewKey) {
        invalidatePreview();
        return;
    }
    if (twoWay) {
        const CloudSyncPreview tp = m_lastPreview;
        const QString q = tr("Two-way synchronisation\n\n• %1 file(s) uploaded\n• %2 file(s) downloaded\n• %3 conflict(s): the cloud "
                             "version is saved next to the local one as “… (cloud copy …)”; nothing is overwritten or deleted.\n\nContinue?")
                              .arg(tp.added.size()).arg(tp.removed.size()).arg(tp.changed.size());
        if (GlMessageBox::question(this, tr("Confirm two-way sync"), q, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        m_planKind = QStringLiteral("sync");
        m_planRemote = remote;
        if (begin(m_cloud->planTwoWay(local, CloudManager::remotePath(remote, remoteDir), tp.changed))) {
            invalidatePreview();
        }
        return;
    }
    const CloudSyncPreview p = m_lastPreview;
    const QString trash = CloudManager::newTrashDir(upload ? remote : QString());
    const int replaced = p.changed.size();
    const int removed = mirror ? p.removed.size() : 0;
    QString question = mirror
                           ? tr("Mirror will make the destination identical to the source.\n\n• %1 file(s) added\n• %2 file(s) "
                                "replaced\n• %3 file(s) removed from the destination")
                                 .arg(p.added.size())
                                 .arg(replaced)
                                 .arg(removed)
                           : tr("Copy %1 new and %2 changed file(s) to the destination.").arg(p.added.size()).arg(replaced);
    if (replaced > 0 || removed > 0) {
        question += tr("\n\nReplaced and removed files are first moved to:\n%1\n(they are not deleted).").arg(trash);
    }
    question += tr("\n\nContinue?");
    if (GlMessageBox::question(this, mirror ? tr("Confirm mirror") : tr("Confirm copy"), question, QMessageBox::Yes | QMessageBox::No,
                               QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    m_planKind = QStringLiteral("sync");
    const CloudPlan plan = mirror ? m_cloud->planMirror(src, dst, trash, removed + replaced + 5)
                                  : m_cloud->planCopyFolder(tr("Copying to %1").arg(upload ? remote : tr("this PC")), src, dst, trash);
    if (begin(plan)) {
        invalidatePreview();
    }
}

void CloudWindow::verifySync() {
    QString src, dst, local, remote, remoteDir;
    if (!syncEndpoints(&src, &dst, &local, &remote, &remoteDir)) {
        setSyncReport(tr("Choose an account and a folder on this PC first."));
        return;
    }
    setSyncReport(tr("Verifying…"));
    m_cloud->verify(src, dst, [this](const CloudSyncPreview& p) {
        if (!p.ok) {
            setSyncReport(p.error);
            return;
        }
        if (p.added.isEmpty() && p.changed.isEmpty() && p.removed.isEmpty() && p.problems == 0) {
            setSyncReport(tr("Verified: both folders match (%1 file(s)).").arg(p.unchanged));
            return;
        }
        QString text = tr("Differences found: %1 missing at the destination, %2 different, %3 only at the destination, %4 "
                          "could not be checked; %5 match.")
                           .arg(p.added.size())
                           .arg(p.changed.size())
                           .arg(p.removed.size())
                           .arg(p.problems)
                           .arg(p.unchanged);
        for (const QString& f : p.added.mid(0, 50)) {
            text += tr("\n    missing: %1").arg(f);
        }
        for (const QString& f : p.changed.mid(0, 50)) {
            text += tr("\n    different: %1").arg(f);
        }
        setSyncReport(text);
    });
}

// ------------------------------------------------------------------ backup

void CloudWindow::backupLibrary() {
    const QString remote = m_backupRemote->currentData().toString();
    if (remote.isEmpty()) {
        showBanner(tr("Connect an account first."), false);
        return;
    }
    CloudPlan plan;
    QString error;
    if (!m_cloud->planBackupLibrary(remote, LibraryDatabase::defaultLibraryPath(), &plan, &error)) {
        showBanner(error, true);
        return;
    }
    m_planRemote = remote;
    m_planKind = QStringLiteral("backup");
    if (!begin(plan) && plan.cleanup) {
        plan.cleanup();
    }
}

void CloudWindow::backupFolder() {
    const QString remote = m_backupRemote->currentData().toString();
    if (remote.isEmpty()) {
        showBanner(tr("Connect an account first."), false);
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder to back up"));
    if (dir.isEmpty()) {
        return;
    }
    m_planKind = QStringLiteral("backup");
    begin(m_cloud->planBackupFolder(remote, dir));
}

void CloudWindow::refreshBackups() {
    const QString remote = m_backupRemote->currentData().toString();
    clearRows(m_backupModel);
    m_backupEntries.clear();
    if (remote.isEmpty()) {
        return;
    }
    m_cloud->list(remote, CloudManager::backupFolderName() + QStringLiteral("/Library"),
                  [this](bool ok, const QList<CloudEntry>& entries, const QString& error, bool reconnectNeeded) {
                      clearRows(m_backupModel);
                      m_backupEntries.clear();
                      if (!ok) {
                          // A missing backup folder just means no backup yet.
                          if (!error.contains(tr("no longer exists"))) {
                              showBanner(error, true, reconnectNeeded ? m_backupRemote->currentData().toString() : QString());
                          }
                          return;
                      }
                      QList<CloudEntry> sorted;
                      for (const CloudEntry& e : entries) {
                          if (!e.isFolder && e.name.endsWith(QStringLiteral(".mtv"), Qt::CaseInsensitive)) {
                              sorted << e;
                          }
                      }
                      std::sort(sorted.begin(), sorted.end(), [](const CloudEntry& a, const CloudEntry& b) { return a.name > b.name; });
                      m_backupEntries = sorted;
                      for (const CloudEntry& e : sorted) {
                          m_backupModel->appendRow(new QStandardItem(tr("%1  —  %2").arg(e.name, sizeText(e.size))));
                      }
                  });
}

void CloudWindow::restoreBackup() {
    const QString remote = m_backupRemote->currentData().toString();
    const int row = m_backups->currentRow();
    if (remote.isEmpty() || row < 0 || row >= m_backupEntries.size()) {
        showBanner(tr("Select a backup to restore first."), false);
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Save the restored library in"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (dir.isEmpty()) {
        return;
    }
    m_planKind = QStringLiteral("restore");
    begin(m_cloud->planRestore(remote, m_backupEntries.at(row), dir, &m_restoreSavedAs));
}

// ------------------------------------------------------------------ transfers

void CloudWindow::setBusy(bool busy) {
    const bool signingIn = m_cloud->isConnecting();
    const bool idle = !busy && !signingIn;
    const bool haveAccount = !m_remotes.isEmpty();
    for (auto it = m_connectButtons.begin(); it != m_connectButtons.end(); ++it) {
        it.value()->setEnabled(!signingIn);
    }
    m_testAccount->setEnabled(haveAccount && !signingIn);
    m_reconnectAccount->setEnabled(haveAccount && !signingIn);
    m_removeAccount->setEnabled(haveAccount && idle);
    m_resetAccounts->setEnabled(idle);
    m_up->setEnabled(haveAccount && !busy && !m_path.isEmpty());
    m_refresh->setEnabled(haveAccount && !busy);
    m_selectAll->setEnabled(haveAccount);
    m_clearSelection->setEnabled(haveAccount);
    if (!haveAccount) {
        m_prevPage->setEnabled(false);
        m_nextPage->setEnabled(false);
    }
    m_newFolder->setEnabled(haveAccount && !busy);
    m_download->setEnabled(haveAccount && !busy);
    m_uploadFiles->setEnabled(haveAccount && !busy);
    m_uploadFolder->setEnabled(haveAccount && !busy);
    m_addToPlaylist->setEnabled(haveAccount && !busy);
    m_syncPreview->setEnabled(haveAccount && !busy);
    m_syncRun->setEnabled(haveAccount && !busy && m_previewValid);
    m_syncVerify->setEnabled(haveAccount && !busy);
    m_backupLibraryButton->setEnabled(haveAccount && !busy);
    m_backupFolderButton->setEnabled(haveAccount && !busy);
    m_restoreButton->setEnabled(haveAccount && !busy);
    m_backupRefresh->setEnabled(haveAccount);
}

void CloudWindow::onTransferProgress(const RcloneProgress& p, int stepIndex, int stepCount, const QString& label) {
    double fraction = p.totalBytes > 0 ? double(p.bytes) / double(p.totalBytes) : 0.0;
    fraction = qBound(0.0, fraction, 1.0);
    const double overall = (stepIndex + fraction) / qMax(1, stepCount);
    if (p.totalBytes <= 0 && stepCount == 1) {
        m_xferBar->setIndeterminate(true); // size unknown yet: busy indicator
        m_xferBar->setText(QString());
    } else {
        m_xferBar->setIndeterminate(false);
        m_xferBar->setRange(0, 1000);
        m_xferBar->setValue(int(overall * 1000));
        m_xferBar->setText(tr("%1%").arg(int(overall * 100)));
    }
    QString detail = label;
    if (stepCount > 1) {
        detail = tr("%1  (%2 of %3)").arg(label).arg(stepIndex + 1).arg(stepCount);
    }
    if (p.totalBytes > 0) {
        detail += tr("  —  %1 of %2").arg(sizeText(p.bytes), sizeText(p.totalBytes));
    }
    if (p.speed > 0) {
        detail += tr("  —  %1/s").arg(sizeText(qint64(p.speed)));
    }
    const QString eta = etaText(p.etaSeconds);
    if (!eta.isEmpty()) {
        detail += QStringLiteral("  —  ") + eta;
    }
    m_xferDetail->setText(detail);
}

void CloudWindow::onTransferFinished(const CloudOutcome& o) {
    m_xferBar->setIndeterminate(false);
    m_xferBar->setRange(0, 1000);
    m_xferBar->setValue(o.ok ? 1000 : 0);
    m_xferBar->setText(o.ok ? tr("100%") : QString());
    m_xferCancel->setEnabled(false);
    QString summary;
    if (o.cancelled) {
        summary = tr("Cancelled. Files already transferred were kept; unfinished files were not left behind.");
    } else if (o.ok) {
        summary = tr("Finished: %1").arg(o.title);
        if (m_planKind == QLatin1String("restore") && !m_restoreSavedAs.isEmpty()) {
            summary = tr("Restored copy saved as %1. Your current library was not changed.").arg(QDir::toNativeSeparators(m_restoreSavedAs));
        }
    } else {
        summary = o.errors.join(QLatin1Char('\n'));
    }
    m_xferTitle->setText(o.ok ? tr("Finished.") : (o.cancelled ? tr("Cancelled.") : tr("Finished with problems.")));
    m_xferDetail->setText(summary.section(QLatin1Char('\n'), 0, 0));
    showBanner(summary, !o.ok && !o.cancelled, o.needsReconnect ? (m_planRemote.isEmpty() ? m_browseRemote->currentData().toString() : m_planRemote) : QString());
    for (const QString& e : o.errors) {
        appendLog(e);
    }
    setBusy(false);

    if (m_addAfterDownload) {
        m_addAfterDownload = false;
        if (!o.paths.isEmpty()) {
            emit filesReady(o.paths);
        }
    }
    if (m_planKind == QLatin1String("upload") || m_planKind == QLatin1String("sync")) {
        reloadListing();
    }
    if (m_planKind == QLatin1String("backup")) {
        refreshBackups();
    }
    m_planKind.clear();
    m_planRemote.clear();
}
