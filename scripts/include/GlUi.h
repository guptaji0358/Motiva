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
#include <QStringList>
#include <QString>
#include <functional>
#include <memory>

class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;
class QPainter;
class QTimer;
class QSvgRenderer;
class GlCanvas;
class QWidget;
class QKeyEvent;
class QWheelEvent;

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

// Borderless-window support shared by every Motiva window.
// handleFrameHitTest answers WM_NCHITTEST: resize edges, caption over
// `captionRect` wherever no interactive item sits, client elsewhere.
bool handleFrameHitTest(QWidget* window, GlCanvas* canvas, const QRectF& captionRect, const void* nativeMsg,
                        qintptr* result);
// Windows 11 rounded corners for a borderless window.
void applyRoundedCorners(QWidget* window);

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

    // Optional clip (e.g. a scrolling viewport): the item is neither painted
    // nor hit outside it. A null rect means no clip.
    void setClip(const QRectF& r);
    QRectF clip() const { return m_clip; }
    bool isHit(const QPointF& p) const;

    // Pointer/keyboard hooks for controls that need more than a click.
    virtual void mousePressAt(const QPointF&) {}
    virtual void mouseDragTo(const QPointF&) {}
    virtual bool keyPressed(QKeyEvent*) { return false; }
    // Popup support (dropdowns): painted above everything, and while open it
    // receives all pointer/keyboard input.
    virtual void paintOverlay(QPainter&, const GlUi::Style&) {}
    virtual bool popupPress(const QPointF&) { return false; } // true = handled inside
    virtual void popupHover(const QPointF&) {}
    virtual bool popupKey(QKeyEvent*) { return false; }

protected:
    GlCanvas* canvas() const { return m_canvas; }

private:
    friend class GlCanvas;
    GlCanvas* m_canvas;
    QRectF m_rect;
    QRectF m_clip;
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
    enum class Tone { Normal, Dim, Accent, Custom };
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
        Tab,       // navigation entry: selectable, optionally left-aligned
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
    void setSelected(bool on) { m_selected = on; update(); }
    bool isSelected() const { return m_selected; }
    void setAlignLeft(bool on) { m_alignLeft = on; update(); }

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
    bool m_selected = false;
    bool m_alignLeft = false;
};

// Row controls. Each draws its own label on the left and the control on the
// right so a settings page is just a column of rows.
class GlToggle : public GlItem {
    Q_OBJECT
public:
    explicit GlToggle(GlCanvas* canvas);
    void setText(const QString& t) { m_text = t; update(); }
    void setIndent(qreal px) { m_indent = px; update(); }
    bool isChecked() const { return m_checked; }
    void setChecked(bool on);
    bool isInteractive() const override { return true; }
    bool advance(qreal dt) override;
    void paint(QPainter& p, const GlUi::Style& s) override;
    void activate() override { setChecked(!m_checked); }
    bool keyPressed(QKeyEvent* e) override;
signals:
    void toggled(bool on);

private:
    QString m_text;
    bool m_checked = false;
    qreal m_indent = 0;
    qreal m_knob = 0.0;
};

class GlSlider : public GlItem {
    Q_OBJECT
public:
    explicit GlSlider(GlCanvas* canvas);
    void setText(const QString& t) { m_text = t; update(); }
    void setRange(int lo, int hi);
    int value() const { return m_value; }
    void setValue(int v);
    bool isInteractive() const override { return true; }
    void paint(QPainter& p, const GlUi::Style& s) override;
    void mousePressAt(const QPointF& p) override { setFromX(p.x()); }
    void mouseDragTo(const QPointF& p) override { setFromX(p.x()); }
    bool keyPressed(QKeyEvent* e) override;
signals:
    void valueChanged(int v);

private:
    QRectF trackRect() const;
    void setFromX(qreal x);
    QString m_text;
    int m_min = 0, m_max = 100, m_value = 0;
};

class GlDropdown : public GlItem {
    Q_OBJECT
public:
    explicit GlDropdown(GlCanvas* canvas);
    void setText(const QString& t) { m_text = t; update(); }
    void addItem(const QString& t) { m_items << t; update(); }
    void addItems(const QStringList& l) { m_items << l; update(); }
    void clear();
    int count() const { return m_items.size(); }
    int currentIndex() const { return m_index; }
    void setCurrentIndex(int i);
    bool isInteractive() const override { return true; }
    void paint(QPainter& p, const GlUi::Style& s) override;
    void paintOverlay(QPainter& p, const GlUi::Style& s) override;
    void activate() override;
    bool popupPress(const QPointF& pt) override;
    void popupHover(const QPointF& pt) override;
    bool popupKey(QKeyEvent* e) override;
    bool keyPressed(QKeyEvent* e) override;
signals:
    void currentIndexChanged(int index);

private:
    QRectF fieldRect() const;
    QRectF popupRect() const;
    int indexAt(const QPointF& pt) const;
    void closePopup();
    QString m_text;
    QStringList m_items;
    int m_index = -1;
    int m_highlight = -1;
    bool m_open = false;
};

// Thin vertical scrollbar for a caller-managed scrolling viewport.
class GlScrollBar : public GlItem {
    Q_OBJECT
public:
    explicit GlScrollBar(GlCanvas* canvas);
    void setMetrics(qreal viewport, qreal content, qreal offset);
    bool isInteractive() const override { return m_content > m_viewport + 0.5; }
    void paint(QPainter& p, const GlUi::Style& s) override;
    void mousePressAt(const QPointF& p) override;
    void mouseDragTo(const QPointF& p) override;
signals:
    void offsetChanged(qreal offset);

private:
    QRectF thumb() const;
    qreal m_viewport = 1, m_content = 1, m_offset = 0, m_grab = 0;
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

    // Popups (dropdown lists): painted last, take all input while open.
    void setPopup(GlItem* item);
    GlItem* popup() const { return m_popup; }
    // Wheel scrolling is owned by whoever builds a scrolling viewport.
    void setWheelCallback(std::function<void(int delta, const QPointF& pos)> fn) { m_wheel = std::move(fn); }

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
    void wheelEvent(QWheelEvent* e) override;
    void changeEvent(QEvent* e) override;
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
    GlItem* m_popup = nullptr;
    std::function<void(int, const QPointF&)> m_wheel;
    bool m_keyboardFocusVisible = false;
    std::function<void(const QSizeF&)> m_layout;

    QTimer* m_timer = nullptr;
    QElapsedTimer m_frameClock;
    qint64 m_lastNs = 0;
    qint64 m_shownNs = 0;
    qreal m_clock = 0.0;
    bool m_bgAnimated = true;
    int m_hotFrames = 0;

    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::unique_ptr<QOpenGLBuffer> m_quad;
    QOpenGLVertexArrayObject* m_vao = nullptr;
    bool m_glFailed = false;

    friend class GlItem;
};
