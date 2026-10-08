#pragma once

#include <QPointer>
#include <QString>
#include <QTextDocument>
#include <QTimer>
#include <QWidget>

class QApplication;

// The one Motiva tooltip. A single, lazily-shown frameless popup that
// replaces Qt's default QTipLabel for every widget / item-view row /
// graphics-view item that already has a toolTip - the tooltip TEXT stays
// whatever each call site set, only the presentation is centralized here.
//
// install() puts one application-level event filter in place: it takes over
// QEvent::ToolTip (the same trigger and hover delay Qt already uses), shows
// the popup next to the pointer, and hides it again on Leave, clicks, key
// presses, wheel, focus loss or when the anchor widget goes away. The popup
// never takes input (transparent for mouse events, never activated) and its
// colors are read from the active Theme palette every time it is painted, so
// a live theme change is picked up by the next tooltip without any signal
// wiring.
class MotivaToolTip : public QWidget {
public:
    static void install(QApplication* app);

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    MotivaToolTip();

    static QString textFor(QWidget* widget, const QPoint& localPos);
    void showTip(const QString& text, const QPoint& globalPos, QWidget* anchor);
    void hideTip();

    QTextDocument m_doc;
    QString m_text;
    QPointer<QWidget> m_anchor;
    QTimer m_hideTimer;
};
