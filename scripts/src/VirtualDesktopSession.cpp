#include "VirtualDesktopSession.h"

#include "PreviewSource.h"
#include "VdDesktopModel.h"
#include "VirtualDesktopWindow.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <algorithm>
#include <windows.h>

namespace {
constexpr int kPowerPollMs = 5000;

bool onBattery() {
    SYSTEM_POWER_STATUS status{};
    return GetSystemPowerStatus(&status) && status.ACLineStatus == 0;
}

// C++17 stand-in for std::erase_if on the entries vector. The vector is in flux
// while windows are destroyed, so playback decisions are suppressed meanwhile.
template <class Vec, class Pred>
void eraseIf(Vec& v, Pred pred) {
    v.erase(std::remove_if(v.begin(), v.end(), pred), v.end());
}
} // namespace

VirtualDesktopSession::VirtualDesktopSession(PreviewSource* source, DesktopScaling::Mode mode, QScreen* screen,
                                             QObject* parent)
    : QObject(parent), m_source(source), m_mode(mode), m_screen(screen) {
    if (m_source) {
        m_source->setParent(this);
    }
    m_powerTimer = new QTimer(this);
    m_powerTimer->setInterval(kPowerPollMs);
    connect(m_powerTimer, &QTimer::timeout, this, &VirtualDesktopSession::applyPlayState);
    // Application shutdown during a preview: close first, so the independent
    // decoder is released while the event loop is still alive.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &VirtualDesktopSession::end);
}

VirtualDesktopSession::~VirtualDesktopSession() {
    // Windows before source: they hold a QPointer to the source, and an
    // independent source must stop its decoder deterministically.
    m_mutating = true;
    m_entries.clear();
    delete m_source;
    m_source = nullptr;
}

void VirtualDesktopSession::start() {
    if (m_active || m_finishedEmitted) {
        return;
    }
    QScreen* start = m_screen ? m_screen.data() : QGuiApplication::primaryScreen();
    m_controllerScreen = start;
    m_active = true;

    for (QScreen* s : QGuiApplication::screens()) {
        addWindowFor(s);
    }
    if (m_entries.empty() && start) {
        addWindowFor(start); // defensive: screens() should never be empty
    }
    ensureController();

    connect(qApp, &QGuiApplication::screenAdded, this, &VirtualDesktopSession::scheduleScreenSync);
    connect(qApp, &QGuiApplication::screenRemoved, this, &VirtualDesktopSession::scheduleScreenSync);

    qInfo() << "[VirtualDesktop] preview started, independentSource=" << (m_source && m_source->isIndependent())
            << "monitors=" << m_entries.size() << "controller=" << (start ? start->name() : QString());
    for (Entry& e : m_entries) {
        e.window->show();
        e.window->raise();
    }
    if (Entry* c = controllerEntry()) {
        c->window->activateWindow();
    }
    m_powerTimer->start();
    applyPlayState();
}

void VirtualDesktopSession::addWindowFor(QScreen* screen) {
    if (!screen) {
        return;
    }
    Entry e;
    e.screen = screen;
    e.window = std::make_unique<VirtualDesktopWindow>(m_source, m_mode, screen);
    e.window->setController(false);
    VirtualDesktopWindow* w = e.window.get();
    connect(w, &VirtualDesktopWindow::closed, this, &VirtualDesktopSession::onWindowClosed);
    connect(w, &VirtualDesktopWindow::windowedToggleRequested, this, &VirtualDesktopSession::toggleWindowed);
    connect(w, &VirtualDesktopWindow::visibilityChanged, this, &VirtualDesktopSession::applyPlayState);
    connect(w, &VirtualDesktopWindow::firstFramePresented, this, &VirtualDesktopSession::maybeEmitFirstFrame);
    // Resolution / arrangement change on a live monitor: refit its window.
    connect(screen, &QScreen::geometryChanged, w, [w] { w->fitToScreen(); });
    m_entries.push_back(std::move(e));
}

VirtualDesktopSession::Entry* VirtualDesktopSession::controllerEntry() {
    for (Entry& e : m_entries) {
        if (e.window && e.window->isController()) {
            return &e;
        }
    }
    return nullptr;
}

// Exactly one controller: the window on the starting screen while it exists,
// else the primary screen's, else the first.
void VirtualDesktopSession::ensureController() {
    Entry* chosen = nullptr;
    for (Entry& e : m_entries) {
        if (e.screen && e.screen == m_controllerScreen) {
            chosen = &e;
            break;
        }
    }
    if (!chosen) {
        for (Entry& e : m_entries) {
            if (e.screen && e.screen == QGuiApplication::primaryScreen()) {
                chosen = &e;
                break;
            }
        }
    }
    if (!chosen && !m_entries.empty()) {
        chosen = &m_entries.front();
    }
    if (!chosen) {
        return;
    }
    m_controllerScreen = chosen->screen;
    for (Entry& e : m_entries) {
        e.window->setController(&e == chosen);
    }
    maybeEmitFirstFrame(); // the new controller may already have presented
}

