#include "VirtualDesktopWindow.h"

#include "GlUi.h"
#include "PreviewSource.h"
#include "VdDesktopChrome.h"
#include "VdDesktopModel.h"
#include "VdWallpaperSurface.h"

#include <QAbstractNativeEventFilter>
#include <QCloseEvent>
#include <QDebug>
#include <QElapsedTimer>
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QPointer>
#include <QCloseEvent>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QScreen>
#include <QVBoxLayout>
#include <QWindow>

namespace {
constexpr Qt::WindowFlags kFlags = Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
constexpr Qt::WindowFlags kWindowedFlags = Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                                           Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint;
constexpr qreal kMargin = 20;
constexpr qreal kExitW = 170;
constexpr qreal kExitH = 36;

// ---- Opt-in flicker diagnostics: set MOTIVA_VD_DIAG=1. Logs lines tagged [VdDiag]
// to Motiva.log; no cost when unset. Used to correlate a visible flicker with
// native-handle changes, GL context loss, swap gaps, resizes and display events.
class VdDiag : public QObject, public QAbstractNativeEventFilter {
public:
    VdDiag(QWidget* w, QOpenGLWidget* canvas) : QObject(w), m_w(w), m_c(canvas) {
        m_clock.start();
        w->installEventFilter(this);
        canvas->installEventFilter(this);
        qApp->installNativeEventFilter(this);
        connect(canvas, &QOpenGLWidget::frameSwapped, this, [this] {
            const qint64 now = m_clock.elapsed();
            if (m_last && now - m_last > 150) {
                log(QStringLiteral("swap gap %1 ms").arg(now - m_last));
            }
            m_last = now;
            if (m_c->context() && m_ctx != m_c->context()) {
                m_ctx = m_c->context();
                log(QStringLiteral("GL context set %1 valid=%2").arg(quintptr(m_ctx.data()), 0, 16).arg(m_ctx->isValid()));
                connect(m_ctx.data(), &QOpenGLContext::aboutToBeDestroyed, this, [this] { log(QStringLiteral("GL context DESTROYED")); });
            }
        });
        connect(canvas, &QOpenGLWidget::aboutToResize, this, [this] { log(QStringLiteral("GL aboutToResize")); });
        connect(canvas, &QOpenGLWidget::resized, this, [this] { log(QStringLiteral("GL resized")); });
        log(QStringLiteral("attached"));
    }
    ~VdDiag() override { qApp->removeNativeEventFilter(this); }
    void log(const QString& m) const {
        qInfo().noquote() << "[VdDiag]" << QStringLiteral("t=%1").arg(m_clock.elapsed()) << m_w->windowTitle() << m;
    }
    bool eventFilter(QObject* o, QEvent* e) override {
        const char* n = nullptr;
        switch (e->type()) {
        case QEvent::WinIdChange: n = "WinIdChange(native handle)"; break;
        case QEvent::Show: n = "Show"; break;
        case QEvent::Hide: n = "Hide"; break;
        case QEvent::Resize: n = "Resize"; break;
        case QEvent::Move: n = "Move"; break;
        case QEvent::WindowStateChange: n = "WindowStateChange"; break;
        case QEvent::ScreenChangeInternal: n = "ScreenChange"; break;
        case QEvent::ActivationChange: n = "ActivationChange"; break;
        case QEvent::WindowActivate: n = "WindowActivate"; break;
        case QEvent::WindowDeactivate: n = "WindowDeactivate"; break;
        case QEvent::PlatformSurface: n = "PlatformSurface"; break;
        default: break;
        }
        if (n) {
            log(QStringLiteral("%1 on %2").arg(QLatin1String(n), QLatin1String(o->metaObject()->className())));
        }
        return false;
    }
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr*) override {
        if (type != "windows_generic_MSG" || !m_w->windowHandle()) {
            return false;
        }
        struct Msg { void* hwnd; unsigned message; };
        const unsigned id = static_cast<Msg*>(message)->message;
        if (id == 0x7E /*DISPLAYCHANGE*/ || id == 0x31E /*DWMCOMPOSITIONCHANGED*/ || id == 0x2E0 /*DPICHANGED*/ ||
            id == 0x218 /*POWERBROADCAST*/ || id == 0x1A /*SETTINGCHANGE*/ || id == 0x31F /*DWMCOLORIZATION*/) {
            log(QStringLiteral("native msg 0x%1").arg(id, 0, 16));
        }
        return false;
    }

private:
    QWidget* m_w;
    QOpenGLWidget* m_c;
    QPointer<QOpenGLContext> m_ctx;
    QElapsedTimer m_clock;
    qint64 m_last = 0;
};
} // namespace

