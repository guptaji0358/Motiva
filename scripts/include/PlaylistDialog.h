#pragma once

#include <functional>
#include <QHash>

#include <QWidget>
#include <QByteArray>
#include <QStringList>
#include "LibraryDatabase.h" // PlaylistFilterInfo

class BackupManager;
class QAction;
class GlMenu;
class PlaylistLibrary;
class PlaylistModel;
class QTimer;
class QSortFilterProxyModel;
class QMimeData;
class SettingsManager;
class GlCanvas;
class GlItem;
class GlItemView;
class GlPanel;
class GlLabel;
class GlButton;
class GlToggle;
class GlDropdown;
class GlTextInput;
class GlTreeDropdown;

// The cards of one playlist (images, videos or both), shown through a GL
// card grid (GlItemView) in playlist order, reorderable by dragging a card,
// and a drop target for supported files from Explorer (the window forwards
// those drops here). Keyboard: arrows select, Ctrl+Left/Right move the
// selected item, Enter shows it now, Delete removes it.
//
// This is an adapter: it owns the arrangement proxy and translates between
// the grid's rows (what is shown) and PLAYLIST rows (what playback follows).
class PlaylistView : public QObject {
    Q_OBJECT
public:
    PlaylistView(GlCanvas* canvas, QObject* parent = nullptr);
    GlItemView* item() const { return m_item; }
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
    int sourceRow(int viewRow) const;
    int viewRow(int sourceRow) const;
    int selectedSourceRow() const;
    void selectSourceRow(int row);
    void clearSelection();
    void setSourceRowHidden(int row, bool hidden);
    bool isSourceRowHidden(int row) const;
    // Nearest playlist row in direction `step` (+1/-1) that is not hidden by
    // the playlist's category filter or search; -1 if none.
    int visibleNeighbour(int row, int step) const;
    QStringList acceptedLocalFiles(const QMimeData* mime) const;

signals:
    void filesDropped(const QStringList& paths, int insertRow);
    void moveRequested(int from, int to);
    void showNowRequested(int row);
    void removeRequested(int row);
    void itemDoubleClicked(int sourceRow);
    void itemPreviewRequested(int sourceRow); // Shift+click
    void itemContextMenu(const QPoint& global, int sourceRow); // -1 = none
    void selectionChanged();

private:
    bool handleKey(class QKeyEvent* event);

    GlItemView* m_item = nullptr;
    PlaylistModel* m_playlist = nullptr;
    QSortFilterProxyModel* m_proxy = nullptr;
    Arrangement m_arrangement = Arrangement::None;
};

// "Playlists" window: the user-facing side of PlaylistLibrary. Left: My
// Playlists (create/rename/delete, which one is active). Right: the
// selected playlist's items, actions and settings. A dedicated top-level
// window (Qt::Window, owned by MainWindow, one instance that MainWindow
// re-activates), not a modal dialog, drawn on the GL canvas like the main
// window. Everything here edits the library; MainWindow reacts to
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
    // Borderless window: drag / resize through hit-testing (see GlUi).
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    // Files dragged in from Explorer are dropped onto the card grid.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void applyThemedIcons(); // add/search SVGs for the active theme
    void buildUi();
    void layoutGl(const QSizeF& size);
    void updateWindowButtonsState();
    void onCreatePlaylist(int type);
    // Inline rename of the selected playlist (double-click, F2, context
    // menu). Commits through PlaylistListModel::setData ->
    // PlaylistLibrary::renamePlaylist - the single rename path.
    void startInlineRename();
    void finishInlineRename(bool commit);
    void onDeletePlaylist();
    void showPlaylistMenu(const QPoint& global, int row);
    void showItemMenu(const QPoint& global, int sourceRow);
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
    void onAddFolder();
    void previewImage(int row);
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
    void addCategoryTree(GlMenu* menu, qint64 parentId, const QSet<qint64>& excluded,
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

    // --- GL scene ---
    GlCanvas* m_canvas = nullptr;
    GlPanel* m_titlePanel = nullptr;
    GlPanel* m_toolbarPanel = nullptr;
    GlPanel* m_sidebarPanel = nullptr;
    GlPanel* m_editorPanel = nullptr;
    GlLabel* m_titleLabel = nullptr;
    GlLabel* m_sidebarHeading = nullptr;
    GlButton* m_minButton = nullptr;
    GlButton* m_maxButton = nullptr;
    GlButton* m_closeButton = nullptr;
    GlItemView* m_playlistList = nullptr;
    GlTextInput* m_renameEdit = nullptr;
    int m_renameRow = -1;

    GlTreeDropdown* m_categoryPicker = nullptr;
    GlToggle* m_includeSubCheck = nullptr;
    GlLabel* m_rotationLabel = nullptr;
    GlButton* m_newMixedButton = nullptr;
    GlButton* m_categoryMenuButton = nullptr;
    GlButton* m_newImageButton = nullptr;
    GlButton* m_newVideoButton = nullptr;
    GlButton* m_importButton = nullptr;
    GlButton* m_exportButton = nullptr;
    GlButton* m_buildCategoryButton = nullptr;
    GlLabel* m_arrangeLabel = nullptr;
    GlDropdown* m_arrangeCombo = nullptr;
    GlButton* m_flowButton = nullptr;
    GlButton* m_addFolderButton = nullptr;
    GlTextInput* m_itemSearch = nullptr;
    QTimer* m_itemSearchTimer = nullptr;
    QTimer* m_filterTimer = nullptr; // coalesces re-filtering after playlist changes
    GlLabel* m_showingLabel = nullptr;
    qint64 m_filtersPlaylistId = 0; // playlist whose categories are loaded
    QVector<PlaylistFilterInfo> m_filters;

    GlLabel* m_emptyLabel = nullptr;
    GlLabel* m_nameLabel = nullptr;
    GlLabel* m_summaryLabel = nullptr;
    GlToggle* m_activeCheck = nullptr;
    PlaylistView* m_view = nullptr;
    GlLabel* m_hintLabel = nullptr;
    GlLabel* m_noticeLabel = nullptr;
    GlButton* m_addButton = nullptr;
    GlButton* m_showNowButton = nullptr;
    GlButton* m_clearButton = nullptr;
    GlPanel* m_missingBanner = nullptr;
    GlLabel* m_missingTitle = nullptr;
    GlLabel* m_missingText = nullptr;
    GlButton* m_findFileButton = nullptr;
    GlButton* m_findAllButton = nullptr;
    GlButton* m_missingRemoveButton = nullptr;
    bool m_recoveryRunning = false;
    BackupManager* m_backup = nullptr;
    GlButton* m_restoreBackupButton = nullptr;
    void onRestoreFromBackup(int row);
    GlLabel* m_libraryLabel = nullptr;
    GlButton* m_applyButton = nullptr;
    GlButton* m_footerClose = nullptr;
    GlToggle* m_unlockCheck = nullptr;
    GlToggle* m_startCheck = nullptr;
    GlToggle* m_intervalCheck = nullptr;
    GlDropdown* m_intervalCombo = nullptr;
    GlLabel* m_imageNote = nullptr;
    GlLabel* m_videoNote = nullptr;
    bool m_videoSettings = false; // which settings block the open playlist shows
};
