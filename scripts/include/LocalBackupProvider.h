#pragma once

#include "BackupProvider.h"

// Stores backup copies as ordinary files under Motiva's own app-data folder:
//
//   %LOCALAPPDATA%\Motiva\Motiva\backups\
//       objects\<first 2 hash chars>\<sha256>.<ext>   finished copies
//       tmp\<uuid>.partial                            copies in progress
//
// (the same Motiva\Motiva root the .mtv library lives under - never the
// install, deployment, build or source folders, never beside the original).
// Object ids are paths RELATIVE to that root, so the folder can be moved
// with the rest of the app data. Files are named by content hash: identical
// media is stored once however many records refer to it.
class LocalBackupProvider : public BackupProvider {
public:
    // `root` empty = the default location (see defaultRoot()).
    explicit LocalBackupProvider(const QString& root = QString());

    static QString defaultRoot();
    QString root() const { return m_root; }

    QString id() const override { return QStringLiteral("local"); }
    QString displayName() const override;
    QString location() const override;
    bool isAvailable(QString* reason) const override;

    BackupResult store(const QString& source, const BackupHashLookup& existingByHash, const BackupProgressFn& progress,
                       const std::atomic<bool>& cancel) override;
    bool verify(const QString& objectId, qint64 expectedSize) const override;
    BackupResult restore(const QString& objectId, const QString& destination, const QString& expectedHash,
                         const QDateTime& setModified, const BackupProgressFn& progress,
                         const std::atomic<bool>& cancel) override;
    QString removeObjects(const QStringList& objectIds) override;
    QString removeEverything() override;
    qint64 usedBytes() const override;

    // Removes leftover *.partial files from an interrupted run (call when no
    // copy is in progress, e.g. at startup).
    void clearPartialFiles();
    // Absolute path of a stored object, or empty if the id is not a valid
    // object path inside this provider's root.
    QString objectPath(const QString& objectId) const;

private:
    QString m_root;
};
