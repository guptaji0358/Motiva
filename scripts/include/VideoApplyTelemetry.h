#pragma once
// Pure (Qt Core only) lifecycle tracker for opt-in video telemetry. No Win32,
// no wallpaper logic: WallpaperManager/VideoPlayer call into it and it emits
// PerfProbe events through an injectable sink.
//
// Stages: loaded (video decoded far enough to read metadata), applied (attach
// confirmed through wallpaperVerified), failed (terminal application failure),
// cancelled (superseded or removed). At most ONE application is pending; every
// token is resolved exactly once. Tokens and load sequences live in memory only
// and are never serialized. No path/URL/filename ever reaches an event.
#include "PerfProbe.h"

#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <functional>

class VideoApplyTelemetry {
public:
    using Sink = std::function<void(const PerfProbe::Event&)>;
    // Extension + size of the source (the only filesystem access of this class). Injectable so tests can
    // observe whether a stat happens at all.
    struct SourceInfo { QString format; qint64 bytes = 0; };
    using StatFn = std::function<SourceInfo(const QString&)>;
    // `sink` default: PerfProbe::instance().record. With the default sink the stat runs only while the user
    // has opted in (the record would be dropped otherwise); an injected sink always gets the fields.
    explicit VideoApplyTelemetry(Sink sink = {}, StatFn stat = {});

    // `source` is only used to derive extension + size bucket input, then dropped.
    void onVideoLoaded(quint64 loadSeq, int w, int h, double fps, const QString& codec, const QString& origin,
                       const QString& source);

    // Starts a new application (a still-pending one is cancelled "superseded"). Returns its token.
    quint64 begin(const QString& source);
    void bindLoad(quint64 token, quint64 loadSeq); // after VideoPlayer::loadFile()
    bool isPending() const { return m_pending.token != 0; }
    quint64 pendingToken() const { return m_pending.token; }

    // All no-ops unless `token` is the pending one (stale callbacks cannot resolve a newer application).
    void verified(quint64 token, quint64 currentLoadSeq);
    void failed(quint64 token, const QString& reason, quint64 currentLoadSeq = 0);
    // Error stamped with the load it came from (VideoPlayer::loadError). Fails the application only when
    // `token` is pending AND `errorLoadSeq` is the load bound to it; an error of an older/other load is
    // stale and ignored (neither failure nor cancellation: the newer application stays pending).
    void failedForLoad(quint64 token, quint64 errorLoadSeq, const QString& reason);
    // Bounded-pending policy: cancels `token` (only if still the pending one) as e.g. "attach_timeout".
    void expire(quint64 token, const QString& reason);
    // Resolves whatever is pending as cancelled (removal, shutdown, battery_hidden).
    void cancelPending(const QString& reason);

private:
    struct Meta { int w = 0, h = 0; double fps = 0.0; QString codec = QStringLiteral("unknown"); };
    struct Pending {
        quint64 token = 0;
        quint64 loadSeq = 0;
        QString format;
        qint64 bytes = 0;
        QElapsedTimer clock;
    };
    void resolve(const QString& stage, const QString& reason);
    void emitEvent(PerfProbe::Event e) const;

    SourceInfo statSource(const QString& source) const;
    Sink m_sink;
    StatFn m_stat;
    bool m_customSink = false;
    Pending m_pending;
    quint64 m_nextToken = 1;
    QHash<quint64, Meta> m_meta; // bounded, keyed by loadSeq
};
