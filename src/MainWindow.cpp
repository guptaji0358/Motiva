#include "MainWindow.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QFileInfo>
#include <QStyle>
#include <QStandardPaths>
#include <QDir>
#include <QPixmap>

MainWindow::MainWindow(bool startMinimized, QWidget* parent)
    : QMainWindow(parent), m_manager(std::make_unique<WallpaperManager>()) {
    setWindowTitle("Video Wallpaper");
    resize(480, 560);

    buildUi();
    buildTray();

    connect(m_manager.get(), &WallpaperManager::errorOccurred, this, &MainWindow::onWallpaperError);
    connect(m_manager.get(), &WallpaperManager::wallpaperActivated, this, [this] {
        m_statusLabel->setText(tr("Wallpaper active."));
        updatePlayPauseLabel();
    });
    connect(m_manager.get(), &WallpaperManager::wallpaperRemoved, this, [this] {
        m_statusLabel->setText(tr("No wallpaper active."));
        updatePlayPauseLabel();
    });
    connect(m_manager->player(), &VideoPlayer::frameReady, this, &MainWindow::onPreviewFrameReady);

    restoreSettingsToUi();

    // Load (but don't attach to the desktop) whatever video was previously
    // selected, purely so the in-app preview has something to show.
    if (!m_selectedVideoPath.isEmpty() && QFileInfo::exists(m_selectedVideoPath)) {
        m_manager->player()->loadFile(m_selectedVideoPath);
        m_manager->player()->play();
    }

    // Restore previous wallpaper if the app was auto-started or the user
    // had one active when they last closed the app to tray.
    if ((startMinimized || m_settings.wasWallpaperActive()) && !m_selectedVideoPath.isEmpty()) {
        onSetWallpaper();
    }

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
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    auto* title = new QLabel(tr("Current Wallpaper"));
    QFont f = title->font();
    f.setBold(true);
    f.setPointSize(f.pointSize() + 1);
    title->setFont(f);
    layout->addWidget(title);

    m_previewLabel = new QLabel(central);
    m_previewLabel->setMinimumHeight(180);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setStyleSheet("background-color: #1c1c1c; border-radius: 8px; color: #888;");
    m_previewLabel->setText(tr("No video selected"));
    layout->addWidget(m_previewLabel);
    m_selectedFileLabel = new QLabel(tr("No file selected"));
    m_selectedFileLabel->setStyleSheet("color: #666;");
    layout->addWidget(m_selectedFileLabel);

    m_chooseButton = new QPushButton(tr("Choose Video"));
    connect(m_chooseButton, &QPushButton::clicked, this, &MainWindow::onChooseVideo);
    layout->addWidget(m_chooseButton);

    auto* form = new QFormLayout();
    form->setSpacing(8);

    m_scalingCombo = new QComboBox();
    m_scalingCombo->addItems({tr("Fill"), tr("Fit"), tr("Stretch"), tr("Original")});
    connect(m_scalingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onScalingChanged);
    form->addRow(tr("Scaling:"), m_scalingCombo);

    m_monitorCombo = new QComboBox();
    connect(m_monitorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onMonitorSelectionChanged);
    form->addRow(tr("Monitor:"), m_monitorCombo);
    populateMonitorCombo();

    layout->addLayout(form);

    auto* volLayout = new QHBoxLayout();
    volLayout->addWidget(new QLabel(tr("Volume:")));
    m_volumeSlider = new QSlider(Qt::Horizontal);
    m_volumeSlider->setRange(0, 100);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &MainWindow::onVolumeChanged);
    volLayout->addWidget(m_volumeSlider);
    layout->addLayout(volLayout);

    m_muteCheck = new QCheckBox(tr("Mute"));
    connect(m_muteCheck, &QCheckBox::toggled, this, &MainWindow::onMuteToggled);
    layout->addWidget(m_muteCheck);

    m_loopCheck = new QCheckBox(tr("Loop video"));
    connect(m_loopCheck, &QCheckBox::toggled, this, &MainWindow::onLoopToggled);
    layout->addWidget(m_loopCheck);

    m_startWithWindowsCheck = new QCheckBox(tr("Start with Windows"));
    connect(m_startWithWindowsCheck, &QCheckBox::toggled, this, &MainWindow::onStartWithWindowsToggled);
    layout->addWidget(m_startWithWindowsCheck);

    auto* buttonRow = new QHBoxLayout();
    m_playPauseButton = new QPushButton(tr("▶ Play"));
    connect(m_playPauseButton, &QPushButton::clicked, this, &MainWindow::onPlayPause);
    buttonRow->addWidget(m_playPauseButton);
    layout->addLayout(buttonRow);

    m_setWallpaperButton = new QPushButton(tr("Set as Wallpaper"));
    m_setWallpaperButton->setStyleSheet("font-weight: bold; padding: 8px;");
    connect(m_setWallpaperButton, &QPushButton::clicked, this, &MainWindow::onSetWallpaper);
    layout->addWidget(m_setWallpaperButton);

    m_removeWallpaperButton = new QPushButton(tr("Remove Wallpaper"));
    connect(m_removeWallpaperButton, &QPushButton::clicked, this, &MainWindow::onRemoveWallpaper);
    layout->addWidget(m_removeWallpaperButton);

    m_statusLabel = new QLabel(tr("No wallpaper active."));
    m_statusLabel->setStyleSheet("color: #666;");
    layout->addWidget(m_statusLabel);

    layout->addStretch();
    setCentralWidget(central);
}

