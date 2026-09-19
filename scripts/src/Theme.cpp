#include "Theme.h"

#include <QApplication>
#include <QPalette>

namespace Theme {

namespace {
AppTheme g_currentTheme = AppTheme::DarkAurora;

// --- Four independently-authored palettes ---------------------------
// Each case below is a self-contained literal color set - none of them
// is computed from another (no "light = dark inverted", no lightening/
// darkening a shared base at runtime). Radius/corner language lives in
// auroraStyleSheet()/onyxStyleSheet() instead, since it's a property of
// the Aurora/Onyx design language, not of light vs dark.

const ThemePalette& darkAuroraPalette() {
    static const ThemePalette p = [] {
        ThemePalette t;
        t.windowBg = QColor(0x1e, 0x1e, 0x1e);
        t.panelBg = QColor(0x24, 0x24, 0x26);
        t.baseBg = QColor(0x25, 0x25, 0x26);
        t.altBg = QColor(0x2a, 0x2a, 0x2b);
        t.textPrimary = QColor(0xf0, 0xf0, 0xf0);
        t.textSecondary = QColor(0x9a, 0x9a, 0x9a);
        t.textDisabled = QColor(0x6a, 0x6a, 0x6a);
        t.border = QColor(0x3a, 0x3a, 0x3a);
        t.borderStrong = QColor(0x50, 0x50, 0x50);
        t.buttonBg = QColor(0x2d, 0x2d, 0x30);
        t.buttonHoverBg = QColor(0x38, 0x38, 0x3a);
        t.buttonPressedBg = QColor(0x20, 0x20, 0x22);
        t.accent = QColor(0x3f, 0xae, 0x5c);
        t.accentHover = QColor(0x4f, 0xc2, 0x6c);
        t.accentPressed = QColor(0x35, 0x8f, 0x4c);
        t.accentSoft = QColor(0x3f, 0xae, 0x5c, 41);
        t.selectionText = QColor(0xff, 0xff, 0xff);
        t.isDark = true;
        return t;
    }();
    return p;
}

const ThemePalette& lightAuroraPalette() {
    static const ThemePalette p = [] {
        ThemePalette t;
        t.windowBg = QColor(0xf3, 0xf3, 0xf3);
        t.panelBg = QColor(0xff, 0xff, 0xff);
        t.baseBg = QColor(0xff, 0xff, 0xff);
        t.altBg = QColor(0xec, 0xec, 0xec);
        t.textPrimary = QColor(0x1b, 0x1b, 0x1b);
        t.textSecondary = QColor(0x6e, 0x6e, 0x6e);
        t.textDisabled = QColor(0xa0, 0xa0, 0xa0);
        t.border = QColor(0xdc, 0xdc, 0xdc);
        t.borderStrong = QColor(0xb7, 0xb7, 0xb7);
        t.buttonBg = QColor(0xe9, 0xe9, 0xe9);
        t.buttonHoverBg = QColor(0xf5, 0xf5, 0xf5);
        t.buttonPressedBg = QColor(0xd0, 0xd0, 0xd0);
        t.accent = QColor(0x2f, 0x97, 0x50);
        t.accentHover = QColor(0x3a, 0xab, 0x5e);
        t.accentPressed = QColor(0x25, 0x7a, 0x41);
        t.accentSoft = QColor(0x2f, 0x97, 0x50, 36);
        t.selectionText = QColor(0xff, 0xff, 0xff);
        t.isDark = false;
        return t;
    }();
    return p;
}

const ThemePalette& darkOnyxPalette() {
    static const ThemePalette p = [] {
        ThemePalette t;
        t.windowBg = QColor(0x14, 0x14, 0x14);
        t.panelBg = QColor(0x1c, 0x1c, 0x1c);
        t.baseBg = QColor(0x1a, 0x1a, 0x1a);
        t.altBg = QColor(0x20, 0x20, 0x20);
        t.textPrimary = QColor(0xf5, 0xf5, 0xf5);
        t.textSecondary = QColor(0x9d, 0x9d, 0x9d);
        t.textDisabled = QColor(0x5c, 0x5c, 0x5c);
        t.border = QColor(0x2a, 0x2a, 0x2a);
        t.borderStrong = QColor(0x3d, 0x3d, 0x3d);
        t.buttonBg = QColor(0x20, 0x20, 0x20);
        t.buttonHoverBg = QColor(0x2b, 0x2b, 0x2b);
        t.buttonPressedBg = QColor(0x16, 0x16, 0x16);
        t.accent = QColor(0x34, 0xc7, 0x6f);
        t.accentHover = QColor(0x45, 0xdb, 0x81);
        t.accentPressed = QColor(0x26, 0x93, 0x54);
        t.accentSoft = QColor(0x34, 0xc7, 0x6f, 46);
        t.selectionText = QColor(0xff, 0xff, 0xff);
        t.isDark = true;
        return t;
    }();
    return p;
}

const ThemePalette& lightOnyxPalette() {
    static const ThemePalette p = [] {
        ThemePalette t;
        t.windowBg = QColor(0xf5, 0xf5, 0xf2);
        t.panelBg = QColor(0xff, 0xff, 0xff);
        t.baseBg = QColor(0xff, 0xff, 0xff);
        t.altBg = QColor(0xeb, 0xeb, 0xe6);
        t.textPrimary = QColor(0x14, 0x17, 0x1a);
        t.textSecondary = QColor(0x5a, 0x5d, 0x60);
        t.textDisabled = QColor(0xa8, 0xa8, 0xa4);
        t.border = QColor(0xde, 0xda, 0xd2);
        t.borderStrong = QColor(0xc2, 0xbe, 0xb5);
        t.buttonBg = QColor(0xec, 0xea, 0xe4);
        t.buttonHoverBg = QColor(0xf5, 0xf4, 0xf0);
        t.buttonPressedBg = QColor(0xd9, 0xd6, 0xcd);
        t.accent = QColor(0x1f, 0x8a, 0x4c);
        t.accentHover = QColor(0x26, 0xa0, 0x58);
        t.accentPressed = QColor(0x18, 0x6b, 0x3a);
        t.accentSoft = QColor(0x1f, 0x8a, 0x4c, 40);
        t.selectionText = QColor(0xff, 0xff, 0xff);
        t.isDark = false;
        return t;
    }();
    return p;
}

QPalette buildQPalette(const ThemePalette& t) {
    QPalette p;
    p.setColor(QPalette::Window, t.windowBg);
    p.setColor(QPalette::WindowText, t.textPrimary);
    p.setColor(QPalette::Base, t.baseBg);
    p.setColor(QPalette::AlternateBase, t.altBg);
    p.setColor(QPalette::Text, t.textPrimary);
    p.setColor(QPalette::Button, t.buttonBg);
    p.setColor(QPalette::ButtonText, t.textPrimary);
    p.setColor(QPalette::Light, t.buttonHoverBg);
    p.setColor(QPalette::Midlight, t.border);
    p.setColor(QPalette::Mid, t.borderStrong);
    p.setColor(QPalette::Dark, t.buttonPressedBg);
    p.setColor(QPalette::Shadow, t.buttonPressedBg.darker(120));
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, t.selectionText);
    p.setColor(QPalette::ToolTipBase, t.panelBg);
    p.setColor(QPalette::ToolTipText, t.textPrimary);
    p.setColor(QPalette::Disabled, QPalette::Text, t.textDisabled);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.textDisabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.textDisabled);
    return p;
}

