#pragma once

#include "GlWindow.h"

#include <QSet>

class PlaylistLibrary;
class PlaylistModel;
class QStandardItemModel;
class GlLabel;
class GlButton;
class GlTextInput;
class GlItemView;

// Pick items of a playlist for a hand-picked category: ticking rows,
// searching, Select All / Clear. Nothing is copied - the category only points
// at items already in the playlist.
class CategoryItemPickerDialog : public GlWindow {
    Q_OBJECT
public:
    // existingName empty -> creating: the dialog asks for a name.
    CategoryItemPickerDialog(PlaylistLibrary* library, PlaylistModel* playlist, const QString& existingName,
                             const QSet<qint64>& preselected, qint64 existingId, QWidget* parent = nullptr);
    QString categoryName() const { return m_name; }
    QSet<qint64> selectedItemIds() const;
    void layoutContent(const QRectF& content, const QRectF& footer) override;

protected:
    void accept() override;

private:
    void applySearch();
    void updateCount();
    void toggleRow(int row);
    QString nameError() const;

    PlaylistLibrary* m_library;
    PlaylistModel* m_playlist;
    qint64 m_existingId;
    bool m_creating;
    QString m_name;
    QStandardItemModel* m_model = nullptr;
    GlLabel* m_intro = nullptr;
    GlLabel* m_nameLabel = nullptr;
    GlTextInput* m_nameEdit = nullptr;
    GlTextInput* m_search = nullptr;
    GlLabel* m_countLabel = nullptr;
    GlItemView* m_list = nullptr;
    GlButton* m_selectAll = nullptr;
    GlButton* m_clear = nullptr;
    GlLabel* m_errorLabel = nullptr;
    GlButton* m_okButton = nullptr;
    GlButton* m_cancelButton = nullptr;
};