void MainWindow::buildTray() {
    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_tray->setToolTip(tr("Video Wallpaper"));

    auto* menu = new QMenu();
    m_trayPlayAction = menu->addAction(tr("▶ Play"), this, [this] { m_manager->play(); updatePlayPauseLabel(); });
    m_trayPauseAction = menu->addAction(tr("⏸ Pause"), this, [this] { m_manager->pause(); updatePlayPauseLabel(); });
    m_trayMuteAction = menu->addAction(tr("\U0001F507 Mute"));
    m_trayMuteAction->setCheckable(true);
    connect(m_trayMuteAction, &QAction::toggled, m_muteCheck, &QCheckBox::setChecked);

    menu->addSeparator();
    menu->addAction(tr("Settings"), this, [this] { showNormal(); raise(); activateWindow(); });
    menu->addAction(tr("Choose Wallpaper"), this, &MainWindow::onChooseVideo);
    menu->addAction(tr("Remove Wallpaper"), this, &MainWindow::onRemoveWallpaper);
    menu->addSeparator();
    menu->addAction(tr("Exit"), this, &MainWindow::onExitRequested);

    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, &MainWindow::onTrayActivated);
    m_tray->show();
}

void MainWindow::populateMonitorCombo() {
    m_monitorCombo->clear();
    m_monitorCombo->addItem(tr("All monitors"));
    m_monitorCombo->addItem(tr("Primary monitor"));

    auto monitors = m_manager->availableMonitors();
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& mon = monitors[i];
        QString label = tr("Monitor %1 (%2x%3)%4")
            .arg(i + 1)
            .arg(mon.rect.right - mon.rect.left)
            .arg(mon.rect.bottom - mon.rect.top)
            .arg(mon.isPrimary ? tr(" - Primary") : "");
        m_monitorCombo->addItem(label);
    }
}

void MainWindow::restoreSettingsToUi() {
    m_selectedVideoPath = m_settings.videoPath();
    applyCurrentVideoLabel();

    m_scalingCombo->setCurrentIndex(m_settings.scalingMode());
    m_manager->setScalingMode(static_cast<ScalingMode>(m_settings.scalingMode()));

    int monSel = m_settings.monitorSelection();
    int specific = m_settings.specificMonitorIndex();
    m_monitorCombo->setCurrentIndex(monSel == 2 ? 2 + qMax(0, specific) : monSel);

    m_volumeSlider->setValue(m_settings.volume());
    m_muteCheck->setChecked(m_settings.muted());
    m_loopCheck->setChecked(m_settings.loop());
    m_startWithWindowsCheck->setChecked(m_settings.startWithWindows());

    m_manager->setVolume(m_settings.volume());
    m_manager->setMuted(m_settings.muted());
    m_manager->setLooping(m_settings.loop());
}

