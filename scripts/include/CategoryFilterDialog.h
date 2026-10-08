#pragma once

#include "LibraryDatabase.h"

#include <QDialog>

class PlaylistLibrary;
class PlaylistModel;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;
class QVBoxLayout;

// "Build Category" for the open playlist: a name plus filter conditions on
// the metadata Motiva actually stores for each item (file name, path,
// extension, file size) - no content analysis. A live preview shows which
// of THIS playlist's items match. The result is a saved filter
// (playlist_filters); nothing is copied. With an existing filter it edits
// that one ("Edit Category…").
class CategoryFilterDialog : public QDialog {
    Q_OBJECT
public:
    CategoryFilterDialog(PlaylistLibrary* library, PlaylistModel* playlist, const PlaylistFilterInfo* existing,
                         QWidget* parent = nullptr);
    qint64 filterId() const { return m_filterId; }

protected:
    void accept() override;
    void changeEvent(QEvent* event) override;

private:
    static void applyRemoveIcon(class IconButton* button); // SVG for the active theme
    struct RuleRow {
        QWidget* row = nullptr;
        QComboBox* field = nullptr;
        QComboBox* op = nullptr;
        QLineEdit* value = nullptr;
    };
    void addRule(const FilterRule& rule = {});
    void fillOperators(RuleRow& row, const QString& selectOp);
    FilterDefinition definition() const;
    void updatePreview();
    QString validationError() const;

    PlaylistLibrary* m_library;
    PlaylistModel* m_playlist;
    qint64 m_filterId = 0;
    QLineEdit* m_nameEdit = nullptr;
    QComboBox* m_matchCombo = nullptr;
    QVBoxLayout* m_rulesLayout = nullptr;
    QList<RuleRow> m_rules;
    QLabel* m_previewLabel = nullptr;
    QListWidget* m_previewList = nullptr;
    QLabel* m_errorLabel = nullptr;
    QPushButton* m_okButton = nullptr;
    QTimer* m_previewTimer = nullptr;
};
