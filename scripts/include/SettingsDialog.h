#pragma once

#include <QDialog>
#include <functional>
#include <vector>
#include <QString>

class QComboBox;
class QSlider;
class QCheckBox;
class WallpaperManager;
class SettingsManager;
class ThemeTransitionOverlay;
class CleanupManager;
class CleanupWindow;
class BackupManager;
class BackupWindow;
class QLabel;
class GlCanvas;
class GlPanel;
class GlLabel;
class GlButton;
class GlToggle;
class GlSlider;
class GlDropdown;
class GlScrollBar;
class GlItem;
class QCheckBox;

// Houses the settings users rarely change (scaling, monitor selection,
// volume, mute, loop, start-with-Windows) separately from the main
// wallpaper workflow in MainWindow. Owns the wiring to WallpaperManager/
// SettingsManager directly for these fields - MainWindow only opens this
// dialog and stays in sync on mute (for the tray menu's checkable action).
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    // cleanup: drives the separate Cleanup & Reset window (see
    // CleanupWindow), opened from this dialog's compact "Open Cleanup" entry.
    SettingsDialog(WallpaperManager* manager, SettingsManager* settings, CleanupManager* cleanup,
                   BackupManager* backup, QWidget* parent = nullptr);

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

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void showEvent(QShowEvent* event) override;

public slots:
    // Used by MainWindow to mirror the tray menu's checkable "Mute" action
    // into this dialog without a circular update loop.
    void setMuted(bool muted);

signals:
    // Lets MainWindow keep the tray "Mute" action's checked state in sync
    // without needing direct access to this dialog's internal checkbox.
    void mutedChanged(bool muted);
    // The Explorer menu was just (un)registered - MainWindow fills in the
    // playlist entries from the library it owns.
    void explorerIntegrationChanged(bool enabled);

    // Fired around a live Appearance/Visual Style change so MainWindow can
    // show/hide its own ThemeTransitionOverlay in lockstep with this
    // dialog's - both top-level windows restyle from the same call, so
    // both need to be covered for the "one deliberate operation" feel.
    void themeTransitionStarted();
    void themeTransitionFinished();

private:
    void buildUi();
    // 0 = Tabs, 1 = Sidebar, 2 = Grouped page; re-parents the same pages.
    void applyNavigationLayout(int layout);
    void layoutGl(const QSizeF& size);
    void setPage(int page);
    void scrollBy(qreal dy);
    void onScalingChanged(int index);
    void onMonitorSelectionChanged(int index);
    void onVolumeChanged(int value);
    void onMuteToggled(bool checked);
    void onLoopToggled(bool checked);
    void onStartWithWindowsToggled(bool checked);
    void onPlayVideosOnBatteryToggled(bool checked);
    void onPlayImagesOnBatteryToggled(bool checked);
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
    // Cleanup & Reset changed preferences underneath this dialog: applies
    // the (possibly default) theme and re-reads every control.
    void onPreferencesChangedExternally();
    // One Cleanup window per dialog: created on first use, re-focused after.
    void openCleanup();
    void openBackup();
    void updateBackupRow();

    WallpaperManager* m_manager;
    SettingsManager* m_settings;
    CleanupManager* m_cleanup;
    CleanupWindow* m_cleanupWindow = nullptr;
    BackupManager* m_backup;
    BackupWindow* m_backupWindow = nullptr;
    GlToggle* m_backupCheck = nullptr;
    GlLabel* m_backupStatus = nullptr;

    ThemeTransitionOverlay* m_transitionOverlay = nullptr;
    bool m_themeTransitionActive = false;
    std::function<void()> m_pendingApply;

    struct SettingsPage {
        int order;
        QString title;
    };
    // One settings row: a section heading, a labelled control, or a
    // description + button. Rows belong to a page; layoutGl() places them.
    struct Row {
        enum Type { Section, Toggle, Dropdown, Slider, ButtonRow };
        Type type;
        int page;
        GlItem* a;
        GlItem* b;
        qreal height;
    };
    std::vector<SettingsPage> m_pages;
    std::vector<Row> m_rows;
    std::vector<GlLabel*> m_headings;
    std::vector<GlButton*> m_navButtons;
    int m_navMode = 0;     // 0 Tabs, 1 Sidebar, 2 Grouped page
    int m_currentPage = 0;
    qreal m_scrollY = 0;

    GlCanvas* m_canvas = nullptr;
    GlPanel* m_titlePanel = nullptr;
    GlPanel* m_navPanel = nullptr;
    GlPanel* m_contentPanel = nullptr;
    GlLabel* m_titleLabel = nullptr;
    GlLabel* m_subtitleLabel = nullptr;
    GlButton* m_closeButton = nullptr;
    GlButton* m_footerClose = nullptr;
    GlScrollBar* m_scrollBar = nullptr;
    GlDropdown* m_layoutCombo = nullptr;

    GlDropdown* m_scalingCombo = nullptr;
    GlDropdown* m_monitorCombo = nullptr;
    GlSlider* m_volumeSlider = nullptr;
    GlToggle* m_muteCheck = nullptr;
    GlToggle* m_loopCheck = nullptr;
    // Notifications: in-app, desktop, success, error, warning, background, playback.
    GlToggle* m_notifyChecks[7] = {};
    GlToggle* m_startWithWindowsCheck = nullptr;
    GlToggle* m_playImagesOnBatteryCheck = nullptr;
    GlToggle* m_playVideosOnBatteryCheck = nullptr;
    GlToggle* m_showInWindowsSearchCheck = nullptr;
    GlToggle* m_explorerIntegrationCheck = nullptr;
    GlToggle* m_browserExtensionCheck = nullptr;
    // Single "Theme" selector (Dark Aurora / Light Aurora / Dark Onyx /
    // Light Onyx). Index order matches Theme::AppTheme's ordinal order exactly.
    GlDropdown* m_themeCombo = nullptr;
};
