#include "RingTimedPcmSource.h"

#include <QTimer>

RingTimedPcmSource::RingTimedPcmSource(QQueue<TimedPcm> chunks, QObject* parent)
    : TimedPcmSource(parent)
    , m_chunks(std::move(chunks))
{}

void RingTimedPcmSource::start() {
    if (m_chunks.isEmpty()) {
        emit finished();
        return;
    }
    m_timer = new QTimer(this);
    m_timer->setInterval(20);   // 50 Hz — matches live audio cadence
    connect(m_timer, &QTimer::timeout, this, &RingTimedPcmSource::emitNextChunk);
    m_timer->start();
}

void RingTimedPcmSource::emitNextChunk() {
    if (m_chunks.isEmpty()) {
        m_timer->stop();
        emit finished();
        return;
    }
    emit pcmReady(m_chunks.dequeue().pcm);
}
