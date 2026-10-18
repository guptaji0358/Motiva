#include "SettingsDialog.h"
#include "WallpaperManager.h"
#include "SettingsManager.h"
#include "DialogSizing.h"
#include "Theme.h"
#include "GlUi.h"
#include "ThemeEditorDialog.h"
#include "ThemeTransitionOverlay.h"
#include "CleanupManager.h"
#include "CleanupWindow.h"
#include "BackupManager.h"
#include "BackupWindow.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QSlider>
#include <QCheckBox>
#include <QLabel>
#include <QIcon>
#include <QApplication>
#include <QWidget>
#include <QSizePolicy>
#include <QTimer>
#include <QPushButton>
#include <QTabWidget>
#include <QListWidget>
#include <QStackedWidget>
#include <QScrollArea>
#include <algorithm>

SettingsDialog::SettingsDialog(WallpaperManager* manager, SettingsManager* settings, CleanupManager* cleanup,
                               BackupManager* backup, QWidget* parent)
    : QDialog(parent), m_manager(manager), m_settings(settings), m_cleanup(cleanup), m_backup(backup) {
    setWindowTitle(tr("Settings"));
    // Borderless GL window like the main window (see GlUi): custom title
    // strip, drag/resize through nativeEvent().
    setWindowFlag(Qt::FramelessWindowHint, true);
    m_transitionOverlay = new ThemeTransitionOverlay(this);
    // Embedded via resources/app.qrc (Assets/settings-icon/settings.svg)
    // - same gear icon as the header button/tray menu entry that opens
    // this dialog. Picks the light/dark SVG variant matching the current
    // Motiva Theme - see Theme::iconVariant() and MainWindow.cpp's
    // settingsIconPath().
    setWindowIcon(QIcon(QStringLiteral(":/settings-icon/%1/settings.svg")
                             .arg(Theme::iconVariant(Theme::currentTheme()))));
    buildUi();
    connect(m_cleanup, &CleanupManager::preferencesChanged, this, &SettingsDialog::onPreferencesChangedExternally);
    resize(920, 640);
    setMinimumSize(640, 440);
}

