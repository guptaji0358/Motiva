#include "DropZoneWidget.h"
#include "Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QSequentialAnimationGroup>
#include <QEasingCurve>

#include <cmath>

namespace {
// Same accent/status color tokens used everywhere else in the app (see
// Theme.h) - reused here rather than inventing a separate palette, and
// deliberately fixed (not theme-derived): this widget only ever lives
// inside the preview area's own fixed-dark surface (kPreviewSurfaceStyle
// in MainWindow.cpp), which is intentionally theme-independent.
constexpr const char* kIdleIconColor = Theme::kPreviewText;
constexpr const char* kDragOverColor = Theme::kBrandAccent;
constexpr const char* kInvalidColor = Theme::kStatusError;
constexpr const char* kIdleBorderColor = "#3a3a3a";

QColor lerp(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
        a.greenF() + (b.greenF() - a.greenF()) * t, a.blueF() + (b.blueF() - a.blueF()) * t);
}
} // namespace

DropZoneWidget::DropZoneWidget(QWidget* parent)
    : QWidget(parent), m_videoIconRenderer(QStringLiteral(":/video/video-file.svg")),
      m_arrowRenderer(QStringLiteral(":/video/drop-arrow.svg")) {
    setMinimumHeight(120);

    // Idle float: a small continuous up/down drift on the icon - purely
    // decorative, communicates "this area is alive" without being
    // distracting. QPropertyAnimation's own setLoopCount(-1) only repeats
    // forward (snaps back to the start value each lap), so a genuine
    // back-and-forth drift needs two legs (up, then down) wrapped in a
    // looping QSequentialAnimationGroup instead.
    auto* up = new QPropertyAnimation(this, "floatOffset");
    up->setStartValue(-4.0);
    up->setEndValue(4.0);
    up->setDuration(1600);
    up->setEasingCurve(QEasingCurve::InOutSine);
    auto* down = new QPropertyAnimation(this, "floatOffset");
    down->setStartValue(4.0);
    down->setEndValue(-4.0);
    down->setDuration(1600);
    down->setEasingCurve(QEasingCurve::InOutSine);
    m_idleFloatAnim = new QSequentialAnimationGroup(this);
    m_idleFloatAnim->addAnimation(up);
    m_idleFloatAnim->addAnimation(down);
    m_idleFloatAnim->setLoopCount(-1);
    m_idleFloatAnim->start();

    // 0 (idle) <-> 1 (drag-over/invalid) - blends border/glow/arrow
    // strength smoothly instead of the UI snapping between states.
    m_activityAnim = new QPropertyAnimation(this, "activity", this);
    m_activityAnim->setDuration(280);
    m_activityAnim->setEasingCurve(QEasingCurve::OutCubic);

    m_invalidAnim = new QPropertyAnimation(this, "invalidBlend", this);
    m_invalidAnim->setDuration(220);

    // "Marching ants" dash offset - only runs while actually in DragOver
    // (started/stopped in setState), never during idle.
    m_dashAnim = new QPropertyAnimation(this, "dashOffset", this);
    m_dashAnim->setStartValue(0.0);
    m_dashAnim->setEndValue(24.0);
    m_dashAnim->setDuration(900);
    m_dashAnim->setLoopCount(-1);
}

void DropZoneWidget::setState(State state) {
    if (m_state == state && state != State::Success) {
        return;
    }
    m_state = state;

    const bool elevated = (state == State::DragOver || state == State::Invalid);
    m_activityAnim->stop();
    m_activityAnim->setStartValue(m_activity);
    m_activityAnim->setEndValue(elevated ? 1.0 : 0.0);
    m_activityAnim->start();

    m_invalidAnim->stop();
    m_invalidAnim->setStartValue(m_invalidBlend);
    m_invalidAnim->setEndValue(state == State::Invalid ? 1.0 : 0.0);
    m_invalidAnim->start();

    if (state == State::DragOver) {
        if (m_dashAnim->state() != QAbstractAnimation::Running) {
            m_dashAnim->start();
        }
    } else {
        m_dashAnim->stop();
    }

    if (state == State::Success) {
        if (!m_successAnim) {
            auto* burst = new QPropertyAnimation(this, "successProgress");
            burst->setStartValue(0.0);
            burst->setEndValue(1.0);
            burst->setDuration(650);
            burst->setEasingCurve(QEasingCurve::OutCubic);
            m_successAnim = new QSequentialAnimationGroup(this);
            m_successAnim->addAnimation(burst);
        }
        setSuccessProgress(0.0);
        m_successAnim->start();
    } else if (m_successAnim) {
        m_successAnim->stop();
        setSuccessProgress(0.0);
    }
}

void DropZoneWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_idleFloatAnim->state() != QAbstractAnimation::Running) {
        m_idleFloatAnim->resume();
    }
}

void DropZoneWidget::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    // Qt animations keep ticking even while their target widget is
    // hidden (e.g. the video preview page is showing instead), which
    // would mean paying for repaints nobody sees - pause everything
    // explicitly so this genuinely goes idle once a video is loaded, per
    // the task's performance requirement.
    m_idleFloatAnim->pause();
    m_activityAnim->pause();
    m_invalidAnim->pause();
    m_dashAnim->pause();
    if (m_successAnim) {
        m_successAnim->pause();
    }
}

void DropZoneWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF bounds = rect().adjusted(1, 1, -1, -1);
    const qreal radius = Theme::kRadiusMedium;

    const QColor idleColor(kIdleIconColor);
    const QColor dragColor(kDragOverColor);
    const QColor invalidColor(kInvalidColor);
    const QColor borderColor(kIdleBorderColor);

    // Blend idle -> drag-over -> invalid, all driven by animated 0..1
    // properties rather than switched instantly.
    QColor mixed = lerp(idleColor, dragColor, m_activity);
    mixed = lerp(mixed, invalidColor, m_invalidBlend);

    QPainterPath borderPath;
    borderPath.addRoundedRect(bounds, radius, radius);
    QPen borderPen(m_activity > 0.01 ? mixed : borderColor);
    borderPen.setWidthF(1.6);
    borderPen.setDashPattern({6, 5});
    borderPen.setDashOffset(m_dashOffset);
    painter.strokePath(borderPath, borderPen);

    const QPointF center(bounds.center().x(), bounds.center().y() - 6);

    // Soft glow behind the icon once a valid drag is active.
    if (m_activity > 0.01 && m_invalidBlend < 0.5) {
        QColor glow = dragColor;
        glow.setAlphaF(0.18 * m_activity);
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        const qreal glowRadius = 34 + 4 * m_activity;
        painter.drawEllipse(center, glowRadius, glowRadius);
    }

    // Downward arrow above the icon - always gently floating; moves
    // faster/more visibly once a drag is over the widget.
    const qreal arrowY = center.y() - 34 + m_floatOffset * (1.0 - 0.4 * m_activity);
    const QRectF arrowRect(center.x() - 8, arrowY, 16, 16);
    painter.save();
    painter.setOpacity(0.35 + 0.45 * m_activity);
    m_arrowRenderer.render(&painter, arrowRect);
    painter.restore();

    // The video icon - recolored by compositing (SourceIn) rather than
    // baked into the SVG file, so drag-over/invalid can retint it
    // smoothly instead of swapping between separate colored assets.
    const QRectF iconRect(center.x() - 22, center.y() - 22 + m_floatOffset * 0.5, 44, 44);
    QPixmap iconPixmap(iconRect.size().toSize() * devicePixelRatioF());
    iconPixmap.setDevicePixelRatio(devicePixelRatioF());
    iconPixmap.fill(Qt::transparent);
    {
        QPainter iconPainter(&iconPixmap);
        iconPainter.setRenderHint(QPainter::Antialiasing);
        m_videoIconRenderer.render(&iconPainter, QRectF(QPointF(0, 0), iconRect.size()));
        iconPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        iconPainter.fillRect(QRectF(QPointF(0, 0), iconRect.size()), mixed);
    }
    painter.drawPixmap(iconRect.topLeft(), iconPixmap);

    // Small "x" badge over the icon while an unsupported item is dragged
    // over the window - subtle, no separate error illustration.
    if (m_invalidBlend > 0.01) {
        painter.save();
        painter.setOpacity(m_invalidBlend);
        QPen xPen(invalidColor);
        xPen.setWidthF(2.4);
        xPen.setCapStyle(Qt::RoundCap);
        painter.setPen(xPen);
        const QPointF badge(iconRect.right() - 2, iconRect.top() + 2);
        constexpr qreal r = 6.0;
        painter.drawLine(badge + QPointF(-r, -r), badge + QPointF(r, r));
        painter.drawLine(badge + QPointF(-r, r), badge + QPointF(r, -r));
        painter.restore();
    }

    // One-shot success burst: a handful of dots drifting outward from
    // the icon while fading, then a checkmark scaling/fading in - purely
    // time-driven by m_successProgress (0..1), never blocks/delays the
    // actual video decode that's already running in parallel.
    if (m_successProgress > 0.0) {
        painter.save();
        const qreal fade = 1.0 - m_successProgress;
        QColor dot = dragColor;
        for (int i = 0; i < 6; ++i) {
            const qreal angle = (M_PI * 2.0 / 6.0) * i - M_PI / 2.0;
            const qreal dist = 30.0 * m_successProgress;
            const QPointF p = center + QPointF(std::cos(angle) * dist, std::sin(angle) * dist);
            dot.setAlphaF(0.8 * fade);
            painter.setPen(Qt::NoPen);
            painter.setBrush(dot);
            painter.drawEllipse(p, 2.5, 2.5);
        }
        if (m_successProgress > 0.5) {
            const qreal checkT = (m_successProgress - 0.5) / 0.5;
            painter.setOpacity(checkT);
            QPen checkPen(dragColor);
            checkPen.setWidthF(3.0);
            checkPen.setCapStyle(Qt::RoundCap);
            checkPen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(checkPen);
            QPainterPath check;
            check.moveTo(center + QPointF(-10, 0));
            check.lineTo(center + QPointF(-3, 8));
            check.lineTo(center + QPointF(12, -10));
            painter.drawPath(check);
        }
        painter.restore();
    }
}
