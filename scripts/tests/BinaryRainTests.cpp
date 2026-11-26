#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include "BinaryRainConfig.h"
#include "BinaryRainEngine.h"

using namespace BinaryRain;

class BinaryRainTests : public QObject {
    Q_OBJECT
private slots:
    // ---- presets ----
    void presetsAreDistinctAndInRange() {
        QSet<QString> seen;
        for (int i = 0; i < kPresetCount - 1; ++i) {
            const Params p = presetParams(static_cast<Preset>(i));
            QCOMPARE(validated(p), p); // table values already valid
            QVERIFY(!seen.contains(presetName(static_cast<Preset>(i))));
            seen.insert(presetName(static_cast<Preset>(i)));
        }
        QCOMPARE(seen.size(), 5);
    }
    void presetCharacter() {
        const Params classic = presetParams(Preset::Classic), dense = presetParams(Preset::Dense),
                     minimal = presetParams(Preset::Minimal), neon = presetParams(Preset::Neon),
                     dark = presetParams(Preset::Dark);
        QVERIFY(dense.density > classic.density);
        QVERIFY(minimal.density < classic.density && minimal.brightness < classic.brightness &&
                minimal.glow < classic.glow);
        QVERIFY(neon.brightness > classic.brightness && neon.glow > classic.glow);
        QVERIFY(dark.backgroundDarkness > classic.backgroundDarkness && dark.brightness < classic.brightness);
    }
    void presetKeysRoundTrip() {
        for (int i = 0; i < kPresetCount; ++i) {
            Preset out = Preset::Classic;
            QVERIFY(presetFromKey(QLatin1String(presetKey(static_cast<Preset>(i))), &out));
            QCOMPARE(int(out), i);
        }
        Preset out;
        QVERIFY(!presetFromKey(QStringLiteral("bogus"), &out));
    }

    // ---- validation ----
    void clampsEveryParam() {
        for (int i = 0; i < kParamCount; ++i) {
            const Param p = static_cast<Param>(i);
            QCOMPARE(clampParam(p, -100000), rangeOf(p).min);
            QCOMPARE(clampParam(p, 100000), rangeOf(p).max);
        }
        Params bad;
        bad.density = -5; bad.speed = 9999; bad.charSize = 0; bad.brightness = 500; bad.glow = -1;
        bad.backgroundDarkness = 101;
        const Params ok = validated(bad);
        QCOMPARE(ok.density, rangeOf(Param::Density).min);
        QCOMPARE(ok.speed, rangeOf(Param::Speed).max);
        QCOMPARE(ok.charSize, rangeOf(Param::CharSize).min);
        QCOMPARE(ok.brightness, 100);
        QCOMPARE(ok.glow, 0);
        QCOMPARE(ok.backgroundDarkness, 100);
    }

    // ---- switching / Custom ----
    void switchingPresetAppliesFullConfig() {
        Config c;
        c.applyPreset(Preset::Neon);
        QCOMPARE(c.preset, Preset::Neon);
        QCOMPARE(c.params, presetParams(Preset::Neon));
        c.applyPreset(Preset::Dark);
        QCOMPARE(c.params, presetParams(Preset::Dark));
    }
    void editingBecomesCustom() {
        Config c;
        c.applyPreset(Preset::Dense);
        QVERIFY(c.setParam(Param::Glow, 5));
        QCOMPARE(c.preset, Preset::Custom);
        QCOMPARE(c.params.glow, 5);
        QCOMPARE(c.params.density, presetParams(Preset::Dense).density); // rest preserved
    }
    void unchangedEditKeepsPreset() {
        Config c;
        QVERIFY(!c.setParam(Param::Speed, c.params.speed));
        QCOMPARE(c.preset, Preset::Classic);
    }
    void outOfRangeEditIsClamped() {
        Config c;
        QVERIFY(c.setParam(Param::Speed, 100000));
        QCOMPARE(c.params.speed, rangeOf(Param::Speed).max);
    }
    void customKeepsValuesWhenPresetCustomApplied() {
        Config c;
        c.setParam(Param::Density, 33);
        c.applyPreset(Preset::Custom);
        QCOMPARE(c.params.density, 33);
        QCOMPARE(c.preset, Preset::Custom);
    }
    void resetGoesToClassic() {
        Config c;
        c.setParam(Param::Density, 33);
        c.resetToDefaults();
        QCOMPARE(c.preset, Preset::Classic);
        QCOMPARE(c.params, presetParams(Preset::Classic));
    }

