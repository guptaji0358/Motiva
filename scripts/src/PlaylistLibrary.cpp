#include "PlaylistLibrary.h"
#include "PlaylistModel.h"
#include "SettingsManager.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>

namespace {
constexpr const char* kActiveKey = "active_playlist_id";
constexpr const char* kSelectedKey = "selected_playlist_id";
constexpr const char* kLegacyMigratedKey = "legacy_settings_migrated";
} // namespace

// ------------------------------------------------------------ PlaylistListModel

int PlaylistListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant PlaylistListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const PlaylistInfo& p = m_rows[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        return p.name;
    case Qt::AccessibleTextRole:
        return tr("%1, %2 playlist, %3%4")
            .arg(p.name, p.type == PlaylistType::Video ? tr("video") : tr("image"),
                 p.itemCount == 1 ? tr("1 item") : tr("%1 items").arg(p.itemCount),
                 p.id == m_activeId ? tr(", active") : QString());
    case IdRole:
        return p.id;
    case TypeRole:
        return static_cast<int>(p.type);
    case CountRole:
        return p.itemCount;
    case IsActiveRole:
        return p.id == m_activeId;
    default:
        return {};
    }
}

Qt::ItemFlags PlaylistListModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

bool PlaylistListModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::EditRole || !index.isValid() || index.row() >= m_rows.size()) {
        return false;
    }
    const QString name = value.toString().trimmed();
    if (name.isEmpty() || name == m_rows[index.row()].name) {
        return false; // empty or unchanged: keep the old name
    }
    auto* library = qobject_cast<PlaylistLibrary*>(QObject::parent());
    return library && library->renamePlaylist(m_rows[index.row()].id, name);
}

int PlaylistListModel::rowOf(qint64 id) const {
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].id == id) {
            return i;
        }
    }
    return -1;
}

qint64 PlaylistListModel::idAt(int row) const {
    return (row >= 0 && row < m_rows.size()) ? m_rows[row].id : 0;
}

// -------------------------------------------------------------- PlaylistLibrary

PlaylistLibrary::PlaylistLibrary(SettingsManager* settings, QObject* parent)
    : QObject(parent), m_settings(settings), m_list(this) {}

PlaylistLibrary::~PlaylistLibrary() {
    qDeleteAll(m_models);
    m_models.clear();
    m_db.close();
}

LibraryDatabase::OpenResult PlaylistLibrary::open() {
    const LibraryDatabase::OpenResult result = m_db.open(LibraryDatabase::defaultLibraryPath());
    switch (result) {
    case LibraryDatabase::OpenResult::Opened:
    case LibraryDatabase::OpenResult::Created:
        break;
    case LibraryDatabase::OpenResult::RecoveredCorrupt:
        m_openNotice = tr("Your Motiva playlist library could not be read, so a new, empty one was created.\n\n"
                          "The unreadable file was kept (not deleted) at:\n%1")
                           .arg(QDir::toNativeSeparators(m_db.movedAsidePath()));
        break;
    case LibraryDatabase::OpenResult::NewerSchema:
    case LibraryDatabase::OpenResult::Failed:
        m_openNotice = tr("Motiva could not use its playlist library:\n%1\n\n"
                          "Playlists will work for this session only and will not be saved.")
                           .arg(m_db.lastError());
        break;
    }
    migrateLegacyImagePlaylist();
    reloadList();

    m_selectedId = m_db.meta(QLatin1String(kSelectedKey)).toLongLong();
    const qint64 active = m_db.meta(QLatin1String(kActiveKey)).toLongLong();
    if (active > 0 && m_list.rowOf(active) >= 0) {
        m_activeId = active;
        m_list.m_activeId = active;
        if (PlaylistModel* m = playlist(active)) {
            m->setActive(true);
        }
    }
    if (m_list.rowOf(m_selectedId) < 0) {
        m_selectedId = m_activeId ? m_activeId : m_list.idAt(0);
    }
    qInfo() << "[Library]" << QDir::toNativeSeparators(m_db.path()) << "-" << playlistCount() << "playlist(s), active="
            << m_activeId << "persistent=" << m_db.isPersistent();
    return result;
}

