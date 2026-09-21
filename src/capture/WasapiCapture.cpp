#include "WasapiCapture.h"
#include "audio/Resampler.h"
#include "audio/StereoDownmix.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
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
// KSDATAFORMAT_SUBTYPE_PCM, for the same reason.
constexpr GUID kPcmSubtype = {
    0x00000001, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};
inline bool isPcmSubtype(const GUID& g) {
    return std::memcmp(&g, &kPcmSubtype, sizeof(GUID)) == 0;
}
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

// REFTIME = 100-ns units. 200 ms buffer is conservative + low-latency enough.
constexpr REFERENCE_TIME kBufferDuration = 2'000'000; // 200 ms

// The parts of a device's format that decide how its samples are read.
DeviceSampleFormat sampleFormatOf(const WAVEFORMATEX& wf) {
    SampleType type = SampleType::Other;
    quint32 mask = 0;
    if (wf.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        // Only a format that is as long as it claims to be is read as one.
        if (wf.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wf);
            if (isIeeeFloatSubtype(ext.SubFormat)) type = SampleType::Float;
            else if (isPcmSubtype(ext.SubFormat)) type = SampleType::Pcm;
            mask = ext.dwChannelMask;
        }
    } else if (wf.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        type = SampleType::Float;
    } else if (wf.wFormatTag == WAVE_FORMAT_PCM) {
        type = SampleType::Pcm;
    }
    return describeDeviceFormat(type, wf.wBitsPerSample, wf.nBlockAlign, wf.nChannels, mask);
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
    // Stopped before it began: do not open the device only to close it.
    if (!m_running.load(std::memory_order_relaxed)) return;

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

    // Refused before the stream starts, and said: a format this cannot read
    // used to be captured as silence while the input looked connected.
    const DeviceSampleFormat format = sampleFormatOf(*mixFormat);
    if (!format.supported()) {
        const QString what = QStringLiteral("WASAPI: unsupported sample format "
                                            "(tag 0x%1, %2 bits, %3 channels)")
                                 .arg(uint(mixFormat->wFormatTag), 4, 16, QLatin1Char('0'))
                                 .arg(uint(mixFormat->wBitsPerSample))
                                 .arg(uint(mixFormat->nChannels));
        CoTaskMemFree(mixFormat);
        client->Release();
        fail(what);
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

    const int deviceSampleRate = mixFormat->nSamplesPerSec;

    // 48 kHz canonical; pull-rate resample by integer ratio if device sample
    // rate is the common 44.1/48/96 kHz. For v4 we assume devices are 48 kHz
    // (Windows default for most hardware) and just pass through; if the rate
    // differs we still emit but flag it once.
    // Converts the device rate to the canonical 48 kHz. A device already at
    // 48 kHz makes this a straight copy.
    StereoResampler resampler(deviceSampleRate, SampleRate);
    std::vector<qint16> resampled;
    resampled.reserve(static_cast<size_t>(SampleRate / 25) * Channels);

    hr = client->Start();
    if (FAILED(hr)) {
        capture->Release();
        CloseHandle(bufferReadyEvent);
        CoTaskMemFree(mixFormat);
        client->Release();
        fail(QStringLiteral("WASAPI: Start failed"));
        return;
    }

    // The loop below has to come back for each packet before the endpoint's
    // buffer overruns, while the encoder and the compositor keep the cores
    // busy. MMCSS schedules a thread registered for its "Audio" task ahead of
    // ordinary work, which is what Windows expects of an audio client. Without
    // it the capture is an ordinary thread competing with ffmpeg.
    DWORD mmcssTask = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcssTask);
    if (!mmcss) {
        qWarning("WASAPI: could not register the capture thread with MMCSS (error %lu)",
                 GetLastError());
    }

    // Packets the device marked as not following on from the one before: the
    // endpoint's buffer overran before this thread read it, and the sound in
    // between is gone. The first packet of a stream can carry the flag
    // without anything being lost, so it is not counted.
    int discontinuities = 0;
    bool firstPacket = true;

    // Output bus buffer reused per chunk.
    std::vector<qint16> outBuf;
    outBuf.reserve(SampleRate / 50 * Channels); // ~20 ms

    // Every way out of the loop other than a stop is said. Only a GetBuffer
    // failure used to be, so a device unplugged or reconfigured between
    // packets ended the worker in silence and the mixer went on showing the
    // input as connected while it recorded nothing.
    const auto lost = [&](const char* what, HRESULT h) {
        m_running.store(false, std::memory_order_relaxed);
        emit captureError(QStringLiteral("WASAPI: %1 (0x%2)")
                              .arg(QLatin1String(what))
                              .arg(static_cast<quint32>(h), 8, 16, QLatin1Char('0')));
    };

    while (m_running.load(std::memory_order_relaxed)) {
        const DWORD waitRes = WaitForSingleObject(bufferReadyEvent, 200);
        UINT32 packetFrames = 0;
        if (waitRes == WAIT_TIMEOUT) {
            // An endpoint removed while it is silent signals nothing at all,
            // so it is asked: an invalidated device answers with an error.
            hr = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) { lost("device lost", hr); break; }
            continue;
        }
        if (waitRes != WAIT_OBJECT_0) { lost("wait failed", HRESULT_FROM_WIN32(GetLastError())); break; }

        hr = capture->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) { lost("device lost", hr); break; }

        while (packetFrames > 0 && m_running.load(std::memory_order_relaxed)) {
            BYTE*  data    = nullptr;
            UINT32 frames  = 0;
            DWORD  pktFlags = 0;
            hr = capture->GetBuffer(&data, &frames, &pktFlags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(hr)) {
                lost(hr == AUDCLNT_E_DEVICE_INVALIDATED ? "device invalidated" : "read failed", hr);
                break;
            }

            if ((pktFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && !firstPacket
                && discontinuities++ == 0) {
                qWarning("WASAPI: %s capture lost sound; the device reported a discontinuity",
                         m_loopback ? "loopback" : "input");
            }
            firstPacket = false;

            outBuf.clear();
            outBuf.resize(static_cast<size_t>(frames) * Channels);

            float peakL = 0.0f, peakR = 0.0f;
            const bool silent = (pktFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            if (silent) {
                std::memset(outBuf.data(), 0, outBuf.size() * sizeof(qint16));
            } else {
                // Every channel folded into the pair, so a surround device
                // keeps its centre and surrounds. See audio/StereoDownmix.h.
                const StereoPeaks peaks =
                    downmixToStereo(format, data, static_cast<int>(frames), outBuf.data());
                peakL = peaks.left;
                peakR = peaks.right;
            }

            capture->ReleaseBuffer(frames);
            packetFrames -= frames;

            // Device rate to canonical rate. Forwarding the samples untouched,
            // as this used to, played the recording at the wrong speed and
            // pitch and drifted further out of sync the longer it ran.
            // Meters read the captured signal, so they update even on a tick
            // that produces no output frames.
            emit levelsUpdated(peakL, peakR);

            const qint16* emitData = outBuf.data();
            size_t emitSamples = outBuf.size();
            if (resampler.active()) {
                resampled.clear();
                resampler.process(outBuf.data(), static_cast<int>(frames), resampled);
                emitData = resampled.data();
                emitSamples = resampled.size();
            }

            // A very small packet can leave the resampler short of a full
            // interpolation window; it keeps those frames and emits them with
            // the next packet.
            if (emitSamples > 0) {
                QByteArray chunk(reinterpret_cast<const char*>(emitData),
                                 static_cast<int>(emitSamples * sizeof(qint16)));
                emit samplesReady(std::move(chunk));
            }

            hr = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                lost("device lost", hr);
                break;
            }
        }
    }

    if (discontinuities > 1) {
        qWarning("WASAPI: %s capture lost sound %d times",
                 m_loopback ? "loopback" : "input", discontinuities);
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);

    client->Stop();
    capture->Release();
    CloseHandle(bufferReadyEvent);
    CoTaskMemFree(mixFormat);
    client->Release();
    if (comOwned) CoUninitialize();
}
