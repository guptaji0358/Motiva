#pragma once

#include "GlUi.h"

// Simulated desktop furniture drawn over the preview wallpaper: a few desktop
// icons (top-left) and a taskbar (bottom). Pure painting - not interactive,
// no real shell data, nothing read from the user's desktop. Lets the user judge
// how a wallpaper looks behind icons and the taskbar.
class VdDesktopChrome : public GlItem {
    Q_OBJECT
public:
    explicit VdDesktopChrome(GlCanvas* canvas);

    void paint(QPainter& p, const GlUi::Style& s) override;

    // Height of the taskbar for a given item size (also used by the window).
    static qreal taskbarHeight(const QSizeF& size);
};
