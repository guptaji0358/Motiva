#pragma once

#include <QDateTime>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QSqlDatabase;
class QSqlQuery;

// What a playlist holds. Stored as text ('image'/'video') so the file is
// self-describing; the schema also accepts 'mixed', reserved for a future
// mixed-media playlist UX (not exposed by Motiva yet).
enum class PlaylistType { Image, Video };

// Per-playlist automatic-change settings. Only meaningful for image
// playlists today (video playlists advance when a video ends) - stored for
// every playlist so the schema doesn't change if video triggers are added.
struct RotationSettings {
    bool onUnlock = true;
    bool onWindowsStart = true;
    bool onInterval = false;
    int intervalMinutes = 30;
};

struct PlaylistInfo {
    qint64 id = 0;
    QString name;
    PlaylistType type = PlaylistType::Image;
    int itemCount = 0;
    qint64 currentItemId = 0; // playlist_items.id, 0 = none
    RotationSettings rotation;
};

// What Motiva remembers about a media file besides its path (stored in
// media.metadata as JSON) - used to recognize the file again if it is
// moved (see MediaRecovery). size < 0 = unknown.
struct MediaFacts {
    qint64 size = -1;
    QDateTime modified; // UTC
    bool isKnown() const { return size >= 0; }
    static MediaFacts ofFile(const QString& path);
};

struct PlaylistItemRecord {
    qint64 itemId = 0;  // playlist_items.id
    qint64 mediaId = 0; // media.id
    QString path;       // native separators, absolute
    MediaFacts facts;
};

// A virtual category: a saved filter of ONE playlist's media (schema 4).
// It stores only a rule - which of the playlist's items match is worked out
// from the media's stored metadata each time - so it never copies media or
// playlist items, and media added later shows up automatically if it
// matches. "All" (every item) is built in and not stored.
struct FilterRule {
    // field: "name" (file name), "path" (full path incl. folders),
    //        "extension", "size_mb" (stored file size).
    // op:    "contains", "not_contains" (name/path), "is", "is_not"
    //        (extension), "at_least", "at_most" (size_mb).
    QString field;
    QString op;
    QString value;
    bool isEmpty() const { return value.trimmed().isEmpty(); }
};

struct FilterDefinition {
    bool matchAll = true; // all rules must match (AND) vs any rule (OR)
    QVector<FilterRule> rules;
    bool isEmpty() const;
    QString toJson() const;
    static FilterDefinition fromJson(const QString& json);
};

// Backup bookkeeping for ONE media record (schema 5). The backed-up bytes
// are a normal file owned by the backup provider (never inside SQLite);
// this row only says which object holds this media's copy and whether that
// copy is valid. One row per media record - however many playlists or
// categories reference the media. media_id becomes 0 (NULL in the table)
// when the media record itself is gone: an "unused backup", removed only
// explicitly (Cleanup & Reset).
struct MediaBackupRecord {
    qint64 id = 0;
    qint64 mediaId = 0;
    QString provider;     // "local" today; a cloud provider can be added later
    QString status;       // "complete" | "failed"
    QString objectId;     // provider-defined handle (local: path under the backup root)
    QString originalPath; // where the original was when it was backed up
    qint64 size = -1;
    QString sourceModified; // original's modified time (UTC ISO) when copied
    QString contentHash;    // SHA-256 of the copied bytes
    QString backedUpAt;
    QString lastError;
    bool isComplete() const { return status == QLatin1String("complete"); }
};

// A media record together with its backup row (backup.id == 0: none yet).
struct BackupCandidate {
    qint64 mediaId = 0;
    QString path;
    MediaBackupRecord backup;
};

struct BackupStats {
    int complete = 0;      // media with a valid backup
    int failed = 0;        // last attempt failed
    int pending = 0;       // media with no backup row yet
    int unused = 0;        // backups whose media record no longer exists
    qint64 bytes = 0;      // size of distinct backup objects
    qint64 pendingBytes = 0; // stored size of media still to back up
};

