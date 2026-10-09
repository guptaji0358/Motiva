#pragma once

#include "LibraryDatabase.h"

#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QVector>

class PlaylistModel;
class SettingsManager;

// "My Playlists": the list of playlists in the library, for
// PlaylistDialog's sidebar. Owned by PlaylistLibrary.
class PlaylistListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { IdRole = Qt::UserRole + 1, TypeRole, CountRole, IsActiveRole };
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    // Editable name: inline rename in PlaylistDialog goes through
    // setData -> PlaylistLibrary::renamePlaylist (the one rename path).
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    int rowOf(qint64 id) const;
    qint64 idAt(int row) const;

private:
    friend class PlaylistLibrary;
    QVector<PlaylistInfo> m_rows;
    qint64 m_activeId = 0;
};

// The playlist layer of Motiva: owns the .mtv library (LibraryDatabase),
// the per-playlist item models (created lazily, on first use), and which
// single playlist is ACTIVE - i.e. controls the wallpaper. Exactly one
// playlist (or none) is active at a time, so two playlists can never
// fight over the wallpaper.
//
// It never touches the wallpaper itself: MainWindow reacts to
// activeChanged/activeCurrentChanged by loading the active playlist's
// current item into the existing media pipeline, like any other media.
//
// Library-scoped state (active playlist, last selected playlist) lives in
// the .mtv's library_metadata; app/session state (the Windows logon stamp)
// stays in SettingsManager - one source of truth for each.
class PlaylistLibrary : public QObject {
    Q_OBJECT
public:
    explicit PlaylistLibrary(SettingsManager* settings, QObject* parent = nullptr);
    ~PlaylistLibrary() override;

    // Opens (creating if needed) the default library and migrates the
    // v1.1 single image playlist from QSettings on first run.
    LibraryDatabase::OpenResult open();
    // A message for the user when opening needed attention (recovered a
    // damaged file, newer schema, could not open). Empty when all is well.
    QString openNotice() const { return m_openNotice; }
    QString libraryPath() const { return m_db.path(); }
    bool isPersistent() const { return m_db.isPersistent(); }

    PlaylistListModel* listModel() { return &m_list; }
    // The library's database, for BackupManager's bookkeeping (media_backups).
    // GUI thread only - SQLite connections belong to the thread that made them.
    LibraryDatabase& database() { return m_db; }

    // --- virtual categories: saved filters of one playlist (see
    // LibraryDatabase). They hold rules, never media - removing/adding
    // items stays a playlist operation. ---
    QVector<PlaylistFilterInfo> filters(qint64 playlistId) { return m_db.playlistFilters(playlistId); }
    qint64 createFilter(qint64 playlistId, const QString& name, const FilterDefinition& definition,
                        const QString& mode = QStringLiteral("condition"));
    // Wallpaper Flow node positions (layout only; the sequence is the playlist order).
    QHash<qint64, QPointF> flowPositions(qint64 playlistId) { return m_db.flowPositions(playlistId); }
    bool saveFlowPositions(qint64 playlistId, const QHash<qint64, QPointF>& positions) {
        return m_db.saveFlowPositions(playlistId, positions);
    }
    bool clearFlowPositions(qint64 playlistId) { return m_db.clearFlowPositions(playlistId); }
    bool setCategoryItems(qint64 categoryId, qint64 playlistId, const QSet<qint64>& itemIds);
    bool updateFilter(qint64 filterId, qint64 playlistId, const QString& name, const FilterDefinition& definition);
    bool deleteFilter(qint64 filterId, qint64 playlistId);
    QSet<qint64> matchingItems(qint64 playlistId, const FilterDefinition* definition, const QString& search) {
        return m_db.matchingItems(playlistId, definition, search);
    }
    int playlistCount() const { return m_list.m_rows.size(); }
    PlaylistModel* playlist(qint64 id);

