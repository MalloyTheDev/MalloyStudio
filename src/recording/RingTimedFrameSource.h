#pragma once
#include "media/TimedSource.h"

#include <QObject>
#include <QMutex>
#include <QQueue>

#include <functional>

// Replays a snapshot of the PreviewWidget replay ring through the
// TimedFrameSource interface. Used by MediaController::saveReplay() to
// feed the EncoderPipeline with pre-recorded JPEG-compressed frames.
//
// Frames are served by their timestamps, in real time from the first call:
// currentFrame() returns the newest buffered frame whose time has come,
// decoding it on demand, however often it is asked. Once the last frame has
// had its time on screen, exhausted() is emitted (once) and currentFrame()
// goes on returning that frame.
//
// It used to hand out the next frame on every call. The buffer is filled at
// five frames a second and the encoder asks at its own rate, so a 30 second
// buffer was consumed in five seconds of ticks: the saved replay was a few
// seconds of video at several times normal speed, and ending there cut the
// real-time audio short with it.
class RingTimedFrameSource : public QObject, public TimedFrameSource {
    Q_OBJECT
public:
    explicit RingTimedFrameSource(QQueue<ReplayFrame> frames,
                                  int width, int height,
                                  QObject* parent = nullptr);

    // TimedFrameSource
    // The buffered frame due at this moment of the replay, which starts on the
    // first call. Unlike a live source this has a clock of its own, so the
    // first call, which is the encoder priming its pipe, starts the replay.
    //
    // Thread-safe, as the interface requires: the encoder calls this from
    // whichever thread its tick runs on, and priming calls it once before that
    // thread exists.
    QImage currentFrame() override;

    // Advances only when a new buffered frame is served, so an encoder that
    // follows its source does not write the same picture again on every tick.
    // Never kUnsequenced: that would tell the encoder to write every tick.
    quint64 compositionSequence() const override;

    // Replaces the real-time clock, in microseconds. For tests.
    void setClockForTesting(std::function<qint64()> nowUs);
    int nativeWidth()  const override { return m_width; }
    int nativeHeight() const override { return m_height; }

    bool isExhausted() const { QMutexLocker lock(&m_mutex); return m_exhausted; }
    int  totalFrames() const { QMutexLocker lock(&m_mutex); return m_total; }
    int  servedFrames() const { QMutexLocker lock(&m_mutex); return m_served; }

signals:
    void exhausted();   // emitted once when all frames have been served

private:
    // Guards everything below. currentFrame mutates all of it, so the
    // interface's thread-safety requirement is not satisfied by reading alone.
    mutable QMutex      m_mutex;
    QQueue<ReplayFrame> m_frames;
    QImage              m_last;   // last decoded frame (returned after exhaustion)
    int                 m_width;
    int                 m_height;
    int                 m_total   = 0;
    int                 m_served  = 0;
    bool                m_exhausted = false;

    std::function<qint64()> m_nowUs;       // microseconds, monotonic
    qint64  m_startUs  = -1;               // clock at the first call
    qint64  m_firstPts = 0;                // pts of the first buffered frame
    qint64  m_lastSpan = 0;                // last pts minus first pts
    qint64  m_holdUs   = 200000;           // how long the last frame is shown
    quint64 m_sequence = 1;                // bumped per served frame
};
