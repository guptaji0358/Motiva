// GUI regression test for the Virtual Desktop flicker fixes. Needs a real
// desktop session with OpenGL (skips itself otherwise). It drives the real
// VirtualDesktopWindow:
//  - F11 (setWindowed) must restyle the live native window: no hide/show, no
//    native-handle change, same GL context, exact client geometry both ways.
//  - firstFramePresented is emitted exactly once, after a frame is swapped.
//  - the fullscreen window covers its monitor but is not exactly its size.
#include "GlUi.h"
#include "PreviewSource.h"
#include "VdDesktopModel.h"
#include "VirtualDesktopWindow.h"

#include <QApplication>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QScreen>
#include <QSignalSpy>
#include <QWindow>
#include <QtTest>

namespace {
class StaticSource : public PreviewSource {
public:
    StaticSource() {
        auto* image = new QImage(640, 360, QImage::Format_ARGB32_Premultiplied);
        image->fill(QColor(255, 0, 200));
        m_frame.reset(image);
    }
    std::shared_ptr<const QImage> currentFrame() const override { return m_frame; }
    bool isIndependent() const override { return true; }
    bool isValid() const override { return true; }

private:
    std::shared_ptr<const QImage> m_frame;
};

// Counts events that mean "the native window went away or was replaced".
class LifecycleCounter : public QObject {
public:
    int hides = 0, shows = 0, winIdChanges = 0;
    bool eventFilter(QObject*, QEvent* e) override {
        switch (e->type()) {
        case QEvent::Hide: ++hides; break;
        case QEvent::Show: ++shows; break;
        case QEvent::WinIdChange: ++winIdChanges; break;
        default: break;
        }
        return false;
    }
};
} // namespace

class VdWindowTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        if (QGuiApplication::platformName() == QLatin1String("offscreen") || !QGuiApplication::primaryScreen()) {
            QSKIP("needs a real desktop session");
        }
    }

    void fullscreenCoversMonitorButIsNotItsExactSize() {
        StaticSource source;
        QScreen* screen = QGuiApplication::primaryScreen();
        VirtualDesktopWindow w(&source, DesktopScaling::Mode::Fill, screen);
        QVERIFY(w.geometry().contains(screen->geometry()));
        QVERIFY(w.geometry() != screen->geometry());
        QCOMPARE(w.geometry(), VdDesktop::coverRect(screen->geometry()));
    }

    void firstFramePresentedIsEmittedOnceAfterASwap() {
        StaticSource source;
        VirtualDesktopWindow w(&source, DesktopScaling::Mode::Fill, QGuiApplication::primaryScreen());
        QSignalSpy spy(&w, &VirtualDesktopWindow::firstFramePresented);
        QVERIFY(!w.hasPresented());
        w.show();
        if (!spy.wait(8000)) {
            QSKIP("no frame was presented (no usable OpenGL here)");
        }
        QVERIFY(w.hasPresented());
        QTest::qWait(300); // more frames follow; the signal must not repeat
        QCOMPARE(spy.count(), 1);
    }

    void f11TogglesRestyleTheLiveWindowWithoutRecreatingIt() {
        StaticSource source;
        QScreen* screen = QGuiApplication::primaryScreen();
        VirtualDesktopWindow w(&source, DesktopScaling::Mode::Fill, screen);
        QSignalSpy presented(&w, &VirtualDesktopWindow::firstFramePresented);
        w.show();
        if (!presented.wait(8000)) {
            QSKIP("no frame was presented (no usable OpenGL here)");
        }
        auto* canvas = w.findChild<QOpenGLWidget*>();
        QVERIFY(canvas);
        QOpenGLContext* ctxBefore = canvas->context();
        QVERIFY(ctxBefore);
        const WId idBefore = w.winId();

        LifecycleCounter counter;
        w.installEventFilter(&counter);

        const QRect cover = VdDesktop::coverRect(screen->geometry());
        QSize windowedSize;
        for (int round = 0; round < 3; ++round) {
            w.setWindowed(true);
            QTest::qWait(150);
            QVERIFY(w.isWindowed());
            QVERIFY(w.isVisible());
            QVERIFY(w.geometry() != cover);
            if (round == 0) {
                windowedSize = w.size();
            } else {
                // The remembered windowed rect comes back exactly (not shrunk by
                // stale frame margins after the in-place style change).
                QCOMPARE(w.size(), windowedSize);
            }

            w.setWindowed(false);
            QTest::qWait(150);
            QVERIFY(!w.isWindowed());
            QVERIFY(w.isVisible());
            QCOMPARE(w.geometry(), cover);
        }

        QCOMPARE(counter.hides, 0);
        QCOMPARE(counter.shows, 0);
        QCOMPARE(counter.winIdChanges, 0);
        QCOMPARE(w.winId(), idBefore);
        QCOMPARE(canvas->context(), ctxBefore);
        QVERIFY(ctxBefore->isValid());
    }
};

QTEST_MAIN(VdWindowTests)
#include "VdWindowTests.moc"