void SettingsDialog::buildUi() {
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    m_canvas = new GlCanvas(this);
    box->addWidget(m_canvas);
    m_canvas->setAccessibleName(tr("Settings"));
    m_canvas->setLayoutCallback([this](const QSizeF& s) { layoutGl(s); });
    m_canvas->setWheelCallback([this](int delta, const QPointF& pos) {
        if (m_contentPanel && m_contentPanel->rect().contains(pos)) {
            scrollBy(-delta / 120.0 * 64.0);
        }
    });

    // Settings are grouped into pages. Every control is created exactly once;
    // the navigation style (tabs / sidebar / one grouped page) only decides
    // which rows are laid out where, so values are never duplicated or reset.
    m_pages = {{0, tr("General")},
               {1, tr("Appearance")},
               {2, tr("Display and Playback")},
               {3, tr("Notifications")},
               {4, tr("Integration")}};
    enum { PgGeneral, PgAppearance, PgDisplay, PgNotify, PgIntegration };

    // Panels first so every control paints above them.
    m_titlePanel = new GlPanel(m_canvas);
    m_titlePanel->setBlock(GlPanel::Block::Accent);
    m_titlePanel->setRadiusPx(16);
    m_navPanel = new GlPanel(m_canvas);
    m_navPanel->setBlock(GlPanel::Block::Accent2);
    m_contentPanel = new GlPanel(m_canvas);
    m_contentPanel->setRadiusPx(20);

    m_titleLabel = new GlLabel(m_canvas);
    m_titleLabel->setText(tr("Settings"));
    m_titleLabel->setPointSize(14);
    m_titleLabel->setBold(true);
    m_subtitleLabel = new GlLabel(m_canvas);
    m_subtitleLabel->setText(tr("Preferences"));
    m_subtitleLabel->setPointSize(7.5);
    m_subtitleLabel->setBold(true);
    m_subtitleLabel->setCaps(true);
    m_subtitleLabel->setTone(GlLabel::Tone::Dim);
    m_closeButton = new GlButton(m_canvas, GlButton::Kind::Close);
    m_closeButton->setToolTip(tr("Close"));
    connect(m_closeButton, &GlButton::clicked, this, &QDialog::close);

    // Rows ------------------------------------------------------------
    const auto addSection = [this](int page, const QString& title) {
        auto* l = new GlLabel(m_canvas);
        l->setText(title);
        l->setCaps(true);
        l->setBold(true);
        l->setPointSize(8.5);
        l->setTone(GlLabel::Tone::Accent);
        l->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
        m_rows.push_back({Row::Section, page, l, nullptr, 46});
    };
    const auto addToggle = [this](int page, const QString& text, const QString& tip = QString(), qreal indent = 0) {
        auto* t = new GlToggle(m_canvas);
        t->setText(text);
        t->setToolTip(tip);
        t->setIndent(indent);
        m_rows.push_back({Row::Toggle, page, t, nullptr, 46});
        return t;
    };
    const auto addDropdown = [this](int page, const QString& text) {
        auto* d = new GlDropdown(m_canvas);
        d->setText(text);
        m_rows.push_back({Row::Dropdown, page, d, nullptr, 54});
        return d;
    };
    const auto addButtonRow = [this](int page, GlLabel* desc, const QString& button) {
        auto* b = new GlButton(m_canvas, GlButton::Kind::Secondary);
        b->setText(button);
        m_rows.push_back({Row::ButtonRow, page, desc, b, 58});
        return b;
    };
    const auto makeDesc = [this](const QString& text) {
        auto* l = new GlLabel(m_canvas);
        l->setText(text);
        l->setPointSize(9.5);
        l->setTone(GlLabel::Tone::Dim);
        l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        return l;
    };

    // General
    addSection(PgGeneral, tr("General"));
    m_startWithWindowsCheck = addToggle(PgGeneral, tr("Start with Windows"));
    connect(m_startWithWindowsCheck, &GlToggle::toggled, this, &SettingsDialog::onStartWithWindowsToggled);
    m_playImagesOnBatteryCheck = addToggle(
        PgGeneral, tr("Play Images on Battery"),
        tr("Keep image wallpapers visible (and let image playlists advance) while running on battery power. "
           "When off, they are hidden on battery and come back when AC power returns."));
    connect(m_playImagesOnBatteryCheck, &GlToggle::toggled, this, &SettingsDialog::onPlayImagesOnBatteryToggled);
    m_playVideosOnBatteryCheck = addToggle(
        PgGeneral, tr("Play Videos on Battery"),
        tr("Keep video wallpapers playing while running on battery power. When off, they are paused and hidden "
           "on battery and come back when AC power returns."));
    connect(m_playVideosOnBatteryCheck, &GlToggle::toggled, this, &SettingsDialog::onPlayVideosOnBatteryToggled);

    // Appearance
    addSection(PgAppearance, tr("Theme"));
    // One "Theme" selector - each entry is a complete, independently-designed
    // Motiva appearance (see Theme.h). Item order matches Theme::AppTheme's
    // ordinal order exactly, so the index casts straight to Theme::AppTheme.
    m_themeCombo = addDropdown(PgAppearance, tr("Theme"));
    m_themeCombo->addItem(Theme::themeName(Theme::AppTheme::DarkAurora));
    m_themeCombo->addItem(Theme::themeName(Theme::AppTheme::LightAurora));
    m_themeCombo->addItem(Theme::themeName(Theme::AppTheme::DarkOnyx));
    m_themeCombo->addItem(Theme::themeName(Theme::AppTheme::LightOnyx));
    m_themeCombo->setToolTip(tr("Choose Motiva's visual theme."));
    connect(m_themeCombo, &GlDropdown::currentIndexChanged, this, &SettingsDialog::onThemeChanged);
    auto* customizeThemeButton =
        addButtonRow(PgAppearance, makeDesc(tr("Edit colors, pick an accent preset, check contrast.")), tr("Customize Theme..."));
    customizeThemeButton->setToolTip(
        tr("Edit the colors of the current theme, pick an accent preset, or check text contrast. "
           "Each theme keeps its own customization, and the original palette can always be restored."));
    connect(customizeThemeButton, &GlButton::clicked, this, [this] {
        ThemeEditorDialog editor(m_settings, this);
        editor.exec();
    });

    // Display and Playback
    addSection(PgDisplay, tr("Display"));
    m_scalingCombo = addDropdown(PgDisplay, tr("Scaling"));
    m_scalingCombo->addItems({tr("Fill"), tr("Fit"), tr("Stretch"), tr("Original")});
    connect(m_scalingCombo, &GlDropdown::currentIndexChanged, this, &SettingsDialog::onScalingChanged);
    m_monitorCombo = addDropdown(PgDisplay, tr("Monitor"));
    connect(m_monitorCombo, &GlDropdown::currentIndexChanged, this, &SettingsDialog::onMonitorSelectionChanged);
    refreshMonitorList();
    addSection(PgDisplay, tr("Playback"));
    {
        auto* v = new GlSlider(m_canvas);
        v->setText(tr("Volume"));
        v->setRange(0, 100);
        m_rows.push_back({Row::Slider, PgDisplay, v, nullptr, 54});
        m_volumeSlider = v;
    }
    connect(m_volumeSlider, &GlSlider::valueChanged, this, &SettingsDialog::onVolumeChanged);
    m_muteCheck = addToggle(PgDisplay, tr("Mute"));
    connect(m_muteCheck, &GlToggle::toggled, this, &SettingsDialog::onMuteToggled);
    m_loopCheck = addToggle(PgDisplay, tr("Loop video"));
    connect(m_loopCheck, &GlToggle::toggled, this, &SettingsDialog::onLoopToggled);

    // Notifications (see NotificationManager). Each switch is saved the
    // moment it is toggled and read live by the notifier - no restart.
    addSection(PgNotify, tr("Notifications"));
    struct NotifyRow {
        const char* text;
        const char* tip;
        void (SettingsManager::*setter)(bool);
    };
    const NotifyRow rows[7] = {
        {"Show in-app notifications", "Short messages that appear inside Motiva's windows.",
         &SettingsManager::setNotifyInApp},
        {"Show Windows notifications",
         "Native Windows notifications for errors, warnings and finished tasks while Motiva isn't in front. "
         "Windows' own notification settings and Focus Assist still apply.",
         &SettingsManager::setNotifyDesktop},
        {"Successes", "Confirmations that something finished, such as a wallpaper added to a playlist.",
         &SettingsManager::setNotifySuccess},
        {"Errors", "Explanations when something failed.", &SettingsManager::setNotifyError},
        {"Warnings", "Recoverable problems such as a missing file or an unreadable folder.",
         &SettingsManager::setNotifyWarning},
        {"Background tasks", "Results of searches and other long-running work.",
         &SettingsManager::setNotifyBackground},
        {"Wallpaper playback", "When the wallpaper changes or playback fails.",
         &SettingsManager::setNotifyPlayback},
    };
    for (int i = 0; i < 7; ++i) {
        m_notifyChecks[i] = addToggle(PgNotify, tr(rows[i].text), tr(rows[i].tip), i >= 2 ? 22 : 0); // sub-switches
        const auto setter = rows[i].setter;
        connect(m_notifyChecks[i], &GlToggle::toggled, this, [this, setter](bool on) { (m_settings->*setter)(on); });
    }

    // Integration
    addSection(PgIntegration, tr("Windows Integration"));
    m_showInWindowsSearchCheck = addToggle(PgIntegration, tr("Show Motiva in Windows Search"),
                                           tr("Add a Start Menu shortcut so Motiva can be found via Windows Search."));
    connect(m_showInWindowsSearchCheck, &GlToggle::toggled, this, &SettingsDialog::onShowInWindowsSearchToggled);
    m_explorerIntegrationCheck = addToggle(
        PgIntegration, tr("Set as background for supported media"),
        tr("Adds one \"Motiva\" entry to the right-click menu for supported video, GIF and image files "
           "in File Explorer, with \"Set as background\" and \"Add to playlist\" (videos and images). On "
           "Windows 11 this may appear under \"Show more options\"."));
    connect(m_explorerIntegrationCheck, &GlToggle::toggled, this, &SettingsDialog::onExplorerIntegrationToggled);

    // Optional local media backup: the ON/OFF switch (OFF by default) and a
    // one-line status; details, progress and storage live in BackupWindow.
    addSection(PgIntegration, tr("Media Backup"));
    m_backupCheck = addToggle(PgIntegration, tr("Back up my media"), BackupWindow::explanationText());
    connect(m_backupCheck, &GlToggle::toggled, this, [this](bool on) {
        const bool ok = on ? BackupWindow::confirmAndEnable(m_backup, this) : BackupWindow::confirmAndDisable(m_backup, this);
        if (!ok) {
            m_backupCheck->blockSignals(true);
            m_backupCheck->setChecked(!on);
            m_backupCheck->blockSignals(false);
        }
        updateBackupRow();
    });
    m_backupStatus = makeDesc(QString());
    auto* manageBackup = addButtonRow(PgIntegration, m_backupStatus, tr("Manage…"));
    connect(manageBackup, &GlButton::clicked, this, &SettingsDialog::openBackup);
    for (auto signal : {&BackupManager::enabledChanged}) {
        connect(m_backup, signal, this, &SettingsDialog::updateBackupRow);
    }
    connect(m_backup, &BackupManager::stateChanged, this, &SettingsDialog::updateBackupRow);
    connect(m_backup, &BackupManager::progressChanged, this, &SettingsDialog::updateBackupRow);
    connect(m_backup, &BackupManager::statsChanged, this, &SettingsDialog::updateBackupRow);
    updateBackupRow();

    // Compact entry only - the cleanup levels live in their own window.
    addSection(PgIntegration, tr("Cleanup & Reset"));
    auto* openCleanupButton =
        addButtonRow(PgIntegration, makeDesc(tr("Manage cache, data, and factory reset.")), tr("Open Cleanup"));
    connect(openCleanupButton, &GlButton::clicked, this, &SettingsDialog::openCleanup);

    // Grouped-page headings (only shown in the "Grouped page" layout).
    for (const SettingsPage& page : m_pages) {
        auto* h = new GlLabel(m_canvas);
        h->setText(page.title);
        h->setPointSize(13);
        h->setBold(true);
        h->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
        m_headings.push_back(h);
    }

    // Navigation entries (tabs along the top, or a sidebar column).
    for (int i = 0; i < int(m_pages.size()); ++i) {
        auto* b = new GlButton(m_canvas, GlButton::Kind::Tab);
        b->setText(m_pages[i].title);
        connect(b, &GlButton::clicked, this, [this, i] { setPage(i); });
        m_navButtons.push_back(b);
    }

    m_scrollBar = new GlScrollBar(m_canvas);
    connect(m_scrollBar, &GlScrollBar::offsetChanged, this, [this](qreal y) {
        m_scrollY = y;
        m_canvas->relayout();
    });

    // Footer: presentation-only preference for how the groups are navigated,
    // plus Close.
    m_layoutCombo = new GlDropdown(m_canvas);
    m_layoutCombo->setText(tr("Settings layout"));
    m_layoutCombo->addItems({tr("Tabs"), tr("Sidebar"), tr("Grouped page")});
    m_layoutCombo->setToolTip(tr("Choose how settings groups are shown. Only the navigation changes, "
                                 "not the settings themselves."));
    m_footerClose = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_footerClose->setText(tr("Close"));
    connect(m_footerClose, &GlButton::clicked, this, &QDialog::close);

    m_layoutCombo->setCurrentIndex(m_settings->settingsLayout());
    connect(m_layoutCombo, &GlDropdown::currentIndexChanged, this, [this](int index) {
        m_settings->setSettingsLayout(index);
        applyNavigationLayout(index);
    });
    applyNavigationLayout(m_layoutCombo->currentIndex());
}

