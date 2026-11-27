#pragma once

#include <QObject>
#include <QUrl>
#include <QImage>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QVideoFrame>
#include <QFutureWatcher>
#include <QMovie>
#include <QElapsedTimer>
#include <QDateTime>
#include <QMediaMetaData>
#include <QTimer>
#include "BinaryRainConfig.h"
#include "BinaryRainEngine.h"
#include <QSet>
#include <QString>
#include <memory>

Q_DECLARE_METATYPE(std::shared_ptr<const QImage>) // needed for the cross-thread frameReady queued connection

// Owns the single decode/playback pipeline for the wallpaper video.
// Decoding happens once regardless of how many monitor windows are
// displaying the wallpaper - each frame is converted to a QImage here and
// shared (by pointer) with every WallpaperWindow, which just paints it
// scaled to its own monitor rect. This avoids redundant decode work.
//
// Also plays animated GIFs (see the "Expand Motiva Media File Support"
// task) via QMovie, which Qt Multimedia's QMediaPlayer does not accept as
// a video source at all - GIF is a raster animation format, not one of
// QMediaFormat's video containers. QMovie is the standard, lightweight Qt
// mechanism for this: it decodes on demand (CacheNone, its own default -
// deliberately not overridden, so a large many-frame GIF doesn't get
// fully decoded into RAM up front), drives its own per-frame timer off
// the GIF's own embedded frame delays (no busy loop), and every decoded
// frame is funneled through the exact same frameReady signal/m_currentFrame
// the QMediaPlayer path already uses - WallpaperWindow/D3DWallpaperRenderer
// need no changes at all to present GIF frames; they only ever see "a new
// QImage arrived", never which pipeline produced it.
//
// Static images (v1.1.0 - see MediaKind::StaticImage) use that same
// contract: decoded once via QImageReader, normalized to RGB32, and
// published through frameReady exactly like a video/GIF frame. The only
// difference is that a still image never produces a "next" frame on its
// own, so play() re-publishes the one decoded frame (see play()) - that is
// how freshly created WallpaperWindows receive it, since they only exist
// after loadFile() has already run (see WallpaperManager::setWallpaper).
class VideoPlayer : public QObject {
    Q_OBJECT
public:
    // What the currently loaded source is, decided once in loadFile().
    // AnimatedImage = anything played through QMovie (GIF, and animated
    // WebP - QMovie::supportedFormats() on this Qt build is "gif webp").
    // Procedural = generated at runtime by BinaryRainEngine (no file).
    enum class MediaKind { None, Video, AnimatedImage, StaticImage, Procedural };

    // Pseudo-source identifying the procedural Binary Rain wallpaper. It
    // flows through the same "source string" plumbing as a file path
    // (settings, restore, Set as Wallpaper) and is never a real file.
    static QString binaryRainSource();
    static bool isProceduralSource(const QString& source);

    explicit VideoPlayer(QObject* parent = nullptr);

    // Lowercase file extensions of still-image formats this build can
    // actually decode, derived from QImageReader::supportedImageFormats()
    // (i.e. from the image plugins really deployed), NOT a hardcoded list.
    // Excludes GIF (it keeps its existing QMovie/AnimatedImage path) and
    // formats that aren't meaningful raster wallpapers (icon containers,
    // vector SVG) - see the .cpp. Requires a QGuiApplication to exist.
    static const QSet<QString>& supportedStaticImageExtensions();
    // Lowercase extensions of the video containers the installed Qt
    // Multimedia backend reports it can decode (QMediaFormat), not a
    // hardcoded list. GIF is not included (it is an AnimatedImage).
    static const QSet<QString>& supportedVideoExtensions();
    static bool hasVideoExtension(const QString& fileNameOrPath);
    static bool hasStaticImageExtension(const QString& fileNameOrPath);

    // Why a load happened (telemetry only; allowlisted tokens via originToken()).
    enum class LoadOrigin { StartupPreview, UserSelection, WallpaperApply, Playlist };
    static QString originToken(LoadOrigin o);
    // True when loadFile() would route this source to the video pipeline
    // (not procedural, GIF or a still image). Pure, no I/O.
    static bool isVideoSource(const QString& pathOrUrl);

