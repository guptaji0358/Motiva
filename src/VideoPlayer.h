#pragma once

#include <QObject>
#include <QUrl>
#include <QImage>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QVideoFrame>
#include <QFutureWatcher>
#include <memory>

Q_DECLARE_METATYPE(std::shared_ptr<const QImage>) // needed for the cross-thread frameReady queued connection

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
    void playbackStateChanged(QMediaPlayer::PlaybackState state);

private slots:
    void onVideoFrameChanged(const QVideoFrame& frame);
    void onConversionFinished();
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
};
