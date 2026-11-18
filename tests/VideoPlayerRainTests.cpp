#include <QSignalSpy>
#include <QtTest>

#include "VideoPlayer.h"

// Procedural (Binary Rain) lifecycle through the real VideoPlayer. Uses the
// event loop, so counts are bounded rather than exact.
class VideoPlayerRainTests : public QObject {
    Q_OBJECT
    static void pump(int ms) { QTest::qWait(ms); }
private slots:
    void sourceTokenIsRecognised() {
        QVERIFY(VideoPlayer::isProceduralSource(VideoPlayer::binaryRainSource()));
        QVERIFY(!VideoPlayer::isProceduralSource(QStringLiteral("C:/x/video.mp4")));
    }
    void loadPlayPauseUnload() {
        VideoPlayer p;
        QSignalSpy frames(&p, &VideoPlayer::frameReady);
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource()));
        QVERIFY(p.isProcedural());
        QVERIFY(p.currentFrame() && !p.currentFrame()->isNull()); // preview frame exists before play
        QVERIFY(!p.isPlaying());
        const int atLoad = frames.count(); // one preview frame is published on load
        QVERIFY(atLoad <= 1);
        pump(150);
        QCOMPARE(frames.count(), atLoad); // no further frames until play()

        p.play();
        QVERIFY(p.isPlaying());
        pump(400);
        QVERIFY2(frames.count() >= 5, qPrintable(QString::number(frames.count())));

        p.pause();
        QVERIFY(!p.isPlaying());
        const int atPause = frames.count();
        pump(250);
        QCOMPARE(frames.count(), atPause); // no redraws while paused

        p.play();
        pump(200);
        QVERIFY(frames.count() > atPause);

        p.unload();
        QCOMPARE(int(p.mediaKind()), int(VideoPlayer::MediaKind::None));
        const int atUnload = frames.count();
        pump(200);
        QCOMPARE(frames.count(), atUnload); // timer stopped, engine released
    }
    void configChangeWhilePausedRedrawsOnce() {
        VideoPlayer p;
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource()));
        QSignalSpy frames(&p, &VideoPlayer::frameReady);
        BinaryRain::Config c;
        c.setParam(BinaryRain::Param::Glow, 0);
        p.setBinaryRainConfig(c);
        QVERIFY(frames.count() >= 1);
        QCOMPARE(p.binaryRainConfig().preset, BinaryRain::Preset::Custom);
    }
    void invalidConfigIsValidatedBeforeUse() {
        VideoPlayer p;
        BinaryRain::Config c;
        c.preset = BinaryRain::Preset::Custom;
        c.params.density = 99999;
        c.params.charSize = -4;
        p.setBinaryRainConfig(c);
        QCOMPARE(p.binaryRainConfig().params.density, BinaryRain::rangeOf(BinaryRain::Param::Density).max);
        QCOMPARE(p.binaryRainConfig().params.charSize, BinaryRain::rangeOf(BinaryRain::Param::CharSize).min);
    }
    void suspendStopsFramesWithoutChangingPlayState() {
        VideoPlayer p;
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource()));
        p.play();
        QSignalSpy frames(&p, &VideoPlayer::frameReady);
        pump(150);
        QVERIFY(frames.count() > 0);
        p.setProceduralSuspended(true);
        QVERIFY(p.isPlaying()); // user play state untouched
        const int n = frames.count();
        pump(200);
        QCOMPARE(frames.count(), n);
        p.setProceduralSuspended(false);
        pump(200);
        QVERIFY(frames.count() > n);
    }
    void targetSizeFollowsAspectAndCapsResolution() {
        VideoPlayer p;
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource()));
        p.setProceduralTargetSize(QSize(3840, 2160));
        QCOMPARE(p.currentFrame()->size(), QSize(1920, 1080));
        p.setProceduralTargetSize(QSize(1080, 1920));
        QCOMPARE(p.currentFrame()->size(), QSize(1080, 1920));
    }
    void switchingToAnotherSourceReleasesProcedural() {
        VideoPlayer p;
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource()));
        p.play();
        p.loadFile(QStringLiteral("C:/does/not/exist.mp4")); // fails, but must leave rain stopped
        QSignalSpy frames(&p, &VideoPlayer::frameReady);
        pump(200);
        QCOMPARE(frames.count(), 0);
        QVERIFY(!p.isProcedural());
    }
};

QTEST_MAIN(VideoPlayerRainTests)
#include "VideoPlayerRainTests.moc"
