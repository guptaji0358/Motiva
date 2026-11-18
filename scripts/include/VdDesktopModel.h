#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

// Header-only, Qt Core/Gui-only logic behind the Virtual Desktop Preview, kept
// free of GL/Win32 so it can be unit-tested (tests/VdDesktopTests.cpp).
//  - VdDesktop::Model: geometry + state of the simulated taskbar, Start menu
//    and the two simulated app windows (open/close/raise/drag).
//  - VdDesktop::shouldPausePlayback: the battery/visibility decision.
//  - VdDesktop::planScreenSync: which preview windows to add/remove when the
//    set of monitors changes.
namespace VdDesktop {

constexpr int kSlotCount = 5;  // taskbar slots: 0 = Start, 1..2 = simulated windows, 3..4 decoration
constexpr int kWindowCount = 2;

inline qreal taskbarHeight(const QSizeF& size) {
    return qBound<qreal>(32, size.height() * 0.045, 48);
}

inline QRectF taskbarRect(const QSizeF& size) {
    const qreal th = taskbarHeight(size);
    return QRectF(0, size.height() - th, size.width(), th);
}

// Taskbar slot `i` (0..kSlotCount-1), centred like the default Windows 11 layout.
inline QRectF slotRect(const QSizeF& size, int i) {
    const qreal th = taskbarHeight(size);
    const qreal btn = th * 0.62;
    const qreal gap = th * 0.28;
    const qreal total = kSlotCount * btn + (kSlotCount - 1) * gap;
    const qreal x = size.width() / 2 - total / 2 + i * (btn + gap);
    return QRectF(x, size.height() - th + (th - btn) / 2, btn, btn);
}

inline QRectF startMenuRect(const QSizeF& size) {
    const qreal th = taskbarHeight(size);
    const qreal w = qBound<qreal>(240, size.width() * 0.3, 420);
    const qreal h = qBound<qreal>(280, size.height() * 0.55, 560);
    return QRectF(size.width() / 2 - w / 2, size.height() - th - 12 - h, w, h);
}

inline QRectF titleBarRect(const QRectF& window) {
    return QRectF(window.left(), window.top(), window.width(), 30);
}

inline QRectF closeButtonRect(const QRectF& window) {
    return QRectF(window.right() - 36, window.top(), 36, 30);
}

struct SimWindow {
    QString title;
    QRectF rect;
    bool open = false;
};

class Model {
public:
    Model() {
        m_windows[0].title = QStringLiteral("File Explorer");
        m_windows[1].title = QStringLiteral("Notepad");
    }

    // Sets the desktop size; the first call places the windows, later calls
    // keep them on screen.
    void setArea(const QSizeF& size) {
        if (size.isEmpty()) {
            return;
        }
        const bool first = m_area.isEmpty();
        m_area = size;
        if (first) {
            const qreal w = qBound<qreal>(280, size.width() * 0.4, 720);
            const qreal h = qBound<qreal>(180, size.height() * 0.45, 480);
            m_windows[0].rect = QRectF(size.width() * 0.18, size.height() * 0.12, w, h);
            m_windows[1].rect = QRectF(size.width() * 0.30, size.height() * 0.28, w * 0.8, h * 0.8);
        }
        for (SimWindow& win : m_windows) {
            win.rect = clamped(win.rect);
        }
    }

    QSizeF area() const { return m_area; }
    bool startOpen() const { return m_startOpen; }
    bool isDragging() const { return m_dragging >= 0; }
    const SimWindow& window(int i) const { return m_windows[i]; }
    // Indices bottom -> top (paint in this order).
    const QVector<int>& zOrder() const { return m_z; }

    // Pointer press at `p`. True when the simulated desktop consumed it.
    bool press(const QPointF& p) {
        if (m_area.isEmpty()) {
            return false;
        }
        // Taskbar slots first.
        for (int i = 0; i < 3; ++i) {
            if (slotRect(m_area, i).contains(p)) {
                if (i == 0) {
                    m_startOpen = !m_startOpen;
                } else {
                    m_startOpen = false;
                    SimWindow& win = m_windows[i - 1];
                    win.open = !win.open;
                    if (win.open) {
                        raise(i - 1);
                    }
                }
                return true;
            }
        }
        if (taskbarRect(m_area).contains(p)) {
            m_startOpen = false;
            return true; // bar background: absorbed
        }
        if (m_startOpen) {
            if (startMenuRect(m_area).contains(p)) {
                return true; // inside the menu: stays open
            }
            m_startOpen = false; // click-away; fall through to what's underneath
        }
        // Windows, topmost first.
        for (int k = m_z.size() - 1; k >= 0; --k) {
            const int i = m_z[k];
            SimWindow& win = m_windows[i];
            if (!win.open || !win.rect.contains(p)) {
                continue;
            }
            raise(i);
            if (closeButtonRect(win.rect).contains(p)) {
                win.open = false;
            } else if (titleBarRect(win.rect).contains(p)) {
                m_dragging = i;
                m_dragOffset = p - win.rect.topLeft();
            }
            return true;
        }
        return false;
    }

    void dragTo(const QPointF& p) {
        if (m_dragging < 0) {
            return;
        }
        SimWindow& win = m_windows[m_dragging];
        QRectF r(p - m_dragOffset, win.rect.size());
        win.rect = clamped(r);
    }

    void release() { m_dragging = -1; }

private:
    // Keeps a window fully inside the area above the taskbar (when it fits).
    QRectF clamped(QRectF r) const {
        const qreal maxBottom = m_area.height() - taskbarHeight(m_area);
        if (r.right() > m_area.width()) r.moveRight(m_area.width());
        if (r.bottom() > maxBottom) r.moveBottom(maxBottom);
        if (r.left() < 0) r.moveLeft(0);
        if (r.top() < 0) r.moveTop(0);
        return r;
    }

    void raise(int i) {
        m_z.removeAll(i);
        m_z.push_back(i);
    }

    QSizeF m_area;
    SimWindow m_windows[kWindowCount];
    QVector<int> m_z{0, 1};
    bool m_startOpen = false;
    int m_dragging = -1;
    QPointF m_dragOffset;
};

// Whether an independent preview source should be paused: on battery with the
// "play on battery" policy off, or with no preview window actually showing.
inline bool shouldPausePlayback(bool pauseOnBattery, bool onBattery, bool anyWindowShown) {
    return !anyWindowShown || (pauseOnBattery && onBattery);
}

struct ScreenPlan {
    QStringList add;    // screens that need a window
    QStringList remove; // windows whose screen is gone
};

// `existing`: screens that currently have a window; `wanted`: all screens now.
inline ScreenPlan planScreenSync(const QStringList& existing, const QStringList& wanted) {
    ScreenPlan plan;
    for (const QString& name : wanted) {
        if (!existing.contains(name) && !plan.add.contains(name)) {
            plan.add.push_back(name);
        }
    }
    for (const QString& name : existing) {
        if (!wanted.contains(name) && !plan.remove.contains(name)) {
            plan.remove.push_back(name);
        }
    }
    return plan;
}

} // namespace VdDesktop
