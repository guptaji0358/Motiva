#include "LibraryDatabase.h"
#include <QJsonArray>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QTimeZone>
#include <QVariant>

// Begins a transaction on construction and rolls it back on destruction
// unless commit() succeeded - so every early `return false` in a multi-step
// change leaves the database exactly as it was.
class LibraryDatabase::Transaction {
public:
    explicit Transaction(const QSqlDatabase& db) : m_db(db) { m_active = m_db.transaction(); }
    ~Transaction() {
        if (m_active) {
            m_db.rollback();
        }
    }
    bool started() const { return m_active; }
    bool commit() {
        if (!m_active) {
            return false;
        }
        m_active = false;
        if (m_db.commit()) {
            return true;
        }
        m_db.rollback();
        return false;
    }

private:
    QSqlDatabase m_db;
    bool m_active = false;
};

namespace {
QString nowIso() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}
} // namespace

MediaFacts MediaFacts::ofFile(const QString& path) {
    MediaFacts facts;
    const QFileInfo fi(path);
    if (fi.isFile()) {
        facts.size = fi.size();
        facts.modified = fi.lastModified(QTimeZone::UTC);
    }
    return facts;
}

QString LibraryDatabase::factsJson(const MediaFacts& facts) {
    QJsonObject o;
    if (facts.isKnown()) {
        o.insert(QStringLiteral("size"), facts.size);
        o.insert(QStringLiteral("modified"), facts.modified.toString(Qt::ISODateWithMs));
    }
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

MediaFacts LibraryDatabase::factsFromJson(const QString& json) {
    MediaFacts facts;
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    if (o.contains(QStringLiteral("size"))) {
        facts.size = o.value(QStringLiteral("size")).toInteger(-1);
        facts.modified = QDateTime::fromString(o.value(QStringLiteral("modified")).toString(), Qt::ISODateWithMs);
    }
    return facts;
}

LibraryDatabase::LibraryDatabase()
    : m_connectionName(QStringLiteral("motiva-library-") + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

LibraryDatabase::~LibraryDatabase() {
    close();
}

QString LibraryDatabase::defaultLibraryPath() {
    // Per-user, per-machine data (absolute media paths are machine-specific,
    // and a SQLite file must not roam): %LOCALAPPDATA%\Motiva\Motiva -
    // never the installation, deployment or build directory.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(base).filePath(QStringLiteral("libraries/MotivaLibrary.mtv"));
}

QString LibraryDatabase::typeToString(PlaylistType type) {
    return type == PlaylistType::Video ? QStringLiteral("video") : QStringLiteral("image");
}

PlaylistType LibraryDatabase::typeFromString(const QString& text) {
    return text == QLatin1String("video") ? PlaylistType::Video : PlaylistType::Image;
}

QString LibraryDatabase::pathKey(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

QSqlDatabase LibraryDatabase::db() const {
    return QSqlDatabase::database(m_connectionName, false);
}

bool LibraryDatabase::fail(const QString& message) {
    m_lastError = message;
    qWarning() << "[Library]" << message;
    return false;
}

bool LibraryDatabase::check(QSqlQuery& query, const char* what) {
    if (query.exec()) {
        return true;
    }
    return fail(QStringLiteral("%1 failed: %2").arg(QLatin1String(what), query.lastError().text()));
}

int LibraryDatabase::queryInt(const QString& sql, int fallback) {
    QSqlQuery q(db());
    if (q.exec(sql) && q.next()) {
        return q.value(0).toInt();
    }
    return fallback;
}

void LibraryDatabase::close() {
    {
        QSqlDatabase d = db();
        if (d.isValid() && d.isOpen()) {
            d.close();
        }
    }
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    m_persistent = false;
}

bool LibraryDatabase::openConnection(const QString& path) {
    close();
    QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    d.setDatabaseName(path);
    if (!d.open()) {
        return fail(QStringLiteral("Cannot open %1: %2").arg(path, d.lastError().text()));
    }
    return configureConnection();
}

bool LibraryDatabase::configureConnection() {
    QSqlQuery q(db());
    // foreign_keys is per-connection in SQLite and off by default.
    if (!q.exec(QStringLiteral("PRAGMA foreign_keys = ON"))) {
        return fail(QStringLiteral("Cannot enable foreign keys: %1").arg(q.lastError().text()));
    }
    // A rollback journal (not WAL) keeps the whole library in the single
    // .mtv file at rest - safe to copy/back up while Motiva is closed.
    q.exec(QStringLiteral("PRAGMA journal_mode = DELETE"));
    q.exec(QStringLiteral("PRAGMA synchronous = FULL"));
    q.exec(QStringLiteral("PRAGMA busy_timeout = 3000"));
    return true;
}

LibraryDatabase::OpenResult LibraryDatabase::open(const QString& path) {
    m_lastError.clear();
    m_movedAsidePath.clear();
    m_path = path;
    QDir().mkpath(QFileInfo(path).absolutePath());
    const bool existed = QFileInfo(path).isFile() && QFileInfo(path).size() > 0;

    auto fallBackToMemory = [this](OpenResult result) {
        const QString reason = m_lastError;
        openConnection(QStringLiteral(":memory:"));
        initializeOrMigrate(true);
        m_persistent = false;
        m_lastError = reason;
        qWarning() << "[Library] Using a temporary in-memory library for this session -" << reason;
        return result;
    };

    if (openConnection(path)) {
        // SQLite opens lazily; the first real read is what detects a file
        // that isn't a database at all ("file is not a database").
        QSqlQuery qc(db());
        const bool readable = qc.exec(QStringLiteral("PRAGMA quick_check")) && qc.next() &&
                              qc.value(0).toString() == QLatin1String("ok");
        const QString readError = qc.lastError().isValid() ? qc.lastError().text() : QString();
        // Finish the statement now: an active one blocks schema changes
        // (a migration's DROP TABLE fails with "database table is locked").
        qc.finish();
        const int appId = queryInt(QStringLiteral("PRAGMA application_id"), -1);
        const int version = queryInt(QStringLiteral("PRAGMA user_version"), -1);
        const bool foreignSqlite = existed && readable && appId != 0 && appId != kApplicationId;

        if (readable && !foreignSqlite) {
            if (version > kSchemaVersion) {
                close();
                fail(QStringLiteral("%1 was created by a newer version of Motiva (library schema %2, this "
                                    "version supports %3). It was not modified.")
                         .arg(QDir::toNativeSeparators(path)).arg(version).arg(kSchemaVersion));
                return fallBackToMemory(OpenResult::NewerSchema);
            }
            if (initializeOrMigrate(!existed)) {
                m_persistent = true;
                qInfo() << "[Library]" << (existed ? "Opened" : "Created") << QDir::toNativeSeparators(path)
                        << "schema" << queryInt(QStringLiteral("PRAGMA user_version"), -1);
                return existed ? OpenResult::Opened : OpenResult::Created;
            }
        } else if (!readable) {
            fail(QStringLiteral("%1 is damaged or is not a Motiva library (%2).")
                     .arg(QDir::toNativeSeparators(path),
                          !readError.isEmpty() ? readError : QStringLiteral("integrity check failed")));
        } else {
            fail(QStringLiteral("%1 is a SQLite database but not a Motiva library.").arg(QDir::toNativeSeparators(path)));
        }
    }

    // Unreadable or not ours: keep the original bytes (rename aside, never
    // delete) and start a fresh library at the normal path.
    close();
    if (existed) {
        const QFileInfo fi(path);
        const QString aside = fi.absoluteDir().filePath(
            QStringLiteral("%1.unreadable-%2.mtv")
                .arg(fi.completeBaseName(), QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
        if (!QFile::rename(path, aside)) {
            fail(QStringLiteral("%1 could not be read, and could not be moved aside to %2.")
                     .arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(aside)));
            return fallBackToMemory(OpenResult::Failed);
        }
        m_movedAsidePath = aside;
        const QString reason = m_lastError;
        if (openConnection(path) && initializeOrMigrate(true)) {
            m_persistent = true;
            m_lastError = reason;
            qWarning() << "[Library] Moved unreadable library aside to" << QDir::toNativeSeparators(aside)
                       << "and created a new one.";
            return OpenResult::RecoveredCorrupt;
        }
    }
    if (m_lastError.isEmpty()) {
        fail(QStringLiteral("Cannot create the Motiva library at %1.").arg(QDir::toNativeSeparators(path)));
    }
    return fallBackToMemory(OpenResult::Failed);
}

bool LibraryDatabase::initializeOrMigrate(bool freshFile) {
    int version = queryInt(QStringLiteral("PRAGMA user_version"), 0);
    if (version == kSchemaVersion) {
        return true;
    }
    if (version == 0) {
        // A brand-new (empty) file. A non-empty file without a version is
        // not something this code ever wrote - refuse rather than guess.
        if (!freshFile && queryInt(QStringLiteral("SELECT count(*) FROM sqlite_master"), 0) > 0) {
            return fail(QStringLiteral("Unrecognized library contents (no schema version)."));
        }
        if (!createSchemaV1()) {
            return false;
        }
        version = 1;
    }
    // One step per version, each inside its own transaction.
    if (version == 1) {
        if (!migrateV1toV4()) {
            return false;
        }
        version = 4;
    }
    if (version == 2) {
        if (!migrateV2toV3()) {
            return false;
        }
        version = 3;
    }
    if (version == 3) {
        if (!migrateV3toV4()) {
            return false;
        }
        version = 4;
    }
    if (version == 4) {
        if (!migrateV4toV5()) {
            return false;
        }
        version = 5;
    }
    if (version == 5) {
        if (!migrateV5toV6()) {
            return false;
        }
        version = 6;
    }
    if (version == 6 || version == 7) {
        if (!migrateToV8()) {
            return false;
        }
        version = 8;
    }
    if (version == 8) {
        return migrateV8toV9();
    }
    return fail(QStringLiteral("No migration path from library schema %1.").arg(version));
}

bool LibraryDatabase::createSchemaV1() {
    Transaction tx(db());
    if (!tx.started()) {
        return fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
    }
    const QStringList statements = {
        QStringLiteral(
            "CREATE TABLE library_metadata ("
            "  key   TEXT PRIMARY KEY NOT NULL,"
            "  value TEXT)"),
        // One row per distinct media file referenced by any playlist.
        // path_key is the case-insensitive identity; path keeps the
        // original spelling. metadata is a free-form JSON object for future
        // use (duration, dimensions...). Media bytes are NEVER stored.
        QStringLiteral(
            "CREATE TABLE media ("
            "  id       INTEGER PRIMARY KEY,"
            "  path     TEXT NOT NULL,"
            "  path_key TEXT NOT NULL UNIQUE,"
            "  type     TEXT NOT NULL CHECK (type IN ('image','video')),"
            "  name     TEXT NOT NULL,"
            "  metadata TEXT NOT NULL DEFAULT '{}')"),
        QStringLiteral(
            "CREATE TABLE playlists ("
            "  id                  INTEGER PRIMARY KEY,"
            "  name                TEXT NOT NULL,"
            "  type                TEXT NOT NULL CHECK (type IN ('image','video','mixed')),"
            "  position            INTEGER NOT NULL,"
            "  current_item_id     INTEGER REFERENCES playlist_items(id) ON DELETE SET NULL,"
            "  rotate_on_unlock    INTEGER NOT NULL DEFAULT 1,"
            "  rotate_on_start     INTEGER NOT NULL DEFAULT 1,"
            "  rotate_on_interval  INTEGER NOT NULL DEFAULT 0,"
            "  interval_minutes    INTEGER NOT NULL DEFAULT 30,"
            "  created_at          TEXT NOT NULL,"
            "  updated_at          TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE playlist_items ("
            "  id          INTEGER PRIMARY KEY,"
            "  playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
            "  media_id    INTEGER NOT NULL REFERENCES media(id) ON DELETE RESTRICT,"
            "  position    INTEGER NOT NULL,"
            "  added_at    TEXT NOT NULL,"
            "  UNIQUE (playlist_id, media_id))"),
        QStringLiteral("CREATE INDEX idx_playlist_items_order ON playlist_items(playlist_id, position)"),
        QStringLiteral("CREATE INDEX idx_playlist_items_media ON playlist_items(media_id)"),
    };
    QSqlQuery q(db());
    for (const QString& sql : statements) {
        if (!q.exec(sql)) {
            return fail(QStringLiteral("Schema creation failed: %1").arg(q.lastError().text()));
        }
    }
    QSqlQuery meta(db());
    meta.prepare(QStringLiteral("INSERT INTO library_metadata(key, value) VALUES (?, ?)"));
    const QString now = nowIso();
    const QList<QPair<QString, QString>> rows = {
        {QStringLiteral("schema_version"), QStringLiteral("1")},
        {QStringLiteral("created_at"), now},
        {QStringLiteral("updated_at"), now},
        {QStringLiteral("format"), QStringLiteral("Motiva playlist library (.mtv, SQLite)")},
    };
    for (const auto& row : rows) {
        meta.addBindValue(row.first);
        meta.addBindValue(row.second);
        if (!check(meta, "metadata insert")) {
            return false;
        }
    }
    if (!q.exec(QStringLiteral("PRAGMA application_id = %1").arg(kApplicationId)) ||
        !q.exec(QStringLiteral("PRAGMA user_version = 1"))) {
        return fail(QStringLiteral("Cannot stamp schema version: %1").arg(q.lastError().text()));
    }
    if (!tx.commit()) {
        return fail(QStringLiteral("Schema commit failed: %1").arg(db().lastError().text()));
    }
    return true;
}

void LibraryDatabase::touch() {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("INSERT INTO library_metadata(key, value) VALUES ('updated_at', ?) "
                             "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(nowIso());
    q.exec();
}

QString LibraryDatabase::meta(const QString& key, const QString& fallback) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT value FROM library_metadata WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return fallback;
}

bool LibraryDatabase::setMeta(const QString& key, const QString& value) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("INSERT INTO library_metadata(key, value) VALUES (?, ?) "
                             "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    return check(q, "setMeta");
}

QVector<PlaylistInfo> LibraryDatabase::playlists() {
    QVector<PlaylistInfo> result;
    QSqlQuery q(db());
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral(
            "SELECT p.id, p.name, p.type, COALESCE(p.current_item_id, 0), p.rotate_on_unlock, p.rotate_on_start,"
            "       p.rotate_on_interval, p.interval_minutes,"
            "       (SELECT count(*) FROM playlist_items i WHERE i.playlist_id = p.id)"
            "  FROM playlists p ORDER BY p.position, p.id"))) {
        fail(QStringLiteral("Loading playlists failed: %1").arg(q.lastError().text()));
        return result;
    }
    while (q.next()) {
        PlaylistInfo info;
        info.id = q.value(0).toLongLong();
        info.name = q.value(1).toString();
        info.type = typeFromString(q.value(2).toString());
        info.currentItemId = q.value(3).toLongLong();
        info.rotation.onUnlock = q.value(4).toBool();
        info.rotation.onWindowsStart = q.value(5).toBool();
        info.rotation.onInterval = q.value(6).toBool();
        info.rotation.intervalMinutes = q.value(7).toInt();
        info.itemCount = q.value(8).toInt();
        result.push_back(info);
    }
    return result;
}

QString LibraryDatabase::uniquePlaylistName(const QString& wanted) {
    QSet<QString> existing;
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("SELECT name FROM playlists"))) {
        while (q.next()) {
            existing.insert(q.value(0).toString().toCaseFolded());
        }
    }
    QString name = wanted.trimmed().isEmpty() ? QStringLiteral("Playlist") : wanted.trimmed();
    if (!existing.contains(name.toCaseFolded())) {
        return name;
    }
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(name).arg(n);
        if (!existing.contains(candidate.toCaseFolded())) {
            return candidate;
        }
    }
}

