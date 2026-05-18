#pragma once
#include "AudioInput.h"
#include "media/TimedSource.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QQueue>
#include <QString>

class WasapiCapture;
class QTimer;

// Owns all WASAPI capture workers, exposes a single "program bus" that the
// EncoderPipeline consumes via the TimedPcmSource interface, and forwards
// per-input VU levels to the mixer UI.
//
// Inherits TimedPcmSource so it can be passed directly to
// EncoderPipeline::start() — no adapter needed. The pcmReady() signal
// (declared in TimedPcmSource) is emitted each time a mix chunk is ready.
//
// v6: real central mixer replaces per-worker pcmReady passthrough.
// Each WasapiCapture pushes to a per-input ring; a 50 Hz QTimer summons all
// rings, applies per-input volume/mute/pan, sums to int32, applies the
// optional master limiter, clamps to int16 and emits pcmReady().
class AudioController : public TimedPcmSource {
    Q_OBJECT
public:
    explicit AudioController(QObject* parent = nullptr);
    ~AudioController() override;

    // --- TimedPcmSource interface ---
    int sampleRate() const override { return 48000; }
    int channels()   const override { return 2; }

    const QList<AudioInput>& inputs() const { return m_inputs; }

    void setVolume(const QString& id, float volume);
    void setMuted(const QString& id, bool muted);
    void setPan(const QString& id, float pan);   // -1..+1

    // Master bus limiter (soft-knee brickwall at threshold).
    void setLimiterEnabled(bool enabled);
    void setLimiterThresholdDb(float db);        // default -3.0
    bool limiterEnabled()    const { return m_limiterEnabled; }
    float limiterThresholdDb() const { return m_limiterThresholdDb; }

    // v5: Dynamic per-source audio management.
    // activeDeviceIds: WASAPI device IDs that should be active capture inputs.
    // The loopback:default input is always kept regardless of this list.
    void reconcileInputs(const QStringList& activeDeviceIds);

    // Returns {deviceId, friendlyName} pairs for all active WASAPI capture
    // (eCapture) endpoints — used to populate the AudioInput source picker.
    QList<QPair<QString, QString>> enumerateInputDevices();

    // Replay buffer: enable/disable and snapshot the PCM ring.
    void setReplayBufferSeconds(int seconds);   // 0 = disable
    QQueue<TimedPcm> snapshotReplayPcm() const; // thread-safe copy

signals:
    // The mixer model changed (input added/removed/connectivity change).
    void inputsChanged();
    // Already-computed levels for the VU meter, normalized to 0..1.
    // Routed by id so the panel can address the right channel strip.
    void levelsUpdated(QString id, float peakL, float peakR);
    // Per-input connectivity (e.g. device unplugged).
    void inputConnectionChanged(QString id, bool connected);
    // pcmReady(QByteArray pcm) — inherited from TimedPcmSource.
    // Program bus output: interleaved s16le 48kHz stereo PCM. Emitted ~50 Hz.

private slots:
    void mixAndEmit();   // fired by m_mixerTimer at 50 Hz

private:
    void addLoopbackDefault();
    void startWorker(int index);
    void stopWorkerAt(int index);
    void persist(const AudioInput& in) const;
    void loadPersisted(AudioInput& in) const;
    int  indexForId(const QString& id) const;

    QList<AudioInput>     m_inputs;
    QList<WasapiCapture*> m_workers; // parallel to m_inputs, nullptr if not started

    // Per-input chunk rings: samplesReady pushes here; mixAndEmit() drains.
    // Capped at kRingMaxChunks to bound memory during pipeline stalls.
    static constexpr int kRingMaxChunks = 8;
    QHash<QString, QQueue<QByteArray>> m_rings;  // keyed by AudioInput.id

    // 50 Hz mixer timer — fires mixAndEmit() on the main thread.
    QTimer* m_mixerTimer = nullptr;

    // Master bus limiter state.
    bool  m_limiterEnabled      = false;
    float m_limiterThresholdDb  = -3.0f;
    float m_limiterGain         = 1.0f;   // current gain reduction (0..1, attacks fast / releases slowly)

    // Replay PCM ring (raw chunks, not JPEG — PCM is small).
    int              m_replaySeconds = 0;
    mutable QMutex   m_replayMutex;
    QQueue<TimedPcm> m_replayPcm;
};
