#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

// Where backup copies of media live. BackupManager (the rest of Motiva)
// talks only to this interface; LocalBackupProvider stores copies in
// Motiva's own folder on this device. A future cloud provider can implement
// the same interface without touching the media/playlist architecture.
//
// Providers move FILES only - they never touch the .mtv database (SQLite
// connections belong to the GUI thread); BackupManager records the results.
// Every method is safe to call from a worker thread.

struct BackupError {
    enum Code {
        None,
        SourceMissing,        // original not found / disappeared during the copy
        SourceUnreadable,     // permission denied, locked, I/O error reading it
        SourceChanged,        // original changed while it was being copied
        DestinationUnavailable,
        InsufficientSpace,
        WriteFailed,
        Corrupted,            // a stored copy no longer matches its recorded hash/size
        AlreadyExists,        // restore: never overwrite an existing file
        Cancelled,
    };
    Code code = None;
    QString message; // user-facing
};

struct BackupObject {
    QString objectId;       // provider-defined handle for the stored copy
    qint64 size = 0;
    QString hash;           // SHA-256 of the copied bytes
    QDateTime sourceModified; // original's modified time (UTC) when copied
    bool reused = false;    // identical content was already stored - nothing new written
};

struct BackupResult {
    bool ok = false;
    BackupError error;
    BackupObject object;
};

using BackupProgressFn = std::function<void(qint64 done, qint64 total)>;
// Returns the object id already holding content with this hash and size, or
// an empty string.
using BackupHashLookup = std::function<QString(const QString& hash, qint64 size)>;

class BackupProvider {
public:
    virtual ~BackupProvider() = default;

    virtual QString id() const = 0;          // stored in the library ("local")
    virtual QString displayName() const = 0; // "This device"
    virtual QString location() const = 0;    // human-readable place (folder path)
    virtual bool isAvailable(QString* reason) const = 0;

    // READS `source` (never modifies, moves or deletes it) and stores a copy.
    // The copy only becomes visible as a finished object once it is complete,
    // verified and the original is confirmed unchanged; any failure or
    // cancellation leaves no partial object behind.
    virtual BackupResult store(const QString& source, const BackupHashLookup& existingByHash,
                               const BackupProgressFn& progress, const std::atomic<bool>& cancel) = 0;

    // Cheap check that the stored object exists with the expected size.
    virtual bool verify(const QString& objectId, qint64 expectedSize) const = 0;

    // Copies a stored object to `destination`, verifying its hash. Never
    // overwrites: fails with AlreadyExists if `destination` exists.
    virtual BackupResult restore(const QString& objectId, const QString& destination, const QString& expectedHash,
                                 const QDateTime& setModified, const BackupProgressFn& progress,
                                 const std::atomic<bool>& cancel) = 0;

    // Deletes these stored objects (explicit user action only). Returns an
    // error text, empty on success; a missing object counts as removed.
    virtual QString removeObjects(const QStringList& objectIds) = 0;
    // Deletes everything this provider stores. Same rules as above.
    virtual QString removeEverything() = 0;

    virtual qint64 usedBytes() const = 0;
};
