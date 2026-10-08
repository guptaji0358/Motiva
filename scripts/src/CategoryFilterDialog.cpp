#include "CategoryFilterDialog.h"
#include "PlaylistLibrary.h"
#include "PlaylistModel.h"
#include "DialogSizing.h"
#include "Theme.h"
#include "IconButton.h"

#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {
// The metadata a rule can test - all of it stored per media record.
struct FieldSpec {
    const char* key;
    const char* label;
    QList<QPair<const char*, const char*>> ops; // (op key, label)
    const char* placeholder;
};
const QList<FieldSpec>& fields() {
    static const QList<FieldSpec> list = {
        {"name", QT_TRANSLATE_NOOP("CategoryFilterDialog", "File name"),
         {{"contains", QT_TRANSLATE_NOOP("CategoryFilterDialog", "contains")},
          {"not_contains", QT_TRANSLATE_NOOP("CategoryFilterDialog", "does not contain")}},
         QT_TRANSLATE_NOOP("CategoryFilterDialog", "e.g. anime")},
        {"path", QT_TRANSLATE_NOOP("CategoryFilterDialog", "Path or folder"),
         {{"contains", QT_TRANSLATE_NOOP("CategoryFilterDialog", "contains")},
          {"not_contains", QT_TRANSLATE_NOOP("CategoryFilterDialog", "does not contain")}},
         QT_TRANSLATE_NOOP("CategoryFilterDialog", "e.g. \\Games\\")},
        {"extension", QT_TRANSLATE_NOOP("CategoryFilterDialog", "Extension"),
         {{"is", QT_TRANSLATE_NOOP("CategoryFilterDialog", "is")},
          {"is_not", QT_TRANSLATE_NOOP("CategoryFilterDialog", "is not")}},
         QT_TRANSLATE_NOOP("CategoryFilterDialog", "e.g. png")},
        {"size_mb", QT_TRANSLATE_NOOP("CategoryFilterDialog", "File size (MB)"),
         {{"at_least", QT_TRANSLATE_NOOP("CategoryFilterDialog", "is at least")},
          {"at_most", QT_TRANSLATE_NOOP("CategoryFilterDialog", "is at most")}},
         QT_TRANSLATE_NOOP("CategoryFilterDialog", "e.g. 5")},
    };
    return list;
}
} // namespace

void CategoryFilterDialog::applyRemoveIcon(IconButton* button) {
    const QString v = Theme::iconVariant(Theme::currentTheme());
    auto icon = [&v](const char* name) {
        return QIcon(QStringLiteral(":/category/%1/%2.svg").arg(v, QLatin1String(name)));
    };
    button->setStateIcon(icon("remove-condition"), icon("remove-condition-hover"), QIcon(),
                         icon("remove-condition-disabled"));
}

void CategoryFilterDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ThemeChange) {
        const auto buttons = findChildren<IconButton*>(QStringLiteral("removeConditionButton"));
        for (IconButton* b : buttons) {
            applyRemoveIcon(b);
        }
    }
}

