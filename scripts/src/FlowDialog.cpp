#include "FlowDialog.h"
#include "PlaylistLibrary.h"
#include "PlaylistModel.h"
#include "DialogSizing.h"
#include "Theme.h"
#include "IconButton.h"
#include "PlaylistDialog.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCollator>
#include <QCoreApplication>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineF>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <functional>

namespace {

constexpr qreal kNodeW = 176;
constexpr qreal kNodeH = 146;
constexpr qreal kGapX = 80;
constexpr qreal kGapY = 60;
constexpr int kDefaultColumns = 6; // default layout: rows of this many nodes, snaking left-right-left
constexpr int kMinColumns = 1;
constexpr int kMaxColumns = 12;  // 12 x (176+80) = ~3000 scene units: still fits at the minimum zoom
constexpr qreal kZoomStep = 1.25;
constexpr const char* kColumnsKey = "flow/columns";
constexpr int kMaxUndo = 100;

QString tx(const char* text) {
    return QCoreApplication::translate("FlowDialog", text);
}

const Theme::ThemePalette& themePal() {
    return Theme::themePalette(Theme::currentTheme());
}

} // namespace

// ----------------------------------------------------------------- items

class FlowNodeItem;

// An arrow between two consecutive nodes of the sequence.
class FlowEdgeItem : public QGraphicsItem {
public:
    enum { Type = QGraphicsItem::UserType + 2 };
    FlowEdgeItem(FlowNodeItem* from, FlowNodeItem* to);
    int type() const override { return Type; }
    FlowNodeItem* from() const { return m_from; }
    FlowNodeItem* to() const { return m_to; }
    void updatePath();
    void setHot(bool hot) {
        if (m_hot != hot) {
            m_hot = hot;
            update();
        }
    }
    QRectF boundingRect() const override { return m_bounds; }
    QPainterPath shape() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) override;

private:
    FlowNodeItem* m_from;
    FlowNodeItem* m_to;
    QPainterPath m_path;
    QPolygonF m_arrow;
    QRectF m_bounds;
    mutable QPainterPath m_shape;
    mutable bool m_shapeValid = false;
    bool m_hot = false;
};

// One media item. Reads its preview/state live from the playlist model (so it
// reuses the model's asynchronous thumbnail loading and never decodes media
// itself); paint only runs for nodes that are actually in view.
class FlowNodeItem : public QGraphicsItem {
public:
    enum { Type = QGraphicsItem::UserType + 1 };
    FlowNodeItem(const QPersistentModelIndex& index, qint64 itemId, int rank, bool last, PlaylistModel* model)
        : m_index(index), m_itemId(itemId), m_rank(rank), m_last(last), m_model(model) {
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setAcceptHoverEvents(false);
    }
    int type() const override { return Type; }
    qint64 itemId() const { return m_itemId; }
    int rank() const { return m_rank; }
    FlowEdgeItem* in = nullptr;
    FlowEdgeItem* out = nullptr;

    QRectF boundingRect() const override { return QRectF(-3, -3, kNodeW + 6, kNodeH + 6); }

    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
        const Theme::ThemePalette& pal = themePal();
        const qreal lod = QStyleOptionGraphicsItem::levelOfDetailFromTransform(p->worldTransform());
        const bool valid = m_index.isValid();
        const bool available = valid && m_index.data(PlaylistModel::AvailableRole).toBool();
        const bool current = valid && m_index.data(PlaylistModel::IsCurrentRole).toBool();
        const bool selected = isSelected();
        const QRectF body(1, 1, kNodeW - 2, kNodeH - 2);

        p->setRenderHint(QPainter::Antialiasing, true);
        QPen border(selected ? pal.accent : (available ? pal.borderStrong : QColor(Theme::kStatusWarning)),
                    selected ? 2.5 : 1.2);
        if (!available) {
            border.setStyle(Qt::DashLine);
        }
        p->setPen(border);
        p->setBrush(pal.panelBg);
        p->drawRoundedRect(body, 10, 10);
        if (selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(pal.accentSoft);
            p->drawRoundedRect(body, 10, 10);
        }
        if (lod < 0.3 || !valid) {
            return; // zoomed far out: just the card
        }

        // Preview
        const QRectF thumb(8, 8, kNodeW - 16, 88);
        p->save();
        QPainterPath clip;
        clip.addRoundedRect(thumb, 7, 7);
        p->setClipPath(clip);
        p->fillRect(thumb, pal.baseBg);
        const QImage image = m_index.data(PlaylistModel::ThumbnailRole).value<QImage>();
        if (!image.isNull()) {
            QSizeF s = image.size();
            s.scale(thumb.size(), Qt::KeepAspectRatio);
            const QRectF target(thumb.center().x() - s.width() / 2, thumb.center().y() - s.height() / 2, s.width(),
                                s.height());
            p->setRenderHint(QPainter::SmoothPixmapTransform, true);
            p->drawImage(target, image);
        } else {
            QFont f = p->font();
            f.setPointSizeF(qMax(7.0, f.pointSizeF()));
            p->setFont(f);
            p->setPen(available ? pal.textSecondary : QColor(Theme::kStatusWarning));
            p->drawText(thumb, Qt::AlignCenter,
                        !available                                                   ? tx("File not found")
                        : m_index.data(PlaylistModel::ThumbnailPendingRole).toBool() ? tx("Loading preview…")
                                                                                     : tx("No preview"));
        }
        p->restore();

        QFont small = p->font();
        small.setBold(true);
        small.setPointSizeF(qMax(7.0, small.pointSizeF() - 1));
        p->setFont(small);
        const QFontMetrics sfm(small);
        auto pill = [&](const QRectF& r, const QColor& fill, const QColor& text, const QString& s) {
            p->setPen(Qt::NoPen);
            p->setBrush(fill);
            p->drawRoundedRect(r, r.height() / 2, r.height() / 2);
            p->setPen(text);
            p->drawText(r, Qt::AlignCenter, s);
        };
        const QString rankText = QStringLiteral("#%1").arg(m_rank + 1);
        pill(QRectF(14, 14, sfm.horizontalAdvance(rankText) + 14, sfm.height() + 4), QColor(0, 0, 0, 170), Qt::white,
             rankText);
        if (current) {
            const QString now = tx("NOW");
            const qreal w = sfm.horizontalAdvance(now) + 14;
            pill(QRectF(kNodeW - 14 - w, 14, w, sfm.height() + 4), pal.accent, pal.selectionText, now);
        }
        if (m_last) {
            // "Loops back to #1": SVG loop mark + text, not a glyph.
            const QString loop = QStringLiteral("#1");
            const int iconSize = sfm.height() - 2;
            const qreal w = sfm.horizontalAdvance(loop) + iconSize + 20;
            const QRectF r(kNodeW - 14 - w, thumb.bottom() - sfm.height() - 10, w, sfm.height() + 4);
            pill(r, QColor(0, 0, 0, 170), Qt::white, QString());
            p->drawPixmap(QPointF(r.left() + 8, r.center().y() - iconSize / 2.0),
                          Theme::tintedIcon(QStringLiteral(":/flow/loop.svg"), Qt::white, iconSize));
            p->setPen(Qt::white);
            p->drawText(QRectF(r.left() + 8 + iconSize + 4, r.top(), sfm.horizontalAdvance(loop) + 2, r.height()),
                        Qt::AlignLeft | Qt::AlignVCenter, loop);
        }

