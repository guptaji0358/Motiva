#pragma once

#include <QObject>
#include <QUrl>
#include <QImage>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <memory>

// Owns the single decode/playback pipeline for the wallpaper video.
// Decoding happens once regardless of how many monitor windows are
// displaying the wallpaper - each frame is converted to a QImage here and
// shared (by pointer) with every WallpaperWindow, which just paints it
// scaled to its own monitor rect. This avoids redundant decode work.
class VideoPlayer : public QObject {
    Q_OBJECT
public:
    explicit VideoPlayer(QObject* parent = nullptr);

    bool loadFile(const QString& path);
    void play();
    void pause();
    void stop();

    void setLooping(bool loop);
    bool isLooping() const { return m_looping; }

    void setVolume(int percent); // 0-100
    int volume() const { return m_volumePercent; }

    void setMuted(bool muted);
    bool isMuted() const { return m_muted; }

    bool isPlaying() const;
    QSize videoNativeSize() const { return m_lastFrameSize; }

    // Latest decoded frame as an image, shared by all render windows.
    std::shared_ptr<const QImage> currentFrame() const { return m_currentFrame; }

signals:
    void frameReady();
    void errorOccurred(const QString& message);
    void playbackStateChanged(QMediaPlayer::PlaybackState state);

private slots:
    void onVideoFrameChanged(const QVideoFrame& frame);
    void onMediaStatusChanged(QMediaPlayer::MediaStatus status);
    void onErrorOccurred(QMediaPlayer::Error error, const QString& errorString);

private:
    QMediaPlayer m_player;
    QAudioOutput m_audioOutput;
    QVideoSink m_sink;

    bool m_looping = true;
    int m_volumePercent = 0;
    bool m_muted = true;

    QSize m_lastFrameSize;
    std::shared_ptr<const QImage> m_currentFrame;
};
