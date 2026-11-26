#include "PerfProbe.h"

#include "ProfileStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dxgi.h>
#endif

namespace {
constexpr int kMaxRecords = 500;
constexpr int kTrimTo = 400;
constexpr int kMaxSeen = 512;

QString filePath(const QString& dir) {
    return dir + QStringLiteral("/perf-records.jsonl");
}
} // namespace

PerfProbe::PerfProbe(QObject* parent) : QObject(parent) {
    m_pool.setMaxThreadCount(1); // serialised, never competes with the GUI thread
}

PerfProbe& PerfProbe::instance() {
    static PerfProbe probe;
    return probe;
}

void PerfProbe::init(const QString& dir, const QString& appVersion) {
    m_dir = dir;
    m_appVersion = appVersion;
    QDir().mkpath(dir);
}

void PerfProbe::setConsent(bool optedIn) {
    const bool was = m_consent.exchange(optedIn);
    if (was && !optedIn) {
        clearAll(); // withdrawing consent discards anything not yet uploaded
    }
}

QString PerfProbe::sanitizeFormat(const QString& ext) {
    static const QSet<QString> ok = {"jpg", "jpeg", "png", "webp", "bmp", "gif", "avif", "mp4", "webm", "mov", "mkv", "m4v"};
    const QString e = ext.toLower();
    return ok.contains(e) ? e : QStringLiteral("other");
}

QString PerfProbe::sanitizeToken(const QString& t) {
    static const QRegularExpression re(QStringLiteral("^[a-z_]{1,24}$"));
    return re.match(t).hasMatch() ? t : QString();
}

QString PerfProbe::sanitizeCodec(const QString& c) {
    static const QSet<QString> ok = {"mpeg1", "mpeg2", "mpeg4", "h264", "h265", "vp8", "vp9", "av1", "theora", "wmv", "mjpeg"};
    const QString l = c.toLower();
    return ok.contains(l) ? l : QStringLiteral("unknown");
}

QString PerfProbe::sanitizeOrigin(const QString& o) {
    static const QSet<QString> ok = {"startup_preview", "user_selection", "wallpaper_apply", "playlist"};
    return ok.contains(o) ? o : QStringLiteral("user_selection");
}

QString PerfProbe::fpsBucket(double fps) {
    if (!(fps > 0.0) || fps > 1000.0) { // also rejects NaN
        return QStringLiteral("unknown");
    }
    if (fps <= 24.5) return QStringLiteral("<=24");
    if (fps <= 30.5) return QStringLiteral("<=30");
    if (fps <= 60.5) return QStringLiteral("<=60");
    return QStringLiteral(">60");
}

QString PerfProbe::sizeBucket(qint64 bytes) {
    if (bytes <= 0) {
        return QStringLiteral("unknown");
    }
    qint64 b = 64 * 1024;
    while (b < bytes && b < (qint64(1) << 40)) {
        b <<= 1;
    }
    return QStringLiteral("<=%1KB").arg(b / 1024);
}

QString PerfProbe::resolutionBucket(int w, int h) {
    const int longest = qMax(w, h);
    if (longest <= 0) {
        return QStringLiteral("unknown");
    }
    if (longest <= 1280) return QStringLiteral("<=720p");
    if (longest <= 1920) return QStringLiteral("<=1080p");
    if (longest <= 2560) return QStringLiteral("<=1440p");
    if (longest <= 3840) return QStringLiteral("<=4k");
    return QStringLiteral(">4k");
}

perf::HardwareClass PerfProbe::detectHardware() {
    perf::HardwareClass hw;
#ifdef Q_OS_WIN
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        hw.ramGb = static_cast<int>((mem.ullTotalPhys + (512ull << 20)) >> 30);
    }
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        // First hardware adapter (skip the software "Basic Render" adapter).
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* a = nullptr;
            if (factory->EnumAdapters1(i, &a) != S_OK) {
                break;
            }
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            a->Release();
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                continue;
            }
            switch (d.VendorId) {
            case 0x10DE: hw.gpuVendor = QStringLiteral("nvidia"); break;
            case 0x1002:
            case 0x1022: hw.gpuVendor = QStringLiteral("amd"); break;
            case 0x8086: hw.gpuVendor = QStringLiteral("intel"); break;
            default: hw.gpuVendor = QStringLiteral("other"); break;
            }
            break;
        }
        factory->Release();
    }
#endif
    return hw;
}

