#include "GlPopups.h"
#include "MainWindow.h"
#include "GlUi.h"
#include "SettingsDialog.h"
#include "ThemeTransitionOverlay.h"
#include "StartupDiagnostics.h"
#include "WindowsDesktopWallpaper.h"
#include "WindowsShellIntegration.h"
#include "Theme.h"
#include "PlaylistLibrary.h"
#include "PlaylistModel.h"
#include "PlaylistRotation.h"
#include "PlaylistDialog.h"
#include "NotificationManager.h"
#include "CleanupManager.h"
#include "BackupManager.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QEvent>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDir>
#include <QPixmap>
#include <QFont>
#include <QSizePolicy>
#include <QIcon>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QTimer>
#include <QShortcut>
#include <QClipboard>
#include <QGuiApplication>
#include <QDialog>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QMediaFormat>
#include <QMimeType>
#include <QSet>
#include <QProcess>
#include <QInputDialog>
#include <QtConcurrent/QtConcurrentRun>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

namespace {
// Embedded via resources/app.qrc - loading via the Qt resource path keeps
// this independent of the process's working directory, unlike a relative
// filesystem path. The window/tray identity icon stays a raster .ico
// (Assets/application/motiva.ico) since that's what Windows itself needs
// for the exe/taskbar/Alt-Tab icon; the in-app UI icons are all .svg
// (Assets/<feature>/...), Qt's native SVG icon engine (Qt6::Svg).
constexpr const char* kAppIconResourcePath = ":/application/motiva.ico";
// Settings/Set-Wallpaper/Remove-Wallpaper have genuinely different SVG
// artwork per light/dark icon variant (not a single SVG recolored via a
// filter) - see Assets/settings-icon/{light,dark}/ and
// Assets/wallpaper/{light,dark}/. Aurora and Onyx deliberately share this
// same artwork (see Theme::iconVariant()) - only the QSS-driven chrome
// differs between them. A function rather than a constant so it
// re-resolves against whichever Motiva Theme is active right now.
QString settingsIconPath() {
    return QStringLiteral(":/settings-icon/%1/settings.svg").arg(Theme::iconVariant(Theme::currentTheme()));
}
QString playlistIconPath() {
    return QStringLiteral(":/playlist/%1/playlist.svg").arg(Theme::iconVariant(Theme::currentTheme()));
}
QString wallpaperIconPath(const char* name) {
    return QStringLiteral(":/wallpaper/%1/").arg(Theme::iconVariant(Theme::currentTheme())) + QLatin1String(name);
}
constexpr int kStatusDotSize = 14;
constexpr const char* kOpenVideoIconResourcePath = ":/video/open-video.svg";
constexpr const char* kOpenVideoIconHoverPath = ":/video/open-video-hover.svg";
constexpr const char* kOpenVideoIconPressedPath = ":/video/open-video-pressed.svg";
constexpr const char* kLinkIconResourcePath = ":/video/link.svg";
constexpr const char* kLinkIconHoverPath = ":/video/link-hover.svg";
constexpr const char* kLinkIconPressedPath = ":/video/link-pressed.svg";
// "Remove Current Video" - unloads the loaded video from Motiva's own
// player/preview. Deliberately its own icon (a video frame with an X),
// distinct from wallpaper/{light,dark}/remove-wallpaper.svg's monitor-with-X
// glyph, so the two different actions never look the same at a glance -
// see the "Remove Current Video" task.
constexpr const char* kRemoveVideoIconResourcePath = ":/video/remove-video.svg";
constexpr const char* kRemoveVideoIconHoverPath = ":/video/remove-video-hover.svg";
constexpr const char* kRemoveVideoIconPressedPath = ":/video/remove-video-pressed.svg";
constexpr const char* kRemoveVideoIconDisabledPath = ":/video/remove-video-disabled.svg";
constexpr const char* kPlayIconResourcePath = ":/playback/play.svg";
constexpr const char* kPlayIconHoverPath = ":/playback/play-hover.svg";
constexpr const char* kPauseIconResourcePath = ":/playback/pause.svg";
constexpr const char* kPauseIconHoverPath = ":/playback/pause-hover.svg";
constexpr const char* kVolumeIconResourcePath = ":/playback/volume.svg";
constexpr const char* kVolumeMuteIconResourcePath = ":/playback/volume-mute.svg";
QString setWallpaperIconPath() { return wallpaperIconPath("set-wallpaper.svg"); }
QString setWallpaperIconHoverPath() { return wallpaperIconPath("set-wallpaper-hover.svg"); }
QString setWallpaperIconPressedPath() { return wallpaperIconPath("set-wallpaper-pressed.svg"); }
QString setWallpaperIconDisabledPath() { return wallpaperIconPath("set-wallpaper-disabled.svg"); }
QString removeWallpaperIconPath() { return wallpaperIconPath("remove-wallpaper.svg"); }
QString removeWallpaperIconHoverPath() { return wallpaperIconPath("remove-wallpaper-hover.svg"); }
QString removeWallpaperIconPressedPath() { return wallpaperIconPath("remove-wallpaper-pressed.svg"); }
QString removeWallpaperIconDisabledPath() { return wallpaperIconPath("remove-wallpaper-disabled.svg"); }

// This app has no app-level light/dark theme toggle of its own (see
// CLAUDE.md) - it simply follows the OS window palette everywhere except
// these few intentional accents. Medium-saturation tones were chosen so
// they stay legible on both a light and a dark Windows palette. These are
// now sourced from Theme.h's centralized token set (see the "Motiva UI
// Styling Pass") rather than being their own separate constants.
constexpr const char* kStatusNeutralColor = Theme::kStatusNeutral;
constexpr const char* kStatusWarningColor = Theme::kStatusWarning;
constexpr const char* kStatusSuccessColor = Theme::kStatusSuccess;
constexpr const char* kStatusErrorColor = Theme::kStatusError;

// Fixed dark preview surface, independent of the OS theme - a live video
// preview reads best against a neutral dark backdrop regardless of the
// surrounding app theme, the same convention most media/player apps use.
// Also see Theme::kRadiusMedium, the same corner radius used for every
// other panel/card-like surface in the app.
const QString kPreviewSurfaceStyle = QStringLiteral(
    "background-color: %1; border: 1px solid %2; border-radius: %3px; color: %4;")
    .arg(Theme::kPreviewSurfaceBg, Theme::kPreviewSurfaceBorder)
    .arg(Theme::kRadiusMedium)
    .arg(Theme::kPreviewText);

const QSet<QString>& supportedVideoContainerExtensions() {
    return VideoPlayer::supportedVideoExtensions();
}

// A GIF is never routed through QMediaFormat/QMediaPlayer (Qt Multimedia
// doesn't treat it as a video container) - it's handled as an animated
// image via QMovie instead (see VideoPlayer::loadFile). Kept as its own
// explicit, named predicate rather than folded into the video-extension
// set, since the two paths behave differently wherever that distinction
// matters (e.g. web video URLs - see isWebVideoUrl - deliberately do NOT
// accept .gif; GIF support here is local-file-only, no new networking/
// download code was added for it).
bool isGifExtension(const QString& fileNameOrPath) {
    return QFileInfo(fileNameOrPath).suffix().compare(QLatin1String("gif"), Qt::CaseInsensitive) == 0;
}

bool hasSupportedVideoExtension(const QString& fileNameOrPath) {
    return supportedVideoContainerExtensions().contains(QFileInfo(fileNameOrPath).suffix().toLower());
}

// Local files only: any backend-decodable video container, a GIF, or a
// still image the deployed Qt image plugins can decode (see
// VideoPlayer::supportedStaticImageExtensions - derived from
// QImageReader, not hardcoded). Used wherever a LOCAL file's usability is
// being decided (drag & drop, Open Media, Explorer verb, startup restore)
// - see isWebVideoUrl for the (deliberately narrower, video-only) URL case.
bool isSupportedLocalMediaFile(const QString& path) {
    return hasSupportedVideoExtension(path) || isGifExtension(path) ||
        VideoPlayer::hasStaticImageExtension(path);
}

// Split out from isWebVideoUrl() so the Paste Video URL dialog can tell
// "that's not even a URL" apart from "that's a real webpage, just not a
// directly playable video file" and show a distinct message for each -
// see MainWindow::onPasteVideoLink().
bool isHttpUrl(const QUrl& url) {
    if (!url.isValid() || url.isLocalFile()) {
        return false;
    }
    const QString scheme = url.scheme();
    return scheme.compare(QLatin1String("http"), Qt::CaseInsensitive) == 0 ||
        scheme.compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
}

bool isWebVideoUrl(const QUrl& url) {
    if (!isHttpUrl(url)) {
        return false;
    }
    // The URL path (excludes query string) needs to look like a video
    // file - accepting any http(s) URL unconditionally would mean
    // "blindly assume every webpage link is a playable video" (e.g. a
    // YouTube watch page), which this app's media backend (QMediaPlayer/
    // FFmpeg) cannot actually play as-is, and which this project
    // explicitly does not attempt to work around (no scraping/yt-dlp/
    // downloading - see the "Paste Video URL" task).
    return hasSupportedVideoExtension(url.path());
}
} // namespace

QStringList MainWindow::explorerIntegrationExtensions() {
    QStringList exts = supportedVideoContainerExtensions().values();
    exts.append(QStringLiteral("gif"));
    exts.append(VideoPlayer::supportedStaticImageExtensions().values());
    return exts;
}

// Fills whatever space the main layout gives the preview and places the panel
// inside it: the largest rectangle of the media's aspect ratio, centred. With
// no aspect (nothing loaded, drop zone shown) the panel simply fills the host.
// Geometry is recomputed from the host's current size on every resize and
// aspect change - nothing is cached.
class PreviewAspectHost : public QWidget {
public:
    using QWidget::QWidget;
    void setPanel(QWidget* panel) {
        m_panel = panel;
        place();
    }
    void setAspect(double aspect) {
        if (qFuzzyCompare(aspect + 1.0, m_aspect + 1.0)) {
            return;
        }
        m_aspect = aspect;
        place();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        place();
    }

private:
    void place() {
        if (!m_panel) {
            return;
        }
        QRect r = rect();
        if (m_aspect > 0.0 && r.width() > 0 && r.height() > 0) {
            int w = r.width();
            int h = qRound(w / m_aspect);
            if (h > r.height()) {
                h = r.height();
                w = qRound(h * m_aspect);
            }
            r = QRect((width() - w) / 2, (height() - h) / 2, w, h);
        }
        m_panel->setGeometry(r);
    }

    QWidget* m_panel = nullptr;
    double m_aspect = 0.0; // width / height of the shown media; 0 = fill
};

