#include "VideoApplyTelemetry.h"

#include <QFileInfo>
#include <algorithm>

namespace {
constexpr int kMaxMeta = 8;

// A file stat can stall on slow/UNC/removable paths, so it is skipped when the default sink would drop the
// record anyway (no consent). An injected sink (tests) always gets the fields.
bool wantSourceStat(bool customSink) {
    return customSink || PerfProbe::instance().consent();
}

VideoApplyTelemetry::SourceInfo defaultStat(const QString& source) {
    const QFileInfo fi(source);
    return {fi.suffix(), fi.size()};
}
} // namespace

VideoApplyTelemetry::SourceInfo VideoApplyTelemetry::statSource(const QString& source) const {
    return m_stat(source);
}

VideoApplyTelemetry::VideoApplyTelemetry(Sink sink, StatFn stat) : m_sink(std::move(sink)), m_stat(std::move(stat)) {
    m_customSink = bool(m_sink);
    if (!m_stat) {
        m_stat = defaultStat;
    }
    if (!m_sink) {
        m_sink = [](const PerfProbe::Event& e) { PerfProbe::instance().record(e); };
    }
}

void VideoApplyTelemetry::emitEvent(PerfProbe::Event e) const {
    e.mediaKind = QStringLiteral("video");
    m_sink(e);
}

void VideoApplyTelemetry::onVideoLoaded(quint64 loadSeq, int w, int h, double fps, const QString& codec,
                                        const QString& origin, const QString& source) {
    Meta m;
    m.w = w;
    m.h = h;
    m.fps = fps;
    m.codec = codec.isEmpty() ? QStringLiteral("unknown") : codec;
    if (m_meta.size() >= kMaxMeta) {
        m_meta.remove(*std::min_element(m_meta.keyBegin(), m_meta.keyEnd()));
    }
    m_meta.insert(loadSeq, m);

    PerfProbe::Event e;
    e.stage = QStringLiteral("loaded");
    e.origin = origin;
    e.width = w;
    e.height = h;
    e.fps = fps;
    e.codec = m.codec;
    if (wantSourceStat(m_customSink)) {
        const SourceInfo si = statSource(source);
        e.format = si.format;
        e.bytes = si.bytes;
    }
    emitEvent(e); // PerfProbe de-duplicates identical media; origin is not part of identity
}

quint64 VideoApplyTelemetry::begin(const QString& source) {
    if (isPending()) {
        resolve(QStringLiteral("cancelled"), QStringLiteral("superseded"));
    }
    m_pending = Pending();
    m_pending.token = m_nextToken++;
    if (wantSourceStat(m_customSink)) {
        const SourceInfo si = statSource(source);
        m_pending.format = si.format;
        m_pending.bytes = si.bytes;
    }
    m_pending.clock.start();
    return m_pending.token;
}

void VideoApplyTelemetry::bindLoad(quint64 token, quint64 loadSeq) {
    if (token == m_pending.token && token != 0) {
        m_pending.loadSeq = loadSeq;
    }
}

void VideoApplyTelemetry::resolve(const QString& stage, const QString& reason) {
    const Pending p = m_pending;
    m_pending = Pending(); // cleared first: exactly-once even if the sink re-enters
    Meta m; // unknown when the load's metadata never arrived (or was evicted)
    const auto it = m_meta.constFind(p.loadSeq);
    if (p.loadSeq != 0 && it != m_meta.constEnd()) {
        m = it.value();
    }
    PerfProbe::Event e;
    e.stage = stage;
    e.format = p.format;
    e.bytes = p.bytes;
    e.width = m.w;
    e.height = m.h;
    e.fps = m.fps;
    e.codec = m.codec;
    e.ms = int(qMin<qint64>(p.clock.elapsed(), 600000));
    e.ok = (stage == QLatin1String("applied"));
    e.errorClass = e.ok ? QString() : reason;
    e.alwaysRecord = true; // identical applications each produce their own record
    emitEvent(e);
}

void VideoApplyTelemetry::verified(quint64 token, quint64 currentLoadSeq) {
    if (token == 0 || token != m_pending.token) {
        return;
    }
    if (m_pending.loadSeq != 0 && currentLoadSeq != m_pending.loadSeq) {
        resolve(QStringLiteral("cancelled"), QStringLiteral("superseded_by_load"));
        return;
    }
    resolve(QStringLiteral("applied"), QString());
}

void VideoApplyTelemetry::failed(quint64 token, const QString& reason, quint64 currentLoadSeq) {
    if (token == 0 || token != m_pending.token) {
        return;
    }
    if (m_pending.loadSeq != 0 && currentLoadSeq != 0 && currentLoadSeq != m_pending.loadSeq) {
        resolve(QStringLiteral("cancelled"), QStringLiteral("superseded_by_load"));
        return;
    }
    resolve(QStringLiteral("failed"), reason);
}

void VideoApplyTelemetry::cancelPending(const QString& reason) {
    if (isPending()) {
        resolve(QStringLiteral("cancelled"), reason);
    }
}

void VideoApplyTelemetry::failedForLoad(quint64 token, quint64 errorLoadSeq, const QString& reason) {
    if (token == 0 || token != m_pending.token || m_pending.loadSeq == 0 || errorLoadSeq != m_pending.loadSeq) {
        return; // stale or unbound: cannot be attributed to the pending application
    }
    resolve(QStringLiteral("failed"), reason);
}

void VideoApplyTelemetry::expire(quint64 token, const QString& reason) {
    if (token != 0 && token == m_pending.token) {
        resolve(QStringLiteral("cancelled"), reason);
    }
}
