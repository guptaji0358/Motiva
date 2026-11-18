#pragma once

#include "DesktopScaling.h"
#include "GlUi.h"

#include <QImage>
#include <QString>
#include <memory>

// Draws the preview wallpaper frame inside the virtual desktop. It is a plain
// GlItem: the frame is a shared, read-only QImage handed over by a
// PreviewSource, drawn straight to the GL canvas (the GPU scales it; Phase 0
// measured this at roughly a quarter of the CPU cost of pre-scaling on the
// CPU). Placement follows DesktopScaling, i.e. the same Fill/Fit/Stretch/
// Original rules as the real wallpaper renderer.
class VdWallpaperSurface : public GlItem {
    Q_OBJECT
public:
    explicit VdWallpaperSurface(GlCanvas* canvas);

    void setFrame(std::shared_ptr<const QImage> frame);
    bool hasFrame() const { return m_frame && !m_frame->isNull(); }
    void setScalingMode(DesktopScaling::Mode mode);
    void setPlaceholderText(const QString& text);

    void paint(QPainter& p, const GlUi::Style& s) override;

private:
    std::shared_ptr<const QImage> m_frame;
    DesktopScaling::Mode m_mode = DesktopScaling::Mode::Fill;
    QString m_placeholder;
};
