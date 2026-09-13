#pragma once

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QStackedLayout>
#include <memory>
#include "WallpaperManager.h"
#include "SettingsManager.h"
#include "RecoveryState.h"
#include "InstanceIpc.h"
#include "DropZoneWidget.h"
#include "IconButton.h"

class QVideoWidget;
class SettingsDialog;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMimeData;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool startMinimized, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    // Drag & drop entry points - accepts a local video file or a web
    // video URL dropped anywhere on the window, with the video preview
    // area as the visual target (see dragEnterEvent's hint text). Feeds
    // into the same loadVideoSource() the Open Video button uses - no
    // second video-loading path.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

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
    // Third input method's two entry points - both validate then hand
    // off to the same loadVideoSource() the Open Video button and drag &
    // drop already use (see MainWindow.cpp's DropZoneWidget comment).
    void onPasteVideoLink();
    void onPasteShortcut();
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

    // Shared by the Open Video button and drag & drop: source is either a
    // local filesystem path or an http(s) video URL - see
    // isUsableVideoSource. Centralizes the "load + persist + update UI"
    // sequence so drag & drop is strictly an additional input method into
    // the existing pipeline, not a parallel one.
    void loadVideoSource(const QString& source);
    // True if source is either an existing local file or a syntactically
    // valid http(s) URL - the same check used to decide whether a
    // drag-and-dropped or persisted/recovered source is still usable.
    // Deliberately does NOT accept arbitrary text that merely looks like
    // a path - see MainWindow.cpp's dropEvent comment.
    static bool isUsableVideoSource(const QString& source);
    // Inspects dropped MIME data for a single best local-file or web-URL
    // video candidate (first supported one wins - see MainWindow.cpp).
    // If more than one candidate was present, extraCandidateCount is set
    // so the caller can inform the user without guessing which one they
    // "meant".
    static QString extractDroppedVideoSource(const QMimeData* mimeData, int* extraCandidateCount);
    void setDragHintActive(bool active);
    void setDragInvalidActive(bool active);
    // Single source of truth for which preview-stack page is shown and
    // what state the animated drop zone is in - derived from
    // m_dragHintActive/m_dragInvalidActive/m_selectedVideoPath rather than
    // scattering that logic across every call site that changes one of
    // them.
    void refreshDropZoneVisual();
    // Briefly shows the drop zone's Invalid visual (same one drag & drop
    // uses for an unsupported drag) then returns to Idle - used to give
    // pasted-but-unrecognized clipboard content the same subtle "that's
    // not a video" feedback, without a blocking dialog.
    void flashDropZoneInvalid();

    std::unique_ptr<WallpaperManager> m_manager;
    SettingsManager m_settings;
    RecoveryState m_recoveryState;
    InstanceIpc m_ipc;
    SettingsDialog* m_settingsDialog = nullptr;

    QString m_selectedVideoPath;
    WallpaperUiState m_uiState = WallpaperUiState::NoVideo;
    QString m_lastErrorMessage;

    bool m_dragHintActive = false;
    bool m_dragInvalidActive = false;
    QStackedLayout* m_previewStack = nullptr;
    QLabel* m_previewLabel = nullptr;
    DropZoneWidget* m_dropZone = nullptr;
    QLabel* m_fileNameLabel = nullptr;
    QLabel* m_fileDetailsLabel = nullptr;
    QString m_lastVideoDetailsText;
    QLabel* m_statusLabel = nullptr;
    QToolButton* m_settingsButton = nullptr;
    IconButton* m_openVideoButton = nullptr;
    IconButton* m_pasteLinkButton = nullptr;
    IconButton* m_playPauseButton = nullptr;
    IconButton* m_primaryButton = nullptr;

    QSystemTrayIcon* m_tray = nullptr;
    QAction* m_trayPlayAction = nullptr;
    QAction* m_trayPauseAction = nullptr;
    QAction* m_trayMuteAction = nullptr;
};
