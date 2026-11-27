#include "VideoPlayer.h"
#include "StartupDiagnostics.h"
#include "D3DWallpaperRenderer.h"
#include <QFileInfo>
#include <QVideoFrame>
#include <QImageReader>
#include <QMediaFormat>
#include <QMimeType>
#include <QPainter>
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

namespace {
bool isGifPath(const QString& path) {
    return QFileInfo(path).suffix().compare(QLatin1String("gif"), Qt::CaseInsensitive) == 0;
}

// D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION - the largest texture side
// D3DWallpaperRenderer can create on a feature-level-11 device. A still
// image larger than this is decoded already downscaled (aspect preserved)
// instead of failing CreateTexture2D later on the render thread.
constexpr int kMaxTextureDimension = 16384;
} // namespace

const QSet<QString>& VideoPlayer::supportedStaticImageExtensions() {
    static const QSet<QString> cached = [] {
        // Formats the deployed image plugins can decode but which are not
        // treated as wallpaper images:
        //  - gif: keeps its existing QMovie (AnimatedImage) path.
        //  - svg/svgz: vector, no intrinsic pixel resolution.
        //  - ico/cur/icns: icon containers (multi-size glyphs, not photos).
        static const QSet<QString> kExcluded = {
            QStringLiteral("gif"), QStringLiteral("svg"), QStringLiteral("svgz"),
            QStringLiteral("ico"), QStringLiteral("cur"), QStringLiteral("icns")};
        QSet<QString> exts;
        for (const QByteArray& format : QImageReader::supportedImageFormats()) {
            const QString ext = QString::fromLatin1(format).toLower();
            if (!kExcluded.contains(ext)) {
                exts.insert(ext);
            }
        }
        qInfo() << "[Media] Still-image formats decodable by this build:" << exts.values();
        return exts;
    }();
    return cached;
}

// Derived from the actual installed Qt Multimedia backend's own reported
// decode capability (QMediaFormat::supportedFileFormats), NOT a hardcoded
// guess list - see the "Expand Motiva Media File Support" task. Filtered
// to the video-container formats only (the enum also lists audio-only
// containers like MP3/AAC/WAV, which aren't relevant here). Computed once
// and cached: the installed backend doesn't change at runtime, and this
// is called from hot paths (drag-over, every dropped file).
const QSet<QString>& VideoPlayer::supportedVideoExtensions() {
    static const QSet<QString> cached = [] {
        QSet<QString> exts;
        QMediaFormat probe;
        const QList<QMediaFormat::FileFormat> formats = probe.supportedFileFormats(QMediaFormat::Decode);
        for (QMediaFormat::FileFormat format : formats) {
            switch (format) {
            case QMediaFormat::WMV:
            case QMediaFormat::AVI:
            case QMediaFormat::Matroska:
            case QMediaFormat::MPEG4:
            case QMediaFormat::Ogg:
            case QMediaFormat::QuickTime:
            case QMediaFormat::WebM:
                break;
            default:
                continue; // audio-only container (MP3/AAC/FLAC/WAV/...) - not a video format.
            }
            QMediaFormat mf(format);
#if QT_CONFIG(mimetype)
            for (const QString& suffix : mf.mimeType().suffixes()) {
                exts.insert(suffix.toLower());
            }
#endif
        }
        // Defensive fallback only - every backend build tested so far
        // already reports mp4 via QMediaFormat::MPEG4's mime type, but if
        // some future/stripped backend build ever reported zero decodable
        // formats, this must not silently regress to "nothing works".
        if (exts.isEmpty()) {
            exts.insert(QStringLiteral("mp4"));
        }
        return exts;
    }();
    return cached;
}

bool VideoPlayer::hasVideoExtension(const QString& fileNameOrPath) {
    return supportedVideoExtensions().contains(QFileInfo(fileNameOrPath).suffix().toLower());
}

bool VideoPlayer::hasStaticImageExtension(const QString& fileNameOrPath) {
    return supportedStaticImageExtensions().contains(QFileInfo(fileNameOrPath).suffix().toLower());
}

