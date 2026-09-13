#include "CameraCapture.h"
#include <memory>
#include <atomic>
#include <mutex>
#include <QPointer>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QDebug>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <chrono>

namespace {
// RAII for MFStartup/MFShutdown. MFSTARTUP_LITE is enough for capture too.
struct MfRuntime {
    bool ok = false;
    MfRuntime()  { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    ~MfRuntime() { if (ok) MFShutdown(); }
};

QString allocatedString(IMFActivate* dev, const GUID& key) {
    WCHAR* str = nullptr;
    UINT32 len = 0;
    QString out;
    if (SUCCEEDED(dev->GetAllocatedString(key, &str, &len)) && str) {
        out = QString::fromWCharArray(str, static_cast<int>(len));
        CoTaskMemFree(str);
    }
    return out;
}

template <typename T> void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Process-wide device cache. Enumeration is slow enough that every UI path
// reads this instead, and refreshes it in the background.
std::mutex& cacheMutex() { static std::mutex m; return m; }
QList<CameraCapture::Device>& deviceCache() { static QList<CameraCapture::Device> c; return c; }
QElapsedTimer& cacheAge() { static QElapsedTimer t; return t; }
bool& cacheFilled() { static bool filled = false; return filled; }
} // namespace

QList<CameraCapture::Device> CameraCapture::cachedDevices() {
    std::lock_guard<std::mutex> lock(cacheMutex());
    return deviceCache();
}

bool CameraCapture::hasEnumerated() {
    std::lock_guard<std::mutex> lock(cacheMutex());
    return cacheFilled();
}

bool CameraCapture::cacheIsStale(int seconds) {
    std::lock_guard<std::mutex> lock(cacheMutex());
    if (!cacheFilled()) return true;
    return cacheAge().hasExpired(qint64(seconds) * 1000);
}

void CameraDeviceNotifier::publish(const QList<CameraCapture::Device>& devices) {
    emit devicesRefreshed(devices);
}

namespace {
// Leaked on purpose: worker threads post here, and it must outlive every
// caller and the application object itself.
CameraDeviceNotifier* notifier() {
    static CameraDeviceNotifier* hub = new CameraDeviceNotifier;
    return hub;
}

// Set when the application starts shutting down, so a worker finishing during
// teardown does not try to post into an event loop that is going away.
std::atomic<bool>& shuttingDown() {
    static std::atomic<bool> flag{false};
    return flag;
}
}  // namespace

void CameraCapture::refreshDevicesAsync(QObject* context,
                                        std::function<void(QList<Device>)> done) {
    CameraDeviceNotifier* hub = notifier();

    static bool hookedShutdown = false;
    if (!hookedShutdown && qApp) {
        hookedShutdown = true;
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, hub,
                         [] { shuttingDown() = true; });
    }

    if (context && done) {
        // One-shot, and bound to `context`: if it is destroyed while the
        // enumeration runs, Qt disconnects and nothing is called.
        auto link = std::make_shared<QMetaObject::Connection>();
        *link = QObject::connect(hub, &CameraDeviceNotifier::devicesRefreshed, context,
                                 [done, link](const QList<Device>& devices) {
            QObject::disconnect(*link);
            done(devices);
        });
    }

    std::thread([] {
        const QList<Device> devices = availableDevices();
        {
            std::lock_guard<std::mutex> lock(cacheMutex());
            deviceCache() = devices;
            cacheAge().restart();
            cacheFilled() = true;
        }
        if (shuttingDown()) return;
        // Hops to the main thread. The target outlives the post by construction.
        QMetaObject::invokeMethod(notifier(), [devices] {
            notifier()->publish(devices);
        }, Qt::QueuedConnection);
    }).detach();
}

