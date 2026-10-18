#include "ThemeEditorDialog.h"
#include "SettingsManager.h"
#include "NotificationManager.h"

#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {
QIcon swatchIcon(const QColor& c) {
    QPixmap pm(28, 16);
    pm.fill(c);
    return QIcon(pm);
}
QString textOn(const QColor& bg) {
    return Theme::contrastRatio(Qt::white, bg) >= Theme::contrastRatio(Qt::black, bg) ? QStringLiteral("#ffffff")
                                                                                       : QStringLiteral("#000000");
}
} // namespace

ThemeEditorDialog::ThemeEditorDialog(SettingsManager* settings, QWidget* parent)
    : QDialog(parent), m_settings(settings), m_theme(Theme::currentTheme()) {
    setWindowTitle(tr("Customize Theme"));
    m_committed = m_settings->themeOverrides(m_theme);
    m_pending = m_committed;

    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    auto* heading = new QLabel(tr("Customizing <b>%1</b>. Changes preview live; nothing is saved until you press Apply.")
                                   .arg(Theme::themeName(m_theme)));
    heading->setWordWrap(true);
    root->addWidget(heading);

    auto* presetRow = new QHBoxLayout();
    presetRow->addWidget(new QLabel(tr("Accent preset:")));
    m_presetCombo = new QComboBox(this);
    m_presetCombo->addItem(tr("Choose..."));
    for (const Theme::AccentPreset& p : Theme::accentPresets()) {
        m_presetCombo->addItem(swatchIcon(p.accent), p.name);
    }
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::activated), this, &ThemeEditorDialog::onPresetChosen);
    presetRow->addWidget(m_presetCombo, 1);
    root->addLayout(presetRow);

    auto* form = new QFormLayout();
    form->setSpacing(6);
    for (const Theme::EditableColor& e : Theme::editableColors()) {
        Row row;
        row.key = e.key;
        row.swatch = new QPushButton(this);
        row.swatch->setMinimumWidth(130);
        row.swatch->setAccessibleName(tr("%1 color").arg(e.label));
        row.swatch->setToolTip(tr("Click to choose a color for: %1").arg(e.label));
        connect(row.swatch, &QPushButton::clicked, this, [this, key = e.key] { pickColor(key); });
        row.reset = new QPushButton(tr("Original"), this);
        row.reset->setToolTip(tr("Return this color to the theme's original value"));
        connect(row.reset, &QPushButton::clicked, this, [this, key = e.key] {
            Theme::ColorOverrides o = m_pending;
            o.remove(key);
            setPending(o);
        });
        auto* h = new QHBoxLayout();
        h->addWidget(row.swatch, 1);
        h->addWidget(row.reset);
        form->addRow(e.label + QLatin1Char(':'), h);
        m_rows << row;
    }
    auto* scrollBody = new QWidget(this);
    scrollBody->setLayout(form);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(scrollBody);
    root->addWidget(scroll, 1);

    m_stateLabel = new QLabel(this);
    m_stateLabel->setWordWrap(true);
    root->addWidget(m_stateLabel);

    auto* tools = new QHBoxLayout();
    auto* contrast = new QPushButton(tr("Check contrast..."), this);
    connect(contrast, &QPushButton::clicked, this, &ThemeEditorDialog::onCheckContrast);
    auto* reset = new QPushButton(tr("Reset to original palette"), this);
    connect(reset, &QPushButton::clicked, this, &ThemeEditorDialog::onResetToOriginal);
    tools->addWidget(contrast);
    tools->addWidget(reset);
    tools->addStretch(1);
    root->addLayout(tools);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    m_revertButton = new QPushButton(tr("Revert"), this);
    connect(m_revertButton, &QPushButton::clicked, this, &ThemeEditorDialog::onRevert);
    m_applyButton = new QPushButton(tr("Apply"), this);
    m_applyButton->setDefault(true);
    connect(m_applyButton, &QPushButton::clicked, this, &ThemeEditorDialog::onApply);
    auto* close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &ThemeEditorDialog::reject);
    buttons->addWidget(m_revertButton);
    buttons->addWidget(m_applyButton);
    buttons->addWidget(close);
    root->addLayout(buttons);

    resize(460, 620);
    refreshRows();
}

bool ThemeEditorDialog::isDirty() const {
    return m_pending != m_committed;
}

void ThemeEditorDialog::refreshRows() {
    const Theme::ThemePalette& p = Theme::themePalette(m_theme);
    for (Row& r : m_rows) {
        for (const Theme::EditableColor& e : Theme::editableColors()) {
            if (e.key != r.key) {
                continue;
            }
            const QColor c = p.*(e.member);
            // Swatch colors are fixed inline so the sample always shows the
            // color itself regardless of the app stylesheet.
            r.swatch->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:%2;border:1px solid #808080;"
                                                   "border-radius:4px;padding:5px 8px;}"
                                                   "QPushButton:focus{border:2px solid %2;}")
                                        .arg(c.name(), textOn(c)));
            r.swatch->setText(c.name().toUpper() + (m_pending.contains(r.key) ? tr("  (custom)") : QString()));
            r.reset->setEnabled(m_pending.contains(r.key));
        }
    }
    m_applyButton->setEnabled(isDirty());
    m_revertButton->setEnabled(isDirty());
    m_stateLabel->setText(isDirty() ? tr("Unsaved changes are previewed on the whole app.")
                          : m_committed.isEmpty() ? tr("Using the original %1 palette.").arg(Theme::themeName(m_theme))
                                                  : tr("Saved customization is active."));
}

