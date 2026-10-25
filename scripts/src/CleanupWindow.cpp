#include "GlPopups.h"
#include "CleanupWindow.h"

#include "DialogSizing.h"
#include "GlUi.h"
#include "Theme.h"
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

using Op = CleanupManager::Operation;

namespace {

// Severity dots for the three levels + danger zone: green and red are
// Motiva's theme-independent status colors; yellow and orange sit between
// them (Theme::kStatusWarning is too close to orange to tell apart).
constexpr const char* kDotYellow = "#d8b01e";
constexpr const char* kDotOrange = "#d9772b";

constexpr int kMarkSize = 16;

QPixmap markPixmap(const char* resource, const QColor& color) {
    return Theme::tintedIcon(QLatin1String(resource), color, kMarkSize);
}

} // namespace

CleanupWindow::CleanupWindow(CleanupManager* manager, QWidget* parent)
    : GlWindow(tr("Cleanup & Reset"), tr("Manage cache, data and reset options"), parent), m_manager(manager) {
    // A utility window, not a modal step of Settings: closing it just
    // returns to Settings, and Motiva keeps running.
    setModal(false);
    GlCanvas* c = canvas();

    m_levels << makeLevelRow(Theme::kStatusSuccess, tr("CACHE"),
                             tr("Temporary files, the diagnostic log and preview thumbnails."), tr("Delete Cache"),
                             false, Op::DeleteCache);
    m_levels << makeLevelRow(kDotYellow, tr("DATA"), tr("Playlists, the .mtv library and saved media references."),
                             tr("Delete Data"), true, Op::DeleteData);
    m_levels << makeLevelRow(kDotOrange, tr("CACHE + DATA"),
                             tr("Removes both cache and application data. Settings and theme are kept."),
                             tr("Delete Cache + Data"), true, Op::DeleteCacheAndData);
    // Backups are neither cache nor data: their own row, removed only on request.
    m_levels << makeLevelRow("#8b6fd6", tr("MEDIA BACKUPS"),
                             tr("Copies of your media that Motiva made after you turned Backup on. Never part of "
                                "Cache or Data - removed only here, and only on request."),
                             tr("Delete Media Backups"), true, Op::DeleteBackups);
    m_levels << makeLevelRow("#3aa6d8", tr("DOWNLOADED MEDIA"),
                             tr("Pictures and videos Motiva saved from your browser extension or downloaded from cloud "
                                "storage. The originals (websites, cloud) are not touched; playlists using these copies "
                                "will show them as missing."),
                             tr("Delete Downloaded Media"), true, Op::DeleteCloudCache);
    m_cacheSize = m_levels[0].size;
    m_dataSize = m_levels[1].size;
    m_combinedSize = m_levels[2].size;
    m_backupSize = m_levels[3].size;
    m_cloudSize = m_levels[4].size;

    m_advancedRule = new GlRule(c);
    m_advancedHeading = new GlLabel(c);
    m_advancedHeading->setText(tr("ADVANCED"));
    m_advancedHeading->setBold(true);
    m_advancedHeading->setCaps(true);
    m_advancedHeading->setPointSize(9);
    m_advancedDesc = new GlLabel(c);
    m_advancedDesc->setText(tr("Individual cleanup operations for data Motiva currently has."));
    m_advancedDesc->setPointSize(9.5);
    m_advancedDesc->setTone(GlLabel::Tone::Dim);
    m_moreButton = new GlButton(c, GlButton::Kind::Secondary);
    m_moreButton->setText(tr("More…"));
    connect(m_moreButton, &GlButton::clicked, this, &CleanupWindow::showMoreDialog);

    m_dangerCard = new GlPanel(c);
    m_dangerCard->setBlock(GlPanel::Block::Danger);
    m_dangerCard->setRadiusPx(16);
    m_dangerDot = new GlLabel(c);
    m_dangerDot->setPixmap(markPixmap(":/status/status-dot.svg", QColor(QLatin1String(Theme::kStatusError))));
    m_dangerHeading = new GlLabel(c);
    m_dangerHeading->setText(tr("DANGER ZONE"));
    m_dangerHeading->setBold(true);
    m_dangerHeading->setCaps(true);
    m_dangerHeading->setPointSize(9);
    m_dangerHeading->setColor(QColor(QLatin1String(Theme::kStatusError)));
    m_dangerDesc = new GlLabel(c);
    m_dangerDesc->setText(tr("Return Motiva to a fresh-install state: settings, theme, playlists, library, "
                             "recovery state, cache and Windows integration. Motiva restarts afterwards."));
    m_dangerDesc->setWordWrap(true);
    m_dangerDesc->setPointSize(9.5);
    m_dangerDesc->setTone(GlLabel::Tone::Dim);
    m_dangerDesc->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_factoryButton = new GlButton(c, GlButton::Kind::Secondary);
    m_factoryButton->setText(tr("Factory Reset"));
    connect(m_factoryButton, &GlButton::clicked, this, [this] {
        if (confirm(Op::FactoryReset)) {
            runWithProgress(Op::FactoryReset);
        }
    });

    // Progress page.
    m_progressTitle = new GlLabel(c);
    m_progressTitle->setText(tr("Cleaning Motiva…"));
    m_progressTitle->setBold(true);
    m_progressTitle->setPointSize(13);
    m_progressStatus = new GlLabel(c);
    m_progressStatus->setWordWrap(true);
    m_progressStatus->setPointSize(10.5);
    m_progressStatus->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_progressButton = new GlButton(c, GlButton::Kind::Primary);
    connect(m_progressButton, &GlButton::clicked, this, &CleanupWindow::onProgressButton);

    // Footer.
    m_safeNote = new GlLabel(c);
    m_safeNote->setText(tr("Motiva-owned data only · Original media files are safe"));
    m_safeNote->setPointSize(9.5);
    m_safeNote->setTone(GlLabel::Tone::Dim);
    m_closeButton = new GlButton(c, GlButton::Kind::Secondary);
    m_closeButton->setText(tr("Close"));
    connect(m_closeButton, &GlButton::clicked, this, &QDialog::close);

    connect(m_manager, &CleanupManager::sizesReady, this, &CleanupWindow::onSizesReady);
    connect(m_manager, &CleanupManager::stepStarted, this, [this](int i) {
        if (m_running) {
            setStepRow(i, StepMark::Running, nullptr, QString());
        }
    });
    connect(m_manager, &CleanupManager::stepFinished, this, [this](int i, bool ok, const QString& detail) {
        if (m_running) {
            setStepRow(i, ok ? StepMark::Succeeded : StepMark::Failed,
                       ok ? Theme::kStatusSuccess : Theme::kStatusError, detail);
        }
    });
    connect(m_manager, &CleanupManager::finished, this, &CleanupWindow::onOperationFinished);
    showPage(0);
    resize(860, 840);
    setMinimumSize(720, 700);
}

