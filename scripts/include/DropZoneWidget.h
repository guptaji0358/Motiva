#pragma once

#include <QWidget>
#include <QSvgRenderer>

class QPropertyAnimation;
class QSequentialAnimationGroup;

// A small, self-contained animated visual for the empty-state video
// preview - communicates "drag a video here" without relying only on
// static text (see the "Animated Drag & Drop Area" task). Purely a UI
// affordance: it knows nothing about the actual drag/drop MIME handling
// or video loading pipeline in MainWindow, which only calls setState()
// in response to its own existing dragEnter/dragLeave/drop logic.
class DropZoneWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal floatOffset READ floatOffset WRITE setFloatOffset)
    Q_PROPERTY(qreal activity READ activity WRITE setActivity)
    Q_PROPERTY(qreal invalidBlend READ invalidBlend WRITE setInvalidBlend)
    Q_PROPERTY(qreal dashOffset READ dashOffset WRITE setDashOffset)
    Q_PROPERTY(qreal successProgress READ successProgress WRITE setSuccessProgress)

public:
    enum class State {
        Idle,     // nothing being dragged - gentle idle float only
        DragOver, // a supported video is currently dragged over the window
        Invalid,  // something unsupported is currently dragged over the window
        Success   // a valid video was just dropped - brief one-shot burst
    };

    explicit DropZoneWidget(QWidget* parent = nullptr);

    void setState(State state);
    State state() const { return m_state; }

    qreal floatOffset() const { return m_floatOffset; }
    void setFloatOffset(qreal value) {
        m_floatOffset = value;
        update();
    }
    qreal activity() const { return m_activity; }
    void setActivity(qreal value) {
        m_activity = value;
        update();
    }
    qreal invalidBlend() const { return m_invalidBlend; }
    void setInvalidBlend(qreal value) {
        m_invalidBlend = value;
        update();
    }
    qreal dashOffset() const { return m_dashOffset; }
    void setDashOffset(qreal value) {
        m_dashOffset = value;
        update();
    }
    qreal successProgress() const { return m_successProgress; }
    void setSuccessProgress(qreal value) {
        m_successProgress = value;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    State m_state = State::Idle;
    qreal m_floatOffset = 0.0;
    qreal m_activity = 0.0;
    qreal m_invalidBlend = 0.0;
    qreal m_dashOffset = 0.0;
    qreal m_successProgress = 0.0;

    QSvgRenderer m_videoIconRenderer;
    QSvgRenderer m_arrowRenderer;

    // Loops forever while this widget is visible (paused in hideEvent, so
    // it costs nothing once the actual video preview takes over) - see
    // .cpp for why a two-leg sequential group is used instead of a single
    // looping QPropertyAnimation.
    QSequentialAnimationGroup* m_idleFloatAnim = nullptr;
    QPropertyAnimation* m_activityAnim = nullptr;
    QPropertyAnimation* m_invalidAnim = nullptr;
    QPropertyAnimation* m_dashAnim = nullptr;
    QSequentialAnimationGroup* m_successAnim = nullptr;
};
