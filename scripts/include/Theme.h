#pragma once

#include <QString>

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
constexpr int kRadiusSmall = 6;   // buttons, inputs, small controls
constexpr int kRadiusMedium = 8;  // panels/cards, the preview container

// The one application-wide stylesheet, applied once via
// QApplication::setStyleSheet in main.cpp. Organized by widget class/
// object name rather than scattered per-widget setStyleSheet() calls
// (see the task's "QSS / styling architecture" section) - the few
// setStyleSheet() calls remaining in MainWindow.cpp/DropZoneWidget.cpp
// are for the deliberately fixed-dark preview surface only, which is
// intentionally exempt from theme-following (see above).
QString appStyleSheet();

} // namespace Theme
