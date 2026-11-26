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
    // The pre-refactor D3DWallpaperRenderer::computeUvTransform formulas, kept
    // here as the reference the shared uvTransform must reproduce.
    static void legacy(Mode mode, QSizeF img, QSizeF w, float s[2], float o[2]) {
        const float iar = float(img.width() / img.height());
        const float war = float(w.width() / w.height());
        switch (mode) {
        case Mode::Stretch: s[0] = s[1] = 1; o[0] = o[1] = 0; return;
        case Mode::Fill:
            if (war > iar) { s[0] = 1; s[1] = iar / war; } else { s[0] = war / iar; s[1] = 1; }
            break;
        case Mode::Fit:
            if (war > iar) { s[0] = war / iar; s[1] = 1; } else { s[0] = 1; s[1] = iar / war; }
            break;
        case Mode::Original:
            s[0] = float(w.width() / img.width()); s[1] = float(w.height() / img.height());
            break;
        }
        o[0] = (1 - s[0]) / 2; o[1] = (1 - s[1]) / 2;
    }
    void uvTransformMatchesLegacyRenderer() {
        const QSizeF images[] = {{1920, 1080}, {1080, 1920}, {3840, 2160}, {500, 500}, {2560, 1080}};
        const QSizeF surfaces[] = {{1920, 1080}, {1080, 1920}, {2560, 1440}, {800, 800}, {3440, 1440}};
        for (Mode m : {Mode::Fill, Mode::Fit, Mode::Stretch, Mode::Original}) {
            for (const QSizeF& img : images) {
                for (const QSizeF& surf : surfaces) {
                    float s1[2], o1[2], s2[2], o2[2];
                    legacy(m, img, surf, s1, o1);
                    DesktopScaling::uvTransform(m, img, surf, s2, o2);
                    for (int i = 0; i < 2; ++i) {
                        QVERIFY2(qAbs(s1[i] - s2[i]) < 1e-4f, "scale differs");
                        QVERIFY2(qAbs(o1[i] - o2[i]) < 1e-4f, "offset differs");
                    }
                }
            }
        }
    }
    void uvTransformDegenerateIsIdentity() {
        float s[2], o[2];
        DesktopScaling::uvTransform(Mode::Fill, QSizeF(0, 0), QSizeF(100, 100), s, o);
        QCOMPARE(s[0], 1.0f); QCOMPARE(s[1], 1.0f); QCOMPARE(o[0], 0.0f); QCOMPARE(o[1], 0.0f);
        DesktopScaling::uvTransform(Mode::Fit, QSizeF(10, 10), QSizeF(0, 0), s, o);
        QCOMPARE(s[0], 1.0f); QCOMPARE(o[1], 0.0f);
    }
    void degenerateInputFallsBackToSurface() {
        QCOMPARE(destRect(Mode::Fill, QSizeF(0, 0), QSizeF(100, 50)), QRectF(0, 0, 100, 50));
        QCOMPARE(destRect(Mode::Fit, QSizeF(10, 10), QSizeF(0, 0)), QRectF(0, 0, 0, 0));
    }
};

QTEST_APPLESS_MAIN(DesktopScalingTests)
#include "DesktopScalingTests.moc"
