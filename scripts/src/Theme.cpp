#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QColor>

namespace Theme {

bool isDarkPalette() {
    // Same heuristic Qt itself effectively uses for "is this a dark
    // theme" - the window surface's own lightness, not any hardcoded
    // color list, so this keeps tracking whatever the OS palette is.
    return qApp && qApp->palette().color(QPalette::Window).lightness() < 128;
}

namespace {
// Populated once by captureSystemPalette(), before applyAppearance() ever
// runs - the only way "System" can be restored later after Light/Dark has
// overwritten QApplication's live palette.
QPalette g_systemPalette;
bool g_systemPaletteCaptured = false;

QPalette buildLightPalette() {
    QPalette p;
    p.setColor(QPalette::Window, QColor(0xf3, 0xf3, 0xf3));
    p.setColor(QPalette::WindowText, QColor(0x1b, 0x1b, 0x1b));
    p.setColor(QPalette::Base, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::AlternateBase, QColor(0xf3, 0xf3, 0xf3));
    p.setColor(QPalette::Text, QColor(0x1b, 0x1b, 0x1b));
    p.setColor(QPalette::Button, QColor(0xe9, 0xe9, 0xe9));
    p.setColor(QPalette::ButtonText, QColor(0x1b, 0x1b, 0x1b));
    p.setColor(QPalette::Light, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::Midlight, QColor(0xde, 0xde, 0xde));
    p.setColor(QPalette::Mid, QColor(0xb7, 0xb7, 0xb7));
    p.setColor(QPalette::Dark, QColor(0x8a, 0x8a, 0x8a));
    p.setColor(QPalette::Shadow, QColor(0x6e, 0x6e, 0x6e));
    p.setColor(QPalette::Highlight, QColor(kAccent));
    p.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::ToolTipText, QColor(0x1b, 0x1b, 0x1b));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0xa0, 0xa0, 0xa0));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0xa0, 0xa0, 0xa0));
    return p;
}

QPalette buildDarkPalette() {
    QPalette p;
    p.setColor(QPalette::Window, QColor(0x1e, 0x1e, 0x1e));
    p.setColor(QPalette::WindowText, QColor(0xf0, 0xf0, 0xf0));
    p.setColor(QPalette::Base, QColor(0x25, 0x25, 0x26));
    p.setColor(QPalette::AlternateBase, QColor(0x2a, 0x2a, 0x2b));
    p.setColor(QPalette::Text, QColor(0xf0, 0xf0, 0xf0));
    p.setColor(QPalette::Button, QColor(0x2d, 0x2d, 0x30));
    p.setColor(QPalette::ButtonText, QColor(0xf0, 0xf0, 0xf0));
    p.setColor(QPalette::Light, QColor(0x45, 0x45, 0x45));
    p.setColor(QPalette::Midlight, QColor(0x3a, 0x3a, 0x3a));
    p.setColor(QPalette::Mid, QColor(0x50, 0x50, 0x50));
    p.setColor(QPalette::Dark, QColor(0x14, 0x14, 0x14));
    p.setColor(QPalette::Shadow, QColor(0x0a, 0x0a, 0x0a));
    p.setColor(QPalette::Highlight, QColor(kAccent));
    p.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::ToolTipBase, QColor(0x2d, 0x2d, 0x30));
    p.setColor(QPalette::ToolTipText, QColor(0xf0, 0xf0, 0xf0));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x6a, 0x6a, 0x6a));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x6a, 0x6a, 0x6a));
    return p;
}
} // namespace

void captureSystemPalette() {
    if (qApp && !g_systemPaletteCaptured) {
        g_systemPalette = qApp->palette();
        g_systemPaletteCaptured = true;
    }
}

void applyAppearance(AppearanceId appearance) {
    if (!qApp) {
        return;
    }
    switch (appearance) {
    case AppearanceId::Light:
        qApp->setPalette(buildLightPalette());
        break;
    case AppearanceId::Dark:
        qApp->setPalette(buildDarkPalette());
        break;
    case AppearanceId::System:
    default:
        if (g_systemPaletteCaptured) {
            qApp->setPalette(g_systemPalette);
        }
        break;
    }
}

QString appearanceName(AppearanceId appearance) {
    switch (appearance) {
    case AppearanceId::Light:
        return QStringLiteral("Light");
    case AppearanceId::Dark:
        return QStringLiteral("Dark");
    case AppearanceId::System:
        return QStringLiteral("System");
    }
    return QStringLiteral("System");
}

QString styleName(StyleId style) {
    switch (style) {
    case StyleId::ModernAurora:
        return QStringLiteral("Modern Aurora");
    case StyleId::MotivaOnyx:
        return QStringLiteral("Motiva Onyx");
    }
    return QStringLiteral("Modern Aurora");
}