VideoPlayer::VideoPlayer(QObject* parent) : QObject(parent), m_movie(this) {
    m_player.setAudioOutput(&m_audioOutput);
    m_player.setVideoSink(&m_sink);
    m_audioOutput.setVolume(0.0);
    m_audioOutput.setMuted(true);

    connect(&m_sink, &QVideoSink::videoFrameChanged, this, &VideoPlayer::onVideoFrameChanged);
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, &VideoPlayer::onMediaStatusChanged);
    connect(&m_player, &QMediaPlayer::errorOccurred, this, &VideoPlayer::onErrorOccurred);
    connect(&m_conversionWatcher, &QFutureWatcher<QImage>::finished, this, &VideoPlayer::onConversionFinished);
    connect(&m_movie, &QMovie::frameChanged, this, &VideoPlayer::onGifFrameChanged);
    connect(&m_movie, &QMovie::finished, this, &VideoPlayer::onGifFinished);
    // Procedural frames are paced by this timer (~30 fps); it only runs
    // while a procedural source is loaded, playing and not suspended.
    m_rainTimer.setInterval(33);
    m_rainTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_rainTimer, &QTimer::timeout, this, &VideoPlayer::onProceduralTick);
}

QString VideoPlayer::binaryRainSource() {
    return QStringLiteral("motiva-procedural:binary-rain");
}

bool VideoPlayer::isProceduralSource(const QString& source) {
    return source == binaryRainSource();
}

QSize VideoPlayer::proceduralFrameSize(const QSize& monitorSize) {
    // Aspect ratio of the target monitor, long side capped at 1920 px: the
    // renderer upsamples, so a 4K display costs the same CPU as 1080p.
    QSize s = monitorSize.isValid() && !monitorSize.isEmpty() ? monitorSize : QSize(1920, 1080);
    const int longest = qMax(s.width(), s.height());
    if (longest > 1920) {
        const double k = 1920.0 / longest;
        s = QSize(qMax(16, int(std::lround(s.width() * k))), qMax(16, int(std::lround(s.height() * k))));
    }
    return s;
}

void VideoPlayer::setBinaryRainConfig(const BinaryRain::Config& config) {
    m_rainConfig = config;
    m_rainConfig.validate();
    if (m_rainEngine) {
        m_rainEngine->setParams(m_rainConfig.params);
        if (m_mediaKind == MediaKind::Procedural && !m_rainTimer.isActive()) {
            renderProceduralFrame(false); // paused: show the change right away
        }
    }
}

void VideoPlayer::setProceduralTargetSize(const QSize& monitorSize) {
    if (monitorSize == m_rainTargetMonitor) {
        return;
    }
    m_rainTargetMonitor = monitorSize;
    if (m_rainEngine) {
        m_rainEngine->resize(proceduralFrameSize(monitorSize));
        if (m_mediaKind == MediaKind::Procedural && !m_rainTimer.isActive()) {
            renderProceduralFrame(false);
        }
    }
}

void VideoPlayer::setProceduralSuspended(bool suspended) {
    if (m_rainSuspended == suspended) {
        return;
    }
    m_rainSuspended = suspended;
    updateProceduralTimer();
}

void VideoPlayer::startProcedural() {
    m_player.setSource(QUrl());
    m_movie.setFileName(QString());
    m_mediaKind = MediaKind::Procedural;
    if (!m_rainEngine) {
        m_rainEngine = std::make_unique<BinaryRainEngine>();
    }
    m_rainEngine->setParams(m_rainConfig.params);
    m_rainEngine->resize(proceduralFrameSize(m_rainTargetMonitor));
    m_rainRunning = false;
    m_lastFrameSize = m_rainEngine->size();
    StartupDiagnostics::instance().mark("videoFileOpened");
    renderProceduralFrame(false); // first frame so the preview is never empty
    emit mediaChanged();
}

void VideoPlayer::updateProceduralTimer() {
    const bool run = m_mediaKind == MediaKind::Procedural && m_rainRunning && !m_rainSuspended;
    if (run && !m_rainTimer.isActive()) {
        m_rainClock.start();
        m_rainTimer.start();
    } else if (!run && m_rainTimer.isActive()) {
        m_rainTimer.stop();
    }
}

