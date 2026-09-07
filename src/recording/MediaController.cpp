#include "MediaController.h"
#include <QTimer>
#include <QPointer>
#include "recording/EncoderPipeline.h"
#include "recording/RingTimedFrameSource.h"
#include "recording/RingTimedPcmSource.h"
#include "recording/StreamingPipeline.h"
#include "recording/RtmpKeyRelay.h"

#include <QStandardPaths>

#include <utility>

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

void MediaController::setCaptureStatsProvider(std::function<CaptureStats()> provider) {
    m_captureStats = std::move(provider);
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
    m_recorder->setSourceStatsProvider(m_captureStats);

    connect(m_recorder, &EncoderPipeline::started, this, &MediaController::recordingStarted);
    connect(m_recorder, &EncoderPipeline::finished, this,
            [this](const QString& p, qint64 b) { emit recordingFinished(p, b); });
    connect(m_recorder, &EncoderPipeline::progress,
            this, &MediaController::recordingProgress);
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

namespace {
// How long to give the relay to prove it is substituting before the
// stream is stopped. The RTMP handshake and publish exchange complete
// well inside this.
constexpr int kRelayCheckMs = 5000;
}  // namespace

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
    m_streamer->setSourceStatsProvider(m_captureStats);

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

    // With the relay enabled, ffmpeg is given a loopback URL carrying a
    // placeholder, and the key is substituted in this process on the way
    // upstream, so it never appears in the child's command line.
    QString publishUrl = url;
    delete m_keyRelay;
    m_keyRelay = nullptr;
    if (stream.useKeyRelay) {
        m_keyRelay = new RtmpKeyRelay(this);
        connect(m_keyRelay, &RtmpKeyRelay::failed, this, [this](const QString& msg) {
            emit errorOccurred(QStringLiteral("streaming"), msg);
            stopStreaming();
        });
        QString relayError;
        const QString relayUrl = m_keyRelay->start(url, &relayError);
        if (relayUrl.isEmpty()) {
            // Falling back to the direct URL would silently reinstate the leak
            // the relay exists to prevent, so this stops instead.
            delete m_keyRelay;
            m_keyRelay = nullptr;
            if (error) *error = relayError;
            return false;
        }
        publishUrl = relayUrl;
    }

    EncoderPipeline::Target target;
    target.kind        = EncoderPipeline::Target::Kind::Rtmp;
    target.destination = publishUrl;
    target.output      = merged;

    const bool started = m_streamer->start(target, m_frames, m_audio, error);
    if (!started) {
        delete m_keyRelay;
        m_keyRelay = nullptr;
        return false;
    }

    if (m_keyRelay) {
        // A publishing handshake substitutes the placeholder several times
        // (releaseStream, FCPublish, publish). If none of that happened by the
        // time media should be flowing, the relay is not doing its job and the
        // stream would be published under the placeholder, so stop rather than
        // carry on in a state nobody asked for.
        QPointer<RtmpKeyRelay> relay(m_keyRelay);
        QTimer::singleShot(kRelayCheckMs, this, [this, relay] {
            if (!relay || !isStreaming()) return;
            if (relay->substitutions() > 0) return;
            emit errorOccurred(QStringLiteral("streaming"),
                               tr("The stream key relay did not engage, so the stream was "
                                  "stopped rather than published under a placeholder."));
            stopStreaming();
        });
    }
    return true;
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
