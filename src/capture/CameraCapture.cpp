#include "CameraCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>

namespace {
// RAII for MFStartup/MFShutdown. MFSTARTUP_LITE is enough for enumeration.
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
} // namespace

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
