#include "CategoryFilterDialog.h"
#include "PlaylistLibrary.h"
#include "PlaylistModel.h"
#include "DialogSizing.h"
#include "Theme.h"
#include "GlUi.h"

#include <QFileInfo>
#include <QStringListModel>
#include <QTimer>

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

void CategoryFilterDialog::applyIcons() {
    const QString v = Theme::iconVariant(Theme::currentTheme());
    if (m_addRuleButton) {
        m_addRuleButton->setIcon(QIcon(QStringLiteral(":/playlist/%1/add.svg").arg(v)));
    }
    for (RuleRow& r : m_rules) {
        r.remove->setIcon(QIcon(QStringLiteral(":/category/%1/remove-condition.svg").arg(v)));
    }
}

void CategoryFilterDialog::changeEvent(QEvent* event) {
    GlWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ThemeChange) {
        applyIcons();
    }
}

CategoryFilterDialog::CategoryFilterDialog(PlaylistLibrary* library, PlaylistModel* playlist,
                                           const PlaylistFilterInfo* existing, QWidget* parent)
    : GlWindow(existing ? tr("Edit Category") : tr("Build Category"), tr("Saved filter"), parent), m_library(library),
      m_playlist(playlist), m_filterId(existing ? existing->id : 0) {
    setModal(true);
    GlCanvas* c = canvas();

    m_intro = new GlLabel(c);
    m_intro->setText(tr("A category is a saved filter for \"%1\". It shows the playlist's items that match - "
                        "nothing is copied, and items added later that match appear automatically.")
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

    m_filterHeading = new GlLabel(c);
    m_filterHeading->setText(tr("Filter"));
    m_filterHeading->setCaps(true);
    m_filterHeading->setBold(true);
    m_filterHeading->setPointSize(8.5);
    m_filterHeading->setTone(GlLabel::Tone::Accent);
    m_matchLabel = new GlLabel(c);
    m_matchLabel->setText(tr("Show items matching"));
    m_matchLabel->setTone(GlLabel::Tone::Dim);
    m_matchLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_matchCombo = new GlDropdown(c);
    m_matchCombo->setCompact(true);
    m_matchCombo->addItem(tr("all conditions"), true);
    m_matchCombo->addItem(tr("any condition"), false);
    m_matchCombo->setCurrentIndex(0);
    m_matchCombo->setAccessibleName(tr("Show items matching"));

    m_addRuleButton = new GlButton(c, GlButton::Kind::Secondary);
    m_addRuleButton->setText(tr("Add Condition"));
    connect(m_addRuleButton, &GlButton::clicked, this, [this] { addRule(); });

    m_previewLabel = new GlLabel(c);
    m_previewLabel->setBold(true);
    m_previewLabel->setPointSize(10.5);
    m_previewModel = new QStringListModel(this);
    m_previewList = new GlItemView(c);
    m_previewList->setModel(m_previewModel);
    m_previewList->setDelegate(new GlTextDelegate(m_previewList));
    m_previewList->setMode(GlItemView::Mode::List, QSize(200, 30), 0);
    m_previewList->setAccessibleName(tr("Preview of matching items"));

    m_errorLabel = new GlLabel(c);
    m_errorLabel->setColor(QColor(QLatin1String(Theme::kStatusError)));
    m_errorLabel->setPointSize(10);
    m_cancelButton = new GlButton(c, GlButton::Kind::Secondary);
    m_cancelButton->setText(tr("Cancel"));
    connect(m_cancelButton, &GlButton::clicked, this, &QDialog::reject);
    m_okButton = new GlButton(c, GlButton::Kind::Primary);
    m_okButton->setText(existing ? tr("Save") : tr("Create"));
    connect(m_okButton, &GlButton::clicked, this, &CategoryFilterDialog::accept);

    // Preview re-queries (SQL over stored metadata) a moment after edits.
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(150);
    connect(m_previewTimer, &QTimer::timeout, this, &CategoryFilterDialog::updatePreview);
    connect(m_nameEdit, &GlTextInput::textChanged, m_previewTimer, qOverload<>(&QTimer::start));
    connect(m_matchCombo, &GlDropdown::currentIndexChanged, m_previewTimer, qOverload<>(&QTimer::start));

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
    applyIcons();
    updatePreview();
    resize(860, 800);
    setMinimumSize(740, 620);
    m_nameEdit->focusInput();
}

void CategoryFilterDialog::addRule(const FilterRule& rule) {
    GlCanvas* c = canvas();
    RuleRow r;
    r.field = new GlDropdown(c);
    r.field->setCompact(true);
    for (const FieldSpec& f : fields()) {
        r.field->addItem(tr(f.label), QString::fromLatin1(f.key));
    }
    r.field->setAccessibleName(tr("Condition field"));
    r.op = new GlDropdown(c);
    r.op->setCompact(true);
    r.op->setAccessibleName(tr("Condition"));
    r.value = new GlTextInput(c);
    r.value->setAccessibleName(tr("Condition value"));
    r.value->setText(rule.value);
    r.remove = new GlButton(c, GlButton::Kind::Icon);
    r.remove->setToolTip(tr("Remove this condition"));
    r.remove->setAccessibleName(tr("Remove condition"));
    m_rules << r;

    const int fieldIndex = qMax(0, r.field->findData(rule.field.isEmpty() ? QStringLiteral("name") : rule.field));
    r.field->setCurrentIndex(fieldIndex);
    fillOperators(m_rules.last(), rule.op);
    GlDropdown* fieldBox = r.field;
    connect(r.field, &GlDropdown::currentIndexChanged, this, [this, fieldBox] {
        for (RuleRow& row : m_rules) {
            if (row.field == fieldBox) {
                fillOperators(row, QString());
            }
        }
        m_previewTimer->start();
    });
    connect(r.op, &GlDropdown::currentIndexChanged, m_previewTimer, qOverload<>(&QTimer::start));
    connect(r.value, &GlTextInput::textChanged, m_previewTimer, qOverload<>(&QTimer::start));
    GlButton* removeButton = r.remove;
    connect(removeButton, &GlButton::clicked, this, [this, removeButton] { removeRule(removeButton); });
    applyIcons();
    if (!rule.field.isEmpty()) {
        m_previewTimer->start();
    }
    r.value->focusInput();
    canvas()->relayout();
}

void CategoryFilterDialog::removeRule(GlButton* which) {
    if (m_rules.size() <= 1) {
        for (RuleRow& row : m_rules) {
            row.value->clear(); // keep one (empty) condition row
        }
        return;
    }
    for (int i = 0; i < m_rules.size(); ++i) {
        if (m_rules[i].remove == which) {
            RuleRow row = m_rules.takeAt(i);
            canvas()->setFocusedItem(nullptr);
            row.field->deleteLater();
            row.op->deleteLater();
            row.value->deleteLater();
            row.remove->deleteLater();
            break;
        }
    }
    m_previewTimer->start();
    canvas()->relayout();
}

void CategoryFilterDialog::layoutContent(const QRectF& content, const QRectF& footer) {
    const qreal ih = m_intro->heightForWidth(content.width()) + 6;
    m_intro->setRect(QRectF(content.left(), content.top(), content.width(), ih));
    qreal y = content.top() + ih + 10;
    m_nameLabel->setRect(QRectF(content.left(), y, 70, 38));
    m_nameEdit->setRect(QRectF(content.left() + 76, y, content.width() - 76, 38));
    y += 54;
    m_filterHeading->setRect(QRectF(content.left(), y, 160, 34));
    m_matchCombo->setRect(QRectF(content.right() - 190, y, 190, 34));
    m_matchLabel->setRect(QRectF(content.right() - 190 - 8 - 200, y, 200, 34));
    y += 44;
    for (RuleRow& r : m_rules) {
        r.field->setRect(QRectF(content.left(), y, 190, 34));
        r.op->setRect(QRectF(content.left() + 198, y, 170, 34));
        r.value->setRect(QRectF(content.left() + 376, y, content.width() - 376 - 48, 34));
        r.remove->setRect(QRectF(content.right() - 40, y - 1, 40, 36));
        y += 42;
    }
    m_addRuleButton->setRect(QRectF(content.left(), y, 170, 36));
    y += 48;
    m_previewLabel->setRect(QRectF(content.left(), y, content.width(), 24));
    y += 28;
    const qreal errH = 26;
    m_previewList->setRect(QRectF(content.left(), y, content.width(), std::max<qreal>(80, content.bottom() - errH - 6 - y)));
    m_errorLabel->setRect(QRectF(content.left(), content.bottom() - errH, content.width(), errH));
    layoutFooter({m_cancelButton, m_okButton}, footer, 130);
}

void CategoryFilterDialog::updatePreview() {
    const QString error = validationError();
    const FilterDefinition def = definition();
    const int total = m_playlist->count();
    QStringList names;
    if (def.isEmpty()) {
        m_previewLabel->setText(tr("Preview: add a condition to see which items match"));
    } else {
        const QSet<qint64> ids = m_library->matchingItems(m_playlist->id(), &def, QString());
        m_previewLabel->setText(tr("Preview: %1 of %2 items match").arg(ids.size()).arg(total));
        for (int row = 0; row < total; ++row) {
            if (ids.contains(m_playlist->itemIdAt(row))) {
                names << QFileInfo(m_playlist->pathAt(row)).fileName();
            }
        }
    }
    m_previewModel->setStringList(names);
    // Only show an error once the user has typed something to be told about.
    m_errorLabel->setText(m_nameEdit->text().trimmed().isEmpty() && def.isEmpty() ? QString() : error);
    m_okButton->setEnabled(error.isEmpty());
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
            row.value->setPlaceholder(tr(f.placeholder));
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
    GlWindow::accept();
}