CleanupWindow::LevelRow CleanupWindow::makeLevelRow(const char* dotColor, const QString& title,
                                                    const QString& description, const QString& buttonText,
                                                    bool destructive, Op op) {
    GlCanvas* c = canvas();
    LevelRow r;
    r.dot = new GlLabel(c);
    r.dot->setPixmap(markPixmap(":/status/status-dot.svg", QColor(QLatin1String(dotColor))));
    r.heading = new GlLabel(c);
    r.heading->setText(title);
    r.heading->setAccessibleName(title);
    r.heading->setBold(true);
    r.heading->setCaps(true);
    r.heading->setPointSize(9);
    r.desc = new GlLabel(c);
    r.desc->setText(description);
    r.desc->setWordWrap(true);
    r.desc->setPointSize(9.5);
    r.desc->setTone(GlLabel::Tone::Dim);
    r.desc->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    r.size = new GlLabel(c);
    r.size->setText(tr("Size: calculating…"));
    r.size->setPointSize(10);
    r.button = new GlButton(c, GlButton::Kind::Secondary);
    r.button->setText(buttonText);
    Q_UNUSED(destructive); // the destructive look is carried by the confirmation box
    connect(r.button, &GlButton::clicked, this, [this, op] {
        if (confirm(op)) {
            runWithProgress(op);
        }
    });
    r.rule = new GlRule(c);
    return r;
}