QString colorSheet(const QColor& c) { return c.name(QColor::HexArgb); }

// "Aurora": clean flat surfaces, restrained 6/8px radius, subtle 1px
// borders, a soft lighten-on-hover/darken-on-press language - matches
// stock modern Windows apps (Settings, Photos). Shared by Dark Aurora and
// Light Aurora; only the ThemePalette passed in differs between them.
QString auroraStyleSheet(const ThemePalette& p) {
    constexpr int kAuroraRadius = 8;
    return QStringLiteral(R"(
QMainWindow, QDialog {
    background: %1;
}
QWidget {
    color: %2;
}
QLabel {
    color: %2;
    background: transparent;
}
QLabel#appTitle {
    color: %2;
}
QLabel#sectionLabel {
    color: %2;
    padding-bottom: 4px;
    margin-top: 4px;
    border-bottom: 1px solid %3;
}
QLabel#secondaryText, QLabel#fileDetailsLabel {
    color: %4;
}
QFrame#headerRule {
    background: %3;
    max-height: 1px;
    border: none;
}

/* --- Buttons: one consistent system (normal/hover/pressed/disabled/focus) --- */
QPushButton, QToolButton {
    background: %5;
    color: %2;
    border: 1px solid %3;
    border-radius: %9px;
    padding: 7px 14px;
}
QPushButton:hover, QToolButton:hover {
    background: %6;
    border-color: %8;
}
QPushButton:pressed, QToolButton:pressed {
    background: %7;
}
QPushButton:disabled, QToolButton:disabled {
    color: %10;
    background: %1;
    border-color: %3;
}
QPushButton:focus, QToolButton:focus {
    outline: none;
    border: 1px solid %11;
}

