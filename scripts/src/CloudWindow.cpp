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
    : GlWindow(tr("Cloud Storage"), tr("View pictures and videos from Google Drive, OneDrive and Dropbox"), parent),
      m_cloud(CloudManager::instance()) {
    setModal(false);

    buildHeader();
    buildAccountsPage();
    buildBrowsePage();
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

    const QStringList names{tr("Accounts"), tr("Browse")};
    for (int i = 0; i < PageCount; ++i) {
        m_tabButtons[i] = new GlButton(c, GlButton::Kind::Tab);
        m_tabButtons[i]->setText(names.at(i));
        connect(m_tabButtons[i], &GlButton::clicked, this, [this, i] { showPage(i); });
    }
}

void CloudWindow::buildAccountsPage() {
    GlCanvas* c = canvas();
    m_accountsIntro = makeLabel(AccountsPage, 10, true, true);
    m_accountsIntro->setText(tr("Connect an account. Sign-in happens in your web browser and is handled securely by the cloud service; "
                                "Motiva never sees your password. Access is read-only: Motiva can only view and download pictures and videos."));

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
    m_list->setEmptyText(tr("Nothing to show"), tr("Connect an account, then open a folder. Only pictures and videos are listed."));
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
    connect(m_selectAll, &GlButton::clicked, this, [this] { setAllChecked(true); });
    connect(m_clearSelection, &GlButton::clicked, this, [this] { setAllChecked(false); });
    connect(m_download, &GlButton::clicked, this, &CloudWindow::downloadSelected);
    connect(m_addToPlaylist, &GlButton::clicked, this, &CloudWindow::addToPlaylist);
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
    const qreal sw = std::clamp<qreal>(W - 560, 140, 280);
    m_search->setRect(QRectF(R - sw, y, sw, H));
    y += H + 6;
    m_pathLabel->setRect(QRectF(L + 4, y, W - 8, 24));
    y += 28;

    const qreal pagerY = a.bottom() - 36;
    const qreal actionsY = pagerY - 8 - H;
    m_list->setRect(QRectF(L, y, W, std::max<qreal>(80, actionsY - 8 - y)));

    m_download->setRect(QRectF(L, actionsY, 190, H));
    const QString addText = m_addToPlaylist->text();
    const qreal addW = std::clamp<qreal>(QFontMetricsF(GlUi::uiFont(10, true)).horizontalAdvance(addText) + 56, 200, W * 0.4);
    m_addToPlaylist->setRect(QRectF(R - addW, actionsY, addW, H));

    m_selectAll->setRect(QRectF(L, pagerY, 120, 36));
    m_clearSelection->setRect(QRectF(L + 128, pagerY, 90, 36));
    m_nextPage->setRect(QRectF(R - 100, pagerY, 100, 36));
    m_pageLabel->setRect(QRectF(R - 100 - 8 - 230, pagerY, 230, 36));
    m_prevPage->setRect(QRectF(R - 100 - 8 - 230 - 8 - 100, pagerY, 100, 36));
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
            m_toolLabel->setText(t.licenseFilePresent ? QString() : tr("Part of the cloud component is missing; reinstall Motiva to restore it."));
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
        fillRemoteCombo(m_browseRemote);
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
                               "has one) here. Leave the ID empty to use Motiva's default registration.\n\nRegistration page: %1\n"
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
    // Motiva is a personal app: one cloud account per computer. Remove it to use another.
    if (!m_remotes.isEmpty()) {
        showBanner(tr("Only one cloud account can be connected. Remove “%1” first to connect a different one.")
                       .arg(m_remotes.first().name),
                   true);
        return;
    }
    const CloudProviderInfo info = CloudManager::provider(providerId);
    if (!m_cloud->connectAccount(providerId, info.displayName)) {
        showBanner(tr("A sign-in is already in progress."), true);
        return;
    }
    m_connectingProvider = providerId;
    m_connectingUi = true;
    showBanner(QString(), false);
    setBusy(m_cloud->busy());
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
        showBanner(tr("Tick the pictures or videos to download first."), false);
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

// ------------------------------------------------------------------ transfers

void CloudWindow::setBusy(bool busy) {
    const bool signingIn = m_cloud->isConnecting();
    const bool idle = !busy && !signingIn;
    const bool haveAccount = !m_remotes.isEmpty();
    for (auto it = m_connectButtons.begin(); it != m_connectButtons.end(); ++it) {
        it.value()->setEnabled(!signingIn && !haveAccount);
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
    m_download->setEnabled(haveAccount && !busy);
    m_addToPlaylist->setEnabled(haveAccount && !busy);
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
    m_planKind.clear();
    m_planRemote.clear();
}