void VideoPlayer::onProceduralTick() {
    renderProceduralFrame(true);
}

void VideoPlayer::renderProceduralFrame(bool advanceTime) {
    if (m_mediaKind != MediaKind::Procedural || !m_rainEngine) {
        return;
    }
    if (advanceTime) {
        const double dt = m_rainClock.restart() / 1000.0;
        m_rainEngine->advance(dt);
    }
    // No free buffer = consumers are still busy with earlier frames: skip
    // this frame instead of allocating or queueing more work.
    std::shared_ptr<QImage> buffer = m_rainPool.acquire(m_rainEngine->size());
    if (!buffer || !m_rainEngine->render(*buffer)) {
        return;
    }
    m_lastFrameSize = buffer->size();
    m_currentFrame = std::shared_ptr<const QImage>(buffer);
    emit frameReady(m_currentFrame);
}

QString VideoPlayer::originToken(LoadOrigin o) {
    switch (o) {
    case LoadOrigin::StartupPreview: return QStringLiteral("startup_preview");
    case LoadOrigin::UserSelection: return QStringLiteral("user_selection");
    case LoadOrigin::WallpaperApply: return QStringLiteral("wallpaper_apply");
    case LoadOrigin::Playlist: return QStringLiteral("playlist");
    }
    return QStringLiteral("user_selection");
}

bool VideoPlayer::isVideoSource(const QString& pathOrUrl) {
    if (isProceduralSource(pathOrUrl)) {
        return false;
    }
    const QUrl u(pathOrUrl);
    if (u.isValid() && (u.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0 ||
                        u.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0)) {
        return true;
    }
    return !isGifPath(pathOrUrl) && !hasStaticImageExtension(pathOrUrl);
}

namespace {
struct SyncCallScope {
    int& depth;
    explicit SyncCallScope(int& d) : depth(d) { ++depth; }
    ~SyncCallScope() { --depth; }
    SyncCallScope(const SyncCallScope&) = delete;
    SyncCallScope& operator=(const SyncCallScope&) = delete;
};
} // namespace

// LIMITATION (verified against the Qt 6.12 headers, not the backend sources): QMediaPlayer::errorOccurred carries
// only (Error, QString) - no source, serial or load identity - and QMediaPlayer::source() at delivery time is the
// CURRENT source, so an error cannot be matched to the load that raised it. The FFmpeg backend loads media
// asynchronously (setMediaAsync + CancelToken), so an old load can complete after loadFile() has queued its fence.
// What this function can and cannot decide:
//  - inside our own synchronous call (m_syncCallDepth > 0): reliably the current load;
//  - delivered from the event loop at/after the fence (m_fenceSeq >= m_loadSeq): attributed to the current load by
//    ORDER ONLY. Best effort: an old load's completion posted after the fence is indistinguishable;
//  - delivered from the event loop before the fence: provably queued before the latest load began, so stale.
void VideoPlayer::emitPlayerError(const QString& message) {
    if (m_syncCallDepth > 0 || m_fenceSeq >= m_loadSeq) {
        emitError(message);
        return;
    }
    qInfo() << "[Media] Ignoring a player error queued before load" << m_loadSeq << "(it belongs to an earlier load)";
    emit loadError(m_fenceSeq, message); // stale stamp: telemetry rejects it; the UI never sees it
}

