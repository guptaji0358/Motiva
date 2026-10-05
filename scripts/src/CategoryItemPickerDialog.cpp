#include "CategoryItemPickerDialog.h"
#include "PlaylistLibrary.h"
#include "PlaylistModel.h"
#include "Theme.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
constexpr int kItemIdRole = Qt::UserRole + 10;
constexpr int kRowRole = Qt::UserRole + 11;
constexpr int kIconSize = 40;
} // namespace

CategoryItemPickerDialog::CategoryItemPickerDialog(PlaylistLibrary* library, PlaylistModel* playlist,
                                                   const QString& existingName, const QSet<qint64>& preselected,
                                                   qint64 existingId, QWidget* parent)
    : QDialog(parent), m_library(library), m_playlist(playlist), m_existingId(existingId),
      m_creating(existingName.isEmpty()) {
    setWindowTitle(m_creating ? tr("Select from Playlist") : tr("Manage Selection"));
    setMinimumSize(520, 520);
    resize(580, 640);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(8);

    auto* title = new QLabel(m_creating ? tr("Select from \"%1\"").arg(playlist->name())
                                        : tr("Select media for \"%1\"").arg(existingName),
                             this);
    title->setObjectName(QStringLiteral("appTitle"));
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSize(tf.pointSize() + 3);
    title->setFont(tf);
    root->addWidget(title);
    auto* intro = new QLabel(tr("Tick the items of \"%1\" that belong in this category. Nothing is copied - the category "
                                "only points at items already in the playlist.")
                                 .arg(playlist->name()),
                             this);
    intro->setObjectName(QStringLiteral("secondaryText"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    if (m_creating) {
        root->addWidget(new QLabel(tr("Name:"), this));
        m_nameEdit = new QLineEdit(this);
        m_nameEdit->setPlaceholderText(tr("e.g. Anime, Games, Nature"));
        m_nameEdit->setMaxLength(80);
        m_nameEdit->setAccessibleName(tr("Category name"));
        root->addWidget(m_nameEdit);
    }

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search these items…"));
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(tr("Search items to select"));
    root->addWidget(m_search);

    m_countLabel = new QLabel(this);
    m_countLabel->setObjectName(QStringLiteral("sectionLabel"));
    QFont bf = m_countLabel->font();
    bf.setBold(true);
    m_countLabel->setFont(bf);
    root->addWidget(m_countLabel);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setIconSize(QSize(kIconSize, kIconSize));
    m_list->setUniformItemSizes(true);
    m_list->setAccessibleName(tr("Items of the playlist"));
    root->addWidget(m_list, 1);

    for (int row = 0; row < playlist->count(); ++row) {
        const QString file = QFileInfo(playlist->pathAt(row)).fileName();
        auto* item = new QListWidgetItem(file, m_list);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        const qint64 id = playlist->itemIdAt(row);
        item->setCheckState(preselected.contains(id) ? Qt::Checked : Qt::Unchecked);
        item->setData(kItemIdRole, id);
        item->setData(kRowRole, row);
        item->setToolTip(playlist->pathAt(row));
        const QImage thumb = playlist->index(row, 0).data(PlaylistModel::ThumbnailRole).value<QImage>();
        if (!thumb.isNull()) {
            item->setIcon(QIcon(QPixmap::fromImage(thumb)));
        }
    }
    // Previews arrive lazily - fill in the icons as they load.
    connect(playlist, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex& from, const QModelIndex& to) {
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem* item = m_list->item(i);
            const int row = item->data(kRowRole).toInt();
            if (row >= from.row() && row <= to.row() && item->icon().isNull()) {
                const QImage thumb = m_playlist->index(row, 0).data(PlaylistModel::ThumbnailRole).value<QImage>();
                if (!thumb.isNull()) {
                    item->setIcon(QIcon(QPixmap::fromImage(thumb)));
                }
            }
        }
    });

    auto* bulk = new QHBoxLayout();
    auto* selectAll = new QPushButton(tr("Select All"), this);
    selectAll->setToolTip(tr("Tick every item currently shown (respects the search)"));
    auto* clear = new QPushButton(tr("Clear Selection"), this);
    clear->setToolTip(tr("Untick every item"));
    bulk->addWidget(selectAll);
    bulk->addWidget(clear);
    bulk->addStretch();
    root->addLayout(bulk);
    connect(selectAll, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < m_list->count(); ++i) {
            if (!m_list->item(i)->isHidden()) {
                m_list->item(i)->setCheckState(Qt::Checked);
            }
        }
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < m_list->count(); ++i) {
            m_list->item(i)->setCheckState(Qt::Unchecked);
        }
    });

    m_errorLabel = new QLabel(this);
    m_errorLabel->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::kStatusError)));
    m_errorLabel->setWordWrap(true);
    root->addWidget(m_errorLabel);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    auto* cancel = new QPushButton(tr("Cancel"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    m_okButton = new QPushButton(m_creating ? tr("Add Selected") : tr("Save"), this);
    m_okButton->setObjectName(QStringLiteral("primaryButton"));
    m_okButton->setDefault(true);
    connect(m_okButton, &QPushButton::clicked, this, &CategoryItemPickerDialog::accept);
    buttons->addWidget(m_okButton);
    root->addLayout(buttons);

    connect(m_search, &QLineEdit::textChanged, this, &CategoryItemPickerDialog::applySearch);
    connect(m_list, &QListWidget::itemChanged, this, &CategoryItemPickerDialog::updateCount);
    if (m_nameEdit) {
        connect(m_nameEdit, &QLineEdit::textChanged, this, &CategoryItemPickerDialog::updateCount);
    }
    updateCount();
    (m_creating ? static_cast<QWidget*>(m_nameEdit) : static_cast<QWidget*>(m_search))->setFocus();
}

QSet<qint64> CategoryItemPickerDialog::selectedItemIds() const {
    QSet<qint64> ids;
    for (int i = 0; i < m_list->count(); ++i) {
        const QListWidgetItem* item = m_list->item(i);
        if (item->checkState() == Qt::Checked) {
            ids.insert(item->data(kItemIdRole).toLongLong());
        }
    }
    return ids;
}

// Search only hides rows in this list; checked state is kept for hidden rows.
void CategoryItemPickerDialog::applySearch() {
    const QStringList terms = m_search->text().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* item = m_list->item(i);
        const QString haystack = m_playlist->pathAt(item->data(kRowRole).toInt());
        bool match = true;
        for (const QString& t : terms) {
            if (!haystack.contains(t, Qt::CaseInsensitive)) {
                match = false;
                break;
            }
        }
        item->setHidden(!match);
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
    m_countLabel->setText(tr("Selected: %1 of %2").arg(selected).arg(m_list->count()));
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
    QDialog::accept();
}
