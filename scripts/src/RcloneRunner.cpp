#include "RcloneRunner.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <windows.h>

namespace {

constexpr int kMaxCapturedBytes = 64 * 1024 * 1024;
constexpr int kGracefulStopMs = 4000;

} // namespace

QString RcloneResult::userMessage() const {
    switch (status) {
    case Status::Ok:
        return QString();
    case Status::Cancelled:
        return QObject::tr("Cancelled.");
    case Status::TimedOut:
        return QObject::tr("The cloud service took too long to answer. Check your connection and try again.");
    case Status::NotStarted:
        return QObject::tr("The cloud transfer tool (rclone) could not be started.");
    case Status::Failed:
        break;
    }
    switch (kind) {
    case RcloneError::Auth:
        return QObject::tr("The sign-in for this account has expired or was revoked. Use Reconnect to sign in again.");
    case RcloneError::Network:
        return QObject::tr("Couldn't reach the service. Check your internet connection and try again.");
    case RcloneError::NotFound:
        return QObject::tr("That folder or file no longer exists in the cloud.");
    case RcloneError::RateLimit:
        return QObject::tr("The service is rate limiting requests. Wait a minute and try again.");
    case RcloneError::Quota:
        return QObject::tr("The cloud account is out of space or has reached a quota.");
    case RcloneError::Conflict:
        return QObject::tr("A different file with the same name already exists there, so it was left untouched.");
    case RcloneError::Config:
        return QObject::tr("Motiva's saved cloud accounts can't be read (the encryption key is missing or changed).");
    default:
        break;
    }
    // Prefer the first specific message over rclone's generic wrap-up lines.
    QString best;
    for (const QString& line : messages) {
        if (!line.contains(QLatin1String("Can't retry")) && !line.contains(QLatin1String("not attempting retries"))) {
            best = line;
            break;
        }
    }
    if (best.isEmpty() && !messages.isEmpty()) {
        best = messages.first();
    }
    if (!best.isEmpty()) {
        QString m = best.simplified();
        if (m.size() > 240) {
            m = m.left(237) + QStringLiteral("...");
        }
        return m;
    }
    return QObject::tr("The cloud operation failed (code %1).").arg(exitCode);
}

QString RcloneJob::scrub(const QString& text) {
    static const QRegularExpression patterns[] = {
        QRegularExpression(QStringLiteral(R"((?i)(access_token|refresh_token|id_token|client_secret|password|pass|token|secret|authorization|api[_-]?key)(\"?\s*[:=]\s*)(\"[^\"]*\"|\{[^}]*\}|[^\s,;]+))")),
        QRegularExpression(QStringLiteral(R"((?i)bearer\s+[A-Za-z0-9._~+/=-]+)")),
        QRegularExpression(QStringLiteral(R"(ya29\.[A-Za-z0-9._-]+)")),
        QRegularExpression(QStringLiteral(R"(\beyJ[A-Za-z0-9._-]{20,})")),
        QRegularExpression(QStringLiteral(R"((?i)([?&](?:code|token|access_token)=)[^&\s]+)")),
    };
    QString out = text;
    out.replace(patterns[0], QStringLiteral("\\1\\2[hidden]"));
    out.replace(patterns[1], QStringLiteral("Bearer [hidden]"));
    out.replace(patterns[2], QStringLiteral("[hidden]"));
    out.replace(patterns[3], QStringLiteral("[hidden]"));
    out.replace(patterns[4], QStringLiteral("\\1[hidden]"));
    return out;
}

RcloneError RcloneJob::classify(const QStringList& messages, int exitCode) {
    const QString all = messages.join(QLatin1Char('\n')).toLower();
    auto has = [&all](std::initializer_list<const char*> needles) {
        for (const char* n : needles) {
            if (all.contains(QLatin1String(n))) {
                return true;
            }
        }
        return false;
    };
    if (has({"immutable file modified"})) {
        return RcloneError::Conflict;
    }
    if (has({"unable to decrypt configuration", "configuration encrypted"})) {
        return RcloneError::Config;
    }
    if (has({"invalid_grant", "token expired", "expired or revoked", "couldn't fetch token", "failed to get oauth token",
             "oauth2: cannot fetch token", "401 unauthorized", "invalidauthenticationtoken", "aadsts", "invalid_access_token",
             "expired_access_token", "unauthorized_client", "autherror", "status code 401", "error 401"})) {
        return RcloneError::Auth;
    }
    if (has({"ratelimit", "rate limit", "too many requests", "error 429", "status code 429", "toomanyrequests", "quota exceeded for quota metric"})) {
        return RcloneError::RateLimit;
    }
    if (has({"storagequotaexceeded", "insufficient storage", "out of space", "insufficient_space", "quota exceeded"})) {
        return RcloneError::Quota;
    }
    if (has({"no such host", "dial tcp", "i/o timeout", "connection refused", "connection reset", "network is unreachable",
             "tls handshake", "context deadline exceeded", "timeout awaiting", "no route to host", "unexpected eof"})) {
        return RcloneError::Network;
    }
    if (exitCode == 3 || exitCode == 4 || has({"directory not found", "object not found", "file not found", "couldn't find root"})) {
        return RcloneError::NotFound;
    }
    return RcloneError::Other;
}

