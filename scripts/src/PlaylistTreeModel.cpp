#include "PlaylistTreeModel.h"
#include "PlaylistLibrary.h"

#include <QMimeData>
#include <QTimer>

namespace {
constexpr const char* kPlaylistRefMime = "application/x-motiva-playlist-ref";
}

PlaylistTreeModel::PlaylistTreeModel(PlaylistLibrary* library, QObject* parent)
    : QAbstractItemModel(parent), m_library(library) {
    PlaylistListModel* list = library->listModel();
    // Structure changes (playlists added/removed/reloaded, categories or
    // their links changed) rebuild the tree; plain data changes (name,
    // item count, active) only repaint the rows.
    connect(list, &QAbstractItemModel::modelReset, this, &PlaylistTreeModel::rebuild);
    connect(list, &QAbstractItemModel::rowsInserted, this, &PlaylistTreeModel::rebuild);
    connect(list, &QAbstractItemModel::rowsRemoved, this, &PlaylistTreeModel::rebuild);
    connect(list, &QAbstractItemModel::dataChanged, this, &PlaylistTreeModel::refreshRows);
    connect(library, &PlaylistLibrary::categoriesChanged, this, &PlaylistTreeModel::rebuild);
    rebuild();
}

void PlaylistTreeModel::setFilter(const QString& text) {
    if (text == m_filter) {
        return;
    }
    m_filter = text;
    rebuild();
}

int PlaylistTreeModel::addNode(const Node& node, int parent) {
    m_nodes.push_back(node);
    const int id = m_nodes.size() - 1;
    m_nodes[id].parent = parent;
    m_nodes[parent].children.push_back(id);
    return id;
}

void PlaylistTreeModel::rebuild() {
    beginResetModel();
    m_nodes.clear();
    m_nodes.push_back(Node{}); // root

    const PlaylistListModel* list = m_library->listModel();
    QVector<qint64> playlists;
    for (int row = 0; row < list->rowCount(); ++row) {
        playlists << list->idAt(row);
    }
    // Search runs on the library's own metadata (SQL over the .mtv).
    QSet<qint64> matched;
    QSet<qint64> matchedCategories;
    const bool filtering = isFiltering();
    if (filtering) {
        matched = m_library->searchPlaylists(m_filter, &matchedCategories);
    }

    QVector<QPair<qint64, QList<qint64>>> categories;
    for (const CategoryInfo& c : m_library->categories()) {
        QList<qint64> members = m_library->playlistsIn(c.id);
        if (filtering && !matchedCategories.contains(c.id)) {
            // Category name doesn't match: show only its matching playlists.
            members.erase(std::remove_if(members.begin(), members.end(),
                                         [&matched](qint64 id) { return !matched.contains(id); }),
                          members.end());
            if (members.isEmpty()) {
                continue;
            }
        }
        categories.push_back({c.id, members});
    }
    QVector<qint64> uncategorized;
    for (qint64 id : playlists) {
        if (m_library->categoriesOf(id).isEmpty() && (!filtering || matched.contains(id))) {
            uncategorized << id;
        }
    }

    if (!categories.isEmpty()) {
        const int header = addNode(Node{HeaderNode}, 0);
        for (const auto& c : categories) {
            Node cat{CategoryNode, c.first};
            const int catNode = addNode(cat, header);
            for (qint64 pl : c.second) {
                Node p{PlaylistNode, pl, c.first};
                addNode(p, catNode);
            }
        }
    }
    // Shown whenever there are playlists outside every category - and as
    // the only group when no category exists, so nothing is ever hidden.
    if (!uncategorized.isEmpty() || (!filtering && m_library->categories().isEmpty() && !playlists.isEmpty())) {
        Node h{HeaderNode};
        h.uncategorized = true;
        const int header = addNode(h, 0);
        for (qint64 pl : uncategorized) {
            Node p{PlaylistNode, pl, 0};
            addNode(p, header);
        }
    }
    endResetModel();
}

