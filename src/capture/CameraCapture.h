#pragma once
#include "CaptureFrameHandoff.h"
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <thread>

// Webcam / capture-card video via Windows MediaFoundation (IMFSourceReader).
//
// availableDevices() is NOT cheap: it starts MediaFoundation and was measured at
// about 3.5 s on a machine with no camera attached, so the UI uses
// cachedDevices() plus refreshDevicesAsync() instead of calling it directly.
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
    // MediaFoundation fails to start (degrades gracefully, no throw).
    //
    // BLOCKING: this starts and stops MediaFoundation, which takes seconds when
    // no device is attached. Call it from a worker thread, or use the cache and
    // the async refresh below.
    static QList<Device> availableDevices();

    // Last enumeration result. Returns immediately; empty until a refresh has
    // completed at least once.
    static QList<Device> cachedDevices();

    // True when the cache has never been filled, or is older than `seconds`.
    // Call sites use this to avoid spawning an enumeration per interaction.
    static bool cacheIsStale(int seconds = 30);

    // True once an enumeration has completed, whatever it found. An empty cache
    // that has been filled means "no cameras", not "not looked yet", and the
    // difference decides whether a call site may use it without blocking.
    static bool hasEnumerated();

    // Enumerates on a worker thread and delivers the result on `context`'s
    // thread once it finishes. Delivery is an ordinary Qt connection bound to
    // `context`, so a context destroyed while the enumeration is still running
    // disconnects itself and the callback never fires.
    static void refreshDevicesAsync(QObject* context,
                                    std::function<void(QList<Device>)> done = {});

    explicit CameraCapture(QObject* parent = nullptr);
    ~CameraCapture() override;

    void start(const QString& deviceId);   // spawn the read thread (no-op if running)
    void stop();                            // stop + join the read thread
    void setDelivering(bool delivering) {
        m_delivering.store(delivering, std::memory_order_relaxed);
    }
    CaptureStats stats() const { return m_handoff.stats(); }

signals:
    void frameReady(QImage frame);
    void error(QString message);

private:
    void captureLoop(QString deviceId);     // runs on m_thread

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_delivering{true};
    CaptureFrameHandoff m_handoff;
};

// Carries enumeration results from a worker thread to the main thread.
//
// A deliberately leaked singleton, so a result posted from a worker can never
// land on a destroyed object. Callers do not touch it: refreshDevicesAsync
// binds the connection to the caller's own context object, which is what makes
// receiver lifetime Qt's problem rather than ours. The previous version copied
// a QPointer across the thread boundary, which QPointer does not support.
class CameraDeviceNotifier : public QObject {
    Q_OBJECT
public:
    // Called on the main thread; emits devicesRefreshed().
    void publish(const QList<CameraCapture::Device>& devices);

signals:
    void devicesRefreshed(const QList<CameraCapture::Device>& devices);
};
