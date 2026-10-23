#pragma once

#include "CleanupManager.h"
#include "GlWindow.h"
#include "Theme.h"

class GlLabel;
class GlButton;
class GlRule;
class GlPanel;

// The dedicated "Cleanup & Reset" window, opened from SettingsDialog's
// compact "Open Cleanup" entry (one instance, re-focused when opened
// again). Cleanup levels (Cache / Data / Cache + Data), a "More..." dialog
// for the individual operations that genuinely exist, and a Factory Reset
// danger zone. Pure UI - every operation is carried out by CleanupManager;
// this window only confirms, shows progress (inline, on its own page) and
// reports the outcome. Drawn on the GL canvas like the other Motiva windows.
class CleanupWindow : public GlWindow {
    Q_OBJECT
public:
    explicit CleanupWindow(CleanupManager* manager, QWidget* parent = nullptr);

    // Shows the window (centered on Motiva's main window the first time),
    // or raises and focuses it if it is already open.
    void present();
    void layoutContent(const QRectF& content, const QRectF& footer) override;

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void reject() override;
    // While an operation runs, input to every other Motiva window is
    // swallowed (and they cannot be closed), so nothing can touch the
    // library/settings being removed. Not done by disabling them: this
    // window is a child of SettingsDialog and would be disabled with it.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct LevelRow {
        GlLabel* dot = nullptr;
        GlLabel* heading = nullptr;
        GlLabel* desc = nullptr;
        GlLabel* size = nullptr;
        GlButton* button = nullptr;
        GlRule* rule = nullptr;
    };
    LevelRow makeLevelRow(const char* dotColor, const QString& title, const QString& description,
                          const QString& buttonText, bool destructive, CleanupManager::Operation op);
    void showPage(int page);
    void onSizesReady(const CleanupManager::Sizes& sizes);
    void showMoreDialog();
    // Operation-specific confirmation; true = proceed.
    bool confirm(CleanupManager::Operation op);
    void runWithProgress(CleanupManager::Operation op);
    enum class StepMark { Pending, Running, Succeeded, Failed };
    void setStepRow(int index, StepMark mark, const char* color, const QString& detail);
    void onOperationFinished(CleanupManager::Operation op, bool allOk);
    void onProgressButton();
    static QString sizeText(qint64 bytes);

    CleanupManager* m_manager;
    CleanupManager::Sizes m_sizes;
    bool m_positioned = false;
    int m_page = 0; // 0 = cleanup levels, 1 = progress

    QList<LevelRow> m_levels; // Cache, Data, Cache + Data, Media Backups, Cloud Downloads
    GlLabel* m_cacheSize = nullptr;
    GlLabel* m_dataSize = nullptr;
    GlLabel* m_combinedSize = nullptr;
    GlLabel* m_backupSize = nullptr;
    GlLabel* m_cloudSize = nullptr;
    GlRule* m_advancedRule = nullptr;
    GlLabel* m_advancedHeading = nullptr;
    GlLabel* m_advancedDesc = nullptr;
    GlButton* m_moreButton = nullptr;
    GlPanel* m_dangerCard = nullptr;
    GlLabel* m_dangerDot = nullptr;
    GlLabel* m_dangerHeading = nullptr;
    GlLabel* m_dangerDesc = nullptr;
    GlButton* m_factoryButton = nullptr;
    GlLabel* m_safeNote = nullptr;
    GlButton* m_closeButton = nullptr;

    // Progress page
    bool m_running = false;
    // Factory Reset finished: the only way out is restarting Motiva.
    bool m_restartPending = false;
    CleanupManager::Operation m_runningOp = CleanupManager::Operation::DeleteCache;
    GlLabel* m_progressTitle = nullptr;
    QList<GlLabel*> m_stepIcons;   // matching SVG status marks
    QList<GlLabel*> m_stepRows;    // step text labels
    QList<GlLabel*> m_stepDetails; // optional detail lines
    int m_stepCount = 0;
    QStringList m_stepLabels;
    GlLabel* m_progressStatus = nullptr;
    GlButton* m_progressButton = nullptr;
};
