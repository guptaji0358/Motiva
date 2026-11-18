#pragma once

#include "GlUi.h"
#include "VdDesktopModel.h"

// Simulated desktop furniture drawn over the preview wallpaper: desktop icons
// (top-left), a taskbar, a clickable Start menu and two draggable simulated
// app windows. State and hit-testing live in VdDesktop::Model (unit-tested);
// this item only paints it and forwards pointer input. Nothing here reads or
// touches the real shell, desktop or any other application.
class VdDesktopChrome : public GlItem {
    Q_OBJECT
public:
    explicit VdDesktopChrome(GlCanvas* canvas);

    void paint(QPainter& p, const GlUi::Style& s) override;

    // Interactive only so the Start menu / windows can take clicks; empty
    // desktop areas still get the normal arrow cursor.
    bool isInteractive() const override { return true; }
    Qt::CursorShape cursorShape() const override { return Qt::ArrowCursor; }
    void mousePressAt(const QPointF& p) override;
    void mouseDragTo(const QPointF& p) override;
    void mouseReleased(const QPointF& p) override;

private:
    VdDesktop::Model m_model;
};
