#pragma once

#include "LibraryDatabase.h"

#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

// One playlist of the Motiva library (.mtv), as an ordered list model for
// PlaylistDialog's card view and for the playlist controller.
//
// Every change is written to the library database FIRST (in a transaction
// - see LibraryDatabase); the in-memory list only changes once the write
// succeeded, so what the UI shows is always what is on disk. On a failed
// write nothing changes and errorOccurred() carries the reason.
//
// Media files are only referenced, never moved/renamed/deleted. A file that
// disappears stays listed and flagged (AvailableRole) - advance() skips it,
// and nothing is silently removed from the database.
class PlaylistModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        PathRole = Qt::UserRole + 1, // QString: native absolute path
        AvailableRole,               // bool: file currently exists
        IsCurrentRole,               // bool: row == currentIndex()
        ThumbnailRole,               // QImage: small preview (null until loaded, lazily)
        IsVideoRole,                 // bool
        ThumbnailPendingRole,        // bool: a preview is still being produced
    };

    struct AddResult {
        int added = 0;
        int duplicates = 0;  // already in this playlist (or repeated in the batch)
        int unsupported = 0; // missing, or not a supported file of this playlist's type
        bool failed = false; // database write failed - nothing was added
    };

    PlaylistModel(LibraryDatabase* db, const PlaylistInfo& info, QObject* parent = nullptr);

    // QAbstractListModel
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    qint64 id() const { return m_id; }
    QString name() const { return m_name; }
    PlaylistType type() const { return m_type; }
    bool isVideo() const { return m_type == PlaylistType::Video; }
    RotationSettings rotation() const { return m_rotation; }

    // Set by PlaylistLibrary: this playlist currently controls the wallpaper.
    bool isActive() const { return m_active; }

    int count() const { return m_items.size(); }
    QString pathAt(int row) const;
    bool isAvailable(int row) const;
    int availableCount() const;
    int currentIndex() const { return m_current; }
    QString currentPath() const { return pathAt(m_current); }
    qint64 mediaIdAt(int row) const;
    qint64 itemIdAt(int row) const;
    int rowOfItem(qint64 itemId) const;
    MediaFacts factsAt(int row) const;
    bool containsMedia(qint64 mediaId) const;

    // Re-reads this playlist's items from the library (after a media record
    // was relocated or merged). Order and current item come from the
    // database, so nothing about the playlist itself changes.
    void reloadFromDatabase();

    // Whether `path` is a supported, existing file for this playlist's type.
    bool acceptsFile(const QString& path) const;
    static bool isImageFile(const QString& path);
    static bool isVideoFile(const QString& path); // backend video containers (not GIF)

    AddResult addFiles(const QStringList& paths, int insertRow = -1);
    bool removeAt(int row);
    bool clear();
    bool move(int from, int to); // QList::move semantics; current image stays current
    bool setCurrentIndex(int row);
    // Next available item in order. wrap=false stops at the last item.
    // Returns false (current unchanged) if there is nowhere to go.
    bool advance(bool wrap = true);
    // Makes sure the current item is an available file, moving to the next
    // available one if not. False if no item is available.
    bool ensureCurrentAvailable();
    void refreshAvailability();

signals:
    void currentChanged();
    void contentsChanged(); // count, order, availability
    void errorOccurred(const QString& message);

private:
    friend class PlaylistLibrary;
    void setActive(bool active);
    void setNameLocal(const QString& name) { m_name = name; }
    void setRotationLocal(const RotationSettings& r) { m_rotation = r; }

    struct Item {
        qint64 itemId = 0;
        qint64 mediaId = 0;
        QString path;
        QString key;
        bool available = true;
        MediaFacts facts;
    };

    void loadItems();

    bool persistCurrent();
    int nextAvailableAfter(int from, bool wrap);
    void updateAvailability(int row);
    void emitRowChanged(int row);
    void requestThumbnail(const QString& key, const QString& path) const;
    void onThumbnailReady(const QString& key, const QImage& image);

    LibraryDatabase* m_db;
    qint64 m_id;
    QString m_name;
    PlaylistType m_type;
    RotationSettings m_rotation;
    bool m_active = false;
    QVector<Item> m_items;
    int m_current = -1;

    mutable QHash<QString, QImage> m_thumbnails;
    mutable QSet<QString> m_pendingThumbnails;
    QSet<QString> m_noThumbnail; // no preview obtainable - don't retry
};
