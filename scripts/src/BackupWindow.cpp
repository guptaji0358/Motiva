#include "GlPopups.h"
#include "BackupWindow.h"
#include "BackupManager.h"

#include "DialogSizing.h"
#include "GlUi.h"
#include "Theme.h"
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
        GlMessageBox::warning(parent, tr("Media Backup"), preview.problem);
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
    GlMessageBox box(icon, tr("Media Backup"), text, QMessageBox::NoButton, parent);
    box.setInformativeText(info);
    auto* cancel = box.addButton(QMessageBox::Cancel);
    auto* enable = box.addButton(tr("Turn On Backup"), QMessageBox::AcceptRole);
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
        const auto answer = GlMessageBox::question(
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

BackupWindow::BackupWindow(BackupManager* manager, QWidget* parent)
    : GlWindow(tr("Media Backup"), tr("Local copies of your media"), parent), m_manager(manager) {
    setModal(false);
    GlCanvas* c = canvas();

    m_intro = new GlLabel(c);
    m_intro->setText(explanationText());
    m_intro->setWordWrap(true);
    m_intro->setPointSize(10);
    m_intro->setTone(GlLabel::Tone::Dim);
    m_intro->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    m_enableCheck = new GlToggle(c);
    m_enableCheck->setText(tr("Back up my media"));
    connect(m_enableCheck, &GlToggle::toggled, this, [this](bool on) {
        const bool ok = on ? confirmAndEnable(m_manager, this) : confirmAndDisable(m_manager, this);
        if (!ok) {
            m_enableCheck->blockSignals(true);
            m_enableCheck->setChecked(!on);
            m_enableCheck->blockSignals(false);
        }
        refresh();
    });

    m_status = new GlLabel(c);
    m_status->setWordWrap(true);
    m_status->setPointSize(10.5);
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_progress = new GlProgressBar(c);
    m_progressDetail = new GlLabel(c);
    m_progressDetail->setPointSize(9.5);
    m_progressDetail->setTone(GlLabel::Tone::Dim);
    m_cancelButton = new GlButton(c, GlButton::Kind::Secondary);
    m_cancelButton->setText(tr("Cancel"));
    m_cancelButton->setToolTip(tr("Stop after the current file. Finished copies are kept."));
    connect(m_cancelButton, &GlButton::clicked, m_manager, &BackupManager::cancel);
    m_problem = new GlLabel(c);
    m_problem->setWordWrap(true);
    m_problem->setColor(QColor(QLatin1String(Theme::kStatusError)));
    m_problem->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    m_storageHeading = new GlLabel(c);
    m_storageHeading->setText(tr("Backup storage"));
    m_storageHeading->setCaps(true);
    m_storageHeading->setBold(true);
    m_storageHeading->setPointSize(8.5);
    m_storageHeading->setTone(GlLabel::Tone::Accent);

    m_stack.addWrapped(m_intro, 14);
    m_stack.add(m_enableCheck, 46, 6);
    m_stack.addWrapped(m_status, 8, 22);
    m_stack.add(m_progress, 26, 4);
    m_stack.addPair(m_progressDetail, -130, m_cancelButton, 34, 8);
    m_stack.addWrapped(m_problem, 8, 0);
    m_stack.addSpace(6);
    m_stack.add(m_storageHeading, 26, 4);
    const auto addRow = [&](const QString& label, GlLabel** value) {
        auto* l = new GlLabel(c);
        l->setText(label);
        l->setPointSize(10);
        l->setTone(GlLabel::Tone::Dim);
        *value = new GlLabel(c);
        (*value)->setPointSize(10);
        m_stack.addPair(l, 150, *value, 26, 2);
    };
    addRow(tr("Location"), &m_locationValue);
    addRow(tr("Backed up"), &m_itemsValue);
    addRow(tr("Space used"), &m_sizeValue);
    addRow(tr("Waiting"), &m_pendingValue);
    addRow(tr("Failed"), &m_failedValue);
    addRow(tr("Unused backups"), &m_unusedValue);

    m_backUpNow = new GlButton(c, GlButton::Kind::Secondary);
    m_backUpNow->setText(tr("Back Up Now"));
    m_backUpNow->setToolTip(tr("Copy anything not backed up yet and retry failed files"));
    connect(m_backUpNow, &GlButton::clicked, m_manager, &BackupManager::backUpNow);
    m_openFolder = new GlButton(c, GlButton::Kind::Secondary);
    m_openFolder->setText(tr("Open Backup Folder"));
    connect(m_openFolder, &GlButton::clicked, this, [this] {
        const QString path = m_manager->provider()->location();
        QDir().mkpath(path);
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    m_removeButton = new GlButton(c, GlButton::Kind::Secondary);
    m_removeButton->setText(tr("Remove Backups…"));
    m_removeButton->setToolTip(tr("Deleting backups is done in Cleanup & Reset"));
    connect(m_removeButton, &GlButton::clicked, this, &BackupWindow::openCleanupRequested);
    m_closeButton = new GlButton(c, GlButton::Kind::Secondary);
    m_closeButton->setText(tr("Close"));
    connect(m_closeButton, &GlButton::clicked, this, &QDialog::close);

    connect(m_manager, &BackupManager::enabledChanged, this, &BackupWindow::refresh);
    connect(m_manager, &BackupManager::stateChanged, this, &BackupWindow::refresh);
    connect(m_manager, &BackupManager::progressChanged, this, &BackupWindow::refresh);
    connect(m_manager, &BackupManager::statsChanged, this, &BackupWindow::refresh);
    refresh();
    resize(800, 660);
    setMinimumSize(700, 560);
}

void BackupWindow::layoutContent(const QRectF& content, const QRectF& footer) {
    m_stack.layout(content);
    layoutFooter({m_backUpNow, m_openFolder, m_removeButton, m_closeButton}, footer, 110);
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
    GlWindow::showEvent(event);
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
        m_progress->setText(tr("%1 / %2").arg(m_manager->done()).arg(m_manager->total()));
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
    canvas()->relayout();
}