namespace {

// "Modern Aurora": the original styling pass - flat palette-based
// surfaces, 6/8px radius, a subtle 1px border, lighten-on-hover /
// darken-on-press. Restrained and Windows-stock in feel.
QString modernAuroraStyleSheet() {
    return QStringLiteral(R"(
QMainWindow, QDialog {
    background: palette(window);
}

QLabel {
    color: palette(windowtext);
}

QLabel#appTitle {
    color: palette(windowtext);
}

QLabel#sectionLabel {
    color: palette(windowtext);
    padding-bottom: 4px;
    margin-top: 4px;
    border-bottom: 1px solid palette(midlight);
}

QLabel#secondaryText, QLabel#fileDetailsLabel {
    color: %1;
}

QFrame#headerRule {
    background: palette(midlight);
    max-height: 1px;
    border: none;
}

/* --- Buttons: one consistent system (normal/hover/pressed/disabled/focus) --- */
QPushButton, QToolButton {
    background: palette(button);
    color: palette(buttontext);
    border: 1px solid palette(midlight);
    border-radius: %2px;
    padding: 7px 14px;
}
QPushButton:hover, QToolButton:hover {
    background: palette(light);
    border-color: palette(mid);
}
QPushButton:pressed, QToolButton:pressed {
    background: palette(mid);
}
QPushButton:disabled, QToolButton:disabled {
    color: palette(buttontext);
    background: palette(window);
    border-color: palette(mid);
}
QPushButton:focus, QToolButton:focus {
    outline: none;
    border: 1px solid %3;
}

QToolButton {
    padding: 6px 10px;
}
QToolButton#settingsButton {
    border: none;
    background: transparent;
}
QToolButton#settingsButton:hover {
    background: palette(light);
    border-radius: %2px;
}
QToolButton#settingsButton:pressed {
    background: palette(mid);
}

/* --- Primary action: clear visual hierarchy over every secondary button --- */
QPushButton#primaryButton {
    background: %3;
    color: #ffffff;
    border: 1px solid %3;
    border-radius: %2px;
    font-weight: 600;
    padding: 9px 16px;
}
QPushButton#primaryButton:hover {
    background: %4;
    border-color: %4;
}
QPushButton#primaryButton:pressed {
    background: %5;
    border-color: %5;
}
QPushButton#primaryButton:disabled {
    background: palette(mid);
    border-color: palette(mid);
    color: palette(buttontext);
}
QPushButton#primaryButton:focus {
    border: 1px solid #ffffff;
}

/* --- Inputs: native-feeling, not HTML-form-like --- */
QLineEdit, QComboBox, QAbstractSpinBox {
    background: palette(base);
    color: palette(text);
    border: 1px solid palette(midlight);
    border-radius: %2px;
    padding: 6px 8px;
    selection-background-color: %3;
}
QLineEdit:hover, QComboBox:hover, QAbstractSpinBox:hover {
    border-color: palette(mid);
}
QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus {
    border: 1px solid %3;
}
QLineEdit:disabled, QComboBox:disabled {
    color: palette(text);
    background: palette(window);
}
QComboBox::drop-down {
    border: none;
    width: 22px;
}
QComboBox QAbstractItemView {
    background: palette(base);
    border: 1px solid palette(midlight);
    selection-background-color: %6;
    outline: none;
}

QCheckBox {
    spacing: 8px;
    color: palette(windowtext);
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 4px;
    border: 1px solid palette(mid);
    background: palette(base);
}
QCheckBox::indicator:hover {
    border-color: %3;
}
QCheckBox::indicator:checked {
    background: %3;
    border-color: %3;
    image: url(:/checkbox/check.svg);
}
QCheckBox::indicator:disabled {
    border-color: palette(mid);
    background: palette(window);
}

QSlider::groove:horizontal {
    height: 4px;
    background: palette(midlight);
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: %3;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    width: 14px;
    height: 14px;
    margin: -5px 0;
    border-radius: 7px;
    background: %3;
    border: 2px solid palette(base);
}
QSlider::handle:horizontal:hover {
    background: %4;
}
QSlider::handle:horizontal:disabled {
    background: palette(mid);
}

QDialogButtonBox QPushButton {
    min-width: 76px;
}
)")
        .arg(kStatusNeutral)
        .arg(kRadiusSmall)
        .arg(kAccent)
        .arg(kAccentHover)
        .arg(kAccentPressed)
        .arg(kAccentSoft);
}