MainWindow::MainWindow(bool startMinimized, const QString& initialExplorerFile,
    qint64 initialPlaylistId, const QString& initialPlaylistFile, QWidget* parent)
    : QMainWindow(parent), m_manager(std::make_unique<WallpaperManager>()) {
    qInfo() << "[Lifecycle] MainWindow construction begin, startMinimized=" << startMinimized;
    m_startMinimized = startMinimized;
    m_initialExplorerFile = initialExplorerFile;
    m_initialPlaylistId = initialPlaylistId;
    m_initialPlaylistFile = initialPlaylistFile;
    // Listen for a second launch's hand-off before any slow work below, so a
    // double-click during Windows startup reaches this instance instead of
    // finding nobody to talk to. The requests are only dispatched once the
    // event loop runs, and runAfterStartup() holds them until the deferred
    // restore has finished.
    m_ipc.startListening();
    setWindowTitle("Motiva");
    setWindowIcon(QIcon(kAppIconResourcePath));
    resize(860, 680);
    setMinimumSize(720, 480);
    // Whole-window drop target (see dragEnterEvent/dropEvent) - the
    // preview area is the visual focus of the drag hint, but the actual
    // Qt drop target is the window so a drop anywhere on it still works.
    setAcceptDrops(true);

    // Playlist library (.mtv, SQLite) + image-rotation triggers. Built
    // before the UI so PlaylistDialog (created in buildUi) can bind to them.
    m_library = new PlaylistLibrary(&m_settings, this);
    m_library->open();
    m_rotation = new PlaylistRotation(m_library, &m_settings, this);
    // Cleanup & Reset (Settings). Built before the UI so SettingsDialog can
    // bind to it; it reaches the wallpaper only through these two hooks.
    // Optional local media backup - OFF unless the user turned it on.
    m_backup = new BackupManager(m_library, &m_settings, this);
    m_cleanup = new CleanupManager(&m_settings, &m_recoveryState, m_library, m_backup, this);
    connect(m_cleanup, &CleanupManager::releaseMediaRequested, this, &MainWindow::releaseMediaForCleanup);
    connect(m_cleanup, &CleanupManager::restartRequested, this, &MainWindow::restartAfterFactoryReset);
    m_explorerMenuSyncTimer = new QTimer(this);
    m_explorerMenuSyncTimer->setSingleShot(true);
    m_explorerMenuSyncTimer->setInterval(250);
    connect(m_explorerMenuSyncTimer, &QTimer::timeout, this, [this] { syncExplorerPlaylistMenu(); });

    buildUi();
    buildTray();

    // Unified notifications: toasts inside this window, Windows
    // notifications through the tray icon. Preferences are read live from
    // m_settings. (The Playlists window attaches its own host.)
    {
        NotificationManager* notifier = NotificationManager::instance();
        notifier->setSettings(&m_settings);
        notifier->setTrayIcon(m_tray);
        notifier->attachHost(this, /*topInset=*/56); // clear of the header buttons
    }

    // Third video input method, alongside drag & drop and the Open Video
    // button: Ctrl+V anywhere in this window loads a recognized video URL
    // (or local file path) straight from the clipboard - see
    // onPasteShortcut(). QShortcut's default WindowShortcut context means
    // this only ever fires while MainWindow itself is the active window;
    // SettingsDialog is a separate top-level QDialog, so a text field
    // there stays focused and gets a completely normal Ctrl+V instead -
    // no manual focus/widget-type checking needed to keep the two apart.
    auto* pasteShortcut = new QShortcut(QKeySequence::Paste, this);
    connect(pasteShortcut, &QShortcut::activated, this, &MainWindow::onPasteShortcut);
    // New Image Playlist (Ctrl+Alt+N) / New Video Playlist (Ctrl+Shift+N),
    // same as in the Playlists window. Ctrl+N is intentionally unbound.
    for (const bool video : {false, true}) {
        auto* shortcut = new QShortcut(
            QKeySequence(video ? (Qt::CTRL | Qt::SHIFT | Qt::Key_N) : (Qt::CTRL | Qt::ALT | Qt::Key_N)), this);
        connect(shortcut, &QShortcut::activated, this, [this, video] {
            if (!PlaylistDialog::isTextInputFocused()) {
                openPlaylist();
                m_playlistDialog->createPlaylist(video);
            }
        });
    }

    m_manager->setRecoveryState(&m_recoveryState);
    // The single-instance winner (only instance that ever reaches this
    // constructor - see main.cpp) starts listening for recovery requests
    // from any later launch attempt.
    connect(&m_ipc, &InstanceIpc::recoverRequested, this, [this] {
        // Bring the window forward immediately; the (potentially wallpaper-
        // attaching) recovery itself waits for startup to finish.
        showNormal();
        raise();
        activateWindow();
        runAfterStartup([this] { recoverOrActivate(); });
    });
    // A second launch attempt invoked via Explorer's "Set as background"
    // verb while this instance is already running - see main.cpp/
    // InstanceIpc::sendSetBackgroundRequest.
    connect(&m_ipc, &InstanceIpc::fileReceived, this,
            [this](const QString& path) { runAfterStartup([this, path] { onExplorerFileReceived(path); }); });
    connect(&m_ipc, &InstanceIpc::addToPlaylistReceived, this, [this](qint64 id, const QString& path) {
        runAfterStartup([this, id, path] { onAddToPlaylistReceived(id, path); });
    });

    // Whenever the playlist's current image changes (any rotation trigger,
    // Show now, removal of the current image) or the playlist is switched
    // on, that image becomes the current media - through the same
    // loadVideoSource() every other input uses, so an active wallpaper
    // switches in place (no window/HWND rebuild; the renderer crossfades).
    connect(m_library, &PlaylistLibrary::activeCurrentChanged, this, [this] {
        applyPlaylistItem();
        syncPlaylistDialogState();
        // Feedback only - never touches playback. Rate limited so a
        // one-minute rotation can't turn into a stream of toasts.
        const PlaylistModel* active = m_library->activePlaylist();
        if (m_manager->isActive() && active && !active->currentPath().isEmpty() &&
            QFileInfo(active->currentPath()).isFile()) {
            Notification n;
            n.kind = Notification::Kind::Info;
            n.category = Notification::Category::Playback;
            n.title = tr("Wallpaper Changed");
            n.message = tr("Now showing \"%1\" from \"%2\".")
                            .arg(QFileInfo(active->currentPath()).fileName(), active->name());
            n.key = QStringLiteral("playback-changed");
            n.rateLimit = true;
            n.cooldownMs = 20000;
            NotificationManager::instance()->notify(n);
        }
    });
    connect(m_library, &PlaylistLibrary::sourceNotice, this, [this](const QString& message) {
        if (m_playlistDialog && m_playlistDialog->isVisible()) {
            return; // PlaylistDialog shows it in its own notice line
        }
        Notification n;
        n.kind = Notification::Kind::Info;
        n.title = tr("Motiva playlists");
        n.message = message;
        n.key = QStringLiteral("playlist-source-notice");
        n.desktop = true;
        NotificationManager::instance()->notify(n);
    });
    connect(m_library, &PlaylistLibrary::activeScopeChanged, this, [this] {
        applyVideoInfoUi(); // "item x of y" follows the category's membership
        updateStatusUi();
        syncPlaylistDialogState();
    });
    // Switching the active playlist safely switches the wallpaper source:
    // the new playlist's current item replaces the current media in place.
    // Only one playlist is ever active (PlaylistLibrary), so two playlists
    // can never fight over the wallpaper.
    connect(m_library, &PlaylistLibrary::activeChanged, this, [this](qint64 id) {
        if (id > 0) {
            applyPlaylistItem();
        }
        updatePlayerSequencing();
        applyVideoInfoUi();
        updateStatusUi();
        syncPlaylistDialogState();
    });
    connect(m_library, &PlaylistLibrary::playlistsChanged, this, [this] {
        applyVideoInfoUi(); // playlist renamed / count changed
        updateStatusUi();
        m_explorerMenuSyncTimer->start(); // created / renamed / deleted / imported / reset
    });
    connect(m_manager->player(), &VideoPlayer::endOfMedia, this, &MainWindow::onPlayerEndOfMedia);
    if (!m_library->openNotice().isEmpty()) {
        QTimer::singleShot(0, this, [this] {
            if (isVisible()) {
                GlMessageBox::warning(this, tr("Motiva playlists"), m_library->openNotice());
            } else if (m_tray) {
                m_tray->showMessage(tr("Motiva playlists"), m_library->openNotice(), QSystemTrayIcon::Warning, 8000);
            }
        });
    }

    connect(m_manager.get(), &WallpaperManager::errorOccurred, this, &MainWindow::onWallpaperError);
    // wallpaperActivated fires as soon as attach is REQUESTED, not once
    // it's verified - see WallpaperManager.h. The UI must not call this
    // "Active" yet, only "Applying".
    connect(m_manager.get(), &WallpaperManager::wallpaperActivated, this, [this] {
        setUiState(WallpaperUiState::Applying);
    });
    // The one signal that corresponds to a genuinely confirmed wallpaper
    // (post-attach presentedFrameCount verification - see WallpaperManager).
    connect(m_manager.get(), &WallpaperManager::wallpaperVerified, this, &MainWindow::onWallpaperVerified);
    connect(m_manager.get(), &WallpaperManager::wallpaperRemoved, this, [this] {
        setUiState(m_selectedVideoPath.isEmpty() ? WallpaperUiState::NoVideo : WallpaperUiState::Ready);
        // Single source of truth for clearing this persisted flag,
        // covering every path that can end our wallpaper - the explicit
        // Remove Wallpaper button, Exit, AND WallpaperManager stepping
        // aside on its own after detecting the user changed Windows' own
        // wallpaper externally (see WallpaperManager::
        // onPossibleExternalWallpaperChange). That last path previously
        // bypassed this entirely (only onRemoveWallpaper()/
        // onExitRequested() cleared it), leaving wasWallpaperActive
        // stuck true after an external wallpaper change even though our
        // video wallpaper was genuinely no longer active - stale
        // persisted state, exactly what section 12 of the "Icon States +
        // Wallpaper Ownership Fix" task warned against reintroducing.
        // Harmless to also still clear it at the other two call sites -
        // this setter is idempotent.
        m_settings.setWasWallpaperActive(false);
    });
    connect(m_manager.get(), &WallpaperManager::wallpaperSuspendedForBattery, this, [this] {
        m_batterySuspended = true;
        // Reaches the Active UI state even when the wallpaper was NEVER
        // actually attached (e.g. Set as Wallpaper clicked while already
        // on battery with the setting off - see WallpaperManager::
        // setWallpaper's shouldStartHidden path) - otherwise
        // wallpaperVerified() would never fire (no attach was ever
        // attempted, so its post-attach verification checkpoint is never
        // reached) and the UI would stay stuck on "Applying wallpaper…"
        // forever. m_batterySuspended is already true by the time this
        // calls updateStatusUi() (via setUiState), so the Active branch's
        // battery-text override applies immediately.
        setUiState(WallpaperUiState::Active);
    });
    connect(m_manager.get(), &WallpaperManager::wallpaperResumedFromBattery, this, [this] {
        m_batterySuspended = false;
        updateStatusUi();
    });
    connect(m_manager->player(), &VideoPlayer::frameReady, this, &MainWindow::onPreviewFrameReady);

    restoreSettingsToUi();

    // Force the native HWND to actually exist even when starting
    // minimized-to-tray and never shown: Qt often defers creating a
    // widget's native window until it's shown, and WallpaperManager
    // relies on receiving the broadcast "TaskbarCreated" message (sent to
    // every real top-level HWND in the process) to detect Explorer
    // restarts (see WallpaperManager::nativeEventFilter) - confirmed by
    // testing that this message is never received at all when the only
    // top-level widget in the process was hidden without ever having been
    // shown, since no native window existed yet to receive it.
    (void)winId();
    // Registers for the AC/battery power-source notification on this
    // now-guaranteed-to-exist HWND - see WallpaperManager::
    // registerPowerNotifications and the "Play Images/Videos on Battery" settings.
    // Placed after the winId() force-create above for the same reason
    // that call exists: the native HWND must be real before anything can
    // register against it.
    m_manager->registerPowerNotifications(reinterpret_cast<HWND>(winId()));
    // An image the battery policy is hiding must not advance (the playlist
    // would otherwise cycle through images nobody can see).
    m_rotation->setSuspendedPredicate([this] { return m_manager->isBatterySuspended(); });
    // Lock -> Unlock playlist trigger, on the same stable HWND.
    m_rotation->registerSessionNotifications(reinterpret_cast<HWND>(winId()));

    if (startMinimized) {
        hide();
    }
    StartupDiagnostics::instance().mark("uiReady");
    qInfo() << "[Lifecycle] MainWindow UI construction complete (media restore deferred until shown).";
}

