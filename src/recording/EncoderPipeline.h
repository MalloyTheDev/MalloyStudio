#pragma once
#include "capture/ICaptureSource.h"
#include "recording/OutputSettings.h"

#include <QImage>

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>

class QProcess;
class QTimer;
class QThread;
class TimedFrameSource;
class TimedPcmSource;

// ---------------------------------------------------------------------------
// EncoderPipeline — the generalized ffmpeg-based encode/output stack.
//
// Pulls video from a TimedFrameSource (typically PreviewWidget) and PCM
// from a TimedPcmSource (typically AudioController). Spawns ffmpeg with
// args tailored to the Target::Kind (file vs RTMP).
//
// Concrete subclasses:
//   - RecorderPipeline   → writes an MP4/MKV file
//   - StreamingPipeline  → pushes to an RTMP URL via -f flv
//
// Both can run simultaneously: MediaController owns one of each, both
// subscribed to the same TimedFrameSource + TimedPcmSource.
// ---------------------------------------------------------------------------
class EncoderPipeline : public QObject {
    Q_OBJECT
public:
    struct Target {
        enum class Kind { File, Rtmp };
        Kind            kind = Kind::File;
        QString         destination;     // file path or RTMP URL
        OutputSettings  output;
    };

    explicit EncoderPipeline(QObject* parent = nullptr);
    ~EncoderPipeline() override;

    bool ffmpegAvailable() const { return !m_ffmpegPath.isEmpty(); }
    QString ffmpegPath()   const { return m_ffmpegPath; }
    bool isRunning()       const { return m_running; }

    // Returns false (and sets *error) if start fails synchronously.
    // The TimedFrameSource and TimedPcmSource pointers must outlive the
    // pipeline; the caller (MediaController) ensures that.
    bool start(const Target& target,
               TimedFrameSource* frames,
               TimedPcmSource*   audio,
               QString*          error = nullptr);

    // Where the capture side's totals are read from, for the run summary.
    //
    // The summary is only interpretable end to end: composed against accepted
    // says what this application refused to hand over, and it says nothing
    // about whether the frames existed in the first place. A run that composed
    // 900 pictures means one thing if the backend offered 900 and another if
    // it offered 3600 and lost 2700 of them.
    //
    // A callable rather than a pointer to the capture layer, because this
    // class has no business knowing what a capture backend is; it only needs
    // two numbers at the end of a run. What is reported is the difference
    // between the start and the end of the run, since the counters are
    // cumulative for as long as the application has been capturing and a
    // recording is entitled to report only its own frames.
    void setSourceStatsProvider(std::function<CaptureStats()> provider);

public slots:
    void stop();

signals:
    void started();
    void finished(QString destination, qint64 bytes);   // bytes=0 for streams
    // Error messages contain the short summary followed by "\n\nLast stderr:\n<tail>"
    // when ffmpeg's stderr tail is available. MainWindow splits this for
    // QMessageBox::setDetailedText so the wall of ffmpeg output is hidden by default.
    void errorOccurred(QString message);
    // Live progress emitted from ffmpeg's stderr progress lines (~1 Hz while
    // running). droppedFrames is 0 if ffmpeg's line omits a `drop=` token, and
    // encodeFps is 0 if it omits `fps=`. Slots may take fewer arguments.
    //
    // Three different facts, kept apart deliberately, because they have
    // different causes and different fixes and summing them hides which is
    // happening:
    //
    //   droppedFrames           what ffmpeg itself discarded
    //   composedFramesRejected  pictures this application composed and never
    //                           handed over, because the encoder input was full
    //   (backend counters)      frames a capture backend produced and lost
    //                           before composition, reported by the backend
    void progress(int bitrateKbps, int droppedFrames, int encodeFps,
                  int composedFramesRejected);

protected:
    // The audio pipe name (filled in by start()). Subclasses pass this to
    // ffmpeg via `-i <pipe>` in their input args.
    QString audioPipeName() const { return m_audioPipeName; }