bool VideoPlayer::loadFile(const QString& pathOrUrl, LoadOrigin origin) {
    ++m_loadSeq;
    // Fence: anything already queued for this object was raised by an earlier load and is delivered
    // before this event (same thread, FIFO); anything raised from now on is delivered after it.
    QMetaObject::invokeMethod(this, [this, seq = m_loadSeq] { m_fenceSeq = qMax(m_fenceSeq, seq); },
                              Qt::QueuedConnection);
    SyncCallScope loadScope(m_syncCallDepth);
    m_loadOrigin = origin;
    m_loadSource = pathOrUrl;
    // QMediaPlayer already natively supports a network QUrl as its
    // source (same FFmpeg-backed decode pipeline, no separate networking
    // code needed) - the only thing this function needs to do
    // differently for a web video is skip the local-existence check and
    // pass the URL straight through instead of QUrl::fromLocalFile. See
    // MainWindow::isUsableVideoSource for the matching web-URL
    // recognition used before a source ever reaches here.
    const QUrl asUrl(pathOrUrl);
    const bool isRemote = asUrl.isValid() &&
        (asUrl.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0 ||
         asUrl.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0);

    // Same still image, unchanged on disk, already decoded: reuse the frame.
    // Choosing an image decodes it for the preview and WallpaperManager then
    // loads the same path again - for a 4K PNG that second decode was ~400 ms
    // of GUI-thread time with an identical result.
    if (!isRemote && m_mediaKind == MediaKind::StaticImage && m_currentFrame && !m_stillPath.isEmpty() &&
        pathOrUrl == m_stillPath) {
        const QFileInfo cur(pathOrUrl);
        if (cur.exists() && cur.size() == m_stillSize && cur.lastModified() == m_stillModified) {
            qInfo() << "[Media] Reusing decoded still image" << pathOrUrl;
            emit mediaChanged();
            return true;
        }
    }

    // Always stop both pipelines before (re)loading, regardless of which
    // one the new source will use - only one is ever meant to be running
    // at a time, and switching from a GIF to a video (or back) must not
    // leave the previous one still ticking in the background.
    m_player.stop();
    m_movie.stop();
    releaseProcedural();
    m_currentFrame.reset();
    m_pendingFrame = QVideoFrame();
    m_hasPendingFrame = false;

    if (isProceduralSource(pathOrUrl)) {
        startProcedural();
        return true;
    }

    // GIF is never a QMediaPlayer source (Qt Multimedia doesn't treat it
    // as a video container at all) - local-file-only, no networking code
    // was added for a remote .gif URL (see MainWindow::isWebVideoUrl,
    // which deliberately excludes .gif).
    if (!isRemote && isGifPath(pathOrUrl)) {
        return startMovie(pathOrUrl);
    }

    // Still images are local-file-only too (no web image URLs - see
    // MainWindow::isWebVideoUrl). An image file that is actually animated
    // (e.g. animated WebP) goes through the same QMovie path GIF uses
    // rather than being flattened to its first frame.
    if (!isRemote && hasStaticImageExtension(pathOrUrl)) {
        QImageReader probe(pathOrUrl);
        if (probe.supportsAnimation() && probe.imageCount() > 1 &&
            QMovie::supportedFormats().contains(probe.format())) {
            qInfo() << "[Media] Animated" << probe.format() << "image - using the QMovie (GIF) path:" << pathOrUrl;
            return startMovie(pathOrUrl);
        }
        return loadStaticImage(pathOrUrl);
    }

    m_mediaKind = MediaKind::Video;

    QUrl source;
    if (isRemote) {
        source = asUrl;
    } else {
        QFileInfo fi(pathOrUrl);
        if (!fi.exists() || !fi.isFile()) {
            m_mediaKind = MediaKind::None;
            emitError(tr("Wallpaper video could not be found."));
            return false;
        }
        source = QUrl::fromLocalFile(pathOrUrl);
    }

    m_metadataReported = false;
    m_player.setSource(source);
    StartupDiagnostics::instance().mark("videoFileOpened");
    emit mediaChanged();
    return true;
}

bool VideoPlayer::startMovie(const QString& path) {
    // A previous video's decoder input is released rather than left
    // loaded-but-stopped behind the QMovie.
    m_player.setSource(QUrl());
    m_mediaKind = MediaKind::AnimatedImage;
    m_movie.setFileName(path);
    if (!m_movie.isValid()) {
        m_mediaKind = MediaKind::None;
        emitError(tr("This media format isn't supported by the current playback backend."));
        return false;
    }
    // QMovie::loopCount() is read-only in this Qt version (reflects
    // only the GIF's own embedded NETSCAPE loop-count extension, no
    // setter to override it) - restarting on finished() instead is
    // how this mirrors setLooping()'s existing QMediaPlayer::
    // setLoops(Infinite/1) semantics: "Loop video" governs GIF
    // playback the same way it already governs video playback, one
    // consistent setting, regardless of what a given GIF file's own
    // metadata says. Purely event-driven (QMovie's own finished()
    // signal), not a timer/poll.
    m_movie.start();
    StartupDiagnostics::instance().mark("videoFileOpened");
    emit mediaChanged();
    return true;
}