void VirtualDesktopSession::maybeEmitFirstFrame() {
    if (m_firstFrameEmitted || !m_active || m_finishedEmitted) {
        return;
    }
    Entry* c = controllerEntry();
    if (c && c->window && c->window->hasPresented()) {
        m_firstFrameEmitted = true;
        emit firstFramePresented();
    }
}

void VirtualDesktopSession::scheduleScreenSync() {
    if (m_syncQueued || !m_active || m_finishedEmitted) {
        return;
    }
    m_syncQueued = true;
    // Deferred: a screenRemoved handler must not delete windows mid-signal,
    // and a plug/unplug often fires several signals in a row.
    QTimer::singleShot(0, this, &VirtualDesktopSession::syncScreens);
}

void VirtualDesktopSession::syncScreens() {
    m_syncQueued = false;
    if (!m_active || m_finishedEmitted) {
        return;
    }
    // Entries whose QScreen is already gone have no name; drop them first.
    m_mutating = true;
    eraseIf(m_entries, [](const Entry& e) { return e.screen.isNull(); });
    m_mutating = false;

    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return;
    }

    if (m_windowed) {
        // Only the controller window exists on screen. If its monitor went away,
        // Qt already moved it; just make sure it is still the controller.
        ensureController();
        applyPlayState();
        return;
    }

    QStringList existing;
    for (const Entry& e : m_entries) {
        existing << e.screen->name();
    }
    QStringList wanted;
    for (QScreen* s : screens) {
        wanted << s->name();
    }
    const VdDesktop::ScreenPlan plan = VdDesktop::planScreenSync(existing, wanted);

    if (!plan.remove.isEmpty()) {
        m_mutating = true;
        eraseIf(m_entries, [&plan](const Entry& e) { return plan.remove.contains(e.screen->name()); });
        m_mutating = false;
    }
    for (QScreen* s : screens) {
        if (plan.add.contains(s->name())) {
            addWindowFor(s);
            m_entries.back().window->show();
            m_entries.back().window->raise();
        }
    }
    ensureController();
    for (Entry& e : m_entries) {
        e.window->fitToScreen();
    }
    qInfo() << "[VirtualDesktop] monitors changed: added" << plan.add << "removed" << plan.remove
            << "now" << m_entries.size();
    applyPlayState();
}

void VirtualDesktopSession::toggleWindowed() {
    Entry* c = controllerEntry();
    if (!c || !m_active || m_finishedEmitted) {
        return;
    }
    m_windowed = !m_windowed;
    if (m_windowed) {
        // Windowed shows only the controller; the other monitors' windows go.
        m_mutating = true;
        eraseIf(m_entries, [](const Entry& e) { return !(e.window && e.window->isController()); });
        m_mutating = false;
        c = controllerEntry();
        if (c) {
            c->window->setWindowed(true);
        }
    } else {
        if (c) {
            c->window->setWindowed(false);
        }
        syncScreens(); // brings the other monitors' windows back
        if (Entry* cc = controllerEntry()) {
            cc->window->activateWindow();
        }
    }
    applyPlayState();
}

// One place decides whether an independent source decodes: not while on
// battery with the policy off, not while no preview window is actually showing.
// The active-wallpaper source ignores pause/resume (WallpaperManager owns it).
void VirtualDesktopSession::applyPlayState() {
    if (!m_source || !m_active || m_finishedEmitted || m_mutating) {
        return;
    }
    bool anyShowing = false;
    for (const Entry& e : m_entries) {
        anyShowing = anyShowing || (e.window && e.window->isShowing());
    }
    const bool battery = m_pauseOnBattery && m_source->isIndependent() && onBattery();
    const bool pause = VdDesktop::shouldPausePlayback(m_pauseOnBattery && m_source->isIndependent(), battery, anyShowing);
    if (Entry* c = controllerEntry()) {
        c->window->setNote(battery ? tr("Paused on battery") : QString());
    }
    if (pause) {
        m_source->pausePreview();
    } else {
        m_source->resumePreview();
    }
}

void VirtualDesktopSession::end() {
    if (m_finishedEmitted) {
        return;
    }
    bool closedAny = false;
    for (Entry& e : m_entries) {
        if (e.window && e.window->isVisible()) {
            e.window->close(); // -> closed() -> onWindowClosed()
            closedAny = true;
            break;
        }
    }
    if (!closedAny) {
        onWindowClosed();
    }
}

void VirtualDesktopSession::onWindowClosed() {
    if (m_finishedEmitted) {
        return;
    }
    m_finishedEmitted = true;
    m_active = false;
    m_powerTimer->stop();
    // One window closing (Esc, Exit, title-bar X) ends the whole preview.
    for (Entry& e : m_entries) {
        if (e.window && e.window->isVisible()) {
            e.window->close();
        }
    }
    if (m_source) {
        m_source->pausePreview();
    }
    qInfo() << "[VirtualDesktop] preview ended";
    emit finished();
}
