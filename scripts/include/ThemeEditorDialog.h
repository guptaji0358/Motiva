#pragma once

#include <QDialog>
#include <QList>
#include "Theme.h"

class QPushButton;
class QComboBox;
class QLabel;
class SettingsManager;

// Visual editor for the ACTIVE theme's colors. Changes preview live on the
// whole app but are only saved by Apply; Revert (or closing and choosing
// Discard) restores the last saved state. "Reset to original" clears the
// customization back to the theme's built-in palette (still needs Apply to
// be saved). Customizations are stored per theme, so switching themes
// never loses them (see SettingsManager::themeOverrides).
class ThemeEditorDialog : public QDialog {
    Q_OBJECT
public:
    ThemeEditorDialog(SettingsManager* settings, QWidget* parent = nullptr);

protected:
    void reject() override;

private slots:
    void onApply();
    void onRevert();
    void onResetToOriginal();
    void onCheckContrast();
    void onPresetChosen(int index);

private:
    struct Row {
        QString key;
        QPushButton* swatch = nullptr;
        QPushButton* reset = nullptr;
    };

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
    QComboBox* m_presetCombo = nullptr;
    QPushButton* m_applyButton = nullptr;
    QPushButton* m_revertButton = nullptr;
    QLabel* m_stateLabel = nullptr;
};