bool VideoPlayer::loadStaticImage(const QString& path) {
    m_player.setSource(QUrl());
    m_movie.setFileName(QString());
    m_mediaKind = MediaKind::None;

    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        emitError(tr("Wallpaper image could not be found."));
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    QImageReader reader(path);
    // Honors EXIF orientation, so a portrait phone photo stored as a
    // rotated landscape JPEG is shown upright.
    reader.setAutoTransform(true);
    const bool rotated90 = reader.transformation() & QImageIOHandler::TransformationRotate90;
    QSize decodeSize = reader.size();
    if (decodeSize.isValid()) {
        // Keep within the GPU texture limit; aspect ratio preserved.
        // setScaledSize() is in the file's stored orientation (applied
        // before the EXIF transform), and so is reader.size().
        const int longest = qMax(decodeSize.width(), decodeSize.height());
        if (longest > kMaxTextureDimension) {
            const double scale = static_cast<double>(kMaxTextureDimension) / longest;
            decodeSize = QSize(qMax(1, static_cast<int>(std::floor(decodeSize.width() * scale))),
                               qMax(1, static_cast<int>(std::floor(decodeSize.height() * scale))));
            reader.setScaledSize(decodeSize);
            qInfo() << "[Media] Image exceeds" << kMaxTextureDimension << "px - decoding downscaled to"
                    << (rotated90 ? decodeSize.transposed() : decodeSize);
        }
    }

    // QImageReader's default 256 MB allocation guard rejects legitimately
    // large photos (e.g. a 9000x8000 panorama); allow up to the size a
    // 16384x16384 RGB32 texture needs for this one read, then restore it.
    const int previousLimit = QImageReader::allocationLimit();
    QImageReader::setAllocationLimit(qMax(previousLimit, 1024));
    QImage img = reader.read();
    QImageReader::setAllocationLimit(previousLimit);

    if (img.isNull()) {
        qWarning() << "[Media] Failed to decode image" << path << "-" << reader.errorString();
        emitError(tr("Unable to open this image. The file may be corrupted or unsupported."));
        return false;
    }

    // Same RGB32 normalization every other frame source performs. Images
    // with transparency are composited onto black first - the renderer's
    // own clear/letterbox color - since RGB32 has no alpha and a straight
    // conversion would expose whatever color values transparent pixels
    // happen to carry.
    if (img.hasAlphaChannel()) {
        QImage opaque(img.size(), QImage::Format_RGB32);
        opaque.fill(Qt::black);
        QPainter painter(&opaque);
        painter.drawImage(0, 0, img);
        painter.end();
        img = std::move(opaque);
    } else if (img.format() != QImage::Format_RGB32) {
        img = img.convertToFormat(QImage::Format_RGB32);
    }

    m_mediaKind = MediaKind::StaticImage;
    m_lastFrameSize = img.size();
    // Lets the renderer crossfade when one still image replaces another
    // (see D3DWallpaperRenderer::tagAsStill) - video/GIF frames stay untagged.
    D3DWallpaperRenderer::tagAsStill(img);
    m_currentFrame = std::make_shared<const QImage>(std::move(img));
    m_stillPath = path;
    m_stillSize = fi.size();
    m_stillModified = fi.lastModified();
    qInfo() << "[Media] Loaded still image" << path << "size=" << m_lastFrameSize
            << "decode took" << timer.elapsed() << "ms";
    StartupDiagnostics::instance().mark("videoFileOpened");
    StartupDiagnostics::instance().mark("firstFrameDecoded");
    emit mediaChanged();
    // Not published here: play() publishes it, after callers have created
    // whatever render windows should receive it (see the class comment).
    return true;
}

void VideoPlayer::releaseProcedural() {
    m_rainTimer.stop();
    m_rainRunning = false;
    m_rainEngine.reset();
    m_rainPool.clear();
}

