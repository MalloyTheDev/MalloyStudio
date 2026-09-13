#pragma once
#include "media/TimedSource.h"

#include <QObject>
#include <QMutex>
#include <QQueue>

// Replays a snapshot of the PreviewWidget replay ring through the
// TimedFrameSource interface. Used by MediaController::saveReplay() to
// feed the EncoderPipeline with pre-recorded JPEG-compressed frames.
//
// Each call to currentFrame() advances to the next JPEG in the queue,
// decoding it on demand. When the queue is exhausted, exhausted() is emitted
// (once) and currentFrame() continues returning the last decoded frame.
class RingTimedFrameSource : public QObject, public TimedFrameSource {
    Q_OBJECT
public:
    explicit RingTimedFrameSource(QQueue<ReplayFrame> frames,
                                  int width, int height,
                                  QObject* parent = nullptr);

    // TimedFrameSource
    // Serves the buffered frames in order, so each call consumes one and the
    // last decoded frame is repeated once they run out. That is a different
    // meaning from a live source, where the same call returns whatever is
    // currently composed and consumes nothing, and it is why a caller that
    // merely wants to look at the picture takes a frame off the replay.
    //
    // Thread-safe, as the interface requires: the encoder calls this from
    // whichever thread its tick runs on, and priming calls it once before that
    // thread exists.
    QImage currentFrame() override;
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
};
