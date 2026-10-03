#pragma once

#include "LibraryDatabase.h" // MediaFacts

#include <QDateTime>
#include <QDialog>
#include <QFutureWatcher>
#include <QString>
#include <QVector>
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
};

// Looks for a moved/renamed media file on the local drives. Pure function,
// run on a worker thread (never the GUI thread). Search order - most likely
// first, each folder visited at most once:
//   1. the nearest still-existing folder of the original path,
//   2. the user's media folders (Pictures, Videos, Desktop, Downloads,
//      Documents, OneDrive),
//   3. every local fixed/removable drive.
// System locations (Windows, Program Files, ProgramData, AppData, recycle
// bin...), junctions/symlinks and network/optical drives are skipped. As
// soon as a phase produces a convincing match the search stops; weak
// (name-only, or renamed-but-identical size+time) matches let it continue.
namespace MediaRecovery {
QVector<MediaCandidate> search(const QString& originalPath, const MediaFacts& facts,
                               std::shared_ptr<MediaSearchProgress> progress);
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
