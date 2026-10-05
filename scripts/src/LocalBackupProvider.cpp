#include "LocalBackupProvider.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>

#include <chrono>

namespace {
constexpr qint64 kChunk = 4 * 1024 * 1024;
constexpr qint64 kSpaceMargin = 64LL * 1024 * 1024; // keep this much free beyond the copy itself

BackupResult fail(BackupError::Code code, const QString& message) {
    BackupResult r;
    r.error = {code, message};
    return r;
}

// Throttles progress callbacks to ~10/s so a fast copy doesn't flood the GUI.
class ProgressThrottle {
public:
    explicit ProgressThrottle(const BackupProgressFn& fn) : m_fn(fn) {}
    void report(qint64 done, qint64 total, bool force = false) {
        if (!m_fn) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (force || now - m_last >= std::chrono::milliseconds(100)) {
            m_last = now;
            m_fn(done, total);
        }
    }

private:
    const BackupProgressFn& m_fn;
    std::chrono::steady_clock::time_point m_last{};
};

QDateTime utcModified(const QFileInfo& fi) {
    return fi.lastModified().toUTC();
}

// Explains a failed write: a full disk is the common, actionable case.
BackupResult writeFailure(const QString& destinationDir, const QString& detail) {
    const QStorageInfo storage(destinationDir);
    if (storage.isValid() && storage.bytesAvailable() < kSpaceMargin / 4) {
        return fail(BackupError::InsufficientSpace,
                    QCoreApplication::translate("LocalBackupProvider", "The backup drive is out of space."));
    }
    return fail(BackupError::WriteFailed,
                QCoreApplication::translate("LocalBackupProvider", "Writing the backup failed: %1").arg(detail));
}
} // namespace

LocalBackupProvider::LocalBackupProvider(const QString& root) : m_root(root.isEmpty() ? defaultRoot() : root) {}

QString LocalBackupProvider::defaultRoot() {
    // Same per-user, per-machine app-data root as the library
    // (LibraryDatabase::defaultLibraryPath): %LOCALAPPDATA%\Motiva\Motiva.
    return QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                           QStringLiteral("/backups"));
}

QString LocalBackupProvider::displayName() const {
    return QCoreApplication::translate("LocalBackupProvider", "This device");
}

QString LocalBackupProvider::location() const {
    return QDir::toNativeSeparators(m_root);
}

bool LocalBackupProvider::isAvailable(QString* reason) const {
    if (!QDir().mkpath(m_root + QStringLiteral("/objects")) || !QDir().mkpath(m_root + QStringLiteral("/tmp"))) {
        if (reason) {
            *reason = QCoreApplication::translate("LocalBackupProvider", "The backup folder %1 can't be created.")
                          .arg(location());
        }
        return false;
    }
    return true;
}

QString LocalBackupProvider::objectPath(const QString& objectId) const {
    // Object ids are "objects/xx/<name>" - reject anything that could leave
    // the objects folder.
    if (!objectId.startsWith(QLatin1String("objects/")) || objectId.contains(QLatin1String("..")) ||
        objectId.contains(QLatin1Char('\\')) || objectId.contains(QLatin1Char(':'))) {
        return QString();
    }
    const QString path = QDir::cleanPath(m_root + QLatin1Char('/') + objectId);
    return path.startsWith(QDir::cleanPath(m_root) + QStringLiteral("/objects/"), Qt::CaseInsensitive) ? path : QString();
}

qint64 LocalBackupProvider::usedBytes() const {
    qint64 total = 0;
    for (const char* sub : {"/objects", "/tmp"}) {
        QDirIterator it(m_root + QLatin1String(sub), QDir::Files | QDir::NoSymLinks | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            total += it.fileInfo().size();
        }
    }
    return total;
}

void LocalBackupProvider::clearPartialFiles() {
    QDirIterator it(m_root + QStringLiteral("/tmp"), {QStringLiteral("*.partial")}, QDir::Files | QDir::NoSymLinks);
    while (it.hasNext()) {
        QFile::remove(it.next());
    }
}

bool LocalBackupProvider::verify(const QString& objectId, qint64 expectedSize) const {
    const QString path = objectPath(objectId);
    const QFileInfo fi(path);
    return !path.isEmpty() && fi.isFile() && !fi.isSymLink() && (expectedSize < 0 || fi.size() == expectedSize);
}

