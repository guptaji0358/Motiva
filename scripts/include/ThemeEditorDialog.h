#pragma once

#include "GlWindow.h"
#include "Theme.h"

#include <QList>

class SettingsManager;
class GlSwatchButton;

// Visual editor for the ACTIVE theme's colors. Changes preview live on the
// whole app but are only saved by Apply; Revert (or closing and choosing
// Discard) restores the last saved state. "Reset to original" clears the
// customization back to the theme's built-in palette (still needs Apply to
// be saved). Customizations are stored per theme, so switching themes
// never loses them (see SettingsManager::themeOverrides).
class ThemeEditorDialog : public GlWindow {
    Q_OBJECT
public:
    ThemeEditorDialog(SettingsManager* settings, QWidget* parent = nullptr);
    void layoutContent(const QRectF& content, const QRectF& footer) override;

protected:
    void reject() override;

private:
    struct Row {
        QString key;
        GlLabel* label = nullptr;
        GlSwatchButton* swatch = nullptr;
        GlButton* reset = nullptr;
    };

    void onApply();
    void onRevert();
    void onResetToOriginal();
    void onCheckContrast();
    void onPresetChosen(int index);
    void pickColor(const QString& key);
    void setPendingColor(const QString& key, const QColor& color);
    void setPending(const Theme::ColorOverrides& pending);
    void previewPending();
    void refreshRows();
    bool isDirty() const;

    SettingsManager* m_settings;
    Theme::AppTheme m_theme;
    Theme::ColorOverrides m_committed;
    Theme::ColorOverrides m_pending;
    QList<Row> m_rows;
    GlLabel* m_heading = nullptr;
    GlLabel* m_presetLabel = nullptr;
    GlDropdown* m_presetCombo = nullptr;
    GlLabel* m_stateLabel = nullptr;
    GlButton* m_contrastButton = nullptr;
    GlButton* m_resetAllButton = nullptr;
    GlButton* m_applyButton = nullptr;
    GlButton* m_revertButton = nullptr;
    GlButton* m_closeButton = nullptr;
    bool m_presetSync = false;
};