void CleanupWindow::showPage(int page) {
    m_page = page;
    const bool levels = page == 0;
    for (LevelRow& r : m_levels) {
        for (GlItem* it : QList<GlItem*>{r.dot, r.heading, r.desc, r.size, r.button, r.rule}) {
            it->setVisible(levels);
        }
    }
    for (GlItem* it : QList<GlItem*>{m_advancedRule, m_advancedHeading, m_advancedDesc, m_moreButton, m_dangerCard,
                                     m_dangerDot, m_dangerHeading, m_dangerDesc, m_factoryButton}) {
        it->setVisible(levels);
    }
    m_progressTitle->setVisible(!levels);
    m_progressStatus->setVisible(!levels);
    m_progressButton->setVisible(!levels);
    for (int i = 0; i < m_stepRows.size(); ++i) {
        const bool on = !levels && i < m_stepCount;
        m_stepIcons[i]->setVisible(on);
        m_stepRows[i]->setVisible(on);
        m_stepDetails[i]->setVisible(on && !m_stepDetails[i]->text().isEmpty());
    }
    canvas()->relayout();
}

void CleanupWindow::layoutContent(const QRectF& content, const QRectF& footer) {
    m_safeNote->setRect(QRectF(footer.left() + 12, footer.top() + 12, footer.width() - 150, 26));
    m_closeButton->setRect(QRectF(footer.right() - 120, footer.top() + 6, 120, 40));
    if (m_page == 0) {
        const qreal btnW = 190;
        qreal y = content.top();
        for (int i = 0; i < m_levels.size(); ++i) {
            LevelRow& r = m_levels[i];
            const qreal textW = content.width() - btnW - 40;
            const qreal descH = std::max<qreal>(20, r.desc->heightForWidth(textW - 28) + 2);
            const qreal rowH = 10 + 24 + descH + 24 + 10;
            r.dot->setRect(QRectF(content.left(), y + 10, 18, 24));
            r.heading->setRect(QRectF(content.left() + 28, y + 10, textW - 28, 24));
            r.desc->setRect(QRectF(content.left() + 28, y + 34, textW - 28, descH));
            r.size->setRect(QRectF(content.left() + 28, y + 34 + descH, textW - 28, 24));
            r.button->setRect(QRectF(content.right() - btnW, y + rowH / 2 - 20, btnW, 40));
            y += rowH;
            r.rule->setRect(QRectF(content.left(), y, content.width(), 1));
            y += 2;
        }
        y += 10;
        m_advancedHeading->setRect(QRectF(content.left(), y, content.width() - btnW - 20, 22));
        m_advancedDesc->setRect(QRectF(content.left(), y + 22, content.width() - btnW - 20, 22));
        m_moreButton->setRect(QRectF(content.right() - 130, y + 4, 130, 38));
        m_advancedRule->setRect(QRectF(content.left(), y - 6, content.width(), 1));
        y += 62;
        const qreal descW = content.width() - btnW - 40 - 28;
        const qreal dh = std::max<qreal>(22, m_dangerDesc->heightForWidth(descW) + 2);
        const QRectF card(content.left(), y, content.width(), 16 + 24 + dh + 16);
        m_dangerCard->setRect(card);
        m_dangerDot->setRect(QRectF(card.left() + 20, card.top() + 14, 18, 24));
        m_dangerHeading->setRect(QRectF(card.left() + 46, card.top() + 14, card.width() - 260, 24));
        m_dangerDesc->setRect(QRectF(card.left() + 46, card.top() + 40, descW, dh));
        m_factoryButton->setRect(QRectF(card.right() - 20 - 170, card.center().y() - 20, 170, 40));
    } else {
        m_progressTitle->setRect(QRectF(content.left(), content.top(), content.width(), 30));
        qreal y = content.top() + 44;
        for (int i = 0; i < m_stepCount; ++i) {
            const qreal tw = content.width() - 30;
            const qreal th = std::max<qreal>(22, m_stepRows[i]->heightForWidth(tw) + 2);
            m_stepIcons[i]->setRect(QRectF(content.left(), y + 3, 20, 20));
            m_stepRows[i]->setRect(QRectF(content.left() + 30, y, tw, th));
            y += th;
            if (m_stepDetails[i]->isVisible()) {
                const qreal dh = std::max<qreal>(18, m_stepDetails[i]->heightForWidth(tw) + 2);
                m_stepDetails[i]->setRect(QRectF(content.left() + 30, y, tw, dh));
                y += dh;
            }
            y += 10;
        }
        const qreal sh = m_progressStatus->text().isEmpty() ? 0 : m_progressStatus->heightForWidth(content.width()) + 6;
        m_progressStatus->setRect(QRectF(content.left(), y + 8, content.width(), std::max<qreal>(0, sh)));
        m_progressButton->setRect(QRectF(content.right() - 230, content.bottom() - 48, 230, 48));
    }
}

