#include "ThemeEditorDialog.h"
#include "GlColor.h"
#include "GlPopups.h"
#include "NotificationManager.h"
#include "SettingsManager.h"

#include <QApplication>

namespace {
// Modal list of proposed contrast fixes; "Preview fixes" only previews.
class ContrastFixDialog : public GlWindow {
public:
    ContrastFixDialog(const QList<Theme::ContrastFix>& fixes, QWidget* parent)
        : GlWindow(QObject::tr("Suggested contrast fixes"), QObject::tr("Readability"), parent, false), m_fixes(fixes) {
        setModal(true);
        GlCanvas* c = canvas();
        m_intro = new GlLabel(c);
        m_intro->setText(QObject::tr("These colors are hard to read. Previewing applies the adjustments to the live "
                                     "preview only; your saved colors stay unchanged until you press Apply in the editor."));
        m_intro->setWordWrap(true);
        m_intro->setPointSize(10);
        m_intro->setTone(GlLabel::Tone::Dim);
        m_intro->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        for (const Theme::ContrastFix& f : fixes) {
            Entry e;
            e.from = new GlSwatchButton(c);
            e.from->setColor(f.from);
            e.from->setEnabled(false);
            e.to = new GlSwatchButton(c);
            e.to->setColor(f.to);
            e.to->setEnabled(false);
            e.text = new GlLabel(c);
            e.text->setText(QObject::tr("%1 - %2\n%3:1 -> %4:1")
                                .arg(f.label, f.reason)
                                .arg(f.before, 0, 'f', 1)
                                .arg(f.after, 0, 'f', 1));
            e.text->setWordWrap(true);
            e.text->setPointSize(10);
            e.text->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            m_entries << e;
        }
        m_apply = new GlButton(c, GlButton::Kind::Primary);
        m_apply->setText(QObject::tr("Preview fixes"));
        m_cancel = new GlButton(c, GlButton::Kind::Secondary);
        m_cancel->setText(QObject::tr("Cancel"));
        QObject::connect(m_apply, &GlButton::clicked, this, &QDialog::accept);
        QObject::connect(m_cancel, &GlButton::clicked, this, &QDialog::reject);
        resize(620, std::min<int>(720, 240 + int(fixes.size()) * 62));
        setMinimumSize(560, 320);
    }
    void layoutContent(const QRectF& content, const QRectF& footer) override {
        const qreal ih = m_intro->heightForWidth(content.width()) + 6;
        m_intro->setRect(QRectF(content.left(), content.top(), content.width(), ih));
        qreal y = content.top() + ih + 12;
        for (Entry& e : m_entries) {
            e.from->setRect(QRectF(content.left(), y, 96, 40));
            e.to->setRect(QRectF(content.left() + 104, y, 96, 40));
            e.text->setRect(QRectF(content.left() + 214, y - 4, content.width() - 214, 48));
            y += 56;
        }
        layoutFooter({m_cancel, m_apply}, footer, 140);
    }

private:
    struct Entry {
        GlSwatchButton* from;
        GlSwatchButton* to;
        GlLabel* text;
    };
    QList<Theme::ContrastFix> m_fixes;
    QList<Entry> m_entries;
    GlLabel* m_intro;
    GlButton* m_apply;
    GlButton* m_cancel;
};
} // namespace

ThemeEditorDialog::ThemeEditorDialog(SettingsManager* settings, QWidget* parent)
    : GlWindow(tr("Customize Theme"), tr("Colors"), parent, false), m_settings(settings), m_theme(Theme::currentTheme()) {
    setModal(true);
    m_committed = m_settings->themeOverrides(m_theme);
    m_pending = m_committed;
    GlCanvas* c = canvas();

    m_heading = new GlLabel(c);
    m_heading->setText(tr("Customizing %1. Changes preview live; nothing is saved until you press Apply.")
                           .arg(Theme::themeName(m_theme)));
    m_heading->setWordWrap(true);
    m_heading->setPointSize(10);
    m_heading->setTone(GlLabel::Tone::Dim);
    m_heading->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    m_presetLabel = new GlLabel(c);
    m_presetLabel->setText(tr("Accent preset"));
    m_presetLabel->setPointSize(10.5);
    m_presetCombo = new GlDropdown(c);
    m_presetCombo->setCompact(true);
    m_presetCombo->addItem(tr("Choose..."));
    for (const Theme::AccentPreset& p : Theme::accentPresets()) {
        m_presetCombo->addItem(p.name);
    }
    m_presetCombo->setCurrentIndex(0);
    connect(m_presetCombo, &GlDropdown::currentIndexChanged, this, &ThemeEditorDialog::onPresetChosen);

    for (const Theme::EditableColor& e : Theme::editableColors()) {
        Row row;
        row.key = e.key;
        row.label = new GlLabel(c);
        row.label->setText(e.label);
        row.label->setPointSize(10.5);
        row.swatch = new GlSwatchButton(c);
        row.swatch->setAccessibleName(tr("%1 color").arg(e.label));
        row.swatch->setToolTip(tr("Click to choose a color for: %1").arg(e.label));
        connect(row.swatch, &GlSwatchButton::clicked, this, [this, key = e.key] { pickColor(key); });
        row.reset = new GlButton(c, GlButton::Kind::Secondary);
        row.reset->setText(tr("Original"));
        row.reset->setToolTip(tr("Return this color to the theme's original value"));
        connect(row.reset, &GlButton::clicked, this, [this, key = e.key] {
            Theme::ColorOverrides o = m_pending;
            o.remove(key);
            setPending(o);
        });
        m_rows << row;
    }
    m_stateLabel = new GlLabel(c);
    m_stateLabel->setPointSize(9.5);
    m_stateLabel->setTone(GlLabel::Tone::Dim);
    m_contrastButton = new GlButton(c, GlButton::Kind::Secondary);
    m_contrastButton->setText(tr("Check contrast..."));
    connect(m_contrastButton, &GlButton::clicked, this, &ThemeEditorDialog::onCheckContrast);
    m_resetAllButton = new GlButton(c, GlButton::Kind::Secondary);
    m_resetAllButton->setText(tr("Reset to original palette"));
    connect(m_resetAllButton, &GlButton::clicked, this, &ThemeEditorDialog::onResetToOriginal);
    m_revertButton = new GlButton(c, GlButton::Kind::Secondary);
    m_revertButton->setText(tr("Revert"));
    connect(m_revertButton, &GlButton::clicked, this, &ThemeEditorDialog::onRevert);
    m_applyButton = new GlButton(c, GlButton::Kind::Primary);
    m_applyButton->setText(tr("Apply"));
    connect(m_applyButton, &GlButton::clicked, this, &ThemeEditorDialog::onApply);
    m_closeButton = new GlButton(c, GlButton::Kind::Secondary);
    m_closeButton->setText(tr("Close"));
    connect(m_closeButton, &GlButton::clicked, this, &ThemeEditorDialog::reject);

    resize(600, 800);
    setMinimumSize(560, 720);
    refreshRows();
}

