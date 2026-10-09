#pragma once

#include <QDialog>
#include <QGraphicsView>
#include <QHash>
#include <QPointF>
#include <QSet>
#include <QVector>

#include <functional>

class FlowEdgeItem;
class FlowNodeItem;
class PlaylistLibrary;
class PlaylistModel;
class QGraphicsScene;
class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class IconButton;
class QTimer;
class QToolButton;

// The canvas of the Wallpaper Flow: lightweight graphics items (no widgets),
// theme-aware, zoomable (Ctrl+wheel) and pannable (middle button). Knows
// nothing about the library - it reports what the user did and the dialog
// decides what that means for the saved order.
class FlowView : public QGraphicsView {
    Q_OBJECT
public:
    explicit FlowView(QWidget* parent = nullptr);

    QList<FlowNodeItem*> selectedNodes() const;

    // Uniform zoom limits (the view's scale factor; 1.0 = 100%). The scene and
    // the media are never resized - only how large the view draws them.
    static constexpr qreal kMinZoom = 0.12;
    static constexpr qreal kMaxZoom = 2.5;
    qreal zoomFactor() const { return transform().m11(); }
    bool canZoomIn() const { return zoomFactor() < kMaxZoom - 1e-6; }
    bool canZoomOut() const { return zoomFactor() > kMinZoom + 1e-6; }
    // Multiplies the zoom by `factor`, clamped to the limits, around the
    // view's centre (the wheel zooms around the cursor instead).
    void zoomBy(qreal factor, bool aroundCursor = false);
    void resetZoom(); // 100%, centred on the diagram
    // Forget item pointers before the scene is cleared.
    void clearInteraction();

signals:
    // A single node was dropped onto an arrow: a SEQUENCE edit (place the node
    // right after `afterItemId`). Dragging anywhere else only moves the node.
    void nodeDroppedOnArrow(qint64 itemId, qint64 afterItemId);
    // Nodes were moved on the canvas: LAYOUT only, the sequence is unchanged.
    void nodesMoved(const QHash<qint64, QPointF>& positions);
    void nodeActivated(qint64 itemId);
    void zoomChanged();

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void changeEvent(QEvent* event) override;

private:
    void setHotEdge(FlowEdgeItem* edge);
    FlowNodeItem* draggedNode() const;

    bool m_panning = false;
    QPoint m_panLast;
    QHash<qint64, QPointF> m_pressPositions; // selected nodes when the left button went down
    FlowEdgeItem* m_hotEdge = nullptr;
};

// "Wallpaper Flow": the order in which a playlist changes the wallpaper, as a
// diagram you can edit. There is exactly ONE source of truth for that order -
// the playlist's own order (playlist_items.position), the same one
// PlaylistModel::advance() follows. Editing the diagram calls
// PlaylistModel::setOrder, which writes the library first, so every change is
// saved the moment it is made (no Save step, nothing is lost by closing).
// Node positions are a separate, presentation-only thing (flow_layout).
//
// The diagram shows a scope: the whole playlist ("All") or one category's
// items. Reordering a category only permutes the playlist positions that the
// category's items already occupy, so other items keep their places.
class FlowDialog : public QDialog {
    Q_OBJECT
public:
    using ScopeProvider = std::function<QSet<qint64>()>;
    // scopeItems() returns the playlist_items ids that belong in the diagram.
    FlowDialog(PlaylistLibrary* library, PlaylistModel* playlist, const QString& scopeName, ScopeProvider scopeItems,
               QWidget* parent = nullptr);

    // Re-reads the scope and playlist (after media was added elsewhere).
    void refresh();
    // The category this diagram's scope is (0 = the whole playlist); used when
    // "Show now" has to activate the playlist, so it activates this same scope.
    void setScopeCategoryId(qint64 categoryId) { m_scopeCategoryId = categoryId; }

signals:
    void addMediaRequested();

protected:
    void changeEvent(QEvent* event) override;

private:
    void buildUi();
    void applyDirectionIcons(); // Earlier/Later SVG icons for the active theme
    QVector<qint64> scopeOrder() const; // scope item ids in playlist order
    void rebuild();
    void scheduleRebuild();
    void updateButtons();
    // Writes a new sequence for the scope (permuting the positions its items
    // already occupy). `recordUndo` false when called from undo/redo.
    bool applyScopeOrder(const QVector<qint64>& newScope, bool recordUndo = true);
    void moveSelected(int direction); // -1 earlier, +1 later
    void moveSelectedToEdge(bool toStart);
    void moveSelectedRelativeToOther(bool before);
    void dropOnArrow(qint64 itemId, qint64 afterItemId);
    void sortSequence(bool ascending);
    void removeSelected();
    void showNodeMenu(const QPoint& viewPos);
    void undo();
    void redo();
    void tidyLayout();
    // Auto-organize wiring: lays the sequence out as a non-crossing snake and
    // saves those positions (layout only - never touches the sequence).
    void organizeLayout();
    QPointF defaultSlot(int rank) const;
    // Zoom (view scale) and grid columns (node layout) are independent.
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void fitAll();
    void updateZoomUi();
    void setColumns(int columns, bool persist);
    void updateGridUi();
    QString timingSummary() const;
    QVector<qint64> selectedIds() const; // in sequence order

    PlaylistLibrary* m_library;
    PlaylistModel* m_playlist;
    QString m_scopeName;
    qint64 m_scopeCategoryId = 0;
    ScopeProvider m_scopeItems;
    QSet<qint64> m_scope;
    QHash<qint64, QPointF> m_positions; // saved node layout (item id -> scene pos)
    QVector<FlowNodeItem*> m_nodes;     // sequence order
    QVector<QVector<qint64>> m_undo;    // previous FULL playlist orders
    QVector<QVector<qint64>> m_redo;
    QSet<qint64> m_keepSelected;
    bool m_rebuildPending = false;
    QVector<qint64> m_lastSequence; // scope order at the previous rebuild (graph-change detection)
    bool m_layoutPending = true;    // force an organize on the next rebuild

    FlowView* m_view = nullptr;
    QGraphicsScene* m_scene = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    IconButton* m_addButton = nullptr;
    QToolButton* m_arrangeButton = nullptr;
    QCheckBox* m_autoOrganize = nullptr;
    int m_columns = 6; // grid columns of the default/auto layout; rows are automatic
    IconButton* m_zoomInButton = nullptr;
    IconButton* m_zoomOutButton = nullptr;
    QLabel* m_zoomLabel = nullptr;
    QComboBox* m_columnsCombo = nullptr;
    QLabel* m_rowsLabel = nullptr;
    QPushButton* m_gridResetButton = nullptr;
    QAction* m_zoomInAction = nullptr;
    QAction* m_zoomOutAction = nullptr;
    QAction* m_zoomResetAction = nullptr;
    IconButton* m_earlierButton = nullptr;
    IconButton* m_laterButton = nullptr;
    QPushButton* m_removeButton = nullptr;
    QPushButton* m_undoButton = nullptr;
    QPushButton* m_redoButton = nullptr;
    QTimer* m_rebuildTimer = nullptr;
};
