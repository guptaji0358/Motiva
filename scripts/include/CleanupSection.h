#pragma once

#include "CleanupManager.h"
#include "Theme.h"

#include <QWidget>

class QLabel;
class QFrame;

// The "Cleanup & Reset" section of SettingsDialog: cleanup levels as
// separate cards (Cache / Data / Cache + Data), a "More..." dialog for the
// individual operations that genuinely exist, and a Factory Reset danger
// zone. Pure UI - every operation is carried out by CleanupManager; this
// widget only confirms, shows progress and reports the outcome.
//
// Styled from the active Motiva Theme's own palette (Theme::themePalette)
// and the app-wide button styles - no separate visual system - and
// restyled on every live theme change (PaletteChange).
class CleanupSection : public QWidget {
    Q_OBJECT
public:
    explicit CleanupSection(CleanupManager* manager, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    QFrame* makeCard(const QString& tier, const QString& title, const QString& description, QLabel** sizeLabel,
                     const QString& buttonText, CleanupManager::Operation op, bool danger = false);
    void applyThemeStyle();
    void onSizesReady(const CleanupManager::Sizes& sizes);
    void showMoreDialog();
    // Operation-specific confirmation; true = proceed.
    bool confirm(CleanupManager::Operation op);
    void runWithProgress(CleanupManager::Operation op);
    static QString sizeText(qint64 bytes);

    CleanupManager* m_manager;
    CleanupManager::Sizes m_sizes;
    Theme::AppTheme m_styledTheme = Theme::AppTheme::DarkAurora;
    QLabel* m_cacheSize = nullptr;
    QLabel* m_dataSize = nullptr;
    QLabel* m_combinedSize = nullptr;
};
