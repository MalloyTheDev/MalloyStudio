#include "RingTimedPcmSource.h"

#include <QMetaMethod>
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
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(20);   // 50 Hz — matches live audio cadence
    connect(m_timer, &QTimer::timeout, this, &RingTimedPcmSource::emitNextChunk);
    m_clock.start();
    m_timer->start();
}

void RingTimedPcmSource::emitNextChunk() {
    if (!isSignalConnected(QMetaMethod::fromSignal(&TimedPcmSource::pcmReady))) return;
    // Every chunk the clock says is due. Chunks held back while nobody was
    // listening are due too, so a late subscriber catches up to real time.
    const qint64 due = m_clock.elapsed() / 20;
    for (; m_emitted < due; ++m_emitted) {
        if (m_chunks.isEmpty()) break;
        emit pcmReady(m_chunks.dequeue().pcm);
    }
    if (m_chunks.isEmpty()) {
        m_timer->stop();
        emit finished();
    }
}
