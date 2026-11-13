#pragma once

#include "GlWindow.h"
#include "Theme.h"

#include <QColor>
#include <QList>

class SettingsManager;
class GlSwatchButton;
class AccentChip;

// Accent Color picker: choose exactly ONE app-wide accent (a preset chip, the
// theme default, or a custom color). The choice previews live on the whole
// app but is only saved by Apply; Cancel/Close restores the saved accent.
// The accent applies to all four themes (see SettingsManager::accentColor).
class ThemeEditorDialog : public GlWindow {
    Q_OBJECT
public:
    ThemeEditorDialog(SettingsManager* settings, QWidget* parent = nullptr);
    void layoutContent(const QRectF& content, const QRectF& footer) override;

protected:
    void reject() override;

private:
    void setPending(const QColor& accent); // invalid = theme default
    void pickCustom();
    void onApply();
    void refresh();

    SettingsManager* m_settings;
    QColor m_committed;
    QColor m_pending;
    QList<AccentChip*> m_chips; // [0] = theme default, then presets
    GlLabel* m_heading = nullptr;
    GlLabel* m_nameLabel = nullptr;
    GlLabel* m_previewLabel = nullptr;
    GlLabel* m_infoLabel = nullptr;
    GlSwatchButton* m_previewSwatch = nullptr;
    GlButton* m_previewPrimary = nullptr;
    GlButton* m_previewTab = nullptr;
    GlButton* m_customButton = nullptr;
    GlButton* m_applyButton = nullptr;
    GlButton* m_closeButton = nullptr;
};