VirtualDesktopWindow::VirtualDesktopWindow(PreviewSource* source, DesktopScaling::Mode mode, QScreen* screen,
                                           QWidget* parent)
    : QWidget(parent, kFlags), m_source(source) {
    setAttribute(Qt::WA_DeleteOnClose, false); // the session owns and deletes this window
    setWindowTitle(tr("Motiva Virtual Desktop"));
    setFocusPolicy(Qt::StrongFocus);
    if (screen) {
        setScreen(screen);
        setGeometry(VdDesktop::coverRect(screen->geometry()));
    }

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_canvas = new GlCanvas(this);
    // No animated GLSL backdrop: the wallpaper covers it, and Phase 0 showed
    // GL windows are the dominant per-window cost.
    m_canvas->setBackgroundAnimated(false);
    layout->addWidget(m_canvas);
    if (qEnvironmentVariableIsSet("MOTIVA_VD_DIAG")) {
        new VdDiag(this, m_canvas);
    }

    m_surface = new VdWallpaperSurface(m_canvas);
    m_surface->setScalingMode(mode);
    m_surface->setPlaceholderText(tr("No wallpaper loaded"));

    // Simulated icons + taskbar, above the wallpaper and below the controls.
    m_chrome = new VdDesktopChrome(m_canvas);
    m_chrome->setVisible(false);

    // Created after the surface so they paint above it (items paint in creation order).
    m_exitButton = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_exitButton->setText(tr("Exit Preview  (Esc)"));
    m_exitButton->setToolTip(tr("Leave the virtual desktop and return to Motiva"));
    connect(m_exitButton, &GlButton::clicked, this, &QWidget::close);

    m_modeButton = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_modeButton->setText(tr("Windowed  (F11)"));
    m_modeButton->setToolTip(tr("Switch between full screen and a resizable window"));
    connect(m_modeButton, &GlButton::clicked, this, &VirtualDesktopWindow::windowedToggleRequested);

    m_chromeButton = new GlButton(m_canvas, GlButton::Kind::Secondary);
    m_chromeButton->setText(tr("Show Icons  (I)"));
    m_chromeButton->setToolTip(tr("Show simulated desktop icons, taskbar, Start menu and windows over the wallpaper"));
    connect(m_chromeButton, &GlButton::clicked, this, [this] { setChromeVisible(!m_chrome->isVisible()); });

    // First frame actually swapped to the screen: the session hides Motiva's own
    // window only after this, so there is never a gap showing the real desktop.
    connect(m_canvas, &QOpenGLWidget::frameSwapped, this, [this] {
        if (!m_presented) {
            m_presented = true;
            emit firstFramePresented();
        }
    });

    m_canvas->setLayoutCallback([this](const QSizeF& size) { layoutItems(size); });
    m_canvas->relayout();

    if (m_source) {
        connect(m_source, &PreviewSource::frameReady, this, [this](std::shared_ptr<const QImage> frame) {
            m_surface->setFrame(std::move(frame));
        });
        connect(m_source, &PreviewSource::mediaChanged, this, &VirtualDesktopWindow::applyFrameFromSource);
        applyFrameFromSource();
    }
}

void VirtualDesktopWindow::layoutItems(const QSizeF& size) {
    m_surface->setRect(QRectF(QPointF(0, 0), size));
    m_chrome->setRect(QRectF(QPointF(0, 0), size));
    layoutButtons();
}

void VirtualDesktopWindow::layoutButtons() {
    const QSizeF size = m_surface->rect().size();
    const qreal gap = 10;
    qreal x = size.width() - kExitW - kMargin;
    m_exitButton->setRect(QRectF(x, kMargin, kExitW, kExitH));
    x -= kExitW + gap;
    m_modeButton->setRect(QRectF(x, kMargin, kExitW, kExitH));
    x -= kExitW + gap;
    m_chromeButton->setRect(QRectF(x, kMargin, kExitW, kExitH));
}