void PlaylistTreeModel::refreshRows() {
    for (int n = 1; n < m_nodes.size(); ++n) {
        const QModelIndex idx = indexForNode(n);
        emit dataChanged(idx, idx);
    }
}

int PlaylistTreeModel::rowInParent(int node) const {
    const int parent = m_nodes[node].parent;
    return parent < 0 ? 0 : static_cast<int>(m_nodes[parent].children.indexOf(node));
}

QModelIndex PlaylistTreeModel::indexForNode(int node) const {
    if (node <= 0 || node >= m_nodes.size()) {
        return {};
    }
    return createIndex(rowInParent(node), 0, static_cast<quintptr>(node));
}

QModelIndex PlaylistTreeModel::index(int row, int column, const QModelIndex& parent) const {
    const int p = parent.isValid() ? static_cast<int>(parent.internalId()) : 0;
    if (column != 0 || p >= m_nodes.size() || row < 0 || row >= m_nodes[p].children.size()) {
        return {};
    }
    return createIndex(row, 0, static_cast<quintptr>(m_nodes[p].children[row]));
}

QModelIndex PlaylistTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    return indexForNode(m_nodes[static_cast<int>(child.internalId())].parent);
}

int PlaylistTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) {
        return 0;
    }
    const int p = parent.isValid() ? static_cast<int>(parent.internalId()) : 0;
    return p < m_nodes.size() ? m_nodes[p].children.size() : 0;
}

int PlaylistTreeModel::columnCount(const QModelIndex&) const {
    return 1;
}

QModelIndex PlaylistTreeModel::playlistSourceIndex(qint64 playlistId) const {
    const PlaylistListModel* list = m_library->listModel();
    const int row = list->rowOf(playlistId);
    return row >= 0 ? list->index(row) : QModelIndex();
}

QVariant PlaylistTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const Node& n = m_nodes[static_cast<int>(index.internalId())];
    if (role == KindRole) {
        return n.kind;
    }
    switch (n.kind) {
    case HeaderNode:
        if (role == Qt::DisplayRole) {
            return n.uncategorized ? tr("Uncategorized") : tr("Categories");
        }
        if (role == IsUncategorizedRole) {
            return n.uncategorized;
        }
        return {};
    case CategoryNode: {
        const int count = m_library->playlistsIn(n.id).size();
        switch (role) {
        case Qt::DisplayRole:
        case Qt::EditRole:
            return m_library->categoryName(n.id);
        case Qt::AccessibleTextRole:
            return tr("%1, category, %2").arg(m_library->categoryName(n.id),
                                              count == 1 ? tr("1 playlist") : tr("%1 playlists").arg(count));
        case CategoryIdRole:
            return n.id;
        case PlaylistListModel::CountRole:
            return count;
        default:
            return {};
        }
    }
    case PlaylistNode:
        if (role == CategoryIdRole) {
            return n.parentCategory;
        }
        // Everything else is the playlist's own data (one source of truth).
        return playlistSourceIndex(n.id).data(role);
    }
    return {};
}

Qt::ItemFlags PlaylistTreeModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    const Node& n = m_nodes[static_cast<int>(index.internalId())];
    switch (n.kind) {
    case HeaderNode:
        return Qt::ItemIsEnabled | (n.uncategorized ? Qt::ItemIsDropEnabled : Qt::NoItemFlags);
    case CategoryNode:
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDropEnabled;
    case PlaylistNode:
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled;
    }
    return Qt::NoItemFlags;
}

bool PlaylistTreeModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::EditRole || !index.isValid()) {
        return false;
    }
    const Node& n = m_nodes[static_cast<int>(index.internalId())];
    const QString name = value.toString().trimmed();
    if (name.isEmpty() || name == index.data(Qt::DisplayRole).toString()) {
        return false;
    }
    if (n.kind == PlaylistNode) {
        return m_library->renamePlaylist(n.id, name); // the one rename path
    }
    if (n.kind == CategoryNode) {
        return m_library->renameCategory(n.id, name);
    }
    return false;
}