        // Name + type
        QFont nameFont = p->font();
        nameFont.setBold(true);
        nameFont.setPointSizeF(small.pointSizeF() + 1);
        p->setFont(nameFont);
        p->setPen(pal.textPrimary);
        const QString name = m_index.data(Qt::DisplayRole).toString();
        p->drawText(QRectF(10, 100, kNodeW - 20, 18), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(nameFont).elidedText(name, Qt::ElideMiddle, int(kNodeW - 20)));
        QFont sub = p->font();
        sub.setBold(false);
        sub.setPointSizeF(small.pointSizeF());
        p->setFont(sub);
        p->setPen(available ? pal.textSecondary : QColor(Theme::kStatusWarning));
        QString line = m_index.data(PlaylistModel::IsVideoRole).toBool() ? tx("Video") : tx("Image");
        if (!available) {
            line += QStringLiteral("  •  ") + tx("File not found");
        } else {
            const qint64 size = m_model->factsAt(m_index.row()).size;
            if (size >= 0) {
                line += QStringLiteral("  •  ") + QLocale().formattedDataSize(size);
            }
        }
        p->drawText(QRectF(10, 119, kNodeW - 20, 18), Qt::AlignLeft | Qt::AlignVCenter, line);
    }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override {
        if (change == ItemPositionHasChanged) {
            if (in) {
                in->updatePath();
            }
            if (out) {
                out->updatePath();
            }
        }
        return QGraphicsItem::itemChange(change, value);
    }

private:
    QPersistentModelIndex m_index;
    qint64 m_itemId;
    int m_rank;
    bool m_last;
    PlaylistModel* m_model;
};

FlowEdgeItem::FlowEdgeItem(FlowNodeItem* from, FlowNodeItem* to) : m_from(from), m_to(to) {
    setZValue(-1);
    updatePath();
}

void FlowEdgeItem::updatePath() {
    prepareGeometryChange();
    const QRectF a(m_from->pos(), QSizeF(kNodeW, kNodeH));
    const QRectF b(m_to->pos(), QSizeF(kNodeW, kNodeH));
    const QPointF d = b.center() - a.center();
    QPointF dir;
    QPointF p1;
    QPointF p2;
    if (qAbs(d.x()) >= qAbs(d.y())) {
        dir = QPointF(d.x() >= 0 ? 1 : -1, 0);
        p1 = a.center() + QPointF(dir.x() * kNodeW / 2, 0);
        p2 = b.center() - QPointF(dir.x() * kNodeW / 2, 0);
    } else {
        dir = QPointF(0, d.y() >= 0 ? 1 : -1);
        p1 = a.center() + QPointF(0, dir.y() * kNodeH / 2);
        p2 = b.center() - QPointF(0, dir.y() * kNodeH / 2);
    }
    const qreal k = qMax<qreal>(36, QLineF(p1, p2).length() * 0.4);
    m_path = QPainterPath(p1);
    m_path.cubicTo(p1 + dir * k, p2 - dir * k, p2);
    const QPointF perp(-dir.y(), dir.x());
    m_arrow = QPolygonF({p2, p2 - dir * 13 + perp * 6.5, p2 - dir * 13 - perp * 6.5});
    m_bounds = m_path.controlPointRect().united(m_arrow.boundingRect()).adjusted(-16, -16, 16, 16);
    m_shapeValid = false;
}

QPainterPath FlowEdgeItem::shape() const {
    if (!m_shapeValid) {
        QPainterPathStroker stroker;
        stroker.setWidth(26); // generous: easy to drop a node onto
        m_shape = stroker.createStroke(m_path);
        m_shapeValid = true;
    }
    return m_shape;
}

void FlowEdgeItem::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) {
    const Theme::ThemePalette& pal = themePal();
    const qreal lod = QStyleOptionGraphicsItem::levelOfDetailFromTransform(p->worldTransform());
    const QColor color = m_hot ? pal.accent : pal.textSecondary;
    p->setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, m_hot ? 4.0 : (lod < 0.3 ? 1.2 : 2.2));
    pen.setCapStyle(Qt::RoundCap);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    p->drawPath(m_path);
    if (lod >= 0.3 || m_hot) {
        p->setPen(Qt::NoPen);
        p->setBrush(color);
        p->drawPolygon(m_arrow);
    }
}

// ------------------------------------------------------------------ view

FlowView::FlowView(QWidget* parent) : QGraphicsView(parent) {
    setRenderHint(QPainter::Antialiasing, true);
    setDragMode(QGraphicsView::RubberBandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    setFrameShape(QFrame::NoFrame);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAccessibleName(tr("Wallpaper flow diagram"));
}

QList<FlowNodeItem*> FlowView::selectedNodes() const {
    QList<FlowNodeItem*> nodes;
    if (scene()) {
        for (QGraphicsItem* item : scene()->selectedItems()) {
            if (auto* n = qgraphicsitem_cast<FlowNodeItem*>(item)) {
                nodes.push_back(n);
            }
        }
    }
    return nodes;
}

FlowNodeItem* FlowView::draggedNode() const {
    if (!scene() || selectedNodes().size() != 1) {
        return nullptr;
    }
    return qgraphicsitem_cast<FlowNodeItem*>(scene()->mouseGrabberItem());
}

void FlowView::setHotEdge(FlowEdgeItem* edge) {
    if (edge == m_hotEdge) {
        return;
    }
    if (m_hotEdge) {
        m_hotEdge->setHot(false);
    }
    m_hotEdge = edge;
    if (m_hotEdge) {
        m_hotEdge->setHot(true);
    }
}

void FlowView::clearInteraction() {
    m_hotEdge = nullptr; // the items are about to be destroyed
    m_pressPositions.clear();
}

void FlowView::wheelEvent(QWheelEvent* event) {
    if (!(event->modifiers() & Qt::ControlModifier)) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    zoomBy(event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15, /*aroundCursor=*/true);
    event->accept();
}

void FlowView::zoomBy(qreal factor, bool aroundCursor) {
    const qreal current = zoomFactor();
    const qreal target = qBound(kMinZoom, current * factor, kMaxZoom);
    if (qFuzzyCompare(target, current)) {
        return;
    }
    setTransformationAnchor(aroundCursor ? QGraphicsView::AnchorUnderMouse : QGraphicsView::AnchorViewCenter);
    scale(target / current, target / current); // uniform: aspect ratios are preserved
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    emit zoomChanged();
}

void FlowView::resetZoom() {
    if (scene()) {
        const QPointF center = scene()->itemsBoundingRect().center();
        resetTransform();
        centerOn(center);
    } else {
        resetTransform();
    }
    emit zoomChanged();
}

void FlowView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panLast = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
    m_pressPositions.clear();
    for (FlowNodeItem* n : selectedNodes()) {
        m_pressPositions.insert(n->itemId(), n->pos());
    }
}