void MainWindow::beginDeferredStartup() {
    // Force the first paint of the finished, themed window now so it is on
    // screen before the heavier restore work below runs on the GUI thread.
    if (isVisible()) {
        repaint();
    }
    QTimer::singleShot(0, this, &MainWindow::completeStartup);
}

void MainWindow::runAfterStartup(std::function<void()> fn) {
    if (m_startupComplete) {
        fn();
    } else {
        m_postStartupTasks.push_back(std::move(fn));
    }
}

void MainWindow::completeStartup() {
    if (m_startupComplete) {
        return;
    }
    qInfo() << "[Lifecycle] Deferred startup begin.";
    // Load (but don't attach to the desktop) whatever video was previously
    // selected, purely so the in-app preview has something to show.
    if (isUsableVideoSource(m_selectedVideoPath)) {
        m_manager->player()->loadFile(m_selectedVideoPath);
        m_manager->player()->play();
        setUiState(WallpaperUiState::Ready);
    } else {
        setUiState(WallpaperUiState::NoVideo);
    }
    updatePlayPauseAvailability();

    // Image playlist startup. A new Windows logon session since Motiva last
    // ran (restart, power-on, sign-in) is the "After Windows restarts"
    // trigger; Explorer restarts and Motiva relaunches within the same
    // session never count (see PlaylistRotation::consumeNewWindowsSession).
    // Per the user's decision, only in that case - playlist on, trigger
    // enabled, and the wallpaper was active when the previous session
    // ended - is the wallpaper re-applied automatically (further below).
    // Every other launch keeps the "never auto-attach" rule.
    const bool newWindowsSession = m_rotation->consumeNewWindowsSession();
    bool autoApplyPlaylistWallpaper = false;
    if (PlaylistModel* active = m_library->activePlaylist()) {
        // Only image playlists have this trigger; fire() checks the active
        // playlist's own setting. Video playlists never advance on startup.
        if (newWindowsSession && m_rotation->fire(PlaylistRotation::Trigger::WindowsStart)) {
            autoApplyPlaylistWallpaper = m_settings.wasWallpaperActive();
        } else if (newWindowsSession && !active->currentIsVideo() && active->rotation().onWindowsStart) {
            autoApplyPlaylistWallpaper = m_settings.wasWallpaperActive(); // single-image playlist
        }
        applyPlaylistItem(); // the active playlist decides the current media
    }

    // A fresh process launch - whether a normal double-click or a
    // Start-with-Windows autostart - NEVER auto-attaches the wallpaper on
    // its own, even if wasWallpaperActive() is true from a previous
    // session (that flag is still tracked/persisted for diagnostics and
    // for onExitRequested()'s own bookkeeping, just no longer read here).
    // "The EXE being alive must NOT automatically mean our wallpaper is
    // being enforced - only the explicit Set as Wallpaper action should
    // activate it" - see CLAUDE.md's "Set as wallpaper interference" fix.
    // This intentionally reverses this app's own earlier "restore
    // previously-active wallpaper on startup" behavior, which is exactly
    // what silently kept re-covering the user's normal Windows wallpaper
    // on every relaunch/autostart with no explicit action that session.
    // Mid-session recovery - the SAME running process reattaching after
    // an Explorer restart (WallpaperManager::onExplorerRestarted) or a
    // second launch attempt asking this instance to recover
    // (MainWindow::recoverOrActivate, via InstanceIpc) - is unaffected:
    // both only ever act while m_manager->isActive() is already true,
    // i.e. only continuing a wallpaper THIS process already had explicitly
    // activated, never reviving one from a past process's persisted state.
    //
    // Since nothing is attached this launch, nudge Explorer to fully
    // reclaim/redraw the desktop background layer - if a previous run
    // ended uncleanly (crash/taskkill before WindowsDesktopWallpaper::
    // RefreshDesktopBackground existed, or before this auto-restore
    // removal) its own detach path may never have run this. Harmless/
    // idempotent if nothing was ever wrong: it just re-applies whatever
    // wallpaper Explorer already has configured.
    // Off the GUI thread: it does an out-of-process COM call and a
    // broadcast SPI_SETDESKWALLPAPER, either of which can stall for seconds
    // while Explorer is still initializing right after sign-in.
    (void)QtConcurrent::run([] { WindowsDesktopWallpaper::RefreshDesktopBackground(); });

    // Self-heal, gated purely on the user's own opt-in settings (never
    // registered "just because the app started" - see CLAUDE.md's
    // architecture-protection notes): if either Windows-integration
    // feature was already turned on in a previous session, silently
    // re-apply it now. Both calls are idempotent (fixed target path /
    // overwritten registry values), so this only matters after a rebuild
    // or redeploy moved the real executable - it keeps the shortcut/verb
    // pointing at the correct current path without the user having to
    // toggle the setting off and on again.
    if (m_settings.showInWindowsSearch()) {
        WindowsShellIntegration::CreateStartMenuShortcut(SettingsManager::motivaExecutablePath());
    }
    if (m_settings.explorerIntegrationEnabled()) {
        // Same registration the Settings toggle performs (also replaces the
        // older flat verbs), then the playlist entries from this library.
        m_settings.setExplorerIntegrationEnabled(true);
        syncExplorerPlaylistMenu(true);
    }

    // A file handed off from Explorer's "Set as background" verb, when
    // THIS process is the one that won the single-instance lock (see
    // main.cpp) - applied last, after the rest of construction/restore
    // above, so it correctly supersedes whatever was merely restored from
    // a previous session.
    if (!m_initialExplorerFile.isEmpty()) {
        handleExplorerRequestedFile(m_initialExplorerFile);
    }
    if (!m_initialPlaylistFile.isEmpty()) {
        // Runs after the window is shown, so a "Create New Playlist" name
        // dialog has a visible parent window.
        QTimer::singleShot(0, this, [this] {
            onAddToPlaylistReceived(m_initialPlaylistId, m_initialPlaylistFile);
        });
    }
    // An explicit Explorer "Set as background" above switches the playlist
    // off, so this only ever re-applies the playlist itself.
    if (autoApplyPlaylistWallpaper && m_library->activeId() > 0 && isPlaylistDrivingMedia()) {
        qInfo() << "[Playlist] New Windows session, playlist was the active wallpaper - re-applying it.";
        m_hasCurrentVideo = true;
        updateRemoveVideoButtonUi();
        onSetWallpaper();
    }

    m_backup->start(); // only does anything if the user turned Backup on
    StartupDiagnostics::instance().mark("mainWindowReady");
    m_startupComplete = true;
    qInfo() << "[Lifecycle] MainWindow construction complete.";
    auto tasks = std::move(m_postStartupTasks);
    m_postStartupTasks.clear();
    for (auto& task : tasks) {
        task();
    }
}

MainWindow::~MainWindow() {
    NotificationManager::instance()->shutdown(); // no host/tray/settings pointers outlive this window
}