    // ---- persistence (QSettings INI; SettingsManager delegates to these) ----
    void persistenceRoundTrip() {
        QTemporaryDir dir;
        const QString file = dir.filePath("s.ini");
        Config custom;
        custom.setParam(Param::Density, 42);
        custom.setParam(Param::CharSize, 30);
        Config neon;
        neon.applyPreset(Preset::Neon);
        {
            QSettings s(file, QSettings::IniFormat);
            custom.toSettings(s);
            s.sync();
        }
        {
            QSettings s(file, QSettings::IniFormat);
            const Config back = Config::fromSettings(s);
            QCOMPARE(back.preset, Preset::Custom);
            QCOMPARE(back.params, custom.params);
            neon.toSettings(s);
            s.sync();
        }
        QSettings s(file, QSettings::IniFormat);
        const Config back = Config::fromSettings(s);
        QCOMPARE(back.preset, Preset::Neon);
        QCOMPARE(back.params, neon.params);
    }
    void loadingEmptyOrCorruptSettingsIsSafe() {
        QTemporaryDir dir;
        QSettings s(dir.filePath("c.ini"), QSettings::IniFormat);
        QCOMPARE(Config::fromSettings(s).preset, Preset::Classic); // nothing stored
        s.setValue("binaryRain/preset", "custom");
        s.setValue("binaryRain/density", 99999);
        s.setValue("binaryRain/speed", "abc");
        const Config c = Config::fromSettings(s);
        QCOMPARE(c.params.density, rangeOf(Param::Density).max);
        QCOMPARE(c.params.speed, presetParams(Preset::Classic).speed);
        s.setValue("binaryRain/preset", "nonsense");
        QCOMPARE(Config::fromSettings(s).preset, Preset::Classic);
    }
    void namedPresetLoadsTableValues() {
        QTemporaryDir dir;
        QSettings s(dir.filePath("n.ini"), QSettings::IniFormat);
        s.setValue("binaryRain/preset", "dense");
        s.setValue("binaryRain/density", 7); // stale value must not survive a named preset
        QCOMPARE(Config::fromSettings(s).params, presetParams(Preset::Dense));
    }

