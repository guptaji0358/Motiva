#pragma once

#include "LibraryDatabase.h" // MediaFacts

#include <QDateTime>
#include <QDialog>
#include <QFutureWatcher>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>

class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTimer;
class QTreeWidget;

// A file found while looking for missing media.
struct MediaCandidate {
    QString path;
    qint64 size = -1;
    QDateTime modified; // UTC
    bool sameName = false;
    bool sameSize = false;     // only meaningful when the original size is known
    bool sameModified = false; // within 2 s (FAT/exFAT time resolution)
    // Name, size and modified time all match: the same file, just moved.
    bool isConvincing() const { return sameName && sameSize && sameModified; }
};

// Shared state between the search worker and the dialog. The worker only
// reads `cancel` and updates counters; the dialog polls them for display.
struct MediaSearchProgress {
    std::atomic<bool> cancel{false};
    std::atomic<int> foldersScanned{0};
    std::atomic<int> filesExamined{0};
    std::atomic<int> matched{0};             // requests with a convincing candidate so far
    std::atomic<int> inaccessibleFolders{0}; // folders that could not be listed
};

// One file to look for.
struct MediaSearchRequest {
    QString path; // last known location
    MediaFacts facts;
};

// Outcome of one coordinated search over several requests.
struct MediaBatchResult {
    QVector<QVector<MediaCandidate>> candidates; // parallel to the requests
    QStringList inaccessible;                    // sample of folders that could not be read
    bool cancelled = false;
};

// Looks for moved/renamed media files on the local drives. Pure functions,
// run on a worker thread (never the GUI thread). Search order - most likely
// first, each folder visited at most once:
//   1. the nearest still-existing folder of each original path,
//   2. the user's media folders (Pictures, Videos, Desktop, Downloads,
//      Documents, OneDrive),
//   3. every local fixed/removable drive.
// System locations (Windows, Program Files, ProgramData, AppData, recycle
// bin...), junctions/symlinks and network/optical drives are skipped. As
// soon as a phase has produced a convincing match for every request the
// search stops; weak (name-only, or renamed-but-identical size+time) matches
// let it continue.
//
// searchMany() is the single-pass form: ALL requests are matched against
// every directory entry in one traversal (a hash lookup per file), so
// finding N files costs one walk, not N. search() is the one-file wrapper.
// A non-empty `rootsOverride` replaces the three phases with exactly those
// folders (a scoped search).
namespace MediaRecovery {
MediaBatchResult searchMany(const QVector<MediaSearchRequest>& requests,
                            std::shared_ptr<MediaSearchProgress> progress,
                            const QStringList& rootsOverride = QStringList());
QVector<MediaCandidate> search(const QString& originalPath, const MediaFacts& facts,
                               std::shared_ptr<MediaSearchProgress> progress);

// Why `path`'s original location can't be searched/read at all (its drive
// isn't connected, or it is a network location), or an empty string.
QString unreachableReason(const QString& path);
}

// "Find File" for a missing image or video: searches (off the GUI thread,
// cancellable), then either
//  - accepts automatically when there is exactly one convincing match,
//  - asks the user to pick when there are several or only weak matches,
//  - or reports that nothing was found.
// Result: exec()/finished() code is Found, NotFound or Rejected (cancelled);
// chosenPath() holds the file for Found. It never changes the library
// itself - the caller decides (PlaylistLibrary::relocateMedia / remove).
class MediaRecoveryDialog : public QDialog {
    Q_OBJECT
public:
    enum Result { Found = 10, NotFound = 11 };

    MediaRecoveryDialog(const QString& originalPath, const MediaFacts& facts, QWidget* parent = nullptr);
    ~MediaRecoveryDialog() override;

    QString chosenPath() const { return m_chosen; }

protected:
    void showEvent(QShowEvent* event) override;
    void reject() override;

private:
    void startSearch();
    void onSearchFinished();
    void showChoices(const QVector<MediaCandidate>& candidates);
    void updateProgressText();

    QString m_originalPath;
    MediaFacts m_facts;
    QString m_chosen;
    bool m_started = false;
    std::shared_ptr<MediaSearchProgress> m_progress;
    QFutureWatcher<QVector<MediaCandidate>> m_watcher;

    QStackedWidget* m_pages = nullptr;
    QLabel* m_statusLabel = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QTimer* m_progressTimer = nullptr;
    QTreeWidget* m_choices = nullptr;
    QPushButton* m_useButton = nullptr;
};

// "Find All at Once": looks for every missing file of a playlist in ONE
// coordinated traversal (MediaRecovery::searchMany), then lists, per file,
// whether it was found, needs a choice, was not found, or could not be
// looked for (its drive isn't connected / folders weren't readable).
// Result: Accepted when the user applies; resolutions() then holds the
// (media id -> new path) repairs to make. It never changes the library
// itself - the caller does (PlaylistLibrary::relocateMedia).
class BulkMediaRecoveryDialog : public QDialog {
    Q_OBJECT
public:
    struct Entry {
        qint64 mediaId = 0;
        QString path;
        MediaFacts facts;
    };
    struct Resolution {
        qint64 mediaId = 0;
        QString newPath;
    };

    BulkMediaRecoveryDialog(const QVector<Entry>& entries, QWidget* parent = nullptr);
    ~BulkMediaRecoveryDialog() override;

    QVector<Resolution> resolutions() const;
    int entryCount() const { return int(m_entries.size()); }

protected:
    void showEvent(QShowEvent* event) override;
    void reject() override;

private:
    void startSearch();
    void onSearchFinished();
    void showResults(const MediaBatchResult& result);
    void updateProgressText();
    void updateApplyButton();

    QVector<Entry> m_entries;
    bool m_started = false;
    bool m_stopping = false;
    std::shared_ptr<MediaSearchProgress> m_progress;
    QFutureWatcher<MediaBatchResult> m_watcher;

    QStackedWidget* m_pages = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_countsLabel = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QPushButton* m_stopButton = nullptr;
    QTimer* m_progressTimer = nullptr;
    QLabel* m_summaryLabel = nullptr;
    QLabel* m_noteLabel = nullptr;
    QTreeWidget* m_results = nullptr;
    QPushButton* m_applyButton = nullptr;
    bool m_updatingChecks = false;
};
