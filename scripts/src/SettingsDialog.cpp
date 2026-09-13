#include "SettingsDialog.h"
#include "WallpaperManager.h"
#include "SettingsManager.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QSlider>
#include <QCheckBox>
#include <QLabel>
#include <QIcon>

SettingsDialog::SettingsDialog(WallpaperManager* manager, SettingsManager* settings, QWidget* parent)
    : QDialog(parent), m_manager(manager), m_settings(settings) {
    setWindowTitle(tr("Settings"));
    // Embedded via resources/app.qrc (Assets/settings-icon/settings.svg)
    // - same gear icon as the header button/tray menu entry that opens
    // this dialog, for visual consistency.
    setWindowIcon(QIcon(":/settings-icon/settings.svg"));
    buildUi();
    // A width-only minimum, set before the controls existed, left this
    // dialog's actual initial height at the mercy of whatever the layout
    // happened to compute with no floor under it - see the "Popup/Dialog
    // Window Visibility and Sizing" task. Both dimensions now have a
    // sensible minimum, set AFTER buildUi() so it reflects this dialog's
    // real content (two combo rows, a volume slider, four checkboxes,
    // three section headers, the button row), and adjustSize() sizes the
    // window from that content's actual sizeHint before it's ever shown -
    // still freely resizable larger, this is only a floor.
    setMinimumSize(380, 480);
    adjustSize();
}

void SettingsDialog::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(14);

    auto* displaySection = new QLabel(tr("Display"));
    displaySection->setObjectName(QStringLiteral("sectionLabel"));
    QFont sectionFont = displaySection->font();
    sectionFont.setBold(true);
    displaySection->setFont(sectionFont);
    root->addWidget(displaySection);

    auto* displayForm = new QFormLayout();
    displayForm->setSpacing(10);
    displayForm->setContentsMargins(0, 4, 0, 0);

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

    root->addLayout(displayForm);

    auto* playbackSection = new QLabel(tr("Playback"));
    playbackSection->setObjectName(QStringLiteral("sectionLabel"));
    playbackSection->setFont(sectionFont);
    root->addWidget(playbackSection);

    auto* volLayout = new QHBoxLayout();
    volLayout->setSpacing(10);
    volLayout->addWidget(new QLabel(tr("Volume:")));
    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setRange(0, 100);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &SettingsDialog::onVolumeChanged);
    volLayout->addWidget(m_volumeSlider);
    root->addLayout(volLayout);

    m_muteCheck = new QCheckBox(tr("Mute"), this);
    connect(m_muteCheck, &QCheckBox::toggled, this, &SettingsDialog::onMuteToggled);
    root->addWidget(m_muteCheck);

    m_loopCheck = new QCheckBox(tr("Loop video"), this);
    connect(m_loopCheck, &QCheckBox::toggled, this, &SettingsDialog::onLoopToggled);
    root->addWidget(m_loopCheck);

    auto* generalSection = new QLabel(tr("General"));
    generalSection->setObjectName(QStringLiteral("sectionLabel"));
    generalSection->setFont(sectionFont);
    root->addWidget(generalSection);

    m_startWithWindowsCheck = new QCheckBox(tr("Start with Windows"), this);
    connect(m_startWithWindowsCheck, &QCheckBox::toggled, this, &SettingsDialog::onStartWithWindowsToggled);
    root->addWidget(m_startWithWindowsCheck);

    m_showVideoOnBatteryCheck = new QCheckBox(tr("Show video on battery"), this);
    m_showVideoOnBatteryCheck->setToolTip(
        tr("Keep the video wallpaper visible when running on battery power."));
    connect(m_showVideoOnBatteryCheck, &QCheckBox::toggled, this, &SettingsDialog::onShowVideoOnBatteryToggled);
    root->addWidget(m_showVideoOnBatteryCheck);

    root->addStretch();

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
