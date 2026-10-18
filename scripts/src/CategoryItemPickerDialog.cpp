#include "CategoryItemPickerDialog.h"
#include "PlaylistLibrary.h"
#include "PlaylistModel.h"
#include "Theme.h"

#include <QFileInfo>
#include <QIcon>
#include <QKeyEvent>
#include <QPixmap>
#include <QStandardItemModel>

namespace {
constexpr int kItemIdRole = Qt::UserRole + 10;
constexpr int kRowRole = Qt::UserRole + 11;
} // namespace

CategoryItemPickerDialog::CategoryItemPickerDialog(PlaylistLibrary* library, PlaylistModel* playlist,
                                                   const QString& existingName, const QSet<qint64>& preselected,
                                                   qint64 existingId, QWidget* parent)
    : GlWindow(existingName.isEmpty() ? tr("Select from \"%1\"").arg(playlist->name())
                                      : tr("Select media for \"%1\"").arg(existingName),
               existingName.isEmpty() ? tr("Select from playlist") : tr("Manage selection"), parent),
      m_library(library), m_playlist(playlist), m_existingId(existingId), m_creating(existingName.isEmpty()) {
    setModal(true);
    GlCanvas* c = canvas();

    m_intro = new GlLabel(c);
    m_intro->setText(tr("Tick the items of \"%1\" that belong in this category. Nothing is copied - the category "
                        "only points at items already in the playlist.")
                         .arg(playlist->name()));
    m_intro->setWordWrap(true);
    m_intro->setPointSize(10);
    m_intro->setTone(GlLabel::Tone::Dim);
    m_intro->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    m_nameLabel = new GlLabel(c);
    m_nameLabel->setText(tr("Name"));
    m_nameLabel->setTone(GlLabel::Tone::Dim);
    m_nameEdit = new GlTextInput(c);
    m_nameEdit->setPlaceholder(tr("e.g. Anime, Games, Nature"));
    m_nameEdit->setAccessibleName(tr("Category name"));
    m_nameLabel->setVisible(m_creating);
    m_nameEdit->setVisible(m_creating);

    m_search = new GlTextInput(c);
    m_search->setPlaceholder(tr("Search these items…"));
    m_search->setClearable(true);
    m_search->setAccessibleName(tr("Search items to select"));

    m_countLabel = new GlLabel(c);
    m_countLabel->setBold(true);
    m_countLabel->setPointSize(10.5);

    m_model = new QStandardItemModel(this);
    for (int row = 0; row < playlist->count(); ++row) {
        auto* item = new QStandardItem(QFileInfo(playlist->pathAt(row)).fileName());
        const qint64 id = playlist->itemIdAt(row);
        item->setData(preselected.contains(id) ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        item->setData(id, kItemIdRole);
        item->setData(row, kRowRole);
        item->setToolTip(playlist->pathAt(row));
        const QImage thumb = playlist->index(row, 0).data(PlaylistModel::ThumbnailRole).value<QImage>();
        if (!thumb.isNull()) {
            item->setIcon(QIcon(QPixmap::fromImage(thumb)));
        }
        m_model->appendRow(item);
    }
    m_list = new GlItemView(c);
    m_list->setModel(m_model);
    m_list->setDelegate(new GlCheckRowDelegate(m_list));
    m_list->setMode(GlItemView::Mode::List, QSize(200, 54), 2);
    m_list->setAccessibleName(tr("Items of the playlist"));
    connect(m_list, &GlItemView::clicked, this, &CategoryItemPickerDialog::toggleRow);
    m_list->setKeyFilter([this](QKeyEvent* e) {
        if (e->key() == Qt::Key_Space && !(e->modifiers() & Qt::ControlModifier)) {
            toggleRow(m_list->currentRow());
            return true;
        }
        return false;
    });
    // Previews arrive lazily - fill in the icons as they load.
    connect(playlist, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex& from, const QModelIndex& to) {
        for (int i = 0; i < m_model->rowCount(); ++i) {
            QStandardItem* item = m_model->item(i);
            const int row = item->data(kRowRole).toInt();
            if (row >= from.row() && row <= to.row() && item->icon().isNull()) {
                const QImage thumb = m_playlist->index(row, 0).data(PlaylistModel::ThumbnailRole).value<QImage>();
                if (!thumb.isNull()) {
                    item->setIcon(QIcon(QPixmap::fromImage(thumb)));
                }
            }
        }
    });

    m_selectAll = new GlButton(c, GlButton::Kind::Secondary);
    m_selectAll->setText(tr("Select All"));
    m_selectAll->setToolTip(tr("Tick every item currently shown (respects the search)"));
    m_clear = new GlButton(c, GlButton::Kind::Secondary);
    m_clear->setText(tr("Clear Selection"));
    m_clear->setToolTip(tr("Untick every item"));
    connect(m_selectAll, &GlButton::clicked, this, [this] {
        for (int i = 0; i < m_model->rowCount(); ++i) {
            if (!m_list->isRowHidden(i)) {
                m_model->item(i)->setData(Qt::Checked, Qt::CheckStateRole);
            }
        }
        updateCount();
    });
    connect(m_clear, &GlButton::clicked, this, [this] {
        for (int i = 0; i < m_model->rowCount(); ++i) {
            m_model->item(i)->setData(Qt::Unchecked, Qt::CheckStateRole);
        }
        updateCount();
    });

    m_errorLabel = new GlLabel(c);
    m_errorLabel->setColor(QColor(QLatin1String(Theme::kStatusError)));
    m_errorLabel->setPointSize(10);
    m_cancelButton = new GlButton(c, GlButton::Kind::Secondary);
    m_cancelButton->setText(tr("Cancel"));
    connect(m_cancelButton, &GlButton::clicked, this, &QDialog::reject);
    m_okButton = new GlButton(c, GlButton::Kind::Primary);
    m_okButton->setText(m_creating ? tr("Add Selected") : tr("Save"));
    connect(m_okButton, &GlButton::clicked, this, &CategoryItemPickerDialog::accept);

    connect(m_search, &GlTextInput::textChanged, this, &CategoryItemPickerDialog::applySearch);
    connect(m_nameEdit, &GlTextInput::textChanged, this, &CategoryItemPickerDialog::updateCount);
    connect(m_nameEdit, &GlTextInput::editingFinished, this, [this] { m_search->focusInput(); });
    updateCount();
    resize(760, 800);
    setMinimumSize(640, 600);
    (m_creating ? m_nameEdit : m_search)->focusInput();
}

