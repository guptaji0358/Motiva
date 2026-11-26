#include <QMediaFormat>
#include <QSignalSpy>
#include <QtTest>

#include <QAudioOutput>
#include <QFutureWatcher>
#include <QMediaPlayer>
#include <QMovie>
#include <QTimer>
#include <QVideoSink>
// Test-only: the tests queue a QMediaPlayer error from "an earlier load" into the player's event queue.
#define private public
#include "VideoPlayer.h"
#undef private

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
    // --- video metadata hook (pure normalisation; no media needed) ---
    void metadata_availableValuesAreKept() {
        QMediaMetaData m;
        m.insert(QMediaMetaData::Resolution, QSize(1920, 1080));
        m.insert(QMediaMetaData::VideoFrameRate, 29.97);
        m.insert(QMediaMetaData::VideoCodec, QVariant::fromValue(QMediaFormat::VideoCodec::H264));
        const auto d = VideoPlayer::describeMetadata(m);
        QCOMPARE(d.width, 1920);
        QCOMPARE(d.height, 1080);
        QVERIFY(qAbs(d.fps - 29.97) < 0.001);
        QCOMPARE(d.codec, QStringLiteral("h264"));
    }
    void metadata_missingBecomesUnknown() {
        const auto d = VideoPlayer::describeMetadata(QMediaMetaData());
        QCOMPARE(d.width, 0);
        QCOMPARE(d.height, 0);
        QCOMPARE(d.fps, 0.0);
        QCOMPARE(d.codec, QStringLiteral("unknown"));
    }
    void metadata_invalidValuesAreRejected() {
        QMediaMetaData m;
        m.insert(QMediaMetaData::Resolution, QSize(-5, 0));
        m.insert(QMediaMetaData::VideoFrameRate, std::numeric_limits<double>::quiet_NaN());
        m.insert(QMediaMetaData::VideoCodec, 9999); // not a known codec
        auto d = VideoPlayer::describeMetadata(m);
        QCOMPARE(d.width, 0);
        QCOMPARE(d.height, 0);
        QCOMPARE(d.fps, 0.0);
        QCOMPARE(d.codec, QStringLiteral("unknown"));
        m.insert(QMediaMetaData::Resolution, QSize(99999, 1080));   // absurd size
        m.insert(QMediaMetaData::VideoFrameRate, -30.0);            // negative
        d = VideoPlayer::describeMetadata(m);
        QCOMPARE(d.width, 0);
        QCOMPARE(d.fps, 0.0);
        m.insert(QMediaMetaData::VideoFrameRate, 5000.0);           // implausible
        QCOMPARE(VideoPlayer::describeMetadata(m).fps, 0.0);
    }
    void metadata_signalNotEmittedForStillsAndProcedural() {
        VideoPlayer p;
        QSignalSpy spy(&p, &VideoPlayer::videoMetadataAvailable);
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource()));
        pump(150);
        QCOMPARE(spy.count(), 0);
    }

    void loadSeq_incrementsPerLoadIncludingFailures() {
        VideoPlayer p;
        QCOMPARE(p.loadSeq(), quint64(0));
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource()));
        QCOMPARE(p.loadSeq(), quint64(1));
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource(), VideoPlayer::LoadOrigin::Playlist));
        QCOMPARE(p.loadSeq(), quint64(2));
        QVERIFY(!p.loadFile(QStringLiteral("C:/does/not/exist.mp4")));
        QCOMPARE(p.loadSeq(), quint64(3));
    }
    void loadError_isStampedWithItsOwnLoadSeq() {
        VideoPlayer p;
        QSignalSpy spy(&p, &VideoPlayer::loadError);
        QSignalSpy plain(&p, &VideoPlayer::errorOccurred);
        QVERIFY(!p.loadFile(QStringLiteral("C:/does/not/exist.mp4")));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toULongLong(), quint64(1)); // the load that failed
        QVERIFY(!p.loadFile(QStringLiteral("C:/does/not/exist2.mp4")));
        QCOMPARE(spy.last().at(0).toULongLong(), quint64(2)); // a newer load carries a newer stamp
        QCOMPARE(plain.count(), spy.count()); // UI-facing signal unchanged
    }
    // A QMediaPlayer error that was queued before the latest loadFile() belongs to the earlier load.
    static void queuePlayerError(VideoPlayer& p, const QString& msg) {
        QMetaObject::invokeMethod(&p.m_player, [&p, msg] { emit p.m_player.errorOccurred(QMediaPlayer::ResourceError, msg); },
                                  Qt::QueuedConnection);
    }
    void staleQueuedError_doesNotReachLaterLoad() {
        VideoPlayer p;
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource())); // load 1
        pump(30);
        queuePlayerError(p, "old load failed");                // queued, not yet delivered
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource())); // load 2 starts before delivery
        QSignalSpy stamped(&p, &VideoPlayer::loadError);
        QSignalSpy ui(&p, &VideoPlayer::errorOccurred);
        pump(60);
        QCOMPARE(stamped.count(), 1);
        QVERIFY(stamped.at(0).at(0).toULongLong() != p.loadSeq()); // never stamped with load 2
        QCOMPARE(stamped.at(0).at(0).toULongLong(), quint64(1));
        QCOMPARE(ui.count(), 0); // UI / browser request never hears about it
    }
    void queuedErrorAfterItsOwnLoadIsReported() {
        VideoPlayer p;
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource())); // load 1
        pump(30);
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource())); // load 2
        queuePlayerError(p, "load 2 failed");                  // raised after load 2 began
        QSignalSpy stamped(&p, &VideoPlayer::loadError);
        QSignalSpy ui(&p, &VideoPlayer::errorOccurred);
        pump(60);
        QCOMPARE(stamped.count(), 1);
        QCOMPARE(stamped.at(0).at(0).toULongLong(), quint64(2));
        QCOMPARE(ui.count(), 1);
    }
    void staleErrorAcrossBackToBackLoadsStaysStale() {
        VideoPlayer p;
        queuePlayerError(p, "load 0 failed"); // before any load: no fence reached yet
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource()));
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource())); // no event-loop turn between loads
        QSignalSpy ui(&p, &VideoPlayer::errorOccurred);
        pump(60);
        QCOMPARE(ui.count(), 0);
    }
    // F2: an error QMediaPlayer raises synchronously inside OUR call (here setLoops via loopsChanged) after
    // loadFile() returned, with the fence not yet delivered, belongs to the current load and must be reported.
    void synchronousErrorInsideOwnCallIsReportedForCurrentLoad() {
        VideoPlayer p;
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource())); // load 1; no event-loop turn: fence undelivered
        QVERIFY(p.m_fenceSeq < p.m_loadSeq);
        QObject::connect(&p.m_player, &QMediaPlayer::loopsChanged, &p, [&p] {
            emit p.m_player.errorOccurred(QMediaPlayer::ResourceError, QStringLiteral("sync error"));
        });
        QSignalSpy stamped(&p, &VideoPlayer::loadError);
        QSignalSpy ui(&p, &VideoPlayer::errorOccurred);
        p.setLooping(false);
        p.setLooping(true); // one of these changes the loop count -> synchronous emission inside applyLoopMode()
        QVERIFY(stamped.count() >= 1);
        QCOMPARE(stamped.at(0).at(0).toULongLong(), p.loadSeq());
        QCOMPARE(ui.count(), stamped.count());
        QCOMPARE(p.m_syncCallDepth, 0); // scope always unwinds
    }
    void sameEmissionFromTheEventLoopBeforeTheFenceStaysStale() { // negative control for the test above
        VideoPlayer p;
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource()));
        queuePlayerError(p, "queued before the latest load");
        QVERIFY(p.loadFile(VideoPlayer::binaryRainSource()));
        QSignalSpy ui(&p, &VideoPlayer::errorOccurred);
        pump(60);
        QCOMPARE(ui.count(), 0);
        QCOMPARE(p.m_syncCallDepth, 0);
    }
    void origin_tokensAreAllowlisted() {
        QCOMPARE(VideoPlayer::originToken(VideoPlayer::LoadOrigin::StartupPreview), QString("startup_preview"));
        QCOMPARE(VideoPlayer::originToken(VideoPlayer::LoadOrigin::UserSelection), QString("user_selection"));
        QCOMPARE(VideoPlayer::originToken(VideoPlayer::LoadOrigin::WallpaperApply), QString("wallpaper_apply"));
        QCOMPARE(VideoPlayer::originToken(VideoPlayer::LoadOrigin::Playlist), QString("playlist"));
    }
    void isVideoSource_routing() {
        QVERIFY(!VideoPlayer::isVideoSource(VideoPlayer::binaryRainSource()));
        QVERIFY(!VideoPlayer::isVideoSource(QStringLiteral("C:/x/a.gif")));
        QVERIFY(!VideoPlayer::isVideoSource(QStringLiteral("C:/x/a.png")));
        QVERIFY(VideoPlayer::isVideoSource(QStringLiteral("C:/x/a.mp4")));
        QVERIFY(VideoPlayer::isVideoSource(QStringLiteral("https://example.com/v")));
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
