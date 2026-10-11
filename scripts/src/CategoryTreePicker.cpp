#include "CategoryTreePicker.h"
#include "Theme.h"

#include <QApplication>
#include <QFrame>
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
}

CategoryTreePicker::CategoryTreePicker(QWidget* parent) : QPushButton(parent) {
    setMinimumWidth(190);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral("QPushButton { text-align: left; padding-right: 26px; }"));
    setAccessibleName(tr("Category"));
    m_allLabel = tr("All");

    m_model = new QStandardItemModel(this);
    m_popup = new QFrame(this, Qt::Popup);
    m_popup->setObjectName(QStringLiteral("categoryPopup"));
    m_popup->setFrameShape(QFrame::NoFrame);
    auto* layout = new QVBoxLayout(m_popup);
    layout->setContentsMargins(0, 0, 0, 0);
    m_tree = new QTreeView(m_popup);
    m_tree->setModel(m_model);
    m_tree->setHeaderHidden(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setIndentation(18);
    m_tree->setAnimated(false);
    m_tree->setAccessibleName(tr("Categories"));
    layout->addWidget(m_tree);

    connect(m_tree, &QTreeView::clicked, this, &CategoryTreePicker::chosen);
    connect(m_tree, &QTreeView::activated, this, &CategoryTreePicker::chosen);
    connect(m_tree, &QTreeView::collapsed, this, [this](const QModelIndex& i) {
        m_collapsed.insert(i.data(kIdRole).toLongLong());
    });
    connect(m_tree, &QTreeView::expanded, this, [this](const QModelIndex& i) {
        m_collapsed.remove(i.data(kIdRole).toLongLong());
    });
    connect(this, &QPushButton::clicked, this, &CategoryTreePicker::showPopup);
    rebuild();
}

void CategoryTreePicker::setCategories(const QVector<PlaylistFilterInfo>& categories, const QString& allLabel) {
    m_categories = categories;
    m_allLabel = allLabel;
    bool exists = m_current == 0;
    QSet<qint64> ids;
    for (const PlaylistFilterInfo& f : m_categories) {
        ids.insert(f.id);
        exists = exists || f.id == m_current;
    }
    m_collapsed.intersect(ids);
    m_current = exists ? m_current : 0;
    rebuild();
}

void CategoryTreePicker::setCurrentCategory(qint64 id) {
    bool exists = id == 0;
    for (const PlaylistFilterInfo& f : std::as_const(m_categories)) {
        exists = exists || f.id == id;
    }
    const qint64 next = exists ? id : 0;
    if (next == m_current) {
        return;
    }
    m_current = next;
    updateText();
    emit currentCategoryChanged(m_current);
}

QString CategoryTreePicker::pathOf(qint64 id) const {
    if (id == 0) {
        return m_allLabel;
    }
    QStringList parts;
    QSet<qint64> seen;
    while (id > 0 && !seen.contains(id)) {
        seen.insert(id);
        qint64 parent = 0;
        bool found = false;
        for (const PlaylistFilterInfo& f : m_categories) {
            if (f.id == id) {
                parts.prepend(f.name);
                parent = f.parentId;
                found = true;
                break;
            }
        }
        if (!found) {
            break;
        }
        id = parent;
    }
    return parts.join(QStringLiteral("  >  "));
}

void CategoryTreePicker::updateText() {
    // The button shows the category's own name; the full path is the tooltip.
    const QString path = pathOf(m_current);
    QString leaf = m_allLabel;
    for (const PlaylistFilterInfo& f : std::as_const(m_categories)) {
        if (f.id == m_current && m_current != 0) {
            leaf = f.name;
        }
    }
    setText(leaf);
    setAccessibleDescription(tr("Selected category: %1").arg(path));
    setToolTip(tr("Selected category: %1").arg(path));
}