qint64 LibraryDatabase::createPlaylist(const QString& name, PlaylistType type) {
    Transaction tx(db());
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO playlists(name, type, position, created_at, updated_at) "
        "VALUES (?, ?, (SELECT COALESCE(MAX(position), -1) + 1 FROM playlists), ?, ?)"));
    const QString now = nowIso();
    q.addBindValue(uniquePlaylistName(name));
    q.addBindValue(typeToString(type));
    q.addBindValue(now);
    q.addBindValue(now);
    if (!tx.started() || !check(q, "createPlaylist")) {
        return 0;
    }
    const qint64 id = q.lastInsertId().toLongLong();
    touch();
    return tx.commit() ? id : (fail(QStringLiteral("createPlaylist commit failed")), 0);
}

bool LibraryDatabase::renamePlaylist(qint64 id, const QString& name) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE playlists SET name = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(name.trimmed());
    q.addBindValue(nowIso());
    q.addBindValue(id);
    return check(q, "renamePlaylist");
}

bool LibraryDatabase::deletePlaylist(qint64 id) {
    Transaction tx(db());
    QSqlQuery q(db());
    // current_item_id references an item of this playlist; clear it first so
    // the cascade on playlist_items has nothing pointing back at it.
    q.prepare(QStringLiteral("UPDATE playlists SET current_item_id = NULL WHERE id = ?"));
    q.addBindValue(id);
    if (!tx.started() || !check(q, "deletePlaylist (current)")) {
        return false;
    }
    QSqlQuery del(db());
    del.prepare(QStringLiteral("DELETE FROM playlists WHERE id = ?")); // items cascade
    del.addBindValue(id);
    if (!check(del, "deletePlaylist")) {
        return false;
    }
    QSqlQuery orphans(db());
    if (!orphans.exec(QStringLiteral(
            "DELETE FROM media WHERE NOT EXISTS (SELECT 1 FROM playlist_items i WHERE i.media_id = media.id)"))) {
        return fail(QStringLiteral("Media cleanup failed: %1").arg(orphans.lastError().text()));
    }
    touch();
    return tx.commit();
}