void FlowView::mouseMoveEvent(QMouseEvent* event) {
    if (m_panning) {
        const QPoint d = event->pos() - m_panLast;
        m_panLast = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
    if (!(event->buttons() & Qt::LeftButton)) {
        return;
    }
    // A single node being dragged over an arrow = "put it in this spot of the
    // sequence". Anywhere else the drag only repositions the node.
    FlowNodeItem* node = draggedNode();
    FlowEdgeItem* found = nullptr;
    if (node) {
        const QPointF center = node->sceneBoundingRect().center();
        for (QGraphicsItem* item : scene()->items(center)) {
            auto* edge = qgraphicsitem_cast<FlowEdgeItem*>(item);
            if (edge && edge->from() != node && edge->to() != node) {
                found = edge;
                break;
            }
        }
    }
    setHotEdge(found);
}

void FlowView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }
    qint64 draggedId = 0;
    qint64 afterId = 0;
    if (m_hotEdge) {
        if (FlowNodeItem* node = draggedNode()) {
            draggedId = node->itemId();
            afterId = m_hotEdge->from()->itemId();
        }
    }
    setHotEdge(nullptr);
    QGraphicsView::mouseReleaseEvent(event);

    QHash<qint64, QPointF> moved;
    for (FlowNodeItem* n : selectedNodes()) {
        const auto it = m_pressPositions.constFind(n->itemId());
        if (it != m_pressPositions.constEnd() && *it != n->pos()) {
            moved.insert(n->itemId(), n->pos());
        }
    }
    m_pressPositions.clear();
    if (!moved.isEmpty()) {
        emit nodesMoved(moved);
    }
    if (draggedId && afterId) {
        emit nodeDroppedOnArrow(draggedId, afterId);
    }
}

void FlowView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (auto* node = qgraphicsitem_cast<FlowNodeItem*>(itemAt(event->pos()))) {
        emit nodeActivated(node->itemId());
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void FlowView::drawBackground(QPainter* painter, const QRectF& rect) {
    const Theme::ThemePalette& pal = themePal();
    painter->fillRect(rect, pal.altBg);
    if (transform().m11() < 0.45) {
        return;
    }
    constexpr int step = 32;
    QColor dot = pal.border;
    dot.setAlpha(150);
    painter->setPen(QPen(dot, 2));
    const int x0 = int(rect.left()) - int(rect.left()) % step;
    const int y0 = int(rect.top()) - int(rect.top()) % step;
    for (int x = x0; x < rect.right(); x += step) {
        for (int y = y0; y < rect.bottom(); y += step) {
            painter->drawPoint(x, y);
        }
    }
}

void FlowView::changeEvent(QEvent* event) {
    QGraphicsView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        viewport()->update(); // items and background re-read the active theme
        if (scene()) {
            scene()->update();
        }
    }
}

// ---------------------------------------------------------------- dialog

FlowDialog::FlowDialog(PlaylistLibrary* library, PlaylistModel* playlist, const QString& scopeName,
                       ScopeProvider scopeItems, QWidget* parent)
    : QDialog(parent), m_library(library), m_playlist(playlist), m_scopeName(scopeName),
      m_scopeItems(std::move(scopeItems)) {
    setWindowTitle(tr("Wallpaper Flow"));
    setWindowIcon(QIcon(QStringLiteral(":/playlist/%1/playlist.svg").arg(Theme::iconVariant(Theme::currentTheme()))));
    {
        bool ok = false;
        const int saved = QSettings().value(QLatin1String(kColumnsKey), kDefaultColumns).toInt(&ok);
        m_columns = ok ? qBound(kMinColumns, saved, kMaxColumns) : kDefaultColumns;
    }
    buildUi();

    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(0);
    connect(m_rebuildTimer, &QTimer::timeout, this, &FlowDialog::rebuild);
    // Anything that changes the playlist (including playback advancing, or
    // edits made elsewhere) is reflected here; the model is the source of truth.
    connect(m_playlist, &PlaylistModel::contentsChanged, this, &FlowDialog::scheduleRebuild);
    connect(m_playlist, &QAbstractItemModel::modelReset, this, &FlowDialog::scheduleRebuild);
    connect(m_playlist, &PlaylistModel::currentChanged, this, [this] { m_scene->update(); });
    connect(m_playlist, &QAbstractItemModel::dataChanged, this, [this] { m_scene->update(); });

    refresh();
    QTimer::singleShot(0, this, [this] { fitAll(); });
    DialogSizing::applyComfortableSize(this, QSize(1120, 740));
}

void FlowDialog::applyDirectionIcons() {
    const QString v = Theme::iconVariant(Theme::currentTheme());
    auto icon = [&v](const char* name) { return QIcon(QStringLiteral(":/flow/%1/%2.svg").arg(v, QLatin1String(name))); };
    m_earlierButton->setStateIcon(icon("earlier"), QIcon(), QIcon(), icon("earlier-disabled"));
    m_laterButton->setStateIcon(icon("later"), QIcon(), QIcon(), icon("later-disabled"));
    const auto add = [&v](const char* n) { return QIcon(QStringLiteral(":/playlist/%1/%2.svg").arg(v, QLatin1String(n))); };
    m_addButton->setStateIcon(add("add"), QIcon(), QIcon(), add("add-disabled"));
    m_zoomInButton->setStateIcon(icon("zoom-in"), QIcon(), QIcon(), icon("zoom-in-disabled"));
    m_zoomOutButton->setStateIcon(icon("zoom-out"), QIcon(), QIcon(), icon("zoom-out-disabled"));
}

void FlowDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::ThemeChange) && m_zoomOutButton) {
        applyDirectionIcons();
    }
}

void FlowDialog::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(8);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("appTitle"));
    QFont tf = m_title->font();
    tf.setBold(true);
    tf.setPointSize(tf.pointSize() + 3);
    m_title->setFont(tf);
    root->addWidget(m_title);
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("secondaryText"));
    m_summary->setWordWrap(true);
    root->addWidget(m_summary);

    auto* bar = new QHBoxLayout();
    bar->setSpacing(8);
    m_addButton = new IconButton(QIcon(), tr("Add Media"), this);
    m_addButton->setToolTip(tr("Add files to this playlist (they are added at the end of the sequence). Nothing is copied."));
    connect(m_addButton, &QPushButton::clicked, this, &FlowDialog::addMediaRequested);
    bar->addWidget(m_addButton);

    m_arrangeButton = new QToolButton(this);
    m_arrangeButton->setText(tr("Arrange")); // Qt draws the menu indicator
    m_arrangeButton->setPopupMode(QToolButton::InstantPopup);
    m_arrangeButton->setToolTip(tr("Rewrite the saved sequence by file name. This changes the order the wallpaper follows "
                                   "(you can undo it here)."));
    auto* arrangeMenu = new QMenu(m_arrangeButton);
    connect(arrangeMenu->addAction(tr("Sequence A – Z")), &QAction::triggered, this, [this] { sortSequence(true); });
    connect(arrangeMenu->addAction(tr("Sequence Z – A")), &QAction::triggered, this, [this] { sortSequence(false); });
    arrangeMenu->addSeparator();
    connect(arrangeMenu->addAction(tr("Tidy Layout")), &QAction::triggered, this, &FlowDialog::tidyLayout);
    m_arrangeButton->setMenu(arrangeMenu);
    bar->addWidget(m_arrangeButton);

    m_autoOrganize = new QCheckBox(tr("Auto-organize wiring"), this);
    m_autoOrganize->setToolTip(tr("When on, the diagram is laid out automatically whenever items are added, removed or "
                                  "reordered, so the arrows don't cross or overlap. Only node positions change - never the "
                                  "sequence. Turn it off to keep your own arrangement."));
    m_autoOrganize->setChecked(QSettings().value(QStringLiteral("flow/autoOrganize"), false).toBool());
    connect(m_autoOrganize, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("flow/autoOrganize"), on);
        if (on) {
            m_layoutPending = true;
            rebuild();
            fitAll();
            m_status->setText(tr("Auto-organize is on. The diagram was arranged; the sequence was not changed."));
        } else {
            m_status->setText(tr("Auto-organize is off. Your arrangement is kept as it is."));
        }
    });
    bar->addWidget(m_autoOrganize);

    m_earlierButton = new IconButton(QIcon(), tr("Earlier"), this);
    m_earlierButton->setToolTip(tr("Move the selected item one step earlier in the sequence (L or Ctrl+Left)"));
    connect(m_earlierButton, &QPushButton::clicked, this, [this] { moveSelected(-1); });
    bar->addWidget(m_earlierButton);
    m_laterButton = new IconButton(QIcon(), tr("Later"), this);
    m_laterButton->setToolTip(tr("Move the selected item one step later in the sequence (R or Ctrl+Right)"));
    connect(m_laterButton, &QPushButton::clicked, this, [this] { moveSelected(+1); });
    bar->addWidget(m_laterButton);
    m_zoomOutButton = new IconButton(QIcon(), QString(), this);
    m_zoomInButton = new IconButton(QIcon(), QString(), this);
    applyDirectionIcons();
    m_removeButton = new QPushButton(tr("Remove"), this);
    m_removeButton->setToolTip(tr("Remove the selected items from this playlist (Delete). No files are deleted."));
    connect(m_removeButton, &QPushButton::clicked, this, &FlowDialog::removeSelected);
    bar->addWidget(m_removeButton);
    bar->addSpacing(10);
    m_undoButton = new QPushButton(tr("Undo"), this);
    m_undoButton->setToolTip(tr("Undo the last sequence change (Ctrl+Z)"));
    connect(m_undoButton, &QPushButton::clicked, this, &FlowDialog::undo);
    bar->addWidget(m_undoButton);
    m_redoButton = new QPushButton(tr("Redo"), this);
    m_redoButton->setToolTip(tr("Redo (Ctrl+Y)"));
    connect(m_redoButton, &QPushButton::clicked, this, &FlowDialog::redo);
    bar->addWidget(m_redoButton);
    bar->addStretch();
    auto* fit = new QPushButton(tr("Fit"), this);
    fit->setToolTip(tr("Zoom to show the whole diagram (Ctrl+wheel zooms, Ctrl+0 resets to 100%, middle-drag pans)"));
    connect(fit, &QPushButton::clicked, this, [this] {
        fitAll();
    });
    bar->addWidget(fit);
    root->addLayout(bar);

    // View row: zoom (how large the diagram is drawn) and grid columns (how the
    // default layout is arranged). Independent of each other.
    auto* viewBar = new QHBoxLayout();
    viewBar->setSpacing(8);
    auto* gridLabel = new QLabel(tr("Grid layout"), this);
    gridLabel->setObjectName(QStringLiteral("secondaryText"));
    viewBar->addWidget(gridLabel);
    auto* columnsLabel = new QLabel(tr("Columns:"), this);
    viewBar->addWidget(columnsLabel);
    m_columnsCombo = new QComboBox(this);
    for (int c = kMinColumns; c <= kMaxColumns; ++c) {
        m_columnsCombo->addItem(QString::number(c), c);
    }
    m_columnsCombo->setAccessibleName(tr("Grid columns"));
    m_columnsCombo->setToolTip(tr("How many nodes sit in each row of the layout (default %1). Changing it re-lays out the "
                                  "diagram; the sequence and the zoom are not changed.").arg(kDefaultColumns));
    columnsLabel->setBuddy(m_columnsCombo);
    viewBar->addWidget(m_columnsCombo);
    m_rowsLabel = new QLabel(this);
    m_rowsLabel->setToolTip(tr("Rows are worked out automatically from the number of items and the columns."));
    viewBar->addWidget(m_rowsLabel);
    m_gridResetButton = new QPushButton(tr("Reset to Default"), this);
    m_gridResetButton->setToolTip(tr("Go back to %1 columns").arg(kDefaultColumns));
    viewBar->addWidget(m_gridResetButton);
    viewBar->addStretch();

    for (IconButton* b : {m_zoomOutButton, m_zoomInButton}) {
        b->setFixedSize(36, 32);
        b->setIconSize(QSize(18, 18));
    }
    m_zoomOutButton->setAccessibleName(tr("Zoom Out"));
    m_zoomInButton->setAccessibleName(tr("Zoom In"));
    m_zoomLabel = new QLabel(this);
    m_zoomLabel->setMinimumWidth(44);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_zoomLabel->setObjectName(QStringLiteral("secondaryText"));
    viewBar->addWidget(m_zoomOutButton);
    viewBar->addWidget(m_zoomLabel);
    viewBar->addWidget(m_zoomInButton);
    root->addLayout(viewBar);

    m_scene = new QGraphicsScene(this);
    m_view = new FlowView(this);
    m_view->setScene(m_scene);
    root->addWidget(m_view, 1);

    // One QAction per zoom command: the shortcuts and the context menu share it.
    // Ctrl+= is accepted too (the "+" key needs Shift on many layouts). The
    // shortcuts do nothing while a text field or spin box is being edited.
    auto makeAction = [this](const QString& text, const QList<QKeySequence>& keys, void (FlowDialog::*slot)()) {
        auto* a = new QAction(text, this);
        a->setShortcuts(keys);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(a);
        connect(a, &QAction::triggered, this, [this, slot] {
            if (!PlaylistDialog::isTextInputFocused()) {
                (this->*slot)();
            }
        });
        return a;
    };
    m_zoomInAction = makeAction(tr("Zoom In"), {QKeySequence(Qt::CTRL | Qt::Key_Plus), QKeySequence(Qt::CTRL | Qt::Key_Equal),
                                                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Equal)}, &FlowDialog::zoomIn);
    m_zoomOutAction = makeAction(tr("Zoom Out"), {QKeySequence(Qt::CTRL | Qt::Key_Minus)}, &FlowDialog::zoomOut);
    m_zoomResetAction = makeAction(tr("Reset Zoom"), {QKeySequence(Qt::CTRL | Qt::Key_0)}, &FlowDialog::resetZoom);
    m_zoomInButton->setToolTip(tr("Zoom In (Ctrl++)"));
    m_zoomOutButton->setToolTip(tr("Zoom Out (Ctrl+-)"));
    m_zoomLabel->setToolTip(tr("Current zoom. Reset Zoom: Ctrl+0"));
    connect(m_zoomInButton, &QPushButton::clicked, this, &FlowDialog::zoomIn);
    connect(m_zoomOutButton, &QPushButton::clicked, this, &FlowDialog::zoomOut);
    connect(m_view, &FlowView::zoomChanged, this, &FlowDialog::updateZoomUi);
    connect(m_columnsCombo, &QComboBox::activated, this, [this](int index) {
        setColumns(m_columnsCombo->itemData(index).toInt(), /*persist=*/true);
    });
    connect(m_gridResetButton, &QPushButton::clicked, this, [this] { setColumns(kDefaultColumns, /*persist=*/true); });
    updateGridUi();
    updateZoomUi();
    connect(m_scene, &QGraphicsScene::selectionChanged, this, &FlowDialog::updateButtons);
    connect(m_view, &QWidget::customContextMenuRequested, this, &FlowDialog::showNodeMenu);
    connect(m_view, &FlowView::nodesMoved, this, [this](const QHash<qint64, QPointF>& moved) {
        // Layout only - the sequence is untouched.
        for (auto it = moved.constBegin(); it != moved.constEnd(); ++it) {
            m_positions.insert(it.key(), it.value());
        }
        m_library->saveFlowPositions(m_playlist->id(), moved);
    });
    connect(m_view, &FlowView::nodeDroppedOnArrow, this, &FlowDialog::dropOnArrow, Qt::QueuedConnection);
    connect(m_view, &FlowView::nodeActivated, this, [this](qint64 itemId) {
        const int row = m_playlist->rowOfItem(itemId);
        if (row < 0) {
            return;
        }
        if (!m_playlist->setCurrentIndex(row)) {
            m_status->setText(tr("That file can't be found on disk, so it can't be shown."));
            return;
        }
        if (!m_playlist->isActive() || m_library->activeCategoryId() != m_scopeCategoryId) {
            m_library->setActive(m_playlist->id(), m_scopeCategoryId);
        }
        m_status->setText(tr("Showing \"%1\" now.").arg(QFileInfo(m_playlist->pathAt(row)).fileName()));
    });

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("secondaryText"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto* bottom = new QHBoxLayout();
    auto* saved = new QLabel(tr("Every change is saved to your library as soon as you make it."), this);
    saved->setObjectName(QStringLiteral("secondaryText"));
    bottom->addWidget(saved, 1);
    auto* close = new QPushButton(tr("Close"), this);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(close);
    root->addLayout(bottom);

    auto shortcut = [this](const QKeySequence& seq, auto slot) {
        auto* s = new QShortcut(seq, this);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        connect(s, &QShortcut::activated, this, slot);
    };
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), [this] { undo(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Y), [this] { redo(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), [this] { redo(); });
    // Item shortcuts. They act on the selected node(s) through the same
    // slots as the buttons and the context menu, and stay silent while a
    // text field / spin box is being edited. Each letter is also bound with
    // Shift so Shift+key behaves like the plain key (Caps Lock needs no
    // special handling: Qt reports the same key either way).
    auto itemShortcut = [this, shortcut](const QList<QKeySequence>& keys, std::function<void()> action) {
        for (const QKeySequence& k : keys) {
            shortcut(k, [action] {
                if (!PlaylistDialog::isTextInputFocused()) {
                    action();
                }
            });
        }
    };
    auto letter = [](int key) {
        return QList<QKeySequence>{QKeySequence(key), QKeySequence(Qt::SHIFT | key)};
    };
    itemShortcut(letter(Qt::Key_L), [this] { moveSelected(-1); });
    itemShortcut(letter(Qt::Key_R), [this] { moveSelected(+1); });
    itemShortcut(letter(Qt::Key_S), [this] { moveSelectedToEdge(true); });
    itemShortcut(letter(Qt::Key_E), [this] { moveSelectedToEdge(false); });
    itemShortcut(letter(Qt::Key_N), [this] { showSelectedNow(); });
    itemShortcut({QKeySequence(Qt::CTRL | Qt::Key_Left)}, [this] { moveSelected(-1); });
    itemShortcut({QKeySequence(Qt::CTRL | Qt::Key_Right)}, [this] { moveSelected(+1); });
    itemShortcut({QKeySequence(Qt::Key_Delete)}, [this] { removeSelected(); });
}

