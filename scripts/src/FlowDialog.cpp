#include "FlowDialog.h"
#include "PlaylistLibrary.h"
#include "PlaylistModel.h"
#include "Theme.h"

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
#include <QShortcut>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

namespace {

constexpr qreal kNodeW = 176;
constexpr qreal kNodeH = 146;
constexpr qreal kGapX = 80;
constexpr qreal kGapY = 60;
constexpr int kColumns = 6; // default layout: rows of this many nodes, snaking left-right-left
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
            const QString loop = QStringLiteral("↻ #1");
            const qreal w = sfm.horizontalAdvance(loop) + 14;
            pill(QRectF(kNodeW - 14 - w, thumb.bottom() - sfm.height() - 10, w, sfm.height() + 4),
                 QColor(0, 0, 0, 170), Qt::white, loop);
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
    const qreal factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    const qreal next = transform().m11() * factor;
    if (next > 0.12 && next < 2.5) {
        scale(factor, factor);
    }
    event->accept();
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
    setMinimumSize(840, 560);
    resize(1120, 740);
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
    QTimer::singleShot(0, this, [this] { m_view->fitInView(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40),
                                                           Qt::KeepAspectRatio); });
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
    m_addButton = new QPushButton(tr("+  Add Media"), this);
    m_addButton->setToolTip(tr("Add files to this playlist (they are added at the end of the sequence). Nothing is copied."));
    connect(m_addButton, &QPushButton::clicked, this, &FlowDialog::addMediaRequested);
    bar->addWidget(m_addButton);

    m_arrangeButton = new QToolButton(this);
    m_arrangeButton->setText(tr("Arrange  ▾"));
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

    m_earlierButton = new QPushButton(tr("◀  Earlier"), this);
    m_earlierButton->setToolTip(tr("Move the selected item one step earlier in the sequence (Ctrl+Left)"));
    connect(m_earlierButton, &QPushButton::clicked, this, [this] { moveSelected(-1); });
    bar->addWidget(m_earlierButton);
    m_laterButton = new QPushButton(tr("Later  ▶"), this);
    m_laterButton->setToolTip(tr("Move the selected item one step later in the sequence (Ctrl+Right)"));
    connect(m_laterButton, &QPushButton::clicked, this, [this] { moveSelected(+1); });
    bar->addWidget(m_laterButton);
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
    fit->setToolTip(tr("Zoom to show the whole diagram (Ctrl+wheel zooms, middle-drag pans)"));
    connect(fit, &QPushButton::clicked, this, [this] {
        m_view->fitInView(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
    });
    bar->addWidget(fit);
    root->addLayout(bar);

    m_scene = new QGraphicsScene(this);
    m_view = new FlowView(this);
    m_view->setScene(m_scene);
    root->addWidget(m_view, 1);
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
        if (!m_playlist->isActive()) {
            m_library->setActive(m_playlist->id());
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
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Left), [this] { moveSelected(-1); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), [this] { moveSelected(+1); });
    shortcut(QKeySequence(Qt::Key_Delete), [this] { removeSelected(); });
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
    const int row = rank / kColumns;
    int col = rank % kColumns;
    if (row % 2 == 1) {
        col = kColumns - 1 - col; // snake, so the arrow at a row end stays short
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

    const bool whole = n == m_playlist->count();
    m_title->setText(tr("Wallpaper Flow — %1").arg(m_scopeName));
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

void FlowDialog::tidyLayout() {
    m_library->clearFlowPositions(m_playlist->id());
    m_positions.clear();
    rebuild();
    m_view->fitInView(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
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
    QAction* showNow = menu.addAction(tr("Show Now"));
    showNow->setEnabled(count == 1);
    menu.addSeparator();
    QAction* earlier = menu.addAction(tr("Move Earlier"));
    QAction* later = menu.addAction(tr("Move Later"));
    QAction* start = menu.addAction(tr("Move to Start"));
    QAction* end = menu.addAction(tr("Move to End"));
    QAction* before = menu.addAction(tr("Move Before…"));
    QAction* after = menu.addAction(tr("Move After…"));
    before->setEnabled(count == 1);
    after->setEnabled(count == 1);
    menu.addSeparator();
    QAction* remove = menu.addAction(tr("Remove from Playlist"));
    QAction* chosen = menu.exec(m_view->viewport()->mapToGlobal(viewPos));
    if (!chosen) {
        return;
    }
    if (chosen == showNow) {
        emit m_view->nodeActivated(selectedIds().first());
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
