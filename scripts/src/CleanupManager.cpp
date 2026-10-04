#include "CleanupManager.h"
#include "LibraryDatabase.h"
#include "PlaylistLibrary.h"
#include "RecoveryState.h"
#include "SettingsManager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent>

CleanupManager::CleanupManager(SettingsManager* settings, RecoveryState* recovery, PlaylistLibrary* library,
                               QObject* parent)
    : QObject(parent), m_settings(settings), m_recovery(recovery), m_library(library) {
    qRegisterMetaType<CleanupManager::Sizes>();
    connect(&m_stepWatcher, &QFutureWatcher<QString>::finished, this,
            [this] { finishStep(m_stepWatcher.result()); });
    connect(&m_sizeWatcher, &QFutureWatcher<Sizes>::finished, this,
            [this] { emit sizesReady(m_sizeWatcher.result()); });
}

CleanupManager::~CleanupManager() {
    // Worker steps only touch files by path, never this object - but don't
    // let one outlive the process' own shutdown either.
    m_stepWatcher.waitForFinished();
    m_sizeWatcher.waitForFinished();
}

// ------------------------------------------------------------------ paths

// Same location main.cpp's fileLogHandler writes to.
QString CleanupManager::logFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/Motiva.log");
}

QString CleanupManager::libraryDirectory() {
    return QFileInfo(LibraryDatabase::defaultLibraryPath()).absolutePath();
}

QStringList CleanupManager::libraryFiles() {
    const QFileInfo library(LibraryDatabase::defaultLibraryPath());
    QStringList files{library.absoluteFilePath(), library.absoluteFilePath() + QStringLiteral("-journal")};
    // Copies LibraryDatabase::open() moved aside after failing to read them
    // ("<name>.unreadable-<timestamp>.mtv"), only in the library folder.
    const QFileInfoList aside = QDir(library.absolutePath())
                                    .entryInfoList({library.completeBaseName() + QStringLiteral(".unreadable-*.mtv")},
                                                   QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo& fi : aside) {
        files << fi.absoluteFilePath();
    }
    return files;
}

QStringList CleanupManager::recoveryFiles() {
    return {RecoveryState::stateFilePath(), RecoveryState::tempFilePath()};
}

// A recovery-state .tmp only outlives RecoveryState::save() if its atomic
// replace failed - a leftover temp file, nothing reads it.
QStringList CleanupManager::staleTempFiles() {
    return {RecoveryState::tempFilePath()};
}

// Builds from before the Motiva rename stored their recovery state under
// the old product name, next to Motiva's own (Roaming\<org>\<app>).
QString CleanupManager::legacyDirectory() {
    QDir dir(QFileInfo(RecoveryState::stateFilePath()).absolutePath());
    if (dir.dirName() != QLatin1String("Motiva") || !dir.cdUp() || dir.dirName() != QLatin1String("Motiva") ||
        !dir.cdUp()) {
        return QString();
    }
    return QDir::cleanPath(dir.filePath(QStringLiteral("VideoWallpaper/VideoWallpaper")));
}

QStringList CleanupManager::legacyFiles() {
    const QString dir = legacyDirectory();
    if (dir.isEmpty()) {
        return {};
    }
    return {dir + QStringLiteral("/recovery-state.json"), dir + QStringLiteral("/recovery-state.json.tmp")};
}

QStringList CleanupManager::approvedRoots() {
    QStringList roots{libraryDirectory(), QFileInfo(RecoveryState::stateFilePath()).absolutePath()};
    const QString legacy = legacyDirectory();
    if (!legacy.isEmpty()) {
        roots << legacy;
    }
    return roots;
}

