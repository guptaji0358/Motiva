#include "PlaylistModel.h"
#include "VideoPlayer.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>

namespace {
constexpr int kThumbMaxWidth = 360;
constexpr int kThumbMaxHeight = 240;

// Thumbnails are produced lazily (only for cards actually painted) on a
// small dedicated pool, so a playlist of thousands of items never decodes
// more than two files at once and never blocks the GUI thread.
QThreadPool* thumbnailPool() {
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool();
        p->setMaxThreadCount(2);
        return p;
    }();
    return pool;
}

QImage imageThumbnail(const QString& path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid()) {
        const QSize bounds = (reader.transformation() & QImageIOHandler::TransformationRotate90)
            ? QSize(kThumbMaxHeight, kThumbMaxWidth) : QSize(kThumbMaxWidth, kThumbMaxHeight);
        if (size.width() > bounds.width() || size.height() > bounds.height()) {
            reader.setScaledSize(size.scaled(bounds, Qt::KeepAspectRatio));
        }
    }
    QImage image = reader.read();
    if (!image.isNull() && (image.width() > kThumbMaxWidth || image.height() > kThumbMaxHeight)) {
        image = image.scaled(kThumbMaxWidth, kThumbMaxHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

// Video frames come from the Windows Shell thumbnail cache/providers
// (IShellItemImageFactory) - the same thumbnails Explorer shows. This is
// not a media pipeline: Motiva never decodes the video for a thumbnail,
// and a file with no available thumbnail simply gets a placeholder card.
QImage videoThumbnail(const QString& path) {
    QImage result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IShellItemImageFactory* factory = nullptr;
    const std::wstring wpath = QDir::toNativeSeparators(path).toStdWString();
    if (SUCCEEDED(SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&factory))) && factory) {
        HBITMAP bitmap = nullptr;
        const SIZE size{kThumbMaxWidth, kThumbMaxHeight};
        if (SUCCEEDED(factory->GetImage(size, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) && bitmap) {
            result = QImage::fromHBITMAP(bitmap);
            DeleteObject(bitmap);
        }
        factory->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    if (!result.isNull() && (result.width() > kThumbMaxWidth || result.height() > kThumbMaxHeight)) {
        result = result.scaled(kThumbMaxWidth, kThumbMaxHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return result;
}
} // namespace

PlaylistModel::PlaylistModel(LibraryDatabase* db, const PlaylistInfo& info, QObject* parent)
    : QAbstractListModel(parent), m_db(db), m_id(info.id), m_name(info.name), m_type(info.type),
      m_rotation(info.rotation) {
    loadItems();
}

void PlaylistModel::loadItems() {
    const qint64 currentItemId = m_db->currentItemId(m_id);
    const QVector<PlaylistItemRecord> records = m_db->items(m_id);
    m_items.clear();
    m_items.reserve(records.size());
    m_current = -1;
    // Size/modified are what let MediaRecovery recognize a moved file;
    // record them for available files that predate this (one transaction).
    QVector<QPair<qint64, QString>> unknownFacts;
    for (const PlaylistItemRecord& r : records) {
        if (r.itemId == currentItemId) {
            m_current = m_items.size();
        }
        Item item{r.itemId, r.mediaId, r.path, LibraryDatabase::pathKey(r.path), QFileInfo(r.path).isFile(), r.facts};
        if (item.available && !item.facts.isKnown()) {
            unknownFacts.push_back({r.mediaId, r.path});
            item.facts = MediaFacts::ofFile(r.path);
        }
        m_items.push_back(item);
    }
    if (m_current < 0 && !m_items.isEmpty()) {
        m_current = 0;
    }
    m_db->backfillMediaFacts(unknownFacts);
}

void PlaylistModel::reloadFromDatabase() {
    beginResetModel();
    m_thumbnails.clear();
    m_noThumbnail.clear();
    loadItems();
    endResetModel();
    emit contentsChanged();
}

qint64 PlaylistModel::mediaIdAt(int row) const {
    return (row >= 0 && row < m_items.size()) ? m_items[row].mediaId : 0;
}

qint64 PlaylistModel::itemIdAt(int row) const {
    return (row >= 0 && row < m_items.size()) ? m_items[row].itemId : 0;
}

int PlaylistModel::rowOfItem(qint64 itemId) const {
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].itemId == itemId) {
            return i;
        }
    }
    return -1;
}

