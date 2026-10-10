#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>
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

    // Default false: the opt-in Explorer "Motiva >" context menu (Set as
    // background / Add to playlist) for supported media - see
    // WindowsShellIntegration::RegisterExplorerMenu/UnregisterExplorerMenu,
    // which the setter below actually invokes. The playlist entries are
    // kept in sync by MainWindow (it owns the library).
    bool explorerIntegrationEnabled() const;
    void setExplorerIntegrationEnabled(bool enabled);

    // Optional local backup of media (BackupManager). OFF by default: until
    // the user turns it on, Motiva never copies, scans or uploads media.
    bool backupEnabled() const;
    void setBackupEnabled(bool enabled);

    // --- Notifications (see NotificationManager) ---
    // In-app toasts and Windows desktop notifications are independent
    // channels; the five category switches below apply to both. Defaults:
    // everything on - desktop delivery is still limited to errors,
    // warnings, background-task completion and playback failures, and only
    // while Motiva is not the foreground app.
    bool notifyInApp() const;
    void setNotifyInApp(bool enabled);
    bool notifyDesktop() const;
    void setNotifyDesktop(bool enabled);
    bool notifySuccess() const;
    void setNotifySuccess(bool enabled);
    bool notifyError() const;
    void setNotifyError(bool enabled);
    bool notifyWarning() const;
    void setNotifyWarning(bool enabled);
    bool notifyBackground() const;
    void setNotifyBackground(bool enabled);
    bool notifyPlayback() const;
    void setNotifyPlayback(bool enabled);

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

    // --- Legacy (Motiva 1.1) single image playlist ---
    // Read once by PlaylistLibrary to migrate into the .mtv library, then
    // cleared - playlists now live only in the library (one source of truth).
    QStringList playlistPaths() const;
    int playlistCurrentIndex() const;
    bool playlistEnabled() const;
    bool playlistRotateOnUnlock() const;
    bool playlistRotateOnWindowsStart() const;
    bool playlistRotateOnInterval() const;
    int playlistIntervalMinutes() const;
    void clearLegacyPlaylistSettings();

    // Identity (logon time) of the Windows session Motiva last ran in -
    // see PlaylistRotation::currentWindowsSessionStamp(). 0 = never seen.
    qint64 lastWindowsSessionStamp() const;
    void setLastWindowsSessionStamp(qint64 stamp);

    // --- Cleanup & Reset (see CleanupManager) ---
    // All of these go through this one QSettings instance - Motiva's only
    // settings store (HKCU\Software\Motiva\Motiva) - never raw registry
    // deletes, and never anything outside Motiva's own keys besides the
    // integration entries the setters below already own.

    // True if any Windows integration this app installs is switched on
    // (Start with Windows, Start Menu shortcut, Explorer verbs).
    bool hasWindowsIntegration() const;
    // Switches all three off through their own setters, so the Run value,
    // Start Menu shortcut and Explorer verbs are actually removed.
    void disableWindowsIntegration();
    // "Delete Data": removes saved media references (the current media
    // path, the wallpaper-was-active flag, legacy v1.1 playlist keys).
    // Preferences (theme, volume, scaling...) are untouched.
    void clearDataReferences();
    // "Reset preferences": Windows integration off, then every user
    // preference back to its default. Saved media references and app
    // state (session stamp) are untouched.
    void resetPreferences();
    // "Factory Reset": Windows integration off, then every Motiva setting
    // removed - the next read of any key returns its built-in default,
    // exactly like a fresh install.
    void resetAll();

private:
    QSettings m_settings;
};
