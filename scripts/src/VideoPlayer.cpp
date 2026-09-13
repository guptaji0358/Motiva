#include "VideoPlayer.h"
#include "StartupDiagnostics.h"
#include <QFileInfo>
#include <QVideoFrame>
#include <QtConcurrent/QtConcurrentRun>

namespace {
bool isGifPath(const QString& path) {
    return QFileInfo(path).suffix().compare(QLatin1String("gif"), Qt::CaseInsensitive) == 0;
}
} // namespace

VideoPlayer::VideoPlayer(QObject* parent) : QObject(parent), m_movie(this) {
    m_player.setAudioOutput(&m_audioOutput);
    m_player.setVideoSink(&m_sink);
    m_audioOutput.setVolume(0.0);
    m_audioOutput.setMuted(true);

    connect(&m_sink, &QVideoSink::videoFrameChanged, this, &VideoPlayer::onVideoFrameChanged);
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, &VideoPlayer::onMediaStatusChanged);
    connect(&m_player, &QMediaPlayer::errorOccurred, this, &VideoPlayer::onErrorOccurred);
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, &VideoPlayer::playbackStateChanged);
    connect(&m_conversionWatcher, &QFutureWatcher<QImage>::finished, this, &VideoPlayer::onConversionFinished);
    connect(&m_movie, &QMovie::frameChanged, this, &VideoPlayer::onGifFrameChanged);
    connect(&m_movie, &QMovie::finished, this, &VideoPlayer::onGifFinished);
}

bool VideoPlayer::loadFile(const QString& pathOrUrl) {
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

    // Always stop both pipelines before (re)loading, regardless of which
    // one the new source will use - only one is ever meant to be running
    // at a time, and switching from a GIF to a video (or back) must not
    // leave the previous one still ticking in the background.
    m_player.stop();
    m_movie.stop();
    m_currentFrame.reset();
    m_pendingFrame = QVideoFrame();
    m_hasPendingFrame = false;

    // GIF is never a QMediaPlayer source (Qt Multimedia doesn't treat it
    // as a video container at all) - local-file-only, no networking code
    // was added for a remote .gif URL (see MainWindow::isWebVideoUrl,
    // which deliberately excludes .gif).
    if (!isRemote && isGifPath(pathOrUrl)) {
        m_isGifMode = true;
        m_movie.setFileName(pathOrUrl);
        if (!m_movie.isValid()) {
            emit errorOccurred(tr("This media format isn't supported by the current playback backend."));
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
        return true;
    }

    m_isGifMode = false;

    QUrl source;
    if (isRemote) {
        source = asUrl;
    } else {
        QFileInfo fi(pathOrUrl);
        if (!fi.exists() || !fi.isFile()) {
            emit errorOccurred(tr("Wallpaper video could not be found."));
            return false;
        }
        source = QUrl::fromLocalFile(pathOrUrl);
    }

    m_player.setSource(source);
    StartupDiagnostics::instance().mark("videoFileOpened");
    return true;
}

void VideoPlayer::play() {
    if (m_isGifMode) {
        m_movie.setPaused(false);
        return;
    }
    m_player.play();
}

void VideoPlayer::pause() {
    if (m_isGifMode) {
        m_movie.setPaused(true);
        return;
    }
    m_player.pause();
}

void VideoPlayer::stop() {
    if (m_isGifMode) {
        m_movie.stop();
        return;
    }
    m_player.stop();
}

void VideoPlayer::setLooping(bool loop) {
    m_looping = loop;
    m_player.setLoops(loop ? QMediaPlayer::Infinite : 1);
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
    if (m_isGifMode) {
        return m_movie.state() == QMovie::Running;
    }
    return m_player.playbackState() == QMediaPlayer::PlayingState;
}

void VideoPlayer::onVideoFrameChanged(const QVideoFrame& frame) {
    if (!frame.isValid()) {
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
    if (m_isGifMode && m_looping && m_movie.frameCount() > 1) {
        // jumpToFrame(0) + start() rather than relying on any implicit
        // restart - QMovie stops advancing once finished() fires, this is
        // the documented way to explicitly loop it again.
        m_movie.jumpToFrame(0);
        m_movie.start();
    }
}

void VideoPlayer::onMediaStatusChanged(QMediaPlayer::MediaStatus status) {
    if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
        StartupDiagnostics::instance().mark("decoderReady");
    }
    if (status == QMediaPlayer::InvalidMedia) {
        emit errorOccurred(tr("Unable to play this video. The file may be corrupted or unsupported."));
    } else if (status == QMediaPlayer::EndOfMedia && !m_looping) {
        // QMediaPlayer::setLoops(1) already stops after one play-through.
    }
}

void VideoPlayer::onErrorOccurred(QMediaPlayer::Error error, const QString& errorString) {
    if (error == QMediaPlayer::NoError) {
        return;
    }
    emit errorOccurred(errorString.isEmpty()
        ? tr("Unable to play this video. The file may be corrupted or unsupported.")
        : errorString);
}