void PlaylistLibrary::migrateLegacyImagePlaylist() {
    // Motiva 1.1 kept a single image playlist in QSettings. Move it into the
    // library exactly once, then remove those keys so the .mtv is the only
    // source of truth (the Windows-session stamp stays in settings - it is
    // app state, not playlist data).
    if (!m_db.isPersistent() || m_db.meta(QLatin1String(kLegacyMigratedKey)) == QLatin1String("1")) {
        return;
    }
    const QStringList paths = m_settings->playlistPaths();
    if (!paths.isEmpty()) {
        const qint64 id = m_db.createPlaylist(tr("My Images"), PlaylistType::Image);
        if (id > 0) {
            RotationSettings r;
            r.onUnlock = m_settings->playlistRotateOnUnlock();
            r.onWindowsStart = m_settings->playlistRotateOnWindowsStart();
            r.onInterval = m_settings->playlistRotateOnInterval();
            r.intervalMinutes = m_settings->playlistIntervalMinutes();
            m_db.setRotation(id, r);
            const QVector<PlaylistItemRecord> inserted =
                m_db.insertItems(id, paths, LibraryDatabase::typeToString(PlaylistType::Image), 0);
            const int current = m_settings->playlistCurrentIndex();
            if (current >= 0 && current < inserted.size()) {
                m_db.setCurrentItem(id, inserted[current].itemId);
            }
            if (m_settings->playlistEnabled()) {
                m_db.setMeta(QLatin1String(kActiveKey), QString::number(id));
            }
            qInfo() << "[Library] Migrated the v1.1 image playlist (" << inserted.size() << "images ) into the library.";
        }
    }
    m_db.setMeta(QLatin1String(kLegacyMigratedKey), QStringLiteral("1"));
    m_settings->clearLegacyPlaylistSettings();
}

void PlaylistLibrary::reloadList() {
    m_list.beginResetModel();
    m_list.m_rows = m_db.playlists();
    m_list.m_activeId = m_activeId;
    m_list.endResetModel();
    emit playlistsChanged();
}

void PlaylistLibrary::refreshListCounts(qint64 id) {
    const int row = m_list.rowOf(id);
    PlaylistModel* m = m_models.value(id);
    if (row < 0 || !m) {
        return;
    }
    m_list.m_rows[row].itemCount = m->count();
    emit m_list.dataChanged(m_list.index(row), m_list.index(row));
    emit playlistsChanged();
}

PlaylistModel* PlaylistLibrary::playlist(qint64 id) {
    if (id <= 0) {
        return nullptr;
    }
    if (PlaylistModel* existing = m_models.value(id)) {
        return existing;
    }
    const int row = m_list.rowOf(id);
    if (row < 0) {
        return nullptr;
    }
    auto* model = new PlaylistModel(&m_db, m_list.m_rows[row], nullptr);
    model->setActive(id == m_activeId);
    connect(model, &PlaylistModel::contentsChanged, this, [this, id] { refreshListCounts(id); });
    connect(model, &PlaylistModel::errorOccurred, this, &PlaylistLibrary::errorOccurred);
    connect(model, &PlaylistModel::currentChanged, this, [this, id] {
        if (id == m_activeId) {
            emit activeCurrentChanged();
        }
    });
    m_models.insert(id, model);
    return model;
}

PlaylistModel* PlaylistLibrary::activePlaylist() {
    return playlist(m_activeId);
}

bool PlaylistLibrary::setActive(qint64 id) {
    if (id > 0) {
        PlaylistModel* m = playlist(id);
        if (!m || !m->ensureCurrentAvailable()) {
            return false;
        }
    }
    if (id == m_activeId) {
        return true;
    }
    if (!m_db.setMeta(QLatin1String(kActiveKey), id > 0 ? QString::number(id) : QString())) {
        emit errorOccurred(m_db.lastError());
        return false;
    }
    if (PlaylistModel* previous = m_models.value(m_activeId)) {
        previous->setActive(false);
    }
    m_activeId = id;
    if (PlaylistModel* now = playlist(id)) {
        now->setActive(true);
    }
    m_list.m_activeId = id;
    if (!m_list.m_rows.isEmpty()) {
        emit m_list.dataChanged(m_list.index(0), m_list.index(m_list.m_rows.size() - 1));
    }
    qInfo() << "[Library] Active playlist ->" << (id > 0 ? playlist(id)->name() : QStringLiteral("none"));
    emit activeChanged(id);
    return true;
}

void PlaylistLibrary::setSelected(qint64 id) {
    if (id == m_selectedId) {
        return;
    }
    m_selectedId = id;
    m_db.setMeta(QLatin1String(kSelectedKey), QString::number(id));
}

qint64 PlaylistLibrary::createPlaylist(const QString& name, PlaylistType type) {
    const qint64 id = m_db.createPlaylist(name, type);
    if (id <= 0) {
        emit errorOccurred(m_db.lastError());
        return 0;
    }
    reloadList();
    return id;
}

bool PlaylistLibrary::renamePlaylist(qint64 id, const QString& name) {
    if (name.trimmed().isEmpty()) {
        return false;
    }
    if (!m_db.renamePlaylist(id, name)) {
        emit errorOccurred(m_db.lastError());
        return false;
    }
    if (PlaylistModel* m = m_models.value(id)) {
        m->setNameLocal(name.trimmed());
    }
    const int row = m_list.rowOf(id);
    if (row >= 0) {
        m_list.m_rows[row].name = name.trimmed();
        emit m_list.dataChanged(m_list.index(row), m_list.index(row));
    }
    emit playlistsChanged();
    return true;
}