void CategoryTreePicker::rebuild() {
    m_model->clear();
    auto* all = new QStandardItem(m_allLabel);
    all->setData(qint64(0), kIdRole);
    all->setEditable(false);
    m_model->appendRow(all);

    QHash<qint64, QStandardItem*> byId;
    QSet<qint64> known;
    for (const PlaylistFilterInfo& f : std::as_const(m_categories)) {
        known.insert(f.id);
    }
    // Parents first: repeat until every category is attached (cycles cannot
    // exist - the library sanitizes them - but never loop forever anyway).
    QVector<const PlaylistFilterInfo*> pending;
    for (const PlaylistFilterInfo& f : std::as_const(m_categories)) {
        pending.push_back(&f);
    }
    int guard = pending.size() + 1;
    while (!pending.isEmpty() && guard-- > 0) {
        QVector<const PlaylistFilterInfo*> next;
        for (const PlaylistFilterInfo* f : std::as_const(pending)) {
            const bool root = f->parentId <= 0 || !known.contains(f->parentId);
            if (!root && !byId.contains(f->parentId)) {
                next.push_back(f);
                continue;
            }
            auto* item = new QStandardItem(f->name);
            item->setData(f->id, kIdRole);
            item->setEditable(false);
            item->setToolTip(f->isSelection() ? tr("Hand-picked items") : tr("Rule-based category"));
            (root ? m_model->invisibleRootItem() : byId.value(f->parentId))->appendRow(item);
            byId.insert(f->id, item);
        }
        pending = next;
    }
    m_tree->expandAll();
    for (qint64 id : std::as_const(m_collapsed)) {
        if (QStandardItem* item = byId.value(id)) {
            m_tree->collapse(item->index());
        }
    }
    updateText();
}

void CategoryTreePicker::showPopup() {
    const Theme::ThemePalette& pal = Theme::themePalette(Theme::currentTheme());
    m_popup->setStyleSheet(QStringLiteral(
        "QFrame#categoryPopup { background: %1; border: 1px solid %2; }"
        "QTreeView { background: %1; color: %3; border: none; outline: none; padding: 4px; }"
        "QTreeView::item { padding: 5px 6px; }"
        "QTreeView::item:hover { background: %4; }"
        "QTreeView::item:selected { background: %5; color: %3; }")
            .arg(pal.baseBg.name(), pal.border.name(), pal.textPrimary.name(), pal.buttonHoverBg.name(),
                 pal.accentSoft.name()));
    // Select the current category so Up/Down start from it.
    const QModelIndexList match = m_model->match(m_model->index(0, 0), kIdRole, QVariant::fromValue(m_current), 1,
                                                 Qt::MatchExactly | Qt::MatchRecursive);
    if (!match.isEmpty()) {
        m_tree->setCurrentIndex(match.first());
    }
    const int rows = 1 + m_categories.size();
    const int height = qBound(80, rows * 30 + 14, 340);
    QPoint at = mapToGlobal(QPoint(0, this->height()));
    const QRect avail = screen() ? screen()->availableGeometry() : QRect(0, 0, 1920, 1080);
    const int width = qMax(this->width(), 260);
    if (at.y() + height > avail.bottom()) {
        at.setY(mapToGlobal(QPoint(0, 0)).y() - height);
    }
    m_popup->setGeometry(at.x(), at.y(), width, height);
    m_popup->show();
    m_tree->setFocus();
}

void CategoryTreePicker::chosen(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    m_popup->hide();
    setCurrentCategory(index.data(kIdRole).toLongLong());
    setFocus();
}

void CategoryTreePicker::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Down || event->key() == Qt::Key_F4) {
        showPopup(); // like a combo box: Down / F4 opens the list
        return;
    }
    QPushButton::keyPressEvent(event);
}

void CategoryTreePicker::paintEvent(QPaintEvent* event) {
    QPushButton::paintEvent(event);
    QPainter p(this);
    const Theme::ThemePalette& pal = Theme::themePalette(Theme::currentTheme());
    const int size = 14;
    const QPixmap chevron = Theme::tintedIcon(QStringLiteral(":/category/chevron-down.svg"),
                                              isEnabled() ? pal.textSecondary : pal.textDisabled, size);
    p.drawPixmap(width() - size - 10, (height() - size) / 2, chevron);
}