void MainWindow::buildUi() {
    // Borderless: Motiva draws its own frame (GL canvas title strip with
    // custom window controls). Dragging/resizing come from nativeEvent().
    setWindowFlag(Qt::FramelessWindowHint, true);
    resize(860, 680); // changing flags resets the size set in the constructor

    m_canvas = new GlCanvas(this);
    setCentralWidget(m_canvas);
    m_canvas->setAccessibleName(tr("Motiva"));
    m_canvas->setLayoutCallback([this](const QSizeF& s) { layoutGl(s); });

    // Items paint in creation order: strip, preview, dock, then controls.
    m_titlePanel = new GlPanel(m_canvas);
    m_titlePanel->setBlock(GlPanel::Block::Accent);
    m_titlePanel->setRadiusPx(16);
    m_logo = new GlLabel(m_canvas);
    m_logo->setPixmap(QIcon(kAppIconResourcePath).pixmap(QSize(24, 24), devicePixelRatioF()));
    m_titleLabel = new GlLabel(m_canvas);
    m_titleLabel->setText(tr("Motiva"));
    m_titleLabel->setPointSize(14);
    m_titleLabel->setBold(true);
    m_taglineLabel = new GlLabel(m_canvas);
    m_taglineLabel->setText(tr("Live wallpapers"));
    m_taglineLabel->setPointSize(7.5);
    m_taglineLabel->setBold(true);
    m_taglineLabel->setCaps(true);
    m_taglineLabel->setTone(GlLabel::Tone::Dim);

    m_playlistButton = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_playlistButton->setIcon(QIcon(playlistIconPath()));
    m_playlistButton->setText(tr("Playlists"));
    m_playlistButton->setToolTip(tr("Image and video playlists that change your wallpaper automatically"));
    connect(m_playlistButton, &GlButton::clicked, this, &MainWindow::openPlaylist);

    m_settingsButton = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_settingsButton->setIcon(QIcon(settingsIconPath()));
    m_settingsButton->setText(tr("Settings"));
    m_settingsButton->setToolTip(tr("Open settings"));
    connect(m_settingsButton, &GlButton::clicked, this, &MainWindow::openSettings);

    m_minButton = new GlButton(m_canvas, GlButton::Kind::Minimize);
    m_minButton->setToolTip(tr("Minimize"));
    connect(m_minButton, &GlButton::clicked, this, &QWidget::showMinimized);
    m_maxButton = new GlButton(m_canvas, GlButton::Kind::Maximize);
    m_maxButton->setToolTip(tr("Maximize"));
    connect(m_maxButton, &GlButton::clicked, this, [this] {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
    });
    m_closeButton = new GlButton(m_canvas, GlButton::Kind::Close);
    m_closeButton->setToolTip(tr("Close to tray (playback keeps running)"));
    connect(m_closeButton, &GlButton::clicked, this, &QWidget::close);

    // --- Media preview / animated drop zone ---
    m_preview = new GlPreview(m_canvas);
    // Three clearly separate input methods: drag & drop (local files), this
    // button for a direct web video URL, and Open Media in the dock.
    m_pasteLinkButton = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_pasteLinkButton->setIcon(QIcon(kLinkIconResourcePath));
    m_pasteLinkButton->setText(tr("Paste Video URL"));
    m_pasteLinkButton->setToolTip(tr("Load a video from a direct URL (or press Ctrl+V anywhere in this window)"));
    connect(m_pasteLinkButton, &GlButton::clicked, this, &MainWindow::onPasteVideoLink);

    // --- Dock: media info, secondary actions, status, the one primary action ---
    m_dockPanel = new GlPanel(m_canvas);
    m_dockPanel->setBlock(GlPanel::Block::Accent2);
    m_fileNameLabel = new GlLabel(m_canvas);
    m_fileNameLabel->setText(tr("No file selected"));
    m_fileNameLabel->setPointSize(11);
    m_fileNameLabel->setBold(true);
    m_fileDetailsLabel = new GlLabel(m_canvas);
    m_fileDetailsLabel->setPointSize(9);
    m_fileDetailsLabel->setTone(GlLabel::Tone::Dim);
    m_fileDetailsLabel->setVisible(false);

    m_removeVideoButton = new GlButton(m_canvas, GlButton::Kind::Icon);
    m_removeVideoButton->setIcon(QIcon(kRemoveVideoIconResourcePath));
    m_removeVideoButton->setToolTip(tr("Unload the current media from Motiva's player/preview"));
    m_removeVideoButton->setEnabled(m_hasCurrentVideo);
    connect(m_removeVideoButton, &GlButton::clicked, this, &MainWindow::onRemoveVideo);

    m_openVideoButton = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_openVideoButton->setIcon(QIcon(kOpenVideoIconResourcePath));
    m_openVideoButton->setText(tr("Open Media"));
    m_openVideoButton->setToolTip(tr("Open a video, GIF or image file"));
    connect(m_openVideoButton, &GlButton::clicked, this, &MainWindow::onChooseVideo);

    m_playPauseButton = new GlButton(m_canvas, GlButton::Kind::Icon);
    m_playPauseButton->setIcon(QIcon(kPlayIconResourcePath));
    m_playPauseButton->setToolTip(tr("Play or pause the preview"));
    connect(m_playPauseButton, &GlButton::clicked, this, &MainWindow::onPlayPause);

    // The status dot is a real SVG mark (Assets/status/status-dot.svg),
    // recolored per state in updateStatusUi().
    m_statusDot = new GlLabel(m_canvas);
    m_statusLabel = new GlLabel(m_canvas);
    m_statusLabel->setPointSize(10);
    m_statusLabel->setBold(true);

    m_primaryButton = new GlButton(m_canvas, GlButton::Kind::Primary);
    m_primaryButton->setText(tr("Set as Wallpaper"));
    connect(m_primaryButton, &GlButton::clicked, this, &MainWindow::onPrimaryButtonClicked);

    m_playlistDialog = new PlaylistDialog(m_library, this);
    m_playlistDialog->setBackupManager(m_backup);
    m_playlistDialog->setSettings(&m_settings);
    connect(m_playlistDialog, &PlaylistDialog::applyToDesktopRequested, this, &MainWindow::onApplyPlaylistToDesktop);

    m_settingsDialog = new SettingsDialog(m_manager.get(), &m_settings, m_cleanup, m_backup, this);
    connect(m_settingsDialog, &SettingsDialog::mutedChanged, this, [this](bool muted) {
        if (m_trayMuteAction) {
            m_trayMuteAction->blockSignals(true);
            m_trayMuteAction->setChecked(muted);
            m_trayMuteAction->setIcon(QIcon(muted ? kVolumeMuteIconResourcePath : kVolumeIconResourcePath));
            m_trayMuteAction->blockSignals(false);
        }
    });

    m_themeTransitionOverlay = new ThemeTransitionOverlay(this);
    connect(m_settingsDialog, &SettingsDialog::explorerIntegrationChanged, this,
            [this](bool) { syncExplorerPlaylistMenu(true); });
    connect(m_settingsDialog, &SettingsDialog::themeTransitionStarted, this, [this] {
        m_themeTransitionOverlay->beginTransition();
    });
    connect(m_settingsDialog, &SettingsDialog::themeTransitionFinished, this, [this] {
        m_themeTransitionOverlay->finishTransition();
    });
    updateWindowButtons();
}

void MainWindow::layoutGl(const QSizeF& size) {
    if (!m_canvas || !m_titlePanel || !m_primaryButton) {
        return;
    }
    const qreal W = size.width(), H = size.height();
    const qreal m = 12;       // outer margin
    const qreal titleH = 46;
    const qreal dockH = 172;

    // Title strip (identity bar).
    const QRectF strip(m, m, W - 2 * m, titleH);
    m_titlePanel->setRect(strip);
    m_logo->setRect(QRectF(strip.left() + 14, strip.top() + 11, 24, 24));
    m_titleLabel->setRect(QRectF(strip.left() + 48, strip.top() + 5, 90, 24));
    m_taglineLabel->setRect(QRectF(strip.left() + 48, strip.top() + 27, 130, 14));
    const qreal bw = 36, bh = 30, by = strip.top() + (titleH - bh) / 2;
    qreal x = strip.right() - 10 - bw;
    m_closeButton->setRect(QRectF(x, by, bw, bh));
    x -= bw + 2;
    m_maxButton->setRect(QRectF(x, by, bw, bh));
    x -= bw + 2;
    m_minButton->setRect(QRectF(x, by, bw, bh));
    x -= 14 + 112;
    m_settingsButton->setRect(QRectF(x, strip.top() + 7, 112, 32));
    x -= 8 + 118;
    m_playlistButton->setRect(QRectF(x, strip.top() + 7, 118, 32));
    const bool wide = strip.width() > 520;
    m_playlistButton->setVisible(wide);
    m_settingsButton->setVisible(wide);

    // Dock.
    const QRectF dock(m, H - m - dockH, W - 2 * m, dockH);
    m_dockPanel->setRect(dock);

    // Preview fills what is left.
    const QRectF prev(m, strip.bottom() + 10, W - 2 * m, dock.top() - 10 - (strip.bottom() + 10));
    m_preview->setRect(prev);
    m_pasteLinkButton->setRect(m_preview->dropActionRect());
    m_pasteLinkButton->setVisible(m_preview->showsDropZone());

    // Dock contents.
    const qreal pad = 22;
    const qreal rowTop = dock.top() + 18;
    qreal rx = dock.right() - pad;
    m_playPauseButton->setRect(QRectF(rx - 40, rowTop + 3, 40, 40));
    rx -= 40 + 8;
    m_openVideoButton->setRect(QRectF(rx - 132, rowTop + 6, 132, 34));
    rx -= 132 + 8;
    m_removeVideoButton->setRect(QRectF(rx - 40, rowTop + 3, 40, 40));
    rx -= 40 + 14;
    const qreal textW = std::max<qreal>(40, rx - (dock.left() + pad));
    m_fileNameLabel->setRect(QRectF(dock.left() + pad, rowTop, textW, 24));
    m_fileDetailsLabel->setRect(QRectF(dock.left() + pad, rowTop + 24, textW, 20));
    m_statusDot->setRect(QRectF(dock.left() + pad, rowTop + 56, 14, 14));
    m_statusLabel->setRect(QRectF(dock.left() + pad + 22, rowTop + 52, dock.width() - 2 * pad - 22, 22));
    m_primaryButton->setRect(QRectF(dock.left() + pad, dock.bottom() - pad - 52, dock.width() - 2 * pad, 52));

    // The media panel hugs the media; re-fit the frame to the new size.
    QTimer::singleShot(0, this, [this] { renderPreviewFrame(); });
}

void MainWindow::updateWindowButtons() {
    if (!m_maxButton) {
        return;
    }
    m_maxButton->setKind(isMaximized() ? GlButton::Kind::Restore : GlButton::Kind::Maximize);
    m_maxButton->setToolTip(isMaximized() ? tr("Restore") : tr("Maximize"));
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG" && m_titlePanel &&
        GlUi::handleFrameHitTest(this, m_canvas, m_titlePanel->rect(), message, result)) {
        return true;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    GlUi::applyRoundedCorners(this);
    updateWindowButtons();
}

void MainWindow::buildTray() {
    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(QIcon(kAppIconResourcePath));
    m_tray->setToolTip(tr("Motiva"));

    m_trayMenu = new GlMenu(); // themed popup; shown from onTrayActivated(Context)
    m_trayPlayAction = m_trayMenu->addAction(QIcon(kPlayIconResourcePath), tr("Play"), this, [this] { m_manager->play(); updatePlayPauseLabel(); });
    m_trayPauseAction = m_trayMenu->addAction(QIcon(kPauseIconResourcePath), tr("Pause"), this, [this] { m_manager->pause(); updatePlayPauseLabel(); });
    m_trayMuteAction = m_trayMenu->addAction(QIcon(kVolumeMuteIconResourcePath), tr("Mute"));
    m_trayMuteAction->setCheckable(true);
    connect(m_trayMuteAction, &QAction::toggled, this, [this](bool checked) {
        if (m_settingsDialog) {
            m_settingsDialog->setMuted(checked);
        }
    });

    m_trayMenu->addSeparator();
    m_trayMenu->addAction(QIcon(settingsIconPath()), tr("Settings"), this, &MainWindow::openSettings);
    m_trayMenu->addAction(QIcon(playlistIconPath()), tr("Playlists"), this, &MainWindow::openPlaylist);
    m_trayMenu->addAction(QIcon(kOpenVideoIconResourcePath), tr("Open Media"), this, &MainWindow::onChooseVideo);
    m_trayMenu->addAction(QIcon(removeWallpaperIconPath()), tr("Remove Wallpaper"), this, &MainWindow::onRemoveWallpaper);
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(tr("Exit"), this, &MainWindow::onExitRequested);

    connect(m_tray, &QSystemTrayIcon::activated, this, &MainWindow::onTrayActivated);
    m_tray->show();
}

void MainWindow::restoreSettingsToUi() {
    m_selectedVideoPath = m_settings.videoPath();
    applyVideoInfoUi();
    m_settingsDialog->restoreFromSettings();
}

