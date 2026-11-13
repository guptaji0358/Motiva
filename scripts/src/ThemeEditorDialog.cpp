#include "ThemeEditorDialog.h"
#include "GlColor.h"
#include "NotificationManager.h"
#include "SettingsManager.h"

#include <QPainter>
#include <QPainterPath>
#include <functional>

// A round, single-select color chip: a ring + check mark marks the selection.
class AccentChip : public GlItem {
public:
    AccentChip(GlCanvas* canvas, const QColor& color, std::function<void()> onPick)
        : GlItem(canvas), m_color(color), m_onPick(std::move(onPick)) {}
    void setColor(const QColor& c) { m_color = c; update(); }
    QColor color() const { return m_color; }
    void setSelected(bool on) { m_selected = on; update(); }
    bool isInteractive() const override { return true; }
    void activate() override { m_onPick(); }
    void paint(QPainter& p, const GlUi::Style& s) override {
        const QRectF r = rect();
        const qreal d = std::min(r.width(), r.height());
        const QPointF c = r.center();
        const qreal rad = d / 2 - 5;
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        if (m_selected || hoverAmount() > 0.01) {
            QColor ring = m_selected ? s.text : s.textDim;
            ring.setAlphaF(m_selected ? 1.0 : 0.7 * hoverAmount());
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(ring, 2.2));
            p.drawEllipse(c, d / 2 - 1.5, d / 2 - 1.5);
        }
        p.setPen(QPen(s.glassEdge, 1.0));
        p.setBrush(m_color);
        p.drawEllipse(c, rad, rad);
        if (m_selected) {
            const double lum = 0.299 * m_color.redF() + 0.587 * m_color.greenF() + 0.114 * m_color.blueF();
            p.setPen(QPen(lum > 0.6 ? QColor(Qt::black) : QColor(Qt::white), 2.4, Qt::SolidLine, Qt::RoundCap,
                          Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            QPainterPath tick;
            tick.moveTo(c.x() - rad * 0.38, c.y() + rad * 0.02);
            tick.lineTo(c.x() - rad * 0.08, c.y() + rad * 0.34);
            tick.lineTo(c.x() + rad * 0.42, c.y() - rad * 0.30);
            p.drawPath(tick);
        }
        if (hasFocus()) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(s.accent2, 2.0));
            p.drawEllipse(c, d / 2 + 1, d / 2 + 1);
        }
        p.restore();
    }

private:
    QColor m_color;
    std::function<void()> m_onPick;
    bool m_selected = false;
};

ThemeEditorDialog::ThemeEditorDialog(SettingsManager* settings, QWidget* parent)
    : GlWindow(tr("Accent Color"), tr("Appearance"), parent, false), m_settings(settings) {
    setModal(true);
    m_committed = m_settings->accentColor();
    m_pending = m_committed;
    GlCanvas* c = canvas();

    m_heading = new GlLabel(c);
    m_heading->setText(tr("Choose one accent color for all of Motiva. It works with every theme and previews "
                          "live; nothing is saved until you press Apply."));
    m_heading->setWordWrap(true);
    m_heading->setPointSize(10);
    m_heading->setTone(GlLabel::Tone::Dim);
    m_heading->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    auto* def = new AccentChip(c, Theme::basePalette(Theme::currentTheme()).accent, [this] { setPending(QColor()); });
    def->setToolTip(tr("Theme default"));
    def->setAccessibleName(tr("Theme default accent"));
    m_chips << def;
    for (const Theme::AccentPreset& pr : Theme::accentPresets()) {
        auto* chip = new AccentChip(c, pr.accent, [this, col = pr.accent] { setPending(col); });
        chip->setToolTip(pr.name);
        chip->setAccessibleName(pr.name);
        m_chips << chip;
    }

    m_nameLabel = new GlLabel(c);
    m_nameLabel->setPointSize(11);
    m_nameLabel->setBold(true);
    m_customButton = new GlButton(c, GlButton::Kind::Secondary);
    m_customButton->setText(tr("Custom color..."));
    m_customButton->setToolTip(tr("Pick any other color as the accent"));
    connect(m_customButton, &GlButton::clicked, this, &ThemeEditorDialog::pickCustom);

    m_previewLabel = new GlLabel(c);
    m_previewLabel->setText(tr("Preview"));
    m_previewLabel->setPointSize(10);
    m_previewLabel->setTone(GlLabel::Tone::Dim);
    m_previewSwatch = new GlSwatchButton(c);
    m_previewSwatch->setEnabled(false);
    m_previewPrimary = new GlButton(c, GlButton::Kind::Primary);
    m_previewPrimary->setText(tr("Primary button"));
    m_previewTab = new GlButton(c, GlButton::Kind::Tab);
    m_previewTab->setText(tr("Selected tab"));
    m_previewTab->setSelected(true);
    m_infoLabel = new GlLabel(c);
    m_infoLabel->setPointSize(9.5);
    m_infoLabel->setTone(GlLabel::Tone::Dim);
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    m_applyButton = new GlButton(c, GlButton::Kind::Primary);
    m_applyButton->setText(tr("Apply"));
    connect(m_applyButton, &GlButton::clicked, this, &ThemeEditorDialog::onApply);
    m_closeButton = new GlButton(c, GlButton::Kind::Secondary);
    m_closeButton->setText(tr("Cancel"));
    connect(m_closeButton, &GlButton::clicked, this, &ThemeEditorDialog::reject);

    resize(560, 560);
    setMinimumSize(520, 520);
    refresh();
}

