#include "MainWindow.h"
#include "SettingsDialog.h"
#include "StartupDiagnostics.h"
#include "WindowsDesktopWallpaper.h"
#include "Theme.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
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

namespace {
// Embedded via resources/app.qrc - loading via the Qt resource path keeps
// this independent of the process's working directory, unlike a relative
// filesystem path. The window/tray identity icon stays a raster .ico
// (Assets/application/motiva.ico) since that's what Windows itself needs
// for the exe/taskbar/Alt-Tab icon; the in-app UI icons are all .svg
// (Assets/<feature>/...), Qt's native SVG icon engine (Qt6::Svg).
constexpr const char* kAppIconResourcePath = ":/application/motiva.ico";
constexpr const char* kSettingsIconResourcePath = ":/settings-icon/settings.svg";
constexpr const char* kOpenVideoIconResourcePath = ":/video/open-video.svg";
constexpr const char* kLinkIconResourcePath = ":/video/link.svg";
constexpr const char* kPlayIconResourcePath = ":/playback/play.svg";
constexpr const char* kPauseIconResourcePath = ":/playback/pause.svg";
constexpr const char* kVolumeIconResourcePath = ":/playback/volume.svg";
constexpr const char* kVolumeMuteIconResourcePath = ":/playback/volume-mute.svg";
constexpr const char* kSetWallpaperIconResourcePath = ":/wallpaper/set-wallpaper.svg";
constexpr const char* kRemoveWallpaperIconResourcePath = ":/wallpaper/remove-wallpaper.svg";

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

// Matches the existing Open Video dialog's own filter ("MP4 Video
// (*.mp4)") - drag & drop deliberately doesn't accept a broader set than
// what's already exposed through that dialog, even though the underlying
// FFmpeg backend can technically decode more. Extend both together if
// that scope ever changes.
bool hasSupportedVideoExtension(const QString& fileNameOrPath) {
    return fileNameOrPath.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive);
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

MainWindow::MainWindow(bool startMinimized, QWidget* parent)
    : QMainWindow(parent), m_manager(std::make_unique<WallpaperManager>()) {
    qInfo() << "[Lifecycle] MainWindow construction begin, startMinimized=" << startMinimized;
    setWindowTitle("Motiva");
    setWindowIcon(QIcon(kAppIconResourcePath));
    resize(560, 680);
    setMinimumSize(420, 480);
    // Whole-window drop target (see dragEnterEvent/dropEvent) - the
    // preview area is the visual focus of the drag hint, but the actual
    // Qt drop target is the window so a drop anywhere on it still works.
    setAcceptDrops(true);

    buildUi();
    buildTray();

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

    m_manager->setRecoveryState(&m_recoveryState);
    // The single-instance winner (only instance that ever reaches this
    // constructor - see main.cpp) starts listening for recovery requests
    // from any later launch attempt.
    m_ipc.startListening();
    connect(&m_ipc, &InstanceIpc::recoverRequested, this, &MainWindow::recoverOrActivate);

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
    });
    connect(m_manager->player(), &VideoPlayer::frameReady, this, &MainWindow::onPreviewFrameReady);

    restoreSettingsToUi();

    // Load (but don't attach to the desktop) whatever video was previously
    // selected, purely so the in-app preview has something to show.
    if (isUsableVideoSource(m_selectedVideoPath)) {
        m_manager->player()->loadFile(m_selectedVideoPath);
        m_manager->player()->play();
        setUiState(WallpaperUiState::Ready);
    } else {
        setUiState(WallpaperUiState::NoVideo);
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
    WindowsDesktopWallpaper::RefreshDesktopBackground();

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

    if (startMinimized) {
        hide();
    }
    StartupDiagnostics::instance().mark("mainWindowReady");
    qInfo() << "[Lifecycle] MainWindow construction complete.";
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(20, 16, 20, 20);
    root->setSpacing(12);

    // --- Header: app name + settings entry point ---
    auto* header = new QHBoxLayout();
    header->setSpacing(8);
    auto* titleLabel = new QLabel(tr("Motiva"), central);
    titleLabel->setObjectName(QStringLiteral("appTitle"));
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleLabel->setFont(titleFont);
    header->addWidget(titleLabel);
    header->addStretch();

    m_settingsButton = new QToolButton(central);
    m_settingsButton->setObjectName(QStringLiteral("settingsButton"));
    m_settingsButton->setIcon(QIcon(kSettingsIconResourcePath));
    m_settingsButton->setText(tr("Settings"));
    m_settingsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_settingsButton->setToolTip(tr("Open settings"));
    m_settingsButton->setAutoRaise(true);
    connect(m_settingsButton, &QToolButton::clicked, this, &MainWindow::openSettings);
    header->addWidget(m_settingsButton);
    root->addLayout(header);

    // A flat 1px rule (styled via Theme's QFrame#headerRule rule) instead
    // of the platform's own 3D sunken groove - a single hairline separator
    // reads as modern/native on Windows 11, a chiseled groove does not.
    auto* headerRule = new QFrame(central);
    headerRule->setObjectName(QStringLiteral("headerRule"));
    headerRule->setFrameShape(QFrame::HLine);
    headerRule->setFrameShadow(QFrame::Plain);
    root->addWidget(headerRule);

    // --- Video preview: the visual focus of the window ---
    // A QStackedLayout switches between the two pages below, driven by
    // refreshDropZoneVisual(): the animated drop-zone page whenever no
    // video is loaded (or a drag is in progress, even over an already-
    // loaded video), and the plain pixmap page once a video is actually
    // playing - the animation never overlaps/covers real video content.
    auto* previewContainer = new QWidget(central);
    previewContainer->setMinimumHeight(220);
    previewContainer->setStyleSheet(kPreviewSurfaceStyle);
    previewContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_previewStack = new QStackedLayout(previewContainer);
    m_previewStack->setContentsMargins(0, 0, 0, 0);

    m_previewLabel = new QLabel(previewContainer);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewStack->addWidget(m_previewLabel); // index kVideoPageIndex

    auto* dropZoneWrapper = new QWidget(previewContainer);
    auto* dropZoneLayout = new QVBoxLayout(dropZoneWrapper);
    dropZoneLayout->setContentsMargins(24, 20, 24, 16);
    dropZoneLayout->setSpacing(4);
    m_dropZone = new DropZoneWidget(dropZoneWrapper);
    dropZoneLayout->addWidget(m_dropZone, /*stretch=*/1);
    auto* dropTitleLabel = new QLabel(tr("Drag & Drop Video"), dropZoneWrapper);
    dropTitleLabel->setAlignment(Qt::AlignCenter);
    QFont dropTitleFont = dropTitleLabel->font();
    dropTitleFont.setBold(true);
    dropTitleLabel->setFont(dropTitleFont);
    dropTitleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::kPreviewTextStrong));
    dropZoneLayout->addWidget(dropTitleLabel);
    auto* dropSubtitleLabel = new QLabel(tr("Local MP4 files"), dropZoneWrapper);
    dropSubtitleLabel->setAlignment(Qt::AlignCenter);
    dropSubtitleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::kPreviewText));
    dropZoneLayout->addWidget(dropSubtitleLabel);

    // Three clearly separate input methods, not one overloaded drop zone
    // (see the "Fix Video Input UX" task): drag & drop above is scoped to
    // local files (the subtitle says so explicitly); a web video goes
    // through this dedicated button/dialog instead of relying on a
    // browser's drag payload, which often carries a thumbnail image
    // rather than the actual video - see extractDroppedVideoSource()'s
    // comment on why that thumbnail is never mistaken for a video either
    // way. Open Video (below the preview) remains the third, unchanged.
    auto* orLabel = new QLabel(QStringLiteral("— %1 —").arg(tr("OR")), dropZoneWrapper);
    orLabel->setAlignment(Qt::AlignCenter);
    orLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::kPreviewTextFaint));
    dropZoneLayout->addWidget(orLabel);

    // On the fixed-dark preview surface (see kPreviewSurfaceStyle above),
    // so this button gets its own light-on-dark styling rather than the
    // app-wide light-surface QPushButton rule, which would be illegible
    // here regardless of the OS theme.
    m_pasteLinkButton = new QPushButton(QIcon(kLinkIconResourcePath), tr("Paste Video URL"), dropZoneWrapper);
    m_pasteLinkButton->setObjectName(QStringLiteral("pasteLinkButton"));
    m_pasteLinkButton->setStyleSheet(QStringLiteral(
        "QPushButton#pasteLinkButton {"
        "  background: rgba(255,255,255,0.06); color: %1;"
        "  border: 1px solid rgba(255,255,255,0.14); border-radius: %2px; padding: 6px 14px; }"
        "QPushButton#pasteLinkButton:hover { background: rgba(255,255,255,0.1); border-color: rgba(255,255,255,0.22); }"
        "QPushButton#pasteLinkButton:pressed { background: rgba(255,255,255,0.04); }")
        .arg(Theme::kPreviewTextStrong).arg(Theme::kRadiusSmall));
    m_pasteLinkButton->setToolTip(tr("Load a video from a direct URL (or press Ctrl+V anywhere in this window)"));
    connect(m_pasteLinkButton, &QPushButton::clicked, this, &MainWindow::onPasteVideoLink);
    dropZoneLayout->addWidget(m_pasteLinkButton, 0, Qt::AlignHCenter);
    m_previewStack->addWidget(dropZoneWrapper); // index kDropZonePageIndex

    root->addWidget(previewContainer, /*stretch=*/1);

    // --- Video info + secondary actions ---
    auto* infoRow = new QHBoxLayout();
    infoRow->setSpacing(8);
    auto* infoTextLayout = new QVBoxLayout();
    infoTextLayout->setSpacing(2);
    m_fileNameLabel = new QLabel(tr("No file selected"), central);
    QFont fileFont = m_fileNameLabel->font();
    fileFont.setBold(true);
    m_fileNameLabel->setFont(fileFont);
    infoTextLayout->addWidget(m_fileNameLabel);
    m_fileDetailsLabel = new QLabel(central);
    m_fileDetailsLabel->setObjectName(QStringLiteral("fileDetailsLabel"));
    m_fileDetailsLabel->setVisible(false);
    infoTextLayout->addWidget(m_fileDetailsLabel);
    infoRow->addLayout(infoTextLayout, /*stretch=*/1);

    m_playPauseButton = new QPushButton(QIcon(kPlayIconResourcePath), tr("Play"), central);
    m_playPauseButton->setToolTip(tr("Play or pause the preview"));
    connect(m_playPauseButton, &QPushButton::clicked, this, &MainWindow::onPlayPause);
    infoRow->addWidget(m_playPauseButton);

    m_openVideoButton = new QPushButton(QIcon(kOpenVideoIconResourcePath), tr("Open Video"), central);
    connect(m_openVideoButton, &QPushButton::clicked, this, &MainWindow::onChooseVideo);
    infoRow->addWidget(m_openVideoButton);

    root->addLayout(infoRow);

    // --- Status ---
    m_statusLabel = new QLabel(central);
    root->addWidget(m_statusLabel);

    // --- Primary action: the one obvious next step ---
    // objectName "primaryButton" is what gives this its distinct accent
    // styling (Theme::appStyleSheet's QPushButton#primaryButton rules) -
    // the one clearly primary action, every other button on this window
    // stays the default secondary button style.
    m_primaryButton = new QPushButton(tr("Set as Wallpaper"), central);
    m_primaryButton->setObjectName(QStringLiteral("primaryButton"));
    m_primaryButton->setMinimumHeight(42);
    m_primaryButton->setDefault(true);
    connect(m_primaryButton, &QPushButton::clicked, this, &MainWindow::onPrimaryButtonClicked);
    root->addWidget(m_primaryButton);

    setCentralWidget(central);

    m_settingsDialog = new SettingsDialog(m_manager.get(), &m_settings, this);
    connect(m_settingsDialog, &SettingsDialog::mutedChanged, this, [this](bool muted) {
        if (m_trayMuteAction) {
            m_trayMuteAction->blockSignals(true);
            m_trayMuteAction->setChecked(muted);
            m_trayMuteAction->setIcon(QIcon(muted ? kVolumeMuteIconResourcePath : kVolumeIconResourcePath));
            m_trayMuteAction->blockSignals(false);
        }
    });
}