bool LibraryDatabase::setRotation(qint64 id, const RotationSettings& r) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE playlists SET rotate_on_unlock = ?, rotate_on_start = ?, rotate_on_interval = ?,"
                             " interval_minutes = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(r.onUnlock ? 1 : 0);
    q.addBindValue(r.onWindowsStart ? 1 : 0);
    q.addBindValue(r.onInterval ? 1 : 0);
    q.addBindValue(r.intervalMinutes);
    q.addBindValue(nowIso());
    q.addBindValue(id);
    return check(q, "setRotation");
}

bool LibraryDatabase::setCurrentItem(qint64 playlistId, qint64 itemId) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE playlists SET current_item_id = ? WHERE id = ?"));
    q.addBindValue(itemId > 0 ? QVariant(itemId) : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(playlistId);
    return check(q, "setCurrentItem");
}

QVector<PlaylistItemRecord> LibraryDatabase::items(qint64 playlistId) {
    QVector<PlaylistItemRecord> result;
    QSqlQuery q(db());
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT i.id, m.id, m.path, m.metadata FROM playlist_items i JOIN media m ON m.id = i.media_id"
                             " WHERE i.playlist_id = ? ORDER BY i.position, i.id"));
    q.addBindValue(playlistId);
    if (!check(q, "items")) {
        return result;
    }
    while (q.next()) {
        result.push_back({q.value(0).toLongLong(), q.value(1).toLongLong(), q.value(2).toString(),
                          factsFromJson(q.value(3).toString())});
    }
    return result;
}

QVector<PlaylistItemRecord> LibraryDatabase::insertItems(qint64 playlistId, const QStringList& paths,
                                                          const QString& mediaType, int position) {
    QVector<PlaylistItemRecord> inserted;
    if (paths.isEmpty()) {
        return inserted;
    }
    Transaction tx(db());
    if (!tx.started()) {
        fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
        return {};
    }
    const int count = queryInt(QStringLiteral("SELECT count(*) FROM playlist_items WHERE playlist_id = %1")
                                   .arg(playlistId), 0); // numeric id, not user text
    position = qBound(0, position < 0 ? count : position, count);

    QSqlQuery shift(db());
    shift.prepare(QStringLiteral("UPDATE playlist_items SET position = position + ? WHERE playlist_id = ? AND position >= ?"));
    shift.addBindValue(paths.size());
    shift.addBindValue(playlistId);
    shift.addBindValue(position);
    if (!check(shift, "insertItems (shift)")) {
        return {};
    }

    QSqlQuery upsertMedia(db());
    upsertMedia.prepare(QStringLiteral("INSERT INTO media(path, path_key, type, name, metadata) VALUES (?, ?, ?, ?, ?) "
                                       "ON CONFLICT(path_key) DO UPDATE SET path = excluded.path, metadata = excluded.metadata"));
    QSqlQuery findMedia(db());
    findMedia.prepare(QStringLiteral("SELECT id FROM media WHERE path_key = ?"));
    QSqlQuery insertItem(db());
    insertItem.prepare(QStringLiteral("INSERT INTO playlist_items(playlist_id, media_id, position, added_at) VALUES (?, ?, ?, ?)"));
    const QString now = nowIso();

    for (int i = 0; i < paths.size(); ++i) {
        const QString native = QDir::toNativeSeparators(QFileInfo(paths[i]).absoluteFilePath());
        const QString key = pathKey(native);
        upsertMedia.addBindValue(native);
        upsertMedia.addBindValue(key);
        upsertMedia.addBindValue(mediaType);
        upsertMedia.addBindValue(QFileInfo(native).fileName());
        const MediaFacts facts = MediaFacts::ofFile(native);
        upsertMedia.addBindValue(factsJson(facts));
        if (!check(upsertMedia, "insertItems (media)")) {
            return {};
        }
        findMedia.addBindValue(key);
        if (!check(findMedia, "insertItems (media id)") || !findMedia.next()) {
            return {};
        }
        const qint64 mediaId = findMedia.value(0).toLongLong();
        findMedia.finish();
        insertItem.addBindValue(playlistId);
        insertItem.addBindValue(mediaId);
        insertItem.addBindValue(position + i);
        insertItem.addBindValue(now);
        if (!check(insertItem, "insertItems (item)")) {
            return {};
        }
        inserted.push_back({insertItem.lastInsertId().toLongLong(), mediaId, native, facts});
    }
    QSqlQuery upd(db());
    upd.prepare(QStringLiteral("UPDATE playlists SET updated_at = ? WHERE id = ?"));
    upd.addBindValue(now);
    upd.addBindValue(playlistId);
    upd.exec();
    touch();
    if (!tx.commit()) {
        fail(QStringLiteral("insertItems commit failed: %1").arg(db().lastError().text()));
        return {};
    }
    return inserted;
}

bool LibraryDatabase::removeItem(qint64 playlistId, qint64 itemId) {
    Transaction tx(db());
    QSqlQuery pos(db());
    pos.prepare(QStringLiteral("SELECT position FROM playlist_items WHERE id = ? AND playlist_id = ?"));
    pos.addBindValue(itemId);
    pos.addBindValue(playlistId);
    if (!tx.started() || !check(pos, "removeItem (lookup)") || !pos.next()) {
        return fail(QStringLiteral("Item %1 not found in playlist %2").arg(itemId).arg(playlistId));
    }
    const int removedPos = pos.value(0).toInt();
    pos.finish();

    QSqlQuery del(db());
    del.prepare(QStringLiteral("DELETE FROM playlist_items WHERE id = ?")); // current_item_id -> NULL via FK
    del.addBindValue(itemId);
    QSqlQuery renumber(db());
    renumber.prepare(QStringLiteral("UPDATE playlist_items SET position = position - 1 WHERE playlist_id = ? AND position > ?"));
    renumber.addBindValue(playlistId);
    renumber.addBindValue(removedPos);
    QSqlQuery orphans(db());
    if (!check(del, "removeItem") || !check(renumber, "removeItem (renumber)") ||
        !orphans.exec(QStringLiteral("DELETE FROM media WHERE NOT EXISTS "
                                     "(SELECT 1 FROM playlist_items i WHERE i.media_id = media.id)"))) {
        return false;
    }
    touch();
    return tx.commit();
}

bool LibraryDatabase::clearItems(qint64 playlistId) {
    Transaction tx(db());
    QSqlQuery cur(db());
    cur.prepare(QStringLiteral("UPDATE playlists SET current_item_id = NULL WHERE id = ?"));
    cur.addBindValue(playlistId);
    QSqlQuery del(db());
    del.prepare(QStringLiteral("DELETE FROM playlist_items WHERE playlist_id = ?"));
    del.addBindValue(playlistId);
    QSqlQuery orphans(db());
    if (!tx.started() || !check(cur, "clearItems (current)") || !check(del, "clearItems") ||
        !orphans.exec(QStringLiteral("DELETE FROM media WHERE NOT EXISTS "
                                     "(SELECT 1 FROM playlist_items i WHERE i.media_id = media.id)"))) {
        return false;
    }
    touch();
    return tx.commit();
}

bool LibraryDatabase::setOrder(qint64 playlistId, const QVector<qint64>& itemIdsInOrder) {
    Transaction tx(db());
    if (!tx.started()) {
        return fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE playlist_items SET position = ? WHERE id = ? AND playlist_id = ?"));
    for (int i = 0; i < itemIdsInOrder.size(); ++i) {
        q.addBindValue(i);
        q.addBindValue(itemIdsInOrder[i]);
        q.addBindValue(playlistId);
        if (!check(q, "setOrder")) {
            return false;
        }
        if (q.numRowsAffected() != 1) {
            return fail(QStringLiteral("setOrder: item %1 is not in playlist %2").arg(itemIdsInOrder[i]).arg(playlistId));
        }
    }
    touch();
    return tx.commit();
}

qint64 LibraryDatabase::currentItemId(qint64 playlistId) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT COALESCE(current_item_id, 0) FROM playlists WHERE id = ?"));
    q.addBindValue(playlistId);
    return (check(q, "currentItemId") && q.next()) ? q.value(0).toLongLong() : 0;
}

bool LibraryDatabase::renumberPositions(qint64 playlistId) {
    // Read the current order first, then rewrite positions 0..n-1 (runs
    // inside the caller's transaction).
    QSqlQuery read(db());
    read.prepare(QStringLiteral("SELECT id FROM playlist_items WHERE playlist_id = ? ORDER BY position, id"));
    read.addBindValue(playlistId);
    if (!check(read, "renumberPositions (read)")) {
        return false;
    }
    QVector<qint64> ids;
    while (read.next()) {
        ids << read.value(0).toLongLong();
    }
    QSqlQuery write(db());
    write.prepare(QStringLiteral("UPDATE playlist_items SET position = ? WHERE id = ?"));
    for (int i = 0; i < ids.size(); ++i) {
        write.addBindValue(i);
        write.addBindValue(ids[i]);
        if (!check(write, "renumberPositions (write)")) {
            return false;
        }
    }
    return true;
}

