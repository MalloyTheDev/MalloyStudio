#pragma once
#include "media/TimedSource.h"

#include <QQueue>

class QTimer;

// Replays a snapshot of the AudioController PCM ring through the
// TimedPcmSource interface. Used by MediaController::saveReplay().
//
// Emits pcmReady() chunks one at a time from a 50 Hz QTimer, matching the
// live audio rate. Emits finished() when the queue is drained.
//
// Nothing is taken off the queue while nobody is connected to pcmReady(). The
// encoder subscribes only once ffmpeg has opened the audio pipe, which is some
// hundreds of milliseconds after start(); chunks emitted before then were lost,
// and since ffmpeg times audio by its bytes, the clip's audio began that much
// later in the buffer than its video did.
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
