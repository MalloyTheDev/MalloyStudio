#pragma once
#include "recording/OutputSettings.h"

#include <QImage>

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>

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
    // backlogDrops is a different fact from droppedFrames and the two are kept
    // apart deliberately: droppedFrames is what ffmpeg discarded, backlogDrops
    // is what this application never handed it because ffmpeg was not keeping
    // up. They have different causes and different fixes, so summing them
    // would hide which one is happening.
    void progress(int bitrateKbps, int droppedFrames, int encodeFps, int backlogDrops);

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
                                     int* encodeFps = nullptr);

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
    // Pure: whether a sink on this cadence should write the frame it is
    // currently looking at. Exposed so the rule can be tested without a
    // running pipeline, because it is the rule and not the plumbing that
    // decides whether a recording is honest.
    static bool shouldWriteFrame(Cadence cadence, quint64 sourceSequence,
                                 quint64 lastSentSequence);

private:

    // The sequence of the last frame actually written, so FollowSource can
    // tell a new picture from the same one polled again.
    quint64 m_lastSentSequence = 0;

    // How much unwritten video may sit in QProcess's buffer before frames are
    // dropped instead of queued, counted in whole frames. Three is enough to
    // ride out scheduling jitter and a slow encoder tick without letting the
    // buffer become unbounded; at 1080p it caps the backlog near 25 MB.
    static constexpr qint64 kMaxWriteBacklogFrames = 3;

    // Frames this application never sent because the buffer was already full.
    // This is media that existed and was lost, which is what progress()
    // reports and what the status bar shows.
    int m_backlogDrops = 0;

    // Timer opportunities where the source had produced nothing new. Counted
    // separately and deliberately not reported as drops: no media existed, so
    // none was lost, and conflating the two is what made a healthy recording
    // look like it was shedding nine frames in ten.
    int m_idleTicks = 0;

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