void SettingsDialog::applyNavigationLayout(int layout) {
    // Extension point: add a case in layoutGl() (and an entry in m_layoutCombo)
    // for a new navigation style. Rows are never destroyed, only re-placed.
    m_navMode = qBound(0, layout, 2);
    m_scrollY = 0;
    if (m_canvas) {
        m_canvas->relayout();
    }
}

void SettingsDialog::setPage(int page) {
    m_currentPage = qBound(0, page, int(m_pages.size()) - 1);
    m_scrollY = 0;
    m_canvas->relayout();
}

void SettingsDialog::scrollBy(qreal dy) {
    m_scrollY += dy;
    m_canvas->relayout();
}

void SettingsDialog::layoutGl(const QSizeF& size) {
    if (!m_scrollBar || !m_footerClose) {
        return;
    }
    const qreal W = size.width(), H = size.height();
    const qreal m = 12;

    const QRectF strip(m, m, W - 2 * m, 46);
    m_titlePanel->setRect(strip);
    m_titleLabel->setRect(QRectF(strip.left() + 20, strip.top() + 5, 200, 24));
    m_subtitleLabel->setRect(QRectF(strip.left() + 20, strip.top() + 27, 200, 14));
    m_closeButton->setRect(QRectF(strip.right() - 10 - 36, strip.top() + 8, 36, 30));

    const qreal footerH = 54;
    const QRectF footer(m, H - m - footerH, W - 2 * m, footerH);
    m_layoutCombo->setRect(QRectF(footer.left(), footer.top(), std::min<qreal>(520, footer.width() - 140), footerH));
    m_footerClose->setRect(QRectF(footer.right() - 110, footer.top() + 10, 110, 36));

    const QRectF body(m, strip.bottom() + 10, W - 2 * m, footer.top() - 10 - (strip.bottom() + 10));
    QRectF content = body;
    const bool sidebar = m_navMode == 1, tabs = m_navMode == 0;
    m_navPanel->setVisible(sidebar);
    const int pageCount = int(m_pages.size());
    if (sidebar) {
        const qreal navW = 200;
        m_navPanel->setRect(QRectF(body.left(), body.top(), navW, body.height()));
        content = QRectF(body.left() + navW + 10, body.top(), body.width() - navW - 10, body.height());
        for (int i = 0; i < pageCount; ++i) {
            m_navButtons[i]->setAlignLeft(true);
            m_navButtons[i]->setRect(QRectF(body.left() + 12, body.top() + 22 + i * 48, navW - 24, 40));
        }
    } else if (tabs) {
        const qreal tabH = 40;
        qreal x = body.left();
        for (int i = 0; i < pageCount; ++i) {
            m_navButtons[i]->setAlignLeft(false);
            const qreal w = std::clamp<qreal>(body.width() / pageCount - 6, 90, 190);
            m_navButtons[i]->setRect(QRectF(x, body.top(), w, tabH));
            x += w + 6;
        }
        content = QRectF(body.left(), body.top() + tabH + 10, body.width(), body.height() - tabH - 10);
    }
    for (int i = 0; i < pageCount; ++i) {
        m_navButtons[i]->setVisible(sidebar || tabs);
        m_navButtons[i]->setSelected(i == m_currentPage);
    }
    m_contentPanel->setRect(content);

    // Scrolling viewport inside the content panel.
    const QRectF vp(content.left() + 18, content.top() + 12, content.width() - 36 - 10, content.height() - 24);
    qreal y = vp.top() - m_scrollY;
    const qreal top0 = y;
    for (GlLabel* h : m_headings) {
        h->setVisible(false);
    }
    for (Row& row : m_rows) {
        const bool shown = m_navMode == 2 || row.page == m_currentPage;
        if (!shown) {
            row.a->setVisible(false);
            if (row.b) row.b->setVisible(false);
        }
    }
    int lastPage = -1;
    for (Row& row : m_rows) {
        const bool shown = m_navMode == 2 || row.page == m_currentPage;
        if (!shown) {
            continue;
        }
        if (m_navMode == 2 && row.page != lastPage) {
            lastPage = row.page;
            GlLabel* h = m_headings[row.page];
            const QRectF hr(vp.left(), y, vp.width(), 54);
            h->setRect(hr);
            h->setClip(vp);
            h->setVisible(hr.intersects(vp));
            y += 54;
        }
        const QRectF r(vp.left(), y, vp.width(), row.height);
        const bool vis = r.intersects(vp);
        row.a->setClip(vp);
        row.a->setVisible(vis);
        if (row.type == Row::ButtonRow) {
            row.a->setRect(QRectF(r.left() + 14, r.top(), r.width() - 14 - 190, r.height()));
            row.b->setClip(vp);
            row.b->setVisible(vis);
            row.b->setRect(QRectF(r.right() - 176, r.center().y() - 18, 170, 36));
        } else {
            row.a->setRect(r);
        }
        y += row.height;
    }
    const qreal contentH = (y - top0) + 6;
    const qreal maxScroll = std::max<qreal>(0, contentH - vp.height());
    if (m_scrollY > maxScroll) {
        m_scrollY = maxScroll; // content shrank (e.g. window grew): stay in range
    }
    m_scrollBar->setRect(QRectF(vp.right() + 4, vp.top(), 8, vp.height()));
    m_scrollBar->setMetrics(vp.height(), contentH, m_scrollY);
    m_scrollBar->setVisible(maxScroll > 0);
}