void MainWindow::applyVideoInfoUi() {
    if (m_selectedVideoPath.isEmpty()) {
        m_fileNameLabel->setText(tr("No file selected"));
        m_fileDetailsLabel->clear();
        m_fileDetailsLabel->setVisible(false);
        m_lastVideoDetailsText.clear();
        refreshDropZoneVisual();
        return;
    }

    const QUrl asUrl(m_selectedVideoPath);
    const bool isWebSource = isWebVideoUrl(asUrl);
    QString suffix;
    if (isWebSource) {
        // A URL's own QFileInfo() split is misleading (no real
        // filesystem semantics), so show the URL itself as the name.
        m_fileNameLabel->setText(m_selectedVideoPath);
        suffix = QFileInfo(asUrl.path()).suffix().toUpper();
    } else {
        QFileInfo fi(m_selectedVideoPath);
        m_fileNameLabel->setText(fi.fileName());
        suffix = fi.suffix().toUpper();
    }

    QStringList parts;
    if (isWebSource) {
        parts << tr("Web video");
    }
    if (isPlaylistDrivingMedia()) {
        const PlaylistModel* active = m_library->activePlaylist();
        const QString category = m_library->activeCategoryName();
        parts << tr("Playlist \"%1\"%2 %3 of %4")
                     .arg(active->name(), category.isEmpty() ? QString() : tr(" > %1").arg(category))
                     .arg(active->scopePosition()).arg(active->scopeCount());
    }
    const QSize size = m_manager->player()->videoNativeSize();
    if (size.isValid() && !size.isEmpty()) {
        parts << QStringLiteral("%1×%2").arg(size.width()).arg(size.height());
    }
    const qint64 durationMs = m_manager->player()->durationMs();
    if (durationMs > 0) {
        const qint64 totalSeconds = durationMs / 1000;
        parts << QStringLiteral("%1:%2").arg(totalSeconds / 60).arg(totalSeconds % 60, 2, 10, QChar('0'));
    }
    if (!suffix.isEmpty()) {
        parts << suffix;
    }

    const QString detailsText = parts.join(QStringLiteral("   •   "));
    if (detailsText != m_lastVideoDetailsText) {
        m_lastVideoDetailsText = detailsText;
        m_fileDetailsLabel->setText(detailsText);
        m_fileDetailsLabel->setVisible(!detailsText.isEmpty());
    }
    refreshDropZoneVisual();
}

void MainWindow::setUiState(WallpaperUiState state) {
    if (state != WallpaperUiState::Error) {
        m_lastErrorMessage.clear();
    }
    m_uiState = state;
    updateStatusUi();
    updatePrimaryButtonUi();
    syncPlaylistDialogState();
}

void MainWindow::updateStatusUi() {
    QString text;
    const char* color = kStatusNeutralColor;
    switch (m_uiState) {
    case WallpaperUiState::NoVideo:
        text = tr("No wallpaper selected");
        color = kStatusNeutralColor;
        break;
    case WallpaperUiState::Ready:
        text = tr("Ready");
        color = kStatusNeutralColor;
        break;
    case WallpaperUiState::Applying:
        text = tr("Applying wallpaper…");
        color = kStatusWarningColor;
        break;
    case WallpaperUiState::Active:
        // The wallpaper is still logically "Active" while battery-
        // suspended (see WallpaperManager::wallpaperSuspendedForBattery)
        // - only the status text changes, so the user understands why
        // the desktop looks different without this reading as an error
        // or as the wallpaper having been removed.
        if (m_batterySuspended) {
            text = m_manager->player()->isStaticImage() ? tr("Image hidden on battery")
                                                        : tr("Video paused on battery");
            color = kStatusWarningColor;
        } else {
            text = tr("Wallpaper Active");
            color = kStatusSuccessColor;
        }
        break;
    case WallpaperUiState::Error:
        text = m_lastErrorMessage.isEmpty() ? tr("Unable to apply wallpaper") : m_lastErrorMessage;
        color = kStatusErrorColor;
        break;
    }
    // Makes it obvious whether one image or the playlist is in use.
    if (isPlaylistDrivingMedia() && (m_uiState == WallpaperUiState::Ready || m_uiState == WallpaperUiState::Active)) {
        const QString category = m_library->activeCategoryName();
        text += category.isEmpty() ? tr("  •  Active playlist: %1").arg(m_library->activePlaylist()->name())
                                   : tr("  •  Active category: %1 > %2").arg(m_library->activePlaylist()->name(), category);
    }
    m_statusLabel->setText(text);
    m_statusDot->setPixmap(Theme::tintedIcon(QStringLiteral(":/status/status-dot.svg"), QColor(QLatin1String(color)),
                                             kStatusDotSize));
    m_statusLabel->setColor(QColor(QLatin1String(color)));
}

void MainWindow::updatePrimaryButtonUi() {
    // setStateIcon() (not a plain setIcon()) so hover/pressed/disabled
    // each keep showing their own dedicated artwork for whichever action
    // (set vs. remove) is current - see IconButton.h.
    switch (m_uiState) {
    case WallpaperUiState::NoVideo:
        m_primaryButton->setStateIcon(QIcon(setWallpaperIconPath()), QIcon(setWallpaperIconHoverPath()),
            QIcon(setWallpaperIconPressedPath()), QIcon(setWallpaperIconDisabledPath()));
        m_primaryButton->setText(tr("Set as Wallpaper"));
        m_primaryButton->setEnabled(false);
        break;
    case WallpaperUiState::Ready:
    case WallpaperUiState::Error:
        m_primaryButton->setStateIcon(QIcon(setWallpaperIconPath()), QIcon(setWallpaperIconHoverPath()),
            QIcon(setWallpaperIconPressedPath()), QIcon(setWallpaperIconDisabledPath()));
        m_primaryButton->setText(tr("Set as Wallpaper"));
        m_primaryButton->setEnabled(true);
        break;
    case WallpaperUiState::Applying:
        m_primaryButton->setStateIcon(QIcon(setWallpaperIconPath()), QIcon(setWallpaperIconHoverPath()),
            QIcon(setWallpaperIconPressedPath()), QIcon(setWallpaperIconDisabledPath()));
        m_primaryButton->setText(tr("Applying…"));
        m_primaryButton->setEnabled(false);
        break;
    case WallpaperUiState::Active:
        // Only one action makes sense once the wallpaper is genuinely
        // active - the button becomes "Remove Wallpaper" instead of
        // showing both actions as equally primary.
        m_primaryButton->setStateIcon(
            QIcon(removeWallpaperIconPath()), QIcon(removeWallpaperIconHoverPath()),
            QIcon(removeWallpaperIconPressedPath()), QIcon(removeWallpaperIconDisabledPath()));
        m_primaryButton->setText(tr("Remove Wallpaper"));
        // Reaching Active already implies a real, verified attach (see
        // the constructor's wallpaperVerified/wallpaperSuspendedForBattery
        // connections), but m_hasCurrentVideo is the explicit, session-
        // scoped gate the task calls for: Remove must visually disable
        // itself (not just silently no-op) whenever there is no video the
        // user loaded via Open/Drop/Paste this session - a merely-recent/
        // restored-from-settings path never sets it. setEnabled(false)
        // here uses IconButton's existing disabled styling/icon, same as
        // every other disabled state in this app.
        m_primaryButton->setEnabled(m_hasCurrentVideo);
        break;
    }
}

void MainWindow::updateRemoveVideoButtonUi() {
    if (m_removeVideoButton) {
        m_removeVideoButton->setEnabled(m_hasCurrentVideo);
    }
}

void MainWindow::onChooseVideo() {
    // Built from the same backend-reported extension set drag & drop uses
    // (supportedVideoContainerExtensions), plus GIF - never a hardcoded
    // "*.mp4"-only filter. "All Files" is offered too since the actual
    // backend decode attempt (VideoPlayer::loadFile, surfaced via
    // onWallpaperError) is still the real source of truth on whether a
    // selected file plays - this filter is a convenience, not a hard gate.
    // Image patterns come from VideoPlayer::supportedStaticImageExtensions
    // (the deployed QImageReader plugins), the same set drag & drop and the
    // Explorer verb accept.
    auto toPatterns = [](const QStringList& exts) {
        QStringList patterns;
        for (const QString& ext : exts) {
            patterns << QStringLiteral("*.%1").arg(ext);
        }
        patterns.sort(Qt::CaseInsensitive);
        return patterns.join(QLatin1Char(' '));
    };
    QStringList videoExts = supportedVideoContainerExtensions().values();
    videoExts << QStringLiteral("gif");
    const QStringList imageExts = VideoPlayer::supportedStaticImageExtensions().values();
    const QString filter = tr("Supported Media (%1);;Video Files (%2);;Image Files (%3);;All Files (*)")
        .arg(toPatterns(videoExts + imageExts), toPatterns(videoExts), toPatterns(imageExts));

    QString path = QFileDialog::getOpenFileName(this, tr("Open Media"), QString(), filter);
    if (path.isEmpty()) {
        return;
    }
    loadVideoSource(path);
}

void MainWindow::onPasteVideoLink() {
    // Pre-fills from the clipboard as a convenience only - the field stays
    // fully editable and nothing loads until the user explicitly presses
    // "Load Video". Reuses the exact same MIME-inspection
    // extractDroppedVideoSource() already uses for drag/drop and the Ctrl+V
    // shortcut, so "what counts as a usable clipboard URL" is defined in
    // exactly one place.
    const QString clipboardCandidate =
        extractDroppedVideoSource(QGuiApplication::clipboard()->mimeData(), nullptr);
    bool accepted = false;
    const QString typed = GlMessageBox::getText(this, tr("Paste Video URL"), tr("Video URL"), clipboardCandidate,
                                                tr("Load Video"), tr("https://example.com/video.mp4"), &accepted);
    if (!accepted) {
        return;
    }

    const QString entered = typed.trimmed();
    const QUrl candidate = QUrl::fromUserInput(entered);
    if (!isHttpUrl(candidate)) {
        // Covers both "hello world" (Test E) and a local filesystem path
        // like C:\Videos\video.mp4 - local files belong to the drag/drop
        // or Open Video workflow, never this URL field.
        GlMessageBox::warning(this, tr("Invalid URL"),
            tr("Please enter a valid http:// or https:// video URL."));
        return;
    }
    if (!hasSupportedVideoExtension(candidate.path())) {
        // A syntactically valid webpage URL (e.g. a YouTube watch page)
        // that this app's media backend cannot actually play as-is - see
        // isWebVideoUrl()'s comment. Deliberately does not attempt to
        // scrape/resolve/download it.
        GlMessageBox::warning(
            this, tr("Unsupported Video URL"), tr("This URL is not a directly playable video source."));
        return;
    }
    loadVideoSource(candidate.toString());
}

void MainWindow::onPasteShortcut() {
    const QMimeData* mimeData = QGuiApplication::clipboard()->mimeData();
    const QString source = extractDroppedVideoSource(mimeData, nullptr);
    if (source.isEmpty()) {
        // Not a recognized local video file or video URL - reject
        // gracefully (never attempt to load arbitrary clipboard text) and
        // just give the same subtle "not a video" feedback drag & drop
        // already uses for an unsupported drag, rather than a dialog.
        flashDropZoneInvalid();
        return;
    }
    qInfo() << "[Paste] Loading video source from the clipboard:" << source;
    loadVideoSource(source);
}

bool MainWindow::isUsableVideoSource(const QString& source) {
    if (source.isEmpty()) {
        return false;
    }
    const QUrl asUrl(source);
    if (isWebVideoUrl(asUrl)) {
        return true;
    }
    // Extension-gated, not just QFileInfo::exists(): this is what keeps
    // an unsupported file type rejected even via this path (startup
    // restore / second-instance IPC recovery), not just the file-picker/
    // drag-drop entry points. Still images are accepted here since v1.1.0
    // and are routed by VideoPlayer::loadFile to its QImageReader path -
    // never to QMediaPlayer, which would otherwise decode a PNG/JPG as a
    // degenerate one-frame "video" (confirmed by earlier testing).
    return QFileInfo::exists(source) && isSupportedLocalMediaFile(source);
}

