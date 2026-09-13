#include "Theme.h"

namespace Theme {

QString appStyleSheet() {
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

} // namespace Theme
