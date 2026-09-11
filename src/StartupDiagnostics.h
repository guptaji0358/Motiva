#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QDebug>

// Process-wide checkpoint timeline, in milliseconds since process start.
// Purely additive instrumentation - no behavior changes, no locking beyond
// what recording a handful of named timestamps needs (some marks happen off
// the GUI thread, e.g. inside WindowsDesktopWallpaper::FindOrCreateWorkerW
// on a QtConcurrent worker thread, so a mutex guards the map).
//
// Exists so a single, non-repeatable Windows restart produces enough
// evidence (via %TEMP%\Motiva.log and RecoveryState's startup{}
// block) to diagnose the 30-60s startup video delay without needing to
// reboot again - see VideoWallpaper/CLAUDE.md.
class StartupDiagnostics {
public:
    static StartupDiagnostics& instance() {
        static StartupDiagnostics inst;
        return inst;
    }

    // Must be called exactly once, as the very first line of main(), before
    // anything else (including the log handler) so even early failures get
    // a t=0 reference.
    void start() {
        QMutexLocker lock(&m_mutex);
        m_timer.start();
    }

    // Records the elapsed time (ms since start()) for a named checkpoint,
    // the first time it's reached - later calls with the same name are
    // ignored so a checkpoint that fires repeatedly (e.g. attach retries)
    // still reports "time to FIRST reach this state".
    void mark(const QString& name) {
        qint64 elapsed;
        bool first;
        {
            QMutexLocker lock(&m_mutex);
            if (m_marks.contains(name)) {
                return;
            }
            elapsed = m_timer.isValid() ? m_timer.elapsed() : -1;
            m_marks.insert(name, elapsed);
            first = true;
        }
        if (first) {
            qInfo() << "[Startup]" << name << "at" << elapsed << "ms since process start.";
        }
    }

    qint64 elapsedFor(const QString& name) const {
        QMutexLocker lock(&m_mutex);
        return m_marks.value(name, -1);
    }

    // Builds the small startup{} diagnostics block (raw checkpoints plus
    // the derived deltas) for persisting into RecoveryState and for the
    // final report - computed once first-frame presentation is reached.
    QJsonObject toJsonAndLogDeltas() const {
        QMutexLocker lock(&m_mutex);
        QJsonObject obj;
        for (auto it = m_marks.constBegin(); it != m_marks.constEnd(); ++it) {
            obj[it.key()] = static_cast<double>(it.value());
        }

        auto delta = [this](const char* from, const char* to) -> qint64 {
            const qint64 a = m_marks.value(QString::fromLatin1(from), -1);
            const qint64 b = m_marks.value(QString::fromLatin1(to), -1);
            if (a < 0 || b < 0) {
                return -1;
            }
            return b - a;
        };

        const qint64 toExplorerReady = delta("mainStart", "explorerDetected");
        const qint64 toWallpaperAttached = delta("mainStart", "wallpaperAttached");
        const qint64 toVideoReady = delta("mainStart", "decoderReady");
        const qint64 toFirstFrame = delta("mainStart", "firstFrameDecoded");
        const qint64 toFirstPresentedFrame = delta("mainStart", "firstFramePresented");
        // Isolates exactly how long desktop discovery+reparenting itself
        // took, separate from video/decoder startup - the specific
        // number this instrumentation exists to pin down (see CLAUDE.md
        // "startup delay" entries).
        const qint64 discoveryToAttached = delta("desktopDiscoveryStart", "wallpaperAttached");
        const qint64 attachDuration = delta("attachToDesktopStart", "attachToDesktopEnd");

        obj["startupToExplorerReady"] = static_cast<double>(toExplorerReady);
        obj["startupToWallpaperAttached"] = static_cast<double>(toWallpaperAttached);
        obj["startupToVideoReady"] = static_cast<double>(toVideoReady);
        obj["startupToFirstFrame"] = static_cast<double>(toFirstFrame);
        obj["discoveryToAttached"] = static_cast<double>(discoveryToAttached);
        obj["attachDuration"] = static_cast<double>(attachDuration);
        obj["startupToFirstPresentedFrame"] = static_cast<double>(toFirstPresentedFrame);

        qInfo() << "[Startup] === Startup timing summary ===";
        qInfo() << "[Startup] startupToExplorerReady=" << toExplorerReady << "ms";
        qInfo() << "[Startup] startupToWallpaperAttached=" << toWallpaperAttached << "ms";
        qInfo() << "[Startup] startupToVideoReady=" << toVideoReady << "ms";
        qInfo() << "[Startup] startupToFirstFrame=" << toFirstFrame << "ms";
        qInfo() << "[Startup] startupToFirstPresentedFrame=" << toFirstPresentedFrame << "ms";
        qInfo() << "[Startup] discoveryToAttached=" << discoveryToAttached << "ms (desktop discovery+reparent only)";
        qInfo() << "[Startup] attachDuration=" << attachDuration << "ms (single AttachToDesktop call)";

        return obj;
    }

private:
    StartupDiagnostics() = default;

    mutable QMutex m_mutex;
    QElapsedTimer m_timer;
    QHash<QString, qint64> m_marks;
};