void CleanupWindow::present() {
    if (!isVisible()) {
        show();
    }
    if (isMinimized()) {
        showNormal();
    }
    raise();
    activateWindow();
}

void CleanupWindow::showEvent(QShowEvent* event) {
    GlWindow::showEvent(event);
    if (!m_positioned) {
        // Centered on Motiva's main window when it is on screen, otherwise
        // on Settings (Motiva may be running from the tray).
        QWidget* anchor = parentWidget();
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (qobject_cast<QMainWindow*>(w) && w->isVisible() && !w->isMinimized()) {
                anchor = w;
                break;
            }
        }
        if (anchor) {
            move(anchor->frameGeometry().center() - QPoint(width() / 2, height() / 2));
        }
        m_positioned = true;
    }
    if (!m_running) {
        m_manager->refreshSizes();
    }
}

void CleanupWindow::closeEvent(QCloseEvent* event) {
    if (m_running || m_restartPending) {
        event->ignore(); // never dismissable mid-run
        if (m_restartPending) {
            onProgressButton(); // closing after a Factory Reset restarts too
        }
        return;
    }
    QDialog::closeEvent(event);
    if (QWidget* settings = parentWidget(); settings && settings->isVisible()) {
        settings->activateWindow(); // back to Settings
    }
}

void CleanupWindow::reject() {
    if (m_running || m_restartPending) {
        return; // Esc mid-run
    }
    QDialog::reject();
    if (QWidget* settings = parentWidget(); settings && settings->isVisible()) {
        settings->activateWindow();
    }
}

bool CleanupWindow::eventFilter(QObject* watched, QEvent* event) {
    if (!m_running || !watched->isWidgetType()) {
        return QDialog::eventFilter(watched, event);
    }
    QWidget* target = static_cast<QWidget*>(watched)->window();
    if (target == this) {
        return false;
    }
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::Wheel:
    case QEvent::Shortcut:
    case QEvent::ShortcutOverride:
    case QEvent::Drop:
        return true;
    case QEvent::Close:
        event->ignore(); // a QCloseEvent starts accepted
        return true;
    default:
        return false;
    }
}

QString CleanupWindow::sizeText(qint64 bytes) {
    if (bytes < 0) {
        return tr("Size: unavailable");
    }
    if (bytes == 0) {
        return tr("Size: nothing stored on disk");
    }
    return tr("Size: approx. %1").arg(QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat));
}

