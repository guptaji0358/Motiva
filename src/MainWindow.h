#pragma once

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QLabel>
#include <QSlider>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <memory>
#include "WallpaperManager.h"
#include "SettingsManager.h"

class QVideoWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool startMinimized, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onChooseVideo();
    void onSetWallpaper();
    void onRemoveWallpaper();
    void onPlayPause();
    void onLoopToggled(bool checked);
    void onVolumeChanged(int value);
    void onMuteToggled(bool checked);
    void onScalingChanged(int index);
    void onMonitorSelectionChanged(int index);
    void onStartWithWindowsToggled(bool checked);
    void onWallpaperError(const QString& message);
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onPreviewFrameReady();
    void onExitRequested();

private:
    void buildUi();
    void buildTray();
    void populateMonitorCombo();
    void restoreSettingsToUi();
    void updatePlayPauseLabel();
    void applyCurrentVideoLabel();

    std::unique_ptr<WallpaperManager> m_manager;
    SettingsManager m_settings;

    QString m_selectedVideoPath;

    QLabel* m_previewLabel = nullptr;
    QLabel* m_selectedFileLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QPushButton* m_chooseButton = nullptr;
    QPushButton* m_setWallpaperButton = nullptr;
    QPushButton* m_removeWallpaperButton = nullptr;
    QPushButton* m_playPauseButton = nullptr;
    QComboBox* m_scalingCombo = nullptr;
    QComboBox* m_monitorCombo = nullptr;
    QSlider* m_volumeSlider = nullptr;
    QCheckBox* m_loopCheck = nullptr;
    QCheckBox* m_muteCheck = nullptr;
    QCheckBox* m_startWithWindowsCheck = nullptr;

    QSystemTrayIcon* m_tray = nullptr;
    QAction* m_trayPlayAction = nullptr;
    QAction* m_trayPauseAction = nullptr;
    QAction* m_trayMuteAction = nullptr;
};