bool LibraryDatabase::backfillMediaFacts(const QVector<QPair<qint64, QString>>& media) {
    if (media.isEmpty()) {
        return true;
    }
    Transaction tx(db());
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE media SET metadata = ? WHERE id = ?"));
    for (const auto& m : media) {
        const MediaFacts facts = MediaFacts::ofFile(m.second);
        if (!facts.isKnown()) {
            continue;
        }
        q.addBindValue(factsJson(facts));
        q.addBindValue(m.first);
        if (!check(q, "backfillMediaFacts")) {
            return false;
        }
    }
    return tx.commit();
}

qint64 LibraryDatabase::relocateMedia(qint64 mediaId, const QString& newPath) {
    const QString native = QDir::toNativeSeparators(QFileInfo(newPath).absoluteFilePath());
    const QString key = pathKey(native);
    const QString facts = factsJson(MediaFacts::ofFile(native));
    Transaction tx(db());
    if (!tx.started()) {
        fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
        return 0;
    }
    QSqlQuery find(db());
    find.prepare(QStringLiteral("SELECT id FROM media WHERE path_key = ? AND id <> ?"));
    find.addBindValue(key);
    find.addBindValue(mediaId);
    if (!check(find, "relocateMedia (lookup)")) {
        return 0;
    }
    const qint64 existing = find.next() ? find.value(0).toLongLong() : 0;
    find.finish();

    qint64 result = mediaId;
    if (existing == 0) {
        QSqlQuery upd(db());
        upd.prepare(QStringLiteral("UPDATE media SET path = ?, path_key = ?, name = ?, metadata = ? WHERE id = ?"));
        upd.addBindValue(native);
        upd.addBindValue(key);
        upd.addBindValue(QFileInfo(native).fileName());
        upd.addBindValue(facts);
        upd.addBindValue(mediaId);
        if (!check(upd, "relocateMedia (update)")) {
            return 0;
        }
    } else {
        // The found file is already a media record. Merge into it: a
        // playlist that already holds that record drops the stale item;
        // every other reference is repointed.
        QSqlQuery dupPlaylists(db());
        dupPlaylists.prepare(QStringLiteral(
            "SELECT DISTINCT playlist_id FROM playlist_items WHERE media_id = ? AND playlist_id IN"
            " (SELECT playlist_id FROM playlist_items WHERE media_id = ?)"));
        dupPlaylists.addBindValue(mediaId);
        dupPlaylists.addBindValue(existing);
        if (!check(dupPlaylists, "relocateMedia (duplicates)")) {
            return 0;
        }
        QVector<qint64> affected;
        while (dupPlaylists.next()) {
            affected << dupPlaylists.value(0).toLongLong();
        }
        QSqlQuery dropDup(db());
        dropDup.prepare(QStringLiteral("DELETE FROM playlist_items WHERE media_id = ? AND playlist_id = ?"));
        for (qint64 pl : affected) {
            dropDup.addBindValue(mediaId);
            dropDup.addBindValue(pl);
            if (!check(dropDup, "relocateMedia (drop duplicate)") || !renumberPositions(pl)) {
                return 0;
            }
        }
        // A backup of the merged-away record moves to the surviving one when
        // that has none (otherwise it becomes an unused backup).
        QSqlQuery moveBackup(db());
        moveBackup.prepare(QStringLiteral(
            "UPDATE media_backups SET media_id = ? WHERE media_id = ? AND NOT EXISTS"
            " (SELECT 1 FROM media_backups WHERE media_id = ?)"));
        moveBackup.addBindValue(existing);
        moveBackup.addBindValue(mediaId);
        moveBackup.addBindValue(existing);
        if (!check(moveBackup, "relocateMedia (backup)")) {
            return 0;
        }
        QSqlQuery repoint(db());
        repoint.prepare(QStringLiteral("UPDATE playlist_items SET media_id = ? WHERE media_id = ?"));
        repoint.addBindValue(existing);
        repoint.addBindValue(mediaId);
        QSqlQuery dropOld(db());
        dropOld.prepare(QStringLiteral("DELETE FROM media WHERE id = ?"));
        dropOld.addBindValue(mediaId);
        QSqlQuery refresh(db());
        refresh.prepare(QStringLiteral("UPDATE media SET path = ?, metadata = ? WHERE id = ?"));
        refresh.addBindValue(native);
        refresh.addBindValue(facts);
        refresh.addBindValue(existing);
        if (!check(repoint, "relocateMedia (repoint)") || !check(dropOld, "relocateMedia (drop old)") ||
            !check(refresh, "relocateMedia (refresh)")) {
            return 0;
        }
        result = existing;
    }
    touch();
    if (!tx.commit()) {
        fail(QStringLiteral("relocateMedia commit failed: %1").arg(db().lastError().text()));
        return 0;
    }
    qInfo() << "[Library] Media" << mediaId << "relocated to" << native << (existing ? "(merged)" : "");
    return result;
}

// The schema-3 tables (playlist groups) - only needed as the intermediate
// step of the 2 -> 3 -> 4 path.
bool LibraryDatabase::createV3CategoryTables(QSqlQuery& q) {
    const QStringList statements = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS categories ("
            "  id          INTEGER PRIMARY KEY,"
            "  name        TEXT NOT NULL,"
            "  position    INTEGER NOT NULL,"
            "  created_at  TEXT NOT NULL,"
            "  updated_at  TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS category_playlists ("
            "  category_id INTEGER NOT NULL REFERENCES categories(id) ON DELETE CASCADE,"
            "  playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
            "  added_at    TEXT NOT NULL,"
            "  PRIMARY KEY (category_id, playlist_id))"),
    };
    for (const QString& sql : statements) {
        if (!q.exec(sql)) {
            return fail(QStringLiteral("Library upgrade failed: %1").arg(q.lastError().text()));
        }
    }
    return true;
}

bool LibraryDatabase::createFilterTable(QSqlQuery& q) {
    // One row per saved filter. A filter belongs to exactly one playlist and
    // goes with it; it holds a rule (filter_definition, JSON), never media.
    const QStringList statements = {
        QStringLiteral(
            "CREATE TABLE playlist_categories ("
            "  id                INTEGER PRIMARY KEY,"
            "  playlist_id       INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
            "  name              TEXT NOT NULL,"
            "  filter_definition TEXT NOT NULL,"
            "  position          INTEGER NOT NULL,"
            "  created_at        TEXT NOT NULL,"
            "  updated_at        TEXT NOT NULL)"),
        QStringLiteral("CREATE INDEX idx_playlist_categories_playlist ON playlist_categories(playlist_id, position)"),
        // A category name is unique within its playlist only - the same name
        // under two different playlists is two unrelated categories.
        QStringLiteral("CREATE UNIQUE INDEX idx_playlist_categories_name ON playlist_categories(playlist_id, name COLLATE NOCASE)"),
        QStringLiteral("UPDATE library_metadata SET value = '4' WHERE key = 'schema_version'"),
        QStringLiteral("PRAGMA user_version = 4"),
    };
    for (const QString& sql : statements) {
        if (!q.exec(sql)) {
            return fail(QStringLiteral("Library upgrade to schema 4 failed: %1").arg(q.lastError().text()));
        }
    }
    return true;
}

bool LibraryDatabase::migrateV1toV4() {
    Transaction tx(db());
    QSqlQuery q(db());
    if (!tx.started() || !createFilterTable(q)) {
        return false;
    }
    if (!tx.commit()) {
        return fail(QStringLiteral("Library upgrade commit failed: %1").arg(db().lastError().text()));
    }
    qInfo() << "[Library] Upgraded library schema 1 -> 4 (virtual categories as playlist filters).";
    return true;
}

// Schema 3 (an unreleased development build) grouped playlists into
// library-wide categories. Categories are now saved filters inside each
// playlist, so those groupings have no equivalent and are dropped; every
// playlist, item and media record stays exactly as it is.
bool LibraryDatabase::migrateV3toV4() {
    Transaction tx(db());
    QSqlQuery q(db());
    if (!tx.started()) {
        return fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
    }
    const int dropped = queryInt(QStringLiteral("SELECT count(*) FROM categories"), 0);
    if (!q.exec(QStringLiteral("DROP TABLE IF EXISTS category_playlists")) ||
        !q.exec(QStringLiteral("DROP TABLE IF EXISTS categories")) || !createFilterTable(q)) {
        return fail(QStringLiteral("Library upgrade to schema 4 failed: %1").arg(q.lastError().text()));
    }
    if (!tx.commit()) {
        return fail(QStringLiteral("Library upgrade commit failed: %1").arg(db().lastError().text()));
    }
    qInfo() << "[Library] Upgraded library schema 3 -> 4:" << dropped
            << "playlist grouping(s) removed (playlists unchanged).";
    return true;
}

