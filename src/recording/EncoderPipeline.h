#pragma once
#include "recording/OutputSettings.h"

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
    // running). droppedFrames is 0 if ffmpeg's line omits a `drop=` token.
    void progress(int bitrateKbps, int droppedFrames);

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
    // Parses an ffmpeg progress line (one of the ~1 Hz status lines printed to
    // stderr). Returns true on success and populates out-params; on failure
    // returns false and leaves them unchanged. Exposed for unit-testing the
    // regex without spinning up a real pipeline. droppedFrames defaults to 0
    // when the line doesn't include a `drop=` token.
    static bool tryParseProgressLine(QStringView line,
                                     int* bitrateKbps,
                                     int* droppedFrames);

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

    QString  m_ffmpegPath;
    Target   m_target;
    QString  m_audioPipeName;     // "\\\\.\\pipe\\malloy_audio_<rnd>"

    QProcess* m_ffmpeg = nullptr;
    QTimer*   m_videoTimer = nullptr;
    QTimer*   m_pipeWatchdog = nullptr;   // single-shot 5s — fails if ffmpeg never opens the audio pipe
    QThread*  m_pipeAcceptor = nullptr;

    void*             m_audioPipe = nullptr;  // HANDLE; void* to avoid windows.h in header
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
