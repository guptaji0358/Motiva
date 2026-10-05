#include "BackupManager.h"
#include "LocalBackupProvider.h"
#include "PlaylistLibrary.h"
#include "SettingsManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QLocale>
#include <QStorageInfo>
#include <QTimer>
#include <QtConcurrent>

namespace {
QString isoOf(const QDateTime& utc) {
    return utc.toUTC().toString(Qt::ISODateWithMs);
}
} // namespace

BackupManager::BackupManager(PlaylistLibrary* library, SettingsManager* settings, QObject* parent)
    : QObject(parent), m_library(library), m_settings(settings),
      m_provider(std::make_shared<LocalBackupProvider>()),
      m_cancel(std::make_shared<std::atomic<bool>>(false)),
      m_restoreCancel(std::make_shared<std::atomic<bool>>(false)) {
    m_pool.setMaxThreadCount(1);
    m_restorePool.setMaxThreadCount(1);
    // Nothing is running yet, so any *.partial left by an interrupted run is garbage.
    if (auto* local = dynamic_cast<LocalBackupProvider*>(m_provider.get())) {
        if (QFileInfo::exists(local->root())) {
            local->clearPartialFiles();
        }
    }

    m_scanTimer = new QTimer(this);
    m_scanTimer->setSingleShot(true);
    m_scanTimer->setInterval(700);
    connect(m_scanTimer, &QTimer::timeout, this, [this] { scan(LibraryDatabase::BackupScan::Pending); });
    m_statsTimer = new QTimer(this);
    m_statsTimer->setSingleShot(true);
    m_statsTimer->setInterval(400);
    connect(m_statsTimer, &QTimer::timeout, this, &BackupManager::refreshStats);

    // Media added to a playlist (the normal ingestion path) shows up as a
    // library change; when backup is ON, new media becomes eligible.
    connect(m_library, &PlaylistLibrary::playlistsChanged, this, [this] { scheduleScan(); });
    // The library is about to be closed/replaced (Cleanup & Reset): stop,
    // finish what the worker already reported, and don't touch the database
    // until it is back.
    connect(m_library, &PlaylistLibrary::aboutToReset, this, [this] {
        cancelAndWait();
        m_libraryClosed = true;
    });
    connect(m_library, &PlaylistLibrary::resetFinished, this, [this] {
        m_libraryClosed = false;
        refreshStats();
        scheduleScan();
    });

    m_state = isEnabled() ? State::Idle : State::Off;
    refreshStats();
}

BackupManager::~BackupManager() {
    cancelAndWait();
    m_restoreCancel->store(true);
    m_restorePool.waitForDone();
}

bool BackupManager::isEnabled() const {
    return m_settings->backupEnabled();
}

void BackupManager::setState(State state) {
    if (m_state != state) {
        m_state = state;
        emit stateChanged();
    }
}

void BackupManager::start() {
    if (!isEnabled()) {
        return;
    }
    // After startup has settled: a full check also notices originals that
    // changed since they were backed up and retries failed ones.
    QTimer::singleShot(3000, this, [this] { scan(LibraryDatabase::BackupScan::All); });
}

BackupManager::EnablePreview BackupManager::previewEnable() {
    EnablePreview preview;
    preview.location = m_provider->location();
    QString why;
    if (!m_provider->isAvailable(&why)) {
        preview.problem = why;
    }
    if (!m_libraryClosed) {
        const BackupStats stats = m_library->database().backupStats();
        preview.items = stats.pending + stats.failed;
        preview.bytes = stats.pendingBytes;
    }
    if (auto* local = dynamic_cast<LocalBackupProvider*>(m_provider.get())) {
        const QStorageInfo storage(local->root());
        preview.freeBytes = storage.isValid() ? storage.bytesAvailable() : -1;
    }
    return preview;
}

void BackupManager::setEnabled(bool on) {
    if (on == isEnabled()) {
        return;
    }
    m_settings->setBackupEnabled(on);
    if (on) {
        qInfo() << "[Backup] Turned ON by the user.";
        m_lastProblem.clear();
        setState(State::Idle);
        emit enabledChanged(true);
        scan(LibraryDatabase::BackupScan::All);
    } else {
        qInfo() << "[Backup] Turned OFF by the user.";
        cancelAndWait();
        m_rescan = -1;
        m_total = m_done = m_failed = m_missing = 0;
        setState(State::Off);
        emit progressChanged();
        emit enabledChanged(false);
    }
    refreshStats();
}

void BackupManager::backUpNow() {
    if (isEnabled()) {
        m_lastProblem.clear();
        scan(LibraryDatabase::BackupScan::All);
    }
}

void BackupManager::scheduleScan() {
    if (isEnabled() && !m_libraryClosed) {
        m_scanTimer->start();
    }
}

void BackupManager::cancel() {
    if (m_state == State::Running) {
        m_cancel->store(true);
        setState(State::Stopping);
    }
}