void VideoPlayer::play() {
    if (m_mediaKind == MediaKind::Procedural) {
        m_rainRunning = true;
        updateProceduralTimer();
        if (m_currentFrame) {
            emit frameReady(m_currentFrame); // new windows receive the current frame
        }
        return;
    }
    if (m_mediaKind == MediaKind::StaticImage) {
        // No playback clock - (re)publish the one decoded frame to every
        // current receiver (preview + any WallpaperWindow created since
        // loadFile()). See the class comment.
        if (m_currentFrame) {
            emit frameReady(m_currentFrame);
        }
        return;
    }
    if (m_mediaKind == MediaKind::AnimatedImage) {
        m_movie.setPaused(false);
        return;
    }
    SyncCallScope scope(m_syncCallDepth); // errors raised synchronously by play() belong to the current load
    m_player.play();
}

void VideoPlayer::pause() {
    if (m_mediaKind == MediaKind::Procedural) {
        m_rainRunning = false;
        updateProceduralTimer();
        return;
    }
    if (m_mediaKind == MediaKind::StaticImage) {
        return;
    }
    if (m_mediaKind == MediaKind::AnimatedImage) {
        m_movie.setPaused(true);
        return;
    }
    SyncCallScope scope(m_syncCallDepth);
    m_player.pause();
}

void VideoPlayer::stop() {
    if (m_mediaKind == MediaKind::Procedural) {
        m_rainRunning = false;
        updateProceduralTimer();
        return;
    }
    if (m_mediaKind == MediaKind::StaticImage) {
        // Nothing is running; the decoded image is kept so a later play()
        // (e.g. Set as Wallpaper again after Remove Wallpaper) can show it.
        return;
    }
    if (m_mediaKind == MediaKind::AnimatedImage) {
        m_movie.stop();
        return;
    }
    m_player.stop();
}

void VideoPlayer::unload() {
    releaseProcedural();
    m_player.stop();
    m_player.setSource(QUrl());
    m_movie.stop();
    m_movie.setFileName(QString());
    m_currentFrame.reset();
    m_pendingFrame = QVideoFrame();
    m_hasPendingFrame = false;
    m_lastFrameSize = QSize();
    m_mediaKind = MediaKind::None;
    emit mediaChanged();
}

void VideoPlayer::setLooping(bool loop) {
    m_looping = loop;
    applyLoopMode();
}

void VideoPlayer::setSequencedPlayback(bool sequenced) {
    if (sequenced == m_sequenced) {
        return;
    }
    m_sequenced = sequenced;
    applyLoopMode();
}

void VideoPlayer::applyLoopMode() {
    // While a video playlist drives playback, each video plays exactly once
    // so EndOfMedia fires and the playlist can move on; the user's "Loop
    // video" preference (m_looping) is kept and decides whether the
    // playlist itself wraps around (see MainWindow::onPlayerEndOfMedia).
    SyncCallScope scope(m_syncCallDepth);
    m_player.setLoops((m_looping && !m_sequenced) ? QMediaPlayer::Infinite : 1);
}

void VideoPlayer::restartFromBeginning() {
    if (m_mediaKind != MediaKind::Video) {
        return;
    }
    SyncCallScope scope(m_syncCallDepth);
    m_player.setPosition(0);
    m_player.play();
}

void VideoPlayer::setVolume(int percent) {
    m_volumePercent = qBound(0, percent, 100);
    m_audioOutput.setVolume(m_muted ? 0.0f : m_volumePercent / 100.0f);
}

void VideoPlayer::setMuted(bool muted) {
    m_muted = muted;
    m_audioOutput.setMuted(muted);
    m_audioOutput.setVolume(muted ? 0.0f : m_volumePercent / 100.0f);
}

bool VideoPlayer::isPlaying() const {
    if (m_mediaKind == MediaKind::StaticImage || m_mediaKind == MediaKind::None) {
        return false;
    }
    if (m_mediaKind == MediaKind::AnimatedImage) {
        return m_movie.state() == QMovie::Running;
    }
    if (m_mediaKind == MediaKind::Procedural) {
        return m_rainRunning;
    }
    return m_player.playbackState() == QMediaPlayer::PlayingState;
}