void ThemeEditorDialog::layoutContent(const QRectF& content, const QRectF& footer) {
    const qreal hh = m_heading->heightForWidth(content.width()) + 6;
    m_heading->setRect(QRectF(content.left(), content.top(), content.width(), hh));
    qreal y = content.top() + hh + 10;
    m_presetLabel->setRect(QRectF(content.left(), y, 160, 36));
    m_presetCombo->setRect(QRectF(content.left() + 170, y, content.width() - 170, 36));
    y += 48;
    for (Row& r : m_rows) {
        r.label->setRect(QRectF(content.left(), y, 190, 34));
        r.swatch->setRect(QRectF(content.left() + 200, y, content.width() - 200 - 100, 34));
        r.reset->setRect(QRectF(content.right() - 90, y, 90, 34));
        y += 39;
    }
    y += 4;
    m_stateLabel->setRect(QRectF(content.left(), y, content.width(), 22));
    y += 28;
    m_contrastButton->setRect(QRectF(content.left(), y, 190, 38));
    m_resetAllButton->setRect(QRectF(content.left() + 200, y, 240, 38));
    layoutFooter({m_revertButton, m_applyButton, m_closeButton}, footer, 110);
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
            r.swatch->setColor(p.*(e.member));
            r.swatch->setCustom(m_pending.contains(r.key));
            r.reset->setEnabled(m_pending.contains(r.key));
        }
    }
    m_applyButton->setEnabled(isDirty());
    m_revertButton->setEnabled(isDirty());
    m_stateLabel->setText(isDirty() ? tr("Unsaved changes are previewed on the whole app.")
                          : m_committed.isEmpty() ? tr("Using the original %1 palette.").arg(Theme::themeName(m_theme))
                                                  : tr("Saved customization is active."));
    canvas()->relayout();
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
    QString label;
    for (const Theme::EditableColor& e : Theme::editableColors()) {
        if (e.key == key) {
            current = Theme::themePalette(m_theme).*(e.member);
            label = e.label;
        }
    }
    const QColor c = GlColorDialog::getColor(current, this, tr("Color: %1").arg(label));
    if (c.isValid()) {
        setPendingColor(key, c);
    }
}

void ThemeEditorDialog::onPresetChosen(int index) {
    if (m_presetSync || index <= 0 || index > Theme::accentPresets().size()) {
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
    m_presetSync = true;
    m_presetCombo->setCurrentIndex(0);
    m_presetSync = false;
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
        GlMessageBox::information(this, tr("Contrast"),
                                  tr("All key text and accent colors already meet readable contrast."));
        return;
    }
    ContrastFixDialog dlg(fixes, this);
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
        GlMessageBox box(QMessageBox::Question, tr("Unsaved theme changes"),
                         tr("Save your color changes before closing?"), QMessageBox::NoButton, this);
        auto* save = box.addButton(tr("Apply"), QMessageBox::AcceptRole);
        auto* discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        auto* keep = box.addButton(tr("Keep editing"), QMessageBox::RejectRole);
        box.setDefaultButton(save);
        box.setEscapeButton(keep);
        box.exec();
        if (box.clickedButton() == save) {
            onApply();
        } else if (box.clickedButton() == discard) {
            onRevert();
        } else {
            return;
        }
    }
    GlWindow::reject();
}