void FlowDialog::showSelectedNow() {
    const QVector<qint64> selected = selectedIds();
    if (selected.size() != 1) {
        if (selected.size() > 1) {
            m_status->setText(tr("Select a single item to show it now."));
        }
        return;
    }
    // Same path as double-clicking a node and the "Show Now" menu entry.
    emit m_view->nodeActivated(selected.first());
}

QString FlowDialog::timingSummary() const {
    const RotationSettings r = m_playlist->rotation();
    QString when;
    if (m_playlist->isVideo()) {
        when = tr("Each video plays to its end, then the next one in this order starts.");
    } else {
        QStringList parts;
        if (r.onInterval) {
            parts << (r.intervalMinutes == 1 ? tr("every minute") : tr("every %1 minutes").arg(r.intervalMinutes));
        }
        if (r.onUnlock) {
            parts << tr("when you unlock");
        }
        if (r.onWindowsStart) {
            parts << tr("when Windows starts");
        }
        when = parts.isEmpty() ? tr("The wallpaper changes only when you pick an image.")
                               : tr("The wallpaper moves to the next image %1.").arg(parts.join(tr(", ")));
    }
    return when + QLatin1Char(' ') + tr("After the last item it loops back to #1.");
}

void FlowDialog::refresh() {
    m_scope = m_scopeItems();
    m_positions = m_library->flowPositions(m_playlist->id());
    m_undo.clear();
    m_redo.clear();
    rebuild();
}

