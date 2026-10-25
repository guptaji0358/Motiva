#include "CloudWindow.h"

#include "CloudManager.h"
#include "GlPopups.h"
#include "GlUi.h"
#include "Theme.h"

#include <QCollator>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QUrl>

#include <algorithm>

namespace {

constexpr int kFolderRole = Qt::UserRole + 1;
constexpr int kMetaRole = Qt::UserRole + 2;
constexpr int kMaxPages = 10; // 2000 entries per folder is plenty for a wallpaper picker

QString sizeText(qint64 bytes) {
    return QLocale().formattedDataSize(qMax<qint64>(0, bytes), 1, QLocale::DataSizeTraditionalFormat);
}

// Folders open on double-click; files carry a check box.
class CloudRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(200, 40); }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const GlUi::Style s = GlUi::Style::current();
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(option.rect).adjusted(2, 2, -2, -2);
        const bool folder = index.data(kFolderRole).toBool();
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
        if (!folder) {
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
        } else {
            // Folder glyph: a tab and a body.
            const QRectF f(x, r.center().y() - 8, 20, 16);
            p->setPen(Qt::NoPen);
            p->setBrush(s.accent);
            p->drawRoundedRect(QRectF(f.left(), f.top(), 9, 5), 2, 2);
            p->drawRoundedRect(QRectF(f.left(), f.top() + 3, f.width(), f.height() - 3), 3, 3);
        }
        x += 32;
        const QString meta = index.data(kMetaRole).toString();
        p->setFont(option.font);
        const int metaW = meta.isEmpty() ? 0 : p->fontMetrics().horizontalAdvance(meta) + 16;
        p->setPen(s.text);
        p->drawText(QRectF(x, r.top(), r.right() - x - metaW - 12, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                    p->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle,
                                                int(r.right() - x - metaW - 12)));
        if (!meta.isEmpty()) {
            p->setPen(s.textDim);
            p->drawText(QRectF(r.right() - metaW - 8, r.top(), metaW, r.height()), Qt::AlignVCenter | Qt::AlignRight, meta);
        }
        p->restore();
    }
};

} // namespace

