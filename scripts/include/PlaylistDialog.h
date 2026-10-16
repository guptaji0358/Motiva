#pragma once

#include <functional>
#include <QHash>

#include <QWidget>
#include <QByteArray>
#include <QListView>
#include <QStringList>
#include "LibraryDatabase.h" // PlaylistFilterInfo

class BackupManager;
class IconButton;
class QAction;
class CategoryTreePicker;
class QMenu;
class QAction;
class PlaylistLibrary;
class PlaylistModel;
class QLineEdit;
class QTimer;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QWidget;
class SettingsManager;

// Card strip for one playlist (images or videos): cards in playlist order,
// reorderable by dragging a card to a new position (drop indicator drawn
// between cards), and a drop target for supported files from Explorer.
// Keyboard: arrows select, Ctrl+Left/Right move the selected item, Enter
// shows it now, Delete removes it.
class PlaylistView : public QListView {
    Q_OBJECT
public:
    explicit PlaylistView(QWidget* parent = nullptr);
    void setPlaylist(PlaylistModel* playlist);
    PlaylistModel* playlist() const { return m_playlist; }

    // View-only arrangement of the cards by file name. The playlist's own
    // order (what wallpaper playback follows) is never touched: None shows
    // that order, A-Z / Z-A only change how the cards are laid out here.
    // While arranged, dragging/moving cards is off (it would be ambiguous).
    enum class Arrangement { None, AtoZ, ZtoA };
    void setArrangement(Arrangement arrangement);
    Arrangement arrangement() const { return m_arrangement; }
    bool isArranged() const { return m_arrangement != Arrangement::None; }

    // Everything below speaks in PLAYLIST rows (the model's rows), whatever
    // order the cards are currently shown in.
    int sourceRow(const QModelIndex& viewIndex) const;
    QModelIndex viewIndex(int row) const;
    void setSourceRowHidden(int row, bool hidden);
    bool isSourceRowHidden(int row) const;
    // Nearest playlist row in direction `step` (+1/-1) that is not hidden by
    // the playlist's category filter or search; -1 if none.
    int visibleNeighbour(int row, int step) const;

signals:
    void filesDropped(const QStringList& paths, int insertRow);
    void moveRequested(int from, int to);
    void showNowRequested(int row);
    void removeRequested(int row);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    int insertPositionAt(const QPoint& pos) const;
    QStringList acceptedLocalFiles(const class QMimeData* mime) const;

    PlaylistModel* m_playlist = nullptr;
    QSortFilterProxyModel* m_proxy = nullptr;
    Arrangement m_arrangement = Arrangement::None;
    int m_dropIndicatorPos = -1;
};

// "Playlists" window: the user-facing side of PlaylistLibrary. Left: My
// Playlists (create/rename/delete, which one is active). Right: the
// selected playlist's items, actions and settings. A dedicated top-level
// window (Qt::Window, owned by MainWindow, one instance that MainWindow
// re-activates), not a modal dialog. Everything here edits the library; MainWindow reacts to
// the library's signals to keep the actual wallpaper in sync, so this
// window never touches the wallpaper pipeline itself.
class PlaylistDialog : public QWidget {
    Q_OBJECT
public:
    PlaylistDialog(PlaylistLibrary* library, QWidget* parent = nullptr);

    // Adds files to a playlist (picker, drops, Explorer "Add to Motiva
    // playlist") and reports the outcome in the window.
    void addFilesTo(qint64 playlistId, const QStringList& paths, int insertRow = -1);
    void selectPlaylist(qint64 id);

    // MainWindow tells this window whether the active playlist is what's on
    // the desktop right now, so the apply button never claims otherwise.
    void setActiveOnDesktop(bool onDesktop);

    // For "Restore from Backup" in the missing-file banner (optional - without
    // a backup manager the button simply never appears).
    void setBackupManager(BackupManager* backup);

    // Remembers the window's size/position (playlistWindow/geometry) and
    // restores it now, kept on a visible screen. Optional.
    void setSettings(SettingsManager* settings);

    // "New Image Playlist" (Ctrl+Alt+N) / "New Video Playlist"
    // (Ctrl+Shift+N): the normal name-then-create flow, no type menu first.
    // Also used by MainWindow's copies of the same shortcuts.
    void createPlaylist(bool video);
    // "New Mixed Playlist" (Ctrl+Alt+M): images and videos in one order.
    void createMixedPlaylist();
    // True while a text field has keyboard focus - shortcuts then leave the
    // keys to normal text editing.
    static bool isTextInputFocused();

signals:
    // "Set as Wallpaper": MainWindow activates the playlist and applies its
    // current item through the normal Set as Wallpaper path.
    // categoryId 0 = the whole playlist ("All").
    void applyToDesktopRequested(qint64 playlistId, qint64 categoryId);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    // Esc closes the window (what QDialog did); editors and popups that use Esc
    // themselves consume it first, so it only gets here when nothing else wants it.
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    // Delete / D on the My Playlists list (only while the list itself has
    // focus - an inline rename editor or any text field keeps its keys).
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyThemedIcons(); // add/search SVGs for the active theme
    void buildUi();
    QWidget* buildSidebar();
    QWidget* buildEditor();
    void onCreatePlaylist(int type);
    // Inline rename of the selected playlist (double-click, F2, context
    // menu). The editor commits through PlaylistListModel::setData ->
    // PlaylistLibrary::renamePlaylist - the single rename path.
    void startInlineRename();
    void onDeletePlaylist();
    void showPlaylistMenu(const QPoint& pos);
    void showItemMenu(const QPoint& pos);
    void openFlow();
    // "Find File" for a missing item (see MediaRecoveryDialog).
    void onFindFile(int row);
    // "Find All at Once": every missing file of the open playlist in ONE
    // coordinated search (see BulkMediaRecoveryDialog).
    void onFindAll();
    void offerRemovalAfterFailedSearch(qint64 itemId, const QString& path);
    void updateMissingBanner(int selectedRow);
    void onImport();
    void onExport();
    void onAddMedia();
    void onRemove();
    void onClear();
    void onMove(int delta);
    void removeRow(int row);
    void moveRow(int row, int delta);
    void onShowNow();
    void onSelectionInSidebar();
    void onRotationEdited();
    void bindSelectedPlaylist();
    void updateUi();
    int selectedRow() const;
    void selectRow(int row);
    void showNotice(const QString& text);
    // Reports an event through the unified NotificationManager. When no
    // in-app toast is shown (no window to host it, or a repeat) `inlineText`
    // goes to this window's own notice line instead, so the existing
    // feedback is never lost and never doubled. If the user switched the
    // notification off, nothing more is shown.
    void announce(int kind, const QString& title, const QString& message, const QString& key,
                  const QString& inlineText, bool desktop = false, bool background = false);
    PlaylistModel* current() const;
    qint64 selectedCategoryId() const; // 0 = All
    // True when the open playlist is active AND limited to the category selected here.
    bool isSelectionActive() const;
    void activateSelection(); // setActive(open playlist, selected category)

