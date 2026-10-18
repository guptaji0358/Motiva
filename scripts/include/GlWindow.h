#pragma once

#include <QDialog>
#include <QStyledItemDelegate>
#include <QList>
#include <QRectF>
#include <functional>

#include "GlUi.h"

class QVBoxLayout;

// A thin vertical stacking helper for GL windows: add items in order and
// layout() assigns their rects top to bottom, skipping hidden items.
// Wrapped labels get the height their text needs at the available width.
class GlStack {
public:
    // Fixed-height row spanning the full width.
    void add(GlItem* item, qreal height, qreal gapAfter = 8);
    // Label whose height follows its text at the layout width.
    void addWrapped(GlLabel* label, qreal gapAfter = 8, qreal minHeight = 20);
    // Two items on one row: `left` takes leftWidth, `right` the rest.
    void addPair(GlItem* left, qreal leftWidth, GlItem* right, qreal height, qreal gapAfter = 8, qreal gap = 12);
    void addSpace(qreal height);
    void clear() { m_rows.clear(); }
    // Returns the total height used.
    qreal layout(const QRectF& area, GlItem* clipTo = nullptr) const;

private:
    struct Row {
        GlItem* a = nullptr;
        GlItem* b = nullptr;
        qreal leftW = 0, h = 0, gapAfter = 0, gap = 0;
        bool wrapped = false;
        qreal minH = 0;
    };
    QList<Row> m_rows;
};

// Base for Motiva's utility windows (Backup, Cleanup, Theme editor, ...): a
// borderless GL window with a title strip, one content panel, an optional
// footer row of buttons, drag/rounded corners handled. Subclasses create
// their items on canvas() and position them in layoutContent().
class GlWindow : public QDialog {
    Q_OBJECT
public:
    GlWindow(const QString& title, const QString& subtitle, QWidget* parent = nullptr, bool resizable = true);

    GlCanvas* canvas() const { return m_canvas; }
    GlPanel* contentPanel() const { return m_content; }
    // Called whenever the canvas lays out. `content` is the inner area of the
    // content panel; `footer` is the strip below it for footer buttons.
    virtual void layoutContent(const QRectF& content, const QRectF& footer) = 0;
    // Right-aligns `buttons` along `footer` (last one right-most).
    static void layoutFooter(const QList<GlButton*>& buttons, const QRectF& footer, qreal width = 130);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void showEvent(QShowEvent* e) override;
    // Content panel height changes with the footer; subclasses may reserve rows.
    void setFooterHeight(qreal h) { m_footerH = h; }

private:
    void layoutGl(const QSizeF& size);

    GlCanvas* m_canvas = nullptr;
    GlPanel* m_title = nullptr;
    GlPanel* m_content = nullptr;
    GlLabel* m_titleLabel = nullptr;
    GlLabel* m_subtitleLabel = nullptr;
    GlButton* m_close = nullptr;
    qreal m_footerH = 56;
    bool m_resizable;
};

// Plain one-line text rows for GlItemView (DisplayRole).
class GlTextDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(200, 30); }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

// Checkable rows with an optional thumbnail (CheckStateRole, DecorationRole).
class GlCheckRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(200, 54); }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

// Multi-column read-only rows (one row of the model = one table row; each
// column's DisplayRole is drawn in its own slot). Column 0 can carry a
// check box (CheckStateRole when kCheckableRole is set), rows can be indented
// (kDepthRole) and any cell may show a small icon (DecorationRole QIcon).
class GlTableDelegate : public QStyledItemDelegate {
public:
    static constexpr int kDepthRole = Qt::UserRole + 40;
    static constexpr int kCheckableRole = Qt::UserRole + 41;
    GlTableDelegate(const QList<qreal>& columnFractions, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_fractions(columnFractions) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(200, 34); }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    QList<qreal> m_fractions;
};
