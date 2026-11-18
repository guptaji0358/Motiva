#include "VirtualDesktopSession.h"

#include "PreviewSource.h"
#include "VirtualDesktopWindow.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>

VirtualDesktopSession::VirtualDesktopSession(PreviewSource* source, DesktopScaling::Mode mode, QScreen* screen,
                                             QObject* parent)
    : QObject(parent), m_source(source), m_mode(mode), m_screen(screen) {
    if (m_source) {
        m_source->setParent(this);
    }
    // Application shutdown during a preview: close first, so the independent
    // decoder is released while the event loop is still alive.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &VirtualDesktopSession::end);
}

VirtualDesktopSession::~VirtualDesktopSession() {
    // Window before source: the window holds a QPointer to the source, and an
    // independent source must stop its decoder deterministically.
    m_window.reset();
    delete m_source;
    m_source = nullptr;
}

void VirtualDesktopSession::start() {
    if (m_active || m_finishedEmitted) {
        return;
    }
    QScreen* screen = m_screen ? m_screen.data() : QGuiApplication::primaryScreen();
    m_window = std::make_unique<VirtualDesktopWindow>(m_source, m_mode, screen);
    m_window->setPauseOnBattery(m_pauseOnBattery && m_source && m_source->isIndependent());
    connect(m_window.get(), &VirtualDesktopWindow::closed, this, &VirtualDesktopSession::onWindowClosed);
    m_active = true;
    qInfo() << "[VirtualDesktop] preview started, independentSource="
            << (m_source && m_source->isIndependent()) << "screen=" << (screen ? screen->name() : QString());
    m_window->show();
    m_window->raise();
    m_window->activateWindow();
}

void VirtualDesktopSession::end() {
    if (m_window && m_window->isVisible()) {
        m_window->close(); // -> closed() -> onWindowClosed()
    } else {
        onWindowClosed();
    }
}

void VirtualDesktopSession::onWindowClosed() {
    if (m_finishedEmitted) {
        return;
    }
    m_finishedEmitted = true;
    m_active = false;
    if (m_source) {
        m_source->pausePreview();
    }
    qInfo() << "[VirtualDesktop] preview ended";
    emit finished();
}
