#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QTimer;

// Persistent bottom system bar (mirrors StatusBar in shell.jsx): app-state
// indicator on the left, live CPU/RAM/DISK/FPS telemetry, build info on the
// right. flashMessage() shows a transient toast in place of the state chip.
class StudioStatusBar : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Idle, Recording, Streaming, Rendering };

    explicit StudioStatusBar(QWidget* parent = nullptr);

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    void flashMessage(const QString& text, int ms = 4000);

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
    QWidget* m_bitrateStat = nullptr;
    QLabel*  m_bitrate = nullptr;

    QTimer* m_statsTimer = nullptr;
    QTimer* m_clock = nullptr;
    QTimer* m_pulse = nullptr;
    QTimer* m_messageTimer = nullptr;

    // animated values
    double m_vCpu = 24, m_vRam = 38, m_vDisk = 64, m_vFps = 60.0;
    int    m_vBitrate = 6400;
};
