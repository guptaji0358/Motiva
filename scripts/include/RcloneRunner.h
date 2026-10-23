#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTimer>

// Process abstraction around ONE rclone invocation. Never blocks the GUI
// thread: QProcess runs asynchronously and this object reports progress and
// the outcome through signals. Structured output is used wherever rclone
// offers it (--use-json-log progress/errors, lsjson, --combined); nothing
// here scrapes human-readable console text for file metadata.
//
// Secrets: rclone's own messages can contain tokens in rare error paths, so
// every message that leaves this class goes through scrub(). The argument
// list is never logged or exposed.

struct RcloneProgress {
    qint64 bytes = 0;
    qint64 totalBytes = 0;
    double speed = 0;      // bytes/s
    int etaSeconds = -1;   // -1 unknown
    int errors = 0;
    int transfersDone = 0;
    int transfersTotal = 0;
    QString current;       // name of a file being transferred
    int currentPercent = -1;
};

enum class RcloneError {
    None,
    Auth,       // token expired / revoked -> reconnect
    Network,
    NotFound,
    RateLimit,
    Quota,
    Config,     // config unreadable
    Conflict,   // a different file already exists and was left untouched (--immutable)
    Other,
};

struct RcloneResult {
    enum class Status { NotStarted, Ok, Failed, Cancelled, TimedOut };
    Status status = Status::NotStarted;
    int exitCode = -1;
    QByteArray out;            // stdout
    QStringList messages;      // scrubbed error-level log lines (JSON "msg")
    RcloneError kind = RcloneError::None;

    bool ok() const { return status == Status::Ok; }
    // One short, user-facing sentence describing the failure (empty on success).
    QString userMessage() const;
};

class RcloneJob : public QObject {
    Q_OBJECT
public:
    RcloneJob(const QString& exe, const QStringList& args, const QProcessEnvironment& env, QObject* parent = nullptr);
    ~RcloneJob() override;

    // `totalMs` bounds the whole run (0 = unbounded, used for transfers);
    // `idleMs` bounds the time without any stderr output (0 = off).
    void setTimeouts(int totalMs, int idleMs);
    void start();
    // Graceful stop (Ctrl+C so rclone cleans up partial files), then a hard kill.
    void cancel();
    bool isRunning() const { return m_proc && m_proc->state() != QProcess::NotRunning; }

    static QString scrub(const QString& text);
    static RcloneError classify(const QStringList& messages, int exitCode);

signals:
    void progress(const RcloneProgress& p);
    void finished(const RcloneResult& r);

private:
    void onStderr();
    void parseLogLine(const QByteArray& line);
    void finish(RcloneResult::Status status);
    void sendCtrlC();

    QString m_exe;
    QStringList m_args;
    QProcessEnvironment m_env;
    QProcess* m_proc = nullptr;
    QTimer m_totalTimer;
    QTimer m_idleTimer;
    QTimer m_killTimer;
    QByteArray m_errBuf;
    QByteArray m_out;
    QStringList m_messages;
    bool m_cancelled = false;
    bool m_timedOut = false;
    bool m_done = false;
    bool m_startFailed = false;
    int m_idleMs = 0;
};