CloudWindow::CloudWindow(QWidget* parent)
    : GlWindow(tr("Cloud Storage"), tr("Use wallpapers from OneDrive, Google Drive or Dropbox"), parent),
      m_cloud(CloudManager::instance()) {
    setModal(false);
    GlCanvas* c = canvas();

    m_provider = new GlDropdown(c);
    m_provider->setCompact(true);
    for (const QString& id : m_cloud->providerIds()) {
        m_provider->addItem(m_cloud->provider(id)->displayName(), id);
    }
    m_provider->setCurrentIndex(0);
    connect(m_provider, &GlDropdown::currentIndexChanged, this, [this] {
        m_trail.clear();
        refreshAccount();
        reload();
    });

    m_status = new GlLabel(c);
    m_status->setPointSize(10);
    m_status->setTone(GlLabel::Tone::Dim);

    m_setup = new GlButton(c, GlButton::Kind::Secondary);
    m_setup->setText(tr("Set Up…"));
    m_setup->setToolTip(tr("Use your own app registration for this service (only needed if Motiva has none built in)"));
    connect(m_setup, &GlButton::clicked, this, &CloudWindow::setupAccount);

    m_connect = new GlButton(c, GlButton::Kind::Primary);
    connect(m_connect, &GlButton::clicked, this, [this] {
        const QString id = providerId();
        if (m_cloud->isConnecting(id)) {
            m_cloud->cancelConnect(id);
        } else if (m_cloud->isConnected(id)) {
            m_cloud->disconnectAccount(id);
        } else if (!m_cloud->isConfigured(id)) {
            if (qEnvironmentVariableIntValue("MOTIVA_CLOUD_DEV") == 1) {
                setupAccount();
            } else {
                m_info->setText(tr("%1 sign-in isn't enabled in this version of Motiva yet.").arg(m_cloud->provider(id)->displayName()));
            }
        } else {
            bool ok = false;
            const QString email = GlMessageBox::getText(
                this, tr("Connect %1").arg(m_cloud->provider(id)->displayName()),
                tr("Enter your account email. Your browser opens the official sign-in page, where you type your "
                   "password and complete any verification (such as a code sent to your email). Motiva never sees "
                   "your password."),
                QString(), tr("Continue"), tr("name@example.com"), &ok);
            if (ok) {
                m_cloud->connectAccount(id, email);
            }
        }
    });

    m_location = new GlLabel(c);
    m_location->setPointSize(10);
    m_location->setBold(true);
    m_up = new GlButton(c, GlButton::Kind::Secondary);
    m_up->setText(tr("Up"));
    connect(m_up, &GlButton::clicked, this, &CloudWindow::goUp);
    m_refresh = new GlButton(c, GlButton::Kind::Secondary);
    m_refresh->setText(tr("Refresh"));
    connect(m_refresh, &GlButton::clicked, this, &CloudWindow::reload);

    m_model = new QStandardItemModel(this);
    m_list = new GlItemView(c);
    m_list->setModel(m_model);
    m_list->setDelegate(new CloudRowDelegate(m_list));
    m_list->setMode(GlItemView::Mode::List, QSize(200, 40), 2);
    m_list->setAccessibleName(tr("Files in your cloud drive"));
    m_list->setEmptyText(tr("Nothing to show"), tr("Connect an account, then open a folder with images or videos."));
    connect(m_list, &GlItemView::clicked, this, &CloudWindow::toggleRow);
    connect(m_list, &GlItemView::doubleClicked, this, [this](int row) {
        if (row >= 0 && row < m_entries.size() && m_entries.at(row).isFolder) {
            openFolder(m_entries.at(row).id, m_entries.at(row).name, true);
        }
    });
    m_list->setKeyFilter([this](QKeyEvent* e) {
        if (e->key() == Qt::Key_Space && !(e->modifiers() & Qt::ControlModifier)) {
            toggleRow(m_list->currentRow());
            return true;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            const int row = m_list->currentRow();
            if (row >= 0 && row < m_entries.size() && m_entries.at(row).isFolder) {
                openFolder(m_entries.at(row).id, m_entries.at(row).name, true);
                return true;
            }
        }
        return false;
    });

    m_info = new GlLabel(c);
    m_info->setPointSize(10);
    m_info->setTone(GlLabel::Tone::Dim);
    m_progress = new GlProgressBar(c);
    m_progress->setVisible(false);

    m_selectAll = new GlButton(c, GlButton::Kind::Secondary);
    m_selectAll->setText(tr("Select All"));
    connect(m_selectAll, &GlButton::clicked, this, [this] {
        for (int i = 0; i < m_model->rowCount(); ++i) {
            if (!m_entries.at(i).isFolder) {
                m_model->item(i)->setData(Qt::Checked, Qt::CheckStateRole);
            }
        }
        updateSelection();
    });
    m_close = new GlButton(c, GlButton::Kind::Secondary);
    m_close->setText(tr("Close"));
    connect(m_close, &GlButton::clicked, this, [this] {
        if (m_cloud->isDownloading()) {
            m_cloud->cancelDownloads();
        }
        close();
    });
    m_add = new GlButton(c, GlButton::Kind::Primary);
    connect(m_add, &GlButton::clicked, this, &CloudWindow::downloadSelected);

    connect(m_cloud, &CloudManager::accountChanged, this, [this](const QString& id) {
        if (id != providerId()) {
            return;
        }
        const bool hadRows = m_model->rowCount() > 0;
        refreshAccount();
        if (m_cloud->isConnected(id) && !hadRows) {
            reload();
        } else if (!m_cloud->isConnected(id)) {
            ++m_listGeneration;
            m_model->clear();
            m_entries.clear();
            updateSelection();
        }
    });
    connect(m_cloud, &CloudManager::connectFailed, this, [this](const QString& id, const QString& message) {
        if (id == providerId()) {
            m_info->setText(message);
            refreshAccount();
        }
    });
    connect(m_cloud, &CloudManager::downloadProgress, this,
            [this](int done, int total, const QString& name, int percent) {
                if (!m_progress->isVisible()) {
                    return;
                }
                m_progress->setRange(0, qMax(1, total));
                m_progress->setValue(done);
                m_progress->setText(tr("%1 / %2").arg(done).arg(total));
                m_info->setText(name.isEmpty() ? tr("Downloading…") : tr("%1  (%2%)").arg(name).arg(percent));
            });
    connect(m_cloud, &CloudManager::downloadFinished, this,
            [this](const QStringList& paths, const QStringList& errors, bool cancelled) {
                if (!m_progress->isVisible()) {
                    return; // another window's batch
                }
                m_progress->setVisible(false);
                QString text;
                if (cancelled) {
                    text = tr("Stopped. %1 file(s) were already saved.").arg(paths.size());
                } else if (!errors.isEmpty()) {
                    text = tr("%1 file(s) could not be downloaded: %2").arg(errors.size()).arg(errors.first());
                } else {
                    text = tr("Saved %1 file(s) to this PC.").arg(paths.size());
                }
                m_info->setText(text);
                for (int i = 0; i < m_model->rowCount(); ++i) {
                    m_model->item(i)->setData(Qt::Unchecked, Qt::CheckStateRole);
                }
                updateSelection();
                if (!paths.isEmpty()) {
                    emit filesReady(paths);
                }
            });

    refreshAccount();
    updateSelection();
    resize(820, 720);
    setMinimumSize(680, 560);
}