CategoryFilterDialog::CategoryFilterDialog(PlaylistLibrary* library, PlaylistModel* playlist,
                                           const PlaylistFilterInfo* existing, QWidget* parent)
    : QDialog(parent), m_library(library), m_playlist(playlist), m_filterId(existing ? existing->id : 0) {
    setWindowTitle(existing ? tr("Edit Category") : tr("Build Category"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(8);
    auto* title = new QLabel(existing ? tr("Edit Category") : tr("Build Virtual Category"), this);
    title->setObjectName(QStringLiteral("appTitle"));
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSize(tf.pointSize() + 3);
    title->setFont(tf);
    root->addWidget(title);
    auto* intro = new QLabel(tr("A category is a saved filter for \"%1\". It shows the playlist's items that match - "
                                "nothing is copied, and items added later that match appear automatically.")
                                 .arg(playlist->name()),
                             this);
    intro->setObjectName(QStringLiteral("secondaryText"));
    intro->setWordWrap(true);
    root->addWidget(intro);
    root->addSpacing(4);

    root->addWidget(new QLabel(tr("Name:"), this));
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(tr("e.g. Anime, Games, Nature"));
    m_nameEdit->setMaxLength(80);
    root->addWidget(m_nameEdit);
    root->addSpacing(4);

    auto* filterHead = new QHBoxLayout();
    auto* filterLabel = new QLabel(tr("Filter"), this);
    filterLabel->setObjectName(QStringLiteral("sectionLabel"));
    QFont bf = filterLabel->font();
    bf.setBold(true);
    filterLabel->setFont(bf);
    filterHead->addWidget(filterLabel, 1);
    filterHead->addWidget(new QLabel(tr("Show items matching"), this));
    m_matchCombo = new QComboBox(this);
    m_matchCombo->addItem(tr("all conditions"), true);
    m_matchCombo->addItem(tr("any condition"), false);
    m_matchCombo->setAccessibleName(tr("Show items matching"));
    filterHead->addWidget(m_matchCombo);
    root->addLayout(filterHead);

    m_rulesLayout = new QVBoxLayout();
    m_rulesLayout->setSpacing(6);
    root->addLayout(m_rulesLayout);
    auto* addRuleButton = new QPushButton(tr("+ Add Condition"), this);
    connect(addRuleButton, &QPushButton::clicked, this, [this] { addRule(); });
    auto* addRow = new QHBoxLayout();
    addRow->addWidget(addRuleButton);
    addRow->addStretch();
    root->addLayout(addRow);

    m_previewLabel = new QLabel(this);
    m_previewLabel->setObjectName(QStringLiteral("sectionLabel"));
    m_previewLabel->setFont(bf);
    root->addWidget(m_previewLabel);
    m_previewList = new QListWidget(this);
    m_previewList->setSelectionMode(QAbstractItemView::NoSelection);
    m_previewList->setFocusPolicy(Qt::NoFocus);
    m_previewList->setAccessibleName(tr("Preview of matching items"));
    root->addWidget(m_previewList, 1);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::kStatusError)));
    m_errorLabel->setWordWrap(true);
    root->addWidget(m_errorLabel);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    auto* cancel = new QPushButton(tr("Cancel"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    m_okButton = new QPushButton(existing ? tr("Save") : tr("Create"), this);
    m_okButton->setObjectName(QStringLiteral("primaryButton"));
    m_okButton->setDefault(true);
    connect(m_okButton, &QPushButton::clicked, this, &CategoryFilterDialog::accept);
    buttons->addWidget(m_okButton);
    root->addLayout(buttons);

    // Preview re-queries (SQL over stored metadata) a moment after edits.
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(150);
    connect(m_previewTimer, &QTimer::timeout, this, &CategoryFilterDialog::updatePreview);
    connect(m_nameEdit, &QLineEdit::textChanged, m_previewTimer, qOverload<>(&QTimer::start));
    connect(m_matchCombo, &QComboBox::currentIndexChanged, m_previewTimer, qOverload<>(&QTimer::start));

    if (existing) {
        m_nameEdit->setText(existing->name);
        m_matchCombo->setCurrentIndex(existing->definition.matchAll ? 0 : 1);
        for (const FilterRule& r : existing->definition.rules) {
            addRule(r);
        }
    }
    if (m_rules.isEmpty()) {
        addRule({QStringLiteral("name"), QStringLiteral("contains"), QString()});
    }
    updatePreview();
    m_nameEdit->setFocus();
    DialogSizing::applyComfortableSize(this, QSize(600, 600));
}

void CategoryFilterDialog::addRule(const FilterRule& rule) {
    RuleRow r;
    r.row = new QWidget(this);
    auto* layout = new QHBoxLayout(r.row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    r.field = new QComboBox(r.row);
    for (const FieldSpec& f : fields()) {
        r.field->addItem(tr(f.label), QString::fromLatin1(f.key));
    }
    r.field->setAccessibleName(tr("Condition field"));
    r.op = new QComboBox(r.row);
    r.op->setAccessibleName(tr("Condition"));
    r.value = new QLineEdit(r.row);
    r.value->setAccessibleName(tr("Condition value"));
    r.value->setText(rule.value);
    auto* remove = new IconButton(r.row);
    remove->setObjectName(QStringLiteral("removeConditionButton"));
    applyRemoveIcon(remove);
    remove->setToolTip(tr("Remove this condition"));
    remove->setAccessibleName(tr("Remove condition"));
    remove->setFixedWidth(40);
    layout->addWidget(r.field);
    layout->addWidget(r.op);
    layout->addWidget(r.value, 1);
    layout->addWidget(remove);
    m_rulesLayout->addWidget(r.row);
    m_rules << r;

    const int fieldIndex = qMax(0, r.field->findData(rule.field.isEmpty() ? QStringLiteral("name") : rule.field));
    r.field->setCurrentIndex(fieldIndex);
    fillOperators(m_rules.last(), rule.op);
    QWidget* rowWidget = r.row;
    connect(r.field, &QComboBox::currentIndexChanged, this, [this, rowWidget] {
        for (RuleRow& row : m_rules) {
            if (row.row == rowWidget) {
                fillOperators(row, QString());
            }
        }
        m_previewTimer->start();
    });
    connect(r.op, &QComboBox::currentIndexChanged, m_previewTimer, qOverload<>(&QTimer::start));
    connect(r.value, &QLineEdit::textChanged, m_previewTimer, qOverload<>(&QTimer::start));
    connect(remove, &QPushButton::clicked, this, [this, rowWidget] {
        if (m_rules.size() <= 1) {
            for (RuleRow& row : m_rules) {
                row.value->clear(); // keep one (empty) condition row
            }
            return;
        }
        for (int i = 0; i < m_rules.size(); ++i) {
            if (m_rules[i].row == rowWidget) {
                m_rules.takeAt(i).row->deleteLater();
                break;
            }
        }
        m_previewTimer->start();
    });
    if (!rule.field.isEmpty()) {
        m_previewTimer->start();
    }
    r.value->setFocus();
}

void CategoryFilterDialog::fillOperators(RuleRow& row, const QString& selectOp) {
    const QString key = row.field->currentData().toString();
    row.op->blockSignals(true);
    row.op->clear();
    for (const FieldSpec& f : fields()) {
        if (key == QLatin1String(f.key)) {
            for (const auto& op : f.ops) {
                row.op->addItem(tr(op.second), QString::fromLatin1(op.first));
            }
            row.value->setPlaceholderText(tr(f.placeholder));
        }
    }
    const int opIndex = row.op->findData(selectOp);
    row.op->setCurrentIndex(opIndex >= 0 ? opIndex : 0);
    row.op->blockSignals(false);
}

FilterDefinition CategoryFilterDialog::definition() const {
    FilterDefinition def;
    def.matchAll = m_matchCombo->currentData().toBool();
    for (const RuleRow& r : m_rules) {
        def.rules.push_back({r.field->currentData().toString(), r.op->currentData().toString(), r.value->text().trimmed()});
    }
    return def;
}

QString CategoryFilterDialog::validationError() const {
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        return tr("Give the category a name.");
    }
    if (name.compare(tr("All"), Qt::CaseInsensitive) == 0 || name.compare(QLatin1String("All"), Qt::CaseInsensitive) == 0) {
        return tr("\"All\" is built in - choose another name.");
    }
    for (const PlaylistFilterInfo& f : m_library->filters(m_playlist->id())) {
        if (f.id != m_filterId && f.name.compare(name, Qt::CaseInsensitive) == 0) {
            return tr("This playlist already has a category called \"%1\".").arg(f.name);
        }
    }
    const FilterDefinition def = definition();
    if (def.isEmpty()) {
        return tr("Add at least one condition.");
    }
    for (const FilterRule& r : def.rules) {
        bool ok = true;
        if (r.field == QLatin1String("size_mb") && !r.isEmpty()) {
            r.value.toDouble(&ok);
        }
        if (!ok) {
            return tr("File size must be a number of megabytes.");
        }
    }
    return QString();
}

void CategoryFilterDialog::updatePreview() {
    const QString error = validationError();
    const FilterDefinition def = definition();
    const int total = m_playlist->count();
    m_previewList->clear();
    if (def.isEmpty()) {
        m_previewLabel->setText(tr("Preview: add a condition to see which items match"));
    } else {
        const QSet<qint64> ids = m_library->matchingItems(m_playlist->id(), &def, QString());
        m_previewLabel->setText(tr("Preview: %1 of %2 items match").arg(ids.size()).arg(total));
        for (int row = 0; row < total; ++row) {
            if (ids.contains(m_playlist->itemIdAt(row))) {
                m_previewList->addItem(QFileInfo(m_playlist->pathAt(row)).fileName());
            }
        }
    }
    // Only show an error once the user has typed something to be told about.
    m_errorLabel->setText(m_nameEdit->text().trimmed().isEmpty() && def.isEmpty() ? QString() : error);
    m_okButton->setEnabled(error.isEmpty());
}

void CategoryFilterDialog::accept() {
    if (!validationError().isEmpty()) {
        updatePreview();
        return;
    }
    const QString name = m_nameEdit->text().trimmed();
    if (m_filterId > 0) {
        if (!m_library->updateFilter(m_filterId, m_playlist->id(), name, definition())) {
            return;
        }
    } else {
        m_filterId = m_library->createFilter(m_playlist->id(), name, definition());
        if (m_filterId <= 0) {
            return; // errorOccurred already explained it
        }
    }
    QDialog::accept();
}
