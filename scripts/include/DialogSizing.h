#pragma once

#include <QDialog>
#include <QGuiApplication>
#include <QLayout>
#include <QScreen>
#include <QSize>

// Sizing for Motiva's dialogs: the floor is whatever the layout itself needs
// (minimumSizeHint), so nothing is hidden or clipped, and the dialog is still
// freely resizable. The opening size is the larger of the layout's sizeHint and
// a comfortable `preferred` size, but never more than `maxScreenFraction` of
// the screen it opens on. Not a fixed size, never maximized.
namespace DialogSizing {

inline void applyComfortableSize(QDialog* dialog, QSize preferred, qreal maxScreenFraction = 0.9) {
    if (QLayout* layout = dialog->layout()) {
        layout->activate();
    }
    QScreen* screen = dialog->screen() ? dialog->screen() : QGuiApplication::primaryScreen();
    const QSize avail = screen ? screen->availableGeometry().size() : QSize(1280, 720);
    const QSize cap(int(avail.width() * maxScreenFraction), int(avail.height() * maxScreenFraction));
    const QSize minimum = dialog->minimumSizeHint().boundedTo(cap);
    dialog->setMinimumSize(minimum);
    dialog->resize(dialog->sizeHint().expandedTo(preferred).expandedTo(minimum).boundedTo(cap));
}

} // namespace DialogSizing
