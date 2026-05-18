#pragma once
#include <QByteArray>
#include <QString>
#include <QThread>
#include <atomic>

// Captures a single WASAPI endpoint on its own thread.
// - loopback=true  → captures rendered audio from the default playback endpoint
//                    (a.k.a. "desktop audio").
// - loopback=false → captures from a specific input (microphone) endpoint.
//
// All COM/WASAPI objects live entirely on the worker thread, matching the
// DxgiCapture convention.
//
// Output format is hard-pinned to 48000 Hz / 16-bit / stereo, interleaved
// little-endian (s16le). The class converts whatever the device delivers
// (typically float32) into this canonical bus format so AudioController can
// mix without per-input format negotiation.
class WasapiCapture : public QThread {
    Q_OBJECT
public:
    static constexpr int SampleRate    = 48000;
    static constexpr int Channels      = 2;
    static constexpr int BytesPerFrame = Channels * sizeof(qint16); // 4 bytes

    // deviceId empty → default endpoint of the appropriate flow.
    WasapiCapture(QString deviceId, bool loopback, QObject* parent = nullptr);
    ~WasapiCapture() override;

    void requestStop();

signals:
    // Already-computed peaks for the VU meter, normalized to 0..1.
    void levelsUpdated(float peakL, float peakR);
    // Interleaved s16le PCM at 48kHz stereo. Roughly every ~20 ms.
    void samplesReady(QByteArray pcm);
    // Permanent failures (device invalidated, format unsupported, etc.).
    void captureError(QString message);

protected:
    void run() override;

private:
    QString           m_deviceId;
    bool              m_loopback = true;
    std::atomic<bool> m_running{false};
};