void MainWindow::loadVideoSource(const QString& source, bool fromPlaylist) {
    if (!fromPlaylist && m_library && m_library->activeId() > 0) {
        qInfo() << "[Playlist] Single media chosen explicitly - deactivating the active playlist "
                    "(the playlist itself is kept).";
        m_library->setActive(0);
    }
    m_selectedVideoPath = source;
    updatePlayerSequencing();
    m_settings.setVideoPath(source);
    // Open Video, drag & drop, and Paste Video URL all converge on this
    // one function - so this is the single place that can honestly say
    // the user explicitly chose a video THIS session. See m_hasCurrentVideo's
    // declaration and the "Remove button correctness" task.
    m_hasCurrentVideo = true;
    updateRemoveVideoButtonUi();
    m_lastVideoDetailsText.clear();
    applyVideoInfoUi();

    // Start decoding immediately so the in-app preview box shows the video
    // right away, even before the user clicks "Set as Wallpaper". Works
    // identically for a local path or a web URL - VideoPlayer::loadFile
    // already handles both via the same QMediaPlayer pipeline.
    m_preview->setLoadingText(QString());
    if (m_manager->player()->loadFile(source)) {
        m_manager->player()->play();
    }
    updatePlayPauseAvailability();

    // If the wallpaper is currently active, the shared decode pipeline
    // already starts presenting this new content on the desktop too (see
    // VideoPlayer's class comment - one decoder, shared with every
    // WallpaperWindow) without a fresh Set as Wallpaper click, so the
    // Active state is still accurate and must not be downgraded here.
    if (m_uiState != WallpaperUiState::Active) {
        setUiState(WallpaperUiState::Ready);
    }
}

QString MainWindow::extractDroppedVideoSource(const QMimeData* mimeData, int* extraCandidateCount) {
    if (extraCandidateCount) {
        *extraCandidateCount = 0;
    }
    if (!mimeData) {
        return QString();
    }

    // hasUrls() covers BOTH local files dragged from Explorer (each a
    // file:// QUrl) and most browser drag payloads for a link/video
    // (text/uri-list, which Qt also surfaces via urls()) - one MIME check
    // handles both of the task's two input paths. First VALID candidate
    // wins; this app has no playlist/queue (see WallpaperManager - one
    // current video only), so multiple dropped files predictably keep
    // just the first, matching Explorer's own drag-multiple-files
    // left-to-right/selection-order convention, rather than guessing an
    // ordering or silently merging them.
    QString firstMatch;
    int extras = 0;
    if (mimeData->hasUrls()) {
        for (const QUrl& url : mimeData->urls()) {
            QString candidate;
            if (url.isLocalFile()) {
                const QString localPath = url.toLocalFile();
                if (QFileInfo(localPath).isFile() && isSupportedLocalMediaFile(localPath)) {
                    candidate = localPath;
                }
            } else if (isWebVideoUrl(url)) {
                candidate = url.toString();
            }
            if (candidate.isEmpty()) {
                continue;
            }
            if (firstMatch.isEmpty()) {
                firstMatch = candidate;
            } else {
                ++extras;
            }
        }
    }

    // Deliberately no plain-text fallback: interpreting arbitrary
    // dropped text as a local file path is exactly the "text that merely
    // happens to look like a path" the redesign task's security section
    // warns against. A URL dragged as plain text (some non-browser
    // sources do this) is still accepted, but ONLY if it strictly parses
    // as an absolute http(s) URL - never as a filesystem path.
    if (firstMatch.isEmpty() && mimeData->hasText()) {
        const QUrl asUrl = QUrl::fromUserInput(mimeData->text().trimmed());
        if (isWebVideoUrl(asUrl)) {
            firstMatch = asUrl.toString();
        }
    }

    // Explicit allowlist, not a denylist: this function only ever returns
    // a candidate that was BOTH an existing local file ending in .mp4 AND
    // matched by an isLocalFile() QUrl, OR an http(s) URL whose path ends
    // in .mp4 - every other payload a browser drag can carry (image/png,
    // image/jpeg, image/webp thumbnail bytes via hasImage()/imageData(),
    // text/html, a bare webpage/watch-page URL, or a data:/blob: URI) is
    // never inspected for its bytes and always falls through to the empty
    // return below, no matter what mimeData->formats() lists. A thumbnail
    // image dragged from a browser therefore can never become the loaded
    // video - see the "Fix Video Input UX" task, which asked this to be
    // verified/hardened rather than assumed. Logged here (not silently
    // swallowed) so a drag that carries only unusable payloads is visible
    // in %TEMP%\Motiva.log instead of just "nothing happened".
    if (firstMatch.isEmpty() && (mimeData->hasImage() || mimeData->hasHtml() ||
                                     (mimeData->hasUrls() && !mimeData->urls().isEmpty()) || mimeData->hasText())) {
        qInfo() << "[DragDrop] Ignoring drag/drop payload - formats present:" << mimeData->formats()
                << "- no existing local supported media file or directly playable http(s) video URL found among them.";
    }

    if (extraCandidateCount) {
        *extraCandidateCount = extras;
    }
    return firstMatch;
}

namespace {
constexpr int kVideoPageIndex = 0;
constexpr int kDropZonePageIndex = 1;
} // namespace

void MainWindow::refreshDropZoneVisual() {
    if (!m_preview) {
        return;
    }
    // The drop-zone page (animated visual) is shown whenever there's no
    // loaded video yet, OR a drag is currently in progress (even over an
    // already-loaded video, matching the original text-hint behavior it
    // replaces) - never both pages at once, and the animation never
    // overlaps a playing video.
    const bool showDropZone = m_dragHintActive || m_dragInvalidActive || m_selectedVideoPath.isEmpty();
    // A source is selected but its first frame hasn't been decoded yet (cold
    // start after Windows boots, slow disk, web video): the video page would
    // otherwise be an empty near-black panel, so say what is happening.
    if (!showDropZone && !m_preview->hasImage()) {
        m_preview->setLoadingText(tr("Loading preview…"));
    }
    m_preview->setShowDropZone(showDropZone);
    if (showDropZone) {
        m_preview->setAspect(0.0); // the drop zone uses the whole preview area
    }

    GlPreview::DropState state = GlPreview::DropState::Idle;
    if (m_dragInvalidActive) {
        state = GlPreview::DropState::Invalid;
    } else if (m_dragHintActive) {
        state = GlPreview::DropState::DragOver;
    }
    m_preview->setDropState(state);
    layoutGl(m_canvas->size()); // the Paste URL button follows the drop zone
}

void MainWindow::flashDropZoneInvalid() {
    if (!m_preview) {
        return;
    }
    // Reuses m_dragInvalidActive/refreshDropZoneVisual rather than poking
    // DropZoneWidget directly, so this composes correctly even if a real
    // drag happens to start immediately afterward.
    setDragInvalidActive(true);
    QTimer::singleShot(900, this, [this] { setDragInvalidActive(false); });
}

void MainWindow::setDragHintActive(bool active) {
    if (m_dragHintActive == active) {
        return;
    }
    m_dragHintActive = active;
    refreshDropZoneVisual();
}

