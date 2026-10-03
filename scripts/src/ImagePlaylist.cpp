#include "ImagePlaylist.h"
#include "SettingsManager.h"
#include "VideoPlayer.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QtConcurrent/QtConcurrentRun>

namespace {
// Thumbnail decode bound - large enough to stay crisp on a high-DPI card.
constexpr int kThumbMaxWidth = 360;
constexpr int kThumbMaxHeight = 240;
} // namespace

ImagePlaylist::ImagePlaylist(SettingsManager* settings, QObject* parent)
    : QAbstractListModel(parent), m_settings(settings) {
    // Restored exactly as saved - including entries whose files are
    // currently missing (they are flagged, not dropped).
    QSet<QString> seen;
    for (const QString& path : m_settings->playlistPaths()) {
        const QString key = normalizedKey(path);
        if (path.isEmpty() || seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        m_items.push_back({QDir::toNativeSeparators(path), key, QFileInfo(path).isFile()});
    }
    m_current = m_settings->playlistCurrentIndex();
    if (m_current < -1 || m_current >= m_items.size()) {
        m_current = m_items.isEmpty() ? -1 : 0;
    }
    m_enabled = m_settings->playlistEnabled() && !m_items.isEmpty();
    qInfo() << "[Playlist] Restored" << m_items.size() << "image(s), current=" << m_current
            << "enabled=" << m_enabled << "available=" << availableCount();
}

bool ImagePlaylist::isSupportedImage(const QString& path) {
    return VideoPlayer::hasStaticImageExtension(path);
}

QString ImagePlaylist::normalizedKey(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

int ImagePlaylist::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_items.size();
}

QVariant ImagePlaylist::data(const QModelIndex& index, int role) const {
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
        return tr("Image %1 of %2: %3%4%5")
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
    case ThumbnailRole: {
        auto it = m_thumbnails.constFind(item.key);
        if (it != m_thumbnails.constEnd()) {
            return *it;
        }
        if (item.available) {
            requestThumbnail(item.key, item.path);
        }
        return QImage();
    }
    default:
        return {};
    }
}

Qt::ItemFlags ImagePlaylist::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::ItemIsDropEnabled;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

QString ImagePlaylist::pathAt(int row) const {
    return (row >= 0 && row < m_items.size()) ? m_items[row].path : QString();
}

bool ImagePlaylist::isAvailable(int row) const {
    return row >= 0 && row < m_items.size() && m_items[row].available;
}

int ImagePlaylist::availableCount() const {
    int n = 0;
    for (const Item& item : m_items) {
        n += item.available ? 1 : 0;
    }
    return n;
}

void ImagePlaylist::save() const {
    QStringList paths;
    paths.reserve(m_items.size());
    for (const Item& item : m_items) {
        paths << item.path;
    }
    m_settings->setPlaylistPaths(paths);
    m_settings->setPlaylistCurrentIndex(m_current);
    m_settings->setPlaylistEnabled(m_enabled);
}

void ImagePlaylist::updateAvailability(int row) {
    if (row < 0 || row >= m_items.size()) {
        return;
    }
    const bool nowAvailable = QFileInfo(m_items[row].path).isFile();
    if (nowAvailable != m_items[row].available) {
        m_items[row].available = nowAvailable;
        if (!nowAvailable) {
            m_thumbnails.remove(m_items[row].key);
        }
        const QModelIndex idx = index(row);
        emit dataChanged(idx, idx);
        emit contentsChanged();
    }
}

void ImagePlaylist::refreshAvailability() {
    for (int row = 0; row < m_items.size(); ++row) {
        updateAvailability(row);
    }
}

bool ImagePlaylist::setEnabled(bool enabled) {
    if (enabled) {
        refreshAvailability();
        if (!isAvailable(m_current)) {
            const int next = nextAvailableAfter(m_current);
            if (next < 0) {
                qInfo() << "[Playlist] Cannot turn on - no available images.";
                enabled = false;
            } else {
                const int previous = m_current;
                m_current = next;
                emit dataChanged(index(qMax(previous, 0)), index(qMax(previous, 0)));
                emit dataChanged(index(next), index(next));
            }
        }
    }
    if (enabled == m_enabled) {
        save();
        return m_enabled;
    }
    m_enabled = enabled;
    save();
    qInfo() << "[Playlist]" << (m_enabled ? "Turned on" : "Turned off") << "- current=" << m_current;
    if (!m_items.isEmpty()) {
        emit dataChanged(index(0), index(m_items.size() - 1));
    }
    emit enabledChanged(m_enabled);
    return m_enabled;
}

ImagePlaylist::AddResult ImagePlaylist::addImages(const QStringList& paths, int insertRow) {
    AddResult result;
    QSet<QString> existing;
    for (const Item& item : m_items) {
        existing.insert(item.key);
    }
    QVector<Item> toAdd;
    for (const QString& raw : paths) {
        const QFileInfo fi(raw);
        if (!fi.isFile() || !isSupportedImage(raw)) {
            ++result.unsupported;
            continue;
        }
        const QString key = normalizedKey(raw);
        if (existing.contains(key)) {
            ++result.duplicates;
            continue;
        }
        existing.insert(key);
        toAdd.push_back({QDir::toNativeSeparators(fi.absoluteFilePath()), key, true});
    }
    if (toAdd.isEmpty()) {
        qInfo() << "[Playlist] Nothing added - duplicates=" << result.duplicates
                << "unsupported=" << result.unsupported;
        return result;
    }

    if (insertRow < 0 || insertRow > m_items.size()) {
        insertRow = m_items.size();
    }
    beginInsertRows(QModelIndex(), insertRow, insertRow + toAdd.size() - 1);
    for (int i = 0; i < toAdd.size(); ++i) {
        m_items.insert(insertRow + i, toAdd[i]);
    }
    endInsertRows();
    result.added = toAdd.size();

    const bool wasEmpty = (m_current < 0);
    if (wasEmpty) {
        m_current = 0;
    } else if (insertRow <= m_current) {
        m_current += toAdd.size(); // same image stays current
    }
    save();
    qInfo() << "[Playlist] Added" << result.added << "image(s) at" << insertRow
            << "duplicates=" << result.duplicates << "unsupported=" << result.unsupported
            << "total=" << m_items.size();
    emit contentsChanged();
    if (wasEmpty) {
        emit currentChanged();
    }
    return result;
}

