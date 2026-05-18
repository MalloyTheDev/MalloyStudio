#include "VuMeter.h"

#include <QLinearGradient>
#include <QPainter>
#include <QTimer>

#include <algorithm>

namespace {
constexpr float DecayPerTick   = 0.04f;  // 60 Hz × 0.04 ≈ 2.4 units/sec
constexpr float HoldSeconds    = 0.9f;
constexpr float TickIntervalMs = 16.0f;  // ~60 Hz
}

VuMeter::VuMeter(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(false);
    m_decay = new QTimer(this);
    m_decay->setInterval(static_cast<int>(TickIntervalMs));
    connect(m_decay, &QTimer::timeout, this, &VuMeter::onDecay);
    m_decay->start();
}

void VuMeter::setLevels(float peakL, float peakR) {
    m_targetL = std::clamp(peakL, 0.0f, 1.0f);
    m_targetR = std::clamp(peakR, 0.0f, 1.0f);
    if (m_targetL > m_curL) { m_curL = m_targetL; m_holdL = std::max(m_holdL, m_targetL); m_holdTL = HoldSeconds; }
    if (m_targetR > m_curR) { m_curR = m_targetR; m_holdR = std::max(m_holdR, m_targetR); m_holdTR = HoldSeconds; }
}

void VuMeter::setEnabled(bool enabled) {
    if (m_active == enabled) return;
    m_active = enabled;
    if (!enabled) {
        m_targetL = m_targetR = 0.0f;
        m_curL = m_curR = 0.0f;
        m_holdL = m_holdR = 0.0f;
    }
    update();
}

void VuMeter::onDecay() {
    const float dt = TickIntervalMs / 1000.0f;
    decayOne(m_curL, m_holdL, m_holdTL);
    decayOne(m_curR, m_holdR, m_holdTR);
    m_holdTL = std::max(0.0f, m_holdTL - dt);
    m_holdTR = std::max(0.0f, m_holdTR - dt);
    update();
}

void VuMeter::decayOne(float& current, float& hold, float& /*holdTimer*/) {
    current = std::max(0.0f, current - DecayPerTick);
    hold    = std::max(current, hold - DecayPerTick * 0.4f);
}

void VuMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const QRect r = rect();
    const int barH = std::max(4, (r.height() - 3) / 2);
    const QRect lRect(r.left(), r.top(),               r.width(), barH);
    const QRect rRect(r.left(), r.top() + barH + 3,    r.width(), barH);

    // Background
    p.fillRect(lRect, QColor(20, 20, 20));
    p.fillRect(rRect, QColor(20, 20, 20));

    if (!m_active) {
        p.setPen(QColor(80, 80, 80));
        p.drawText(r, Qt::AlignCenter, tr("disconnected"));
        return;
    }

    // Gradient: green → yellow → red across the full width
    QLinearGradient grad(0, 0, r.width(), 0);
    grad.setColorAt(0.00, QColor( 30, 200,  60));
    grad.setColorAt(0.70, QColor( 30, 200,  60));
    grad.setColorAt(0.85, QColor(230, 210,  40));
    grad.setColorAt(1.00, QColor(230,  60,  40));

    auto drawBar = [&](const QRect& bg, float level, float hold) {
        const int w = static_cast<int>(bg.width() * level);
        if (w > 0) p.fillRect(QRect(bg.left(), bg.top(), w, bg.height()), grad);
        // peak hold tick
        if (hold > 0.0f) {
            const int hx = bg.left() + static_cast<int>((bg.width() - 1) * hold);
            p.setPen(QColor(255, 255, 255, 200));
            p.drawLine(hx, bg.top(), hx, bg.bottom());
        }
    };

    drawBar(lRect, m_curL, m_holdL);
    drawBar(rRect, m_curR, m_holdR);

    // Subtle frame
    p.setPen(QColor(60, 60, 60));
    p.setBrush(Qt::NoBrush);
    p.drawRect(lRect.adjusted(0, 0, -1, -1));
    p.drawRect(rRect.adjusted(0, 0, -1, -1));
}