struct PlaylistFilterInfo {
    qint64 id = 0;
    qint64 playlistId = 0;
    QString name;
    FilterDefinition definition;
};

// The Motiva library: a .mtv file, which is a SQLite database (Qt SQL's
// bundled SQLite driver - no separate SQLite dependency). It holds only
// playlist structure, order, media REFERENCES (paths) and metadata - never
// the media bytes themselves, so a .mtv can be copied/backed up and opened
// elsewhere, with missing paths simply showing as unavailable.
//
// Robustness rules:
//  - every query is prepared with bound values (paths/names are never
//    concatenated into SQL);
//  - every multi-step change runs in one transaction (Transaction below),
//    rolled back on any failure, so the file is never left half-written;
//  - PRAGMA foreign_keys=ON per connection; rollback journal (not WAL) so
//    the single .mtv file is always complete and safe to copy;
//  - schema version in PRAGMA user_version (+ library_metadata), with a
//    migration step table for future versions;
//  - an unreadable/corrupt file is renamed aside (never deleted) and a
//    fresh library is created; a file from a NEWER Motiva is left untouched
//    and the session runs on a temporary in-memory library instead.
class LibraryDatabase {
public:
    enum class OpenResult {
        Opened,          // existing library opened (and migrated if needed)
        Created,         // no file yet - new library created
        RecoveredCorrupt,// file was unreadable; moved aside, new library created
        NewerSchema,     // file is from a newer Motiva - not modified; in-memory session
        Failed           // could not open/create at all - in-memory session
    };

    // 1: playlists. 4: + virtual categories as per-playlist saved filters
    // (playlist_categories). 5: + media_backups. 2 and 3 only ever existed in
    // unreleased development builds (media groups, then playlist groups);
    // both are migrated forward.
    static constexpr int kSchemaVersion = 5;
    // PRAGMA application_id marking a SQLite file as a Motiva library ("MTV1").
    static constexpr int kApplicationId = 0x4D545631;

    LibraryDatabase();
    ~LibraryDatabase();
    LibraryDatabase(const LibraryDatabase&) = delete;
    LibraryDatabase& operator=(const LibraryDatabase&) = delete;

    // %LOCALAPPDATA%\Motiva\Motiva\libraries\MotivaLibrary.mtv
    static QString defaultLibraryPath();

    OpenResult open(const QString& path);
    void close();
    bool isPersistent() const { return m_persistent; } // false for the in-memory fallback
    QString path() const { return m_path; }
    QString lastError() const { return m_lastError; }
    QString movedAsidePath() const { return m_movedAsidePath; } // set by RecoveredCorrupt

    // --- playlists ---
    QVector<PlaylistInfo> playlists();
    qint64 createPlaylist(const QString& name, PlaylistType type); // 0 on failure
    bool renamePlaylist(qint64 id, const QString& name);
    bool deletePlaylist(qint64 id);
    bool setRotation(qint64 id, const RotationSettings& rotation);
    bool setCurrentItem(qint64 playlistId, qint64 itemId);

    // --- items (ordered) ---
    QVector<PlaylistItemRecord> items(qint64 playlistId);
    // Inserts `paths` (in order) at `position` (0-based; clamped). Returns
    // the new records in order; empty + lastError on failure.
    QVector<PlaylistItemRecord> insertItems(qint64 playlistId, const QStringList& paths,
                                            const QString& mediaType, int position);
    bool removeItem(qint64 playlistId, qint64 itemId);
    bool clearItems(qint64 playlistId);
    // Rewrites every item's position to match `itemIdsInOrder`.
    bool setOrder(qint64 playlistId, const QVector<qint64>& itemIdsInOrder);
    qint64 currentItemId(qint64 playlistId);

