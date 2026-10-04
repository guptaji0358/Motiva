#pragma once

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QStackedLayout>
#include <QStringList>
#include <memory>
#include "WallpaperManager.h"
#include "SettingsManager.h"
#include "RecoveryState.h"
#include "InstanceIpc.h"
#include "DropZoneWidget.h"
#include "IconButton.h"

class QVideoWidget;
class SettingsDialog;
class PlaylistLibrary;
class PlaylistRotation;
class PlaylistDialog;
class CleanupManager;
class ThemeTransitionOverlay;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMimeData;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // initialExplorerFile: a file path handed off from Explorer's "Set as
    // background" verb via the command line (see main.cpp) when THIS
    // process is the one that wins the single-instance lock. Loaded and
    // applied at the end of construction via the same
    // handleExplorerRequestedFile() helper the InstanceIpc::fileReceived
    // path uses - see MainWindow.cpp.
    // initialPlaylistFile: same, for Explorer's "Add to Motiva playlist"
    // verb - added to the image playlist instead of becoming current media.
    explicit MainWindow(bool startMinimized, const QString& initialExplorerFile = QString(),
        const QString& initialPlaylistFile = QString(), QWidget* parent = nullptr);
    ~MainWindow() override;

    // Every extension Explorer's "Set as background" verb should be
    // registered for - the same backend-decodable video containers Open
    // Media/drag & drop already accept, plus GIF and the decodable still-
    // image formats (see
    // isSupportedLocalMediaFile in MainWindow.cpp). Public/static so
    // SettingsManager::setExplorerIntegrationEnabled() can reuse this
    // exact validation logic instead of re-deriving/hardcoding a format
    // list - see WindowsShellIntegration::RegisterSetBackgroundVerb.
    static QStringList explorerIntegrationExtensions();

protected:
    void closeEvent(QCloseEvent* event) override;
    // Re-picks every theme-aware icon (Settings/Set-Wallpaper/Remove-
    // Wallpaper) whenever the active Motiva Theme changes (QApplication::
    // setPalette() inside Theme::applyTheme() triggers this event on every
    // top-level widget) - otherwise an icon chosen for the OLD theme would
    // stay on screen (e.g. invisible dark-on-dark) until something else
    // happened to call updatePrimaryButtonUi()/refresh the Settings icon.
    // See Theme::iconVariant() and the icon-path functions in MainWindow.cpp.
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
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
    // Unloads the video currently loaded in Motiva's own player/preview
    // (the last source loaded via Open Video / Drag & Drop / Paste URL) -
    // a distinct action from onRemoveWallpaper() above, which only detaches
    // Motiva from the desktop wallpaper. Does not delete the file and does
    // not touch m_settings' persisted video path (the closest thing this
    // app has to "history"/recent-item storage) - see MainWindow.cpp.
    void onRemoveVideo();
    void onPrimaryButtonClicked();
    void onPlayPause();
    void onWallpaperError(const QString& message);
    void onWallpaperVerified();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onPreviewFrameReady();
    void onExitRequested();
    void recoverOrActivate();
    void openSettings();
    // Connected to InstanceIpc::fileReceived - a running instance's
    // handling of a file handed off from Explorer's "Set as background"
    // verb while it's already running (see main.cpp/InstanceIpc.h).
    void onExplorerFileReceived(const QString& path);
    // Explorer "Add to Motiva playlist" (command line or InstanceIpc).
    void onAddToPlaylistReceived(const QString& path);
    void openPlaylist();
    // "Set as Wallpaper" from PlaylistDialog: makes `playlistId` the active
    // playlist, makes its current item the current media, and runs the
    // normal Set as Wallpaper path if the wallpaper isn't already active.
    void onApplyPlaylistToDesktop(qint64 playlistId);
    // A video finished (VideoPlayer::endOfMedia). Advances an active video
    // playlist; does nothing for standalone media.
    void onPlayerEndOfMedia();
    // Cleanup & Reset hooks (see CleanupManager): release the wallpaper and
    // current media before data is removed; restart after a Factory Reset.
    void releaseMediaForCleanup();
    void restartAfterFactoryReset();