QModelIndex PlaylistTreeModel::indexOfPlaylist(qint64 playlistId, qint64 underCategory) const {
    QModelIndex fallback;
    for (int n = 1; n < m_nodes.size(); ++n) {
        if (m_nodes[n].kind == PlaylistNode && m_nodes[n].id == playlistId) {
            if (underCategory < 0 || m_nodes[n].parentCategory == underCategory) {
                return indexForNode(n);
            }
            if (!fallback.isValid()) {
                fallback = indexForNode(n);
            }
        }
    }
    return fallback;
}

QModelIndex PlaylistTreeModel::indexOfCategory(qint64 categoryId) const {
    for (int n = 1; n < m_nodes.size(); ++n) {
        if (m_nodes[n].kind == CategoryNode && m_nodes[n].id == categoryId) {
            return indexForNode(n);
        }
    }
    return {};
}

QModelIndexList PlaylistTreeModel::groupIndexes() const {
    QModelIndexList result;
    for (int n = 1; n < m_nodes.size(); ++n) {
        if (m_nodes[n].kind != PlaylistNode) {
            result << indexForNode(n);
        }
    }
    return result;
}

// ------------------------------------------------------------ drag & drop

QStringList PlaylistTreeModel::mimeTypes() const {
    return {QString::fromLatin1(kPlaylistRefMime)};
}

QMimeData* PlaylistTreeModel::mimeData(const QModelIndexList& indexes) const {
    for (const QModelIndex& idx : indexes) {
        const Node& n = m_nodes[static_cast<int>(idx.internalId())];
        if (n.kind == PlaylistNode) {
            auto* mime = new QMimeData;
            // "<playlist id>:<category it was dragged from, 0 = none>"
            mime->setData(QString::fromLatin1(kPlaylistRefMime),
                          QByteArray::number(n.id) + ':' + QByteArray::number(n.parentCategory));
            return mime;
        }
    }
    return nullptr;
}

Qt::DropActions PlaylistTreeModel::supportedDropActions() const {
    return Qt::MoveAction | Qt::CopyAction;
}

Qt::DropActions PlaylistTreeModel::supportedDragActions() const {
    return Qt::MoveAction | Qt::CopyAction;
}

qint64 PlaylistTreeModel::dropCategory(const QModelIndex& target) const {
    if (!target.isValid()) {
        return -1;
    }
    const Node& n = m_nodes[static_cast<int>(target.internalId())];
    switch (n.kind) {
    case CategoryNode:
        return n.id;
    case HeaderNode:
        return n.uncategorized ? 0 : -1;
    case PlaylistNode:
        return n.parentCategory; // dropped onto a sibling: its group
    }
    return -1;
}

bool PlaylistTreeModel::canDropMimeData(const QMimeData* data, Qt::DropAction, int, int,
                                        const QModelIndex& parent) const {
    return data && data->hasFormat(QString::fromLatin1(kPlaylistRefMime)) && dropCategory(parent) >= 0;
}

bool PlaylistTreeModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                                     const QModelIndex& parent) {
    if (!canDropMimeData(data, action, row, column, parent)) {
        return false;
    }
    const QList<QByteArray> parts = data->data(QString::fromLatin1(kPlaylistRefMime)).split(':');
    if (parts.size() != 2) {
        return false;
    }
    const qint64 playlistId = parts[0].toLongLong();
    const qint64 from = parts[1].toLongLong();
    const qint64 to = dropCategory(parent);
    if (playlistId <= 0 || to == from) {
        return false;
    }
    const bool move = action == Qt::MoveAction;
    // After the drop event finishes - this rebuilds the tree under the view.
    QTimer::singleShot(0, m_library, [library = m_library, playlistId, from, to, move] {
        if (to > 0) {
            library->addPlaylistToCategory(to, playlistId);
        }
        if (from > 0 && (move || to == 0)) {
            library->removePlaylistFromCategory(from, playlistId);
        }
    });
    return true;
}