void ThemeEditorDialog::layoutContent(const QRectF& content, const QRectF& footer) {
    const qreal hh = m_heading->heightForWidth(content.width()) + 6;
    m_heading->setRect(QRectF(content.left(), content.top(), content.width(), hh));
    qreal y = content.top() + hh + 12;
    const int cols = 7;
    const qreal cell = std::min<qreal>(54, content.width() / cols);
    for (int i = 0; i < m_chips.size(); ++i) {
        m_chips[i]->setRect(QRectF(content.left() + (i % cols) * cell, y + (i / cols) * cell, cell, cell));
    }
    y += ((m_chips.size() + cols - 1) / cols) * cell + 10;
    m_nameLabel->setRect(QRectF(content.left(), y, content.width() - 170, 36));
    m_customButton->setRect(QRectF(content.right() - 160, y, 160, 36));
    y += 52;
    m_previewLabel->setRect(QRectF(content.left(), y, content.width(), 24));
    y += 28;
    m_previewSwatch->setRect(QRectF(content.left(), y, content.width(), 44));
    y += 54;
    m_previewPrimary->setRect(QRectF(content.left(), y, 190, 40));
    m_previewTab->setRect(QRectF(content.left() + 202, y, 170, 40));
    y += 52;
    m_infoLabel->setRect(QRectF(content.left(), y, content.width(), m_infoLabel->heightForWidth(content.width()) + 6));
    layoutFooter({m_closeButton, m_applyButton}, footer, 120);
}

void ThemeEditorDialog::refresh() {
    const QColor effective = Theme::themePalette(Theme::currentTheme()).accent;
    int custom = 1;
    for (int i = 0; i < m_chips.size(); ++i) {
        const bool sel = (i == 0) ? !m_pending.isValid()
                                  : m_pending.isValid() && m_chips[i]->color().rgb() == m_pending.rgb();
        m_chips[i]->setSelected(sel);
        if (sel) {
            custom = 0;
        }
    }
    m_customButton->setSelected(custom && m_pending.isValid());
    QString name = tr("Theme default");
    if (m_pending.isValid()) {
        name = tr("Custom color");
        for (int i = 1; i < m_chips.size(); ++i) {
            if (m_chips[i]->color().rgb() == m_pending.rgb()) {
                name = Theme::accentPresets().at(i - 1).name;
            }
        }
    }
    m_nameLabel->setText(name);
    m_previewSwatch->setColor(effective);
    const Theme::ThemePalette& pal = Theme::themePalette(Theme::currentTheme());
    QString info = tr("Contrast: %1:1 against the window, %2:1 for text on the accent.")
                       .arg(Theme::contrastRatio(pal.accent, pal.windowBg), 0, 'f', 1)
                       .arg(Theme::contrastRatio(pal.selectionText, pal.accent), 0, 'f', 1);
    if (m_pending.isValid() && pal.accent.rgb() != m_pending.rgb()) {
        info += tr(" The color was adjusted slightly to stay readable on this theme.");
    }
    m_infoLabel->setText(info);
    m_applyButton->setEnabled(m_pending != m_committed);
    canvas()->relayout();
}

void ThemeEditorDialog::setPending(const QColor& accent) {
    m_pending = accent;
    Theme::setAccent(m_pending);
    Theme::applyTheme(Theme::currentTheme());
    refresh();
}

void ThemeEditorDialog::pickCustom() {
    const QColor start = m_pending.isValid() ? m_pending : Theme::themePalette(Theme::currentTheme()).accent;
    const QColor c = GlColorDialog::getColor(start, this, tr("Accent color"));
    if (c.isValid()) {
        setPending(c);
    }
}

void ThemeEditorDialog::onApply() {
    m_settings->setAccentColor(m_pending);
    m_committed = m_pending;
    refresh();
    Notification n;
    n.kind = Notification::Kind::Success;
    n.category = Notification::Category::General;
    n.title = tr("Accent saved");
    n.message = tr("Your accent color was saved.");
    n.key = QStringLiteral("theme-saved");
    n.rateLimit = true;
    if (auto* nm = NotificationManager::instance()) {
        nm->notify(n);
    }
    GlWindow::accept();
}

void ThemeEditorDialog::reject() {
    if (m_pending != m_committed) {
        setPending(m_committed); // restore the saved accent
    }
    GlWindow::reject();
}