    // ---- engine ----
    void engineIsDeterministicForSeed() {
        BinaryRainEngine a, b;
        a.resize({320, 180}); b.resize({320, 180});
        a.seed(7); b.seed(7);
        QImage ia(320, 180, QImage::Format_RGB32), ib(320, 180, QImage::Format_RGB32);
        for (int i = 0; i < 30; ++i) { a.advance(1 / 30.0); b.advance(1 / 30.0); }
        QVERIFY(a.render(ia) && b.render(ib));
        QCOMPARE(ia, ib);
    }
    void engineDrawsGreenOnDarkAndAnimates() {
        BinaryRainEngine e;
        e.resize({480, 270});
        e.seed(1);
        QImage f1(480, 270, QImage::Format_RGB32), f2(480, 270, QImage::Format_RGB32);
        QVERIFY(e.render(f1));
        for (int i = 0; i < 15; ++i) e.advance(1 / 30.0);
        QVERIFY(e.render(f2));
        QVERIFY(f1 != f2);
        qint64 r = 0, g = 0, b = 0;
        for (int y = 0; y < f2.height(); ++y)
            for (int x = 0; x < f2.width(); ++x) {
                const QRgb p = f2.pixel(x, y);
                r += qRed(p); g += qGreen(p); b += qBlue(p);
            }
        QVERIFY(g > r * 2 && g > b * 2); // predominantly green
        QVERIFY(qGreen(f2.pixel(0, 0)) < 40); // dark background
    }
    void engineRejectsMismatchedImage() {
        BinaryRainEngine e;
        e.resize({200, 100});
        QImage wrongSize(100, 100, QImage::Format_RGB32), wrongFormat(200, 100, QImage::Format_ARGB32);
        QVERIFY(!e.render(wrongSize));
        QVERIFY(!e.render(wrongFormat));
    }
    void densityControlsStreamCount() {
        BinaryRainEngine e;
        e.resize({960, 540});
        Params p = presetParams(Preset::Classic);
        p.density = 100; e.setParams(p); e.seed(3);
        const int full = e.activeStreamCount();
        p.density = 10; e.setParams(p); e.seed(3);
        QVERIFY(e.activeStreamCount() < full / 3);
    }
    void invalidParamsNeverReachEngine() {
        BinaryRainEngine e;
        Params bad; bad.charSize = -50; bad.density = 1000; bad.speed = -5;
        e.setParams(bad);
        QCOMPARE(e.params(), validated(bad));
        QVERIFY(e.cellHeight() >= 6);
        e.advance(0.033);
        QImage f(e.size(), QImage::Format_RGB32);
        QVERIFY(e.render(f));
    }
    void largeStepsAreClamped() {
        BinaryRainEngine e;
        e.resize({320, 180}); e.seed(5);
        e.advance(60.0); // a stall must not blow up
        QImage f(320, 180, QImage::Format_RGB32);
        QVERIFY(e.render(f));
    }
    void adaptsToAspectRatios() {
        for (QSize s : {QSize(1920, 1080), QSize(3440, 1440), QSize(1080, 1920), QSize(800, 800), QSize(7, 5)}) {
            BinaryRainEngine e;
            e.resize(s); e.seed(2); e.advance(0.03);
            QImage f(e.size(), QImage::Format_RGB32);
            QVERIFY(e.render(f));
            QVERIFY(e.columns() >= 1 && e.rows() >= 1);
        }
    }
    void resizeAfterRunningKeepsWorking() {
        BinaryRainEngine e;
        e.resize({640, 360}); e.seed(9);
        for (int i = 0; i < 10; ++i) e.advance(1 / 30.0);
        e.resize({1280, 720});
        QCOMPARE(e.size(), QSize(1280, 720));
        QImage f(1280, 720, QImage::Format_RGB32);
        QVERIFY(e.render(f));
    }

    // ---- frame pool (allocation / lifecycle behaviour) ----
    void poolRecyclesAndBacksOff() {
        BinaryRainFramePool pool;
        const QSize sz(64, 64);
        auto a = pool.acquire(sz);
        const QImage* pa = a.get();
        a.reset(); // consumer done
        auto again = pool.acquire(sz);
        QCOMPARE(again.get(), pa); // same buffer reused, no new allocation
        QCOMPARE(pool.bufferCount(), 1);
        std::vector<std::shared_ptr<QImage>> held{again};
        for (int i = 0; i < 10; ++i) {
            auto b = pool.acquire(sz);
            if (!b) break;
            held.push_back(b);
        }
        QVERIFY(pool.bufferCount() <= BinaryRainFramePool::kMaxBuffers);
        QVERIFY(pool.acquire(sz) == nullptr); // all busy -> skip frame
        held.clear();
        QVERIFY(pool.acquire(sz) != nullptr);
        pool.clear();
        QCOMPARE(pool.bufferCount(), 0);
    }
    void steadyStateDoesNotGrowPool() {
        BinaryRainEngine e;
        e.resize({1920, 1080}); e.seed(1);
        BinaryRainFramePool pool;
        std::shared_ptr<const QImage> consumer;
        for (int i = 0; i < 100; ++i) {
            e.advance(1 / 30.0);
            auto buf = pool.acquire(e.size());
            QVERIFY(buf);
            QVERIFY(e.render(*buf));
            consumer = buf; // one frame in flight, like the preview
        }
        QVERIFY(pool.bufferCount() <= 2);
    }

    // ---- timing (informational, machine dependent) ----
    void reportRenderCost() {
        BinaryRainEngine e;
        Params p = presetParams(Preset::Dense);
        e.setParams(p);
        e.resize({1920, 1080}); e.seed(1);
        QImage f(1920, 1080, QImage::Format_RGB32);
        QElapsedTimer t; t.start();
        const int n = 60;
        for (int i = 0; i < n; ++i) { e.advance(1 / 30.0); e.render(f); }
        qInfo() << "Dense 1920x1080 advance+render avg ms/frame:" << double(t.nsecsElapsed()) / 1e6 / n;
    }
};

QTEST_GUILESS_MAIN(BinaryRainTests)
#include "BinaryRainTests.moc"