void MainWindow::applyCurrentVideoLabel() {
    if (m_selectedVideoPath.isEmpty()) {
        m_selectedFileLabel->setText(tr("No file selected"));
    } else {
        m_selectedFileLabel->setText(QFileInfo(m_selectedVideoPath).fileName() + "\n" + m_selectedVideoPath);
    }
}

void MainWindow::onChooseVideo() {
    QString path = QFileDialog::getOpenFileName(this, tr("Choose Video"), QString(), tr("MP4 Video (*.mp4)"));
    if (path.isEmpty()) {
        return;
    }
    m_selectedVideoPath = path;
    m_settings.setVideoPath(path);
    applyCurrentVideoLabel();

    // Start decoding immediately so the in-app preview box shows the video
    // right away, even before the user clicks "Set as Wallpaper".
    m_previewLabel->setText(QString());
    if (m_manager->player()->loadFile(path)) {
        m_manager->player()->play();
    }
}

void MainWindow::onSetWallpaper() {
    if (m_selectedVideoPath.isEmpty()) {
        QMessageBox::information(this, tr("Choose a video"), tr("Please choose an MP4 file first."));
        return;
    }
    if (!QFileInfo::exists(m_selectedVideoPath)) {
        QMessageBox::warning(this, tr("Video not found"),
            tr("Wallpaper video could not be found.\n\nPlease choose another video."));
        return;
    }

    if (m_manager->setWallpaper(m_selectedVideoPath)) {
        m_settings.setVideoPath(m_selectedVideoPath);
        m_settings.setWasWallpaperActive(true);
    }
}

void MainWindow::onRemoveWallpaper() {
    m_manager->removeWallpaper();
    m_settings.setWasWallpaperActive(false);
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
    m_playPauseButton->setText(playing ? tr("⏸ Pause") : tr("▶ Play"));
    m_trayPlayAction->setEnabled(!playing);
    m_trayPauseAction->setEnabled(playing);
}

void MainWindow::onLoopToggled(bool checked) {
    m_manager->setLooping(checked);
    m_settings.setLoop(checked);
}

void MainWindow::onVolumeChanged(int value) {
    m_manager->setVolume(value);
    m_settings.setVolume(value);
}

void MainWindow::onMuteToggled(bool checked) {
    m_manager->setMuted(checked);
    m_settings.setMuted(checked);
    m_trayMuteAction->blockSignals(true);
    m_trayMuteAction->setChecked(checked);
    m_trayMuteAction->blockSignals(false);
}

void MainWindow::onScalingChanged(int index) {
    m_manager->setScalingMode(static_cast<ScalingMode>(index));
    m_settings.setScalingMode(index);
}

void MainWindow::onMonitorSelectionChanged(int index) {
    if (index == 0) {
        m_manager->setMonitorSelection(MonitorSelection::All);
        m_settings.setMonitorSelection(0);
    } else if (index == 1) {
        m_manager->setMonitorSelection(MonitorSelection::Primary);
        m_settings.setMonitorSelection(1);
    } else {
        int specificIndex = index - 2;
        m_manager->setMonitorSelection(MonitorSelection::Specific, specificIndex);
        m_settings.setMonitorSelection(2);
        m_settings.setSpecificMonitorIndex(specificIndex);
    }
}

void MainWindow::onStartWithWindowsToggled(bool checked) {
    m_settings.setStartWithWindows(checked);
}

void MainWindow::onWallpaperError(const QString& message) {
    m_statusLabel->setText(message);
    const QString logPath = QDir::toNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/VideoWallpaper.log");
    const QString detailed = message + tr("\n\nDetails were written to:\n%1").arg(logPath);
    if (isVisible()) {
        QMessageBox::warning(this, tr("Video Wallpaper"), detailed);
    } else if (m_tray) {
        m_tray->showMessage(tr("Video Wallpaper"), message, QSystemTrayIcon::Warning, 5000);
    }
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
    if (m_tray) {
        m_tray->hide();
    }
    qApp->quit();
}