void MainWindow::setDragInvalidActive(bool active) {
    if (m_dragInvalidActive == active) {
        return;
    }
    m_dragInvalidActive = active;
    refreshDropZoneVisual();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (!extractDroppedVideoSource(event->mimeData(), nullptr).isEmpty()) {
        event->acceptProposedAction();
        setDragInvalidActive(false);
        setDragHintActive(true);
    } else {
        // Still ignored at the Qt/OS level (so the cursor shows the usual
        // "not allowed" feedback), but the drop zone gets its own subtle
        // Invalid visual too - see DropZoneWidget::State::Invalid.
        event->ignore();
        setDragHintActive(false);
        setDragInvalidActive(true);
    }
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event) {
    if (!extractDroppedVideoSource(event->mimeData(), nullptr).isEmpty()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent* /*event*/) {
    setDragHintActive(false);
    setDragInvalidActive(false);
}

void MainWindow::dropEvent(QDropEvent* event) {
    setDragHintActive(false);
    setDragInvalidActive(false);
    int extraCandidates = 0;
    const QString source = extractDroppedVideoSource(event->mimeData(), &extraCandidates);
    if (source.isEmpty()) {
        event->ignore();
        qInfo() << "[DragDrop] Drop rejected - no supported local media file or web video URL found in the "
                    "dropped data.";
        return;
    }
    event->acceptProposedAction();
    qInfo() << "[DragDrop] Loading dropped video source:" << source
            << (extraCandidates > 0 ? QString(" (%1 additional dropped file(s) ignored)").arg(extraCandidates) : QString());
    loadVideoSource(source);
    if (extraCandidates > 0) {
        m_statusLabel->setToolTip(tr("%1 additional dropped file(s) were ignored - only one media file can be "
                                      "loaded at a time.").arg(extraCandidates));
    }
    // A short, purely visual success burst - loadVideoSource() above
    // already kicked off decoding in parallel, so this never delays
    // playback. refreshDropZoneVisual() (called from loadVideoSource's
    // applyVideoInfoUi) already switched to the video page since
    // m_selectedVideoPath is non-empty now; re-show the drop-zone page
    // just long enough for the burst, then hand back to the normal logic.
    if (m_preview) {
        m_preview->setShowDropZone(true);
        m_preview->setDropState(GlPreview::DropState::Success);
        QTimer::singleShot(650, this, [this] { refreshDropZoneVisual(); });
    }
}

void MainWindow::onSetWallpaper() {
    if (m_selectedVideoPath.isEmpty()) {
        GlMessageBox::information(this, tr("Choose media"), tr("Please open a video or image first."));
        return;
    }
    if (!isUsableVideoSource(m_selectedVideoPath)) {
        GlMessageBox::warning(this, tr("Media not found"),
            tr("The wallpaper file could not be found.\n\nPlease choose another video or image."));
        return;
    }

    if (m_manager->setWallpaper(m_selectedVideoPath)) {
        m_settings.setVideoPath(m_selectedVideoPath);
        m_settings.setWasWallpaperActive(true);
    }
    // UI state itself is driven by WallpaperManager::wallpaperActivated/
    // wallpaperVerified/errorOccurred (see the constructor's connections),
    // not set directly here - that keeps "Active" tied to the same
    // verification the backend already performs.
}

void MainWindow::onRemoveWallpaper() {
    m_manager->removeWallpaper();
    m_settings.setWasWallpaperActive(false);
    // Removing clears the "explicitly loaded this session" flag too, so
    // the button goes back to visually disabled until the user opens/
    // drops/pastes a video again - see m_hasCurrentVideo.
    m_hasCurrentVideo = false;
    updateRemoveVideoButtonUi();
    // UI state follows WallpaperManager::wallpaperRemoved.
}

void MainWindow::onRemoveVideo() {
    // Distinct action from onRemoveWallpaper() above: this unloads the
    // video from Motiva's OWN player/preview - it never touches the
    // desktop wallpaper detach path directly, and never deletes the file
    // or m_settings' persisted video path (this app's closest thing to
    // "history"/recent-item storage - see loadVideoSource()).
    if (!m_hasCurrentVideo) {
        return;
    }
    m_selectedVideoPath.clear();
    m_hasCurrentVideo = false;

    // If this same media also happens to be the active desktop wallpaper,
    // reuse the EXISTING wallpaper lifecycle hook (WallpaperManager::
    // removeWallpaper(), the same call onRemoveWallpaper() above makes) -
    // no parallel teardown path, no D3D/DirectComposition code here.
    // m_selectedVideoPath is already cleared above, so wallpaperRemoved's
    // handler (see the constructor) correctly lands on NoVideo rather than
    // Ready. Done before unload() below so the media change can't first
    // trigger a battery re-evaluation against a wallpaper that is about to
    // be removed anyway.
    // Removing the current media while the playlist supplies it also turns
    // the playlist off - otherwise the next trigger would bring an image
    // straight back. The playlist itself is kept.
    if (m_library->activeId() > 0) {
        m_library->setActive(0);
    }
    const bool wallpaperWasOurs =
        m_uiState == WallpaperUiState::Active || m_uiState == WallpaperUiState::Applying;
    if (wallpaperWasOurs) {
        m_manager->removeWallpaper();
        m_settings.setWasWallpaperActive(false);
    }

    // Stops playback and releases the source (decoder input / decoded
    // still image) - the file itself is never touched.
    m_manager->player()->unload();
    updateRemoveVideoButtonUi();
    updatePlayPauseAvailability();
    applyVideoInfoUi();
    m_preview->clearImage();
    m_preview->setLoadingText(QString());
    refreshDropZoneVisual();

    if (!wallpaperWasOurs) {
        setUiState(WallpaperUiState::NoVideo);
    }
}

void MainWindow::onPrimaryButtonClicked() {
    if (m_uiState == WallpaperUiState::Active) {
        onRemoveWallpaper();
    } else {
        onSetWallpaper();
    }
}

void MainWindow::onPlayPause() {
    if (m_manager->isPlaying()) {
        m_manager->pause();
    } else {
        m_manager->play();
    }
    updatePlayPauseLabel();
}

void MainWindow::updatePlayPauseAvailability() {
    // A still image has no playback to pause/resume. Video and animated
    // GIF/WebP keep Play/Pause exactly as before.
    const bool canPlay = !m_manager->player()->isStaticImage();
    m_playPauseButton->setEnabled(canPlay);
    m_playPauseButton->setToolTip(canPlay ? tr("Play or pause the preview")
                                          : tr("Play/Pause is not available for still images"));
    if (!canPlay) {
        m_trayPlayAction->setEnabled(false);
        m_trayPauseAction->setEnabled(false);
    } else {
        m_trayPlayAction->setEnabled(true);
        m_trayPauseAction->setEnabled(true);
    }
}

void MainWindow::updatePlayPauseLabel() {
    bool playing = m_manager->isPlaying();
    m_playPauseButton->setStateIcon(QIcon(playing ? kPauseIconResourcePath : kPlayIconResourcePath),
        QIcon(playing ? kPauseIconHoverPath : kPlayIconHoverPath));
    m_playPauseButton->setText(playing ? tr("Pause") : tr("Play"));
    m_trayPlayAction->setEnabled(!playing);
    m_trayPauseAction->setEnabled(playing);
}

void MainWindow::onWallpaperError(const QString& message) {
    m_lastErrorMessage = message;
    setUiState(WallpaperUiState::Error);

    // Technical details are already in Motiva.log (written where the error
    // is raised); the user gets the short reason. Repeats of the same
    // failure are collapsed by the notifier (cooldown), so a continuously
    // failing source can't flood the UI.
    Notification n;
    n.kind = Notification::Kind::Error;
    n.category = Notification::Category::Playback;
    n.title = tr("Wallpaper playback failed");
    n.message = message;
    n.key = QStringLiteral("playback-error");
    n.desktop = true;
    NotificationManager::instance()->notify(n);
}

void MainWindow::onWallpaperVerified() {
    setUiState(WallpaperUiState::Active);
    Notification n;
    n.kind = Notification::Kind::Success;
    n.category = Notification::Category::Playback;
    n.title = tr("Wallpaper Active");
    n.message = m_selectedVideoPath.isEmpty()
        ? tr("Your wallpaper is now showing on the desktop.")
        : tr("\"%1\" is now showing on the desktop.").arg(QFileInfo(m_selectedVideoPath).fileName());
    n.key = QStringLiteral("playback-active");
    n.rateLimit = true;
    n.cooldownMs = 5000;
    NotificationManager::instance()->notify(n);
}

void MainWindow::onPreviewFrameReady() {
    auto frame = m_manager->player()->currentFrame();
    if (!frame || frame->isNull()) {
        return;
    }
    // Info labels are cheap and updated even while hidden: main.cpp shows
    // the window only after the constructor (which may already have
    // loaded a previously-selected source) returns, and a single-frame
    // source fires this exactly once.
    applyVideoInfoUi();
    renderPreviewFrame();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::renderPreviewFrame() {
    auto frame = m_manager->player()->currentFrame();
    if (!frame || frame->isNull() || m_selectedVideoPath.isEmpty() || !m_preview) {
        return;
    }
    // Shape the panel to the media first, so the frame below is scaled to
    // its final size.
    if (frame->height() > 0 && !m_preview->showsDropZone()) {
        m_preview->setAspect(double(frame->width()) / frame->height());
    }
    // Scaling is the real per-frame cost, worth skipping while the window
    // isn't visible (tray) - the decode pipeline itself keeps running since
    // it is shared with the desktop wallpaper. A single-frame source is
    // re-rendered by layoutGl() on resize.
    if (!isVisible() || isMinimized()) {
        return;
    }
    const QRectF cr = m_preview->contentRect();
    if (cr.width() <= 4 || cr.height() <= 4) {
        return;
    }
    const qreal dpr = devicePixelRatioF();
    const QSize target(qMax(1, qRound((cr.width() - 2) * dpr)), qMax(1, qRound((cr.height() - 2) * dpr)));
    // Scale on the CPU to the panel's pixel size so the GPU only ever
    // uploads a preview-sized texture, never a full-resolution (8K) frame.
    QImage scaled = frame->scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    m_preview->setImage(scaled);
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Context) {
        m_trayMenu->popup(QCursor::pos());
        return;
    }
    if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) {
        if (isVisible()) {
            hide();
        } else {
            showNormal();
            raise();
            activateWindow();
        }
    }
}

void MainWindow::openSettings() {
    if (!m_settingsDialog) {
        return;
    }
    m_settingsDialog->refreshMonitorList();
    m_settingsDialog->show();
    m_settingsDialog->raise();
    m_settingsDialog->activateWindow();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Closing the window minimizes to tray; the wallpaper (if any) keeps
    // running. Actual exit happens via the tray menu's "Exit".
    if (m_tray && m_tray->isVisible()) {
        hide();
        event->ignore();
    } else {
        onExitRequested();
        event->accept();
    }
}

void MainWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        updateWindowButtons();
    }
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ThemeChange) {
        m_settingsButton->setIcon(QIcon(settingsIconPath()));
        m_playlistButton->setIcon(QIcon(playlistIconPath()));
        if (m_canvas) {
            m_canvas->requestAnimation();
        }
        updatePrimaryButtonUi();
    }
}

