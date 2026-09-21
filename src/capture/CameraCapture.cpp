#include "CameraCapture.h"
#include "platform/FrameProfile.h"
#include <algorithm>
#include <cstring>
#include <limits>
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

// The one enumeration allowed to run. `running` covers a worker from its start
// until it has posted its result, and the thread stays joinable so shutdown can
// wait for it: detached, it could still be writing the cache while static
// destruction took the cache away.
struct Enumeration {
    std::mutex mutex;
    std::thread thread;
    bool running = false;
    std::function<QList<CameraCapture::Device>()> enumerate;   // empty: the real one

    // Only reached when no application object was destroyed to do this first.
    ~Enumeration() {
        shuttingDown() = true;
        join();
    }

    void join() {
        std::thread finished;
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = std::move(thread);
        }
        if (finished.joinable()) finished.join();
    }
};

Enumeration& enumeration() {
    // Everything the worker touches is constructed first, so it is destroyed
    // after this and is still there while the destructor above waits.
    cacheMutex();
    deviceCache();
    cacheAge();
    cacheFilled();
    shuttingDown();
    static Enumeration e;
    return e;
}

void shutDownEnumeration() {
    shuttingDown() = true;
    CameraCapture::waitForEnumeration();
}

// A context's unanswered request. A child of the context, so it goes when the
// context does and takes its connection with it, and never more than one per
// context, so asking again replaces the callback rather than adding an answer.
class PendingRefresh : public QObject {
public:
    explicit PendingRefresh(QObject* context) : QObject(context) {}
    std::function<void(QList<CameraCapture::Device>)> done;
};

PendingRefresh* pendingRefreshOf(QObject* context) {
    for (QObject* child : context->children())
        if (auto* pending = dynamic_cast<PendingRefresh*>(child)) return pending;
    return nullptr;
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
        // Joined as the application object is destroyed, before any static is.
        qAddPostRoutine(shutDownEnumeration);
    }

    if (context && done) {
        if (PendingRefresh* pending = pendingRefreshOf(context)) {
            pending->done = std::move(done);
        } else {
            auto* waiter = new PendingRefresh(context);
            waiter->done = std::move(done);
            QObject::connect(hub, &CameraDeviceNotifier::devicesRefreshed, waiter,
                             [hub, waiter](const QList<Device>& devices) {
                // Off the context before the callback runs, so a callback that
                // asks again registers afresh instead of finding this one.
                auto answer = std::move(waiter->done);
                QObject::disconnect(hub, nullptr, waiter, nullptr);
                waiter->setParent(nullptr);
                waiter->deleteLater();
                if (answer) answer(devices);
            });
        }
    }

    Enumeration& e = enumeration();
    std::lock_guard<std::mutex> lock(e.mutex);
    if (e.running || shuttingDown()) return;
    // Finished but not yet joined: it has posted its result and is returning.
    if (e.thread.joinable()) e.thread.join();
    e.running = true;
    e.thread = std::thread([enumerate = e.enumerate] {
        const QList<Device> devices = enumerate ? enumerate() : availableDevices();
        {
            std::lock_guard<std::mutex> lock(cacheMutex());
            deviceCache() = devices;
            cacheAge().restart();
            cacheFilled() = true;
        }
        if (!shuttingDown()) {
            // Hops to the main thread. The target outlives the post by construction.
            QMetaObject::invokeMethod(notifier(), [devices] {
                notifier()->publish(devices);
            }, Qt::QueuedConnection);
        }
        // Cleared after the post, so a request arriving in between is answered
        // by this result rather than starting another enumeration.
        Enumeration& self = enumeration();
        std::lock_guard<std::mutex> lock(self.mutex);
        self.running = false;
    });
}

void CameraCapture::waitForEnumeration() {
    enumeration().join();
}

void CameraCapture::setEnumeratorForTesting(std::function<QList<Device>()> enumerate) {
    Enumeration& e = enumeration();
    e.join();
    {
        std::lock_guard<std::mutex> lock(e.mutex);
        e.enumerate = std::move(enumerate);
    }
    std::lock_guard<std::mutex> lock(cacheMutex());
    deviceCache().clear();
    cacheAge().invalidate();
    cacheFilled() = false;
}

