#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class SettingsManager;

// The user's ordered image wallpaper playlist (Motiva v1.1.0).
//
// Pure data + persistence: an ordered list of still-image paths, which one
// is "current", and whether the playlist is on ("Use image playlist"). It
// never touches the wallpaper itself - MainWindow listens to
// currentChanged/enabledChanged and feeds the current image through the
// existing VideoPlayer/WallpaperManager pipeline, exactly like any other
// current media. PlaylistRotation decides WHEN to call advance().
//
// Deliberately distinct from "current media": opening a single image or
// video (Open Media, drag & drop on the main window, Explorer "Set as
// background") does not touch this list; MainWindow just switches the
// playlist off so rotation can't replace the user's explicit pick.
//
// Files are only ever referenced, never moved/renamed/deleted. A file
// that disappears (deleted, renamed, external drive unplugged) stays in
// the list flagged unavailable (AvailableRole) and is skipped by
// advance(), so the user's playlist is never silently destroyed.
//
// A QAbstractListModel so PlaylistDialog's view can show it directly.
class ImagePlaylist : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        PathRole = Qt::UserRole + 1, // QString: native absolute path
        AvailableRole,               // bool: file currently exists
        IsCurrentRole,               // bool: row == currentIndex()
        ThumbnailRole,               // QImage: small preview (null until loaded)
    };

    struct AddResult {
        int added = 0;
        int duplicates = 0;   // already in the playlist (or repeated in the same batch)
        int unsupported = 0;  // not an existing file of a supported still-image format
    };

    explicit ImagePlaylist(SettingsManager* settings, QObject* parent = nullptr);

    // QAbstractListModel
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    int count() const { return m_items.size(); }
    QString pathAt(int row) const;
    bool isAvailable(int row) const;
    int availableCount() const;

    int currentIndex() const { return m_current; }
    QString currentPath() const { return pathAt(m_current); }

    bool isEnabled() const { return m_enabled; }
    // Turning on requires at least one available image; if the current
    // one is missing, moves to the next available first. Returns the
    // resulting state.
    bool setEnabled(bool enabled);

    // Adds supported, existing image files, skipping duplicates (compared
    // case-insensitively on the absolute path, as Windows does).
    // insertRow < 0 appends.
    AddResult addImages(const QStringList& paths, int insertRow = -1);
    void removeAt(int row);
    void clear();
    // Moves the item at `from` so it ends up at index `to` (QList::move
    // semantics). The current image stays current, wherever it moves.
    bool move(int from, int to);

    // Makes `row` the current image ("Show now"). Fails for a missing file.
    bool setCurrentIndex(int row);
    // Next available image in order, wrapping last -> first. Returns false
    // (current unchanged) if no other image is available.
    bool advance();

    // Re-checks every file's existence (e.g. when the playlist window is
    // opened, after a drive was reconnected). Never removes anything.
    void refreshAvailability();

    // True for a file name/path whose extension is a decodable still
    // image (VideoPlayer::supportedStaticImageExtensions).
    static bool isSupportedImage(const QString& path);

signals:
    // The current image changed (advance, Show now, removal of the current
    // image, first image added to an empty list). When the playlist is on,
    // MainWindow presents currentPath() in response.
    void currentChanged();
    void enabledChanged(bool enabled);
    // Count, order or availability changed - for summary labels.
    void contentsChanged();

private:
    struct Item {
        QString path; // native separators, absolute
        QString key;  // normalized, case-folded - duplicate/thumbnail key
        bool available = true;
    };

    static QString normalizedKey(const QString& path);
    void save() const;
    void requestThumbnail(const QString& key, const QString& path) const;
    void onThumbnailReady(const QString& key, const QImage& image);
    // Next available row after `from` in playlist order (wrapping), or -1.
    int nextAvailableAfter(int from);
    void updateAvailability(int row);

    SettingsManager* m_settings;
    QVector<Item> m_items;
    int m_current = -1;
    bool m_enabled = false;

    // Small, aspect-preserving previews keyed by Item::key, decoded off the
    // GUI thread on first request.
    mutable QHash<QString, QImage> m_thumbnails;
    mutable QSet<QString> m_pendingThumbnails;
};
