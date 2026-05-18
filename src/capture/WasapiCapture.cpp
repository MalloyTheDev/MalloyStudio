#include "WasapiCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <objbase.h>

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT is declared in <ksmedia.h> but its GUID
// definition lives in ksuser.lib, which MinGW's Qt build doesn't expose
// reliably. Define the value inline so the comparison works without
// changing the link line.
namespace {
constexpr GUID kIeeeFloatSubtype = {
    0x00000003, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};
inline bool isIeeeFloatSubtype(const GUID& g) {
    return std::memcmp(&g, &kIeeeFloatSubtype, sizeof(GUID)) == 0;
}
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

// REFTIME = 100-ns units. 200 ms buffer is conservative + low-latency enough.
constexpr REFERENCE_TIME kBufferDuration = 2'000'000; // 200 ms

// Convert a single sample of arbitrary device format into signed 16-bit.
// Returns the absolute value in 0..1 for VU computation as well.
struct ConvertedSample {
    qint16 s;
    float  abs01;
};

inline qint16 clamp16(int v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return static_cast<qint16>(v);
}

inline ConvertedSample fromFloat32(const float* src) {
    const float f = *src;
    const float clamped = std::max(-1.0f, std::min(1.0f, f));
    return { clamp16(static_cast<int>(clamped * 32767.0f)),
             std::min(1.0f, std::fabs(clamped)) };
}

inline ConvertedSample fromInt16(const qint16* src) {
    return { *src, std::fabs(static_cast<float>(*src)) / 32768.0f };
}

inline ConvertedSample fromInt32(const qint32* src) {
    const qint32 v = *src;
    return { static_cast<qint16>(v >> 16),
             std::fabs(static_cast<float>(v)) / 2147483648.0f };
}

} // namespace

WasapiCapture::WasapiCapture(QString deviceId, bool loopback, QObject* parent)
    : QThread(parent), m_deviceId(std::move(deviceId)), m_loopback(loopback) {}

WasapiCapture::~WasapiCapture() {
    requestStop();
    wait(4000);
}

void WasapiCapture::requestStop() {
    m_running.store(false, std::memory_order_relaxed);
}

