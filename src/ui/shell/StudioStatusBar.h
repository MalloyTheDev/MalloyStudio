#pragma once

#include "platform/MachineLoad.h"

#include <QString>
#include <QWidget>

class QLabel;
class QTimer;

// Persistent bottom system bar: app-state indicator on the left, machine load
// and encoder telemetry in the middle, build info on the right.
// flashMessage() shows a transient toast in place of the state chip.
//
// Every figure here is measured or blank. CPU and memory come from the OS and
// free space from the recording volume, all three verified against what Windows
// reports. Bitrate and encode rate come from ffmpeg's own progress lines by way
// of setEncodeStats(); those lines do not currently reach the application, so
// both read as unknown, which is the truthful thing to show until they do.
//
// Nothing here is ever filled in with something plausible. A bar whose job is
// to say whether the machine is coping is worse than useless when it invents
// reassuring numbers, which is what this one used to do for all five.
class StudioStatusBar : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Idle, Recording, Streaming, Rendering };

    explicit StudioStatusBar(QWidget* parent = nullptr);

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    void flashMessage(const QString& text, int ms = 4000);

public slots:
    // Measured encoder throughput, driven from MediaController's progress
    // signals while recording or streaming. Zero means ffmpeg has not reported
    // that figure yet.
    void setEncodeStats(int bitrateKbps, int droppedFrames, int encodeFps);

private:
    QWidget* makeStat(const QString& label, QLabel** valueOut);
    void tickStats();
    void tickClock();
    void refreshState();

    Mode m_mode = Mode::Idle;
    int  m_elapsed = 0;   // seconds in rec/stream

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

    QTimer* m_statsTimer = nullptr;
    QTimer* m_clock = nullptr;
    QTimer* m_pulse = nullptr;
    QTimer* m_messageTimer = nullptr;

    // Previous processor reading. A percentage needs two, so the first tick
    // after startup reports nothing rather than guessing.
    MachineLoad::CpuSample m_lastCpu;
};
