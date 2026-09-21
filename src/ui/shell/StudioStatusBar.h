#pragma once

#include "capture/ICaptureSource.h"
#include "platform/MachineLoad.h"
#include "recording/FfmpegVersion.h"

#include <QElapsedTimer>
#include <QString>
#include <QWidget>

#include <functional>

class QLabel;
class QTimer;

// Persistent bottom system bar: app-state indicator on the left, machine load
// and encoder telemetry in the middle, build info on the right.
// flashMessage() shows a transient toast in place of the state chip.
//
// Every figure here is measured or blank. CPU and memory come from the OS and
// free space from the recording volume, all three verified against what Windows
// reports. Bitrate and encode rate come from ffmpeg's own progress lines by way
// of setEncodeStats(), and read as unknown until ffmpeg reports them, which it
// does not do until every one of its inputs has opened.
//
// Nothing here is ever filled in with something plausible. A bar whose job is
// to say whether the machine is coping is worse than useless when it invents
// reassuring numbers, which is what this one used to do for all five.
class StudioStatusBar : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Idle, Recording, Streaming, Rendering };

    explicit StudioStatusBar(QWidget* parent = nullptr);

    // Which state the chip shows. Showing a recording or a stream shows that
    // session's own elapsed time, so changing what is shown resets nothing.
    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    void flashMessage(const QString& text, int ms = 4000);

public slots:
    // When each output actually began and ended, from MediaController's
    // started and finished signals. Each session keeps its own clock: a
    // recording that outlives a stream started during it goes back to showing
    // the recording's time, and time spent in a save dialog or a stream key
    // prompt before ffmpeg starts is not counted.
    void markRecordingStarted();
    void markRecordingFinished();
    void markStreamingStarted();
    void markStreamingFinished();

    // Measured encoder throughput, driven from MediaController's progress
    // signals while recording or streaming. Zero means ffmpeg has not reported
    // that figure yet.
    void setEncodeStats(int bitrateKbps, int droppedFrames, int encodeFps,
                        int composedFramesRejected);

public:
    // Where the capture side's frame counts are read from.
    //
    // Polled once a second alongside CPU and disk rather than pushed, because
    // these counters move once per captured frame and nothing is served by
    // relaying sixty updates a second to a label a person reads at a glance.
    // A provider rather than a direct dependency so this widget still knows
    // nothing about what is being captured.
    void setCaptureStatsProvider(std::function<CaptureStats()> provider);

    // Names the ffmpeg this build runs, from FfmpegVersion::probe. The version
    // reads as unknown until this is called, and a result with no path says
    // that ffmpeg was not found, which is also why Record and Stream are off.
    void setFfmpegVersion(const FfmpegVersion::Result& found);

private:
    QWidget* makeStat(const QString& label, QLabel** valueOut);
    void tickStats();
    void tickClock();
    void refreshState();

    Mode m_mode = Mode::Idle;
    // Running from each session's started signal until its finished signal,
    // and invalid outside a session.
    QElapsedTimer m_recordingClock;
    QElapsedTimer m_streamingClock;

    QWidget* m_stateChip = nullptr;
    QLabel*  m_stateDot = nullptr;
    QLabel*  m_stateText = nullptr;
    QLabel*  m_message = nullptr;
    bool     m_dotBright = true;

    QLabel* m_cpu = nullptr;
    QLabel* m_ram = nullptr;
    QLabel* m_disk = nullptr;
    QLabel* m_fps = nullptr;
    QLabel* m_bitrate = nullptr;
    QWidget* m_dropStat = nullptr;   // hidden until frames are actually lost
    QLabel*  m_drops = nullptr;
    // Frames lost at the capture backend, before this application composed
    // anything. A different failure from ENC DROP with a different cause, so
    // it gets its own row rather than being folded into that one.
    QWidget* m_capDropStat = nullptr;
    QLabel*  m_capDrops = nullptr;
    std::function<CaptureStats()> m_captureStats;

    // The application's version, from the build, and ffmpeg's, from the
    // binary on PATH.
    QLabel* m_version = nullptr;

    QTimer* m_statsTimer = nullptr;
    QTimer* m_clock = nullptr;
    QTimer* m_pulse = nullptr;
    QTimer* m_messageTimer = nullptr;

    // Previous processor reading. A percentage needs two, so the first tick
    // after startup reports nothing rather than guessing.
    MachineLoad::CpuSample m_lastCpu;
};