QStringList CameraCapture::pickerLabels(const QList<Device>& devices) {
    QStringList labels;
    for (const Device& device : devices) {
        QString label = device.name;
        for (int n = 2; labels.contains(label); ++n)
            label = QStringLiteral("%1 (%2)").arg(device.name).arg(n);
        labels.append(label);
    }
    return labels;
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

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

// Video subtypes are one GUID with the format in its first field. Anything not
// built that way is reported as zero, which ranks as unrecognised.
quint32 subtypeOf(const GUID& subtype) {
    const GUID& base = MFVideoFormat_Base;
    if (subtype.Data2 != base.Data2 || subtype.Data3 != base.Data3
        || memcmp(subtype.Data4, base.Data4, sizeof(base.Data4)) != 0) {
        return 0;
    }
    return subtype.Data1;
}

CameraCapture::NativeFormat formatOf(IMFMediaType* type) {
    CameraCapture::NativeFormat format;
    if (!type) return format;
    GUID subtype = GUID_NULL;
    if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) format.subtype = subtypeOf(subtype);
    UINT32 w = 0, h = 0;
    if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h))
        && w <= UINT32(std::numeric_limits<int>::max())
        && h <= UINT32(std::numeric_limits<int>::max())) {
        format.width = int(w);
        format.height = int(h);
    }
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den))) {
        format.rateNumerator = num;
        format.rateDenominator = den;
    }
    return format;
}

// Everything the camera lists, in the reader's own order: the order the
// indices from rankNativeFormats refer to.
QList<CameraCapture::NativeFormat> nativeFormats(IMFSourceReader* reader) {
    QList<CameraCapture::NativeFormat> formats;
    for (DWORD i = 0;; ++i) {
        IMFMediaType* type = nullptr;
        const HRESULT hr = reader->GetNativeMediaType(kVideoStream, i, &type);
        if (FAILED(hr) || !type) break;   // MF_E_NO_MORE_TYPES ends the walk
        formats.append(formatOf(type));
        safeRelease(type);
    }
    return formats;
}

// What the reader's video processor has to do to make RGB32 of a subtype, in
// preference order.
int conversionCost(quint32 subtype) {
    switch (subtype) {
    case CameraCapture::kSubtypeRgb32:
    case 21:   // D3DFMT_A8R8G8B8, MFVideoFormat_ARGB32
        return 0;   // handed over as it is
    case 20:   // D3DFMT_R8G8B8, MFVideoFormat_RGB24
    case CameraCapture::fourcc('N', 'V', '1', '2'):
    case CameraCapture::fourcc('Y', 'U', 'Y', '2'):
    case CameraCapture::fourcc('U', 'Y', 'V', 'Y'):
    case CameraCapture::fourcc('I', '4', '2', '0'):
    case CameraCapture::fourcc('I', 'Y', 'U', 'V'):
    case CameraCapture::fourcc('Y', 'V', '1', '2'):
        return 1;   // a colour conversion
    case CameraCapture::fourcc('M', 'J', 'P', 'G'):
        return 2;   // a JPEG decode on every frame, then the conversion
    default:
        return 3;   // unrecognised: may need a decoder, or not convert at all
    }
}

// The frames the reader is handing over, as the loop wraps them.
struct OutputFormat {
    UINT32 width = 0;
    UINT32 height = 0;
    LONG stride = 0;
};

