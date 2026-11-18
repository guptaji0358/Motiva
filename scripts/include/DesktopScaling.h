#pragma once

#include <QRectF>
#include <QSizeF>

// Pure geometry for placing a wallpaper frame inside a surface. Same rules as
// D3DWallpaperRenderer::computeUvTransform (Fill = cover, Fit = contain,
// Stretch = fill ignoring aspect, Original = native size centered), expressed
// as a destination rectangle so the virtual-desktop preview can reuse the
// behaviour without touching the D3D/Win32 pipeline.
//
// The enum order matches ScalingMode (WallpaperWindow.h) so a ScalingMode can
// be converted with static_cast; VdWallpaperSurface asserts that at compile
// time. Header-only and Qt-Core-only so it is unit-testable on its own.
namespace DesktopScaling {

enum class Mode { Fill = 0, Fit = 1, Stretch = 2, Original = 3 };

// Rectangle (in the same units as `surface`) the source image should be drawn
// into. For Fill it may extend past the surface (the caller clips to it).
inline QRectF destRect(Mode mode, const QSizeF& source, const QSizeF& surface) {
    const QRectF whole(QPointF(0, 0), surface);
    if (source.width() <= 0 || source.height() <= 0 || surface.width() <= 0 || surface.height() <= 0) {
        return whole;
    }
    QSizeF size = surface;
    switch (mode) {
        case Mode::Stretch:
            return whole;
        case Mode::Fill:
            size = source.scaled(surface, Qt::KeepAspectRatioByExpanding);
            break;
        case Mode::Fit:
            size = source.scaled(surface, Qt::KeepAspectRatio);
            break;
        case Mode::Original:
            size = source;
            break;
    }
    return QRectF(QPointF((surface.width() - size.width()) / 2.0, (surface.height() - size.height()) / 2.0), size);
}

} // namespace DesktopScaling