// "Motiva Onyx": a distinct, more graphic identity - almost borderless,
// sharper 2/3px corners, and instead of Aurora's subtle
// lighten-on-hover/darken-on-press, hover/pressed swap in full accent-
// colored blocks (a different HOVER LANGUAGE, not just a recolor).
// Section labels get a left accent bar instead of Aurora's bottom rule,
// and body text is a step heavier, giving the whole app a bolder,
// poster-like read rather than Aurora's quiet native-Windows one.
QString motivaOnyxStyleSheet() {
    return QStringLiteral(R"(
QMainWindow, QDialog {
    background: palette(window);
}

QLabel {
    color: palette(windowtext);
    font-weight: 500;
}

QLabel#appTitle {
    color: palette(windowtext);
    font-weight: 700;
    letter-spacing: 0.5px;
}

QLabel#sectionLabel {
    color: palette(windowtext);
    font-weight: 700;
    padding: 2px 0 4px 8px;
    margin-top: 6px;
    border-left: 3px solid %3;
    border-bottom: none;
}

QLabel#secondaryText, QLabel#fileDetailsLabel {
    color: %1;
}

QFrame#headerRule {
    background: %3;
    max-height: 2px;
    border: none;
}

/* --- Buttons: borderless flat blocks, full-accent hover/pressed --- */
QPushButton, QToolButton {
    background: palette(button);
    color: palette(buttontext);
    border: none;
    border-radius: %2px;
    padding: 8px 15px;
    font-weight: 600;
}
QPushButton:hover, QToolButton:hover {
    background: %6;
    color: palette(windowtext);
}
QPushButton:pressed, QToolButton:pressed {
    background: %3;
    color: #ffffff;
}
QPushButton:disabled, QToolButton:disabled {
    color: palette(mid);
    background: palette(window);
}
QPushButton:focus, QToolButton:focus {
    outline: none;
    border: 2px solid %3;
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
    border-radius: %2px;
}
QToolButton#settingsButton:pressed {
    background: %3;
}

/* --- Primary action: solid accent block, square corners, a bold left
   flag stripe - deliberately NOT just a recolor of Aurora's rounded
   primary button, so the single most prominent control in the app reads
   differently between styles even at rest, not only on hover/press. --- */
QPushButton#primaryButton {
    background: %3;
    color: #ffffff;
    border: none;
    border-left: 5px solid #ffffff;
    border-radius: %2px;
    font-weight: 800;
    letter-spacing: 0.6px;
    padding: 10px 18px 10px 14px;
}
QPushButton#primaryButton:hover {
    background: %4;
}
QPushButton#primaryButton:pressed {
    background: %5;
}
QPushButton#primaryButton:disabled {
    background: palette(mid);
    color: palette(window);
}
QPushButton#primaryButton:focus {
    border: 2px solid #ffffff;
}

/* --- Inputs: flat, underline-style focus instead of Aurora's boxed border --- */
QLineEdit, QComboBox, QAbstractSpinBox {
    background: palette(base);
    color: palette(text);
    border: none;
    border-bottom: 2px solid palette(midlight);
    border-radius: 2px;
    padding: 6px 8px;
    selection-background-color: %3;
}
QLineEdit:hover, QComboBox:hover, QAbstractSpinBox:hover {
    border-bottom-color: palette(mid);
}
QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus {
    border-bottom: 2px solid %3;
}
QLineEdit:disabled, QComboBox:disabled {
    color: palette(mid);
    background: palette(window);
}
QComboBox::drop-down {
    border: none;
    width: 22px;
}
QComboBox QAbstractItemView {
    background: palette(base);
    border: 1px solid %3;
    selection-background-color: %6;
    outline: none;
}

QCheckBox {
    spacing: 8px;
    color: palette(windowtext);
    font-weight: 500;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 2px;
    border: 2px solid palette(mid);
    background: palette(base);
}
QCheckBox::indicator:hover {
    border-color: %3;
}
QCheckBox::indicator:checked {
    background: %3;
    border-color: %3;
    image: url(:/checkbox/check.svg);
}
QCheckBox::indicator:disabled {
    border-color: palette(mid);
    background: palette(window);
}

QSlider::groove:horizontal {
    height: 3px;
    background: palette(midlight);
    border-radius: 0px;
}
QSlider::sub-page:horizontal {
    background: %3;
    border-radius: 0px;
}
QSlider::handle:horizontal {
    width: 12px;
    height: 12px;
    margin: -5px 0;
    border-radius: 2px;
    background: %3;
    border: none;
}
QSlider::handle:horizontal:hover {
    background: %4;
}
QSlider::handle:horizontal:disabled {
    background: palette(mid);
}

QDialogButtonBox QPushButton {
    min-width: 76px;
}
)")
        .arg(kStatusNeutral)
        .arg(kRadiusOnyx)
        .arg(kAccent)
        .arg(kAccentHover)
        .arg(kAccentPressed)
        .arg(kAccentSoft);
}

} // namespace

QString appStyleSheet(StyleId style) {
    switch (style) {
    case StyleId::ModernAurora:
        return modernAuroraStyleSheet();
    case StyleId::MotivaOnyx:
        return motivaOnyxStyleSheet();
    }
    return modernAuroraStyleSheet();
}

} // namespace Theme
