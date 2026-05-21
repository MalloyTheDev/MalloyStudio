#include "ui/components/MeterBar.h"
#include "ui/Theme.h"

#include <QLinearGradient>
#include <QPainter>

MeterBar::MeterBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(m_h);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void MeterBar::setValues(double level, double peak) {
    m_level = qBound(0.0, level, 1.0);
    m_peak = qBound(0.0, peak, 1.0);
    update();
}

void MeterBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect();
    p.setBrush(Theme::Surface2);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(r, 2, 2);

    // Gradient fill up to the current level.
    if (m_level > 0.0) {
        QLinearGradient g(r.left(), 0, r.right(), 0);
        g.setColorAt(0.00, Theme::MeterGreen);
        g.setColorAt(0.60, Theme::MeterGreen);
        g.setColorAt(0.80, Theme::MeterYellow);
        g.setColorAt(0.92, Theme::MeterRed);
        g.setColorAt(1.00, Theme::MeterRed);
        QRectF fill(r.left(), r.top(), r.width() * m_level, r.height());
        p.setBrush(g);
        p.drawRoundedRect(fill, 2, 2);
    }

    // Peak hold tick.
    if (m_peak > 0.0) {
        const double x = r.left() + r.width() * m_peak;
        p.setPen(QPen(QColor(0xfa, 0xfa, 0xfa), 1.5));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
}