void VideoPlayer::onVideoFrameChanged(const QVideoFrame& frame) {
    // A late frame from a video that was just replaced by a GIF/image must
    // not overwrite the new source's frame.
    if (!frame.isValid() || m_mediaKind != MediaKind::Video) {
        return;
    }
    StartupDiagnostics::instance().mark("firstFrameDecoded");
    if (m_conversionInFlight) {
        // A conversion is already running on the thread pool. Only the
        // most recent frame matters for a live wallpaper - queuing every
        // intermediate frame would just add growing latency under load,
        // not smoother playback. Drop anything older still pending.
        m_pendingFrame = frame;
        m_hasPendingFrame = true;
        return;
    }
    startConversion(frame);
}

void VideoPlayer::startConversion(const QVideoFrame& frame) {
    m_conversionInFlight = true;
    QFuture<QImage> future = QtConcurrent::run([frame]() {
        // frame.toImage() (pixel-format/color-space conversion) and the
        // RGB32 normalization are real CPU work for a 1080p+ frame; doing
        // both here, off the GUI thread, is what keeps window/keyboard
        // input responsive while video plays. Converting to RGB32 once
        // here (rather than per-monitor in each WallpaperWindow's paint)
        // also avoids redundant conversion work when multiple monitors
        // share the same frame.
        QImage img = frame.toImage();
        if (!img.isNull() && img.format() != QImage::Format_RGB32) {
            img = img.convertToFormat(QImage::Format_RGB32);
        }
        return img;
    });
    m_conversionWatcher.setFuture(future);
}

void VideoPlayer::onConversionFinished() {
    m_conversionInFlight = false;
    QImage img = m_conversionWatcher.result();
    // Same guard as onVideoFrameChanged: a conversion that was already in
    // flight when the source switched away from video is discarded - for a
    // still image it would otherwise replace the only frame it ever has.
    if (m_mediaKind != MediaKind::Video) {
        m_hasPendingFrame = false;
        m_pendingFrame = QVideoFrame();
        return;
    }
    if (!img.isNull()) {
        m_lastFrameSize = img.size();
        m_currentFrame = std::make_shared<const QImage>(std::move(img));
        emit frameReady(m_currentFrame);
    }

    if (m_hasPendingFrame) {
        QVideoFrame next = m_pendingFrame;
        m_pendingFrame = QVideoFrame();
        m_hasPendingFrame = false;
        startConversion(next);
    }
}

void VideoPlayer::onGifFrameChanged(int frameNumber) {
    Q_UNUSED(frameNumber);
    QImage img = m_movie.currentImage();
    if (img.isNull()) {
        return;
    }
    // Same RGB32 normalization the video path's startConversion() does -
    // GIF frames are typically small/cheap enough (unlike HD+ video
    // frames) that doing this inline on the GUI thread here, rather than
    // via the QtConcurrent hop the video path uses, is not a responsiveness
    // concern; it keeps this path simple and still emits through the
    // exact same frameReady contract every WallpaperWindow already
    // consumes, regardless of which pipeline produced the frame.
    if (img.format() != QImage::Format_RGB32) {
        img = img.convertToFormat(QImage::Format_RGB32);
    }
    m_lastFrameSize = img.size();
    m_currentFrame = std::make_shared<const QImage>(std::move(img));
    emit frameReady(m_currentFrame);
}

void VideoPlayer::onGifFinished() {
    // Reached only for a GIF whose own embedded loop count is finite (an
    // infinite-loop GIF - by far the common case - never emits finished()
    // at all; QMovie loops it internally on its own, verified live). A
    // single-frame ("static") GIF has nothing to loop - restarting it
    // anyway would just repeatedly re-emit the same frame forever for no
    // visible benefit, wasted work for a file that isn't animated at all.
    if (m_mediaKind == MediaKind::AnimatedImage && m_looping && m_movie.frameCount() > 1) {
        // jumpToFrame(0) + start() rather than relying on any implicit
        // restart - QMovie stops advancing once finished() fires, this is
        // the documented way to explicitly loop it again.
        m_movie.jumpToFrame(0);
        m_movie.start();
    }
}

