#include "MediaController.h"
#include "recording/EncoderPipeline.h"
#include "recording/RingTimedFrameSource.h"
#include "recording/RingTimedPcmSource.h"
#include "recording/StreamingPipeline.h"

#include <QStandardPaths>

MediaController::MediaController(TimedFrameSource* frames,
                                 TimedPcmSource*   audio,
                                 QObject*          parent)
    : QObject(parent)
    , m_frames(frames)
    , m_audio(audio)
{
    // Pipelines are created lazily in start*() so they survive repeated
    // start/stop cycles without leaking HANDLE resources.
}

MediaController::~MediaController() {
    if (m_recorder && m_recorder->isRunning()) m_recorder->stop();
    if (m_streamer && m_streamer->isRunning()) m_streamer->stop();
}

bool MediaController::ffmpegAvailable() const {
    return !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty();
}

bool MediaController::isRecording() const {
    return m_recorder && m_recorder->isRunning();
}

bool MediaController::isStreaming() const {
    return m_streamer && m_streamer->isRunning();
}

bool MediaController::startRecording(const QString& path,
                                      const OutputSettings& settings,
                                      QString* error) {
    if (isRecording()) {
        if (error) *error = QStringLiteral("Already recording");
        return false;
    }

    // Create a fresh RecorderPipeline each time so Windows HANDLE state is clean.
    delete m_recorder;
    m_recorder = new RecorderPipeline(this);

    connect(m_recorder, &EncoderPipeline::started, this, &MediaController::recordingStarted);
    connect(m_recorder, &EncoderPipeline::finished, this,
            [this](const QString& p, qint64 b) { emit recordingFinished(p, b); });
    connect(m_recorder, &EncoderPipeline::errorOccurred, this,
            [this](const QString& msg) { emit errorOccurred(QStringLiteral("recording"), msg); });

    EncoderPipeline::Target target;
    target.kind        = EncoderPipeline::Target::Kind::File;
    target.destination = path;
    target.output      = settings;

    return m_recorder->start(target, m_frames, m_audio, error);
}

void MediaController::stopRecording() {
    if (m_recorder) m_recorder->stop();
}

bool MediaController::startStreaming(const StreamSettings& stream,
                                      const OutputSettings& output,
                                      QString* error) {
    if (isStreaming()) {
        if (error) *error = QStringLiteral("Already streaming");
        return false;
    }

    const QString url = stream.rtmpUrl();
    if (url.isEmpty() || url.contains(QStringLiteral("{key}"))) {
        if (error) *error = QStringLiteral("Stream key is not configured");
        return false;
    }

    delete m_streamer;
    m_streamer = new StreamingPipeline(this);

    connect(m_streamer, &EncoderPipeline::started, this, &MediaController::streamingStarted);
    connect(m_streamer, &EncoderPipeline::finished, this,
            [this](const QString&, qint64) { emit streamingFinished(); });
    connect(m_streamer, &EncoderPipeline::errorOccurred, this,
            [this](const QString& msg) { emit errorOccurred(QStringLiteral("streaming"), msg); });
    connect(m_streamer, &EncoderPipeline::progress,
            this, &MediaController::streamingProgress);

    // Merge the stream-specific fields into the OutputSettings the pipeline sees.
    // bitrateKbps was already plumbed in v6; keyframeSec is new in v7 (was being
    // dropped, so StreamingPipeline always emitted a hardcoded 2s GOP).
    OutputSettings merged = output;
    merged.bitrateKbps = stream.bitrateKbps;
    merged.keyframeSec = stream.keyframeSec;

    EncoderPipeline::Target target;
    target.kind        = EncoderPipeline::Target::Kind::Rtmp;
    target.destination = url;
    target.output      = merged;

    return m_streamer->start(target, m_frames, m_audio, error);
}

void MediaController::stopStreaming() {
    if (m_streamer) m_streamer->stop();
}

bool MediaController::saveReplay(const QString&        path,
                                  const OutputSettings& settings,
                                  QQueue<ReplayFrame>   replayFrames,
                                  int                   canvasWidth,
                                  int                   canvasHeight,
                                  QQueue<TimedPcm>      replayPcm,
                                  QString*              error) {
    if (replayFrames.isEmpty()) {
        if (error)
            *error = QStringLiteral(
                "Replay buffer is empty — wait for frames to accumulate "
                "(enable the replay buffer in Output Settings first)");
        return false;
    }

    const int w = canvasWidth;
    const int h = canvasHeight;

    // Everything for this one-shot encode is parented to a temporary QObject
    // that gets deleteLater()'d once the pipeline finishes (or errors).
    auto* ctx = new QObject(this);

    auto* rFrames   = new RingTimedFrameSource(std::move(replayFrames), w, h, ctx);
    auto* rAudio    = new RingTimedPcmSource(std::move(replayPcm), ctx);
    auto* pipeline  = new RecorderPipeline(ctx);

    // When the video ring is exhausted the encode should end.  Use
    // Qt::QueuedConnection so stop() is not called recursively from inside
    // currentFrame() → exhausted().
    connect(rFrames, &RingTimedFrameSource::exhausted,
            pipeline, &EncoderPipeline::stop, Qt::QueuedConnection);

    connect(pipeline, &EncoderPipeline::finished, this,
            [this, path, ctx](const QString&, qint64) {
        emit replaySaved(path);
        ctx->deleteLater();
    });
    connect(pipeline, &EncoderPipeline::errorOccurred, this,
            [this, ctx](const QString& msg) {
        emit errorOccurred(QStringLiteral("replay"), msg);
        ctx->deleteLater();
    });

    EncoderPipeline::Target target;
    target.kind        = EncoderPipeline::Target::Kind::File;
    target.destination = path;
    target.output      = settings;

    if (!pipeline->start(target, rFrames, rAudio, error)) {
        ctx->deleteLater();
        return false;
    }

    // Start the PCM pump after the pipeline is running so the audio pipe is
    // ready to accept bytes.
    rAudio->start();
    return true;
}
