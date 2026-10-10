#include "SettingsDialog.h"
#include "WallpaperManager.h"
#include "SettingsManager.h"
#include "DialogSizing.h"
#include "Theme.h"
#include "ThemeTransitionOverlay.h"
#include "CleanupManager.h"
#include "CleanupWindow.h"
#include "BackupManager.h"
#include "BackupWindow.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QSlider>
#include <QCheckBox>
#include <QLabel>
#include <QIcon>
#include <QApplication>
#include <QWidget>
#include <QSizePolicy>
#include <QTimer>
#include <QPushButton>

SettingsDialog::SettingsDialog(WallpaperManager* manager, SettingsManager* settings, CleanupManager* cleanup,
                               BackupManager* backup, QWidget* parent)
    : QDialog(parent), m_manager(manager), m_settings(settings), m_cleanup(cleanup), m_backup(backup) {
    setWindowTitle(tr("Settings"));
    m_transitionOverlay = new ThemeTransitionOverlay(this);
    // Embedded via resources/app.qrc (Assets/settings-icon/settings.svg)
    // - same gear icon as the header button/tray menu entry that opens
    // this dialog, for visual consistency. Picks the light/dark SVG
    // variant matching the current Motiva Theme - see Theme::iconVariant()
    // and MainWindow.cpp's settingsIconPath().
    setWindowIcon(QIcon(QStringLiteral(":/settings-icon/%1/settings.svg")
                             .arg(Theme::iconVariant(Theme::currentTheme()))));
    buildUi();
    // Redo of the "Popup proportions" fix: the previous pass split content
    // into two columns but only bumped setMinimumSize() to 560x320 (a
    // 1.75:1 ratio that's landscape on paper, yet still rendered
    // basically square/tall in practice) - because a QFormLayout's fields
    // default to growing only as much as their sizeHint demands
    // (QFormLayout::FieldsStayAtSizeHint is the *default* growth policy),
    // so the combo boxes/labels never actually claimed the extra width
    // columns->addLayout(..., 3/2) implied; adjustSize() then shrank the
    // window right back down to that narrow content's sizeHint, with only
    // the 560px floor propping the width up at all - the two columns were
    // narrow content Qt was free to shrink, not a real landscape layout.
    //
    // Real fix, in the layout itself (see buildUi() below), not a bigger
    // hardcoded floor fighting the layout:
    //  - both QFormLayout instances now use AllNonFixedFieldsGrow, so their
    //    combo boxes actually expand to fill the column width given to them
    //    by the QHBoxLayout stretch factors, instead of sizing to content;
    //  - both columns are wrapped in a QWidget with
    //    QSizePolicy::Expanding so the QHBoxLayout's 1:1 stretch is
    //    honored by real widgets, not by two QVBoxLayouts Qt can still
    //    collapse.
    // With the layout itself now landscape-shaped, this minimum is a
    // sensible floor (not a fight): resulting default size lands in the
    // ~760x380 range on this content, comfortably inside the requested
    // 720-800 x 360-450 range, and the dialog stays freely resizable
    // larger or smaller (down to this floor) afterward.
    connect(m_cleanup, &CleanupManager::preferencesChanged, this, &SettingsDialog::onPreferencesChangedExternally);
    DialogSizing::applyComfortableSize(this, QSize(880, 520));
}

