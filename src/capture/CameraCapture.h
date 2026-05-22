#pragma once
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <thread>

// Webcam / capture-card video via Windows MediaFoundation (IMFSourceReader).
//
// availableDevices() is a cheap synchronous enumeration the UI can call directly.
// A CameraCapture instance owns a single device: start() spawns a read thread
// that converts each frame to an RGB QImage and emits frameReady(); stop() ends
// and joins it. Frames are emitted across the thread boundary via a queued
// connection (QImage is implicitly shared + a registered metatype).
class CameraCapture : public QObject {
    Q_OBJECT
public:
    struct Device {
        QString id;    // MediaFoundation symbolic link (stable across sessions)
        QString name;  // friendly device name, for display
    };

    // Connected video capture devices. Empty if none are present or if
    // MediaFoundation fails to start (degrades gracefully — no throw).
    static QList<Device> availableDevices();

    explicit CameraCapture(QObject* parent = nullptr);
    ~CameraCapture() override;

    void start(const QString& deviceId);   // spawn the read thread (no-op if running)
    void stop();                            // stop + join the read thread

signals:
    void frameReady(QImage frame);
    void error(QString message);

private:
    void captureLoop(QString deviceId);     // runs on m_thread

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
};
