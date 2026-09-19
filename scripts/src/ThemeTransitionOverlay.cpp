#include "ThemeTransitionOverlay.h"
#include "Theme.h"

#include <QPainter>
#include <QVariantAnimation>
#include <QLinearGradient>

ThemeTransitionOverlay::ThemeTransitionOverlay(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    hide();

    // Loops indefinitely while visible - a genuinely indeterminate
    // progress indicator (the actual theme apply has no meaningful
    // percentage), not a fixed-duration fake-work delay.
    m_sweepAnim = new QVariantAnimation(this);
    m_sweepAnim->setStartValue(0.0);
    m_sweepAnim->setEndValue(1.0);
    m_sweepAnim->setDuration(900);
    m_sweepAnim->setLoopCount(-1);
    connect(m_sweepAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_sweepPos = v.toReal();
        update();
    });

    m_fadeAnim = new QVariantAnimation(this);
    m_fadeAnim->setDuration(150);
    connect(m_fadeAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_opacity = v.toReal();
        update();
    });
    connect(m_fadeAnim, &QVariantAnimation::finished, this, [this]() {
        if (qFuzzyIsNull(m_opacity)) {
            m_sweepAnim->stop();
            hide();
        }
    });
}

void ThemeTransitionOverlay::beginTransition() {
    if (!parentWidget()) {
        return;
    }
    m_fadeAnim->stop();
    m_opacity = 1.0;
    setGeometry(parentWidget()->rect());
    show();
    raise();
    m_sweepAnim->start();
}

void ThemeTransitionOverlay::finishTransition() {
    if (!isVisible()) {
        return;
    }
    m_fadeAnim->stop();
    m_fadeAnim->setStartValue(m_opacity);
    m_fadeAnim->setEndValue(0.0);
    m_fadeAnim->start();
}

void ThemeTransitionOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(m_opacity);

    // Backdrop - dims whatever's underneath rather than hiding it behind a
    // solid color, so the overlay reads as "on top of Motiva" not as a
    // separate blank window.
    p.fillRect(rect(), QColor(0, 0, 0, 120));

    const int cardW = 260;
    const int cardH = 108;
    QRect card((width() - cardW) / 2, (height() - cardH) / 2, cardW, cardH);

    const bool dark = Theme::isDarkTheme(Theme::currentTheme());
    const QColor cardBg = dark ? QColor(0x25, 0x25, 0x26) : QColor(0xff, 0xff, 0xff);
    const QColor titleColor = dark ? QColor(0xf0, 0xf0, 0xf0) : QColor(0x1b, 0x1b, 0x1b);
    const QColor subtitleColor = dark ? QColor(0x9a, 0x9a, 0x9a) : QColor(0x6e, 0x6e, 0x6e);

    p.setPen(Qt::NoPen);
    p.setBrush(cardBg);
    p.drawRoundedRect(card, 12, 12);

    p.setPen(titleColor);
    QFont titleFont = p.font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.0);
    titleFont.setWeight(QFont::DemiBold);
    p.setFont(titleFont);
    p.drawText(QRect(card.x(), card.y() + 18, card.width(), 22), Qt::AlignHCenter, tr("Applying Theme"));

    // Track + moving highlight sweep (indeterminate progress).
    QRect track(card.x() + 28, card.y() + 56, card.width() - 56, 4);
    p.setBrush(dark ? QColor(0x45, 0x45, 0x45) : QColor(0xe0, 0xe0, 0xe0));
    p.drawRoundedRect(track, 2, 2);

    const qreal sweepWidth = track.width() * 0.35;
    qreal center = track.x() + m_sweepPos * (track.width() + sweepWidth) - sweepWidth / 2.0;
    QRectF sweep(center, track.y(), sweepWidth, track.height());
    sweep = sweep.intersected(QRectF(track));
    if (sweep.width() > 0) {
        QLinearGradient grad(sweep.left(), 0, sweep.right(), 0);
        grad.setColorAt(0.0, QColor(Theme::kBrandAccent).lighter(160));
        grad.setColorAt(0.5, QColor(Theme::kBrandAccent));
        grad.setColorAt(1.0, QColor(Theme::kBrandAccent).lighter(160));
        p.setBrush(grad);
        p.drawRoundedRect(sweep, 2, 2);
    }

    p.setPen(subtitleColor);
    QFont subtitleFont = p.font();
    subtitleFont.setWeight(QFont::Normal);
    subtitleFont.setPointSizeF(subtitleFont.pointSizeF() - 0.5);
    p.setFont(subtitleFont);
    p.drawText(QRect(card.x(), card.y() + 72, card.width(), 20), Qt::AlignHCenter,
               tr("Updating Motiva appearance..."));
}