void SettingsDialog::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(14);

    // Two side-by-side columns instead of one long vertical stack - the
    // sections (Display/Playback on the left, General on the right) read
    // fine split this way, and it's what makes the dialog's own content
    // landscape (wider than tall) rather than needing an arbitrary width
    // override. See the "Popup proportions" task / constructor comment.
    auto* columns = new QHBoxLayout();
    columns->setSpacing(28);

    // Each column is a real QWidget with an Expanding horizontal size
    // policy - not just a bare QVBoxLayout - so the QHBoxLayout's stretch
    // factors below have an actual widget to distribute extra width into.
    // A layout added directly to another layout has no size policy of its
    // own to expand; Qt was free to shrink it to its children's sizeHint
    // regardless of the stretch factor, which is what kept this dialog
    // narrow despite the "landscape" two-column structure.
    auto* leftPanel = new QWidget(this);
    leftPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* leftCol = new QVBoxLayout(leftPanel);
    leftCol->setContentsMargins(0, 0, 0, 0);
    leftCol->setSpacing(14);

    auto* displaySection = new QLabel(tr("Display"));
    displaySection->setObjectName(QStringLiteral("sectionLabel"));
    QFont sectionFont = displaySection->font();
    sectionFont.setBold(true);
    displaySection->setFont(sectionFont);
    leftCol->addWidget(displaySection);

    auto* displayForm = new QFormLayout();
    displayForm->setSpacing(10);
    displayForm->setContentsMargins(0, 4, 0, 0);
    // Default growth policy (FieldsStayAtSizeHint) is exactly what let
    // the combo boxes sit narrow inside a "wide" column - this makes them
    // actually claim the width the column now really has.
    displayForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_scalingCombo = new QComboBox(this);
    m_scalingCombo->addItems({tr("Fill"), tr("Fit"), tr("Stretch"), tr("Original")});
    connect(m_scalingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onScalingChanged);
    displayForm->addRow(tr("Scaling:"), m_scalingCombo);

    m_monitorCombo = new QComboBox(this);
    connect(m_monitorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onMonitorSelectionChanged);
    displayForm->addRow(tr("Monitor:"), m_monitorCombo);
    refreshMonitorList();

    leftCol->addLayout(displayForm);

    auto* playbackSection = new QLabel(tr("Playback"));
    playbackSection->setObjectName(QStringLiteral("sectionLabel"));
    playbackSection->setFont(sectionFont);
    leftCol->addWidget(playbackSection);

    auto* volLayout = new QHBoxLayout();
    volLayout->setSpacing(10);
    volLayout->addWidget(new QLabel(tr("Volume:")));
    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setRange(0, 100);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &SettingsDialog::onVolumeChanged);
    volLayout->addWidget(m_volumeSlider);
    leftCol->addLayout(volLayout);

    m_muteCheck = new QCheckBox(tr("Mute"), this);
    connect(m_muteCheck, &QCheckBox::toggled, this, &SettingsDialog::onMuteToggled);
    leftCol->addWidget(m_muteCheck);

    m_loopCheck = new QCheckBox(tr("Loop video"), this);
    connect(m_loopCheck, &QCheckBox::toggled, this, &SettingsDialog::onLoopToggled);
    leftCol->addWidget(m_loopCheck);

    // Notifications (see NotificationManager). Each switch is saved the
    // moment it is toggled and read live by the notifier - no restart.
    auto* notifySection = new QLabel(tr("Notifications"));
    notifySection->setObjectName(QStringLiteral("sectionLabel"));
    notifySection->setFont(sectionFont);
    leftCol->addWidget(notifySection);
    struct NotifyRow {
        const char* text;
        const char* tip;
        void (SettingsManager::*setter)(bool);
    };
    const NotifyRow rows[7] = {
        {"Show in-app notifications", "Short messages that appear inside Motiva's windows.",
         &SettingsManager::setNotifyInApp},
        {"Show Windows notifications",
         "Native Windows notifications for errors, warnings and finished tasks while Motiva isn't in front. "
         "Windows' own notification settings and Focus Assist still apply.",
         &SettingsManager::setNotifyDesktop},
        {"Successes", "Confirmations that something finished, such as a wallpaper added to a playlist.",
         &SettingsManager::setNotifySuccess},
        {"Errors", "Explanations when something failed.", &SettingsManager::setNotifyError},
        {"Warnings", "Recoverable problems such as a missing file or an unreadable folder.",
         &SettingsManager::setNotifyWarning},
        {"Background tasks", "Results of searches and other long-running work.",
         &SettingsManager::setNotifyBackground},
        {"Wallpaper playback", "When the wallpaper changes or playback fails.",
         &SettingsManager::setNotifyPlayback},
    };
    for (int i = 0; i < 7; ++i) {
        m_notifyChecks[i] = new QCheckBox(tr(rows[i].text), this);
        m_notifyChecks[i]->setToolTip(tr(rows[i].tip));
        if (i >= 2) {
            m_notifyChecks[i]->setStyleSheet(QStringLiteral("margin-left: 18px;")); // sub-switches of the two channels
        }
        const auto setter = rows[i].setter;
        connect(m_notifyChecks[i], &QCheckBox::toggled, this, [this, setter](bool on) { (m_settings->*setter)(on); });
        leftCol->addWidget(m_notifyChecks[i]);
    }

    leftCol->addStretch();

    auto* rightPanel = new QWidget(this);
    rightPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* rightCol = new QVBoxLayout(rightPanel);
    rightCol->setContentsMargins(0, 0, 0, 0);
    rightCol->setSpacing(14);

    auto* generalSection = new QLabel(tr("General"));
    generalSection->setObjectName(QStringLiteral("sectionLabel"));
    generalSection->setFont(sectionFont);
    rightCol->addWidget(generalSection);

    m_startWithWindowsCheck = new QCheckBox(tr("Start with Windows"), this);
    connect(m_startWithWindowsCheck, &QCheckBox::toggled, this, &SettingsDialog::onStartWithWindowsToggled);
    rightCol->addWidget(m_startWithWindowsCheck);

    m_showVideoOnBatteryCheck = new QCheckBox(tr("Show video on battery"), this);
    m_showVideoOnBatteryCheck->setToolTip(
        tr("Keep the video wallpaper visible when running on battery power. Image wallpapers are never shown on "
           "battery; they come back when AC power returns."));
    connect(m_showVideoOnBatteryCheck, &QCheckBox::toggled, this, &SettingsDialog::onShowVideoOnBatteryToggled);
    rightCol->addWidget(m_showVideoOnBatteryCheck);

    auto* themeForm = new QFormLayout();
    themeForm->setSpacing(10);
    themeForm->setContentsMargins(0, 4, 0, 0);
    themeForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // One "Theme" selector, not two competing Appearance/Visual Style
    // axes - each entry is a complete, independently-designed Motiva
    // appearance (see Theme.h). Item order matches Theme::AppTheme's own
    // ordinal order exactly, so `index` here can be cast straight to
    // Theme::AppTheme with no separate lookup table.
    m_themeCombo = new QComboBox(this);
    m_themeCombo->addItem(Theme::themeName(Theme::AppTheme::DarkAurora));
    m_themeCombo->addItem(Theme::themeName(Theme::AppTheme::LightAurora));
    m_themeCombo->addItem(Theme::themeName(Theme::AppTheme::DarkOnyx));
    m_themeCombo->addItem(Theme::themeName(Theme::AppTheme::LightOnyx));
    m_themeCombo->setToolTip(tr("Choose Motiva's visual theme."));
    connect(m_themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onThemeChanged);
    themeForm->addRow(tr("Theme:"), m_themeCombo);
    rightCol->addLayout(themeForm);

    auto* integrationSection = new QLabel(tr("Windows Integration"));
    integrationSection->setObjectName(QStringLiteral("sectionLabel"));
    integrationSection->setFont(sectionFont);
    rightCol->addWidget(integrationSection);

    m_showInWindowsSearchCheck = new QCheckBox(tr("Show Motiva in Windows Search"), this);
    m_showInWindowsSearchCheck->setToolTip(
        tr("Add a Start Menu shortcut so Motiva can be found via Windows Search."));
    connect(m_showInWindowsSearchCheck, &QCheckBox::toggled, this,
        &SettingsDialog::onShowInWindowsSearchToggled);
    rightCol->addWidget(m_showInWindowsSearchCheck);

    m_explorerIntegrationCheck = new QCheckBox(tr("Set as background for supported media"), this);
    m_explorerIntegrationCheck->setToolTip(
        tr("Adds one \"Motiva\" entry to the right-click menu for supported video, GIF and image files "
           "in File Explorer, with \"Set as background\" and \"Add to playlist\" (videos and images). On "
           "Windows 11 this may appear under \"Show more options\"."));
    connect(m_explorerIntegrationCheck, &QCheckBox::toggled, this,
        &SettingsDialog::onExplorerIntegrationToggled);
    rightCol->addWidget(m_explorerIntegrationCheck);

    // Optional local media backup: the ON/OFF switch (OFF by default) and a
    // one-line status; details, progress and storage live in BackupWindow.
    auto* backupSection = new QLabel(tr("Media Backup"));
    backupSection->setObjectName(QStringLiteral("sectionLabel"));
    backupSection->setFont(sectionFont);
    rightCol->addWidget(backupSection);
    m_backupCheck = new QCheckBox(tr("Back up my media"), this);
    m_backupCheck->setToolTip(BackupWindow::explanationText());
    connect(m_backupCheck, &QCheckBox::toggled, this, [this](bool on) {
        const bool ok = on ? BackupWindow::confirmAndEnable(m_backup, this) : BackupWindow::confirmAndDisable(m_backup, this);
        if (!ok) {
            m_backupCheck->blockSignals(true);
            m_backupCheck->setChecked(!on);
            m_backupCheck->blockSignals(false);
        }
        updateBackupRow();
    });
    rightCol->addWidget(m_backupCheck);
    auto* backupRow = new QHBoxLayout();
    m_backupStatus = new QLabel(this);
    m_backupStatus->setObjectName(QStringLiteral("secondaryText"));
    m_backupStatus->setWordWrap(true);
    backupRow->addWidget(m_backupStatus, 1);
    auto* manageBackup = new QPushButton(tr("Manage…"), this);
    connect(manageBackup, &QPushButton::clicked, this, &SettingsDialog::openBackup);
    backupRow->addWidget(manageBackup);
    rightCol->addLayout(backupRow);
    for (auto signal : {&BackupManager::enabledChanged}) {
        connect(m_backup, signal, this, &SettingsDialog::updateBackupRow);
    }
    connect(m_backup, &BackupManager::stateChanged, this, &SettingsDialog::updateBackupRow);
    connect(m_backup, &BackupManager::progressChanged, this, &SettingsDialog::updateBackupRow);
    connect(m_backup, &BackupManager::statsChanged, this, &SettingsDialog::updateBackupRow);
    updateBackupRow();

    // Compact entry only - the cleanup levels live in their own window.
    auto* cleanupSection = new QLabel(tr("Cleanup & Reset"));
    cleanupSection->setObjectName(QStringLiteral("sectionLabel"));
    cleanupSection->setFont(sectionFont);
    rightCol->addWidget(cleanupSection);
    auto* cleanupRow = new QHBoxLayout();
    auto* cleanupDesc = new QLabel(tr("Manage cache, data, and factory reset."), this);
    cleanupDesc->setObjectName(QStringLiteral("secondaryText"));
    cleanupDesc->setWordWrap(true);
    cleanupRow->addWidget(cleanupDesc, 1);
    auto* openCleanupButton = new QPushButton(tr("Open Cleanup"), this);
    connect(openCleanupButton, &QPushButton::clicked, this, &SettingsDialog::openCleanup);
    cleanupRow->addWidget(openCleanupButton);
    rightCol->addLayout(cleanupRow);

    rightCol->addStretch();

    columns->addWidget(leftPanel, 1);
    columns->addWidget(rightPanel, 1);
    root->addLayout(columns);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::close);
    root->addWidget(buttons);
}

