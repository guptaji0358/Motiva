#pragma once

#include <QDialog>
#include <QSet>

class PlaylistLibrary;
class PlaylistModel;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// "Select from Playlist": hand-pick which of the open playlist's items
// belong to a category. Lists ONLY that playlist's items; nothing is copied -
// the result is a set of playlist_items ids. Used both to create a category
// (with a name) and to manage the selection of an existing one (names are
// changed with Rename). The search box only narrows what is shown here; it
// never changes the playlist or the saved category.
class CategoryItemPickerDialog : public QDialog {
    Q_OBJECT
public:
    // existingName empty -> creating: the dialog asks for a name.
    CategoryItemPickerDialog(PlaylistLibrary* library, PlaylistModel* playlist, const QString& existingName,
                             const QSet<qint64>& preselected, qint64 existingId, QWidget* parent = nullptr);
    QString categoryName() const { return m_name; }
    QSet<qint64> selectedItemIds() const;

protected:
    void accept() override;

private:
    void applySearch();
    void updateCount();
    QString nameError() const;

    PlaylistLibrary* m_library;
    PlaylistModel* m_playlist;
    qint64 m_existingId;
    bool m_creating;
    QString m_name;
    QLineEdit* m_nameEdit = nullptr;
    QLineEdit* m_search = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_countLabel = nullptr;
    QLabel* m_errorLabel = nullptr;
    QPushButton* m_okButton = nullptr;
};