void FlowDialog::scheduleRebuild() {
    if (!m_rebuildPending) {
        m_rebuildPending = true;
        m_rebuildTimer->start();
    }
}

QVector<qint64> FlowDialog::scopeOrder() const {
    QVector<qint64> ids;
    for (int r = 0; r < m_playlist->count(); ++r) {
        const qint64 id = m_playlist->itemIdAt(r);
        if (m_scope.contains(id)) {
            ids.push_back(id);
        }
    }
    return ids;
}

QPointF FlowDialog::defaultSlot(int rank) const {
    const int row = rank / m_columns;
    int col = rank % m_columns;
    if (row % 2 == 1) {
        col = m_columns - 1 - col; // snake, so the arrow at a row end stays short
    }
    return QPointF(col * (kNodeW + kGapX), row * (kNodeH + kGapY));
}

QVector<qint64> FlowDialog::selectedIds() const {
    QSet<qint64> sel;
    for (FlowNodeItem* n : m_view->selectedNodes()) {
        sel.insert(n->itemId());
    }
    QVector<qint64> ordered;
    for (qint64 id : scopeOrder()) {
        if (sel.contains(id)) {
            ordered.push_back(id);
        }
    }
    return ordered;
}

void FlowDialog::rebuild() {
    m_rebuildPending = false;
    QSet<qint64> keep;
    for (FlowNodeItem* n : m_view->selectedNodes()) {
        keep.insert(n->itemId());
    }
    m_view->clearInteraction();
    m_scene->clear();
    m_nodes.clear();

    // Rows of the playlist in sequence order, restricted to the scope.
    QVector<qint64> ids;
    QVector<int> rows;
    for (int r = 0; r < m_playlist->count(); ++r) {
        const qint64 id = m_playlist->itemIdAt(r);
        if (m_scope.contains(id)) {
            ids.push_back(id);
            rows.push_back(r);
        }
    }
    const int n = ids.size();
    // Auto-organize only on a real graph change (items added/removed/reordered),
    // or when just switched on - not on every rebuild (playback advance, theme),
    // and never while dragging.
    if (m_autoOrganize && m_autoOrganize->isChecked() && (m_layoutPending || ids != m_lastSequence)) {
        organizeLayout();
    }
    m_layoutPending = false;
    m_lastSequence = ids;
    m_nodes.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto* node = new FlowNodeItem(QPersistentModelIndex(m_playlist->index(rows[i])), ids[i], i, i == n - 1, m_playlist);
        node->setPos(m_positions.value(ids[i], defaultSlot(i)));
        m_scene->addItem(node);
        node->setSelected(keep.contains(ids[i]));
        m_nodes.push_back(node);
    }
    for (int i = 1; i < n; ++i) {
        auto* edge = new FlowEdgeItem(m_nodes[i - 1], m_nodes[i]);
        m_nodes[i - 1]->out = edge;
        m_nodes[i]->in = edge;
        m_scene->addItem(edge);
    }
    m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-400, -300, 400, 300));
    updateGridUi();

    const bool whole = n == m_playlist->count();
    // Say when this diagram IS what the wallpaper is currently rotating.
    const bool isActiveSequence = m_playlist->isActive() && m_library->activeCategoryId() == m_scopeCategoryId;
    m_title->setText(isActiveSequence ? tr("Wallpaper Flow — %1  (active sequence)").arg(m_scopeName)
                                      : tr("Wallpaper Flow — %1").arg(m_scopeName));
    m_summary->setText(timingSummary() + QLatin1Char(' ') +
                       (whole ? tr("#n is an item's place in the sequence.")
                              : tr("Showing %1 of %2 items; the others keep their places in the playlist. #n is the place "
                                   "within this view.")
                                    .arg(n)
                                    .arg(m_playlist->count())));
    if (n == 0) {
        m_status->setText(tr("There is nothing to show here yet. Use Add Media."));
    } else if (m_status->text().isEmpty()) {
        m_status->setText(tr("Drag a node to arrange the diagram (layout only). To change the sequence, drop a node onto "
                             "an arrow, or use Earlier / Later."));
    }
    updateButtons();
}

void FlowDialog::updateButtons() {
    const int selected = m_view->selectedNodes().size();
    m_earlierButton->setEnabled(selected > 0);
    m_laterButton->setEnabled(selected > 0);
    m_removeButton->setEnabled(selected > 0);
    m_undoButton->setEnabled(!m_undo.isEmpty());
    m_redoButton->setEnabled(!m_redo.isEmpty());
    m_arrangeButton->setEnabled(m_nodes.size() > 1);
}