void CategoryItemPickerDialog::toggleRow(int row) {
    if (row < 0 || row >= m_model->rowCount()) {
        return;
    }
    QStandardItem* item = m_model->item(row);
    item->setData(item->data(Qt::CheckStateRole).toInt() == Qt::Checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
    updateCount();
}

void CategoryItemPickerDialog::layoutContent(const QRectF& content, const QRectF& footer) {
    const qreal ih = m_intro->heightForWidth(content.width()) + 6;
    m_intro->setRect(QRectF(content.left(), content.top(), content.width(), ih));
    qreal y = content.top() + ih + 8;
    if (m_creating) {
        m_nameLabel->setRect(QRectF(content.left(), y, 70, 38));
        m_nameEdit->setRect(QRectF(content.left() + 76, y, content.width() - 76, 38));
        y += 46;
    }
    m_search->setRect(QRectF(content.left(), y, content.width(), 38));
    y += 46;
    m_countLabel->setRect(QRectF(content.left(), y, content.width(), 24));
    y += 28;
    const qreal bottomRows = 8 + 38 + 8 + 24;
    m_list->setRect(QRectF(content.left(), y, content.width(), std::max<qreal>(120, content.bottom() - bottomRows - y)));
    const qreal by = content.bottom() - bottomRows + 8;
    m_selectAll->setRect(QRectF(content.left(), by, 130, 38));
    m_clear->setRect(QRectF(content.left() + 140, by, 160, 38));
    m_errorLabel->setRect(QRectF(content.left(), content.bottom() - 24, content.width(), 24));
    layoutFooter({m_cancelButton, m_okButton}, footer, 140);
}

QSet<qint64> CategoryItemPickerDialog::selectedItemIds() const {
    QSet<qint64> ids;
    for (int i = 0; i < m_model->rowCount(); ++i) {
        const QStandardItem* item = m_model->item(i);
        if (item->data(Qt::CheckStateRole).toInt() == Qt::Checked) {
            ids.insert(item->data(kItemIdRole).toLongLong());
        }
    }
    return ids;
}

// Search only hides rows in this list; checked state is kept for hidden rows.
void CategoryItemPickerDialog::applySearch() {
    const QStringList terms = m_search->text().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 0; i < m_model->rowCount(); ++i) {
        const QString haystack = m_playlist->pathAt(m_model->item(i)->data(kRowRole).toInt());
        bool match = true;
        for (const QString& t : terms) {
            if (!haystack.contains(t, Qt::CaseInsensitive)) {
                match = false;
                break;
            }
        }
        m_list->setRowHidden(i, !match);
    }
}

QString CategoryItemPickerDialog::nameError() const {
    if (!m_creating) {
        return QString();
    }
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        return tr("Give the category a name.");
    }
    if (name.compare(tr("All"), Qt::CaseInsensitive) == 0 || name.compare(QLatin1String("All"), Qt::CaseInsensitive) == 0) {
        return tr("\"All\" is built in - choose another name.");
    }
    for (const PlaylistFilterInfo& f : m_library->filters(m_playlist->id())) {
        if (f.id != m_existingId && f.name.compare(name, Qt::CaseInsensitive) == 0) {
            return tr("This playlist already has a category called \"%1\".").arg(f.name);
        }
    }
    return QString();
}

void CategoryItemPickerDialog::updateCount() {
    const int selected = selectedItemIds().size();
    m_countLabel->setText(tr("Selected: %1 of %2").arg(selected).arg(m_model->rowCount()));
    const QString error = nameError();
    m_errorLabel->setText(m_creating && m_nameEdit->text().trimmed().isEmpty() ? QString() : error);
    // An empty selection is allowed when managing (clears the category) but
    // pointless when creating.
    m_okButton->setEnabled(error.isEmpty() && (!m_creating || selected > 0));
}

void CategoryItemPickerDialog::accept() {
    if (!nameError().isEmpty()) {
        updateCount();
        return;
    }
    if (m_creating) {
        m_name = m_nameEdit->text().trimmed();
    }
    GlWindow::accept();
}
