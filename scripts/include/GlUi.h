#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QObject>
#include <QOpenGLWidget>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <functional>
#include <memory>

class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;
class QPainter;
class QTimer;
class QSvgRenderer;

// Motiva's own GPU-drawn UI toolkit ("Colorful Modular x dark glass/neon").
// Nothing here is a QWidget except the single GlCanvas: items are light
// retained-mode objects that the canvas lays out, paints (an animated GLSL
// background, then glass/neon shapes through QPainter on the same OpenGL
// context) and routes mouse/keyboard to. Colors come from the active
// Theme (Theme::themePalette, so Theme-editor customizations apply), and
// the Aurora vs Onyx families keep their soft vs sharp corner language.
namespace GlUi {

struct Style {
    QColor bgBase, bgTint;      // background gradient ends
    QColor accent, accent2;     // main neon accent + complementary accent
    QColor onAccent;            // text on accent fills
    QColor glass, glassEdge;    // translucent panel fill / rim
    QColor text, textDim;
    QColor ok, warn, bad;
    bool dark = true;
    qreal radius = 1.0;         // corner scale: 1 = soft (Aurora), ~0.4 = sharp (Onyx)
    static Style current();
};

QFont uiFont(qreal pointSize, bool bold = false);

} // namespace GlUi

class GlCanvas;

class GlItem : public QObject {
    Q_OBJECT
public:
    explicit GlItem(GlCanvas* canvas);

    QRectF rect() const { return m_rect; }
    void setRect(const QRectF& r);
    bool isVisible() const { return m_visible; }
    void setVisible(bool v);
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool e);
    QString toolTip() const { return m_toolTip; }
    void setToolTip(const QString& t) { m_toolTip = t; }
    QString accessibleName() const { return m_name; }
    void setAccessibleName(const QString& n) { m_name = n; }

    // Interactive items receive hover/press/focus; labels and panels don't.
    virtual bool isInteractive() const { return false; }
    bool isHovered() const { return m_hovered; }
    bool isPressed() const { return m_pressed; }
    bool hasFocus() const;

    void update();
    // 0..1 eased hover/press amounts, advanced by the canvas tick.
    qreal hoverAmount() const { return m_hoverT; }
    qreal pressAmount() const { return m_pressT; }
    virtual bool advance(qreal dt); // returns true while still animating

    virtual void paint(QPainter& p, const GlUi::Style& s) = 0;
    virtual void activate() {}

protected:
    GlCanvas* canvas() const { return m_canvas; }

private:
    friend class GlCanvas;
    GlCanvas* m_canvas;
    QRectF m_rect;
    QString m_toolTip;
    QString m_name;
    bool m_visible = true;
    bool m_enabled = true;
    bool m_hovered = false;
    bool m_pressed = false;
    qreal m_hoverT = 0.0;
    qreal m_pressT = 0.0;
};

// A glass card. The optional color block is the "modular" accent tab.
class GlPanel : public GlItem {
    Q_OBJECT
public:
    enum class Block { None, Accent, Accent2 };
    explicit GlPanel(GlCanvas* canvas);
    void setBlock(Block b) { m_block = b; update(); }
    void setRadiusPx(qreal r) { m_radius = r; update(); }
    void paint(QPainter& p, const GlUi::Style& s) override;

private:
    Block m_block = Block::None;
    qreal m_radius = 20;
};

class GlLabel : public GlItem {
    Q_OBJECT
public:
    enum class Tone { Normal, Dim, Custom };
    explicit GlLabel(GlCanvas* canvas);

    void setText(const QString& t);
    QString text() const { return m_text; }
    void clear() { setText(QString()); }
    void setPixmap(const QPixmap& pm);
    void setPointSize(qreal pt);
    void setBold(bool b);
    void setTone(Tone t);
    void setColor(const QColor& c); // switches to Tone::Custom
    void setAlignment(Qt::Alignment a);
    void setCaps(bool caps); // letter-spaced small caps look for section tags

    void paint(QPainter& p, const GlUi::Style& s) override;

private:
    QString m_text;
    QPixmap m_pixmap;
    qreal m_pt = 10.0;
    bool m_bold = false;
    bool m_caps = false;
    Tone m_tone = Tone::Normal;
    QColor m_color;
    Qt::Alignment m_align = Qt::AlignLeft | Qt::AlignVCenter;
};