void SettingsDialog::restoreFromSettings() {
    m_scalingCombo->setCurrentIndex(m_settings->scalingMode());
    m_manager->setScalingMode(static_cast<ScalingMode>(m_settings->scalingMode()));

    int monSel = m_settings->monitorSelection();
    int specific = m_settings->specificMonitorIndex();
    m_monitorCombo->setCurrentIndex(monSel == 2 ? 2 + qMax(0, specific) : monSel);

    m_volumeSlider->setValue(m_settings->volume());
    m_muteCheck->setChecked(m_settings->muted());
    m_loopCheck->setChecked(m_settings->loop());
    m_startWithWindowsCheck->setChecked(m_settings->startWithWindows());
    m_showVideoOnBatteryCheck->setChecked(m_settings->showVideoOnBattery());
    m_manager->setShowVideoOnBattery(m_settings->showVideoOnBattery());
    m_showInWindowsSearchCheck->setChecked(m_settings->showInWindowsSearch());
    m_explorerIntegrationCheck->setChecked(m_settings->explorerIntegrationEnabled());
    const bool notifyValues[7] = {m_settings->notifyInApp(),      m_settings->notifyDesktop(),
                                  m_settings->notifySuccess(),    m_settings->notifyError(),
                                  m_settings->notifyWarning(),    m_settings->notifyBackground(),
                                  m_settings->notifyPlayback()};
    for (int i = 0; i < 7; ++i) {
        m_notifyChecks[i]->blockSignals(true);
        m_notifyChecks[i]->setChecked(notifyValues[i]);
        m_notifyChecks[i]->blockSignals(false);
    }

    // Blocked, unlike a normal user pick: this only seeds the combo from
    // the persisted value at startup and must NOT fire onThemeChanged -
    // that kicks off a theme-transition overlay, which must never appear
    // during startup (only on a live, later change). Theme::applyTheme()
    // is already applied separately at startup (see main.cpp) - this
    // combo is just reflecting that, not re-applying it a second time.
    m_themeCombo->blockSignals(true);
    m_themeCombo->setCurrentIndex(static_cast<int>(m_settings->theme()));
    m_themeCombo->blockSignals(false);

    m_manager->setVolume(m_settings->volume());
    m_manager->setMuted(m_settings->muted());
    m_manager->setLooping(m_settings->loop());
}

