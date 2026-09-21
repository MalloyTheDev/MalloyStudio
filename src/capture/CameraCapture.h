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
// that chooses one of the device's native formats (rankNativeFormats), has the
// reader convert each frame to an RGB QImage and emits frameReady(); stop() ends
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

    // One format a camera produces itself, as its source reader lists it.
    //
    // `subtype` is the first field of the Media Foundation subtype GUID, which
    // for video is the FOURCC ('NV12', 'YUY2', 'MJPG') or, for the RGB types,
    // a D3DFORMAT number. Zero stands for a subtype outside that family.
    struct NativeFormat {
        quint32 subtype = 0;
        int width = 0;
        int height = 0;
        quint32 rateNumerator = 0;
        quint32 rateDenominator = 0;

        double frameRate() const {
            return rateDenominator ? double(rateNumerator) / double(rateDenominator) : 0.0;
        }
    };

    static constexpr quint32 fourcc(char a, char b, char c, char d) {
        return quint32(quint8(a)) | (quint32(quint8(b)) << 8)
             | (quint32(quint8(c)) << 16) | (quint32(quint8(d)) << 24);
    }
    // MFVideoFormat_RGB32 carries D3DFMT_X8R8G8B8 rather than a FOURCC.
    static constexpr quint32 kSubtypeRgb32 = 22;

    // The native formats worth running, best first, as indices into `formats`.
    //
    // Frame rate decides first, compared to the nearest whole frame so 59.94
    // and 60 are the same choice, because lost smoothness is what a user sees.
    // Then the larger frame. Then the subtype that costs least to turn into the
    // RGB32 the reader hands over: RGB32 itself, then uncompressed YUV, then
    // MJPG, which is a JPEG decode on every frame, then anything unrecognised.
    // Then the device's own order.
    //
    // Frames larger than 1920x1080 are left out, so a capture card advertising
    // something enormous does not set the cost of every composition, and so
    // are frames with no size. An empty result means nothing listed is usable
    // and the device's own default should stand.
    static QList<int> rankNativeFormats(const QList<NativeFormat>& formats);

    // "1920x1080 NV12 at 60.00 fps", for the log.
    static QString describe(const NativeFormat& format);

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