RcloneJob::RcloneJob(const QString& exe, const QStringList& args, const QProcessEnvironment& env, QObject* parent)
    : QObject(parent), m_exe(exe), m_args(args), m_env(env) {
    m_totalTimer.setSingleShot(true);
    m_idleTimer.setSingleShot(true);
    m_killTimer.setSingleShot(true);
    connect(&m_totalTimer, &QTimer::timeout, this, [this] {
        m_timedOut = true;
        cancel();
    });
    connect(&m_idleTimer, &QTimer::timeout, this, [this] {
        m_timedOut = true;
        cancel();
    });
    connect(&m_killTimer, &QTimer::timeout, this, [this] {
        if (m_proc && m_proc->state() != QProcess::NotRunning) {
            m_proc->kill();
        }
    });
}

RcloneJob::~RcloneJob() {
    if (m_proc) {
        m_proc->disconnect(this);
        if (m_proc->state() != QProcess::NotRunning) {
            m_proc->kill();
            m_proc->waitForFinished(2000);
        }
    }
}

void RcloneJob::setTimeouts(int totalMs, int idleMs) {
    m_totalTimer.setInterval(qMax(0, totalMs));
    m_idleMs = qMax(0, idleMs);
    m_idleTimer.setInterval(m_idleMs);
}

void RcloneJob::start() {
    m_proc = new QProcess(this);
    m_proc->setProgram(m_exe);
    m_proc->setArguments(m_args);
    m_proc->setProcessEnvironment(m_env);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    // No console window flashing up in front of a GUI app.
    m_proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });

    connect(m_proc, &QProcess::readyReadStandardOutput, this, [this] {
        const QByteArray chunk = m_proc->readAllStandardOutput();
        if (m_out.size() + chunk.size() <= kMaxCapturedBytes) {
            m_out += chunk;
        }
    });
    connect(m_proc, &QProcess::readyReadStandardError, this, &RcloneJob::onStderr);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            m_startFailed = true;
            finish(RcloneResult::Status::NotStarted);
        }
    });
    connect(m_proc, &QProcess::finished, this, [this](int, QProcess::ExitStatus) { finish(RcloneResult::Status::Failed); });

    if (m_totalTimer.interval() > 0) {
        m_totalTimer.start();
    }
    if (m_idleMs > 0) {
        m_idleTimer.start();
    }
    m_proc->start();
    m_proc->closeWriteChannel(); // never wait for a prompt on stdin
}

void RcloneJob::sendCtrlC() {
    if (!m_proc || m_proc->state() == QProcess::NotRunning) {
        return;
    }
    // A GUI process has no console, so borrow the (hidden) one of the child for
    // an instant, send Ctrl+C - rclone then stops cleanly and removes its
    // partial files - and let go again. The event goes to every process on
    // that console, this one included, and is dispatched asynchronously: Ctrl+C
    // must be ignored here (set while attached, restored a moment later), or
    // Motiva itself would be terminated. Verified with a standalone test.
    const DWORD pid = DWORD(m_proc->processId());
    FreeConsole();
    if (AttachConsole(pid)) {
        SetConsoleCtrlHandler(nullptr, TRUE);
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        FreeConsole();
        QTimer::singleShot(2000, [] { SetConsoleCtrlHandler(nullptr, FALSE); });
    }
}

void RcloneJob::cancel() {
    if (m_done || !m_proc || m_proc->state() == QProcess::NotRunning) {
        return;
    }
    if (!m_timedOut) {
        m_cancelled = true;
    }
    m_totalTimer.stop();
    m_idleTimer.stop();
    sendCtrlC();
    m_killTimer.start(kGracefulStopMs); // hard stop if it does not exit by itself
}

