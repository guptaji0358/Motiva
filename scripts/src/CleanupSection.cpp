#include "CleanupSection.h"
#include "Theme.h"

#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

using Op = CleanupManager::Operation;

namespace {

bool isOnyx(Theme::AppTheme theme) {
    return theme == Theme::AppTheme::DarkOnyx || theme == Theme::AppTheme::LightOnyx;
}

// "Cleaning Motiva..." - lists every step up front, then marks each one as
// it runs. Application-modal: nothing else in Motiva may touch the library
// or settings while they are being removed. Cannot be dismissed mid-run.
class CleanupProgressDialog : public QDialog {
public:
    CleanupProgressDialog(CleanupManager* manager, Op op, QWidget* parent)
        : QDialog(parent), m_manager(manager), m_op(op) {
        setWindowTitle(op == Op::FactoryReset ? tr("Factory Reset") : tr("Cleanup & Reset"));
        setWindowModality(Qt::ApplicationModal);
        setAttribute(Qt::WA_DeleteOnClose);
        setMinimumWidth(440);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(20, 18, 20, 18);
        root->setSpacing(8);
        auto* title = new QLabel(tr("Cleaning Motiva…"), this);
        title->setObjectName(QStringLiteral("sectionLabel"));
        QFont f = title->font();
        f.setBold(true);
        title->setFont(f);
        root->addWidget(title);

        for (const QString& label : manager->stepLabels(op)) {
            auto* row = new QLabel(this);
            row->setWordWrap(true);
            m_rows << row;
            m_labels << label;
            setRow(m_rows.size() - 1, QStringLiteral("○"), Theme::kStatusNeutral, QString());
            root->addWidget(row);
        }

        m_status = new QLabel(this);
        m_status->setWordWrap(true);
        root->addSpacing(6);
        root->addWidget(m_status);

        auto* buttons = new QHBoxLayout();
        buttons->addStretch();
        m_button = new QPushButton(op == Op::FactoryReset ? tr("Restart Motiva") : tr("Close"), this);
        m_button->setObjectName(QStringLiteral("primaryButton"));
        m_button->setEnabled(false);
        connect(m_button, &QPushButton::clicked, this, &QDialog::accept);
        buttons->addWidget(m_button);
        root->addLayout(buttons);

        connect(manager, &CleanupManager::stepStarted, this, [this](int i) {
            setRow(i, QStringLiteral("→"), nullptr, QString());
        });
        connect(manager, &CleanupManager::stepFinished, this, [this](int i, bool ok, const QString& detail) {
            setRow(i, ok ? QStringLiteral("✓") : QStringLiteral("✕"), ok ? Theme::kStatusSuccess : Theme::kStatusError,
                   detail);
        });
        connect(manager, &CleanupManager::finished, this, [this](Op, bool allOk) {
            m_done = true;
            if (allOk) {
                m_status->setText(m_op == Op::FactoryReset
                                      ? tr("Cleanup complete. Motiva will now restart with its default settings.")
                                      : tr("Cleanup complete."));
                m_status->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::kStatusSuccess)));
            } else {
                m_status->setText(m_op == Op::FactoryReset
                                      ? tr("Some items could not be removed (see above). Motiva will restart so "
                                           "everything else starts clean; run Factory Reset again after closing "
                                           "whatever is holding those files.")
                                      : tr("Some items could not be removed (see above). Everything else was "
                                           "cleaned. Close whatever is using those files and try again."));
                m_status->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::kStatusError)));
            }
            m_button->setEnabled(true);
            m_button->setFocus();
        });
    }

    void done(int result) override {
        if (!m_done) {
            return; // never dismissable mid-run
        }
        QDialog::done(result);
        if (m_op == Op::FactoryReset) {
            // The in-memory state no longer matches disk either way -
            // restart regardless of how the dialog was closed.
            m_manager->requestRestart();
        }
    }

