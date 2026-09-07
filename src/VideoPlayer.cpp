#include "VideoPlayer.h"
#include <QFileInfo>
#include <QVideoFrame>

VideoPlayer::VideoPlayer(QObject* parent) : QObject(parent) {
    m_player.setAudioOutput(&m_audioOutput);
    m_player.setVideoSink(&m_sink);
    m_audioOutput.setVolume(0.0);
    m_audioOutput.setMuted(true);

    connect(&m_sink, &QVideoSink::videoFrameChanged, this, &VideoPlayer::onVideoFrameChanged);
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, &VideoPlayer::onMediaStatusChanged);
    connect(&m_player, &QMediaPlayer::errorOccurred, this, &VideoPlayer::onErrorOccurred);
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, &VideoPlayer::playbackStateChanged);
}

bool VideoPlayer::loadFile(const QString& path) {
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        emit errorOccurred(tr("Wallpaper video could not be found."));
        return false;
    }

    m_player.stop();
    m_currentFrame.reset();
    m_player.setSource(QUrl::fromLocalFile(path));
    return true;
}

void VideoPlayer::play() {
    m_player.play();
}

void VideoPlayer::pause() {
    m_player.pause();
}

void VideoPlayer::stop() {
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
    return m_player.playbackState() == QMediaPlayer::PlayingState;
}

void VideoPlayer::onVideoFrameChanged(const QVideoFrame& frame) {
    if (!frame.isValid()) {
        return;
    }
    QImage img = frame.toImage();
    if (img.isNull()) {
        return;
    }
    m_lastFrameSize = img.size();
    m_currentFrame = std::make_shared<const QImage>(std::move(img));
    emit frameReady();
}

void VideoPlayer::onMediaStatusChanged(QMediaPlayer::MediaStatus status) {
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
