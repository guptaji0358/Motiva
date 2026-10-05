#pragma once

#include "BackupProvider.h"
#include "LibraryDatabase.h"

#include <QObject>
#include <QThreadPool>

#include <atomic>
#include <memory>

class PlaylistLibrary;
class SettingsManager;
class QTimer;

// Optional backup of media. OFF by default and never active unless the user
// turns it on: while OFF this class does not scan, copy or record anything.
//
// When ON, every media record of the library (a media record = one file,
// however many playlists reference it) gets ONE backup object through the
// BackupProvider (local folder today). The bookkeeping lives in the .mtv's
// media_backups table; the copy itself is a normal file.
//
// Threading: file copying runs on a single background worker (one file at a
// time); everything that touches SQLite - candidate scans, recording
// results, statistics - runs on the GUI thread, because Qt SQL connections
// belong to the thread that created them. The worker reports back through
// queued calls. Cancelling (or quitting) stops the copy at the next chunk
// and removes its partial file; finished copies stay valid, unfinished
// media stay eligible for a later run.
class BackupManager : public QObject {
    Q_OBJECT
public:
    enum class State { Off, Idle, Running, Stopping };

    BackupManager(PlaylistLibrary* library, SettingsManager* settings, QObject* parent = nullptr);
    ~BackupManager() override;

    BackupProvider* provider() const { return m_provider.get(); }
    bool isEnabled() const;
    State state() const { return m_state; }

    // What turning backup on would do - shown before the user confirms.
    struct EnablePreview {
        int items = 0;          // media still to back up
        qint64 bytes = 0;       // their stored size
        qint64 freeBytes = -1;  // free space where backups are stored (-1 unknown)
        QString location;
        QString problem;        // non-empty: the backup folder is unusable
    };
    EnablePreview previewEnable();

    // Turns backup on (and starts backing up existing media) or off (stops
    // a running backup; copies already made are kept).
    void setEnabled(bool on);
    // Runs a backup now, including media whose last attempt failed.
    void backUpNow();
    // Called once at startup: if backup is ON, checks the library after a
    // short delay (so startup is never slowed).
    void start();
    void cancel();
    // Stops a running backup and waits for the worker (it stops at the next
    // chunk). Used before data is removed and when Motiva exits.
    void cancelAndWait();

    // --- status (for the UI) ---
    int total() const { return m_total; }
    int done() const { return m_done; }
    int failed() const { return m_failed; }
    int waitingMissing() const { return m_missing; } // originals not found - nothing to copy
    QString currentName() const { return m_currentName; }
    int currentPercent() const { return m_currentPercent; }
    QString lastProblem() const { return m_lastProblem; }
    BackupStats stats() const { return m_stats; }
    void refreshStats();
    QString statusText() const; // one line for Settings / the backup window

    // --- restore (the missing-media recovery's "Restore from Backup") ---
    bool hasUsableBackup(qint64 mediaId);
    // Copies the backup to `destination` in the background (never over an
    // existing file), then points the SAME media record at it
    // (PlaylistLibrary::relocateMedia). Returns false if one is running.
    bool restoreMedia(qint64 mediaId, const QString& destination);
    bool isRestoring() const { return m_restoring; }

    // --- support for Cleanup & Reset ---
    // Unused backups (their media record is gone): the object files only
    // they use, then the rows. The file removal itself runs wherever the
    // caller wants (it is path-only); rows are GUI-thread database work.
    QStringList unusedObjects();
    void forgetUnusedBackups();
    // All backups: rows go, backup turns off (or it would just copy again).
    void forgetAllBackups();

signals:
    void enabledChanged(bool enabled);
    void stateChanged();
    void progressChanged();   // done/total/current file/percent
    void statsChanged();
    void itemFailed(const QString& name, const QString& reason);
    void restoreProgress(qint64 mediaId, int percent);
    void restoreFinished(qint64 mediaId, bool ok, const QString& path, const QString& message);

private:
    struct Job {
        qint64 mediaId = 0;
        QString path;
        QString name;
    };
    void scan(LibraryDatabase::BackupScan mode);
    void scheduleScan();
    void startBatch(const QVector<Job>& jobs);
    void onItemStarted(int index, const QString& name);
    void onItemFinished(const Job& job, const BackupResult& result);
    void onBatchFinished();
    void setState(State state);
    static QDateTime parseModified(const QString& iso);

    PlaylistLibrary* m_library;
    SettingsManager* m_settings;
    std::shared_ptr<BackupProvider> m_provider;
    QThreadPool m_pool;        // exactly one worker: files are copied one at a time
    QThreadPool m_restorePool; // restores must not queue behind a long backup
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<std::atomic<bool>> m_restoreCancel;
    QTimer* m_scanTimer = nullptr;
    QTimer* m_statsTimer = nullptr;
    State m_state = State::Off;
    bool m_libraryClosed = false;
    bool m_restoring = false;
    int m_rescan = -1; // a scan requested while running (LibraryDatabase::BackupScan), -1 = none
    int m_total = 0;
    int m_done = 0;
    int m_failed = 0;
    int m_missing = 0;
    int m_currentPercent = 0;
    QString m_currentName;
    QString m_lastProblem;
    BackupStats m_stats;
};
