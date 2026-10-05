#pragma once

#include <QDialog>
#include <QList>

class PlaylistLibrary;
class QLabel;
class QLineEdit;
class QCheckBox;
class QPushButton;

// "Build Category": name a category and tick the existing playlists that
// belong in it - grouping playlists so they're easier to find. Nothing is
// copied or changed; the category only links to the playlists.
//
// With an existing categoryId it edits that category's playlists instead
// ("Choose Playlists…").
class CategoryBuilderDialog : public QDialog {
    Q_OBJECT
public:
    CategoryBuilderDialog(PlaylistLibrary* library, qint64 categoryId, QWidget* parent = nullptr);
    // Ticks these playlists up front (e.g. the one that was right-clicked).
    void preselect(const QList<qint64>& playlistIds);
    qint64 categoryId() const { return m_categoryId; }

protected:
    void accept() override;

private:
    void applyFilter();
    void updateState();
    QList<qint64> checkedPlaylists() const;

    PlaylistLibrary* m_library;
    qint64 m_categoryId;
    QLineEdit* m_nameEdit = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    // Real checkboxes (not checkable list rows): screen readers' Toggle
    // works on these, while on item-view rows it only changes selection.
    QList<QCheckBox*> m_boxes;
    QLabel* m_selectedLabel = nullptr;
    QPushButton* m_okButton = nullptr;
};
