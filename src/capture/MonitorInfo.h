#pragma once
#include <QString>
#include <QRect>
#include <QList>

struct MonitorInfo {
    QString name;
    QRect    geometry;
    int      adapterIndex = 0;
    int      outputIndex  = 0;

    QString displayName() const {
        return QString("Monitor %1: %2  (%3\xC3\x97%4 @ %5,%6)")
            .arg(adapterIndex * 8 + outputIndex + 1)
            .arg(name)
            .arg(geometry.width()).arg(geometry.height())
            .arg(geometry.left()).arg(geometry.top());
    }
};

// Enumerate all connected monitors via DXGI. Defined in DxgiCapture.cpp.
QList<MonitorInfo> enumerateMonitors();
