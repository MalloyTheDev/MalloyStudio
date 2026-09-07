#pragma once
#include "capture/ICaptureSource.h"
#include "recording/OutputSettings.h"

#include <QElapsedTimer>
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

    // The one QImage format the video pipe accepts.
    //
    // ffmpeg is told `-pix_fmt bgra` and reads the bytes as they arrive, so the
    // frame handed over has to be in exactly that byte order. Format_ARGB32 is
    // it on a little endian machine: BGRA in memory reads back as 0xAARRGGBB.
    // Anything else would be encoded as though it were, which produces a file
    // with wrong colours rather than an error.
    //
    // Separate from the stride question below, and deliberately so: a frame can
    // be laid out correctly and still be the wrong format, and handling padded
    // rows says nothing about which formats this pipe accepts.
    static QImage::Format rawVideoFormat() { return QImage::Format_ARGB32; }

    // Whether a frame can go into a rawvideo stream as one contiguous block.
    //
    // rawvideo has no notion of stride: every row must be exactly width times
    // the pixel size, with nothing between them. QImage is allowed to pad rows
    // for alignment, and when it does, writing the buffer whole would feed
    // ffmpeg the padding as if it were picture and shear the image. Four byte
    // formats at ordinary widths are naturally tight, which is why this has
    // never been seen, and is also why it would be found the hard way.
    //
    // Pure, so the rule can be checked against a deliberately padded image
    // rather than trusted.
    static bool isTightlyPacked(const QImage& frame);

    // Whether a frame is shaped the way the pipe was declared to ffmpeg. The
    // resolution belongs in this test as much as the pixel format does: both
    // are named in the arguments, and rawvideo can neither correct nor even
    // notice a frame that disagrees with either. A frame of the right format
    // and the wrong size is written without complaint and displaces every
    // frame after it, which reads as a decoder fault rather than a writer one.
    //
    // Pure, for the same reason as the rule above.
    static bool conformsToPipeDeclaration(const QImage& frame);

    // The frame rate to declare on the rawvideo input, for a given output
    // configuration.
    //
    // rawvideo carries no rate of its own, so ffmpeg has to be told one, and
    // that number is the input's time base: arrivals closer together than
    // 1/rate land on the same timestamp and `-fps_mode vfr` discards all but
    // one of them. Left undeclared, ffmpeg assumes 25, which is why every
    // recording this application made held 25 frames a second whatever it was
    // configured for and whatever the capture side delivered. Measured: 600
    // frames in at 60 a second produced 250 out, and 597 out once a rate was
    // declared.
    //
    // The configured output rate is the right number to declare because it is
    // the rate this application's sink is clocked at, so nothing it produces
    // can legitimately arrive faster. Declaring more than that would not add
    // any frames and would cost bitrate on the stream path, where the rate
    // control divides its budget by exactly this number.
    //
    // Clamped because it also has to be a legal time base: a zero or negative
    // rate from a corrupted setting would make ffmpeg refuse the input
    // outright, and the recording would fail at start rather than record at
    // some wrong rate.
    static int declaredInputFrameRate(const OutputSettings& settings);

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
    void onVideoPipeConnected(int primedFrames);
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

    // The longest unbroken stretch in which every composed picture was
    // refused.
    //
    // A count of rejections cannot distinguish a hundred spread evenly across
    // a minute from a hundred in a row, and only the second is a hole. This is
    // pressure inside the application, not damage to the file: a burst can be
    // long while the recording stays continuous, because the frames that were
    // accepted around it may still be well spread. What reached the file is a
    // separate measurement taken from the file itself.
    //
    // Deliberately not called starvation. A refused frame means the transport
    // was full, which is the bound doing its job, and says nothing on its own
    // about what a viewer would see.
    qint64 m_longestDropBurstMs = 0;
    QElapsedTimer m_dropBurstClock;
    bool m_inDropBurst = false;

    void noteFrameRejected();
    void noteFrameAccepted();
    void closeDropBurst();

    // Frames the transport finished writing, kept because the run summary
    // is printed after the writer has already been retired.
    int m_framesPiped = 0;

    // Wall clock at the previous video tick, so the gap between what the
    // timer asked for and what it got can be recorded. That gap is how
    // starved the event loop this shares with composition and painting is.
    QElapsedTimer m_tickClock;
    std::function<CaptureStats()> m_sourceStats;
    // The capture totals as they stood when this run began, so the summary can
    // report what this run captured rather than what the application has
    // captured since it launched.
    CaptureStats m_sourceStatsAtStart;

    QString  m_ffmpegPath;
    Target   m_target;
    QString  m_audioPipeName;     // "\\\\.\\pipe\\malloy_audio_<rnd>"
    QString  m_videoPipeName;     // the same, for video

    QProcess* m_ffmpeg = nullptr;
    QTimer*   m_videoTimer = nullptr;
    QTimer*   m_pipeWatchdog = nullptr;   // single-shot 5s — fails if ffmpeg never opens the audio pipe
    QThread*  m_pipeAcceptor = nullptr;

    void*             m_audioPipe = nullptr;  // HANDLE; void* to avoid windows.h in header
    void*             m_videoPipe = nullptr;
    // Owns the blocking WriteFile on the audio pipe. Defined in the .cpp
    // because it needs windows.h. Never write to the pipe from this class's
    // own thread: see AudioPipeWriter for the deadlock that caused.
    class AudioPipeWriter* m_audioWriter = nullptr;
    // Owns the write into ffmpeg's video pipe. Video used to go through
    // QProcess to stdin, whose buffer only moves when the Qt event loop
    // runs, on the same thread that composes the frames. A congested loop
    // therefore starved ffmpeg while the application looked busy and well.
    class VideoPipeWriter* m_videoWriter = nullptr;
    QThread*  m_videoAcceptor = nullptr;
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