    // --- virtual categories: saved filters of the open playlist ---
    // Reloads the Category selector for the open playlist; resetToAll picks
    // "All" (every playlist opens on All).
    void reloadCategories(bool resetToAll);
    const PlaylistFilterInfo* currentFilter() const;
    // Hides the rows outside the selected category + search (the rows stay
    // in the playlist; only the view hides them).
    void applyItemFilter();
    // playlist_items.id of the items in the selected category (all of them for
    // "All"), optionally narrowed by `search`.
    QSet<qint64> categoryItemIds(const QString& search) const;
    void showCategoryMenu(const QPoint& globalPos);
    // parentId > 0 builds the new category as a subcategory of that one.
    void onBuildCategory(qint64 parentId = 0);
    void onMoveCategory(qint64 categoryId, qint64 newParentId);
    // "Add to Category": adds items of the open playlist to a hand-picked category.
    void assignItemsToCategory(qint64 categoryId, const QSet<qint64>& itemIds);
    void onNewCategoryFromItems(const QSet<qint64>& itemIds);
    // Adds the categories below `parentId` to `menu` as a hierarchy (a category
    // with subcategories becomes a submenu whose first entry is the category
    // itself). `actions` maps every added action to its category id.
    void addCategoryTree(QMenu* menu, qint64 parentId, const QSet<qint64>& excluded,
                         QHash<QAction*, qint64>& actions,
                         const std::function<void(const PlaylistFilterInfo&, QAction*)>& decorate);
    void onEditCategory();
    void onRenameCategory();
    void onDeleteCategory();

    void saveGeometryLater();
    void restoreSavedGeometry();

    PlaylistLibrary* m_library;
    SettingsManager* m_settings = nullptr;
    QTimer* m_geometryTimer = nullptr;
    bool m_onDesktop = false;
    bool m_binding = false;

    QListView* m_playlistList = nullptr;

    QLabel* m_categoryLabel = nullptr;
    CategoryTreePicker* m_categoryPicker = nullptr;
    QCheckBox* m_includeSubCheck = nullptr;
    QLabel* m_rotationLabel = nullptr;
    IconButton* m_newMixedButton = nullptr;
    IconButton* m_categoryMenuButton = nullptr;
    IconButton* m_newImageButton = nullptr;
    IconButton* m_newVideoButton = nullptr;
    IconButton* m_importButton = nullptr;
    IconButton* m_exportButton = nullptr;
    IconButton* m_buildCategoryButton = nullptr;
    QComboBox* m_arrangeCombo = nullptr;
    QPushButton* m_flowButton = nullptr;
    QLineEdit* m_itemSearch = nullptr;
    QTimer* m_itemSearchTimer = nullptr;
    QTimer* m_filterTimer = nullptr; // coalesces re-filtering after playlist changes
    QLabel* m_showingLabel = nullptr;
    qint64 m_filtersPlaylistId = 0; // playlist whose categories are loaded
    QVector<PlaylistFilterInfo> m_filters;

    QStackedWidget* m_editorStack = nullptr;
    QLabel* m_emptyLabel = nullptr;
    QLabel* m_nameLabel = nullptr;
    QLabel* m_summaryLabel = nullptr;
    QCheckBox* m_activeCheck = nullptr;
    PlaylistView* m_view = nullptr;
    QLabel* m_hintLabel = nullptr;
    QLabel* m_noticeLabel = nullptr;
    IconButton* m_addButton = nullptr;
    QAction* m_searchAction = nullptr;
    QPushButton* m_showNowButton = nullptr;
    QPushButton* m_clearButton = nullptr;
    QFrame* m_missingBanner = nullptr;
    QLabel* m_missingTitle = nullptr;
    QLabel* m_missingText = nullptr;
    QPushButton* m_findFileButton = nullptr;
    QPushButton* m_findAllButton = nullptr;
    QPushButton* m_missingRemoveButton = nullptr;
    bool m_recoveryRunning = false;
    BackupManager* m_backup = nullptr;
    QPushButton* m_restoreBackupButton = nullptr;
    void onRestoreFromBackup(int row);
    QPushButton* m_applyButton = nullptr;
    QStackedWidget* m_settingsStack = nullptr;
    QCheckBox* m_unlockCheck = nullptr;
    QCheckBox* m_startCheck = nullptr;
    QCheckBox* m_intervalCheck = nullptr;
    QComboBox* m_intervalCombo = nullptr;
};