void CleanupWindow::onSizesReady(const CleanupManager::Sizes& sizes) {
    m_sizes = sizes;
    const int thumbnails = m_manager->cachedThumbnailCount();
    QString cache = sizeText(sizes.cacheBytes);
    if (thumbnails > 0) {
        cache += thumbnails == 1 ? tr(" + 1 preview in memory") : tr(" + %1 previews in memory").arg(thumbnails);
    }
    m_cacheSize->setText(cache);
    m_dataSize->setText(sizeText(sizes.dataBytes));
    m_backupSize->setText(sizes.backupBytes < 0 ? sizeText(-1)
                          : sizes.backupBytes == 0
                              ? tr("Size: no backups stored")
                              : sizeText(sizes.backupBytes) +
                                    (sizes.backupItems > 0 ? tr("  \u2022  %1 media files").arg(sizes.backupItems) : QString()));
    m_cloudSize->setText(sizes.cloudBytes <= 0 ? tr("Size: nothing saved")
                                               : sizeText(sizes.cloudBytes) + tr("  •  %1 files").arg(sizes.cloudFiles));
    m_combinedSize->setText(sizes.cacheBytes < 0 || sizes.dataBytes < 0 ? sizeText(-1)
                                                                        : sizeText(sizes.cacheBytes + sizes.dataBytes));
}

bool CleanupWindow::confirm(Op op) {
    QString title, text, info, action;
    QMessageBox::Icon icon = QMessageBox::Warning;
    const QString mediaSafe = tr("Your original video and image files will NOT be deleted.");
    switch (op) {
    case Op::DeleteCache:
        title = tr("Delete Cache");
        text = tr("This will remove Motiva's temporary cache files.");
        info = tr("Removed: the diagnostic log, leftover temporary files and preview thumbnails held in memory "
                  "(they are recreated when needed).\n\nYour playlists, settings, and original media files will "
                  "not be deleted.");
        action = tr("Delete Cache");
        icon = QMessageBox::Question;
        break;
    case Op::DeleteData:
        title = tr("Delete Data");
        text = tr("Delete your Motiva playlists and library?");
        info = tr("Removed:\n• every playlist, its order and settings (the .mtv library)\n• saved media "
                  "references, including the current wallpaper media\n• unreadable library copies Motiva set "
                  "aside earlier\n\nMotiva's wallpaper is released first and a new, empty library is created.\n"
                  "Kept: your settings, theme, cache and media backups.\n\n%1\n\nTo keep a copy, use Playlists → Export… first. "
                  "This action cannot be undone.")
                   .arg(mediaSafe);
        action = tr("Delete Data");
        break;
    case Op::DeleteCacheAndData:
        title = tr("Delete Cache + Data");
        text = tr("Delete Motiva's cache and all playlist/library data?");
        info = tr("Removed:\n• every playlist and the .mtv library, saved media references\n• the diagnostic "
                  "log, temporary files and preview thumbnails\n\nMotiva's wallpaper is released first. Kept: your "
                  "settings, theme, media backups and the Motiva installation.\n\n%1\n\nThis action cannot be undone.")
                   .arg(mediaSafe);
        action = tr("Delete Cache + Data");
        break;
    case Op::FactoryReset:
        title = tr("Factory Reset");
        text = tr("Factory Reset Motiva?");
        info = tr("This will reset Motiva to its default state and remove your Motiva settings, playlists, "
                  "library, recovery state, and cached data. Start with Windows, the Start Menu shortcut and the "
                  "Explorer menu entries are turned off, and Backup is turned off. Motiva then restarts.\n\n"
                  "Media backups already on disk are NOT deleted by Factory Reset - remove them first with "
                  "Delete Media Backups if you want them gone.\n\n%1\nMotiva itself stays installed.\n\n"
                  "This action cannot be undone.")
                   .arg(mediaSafe);
        action = tr("Factory Reset");
        icon = QMessageBox::Critical;
        break;
    case Op::DeleteCloudCache:
        title = tr("Delete Downloaded Media");
        text = tr("Delete the pictures and videos Motiva saved from the web and cloud storage?");
        info = tr("Removed: the local copies Motiva saved through the browser extension or downloaded from cloud "
                  "storage. Websites and your cloud are not touched. Playlist items that use these copies will show "
                  "as missing until you save them again.\n\nThis action cannot be undone.");
        action = tr("Delete Downloaded Media");
        break;
    case Op::DeleteBackups:
        title = tr("Delete Media Backups");
        text = tr("Delete all of Motiva's media backups?");
        info = tr("Removed: every backup copy Motiva made (and the records of them), and Backup is turned off so "
                  "nothing is copied again until you turn it back on.\n\nYour original files are not touched - "
                  "only Motiva's own copies are deleted. If an original is later lost, it can no longer be "
                  "restored from backup.\n\nThis action cannot be undone.");
        action = tr("Delete Media Backups");
        icon = QMessageBox::Critical;
        break;
    case Op::DeleteUnusedBackups:
        title = tr("Delete Unused Backups");
        text = tr("Delete the backups of media that is no longer in any playlist?");
        info = tr("Removed: backup copies whose media is no longer used by any playlist. Backups of media that is "
                  "still in a playlist are kept.\n\nYour original files are not touched.\n\nThis action cannot be "
                  "undone.");
        action = tr("Delete Unused Backups");
        break;
    case Op::ResetRecoveryHistory:
        title = tr("Reset Recovery History");
        text = tr("Reset Motiva's recovery history?");
        info = tr("Clears the Explorer-restart counter, the last recorded failure and the saved startup "
                  "diagnostics. The current wallpaper keeps running. Nothing else is changed.");
        action = tr("Reset");
        icon = QMessageBox::Question;
        break;
    case Op::ClearLog:
        title = tr("Clear Diagnostic Log");
        text = tr("Clear Motiva's diagnostic log?");
        info = tr("Empties %1. Nothing else is changed.").arg(QDir::toNativeSeparators(CleanupManager::logFilePath()));
        action = tr("Clear Log");
        icon = QMessageBox::Question;
        break;
    case Op::ResetWindowsIntegration:
        title = tr("Reset Windows Integration");
        text = tr("Turn off all of Motiva's Windows integration?");
        info = tr("Removes Start with Windows, the Start Menu (Windows Search) shortcut and the Explorer "
                  "right-click entries. You can turn each back on in Settings.");
        action = tr("Turn Off");
        break;
    case Op::ResetPreferences:
        title = tr("Reset Preferences");
        text = tr("Reset Motiva's preferences to their defaults?");
        info = tr("Theme, scaling, monitor, volume, mute, loop, battery and Windows integration settings go back to "
                  "their defaults. Playlists, the library and the current media are kept.");
        action = tr("Reset Preferences");
        break;
    case Op::RemoveLegacyData:
        title = tr("Remove Old Version Data");
        text = tr("Remove data left by earlier Motiva versions?");
        info = tr("Deletes the recovery state an earlier version stored under its old name (VideoWallpaper). "
                  "Nothing the current Motiva uses is affected.");
        action = tr("Remove");
        icon = QMessageBox::Question;
        break;
    }

    GlMessageBox box(icon, title, text, QMessageBox::NoButton, this);
    box.setInformativeText(info);
    auto* cancel = box.addButton(QMessageBox::Cancel);
    auto* proceed = box.addButton(action, QMessageBox::DestructiveRole);
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    return box.clickedButton() == proceed;
}