void BackupManager::cancelAndWait() {
    m_cancel->store(true);
    m_scanTimer->stop();
    m_pool.waitForDone();
    // Deliver what the worker already reported (valid finished copies are
    // recorded) while the library is still open.
    QCoreApplication::sendPostedEvents(this, 0);
    m_cancel->store(false);
    if (m_state == State::Running || m_state == State::Stopping) {
        onBatchFinished();
    }
}

void BackupManager::scan(LibraryDatabase::BackupScan mode) {
    if (!isEnabled() || m_libraryClosed) {
        return;
    }
    if (m_state == State::Running || m_state == State::Stopping) {
        m_rescan = qMax(m_rescan, static_cast<int>(mode)); // run again once this batch ends
        return;
    }
    QString why;
    if (!m_provider->isAvailable(&why)) {
        m_lastProblem = why;
        setState(State::Idle);
        emit progressChanged();
        return;
    }
    QVector<Job> jobs;
    m_missing = 0;
    for (const BackupCandidate& c : m_library->database().backupCandidates(mode)) {
        const QFileInfo fi(c.path);
        if (!fi.isFile()) {
            if (!c.backup.isComplete()) {
                ++m_missing; // nothing to copy: the original isn't there
            }
            continue;
        }
        if (c.backup.isComplete() && c.backup.size == fi.size() &&
            c.backup.sourceModified == isoOf(fi.lastModified()) &&
            m_provider->verify(c.backup.objectId, c.backup.size)) {
            continue; // already backed up and unchanged
        }
        jobs.push_back({c.mediaId, c.path, fi.fileName()});
    }
    if (jobs.isEmpty()) {
        m_total = m_done = m_failed = 0;
        setState(State::Idle);
        emit progressChanged();
        refreshStats();
        return;
    }
    startBatch(jobs);
}

