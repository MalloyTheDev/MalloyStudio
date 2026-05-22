#pragma once
#include <QList>
#include <QString>

// Webcam / capture-card video via Windows MediaFoundation.
//
// Increment 1 exposes device enumeration only; the live IMFSourceReader capture
// worker is added in increment 2. Enumeration is cheap and synchronous, so the
// UI (Sources add dialog, Inspector device picker) can call it directly.
class CameraCapture {
public:
    struct Device {
        QString id;    // MediaFoundation symbolic link (stable across sessions)
        QString name;  // friendly device name, for display
    };

    // Connected video capture devices. Empty if none are present or if
    // MediaFoundation fails to start (degrades gracefully — no throw).
    static QList<Device> availableDevices();
};