bool SettingsDialog::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG" && m_titlePanel &&
        GlUi::handleFrameHitTest(this, m_canvas, m_titlePanel->rect(), message, result)) {
        return true;
    }
    return QDialog::nativeEvent(eventType, message, result);
}

void SettingsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    GlUi::applyRoundedCorners(this);
}

void SettingsDialog::restoreFromSettings() {
    m_scalingCombo->setCurrentIndex(m_settings->scalingMode());
    m_manager->setScalingMode(static_cast<ScalingMode>(m_settings->scalingMode()));

    int monSel = m_settings->monitorSelection();
    int specific = m_settings->specificMonitorIndex();
    m_monitorCombo->setCurrentIndex(monSel == 2 ? 2 + qMax(0, specific) : monSel);

    m_volumeSlider->setValue(m_settings->volume());
    m_muteCheck->setChecked(m_settings->muted());
    m_loopCheck->setChecked(m_settings->loop());
    m_startWithWindowsCheck->setChecked(m_settings->startWithWindows());
    m_playImagesOnBatteryCheck->setChecked(m_settings->playImagesOnBattery());
    m_manager->setPlayImagesOnBattery(m_settings->playImagesOnBattery());
    m_playVideosOnBatteryCheck->setChecked(m_settings->playVideosOnBattery());
    m_manager->setPlayVideosOnBattery(m_settings->playVideosOnBattery());
    m_showInWindowsSearchCheck->setChecked(m_settings->showInWindowsSearch());
    m_explorerIntegrationCheck->setChecked(m_settings->explorerIntegrationEnabled());
    const bool notifyValues[7] = {m_settings->notifyInApp(),      m_settings->notifyDesktop(),
                                  m_settings->notifySuccess(),    m_settings->notifyError(),
                                  m_settings->notifyWarning(),    m_settings->notifyBackground(),
                                  m_settings->notifyPlayback()};
    for (int i = 0; i < 7; ++i) {
        m_notifyChecks[i]->blockSignals(true);
        m_notifyChecks[i]->setChecked(notifyValues[i]);
        m_notifyChecks[i]->blockSignals(false);
    }

    // Blocked, unlike a normal user pick: this only seeds the combo from
    // the persisted value at startup and must NOT fire onThemeChanged -
    // that kicks off a theme-transition overlay, which must never appear
    // during startup (only on a live, later change). Theme::applyTheme()
    // is already applied separately at startup (see main.cpp) - this
    // combo is just reflecting that, not re-applying it a second time.
    m_themeCombo->blockSignals(true);
    m_themeCombo->setCurrentIndex(static_cast<int>(m_settings->theme()));
    m_themeCombo->blockSignals(false);

    m_manager->setVolume(m_settings->volume());
    m_manager->setMuted(m_settings->muted());
    m_manager->setLooping(m_settings->loop());
}

