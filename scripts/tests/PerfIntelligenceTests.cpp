// Tests for the opt-in performance-intelligence layer: profile verification,
// activation/rollback, telemetry consent + sanitisation, offline/mock-server sync.
// Needs Python + "cryptography" (scripts/tools/mock-perf) for signed test profiles.
#include "BrowserApplyTracker.h"
#include "PerfProbe.h"
#include "ProfileStore.h"
#include "ProfileSync.h"
#include "VideoApplyTelemetry.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <memory>
#include <QSignalSpy>
#include <QThread>
#include <QTemporaryDir>
#include <QtTest>

using perf::Param;

// Records every reply a BrowserApplyTracker request receives (F3 tests).
struct Replies {
    QList<QPair<bool, QString>> got;
    BrowserApplyTracker::Reply fn() {
        return [this](bool ok, const QString& d) { got.append({ok, d}); };
    }
};

class PerfIntelligenceTests : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    QString m_profiles;
    QString toolsDir() const { return QStringLiteral(MOCK_PERF_DIR); }
    QByteArray prof(const char* name) const {
        QFile f(m_profiles + "/" + name);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
    QString fresh(const char* n) const {
        const QString d = m_tmp.path() + "/" + n;
        QDir(d).removeRecursively();
        QDir().mkpath(d);
        return d;
    }

    static QJsonObject lineAt(PerfProbe& p, int idx) {
        return QJsonDocument::fromJson(p.peekBatch(50).value(idx)).object();
    }
    struct Rec {
        QList<PerfProbe::Event> ev;
        VideoApplyTelemetry::Sink sink() { return [this](const PerfProbe::Event& e) { ev.append(e); }; }
        QStringList stages() const { QStringList s; for (const auto& e : ev) s << e.stage; return s; }
    };
    struct Replies {
        QList<QPair<bool, QString>> got;
        BrowserApplyTracker::Reply fn() { return [this](bool ok, const QString& d) { got.append({ok, d}); }; }
    };
private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        m_profiles = m_tmp.path() + "/profiles";
        QProcess p;
        p.start("python", {toolsDir() + "/make_profiles.py", m_profiles});
        QVERIFY2(p.waitForFinished(20000) && p.exitCode() == 0, p.readAllStandardError());
    }

    void verifier_acceptsValidAndRejectsBad() {
        const QByteArray key = perf::placeholderPublicKey();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QVERIFY(perf::verifyAndParse(prof("valid_v1.json"), key, now).ok);
        for (const char* bad : {"unsigned.json", "wrong_key.json", "tampered.json", "expired.json",
                                "out_of_range.json", "unknown_param.json", "garbage.json"}) {
            const auto r = perf::verifyAndParse(prof(bad), key, now);
            QVERIFY2(!r.ok, bad);
            qInfo() << bad << "->" << r.error;
        }
        QVERIFY(!perf::verifyAndParse(QByteArray(), key, now).ok);
        QVERIFY(!perf::verifyAndParse(QByteArray(70000, 'x'), key, now).ok);
        // Valid profile checked after its expiry window is rejected too.
        QVERIFY(!perf::verifyAndParse(prof("valid_v1.json"), key, now + 86400LL * 400).ok);
    }

    void store_defaultsWithoutProfile() {
        ProfileStore s;
        s.init(fresh("s0"), perf::placeholderPublicKey(), {});
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 700);
        QCOMPARE(ProfileStore::value(Param::AttachRetryMs), 300);
        QCOMPARE(ProfileStore::value(Param::DownloadTimeoutMs), 60000);
    }

    void store_installRollbackAndTrial() {
        const QString dir = fresh("s1");
        const QByteArray key = perf::placeholderPublicKey();
        ProfileStore s;
        s.init(dir, key, {});
        QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 500);
        QCOMPARE(ProfileStore::value(Param::AttachRetryMs), 200);
        QVERIFY(s.trialPending());
        s.confirmHealthy();
        QVERIFY(!s.trialPending());
        QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::NotNewer);

        // Invalid updates never change the active values.
        for (const char* bad : {"unsigned.json", "wrong_key.json", "tampered.json", "expired.json",
                                "out_of_range.json", "unknown_param.json"}) {
            QCOMPARE(s.install(prof(bad)), ProfileStore::InstallResult::Rejected);
            QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 500);
        }

        QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 600);
        QVERIFY(s.rollback());
        QCOMPARE(s.activeVersion(), 1);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 500);
        QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::KnownBad);

        // Persisted across restart.
        ProfileStore s2;
        s2.init(dir, key, {});
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 500);
        QCOMPARE(s2.activeVersion(), 1);
    }

    void store_crashDuringTrialRollsBackOnNextStart() {
        const QString dir = fresh("s2");
        const QByteArray key = perf::placeholderPublicKey();
        {
            ProfileStore s;
            s.init(dir, key, {});
            QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
            // simulated crash: confirmHealthy() never runs
        }
        ProfileStore next;
        next.init(dir, key, {});
        QCOMPARE(next.activeVersion(), 0);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 700); // back to built-in default
        QCOMPARE(next.install(prof("valid_v1.json")), ProfileStore::InstallResult::KnownBad);
    }

    void store_expiredStoredProfileFallsBack() {
        const QString dir = fresh("s3");
        const QByteArray key = perf::placeholderPublicKey();
        ProfileStore s;
        s.init(dir, key, {});
        QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
        s.confirmHealthy();
        ProfileStore later;
        later.setClock([] { return QDateTime::currentSecsSinceEpoch() + 86400LL * 400; });
        later.init(dir, key, {});
        QCOMPARE(later.activeVersion(), 0);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 700);
    }

    void probe_consentAndSanitisation() {
        const QString dir = fresh("probe");
        PerfProbe& p = PerfProbe::instance();
        p.init(dir, "1.0");
        PerfProbe::Event e;
        e.mediaKind = "image";
        e.format = "C:\\Users\\someone\\secret.png"; // must never survive
        e.bytes = 1900000;
        e.width = 3840;
        e.height = 2160;
        e.stage = "apply";
        e.ms = 1100;
        e.ok = false;
        e.errorClass = "Bad Text With Spaces https://x.example/y";

        p.setConsent(false);
        p.record(e);
        p.waitForIdle();
        QCOMPARE(p.pendingCount(), 0); // OFF by default behaviour: nothing recorded

        p.setConsent(true);
        p.record(e);
        p.record(e); // identical unchanged media: de-duplicated
        p.waitForIdle();
        QCOMPARE(p.pendingCount(), 1);
        const QByteArray line = p.peekBatch(5).value(0);
        QVERIFY(!line.contains("Users"));
        QVERIFY(!line.contains("secret"));
        QVERIFY(!line.contains("http"));
        QVERIFY(!line.contains("\\\\"));
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        QCOMPARE(o["fmt"].toString(), QString("other"));
        QCOMPARE(o["res"].toString(), QString("<=4k"));
        QCOMPARE(o["err"].toString(), QString());

        p.setConsent(false); // withdrawing consent discards the queue
        QCOMPARE(p.pendingCount(), 0);
    }

    void probe_videoMetadataRecord() {
        PerfProbe p;
        p.init(fresh("pv"), "1.0");
        PerfProbe::Event e;
        e.mediaKind = "video";
        e.stage = "meta";
        e.width = 3840;
        e.height = 2160;
        e.codec = "H265";
        e.fps = 59.94;
        p.setConsent(false);
        p.record(e);
        p.waitForIdle();
        QCOMPARE(p.pendingCount(), 0); // no consent -> nothing
        p.setConsent(true);
        p.record(e);
        p.waitForIdle();
        QCOMPARE(p.pendingCount(), 1);
        const QJsonObject o = QJsonDocument::fromJson(p.peekBatch(1).value(0)).object();
        QCOMPARE(o["kind"].toString(), QString("video"));
        QCOMPARE(o["stage"].toString(), QString("meta"));
        QCOMPARE(o["codec"].toString(), QString("h265"));
        QCOMPARE(o["fps"].toString(), QString("<=60"));
        QCOMPARE(o["res"].toString(), QString("<=4k"));
        // Unknown / hostile values are generalised, never echoed.
        PerfProbe::Event bad;
        bad.mediaKind = "video";
        bad.stage = "meta";
        bad.codec = "C:/Users/secret/clip.mkv";
        bad.fps = std::numeric_limits<double>::quiet_NaN();
        p.record(bad);
        p.waitForIdle();
        const QByteArray line = p.peekBatch(5).value(1);
        QVERIFY(!line.contains("secret") && !line.contains("Users"));
        const QJsonObject b = QJsonDocument::fromJson(line).object();
        QCOMPARE(b["codec"].toString(), QString("unknown"));
        QCOMPARE(b["fps"].toString(), QString("unknown"));
        QCOMPARE(b["res"].toString(), QString("unknown"));
        // Non-video records keep their old shape (no codec/fps keys).
        PerfProbe::Event img;
        img.mediaKind = "image";
        img.stage = "apply";
        img.format = "png";
        img.bytes = 123456;
        p.record(img);
        p.waitForIdle();
        const QJsonObject i = QJsonDocument::fromJson(p.peekBatch(5).value(2)).object();
        QVERIFY(!i.contains("codec") && !i.contains("fps"));
        QCOMPARE(PerfProbe::fpsBucket(-1), QString("unknown"));
        QCOMPARE(PerfProbe::fpsBucket(24.0), QString("<=24"));
        QCOMPARE(PerfProbe::fpsBucket(120.0), QString(">60"));
        QCOMPARE(PerfProbe::sanitizeCodec("AV1"), QString("av1"));
        QCOMPARE(PerfProbe::sanitizeCodec(""), QString("unknown"));
    }

    // ---- VideoApplyTelemetry: loaded / applied / failed / cancelled lifecycle ----
    void vt_previewLoadsButNeverApplies() {
        Rec r; VideoApplyTelemetry t(r.sink());
        t.onVideoLoaded(1, 1920, 1080, 30.0, "h264", "startup_preview", "C:/x/a.mp4");
        QCOMPARE(r.stages(), QStringList({"loaded"}));
        QCOMPARE(r.ev[0].origin, QString("startup_preview"));
        QVERIFY(!t.isPending());
    }
    void vt_successProducesExactlyOneApplied() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 k = t.begin("C:/x/a.mp4"); t.bindLoad(k, 5);
        t.onVideoLoaded(5, 1280, 720, 24.0, "h264", "wallpaper_apply", "C:/x/a.mp4");
        t.verified(k, 5); t.verified(k, 5); // duplicate verification callback
        QCOMPARE(r.stages(), QStringList({"loaded", "applied"}));
        QCOMPARE(r.ev[1].width, 1280); // metadata matched by load sequence
        QCOMPARE(r.ev[1].codec, QString("h264"));
        QVERIFY(r.ev[1].ok);
    }
    void vt_metadataMatchedByLoadSeqNotLatest() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 k = t.begin("a.mp4"); t.bindLoad(k, 2);
        t.onVideoLoaded(2, 3840, 2160, 60.0, "h265", "wallpaper_apply", "a.mp4");
        t.onVideoLoaded(3, 640, 480, 15.0, "mpeg4", "user_selection", "b.mp4"); // newer, unrelated load
        t.verified(k, 2);
        QCOMPARE(r.ev.last().stage, QString("applied"));
        QCOMPARE(r.ev.last().width, 3840);
        QCOMPARE(r.ev.last().codec, QString("h265"));
    }
    void vt_unknownMetadataStillApplies() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 k = t.begin("a.webm"); t.bindLoad(k, 9); // no metadata ever arrived
        t.verified(k, 9);
        QCOMPARE(r.stages(), QStringList({"applied"}));
        QCOMPARE(r.ev[0].codec, QString("unknown"));
        QCOMPARE(r.ev[0].width, 0);
        QCOMPARE(r.ev[0].fps, 0.0);
    }
    void vt_sameSourceTwiceGivesTwoApplied() {
        Rec r; VideoApplyTelemetry t(r.sink());
        for (int i = 0; i < 2; ++i) {
            const quint64 k = t.begin("a.mp4"); t.bindLoad(k, 10 + i);
            t.verified(k, 10 + i);
        }
        QCOMPARE(r.stages(), QStringList({"applied", "applied"}));
        QVERIFY(r.ev[0].alwaysRecord && r.ev[1].alwaysRecord);
    }
    void vt_supersededAndRemoved() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 a = t.begin("a.mp4"); t.bindLoad(a, 1);
        const quint64 b = t.begin("b.mp4"); t.bindLoad(b, 2); // supersedes a
        QCOMPARE(r.stages(), QStringList({"cancelled"}));
        QCOMPARE(r.ev[0].errorClass, QString("superseded"));
        t.verified(a, 1); // stale callback for a: must not resolve b
        QVERIFY(t.isPending() && t.pendingToken() == b);
        t.cancelPending("removed");
        t.verified(b, 2); // after removal
        QCOMPARE(r.stages(), QStringList({"cancelled", "cancelled"}));
        QCOMPARE(r.ev[1].errorClass, QString("removed"));
        QVERIFY(!r.ev[1].ok);
    }
    void vt_failureIsTerminalAndExactlyOnce() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 k = t.begin("a.mp4"); t.bindLoad(k, 1);
        t.failed(k, "player_error", 1);
        t.failed(k, "load_failed"); t.verified(k, 1);
        QCOMPARE(r.stages(), QStringList({"failed"}));
        QCOMPARE(r.ev[0].errorClass, QString("player_error"));
    }
    void vt_retriesKeepPendingUntilVerified() {
        // STALLED/retry cycles never call verified()/failed(): the token stays pending
        // and a later successful verification resolves it once.
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 k = t.begin("a.mp4"); t.bindLoad(k, 1);
        QVERIFY(r.ev.isEmpty() && t.isPending());
        t.verified(k, 1);
        QCOMPARE(r.stages(), QStringList({"applied"}));
    }
    void vt_batteryHiddenStartupStaysPendingThenCancels() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 k = t.begin("a.mp4"); t.bindLoad(k, 1); // setWallpaper hidden: no attach
        QVERIFY(r.ev.isEmpty() && t.isPending());
        t.cancelPending("removed"); // removed while hidden
        QCOMPARE(r.stages(), QStringList({"cancelled"}));
    }
    void vt_loadSupersededByNewerLoad() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 k = t.begin("a.mp4"); t.bindLoad(k, 1);
        t.verified(k, 2); // player already holds a different load
        QCOMPARE(r.ev[0].stage, QString("cancelled"));
        QCOMPARE(r.ev[0].errorClass, QString("superseded_by_load"));
    }
    void vt_playlistOriginAndPrivacyPayload() {
        PerfProbe p; p.init(fresh("vt"), "1.0"); p.setConsent(true);
        VideoApplyTelemetry t([&p](const PerfProbe::Event& e) { p.record(e); });
        const QString secret = "C:/Users/secretname/private clip.mp4";
        t.onVideoLoaded(1, 1920, 1080, 30.0, "h264", "playlist", secret);
        t.onVideoLoaded(1, 1920, 1080, 30.0, "h264", "playlist", secret); // identical loaded: de-duplicated
        for (int i = 0; i < 2; ++i) { const quint64 k = t.begin(secret); t.bindLoad(k, 1); t.verified(k, 1); }
        p.waitForIdle();
        const auto lines = p.peekBatch(50);
        QCOMPARE(lines.size(), 3); // 1 loaded + 2 applied
        for (const auto& l : lines) {
            QVERIFY(!l.contains("secretname") && !l.contains("Users") && !l.contains("private"));
            QVERIFY(!l.contains("token") && !l.contains("seq"));
        }
        const QJsonObject lo = lineAt(p, 0), ap = lineAt(p, 1);
        QCOMPARE(lo["stage"].toString(), QString("loaded"));
        QCOMPARE(lo["origin"].toString(), QString("playlist"));
        QCOMPARE(ap["stage"].toString(), QString("applied"));
        QVERIFY(!ap.contains("origin"));
        QCOMPARE(ap["res"].toString(), QString("<=1080p"));
    }
    void vt_hostileOriginAndOptOut() {
        QCOMPARE(PerfProbe::sanitizeOrigin("C:/evil"), QString("user_selection"));
        for (const char* o : {"startup_preview", "user_selection", "wallpaper_apply", "playlist"})
            QCOMPARE(PerfProbe::sanitizeOrigin(o), QString(o));
        PerfProbe p; p.init(fresh("vtoff"), "1.0"); // consent off (default)
        VideoApplyTelemetry t([&p](const PerfProbe::Event& e) { p.record(e); });
        t.onVideoLoaded(1, 1, 1, 30, "h264", "playlist", "a.mp4");
        const quint64 k = t.begin("a.mp4"); t.bindLoad(k, 1); t.verified(k, 1);
        p.waitForIdle();
        QCOMPARE(p.pendingCount(), 0);
    }

    // ---- Defect 1: stale errors / bounded pending ----
    void vt_staleErrorFromOlderLoadIsIgnored() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 a = t.begin("a.mp4"); t.bindLoad(a, 1);
        const quint64 b = t.begin("b.mp4"); t.bindLoad(b, 2); // a -> cancelled(superseded)
        t.failedForLoad(a, 1, "player_error"); // old token, old load
        t.failedForLoad(b, 1, "player_error"); // current token, but the error belongs to load 1
        QCOMPARE(r.stages(), QStringList({"cancelled"}));
        QVERIFY(t.isPending() && t.pendingToken() == b);
        t.verified(b, 2);
        QCOMPARE(r.stages(), QStringList({"cancelled", "applied"})); // not turned into a failure
        QCOMPARE(r.ev[0].errorClass, QString("superseded"));
    }
    void vt_errorOfCurrentLoadFailsExactlyOnce() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 k = t.begin("a.mp4"); t.bindLoad(k, 4);
        t.failedForLoad(k, 4, "player_error"); t.failedForLoad(k, 4, "player_error"); t.verified(k, 4);
        QCOMPARE(r.stages(), QStringList({"failed"}));
        QCOMPARE(r.ev[0].errorClass, QString("player_error"));
    }
    void vt_boundedPendingCancelsNotFails() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 a = t.begin("a.mp4"); t.bindLoad(a, 1);
        t.cancelPending("battery_hidden");
        QCOMPARE(r.ev.last().stage, QString("cancelled"));
        QCOMPARE(r.ev.last().errorClass, QString("battery_hidden"));
        const quint64 b = t.begin("b.mp4"); t.bindLoad(b, 2);
        t.expire(a, "attach_timeout"); // stale timer of a finished application: no-op
        QVERIFY(t.isPending());
        t.expire(b, "attach_timeout");
        QCOMPARE(r.stages(), QStringList({"cancelled", "cancelled"}));
        QCOMPARE(r.ev.last().errorClass, QString("attach_timeout"));
        t.verified(b, 2); // late verification after expiry: nothing more
        QCOMPARE(r.ev.size(), 2);
    }

    // ---- Defect 2: loaded vs applied ordering ----
    void vt_order_metadataBeforeApply() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 k = t.begin("a.mp4"); t.bindLoad(k, 1);
        t.onVideoLoaded(1, 1920, 1080, 30, "h264", "wallpaper_apply", "a.mp4");
        t.verified(k, 1);
        QCOMPARE(r.stages(), QStringList({"loaded", "applied"}));
        QCOMPARE(r.ev[1].codec, QString("h264"));
    }
    void vt_order_applyBeforeMetadataKeepsUnknownAndNoRewrite() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 k = t.begin("a.mp4"); t.bindLoad(k, 1);
        t.verified(k, 1);
        t.onVideoLoaded(1, 1920, 1080, 30, "h264", "wallpaper_apply", "a.mp4"); // late
        QCOMPARE(r.stages(), QStringList({"applied", "loaded"})); // no duplicate applied, no rewrite
        QCOMPARE(r.ev[0].codec, QString("unknown"));
        QCOMPARE(r.ev[0].width, 0);
        QCOMPARE(r.ev[1].codec, QString("h264")); // loaded keeps its first-seen meaning
    }
    void vt_order_supersededLoadMetadataNeverLeaks() {
        Rec r; VideoApplyTelemetry t(r.sink());
        const quint64 a = t.begin("a.mp4"); t.bindLoad(a, 1);
        const quint64 b = t.begin("b.mp4"); t.bindLoad(b, 2);
        t.onVideoLoaded(1, 3840, 2160, 60, "h265", "wallpaper_apply", "a.mp4"); // belongs to superseded load
        t.verified(b, 2);
        QCOMPARE(r.stages(), QStringList({"cancelled", "loaded", "applied"}));
        QCOMPARE(r.ev[2].codec, QString("unknown"));
        QCOMPARE(r.ev[2].width, 0);
    }

    // ---- Defect 3: queue ids / peek-trim-ack ----
    static PerfProbe::Event ev(int i) {
        PerfProbe::Event e; e.mediaKind = "video"; e.format = "mp4"; e.stage = "applied"; e.ms = i;
        e.alwaysRecord = true; return e;
    }
    void queue_ackAfterTrimDropsNothingUnrelated() {
        PerfProbe p; p.init(fresh("q1"), "1.0"); p.setConsent(true);
        for (int i = 0; i < 5; ++i) p.record(ev(i));
        p.waitForIdle();
        const auto b = p.peekBatchIds(3);
        QCOMPARE(b.ids.size(), 3);
        for (int i = 0; i < 520; ++i) p.record(ev(100 + (i % 50))); // forces a trim that drops the peeked records
        p.waitForIdle();
        const int before = p.pendingCount();
        const auto head = p.peekBatchIds(1);
        QVERIFY(head.ids.value(0) > b.ids.last()); // the peeked records were trimmed away
        p.acknowledgeIds(b.ids); // old positional ack would have eaten 3 unrelated records
        QCOMPARE(p.pendingCount(), before);
    }
    void queue_ackRemovesOnlyAckedWithConcurrentAppend() {
        PerfProbe p; p.init(fresh("q2"), "1.0"); p.setConsent(true);
        for (int i = 0; i < 6; ++i) p.record(ev(i));
        p.waitForIdle();
        const auto b = p.peekBatchIds(4);
        QThread* th = QThread::create([&p] { for (int i = 0; i < 30; ++i) p.record(ev(200 + i)); });
        th->start();
        p.acknowledgeIds(b.ids); // races with appends on the worker pool
        th->wait(); delete th; p.waitForIdle();
        const auto all = p.peekBatchIds(500);
        QCOMPARE(all.ids.size(), 2 + 30);
        for (quint64 id : b.ids) QVERIFY(!all.ids.contains(id));
        QSet<quint64> uniq(all.ids.begin(), all.ids.end());
        QCOMPARE(uniq.size(), all.ids.size()); // ids stay unique
        p.acknowledgeIds(b.ids); // idempotent
        QCOMPARE(p.pendingCount(), 32);
    }
    void queue_payloadUnchangedAndLegacyFileMigrates() {
        const QString d = fresh("q3");
        QFile f(d + "/perf-records.jsonl"); // legacy file: bare JSON lines
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"a\":1}\n{\"a\":2}\n"); f.close();
        PerfProbe p; p.init(d, "1.0");
        const auto b = p.peekBatchIds(10);
        QCOMPARE(b.records, QList<QByteArray>({"{\"a\":1}", "{\"a\":2}"}));
        p.acknowledgeIds({b.ids[0]});
        QCOMPARE(p.peekBatch(10), QList<QByteArray>({"{\"a\":2}"}));
    }

    // ---- Defect 4: trial / rollback ----
    void trial_secondInstallDuringTrialIsDeferredAndPreviousKept() {
        const QString dir = fresh("t1"); const QByteArray key = perf::placeholderPublicKey();
        ProfileStore s; s.init(dir, key, {});
        QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
        s.confirmHealthy(); // v1 is the proven profile
        QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated); // trial of v2
        QCOMPARE(s.install(prof("valid_v3.json")), ProfileStore::InstallResult::TrialActive);
        QCOMPARE(s.activeVersion(), 2);
        QVERIFY(s.rollback()); // failed validation of v2 -> last known-good v1, not an unproven profile
        QCOMPARE(s.activeVersion(), 1);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 500);
        QCOMPARE(s.install(prof("valid_v3.json")), ProfileStore::InstallResult::Activated); // now allowed
    }
    void trial_quitWithoutVerificationDoesNotConfirm() {
        const QString dir = fresh("t2"); const QByteArray key = perf::placeholderPublicKey();
        {
            ProfileStore s; s.init(dir, key, {});
            QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
            s.noteCleanExit();
            QVERIFY(s.trialPending());
        }
        {
            ProfileStore n; n.init(dir, key, {});
            QVERIFY(n.trialPending()); // still unproven, trial continues, not marked bad
            QCOMPARE(n.activeVersion(), 1);
            // this continued run crashes (no confirm, no clean exit)
        }
        ProfileStore crashed; crashed.init(dir, key, {});
        QCOMPARE(crashed.activeVersion(), 0);
        QCOMPARE(crashed.install(prof("valid_v1.json")), ProfileStore::InstallResult::KnownBad);
    }
    void trial_verifiedApplicationConfirmsAndFailedDoesNot() {
        const QString dir = fresh("t3"); const QByteArray key = perf::placeholderPublicKey();
        ProfileStore s; s.init(dir, key, {});
        QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
        // a failed application emits no wallpaperVerified -> confirmHealthy is never called
        QVERIFY(s.trialPending());
        s.confirmHealthy(); // wallpaperVerified
        QVERIFY(!s.trialPending());
        ProfileStore after; after.init(dir, key, {});
        QCOMPARE(after.activeVersion(), 1); // persisted as proven
        QVERIFY(!after.trialPending());
    }
    void trial_recoveryAfterFailedTrialKeepsKnownGood() {
        const QString dir = fresh("t4"); const QByteArray key = perf::placeholderPublicKey();
        {
            ProfileStore s; s.init(dir, key, {});
            s.install(prof("valid_v1.json")); s.confirmHealthy();
            QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated);
            QCOMPARE(s.install(prof("valid_v3.json")), ProfileStore::InstallResult::TrialActive);
            // crash: no confirm, no clean exit
        }
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 1);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 500);
        QCOMPARE(n.install(prof("valid_v2.json")), ProfileStore::InstallResult::KnownBad);
        QCOMPARE(n.install(prof("valid_v3.json")), ProfileStore::InstallResult::Activated);
    }

    void sync_offlineIsSilentAndNonBlocking() {
        PerfProbe& p = PerfProbe::instance();
        p.init(fresh("sync-off"), "1.0");
        p.setConsent(true);
        ProfileStore store;
        store.init(fresh("sync-off-s"), perf::placeholderPublicKey(), {});
        ProfileSync sync(&store, &p);
        sync.configure(QUrl("http://127.0.0.1:1"), fresh("sync-off-st")); // nothing listens
        sync.setTimeoutMs(2000);
        QSignalSpy spy(&sync, &ProfileSync::finished);
        QElapsedTimer t;
        t.start();
        sync.syncNow(true);
        QVERIFY(t.elapsed() < 100); // returns immediately (asynchronous)
        QVERIFY(spy.wait(8000));
        QCOMPARE(store.activeVersion(), 0);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 700);
    }

    void sync_gatesConsentAndEndpoint() {
        PerfProbe& p = PerfProbe::instance();
        p.init(fresh("sync-g"), "1.0");
        ProfileStore store;
        store.init(fresh("sync-g-s"), perf::placeholderPublicKey(), {});
        ProfileSync sync(&store, &p);
        sync.configure(QUrl("http://127.0.0.1:1"), fresh("sync-g-st"));
        QSignalSpy spy(&sync, &ProfileSync::finished);
        p.setConsent(false);
        sync.syncNow(true);
        QVERIFY(!sync.busy()); // no consent: no network activity at all
        p.setConsent(true);
        // Placeholder key => only loopback endpoints; production host refused.
        QVERIFY(!sync.endpointAllowed(QUrl("https://perf.example.com")));
        QVERIFY(sync.endpointAllowed(QUrl("http://127.0.0.1:8765")));
        sync.configure(QUrl("https://perf.example.com"), fresh("sync-g-st2"));
        sync.syncNow(true);
        QVERIFY(!sync.busy());
        p.setConsent(false);
    }

    void sync_mockServerUploadsAndActivates() {
        const QString recs = m_tmp.path() + "/server-records.jsonl";
        QProcess server;
        server.start("python", {toolsDir() + "/mock_server.py", "--port", "18765", "--profile",
                                m_profiles + "/valid_v1.json", "--records", recs});
        QVERIFY(server.waitForStarted());
        QTest::qWait(1200);

        PerfProbe& p = PerfProbe::instance();
        p.init(fresh("sync-ok"), "1.0");
        p.setConsent(true);
        PerfProbe::Event e;
        e.mediaKind = "video";
        e.format = "mp4";
        e.bytes = 5000000;
        e.stage = "apply";
        e.ms = 900;
        p.record(e);
        p.waitForIdle();
        QCOMPARE(p.pendingCount(), 1);

        ProfileStore store;
        store.init(fresh("sync-ok-s"), perf::placeholderPublicKey(), {});
        ProfileSync sync(&store, &p);
        sync.configure(QUrl("http://127.0.0.1:18765"), fresh("sync-ok-st"));
        QSignalSpy spy(&sync, &ProfileSync::finished);
        sync.syncNow(true);
        QVERIFY(spy.wait(10000));
        qInfo() << spy.value(0).value(0).toString();
        QCOMPARE(store.activeVersion(), 1);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 500);
        QCOMPARE(p.pendingCount(), 0); // acknowledged after the server accepted the batch
        QVERIFY(QFile(recs).size() > 0);
        QVERIFY(!QFile(recs).readAll().contains("mock"));

        // Second pass: ETag matches -> unchanged, nothing re-uploaded.
        spy.clear();
        sync.syncNow(true);
        QVERIFY(spy.wait(10000));
        QVERIFY(spy.value(0).value(0).toString().contains("unchanged"));
        p.setConsent(false);
        server.kill();
        server.waitForFinished(3000);
    }

    void sync_mockServerFailUploadKeepsRecordsQueued() {
        QProcess server;
        server.start("python", {toolsDir() + "/mock_server.py", "--port", "18767", "--profile",
                                m_profiles + "/valid_v1.json", "--records", m_tmp.path() + "/r3.jsonl",
                                "--fail-upload"});
        QVERIFY(server.waitForStarted());
        QTest::qWait(1200);
        PerfProbe& p = PerfProbe::instance();
        p.init(fresh("sync-fu"), "1.0");
        p.setConsent(true);
        PerfProbe::Event e;
        e.mediaKind = "video";
        e.format = "mp4";
        e.stage = "applied";
        e.alwaysRecord = true;
        p.record(e);
        p.waitForIdle();
        QCOMPARE(p.pendingCount(), 1);
        ProfileStore store;
        store.init(fresh("sync-fu-s"), perf::placeholderPublicKey(), {});
        ProfileSync sync(&store, &p);
        sync.configure(QUrl("http://127.0.0.1:18767"), fresh("sync-fu-st"));
        QSignalSpy spy(&sync, &ProfileSync::finished);
        sync.syncNow(true);
        QVERIFY(spy.wait(10000));
        QVERIFY(spy.value(0).value(0).toString().contains("upload failed"));
        QCOMPARE(p.pendingCount(), 1); // 500 => not acknowledged, stays queued for retry
        QVERIFY(!QFile::exists(m_tmp.path() + "/r3.jsonl"));
        QCOMPARE(store.activeVersion(), 1); // profile poll is independent of the upload failure
        p.setConsent(false);
        server.kill();
        server.waitForFinished(3000);
    }

    void sync_mockServerBadProfileKeepsDefaults() {
        QProcess server;
        server.start("python", {toolsDir() + "/mock_server.py", "--port", "18766", "--profile",
                                m_profiles + "/tampered.json", "--records", m_tmp.path() + "/r2.jsonl"});
        QVERIFY(server.waitForStarted());
        QTest::qWait(1200);
        PerfProbe& p = PerfProbe::instance();
        p.init(fresh("sync-bad"), "1.0");
        p.setConsent(true);
        ProfileStore store;
        store.init(fresh("sync-bad-s"), perf::placeholderPublicKey(), {});
        ProfileSync sync(&store, &p);
        sync.configure(QUrl("http://127.0.0.1:18766"), fresh("sync-bad-st"));
        QSignalSpy spy(&sync, &ProfileSync::finished);
        sync.syncNow(true);
        QVERIFY(spy.wait(10000));
        QCOMPARE(store.activeVersion(), 0);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 700);
        p.setConsent(false);
        server.kill();
        server.waitForFinished(3000);
    }

    // ---- Browser apply tracker: stale confirmation / unrelated error ----
    void ba_staleVerificationCannotConfirmNewerRequest() {
        BrowserApplyTracker t; Replies a, b;
        t.begin("C:/cache/a.mp4", a.fn(), "replaced");
        QVERIFY(t.begin("C:/cache/b.mp4", b.fn(), "replaced")); // newer request replaces A
        QCOMPARE(a.got.size(), 1);
        QVERIFY(!a.got[0].first);
        QCOMPARE(a.got[0].second, QString("replaced"));
        // A's old verification arrives while A is still the active file: must not confirm B
        QVERIFY(!t.confirmIfActive("C:/cache/a.mp4"));
        QVERIFY(b.got.isEmpty());
        QVERIFY(t.pending());
        // B becomes the active file and is verified: confirmed exactly once
        QVERIFY(t.confirmIfActive("C:/cache/b.mp4"));
        QVERIFY(!t.confirmIfActive("C:/cache/b.mp4"));
        QCOMPARE(b.got.size(), 1);
        QVERIFY(b.got[0].first);
        QCOMPARE(a.got.size(), 1); // A never answered twice
    }
    void ba_manualWallpaperChangeCannotConfirmOrFail() {
        BrowserApplyTracker t; Replies a;
        t.begin("C:/cache/a.mp4", a.fn(), "r");
        QVERIFY(!t.confirmIfActive("C:/user/other.mp4"));
        QVERIFY(!t.failIfActive("C:/user/other.mp4", "boom"));
        QVERIFY(!t.confirmIfActive(QString()));
        QVERIFY(a.got.isEmpty());
        QVERIFY(t.pending());
    }
    void ba_matchingActiveFileIsCaseAndSeparatorInsensitiveAndOnce() {
        BrowserApplyTracker t; Replies a;
        t.begin("C:/Cache/A.MP4", a.fn(), "r");
        QVERIFY(t.matchesActive("c:\\cache\\a.mp4"));
        QVERIFY(t.confirmIfActive("c:\\cache\\a.mp4", "is set"));
        QVERIFY(!t.confirmIfActive("C:/Cache/A.MP4"));
        QVERIFY(!t.settle(false, "late timeout"));
        QCOMPARE(a.got.size(), 1);
        QCOMPARE(a.got[0].second, QString("is set"));
        QVERIFY(!t.pending());
    }
    void ba_unrelatedErrorDoesNotFailButOwnErrorDoes() {
        BrowserApplyTracker t; Replies a;
        t.begin("C:/cache/a.mp4", a.fn(), "r");
        QVERIFY(!t.failIfActive("C:/playlist/next.mp4", "playlist item broke"));
        QVERIFY(a.got.isEmpty());
        QVERIFY(t.failIfActive("C:/cache/a.mp4", "own error"));
        QVERIFY(!t.confirmIfActive("C:/cache/a.mp4")); // a later verification cannot resurrect it
        QCOMPARE(a.got.size(), 1);
        QVERIFY(!a.got[0].first);
    }
    void ba_timeoutAnswersOnceAndEmptyPathNeverMatches() {
        BrowserApplyTracker t; Replies a;
        QVERIFY(!t.matchesActive(QString())); // nothing pending
        t.begin(QString(), a.fn(), "r");
        QVERIFY(!t.matchesActive(QString()));
        QVERIFY(t.settle(false, "timeout"));
        QVERIFY(!t.settle(false, "timeout again"));
        QCOMPARE(a.got.size(), 1);
    }

    // ---- Consent-gated source stat ----
    void stat_optOutDefaultSinkPerformsNoStat() {
        PerfProbe& p = PerfProbe::instance();
        p.init(fresh("stat-off"), "1.0");
        p.setConsent(false);
        int stats = 0;
        VideoApplyTelemetry t({}, [&stats](const QString&) { ++stats; return VideoApplyTelemetry::SourceInfo{"mp4", 5}; });
        const quint64 k = t.begin("C:/x/a.mp4"); t.bindLoad(k, 1);
        t.onVideoLoaded(1, 1280, 720, 30, "h264", "wallpaper_apply", "C:/x/a.mp4");
        t.verified(k, 1);
        p.waitForIdle();
        QCOMPARE(stats, 0);
        QCOMPARE(p.pendingCount(), 0);
    }
    void stat_optInDefaultSinkStatsAndRecords() {
        PerfProbe& p = PerfProbe::instance();
        p.init(fresh("stat-on"), "1.0");
        p.setConsent(true);
        int stats = 0;
        VideoApplyTelemetry t({}, [&stats](const QString&) { ++stats; return VideoApplyTelemetry::SourceInfo{"mp4", 5000000}; });
        const quint64 k = t.begin("C:/x/a.mp4"); t.bindLoad(k, 1);
        t.verified(k, 1);
        p.waitForIdle();
        QCOMPARE(stats, 1);
        QCOMPARE(p.pendingCount(), 1);
        const QJsonObject o = lineAt(p, 0);
        QCOMPARE(o["stage"].toString(), QString("applied"));
        QCOMPARE(o["fmt"].toString(), QString("mp4"));
        p.setConsent(false);
    }
    void stat_injectedSinkAlwaysGetsFieldsRegardlessOfConsent() {
        PerfProbe::instance().setConsent(false);
        int stats = 0;
        Rec r;
        VideoApplyTelemetry t(r.sink(), [&stats](const QString&) { ++stats; return VideoApplyTelemetry::SourceInfo{"mkv", 42}; });
        const quint64 k = t.begin("a.mkv"); t.bindLoad(k, 1);
        t.verified(k, 1);
        QCOMPARE(stats, 1);
        QCOMPARE(r.ev[0].format, QString("mkv"));
        QCOMPARE(r.ev[0].bytes, qint64(42));
    }

    // ---- Download failure telemetry ----
    void dl_failedDownloadRecordHasNoIdentifyingData() {
        PerfProbe p; p.init(fresh("dl1"), "1.0"); p.setConsent(true);
        p.record(PerfProbe::failedStageEvent("download", 1234));
        p.waitForIdle();
        QCOMPARE(p.pendingCount(), 1);
        const QByteArray raw = p.peekBatch(5).value(0);
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        QCOMPARE(o["stage"].toString(), QString("download"));
        QVERIFY(!o["ok"].toBool());
        QCOMPARE(o["err"].toString(), QString("download_failed"));
        QCOMPARE(o["ms"].toInt(), 1234);
        for (const char* k : {"url", "name", "file", "path", "host"}) QVERIFY2(!o.contains(k), k);
        for (const char* bad : {"http", "://", ".mp4", ".png", "Users"}) QVERIFY2(!raw.contains(bad), bad);
        QVERIFY(o["fmt"].toString() == "other" || o["fmt"].toString().isEmpty());
        QCOMPARE(o["size"].toString(), PerfProbe::sizeBucket(0));
        p.setConsent(false);
    }
    void dl_failedDownloadWithoutConsentRecordsNothing() {
        PerfProbe p; p.init(fresh("dl2"), "1.0"); p.setConsent(false);
        p.record(PerfProbe::failedStageEvent("download", 5));
        p.waitForIdle();
        QCOMPARE(p.pendingCount(), 0);
    }

    // ---- Upload cadence ----
    void cadence_dueFunctionBoundaries() {
        QCOMPARE(ProfileSync::kUploadPeriodSecs, 21600);
        QVERIFY(ProfileSync::due(5, 0, 21600)); // initial sync: never done => due at any clock
        const qint64 T = 1800000000;
        QVERIFY(!ProfileSync::due(T, T, 21600));
        QVERIFY(!ProfileSync::due(T + 21299, T, 21600)); // one second inside the rate limit
        QVERIFY(ProfileSync::due(T + 21300, T, 21600));  // boundary: period - 300 s slack
        QVERIFY(ProfileSync::due(T + 21600, T, 21600));
        // the 6 h timer is armed kFirstSyncDelayMs before the first sync stamps lastUpload: its tick must not be skipped
        QVERIFY(ProfileSync::due(T + 21600 - ProfileSync::kFirstSyncDelayMs / 1000, T, 21600));
        QVERIFY(!ProfileSync::due(T + 86400 - 301, T, 86400));
        QVERIFY(ProfileSync::due(T + 86400 - 300, T, 86400));
    }
    void cadence_mockServerInitialTimerRateLimitAndRepeat() {
        const QString recs = m_tmp.path() + "/cad-records.jsonl";
        QProcess server;
        server.start("python", {toolsDir() + "/mock_server.py", "--port", "18768", "--profile",
                                m_profiles + "/valid_v1.json", "--records", recs});
        QVERIFY(server.waitForStarted());
        QTest::qWait(1200);
        PerfProbe& p = PerfProbe::instance();
        p.init(fresh("cad"), "1.0"); p.setConsent(true);
        ProfileStore store; store.init(fresh("cad-s"), perf::placeholderPublicKey(), {});
        ProfileSync sync(&store, &p);
        qint64 clk = 1800000000;
        sync.setClock([&clk] { return clk; });
        sync.configure(QUrl("http://127.0.0.1:18768"), fresh("cad-st"));
        const auto addRecord = [&p](int ms) { p.record(ev(ms)); p.waitForIdle(); };
        const auto run = [&sync](bool force) {
            QSignalSpy spy(&sync, &ProfileSync::finished);
            sync.syncNow(force);
            return spy.wait(10000) ? spy.value(0).value(0).toString() : QString("TIMEOUT");
        };
        const auto lines = [&recs] { QFile f(recs); return f.open(QIODevice::ReadOnly) ? f.readAll().count('\n') : 0; };
        addRecord(1);
        QVERIFY(run(false).contains("uploaded 1"));            // initial sync
        QCOMPARE(p.pendingCount(), 0);
        const int afterFirst = lines();
        QVERIFY(afterFirst >= 1);
        addRecord(2);
        clk += 60;                                              // repeated sync right away
        QVERIFY(!run(false).contains("uploaded"));
        QCOMPARE(p.pendingCount(), 1);
        clk = 1800000000 + 21299;                               // just inside the rate limit
        QVERIFY(!run(false).contains("uploaded"));
        QCOMPARE(lines(), afterFirst);
        clk = 1800000000 + 21300;                               // six-hour timer tick (boundary)
        QVERIFY(run(false).contains("uploaded 1"));
        QCOMPARE(p.pendingCount(), 0);
        addRecord(3);
        QVERIFY(!run(false).contains("uploaded"));              // repeated sync at the same instant
        QVERIFY(run(true).contains("uploaded 1"));              // forced sync bypasses the limit
        p.setConsent(false);
        server.kill(); server.waitForFinished(3000);
    }

    // ---- Trial recovery policy ----
    void trial_forcedKillBeforeVerificationRollsBackAndMarksBad() {
        const QString dir = fresh("tr1"); const QByteArray key = perf::placeholderPublicKey();
        {
            ProfileStore s; s.init(dir, key, {});
            s.install(prof("valid_v1.json")); s.confirmHealthy();
            QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated);
            // process killed here: no confirm, no noteCleanExit
        }
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 1);
        QVERIFY(!n.trialPending());
        QCOMPARE(n.install(prof("valid_v2.json")), ProfileStore::InstallResult::KnownBad);
    }
    void trial_killAfterConfirmIsNotAFailure() {
        const QString dir = fresh("tr2"); const QByteArray key = perf::placeholderPublicKey();
        {
            ProfileStore s; s.init(dir, key, {});
            s.install(prof("valid_v1.json")); s.confirmHealthy();
        } // killed afterwards
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 1);
        QVERIFY(!n.trialPending());
        QCOMPARE(n.install(prof("valid_v1.json")), ProfileStore::InstallResult::NotNewer);
    }
    void trial_cleanExitThenVerifiedRunConfirms() {
        const QString dir = fresh("tr3"); const QByteArray key = perf::placeholderPublicKey();
        {
            ProfileStore s; s.init(dir, key, {});
            s.install(prof("valid_v1.json")); s.noteCleanExit();
        }
        {
            ProfileStore n; n.init(dir, key, {});
            QVERIFY(n.trialPending()); QCOMPARE(n.activeVersion(), 1);
            n.confirmHealthy(); // this run verified a wallpaper
        }
        ProfileStore m; m.init(dir, key, {});
        QVERIFY(!m.trialPending()); QCOMPARE(m.activeVersion(), 1);
    }
    void trial_interruptedInstallBeforeActiveSwapIsNotATrial() {
        const QString dir = fresh("tr4"); const QByteArray key = perf::placeholderPublicKey();
        {
            ProfileStore s; s.init(dir, key, {});
            s.install(prof("valid_v1.json")); s.confirmHealthy();
        }
        // Simulate a crash inside install(v2): trial state persisted, active.json still v1.
        QFile st(dir + "/state.json");
        QVERIFY(st.open(QIODevice::WriteOnly | QIODevice::Truncate));
        st.write("{\"trial\":true,\"cleanExit\":false,\"trialVersion\":2,\"bad\":[]}"); st.close();
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 1);      // known-good profile untouched
        QVERIFY(!n.trialPending());
        QCOMPARE(n.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated); // v2 was never marked bad
    }
    void trial_legacyStateWithoutTrialVersionStillRollsBack() {
        const QString dir = fresh("tr5"); const QByteArray key = perf::placeholderPublicKey();
        {
            ProfileStore s; s.init(dir, key, {});
            s.install(prof("valid_v1.json"));
        }
        QFile st(dir + "/state.json");
        QVERIFY(st.open(QIODevice::WriteOnly | QIODevice::Truncate));
        st.write("{\"trial\":true,\"cleanExit\":false,\"bad\":[]}"); st.close(); // pre-trialVersion format
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 0);
        QCOMPARE(n.install(prof("valid_v1.json")), ProfileStore::InstallResult::KnownBad);
    }

    // ---- F4: persistence failures are observable and never leave a half-committed trial ----
    static bool okWrite(const QString& path, const QByteArray& data) {
        QSaveFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
    }
    // Fails writes whose file name is in `names`; `okFirst` successful writes of a failing name are allowed first.
    static ProfileStore::Writer failing(const QStringList& names, int okFirst = 0) {
        auto seen = std::make_shared<int>(0);
        return [names, okFirst, seen](const QString& path, const QByteArray& data) {
            if (names.contains(QFileInfo(path).fileName()) && (*seen)++ >= okFirst) {
                return false;
            }
            return okWrite(path, data);
        };
    }
    // A confirmed known-good v1 in `dir`.
    void seedKnownGood(const QString& dir) {
        ProfileStore s; s.init(dir, perf::placeholderPublicKey(), {});
        QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
        QVERIFY(s.confirmHealthy());
    }

    void f4_stateWriteFailureBlocksInstallAndKeepsKnownGood() {
        const QString dir = fresh("f4a"); const QByteArray key = perf::placeholderPublicKey();
        seedKnownGood(dir);
        ProfileStore s; s.init(dir, key, {});
        s.setWriterForTest(failing({"state.json"}));
        QString err;
        QCOMPARE(s.install(prof("valid_v2.json"), &err), ProfileStore::InstallResult::StoreFailed);
        QVERIFY(!err.isEmpty());
        QVERIFY(!s.trialPending());
        QCOMPARE(s.activeVersion(), 1);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 500); // v1 values still in force
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 1);
        QVERIFY(!n.trialPending());
        QCOMPARE(n.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated); // not marked bad
    }
    void f4_rollbackCopyFailureBlocksInstall() {
        const QString dir = fresh("f4b"); const QByteArray key = perf::placeholderPublicKey();
        seedKnownGood(dir);
        ProfileStore s; s.init(dir, key, {});
        s.setWriterForTest(failing({"previous.json"}));
        QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::StoreFailed);
        QVERIFY(!s.trialPending());
        QCOMPARE(s.activeVersion(), 1);
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 1);
        QVERIFY(!n.trialPending());
    }
    void f4_activeWriteFailureAfterStateWriteUndoesTrial() {
        const QString dir = fresh("f4c"); const QByteArray key = perf::placeholderPublicKey();
        seedKnownGood(dir);
        ProfileStore s; s.init(dir, key, {});
        s.setWriterForTest(failing({"active.json"}));
        QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::StoreFailed);
        QVERIFY(!s.trialPending());
        QCOMPARE(s.activeVersion(), 1);
        QCOMPARE(ProfileStore::value(Param::VerifyWaitMs), 500);
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 1);
        QVERIFY(!n.trialPending());
        QCOMPARE(n.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated);
    }
    void f4_activeWriteFailureThenTrialRecordCannotBeClearedIsRecovered() {
        const QString dir = fresh("f4d"); const QByteArray key = perf::placeholderPublicKey();
        seedKnownGood(dir);
        ProfileStore s; s.init(dir, key, {});
        // First state write (the trial record) succeeds, the undo write fails, active.json write fails.
        auto seen = std::make_shared<int>(0);
        s.setWriterForTest([seen](const QString& path, const QByteArray& data) {
            const QString n = QFileInfo(path).fileName();
            if (n == "active.json") return false;
            if (n == "state.json" && (*seen)++ >= 1) return false;
            return okWrite(path, data);
        });
        QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::StoreFailed);
        ProfileStore n; n.init(dir, key, {}); // leftover record names v2 but active.json is still v1
        QCOMPARE(n.activeVersion(), 1);
        QVERIFY(!n.trialPending());
        QCOMPARE(n.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated); // v2 not marked bad
    }
    void f4_successfulInstallThenCrashRecovers() {
        const QString dir = fresh("f4e"); const QByteArray key = perf::placeholderPublicKey();
        seedKnownGood(dir);
        {
            ProfileStore s; s.init(dir, key, {});
            QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated);
            QVERIFY(s.trialPending());
        } // crash
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 1);
        QVERIFY(!n.trialPending());
        QCOMPARE(n.install(prof("valid_v2.json")), ProfileStore::InstallResult::KnownBad);
    }
    void f4_confirmPersistFailureKeepsTrialPendingThenRetries() {
        const QString dir = fresh("f4f"); const QByteArray key = perf::placeholderPublicKey();
        ProfileStore s; s.init(dir, key, {});
        QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
        s.setWriterForTest(failing({"state.json"}));
        QVERIFY(!s.confirmHealthy());
        QVERIFY(s.trialPending());           // never reported healthy while the record could not be written
        s.setWriterForTest(nullptr);
        QVERIFY(s.confirmHealthy());
        QVERIFY(!s.trialPending());
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 1);
        QVERIFY(!n.trialPending());
    }
    void f4_cleanExitPersistFailureRollsBackNextStart() {
        const QString dir = fresh("f4g"); const QByteArray key = perf::placeholderPublicKey();
        {
            ProfileStore s; s.init(dir, key, {});
            QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
            s.setWriterForTest(failing({"state.json"}));
            QVERIFY(!s.noteCleanExit());
        }
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 0);      // treated as a crash: conservative rollback
        QCOMPARE(n.install(prof("valid_v1.json")), ProfileStore::InstallResult::KnownBad);
    }
    void f4_initCleanExitWithUndurableMarkerRollsBack() {
        const QString dir = fresh("f4h"); const QByteArray key = perf::placeholderPublicKey();
        {
            ProfileStore s; s.init(dir, key, {});
            QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
            QVERIFY(s.noteCleanExit());
        }
        {
            ProfileStore n; n.setWriterForTest(failing({"state.json"}));
            n.init(dir, key, {});
            QCOMPARE(n.activeVersion(), 0);  // did not continue an unproven trial it cannot protect
        }
        ProfileStore m; m.init(dir, key, {});
        QCOMPARE(m.activeVersion(), 0);
        QVERIFY(!m.trialPending());
    }
    void f4_rollbackRestoreFailureKeepsTrialRecordAndRetries() {
        const QString dir = fresh("f4i"); const QByteArray key = perf::placeholderPublicKey();
        seedKnownGood(dir);
        {
            ProfileStore s; s.init(dir, key, {});
            QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated);
            s.setWriterForTest(failing({"active.json"}));
            QVERIFY(!s.rollback());
            QCOMPARE(s.activeVersion(), 1);  // known-good values applied in memory regardless
            QVERIFY(s.trialPending());       // trial record kept: not silently declared resolved
        }
        ProfileStore n; n.init(dir, key, {}); // active.json is still v2, no clean exit -> retried rollback
        QCOMPARE(n.activeVersion(), 1);
        QVERIFY(!n.trialPending());
        QCOMPARE(n.install(prof("valid_v2.json")), ProfileStore::InstallResult::KnownBad);
    }

    // Review finding: after a failed rollback, neither a verification nor a clean exit may end the trial while
    // active.json still holds the failed profile.
    void f4_failedRollbackCannotBeConfirmedHealthy() {
        const QString dir = fresh("f4j"); const QByteArray key = perf::placeholderPublicKey();
        seedKnownGood(dir);
        {
            ProfileStore s; s.init(dir, key, {});
            QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated);
            s.setWriterForTest(failing({"active.json"}));
            QVERIFY(!s.rollback());
            QVERIFY(!s.confirmHealthy(s.generation())); // would have ended the trial and blessed v2 on disk
            QVERIFY(s.trialPending());
            s.setWriterForTest(nullptr);                 // disk recovers: the next verification completes the rollback
            QVERIFY(!s.confirmHealthy(s.generation()));
            QVERIFY(!s.trialPending());
            QCOMPARE(s.activeVersion(), 1);
        }
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 1);
        QCOMPARE(n.install(prof("valid_v2.json")), ProfileStore::InstallResult::KnownBad);
    }
    void f4_failedRollbackThenCleanExitStillRollsBackNextStart() {
        const QString dir = fresh("f4k"); const QByteArray key = perf::placeholderPublicKey();
        seedKnownGood(dir);
        {
            ProfileStore s; s.init(dir, key, {});
            QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated);
            s.setWriterForTest(failing({"active.json"}));
            QVERIFY(!s.rollback());
            QVERIFY(!s.noteCleanExit()); // must not mark a clean exit over a bad active.json
        }
        ProfileStore n; n.init(dir, key, {});
        QCOMPARE(n.activeVersion(), 1);  // not v2 "continuing"
        QVERIFY(!n.trialPending());
    }

    // ---- F3: a superseded browser request is answered once, truthfully, and immediately ----

    void f3_manualOrPlaylistSelectionOfAnotherFileSupersedes() { // loadVideoSource() is the one path for both
        BrowserApplyTracker t; Replies r;
        t.begin("C:/cache/a.mp4", r.fn(), "replaced");
        QVERIFY(t.supersedeUnlessActive("C:/videos/other.mp4", "cancelled: different wallpaper"));
        QCOMPARE(r.got.size(), 1);
        QVERIFY(!r.got[0].first);
        QCOMPARE(r.got[0].second, QString("cancelled: different wallpaper"));
        QVERIFY(!r.got[0].second.contains("may still appear"));
        QVERIFY(!t.pending());
        // exactly once: later verification, error, timeout or a second supersede are all no-ops
        QVERIFY(!t.confirmIfActive("C:/cache/a.mp4"));
        QVERIFY(!t.failIfActive("C:/cache/a.mp4", "x"));
        QVERIFY(!t.settle(false, "timeout"));
        QVERIFY(!t.supersedeUnlessActive("C:/videos/x.mp4", "again"));
        QCOMPARE(r.got.size(), 1);
    }
    void f3_requestsOwnLoadDoesNotSupersedeItself() {
        BrowserApplyTracker t; Replies r;
        t.begin("C:/Cache/A.mp4", r.fn(), "replaced");
        QVERIFY(!t.supersedeUnlessActive("c:\\cache\\a.mp4", "cancelled")); // same file, other spelling
        QVERIFY(t.pending());
        QVERIFY(r.got.isEmpty());
        QVERIFY(t.confirmIfActive("C:/cache/a.mp4")); // still confirmable
        QCOMPARE(r.got.size(), 1);
        QVERIFY(r.got[0].first);
    }
    void f3_removalCancelsPendingRequestOnce() {
        BrowserApplyTracker t; Replies r;
        t.begin("C:/cache/a.mp4", r.fn(), "replaced");
        QVERIFY(t.cancel("cancelled: removed"));
        QVERIFY(!t.cancel("cancelled: removed"));
        QCOMPARE(r.got.size(), 1);
        QVERIFY(!r.got[0].first);
        QVERIFY(!t.confirmIfActive("C:/cache/a.mp4"));
        QCOMPARE(r.got.size(), 1);
    }
    void f3_newerRequestReplacesOlderAndOnlyTheOlder() {
        BrowserApplyTracker t; Replies a, b;
        t.begin("C:/cache/a.mp4", a.fn(), "replaced");
        QVERIFY(t.begin("C:/cache/b.mp4", b.fn(), "replaced"));
        QCOMPARE(a.got.size(), 1);
        QVERIFY(!a.got[0].first);
        QVERIFY(b.got.isEmpty());
        QVERIFY(!t.supersedeUnlessActive("C:/cache/b.mp4", "x")); // b's own load: untouched
        QVERIFY(t.pending());
        QVERIFY(t.confirmIfActive("C:/cache/b.mp4"));
        QCOMPARE(a.got.size(), 1);
        QCOMPARE(b.got.size(), 1);
    }
    void f3_noPendingRequestMeansNothingToSupersede() {
        BrowserApplyTracker t;
        QVERIFY(!t.supersedeUnlessActive("C:/x.mp4", "x"));
        QVERIFY(!t.cancel("x"));
    }
    void f3_genuinelyPendingRequestStillGetsItsTimeoutSettle() {
        BrowserApplyTracker t; Replies r;
        t.begin("C:/cache/a.mp4", r.fn(), "replaced");
        QVERIFY(t.settle(false, "timeout")); // MainWindow's 45 s timer path is unchanged
        QCOMPARE(r.got.size(), 1);
        QCOMPARE(r.got[0].second, QString("timeout"));
    }

    // ---- F6: a verification must belong to the profile generation it confirms ----
    void f6_verificationStartedBeforeInstallCannotConfirmTrial() {
        const QString dir = fresh("f6a"); const QByteArray key = perf::placeholderPublicKey();
        ProfileStore s; s.init(dir, key, {});
        const quint64 before = s.generation(); // verification begins here
        QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
        QVERIFY(s.generation() != before);
        QVERIFY(!s.confirmHealthy(before));
        QVERIFY(s.trialPending());
    }
    void f6_verificationAfterInstallConfirms() {
        const QString dir = fresh("f6b"); const QByteArray key = perf::placeholderPublicKey();
        ProfileStore s; s.init(dir, key, {});
        QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
        const quint64 g = s.generation();
        QVERIFY(s.confirmHealthy(g));
        QVERIFY(!s.trialPending());
        QVERIFY(!s.confirmHealthy(g)); // nothing left to confirm
    }
    void f6_staleCallbackAfterNewerProfileIsRejected() {
        const QString dir = fresh("f6c"); const QByteArray key = perf::placeholderPublicKey();
        ProfileStore s; s.init(dir, key, {});
        QCOMPARE(s.install(prof("valid_v1.json")), ProfileStore::InstallResult::Activated);
        const quint64 g1 = s.generation();
        QVERIFY(s.confirmHealthy(g1));
        QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated);
        QVERIFY(!s.confirmHealthy(g1));            // stale: began under v1
        QVERIFY(s.trialPending());
        QVERIFY(s.confirmHealthy(s.generation())); // genuine for v2
    }
    void f6_rollbackAndHardwareGeneration() {
        const QString dir = fresh("f6d"); const QByteArray key = perf::placeholderPublicKey();
        seedKnownGood(dir);
        ProfileStore s; s.init(dir, key, {});
        QCOMPARE(s.install(prof("valid_v2.json")), ProfileStore::InstallResult::Activated);
        const quint64 g2 = s.generation();
        s.setHardware({});                         // re-resolves values, same profile: same identity
        QCOMPARE(s.generation(), g2);
        QVERIFY(s.rollback());
        QVERIFY(s.generation() != g2);             // rollback is a new identity
        QVERIFY(!s.confirmHealthy(g2));
    }
};

QTEST_MAIN(PerfIntelligenceTests)
#include "PerfIntelligenceTests.moc"
