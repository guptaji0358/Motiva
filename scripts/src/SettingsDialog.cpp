#include "SettingsDialog.h"
#include "WallpaperManager.h"
#include "SettingsManager.h"
#include "Theme.h"

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

SettingsDialog::SettingsDialog(WallpaperManager* manager, SettingsManager* settings, QWidget* parent)
    : QDialog(parent), m_manager(manager), m_settings(settings) {
    setWindowTitle(tr("Settings"));
    // Embedded via resources/app.qrc (Assets/settings-icon/settings.svg)
    // - same gear icon as the header button/tray menu entry that opens
    // this dialog, for visual consistency. Picks the light/dark SVG
    // variant matching the current OS palette - see
    // Theme::isDarkPalette() and MainWindow.cpp's settingsIconPath().
    setWindowIcon(QIcon(Theme::isDarkPalette() ? QStringLiteral(":/settings-icon/dark/settings.svg")
                                                : QStringLiteral(":/settings-icon/light/settings.svg")));
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
    setMinimumSize(860, 380);
    resize(880, 400);
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
        tr("Keep the video wallpaper visible when running on battery power."));
    connect(m_showVideoOnBatteryCheck, &QCheckBox::toggled, this, &SettingsDialog::onShowVideoOnBatteryToggled);
    rightCol->addWidget(m_showVideoOnBatteryCheck);

    auto* appearanceForm = new QFormLayout();
    appearanceForm->setSpacing(10);
    appearanceForm->setContentsMargins(0, 4, 0, 0);
    appearanceForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // Appearance (Light/Dark/System) and Style (Modern Aurora/Motiva Onyx)
    // are two independent axes - see Theme.h. Appearance previously had no
    // user-facing control at all (the app only ever silently followed the
    // OS palette); this combo is new.
    m_appearanceCombo = new QComboBox(this);
    m_appearanceCombo->addItem(tr("System"));
    m_appearanceCombo->addItem(tr("Light"));
    m_appearanceCombo->addItem(tr("Dark"));
    m_appearanceCombo->setToolTip(tr("Choose Light or Dark, or follow the Windows setting."));
    connect(m_appearanceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onAppearanceChanged);
    appearanceForm->addRow(tr("Appearance:"), m_appearanceCombo);

    m_styleCombo = new QComboBox(this);
    m_styleCombo->addItem(Theme::styleName(Theme::StyleId::ModernAurora));
    m_styleCombo->addItem(Theme::styleName(Theme::StyleId::MotivaOnyx));
    m_styleCombo->setToolTip(tr("Switch between Motiva's two built-in visual styles."));
    connect(m_styleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onStyleChanged);
    appearanceForm->addRow(tr("Style:"), m_styleCombo);
    rightCol->addLayout(appearanceForm);

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
    m_styleCombo->setCurrentIndex(m_settings->uiStyle());
    m_appearanceCombo->setCurrentIndex(m_settings->appearance());

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

void SettingsDialog::onAppearanceChanged(int index) {
    m_settings->setAppearance(index);
    // Live switch, no restart needed - Theme::applyAppearance() swaps
    // QApplication's actual QPalette, and every widget using palette(...)
    // in the QSS (i.e. all of it, for base surfaces/text) re-polishes
    // immediately off the new palette. Icons that pick a light/dark SVG
    // variant (settings gear, wallpaper set/remove) refresh themselves via
    // MainWindow::changeEvent()'s existing QEvent::PaletteChange handling,
    // since QApplication::setPalette() triggers exactly that event on
    // every top-level widget - no separate icon-refresh call needed here.
    Theme::applyAppearance(static_cast<Theme::AppearanceId>(index));
    setWindowIcon(QIcon(Theme::isDarkPalette() ? QStringLiteral(":/settings-icon/dark/settings.svg")
                                                : QStringLiteral(":/settings-icon/light/settings.svg")));
}

void SettingsDialog::onStyleChanged(int index) {
    m_settings->setUiStyle(index);
    // Live switch, no restart needed - same mechanism main.cpp uses for
    // the initial application, just re-applied to the already-running
    // QApplication. Every existing widget re-polishes against the new
    // stylesheet immediately; icon variants (light/dark, per-state) are
    // independent of this and unaffected by a style change.
    qApp->setStyleSheet(Theme::appStyleSheet(static_cast<Theme::StyleId>(index)));
}
