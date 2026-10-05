#include "BackupWindow.h"
#include "BackupManager.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

QString BackupWindow::explanationText() {
    return tr("Backup is optional and disabled by default. Motiva does not create backup copies of your media "
              "unless you explicitly enable this feature. When enabled, media is copied to Motiva's local backup "
              "storage on this device. Nothing is uploaded anywhere.");
}

QString BackupWindow::sizeText(qint64 bytes) {
    return QLocale().formattedDataSize(qMax<qint64>(0, bytes), 1, QLocale::DataSizeTraditionalFormat);
}

bool BackupWindow::confirmAndEnable(BackupManager* manager, QWidget* parent) {
    const BackupManager::EnablePreview preview = manager->previewEnable();
    if (!preview.problem.isEmpty()) {
        QMessageBox::warning(parent, tr("Media Backup"), preview.problem);
        return false;
    }
    QString text = tr("Turn on local backup of your media?");
    QString info = explanationText() + QStringLiteral("\n\n");
    if (preview.items > 0) {
        info += tr("Motiva will now copy %1 media file(s) (about %2) from your playlists to:\n%3")
                    .arg(preview.items)
                    .arg(sizeText(preview.bytes), preview.location);
    } else {
        info += tr("Your playlists have no media to copy yet. Media you add later will be copied to:\n%1")
                    .arg(preview.location);
    }
    info += tr("\n\nCopying runs in the background and can be cancelled any time. Your original files are only read, "
               "never changed, moved or deleted.");
    QMessageBox::Icon icon = QMessageBox::Question;
    if (preview.freeBytes >= 0 && preview.bytes > 0 && preview.freeBytes < preview.bytes + 256LL * 1024 * 1024) {
        info += tr("\n\nWarning: only %1 is free on that drive, so not everything may fit.").arg(sizeText(preview.freeBytes));
        icon = QMessageBox::Warning;
    }
    QMessageBox box(icon, tr("Media Backup"), text, QMessageBox::NoButton, parent);
    box.setInformativeText(info);
    QPushButton* cancel = box.addButton(QMessageBox::Cancel);
    QPushButton* enable = box.addButton(tr("Turn On Backup"), QMessageBox::AcceptRole);
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    if (box.clickedButton() != enable) {
        return false;
    }
    manager->setEnabled(true);
    return true;
}

