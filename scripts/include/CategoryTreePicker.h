#pragma once

#include "LibraryDatabase.h"

#include <QPushButton>
#include <QSet>
#include <QString>
#include <QVector>

class QFrame;
class QModelIndex;
class QStandardItemModel;
class QTreeView;

// A combo-box-like selector for a playlist's categories that shows them as a
// tree: parent categories can be expanded and collapsed, and the selected
// category is reported by its stable database id (0 = the built-in "All").
// Only presentation - it owns no category data.
class CategoryTreePicker : public QPushButton {
    Q_OBJECT
public:
    explicit CategoryTreePicker(QWidget* parent = nullptr);

    // Rebuilds the tree (`categories` in display order). The selection is
    // kept when that category still exists, otherwise falls back to "All".
    // Expanded/collapsed state of surviving parents is preserved.
    void setCategories(const QVector<PlaylistFilterInfo>& categories, const QString& allLabel);
    qint64 currentCategoryId() const { return m_current; }
    // Selects `id` (0 = All); unknown ids select "All". Emits
    // currentCategoryChanged only when the selection really changes.
    void setCurrentCategory(qint64 id);
    // "Development  >  Programming  >  C++" style path of a category.
    QString pathOf(qint64 id) const;

signals:
    void currentCategoryChanged(qint64 id);

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void rebuild();
    void showPopup();
    void chosen(const QModelIndex& index);
    void updateText();

    QVector<PlaylistFilterInfo> m_categories;
    QString m_allLabel;
    qint64 m_current = 0;
    QSet<qint64> m_collapsed; // ids the user collapsed (everything else is open)
    QFrame* m_popup = nullptr;
    QTreeView* m_tree = nullptr;
    QStandardItemModel* m_model = nullptr;
};