bool LibraryDatabase::migrateV2toV3() {
    Transaction tx(db());
    QSqlQuery q(db());
    if (!tx.started() || !createV3CategoryTables(q)) {
        return fail(QStringLiteral("Library upgrade to schema 3 failed: %1").arg(q.lastError().text()));
    }
    const QString now = nowIso();
    QVector<QPair<qint64, QString>> oldCategories;
    if (!q.exec(QStringLiteral("SELECT id, name FROM categories ORDER BY position, id"))) {
        return fail(QStringLiteral("Library upgrade failed: %1").arg(q.lastError().text()));
    }
    while (q.next()) {
        oldCategories.push_back({q.value(0).toLongLong(), q.value(1).toString()});
    }
    int converted = 0;
    for (const auto& category : oldCategories) {
        QSqlQuery members(db());
        members.prepare(QStringLiteral("SELECT m.id, m.type FROM category_items ci JOIN media m ON m.id = ci.media_id"
                                       " WHERE ci.category_id = ? ORDER BY ci.position, ci.media_id"));
        members.addBindValue(category.first);
        if (!check(members, "upgrade (category media)")) {
            return false;
        }
        QVector<qint64> images, videos;
        while (members.next()) {
            (members.value(1).toString() == QLatin1String("video") ? videos : images) << members.value(0).toLongLong();
        }
        members.finish();
        for (const bool video : {false, true}) {
            const QVector<qint64>& ids = video ? videos : images;
            if (ids.isEmpty()) {
                continue;
            }
            QString name = category.second;
            if (!images.isEmpty() && !videos.isEmpty()) {
                name += video ? QStringLiteral(" (Videos)") : QStringLiteral(" (Images)");
            }
            QSqlQuery pl(db());
            pl.prepare(QStringLiteral("INSERT INTO playlists(name, type, position, created_at, updated_at)"
                                      " VALUES (?, ?, (SELECT COALESCE(MAX(position), -1) + 1 FROM playlists), ?, ?)"));
            pl.addBindValue(uniquePlaylistName(name));
            pl.addBindValue(video ? QStringLiteral("video") : QStringLiteral("image"));
            pl.addBindValue(now);
            pl.addBindValue(now);
            if (!check(pl, "upgrade (playlist)")) {
                return false;
            }
            const qint64 playlistId = pl.lastInsertId().toLongLong();
            QSqlQuery item(db());
            item.prepare(QStringLiteral("INSERT INTO playlist_items(playlist_id, media_id, position, added_at) VALUES (?, ?, ?, ?)"));
            for (int i = 0; i < ids.size(); ++i) {
                item.addBindValue(playlistId);
                item.addBindValue(ids[i]);
                item.addBindValue(i);
                item.addBindValue(now);
                if (!check(item, "upgrade (playlist item)")) {
                    return false;
                }
            }
            QSqlQuery link(db());
            link.prepare(QStringLiteral("INSERT INTO category_playlists(category_id, playlist_id, added_at) VALUES (?, ?, ?)"));
            link.addBindValue(category.first);
            link.addBindValue(playlistId);
            link.addBindValue(now);
            if (!check(link, "upgrade (link)")) {
                return false;
            }
            ++converted;
        }
    }
    if (!q.exec(QStringLiteral("DROP TABLE IF EXISTS category_items")) ||
        !q.exec(QStringLiteral("UPDATE library_metadata SET value = '3' WHERE key = 'schema_version'")) ||
        !q.exec(QStringLiteral("PRAGMA user_version = 3"))) {
        return fail(QStringLiteral("Library upgrade to schema 3 failed: %1").arg(q.lastError().text()));
    }
    if (!tx.commit()) {
        return fail(QStringLiteral("Library upgrade commit failed: %1").arg(db().lastError().text()));
    }
    qInfo() << "[Library] Upgraded library schema 2 -> 3:" << oldCategories.size() << "category(ies) kept,"
            << converted << "media group(s) converted to playlists inside them.";
    return true;
}

bool LibraryDatabase::migrateV4toV5() {
    Transaction tx(db());
    QSqlQuery q(db());
    if (!tx.started()) {
        return fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
    }
    const QStringList statements = {
        // One row per media record. ON DELETE SET NULL: when the media
        // record goes away the row stays as an "unused backup" (its file is
        // only ever removed explicitly) instead of vanishing silently.
        QStringLiteral(
            "CREATE TABLE media_backups ("
            "  id              INTEGER PRIMARY KEY,"
            "  media_id        INTEGER UNIQUE REFERENCES media(id) ON DELETE SET NULL,"
            "  provider        TEXT NOT NULL DEFAULT 'local',"
            "  status          TEXT NOT NULL CHECK (status IN ('complete','failed')),"
            "  object_id       TEXT,"
            "  original_path   TEXT NOT NULL,"
            "  size            INTEGER,"
            "  source_modified TEXT,"
            "  content_hash    TEXT,"
            "  backed_up_at    TEXT,"
            "  last_error      TEXT,"
            "  format_version  INTEGER NOT NULL DEFAULT 1)"),
        QStringLiteral("CREATE INDEX idx_media_backups_hash ON media_backups(content_hash, size)"),
        QStringLiteral("UPDATE library_metadata SET value = '5' WHERE key = 'schema_version'"),
        QStringLiteral("PRAGMA user_version = 5"),
    };
    for (const QString& sql : statements) {
        if (!q.exec(sql)) {
            return fail(QStringLiteral("Library upgrade to schema 5 failed: %1").arg(q.lastError().text()));
        }
    }
    if (!tx.commit()) {
        return fail(QStringLiteral("Library upgrade commit failed: %1").arg(db().lastError().text()));
    }
    qInfo() << "[Library] Upgraded library schema 4 -> 5 (media backup bookkeeping).";
    return true;
}

bool LibraryDatabase::migrateV5toV6() {
    Transaction tx(db());
    QSqlQuery q(db());
    if (!tx.started()) {
        return fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
    }
    const QStringList statements = {
        // Existing categories are all condition-based.
        QStringLiteral("ALTER TABLE playlist_categories ADD COLUMN mode TEXT NOT NULL DEFAULT 'condition'"),
        // Hand-picked members: a reference to an existing playlist item, never
        // a copy. Deleting the item (or the category/playlist) removes the row.
        QStringLiteral(
            "CREATE TABLE category_items ("
            "  category_id INTEGER NOT NULL REFERENCES playlist_categories(id) ON DELETE CASCADE,"
            "  item_id     INTEGER NOT NULL REFERENCES playlist_items(id) ON DELETE CASCADE,"
            "  PRIMARY KEY (category_id, item_id))"),
        QStringLiteral("CREATE INDEX idx_category_items_item ON category_items(item_id)"),
        QStringLiteral("UPDATE library_metadata SET value = '6' WHERE key = 'schema_version'"),
        QStringLiteral("PRAGMA user_version = 6"),
    };
    for (const QString& sql : statements) {
        if (!q.exec(sql)) {
            return fail(QStringLiteral("Library upgrade to schema 6 failed: %1").arg(q.lastError().text()));
        }
    }
    if (!tx.commit()) {
        return fail(QStringLiteral("Library upgrade commit failed: %1").arg(db().lastError().text()));
    }
    qInfo() << "[Library] Upgraded library schema 5 -> 6 (hand-picked categories).";
    return true;
}

// Schema 7 existed only in a development build that had a third category mode
// ("similar") and a fingerprint cache table. Both are removed; any category
// that mode produced keeps its saved members as an ordinary hand-picked one,
// so nothing the user built is lost. Also runs for schema 6 (nothing to do
// beyond the version bump).
bool LibraryDatabase::migrateToV8() {
    Transaction tx(db());
    QSqlQuery q(db());
    if (!tx.started()) {
        return fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
    }
    const QStringList statements = {
        QStringLiteral("DROP TABLE IF EXISTS media_features"),
        QStringLiteral("UPDATE playlist_categories SET mode = 'selected',"
                       " filter_definition = '{\"version\":1,\"match\":\"all\",\"rules\":[]}'"
                       " WHERE mode NOT IN ('condition', 'selected')"),
        QStringLiteral("UPDATE library_metadata SET value = '8' WHERE key = 'schema_version'"),
        QStringLiteral("PRAGMA user_version = 8"),
    };
    for (const QString& sql : statements) {
        if (!q.exec(sql)) {
            return fail(QStringLiteral("Library upgrade to schema 8 failed: %1").arg(q.lastError().text()));
        }
    }
    if (!tx.commit()) {
        return fail(QStringLiteral("Library upgrade commit failed: %1").arg(db().lastError().text()));
    }
    qInfo() << "[Library] Upgraded library schema to 8.";
    return true;
}