private:
    void buildUi();
    void buildTray();
    void restoreSettingsToUi();
    void updatePlayPauseLabel();
    // Enables Play/Pause (button + tray) only for media that actually
    // plays - disabled for a still image. Called whenever the current
    // media changes.
    void updatePlayPauseAvailability();
    // Scales the player's current frame into the preview label at its
    // current size. Called per decoded frame and on the label's own
    // Show/Resize (see eventFilter) - a still image has only one frame,
    // so it can't rely on a later frame to correct the preview size.
    void renderPreviewFrame();
    void applyVideoInfoUi();
    void setUiState(WallpaperUiState state);
    void updateStatusUi();
    void updatePrimaryButtonUi();
    // Enabled/disabled state of the standalone "Remove Current Video"
    // button - driven purely by m_hasCurrentVideo, same gating rule as
    // the old Remove Wallpaper button used (Open/Drop/Paste enable it,
    // a startup-restored path never does).
    void updateRemoveVideoButtonUi();

    // Shared by the Open Video button and drag & drop: source is either a
    // local filesystem path or an http(s) video URL - see
    // isUsableVideoSource. Centralizes the "load + persist + update UI"
    // sequence so drag & drop is strictly an additional input method into
    // the existing pipeline, not a parallel one.
    // fromPlaylist: the image playlist is supplying this source. Any other
    // caller is an explicit single-media pick (Open Media, drop, paste,
    // Explorer "Set as background"), which switches the playlist off so
    // automatic rotation can't replace what the user just chose.
    void loadVideoSource(const QString& source, bool fromPlaylist = false);
    // Makes the active playlist's current item the current media (and so
    // the wallpaper, if one is active) - the only place a playlist reaches
    // the wallpaper pipeline. Skips missing files via PlaylistModel::advance.
    void applyPlaylistItem();
    // A playlist is active AND its current item is what's loaded.
    bool isPlaylistDrivingMedia() const;
    // Video playlists play each video once (so endOfMedia fires); anything
    // else uses the normal Loop video behavior. See VideoPlayer::setSequencedPlayback.
    void updatePlayerSequencing();
    void syncPlaylistDialogState();
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
    // Shared by the constructor's initialExplorerFile handling and
    // onExplorerFileReceived() above - one implementation of "apply a
    // file Explorer's Set as background verb supplied," not two. Valid
    // media loads via the existing loadVideoSource()/onSetWallpaper()
    // convergence point; anything else reuses the existing
    // onWallpaperError() warning/tray-message path rather than a new one.
    void handleExplorerRequestedFile(const QString& path);

    std::unique_ptr<WallpaperManager> m_manager;
    SettingsManager m_settings;
    RecoveryState m_recoveryState;
    InstanceIpc m_ipc;
    PlaylistLibrary* m_library = nullptr;
    PlaylistRotation* m_rotation = nullptr;
    PlaylistDialog* m_playlistDialog = nullptr;
    SettingsDialog* m_settingsDialog = nullptr;
    CleanupManager* m_cleanup = nullptr;
    // Mirrors SettingsDialog's own overlay whenever a theme transition it
    // starts also restyles MainWindow (see its themeTransitionStarted/
    // Finished signals) - both top-level windows repaint from the same
    // palette/stylesheet swap, so both need covering.
    ThemeTransitionOverlay* m_themeTransitionOverlay = nullptr;

    QString m_selectedVideoPath;
    WallpaperUiState m_uiState = WallpaperUiState::NoVideo;
    QString m_lastErrorMessage;

    // True only while the user has explicitly loaded a video THIS
    // session via Open Video, drag & drop, or Paste Video URL - i.e.
    // whenever MainWindow::loadVideoSource() (the one convergence point
    // all three of those use) has run and Remove hasn't undone it yet.
    // Deliberately distinct from m_selectedVideoPath, which also holds
    // whatever video was merely restored from settings/history at
    // startup for the in-app preview - that "recent" restore must NOT by
    // itself make Remove Wallpaper clickable. False at construction and
    // reset to false after a successful Remove; see loadVideoSource()/
    // onRemoveWallpaper()/updatePrimaryButtonUi().
    bool m_hasCurrentVideo = false;

    // True while WallpaperManager has temporarily hidden the video
    // because the system is on battery and "Show video on battery" is
    // off - purely a status-text concern here (see updateStatusUi); the
    // actual show/hide decision and lifecycle live entirely in
    // WallpaperManager.
    bool m_batterySuspended = false;
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
    QToolButton* m_playlistButton = nullptr;
    IconButton* m_openVideoButton = nullptr;
    IconButton* m_removeVideoButton = nullptr;
    IconButton* m_pasteLinkButton = nullptr;
    IconButton* m_playPauseButton = nullptr;
    IconButton* m_primaryButton = nullptr;

    QSystemTrayIcon* m_tray = nullptr;
    QAction* m_trayPlayAction = nullptr;
    QAction* m_trayPauseAction = nullptr;
    QAction* m_trayMuteAction = nullptr;
};
