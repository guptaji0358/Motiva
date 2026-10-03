#pragma once

#include <QDialog>
#include <QListView>
#include <QStringList>

class ImagePlaylist;
class PlaylistRotation;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

// Thumbnail strip for the image playlist: cards in playlist order,
// reorderable by dragging a card to a new position (drop indicator drawn
// between cards), and a drop target for image files from Explorer.
// Keyboard: arrows select, Ctrl+Left/Right move the selected image,
// Enter shows it now, Delete removes it.
class PlaylistView : public QListView {
    Q_OBJECT
public:
    explicit PlaylistView(ImagePlaylist* playlist, QWidget* parent = nullptr);

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
    // Insert-before position (0..count) for a drop at `pos`.
    int insertPositionAt(const QPoint& pos) const;
    static QStringList supportedLocalFiles(const class QMimeData* mime);

    ImagePlaylist* m_playlist;
    int m_dropIndicatorPos = -1;
};

// "Image Playlist" window - the user-facing side of ImagePlaylist and
// PlaylistRotation. Non-modal, like SettingsDialog. Everything here edits
// the model/rotation objects directly; MainWindow reacts to their signals
// to keep the actual wallpaper in sync, so this window never touches the
// wallpaper pipeline itself.
class PlaylistDialog : public QDialog {
    Q_OBJECT
public:
    PlaylistDialog(ImagePlaylist* playlist, PlaylistRotation* rotation, QWidget* parent = nullptr);

    // Adds files (Add Images picker, drops, Explorer "Add to Motiva
    // playlist") and reports the outcome in the window.
    void addFiles(const QStringList& paths, int insertRow = -1);

    // MainWindow tells this window whether the playlist is what's on the
    // desktop right now, so the apply button never claims otherwise.
    void setPlaylistOnDesktop(bool onDesktop);

signals:
    // "Set Playlist as Wallpaper": MainWindow turns the playlist on and
    // applies its current image through the normal Set as Wallpaper path.
    void applyToDesktopRequested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void onAddImages();
    void onRemove();
    void onClear();
    void onMove(int delta);
    void onShowNow();
    void updateUi();
    int selectedRow() const;
    void selectRow(int row);
    void showNotice(const QString& text);

    ImagePlaylist* m_playlist;
    PlaylistRotation* m_rotation;
    bool m_onDesktop = false;

    PlaylistView* m_view = nullptr;
    QCheckBox* m_enabledCheck = nullptr;
    QLabel* m_summaryLabel = nullptr;
    QLabel* m_noticeLabel = nullptr;
    QPushButton* m_addButton = nullptr;
    QPushButton* m_moveLeftButton = nullptr;
    QPushButton* m_moveRightButton = nullptr;
    QPushButton* m_showNowButton = nullptr;
    QPushButton* m_removeButton = nullptr;
    QPushButton* m_clearButton = nullptr;
    QPushButton* m_applyButton = nullptr;
    QCheckBox* m_unlockCheck = nullptr;
    QCheckBox* m_startCheck = nullptr;
    QCheckBox* m_intervalCheck = nullptr;
    QComboBox* m_intervalCombo = nullptr;
};
