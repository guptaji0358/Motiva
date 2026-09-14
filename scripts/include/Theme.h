#pragma once

#include <QString>
#include <QPalette>

// Centralized Motiva visual design system - colors, typography, and the
// single application-wide QSS stylesheet. This is a pure styling module
// (see the "Motiva UI Styling Pass" task): no widget behavior, layout
// structure, or backend logic lives here, only how existing widgets look.
//
// Base surfaces/text intentionally use Qt's "palette(...)" QSS functions
// rather than fixed hex values, so the app keeps following the OS
// light/dark palette exactly as it did before this styling pass (see
// MainWindow.cpp's original comment: "This app has no app-level
// light/dark theme toggle of its own - it simply follows the OS window
// palette"). Only the accent/status colors and a couple of deliberately
// fixed-dark surfaces (the video preview, matching prior behavior) are
// fixed hex values, consistent with how they already worked before this
// pass.
namespace Theme {

// Two genuinely different, professional, Motiva-branded visual styles the
// user can pick in Settings (see the "Two distinct Motiva visual styles"
// task) - not two recolors of the same rules, but different border/
// radius/hover/pressed language per style. Persisted via
// SettingsManager::uiStyle().
enum class StyleId {
    // "Modern Aurora" - clean flat surfaces, restrained 6/8px radius,
    // subtle 1px borders, a soft lighten-on-hover/darken-on-press
    // language, matches stock modern Windows apps (Settings, Photos).
    ModernAurora = 0,
    // "Motiva Onyx" - flatter/sharper (2/3px radius, mostly borderless),
    // bolder full-accent-fill hover/pressed blocks instead of a subtle
    // lighten, and heavier-weight text - a distinct, more graphic Motiva
    // identity rather than a Windows-stock look.
    MotivaOnyx = 1,
};

// Appearance is an independent axis from StyleId above: StyleId controls
// button/border/radius/hover *chrome* language; AppearanceId controls
// which light/dark QPalette the whole app renders with. Persisted via
// SettingsManager::appearance().
enum class AppearanceId {
    // Follow whatever the OS's own window palette currently is - the
    // app's original, only-ever behavior before this setting existed.
    System = 0,
    Light = 1,
    Dark = 2,
};

// Whether an icon variant folder ("light" or "dark") should be used for
// the CURRENT effective palette - i.e. is the app rendering on a light
// or dark theme right now (System-followed OR explicitly chosen via
// AppearanceId - both end up expressed through qApp->palette(), so this
// one check covers both). Based on QApplication::palette().window()
// lightness - centralized here so every icon lookup site agrees.
bool isDarkPalette();

// Snapshots the real OS-driven palette. Must be called exactly once, at
// startup, before any call to applyAppearance() ever changes
// QApplication's palette - otherwise "System" could no longer be
// recovered later. See main.cpp.
void captureSystemPalette();

// Switches QApplication's actual QPalette to match the requested
// appearance: System restores the snapshot captured by
// captureSystemPalette(), Light/Dark install a fixed, deliberately
// distinct palette of Motiva's own. All of this app's QSS uses
// palette(...) tokens rather than fixed colors specifically so this one
// call is enough to re-theme everything already on screen - no
// stylesheet re-apply needed alongside it.
void applyAppearance(AppearanceId appearance);

// Human-readable name for an appearance, used by SettingsDialog's picker.
QString appearanceName(AppearanceId appearance);

// --- Accent (Motiva brand color) ---
// The same green already used for the "Wallpaper Active" status text and
// the drop zone's drag-over highlight (see DropZoneWidget.cpp) - adopted
// here as the one intentional accent color used for every primary
// interactive element, rather than introducing a second, competing hue.
constexpr const char* kAccent = "#3fae5c";
constexpr const char* kAccentHover = "#4fc26c";
constexpr const char* kAccentPressed = "#358f4c";
constexpr const char* kAccentSoft = "rgba(63, 174, 92, 0.16)";

// --- Status colors (state-only, not the general accent) ---
constexpr const char* kStatusNeutral = "#808080";
constexpr const char* kStatusWarning = "#c98a1a";
constexpr const char* kStatusSuccess = "#2e9e4f";
constexpr const char* kStatusError = "#d64545";

// --- Fixed-dark preview surface (theme-independent by design - see
// MainWindow.cpp's original comment on kPreviewSurfaceStyle) ---
constexpr const char* kPreviewSurfaceBg = "#161616";
constexpr const char* kPreviewSurfaceBorder = "#2c2c2c";
constexpr const char* kPreviewText = "#8a8a8a";
constexpr const char* kPreviewTextStrong = "#c9c9c9";
constexpr const char* kPreviewTextFaint = "#5a5a5a";

// Corner radii used consistently across surfaces/buttons/inputs - one
// scale, not a random assortment of rounded rectangles (see the task's
// "Avoid excessive rounded rectangles everywhere").
constexpr int kRadiusSmall = 8;   // buttons, inputs, small controls (Modern Aurora)
constexpr int kRadiusMedium = 10; // panels/cards, the preview container (Modern Aurora)
constexpr int kRadiusOnyx = 0;    // Motiva Onyx is square-cornered, full stop - not just
                                  // "slightly sharper" than Aurora's soft radius, so the
                                  // two are unmistakable even in a button at rest.

// The one application-wide stylesheet, applied once via
// QApplication::setStyleSheet in main.cpp. Organized by widget class/
// object name rather than scattered per-widget setStyleSheet() calls
// (see the task's "QSS / styling architecture" section) - the few
// setStyleSheet() calls remaining in MainWindow.cpp/DropZoneWidget.cpp
// are for the deliberately fixed-dark preview surface only, which is
// intentionally exempt from theme-following (see above).
QString appStyleSheet(StyleId style = StyleId::ModernAurora);

// Human-readable name for a style, used by SettingsDialog's picker.
QString styleName(StyleId style);

} // namespace Theme
