#pragma once

#include "CleanupManager.h"
#include "Theme.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

// The dedicated "Cleanup & Reset" window, opened from SettingsDialog's
// compact "Open Cleanup" entry (one instance, re-focused when opened
// again). Cleanup levels (Cache / Data / Cache + Data), a "More..." dialog
// for the individual operations that genuinely exist, and a Factory Reset
// danger zone. Pure UI - every operation is carried out by CleanupManager;
// this window only confirms, shows progress (inline, on its own page) and
// reports the outcome.
//
// Styled from the active Motiva Theme's own palette (Theme::themePalette)
// and the app-wide button styles - no separate visual system - and
// restyled on every live theme change (PaletteChange).
class CleanupWindow : public QDialog {
    Q_OBJECT
public:
    explicit CleanupWindow(CleanupManager* manager, QWidget* parent = nullptr);

    // Shows the window (centered on Motiva's main window the first time),
    // or raises and focuses it if it is already open.
    void present();

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void reject() override;
    // While an operation runs, input to every other Motiva window is
    // swallowed (and they cannot be closed), so nothing can touch the
    // library/settings being removed. Not done by disabling them: this
    // window is a child of SettingsDialog and would be disabled with it.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* buildLevelsPage();
    QWidget* buildProgressPage();
    QWidget* makeLevelRow(const char* dotColor, const QString& title, const QString& description,
                          QLabel** sizeLabel, const QString& buttonText, CleanupManager::Operation op);
    void applyThemeStyle();
    void onSizesReady(const CleanupManager::Sizes& sizes);
    void showMoreDialog();
    // Operation-specific confirmation; true = proceed.
    bool confirm(CleanupManager::Operation op);
    void runWithProgress(CleanupManager::Operation op);
    void setStepRow(int index, const QString& marker, const char* color, const QString& detail);
    void onOperationFinished(CleanupManager::Operation op, bool allOk);
    void onProgressButton();
    static QString sizeText(qint64 bytes);

    CleanupManager* m_manager;
    CleanupManager::Sizes m_sizes;
    Theme::AppTheme m_styledTheme = Theme::AppTheme::DarkAurora;
    bool m_positioned = false;

    QStackedWidget* m_pages = nullptr;
    QLabel* m_cacheSize = nullptr;
    QLabel* m_dataSize = nullptr;
    QLabel* m_combinedSize = nullptr;
    QPushButton* m_closeButton = nullptr;

    // Progress page
    bool m_running = false;
    // Factory Reset finished: the only way out is restarting Motiva.
    bool m_restartPending = false;
    CleanupManager::Operation m_runningOp = CleanupManager::Operation::DeleteCache;
    QLabel* m_progressTitle = nullptr;
    QVBoxLayout* m_stepsLayout = nullptr;
    QList<QLabel*> m_stepRows;
    QStringList m_stepLabels;
    QLabel* m_progressStatus = nullptr;
    QPushButton* m_progressButton = nullptr;
};