void WasapiCapture::run() {
    m_running.store(true, std::memory_order_relaxed);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(hr); // RPC_E_CHANGED_MODE means thread is already STA — uncommon for QThread

    auto fail = [&](const QString& msg) {
        emit captureError(msg);
        if (comOwned) CoUninitialize();
    };

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) { fail(QStringLiteral("WASAPI: enumerator unavailable")); return; }

    IMMDevice* device = nullptr;
    if (m_deviceId.isEmpty()) {
        // Loopback always pulls from a render endpoint (eRender). For a true
        // input (microphone) we'd ask for eCapture; for v4 we always loopback.
        const EDataFlow flow = m_loopback ? eRender : eCapture;
        hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
    } else {
        hr = enumerator->GetDevice(reinterpret_cast<LPCWSTR>(m_deviceId.utf16()), &device);
    }
    enumerator->Release();
    if (FAILED(hr) || !device) { fail(QStringLiteral("WASAPI: device unavailable")); return; }

    IAudioClient* client = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(hr) || !client) { fail(QStringLiteral("WASAPI: IAudioClient activation failed")); return; }

    WAVEFORMATEX* mixFormat = nullptr;
    if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat) {
        client->Release();
        fail(QStringLiteral("WASAPI: GetMixFormat failed"));
        return;
    }

    const DWORD flags = (m_loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0u)
                      | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                            kBufferDuration, 0, mixFormat, nullptr);
    if (FAILED(hr)) {
        CoTaskMemFree(mixFormat);
        client->Release();
        fail(QStringLiteral("WASAPI: Initialize failed (0x%1)")
                .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
        return;
    }

    HANDLE bufferReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!bufferReadyEvent) {
        CoTaskMemFree(mixFormat);
        client->Release();
        fail(QStringLiteral("WASAPI: event handle creation failed"));
        return;
    }
    client->SetEventHandle(bufferReadyEvent);

    IAudioCaptureClient* capture = nullptr;
    hr = client->GetService(__uuidof(IAudioCaptureClient),
                            reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) {
        CloseHandle(bufferReadyEvent);
        CoTaskMemFree(mixFormat);
        client->Release();
        fail(QStringLiteral("WASAPI: GetService(IAudioCaptureClient) failed"));
        return;
    }

    // Detect format up front so the hot loop can stay branch-light.
    const int  deviceChannels   = mixFormat->nChannels;
    const int  deviceSampleRate = mixFormat->nSamplesPerSec;
    const bool deviceIsFloat    = (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
                               || (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                                   && isIeeeFloatSubtype(
                                       reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat)->SubFormat));
    const int  deviceBytesPerSample = mixFormat->wBitsPerSample / 8;
    const int  deviceFrameBytes     = deviceBytesPerSample * deviceChannels;

    // 48 kHz canonical; pull-rate resample by integer ratio if device sample
    // rate is the common 44.1/48/96 kHz. For v4 we assume devices are 48 kHz
    // (Windows default for most hardware) and just pass through; if the rate
    // differs we still emit but flag it once.
    bool warnedSampleRate = false;

    hr = client->Start();
    if (FAILED(hr)) {
        capture->Release();
        CloseHandle(bufferReadyEvent);
        CoTaskMemFree(mixFormat);
        client->Release();
        fail(QStringLiteral("WASAPI: Start failed"));
        return;
    }

    // Output bus buffer reused per chunk.
    std::vector<qint16> outBuf;
    outBuf.reserve(SampleRate / 50 * Channels); // ~20 ms

    while (m_running.load(std::memory_order_relaxed)) {
        const DWORD waitRes = WaitForSingleObject(bufferReadyEvent, 200);
        if (waitRes == WAIT_TIMEOUT) continue;
        if (waitRes != WAIT_OBJECT_0) break;

        UINT32 packetFrames = 0;
        if (FAILED(capture->GetNextPacketSize(&packetFrames))) break;

        while (packetFrames > 0 && m_running.load(std::memory_order_relaxed)) {
            BYTE*  data    = nullptr;
            UINT32 frames  = 0;
            DWORD  pktFlags = 0;
            hr = capture->GetBuffer(&data, &frames, &pktFlags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(hr)) {
                m_running.store(false, std::memory_order_relaxed);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
                    emit captureError(QStringLiteral("WASAPI: device invalidated"));
                break;
            }

            outBuf.clear();
            outBuf.resize(static_cast<size_t>(frames) * Channels);

            float peakL = 0.0f, peakR = 0.0f;
            const bool silent = (pktFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            if (silent) {
                std::memset(outBuf.data(), 0, outBuf.size() * sizeof(qint16));
            } else {
                // Per-frame: pick L (channel 0) and R (channel 1, or duplicate L for mono).
                const BYTE* src = data;
                for (UINT32 f = 0; f < frames; ++f) {
                    ConvertedSample l{0,0}, r{0,0};
                    const BYTE* chBase = src + static_cast<size_t>(f) * deviceFrameBytes;

                    if (deviceIsFloat) {
                        l = fromFloat32(reinterpret_cast<const float*>(chBase));
                        r = deviceChannels >= 2
                            ? fromFloat32(reinterpret_cast<const float*>(chBase + sizeof(float)))
                            : l;
                    } else if (deviceBytesPerSample == 2) {
                        l = fromInt16(reinterpret_cast<const qint16*>(chBase));
                        r = deviceChannels >= 2
                            ? fromInt16(reinterpret_cast<const qint16*>(chBase + sizeof(qint16)))
                            : l;
                    } else if (deviceBytesPerSample == 4) {
                        l = fromInt32(reinterpret_cast<const qint32*>(chBase));
                        r = deviceChannels >= 2
                            ? fromInt32(reinterpret_cast<const qint32*>(chBase + sizeof(qint32)))
                            : l;
                    } else {
                        l = r = {0, 0.0f};
                    }

                    outBuf[static_cast<size_t>(f) * 2 + 0] = l.s;
                    outBuf[static_cast<size_t>(f) * 2 + 1] = r.s;
                    if (l.abs01 > peakL) peakL = l.abs01;
                    if (r.abs01 > peakR) peakR = r.abs01;
                }
            }

            capture->ReleaseBuffer(frames);
            packetFrames -= frames;

            if (!warnedSampleRate && deviceSampleRate != SampleRate) {
                warnedSampleRate = true;
                emit captureError(QStringLiteral("WASAPI: device is %1 Hz, expected %2 Hz "
                                                 "(no resample in v4 — audio pitch may drift)")
                                    .arg(deviceSampleRate).arg(SampleRate));
            }

            QByteArray chunk(reinterpret_cast<const char*>(outBuf.data()),
                             static_cast<int>(outBuf.size() * sizeof(qint16)));
            emit samplesReady(std::move(chunk));
            emit levelsUpdated(peakL, peakR);

            if (FAILED(capture->GetNextPacketSize(&packetFrames))) {
                m_running.store(false, std::memory_order_relaxed);
                break;
            }
        }
    }

    client->Stop();
    capture->Release();
    CloseHandle(bufferReadyEvent);
    CoTaskMemFree(mixFormat);
    client->Release();
    if (comOwned) CoUninitialize();
}
