#pragma once

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <memory>
#include "WallpaperManager.h"
#include "SettingsManager.h"
#include "RecoveryState.h"
#include "InstanceIpc.h"

class QVideoWidget;
class SettingsDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool startMinimized, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    // The UI must distinguish "a video is loaded/previewable" from
    // "the desktop wallpaper is actually active" - see CLAUDE.md's
    // "VideoWallpaper UI Redesign" notes. These states drive the single
    // primary button's label/enabled-state and the status indicator; they
    // are a UI-layer concept only, never fed back into WallpaperManager.
    enum class WallpaperUiState {
        NoVideo,   // nothing loaded yet
        Ready,     // video loaded/previewable, desktop wallpaper untouched
        Applying,  // user clicked Set as Wallpaper; attach requested but not yet verified
        Active,    // attach verified (frames confirmed reaching the desktop)
        Error      // last attach/apply attempt failed
    };

private slots:
    void onChooseVideo();
    void onSetWallpaper();
    void onRemoveWallpaper();
    void onPrimaryButtonClicked();
    void onPlayPause();
    void onWallpaperError(const QString& message);
    void onWallpaperVerified();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onPreviewFrameReady();
    void onExitRequested();
    void recoverOrActivate();
    void openSettings();

private:
    void buildUi();
    void buildTray();
    void restoreSettingsToUi();
    void updatePlayPauseLabel();
    void applyVideoInfoUi();
    void setUiState(WallpaperUiState state);
    void updateStatusUi();
    void updatePrimaryButtonUi();

    std::unique_ptr<WallpaperManager> m_manager;
    SettingsManager m_settings;
    RecoveryState m_recoveryState;
    InstanceIpc m_ipc;
    SettingsDialog* m_settingsDialog = nullptr;

    QString m_selectedVideoPath;
    WallpaperUiState m_uiState = WallpaperUiState::NoVideo;
    QString m_lastErrorMessage;

    QLabel* m_previewLabel = nullptr;
    QLabel* m_fileNameLabel = nullptr;
    QLabel* m_fileDetailsLabel = nullptr;
    QString m_lastVideoDetailsText;
    QLabel* m_statusLabel = nullptr;
    QToolButton* m_settingsButton = nullptr;
    QPushButton* m_openVideoButton = nullptr;
    QPushButton* m_playPauseButton = nullptr;
    QPushButton* m_primaryButton = nullptr;

    QSystemTrayIcon* m_tray = nullptr;
    QAction* m_trayPlayAction = nullptr;
    QAction* m_trayPauseAction = nullptr;
    QAction* m_trayMuteAction = nullptr;
};