MediaFacts PlaylistModel::factsAt(int row) const {
    return (row >= 0 && row < m_items.size()) ? m_items[row].facts : MediaFacts();
}

bool PlaylistModel::containsMedia(qint64 mediaId) const {
    for (const Item& item : m_items) {
        if (item.mediaId == mediaId) {
            return true;
        }
    }
    return false;
}

bool PlaylistModel::isImageFile(const QString& path) {
    return VideoPlayer::hasStaticImageExtension(path);
}

bool PlaylistModel::isVideoFile(const QString& path) {
    return VideoPlayer::hasVideoExtension(path);
}

bool PlaylistModel::acceptsFile(const QString& path) const {
    return QFileInfo(path).isFile() && (isVideo() ? isVideoFile(path) : isImageFile(path));
}

int PlaylistModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_items.size();
}

QVariant PlaylistModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) {
        return {};
    }
    const Item& item = m_items[index.row()];
    switch (role) {
    case Qt::DisplayRole:
        return QFileInfo(item.path).fileName();
    case Qt::ToolTipRole:
        return item.available ? item.path : tr("%1\n\nFile not found - it may have been moved, renamed, "
                                               "deleted or be on a disconnected drive.").arg(item.path);
    case Qt::AccessibleTextRole:
        return tr("%1 %2 of %3: %4%5%6")
            .arg(isVideo() ? tr("Video") : tr("Image"))
            .arg(index.row() + 1)
            .arg(m_items.size())
            .arg(QFileInfo(item.path).fileName())
            .arg(index.row() == m_current ? tr(", current") : QString())
            .arg(item.available ? QString() : tr(", file not found"));
    case PathRole:
        return item.path;
    case AvailableRole:
        return item.available;
    case IsCurrentRole:
        return index.row() == m_current;
    case IsVideoRole:
        return isVideo();
    case ThumbnailPendingRole:
        return m_pendingThumbnails.contains(item.key);
    case ThumbnailRole: {
        auto it = m_thumbnails.constFind(item.key);
        if (it != m_thumbnails.constEnd()) {
            return *it;
        }
        if (item.available && !m_noThumbnail.contains(item.key)) {
            requestThumbnail(item.key, item.path);
        }
        return QImage();
    }
    default:
        return {};
    }
}

Qt::ItemFlags PlaylistModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::ItemIsDropEnabled;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

QString PlaylistModel::pathAt(int row) const {
    return (row >= 0 && row < m_items.size()) ? m_items[row].path : QString();
}

bool PlaylistModel::isAvailable(int row) const {
    return row >= 0 && row < m_items.size() && m_items[row].available;
}

int PlaylistModel::availableCount() const {
    int n = 0;
    for (const Item& item : m_items) {
        n += item.available ? 1 : 0;
    }
    return n;
}

void PlaylistModel::setActive(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (!m_items.isEmpty()) {
        emit dataChanged(index(0), index(m_items.size() - 1));
    }
}

void PlaylistModel::emitRowChanged(int row) {
    if (row >= 0 && row < m_items.size()) {
        emit dataChanged(index(row), index(row));
    }
}

bool PlaylistModel::persistCurrent() {
    const qint64 itemId = (m_current >= 0 && m_current < m_items.size()) ? m_items[m_current].itemId : 0;
    if (!m_db->setCurrentItem(m_id, itemId)) {
        emit errorOccurred(m_db->lastError());
        return false;
    }
    return true;
}