void VirtualDesktopWindow::setController(bool controller) {
    m_controller = controller;
    m_exitButton->setVisible(controller);
    m_modeButton->setVisible(controller);
    m_chromeButton->setVisible(controller);
    if (!controller) {
        m_chrome->setVisible(false);
    }
    m_canvas->update();
}

void VirtualDesktopWindow::setChromeVisible(bool visible) {
    if (!m_controller) {
        visible = false;
    }
    m_chrome->setVisible(visible);
    m_chromeButton->setText(visible ? tr("Hide Icons  (I)") : tr("Show Icons  (I)"));
    m_canvas->update();
}

void VirtualDesktopWindow::setNote(const QString& text) {
    m_surface->setNote(text);
}

void VirtualDesktopWindow::fitToScreen() {
    if (m_windowed) {
        return;
    }
    if (QScreen* s = screen()) {
        const QRect target = VdDesktop::coverRect(s->geometry());
        if (geometry() != target) {
            setGeometry(target);
        }
    }
}

void VirtualDesktopWindow::setWindowed(bool windowed) {
    if (windowed == m_windowed) {
        return;
    }
    m_windowed = windowed;
    QScreen* scr = screen() ? screen() : QGuiApplication::primaryScreen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1280, 720);
    const Qt::WindowFlags flags = windowed ? kWindowedFlags : kFlags;
    // Restyles the EXISTING native window instead of QWidget::setWindowFlags(),
    // which would hide it, destroy and recreate the HWND and the GL surface, and
    // show a new one (the desktop and a blank frame are visible in between =
    // flicker). QWindow::setFlags() changes the live HWND's style in place
    // (SWP_FRAMECHANGED): the window never hides, the GL context and its
    // textures survive. QWidget::windowFlags() is not updated by it, so
    // m_windowed is the source of truth. A window with no native handle yet has
    // nothing to recreate, so the plain widget call is fine there.
    auto applyFlags = [this, flags] {
        if (QWindow* handle = windowHandle()) {
            handle->setFlags(flags);
        } else {
            setWindowFlags(flags);
        }
    };

    // Order matters: Qt converts a client rect to a native rect with the frame
    // margins of the style the window has WHEN geometry is set, and refreshes them
    // only after the style change is processed (measured: geometry set after the
    // change came out 18x47 px too small). Windowed: set the client rect while
    // frameless, then add the frame (client rect is preserved). Fullscreen: drop
    // the frame first, then set the covering rect.
    if (windowed) {
        if (!m_windowedGeometry.isValid()) {
            const QSize sz(avail.width() * 6 / 10, avail.height() * 6 / 10);
            m_windowedGeometry = QRect(avail.center() - QPoint(sz.width() / 2, sz.height() / 2), sz);
        }
        setGeometry(m_windowedGeometry);
        applyFlags();
        m_modeButton->setText(tr("Fullscreen  (F11)"));
    } else {
        m_windowedGeometry = geometry();
        applyFlags();
        setGeometry(VdDesktop::coverRect(scr ? scr->geometry() : avail));
        m_modeButton->setText(tr("Windowed  (F11)"));
    }
    if (!isVisible()) {
        show(); // only when the window was never shown (first toggle before show)
    }
    raise();
    activateWindow();
    m_canvas->setFocus();
}

void VirtualDesktopWindow::applyFrameFromSource() {
    // Keep the last frame when media changes; a null frame (media unloaded)
    // falls back to the placeholder instead of closing the preview.
    m_surface->setFrame(m_source ? m_source->currentFrame() : nullptr);
}

void VirtualDesktopWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    if (m_controller && event->key() == Qt::Key_F11) {
        emit windowedToggleRequested();
        return;
    }
    if (m_controller && event->key() == Qt::Key_I) {
        setChromeVisible(!m_chrome->isVisible());
        return;
    }
    QWidget::keyPressEvent(event);
}

void VirtualDesktopWindow::closeEvent(QCloseEvent* event) {
    event->accept();
    if (!m_closedEmitted) {
        m_closedEmitted = true;
        emit closed();
    }
}

void VirtualDesktopWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    emit visibilityChanged();
    m_canvas->setFocus();
}

void VirtualDesktopWindow::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    emit visibilityChanged();
}

void VirtualDesktopWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        emit visibilityChanged(); // a minimised windowed preview needn't decode
    }
}
