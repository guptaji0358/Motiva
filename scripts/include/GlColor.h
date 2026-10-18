#pragma once

#include "GlWindow.h"

#include <QColor>

// A color chip that shows its hex value and opens a color picker when pressed.
class GlSwatchButton : public GlItem {
    Q_OBJECT
public:
    explicit GlSwatchButton(GlCanvas* canvas) : GlItem(canvas) {}
    void setColor(const QColor& c) { m_color = c; update(); }
    QColor color() const { return m_color; }
    void setCustom(bool on) { m_custom = on; update(); }
    bool isInteractive() const override { return true; }
    void paint(QPainter& p, const GlUi::Style& s) override;
    void activate() override { emit clicked(); }
signals:
    void clicked();

private:
    QColor m_color;
    bool m_custom = false;
};

// Saturation / value square for a given hue.
class GlColorSquare : public GlItem {
    Q_OBJECT
public:
    explicit GlColorSquare(GlCanvas* canvas) : GlItem(canvas) {}
    void setHsv(qreal h, qreal s, qreal v) { m_h = h; m_s = s; m_v = v; update(); }
    bool isInteractive() const override { return true; }
    void paint(QPainter& p, const GlUi::Style& s) override;
    void mousePressAt(const QPointF& pt) override { pick(pt); }
    void mouseDragTo(const QPointF& pt) override { pick(pt); }
signals:
    void picked(qreal saturation, qreal value);

private:
    void pick(const QPointF& pt);
    qreal m_h = 0, m_s = 0, m_v = 0;
};

class GlHueBar : public GlItem {
    Q_OBJECT
public:
    explicit GlHueBar(GlCanvas* canvas) : GlItem(canvas) {}
    void setHue(qreal h) { m_h = h; update(); }
    bool isInteractive() const override { return true; }
    void paint(QPainter& p, const GlUi::Style& s) override;
    void mousePressAt(const QPointF& pt) override { pick(pt); }
    void mouseDragTo(const QPointF& pt) override { pick(pt); }
signals:
    void picked(qreal hue);

private:
    void pick(const QPointF& pt);
    qreal m_h = 0;
};

// Motiva's color picker (replaces QColorDialog): hue bar, saturation/value
// square, hex field and a before/after preview.
class GlColorDialog : public GlWindow {
    Q_OBJECT
public:
    static QColor getColor(const QColor& initial, QWidget* parent, const QString& title);
    void layoutContent(const QRectF& content, const QRectF& footer) override;

private:
    GlColorDialog(const QColor& initial, QWidget* parent, const QString& title);
    void setColor(const QColor& c, bool updateHex);
    QColor m_initial, m_color;
    qreal m_h = 0, m_s = 0, m_v = 0;
    GlColorSquare* m_square = nullptr;
    GlHueBar* m_hue = nullptr;
    GlTextInput* m_hex = nullptr;
    GlLabel* m_hexLabel = nullptr;
    GlSwatchButton* m_before = nullptr;
    GlSwatchButton* m_after = nullptr;
    GlLabel* m_beforeLabel = nullptr;
    GlLabel* m_afterLabel = nullptr;
    GlButton* m_ok = nullptr;
    GlButton* m_cancel = nullptr;
    bool m_syncing = false;
};