void MainWindow::recoverOrActivate() {
    qInfo() << "[IPC] Handling recovery request from a second launch attempt.";
    if (m_manager->isActive()) {
        m_manager->recoverOrActivate();
    } else if (isUsableVideoSource(m_selectedVideoPath)) {
        qInfo() << "[IPC] No wallpaper was active - attaching the last-configured video.";
        onSetWallpaper();
    } else {
        qInfo() << "[IPC] No wallpaper configured yet - nothing to recover, just bringing the UI forward.";
    }
    // Give the user visible confirmation that double-clicking the EXE did
    // something, instead of the previous silent no-op that forced End Task.
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::handleExplorerRequestedFile(const QString& path) {
    qInfo() << "[Explorer] Handling file supplied via \"Set as background\":" << path;
    // Untrusted input from Explorer/the command line - validated with the
    // exact same check every other input method (Open Video, drag & drop,
    // startup restore) already relies on, never executed/passed to a
    // shell, and never given a second, parallel loading path. The extra
    // isFile() check below (Explorer's own verb only ever supplies a real
    // file, but the command line is untrusted either way) rejects a
    // directory that happens to end in a supported extension - something
    // isUsableVideoSource() alone doesn't rule out, since every existing
    // caller of it only ever sees paths that already went through a file
    // picker/drag payload that can't produce a directory.
    const QUrl asUrl(path);
    if (!isWebVideoUrl(asUrl) && !QFileInfo(path).isFile()) {
        onWallpaperError(tr("Unsupported media.\n\nMotiva could not load \"%1\".").arg(path));
        return;
    }
    if (!isUsableVideoSource(path)) {
        onWallpaperError(tr("Unsupported media.\n\nMotiva could not load \"%1\".").arg(path));
        return;
    }
    loadVideoSource(path);
    onSetWallpaper();
}

void MainWindow::onExplorerFileReceived(const QString& path) {
    handleExplorerRequestedFile(path);
}

void MainWindow::onAddToPlaylistReceived(qint64 playlistId, const QString& path) {
    qInfo() << "[Explorer] \"Add to playlist\" playlist=" << playlistId << ":" << path;
    // Untrusted command-line/IPC input: only an existing file of a type a
    // playlist can hold (PlaylistModel's own media detection).
    const QFileInfo file(path);
    const bool video = PlaylistModel::isVideoFile(path);
    if (!file.isFile()) {
        showExplorerPlaylistError(tr("\"%1\" was not found.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (!video && !PlaylistModel::isImageFile(path)) {
        showExplorerPlaylistError(
            tr("\"%1\" can't be added to a playlist - playlists hold videos or still images.").arg(file.fileName()));
        return;
    }
    if (playlistId <= 0) {
        createPlaylistFromExplorer(path, video);
        return;
    }
    PlaylistModel* target = m_library->playlist(playlistId);
    if (!target) {
        // The menu was out of date (e.g. written while this playlist still
        // existed) - refresh it rather than guessing another playlist.
        syncExplorerPlaylistMenu(true);
        showExplorerPlaylistError(tr("That playlist no longer exists. \"%1\" was not added.").arg(file.fileName()));
        return;
    }
    if (!target->isMixed() && target->isVideo() != video) {
        showExplorerPlaylistError(tr("\"%1\" is a %2 playlist, so \"%3\" can't be added to it.")
                                      .arg(target->name(), target->isVideo() ? tr("video") : tr("image"),
                                           file.fileName()));
        return;
    }
    // The normal add path: one shared media record per file, a file already
    // in this playlist is reported as such and not added twice.
    m_playlistDialog->addFilesTo(playlistId, {path});
    openPlaylist();
}

void MainWindow::createPlaylistFromExplorer(const QString& path, bool video) {
    showNormal();
    raise();
    activateWindow();
    const QString fallback = video ? tr("Videos") : tr("Images");
    bool accepted = false;
    QString name = GlMessageBox::getText(this, tr("New Playlist"),
                                         video ? tr("Name of the new video playlist:")
                                               : tr("Name of the new image playlist:"),
                                         QLineEdit::Normal, fallback, &accepted)
                       .trimmed();
    if (!accepted) {
        qInfo() << "[Explorer] New playlist cancelled - nothing added:" << path;
        return;
    }
    if (name.isEmpty()) {
        name = fallback;
    }
    // Same library call as Playlists > + New (names are made unique there).
    const qint64 id = m_library->createPlaylist(name, video ? PlaylistType::Video : PlaylistType::Image);
    if (id <= 0) {
        showExplorerPlaylistError(tr("The playlist could not be created:\n%1").arg(m_library->lastError()));
        return;
    }
    m_playlistDialog->addFilesTo(id, {path});
    openPlaylist();
}

void MainWindow::showExplorerPlaylistError(const QString& message) {
    qWarning() << "[Explorer] Add to playlist:" << message;
    showNormal();
    raise();
    activateWindow();
    GlMessageBox::warning(this, tr("Add to playlist"), message);
}

void MainWindow::syncExplorerPlaylistMenu(bool force) {
    if (!m_settings.explorerIntegrationEnabled()) {
        m_explorerMenuSignature.clear();
        return;
    }
    QVector<WindowsShellIntegration::ExplorerPlaylist> images, videos;
    QString signature;
    const PlaylistListModel* list = m_library->listModel();
    for (int row = 0; row < list->rowCount(); ++row) {
        const QModelIndex index = list->index(row);
        const qint64 id = index.data(PlaylistListModel::IdRole).toLongLong();
        const QString name = index.data(Qt::DisplayRole).toString();
        const auto type = static_cast<PlaylistType>(index.data(PlaylistListModel::TypeRole).toInt());
        const bool video = type == PlaylistType::Video;
        if (type == PlaylistType::Mixed) {
            images.append({id, name}); // a mixed playlist takes both kinds
            videos.append({id, name});
        } else {
            (video ? videos : images).append({id, name});
        }
        signature += QStringLiteral("%1|%2|%3\n").arg(id).arg(type == PlaylistType::Mixed ? 'm' : (video ? 'v' : 'i')).arg(name);
    }
    if (!force && signature == m_explorerMenuSignature) {
        return;
    }
    if (WindowsShellIntegration::UpdateExplorerPlaylistMenu(SettingsManager::motivaExecutablePath(), images, videos)) {
        m_explorerMenuSignature = signature;
        qInfo() << "[Explorer] Playlist submenu updated:" << images.size() << "image /" << videos.size()
                << "video playlist(s).";
    }
}

void MainWindow::openPlaylist() {
    if (!m_playlistDialog) {
        return;
    }
    syncPlaylistDialogState();
    // One window, reused: a minimized one is restored, a hidden one shown,
    // an open one just brought forward.
    if (m_playlistDialog->isMinimized()) {
        m_playlistDialog->showNormal();
    } else {
        m_playlistDialog->show();
    }
    m_playlistDialog->raise();
    m_playlistDialog->activateWindow();
}

bool MainWindow::isPlaylistDrivingMedia() const {
    const PlaylistModel* active = m_library ? m_library->activePlaylist() : nullptr;
    return active && !m_selectedVideoPath.isEmpty() &&
        QDir::toNativeSeparators(m_selectedVideoPath).compare(active->currentPath(), Qt::CaseInsensitive) == 0;
}

void MainWindow::updatePlayerSequencing() {
    const PlaylistModel* active = m_library ? m_library->activePlaylist() : nullptr;
    // A video of a video OR mixed playlist plays once and hands over to the
    // next item; an image of a mixed playlist is not sequenced.
    m_manager->player()->setSequencedPlayback(active && active->currentIsVideo() && isPlaylistDrivingMedia());
}

void MainWindow::syncPlaylistDialogState() {
    if (m_playlistDialog) {
        m_playlistDialog->setActiveOnDesktop(
            isPlaylistDrivingMedia() &&
            (m_uiState == WallpaperUiState::Active || m_uiState == WallpaperUiState::Applying));
    }
}

void MainWindow::applyPlaylistItem() {
    PlaylistModel* active = m_library->activePlaylist();
    if (!active) {
        return;
    }
    const QString path = active->currentPath();
    if (path.isEmpty()) {
        return;
    }
    if (!QFileInfo(path).isFile()) {
        // Missing file: move on to the next available one (that emits
        // currentChanged, which re-enters here). If none is available, keep
        // whatever is showing - never blank the wallpaper over it.
        qInfo() << "[Playlist] Current item is missing:" << path;
        {
            Notification n;
            n.kind = Notification::Kind::Warning;
            n.category = Notification::Category::Playback;
            n.title = tr("Wallpaper File Not Found");
            n.message = tr("\"%1\" is missing, so Motiva skipped it.").arg(QFileInfo(path).fileName());
            n.key = QStringLiteral("playlist-missing:") + path;
            n.desktop = true;
            NotificationManager::instance()->notify(n);
        }
        active->refreshAvailability();
        if (!active->advance(true)) {
            qWarning() << "[Playlist] No available item in" << active->name() << "- keeping the current media.";
        }
        return;
    }
    const VideoPlayer::MediaKind kind = m_manager->player()->mediaKind();
    const bool alreadyLoaded = isPlaylistDrivingMedia() &&
        (active->currentIsVideo() ? kind == VideoPlayer::MediaKind::Video : kind == VideoPlayer::MediaKind::StaticImage);
    if (alreadyLoaded) {
        updatePlayerSequencing(); // e.g. the same video was open as standalone media
        return;
    }
    qInfo() << "[Playlist] Presenting" << active->name() << "item" << (active->currentIndex() + 1) << "of"
            << active->count() << ":" << path;
    loadVideoSource(path, /*fromPlaylist=*/true);
    syncPlaylistDialogState();
}

void MainWindow::onPlayerEndOfMedia() {
    PlaylistModel* active = m_library->activePlaylist();
    if (!active || !active->currentIsVideo() || !isPlaylistDrivingMedia()) {
        return; // standalone media / an image keeps its existing behavior
    }
    // The existing "Loop video" preference decides what happens after the
    // last video: on -> start the playlist again from the first, off ->
    // stop on the last video.
    const bool loop = m_manager->player()->isLooping();
    qInfo() << "[Playlist]" << active->name() << "- video" << (active->currentIndex() + 1) << "ended.";
    if (active->advance(loop)) {
        return; // currentChanged -> applyPlaylistItem loads the next video
    }
    if (loop) {
        m_manager->player()->restartFromBeginning(); // only one available video
    } else {
        qInfo() << "[Playlist]" << active->name() << "- reached the last video (Loop video is off).";
    }
}

void MainWindow::onApplyPlaylistToDesktop(qint64 playlistId, qint64 categoryId) {
    // The same playlist pipeline for a whole playlist (categoryId 0) and for
    // one of its categories: the category only limits which items rotate.
    if ((m_library->activeId() != playlistId || m_library->activeCategoryId() != categoryId) &&
        !m_library->setActive(playlistId, categoryId)) {
        return; // nothing available - PlaylistDialog keeps its button disabled for this
    }
    applyPlaylistItem();
    if (!isPlaylistDrivingMedia()) {
        return;
    }
    if (m_uiState != WallpaperUiState::Active && m_uiState != WallpaperUiState::Applying) {
        onSetWallpaper();
    }
    syncPlaylistDialogState();
}

void MainWindow::releaseMediaForCleanup() {
    // Cleanup & Reset is about to remove the library and saved media
    // references: detach Motiva's wallpaper through the normal
    // WallpaperManager path (tears down the render windows - no orphaned
    // HWND, no D3D/DirectComposition code here) and unload the player, so
    // nothing still points at data that is about to disappear. The media
    // files themselves are never touched.
    qInfo() << "[Cleanup] Releasing wallpaper and current media before cleanup.";
    m_manager->removeWallpaper();
    m_settings.setWasWallpaperActive(false);
    m_selectedVideoPath.clear();
    m_hasCurrentVideo = false;
    m_manager->player()->unload();
    updateRemoveVideoButtonUi();
    updatePlayPauseAvailability();
    applyVideoInfoUi();
    m_preview->clearImage();
    refreshDropZoneVisual();
    setUiState(WallpaperUiState::NoVideo);
}

void MainWindow::restartAfterFactoryReset() {
    // Everything on disk is back to fresh-install state; restarting rebuilds
    // every in-memory object (theme, dialogs, library, recovery state) from
    // it rather than patching each one in place. Deliberately NOT
    // onExitRequested(): that writes settings/recovery state back, which
    // would re-create part of what Factory Reset just removed.
    qInfo() << "[Cleanup] Factory Reset complete - restarting Motiva.";
    if (m_manager) {
        m_manager->removeWallpaper(); // already released; idempotent
    }
    if (m_tray) {
        m_tray->hide();
    }
    // --after-reset: the new process waits for this one to release the
    // single-instance lock instead of handing off to it (see main.cpp).
    // Same executable and inherited environment as this process, so a
    // deployed install still finds its DLLs.
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), {QStringLiteral("--after-reset")})) {
        GlMessageBox::information(this, tr("Motiva"),
                                 tr("Motiva has been reset. Please start Motiva again."));
    }
    qApp->quit();
}

void MainWindow::onExitRequested() {
    // A running backup stops at the next chunk and removes its partial file.
    if (m_backup) {
        m_backup->cancelAndWait();
    }
    // Explicit, deterministic teardown *before* the event loop stops:
    // WallpaperManager::removeWallpaper() stops the player and destroys
    // the native render windows, which in turn lets Qt Multimedia's
    // FFmpeg-backed decoder pipeline release its internal worker threads
    // while the event loop is still alive to service any async cleanup
    // it depends on. Relying solely on destructors running *after*
    // app.exec() returns is what left the process alive in the
    // background (Task Manager) after "closing" the app - by then there
    // is no running event loop left for that cleanup to complete on.
    if (m_manager) {
        m_manager->removeWallpaper();
    }
    // Mirror onRemoveWallpaper()'s bookkeeping: removeWallpaper() above
    // already cleared RecoveryState's wallpaperWasAttached, but this path
    // bypasses onRemoveWallpaper() itself, which is the only other place
    // that clears SettingsManager's registry-persisted wasWallpaperActive.
    // Left uncleared, a deliberate Exit would detach the wallpaper visually
    // right now yet still leave "was active" on disk, so the startup
    // restore check (see the constructor) would silently reapply it on the
    // next launch even though the user explicitly asked to stop - exactly
    // the "wallpaper keeps coming back" symptom this fixes. An unclean
    // kill/crash never reaches this function at all, so the flag is left
    // untouched in that case - that's the correct, distinct "legitimate
    // recovery" path (RecoveryState.cleanExit also stays false for it).
    m_settings.setWasWallpaperActive(false);
    if (m_tray) {
        m_tray->hide();
    }
    m_recoveryState.setCleanExit(true);
    qApp->quit();
}