void SettingsDialog::refreshMonitorList() {
    const int previousIndex = m_monitorCombo->currentIndex();
    m_monitorCombo->blockSignals(true);
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
    if (previousIndex >= 0 && previousIndex < m_monitorCombo->count()) {
        m_monitorCombo->setCurrentIndex(previousIndex);
    }
    m_monitorCombo->blockSignals(false);
}

bool SettingsDialog::isMuted() const {
    return m_muteCheck && m_muteCheck->isChecked();
}

void SettingsDialog::setMuted(bool muted) {
    if (m_muteCheck) {
        m_muteCheck->setChecked(muted);
    }
}

void SettingsDialog::onScalingChanged(int index) {
    m_manager->setScalingMode(static_cast<ScalingMode>(index));
    m_settings->setScalingMode(index);
}

void SettingsDialog::onMonitorSelectionChanged(int index) {
    if (index == 0) {
        m_manager->setMonitorSelection(MonitorSelection::All);
        m_settings->setMonitorSelection(0);
    } else if (index == 1) {
        m_manager->setMonitorSelection(MonitorSelection::Primary);
        m_settings->setMonitorSelection(1);
    } else {
        int specificIndex = index - 2;
        m_manager->setMonitorSelection(MonitorSelection::Specific, specificIndex);
        m_settings->setMonitorSelection(2);
        m_settings->setSpecificMonitorIndex(specificIndex);
    }
}

