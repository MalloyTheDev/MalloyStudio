#pragma once
#include "media/TimedSource.h"

#include <QObject>
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
    QImage currentFrame() override;
    int nativeWidth()  const override { return m_width; }
    int nativeHeight() const override { return m_height; }

    bool isExhausted() const { return m_exhausted; }
    int  totalFrames() const { return m_total; }
    int  servedFrames() const { return m_served; }

signals:
    void exhausted();   // emitted once when all frames have been served

private:
    QQueue<ReplayFrame> m_frames;
    QImage              m_last;   // last decoded frame (returned after exhaustion)
    int                 m_width;
    int                 m_height;
    int                 m_total   = 0;
    int                 m_served  = 0;
    bool                m_exhausted = false;
};
