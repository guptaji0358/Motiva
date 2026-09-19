#pragma once

#include <QString>
#include <QColor>

// Centralized Motiva visual design system - see the "Motiva Theme System"
// task. A Theme is a single, complete, explicitly-defined visual identity
// (background/panel/text/border/accent colors, corner language, hover/
// pressed behavior, icon variant) - never derived from another theme at
// runtime (no "light = dark inverted", no relying on Qt's native QPalette
// to auto-convert anything). Exactly one AppTheme is ever active; there is
// no separate Appearance/Visual Style axis anymore.
namespace Theme {

// The four themes. Values are stable (persisted via
// SettingsManager::setTheme()) - do not renumber existing entries.
enum class AppTheme {
    DarkAurora = 0,
    LightAurora = 1,
    DarkOnyx = 2,
    LightOnyx = 3,
};

// Every field a theme needs to render Motiva's UI, as literal colors -
// see themePalette()'s four independently-authored definitions in
// Theme.cpp. Corner radius is NOT here: it's a property of the Aurora vs
// Onyx *design language* (soft vs sharp corners), not of light vs dark,
// so it's baked into auroraStyleSheet()/onyxStyleSheet() directly instead
// (both Aurora themes share one radius scale, both Onyx themes share
// another).
struct ThemePalette {
    QColor windowBg;       // main window/dialog background
    QColor panelBg;        // section/card surfaces
    QColor baseBg;         // inputs, combo popups
    QColor altBg;          // alternate rows / subtle recessed surfaces
    QColor textPrimary;
    QColor textSecondary;
    QColor textDisabled;
    QColor border;         // subtle/default borders
    QColor borderStrong;   // hover/focus-adjacent borders
    QColor buttonBg;
    QColor buttonHoverBg;
    QColor buttonPressedBg;
    QColor accent;
    QColor accentHover;
    QColor accentPressed;
    QColor accentSoft;      // translucent accent, e.g. list-item selection
    QColor selectionText;   // text color on top of an accent-filled surface
    bool isDark;
};

// The theme most recently passed to applyTheme() - readable anywhere
// (icon lookups, the transition overlay's own paint) without threading
// the current selection through every call site. Defaults to DarkAurora
// until applyTheme() has run at least once (see main.cpp).
AppTheme currentTheme();

// The one, fully deterministic source of truth for `theme`'s look:
// swaps QApplication's QPalette AND its global stylesheet together, both
// generated from the same literal ThemePalette - see themePalette().
void applyTheme(AppTheme theme);

// The literal, independently-authored color set for `theme` - see
// Theme.cpp. Never computed from another theme at runtime.
const ThemePalette& themePalette(AppTheme theme);

// "Dark Aurora" etc - for the Settings Theme combo box, in the exact
// order that combo lists them (matches AppTheme's own ordinal order).
QString themeName(AppTheme theme);

// "DarkAurora" etc - a stable persistence key, independent of themeName()
// so a future retranslation/rename of the display name can never change
// what's stored in the registry.
QString themeSettingsKey(AppTheme theme);
AppTheme themeFromSettingsKey(const QString& key, AppTheme fallback = AppTheme::DarkAurora);

bool isDarkTheme(AppTheme theme);

// "dark" or "light" - the icon-variant subfolder shared by every theme.
// Aurora and Onyx deliberately use the SAME icon artwork per light/dark
// (only their QSS-driven chrome differs) - see CLAUDE.md's asset-layout
// notes and the "Only create theme-specific icon files when the artwork
// genuinely needs to differ" instruction. Used everywhere an icon path is
// built, e.g. ":/settings-icon/" + iconVariant(theme) + "/settings.svg".
QString iconVariant(AppTheme theme);

// One-time, pure (no I/O) migration from the old two-axis Appearance
// (0=System/1=Light/2=Dark) x Visual Style (0=ModernAurora/1=MotivaOnyx)
// settings to a single AppTheme - see SettingsManager::theme(). "System"
// maps to Dark (the axis that was already reported working correctly),
// matching this task's note that Light was the broken one, not System.
AppTheme migrateLegacySettings(int legacyAppearance, int legacyUiStyle);

// --- Colors that intentionally stay constant across every theme ---

// Status text colors - kept legible/consistent regardless of the active
// theme rather than reformulated per-theme (a warning should always read
// as "warning", independent of which theme is active).
constexpr const char* kStatusNeutral = "#808080";
constexpr const char* kStatusWarning = "#c98a1a";
constexpr const char* kStatusSuccess = "#2e9e4f";
constexpr const char* kStatusError = "#d64545";

// The video preview surface is a deliberately fixed-dark backdrop
// regardless of the active theme (the same convention most media/player
// apps use) - see MainWindow.cpp's kPreviewSurfaceStyle.
constexpr const char* kPreviewSurfaceBg = "#161616";
constexpr const char* kPreviewSurfaceBorder = "#2c2c2c";
constexpr const char* kPreviewText = "#8a8a8a";
constexpr const char* kPreviewTextStrong = "#c9c9c9";
constexpr const char* kPreviewTextFaint = "#5a5a5a";

// Same fixed brand green used by the (theme-independent) preview surface
// and the theme-transition overlay's own decorative sweep - not one of
// the four themes' own (per-theme) accent colors.
constexpr const char* kBrandAccent = "#3fae5c";

// Corner radius scale used by the fixed-dark preview surface above and
// its own light-on-dark controls (which do not belong to either Aurora
// or Onyx's own per-theme-family radius language, applied inside
// appStyleSheet() instead).
constexpr int kRadiusSmall = 8;
constexpr int kRadiusMedium = 10;

// The one application-wide stylesheet for `theme`, built from its
// ThemePalette - applied once via QApplication::setStyleSheet (main.cpp)
// and again on every live theme change (SettingsDialog).
QString appStyleSheet(AppTheme theme);

} // namespace Theme