bool CleanupManager::isOwnedPath(const QString& path) {
    if (path.isEmpty()) {
        return false;
    }
    const QFileInfo fi(path);
    if (fi.isSymLink() || fi.isJunction() || fi.isDir()) {
        return false;
    }
    const QString clean = QDir::cleanPath(fi.absoluteFilePath());
    if (clean.compare(QDir::cleanPath(logFilePath()), Qt::CaseInsensitive) == 0) {
        return true;
    }
    for (const QString& root : approvedRoots()) {
        const QString cleanRoot = QDir::cleanPath(root);
        // A root must be a real per-user app folder, never a drive or a
        // shallow system folder (defends against an empty/odd location).
        if (cleanRoot.count(QLatin1Char('/')) < 4) {
            continue;
        }
        if (clean.startsWith(cleanRoot + QLatin1Char('/'), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QString CleanupManager::removeOwnedFile(const QString& path) {
    if (!isOwnedPath(path)) {
        qWarning() << "[Cleanup] Refused to delete a path outside Motiva's own data:" << QDir::toNativeSeparators(path);
        return tr("Refused (not Motiva's own data): %1").arg(QDir::toNativeSeparators(path));
    }
    QFile f(path);
    if (!f.exists()) {
        return QString();
    }
    if (!f.remove()) {
        qWarning() << "[Cleanup] Could not delete" << QDir::toNativeSeparators(path) << "-" << f.errorString();
        return tr("%1 could not be deleted (%2)").arg(QFileInfo(path).fileName(), f.errorString());
    }
    qInfo() << "[Cleanup] Deleted" << QDir::toNativeSeparators(path);
    return QString();
}

QString CleanupManager::removeOwnedFiles(const QStringList& paths) {
    QStringList errors;
    for (const QString& path : paths) {
        const QString error = removeOwnedFile(path);
        if (!error.isEmpty()) {
            errors << error;
        }
    }
    return errors.join(QLatin1Char('\n'));
}

// rmdir only - succeeds solely for an already-empty folder, and only for
// the exact folders Motiva creates.
void CleanupManager::removeEmptyOwnedDir(const QString& dir) {
    // Parents are derived from the path strings, not QDir::cdUp(): cdUp()
    // fails once the child folder itself has just been removed.
    auto parentOf = [](const QString& path) { return QFileInfo(path).absolutePath(); };
    QStringList allowed;
    const QString library = libraryDirectory();
    const QString recovery = QFileInfo(RecoveryState::stateFilePath()).absolutePath();
    allowed << library << recovery;
    for (const QString& appDir : {parentOf(library), recovery}) { // ...\Motiva\Motiva
        for (const QString& candidate : {appDir, parentOf(appDir)}) {  // and ...\Motiva
            if (QFileInfo(candidate).fileName() == QLatin1String("Motiva")) {
                allowed << candidate;
            }
        }
    }
    const QString legacy = legacyDirectory();
    if (!legacy.isEmpty()) {
        allowed << legacy << parentOf(legacy);
    }
    const QString clean = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    bool ok = false;
    for (const QString& a : std::as_const(allowed)) {
        ok = ok || clean.compare(QDir::cleanPath(a), Qt::CaseInsensitive) == 0;
    }
    const QFileInfo fi(clean);
    if (!ok || !fi.isDir() || fi.isSymLink() || fi.isJunction()) {
        return;
    }
    if (QDir().rmdir(clean)) {
        qInfo() << "[Cleanup] Removed empty folder" << QDir::toNativeSeparators(clean);
    }
}

// The log is held open (append) by main.cpp's message handler for the life
// of the process, so it is emptied in place rather than deleted; the
// handler seeks to the end before every write and carries on from zero.
QString CleanupManager::truncateLog() {
    const QString path = logFilePath();
    QFile f(path);
    if (!f.exists()) {
        return QString();
    }
    if (!isOwnedPath(path)) {
        return tr("Refused (not Motiva's own data): %1").arg(QDir::toNativeSeparators(path));
    }
    if (!f.open(QIODevice::ReadWrite) || !f.resize(0)) {
        return tr("The log could not be cleared (%1)").arg(f.errorString());
    }
    f.close();
    qInfo() << "[Cleanup] Diagnostic log cleared.";
    return QString();
}

qint64 CleanupManager::totalSize(const QStringList& paths) {
    qint64 total = 0;
    for (const QString& path : paths) {
        const QFileInfo fi(path);
        if (fi.isFile()) {
            total += fi.size();
        }
    }
    return total;
}

// ------------------------------------------------------------------ sizes

void CleanupManager::refreshSizes() {
    if (m_sizeWatcher.isRunning()) {
        return;
    }
    m_sizeWatcher.setFuture(QtConcurrent::run([] {
        Sizes s;
        const QFileInfo log(logFilePath());
        s.logPresent = log.isFile();
        s.logBytes = s.logPresent ? log.size() : 0;
        s.cacheBytes = s.logBytes + totalSize(staleTempFiles());
        s.dataBytes = totalSize(libraryFiles());
        s.recoveryPresent = QFileInfo(RecoveryState::stateFilePath()).isFile();
        s.recoveryBytes = totalSize(recoveryFiles());
        const QStringList legacy = legacyFiles();
        s.legacyPresent = !legacy.isEmpty() && QFileInfo(legacy.first()).isFile();
        s.legacyBytes = totalSize(legacy);
        return s;
    }));
}

int CleanupManager::cachedThumbnailCount() const {
    return m_library ? m_library->cachedThumbnailCount() : 0;
}

bool CleanupManager::hasWindowsIntegration() const {
    return m_settings && m_settings->hasWindowsIntegration();
}

// ------------------------------------------------------------------ steps

QVector<CleanupManager::Step> CleanupManager::buildSteps(Operation op) {
    const bool cache = op == Operation::DeleteCache || op == Operation::DeleteCacheAndData ||
                       op == Operation::FactoryReset;
    const bool data = op == Operation::DeleteData || op == Operation::DeleteCacheAndData ||
                      op == Operation::FactoryReset;
    const bool factory = op == Operation::FactoryReset;

    QVector<Step> steps;
    if (data) {
        // Before anything is removed: no wallpaper or player may still be
        // using a library/settings reference that is about to disappear.
        steps.push_back({tr("Releasing the wallpaper and current media"), false, [this] {
                             emit releaseMediaRequested();
                             return QString();
                         }});
    }
    if (factory) {
        steps.push_back({tr("Turning off Windows integration"), false, [this] {
                             m_settings->disableWindowsIntegration();
                             return m_settings->hasWindowsIntegration()
                                        ? tr("Windows integration is still reported as enabled.")
                                        : QString();
                         }});
    }
    if (data) {
        // Saved references go before the library is reopened, or its
        // one-time v1.1 settings migration would bring them straight back.
        if (!factory) {
            steps.push_back({tr("Clearing saved media references"), false, [this] {
                                 m_settings->clearDataReferences();
                                 m_recovery->setVideoPath(QString());
                                 return QString();
                             }});
        }
        steps.push_back({tr("Closing the playlist library"), false, [this] {
                             m_library->closeForCleanup();
                             return QString();
                         }});
        steps.push_back({tr("Removing playlists and the .mtv library"), true,
                         [] { return removeOwnedFiles(libraryFiles()); }});
        if (!factory) {
            steps.push_back({tr("Creating a new, empty library"), false, [this] {
                                 return m_library->reopenAfterCleanup()
                                            ? QString()
                                            : tr("The new library could not be saved: %1").arg(m_library->openNotice());
                             }});
        }
    }
    if (cache) {
        steps.push_back({tr("Clearing preview thumbnails"), false, [this] {
                             m_library->clearThumbnailCache();
                             return QString();
                         }});
        steps.push_back({tr("Clearing the diagnostic log"), false, [] { return truncateLog(); }});
        // GUI thread: RecoveryState::save() (also GUI thread) writes this file.
        steps.push_back({tr("Removing temporary files"), false, [] { return removeOwnedFiles(staleTempFiles()); }});
    }
    if (factory) {
        steps.push_back({tr("Resetting recovery state"), false, [this] { return m_recovery->removeForFactoryReset(); }});
        steps.push_back({tr("Resetting settings to defaults"), false, [this] {
                             m_settings->resetAll();
                             return QString();
                         }});
        steps.push_back({tr("Removing empty Motiva folders"), true, [] {
                             // Innermost first; rmdir keeps any folder that isn't empty.
                             const QString library = libraryDirectory();
                             const QString appDir = QFileInfo(library).absolutePath();
                             const QString recoveryDir = QFileInfo(RecoveryState::stateFilePath()).absolutePath();
                             for (const QString& dir : {library, appDir, QFileInfo(appDir).absolutePath(), recoveryDir,
                                                        QFileInfo(recoveryDir).absolutePath()}) {
                                 removeEmptyOwnedDir(dir);
                             }
                             return QString(); // a non-empty folder is simply kept
                         }});
    }

    switch (op) {
    case Operation::ResetRecoveryHistory:
        steps.push_back({tr("Resetting recovery history"), false, [this] { return m_recovery->resetHistory(); }});
        break;
    case Operation::ClearLog:
        steps.push_back({tr("Clearing the diagnostic log"), false, [] { return truncateLog(); }});
        break;
    case Operation::ResetWindowsIntegration:
        steps.push_back({tr("Turning off Windows integration"), false, [this] {
                             m_settings->disableWindowsIntegration();
                             emit preferencesChanged();
                             return QString();
                         }});
        break;
    case Operation::ResetPreferences:
        steps.push_back({tr("Resetting preferences to defaults"), false, [this] {
                             m_settings->resetPreferences();
                             emit preferencesChanged();
                             return QString();
                         }});
        break;
    case Operation::RemoveLegacyData:
        steps.push_back({tr("Removing data from earlier Motiva versions"), true, [] {
                             const QString error = removeOwnedFiles(legacyFiles());
                             const QString dir = legacyDirectory();
                             removeEmptyOwnedDir(dir);
                             removeEmptyOwnedDir(QFileInfo(dir).absolutePath());
                             return error;
                         }});
        break;
    default:
        break;
    }
    return steps;
}

QStringList CleanupManager::stepLabels(Operation op) const {
    QStringList labels;
    for (const Step& step : const_cast<CleanupManager*>(this)->buildSteps(op)) {
        labels << step.label;
    }
    return labels;
}

void CleanupManager::run(Operation op) {
    if (m_running) {
        return;
    }
    m_operation = op;
    m_steps = buildSteps(op);
    m_stepIndex = -1;
    m_allOk = true;
    m_running = true;
    qInfo() << "[Cleanup] Starting operation" << static_cast<int>(op) << "with" << m_steps.size() << "step(s).";
    QTimer::singleShot(0, this, &CleanupManager::runNextStep);
}

void CleanupManager::runNextStep() {
    ++m_stepIndex;
    if (m_stepIndex >= m_steps.size()) {
        m_running = false;
        qInfo() << "[Cleanup] Operation" << static_cast<int>(m_operation) << "finished, allSucceeded=" << m_allOk;
        emit finished(m_operation, m_allOk);
        return;
    }
    const Step& step = m_steps[m_stepIndex];
    emit stepStarted(m_stepIndex);
    if (step.onWorker) {
        m_stepWatcher.setFuture(QtConcurrent::run(step.action));
    } else {
        finishStep(step.action());
    }
}

void CleanupManager::finishStep(const QString& error) {
    if (!error.isEmpty()) {
        m_allOk = false;
        qWarning() << "[Cleanup] Step failed:" << m_steps[m_stepIndex].label << "-" << error;
    }
    emit stepFinished(m_stepIndex, error.isEmpty(), error);
    // Next event-loop turn: lets the progress UI paint each step.
    QTimer::singleShot(0, this, &CleanupManager::runNextStep);
}
