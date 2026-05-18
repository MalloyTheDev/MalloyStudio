#pragma once
#include "media/TimedSource.h"

#include <QQueue>

class QTimer;

// Replays a snapshot of the AudioController PCM ring through the
// TimedPcmSource interface. Used by MediaController::saveReplay().
//
// Emits pcmReady() chunks one at a time from a 50 Hz QTimer, matching the
// live audio rate. Emits finished() when the queue is drained.
class RingTimedPcmSource : public TimedPcmSource {
    Q_OBJECT
public:
    explicit RingTimedPcmSource(QQueue<TimedPcm> chunks, QObject* parent = nullptr);

    // TimedPcmSource
    int sampleRate() const override { return 48000; }
    int channels()   const override { return 2; }

    void start();   // begin emitting chunks; call after connecting signals

signals:
    void finished();   // all chunks emitted; pipeline should stop

private slots:
    void emitNextChunk();

private:
    QQueue<TimedPcm> m_chunks;
    QTimer*          m_timer = nullptr;
};