private:
    void setRow(int i, const QString& marker, const char* color, const QString& detail) {
        if (i < 0 || i >= m_rows.size()) {
            return;
        }
        QString text = QStringLiteral("%1  %2").arg(marker, m_labels[i].toHtmlEscaped());
        if (!detail.isEmpty()) {
            text += QStringLiteral("<br><span style=\"font-size:small\">%1</span>")
                        .arg(detail.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
        }
        m_rows[i]->setText(text);
        m_rows[i]->setStyleSheet(color ? QStringLiteral("color: %1;").arg(QLatin1String(color)) : QString());
    }

    CleanupManager* m_manager;
    Op m_op;
    bool m_done = false;
    QList<QLabel*> m_rows;
    QStringList m_labels;
    QLabel* m_status = nullptr;
    QPushButton* m_button = nullptr;
};

} // namespace

CleanupSection::CleanupSection(CleanupManager* manager, QWidget* parent) : QWidget(parent), m_manager(manager) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto* heading = new QLabel(tr("Cleanup & Reset"), this);
    heading->setObjectName(QStringLiteral("sectionLabel"));
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto* intro = new QLabel(tr("Manage Motiva's cached files, application data, libraries, and reset state. "
                                "Your own video and image files are never deleted."),
                             this);
    intro->setObjectName(QStringLiteral("secondaryText"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* levels = new QHBoxLayout();
    levels->setSpacing(10);
    levels->addWidget(makeCard(tr("SAFE"), tr("Delete Cache"), tr("Temporary files, the diagnostic log and preview "
                                                                  "thumbnails."),
                               &m_cacheSize, tr("Delete Cache"), Op::DeleteCache));
    levels->addWidget(makeCard(tr("DATA"), tr("Delete Data"), tr("Playlists, the .mtv library and saved media "
                                                                 "references."),
                               &m_dataSize, tr("Delete Data"), Op::DeleteData));
    levels->addWidget(makeCard(tr("COMBINED"), tr("Delete Cache + Data"), tr("Removes both categories. Settings "
                                                                             "and theme are kept."),
                               &m_combinedSize, tr("Delete Cache + Data"), Op::DeleteCacheAndData));
    root->addLayout(levels);

    auto* advanced = new QHBoxLayout();
    auto* advancedLabel = new QLabel(tr("Advanced"), this);
    advancedLabel->setObjectName(QStringLiteral("secondaryText"));
    advanced->addWidget(advancedLabel);
    auto* more = new QPushButton(tr("More…"), this);
    more->setToolTip(tr("Individual cleanup operations"));
    connect(more, &QPushButton::clicked, this, &CleanupSection::showMoreDialog);
    advanced->addWidget(more);
    advanced->addStretch();
    root->addLayout(advanced);

    root->addWidget(makeCard(tr("DANGER ZONE"), tr("Factory Reset"),
                             tr("Return Motiva to a fresh-install state: settings, theme, playlists, library, "
                                "recovery state, cache and Windows integration. Motiva restarts afterwards."),
                             nullptr, tr("Factory Reset"), Op::FactoryReset, true));

    connect(m_manager, &CleanupManager::sizesReady, this, &CleanupSection::onSizesReady);
    connect(m_manager, &CleanupManager::finished, this, [this] { m_manager->refreshSizes(); });
    applyThemeStyle();
}

QFrame* CleanupSection::makeCard(const QString& tier, const QString& title, const QString& description,
                                 QLabel** sizeLabel, const QString& buttonText, Op op, bool danger) {
    auto* card = new QFrame(this);
    card->setObjectName(danger ? QStringLiteral("cleanupDangerCard") : QStringLiteral("cleanupCard"));
    auto* body = new QHBoxLayout(card);
    body->setContentsMargins(12, 10, 12, 10);
    body->setSpacing(12);

    auto* text = new QVBoxLayout();
    text->setSpacing(3);
    auto* tierLabel = new QLabel(tier, card);
    tierLabel->setObjectName(danger ? QStringLiteral("cleanupDangerTier") : QStringLiteral("cleanupTier"));
    text->addWidget(tierLabel);
    auto* titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("cleanupTitle"));
    text->addWidget(titleLabel);
    auto* desc = new QLabel(description, card);
    desc->setObjectName(QStringLiteral("secondaryText"));
    desc->setWordWrap(true);
    text->addWidget(desc);
    if (sizeLabel) {
        *sizeLabel = new QLabel(tr("Calculating…"), card);
        text->addWidget(*sizeLabel);
    }
    text->addStretch();

    auto* button = new QPushButton(buttonText, card);
    // The safe level keeps the normal button look; anything that removes
    // user data gets the destructive accent.
    if (op != Op::DeleteCache) {
        button->setObjectName(QStringLiteral("dangerButton"));
    }
    connect(button, &QPushButton::clicked, this, [this, op] {
        if (confirm(op)) {
            runWithProgress(op);
        }
    });

    if (danger) {
        body->addLayout(text, 1);
        body->addWidget(button, 0, Qt::AlignVCenter);
    } else {
        // Narrow cards: button under the text, right-aligned.
        auto* column = new QVBoxLayout();
        column->addLayout(text, 1);
        column->addWidget(button, 0, Qt::AlignRight);
        body->addLayout(column, 1);
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    return card;
}

void CleanupSection::applyThemeStyle() {
    const Theme::AppTheme theme = Theme::currentTheme();
    m_styledTheme = theme;
    const Theme::ThemePalette& p = Theme::themePalette(theme);
    const int radius = isOnyx(theme) ? 2 : 8;
    const QString error = QLatin1String(Theme::kStatusError);
    // Only the card surfaces and the destructive-button accent are local;
    // everything else (fonts, base button look, hover/pressed/focus) comes
    // from the app-wide theme stylesheet.
    setStyleSheet(QStringLiteral(R"(
QFrame#cleanupCard { background: %1; border: 1px solid %2; border-radius: %3px; }
QFrame#cleanupDangerCard { background: %1; border: 1px solid %4; border-radius: %3px; }
QLabel#cleanupTitle { font-weight: 600; }
QLabel#cleanupTier { color: %5; font-size: 8pt; font-weight: 600; }
QLabel#cleanupDangerTier { color: %4; font-size: 8pt; font-weight: 600; }
QPushButton#dangerButton { color: %4; border: 1px solid %4; }
QPushButton#dangerButton:hover { background: %4; color: #ffffff; }
QPushButton#dangerButton:pressed { background: %4; color: #ffffff; }
QPushButton#dangerButton:disabled { color: %6; border-color: %2; background: transparent; }
)")
                      .arg(p.panelBg.name(QColor::HexArgb), p.border.name(QColor::HexArgb))
                      .arg(radius)
                      .arg(error, p.textSecondary.name(QColor::HexArgb), p.textDisabled.name(QColor::HexArgb)));
}

void CleanupSection::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    // setStyleSheet() itself raises PaletteChange - only restyle when the
    // Motiva theme really changed, or this would recurse forever.
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::ThemeChange) &&
        Theme::currentTheme() != m_styledTheme) {
        applyThemeStyle();
    }
}

void CleanupSection::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_manager->refreshSizes();
}

QString CleanupSection::sizeText(qint64 bytes) {
    if (bytes < 0) {
        return tr("Size unavailable");
    }
    if (bytes == 0) {
        return tr("Nothing stored on disk");
    }
    return tr("Approx. %1").arg(QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat));
}

void CleanupSection::onSizesReady(const CleanupManager::Sizes& sizes) {
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

bool CleanupSection::confirm(Op op) {
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

    QMessageBox box(icon, title, text, QMessageBox::NoButton, window());
    box.setInformativeText(info);
    QPushButton* cancel = box.addButton(QMessageBox::Cancel);
    QPushButton* proceed = box.addButton(action, QMessageBox::DestructiveRole);
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    return box.clickedButton() == proceed;
}

void CleanupSection::runWithProgress(Op op) {
    if (m_manager->isBusy()) {
        return;
    }
    auto* dialog = new CleanupProgressDialog(m_manager, op, window());
    dialog->show();
    m_manager->run(op);
}

void CleanupSection::showMoreDialog() {
    auto* dialog = new QDialog(window());
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
