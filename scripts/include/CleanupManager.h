#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class SettingsManager;
class RecoveryState;
class PlaylistLibrary;
class BackupManager;

// Cleanup & Reset: the one place that knows which files and settings Motiva
// owns, and the only code that deletes any of them. The Settings UI
// (CleanupWindow) only asks for an Operation; it never touches the
// filesystem itself.
//
// What Motiva actually stores (inspected, not assumed - every path comes
// from the class that owns it):
//  - Settings: QSettings, HKCU\Software\Motiva\Motiva (SettingsManager)
//  - Windows integration: Run value, Start Menu shortcut, Explorer verbs
//    (SettingsManager's setters / WindowsShellIntegration)
//  - Library: %LOCALAPPDATA%\Motiva\Motiva\libraries\MotivaLibrary.mtv
//    (+ SQLite's -journal, + "*.unreadable-*.mtv" copies LibraryDatabase
//    sets aside when a library can't be read) (LibraryDatabase)
//  - Recovery state: %APPDATA%\Motiva\Motiva\recovery-state.json (+ .tmp)
//    (RecoveryState)
//  - Diagnostic log: %TEMP%\Motiva.log (main.cpp's message handler)
//  - Preview thumbnails: in memory only (PlaylistModel) - no disk cache
//  - Pre-rename leftovers: %APPDATA%\VideoWallpaper\VideoWallpaper\
//    recovery-state.json, written by builds from before the Motiva rename
//
// Safety rules (enforced in removeOwnedFile/removeEmptyOwnedDir):
//  - only exact, known file paths are removed - never a directory tree,
//    never a filename pattern outside the library folder, never a path
//    that came from the user, the library contents or the settings;
//  - each path must resolve inside one of the approved roots above (the
//    log only by exact match) and must not be a symlink/junction;
//  - folders are only removed with rmdir (fails unless already empty);
//  - a missing file counts as done, a locked file is reported as failed.
// Media files referenced by playlists or settings are never candidates.
class CleanupManager : public QObject {
    Q_OBJECT
public:
    enum class Operation {
        DeleteCache,
        DeleteData,
        DeleteCacheAndData,
        FactoryReset,
        // Media backups are separate from cache and data: only these two
        // ever remove them, and only when the user asks.
        DeleteBackups,
        // "More..." operations
        ResetRecoveryHistory,
        ClearLog,
        ResetWindowsIntegration,
        ResetPreferences,
        RemoveLegacyData,
        DeleteUnusedBackups,
    };

    // Approximate on-disk sizes, -1 = could not be determined.
    struct Sizes {
        qint64 cacheBytes = -1;     // log + stale temp files
        qint64 dataBytes = -1;      // library files
        qint64 logBytes = -1;
        qint64 recoveryBytes = -1;  // recovery-state.json (+ .tmp)
        qint64 legacyBytes = -1;
        qint64 backupBytes = -1;   // everything under the backup folder
        int backupItems = 0;       // media with a valid backup
        int unusedBackups = 0;     // backups whose media is in no playlist any more
        bool recoveryPresent = false;
        bool logPresent = false;
        bool legacyPresent = false;
    };

    CleanupManager(SettingsManager* settings, RecoveryState* recovery, PlaylistLibrary* library,
                   BackupManager* backup, QObject* parent = nullptr);
    ~CleanupManager() override;

    // Measures the owned files off the GUI thread; emits sizesReady().
    void refreshSizes();
    bool isBusy() const { return m_running; }
    int cachedThumbnailCount() const;
    bool hasWindowsIntegration() const;

    // Human-readable step list for `op`, in execution order (the progress
    // dialog shows these before anything runs).
    QStringList stepLabels(Operation op) const;
    // Runs `op` step by step: filesystem steps on a worker thread, steps
    // that touch live objects (settings, library, recovery state) on the
    // GUI thread. Ignored while another operation is running.
    void run(Operation op);

    // After a Factory Reset: asks MainWindow to restart Motiva so every
    // in-memory object (theme, dialogs, library, recovery state) is rebuilt
    // from the now-clean state instead of being patched in place.
    void requestRestart() { emit restartRequested(); }

    static QString logFilePath();

signals:
    void sizesReady(const CleanupManager::Sizes& sizes);
    void stepStarted(int index);
    void stepFinished(int index, bool ok, const QString& detail);
    void finished(CleanupManager::Operation op, bool allSucceeded);
    // Emitted (synchronously) before any data is removed: MainWindow
    // releases the desktop wallpaper and unloads the current media through
    // its normal WallpaperManager/VideoPlayer paths.
    void releaseMediaRequested();
    // Preferences changed underneath the Settings UI (theme, toggles) -
    // SettingsDialog re-reads and re-applies them.
    void preferencesChanged();
    void restartRequested();

private:
    struct Step {
        QString label;
        bool onWorker = false;
        // Returns an error message; empty = success.
        std::function<QString()> action;
    };
    QVector<Step> buildSteps(Operation op);
    void runNextStep();
    void finishStep(const QString& error);

    // Owned-path rules - every deletion goes through these.
    static QStringList approvedRoots();
    static bool isOwnedPath(const QString& path);
    static QString removeOwnedFile(const QString& path);
    static QString removeOwnedFiles(const QStringList& paths);
    static void removeEmptyOwnedDir(const QString& dir);
    static QString truncateLog();

    // Known file sets, resolved from the owning classes.
    static QString libraryDirectory();
    static QStringList libraryFiles();
    static QStringList recoveryFiles();
    static QStringList staleTempFiles();
    static QString legacyDirectory();
    static QStringList legacyFiles();
    static qint64 totalSize(const QStringList& paths);

    SettingsManager* m_settings;
    RecoveryState* m_recovery;
    PlaylistLibrary* m_library;
    BackupManager* m_backup;

    QVector<Step> m_steps;
    int m_stepIndex = -1;
    bool m_running = false;
    bool m_allOk = true;
    Operation m_operation = Operation::DeleteCache;
    QFutureWatcher<QString> m_stepWatcher;
    QFutureWatcher<Sizes> m_sizeWatcher;
};

Q_DECLARE_METATYPE(CleanupManager::Sizes)
