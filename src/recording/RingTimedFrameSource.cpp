#include "RingTimedFrameSource.h"

#include <algorithm>
#include <chrono>

RingTimedFrameSource::RingTimedFrameSource(QQueue<ReplayFrame> frames,
                                           int width, int height,
                                           QObject* parent)
    : QObject(parent)
    , m_frames(std::move(frames))
    , m_width(width)
    , m_height(height)
    , m_total(m_frames.size())
{
    // Pre-fill with a black frame so currentFrame() never returns null.
    m_last = QImage(m_width, m_height, QImage::Format_ARGB32_Premultiplied);
    m_last.fill(Qt::black);

    m_nowUs = [] {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    // The replay's own timeline: where it starts, where its last frame sits,
    // and how long that last frame stays up, taken as the buffer's average
    // spacing so the clip ends one frame after its last one, not on it.
    if (!m_frames.isEmpty()) {
        m_firstPts = m_frames.front().ptsUs;
        m_lastSpan = std::max<qint64>(0, m_frames.back().ptsUs - m_firstPts);
        if (m_frames.size() > 1 && m_lastSpan > 0)
            m_holdUs = m_lastSpan / (m_frames.size() - 1);
    }
}

void RingTimedFrameSource::setClockForTesting(std::function<qint64()> nowUs) {
    QMutexLocker lock(&m_mutex);
    m_nowUs = std::move(nowUs);
}

quint64 RingTimedFrameSource::compositionSequence() const {
    QMutexLocker lock(&m_mutex);
    return m_sequence;
}

QImage RingTimedFrameSource::currentFrame() {
    QImage frame;
    bool justExhausted = false;
    {
        QMutexLocker lock(&m_mutex);
        const qint64 now = m_nowUs();
        if (m_startUs < 0) m_startUs = now;
        const qint64 elapsed = now - m_startUs;

        // Every frame whose time has come is taken off the queue; only the
        // newest of them is decoded and shown. Asking twice in the same moment
        // serves nothing new, which is the whole point.
        QByteArray due;
        bool advanced = false;
        while (!m_frames.isEmpty() && m_frames.front().ptsUs - m_firstPts <= elapsed) {
            due = m_frames.front().jpeg;
            m_frames.dequeue();
            ++m_served;
            advanced = true;
        }
        if (advanced) {
            QImage img;
            if (img.loadFromData(due, "JPEG"))
                m_last = std::move(img);
            ++m_sequence;
        }

        // Finished once the last frame has had its time, not the moment it is
        // served, so the clip's final frame is not cut to nothing.
        if (m_frames.isEmpty() && !m_exhausted && elapsed >= m_lastSpan + m_holdUs) {
            m_exhausted = true;
            justExhausted = true;
        }
        frame = m_last;
    }
    // Emitted outside the lock. A receiver may call back into this object, and
    // holding the lock across an emission is how that becomes a deadlock rather
    // than a re-entrant call.
    if (justExhausted) emit exhausted();
    return frame;
}