bool FlowDialog::applyScopeOrder(const QVector<qint64>& newScope, bool recordUndo) {
    const QVector<qint64> before = m_playlist->itemIds();
    QVector<qint64> full = before;
    QVector<int> places;
    for (int i = 0; i < full.size(); ++i) {
        if (m_scope.contains(full[i])) {
            places.push_back(i);
        }
    }
    if (places.size() != newScope.size()) {
        return false;
    }
    // The scope's items only trade among the places they already hold.
    for (int k = 0; k < places.size(); ++k) {
        full[places[k]] = newScope[k];
    }
    if (full == before) {
        return true;
    }
    if (!m_playlist->setOrder(full)) {
        m_status->setText(tr("The new order could not be saved. Nothing was changed."));
        return false;
    }
    if (recordUndo) {
        m_undo.push_back(before);
        if (m_undo.size() > kMaxUndo) {
            m_undo.removeFirst();
        }
        m_redo.clear();
    }
    m_status->setText(tr("Saved. The wallpaper will follow this order."));
    scheduleRebuild();
    updateButtons();
    return true;
}

void FlowDialog::moveSelected(int direction) {
    QVector<qint64> ids = scopeOrder();
    const QVector<qint64> selected = selectedIds();
    if (selected.isEmpty()) {
        return;
    }
    const QSet<qint64> sel(selected.begin(), selected.end());
    if (direction < 0) {
        for (int i = 1; i < ids.size(); ++i) {
            if (sel.contains(ids[i]) && !sel.contains(ids[i - 1])) {
                std::swap(ids[i], ids[i - 1]);
            }
        }
    } else {
        for (int i = ids.size() - 2; i >= 0; --i) {
            if (sel.contains(ids[i]) && !sel.contains(ids[i + 1])) {
                std::swap(ids[i], ids[i + 1]);
            }
        }
    }
    applyScopeOrder(ids);
}

void FlowDialog::moveSelectedToEdge(bool toStart) {
    const QVector<qint64> selected = selectedIds();
    if (selected.isEmpty()) {
        return;
    }
    const QSet<qint64> sel(selected.begin(), selected.end());
    QVector<qint64> rest;
    for (qint64 id : scopeOrder()) {
        if (!sel.contains(id)) {
            rest.push_back(id);
        }
    }
    applyScopeOrder(toStart ? selected + rest : rest + selected);
}

void FlowDialog::moveSelectedRelativeToOther(bool before) {
    const QVector<qint64> selected = selectedIds();
    if (selected.size() != 1) {
        return;
    }
    const QVector<qint64> ids = scopeOrder();
    QStringList names;
    QVector<qint64> targets;
    for (int i = 0; i < ids.size(); ++i) {
        if (ids[i] != selected.first()) {
            names << QStringLiteral("#%1   %2").arg(i + 1).arg(QFileInfo(m_playlist->pathAt(m_playlist->rowOfItem(ids[i]))).fileName());
            targets.push_back(ids[i]);
        }
    }
    bool ok = false;
    const QString choice = QInputDialog::getItem(this, before ? tr("Move Before") : tr("Move After"),
                                                 before ? tr("Place the selected item before:") : tr("Place the selected item after:"),
                                                 names, 0, false, &ok);
    if (!ok) {
        return;
    }
    const qint64 target = targets.value(names.indexOf(choice));
    QVector<qint64> next = ids;
    next.removeOne(selected.first());
    const int pos = next.indexOf(target);
    next.insert(before ? pos : pos + 1, selected.first());
    applyScopeOrder(next);
}

void FlowDialog::dropOnArrow(qint64 itemId, qint64 afterItemId) {
    QVector<qint64> ids = scopeOrder();
    if (itemId == afterItemId || !ids.contains(itemId) || !ids.contains(afterItemId)) {
        return;
    }
    ids.removeOne(itemId);
    ids.insert(ids.indexOf(afterItemId) + 1, itemId);
    applyScopeOrder(ids);
}

