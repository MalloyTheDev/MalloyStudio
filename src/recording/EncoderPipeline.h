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
    void errorOccurred(QString message);

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

private slots:
    void onTickVideo();
    void onMixedSamples(QByteArray pcm);
    void onFfmpegError();
    void onFfmpegFinished(int exitCode);
    void onPipeConnected();
    void onPipeConnectFailed();

private:
    void cleanup();
    bool writeAudioBytes(const QByteArray& pcm);

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
};

// ---------------------------------------------------------------------------
// RecorderPipeline — file output (MP4 / MKV). Uses the base buildFfmpegArgs().
// ---------------------------------------------------------------------------
class RecorderPipeline final : public EncoderPipeline {
    Q_OBJECT
public:
    explicit RecorderPipeline(QObject* parent = nullptr) : EncoderPipeline(parent) {}
};