void SettingsDialog::onVolumeChanged(int value) {
    m_manager->setVolume(value);
    m_settings->setVolume(value);
}

void SettingsDialog::onMuteToggled(bool checked) {
    m_manager->setMuted(checked);
    m_settings->setMuted(checked);
    emit mutedChanged(checked);
}

void SettingsDialog::onLoopToggled(bool checked) {
    m_manager->setLooping(checked);
    m_settings->setLoop(checked);
}

void SettingsDialog::onStartWithWindowsToggled(bool checked) {
    m_settings->setStartWithWindows(checked);
}

void SettingsDialog::onPlayVideosOnBatteryToggled(bool checked) {
    m_settings->setPlayVideosOnBattery(checked);
    // Takes effect immediately - WallpaperManager re-evaluates and
    // hides/restores the media right away if the current power state
    // means this toggle actually changes anything.
    m_manager->setPlayVideosOnBattery(checked);
}

void SettingsDialog::onPlayImagesOnBatteryToggled(bool checked) {
    m_settings->setPlayImagesOnBattery(checked);
    m_manager->setPlayImagesOnBattery(checked);
}

void SettingsDialog::onShowInWindowsSearchToggled(bool checked) {
    // The actual shortcut create/remove happens inside the setter itself
    // (see SettingsManager::setShowInWindowsSearch) - same convention as
    // onStartWithWindowsToggled's registry side effect above.
    m_settings->setShowInWindowsSearch(checked);
}