bool BackupWindow::confirmAndDisable(BackupManager* manager, QWidget* parent) {
    if (manager->state() == BackupManager::State::Running) {
        const auto answer = QMessageBox::question(
            parent, tr("Media Backup"),
            tr("A backup is running. Turn backup off and stop it?\n\nCopies already made are kept; files not yet "
               "copied are simply skipped."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            return false;
        }
    }
    manager->setEnabled(false);
    return true;
}

BackupWindow::BackupWindow(BackupManager* manager, QWidget* parent) : QDialog(parent), m_manager(manager) {
    setWindowTitle(tr("Media Backup"));
    setModal(false);
    setMinimumSize(520, 560);
    resize(560, 600);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 18);
    root->setSpacing(10);

    auto* title = new QLabel(tr("Media Backup"), this);
    title->setObjectName(QStringLiteral("appTitle"));
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSize(tf.pointSize() + 3);
    title->setFont(tf);
    root->addWidget(title);
    auto* intro = new QLabel(explanationText(), this);
    intro->setObjectName(QStringLiteral("secondaryText"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    m_enableCheck = new QCheckBox(tr("Back up my media"), this);
    QFont bf = m_enableCheck->font();
    bf.setBold(true);
    m_enableCheck->setFont(bf);
    connect(m_enableCheck, &QCheckBox::toggled, this, [this](bool on) {
        const bool ok = on ? confirmAndEnable(m_manager, this) : confirmAndDisable(m_manager, this);
        if (!ok) {
            m_enableCheck->blockSignals(true);
            m_enableCheck->setChecked(!on);
            m_enableCheck->blockSignals(false);
        }
        refresh();
    });
    root->addWidget(m_enableCheck);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);
    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(true);
    root->addWidget(m_progress);
    auto* progressRow = new QHBoxLayout();
    m_progressDetail = new QLabel(this);
    m_progressDetail->setObjectName(QStringLiteral("secondaryText"));
    progressRow->addWidget(m_progressDetail, 1);
    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setToolTip(tr("Stop after the current file. Finished copies are kept."));
    connect(m_cancelButton, &QPushButton::clicked, m_manager, &BackupManager::cancel);
    progressRow->addWidget(m_cancelButton);
    root->addLayout(progressRow);
    m_problem = new QLabel(this);
    m_problem->setWordWrap(true);
    m_problem->setStyleSheet(QStringLiteral("color: #d64545;"));
    root->addWidget(m_problem);

    auto* detailsLabel = new QLabel(tr("Backup storage"), this);
    detailsLabel->setObjectName(QStringLiteral("sectionLabel"));
    detailsLabel->setFont(bf);
    root->addWidget(detailsLabel);
    auto* grid = new QGridLayout();
    grid->setColumnStretch(1, 1);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(6);
    auto addRow = [&](int row, const QString& label, QLabel** value) {
        auto* l = new QLabel(label, this);
        l->setObjectName(QStringLiteral("secondaryText"));
        grid->addWidget(l, row, 0, Qt::AlignTop);
        *value = new QLabel(this);
        (*value)->setWordWrap(true);
        (*value)->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(*value, row, 1);
    };
    addRow(0, tr("Location"), &m_locationValue);
    addRow(1, tr("Backed up"), &m_itemsValue);
    addRow(2, tr("Space used"), &m_sizeValue);
    addRow(3, tr("Waiting"), &m_pendingValue);
    addRow(4, tr("Failed"), &m_failedValue);
    addRow(5, tr("Unused backups"), &m_unusedValue);
    root->addLayout(grid);
    root->addStretch();

    auto* buttons = new QHBoxLayout();
    m_backUpNow = new QPushButton(tr("Back Up Now"), this);
    m_backUpNow->setToolTip(tr("Copy anything not backed up yet and retry failed files"));
    connect(m_backUpNow, &QPushButton::clicked, m_manager, &BackupManager::backUpNow);
    buttons->addWidget(m_backUpNow);
    auto* openFolder = new QPushButton(tr("Open Backup Folder"), this);
    connect(openFolder, &QPushButton::clicked, this, [this] {
        const QString path = m_manager->provider()->location();
        QDir().mkpath(path);
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    buttons->addWidget(openFolder);
    buttons->addStretch();
    auto* remove = new QPushButton(tr("Remove Backups…"), this);
    remove->setToolTip(tr("Deleting backups is done in Cleanup & Reset"));
    connect(remove, &QPushButton::clicked, this, &BackupWindow::openCleanupRequested);
    buttons->addWidget(remove);
    auto* close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(close);
    root->addLayout(buttons);

    connect(m_manager, &BackupManager::enabledChanged, this, &BackupWindow::refresh);
    connect(m_manager, &BackupManager::stateChanged, this, &BackupWindow::refresh);
    connect(m_manager, &BackupManager::progressChanged, this, &BackupWindow::refresh);
    connect(m_manager, &BackupManager::statsChanged, this, &BackupWindow::refresh);
    refresh();
}

void BackupWindow::present() {
    if (!isVisible()) {
        show();
    }
    if (isMinimized()) {
        showNormal();
    }
    raise();
    activateWindow();
}

void BackupWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    m_manager->refreshStats();
    refresh();
}

void BackupWindow::refresh() {
    const bool on = m_manager->isEnabled();
    const bool running = m_manager->state() == BackupManager::State::Running ||
                         m_manager->state() == BackupManager::State::Stopping;
    m_enableCheck->blockSignals(true);
    m_enableCheck->setChecked(on);
    m_enableCheck->blockSignals(false);
    m_status->setText(m_manager->statusText());
    m_progress->setVisible(running);
    m_progressDetail->setVisible(running);
    m_cancelButton->setVisible(running);
    m_cancelButton->setEnabled(m_manager->state() == BackupManager::State::Running);
    if (running) {
        m_progress->setRange(0, qMax(1, m_manager->total()));
        m_progress->setValue(m_manager->done());
        m_progress->setFormat(tr("%1 / %2").arg(m_manager->done()).arg(m_manager->total()));
        m_progressDetail->setText(m_manager->currentName().isEmpty()
                                      ? QString()
                                      : tr("%1  (%2%)").arg(m_manager->currentName()).arg(m_manager->currentPercent()));
    }
    m_backUpNow->setEnabled(on && !running);
    const QString problem = m_manager->lastProblem();
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());

    const BackupStats s = m_manager->stats();
    m_locationValue->setText(m_manager->provider()->location());
    m_itemsValue->setText(s.complete == 1 ? tr("1 media file") : tr("%1 media files").arg(s.complete));
    m_sizeValue->setText(sizeText(s.bytes));
    m_pendingValue->setText(on ? tr("%1 not backed up yet").arg(s.pending) : tr("-"));
    m_failedValue->setText(QString::number(s.failed));
    m_unusedValue->setText(s.unused == 0 ? tr("none")
                                         : tr("%1 (their media is no longer in any playlist)").arg(s.unused));
}