    bool loadFile(const QString& path, LoadOrigin origin = LoadOrigin::UserSelection);
    // Monotonic, in-memory only: incremented by every loadFile() call.
    quint64 loadSeq() const { return m_loadSeq; }
    // In-memory only, for deriving coarse buckets (extension, size) at emit time; never serialized.
    const QString& lastLoadSource() const { return m_loadSource; }
    void play();
    void pause();
    void stop();

    void setLooping(bool loop);
    bool isLooping() const { return m_looping; }
    // Video playlist mode: play each video once (ignoring the Loop
    // preference) so endOfMedia() fires and the playlist can advance.
    void setSequencedPlayback(bool sequenced);
    // Seeks a loaded video back to 0 and plays (single-item video playlist
    // that should repeat).
    void restartFromBeginning();

    void setVolume(int percent); // 0-100
    int volume() const { return m_volumePercent; }

    void setMuted(bool muted);

    // Stops playback AND releases the loaded source (decoder input, QMovie
    // file, decoded still image) - used by "Remove Media". stop() alone
    // keeps the source loaded so a later play() can resume it.
    void unload();

    bool isPlaying() const;
    bool isProcedural() const { return m_mediaKind == MediaKind::Procedural; }

    // Binary Rain settings: applied immediately; while paused one frame is
    // re-rendered so the change is visible. Safe to call before loading.
    void setBinaryRainConfig(const BinaryRain::Config& config);
    const BinaryRain::Config& binaryRainConfig() const { return m_rainConfig; }
    // Size of the monitor the frame will mostly be shown on; the engine
    // renders at that aspect ratio, capped to 1920 px on the long side.
    void setProceduralTargetSize(const QSize& monitorSize);
    // Stops frame generation while the wallpaper is hidden (battery policy)
    // without changing the user's play/pause state.
    void setProceduralSuspended(bool suspended);
    MediaKind mediaKind() const { return m_mediaKind; }
    bool isStaticImage() const { return m_mediaKind == MediaKind::StaticImage; }
    QSize videoNativeSize() const { return m_lastFrameSize; }
    // Passthrough to the underlying QMediaPlayer - for UI display only
    // (e.g. showing the loaded video's length), not used by any playback
    // or wallpaper-lifecycle decision. 0 unless the current source is a
    // video (QMediaPlayer keeps a previous video's duration after stop()).
    qint64 durationMs() const { return m_mediaKind == MediaKind::Video ? m_player.duration() : 0; }

    struct VideoMetadata {
        int width = 0;
        int height = 0;
        double fps = 0.0;
        QString codec = QStringLiteral("unknown"); // allowlisted token
    };
    // Pure: turns player metadata into safe values (invalid/missing -> 0 / "unknown").
    static VideoMetadata describeMetadata(const QMediaMetaData& meta);

