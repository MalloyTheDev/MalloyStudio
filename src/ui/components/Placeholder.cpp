#include "ui/components/Placeholder.h"
#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>

Placeholder::Placeholder(const QString& label, int ratioW, int ratioH, QWidget* parent)
    : QWidget(parent), m_label(label), m_rw(ratioW), m_rh(ratioH) {
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
}

void Placeholder::setLabel(const QString& label) {
    m_label = label;
    update();
}

int Placeholder::heightForWidth(int w) const {
    return m_rw > 0 ? w * m_rh / m_rw : w;
}

QSize Placeholder::sizeHint() const {
    return QSize(240, heightForWidth(240));
}

void Placeholder::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath clip;
    clip.addRoundedRect(r, 6, 6);
    p.setClipPath(clip);

    // Base + diagonal hatch (≈135°), matching .placeholder-fill.
    const QColor base(0x24, 0x24, 0x29);
    const QColor dark(0x20, 0x20, 0x25);
    p.fillRect(rect(), base);
    p.setPen(QPen(dark, 8));
    const int step = 16;
    for (int x = -height(); x < width() + height(); x += step) {
        p.drawLine(x, height(), x + height(), 0);
    }

    p.setClipping(false);
    p.setPen(QPen(Theme::Border, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 6, 6);

    if (!m_label.isEmpty()) {
        QFont f(QStringLiteral("Cascadia Mono"));
        f.setPixelSize(11);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
        p.setFont(f);
        p.setPen(Theme::TextMute);
        p.drawText(rect(), Qt::AlignCenter, m_label.toUpper());
    }
}