void BackupManager::startBatch(const QVector<Job>& jobs) {
    m_total = jobs.size();
    m_done = 0;
    m_failed = 0;
    m_currentPercent = 0;
    m_currentName.clear();
    m_cancel->store(false);
    setState(State::Running);
    emit progressChanged();
    qInfo() << "[Backup] Starting a backup of" << jobs.size() << "media file(s).";

    auto provider = m_provider;
    auto cancel = m_cancel;
    QHash<QString, QString> hashIndex = m_library->database().backupHashIndex();
    QtConcurrent::run(&m_pool, [this, jobs, provider, cancel, hashIndex]() mutable {
        for (int i = 0; i < jobs.size(); ++i) {
            if (cancel->load()) {
                break;
            }
            const Job job = jobs[i];
            QMetaObject::invokeMethod(this, [this, i, name = job.name] { onItemStarted(i, name); }, Qt::QueuedConnection);
            const BackupResult result = provider->store(
                job.path,
                [&hashIndex](const QString& hash, qint64 size) {
                    return hashIndex.value(hash + QLatin1Char(':') + QString::number(size));
                },
                [this](qint64 doneBytes, qint64 totalBytes) {
                    const int percent = totalBytes > 0 ? static_cast<int>(doneBytes * 100 / totalBytes) : 100;
                    QMetaObject::invokeMethod(this, [this, percent] {
                        m_currentPercent = percent;
                        emit progressChanged();
                    }, Qt::QueuedConnection);
                },
                *cancel);
            if (result.ok && !result.object.reused) {
                hashIndex.insert(result.object.hash + QLatin1Char(':') + QString::number(result.object.size),
                                 result.object.objectId);
            }
            QMetaObject::invokeMethod(this, [this, job, result] { onItemFinished(job, result); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(this, [this] { onBatchFinished(); }, Qt::QueuedConnection);
    });
}

void BackupManager::onItemStarted(int /*index*/, const QString& name) {
    m_currentName = name;
    m_currentPercent = 0;
    emit progressChanged();
}

void BackupManager::onItemFinished(const Job& job, const BackupResult& result) {
    if (result.error.code == BackupError::Cancelled) {
        return; // unfinished: stays eligible, nothing is recorded
    }
    LibraryDatabase& db = m_library->database();
    if (m_libraryClosed) {
        // The library is gone: a finished copy nobody can ever refer to.
        if (result.ok && !result.object.reused) {
            m_provider->removeObjects({result.object.objectId});
        }
        return;
    }
    QString failure = result.ok ? QString() : result.error.message;
    if (result.ok) {
        const MediaBackupRecord old = db.backupFor(job.mediaId);
        MediaBackupRecord record;
        record.mediaId = job.mediaId;
        record.provider = m_provider->id();
        record.objectId = result.object.objectId;
        record.originalPath = job.path;
        record.size = result.object.size;
        record.sourceModified = isoOf(result.object.sourceModified);
        record.contentHash = result.object.hash;
        if (db.saveBackup(record)) {
            ++m_done;
            // A re-backup (the original changed) replaces the old copy: remove
            // it unless something else still uses that object.
            if (old.isComplete() && !old.objectId.isEmpty() && old.objectId != record.objectId &&
                db.backupObjectReferences(old.objectId) == 0) {
                m_provider->removeObjects({old.objectId});
            }
        } else {
            failure = tr("The backup could not be recorded in the library.");
            // Never leave a copy that isn't recorded (unless another record uses that object).
            if (!result.object.reused && db.backupObjectReferences(result.object.objectId) == 0) {
                m_provider->removeObjects({result.object.objectId});
            }
        }
    }
    if (!failure.isEmpty()) {
        db.saveBackupFailure(job.mediaId, job.path, failure);
        ++m_failed;
        ++m_done;
        m_lastProblem = tr("%1: %2").arg(job.name, failure);
        qWarning() << "[Backup] Failed:" << job.path << "-" << failure;
        emit itemFailed(job.name, failure);
    }
    emit progressChanged();
    m_statsTimer->start();
}

void BackupManager::onBatchFinished() {
    if (m_state != State::Running && m_state != State::Stopping) {
        return;
    }
    const bool stopped = m_state == State::Stopping || !isEnabled();
    m_currentName.clear();
    m_currentPercent = 0;
    setState(isEnabled() ? State::Idle : State::Off);
    qInfo() << "[Backup] Batch finished:" << m_done << "of" << m_total << "processed," << m_failed << "failed"
            << (stopped ? "(stopped)" : "");
    emit progressChanged();
    refreshStats();
    if (!stopped && m_rescan >= 0) {
        const auto mode = static_cast<LibraryDatabase::BackupScan>(m_rescan);
        m_rescan = -1;
        scan(mode);
    } else {
        m_rescan = -1;
    }
}

void BackupManager::refreshStats() {
    if (m_libraryClosed) {
        return;
    }
    m_stats = m_library->database().backupStats();
    emit statsChanged();
}

QString BackupManager::statusText() const {
    if (!isEnabled()) {
        return tr("Off - Motiva is not copying your media.");
    }
    if (m_state == State::Running) {
        return tr("Backing up…  %1 / %2").arg(m_done).arg(m_total);
    }
    if (m_state == State::Stopping) {
        return tr("Stopping…");
    }
    QString text = m_stats.complete == 1 ? tr("On - 1 media file backed up")
                                         : tr("On - %1 media files backed up").arg(m_stats.complete);
    if (m_stats.failed > 0) {
        text += tr("; %1 failed").arg(m_stats.failed);
    }
    if (m_missing > 0) {
        text += tr("; %1 waiting (original not found)").arg(m_missing);
    }
    return text;
}

QDateTime BackupManager::parseModified(const QString& iso) {
    return QDateTime::fromString(iso, Qt::ISODateWithMs).toUTC();
}

// ------------------------------------------------------------------ restore

bool BackupManager::hasUsableBackup(qint64 mediaId) {
    if (m_libraryClosed || mediaId <= 0) {
        return false;
    }
    const MediaBackupRecord rec = m_library->database().backupFor(mediaId);
    return rec.isComplete() && m_provider->verify(rec.objectId, rec.size);
}

bool BackupManager::restoreMedia(qint64 mediaId, const QString& destination) {
    if (m_restoring || m_libraryClosed) {
        return false;
    }
    const MediaBackupRecord rec = m_library->database().backupFor(mediaId);
    if (!rec.isComplete()) {
        emit restoreFinished(mediaId, false, destination, tr("There is no backup of this file."));
        return true;
    }
    m_restoring = true;
    m_restoreCancel->store(false);
    auto provider = m_provider;
    auto cancel = m_restoreCancel;
    const QDateTime modified = parseModified(rec.sourceModified);
    QtConcurrent::run(&m_restorePool, [this, provider, cancel, rec, mediaId, destination, modified] {
        const BackupResult result = provider->restore(
            rec.objectId, destination, rec.contentHash, modified,
            [this, mediaId](qint64 doneBytes, qint64 totalBytes) {
                const int percent = totalBytes > 0 ? static_cast<int>(doneBytes * 100 / totalBytes) : 100;
                QMetaObject::invokeMethod(this, [this, mediaId, percent] { emit restoreProgress(mediaId, percent); },
                                          Qt::QueuedConnection);
            },
            *cancel);
        QMetaObject::invokeMethod(this, [this, mediaId, destination, result] {
            m_restoring = false;
            if (!result.ok) {
                emit restoreFinished(mediaId, false, destination, result.error.message);
                return;
            }
            // Same media record, new location - every playlist follows.
            if (m_libraryClosed || !m_library->relocateMedia(mediaId, destination)) {
                emit restoreFinished(mediaId, false, destination,
                                     tr("The file was restored, but its location could not be updated in the library."));
                return;
            }
            emit restoreFinished(mediaId, true, destination, QString());
        }, Qt::QueuedConnection);
    });
    return true;
}

// ----------------------------------------------------- Cleanup & Reset support

QStringList BackupManager::unusedObjects() {
    return m_libraryClosed ? QStringList() : m_library->database().unusedBackupObjects();
}

void BackupManager::forgetUnusedBackups() {
    if (!m_libraryClosed) {
        m_library->database().deleteUnusedBackupRows();
        refreshStats();
    }
}

void BackupManager::forgetAllBackups() {
    cancelAndWait();
    if (!m_libraryClosed) {
        m_library->database().deleteAllBackupRows();
    }
    setEnabled(false); // otherwise the next scan would simply copy everything again
    refreshStats();
}
