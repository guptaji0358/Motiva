#include "PreviewSource.h"

ActiveWallpaperPreviewSource::ActiveWallpaperPreviewSource(VideoPlayer* shared, QObject* parent)
    : PreviewSource(parent), m_player(shared) {
    if (m_player) {
        connect(m_player, &VideoPlayer::frameReady, this, &PreviewSource::frameReady);
        connect(m_player, &VideoPlayer::mediaChanged, this, &PreviewSource::mediaChanged);
    }
}

std::shared_ptr<const QImage> ActiveWallpaperPreviewSource::currentFrame() const {
    return m_player ? m_player->currentFrame() : nullptr;
}

IndependentPreviewSource::IndependentPreviewSource(const QString& source, const BinaryRain::Config& rain,
                                                   const QSize& proceduralSize, QObject* parent)
    : PreviewSource(parent), m_player(std::make_unique<VideoPlayer>()) {
    // A preview must never make noise.
    m_player->setMuted(true);
    m_player->setVolume(0);
    connect(m_player.get(), &VideoPlayer::frameReady, this, &PreviewSource::frameReady);
    connect(m_player.get(), &VideoPlayer::mediaChanged, this, &PreviewSource::mediaChanged);
    if (VideoPlayer::isProceduralSource(source)) {
        m_player->setBinaryRainConfig(rain);
        m_player->setProceduralTargetSize(proceduralSize);
    }
    m_valid = m_player->loadFile(source);
    if (m_valid) {
        m_player->play();
    }
}

IndependentPreviewSource::~IndependentPreviewSource() {
    // Release the decoder deterministically while the event loop is alive
    // (same reason MainWindow::onExitRequested tears the wallpaper down early).
    if (m_player) {
        m_player->unload();
    }
}

std::shared_ptr<const QImage> IndependentPreviewSource::currentFrame() const {
    return m_player ? m_player->currentFrame() : nullptr;
}

void IndependentPreviewSource::pausePreview() {
    if (m_player && m_valid) {
        m_player->pause();
    }
}

void IndependentPreviewSource::resumePreview() {
    if (m_player && m_valid) {
        m_player->play();
    }
}