    // Concrete subclasses override to build per-Target output/codec/muxer args.
    // The base class prepends the input args (rawvideo stdin + audio pipe)
    // before this is appended.
    //
    // Default implementation produces file-output args (used by RecorderPipeline):
    //   -c:v <codec> -preset <p> -crf <q> -pix_fmt yuv420p
    //   -c:a <codec> -b:a <kbps>k -shortest <destination>
    virtual QStringList buildOutputArgs(const Target& target) const;

public:
    // Removes the stream key from text that came out of ffmpeg.
    //
    // ffmpeg prints the destination URL on a failed connect, even at
    // `-loglevel error`, and that URL carries the key. The tail of stderr is
    // attached to errorOccurred() and shown in an error dialog, so without this
    // the key can be screenshotted or pasted into a bug report. Static and pure
    // so the redaction itself is testable.
    static QString redactDestination(QString text, const QString& destination);

    // Parses an ffmpeg progress line (one of the ~1 Hz status lines printed to
    // stderr). Returns true on success and populates out-params; on failure
    // returns false and leaves them unchanged. Exposed for unit-testing the
    // regex without spinning up a real pipeline. droppedFrames defaults to 0
    // when the line doesn't include a `drop=` token, and encodeFps to 0 when it
    // has no `fps=` token yet.
    static bool tryParseProgressLine(QStringView line,
                                     int* bitrateKbps,
                                     int* droppedFrames,
                                     int* encodeFps = nullptr,
                                     int* cfrDuplicates = nullptr);

private slots:
    void onTickVideo();
    void onMixedSamples(QByteArray pcm);
    void onFfmpegError();
    void onFfmpegFinished(int exitCode);
    void onPipeConnected();
    void onPipeConnectFailed();
    void onFfmpegStderrReady();

private:
    void cleanup();
    bool writeAudioBytes(const QByteArray& pcm);
    void appendStderrTail(const QString& chunk);
    void parseProgressLine(QStringView line);

    // Written to stdin whenever the frame source has nothing yet, so ffmpeg's
    // video input never starves. See onTickVideo for why that matters.
    QImage m_blackFrame;

public:
    // How this sink decides when a frame is due.
    //
    // A file follows the source: a picture that has not changed is not new
    // media, so nothing is written and the wall-clock timestamps carry the
    // gap. A stream keeps its own cadence: an ingest has negotiated a rate and
    // needs frames at that rate however seldom the screen changes, so the
    // latest picture is repeated to fill the time. The same fps number means
    // different things on the two paths, which is why they are not the same
    // decision.
    enum class Cadence { FollowSource, ConstantRate };
    Cadence m_cadence = Cadence::ConstantRate;

public:
    // Pure: whether a sink on this cadence should write the picture it is
    // currently looking at. Exposed so the rule can be tested without a
    // running pipeline, because it is the rule and not the plumbing that
    // decides whether a recording is honest.
    //
    // cadenceDue is the sink's own clock saying a frame is owed. It is a
    // separate input from the sequence on purpose: a constant-rate sink owns
    // its presentation clock, and a source producing a new picture must never
    // advance it. Today the timer only fires when a frame is due so this is
    // always true, but writing the rule this way means an event-driven tick
    // cannot quietly turn a capture event into a stream clock tick.
    static bool shouldWriteFrame(Cadence cadence, quint64 compositionSequence,
                                 quint64 lastSentSequence, bool cadenceDue);

private:

    // The composition sequence of the last frame actually written, so
    // FollowSource can tell a new picture from the same one polled again.
    quint64 m_lastSentSequence = 0;

    // How much unwritten video may sit in QProcess's buffer before frames are
    // dropped instead of queued, counted in whole frames. Three is enough to
    // ride out scheduling jitter and a slow encoder tick without letting the
    // buffer become unbounded; at 1080p it caps the backlog near 25 MB.
    static constexpr qint64 kMaxWriteBacklogFrames = 3;