void RcloneJob::onStderr() {
    m_errBuf += m_proc->readAllStandardError();
    if (m_idleMs > 0) {
        m_idleTimer.start();
    }
    int nl;
    while ((nl = m_errBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_errBuf.left(nl).trimmed();
        m_errBuf.remove(0, nl + 1);
        if (!line.isEmpty()) {
            parseLogLine(line);
        }
    }
    if (m_errBuf.size() > 1024 * 1024) {
        m_errBuf.clear();
    }
}

void RcloneJob::parseLogLine(const QByteArray& line) {
    if (line.startsWith('{')) {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        if (pe.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject o = doc.object();
            if (o.contains(QStringLiteral("stats"))) {
                const QJsonObject s = o.value(QStringLiteral("stats")).toObject();
                RcloneProgress p;
                p.bytes = qint64(s.value(QStringLiteral("bytes")).toDouble());
                p.totalBytes = qint64(s.value(QStringLiteral("totalBytes")).toDouble());
                p.speed = s.value(QStringLiteral("speed")).toDouble();
                const QJsonValue eta = s.value(QStringLiteral("eta"));
                p.etaSeconds = eta.isNull() || eta.isUndefined() ? -1 : int(eta.toDouble());
                p.errors = s.value(QStringLiteral("errors")).toInt();
                p.transfersDone = s.value(QStringLiteral("transfers")).toInt();
                p.transfersTotal = s.value(QStringLiteral("totalTransfers")).toInt();
                const QJsonArray tr = s.value(QStringLiteral("transferring")).toArray();
                if (!tr.isEmpty()) {
                    const QJsonObject t = tr.first().toObject();
                    p.current = t.value(QStringLiteral("name")).toString();
                    p.currentPercent = t.value(QStringLiteral("percentage")).toInt(-1);
                }
                emit progress(p);
                return;
            }
            const QString level = o.value(QStringLiteral("level")).toString();
            if (level == QLatin1String("error") || level == QLatin1String("critical") || level == QLatin1String("fatal")) {
                QString msg = o.value(QStringLiteral("msg")).toString();
                const QString obj = o.value(QStringLiteral("object")).toString();
                if (!obj.isEmpty()) {
                    msg = obj + QStringLiteral(": ") + msg;
                }
                if (m_messages.size() < 200) {
                    m_messages << scrub(msg);
                }
            }
            return;
        }
    }
    // Plain-text log line (commands that run without --use-json-log).
    static const QRegularExpression plain(QStringLiteral(R"(^\d{4}/\d\d/\d\d \d\d:\d\d:\d\d (?:ERROR|CRITICAL)\s*:\s*(.*)$)"));
    const QRegularExpressionMatch m = plain.match(QString::fromUtf8(line));
    if (m.hasMatch() && m_messages.size() < 200) {
        m_messages << scrub(m.captured(1));
    }
}

void RcloneJob::finish(RcloneResult::Status status) {
    if (m_done) {
        return;
    }
    m_done = true;
    m_totalTimer.stop();
    m_idleTimer.stop();
    m_killTimer.stop();
    if (m_proc) {
        // QProcess can report "finished" before the last readyRead was delivered.
        const QByteArray tailOut = m_proc->readAllStandardOutput();
        if (m_out.size() + tailOut.size() <= kMaxCapturedBytes) {
            m_out += tailOut;
        }
        m_errBuf += m_proc->readAllStandardError();
        int nl;
        while ((nl = m_errBuf.indexOf('\n')) >= 0) {
            const QByteArray line = m_errBuf.left(nl).trimmed();
            m_errBuf.remove(0, nl + 1);
            if (!line.isEmpty()) {
                parseLogLine(line);
            }
        }
        if (!m_errBuf.trimmed().isEmpty()) {
            parseLogLine(m_errBuf.trimmed());
        }
    }
    RcloneResult r;
    r.out = m_out;
    r.messages = m_messages;
    r.exitCode = m_proc ? m_proc->exitCode() : -1;
    if (m_startFailed) {
        r.status = RcloneResult::Status::NotStarted;
    } else if (m_timedOut) {
        r.status = RcloneResult::Status::TimedOut;
    } else if (m_cancelled) {
        r.status = RcloneResult::Status::Cancelled;
    } else if (status == RcloneResult::Status::Failed && m_proc && m_proc->exitStatus() == QProcess::NormalExit &&
               r.exitCode == 0) {
        r.status = RcloneResult::Status::Ok;
    } else {
        r.status = RcloneResult::Status::Failed;
    }
    if (r.status == RcloneResult::Status::Failed) {
        r.kind = classify(r.messages, r.exitCode);
    }
    emit finished(r);
}