void SettingsDialog::onExplorerIntegrationToggled(bool checked) {
    // Likewise, the actual Explorer verb register/unregister happens
    // inside the setter (see SettingsManager::setExplorerIntegrationEnabled).
    m_settings->setExplorerIntegrationEnabled(checked);
    emit explorerIntegrationChanged(checked);
}

void SettingsDialog::onThemeChanged(int index) {
    const auto theme = static_cast<Theme::AppTheme>(index);
    // The persisted setting is the user's final selection the instant
    // they pick it in the combo - never an intermediate/transition state,
    // even if this particular apply ends up superseded by a later one
    // below (rapid switching).
    m_settings->setTheme(theme);
    requestThemeTransition([this, theme]() {
        // Live switch, no restart needed - Theme::applyTheme() swaps
        // QApplication's actual QPalette AND its global stylesheet
        // together, both generated from the same literal ThemePalette, so
        // every widget re-polishes off one fully self-consistent theme -
        // never a runtime derivation/inversion of another theme, and
        // never left to Qt's native palette conversion. Icons that pick a
        // light/dark SVG variant (settings gear, wallpaper set/remove)
        // refresh via MainWindow::changeEvent()'s existing
        // QEvent::PaletteChange handling, since QApplication::setPalette()
        // triggers exactly that event on every top-level widget.
        Theme::applyTheme(theme);
        setWindowIcon(QIcon(QStringLiteral(":/settings-icon/%1/settings.svg")
                                 .arg(Theme::iconVariant(theme))));
    });
}