    // Composed pictures that existed and were never handed to the encoder
    // because its input buffer was already full. This is media loss, and it is
    // what progress() reports and the status bar shows as ENC DROP.
    //
    // Distinct from frames a capture backend produced and lost before
    // composition, which is the backend's own counter and would read CAP DROP.
    // Keeping the two apart is what lets a future report say whether a
    // recording lost content at the source or at the encoder.
    int m_composedFramesRejected = 0;

    // The ENC ACCEPT row.
    //
    // Precisely: pictures this pipeline took responsibility for and issued a
    // write for. It is NOT a count of successfully encoded frames, and must
    // not drift into meaning that. Nothing here observes what ffmpeg did with
    // the bytes afterwards, so a frame counted as accepted can still be lost
    // downstream, and a later output-side counter can be added without
    // changing what this number has historically meant.
    //
    // It exists because a rejection count is uninterpretable on its own: ENC
    // DROP only says something next to what got through.
    int m_composedFramesAccepted = 0;

    // The CFR DUP row: frames synthesised to satisfy a constant-rate output,
    // read from ffmpeg's own `dup=` token.
    //
    // Output duplication, not encoder duplication. Depending on the filter
    // graph, frame-rate synchronisation can insert these ahead of the codec,
    // so attributing them to the encoder would name the wrong stage. What is
    // certain is that they are not media this application produced.
    //
    // ffmpeg's counter is cumulative and non-decreasing across progress lines,
    // verified against 8.1.1 (33, 62, 90, 103 ... 691 within one run), so this
    // takes the latest value and never accumulates. Summing observations would
    // over-report duplication by roughly the number of progress lines.
    int m_cfrDuplicates = 0;

    // Timer opportunities where the source had produced nothing new. Counted
    // separately and deliberately not reported as drops: no media existed, so
    // none was lost, and conflating the two is what made a healthy recording
    // look like it was shedding nine frames in ten.
    int m_idleTicks = 0;

    std::function<CaptureStats()> m_sourceStats;
    // The capture totals as they stood when this run began, so the summary can
    // report what this run captured rather than what the application has
    // captured since it launched.
    CaptureStats m_sourceStatsAtStart;

    QString  m_ffmpegPath;
    Target   m_target;
    QString  m_audioPipeName;     // "\\\\.\\pipe\\malloy_audio_<rnd>"

    QProcess* m_ffmpeg = nullptr;
    QTimer*   m_videoTimer = nullptr;
    QTimer*   m_pipeWatchdog = nullptr;   // single-shot 5s — fails if ffmpeg never opens the audio pipe
    QThread*  m_pipeAcceptor = nullptr;

    void*             m_audioPipe = nullptr;  // HANDLE; void* to avoid windows.h in header
    // Owns the blocking WriteFile on the audio pipe. Defined in the .cpp
    // because it needs windows.h. Never write to the pipe from this class's
    // own thread: see AudioPipeWriter for the deadlock that caused.
    class AudioPipeWriter* m_audioWriter = nullptr;
    TimedFrameSource* m_frames = nullptr;
    TimedPcmSource*   m_audio  = nullptr;

    bool m_running   = false;
    bool m_pipeReady = false;

    // Ring-trimmed tail of ffmpeg's stderr — included in errorOccurred()
    // messages so users see the actual diagnostic. Capped at ~4 KB.
    QString m_stderrTail;
    // Pending unterminated bytes from the stderr stream (ffmpeg uses \r for
    // in-place progress updates, so we buffer across reads to keep lines whole).
    QString m_stderrPending;
};

// ---------------------------------------------------------------------------
// RecorderPipeline — file output (MP4 / MKV). Uses the base buildFfmpegArgs().
// ---------------------------------------------------------------------------
class RecorderPipeline final : public EncoderPipeline {
    Q_OBJECT
public:
    explicit RecorderPipeline(QObject* parent = nullptr) : EncoderPipeline(parent) {}
};
