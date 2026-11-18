#pragma once

#include "BinaryRainConfig.h"
#include "VideoPlayer.h"

#include <QObject>
#include <QString>
#include <memory>

// Where the virtual-desktop preview gets its frames. Two implementations:
//
//  - ActiveWallpaperPreviewSource listens to the SHARED player that drives the
//    real desktop wallpaper. It is strictly a consumer: it never calls
//    loadFile/play/pause/stop/unload on that player, so previewing can never
//    change or interrupt the real wallpaper.
//
//  - IndependentPreviewSource owns its own muted VideoPlayer for previewing a
//    selected playlist item. It is never given to WallpaperManager, so the
//    real wallpaper and its battery policy are unaffected.
//
// Both forward the player's frameReady (shared, read-only frames) so any
// number of surfaces can consume them without copying pixels.
class PreviewSource : public QObject {
    Q_OBJECT
public:
    explicit PreviewSource(QObject* parent = nullptr) : QObject(parent) {}
    ~PreviewSource() override = default;

    // Latest frame, or null before the first one. GUI thread only.
    virtual std::shared_ptr<const QImage> currentFrame() const = 0;
    // True when this source owns its own player (does not mirror the desktop).
    virtual bool isIndependent() const = 0;
    // False if the media could not be opened.
    virtual bool isValid() const = 0;

    // Presentation-only controls. For the active source these are no-ops:
    // the real wallpaper's play state belongs to the user and WallpaperManager.
    virtual void pausePreview() {}
    virtual void resumePreview() {}

signals:
    void frameReady(std::shared_ptr<const QImage> frame);
    // The underlying media was replaced or unloaded.
    void mediaChanged();
};

class ActiveWallpaperPreviewSource : public PreviewSource {
    Q_OBJECT
public:
    // `shared` is WallpaperManager::player(); it must outlive this object.
    explicit ActiveWallpaperPreviewSource(VideoPlayer* shared, QObject* parent = nullptr);

    std::shared_ptr<const QImage> currentFrame() const override;
    bool isIndependent() const override { return false; }
    bool isValid() const override { return m_player != nullptr; }

private:
    VideoPlayer* m_player = nullptr;
};

class IndependentPreviewSource : public PreviewSource {
    Q_OBJECT
public:
    // `source` is a local path, http(s) URL or the Binary Rain pseudo-source.
    // `rain` is used only for the Binary Rain source; `proceduralSize` is the
    // size of the surface it will mostly be drawn on.
    IndependentPreviewSource(const QString& source, const BinaryRain::Config& rain, const QSize& proceduralSize,
                             QObject* parent = nullptr);
    ~IndependentPreviewSource() override;

    std::shared_ptr<const QImage> currentFrame() const override;
    bool isIndependent() const override { return true; }
    bool isValid() const override { return m_valid; }
    void pausePreview() override;
    void resumePreview() override;

private:
    std::unique_ptr<VideoPlayer> m_player;
    bool m_valid = false;
};