QToolButton {
    padding: 6px 10px;
}
QToolButton#settingsButton {
    border: none;
    background: transparent;
}
QToolButton#settingsButton:hover {
    background: %6;
    border-radius: %9px;
}
QToolButton#settingsButton:pressed {
    background: %7;
}

/* --- Primary action: clear visual hierarchy over every secondary button --- */
QPushButton#primaryButton {
    background: %11;
    color: #ffffff;
    border: 1px solid %11;
    border-radius: %9px;
    font-weight: 600;
    padding: 9px 16px;
}
QPushButton#primaryButton:hover {
    background: %12;
    border-color: %12;
}
QPushButton#primaryButton:pressed {
    background: %13;
    border-color: %13;
}
QPushButton#primaryButton:disabled {
    background: %8;
    border-color: %8;
    color: %10;
}
QPushButton#primaryButton:focus {
    border: 1px solid #ffffff;
}

/* --- Inputs: native-feeling, not HTML-form-like --- */
QLineEdit, QComboBox, QAbstractSpinBox {
    background: %14;
    color: %2;
    border: 1px solid %3;
    border-radius: %9px;
    padding: 6px 8px;
    selection-background-color: %11;
}
QLineEdit:hover, QComboBox:hover, QAbstractSpinBox:hover {
    border-color: %8;
}
QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus {
    border: 1px solid %11;
}
QLineEdit:disabled, QComboBox:disabled {
    color: %10;
    background: %1;
}
QComboBox::drop-down {
    border: none;
    width: 22px;
}
QComboBox QAbstractItemView {
    background: %14;
    color: %2;
    border: 1px solid %3;
    selection-background-color: %15;
    selection-color: %2;
    outline: none;
}

QCheckBox {
    spacing: 8px;
    color: %2;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 4px;
    border: 1px solid %8;
    background: %14;
}
QCheckBox::indicator:hover {
    border-color: %11;
}
QCheckBox::indicator:checked {
    background: %11;
    border-color: %11;
    image: url(:/checkbox/check.svg);
}
QCheckBox::indicator:disabled {
    border-color: %8;
    background: %1;
}

QSlider::groove:horizontal {
    height: 4px;
    background: %3;
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: %11;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    width: 14px;
    height: 14px;
    margin: -5px 0;
    border-radius: 7px;
    background: %11;
    border: 2px solid %14;
}
QSlider::handle:horizontal:hover {
    background: %12;
}
QSlider::handle:horizontal:disabled {
    background: %8;
}

QDialogButtonBox QPushButton {
    min-width: 76px;
}
)")
        .arg(colorSheet(p.windowBg))     // %1
        .arg(colorSheet(p.textPrimary))  // %2
        .arg(colorSheet(p.border))       // %3
        .arg(colorSheet(p.textSecondary)) // %4
        .arg(colorSheet(p.buttonBg))     // %5
        .arg(colorSheet(p.buttonHoverBg)) // %6
        .arg(colorSheet(p.buttonPressedBg)) // %7
        .arg(colorSheet(p.borderStrong)) // %8
        .arg(kAuroraRadius)              // %9
        .arg(colorSheet(p.textDisabled)) // %10
        .arg(colorSheet(p.accent))       // %11
        .arg(colorSheet(p.accentHover))  // %12
        .arg(colorSheet(p.accentPressed)) // %13
        .arg(colorSheet(p.baseBg))       // %14
        .arg(colorSheet(p.accentSoft));  // %15
}