// Reads the reader's current output, and refuses anything the loop cannot wrap
// as RGB32. Checking the subtype matters: a reader that kept a YUY2 or MJPG
// type would otherwise have its bytes wrapped as RGB32, giving a null image or
// failing the length check on every frame, with nothing reported.
bool readOutputFormat(IMFSourceReader* reader, OutputFormat& out) {
    IMFMediaType* current = nullptr;
    if (FAILED(reader->GetCurrentMediaType(kVideoStream, &current)) || !current) return false;
    GUID subtype = GUID_NULL;
    const bool rgb32 = SUCCEEDED(current->GetGUID(MF_MT_SUBTYPE, &subtype))
                       && IsEqualGUID(subtype, MFVideoFormat_RGB32);
    UINT32 w = 0, h = 0;
    const bool sized = SUCCEEDED(MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h));
    UINT32 s = 0;
    LONG stride = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)))
        stride = static_cast<LONG>(static_cast<INT32>(s));
    safeRelease(current);

    // Bounded so the byte arithmetic below and in QImage cannot overflow.
    constexpr UINT32 kMaxSide = 16384;
    if (!rgb32 || !sized || w == 0 || h == 0 || w > kMaxSide || h > kMaxSide) return false;
    if (stride == 0) stride = static_cast<LONG>(w) * 4;
    // QImage refuses a line shorter than its pixels, so such a stride would
    // make every frame a null image.
    if (qAbs(qint64(stride)) < qint64(w) * 4) return false;
    out = {w, h, stride};
    return true;
}

// Asks for RGB32 converted from whatever native format is current. A partial
// type, so the frame size and rate stay the native format's.
bool requestRgb32(IMFSourceReader* reader) {
    IMFMediaType* type = nullptr;
    if (FAILED(MFCreateMediaType(&type)) || !type) return false;
    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(kVideoStream, nullptr, type);
    safeRelease(type);
    OutputFormat agreed;
    return SUCCEEDED(hr) && readOutputFormat(reader, agreed);
}

}  // namespace

std::optional<int> CameraCapture::negotiateFormat(
    const QList<NativeFormat>& formats,
    const std::function<FormatAttempt(int index)>& attempt) {
    QList<quint32> unconvertible;
    for (const int index : rankNativeFormats(formats)) {
        const quint32 subtype = formats.at(index).subtype;
        if (unconvertible.contains(subtype)) continue;
        switch (attempt(index)) {
        case FormatAttempt::Accepted:
            return index;
        case FormatAttempt::ConversionRefused:
            // Zero is every unrecognised subtype at once, so it proves nothing
            // about the next one.
            if (subtype != 0) unconvertible.append(subtype);
            break;
        case FormatAttempt::DeviceRefused:
            break;
        }
    }
    if (attempt(-1) == FormatAttempt::Accepted) return -1;
    return std::nullopt;
}

QList<int> CameraCapture::rankNativeFormats(const QList<NativeFormat>& formats) {
    constexpr int kMaxWidth  = 1920;
    constexpr int kMaxHeight = 1080;

    QList<int> ranked;
    for (int i = 0; i < formats.size(); ++i) {
        const NativeFormat& f = formats.at(i);
        if (f.width > 0 && f.height > 0 && f.width <= kMaxWidth && f.height <= kMaxHeight)
            ranked.append(i);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [&formats](int a, int b) {
        const NativeFormat& x = formats.at(a);
        const NativeFormat& y = formats.at(b);
        const qint64 rateX = qRound64(x.frameRate());
        const qint64 rateY = qRound64(y.frameRate());
        if (rateX != rateY) return rateX > rateY;
        const qint64 areaX = qint64(x.width) * x.height;
        const qint64 areaY = qint64(y.width) * y.height;
        if (areaX != areaY) return areaX > areaY;
        return conversionCost(x.subtype) < conversionCost(y.subtype);
    });
    return ranked;
}

QString CameraCapture::describe(const NativeFormat& format) {
    QString subtype;
    switch (format.subtype) {
    case kSubtypeRgb32: subtype = QStringLiteral("RGB32");  break;
    case 21:            subtype = QStringLiteral("ARGB32"); break;
    case 20:            subtype = QStringLiteral("RGB24");  break;
    default: {
        bool printable = format.subtype != 0;
        for (int shift = 0; shift < 32 && printable; shift += 8) {
            const char c = char((format.subtype >> shift) & 0xFF);
            printable = c >= 0x20 && c < 0x7F;
        }
        if (printable) {
            for (int shift = 0; shift < 32; shift += 8)
                subtype += QLatin1Char(char((format.subtype >> shift) & 0xFF));
            subtype = subtype.trimmed();
        } else {
            subtype = QStringLiteral("0x%1").arg(format.subtype, 8, 16, QLatin1Char('0'));
        }
    }
    }
    return QStringLiteral("%1x%2 %3 at %4 fps")
        .arg(format.width).arg(format.height).arg(subtype)
        .arg(format.frameRate(), 0, 'f', 2);
}

