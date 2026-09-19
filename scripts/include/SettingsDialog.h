#pragma once

#include <QDialog>
#include <functional>

class QComboBox;
class QSlider;
class QCheckBox;
class WallpaperManager;
class SettingsManager;
class ThemeTransitionOverlay;

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

    // Fired around a live Appearance/Visual Style change so MainWindow can
    // show/hide its own ThemeTransitionOverlay in lockstep with this
    // dialog's - both top-level windows restyle from the same call, so
    // both need to be covered for the "one deliberate operation" feel.
    void themeTransitionStarted();
    void themeTransitionFinished();

private:
    void buildUi();
    void onScalingChanged(int index);
    void onMonitorSelectionChanged(int index);
    void onVolumeChanged(int value);
    void onMuteToggled(bool checked);
    void onLoopToggled(bool checked);
    void onStartWithWindowsToggled(bool checked);
    void onShowVideoOnBatteryToggled(bool checked);
    void onThemeChanged(int index);
    void onShowInWindowsSearchToggled(bool checked);
    void onExplorerIntegrationToggled(bool checked);

    // Shared machinery behind a live Theme change: shows the transition
    // overlay (this dialog's own + MainWindow's, via the signals above),
    // runs applyFn on the next event-loop turn (so the overlay actually
    // paints first), suppresses repaints on the affected top-level windows
    // while applyFn runs so the palette/stylesheet swap can't be seen
    // mid-way through, then fades the overlay(s) out.
    //
    // Rapid switching: while a transition is already running, a new call
    // just replaces m_pendingApply (single controlled transition state,
    // m_themeTransitionActive) rather than starting a second overlay/apply
    // - only the latest selection ends up applied and persisted.
    void requestThemeTransition(std::function<void()> applyFn);
    void runPendingThemeApply();

    WallpaperManager* m_manager;
    SettingsManager* m_settings;

    ThemeTransitionOverlay* m_transitionOverlay = nullptr;
    bool m_themeTransitionActive = false;
    std::function<void()> m_pendingApply;

    QComboBox* m_scalingCombo = nullptr;
    QComboBox* m_monitorCombo = nullptr;
    QSlider* m_volumeSlider = nullptr;
    QCheckBox* m_muteCheck = nullptr;
    QCheckBox* m_loopCheck = nullptr;
    QCheckBox* m_startWithWindowsCheck = nullptr;
    QCheckBox* m_showVideoOnBatteryCheck = nullptr;
    QCheckBox* m_showInWindowsSearchCheck = nullptr;
    QCheckBox* m_explorerIntegrationCheck = nullptr;
    // Single "Theme" combo (Dark Aurora / Light Aurora / Dark Onyx /
    // Light Onyx) - replaces the old separate Appearance + Visual Style
    // combos, see the "Motiva Theme System" task. Index order matches
    // Theme::AppTheme's own ordinal order exactly.
    QComboBox* m_themeCombo = nullptr;
};