// "Onyx": flatter/sharper (near-borderless, square corners), bolder
// full-accent-fill hover/pressed blocks instead of a subtle lighten, and
// heavier-weight text - a distinct, more graphic Motiva identity. Shared
// by Dark Onyx and Light Onyx; only the ThemePalette passed in differs.
QString onyxStyleSheet(const ThemePalette& p) {
    constexpr int kOnyxRadius = 2;
    return QStringLiteral(R"(
QMainWindow, QDialog {
    background: %1;
}
QWidget {
    color: %2;
}
QLabel {
    color: %2;
    background: transparent;
    font-weight: 500;
}
QLabel#appTitle {
    color: %2;
    font-weight: 700;
    letter-spacing: 0.5px;
}
QLabel#sectionLabel {
    color: %2;
    font-weight: 700;
    padding: 2px 0 4px 8px;
    margin-top: 6px;
    border-left: 3px solid %11;
    border-bottom: none;
}
QLabel#secondaryText, QLabel#fileDetailsLabel {
    color: %4;
}
QFrame#headerRule {
    background: %11;
    max-height: 2px;
    border: none;
}

/* --- Buttons: borderless flat blocks, full-accent hover/pressed --- */
QPushButton, QToolButton {
    background: %5;
    color: %2;
    border: none;
    border-radius: %9px;
    padding: 8px 15px;
    font-weight: 600;
}
QPushButton:hover, QToolButton:hover {
    background: %11;
    color: #ffffff;
}
QPushButton:pressed, QToolButton:pressed {
    background: %13;
    color: #ffffff;
}
QPushButton:disabled, QToolButton:disabled {
    color: %8;
    background: %1;
}
QPushButton:focus, QToolButton:focus {
    outline: none;
    border: 2px solid %11;
}

QToolButton {
    padding: 6px 10px;
}
QToolButton#settingsButton {
    border: none;
    background: transparent;
}
QToolButton#settingsButton:hover {
    background: %11;
    border-radius: %9px;
}
QToolButton#settingsButton:pressed {
    background: %13;
}

/* --- Primary action: solid accent block, square corners, a bold left
   flag stripe --- */
QPushButton#primaryButton {
    background: %11;
    color: #ffffff;
    border: none;
    border-left: 5px solid #ffffff;
    border-radius: %9px;
    font-weight: 800;
    letter-spacing: 0.6px;
    padding: 10px 18px 10px 14px;
}
QPushButton#primaryButton:hover {
    background: %12;
}
QPushButton#primaryButton:pressed {
    background: %13;
}
QPushButton#primaryButton:disabled {
    background: %8;
    color: %1;
}
QPushButton#primaryButton:focus {
    border: 2px solid #ffffff;
}

/* --- Inputs: flat, underline-style focus --- */
QLineEdit, QComboBox, QAbstractSpinBox {
    background: %14;
    color: %2;
    border: none;
    border-bottom: 2px solid %3;
    border-radius: 2px;
    padding: 6px 8px;
    selection-background-color: %11;
}
QLineEdit:hover, QComboBox:hover, QAbstractSpinBox:hover {
    border-bottom-color: %8;
}
QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus {
    border-bottom: 2px solid %11;
}
QLineEdit:disabled, QComboBox:disabled {
    color: %8;
    background: %1;
}
QComboBox::drop-down {
    border: none;
    width: 22px;
}
QComboBox QAbstractItemView {
    background: %14;
    color: %2;
    border: 1px solid %11;
    selection-background-color: %15;
    selection-color: %2;
    outline: none;
}

QCheckBox {
    spacing: 8px;
    color: %2;
    font-weight: 500;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 2px;
    border: 2px solid %8;
    background: %14;
}
QCheckBox::indicator:hover {
    border-color: %11;
}
QCheckBox::indicator:checked {
    background: %11;
    border-color: %11;
    image: url(:/checkbox/check.svg);
}
QCheckBox::indicator:disabled {
    border-color: %8;
    background: %1;
}

QSlider::groove:horizontal {
    height: 3px;
    background: %3;
    border-radius: 0px;
}
QSlider::sub-page:horizontal {
    background: %11;
    border-radius: 0px;
}
QSlider::handle:horizontal {
    width: 12px;
    height: 12px;
    margin: -5px 0;
    border-radius: 2px;
    background: %11;
    border: none;
}
QSlider::handle:horizontal:hover {
    background: %12;
}
QSlider::handle:horizontal:disabled {
    background: %8;
}