QByteArray PerfProbe::buildLine(const Event& e) const {
    const perf::HardwareClass hw = detectHardware();
    QJsonObject o;
    o["hour"] = QDateTime::currentSecsSinceEpoch() / 3600 * 3600; // coarse time only
    o["kind"] = (e.mediaKind == "image" || e.mediaKind == "video" || e.mediaKind == "gif") ? e.mediaKind : QStringLiteral("other");
    o["fmt"] = sanitizeFormat(e.format);
    o["size"] = sizeBucket(e.bytes);
    o["res"] = resolutionBucket(e.width, e.height);
    static const QSet<QString> stages = {"download", "meta", "loaded", "applied", "failed", "cancelled"};
    o["stage"] = stages.contains(e.stage) ? e.stage : QStringLiteral("apply");
    if (e.stage == "loaded") {
        o["origin"] = sanitizeOrigin(e.origin);
    }
    if (!e.codec.isEmpty()) { // video metadata records only
        o["codec"] = sanitizeCodec(e.codec);
        o["fps"] = fpsBucket(e.fps);
    }
    o["ms"] = qBound(0, e.ms, 600000);
    o["ok"] = e.ok;
    o["err"] = e.ok ? QString() : sanitizeToken(e.errorClass);
    o["pv"] = ProfileStore::instance().activeVersion();
    o["gpu"] = hw.gpuVendor;
    o["ram"] = hw.ramGb;
    o["os"] = QSysInfo::productVersion();
    o["app"] = m_appVersion;
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

void PerfProbe::record(const Event& e) {
    if (!m_consent.load() || m_dir.isEmpty()) {
        return;
    }
    m_pool.start([this, e] {
        if (!m_consent.load()) {
            return;
        }
        // Unchanged media (same kind/format/exact size/stage) is not re-analysed.
        const quint64 key = e.alwaysRecord ? 0 : qHash(QStringLiteral("%1|%2|%3|%4|%5|%6").arg(e.mediaKind, e.format).arg(e.bytes).arg(e.stage).arg(e.width).arg(e.height) + QStringLiteral("|%1|%2").arg(e.codec).arg(qRound(e.fps)));
        {
            QMutexLocker lock(&m_mutex);
            if (e.alwaysRecord) {
                // lifecycle outcome: every application is its own record
            } else if (m_seen.contains(key)) {
                return;
            }
            if (!e.alwaysRecord) {
                if (m_seen.size() >= kMaxSeen) {
                    m_seen.clear();
                }
                m_seen.insert(key);
            }
        }
        append(buildLine(e));
    });
}

QList<PerfProbe::Line> PerfProbe::readLines() {
    QList<Line> out;
    QFile f(filePath(m_dir));
    if (!f.open(QIODevice::ReadOnly)) {
        return out;
    }
    const QByteArray content = f.readAll();
    f.close(); // must be closed before QSaveFile replaces the file (Windows)
    for (const QByteArray& raw : content.split('\n')) {
        if (raw.isEmpty()) {
            continue;
        }
        const int tab = raw.indexOf('\t');
        bool ok = false;
        const quint64 id = tab > 0 ? raw.left(tab).toULongLong(&ok) : 0;
        if (ok && id > 0) {
            out.append(Line{id, raw.mid(tab + 1)});
            m_nextId = qMax(m_nextId, id + 1);
        } else {
            out.append(Line{0, raw}); // legacy line: numbered below once all known ids are seen
        }
    }
    bool migrated = false;
    for (Line& l : out) {
        if (l.id == 0) {
            l.id = m_nextId++;
            migrated = true;
        }
    }
    if (migrated) {
        writeLines(out); // persist the ids so they stay stable across later reads
    }
    return out;
}

void PerfProbe::writeLines(const QList<Line>& lines) {
    QSaveFile out(filePath(m_dir));
    if (!out.open(QIODevice::WriteOnly)) {
        return;
    }
    for (const Line& l : lines) {
        out.write(QByteArray::number(l.id) + '\t' + l.json + '\n');
    }
    out.commit();
}

void PerfProbe::append(const QByteArray& line) {
    QMutexLocker lock(&m_mutex);
    QList<Line> lines = readLines();
    lines.append(Line{m_nextId++, line});
    if (lines.size() > kMaxRecords) {
        lines = lines.mid(lines.size() - kTrimTo);
    }
    writeLines(lines);
}

void PerfProbe::waitForIdle() {
    m_pool.waitForDone();
}

PerfProbe::Batch PerfProbe::peekBatchIds(int maxRecords) {
    QMutexLocker lock(&m_mutex);
    Batch b;
    for (const Line& l : readLines()) {
        if (b.records.size() >= maxRecords) {
            break;
        }
        b.records.append(l.json);
        b.ids.append(l.id);
    }
    return b;
}

QList<QByteArray> PerfProbe::peekBatch(int maxRecords) {
    return peekBatchIds(maxRecords).records;
}

void PerfProbe::acknowledgeIds(const QList<quint64>& ids) {
    QMutexLocker lock(&m_mutex);
    const QSet<quint64> gone(ids.begin(), ids.end());
    QList<Line> lines = readLines();
    QList<Line> keep;
    for (const Line& l : lines) {
        if (!gone.contains(l.id)) {
            keep.append(l);
        }
    }
    if (keep.size() != lines.size()) {
        writeLines(keep);
    }
}

int PerfProbe::pendingCount() {
    return peekBatch(kMaxRecords).size();
}

void PerfProbe::clearAll() {
    m_pool.waitForDone();
    QMutexLocker lock(&m_mutex);
    QFile::remove(filePath(m_dir));
    m_seen.clear();
}

PerfProbe::Event PerfProbe::failedStageEvent(const QString& stage, int ms) {
    Event e;
    e.mediaKind = QStringLiteral("other");
    e.stage = stage;
    e.ms = ms;
    e.ok = false;
    e.errorClass = QStringLiteral("%1_failed").arg(stage);
    return e;
}
