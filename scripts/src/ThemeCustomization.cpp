#include "Theme.h"

#include <QtMath>
#include <algorithm>

// User customization of the four themes: the editable-color table, how a
// sparse override set composes over an original palette, accent presets and
// the WCAG contrast helper behind the theme editor's "Check contrast".
// Nothing here writes settings or touches QApplication.
namespace Theme {

const QList<EditableColor>& editableColors() {
    static const QList<EditableColor> colors = {
        {QStringLiteral("accent"), QStringLiteral("Main accent"), &ThemePalette::accent},
        {QStringLiteral("accentHover"), QStringLiteral("Accent (hover)"), &ThemePalette::accentHover},
        {QStringLiteral("accentPressed"), QStringLiteral("Accent (pressed)"), &ThemePalette::accentPressed},
        {QStringLiteral("selectionText"), QStringLiteral("Text on accent"), &ThemePalette::selectionText},
        {QStringLiteral("windowBg"), QStringLiteral("Window background"), &ThemePalette::windowBg},
        {QStringLiteral("panelBg"), QStringLiteral("Panels and cards"), &ThemePalette::panelBg},
        {QStringLiteral("baseBg"), QStringLiteral("Inputs and lists"), &ThemePalette::baseBg},
        {QStringLiteral("buttonBg"), QStringLiteral("Buttons"), &ThemePalette::buttonBg},
        {QStringLiteral("textPrimary"), QStringLiteral("Text"), &ThemePalette::textPrimary},
        {QStringLiteral("textSecondary"), QStringLiteral("Secondary text"), &ThemePalette::textSecondary},
        {QStringLiteral("border"), QStringLiteral("Borders"), &ThemePalette::border},
        {QStringLiteral("borderStrong"), QStringLiteral("Strong borders and focus"), &ThemePalette::borderStrong},
    };
    return colors;
}

namespace {
double relativeLuminance(const QColor& c) {
    auto lin = [](qreal v) { return v <= 0.03928 ? v / 12.92 : qPow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}

QColor opaque(QColor c) {
    c.setAlpha(255);
    return c;
}
} // namespace

double contrastRatio(const QColor& a, const QColor& b) {
    const double la = relativeLuminance(opaque(a));
    const double lb = relativeLuminance(opaque(b));
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

ThemePalette composePalette(const ThemePalette& base, const ColorOverrides& o) {
    ThemePalette p = base;
    for (const EditableColor& e : editableColors()) {
        const auto it = o.constFind(e.key);
        if (it != o.constEnd() && it->isValid()) {
            p.*(e.member) = *it;
        }
    }
    // Values the user did not set explicitly follow the accent they did set,
    // so one accent pick re-colors hover/pressed/selection coherently.
    if (o.contains(QStringLiteral("accent"))) {
        const QColor a = p.accent;
        if (!o.contains(QStringLiteral("accentHover"))) {
            p.accentHover = p.isDark ? a.lighter(118) : a.lighter(112);
        }
        if (!o.contains(QStringLiteral("accentPressed"))) {
            p.accentPressed = a.darker(125);
        }
        p.accentSoft = QColor(a.red(), a.green(), a.blue(), base.accentSoft.alpha());
        if (!o.contains(QStringLiteral("selectionText"))) {
            p.selectionText = contrastRatio(Qt::white, a) >= contrastRatio(Qt::black, a) ? QColor(Qt::white)
                                                                                         : QColor(Qt::black);
        }
    }
    return p;
}

const QList<AccentPreset>& accentPresets() {
    static const QList<AccentPreset> presets = {
        {QStringLiteral("Electric Violet"), QColor(0x7c, 0x3a, 0xed)},
        {QStringLiteral("Magenta Pop"), QColor(0xd9, 0x2b, 0x8f)},
        {QStringLiteral("Ocean Blue"), QColor(0x1f, 0x7a, 0xe0)},
        {QStringLiteral("Teal Surge"), QColor(0x0e, 0x9f, 0x9a)},
        {QStringLiteral("Sunset Orange"), QColor(0xe8, 0x6a, 0x1c)},
        {QStringLiteral("Crimson"), QColor(0xd1, 0x34, 0x4a)},
    };
    return presets;
}

namespace {
struct Pair {
    const char* fg;
    const char* bg;
    double minRatio;
    const char* reason;
};

QColor* slot(ThemePalette& p, const QString& key) {
    for (const EditableColor& e : editableColors()) {
        if (e.key == key) {
            return &(p.*(e.member));
        }
    }
    return nullptr;
}

// Moves `fg` toward white or black (whichever direction increases contrast
// against `bg`) in small steps until `minRatio` is met or it hits the limit.
QColor nudgeForContrast(const QColor& fg, const QColor& bg, double minRatio) {
    const bool towardWhite = relativeLuminance(bg) < 0.5;
    const QColor target = towardWhite ? QColor(Qt::white) : QColor(Qt::black);
    QColor c = opaque(fg);
    for (int i = 0; i < 40 && contrastRatio(c, bg) < minRatio; ++i) {
        c = QColor::fromRgbF(c.redF() + (target.redF() - c.redF()) * 0.08,
                             c.greenF() + (target.greenF() - c.greenF()) * 0.08,
                             c.blueF() + (target.blueF() - c.blueF()) * 0.08);
    }
    return c;
}
} // namespace

QList<ContrastFix> proposeContrastFixes(const ThemePalette& palette) {
    static const Pair pairs[] = {
        {"textPrimary", "windowBg", 4.5, "Text on the window background"},
        {"textPrimary", "panelBg", 4.5, "Text on panels and cards"},
        {"textPrimary", "baseBg", 4.5, "Text in inputs and lists"},
        {"textPrimary", "buttonBg", 4.5, "Text on buttons"},
        {"textSecondary", "windowBg", 4.5, "Secondary text on the window background"},
        {"selectionText", "accent", 4.5, "Text on the accent color"},
        {"accent", "windowBg", 3.0, "Accent against the window background"},
    };

    ThemePalette work = palette; // fixes accumulate so later pairs see earlier ones
    QHash<QString, ContrastFix> byKey;
    QStringList order;
    for (const Pair& pr : pairs) {
        QColor* fg = slot(work, QLatin1String(pr.fg));
        QColor* bg = slot(work, QLatin1String(pr.bg));
        if (!fg || !bg || contrastRatio(*fg, *bg) >= pr.minRatio) {
            continue;
        }
        const QString key = QLatin1String(pr.fg);
        QColor fixed;
        if (key == QLatin1String("selectionText")) {
            fixed = contrastRatio(Qt::white, *bg) >= contrastRatio(Qt::black, *bg) ? QColor(Qt::white)
                                                                                   : QColor(Qt::black);
        } else {
            fixed = nudgeForContrast(*fg, *bg, pr.minRatio);
        }
        if (fixed == opaque(*fg)) {
            continue;
        }
        if (!byKey.contains(key)) {
            ContrastFix f;
            f.key = key;
            for (const EditableColor& e : editableColors()) {
                if (e.key == key) {
                    f.label = e.label;
                }
            }
            f.reason = QLatin1String(pr.reason);
            f.from = *fg;
            f.before = contrastRatio(*fg, *bg);
            byKey.insert(key, f);
            order << key;
        }
        ContrastFix& f = byKey[key];
        f.to = fixed;
        f.after = contrastRatio(fixed, *bg);
        *fg = fixed;
    }
    QList<ContrastFix> out;
    for (const QString& k : order) {
        out << byKey.value(k);
    }
    return out;
}

} // namespace Theme
