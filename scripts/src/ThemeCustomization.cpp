#include "Theme.h"

#include <QtMath>
#include <algorithm>

// User customization of the four themes: the single editable accent, how it
// composes over an original palette (hover/pressed/text-on-accent derived,
// WCAG contrast enforced), accent presets and the contrast helper.
// Nothing here writes settings or touches QApplication.
namespace Theme {

const QList<EditableColor>& editableColors() {
    // Only the accent is user-editable; hover/pressed/soft/text-on-accent are
    // derived from it in composePalette().
    static const QList<EditableColor> colors = {
        {QStringLiteral("accent"), QStringLiteral("Accent color"), &ThemePalette::accent},
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

// Moves `fg` toward white or black (whichever direction increases contrast
// against `bg`) in small steps until `minRatio` is met or it hits the limit.
QColor nudgeForContrast(const QColor& fg, const QColor& bg, double minRatio);
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
        // WCAG non-text contrast (3:1) against the window background, so any
        // chosen accent stays visible on both dark and light themes.
        p.accent = nudgeForContrast(p.accent, p.windowBg, 3.0);
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

void setAccent(const QColor& accent) {
    ColorOverrides o;
    if (accent.isValid()) {
        o.insert(QStringLiteral("accent"), accent);
    }
    for (auto t : {AppTheme::DarkAurora, AppTheme::LightAurora, AppTheme::DarkOnyx, AppTheme::LightOnyx}) {
        setOverrides(t, o);
    }
}

const QList<AccentPreset>& accentPresets() {
    static const QList<AccentPreset> presets = {
        {QStringLiteral("Electric Violet"), QColor(0x7c, 0x3a, 0xed)},
        {QStringLiteral("Indigo"), QColor(0x4f, 0x46, 0xe5)},
        {QStringLiteral("Ocean Blue"), QColor(0x1f, 0x7a, 0xe0)},
        {QStringLiteral("Sky"), QColor(0x0e, 0x9e, 0xd9)},
        {QStringLiteral("Teal Surge"), QColor(0x0e, 0x9f, 0x9a)},
        {QStringLiteral("Emerald"), QColor(0x2e, 0x9e, 0x4f)},
        {QStringLiteral("Lime"), QColor(0x84, 0xb8, 0x1a)},
        {QStringLiteral("Amber"), QColor(0xe0, 0x9a, 0x0c)},
        {QStringLiteral("Sunset Orange"), QColor(0xe8, 0x6a, 0x1c)},
        {QStringLiteral("Crimson"), QColor(0xd1, 0x34, 0x4a)},
        {QStringLiteral("Magenta Pop"), QColor(0xd9, 0x2b, 0x8f)},
        {QStringLiteral("Rose"), QColor(0xf4, 0x5b, 0x8d)},
    };
    return presets;
}

namespace {
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

} // namespace Theme