class GlButton : public GlItem {
    Q_OBJECT
public:
    enum class Kind {
        Primary,   // split-color neon capsule: icon segment + gradient body
        Secondary, // glass pill, text and/or icon
        Icon,      // round/squircle glass icon-only control
        Minimize,
        Maximize,
        Restore,
        Close,
    };
    GlButton(GlCanvas* canvas, Kind kind);

    void setKind(Kind k);
    void setText(const QString& t);
    QString text() const { return m_text; }
    // Icons are drawn as monochrome glyphs tinted to the button's current
    // foreground, so one SVG fits every theme and state.
    void setIcon(const QIcon& icon);
    // Compatibility with the old IconButton call sites: only the normal
    // icon is used (hover/pressed come from the tint and glow).
    void setStateIcon(const QIcon& normal, const QIcon& = QIcon(), const QIcon& = QIcon(), const QIcon& = QIcon()) {
        setIcon(normal);
    }
    void setDefaultAction(bool d) { m_default = d; }

    bool isInteractive() const override { return true; }
    void paint(QPainter& p, const GlUi::Style& s) override;
    void activate() override;

signals:
    void clicked();

private:
    Kind m_kind;
    QString m_text;
    QIcon m_icon;
    bool m_default = false;
};

// The media preview panel: shows the current frame letterboxed inside a
// glass frame, or the animated drop zone when no media is loaded.
class GlPreview : public GlItem {
    Q_OBJECT
public:
    enum class DropState { Idle, DragOver, Invalid, Success };
    explicit GlPreview(GlCanvas* canvas);

    void setImage(const QImage& img);
    void clearImage();
    bool hasImage() const { return !m_image.isNull(); }
    void setLoadingText(const QString& t); // shown when no image and drop zone hidden
    void setShowDropZone(bool show);
    bool showsDropZone() const { return m_showDrop; }
    void setDropState(DropState s);
    // Area the media itself occupies (aspect-fit inside rect()); used so the
    // panel hugs the media like the old preview host did.
    void setAspect(double aspect);
    QRectF contentRect() const;
    // Free space under the drop-zone artwork for a caller-placed button.
    QRectF dropActionRect() const;

    void paint(QPainter& p, const GlUi::Style& s) override;
    bool advance(qreal dt) override;

signals:
    void contentResized();

private:
    QImage m_image;
    QString m_loading;
    bool m_showDrop = true;
    double m_aspect = 0.0;
    DropState m_state = DropState::Idle;
    qreal m_activity = 0, m_invalid = 0, m_success = 0;
    qreal m_successTarget = 0;
    std::unique_ptr<QSvgRenderer> m_icon;
    QSizeF m_lastSize;
};

class GlCanvas : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit GlCanvas(QWidget* parent = nullptr);
    ~GlCanvas() override;

    // Called whenever the canvas resizes or an item asks for a relayout.
    void setLayoutCallback(std::function<void(const QSizeF&)> fn);
    void relayout();

    void registerItem(GlItem* item);
    void unregisterItem(GlItem* item);
    GlItem* focusedItem() const { return m_focus; }
    void setFocusedItem(GlItem* item);
    // Topmost visible item under p (for window hit-testing).
    GlItem* itemAt(const QPointF& p) const;
    bool interactiveAt(const QPointF& p) const;

    qreal clock() const { return m_clock; }
    void setBackgroundAnimated(bool animated);
    void requestAnimation(); // keep the tick running a little longer
    QImage snapshot(); // offscreen-capable test hook

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    bool focusNextPrevChild(bool next) override;

private:
    void tick();
    void cycleFocus(bool forward);
    void updateHover(const QPointF& p);
    void drawBackground(const GlUi::Style& s);

    QList<GlItem*> m_items;
    GlItem* m_hover = nullptr;
    GlItem* m_press = nullptr;
    GlItem* m_focus = nullptr;
    bool m_keyboardFocusVisible = false;
    std::function<void(const QSizeF&)> m_layout;

    QTimer* m_timer = nullptr;
    QElapsedTimer m_frameClock;
    qint64 m_lastNs = 0;
    qreal m_clock = 0.0;
    bool m_bgAnimated = true;
    int m_hotFrames = 0;

    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::unique_ptr<QOpenGLBuffer> m_quad;
    QOpenGLVertexArrayObject* m_vao = nullptr;
    bool m_glFailed = false;

    friend class GlItem;
};