QDialogButtonBox QPushButton {
    min-width: 76px;
}
)")
        // NOTE: QString::arg() fills the lowest-numbered %N actually
        // PRESENT in the string, not the Nth call - this template
        // deliberately skips %6/%7/%10 (unused, unlike Aurora's template),
        // so the calls below only cover the placeholders that exist,
        // strictly in ascending numeric order: 1,2,3,4,5,8,9,11,12,13,14,15.
        .arg(colorSheet(p.windowBg))       // %1
        .arg(colorSheet(p.textPrimary))    // %2
        .arg(colorSheet(p.border))         // %3
        .arg(colorSheet(p.textSecondary))  // %4
        .arg(colorSheet(p.buttonBg))       // %5
        .arg(colorSheet(p.borderStrong))   // %8
        .arg(kOnyxRadius)                  // %9
        .arg(colorSheet(p.accent))         // %11
        .arg(colorSheet(p.accentHover))    // %12
        .arg(colorSheet(p.accentPressed))  // %13
        .arg(colorSheet(p.baseBg))         // %14
        .arg(colorSheet(p.accentSoft));    // %15
}

} // namespace

AppTheme currentTheme() {
    return g_currentTheme;
}

void applyTheme(AppTheme theme) {
    if (!qApp) {
        return;
    }
    g_currentTheme = theme;
    qApp->setPalette(buildQPalette(themePalette(theme)));
    qApp->setStyleSheet(appStyleSheet(theme));
}

const ThemePalette& themePalette(AppTheme theme) {
    switch (theme) {
    case AppTheme::DarkAurora:
        return darkAuroraPalette();
    case AppTheme::LightAurora:
        return lightAuroraPalette();
    case AppTheme::DarkOnyx:
        return darkOnyxPalette();
    case AppTheme::LightOnyx:
        return lightOnyxPalette();
    }
    return darkAuroraPalette();
}

QString appStyleSheet(AppTheme theme) {
    const ThemePalette& p = themePalette(theme);
    switch (theme) {
    case AppTheme::DarkAurora:
    case AppTheme::LightAurora:
        return auroraStyleSheet(p);
    case AppTheme::DarkOnyx:
    case AppTheme::LightOnyx:
        return onyxStyleSheet(p);
    }
    return auroraStyleSheet(p);
}

QString themeName(AppTheme theme) {
    switch (theme) {
    case AppTheme::DarkAurora:
        return QStringLiteral("Dark Aurora");
    case AppTheme::LightAurora:
        return QStringLiteral("Light Aurora");
    case AppTheme::DarkOnyx:
        return QStringLiteral("Dark Onyx");
    case AppTheme::LightOnyx:
        return QStringLiteral("Light Onyx");
    }
    return QStringLiteral("Dark Aurora");
}

QString themeSettingsKey(AppTheme theme) {
    switch (theme) {
    case AppTheme::DarkAurora:
        return QStringLiteral("DarkAurora");
    case AppTheme::LightAurora:
        return QStringLiteral("LightAurora");
    case AppTheme::DarkOnyx:
        return QStringLiteral("DarkOnyx");
    case AppTheme::LightOnyx:
        return QStringLiteral("LightOnyx");
    }
    return QStringLiteral("DarkAurora");
}

AppTheme themeFromSettingsKey(const QString& key, AppTheme fallback) {
    if (key == QStringLiteral("DarkAurora")) return AppTheme::DarkAurora;
    if (key == QStringLiteral("LightAurora")) return AppTheme::LightAurora;
    if (key == QStringLiteral("DarkOnyx")) return AppTheme::DarkOnyx;
    if (key == QStringLiteral("LightOnyx")) return AppTheme::LightOnyx;
    return fallback;
}

bool isDarkTheme(AppTheme theme) {
    return themePalette(theme).isDark;
}

QString iconVariant(AppTheme theme) {
    return isDarkTheme(theme) ? QStringLiteral("dark") : QStringLiteral("light");
}

AppTheme migrateLegacySettings(int legacyAppearance, int legacyUiStyle) {
    // Legacy Appearance: 0=System, 1=Light, 2=Dark. System maps to Dark -
    // Dark was already reported as working correctly under the old system,
    // unlike Light, so this is the safer one-time default.
    const bool wasLight = (legacyAppearance == 1);
    const bool wasOnyx = (legacyUiStyle == 1);
    if (wasLight && wasOnyx) return AppTheme::LightOnyx;
    if (wasLight) return AppTheme::LightAurora;
    if (wasOnyx) return AppTheme::DarkOnyx;
    return AppTheme::DarkAurora;
}

} // namespace Theme
