#pragma once

#include <QAbstractItemModel>
#include <QSet>
#include <QVector>

class PlaylistLibrary;

// The Playlists window's sidebar as a tree - the same playlists as
// PlaylistListModel, grouped for finding them:
//
//   CATEGORIES
//     Anime                 (a category: organizes playlists, holds no media)
//       Anime Wallpapers    (an existing playlist)
//       Naruto
//     Games
//       GTA
//   UNCATEGORIZED
//     Cars                  (playlists in no category)
//
// A playlist in several categories appears under each of them - the same
// playlist (same id) every time, never a copy. Playlist rows answer the
// PlaylistListModel roles, so the existing row delegate draws them.
//
// Drag a playlist onto a category to file it there (moves it out of the
// category it was dragged from; hold Ctrl to also keep it there). Dropping
// on UNCATEGORIZED removes it from the category it was dragged from.
class PlaylistTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Kind { HeaderNode = 1, CategoryNode, PlaylistNode };
    enum Roles {
        KindRole = Qt::UserRole + 50,
        CategoryIdRole,     // category node: its id; playlist node: the category it sits under (0 = Uncategorized)
        IsUncategorizedRole // header node: true for UNCATEGORIZED
    };

    explicit PlaylistTreeModel(PlaylistLibrary* library, QObject* parent = nullptr);

    // Search (playlist name, type, category name - see
    // LibraryDatabase::searchPlaylists). Empty = show everything.
    void setFilter(const QString& text);
    bool isFiltering() const { return !m_filter.trimmed().isEmpty(); }

    // First node showing this playlist (preferring the one under
    // `underCategory`), or invalid.
    QModelIndex indexOfPlaylist(qint64 playlistId, qint64 underCategory = -1) const;
    QModelIndex indexOfCategory(qint64 categoryId) const;
    // Every category and header node (for expanding after a rebuild).
    QModelIndexList groupIndexes() const;

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDropActions() const override;
    Qt::DropActions supportedDragActions() const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                         const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override;

private:
    struct Node {
        Kind kind = HeaderNode;
        qint64 id = 0;             // playlist / category id
        qint64 parentCategory = 0; // playlist nodes: the category above (0 = Uncategorized)
        bool uncategorized = false;
        int parent = -1;
        QVector<int> children;
    };
    void rebuild();
    void refreshRows(); // same structure, data changed (counts, active, names)
    int addNode(const Node& node, int parent);
    int rowInParent(int node) const;
    QModelIndex indexForNode(int node) const;
    QModelIndex playlistSourceIndex(qint64 playlistId) const;
    // Category a drop on `target` files the playlist under: >0 a category,
    // 0 = Uncategorized, -1 = not a drop target.
    qint64 dropCategory(const QModelIndex& target) const;

    PlaylistLibrary* m_library;
    QString m_filter;
    QVector<Node> m_nodes; // [0] = invisible root
};
