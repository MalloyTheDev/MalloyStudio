#pragma once

// Runs one render job as a single ffmpeg process.
//
// Unlike EncoderPipeline, which pulls live frames from capture at wall-clock
// rate, this feeds ffmpeg file inputs and a filter graph and lets it run as fast
// as the machine allows (docs/adr/0003-render-worker-pipeline.md). Progress
// comes from ffmpeg's own `-progress` stream rather than from guessing.
//
// Before ffmpeg starts, each source file is measured with ffprobe, so that a
// clip running past the end of its source is cut there and reported rather
// than rendered with a gap nobody is told about (ADR-0001, contract 1). The
// probes run one at a time, each with a time limit, off the caller's stack:
// start() returns as soon as the job has been checked, and the encoder starts
// when the last probe has answered or been given up on.
//
// One job at a time. RenderQueue owns the instance and reuses it.

#include "recording/RenderQueue.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

class RenderPipeline : public QObject {
    Q_OBJECT
public:
    explicit RenderPipeline(QObject* parent = nullptr);
    ~RenderPipeline() override;

    bool ffmpegAvailable() const { return !m_ffmpegPath.isEmpty(); }
    // From a successful start() until finished(), failed() or cancel(),
    // including while the sources are being measured.
    bool isRunning() const { return m_proc != nullptr || m_probing; }

    // Starts rendering `job`. Returns false and sets *error when the job cannot
    // start at all: no ffmpeg, an unrenderable timeline, or an output path that
    // is taken. Nothing is emitted in that case, so the caller owns the failure.
    // A failure found after the sources are measured is reported by failed().
    bool start(const RenderJob& job, QString* error = nullptr);

    // Stops the run and removes the partial file. finished()/failed() are not
    // emitted: the caller asked for this and already knows.
    void cancel();

    // For tests: runs `program`, with `leadingArgs` before the usual ffprobe
    // arguments, in place of ffprobe, and gives each run `timeoutMs`.
    void setProbeCommandForTesting(const QString& program, const QStringList& leadingArgs,
                                   int timeoutMs);

signals:
    void progress(int percent);              // 0..100, monotonic
    // ffmpeg has started on a timeline that had to be changed to render:
    // `note` names each clip cut at the end of its source and by how much.
    // Only ever emitted after start() has returned.
    void adjusted(QString note);
    void finished();                         // output file written
    void failed(QString message);            // includes the tail of ffmpeg stderr

private slots:
    void onProgressOutput();
    void onProcessFinished(int exitCode);
    void onProcessError();

private:
    void probeNext(quint64 run);             // measure the next source, or launch
    // Builds the graph from what was measured and starts ffmpeg. *note gets
    // the report of any clip that was cut.
    bool launch(QString* error, QString* note = nullptr);
    void cleanup();                          // drops the process and the graph file
    void removePartialOutput();

    QString   m_ffmpegPath;
    QString   m_ffprobePath;                 // empty: lengths are not measured
    QStringList m_probeLeadingArgs;
    int       m_probeTimeoutMs;
    QProcess* m_proc = nullptr;
    QProcess* m_probe = nullptr;             // the single in-flight ffprobe
    bool      m_probing = false;             // started, ffmpeg not yet launched
    quint64   m_run = 0;                     // bumped per start and cancel
    RenderJob m_job;                         // what is being measured, then rendered
    QStringList m_toProbe;                   // sources still to measure
    QHash<QString, double> m_sourceSeconds;  // what was measured, by sourcePath
    QString   m_graphPath;                   // temp file holding the filter graph
    QString   m_outputPath;
    double    m_totalUs = 0.0;
    int       m_percent = 0;                 // last reported, never decreases
    bool      m_cancelled = false;
    QString   m_stderrTail;                  // capped; surfaced on failure
    QString   m_pendingOut;                  // partial progress line across reads
};