void PlaylistModel::updateAvailability(int row) {
    if (row < 0 || row >= m_items.size()) {
        return;
    }
    const bool nowAvailable = QFileInfo(m_items[row].path).isFile();
    if (nowAvailable != m_items[row].available) {
        m_items[row].available = nowAvailable;
        if (!nowAvailable) {
            m_thumbnails.remove(m_items[row].key);
        }
        emitRowChanged(row);
        emit contentsChanged();
    }
}

void PlaylistModel::refreshAvailability() {
    for (int row = 0; row < m_items.size(); ++row) {
        updateAvailability(row);
    }
}

PlaylistModel::AddResult PlaylistModel::addFiles(const QStringList& paths, int insertRow) {
    AddResult result;
    QSet<QString> existing;
    for (const Item& item : m_items) {
        existing.insert(item.key);
    }
    QStringList toAdd;
    for (const QString& raw : paths) {
        if (!acceptsFile(raw)) {
            ++result.unsupported;
            continue;
        }
        const QString key = LibraryDatabase::pathKey(raw);
        if (existing.contains(key)) {
            ++result.duplicates;
            continue;
        }
        existing.insert(key);
        toAdd << raw;
    }
    if (toAdd.isEmpty()) {
        return result;
    }
    if (insertRow < 0 || insertRow > m_items.size()) {
        insertRow = m_items.size();
    }
    const QVector<PlaylistItemRecord> records =
        m_db->insertItems(m_id, toAdd, LibraryDatabase::typeToString(m_type), insertRow);
    if (records.size() != toAdd.size()) {
        result.failed = true;
        emit errorOccurred(m_db->lastError());
        return result;
    }
    beginInsertRows(QModelIndex(), insertRow, insertRow + records.size() - 1);
    for (int i = 0; i < records.size(); ++i) {
        m_items.insert(insertRow + i, {records[i].itemId, records[i].mediaId, records[i].path,
                                       LibraryDatabase::pathKey(records[i].path), true, records[i].facts});
    }
    endInsertRows();
    result.added = records.size();

    const bool wasEmpty = (m_current < 0);
    if (wasEmpty) {
        m_current = 0;
        persistCurrent();
    } else if (insertRow <= m_current) {
        m_current += records.size();
    }
    qInfo() << "[Playlist]" << m_name << "- added" << result.added << "item(s) at" << insertRow
            << "duplicates=" << result.duplicates << "unsupported=" << result.unsupported << "total=" << m_items.size();
    emit contentsChanged();
    if (wasEmpty) {
        emit currentChanged();
    }
    return result;
}

bool PlaylistModel::removeAt(int row) {
    if (row < 0 || row >= m_items.size()) {
        return false;
    }
    if (!m_db->removeItem(m_id, m_items[row].itemId)) {
        emit errorOccurred(m_db->lastError());
        return false;
    }
    const bool wasCurrent = (row == m_current);
    qInfo() << "[Playlist]" << m_name << "- removed" << m_items[row].path << "(file untouched)";
    beginRemoveRows(QModelIndex(), row, row);
    m_thumbnails.remove(m_items[row].key);
    m_items.removeAt(row);
    endRemoveRows();

    if (m_items.isEmpty()) {
        m_current = -1;
    } else if (row < m_current) {
        --m_current;
    } else if (wasCurrent) {
        // The item that followed takes its place in the sequence.
        const int candidate = row % m_items.size();
        m_current = isAvailable(candidate) ? candidate : nextAvailableAfter(candidate, true);
        if (m_current < 0) {
            m_current = candidate;
        }
        emitRowChanged(m_current);
    }
    persistCurrent();
    emit contentsChanged();
    if (wasCurrent) {
        emit currentChanged();
    }
    return true;
}

bool PlaylistModel::clear() {
    if (m_items.isEmpty()) {
        return true;
    }
    if (!m_db->clearItems(m_id)) {
        emit errorOccurred(m_db->lastError());
        return false;
    }
    beginResetModel();
    m_items.clear();
    m_thumbnails.clear();
    m_current = -1;
    endResetModel();
    emit contentsChanged();
    emit currentChanged();
    return true;
}

