#include "CleanupWindow.h"

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

bool isOnyx(Theme::AppTheme theme) {
    return theme == Theme::AppTheme::DarkOnyx || theme == Theme::AppTheme::LightOnyx;
}

// Thin separator in the theme's border color (Onyx's headerRule is a heavy
// accent bar - right under a title, too loud between rows).
QFrame* makeRule(QWidget* parent, const char* name = "cleanupRule") {
    auto* rule = new QFrame(parent);
    rule->setObjectName(QLatin1String(name));
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    return rule;
}

QLabel* makeHeading(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("cleanupHeading"));
    return label;
}

} // namespace

CleanupWindow::CleanupWindow(CleanupManager* manager, QWidget* parent) : QDialog(parent), m_manager(manager) {
    setWindowTitle(tr("Cleanup & Reset"));
    // A utility window, not a modal step of Settings: closing it just
    // returns to Settings, and Motiva keeps running.
    setModal(false);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumSize(560, 540);
    resize(640, 590);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header
    auto* header = new QWidget(this);
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(24, 20, 24, 14);
    headerLayout->setSpacing(4);
    auto* title = new QLabel(tr("Cleanup & Reset"), header);
    title->setObjectName(QStringLiteral("appTitle"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 3);
    title->setFont(titleFont);
    headerLayout->addWidget(title);
    auto* subtitle = new QLabel(tr("Manage Motiva's cached files, application data, libraries, and reset options."),
                                header);
    subtitle->setObjectName(QStringLiteral("secondaryText"));
    subtitle->setWordWrap(true);
    headerLayout->addWidget(subtitle);
    root->addWidget(header);
    root->addWidget(makeRule(this, "headerRule"));

    // Body: the levels, or the progress of a running operation.
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildLevelsPage());
    m_pages->addWidget(buildProgressPage());
    root->addWidget(m_pages, 1);

    // Footer
    root->addWidget(makeRule(this));
    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(24, 12, 24, 14);
    auto* safeNote = new QLabel(tr("Motiva-owned data only · Original media files are safe"), this);
    safeNote->setObjectName(QStringLiteral("secondaryText"));
    footer->addWidget(safeNote, 1);
    m_closeButton = new QPushButton(tr("Close"), this);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::close);
    footer->addWidget(m_closeButton);
    root->addLayout(footer);

    connect(m_manager, &CleanupManager::sizesReady, this, &CleanupWindow::onSizesReady);
    connect(m_manager, &CleanupManager::stepStarted, this, [this](int i) {
        if (m_running) {
            setStepRow(i, QStringLiteral("→"), nullptr, QString());
        }
    });
    connect(m_manager, &CleanupManager::stepFinished, this, [this](int i, bool ok, const QString& detail) {
        if (m_running) {
            setStepRow(i, ok ? QStringLiteral("✓") : QStringLiteral("✕"),
                       ok ? Theme::kStatusSuccess : Theme::kStatusError, detail);
        }
    });
    connect(m_manager, &CleanupManager::finished, this, &CleanupWindow::onOperationFinished);
    applyThemeStyle();
}

