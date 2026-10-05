#include "CategoryBuilderDialog.h"
#include "PlaylistLibrary.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QScrollArea>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

CategoryBuilderDialog::CategoryBuilderDialog(PlaylistLibrary* library, qint64 categoryId, QWidget* parent)
    : QDialog(parent), m_library(library), m_categoryId(categoryId) {
    const bool editing = categoryId > 0;
    setWindowTitle(editing ? tr("Choose Playlists") : tr("Build Category"));
    setMinimumSize(460, 480);
    resize(500, 560);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(8);

    auto* title = new QLabel(editing ? tr("Playlists in \"%1\"").arg(library->categoryName(categoryId))
                                     : tr("Build Category"),
                             this);
    title->setObjectName(QStringLiteral("appTitle"));
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSize(tf.pointSize() + 3);
    title->setFont(tf);
    root->addWidget(title);
    auto* intro = new QLabel(tr("Group your existing playlists so they're easier to find. A playlist can be in "
                                "several categories; nothing about the playlists themselves changes."),
                             this);
    intro->setObjectName(QStringLiteral("secondaryText"));
    intro->setWordWrap(true);
    root->addWidget(intro);
    root->addSpacing(4);

    if (!editing) {
        root->addWidget(new QLabel(tr("Category name:"), this));
        m_nameEdit = new QLineEdit(this);
        m_nameEdit->setPlaceholderText(tr("e.g. Anime, Games, Nature"));
        m_nameEdit->setMaxLength(120);
        connect(m_nameEdit, &QLineEdit::textChanged, this, &CategoryBuilderDialog::updateState);
        root->addWidget(m_nameEdit);
        root->addSpacing(4);
    }

    auto* playlistsLabel = new QLabel(tr("Playlists"), this);
    playlistsLabel->setObjectName(QStringLiteral("sectionLabel"));
    QFont bf = playlistsLabel->font();
    bf.setBold(true);
    playlistsLabel->setFont(bf);
    root->addWidget(playlistsLabel);
    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(tr("🔎  Filter playlists"));
    m_filterEdit->setClearButtonEnabled(true);
    connect(m_filterEdit, &QLineEdit::textChanged, this, &CategoryBuilderDialog::applyFilter);
    root->addWidget(m_filterEdit);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* listWidget = new QWidget(scroll);
    auto* listLayout = new QVBoxLayout(listWidget);
    listLayout->setContentsMargins(4, 4, 4, 4);
    listLayout->setSpacing(8);
    const PlaylistListModel* playlists = library->listModel();
    const QList<qint64> members = editing ? library->playlistsIn(categoryId) : QList<qint64>{};
    for (int row = 0; row < playlists->rowCount(); ++row) {
        const QModelIndex idx = playlists->index(row);
        const bool video = idx.data(PlaylistListModel::TypeRole).toInt() == static_cast<int>(PlaylistType::Video);
        const int count = idx.data(PlaylistListModel::CountRole).toInt();
        auto* box = new QCheckBox(
            QStringLiteral("%1   —   %2").arg(idx.data().toString(),
                                              video ? (count == 1 ? tr("Video playlist, 1 video")
                                                                  : tr("Video playlist, %1 videos").arg(count))
                                                    : (count == 1 ? tr("Image playlist, 1 image")
                                                                  : tr("Image playlist, %1 images").arg(count))),
            listWidget);
        box->setProperty("playlistId", playlists->idAt(row));
        box->setChecked(members.contains(playlists->idAt(row)));
        connect(box, &QCheckBox::toggled, this, &CategoryBuilderDialog::updateState);
        listLayout->addWidget(box);
        m_boxes << box;
    }
    if (m_boxes.isEmpty()) {
        auto* none = new QLabel(tr("There are no playlists yet - create one first."), listWidget);
        none->setObjectName(QStringLiteral("secondaryText"));
        listLayout->addWidget(none);
    }
    listLayout->addStretch();
    scroll->setWidget(listWidget);
    root->addWidget(scroll, 1);

    m_selectedLabel = new QLabel(this);
    m_selectedLabel->setObjectName(QStringLiteral("secondaryText"));
    root->addWidget(m_selectedLabel);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    auto* cancel = new QPushButton(tr("Cancel"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    m_okButton = new QPushButton(editing ? tr("Save") : tr("Create Category"), this);
    m_okButton->setObjectName(QStringLiteral("primaryButton"));
    m_okButton->setDefault(true);
    connect(m_okButton, &QPushButton::clicked, this, &CategoryBuilderDialog::accept);
    buttons->addWidget(m_okButton);
    root->addLayout(buttons);

    updateState();
    (m_nameEdit ? m_nameEdit : m_filterEdit)->setFocus();
}

void CategoryBuilderDialog::preselect(const QList<qint64>& playlistIds) {
    for (QCheckBox* box : std::as_const(m_boxes)) {
        if (playlistIds.contains(box->property("playlistId").toLongLong())) {
            box->setChecked(true);
        }
    }
}

void CategoryBuilderDialog::applyFilter() {
    const QStringList terms = m_filterEdit->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (QCheckBox* box : std::as_const(m_boxes)) {
        bool show = true;
        for (const QString& t : terms) {
            show = show && box->text().contains(t, Qt::CaseInsensitive);
        }
        // Ticked playlists stay visible so a selection is never hidden.
        box->setVisible(show || box->isChecked());
    }
}

QList<qint64> CategoryBuilderDialog::checkedPlaylists() const {
    QList<qint64> ids;
    for (const QCheckBox* box : m_boxes) {
        if (box->isChecked()) {
            ids << box->property("playlistId").toLongLong();
        }
    }
    return ids;
}

void CategoryBuilderDialog::updateState() {
    const int n = checkedPlaylists().size();
    m_selectedLabel->setText(n == 1 ? tr("1 playlist selected") : tr("%1 playlists selected").arg(n));
    m_okButton->setEnabled(!m_nameEdit || !m_nameEdit->text().trimmed().isEmpty());
}

void CategoryBuilderDialog::accept() {
    const QList<qint64> ids = checkedPlaylists();
    if (m_categoryId > 0) {
        if (!m_library->setCategoryPlaylists(m_categoryId, ids)) {
            return; // errorOccurred already explained it
        }
    } else {
        const QString name = m_nameEdit->text().trimmed();
        if (name.isEmpty()) {
            return;
        }
        m_categoryId = m_library->createCategory(name, ids);
        if (m_categoryId <= 0) {
            QMessageBox::warning(this, windowTitle(), tr("The category could not be created."));
            return;
        }
    }
    QDialog::accept();
}
