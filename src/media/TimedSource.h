#pragma once
#include <QByteArray>
#include <QImage>
#include <QObject>
#include <cstdint>

// ---------------------------------------------------------------------------
// TimedSource — NLE-shaped abstractions consumed by EncoderPipeline.
//
// Two pure-virtual interfaces decouple the encoder/output stack from
// whatever produces frames and audio. In v6:
//   - PreviewWidget implements TimedFrameSource (live compositor)
//   - AudioController inherits TimedPcmSource (mixed program bus)
//
// In v7 (future Vegas-style NLE):
//   - TimelineRenderer will implement both → plugs into the same
//     EncoderPipeline with zero changes to the encoder.
//
// PTS (presentation timestamp) is in microseconds since some monotonic
// origin (we use QDateTime::currentMSecsSinceEpoch * 1000). The encoder
// doesn't currently use PTS for the live path (it pulls on a fixed-rate
// timer), but the replay-buffer ring sources rely on it to drive playback.
// ---------------------------------------------------------------------------

struct TimedFrame {
    QImage image;
    qint64 ptsUs = 0;
};

struct TimedPcm {
    QByteArray pcm;    // interleaved s16le, 48 kHz stereo
    qint64     ptsUs = 0;
};

// Replay-buffer ring stores frames as JPEG so a 30-second buffer at 5fps
// stays within ~22 MB instead of the ~7 GB an uncompressed QImage would need.
struct ReplayFrame {
    QByteArray jpeg;   // JPEG-encoded canvas at quality 75
    qint64     ptsUs = 0;
};

// Frame producer. Encoder pulls currentFrame() on its render timer.
class TimedFrameSource {
public:
    virtual ~TimedFrameSource() = default;

    // The most recently composed canvas frame. Must be thread-safe; the
    // encoder may call this from a worker timer thread.
    virtual QImage currentFrame() = 0;

    // Native source dimensions — used by the encoder to size the raw video
    // stdin and the optional scale filter. The compositor canvas is 1920x1080
    // (MalloyCanvas::Width/Height); a TimelineRenderer in v7+ would also return
    // its rendering resolution here.
    virtual int nativeWidth()  const = 0;
    virtual int nativeHeight() const = 0;
};

// PCM producer. Encoder subscribes to pcmReady() and writes incoming
// chunks to the audio named pipe.
class TimedPcmSource : public QObject {
    Q_OBJECT
public:
    explicit TimedPcmSource(QObject* parent = nullptr) : QObject(parent) {}
    ~TimedPcmSource() override = default;

    virtual int sampleRate() const = 0;   // canonical bus is 48000 Hz
    virtual int channels()   const = 0;   // canonical bus is 2 (stereo)

signals:
    // Interleaved s16le PCM at sampleRate() × channels(). Emitted ~50 Hz
    // (every ~20 ms) by the live AudioController; the replay ring source
    // emits at whatever rate the encoder timer pulls.
    void pcmReady(QByteArray pcm);
};
