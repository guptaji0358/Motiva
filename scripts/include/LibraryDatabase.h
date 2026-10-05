#pragma once

#include <QDateTime>
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

// A playlist category (schema 3): an organizational group of existing
// playlists, for finding them when there are many. It holds no media and
// is never played - category_playlists only links category ids to
// playlist ids, so a playlist can be in several categories (or none) and a
// rename shows everywhere.
struct CategoryInfo {
    qint64 id = 0;
    QString name;
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

    // 1: playlists. 3: + playlist categories (categories,
    // category_playlists). 2 only ever existed in an unreleased development
    // build (media-based categories); it is migrated forward to 3.
    static constexpr int kSchemaVersion = 3;
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

    // --- playlist categories (schema 3) ---
    QVector<CategoryInfo> categories(); // in creation order
    // Every (category id, playlist id) link.
    QVector<QPair<qint64, qint64>> categoryLinks();
    qint64 createCategory(const QString& name); // name made unique; 0 on failure
    bool renameCategory(qint64 id, const QString& name);
    // Deletes the category and its links only - its playlists stay.
    bool deleteCategory(qint64 id);
    bool addPlaylistToCategory(qint64 categoryId, qint64 playlistId); // already linked = success
    bool removePlaylistFromCategory(qint64 categoryId, qint64 playlistId);
    // Ids of playlists whose name, type, or category name contains every
    // whitespace-separated term (case-insensitive), and - via
    // matchedCategories - categories whose own name matches.
    QSet<qint64> searchPlaylists(const QString& text, QSet<qint64>* matchedCategories);

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
    bool migrateV1toV3();
    bool migrateV2toV3();
    bool createCategoryTables(QSqlQuery& q);
    QString uniqueCategoryName(const QString& wanted);
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