void CameraCapture::captureLoop(QString deviceId) {
    // Own COM + MF on this worker thread.
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    MfRuntime mf;
    if (!mf.ok) { if (com) CoUninitialize(); emit error(QStringLiteral("MediaFoundation unavailable")); return; }

    // Build the device source from the symbolic link.
    IMFAttributes* devAttrs = nullptr;
    HRESULT hr = MFCreateAttributes(&devAttrs, 2);
    if (SUCCEEDED(hr) && !devAttrs) hr = E_POINTER;
    if (SUCCEEDED(hr))
        hr = devAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                               MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr))
        hr = devAttrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                 reinterpret_cast<LPCWSTR>(deviceId.utf16()));

    IMFMediaSource* source = nullptr;
    if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(devAttrs, &source);
    safeRelease(devAttrs);
    if (FAILED(hr) || !source) {
        safeRelease(source);
        emit error(QStringLiteral("Could not open the camera"));
        if (com) CoUninitialize();
        return;
    }

    // Source reader with video processing enabled so it can hand us RGB32
    // regardless of the camera's native format (NV12 / YUY2 / MJPG …).
    IMFAttributes* readerAttrs = nullptr;
    IMFSourceReader* reader = nullptr;
    hr = MFCreateAttributes(&readerAttrs, 1);
    if (SUCCEEDED(hr) && !readerAttrs) hr = E_POINTER;
    if (SUCCEEDED(hr)) hr = readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(source, readerAttrs, &reader);
    safeRelease(readerAttrs);
    if (FAILED(hr) || !reader) {
        safeRelease(reader);
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
    // 60 and each as both UYVY and MJPG, and taking the default can leave a
    // 60 fps camera running at 30, or decoding JPEG when it need not.
    // rankNativeFormats holds the rule. The format sets size and rate: the
    // RGB32 request is a partial type (BGRA in memory, matching
    // QImage::Format_RGB32), so the reader's conversion keeps both.
    //
    // Each result is checked. A native type the reader cannot convert would
    // otherwise stay current and have its bytes wrapped as RGB32: a null
    // image, or every frame failing the length check, and a black source with
    // no error. negotiateFormat moves on to the next format and finally to the
    // device's own default; when even that fails, the camera says so.
    IMFMediaType* deviceDefault = nullptr;
    if (FAILED(reader->GetNativeMediaType(
            kVideoStream, DWORD(MF_SOURCE_READER_CURRENT_TYPE_INDEX), &deviceDefault))) {
        deviceDefault = nullptr;
    }
    const QList<NativeFormat> offered = nativeFormats(reader);
    const std::optional<int> chosen = negotiateFormat(offered, [&](int index) {
        IMFMediaType* native = nullptr;
        if (index >= 0) {
            if (FAILED(reader->GetNativeMediaType(kVideoStream, DWORD(index), &native)))
                native = nullptr;
            if (!native) return FormatAttempt::DeviceRefused;
        } else if (deviceDefault) {
            native = deviceDefault;
            native->AddRef();
        }
        // With no default to go back to, the default is whatever is current.
        if (native) {
            const HRESULT nativeHr = reader->SetCurrentMediaType(kVideoStream, nullptr, native);
            safeRelease(native);
            if (FAILED(nativeHr)) return FormatAttempt::DeviceRefused;
        }
        return requestRgb32(reader) ? FormatAttempt::Accepted
                                    : FormatAttempt::ConversionRefused;
    });
    safeRelease(deviceDefault);

    OutputFormat format;
    if (!chosen || !readOutputFormat(reader, format)) {
        qWarning("camera: none of %lld native formats could be delivered as RGB32",
                 qlonglong(offered.size()));
        safeRelease(reader); source->Shutdown(); safeRelease(source);
        emit error(QStringLiteral("Camera format not supported"));
        if (com) CoUninitialize();
        return;
    }

    // What the camera actually agreed to, once. Until now nothing reported the
    // negotiated format, so a camera quietly running at half its advertised
    // rate looked identical to one running properly. The native format is the
    // one the device runs, which is what decides the rate and the cost; the
    // RGB32 size is what arrives here.
    {
        NativeFormat running;
        if (*chosen >= 0) {
            running = offered.at(*chosen);
        } else {
            IMFMediaType* native = nullptr;
            if (SUCCEEDED(reader->GetNativeMediaType(
                    kVideoStream, DWORD(MF_SOURCE_READER_CURRENT_TYPE_INDEX), &native))
                && native) {
                running = formatOf(native);
                safeRelease(native);
            }
        }
        qInfo("camera format: %s (%s of %lld offered), delivered as RGB32 %ux%u",
              qPrintable(describe(running)),
              *chosen >= 0 ? "chosen" : "device default", qlonglong(offered.size()),
              format.width, format.height);
    }

    // Wall clock since the previous frame, for the CAMERA ARRIVAL stage.
    // Invalid until the first frame, and only read while profiling.
    QElapsedTimer sinceArrival;

    while (m_running.load()) {
        DWORD streamFlags = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                0, nullptr, &streamFlags, &timestamp, &sample);
        if (FAILED(hr) || (streamFlags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
            safeRelease(sample);
            // Unplugged, taken by another application, or access revoked. It
            // used to end here in silence: the session stayed, the preview kept
            // its last frame, and nothing ever tried the camera again. Said
            // unless this is a stop, so the controller drops and retries it.
            if (m_running.load()) {
                emit error(FAILED(hr)
                    ? QStringLiteral("Camera stopped (0x%1)").arg(quint32(hr), 8, 16, QLatin1Char('0'))
                    : QStringLiteral("Camera stopped"));
            }
            break;
        }
        if (streamFlags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED
                           | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) {
            // The device changed format mid-stream. This sample and those
            // after it are in the new format, so the size and stride they are
            // wrapped with are read again. With the old ones kept, every frame
            // would fail the length check from here on and the source would
            // freeze on its last picture.
            if (!readOutputFormat(reader, format)) {
                safeRelease(sample);
                if (m_running.load()) {
                    qWarning("camera: format changed to one that cannot be delivered as RGB32");
                    emit error(QStringLiteral("Camera format changed and is not supported"));
                }
                break;
            }
            qInfo("camera format changed: delivered as RGB32 %ux%u", format.width, format.height);
        }
        if (!sample) {
            // Format change or no data yet — avoid a busy spin.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Every frame the device delivers, before anything decides whether it
        // is wanted, so the count per second is the rate the camera is really
        // running at rather than what composition made of it.
        if (FrameProfile::enabled()) {
            if (sinceArrival.isValid())
                FrameProfile::record(FrameProfile::Stage::CameraArrival,
                                     sinceArrival.nsecsElapsed());
            sinceArrival.start();
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
                const qint64 needed = qint64(qAbs(format.stride)) * qint64(format.height);
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
                // Deep copy before unlocking; flip if the buffer is bottom-up
                // (rare for RGB32 via the video processor, but handle it).
                //
                // Timed as CAPTURE READBACK, the copy into a QImage that the
                // other backends time too. The reader's conversion to RGB32
                // happens inside ReadSample and cannot be told apart from
                // waiting for the frame, so it is not in this figure.
                QImage out;
                {
                    FrameProfile::Scoped timing(FrameProfile::Stage::CaptureReadback);
                    QImage frame(reinterpret_cast<const uchar*>(data),
                                 static_cast<int>(format.width), static_cast<int>(format.height),
                                 static_cast<int>(qAbs(format.stride)), QImage::Format_RGB32);
                    if (format.stride < 0) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
                        out = frame.flipped(Qt::Vertical);
#else
                        out = frame.mirrored(false, true);
#endif
                    } else {
                        out = frame.copy();
                    }
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