void MainWindow::buildTray() {
    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(QIcon(kAppIconResourcePath));
    m_tray->setToolTip(tr("Motiva"));

    auto* menu = new QMenu();
    m_trayPlayAction = menu->addAction(QIcon(kPlayIconResourcePath), tr("Play"), this, [this] { m_manager->play(); updatePlayPauseLabel(); });
    m_trayPauseAction = menu->addAction(QIcon(kPauseIconResourcePath), tr("Pause"), this, [this] { m_manager->pause(); updatePlayPauseLabel(); });
    m_trayMuteAction = menu->addAction(QIcon(kVolumeMuteIconResourcePath), tr("Mute"));
    m_trayMuteAction->setCheckable(true);
    connect(m_trayMuteAction, &QAction::toggled, this, [this](bool checked) {
        if (m_settingsDialog) {
            m_settingsDialog->setMuted(checked);
        }
    });

    menu->addSeparator();
    menu->addAction(QIcon(kSettingsIconResourcePath), tr("Settings"), this, &MainWindow::openSettings);
    menu->addAction(QIcon(kOpenVideoIconResourcePath), tr("Open Video"), this, &MainWindow::onChooseVideo);
    menu->addAction(QIcon(kRemoveWallpaperIconResourcePath), tr("Remove Wallpaper"), this, &MainWindow::onRemoveWallpaper);
    menu->addSeparator();
    menu->addAction(tr("Exit"), this, &MainWindow::onExitRequested);

    m_tray->setContextMenu(menu);
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
        text = tr("Wallpaper Active");
        color = kStatusSuccessColor;
        break;
    case WallpaperUiState::Error:
        text = m_lastErrorMessage.isEmpty() ? tr("Unable to apply wallpaper") : m_lastErrorMessage;
        color = kStatusErrorColor;
        break;
    }
    m_statusLabel->setText(QStringLiteral("●  ") + text);
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1; font-weight: 600; padding: 2px 0;").arg(color));
}