QWidget* CleanupWindow::buildLevelsPage() {
    auto* page = new QWidget(this);
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(24, 16, 24, 16);
    col->setSpacing(0);

    col->addWidget(makeLevelRow(Theme::kStatusSuccess, tr("CACHE"),
                                tr("Temporary files, the diagnostic log and preview thumbnails."), &m_cacheSize,
                                tr("Delete Cache"), Op::DeleteCache));
    col->addWidget(makeRule(page));
    col->addWidget(makeLevelRow(kDotYellow, tr("DATA"),
                                tr("Playlists, the .mtv library and saved media references."), &m_dataSize,
                                tr("Delete Data"), Op::DeleteData));
    col->addWidget(makeRule(page));
    col->addWidget(makeLevelRow(kDotOrange, tr("CACHE + DATA"),
                                tr("Removes both cache and application data. Settings and theme are kept."),
                                &m_combinedSize, tr("Delete Cache + Data"), Op::DeleteCacheAndData));

    col->addSpacing(14);
    col->addWidget(makeRule(page));
    col->addSpacing(12);

    // Advanced
    auto* advanced = new QHBoxLayout();
    auto* advancedText = new QVBoxLayout();
    advancedText->setSpacing(3);
    advancedText->addWidget(makeHeading(tr("ADVANCED"), page));
    auto* advancedDesc = new QLabel(tr("Individual cleanup operations for data Motiva currently has."), page);
    advancedDesc->setObjectName(QStringLiteral("secondaryText"));
    advancedDesc->setWordWrap(true);
    advancedText->addWidget(advancedDesc);
    advanced->addLayout(advancedText, 1);
    auto* more = new QPushButton(tr("More…"), page);
    connect(more, &QPushButton::clicked, this, &CleanupWindow::showMoreDialog);
    advanced->addWidget(more, 0, Qt::AlignVCenter);
    col->addLayout(advanced);

    col->addSpacing(16);

    // Danger zone
    auto* danger = new QFrame(page);
    danger->setObjectName(QStringLiteral("cleanupDangerCard"));
    auto* dangerLayout = new QHBoxLayout(danger);
    dangerLayout->setContentsMargins(14, 12, 14, 12);
    dangerLayout->setSpacing(12);
    auto* dangerText = new QVBoxLayout();
    dangerText->setSpacing(3);
    auto* dangerTitle = new QLabel(QStringLiteral("<span style=\"color:%1\">●</span>&nbsp; %2")
                                       .arg(QLatin1String(Theme::kStatusError), tr("DANGER ZONE")),
                                   danger);
    dangerTitle->setObjectName(QStringLiteral("cleanupDangerHeading"));
    dangerTitle->setAccessibleName(tr("DANGER ZONE"));
    dangerText->addWidget(dangerTitle);
    auto* dangerDesc = new QLabel(tr("Return Motiva to a fresh-install state: settings, theme, playlists, library, "
                                     "recovery state, cache and Windows integration. Motiva restarts afterwards."),
                                  danger);
    dangerDesc->setObjectName(QStringLiteral("secondaryText"));
    dangerDesc->setWordWrap(true);
    dangerText->addWidget(dangerDesc);
    dangerLayout->addLayout(dangerText, 1);
    auto* factory = new QPushButton(tr("Factory Reset"), danger);
    factory->setObjectName(QStringLiteral("factoryResetButton"));
    connect(factory, &QPushButton::clicked, this, [this] {
        if (confirm(Op::FactoryReset)) {
            runWithProgress(Op::FactoryReset);
        }
    });
    dangerLayout->addWidget(factory, 0, Qt::AlignVCenter);
    col->addWidget(danger);

    col->addStretch();
    return page;
}

QWidget* CleanupWindow::makeLevelRow(const char* dotColor, const QString& title, const QString& description,
                                     QLabel** sizeLabel, const QString& buttonText, Op op) {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 12, 0, 12);
    layout->setSpacing(16);

    auto* text = new QVBoxLayout();
    text->setSpacing(3);
    auto* heading = makeHeading(
        QStringLiteral("<span style=\"color:%1\">●</span>&nbsp; %2").arg(QLatin1String(dotColor), title), row);
    heading->setAccessibleName(title);
    text->addWidget(heading);
    auto* desc = new QLabel(description, row);
    desc->setObjectName(QStringLiteral("secondaryText"));
    desc->setWordWrap(true);
    text->addWidget(desc);
    *sizeLabel = new QLabel(tr("Size: calculating…"), row);
    text->addWidget(*sizeLabel);
    layout->addLayout(text, 1);

    auto* button = new QPushButton(buttonText, row);
    // The safe level keeps the normal button look; anything that removes
    // user data gets the destructive accent.
    if (op != Op::DeleteCache) {
        button->setObjectName(QStringLiteral("dangerButton"));
    }
    button->setMinimumWidth(170);
    connect(button, &QPushButton::clicked, this, [this, op] {
        if (confirm(op)) {
            runWithProgress(op);
        }
    });
    layout->addWidget(button, 0, Qt::AlignVCenter);
    return row;
}

