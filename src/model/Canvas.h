#pragma once

#include <QRectF>
#include <QtGlobal>
#include <cmath>

namespace MalloyCanvas {
inline constexpr qreal Width = 1920.0;
inline constexpr qreal Height = 1080.0;
inline constexpr qreal MinItemSize = 16.0;
inline constexpr qreal SnapDistance = 12.0;

inline QRectF clampRect(QRectF rect) {
    rect = rect.normalized();

    const qreal w = qBound(MinItemSize, rect.width(), Width);
    const qreal h = qBound(MinItemSize, rect.height(), Height);
    rect.setSize({w, h});

    if (rect.left() < 0.0) rect.moveLeft(0.0);
    if (rect.top() < 0.0) rect.moveTop(0.0);
    if (rect.right() > Width) rect.moveRight(Width);
    if (rect.bottom() > Height) rect.moveBottom(Height);
    return rect;
}

inline QRectF fitRectForAspect(qreal aspect) {
    if (aspect <= 0.0) return {0.0, 0.0, Width, Height};

    qreal w = Width;
    qreal h = w / aspect;
    if (h > Height) {
        h = Height;
        w = h * aspect;
    }

    QRectF rect(0.0, 0.0, w, h);
    rect.moveCenter({Width / 2.0, Height / 2.0});
    return rect;
}

inline QRectF snapRect(QRectF rect) {
    rect = clampRect(rect);

    const qreal cx = rect.center().x();
    const qreal cy = rect.center().y();
    if (std::abs(cx - Width / 2.0) <= SnapDistance)
        rect.moveCenter({Width / 2.0, rect.center().y()});
    if (std::abs(cy - Height / 2.0) <= SnapDistance)
        rect.moveCenter({rect.center().x(), Height / 2.0});
    if (std::abs(rect.left()) <= SnapDistance) rect.moveLeft(0.0);
    if (std::abs(rect.top()) <= SnapDistance) rect.moveTop(0.0);
    if (std::abs(Width - rect.right()) <= SnapDistance) rect.moveRight(Width);
    if (std::abs(Height - rect.bottom()) <= SnapDistance) rect.moveBottom(Height);
    return clampRect(rect);
}
}
