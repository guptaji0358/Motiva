#include "DesktopScaling.h"

#include <QtTest>

using DesktopScaling::Mode;
using DesktopScaling::destRect;

class DesktopScalingTests : public QObject {
    Q_OBJECT
private slots:
    void fillCoversWideSurface() {
        // 16:9 source into a 2:1 surface: width matches, height overflows, centered.
        const QRectF r = destRect(Mode::Fill, QSizeF(1920, 1080), QSizeF(2000, 1000));
        QCOMPARE(r.width(), 2000.0);
        QVERIFY(r.height() > 1000.0);
        QCOMPARE(r.center(), QPointF(1000, 500));
    }
    void fillCoversTallSurface() {
        const QRectF r = destRect(Mode::Fill, QSizeF(1920, 1080), QSizeF(1000, 1000));
        QCOMPARE(r.height(), 1000.0);
        QVERIFY(r.width() > 1000.0);
        QCOMPARE(r.center(), QPointF(500, 500));
    }
    void fitLetterboxes() {
        const QRectF r = destRect(Mode::Fit, QSizeF(1920, 1080), QSizeF(1000, 1000));
        QCOMPARE(r.width(), 1000.0);
        QVERIFY(r.height() < 1000.0);
        QCOMPARE(r.center(), QPointF(500, 500));
        QVERIFY(QRectF(0, 0, 1000, 1000).contains(r));
    }
    void stretchFillsSurface() {
        QCOMPARE(destRect(Mode::Stretch, QSizeF(1920, 1080), QSizeF(800, 600)), QRectF(0, 0, 800, 600));
    }
    void originalKeepsNativeSizeCentered() {
        const QRectF r = destRect(Mode::Original, QSizeF(400, 200), QSizeF(1000, 1000));
        QCOMPARE(r.size(), QSizeF(400, 200));
        QCOMPARE(r.center(), QPointF(500, 500));
    }
    void sameAspectIsIdentityForFillAndFit() {
        QCOMPARE(destRect(Mode::Fill, QSizeF(1920, 1080), QSizeF(960, 540)), QRectF(0, 0, 960, 540));
        QCOMPARE(destRect(Mode::Fit, QSizeF(1920, 1080), QSizeF(960, 540)), QRectF(0, 0, 960, 540));
    }
    void degenerateInputFallsBackToSurface() {
        QCOMPARE(destRect(Mode::Fill, QSizeF(0, 0), QSizeF(100, 50)), QRectF(0, 0, 100, 50));
        QCOMPARE(destRect(Mode::Fit, QSizeF(10, 10), QSizeF(0, 0)), QRectF(0, 0, 0, 0));
    }
};

QTEST_APPLESS_MAIN(DesktopScalingTests)
#include "DesktopScalingTests.moc"
