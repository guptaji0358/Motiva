#pragma once

#include <QDialog>

class QComboBox;
class QSlider;
class QCheckBox;
class WallpaperManager;
class SettingsManager;

// Houses the settings users rarely change (scaling, monitor selection,
// volume, mute, loop, start-with-Windows) separately from the main
// wallpaper workflow in MainWindow. Owns the wiring to WallpaperManager/
// SettingsManager directly for these fields - MainWindow only opens this
// dialog and stays in sync on mute (for the tray menu's checkable action).
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(WallpaperManager* manager, SettingsManager* settings, QWidget* parent = nullptr);

    // Applies the persisted values from SettingsManager to both the UI
    // controls here and the live WallpaperManager - call once at startup,
    // after both dependencies exist.
    void restoreFromSettings();

    // Re-reads WallpaperManager::availableMonitors() and rebuilds the
    // monitor combo's entries, preserving the current selection where
    // possible. Safe to call any time (e.g. if a monitor is connected while
    // the dialog is open).
    void refreshMonitorList();

    bool isMuted() const;

public slots:
    // Used by MainWindow to mirror the tray menu's checkable "Mute" action
    // into this dialog without a circular update loop.
    void setMuted(bool muted);

signals:
    // Lets MainWindow keep the tray "Mute" action's checked state in sync
    // without needing direct access to this dialog's internal checkbox.
    void mutedChanged(bool muted);

private:
    void buildUi();
    void onScalingChanged(int index);
    void onMonitorSelectionChanged(int index);
    void onVolumeChanged(int value);
    void onMuteToggled(bool checked);
    void onLoopToggled(bool checked);
    void onStartWithWindowsToggled(bool checked);
    void onShowVideoOnBatteryToggled(bool checked);
    void onStyleChanged(int index);
    void onAppearanceChanged(int index);

    WallpaperManager* m_manager;
    SettingsManager* m_settings;

    QComboBox* m_scalingCombo = nullptr;
    QComboBox* m_monitorCombo = nullptr;
    QSlider* m_volumeSlider = nullptr;
    QCheckBox* m_muteCheck = nullptr;
    QCheckBox* m_loopCheck = nullptr;
    QCheckBox* m_startWithWindowsCheck = nullptr;
    QCheckBox* m_showVideoOnBatteryCheck = nullptr;
    QComboBox* m_styleCombo = nullptr;
    QComboBox* m_appearanceCombo = nullptr;
};
