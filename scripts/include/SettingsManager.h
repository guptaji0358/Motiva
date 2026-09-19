#pragma once

#include <QSettings>
#include <QString>
#include "Theme.h"

// Thin wrapper around QSettings (stored in the registry under
// HKCU\Software\Motiva via the default Windows QSettings backend).
// No cloud/account/internet dependency.
class SettingsManager {
public:
    SettingsManager();

    QString videoPath() const;
    void setVideoPath(const QString& path);

    int volume() const;
    void setVolume(int percent);

    bool muted() const;
    void setMuted(bool muted);

    bool loop() const;
    void setLoop(bool loop);

    int scalingMode() const; // maps to ScalingMode enum int
    void setScalingMode(int mode);

    int monitorSelection() const; // maps to MonitorSelection enum int
    void setMonitorSelection(int selection);

    int specificMonitorIndex() const;
    void setSpecificMonitorIndex(int index);

    bool startWithWindows() const;
    void setStartWithWindows(bool enabled);

    // The real user-facing Motiva executable path - resolves the dev-build
    // vs. deployed (resources/bin + deployment-root launcher) layout
    // difference exactly like startWithWindows's own autostart target does
    // (this IS that same resolution, promoted to public so Windows Search
    // shortcut / Explorer integration can reuse it instead of re-deriving
    // it - see WindowsShellIntegration.h).
    static QString motivaExecutablePath();

    // Default false: an opt-in Windows Search / Start Menu shortcut - see
    // WindowsShellIntegration::CreateStartMenuShortcut/RemoveStartMenuShortcut,
    // which the setter below actually invokes.
    bool showInWindowsSearch() const;
    void setShowInWindowsSearch(bool enabled);

    // Default false: an opt-in Explorer "Set as background" context-menu
    // verb for supported media - see
    // WindowsShellIntegration::Register/UnregisterSetBackgroundVerb, which
    // the setter below actually invokes.
    bool explorerIntegrationEnabled() const;
    void setExplorerIntegrationEnabled(bool enabled);

    bool wasWallpaperActive() const;
    void setWasWallpaperActive(bool active);

    // Default true: preserves existing behavior for existing users - only
    // someone who explicitly turns this off gets the battery-saving
    // behavior (see WallpaperManager's battery suspend/resume logic).
    bool showVideoOnBattery() const;
    void setShowVideoOnBattery(bool enabled);

    // The single selected Motiva Theme (see Theme::AppTheme) - replaces
    // the old two-axis Appearance x Visual Style settings. If neither
    // "app/theme" nor any legacy key has ever been written, defaults to
    // DarkAurora. If "app/theme" is absent but a legacy Appearance/Visual
    // Style value exists (pre-Theme-system installs), one-time migrates
    // via Theme::migrateLegacySettings() - see Theme.h.
    Theme::AppTheme theme() const;
    void setTheme(Theme::AppTheme theme);

private:
    QSettings m_settings;
};