bool PlaylistLibrary::deletePlaylist(qint64 id) {
    if (id == m_activeId) {
        setActive(0);
    }
    if (!m_db.deletePlaylist(id)) {
        emit errorOccurred(m_db.lastError());
        return false;
    }
    delete m_models.take(id);
    if (m_selectedId == id) {
        m_selectedId = 0;
    }
    reloadList();
    return true;
}

bool PlaylistLibrary::setRotation(qint64 id, const RotationSettings& rotation) {
    if (!m_db.setRotation(id, rotation)) {
        emit errorOccurred(m_db.lastError());
        return false;
    }
    if (PlaylistModel* m = m_models.value(id)) {
        m->setRotationLocal(rotation);
    }
    const int row = m_list.rowOf(id);
    if (row >= 0) {
        m_list.m_rows[row].rotation = rotation;
    }
    if (id == m_activeId) {
        emit activeSettingsChanged();
    }
    return true;
}

bool PlaylistLibrary::relocateMedia(qint64 mediaId, const QString& newPath) {
    PlaylistModel* active = activePlaylist();
    const bool wasActiveCurrent = active && active->mediaIdAt(active->currentIndex()) == mediaId;
    const qint64 result = m_db.relocateMedia(mediaId, newPath);
    if (result == 0) {
        emit errorOccurred(m_db.lastError());
        return false;
    }
    // Every loaded playlist that referenced either record is re-read, so
    // shared media is repaired everywhere at once.
    for (PlaylistModel* m : std::as_const(m_models)) {
        if (m->containsMedia(mediaId) || m->containsMedia(result)) {
            m->reloadFromDatabase();
            refreshListCounts(m->id());
        }
    }
    if (wasActiveCurrent) {
        emit activeCurrentChanged();
    }
    return true;
}

bool PlaylistLibrary::exportLibrary(const QString& path) {
    if (!m_db.exportTo(path)) {
        emit errorOccurred(m_db.lastError());
        return false;
    }
    return true;
}

int PlaylistLibrary::importLibrary(const QString& path) {
    const int count = m_db.importFrom(path);
    if (count < 0) {
        emit errorOccurred(m_db.lastError());
        return -1;
    }
    reloadList();
    return count;
}

void PlaylistLibrary::closeForCleanup() {
    emit aboutToReset();
    const bool hadActive = m_activeId > 0;
    qDeleteAll(m_models);
    m_models.clear();
    m_activeId = 0;
    m_selectedId = 0;
    m_list.beginResetModel();
    m_list.m_rows.clear();
    m_list.m_activeId = 0;
    m_list.endResetModel();
    m_db.close();
    qInfo() << "[Library] Closed for Cleanup & Reset.";
    if (hadActive) {
        emit activeChanged(0);
    }
    emit playlistsChanged();
}

bool PlaylistLibrary::reopenAfterCleanup() {
    m_openNotice.clear();
    open();
    emit resetFinished();
    return m_db.isPersistent();
}

int PlaylistLibrary::clearThumbnailCache() {
    int dropped = 0;
    for (PlaylistModel* m : std::as_const(m_models)) {
        dropped += m->clearThumbnailCache();
    }
    return dropped;
}

int PlaylistLibrary::cachedThumbnailCount() const {
    int count = 0;
    for (PlaylistModel* m : std::as_const(m_models)) {
        count += m->m_thumbnails.size();
    }
    return count;
}

bool PlaylistLibrary::setCategoryItems(qint64 categoryId, qint64 playlistId, const QSet<qint64>& itemIds) {
    if (!m_db.setCategoryItems(categoryId, playlistId, itemIds)) {
        emit errorOccurred(m_db.lastError());
        return false;
    }
    emit filtersChanged(playlistId);
    return true;
}

qint64 PlaylistLibrary::createFilter(qint64 playlistId, const QString& name, const FilterDefinition& definition,
                                     const QString& mode) {
    const qint64 id = m_db.createPlaylistFilter(playlistId, name, definition, mode);
    if (id <= 0) {
        emit errorOccurred(m_db.lastError());
        return 0;
    }
    emit filtersChanged(playlistId);
    return id;
}

bool PlaylistLibrary::updateFilter(qint64 filterId, qint64 playlistId, const QString& name,
                                   const FilterDefinition& definition) {
    if (!m_db.updatePlaylistFilter(filterId, name, definition)) {
        emit errorOccurred(m_db.lastError());
        return false;
    }
    emit filtersChanged(playlistId);
    return true;
}

bool PlaylistLibrary::deleteFilter(qint64 filterId, qint64 playlistId) {
    if (!m_db.deletePlaylistFilter(filterId)) {
        emit errorOccurred(m_db.lastError());
        return false;
    }
    emit filtersChanged(playlistId);
    return true;
}