void SettingsDialog::updateBackupRow() {
    m_backupCheck->blockSignals(true);
    m_backupCheck->setChecked(m_backup->isEnabled());
    m_backupCheck->blockSignals(false);
    m_backupStatus->setText(m_backup->statusText());
}

void SettingsDialog::openBackup() {
    if (!m_backupWindow) {
        m_backupWindow = new BackupWindow(m_backup, this);
        connect(m_backupWindow, &BackupWindow::openCleanupRequested, this, &SettingsDialog::openCleanup);
    }
    m_backupWindow->present();
}

void SettingsDialog::openCleanup() {
    if (!m_cleanupWindow) {
        m_cleanupWindow = new CleanupWindow(m_cleanup, this);
    }
    m_cleanupWindow->present();
}

void SettingsDialog::onPreferencesChangedExternally() {
    const Theme::AppTheme theme = m_settings->theme();
    m_settings->loadThemeOverrides();
    Theme::applyTheme(theme);
    setWindowIcon(QIcon(QStringLiteral(":/settings-icon/%1/settings.svg").arg(Theme::iconVariant(theme))));
    restoreFromSettings();
}

void SettingsDialog::requestThemeTransition(std::function<void()> applyFn) {
    // "Ignore intermediate requests, apply the newest" - a rapid run of
    // selections while a transition is already in flight just keeps
    // replacing this, so only the latest ever actually gets applied.
    m_pendingApply = std::move(applyFn);

    if (m_themeTransitionActive) {
        return;
    }
    m_themeTransitionActive = true;

    m_transitionOverlay->beginTransition();
    emit themeTransitionStarted();

    // Deferred to the next event-loop turn so the overlay's first paint
    // actually reaches the screen before the (synchronous) theme apply
    // below runs - otherwise Qt could coalesce both into one repaint and
    // the overlay would never visibly appear before the change lands.
    QTimer::singleShot(0, this, &SettingsDialog::runPendingThemeApply);
}

void SettingsDialog::runPendingThemeApply() {
    // Suppress repaints on both affected top-level windows for the
    // duration of the actual palette/stylesheet swap, so the user never
    // sees it applied widget-by-widget - only the fully-updated result,
    // in one paint, once updates are re-enabled below.
    QWidget* mainWindow = parentWidget();
    if (mainWindow) {
        mainWindow->setUpdatesEnabled(false);
    }
    setUpdatesEnabled(false);

    // A change requested again while this very apply is running (it isn't
    // - the apply itself is synchronous - but a future async apply might
    // be) still only leaves the latest pending change in effect.
    while (m_pendingApply) {
        auto fn = std::move(m_pendingApply);
        m_pendingApply = nullptr;
        fn();
    }

    setUpdatesEnabled(true);
    update();
    if (mainWindow) {
        mainWindow->setUpdatesEnabled(true);
        mainWindow->update();
    }

    m_themeTransitionActive = false;
    m_transitionOverlay->finishTransition();
    emit themeTransitionFinished();
}
