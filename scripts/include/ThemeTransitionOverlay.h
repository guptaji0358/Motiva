#pragma once

#include <QWidget>

class QVariantAnimation;

// A short-lived overlay shown over one top-level Motiva window while a new
// Appearance/Visual Style is being applied, so the user sees one deliberate
// "Applying Theme" transition instead of the underlying widgets restyling
// visibly in stages (palette swap, then stylesheet repolish, then icon
// swaps, each painting separately). Purely decorative and input-blocking -
// it never performs, delays, or waits on the actual theme application
// itself; see SettingsDialog's theme-transition handling for that. Covers
// exactly its parent widget's rect, so MainWindow and SettingsDialog each
// own their own instance.
class ThemeTransitionOverlay : public QWidget {
    Q_OBJECT
public:
    explicit ThemeTransitionOverlay(QWidget* parent);

    // Resizes to fully cover the parent, raises above all siblings, and
    // starts the indeterminate sweep animation. Being on top and opaque to
    // mouse events is what blocks interaction with the underlying UI -
    // no separate setEnabled(false) bookkeeping needed.
    void beginTransition();

    // Fades the overlay out over a short fixed duration and hides it - a
    // real closing animation, not a delay inserted before/after real work.
    // Safe to call even if beginTransition() was never called or a fade is
    // already running.
    void finishTransition();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVariantAnimation* m_sweepAnim;
    QVariantAnimation* m_fadeAnim;
    qreal m_sweepPos = 0.0;
    qreal m_opacity = 1.0;
};