// Schema 9: where each item's node sits in the Wallpaper Flow diagram. Layout
// only - the sequence is playlist_items.position and is never derived from
// these coordinates. Rows vanish with their item (FK cascade).
bool LibraryDatabase::migrateV8toV9() {
    Transaction tx(db());
    QSqlQuery q(db());
    if (!tx.started()) {
        return fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
    }
    const QStringList statements = {
        QStringLiteral(
            "CREATE TABLE flow_layout ("
            "  item_id INTEGER PRIMARY KEY REFERENCES playlist_items(id) ON DELETE CASCADE,"
            "  x       REAL NOT NULL,"
            "  y       REAL NOT NULL)"),
        QStringLiteral("UPDATE library_metadata SET value = '9' WHERE key = 'schema_version'"),
        QStringLiteral("PRAGMA user_version = 9"),
    };
    for (const QString& sql : statements) {
        if (!q.exec(sql)) {
            return fail(QStringLiteral("Library upgrade to schema 9 failed: %1").arg(q.lastError().text()));
        }
    }
    if (!tx.commit()) {
        return fail(QStringLiteral("Library upgrade commit failed: %1").arg(db().lastError().text()));
    }
    qInfo() << "[Library] Upgraded library schema 8 -> 9 (Wallpaper Flow node layout).";
    return true;
}

QHash<qint64, QPointF> LibraryDatabase::flowPositions(qint64 playlistId) {
    QHash<qint64, QPointF> result;
    QSqlQuery q(db());
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT f.item_id, f.x, f.y FROM flow_layout f"
                             " JOIN playlist_items i ON i.id = f.item_id WHERE i.playlist_id = ?"));
    q.addBindValue(playlistId);
    if (check(q, "flowPositions")) {
        while (q.next()) {
            result.insert(q.value(0).toLongLong(), QPointF(q.value(1).toDouble(), q.value(2).toDouble()));
        }
    }
    return result;
}

bool LibraryDatabase::saveFlowPositions(qint64 playlistId, const QHash<qint64, QPointF>& positions) {
    if (positions.isEmpty()) {
        return true;
    }
    Transaction tx(db());
    if (!tx.started()) {
        return fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
    }
    QSqlQuery q(db());
    // Only items of this playlist can be positioned.
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO flow_layout(item_id, x, y)"
                             " SELECT ?, ?, ? WHERE EXISTS (SELECT 1 FROM playlist_items WHERE id = ? AND playlist_id = ?)"));
    for (auto it = positions.constBegin(); it != positions.constEnd(); ++it) {
        q.addBindValue(it.key());
        q.addBindValue(it.value().x());
        q.addBindValue(it.value().y());
        q.addBindValue(it.key());
        q.addBindValue(playlistId);
        if (!check(q, "saveFlowPositions")) {
            return false;
        }
    }
    touch();
    return tx.commit();
}

bool LibraryDatabase::clearFlowPositions(qint64 playlistId) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM flow_layout WHERE item_id IN (SELECT id FROM playlist_items WHERE playlist_id = ?)"));
    q.addBindValue(playlistId);
    if (!check(q, "clearFlowPositions")) {
        return false;
    }
    touch();
    return true;
}

// ------------------------------------------------------------ media backups

static MediaBackupRecord backupFromQuery(const QSqlQuery& q, int first) {
    MediaBackupRecord r;
    r.id = q.value(first).toLongLong();
    r.mediaId = q.value(first + 1).toLongLong();
    r.provider = q.value(first + 2).toString();
    r.status = q.value(first + 3).toString();
    r.objectId = q.value(first + 4).toString();
    r.originalPath = q.value(first + 5).toString();
    r.size = q.value(first + 6).isNull() ? -1 : q.value(first + 6).toLongLong();
    r.sourceModified = q.value(first + 7).toString();
    r.contentHash = q.value(first + 8).toString();
    r.backedUpAt = q.value(first + 9).toString();
    r.lastError = q.value(first + 10).toString();
    return r;
}

static const char* kBackupColumns =
    "b.id, b.media_id, b.provider, b.status, b.object_id, b.original_path, b.size, b.source_modified, b.content_hash,"
    " b.backed_up_at, b.last_error";

QVector<BackupCandidate> LibraryDatabase::backupCandidates(BackupScan scan) {
    QVector<BackupCandidate> result;
    QSqlQuery q(db());
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT m.id, m.path, %1 FROM media m LEFT JOIN media_backups b ON b.media_id = m.id"
                               " ORDER BY m.id")
                    .arg(QLatin1String(kBackupColumns)))) {
        fail(QStringLiteral("Reading backup state failed: %1").arg(q.lastError().text()));
        return result;
    }
    while (q.next()) {
        BackupCandidate c;
        c.mediaId = q.value(0).toLongLong();
        c.path = q.value(1).toString();
        if (!q.value(2).isNull()) {
            c.backup = backupFromQuery(q, 2);
        }
        const bool hasRow = c.backup.id != 0;
        const bool take = scan == BackupScan::All || !hasRow ||
                          (scan == BackupScan::PendingAndFailed && !c.backup.isComplete());
        if (take) {
            result.push_back(c);
        }
    }
    return result;
}

MediaBackupRecord LibraryDatabase::backupFor(qint64 mediaId) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT %1 FROM media_backups b WHERE b.media_id = ?").arg(QLatin1String(kBackupColumns)));
    q.addBindValue(mediaId);
    if (check(q, "backupFor") && q.next()) {
        return backupFromQuery(q, 0);
    }
    return {};
}

bool LibraryDatabase::saveBackup(const MediaBackupRecord& r) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO media_backups(media_id, provider, status, object_id, original_path, size, source_modified,"
        " content_hash, backed_up_at, last_error)"
        " VALUES (?, ?, 'complete', ?, ?, ?, ?, ?, ?, NULL)"
        " ON CONFLICT(media_id) DO UPDATE SET provider = excluded.provider, status = 'complete',"
        " object_id = excluded.object_id, original_path = excluded.original_path, size = excluded.size,"
        " source_modified = excluded.source_modified, content_hash = excluded.content_hash,"
        " backed_up_at = excluded.backed_up_at, last_error = NULL"));
    q.addBindValue(r.mediaId);
    q.addBindValue(r.provider.isEmpty() ? QStringLiteral("local") : r.provider);
    q.addBindValue(r.objectId);
    q.addBindValue(r.originalPath);
    q.addBindValue(r.size);
    q.addBindValue(r.sourceModified);
    q.addBindValue(r.contentHash);
    q.addBindValue(r.backedUpAt.isEmpty() ? nowIso() : r.backedUpAt);
    return check(q, "saveBackup");
}

bool LibraryDatabase::saveBackupFailure(qint64 mediaId, const QString& originalPath, const QString& error) {
    // A failed attempt never replaces a still-valid earlier backup.
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO media_backups(media_id, provider, status, original_path, last_error)"
        " VALUES (?, 'local', 'failed', ?, ?)"
        " ON CONFLICT(media_id) DO UPDATE SET last_error = excluded.last_error, original_path = excluded.original_path,"
        " status = CASE WHEN media_backups.status = 'complete' THEN 'complete' ELSE 'failed' END"));
    q.addBindValue(mediaId);
    q.addBindValue(originalPath);
    q.addBindValue(error);
    return check(q, "saveBackupFailure");
}

BackupStats LibraryDatabase::backupStats() {
    BackupStats s;
    s.complete = queryInt(QStringLiteral("SELECT count(*) FROM media_backups WHERE status = 'complete' AND media_id IS NOT NULL"), 0);
    s.failed = queryInt(QStringLiteral("SELECT count(*) FROM media_backups WHERE status = 'failed' AND media_id IS NOT NULL"), 0);
    s.pending = queryInt(QStringLiteral("SELECT count(*) FROM media m WHERE NOT EXISTS"
                                        " (SELECT 1 FROM media_backups b WHERE b.media_id = m.id)"), 0);
    s.unused = queryInt(QStringLiteral("SELECT count(*) FROM media_backups WHERE media_id IS NULL"), 0);
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("SELECT COALESCE(SUM(sz), 0) FROM (SELECT MAX(size) AS sz FROM media_backups"
                              " WHERE status = 'complete' AND object_id IS NOT NULL GROUP BY object_id)")) && q.next()) {
        s.bytes = q.value(0).toLongLong();
    }
    if (q.exec(QStringLiteral("SELECT COALESCE(SUM(CASE WHEN json_extract(m.metadata, '$.size') > 0"
                              " THEN json_extract(m.metadata, '$.size') ELSE 0 END), 0) FROM media m WHERE NOT EXISTS"
                              " (SELECT 1 FROM media_backups b WHERE b.media_id = m.id)")) && q.next()) {
        s.pendingBytes = q.value(0).toLongLong();
    }
    return s;
}

QHash<QString, QString> LibraryDatabase::backupHashIndex() {
    QHash<QString, QString> index;
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("SELECT content_hash, size, object_id FROM media_backups"
                              " WHERE status = 'complete' AND content_hash IS NOT NULL AND object_id IS NOT NULL"))) {
        while (q.next()) {
            index.insert(q.value(0).toString() + QLatin1Char(':') + q.value(1).toString(), q.value(2).toString());
        }
    }
    return index;
}

