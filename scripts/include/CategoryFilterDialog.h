#pragma once

#include "GlWindow.h"
#include "LibraryDatabase.h"

class PlaylistLibrary;
class PlaylistModel;
class QStringListModel;
class QTimer;
class GlLabel;
class GlButton;
class GlDropdown;
class GlTextInput;
class GlItemView;

// Builds or edits a rule-based ("virtual") category of one playlist: a name,
// a match mode and one or more conditions over stored media metadata, with a
// live preview of the matching items. Nothing is copied.
class CategoryFilterDialog : public GlWindow {
    Q_OBJECT
public:
    CategoryFilterDialog(PlaylistLibrary* library, PlaylistModel* playlist, const PlaylistFilterInfo* existing,
                         QWidget* parent = nullptr);
    qint64 filterId() const { return m_filterId; }
    void layoutContent(const QRectF& content, const QRectF& footer) override;

protected:
    void accept() override;
    void changeEvent(QEvent* event) override;

private:
    struct RuleRow {
        GlDropdown* field = nullptr;
        GlDropdown* op = nullptr;
        GlTextInput* value = nullptr;
        GlButton* remove = nullptr;
    };
    void applyIcons();
    void addRule(const FilterRule& rule = {});
    void removeRule(GlButton* which);
    void fillOperators(RuleRow& row, const QString& selectOp);
    FilterDefinition definition() const;
    void updatePreview();
    QString validationError() const;

    PlaylistLibrary* m_library;
    PlaylistModel* m_playlist;
    qint64 m_filterId = 0;
    GlLabel* m_intro = nullptr;
    GlLabel* m_nameLabel = nullptr;
    GlTextInput* m_nameEdit = nullptr;
    GlLabel* m_filterHeading = nullptr;
    GlLabel* m_matchLabel = nullptr;
    GlDropdown* m_matchCombo = nullptr;
    QList<RuleRow> m_rules;
    GlButton* m_addRuleButton = nullptr;
    GlLabel* m_previewLabel = nullptr;
    GlItemView* m_previewList = nullptr;
    QStringListModel* m_previewModel = nullptr;
    GlLabel* m_errorLabel = nullptr;
    GlButton* m_okButton = nullptr;
    GlButton* m_cancelButton = nullptr;
    QTimer* m_previewTimer = nullptr;
};
