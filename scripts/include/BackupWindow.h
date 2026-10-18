#pragma once

#include "GlWindow.h"

class BackupManager;
class GlToggle;
class GlLabel;
class GlProgressBar;
class GlButton;

// "Media Backup": a utility window opened from Settings. Shows whether
// backup is on, what it is doing right now (with progress and Cancel), where
// the copies are and how much space they use. Deleting backups is a
// destructive action and lives in Cleanup & Reset (the button here opens
// it), so there is one place for it.
class BackupWindow : public GlWindow {
    Q_OBJECT
public:
    explicit BackupWindow(BackupManager* manager, QWidget* parent = nullptr);
    void present();

    // The one place backup is turned on: explains what will happen (how
    // much will be copied and where), warns about space, and only then
    // enables it. Returns true if backup is now ON.
    static bool confirmAndEnable(BackupManager* manager, QWidget* parent);
    // Turning off asks only if a backup is running (finished copies are kept).
    static bool confirmAndDisable(BackupManager* manager, QWidget* parent);
    static QString explanationText();

    void layoutContent(const QRectF& content, const QRectF& footer) override;

signals:
    void openCleanupRequested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refresh();
    static QString sizeText(qint64 bytes);

    BackupManager* m_manager;
    GlStack m_stack;
    GlLabel* m_intro = nullptr;
    GlToggle* m_enableCheck = nullptr;
    GlLabel* m_status = nullptr;
    GlProgressBar* m_progress = nullptr;
    GlLabel* m_progressDetail = nullptr;
    GlButton* m_cancelButton = nullptr;
    GlButton* m_backUpNow = nullptr;
    GlButton* m_openFolder = nullptr;
    GlButton* m_removeButton = nullptr;
    GlButton* m_closeButton = nullptr;
    GlLabel* m_storageHeading = nullptr;
    GlLabel* m_locationValue = nullptr;
    GlLabel* m_itemsValue = nullptr;
    GlLabel* m_sizeValue = nullptr;
    GlLabel* m_pendingValue = nullptr;
    GlLabel* m_failedValue = nullptr;
    GlLabel* m_unusedValue = nullptr;
    GlLabel* m_problem = nullptr;
};
