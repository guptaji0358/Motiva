#pragma once
// Opt-in, anonymous performance measurements. Observer only: it never touches
// the wallpaper pipeline, does all I/O on its own single worker thread, and
// record() is a no-op (one atomic read) unless the user opted in.
//
// A record holds ONLY coarse, non-identifying values: media kind, format
// (allowlisted extension), bucketed size/resolution, stage duration, outcome
// class, hardware class, profile version. Never a path, URL, filename, image
// content, user name or machine name.

#include "PerfProfile.h"

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <atomic>

class PerfProbe : public QObject {
    Q_OBJECT
public:
    struct Event {
        QString mediaKind; // image | video | gif | other
        QString format;    // file extension (sanitized to an allowlist)
        qint64 bytes = 0;
        int width = 0; // 0 = unknown
        int height = 0;
        QString stage; // download | apply
        int ms = 0;
        bool ok = true;
        QString errorClass; // short [a-z_] token, e.g. "timeout"; never free text
        QString codec;      // video only: allowlisted token (h264, vp9, ...); empty = not a video record
        double fps = 0.0;   // video only: 0 = unknown
        QString origin;     // loaded only: startup_preview | user_selection | wallpaper_apply | playlist
        bool alwaysRecord = false; // lifecycle outcomes (applied/failed/cancelled): never de-duplicated
    };

    explicit PerfProbe(QObject* parent = nullptr);
    static PerfProbe& instance();

    void init(const QString& dir, const QString& appVersion);
    void setConsent(bool optedIn);
    bool consent() const { return m_consent.load(); }

    void record(const Event& e); // async; no-op without consent
    void waitForIdle();          // tests/shutdown

    // Upload support (called from the sync layer): oldest-first batch, then ack.
    // Every stored record has a stable sequence id (file line "<id> TAB <json>"; the uploaded JSON is unchanged).
    struct Batch { QList<QByteArray> records; QList<quint64> ids; };
    Batch peekBatchIds(int maxRecords);
    QList<QByteArray> peekBatch(int maxRecords); // payloads only
    // Removes exactly the records with these ids. Ids already trimmed/cleared are ignored; records
    // appended or surviving a trim in between are never touched.
    void acknowledgeIds(const QList<quint64>& ids);
    int pendingCount();
    void clearAll(); // e.g. when consent is withdrawn

    // A download that produced no file. Carries NO URL, file name, path, format or size (none is known
    // locally and none may be recorded): only the stage, the elapsed time and "<stage>_failed".
    static Event failedStageEvent(const QString& stage, int ms);

    static perf::HardwareClass detectHardware();
    static QString sizeBucket(qint64 bytes);
    static QString resolutionBucket(int w, int h);
    static QString sanitizeFormat(const QString& ext);
    static QString sanitizeToken(const QString& t);
    static QString sanitizeCodec(const QString& c); // allowlist, else "unknown"
    static QString sanitizeOrigin(const QString& o); // allowlist, else "user_selection"
    static QString fpsBucket(double fps);           // invalid -> "unknown"

private:
    QByteArray buildLine(const Event& e) const;
    void append(const QByteArray& line);

    QString m_dir;
    QString m_appVersion;
    std::atomic<bool> m_consent{false};
    QThreadPool m_pool;
    QMutex m_mutex;
    struct Line { quint64 id; QByteArray json; };
    QList<Line> readLines(); // caller holds m_mutex; assigns ids to legacy un-numbered lines
    void writeLines(const QList<Line>& lines);
    QSet<quint64> m_seen;
    quint64 m_nextId = 1;
};