BackupResult LocalBackupProvider::store(const QString& source, const BackupHashLookup& existingByHash,
                                        const BackupProgressFn& progress, const std::atomic<bool>& cancel) {
    QString why;
    if (!isAvailable(&why)) {
        return fail(BackupError::DestinationUnavailable, why);
    }
    const QFileInfo before(source);
    if (!before.isFile()) {
        return fail(BackupError::SourceMissing, QCoreApplication::translate("LocalBackupProvider", "The original file was not found."));
    }
    const qint64 total = before.size();
    const QDateTime modifiedBefore = utcModified(before);

    // Space for the copy (and a margin), before writing a single byte.
    const QStorageInfo storage(m_root);
    if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < total + kSpaceMargin) {
        return fail(BackupError::InsufficientSpace,
                    QCoreApplication::translate("LocalBackupProvider", "Not enough free space for the backup (%1 MB needed).")
                        .arg((total + kSpaceMargin) / (1024 * 1024)));
    }

    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        return fail(before.exists() ? BackupError::SourceUnreadable : BackupError::SourceMissing,
                    QCoreApplication::translate("LocalBackupProvider", "The original can't be read: %1").arg(in.errorString()));
    }
    const QString partialPath = m_root + QStringLiteral("/tmp/") + QUuid::createUuid().toString(QUuid::WithoutBraces) +
                                QStringLiteral(".partial");
    QFile out(partialPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(BackupError::DestinationUnavailable,
                    QCoreApplication::translate("LocalBackupProvider", "The backup file can't be created: %1").arg(out.errorString()));
    }
    auto discard = [&out, &partialPath] {
        out.close();
        QFile::remove(partialPath);
    };

    QCryptographicHash hash(QCryptographicHash::Sha256);
    ProgressThrottle throttle(progress);
    QByteArray buffer(static_cast<int>(kChunk), Qt::Uninitialized);
    qint64 done = 0;
    while (true) {
        if (cancel.load()) {
            discard();
            return fail(BackupError::Cancelled, QCoreApplication::translate("LocalBackupProvider", "Cancelled."));
        }
        const qint64 n = in.read(buffer.data(), buffer.size());
        if (n < 0) {
            const bool gone = !QFileInfo::exists(source);
            const QString detail = in.errorString();
            discard();
            return fail(gone ? BackupError::SourceMissing : BackupError::SourceUnreadable,
                        QCoreApplication::translate("LocalBackupProvider", "Reading the original failed: %1").arg(detail));
        }
        if (n == 0) {
            break;
        }
        hash.addData(QByteArrayView(buffer.constData(), n));
        if (out.write(buffer.constData(), n) != n) {
            const QString detail = out.errorString();
            discard();
            return writeFailure(m_root, detail);
        }
        done += n;
        throttle.report(done, total);
    }
    if (!out.flush()) {
        const QString detail = out.errorString();
        discard();
        return writeFailure(m_root, detail);
    }
    out.close();
    in.close();

    // The original must be the same file it was when we started.
    const QFileInfo after(source);
    if (!after.isFile()) {
        QFile::remove(partialPath);
        return fail(BackupError::SourceMissing, QCoreApplication::translate("LocalBackupProvider", "The original disappeared during the copy."));
    }
    if (after.size() != total || utcModified(after) != modifiedBefore || done != total) {
        QFile::remove(partialPath);
        return fail(BackupError::SourceChanged, QCoreApplication::translate("LocalBackupProvider", "The original changed during the copy."));
    }
    if (QFileInfo(partialPath).size() != done) {
        QFile::remove(partialPath);
        return fail(BackupError::Corrupted, QCoreApplication::translate("LocalBackupProvider", "The copy is incomplete."));
    }

    BackupResult result;
    result.object.size = done;
    result.object.hash = QString::fromLatin1(hash.result().toHex());
    result.object.sourceModified = modifiedBefore;

    // Identical content already stored: keep that one, drop this copy.
    const QString existing = existingByHash ? existingByHash(result.object.hash, done) : QString();
    if (!existing.isEmpty() && verify(existing, done)) {
        QFile::remove(partialPath);
        result.object.objectId = existing;
        result.object.reused = true;
        result.ok = true;
        throttle.report(done, total, true);
        return result;
    }

    // Publish: only now does the copy become a finished object.
    const QString ext = QFileInfo(source).suffix().toLower();
    const QString objectId = QStringLiteral("objects/%1/%2%3")
                                 .arg(result.object.hash.left(2), result.object.hash,
                                      ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
    const QString finalPath = m_root + QLatin1Char('/') + objectId;
    QDir().mkpath(QFileInfo(finalPath).absolutePath());
    if (QFileInfo::exists(finalPath)) {
        // Same hash already on disk (e.g. a record not yet in the index): it is the same content.
        QFile::remove(partialPath);
    } else if (!QFile::rename(partialPath, finalPath)) {
        QFile::remove(partialPath);
        return writeFailure(m_root, QCoreApplication::translate("LocalBackupProvider", "the finished copy could not be saved"));
    }
    result.object.objectId = objectId;
    result.ok = true;
    throttle.report(done, total, true);
    return result;
}