    qint64 activeId() const { return m_activeId; }
    PlaylistModel* activePlaylist();
    // The category (playlist_categories.id) the active playlist is limited to;
    // 0 = the whole playlist ("All"). Always one of the active playlist's own
    // categories while activeId() > 0.
    qint64 activeCategoryId() const { return m_activeCategoryId; }
    QString activeCategoryName(); // empty when the whole playlist is the source
    // playlist_items.id of the items category `categoryId` of `playlistId`
    // currently contains (recomputed from the database each call, so a
    // condition category follows media added later). categoryId 0 = every
    // item. `found` is false when the category does not belong to the playlist.
    QSet<qint64> categoryItemIds(qint64 playlistId, qint64 categoryId, bool* found = nullptr);
    // Activates playlist `id` (0 = none), optionally limited to one of its
    // categories. categoryId < 0 keeps the current category when `id` is
    // already active, else uses the whole playlist; 0 = whole playlist.
    // Fails (false, current source unchanged) if the playlist or category has
    // no available item. The playlist's stored order is never touched.
    bool setActive(qint64 id, qint64 categoryId = -1);

    qint64 selectedId() const { return m_selectedId; }
    void setSelected(qint64 id);

    qint64 createPlaylist(const QString& name, PlaylistType type);
    bool renamePlaylist(qint64 id, const QString& name);
    bool deletePlaylist(qint64 id);
    bool setRotation(qint64 id, const RotationSettings& rotation);

    // A missing media file was found at `newPath` (see MediaRecovery):
    // repoints the shared media record (every playlist that uses it
    // follows) and reloads the affected playlists. If it is the active
    // playlist's current item, activeCurrentChanged() re-applies it.
    bool relocateMedia(qint64 mediaId, const QString& newPath);

    bool exportLibrary(const QString& path);
    int importLibrary(const QString& path);
    QString lastError() const { return m_db.lastError(); }

    // --- Cleanup & Reset (see CleanupManager) ---
    // Closes the library through LibraryDatabase's own lifecycle so its
    // files can be removed safely: emits aboutToReset() (views drop their
    // playlist models), destroys every playlist model, empties the list
    // and closes the SQLite connection. The active playlist is cleared
    // locally only - nothing is written to the file that is about to go.
    void closeForCleanup();
    // Re-opens the default library after cleanup (creating a fresh, empty
    // one with the normal schema initialization if the file was removed)
    // and emits resetFinished(). Returns false if the library could only
    // be opened in memory (see openNotice()).
    bool reopenAfterCleanup();
    // Drops every open playlist's in-memory preview thumbnails. Returns
    // how many cached previews were dropped.
    int clearThumbnailCache();
    // Number of preview thumbnails currently held in memory.
    int cachedThumbnailCount() const;


signals:
    // Fires when the active playlist OR its category changes.
    void activeChanged(qint64 id);
    // The active category's membership changed (media added/removed, rules edited).
    void activeScopeChanged();
    // Something about the active wallpaper source the user should know (a category
    // emptied/was deleted, a category cannot be activated). Not a save failure.
    void sourceNotice(const QString& message);
    // The active playlist's current item changed (advance, Show now, removal).
    void activeCurrentChanged();
    // The active playlist's own settings changed (rotation).
    void activeSettingsChanged();
    void playlistsChanged();
    void errorOccurred(const QString& message);
    // A playlist's saved filters were created, renamed/edited or deleted.
    void filtersChanged(qint64 playlistId);
    // Around closeForCleanup()/reopenAfterCleanup(): every PlaylistModel*
    // handed out before aboutToReset() is destroyed right after it.
    void aboutToReset();
    void resetFinished();

private:
    void reloadList();
    void migrateLegacyImagePlaylist();
    void refreshListCounts(qint64 id);
    // Re-resolves the active category's members into the active model; if the
    // category vanished or emptied, the source is switched off with a notice.
    void refreshActiveScope();
    bool persistActiveSource(qint64 playlistId, qint64 categoryId);
    void restoreActiveSource(qint64 playlistId, qint64 categoryId, bool categoryType);

    SettingsManager* m_settings;
    LibraryDatabase m_db;
    PlaylistListModel m_list;
    QHash<qint64, PlaylistModel*> m_models;
    qint64 m_activeId = 0;
    qint64 m_activeCategoryId = 0;
    bool m_refreshingScope = false;
    qint64 m_selectedId = 0;
    QString m_openNotice;
};