QWidget* CleanupWindow::buildProgressPage() {
    auto* page = new QWidget(this);
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(24, 18, 24, 16);
    col->setSpacing(8);
    m_progressTitle = new QLabel(tr("Cleaning Motiva…"), page);
    m_progressTitle->setObjectName(QStringLiteral("sectionLabel"));
    QFont f = m_progressTitle->font();
    f.setBold(true);
    m_progressTitle->setFont(f);
    col->addWidget(m_progressTitle);
    m_stepsLayout = new QVBoxLayout();
    m_stepsLayout->setSpacing(8);
    col->addLayout(m_stepsLayout);
    col->addSpacing(6);
    m_progressStatus = new QLabel(page);
    m_progressStatus->setWordWrap(true);
    col->addWidget(m_progressStatus);
    col->addStretch();
    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    m_progressButton = new QPushButton(page);
    m_progressButton->setObjectName(QStringLiteral("primaryButton"));
    connect(m_progressButton, &QPushButton::clicked, this, &CleanupWindow::onProgressButton);
    buttons->addWidget(m_progressButton);
    col->addLayout(buttons);
    return page;
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
    QDialog::showEvent(event);
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

void CleanupWindow::applyThemeStyle() {
    const Theme::AppTheme theme = Theme::currentTheme();
    m_styledTheme = theme;
    const Theme::ThemePalette& p = Theme::themePalette(theme);
    const int radius = isOnyx(theme) ? 2 : 8;
    const QString error = QLatin1String(Theme::kStatusError);
    // Only headings, the danger zone and the destructive-button accent are
    // local; everything else (fonts, base button look, hover/pressed/focus,
    // rules, secondary text) comes from the app-wide theme stylesheet.
    setStyleSheet(QStringLiteral(R"(
QFrame#cleanupRule { background: %2; border: none; max-height: 1px; min-height: 1px; }
QLabel#cleanupHeading { font-weight: 600; letter-spacing: 0.5px; }
QLabel#cleanupDangerHeading { color: %4; font-weight: 600; letter-spacing: 0.5px; }
QFrame#cleanupDangerCard { background: %1; border: 1px solid %4; border-radius: %3px; }
QPushButton#dangerButton, QPushButton#factoryResetButton { color: %4; border: 1px solid %4; }
QPushButton#dangerButton:hover, QPushButton#factoryResetButton:hover { background: %4; color: #ffffff; }
QPushButton#dangerButton:pressed, QPushButton#factoryResetButton:pressed { background: %4; color: #ffffff; }
QPushButton#dangerButton:disabled, QPushButton#factoryResetButton:disabled { color: %5; border-color: %2; background: transparent; }
)")
                      .arg(p.panelBg.name(QColor::HexArgb), p.border.name(QColor::HexArgb))
                      .arg(radius)
                      .arg(error, p.textDisabled.name(QColor::HexArgb)));
}

void CleanupWindow::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    // setStyleSheet() itself raises PaletteChange - only restyle when the
    // Motiva theme really changed, or this would recurse forever.
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::ThemeChange) &&
        Theme::currentTheme() != m_styledTheme) {
        applyThemeStyle();
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
                  "Kept: your settings, theme and cache.\n\n%1\n\nTo keep a copy, use Playlists → Export… first. "
                  "This action cannot be undone.")
                   .arg(mediaSafe);
        action = tr("Delete Data");
        break;
    case Op::DeleteCacheAndData:
        title = tr("Delete Cache + Data");
        text = tr("Delete Motiva's cache and all playlist/library data?");
        info = tr("Removed:\n• every playlist and the .mtv library, saved media references\n• the diagnostic "
                  "log, temporary files and preview thumbnails\n\nMotiva's wallpaper is released first. Kept: your "
                  "settings, theme and the Motiva installation.\n\n%1\n\nThis action cannot be undone.")
                   .arg(mediaSafe);
        action = tr("Delete Cache + Data");
        break;
    case Op::FactoryReset:
        title = tr("Factory Reset");
        text = tr("Factory Reset Motiva?");
        info = tr("This will reset Motiva to its default state and remove your Motiva settings, playlists, "
                  "library, recovery state, and cached data. Start with Windows, the Start Menu shortcut and the "
                  "Explorer menu entries are turned off. Motiva then restarts.\n\n%1\nMotiva itself stays "
                  "installed.\n\nThis action cannot be undone.")
                   .arg(mediaSafe);
        action = tr("Factory Reset");
        icon = QMessageBox::Critical;
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

    QMessageBox box(icon, title, text, QMessageBox::NoButton, this);
    box.setInformativeText(info);
    QPushButton* cancel = box.addButton(QMessageBox::Cancel);
    QPushButton* proceed = box.addButton(action, QMessageBox::DestructiveRole);
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    return box.clickedButton() == proceed;
}

void CleanupWindow::runWithProgress(Op op) {
    if (m_manager->isBusy() || m_running) {
        return;
    }
    // Fresh step list for this operation.
    for (QLabel* row : std::as_const(m_stepRows)) {
        row->deleteLater();
    }
    m_stepRows.clear();
    m_stepLabels = m_manager->stepLabels(op);
    for (int i = 0; i < m_stepLabels.size(); ++i) {
        auto* row = new QLabel(m_pages->widget(1));
        row->setWordWrap(true);
        // Auto-detection misses rich text that doesn't start with a tag.
        row->setTextFormat(Qt::RichText);
        m_stepsLayout->addWidget(row);
        m_stepRows << row;
        setStepRow(i, QStringLiteral("○"), Theme::kStatusNeutral, QString());
    }
    m_progressTitle->setText(op == Op::FactoryReset ? tr("Resetting Motiva…") : tr("Cleaning Motiva…"));
    m_progressStatus->clear();
    m_progressStatus->setStyleSheet(QString());
    m_progressButton->setText(op == Op::FactoryReset ? tr("Restart Motiva") : tr("Back to Cleanup"));
    m_progressButton->setEnabled(false);
    m_closeButton->setEnabled(false);
    m_pages->setCurrentIndex(1);

    m_running = true;
    m_runningOp = op;
    qApp->installEventFilter(this);
    m_manager->run(op);
}