BackupResult LocalBackupProvider::restore(const QString& objectId, const QString& destination, const QString& expectedHash,
                                          const QDateTime& setModified, const BackupProgressFn& progress,
                                          const std::atomic<bool>& cancel) {
    const QString path = objectPath(objectId);
    QFile in(path);
    if (path.isEmpty() || !in.open(QIODevice::ReadOnly)) {
        return fail(BackupError::Corrupted, QCoreApplication::translate("LocalBackupProvider", "The backup copy is missing."));
    }
    const QFileInfo dest(destination);
    if (dest.exists()) {
        return fail(BackupError::AlreadyExists,
                    QCoreApplication::translate("LocalBackupProvider", "A file already exists at %1 - Motiva never overwrites files.")
                        .arg(QDir::toNativeSeparators(destination)));
    }
    if (!QDir().mkpath(dest.absolutePath())) {
        return fail(BackupError::DestinationUnavailable,
                    QCoreApplication::translate("LocalBackupProvider", "The folder %1 can't be created.")
                        .arg(QDir::toNativeSeparators(dest.absolutePath())));
    }
    const qint64 total = in.size();
    const QStorageInfo storage(dest.absolutePath());
    if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < total + kSpaceMargin) {
        return fail(BackupError::InsufficientSpace,
                    QCoreApplication::translate("LocalBackupProvider", "Not enough free space to restore (%1 MB needed).")
                        .arg((total + kSpaceMargin) / (1024 * 1024)));
    }
    // Written beside the destination, then renamed into place: never a
    // half-written file at the real name.
    const QString partialPath = destination + QStringLiteral(".motiva-restore.partial");
    QFile out(partialPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(BackupError::DestinationUnavailable,
                    QCoreApplication::translate("LocalBackupProvider", "The restored file can't be created: %1").arg(out.errorString()));
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    ProgressThrottle throttle(progress);
    QByteArray buffer(static_cast<int>(kChunk), Qt::Uninitialized);
    qint64 done = 0;
    while (true) {
        if (cancel.load()) {
            out.close();
            QFile::remove(partialPath);
            return fail(BackupError::Cancelled, QCoreApplication::translate("LocalBackupProvider", "Cancelled."));
        }
        const qint64 n = in.read(buffer.data(), buffer.size());
        if (n < 0) {
            out.close();
            QFile::remove(partialPath);
            return fail(BackupError::Corrupted, QCoreApplication::translate("LocalBackupProvider", "The backup copy can't be read: %1").arg(in.errorString()));
        }
        if (n == 0) {
            break;
        }
        hash.addData(QByteArrayView(buffer.constData(), n));
        if (out.write(buffer.constData(), n) != n) {
            const QString detail = out.errorString();
            out.close();
            QFile::remove(partialPath);
            return writeFailure(dest.absolutePath(), detail);
        }
        done += n;
        throttle.report(done, total);
    }
    out.flush();
    out.close();
    const QString actual = QString::fromLatin1(hash.result().toHex());
    if (!expectedHash.isEmpty() && actual != expectedHash) {
        QFile::remove(partialPath);
        return fail(BackupError::Corrupted, QCoreApplication::translate("LocalBackupProvider", "The backup copy is damaged (its checksum no longer matches)."));
    }
    if (QFileInfo::exists(destination) || !QFile::rename(partialPath, destination)) {
        QFile::remove(partialPath);
        return fail(BackupError::AlreadyExists,
                    QCoreApplication::translate("LocalBackupProvider", "A file appeared at %1 while restoring - it was not overwritten.")
                        .arg(QDir::toNativeSeparators(destination)));
    }
    if (setModified.isValid()) {
        QFile restored(destination);
        if (restored.open(QIODevice::ReadWrite)) { // Windows needs write access to set the time
            restored.setFileTime(setModified, QFileDevice::FileModificationTime);
        }
    }
    BackupResult result;
    result.ok = true;
    result.object = {objectId, done, actual, setModified, false};
    throttle.report(done, total, true);
    return result;
}

QString LocalBackupProvider::removeObjects(const QStringList& objectIds) {
    QStringList errors;
    for (const QString& id : objectIds) {
        const QString path = objectPath(id);
        if (path.isEmpty()) {
            errors << QCoreApplication::translate("LocalBackupProvider", "Refused (not a backup object): %1").arg(id);
            continue;
        }
        QFile f(path);
        if (f.exists() && !f.remove()) {
            errors << QCoreApplication::translate("LocalBackupProvider", "%1 could not be deleted (%2)").arg(QFileInfo(path).fileName(), f.errorString());
            continue;
        }
        QDir().rmdir(QFileInfo(path).absolutePath()); // only succeeds when empty
    }
    return errors.join(QLatin1Char('\n'));
}

QString LocalBackupProvider::removeEverything() {
    // Only the two folders this provider writes to, file by file, never
    // following links; then the (now empty) folders themselves.
    QStringList errors;
    const QString root = QDir::cleanPath(m_root);
    for (const char* sub : {"/objects", "/tmp"}) {
        const QString dir = root + QLatin1String(sub);
        QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            if (!QDir::cleanPath(file).startsWith(dir + QLatin1Char('/'), Qt::CaseInsensitive)) {
                continue;
            }
            QFile f(file);
            if (!f.remove()) {
                errors << QCoreApplication::translate("LocalBackupProvider", "%1 could not be deleted (%2)").arg(QFileInfo(file).fileName(), f.errorString());
            }
        }
        QDirIterator dirs(dir, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
        QStringList subdirs;
        while (dirs.hasNext()) {
            subdirs << dirs.next();
        }
        std::sort(subdirs.begin(), subdirs.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
        for (const QString& d : std::as_const(subdirs)) {
            QDir().rmdir(d); // empty folders only
        }
        QDir().rmdir(dir);
    }
    QDir().rmdir(root); // only if nothing else is in it
    return errors.join(QLatin1Char('\n'));
}