QString CloudWindow::providerId() const {
    return m_provider->currentData().toString();
}

void CloudWindow::setTarget(const QString& playlistName, std::function<bool(const QString&)> accepts) {
    m_playlistName = playlistName;
    m_accepts = std::move(accepts);
    if (m_cloud->isConnected(providerId())) {
        reload(); // the filter may differ from the last playlist
    }
    updateSelection();
}

void CloudWindow::present() {
    if (!isVisible()) {
        show();
    }
    if (isMinimized()) {
        showNormal();
    }
    raise();
    activateWindow();
}

void CloudWindow::layoutContent(const QRectF& content, const QRectF& footer) {
    const qreal L = content.left(), W = content.width();
    qreal y = content.top();
    constexpr qreal H = 38;
    // Account row.
    const qreal connectW = 130, setupW = 110, provW = 190;
    m_provider->setRect(QRectF(L, y, provW, H));
    m_connect->setRect(QRectF(content.right() - connectW, y, connectW, H));
    m_setup->setRect(QRectF(content.right() - connectW - 8 - setupW, y, setupW, H));
    m_status->setRect(QRectF(L + provW + 12, y, std::max<qreal>(40, W - provW - connectW - setupW - 40), H));
    y += H + 12;
    // Location row.
    m_up->setRect(QRectF(L, y, 70, H));
    m_refresh->setRect(QRectF(content.right() - 100, y, 100, H));
    m_location->setRect(QRectF(L + 80, y, std::max<qreal>(40, W - 80 - 110), H));
    y += H + 8;
    const qreal bottom = 8 + 26 + 8 + 22;
    m_list->setRect(QRectF(L, y, W, std::max<qreal>(120, content.bottom() - bottom - y)));
    const qreal by = content.bottom() - bottom + 8;
    m_progress->setRect(QRectF(L, by, W, 24));
    m_info->setRect(QRectF(L, by + (m_progress->isVisible() ? 30 : 0), W, 22));
    layoutFooter({m_selectAll, m_close, m_add}, footer, 150);
}

void CloudWindow::refreshAccount() {
    const QString id = providerId();
    const bool connecting = m_cloud->isConnecting(id);
    const bool connected = m_cloud->isConnected(id);
    const bool configured = m_cloud->isConfigured(id);
    m_status->setText(connecting   ? tr("Waiting for you to sign in in your browser…")
                      : connected  ? tr("Connected")
                      : configured ? tr("Not connected")
                                   : tr("Not available in this version yet"));
    m_connect->setText(connecting ? tr("Cancel") : connected ? tr("Disconnect") : tr("Connect"));
    // Developer-only: normal users never see app-registration steps.
    m_setup->setVisible(!connected && !connecting && !m_cloud->hasBuiltInCredentials(id) && qEnvironmentVariableIntValue("MOTIVA_CLOUD_DEV") == 1);
    m_up->setEnabled(connected && m_trail.size() > 0);
    m_refresh->setEnabled(connected);
    m_location->setText(connected ? (m_trail.isEmpty() ? tr("Your drive") : m_trail.last().second) : QString());
    canvas()->relayout();
}

void CloudWindow::setupAccount() {
    const QString id = providerId();
    CloudProvider* p = m_cloud->provider(id);
    const QString intro =
        tr("Motiva connects with your own %1 app registration, so it never shares one key between users.\n\n"
           "1. Register an app at %2\n2. Add this redirect URI: %3\n3. Paste its client (application) ID below.")
            .arg(p->displayName(), p->registrationUrl(), CloudManager::redirectUri());
    QDesktopServices::openUrl(QUrl(p->registrationUrl()));
    bool ok = false;
    const QString clientId = GlMessageBox::getText(this, tr("%1 setup").arg(p->displayName()), intro,
                                                   m_cloud->clientId(id), tr("Save"), tr("Client ID"), &ok);
    if (!ok) {
        return;
    }
    QString secret = m_cloud->clientSecret(id);
    if (p->usesClientSecret() && !clientId.trimmed().isEmpty()) {
        bool ok2 = false;
        const QString entered = GlMessageBox::getText(
            this, tr("%1 setup").arg(p->displayName()),
            tr("Google also issues a client secret for desktop apps. Paste it here (it is stored encrypted for your "
               "Windows account)."),
            QString(), tr("Save"), tr("Client secret"), &ok2);
        if (ok2) {
            secret = entered;
        }
    }
    m_cloud->setClientCredentials(id, clientId, secret);
    refreshAccount();
}