QList<CameraCapture::Device> CameraCapture::availableDevices() {
    QList<Device> result;
    MfRuntime mf;
    if (!mf.ok) return result;

    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1)) || !attrs) return result;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(MFEnumDeviceSources(attrs, &devices, &count)) && devices) {
        for (UINT32 i = 0; i < count; ++i) {
            if (!devices[i]) continue;
            const QString id = allocatedString(
                devices[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
            QString name = allocatedString(devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
            if (!id.isEmpty())
                result.append({id, name.isEmpty() ? QStringLiteral("Camera") : name});
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
    }
    attrs->Release();
    return result;
}

CameraCapture::CameraCapture(QObject* parent) : QObject(parent) {}

CameraCapture::~CameraCapture() { stop(); }

void CameraCapture::start(const QString& deviceId) {
    if (m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread(&CameraCapture::captureLoop, this, deviceId);
}

void CameraCapture::stop() {
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
}

namespace {

// Picks the camera's best native format and makes it current: highest frame
// rate first, then largest frame within a sane cap.
//
// Native rather than converted, because this decides what the device produces.
// The reader is still asked for RGB32 afterwards, which converts from whatever
// is chosen here.
void selectNativeMediaType(IMFSourceReader* reader) {
    if (!reader) return;

    constexpr UINT32 kMaxWidth  = 1920;
    constexpr UINT32 kMaxHeight = 1080;

    IMFMediaType* best = nullptr;
    double bestRate = -1.0;
    UINT64 bestArea = 0;

    for (DWORD i = 0;; ++i) {
        IMFMediaType* candidate = nullptr;
        const HRESULT hr = reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &candidate);
        if (FAILED(hr) || !candidate) break;   // MF_E_NO_MORE_TYPES ends the walk

        UINT32 w = 0, h = 0;
        UINT32 num = 0, den = 0;
        double rate = 0.0;
        if (SUCCEEDED(MFGetAttributeSize(candidate, MF_MT_FRAME_SIZE, &w, &h))
            && w > 0 && h > 0 && w <= kMaxWidth && h <= kMaxHeight) {
            if (SUCCEEDED(MFGetAttributeRatio(candidate, MF_MT_FRAME_RATE, &num, &den))
                && den > 0) {
                rate = double(num) / double(den);
            }
            const UINT64 area = UINT64(w) * UINT64(h);
            // Rate first, then area. The comparison is on the rate rounded to a
            // whole frame, because 60000/1001 and 60/1 are the same choice as
            // far as this decision goes.
            const double roundedRate = double(qRound(rate));
            const double roundedBest = double(qRound(bestRate));
            if (roundedRate > roundedBest
                || (qFuzzyCompare(roundedRate + 1.0, roundedBest + 1.0) && area > bestArea)) {
                safeRelease(best);
                best = candidate;
                best->AddRef();
                bestRate = rate;
                bestArea = area;
            }
        }
        safeRelease(candidate);
    }

    if (best) {
        reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, best);
        safeRelease(best);
    }
}

}  // namespace

void CameraCapture::captureLoop(QString deviceId) {
    // Own COM + MF on this worker thread.
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    MfRuntime mf;
    if (!mf.ok) { if (com) CoUninitialize(); emit error(QStringLiteral("MediaFoundation unavailable")); return; }

    // Build the device source from the symbolic link.
    IMFAttributes* devAttrs = nullptr;
    if (FAILED(MFCreateAttributes(&devAttrs, 2)) || !devAttrs) { if (com) CoUninitialize(); return; }
    devAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    devAttrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        reinterpret_cast<LPCWSTR>(deviceId.utf16()));

    IMFMediaSource* source = nullptr;
    HRESULT hr = MFCreateDeviceSource(devAttrs, &source);
    safeRelease(devAttrs);
    if (FAILED(hr) || !source) {
        emit error(QStringLiteral("Could not open the camera"));
        if (com) CoUninitialize();
        return;
    }

    // Source reader with video processing enabled so it can hand us RGB32
    // regardless of the camera's native format (NV12 / YUY2 / MJPG …).
    IMFAttributes* readerAttrs = nullptr;
    MFCreateAttributes(&readerAttrs, 1);
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    IMFSourceReader* reader = nullptr;
    hr = MFCreateSourceReaderFromMediaSource(source, readerAttrs, &reader);
    safeRelease(readerAttrs);
    if (FAILED(hr) || !reader) {
        source->Shutdown(); safeRelease(source);
        emit error(QStringLiteral("Could not start the camera reader"));
        if (com) CoUninitialize();
        return;
    }

    // Choose which of the camera's own formats to run, rather than accepting
    // whichever one it happens to offer first.
    //
    // A webcam advertises a list, and the first entry is not the best one. An
    // Elgato Facecam offers 960x540, 1280x720 and 1920x1080, each at 30 and at
    // 60, and taking the default can leave a 60 fps camera running at 30. The
    // rate is what is chosen for here, since dropped smoothness is what a user
    // notices; among equal rates the larger frame wins, capped so a capture
    // card claiming something enormous does not set the canvas cost.
    //
    // Failing to find one is not fatal: leaving the reader alone falls back to
    // exactly the previous behaviour, which worked.
    selectNativeMediaType(reader);

    // Request RGB32 (BGRA in memory, matching QImage::Format_RGB32). The
    // reader converts from whatever native format was chosen above; no camera
    // here offers RGB32 itself.
    IMFMediaType* outType = nullptr;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                nullptr, outType);
    safeRelease(outType);

    // Resolve the negotiated frame size + stride.
    UINT32 width = 0, height = 0;
    LONG stride = 0;
    IMFMediaType* current = nullptr;
    if (SUCCEEDED(reader->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current)) && current) {
        MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &width, &height);
        UINT32 s = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)))
            stride = static_cast<LONG>(static_cast<INT32>(s));
        safeRelease(current);
    }
    if (width == 0 || height == 0) {
        safeRelease(reader); source->Shutdown(); safeRelease(source);
        emit error(QStringLiteral("Camera returned no video format"));
        if (com) CoUninitialize();
        return;
    }
    if (stride == 0) stride = static_cast<LONG>(width) * 4;

    // What the camera actually agreed to, once. Until now nothing reported the
    // negotiated format, so a camera quietly running at half its advertised
    // rate looked identical to one running properly.
    {
        UINT32 rateNum = 0, rateDen = 0;
        IMFMediaType* agreed = nullptr;
        if (SUCCEEDED(reader->GetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &agreed))
            && agreed) {
            MFGetAttributeRatio(agreed, MF_MT_FRAME_RATE, &rateNum, &rateDen);
            safeRelease(agreed);
        }
        qInfo("camera format: %ux%u @ %.2f fps", width, height,
              rateDen ? double(rateNum) / double(rateDen) : 0.0);
    }

    while (m_running.load()) {
        DWORD streamFlags = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                0, nullptr, &streamFlags, &timestamp, &sample);
        if (FAILED(hr) || (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample) {
            // Format change or no data yet — avoid a busy spin.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (!m_delivering.load(std::memory_order_relaxed)) {
            safeRelease(sample);
            continue;
        }

        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (SUCCEEDED(buffer->Lock(&data, &maxLen, &curLen)) && data) {
                // A driver can hand back fewer bytes than the negotiated stride
                // and height imply. QImage would happily wrap the short buffer
                // and then read past it on copy, so check before trusting it.
                const qint64 needed = qint64(qAbs(stride)) * qint64(height);
                if (qint64(curLen) < needed) {
                    buffer->Unlock();
                    safeRelease(buffer);
                    safeRelease(sample);
                    continue;   // drop this frame; the device keeps streaming
                }
                CapturedFrame captured;
                if (!m_handoff.tryAcquire(captured)) {
                    buffer->Unlock();
                    safeRelease(buffer);
                    safeRelease(sample);
                    continue;
                }
                QImage frame(reinterpret_cast<const uchar*>(data),
                             static_cast<int>(width), static_cast<int>(height),
                             static_cast<int>(qAbs(stride)), QImage::Format_RGB32);
                // Deep copy before unlocking; flip if the buffer is bottom-up
                // (rare for RGB32 via the video processor, but handle it).
                QImage out;
                if (stride < 0) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
                    out = frame.flipped(Qt::Vertical);
#else
                    out = frame.mirrored(false, true);
#endif
                } else {
                    out = frame.copy();
                }
                buffer->Unlock();
                if (!out.isNull()) {
                    captured.image = std::move(out);
                    emit frameReady(CaptureFrameHandoff::imageForDelivery(std::move(captured)));
                }
            }
            safeRelease(buffer);
        }
        safeRelease(sample);
    }

    safeRelease(reader);
    source->Shutdown();
    safeRelease(source);
    if (com) CoUninitialize();
}
