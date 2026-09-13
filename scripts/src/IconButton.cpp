#include "IconButton.h"

#include <QEvent>

IconButton::IconButton(QWidget* parent) : QPushButton(parent) {
    connect(this, &QAbstractButton::pressed, this, &IconButton::refreshIcon);
    connect(this, &QAbstractButton::released, this, &IconButton::refreshIcon);
}

IconButton::IconButton(const QIcon& icon, const QString& text, QWidget* parent)
    : QPushButton(icon, text, parent) {
    connect(this, &QAbstractButton::pressed, this, &IconButton::refreshIcon);
    connect(this, &QAbstractButton::released, this, &IconButton::refreshIcon);
}

void IconButton::setStateIcon(const QIcon& normal, const QIcon& hover, const QIcon& pressed, const QIcon& disabled) {
    m_normalIcon = normal;
    m_hoverIcon = hover.isNull() ? m_normalIcon : hover;
    m_pressedIcon = pressed.isNull() ? m_hoverIcon : pressed;
    m_disabledIcon = disabled.isNull() ? m_normalIcon : disabled;
    refreshIcon();
}

void IconButton::enterEvent(QEnterEvent* event) {
    QPushButton::enterEvent(event);
    m_hovered = true;
    refreshIcon();
}

void IconButton::leaveEvent(QEvent* event) {
    QPushButton::leaveEvent(event);
    m_hovered = false;
    refreshIcon();
}

void IconButton::changeEvent(QEvent* event) {
    QPushButton::changeEvent(event);
    if (event->type() == QEvent::EnabledChange) {
        refreshIcon();
    }
}

void IconButton::refreshIcon() {
    if (m_normalIcon.isNull()) {
        // setStateIcon() was never called - behave like a plain
        // QPushButton (whatever setIcon() was called with directly).
        return;
    }
    if (!isEnabled()) {
        setIcon(m_disabledIcon);
    } else if (isDown()) {
        setIcon(m_pressedIcon);
    } else if (m_hovered) {
        setIcon(m_hoverIcon);
    } else {
        setIcon(m_normalIcon);
    }
}