QStringList LibraryDatabase::unusedBackupObjects() {
    QStringList objects;
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("SELECT DISTINCT object_id FROM media_backups WHERE media_id IS NULL AND object_id IS NOT NULL"
                              " AND object_id NOT IN (SELECT object_id FROM media_backups"
                              "   WHERE media_id IS NOT NULL AND object_id IS NOT NULL)"))) {
        while (q.next()) {
            objects << q.value(0).toString();
        }
    }
    return objects;
}

int LibraryDatabase::backupObjectReferences(const QString& objectId) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT count(*) FROM media_backups WHERE object_id = ?"));
    q.addBindValue(objectId);
    return check(q, "backupObjectReferences") && q.next() ? q.value(0).toInt() : 0;
}

bool LibraryDatabase::deleteUnusedBackupRows() {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM media_backups WHERE media_id IS NULL"));
    return check(q, "deleteUnusedBackupRows");
}

bool LibraryDatabase::deleteAllBackupRows() {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM media_backups"));
    return check(q, "deleteAllBackupRows");
}

// ------------------------------------------------------- saved filters

bool FilterDefinition::isEmpty() const {
    for (const FilterRule& r : rules) {
        if (!r.isEmpty()) {
            return false;
        }
    }
    return true;
}

QString FilterDefinition::toJson() const {
    QJsonArray array;
    for (const FilterRule& r : rules) {
        if (!r.isEmpty()) {
            array.append(QJsonObject{{QStringLiteral("field"), r.field},
                                     {QStringLiteral("op"), r.op},
                                     {QStringLiteral("value"), r.value.trimmed()}});
        }
    }
    const QJsonObject root{{QStringLiteral("version"), 1},
                           {QStringLiteral("match"), matchAll ? QStringLiteral("all") : QStringLiteral("any")},
                           {QStringLiteral("rules"), array}};
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

FilterDefinition FilterDefinition::fromJson(const QString& json) {
    FilterDefinition def;
    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    def.matchAll = root.value(QStringLiteral("match")).toString() != QLatin1String("any");
    for (const QJsonValue& v : root.value(QStringLiteral("rules")).toArray()) {
        const QJsonObject o = v.toObject();
        def.rules.push_back({o.value(QStringLiteral("field")).toString(), o.value(QStringLiteral("op")).toString(),
                             o.value(QStringLiteral("value")).toString()});
    }
    return def;
}

QVector<PlaylistFilterInfo> LibraryDatabase::playlistFilters(qint64 playlistId) {
    QVector<PlaylistFilterInfo> result;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT id, name, filter_definition, mode FROM playlist_categories WHERE playlist_id = ?"
                             " ORDER BY position, id"));
    q.addBindValue(playlistId);
    if (!check(q, "playlistFilters")) {
        return result;
    }
    while (q.next()) {
        PlaylistFilterInfo info{q.value(0).toLongLong(), playlistId, q.value(1).toString(),
                                FilterDefinition::fromJson(q.value(2).toString())};
        info.mode = q.value(3).toString() == QLatin1String("selected") ? QStringLiteral("selected")
                                                                       : QStringLiteral("condition");
        result.push_back(info);
    }
    QSqlQuery members(db());
    members.setForwardOnly(true);
    members.prepare(QStringLiteral("SELECT c.category_id, c.item_id FROM category_items c"
                                   " JOIN playlist_categories k ON k.id = c.category_id WHERE k.playlist_id = ?"));
    members.addBindValue(playlistId);
    if (check(members, "playlistFilterItems")) {
        while (members.next()) {
            for (PlaylistFilterInfo& info : result) {
                if (info.id == members.value(0).toLongLong()) {
                    info.itemIds.insert(members.value(1).toLongLong());
                    break;
                }
            }
        }
    }
    return result;
}

qint64 LibraryDatabase::createPlaylistFilter(qint64 playlistId, const QString& name, const FilterDefinition& definition,
                                             const QString& mode) {
    const QString now = nowIso();
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO playlist_categories(playlist_id, name, filter_definition, mode, position, created_at, updated_at)"
        " VALUES (?, ?, ?, ?, (SELECT COALESCE(MAX(position), -1) + 1 FROM playlist_categories WHERE playlist_id = ?), ?, ?)"));
    q.addBindValue(playlistId);
    q.addBindValue(name.trimmed());
    q.addBindValue(definition.toJson());
    q.addBindValue(mode == QLatin1String("selected") ? QStringLiteral("selected") : QStringLiteral("condition"));
    q.addBindValue(playlistId);
    q.addBindValue(now);
    q.addBindValue(now);
    if (!check(q, "createPlaylistFilter")) {
        return 0;
    }
    touch();
    return q.lastInsertId().toLongLong();
}

bool LibraryDatabase::updatePlaylistFilter(qint64 filterId, const QString& name, const FilterDefinition& definition) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE playlist_categories SET name = ?, filter_definition = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(name.trimmed());
    q.addBindValue(definition.toJson());
    q.addBindValue(nowIso());
    q.addBindValue(filterId);
    if (!check(q, "updatePlaylistFilter")) {
        return false;
    }
    touch();
    return true;
}

bool LibraryDatabase::setCategoryItems(qint64 categoryId, qint64 playlistId, const QSet<qint64>& itemIds) {
    Transaction tx(db());
    if (!tx.started()) {
        return fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM category_items WHERE category_id = ?"));
    q.addBindValue(categoryId);
    if (!check(q, "setCategoryItems clear")) {
        return false;
    }
    QSqlQuery insert(db());
    // Only items of the category's own playlist can be members.
    insert.prepare(QStringLiteral("INSERT OR IGNORE INTO category_items(category_id, item_id)"
                                  " SELECT ?, id FROM playlist_items WHERE id = ? AND playlist_id = ?"));
    for (qint64 itemId : itemIds) {
        insert.addBindValue(categoryId);
        insert.addBindValue(itemId);
        insert.addBindValue(playlistId);
        if (!check(insert, "setCategoryItems insert")) {
            return false;
        }
    }
    QSqlQuery stamp(db());
    stamp.prepare(QStringLiteral("UPDATE playlist_categories SET updated_at = ? WHERE id = ?"));
    stamp.addBindValue(nowIso());
    stamp.addBindValue(categoryId);
    if (!check(stamp, "setCategoryItems stamp")) {
        return false;
    }
    if (!tx.commit()) {
        return fail(QStringLiteral("Saving the selection failed: %1").arg(db().lastError().text()));
    }
    touch();
    return true;
}

bool LibraryDatabase::deletePlaylistFilter(qint64 filterId) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM playlist_categories WHERE id = ?"));
    q.addBindValue(filterId);
    if (!check(q, "deletePlaylistFilter")) {
        return false;
    }
    touch();
    return true;
}

QSet<qint64> LibraryDatabase::matchingItems(qint64 playlistId, const FilterDefinition* definition, const QString& search) {
    auto like = [](QString term) {
        term.replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
            .replace(QLatin1Char('%'), QStringLiteral("\\%"))
            .replace(QLatin1Char('_'), QStringLiteral("\\_"));
        return term;
    };
    QString sql = QStringLiteral("SELECT i.id FROM playlist_items i JOIN media m ON m.id = i.media_id WHERE i.playlist_id = ?");
    QVariantList binds{playlistId};

    // The saved filter's rules (stored metadata only: name, path, size).
    QStringList ruleSql;
    if (definition) {
        for (const FilterRule& r : definition->rules) {
            const QString value = r.value.trimmed();
            if (value.isEmpty()) {
                continue;
            }
            if (r.field == QLatin1String("name") || r.field == QLatin1String("path")) {
                const QString column = r.field == QLatin1String("name") ? QStringLiteral("m.name") : QStringLiteral("m.path");
                ruleSql << column + (r.op == QLatin1String("not_contains") ? QStringLiteral(" NOT") : QString()) +
                               QStringLiteral(" LIKE ? ESCAPE '\\'");
                binds << QLatin1Char('%') + like(value) + QLatin1Char('%');
            } else if (r.field == QLatin1String("extension")) {
                QString ext = value;
                while (ext.startsWith(QLatin1Char('.'))) {
                    ext.remove(0, 1);
                }
                ruleSql << QStringLiteral("m.name") + (r.op == QLatin1String("is_not") ? QStringLiteral(" NOT") : QString()) +
                               QStringLiteral(" LIKE ? ESCAPE '\\'");
                binds << QStringLiteral("%.") + like(ext);
            } else if (r.field == QLatin1String("size_mb")) {
                bool ok = false;
                const double mb = value.toDouble(&ok);
                if (!ok) {
                    continue;
                }
                // Unknown size (-1, file never seen) never passes a size rule.
                ruleSql << QStringLiteral("(json_extract(m.metadata, '$.size') >= 0 AND json_extract(m.metadata, '$.size') ") +
                               (r.op == QLatin1String("at_most") ? QStringLiteral("<=") : QStringLiteral(">=")) +
                               QStringLiteral(" ?)");
                binds << mb * 1024.0 * 1024.0;
            }
        }
    }
    if (!ruleSql.isEmpty()) {
        sql += QStringLiteral(" AND (") + ruleSql.join(definition->matchAll ? QStringLiteral(" AND ") : QStringLiteral(" OR ")) +
               QLatin1Char(')');
    }
    // Search inside the current view: every word in the name or path.
    for (const QString& term : search.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts).mid(0, 8)) {
        sql += QStringLiteral(" AND (m.name LIKE ? ESCAPE '\\' OR m.path LIKE ? ESCAPE '\\')");
        binds << QLatin1Char('%') + like(term) + QLatin1Char('%') << QLatin1Char('%') + like(term) + QLatin1Char('%');
    }

    QSet<qint64> ids;
    QSqlQuery q(db());
    q.setForwardOnly(true);
    q.prepare(sql);
    for (const QVariant& b : std::as_const(binds)) {
        q.addBindValue(b);
    }
    if (check(q, "matchingItems")) {
        while (q.next()) {
            ids.insert(q.value(0).toLongLong());
        }
    }
    return ids;
}