void SettingsDialog::refreshMonitorList() {
    const int previousIndex = m_monitorCombo->currentIndex();
    m_monitorCombo->blockSignals(true);
    m_monitorCombo->clear();
    m_monitorCombo->addItem(tr("All monitors"));
    m_monitorCombo->addItem(tr("Primary monitor"));

    auto monitors = m_manager->availableMonitors();
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& mon = monitors[i];
        QString label = tr("Monitor %1 (%2x%3)%4")
            .arg(i + 1)
            .arg(mon.rect.right - mon.rect.left)
            .arg(mon.rect.bottom - mon.rect.top)
            .arg(mon.isPrimary ? tr(" - Primary") : "");
        m_monitorCombo->addItem(label);
    }
    if (previousIndex >= 0 && previousIndex < m_monitorCombo->count()) {
        m_monitorCombo->setCurrentIndex(previousIndex);
    }
    m_monitorCombo->blockSignals(false);
}

bool SettingsDialog::isMuted() const {
    return m_muteCheck && m_muteCheck->isChecked();
}

void SettingsDialog::setMuted(bool muted) {
    if (m_muteCheck) {
        m_muteCheck->setChecked(muted);
    }
}

void SettingsDialog::onScalingChanged(int index) {
    m_manager->setScalingMode(static_cast<ScalingMode>(index));
    m_settings->setScalingMode(index);
}

