#include "AudioController.h"
#include "capture/WasapiCapture.h"

#include <QDateTime>
#include <QHash>
#include <QMutexLocker>
#include <QSet>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>

// ---------------------------------------------------------------------------
// Mixer constants
// ---------------------------------------------------------------------------

// Canonical bus format: 48kHz / stereo / s16le
static constexpr int kSampleRate  = 48000;
static constexpr int kChannels    = 2;
static constexpr int kBytesPerSample = sizeof(qint16);

// One mix tick = 20 ms = 960 frames per channel = 3840 bytes.
static constexpr int kFramesPerTick = kSampleRate / 50;   // 960
static constexpr int kBytesPerTick  = kFramesPerTick * kChannels * kBytesPerSample; // 3840

// Limiter: attack ≈ 1 tick (fast), release ≈ 60 ticks (1.2 s).
static constexpr float kLimiterAttack  = 0.0f;           // snap to target in one tick
static constexpr float kLimiterRelease = 1.0f / 60.0f;   // gain back 1/60 per tick

// Equal-power pan: gainL = cos(angle), gainR = sin(angle), angle ∈ [0, π/2].
static void panGains(float pan, float& gainL, float& gainR) {
    const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * static_cast<float>(M_PI_4);
    gainL = std::cos(angle);
    gainR = std::sin(angle);
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AudioController::AudioController(QObject* parent) : TimedPcmSource(parent) {
    // 50 Hz mixer clock.
    m_mixerTimer = new QTimer(this);
    m_mixerTimer->setInterval(20);  // 20 ms ≈ 50 Hz
    connect(m_mixerTimer, &QTimer::timeout, this, &AudioController::mixAndEmit);
    m_mixerTimer->start();

    addLoopbackDefault();

    // Each time a PCM chunk reaches the program bus, also push it to the replay ring.
    connect(this, &TimedPcmSource::pcmReady, this, [this](const QByteArray& chunk) {
        if (m_replaySeconds <= 0) return;
        const qint64 nowUs    = QDateTime::currentMSecsSinceEpoch() * 1000;
        const qint64 maxSpan  = static_cast<qint64>(m_replaySeconds) * 1'000'000;
        QMutexLocker lock(&m_replayMutex);
        m_replayPcm.enqueue({chunk, nowUs});
        while (m_replayPcm.size() > 1) {
            if (m_replayPcm.back().ptsUs - m_replayPcm.front().ptsUs <= maxSpan) break;
            m_replayPcm.dequeue();
        }
    });
}

AudioController::~AudioController() {
    m_mixerTimer->stop();
    for (WasapiCapture* w : std::as_const(m_workers)) {
        if (!w) continue;
        w->requestStop();
        w->wait(4000);
        delete w;
    }
}

// ---------------------------------------------------------------------------
// Central mixer — fires at 50 Hz
// ---------------------------------------------------------------------------

void AudioController::mixAndEmit() {
    // Always emit a chunk every tick — even when no inputs exist. The
    // recording pipeline opens a second ffmpeg input on the audio pipe
    // and ffmpeg will exit with EINVAL (-22) if it doesn't see steady
    // bytes there. We send 20 ms of digital silence in that case.
    //
    // Accumulate mixed signal in int32 to avoid clipping during summation.
    std::vector<int32_t> accum(static_cast<size_t>(kFramesPerTick * kChannels), 0);

    for (const AudioInput& in : std::as_const(m_inputs)) {
        if (in.muted) {
            m_rings[in.id].clear();  // discard buffered data while muted
            continue;
        }

        auto& ring = m_rings[in.id];
        if (ring.isEmpty()) continue;   // no data this tick → silence contribution

        // Pop one chunk (typically ~3840 bytes = 960 stereo frames).
        // If WasapiCapture emits at a different rate, we take just the first chunk.
        QByteArray chunk = ring.dequeue();
        const int nSamples = static_cast<int>(chunk.size()) / kBytesPerSample;
        const auto* src    = reinterpret_cast<const qint16*>(chunk.constData());

        float gainL, gainR;
        panGains(in.pan, gainL, gainR);
        gainL *= in.volume;
        gainR *= in.volume;

        // Interleaved stereo: [L0, R0, L1, R1, …]
        // Mix the source's stereo into the accumulator with per-channel pan gains.
        const int nFrames = nSamples / kChannels;
        for (int f = 0; f < nFrames; ++f) {
            const int fi = f * 2;
            if (fi + 1 >= static_cast<int>(accum.size())) break;
            const int32_t srcL = src[fi];
            const int32_t srcR = src[fi + 1];
            // Equal-power pan applies both L and R of the source into each output channel.
            accum[fi]     += static_cast<int32_t>((srcL + srcR) * 0.5f * gainL * 2.0f);
            accum[fi + 1] += static_cast<int32_t>((srcL + srcR) * 0.5f * gainR * 2.0f);
        }
    }

    // --- Optional master bus limiter ---
    if (m_limiterEnabled) {
        const float threshold = std::pow(10.0f, m_limiterThresholdDb / 20.0f) * 32767.0f;

        // Find peak this tick.
        float peak = 0.0f;
        for (int32_t s : accum)
            peak = std::max(peak, std::abs(static_cast<float>(s)));

        // Attack: if peak exceeds threshold, set gain to bring it down.
        if (peak > threshold && peak > 0.0f) {
            const float targetGain = threshold / peak;
            if (targetGain < m_limiterGain)
                m_limiterGain = targetGain;  // instant attack
        }

        // Apply gain reduction.
        if (m_limiterGain < 1.0f) {
            for (int32_t& s : accum)
                s = static_cast<int32_t>(s * m_limiterGain);
        }

        // Release: creep gain back toward 1.0.
        m_limiterGain = std::min(1.0f, m_limiterGain + kLimiterRelease);
    }

    // Clamp to int16 and build the output QByteArray.
    QByteArray out;
    out.resize(static_cast<qsizetype>(accum.size()) * kBytesPerSample);
    auto* dst = reinterpret_cast<qint16*>(out.data());
    for (size_t i = 0; i < accum.size(); ++i)
        dst[i] = static_cast<qint16>(std::clamp(accum[i], INT32_C(-32768), INT32_C(32767)));

    emit pcmReady(out);
}

// ---------------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------------

void AudioController::addLoopbackDefault() {
    AudioInput in;
    in.id        = QStringLiteral("loopback:default");
    in.name      = QStringLiteral("Desktop Audio");
    in.deviceId  = QString();      // default endpoint
    in.loopback  = true;
    loadPersisted(in);

    m_inputs.append(in);
    m_workers.append(nullptr);
    startWorker(m_inputs.size() - 1);
    emit inputsChanged();
}

void AudioController::startWorker(int index) {
    if (index < 0 || index >= m_inputs.size()) return;
    const AudioInput& in = m_inputs[index];

    auto* worker = new WasapiCapture(in.deviceId, in.loopback);
    m_workers[index] = worker;

    const QString id = in.id;

    connect(worker, &WasapiCapture::levelsUpdated, this,
            [this, id](float peakL, float peakR) {
        const int i = indexForId(id);
        if (i < 0) return;
        const AudioInput& ai = m_inputs[i];
        float gainL, gainR;
        panGains(ai.pan, gainL, gainR);
        const float g = ai.muted ? 0.0f : ai.volume;
        const float pL = std::min(1.0f, peakL * g * gainL);
        const float pR = std::min(1.0f, peakR * g * gainR);
        m_inputs[i].peakL = pL;
        m_inputs[i].peakR = pR;
        emit levelsUpdated(id, pL, pR);
    });

    connect(worker, &WasapiCapture::samplesReady, this,
            [this, id](QByteArray pcm) {
        // Push into the per-input ring; the mixer timer drains it at 50 Hz.
        auto& ring = m_rings[id];
        ring.enqueue(std::move(pcm));
        // Cap the ring to prevent unbounded growth if the mixer is slow.
        while (ring.size() > kRingMaxChunks)
            ring.dequeue();
    });

    connect(worker, &WasapiCapture::captureError, this,
            [this, id](const QString& /*msg*/) {
        const int i = indexForId(id);
        if (i < 0 || !m_inputs[i].connected) return;
        m_inputs[i].connected = false;
        emit inputConnectionChanged(id, false);
    });

    worker->start();
}

// ---------------------------------------------------------------------------
// Public setters
// ---------------------------------------------------------------------------

void AudioController::setVolume(const QString& id, float volume) {
    const int i = indexForId(id);
    if (i < 0) return;
    volume = std::clamp(volume, 0.0f, 1.5f);
    if (m_inputs[i].volume == volume) return;
    m_inputs[i].volume = volume;
    persist(m_inputs[i]);
}

void AudioController::setMuted(const QString& id, bool muted) {
    const int i = indexForId(id);
    if (i < 0 || m_inputs[i].muted == muted) return;
    m_inputs[i].muted = muted;
    persist(m_inputs[i]);
}

void AudioController::setPan(const QString& id, float pan) {
    const int i = indexForId(id);
    if (i < 0) return;
    pan = std::clamp(pan, -1.0f, 1.0f);
    if (m_inputs[i].pan == pan) return;
    m_inputs[i].pan = pan;
    persist(m_inputs[i]);
}

void AudioController::setLimiterEnabled(bool enabled) {
    m_limiterEnabled = enabled;
    if (!enabled) m_limiterGain = 1.0f;   // reset gain reduction
}

void AudioController::setLimiterThresholdDb(float db) {
    m_limiterThresholdDb = db;
}

// ---------------------------------------------------------------------------
// Worker management
// ---------------------------------------------------------------------------

void AudioController::stopWorkerAt(int index) {
    if (index < 0 || index >= m_workers.size()) return;
    WasapiCapture* w = m_workers[index];
    if (!w) return;
    w->requestStop();
    w->wait(4000);
    delete w;
    m_workers[index] = nullptr;
}

void AudioController::reconcileInputs(const QStringList& activeDeviceIds) {
    // Deduplicate so the same device doesn't get two workers.
    const QSet<QString> wanted(activeDeviceIds.begin(), activeDeviceIds.end());

    // Remove non-loopback inputs that are no longer needed (iterate backwards
    // so remove-by-index is safe).
    for (int i = m_inputs.size() - 1; i >= 0; --i) {
        if (m_inputs[i].loopback) continue;                     // never remove loopback:default
        if (wanted.contains(m_inputs[i].deviceId)) continue;    // still needed
        m_rings.remove(m_inputs[i].id);
        stopWorkerAt(i);
        m_inputs.removeAt(i);
        m_workers.removeAt(i);
    }

    // Determine which device IDs still need to be added.
    QStringList toAdd;
    for (const QString& deviceId : wanted) {
        bool found = false;
        for (const AudioInput& in : std::as_const(m_inputs))
            if (!in.loopback && in.deviceId == deviceId) { found = true; break; }
        if (!found) toAdd << deviceId;
    }

    if (!toAdd.isEmpty()) {
        // Build id→name map once (single COM round-trip for all new devices).
        QHash<QString, QString> nameMap;
        for (const auto& pair : enumerateInputDevices())
            nameMap[pair.first] = pair.second;

        for (const QString& deviceId : std::as_const(toAdd)) {
            AudioInput in;
            in.id       = QStringLiteral("input:") + deviceId;
            in.deviceId = deviceId;
            in.loopback = false;
            in.name     = nameMap.value(deviceId, tr("Microphone"));
            loadPersisted(in);
            m_inputs.append(in);
            m_workers.append(nullptr);
            startWorker(m_inputs.size() - 1);
        }
    }

    emit inputsChanged();
}

// ---------------------------------------------------------------------------
// Device enumeration
// ---------------------------------------------------------------------------

QList<QPair<QString, QString>> AudioController::enumerateInputDevices() {
    QList<QPair<QString, QString>> result;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return result;

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    enumerator->Release();
    if (FAILED(hr) || !collection) return result;

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device)) || !device) continue;

        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId)) || !rawId) { device->Release(); continue; }
        const QString id = QString::fromWCharArray(rawId);
        CoTaskMemFree(rawId);

        // Try to get a user-friendly name from the property store.
        QString name = id;   // fallback to raw device ID
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv))
                    && pv.vt == VT_LPWSTR && pv.pwszVal)
                name = QString::fromWCharArray(pv.pwszVal);
            PropVariantClear(&pv);
            props->Release();
        }

        result.append({id, name});
        device->Release();
    }

    collection->Release();
    return result;
}