bool LibraryDatabase::exportTo(const QString& path) {
    if (QFileInfo::exists(path) && !QFile::remove(path)) {
        return fail(QStringLiteral("Cannot replace %1").arg(QDir::toNativeSeparators(path)));
    }
    QSqlQuery q(db());
    // VACUUM INTO writes a complete, compacted, consistent copy (including
    // application_id/user_version). It must run outside any transaction.
    q.prepare(QStringLiteral("VACUUM INTO ?"));
    q.addBindValue(QDir::toNativeSeparators(path));
    if (!check(q, "export")) {
        return false;
    }
    // The copy gets no backup bookkeeping: its object ids point into THIS
    // device's backup folder.
    {
        const QString exportName = m_connectionName + QStringLiteral("-export");
        {
            QSqlDatabase out = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), exportName);
            out.setDatabaseName(path);
            if (out.open()) {
                QSqlQuery clear(out);
                clear.exec(QStringLiteral("DELETE FROM media_backups"));
                out.close();
            }
        }
        QSqlDatabase::removeDatabase(exportName);
    }
    qInfo() << "[Library] Exported library to" << QDir::toNativeSeparators(path);
    return true;
}

int LibraryDatabase::importFrom(const QString& path) {
    if (QFileInfo(path).absoluteFilePath().compare(QFileInfo(m_path).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
        fail(QStringLiteral("That file is the library Motiva is already using."));
        return -1;
    }
    // Read the other library through its own read-only connection; nothing
    // is written to it.
    const QString srcName = m_connectionName + QStringLiteral("-import");
    struct Source {
        QString name;
        PlaylistType type;
        RotationSettings rotation;
        QStringList paths;
        int currentIndex = -1;
    };
    QVector<Source> sources;
    bool readOk = false;
    {
        QSqlDatabase src = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), srcName);
        src.setDatabaseName(path);
        src.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        bool ok = src.open();
        QSqlQuery q(src);
        if (ok) {
            ok = q.exec(QStringLiteral("PRAGMA application_id")) && q.next() && q.value(0).toInt() == kApplicationId;
        }
        int srcVersion = 0;
        if (ok && q.exec(QStringLiteral("PRAGMA user_version")) && q.next()) {
            srcVersion = q.value(0).toInt();
        }
        if (!ok || srcVersion < 1) {
            fail(QStringLiteral("%1 is not a readable Motiva library.").arg(QDir::toNativeSeparators(path)));
        } else if (srcVersion > kSchemaVersion) {
            ok = false;
            fail(QStringLiteral("%1 was created by a newer version of Motiva.").arg(QDir::toNativeSeparators(path)));
        } else {
            QSqlQuery pl(src);
            ok = pl.exec(QStringLiteral("SELECT id, name, type, COALESCE(current_item_id, 0), rotate_on_unlock,"
                                        " rotate_on_start, rotate_on_interval, interval_minutes"
                                        " FROM playlists WHERE type IN ('image','video') ORDER BY position, id"));
            while (ok && pl.next()) {
                Source s;
                s.name = pl.value(1).toString();
                s.type = typeFromString(pl.value(2).toString());
                const qint64 currentItem = pl.value(3).toLongLong();
                s.rotation = {pl.value(4).toBool(), pl.value(5).toBool(), pl.value(6).toBool(), pl.value(7).toInt()};
                QSqlQuery it(src);
                it.prepare(QStringLiteral("SELECT i.id, m.path FROM playlist_items i JOIN media m ON m.id = i.media_id"
                                          " WHERE i.playlist_id = ? ORDER BY i.position, i.id"));
                it.addBindValue(pl.value(0));
                ok = it.exec();
                while (ok && it.next()) {
                    if (it.value(0).toLongLong() == currentItem) {
                        s.currentIndex = s.paths.size();
                    }
                    s.paths << it.value(1).toString();
                }
                sources.push_back(s);
            }
            if (!ok) {
                fail(QStringLiteral("Reading %1 failed.").arg(QDir::toNativeSeparators(path)));
            }
        }
        readOk = ok;
        src.close();
    } // every query/handle on the source connection is destroyed here
    QSqlDatabase::removeDatabase(srcName);
    if (!readOk) {
        return -1;
    }

    // All-or-nothing into this library. Paths are kept exactly as stored -
    // on another machine they may simply be unavailable, which the UI shows.
    Transaction tx(db());
    if (!tx.started()) {
        fail(QStringLiteral("Cannot start transaction: %1").arg(db().lastError().text()));
        return -1;
    }
    for (const Source& s : sources) {
        QSqlQuery q(db());
        q.prepare(QStringLiteral(
            "INSERT INTO playlists(name, type, position, rotate_on_unlock, rotate_on_start, rotate_on_interval,"
            " interval_minutes, created_at, updated_at)"
            " VALUES (?, ?, (SELECT COALESCE(MAX(position), -1) + 1 FROM playlists), ?, ?, ?, ?, ?, ?)"));
        const QString now = nowIso();
        q.addBindValue(uniquePlaylistName(s.name));
        q.addBindValue(typeToString(s.type));
        q.addBindValue(s.rotation.onUnlock ? 1 : 0);
        q.addBindValue(s.rotation.onWindowsStart ? 1 : 0);
        q.addBindValue(s.rotation.onInterval ? 1 : 0);
        q.addBindValue(s.rotation.intervalMinutes);
        q.addBindValue(now);
        q.addBindValue(now);
        if (!check(q, "import (playlist)")) {
            return -1;
        }
        const qint64 newId = q.lastInsertId().toLongLong();
        // insertItems() opens its own transaction; SQLite has no nested
        // transactions, so insert inline here instead.
        QSqlQuery upsert(db());
        upsert.prepare(QStringLiteral("INSERT INTO media(path, path_key, type, name) VALUES (?, ?, ?, ?) "
                                      "ON CONFLICT(path_key) DO NOTHING"));
        QSqlQuery find(db());
        find.prepare(QStringLiteral("SELECT id FROM media WHERE path_key = ?"));
        QSqlQuery item(db());
        item.prepare(QStringLiteral("INSERT OR IGNORE INTO playlist_items(playlist_id, media_id, position, added_at) VALUES (?, ?, ?, ?)"));
        qint64 currentItemId = 0;
        for (int i = 0; i < s.paths.size(); ++i) {
            const QString key = pathKey(s.paths[i]);
            upsert.addBindValue(s.paths[i]);
            upsert.addBindValue(key);
            upsert.addBindValue(typeToString(s.type));
            upsert.addBindValue(QFileInfo(s.paths[i]).fileName());
            if (!check(upsert, "import (media)")) {
                return -1;
            }
            find.addBindValue(key);
            if (!check(find, "import (media id)") || !find.next()) {
                return -1;
            }
            const qint64 mediaId = find.value(0).toLongLong();
            find.finish();
            item.addBindValue(newId);
            item.addBindValue(mediaId);
            item.addBindValue(i);
            item.addBindValue(now);
            if (!check(item, "import (item)")) {
                return -1;
            }
            if (i == s.currentIndex) {
                currentItemId = item.lastInsertId().toLongLong();
            }
        }
        if (currentItemId > 0) {
            QSqlQuery cur(db());
            cur.prepare(QStringLiteral("UPDATE playlists SET current_item_id = ? WHERE id = ?"));
            cur.addBindValue(currentItemId);
            cur.addBindValue(newId);
            cur.exec();
        }
    }
    touch();
    if (!tx.commit()) {
        fail(QStringLiteral("Import commit failed: %1").arg(db().lastError().text()));
        return -1;
    }
    qInfo() << "[Library] Imported" << sources.size() << "playlist(s) from" << QDir::toNativeSeparators(path);
    return sources.size();
}
