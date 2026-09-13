#pragma once

#include <QPushButton>
#include <QIcon>

// A QPushButton that swaps its entire icon - not just its opacity/tint -
// per interaction state (see the "Motiva Icon States" task). Each state
// is backed by its own separately-authored SVG asset, set once via
// setStateIcon(); switching between them is purely event-driven
// (QAbstractButton's own pressed()/released() signals plus enterEvent/
// leaveEvent/changeEvent) - no timer, no polling, and no icon is
// recreated on the fly, only re-assigned from the four already-loaded
// QIcon objects, so hovering/pressing repeatedly costs nothing beyond a
// normal repaint.
class IconButton : public QPushButton {
    Q_OBJECT
public:
    explicit IconButton(QWidget* parent = nullptr);
    IconButton(const QIcon& icon, const QString& text, QWidget* parent = nullptr);

    // Any state left as a null QIcon falls back to the next-more-generic
    // one it was actually given (hover falls back to normal, pressed
    // falls back to hover, disabled falls back to normal) - a caller only
    // needs to provide the states that actually differ for its button.
    void setStateIcon(const QIcon& normal, const QIcon& hover = QIcon(), const QIcon& pressed = QIcon(),
        const QIcon& disabled = QIcon());

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void refreshIcon();

    QIcon m_normalIcon;
    QIcon m_hoverIcon;
    QIcon m_pressedIcon;
    QIcon m_disabledIcon;
    bool m_hovered = false;
};