// ---------------------------------------------------------------------------
// Replay buffer
// ---------------------------------------------------------------------------

void AudioController::setReplayBufferSeconds(int seconds) {
    m_replaySeconds = seconds;
    if (seconds <= 0) {
        QMutexLocker lock(&m_replayMutex);
        m_replayPcm.clear();
    }
}

QQueue<TimedPcm> AudioController::snapshotReplayPcm() const {
    QMutexLocker lock(&m_replayMutex);
    return m_replayPcm;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int AudioController::indexForId(const QString& id) const {
    for (int i = 0; i < m_inputs.size(); ++i)
        if (m_inputs[i].id == id) return i;
    return -1;
}

void AudioController::persist(const AudioInput& in) const {
    QSettings s;
    s.beginGroup(QStringLiteral("audio/inputs/") + in.id);
    s.setValue(QStringLiteral("volume"), in.volume);
    s.setValue(QStringLiteral("pan"),    in.pan);
    s.setValue(QStringLiteral("muted"),  in.muted);
    s.endGroup();
}

void AudioController::loadPersisted(AudioInput& in) const {
    QSettings s;
    s.beginGroup(QStringLiteral("audio/inputs/") + in.id);
    in.volume = std::clamp(s.value(QStringLiteral("volume"), 1.0f).toFloat(), 0.0f, 1.5f);
    in.pan    = std::clamp(s.value(QStringLiteral("pan"),    0.0f).toFloat(), -1.0f, 1.0f);
    in.muted  = s.value(QStringLiteral("muted"),  false).toBool();
    s.endGroup();
}