void CleanupWindow::runWithProgress(Op op) {
    if (m_manager->isBusy() || m_running) {
        return;
    }
    m_stepLabels = m_manager->stepLabels(op);
    m_stepCount = int(m_stepLabels.size());
    GlCanvas* c = canvas();
    while (m_stepRows.size() < m_stepCount) {
        auto* icon = new GlLabel(c);
        auto* row = new GlLabel(c);
        row->setWordWrap(true);
        row->setPointSize(10.5);
        row->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        auto* detail = new GlLabel(c);
        detail->setWordWrap(true);
        detail->setPointSize(9);
        detail->setTone(GlLabel::Tone::Dim);
        detail->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_stepIcons << icon;
        m_stepRows << row;
        m_stepDetails << detail;
    }
    for (int i = 0; i < m_stepCount; ++i) {
        m_stepDetails[i]->setText(QString());
        setStepRow(i, StepMark::Pending, Theme::kStatusNeutral, QString());
    }
    m_progressTitle->setText(op == Op::FactoryReset ? tr("Resetting Motiva…") : tr("Cleaning Motiva…"));
    m_progressStatus->clear();
    m_progressButton->setText(op == Op::FactoryReset ? tr("Restart Motiva") : tr("Back to Cleanup"));
    m_progressButton->setEnabled(false);
    m_closeButton->setEnabled(false);
    showPage(1);

    m_running = true;
    m_runningOp = op;
    qApp->installEventFilter(this);
    m_manager->run(op);
}