    // --- media records (shared by every playlist that references a file) ---
    // Records size/modified for media whose facts aren't known yet (one
    // transaction). Pairs are (media id, current path).
    bool backfillMediaFacts(const QVector<QPair<qint64, QString>>& media);
    // Points a media record at the file's new location (after the user
    // found a moved file). Every playlist referencing the record follows.
    // If `newPath` is already a different media record in the library, the
    // two are merged into that one (references moved, a playlist that held
    // both keeps a single item) instead of creating a duplicate. Returns
    // the id of the record now holding the file, 0 on failure. Never
    // touches files on disk.
    qint64 relocateMedia(qint64 mediaId, const QString& newPath);

    // --- virtual categories: saved filters of one playlist (schema 4) ---
    QVector<PlaylistFilterInfo> playlistFilters(qint64 playlistId); // in creation order
    qint64 createPlaylistFilter(qint64 playlistId, const QString& name, const FilterDefinition& definition);
    bool updatePlaylistFilter(qint64 filterId, const QString& name, const FilterDefinition& definition);
    // Deletes the saved filter only - never media or playlist items.
    bool deletePlaylistFilter(qint64 filterId);
    // playlist_items.id of the playlist's items that pass `definition`
    // (nullptr = All) and contain every word of `search` in their file name
    // or path. Evaluated in SQL on stored metadata - no file is opened.
    QSet<qint64> matchingItems(qint64 playlistId, const FilterDefinition* definition, const QString& search);

    // --- media backup bookkeeping (schema 5; see BackupManager) ---
    enum class BackupScan { Pending, PendingAndFailed, All };
    // Media records with their backup row, filtered by what a backup run
    // should look at: Pending = no row yet; PendingAndFailed adds failed
    // ones; All also returns complete ones (to notice changed originals).
    QVector<BackupCandidate> backupCandidates(BackupScan scan);
    MediaBackupRecord backupFor(qint64 mediaId);
    bool saveBackup(const MediaBackupRecord& record); // upsert by media id
    bool saveBackupFailure(qint64 mediaId, const QString& originalPath, const QString& error);
    BackupStats backupStats();
    // "hash:size" -> object id of complete backups, to reuse identical content.
    QHash<QString, QString> backupHashIndex();
    // Object ids held ONLY by unused backups (nothing live shares them).
    QStringList unusedBackupObjects();
    // How many backup rows (live or unused) point at this object.
    int backupObjectReferences(const QString& objectId);
    bool deleteUnusedBackupRows();
    bool deleteAllBackupRows();

    // --- library-level key/value state (active playlist etc.) ---
    QString meta(const QString& key, const QString& fallback = QString());
    bool setMeta(const QString& key, const QString& value);

    // Writes a complete, self-contained copy of the library to `path`
    // (SQLite VACUUM INTO). Media files are referenced, not copied.
    bool exportTo(const QString& path);
    // Copies every playlist from another .mtv into this library (new ids,
    // names made unique). Returns the number of playlists imported, -1 on
    // error (nothing is changed then).
    int importFrom(const QString& path);

    static QString typeToString(PlaylistType type);
    static PlaylistType typeFromString(const QString& text);
    // Lowercase, cleaned absolute path - the case-insensitive identity of a
    // media file on Windows (media.path_key, UNIQUE).
    static QString pathKey(const QString& path);

private:
    class Transaction;

    QSqlDatabase db() const;
    bool openConnection(const QString& path);
    bool configureConnection();
    bool initializeOrMigrate(bool freshFile);
    bool createSchemaV1();
    bool migrateV2toV3();
    bool migrateV3toV4();
    bool migrateV4toV5();
    bool migrateV1toV4();
    bool createV3CategoryTables(QSqlQuery& q);
    bool createFilterTable(QSqlQuery& q);
    int queryInt(const QString& sql, int fallback);
    bool check(QSqlQuery& query, const char* what);
    bool fail(const QString& message);
    void touch(); // library_metadata.updated_at
    QString uniquePlaylistName(const QString& wanted);
    bool renumberPositions(qint64 playlistId);
    static QString factsJson(const MediaFacts& facts);
    static MediaFacts factsFromJson(const QString& json);

    QString m_connectionName;
    QString m_path;
    QString m_lastError;
    QString m_movedAsidePath;
    bool m_persistent = false;
};