void MainWindow::updatePrimaryButtonUi() {
    switch (m_uiState) {
    case WallpaperUiState::NoVideo:
        m_primaryButton->setIcon(QIcon(kSetWallpaperIconResourcePath));
        m_primaryButton->setText(tr("Set as Wallpaper"));
        m_primaryButton->setEnabled(false);
        break;
    case WallpaperUiState::Ready:
    case WallpaperUiState::Error:
        m_primaryButton->setIcon(QIcon(kSetWallpaperIconResourcePath));
        m_primaryButton->setText(tr("Set as Wallpaper"));
        m_primaryButton->setEnabled(true);
        break;
    case WallpaperUiState::Applying:
        m_primaryButton->setIcon(QIcon(kSetWallpaperIconResourcePath));
        m_primaryButton->setText(tr("Applying…"));
        m_primaryButton->setEnabled(false);
        break;
    case WallpaperUiState::Active:
        // Only one action makes sense once the wallpaper is genuinely
        // active - the button becomes "Remove Wallpaper" instead of
        // showing both actions as equally primary.
        m_primaryButton->setIcon(QIcon(kRemoveWallpaperIconResourcePath));
        m_primaryButton->setText(tr("Remove Wallpaper"));
        m_primaryButton->setEnabled(true);
        break;
    }
}

