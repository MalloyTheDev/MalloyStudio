#include "RingTimedFrameSource.h"

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
}

QImage RingTimedFrameSource::currentFrame() {
    if (!m_frames.isEmpty()) {
        const ReplayFrame& rf = m_frames.front();
        QImage img;
        if (img.loadFromData(rf.jpeg, "JPEG"))
            m_last = std::move(img);
        m_frames.dequeue();
        ++m_served;

        if (m_frames.isEmpty() && !m_exhausted) {
            m_exhausted = true;
            emit exhausted();
        }
    }
    return m_last;
}