void CleanupWindow::setStepRow(int index, const QString& marker, const char* color, const QString& detail) {
    if (index < 0 || index >= m_stepRows.size()) {
        return;
    }
    QString text = QStringLiteral("%1&nbsp;&nbsp;%2").arg(marker, m_stepLabels[index].toHtmlEscaped());
    if (!detail.isEmpty()) {
        text += QStringLiteral("<br><span style=\"font-size:small\">%1</span>")
                    .arg(detail.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    }
    m_stepRows[index]->setText(text);
    m_stepRows[index]->setAccessibleName(detail.isEmpty() ? QStringLiteral("%1 %2").arg(marker, m_stepLabels[index])
                                                          : QStringLiteral("%1 %2: %3").arg(marker, m_stepLabels[index], detail));
    m_stepRows[index]->setStyleSheet(color ? QStringLiteral("color: %1;").arg(QLatin1String(color)) : QString());
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
        m_progressStatus->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::kStatusSuccess)));
    } else {
        m_progressStatus->setText(op == Op::FactoryReset
                                      ? tr("Some items could not be removed (see above). Motiva will restart so "
                                           "everything else starts clean; run Factory Reset again after closing "
                                           "whatever is holding those files.")
                                      : tr("Some items could not be removed (see above). Everything else was "
                                           "cleaned. Close whatever is using those files and try again."));
        m_progressStatus->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::kStatusError)));
    }
    m_progressButton->setEnabled(true);
    m_progressButton->setFocus();
    // After a Factory Reset the footer Close (like closing the window)
    // restarts Motiva - see closeEvent().
    m_restartPending = op == Op::FactoryReset;
    m_closeButton->setEnabled(true);
}

void CleanupWindow::onProgressButton() {
    if (m_runningOp == Op::FactoryReset) {
        // The in-memory state no longer matches disk - restart regardless.
        m_manager->requestRestart();
        return;
    }
    m_pages->setCurrentIndex(0);
    m_manager->refreshSizes();
}

void CleanupWindow::showMoreDialog() {
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("More Cleanup Options"));
    dialog->setMinimumWidth(520);
    auto* root = new QVBoxLayout(dialog);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(10);

    auto* heading = new QLabel(tr("More cleanup options"), dialog);
    heading->setObjectName(QStringLiteral("sectionLabel"));
    QFont f = heading->font();
    f.setBold(true);
    heading->setFont(f);
    root->addWidget(heading);

    // Only operations whose data/state actually exists right now.
    auto addRow = [&](const QString& title, const QString& description, const QString& status,
                      const QString& buttonText, Op op) {
        auto* row = new QHBoxLayout();
        auto* text = new QVBoxLayout();
        text->setSpacing(2);
        auto* t = new QLabel(title, dialog);
        QFont bf = t->font();
        bf.setBold(true);
        t->setFont(bf);
        text->addWidget(t);
        auto* d = new QLabel(description, dialog);
        d->setObjectName(QStringLiteral("secondaryText"));
        d->setWordWrap(true);
        text->addWidget(d);
        if (!status.isEmpty()) {
            text->addWidget(new QLabel(status, dialog));
        }
        row->addLayout(text, 1);
        auto* button = new QPushButton(buttonText, dialog);
        connect(button, &QPushButton::clicked, dialog, [this, dialog, op] {
            if (confirm(op)) {
                dialog->close();
                runWithProgress(op);
            }
        });
        row->addWidget(button, 0, Qt::AlignVCenter);
        root->addLayout(row);
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
    if (m_sizes.legacyPresent) {
        addRow(tr("Remove old version data"), tr("Recovery state left by Motiva builds from before the rename."),
               sizeText(m_sizes.legacyBytes), tr("Remove"), Op::RemoveLegacyData);
    }

    root->addStretch();
    auto* close = new QPushButton(tr("Close"), dialog);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    auto* footer = new QHBoxLayout();
    footer->addStretch();
    footer->addWidget(close);
    root->addLayout(footer);
    dialog->show();
}