void MainWindow::onChooseVideo() {
    QString path = QFileDialog::getOpenFileName(this, tr("Open Video"), QString(), tr("MP4 Video (*.mp4)"));
    if (path.isEmpty()) {
        return;
    }
    loadVideoSource(path);
}

void MainWindow::onPasteVideoLink() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Paste Video URL"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(10);

    auto* label = new QLabel(tr("Video URL"), &dialog);
    QFont labelFont = label->font();
    labelFont.setBold(true);
    label->setFont(labelFont);
    layout->addWidget(label);

    auto* urlEdit = new QLineEdit(&dialog);
    urlEdit->setPlaceholderText(tr("https://example.com/video.mp4"));
    // Pre-fills from the clipboard as a convenience only - the field
    // stays fully editable and nothing loads until the user explicitly
    // presses "Load Video" below. Reuses the exact same MIME-inspection
    // extractDroppedVideoSource() already uses for drag/drop and the
    // Ctrl+V shortcut, so "what counts as a usable clipboard URL" is
    // defined in exactly one place.
    const QString clipboardCandidate =
        extractDroppedVideoSource(QGuiApplication::clipboard()->mimeData(), nullptr);
    if (!clipboardCandidate.isEmpty()) {
        urlEdit->setText(clipboardCandidate);
        urlEdit->selectAll();
    }
    layout->addWidget(urlEdit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* loadButton = buttons->addButton(tr("Load Video"), QDialogButtonBox::AcceptRole);
    loadButton->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.setMinimumWidth(380);
    // Modal - the drop-zone's own idle/float animation behind it is
    // already paused for free (DropZoneWidget::hideEvent fires once this
    // modal dialog occludes/deactivates the main window's rendering the
    // same way any other overlapping window would), so no separate
    // "pause the background animation" bookkeeping is needed here.
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString entered = urlEdit->text().trimmed();
    const QUrl candidate = QUrl::fromUserInput(entered);
    if (!isHttpUrl(candidate)) {
        // Covers both "hello world" (Test E) and a local filesystem path
        // like C:\Videos\video.mp4 - local files belong to the drag/drop
        // or Open Video workflow, never this URL field.
        QMessageBox::warning(this, tr("Invalid URL"),
            tr("Please enter a valid http:// or https:// video URL."));
        return;
    }
    if (!hasSupportedVideoExtension(candidate.path())) {
        // A syntactically valid webpage URL (e.g. a YouTube watch page)
        // that this app's media backend cannot actually play as-is - see
        // isWebVideoUrl()'s comment. Deliberately does not attempt to
        // scrape/resolve/download it.
        QMessageBox::warning(
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
    return QFileInfo::exists(source);
}

void MainWindow::loadVideoSource(const QString& source) {
    m_selectedVideoPath = source;
    m_settings.setVideoPath(source);
    m_lastVideoDetailsText.clear();
    applyVideoInfoUi();

    // Start decoding immediately so the in-app preview box shows the video
    // right away, even before the user clicks "Set as Wallpaper". Works
    // identically for a local path or a web URL - VideoPlayer::loadFile
    // already handles both via the same QMediaPlayer pipeline.
    m_previewLabel->setText(QString());
    if (m_manager->player()->loadFile(source)) {
        m_manager->player()->play();
    }

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
                if (QFileInfo::exists(localPath) && hasSupportedVideoExtension(localPath)) {
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
                << "- no existing local .mp4 or directly playable http(s) .mp4 URL found among them.";
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
    if (!m_previewStack || !m_dropZone) {
        return;
    }
    // The drop-zone page (animated visual) is shown whenever there's no
    // loaded video yet, OR a drag is currently in progress (even over an
    // already-loaded video, matching the original text-hint behavior it
    // replaces) - never both pages at once, and the animation never
    // overlaps a playing video.
    const bool showDropZone = m_dragHintActive || m_dragInvalidActive || m_selectedVideoPath.isEmpty();
    m_previewStack->setCurrentIndex(showDropZone ? kDropZonePageIndex : kVideoPageIndex);

    DropZoneWidget::State state = DropZoneWidget::State::Idle;
    if (m_dragInvalidActive) {
        state = DropZoneWidget::State::Invalid;
    } else if (m_dragHintActive) {
        state = DropZoneWidget::State::DragOver;
    }
    m_dropZone->setState(state);
}

void MainWindow::flashDropZoneInvalid() {
    if (!m_dropZone) {
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
        qInfo() << "[DragDrop] Drop rejected - no supported local video or web video URL found in the "
                    "dropped data.";
        return;
    }
    event->acceptProposedAction();
    qInfo() << "[DragDrop] Loading dropped video source:" << source
            << (extraCandidates > 0 ? QString(" (%1 additional dropped file(s) ignored)").arg(extraCandidates) : QString());
    loadVideoSource(source);
    if (extraCandidates > 0) {
        m_statusLabel->setToolTip(tr("%1 additional dropped file(s) were ignored - only one video can be "
                                      "loaded at a time.").arg(extraCandidates));
    }
    // A short, purely visual success burst - loadVideoSource() above
    // already kicked off decoding in parallel, so this never delays
    // playback. refreshDropZoneVisual() (called from loadVideoSource's
    // applyVideoInfoUi) already switched to the video page since
    // m_selectedVideoPath is non-empty now; re-show the drop-zone page
    // just long enough for the burst, then hand back to the normal logic.
    if (m_dropZone && m_previewStack) {
        m_previewStack->setCurrentIndex(kDropZonePageIndex);
        m_dropZone->setState(DropZoneWidget::State::Success);
        QTimer::singleShot(650, this, [this] { refreshDropZoneVisual(); });
    }
}

void MainWindow::onSetWallpaper() {
    if (m_selectedVideoPath.isEmpty()) {
        QMessageBox::information(this, tr("Choose a video"), tr("Please open a video first."));
        return;
    }
    if (!isUsableVideoSource(m_selectedVideoPath)) {
        QMessageBox::warning(this, tr("Video not found"),
            tr("Wallpaper video could not be found.\n\nPlease choose another video."));
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
    // UI state follows WallpaperManager::wallpaperRemoved.
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

void MainWindow::updatePlayPauseLabel() {
    bool playing = m_manager->isPlaying();
    m_playPauseButton->setIcon(QIcon(playing ? kPauseIconResourcePath : kPlayIconResourcePath));
    m_playPauseButton->setText(playing ? tr("Pause") : tr("Play"));
    m_trayPlayAction->setEnabled(!playing);
    m_trayPauseAction->setEnabled(playing);
}

void MainWindow::onWallpaperError(const QString& message) {
    m_lastErrorMessage = message;
    setUiState(WallpaperUiState::Error);

    const QString logPath = QDir::toNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/Motiva.log");
    const QString detailed = message + tr("\n\nDetails were written to:\n%1").arg(logPath);
    if (isVisible()) {
        QMessageBox::warning(this, tr("Motiva"), detailed);
    } else if (m_tray) {
        m_tray->showMessage(tr("Motiva"), message, QSystemTrayIcon::Warning, 5000);
    }
}

void MainWindow::onWallpaperVerified() {
    setUiState(WallpaperUiState::Active);
}

void MainWindow::onPreviewFrameReady() {
    // Only bother painting the preview while the window is actually
    // visible (main window shown, not minimized to tray) - the decode
    // pipeline itself keeps running regardless since it's shared with the
    // desktop wallpaper.
    if (!isVisible() || !m_previewLabel) {
        return;
    }
    auto frame = m_manager->player()->currentFrame();
    if (!frame || frame->isNull()) {
        return;
    }
    QPixmap pixmap = QPixmap::fromImage(*frame).scaled(
        m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_previewLabel->setPixmap(pixmap);
    applyVideoInfoUi();
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
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

void MainWindow::onExitRequested() {
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
