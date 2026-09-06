#pragma once

// Runs one render job as a single ffmpeg process.
//
// Unlike EncoderPipeline, which pulls live frames from capture at wall-clock
// rate, this feeds ffmpeg file inputs and a filter graph and lets it run as fast
// as the machine allows (docs/adr/0003-render-worker-pipeline.md). Progress
// comes from ffmpeg's own `-progress` stream rather than from guessing.
//
// One job at a time. RenderQueue owns the instance and reuses it.

#include "recording/RenderQueue.h"

#include <QObject>
#include <QString>

class QProcess;

class RenderPipeline : public QObject {
    Q_OBJECT
public:
    explicit RenderPipeline(QObject* parent = nullptr);
    ~RenderPipeline() override;

    bool ffmpegAvailable() const { return !m_ffmpegPath.isEmpty(); }
    bool isRunning() const { return m_proc != nullptr; }

    // Starts rendering `job`. Returns false and sets *error when the job cannot
    // start at all: no ffmpeg, an unrenderable timeline, or an output path that
    // is taken. Nothing is emitted in that case, so the caller owns the failure.
    bool start(const RenderJob& job, QString* error = nullptr);

    // Stops the run and removes the partial file. finished()/failed() are not
    // emitted: the caller asked for this and already knows.
    void cancel();

signals:
    void progress(int percent);              // 0..100, monotonic
    void finished();                         // output file written
    void failed(QString message);            // includes the tail of ffmpeg stderr

private slots:
    void onProgressOutput();
    void onProcessFinished(int exitCode);
    void onProcessError();

private:
    void cleanup();                          // drops the process and the graph file
    void removePartialOutput();

    QString   m_ffmpegPath;
    QProcess* m_proc = nullptr;
    QString   m_graphPath;                   // temp file holding the filter graph
    QString   m_outputPath;
    double    m_totalUs = 0.0;
    int       m_percent = 0;                 // last reported, never decreases
    bool      m_cancelled = false;
    QString   m_stderrTail;                  // capped; surfaced on failure
    QString   m_pendingOut;                  // partial progress line across reads
};