void ThemeEditorDialog::previewPending() {
    Theme::setOverrides(m_theme, m_pending);
    Theme::applyTheme(m_theme);
    refreshRows();
}

void ThemeEditorDialog::setPending(const Theme::ColorOverrides& pending) {
    m_pending = pending;
    previewPending();
}

void ThemeEditorDialog::setPendingColor(const QString& key, const QColor& color) {
    Theme::ColorOverrides o = m_pending;
    o.insert(key, color);
    setPending(o);
}

void ThemeEditorDialog::pickColor(const QString& key) {
    QColor current;
    for (const Theme::EditableColor& e : Theme::editableColors()) {
        if (e.key == key) {
            current = Theme::themePalette(m_theme).*(e.member);
        }
    }
    const QColor c = QColorDialog::getColor(current, this, tr("Choose color"));
    if (c.isValid()) {
        setPendingColor(key, c);
    }
}

void ThemeEditorDialog::onPresetChosen(int index) {
    if (index <= 0 || index > Theme::accentPresets().size()) {
        return;
    }
    // A preset only sets the main accent; hover/pressed/text-on-accent
    // follow it automatically (Theme::composePalette) unless customized.
    Theme::ColorOverrides o = m_pending;
    o.remove(QStringLiteral("accentHover"));
    o.remove(QStringLiteral("accentPressed"));
    o.remove(QStringLiteral("selectionText"));
    o.insert(QStringLiteral("accent"), Theme::accentPresets().at(index - 1).accent);
    setPending(o);
    m_presetCombo->setCurrentIndex(0);
}

void ThemeEditorDialog::onResetToOriginal() {
    setPending({});
}

void ThemeEditorDialog::onRevert() {
    setPending(m_committed);
}

void ThemeEditorDialog::onApply() {
    m_settings->setThemeOverrides(m_theme, m_pending);
    m_committed = m_pending;
    refreshRows();
    Notification n;
    n.kind = Notification::Kind::Success;
    n.category = Notification::Category::General;
    n.title = tr("Theme saved");
    n.message = tr("Your %1 colors were saved.").arg(Theme::themeName(m_theme));
    n.key = QStringLiteral("theme-saved");
    n.rateLimit = true;
    if (auto* nm = NotificationManager::instance()) {
        nm->notify(n);
    }
}

void ThemeEditorDialog::onCheckContrast() {
    const QList<Theme::ContrastFix> fixes = Theme::proposeContrastFixes(Theme::themePalette(m_theme));
    if (fixes.isEmpty()) {
        QMessageBox::information(this, tr("Contrast"),
                                 tr("All key text and accent colors already meet readable contrast."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Suggested contrast fixes"));
    auto* l = new QVBoxLayout(&dlg);
    auto* intro = new QLabel(tr("These colors are hard to read. Previewing applies the adjustments to the live "
                                "preview only; your saved colors stay unchanged until you press Apply in the editor."));
    intro->setWordWrap(true);
    l->addWidget(intro);
    auto* list = new QListWidget(&dlg);
    for (const Theme::ContrastFix& f : fixes) {
        list->addItem(new QListWidgetItem(swatchIcon(f.to),
                                          tr("%1 - %2\n%3:1 -> %4:1   (%5 -> %6)")
                                              .arg(f.label, f.reason)
                                              .arg(f.before, 0, 'f', 1)
                                              .arg(f.after, 0, 'f', 1)
                                              .arg(f.from.name().toUpper(), f.to.name().toUpper())));
    }
    l->addWidget(list);
    auto* box = new QDialogButtonBox(&dlg);
    box->addButton(tr("Preview fixes"), QDialogButtonBox::AcceptRole);
    box->addButton(QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    l->addWidget(box);
    dlg.resize(440, 340);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    Theme::ColorOverrides o = m_pending;
    for (const Theme::ContrastFix& f : fixes) {
        o.insert(f.key, f.to);
    }
    setPending(o);
}

void ThemeEditorDialog::reject() {
    if (isDirty()) {
        QMessageBox box(QMessageBox::Question, tr("Unsaved theme changes"),
                        tr("Save your color changes before closing?"), QMessageBox::NoButton, this);
        auto* save = box.addButton(tr("Apply"), QMessageBox::AcceptRole);
        auto* discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        box.addButton(tr("Keep editing"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == save) {
            onApply();
        } else if (box.clickedButton() == discard) {
            onRevert();
        } else {
            return;
        }
    }
    QDialog::reject();
}
