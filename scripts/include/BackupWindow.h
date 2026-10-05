#pragma once

#include <QDialog>

class BackupManager;
class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;

// "Media Backup": a utility window opened from Settings. Shows whether
// backup is on, what it is doing right now (with progress and Cancel), where
// the copies are and how much space they use. Deleting backups is a
// destructive action and lives in Cleanup & Reset (the button here opens
// it), so there is one place for it.
class BackupWindow : public QDialog {
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

signals:
    void openCleanupRequested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refresh();
    static QString sizeText(qint64 bytes);

    BackupManager* m_manager;
    QCheckBox* m_enableCheck = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_progressDetail = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_backUpNow = nullptr;
    QLabel* m_locationValue = nullptr;
    QLabel* m_itemsValue = nullptr;
    QLabel* m_sizeValue = nullptr;
    QLabel* m_pendingValue = nullptr;
    QLabel* m_failedValue = nullptr;
    QLabel* m_unusedValue = nullptr;
    QLabel* m_problem = nullptr;
};