void CleanupWindow::setStepRow(int index, StepMark mark, const char* color, const QString& detail) {
    if (index < 0 || index >= m_stepRows.size()) {
        return;
    }
    const char* resource = ":/status/pending.svg";
    QString markName = tr("Pending");
    switch (mark) {
    case StepMark::Pending: break;
    case StepMark::Running: resource = ":/status/arrow-right.svg"; markName = tr("In progress"); break;
    case StepMark::Succeeded: resource = ":/status/check.svg"; markName = tr("Done"); break;
    case StepMark::Failed: resource = ":/status/cross.svg"; markName = tr("Failed"); break;
    }
    const GlUi::Style st = GlUi::Style::current();
    const QColor markColor = color ? QColor(QLatin1String(color)) : st.text;
    m_stepIcons[index]->setPixmap(markPixmap(resource, markColor));
    m_stepRows[index]->setText(m_stepLabels[index]);
    if (color) {
        m_stepRows[index]->setColor(QColor(QLatin1String(color)));
    } else {
        m_stepRows[index]->setTone(GlLabel::Tone::Normal);
    }
    m_stepDetails[index]->setText(detail);
    m_stepDetails[index]->setVisible(m_page == 1 && index < m_stepCount && !detail.isEmpty());
    m_stepRows[index]->setAccessibleName(detail.isEmpty() ? QStringLiteral("%1 %2").arg(markName, m_stepLabels[index])
                                                          : QStringLiteral("%1 %2: %3").arg(markName, m_stepLabels[index], detail));
    canvas()->relayout();
}

void CleanupWindow::onOperationFinished(Op op, bool allOk) {
    if (!m_running) {
        return;
    }
    m_running = false;
    qApp->removeEventFilter(this);
    if (allOk) {
        m_progressStatus->setText(op == Op::FactoryReset
                                      ? tr("Cleanup complete. Motiva will now restart with its default settings.")
                                      : tr("Cleanup complete."));
        m_progressStatus->setColor(QColor(QLatin1String(Theme::kStatusSuccess)));
    } else {
        m_progressStatus->setText(op == Op::FactoryReset
                                      ? tr("Some items could not be removed (see above). Motiva will restart so "
                                           "everything else starts clean; run Factory Reset again after closing "
                                           "whatever is holding those files.")
                                      : tr("Some items could not be removed (see above). Everything else was "
                                           "cleaned. Close whatever is using those files and try again."));
        m_progressStatus->setColor(QColor(QLatin1String(Theme::kStatusError)));
    }
    m_progressButton->setEnabled(true);
    canvas()->setFocusedItem(m_progressButton);
    // After a Factory Reset the footer Close (like closing the window)
    // restarts Motiva - see closeEvent().
    m_restartPending = op == Op::FactoryReset;
    m_closeButton->setEnabled(true);
    canvas()->relayout();
}

void CleanupWindow::onProgressButton() {
    if (m_runningOp == Op::FactoryReset) {
        // The in-memory state no longer matches disk - restart regardless.
        m_manager->requestRestart();
        return;
    }
    showPage(0);
    m_manager->refreshSizes();
}