void FlowDialog::sortSequence(bool ascending) {
    const QVector<qint64> ids = scopeOrder();
    if (ids.size() < 2) {
        return;
    }
    if (QMessageBox::question(this, tr("Arrange Sequence"),
                              tr("Rewrite the saved sequence of %1 by file name (%2)?\n\nThis changes the order the "
                                 "wallpaper follows. You can undo it with Undo.")
                                  .arg(m_scopeName, ascending ? tr("A – Z") : tr("Z – A")),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    QVector<QPair<QString, qint64>> named;
    for (qint64 id : ids) {
        named.push_back({QFileInfo(m_playlist->pathAt(m_playlist->rowOfItem(id))).fileName(), id});
    }
    std::stable_sort(named.begin(), named.end(), [&](const auto& a, const auto& b) {
        const int c = collator.compare(a.first, b.first);
        return ascending ? c < 0 : c > 0;
    });
    QVector<qint64> sorted;
    for (const auto& pair : named) {
        sorted.push_back(pair.second);
    }
    applyScopeOrder(sorted);
}

void FlowDialog::removeSelected() {
    const QVector<qint64> selected = selectedIds();
    if (selected.isEmpty()) {
        return;
    }
    const QString question = selected.size() == 1
        ? tr("Remove this item from \"%1\"?\n\nThe file is not deleted.").arg(m_playlist->name())
        : tr("Remove %1 items from \"%2\"?\n\nYour files are not deleted.").arg(selected.size()).arg(m_playlist->name());
    if (QMessageBox::question(this, tr("Remove from Playlist"), question, QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    QVector<int> rows;
    for (qint64 id : selected) {
        rows.push_back(m_playlist->rowOfItem(id));
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    int removed = 0;
    for (int row : rows) {
        removed += (row >= 0 && m_playlist->removeAt(row)) ? 1 : 0;
    }
    if (m_playlist->count() == 0 && m_playlist->isActive()) {
        m_library->setActive(0); // nothing left to show
    }
    m_undo.clear(); // the saved orders no longer describe the same items
    m_redo.clear();
    m_scope = m_scopeItems();
    m_status->setText(tr("Removed %1 from the playlist. No files were touched.").arg(removed));
    scheduleRebuild();
    updateButtons();
}

void FlowDialog::undo() {
    if (m_undo.isEmpty()) {
        return;
    }
    const QVector<qint64> previous = m_undo.takeLast();
    const QVector<qint64> now = m_playlist->itemIds();
    if (!m_playlist->setOrder(previous)) {
        m_undo.clear();
        m_redo.clear();
        m_status->setText(tr("That change can no longer be undone."));
        updateButtons();
        return;
    }
    m_redo.push_back(now);
    m_status->setText(tr("Undone and saved."));
    scheduleRebuild();
    updateButtons();
}

void FlowDialog::redo() {
    if (m_redo.isEmpty()) {
        return;
    }
    const QVector<qint64> next = m_redo.takeLast();
    const QVector<qint64> now = m_playlist->itemIds();
    if (!m_playlist->setOrder(next)) {
        m_undo.clear();
        m_redo.clear();
        m_status->setText(tr("That change can no longer be redone."));
        updateButtons();
        return;
    }
    m_undo.push_back(now);
    m_status->setText(tr("Redone and saved."));
    scheduleRebuild();
    updateButtons();
}

void FlowDialog::organizeLayout() {
    // The wiring is the chain #1 -> #2 -> ... in sequence order. The snake
    // (left-to-right, then right-to-left on the next row) keeps every arrow
    // between grid neighbours, so arrows never cross or overlap, however long
    // the playlist is. One O(n) pass and one transaction per graph change.
    const QVector<qint64> ids = scopeOrder();
    QHash<qint64, QPointF> positions;
    positions.reserve(ids.size());
    for (int i = 0; i < ids.size(); ++i) {
        positions.insert(ids[i], defaultSlot(i));
    }
    for (auto it = positions.constBegin(); it != positions.constEnd(); ++it) {
        m_positions.insert(it.key(), it.value());
    }
    m_library->saveFlowPositions(m_playlist->id(), positions);
}

void FlowDialog::zoomIn() {
    m_view->zoomBy(kZoomStep);
}

void FlowDialog::zoomOut() {
    m_view->zoomBy(1.0 / kZoomStep);
}

void FlowDialog::resetZoom() {
    m_view->resetZoom();
}

void FlowDialog::fitAll() {
    m_view->fitInView(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
    updateZoomUi();
}

void FlowDialog::updateZoomUi() {
    if (!m_zoomLabel || !m_zoomInAction) {
        return;
    }
    m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(m_view->zoomFactor() * 100)));
    m_zoomInButton->setEnabled(m_view->canZoomIn());
    m_zoomOutButton->setEnabled(m_view->canZoomOut());
    m_zoomInAction->setEnabled(m_view->canZoomIn());
    m_zoomOutAction->setEnabled(m_view->canZoomOut());
}

void FlowDialog::updateGridUi() {
    if (!m_columnsCombo || !m_gridResetButton) {
        return;
    }
    const int index = m_columnsCombo->findData(m_columns);
    m_columnsCombo->blockSignals(true);
    m_columnsCombo->setCurrentIndex(qMax(0, index));
    m_columnsCombo->blockSignals(false);
    const int items = m_nodes.size();
    const int rows = items == 0 ? 0 : (items + m_columns - 1) / m_columns;
    m_rowsLabel->setText(items == 0 ? tr("Rows: Auto") : tr("Rows: Auto (%1)").arg(rows));
    m_gridResetButton->setEnabled(m_columns != kDefaultColumns);
}

void FlowDialog::setColumns(int columns, bool persist) {
    columns = qBound(kMinColumns, columns, kMaxColumns);
    if (columns == m_columns) {
        return;
    }
    m_columns = columns;
    if (persist) {
        // Only the column preference is stored; rows are automatic. The
        // default is stored as "no preference".
        QSettings settings;
        if (columns == kDefaultColumns) {
            settings.remove(QLatin1String(kColumnsKey));
        } else {
            settings.setValue(QLatin1String(kColumnsKey), columns);
        }
    }
    // Re-grid like Tidy Layout (node positions only - never the sequence),
    // but keep the user's zoom: only re-centre on the new layout.
    m_library->clearFlowPositions(m_playlist->id());
    m_positions.clear();
    rebuild();
    m_view->centerOn(m_scene->itemsBoundingRect().center());
    m_status->setText(tr("Layout set to %1 column(s). The sequence was not changed.").arg(m_columns));
}

void FlowDialog::tidyLayout() {
    m_library->clearFlowPositions(m_playlist->id());
    m_positions.clear();
    rebuild();
    fitAll();
    m_status->setText(tr("Layout tidied. The sequence was not changed."));
}

void FlowDialog::showNodeMenu(const QPoint& viewPos) {
    if (auto* node = qgraphicsitem_cast<FlowNodeItem*>(m_view->itemAt(viewPos))) {
        if (!node->isSelected()) {
            m_scene->clearSelection();
            node->setSelected(true);
        }
    }
    const int count = m_view->selectedNodes().size();
    if (count == 0) {
        return;
    }
    QMenu menu(this);
    // Shortcuts are shown for reference; the dialog's own shortcuts do the work.
    auto menuAction = [&menu](const QString& text, const QKeySequence& key) {
        QAction* a = menu.addAction(text);
        a->setShortcut(key);
        return a;
    };
    QAction* showNow = menuAction(tr("Show Now"), QKeySequence(Qt::Key_N));
    showNow->setEnabled(count == 1);
    menu.addSeparator();
    QAction* earlier = menuAction(tr("Move Earlier"), QKeySequence(Qt::Key_L));
    QAction* later = menuAction(tr("Move Later"), QKeySequence(Qt::Key_R));
    QAction* start = menuAction(tr("Move to Start"), QKeySequence(Qt::Key_S));
    QAction* end = menuAction(tr("Move to End"), QKeySequence(Qt::Key_E));
    QAction* before = menu.addAction(tr("Move Before…"));
    QAction* after = menu.addAction(tr("Move After…"));
    before->setEnabled(count == 1);
    after->setEnabled(count == 1);
    menu.addSeparator();
    QAction* remove = menuAction(tr("Remove from Playlist"), QKeySequence(Qt::Key_Delete));
    menu.addSeparator();
    menu.addAction(m_zoomInAction);
    menu.addAction(m_zoomOutAction);
    menu.addAction(m_zoomResetAction);
    QAction* chosen = menu.exec(m_view->viewport()->mapToGlobal(viewPos));
    if (!chosen) {
        return;
    }
    if (chosen == showNow) {
        showSelectedNow();
    } else if (chosen == earlier) {
        moveSelected(-1);
    } else if (chosen == later) {
        moveSelected(+1);
    } else if (chosen == start) {
        moveSelectedToEdge(true);
    } else if (chosen == end) {
        moveSelectedToEdge(false);
    } else if (chosen == before) {
        moveSelectedRelativeToOther(true);
    } else if (chosen == after) {
        moveSelectedRelativeToOther(false);
    } else if (chosen == remove) {
        removeSelected();
    }
}