void SettingsDialog::onMonitorSelectionChanged(int index) {
    if (index == 0) {
        m_manager->setMonitorSelection(MonitorSelection::All);
        m_settings->setMonitorSelection(0);
    } else if (index == 1) {
        m_manager->setMonitorSelection(MonitorSelection::Primary);
        m_settings->setMonitorSelection(1);
    } else {
        int specificIndex = index - 2;
        m_manager->setMonitorSelection(MonitorSelection::Specific, specificIndex);
        m_settings->setMonitorSelection(2);
        m_settings->setSpecificMonitorIndex(specificIndex);
    }
}

void SettingsDialog::onVolumeChanged(int value) {
    m_manager->setVolume(value);
    m_settings->setVolume(value);
}

void SettingsDialog::onMuteToggled(bool checked) {
    m_manager->setMuted(checked);
    m_settings->setMuted(checked);
    emit mutedChanged(checked);
}

void SettingsDialog::onLoopToggled(bool checked) {
    m_manager->setLooping(checked);
    m_settings->setLoop(checked);
}

void SettingsDialog::onStartWithWindowsToggled(bool checked) {
    m_settings->setStartWithWindows(checked);
}

void SettingsDialog::onShowVideoOnBatteryToggled(bool checked) {
    m_settings->setShowVideoOnBattery(checked);
    // Takes effect immediately (matches Set as Wallpaper/Remove
    // Wallpaper's own convention) - WallpaperManager re-evaluates and
    // hides/restores the video right away if the current power state
    // means this toggle actually changes anything.
    m_manager->setShowVideoOnBattery(checked);
}

void SettingsDialog::onShowInWindowsSearchToggled(bool checked) {
    // The actual shortcut create/remove happens inside the setter itself
    // (see SettingsManager::setShowInWindowsSearch) - same convention as
    // onStartWithWindowsToggled's registry side effect above.
    m_settings->setShowInWindowsSearch(checked);
}

void SettingsDialog::onExplorerIntegrationToggled(bool checked) {
    // Likewise, the actual Explorer verb register/unregister happens
    // inside the setter (see SettingsManager::setExplorerIntegrationEnabled).
    m_settings->setExplorerIntegrationEnabled(checked);
    emit explorerIntegrationChanged(checked);
}