void CloudWindow::openFolder(const QString& folderId, const QString& name, bool push) {
    if (push) {
        m_trail.append({folderId, name});
    }
    refreshAccount();
    reload();
}

void CloudWindow::goUp() {
    if (!m_trail.isEmpty()) {
        m_trail.removeLast();
        refreshAccount();
        reload();
    }
}

void CloudWindow::reload() {
    const QString id = providerId();
    ++m_listGeneration;
    m_model->clear();
    m_entries.clear();
    updateSelection();
    if (!m_cloud->isConnected(id)) {
        return;
    }
    m_loading = true;
    m_info->setText(tr("Loading…"));
    loadPage(QString(), m_listGeneration, 0);
}

void CloudWindow::loadPage(const QString& pageToken, int generation, int pages) {
    const QString id = providerId();
    const QString folder = m_trail.isEmpty() ? QString() : m_trail.last().first;
    QPointer<CloudWindow> self(this);
    m_cloud->list(id, folder, pageToken, [self, generation, pages](const CloudListPage& page) {
        if (!self || generation != self->m_listGeneration) {
            return; // the user moved on
        }
        if (!page.ok) {
            self->m_loading = false;
            self->m_info->setText(page.error.isEmpty() ? tr("Could not read this folder.") : page.error);
            return;
        }
        for (const CloudEntry& e : page.entries) {
            if (e.isFolder || (self->m_accepts ? self->m_accepts(e.name) : true)) {
                self->m_entries << e;
            }
        }
        if (!page.nextPage.isEmpty() && pages + 1 < kMaxPages) {
            self->loadPage(page.nextPage, generation, pages + 1);
            return;
        }
        self->m_loading = false;
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::stable_sort(self->m_entries.begin(), self->m_entries.end(), [&collator](const CloudEntry& a, const CloudEntry& b) {
            return a.isFolder != b.isFolder ? a.isFolder : collator.compare(a.name, b.name) < 0;
        });
        self->m_model->clear();
        const QString pid = self->providerId();
        int files = 0;
        for (const CloudEntry& e : self->m_entries) {
            auto* item = new QStandardItem(e.name);
            item->setData(e.isFolder, kFolderRole);
            if (!e.isFolder) {
                ++files;
                item->setData(Qt::Unchecked, Qt::CheckStateRole);
                QString meta = sizeText(e.size);
                if (self->m_cloud->isCached(pid, e)) {
                    meta += tr("  ·  on this PC");
                }
                item->setData(meta, kMetaRole);
            }
            self->m_model->appendRow(item);
        }
        self->m_info->setText(files == 0 && self->m_entries.isEmpty()
                                  ? tr("No supported files or folders here.")
                                  : tr("Double-click a folder to open it. Tick files to add."));
        self->updateSelection();
    });
}

void CloudWindow::toggleRow(int row) {
    if (row < 0 || row >= m_entries.size() || m_entries.at(row).isFolder || m_cloud->isDownloading()) {
        return;
    }
    QStandardItem* item = m_model->item(row);
    item->setData(item->data(Qt::CheckStateRole).toInt() == Qt::Checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
    updateSelection();
}

void CloudWindow::updateSelection() {
    int n = 0;
    qint64 bytes = 0;
    for (int i = 0; i < m_model->rowCount() && i < m_entries.size(); ++i) {
        if (m_model->item(i)->data(Qt::CheckStateRole).toInt() == Qt::Checked) {
            ++n;
            bytes += m_entries.at(i).size;
        }
    }
    const bool busy = m_cloud->isDownloading();
    m_add->setText(n == 0 ? (m_playlistName.isEmpty() ? tr("Add to Playlist") : tr("Add to \"%1\"").arg(m_playlistName))
                          : tr("Add %1 (%2)").arg(n).arg(sizeText(bytes)));
    m_add->setEnabled(n > 0 && !busy);
    m_selectAll->setEnabled(!busy && m_model->rowCount() > 0);
}

void CloudWindow::downloadSelected() {
    QList<CloudEntry> picked;
    for (int i = 0; i < m_model->rowCount() && i < m_entries.size(); ++i) {
        if (m_model->item(i)->data(Qt::CheckStateRole).toInt() == Qt::Checked) {
            picked << m_entries.at(i);
        }
    }
    if (picked.isEmpty()) {
        return;
    }
    m_progress->setVisible(true);
    m_progress->setRange(0, picked.size());
    m_progress->setValue(0);
    m_progress->setText(tr("0 / %1").arg(picked.size()));
    m_info->setText(tr("Downloading…"));
    if (!m_cloud->downloadMany(providerId(), picked)) {
        m_progress->setVisible(false);
        m_info->setText(tr("A download is already running."));
    }
    updateSelection();
    canvas()->relayout();
}