namespace {
// "More cleanup options": one row per operation that has something to do.
class MoreDialog : public GlWindow {
public:
    struct Row {
        GlLabel* title;
        GlLabel* desc;
        GlLabel* status;
        GlButton* button;
    };
    MoreDialog(QWidget* parent) : GlWindow(QObject::tr("More Cleanup Options"), QObject::tr("Individual operations"), parent) {
        setAttribute(Qt::WA_DeleteOnClose);
        m_close = new GlButton(canvas(), GlButton::Kind::Secondary);
        m_close->setText(QObject::tr("Close"));
        connect(m_close, &GlButton::clicked, this, &QDialog::close);
        resize(760, 520);
        setMinimumSize(640, 360);
    }
    Row& addRow(const QString& title, const QString& description, const QString& status, const QString& button) {
        GlCanvas* c = canvas();
        Row r;
        r.title = new GlLabel(c);
        r.title->setText(title);
        r.title->setBold(true);
        r.title->setPointSize(10.5);
        r.desc = new GlLabel(c);
        r.desc->setText(description);
        r.desc->setWordWrap(true);
        r.desc->setPointSize(9.5);
        r.desc->setTone(GlLabel::Tone::Dim);
        r.desc->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        r.status = new GlLabel(c);
        r.status->setText(status);
        r.status->setPointSize(9.5);
        r.status->setVisible(!status.isEmpty());
        r.button = new GlButton(c, GlButton::Kind::Secondary);
        r.button->setText(button);
        m_rows.push_back(r);
        return m_rows.last();
    }
    void layoutContent(const QRectF& content, const QRectF& footer) override {
        qreal y = content.top();
        const qreal btnW = 150;
        for (Row& r : m_rows) {
            const qreal tw = content.width() - btnW - 24;
            const qreal dh = std::max<qreal>(18, r.desc->heightForWidth(tw) + 2);
            const qreal sh = r.status->isVisible() ? 22 : 0;
            r.title->setRect(QRectF(content.left(), y, tw, 24));
            r.desc->setRect(QRectF(content.left(), y + 24, tw, dh));
            r.status->setRect(QRectF(content.left(), y + 24 + dh, tw, sh));
            const qreal h = 24 + dh + sh + 12;
            r.button->setRect(QRectF(content.right() - btnW, y + h / 2 - 22, btnW, 40));
            y += h + 10;
        }
        layoutFooter({m_close}, footer, 120);
    }

private:
    QList<Row> m_rows;
    GlButton* m_close = nullptr;
};
} // namespace

void CleanupWindow::showMoreDialog() {
    auto* dialog = new MoreDialog(this);
    // Only operations whose data/state actually exists right now.
    auto addRow = [&](const QString& title, const QString& description, const QString& status,
                      const QString& buttonText, Op op) {
        MoreDialog::Row& r = dialog->addRow(title, description, status, buttonText);
        QObject::connect(r.button, &GlButton::clicked, dialog, [this, dialog, op] {
            if (confirm(op)) {
                dialog->close();
                runWithProgress(op);
            }
        });
    };

    if (m_sizes.recoveryPresent) {
        addRow(tr("Reset recovery history"),
               tr("Explorer-restart counter, last recorded failure and startup diagnostics."),
               sizeText(m_sizes.recoveryBytes), tr("Reset"), Op::ResetRecoveryHistory);
    }
    if (m_sizes.logPresent) {
        addRow(tr("Clear diagnostic log"), QDir::toNativeSeparators(CleanupManager::logFilePath()),
               sizeText(m_sizes.logBytes), tr("Clear"), Op::ClearLog);
    }
    if (m_manager->hasWindowsIntegration()) {
        addRow(tr("Reset Windows integration"),
               tr("Start with Windows, Start Menu shortcut and Explorer right-click entries."), QString(),
               tr("Turn Off"), Op::ResetWindowsIntegration);
    }
    addRow(tr("Reset preferences"), tr("Theme and every other setting back to defaults; playlists are kept."),
           QString(), tr("Reset"), Op::ResetPreferences);
    if (m_sizes.unusedBackups > 0) {
        addRow(tr("Delete unused backups"),
               tr("Backup copies of media that is no longer in any playlist (%1).").arg(m_sizes.unusedBackups), QString(),
               tr("Delete"), Op::DeleteUnusedBackups);
    }
    if (m_sizes.legacyPresent) {
        addRow(tr("Remove old version data"), tr("Recovery state left by Motiva builds from before the rename."),
               sizeText(m_sizes.legacyBytes), tr("Remove"), Op::RemoveLegacyData);
    }
    dialog->show();
}