void SettingsDialog::onThemeChanged(int index) {
    const auto theme = static_cast<Theme::AppTheme>(index);
    // The persisted setting is the user's final selection the instant
    // they pick it in the combo - never an intermediate/transition state,
    // even if this particular apply ends up superseded by a later one
    // below (rapid switching).
    m_settings->setTheme(theme);
    requestThemeTransition([this, theme]() {
        // Live switch, no restart needed - Theme::applyTheme() swaps
        // QApplication's actual QPalette AND its global stylesheet
        // together, both generated from the same literal ThemePalette, so
        // every widget re-polishes off one fully self-consistent theme -
        // never a runtime derivation/inversion of another theme, and
        // never left to Qt's native palette conversion. Icons that pick a
        // light/dark SVG variant (settings gear, wallpaper set/remove)
        // refresh via MainWindow::changeEvent()'s existing
        // QEvent::PaletteChange handling, since QApplication::setPalette()
        // triggers exactly that event on every top-level widget.
        Theme::applyTheme(theme);
        setWindowIcon(QIcon(QStringLiteral(":/settings-icon/%1/settings.svg")
                                 .arg(Theme::iconVariant(theme))));
    });
}

void SettingsDialog::updateBackupRow() {
    m_backupCheck->blockSignals(true);
    m_backupCheck->setChecked(m_backup->isEnabled());
    m_backupCheck->blockSignals(false);
    m_backupStatus->setText(m_backup->statusText());
}

void SettingsDialog::openBackup() {
    if (!m_backupWindow) {
        m_backupWindow = new BackupWindow(m_backup, this);
        connect(m_backupWindow, &BackupWindow::openCleanupRequested, this, &SettingsDialog::openCleanup);
    }
    m_backupWindow->present();
}

void SettingsDialog::openCleanup() {
    if (!m_cleanupWindow) {
        m_cleanupWindow = new CleanupWindow(m_cleanup, this);
    }
    m_cleanupWindow->present();
}

void SettingsDialog::onPreferencesChangedExternally() {
    const Theme::AppTheme theme = m_settings->theme();
    if (theme != Theme::currentTheme()) {
        Theme::applyTheme(theme);
        setWindowIcon(QIcon(QStringLiteral(":/settings-icon/%1/settings.svg").arg(Theme::iconVariant(theme))));
    }
    restoreFromSettings();
}

void SettingsDialog::requestThemeTransition(std::function<void()> applyFn) {
    // "Ignore intermediate requests, apply the newest" - a rapid run of
    // selections while a transition is already in flight just keeps
    // replacing this, so only the latest ever actually gets applied.
    m_pendingApply = std::move(applyFn);

    if (m_themeTransitionActive) {
        return;
    }
    m_themeTransitionActive = true;

    m_transitionOverlay->beginTransition();
    emit themeTransitionStarted();

    // Deferred to the next event-loop turn so the overlay's first paint
    // actually reaches the screen before the (synchronous) theme apply
    // below runs - otherwise Qt could coalesce both into one repaint and
    // the overlay would never visibly appear before the change lands.
    QTimer::singleShot(0, this, &SettingsDialog::runPendingThemeApply);
}

void SettingsDialog::runPendingThemeApply() {
    // Suppress repaints on both affected top-level windows for the
    // duration of the actual palette/stylesheet swap, so the user never
    // sees it applied widget-by-widget - only the fully-updated result,
    // in one paint, once updates are re-enabled below.
    QWidget* mainWindow = parentWidget();
    if (mainWindow) {
        mainWindow->setUpdatesEnabled(false);
    }
    setUpdatesEnabled(false);

    // A change requested again while this very apply is running (it isn't
    // - the apply itself is synchronous - but a future async apply might
    // be) still only leaves the latest pending change in effect.
    while (m_pendingApply) {
        auto fn = std::move(m_pendingApply);
        m_pendingApply = nullptr;
        fn();
    }

    setUpdatesEnabled(true);
    update();
    if (mainWindow) {
        mainWindow->setUpdatesEnabled(true);
        mainWindow->update();
    }

    m_themeTransitionActive = false;
    m_transitionOverlay->finishTransition();
    emit themeTransitionFinished();
}