    // Latest decoded frame, already converted to Format_RGB32 exactly once
    // (not per-monitor), shared by all render windows.
    std::shared_ptr<const QImage> currentFrame() const { return m_currentFrame; }

signals:
    // Carries the frame by value (a shared_ptr copy made here on this
    // (GUI) thread before the signal crosses to each WallpaperWindow's own
    // render thread via a queued connection) rather than requiring
    // receivers to call currentFrame() themselves - that getter is only
    // safe to call from this object's own thread, since m_currentFrame is
    // written here with no synchronization.
    void frameReady(std::shared_ptr<const QImage> frame);
    void errorOccurred(const QString& message);
    // Same error, stamped with the load it belongs to (telemetry correlation; stale ones are rejected by the receiver).
    void loadError(quint64 loadSeq, const QString& message);
    // A new source was successfully loaded (or the current one unloaded) -
    // mediaKind() may have changed. Lets WallpaperManager re-evaluate
    // anything that depends on the media type (the battery policy).
    void mediaChanged();
    // A video played to its end (only when it isn't looping infinitely).
    void endOfMedia();
    // Passive, once per loaded video: coarse stream facts for opt-in performance
    // measurement. Values are normalised (0 / "unknown" when the container does
    // not report them); no path, URL or file name is ever part of it.
    // loadSeq identifies the load the values belong to; origin is an allowlisted token.
    void videoMetadataAvailable(quint64 loadSeq, int width, int height, double fps, const QString& codec,
                                const QString& origin);

private:
    // Error raised by loadFile() itself (synchronous): always belongs to the current load.
    void emitError(const QString& message) { emit loadError(m_loadSeq, message); emit errorOccurred(message); }
    // Error delivered by a QMediaPlayer signal. An error queued before the latest loadFile() (it is
    // delivered before that load's fence event) belongs to an EARLIER load: it is stamped with the last
    // delivered fence (never equal to the current load) and is not reported through errorOccurred, so
    // it can neither fail the current load's telemetry nor the UI/browser request waiting on it.
    void emitPlayerError(const QString& message);
private slots:
    void onVideoFrameChanged(const QVideoFrame& frame);
    void onConversionFinished();
    void onMediaStatusChanged(QMediaPlayer::MediaStatus status);
    void onErrorOccurred(QMediaPlayer::Error error, const QString& errorString);
    // QMovie delivers decoded frames synchronously via this signal
    // (already off any hot path - GIF frames are small/cheap compared to
    // HD video, so no QtConcurrent hop is needed here the way the video
    // path uses one for frame.toImage() - see startConversion).
    void onGifFrameChanged(int frameNumber);
    // QMovie::loopCount() has no setter in this Qt version - restarting
    // here on finished() (only while m_looping) is how GIF playback gets
    // the same "loop forever until told otherwise" behavior setLooping()
    // already gives video via QMediaPlayer::setLoops(Infinite).
    void onGifFinished();
    void onProceduralTick();

private:
    QMediaPlayer m_player;
    QAudioOutput m_audioOutput;
    QVideoSink m_sink;
    QMovie m_movie;
    // Which pipeline the currently-loaded source is playing through -
    // decided once in loadFile(), drives play()/pause()/stop()/isPlaying()
    // below. Both m_player and m_movie always exist; only one is ever
    // actually started for a given loaded source (and neither for a
    // StaticImage, which has no playback clock at all).
    MediaKind m_mediaKind = MediaKind::None;
    bool m_metadataReported = false; // one videoMetadataAvailable per loaded video
    quint64 m_loadSeq = 0;
    quint64 m_fenceSeq = 0;    // highest load whose queued fence event has been delivered
    // > 0 while loadFile() or one of our own calls into QMediaPlayer (play/pause/setLoops/setPosition) is on the
    // stack. A QMediaPlayer signal emitted synchronously inside such a call was raised BY that call on the current
    // media: stale (queued) events are only ever delivered from the event loop, never inside our own call.
    // This is the only identity available here; Qt attaches none to errorOccurred (see emitPlayerError).
    int m_syncCallDepth = 0;
    QString m_loadSource;
    LoadOrigin m_loadOrigin = LoadOrigin::UserSelection;

    bool m_looping = true;
    bool m_sequenced = false;
    void applyLoopMode();
    int m_volumePercent = 0;
    bool m_muted = true;

    QSize m_lastFrameSize;
    std::shared_ptr<const QImage> m_currentFrame;
    // Identity of the decoded still image in m_currentFrame (reuse check in loadFile).
    QString m_stillPath;
    qint64 m_stillSize = 0;
    QDateTime m_stillModified;

    // frame.toImage() + format conversion is real CPU work (color-space
    // conversion of a full HD+ image); doing it inline in the
    // videoFrameChanged slot would block the GUI thread once per decoded
    // frame. It runs on a thread pool thread instead; the watcher's
    // finished() delivery back onto this (GUI) thread is what makes
    // updating m_currentFrame from here safe without extra locking.
    QFutureWatcher<QImage> m_conversionWatcher;
    bool m_conversionInFlight = false;
    QVideoFrame m_pendingFrame; // set only if a new frame arrives while one is still converting
    bool m_hasPendingFrame = false;

    void startConversion(const QVideoFrame& frame);
    bool startMovie(const QString& path);
    bool loadStaticImage(const QString& path);

    // --- Procedural (Binary Rain) ---
    static QSize proceduralFrameSize(const QSize& monitorSize);
    void startProcedural();
    void releaseProcedural();
    void renderProceduralFrame(bool advanceTime);
    void updateProceduralTimer();
    BinaryRain::Config m_rainConfig;
    std::unique_ptr<BinaryRainEngine> m_rainEngine;
    BinaryRainFramePool m_rainPool;
    QTimer m_rainTimer;
    QElapsedTimer m_rainClock;
    QSize m_rainTargetMonitor{1920, 1080};
    bool m_rainRunning = false;   // play state (user pause = false)
    bool m_rainSuspended = false; // hidden by the battery policy
};
