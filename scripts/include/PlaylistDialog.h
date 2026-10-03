#pragma once

#include <QDialog>
#include <QListView>
#include <QStringList>

class PlaylistLibrary;
class PlaylistModel;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QStackedWidget;
class QWidget;

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
    int m_dropIndicatorPos = -1;
};

// "Playlists" window: the user-facing side of PlaylistLibrary. Left: My
// Playlists (create/rename/delete, which one is active). Right: the
// selected playlist's items, actions and settings. Non-modal, like
// SettingsDialog. Everything here edits the library; MainWindow reacts to
// the library's signals to keep the actual wallpaper in sync, so this
// window never touches the wallpaper pipeline itself.
class PlaylistDialog : public QDialog {
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

signals:
    // "Set as Wallpaper": MainWindow activates the playlist and applies its
    // current item through the normal Set as Wallpaper path.
    void applyToDesktopRequested(qint64 playlistId);

protected:
    void showEvent(QShowEvent* event) override;
    // Delete / D on the My Playlists list (only while the list itself has
    // focus - an inline rename editor or any text field keeps its keys).
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
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
    // "Find File" for a missing item (see MediaRecoveryDialog).
    void onFindFile(int row);
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
    PlaylistModel* current() const;

    PlaylistLibrary* m_library;
    bool m_onDesktop = false;
    bool m_binding = false;

    QListView* m_playlistList = nullptr;
    QPushButton* m_newButton = nullptr;

    QStackedWidget* m_editorStack = nullptr;
    QLabel* m_emptyLabel = nullptr;
    QLabel* m_nameLabel = nullptr;
    QLabel* m_summaryLabel = nullptr;
    QCheckBox* m_activeCheck = nullptr;
    PlaylistView* m_view = nullptr;
    QLabel* m_hintLabel = nullptr;
    QLabel* m_noticeLabel = nullptr;
    QPushButton* m_addButton = nullptr;
    QPushButton* m_showNowButton = nullptr;
    QPushButton* m_clearButton = nullptr;
    QFrame* m_missingBanner = nullptr;
    QLabel* m_missingTitle = nullptr;
    QLabel* m_missingText = nullptr;
    QPushButton* m_findFileButton = nullptr;
    QPushButton* m_missingRemoveButton = nullptr;
    bool m_recoveryRunning = false;
    QPushButton* m_applyButton = nullptr;
    QStackedWidget* m_settingsStack = nullptr;
    QCheckBox* m_unlockCheck = nullptr;
    QCheckBox* m_startCheck = nullptr;
    QCheckBox* m_intervalCheck = nullptr;
    QComboBox* m_intervalCombo = nullptr;
};
