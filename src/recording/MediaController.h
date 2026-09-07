#pragma once
#include "recording/OutputSettings.h"
#include "recording/StreamSettings.h"
#include "media/TimedSource.h"

#include <QObject>
#include <QQueue>
#include <QString>

class RtmpKeyRelay;
class TimedFrameSource;
class TimedPcmSource;
class EncoderPipeline;

// ---------------------------------------------------------------------------
// MediaController — single facade for recording, streaming, and (future)
// replay-buffer management. MainWindow owns one MediaController and hands it
// a TimedFrameSource (PreviewWidget) + TimedPcmSource (AudioController).
//
// Both RecorderPipeline and StreamingPipeline can run simultaneously — a
// common workflow in OBS: record to disk AND stream to Twitch at the same
// time, each with independent quality settings.
// ---------------------------------------------------------------------------
class MediaController : public QObject {
    Q_OBJECT
public:
    explicit MediaController(TimedFrameSource* frames,
                             TimedPcmSource*   audio,
                             QObject*          parent = nullptr);
    ~MediaController() override;

    // --- ffmpeg availability (both pipelines share the same binary) ---
    bool ffmpegAvailable() const;

    // --- Recording ---
    bool isRecording() const;
    // Returns false (and sets *error) on synchronous failure.
    bool startRecording(const QString& path,
                        const OutputSettings& settings,
                        QString* error = nullptr);
    void stopRecording();

    // --- Replay buffer ---
    // Caller pre-snapshots the rings (PreviewWidget::snapshotReplayFrames()
    // and AudioController::snapshotReplayPcm()) and passes them in so
    // MediaController stays free of UI-layer dependencies.
    bool saveReplay(const QString&        path,
                    const OutputSettings& settings,
                    QQueue<ReplayFrame>   frames,
                    int                   canvasWidth,
                    int                   canvasHeight,
                    QQueue<TimedPcm>      audio,
                    QString*              error = nullptr);

    // --- Streaming ---
    bool isStreaming() const;
    // Returns false (and sets *error) on synchronous failure.
    bool startStreaming(const StreamSettings& stream,
                        const OutputSettings& output,
                        QString*              error = nullptr);
    void stopStreaming();

signals:
    void recordingStarted();
    void recordingFinished(QString path, qint64 bytes);

    void streamingStarted();
    void streamingFinished();
    // Live bitrate, dropped-frame counter and encode rate, parsed from
    // ffmpeg's ~1 Hz progress lines. ControlsBar shows the first two next to
    // the LIVE timer; the status bar shows the encode rate. Both pipelines
    // report, because the status bar is just as wrong sitting at a made-up
    // number while recording as while streaming. Slots may take fewer
    // arguments than the signal carries.
    void streamingProgress(int bitrateKbps, int droppedFrames, int encodeFps);
    void recordingProgress(int bitrateKbps, int droppedFrames, int encodeFps);

    void replaySaved(QString path);

    // Both pipelines surface errors here; origin = "recording" | "streaming" | "replay".
    void errorOccurred(QString origin, QString message);

private:
    TimedFrameSource* m_frames   = nullptr;
    TimedPcmSource*   m_audio    = nullptr;
    EncoderPipeline*  m_recorder = nullptr;
    EncoderPipeline*  m_streamer = nullptr;
    RtmpKeyRelay*      m_keyRelay = nullptr;   // owns the loopback publish path
};