bool PlaylistModel::move(int from, int to) {
    if (from < 0 || from >= m_items.size() || to < 0 || to >= m_items.size() || from == to) {
        return false;
    }
    const qint64 currentId = (m_current >= 0) ? m_items[m_current].itemId : 0;
    QVector<Item> reordered = m_items;
    reordered.move(from, to);
    QVector<qint64> ids;
    for (const Item& item : reordered) {
        ids.push_back(item.itemId);
    }
    if (!m_db->setOrder(m_id, ids)) {
        emit errorOccurred(m_db->lastError());
        return false;
    }
    const int destinationChild = (to > from) ? to + 1 : to;
    beginMoveRows(QModelIndex(), from, from, QModelIndex(), destinationChild);
    m_items = reordered;
    endMoveRows();
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].itemId == currentId) {
            m_current = i;
        }
    }
    emit contentsChanged();
    return true;
}

bool PlaylistModel::setCurrentIndex(int row) {
    if (row < 0 || row >= m_items.size()) {
        return false;
    }
    updateAvailability(row);
    if (!m_items[row].available) {
        return false;
    }
    if (row == m_current) {
        return true;
    }
    const int previous = m_current;
    m_current = row;
    persistCurrent();
    emitRowChanged(previous);
    emitRowChanged(row);
    emit currentChanged();
    return true;
}

int PlaylistModel::nextAvailableAfter(int from, bool wrap) {
    const int n = m_items.size();
    for (int step = 1; step <= n; ++step) {
        const int raw = (from < 0 ? -1 : from) + step;
        if (!wrap && raw >= n) {
            break;
        }
        const int row = raw % n;
        if (row == from) {
            break;
        }
        updateAvailability(row);
        if (m_items[row].available) {
            return row;
        }
    }
    return -1;
}

bool PlaylistModel::advance(bool wrap) {
    if (m_items.isEmpty()) {
        return false;
    }
    const int next = nextAvailableAfter(m_current, wrap);
    if (next < 0) {
        return false;
    }
    const int previous = m_current;
    m_current = next;
    persistCurrent();
    emitRowChanged(previous);
    emitRowChanged(next);
    qInfo() << "[Playlist]" << m_name << "- advanced" << previous << "->" << next << m_items[next].path;
    emit currentChanged();
    return true;
}

bool PlaylistModel::ensureCurrentAvailable() {
    updateAvailability(m_current);
    if (isAvailable(m_current)) {
        return true;
    }
    const int next = nextAvailableAfter(m_current, true);
    if (next < 0) {
        return false;
    }
    const int previous = m_current;
    m_current = next;
    persistCurrent();
    emitRowChanged(previous);
    emitRowChanged(next);
    emit currentChanged();
    return true;
}

void PlaylistModel::requestThumbnail(const QString& key, const QString& path) const {
    if (m_pendingThumbnails.contains(key)) {
        return;
    }
    m_pendingThumbnails.insert(key);
    auto* self = const_cast<PlaylistModel*>(this);
    auto* watcher = new QFutureWatcher<QImage>(self);
    connect(watcher, &QFutureWatcher<QImage>::finished, self, [self, watcher, key]() {
        const QImage image = watcher->result();
        watcher->deleteLater();
        self->onThumbnailReady(key, image);
    });
    const bool video = isVideo();
    watcher->setFuture(QtConcurrent::run(thumbnailPool(), [path, video]() {
        return video ? videoThumbnail(path) : imageThumbnail(path);
    }));
}

void PlaylistModel::onThumbnailReady(const QString& key, const QImage& image) {
    m_pendingThumbnails.remove(key);
    if (image.isNull()) {
        m_noThumbnail.insert(key); // card shows its "no preview" placeholder
    } else {
        m_thumbnails.insert(key, image);
    }
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items[row].key == key) {
            emit dataChanged(index(row), index(row), {ThumbnailRole});
        }
    }
}