VideoPlayer::VideoMetadata VideoPlayer::describeMetadata(const QMediaMetaData& meta) {
    VideoMetadata out;
    const QVariant res = meta.value(QMediaMetaData::Resolution);
    if (res.canConvert<QSize>()) {
        const QSize s = res.toSize();
        if (s.width() > 0 && s.height() > 0 && s.width() <= 16384 && s.height() <= 16384) {
            out.width = s.width();
            out.height = s.height();
        }
    }
    const QVariant fpsV = meta.value(QMediaMetaData::VideoFrameRate);
    bool ok = false;
    const double fps = fpsV.isValid() ? fpsV.toDouble(&ok) : 0.0;
    if (ok && std::isfinite(fps) && fps > 0.0 && fps <= 1000.0) {
        out.fps = fps;
    }
    const QVariant codecV = meta.value(QMediaMetaData::VideoCodec);
    bool codecOk = false;
    int codecInt = 0;
    if (codecV.metaType() == QMetaType::fromType<QMediaFormat::VideoCodec>()) {
        codecInt = static_cast<int>(codecV.value<QMediaFormat::VideoCodec>());
        codecOk = true;
    } else if (codecV.isValid()) {
        codecInt = codecV.toInt(&codecOk);
    }
    if (codecOk) {
        switch (static_cast<QMediaFormat::VideoCodec>(codecInt)) {
        case QMediaFormat::VideoCodec::MPEG1: out.codec = QStringLiteral("mpeg1"); break;
        case QMediaFormat::VideoCodec::MPEG2: out.codec = QStringLiteral("mpeg2"); break;
        case QMediaFormat::VideoCodec::MPEG4: out.codec = QStringLiteral("mpeg4"); break;
        case QMediaFormat::VideoCodec::H264: out.codec = QStringLiteral("h264"); break;
        case QMediaFormat::VideoCodec::H265: out.codec = QStringLiteral("h265"); break;
        case QMediaFormat::VideoCodec::VP8: out.codec = QStringLiteral("vp8"); break;
        case QMediaFormat::VideoCodec::VP9: out.codec = QStringLiteral("vp9"); break;
        case QMediaFormat::VideoCodec::AV1: out.codec = QStringLiteral("av1"); break;
        case QMediaFormat::VideoCodec::Theora: out.codec = QStringLiteral("theora"); break;
        case QMediaFormat::VideoCodec::WMV: out.codec = QStringLiteral("wmv"); break;
        case QMediaFormat::VideoCodec::MotionJPEG: out.codec = QStringLiteral("mjpeg"); break;
        default: break; // Unspecified / unknown value -> "unknown"
        }
    }
    return out;
}

void VideoPlayer::onMediaStatusChanged(QMediaPlayer::MediaStatus status) {
    if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
        StartupDiagnostics::instance().mark("decoderReady");
        // Passive read of metadata the player already holds; nothing here touches
        // decoding, rendering or buffering, and it runs once per loaded video.
        if (m_mediaKind == MediaKind::Video && !m_metadataReported) {
            m_metadataReported = true;
            const VideoMetadata md = describeMetadata(m_player.metaData());
            emit videoMetadataAvailable(m_loadSeq, md.width, md.height, md.fps, md.codec, originToken(m_loadOrigin));
        }
    }
    if (status == QMediaPlayer::InvalidMedia) {
        emitPlayerError(tr("Unable to play this video. The file may be corrupted or unsupported."));
    } else if (status == QMediaPlayer::EndOfMedia && m_mediaKind == MediaKind::Video) {
        // Only reachable when the player is not looping infinitely (Loop
        // video off, or sequenced playlist playback) - QMediaPlayer's own
        // event, no position polling.
        emit endOfMedia();
    }
}

void VideoPlayer::onErrorOccurred(QMediaPlayer::Error error, const QString& errorString) {
    if (error == QMediaPlayer::NoError) {
        return;
    }
    emitPlayerError(errorString.isEmpty()
        ? tr("Unable to play this video. The file may be corrupted or unsupported.")
        : errorString);
}