void ImagePlaylist::removeAt(int row) {
    if (row < 0 || row >= m_items.size()) {
        return;
    }
    const bool wasCurrent = (row == m_current);
    qInfo() << "[Playlist] Removing" << m_items[row].path << "(file itself is untouched)";
    beginRemoveRows(QModelIndex(), row, row);
    m_thumbnails.remove(m_items[row].key);
    m_items.removeAt(row);
    endRemoveRows();

    if (m_items.isEmpty()) {
        m_current = -1;
        const bool wasEnabled = m_enabled;
        m_enabled = false;
        save();
        emit contentsChanged();
        if (wasEnabled) {
            emit enabledChanged(false);
        }
        return;
    }

    if (row < m_current) {
        --m_current; // same image, shifted left
    } else if (wasCurrent) {
        // The image that followed the removed one takes its place in the
        // sequence (wrapping), skipping any missing files.
        const int candidate = row % m_items.size();
        m_current = isAvailable(candidate) ? candidate : nextAvailableAfter(candidate);
        if (m_current < 0) {
            m_current = candidate; // nothing available; keep a position to resume from
        }
        emit dataChanged(index(m_current), index(m_current));
    }
    save();
    emit contentsChanged();
    if (wasCurrent) {
        emit currentChanged();
    }
}

void ImagePlaylist::clear() {
    if (m_items.isEmpty()) {
        return;
    }
    qInfo() << "[Playlist] Cleared" << m_items.size() << "image(s) (files themselves are untouched)";
    beginResetModel();
    m_items.clear();
    m_thumbnails.clear();
    m_current = -1;
    endResetModel();
    const bool wasEnabled = m_enabled;
    m_enabled = false;
    save();
    emit contentsChanged();
    if (wasEnabled) {
        emit enabledChanged(false);
    }
}

bool ImagePlaylist::move(int from, int to) {
    if (from < 0 || from >= m_items.size() || to < 0 || to >= m_items.size() || from == to) {
        return false;
    }
    // beginMoveRows wants the destination as an insert-before position in
    // the pre-move numbering.
    const int destinationChild = (to > from) ? to + 1 : to;
    if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), destinationChild)) {
        return false;
    }
    const QString currentKey = (m_current >= 0) ? m_items[m_current].key : QString();
    m_items.move(from, to);
    endMoveRows();
    if (!currentKey.isEmpty()) {
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items[i].key == currentKey) {
                m_current = i;
                break;
            }
        }
    }
    save();
    qInfo() << "[Playlist] Moved image from" << from << "to" << to << "- current is now" << m_current;
    emit contentsChanged();
    return true;
}

bool ImagePlaylist::setCurrentIndex(int row) {
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
    save();
    if (previous >= 0 && previous < m_items.size()) {
        emit dataChanged(index(previous), index(previous));
    }
    emit dataChanged(index(row), index(row));
    qInfo() << "[Playlist] Current image set to" << row << m_items[row].path;
    emit currentChanged();
    return true;
}

int ImagePlaylist::nextAvailableAfter(int from) {
    const int n = m_items.size();
    for (int step = 1; step <= n; ++step) {
        const int row = ((from < 0 ? -1 : from) + step) % n;
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

bool ImagePlaylist::advance() {
    if (m_items.isEmpty()) {
        return false;
    }
    const int next = nextAvailableAfter(m_current);
    if (next < 0) {
        qInfo() << "[Playlist] advance(): no other available image - staying on" << m_current;
        return false;
    }
    const int previous = m_current;
    m_current = next;
    save();
    if (previous >= 0 && previous < m_items.size()) {
        emit dataChanged(index(previous), index(previous));
    }
    emit dataChanged(index(next), index(next));
    qInfo() << "[Playlist] Advanced" << previous << "->" << next << m_items[next].path;
    emit currentChanged();
    return true;
}

void ImagePlaylist::requestThumbnail(const QString& key, const QString& path) const {
    if (m_pendingThumbnails.contains(key)) {
        return;
    }
    m_pendingThumbnails.insert(key);
    auto* watcher = new QFutureWatcher<QImage>(const_cast<ImagePlaylist*>(this));
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, key]() {
        const QImage image = watcher->result();
        watcher->deleteLater();
        const_cast<ImagePlaylist*>(this)->onThumbnailReady(key, image);
    });
    watcher->setFuture(QtConcurrent::run([path]() {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QSize size = reader.size();
        if (size.isValid()) {
            // Decode already downscaled (cheap for JPEG); scaled size is in
            // the file's stored orientation, before the EXIF transform.
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
    }));
}

void ImagePlaylist::onThumbnailReady(const QString& key, const QImage& image) {
    m_pendingThumbnails.remove(key);
    if (image.isNull()) {
        return;
    }
    m_thumbnails.insert(key, image);
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items[row].key == key) {
            emit dataChanged(index(row), index(row), {ThumbnailRole});
        }
    }
}
